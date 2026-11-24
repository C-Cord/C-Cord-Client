// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M0-6: dedicated voice audio pthread + voice_gateway PCM locking.
//
// Part 1 drives ccord_voice_audio_thread against a test-local fake audio device: this file
// defines the platform ccord_audio_* symbols (no SDL / ccord_platform is linked), capture
// delivers a constant +/- tone and playback records every written sample, so the mute /
// deafen / input-volume / output-volume gates and BOTH gain paths are observable.
// Part 2 wires the thread to a real voice gateway (loopback SFU harness from test_voice_core)
// and checks the PCM round trip: mic tone -> encrypted RTP out, RX RTP -> decoded playback.
// Part 3 stress-tests the gateway mutex: six pthreads push / pull / soundboard / poll / feed
// RX while the audio thread moves PCM. With the lock removed, concurrent ring updates
// corrupt tx_samples / rx_samples and the drain memmove() runs out of bounds (ASan).
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <opus/opus.h>

#include <ccord/net/live_transport.h>
#include <ccord/net/voice_crypto.h>
#include <ccord/platform/platform.h>

#include "voice_audio_thread.h"

#include "ccord_test.h"

#define FRAME CCORD_VOICE_FRAME_SAMPLES
#define CH CCORD_VOICE_CHANNELS
#define RTP_PT 120u
#define TX_SSRC 1000u
#define RX_SSRC 2000u

static const double k_two_pi = 6.28311111111111323846;
static const uint8_t k_key[CCORD_VOICE_KEY_BYTES] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
  17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

// ---- timing helpers ------------------------------------------------------------------------------------------
static uint64_t mono_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void sleep_ms(unsigned ms)
{
  struct timespec ts = {.tv_sec = ms / 1000u, .tv_nsec = (long)(ms % 1000u) * 1000000L};
  nanosleep(&ts, nullptr);
}

// Waits until *v >= want (or timeout). Returns true when the condition was met.
static bool wait_i32(atomic_int *v, int want, unsigned timeout_ms)
{
  uint64_t end = mono_ms() + timeout_ms;
  while (mono_ms() < end) {
    if (atomic_load(v) >= want) return true;
    sleep_ms(5);
  }
  return atomic_load(v) >= want;
}

// Waits until *v grows past its current value (the audio thread made progress).
static bool resumes_i32(atomic_int *v, unsigned timeout_ms)
{
  int v0 = atomic_load(v);
  uint64_t end = mono_ms() + timeout_ms;
  while (mono_ms() < end) {
    if (atomic_load(v) > v0) return true;
    sleep_ms(5);
  }
  return atomic_load(v) > v0;
}

// True when *v stays unchanged for ms (the audio thread is gated off).
static bool frozen_i32(atomic_int *v, unsigned ms)
{
  int v0 = atomic_load(v);
  sleep_ms(ms);
  return atomic_load(v) == v0;
}

// ---- fake audio device (test-local ccord_audio_* implementation) ----------------------------------------------
// Capture returns 960-frame blocks of a constant tone (+amp on the left, -amp on the right);
// playback records the running max |sample| per channel parity plus the last written block.
static struct {
  atomic_int created, closed, cap_reads, play_writes, play_clears;
  atomic_bool cap_paused, play_paused;
  atomic_int cap_pause_calls, play_pause_calls;
  atomic_long play_frames;
  atomic_int cap_amp;
  pthread_mutex_t mtx;          // protects play_maxabs*, last[], and device id records
  int play_maxabs, play_maxabs_odd;
  int16_t last[FRAME * CH];
  size_t last_frames;
  char fail_dev_id[64];
  char last_cap_dev[64];
  char last_play_dev[64];
} g_fake = {.mtx = PTHREAD_MUTEX_INITIALIZER, .cap_amp = 10000};

struct ccord_audio {
  bool capture;
  bool paused;
};

ccord_audio *ccord_audio_create(const ccord_audio_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  if (!cfg) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "fake audio: null config");
    return nullptr;
  }
  const char *want = cfg->device_id ? cfg->device_id : "";
  pthread_mutex_lock(&g_fake.mtx);
  bool reject = g_fake.fail_dev_id[0] != '\0' && strcmp(want, g_fake.fail_dev_id) == 0;
  if (!reject) {
    char *dst = cfg->capture ? g_fake.last_cap_dev : g_fake.last_play_dev;
    snprintf(dst, 64, "%s", want);
  }
  pthread_mutex_unlock(&g_fake.mtx);
  if (reject) {
    (void)ccord_error_set(err, CCORD_ERR_IO, "fake audio: device unavailable");
    return nullptr;
  }
  ccord_audio *a = calloc(1, sizeof *a);
  if (!a) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "fake audio: out of memory");
    return nullptr;
  }
  a->capture = cfg->capture;
  atomic_fetch_add(&g_fake.created, 1);
  return a;
}

void ccord_audio_destroy(ccord_audio *a)
{
  if (!a) return;
  atomic_fetch_add(&g_fake.closed, 1);
  free(a);
}

size_t ccord_audio_read(ccord_audio *a, int16_t *pcm, size_t frames)
{
  if (!a || !a->capture || a->paused || !pcm) return 0;
  size_t n = frames < FRAME ? frames : FRAME;
  int amp = atomic_load(&g_fake.cap_amp);
  for (size_t i = 0; i < n; i++) {
    pcm[i * CH] = (int16_t)amp;
    pcm[i * CH + 1] = (int16_t)-amp;
  }
  atomic_fetch_add(&g_fake.cap_reads, 1);
  return n;
}

