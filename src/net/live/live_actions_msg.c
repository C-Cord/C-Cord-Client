// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "live_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

ccord_status live_request_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  if (limit == 0) limit = CCORD_LIVE_HISTORY_DEFAULT;
  if (limit > 100) limit = 100;

  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/messages?limit=%u", (unsigned long long)channel_id,
                        (unsigned)limit) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  } else {
    char cpath[4096] = {0};
    if (live_api_cache_path(l, ccord_buf_cstr(&path), cpath, sizeof cpath)) {
      // Stale-while-revalidate: deliver the cached page at once (no TTL), then always ask the network too.
      size_t clen = 0;
      char *cbody = live_api_cache_read(cpath, 0u, &clen);
      if (cbody) {
        ccord_live_event ev;
        ccord_error cerr;
        if (ccord_live_json_history(cbody, clen, channel_id, &ev, &cerr) == CCORD_OK) {
          (void)live_push(l, &ev);
        } else {
          ccord_live_event_free(&ev);
        }
        ccord_free(cbody);
      }
    }
    st = live_enqueue_rest_cached(l, CCORD_REST_GET, ccord_buf_cstr(&path), LIVE_REST_HISTORY, channel_id,
                                  cpath[0] ? cpath : nullptr, err);
  }
  ccord_buf_free(&path);
  return st;
}

ccord_status live_request_history_before(ccord_transport *t, ccord_id channel_id, ccord_id before_id,
                                                uint32_t limit, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE || before_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: history ids");
  }
  if (limit == 0) limit = CCORD_LIVE_HISTORY_DEFAULT;
  if (limit > 100) limit = 100;
  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/messages?limit=%u&before=%llu", (unsigned long long)channel_id,
                        (unsigned)limit, (unsigned long long)before_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  } else {
    st = live_enqueue_rest(l, CCORD_REST_GET, ccord_buf_cstr(&path), LIVE_REST_HISTORY_BEFORE, channel_id, err);
  }
  ccord_buf_free(&path);
  return st;
}

ccord_status live_request_pins(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  if (limit == 0) limit = CCORD_LIVE_HISTORY_DEFAULT;
  if (limit > 50) limit = 50;
  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/pins?limit=%u", (unsigned long long)channel_id,
                        (unsigned)limit) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  } else {
    char cpath[4096] = {0};
    if (live_api_cache_path(l, ccord_buf_cstr(&path), cpath, sizeof cpath)) {
      size_t clen = 0;
      char *cbody = live_api_cache_read(cpath, 600u, &clen);
      if (cbody) {
        ccord_live_event ev;
        if (ccord_live_json_pins(cbody, clen, channel_id, false, &ev, err) == CCORD_OK) {
          (void)live_push(l, &ev);
          ccord_free(cbody);
          ccord_buf_free(&path);
          return CCORD_OK;
        }
        ccord_live_event_free(&ev);
        ccord_free(cbody);
      }
    }
    st = live_enqueue_rest_cached(l, CCORD_REST_GET, ccord_buf_cstr(&path), LIVE_REST_PINS, channel_id,
                                  cpath[0] ? cpath : nullptr, err);
  }
  ccord_buf_free(&path);
  return st;
}

ccord_status live_request_pins_before(ccord_transport *t, ccord_id channel_id, const char *before_cursor,
                                             uint32_t limit, ccord_error *err)
{
  if (!before_cursor || before_cursor[0] == '\0') {
    return live_request_pins(t, channel_id, limit, err);
  }
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  size_t blen = strlen(before_cursor);
  if (!ccord_utf8_valid_text(before_cursor, blen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid pins cursor");
  }
  if (limit == 0) limit = CCORD_LIVE_HISTORY_DEFAULT;
  if (limit > 50) limit = 50;
  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/messages/pins?limit=%u&before=", (unsigned long long)channel_id,
                        (unsigned)limit) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  } else {
    st = live_append_encoded(&path, before_cursor, blen, err);
  }
  if (st == CCORD_OK) {
    st = live_enqueue_rest(l, CCORD_REST_GET, ccord_buf_cstr(&path), LIVE_REST_PINS_BEFORE, channel_id, err);
  }
  ccord_buf_free(&path);
  return st;
}

