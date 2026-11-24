// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/platform/platform.h>
#include "audio/devices_internal.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>

// ---- audio media plane ----

#define AUDIO_DEFAULT_RATE 48000
#define AUDIO_DEFAULT_CHANNELS 2
#define AUDIO_DEFAULT_FRAMES 960

// struct ccord_audio lives in devices_internal.h (shared with the hot-plug registry).

SDL_AudioStream *ccord_audio_open_device_stream(SDL_AudioDeviceID id, const SDL_AudioSpec *spec, int frame_samples)
{
  char prev_hint[32] = {0};
  bool had_prev = false;
  if (frame_samples > 0) {
    const char *prev = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
    if (prev) {
      had_prev = true;
      snprintf(prev_hint, sizeof prev_hint, "%s", prev);
    }
    char frames_str[32];
    snprintf(frames_str, sizeof frames_str, "%d", frame_samples);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames_str);
  }
  SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(id, spec, nullptr, nullptr);
  if (frame_samples > 0) {
    if (had_prev) SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, prev_hint);
    else SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
  }
  return stream;
}

ccord_audio *ccord_audio_create(const ccord_audio_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  ccord_audio_config c = {.sample_rate = AUDIO_DEFAULT_RATE, .channels = AUDIO_DEFAULT_CHANNELS,
                          .frame_samples = AUDIO_DEFAULT_FRAMES};
  if (cfg) {
    c = *cfg;
    if (c.sample_rate == 0) c.sample_rate = AUDIO_DEFAULT_RATE;
    if (c.channels == 0) c.channels = AUDIO_DEFAULT_CHANNELS;
    if (c.frame_samples == 0) c.frame_samples = AUDIO_DEFAULT_FRAMES;
  }
  if (c.sample_rate < 8000 || c.sample_rate > 192000) {
    ccord_error_set(err, CCORD_ERR_INVALID, "audio: sample rate out of range");
    return nullptr;
  }
  if (c.channels < 1 || c.channels > 8) {
    ccord_error_set(err, CCORD_ERR_INVALID, "audio: channel count out of range");
    return nullptr;
  }
  if (c.frame_samples < 1 || c.frame_samples > 48000) {
    ccord_error_set(err, CCORD_ERR_INVALID, "audio: frame size out of range");
    return nullptr;
  }
  ccord_audio *a = ccord_calloc(1, sizeof *a);
  if (!a) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "audio: out of memory");
    return nullptr;
  }
  a->stream_mtx = SDL_CreateMutex();
  if (!a->stream_mtx) {
    ccord_free(a);
    ccord_error_set(err, CCORD_ERR_NOMEM, "audio: mutex create failed");
    return nullptr;
  }
  a->capture = c.capture;
  a->frame_bytes = (uint64_t)c.channels * sizeof(int16_t);
  a->frame_samples = c.frame_samples;
  const char *vdrv = SDL_GetCurrentVideoDriver();
  if (vdrv && strcmp(vdrv, "dummy") == 0) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  }
  // Headless CI has no audio device. A valid inert handle keeps the media loop
  // simple: it drains silence and drops playback instead of failing to start.
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    CCORD_LOG_D("audio: SDL audio unavailable: %s", SDL_GetError());
    return a;
  }
  a->audio_init = true;
  a->spec = (SDL_AudioSpec){.format = SDL_AUDIO_S16, .channels = c.channels, .freq = c.sample_rate};
  SDL_AudioDeviceID def = c.capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  a->opened_id = ccord_audio_parse_device_id(c.capture, c.device_id);
  a->stream = ccord_audio_open_device_stream(a->opened_id, &a->spec, a->frame_samples);
  if (!a->stream && a->opened_id != def) {
    // An explicit device id may be stale (unplugged since the settings page was
    // drawn); fall back to the system default instead of going silent.
    CCORD_LOG_D("audio: device %u unavailable (%s); using default", (unsigned)a->opened_id, SDL_GetError());
    a->opened_id = def;
    a->stream = ccord_audio_open_device_stream(def, &a->spec, a->frame_samples);
  }
  if (!a->stream) {
    CCORD_LOG_D("audio: no %s device: %s", c.capture ? "capture" : "playback", SDL_GetError());
    return a;
  }
  if (!SDL_ResumeAudioStreamDevice(a->stream)) {
    CCORD_LOG_D("audio: cannot start %s stream: %s", c.capture ? "capture" : "playback", SDL_GetError());
    SDL_DestroyAudioStream(a->stream);
    a->stream = nullptr;
    return a;
  }
  a->device = SDL_GetAudioStreamDevice(a->stream);
  ccord_audio_devices_register(a);
  return a;
}

