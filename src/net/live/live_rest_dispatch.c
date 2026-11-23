// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "live_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ccord/core/os.h>

bool live_push_media_event_owned(live *l, const char *url, const char *path, char *owned_data, size_t len)
{
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  size_t ulen = url ? strlen(url) : 0;
  size_t plen = path ? strlen(path) : 0;
  ccord_arena_init(&ev.arena, ulen + plen + 64u);
  char *ucopy = ccord_arena_strndup(&ev.arena, url ? url : "", ulen);
  char *pcopy = ccord_arena_strndup(&ev.arena, path ? path : "", plen);
  if (!ucopy || !pcopy) {
    ccord_live_event_free(&ev);
    ccord_free(owned_data);
    return false;
  }
  ev.ev.type = CCORD_NET_MEDIA_LOADED;
  ev.ev.media_url = ucopy;
  ev.ev.media_path = pcopy;
  ev.ev.media_data = (const uint8_t *)owned_data;
  ev.ev.media_len = len;
  return live_push_with_media(l, &ev, owned_data);
}

static bool push_media_event(live *l, const char *url, const char *path, const void *data, size_t len)
{
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  size_t ulen = url ? strlen(url) : 0;
  size_t plen = path ? strlen(path) : 0;
  ccord_arena_init(&ev.arena, ulen + plen + len + 64u);
  char *ucopy = ccord_arena_strndup(&ev.arena, url ? url : "", ulen);
  char *pcopy = ccord_arena_strndup(&ev.arena, path ? path : "", plen);
  uint8_t *dcopy = len > 0 ? ccord_arena_alloc_uninit(&ev.arena, len, 1u) : ccord_arena_alloc(&ev.arena, 1u, 1u);
  if (!ucopy || !pcopy || !dcopy) {
    ccord_live_event_free(&ev);
    return false;
  }
  if (len > 0 && data) memcpy(dcopy, data, len);
  ev.ev.type = CCORD_NET_MEDIA_LOADED;
  ev.ev.media_url = ucopy;
  ev.ev.media_path = pcopy;
  ev.ev.media_data = dcopy;
  ev.ev.media_len = len;
  return live_push(l, &ev);
}

// One answer to a ranged video fetch. `st` != OK is a failure (no data); `ri` is null when the queue gave no info.
// Always carries the request echo (url, id) so the stream layer can match it, and the final HTTP status and hops.
// If `owned_data` is non-null, takes ownership (zero-copy) instead of copying into `ev.arena`.
static void push_media_range_event(live *l, const live_rest_op *op, ccord_status st, const ccord_rest_range_info *ri,
                                   uint32_t flags, int64_t total, const void *data, size_t len, char *owned_data)
{
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  size_t ulen = op->media_url ? strlen(op->media_url) : 0;
  ccord_arena_init(&ev.arena, ulen + (owned_data ? 0u : len) + 64u);
  char *ucopy = ccord_arena_strndup(&ev.arena, op->media_url ? op->media_url : "", ulen);
  const uint8_t *mdata = nullptr;
  if (owned_data) {
    mdata = (const uint8_t *)owned_data;
  } else {
    uint8_t *dcopy = len > 0 ? ccord_arena_alloc_uninit(&ev.arena, len, 1u) : ccord_arena_alloc(&ev.arena, 1u, 1u);
    if (!dcopy) {
      ccord_live_event_free(&ev);
      return;
    }
    if (len > 0 && data) memcpy(dcopy, data, len);
    mdata = dcopy;
  }
  if (!ucopy) {
    ccord_live_event_free(&ev);
    ccord_free(owned_data);
    return;
  }
  ev.ev.type = CCORD_NET_MEDIA_RANGE;
  ev.ev.status = st;
  ev.ev.media_url = ucopy;
  ev.ev.media_path = "";
  ev.ev.media_data = mdata;
  ev.ev.media_len = len;
  ev.ev.range_id = op->range_id;
  ev.ev.range_start = ri && ri->http_status == 206 ? ri->range_start : op->range_start;
  ev.ev.range_total = total;
  ev.ev.range_flags = flags;
  ev.ev.range_http = ri ? (int32_t)ri->http_status : 0;
  ev.ev.range_redirects = ri ? ri->redirects : 0;
  if (ri) (void)snprintf(ev.ev.range_etag, sizeof ev.ev.range_etag, "%s", ri->etag);
  (void)live_push_with_media(l, &ev, owned_data);
}

// Completion of a LIVE_REST_MEDIA_RANGE job (the queue already checked a 206 against the request).
//   206            -> OK with the bytes; EOF when the total is known and reached, or when the total is unknown and the
//                     server sent less than asked (then the total is start + got)
//   200            -> OK, UNRANGED, no bytes: the server ignored Range (the queue kept at most range_len bytes)
//   416            -> OK, EOF, no bytes: the file is shorter than assumed; the total is from "bytes */T" when sent
//   401 403 410    -> PERMISSION   404 -> NOT_FOUND   anything else -> IO (a transport error keeps its own status)
static void live_media_range_done(live_rest_op *op, ccord_status status, long http_status, const char *body, size_t len)
{
  live *l = op->l;
  const ccord_rest_range_info *ri = &op->range_info;
  ccord_rest_range_info none = {.http_status = http_status, .entity_total = -1};
  if (ri->http_status == 0 && http_status != 0) ri = &none;   // the queue gave no info (for example a destroyed queue)
  if (status != CCORD_OK) {
    push_media_range_event(l, op, status, ri, 0, -1, nullptr, 0, nullptr);
    live_log_media_failure(op->media_url, status, http_status);
    return;
  }
  ccord_status st = CCORD_OK;
  uint32_t flags = 0;
  int64_t total = ri->entity_total;
  const void *data = nullptr;
  size_t dlen = 0;
  if (http_status == 206) {
    data = body;
    dlen = len;
    flags |= CCORD_RANGE_ACCEPTS;
    int64_t end = ri->range_start + (int64_t)len;
    if (total < 0 && len < op->range_len) {
      total = end;
      flags |= CCORD_RANGE_EOF;
    } else if (total >= 0 && end >= total) {
      flags |= CCORD_RANGE_EOF;
    }
    if (len == 0) st = CCORD_ERR_PROTOCOL;
  } else if (http_status == 200) {
    flags |= CCORD_RANGE_UNRANGED;
  } else if (http_status == 416) {
    flags |= CCORD_RANGE_ACCEPTS | CCORD_RANGE_EOF;
  } else if (http_status == 401 || http_status == 403 || http_status == 410) {
    st = CCORD_ERR_PERMISSION;
  } else if (http_status == 404) {
    st = CCORD_ERR_NOT_FOUND;
  } else {
    st = CCORD_ERR_IO;
  }
  if (st != CCORD_OK) {
    push_media_range_event(l, op, st, ri, 0, -1, nullptr, 0, nullptr);
    live_log_media_failure(op->media_url, CCORD_OK, http_status);
    return;
  }
  char *owned = nullptr;
  if (data && dlen > 0 && l->restq) {
    size_t taken = 0;
    char *buf = ccord_rest_take_active_resp(l->restq, &taken);
    if (buf && taken == dlen) owned = buf;
    else ccord_free(buf);
  }
  push_media_range_event(l, op, CCORD_OK, ri, flags, total, data, dlen, owned);
}

