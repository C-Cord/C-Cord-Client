// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/rpc/activity.h>

#include <ccord/core/buf.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define A_TEXT_CAP (CCORD_RPC_ACTIVITY_TEXT_MAX + 1u)

static const char k_default_name[] = "Rich Presence";

// Copies a string, truncating to the field capacity on a UTF-8 boundary so the writer never sees a split sequence.
static void copy_text_n(char dst[static A_TEXT_CAP], const char *src, size_t n)
{
  dst[0] = '\0';
  if (!src) return;
  if (n > CCORD_RPC_ACTIVITY_TEXT_MAX) {
    n = CCORD_RPC_ACTIVITY_TEXT_MAX;
    // The first dropped byte may be a continuation byte; back up to the start of its sequence and drop it whole.
    while (n > 0 && (((unsigned char)src[n]) & 0xc0u) == 0x80u) n--;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static ccord_status text_field(const ccord_json *root, const char *key, char dst[static A_TEXT_CAP], ccord_error *err)
{
  const ccord_json *v = ccord_json_get(root, key);
  if (!v) return CCORD_OK;
  size_t len = 0;
  const char *s = ccord_json_string(v, &len);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: %s must be a string", key);
  copy_text_n(dst, s, len);
  return CCORD_OK;
}

static ccord_status object_field(const ccord_json *root, const char *key, const ccord_json **out, ccord_error *err)
{
  *out = ccord_json_get(root, key);
  if (*out && ccord_json_type_of(*out) != CCORD_JSON_OBJECT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: %s must be an object", key);
  }
  return CCORD_OK;
}

static ccord_status timestamp_field(const ccord_json *root, const char *key, int64_t *out, ccord_error *err)
{
  const ccord_json *v = ccord_json_get(root, key);
  *out = 0;
  if (!v) return CCORD_OK;
  if (ccord_json_type_of(v) != CCORD_JSON_NUMBER) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: timestamp %s must be a number", key);
  }
  double d = ccord_json_number(v, 0);
  if (!(d > 0)) return CCORD_OK;
  if (d < 1e11) d *= 1000.0;  // Discord RPC historically sends seconds; gateway expects milliseconds.
  // 2^63 is exactly representable as a double but not as an int64_t; reject at the bound before casting.
  if (d >= 9223372036854775808.0) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "rpc: timestamp %s out of range", key);
  }
  *out = (int64_t)d;
  return CCORD_OK;
}

static bool is_http_url(const char *url)
{
  return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

static ccord_status parse_buttons(const ccord_json *root, ccord_rpc_activity *out, ccord_error *err)
{
  const ccord_json *buttons = ccord_json_get(root, "buttons");
  if (!buttons) return CCORD_OK;
  if (ccord_json_type_of(buttons) != CCORD_JSON_ARRAY) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: buttons must be an array");
  }
  size_t n = ccord_json_array_len(buttons);
  if (n > CCORD_RPC_ACTIVITY_BUTTONS_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "rpc: at most %u buttons", (unsigned)CCORD_RPC_ACTIVITY_BUTTONS_MAX);
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_json *b = ccord_json_array_at(buttons, i);
    if (!b || ccord_json_type_of(b) != CCORD_JSON_OBJECT) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: button must be an object");
    }
    const ccord_json *label_json = ccord_json_get(b, "label");
    const ccord_json *url_json = ccord_json_get(b, "url");
    if (!label_json || ccord_json_type_of(label_json) != CCORD_JSON_STRING) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: button label must be a string");
    }
    if (!url_json || ccord_json_type_of(url_json) != CCORD_JSON_STRING) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: button url must be a string");
    }
    size_t llen = 0, ulen = 0;
    const char *label = ccord_json_string(label_json, &llen);
    const char *url = ccord_json_string(url_json, &ulen);
    if (!is_http_url(url)) return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: button url must be http or https");
    copy_text_n(out->button_labels[i], label, llen);
    copy_text_n(out->button_urls[i], url, ulen);
    out->button_count = i + 1u;
  }
  return CCORD_OK;
}

