// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "app_media_internal.h"

// ---- small helpers --------------------------------------------------------------------------

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

// True when a failed open may be retried now; on failure the caller defers the next attempt by
// MEDIA_OPEN_RETRY_MS so an unavailable device/codec is not reopened (and re-logged) every pump.
// ---- soundboard local playback + audio pthread io -------------------------------------------

void app_media_clear_soundboard(ccord_app_media *m)
{
  if (!atomic_load_explicit(&m->sb_active, memory_order_acquire)) return;
  ccord_mutex_lock(m->sb_mtx);
  ccord_free(m->sb_pcm);
  m->sb_pcm = nullptr;
  m->sb_samples = 0;
  m->sb_pos = 0;
  m->sb_next_ms = 0;
  atomic_store_explicit(&m->sb_active, false, memory_order_release);
  ccord_mutex_unlock(m->sb_mtx);
}

void app_media_discard_pending_soundboard(ccord_app_media *m)
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
void app_media_pump_stream_loopback(ccord_app_media *m, uint64_t now, bool loop_on, bool paused)
{
  if (!loop_on) {
    if (m->loopback) {
      m->ops->loopback_close(m->ud, m->loopback);
      m->loopback = nullptr;
    }
    m->loopback_paused = false;
    m->loopback_unsupported = false;
    m->loopback_retry_ms = 0;
    m->loopback_next_ms = 0;
    return;
  }
  if (paused || !m->ops->stream_send_pcm) {
    if (m->loopback && !m->loopback_paused) {
      if (m->ops->loopback_set_paused) m->ops->loopback_set_paused(m->ud, m->loopback, true);
      m->loopback_paused = true;
    }
    m->loopback_next_ms = 0;
    return;   // pause stream while keeping the handle open; resume immediately on unpause
  }
  if (m->loopback && m->loopback_paused) {
    if (m->ops->loopback_set_paused) m->ops->loopback_set_paused(m->ud, m->loopback, false);
    m->loopback_paused = false;
  }
  if (!m->ops->loopback_open || !m->ops->loopback_read || !m->ops->loopback_close) return;
  if (!m->loopback && m->loopback_unsupported) return;
  if (!m->loopback && app_media_open_retry_due(now, m->loopback_retry_ms)) {
    ccord_error err;
    m->loopback = m->ops->loopback_open(m->ud, &err);
    if (!m->loopback) {
#if CCORD_OS_ANDROID
      if (err.code == CCORD_ERR_UNSUPPORTED) {
        m->loopback_unsupported = true;
        if (!m->warn_loopback) {
          m->warn_loopback = true;
          CCORD_LOG_W("media: loopback audio unsupported on Android: %s", err.message);
        }
        return;
      }
#endif
      m->loopback_retry_ms = now + MEDIA_OPEN_RETRY_MS;
      if (!m->warn_loopback) {
        m->warn_loopback = true;
        CCORD_LOG_W("media: loopback audio open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS,
                    err.message);
      }
      return;
    }
    m->loopback_paused = false;
    m->loopback_retry_ms = 0;
  }
  if (!m->loopback) return;
  if (m->ops->loopback_failed && m->ops->loopback_failed(m->ud, m->loopback)) {
    m->ops->loopback_close(m->ud, m->loopback);
    m->loopback = nullptr;
    m->loopback_paused = false;
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
  if (atomic_load_explicit(&m->sb_active, memory_order_acquire)) {
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
        atomic_store_explicit(&m->sb_active, false, memory_order_release);
      }
    } else {
      atomic_store_explicit(&m->sb_active, false, memory_order_release);
    }
    ccord_mutex_unlock(m->sb_mtx);
  }

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

void app_media_pump_soundboard(ccord_app_media *m, uint64_t now)
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
  if (samples < MEDIA_SB_CAP_SAMPLES) {
    int16_t *shrunk = ccord_realloc(buf, samples * sizeof(int16_t));
    if (shrunk) buf = shrunk;
  }
  apply_gain(buf, samples, app_media_clamp_vol((int)vol_pct));

  ccord_mutex_lock(m->sb_mtx);
  ccord_free(m->sb_pcm);
  m->sb_pcm = buf;
  m->sb_samples = samples;
  m->sb_pos = 0;
  m->sb_next_ms = now;
  atomic_store_explicit(&m->sb_active, true, memory_order_release);
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

void app_media_hotplug(void *ud, bool capture, bool added)
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
  app_media_copy_id(m->sel_in, m->prefs.input_device_id);
  app_media_copy_id(m->sel_out, m->prefs.output_device_id);
  if (device_present(m, true, m->sel_in)) app_media_copy_id(m->eff_in, m->sel_in);
  else m->eff_in[0] = '\0';
  if (device_present(m, false, m->sel_out)) app_media_copy_id(m->eff_out, m->sel_out);
  else m->eff_out[0] = '\0';
  m->resolved = true;
  m->devices_dirty = false;
}

void app_media_close_inline(ccord_app_media *m)
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
  m->inline_cap_gated = false;
}

