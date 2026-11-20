// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void session_set_self_activity(ccord_session *s, bool has, ccord_activity_type type, const char *name,
                                      const char *details, const char *state, int64_t start_ms, int64_t end_ms)
{
  ccord_id self = ccord_store_self_id(s->store);
  const ccord_user *me = self != CCORD_ID_NONE ? ccord_store_user(s->store, self) : nullptr;
  if (!me) return;
  if (!name) name = "";
  if (!details) details = "";
  if (!state) state = "";
  has = has && name[0] != '\0';
  if (!has && !me->has_activity && (!me->status_text || me->status_text[0] == '\0')) return;
  if (has && me->has_activity && me->activity_type == type && strcmp(me->activity_name, name) == 0 &&
      strcmp(me->activity_details, details) == 0 && strcmp(me->activity_state, state) == 0 &&
      me->activity_start_ms == start_ms && me->activity_end_ms == end_ms) {
    return;
  }
  char status_buf[CCORD_ACTIVITY_TEXT_MAX_BYTES + 32];
  status_buf[0] = '\0';
  if (has) {
    const char *prefix = "";
    switch (type) {
      case CCORD_ACTIVITY_PLAYING: prefix = "Playing "; break;
      case CCORD_ACTIVITY_STREAMING: prefix = "Streaming "; break;
      case CCORD_ACTIVITY_LISTENING: prefix = "Listening to "; break;
      case CCORD_ACTIVITY_WATCHING: prefix = "Watching "; break;
      case CCORD_ACTIVITY_COMPETING: prefix = "Competing in "; break;
      default: break;
    }
    (void)snprintf(status_buf, sizeof status_buf, "%s%s", prefix, name);
  }
  char emoji[sizeof me->custom_status_emoji], text[sizeof me->custom_status_text];
  memcpy(emoji, me->custom_status_emoji, sizeof emoji);
  memcpy(text, me->custom_status_text, sizeof text);
  ccord_error err;
  if (ccord_store_update_presence_detail(s->store, self, me->presence, status_buf, emoji, text,
                                         me->custom_status_expires_ms, has, type, name, details, state, start_ms,
                                         end_ms, &err) != CCORD_OK) {
    CCORD_LOG_W("session: own activity not applied: %s", err.message);
  }
}


static bool ascii_strcasecmp_eq(const char *a, const char *b)
{
  if (!a || !b) return false;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a++;
    unsigned char cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
    if (ca != cb) return false;
  }
  return *a == '\0' && *b == '\0';
}

// True when the Rich Presence activities array `json` has an activity called `name` (case-insensitive).
static bool rich_json_has_name(const char *json, const char *name)
{
  if (!json || json[0] == '\0' || !name || name[0] == '\0') return false;
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  bool found = false;
  const ccord_json *arr = ccord_json_parse(&tmp, json, strlen(json), nullptr, nullptr);
  for (size_t i = 0; arr && i < ccord_json_array_len(arr) && !found; i++) {
    const ccord_json *nj = ccord_json_get(ccord_json_array_at(arr, i), "name");
    const char *n = nj ? ccord_json_string(nj, nullptr) : nullptr;
    found = n && ascii_strcasecmp_eq(n, name);
  }
  ccord_arena_free(&tmp);
  return found;
}

static bool rich_json_has_any(const char *json)
{
  if (!json || json[0] == '\0') return false;
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  const ccord_json *arr = ccord_json_parse(&tmp, json, strlen(json), nullptr, nullptr);
  bool any = arr && ccord_json_array_len(arr) > 0;
  ccord_arena_free(&tmp);
  return any;
}



static void split_stored_emoji(const char *stored, char name_out[CCORD_EMOJI_MAX_BYTES + 1], ccord_id *id_out)
{
  name_out[0] = '\0';
  *id_out = CCORD_ID_NONE;
  if (!stored || stored[0] == '\0') return;
  const char *colon = strrchr(stored, ':');
  if (colon && colon > stored && colon[1] != '\0') {
    uint64_t eid = 0;
    bool digits = true;
    for (const char *p = colon + 1; *p; p++) {
      if (*p < '0' || *p > '9') { digits = false; break; }
      eid = eid * 10u + (uint64_t)(*p - '0');
    }
    if (digits && eid > 0) {
      size_t nlen = (size_t)(colon - stored);
      if (nlen > CCORD_EMOJI_MAX_BYTES) nlen = CCORD_EMOJI_MAX_BYTES;
      memcpy(name_out, stored, nlen);
      name_out[nlen] = '\0';
      *id_out = eid;
      return;
    }
  }
  size_t len = strlen(stored);
  if (len > CCORD_EMOJI_MAX_BYTES) len = CCORD_EMOJI_MAX_BYTES;
  memcpy(name_out, stored, len);
  name_out[len] = '\0';
}

