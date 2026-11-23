// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sess_voice_track_upsert(ccord_session *s, ccord_id guild_id, ccord_id channel_id, ccord_id user_id)
{
  if (user_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < s->nvoice_track; i++) {
    if (s->voice_track[i].guild_id == guild_id && s->voice_track[i].user_id == user_id) {
      if (channel_id == CCORD_ID_NONE) {
        s->voice_track[i] = s->voice_track[--s->nvoice_track];
      } else {
        s->voice_track[i].channel_id = channel_id;
      }
      return;
    }
  }
  if (channel_id == CCORD_ID_NONE) return;
  if (s->nvoice_track == s->capvoice_track) {
    size_t ncap = s->capvoice_track ? s->capvoice_track * 2u : 16u;
    session_voice_entry *g = ccord_realloc_array(s->voice_track, ncap, sizeof *g);
    if (!g) {
      s->voice_track_synced = false;
      return;
    }
    s->voice_track = g;
    s->capvoice_track = ncap;
  }
  s->voice_track[s->nvoice_track++] = (session_voice_entry){
    .guild_id = guild_id,
    .channel_id = channel_id,
    .user_id = user_id,
  };
}

void sess_voice_track_remove_guild(ccord_session *s, ccord_id guild_id)
{
  for (size_t i = 0; i < s->nvoice_track; ) {
    if (s->voice_track[i].guild_id == guild_id) {
      s->voice_track[i] = s->voice_track[--s->nvoice_track];
    } else {
      i++;
    }
  }
}

void sess_voice_track_remove_channel(ccord_session *s, ccord_id channel_id)
{
  for (size_t i = 0; i < s->nvoice_track; ) {
    if (s->voice_track[i].channel_id == channel_id) {
      s->voice_track[i] = s->voice_track[--s->nvoice_track];
    } else {
      i++;
    }
  }
}

static size_t voice_id_slot(ccord_id id, size_t mask)
{
  uint64_t x = (uint64_t)id;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return (size_t)(x & (uint64_t)mask);
}

static bool voice_keep_lookup(const ccord_id *keep_set, size_t keep_mask,
                              const ccord_voice_state *states, size_t count,
                              ccord_id guild_id, ccord_id user_id)
{
  if (keep_set) {
    size_t b = voice_id_slot(user_id, keep_mask);
    while (keep_set[b] != CCORD_ID_NONE) {
      if (keep_set[b] == user_id) return true;
      b = (b + 1u) & keep_mask;
    }
    return false;
  }
  for (size_t k = 0; states && k < count; k++) {
    if (states[k].guild_id == guild_id && states[k].user_id == user_id &&
        states[k].channel_id != CCORD_ID_NONE) {
      return true;
    }
  }
  return false;
}

static bool push_stale_voice_id(ccord_id **buf, ccord_id **heap_buf, size_t *count, size_t *cap, ccord_id uid)
{
  if (*count == *cap) {
    size_t ncap = (*cap) * 2u;
    ccord_id *g = ccord_alloc_array(ncap, sizeof *g);
    if (!g) return false;
    memcpy(g, *buf, (*count) * sizeof *g);
    ccord_free(*heap_buf);
    *heap_buf = g;
    *buf = g;
    *cap = ncap;
  }
  (*buf)[(*count)++] = uid;
  return true;
}

