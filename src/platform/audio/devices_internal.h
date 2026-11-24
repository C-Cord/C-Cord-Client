// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Internal glue between platform.c (audio media plane), audio_devices.c (enumeration,
// device-id parsing, hot-plug registry) and camera.c. Not part of the public API.
// The media plane and the hot-plug pump both run on the main thread (see the
// ccord_audio_set_hotplug_callback contract in platform.h).
#include <SDL3/SDL.h>

#include <ccord/platform/platform.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Audio handle. `device` is the logical SDL device id of the open stream (0 when
// inert); SDL assigns a fresh logical id per open, distinct from the physical id.
// `opened_id` is the id passed to SDL_OpenAudioDeviceStream (a physical id or a
// SDL_AUDIO_DEVICE_DEFAULT_* constant). On removal SDL emits events for both the
// logical and the physical id, so the hot-plug matcher accepts either.
struct ccord_audio {
  SDL_Mutex *stream_mtx;            // guards `stream`/`device`/`opened_id` between audio thread & main-thread hot-plug
  SDL_AudioStream *stream;
  SDL_AudioDeviceID device;
  SDL_AudioDeviceID opened_id;
  SDL_AudioSpec spec;               // requested format, kept for the fallback reopen
  uint64_t frame_bytes;
  int frame_samples;                // requested device burst size in frames (e.g. 960 = 20 ms at 48 kHz)
  bool capture;
  bool audio_init;
  bool paused;
  struct ccord_audio *hotplug_next; // registry link owned by audio_devices.c
};

// Strict decimal id parse: non-empty, digits only, non-zero, fits uint32.
bool ccord_devices_parse_id(const char *s, uint32_t *out);
// Decimal ccord_audio_config.device_id -> SDL device id; nullptr/""/malformed -> the
// direction's SDL_AUDIO_DEVICE_DEFAULT_* constant.
SDL_AudioDeviceID ccord_audio_parse_device_id(bool capture, const char *id);
// UTF-8-safe truncating copy; always NUL-terminates when cap > 0 (src may be nullptr).
void ccord_devices_copy_utf8(char *dst, size_t cap, const char *src);

// Opens an SDL audio device stream with SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES applied and restored.
SDL_AudioStream *ccord_audio_open_device_stream(SDL_AudioDeviceID id, const SDL_AudioSpec *spec, int frame_samples);

// Hot-plug registry of live audio handles.
void ccord_audio_devices_register(ccord_audio *a);
void ccord_audio_devices_unregister(ccord_audio *a);
// Consumes SDL_EVENT_AUDIO_DEVICE_ADDED/REMOVED: reopens streams whose device was
// removed on the system default, then fires the hot-plug callback. Returns true when
// the event was consumed. Called from ccord_platform_next_event on the main thread.
bool ccord_audio_devices_handle_event(const SDL_Event *e);
