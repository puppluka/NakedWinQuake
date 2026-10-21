#include "quakedef.h"
#include <SDL3/SDL.h>

// Include stb_vorbis implementation
#include "stb_vorbis.c"

#if (defined(_WIN32) || defined(WIN32))
    #define strcasecmp _stricmp
    #define strncasecmp _strnicmp
#endif

static qboolean cd_initialized = false;
static qboolean cd_playing = false;
static qboolean cd_looping = false;
static byte cd_current_track = 0;

// SDL3 Audio Stream and state
static SDL_AudioStream* cd_stream = NULL;
static stb_vorbis* ogg_decoder = NULL;
static stb_vorbis_info ogg_info;

void CDAudio_Stop(void);

void CDAudio_Console_f(void){
    if(Cmd_Argc() < 2){
        Con_Printf("Usage: cd <command> [args]\n");
        Con_Printf("Commands:\n");
        Con_Printf("  play <track> [loop] - Play the specified track (1-99), optionally looping\n");
        Con_Printf("  stop   - Stop playback\n");
        Con_Printf("  pause  - Pause playback\n");
        Con_Printf("  resume - Resume playback\n");
        return;
	}

	const char* subcmd = Cmd_Argv(1);

    if(!strcasecmp(subcmd, "play")){
        if(Cmd_Argc() < 3){
            Con_Printf("Usage: cd play <track> [loop]\n");
            return;
        }
        int track = atoi(Cmd_Argv(2));
        if(track < 1 || track > 99){
            Con_Printf("Track number must be between 1 and 99.\n");
            return;
        }
        qboolean loop = (Cmd_Argc() >= 4 && !strcasecmp(Cmd_Argv(3), "loop"));
        CDAudio_Play((byte)track, loop);
    } else if(!strcasecmp(subcmd, "stop")){
        CDAudio_Stop();
    } else if(!strcasecmp(subcmd, "pause")){
        CDAudio_Pause();
    } else if(!strcasecmp(subcmd, "resume")){
        CDAudio_Resume();
    } else if(!strcasecmp(subcmd, "info")){
        if (cd_playing){
            Con_Printf("Currently playing track %02d%s\n", cd_current_track, cd_looping ? " (looping)" : "");
        } else {
            Con_Printf("No CD track is currently playing.\n");
		}
    } else {
        Con_Printf("Unknown CD subcommand: %s\n", subcmd);
	}
}

int CDAudio_Init(void)
{
    cd_initialized = true;
    // In SDL3, SDL_InitSubSystem returns true on success, false on failure
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        Con_Printf("CDAudio: Failed to initialize SDL3 audio: %s\n", SDL_GetError());
        cd_initialized = false;
        return 0;
    }
	Cmd_AddCommand("cd", CDAudio_Console_f);    // CDAudio control from the console
    Con_Printf("CDAudio: SDL3 + stb_vorbis OGG audio initialized.\n");
    return 1;
}

void CDAudio_Shutdown(void)
{
    if (!cd_initialized) return;
    CDAudio_Stop();
    cd_initialized = false;
}

static qboolean FindMusicFile(byte track, char* out_path, size_t path_size)
{
    const char* dirs[2];
    int num_dirs = 0;

    if (com_gamedir && com_gamedir[0]) {
        dirs[num_dirs++] = com_gamedir;
    }

    if (!com_gamedir || strcmp(com_gamedir, "id1") != 0) {
        dirs[num_dirs++] = "id1";
    }

    for (int d = 0; d < num_dirs; d++) {
        snprintf(out_path, path_size, "%s/music/track%02d.ogg", dirs[d], track);
        FILE* f = fopen(out_path, "rb");
        if (f) {
            fclose(f);
            return true;
        }
    }
    return false;
}

void CDAudio_Play(byte track, qboolean looping)
{
    char filepath[MAX_OSPATH];
    int error = 0;

    if (!cd_initialized) return;

    if (track == cd_current_track && cd_playing) {
        CDAudio_Stop();
    }

    if (!FindMusicFile(track, filepath, sizeof(filepath))) {
        Con_DPrintf("CDAudio: Could not find track %02d.ogg\n", track);
        return;
    }

    // Stop any existing playback and clean up stream/decoder
    CDAudio_Stop();

    // Open OGG file using stb_vorbis
    ogg_decoder = stb_vorbis_open_filename(filepath, &error, NULL);
    if (!ogg_decoder) {
        Con_Printf("CDAudio: Failed to decode %s (error %d)\n", filepath, error);
        return;
    }

    ogg_info = stb_vorbis_get_info(ogg_decoder);

    // Set up SDL3 Audio Spec matching the OGG format
    SDL_AudioSpec src_spec;
    SDL_zero(src_spec);
    src_spec.format = SDL_AUDIO_S16LE; // stb_vorbis outputs signed 16-bit PCM
    src_spec.channels = ogg_info.channels;
    src_spec.freq = ogg_info.sample_rate;

    // Use SDL_OpenAudioDeviceStream to open the device, create the stream, and bind them all at once
    cd_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &src_spec, NULL, NULL);
    if (!cd_stream) {
        Con_Printf("CDAudio: Failed to open audio device stream: %s\n", SDL_GetError());
        stb_vorbis_close(ogg_decoder);
        ogg_decoder = NULL;
        return;
    }

    // Devices opened via SDL_OpenAudioDeviceStream start in a PAUSED state; unpause to allow audio flow
    SDL_ResumeAudioStreamDevice(cd_stream);

    cd_playing = true;
    cd_looping = looping;
    cd_current_track = track;
}

void CDAudio_Stop(void)
{
    if (cd_stream) {
        // Destroying an audio stream created via SDL_OpenAudioDeviceStream automatically 
        // unbinds and closes its associated logical audio device
        SDL_DestroyAudioStream(cd_stream);
        cd_stream = NULL;
    }

    if (ogg_decoder) {
        stb_vorbis_close(ogg_decoder);
        ogg_decoder = NULL;
    }

    cd_playing = false;
    cd_current_track = 0;
}

void CDAudio_Pause(void)
{
    if (cd_stream) {
        SDL_PauseAudioStreamDevice(cd_stream);
    }
}

void CDAudio_Resume(void)
{
    if (cd_stream) {
        SDL_ResumeAudioStreamDevice(cd_stream);
    }
}

// Call this once per frame (e.g., inside host_frame or CDAudio_Update) to pump PCM samples into SDL3
void CDAudio_Update(void)
{
    if (!cd_playing || !ogg_decoder || !cd_stream) return;

    // Keep around 1/2 second (0.5s) worth of audio queued in SDL3 buffer
    int target_queued_bytes = (ogg_info.sample_rate * ogg_info.channels * sizeof(short)) / 2;

    while (SDL_GetAudioStreamQueued(cd_stream) < target_queued_bytes) {
        short pcm_buffer[4096];
        int samples_per_channel = sizeof(pcm_buffer) / (ogg_info.channels * sizeof(short));

        // Read decoded PCM samples from stb_vorbis
        int samples_read = stb_vorbis_get_samples_short_interleaved(
            ogg_decoder,
            ogg_info.channels,
            pcm_buffer,
            samples_per_channel * ogg_info.channels
        );

        if (samples_read > 0) {
            int bytes_read = samples_read * ogg_info.channels * sizeof(short);
            SDL_PutAudioStreamData(cd_stream, pcm_buffer, bytes_read);
        }
        else {
            // End of track reached
            if (cd_looping) {
                // Rewind to beginning of OGG stream
                stb_vorbis_seek_start(ogg_decoder);
            }
            else {
                CDAudio_Stop();
                break;
            }
        }
    }
}