static ccord_status live_send_message_impl(live *l, ccord_id channel_id, const char *utf8, size_t len,
                                           const char *nonce, size_t nonce_len, ccord_error *err)
{
  ccord_buf body, path;
  ccord_buf_init(&body, LIVE_BODY_MAX);
  ccord_buf_init(&path, 512);
  ccord_status st = live_content_body(utf8, len, nonce, nonce_len, &body, err);
  if (st == CCORD_OK && ccord_buf_appendf(&path, "/channels/%llu/messages", (unsigned long long)channel_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_SEND_MESSAGE, channel_id, CCORD_ID_NONE, utf8, len);
    if (op && nonce && nonce_len > 0) {
      memcpy(op->nonce, nonce, nonce_len);
      op->nonce[nonce_len] = '\0';
    }
    st = op ? live_action_submit(l, CCORD_REST_POST, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err)
            : ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_buf_free(&body);
  ccord_buf_free(&path);
  return st;
}

ccord_status live_send_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                      ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  return live_send_message_impl(l, channel_id, utf8, len, nullptr, 0, err);
}

// M0-3: plain-text send with the session's nonce. The echo (or refusal) carries it back so the pending local
// echo can be confirmed or failed exactly.
ccord_status live_send_message_with_nonce(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                                 const char *nonce, size_t nonce_len, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  if (!nonce || nonce_len == 0 || nonce_len >= 32u ||   // 32 = the nonce buffer on live_rest_op / ccord_message
      memchr(nonce, 0, nonce_len) != nullptr) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: send nonce");
  }
  return live_send_message_impl(l, channel_id, utf8, len, nonce, nonce_len, err);
}

ccord_status live_send_tts_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                          ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  if (!utf8 || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty tts text");
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_INVALID, "live: text too long");
  if (memchr(utf8, 0, len) != nullptr) return ccord_error_set(err, CCORD_ERR_INVALID, "live: text has NUL");

  ccord_buf body, path;
  ccord_buf_init(&body, LIVE_BODY_MAX);
  ccord_buf_init(&path, 512);
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "content");
  ccord_jw_string(&w, utf8, len);
  ccord_jw_key(&w, "tts");
  ccord_jw_bool(&w, true);
  ccord_jw_end_object(&w);
  ccord_status st = CCORD_OK;
  if (!ccord_jw_ok(&w)) {
    st = ccord_error_set(err, body.failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "live: text is not valid UTF-8");
  }
  if (st == CCORD_OK && ccord_buf_appendf(&path, "/channels/%llu/messages", (unsigned long long)channel_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_SEND_TTS, channel_id, CCORD_ID_NONE, utf8, len);
    st = op ? live_action_submit(l, CCORD_REST_POST, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err)
            : ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_buf_free(&body);
  ccord_buf_free(&path);
  return st;
}

