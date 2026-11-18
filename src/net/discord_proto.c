// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/net/discord_proto.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>

// Schema and sources: see the header comment.

// ---------------------------------------------------------------------------------------------------------------
// Wire reader
// ---------------------------------------------------------------------------------------------------------------

typedef struct slice {
  const uint8_t *p;
  size_t n;
} slice;

typedef struct fld {
  uint32_t field;
  uint32_t wt;
  uint64_t v;         // varint / fixed32 / fixed64 value
  slice data;         // length-delimited payload (wire type 2)
} fld;

static ccord_status bad(ccord_error *err, const char *what)
{
  return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: %s", what);
}

static bool read_varint(slice *r, uint64_t *out)
{
  uint64_t v = 0;
  for (unsigned i = 0; i < 10; i++) {
    if (r->n == 0) return false;
    uint8_t b = *r->p;
    r->p++;
    r->n--;
    if (i == 9 && b > 1) return false;  // would overflow 64 bits
    v |= (uint64_t)(b & 0x7fu) << (7u * i);
    if (!(b & 0x80u)) {
      *out = v;
      return true;
    }
  }
  return false;  // 10 continuation bytes
}

static bool read_fixed(slice *r, size_t bytes, uint64_t *out)
{
  if (r->n < bytes) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < bytes; i++) v |= (uint64_t)r->p[i] << (8u * i);
  r->p += bytes;
  r->n -= bytes;
  *out = v;
  return true;
}

// Reads the next field. Wire types 3/4 (groups) and 6/7 are rejected: they cannot be skipped safely without
// recursion and Discord never emits them.
static ccord_status next_field(slice *r, fld *f, ccord_error *err)
{
  uint64_t tag;
  if (!read_varint(r, &tag)) return bad(err, "truncated field tag");
  if (tag > UINT32_MAX || (tag >> 3) == 0 || (tag >> 3) > 0x1fffffffu) return bad(err, "invalid field number");
  f->field = (uint32_t)(tag >> 3);
  f->wt = (uint32_t)(tag & 7u);
  f->v = 0;
  f->data = (slice){nullptr, 0};
  switch (f->wt) {
    case 0:
      if (!read_varint(r, &f->v)) return bad(err, "truncated varint");
      return CCORD_OK;
    case 1:
      if (!read_fixed(r, 8, &f->v)) return bad(err, "truncated fixed64");
      return CCORD_OK;
    case 5:
      if (!read_fixed(r, 4, &f->v)) return bad(err, "truncated fixed32");
      return CCORD_OK;
    case 2: {
      uint64_t len;
      if (!read_varint(r, &len)) return bad(err, "truncated length");
      if (len > r->n) return bad(err, "length-delimited field overruns buffer");
      f->data = (slice){r->p, (size_t)len};
      r->p += (size_t)len;
      r->n -= (size_t)len;
      return CCORD_OK;
    }
    default:
      return bad(err, "unsupported wire type");
  }
}

