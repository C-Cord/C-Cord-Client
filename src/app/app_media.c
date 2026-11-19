// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M5 runtime media coordinator. See app_media.h. All external effects go through the injected ops
// vtable, so this file is SDL-free and unit-testable headless. The host (main.c) owns the real
// session/store/ui/platform/renderer and supplies the ops; the coordinator only sequences them.
#include "app_media.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>

#define MEDIA_FRAME 960u                       // 20 ms at 48 kHz, matches the gateway frame size
#define MEDIA_MAX_USERS 64u                    // per-user volume sync table / voice user read cap
#define MEDIA_ID_CAP 64u                       // matches ccord_ui_voice_prefs device id fields
#define MEDIA_PKT_CAP (256u * 1024u)
#define MEDIA_CAM_MAX_BYTES (1920u * 1080u * 4u)   // largest BGRA camera frame we buffer
#define MEDIA_SB_CAP_SAMPLES (48000u * 2u * 6u)    // 6 s stereo 48 kHz cap for soundboard clips
#define MEDIA_VIDEO_KEY "__video_stream__"     // same texture the voice/stage views draw
#define MEDIA_OPEN_RETRY_MS 2000u              // backoff before retrying a failed device/codec open
#define MEDIA_CAM_AUS_PER_PUMP_TOTAL 4         // remote camera access units decoded per pump across all users
#define MEDIA_CAM_TEX_MAX_W 960                // remote camera decoder output clamp (aspect kept)
#define MEDIA_CAM_TEX_MAX_H 540
#define MEDIA_CAM_KEY_CAP 40u                  // "__cam__:<decimal id>" (== CCORD_CAM_TEXTURE_KEY_MAX)

typedef struct media_user_vol {
  ccord_id user;
  uint8_t pct;
  bool used;
} media_user_vol;

// One remote camera: a decoder per admitted sender.
typedef struct cam_slot {
  bool used;
  ccord_id user;
  uint32_t generation;
  ccord_video_decoder *decoder;
  uint64_t retry_ms;   // open backoff
  bool need_idr;       // discard non-keyframes until the first IDR decodes
  bool fresh;          // a unit decoded since the last texture upload
  int w, h;
  uint64_t frames, decode_errors, dropped_until_idr, frame_hash;
  uint8_t center_rgb[3];
  char key[MEDIA_CAM_KEY_CAP];
} cam_slot;

struct ccord_app_media {
  const ccord_app_media_ops *ops;
  void *ud;

  // settings sync
  ccord_ui_voice_prefs prefs;
  ccord_voice_config last_cfg;
  bool have_cfg;

  // per-user playback volume sync
  media_user_vol vol[MEDIA_MAX_USERS];

  // device selection (both paths). sel_* = last resolved settings ids, eff_* = ids actually used
  // (default "" when the chosen device is gone), opened_* = ids the inline handles were last tried
  // with, thread_* = ids the live audio pthread was last started with.
  char sel_in[MEDIA_ID_CAP], sel_out[MEDIA_ID_CAP];
  char eff_in[MEDIA_ID_CAP], eff_out[MEDIA_ID_CAP];
  char opened_in[MEDIA_ID_CAP], opened_out[MEDIA_ID_CAP];
  char thread_in[MEDIA_ID_CAP], thread_out[MEDIA_ID_CAP];
  char opened_cam[MEDIA_ID_CAP];
  bool resolved;
  bool devices_dirty;

  // audio
  ccord_audio *audio_cap;
  ccord_audio *audio_play;
  ccord_voice_audio_thread *thread;

  // local soundboard playback ring (shared between main thread decode and live/inline speaker pull)
  ccord_mutex *sb_mtx;
  int16_t *sb_pcm;
  size_t sb_samples;
  size_t sb_pos;
  uint64_t sb_next_ms;

  // Go Live send
  ccord_video_stream *video;
  ccord_capture *screen_cap;
  ccord_camera *camera;
  uint8_t *cam_buf;
  uint8_t *pkt;
  uint64_t last_video_ms;
  uint64_t stream_start_ms;
  bool streaming;
  bool was_paused;
  bool pending_keyframe;
  _Atomic bool paused_flag;

  // Go Live desktop-audio loopback ("share audio"): while enabled, the broadcast carries the
  // monitor mix instead of the mic downmix. The handle is main-thread only; loopback_flag tells
  // the audio pthread's mic->stream path to stand down (same cross-thread class as paused_flag).
  ccord_loopback_audio *loopback;
  uint64_t loopback_retry_ms;
  uint64_t loopback_next_ms;
  _Atomic bool loopback_enabled;   // coordinator-level switch (ccord_app_media_set_loopback_audio)
  _Atomic bool loopback_flag;      // effective this pump: stream_on && (setter || host read seam)

  // Standalone voice camera TX (self_video while not Go Live streaming)
  ccord_video_stream *cam_video;
  uint64_t cam_start_ms;
  uint64_t last_cam_ms;
  uint64_t cam_enc_retry_ms;
  bool cam_streaming;
  bool cam_pending_keyframe;
  bool warn_cam_encoder, warn_cam_encode, warn_cam_send;

  // Go Live receive
  ccord_video_decoder *decoder;
  uint64_t stream_next_ms;   // 20 ms playout pace when stream audio runs without remote voice
  bool recv_fresh;   // a packet decoded cleanly since the last stage texture upload
  bool recv_need_idr;

  // Remote voice cameras (one decoder per admitted sender, at most CCORD_CAM_DECODE_MAX)
  cam_slot cams[CCORD_CAM_RX_MAX];
  size_t cam_rr;   // rotating start index for the per-pump AU budget

  // Failed-open backoff: retry no sooner than these timestamps (0 = try now). Prevents per-pump
  // open churn and repeated warning logs when a device/encoder/camera is unavailable.
  uint64_t audio_cap_retry_ms, audio_play_retry_ms, thread_retry_ms;
  uint64_t camera_retry_ms, encoder_retry_ms, capture_retry_ms, decoder_retry_ms;

  // Per-stream Go Live warning latches: each send-plane error class logs one CCORD_LOG_W per
  // stream (reset when the stream stops or restarts) so live failures are visible on stderr
  // without per-frame spam. warn_send_pcm may be set from the audio pthread (same plain-bool
  // cross-thread class as the PTT gate); the rest are main-thread only.
  bool warn_capture, warn_camera, warn_encoder, warn_encode, warn_send, warn_loopback;
  _Atomic bool warn_send_pcm;
  // The user cancelled the portal picker for this stream: offer the X11 default instead of
  // re-prompting on every retry. Reset with the stream.
  bool cap_portal_refused;
};

// ---- small helpers --------------------------------------------------------------------------

static int clamp_vol(int v)
{
  if (v < 0) return 0;
  if (v > 200) return 200;
  return v;
}

static uint32_t clamp_sens(int v)
{
  if (v < 0) return 0u;
  if (v > 100) return 100u;
  return (uint32_t)v;
}

// Saturating gain on interleaved S16, in place.
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

// Downmix interleaved stereo S16 to mono by averaging the two channels. Go Live stream audio is
// 48 kHz mono, while the mic capture / playback paths are stereo.
static void downmix_mono(const int16_t *stereo, size_t frames, int16_t *mono)
{
  for (size_t i = 0; i < frames; i++) {
    mono[i] = (int16_t)(((int32_t)stereo[i * 2] + (int32_t)stereo[i * 2 + 1]) / 2);
  }
}

static bool cfg_equal(const ccord_voice_config *a, const ccord_voice_config *b)
{
  return a->ptt == b->ptt && a->vad_sensitivity_pct == b->vad_sensitivity_pct && a->ns == b->ns &&
         a->aec == b->aec && a->agc == b->agc;
}

// True when a failed open may be retried now; on failure the caller defers the next attempt by
// MEDIA_OPEN_RETRY_MS so an unavailable device/codec is not reopened (and re-logged) every pump.
static bool open_retry_due(uint64_t now, uint64_t retry_ms)
{
  return now >= retry_ms;
}

