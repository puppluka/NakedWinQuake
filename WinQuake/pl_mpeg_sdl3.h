// SDL3 playback helpers for the bundled pl_mpeg decoder.
#ifndef PL_MPEG_SDL3_H
#define PL_MPEG_SDL3_H

#include <stddef.h>
#include <stdint.h>
#include <SDL3/SDL.h>

#include "pl_mpeg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SDL_Texture *video_texture;
    SDL_AudioStream *audio_stream;
    SDL_AudioDeviceID audio_device;
    int audio_failed;
} plm_sdl3_context_t;

int PLM_PlayVideo(const char *filename, SDL_Renderer *renderer,
    SDL_AudioDeviceID audio_device);
int PLM_PlayVideoMemory(const unsigned char *data, size_t length,
    SDL_Renderer *renderer, SDL_AudioDeviceID audio_device);
void PLM_PlayStartupVideo(const char *filename, SDL_Renderer *renderer,
    SDL_AudioDeviceID audio_device);

#ifdef __cplusplus
}
#endif

#ifdef PL_MPEG_SDL3_IMPLEMENTATION

static void PLM_SDL3VideoCallback(plm_t *plm, plm_frame_t *frame, void *user)
{
    plm_sdl3_context_t *context = (plm_sdl3_context_t *)user;
    if (!SDL_UpdateYUVTexture(context->video_texture, NULL,
            frame->y.data, (int)frame->y.width,
            frame->cb.data, (int)frame->cb.width,
            frame->cr.data, (int)frame->cr.width))
        SDL_Log("PLM: couldn't update video frame: %s", SDL_GetError());
}

static void PLM_SDL3AudioCallback(plm_t *plm, plm_samples_t *samples, void *user)
{
    plm_sdl3_context_t *context = (plm_sdl3_context_t *)user;
    if (!context->audio_stream)
        return;

    if (!SDL_PutAudioStreamData(context->audio_stream, samples->interleaved,
            (int)(samples->count * 2 * sizeof(float))))
        SDL_Log("PLM: couldn't queue audio: %s", SDL_GetError());
}

/*
 * Opens the video's audio stream before playback starts.  Opening it lazily
 * from the decode callback is fragile: failure retries on every callback and
 * the stream format is never validated.  The stream is opened on the same
 * device the engine already uses, and the lead time is derived from that
 * device's real buffer size rather than from an arbitrary endpoint.
 */
static int PLM_SDL3OpenAudio(plm_t *plm, plm_sdl3_context_t *context)
{
    SDL_AudioSpec spec;
    SDL_AudioSpec device_format;
    int device_samples = 0;
    int samplerate = plm_get_samplerate(plm);

    context->audio_stream = NULL;
    context->audio_failed = 0;

    if (plm_get_num_audio_streams(plm) < 1 || samplerate <= 0) {
        plm_set_audio_enabled(plm, 0);
        return 0;
    }

    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = samplerate;

    context->audio_stream = SDL_OpenAudioDeviceStream(
        context->audio_device, &spec, NULL, NULL);
    if (!context->audio_stream) {
        SDL_Log("PLM: couldn't open audio stream (%s); playing video without audio",
            SDL_GetError());
        context->audio_failed = 1;
        plm_set_audio_enabled(plm, 0);
        return 0;
    }

    if (!SDL_ResumeAudioStreamDevice(context->audio_stream)) {
        SDL_Log("PLM: couldn't resume audio stream: %s", SDL_GetError());
        SDL_DestroyAudioStream(context->audio_stream);
        context->audio_stream = NULL;
        context->audio_failed = 1;
        plm_set_audio_enabled(plm, 0);
        return 0;
    }

    {
        SDL_AudioDeviceID stream_device = SDL_GetAudioStreamDevice(context->audio_stream);
        if (stream_device && SDL_GetAudioDeviceFormat(stream_device, &device_format,
                &device_samples) && device_samples > 0 && device_format.freq > 0)
            plm_set_audio_lead_time(plm,
                (double)device_samples / (double)device_format.freq);
        else
            plm_set_audio_lead_time(plm, 0.05);
    }

    plm_set_audio_decode_callback(plm, PLM_SDL3AudioCallback, context);
    return 1;
}

/*
 * Waits (briefly) for already-queued audio to be consumed.  The previous
 * implementation spun forever when the endpoint stopped consuming, which hung
 * the whole engine after the clip ended.  This version is bounded and keeps
 * the window responsive while waiting.
 */
static void PLM_SDL3DrainAudio(plm_sdl3_context_t *context, int samplerate)
{
    Uint64 deadline;
    int queued;
    double seconds;

    if (!context->audio_stream)
        return;

    if (samplerate <= 0)
        samplerate = 48000;

    queued = (int)SDL_GetAudioStreamQueued(context->audio_stream);
    seconds = (double)queued / ((double)samplerate * 2.0 * (double)sizeof(float));
    if (seconds > 2.0)
        seconds = 2.0;
    if (seconds < 0.0)
        seconds = 0.0;

    deadline = SDL_GetTicksNS() + (Uint64)(seconds * 1000000000.0) + 250000000ULL;
    while (SDL_GetAudioStreamQueued(context->audio_stream) > 0 &&
           SDL_GetTicksNS() < deadline) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                SDL_ClearAudioStream(context->audio_stream);
                return;
            }
        }
        SDL_Delay(5);
    }
}

