// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "live_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Standalone camera caps advertised by Voice OP 12 while self_video is on without Go Live: 720p30,
// the app camera encoder's ceiling (app_media pump_voice_camera), with Discord's standard H.264 PTs.
static const ccord_voice_video_desc k_live_camera_desc = {
  .width = 1280u,
  .height = 720u,
  .fps = 30u,
  .max_bitrate = 2500000u,
  .payload_type = CCORD_VSTREAM_H264_PT_DEFAULT,
  .rtx_payload_type = CCORD_VSTREAM_H264_RTX_PT_DEFAULT,
};

ccord_voice_gateway *live_voice_acquire(live *l)
{
  if (!l || !l->voice_mtx) return nullptr;
  ccord_mutex_lock(l->voice_mtx);
  ccord_voice_gateway *vg = l->voice;
  if (vg) l->voice_inflight++;
  ccord_mutex_unlock(l->voice_mtx);
  return vg;
}

void live_voice_release(live *l)
{
  if (!l || !l->voice_mtx) return;
  ccord_mutex_lock(l->voice_mtx);
  l->voice_inflight--;
  if (l->voice_inflight == 0 && l->voice_cond) ccord_cond_broadcast(l->voice_cond);
  ccord_mutex_unlock(l->voice_mtx);
}

static void live_voice_publish(live *l, ccord_voice_gateway *vg)
{
  if (!l) return;
  if (l->voice_mtx) ccord_mutex_lock(l->voice_mtx);
  l->voice = vg;
  if (l->voice_mtx) ccord_mutex_unlock(l->voice_mtx);
}

static void live_voice_detach_and_destroy(live *l)
{
  if (!l) return;
  ccord_voice_gateway *old = nullptr;
  if (l->voice_mtx) ccord_mutex_lock(l->voice_mtx);
  old = l->voice;
  l->voice = nullptr;
  while (l->voice_inflight > 0 && l->voice_cond) {
    ccord_cond_wait(l->voice_cond, l->voice_mtx);
  }
  if (l->voice_mtx) ccord_mutex_unlock(l->voice_mtx);
  if (old) ccord_voice_gateway_destroy(old);
}


void live_voice_cache_put(live *l, const ccord_voice_state *v)
{
  if (!v || v->user_id == CCORD_ID_NONE) return;
  long idx = -1;
  for (size_t i = 0; i < l->voice_cache_count; i++) {
    if (l->voice_cache[i].guild_id == v->guild_id && l->voice_cache[i].user_id == v->user_id) {
      idx = (long)i;
      break;
    }
  }
  if (v->channel_id == CCORD_ID_NONE) {
    // Disconnected: drop the row so a later stage answer cannot resurrect stale flags.
    if (idx >= 0) {
      memmove(&l->voice_cache[idx], &l->voice_cache[(size_t)idx + 1u],
              (l->voice_cache_count - (size_t)idx - 1u) * sizeof l->voice_cache[0]);
      l->voice_cache_count--;
    }
    return;
  }
  if (idx >= 0) {
    ccord_voice_state merged = *v;
    // A partial dispatch may omit the stage fields; keep the last known values for those.
    if (v->omit_suppress) merged.suppress = l->voice_cache[idx].suppress;
    if (v->omit_request_to_speak) merged.request_to_speak_timestamp = l->voice_cache[idx].request_to_speak_timestamp;
    l->voice_cache[idx] = merged;
    return;
  }
  if (l->voice_cache_count == LIVE_VOICE_CACHE_MAX) {
    memmove(&l->voice_cache[0], &l->voice_cache[1], (LIVE_VOICE_CACHE_MAX - 1u) * sizeof l->voice_cache[0]);
    l->voice_cache_count = LIVE_VOICE_CACHE_MAX - 1u;
  }
  l->voice_cache[l->voice_cache_count++] = *v;
}

const ccord_voice_state *live_voice_cache_find(const live *l, ccord_id guild_id, ccord_id user_id)
{
  for (size_t i = 0; i < l->voice_cache_count; i++) {
    if (l->voice_cache[i].guild_id == guild_id && l->voice_cache[i].user_id == user_id) return &l->voice_cache[i];
  }
  return nullptr;
}

void live_voice_cache_from_event(live *l, const ccord_net_event *ev)
{
  // Cache single-slot VOICE_STATE_UPDATE as well as batch VOICE_STATES / GUILD_UPSERT lists.
  if (ev->voice_state) {
    live_voice_cache_put(l, ev->voice_state);
    ccord_id uid = ev->voice_state->user_id;
    if (uid != CCORD_ID_NONE && ev->voice_state->channel_id != CCORD_ID_NONE) {
      if (l->tx_stream.vs) ccord_vstream_register_peers(l->tx_stream.vs, CCORD_ID_NONE, &uid, 1);
      if (l->rx_stream.vs) ccord_vstream_register_peers(l->rx_stream.vs, l->watched_user, &uid, 1);
    }
  }
  if (ev->voice_states && ev->voice_state_count > 0) {
    for (size_t i = 0; i < ev->voice_state_count; i++) {
      live_voice_cache_put(l, &ev->voice_states[i]);
      ccord_id uid = ev->voice_states[i].user_id;
      if (uid != CCORD_ID_NONE && ev->voice_states[i].channel_id != CCORD_ID_NONE) {
        if (l->tx_stream.vs) ccord_vstream_register_peers(l->tx_stream.vs, CCORD_ID_NONE, &uid, 1);
        if (l->rx_stream.vs) ccord_vstream_register_peers(l->rx_stream.vs, l->watched_user, &uid, 1);
      }
    }
  }
}


