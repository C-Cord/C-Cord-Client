// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M0-6: dedicated voice audio pthread. See voice_audio_thread.h. All SDL device open/read/
// write/close happens on this thread; the UI loop only publishes controls and stops+joins.
#define _POSIX_C_SOURCE 200809L
#include "voice_audio_thread.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/platform/platform.h>

#define AUDIO_FRAME_SAMPLES 960u          // 20 ms at 48 kHz, matches the gateway frame size
#define AUDIO_SLEEP_NS (10L * 1000L * 1000L)  // ~10 ms poll cadence

struct ccord_voice_audio_thread {
  pthread_t thread;
  ccord_voice_audio_io io;
  atomic_bool stop;
  atomic_bool muted;
  atomic_bool deafened;
  atomic_int in_vol;
  atomic_int out_vol;
};

static int clamp_vol(int v)
{
  if (v < 0) return 0;
  if (v > 200) return 200;
  return v;
}

// Scales `samples` interleaved S16 values by pct/100 with saturation, in place.
static void apply_gain(int16_t *pcm, size_t samples, int pct)
{
  if (pct == 100) return;
  for (size_t i = 0; i < samples; i++) {
    int32_t v = ((int32_t)pcm[i] * pct) / 100;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    pcm[i] = (int16_t)v;
  }
}

static void *audio_thread_main(void *arg)
{
  ccord_voice_audio_thread *t = arg;
  ccord_error err;
  // Device selection: the coordinator resolved settings + hot-plug fallback into these ids before
  // starting the thread; "" opens the system default.
  ccord_audio_config ccfg = {.sample_rate = 48000, .channels = 2, .frame_samples = (int)AUDIO_FRAME_SAMPLES,
                             .capture = true,
                             .device_id = t->io.input_device_id[0] ? t->io.input_device_id : nullptr};
  ccord_audio *cap = ccord_audio_create(&ccfg, &err);
  if (!cap && ccfg.device_id != nullptr) {
    ccfg.device_id = nullptr;
    cap = ccord_audio_create(&ccfg, &err);
  }
  ccord_audio_config pcfg = {.sample_rate = 48000, .channels = 2, .frame_samples = (int)AUDIO_FRAME_SAMPLES,
                             .capture = false,
                             .device_id = t->io.output_device_id[0] ? t->io.output_device_id : nullptr};
  ccord_audio *play = ccord_audio_create(&pcfg, &err);
  if (!play && pcfg.device_id != nullptr) {
    pcfg.device_id = nullptr;
    play = ccord_audio_create(&pcfg, &err);
  }

  int16_t pcm[AUDIO_FRAME_SAMPLES * 2];
  bool was_play_gated = false;
  while (!atomic_load(&t->stop)) {
    bool muted = atomic_load(&t->muted);
    bool deafened = atomic_load(&t->deafened);
    int in_vol = atomic_load(&t->in_vol);
    int out_vol = atomic_load(&t->out_vol);

    // Mic: skip the capture read entirely while muted or input gain is zero (matches the old
    // inline loop). Only push when SDL actually returned frames.
    if (cap && !muted && in_vol > 0) {
      size_t rframes = ccord_audio_read(cap, pcm, AUDIO_FRAME_SAMPLES);
      if (rframes > 0) {
        apply_gain(pcm, rframes * 2, in_vol);
        (void)t->io.push(t->io.ud, pcm, rframes, &err);
      }
    }

    // Playback: mirror the original inline loop — only pull (and write) when not deafened and
    // output gain is non-zero, so deafen/volume are observable and no stale audio is queued.
    // Clear already-queued SDL playback samples on the transition so deafen silences at once.
    bool play_gated = (deafened || out_vol <= 0);
    if (play && !play_gated) {
      was_play_gated = false;
      size_t pframes = t->io.pull(t->io.ud, pcm, AUDIO_FRAME_SAMPLES);
      if (pframes > 0) {
        apply_gain(pcm, pframes * 2, out_vol);
        (void)ccord_audio_write(play, pcm, pframes);
      }
    } else if (play && play_gated && !was_play_gated) {
      ccord_audio_clear(play);
      was_play_gated = true;
    }

    struct timespec ts = {.tv_sec = 0, .tv_nsec = AUDIO_SLEEP_NS};
    nanosleep(&ts, nullptr);
  }

  // Close devices on the same thread that opened them, before the join returns.
  ccord_audio_destroy(cap);
  ccord_audio_destroy(play);
  return nullptr;
}

ccord_voice_audio_thread *ccord_voice_audio_thread_start(const ccord_voice_audio_io *io, ccord_error *err)
{
  ccord_error_clear(err);
  if (!io || !io->push || !io->pull) {
    ccord_error_set(err, CCORD_ERR_INVALID, "audio thread: push/pull callbacks required");
    return nullptr;
  }
  ccord_voice_audio_thread *t = ccord_calloc(1, sizeof *t);
  if (!t) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "audio thread: out of memory");
    return nullptr;
  }
  t->io = *io;
  atomic_init(&t->stop, false);
  atomic_init(&t->muted, false);
  atomic_init(&t->deafened, false);
  atomic_init(&t->in_vol, 100);
  atomic_init(&t->out_vol, 100);
  if (pthread_create(&t->thread, nullptr, audio_thread_main, t) != 0) {
    ccord_free(t);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "audio thread: pthread_create failed");
    return nullptr;
  }
  return t;
}

void ccord_voice_audio_thread_set_controls(ccord_voice_audio_thread *t, bool muted, bool deafened, int in_vol,
                                           int out_vol)
{
  if (!t) return;
  atomic_store(&t->muted, muted);
  atomic_store(&t->deafened, deafened);
  atomic_store(&t->in_vol, clamp_vol(in_vol));
  atomic_store(&t->out_vol, clamp_vol(out_vol));
}

void ccord_voice_audio_thread_stop(ccord_voice_audio_thread *t)
{
  if (!t) return;
  atomic_store(&t->stop, true);
  (void)pthread_join(t->thread, nullptr);
  ccord_free(t);
}