// The presence to broadcast when none was asked for: the last accepted set_presence wins until the server
// echoes it back, then the store's view of self.
ccord_presence session_current_presence(const ccord_session *s)
{
  if (s->has_pending_presence) return s->pending_presence;
  const ccord_user *me = ccord_store_user(s->store, ccord_store_self_id(s->store));
  return me ? me->presence : CCORD_PRESENCE_ONLINE;
}

// Broadcasts `presence` together with the stored custom status (the set_presence path without a status text).
ccord_status session_send_presence_with_custom(ccord_session *s, ccord_presence presence, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  const ccord_user *me = ccord_store_user(s->store, ccord_store_self_id(s->store));
  if (me && (me->custom_status_emoji[0] != '\0' || me->custom_status_text[0] != '\0')) {
    char ename[CCORD_EMOJI_MAX_BYTES + 1];
    ccord_id eid = CCORD_ID_NONE;
    split_stored_emoji(me->custom_status_emoji, ename, &eid);
    return ccord_transport_set_presence_ex(s->transport, presence, ename[0] ? ename : nullptr, eid,
                                           me->custom_status_text[0] ? me->custom_status_text : nullptr, err);
  }
  return ccord_transport_set_presence(s->transport, presence, nullptr, err);
}

// ---- status through settings-proto/1 ----
// Op 3 and the legacy settings PATCH reach this connection and the account's old settings, but other open clients
// follow the settings proto, so every status change is also written there (the transport patches the account's
// current type-1 bytes, keeping everything else). A transport without it keeps what it already sent.

static const char *const k_status_names[] = {
  [CCORD_PRESENCE_OFFLINE] = "invisible", [CCORD_PRESENCE_ONLINE] = "online",
  [CCORD_PRESENCE_IDLE] = "idle",         [CCORD_PRESENCE_DND] = "dnd",
};

// Fills the custom-status half of `ps` from the store's encoding ("name:id" for a custom emoji). Nothing set = none.
// False when a string does not fit the wire struct (the write is skipped rather than truncated).
static bool status_proto_custom(ccord_proto_status *ps, const char *stored_emoji, const char *text, int64_t expires_ms)
{
  char ename[CCORD_EMOJI_MAX_BYTES + 1];
  ccord_id eid = CCORD_ID_NONE;
  split_stored_emoji(stored_emoji ? stored_emoji : "", ename, &eid);
  if (!text) text = "";
  if (ename[0] == '\0' && text[0] == '\0') return true;
  if (strlen(text) > CCORD_PROTO_CUSTOM_TEXT_MAX || strlen(ename) > CCORD_PROTO_EMOJI_NAME_MAX) return false;
  ps->has_custom = true;
  memcpy(ps->custom_text, text, strlen(text) + 1u);
  memcpy(ps->emoji_name, ename, strlen(ename) + 1u);
  ps->emoji_id = eid;
  ps->custom_expires_at_ms = expires_ms > 0 ? (uint64_t)expires_ms : 0u;
  return true;
}

static void session_write_status_proto(ccord_session *s, ccord_presence presence, const char *stored_emoji,
                                       const char *text, int64_t expires_ms)
{
  if (presence > CCORD_PRESENCE_DND) return;
  ccord_proto_status ps = {0};
  ps.has_status = true;
  memcpy(ps.status, k_status_names[presence], strlen(k_status_names[presence]) + 1u);
  if (!status_proto_custom(&ps, stored_emoji, text, expires_ms)) {
    CCORD_LOG_W("session: status proto not written: custom status too long");
    return;
  }
  ccord_error err;
  ccord_status st = ccord_transport_set_status_proto(s->transport, &ps, &err);
  if (st != CCORD_OK && st != CCORD_ERR_UNSUPPORTED) CCORD_LOG_W("session: status proto write rejected: %s", err.message);
  if (st == CCORD_OK) {   // remember it so its echo is not taken for another client's change
    typeof(s->status_echo[0]) *e = &s->status_echo[s->status_echo_next++ % (sizeof s->status_echo / sizeof s->status_echo[0])];
    e->used = true;
    e->presence = presence;
    snprintf(e->emoji, sizeof e->emoji, "%s", stored_emoji ? stored_emoji : "");
    snprintf(e->text, sizeof e->text, "%s", text ? text : "");
    e->at_ms = ccord_clock_ms();
  }
}

