// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>




int64_t session_now_unix_ms(const ccord_session *s)
{
  (void)s;
  return ccord_clock_unix_ms();
}

ccord_status sess_check_thread_lock(const ccord_session *s, const ccord_channel *ch, ccord_error *err)
{
  if (ch && ccord_discord_type_is_thread(ch->discord_type) && ch->thread_locked) {
    ccord_id self = ccord_store_self_id(s->store);
    if (!ccord_store_permissions_exact(s->store, ch->guild_id, self) ||
        (ccord_store_permissions(s->store, ch->guild_id, ch->id, self, session_now_unix_ms(s)) & CCORD_PERM_MANAGE_THREADS) == 0) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION, "thread is locked");
    }
  }
  return CCORD_OK;
}

// A new transport or a new READY belongs to another connection (and possibly another account): drop the
// account lists and their fetch state. The store drops its cached settings on SNAPSHOT itself.

ccord_session *ccord_session_create(ccord_bus *bus, ccord_store *store, ccord_transport *transport, ccord_error *err)
{
  ccord_error_clear(err);
  if (!store || !transport) {
    ccord_transport_destroy(transport);
    ccord_error_set(err, CCORD_ERR_INVALID, "session: store and transport are required");
    return nullptr;
  }
  ccord_session *s = ccord_calloc(1, sizeof *s);
  if (!s) {
    ccord_transport_destroy(transport);
    ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
    return nullptr;
  }
  s->mc = ccord_calloc(1, sizeof *s->mc);
  if (!s->mc) {
    ccord_free(s);
    ccord_transport_destroy(transport);
    ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
    return nullptr;
  }
  s->streams = ccord_stream_hub_create(transport);
  if (!s->streams) {
    ccord_free(s->mc);
    ccord_free(s);
    ccord_transport_destroy(transport);
    ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
    return nullptr;
  }
  s->bus = bus;
  s->store = store;
  s->transport = transport;
  s->audio_mtx = ccord_mutex_create();
  s->state = CCORD_CONN_DISCONNECTED;
  s->camera_rx_enabled = true;
  s->voice_track_synced = (ccord_store_guild_count(store) == 0u &&
                           ccord_store_channel_count(store, CCORD_DM_GUILD_ID) == 0u);
  return s;
}


void ccord_session_destroy(ccord_session *s)
{
  if (!s) return;
  ccord_stream_hub_destroy(s->streams);   // before the transport: closes the streams that read through it
  ccord_mutex_lock(s->audio_mtx);
  ccord_transport_destroy(s->transport);
  s->transport = nullptr;
  ccord_mutex_unlock(s->audio_mtx);
  ccord_mutex_destroy(s->audio_mtx);
  ccord_profile_rest_destroy(s->profile_rest);   // pending edits answer CCORD_ERR_IO without touching the store
  ccord_account_rest_destroy(s->account_rest);
  sess_account_wipe_vault(s);
  for (size_t i = 0; i < s->nfav_pending; i++) ccord_free(s->fav_pending[i].url);
  ccord_free(s->requested);
  ccord_free(s->chan_hist);
  ccord_free(s->profile_cache);
  ccord_free(s->modals);
  ccord_free(s->slash_cache);
  ccord_free(s->activity_shelves);
  ccord_free(s->guild_profiles);
  ccord_free(s->voice_track);
  ccord_free(s->invite_cache);
  ccord_free(s->discovery_items);
  ccord_free(s->discovery_cache);
  ccord_free(s->stream_frame_buf);
  ccord_free(s->cam_frame_buf);
  ccord_free(s->rich_json);
  ccord_free(s->stale_rich);
  sess_clear_media_cache(s);
  ccord_free(s->mc);
  for (size_t i = 0; i < s->fail_count; i++) {
    ccord_free(s->failures[(s->fail_head + i) % SESSION_MAX_FAILURES].draft);
  }
  for (size_t i = 0; i < s->report_count; i++) {
    ccord_report_event_free(&s->report_events[(s->report_head + i) % SESSION_MAX_REPORT_EVENTS]);
  }
  sess_clear_search_results(s);
  sess_clear_reaction_users(s);
  sess_reset_verification_cache(s);
  ccord_free(s->verification_cache);
  ccord_free(s->video_resolved);
  ccord_free(s->collectibles);
  ccord_free(s);
}