ccord_status live_create_poll(ccord_transport *t, ccord_id channel_id, const ccord_poll_create *poll,
                                     ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  ccord_buf body, path;
  ccord_buf_init(&body, LIVE_BODY_MAX);
  ccord_buf_init(&path, 128);
  ccord_status st = ccord_poll_build_body(poll, &body, err);
  if (st == CCORD_OK && ccord_buf_appendf(&path, "/channels/%llu/messages", (unsigned long long)channel_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_CREATE_POLL, channel_id, CCORD_ID_NONE, nullptr, 0);
    st = op ? live_action_submit(l, CCORD_REST_POST, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err)
            : ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_buf_free(&body);
  ccord_buf_free(&path);
  return st;
}

// The path comes only from ccord_report_type_name, never from caller text.
ccord_status live_request_report_menu(ccord_transport *t, ccord_report_type type, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  const char *name = ccord_report_type_name(type);
  if (!name) return ccord_error_set(err, CCORD_ERR_INVALID, "live: bad report type");
  char path[64];
  (void)snprintf(path, sizeof path, "/reporting/menu/%s", name);
  live_rest_op *op = live_action_op(l, LIVE_REST_REPORT_MENU, CCORD_ID_NONE, CCORD_ID_NONE, nullptr, 0);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->expr_arg = ccord_strndup(name, strlen(name));
  if (!op->expr_arg) {
    live_rest_op_free(op);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  return live_action_submit(l, CCORD_REST_GET, path, nullptr, op, err);
}

ccord_status live_submit_report(ccord_transport *t, const ccord_report_request *req, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!req) return ccord_error_set(err, CCORD_ERR_INVALID, "live: report request");
  const char *name = ccord_report_type_name(req->type);
  if (!name) return ccord_error_set(err, CCORD_ERR_INVALID, "live: bad report type");
  ccord_buf body;
  ccord_buf_init(&body, 1024);
  ccord_status st = ccord_report_build_body(req, &body, err);
  if (st == CCORD_OK) {
    char path[64];
    (void)snprintf(path, sizeof path, "/reporting/%s", name);
    live_rest_op *op = live_action_op(l, LIVE_REST_REPORT, req->channel_id, req->message_id, nullptr, 0);
    if (!op) {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    } else {
      op->user_id = req->reported_user_id;
      op->expr_arg = ccord_strndup(name, strlen(name));
      st = op->expr_arg ? live_action_submit(l, CCORD_REST_POST, path, ccord_buf_cstr(&body), op, err)
                        : (live_rest_op_free(op), ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory"));
    }
  }
  ccord_buf_free(&body);
  return st;
}

ccord_status live_forward_message(ccord_transport *t, ccord_id channel_id, ccord_id source_channel_id,
                                         ccord_id message_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || source_channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: forward ids");
  }
  ccord_id src_guild = live_chan_guild_find(l, source_channel_id);
  ccord_buf body, path;
  ccord_buf_init(&body, 256);
  ccord_buf_init(&path, 128);
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "message_reference");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "type");
  ccord_jw_int(&w, 1);
  ccord_jw_key(&w, "message_id");
  ccord_jw_id(&w, message_id);
  ccord_jw_key(&w, "channel_id");
  ccord_jw_id(&w, source_channel_id);
  if (src_guild != CCORD_ID_NONE && src_guild != CCORD_DM_GUILD_ID) {
    ccord_jw_key(&w, "guild_id");
    ccord_jw_id(&w, src_guild);
  }
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  ccord_status st = CCORD_OK;
  if (!ccord_jw_ok(&w) ||
      ccord_buf_appendf(&path, "/channels/%llu/messages", (unsigned long long)channel_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: forward payload failed");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_FORWARD_MESSAGE, channel_id, message_id, nullptr, 0);
    st = op ? live_action_submit(l, CCORD_REST_POST, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err)
            : ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_buf_free(&body);
  ccord_buf_free(&path);
  return st;
}

ccord_status live_crosspost_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                           ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: crosspost ids");
  }
  char path[160];
  (void)snprintf(path, sizeof path, "/channels/%llu/messages/%llu/crosspost", (unsigned long long)channel_id,
                 (unsigned long long)message_id);
  live_rest_op *op = live_action_op(l, LIVE_REST_CROSSPOST_MESSAGE, channel_id, message_id, nullptr, 0);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  return live_action_submit(l, CCORD_REST_POST, path, nullptr, op, err);
}

ccord_status live_edit_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                      const char *utf8, size_t len, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: message id");
  }
  ccord_buf body, path;
  ccord_buf_init(&body, LIVE_BODY_MAX);
  ccord_buf_init(&path, 512);
  ccord_status st = live_content_body(utf8, len, nullptr, 0, &body, err);
  if (st == CCORD_OK && ccord_buf_appendf(&path, "/channels/%llu/messages/%llu", (unsigned long long)channel_id,
                                          (unsigned long long)message_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_EDIT_MESSAGE, channel_id, message_id, utf8, len);
    st = op ? live_action_submit(l, CCORD_REST_PATCH, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err)
            : ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_buf_free(&body);
  ccord_buf_free(&path);
  return st;
}

ccord_status live_delete_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                        ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: message id");
  }
  char path[128];
  (void)snprintf(path, sizeof path, "/channels/%llu/messages/%llu", (unsigned long long)channel_id,
                 (unsigned long long)message_id);
  live_rest_op *op = live_action_op(l, LIVE_REST_DELETE_MESSAGE, channel_id, message_id, nullptr, 0);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  return live_action_submit(l, CCORD_REST_DELETE, path, nullptr, op, err);
}

ccord_status live_send_typing(ccord_transport *t, ccord_id channel_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  char path[96];
  (void)snprintf(path, sizeof path, "/channels/%llu/typing", (unsigned long long)channel_id);
  live_rest_op *op = live_action_op(l, LIVE_REST_TYPING, channel_id, CCORD_ID_NONE, nullptr, 0);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  return live_action_submit(l, CCORD_REST_POST, path, "{}", op, err);
}

static void strip_wire_emoji(const char *emoji, size_t len, const char **out_wire, size_t *out_len)
{
  const char *w = emoji;
  size_t wl = len;
  if (wl > 4 && w[0] == '<' && w[wl - 1] == '>') {
    if (w[1] == ':') {
      w += 2;
      wl -= 3;
    } else if ((w[1] == 'a' || w[1] == 'A') && w[2] == ':') {
      w += 3;
      wl -= 4;
    }
  }
  *out_wire = w;
  *out_len = wl;
}

