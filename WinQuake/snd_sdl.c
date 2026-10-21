#include <stdio.h>
#include <SDL3/SDL.h>
#include "quakedef.h"

/* One DMA ring of 4096 frames (~93 ms at 11025 Hz).  The mixer paints
 * _snd_mixahead (100 ms) ahead of the play cursor, so the ring must be large
 * enough to hold that look-ahead; a 512-frame ring made the cursor wrap onto
 * audio that was still queued and left no slack for frame hitches. */
#define SND_DMA_FRAMES 4096

static dma_t the_shm;
static int snd_inited;
static SDL_AudioStream *audio_stream = NULL;
static Uint64 audio_submitted_frames;
static unsigned char audio_submit_buffer[SND_DMA_FRAMES * 2 * 2];

extern int desired_speed;
extern int desired_bits;

qboolean SNDDMA_Init(void)
{
	SDL_AudioSpec desired, stream_input;
	snd_inited = 0;
	SDL_zero(desired);
	desired.freq = desired_speed;
	
	switch (desired_bits) {
	case 8:
		desired.format = SDL_AUDIO_U8;
		break;
	case 16:
		desired.format = SDL_AUDIO_S16;
		break;
	default:
		Con_Printf("Unknown number of audio bits: %d\n", desired_bits);
		return 0;
	}
	
	desired.channels = 2;
	audio_stream = SDL_OpenAudioDeviceStream(
		SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
		&desired,
		NULL,
		NULL
	);
	
	if (!audio_stream) {
		Con_Printf("Couldn't open SDL audio: %s\n", SDL_GetError());
		return 0;
	}

	if (!SDL_GetAudioStreamFormat(audio_stream, &stream_input, NULL)) {
		Con_Printf("Couldn't get audio stream format: %s\n", SDL_GetError());
		SDL_DestroyAudioStream(audio_stream);
		audio_stream = NULL;
		return 0;
	}
	if (stream_input.freq != desired.freq || stream_input.channels != desired.channels ||
		stream_input.format != desired.format) {
		Con_Printf("SDL audio stream input format doesn't match requested mixer format\n");
		SDL_DestroyAudioStream(audio_stream);
		audio_stream = NULL;
		return 0;
	}

	/* SDL converts the stream input to the opened device's output format. */
	if (!SDL_ResumeAudioStreamDevice(audio_stream)) {
		Con_Printf("Couldn't resume SDL audio: %s\n", SDL_GetError());
		SDL_DestroyAudioStream(audio_stream);
		audio_stream = NULL;
		return 0;
	}

	shm = &the_shm;
	shm->splitbuffer = 0;
	
	switch (desired.format) {
	case SDL_AUDIO_U8:
		shm->samplebits = 8;
		break;
	case SDL_AUDIO_S16:
		shm->samplebits = 16;
		break;
	default:
		shm->samplebits = 16;
		break;
	}
	
	shm->speed = desired.freq;
	shm->channels = desired.channels;
	shm->samples = SND_DMA_FRAMES * shm->channels;
	shm->samplepos = 0;
	shm->submission_chunk = 1;
	audio_submitted_frames = 0;
	
	int buffer_size = shm->samples * (shm->samplebits / 8);
	shm->buffer = (unsigned char*)malloc(buffer_size);
	
	if (!shm->buffer) {
		Con_Printf("Couldn't allocate audio buffer\n");
		shm->buffer = NULL;
		SDL_DestroyAudioStream(audio_stream);
		audio_stream = NULL;
		return 0;
	}
	
	if (shm->samplebits == 8)
		memset(shm->buffer, 0x80, buffer_size); /* Mixer writes unsigned 8-bit PCM. */
	else
		memset(shm->buffer, 0, buffer_size);

	snd_inited = 1;
	
	Con_Printf("Sound initialized: %d Hz, %d bits, %d channels, %d samples\n",
	           shm->speed, shm->samplebits, shm->channels, shm->samples);
	
	return 1;
}

