// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/json.h>

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>

#define JSON_DEFAULT_DEPTH 16u
#define JSON_DEFAULT_NODES 8192u
#define JSON_DEFAULT_STRING (1024u * 1024u)
#define JSON_NUMBER_MAX 64u
#define JSON_WRITER_MAX_DEPTH 32u

struct ccord_json {
  ccord_json_type type;
  bool b;
  double num;
  const char *str;    // string value, or raw NUMBER text (NUL-terminated)
  size_t slen;
  ccord_json **items; // array elements or object values
  const char **keys;  // object keys
  size_t count;
};

typedef struct parser {
  const char *base, *p, *end;
  ccord_arena *a;
  size_t max_depth, max_nodes, max_string, nodes;
  ccord_error *err;
} parser;

static ccord_json *fail(parser *ps, const char *what)
{
  ccord_error_set(ps->err, CCORD_ERR_PARSE, "json: %s at byte %zu", what,
                  (size_t)(ps->p - ps->base));
  return nullptr;
}

static void skip_ws(parser *ps)
{
  while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
    ps->p++;
  }
}

static ccord_json *node_new(parser *ps, ccord_json_type t)
{
  if (++ps->nodes > ps->max_nodes) {
    ccord_error_set(ps->err, CCORD_ERR_RANGE, "json: too many nodes");
    return nullptr;
  }
  ccord_json *j = ccord_arena_alloc(ps->a, sizeof *j, alignof(ccord_json));
  if (!j) {
    ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory");
    return nullptr;
  }
  j->type = t;
  j->b = false;
  j->num = 0;
  j->str = nullptr;
  j->slen = 0;
  j->items = nullptr;
  j->keys = nullptr;
  j->count = 0;
  return j;
}

