// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// SpeexDSP chain unit tests (m5-voice-core): NS/AGC/AEC effect checks, bit-exact passthrough,
// stereo handling, configure rebuilds and argument validation. Effect checks are skipped (not
// failed) when speexdsp is not linked; passthrough and validation always run.
#include <math.h>
#include <string.h>

#include <ccord/net/voice_audio.h>
#include <ccord/net/voice_dsp.h>

#include "ccord_test.h"

#define FRAME 960u
#define RATE 48000.0
static const double k_two_pi = 6.28311111111111323846;

static uint32_t lcg_state = 12345u;
static int16_t noise_sample(void)
{
  lcg_state = lcg_state * 1103515245u + 12345u;
  return (int16_t)((int32_t)(lcg_state % 12001u) - 6000);
}

// One mono test frame: stationary noise plus a quiet tone (NS), a voice-like harmonic stack (AGC),
// or a pure tone (AEC reference). `phase` advances across frames so the signal is continuous.
static void gen_frame(int16_t *out, int kind, double amp, double *phase)
{
  for (size_t i = 0; i < FRAME; i++) {
    double s = 0.0;
    if (kind == 0) {
      s = (double)noise_sample() + amp * sin(*phase);
    } else if (kind == 1) {
      double am = 0.6 + 0.4 * sin(*phase * 0.05);
      s = amp * am * (sin(*phase) + 0.5 * sin(2.0 * *phase) + 0.3 * sin(3.0 * *phase));
    } else {
      s = amp * sin(*phase);
    }
    *phase += k_two_pi * 150.0 / RATE;
    if (s > 32767.0) s = 32767.0;
    if (s < -32768.0) s = -32768.0;
    out[i] = (int16_t)s;
  }
}

static double rms_db(const int16_t *pcm, size_t n)
{
  return (double)ccord_pcm_rms_dbfs(pcm, n, 1);
}

static void test_passthrough_and_validation(void)
{
  ccord_voice_config off = {0};
  ccord_voice_dsp *d = ccord_voice_dsp_create(&off, 2);
  CT_CHECK(d != nullptr);
  if (!d) return;

  int16_t frame[FRAME * 2];
  int16_t copy[FRAME * 2];
  for (size_t i = 0; i < FRAME * 2; i++) frame[i] = (int16_t)(((i * 37u) % 20000u) - 10000u);
  memcpy(copy, frame, sizeof frame);
  CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
  CT_CHECK(memcmp(frame, copy, sizeof frame) == 0);  // all toggles off: bit-exact

  // configure(nullptr) restores defaults (all off) and stays a passthrough.
  ccord_voice_config on = {.ns = true, .aec = true, .agc = true};
  ccord_voice_dsp_configure(d, &on);
  ccord_voice_dsp_configure(d, nullptr);
  memcpy(frame, copy, sizeof frame);
  CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
  CT_CHECK(memcmp(frame, copy, sizeof frame) == 0);

  // Argument validation.
  CT_CHECK(ccord_voice_dsp_create(nullptr, 2) == nullptr);
  CT_CHECK(ccord_voice_dsp_create(&off, 0) == nullptr);
  CT_CHECK(ccord_voice_dsp_create(&off, 3) == nullptr);
  CT_CHECK(!ccord_voice_dsp_process(nullptr, frame, FRAME));
  CT_CHECK(!ccord_voice_dsp_process(d, nullptr, FRAME));
  CT_CHECK(!ccord_voice_dsp_process(d, frame, FRAME - 1u));
  CT_CHECK(!ccord_voice_dsp_process(d, frame, FRAME + 1u));
  ccord_voice_dsp_playback(nullptr, frame, FRAME);  // null-safe
  ccord_voice_dsp_playback(d, nullptr, FRAME);
  ccord_voice_dsp_playback(d, frame, FRAME - 1u);
  ccord_voice_dsp_configure(nullptr, &on);
  ccord_voice_dsp_destroy(nullptr);
  ccord_voice_dsp_destroy(d);

  // With the backend unavailable, even all-on settings must stay a bit-exact passthrough.
  if (!ccord_dsp_available()) {
    ccord_voice_dsp *p = ccord_voice_dsp_create(&on, 1);
    CT_CHECK(p != nullptr);
    if (p) {
      int16_t mono[FRAME];
      int16_t mono_copy[FRAME];
      for (size_t i = 0; i < FRAME; i++) mono[i] = (int16_t)((i * 13u) - 6000u);
      memcpy(mono_copy, mono, sizeof mono);
      CT_CHECK(ccord_voice_dsp_process(p, mono, FRAME));
      CT_CHECK(memcmp(mono, mono_copy, sizeof mono) == 0);
      ccord_voice_dsp_destroy(p);
    }
  }
}