static ccord_status take_string(const fld *f, slice *out, ccord_error *err)
{
  if (f->data.n > CCORD_PROTO_MAX_STRING)
    return ccord_error_set(err, CCORD_ERR_RANGE, "settings proto: string longer than %u bytes", CCORD_PROTO_MAX_STRING);
  if (f->data.n > 0) {
    if (memchr(f->data.p, 0, f->data.n)) return bad(err, "string contains NUL");
    if (!ccord_utf8_valid((const char *)f->data.p, f->data.n)) return bad(err, "string is not valid UTF-8");
  }
  *out = f->data;
  return CCORD_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Temporary (borrowing) records
// ---------------------------------------------------------------------------------------------------------------

typedef struct tgif {
  slice key, src;
  uint32_t format, width, height, order;
  size_t seq;
} tgif;

typedef struct tfrec {
  slice key;
  uint64_t id;
  uint32_t total;
  int32_t frecency, score;
  size_t seq;
} tfrec;

typedef struct vec {
  void *p;
  size_t n, cap;
} vec;

static ccord_status vec_push(vec *v, size_t esz, const void *e, ccord_error *err)
{
  if (v->n >= CCORD_PROTO_MAX_ITEMS)
    return ccord_error_set(err, CCORD_ERR_RANGE, "settings proto: more than %u items in a collection",
                           CCORD_PROTO_MAX_ITEMS);
  if (v->n == v->cap) {
    size_t cap = v->cap ? v->cap * 2 : 16;
    if (cap > CCORD_PROTO_MAX_ITEMS) cap = CCORD_PROTO_MAX_ITEMS;
    void *np = ccord_realloc_array(v->p, cap, esz);
    if (!np) return ccord_error_set(err, CCORD_ERR_NOMEM, "settings proto: out of memory");
    v->p = np;
    v->cap = cap;
  }
  memcpy((char *)v->p + v->n * esz, e, esz);
  v->n++;
  return CCORD_OK;
}

typedef struct pctx {
  vec gifs;          // tgif
  vec fav_stickers;  // uint64_t
  vec sticker_frec;  // tfrec
  vec fav_emojis;    // slice
  vec emoji_frec;    // tfrec
  size_t seq;
  bool has_gifs;     // root field 2 (favorite_gifs) was present, even when it holds no entries
} pctx;

// ---------------------------------------------------------------------------------------------------------------
// Message parsers (nesting depth is fixed by the schema: settings -> container -> map entry -> value)
// ---------------------------------------------------------------------------------------------------------------

static ccord_status parse_gif_value(slice msg, tgif *g, ccord_error *err)
{
  while (msg.n > 0) {
    fld f;
    ccord_status st = next_field(&msg, &f, err);
    if (st != CCORD_OK) return st;
    if (f.field == 1 && f.wt == 0) g->format = (uint32_t)f.v;
    else if (f.field == 2 && f.wt == 2) {
      st = take_string(&f, &g->src, err);
      if (st != CCORD_OK) return st;
    } else if (f.field == 3 && f.wt == 0) g->width = (uint32_t)f.v;
    else if (f.field == 4 && f.wt == 0) g->height = (uint32_t)f.v;
    else if (f.field == 5 && f.wt == 0) g->order = (uint32_t)f.v;
  }
  return CCORD_OK;
}

static ccord_status parse_gifs(slice msg, pctx *c, ccord_error *err)
{
  while (msg.n > 0) {
    fld f;
    ccord_status st = next_field(&msg, &f, err);
    if (st != CCORD_OK) return st;
    if (f.field != 1 || f.wt != 2) continue;  // hide_tooltip and unknown fields
    slice entry = f.data;
    tgif g = {};
    while (entry.n > 0) {
      fld e;
      st = next_field(&entry, &e, err);
      if (st != CCORD_OK) return st;
      if (e.field == 1 && e.wt == 2) {
        st = take_string(&e, &g.key, err);
        if (st != CCORD_OK) return st;
      } else if (e.field == 2 && e.wt == 2) {
        st = parse_gif_value(e.data, &g, err);
        if (st != CCORD_OK) return st;
      }
    }
    if (g.key.n == 0) continue;
    g.seq = c->seq++;
    st = vec_push(&c->gifs, sizeof g, &g, err);
    if (st != CCORD_OK) return st;
  }
  return CCORD_OK;
}

static ccord_status parse_frec_item(slice msg, tfrec *t, ccord_error *err)
{
  while (msg.n > 0) {
    fld f;
    ccord_status st = next_field(&msg, &f, err);
    if (st != CCORD_OK) return st;
    if (f.wt != 0) continue;  // recent_uses (packed or not) and unknowns are skipped
    if (f.field == 1) t->total = (uint32_t)f.v;
    else if (f.field == 3) t->frecency = (int32_t)(uint32_t)f.v;  // int32 varint: low 32 bits (negatives are sign-extended)
    else if (f.field == 4) t->score = (int32_t)(uint32_t)f.v;
  }
  return CCORD_OK;
}

// Parses `message { map<K, FrecencyItem> field1 = 1; }` where K is a string (fixed_key == false) or fixed64.
static ccord_status parse_frec_map(slice msg, bool fixed_key, vec *out, pctx *c, ccord_error *err)
{
  while (msg.n > 0) {
    fld f;
    ccord_status st = next_field(&msg, &f, err);
    if (st != CCORD_OK) return st;
    if (f.field != 1 || f.wt != 2) continue;
    slice entry = f.data;
    tfrec t = {};
    while (entry.n > 0) {
      fld e;
      st = next_field(&entry, &e, err);
      if (st != CCORD_OK) return st;
      if (e.field == 1 && fixed_key && e.wt == 1) t.id = e.v;
      else if (e.field == 1 && !fixed_key && e.wt == 2) {
        st = take_string(&e, &t.key, err);
        if (st != CCORD_OK) return st;
      } else if (e.field == 2 && e.wt == 2) {
        st = parse_frec_item(e.data, &t, err);
        if (st != CCORD_OK) return st;
      }
    }
    if (fixed_key ? t.id == 0 : t.key.n == 0) continue;
    t.seq = c->seq++;
    st = vec_push(out, sizeof t, &t, err);
    if (st != CCORD_OK) return st;
  }
  return CCORD_OK;
}

static ccord_status parse_fav_stickers(slice msg, pctx *c, ccord_error *err)
{
  while (msg.n > 0) {
    fld f;
    ccord_status st = next_field(&msg, &f, err);
    if (st != CCORD_OK) return st;
    if (f.field != 1) continue;
    if (f.wt == 1) {  // unpacked record
      st = vec_push(&c->fav_stickers, sizeof f.v, &f.v, err);
      if (st != CCORD_OK) return st;
    } else if (f.wt == 2) {  // packed
      if (f.data.n % 8 != 0) return bad(err, "packed fixed64 length not a multiple of 8");
      slice p = f.data;
      while (p.n > 0) {
        uint64_t id = 0;
        if (!read_fixed(&p, 8, &id)) return bad(err, "truncated packed fixed64");
        st = vec_push(&c->fav_stickers, sizeof id, &id, err);
        if (st != CCORD_OK) return st;
      }
    }
  }
  return CCORD_OK;
}

static ccord_status parse_fav_emojis(slice msg, pctx *c, ccord_error *err)
{
  while (msg.n > 0) {
    fld f;
    ccord_status st = next_field(&msg, &f, err);
    if (st != CCORD_OK) return st;
    if (f.field != 1 || f.wt != 2) continue;
    slice s;
    st = take_string(&f, &s, err);
    if (st != CCORD_OK) return st;
    if (s.n == 0) continue;
    st = vec_push(&c->fav_emojis, sizeof s, &s, err);
    if (st != CCORD_OK) return st;
  }
  return CCORD_OK;
}

static ccord_status parse_settings(slice msg, pctx *c, ccord_error *err)
{
  while (msg.n > 0) {
    fld f;
    ccord_status st = next_field(&msg, &f, err);
    if (st != CCORD_OK) return st;
    if (f.wt != 2) continue;
    switch (f.field) {
      case 2: c->has_gifs = true; st = parse_gifs(f.data, c, err); break;
      case 3: st = parse_fav_stickers(f.data, c, err); break;
      case 4: st = parse_frec_map(f.data, true, &c->sticker_frec, c, err); break;
      case 5: st = parse_fav_emojis(f.data, c, err); break;
      case 6: st = parse_frec_map(f.data, false, &c->emoji_frec, c, err); break;
      default: st = CCORD_OK; break;  // versions, application/sound/guild frecency, pinned packs, ...
    }
    if (st != CCORD_OK) return st;
  }
  return CCORD_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Finalisation: dedupe (last wins), sort, copy into the arena
// ---------------------------------------------------------------------------------------------------------------

static int slice_cmp(slice a, slice b)
{
  size_t m = a.n < b.n ? a.n : b.n;
  int r = m ? memcmp(a.p, b.p, m) : 0;
  if (r) return r;
  return (a.n > b.n) - (a.n < b.n);
}

static int cmp_gif_key(const void *x, const void *y)
{
  const tgif *a = x, *b = y;
  int r = slice_cmp(a->key, b->key);
  if (r) return r;
  return (a->seq > b->seq) - (a->seq < b->seq);
}

static int cmp_gif_order(const void *x, const void *y)
{
  const tgif *a = x, *b = y;
  if (a->order != b->order) return a->order < b->order ? -1 : 1;
  // The official client reads the map with lodash sortBy("order") (stable ascending) and then .reverse()s it. This is
  // the sortBy half: equal (or missing) orders keep wire order; the URL is not a tiebreak. The caller reverses.
  return (a->seq > b->seq) - (a->seq < b->seq);
}

static int cmp_frec_key(const void *x, const void *y)
{
  const tfrec *a = x, *b = y;
  int r = slice_cmp(a->key, b->key);
  if (r) return r;
  if (a->id != b->id) return a->id < b->id ? -1 : 1;
  return (a->seq > b->seq) - (a->seq < b->seq);
}

static int cmp_frec_rank(const void *x, const void *y)
{
  const tfrec *a = x, *b = y;
  if (a->score != b->score) return a->score > b->score ? -1 : 1;
  if (a->frecency != b->frecency) return a->frecency > b->frecency ? -1 : 1;
  if (a->total != b->total) return a->total > b->total ? -1 : 1;
  int r = slice_cmp(a->key, b->key);
  if (r) return r;
  return (a->id > b->id) - (a->id < b->id);
}

static void dedupe_gifs(vec *v)
{
  tgif *a = v->p;
  if (v->n == 0) return;
  qsort(a, v->n, sizeof *a, cmp_gif_key);
  size_t w = 0;
  for (size_t i = 0; i < v->n; i++) {
    if (i + 1 < v->n && slice_cmp(a[i].key, a[i + 1].key) == 0) continue;  // a later duplicate follows (higher seq)
    a[w++] = a[i];
  }
  v->n = w;
}

static void dedupe_frec(vec *v)
{
  tfrec *a = v->p;
  if (v->n == 0) return;
  qsort(a, v->n, sizeof *a, cmp_frec_key);
  size_t w = 0;
  for (size_t i = 0; i < v->n; i++) {
    if (i + 1 < v->n && a[i].id == a[i + 1].id && slice_cmp(a[i].key, a[i + 1].key) == 0) continue;  // later wins
    a[w++] = a[i];
  }
  v->n = w;
}

static char *arena_str(ccord_arena *a, slice s)
{
  return ccord_arena_strndup(a, s.n ? (const char *)s.p : "", s.n);
}

// All-digit key that fits in uint64 -> numeric id (custom emoji snowflake), else 0.
static uint64_t numeric_key(slice k)
{
  if (k.n == 0 || k.n > 20) return 0;
  uint64_t v = 0;
  for (size_t i = 0; i < k.n; i++) {
    if (k.p[i] < '0' || k.p[i] > '9') return 0;
    uint64_t d = (uint64_t)(k.p[i] - '0');
    if (v > (UINT64_MAX - d) / 10) return 0;
    v = v * 10 + d;
  }
  return v;
}

static ccord_status copy_frec(ccord_arena *ar, vec *v, bool sticker, ccord_proto_frecency **out, size_t *count,
                              ccord_error *err)
{
  *out = nullptr;
  *count = 0;
  if (v->n == 0) return CCORD_OK;
  tfrec *t = v->p;
  qsort(t, v->n, sizeof *t, cmp_frec_rank);
  ccord_proto_frecency *r = ccord_arena_alloc(ar, v->n * sizeof *r, alignof(ccord_proto_frecency));
  if (!r) return ccord_error_set(err, CCORD_ERR_NOMEM, "settings proto: out of memory");
  for (size_t i = 0; i < v->n; i++) {
    r[i] = (ccord_proto_frecency){.total_uses = t[i].total, .frecency = t[i].frecency, .score = t[i].score};
    if (sticker) {
      char buf[24];
      int n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)t[i].id);
      r[i].id = t[i].id;
      r[i].key = ccord_arena_strndup(ar, buf, n > 0 ? (size_t)n : 0);
    } else {
      r[i].id = numeric_key(t[i].key);
      r[i].key = arena_str(ar, t[i].key);
    }
    if (!r[i].key) return ccord_error_set(err, CCORD_ERR_NOMEM, "settings proto: out of memory");
  }
  *out = r;
  *count = v->n;
  return CCORD_OK;
}