ccord_status live_toggle_reaction(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                         const char *emoji, size_t len, bool add, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE || !emoji || len == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: reaction");
  }
  const char *wire_emoji = emoji;
  size_t wire_len = len;
  strip_wire_emoji(emoji, len, &wire_emoji, &wire_len);
  ccord_buf path;
  ccord_buf_init(&path, 1024);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/messages/%llu/reactions/", (unsigned long long)channel_id,
                        (unsigned long long)message_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) st = live_append_encoded(&path, wire_emoji, wire_len, err);
  if (st == CCORD_OK && ccord_buf_append_str(&path, "/@me") != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_REACTION, channel_id, message_id, nullptr, 0);
    if (op) {
      op->flag = add;
      op->emoji = ccord_strndup(wire_emoji, wire_len);
    }
    if (!op || !op->emoji) {
      live_rest_op_free(op);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    } else {
      st = live_action_submit(l, add ? CCORD_REST_PUT : CCORD_REST_DELETE, ccord_buf_cstr(&path), nullptr, op, err);
    }
  }
  ccord_buf_free(&path);
  return st;
}

ccord_status live_request_reaction_users(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                                const char *emoji, size_t emoji_len, uint32_t limit, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE || !emoji || emoji_len == 0 ||
      !ccord_utf8_valid_text(emoji, emoji_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: reaction users args");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  if (limit == 0) limit = 50u;
  if (limit > 100u) limit = 100u;
  const char *wire_emoji = emoji;
  size_t wire_len = emoji_len;
  strip_wire_emoji(emoji, emoji_len, &wire_emoji, &wire_len);
  ccord_buf path;
  ccord_buf_init(&path, 1024);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/messages/%llu/reactions/", (unsigned long long)channel_id,
                        (unsigned long long)message_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) st = live_append_encoded(&path, wire_emoji, wire_len, err);
  if (st == CCORD_OK && ccord_buf_appendf(&path, "?limit=%u", (unsigned)limit) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = ccord_calloc(1, sizeof *op);
    if (!op) {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    } else {
      op->l = l;
      op->kind = LIVE_REST_REACTION_USERS;
      op->channel_id = channel_id;
      op->message_id = message_id;
      op->emoji = ccord_strndup(wire_emoji, wire_len);
      if (!op->emoji) {
        live_rest_op_free(op);
        st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
      } else {
        st = live_queue_add(l, CCORD_REST_GET, ccord_buf_cstr(&path), nullptr, op, err);
        if (st != CCORD_OK) live_rest_op_free(op);
      }
    }
  }
  ccord_buf_free(&path);
  return st;
}

ccord_status live_ack_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: ack id");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  ccord_buf path;
  ccord_buf_init(&path, 512);
  ccord_status st = CCORD_OK;
  if (ccord_buf_appendf(&path, "/channels/%llu/messages/%llu/ack", (unsigned long long)channel_id,
                        (unsigned long long)message_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
    ccord_buf_free(&path);
    return st;
  }
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&path);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_ACK;
  op->channel_id = channel_id;
  op->message_id = message_id;
  st = live_queue_add(l, CCORD_REST_POST, ccord_buf_cstr(&path), "{\"token\":null}",
                      op, err);
  if (st != CCORD_OK) ccord_free(op);
  ccord_buf_free(&path);
  return st;
}

ccord_status live_mark_unread(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  ccord_id ack_id = CCORD_ID_NONE;
  if (!live_chan_prev_msg_id(l, channel_id, message_id, &ack_id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: mark unread id");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  char path[160];
  (void)snprintf(path, sizeof path, "/channels/%llu/messages/%llu/ack", (unsigned long long)channel_id,
                 (unsigned long long)ack_id);
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_MARK_UNREAD;
  op->channel_id = channel_id;
  op->message_id = message_id;
  op->expr_id = ack_id;
  ccord_status st = live_queue_add(l, CCORD_REST_POST, path, "{\"manual\":true,\"mention_count\":0}", op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}


ccord_status live_send_reply_ex(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id,
                                       bool mention_author, const char *utf8, size_t len, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || reply_to_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: reply ids");
  }
  ccord_buf body, path;
  ccord_buf_init(&body, 0);
  ccord_buf_init(&path, 512);
  ccord_status st = ccord_live_build_reply_body(utf8, len, reply_to_id, mention_author, &body, err);
  if (st == CCORD_OK && ccord_buf_appendf(&path, "/channels/%llu/messages", (unsigned long long)channel_id) != CCORD_OK) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
  }
  if (st == CCORD_OK) {
    live_rest_op *op = live_action_op(l, LIVE_REST_SEND_REPLY, channel_id, reply_to_id, utf8, len);
    if (op) {
      op->flag = mention_author;
      st = live_action_submit(l, CCORD_REST_POST, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err);
    } else {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    }
  }
  ccord_buf_free(&body);
  ccord_buf_free(&path);
  return st;
}

ccord_status live_send_reply(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id,
                                    const char *utf8, size_t len, ccord_error *err)
{
  return live_send_reply_ex(t, channel_id, reply_to_id, true, utf8, len, err);
}

ccord_status live_toggle_pin(ccord_transport *t, ccord_id channel_id, ccord_id message_id, bool pinned,
                                    ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: pin ids");
  }
  char path[128];
  (void)snprintf(path, sizeof path, "/channels/%llu/pins/%llu", (unsigned long long)channel_id,
                 (unsigned long long)message_id);
  live_rest_op *op = live_action_op(l, LIVE_REST_PIN, channel_id, message_id, nullptr, 0);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->flag = pinned;
  return live_action_submit(l, pinned ? CCORD_REST_PUT : CCORD_REST_DELETE, path, nullptr, op, err);
}