void live_voice_stop(live *l)
{
  live_vstream_slot_reset(l, &l->tx_stream);
  live_vstream_slot_close_media(l, &l->rx_stream);
  live_voice_detach_and_destroy(l);
  l->voice_video_synced = false;
  ccord_live_wipe(l->voice_token, sizeof l->voice_token);
  l->voice_endpoint[0] = '\0';
  l->voice_session_id[0] = '\0';
  l->voice_server_valid = false;
  l->voice_state_valid = false;
  l->voice_attempted = false;
  // The call is over (user hang-up, give-up, channel change): drop any reconnect ladder state with it.
  uint64_t last_nudge = l->vc.last_event_ms;
  ccord_vc_init(&l->vc, live_now(l) ^ (uint64_t)(uintptr_t)l);
  l->vc.last_event_ms = last_nudge;
  l->vlink = LV_IDLE;
  l->vlink_at_ms = 0;
  l->vlink_plan = (ccord_vc_plan){0};
  l->voice_swallow_leave = false;
  l->vlink_was_down = false;
  l->vlink_gw_reported = false;
  l->vlink_rewatch = false;
  atomic_store(&l->ptt_active, false);
  atomic_store(&l->ptt_release_ms, 0);
  l->sb_speaking = false;
  l->sb_speaking_until_ms = 0;
  ccord_free(l->sb_pending_pcm);
  l->sb_pending_pcm = nullptr;
  l->sb_pending_samples = 0;
  l->sb_pending_offset = 0;
}


// ---- Voice reconnect ladder -----------------------------------------
// The voice gateway resumes a dropped WebSocket by itself (layer 1). Everything it cannot do alone comes back here
// as a ccord_voice_failure: REIDENTIFY, REJOIN, AWAIT_MAIN or FINAL. The call stays "joined" (voice_guild/channel,
// store, UI) until the ladder gives up or the user leaves.

static void live_voice_link_report(live *l, ccord_voice_link st, uint32_t attempt, uint32_t retry_ms, const char *text)
{
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  ev.ev.type = CCORD_NET_VOICE_LINK;
  ev.ev.guild_id = l->voice_guild;
  ev.ev.channel_id = l->voice_channel;
  ev.ev.voice_link = (uint8_t)st;
  ev.ev.voice_link_attempt = attempt;
  ev.ev.voice_link_retry_ms = retry_ms;
  ev.ev.text = text ? text : "";   // always a static string
  if (st == CCORD_VLINK_RECONNECTING) l->vlink_was_down = true;
  (void)live_push(l, &ev);
}

// Destroys the voice gateway only: token, endpoint, session id and the voice_*_valid flags stay, so a REIDENTIFY can
// reconnect with the same credentials.
static void live_voice_gateway_drop(live *l)
{
  live_voice_detach_and_destroy(l);
  l->voice_video_synced = false;   // the new gateway re-sends OP 12 once ESTABLISHED
  l->voice_attempted = false;
  l->vlink_gw_reported = false;
}

// Gives up the call. `tell_server` sends an OP 4 leave so no ghost presence stays on Discord; it is false after
// 4014 / 4022, where the user may have been moved and a leave would disconnect them from the new channel.
static void live_voice_giveup(live *l, const char *reason, bool tell_server)
{
  ccord_id guild = l->voice_guild;
  ccord_id channel = l->voice_channel;
  if (!reason || reason[0] == '\0') reason = "connection lost";
  if (tell_server && l->gw && guild != CCORD_ID_NONE) {
    ccord_error ignored;
    (void)ccord_gateway_send_voice_state(l->gw, guild, CCORD_ID_NONE, l->self_mute, l->self_deaf, false, false,
                                         &ignored);
  }
  live_streams_stop_all(l);
  live_voice_stop(l);
  if (guild != CCORD_ID_NONE && channel != CCORD_ID_NONE && l->self_id != CCORD_ID_NONE) {
    ccord_live_event ev;
    ccord_live_event_init(&ev);
    ccord_arena_init(&ev.arena, sizeof(ccord_voice_state) + 32u);
    ccord_voice_state *v = ccord_arena_alloc(&ev.arena, sizeof *v, _Alignof(ccord_voice_state));
    if (v) {
      memset(v, 0, sizeof *v);
      v->guild_id = guild;
      v->channel_id = CCORD_ID_NONE;
      v->user_id = l->self_id;
      ev.voice_states = v;
      ev.voice_state_count = 1;
      ev.ev.type = CCORD_NET_VOICE_STATE_UPDATE;
      ev.ev.guild_id = guild;
      ev.ev.channel_id = CCORD_ID_NONE;
      ev.ev.user_id = l->self_id;
      ev.ev.voice_state = v;
      live_voice_cache_put(l, v);
      (void)live_push(l, &ev);
    }
  }
  live_voice_link_report(l, CCORD_VLINK_FAILED, 0, 0, reason);
  {
    ccord_live_event ev;
    ccord_live_event_init(&ev);
    ccord_arena_init(&ev.arena, strlen(reason) + 16u);
    ev.ev.type = CCORD_NET_ACTION_FAILED;
    ev.ev.action = CCORD_ACTION_VOICE;
    ev.ev.guild_id = guild;
    ev.ev.channel_id = channel;
    ev.ev.status = CCORD_ERR_IO;
    ev.ev.text = ccord_arena_strndup(&ev.arena, reason, strlen(reason));
    (void)live_push(l, &ev);
  }
  l->voice_guild = CCORD_ID_NONE;
  l->voice_channel = CCORD_ID_NONE;
  l->self_video = false;
  l->streaming = false;
  l->self_suppressed = false;
  l->vlink_was_down = false;
}

// Carries out a policy decision for a gateway that is gone (or never connected).
static void live_voice_apply_plan(live *l, ccord_vc_plan plan)
{
  uint64_t now = live_now(l);
  switch (plan.action) {
    case CCORD_VC_RESUME:   // the gateway object is gone: there is no session left to resume
    case CCORD_VC_REIDENTIFY:
    case CCORD_VC_REJOIN:
      live_voice_gateway_drop(l);
      l->vlink_plan = plan;
      if (plan.action == CCORD_VC_RESUME) l->vlink_plan.action = CCORD_VC_REIDENTIFY;
      l->vlink = LV_WAIT_ATTEMPT;
      l->vlink_at_ms = now + plan.delay_ms;
      live_voice_link_report(l, CCORD_VLINK_RECONNECTING, plan.attempt, plan.delay_ms, "");
      break;
    case CCORD_VC_AWAIT_MAIN:
      live_voice_gateway_drop(l);
      l->vlink_plan = plan;
      l->vlink = LV_AWAIT_MAIN;
      l->vlink_at_ms = now + plan.delay_ms;
      live_voice_link_report(l, CCORD_VLINK_RECONNECTING, 0, plan.delay_ms, "");
      break;
    case CCORD_VC_FINAL:
      live_voice_giveup(l, plan.reason, true);
      break;
  }
}