static ccord_status finalize(pctx *c, ccord_user_expressions *x, ccord_error *err)
{
  ccord_arena *ar = &x->arena;
  ccord_status st;

  x->has_favorite_gifs = c->has_gifs;
  if (c->gifs.n > 0) {
    dedupe_gifs(&c->gifs);
    tgif *g = c->gifs.p;
    qsort(g, c->gifs.n, sizeof *g, cmp_gif_order);
    x->gifs = ccord_arena_alloc(ar, c->gifs.n * sizeof *x->gifs, alignof(ccord_proto_gif));
    if (!x->gifs) return ccord_error_set(err, CCORD_ERR_NOMEM, "settings proto: out of memory");
    for (size_t i = 0; i < c->gifs.n; i++) {
      x->gifs[i] = (ccord_proto_gif){.url = arena_str(ar, g[i].key), .src = arena_str(ar, g[i].src),
                                     .format = g[i].format, .width = g[i].width, .height = g[i].height,
                                     .order = g[i].order};
      if (!x->gifs[i].url || !x->gifs[i].src) return ccord_error_set(err, CCORD_ERR_NOMEM, "settings proto: out of memory");
    }
    x->gif_count = c->gifs.n;
  }

  if (c->fav_stickers.n > 0) {
    x->favorite_stickers = ccord_arena_alloc(ar, c->fav_stickers.n * sizeof(uint64_t), alignof(uint64_t));
    if (!x->favorite_stickers) return ccord_error_set(err, CCORD_ERR_NOMEM, "settings proto: out of memory");
    memcpy(x->favorite_stickers, c->fav_stickers.p, c->fav_stickers.n * sizeof(uint64_t));
    x->favorite_sticker_count = c->fav_stickers.n;
  }

  if (c->fav_emojis.n > 0) {
    x->favorite_emojis = ccord_arena_alloc(ar, c->fav_emojis.n * sizeof *x->favorite_emojis, alignof(const char *));
    if (!x->favorite_emojis) return ccord_error_set(err, CCORD_ERR_NOMEM, "settings proto: out of memory");
    const slice *s = c->fav_emojis.p;
    for (size_t i = 0; i < c->fav_emojis.n; i++) {
      x->favorite_emojis[i] = arena_str(ar, s[i]);
      if (!x->favorite_emojis[i]) return ccord_error_set(err, CCORD_ERR_NOMEM, "settings proto: out of memory");
    }
    x->favorite_emoji_count = c->fav_emojis.n;
  }

  dedupe_frec(&c->sticker_frec);
  st = copy_frec(ar, &c->sticker_frec, true, &x->sticker_frecency, &x->sticker_frecency_count, err);
  if (st != CCORD_OK) return st;
  dedupe_frec(&c->emoji_frec);
  return copy_frec(ar, &c->emoji_frec, false, &x->emoji_frecency, &x->emoji_frecency_count, err);
}