int SNDDMA_GetDMAPos(void)
{
	int queued_bytes;
	int bytes_per_frame;
	Uint64 consumed_frames;
	Uint64 buffer_frames;

	if (!snd_inited || !audio_stream || !shm)
		return 0;

	queued_bytes = SDL_GetAudioStreamQueued(audio_stream);
	if (queued_bytes < 0)
		return shm->samplepos;

	bytes_per_frame = (shm->samplebits / 8) * shm->channels;
	buffer_frames = (Uint64)(shm->samples / shm->channels);
	if (!bytes_per_frame || !buffer_frames)
		return 0;
	consumed_frames = audio_submitted_frames - (Uint64)(queued_bytes / bytes_per_frame);
	shm->samplepos = (int)((consumed_frames % buffer_frames) * shm->channels);
	return shm->samplepos;
}

void SNDDMA_Shutdown(void)
{
	if (snd_inited)
	{
		if (audio_stream) {
			SDL_DestroyAudioStream(audio_stream);
			audio_stream = NULL;
		}
		
		if (shm && shm->buffer) {
			free(shm->buffer);
			shm->buffer = NULL;
		}
		audio_submitted_frames = 0;
		snd_inited = 0;
	}
}

void SNDDMA_Submit(void)
{
	int bytes_per_frame;
	int queued_bytes;
	int queued_frames;
	int buffer_frames;
	int available_frames;
	int frames_to_queue;
	int frame_offset;
	int first_frames;
	int chunk_bytes;
	int bytes_per_sample;
	int i;

	if (!snd_inited || !audio_stream || !shm || !shm->buffer)
		return;

	bytes_per_sample = shm->samplebits / 8;
	bytes_per_frame = bytes_per_sample * shm->channels;
	buffer_frames = shm->samples / shm->channels;
	if (bytes_per_frame <= 0 || buffer_frames <= 0 ||
		buffer_frames * bytes_per_frame > (int)sizeof(audio_submit_buffer))
		return;
	queued_bytes = SDL_GetAudioStreamQueued(audio_stream);
	if (queued_bytes < 0) {
		Con_Printf("Couldn't query SDL audio queue: %s\n", SDL_GetError());
		return;
	}

	queued_frames = queued_bytes / bytes_per_frame;
	available_frames = (int)(paintedtime - audio_submitted_frames);
	if (available_frames <= 0)
		return;

	/* Keep at most one ring-buffer's worth queued. Copy the freshly mixed
	 * ring segment to contiguous storage before SDL takes ownership. */
	frames_to_queue = buffer_frames - queued_frames;
	if (frames_to_queue <= 0)
		return;
	if (frames_to_queue > available_frames)
		frames_to_queue = available_frames;

	frame_offset = (int)(audio_submitted_frames % (Uint64)buffer_frames);
	first_frames = buffer_frames - frame_offset;
	if (first_frames > frames_to_queue)
		first_frames = frames_to_queue;
	chunk_bytes = frames_to_queue * bytes_per_frame;
	if (bytes_per_sample == 2) {
		memcpy(audio_submit_buffer,
			(unsigned char *)shm->buffer + frame_offset * bytes_per_frame,
			first_frames * bytes_per_frame);
		if (first_frames < frames_to_queue)
			memcpy(audio_submit_buffer + first_frames * bytes_per_frame,
				shm->buffer, (frames_to_queue - first_frames) * bytes_per_frame);
	} else {
		for (i = 0; i < first_frames * shm->channels; i++)
			audio_submit_buffer[i] = ((unsigned char *)shm->buffer)[frame_offset * shm->channels + i];
		for (i = first_frames * shm->channels; i < frames_to_queue * shm->channels; i++)
			audio_submit_buffer[i] = ((unsigned char *)shm->buffer)[i - first_frames * shm->channels];
	}

	if (!SDL_PutAudioStreamData(audio_stream, audio_submit_buffer, chunk_bytes)) {
		Con_Printf("Couldn't queue SDL audio: %s\n", SDL_GetError());
		return;
	}
	audio_submitted_frames += frames_to_queue;
}