// The voice WebSocket never connected (curl error / create failure): same ladder, same credentials.
static void live_voice_connect_failed(live *l)
{
  ccord_vc_plan plan = ccord_vc_plan_next(&l->vc, live_now(l), 0, CCORD_VC_CAUSE_CONNECT, false);
  live_voice_apply_plan(l, plan);
}

void live_voice_maybe_start(live *l)
{
  if (!l->voice_server_valid || !l->voice_state_valid || l->voice_attempted) return;
  if (l->voice_guild == CCORD_ID_NONE || l->voice_channel == CCORD_ID_NONE || l->self_id == CCORD_ID_NONE) return;
  if (l->voice) return;
  // A timer step (backoff, grace for the main gateway) owns the next attempt; other events must not skip it.
  if (l->vlink == LV_WAIT_ATTEMPT || l->vlink == LV_AWAIT_MAIN || l->vlink == LV_WAIT_MAIN_GW) return;
  l->voice_attempted = true;
  ccord_voice_gateway_config vc = {
    .server_id = l->voice_guild == CCORD_DM_GUILD_ID ? l->voice_channel : l->voice_guild,
    .channel_id = l->voice_channel,
    .user_id = l->self_id,
    .session_id = l->voice_session_id[0] ? l->voice_session_id : l->gw_session_id,
    .token = l->voice_token,
    .endpoint = l->voice_endpoint,
    .connect_timeout_ms = l->vlink_was_down ? CCORD_VC_CONNECT_TIMEOUT_MS : 0,   // a retry must not freeze the UI for 15 s
    .now_ms = l->now_ms,
    .clock_ud = l->clock_ud,
  };
  ccord_error verr;
  ccord_voice_gateway *vg = ccord_voice_gateway_create(&vc, &verr);
  if (!vg) {
    CCORD_LOG_W("live: voice create failed: %s", verr.message);
    live_voice_connect_failed(l);
    return;
  }
  if (ccord_voice_gateway_start(vg, &verr) != CCORD_OK) {
    CCORD_LOG_W("live: voice connect failed: %s", verr.message);
    ccord_voice_gateway_destroy(vg);
    live_voice_connect_failed(l);
    return;
  }
  for (size_t i = 0; i < l->user_volume_count; i++) {
    ccord_voice_gateway_set_user_volume(vg, l->user_volume[i].user_id, l->user_volume[i].pct);
  }
  if (l->has_voice_cfg) {
    ccord_voice_gateway_set_dsp(vg, &l->voice_cfg);
  }
  if (l->self_suppressed) {
    ccord_voice_gateway_set_suppressed(vg, true);
  }
  if (l->self_deaf) {
    ccord_voice_gateway_set_deafened(vg, true);
  }
  if (l->self_video) {
    ccord_error derr;
    (void)ccord_voice_gateway_set_video_desc(vg, &k_live_camera_desc, &derr);   // a new gateway starts without it
  }
  ccord_voice_gateway_set_camera_rx(vg, true);   // main voice gateway only: per-user remote cameras
  live_voice_publish(l, vg);
  l->vlink_gw_reported = false;
  if (l->vlink == LV_WAIT_SERVER) l->vlink = LV_IDLE;   // the new credentials arrived; the gateway takes over
}

// REJOIN: leave and join again on the main gateway, which hands out fresh credentials. Other participants see us leave
// and re-enter for a moment; Discord's own client does the same for a new session.
static void live_voice_rejoin(live *l, uint64_t now)
{
  bool sent = false;
  if (l->gw && l->voice_guild != CCORD_ID_NONE && l->voice_channel != CCORD_ID_NONE &&
      ccord_gateway_state(l->gw) == CCORD_CONN_CONNECTED) {
    ccord_error e1, e2;
    sent = ccord_gateway_send_voice_state(l->gw, l->voice_guild, CCORD_ID_NONE, l->self_mute, l->self_deaf, false,
                                          false, &e1) == CCORD_OK &&
           ccord_gateway_send_voice_state(l->gw, l->voice_guild, l->voice_channel, l->self_mute, l->self_deaf,
                                          l->self_video, false, &e2) == CCORD_OK;
  }
  if (!sent) {
    // The main gateway is down (it reconnects on its own and then re-announces us): wait, bounded by the outage clock.
    if (l->vc.first_fail_ms != 0 && now > l->vc.first_fail_ms && now - l->vc.first_fail_ms >= CCORD_VC_GIVEUP_MS) {
      live_voice_giveup(l, "connection lost", false);
      return;
    }
    live_voice_gateway_drop(l);
    l->vlink = LV_WAIT_MAIN_GW;
    l->vlink_at_ms = now + 500u;
    return;
  }
  live_voice_gateway_drop(l);
  // The old voice session is over: Discord deletes our stream with the leave, and a watched stream is re-requested
  // once the new session is ESTABLISHED.
  live_vstream_slot_reset(l, &l->tx_stream);
  l->streaming = false;
  l->tx_stream_paused = false;
  live_vstream_slot_close_media(l, &l->rx_stream);
  l->vlink_rewatch = l->watching_stream;
  ccord_live_wipe(l->voice_token, sizeof l->voice_token);
  l->voice_endpoint[0] = '\0';
  l->voice_session_id[0] = '\0';
  l->voice_server_valid = false;
  l->voice_state_valid = false;
  l->voice_swallow_leave = true;
  l->vlink = LV_WAIT_SERVER;
  l->vlink_at_ms = now + CCORD_VC_REJOIN_WAIT_MS;
}