static int PLM_SDL3GetHeaders(plm_t *plm, int *width, int *height)
{
    /* plm_create_with_filename/memory initialize decoders and scan for headers.
     * plm_probe() would rewind only seekable inputs and can skip bytes for
     * forward-only buffers, so don't use it here. */
    if (!plm_has_headers(plm) || plm_get_num_video_streams(plm) < 1)
        return 0;

    *width = plm_get_width(plm);
    *height = plm_get_height(plm);
    return *width > 0 && *height > 0 && *width <= 8192 && *height <= 8192;
}

static int PLM_SDL3Play(plm_t *plm, SDL_Renderer *renderer,
    SDL_AudioDeviceID audio_device, int startup_video)
{
    int width, height, old_width = 0, old_height = 0;
    SDL_RendererLogicalPresentation old_presentation = SDL_LOGICAL_PRESENTATION_DISABLED;
    int playing = 1, skipped = 0, quit = 0;
    int samplerate;
    double duration;
    Uint64 start_time, deadline;
    SDL_Event event;
    SDL_Texture *video_texture;
    plm_sdl3_context_t context;

    if (!plm || !renderer) {
        if (plm)
            plm_destroy(plm);
        SDL_Log("PLM: invalid player or renderer");
        return 0;
    }
    SDL_GetRenderLogicalPresentation(renderer, &old_width, &old_height,
        &old_presentation);
    if (!PLM_SDL3GetHeaders(plm, &width, &height)) {
        SDL_Log("PLM: no usable MPEG-1 video stream or invalid dimensions");
        plm_destroy(plm);
        return 0;
    }

    video_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
        SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!video_texture) {
        SDL_Log("PLM: couldn't create video texture: %s", SDL_GetError());
        plm_destroy(plm);
        return 0;
    }
    if (!SDL_SetTextureScaleMode(video_texture, SDL_SCALEMODE_NEAREST) ||
        !SDL_SetRenderLogicalPresentation(renderer, width, height,
            SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        SDL_Log("PLM: couldn't configure video presentation: %s", SDL_GetError());
        SDL_DestroyTexture(video_texture);
        plm_destroy(plm);
        SDL_SetRenderLogicalPresentation(renderer, old_width, old_height,
            old_presentation);
        return 0;
    }

    SDL_zero(context);
    context.video_texture = video_texture;
    context.audio_device = audio_device;
    PLM_SDL3OpenAudio(plm, &context);
    plm_set_video_decode_callback(plm, PLM_SDL3VideoCallback, &context);

    samplerate = plm_get_samplerate(plm);
    duration = plm_get_duration(plm);
    if (!(duration > 0.0) || duration > 3600.0)
        duration = 0.0;

    while (SDL_PollEvent(&event)) { /* Discard input queued before playback. */ }
    start_time = SDL_GetTicksNS();
    /* Hard stop: the clip length plus a generous margin, so a stuck demuxer
     * can never trap the engine inside the player. */
    deadline = start_time + (Uint64)(((duration > 0.0 ? duration : 300.0) + 30.0)
        * 1000000000.0);

    while (playing) {
        Uint64 now;
        double delta;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                playing = 0;
                quit = 1;
            } else if ((event.type == SDL_EVENT_KEY_DOWN &&
                    (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_SPACE ||
                     event.key.key == SDLK_RETURN ||
                     event.key.key == SDLK_KP_ENTER)) ||
                    event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                playing = 0;
                skipped = 1;
            }
        }
        if (!playing)
            break;

        now = SDL_GetTicksNS();
        delta = (double)(now - start_time) / 1000000000.0;
        start_time = now;
        if (delta > 0.1)
            delta = 0.1;
        if (!plm_has_ended(plm))
            plm_decode(plm, delta);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (!SDL_RenderTexture(renderer, video_texture, NULL, NULL))
            SDL_Log("PLM: couldn't render frame: %s", SDL_GetError());
        SDL_RenderPresent(renderer);

        if (plm_has_ended(plm))
            playing = 0;
        if (SDL_GetTicksNS() >= deadline) {
            SDL_Log("PLM: stopping playback after reaching the time limit");
            playing = 0;
        }
        SDL_Delay(1);
    }

    if (context.audio_stream) {
        /* Let queued audio drain on natural EOF, but discard it on skip/quit. */
        if (skipped || quit)
            SDL_ClearAudioStream(context.audio_stream);
        else
            PLM_SDL3DrainAudio(&context, samplerate);
        SDL_DestroyAudioStream(context.audio_stream);
        context.audio_stream = NULL;
    }
    SDL_DestroyTexture(video_texture);
    plm_destroy(plm);
    SDL_SetRenderLogicalPresentation(renderer, old_width, old_height,
        old_presentation);
    (void)startup_video;
    return !quit && !skipped;
}

int PLM_PlayVideo(const char *filename, SDL_Renderer *renderer,
    SDL_AudioDeviceID audio_device)
{
    plm_t *plm = plm_create_with_filename(filename);
    if (!plm)
        SDL_Log("PLM: couldn't open video file %s", filename ? filename : "(null)");
    return PLM_SDL3Play(plm, renderer, audio_device, 0);
}

int PLM_PlayVideoMemory(const unsigned char *data, size_t length,
    SDL_Renderer *renderer, SDL_AudioDeviceID audio_device)
{
    plm_t *plm = data && length ? plm_create_with_memory((uint8_t *)data, length, 0) : NULL;
    return PLM_SDL3Play(plm, renderer, audio_device, 0);
}

void PLM_PlayStartupVideo(const char *filename, SDL_Renderer *renderer,
    SDL_AudioDeviceID audio_device)
{
    PLM_PlayVideo(filename, renderer, audio_device);
}

#endif /* PL_MPEG_SDL3_IMPLEMENTATION */

#endif /* PL_MPEG_SDL3_H */