// A media download that did not produce bytes: MEDIA_LOADED with a failure status and no data, so the session can
// forget the pending request and the UI can try again later (or fall back to another URL).
static void push_media_failed(live *l, const char *url, ccord_status status)
{
  if (!url || url[0] == '\0') return;
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  size_t ulen = strlen(url);
  ccord_arena_init(&ev.arena, ulen + 64u);
  char *ucopy = ccord_arena_strndup(&ev.arena, url, ulen);
  if (!ucopy) {
    ccord_live_event_free(&ev);
    return;
  }
  ev.ev.type = CCORD_NET_MEDIA_LOADED;
  ev.ev.status = status != CCORD_OK ? status : CCORD_ERR_IO;
  ev.ev.media_url = ucopy;
  ev.ev.media_path = "";
  (void)live_push(l, &ev);
}

static void push_history_error(live *l, ccord_id channel_id, ccord_status status, const char *msg)
{
  ccord_live_event err_ev;
  ccord_live_event_init(&err_ev);
  err_ev.ev.type = CCORD_NET_ERROR;
  err_ev.ev.channel_id = channel_id;
  err_ev.ev.status = status;
  err_ev.ev.text = msg ? ccord_arena_strndup(&err_ev.arena, msg, strlen(msg)) : "rest: history failed";
  (void)live_push(l, &err_ev);
}

static void push_history_failed(live *l, ccord_id channel_id, ccord_status status, const char *msg)
{
  ccord_live_event err_ev;
  ccord_live_event_init(&err_ev);
  err_ev.ev.type = CCORD_NET_HISTORY_FAILED;
  err_ev.ev.channel_id = channel_id;
  err_ev.ev.status = status;
  err_ev.ev.text = msg ? ccord_arena_strndup(&err_ev.arena, msg, strlen(msg)) : "rest: history before failed";
  (void)live_push(l, &err_ev);
}

static void push_profile_error(live *l, ccord_id user_id, ccord_status status, const char *msg)
{
  ccord_live_event err_ev;
  ccord_live_event_init(&err_ev);
  err_ev.ev.type = CCORD_NET_ERROR;
  err_ev.ev.user_id = user_id;
  err_ev.ev.status = status;
  err_ev.ev.text = msg ? ccord_arena_strndup(&err_ev.arena, msg, strlen(msg)) : "rest: user profile failed";
  (void)live_push(l, &err_ev);
}

static void push_forum_failed(live *l, ccord_id channel_id, uint32_t offset, bool creating,
                              ccord_status status, const char *msg)
{
  ccord_live_event err_ev;
  ccord_live_event_init(&err_ev);
  err_ev.ev.type = CCORD_NET_FORUM_FAILED;
  err_ev.ev.channel_id = channel_id;
  err_ev.ev.forum_offset = offset;
  err_ev.ev.forum_creating = creating;
  err_ev.ev.status = status;
  err_ev.ev.text = msg ? ccord_arena_strndup(&err_ev.arena, msg, strlen(msg)) : "rest: forum request failed";
  (void)live_push(l, &err_ev);
}

static void push_discovery_error(live *l, ccord_status status, const char *msg)
{
  ccord_live_event err_ev;
  ccord_live_event_init(&err_ev);
  err_ev.ev.type = CCORD_NET_ERROR;
  err_ev.ev.status = status;
  err_ev.ev.text = msg ? ccord_arena_strndup(&err_ev.arena, msg, strlen(msg)) : "rest: discovery failed";
  (void)live_push(l, &err_ev);
}

// Which user-facing action a request kind belongs to (NONE: failures are only logged).
static ccord_action_kind live_kind_action(live_rest_kind kind)
{
  switch (kind) {
    case LIVE_REST_SEND_MESSAGE:
    case LIVE_REST_SEND_TTS:
    case LIVE_REST_FORWARD_MESSAGE: return CCORD_ACTION_SEND;
    case LIVE_REST_SEND_REPLY: return CCORD_ACTION_REPLY;
    case LIVE_REST_CREATE_POLL: return CCORD_ACTION_CREATE_POLL;
    case LIVE_REST_REPORT_MENU: return CCORD_ACTION_REPORT_MENU;
    case LIVE_REST_REPORT: return CCORD_ACTION_REPORT;
    case LIVE_REST_EDIT_MESSAGE:
    case LIVE_REST_CROSSPOST_MESSAGE: return CCORD_ACTION_EDIT;
    case LIVE_REST_DELETE_MESSAGE: return CCORD_ACTION_DELETE;
    case LIVE_REST_REACTION: return CCORD_ACTION_REACTION;
    case LIVE_REST_PIN: return CCORD_ACTION_PIN;
    case LIVE_REST_UPLOAD_SLOT:
    case LIVE_REST_UPLOAD_PUT:
    case LIVE_REST_UPLOAD_SEND: return CCORD_ACTION_UPLOAD;
    case LIVE_REST_SEND_STICKER: return CCORD_ACTION_STICKER;
    case LIVE_REST_OPEN_DM: return CCORD_ACTION_OPEN_DM;
    case LIVE_REST_RELATIONSHIP: return CCORD_ACTION_RELATIONSHIP;
    case LIVE_REST_ADMIN: return CCORD_ACTION_ADMIN;
    case LIVE_REST_SOUNDBOARD:
    case LIVE_REST_CALL_RING: return CCORD_ACTION_ADMIN;
    case LIVE_REST_POLL_VOTE: return CCORD_ACTION_POLL_VOTE;
    case LIVE_REST_INTERACTION: return CCORD_ACTION_INTERACTION;
    case LIVE_REST_HISTORY:
    case LIVE_REST_HISTORY_BEFORE:
    case LIVE_REST_PINS:
    case LIVE_REST_PINS_BEFORE:
    case LIVE_REST_ACK:
    case LIVE_REST_SEARCH:
    case LIVE_REST_MEDIA:
    case LIVE_REST_MEDIA_RANGE:
    case LIVE_REST_USER_PROFILE:
    case LIVE_REST_PROFILE_EFFECTS:
    case LIVE_REST_COLLECTIBLE_PRODUCT:
    case LIVE_REST_CHANNEL:
    case LIVE_REST_FORUM_POSTS:
    case LIVE_REST_FORUM_CREATE:      // has its own FORUM_FAILED event
    case LIVE_REST_EXPRESSION_DATA:
    case LIVE_REST_TYPING:
    case LIVE_REST_REACTION_REFRESH:
    case LIVE_REST_REACTION_USERS:
    case LIVE_REST_INTERACTION_AUTOCOMPLETE:
    case LIVE_REST_SLASH_COMMANDS:
    case LIVE_REST_DISCOVERY:
    case LIVE_REST_MARK_UNREAD:
    case LIVE_REST_PATCH_SETTINGS_PROTO:
    case LIVE_REST_STATUS_GET:
    case LIVE_REST_STATUS_PATCH: return CCORD_ACTION_NONE;
  }
  return CCORD_ACTION_NONE;
}