// A gateway in state FAILED: adopt its policy state (or re-plan when an outage is already running) and continue.
void live_voice_lost(live *l)
{
  ccord_voice_failure f;
  if (!l->voice || !ccord_voice_gateway_failure(l->voice, &f)) {
    f = (ccord_voice_failure){.cause = CCORD_VC_CAUSE_IO};
    f.plan = ccord_vc_plan_next(&l->vc, live_now(l), 0, CCORD_VC_CAUSE_IO, false);
  } else if (l->vc.first_fail_ms == 0 && l->vc.attempt == 0) {
    // A new outage: the failing gateway already planned it (and counted its own resume tries).
    uint64_t last_nudge = l->vc.last_event_ms;
    l->vc = f.state;
    l->vc.last_event_ms = last_nudge;
  } else {
    // A replacement gateway failed inside a running outage: the budget lives here, not in the new gateway.
    f.plan = ccord_vc_plan_next(&l->vc, live_now(l), f.close_code, f.cause, f.was_established);
  }
  CCORD_LOG_D("live: voice lost (close %u) -> %s attempt %u", (unsigned)f.close_code,
              ccord_vc_action_name(f.plan.action), (unsigned)f.plan.attempt);
  live_voice_apply_plan(l, f.plan);
}

// Every poll: report the gateway's own resume, and the end of an outage.
void live_voice_watch(live *l, uint64_t now)
{
  ccord_voice_connection_state st = ccord_voice_gateway_state(l->voice);
  if (st == CCORD_VOICE_FAILED) {
    live_voice_lost(l);
  } else if (st == CCORD_VOICE_RECONNECTING) {
    if (!l->vlink_gw_reported) {
      l->vlink_gw_reported = true;
      live_voice_link_report(l, CCORD_VLINK_RECONNECTING, 1, 0, "");
    }
  } else if (st == CCORD_VOICE_ESTABLISHED) {
    if (l->vlink_was_down) {
      l->vlink_was_down = false;
      l->vlink_gw_reported = false;
      ccord_vc_established(&l->vc, now);
      live_voice_link_report(l, CCORD_VLINK_CONNECTED, 0, 0, "");
      if (l->vlink_rewatch) {
        l->vlink_rewatch = false;
        if (l->gw && l->watching_stream && l->rx_stream.stream_key[0] != '\0') {
          ccord_error werr;
          (void)ccord_gateway_send_stream_watch(l->gw, l->rx_stream.stream_key, &werr);
        }
      }
    }
    ccord_vc_tick(&l->vc, now);
  }
}

// Timers of every LV_* state.
void live_voice_link_tick(live *l, uint64_t now)
{
  if (l->voice_guild == CCORD_ID_NONE || l->vlink == LV_IDLE || now < l->vlink_at_ms) return;
  switch (l->vlink) {
    case LV_WAIT_ATTEMPT:
      if (l->vlink_plan.action == CCORD_VC_REJOIN || !l->voice_server_valid || !l->voice_state_valid) {
        live_voice_rejoin(l, now);   // stale or missing credentials cannot be re-identified
      } else {
        l->vlink = LV_IDLE;
        l->voice_attempted = false;
        live_voice_maybe_start(l);
      }
      break;
    case LV_WAIT_MAIN_GW:
      live_voice_rejoin(l, now);
      break;
    case LV_WAIT_SERVER: {
      // No new VOICE_SERVER_UPDATE: this rejoin attempt failed. 4006 classifies as REJOIN, so the budget decides.
      ccord_vc_plan plan = ccord_vc_plan_next(&l->vc, now, 4006, CCORD_VC_CAUSE_CLOSE, true);
      l->voice_swallow_leave = false;
      live_voice_apply_plan(l, plan);
      break;
    }
    case LV_AWAIT_MAIN:
      live_voice_giveup(l, "you were disconnected", false);   // no OP 4: the user may already be in another channel
      break;
    default: break;
  }
}

// A fresh READY while we are in voice: Discord dropped our voice state with the old main session. A new outage: REJOIN now.
void live_voice_main_session_replaced(live *l)
{
  uint64_t now = live_now(l);
  uint64_t last_nudge = l->vc.last_event_ms;
  ccord_vc_init(&l->vc, now ^ (uint64_t)(uintptr_t)l);
  l->vc.last_event_ms = last_nudge;
  ccord_vc_plan plan = ccord_vc_plan_next(&l->vc, now, 4006, CCORD_VC_CAUSE_CLOSE, true);
  if (plan.action != CCORD_VC_REJOIN) return;
  plan.delay_ms = 0;
  live_voice_apply_plan(l, plan);
}

void live_set_self_voice_flags(live *l, bool self_mute, bool self_deaf)
{
  l->self_mute = self_mute;
  l->self_deaf = self_deaf;
  if (l->voice) ccord_voice_gateway_set_deafened(l->voice, self_deaf);
  if (self_deaf) {
    ccord_mutex_lock(l->media_mtx);
    l->rx_pcm_head = 0;
    l->rx_pcm_count = 0;
    ccord_mutex_unlock(l->media_mtx);
  }
}