void session_apply_voice_state(ccord_session *s, const ccord_voice_state *raw_vs, const ccord_user *opt_user)
{
  if (!s || !raw_vs) return;
  ccord_error err;
  if (opt_user) {
    (void)ccord_store_upsert_user(s->store, opt_user, &err);
    ccord_error_clear(&err);
  } else if (raw_vs->channel_id != CCORD_ID_NONE && ccord_store_user(s->store, raw_vs->user_id) == nullptr) {
    char idbuf[21];
    ccord_id_format(raw_vs->user_id, idbuf);
    char namebuf[sizeof("User ") + sizeof idbuf];
    memcpy(namebuf, "User ", 5);
    size_t idlen = strlen(idbuf);
    memcpy(namebuf + 5, idbuf, idlen + 1);
    ccord_user stub = {
      .id = raw_vs->user_id,
      .name = namebuf,
      .status_text = "",
      .presence = CCORD_PRESENCE_ONLINE,
      .is_bot = false,
      .avatar_palette = (uint8_t)(raw_vs->user_id % 6u),
    };
    (void)ccord_store_upsert_user(s->store, &stub, &err);
    ccord_error_clear(&err);
  }
  // A partial gateway payload may omit suppress / request_to_speak_timestamp: keep the last known
  // stage state instead of silently clearing it (the omit flags say which fields were not sent).
  ccord_voice_state vs = *raw_vs;
  const ccord_voice_state *cur = ccord_store_voice_user(s->store, vs.guild_id, vs.user_id);
  ccord_id prev_voice_ch = cur ? cur->channel_id : CCORD_ID_NONE;
  bool was_streaming = cur ? cur->self_stream : false;
  if (vs.omit_suppress || vs.omit_request_to_speak) {
    if (cur) {
      if (vs.omit_suppress) vs.suppress = cur->suppress;
      if (vs.omit_request_to_speak) vs.request_to_speak_timestamp = cur->request_to_speak_timestamp;
    }
    vs.omit_suppress = false;
    vs.omit_request_to_speak = false;
  }
  if (ccord_store_set_voice_state(s->store, &vs, &err) != CCORD_OK) {
    if (err.code != CCORD_ERR_NOT_FOUND) {
      CCORD_LOG_W("session: voice state rejected: %s", err.message);
    }
  } else {
    sess_voice_track_upsert(s, vs.guild_id, vs.channel_id, vs.user_id);
  }
  if (vs.user_id == ccord_store_self_id(s->store)) {
    s->pending_voice_guild = CCORD_ID_NONE;
    s->pending_voice_channel = CCORD_ID_NONE;
    if (vs.channel_id != CCORD_ID_NONE) {
      s->voice_self_mute = vs.self_mute;
      s->voice_self_deaf = vs.self_deaf;
    }
  }
  // Our own connection health belongs to one voice session: leaving or moving to another channel ends it, so a
  // later call never starts out showing "reconnecting" from the previous one.
  if (vs.user_id == ccord_store_self_id(s->store) && vs.channel_id != prev_voice_ch &&
      (vs.channel_id == CCORD_ID_NONE || prev_voice_ch != CCORD_ID_NONE)) {
    ccord_rtc_stats none = {0};
    (void)ccord_store_set_rtc_stats(s->store, &none, &err);
    ccord_error_clear(&err);
  }
  // The signed-in user left (or moved out of) a voice channel: Discord keeps a participant in an Activity until
  // told otherwise, so leave every instance there that still lists us (no ghost participant behind).
  if (prev_voice_ch != CCORD_ID_NONE && vs.channel_id != prev_voice_ch && vs.user_id == ccord_store_self_id(s->store)) {
    session_leave_channel_activities(s, prev_voice_ch);
    if (vs.channel_id == CCORD_ID_NONE) {
      ccord_free(s->cam_frame_buf);
      s->cam_frame_buf = nullptr;
    }
  }
  if (prev_voice_ch != CCORD_ID_NONE && (vs.channel_id != prev_voice_ch || (was_streaming && !vs.self_stream))) {
    (void)ccord_store_stream_remove(s->store, prev_voice_ch, vs.user_id, &err);
    if (s->unsub_channel_id == prev_voice_ch && s->unsub_user_id == vs.user_id) {
      s->unsub_channel_id = CCORD_ID_NONE;
      s->unsub_user_id = CCORD_ID_NONE;
    }
  }
  if (s->watched.active && vs.user_id == s->watched.user_id && vs.guild_id == s->watched.guild_id) {
    if (vs.channel_id != s->watched.channel_id || !vs.self_stream) {
      s->watched.active = false;
      atomic_store_explicit(&s->watched_active, false, memory_order_release);
      s->watched.paused = false;
      ccord_free(s->stream_frame_buf);
      s->stream_frame_buf = nullptr;
    }
  }
}