// Another client changed the account's status: type-1 bytes arrived (a USER_SETTINGS_PROTO_UPDATE). Only a proto that
// carries the status submessage says anything; the presence and custom status of the store follow it and op 3 goes out
// again so this connection shows the same. In a partial (delta) update an absent custom_status means "unchanged".
void session_apply_status_proto(ccord_session *s, const ccord_net_event *ev)
{
  if (!ev->settings_proto1 || ev->settings_proto1_len == 0) return;
  ccord_proto_status ps;
  ccord_error err;
  if (ccord_user_status_decode(ev->settings_proto1, ev->settings_proto1_len, &ps, &err) != CCORD_OK) {
    CCORD_LOG_W("session: settings proto status undecodable: %s", err.message);
    return;
  }
  ccord_id self = ccord_store_self_id(s->store);
  const ccord_user *me = self != CCORD_ID_NONE ? ccord_store_user(s->store, self) : nullptr;
  if (!ps.present || !me) return;

  ccord_presence cur = session_current_presence(s), np = cur;
  if (ps.has_status) {
    for (size_t i = 0; i < sizeof k_status_names / sizeof k_status_names[0]; i++) {
      if (strcmp(ps.status, k_status_names[i]) == 0) np = (ccord_presence)i;
    }
  }
  char emoji[CCORD_EMOJI_MAX_BYTES + 1];
  char text[129];   // the store keeps at most 128 bytes of status text
  int64_t exp_ms = me->custom_status_expires_ms;
  bool clear_custom = false;
  snprintf(emoji, sizeof emoji, "%s", me->custom_status_emoji);
  snprintf(text, sizeof text, "%s", me->custom_status_text);
  if (ps.has_custom && (strlen(ps.custom_text) > sizeof text - 1u || strlen(ps.emoji_name) > 40u)) {
    CCORD_LOG_W("session: custom status from settings proto too long, ignored");
  } else if (ps.has_custom) {
    if (ps.emoji_id != 0 && ps.emoji_name[0] != '\0') {
      snprintf(emoji, sizeof emoji, "%.40s:%llu", ps.emoji_name, (unsigned long long)ps.emoji_id);
    } else {
      snprintf(emoji, sizeof emoji, "%.40s", ps.emoji_name);
    }
    // The guard above already ignored text longer than 128 bytes; the clamp only lets the compiler prove the copy fits.
    size_t text_len = strlen(ps.custom_text);
    if (text_len > sizeof text - 1u) text_len = sizeof text - 1u;
    memcpy(text, ps.custom_text, text_len);
    text[text_len] = '\0';
    exp_ms = (int64_t)ps.custom_expires_at_ms;
    clear_custom = emoji[0] == '\0' && text[0] == '\0';
  } else if (!ev->settings_proto1_partial) {
    emoji[0] = text[0] = '\0';
    exp_ms = 0;
    clear_custom = true;
  }
  bool custom_changed = strcmp(emoji, me->custom_status_emoji) != 0 || strcmp(text, me->custom_status_text) != 0 ||
                        (!clear_custom && exp_ms != me->custom_status_expires_ms);
  if (np == cur && !custom_changed && me->presence == cur) return;
  // The echo of one of our own writes that a newer write has already replaced: ignore it (the newer echo follows). The
  // newest write is never skipped, so another client choosing the same status as our last write still applies.
  size_t latest = (s->status_echo_next + sizeof s->status_echo / sizeof s->status_echo[0] - 1u) %
                  (sizeof s->status_echo / sizeof s->status_echo[0]);
  for (size_t i = 0; i < sizeof s->status_echo / sizeof s->status_echo[0]; i++) {
    typeof(s->status_echo[0]) *e = &s->status_echo[i];
    if (!e->used || i == latest || ccord_clock_ms() - e->at_ms >= 10000u) continue;
    if (e->presence == np && strcmp(e->emoji, emoji) == 0 && strcmp(e->text, text) == 0) {
      e->used = false;   // consumed
      return;
    }
  }

  if (np != cur || me->presence != np) {
    char *keep = ccord_strdup(me->status_text ? me->status_text : "");
    if (ccord_store_update_presence(s->store, self, np, keep, &err) != CCORD_OK) {
      CCORD_LOG_W("session: status from settings proto not applied: %s", err.message);
    }
    ccord_free(keep);
  }
  if (custom_changed &&
      ccord_store_set_self_custom_status(s->store, emoji, text, exp_ms, clear_custom, &err) != CCORD_OK) {
    CCORD_LOG_W("session: custom status from settings proto not applied: %s", err.message);
  }
  s->has_pending_presence = false;   // the store now holds what the account says
  if (session_send_presence_with_custom(s, np, &err) != CCORD_OK) {
    CCORD_LOG_D("session: status op 3 not re-sent: %s", err.message);
  }
}