void ccord_audio_destroy(ccord_audio *a)
{
  if (!a) return;
  ccord_audio_devices_unregister(a);
  if (a->stream_mtx) SDL_LockMutex(a->stream_mtx);
  if (a->stream) {
    SDL_DestroyAudioStream(a->stream);
    a->stream = nullptr;
  }
  if (a->stream_mtx) {
    SDL_UnlockMutex(a->stream_mtx);
    SDL_DestroyMutex(a->stream_mtx);
  }
  if (a->audio_init) SDL_QuitSubSystem(SDL_INIT_AUDIO);
  ccord_free(a);
}

bool ccord_audio_available(const ccord_audio *a)
{
  if (!a) return false;
  if (a->stream_mtx) SDL_LockMutex(a->stream_mtx);
  bool ok = a->stream != nullptr;
  if (a->stream_mtx) SDL_UnlockMutex(a->stream_mtx);
  return ok;
}

size_t ccord_audio_read(ccord_audio *a, int16_t *pcm, size_t frames)
{
  if (!a || !pcm || frames == 0 || !a->capture || a->frame_bytes == 0) return 0;
  if (frames > (size_t)(INT_MAX / (int)a->frame_bytes)) return 0;
  int want = (int)(frames * (size_t)a->frame_bytes);
  int got = 0;
  if (a->stream_mtx) SDL_LockMutex(a->stream_mtx);
  if (a->stream && !a->paused) got = SDL_GetAudioStreamData(a->stream, pcm, want);
  if (a->stream_mtx) SDL_UnlockMutex(a->stream_mtx);
  if (got <= 0) return 0;
  return (size_t)got / (size_t)a->frame_bytes;
}

size_t ccord_audio_write(ccord_audio *a, const int16_t *pcm, size_t frames)
{
  if (!a || !pcm || frames == 0 || a->capture || a->frame_bytes == 0) return 0;
  if (frames > (size_t)(INT_MAX / (int)a->frame_bytes)) return 0;
  int bytes = (int)(frames * (size_t)a->frame_bytes);
  bool ok = false;
  if (a->stream_mtx) SDL_LockMutex(a->stream_mtx);
  if (a->stream && !a->paused) ok = SDL_PutAudioStreamData(a->stream, pcm, bytes);
  if (a->stream_mtx) SDL_UnlockMutex(a->stream_mtx);
  return ok ? frames : 0;
}

void ccord_audio_clear(ccord_audio *a)
{
  if (!a) return;
  if (a->stream_mtx) SDL_LockMutex(a->stream_mtx);
  if (a->stream) (void)SDL_ClearAudioStream(a->stream);
  if (a->stream_mtx) SDL_UnlockMutex(a->stream_mtx);
}

void ccord_audio_set_paused(ccord_audio *a, bool paused)
{
  if (!a) return;
  if (a->stream_mtx) SDL_LockMutex(a->stream_mtx);
  if (a->paused == paused) {
    if (a->stream_mtx) SDL_UnlockMutex(a->stream_mtx);
    return;
  }
  a->paused = paused;
  if (a->stream) {
    if (paused) {
      (void)SDL_PauseAudioStreamDevice(a->stream);
      (void)SDL_ClearAudioStream(a->stream);
    } else {
      (void)SDL_ClearAudioStream(a->stream);
      (void)SDL_ResumeAudioStreamDevice(a->stream);
    }
  }
  if (a->stream_mtx) SDL_UnlockMutex(a->stream_mtx);
}