// A short sentence for the user (it ends up in a one-line notice). `st` is the transport status, `http` the HTTP
// status (0 when no response arrived). `discord` is Discord's own wording from the error body ("Missing
// Permissions"), appended when it adds something: it is the only place the real reason is spelled out.
static void live_action_reason(ccord_action_kind action, ccord_status st, long http, const char *discord, char *out,
                               size_t cap)
{
  bool may_have_applied = action == CCORD_ACTION_SEND || action == CCORD_ACTION_REPLY ||
                          action == CCORD_ACTION_EDIT || action == CCORD_ACTION_UPLOAD ||
                          action == CCORD_ACTION_STICKER || action == CCORD_ACTION_ADMIN ||
                          action == CCORD_ACTION_POLL_VOTE || action == CCORD_ACTION_INTERACTION ||
                          action == CCORD_ACTION_CREATE_POLL || action == CCORD_ACTION_REPORT;
  const char *msg = nullptr;
  if (http == 401) msg = "Discord rejected your login. Sign in again.";
  else if (http == 403) msg = "You don't have permission to do that.";
  else if (http == 404) msg = "It no longer exists.";
  else if (http == 429) msg = "Discord is rate limiting you. Try again shortly.";
  else if (http == 400 || http == 422) msg = "Discord rejected it.";
  else if (http == 409) msg = "It conflicts with something that changed.";
  else if (http == 413) msg = "File is too large for Discord to accept.";
  else if (st == CCORD_ERR_TIMEOUT) {
    msg = may_have_applied ? "Timed out. It may still have gone through." : "Timed out talking to Discord.";
  } else if (st == CCORD_ERR_RANGE) msg = "It is too large.";
  bool detail = discord && discord[0] != '\0' && http >= 400 && http < 500 && http != 401 && http != 429;
  if (msg && detail) {
    (void)snprintf(out, cap, "%s (%s)", msg, discord);
  } else if (msg) {
    (void)snprintf(out, cap, "%s", msg);
  } else if (http >= 500) {
    (void)snprintf(out, cap, "Discord is having problems (HTTP %ld). Try again.", http);
  } else if (detail) {
    (void)snprintf(out, cap, "Discord refused it (%s)", discord);
  } else {
    (void)snprintf(out, cap, "Network error. Check your connection.");
  }
}

// Reports that a user action did not go through, with the text they wrote so the UI can give it back.
// `body` is the response body when there was one (it supplies Discord's own error wording).
static void push_action_failed(live *l, const live_rest_op *op, ccord_status st, long http, const char *body,
                               size_t body_len)
{
  ccord_action_kind action = live_kind_action(op->kind);
  if (action == CCORD_ACTION_NONE && op->kind != LIVE_REST_MARK_UNREAD) return;
  char discord[200] = "";
  if (body && body_len > 0) (void)ccord_live_json_error_message(body, body_len, discord, sizeof discord);
  char reason[320];
  live_action_reason(action, st, http, discord, reason, sizeof reason);
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  size_t rlen = strlen(reason);
  size_t dlen = op->text ? op->text_len : 0;
  ccord_arena_init(&ev.arena, rlen + dlen + 64u);
  ev.ev.type = CCORD_NET_ACTION_FAILED;
  ev.ev.action = action;
  ev.ev.channel_id = op->channel_id;
  ev.ev.message_id = op->message_id;
  ev.ev.guild_id = op->guild_id;
  ev.ev.user_id = op->user_id;
  ev.ev.expr_id = op->expr_id;
  ev.ev.scheduled_event_id = op->event_id;
  ev.ev.admin_op = op->admin_op;
  ev.ev.status = st != CCORD_OK ? st : CCORD_ERR_PROTOCOL;
  memcpy(ev.ev.action_nonce, op->nonce, sizeof ev.ev.action_nonce);   // M0-3: which pending echo this refusal belongs to
  ev.ev.text = ccord_arena_strndup(&ev.arena, reason, rlen);
  if (dlen > 0) {
    ev.ev.draft = ccord_arena_strndup(&ev.arena, op->text, dlen);
    ev.ev.draft_len = dlen;
  }
  (void)live_push(l, &ev);
  if (op->kind == LIVE_REST_MARK_UNREAD) {
    ccord_id prev_ack = live_chan_last_ack(l, op->channel_id);
    if (prev_ack != CCORD_ID_NONE) {
      ccord_live_event ack_ev;
      ccord_live_event_init(&ack_ev);
      ack_ev.ev.type = CCORD_NET_MESSAGE_ACK;
      ack_ev.ev.channel_id = op->channel_id;
      ack_ev.ev.message_id = prev_ack;
      ack_ev.ev.mention_count = UINT32_MAX;
      (void)live_push(l, &ack_ev);
    }
  }
}