void session_apply_voice_states(ccord_session *s, ccord_id replace_guild_id, const ccord_voice_state *states,
                                       size_t count, const ccord_user *users, size_t user_count)
{
  if (!s) return;
  ccord_error err;
  for (size_t u = 0; users && u < user_count; u++) {
    (void)ccord_store_upsert_user(s->store, &users[u], &err);
    ccord_error_clear(&err);
  }
  if (replace_guild_id != CCORD_ID_NONE && replace_guild_id != CCORD_DM_GUILD_ID) {
    ccord_id keep_stack[128];
    ccord_id *keep_heap = nullptr;
    ccord_id *keep_set = nullptr;
    size_t keep_mask = 0;
    if (states && count > 8u) {
      size_t keep_cap = 32u;
      while (keep_cap < count * 2u && keep_cap <= (SIZE_MAX >> 1)) keep_cap <<= 1u;
      if (keep_cap <= sizeof keep_stack / sizeof keep_stack[0]) {
        memset(keep_stack, 0, keep_cap * sizeof keep_stack[0]);
        keep_set = keep_stack;
      } else {
        keep_heap = ccord_calloc(keep_cap, sizeof *keep_heap);
        keep_set = keep_heap;
      }
      if (keep_set) {
        keep_mask = keep_cap - 1u;
        for (size_t k = 0; k < count; k++) {
          if (states[k].guild_id != replace_guild_id || states[k].channel_id == CCORD_ID_NONE ||
              states[k].user_id == CCORD_ID_NONE) {
            continue;
          }
          size_t b = voice_id_slot(states[k].user_id, keep_mask);
          while (keep_set[b] != CCORD_ID_NONE && keep_set[b] != states[k].user_id) {
            b = (b + 1u) & keep_mask;
          }
          keep_set[b] = states[k].user_id;
        }
      }
    }

    ccord_id stale_stack[64];
    ccord_id *stale_heap = nullptr;
    ccord_id *stale = stale_stack;
    size_t nstale = 0;
    size_t stale_cap = sizeof stale_stack / sizeof stale_stack[0];

    if (s->voice_track_synced) {
      for (size_t i = 0; i < s->nvoice_track; i++) {
        if (s->voice_track[i].guild_id != replace_guild_id) continue;
        ccord_id uid = s->voice_track[i].user_id;
        if (!voice_keep_lookup(keep_set, keep_mask, states, count, replace_guild_id, uid)) {
          if (!push_stale_voice_id(&stale, &stale_heap, &nstale, &stale_cap, uid)) break;
        }
      }
    } else {
      size_t nc = ccord_store_channel_count(s->store, replace_guild_id);
      for (size_t ci = 0; ci < nc; ci++) {
        const ccord_channel *ch = ccord_store_channel_at(s->store, replace_guild_id, ci);
        if (!ch || (ch->kind != CCORD_CHANNEL_VOICE && ch->kind != CCORD_CHANNEL_STAGE)) continue;
        for (size_t vi = 0; ; vi++) {
          const ccord_voice_state *cur = ccord_store_voice_at(s->store, ch->id, vi);
          if (!cur) break;
          if (!voice_keep_lookup(keep_set, keep_mask, states, count, replace_guild_id, cur->user_id)) {
            if (!push_stale_voice_id(&stale, &stale_heap, &nstale, &stale_cap, cur->user_id)) break;
          }
        }
      }
    }

    ccord_free(keep_heap);
    for (size_t i = 0; i < nstale; i++) {
      ccord_voice_state leave = {.guild_id = replace_guild_id, .channel_id = CCORD_ID_NONE, .user_id = stale[i]};
      session_apply_voice_state(s, &leave, nullptr);
    }
    ccord_free(stale_heap);
  }
  for (size_t i = 0; states && i < count; i++) {
    session_apply_voice_state(s, &states[i], nullptr);
  }
}


const ccord_voice_state *sess_self_voice(const ccord_session *s, ccord_id guild_id)
{
  return ccord_store_voice_user(s->store, guild_id, ccord_store_self_id(s->store));
}

static const ccord_voice_state *find_active_self_voice(const ccord_session *s)
{
  ccord_id self_id = ccord_store_self_id(s->store);
  if (self_id == CCORD_ID_NONE) return nullptr;
  ccord_id ch_id = ccord_store_active_voice_channel(s->store);
  if (ch_id == CCORD_ID_NONE) return nullptr;
  const ccord_channel *ch = ccord_store_channel(s->store, ch_id);
  if (ch) {
    const ccord_voice_state *v = ccord_store_voice_user(s->store, ch->guild_id, self_id);
    if (v && v->channel_id != CCORD_ID_NONE) return v;
  }
  const ccord_voice_state *dm = ccord_store_voice_user(s->store, CCORD_DM_GUILD_ID, self_id);
  if (dm && dm->channel_id != CCORD_ID_NONE) return dm;
  size_t nv = ccord_store_voice_count(s->store, ch_id);
  for (size_t i = 0; i < nv; i++) {
    const ccord_voice_state *v = ccord_store_voice_at(s->store, ch_id, i);
    if (v && v->user_id == self_id && v->channel_id != CCORD_ID_NONE) return v;
  }
  return nullptr;
}