// ---------------------------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------------------------

ccord_status ccord_user_expressions_decode(const uint8_t *buf, size_t len, ccord_user_expressions *out, ccord_error *err)
{
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: no output");
  *out = (ccord_user_expressions){};
  if (len > 0 && !buf) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: null buffer");
  if (len > CCORD_PROTO_MAX_INPUT)
    return ccord_error_set(err, CCORD_ERR_RANGE, "settings proto: input larger than %u bytes", CCORD_PROTO_MAX_INPUT);
  ccord_arena_init(&out->arena, 0);
  pctx c = {};
  ccord_status st = parse_settings((slice){buf, len}, &c, err);
  if (st == CCORD_OK) st = finalize(&c, out, err);
  ccord_free(c.gifs.p);
  ccord_free(c.fav_stickers.p);
  ccord_free(c.sticker_frec.p);
  ccord_free(c.fav_emojis.p);
  ccord_free(c.emoji_frec.p);
  if (st != CCORD_OK) {
    ccord_arena_free(&out->arena);
    *out = (ccord_user_expressions){};
  }
  return st;
}

static int b64_val(unsigned char ch)
{
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+' || ch == '-') return 62;
  if (ch == '/' || ch == '_') return 63;
  return -1;
}

// Decodes standard or url-safe base64 (padding optional, ASCII whitespace ignored) into a fresh ccord_malloc
// buffer. *out is NULL on failure; *out_len may be NULL.
static ccord_status b64_decode_alloc(const char *b64, size_t len, uint8_t **out, size_t *out_len, ccord_error *err)
{
  *out = nullptr;
  if (out_len) *out_len = 0;
  if (len > 0 && !b64) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: null base64");
  if (len > CCORD_PROTO_MAX_INPUT * 2u)
    return ccord_error_set(err, CCORD_ERR_RANGE, "settings proto: base64 input too large");
  uint8_t *raw = ccord_malloc(len / 4 * 3 + 4);
  if (!raw) return ccord_error_set(err, CCORD_ERR_NOMEM, "settings proto: out of memory");
  size_t n = 0;
  uint32_t acc = 0;
  unsigned bits = 0, chars = 0;
  bool padding = false;
  ccord_status st = CCORD_OK;
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)b64[i];
    if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
    if (ch == '=') {
      padding = true;
      continue;
    }
    int v = b64_val(ch);
    if (v < 0 || padding) {
      st = bad(err, "invalid base64");
      break;
    }
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    chars++;
    if (bits >= 8) {
      bits -= 8;
      raw[n++] = (uint8_t)((acc >> bits) & 0xffu);
    }
  }
  if (st == CCORD_OK && chars % 4 == 1) st = bad(err, "invalid base64 length");
  if (st != CCORD_OK) {
    ccord_free(raw);
    return st;
  }
  *out = raw;
  if (out_len) *out_len = n;
  return CCORD_OK;
}

ccord_status ccord_user_expressions_decode_b64(const char *b64, size_t len, ccord_user_expressions *out, ccord_error *err)
{
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: no output");
  *out = (ccord_user_expressions){};
  uint8_t *raw = nullptr;
  size_t raw_len = 0;
  ccord_status st = b64_decode_alloc(b64, len, &raw, &raw_len, err);
  if (st == CCORD_OK) st = ccord_user_expressions_decode(raw, raw_len, out, err);
  ccord_free(raw);
  return st;
}

// Parses the body of GET /users/@me/settings-proto/2 and borrows its "settings" base64 value from `tmp`.
static ccord_status settings_json_b64(const char *json, size_t len, ccord_arena *tmp, slice *out, ccord_error *err)
{
  ccord_json_limits lim = {.max_depth = 8, .max_nodes = 256, .max_string = CCORD_PROTO_MAX_INPUT * 2u};
  const ccord_json *root = ccord_json_parse(tmp, json, len, &lim, err);
  if (!root) return (err && err->code != CCORD_OK) ? err->code : CCORD_ERR_PARSE;
  size_t slen = 0;
  const ccord_json *node = ccord_json_type_of(root) == CCORD_JSON_OBJECT ? ccord_json_get(root, "settings") : nullptr;
  const char *s = node ? ccord_json_string(node, &slen) : nullptr;
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: response has no \"settings\" string");
  *out = (slice){(const uint8_t *)s, slen};
  return CCORD_OK;
}