static void test_noise_suppression(void)
{
  if (!ccord_dsp_available()) return;
  ccord_voice_config cfg = {.ns = true};
  ccord_voice_dsp *d = ccord_voice_dsp_create(&cfg, 1);
  CT_CHECK(d != nullptr);
  if (!d) return;
  int16_t frame[FRAME];
  double phase = 0.0;
  for (int f = 0; f < 120; f++) {
    gen_frame(frame, 0, 800.0, &phase);
    CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
  }
  gen_frame(frame, 0, 800.0, &phase);
  double in_db = rms_db(frame, FRAME);
  CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
  double out_db = rms_db(frame, FRAME);
  CT_CHECK(out_db <= in_db - 6.0);  // stationary noise loses at least 6 dB
  ccord_voice_dsp_destroy(d);
}

static void test_agc(void)
{
  if (!ccord_dsp_available()) return;
  ccord_voice_config cfg = {.agc = true};
  ccord_voice_dsp *d = ccord_voice_dsp_create(&cfg, 1);
  CT_CHECK(d != nullptr);
  if (!d) return;
  int16_t frame[FRAME];
  double phase = 0.0;
  for (int f = 0; f < 200; f++) {
    gen_frame(frame, 1, 1500.0, &phase);
    CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
  }
  // speex emits the previous frame (one-frame latency) and the test tone is AM, so a
  // single-frame in/out delta lands on an arbitrary AM phase. Measure the mean power
  // gain over a window spanning many AM periods instead: quiet voice must be lifted.
  double in_energy = 0.0;
  double out_energy = 0.0;
  for (int f = 0; f < 100; f++) {
    gen_frame(frame, 1, 1500.0, &phase);
    for (size_t i = 0; i < FRAME; i++) in_energy += (double)frame[i] * frame[i];
    CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
    for (size_t i = 0; i < FRAME; i++) out_energy += (double)frame[i] * frame[i];
  }
  CT_CHECK(in_energy > 0.0 && out_energy > 0.0);
  double gain_db = 10.0 * log10(out_energy / in_energy);
  CT_CHECK(gain_db >= 3.0);  // quiet voice lifted by ~1.4x on average (measured ~+4.5 dB)
  ccord_voice_dsp_destroy(d);
}