size_t ccord_audio_write(ccord_audio *a, const int16_t *pcm, size_t frames)
{
  if (!a || a->capture || a->paused || !pcm) return 0;
  int maxabs = 0, maxodd = 0;
  for (size_t i = 0; i < frames; i++) {
    int v0 = abs(pcm[i * CH]);
    int v1 = abs(pcm[i * CH + 1]);
    if (v0 > maxabs) maxabs = v0;
    if (v1 > maxodd) maxodd = v1;
  }
  pthread_mutex_lock(&g_fake.mtx);
  if (maxabs > g_fake.play_maxabs) g_fake.play_maxabs = maxabs;
  if (maxodd > g_fake.play_maxabs_odd) g_fake.play_maxabs_odd = maxodd;
  size_t n = frames < FRAME ? frames : FRAME;
  memcpy(g_fake.last, pcm, n * CH * sizeof(int16_t));
  g_fake.last_frames = n;
  pthread_mutex_unlock(&g_fake.mtx);
  atomic_fetch_add(&g_fake.play_writes, 1);
  atomic_fetch_add(&g_fake.play_frames, (long)frames);
  return frames;
}

void ccord_audio_clear(ccord_audio *a)
{
  if (!a || a->capture) return;
  atomic_fetch_add(&g_fake.play_clears, 1);
}

void ccord_audio_set_paused(ccord_audio *a, bool paused)
{
  if (!a || a->paused == paused) return;
  a->paused = paused;
  if (a->capture) {
    atomic_store(&g_fake.cap_paused, paused);
    if (paused) atomic_fetch_add(&g_fake.cap_pause_calls, 1);
  } else {
    atomic_store(&g_fake.play_paused, paused);
    if (paused) atomic_fetch_add(&g_fake.play_pause_calls, 1);
  }
}

static void fake_reset(void)
{
  atomic_store(&g_fake.created, 0);
  atomic_store(&g_fake.closed, 0);
  atomic_store(&g_fake.cap_reads, 0);
  atomic_store(&g_fake.play_writes, 0);
  atomic_store(&g_fake.play_clears, 0);
  atomic_store(&g_fake.cap_paused, false);
  atomic_store(&g_fake.play_paused, false);
  atomic_store(&g_fake.cap_pause_calls, 0);
  atomic_store(&g_fake.play_pause_calls, 0);
  atomic_store(&g_fake.play_frames, 0);
  atomic_store(&g_fake.cap_amp, 10000);
  pthread_mutex_lock(&g_fake.mtx);
  g_fake.play_maxabs = 0;
  g_fake.play_maxabs_odd = 0;
  g_fake.last_frames = 0;
  g_fake.fail_dev_id[0] = '\0';
  g_fake.last_cap_dev[0] = '\0';
  g_fake.last_play_dev[0] = '\0';
  pthread_mutex_unlock(&g_fake.mtx);
}

static void fake_play_max(int *even, int *odd)
{
  pthread_mutex_lock(&g_fake.mtx);
  *even = g_fake.play_maxabs;
  *odd = g_fake.play_maxabs_odd;
  pthread_mutex_unlock(&g_fake.mtx);
}

// Last frame of the most recently written block: catches gain applied to only part of the
// interleaved buffer (a running max cannot see an unscaled tail when the tone is constant).
static void fake_play_last(int *even, int *odd)
{
  pthread_mutex_lock(&g_fake.mtx);
  *even = g_fake.last[(FRAME - 1) * CH];
  *odd = g_fake.last[(FRAME - 1) * CH + 1];
  pthread_mutex_unlock(&g_fake.mtx);
}

// ---- probe IO hooks (part 1) ----------------------------------------------------------------------------------
typedef struct audio_probe {
  atomic_int push_count, push_nonzero, push_maxabs, push_maxabs_odd;
  atomic_int push_last_even, push_last_odd;
  atomic_long push_frames;
  atomic_int pull_count;
} audio_probe;

static ccord_status probe_push(void *ud, const int16_t *pcm, size_t frames, ccord_error *err)
{
  ccord_error_clear(err);
  audio_probe *p = ud;
  if (frames == 0) return CCORD_OK;
  atomic_fetch_add(&p->push_count, 1);
  atomic_fetch_add(&p->push_frames, (long)frames);
  int maxabs = 0, maxodd = 0;
  bool nonzero = false;
  for (size_t i = 0; i < frames; i++) {
    int v0 = abs(pcm[i * CH]);
    int v1 = abs(pcm[i * CH + 1]);
    if (v0 != 0 || v1 != 0) nonzero = true;
    if (v0 > maxabs) maxabs = v0;
    if (v1 > maxodd) maxodd = v1;
  }
  if (nonzero) atomic_fetch_add(&p->push_nonzero, 1);
  // Only the audio thread calls push, so load/store of the running max is race-free.
  if (maxabs > atomic_load(&p->push_maxabs)) atomic_store(&p->push_maxabs, maxabs);
  if (maxodd > atomic_load(&p->push_maxabs_odd)) atomic_store(&p->push_maxabs_odd, maxodd);
  // Last frame of the block: verifies gain covered the whole buffer, not just the first half.
  atomic_store(&p->push_last_even, pcm[(frames - 1) * CH]);
  atomic_store(&p->push_last_odd, pcm[(frames - 1) * CH + 1]);
  return CCORD_OK;
}