// REACTION_UPDATE for a finished reaction change. With the refreshed message (`json`) the counts are authoritative;
// without it (the refresh failed or was never sent) the event carries a +1/-1 delta instead.
static void push_reaction_event(live *l, const live_rest_op *op, const char *json, size_t json_len)
{
  size_t elen = op->emoji ? strlen(op->emoji) : 0;
  ccord_live_event ev;
  bool have = false;
  if (json) {
    ccord_error jerr;
    have = ccord_live_json_message(json, json_len, CCORD_NET_REACTION_UPDATE, &ev, &jerr) == CCORD_OK;
    if (!have) ccord_live_event_free(&ev);
  }
  if (!have) {
    ccord_live_event_init(&ev);
    ccord_arena_init(&ev.arena, elen + 32u);
  }
  ev.ev.type = CCORD_NET_REACTION_UPDATE;
  ev.ev.channel_id = op->channel_id;
  ev.ev.message_id = op->message_id;
  ev.ev.user_id = l->self_id;
  ev.ev.reaction_emoji = ccord_arena_strndup(&ev.arena, op->emoji ? op->emoji : "", elen);
  if (have && ev.message_count > 0) {
    ev.ev.reaction_count = ev.messages[0].reaction_count;
    ev.ev.self_reacted = ev.messages[0].self_reacted;
  } else {
    ev.ev.self_reacted = op->flag;
    ev.ev.reaction_count = op->flag ? 1u : 0u;
    ev.ev.reaction_delta = op->flag ? 1 : -1;
  }
  (void)live_push(l, &ev);
}

bool live_is_authenticated(const live *l)
{
  return l && (l->state == CCORD_CONN_CONNECTED || l->self_id != CCORD_ID_NONE || l->token[0] != '\0');
}

// An administration call succeeded. The same changes arrive again from the gateway (CHANNEL_CREATE, GUILD_MEMBER_UPDATE,
// ...); applying them from the REST answer as well keeps the UI right when the gateway is slow, and every store
// update below is idempotent. Nothing is applied before Discord has said yes.
// Defined in discord_json.c.
void live_fill_guild_profile_unavailable(ccord_live_event *ev, ccord_id guild_id)
{
  ccord_live_event_init(ev);
  ev->ev.type = CCORD_NET_GUILD_PROFILE;
  ev->ev.guild_id = guild_id;
  ev->ev.guild_profile = (ccord_guild_profile){
    .guild_id = guild_id,
    .unavailable = true,
  };
}

static void push_guild_profile_unavailable(live *l, ccord_id guild_id)
{
  ccord_live_event unavail;
  live_fill_guild_profile_unavailable(&unavail, guild_id);
  (void)live_push(l, &unavail);
}



bool live_b64_decode_alloc(const char *b64, size_t len, uint8_t **out_raw, size_t *out_len)
{
  *out_raw = nullptr;
  *out_len = 0;
  if (len > 0 && !b64) return false;
  if (len > CCORD_PROTO_MAX_INPUT * 2u) return false;
  if (len == 0) {
    *out_raw = ccord_malloc(1);
    return *out_raw != nullptr;
  }
  uint8_t *raw = ccord_base64_decode_alloc(b64, len, out_len);
  if (!raw) return false;
  *out_raw = raw;
  return true;
}

ccord_status live_b64_encode(const uint8_t *src, size_t len, ccord_buf *out)
{
  size_t enc_len = 0;
  char *enc = ccord_base64_encode_alloc(src, len, &enc_len);
  if (!enc) return CCORD_ERR_NOMEM;
  ccord_status st = ccord_buf_append(out, enc, enc_len);
  ccord_free(enc);
  return st;
}

static bool live_settings_json_raw_proto(const char *json, size_t len, uint8_t **out_raw, size_t *out_len)
{
  *out_raw = nullptr;
  *out_len = 0;
  if (!json || len == 0) return false;
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  ccord_error jerr;
  ccord_json_limits lim = {.max_depth = 8, .max_nodes = 256, .max_string = CCORD_PROTO_MAX_INPUT * 2u};
  const ccord_json *root = ccord_json_parse(&tmp, json, len, &lim, &jerr);
  bool ok = false;
  if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    const ccord_json *node = ccord_json_get(root, "settings");
    size_t slen = 0;
    const char *s = node ? ccord_json_string(node, &slen) : nullptr;
    if (s) ok = live_b64_decode_alloc(s, slen, out_raw, out_len);
  }
  ccord_arena_free(&tmp);
  return ok;
}

void live_push_expression_data_ok(live *l, ccord_expr_kind kind, const char *arg, size_t arg_len,
                                    const char *body, size_t body_len,
                                    const uint8_t *fallback_proto, size_t fallback_proto_len, bool has_fallback_proto,
                                    const char *cache_path)
{
  uint8_t *decoded = nullptr;
  size_t decoded_len = 0;
  const uint8_t *raw_src = nullptr;
  size_t raw_len = 0;
  bool have_raw = false;
  ccord_buf synth;
  ccord_buf_init(&synth, 0);
  const char *eff_body = body ? body : "";
  size_t eff_len = body ? body_len : 0;

  if (kind == CCORD_EXPR_SETTINGS_PROTO) {
    if (live_settings_json_raw_proto(body, body_len, &decoded, &decoded_len)) {
      raw_src = decoded;
      raw_len = decoded_len;
      have_raw = true;
      if (cache_path && body && body_len > 0) live_api_cache_write(cache_path, body, body_len);
    } else if (has_fallback_proto) {
      raw_src = fallback_proto;
      raw_len = fallback_proto_len;
      have_raw = true;
      ccord_buf b64;
      ccord_buf_init(&b64, 0);
      if (live_b64_encode(fallback_proto, fallback_proto_len, &b64) == CCORD_OK) {
        ccord_jw w;
        ccord_jw_init(&w, &synth);
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "settings");
        ccord_jw_string(&w, ccord_buf_cstr(&b64), b64.len);
        ccord_jw_end_object(&w);
        if (ccord_jw_ok(&w)) {
          if (eff_len == 0) {
            eff_body = ccord_buf_cstr(&synth);
            eff_len = synth.len;
          }
          if (cache_path) live_api_cache_write(cache_path, ccord_buf_cstr(&synth), synth.len);
        }
      }
      ccord_buf_free(&b64);
    }
  } else if (cache_path && body && body_len > 0) {
    live_api_cache_write(cache_path, body, body_len);
  }

  ccord_live_event ev;
  ccord_live_event_init(&ev);
  ccord_arena_init(&ev.arena, eff_len + arg_len + raw_len + 64u);
  ev.ev.type = CCORD_NET_EXPRESSION_DATA;
  ev.ev.expr_kind = kind;
  ev.ev.expr_arg = (arg && arg_len > 0) ? ccord_arena_strndup(&ev.arena, arg, arg_len) : "";
  ev.ev.expr_body = ccord_arena_strndup(&ev.arena, eff_body, eff_len);
  ev.ev.expr_body_len = eff_len;
  if (have_raw) {
    uint8_t *rcopy = ccord_arena_alloc(&ev.arena, raw_len ? raw_len : 1u, 1u);
    if (rcopy) {
      if (raw_len > 0 && raw_src) memcpy(rcopy, raw_src, raw_len);
      ev.ev.raw_proto = rcopy;
      ev.ev.raw_proto_len = raw_len;
    }
  }
  ev.ev.status = CCORD_OK;
  (void)live_push(l, &ev);
  ccord_free(decoded);
  ccord_buf_free(&synth);
}

