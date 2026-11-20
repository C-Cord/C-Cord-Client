// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/json.h>

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/utf8.h>

#define JSON_WRITER_MAX_DEPTH 32u

// ---- writer ----
// first_bits bit d set = the next item at depth d needs no leading comma.

static void jw_put(ccord_jw *w, const char *s, size_t n)
{
  if (!w || w->failed || !w->out) return;
  if (ccord_buf_append(w->out, s, n) != CCORD_OK) w->failed = true;
}

static void jw_sep(ccord_jw *w)
{
  uint64_t bit = 1ull << w->depth;
  if (w->first_bits & bit) {
    w->first_bits &= ~bit;
  } else {
    jw_put(w, ",", 1);
  }
}

static void jw_begin(ccord_jw *w, char open)
{
  if (!w) return;
  jw_sep(w);
  jw_put(w, &open, 1);
  if (w->depth >= JSON_WRITER_MAX_DEPTH) {
    w->failed = true;
    return;
  }
  w->depth++;
  w->first_bits |= 1ull << w->depth;
}

static void jw_end(ccord_jw *w, char close)
{
  if (!w) return;
  if (w->depth == 0) {
    w->failed = true;
    return;
  }
  w->first_bits &= ~(1ull << w->depth);
  w->depth--;
  jw_put(w, &close, 1);
}

void ccord_jw_init(ccord_jw *w, ccord_buf *out)
{
  if (!w) return;
  w->out = out;
  w->depth = 0;
  w->first_bits = 1; // top level: first value needs no comma
  w->failed = (out == nullptr);
}

void ccord_jw_begin_object(ccord_jw *w) { jw_begin(w, '{'); }
void ccord_jw_end_object(ccord_jw *w) { jw_end(w, '}'); }
void ccord_jw_begin_array(ccord_jw *w) { jw_begin(w, '['); }
void ccord_jw_end_array(ccord_jw *w) { jw_end(w, ']'); }

void ccord_jw_string(ccord_jw *w, const char *s, size_t len)
{
  if (!w) return;
  if (!s) {
    if (len > 0) {
      w->failed = true;
      return;
    }
    s = "";
  }
  jw_sep(w);
  if (!ccord_utf8_valid(s, len)) {
    w->failed = true;
    return;
  }
  jw_put(w, "\"", 1);
  size_t span_start = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 0x20u && c != '"' && c != '\\') {
      continue;
    }
    if (i > span_start) {
      jw_put(w, s + span_start, i - span_start);
    }
    switch (c) {
    case '"': jw_put(w, "\\\"", 2); break;
    case '\\': jw_put(w, "\\\\", 2); break;
    case '\n': jw_put(w, "\\n", 2); break;
    case '\r': jw_put(w, "\\r", 2); break;
    case '\t': jw_put(w, "\\t", 2); break;
    case '\b': jw_put(w, "\\b", 2); break;
    case '\f': jw_put(w, "\\f", 2); break;
    default: {
      char e[7];
      snprintf(e, sizeof e, "\\u%04x", (unsigned)c);
      jw_put(w, e, 6);
      break;
    }
    }
    span_start = i + 1u;
  }
  if (len > span_start) {
    jw_put(w, s + span_start, len - span_start);
  }
  jw_put(w, "\"", 1);
}

void ccord_jw_key(ccord_jw *w, const char *key)
{
  if (!w) return;
  if (!key) {
    w->failed = true;
    return;
  }
  ccord_jw_string(w, key, strlen(key));
  jw_put(w, ":", 1);
  w->first_bits |= 1ull << w->depth; // the value that follows has no comma
}

void ccord_jw_cstr(ccord_jw *w, const char *s)
{
  if (!w) return;
  if (!s) {
    w->failed = true;
    return;
  }
  ccord_jw_string(w, s, strlen(s));
}

void ccord_jw_int(ccord_jw *w, int64_t v)
{
  if (!w) return;
  char tmp[32];
  snprintf(tmp, sizeof tmp, "%" PRId64, v);
  jw_sep(w);
  jw_put(w, tmp, strlen(tmp));
}

void ccord_jw_double(ccord_jw *w, double v)
{
  if (!w) return;
  if (!isfinite(v)) {
    w->failed = true;
    return;
  }
  char tmp[32];
  snprintf(tmp, sizeof tmp, "%.17g", v);
  jw_sep(w);
  jw_put(w, tmp, strlen(tmp));
}

void ccord_jw_bool(ccord_jw *w, bool v)
{
  if (!w) return;
  jw_sep(w);
  if (v) jw_put(w, "true", 4); else jw_put(w, "false", 5);
}

void ccord_jw_null(ccord_jw *w)
{
  if (!w) return;
  jw_sep(w);
  jw_put(w, "null", 4);
}

void ccord_jw_id(ccord_jw *w, uint64_t id)
{
  if (!w) return;
  char tmp[24];
  snprintf(tmp, sizeof tmp, "%" PRIu64, id);
  ccord_jw_string(w, tmp, strlen(tmp));
}

static void jw_copy_depth(ccord_jw *w, const ccord_json *v, unsigned depth)
{
  if (!w || w->failed) return;
  if (!v) {
    ccord_jw_null(w);
    return;
  }
  if (depth > JSON_WRITER_MAX_DEPTH) {
    w->failed = true;
    return;
  }
  switch (ccord_json_type_of(v)) {
  case CCORD_JSON_NULL:
    ccord_jw_null(w);
    break;
  case CCORD_JSON_BOOL:
    ccord_jw_bool(w, ccord_json_bool(v, false));
    break;
  case CCORD_JSON_NUMBER: {
    int64_t iv = 0;
    if (ccord_json_int64(v, &iv)) ccord_jw_int(w, iv);
    else ccord_jw_double(w, ccord_json_number(v, 0.0));
    break;
  }
  case CCORD_JSON_STRING: {
    size_t len = 0;
    const char *s = ccord_json_string(v, &len);
    ccord_jw_string(w, s ? s : "", len);
    break;
  }
  case CCORD_JSON_ARRAY: {
    ccord_jw_begin_array(w);
    size_t n = ccord_json_array_len(v);
    for (size_t i = 0; i < n && !w->failed; i++) {
      jw_copy_depth(w, ccord_json_array_at(v, i), depth + 1u);
    }
    ccord_jw_end_array(w);
    break;
  }
  case CCORD_JSON_OBJECT: {
    ccord_jw_begin_object(w);
    size_t n = ccord_json_object_len(v);
    for (size_t i = 0; i < n && !w->failed; i++) {
      const ccord_json *val = nullptr;
      const char *k = ccord_json_object_key(v, i, &val);
      if (k) {
        ccord_jw_key(w, k);
        jw_copy_depth(w, val, depth + 1u);
      }
    }
    ccord_jw_end_object(w);
    break;
  }
  }
}

void ccord_jw_copy_json(ccord_jw *w, const ccord_json *v)
{
  jw_copy_depth(w, v, 0);
}

bool ccord_jw_ok(const ccord_jw *w)
{
  return w && w->out && !w->failed && !w->out->failed && w->depth == 0;
}