static void test_aec(void)
{
  if (!ccord_dsp_available()) return;
  ccord_voice_config cfg = {.aec = true};
  ccord_voice_dsp *d = ccord_voice_dsp_create(&cfg, 1);
  CT_CHECK(d != nullptr);
  if (!d) return;
  int16_t ref[FRAME];
  int16_t prev[FRAME];
  int16_t mic[FRAME];
  memset(prev, 0, sizeof prev);
  // Broadband noise reference; the mic carries the PREVIOUS frame's echo (speaker->mic delay,
  // which also matches speex's one-frame playback offset). A pure tone would still be partly
  // cancelled from a stale or zero reference by speex's residual suppressor, so only noise
  // proves the reference ring delivers the correct frame to the canceller.
  for (int f = 0; f < 300; f++) {
    for (size_t i = 0; i < FRAME; i++) ref[i] = noise_sample();
    for (size_t i = 0; i < FRAME; i++) mic[i] = (int16_t)((int32_t)prev[i] * 6 / 10);
    ccord_voice_dsp_playback(d, ref, FRAME);
    CT_CHECK(ccord_voice_dsp_process(d, mic, FRAME));
    memcpy(prev, ref, sizeof prev);
  }
  for (size_t i = 0; i < FRAME; i++) ref[i] = noise_sample();
  for (size_t i = 0; i < FRAME; i++) mic[i] = (int16_t)((int32_t)prev[i] * 6 / 10);
  double in_db = rms_db(mic, FRAME);
  ccord_voice_dsp_playback(d, ref, FRAME);
  CT_CHECK(ccord_voice_dsp_process(d, mic, FRAME));
  double out_db = rms_db(mic, FRAME);
  CT_CHECK(out_db <= in_db - 6.0);  // the linear echo is cancelled by at least 6 dB
  ccord_voice_dsp_destroy(d);
}

static void test_stereo_and_reconfigure(void)
{
  if (!ccord_dsp_available()) return;
  ccord_voice_config cfg = {.ns = true};
  ccord_voice_dsp *d = ccord_voice_dsp_create(&cfg, 2);
  CT_CHECK(d != nullptr);
  if (!d) return;
  int16_t frame[FRAME * 2];
  double phase = 0.0;
  for (int f = 0; f < 120; f++) {
    for (size_t i = 0; i < FRAME; i++) {
      frame[i * 2] = noise_sample();
      frame[i * 2 + 1] = noise_sample();
    }
    CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
  }
  // Both interleaved channels must be denoised (catches broken de/interleaving).
  for (size_t i = 0; i < FRAME; i++) {
    frame[i * 2] = noise_sample();
    frame[i * 2 + 1] = noise_sample();
  }
  int16_t left[FRAME];
  int16_t right[FRAME];
  for (size_t i = 0; i < FRAME; i++) {
    left[i] = frame[i * 2];
    right[i] = frame[i * 2 + 1];
  }
  double l_in = rms_db(left, FRAME);
  double r_in = rms_db(right, FRAME);
  CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
  for (size_t i = 0; i < FRAME; i++) {
    left[i] = frame[i * 2];
    right[i] = frame[i * 2 + 1];
  }
  CT_CHECK(rms_db(left, FRAME) <= l_in - 6.0);
  CT_CHECK(rms_db(right, FRAME) <= r_in - 6.0);
  (void)phase;

  // Toggling everything off rebuilds the chain into a passthrough; re-enabling NS denoises again.
  ccord_voice_dsp_configure(d, nullptr);
  for (size_t i = 0; i < FRAME * 2; i++) frame[i] = (int16_t)((i * 7u) - 8000u);
  int16_t copy[FRAME * 2];
  memcpy(copy, frame, sizeof frame);
  CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
  CT_CHECK(memcmp(frame, copy, sizeof frame) == 0);
  ccord_voice_dsp_configure(d, &cfg);
  for (int f = 0; f < 120; f++) {
    for (size_t i = 0; i < FRAME * 2; i++) frame[i] = noise_sample();
    CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
  }
  for (size_t i = 0; i < FRAME * 2; i++) frame[i] = noise_sample();
  memcpy(copy, frame, sizeof frame);
  CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
  CT_CHECK(memcmp(frame, copy, sizeof frame) != 0);  // processing is active again
  ccord_voice_dsp_destroy(d);
}

