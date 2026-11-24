// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "app_media_internal.h"

static uint32_t clamp_sens(int v)
{
  if (v < 0) return 0u;
  if (v > 100) return 100u;
  return (uint32_t)v;
}

static bool cfg_equal(const ccord_voice_config *a, const ccord_voice_config *b)
{
  return a->ptt == b->ptt && a->vad_sensitivity_pct == b->vad_sensitivity_pct &&
         a->ns == b->ns && a->aec == b->aec && a->agc == b->agc;
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
  if (m->have_ptt && held == m->last_ptt_held && m->prefs.ptt == m->last_ptt_mode) return;
  ccord_error err;
  if (m->ops->set_ptt_held(m->ud, held, &err) == CCORD_OK) {
    m->have_ptt = true;
    m->last_ptt_held = held;
    m->last_ptt_mode = m->prefs.ptt;
  }
}

static void sync_volumes(ccord_app_media *m)
{
  ccord_id users[MEDIA_MAX_USERS];
  size_t n = m->ops->voice_users(m->ud, users, MEDIA_MAX_USERS);
  if (n > MEDIA_MAX_USERS) n = MEDIA_MAX_USERS;
  if (n == 0 && m->vol_count == 0) return;

  bool roster_changed = n != m->last_vol_n ||
                        (n > 0 && memcmp(users, m->last_vol_users, n * sizeof(ccord_id)) != 0);
  if (roster_changed) {
    // Evict departed users first so a full 64-slot table frees slots immediately on member rotation.
    if (m->vol_count > 0) {
      for (size_t j = 0; j < MEDIA_MAX_USERS; j++) {
        if (!m->vol[j].used) continue;
        bool present = false;
        for (size_t i = 0; i < n; i++) {
          if (users[i] == m->vol[j].user) {
            present = true;
            break;
          }
        }
        if (!present) {
          m->vol[j].used = false;
          m->vol_count--;
        }
      }
    }
    if (n > 0) memcpy(m->last_vol_users, users, n * sizeof(ccord_id));
    m->last_vol_n = n;
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
      if (!m->vol[slot].used) m->vol_count++;
      m->vol[slot].user = u;
      m->vol[slot].pct = pct;
      m->vol[slot].used = true;
    }
  }
}

static void reset_sync(ccord_app_media *m)
{
  m->have_cfg = false;
  m->have_ptt = false;
  m->ptt_active_until_ms = 0;
  m->resolved = false;
  m->audio_cap_retry_ms = 0;
  m->audio_play_retry_ms = 0;
  m->thread_retry_ms = 0;
  if (m->vol_count > 0) {
    for (size_t i = 0; i < MEDIA_MAX_USERS; i++) m->vol[i].used = false;
    m->vol_count = 0;
  }
  m->last_vol_n = 0;
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
  m->stage_visible = true;
  atomic_init(&m->sb_active, false);
  m->prefs.input_volume = 100;
  m->prefs.output_volume = 100;
  ops->hotplug_register(cfg->ud, app_media_hotplug, m);
  return m;
}

void ccord_app_media_destroy(ccord_app_media **pm)
{
  if (!pm || !*pm) return;
  ccord_app_media *m = *pm;
  *pm = nullptr;
  m->ops->hotplug_register(m->ud, nullptr, nullptr);
  app_media_stop_thread(m);
  app_media_close_inline(m);
  app_media_close_cam_tx(m, true);
  app_media_close_send(m);
  if (m->decoder) {
    m->ops->decoder_close(m->ud, m->decoder);
    m->decoder = nullptr;
  }
  app_media_cam_close_all(m);
  app_media_clear_soundboard(m);
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
    int in_vol = app_media_clamp_vol(m->prefs.input_volume);
    int out_vol = app_media_clamp_vol(m->prefs.output_volume);
    bool was_ptt_held = m->have_ptt && m->last_ptt_held;
    sync_config(m);
    sync_ptt(m);
    sync_volumes(m);
    // Go Live stream audio falls back to mic capture when active, unpaused, and not using desktop loopback.
    bool stream_needs_mic = stream_on && !paused && !loop_on && m->ops->stream_send_pcm != nullptr;
    bool ptt_idle = false;
    if (m->prefs.ptt) {
      bool ptt_held = ccord_app_media_ptt_open(m->ops, m->ud, &m->prefs);
      if (ptt_held || was_ptt_held) {
        // Keep mic capture open while held and across the 300 ms PTT hangover + 5-frame silence tail after release.
        m->ptt_active_until_ms = now + 450u;
      } else if (m->ptt_active_until_ms != 0 && now >= m->ptt_active_until_ms) {
        m->ptt_active_until_ms = 0;
      }
      bool ptt_tail = m->ptt_active_until_ms != 0 && now < m->ptt_active_until_ms;
      ptt_idle = !ptt_held && !ptt_tail && !stream_needs_mic;
    } else {
      m->ptt_active_until_ms = 0;
    }
    bool cap_suppressed = v.suppressed && !stream_needs_mic;
    if (!virtual_clock) {
      if (v.deafened || out_vol <= 0) {
        app_media_discard_pending_soundboard(m);
        app_media_clear_soundboard(m);
      } else {
        app_media_pump_soundboard(m, now);
      }
      if (live) {
        app_media_close_inline(m);   // mode switch: the pthread owns the devices now
        app_media_sync_live_thread(m, &v, cap_suppressed, ptt_idle, in_vol, out_vol, now);
      } else {
        app_media_stop_thread(m);    // mode switch: inline devices own the PCM now
        app_media_sync_devices(m, now);
        app_media_pump_inline(m, v.muted, v.deafened, cap_suppressed, ptt_idle, in_vol, out_vol);
      }
    } else {
      app_media_stop_thread(m);
      app_media_close_inline(m);
      app_media_discard_pending_soundboard(m);
      app_media_clear_soundboard(m);
    }
  } else {
    app_media_stop_thread(m);
    app_media_close_inline(m);
    reset_sync(m);
    app_media_discard_pending_soundboard(m);
    app_media_clear_soundboard(m);
  }

  if (m->ops->stream_want_keyframe && m->ops->stream_want_keyframe(m->ud)) {
    if (stream_on) m->pending_keyframe = true;
    else if (v.active && v.camera) m->cam_pending_keyframe = true;
  }

  app_media_pump_send(m, &v, now, virtual_clock, stream_on, paused);
  app_media_pump_voice_camera(m, &v, now, virtual_clock, stream_on);
  app_media_pump_stream_loopback(m, now, loop_on, paused);
  app_media_pump_receive(m, now);
  app_media_pump_cameras(m, &v, now);
}

uint32_t ccord_app_media_next_wait_ms(const ccord_app_media *m, uint64_t now_ms)
{
  if (!m || !m->prefs.ptt || m->ptt_active_until_ms == 0) return UINT32_MAX;
  // While the PTT key is still held, wait for KEY_UP / portal Deactivated instead of waking every 450 ms.
  if (m->have_ptt && m->last_ptt_held) return UINT32_MAX;
  if (now_ms >= m->ptt_active_until_ms) return 0u;
  uint64_t rem = m->ptt_active_until_ms - now_ms;
  return rem > UINT32_MAX ? UINT32_MAX : (uint32_t)rem;
}