ccord_status ccord_session_start(ccord_session *s, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  return ccord_transport_connect(s->transport, err);
}

void ccord_session_set_stale(ccord_session *s, bool stale)
{
  if (s) s->stale = stale;
}

ccord_status ccord_session_link_event(ccord_session *s, ccord_link_event kind, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  return ccord_transport_network_event(s->transport, kind, err);
}

static void sess_sync_selected_channel(ccord_session *s)
{
  if (!s || !session_is_authenticated(s)) return;
  ccord_id sel_ch = ccord_store_selected_channel(s->store);
  sess_request_history_once(s, sel_ch);
  sess_maybe_request_dm_profile(s, sel_ch);
  if (s->reading_known && s->reading_channel != CCORD_ID_NONE) {
    ccord_id self_id = ccord_store_self_id(s->store);
    if (self_id != CCORD_ID_NONE && !ccord_store_user_profile(s->store, self_id)) {
      ccord_error ignored;
      (void)ccord_session_request_profile(s, self_id, &ignored);
    }
  }
}

void ccord_session_set_background(ccord_session *s, bool background)
{
  if (!s) return;
  bool was_bg = s->in_background;
  s->in_background = background;
  ccord_transport_set_background(s->transport, background);
  if (was_bg && !background && s->bg_deferred_sync) {
    s->bg_deferred_sync = false;
    sess_sync_selected_channel(s);
  }
}

void ccord_session_set_suspended(ccord_session *s, bool suspended)
{
  if (!s) return;
  s->suspended = suspended;
  ccord_transport_set_suspended(s->transport, suspended);
}

void ccord_session_set_camera_rx(ccord_session *s, bool enabled)
{
  if (!s) return;
  s->camera_rx_enabled = enabled;
  ccord_transport_set_camera_rx(s->transport, enabled);
}

bool ccord_session_is_stale(const ccord_session *s)
{
  return s && s->stale;
}

void ccord_session_trim_memory(ccord_session *s)
{
  if (!s) return;
  sess_clear_search_results(s);
  sess_clear_reaction_users(s);
  sess_reset_invite_cache(s);
  ccord_free(s->invite_cache);
  s->invite_cache = nullptr;
  ccord_free(s->discovery_cache);
  s->discovery_cache = nullptr;
  s->discovery_cache_rr = 0;
  ccord_free(s->video_resolved);
  s->video_resolved = nullptr;
  s->video_resolved_head = 0;
  s->video_resolved_count = 0;
  sess_trim_media_cache_bytes(s);
}