static void copy_id(char *dst, const char *src)
{
  size_t n = strlen(src);
  if (n >= MEDIA_ID_CAP) n = MEDIA_ID_CAP - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// ---- soundboard local playback + audio pthread io -------------------------------------------

static void clear_soundboard(ccord_app_media *m)
{
  ccord_mutex_lock(m->sb_mtx);
  ccord_free(m->sb_pcm);
  m->sb_pcm = nullptr;
  m->sb_samples = 0;
  m->sb_pos = 0;
  m->sb_next_ms = 0;
  ccord_mutex_unlock(m->sb_mtx);
}

static void discard_pending_soundboard(ccord_app_media *m)
{
  const uint8_t *data = nullptr;
  size_t len = 0;
  uint8_t vol = 100u;
  while (m->ops->pop_soundboard(m->ud, &data, &len, &vol)) {}
}

// Outbound Go Live stream audio: the captured mic frame (already gained) is downmixed to mono and
// pushed to the active broadcast. This is a separate path from the PTT-gated voice mic push, so a
// stream keeps its audio even when push-to-talk is not held; it still follows the mic mute gate
// because it reuses the same captured frame. No-op when the host has no stream-audio seam, no
// active outbound stream, or desktop-audio loopback owns the stream audio (pump_stream_loopback).
static void pump_stream_send_pcm(ccord_app_media *m, const int16_t *stereo, size_t frames)
{
  if (!m->ops->stream_send_pcm || !m->ops->stream_active(m->ud) || atomic_load(&m->paused_flag)) return;
  if (atomic_load(&m->loopback_flag)) return;   // loopback-only TX: the mic downmix stands down
  int16_t mono[MEDIA_FRAME];
  size_t n = frames < MEDIA_FRAME ? frames : MEDIA_FRAME;
  downmix_mono(stereo, n, mono);
  ccord_error err;
  if (m->ops->stream_send_pcm(m->ud, mono, n, &err) != CCORD_OK) {
    if (!atomic_exchange(&m->warn_send_pcm, true)) {
      CCORD_LOG_W("media: stream audio send failed: %s", err.message);
    }
  }
}

// Outbound Go Live desktop audio ("share audio"): while loopback is enabled the broadcast carries
// the desktop monitor mix instead of the mic downmix. Runs on the main thread only (the loopback
// handle is single-threaded); the audio pthread's mic->stream path is suppressed via loopback_flag
// while mic voice TX (push_pcm) continues untouched. Paced at 20 ms per 960-sample frame so a 60 Hz
// UI pump sustains the full 50 frames/sec rate, and gated by stream_paused; an unavailable monitor
// fails soft with retry backoff.
static void pump_stream_loopback(ccord_app_media *m, uint64_t now, bool loop_on, bool paused)
{
  if (!loop_on) {
    if (m->loopback) {
      m->ops->loopback_close(m->ud, m->loopback);
      m->loopback = nullptr;
    }
    m->loopback_retry_ms = 0;
    m->loopback_next_ms = 0;
    return;
  }
  if (paused || !m->ops->stream_send_pcm) {
    m->loopback_next_ms = 0;
    return;   // keep the handle across a pause; send immediately on resume
  }
  if (!m->ops->loopback_open || !m->ops->loopback_read || !m->ops->loopback_close) return;
  if (!m->loopback && open_retry_due(now, m->loopback_retry_ms)) {
    ccord_error err;
    m->loopback = m->ops->loopback_open(m->ud, &err);
    if (!m->loopback) {
      m->loopback_retry_ms = now + MEDIA_OPEN_RETRY_MS;
      if (!m->warn_loopback) {
        m->warn_loopback = true;
        CCORD_LOG_W("media: loopback audio open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS,
                    err.message);
      }
      return;
    }
    m->loopback_retry_ms = 0;
  }
  if (!m->loopback) return;
  if (m->ops->loopback_failed && m->ops->loopback_failed(m->ud, m->loopback)) {
    m->ops->loopback_close(m->ud, m->loopback);
    m->loopback = nullptr;
    m->loopback_retry_ms = now + MEDIA_OPEN_RETRY_MS;
    m->loopback_next_ms = 0;
    if (!m->warn_loopback) {
      m->warn_loopback = true;
      CCORD_LOG_W("media: loopback audio source died (retry in %u ms)", MEDIA_OPEN_RETRY_MS);
    }
    return;
  }
  if (now < m->loopback_next_ms) return;
  int16_t mono[MEDIA_FRAME];
  size_t n = m->ops->loopback_read(m->ud, m->loopback, mono, MEDIA_FRAME);
  if (n > MEDIA_FRAME) n = MEDIA_FRAME;
  if (n == 0) return;
  uint64_t step_ms = ((uint64_t)n * 1000u) / 48000u;
  if (step_ms == 0) step_ms = 1u;
  m->loopback_next_ms = m->loopback_next_ms == 0 ? now + step_ms : m->loopback_next_ms + step_ms;
  if (m->loopback_next_ms <= now) m->loopback_next_ms = now + step_ms;
  ccord_error err;
  if (m->ops->stream_send_pcm(m->ud, mono, n, &err) != CCORD_OK) {
    if (!atomic_exchange(&m->warn_send_pcm, true)) {
      CCORD_LOG_W("media: stream audio send failed: %s", err.message);
    }
  }
}

// Pulls remote voice RX frames and mixes in any locally decoded soundboard clip (20 ms paced when
// no remote voice frames are arriving). Used by both the live audio pthread and inline playback so
// soundboard clips never enter the PTT-gated mic path and never double-transmit.
static size_t pull_playback_pcm(ccord_app_media *m, int16_t *pcm, size_t max_frames)
{
  size_t pframes = m->ops->pull_pcm(m->ud, pcm, max_frames);
  ccord_mutex_lock(m->sb_mtx);
  if (m->sb_pcm && m->sb_pos < m->sb_samples) {
    size_t avail_frames = (m->sb_samples - m->sb_pos) / 2u;
    if (pframes > 0) {
      size_t mix = avail_frames < pframes ? avail_frames : pframes;
      for (size_t i = 0; i < mix * 2u; i++) {
        int32_t s = (int32_t)pcm[i] + (int32_t)m->sb_pcm[m->sb_pos + i];
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        pcm[i] = (int16_t)s;
      }
      m->sb_pos += mix * 2u;
      uint64_t now = m->ops->now_ms(m->ud);
      uint64_t step_ms = ((uint64_t)mix * 1000u) / 48000u;
      if (step_ms == 0) step_ms = 1u;
      m->sb_next_ms = now + step_ms;
    } else {
      uint64_t now = m->ops->now_ms(m->ud);
      if (now >= m->sb_next_ms) {
        size_t cap_frames = max_frames < MEDIA_FRAME ? max_frames : MEDIA_FRAME;
        size_t take = avail_frames < cap_frames ? avail_frames : cap_frames;
        if (take > 0) {
          memcpy(pcm, m->sb_pcm + m->sb_pos, take * 2u * sizeof(int16_t));
          m->sb_pos += take * 2u;
          uint64_t step_ms = ((uint64_t)take * 1000u) / 48000u;
          if (step_ms == 0) step_ms = 1u;
          m->sb_next_ms += step_ms;
          if (m->sb_next_ms < now) m->sb_next_ms = now + step_ms;
          pframes = take;
        }
      }
    }
    if (m->sb_pos >= m->sb_samples) {
      ccord_free(m->sb_pcm);
      m->sb_pcm = nullptr;
      m->sb_samples = 0;
      m->sb_pos = 0;
    }
  }
  ccord_mutex_unlock(m->sb_mtx);

  // Go Live stream audio: drain the watched broadcast's mono PCM and mix it into local playback,
  // duplicating each mono sample to both stereo channels with additive int16 clamping. When no
  // remote voice frame arrived this pump the stream audio plays on its own. The session/transport
  // gate the poll on an active watched stream; the explicit watch check keeps the mix honest when
  // the host poll seam would otherwise return samples.
  if (m->ops->stream_poll_pcm && m->ops->watch_active(m->ud)) {
    uint64_t now = m->ops->now_ms(m->ud);
    size_t cap_frames = max_frames < MEDIA_FRAME ? max_frames : MEDIA_FRAME;
    if (pframes > 0) {
      int16_t mono[MEDIA_FRAME];
      size_t want = pframes < cap_frames ? pframes : cap_frames;
      size_t ns = m->ops->stream_poll_pcm(m->ud, mono, want);
      for (size_t i = 0; i < ns; i++) {
        int32_t l = (int32_t)pcm[i * 2] + (int32_t)mono[i];
        int32_t r = (int32_t)pcm[i * 2 + 1] + (int32_t)mono[i];
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        pcm[i * 2] = (int16_t)l;
        pcm[i * 2 + 1] = (int16_t)r;
      }
      uint64_t step_ms = ((uint64_t)pframes * 1000u) / 48000u;
      if (step_ms == 0) step_ms = 1u;
      m->stream_next_ms = now + step_ms;
    } else if (now >= m->stream_next_ms) {
      int16_t mono[MEDIA_FRAME];
      size_t ns = m->ops->stream_poll_pcm(m->ud, mono, cap_frames);
      if (ns > 0) {
        for (size_t i = 0; i < ns; i++) {
          pcm[i * 2] = mono[i];
          pcm[i * 2 + 1] = mono[i];
        }
        uint64_t step_ms = ((uint64_t)ns * 1000u) / 48000u;
        if (step_ms == 0) step_ms = 1u;
        m->stream_next_ms += step_ms;
        if (m->stream_next_ms < now) m->stream_next_ms = now + step_ms;
        pframes = ns;
      }
    }
  } else {
    m->stream_next_ms = 0;
  }
  return pframes;
}

static void pump_soundboard(ccord_app_media *m, uint64_t now)
{
  const uint8_t *data = nullptr;
  size_t len = 0;
  uint8_t vol_pct = 100u;
  if (!m->ops->pop_soundboard(m->ud, &data, &len, &vol_pct) || !data || len == 0) return;

  int16_t *buf = ccord_malloc(MEDIA_SB_CAP_SAMPLES * sizeof(int16_t));
  if (!buf) return;
  ccord_error err;
  size_t samples = 0;
  if (m->ops->sound_decode(m->ud, data, len, buf, MEDIA_SB_CAP_SAMPLES, &samples, &err) != CCORD_OK ||
      (samples & ~(size_t)1u) == 0) {
    CCORD_LOG_W("media: soundboard decode failed: %s", err.message);
    ccord_free(buf);
    return;
  }
  samples &= ~(size_t)1u;
  apply_gain(buf, samples, clamp_vol((int)vol_pct));

  ccord_mutex_lock(m->sb_mtx);
  ccord_free(m->sb_pcm);
  m->sb_pcm = buf;
  m->sb_samples = samples;
  m->sb_pos = 0;
  m->sb_next_ms = now;
  ccord_mutex_unlock(m->sb_mtx);
}

ccord_status ccord_app_media_play_soundboard(ccord_app_media *m, ccord_id guild_id, ccord_id sound_id,
                                             ccord_error *err)
{
  if (!m) return ccord_error_set(err, CCORD_ERR_INVALID, "app_media: null media");
  return m->ops->play_soundboard(m->ud, guild_id, sound_id, err);
}

void ccord_app_media_set_loopback_audio(ccord_app_media *m, bool on)
{
  if (m) atomic_store(&m->loopback_enabled, on);
}

bool ccord_app_media_loopback_audio(const ccord_app_media *m)
{
  return m ? atomic_load(&m->loopback_enabled) : false;
}

static void media_hotplug(void *ud, bool capture, bool added)
{
  ccord_app_media *m = ud;
  (void)capture;
  (void)added;
  if (m) m->devices_dirty = true;   // resolved on the next pump (main thread, same as this callback)
}

static ccord_status media_io_push(void *ud, const int16_t *pcm, size_t frames, ccord_error *err)
{
  ccord_app_media *m = ud;
  ccord_status st = m->ops->push_pcm(m->ud, pcm, frames, err);
  pump_stream_send_pcm(m, pcm, frames);
  return st;
}

static size_t media_io_pull(void *ud, int16_t *pcm, size_t max_frames)
{
  ccord_app_media *m = ud;
  return pull_playback_pcm(m, pcm, max_frames);
}

// ---- device selection ------------------------------------------------------------------------

// True when `id` is usable: "" means the system default, otherwise it must appear in the enumeration.
static bool device_present(ccord_app_media *m, bool capture, const char *id)
{
  if (id[0] == '\0') return true;
  ccord_audio_device_info list[MEDIA_MAX_USERS];
  size_t n = m->ops->devices_list(m->ud, capture, list, MEDIA_MAX_USERS);
  for (size_t i = 0; i < n; i++) {
    if (strcmp(list[i].id, id) == 0) return true;
  }
  return false;
}

// Re-enumerate and resolve the effective device ids, falling back to the default when the chosen
// device vanished. Runs only on a settings change or hot-plug, not every frame.
static void resolve_devices(ccord_app_media *m)
{
  copy_id(m->sel_in, m->prefs.input_device_id);
  copy_id(m->sel_out, m->prefs.output_device_id);
  if (device_present(m, true, m->sel_in)) copy_id(m->eff_in, m->sel_in);
  else m->eff_in[0] = '\0';
  if (device_present(m, false, m->sel_out)) copy_id(m->eff_out, m->sel_out);
  else m->eff_out[0] = '\0';
  m->resolved = true;
  m->devices_dirty = false;
}

static void close_inline(ccord_app_media *m)
{
  if (m->audio_cap) {
    m->ops->audio_close(m->ud, m->audio_cap);
    m->audio_cap = nullptr;
  }
  if (m->audio_play) {
    m->ops->audio_close(m->ud, m->audio_play);
    m->audio_play = nullptr;
  }
  m->opened_in[0] = '\0';
  m->opened_out[0] = '\0';
}

static void stop_thread(ccord_app_media *m)
{
  if (m->thread) {
    m->ops->thread_stop(m->ud, m->thread);
    m->thread = nullptr;
  }
  m->thread_in[0] = '\0';
  m->thread_out[0] = '\0';
}

// Re-resolve the effective device ids when settings changed or a hot-plug event arrived. Shared by
// the inline and the live (pthread) paths so both honor selection + default fallback.
static void maybe_resolve_devices(ccord_app_media *m)
{
  bool need_resolve = m->devices_dirty || !m->resolved ||
                      strcmp(m->prefs.input_device_id, m->sel_in) != 0 ||
                      strcmp(m->prefs.output_device_id, m->sel_out) != 0;
  if (need_resolve) resolve_devices(m);
}

// Start/restart the live audio pthread when effective device ids change, with retry backoff.
static void sync_live_thread(ccord_app_media *m, const ccord_app_media_voice *v, int in_vol, int out_vol,
                             uint64_t now)
{
  maybe_resolve_devices(m);
  bool changed = strcmp(m->eff_in, m->thread_in) != 0 || strcmp(m->eff_out, m->thread_out) != 0;
  if (changed) {
    stop_thread(m);
    m->thread_retry_ms = 0;
  }
  if (!m->thread && open_retry_due(now, m->thread_retry_ms)) {
    ccord_error err;
    ccord_voice_audio_io io = {.push = media_io_push, .pull = media_io_pull, .ud = m};
    copy_id(io.input_device_id, m->eff_in);
    copy_id(io.output_device_id, m->eff_out);
    copy_id(m->thread_in, m->eff_in);
    copy_id(m->thread_out, m->eff_out);
    m->thread = m->ops->thread_start(m->ud, &io, &err);
    if (m->thread) {
      m->thread_retry_ms = 0;
    } else {
      m->thread_retry_ms = now + MEDIA_OPEN_RETRY_MS;
      CCORD_LOG_W("media: audio thread start failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
    }
  }
  if (m->thread) {
    m->ops->thread_controls(m->ud, m->thread, v->muted, v->deafened, in_vol, out_vol);
  }
}

// Open/reopen the inline capture+playback handles when the effective device ids change.
static void sync_devices(ccord_app_media *m, uint64_t now)
{
  maybe_resolve_devices(m);
  ccord_error err;
  bool cap_changed = strcmp(m->eff_in, m->opened_in) != 0;
  if (cap_changed) m->audio_cap_retry_ms = 0;
  if (!m->audio_cap || cap_changed) {
    if (open_retry_due(now, m->audio_cap_retry_ms)) {
      if (m->audio_cap) m->ops->audio_close(m->ud, m->audio_cap);
      m->audio_cap = m->ops->audio_open(m->ud, true, m->eff_in, &err);
      copy_id(m->opened_in, m->eff_in);
      if (m->audio_cap) {
        m->audio_cap_retry_ms = 0;
      } else {
        m->audio_cap_retry_ms = now + MEDIA_OPEN_RETRY_MS;
        CCORD_LOG_W("media: capture open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
      }
    }
  }
  bool play_changed = strcmp(m->eff_out, m->opened_out) != 0;
  if (play_changed) m->audio_play_retry_ms = 0;
  if (!m->audio_play || play_changed) {
    if (open_retry_due(now, m->audio_play_retry_ms)) {
      if (m->audio_play) m->ops->audio_close(m->ud, m->audio_play);
      m->audio_play = m->ops->audio_open(m->ud, false, m->eff_out, &err);
      copy_id(m->opened_out, m->eff_out);
      if (m->audio_play) {
        m->audio_play_retry_ms = 0;
      } else {
        m->audio_play_retry_ms = now + MEDIA_OPEN_RETRY_MS;
        CCORD_LOG_W("media: playback open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
      }
    }
  }
}

static void pump_inline(ccord_app_media *m, bool muted, bool deafened, int in_vol, int out_vol)
{
  ccord_error err;
  int16_t pcm[MEDIA_FRAME * 2];
  if (m->audio_cap && !muted && in_vol > 0) {
    size_t rframes = m->ops->audio_read(m->ud, m->audio_cap, pcm, MEDIA_FRAME);
    if (rframes > 0) {
      apply_gain(pcm, rframes * 2, in_vol);
      (void)m->ops->push_pcm(m->ud, pcm, rframes, &err);
      pump_stream_send_pcm(m, pcm, rframes);
    }
  }
  if (m->audio_play && !deafened && out_vol > 0) {
    size_t pframes = pull_playback_pcm(m, pcm, MEDIA_FRAME);
    if (pframes > 0) {
      apply_gain(pcm, pframes * 2, out_vol);
      (void)m->ops->audio_write(m->ud, m->audio_play, pcm, pframes);
    }
  }
}

// ---- settings sync ---------------------------------------------------------------------------

static void sync_config(ccord_app_media *m)
{
  bool dsp = m->ops->dsp_available(m->ud);
  ccord_voice_config cfg = {
    .ptt = m->prefs.ptt,
    .vad_sensitivity_pct = clamp_sens(m->prefs.vad_sensitivity),
    .ns = m->prefs.noise_suppression && dsp,
    .aec = m->prefs.echo_cancel && dsp,
    .agc = m->prefs.auto_gain && dsp,
  };
  if (!m->have_cfg || !cfg_equal(&cfg, &m->last_cfg)) {
    ccord_error err;
    if (m->ops->set_voice_config(m->ud, &cfg, &err) == CCORD_OK) {
      m->last_cfg = cfg;
      m->have_cfg = true;
    }
  }
}

bool ccord_app_media_ptt_open(const ccord_app_media_ops *ops, void *ud, const ccord_ui_voice_prefs *prefs)
{
  if (!ops || !prefs) return false;
  if (!prefs->ptt) return true;   // voice activity mode: the gate is always open
  // Focused key state, or the portal shortcut (the only source while another window has focus).
  return (prefs->ptt_scancode > 0 && ops->key_held && ops->key_held(ud, prefs->ptt_scancode)) ||
         (ops->global_ptt_held && ops->global_ptt_held(ud));
}

static void sync_ptt(ccord_app_media *m)
{
  // Per-frame key state: in PTT mode the mic is open only while the bound key (or the portal's push-to-talk
  // shortcut) is held. The session applies the gate on push_voice_pcm.
  bool held = ccord_app_media_ptt_open(m->ops, m->ud, &m->prefs);
  ccord_error err;
  (void)m->ops->set_ptt_held(m->ud, held, &err);
}

static void sync_volumes(ccord_app_media *m)
{
  ccord_id users[MEDIA_MAX_USERS];
  size_t n = m->ops->voice_users(m->ud, users, MEDIA_MAX_USERS);
  // Evict departed users first so a full 64-slot table frees slots immediately on member rotation.
  for (size_t j = 0; j < MEDIA_MAX_USERS; j++) {
    if (!m->vol[j].used) continue;
    bool present = false;
    for (size_t i = 0; i < n; i++) {
      if (users[i] == m->vol[j].user) {
        present = true;
        break;
      }
    }
    if (!present) m->vol[j].used = false;
  }
  for (size_t i = 0; i < n; i++) {
    ccord_id u = users[i];
    if (u == CCORD_ID_NONE) continue;
    uint8_t pct = m->ops->user_volume(m->ud, u);
    size_t slot = MEDIA_MAX_USERS;
    for (size_t j = 0; j < MEDIA_MAX_USERS; j++) {
      if (m->vol[j].used && m->vol[j].user == u) {
        slot = j;
        break;
      }
      if (!m->vol[j].used && slot == MEDIA_MAX_USERS) slot = j;
    }
    if (slot == MEDIA_MAX_USERS) continue;   // table full
    if (m->vol[slot].used && m->vol[slot].user == u && m->vol[slot].pct == pct) continue;
    ccord_error err;
    if (m->ops->set_user_volume(m->ud, u, pct, &err) == CCORD_OK) {
      m->vol[slot].user = u;
      m->vol[slot].pct = pct;
      m->vol[slot].used = true;
    }
  }
}

static void reset_sync(ccord_app_media *m)
{
  m->have_cfg = false;
  m->resolved = false;
  m->audio_cap_retry_ms = 0;
  m->audio_play_retry_ms = 0;
  m->thread_retry_ms = 0;
  for (size_t i = 0; i < MEDIA_MAX_USERS; i++) m->vol[i].used = false;
}

// ---- Go Live send / receive ------------------------------------------------------------------

// Clears the per-stream send-plane warning latches so a new stream reports its first failures
// again. Idempotent; also run on the idle pump to cover a PCM latch set from the audio pthread
// before the send plane ever opened.
static void reset_stream_warns(ccord_app_media *m)
{
  m->warn_capture = false;
  m->warn_camera = false;
  m->warn_encoder = false;
  m->warn_encode = false;
  m->warn_send = false;
  atomic_store(&m->warn_send_pcm, false);
  m->warn_loopback = false;
}

static void close_cam_tx(ccord_app_media *m, bool close_cam)
{
  if (m->cam_video) {
    m->ops->video_close(m->ud, m->cam_video);
    m->cam_video = nullptr;
  }
  if (close_cam) {
    if (m->camera) {
      m->ops->camera_close(m->ud, m->camera);
      m->camera = nullptr;
    }
    m->opened_cam[0] = '\0';
    ccord_free(m->cam_buf);
    m->cam_buf = nullptr;
    m->camera_retry_ms = 0;
    m->warn_camera = false;
  }
  m->cam_streaming = false;
  m->last_cam_ms = 0;
  m->cam_enc_retry_ms = 0;
  m->cam_pending_keyframe = false;
  m->warn_cam_encoder = false;
  m->warn_cam_encode = false;
  m->warn_cam_send = false;
}

static void close_send(ccord_app_media *m)
{
  if (m->screen_cap) {
    m->ops->capture_close(m->ud, m->screen_cap);
    m->screen_cap = nullptr;
  }
  if (m->loopback) {
    m->ops->loopback_close(m->ud, m->loopback);
    m->loopback = nullptr;
  }
  m->loopback_retry_ms = 0;
  m->loopback_next_ms = 0;
  if (m->camera) {
    m->ops->camera_close(m->ud, m->camera);
    m->camera = nullptr;
  }
  m->opened_cam[0] = '\0';
  ccord_free(m->cam_buf);
  m->cam_buf = nullptr;
  if (m->video) {
    m->ops->video_close(m->ud, m->video);
    m->video = nullptr;
  }
  m->last_video_ms = 0;
  m->streaming = false;
  m->was_paused = false;
  m->pending_keyframe = false;
  m->encoder_retry_ms = 0;
  m->camera_retry_ms = 0;
  m->capture_retry_ms = 0;
  m->cap_portal_refused = false;
  reset_stream_warns(m);
}

static void open_encoder(ccord_app_media *m, bool virtual_clock, uint64_t now)
{
  if (!open_retry_due(now, m->encoder_retry_ms)) return;
  ccord_stream_config sc;
  m->ops->stream_config(m->ud, &sc);
  int target_h = sc.resolution == CCORD_STREAM_RES_1080P   ? 1080
                 : sc.resolution == CCORD_STREAM_RES_1440P ? 1440
                 : sc.resolution == CCORD_STREAM_RES_SOURCE ? 0
                                                            : 720;
  int vw = 640, vh = 360;
  m->ops->resolve_dims(m->ud, 1280, 720, target_h, &vw, &vh);
  int vfps = sc.fps == CCORD_STREAM_FPS_15 ? 15 : sc.fps == CCORD_STREAM_FPS_60 ? 60 : 30;
  const char *pref_enc = sc.encoder == CCORD_STREAM_ENCODER_NVENC          ? "h264_nvenc"
                         : sc.encoder == CCORD_STREAM_ENCODER_VAAPI        ? "h264_vaapi"
                         : sc.encoder == CCORD_STREAM_ENCODER_AMF          ? "h264_amf"
                         : sc.encoder == CCORD_STREAM_ENCODER_QSV          ? "h264_qsv"
                         : sc.encoder == CCORD_STREAM_ENCODER_MF           ? "h264_mf"
                         : sc.encoder == CCORD_STREAM_ENCODER_VIDEOTOOLBOX ? "h264_videotoolbox"
                         : sc.encoder == CCORD_STREAM_ENCODER_X264         ? "libx264"
                         : sc.encoder == CCORD_STREAM_ENCODER_OPENH264     ? "libopenh264"
                                                                           : "auto";
  if (virtual_clock && (vw > 640 || vh > 360)) {
    vw = 640;
    vh = 360;
  }
  ccord_video_config vcfg = {.width = vw,
                             .height = vh,
                             .fps = vfps,
                             .bitrate = 2000000,
                             // The Go Live wire format is H.264 Annex-B: HEVC/AV1 stream
                             // selections fall back to H.264 (the platform encodes H.264/MPEG4).
                             .codec = CCORD_VIDEO_CODEC_H264,
                             // Probe PipeWire frames only for the portal/auto capture frameworks;
                             // an explicit X11/GPU framework stays on software frames.
                             .use_pipewire = sc.framework == CCORD_STREAM_FRAMEWORK_PIPEWIRE ||
                                             sc.framework == CCORD_STREAM_FRAMEWORK_AUTO,
                             .preferred_encoder = pref_enc};
  ccord_error err;
  m->video = m->ops->video_open(m->ud, &vcfg, &err);
  if (!m->video && strcmp(pref_enc, "libx264") != 0) {
    // Requested hardware/auto encoder unavailable: fall back to software H.264 right away so
    // the stream still encodes and updates the preview instead of staying dark.
    vcfg.preferred_encoder = "libx264";
    m->video = m->ops->video_open(m->ud, &vcfg, &err);
    if (m->video && !m->warn_encoder) {
      m->warn_encoder = true;
      CCORD_LOG_W("media: encoder %s unavailable, fell back to software H.264 (libx264)", pref_enc);
    }
  }
  if (m->video) {
    m->encoder_retry_ms = 0;
  } else {
    m->encoder_retry_ms = now + MEDIA_OPEN_RETRY_MS;
    if (!m->warn_encoder) {
      m->warn_encoder = true;
      CCORD_LOG_W("media: encoder open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
    }
  }
}

// Default Go Live capture source when the user starts a stream without picking a source card:
// the portal system picker (Wayland, or any host where xdg-desktop-portal answers) first, then
// the primary X11 screen. Both go through the host capture seam; the backends fail cleanly when
// unavailable, so the coordinator never probes the platform itself. The X11 screen source opens
// the root window, which capture_x11 treats as the primary screen.
static void fill_default_capture_source(ccord_capture_source *src, bool portal)
{
  memset(src, 0, sizeof *src);
  src->kind = CCORD_CAPTURE_SCREEN;
  src->primary = true;
#if CCORD_OS_WINDOWS
  (void)portal;
  src->backend = CCORD_CAPBE_WIN32;
  snprintf(src->id, sizeof src->id, "win32:mon:primary");
  snprintf(src->title, sizeof src->title, "Screen 1 (primary)");
#elif CCORD_OS_MACOS
  (void)portal;
  src->backend = CCORD_CAPBE_MACOS;
  snprintf(src->id, sizeof src->id, "macos:mon:main");
  snprintf(src->title, sizeof src->title, "Main Display");
#elif CCORD_OS_ANDROID
  (void)portal;
  src->backend = CCORD_CAPBE_ANDROID;
  snprintf(src->id, sizeof src->id, "android:screen");
  snprintf(src->title, sizeof src->title, "Android Screen (MediaProjection)");
#else
  if (portal) {
    src->backend = CCORD_CAPBE_PORTAL;
    snprintf(src->id, sizeof src->id, "portal:pick");
    snprintf(src->title, sizeof src->title, "Wayland Screen (System Picker)");
    snprintf(src->app, sizeof src->app, "xdg-desktop-portal");
  } else {
    src->backend = CCORD_CAPBE_X11;
    snprintf(src->id, sizeof src->id, "x11:mon:root");
    snprintf(src->title, sizeof src->title, "Screen 1 (primary)");
  }
#endif
}

static void grab_camera_frame(ccord_app_media *m, uint64_t now, const uint8_t **out_bgra, int *out_w, int *out_h,
                              int *out_stride)
{
  *out_bgra = nullptr;
  *out_w = 0;
  *out_h = 0;
  *out_stride = 0;
  bool cam_changed = strcmp(m->prefs.camera_device_id, m->opened_cam) != 0;
  if (cam_changed) m->camera_retry_ms = 0;
  if ((!m->camera || cam_changed) && open_retry_due(now, m->camera_retry_ms)) {
    if (m->camera) m->ops->camera_close(m->ud, m->camera);
    ccord_error err;
    m->camera = m->ops->camera_open(m->ud, m->prefs.camera_device_id, &err);
    copy_id(m->opened_cam, m->prefs.camera_device_id);
    if (m->camera && !m->cam_buf) m->cam_buf = ccord_malloc(MEDIA_CAM_MAX_BYTES);
    if (m->camera) {
      m->camera_retry_ms = 0;
    } else {
      m->camera_retry_ms = now + MEDIA_OPEN_RETRY_MS;
      if (!m->warn_camera) {
        m->warn_camera = true;
        CCORD_LOG_W("media: camera open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
      }
    }
  }
  int w = 0, h = 0;
  if (m->camera && m->cam_buf &&
      m->ops->camera_grab(m->ud, m->camera, m->cam_buf, MEDIA_CAM_MAX_BYTES, &w, &h)) {
    *out_bgra = m->cam_buf;
    *out_w = w;
    *out_h = h;
    *out_stride = w * 4;
  }
}

static void pump_send(ccord_app_media *m, const ccord_app_media_voice *v, uint64_t now, bool virtual_clock,
                      bool stream_on, bool paused)
{
  if (!stream_on) {
    if (m->streaming || m->video || m->screen_cap || m->loopback ||
        (m->camera && (!v->active || !v->camera || !m->ops->voice_video_send))) {
      close_send(m);
    } else {
      m->was_paused = false;
      m->pending_keyframe = false;
      reset_stream_warns(m);
    }
    return;
  }
  close_cam_tx(m, false);
  if (!m->streaming) {
    m->stream_start_ms = now;
    m->streaming = true;
  }
  if (paused) {
    m->was_paused = true;
    return;
  }
  if (m->was_paused) {
    m->was_paused = false;
    m->last_video_ms = 0;
    m->pending_keyframe = true;
  }
  if (!m->video) open_encoder(m, virtual_clock, now);
  if (!m->video) return;

  // Source: camera when self_video is on, otherwise the selected screen/portal capture source.
  const uint8_t *bgra = nullptr;
  int w = 0, h = 0, stride = 0;
  if (v->camera) {
    if (m->screen_cap) {
      m->ops->capture_close(m->ud, m->screen_cap);
      m->screen_cap = nullptr;
    }
    grab_camera_frame(m, now, &bgra, &w, &h, &stride);
  } else {
    if (m->camera) {
      m->ops->camera_close(m->ud, m->camera);
      m->camera = nullptr;
      m->opened_cam[0] = '\0';
    }
    if (!m->screen_cap && open_retry_due(now, m->capture_retry_ms)) {
      ccord_capture_source src;
      ccord_error err;
      ccord_error_clear(&err);
      if (m->ops->capture_source(m->ud, &src)) {
        m->screen_cap = m->ops->capture_open(m->ud, &src, &err);
      } else {
        // No explicit source selected (Go Live started without picking a card, or Wayland where
        // the portal picker is the source): default to the portal picker, then the primary X11
        // screen, instead of leaving the stream without capture.
        if (!m->cap_portal_refused) {
          fill_default_capture_source(&src, true);
          m->screen_cap = m->ops->capture_open(m->ud, &src, &err);
          // A cancelled picker is the user's answer for this stream: don't re-prompt every retry.
          if (!m->screen_cap && err.code == CCORD_ERR_PERMISSION) m->cap_portal_refused = true;
        }
        if (!m->screen_cap) {
          fill_default_capture_source(&src, false);
          m->screen_cap = m->ops->capture_open(m->ud, &src, &err);
        }
      }
      if (m->screen_cap) {
        m->capture_retry_ms = 0;
      } else {
        m->capture_retry_ms = now + MEDIA_OPEN_RETRY_MS;
        if (!m->warn_capture) {
          m->warn_capture = true;
          CCORD_LOG_W("media: screen capture open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
        }
      }
    }
    if (m->screen_cap) {
      ccord_capture_frame frame;
      if (m->ops->capture_grab(m->ud, m->screen_cap, &frame)) {
        bgra = frame.data;
        w = frame.width;
        h = frame.height;
        stride = frame.stride;
      } else if (m->ops->capture_failed(m->ud, m->screen_cap)) {
        m->ops->capture_close(m->ud, m->screen_cap);
        m->screen_cap = nullptr;
        m->capture_retry_ms = now + MEDIA_OPEN_RETRY_MS;
      }
    }
  }

  int fps = m->ops->video_fps(m->ud, m->video);
  uint64_t interval = fps > 0 ? (uint64_t)(1000 / fps) : 33u;
  if (m->last_video_ms != 0 && now - m->last_video_ms < interval) return;
  m->last_video_ms = now;
  if (!bgra || w <= 0 || h <= 0) return;

  if (m->pending_keyframe) {
    m->pending_keyframe = false;
    if (m->ops->video_force_keyframe) m->ops->video_force_keyframe(m->ud, m->video);
  }
  ccord_error err;
  size_t pkt_len = 0;
  if (m->ops->video_encode_bgra(m->ud, m->video, bgra, w, h, stride, m->pkt, MEDIA_PKT_CAP, &pkt_len, &err) !=
          CCORD_OK ||
      pkt_len == 0) {
    if (!m->warn_encode) {
      m->warn_encode = true;
      CCORD_LOG_W("media: stream encode failed: %s", err.message);
    }
    return;
  }
  uint32_t ts90k = (uint32_t)((now - m->stream_start_ms) * 90u);
  bool keyframe = m->ops->video_keyframe(m->ud, m->video);
  if (m->ops->stream_send(m->ud, m->pkt, pkt_len, ts90k, keyframe, &err) != CCORD_OK) {
    if (!m->warn_send) {
      m->warn_send = true;
      CCORD_LOG_W("media: stream send failed: %s", err.message);
    }
  }
  // Local preview: the encoder keeps a scaled RGBA copy of the frame it just encoded.
  int pw = 0, ph = 0;
  const uint8_t *rgba = m->ops->video_rgba(m->ud, m->video, &pw, &ph);
  if (rgba && pw > 0 && ph > 0) {
    (void)m->ops->upload_rgba(m->ud, MEDIA_VIDEO_KEY, rgba, pw, ph, &err);
  }
}

static void pump_voice_camera(ccord_app_media *m, const ccord_app_media_voice *v, uint64_t now, bool virtual_clock,
                              bool stream_on)
{
  if (stream_on || !v->active || !v->camera || !m->ops->voice_video_send) {
    if (m->cam_streaming || m->cam_video || (!stream_on && m->camera)) {
      close_cam_tx(m, !stream_on);
    }
    return;
  }
  if (!m->cam_streaming) {
    m->cam_start_ms = now;
    m->cam_streaming = true;
  }
  if (!m->cam_video && open_retry_due(now, m->cam_enc_retry_ms)) {
    int vw = virtual_clock ? 640 : 1280;
    int vh = virtual_clock ? 360 : 720;
    ccord_video_config vcfg = {.width = vw,
                               .height = vh,
                               .fps = 30,
                               .bitrate = 1500000,
                               .codec = CCORD_VIDEO_CODEC_H264,
                               .use_pipewire = false,
                               .preferred_encoder = "auto"};
    ccord_error err;
    m->cam_video = m->ops->video_open(m->ud, &vcfg, &err);
    if (!m->cam_video) {
      vcfg.preferred_encoder = "libx264";
      m->cam_video = m->ops->video_open(m->ud, &vcfg, &err);
    }
    if (m->cam_video) {
      m->cam_enc_retry_ms = 0;
    } else {
      m->cam_enc_retry_ms = now + MEDIA_OPEN_RETRY_MS;
      if (!m->warn_cam_encoder) {
        m->warn_cam_encoder = true;
        CCORD_LOG_W("media: camera encoder open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
      }
    }
  }
  if (!m->cam_video) return;

  const uint8_t *bgra = nullptr;
  int w = 0, h = 0, stride = 0;
  grab_camera_frame(m, now, &bgra, &w, &h, &stride);

  int fps = m->ops->video_fps(m->ud, m->cam_video);
  uint64_t interval = fps > 0 ? (uint64_t)(1000 / fps) : 33u;
  if (m->last_cam_ms != 0 && now - m->last_cam_ms < interval) return;
  m->last_cam_ms = now;
  if (!bgra || w <= 0 || h <= 0) return;

  if (m->cam_pending_keyframe) {
    m->cam_pending_keyframe = false;
    if (m->ops->video_force_keyframe) m->ops->video_force_keyframe(m->ud, m->cam_video);
  }
  ccord_error err;
  size_t pkt_len = 0;
  if (m->ops->video_encode_bgra(m->ud, m->cam_video, bgra, w, h, stride, m->pkt, MEDIA_PKT_CAP, &pkt_len, &err) !=
          CCORD_OK ||
      pkt_len == 0) {
    if (!m->warn_cam_encode) {
      m->warn_cam_encode = true;
      CCORD_LOG_W("media: camera encode failed: %s", err.message);
    }
    return;
  }
  uint32_t ts90k = (uint32_t)((now - m->cam_start_ms) * 90u);
  bool keyframe = m->ops->video_keyframe(m->ud, m->cam_video);
  if (m->ops->voice_video_send(m->ud, m->pkt, pkt_len, ts90k, keyframe, &err) != CCORD_OK) {
    if (!m->warn_cam_send) {
      m->warn_cam_send = true;
      CCORD_LOG_W("media: camera send failed: %s", err.message);
    }
  }
}

// ---- remote voice cameras ---------------------------------------------------------------------

static void cam_close_slot(ccord_app_media *m, size_t i)
{
  cam_slot *c = &m->cams[i];
  if (!c->used) return;
  if (c->decoder) m->ops->decoder_close(m->ud, c->decoder);
  if (m->ops->texture_evict) m->ops->texture_evict(m->ud, c->key);
  memset(c, 0, sizeof *c);
}

static void cam_close_all(ccord_app_media *m)
{
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) cam_close_slot(m, i);
}

static cam_slot *cam_find(ccord_app_media *m, ccord_id user)
{
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) {
    if (m->cams[i].used && m->cams[i].user == user) return &m->cams[i];
  }
  return nullptr;
}

// A slot exists iff its sender is listed as decoding; a changed generation restarts the decoder.
static void cam_reconcile(ccord_app_media *m, const ccord_camera_rx *list, size_t n)
{
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) {
    cam_slot *c = &m->cams[i];
    if (!c->used) continue;
    bool keep = false;
    for (size_t k = 0; k < n; k++) {
      if (list[k].decoding && list[k].user_id == c->user) {
        keep = true;
        break;
      }
    }
    if (!keep) cam_close_slot(m, i);
  }
  size_t slots = 0;
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) slots += m->cams[i].used ? 1u : 0u;
  for (size_t k = 0; k < n; k++) {
    if (!list[k].decoding) continue;
    cam_slot *c = cam_find(m, list[k].user_id);
    if (c && c->generation != list[k].generation) {
      cam_close_slot(m, (size_t)(c - m->cams));
      c = nullptr;
      slots--;
    }
    if (c) continue;
    if (slots >= CCORD_CAM_DECODE_MAX) continue;   // never more than the cap, whatever the list says
    for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) {
      if (m->cams[i].used) continue;
      c = &m->cams[i];
      memset(c, 0, sizeof *c);
      c->used = true;
      c->user = list[k].user_id;
      c->generation = list[k].generation;
      c->need_idr = true;
      snprintf(c->key, sizeof c->key, "__cam__:%llu", (unsigned long long)c->user);
      slots++;
      break;
    }
  }
}

static void cam_open_decoder(ccord_app_media *m, cam_slot *c, uint64_t now)
{
  if (c->decoder || !open_retry_due(now, c->retry_ms)) return;
  ccord_error err;
  c->decoder = m->ops->decoder_open(m->ud, &err);
  if (!c->decoder) {
    c->retry_ms = now + MEDIA_OPEN_RETRY_MS;
    CCORD_LOG_W("media: camera decoder open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
    return;
  }
  c->retry_ms = 0;
  c->need_idr = true;
  if (m->ops->decoder_set_max_dim) {
    m->ops->decoder_set_max_dim(m->ud, c->decoder, MEDIA_CAM_TEX_MAX_W, MEDIA_CAM_TEX_MAX_H);
  }
}

// FNV-1a over 4096 bytes of the RGBA sampled at a fixed stride, plus the centre pixel.
static void cam_fingerprint(cam_slot *c, const uint8_t *rgba, int w, int h)
{
  size_t total = (size_t)w * (size_t)h * 4u;
  size_t stride = total / 4096u;
  if (stride == 0) stride = 1;
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0, off = 0; i < 4096u && off < total; i++, off += stride) {
    hash ^= rgba[off];
    hash *= 1099511628211ull;
  }
  c->frame_hash = hash;
  size_t mid = (((size_t)h / 2u) * (size_t)w + (size_t)w / 2u) * 4u;
  c->center_rgb[0] = rgba[mid];
  c->center_rgb[1] = rgba[mid + 1];
  c->center_rgb[2] = rgba[mid + 2];
}

static void cam_upload(ccord_app_media *m, cam_slot *c)
{
  int w = 0, h = 0;
  const uint8_t *rgba = m->ops->decoder_rgba(m->ud, c->decoder, &w, &h);
  if (!rgba || w <= 0 || h <= 0) {
    c->fresh = false;
    return;
  }
  ccord_error err;
  if (m->ops->upload_rgba(m->ud, c->key, rgba, w, h, &err) != CCORD_OK) return;   // retried next pump
  c->fresh = false;
  c->w = w;
  c->h = h;
  cam_fingerprint(c, rgba, w, h);
}

static void pump_cameras(ccord_app_media *m, const ccord_app_media_voice *v, uint64_t now)
{
  if (!m->ops->camera_senders || !m->ops->camera_poll) return;
  if (!v->active) {
    cam_close_all(m);
    return;
  }
  ccord_camera_rx list[CCORD_CAM_RX_MAX];
  size_t n = m->ops->camera_senders(m->ud, list, CCORD_CAM_RX_MAX);
  if (n > CCORD_CAM_RX_MAX) n = CCORD_CAM_RX_MAX;
  cam_reconcile(m, list, n);
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) {
    if (m->cams[i].used) cam_open_decoder(m, &m->cams[i], now);
  }
  // Round robin from a rotating start so one busy sender cannot starve the others; no clock involved.
  int budget = MEDIA_CAM_AUS_PER_PUMP_TOTAL;
  bool progress = true;
  while (budget > 0 && progress) {
    progress = false;
    for (size_t k = 0; k < CCORD_CAM_RX_MAX && budget > 0; k++) {
      cam_slot *c = &m->cams[(m->cam_rr + k) % CCORD_CAM_RX_MAX];
      if (!c->used || !c->decoder) continue;
      const uint8_t *annexb = nullptr;
      size_t len = 0;
      bool keyframe = false;
      if (!m->ops->camera_poll(m->ud, c->user, &annexb, &len, &keyframe)) continue;
      progress = true;
      budget--;
      if (!annexb || len == 0) continue;
      if (c->need_idr && !keyframe) {
        c->dropped_until_idr++;
        continue;
      }
      ccord_error err;
      if (m->ops->decoder_feed(m->ud, c->decoder, annexb, len, &err) == CCORD_OK) {
        c->need_idr = false;
        c->fresh = true;
        c->frames++;
      } else {
        c->need_idr = true;
        c->decode_errors++;
      }
    }
  }
  m->cam_rr = (m->cam_rr + 1) % CCORD_CAM_RX_MAX;
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) {
    cam_slot *c = &m->cams[i];
    if (c->used && c->decoder && c->fresh) cam_upload(m, c);
  }
}

size_t ccord_app_media_cam_count(const ccord_app_media *m)
{
  size_t n = 0;
  if (!m) return 0;
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) n += m->cams[i].used ? 1u : 0u;
  return n;
}

