// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M0-6: dedicated voice audio pthread. See voice_audio_thread.h. All SDL device open/read/
// write/close happens on this thread; the UI loop only publishes controls and stops+joins.
#include "voice_audio_thread.h"

#include <stdatomic.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/platform/platform.h>

#define AUDIO_FRAME_SAMPLES 960u          // 20 ms at 48 kHz, matches the gateway frame size
#define AUDIO_SLEEP_MS 20u                // 50 Hz active cadence matching 960-sample Opus frames
#define AUDIO_MAX_DRAIN_FRAMES 3          // cap frames drained per wakeup to absorb scheduling jitter

struct ccord_voice_audio_thread {
  ccord_thread *thread;
  ccord_mutex *mtx;
  ccord_cond *cv;
  ccord_voice_audio_io io;
  atomic_bool stop;
  atomic_bool muted;
  atomic_bool deafened;
  atomic_bool cap_suppressed;
  atomic_bool ptt_idle;
  atomic_int in_vol;
  atomic_int out_vol;
  _Atomic uint64_t ctrl_seq;
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

static void audio_thread_main(void *arg)
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
  bool was_cap_gated = false;
  bool was_play_gated = false;
  while (!atomic_load_explicit(&t->stop, memory_order_acquire)) {
    uint64_t seq = atomic_load_explicit(&t->ctrl_seq, memory_order_acquire);
    bool muted = atomic_load_explicit(&t->muted, memory_order_relaxed);
    bool deafened = atomic_load_explicit(&t->deafened, memory_order_relaxed);
    bool cap_suppressed = atomic_load_explicit(&t->cap_suppressed, memory_order_relaxed);
    bool ptt_idle = atomic_load_explicit(&t->ptt_idle, memory_order_relaxed);
    int in_vol = atomic_load_explicit(&t->in_vol, memory_order_relaxed);
    int out_vol = atomic_load_explicit(&t->out_vol, memory_order_relaxed);

    // Mic: while muted, suppressed, PTT-idle, or input gain is zero, pause the hardware capture
    // device (shutting down Android mic ADC / AudioFlinger capture) and discard buffered samples
    // so unread PCM cannot accumulate (+691 MiB/hr at 48 kHz stereo S16). Clear once more on
    // unpause before reading so stale samples are never transmitted.
    bool cap_gated = (muted || in_vol <= 0 || cap_suppressed || ptt_idle);
    if (cap && !cap_gated) {
      if (was_cap_gated) {
        ccord_audio_clear(cap);
        ccord_audio_set_paused(cap, false);
        was_cap_gated = false;
      }
      for (int f = 0; f < AUDIO_MAX_DRAIN_FRAMES && !atomic_load_explicit(&t->stop, memory_order_acquire); f++) {
        size_t rframes = ccord_audio_read(cap, pcm, AUDIO_FRAME_SAMPLES);
        if (rframes == 0) break;
        apply_gain(pcm, rframes * 2, in_vol);
        (void)t->io.push(t->io.ud, pcm, rframes, &err);
        if (rframes < AUDIO_FRAME_SAMPLES) break;
      }
    } else if (cap && cap_gated && !was_cap_gated) {
      ccord_audio_set_paused(cap, true);
      ccord_audio_clear(cap);
      was_cap_gated = true;
    }

    // Playback: pause the hardware output device and clear already-queued SDL playback samples on
    // the transition to deafened/zero-volume so audio silences at once and the DAC goes idle.
    bool play_gated = (deafened || out_vol <= 0);
    if (play && !play_gated) {
      if (was_play_gated) {
        ccord_audio_set_paused(play, false);
        was_play_gated = false;
      }
      for (int f = 0; f < AUDIO_MAX_DRAIN_FRAMES && !atomic_load_explicit(&t->stop, memory_order_acquire); f++) {
        size_t pframes = t->io.pull(t->io.ud, pcm, AUDIO_FRAME_SAMPLES);
        if (pframes == 0) break;
        apply_gain(pcm, pframes * 2, out_vol);
        (void)ccord_audio_write(play, pcm, pframes);
        if (pframes < AUDIO_FRAME_SAMPLES) break;
      }
    } else if (play && play_gated && !was_play_gated) {
      ccord_audio_set_paused(play, true);
      ccord_audio_clear(play);
      was_play_gated = true;
    }

    ccord_mutex_lock(t->mtx);
    if (cap_gated && play_gated) {
      // Both capture and playback are paused: block indefinitely at 0 Hz until a control change
      // or stop request signals t->cv.
      while (!atomic_load_explicit(&t->stop, memory_order_acquire) &&
             atomic_load_explicit(&t->ctrl_seq, memory_order_acquire) == seq) {
        ccord_cond_wait(t->cv, t->mtx);
      }
    } else if (!atomic_load_explicit(&t->stop, memory_order_acquire) &&
               atomic_load_explicit(&t->ctrl_seq, memory_order_acquire) == seq) {
      (void)ccord_cond_wait_timeout(t->cv, t->mtx, AUDIO_SLEEP_MS);
    }
    ccord_mutex_unlock(t->mtx);
  }