ccord_status ccord_session_set_transport(ccord_session *s, ccord_transport *transport, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !transport) {
    ccord_transport_destroy(transport);
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session or transport");
  }
  ccord_mutex_lock(s->audio_mtx);
  ccord_transport_destroy(s->transport);
  s->transport = transport;
  ccord_mutex_unlock(s->audio_mtx);
  ccord_stream_hub_set_transport(s->streams, transport);
  ccord_transport_set_background(transport, s->in_background);
  ccord_transport_set_camera_rx(transport, s->camera_rx_enabled);
  s->bg_deferred_sync = false;
  s->rich_op3_retry_ms = 0;
  s->nrequested = 0;
  s->nchan_hist = 0;
  s->nprofile_req = 0;
  s->nslash_cache = 0;
  s->nactivity_shelves = 0;
  s->nguild_profiles = 0;
  s->slash_choice_count = 0;
  s->npins_in_flight = 0;
  s->notif_head = 0;
  sess_account_wipe_vault(s);
  ccord_free(s->collectibles);
  s->collectibles = nullptr;
  s->collectible_count = 0;
  s->collectibles_state = CCORD_FETCH_IDLE;
  s->collectibles_fetched_ms = 0;
  s->collectibles_gen++;
  s->collectibles_error[0] = '\0';
  s->notif_count = 0;
  s->video_resolved_head = 0;
  s->video_resolved_count = 0;
  sess_clear_search_results(s);
  sess_clear_reaction_users(s);
  s->has_invite_preview = false;
  s->invite_preview_loading = false;
  sess_reset_invite_cache(s);
  sess_reset_verification_cache(s);
  s->pending_join_guild_id = CCORD_ID_NONE;
  s->pending_join_channel_id = CCORD_ID_NONE;
  s->discovery_count = 0;
  s->discovery_total = 0;
  s->discovery_loading = false;
  if (s->discovery_cache) {
    memset(s->discovery_cache, 0, SESSION_DISCOVERY_CACHE_SLOTS * sizeof *s->discovery_cache);
  }
  s->discovery_cache_rr = 0;
  ccord_mutex_lock(s->audio_mtx);
  atomic_store_explicit(&s->has_voice_cfg, false, memory_order_release);
  ccord_mutex_unlock(s->audio_mtx);
  s->watched.active = false;
  atomic_store_explicit(&s->watched_active, false, memory_order_release);
  s->watched.paused = false;
  ccord_free(s->stream_frame_buf);
  s->stream_frame_buf = nullptr;
  ccord_free(s->cam_frame_buf);
  s->cam_frame_buf = nullptr;
  s->ptt_held = false;
  sess_reset_account_lists(s);
  sess_clear_media_cache(s);
  s->state = CCORD_CONN_DISCONNECTED;
  return ccord_transport_connect(s->transport, err);
}

ccord_connection_state ccord_session_state(const ccord_session *s)
{
  return s ? s->state : CCORD_CONN_DISCONNECTED;
}

static bool was_requested(const ccord_session *s, ccord_id id)
{
  for (size_t i = 0; i < s->nrequested; i++) {
    if (s->requested[i] == id) return true;
  }
  return false;
}

void sess_clear_requested(ccord_session *s, ccord_id id)
{
  if (!s || id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < s->nrequested; ) {
    if (s->requested[i] == id) {
      memmove(&s->requested[i], &s->requested[i + 1], (s->nrequested - i - 1) * sizeof *s->requested);
      s->nrequested--;
    } else {
      i++;
    }
  }
}

session_channel_hist *sess_find_chan_hist(const ccord_session *s, ccord_id channel_id)
{
  if (!s || channel_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nchan_hist; i++) {
    if (s->chan_hist[i].channel_id == channel_id) return &s->chan_hist[i];
  }
  return nullptr;
}

session_channel_hist *sess_chan_hist_for(ccord_session *s, ccord_id channel_id)
{
  session_channel_hist *e = sess_find_chan_hist(s, channel_id);
  if (e) return e;
  if (s->nchan_hist == s->capchan_hist) {
    size_t ncap = s->capchan_hist ? s->capchan_hist * 2 : 32;
    session_channel_hist *g = ccord_realloc_array(s->chan_hist, ncap, sizeof *g);
    if (!g) return nullptr;
    s->chan_hist = g;
    s->capchan_hist = ncap;
  }
  e = &s->chan_hist[s->nchan_hist++];
  *e = (session_channel_hist){
    .channel_id = channel_id,
    .in_flight_before_id = CCORD_ID_NONE,
    .has_more = true,
  };
  return e;
}

session_profile_cache *sess_find_profile_cache(const ccord_session *s, ccord_id user_id)
{
  if (!s || user_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nprofile_req; i++) {
    if (s->profile_cache[i].user_id == user_id) return &s->profile_cache[i];
  }
  return nullptr;
}