ccord_status ccord_session_join_voice(ccord_session *s, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!ccord_store_guild(s->store, guild_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown guild");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch || ch->guild_id != guild_id) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown voice channel");
  // Stage channels join as audience: the server answers with suppress=true until invited to speak.
  if (ch->kind != CCORD_CHANNEL_VOICE && ch->kind != CCORD_CHANNEL_STAGE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "channel is not a voice channel");
  }
  if (guild_id != CCORD_DM_GUILD_ID) {
    if (ccord_session_self_pending(s, guild_id)) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION,
                             "Complete the server's membership screening before joining voice.");
    }
    ccord_id self = ccord_store_self_id(s->store);
    if (ccord_store_permissions_exact(s->store, guild_id, self)) {
      int64_t now = session_now_unix_ms(s);
      uint64_t perms = ccord_store_permissions(s->store, guild_id, channel_id, self, now);
      if (!ccord_perm_has(perms, CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_CONNECT)) {
        const ccord_member *me = ccord_store_find_member(s->store, guild_id, self);
        if (me && me->timeout_until_ms > now) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You have been timed out and cannot join voice.");
        }
        return ccord_error_set(err, CCORD_ERR_PERMISSION,
                               "You do not have permission to connect to this voice channel.");
      }
      if (ch->kind == CCORD_CHANNEL_VOICE && ch->user_limit > 0) {
        const ccord_voice_state *cur = sess_self_voice(s, guild_id);
        bool already_here = cur && cur->channel_id == channel_id;
        if (!already_here && !ccord_perm_has(perms, CCORD_PERM_MOVE_MEMBERS) &&
            ccord_store_voice_count(s->store, channel_id) >= ch->user_limit) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "This voice channel is full.");
        }
      }
    }
  }
  ccord_status st = ccord_transport_join_voice(s->transport, guild_id, channel_id, err);
  if (st == CCORD_OK) {
    s->pending_voice_guild = guild_id;
    s->pending_voice_channel = channel_id;
  }
  return st;
}

ccord_status ccord_session_leave_voice(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_voice_state *v = sess_self_voice(s, guild_id);
  bool pending = s->pending_voice_guild == guild_id && s->pending_voice_channel != CCORD_ID_NONE;
  if ((!v || v->channel_id == CCORD_ID_NONE) && !pending) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: not connected to voice");
  }
  ccord_status st = ccord_transport_leave_voice(s->transport, guild_id, err);
  if (st == CCORD_OK) {
    if (pending) {
      s->pending_voice_guild = CCORD_ID_NONE;
      s->pending_voice_channel = CCORD_ID_NONE;
    }
    ccord_free(s->cam_frame_buf);
    s->cam_frame_buf = nullptr;
  }
  return st;
}

static ccord_status session_update_self_voice(ccord_session *s, ccord_id gid, bool next_mute, bool next_deaf,
                                              ccord_error *err)
{
  if (gid != CCORD_ID_NONE || (s->transport && s->transport->vt && s->transport->vt->update_voice_state)) {
    ccord_status st = ccord_transport_update_voice_state(s->transport, gid, next_mute, next_deaf, err);
    if (st != CCORD_OK) return st;
  }
  s->voice_self_mute = next_mute;
  s->voice_self_deaf = next_deaf;
  return CCORD_OK;
}

ccord_status ccord_session_set_voice_mute(ccord_session *s, bool muted, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_voice_state *v = find_active_self_voice(s);
  ccord_id gid = v ? v->guild_id : s->pending_voice_guild;
  bool cur_deaf = v ? v->self_deaf : s->voice_self_deaf;
  bool next_deaf = !muted ? false : cur_deaf;
  return session_update_self_voice(s, gid, muted, next_deaf, err);
}

ccord_status ccord_session_set_voice_deafen(ccord_session *s, bool deafened, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_voice_state *v = find_active_self_voice(s);
  ccord_id gid = v ? v->guild_id : s->pending_voice_guild;
  bool cur_mute = v ? v->self_mute : s->voice_self_mute;
  // Deafened implies muted in Discord; keep both flags consistent.
  bool next_mute = deafened ? true : cur_mute;
  return session_update_self_voice(s, gid, next_mute, deafened, err);
}

bool ccord_session_voice_muted(const ccord_session *s)
{
  if (!s) return false;
  const ccord_voice_state *v = find_active_self_voice(s);
  return v ? v->self_mute : s->voice_self_mute;
}

bool ccord_session_voice_deafened(const ccord_session *s)
{
  if (!s) return false;
  const ccord_voice_state *v = find_active_self_voice(s);
  return v ? v->self_deaf : s->voice_self_deaf;
}

ccord_status ccord_session_start_stream(ccord_session *s, ccord_id guild_id, ccord_id channel_id,
                                        const ccord_stream_config *config, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!config) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null stream config");
  if (config->encoder > CCORD_STREAM_ENCODER_AUTO || config->codec > CCORD_STREAM_CODEC_AV1 ||
      config->framework > CCORD_STREAM_FRAMEWORK_AUTO || config->resolution > CCORD_STREAM_RES_SOURCE ||
      config->fps > CCORD_STREAM_FPS_60) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: stream config");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  bool dm_call = guild_id == CCORD_DM_GUILD_ID && ch && ch->guild_id == CCORD_DM_GUILD_ID &&
                 ch->kind == CCORD_CHANNEL_TEXT;
  bool guild_voice = ch && ch->guild_id == guild_id &&
                     (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE);
  if (!ch || (!dm_call && ch->guild_id != guild_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown voice channel");
  if (!dm_call && !guild_voice) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "channel is not a voice channel");
  }
  const ccord_voice_state *v = sess_self_voice(s, guild_id);
  if (!v || v->channel_id != channel_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: join the voice channel before streaming");
  }
  s->pending_stream_cfg = *config;
  return ccord_transport_start_stream(s->transport, guild_id, channel_id, config, err);
}