// A new READY (SNAPSHOT) belongs to a fresh gateway that knows nothing about the Rich Presence activities:
// push the stored array again and re-broadcast the presence so the activity goes out without waiting for the
// next status change. No-op when no activity is stored.
void session_repush_rich_presence(ccord_session *s)
{
  if (!s->rich_json || s->stale_has_rich) return;   // a change held back while stale goes out by the replay
  ccord_error err;
  if (ccord_transport_set_rich_activities(s->transport, s->rich_json, s->rich_len, &err) != CCORD_OK) {
    CCORD_LOG_W("session: rich presence re-push rejected: %s", err.message);
    return;
  }
  if (session_send_presence_with_custom(s, session_current_presence(s), &err) != CCORD_OK) {
    CCORD_LOG_W("session: rich presence resend rejected: %s", err.message);
  } else {
    s->rich_op3_dirty = false;
  }
}

// READY cleared stale: send what was held back while it was set (each once).

ccord_status ccord_session_set_presence(ccord_session *s, ccord_presence presence, const char *status_text,
                                        ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  // CCORD_PRESENCE_OFFLINE as a request means Invisible (the gateway status "invisible").
  if (presence > CCORD_PRESENCE_DND) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: presence");
  }
  ccord_status st;
  if (status_text) {
    size_t n = strlen(status_text);
    if (n > 128u || !ccord_utf8_valid_text(status_text, n)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: presence text");
    }
    st = ccord_transport_set_presence(s->transport, presence, status_text, err);
  } else {
    st = session_send_presence_with_custom(s, presence, err);
  }
  if (st != CCORD_OK) return st;
  s->has_pending_presence = true;
  s->pending_presence = presence;
  // Discord does not reliably echo our own op 3 back as a PRESENCE_UPDATE, so the store (and with it the user panel)
  // would keep showing the old status forever: apply it here. The custom status is left as it is.
  ccord_id self = ccord_store_self_id(s->store);
  if (self != CCORD_ID_NONE) {
    ccord_error serr;
    if (ccord_store_update_presence(s->store, self, presence, status_text, &serr) != CCORD_OK) {
      CCORD_LOG_W("session: own presence not applied locally: %s", serr.message);
    }
  }
  {   // other open clients follow the settings proto, not op 3 (the custom status stays as the store has it)
    const ccord_user *own = self != CCORD_ID_NONE ? ccord_store_user(s->store, self) : nullptr;
    session_write_status_proto(s, presence, own ? own->custom_status_emoji : nullptr,
                               own ? own->custom_status_text : nullptr, own ? own->custom_status_expires_ms : 0);
  }
  // Op 3 only changes this connection. The account's status setting is what survives a reconnect and what other
  // sessions follow, so persist it the way the web client does. A refusal comes back as an action failure.
  ccord_admin_request req;
  st = ccord_admin_set_status(&req, presence, err);
  if (st != CCORD_OK) return st;
  st = ccord_session_admin(s, &req, err);
  // A transport with no account REST (a bare gateway) keeps the op 3 it already accepted.
  if (st == CCORD_ERR_UNSUPPORTED) {
    ccord_error_clear(err);
    return CCORD_OK;
  }
  return st;
}