// A failed expression fetch must still reach the session: the blob was marked PENDING when the request was
// queued, and only this event moves it to FAILED so the picker can retry (a dropped event leaves it PENDING
// forever and every retry is suppressed).
static void push_expression_data_failed(live *l, const live_rest_op *op, ccord_status st)
{
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  size_t alen = op->expr_arg ? strlen(op->expr_arg) : 0;
  ccord_arena_init(&ev.arena, alen + 64u);
  ev.ev.type = CCORD_NET_EXPRESSION_DATA;
  ev.ev.expr_kind = op->expr_kind;
  ev.ev.expr_arg = (op->expr_arg && alen > 0) ? ccord_arena_strndup(&ev.arena, op->expr_arg, alen) : "";
  ev.ev.expr_body = nullptr;
  ev.ev.expr_body_len = 0;
  ev.ev.status = st != CCORD_OK ? st : CCORD_ERR_PROTOCOL;
  (void)live_push(l, &ev);
}

// A finished report call. Menu: the raw JSON goes to the session, which parses it with the report model. Submit: the
// optional report_id is read here; an answer without a usable one still counts as sent (Discord accepted it).
static void live_report_done(live *l, const live_rest_op *op, const char *body, size_t len)
{
  size_t alen = op->expr_arg ? strlen(op->expr_arg) : 0;
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  ccord_arena_init(&ev.arena, (op->kind == LIVE_REST_REPORT_MENU ? len : 0) + alen + 64u);
  ev.ev.expr_arg = (op->expr_arg && alen > 0) ? ccord_arena_strndup(&ev.arena, op->expr_arg, alen) : "";
  ev.ev.status = CCORD_OK;
  ev.ev.channel_id = op->channel_id;
  ev.ev.message_id = op->message_id;
  ev.ev.user_id = op->user_id;
  if (op->kind == LIVE_REST_REPORT_MENU) {
    ev.ev.type = CCORD_NET_REPORT_MENU;
    ev.ev.expr_body = ccord_arena_strndup(&ev.arena, body ? body : "", body ? len : 0);
    ev.ev.expr_body_len = body ? len : 0;
  } else {
    ev.ev.type = CCORD_NET_REPORT_SUBMITTED;
    if (body && len > 0) {
      ccord_arena ja;
      ccord_arena_init(&ja, len + 256u);
      ccord_error jerr;
      ccord_json_limits lim = {8, 256, 4096};
      const ccord_json *j = ccord_json_parse(&ja, body, len, &lim, &jerr);
      const ccord_json *rid = j ? ccord_json_get(j, "report_id") : nullptr;
      if (rid) {
        ccord_id id = CCORD_ID_NONE;
        int64_t n = 0;
        size_t sl = 0;
        const char *str = ccord_json_type_of(rid) == CCORD_JSON_STRING ? ccord_json_string(rid, &sl) : nullptr;
        if (str && ccord_id_parse(str, sl, &id)) ev.ev.report_id = id;
        else if (!str && ccord_json_int64(rid, &n) && n > 0) ev.ev.report_id = (ccord_id)n;
      }
      ccord_arena_free(&ja);
    }
  }
  if (!ev.ev.expr_arg || (op->kind == LIVE_REST_REPORT_MENU && !ev.ev.expr_body)) {
    ccord_live_event_free(&ev);
    return;
  }
  (void)live_push(l, &ev);
}