static int hexval(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Reads 4 hex digits at src[i..i+3]; returns -1 if malformed.
static long read_u4(const char *src, size_t span, size_t i)
{
  if (i + 4 > span) return -1;
  long v = 0;
  for (size_t k = 0; k < 4; k++) {
    int h = hexval(src[i + k]);
    if (h < 0) return -1;
    v = v * 16 + h;
  }
  return v;
}

// Parses a string token at ps->p (which points at the opening quote). On success the
// decoded, NUL-terminated copy is returned in the arena and *len is set.
static char *parse_string(parser *ps, size_t *len)
{
  const char *start = ps->p + 1;
  const char *s = start;
  while (true) {
    if (s >= ps->end) {
      ps->p = s;
      fail(ps, "unterminated string");
      return nullptr;
    }
    unsigned char c = (unsigned char)*s;
    if (c == '"') break;
    if (c < 0x20) {
      ps->p = s;
      fail(ps, "control character in string");
      return nullptr;
    }
    if (c == '\\') {
      s++;
      if (s >= ps->end) {
        ps->p = s;
        fail(ps, "bad escape");
        return nullptr;
      }
      s++;
      continue;
    }
    s++;
  }
  size_t span = (size_t)(s - start);
  if (span > ps->max_string) {
    ps->p = start;
    ccord_error_set(ps->err, CCORD_ERR_RANGE, "json: string too long");
    return nullptr;
  }
  char *out = ccord_arena_alloc(ps->a, span + 1, 1);
  if (!out) {
    ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory");
    return nullptr;
  }
  size_t o = 0;
  for (size_t i = 0; i < span; i++) {
    unsigned char c = (unsigned char)start[i];
    if (c != '\\') {
      out[o++] = (char)c;
      continue;
    }
    i++;
    char e = start[i];
    switch (e) {
    case '"': out[o++] = '"'; break;
    case '\\': out[o++] = '\\'; break;
    case '/': out[o++] = '/'; break;
    case 'b': out[o++] = '\b'; break;
    case 'f': out[o++] = '\f'; break;
    case 'n': out[o++] = '\n'; break;
    case 'r': out[o++] = '\r'; break;
    case 't': out[o++] = '\t'; break;
    case 'u': {
      long cp = read_u4(start, span, i + 1);
      if (cp < 0) {
        ps->p = start + i;
        fail(ps, "bad \\u escape");
        return nullptr;
      }
      i += 4;
      if (cp >= 0xDC00 && cp <= 0xDFFF) {
        ps->p = start + i;
        fail(ps, "lone low surrogate");
        return nullptr;
      }
      if (cp >= 0xD800 && cp <= 0xDBFF) {
        long lo = (i + 2 < span && start[i + 1] == '\\' && start[i + 2] == 'u')
                    ? read_u4(start, span, i + 3) : -1;
        if (lo < 0xDC00 || lo > 0xDFFF) {
          ps->p = start + i;
          fail(ps, "lone high surrogate");
          return nullptr;
        }
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i += 6;
      }
      if (cp == 0) {
        ps->p = start + i;
        fail(ps, "control character escape");
        return nullptr;
      }
      char enc[4];
      size_t n = ccord_utf8_encode((char32_t)cp, enc);
      if (n == 0) {
        ps->p = start + i;
        fail(ps, "invalid code point");
        return nullptr;
      }
      memcpy(out + o, enc, n);
      o += n;
      break;
    }
    default:
      ps->p = start + i;
      fail(ps, "bad escape");
      return nullptr;
    }
  }
  out[o] = '\0';
  if (!ccord_utf8_valid(out, o)) {
    ps->p = start;
    fail(ps, "invalid UTF-8");
    return nullptr;
  }
  *len = o;
  ps->p = s + 1;
  return out;
}

static ccord_json *parse_number(parser *ps)
{
  const char *q = ps->p;
  if (q < ps->end && *q == '-') q++;
  if (q < ps->end && *q == '0') {
    q++;
  } else if (q < ps->end && *q >= '1' && *q <= '9') {
    while (q < ps->end && *q >= '0' && *q <= '9') q++;
  } else {
    return fail(ps, "bad number");
  }
  if (q < ps->end && *q == '.') {
    q++;
    if (!(q < ps->end && *q >= '0' && *q <= '9')) return fail(ps, "bad number");
    while (q < ps->end && *q >= '0' && *q <= '9') q++;
  }
  if (q < ps->end && (*q == 'e' || *q == 'E')) {
    q++;
    if (q < ps->end && (*q == '+' || *q == '-')) q++;
    if (!(q < ps->end && *q >= '0' && *q <= '9')) return fail(ps, "bad number");
    while (q < ps->end && *q >= '0' && *q <= '9') q++;
  }
  size_t n = (size_t)(q - ps->p);
  if (n > JSON_NUMBER_MAX) return fail(ps, "number too long");
  ccord_json *j = node_new(ps, CCORD_JSON_NUMBER);
  if (!j) return nullptr;
  char *raw = ccord_arena_strndup(ps->a, ps->p, n);
  if (!raw) {
    ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory");
    return nullptr;
  }
  j->str = raw;
  j->slen = n;
  j->num = strtod(raw, nullptr);
  if (!isfinite(j->num)) return fail(ps, "number out of range");
  ps->p = q;
  return j;
}

static bool match_word(parser *ps, const char *w)
{
  size_t n = strlen(w);
  if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, w, n) != 0) return false;
  ps->p += n;
  return true;
}

static ccord_json *parse_value(parser *ps, size_t depth);