static ccord_status parse_party(const ccord_json *root, ccord_rpc_activity *out, ccord_error *err)
{
  const ccord_json *party = nullptr;
  ccord_status st = object_field(root, "party", &party, err);
  if (st != CCORD_OK || !party) return st;
  st = text_field(party, "id", out->party_id, err);
  if (st != CCORD_OK) return st;
  const ccord_json *size = ccord_json_get(party, "size");
  if (!size) return CCORD_OK;
  if (ccord_json_type_of(size) != CCORD_JSON_ARRAY || ccord_json_array_len(size) != 2) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: party size must be [current, max]");
  }
  int64_t cur = 0, max = 0;
  if (!ccord_json_int64(ccord_json_array_at(size, 0), &cur) || !ccord_json_int64(ccord_json_array_at(size, 1), &max) ||
      cur < 0 || max < 0 || cur > INT_MAX || max > INT_MAX) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: party size must be non-negative integers");
  }
  out->party_size_cur = (int)cur;
  out->party_size_max = (int)max;
  return CCORD_OK;
}

static ccord_status parse_timestamps(const ccord_json *root, ccord_rpc_activity *out, ccord_error *err)
{
  const ccord_json *timestamps = nullptr;
  ccord_status st = object_field(root, "timestamps", &timestamps, err);
  if (st != CCORD_OK || !timestamps) return st;
  st = timestamp_field(timestamps, "start", &out->start_ms, err);
  if (st != CCORD_OK) return st;
  return timestamp_field(timestamps, "end", &out->end_ms, err);
}

static ccord_status parse_assets(const ccord_json *root, ccord_rpc_activity *out, ccord_error *err)
{
  const ccord_json *assets = nullptr;
  ccord_status st = object_field(root, "assets", &assets, err);
  if (st != CCORD_OK || !assets) return st;
  if ((st = text_field(assets, "large_image", out->large_image, err)) != CCORD_OK ||
      (st = text_field(assets, "large_text", out->large_text, err)) != CCORD_OK ||
      (st = text_field(assets, "small_image", out->small_image, err)) != CCORD_OK) {
    return st;
  }
  return text_field(assets, "small_text", out->small_text, err);
}

static ccord_status parse_secrets(const ccord_json *root, ccord_rpc_activity *out, ccord_error *err)
{
  const ccord_json *secrets = nullptr;
  ccord_status st = object_field(root, "secrets", &secrets, err);
  if (st != CCORD_OK || !secrets) return st;
  if ((st = text_field(secrets, "join", out->secret_join, err)) != CCORD_OK ||
      (st = text_field(secrets, "spectate", out->secret_spectate, err)) != CCORD_OK) {
    return st;
  }
  return text_field(secrets, "match", out->secret_match, err);
}

static ccord_status parse_application_id(const ccord_json *root, ccord_rpc_activity *out, ccord_error *err)
{
  const ccord_json *v = ccord_json_get(root, "application_id");
  if (!v) return CCORD_OK;
  size_t len = 0;
  const char *s = ccord_json_string(v, &len);
  if (s) {
    copy_text_n(out->application_id, s, len);
    return CCORD_OK;
  }
  int64_t id = 0;
  if (!ccord_json_int64(v, &id) || id < 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: application_id must be a snowflake");
  }
  (void)snprintf(out->application_id, A_TEXT_CAP, "%lld", (long long)id);
  return CCORD_OK;
}