bool live_voice_on_event(live *l, const ccord_net_event *ev, const char *voice_session_id)
{
  if (ev->type == CCORD_NET_VOICE_SERVER_UPDATE && ev->voice_server) {
    if (l->voice_guild == CCORD_ID_NONE || ev->voice_server->guild_id != l->voice_guild) return false;
    bool changed = strcmp(l->voice_token, ev->voice_server->token ? ev->voice_server->token : "") != 0 ||
                   strcmp(l->voice_endpoint, ev->voice_server->endpoint ? ev->voice_server->endpoint : "") != 0;
    live_copy_cstr(l->voice_token, sizeof l->voice_token, ev->voice_server->token);
    live_copy_cstr(l->voice_endpoint, sizeof l->voice_endpoint, ev->voice_server->endpoint);
    l->voice_server_guild = ev->voice_server->guild_id;
    l->voice_server_channel = ev->voice_server->channel_id;
    l->voice_server_valid = l->voice_token[0] != '\0' && l->voice_endpoint[0] != '\0';
    // Server migration: follow the new endpoint (no budget use). A pending timer step is moot once Discord
    // hands out credentials, and the main gateway answered a 4014 / 4022 wait.
    if (l->voice && changed) live_voice_gateway_drop(l);
    if (l->vlink == LV_AWAIT_MAIN || (changed && l->vlink != LV_WAIT_SERVER)) l->vlink = LV_IDLE;
    l->voice_attempted = false;
    live_voice_maybe_start(l);
  } else if (ev->type == CCORD_NET_VOICE_STATE_UPDATE && ev->voice_state) {
    if (ev->guild_id != l->voice_guild) return false;
    if (l->self_id == CCORD_ID_NONE || ev->user_id != l->self_id) return false;
    if (ev->channel_id == CCORD_ID_NONE) {
      if (l->voice_swallow_leave) {   // the echo of our own rejoin's leave: the call goes on
        l->voice_swallow_leave = false;
        return true;
      }
      live_streams_stop_all(l);
      live_voice_stop(l);
      l->voice_guild = CCORD_ID_NONE;
      l->voice_channel = CCORD_ID_NONE;
      l->self_video = false;
      l->streaming = false;
      l->self_suppressed = false;
      return false;
    }
    if (ev->channel_id != l->voice_channel) {
      // Server/moderator moved us to a different channel in the same guild: track the new channel so OP 4
      // mute/deafen/camera/leave payloads target the active channel instead of snapping us back.
      l->voice_channel = ev->channel_id;
    }
    l->voice_swallow_leave = false;   // the join echo: any leave echo is behind us
    if (!ev->voice_state->omit_suppress) {
      l->self_suppressed = ev->voice_state->suppress;
      if (l->voice) ccord_voice_gateway_set_suppressed(l->voice, l->self_suppressed);
    }
    live_set_self_voice_flags(l, ev->voice_state->self_mute, ev->voice_state->self_deaf);
    bool sid_changed = false;
    if (voice_session_id && voice_session_id[0] != '\0') {
      sid_changed = strcmp(l->voice_session_id, voice_session_id) != 0;
      live_copy_cstr(l->voice_session_id, sizeof l->voice_session_id, voice_session_id);
    }
    l->voice_state_valid = true;
    l->voice_attempted = false;
    live_voice_maybe_start(l);
    if (sid_changed) {
      live_vstream_slot_close_media(l, &l->tx_stream);
      live_vstream_slot_close_media(l, &l->rx_stream);
    }
    live_vstream_maybe_open(l, &l->tx_stream);
    live_vstream_maybe_open(l, &l->rx_stream);
  }
  return false;
}

long live_call_index(const live *l, ccord_id channel_id)
{
  for (size_t i = 0; i < l->dm_call_count; i++) {
    if (l->dm_calls[i].channel_id == channel_id) return (long)i;
  }
  return -1;
}

bool live_call_drop_ringing(ccord_call_state *call, ccord_id user_id)
{
  if (!call || user_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < call->ringing_count; i++) {
    if (call->ringing[i] == user_id) {
      memmove(&call->ringing[i], &call->ringing[i + 1u], (call->ringing_count - i - 1u) * sizeof call->ringing[0]);
      call->ringing_count--;
      return true;
    }
  }
  return false;
}

static bool live_call_is_ringing_self(const live *l, ccord_id channel_id)
{
  if (l->self_id == CCORD_ID_NONE) return false;
  long ci = live_call_index(l, channel_id);
  if (ci < 0) return false;
  const ccord_call_state *call = &l->dm_calls[(size_t)ci];
  for (size_t i = 0; i < call->ringing_count; i++) {
    if (call->ringing[i] == l->self_id) return true;
  }
  return false;
}

void live_call_on_event(live *l, const ccord_net_event *ev)
{
  if (!ev || ev->channel_id == CCORD_ID_NONE) return;
  if (ev->type == CCORD_NET_CALL_CREATE || ev->type == CCORD_NET_CALL_UPDATE) {
    if (!ev->call) return;
    long ci = live_call_index(l, ev->channel_id);
    if (ci >= 0) {
      l->dm_calls[(size_t)ci] = *ev->call;
    } else {
      if (l->dm_call_count == LIVE_CALLS_MAX) {
        memmove(&l->dm_calls[0], &l->dm_calls[1], (LIVE_CALLS_MAX - 1u) * sizeof l->dm_calls[0]);
        l->dm_call_count = LIVE_CALLS_MAX - 1u;
      }
      l->dm_calls[l->dm_call_count++] = *ev->call;
    }
  } else if (ev->type == CCORD_NET_CALL_DELETE) {
    long ci = live_call_index(l, ev->channel_id);
    if (ci >= 0) {
      memmove(&l->dm_calls[(size_t)ci], &l->dm_calls[(size_t)ci + 1u],
              (l->dm_call_count - (size_t)ci - 1u) * sizeof l->dm_calls[0]);
      l->dm_call_count--;
    }
    if (l->voice_guild == CCORD_DM_GUILD_ID && l->voice_channel == ev->channel_id) {
      live_streams_stop_all(l);
      live_voice_stop(l);
      l->voice_guild = CCORD_ID_NONE;
      l->voice_channel = CCORD_ID_NONE;
      l->self_video = false;
      l->streaming = false;
      l->self_suppressed = false;
    }
  }
}

ccord_status live_join_voice(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE || channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: voice ids");
  }
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (!l->watching_stream || l->watched_guild != guild_id || l->watched_channel != channel_id) {
    live_streams_stop_all(l);
  } else if (l->streaming) {
    if (l->tx_stream.stream_key[0] != '\0') {
      ccord_error ignored;
      (void)ccord_gateway_send_stream_delete_key(l->gw, l->tx_stream.stream_key, &ignored);
    }
    live_vstream_slot_reset(l, &l->tx_stream);
    l->streaming = false;
    l->tx_stream_paused = false;
  }
  live_voice_stop(l);
  ccord_status st = ccord_gateway_send_voice_state(l->gw, guild_id, channel_id, l->self_mute, l->self_deaf,
                                                    l->self_video, false, err);
  if (st == CCORD_OK) {
    l->voice_guild = guild_id;
    l->voice_channel = channel_id;
    l->streaming = false;
    l->self_suppressed = false;
  }
  return st;
}