bool ccord_app_media_cam_debug_of(const ccord_app_media *m, ccord_id user_id, ccord_app_media_cam_debug *out)
{
  if (!m || !out) return false;
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) {
    const cam_slot *c = &m->cams[i];
    if (!c->used || c->user != user_id) continue;
    memset(out, 0, sizeof *out);
    out->open = c->decoder != nullptr;
    out->generation = c->generation;
    out->w = c->w;
    out->h = c->h;
    out->frames = c->frames;
    out->decode_errors = c->decode_errors;
    out->dropped_until_idr = c->dropped_until_idr;
    out->frame_hash = c->frame_hash;
    memcpy(out->center_rgb, c->center_rgb, sizeof out->center_rgb);
    snprintf(out->key, sizeof out->key, "%s", c->key);
    return true;
  }
  return false;
}

static void pump_receive(ccord_app_media *m, uint64_t now)
{
  if (!m->ops->watch_active(m->ud)) {
    if (m->decoder) {
      m->ops->decoder_close(m->ud, m->decoder);
      m->decoder = nullptr;
    }
    m->recv_fresh = false;
    m->recv_need_idr = true;
    m->decoder_retry_ms = 0;
    return;
  }
  if (!m->decoder) {
    if (!open_retry_due(now, m->decoder_retry_ms)) return;
    ccord_error err;
    m->decoder = m->ops->decoder_open(m->ud, &err);
    if (!m->decoder) {
      m->decoder_retry_ms = now + MEDIA_OPEN_RETRY_MS;
      CCORD_LOG_W("media: decoder open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
      return;
    }
    m->decoder_retry_ms = 0;
    m->recv_need_idr = true;
  }
  ccord_error err;
  const uint8_t *annexb = nullptr;
  size_t len = 0;
  bool keyframe = false;
  // Drain a bounded batch per frame so a burst can't stall the UI loop.
  bool fed = false;
  for (int i = 0; i < 16 && m->ops->stream_poll(m->ud, &annexb, &len, &keyframe); i++) {
    if (!annexb || len == 0) continue;
    if (m->recv_need_idr && !keyframe) continue;
    if (m->ops->decoder_feed(m->ud, m->decoder, annexb, len, &err) == CCORD_OK) {
      m->recv_need_idr = false;
      fed = true;
    } else {
      m->recv_need_idr = true;
    }
  }
  // Only re-upload the stage texture when a packet decoded cleanly since the last upload.
  if (fed) m->recv_fresh = true;
  if (!m->recv_fresh) return;
  int w = 0, h = 0;
  const uint8_t *rgba = m->ops->decoder_rgba(m->ud, m->decoder, &w, &h);
  if (!rgba || w <= 0 || h <= 0) {
    m->recv_fresh = false;
    return;
  }
  if (m->ops->upload_rgba(m->ud, MEDIA_VIDEO_KEY, rgba, w, h, &err) == CCORD_OK) {
    m->recv_fresh = false;
  }
}

// ---- lifecycle -------------------------------------------------------------------------------

#define REQUIRE_OP(member)                                                    \
  do {                                                                        \
    if (!ops->member) {                                                       \
      ccord_error_set(err, CCORD_ERR_INVALID, "app_media: missing op " #member); \
      return nullptr;                                                         \
    }                                                                         \
  } while (0)

ccord_app_media *ccord_app_media_create(const ccord_app_media_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  if (!cfg || !cfg->ops) {
    ccord_error_set(err, CCORD_ERR_INVALID, "app_media: ops required");
    return nullptr;
  }
  const ccord_app_media_ops *ops = cfg->ops;
  REQUIRE_OP(prefs);
  REQUIRE_OP(user_volume);
  REQUIRE_OP(key_held);
  REQUIRE_OP(now_ms);
  REQUIRE_OP(dsp_available);
  REQUIRE_OP(voice);
  REQUIRE_OP(voice_users);
  REQUIRE_OP(stream_active);
  REQUIRE_OP(stream_config);
  REQUIRE_OP(watch_active);
  REQUIRE_OP(capture_source);
  REQUIRE_OP(set_voice_config);
  REQUIRE_OP(set_ptt_held);
  REQUIRE_OP(set_user_volume);
  REQUIRE_OP(push_pcm);
  REQUIRE_OP(pull_pcm);
  REQUIRE_OP(stream_send);
  REQUIRE_OP(stream_poll);
  REQUIRE_OP(play_soundboard);
  REQUIRE_OP(pop_soundboard);
  REQUIRE_OP(thread_start);
  REQUIRE_OP(thread_controls);
  REQUIRE_OP(thread_stop);
  REQUIRE_OP(audio_open);
  REQUIRE_OP(audio_close);
  REQUIRE_OP(audio_read);
  REQUIRE_OP(audio_write);
  REQUIRE_OP(devices_list);
  REQUIRE_OP(hotplug_register);
  REQUIRE_OP(camera_open);
  REQUIRE_OP(camera_grab);
  REQUIRE_OP(camera_close);
  REQUIRE_OP(capture_open);
  REQUIRE_OP(capture_grab);
  REQUIRE_OP(capture_failed);
  REQUIRE_OP(capture_close);
  REQUIRE_OP(resolve_dims);
  REQUIRE_OP(video_open);
  REQUIRE_OP(video_close);
  REQUIRE_OP(video_fps);
  REQUIRE_OP(video_keyframe);
  REQUIRE_OP(video_encode_bgra);
  REQUIRE_OP(video_rgba);
  REQUIRE_OP(decoder_open);
  REQUIRE_OP(decoder_feed);
  REQUIRE_OP(decoder_rgba);
  REQUIRE_OP(decoder_close);
  REQUIRE_OP(sound_decode);
  REQUIRE_OP(upload_rgba);
#undef REQUIRE_OP

  ccord_app_media *m = ccord_calloc(1, sizeof *m);
  if (!m) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "app_media: out of memory");
    return nullptr;
  }
  m->sb_mtx = ccord_mutex_create();
  if (!m->sb_mtx) {
    ccord_free(m);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "app_media: mutex init failed");
    return nullptr;
  }
  m->ops = ops;
  m->ud = cfg->ud;
  m->pkt = ccord_malloc(MEDIA_PKT_CAP);
  if (!m->pkt) {
    ccord_mutex_destroy(m->sb_mtx);
    ccord_free(m);
    ccord_error_set(err, CCORD_ERR_NOMEM, "app_media: out of memory");
    return nullptr;
  }
  m->prefs.input_volume = 100;
  m->prefs.output_volume = 100;
  ops->hotplug_register(cfg->ud, media_hotplug, m);
  return m;
}