session_profile_cache *sess_profile_cache_for(ccord_session *s, ccord_id user_id)
{
  session_profile_cache *pe = sess_find_profile_cache(s, user_id);
  if (pe) {
    pe->last_use = ++s->cache_tick;
    return pe;
  }
  if (!s || user_id == CCORD_ID_NONE) return nullptr;
  if (s->nprofile_req >= SESSION_PROFILE_CACHE_MAX) {
    size_t victim = SIZE_MAX;
    for (size_t i = 0; i < s->nprofile_req; i++) {
      if (s->profile_cache[i].in_flight) continue;
      if (victim == SIZE_MAX || s->profile_cache[i].last_use < s->profile_cache[victim].last_use) victim = i;
    }
    if (victim == SIZE_MAX) {
      victim = 0;
      for (size_t i = 1; i < s->nprofile_req; i++) {
        if (s->profile_cache[i].last_use < s->profile_cache[victim].last_use) victim = i;
      }
    }
    pe = &s->profile_cache[victim];
  } else {
    if (s->nprofile_req == s->capprofile_req) {
      size_t ncap = s->capprofile_req ? s->capprofile_req * 2u : 16u;
      if (ncap > SESSION_PROFILE_CACHE_MAX) ncap = SESSION_PROFILE_CACHE_MAX;
      session_profile_cache *g = ccord_realloc_array(s->profile_cache, ncap, sizeof *g);
      if (!g) return nullptr;
      s->profile_cache = g;
      s->capprofile_req = ncap;
    }
    pe = &s->profile_cache[s->nprofile_req++];
  }
  memset(pe, 0, sizeof *pe);
  pe->user_id = user_id;
  pe->last_use = ++s->cache_tick;
  return pe;
}


bool session_is_authenticated(const ccord_session *s)
{
  return s && !s->stale && (s->state == CCORD_CONN_CONNECTED || ccord_store_self_id(s->store) != CCORD_ID_NONE);
}


void sess_maybe_request_dm_profile(ccord_session *s, ccord_id channel_id)
{
  if (!s || channel_id == CCORD_ID_NONE || !session_is_authenticated(s)) return;
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (ch && ch->guild_id == CCORD_DM_GUILD_ID && ch->user_id != CCORD_ID_NONE) {
    ccord_error ignored;
    (void)ccord_session_request_profile(s, ch->user_id, &ignored);
  }
}

// Asks for the channel history once, if the store does not have it yet.
void sess_request_history_once(ccord_session *s, ccord_id channel_id)
{
  if (channel_id == CCORD_ID_NONE || !session_is_authenticated(s)) return;
  if (ccord_store_channel_is_forum(s->store, channel_id)) {
    const ccord_forum_state *fst = ccord_store_forum_state(s->store, channel_id);
    if (!fst || (!fst->loaded && !fst->loading)) {
      const ccord_forum_meta *fmeta = ccord_store_forum_meta(s->store, channel_id);
      ccord_forum_query q = {
        .forum_id = channel_id,
        .offset = 0,
        .limit = CCORD_FORUM_PAGE_SIZE,
        .sort = fmeta ? (ccord_forum_sort)fmeta->default_sort : CCORD_FORUM_SORT_ACTIVITY,
      };
      ccord_error ferr;
      (void)ccord_session_request_forum_posts(s, &q, &ferr);
    }
    return;
  }
  if ((ccord_store_history_loaded(s->store, channel_id) && !ccord_store_history_stale(s->store, channel_id)) ||
      was_requested(s, channel_id)) {
    return;
  }
  if (s->nrequested == s->caprequested) {
    size_t ncap = s->caprequested ? s->caprequested * 2 : 32;
    ccord_id *n = ccord_realloc_array(s->requested, ncap, sizeof *n);
    if (!n) {
      CCORD_LOG_W("session: out of memory while tracking history requests");
      return;
    }
    s->requested = n;
    s->caprequested = ncap;
  }
  ccord_error err;
  ccord_status st = ccord_transport_request_history(s->transport, channel_id, CCORD_SESSION_HISTORY_LIMIT, &err);
  if (st != CCORD_OK) {
    CCORD_LOG_W("session: history request failed: %s", err.message);
    return;
  }
  s->requested[s->nrequested++] = channel_id;
}

void sess_post_state(ccord_session *s)
{
  if (!s->bus) return;
  ccord_event ev = {.type = CCORD_EVT_CONNECTION_STATE, .value = (int64_t)s->state};
  if (ccord_bus_post(s->bus, ev) != CCORD_OK) CCORD_LOG_W("session: connection.state event dropped");
}