ccord_status ccord_session_stop_stream(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_stream_state *st = ccord_store_stream(s->store);
  if (!st || !st->active || st->guild_id != guild_id) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: no active stream");
  }
  return ccord_transport_stop_stream(s->transport, guild_id, err);
}

static void session_drain_watched_rx(ccord_session *s)
{
  if (!s || !s->transport) return;
  uint8_t small_buf[1024];
  uint8_t *fbuf = s->stream_frame_buf ? s->stream_frame_buf : small_buf;
  size_t fcap = s->stream_frame_buf ? SESSION_STREAM_FRAME_MAX : sizeof small_buf;
  for (size_t i = 0; i < 64; i++) {
    size_t out_len = 0;
    if (!ccord_transport_poll_stream_frame(s->transport, fbuf, fcap, &out_len, nullptr, nullptr)) break;
  }
  int16_t pcm_buf[960];
  for (size_t i = 0; i < 256; i++) {
    if (ccord_transport_poll_stream_pcm(s->transport, pcm_buf, sizeof pcm_buf / sizeof pcm_buf[0]) == 0) break;
  }
}

ccord_status ccord_session_watch_stream(ccord_session *s, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                        ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid stream target");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  // Guild broadcasts ride voice/stage channels; DM-call broadcasts ride the call's text channel.
  bool call_ch = ch && ch->guild_id == CCORD_DM_GUILD_ID && ch->kind == CCORD_CHANNEL_TEXT;
  bool voice_ch = ch && ch->guild_id == guild_id &&
                  (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE);
  if (!call_ch && !voice_ch) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: unknown voice channel");
  }
  if (call_ch && guild_id != CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid stream target");
  }
  if (s->watched.active && s->watched.guild_id == guild_id && s->watched.channel_id == channel_id &&
      s->watched.user_id == user_id) {
    ccord_error_clear(err);
    return CCORD_OK;
  }
  bool switching = s->watched.active && (s->watched.channel_id != channel_id || s->watched.user_id != user_id);
  ccord_id prev_ch = s->watched.channel_id;
  ccord_id prev_user = s->watched.user_id;
  if (switching) session_drain_watched_rx(s);
  ccord_status st = ccord_transport_watch_stream(s->transport, guild_id, channel_id, user_id, err);
  if (st == CCORD_OK) {
    if (switching) {
      s->unsub_channel_id = prev_ch;
      s->unsub_user_id = prev_user;
    } else {
      s->unsub_channel_id = CCORD_ID_NONE;
      s->unsub_user_id = CCORD_ID_NONE;
    }
    const ccord_stream_state *known = ccord_store_stream_find(s->store, channel_id, user_id);
    s->watched = (ccord_watched_stream){
      .guild_id = guild_id,
      .channel_id = channel_id,
      .user_id = user_id,
      .active = true,
      .paused = known ? known->paused : false,
    };
    atomic_store_explicit(&s->watched_active, true, memory_order_release);
  }
  return st;
}

ccord_status ccord_session_stop_watching(ccord_session *s, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!s->watched.active) return CCORD_OK;
  s->unsub_channel_id = CCORD_ID_NONE;
  s->unsub_user_id = CCORD_ID_NONE;
  ccord_status st = ccord_transport_stop_watching(s->transport, s->watched.guild_id, s->watched.channel_id,
                                                  s->watched.user_id, err);
  s->watched.active = false;
  atomic_store_explicit(&s->watched_active, false, memory_order_release);
  s->watched.paused = false;
  ccord_free(s->stream_frame_buf);
  s->stream_frame_buf = nullptr;
  return st;
}

ccord_status ccord_session_set_stream_paused(ccord_session *s, bool paused, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (s->watched.active) {
    ccord_status st = ccord_transport_set_stream_paused(s->transport, s->watched.guild_id, s->watched.channel_id,
                                                        s->watched.user_id, paused, err);
    if (st == CCORD_OK) s->watched.paused = paused;
    return st;
  }
  const ccord_stream_state *cur = ccord_store_stream(s->store);
  if (cur && cur->active) {
    ccord_id uid = cur->user_id != CCORD_ID_NONE ? cur->user_id : ccord_store_self_id(s->store);
    ccord_status st = ccord_transport_set_stream_paused(s->transport, cur->guild_id, cur->channel_id,
                                                        uid, paused, err);
    if (st == CCORD_OK) {
      ccord_stream_state updated = *cur;
      updated.user_id = uid;
      updated.paused = paused;
      (void)ccord_store_set_stream(s->store, &updated, err);
    }
    return st;
  }
  return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: not watching a stream");
}