ccord_status live_send_message_with_files(ccord_transport *t, ccord_id channel_id,
                                                 ccord_id reply_to_id, bool mention_author,
                                                 const char *utf8, size_t len,
                                                 const ccord_upload_file *files, size_t file_count,
                                                 ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  if (!files || file_count == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "live: no upload files");
  if (file_count > CCORD_RICH_MAX_ATTACHMENTS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: too many upload files");
  }
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "live: text too long");
  if (len > 0 && (!utf8 || memchr(utf8, 0, len) != nullptr || !ccord_utf8_valid_text(utf8, len))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: text is not valid UTF-8");
  }
  for (size_t i = 0; i < file_count; i++) {
    if (!files[i].filename || files[i].filename[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty filename");
    }
    if (!files[i].data || files[i].size == 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty file data");
    }
    if ((uint64_t)files[i].size > CCORD_UPLOAD_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "live: file too large");
    }
    ccord_status vst = live_upload_voice_check(&files[i], err);
    if (vst != CCORD_OK) return vst;
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf path, body;
  ccord_buf_init(&path, 256);
  ccord_buf_init(&body, 0);
  ccord_status st = ccord_live_build_upload_slots_body(files, file_count, &body, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&path);
    ccord_buf_free(&body);
    return st;
  }
  (void)ccord_buf_appendf(&path, "/channels/%llu/attachments", (unsigned long long)channel_id);
  if (path.failed) {
    ccord_buf_free(&path);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: upload slot path too long");
  }

  live_rest_op *op = ccord_calloc(1, sizeof *op);
  live_upload_state *u = op ? ccord_calloc(1, sizeof *u) : nullptr;
  if (!op || !u) {
    ccord_free(u);
    ccord_free(op);
    ccord_buf_free(&path);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_UPLOAD_SLOT;
  op->channel_id = channel_id;
  op->message_id = reply_to_id;
  op->flag = mention_author;
  op->text = len > 0 ? ccord_strndup(utf8, len) : ccord_strndup("", 0);
  op->text_len = len;
  op->upload = u;
  u->file_count = file_count;
  if (!op->text) {
    live_rest_op_free(op);
    ccord_buf_free(&path);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  for (size_t i = 0; i < file_count; i++) {
    u->filenames[i] = ccord_strndup(files[i].filename, strlen(files[i].filename));
    u->file_datas[i] = ccord_alloc_array(files[i].size, 1);
    u->file_sizes[i] = files[i].size;
    u->upload_orig_sizes[i] = files[i].size;
    u->upload_total_bytes += (uint64_t)files[i].size;
    u->file_spoilers[i] = files[i].spoiler;
    u->file_durations_ms[i] = files[i].duration_ms;
    if (files[i].duration_ms != 0 && files[i].waveform_b64 && files[i].waveform_b64[0] != '\0') {
      u->file_waveforms[i] = ccord_strndup(files[i].waveform_b64, strlen(files[i].waveform_b64));
      if (!u->file_waveforms[i]) {
        live_rest_op_free(op);
        ccord_buf_free(&path);
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
      }
    }
    if (!u->filenames[i] || !u->file_datas[i]) {
      live_rest_op_free(op);
      ccord_buf_free(&path);
      ccord_buf_free(&body);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    }
    memcpy(u->file_datas[i], files[i].data, files[i].size);
  }

  st = live_queue_add(l, CCORD_REST_POST, ccord_buf_cstr(&path), ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&path);
  ccord_buf_free(&body);
  if (st != CCORD_OK) {
    live_rest_op_free(op);
    return st;
  }
  u->upload_id = ++l->next_upload_id;
  u->upload_prev = l->upload_tail;
  u->upload_next = nullptr;
  if (l->upload_tail) l->upload_tail->upload->upload_next = op;
  else l->upload_head = op;
  l->upload_tail = op;
  u->upload_linked = true;
  return CCORD_OK;
}

size_t live_active_uploads(ccord_transport *t, ccord_upload_progress *out, size_t max)
{
  live *l = (live *)t;
  if (!l) return 0;
  size_t total = 0;
  for (const live_rest_op *op = l->upload_head; op; op = op->upload->upload_next) {
    const live_upload_state *u = op->upload;
    if (out && total < max) {
      ccord_upload_progress *p = &out[total];
      memset(p, 0, sizeof *p);
      p->upload_id = u->upload_id;
      p->channel_id = op->channel_id;
      p->phase = op->kind == LIVE_REST_UPLOAD_SLOT  ? CCORD_UPLOAD_SLOT
                 : op->kind == LIVE_REST_UPLOAD_PUT ? CCORD_UPLOAD_PUT
                                                    : CCORD_UPLOAD_FINALIZE;
      p->file_count = (uint32_t)u->file_count;
      size_t fidx = u->upload_idx < u->file_count ? u->upload_idx
                    : (u->file_count > 0 ? u->file_count - 1u : 0u);
      p->file_index = (uint32_t)fidx;
      uint64_t done = u->upload_completed_bytes + u->upload_cur_file_bytes;
      if (done > u->upload_total_bytes) done = u->upload_total_bytes;
      p->uploaded_bytes = done;
      p->total_bytes = u->upload_total_bytes;
      const char *raw_fn = (fidx < CCORD_RICH_MAX_ATTACHMENTS && u->filenames[fidx]) ? u->filenames[fidx] : "file";
      const char *slash = strrchr(raw_fn, '/');
      const char *bslash = strrchr(raw_fn, '\\');
      const char *base = raw_fn;
      if (slash && slash + 1 > base) base = slash + 1;
      if (bslash && bslash + 1 > base) base = bslash + 1;
      bool add_sp = fidx < CCORD_RICH_MAX_ATTACHMENTS && u->file_spoilers[fidx] && strncmp(base, "SPOILER_", 8) != 0;
      (void)snprintf(p->filename, sizeof p->filename, "%s%s", add_sp ? "SPOILER_" : "", base[0] ? base : "file");
    }
    total++;
  }
  return total;
}

ccord_status live_send_message_with_file(ccord_transport *t, ccord_id channel_id,
                                                const char *utf8, size_t len,
                                                const char *filename, const void *file_data,
                                                size_t file_size, ccord_error *err)
{
  ccord_upload_file f = {
    .filename = filename,
    .data = file_data,
    .size = file_size,
    .spoiler = false,
  };
  return live_send_message_with_files(t, channel_id, CCORD_ID_NONE, true, utf8, len, &f, 1, err);
}

ccord_status live_request_slash_commands(ccord_transport *t, ccord_id guild_id,
                                                ccord_id channel_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: channel id");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  char path[128];
  (void)snprintf(path, sizeof path, "/channels/%llu/application-command-index", (unsigned long long)channel_id);
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_SLASH_COMMANDS;
  op->guild_id = guild_id;
  op->channel_id = channel_id;
  ccord_status st = live_queue_add(l, CCORD_REST_GET, path, nullptr, op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

// `user`: the download answers a click on a video, so any https host passes ccord_live_user_video_url_allowed;
// automatic loads keep the strict allow-list. Both queue the same raw job: no Authorization header, no redirects,
// TLS verified, 32 MiB cap (see discord_rest.c rest_job_prepare and live_queue_add_raw).
static ccord_status live_fetch_media_gated(ccord_transport *t, const char *url, bool user, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!url || url[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty media url");
  if (!(user ? ccord_live_user_video_url_allowed(url) : ccord_live_media_url_allowed(url))) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "live: media url blocked");
  }

  char cache_path[4096];
  ccord_status st = ccord_media_cache_path(url, cache_path, sizeof cache_path, err);
  if (st != CCORD_OK) return st;

  FILE *fp = fopen(cache_path, "rb");
  if (fp) {
    if (fseek(fp, 0, SEEK_END) == 0) {
      long sz = ftell(fp);
      if (sz > 0 && sz <= (long)CCORD_LIVE_MAX_MEDIA && fseek(fp, 0, SEEK_SET) == 0) {
        char *buf = ccord_alloc_array((size_t)sz, 1);
        if (buf) {
          size_t rd = fread(buf, 1, (size_t)sz, fp);
          fclose(fp);
          if (rd == (size_t)sz) {
            (void)live_push_media_event_owned(l, url, cache_path, buf, rd);
            return CCORD_OK;
          }
          ccord_free(buf);
        } else {
          fclose(fp);
        }
      } else {
        fclose(fp);
      }
    } else {
      fclose(fp);
    }
  }

  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_MEDIA;
  op->media_url = ccord_strndup(url, strlen(url));
  op->media_path = ccord_strndup(cache_path, strlen(cache_path));
  if (!op->media_url || !op->media_path) {
    live_rest_op_free(op);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  st = live_queue_add_raw(l, CCORD_REST_GET, url, nullptr, nullptr, 0, op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

ccord_status live_fetch_media(ccord_transport *t, const char *url, ccord_error *err)
{
  return live_fetch_media_gated(t, url, false, err);
}

ccord_status live_fetch_media_user(ccord_transport *t, const char *url, ccord_error *err)
{
  return live_fetch_media_gated(t, url, true, err);
}

// One byte range of a clicked video, for the streaming player. Same gate as the whole-file click path
// (ccord_live_user_video_url_allowed: https, a DNS host, port 443; no IP literal, localhost or other port); the
// queue repeats it and adds byte hygiene. Never touches the disk cache. The answer arrives as CCORD_NET_MEDIA_RANGE.
ccord_status live_fetch_media_range(ccord_transport *t, const ccord_media_range_req *req, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!req || !req->url || req->url[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty media url");
  if (!ccord_live_user_video_url_allowed(req->url)) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "live: media url blocked");
  }
  if (req->start < 0 || req->len < 1 || req->len > CCORD_STREAM_RANGE_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: bad media range");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_MEDIA_RANGE;
  op->range_id = req->id;
  op->range_start = req->start;
  op->range_len = req->len;
  op->media_url = ccord_strndup(req->url, strlen(req->url));
  if (!op->media_url) {
    live_rest_op_free(op);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  ccord_rest_request r = ccord_rest_request_make(CCORD_REST_GET, nullptr, live_kind_priority(op->kind));
  r.full_url = op->media_url;
  r.cb = live_rest_done;
  r.ud = op;
  r.stream_range = true;
  r.range_start = req->start;
  r.range_len = req->len;
  r.if_range = req->if_range;
  r.range_info = &op->range_info;
  ccord_status st = ccord_rest_queue_submit(l->restq, &r, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

ccord_status live_resolve_video(ccord_transport *t, uint64_t req_id, const char *url, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!url || url[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty video url");
  if (!ccord_video_resolve_url_safe(url, CCORD_VIDEO_RESOLVE_PAGE_URL_MAX)) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "live: video url blocked");
  }
  if (!l->vresolve) {
    l->vresolve = ccord_video_resolver_create();
    if (!l->vresolve) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  return ccord_video_resolver_submit(l->vresolve, req_id, url, err);
}


ccord_status live_search_messages_ex(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                            const ccord_search_query *query, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  ccord_buf path;
  ccord_buf_init(&path, 1024);
  ccord_status st = ccord_live_build_search_path(guild_id, channel_id, query, &path, err);
  if (st == CCORD_OK) {
    live_rest_op *op = ccord_calloc(1, sizeof *op);
    if (!op) {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    } else {
      op->l = l;
      op->kind = LIVE_REST_SEARCH;
      op->channel_id = query->channel_id != CCORD_ID_NONE ? query->channel_id : channel_id;
      op->search_offset = query->offset;
      st = live_queue_add(l, CCORD_REST_GET, ccord_buf_cstr(&path), nullptr, op, err);
      if (st != CCORD_OK) live_rest_op_free(op);
    }
  }
  ccord_buf_free(&path);
  return st;
}

ccord_status live_search_messages(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                         const char *query, size_t query_len, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!query || query_len == 0 || !ccord_utf8_valid_text(query, query_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid search query");
  }
  if (guild_id == CCORD_ID_NONE && channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: search requires guild or channel id");
  }
  ccord_buf path;
  ccord_buf_init(&path, 1024);
  ccord_status st = CCORD_OK;
  if (guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID) {
    (void)ccord_buf_appendf(&path, "/guilds/%llu/messages/search?content=", (unsigned long long)guild_id);
    st = live_append_encoded(&path, query, query_len, err);
    if (st == CCORD_OK && channel_id != CCORD_ID_NONE) {
      (void)ccord_buf_appendf(&path, "&channel_id=%llu", (unsigned long long)channel_id);
    }
  } else {
    (void)ccord_buf_appendf(&path, "/channels/%llu/messages/search?content=", (unsigned long long)channel_id);
    st = live_append_encoded(&path, query, query_len, err);
  }
  if (st == CCORD_OK && path.failed) st = ccord_error_set(err, CCORD_ERR_RANGE, "live: search path too long");
  if (st == CCORD_OK) {
    st = live_enqueue_rest(l, CCORD_REST_GET, ccord_buf_cstr(&path), LIVE_REST_SEARCH, channel_id, err);
  }
  ccord_buf_free(&path);
  return st;
}

ccord_status live_open_dm(ccord_transport *t, ccord_id recipient_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (recipient_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid recipient");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");

  ccord_buf body;
  ccord_buf_init(&body, 256);
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "recipient_id");
  ccord_jw_id(&w, recipient_id);
  ccord_jw_key(&w, "recipients");
  ccord_jw_begin_array(&w);
  ccord_jw_id(&w, recipient_id);
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: dm payload too large");
  }
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->l = l;
  op->kind = LIVE_REST_OPEN_DM;
  ccord_status st = live_queue_add(l, CCORD_REST_POST, "/users/@me/channels",
                                   ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&body);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

ccord_status live_request_channel(ccord_transport *t, ccord_id channel_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid channel");
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  char path[64];
  snprintf(path, sizeof path, "/channels/%llu", (unsigned long long)channel_id);
  char cpath[4096] = {0};
  if (live_api_cache_path(l, path, cpath, sizeof cpath)) {
    size_t clen = 0;
    char *cbody = live_api_cache_read(cpath, 86400u, &clen);
    if (cbody) {
      ccord_live_event ev;
      if (ccord_live_json_channel(cbody, clen, CCORD_ID_NONE, &ev, err) == CCORD_OK) {
        (void)live_push(l, &ev);
        ccord_free(cbody);
        return CCORD_OK;
      }
      ccord_free(cbody);
    }
  }
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = LIVE_REST_CHANNEL;
  op->channel_id = channel_id;
  if (cpath[0] != '\0') op->cache_path = ccord_strndup(cpath, strlen(cpath));
  ccord_status st = live_queue_add(l, CCORD_REST_GET, path, nullptr, op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}


ccord_status live_vote_poll(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                   const uint32_t *answer_ids, size_t answer_count, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid poll ids");
  }
  ccord_buf body;
  ccord_buf_init(&body, 0);
  ccord_status st = ccord_live_build_poll_vote_body(answer_ids, answer_count, &body, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&body);
    return st;
  }
  char path[160];
  (void)snprintf(path, sizeof path, "/channels/%llu/polls/%llu/answers/@me",
                 (unsigned long long)channel_id, (unsigned long long)message_id);
  live_rest_op *op = live_action_op(l, LIVE_REST_POLL_VOTE, channel_id, message_id, nullptr, 0);
  if (!op) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  st = live_action_submit(l, CCORD_REST_PUT, path, ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&body);
  return st;
}

ccord_status live_send_interaction(ccord_transport *t, const ccord_interaction_request *req, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!live_is_authenticated(l)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected");
  if (!req) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null interaction");

  uint64_t now = live_now(l);
  uint64_t epoch = UINT64_C(1420070400000);
  uint64_t nonce_id = ((now > epoch ? now - epoch : UINT64_C(1)) << 22) | (uint64_t)(++l->interaction_seq & 0x3FFFFFu);
  char nonce[24];
  ccord_id_format(nonce_id, nonce);
  const char *sid = l->gw_session_id[0] ? l->gw_session_id : (l->gw ? ccord_gateway_session_id(l->gw) : nullptr);

  ccord_buf body;
  ccord_buf_init(&body, 0);
  ccord_status st = ccord_live_build_interaction_body(req, sid, nonce, &body, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&body);
    return st;
  }
  live_rest_kind kind = req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE
                          ? LIVE_REST_INTERACTION_AUTOCOMPLETE
                          : LIVE_REST_INTERACTION;
  ccord_id op_msg = req->message_id != CCORD_ID_NONE
                      ? req->message_id
                      : (req->command_type == 3u ? req->target_id : CCORD_ID_NONE);
  live_rest_op *op = live_action_op(l, kind, req->channel_id, op_msg, nullptr, 0);
  if (!op) {
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->guild_id = req->guild_id;
  if (req->command_type == 2u) op->user_id = req->target_id;
  st = live_action_submit(l, CCORD_REST_POST, "/interactions", ccord_buf_cstr(&body), op, err);
  ccord_buf_free(&body);
  return st;
}