void app_media_stop_thread(ccord_app_media *m)
{
  if (m->thread) {
    m->ops->thread_stop(m->ud, m->thread);
    m->thread = nullptr;
  }
  m->thread_in[0] = '\0';
  m->thread_out[0] = '\0';
  m->have_thread_ctrl = false;
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
void app_media_sync_live_thread(ccord_app_media *m, const ccord_app_media_voice *v, bool cap_suppressed,
                                bool ptt_idle, int in_vol, int out_vol, uint64_t now)
{
  maybe_resolve_devices(m);
  bool changed = strcmp(m->eff_in, m->thread_in) != 0 || strcmp(m->eff_out, m->thread_out) != 0;
  if (changed) {
    app_media_stop_thread(m);
    m->thread_retry_ms = 0;
  }
  if (!m->thread && app_media_open_retry_due(now, m->thread_retry_ms)) {
    ccord_error err;
    ccord_voice_audio_io io = {.push = media_io_push, .pull = media_io_pull, .ud = m};
    app_media_copy_id(io.input_device_id, m->eff_in);
    app_media_copy_id(io.output_device_id, m->eff_out);
    app_media_copy_id(m->thread_in, m->eff_in);
    app_media_copy_id(m->thread_out, m->eff_out);
    m->thread = m->ops->thread_start(m->ud, &io, &err);
    if (m->thread) {
      m->thread_retry_ms = 0;
      m->have_thread_ctrl = false;
    } else {
      m->thread_retry_ms = now + MEDIA_OPEN_RETRY_MS;
      CCORD_LOG_W("media: audio thread start failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
    }
  }
  if (m->thread) {
    if (!m->have_thread_ctrl || v->muted != m->last_th_muted || v->deafened != m->last_th_deafened ||
        cap_suppressed != m->last_th_cap_suppressed || ptt_idle != m->last_th_ptt_idle ||
        in_vol != m->last_th_in_vol || out_vol != m->last_th_out_vol) {
      if (m->ops->thread_controls_ex) {
        m->ops->thread_controls_ex(m->ud, m->thread, v->muted, v->deafened, cap_suppressed, ptt_idle, in_vol,
                                   out_vol);
      } else {
        m->ops->thread_controls(m->ud, m->thread, v->muted, v->deafened, in_vol, out_vol);
      }
      m->have_thread_ctrl = true;
      m->last_th_muted = v->muted;
      m->last_th_deafened = v->deafened;
      m->last_th_cap_suppressed = cap_suppressed;
      m->last_th_ptt_idle = ptt_idle;
      m->last_th_in_vol = in_vol;
      m->last_th_out_vol = out_vol;
    }
  }
}

// Open/reopen the inline capture+playback handles when the effective device ids change.
void app_media_sync_devices(ccord_app_media *m, uint64_t now)
{
  maybe_resolve_devices(m);
  ccord_error err;
  bool cap_changed = strcmp(m->eff_in, m->opened_in) != 0;
  if (cap_changed) m->audio_cap_retry_ms = 0;
  if (!m->audio_cap || cap_changed) {
    if (app_media_open_retry_due(now, m->audio_cap_retry_ms)) {
      if (m->audio_cap) m->ops->audio_close(m->ud, m->audio_cap);
      m->audio_cap = m->ops->audio_open(m->ud, true, m->eff_in, &err);
      app_media_copy_id(m->opened_in, m->eff_in);
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
    if (app_media_open_retry_due(now, m->audio_play_retry_ms)) {
      if (m->audio_play) m->ops->audio_close(m->ud, m->audio_play);
      m->audio_play = m->ops->audio_open(m->ud, false, m->eff_out, &err);
      app_media_copy_id(m->opened_out, m->eff_out);
      if (m->audio_play) {
        m->audio_play_retry_ms = 0;
      } else {
        m->audio_play_retry_ms = now + MEDIA_OPEN_RETRY_MS;
        CCORD_LOG_W("media: playback open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
      }
    }
  }
}

void app_media_pump_inline(ccord_app_media *m, bool muted, bool deafened, bool cap_suppressed, bool ptt_idle,
                           int in_vol, int out_vol)
{
  ccord_error err;
  int16_t pcm[MEDIA_FRAME * 2];
  bool cap_gated = (muted || in_vol <= 0 || cap_suppressed || ptt_idle);
  if (m->audio_cap && !cap_gated) {
    if (m->inline_cap_gated) {
      // Discard any stale PCM queued right before unmuting.
      for (int i = 0; i < 4; i++) {
        size_t drop = m->ops->audio_read(m->ud, m->audio_cap, pcm, MEDIA_FRAME);
        if (drop < MEDIA_FRAME) break;
      }
      m->inline_cap_gated = false;
    }
    size_t rframes = m->ops->audio_read(m->ud, m->audio_cap, pcm, MEDIA_FRAME);
    if (rframes > 0) {
      apply_gain(pcm, rframes * 2, in_vol);
      (void)m->ops->push_pcm(m->ud, pcm, rframes, &err);
      pump_stream_send_pcm(m, pcm, rframes);
    }
  } else if (m->audio_cap && cap_gated) {
    // Drain and discard capture samples while muted so the SDL capture stream cannot leak RAM.
    for (int i = 0; i < 8; i++) {
      size_t drop = m->ops->audio_read(m->ud, m->audio_cap, pcm, MEDIA_FRAME);
      if (drop < MEDIA_FRAME) break;
    }
    m->inline_cap_gated = true;
  }
  if (m->audio_play && !deafened && out_vol > 0) {
    size_t pframes = pull_playback_pcm(m, pcm, MEDIA_FRAME);
    if (pframes > 0) {
      apply_gain(pcm, pframes * 2, out_vol);
      (void)m->ops->audio_write(m->ud, m->audio_play, pcm, pframes);
    }
  }
}