const ccord_watched_stream *ccord_session_watched_stream(const ccord_session *s)
{
  return (s && s->watched.active) ? &s->watched : nullptr;
}


ccord_status ccord_session_set_voice_camera(ccord_session *s, bool enabled, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_voice_state *v = find_active_self_voice(s);
  if (!v) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: not connected to voice");
  }
  return ccord_transport_set_voice_camera(s->transport, v->guild_id, enabled, err);
}


ccord_status ccord_session_push_voice_pcm(ccord_session *s, const int16_t *pcm,
                                          size_t samples, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_mutex_lock(s->audio_mtx);
  if (!s->transport) {
    ccord_mutex_unlock(s->audio_mtx);
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: transport is null");
  }
  // Push-to-talk gate: while PTT mode is on and the key is not held, mic frames are dropped before
  // they reach the transport (the live voice gateway applies the same gate; no traffic either way).
  if (atomic_load(&s->has_voice_cfg) && s->voice_cfg.ptt && !atomic_load(&s->ptt_held)) {
    ccord_mutex_unlock(s->audio_mtx);
    return CCORD_OK;
  }
  ccord_status st = ccord_transport_push_voice_pcm(s->transport, pcm, samples, err);
  ccord_mutex_unlock(s->audio_mtx);
  return st;
}

size_t ccord_session_pull_voice_pcm(ccord_session *s, int16_t *pcm, size_t max_samples)
{
  if (!s || !pcm || max_samples == 0) return 0;
  ccord_mutex_lock(s->audio_mtx);
  if (!s->transport) {
    ccord_mutex_unlock(s->audio_mtx);
    return 0;
  }
  size_t n = ccord_transport_pull_voice_pcm(s->transport, pcm, max_samples);
  ccord_mutex_unlock(s->audio_mtx);
  return n;
}


// ---- M5 voice & video: DM calls, soundboard, local audio prefs, Go Live frames, stage/DAVE getters ----

static bool session_dm_is_bot(const ccord_store *store, const ccord_channel *ch)
{
  if (!store || !ch || ch->guild_id != CCORD_DM_GUILD_ID ||
      ch->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM) {
    return false;
  }
  ccord_id uid = ch->user_id;
  if (uid == CCORD_ID_NONE && ch->recipient_count > 0) uid = ch->recipient_ids[0];
  const ccord_user *u = uid != CCORD_ID_NONE ? ccord_store_user(store, uid) : nullptr;
  return u && u->is_bot;
}

ccord_status ccord_session_call_join(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid call channel");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch || ch->guild_id != CCORD_DM_GUILD_ID || ch->kind != CCORD_CHANNEL_TEXT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: calls only happen in DM channels");
  }
  if (session_dm_is_bot(s->store, ch)) {
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "session: bots cannot be called");
  }
  ccord_id self = ccord_store_self_id(s->store);
  const ccord_voice_state *v = ccord_store_voice_user(s->store, CCORD_DM_GUILD_ID, self);
  if (v && v->channel_id == channel_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: already in this call");
  }
  // There is one voice state per DM pseudo-guild: joining a second call would silently abandon the
  // first (its row would keep ringing with nobody to answer it). Leave the current call first.
  if (v && v->channel_id != CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: leave the current call first");
  }
  // Ringing is passive (CALL_CREATE); answering only sends OP 4. The store changes on the events.
  ccord_status st = ccord_transport_call_action(s->transport, channel_id, CCORD_CALL_JOIN, err);
  if (st == CCORD_OK) {
    s->pending_voice_guild = CCORD_DM_GUILD_ID;
    s->pending_voice_channel = channel_id;
  }
  return st;
}

ccord_status ccord_session_call_leave(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid call channel");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch || ch->guild_id != CCORD_DM_GUILD_ID || ch->kind != CCORD_CHANNEL_TEXT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: calls only happen in DM channels");
  }
  ccord_id self = ccord_store_self_id(s->store);
  const ccord_voice_state *v = ccord_store_voice_user(s->store, CCORD_DM_GUILD_ID, self);
  bool in_call = v && v->channel_id == channel_id;
  bool ringing = false;
  const ccord_call_state *call = ccord_store_call(s->store, channel_id);
  for (size_t i = 0; call && i < call->ringing_count; i++) {
    if (call->ringing[i] == self) {
      ringing = true;
      break;
    }
  }
  if (!in_call && !ringing) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: not in this call");
  // Decline while ringing, or hang up while connected; the server decides whether the call ends.
  ccord_status st = ccord_transport_call_action(s->transport, channel_id, CCORD_CALL_LEAVE, err);
  if (st == CCORD_OK) {
    ccord_free(s->cam_frame_buf);
    s->cam_frame_buf = nullptr;
  }
  return st;
}