static size_t probe_pull(void *ud, int16_t *pcm, size_t max_frames)
{
  audio_probe *p = ud;
  size_t n = max_frames < FRAME ? max_frames : FRAME;
  for (size_t i = 0; i < n; i++) {
    pcm[i * CH] = 20000;
    pcm[i * CH + 1] = -20000;
  }
  atomic_fetch_add(&p->pull_count, 1);
  return n;
}

// ---- 1. lifecycle, control gates, gain ------------------------------------------------------------------------
static void test_thread_lifecycle_and_controls(void)
{
  fake_reset();
  audio_probe p = {0};
  ccord_voice_audio_io io = {.push = probe_push, .pull = probe_pull, .ud = &p};
  ccord_error err;
  ccord_voice_audio_thread *t = ccord_voice_audio_thread_start(&io, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;

  CT_CHECK(wait_i32(&p.push_count, 3, 2000));
  CT_CHECK(wait_i32(&p.pull_count, 3, 2000));
  CT_CHECK(atomic_load(&g_fake.created) == 2);   // capture + playback opened on the thread
  CT_CHECK(atomic_load(&g_fake.closed) == 0);
  CT_CHECK(atomic_load(&g_fake.cap_reads) > 0);
  CT_CHECK(atomic_load(&g_fake.play_writes) > 0);
  // Unity gain (default 100 %): the +/-10000 mic tone and +/-20000 playout pass through unchanged.
  CT_CHECK(atomic_load(&p.push_maxabs) == 10000 && atomic_load(&p.push_maxabs_odd) == 10000);
  int pe = 0, po = 0;
  fake_play_max(&pe, &po);
  CT_CHECK(pe == 20000 && po == 20000);
  CT_CHECK(atomic_load(&p.push_nonzero) == atomic_load(&p.push_count));
  CT_CHECK(atomic_load(&p.push_frames) % (long)FRAME == 0);
  CT_CHECK(atomic_load(&g_fake.play_frames) % (long)FRAME == 0);

  // Mute gates the mic only; deafen gates playout only; zero volumes gate each path.
  ccord_voice_audio_thread_set_controls(t, true, false, 100, 100);
  sleep_ms(30);
  CT_CHECK(frozen_i32(&p.push_count, 60));
  CT_CHECK(resumes_i32(&p.pull_count, 2000));
  ccord_voice_audio_thread_set_controls(t, false, true, 100, 100);
  sleep_ms(30);
  CT_CHECK(frozen_i32(&p.pull_count, 60));
  CT_CHECK(atomic_load(&g_fake.play_clears) >= 1);
  CT_CHECK(resumes_i32(&p.push_count, 2000));
  ccord_voice_audio_thread_set_controls(t, false, false, 0, 100);
  sleep_ms(30);
  CT_CHECK(frozen_i32(&p.push_count, 60));
  ccord_voice_audio_thread_set_controls(t, false, false, 100, 0);
  sleep_ms(30);
  CT_CHECK(frozen_i32(&p.pull_count, 60));
  CT_CHECK(resumes_i32(&p.push_count, 2000));
  ccord_voice_audio_thread_set_controls(t, false, false, 100, 100);
  CT_CHECK(resumes_i32(&p.pull_count, 2000));

  // 150 % scales both paths (10000 -> 15000, 20000 -> 30000), per channel.
  ccord_voice_audio_thread_set_controls(t, false, false, 150, 150);
  sleep_ms(60);
  CT_CHECK(atomic_load(&p.push_maxabs) == 15000 && atomic_load(&p.push_maxabs_odd) == 15000);
  fake_play_max(&pe, &po);
  CT_CHECK(pe == 30000 && po == 30000);
  // The last frame of each block must be scaled too (gain covers all rframes*2 / pframes*2
  // interleaved samples, not just the first half).
  CT_CHECK(atomic_load(&p.push_last_even) == 15000 && atomic_load(&p.push_last_odd) == -15000);
  fake_play_last(&pe, &po);
  CT_CHECK(pe == 30000 && po == -30000);

  // 500 % clamps to 200 %: mic 10000 -> 20000, playout 20000 -> 40000 saturates at +32767/-32768.
  ccord_voice_audio_thread_set_controls(t, false, false, 500, 500);
  sleep_ms(60);
  CT_CHECK(atomic_load(&p.push_maxabs) == 20000 && atomic_load(&p.push_maxabs_odd) == 20000);
  fake_play_max(&pe, &po);
  CT_CHECK(pe == 32767 && po == 32768);

  // Negative volumes clamp to 0 and gate both paths off.
  ccord_voice_audio_thread_set_controls(t, false, false, -50, -50);
  sleep_ms(30);
  CT_CHECK(frozen_i32(&p.push_count, 60));
  CT_CHECK(frozen_i32(&p.pull_count, 60));

  // Capture saturation: 30000 at 200 % -> 60000 clamps to +32767/-32768 (not wrapping).
  atomic_store(&g_fake.cap_amp, 30000);
  ccord_voice_audio_thread_set_controls(t, false, false, 200, 100);
  sleep_ms(60);
  CT_CHECK(atomic_load(&p.push_maxabs) == 32767 && atomic_load(&p.push_maxabs_odd) == 32768);

  // Stop joins promptly, closes both devices on the thread, and leaves no callbacks running.
  uint64_t t0 = mono_ms();
  ccord_voice_audio_thread_stop(t);
  CT_CHECK(mono_ms() - t0 < 500);
  CT_CHECK(atomic_load(&g_fake.closed) == 2);
  int pc = atomic_load(&p.push_count);
  int pl = atomic_load(&p.pull_count);
  sleep_ms(60);
  CT_CHECK(atomic_load(&p.push_count) == pc);
  CT_CHECK(atomic_load(&p.pull_count) == pl);
}

static void test_thread_invalid_io(void)
{
  fake_reset();
  ccord_error err;
  CT_CHECK(ccord_voice_audio_thread_start(nullptr, &err) == nullptr);
  CT_CHECK(err.code == CCORD_ERR_INVALID);
  ccord_voice_audio_io io = {.push = probe_push, .pull = nullptr, .ud = nullptr};
  CT_CHECK(ccord_voice_audio_thread_start(&io, &err) == nullptr);
  CT_CHECK(err.code == CCORD_ERR_INVALID);
  io = (ccord_voice_audio_io){.push = nullptr, .pull = probe_pull, .ud = nullptr};
  CT_CHECK(ccord_voice_audio_thread_start(&io, &err) == nullptr);
  CT_CHECK(err.code == CCORD_ERR_INVALID);
  // Null-safe controls/stop; no device was ever opened.
  ccord_voice_audio_thread_set_controls(nullptr, true, true, 50, 50);
  ccord_voice_audio_thread_stop(nullptr);
  CT_CHECK(atomic_load(&g_fake.created) == 0);
}

static void test_thread_device_selection_and_fallback(void)
{
  fake_reset();
  audio_probe p = {0};
  ccord_voice_audio_io io = {.push = probe_push, .pull = probe_pull, .ud = &p};
  snprintf(io.input_device_id, sizeof io.input_device_id, "mic-42");
  snprintf(io.output_device_id, sizeof io.output_device_id, "spk-99");
  ccord_error err;
  ccord_voice_audio_thread *t = ccord_voice_audio_thread_start(&io, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(wait_i32(&p.push_count, 1, 2000));
  CT_CHECK(wait_i32(&p.pull_count, 1, 2000));
  pthread_mutex_lock(&g_fake.mtx);
  bool cap_ok = strcmp(g_fake.last_cap_dev, "mic-42") == 0;
  bool play_ok = strcmp(g_fake.last_play_dev, "spk-99") == 0;
  pthread_mutex_unlock(&g_fake.mtx);
  CT_CHECK(cap_ok && play_ok);
  ccord_voice_audio_thread_stop(t);

  // If the requested device fails to open at thread start, fall back to system default ("").
  fake_reset();
  p = (audio_probe){0};
  pthread_mutex_lock(&g_fake.mtx);
  snprintf(g_fake.fail_dev_id, sizeof g_fake.fail_dev_id, "vanished-dev");
  pthread_mutex_unlock(&g_fake.mtx);
  snprintf(io.input_device_id, sizeof io.input_device_id, "vanished-dev");
  snprintf(io.output_device_id, sizeof io.output_device_id, "vanished-dev");
  t = ccord_voice_audio_thread_start(&io, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(wait_i32(&p.push_count, 1, 2000));
  CT_CHECK(wait_i32(&p.pull_count, 1, 2000));
  pthread_mutex_lock(&g_fake.mtx);
  cap_ok = strcmp(g_fake.last_cap_dev, "") == 0;
  play_ok = strcmp(g_fake.last_play_dev, "") == 0;
  pthread_mutex_unlock(&g_fake.mtx);
  CT_CHECK(cap_ok && play_ok);
  ccord_voice_audio_thread_stop(t);
}

static void test_thread_pause_and_idle_controls(void)
{
  fake_reset();
  audio_probe p = {0};
  ccord_voice_audio_io io = {.push = probe_push, .pull = probe_pull, .ud = &p};
  ccord_error err;
  ccord_voice_audio_thread *t = ccord_voice_audio_thread_start(&io, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;

  CT_CHECK(wait_i32(&p.push_count, 2, 2000));
  CT_CHECK(wait_i32(&p.pull_count, 2, 2000));
  CT_CHECK(!atomic_load(&g_fake.cap_paused) && !atomic_load(&g_fake.play_paused));

  // Stage audience suppress pauses capture hardware while playback stays active.
  ccord_voice_audio_thread_set_controls_ex(t, false, false, true, false, 100, 100);
  sleep_ms(40);
  CT_CHECK(atomic_load(&g_fake.cap_paused));
  CT_CHECK(!atomic_load(&g_fake.play_paused));
  CT_CHECK(frozen_i32(&p.push_count, 60));
  CT_CHECK(resumes_i32(&p.pull_count, 2000));

  // Un-suppress resumes capture hardware immediately.
  ccord_voice_audio_thread_set_controls_ex(t, false, false, false, false, 100, 100);
  CT_CHECK(resumes_i32(&p.push_count, 2000));
  CT_CHECK(!atomic_load(&g_fake.cap_paused));

  // Idle PTT pauses capture hardware while playback stays active.
  ccord_voice_audio_thread_set_controls_ex(t, false, false, false, true, 100, 100);
  sleep_ms(40);
  CT_CHECK(atomic_load(&g_fake.cap_paused));
  CT_CHECK(frozen_i32(&p.push_count, 60));
  CT_CHECK(resumes_i32(&p.pull_count, 2000));

  // Both capture and playback gated: pauses both streams and enters 0 Hz indefinite wait,
  // waking promptly on undeafen/unmute or stop.
  ccord_voice_audio_thread_set_controls_ex(t, true, true, false, false, 100, 100);
  sleep_ms(40);
  CT_CHECK(atomic_load(&g_fake.cap_paused) && atomic_load(&g_fake.play_paused));
  CT_CHECK(frozen_i32(&p.push_count, 80));
  CT_CHECK(frozen_i32(&p.pull_count, 80));

  // Wake promptly from indefinite wait via control change.
  ccord_voice_audio_thread_set_controls_ex(t, false, false, false, false, 100, 100);
  CT_CHECK(resumes_i32(&p.push_count, 2000));
  CT_CHECK(resumes_i32(&p.pull_count, 2000));
  CT_CHECK(!atomic_load(&g_fake.cap_paused) && !atomic_load(&g_fake.play_paused));

  // Enter indefinite wait again and verify stop wakes and joins promptly.
  ccord_voice_audio_thread_set_controls_ex(t, true, true, false, false, 100, 100);
  sleep_ms(40);
  uint64_t t0 = mono_ms();
  ccord_voice_audio_thread_stop(t);
  CT_CHECK(mono_ms() - t0 < 500);
  CT_CHECK(atomic_load(&g_fake.closed) == 2);
}

// ---- captured WebSocket frames + injectable clock (harness from test_voice_core) ------------------------------
typedef struct vcap {
  pthread_mutex_t mtx;          // the audio thread sends via the gateway while main reads
  char frames[256][512];
  size_t count;
} vcap;

static ccord_status vcap_send(void *ud, const char *data, size_t len, ccord_error *err)
{
  vcap *c = ud;
  ccord_error_clear(err);
  pthread_mutex_lock(&c->mtx);
  if (c->count >= 256 || len >= sizeof c->frames[0]) {
    pthread_mutex_unlock(&c->mtx);
    return ccord_error_set(err, CCORD_ERR_RANGE, "capture full");
  }
  memcpy(c->frames[c->count], data, len);
  c->frames[c->count][len] = '\0';
  c->count++;
  pthread_mutex_unlock(&c->mtx);
  return CCORD_OK;
}

static _Atomic uint64_t g_tnow;
static uint64_t voice_now(void *ud)
{
  (void)ud;
  return atomic_load(&g_tnow);
}

static int frame_op(const char *f)
{
  const char *t = strstr(f, "\"op\":");
  return t ? atoi(t + 5) : -1;
}
static int frame_speaking(const char *f)
{
  const char *t = strstr(f, "\"speaking\":");
  return t ? atoi(t + 11) : -1;
}
static bool cap_has_speaking(vcap *c, int flags)
{
  pthread_mutex_lock(&c->mtx);
  bool found = false;
  for (size_t i = 0; i < c->count; i++) {
    if (frame_op(c->frames[i]) == 5 && frame_speaking(c->frames[i]) == flags) {
      found = true;
      break;
    }
  }
  pthread_mutex_unlock(&c->mtx);
  return found;
}

// ---- loopback SFU ---------------------------------------------------------------------------------------------
typedef struct sfu {
  int fd;
  uint16_t port;
  struct sockaddr_in gw;  // the gateway's UDP source address (learned from the discovery request)
} sfu;

static bool sfu_open(sfu *s)
{
  s->fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (s->fd < 0) return false;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  socklen_t len = sizeof a;
  if (bind(s->fd, (struct sockaddr *)&a, sizeof a) != 0 || getsockname(s->fd, (struct sockaddr *)&a, &len) != 0) {
    close(s->fd);
    s->fd = -1;
    return false;
  }
  s->port = ntohs(a.sin_port);
  return true;
}

static void sfu_close(sfu *s)
{
  if (s->fd >= 0) close(s->fd);
  s->fd = -1;
}

static bool sfu_answer_discovery(sfu *s)
{
  struct pollfd pfd = {.fd = s->fd, .events = POLLIN, .revents = 0};
  if (poll(&pfd, 1, 2000) <= 0) return false;
  uint8_t req[128];
  struct sockaddr_in from;
  socklen_t flen = sizeof from;
  ssize_t n = recvfrom(s->fd, req, sizeof req, 0, (struct sockaddr *)&from, &flen);
  if (n != (ssize_t)CCORD_VOICE_IP_DISCOVERY_SIZE || req[0] != 0x00 || req[1] != 0x01) return false;
  s->gw = from;
  uint8_t resp[CCORD_VOICE_IP_DISCOVERY_SIZE];
  memset(resp, 0, sizeof resp);
  resp[0] = 0x00;
  resp[1] = 0x02;
  resp[2] = 0x00;
  resp[3] = 0x46;  // 70-byte body
  memcpy(resp + 4, req + 4, 4);  // echo the SSRC
  memcpy(resp + 8, "127.0.0.1", 9);
  uint16_t gport = ntohs(from.sin_port);
  resp[72] = (uint8_t)(gport >> 8);
  resp[73] = (uint8_t)(gport & 0xFFu);
  return sendto(s->fd, resp, sizeof resp, 0, (struct sockaddr *)&from, flen) == (ssize_t)sizeof resp;
}

// ---- handshake ------------------------------------------------------------------------------------------------
static void vg_poll(ccord_voice_gateway *g)
{
  ccord_voice_event ev;
  (void)ccord_voice_gateway_poll(g, &ev);
}

static ccord_status vg_handle(ccord_voice_gateway *g, const char *json)
{
  ccord_voice_event ev;
  ccord_error err;
  return ccord_voice_gateway_handle(g, json, strlen(json), &ev, &err);
}

// Brings a fresh gateway to ESTABLISHED with a 1 s heartbeat. fake_clock selects the injectable
// g_tnow clock (functional tests) vs the real ccord_clock_ms (stress). cap holds IDENTIFY + SELECT.
static ccord_voice_gateway *bring_up(vcap *cap, sfu *s, bool fake_clock)
{
  memset(cap, 0, sizeof *cap);
  pthread_mutex_init(&cap->mtx, nullptr);
  memset(s, 0, sizeof *s);
  s->fd = -1;
  if (fake_clock) atomic_store(&g_tnow, 1000);
  ccord_voice_gateway_config cfg = {
    .server_id = 77,
    .channel_id = 88,
    .user_id = 42,
    .session_id = "voice-session",
    .token = "REDACTED_TEST_TOKEN",
    .endpoint = "voice.example",
    .connect_timeout_ms = 100,
    .now_ms = fake_clock ? voice_now : nullptr,
    .clock_ud = nullptr,
    .send = vcap_send,
    .send_ud = cap,
  };
  ccord_error err;
  ccord_voice_gateway *g = ccord_voice_gateway_create(&cfg, &err);
  if (!g) return nullptr;
  if (ccord_voice_gateway_start(g, &err) != CCORD_OK) goto fail;
  if (vg_handle(g, "{\"op\":8,\"d\":{\"heartbeat_interval\":1000}}") != CCORD_OK) goto fail;
  if (!sfu_open(s)) goto fail;
  char ready[256];
  snprintf(ready, sizeof ready,
           "{\"op\":2,\"d\":{\"ssrc\":%u,\"ip\":\"127.0.0.1\",\"port\":%u,"
           "\"modes\":[\"aead_xchacha20_poly1305_rtpsize\"]}}",
           TX_SSRC, (unsigned)s->port);
  if (vg_handle(g, ready) != CCORD_OK) goto fail;
  if (!sfu_answer_discovery(s)) goto fail;
  if (fake_clock) atomic_fetch_add(&g_tnow, 300);
  vg_poll(g);  // consumes the discovery response -> SELECT_PROTOCOL
  char key_json[256];
  size_t off = 0;
  off += (size_t)snprintf(key_json + off, sizeof key_json - off, "{\"op\":4,\"d\":{\"mode\":"
                       "\"aead_xchacha20_poly1305_rtpsize\",\"dave_protocol_version\":0,\"secret_key\":[");
  for (unsigned i = 0; i < CCORD_VOICE_KEY_BYTES; i++) {
    off += (size_t)snprintf(key_json + off, sizeof key_json - off, "%s%u", i ? "," : "", (unsigned)k_key[i]);
    if (off >= sizeof key_json) off = sizeof key_json - 1;
  }
  snprintf(key_json + off, sizeof key_json - off, "]}}");
  if (vg_handle(g, key_json) != CCORD_OK) goto fail;
  if (ccord_voice_gateway_state(g) != CCORD_VOICE_ESTABLISHED) goto fail;
  return g;
fail:
  ccord_voice_gateway_destroy(g);
  sfu_close(s);
  return nullptr;
}

// ---- signal helpers -------------------------------------------------------------------------------------------
static void gen_tone(int16_t *stereo, double amp, double freq, double *phase)
{
  for (size_t i = 0; i < FRAME; i++) {
    double s = amp * sin(*phase);
    *phase += k_two_pi * freq / 48000.0;
    int16_t v = (int16_t)(s > 32767.0 ? 32767.0 : (s < -32768.0 ? -32768.0 : s));
    stereo[i * CH] = v;
    stereo[i * CH + 1] = v;
  }
}

// Sends one encrypted RX RTP frame (ssrc RX_SSRC) from the SFU to the gateway; returns wire bytes.
static size_t send_rx(sfu *s, OpusEncoder *enc, unsigned seq, const int16_t *pcm)
{
  uint8_t opus[512];
  int n = opus_encode(enc, pcm, (int)FRAME, opus, (int)sizeof opus);
  if (n <= 0) return 0;
  uint8_t hdr[CCORD_VOICE_RTP_HEADER_SIZE];
  if (ccord_rtp_write_header(hdr, sizeof hdr, RTP_PT, false, (uint16_t)seq, (uint32_t)seq * FRAME, RX_SSRC) == 0)
    return 0;
  uint8_t pkt[600];
  size_t pkt_len = 0;
  ccord_error err;
  if (ccord_voice_seal(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, (uint32_t)seq + 1u, hdr, sizeof hdr, opus,
                       (size_t)n, pkt, sizeof pkt, &pkt_len, &err) != CCORD_OK)
    return 0;
  if (sendto(s->fd, pkt, pkt_len, 0, (struct sockaddr *)&s->gw, sizeof s->gw) != (ssize_t)pkt_len) return 0;
  return pkt_len;
}

typedef struct rxcap {
  uint8_t pkt[64][400];
  size_t len[64];
  size_t count;
} rxcap;

static size_t sfu_drain(sfu *s, rxcap *r)
{
  r->count = 0;
  for (;;) {
    struct pollfd pfd = {.fd = s->fd, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, 15) <= 0) break;
    uint8_t buf[2048];
    ssize_t n = recvfrom(s->fd, buf, sizeof buf, 0, nullptr, nullptr);
    if (n <= 0) break;
    if (r->count < 64 && (size_t)n <= sizeof r->pkt[0]) {
      memcpy(r->pkt[r->count], buf, (size_t)n);
      r->len[r->count] = (size_t)n;
      r->count++;
    }
  }
  return r->count;
}

static bool rtp_is_silence(const uint8_t *pkt, size_t len)
{
  uint8_t plain[64];
  size_t plain_len = 0;
  ccord_error err;
  if (ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, pkt, len, plain, sizeof plain, &plain_len,
                       nullptr, &err) != CCORD_OK)
    return false;
  return plain_len == 3 && plain[0] == 0xF8 && plain[1] == 0xFF && plain[2] == 0xFE;
}

static void map_rx_user(ccord_voice_gateway *g, const char *user_id)
{
  char json[128];
  snprintf(json, sizeof json, "{\"op\":5,\"d\":{\"ssrc\":%u,\"user_id\":\"%s\",\"speaking\":1}}", RX_SSRC, user_id);
  (void)vg_handle(g, json);
}

// Audio-thread IO hooks wired straight to the gateway (parts 2 and 3).
static ccord_status gw_push(void *ud, const int16_t *pcm, size_t frames, ccord_error *err)
{
  return ccord_voice_gateway_push_pcm(ud, pcm, frames, err);
}

static size_t gw_pull(void *ud, int16_t *pcm, size_t max_frames)
{
  return ccord_voice_gateway_pull_pcm(ud, pcm, max_frames);
}

// ---- 2. gateway PCM round trip through the audio thread --------------------------------------------------------
static void test_gateway_roundtrip(void)
{
  fake_reset();
  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s, true);
  CT_CHECK(g != nullptr);
  if (!g) return;
  map_rx_user(g, "777");
  int opus_err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(48000, (int)CH, OPUS_APPLICATION_AUDIO, &opus_err);
  CT_CHECK(enc != nullptr);
  if (!enc) {
    ccord_voice_gateway_destroy(g);
    sfu_close(&s);
    return;
  }

  ccord_voice_audio_io io = {.push = gw_push, .pull = gw_pull, .ud = g};
  ccord_error err;
  ccord_voice_audio_thread *t = ccord_voice_audio_thread_start(&io, &err);
  CT_CHECK(t != nullptr);
  if (!t) {
    opus_encoder_destroy(enc);
    ccord_voice_gateway_destroy(g);
    sfu_close(&s);
    return;
  }

  // TX: the fake mic tone must reach the SFU as audible RTP. Advance the fake clock so the
  // 20 ms TX pacing keeps draining while the audio thread pushes.
  rxcap r;
  bool got_tx = false;
  for (int i = 0; i < 200 && !got_tx; i++) {
    atomic_fetch_add(&g_tnow, 20);
    if (sfu_drain(&s, &r) > 0) got_tx = true;
    else sleep_ms(10);
  }
  CT_CHECK(got_tx);
  if (got_tx) CT_CHECK(!rtp_is_silence(r.pkt[0], r.len[0]));
  CT_CHECK(cap_has_speaking(&cap, 1));

  // RX: feed encrypted frames; the audio thread pulls, mixes and writes them to playback.
  int16_t sig[FRAME * CH];
  double phase = 0.0;
  for (unsigned i = 0; i < 8; i++) {
    gen_tone(sig, 8000.0, 440.0, &phase);
    CT_CHECK(send_rx(&s, enc, i, sig) > 0);
    atomic_fetch_add(&g_tnow, 20);
    sleep_ms(5);
  }
  bool got_rx = false;
  uint64_t end = mono_ms() + 2000;
  while (mono_ms() < end) {
    int pe = 0, po = 0;
    fake_play_max(&pe, &po);
    if (pe > 1000) {
      got_rx = true;
      break;
    }
    sleep_ms(10);
  }
  CT_CHECK(got_rx);

  ccord_voice_audio_thread_stop(t);
  CT_CHECK(atomic_load(&g_fake.closed) == 2);
  opus_encoder_destroy(enc);
  ccord_voice_gateway_destroy(g);
  sfu_close(&s);
  pthread_mutex_destroy(&cap.mtx);
}

// ---- 3. concurrent stress: audio thread + 6 actors on one locked gateway ---------------------------------------
typedef struct stress_ctx {
  ccord_voice_gateway *g;
  sfu *s;
  OpusEncoder *enc;
  const int16_t *loud;
  const int16_t *sb;
  atomic_bool run;
  atomic_int pushes, sb_pushes, pulls, polls, feeds;
  atomic_long pull_frames;
} stress_ctx;

static void *stress_pusher(void *arg)
{
  stress_ctx *c = arg;
  ccord_error err;
  while (atomic_load(&c->run)) {
    (void)ccord_voice_gateway_push_pcm(c->g, c->loud, FRAME, &err);
    atomic_fetch_add(&c->pushes, 1);
  }
  return nullptr;
}

static void *stress_sb_pusher(void *arg)
{
  stress_ctx *c = arg;
  ccord_error err;
  while (atomic_load(&c->run)) {
    (void)ccord_voice_gateway_push_soundboard_pcm(c->g, c->sb, FRAME, &err);
    atomic_fetch_add(&c->sb_pushes, 1);
  }
  return nullptr;
}

static void *stress_puller(void *arg)
{
  stress_ctx *c = arg;
  int16_t buf[FRAME * CH];
  while (atomic_load(&c->run)) {
    size_t n = ccord_voice_gateway_pull_pcm(c->g, buf, FRAME);
    atomic_fetch_add(&c->pulls, 1);
    atomic_fetch_add(&c->pull_frames, (long)n);
  }
  return nullptr;
}

static void *stress_puller_big(void *arg)
{
  stress_ctx *c = arg;
  int16_t buf[FRAME * CH * 8];
  while (atomic_load(&c->run)) {
    size_t n = ccord_voice_gateway_pull_pcm(c->g, buf, FRAME * 8);
    atomic_fetch_add(&c->pulls, 1);
    atomic_fetch_add(&c->pull_frames, (long)n);
  }
  return nullptr;
}

static void *stress_poller(void *arg)
{
  stress_ctx *c = arg;
  ccord_voice_event ev;
  while (atomic_load(&c->run)) {
    (void)ccord_voice_gateway_poll(c->g, &ev);
    atomic_fetch_add(&c->polls, 1);
  }
  return nullptr;
}

static void *stress_feeder(void *arg)
{
  stress_ctx *c = arg;
  int16_t sig[FRAME * CH];
  double phase = 0.0;
  unsigned seq = 0;
  while (atomic_load(&c->run)) {
    gen_tone(sig, 8000.0, 440.0, &phase);
    if (send_rx(c->s, c->enc, seq++, sig) > 0) atomic_fetch_add(&c->feeds, 1);
  }
  return nullptr;
}

static void test_gateway_stress(void)
{
  fake_reset();
  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s, false);  // real clock
  CT_CHECK(g != nullptr);
  if (!g) return;
  map_rx_user(g, "777");
  int opus_err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(48000, (int)CH, OPUS_APPLICATION_AUDIO, &opus_err);
  CT_CHECK(enc != nullptr);
  if (!enc) {
    ccord_voice_gateway_destroy(g);
    sfu_close(&s);
    return;
  }

  int16_t loud[FRAME * CH];
  int16_t sb[FRAME * CH];
  int16_t sig[FRAME * CH];
  double phase = 0.0;
  gen_tone(loud, 12000.0, 440.0, &phase);
  gen_tone(sb, 9000.0, 880.0, &phase);
  // Pre-feed RX so the pullers have data immediately.
  for (unsigned i = 0; i < 30; i++) {
    gen_tone(sig, 8000.0, 440.0, &phase);
    CT_CHECK(send_rx(&s, enc, i, sig) > 0);
  }

  stress_ctx c = {.g = g, .s = &s, .enc = enc, .loud = loud, .sb = sb};
  atomic_store(&c.run, true);
  ccord_voice_audio_io io = {.push = gw_push, .pull = gw_pull, .ud = g};
  ccord_error err;
  ccord_voice_audio_thread *t = ccord_voice_audio_thread_start(&io, &err);
  CT_CHECK(t != nullptr);
  if (!t) {
    opus_encoder_destroy(enc);
    ccord_voice_gateway_destroy(g);
    sfu_close(&s);
    return;
  }

  // Let the audio thread consume at least one pre-fed RX frame before the unthrottled stress
  // pullers compete for the same single-consumer rx_pcm ring (audio_thread_main paces at 20 ms).
  CT_CHECK(wait_i32(&g_fake.play_writes, 1, 2000));
  int cap_before = atomic_load(&g_fake.cap_reads);

  pthread_t th[6];
  CT_CHECK(pthread_create(&th[0], nullptr, stress_pusher, &c) == 0);
  CT_CHECK(pthread_create(&th[1], nullptr, stress_sb_pusher, &c) == 0);
  CT_CHECK(pthread_create(&th[2], nullptr, stress_puller, &c) == 0);
  CT_CHECK(pthread_create(&th[3], nullptr, stress_puller_big, &c) == 0);
  CT_CHECK(pthread_create(&th[4], nullptr, stress_poller, &c) == 0);
  CT_CHECK(pthread_create(&th[5], nullptr, stress_feeder, &c) == 0);

  sleep_ms(500);
  atomic_store(&c.run, false);
  // Stop + join the audio thread while the actors are still hammering the gateway.
  ccord_voice_audio_thread_stop(t);
  for (int i = 0; i < 6; i++) pthread_join(th[i], nullptr);

  CT_CHECK(atomic_load(&c.pushes) > 0);
  CT_CHECK(atomic_load(&c.sb_pushes) > 0);
  CT_CHECK(atomic_load(&c.pulls) > 0);
  CT_CHECK(atomic_load(&c.pull_frames) > 0);
  CT_CHECK(atomic_load(&c.polls) > 0);
  CT_CHECK(atomic_load(&c.feeds) > 0);
  CT_CHECK(atomic_load(&g_fake.cap_reads) > cap_before);
  CT_CHECK(atomic_load(&g_fake.play_writes) > 0);
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_ESTABLISHED);

  opus_encoder_destroy(enc);
  ccord_voice_gateway_destroy(g);
  sfu_close(&s);
  pthread_mutex_destroy(&cap.mtx);
}

int main(void)
{
  test_thread_lifecycle_and_controls();
  test_thread_invalid_io();
  test_thread_device_selection_and_fallback();
  test_thread_pause_and_idle_controls();
  test_gateway_roundtrip();
  test_gateway_stress();
  return CT_RESULT("test_voice_audio_thread");
}