ccord_status ccord_user_expressions_from_settings_json(const char *json, size_t len, ccord_user_expressions *out,
                                                       ccord_error *err)
{
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: no output");
  *out = (ccord_user_expressions){};
  if (!json) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: null json");
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  slice b64 = {nullptr, 0};
  ccord_status st = settings_json_b64(json, len, &tmp, &b64, err);
  if (st == CCORD_OK) st = ccord_user_expressions_decode_b64((const char *)b64.p, b64.n, out, err);
  ccord_arena_free(&tmp);
  return st;
}

// ---------------------------------------------------------------------------------------------------------------
// Favorite GIF writer
// ---------------------------------------------------------------------------------------------------------------

static ccord_status enc_status(ccord_status st, ccord_error *err)
{
  if (st == CCORD_ERR_NOMEM) return ccord_error_set(err, st, "settings proto: out of memory");
  if (st == CCORD_ERR_RANGE) return ccord_error_set(err, st, "settings proto: encoded output too large");
  return st;
}

static ccord_status pb_varint(ccord_buf *b, uint64_t v)
{
  uint8_t t[10];
  size_t n = 0;
  while (v >= 0x80u) {
    t[n++] = (uint8_t)(v | 0x80u);
    v >>= 7u;
  }
  t[n++] = (uint8_t)v;
  return ccord_buf_append(b, t, n);
}

static ccord_status pb_u(ccord_buf *b, uint32_t field, uint64_t v)
{
  ccord_status st = pb_varint(b, ((uint64_t)field << 3u) | 0u);
  if (st == CCORD_OK) st = pb_varint(b, v);
  return st;
}

static ccord_status pb_bytes(ccord_buf *b, uint32_t field, const void *p, size_t n)
{
  ccord_status st = pb_varint(b, ((uint64_t)field << 3u) | 2u);
  if (st == CCORD_OK) st = pb_varint(b, n);
  if (st == CCORD_OK) st = ccord_buf_append(b, p, n);
  return st;
}

static ccord_status validate_gif(const ccord_proto_gif *g, ccord_error *err)
{
  if (!g->url || !g->src) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: favorite gif has no url/src");
  size_t ulen = strlen(g->url), slen = strlen(g->src);
  if (ulen == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: favorite gif has an empty url");
  if (ulen > CCORD_PROTO_MAX_STRING || slen > CCORD_PROTO_MAX_STRING)
    return ccord_error_set(err, CCORD_ERR_RANGE, "settings proto: favorite gif string longer than %u bytes",
                           CCORD_PROTO_MAX_STRING);
  if (!ccord_utf8_valid(g->url, ulen) || !ccord_utf8_valid(g->src, slen))
    return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: favorite gif string is not valid UTF-8");
  return CCORD_OK;
}

// Appends the serialized settings to `out`: every top-level record except field 2 is copied byte-for-byte
// (tag included) from `existing`, then a fresh favorite_gifs container is written as root field 2.
static ccord_status encode_settings(const ccord_proto_gif *gifs, size_t count, const uint8_t *existing,
                                    size_t existing_len, ccord_buf *out, ccord_error *err)
{
  if (count > CCORD_PROTO_MAX_ITEMS)
    return ccord_error_set(err, CCORD_ERR_RANGE, "settings proto: more than %u favorite gifs", CCORD_PROTO_MAX_ITEMS);
  if (count > 0 && !gifs) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: no favorite gifs");
  if (existing_len > CCORD_PROTO_MAX_INPUT)
    return ccord_error_set(err, CCORD_ERR_RANGE, "settings proto: existing input larger than %u bytes",
                           CCORD_PROTO_MAX_INPUT);
  if (existing_len > 0 && !existing)
    return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: null existing input");
  for (size_t i = 0; i < count; i++) {
    ccord_status st = validate_gif(&gifs[i], err);
    if (st != CCORD_OK) return st;
  }

  slice r = {existing, existing_len};
  while (r.n > 0) {
    const uint8_t *start = r.p;
    size_t before = r.n;
    fld f;
    ccord_status st = next_field(&r, &f, err);
    if (st != CCORD_OK) return st;
    if (f.field == 2) continue;  // replaced below, including any malformed/legacy shape
    st = ccord_buf_append(out, start, before - r.n);
    if (st != CCORD_OK) return enc_status(st, err);
  }

  ccord_buf container, value, entry;
  ccord_buf_init(&container, 0);
  ccord_buf_init(&value, 0);
  ccord_buf_init(&entry, 0);
  ccord_status st = CCORD_OK;
  for (size_t i = 0; i < count && st == CCORD_OK; i++) {
    const ccord_proto_gif *g = &gifs[i];
    ccord_buf_clear(&value);
    ccord_buf_clear(&entry);
    st = pb_u(&value, 1, g->format);
    if (st == CCORD_OK) st = pb_bytes(&value, 2, g->src, strlen(g->src));
    if (st == CCORD_OK) st = pb_u(&value, 3, g->width);
    if (st == CCORD_OK) st = pb_u(&value, 4, g->height);
    if (st == CCORD_OK) st = pb_u(&value, 5, g->order);
    if (st == CCORD_OK) st = pb_bytes(&entry, 1, g->url, strlen(g->url));
    if (st == CCORD_OK) st = pb_bytes(&entry, 2, value.data ? value.data : "", value.len);
    if (st == CCORD_OK) st = pb_bytes(&container, 1, entry.data ? entry.data : "", entry.len);
  }
  if (st == CCORD_OK) st = pb_bytes(out, 2, container.data ? container.data : "", container.len);
  ccord_buf_free(&container);
  ccord_buf_free(&value);
  ccord_buf_free(&entry);
  return st == CCORD_OK ? CCORD_OK : enc_status(st, err);
}

static ccord_status b64_encode_alloc(const uint8_t *data, size_t len, char **out, ccord_error *err)
{
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  *out = nullptr;
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_status st = CCORD_OK;
  size_t i = 0;
  for (; i + 2 < len && st == CCORD_OK; i += 3) {
    uint32_t v = ((uint32_t)data[i] << 16u) | ((uint32_t)data[i + 1] << 8u) | data[i + 2];
    char q[4] = {tbl[(v >> 18u) & 63u], tbl[(v >> 12u) & 63u], tbl[(v >> 6u) & 63u], tbl[v & 63u]};
    st = ccord_buf_append(&b, q, sizeof q);
  }
  if (st == CCORD_OK && len - i == 1) {
    uint32_t v = (uint32_t)data[i] << 16u;
    char q[4] = {tbl[(v >> 18u) & 63u], tbl[(v >> 12u) & 63u], '=', '='};
    st = ccord_buf_append(&b, q, sizeof q);
  } else if (st == CCORD_OK && len - i == 2) {
    uint32_t v = ((uint32_t)data[i] << 16u) | ((uint32_t)data[i + 1] << 8u);
    char q[4] = {tbl[(v >> 18u) & 63u], tbl[(v >> 12u) & 63u], tbl[(v >> 6u) & 63u], '='};
    st = ccord_buf_append(&b, q, sizeof q);
  }
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return enc_status(st, err);
  }
  char *s = ccord_buf_take(&b, nullptr);
  if (!s) return ccord_error_set(err, CCORD_ERR_NOMEM, "settings proto: out of memory");
  *out = s;
  return CCORD_OK;
}