void ccord_app_media_destroy(ccord_app_media **pm)
{
  if (!pm || !*pm) return;
  ccord_app_media *m = *pm;
  *pm = nullptr;
  m->ops->hotplug_register(m->ud, nullptr, nullptr);
  stop_thread(m);
  close_inline(m);
  close_cam_tx(m, true);
  close_send(m);
  if (m->decoder) {
    m->ops->decoder_close(m->ud, m->decoder);
    m->decoder = nullptr;
  }
  cam_close_all(m);
  clear_soundboard(m);
  ccord_mutex_destroy(m->sb_mtx);
  ccord_free(m->pkt);
  ccord_free(m);
}

void ccord_app_media_pump(ccord_app_media *m, bool live, bool virtual_clock)
{
  if (!m) return;
  m->ops->prefs(m->ud, &m->prefs);
  ccord_app_media_voice v;
  m->ops->voice(m->ud, &v);
  uint64_t now = m->ops->now_ms(m->ud);
  bool stream_on = m->ops->stream_active(m->ud);
  bool paused = stream_on && m->ops->stream_paused && m->ops->stream_paused(m->ud);
  atomic_store(&m->paused_flag, paused);
  bool has_lb_ops = m->ops->loopback_open && m->ops->loopback_read && m->ops->loopback_close;
  bool want_lb = atomic_load(&m->loopback_enabled) ||
                 (m->ops->stream_loopback_audio && m->ops->stream_loopback_audio(m->ud));
  bool loop_on = stream_on && want_lb && has_lb_ops;
  atomic_store(&m->loopback_flag, loop_on);

  if (v.active) {
    int in_vol = clamp_vol(m->prefs.input_volume);
    int out_vol = clamp_vol(m->prefs.output_volume);
    sync_config(m);
    sync_ptt(m);
    sync_volumes(m);
    if (!virtual_clock) {
      if (v.deafened || out_vol <= 0) {
        discard_pending_soundboard(m);
        clear_soundboard(m);
      } else {
        pump_soundboard(m, now);
      }
      if (live) {
        close_inline(m);   // mode switch: the pthread owns the devices now
        sync_live_thread(m, &v, in_vol, out_vol, now);
      } else {
        stop_thread(m);    // mode switch: inline devices own the PCM now
        sync_devices(m, now);
        pump_inline(m, v.muted, v.deafened, in_vol, out_vol);
      }
    } else {
      stop_thread(m);
      close_inline(m);
      discard_pending_soundboard(m);
      clear_soundboard(m);
    }
  } else {
    stop_thread(m);
    close_inline(m);
    reset_sync(m);
    discard_pending_soundboard(m);
    clear_soundboard(m);
  }

  if (m->ops->stream_want_keyframe && m->ops->stream_want_keyframe(m->ud)) {
    if (stream_on) m->pending_keyframe = true;
    else if (v.active && v.camera) m->cam_pending_keyframe = true;
  }

  pump_send(m, &v, now, virtual_clock, stream_on, paused);
  pump_voice_camera(m, &v, now, virtual_clock, stream_on);
  pump_stream_loopback(m, now, loop_on, paused);
  pump_receive(m, now);
  pump_cameras(m, &v, now);
}