ccord_status live_leave_voice(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: voice guild id");
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  ccord_status st = ccord_gateway_send_voice_state(l->gw, guild_id, CCORD_ID_NONE, l->self_mute, l->self_deaf,
                                                    false, false, err);
  if (st == CCORD_OK) {
    if (l->voice_guild == guild_id) {
      live_streams_stop_all(l);
      live_voice_stop(l);
      l->voice_guild = CCORD_ID_NONE;
      l->voice_channel = CCORD_ID_NONE;
      l->self_video = false;
      l->streaming = false;
      l->self_suppressed = false;
    }
  }
  return st;
}

ccord_status live_update_voice_state(ccord_transport *t, ccord_id guild_id, bool self_mute, bool self_deaf,
                                            ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE) {
    live_set_self_voice_flags(l, self_mute, self_deaf);
    return CCORD_OK;
  }
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  ccord_id channel_id = CCORD_ID_NONE;
  bool self_video = l->self_video;
  bool self_stream = l->streaming;
  if (l->voice_guild == guild_id && l->voice_channel != CCORD_ID_NONE) {
    channel_id = l->voice_channel;
  } else if (l->self_id != CCORD_ID_NONE) {
    const ccord_voice_state *cached = live_voice_cache_find(l, guild_id, l->self_id);
    if (cached && cached->channel_id != CCORD_ID_NONE) {
      channel_id = cached->channel_id;
      self_video = cached->self_video;
      self_stream = cached->self_stream;
    }
  }
  if (channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "live: not connected to voice");
  }
  ccord_status st = ccord_gateway_send_voice_state(l->gw, guild_id, channel_id, self_mute, self_deaf,
                                                   self_video, self_stream, err);
  if (st == CCORD_OK) live_set_self_voice_flags(l, self_mute, self_deaf);
  return st;
}


ccord_status live_set_voice_camera(ccord_transport *t, ccord_id guild_id, bool enabled, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (l->voice_guild != guild_id || l->voice_channel == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "live: not connected to voice");
  }
  ccord_status st = ccord_gateway_send_voice_state(l->gw, guild_id, l->voice_channel, l->self_mute, l->self_deaf,
                                                    enabled, l->streaming, err);
  if (st == CCORD_OK) {
    l->self_video = enabled;
    l->voice_video_synced = false;
    if (l->voice) {
      ccord_error verr;
      // Populate the descriptor before OP 12 so the standalone camera advertises its caps even when
      // the voice gateway is still handshaking (live_poll re-sends OP 12 once ESTABLISHED).
      if (enabled && ccord_voice_gateway_set_video_desc(l->voice, &k_live_camera_desc, &verr) != CCORD_OK) {
        CCORD_LOG_W("live: voice camera descriptor failed: %s", verr.message);
      }
      ccord_voice_connection_state vs = ccord_voice_gateway_state(l->voice);
      if (vs == CCORD_VOICE_READY || vs == CCORD_VOICE_ESTABLISHED) {
        if (ccord_voice_gateway_set_video(l->voice, enabled, &verr) != CCORD_OK) {
          CCORD_LOG_W("live: voice video signalling failed: %s", verr.message);
        } else {
          l->voice_video_synced = enabled;
        }
      }
      if (!enabled) (void)ccord_voice_gateway_set_video_desc(l->voice, nullptr, &verr);
    }
  }
  return st;
}


ccord_status live_push_voice_pcm(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  ccord_voice_gateway *vg = live_voice_acquire(l);
  if (!vg) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "live: voice gateway not connected");
  // Audio-thread side of the PTT handshake: read the UI-published mode atomically and arm the release deadline.
  if (atomic_load(&l->ptt_enabled) && samples > 0) {
    ccord_voice_gateway_set_ptt(vg, true);
    atomic_store(&l->ptt_active, true);
    atomic_store(&l->ptt_release_ms, live_now(l) + 60u);
  }
  ccord_status st = ccord_voice_gateway_push_pcm(vg, pcm, samples, err);
  live_voice_release(l);
  return st;
}

size_t live_pull_voice_pcm(ccord_transport *t, int16_t *pcm, size_t max_samples)
{
  live *l = (live *)t;
  ccord_voice_gateway *vg = live_voice_acquire(l);
  if (!vg) return 0;
  size_t n = ccord_voice_gateway_pull_pcm(vg, pcm, max_samples);
  live_voice_release(l);
  return n;
}

