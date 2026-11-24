// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// SpeexDSP input-processing chain (m5-voice-core). See include/ccord/net/voice_dsp.h.
// Compiles to a bit-exact passthrough when CCORD_HAVE_SPEEXDSP=0 (no speex symbols referenced).
#include <ccord/net/voice_dsp.h>

#include <string.h>

#include <ccord/core/alloc.h>

#if defined(CCORD_HAVE_SPEEXDSP) && CCORD_HAVE_SPEEXDSP
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#define VG_DSP_HAVE_SPEEX 1
#else
#define VG_DSP_HAVE_SPEEX 0
#endif

#define VG_DSP_FRAME 960u      // 20 ms at 48 kHz
#define VG_DSP_RATE 48000
#define VG_DSP_TAIL 4800       // 100 ms echo tail
#define VG_DSP_REF_FRAMES 8u   // playback-reference FIFO depth (frames)
#define VG_DSP_MAX_CH 2u
#define VG_DSP_AEC_HOLD_FRAMES 10u      // 200 ms tail hold after last non-zero playback reference
#define VG_DSP_STEREO_DIFF_THRESH 64    // ignore <= 64 LSB L/R noise floor when detecting stereo
#define VG_DSP_STEREO_MIN_SAMPLES 16u   // samples/frame above threshold required to treat input as stereo
#define VG_DSP_MONO_RETURN_FRAMES 50u   // 1 s of mono frames returns a 2-channel chain to single-channel DSP

struct ccord_voice_dsp {
  ccord_voice_config cfg;
  unsigned channels;
#if VG_DSP_HAVE_SPEEX
  SpeexPreprocessState *pre[VG_DSP_MAX_CH];
  SpeexEchoState *echo[VG_DSP_MAX_CH];
  int16_t *ref;      // lazily allocated VG_DSP_REF_FRAMES * VG_DSP_FRAME * channels when cfg.aec is true
  size_t ref_head;   // oldest buffered frame
  size_t ref_count;
  uint16_t aec_hold_frames; // remaining 20 ms frames to run AEC after non-zero playback reference
  uint16_t mono_streak;     // consecutive mono frames seen while stereo_active is true
  bool stereo_active;       // true while meaningful stereo (L != R) input is active on a 2-channel chain
#endif
};

bool ccord_dsp_available(void)
{
  return VG_DSP_HAVE_SPEEX != 0;
}

#if VG_DSP_HAVE_SPEEX
static void dsp_free_states(ccord_voice_dsp *d)
{
  for (unsigned ch = 0; ch < VG_DSP_MAX_CH; ch++) {
    if (d->pre[ch]) {
      speex_preprocess_state_destroy(d->pre[ch]);
      d->pre[ch] = nullptr;
    }
    if (d->echo[ch]) {
      speex_echo_state_destroy(d->echo[ch]);
      d->echo[ch] = nullptr;
    }
  }
  ccord_free(d->ref);
  d->ref = nullptr;
  d->ref_head = 0;
  d->ref_count = 0;
  d->aec_hold_frames = 0;
  d->mono_streak = 0;
  d->stereo_active = false;
}

static bool dsp_init_channel(ccord_voice_dsp *d, unsigned ch)
{
  if (d->pre[ch]) return true;
  d->pre[ch] = speex_preprocess_state_init((int)VG_DSP_FRAME, VG_DSP_RATE);
  if (!d->pre[ch]) return false;
  int denoise = d->cfg.ns ? 1 : 0;
  int agc = d->cfg.agc ? 1 : 0;
  int vad = 0;  // the gateway runs its own VAD/PTT gate
  (void)speex_preprocess_ctl(d->pre[ch], SPEEX_PREPROCESS_SET_DENOISE, &denoise);
  (void)speex_preprocess_ctl(d->pre[ch], SPEEX_PREPROCESS_SET_AGC, &agc);
  (void)speex_preprocess_ctl(d->pre[ch], SPEEX_PREPROCESS_SET_VAD, &vad);
  if (d->cfg.agc) {
    float level = 12000.0f;   // target amplitude (default 8000)
    int32_t max_gain = 30;    // dB
    (void)speex_preprocess_ctl(d->pre[ch], SPEEX_PREPROCESS_SET_AGC_LEVEL, &level);
    (void)speex_preprocess_ctl(d->pre[ch], SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &max_gain);
  }
  if (d->cfg.aec) {
    d->echo[ch] = speex_echo_state_init((int)VG_DSP_FRAME, VG_DSP_TAIL);
    if (!d->echo[ch]) {
      speex_preprocess_state_destroy(d->pre[ch]);
      d->pre[ch] = nullptr;
      return false;
    }
  }
  return true;
}