static ccord_status encode_b64(const ccord_proto_gif *gifs, size_t count, const uint8_t *existing, size_t existing_len,
                               char **out, ccord_error *err)
{
  *out = nullptr;
  ccord_buf raw;
  ccord_buf_init(&raw, 0);
  ccord_status st = encode_settings(gifs, count, existing, existing_len, &raw, err);
  if (st == CCORD_OK) st = b64_encode_alloc((const uint8_t *)raw.data, raw.len, out, err);
  ccord_buf_free(&raw);
  return st;
}

static ccord_status encode_json(const ccord_proto_gif *gifs, size_t count, const uint8_t *existing,
                                size_t existing_len, char **out, ccord_error *err)
{
  *out = nullptr;
  char *b64 = nullptr;
  ccord_status st = encode_b64(gifs, count, existing, existing_len, &b64, err);
  if (st != CCORD_OK) return st;
  ccord_buf b;
  ccord_buf_init(&b, 0);
  st = ccord_buf_append_str(&b, "{\"settings\":\"");
  if (st == CCORD_OK) st = ccord_buf_append_str(&b, b64);
  if (st == CCORD_OK) st = ccord_buf_append_str(&b, "\"}");
  ccord_free(b64);
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return enc_status(st, err);
  }
  char *s = ccord_buf_take(&b, nullptr);
  if (!s) return ccord_error_set(err, CCORD_ERR_NOMEM, "settings proto: out of memory");
  *out = s;
  return CCORD_OK;
}

ccord_status ccord_user_expressions_encode_gifs(const ccord_proto_gif *gifs, size_t count, const uint8_t *existing,
                                                size_t existing_len, uint8_t **out, size_t *out_len, ccord_error *err)
{
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: no output");
  *out = nullptr;
  if (out_len) *out_len = 0;
  ccord_buf raw;
  ccord_buf_init(&raw, 0);
  ccord_status st = encode_settings(gifs, count, existing, existing_len, &raw, err);
  if (st == CCORD_OK) {
    size_t n = 0;
    uint8_t *p = (uint8_t *)ccord_buf_take(&raw, &n);
    if (!p) {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "settings proto: out of memory");
    } else {
      *out = p;
      if (out_len) *out_len = n;
    }
  }
  ccord_buf_free(&raw);
  return st;
}

ccord_status ccord_user_expressions_encode_gifs_b64(const ccord_proto_gif *gifs, size_t count,
                                                    const char *existing_b64, size_t existing_b64_len, char **out,
                                                    ccord_error *err)
{
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: no output");
  *out = nullptr;
  if (!existing_b64 && existing_b64_len > 0)
    return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: null existing base64");
  uint8_t *raw = nullptr;
  size_t raw_len = 0;
  ccord_status st = CCORD_OK;
  if (existing_b64) st = b64_decode_alloc(existing_b64, existing_b64_len, &raw, &raw_len, err);
  if (st == CCORD_OK) st = encode_b64(gifs, count, raw, raw_len, out, err);
  ccord_free(raw);
  return st;
}

ccord_status ccord_user_expressions_encode_gifs_json(const ccord_proto_gif *gifs, size_t count,
                                                     const char *existing_json, size_t existing_json_len, char **out,
                                                     ccord_error *err)
{
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: no output");
  *out = nullptr;
  if (!existing_json && existing_json_len > 0)
    return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: null existing json");
  uint8_t *raw = nullptr;
  size_t raw_len = 0;
  ccord_status st = CCORD_OK;
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  if (existing_json) {
    slice b64 = {nullptr, 0};
    st = settings_json_b64(existing_json, existing_json_len, &tmp, &b64, err);
    if (st == CCORD_OK) st = b64_decode_alloc((const char *)b64.p, b64.n, &raw, &raw_len, err);
  }
  if (st == CCORD_OK) st = encode_json(gifs, count, raw, raw_len, out, err);
  ccord_arena_free(&tmp);
  ccord_free(raw);
  return st;
}

void ccord_user_expressions_free(ccord_user_expressions *x)
{
  if (!x) return;
  ccord_arena_free(&x->arena);
  *x = (ccord_user_expressions){};
}