ccord_status ccord_session_play_soundboard(ccord_session *s, ccord_id guild_id, ccord_id sound_id,
                                           ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (guild_id == CCORD_ID_NONE || sound_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid soundboard sound");
  }
  if (!ccord_store_guild(s->store, guild_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown guild");
  const ccord_voice_state *v = sess_self_voice(s, guild_id);
  if (!v || v->channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: join voice before playing sounds");
  }
  // Sound existence, availability and USE_SOUNDBOARD are the server's answer: the soundboard list is
  // lazy, so the store may not hold it. Playback itself is the governed media path, not a store write.
  return ccord_transport_play_soundboard(s->transport, guild_id, sound_id, err);
}

ccord_status ccord_session_set_user_volume(ccord_session *s, ccord_id user_id, uint8_t pct, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid user");
  if (pct > 200u) return ccord_error_set(err, CCORD_ERR_RANGE, "session: volume must be 0..200");
  ccord_status st = ccord_transport_set_user_volume(s->transport, user_id, pct, err);
  if (st == CCORD_OK) {
    // Local playback preference: it mirrors the transport's gain table and is never Discord state.
    ccord_error pref_err;
    (void)ccord_store_set_user_volume(s->store, user_id, pct, &pref_err);
  }
  return st;
}

ccord_status ccord_session_set_voice_config(ccord_session *s, const ccord_voice_config *cfg, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!cfg) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null voice config");
  if (cfg->vad_sensitivity_pct > 100u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "session: sensitivity must be 0..100");
  }
  if (s->stale) {   // no transport call before READY; the latest config is applied then
    s->stale_voice_cfg = *cfg;
    s->stale_has_voice_cfg = true;
    return CCORD_OK;
  }
  ccord_status st = ccord_transport_set_voice_config(s->transport, cfg, err);
  if (st == CCORD_OK) {
    // Remembered for the local push-to-talk gate on push_voice_pcm; no Discord traffic.
    ccord_mutex_lock(s->audio_mtx);
    s->voice_cfg = *cfg;
    atomic_store_explicit(&s->has_voice_cfg, true, memory_order_release);
    ccord_mutex_unlock(s->audio_mtx);
  }
  return st;
}

ccord_status ccord_session_set_ptt_held(ccord_session *s, bool held, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  s->ptt_held = held;   // per-frame key state from the app; gates the mic path, never sends anything
  return CCORD_OK;
}

bool ccord_session_ptt_held(const ccord_session *s) { return s && s->ptt_held; }

ccord_status ccord_session_stream_send_frame(ccord_session *s, const uint8_t *annexb, size_t len,
                                             uint32_t ts90k, bool keyframe, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty stream frame");
  if (!ccord_store_stream_active(s->store)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: start a stream before sending frames");
  }
  return ccord_transport_send_stream_frame(s->transport, annexb, len, ts90k, keyframe, err);
}

bool ccord_session_stream_poll_frame(ccord_session *s, const uint8_t **annexb, size_t *len, bool *keyframe)
{
  if (!s || !annexb || !len) return false;
  *annexb = nullptr;
  *len = 0;
  if (!s->watched.active) return false;
  if (!s->stream_frame_buf) {
    s->stream_frame_buf = ccord_alloc_array(SESSION_STREAM_FRAME_MAX, sizeof *s->stream_frame_buf);
    if (!s->stream_frame_buf) return false;
  }
  size_t out_len = 0;
  bool out_key = false;
  bool got = ccord_transport_poll_stream_frame(s->transport, s->stream_frame_buf, SESSION_STREAM_FRAME_MAX,
                                               &out_len, nullptr, &out_key);
  if (!got) return false;
  *annexb = s->stream_frame_buf;
  *len = out_len;
  if (keyframe) *keyframe = out_key;
  return true;
}

size_t ccord_session_camera_senders(ccord_session *s, ccord_camera_rx *out, size_t max)
{
  if (!s || !out || max == 0 || !find_active_self_voice(s)) return 0;
  return ccord_transport_camera_senders(s->transport, out, max);
}