static void dsp_build_states(ccord_voice_dsp *d)
{
  if (!(d->cfg.ns || d->cfg.aec || d->cfg.agc)) return;
  // Allocate channel 0 eagerly; for 2-channel chains, channel 1 is created lazily only if true stereo
  // (L != R) input arrives, so mono mics duplicated to stereo run a single Speex FFT/AEC instance.
  if (!dsp_init_channel(d, 0u)) {
    dsp_free_states(d);
    return;
  }
  if (d->cfg.aec) {
    d->ref = ccord_calloc(VG_DSP_REF_FRAMES * VG_DSP_FRAME * (size_t)d->channels, sizeof(int16_t));
    if (!d->ref) {
      dsp_free_states(d);
      return;
    }
  }
}

// Pushes one interleaved playback frame; overwrites the oldest when full.
static void dsp_ref_push(ccord_voice_dsp *d, const int16_t *pcm)
{
  size_t stride = VG_DSP_FRAME * (size_t)d->channels;
  size_t slot = (d->ref_head + d->ref_count) % VG_DSP_REF_FRAMES;
  memcpy(d->ref + slot * stride, pcm, stride * sizeof(int16_t));
  if (d->ref_count < VG_DSP_REF_FRAMES) {
    d->ref_count++;
  } else {
    d->ref_head = (d->ref_head + 1u) % VG_DSP_REF_FRAMES;
  }
}

// Pops the oldest interleaved reference frame; zero-fills when none is buffered.
static void dsp_ref_pop(ccord_voice_dsp *d, int16_t *out)
{
  size_t stride = VG_DSP_FRAME * (size_t)d->channels;
  if (!d->ref || d->ref_count == 0) {
    memset(out, 0, stride * sizeof(int16_t));
    return;
  }
  memcpy(out, d->ref + d->ref_head * stride, stride * sizeof(int16_t));
  d->ref_head = (d->ref_head + 1u) % VG_DSP_REF_FRAMES;
  d->ref_count--;
}
// Detects meaningful stereo separation rather than single-sample ADC noise.
static bool dsp_frame_is_stereo(const int16_t *pcm)
{
  size_t diff_count = 0;
  for (size_t i = 0; i < VG_DSP_FRAME; i++) {
    int32_t diff = (int32_t)pcm[i * 2u] - (int32_t)pcm[i * 2u + 1u];
    if (diff < 0) diff = -diff;
    if (diff > VG_DSP_STEREO_DIFF_THRESH) {
      diff_count++;
      if (diff_count >= VG_DSP_STEREO_MIN_SAMPLES) return true;
    }
  }
  return false;
}
#endif  // VG_DSP_HAVE_SPEEX

ccord_voice_dsp *ccord_voice_dsp_create(const ccord_voice_config *cfg, unsigned channels)
{
  if (!cfg || (channels != 1u && channels != 2u)) return nullptr;
  ccord_voice_dsp *d = ccord_calloc(1, sizeof *d);
  if (!d) return nullptr;
  d->channels = channels;
  ccord_voice_dsp_configure(d, cfg);
  return d;
}

void ccord_voice_dsp_destroy(ccord_voice_dsp *d)
{
  if (!d) return;
#if VG_DSP_HAVE_SPEEX
  dsp_free_states(d);
#endif
  ccord_free(d);
}

void ccord_voice_dsp_configure(ccord_voice_dsp *d, const ccord_voice_config *cfg)
{
  if (!d) return;
  d->cfg = cfg ? *cfg : (ccord_voice_config){.vad_sensitivity_pct = 50u};
#if VG_DSP_HAVE_SPEEX
  dsp_free_states(d);
  dsp_build_states(d);
#else
  (void)d;
#endif
}