ccord_status ccord_rpc_activity_parse(const ccord_json *json_value, ccord_rpc_activity *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: missing activity output");
  memset(out, 0, sizeof *out);
  if (!json_value) return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: missing activity");
  if (ccord_json_type_of(json_value) != CCORD_JSON_OBJECT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: activity must be an object");
  }
  ccord_status st = text_field(json_value, "name", out->name, err);
  if (st != CCORD_OK) return st;
  // A payload without a name keeps name empty: the app resolves the real one from the client's application id.
  // Only the reply echo to the client (ccord_rpc_activity_to_gateway_json) falls back to k_default_name.

  const ccord_json *type = ccord_json_get(json_value, "type");
  if (type) {
    int64_t v = 0;
    if (!ccord_json_int64(type, &v) || !(v == 0 || v == 1 || v == 2 || v == 3 || v == 5)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: activity type must be 0, 1, 2, 3 or 5");
    }
    out->type = (int)v;
  }

  if ((st = parse_application_id(json_value, out, err)) != CCORD_OK ||
      (st = text_field(json_value, "details", out->details, err)) != CCORD_OK ||
      (st = text_field(json_value, "state", out->state, err)) != CCORD_OK ||
      (st = text_field(json_value, "url", out->url, err)) != CCORD_OK ||
      (st = parse_timestamps(json_value, out, err)) != CCORD_OK ||
      (st = parse_assets(json_value, out, err)) != CCORD_OK ||
      (st = parse_party(json_value, out, err)) != CCORD_OK ||
      (st = parse_secrets(json_value, out, err)) != CCORD_OK ||
      (st = parse_buttons(json_value, out, err)) != CCORD_OK) {
    return st;
  }
  if (out->url[0] && !is_http_url(out->url)) {
    out->url[0] = '\0';
  }

  const ccord_json *instance = ccord_json_get(json_value, "instance");
  if (instance) {
    if (ccord_json_type_of(instance) != CCORD_JSON_BOOL) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "rpc: instance must be a bool");
    }
    out->instance = ccord_json_bool(instance, false);
  }
  return CCORD_OK;
}

static void jw_text(ccord_jw *w, const char *key, const char *s)
{
  if (!s || s[0] == '\0') return;
  ccord_jw_key(w, key);
  ccord_jw_string(w, s, strlen(s));
}

static bool has_assets(const ccord_rpc_activity *a)
{
  return a->large_image[0] || a->large_text[0] || a->small_image[0] || a->small_text[0];
}

static bool has_party(const ccord_rpc_activity *a)
{
  return a->party_id[0] || a->party_size_cur > 0 || a->party_size_max > 0;
}

static bool has_secrets(const ccord_rpc_activity *a)
{
  return a->secret_join[0] || a->secret_spectate[0] || a->secret_match[0];
}

static void jw_activity(ccord_jw *w, const ccord_rpc_activity *a)
{
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  if (a->name[0]) ccord_jw_string(w, a->name, strlen(a->name));
  else ccord_jw_string(w, k_default_name, sizeof k_default_name - 1u);
  ccord_jw_key(w, "type");
  ccord_jw_int(w, a->type);
  jw_text(w, "application_id", a->application_id);
  jw_text(w, "details", a->details);
  jw_text(w, "state", a->state);
  jw_text(w, "url", a->url);
  if (a->start_ms > 0 || a->end_ms > 0) {
    ccord_jw_key(w, "timestamps");
    ccord_jw_begin_object(w);
    if (a->start_ms > 0) {
      ccord_jw_key(w, "start");
      ccord_jw_int(w, a->start_ms);
    }
    if (a->end_ms > 0) {
      ccord_jw_key(w, "end");
      ccord_jw_int(w, a->end_ms);
    }
    ccord_jw_end_object(w);
  }
  if (has_assets(a)) {
    ccord_jw_key(w, "assets");
    ccord_jw_begin_object(w);
    jw_text(w, "large_image", a->large_image);
    jw_text(w, "large_text", a->large_text);
    jw_text(w, "small_image", a->small_image);
    jw_text(w, "small_text", a->small_text);
    ccord_jw_end_object(w);
  }
  if (has_party(a)) {
    ccord_jw_key(w, "party");
    ccord_jw_begin_object(w);
    jw_text(w, "id", a->party_id);
    if (a->party_size_cur > 0 || a->party_size_max > 0) {
      ccord_jw_key(w, "size");
      ccord_jw_begin_array(w);
      ccord_jw_int(w, a->party_size_cur);
      ccord_jw_int(w, a->party_size_max);
      ccord_jw_end_array(w);
    }
    ccord_jw_end_object(w);
  }
  if (has_secrets(a)) {
    ccord_jw_key(w, "secrets");
    ccord_jw_begin_object(w);
    jw_text(w, "join", a->secret_join);
    jw_text(w, "spectate", a->secret_spectate);
    jw_text(w, "match", a->secret_match);
    ccord_jw_end_object(w);
  }
  size_t buttons = a->button_count > CCORD_RPC_ACTIVITY_BUTTONS_MAX ? CCORD_RPC_ACTIVITY_BUTTONS_MAX : a->button_count;
  if (buttons > 0) {
    ccord_jw_key(w, "buttons");
    ccord_jw_begin_array(w);
    for (size_t i = 0; i < buttons; i++) ccord_jw_cstr(w, a->button_labels[i]);
    ccord_jw_end_array(w);
    ccord_jw_key(w, "metadata");
    ccord_jw_begin_object(w);
    ccord_jw_key(w, "button_urls");
    ccord_jw_begin_array(w);
    for (size_t i = 0; i < buttons; i++) ccord_jw_cstr(w, a->button_urls[i]);
    ccord_jw_end_array(w);
    ccord_jw_end_object(w);
  }
  ccord_jw_key(w, "instance");
  ccord_jw_bool(w, a->instance);
  ccord_jw_end_object(w);
}