static uint16_t live_rd_u16le(const uint8_t *p)
{
  return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static uint32_t live_rd_u32le(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool live_decode_wav_s16(const uint8_t *data, size_t len, int16_t **out_pcm, size_t *out_samples)
{
  *out_pcm = nullptr;
  *out_samples = 0;
  if (!data || len < 44u || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return false;
  uint16_t fmt = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  const uint8_t *pcm_bytes = nullptr;
  size_t pcm_len = 0;
  size_t pos = 12u;
  while (pos + 8u <= len) {
    const uint8_t *hdr = data + pos;
    uint32_t csz = live_rd_u32le(hdr + 4);
    pos += 8u;
    if (csz > len - pos) return false;
    if (memcmp(hdr, "fmt ", 4) == 0) {
      if (csz < 16u) return false;
      fmt = live_rd_u16le(data + pos);
      channels = live_rd_u16le(data + pos + 2u);
      rate = live_rd_u32le(data + pos + 4u);
      bits = live_rd_u16le(data + pos + 14u);
    } else if (memcmp(hdr, "data", 4) == 0) {
      pcm_bytes = data + pos;
      pcm_len = (size_t)csz;
      break;
    }
    pos += (size_t)csz + ((csz & 1u) != 0 && (size_t)csz < len - pos ? 1u : 0u);
  }
  if (fmt != 1u || (channels != 1u && channels != 2u) || bits != 16u || rate < 8000u || rate > 96000u ||
      !pcm_bytes || pcm_len < (size_t)channels * 2u) {
    return false;
  }
  size_t src_frames = pcm_len / ((size_t)channels * 2u);
  size_t dst_frames = (size_t)(((uint64_t)src_frames * 48000u) / (uint64_t)rate);
  if (dst_frames == 0) return false;
  if (dst_frames > LIVE_SOUNDBOARD_MAX_SAMPLES) dst_frames = LIVE_SOUNDBOARD_MAX_SAMPLES;
  int16_t *pcm = ccord_alloc_array(dst_frames * 2u, sizeof *pcm);
  if (!pcm) return false;
  for (size_t i = 0; i < dst_frames; i++) {
    size_t si = (size_t)(((uint64_t)i * (uint64_t)rate) / 48000u);
    if (si >= src_frames) si = src_frames - 1u;
    const uint8_t *sp = pcm_bytes + si * (size_t)channels * 2u;
    uint16_t ul = live_rd_u16le(sp);
    uint16_t ur = channels == 2u ? live_rd_u16le(sp + 2u) : ul;
    int16_t sl = 0, sr = 0;
    memcpy(&sl, &ul, sizeof sl);
    memcpy(&sr, &ur, sizeof sr);
    pcm[i * 2u] = sl;
    pcm[i * 2u + 1u] = sr;
  }
  *out_pcm = pcm;
  *out_samples = dst_frames;
  return true;
}

void live_soundboard_done(live *l, const live_rest_op *op, const char *body, size_t len)
{
  if (live_api_cache_enabled(l) && op->expr_id != CCORD_ID_NONE) {
    char url[160];
    (void)snprintf(url, sizeof url, "https://cdn.discordapp.com/soundboard-sounds/%llu.mp3",
                   (unsigned long long)op->expr_id);
    ccord_error merr;
    (void)live_fetch_media((ccord_transport *)l, url, &merr);
  }
  if (!l->voice || l->voice_guild != op->guild_id || l->voice_channel != op->channel_id) return;
  int16_t *pending = nullptr;
  size_t samples = 0;
  if (body && len >= 4u && memcmp(body, "RIFF", 4) == 0) {
    if (!live_decode_wav_s16((const uint8_t *)body, len, &pending, &samples)) {
      CCORD_LOG_W("live: soundboard WAV decode failed");
      return;
    }
  } else {
    samples = CCORD_VOICE_FRAME_SAMPLES;
    pending = ccord_alloc_array(samples * 2u, sizeof *pending);
    if (!pending) return;
    for (size_t i = 0; i < CCORD_VOICE_FRAME_SAMPLES; i++) {
      int16_t s = (i % 48u) < 24u ? (int16_t)3000 : (int16_t)-3000;
      pending[i * 2u] = s;
      pending[i * 2u + 1u] = s;
    }
  }
  // A new play replaces any clip still queued. Store the decoded PCM and let live_soundboard_feed drip it into the
  // 1 s voice-gateway ring in <= 20 ms chunks; a single push of a >1 s clip is rejected with CCORD_ERR_RANGE.
  ccord_free(l->sb_pending_pcm);
  l->sb_pending_pcm = pending;
  l->sb_pending_samples = samples;
  l->sb_pending_offset = 0;
  live_soundboard_feed(l);
}

// Feed the queued soundboard clip into the voice ring one 20 ms frame at a time. The ring only holds 1 s, so a long
// clip must be paced: on CCORD_ERR_RANGE we stop and retry from live_poll once the transmit pump has drained space.
void live_soundboard_feed(live *l)
{
  if (!l->sb_pending_pcm || !l->voice) return;
  while (l->sb_pending_offset < l->sb_pending_samples) {
    size_t remain = l->sb_pending_samples - l->sb_pending_offset;
    size_t chunk = remain < CCORD_VOICE_FRAME_SAMPLES ? remain : CCORD_VOICE_FRAME_SAMPLES;
    bool first_chunk = l->sb_pending_offset == 0;
    ccord_error verr;
    ccord_status st = ccord_voice_gateway_push_soundboard_pcm(
        l->voice, l->sb_pending_pcm + l->sb_pending_offset * 2u, chunk, &verr);
    if (st == CCORD_ERR_RANGE) return;   // ring full; retry on the next poll
    if (st != CCORD_OK) {
      // Session gone or not established: drop the clip rather than spin on it.
      ccord_free(l->sb_pending_pcm);
      l->sb_pending_pcm = nullptr;
      l->sb_pending_samples = 0;
      l->sb_pending_offset = 0;
      return;
    }
    l->sb_pending_offset += chunk;
    if (first_chunk) {
      if (!l->sb_speaking && l->self_id != CCORD_ID_NONE) {
        ccord_live_event sev;
        ccord_live_event_init(&sev);
        sev.ev.type = CCORD_NET_SPEAKING;
        sev.ev.guild_id = l->voice_guild;
        sev.ev.channel_id = l->voice_channel;
        sev.ev.user_id = l->self_id;
        sev.ev.speaking = true;
        (void)live_push(l, &sev);
      }
      l->sb_speaking = true;
      uint64_t dur_ms = ((uint64_t)l->sb_pending_samples * 1000u) / 48000u;
      if (dur_ms < 40u) dur_ms = 40u;
      l->sb_speaking_until_ms = live_now(l) + dur_ms;
    }
  }
  // Fully queued: the ring now owns the audio and drains it at 20 ms/frame.
  ccord_free(l->sb_pending_pcm);
  l->sb_pending_pcm = nullptr;
  l->sb_pending_samples = 0;
  l->sb_pending_offset = 0;
}

// Starting a DM / group DM call rings its recipients: the voice-state update alone only joins. Body {"recipients":null}
// rings everyone (discord.py-self PrivateCall.ring -> http.ring(channel_id), POST /channels/{c}/call/ring). Best effort:
// a refusal arrives as ACTION_FAILED with Discord's reason and the user stays in the call.
static void live_call_ring(live *l, ccord_id channel_id)
{
  char path[96];
  (void)snprintf(path, sizeof path, "/channels/%llu/call/ring", (unsigned long long)channel_id);
  live_rest_op *op = live_action_op(l, LIVE_REST_CALL_RING, channel_id, CCORD_ID_NONE, nullptr, 0);
  if (!op) return;
  op->guild_id = CCORD_DM_GUILD_ID;
  ccord_error rerr;
  if (live_action_submit(l, CCORD_REST_POST, path, "{\"recipients\":null}", op, &rerr) != CCORD_OK) {
    CCORD_LOG_W("live: could not queue the call ring: %s", rerr.message);
  }
}

ccord_status live_call_action(ccord_transport *t, ccord_id channel_id, ccord_call_action act, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l || channel_id == CCORD_ID_NONE || (act != CCORD_CALL_JOIN && act != CCORD_CALL_LEAVE)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: bad call action");
  }
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  bool in_call = l->voice_guild == CCORD_DM_GUILD_ID && l->voice_channel == channel_id;
  if (act == CCORD_CALL_JOIN) {
    if (in_call) return ccord_error_set(err, CCORD_ERR_INVALID, "live: already in this call");
    // A call row exists once anyone has started it (CALL_CREATE); joining that rings nobody new.
    bool call_in_progress = live_call_index(l, channel_id) >= 0;
    ccord_status st = ccord_gateway_send_voice_state(l->gw, CCORD_DM_GUILD_ID, channel_id, l->self_mute,
                                                     l->self_deaf, false, false, err);
    if (st == CCORD_OK) {
      if (!l->watching_stream || l->watched_guild != CCORD_DM_GUILD_ID || l->watched_channel != channel_id) {
        live_streams_stop_all(l);
      } else if (l->streaming) {
        if (l->tx_stream.stream_key[0] != '\0') {
          ccord_error ignored;
          (void)ccord_gateway_send_stream_delete_key(l->gw, l->tx_stream.stream_key, &ignored);
        }
        live_vstream_slot_reset(l, &l->tx_stream);
        l->streaming = false;
        l->tx_stream_paused = false;
      }
      live_voice_stop(l);
      l->voice_guild = CCORD_DM_GUILD_ID;
      l->voice_channel = channel_id;
      l->self_video = false;
      l->streaming = false;
      l->self_suppressed = false;
      long ci = live_call_index(l, channel_id);
      if (ci >= 0) (void)live_call_drop_ringing(&l->dm_calls[(size_t)ci], l->self_id);
      if (!call_in_progress) live_call_ring(l, channel_id);
    }
    return st;
  }
  bool ringing = live_call_is_ringing_self(l, channel_id);
  if (!in_call && !ringing) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "live: not in this call");
  if (!in_call) {
    // Ringing-only decline goes through the governed REST queue (POST /channels/{c}/call/stop-ringing). A Gateway
    // OP 4 {guild_id:null, channel_id:null} here would also drop any active guild voice. No local mutation: the
    // completion drops self from ringing[] only after Discord answers 204.
    ccord_admin_request *req = ccord_calloc(1, sizeof *req);
    if (!req) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    ccord_status st = ccord_admin_call_stop_ringing(req, channel_id, err);
    if (st == CCORD_OK) st = live_admin_action(t, req, err);
    ccord_free(req);
    return st;
  }
  ccord_status st = ccord_gateway_send_voice_state(l->gw, CCORD_DM_GUILD_ID, CCORD_ID_NONE, l->self_mute,
                                                   l->self_deaf, false, false, err);
  if (st == CCORD_OK) {
    long ci = live_call_index(l, channel_id);
    if (ci >= 0) (void)live_call_drop_ringing(&l->dm_calls[(size_t)ci], l->self_id);
    live_streams_stop_all(l);
    live_voice_stop(l);
    l->voice_guild = CCORD_ID_NONE;
    l->voice_channel = CCORD_ID_NONE;
    l->self_video = false;
    l->streaming = false;
    l->self_suppressed = false;
  }
  return st;
}

ccord_status live_play_soundboard(ccord_transport *t, ccord_id guild_id, ccord_id sound_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID || sound_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: soundboard ids");
  }
  if (l->voice_guild != guild_id || l->voice_channel == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: join voice before playing sounds");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  char path[128];
  (void)snprintf(path, sizeof path, "/channels/%llu/send-soundboard-sound", (unsigned long long)l->voice_channel);
  ccord_buf body;
  ccord_buf_init(&body, 128);
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "sound_id");
  ccord_jw_id(&w, sound_id);
  ccord_jw_key(&w, "source_guild_id");
  ccord_jw_id(&w, guild_id);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: soundboard body failed");
  }
  live_rest_op *op = live_action_op(l, LIVE_REST_SOUNDBOARD, l->voice_channel, CCORD_ID_NONE, nullptr, 0);
  if (!op) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->guild_id = guild_id;
  op->expr_id = sound_id;
  ccord_status st = live_action_submit(l, CCORD_REST_POST, path, ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&body);
  return st;
}