static ccord_json *parse_container(parser *ps, size_t depth, bool is_object)
{
  if (depth > ps->max_depth) {
    ccord_error_set(ps->err, CCORD_ERR_RANGE, "json: nesting too deep");
    return nullptr;
  }
  ps->p++; // opening bracket
  ccord_json *j = node_new(ps, is_object ? CCORD_JSON_OBJECT : CCORD_JSON_ARRAY);
  if (!j) return nullptr;
  const char close = is_object ? '}' : ']';

  size_t cap = 0, n = 0;
  ccord_json **vals = nullptr;
  const char **keys = nullptr;
  ccord_json *result = nullptr;

  skip_ws(ps);
  if (ps->p < ps->end && *ps->p == close) {
    ps->p++;
    return j;
  }
  while (true) {
    const char *key = nullptr;
    skip_ws(ps);
    if (is_object) {
      if (ps->p >= ps->end || *ps->p != '"') { fail(ps, "expected key"); goto done; }
      size_t klen;
      key = parse_string(ps, &klen);
      if (!key) goto done;
      for (size_t i = 0; i < n; i++) {
        if (strcmp(keys[i], key) == 0) { fail(ps, "duplicate key"); goto done; }
      }
      skip_ws(ps);
      if (ps->p >= ps->end || *ps->p != ':') { fail(ps, "expected ':'"); goto done; }
      ps->p++;
      skip_ws(ps);
    }
    if (n == cap) {
      size_t ncap = cap ? cap * 2 : 8;
      ccord_json **nv = ccord_realloc_array(vals, ncap, sizeof *nv);
      if (!nv) { ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory"); goto done; }
      vals = nv;
      if (is_object) {
        const char **nk = ccord_realloc_array(keys, ncap, sizeof *nk);
        if (!nk) { ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory"); goto done; }
        keys = nk;
      }
      cap = ncap;
    }
    ccord_json *v = parse_value(ps, depth + 1);
    if (!v) goto done;
    vals[n] = v;
    if (is_object) keys[n] = key;
    n++;
    skip_ws(ps);
    if (ps->p >= ps->end) { fail(ps, "unterminated container"); goto done; }
    if (*ps->p == ',') {
      ps->p++;
      continue;
    }
    if (*ps->p == close) {
      ps->p++;
      break;
    }
    fail(ps, "expected ',' or closing bracket");
    goto done;
  }

  j->count = n;
  if (n > 0) {
    j->items = ccord_arena_alloc(ps->a, n * sizeof *j->items, alignof(ccord_json *));
    if (!j->items) { ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory"); goto done; }
    memcpy(j->items, vals, n * sizeof *vals);
    if (is_object) {
      j->keys = ccord_arena_alloc(ps->a, n * sizeof *j->keys, alignof(const char *));
      if (!j->keys) { ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory"); goto done; }
      memcpy(j->keys, keys, n * sizeof *keys);
    }
  }
  result = j;
done:
  ccord_free(vals);
  ccord_free(keys);
  return result;
}

static ccord_json *parse_value(parser *ps, size_t depth)
{
  skip_ws(ps);
  if (ps->p >= ps->end) return fail(ps, "unexpected end");
  switch (*ps->p) {
  case '{': return parse_container(ps, depth, true);
  case '[': return parse_container(ps, depth, false);
  case '"': {
    size_t len;
    char *s = parse_string(ps, &len);
    if (!s) return nullptr;
    ccord_json *j = node_new(ps, CCORD_JSON_STRING);
    if (!j) return nullptr;
    j->str = s;
    j->slen = len;
    return j;
  }
  case 't':
  case 'f': {
    bool v = *ps->p == 't';
    if (!match_word(ps, v ? "true" : "false")) return fail(ps, "bad literal");
    ccord_json *j = node_new(ps, CCORD_JSON_BOOL);
    if (j) j->b = v;
    return j;
  }
  case 'n': {
    if (!match_word(ps, "null")) return fail(ps, "bad literal");
    return node_new(ps, CCORD_JSON_NULL);
  }
  default:
    if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9')) return parse_number(ps);
    return fail(ps, "unexpected character");
  }
}

const ccord_json *ccord_json_parse(ccord_arena *a, const char *text, size_t len,
                                   const ccord_json_limits *lim, ccord_error *err)
{
  ccord_error_clear(err);
  if (!a || (!text && len)) {
    ccord_error_set(err, CCORD_ERR_INVALID, "json: bad arguments");
    return nullptr;
  }
  parser ps = {
    .base = text, .p = text, .end = text ? text + len : nullptr, .a = a,
    .max_depth = lim && lim->max_depth ? lim->max_depth : JSON_DEFAULT_DEPTH,
    .max_nodes = lim && lim->max_nodes ? lim->max_nodes : JSON_DEFAULT_NODES,
    .max_string = lim && lim->max_string ? lim->max_string : JSON_DEFAULT_STRING,
    .nodes = 0, .err = err,
  };
  if (!text) {
    ccord_error_set(err, CCORD_ERR_PARSE, "json: empty input");
    return nullptr;
  }
  ccord_json *root = parse_value(&ps, 1);
  if (!root) return nullptr;
  skip_ws(&ps);
  if (ps.p != ps.end) {
    fail(&ps, "trailing data");
    return nullptr;
  }
  return root;
}

ccord_json_type ccord_json_type_of(const ccord_json *j)
{
  return j ? j->type : CCORD_JSON_NULL;
}

bool ccord_json_bool(const ccord_json *j, bool fallback)
{
  if (!j) return fallback;
  return j->type == CCORD_JSON_BOOL ? j->b : fallback;
}

double ccord_json_number(const ccord_json *j, double fallback)
{
  if (!j) return fallback;
  return j->type == CCORD_JSON_NUMBER ? j->num : fallback;
}

bool ccord_json_int64(const ccord_json *j, int64_t *out)
{
  if (!j || !out || j->type != CCORD_JSON_NUMBER || !j->str) return false;
  for (size_t i = 0; i < j->slen; i++) {
    char c = j->str[i];
    if (c == '.' || c == 'e' || c == 'E') return false;
  }
  errno = 0;
  long long v = strtoll(j->str, nullptr, 10);
  if (errno == ERANGE) return false;
  *out = (int64_t)v;
  return true;
}

const char *ccord_json_string(const ccord_json *j, size_t *len)
{
  if (!j || j->type != CCORD_JSON_STRING) return nullptr;
  if (len) *len = j->slen;
  return j->str;
}

size_t ccord_json_array_len(const ccord_json *j)
{
  return (j && j->type == CCORD_JSON_ARRAY) ? j->count : 0;
}

const ccord_json *ccord_json_array_at(const ccord_json *j, size_t i)
{
  if (!j || j->type != CCORD_JSON_ARRAY || i >= j->count) return nullptr;
  return j->items[i];
}

size_t ccord_json_object_len(const ccord_json *j)
{
  return (j && j->type == CCORD_JSON_OBJECT) ? j->count : 0;
}

const ccord_json *ccord_json_get(const ccord_json *obj, const char *key)
{
  if (!obj || !key || obj->type != CCORD_JSON_OBJECT) return nullptr;
  for (size_t i = 0; i < obj->count; i++) {
    if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
  }
  return nullptr;
}

const char *ccord_json_object_key(const ccord_json *obj, size_t i, const ccord_json **value)
{
  if (!obj || obj->type != CCORD_JSON_OBJECT || i >= obj->count) return nullptr;
  if (value) *value = obj->items[i];
  return obj->keys[i];
}

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
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
    case '"': jw_put(w, "\\\"", 2); break;
    case '\\': jw_put(w, "\\\\", 2); break;
    case '\n': jw_put(w, "\\n", 2); break;
    case '\r': jw_put(w, "\\r", 2); break;
    case '\t': jw_put(w, "\\t", 2); break;
    case '\b': jw_put(w, "\\b", 2); break;
    case '\f': jw_put(w, "\\f", 2); break;
    default:
      if (c < 0x20) {
        char e[7];
        snprintf(e, sizeof e, "\\u%04x", (unsigned)c);
        jw_put(w, e, 6);
      } else {
        jw_put(w, s + i, 1);
      }
    }
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

bool ccord_jw_ok(const ccord_jw *w)
{
  return w && w->out && !w->failed && !w->out->failed && w->depth == 0;
}