ccord_status ccord_rpc_activity_to_gateway_json(const ccord_rpc_activity *acts, size_t n, char *out, size_t cap)
{
  if (!out || cap == 0) return CCORD_ERR_INVALID;
  out[0] = '\0';
  if (n > 0 && !acts) return CCORD_ERR_INVALID;
  if (n == 0) {
    if (cap < 3u) return CCORD_ERR_RANGE;
    memcpy(out, "[]", 3u);
    return CCORD_OK;
  }
  if (n > CCORD_RPC_ACTIVITY_MAX) n = CCORD_RPC_ACTIVITY_MAX;
  ccord_buf b;
  ccord_buf_init(&b, cap);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < n; i++) jw_activity(&w, &acts[i]);
  ccord_jw_end_array(&w);
  ccord_status st = CCORD_OK;
  if (!ccord_jw_ok(&w) || b.failed || b.len + 1u > cap) {
    st = CCORD_ERR_RANGE;
  } else {
    memcpy(out, b.data, b.len);
    out[b.len] = '\0';
  }
  ccord_buf_free(&b);
  return st;
}

static uint32_t rd_le32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le32(unsigned char *p, uint32_t v)
{
  p[0] = (unsigned char)(v & 0xffu);
  p[1] = (unsigned char)((v >> 8) & 0xffu);
  p[2] = (unsigned char)((v >> 16) & 0xffu);
  p[3] = (unsigned char)((v >> 24) & 0xffu);
}

ccord_status ccord_rpc_frame_encode(char *out, size_t cap, uint32_t op, const char *json, size_t len)
{
  if (!out || (len > 0 && !json)) return CCORD_ERR_INVALID;
  if (op > CCORD_RPC_FRAME_MAX_OP || len > CCORD_RPC_FRAME_MAX_PAYLOAD) return CCORD_ERR_RANGE;
  if (cap < 8u + len) return CCORD_ERR_RANGE;
  wr_le32((unsigned char *)out, op);
  wr_le32((unsigned char *)out + 4, (uint32_t)len);
  if (len > 0) memcpy(out + 8, json, len);
  return CCORD_OK;
}

ccord_rpc_frame_result ccord_rpc_frame_decode(const char *buf, size_t len, uint32_t *op, const char **payload,
                                              size_t *plen, size_t *consumed)
{
  if (op) *op = 0;
  if (payload) *payload = nullptr;
  if (plen) *plen = 0;
  if (consumed) *consumed = 0;
  if (!buf || len < 8) return CCORD_RPC_FRAME_NEED_MORE;
  uint32_t frame_op = rd_le32((const unsigned char *)buf);
  uint32_t frame_len = rd_le32((const unsigned char *)buf + 4);
  if (frame_op > CCORD_RPC_FRAME_MAX_OP || frame_len > CCORD_RPC_FRAME_MAX_PAYLOAD) return CCORD_RPC_FRAME_BAD;
  if (len - 8u < frame_len) return CCORD_RPC_FRAME_NEED_MORE;
  if (op) *op = frame_op;
  if (payload) *payload = buf + 8;
  if (plen) *plen = frame_len;
  if (consumed) *consumed = 8u + frame_len;
  return CCORD_RPC_FRAME_FRAME;
}