ccord_status live_set_user_volume(ccord_transport *t, ccord_id user_id, uint8_t pct, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l || user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid user");
  if (pct > 200u) return ccord_error_set(err, CCORD_ERR_RANGE, "live: volume must be 0..200");
  bool found = false;
  for (size_t i = 0; i < l->user_volume_count; i++) {
    if (l->user_volume[i].user_id == user_id) {
      l->user_volume[i].pct = pct;
      found = true;
      break;
    }
  }
  if (!found) {
    if (l->user_volume_count == CCORD_SPEAK_MAX) {
      memmove(&l->user_volume[0], &l->user_volume[1], (CCORD_SPEAK_MAX - 1u) * sizeof l->user_volume[0]);
      l->user_volume_count = CCORD_SPEAK_MAX - 1u;
    }
    l->user_volume[l->user_volume_count++] = (live_user_vol){.user_id = user_id, .pct = pct};
  }
  if (l->voice) ccord_voice_gateway_set_user_volume(l->voice, user_id, pct);
  return CCORD_OK;
}

ccord_status live_set_voice_config(ccord_transport *t, const ccord_voice_config *cfg, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l || !cfg) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null voice config");
  if (cfg->vad_sensitivity_pct > 100u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: sensitivity must be 0..100");
  }
  l->voice_cfg = *cfg;
  l->has_voice_cfg = true;
  atomic_store(&l->ptt_enabled, cfg->ptt);
  if (l->voice) ccord_voice_gateway_set_dsp(l->voice, cfg);
  return CCORD_OK;
}