  // Close devices on the same thread that opened them, before the join returns.
  ccord_audio_destroy(cap);
  ccord_audio_destroy(play);
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
  t->mtx = ccord_mutex_create();
  t->cv = ccord_cond_create();
  if (!t->mtx || !t->cv) {
    ccord_cond_destroy(t->cv);
    ccord_mutex_destroy(t->mtx);
    ccord_free(t);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "audio thread: sync init failed");
    return nullptr;
  }
  t->io = *io;
  atomic_init(&t->stop, false);
  atomic_init(&t->muted, false);
  atomic_init(&t->deafened, false);
  atomic_init(&t->cap_suppressed, false);
  atomic_init(&t->ptt_idle, false);
  atomic_init(&t->in_vol, 100);
  atomic_init(&t->out_vol, 100);
  atomic_init(&t->ctrl_seq, 0u);
  t->thread = ccord_thread_create(audio_thread_main, t);
  if (!t->thread) {
    ccord_cond_destroy(t->cv);
    ccord_mutex_destroy(t->mtx);
    ccord_free(t);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "audio thread: thread start failed");
    return nullptr;
  }
  return t;
}

void ccord_voice_audio_thread_set_controls_ex(ccord_voice_audio_thread *t, bool muted, bool deafened,
                                              bool cap_suppressed, bool ptt_idle, int in_vol, int out_vol)
{
  if (!t) return;
  int iv = clamp_vol(in_vol);
  int ov = clamp_vol(out_vol);
  ccord_mutex_lock(t->mtx);
  if (atomic_load_explicit(&t->muted, memory_order_relaxed) == muted &&
      atomic_load_explicit(&t->deafened, memory_order_relaxed) == deafened &&
      atomic_load_explicit(&t->cap_suppressed, memory_order_relaxed) == cap_suppressed &&
      atomic_load_explicit(&t->ptt_idle, memory_order_relaxed) == ptt_idle &&
      atomic_load_explicit(&t->in_vol, memory_order_relaxed) == iv &&
      atomic_load_explicit(&t->out_vol, memory_order_relaxed) == ov) {
    ccord_mutex_unlock(t->mtx);
    return;
  }
  atomic_store_explicit(&t->muted, muted, memory_order_relaxed);
  atomic_store_explicit(&t->deafened, deafened, memory_order_relaxed);
  atomic_store_explicit(&t->cap_suppressed, cap_suppressed, memory_order_relaxed);
  atomic_store_explicit(&t->ptt_idle, ptt_idle, memory_order_relaxed);
  atomic_store_explicit(&t->in_vol, iv, memory_order_relaxed);
  atomic_store_explicit(&t->out_vol, ov, memory_order_relaxed);
  atomic_fetch_add_explicit(&t->ctrl_seq, 1u, memory_order_release);
  ccord_cond_signal(t->cv);
  ccord_mutex_unlock(t->mtx);
}

void ccord_voice_audio_thread_set_controls(ccord_voice_audio_thread *t, bool muted, bool deafened, int in_vol,
                                           int out_vol)
{
  ccord_voice_audio_thread_set_controls_ex(t, muted, deafened, false, false, in_vol, out_vol);
}

void ccord_voice_audio_thread_stop(ccord_voice_audio_thread *t)
{
  if (!t) return;
  ccord_mutex_lock(t->mtx);
  atomic_store_explicit(&t->stop, true, memory_order_release);
  ccord_cond_signal(t->cv);
  ccord_mutex_unlock(t->mtx);
  ccord_thread_join(t->thread);
  ccord_cond_destroy(t->cv);
  ccord_mutex_destroy(t->mtx);
  ccord_free(t);
}
