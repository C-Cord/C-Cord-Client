// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/json.h>

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
#define JSON_KEY_HT_THRESH 16u

struct ccord_json {
  uint8_t type;    // ccord_json_type
  uint8_t flags;   // BOOL: 0/1; NUMBER: 1 iff u.n.i64 is valid
  uint32_t count;  // ARRAY / OBJECT element count
  union {
    struct { double num; int64_t i64; } n;
    struct { const char *str; size_t slen; } s;
    struct { ccord_json **items; const char **keys; } c;
  } u;
};

static_assert(sizeof(struct ccord_json) <= 24, "ccord_json node must stay compact");

typedef struct parser {
  const char *base, *p, *end;
  ccord_arena *a;
  size_t max_depth, max_nodes, max_string, nodes;
  ccord_error *err;
  ccord_json **val_stk;
  size_t val_top, val_cap;
  const char **key_stk;
  size_t key_top, key_cap;
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
  ccord_json *j = ccord_arena_alloc_uninit(ps->a, sizeof *j, alignof(ccord_json));
  if (!j) {
    ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory");
    return nullptr;
  }
  j->type = (uint8_t)t;
  j->flags = 0;
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
  bool has_escape = false;
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
      has_escape = true;
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
  size_t alloc_sz;
  if (ckd_add(&alloc_sz, span, 1u)) {
    ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory");
    return nullptr;
  }
  if (!has_escape) {
    if (!ccord_utf8_valid(start, span)) {
      ps->p = start;
      fail(ps, "invalid UTF-8");
      return nullptr;
    }
    char *out = ccord_arena_alloc_uninit(ps->a, alloc_sz, 1);
    if (!out) {
      ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory");
      return nullptr;
    }
    if (span > 0) {
      memcpy(out, start, span);
    }
    out[span] = '\0';
    *len = span;
    ps->p = s + 1;
    return out;
  }
  char *out = ccord_arena_alloc_uninit(ps->a, alloc_sz, 1);
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
  bool is_int = true;
  if (q < ps->end && *q == '-') q++;
  if (q < ps->end && *q == '0') {
    q++;
  } else if (q < ps->end && *q >= '1' && *q <= '9') {
    while (q < ps->end && *q >= '0' && *q <= '9') q++;
  } else {
    return fail(ps, "bad number");
  }
  if (q < ps->end && *q == '.') {
    is_int = false;
    q++;
    if (!(q < ps->end && *q >= '0' && *q <= '9')) return fail(ps, "bad number");
    while (q < ps->end && *q >= '0' && *q <= '9') q++;
  }
  if (q < ps->end && (*q == 'e' || *q == 'E')) {
    is_int = false;
    q++;
    if (q < ps->end && (*q == '+' || *q == '-')) q++;
    if (!(q < ps->end && *q >= '0' && *q <= '9')) return fail(ps, "bad number");
    while (q < ps->end && *q >= '0' && *q <= '9') q++;
  }
  size_t n = (size_t)(q - ps->p);
  if (n > JSON_NUMBER_MAX) return fail(ps, "number too long");

  bool has_i64 = false;
  int64_t i64 = 0;
  double num = 0.0;
  if (is_int) {
    bool neg = (*ps->p == '-');
    const char *d = ps->p + (neg ? 1 : 0);
    uint64_t limit = neg ? ((uint64_t)INT64_MAX + 1u) : (uint64_t)INT64_MAX;
    uint64_t acc = 0;
    bool overflow = false;
    while (d < q) {
      uint64_t digit = (uint64_t)(unsigned char)(*d - '0');
      if (acc > (limit - digit) / 10u) {
        overflow = true;
        break;
      }
      acc = acc * 10u + digit;
      d++;
    }
    if (!overflow) {
      has_i64 = true;
      if (neg) {
        i64 = (acc == ((uint64_t)INT64_MAX + 1u)) ? INT64_MIN : -(int64_t)acc;
        num = (acc == 0) ? -0.0 : (double)i64;
      } else {
        i64 = (int64_t)acc;
        num = (double)i64;
      }
    }
  }
  if (!has_i64) {
    char raw[JSON_NUMBER_MAX + 1u];
    memcpy(raw, ps->p, n);
    raw[n] = '\0';
    num = strtod(raw, nullptr);
    if (!isfinite(num)) return fail(ps, "number out of range");
  }
  ccord_json *j = node_new(ps, CCORD_JSON_NUMBER);
  if (!j) return nullptr;
  j->flags = has_i64 ? 1u : 0u;
  j->u.n.num = num;
  j->u.n.i64 = i64;
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

static bool stk_push_val(parser *ps, ccord_json *v)
{
  if (ps->val_top == ps->val_cap) {
    size_t ncap = ps->val_cap ? ps->val_cap * 2u : 32u;
    if (ncap < ps->val_cap) {
      ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory");
      return false;
    }
    ccord_json **nv = ccord_realloc_array(ps->val_stk, ncap, sizeof *nv);
    if (!nv) {
      ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory");
      return false;
    }
    ps->val_stk = nv;
    ps->val_cap = ncap;
  }
  ps->val_stk[ps->val_top++] = v;
  return true;
}

static bool stk_push_key(parser *ps, const char *k)
{
  if (ps->key_top == ps->key_cap) {
    size_t ncap = ps->key_cap ? ps->key_cap * 2u : 32u;
    if (ncap < ps->key_cap) {
      ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory");
      return false;
    }
    const char **nk = ccord_realloc_array(ps->key_stk, ncap, sizeof *nk);
    if (!nk) {
      ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory");
      return false;
    }
    ps->key_stk = nk;
    ps->key_cap = ncap;
  }
  ps->key_stk[ps->key_top++] = k;
  return true;
}

typedef struct key_ht_slot {
  uint64_t hash;
  const char *key;
} key_ht_slot;

static uint64_t fnv1a64(const char *s, size_t n)
{
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static bool key_ht_grow(key_ht_slot **ht, size_t *cap, size_t min_cap)
{
  size_t ncap = *cap ? *cap : 64u;
  while (ncap < min_cap) {
    if (ckd_mul(&ncap, ncap, 2u)) return false;
  }
  key_ht_slot *ns = ccord_calloc(ncap, sizeof *ns);
  if (!ns) return false;
  size_t mask = ncap - 1u;
  for (size_t i = 0; i < *cap; i++) {
    if (!(*ht)[i].key) continue;
    size_t at = (size_t)(*ht)[i].hash & mask;
    while (ns[at].key) at = (at + 1u) & mask;
    ns[at] = (*ht)[i];
  }
  ccord_free(*ht);
  *ht = ns;
  *cap = ncap;
  return true;
}

// Returns 1 if inserted, 0 if duplicate, -1 on OOM.
static int key_ht_insert(key_ht_slot **ht, size_t *cap, size_t count, const char *key, size_t klen)
{
  size_t need;
  if (ckd_add(&need, count, 1u) || ckd_mul(&need, need, 2u)) return -1;
  if (*cap < need && !key_ht_grow(ht, cap, need)) return -1;
  uint64_t h = fnv1a64(key, klen);
  size_t mask = *cap - 1u;
  size_t at = (size_t)h & mask;
  while ((*ht)[at].key) {
    if ((*ht)[at].hash == h && strcmp((*ht)[at].key, key) == 0) return 0;
    at = (at + 1u) & mask;
  }
  (*ht)[at].hash = h;
  (*ht)[at].key = key;
  return 1;
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
  j->u.c.items = nullptr;
  j->u.c.keys = nullptr;
  const char close = is_object ? '}' : ']';

  skip_ws(ps);
  if (ps->p < ps->end && *ps->p == close) {
    ps->p++;
    return j;
  }

  size_t val_base = ps->val_top;
  size_t key_base = ps->key_top;
  size_t n = 0;
  key_ht_slot *ht = nullptr;
  size_t ht_cap = 0;
  ccord_json *result = nullptr;

  while (true) {
    const char *key = nullptr;
    skip_ws(ps);
    if (is_object) {
      if (ps->p >= ps->end || *ps->p != '"') { fail(ps, "expected key"); goto done; }
      size_t klen;
      key = parse_string(ps, &klen);
      if (!key) goto done;
      if (!ht && n == JSON_KEY_HT_THRESH) {
        if (!key_ht_grow(&ht, &ht_cap, 64u)) {
          ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory");
          goto done;
        }
        size_t mask = ht_cap - 1u;
        for (size_t i = 0; i < n; i++) {
          const char *ek = ps->key_stk[key_base + i];
          uint64_t eh = fnv1a64(ek, strlen(ek));
          size_t at = (size_t)eh & mask;
          while (ht[at].key) at = (at + 1u) & mask;
          ht[at].hash = eh;
          ht[at].key = ek;
        }
      }
      if (ht) {
        int ins = key_ht_insert(&ht, &ht_cap, n, key, klen);
        if (ins < 0) { ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory"); goto done; }
        if (ins == 0) { fail(ps, "duplicate key"); goto done; }
      } else {
        for (size_t i = 0; i < n; i++) {
          if (strcmp(ps->key_stk[key_base + i], key) == 0) { fail(ps, "duplicate key"); goto done; }
        }
      }
      skip_ws(ps);
      if (ps->p >= ps->end || *ps->p != ':') { fail(ps, "expected ':'"); goto done; }
      ps->p++;
      skip_ws(ps);
    }
    ccord_json *v = parse_value(ps, depth + 1);
    if (!v) goto done;
    if (!stk_push_val(ps, v)) goto done;
    if (is_object && !stk_push_key(ps, key)) goto done;
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

  if (n > UINT32_MAX) {
    ccord_error_set(ps->err, CCORD_ERR_RANGE, "json: container too large");
    goto done;
  }
  j->count = (uint32_t)n;
  if (n > 0) {
    size_t item_bytes;
    if (ckd_mul(&item_bytes, n, sizeof *j->u.c.items)) {
      ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory");
      goto done;
    }
    j->u.c.items = ccord_arena_alloc_uninit(ps->a, item_bytes, alignof(ccord_json *));
    if (!j->u.c.items) { ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory"); goto done; }
    memcpy(j->u.c.items, ps->val_stk + val_base, item_bytes);
    if (is_object) {
      size_t key_bytes;
      if (ckd_mul(&key_bytes, n, sizeof *j->u.c.keys)) {
        ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory");
        goto done;
      }
      j->u.c.keys = ccord_arena_alloc_uninit(ps->a, key_bytes, alignof(const char *));
      if (!j->u.c.keys) { ccord_error_set(ps->err, CCORD_ERR_NOMEM, "json: out of memory"); goto done; }
      memcpy(j->u.c.keys, ps->key_stk + key_base, key_bytes);
    }
  }
  result = j;
done:
  ccord_free(ht);
  ps->val_top = val_base;
  ps->key_top = key_base;
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
    j->u.s.str = s;
    j->u.s.slen = len;
    return j;
  }
  case 't':
  case 'f': {
    bool v = *ps->p == 't';
    if (!match_word(ps, v ? "true" : "false")) return fail(ps, "bad literal");
    ccord_json *j = node_new(ps, CCORD_JSON_BOOL);
    if (j) j->flags = v ? 1u : 0u;
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
  ccord_free(ps.val_stk);
  ccord_free(ps.key_stk);
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
  return j ? (ccord_json_type)j->type : CCORD_JSON_NULL;
}

bool ccord_json_bool(const ccord_json *j, bool fallback)
{
  if (!j) return fallback;
  return j->type == CCORD_JSON_BOOL ? (j->flags != 0) : fallback;
}

double ccord_json_number(const ccord_json *j, double fallback)
{
  if (!j) return fallback;
  return j->type == CCORD_JSON_NUMBER ? j->u.n.num : fallback;
}

bool ccord_json_int64(const ccord_json *j, int64_t *out)
{
  if (!j || !out || j->type != CCORD_JSON_NUMBER || !j->flags) return false;
  *out = j->u.n.i64;
  return true;
}

const char *ccord_json_string(const ccord_json *j, size_t *len)
{
  if (!j || j->type != CCORD_JSON_STRING) return nullptr;
  if (len) *len = j->u.s.slen;
  return j->u.s.str;
}

size_t ccord_json_array_len(const ccord_json *j)
{
  return (j && j->type == CCORD_JSON_ARRAY) ? (size_t)j->count : 0;
}

const ccord_json *ccord_json_array_at(const ccord_json *j, size_t i)
{
  if (!j || j->type != CCORD_JSON_ARRAY || i >= j->count) return nullptr;
  return j->u.c.items[i];
}

size_t ccord_json_object_len(const ccord_json *j)
{
  return (j && j->type == CCORD_JSON_OBJECT) ? (size_t)j->count : 0;
}

const ccord_json *ccord_json_get(const ccord_json *obj, const char *key)
{
  if (!obj || !key || obj->type != CCORD_JSON_OBJECT) return nullptr;
  for (size_t i = 0; i < obj->count; i++) {
    if (strcmp(obj->u.c.keys[i], key) == 0) return obj->u.c.items[i];
  }
  return nullptr;
}

const char *ccord_json_object_key(const ccord_json *obj, size_t i, const ccord_json **value)
{
  if (!obj || obj->type != CCORD_JSON_OBJECT || i >= obj->count) return nullptr;
  if (value) *value = obj->u.c.items[i];
  return obj->u.c.keys[i];
}