// ---------------------------------------------------------------------------------------------------------------
// Status (type-1 settings proto, StatusSettings = root field 11). Layout: see the header comment.
// ---------------------------------------------------------------------------------------------------------------

// Copies a length-delimited string field into a fixed buffer (NUL-terminated), rejecting NUL / bad UTF-8 / overflow.
static ccord_status status_copy_str(const fld *f, char *dst, size_t cap, ccord_error *err)
{
  slice s = {nullptr, 0};
  ccord_status st = take_string(f, &s, err);
  if (st != CCORD_OK) return st;
  if (s.n >= cap) return ccord_error_set(err, CCORD_ERR_RANGE, "settings proto: status string longer than %zu bytes", cap - 1);
  if (s.n) memcpy(dst, s.p, s.n);
  dst[s.n] = '\0';
  return CCORD_OK;
}

// A StringValue wrapper {1: string}; an empty message is the empty string.
static ccord_status status_string_value(slice msg, char *dst, size_t cap, ccord_error *err)
{
  dst[0] = '\0';
  while (msg.n > 0) {
    fld f;
    ccord_status st = next_field(&msg, &f, err);
    if (st != CCORD_OK) return st;
    if (f.field == 1 && f.wt == 2) {
      st = status_copy_str(&f, dst, cap, err);
      if (st != CCORD_OK) return st;
    }
  }
  return CCORD_OK;
}

static ccord_status parse_custom_status(slice msg, ccord_proto_status *o, ccord_error *err)
{
  while (msg.n > 0) {
    fld f;
    ccord_status st = next_field(&msg, &f, err);
    if (st != CCORD_OK) return st;
    if (f.field == 1 && f.wt == 2) st = status_copy_str(&f, o->custom_text, sizeof o->custom_text, err);
    else if (f.field == 2 && f.wt == 1) o->emoji_id = f.v;
    else if (f.field == 3 && f.wt == 2) st = status_copy_str(&f, o->emoji_name, sizeof o->emoji_name, err);
    else if (f.field == 4 && f.wt == 1) o->custom_expires_at_ms = f.v;
    if (st != CCORD_OK) return st;
  }
  return CCORD_OK;
}

static ccord_status parse_status_settings(slice msg, ccord_proto_status *o, ccord_error *err)
{
  while (msg.n > 0) {
    fld f;
    ccord_status st = next_field(&msg, &f, err);
    if (st != CCORD_OK) return st;
    if (f.field == 1 && f.wt == 2) {
      o->has_status = true;
      st = status_string_value(f.data, o->status, sizeof o->status, err);
    } else if (f.field == 2 && f.wt == 2) {
      o->has_custom = true;
      st = parse_custom_status(f.data, o, err);
    } else if (f.field == 4 && f.wt == 1) {
      o->status_expires_at_ms = f.v;
    }
    if (st != CCORD_OK) return st;
  }
  return CCORD_OK;
}

ccord_status ccord_user_status_decode(const uint8_t *buf, size_t len, ccord_proto_status *out, ccord_error *err)
{
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: no output");
  *out = (ccord_proto_status){};
  if (len > 0 && !buf) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: null buffer");
  if (len > CCORD_PROTO_MAX_INPUT)
    return ccord_error_set(err, CCORD_ERR_RANGE, "settings proto: input larger than %u bytes", CCORD_PROTO_MAX_INPUT);
  slice r = {buf, len};
  while (r.n > 0) {
    fld f;
    ccord_status st = next_field(&r, &f, err);
    if (st == CCORD_OK && f.field == 11 && f.wt == 2) {
      out->present = true;
      st = parse_status_settings(f.data, out, err);
    }
    if (st != CCORD_OK) {
      *out = (ccord_proto_status){};
      return st;
    }
  }
  return CCORD_OK;
}

ccord_status ccord_user_status_decode_b64(const char *b64, size_t len, ccord_proto_status *out, ccord_error *err)
{
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: no output");
  *out = (ccord_proto_status){};
  uint8_t *raw = nullptr;
  size_t raw_len = 0;
  ccord_status st = b64_decode_alloc(b64, len, &raw, &raw_len, err);
  if (st == CCORD_OK) st = ccord_user_status_decode(raw, raw_len, out, err);
  ccord_free(raw);
  return st;
}

static ccord_status pb_fixed64(ccord_buf *b, uint32_t field, uint64_t v)
{
  uint8_t t[8];
  for (unsigned i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8u * i));
  ccord_status st = pb_varint(b, ((uint64_t)field << 3u) | 1u);
  if (st == CCORD_OK) st = ccord_buf_append(b, t, sizeof t);
  return st;
}

// Writes `field` as a message holding one string (StringValue) or, with wrap == false, as a plain string.
// Empty strings are omitted for plain strings and written as an empty StringValue for wrappers.
static ccord_status pb_string(ccord_buf *b, uint32_t field, const char *s, bool wrap)
{
  size_t n = strlen(s);
  if (!wrap) return n ? pb_bytes(b, field, s, n) : CCORD_OK;
  ccord_buf inner;
  ccord_buf_init(&inner, 0);
  ccord_status st = n ? pb_bytes(&inner, 1, s, n) : CCORD_OK;
  if (st == CCORD_OK) st = pb_bytes(b, field, inner.data ? inner.data : "", inner.len);
  ccord_buf_free(&inner);
  return st;
}

static ccord_status status_check_str(const char *s, size_t cap, const char *what, ccord_error *err)
{
  const char *nul = memchr(s, '\0', cap);
  if (!nul) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: %s is not terminated", what);
  size_t n = (size_t)(nul - s);
  if (!ccord_utf8_valid(s, n)) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: %s is not valid UTF-8", what);
  return CCORD_OK;
}