void live_rest_done(void *ud, ccord_status status, long http_status, const char *body, size_t len)
{
  live_rest_op *op = ud;
  if (!op) return;
  live *l = op->l;
  ccord_error err;
  if (op->kind == LIVE_REST_MEDIA_RANGE) {
    live_media_range_done(op, status, http_status, body, len);
    live_rest_op_free(op);
    return;
  }
  if (status != CCORD_OK) {
    if (op->kind == LIVE_REST_HISTORY) {
      push_history_error(l, op->channel_id, status, ccord_status_name(status));
    } else if (op->kind == LIVE_REST_HISTORY_BEFORE) {
      push_history_failed(l, op->channel_id, status, ccord_status_name(status));
    } else if (op->kind == LIVE_REST_USER_PROFILE) {
      push_profile_error(l, op->user_id, status, ccord_status_name(status));
    } else if (op->kind == LIVE_REST_PROFILE_EFFECTS) {
      l->effect_catalog_inflight = false;
      l->effect_catalog_loaded = true;
      live_flush_pending_decor_profiles(l);
    } else if (op->kind == LIVE_REST_COLLECTIBLE_PRODUCT) {
      if (l->product_inflight > 0) l->product_inflight--;
      live_flush_pending_decor_profiles(l);
    } else if (op->kind == LIVE_REST_FORUM_POSTS) {
      push_forum_failed(l, op->channel_id, op->forum_offset, false, status, ccord_status_name(status));
    } else if (op->kind == LIVE_REST_FORUM_CREATE) {
      push_forum_failed(l, op->channel_id, 0, true, status, ccord_status_name(status));
    } else if (op->kind == LIVE_REST_DISCOVERY) {
      push_discovery_error(l, status, ccord_status_name(status));
    } else if (op->kind == LIVE_REST_REACTION_REFRESH) {
      push_reaction_event(l, op, nullptr, 0);
    } else if (op->kind == LIVE_REST_EXPRESSION_DATA || op->kind == LIVE_REST_PATCH_SETTINGS_PROTO) {
      push_expression_data_failed(l, op, status);
    } else if (op->kind == LIVE_REST_STATUS_GET || op->kind == LIVE_REST_STATUS_PATCH) {
      live_status_failed(l, op);
    } else if (op->kind == LIVE_REST_MEDIA) {
      push_media_failed(l, op->media_url, status);
    } else if (op->kind == LIVE_REST_ADMIN && op->admin_op == CCORD_ADMIN_GET_GUILD_PROFILE) {
      push_guild_profile_unavailable(l, op->guild_id);
    } else {
      push_action_failed(l, op, status, http_status, nullptr, 0);
    }
    if (op->kind == LIVE_REST_MEDIA) live_log_media_failure(op->media_url, status, http_status);
    else CCORD_LOG_W("live: %s failed: %s", live_rest_name(op->kind), ccord_status_name(status));
    live_rest_op_free(op);
    return;
  }
  ccord_status hs = live_http_status_error(http_status, &err, live_rest_name(op->kind));
  if (hs != CCORD_OK) {
    // An expired signed attachment URL: the op turns into a refresh request, and no failure is reported unless that fails.
    if (live_media_try_refresh(l, op, http_status)) return;
    if (op->kind == LIVE_REST_HISTORY) {
      if (hs == CCORD_ERR_PERMISSION || hs == CCORD_ERR_NOT_FOUND) {
        ccord_live_event empty_ev;
        ccord_live_event_init(&empty_ev);
        empty_ev.ev.type = CCORD_NET_HISTORY;
        empty_ev.ev.channel_id = op->channel_id;
        (void)live_push(l, &empty_ev);
      } else {
        push_history_error(l, op->channel_id, hs, err.message);
      }
    } else if (op->kind == LIVE_REST_HISTORY_BEFORE) {
      push_history_failed(l, op->channel_id, hs, err.message);
    } else if (op->kind == LIVE_REST_USER_PROFILE) {
      push_profile_error(l, op->user_id, hs, err.message);
    } else if (op->kind == LIVE_REST_PROFILE_EFFECTS) {
      l->effect_catalog_inflight = false;
      l->effect_catalog_loaded = true;
      live_flush_pending_decor_profiles(l);
    } else if (op->kind == LIVE_REST_COLLECTIBLE_PRODUCT) {
      if (l->product_inflight > 0) l->product_inflight--;
      live_flush_pending_decor_profiles(l);
    } else if (op->kind == LIVE_REST_FORUM_POSTS) {
      push_forum_failed(l, op->channel_id, op->forum_offset, false, hs, err.message);
    } else if (op->kind == LIVE_REST_FORUM_CREATE) {
      push_forum_failed(l, op->channel_id, 0, true, hs, err.message);
    } else if (op->kind == LIVE_REST_DISCOVERY) {
      push_discovery_error(l, hs, err.message);
    } else if (op->kind == LIVE_REST_REACTION_REFRESH) {
      push_reaction_event(l, op, nullptr, 0);
    } else if (op->kind == LIVE_REST_EXPRESSION_DATA || op->kind == LIVE_REST_PATCH_SETTINGS_PROTO) {
      push_expression_data_failed(l, op, hs);
    } else if (op->kind == LIVE_REST_STATUS_GET || op->kind == LIVE_REST_STATUS_PATCH) {
      live_status_failed(l, op);
    } else if (op->kind == LIVE_REST_MEDIA) {
      push_media_failed(l, op->media_url, hs);
    } else if (op->kind == LIVE_REST_ADMIN && op->admin_op == CCORD_ADMIN_GET_GUILD_PROFILE) {
      push_guild_profile_unavailable(l, op->guild_id);
    } else {
      push_action_failed(l, op, hs, http_status, body, len);
    }
    if (op->kind == LIVE_REST_MEDIA) live_log_media_failure(op->media_url, CCORD_OK, http_status);
    else CCORD_LOG_W("live: %s failed: %s", live_rest_name(op->kind), err.message);
    live_rest_op_free(op);
    return;
  }
  switch (op->kind) {
    case LIVE_REST_HISTORY:
    case LIVE_REST_HISTORY_BEFORE: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_history(body, len, op->channel_id, &ev, &err) == CCORD_OK) {
        if (op->cache_path) live_api_cache_write(op->cache_path, body, len);
        if (op->kind == LIVE_REST_HISTORY) live_warm_history_write(l, op->channel_id, body, len);
        (void)live_push(l, &ev);
      } else {
        if (op->kind == LIVE_REST_HISTORY) {
          push_history_error(l, op->channel_id, CCORD_ERR_PARSE, err.message);
        } else {
          push_history_failed(l, op->channel_id, CCORD_ERR_PARSE, err.message);
        }
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: %s parse failed: %s", live_rest_name(op->kind), err.message);
      }
      break;
    }
    case LIVE_REST_PINS:
    case LIVE_REST_PINS_BEFORE: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      bool append = op->kind == LIVE_REST_PINS_BEFORE;
      if (ccord_live_json_pins(body, len, op->channel_id, append, &ev, &err) == CCORD_OK) {
        if (!append && op->cache_path) live_api_cache_write(op->cache_path, body, len);
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: %s parse failed: %s", live_rest_name(op->kind), err.message);
      }
      break;
    }
    case LIVE_REST_USER_PROFILE: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_user_profile(body, len, op->user_id, &ev, &err) == CCORD_OK) {
        if (op->cache_path) live_api_cache_write(op->cache_path, body, len);
        live_enrich_user_profile(l, ev.user_profile, body, len, true);
        (void)live_push(l, &ev);
      } else {
        push_profile_error(l, op->user_id, CCORD_ERR_PARSE, err.message);
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: user profile parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_PROFILE_EFFECTS: {
      l->effect_catalog_inflight = false;
      l->effect_catalog_loaded = true;
      ccord_profile_effect_config *items = nullptr;
      size_t count = 0;
      if (ccord_live_json_profile_effects(body, len, &items, &count, &err) == CCORD_OK) {
        if (op->cache_path) live_api_cache_write(op->cache_path, body, len);
        for (size_t i = 0; i < count; i++) live_upsert_effect_config(l, &items[i]);
        ccord_free(items);
      } else {
        CCORD_LOG_W("live: profile effects parse failed: %s", err.message);
      }
      live_flush_pending_decor_profiles(l);
      break;
    }
    case LIVE_REST_COLLECTIBLE_PRODUCT: {
      if (l->product_inflight > 0) l->product_inflight--;
      ccord_profile_effect_config eff;
      ccord_profile_frame_config frm;
      bool has_eff = false, has_frm = false;
      if (ccord_live_json_collectible_product(body, len, op->expr_id, &eff, &has_eff, &frm, &has_frm, &err) == CCORD_OK) {
        if (op->cache_path) live_api_cache_write(op->cache_path, body, len);
        if (has_eff) live_upsert_effect_config(l, &eff);
        if (has_frm) live_upsert_frame_config(l, &frm);
      } else {
        CCORD_LOG_W("live: collectible product parse failed: %s", err.message);
      }
      live_flush_pending_decor_profiles(l);
      break;
    }
    case LIVE_REST_ACK:
      if (op->message_id != CCORD_ID_NONE) live_chan_set_ack(l, op->channel_id, op->message_id);
      break;
    case LIVE_REST_MARK_UNREAD:
      if (op->expr_id != CCORD_ID_NONE) live_chan_set_ack(l, op->channel_id, op->expr_id);
      break;
    case LIVE_REST_POLL_VOTE:
    case LIVE_REST_INTERACTION:
    case LIVE_REST_INTERACTION_AUTOCOMPLETE:
      break;
    case LIVE_REST_SLASH_COMMANDS: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_slash_commands(body, len, op->guild_id, op->channel_id, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: slash commands parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_DISCOVERY: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_discoverable_guilds(body, len, op->discovery_query, op->discovery_category_id,
                                              op->discovery_offset, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        push_discovery_error(l, CCORD_ERR_PARSE, err.message);
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: discovery parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_RELATIONSHIP: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_RELATIONSHIP_UPDATE;
      ev.ev.user_id = op->user_id;
      ev.ev.relationship_kind = op->rel_kind;
      (void)live_push(l, &ev);
      break;
    }
    case LIVE_REST_SEARCH: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_search(body, len, &ev, &err) == CCORD_OK) {
        ev.ev.channel_id = op->channel_id;
        ev.ev.search_offset = op->search_offset;
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: search parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_OPEN_DM: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_channel(body, len, CCORD_DM_GUILD_ID, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: open dm parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_CHANNEL: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_channel(body, len, CCORD_ID_NONE, &ev, &err) == CCORD_OK) {
        if (op->cache_path) live_api_cache_write(op->cache_path, body, len);
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: channel lookup parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_MEDIA_RANGE:   // answered at the top of live_rest_done
      break;
    case LIVE_REST_MEDIA: {
      if (op->media_stage == 1u) {   // the refresh-urls answer, not a picture
        if (live_media_refreshed(l, op, body, len)) return;   // the op carries on as the GET of the fresh URL
        live_log_media_failure(op->media_url, CCORD_ERR_PERMISSION, http_status);
        push_media_failed(l, op->media_url, CCORD_ERR_PERMISSION);
        break;
      }
      if (!body || len == 0) {
        live_log_media_failure(op->media_url, CCORD_ERR_NOT_FOUND, http_status);   // an HTTP 2xx with no body
        push_media_failed(l, op->media_url, CCORD_ERR_NOT_FOUND);   // an empty body is not an image
        break;
      }
      if (op->media_path && op->media_path[0] != '\0' && body && len > 0) {
        live_ensure_live_cache_dirs(op->media_path);
        ccord_error werr;
        if (ccord_file_write_atomic(op->media_path, body, len, 0600, false, &werr) != CCORD_OK) {
          CCORD_LOG_W("live: media cache write failed: %s", werr.message);
        }
      }
      size_t taken_len = 0;
      char *owned = l->restq ? ccord_rest_take_active_resp(l->restq, &taken_len) : nullptr;
      if (owned && taken_len == len) {
        (void)live_push_media_event_owned(l, op->media_url, op->media_path, owned, taken_len);
      } else {
        ccord_free(owned);
        (void)push_media_event(l, op->media_url, op->media_path, body, len);
      }
      break;
    }
    case LIVE_REST_UPLOAD_SLOT: {
      live_upload_state *u = op->upload;
      if (!u || ccord_live_json_upload_slots(body, len, u->file_count, u->upload_urls,
                                             u->upload_filenames, &err) != CCORD_OK) {
        CCORD_LOG_W("live: upload slot parse failed: %s", err.message);
        push_action_failed(l, op, CCORD_ERR_PARSE, http_status, body, len);
        break;
      }
      op->kind = LIVE_REST_UPLOAD_PUT;
      u->upload_idx = 0;
      u->upload_completed_bytes = 0;
      u->upload_cur_file_bytes = 0;
      ccord_status pst = live_queue_add_raw(l, CCORD_REST_PUT, u->upload_urls[0],
                                            "application/octet-stream", u->file_datas[0], u->file_sizes[0],
                                            op, &err);
      bool trim0 = u->file_sizes[0] >= (1024u * 1024u);
      ccord_free(u->file_datas[0]);
      u->file_datas[0] = nullptr;
      u->file_sizes[0] = 0;
      if (trim0) ccord_os_trim_memory();
      if (pst == CCORD_OK) return; // op stays alive for next step
      CCORD_LOG_W("live: upload put enqueue failed: %s", err.message);
      push_action_failed(l, op, pst, 0, nullptr, 0);
      break;
    }
    case LIVE_REST_UPLOAD_PUT: {
      live_upload_state *u = op->upload;
      if (!u) {
        push_action_failed(l, op, CCORD_ERR_INTERNAL, 0, nullptr, 0);
        break;
      }
      if (u->upload_idx < u->file_count) {
        u->upload_completed_bytes += (uint64_t)u->upload_orig_sizes[u->upload_idx];
      }
      u->upload_cur_file_bytes = 0;
      u->upload_idx++;
      if (u->upload_idx < u->file_count) {
        size_t idx = u->upload_idx;
        ccord_status pst = live_queue_add_raw(l, CCORD_REST_PUT, u->upload_urls[idx],
                                              "application/octet-stream", u->file_datas[idx], u->file_sizes[idx],
                                              op, &err);
        bool trimi = u->file_sizes[idx] >= (1024u * 1024u);
        ccord_free(u->file_datas[idx]);
        u->file_datas[idx] = nullptr;
        u->file_sizes[idx] = 0;
        if (trimi) ccord_os_trim_memory();
        if (pst == CCORD_OK) return;
        CCORD_LOG_W("live: upload put [%zu] enqueue failed: %s", idx, err.message);
        push_action_failed(l, op, pst, 0, nullptr, 0);
        break;
      }
      u->upload_completed_bytes = u->upload_total_bytes;
      ccord_upload_file ufiles[CCORD_RICH_MAX_ATTACHMENTS];
      memset(ufiles, 0, sizeof ufiles);
      for (size_t i = 0; i < u->file_count; i++) {
        ufiles[i].filename = u->filenames[i] ? u->filenames[i] : "file.bin";
        ufiles[i].spoiler = u->file_spoilers[i];
        ufiles[i].duration_ms = u->file_durations_ms[i];
        ufiles[i].waveform_b64 = u->file_waveforms[i];
      }
      ccord_buf path, jbody;
      ccord_buf_init(&path, 0);
      ccord_buf_init(&jbody, 0);
      (void)ccord_buf_appendf(&path, "/channels/%llu/messages", (unsigned long long)op->channel_id);
      ccord_status bst = ccord_live_build_upload_message_body(op->text ? op->text : "", op->text_len,
                                                              op->message_id, op->flag, ufiles,
                                                              (const char (*)[256])u->upload_filenames,
                                                              u->file_count, &jbody, &err);
      if (bst == CCORD_OK && !path.failed) {
        op->kind = LIVE_REST_UPLOAD_SEND;
        ccord_status sst = live_queue_add(l, CCORD_REST_POST, ccord_buf_cstr(&path),
                                          ccord_buf_cstr(&jbody), op, &err);
        ccord_buf_free(&path);
        ccord_buf_free(&jbody);
        if (sst == CCORD_OK) return; // op stays alive for final step
        push_action_failed(l, op, sst, 0, nullptr, 0);
      } else {
        ccord_buf_free(&path);
        ccord_buf_free(&jbody);
        push_action_failed(l, op, bst != CCORD_OK ? bst : CCORD_ERR_RANGE, 0, nullptr, 0);
      }
      CCORD_LOG_W("live: upload message send enqueue failed");
      break;
    }
    case LIVE_REST_UPLOAD_SEND:
    case LIVE_REST_SEND_STICKER: {
      live_api_cache_invalidate_channel_history(l, op->channel_id);
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_message(body, len, CCORD_NET_MESSAGE_CREATE, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: %s parse failed: %s", live_rest_name(op->kind), err.message);
      }
      break;
    }
    case LIVE_REST_FORUM_POSTS: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_forum_posts(body, len, op->channel_id, op->forum_offset, &ev, &err) == CCORD_OK) {
        if (op->cache_path) live_api_cache_write(op->cache_path, body, len);
        (void)live_push(l, &ev);
      } else {
        push_forum_failed(l, op->channel_id, op->forum_offset, false, CCORD_ERR_PARSE, err.message);
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: forum posts parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_FORUM_CREATE: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_forum_post_created(body, len, op->channel_id, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        push_forum_failed(l, op->channel_id, 0, true, CCORD_ERR_PARSE, err.message);
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: forum post created parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_SEND_MESSAGE:
    case LIVE_REST_CREATE_POLL:
    case LIVE_REST_SEND_TTS:
    case LIVE_REST_FORWARD_MESSAGE:
    case LIVE_REST_SEND_REPLY:
    case LIVE_REST_EDIT_MESSAGE:
    case LIVE_REST_CROSSPOST_MESSAGE: {
      live_api_cache_invalidate_channel_history(l, op->channel_id);
      ccord_net_event_type type =
        (op->kind == LIVE_REST_EDIT_MESSAGE || op->kind == LIVE_REST_CROSSPOST_MESSAGE)
          ? CCORD_NET_MESSAGE_UPDATE
          : CCORD_NET_MESSAGE_CREATE;
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_message(body, len, type, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        // Delivered, but we could not read the echo; the gateway dispatch will bring the message in.
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: %s response parse failed: %s", live_rest_name(op->kind), err.message);
      }
      break;
    }
    case LIVE_REST_DELETE_MESSAGE: {
      live_api_cache_invalidate_channel_history(l, op->channel_id);
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_MESSAGE_DELETE;
      ev.ev.channel_id = op->channel_id;
      ev.ev.message_id = op->message_id;
      (void)live_push(l, &ev);
      break;
    }
    case LIVE_REST_TYPING: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_TYPING_START;
      ev.ev.channel_id = op->channel_id;
      ev.ev.user_id = l->self_id;
      ev.ev.timestamp_ms = (int64_t)live_now(l) + 10000;
      (void)live_push(l, &ev);
      break;
    }
    case LIVE_REST_REACTION: {
      // Re-read the message so the event carries the authoritative counter and breakdown.
      char mpath[96];
      (void)snprintf(mpath, sizeof mpath, "/channels/%llu/messages/%llu", (unsigned long long)op->channel_id,
                     (unsigned long long)op->message_id);
      bool queued = false;
      live_rest_op *rop = live_action_op(l, LIVE_REST_REACTION_REFRESH, op->channel_id, op->message_id, nullptr, 0);
      if (rop) {
        rop->flag = op->flag;
        rop->emoji = op->emoji ? ccord_strndup(op->emoji, strlen(op->emoji)) : nullptr;
        queued = (!op->emoji || rop->emoji) && live_queue_add(l, CCORD_REST_GET, mpath, nullptr, rop, &err) == CCORD_OK;
        if (!queued) live_rest_op_free(rop);
      }
      if (!queued) push_reaction_event(l, op, nullptr, 0);
      break;
    }
    case LIVE_REST_REACTION_REFRESH:
      push_reaction_event(l, op, body, len);
      break;
    case LIVE_REST_REACTION_USERS: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      if (ccord_live_json_reaction_users(body, len, op->channel_id, op->message_id, op->emoji, &ev, &err) == CCORD_OK) {
        (void)live_push(l, &ev);
      } else {
        ccord_live_event_free(&ev);
        CCORD_LOG_W("live: reaction users parse failed: %s", err.message);
      }
      break;
    }
    case LIVE_REST_PIN: {
      ccord_live_event ev;
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_MESSAGE_PIN_UPDATE;
      ev.ev.channel_id = op->channel_id;
      ev.ev.message_id = op->message_id;
      ev.ev.pinned = op->flag;
      (void)live_push(l, &ev);
      break;
    }
    case LIVE_REST_ADMIN:
      live_admin_done(l, op, body, len);
      break;
    case LIVE_REST_SOUNDBOARD:
      live_soundboard_done(l, op, body, len);
      break;
    case LIVE_REST_CALL_RING:   // 204: nothing to apply, the CALL_UPDATE dispatch carries the new ringing[]
      break;
    case LIVE_REST_REPORT_MENU:
    case LIVE_REST_REPORT:
      live_report_done(l, op, body, len);
      break;
    case LIVE_REST_EXPRESSION_DATA: {
      size_t alen = op->expr_arg ? strlen(op->expr_arg) : 0;
      live_push_expression_data_ok(l, op->expr_kind, op->expr_arg, alen, body, len, nullptr, 0, false, op->cache_path);
      break;
    }
    case LIVE_REST_PATCH_SETTINGS_PROTO: {
      live_push_expression_data_ok(l, CCORD_EXPR_SETTINGS_PROTO, nullptr, 0, body, len,
                              op->proto_data, op->proto_len, true, op->cache_path);
      break;
    }
    case LIVE_REST_STATUS_GET:
      live_status_get_done(l, body, len);
      break;
    case LIVE_REST_STATUS_PATCH:
      live_status_patch_done(l, body, len);
      break;
  }
  live_rest_op_free(op);
}