ccord_status ccord_session_set_rich_presence(ccord_session *s, const char *activities_json, ccord_error *err)
{
  if (s && s->stale) {   // refused now, remembered, applied once after READY
    char *copy = activities_json && activities_json[0] ? ccord_strndup(activities_json, strlen(activities_json)) : nullptr;
    if (copy || !activities_json || !activities_json[0]) {
      ccord_free(s->stale_rich);
      s->stale_rich = copy;
      s->stale_has_rich = true;
    }
  }
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  size_t len = activities_json ? strlen(activities_json) : 0;
  char *copy = nullptr;
  if (len > 0) {
    copy = ccord_strndup(activities_json, len);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
  }
  // The transport validates the array (shape and size limits); a rejected set keeps the stored one.
  ccord_status st = ccord_transport_set_rich_activities(s->transport, activities_json, len, err);
  if (st != CCORD_OK) {
    ccord_free(copy);
    return st;
  }
  {   // Discord may never echo our own presence back: an activity we published and now withdraw ends here too
    ccord_id self = ccord_store_self_id(s->store);
    const ccord_user *me = self != CCORD_ID_NONE ? ccord_store_user(s->store, self) : nullptr;
    if (me && me->has_activity &&
        (rich_json_has_name(s->rich_json, me->activity_name) || s->self_activity_from_rich) &&
        !rich_json_has_name(copy, me->activity_name)) {
      session_set_self_activity(s, false, CCORD_ACTIVITY_PLAYING, "", "", "", 0, 0);
      s->self_activity_from_rich = false;
    }
  }
  ccord_free(s->rich_json);
  ccord_free(s->stale_rich);
  s->stale_rich = nullptr;
  s->stale_has_rich = false;
  s->rich_json = copy;
  s->rich_len = copy ? len : 0;
  s->self_activity_from_rich = rich_json_has_any(copy);
  s->rich_op3_dirty = true;
  if (s->state == CCORD_CONN_CONNECTED) {
    ccord_status gw_st = session_send_presence_with_custom(s, session_current_presence(s), err);
    if (gw_st != CCORD_OK) return gw_st;
    s->rich_op3_dirty = false;
  }
  return CCORD_OK;
}


ccord_status ccord_session_set_custom_status(ccord_session *s, const char *emoji_name, ccord_id emoji_id,
                                             const char *text, int64_t expires_unix_ms, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const char *ename = emoji_name ? emoji_name : "";
  const char *txt = text ? text : "";
  size_t elen = strlen(ename);
  size_t tlen = strlen(txt);
  if (elen > CCORD_EMOJI_MAX_BYTES || memchr(ename, 0, elen) || !ccord_utf8_valid_text(ename, elen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid custom status emoji");
  }
  if (tlen > 128u || memchr(txt, 0, tlen) || !ccord_utf8_valid_text(txt, tlen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid custom status text");
  }
  bool clear = (elen == 0 && emoji_id == CCORD_ID_NONE && tlen == 0);
  if (!clear && expires_unix_ms < 0) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "session: invalid custom status expiry");
  }
  ccord_admin_request req;
  ccord_status bst = ccord_admin_set_custom_status(&req, ename, emoji_id, txt, clear ? 0 : expires_unix_ms, err);
  if (bst != CCORD_OK) return bst;
  ccord_status st = ccord_session_admin(s, &req, err);
  if (st != CCORD_OK) return st;
  // The REST write persists it; op 3 makes it show on this connection at once. A gateway that refuses the frame is
  // the caller's problem too (the status would otherwise look set while nobody sees it).
  ccord_presence cur = session_current_presence(s);
  st = ccord_transport_set_presence_ex(s->transport, cur, clear ? nullptr : (elen ? ename : nullptr),
                                       clear ? CCORD_ID_NONE : emoji_id, clear ? nullptr : (tlen ? txt : nullptr), err);
  // The settings proto is what other clients follow: write the new custom status (or its removal) there too. The
  // store holds a custom emoji as "name:id".
  char stored[CCORD_EMOJI_MAX_BYTES + 32u];
  if (!clear && emoji_id != CCORD_ID_NONE) snprintf(stored, sizeof stored, "%s:%llu", ename, (unsigned long long)emoji_id);
  else snprintf(stored, sizeof stored, "%s", clear ? "" : ename);
  session_write_status_proto(s, cur, stored, clear ? "" : txt, clear ? 0 : expires_unix_ms);
  return st;
}