// Whether the message pings one of the roles the user holds. Unknown roles (the member list is lazy) answer false: a
// ping is only claimed when it is certain.
bool sess_message_pings_own_role(const ccord_session *s, ccord_id guild_id, const ccord_message_rich *rich)
{
  if (!rich || rich->mention_role_count == 0 || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return false;
  const ccord_id *roles = nullptr;
  size_t n = 0;
  if (!ccord_store_member_roles(s->store, guild_id, ccord_store_self_id(s->store), &roles, &n)) return false;
  for (size_t i = 0; i < n; i++) {
    if (ccord_rich_mentions_role(rich, roles[i])) return true;
  }
  return false;
}

void sess_queue_notification(ccord_session *s, ccord_notification n)
{
  if (s->notif_count == SESSION_MAX_NOTIFICATIONS) {
    s->notif_head = (s->notif_head + 1u) % SESSION_MAX_NOTIFICATIONS;
    s->notif_count--;
  }
  size_t idx = (s->notif_head + s->notif_count) % SESSION_MAX_NOTIFICATIONS;
  s->notifications[idx] = n;
  s->notif_count++;
}


void sess_queue_report_event(ccord_session *s, const ccord_net_event *ev)
{
  if (s->report_count == SESSION_MAX_REPORT_EVENTS) return;   // the UI drains every frame; never grow unbounded
  ccord_report_event e = {.kind = ev->type == CCORD_NET_REPORT_MENU ? CCORD_REPORT_EV_MENU : CCORD_REPORT_EV_SUBMITTED};
  if (ev->expr_arg && strcmp(ev->expr_arg, ccord_report_type_name(CCORD_REPORT_USER)) == 0) e.type = CCORD_REPORT_USER;
  else e.type = CCORD_REPORT_MESSAGE;
  if (e.kind == CCORD_REPORT_EV_MENU) {
    if (!ev->expr_body) return;
    e.menu_json = ccord_strndup(ev->expr_body, ev->expr_body_len);
    if (!e.menu_json) return;
    e.menu_len = ev->expr_body_len;
  } else {
    e.report_id = ev->report_id;
  }
  s->report_events[(s->report_head + s->report_count) % SESSION_MAX_REPORT_EVENTS] = e;
  s->report_count++;
}

bool ccord_session_pop_report_event(ccord_session *s, ccord_report_event *out)
{
  if (!s || !out || s->report_count == 0) return false;
  *out = s->report_events[s->report_head];
  s->report_events[s->report_head] = (ccord_report_event){0};   // ownership of menu_json moves to the caller
  s->report_head = (s->report_head + 1u) % SESSION_MAX_REPORT_EVENTS;
  s->report_count--;
  return true;
}

void ccord_report_event_free(ccord_report_event *e)
{
  if (!e) return;
  ccord_free(e->menu_json);
  e->menu_json = nullptr;
  e->menu_len = 0;
}

void sess_record_failure(ccord_session *s, const ccord_net_event *ev)
{
  CCORD_LOG_W("session: action %d failed: %s", (int)ev->action, ev->text ? ev->text : "");
  if (s->fail_count == SESSION_MAX_FAILURES) {  // unreachable while the UI drains every frame; never grow unbounded
    ccord_free(s->failures[s->fail_head].draft);
    s->fail_head = (s->fail_head + 1u) % SESSION_MAX_FAILURES;
    s->fail_count--;
  }
  ccord_action_failure *f = &s->failures[(s->fail_head + s->fail_count) % SESSION_MAX_FAILURES];
  memset(f, 0, sizeof *f);
  f->action = ev->action;
  f->channel_id = ev->channel_id;
  f->message_id = ev->message_id;
  f->guild_id = ev->guild_id;
  f->user_id = ev->user_id;
  f->expr_id = ev->expr_id;
  f->admin_op = ev->admin_op;
  f->status = ev->status;
  const char *reason = ev->text && ev->text[0] ? ev->text : ccord_status_name(ev->status);
  size_t rlen = strlen(reason);
  if (rlen >= sizeof f->reason) rlen = sizeof f->reason - 1u;
  memcpy(f->reason, reason, rlen);
  f->reason[rlen] = '\0';
  if (ev->draft && ev->draft_len > 0) {
    f->draft = ccord_strndup(ev->draft, ev->draft_len);
    if (f->draft) f->draft_len = ev->draft_len;
  }
  s->fail_count++;
}

bool ccord_session_pop_failure(ccord_session *s, ccord_action_failure *out)
{
  if (!s || !out || s->fail_count == 0) return false;
  *out = s->failures[s->fail_head];
  s->failures[s->fail_head].draft = nullptr;  // ownership moves to the caller
  s->fail_head = (s->fail_head + 1u) % SESSION_MAX_FAILURES;
  s->fail_count--;
  return true;
}

void ccord_action_failure_free(ccord_action_failure *f)
{
  if (!f) return;
  ccord_free(f->draft);
  f->draft = nullptr;
  f->draft_len = 0;
}

size_t ccord_session_pump(ccord_session *s)
{
  if (!s) return 0;
  size_t n = 0;
  ccord_net_event ev;
  while (n < PUMP_BATCH && ccord_transport_poll(s->transport, &ev)) {
    sess_apply_event(s, &ev);
    n++;
  }
  uint64_t now_ms = ccord_clock_ms();
  ccord_stream_hub_pump(s->streams, now_ms);
  ccord_profile_rest_pump(s->profile_rest, now_ms);
  ccord_account_rest_pump(s->account_rest, now_ms);
  if (s->rich_op3_dirty && s->state == CCORD_CONN_CONNECTED && !s->stale &&
      ccord_store_self_id(s->store) != CCORD_ID_NONE && now_ms >= s->rich_op3_retry_ms) {
    ccord_error gw_err;
    if (session_send_presence_with_custom(s, session_current_presence(s), &gw_err) == CCORD_OK) {
      s->rich_op3_dirty = false;
      s->rich_op3_retry_ms = 0;
    } else {
      s->rich_op3_retry_ms = now_ms + 1000u;
    }
  }
  return n;
}

bool ccord_session_wait_info(const ccord_session *s, ccord_wait_info *out)
{
  if (!out) return false;
  *out = (ccord_wait_info){.timeout_ms = UINT32_MAX};
  if (!s || !s->transport) return false;
  bool supported = ccord_transport_wait_info(s->transport, out);
  if (s->notif_count > 0) {
    out->immediate = true;
    out->timeout_ms = 0;
    return supported;
  }
  uint64_t now_ms = ccord_clock_ms();
  if (s->rich_op3_dirty && s->state == CCORD_CONN_CONNECTED && !s->stale &&
      ccord_store_self_id(s->store) != CCORD_ID_NONE) {
    if (now_ms >= s->rich_op3_retry_ms) {
      out->immediate = true;
      out->timeout_ms = 0;
      return supported;
    }
    uint64_t rem = s->rich_op3_retry_ms - now_ms;
    uint32_t w = rem > UINT32_MAX ? UINT32_MAX : (uint32_t)rem;
    if (w < out->timeout_ms) out->timeout_ms = w;
  }
  if ((s->profile_rest && ccord_profile_rest_pending(s->profile_rest)) ||
      (s->account_rest && ccord_account_rest_pending(s->account_rest))) {
    if (out->timeout_ms > 50u) out->timeout_ms = 50u;
  }
  if (s->streams) {
    uint32_t sm_wait = ccord_stream_hub_next_wait_ms(s->streams, now_ms);
    if (sm_wait == 0) {
      out->immediate = true;
      out->timeout_ms = 0;
    } else if (sm_wait < out->timeout_ms) {
      out->timeout_ms = sm_wait;
    }
  }
  return supported;
}

ccord_status ccord_session_select_guild(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_status st = ccord_store_select_guild(s->store, guild_id, err);
  if (st == CCORD_OK) {
    ccord_id sel_ch = ccord_store_selected_channel(s->store);
    ccord_error hook_err;
    ccord_status hst = s->stale ? CCORD_OK : ccord_transport_select_channel(s->transport, guild_id, sel_ch, &hook_err);
    if (hst != CCORD_OK && hst != CCORD_ERR_UNSUPPORTED) {
      CCORD_LOG_W("session: guild select hook failed: %s", hook_err.message);
    }
    s->retried_channel = CCORD_ID_NONE;
    sess_request_history_once(s, sel_ch);
    sess_maybe_request_dm_profile(s, sel_ch);
  }
  return st;
}

// Text, voice and stage channels all carry a message timeline (a voice/stage channel's own "Text Chat"); categories do not.
bool session_channel_has_chat(const ccord_channel *ch)
{
  return ch && (ch->kind == CCORD_CHANNEL_TEXT || ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE);
}

ccord_status ccord_session_select_channel(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_id prev_guild = ccord_store_selected_guild(s->store);
  ccord_id prev_ch = ccord_store_selected_channel(s->store);
  ccord_status st = ccord_store_select_channel(s->store, channel_id, err);
  if (st == CCORD_OK) {
    ccord_id new_guild = ccord_store_selected_guild(s->store);
    if (new_guild != CCORD_ID_NONE && !s->stale) {   // stale: READY re-subscribes the selected channel
      if (new_guild != prev_guild) {
        ccord_error hook_err;
        ccord_status hst = ccord_transport_select_channel(s->transport, new_guild, channel_id, &hook_err);
        if (hst != CCORD_OK && hst != CCORD_ERR_UNSUPPORTED) {
          CCORD_LOG_W("session: guild select hook failed: %s", hook_err.message);
        }
      } else if (channel_id != prev_ch && new_guild != CCORD_DM_GUILD_ID &&
                 CCORD_TRANSPORT_OPTIONAL(s->transport, select_channel)) {
        ccord_error hook_err;
        ccord_status hst = s->transport->vt->select_channel(s->transport, new_guild, channel_id, &hook_err);
        if (hst != CCORD_OK && hst != CCORD_ERR_UNSUPPORTED) {
          CCORD_LOG_W("session: channel select hook failed: %s", hook_err.message);
        }
      }
    }
    s->retried_channel = CCORD_ID_NONE;
    sess_request_history_once(s, channel_id);
    sess_maybe_request_dm_profile(s, channel_id);
  }
  return st;
}


bool ccord_session_more_members(ccord_session *s, uint64_t now_ms)
{
  if (!s || s->stale) return false;
  ccord_id guild = ccord_store_selected_guild(s->store), channel = ccord_store_selected_channel(s->store);
  if (guild == CCORD_ID_NONE || guild == CCORD_DM_GUILD_ID || channel == CCORD_ID_NONE) return false;
  if (s->paging_guild != guild || s->paging_channel != channel) {   // another list: it starts at the top again
    s->paging_guild = guild;
    s->paging_channel = channel;
    s->paging_chunks = 1;
    s->paging_wait = s->paging_done = false;
  }
  if (s->paging_done || s->paging_chunks >= CCORD_MEMBER_CHUNKS_MAX) return false;
  // Only once the server has said how big the list is (its first update does): before that there is no telling whether
  // there is more, and a request would only be a guess. And not when everything it holds is here already.
  uint32_t total = 0;
  size_t held = ccord_store_member_count(s->store, guild);
  if (!ccord_store_member_list_size(s->store, guild, &total, nullptr) || held >= total) return false;
  // One piece at a time, and not faster than SESSION_MEMBER_PAGE_GAP_MS: a piece is awaited (a silent server is given
  // SESSION_MEMBER_PAGE_WAIT_MS before asking again), so scrolling a long list cannot become a stream of requests.
  if (s->paging_sent_ms != 0 && now_ms < s->paging_sent_ms + SESSION_MEMBER_PAGE_GAP_MS) return false;
  if (s->paging_wait && now_ms < s->paging_sent_ms + SESSION_MEMBER_PAGE_WAIT_MS) return false;
  ccord_error err;
  s->paging_sent_ms = now_ms ? now_ms : 1u;   // a failed request is not retried at once either
  if (ccord_transport_request_members(s->transport, guild, channel, s->paging_chunks + 1u, &err) != CCORD_OK) return false;
  s->paging_chunks++;
  s->paging_wait = true;
  s->paging_held = held;
  return true;
}

void ccord_session_set_reading(ccord_session *s, ccord_id channel_id)
{
  if (!s) return;
  s->reading_channel = channel_id;
  s->reading_known = true;
  if (channel_id != CCORD_ID_NONE && s->state == CCORD_CONN_CONNECTED && !s->stale) {
    ccord_id self_id = ccord_store_self_id(s->store);
    if (self_id != CCORD_ID_NONE && !ccord_store_user_profile(s->store, self_id)) {
      ccord_error ignored;
      (void)ccord_session_request_profile(s, self_id, &ignored);
    }
  }
}

ccord_status ccord_session_mark_read(ccord_session *s, ccord_id guild_id, ccord_id channel_id, size_t *acked, ccord_error *err)
{
  if (acked) *acked = 0;
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (guild_id != CCORD_DM_GUILD_ID && !ccord_store_guild(s->store, guild_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That server isn't available.");
  }
  if (channel_id != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
    if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
    if (ch->guild_id != guild_id) return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't in this server.");
  }
  ccord_read_target targets[CCORD_MARK_READ_MAX];
  size_t n = ccord_store_read_targets(s->store, guild_id, channel_id, ccord_clock_unix_ms(), targets, CCORD_MARK_READ_MAX);
  if (n > CCORD_MARK_READ_MAX) n = CCORD_MARK_READ_MAX;
  size_t acked_count = 0;
  for (size_t i = 0; i < n; i++) {
    if (session_is_local_echo_id(targets[i].message_id)) continue;   // no server message to acknowledge yet
    ccord_status st = ccord_session_ack_message(s, targets[i].channel_id, targets[i].message_id, err);
    if (st != CCORD_OK) {
      if (acked_count == 0) return st;
      break;   // some went out: report those, the rest are still unread
    }
    acked_count++;
  }
  if (acked_count > 0) ccord_error_clear(err);
  if (acked) *acked = acked_count;
  return CCORD_OK;
}


void session_replay_stale(ccord_session *s)
{
  ccord_error err;
  if (s->stale_has_rich) {
    char *json = s->stale_rich;
    s->stale_rich = nullptr;
    s->stale_has_rich = false;
    if (ccord_session_set_rich_presence(s, json, &err) != CCORD_OK) {
      CCORD_LOG_W("session: deferred rich presence rejected: %s", err.message);
    }
    ccord_free(json);
  }
  if (s->stale_has_voice_cfg) {
    s->stale_has_voice_cfg = false;
    if (ccord_session_set_voice_config(s, &s->stale_voice_cfg, &err) != CCORD_OK) {
      CCORD_LOG_W("session: deferred voice config rejected: %s", err.message);
    }
  }
  size_t n = s->nstale_expr;
  s->nstale_expr = 0;
  for (size_t i = 0; i < n; i++) {
    (void)ccord_session_request_expression_data(s, s->stale_expr[i].kind, s->stale_expr[i].arg, false, &err);
  }
  if (s->stale_slash_channel != CCORD_ID_NONE) {
    ccord_id g = s->stale_slash_guild, c = s->stale_slash_channel;
    s->stale_slash_guild = s->stale_slash_channel = CCORD_ID_NONE;
    uint64_t now = ccord_clock_ms();
    (void)ccord_session_ensure_slash_commands(s, g, c, now ? now : 1u);
  }
}


bool ccord_session_pop_notification(ccord_session *s, ccord_notification *out)
{
  if (!s || !out || s->notif_count == 0) return false;
  *out = s->notifications[s->notif_head];
  s->notif_head = (s->notif_head + 1u) % SESSION_MAX_NOTIFICATIONS;
  s->notif_count--;
  return true;
}


void ccord_session_set_raw_gateway_cb(ccord_session *s, ccord_session_raw_gateway_cb cb, void *ud)
{
  if (!s) return;
  s->raw_gw_cb = cb;
  s->raw_gw_ud = ud;
  ccord_gateway_set_raw_tap(cb != nullptr);
}

