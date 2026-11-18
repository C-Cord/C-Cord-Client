// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// M0-6: dedicated voice audio pthread. Owns the SDL capture/playback streams and moves PCM
// between them and the session (push/pull) off the UI loop. The caller only publishes
// mute/deafen/volume controls (atomics) and stops+joins the thread before tearing down the
// session or SDL. SDL device open/read/write/close all happen on the audio thread itself.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>

// PCM transport hooks. `frames` counts 48 kHz stereo S16 frames (2 samples each).
// The device ids select the capture/playback hardware (decimal SDL ids from ccord_audio_devices_list;
// "" = system default). The coordinator resolves settings + hot-plug fallback before starting the
// thread and restarts it when the effective ids change.
#define CCORD_VOICE_DEVICE_ID_CAP 64
typedef struct ccord_voice_audio_io {
  ccord_status (*push)(void *ud, const int16_t *pcm, size_t frames, ccord_error *err);
  size_t (*pull)(void *ud, int16_t *pcm, size_t max_frames);
  void *ud;
  char input_device_id[CCORD_VOICE_DEVICE_ID_CAP];
  char output_device_id[CCORD_VOICE_DEVICE_ID_CAP];
} ccord_voice_audio_io;

typedef struct ccord_voice_audio_thread ccord_voice_audio_thread;

// Starts the audio pthread. Both io callbacks are required. Returns null and sets err on failure.
[[nodiscard]] ccord_voice_audio_thread *ccord_voice_audio_thread_start(const ccord_voice_audio_io *io,
                                                                       ccord_error *err);
// Publishes mic mute / speaker deafen / input & output gain (percent, clamped to 0..200).
// Safe to call from any thread at any time; null is a no-op.
void ccord_voice_audio_thread_set_controls(ccord_voice_audio_thread *t, bool muted, bool deafened, int in_vol,
                                           int out_vol);
// Signals stop, joins the pthread (devices are closed on the audio thread before it exits),
// then frees. Null-safe.
void ccord_voice_audio_thread_stop(ccord_voice_audio_thread *t);