bool ccord_voice_dsp_process(ccord_voice_dsp *d, int16_t *pcm, size_t frames)
{
  if (!d || !pcm || frames != VG_DSP_FRAME) return false;
#if VG_DSP_HAVE_SPEEX
  if (!d->pre[0]) return true;  // passthrough (unavailable or all toggles off)
  // Skip Speex AEC completely when no non-zero playback reference has arrived in the last 200 ms.
  bool do_aec = d->cfg.aec && d->echo[0] != nullptr && (d->ref_count > 0 || d->aec_hold_frames > 0);
  if (d->channels == 1u) {
    if (do_aec) {
      int16_t ref_ch[VG_DSP_FRAME];
      int16_t out_ch[VG_DSP_FRAME];
      dsp_ref_pop(d, ref_ch);
      if (d->aec_hold_frames > 0) d->aec_hold_frames--;
      speex_echo_playback(d->echo[0], ref_ch);
      speex_echo_capture(d->echo[0], pcm, out_ch);
      (void)speex_preprocess_run(d->pre[0], out_ch);
      memcpy(pcm, out_ch, sizeof out_ch);
    } else {
      (void)speex_preprocess_run(d->pre[0], pcm);
    }
    return true;
  }

  // 2-channel interleaved input: require meaningful stereo separation to engage channel 1,
  // and drop back to single-channel DSP after 1 s of mono frames.
  bool is_stereo = dsp_frame_is_stereo(pcm);
  if (d->stereo_active) {
    if (is_stereo) {
      d->mono_streak = 0;
    } else {
      d->mono_streak++;
      if (d->mono_streak >= VG_DSP_MONO_RETURN_FRAMES) {
        d->stereo_active = false;
        d->mono_streak = 0;
      }
    }
  } else if (is_stereo) {
    if (dsp_init_channel(d, 1u)) {
      d->stereo_active = true;
      d->mono_streak = 0;
    }
  }

  if (!d->stereo_active) {
    int16_t mic_ch[VG_DSP_FRAME];
    for (size_t i = 0; i < VG_DSP_FRAME; i++) {
      mic_ch[i] = (int16_t)(((int32_t)pcm[i * 2u] + (int32_t)pcm[i * 2u + 1u]) / 2);
    }
    int16_t *out = mic_ch;
    int16_t out_ch[VG_DSP_FRAME];
    if (do_aec) {
      int16_t ref_frame[VG_DSP_FRAME * VG_DSP_MAX_CH];
      int16_t ref_ch[VG_DSP_FRAME];
      dsp_ref_pop(d, ref_frame);
      if (d->aec_hold_frames > 0) d->aec_hold_frames--;
      for (size_t i = 0; i < VG_DSP_FRAME; i++) {
        ref_ch[i] = (int16_t)(((int32_t)ref_frame[i * 2u] + (int32_t)ref_frame[i * 2u + 1u]) / 2);
      }
      speex_echo_playback(d->echo[0], ref_ch);
      speex_echo_capture(d->echo[0], mic_ch, out_ch);
      out = out_ch;
    }
    (void)speex_preprocess_run(d->pre[0], out);
    for (size_t i = 0; i < VG_DSP_FRAME; i++) {
      int16_t s = out[i];
      pcm[i * 2u] = s;
      pcm[i * 2u + 1u] = s;
    }
    return true;
  }

  int16_t mic_ch[VG_DSP_FRAME];
  if (do_aec) {
    int16_t ref_frame[VG_DSP_FRAME * VG_DSP_MAX_CH];
    int16_t ref_ch[VG_DSP_FRAME];
    int16_t out_ch[VG_DSP_FRAME];
    dsp_ref_pop(d, ref_frame);
    if (d->aec_hold_frames > 0) d->aec_hold_frames--;
    for (unsigned ch = 0; ch < d->channels; ch++) {
      if (!d->pre[ch]) continue;
      for (size_t i = 0; i < VG_DSP_FRAME; i++) {
        mic_ch[i] = pcm[i * d->channels + ch];
        ref_ch[i] = ref_frame[i * d->channels + ch];
      }
      if (d->echo[ch]) {
        speex_echo_playback(d->echo[ch], ref_ch);
        speex_echo_capture(d->echo[ch], mic_ch, out_ch);
      } else {
        memcpy(out_ch, mic_ch, sizeof out_ch);
      }
      (void)speex_preprocess_run(d->pre[ch], out_ch);
      for (size_t i = 0; i < VG_DSP_FRAME; i++) pcm[i * d->channels + ch] = out_ch[i];
    }
  } else {
    for (unsigned ch = 0; ch < d->channels; ch++) {
      if (!d->pre[ch]) continue;
      for (size_t i = 0; i < VG_DSP_FRAME; i++) mic_ch[i] = pcm[i * d->channels + ch];
      (void)speex_preprocess_run(d->pre[ch], mic_ch);
      for (size_t i = 0; i < VG_DSP_FRAME; i++) pcm[i * d->channels + ch] = mic_ch[i];
    }
  }
#else
  (void)d;
  (void)pcm;
#endif
  return true;
}

void ccord_voice_dsp_playback(ccord_voice_dsp *d, const int16_t *pcm, size_t frames)
{
  if (!d || !pcm || frames != VG_DSP_FRAME) return;
#if VG_DSP_HAVE_SPEEX
  if (!d->cfg.aec || !d->echo[0] || !d->ref) return;
  bool nonzero = false;
  size_t total = VG_DSP_FRAME * (size_t)d->channels;
  for (size_t i = 0; i < total; i++) {
    if (pcm[i] != 0) {
      nonzero = true;
      break;
    }
  }
  if (nonzero) {
    dsp_ref_push(d, pcm);
    d->aec_hold_frames = VG_DSP_AEC_HOLD_FRAMES;
  } else if (d->aec_hold_frames > 0) {
    // Keep feeding silent reference frames only while the 200 ms echo tail is still draining.
    dsp_ref_push(d, pcm);
  }
#else
  (void)d;
  (void)pcm;
#endif
}