static void test_aec_idle_and_stereo_threshold(void)
{
  if (!ccord_dsp_available()) return;

  // 1. Small <=64 LSB L/R noise must NOT latch stereo_active: 2-channel chain stays single-channel
  //    and writes identical L == R output.
  ccord_voice_config ns_cfg = {.ns = true};
  ccord_voice_dsp *d = ccord_voice_dsp_create(&ns_cfg, 2);
  CT_CHECK(d != nullptr);
  if (!d) return;
  int16_t frame[FRAME * 2];
  for (int f = 0; f < 10; f++) {
    for (size_t i = 0; i < FRAME; i++) {
      int16_t s = noise_sample();
      frame[i * 2] = s;
      frame[i * 2 + 1] = (int16_t)(s + 1);
    }
    CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
  }
  bool eq = true;
  for (size_t i = 0; i < FRAME; i++) {
    if (frame[i * 2] != frame[i * 2 + 1]) {
      eq = false;
      break;
    }
  }
  CT_CHECK(eq);

  // 2. True stereo (|L - R| > 64) activates channel 1; returning to mono for 50 frames (1 s) drops
  //    back to single-channel DSP (L == R again).
  for (int f = 0; f < 5; f++) {
    for (size_t i = 0; i < FRAME; i++) {
      int16_t s = noise_sample();
      frame[i * 2] = s;
      frame[i * 2 + 1] = (int16_t)(-s);
    }
    CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
  }
  bool saw_diff = false;
  for (size_t i = 0; i < FRAME; i++) {
    if (frame[i * 2] != frame[i * 2 + 1]) {
      saw_diff = true;
      break;
    }
  }
  CT_CHECK(saw_diff);

  for (int f = 0; f < 55; f++) {
    for (size_t i = 0; i < FRAME; i++) {
      int16_t s = noise_sample();
      frame[i * 2] = s;
      frame[i * 2 + 1] = (int16_t)(s + 8);
    }
    CT_CHECK(ccord_voice_dsp_process(d, frame, FRAME));
  }
  eq = true;
  for (size_t i = 0; i < FRAME; i++) {
    if (frame[i * 2] != frame[i * 2 + 1]) {
      eq = false;
      break;
    }
  }
  CT_CHECK(eq);
  ccord_voice_dsp_destroy(d);

  // 3. AEC skips while playback is silent/idle (>200 ms) and seamlessly re-engages when non-zero
  //    playback reference resumes.
  ccord_voice_config aec_cfg = {.aec = true};
  ccord_voice_dsp *a = ccord_voice_dsp_create(&aec_cfg, 1);
  CT_CHECK(a != nullptr);
  if (!a) return;
  int16_t zero[FRAME] = {0};
  int16_t ref[FRAME], prev[FRAME], mic[FRAME];
  memset(prev, 0, sizeof prev);
  for (int f = 0; f < 25; f++) {
    ccord_voice_dsp_playback(a, zero, FRAME);
    for (size_t i = 0; i < FRAME; i++) mic[i] = noise_sample();
    CT_CHECK(ccord_voice_dsp_process(a, mic, FRAME));
  }
  for (int f = 0; f < 300; f++) {
    for (size_t i = 0; i < FRAME; i++) ref[i] = noise_sample();
    for (size_t i = 0; i < FRAME; i++) mic[i] = (int16_t)((int32_t)prev[i] * 6 / 10);
    ccord_voice_dsp_playback(a, ref, FRAME);
    CT_CHECK(ccord_voice_dsp_process(a, mic, FRAME));
    memcpy(prev, ref, sizeof prev);
  }
  for (size_t i = 0; i < FRAME; i++) ref[i] = noise_sample();
  for (size_t i = 0; i < FRAME; i++) mic[i] = (int16_t)((int32_t)prev[i] * 6 / 10);
  double in_db = rms_db(mic, FRAME);
  ccord_voice_dsp_playback(a, ref, FRAME);
  CT_CHECK(ccord_voice_dsp_process(a, mic, FRAME));
  CT_CHECK(rms_db(mic, FRAME) <= in_db - 6.0);
  ccord_voice_dsp_destroy(a);
}

int main(void)
{
  test_passthrough_and_validation();
  test_noise_suppression();
  test_agc();
  test_aec();
  test_stereo_and_reconfigure();
  test_aec_idle_and_stereo_threshold();
  return CT_RESULT("test_voice_dsp");
}