static ccord_status status_validate(const ccord_proto_status *s, ccord_error *err)
{
  if (s->has_status) {
    static const char *const ok[] = {"online", "idle", "dnd", "invisible"};
    bool found = false;
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
      if (strncmp(s->status, ok[i], sizeof s->status) == 0) found = true;
    if (!found) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: unknown status string");
  }
  ccord_status st = status_check_str(s->custom_text, sizeof s->custom_text, "custom status text", err);
  if (st == CCORD_OK) st = status_check_str(s->emoji_name, sizeof s->emoji_name, "custom status emoji name", err);
  return st;
}

// Appends every record of `src` except the field numbers in `skip` (a small list) byte for byte to `out`.
static ccord_status copy_fields_except(slice src, const uint32_t *skip, size_t nskip, ccord_buf *out, ccord_error *err)
{
  while (src.n > 0) {
    const uint8_t *start = src.p;
    size_t before = src.n;
    fld f;
    ccord_status st = next_field(&src, &f, err);
    if (st != CCORD_OK) return st;
    bool drop = false;
    for (size_t i = 0; i < nskip; i++)
      if (f.field == skip[i]) drop = true;
    if (drop) continue;
    st = ccord_buf_append(out, start, before - src.n);
    if (st != CCORD_OK) return enc_status(st, err);
  }
  return CCORD_OK;
}

static ccord_status status_patch_raw(const uint8_t *base, size_t base_len, const ccord_proto_status *s, ccord_buf *out,
                                     ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: no status");
  if (base_len > CCORD_PROTO_MAX_INPUT)
    return ccord_error_set(err, CCORD_ERR_RANGE, "settings proto: existing input larger than %u bytes",
                           CCORD_PROTO_MAX_INPUT);
  if (base_len > 0 && !base) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: null existing input");
  ccord_status st = status_validate(s, err);
  if (st != CCORD_OK) return st;

  // 1. Root: copy everything but field 11; the status payloads (repeated records merge by concatenation) are gathered.
  ccord_buf old, msg, custom;
  ccord_buf_init(&old, 0);
  ccord_buf_init(&msg, 0);
  ccord_buf_init(&custom, 0);
  slice r = {base, base_len};
  while (r.n > 0 && st == CCORD_OK) {
    const uint8_t *start = r.p;
    size_t before = r.n;
    fld f;
    st = next_field(&r, &f, err);
    if (st != CCORD_OK) break;
    if (f.field == 11) {
      if (f.wt != 2) {
        st = bad(err, "status field has the wrong wire type");
        break;
      }
      st = ccord_buf_append(&old, f.data.p, f.data.n);
    } else {
      st = ccord_buf_append(out, start, before - r.n);
    }
    if (st != CCORD_OK) st = enc_status(st, err);
  }

  // 2. New StatusSettings: fresh fields 1, 2, then the untouched old subfields (3, 5, unknown), then 4.
  if (st == CCORD_OK && s->has_status) st = pb_string(&msg, 1, s->status, true);
  if (st == CCORD_OK && s->has_custom) {
    st = pb_string(&custom, 1, s->custom_text, false);
    if (st == CCORD_OK && s->emoji_id) st = pb_fixed64(&custom, 2, s->emoji_id);
    if (st == CCORD_OK) st = pb_string(&custom, 3, s->emoji_name, false);
    if (st == CCORD_OK && s->custom_expires_at_ms) st = pb_fixed64(&custom, 4, s->custom_expires_at_ms);
    if (st == CCORD_OK) st = pb_bytes(&msg, 2, custom.data ? custom.data : "", custom.len);
  }
  if (st == CCORD_OK) {
    static const uint32_t replaced[] = {1, 2, 4};
    st = copy_fields_except((slice){(const uint8_t *)old.data, old.len}, replaced, 3, &msg, err);
  }
  if (st == CCORD_OK && s->status_expires_at_ms) st = pb_fixed64(&msg, 4, s->status_expires_at_ms);
  if (st != CCORD_OK) st = enc_status(st, err);
  if (st == CCORD_OK && msg.len > 0) st = enc_status(pb_bytes(out, 11, msg.data, msg.len), err);
  ccord_buf_free(&old);
  ccord_buf_free(&msg);
  ccord_buf_free(&custom);
  return st;
}

ccord_status ccord_user_status_patch(const uint8_t *base, size_t base_len, const ccord_proto_status *st, uint8_t **out,
                                     size_t *out_len, ccord_error *err)
{
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: no output");
  *out = nullptr;
  if (out_len) *out_len = 0;
  ccord_buf raw;
  ccord_buf_init(&raw, 0);
  ccord_status rc = status_patch_raw(base, base_len, st, &raw, err);
  if (rc == CCORD_OK) {
    size_t n = 0;
    uint8_t *p = (uint8_t *)ccord_buf_take(&raw, &n);
    if (!p) {
      rc = ccord_error_set(err, CCORD_ERR_NOMEM, "settings proto: out of memory");
    } else {
      *out = p;
      if (out_len) *out_len = n;
    }
  }
  ccord_buf_free(&raw);
  return rc;
}

ccord_status ccord_user_status_patch_b64(const char *base_b64, size_t base_b64_len, const ccord_proto_status *st,
                                         char **out, ccord_error *err)
{
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: no output");
  *out = nullptr;
  if (!base_b64 && base_b64_len > 0)
    return ccord_error_set(err, CCORD_ERR_INVALID, "settings proto: null existing base64");
  uint8_t *raw = nullptr, *res = nullptr;
  size_t raw_len = 0, res_len = 0;
  ccord_status rc = CCORD_OK;
  if (base_b64) rc = b64_decode_alloc(base_b64, base_b64_len, &raw, &raw_len, err);
  if (rc == CCORD_OK) rc = ccord_user_status_patch(raw, raw_len, st, &res, &res_len, err);
  if (rc == CCORD_OK) rc = b64_encode_alloc(res, res_len, out, err);
  ccord_free(raw);
  ccord_free(res);
  return rc;
}