bool ccord_session_camera_poll_frame(ccord_session *s, ccord_id user_id, const uint8_t **annexb, size_t *len,
                                     bool *keyframe)
{
  if (!s || !annexb || !len) return false;
  *annexb = nullptr;
  *len = 0;
  if (user_id == CCORD_ID_NONE || !find_active_self_voice(s)) return false;
  if (!s->cam_frame_buf) {
    s->cam_frame_buf = ccord_alloc_array(CCORD_CAM_FRAME_MAX, sizeof *s->cam_frame_buf);
    if (!s->cam_frame_buf) return false;
  }
  size_t out_len = 0;
  bool out_key = false;
  if (!ccord_transport_poll_camera_frame(s->transport, user_id, s->cam_frame_buf, CCORD_CAM_FRAME_MAX, &out_len,
                                         nullptr, &out_key)) {
    return false;
  }
  *annexb = s->cam_frame_buf;
  *len = out_len;
  if (keyframe) *keyframe = out_key;
  return true;
}

void ccord_session_camera_texture_key(ccord_id user_id, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  (void)snprintf(out, cap, "__cam__:%llu", (unsigned long long)user_id);
}

ccord_status ccord_session_voice_send_frame(ccord_session *s, const uint8_t *annexb, size_t len,
                                            uint32_t ts90k, bool keyframe, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty voice frame");
  const ccord_voice_state *v = find_active_self_voice(s);
  if (!v) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: not connected to voice");
  if (!v->self_video) return ccord_error_set(err, CCORD_ERR_INVALID, "session: voice camera not enabled");
  return ccord_transport_send_voice_video(s->transport, annexb, len, ts90k, keyframe, err);
}

bool ccord_session_consume_keyframe_request(ccord_session *s)
{
  if (!s) return false;
  return ccord_transport_consume_keyframe_request(s->transport);
}

ccord_status ccord_session_stream_send_pcm(ccord_session *s, const int16_t *pcm, size_t samples, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_mutex_lock(s->audio_mtx);
  if (!s->transport) {
    ccord_mutex_unlock(s->audio_mtx);
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: transport is null");
  }
  if (!pcm || samples == 0) {
    ccord_mutex_unlock(s->audio_mtx);
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty stream pcm");
  }
  if (!ccord_store_stream_active(s->store)) {
    ccord_mutex_unlock(s->audio_mtx);
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: start a stream before sending audio");
  }
  ccord_status st = ccord_transport_send_stream_pcm(s->transport, pcm, samples, err);
  ccord_mutex_unlock(s->audio_mtx);
  return st;
}

size_t ccord_session_stream_poll_pcm(ccord_session *s, int16_t *out_pcm, size_t cap_samples)
{
  if (!s || !out_pcm || cap_samples == 0) return 0;
  ccord_mutex_lock(s->audio_mtx);
  if (!s->transport || !atomic_load_explicit(&s->watched_active, memory_order_acquire)) {
    ccord_mutex_unlock(s->audio_mtx);
    return 0;
  }
  size_t n = ccord_transport_poll_stream_pcm(s->transport, out_pcm, cap_samples);
  ccord_mutex_unlock(s->audio_mtx);
  return n;
}

const ccord_call_state *ccord_session_incoming_call(const ccord_session *s)
{
  if (!s) return nullptr;
  ccord_id self = ccord_store_self_id(s->store);
  if (self == CCORD_ID_NONE) return nullptr;
  const ccord_voice_state *v = ccord_store_voice_user(s->store, CCORD_DM_GUILD_ID, self);
  for (size_t i = 0; i < ccord_store_call_count(s->store); i++) {
    const ccord_call_state *call = ccord_store_call_at(s->store, i);
    if (!call || !call->active) continue;
    if (v && v->channel_id == call->channel_id) continue;   // already answered
    for (size_t k = 0; k < call->ringing_count; k++) {
      if (call->ringing[k] == self) return call;
    }
  }
  return nullptr;
}

const ccord_stage_instance *ccord_session_stage_instance(const ccord_session *s, ccord_id channel_id)
{
  return s ? ccord_store_stage_instance(s->store, channel_id) : nullptr;
}

void ccord_session_voice_stats(const ccord_session *s, ccord_rtc_stats *out)
{
  if (!out) return;
  if (!s || ccord_store_active_voice_channel(s->store) == CCORD_ID_NONE) {
    *out = (ccord_rtc_stats){0};
    return;
  }
  ccord_store_rtc_stats(s->store, out);
}

void ccord_session_dave_status(const ccord_session *s, ccord_dave_status *out)
{
  if (!out) return;
  if (!s || ccord_store_active_voice_channel(s->store) == CCORD_ID_NONE) {
    *out = (ccord_dave_status){0};
    return;
  }
  ccord_store_dave_status(s->store, out);
}


// ---- stage lifecycle & guild scheduled events ----

ccord_status ccord_session_stage_start(ccord_session *s, ccord_id guild_id, ccord_id channel_id, const char *topic,
                                       ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_stage_create(&req, guild_id, channel_id, topic, 2u, nullptr, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);
}

ccord_status ccord_session_stage_end(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_stage_delete(&req, channel_id, nullptr, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);
}


