// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/net/discord_proto.h>

#include "ccord_test.h"

// ---- tiny protobuf encoder (synthetic fixtures only) -------------------------------------------------------------

typedef struct bb {
  uint8_t *d;
  size_t n, cap;
} bb;

static void bb_put(bb *b, const void *p, size_t n)
{
  if (b->n + n > b->cap) {
    b->cap = (b->n + n) * 2 + 64;
    b->d = realloc(b->d, b->cap);
    if (!b->d) abort();
  }
  if (n) memcpy(b->d + b->n, p, n);
  b->n += n;
}

static void bb_free(bb *b)
{
  free(b->d);
  *b = (bb){};
}

static void put_varint(bb *b, uint64_t v)
{
  uint8_t t[10];
  size_t n = 0;
  while (v >= 0x80) {
    t[n++] = (uint8_t)(v | 0x80);
    v >>= 7;
  }
  t[n++] = (uint8_t)v;
  bb_put(b, t, n);
}

static void put_tag(bb *b, uint32_t field, uint32_t wt) { put_varint(b, ((uint64_t)field << 3) | wt); }

static void put_u(bb *b, uint32_t field, uint64_t v)
{
  put_tag(b, field, 0);
  put_varint(b, v);
}

static void put_i32(bb *b, uint32_t field, int32_t v)
{
  put_tag(b, field, 0);
  put_varint(b, (uint64_t)(int64_t)v);  // negative int32 -> 10-byte sign-extended varint, like protoc
}

static void put_fixed64(bb *b, uint32_t field, uint64_t v)
{
  put_tag(b, field, 1);
  uint8_t t[8];
  for (unsigned i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8 * i));
  bb_put(b, t, 8);
}

static void put_bytes(bb *b, uint32_t field, const void *p, size_t n)
{
  put_tag(b, field, 2);
  put_varint(b, n);
  bb_put(b, p, n);
}

static void put_str(bb *b, uint32_t field, const char *s) { put_bytes(b, field, s, strlen(s)); }

static void put_msg(bb *b, uint32_t field, bb *m)
{
  put_bytes(b, field, m->d, m->n);
  bb_free(m);
}

static void put_item(bb *entry, uint32_t total, int32_t frecency, int32_t score, bool with_recent)
{
  bb v = {};
  put_u(&v, 1, total);
  if (with_recent) {
    bb packed = {};
    put_varint(&packed, 1700000000000ull);
    put_varint(&packed, 1700000005000ull);
    put_bytes(&v, 2, packed.d, packed.n);
    bb_free(&packed);
  }
  put_i32(&v, 3, frecency);
  put_i32(&v, 4, score);
  put_tag(&v, 77, 5);  // unknown fixed32 inside the item
  uint8_t z[4] = {1, 2, 3, 4};
  bb_put(&v, z, 4);
  put_msg(entry, 2, &v);
}

static void put_emoji_entry(bb *map, const char *key, uint32_t total, int32_t frec, int32_t score, bool key_first, bool recent)
{
  bb e = {};
  if (key_first) {
    put_str(&e, 1, key);
    put_item(&e, total, frec, score, recent);
  } else {
    put_item(&e, total, frec, score, recent);
    put_str(&e, 1, key);
  }
  put_msg(map, 1, &e);
}

static void put_gif_entry(bb *gifs, const char *url, const char *src, uint32_t fmt, uint32_t w, uint32_t h, uint32_t order,
                          bool value_first)
{
  bb v = {};
  put_u(&v, 1, fmt);
  put_str(&v, 2, src);
  put_u(&v, 3, w);
  put_u(&v, 4, h);
  put_u(&v, 5, order);
  bb e = {};
  if (value_first) {
    put_msg(&e, 2, &v);
    put_str(&e, 1, url);
  } else {
    put_str(&e, 1, url);
    put_msg(&e, 2, &v);
  }
  put_msg(gifs, 1, &e);
}

static void build_fixture(bb *out)
{
  // versions (field 1) - skipped by the reader
  bb ver = {};
  put_u(&ver, 1, 7);
  put_u(&ver, 2, 1);
  put_u(&ver, 3, 9);
  put_msg(out, 1, &ver);

  put_u(out, 99, 12345);  // unknown top-level varint

  bb gifs = {};
  put_gif_entry(&gifs, "https://example.invalid/gifs/a", "https://media.example.invalid/a.mp4", 2, 320, 240, 5, false);
  put_u(&gifs, 2, 1);  // hide_tooltip
  put_gif_entry(&gifs, "https://example.invalid/gifs/b", "https://media.example.invalid/b.gif", 1, 498, 280, 2, true);
  put_msg(out, 2, &gifs);

  bb st1 = {};
  uint64_t packed_ids[2] = {111, 222};
  bb pk = {};
  for (int i = 0; i < 2; i++) {
    for (unsigned k = 0; k < 8; k++) {
      uint8_t byte = (uint8_t)(packed_ids[i] >> (8 * k));
      bb_put(&pk, &byte, 1);
    }
  }
  put_bytes(&st1, 1, pk.d, pk.n);
  bb_free(&pk);
  put_msg(out, 3, &st1);

  bb st2 = {};  // a second occurrence of the same message with an unpacked record: appended
  put_fixed64(&st2, 1, 333);
  put_str(&st2, 5, "unknown string inside favorite stickers");
  put_msg(out, 3, &st2);

  bb sf = {};
  for (int i = 0; i < 3; i++) {
    uint64_t ids[3] = {111, 222, 111};
    int32_t scores[3] = {10, 40, 25};  // 111 appears twice: the later (score 25) wins
    bb e = {};
    put_fixed64(&e, 1, ids[i]);
    put_item(&e, (uint32_t)(i + 1), 100 + i, scores[i], true);
    put_msg(&sf, 1, &e);
  }
  put_msg(out, 4, &sf);

  bb fe = {};
  put_str(&fe, 1, "\xF0\x9F\x94\xA5");  // fire
  put_str(&fe, 1, "123456789012345678");
  put_str(&fe, 1, "");  // empty strings are dropped
  put_msg(out, 5, &fe);

  bb ef = {};
  put_emoji_entry(&ef, "\xF0\x9F\x98\x80", 3, 120, 50, true, false);
  put_emoji_entry(&ef, "123456789012345678", 9, -5, 80, false, true);
  put_emoji_entry(&ef, "wave", 1, 10, 50, true, false);
  put_msg(out, 6, &ef);

  bb oth = {};  // application_command_frecency (field 7): ignored
  put_emoji_entry(&oth, "ignored", 1, 1, 1, true, false);
  put_msg(out, 7, &oth);
}

// ---- base64 encoder for tests ---------------------------------------------------------------------------------

static char *b64_encode(const uint8_t *d, size_t n, bool pad, bool url)
{
  const char *std = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const char *alt = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  const char *al = url ? alt : std;
  char *o = malloc(n / 3 * 4 + 8);
  if (!o) abort();
  size_t k = 0;
  size_t i = 0;
  for (; i + 2 < n; i += 3) {
    uint32_t v = ((uint32_t)d[i] << 16) | ((uint32_t)d[i + 1] << 8) | d[i + 2];
    o[k++] = al[(v >> 18) & 63];
    o[k++] = al[(v >> 12) & 63];
    o[k++] = al[(v >> 6) & 63];
    o[k++] = al[v & 63];
  }
  if (n - i == 1) {
    uint32_t v = (uint32_t)d[i] << 16;
    o[k++] = al[(v >> 18) & 63];
    o[k++] = al[(v >> 12) & 63];
    if (pad) { o[k++] = '='; o[k++] = '='; }
  } else if (n - i == 2) {
    uint32_t v = ((uint32_t)d[i] << 16) | ((uint32_t)d[i + 1] << 8);
    o[k++] = al[(v >> 18) & 63];
    o[k++] = al[(v >> 12) & 63];
    o[k++] = al[(v >> 6) & 63];
    if (pad) o[k++] = '=';
  }
  o[k] = 0;
  return o;
}

// ---- checks ----------------------------------------------------------------------------------------------------

static void check_fixture(const ccord_user_expressions *x)
{
  // gifs sorted by order ascending: b (2), a (5)
  CT_CHECK(x->gif_count == 2);
  if (x->gif_count == 2) {
    CT_CHECK(strcmp(x->gifs[0].url, "https://example.invalid/gifs/b") == 0);
    CT_CHECK(strcmp(x->gifs[0].src, "https://media.example.invalid/b.gif") == 0);
    CT_CHECK(x->gifs[0].format == 1 && x->gifs[0].width == 498 && x->gifs[0].height == 280 && x->gifs[0].order == 2);
    CT_CHECK(strcmp(x->gifs[1].url, "https://example.invalid/gifs/a") == 0);
    CT_CHECK(strcmp(x->gifs[1].src, "https://media.example.invalid/a.mp4") == 0);
    CT_CHECK(x->gifs[1].format == 2 && x->gifs[1].width == 320 && x->gifs[1].height == 240 && x->gifs[1].order == 5);
  }

  // favorite stickers: packed 111,222 then unpacked 333 (wire order)
  CT_CHECK(x->favorite_sticker_count == 3);
  if (x->favorite_sticker_count == 3) {
    CT_CHECK(x->favorite_stickers[0] == 111 && x->favorite_stickers[1] == 222 && x->favorite_stickers[2] == 333);
  }

  // sticker frecency: 222 (score 40), 111 (score 25, later duplicate wins)
  CT_CHECK(x->sticker_frecency_count == 2);
  if (x->sticker_frecency_count == 2) {
    CT_CHECK(x->sticker_frecency[0].id == 222 && strcmp(x->sticker_frecency[0].key, "222") == 0);
    CT_CHECK(x->sticker_frecency[0].score == 40 && x->sticker_frecency[0].frecency == 101 && x->sticker_frecency[0].total_uses == 2);
    CT_CHECK(x->sticker_frecency[1].id == 111 && strcmp(x->sticker_frecency[1].key, "111") == 0);
    CT_CHECK(x->sticker_frecency[1].score == 25 && x->sticker_frecency[1].frecency == 102 && x->sticker_frecency[1].total_uses == 3);
  }

  // favorite emojis: wire order, empty dropped
  CT_CHECK(x->favorite_emoji_count == 2);
  if (x->favorite_emoji_count == 2) {
    CT_CHECK(strcmp(x->favorite_emojis[0], "\xF0\x9F\x94\xA5") == 0);
    CT_CHECK(strcmp(x->favorite_emojis[1], "123456789012345678") == 0);
  }

  // emoji frecency by score desc, then frecency desc: custom(80), unicode(50, frec 120), wave(50, frec 10)
  CT_CHECK(x->emoji_frecency_count == 3);
  if (x->emoji_frecency_count == 3) {
    CT_CHECK(strcmp(x->emoji_frecency[0].key, "123456789012345678") == 0 && x->emoji_frecency[0].id == 123456789012345678ull);
    CT_CHECK(x->emoji_frecency[0].frecency == -5 && x->emoji_frecency[0].score == 80 && x->emoji_frecency[0].total_uses == 9);
    CT_CHECK(strcmp(x->emoji_frecency[1].key, "\xF0\x9F\x98\x80") == 0 && x->emoji_frecency[1].id == 0);
    CT_CHECK(x->emoji_frecency[1].frecency == 120);
    CT_CHECK(strcmp(x->emoji_frecency[2].key, "wave") == 0 && x->emoji_frecency[2].id == 0);
  }
}

static void test_decode_fixture(void)
{
  bb b = {};
  build_fixture(&b);
  ccord_user_expressions x;
  ccord_error err;
  CT_CHECK(ccord_user_expressions_decode(b.d, b.n, &x, &err) == CCORD_OK);
  check_fixture(&x);
  ccord_user_expressions_free(&x);
  ccord_user_expressions_free(&x);  // idempotent
  bb_free(&b);
}

static void test_empty_and_args(void)
{
  ccord_user_expressions x;
  ccord_error err;
  CT_CHECK(ccord_user_expressions_decode(nullptr, 0, &x, &err) == CCORD_OK);
  CT_CHECK(x.gif_count == 0 && x.gifs == nullptr && x.emoji_frecency_count == 0 && x.favorite_emojis == nullptr);
  ccord_user_expressions_free(&x);
  uint8_t one = 0x0a;
  CT_CHECK(ccord_user_expressions_decode(nullptr, 1, &x, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_user_expressions_decode(&one, 1, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_user_expressions_decode(&one, 1, &x, nullptr) == CCORD_ERR_INVALID);  // lone tag: truncated
  ccord_user_expressions_free(nullptr);
}

static void test_packed_and_unpacked_equivalence(void)
{
  bb a = {};
  bb m = {};
  put_fixed64(&m, 1, 5);
  put_fixed64(&m, 1, 0xfedcba9876543210ull);
  put_msg(&a, 3, &m);
  ccord_user_expressions x;
  ccord_error err;
  CT_CHECK(ccord_user_expressions_decode(a.d, a.n, &x, &err) == CCORD_OK);
  CT_CHECK(x.favorite_sticker_count == 2 && x.favorite_stickers[0] == 5 && x.favorite_stickers[1] == 0xfedcba9876543210ull);
  ccord_user_expressions_free(&x);
  bb_free(&a);

  // packed with a length that is not a multiple of 8 is malformed
  bb b2 = {};
  bb m2 = {};
  put_bytes(&m2, 1, "123456789", 9);
  put_msg(&b2, 3, &m2);
  CT_CHECK(ccord_user_expressions_decode(b2.d, b2.n, &x, &err) == CCORD_ERR_INVALID);
  bb_free(&b2);
}

static void test_truncation_errors(void)
{
  bb b = {};
  build_fixture(&b);
  ccord_user_expressions x;
  ccord_error err;
  // The final field is length-delimited, so any strict prefix that cuts it must be rejected.
  CT_CHECK(ccord_user_expressions_decode(b.d, b.n - 1, &x, &err) == CCORD_ERR_INVALID);
  CT_CHECK(err.code == CCORD_ERR_INVALID && err.message[0] != 0);
  // Truncation at every offset: never crashes, never reads out of bounds (ASan), and success implies a clean cut.
  size_t ok = 0;
  size_t failed = 0;
  for (size_t cut = 0; cut < b.n; cut++) {
    // copy into an exactly-sized heap block so ASan sees any over-read
    uint8_t *copy = malloc(cut ? cut : 1);
    if (!copy) abort();
    if (cut) memcpy(copy, b.d, cut);
    ccord_status st = ccord_user_expressions_decode(copy, cut, &x, &err);
    if (st == CCORD_OK) {
      ok++;
      ccord_user_expressions_free(&x);
    } else {
      failed++;
      CT_CHECK(st == CCORD_ERR_INVALID);
    }
    free(copy);
  }
  CT_CHECK(failed > b.n / 2);
  CT_CHECK(ok >= 1);  // cut == 0 and top-level boundaries are valid empty/partial messages
  bb_free(&b);
}

static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static uint64_t rng_next(void)
{
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}

static void test_mutation_fuzz(void)
{
  bb b = {};
  build_fixture(&b);
  ccord_user_expressions x;
  ccord_error err;
  size_t runs = 0;
  // single-byte flips at every offset with several patterns
  static const uint8_t pats[] = {0x01, 0x80, 0xff, 0x7f, 0x55};
  for (size_t i = 0; i < b.n; i++) {
    for (size_t p = 0; p < sizeof pats; p++) {
      uint8_t *copy = malloc(b.n);
      if (!copy) abort();
      memcpy(copy, b.d, b.n);
      copy[i] ^= pats[p];
      if (ccord_user_expressions_decode(copy, b.n, &x, &err) == CCORD_OK) ccord_user_expressions_free(&x);
      free(copy);
      runs++;
    }
  }
  // random multi-byte mutations, inserted/deleted bytes, random garbage
  for (unsigned iter = 0; iter < 6000; iter++) {
    size_t len = b.n;
    uint8_t *copy = malloc(len + 16);
    if (!copy) abort();
    memcpy(copy, b.d, len);
    unsigned muts = 1 + (unsigned)(rng_next() % 6);
    for (unsigned m = 0; m < muts; m++) {
      uint64_t r = rng_next();
      size_t pos = (size_t)(r >> 8) % len;
      switch (r % 4) {
        case 0: copy[pos] = (uint8_t)(r >> 32); break;
        case 1: copy[pos] ^= (uint8_t)(1u << ((r >> 40) % 8)); break;
        case 2: // delete a byte
          memmove(copy + pos, copy + pos + 1, len - pos - 1);
          len--;
          break;
        default: // truncate
          len = pos + 1;
          break;
      }
      if (len == 0) len = 1;
    }
    // exact-size heap copy so ASan catches over-reads
    uint8_t *exact = malloc(len);
    if (!exact) abort();
    memcpy(exact, copy, len);
    if (ccord_user_expressions_decode(exact, len, &x, &err) == CCORD_OK) ccord_user_expressions_free(&x);
    free(exact);
    free(copy);
    runs++;
  }
  for (unsigned iter = 0; iter < 2000; iter++) {
    size_t len = (size_t)(rng_next() % 96);
    uint8_t *g = malloc(len ? len : 1);
    if (!g) abort();
    for (size_t i = 0; i < len; i++) g[i] = (uint8_t)rng_next();
    if (ccord_user_expressions_decode(g, len, &x, &err) == CCORD_OK) ccord_user_expressions_free(&x);
    free(g);
    runs++;
  }
  CT_CHECK(runs > 8000);
  bb_free(&b);
}

static void test_alloc_failures(void)
{
  bb b = {};
  build_fixture(&b);
  ccord_user_expressions x;
  ccord_error err;
  bool saw_nomem = false;
  for (long n = 0; n < 400; n++) {
    ccord_alloc_test_fail_after(n);
    ccord_status st = ccord_user_expressions_decode(b.d, b.n, &x, &err);
    ccord_alloc_test_fail_after(-1);
    if (st == CCORD_OK) {
      ccord_user_expressions_free(&x);
      break;  // all allocations succeeded: the budget is larger than the need
    }
    CT_CHECK(st == CCORD_ERR_NOMEM);
    saw_nomem = true;
  }
  CT_CHECK(saw_nomem);
  bb_free(&b);
}

static void test_unknown_and_bad_wire_types(void)
{
  ccord_user_expressions x;
  ccord_error err;
  bb b = {};
  // unknown fields of every supported wire type before and after a real message
  put_u(&b, 50, 1);
  put_fixed64(&b, 51, 2);
  put_str(&b, 52, "ignored");
  put_tag(&b, 53, 5);
  uint8_t four[4] = {1, 2, 3, 4};
  bb_put(&b, four, 4);
  bb fe = {};
  put_str(&fe, 1, "ok");
  put_u(&fe, 9, 3);  // unknown field inside the message
  put_msg(&b, 5, &fe);
  put_u(&b, 5, 1);   // known field number with the wrong wire type: treated as unknown
  CT_CHECK(ccord_user_expressions_decode(b.d, b.n, &x, &err) == CCORD_OK);
  CT_CHECK(x.favorite_emoji_count == 1 && strcmp(x.favorite_emojis[0], "ok") == 0);
  ccord_user_expressions_free(&x);
  bb_free(&b);

  // group start / end / reserved wire types are rejected, not skipped
  const uint8_t grp[] = {(uint8_t)((60 << 3) | 3), 0x01, (uint8_t)((60 << 3) | 4)};
  CT_CHECK(ccord_user_expressions_decode(grp, sizeof grp, &x, &err) == CCORD_ERR_INVALID);
  const uint8_t w6[] = {(uint8_t)((60 << 3) | 6), 0x00};
  CT_CHECK(ccord_user_expressions_decode(w6, sizeof w6, &x, &err) == CCORD_ERR_INVALID);
  const uint8_t w7[] = {(uint8_t)((60 << 3) | 7), 0x00};
  CT_CHECK(ccord_user_expressions_decode(w7, sizeof w7, &x, &err) == CCORD_ERR_INVALID);
  // field number 0
  const uint8_t f0[] = {0x00, 0x01};
  CT_CHECK(ccord_user_expressions_decode(f0, sizeof f0, &x, &err) == CCORD_ERR_INVALID);
  // varint longer than 10 bytes / overflowing 64 bits
  uint8_t ov[12];
  memset(ov, 0xff, sizeof ov);
  ov[0] = (uint8_t)(50 << 3);
  CT_CHECK(ccord_user_expressions_decode(ov, sizeof ov, &x, &err) == CCORD_ERR_INVALID);
  const uint8_t ov10[] = {(uint8_t)(50 << 3), 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02};
  CT_CHECK(ccord_user_expressions_decode(ov10, sizeof ov10, &x, &err) == CCORD_ERR_INVALID);
  // length larger than the remaining buffer (and larger than SIZE_MAX-ish values)
  const uint8_t big[] = {(uint8_t)((50 << 3) | 2), 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 'x'};
  CT_CHECK(ccord_user_expressions_decode(big, sizeof big, &x, &err) == CCORD_ERR_INVALID);
}

static void test_limits_and_string_validation(void)
{
  ccord_user_expressions x;
  ccord_error err;

  // string with an embedded NUL
  bb b = {};
  bb fe = {};
  put_bytes(&fe, 1, "a\0b", 3);
  put_msg(&b, 5, &fe);
  CT_CHECK(ccord_user_expressions_decode(b.d, b.n, &x, &err) == CCORD_ERR_INVALID);
  bb_free(&b);

  // invalid UTF-8
  fe = (bb){};
  put_bytes(&fe, 1, "\xff\xfe", 2);
  put_msg(&b, 5, &fe);
  CT_CHECK(ccord_user_expressions_decode(b.d, b.n, &x, &err) == CCORD_ERR_INVALID);
  bb_free(&b);

  // 4 KiB string is fine, 4 KiB + 1 is a range error
  char *s = malloc(CCORD_PROTO_MAX_STRING + 1);
  if (!s) abort();
  memset(s, 'e', CCORD_PROTO_MAX_STRING + 1);
  fe = (bb){};
  put_bytes(&fe, 1, s, CCORD_PROTO_MAX_STRING);
  put_msg(&b, 5, &fe);
  CT_CHECK(ccord_user_expressions_decode(b.d, b.n, &x, &err) == CCORD_OK);
  CT_CHECK(x.favorite_emoji_count == 1 && strlen(x.favorite_emojis[0]) == CCORD_PROTO_MAX_STRING);
  ccord_user_expressions_free(&x);
  bb_free(&b);
  fe = (bb){};
  put_bytes(&fe, 1, s, CCORD_PROTO_MAX_STRING + 1);
  put_msg(&b, 5, &fe);
  CT_CHECK(ccord_user_expressions_decode(b.d, b.n, &x, &err) == CCORD_ERR_RANGE);
  bb_free(&b);
  free(s);

  // item caps: exactly the limit passes, one more is a range error
  fe = (bb){};
  for (unsigned i = 0; i < CCORD_PROTO_MAX_ITEMS; i++) put_str(&fe, 1, "x");
  put_msg(&b, 5, &fe);
  CT_CHECK(ccord_user_expressions_decode(b.d, b.n, &x, &err) == CCORD_OK);
  CT_CHECK(x.favorite_emoji_count == CCORD_PROTO_MAX_ITEMS);
  ccord_user_expressions_free(&x);
  bb_free(&b);
  fe = (bb){};
  for (unsigned i = 0; i < CCORD_PROTO_MAX_ITEMS + 1; i++) put_str(&fe, 1, "x");
  put_msg(&b, 5, &fe);
  CT_CHECK(ccord_user_expressions_decode(b.d, b.n, &x, &err) == CCORD_ERR_RANGE);
  bb_free(&b);

  // sticker ids: a packed run over the cap
  uint8_t *ids = calloc(CCORD_PROTO_MAX_ITEMS + 1, 8);
  if (!ids) abort();
  fe = (bb){};
  put_bytes(&fe, 1, ids, (CCORD_PROTO_MAX_ITEMS + 1) * 8u);
  put_msg(&b, 3, &fe);
  CT_CHECK(ccord_user_expressions_decode(b.d, b.n, &x, &err) == CCORD_ERR_RANGE);
  bb_free(&b);
  free(ids);

  // input larger than 1 MiB
  size_t huge = CCORD_PROTO_MAX_INPUT + 1;
  uint8_t *h = calloc(huge, 1);
  if (!h) abort();
  CT_CHECK(ccord_user_expressions_decode(h, huge, &x, &err) == CCORD_ERR_RANGE);
  free(h);

  // many duplicate keys collapse and do not count against anything but the item cap
  fe = (bb){};
  for (int i = 0; i < 500; i++) put_emoji_entry(&fe, "same", (uint32_t)i, 0, i, true, false);
  put_msg(&b, 6, &fe);
  CT_CHECK(ccord_user_expressions_decode(b.d, b.n, &x, &err) == CCORD_OK);
  CT_CHECK(x.emoji_frecency_count == 1 && x.emoji_frecency[0].score == 499);
  ccord_user_expressions_free(&x);
  bb_free(&b);
}

static void test_base64_variants(void)
{
  bb b = {};
  build_fixture(&b);
  ccord_user_expressions x;
  ccord_error err;

  for (int variant = 0; variant < 4; variant++) {
    bool pad = variant & 1;
    bool url = variant & 2;
    char *e = b64_encode(b.d, b.n, pad, url);
    CT_CHECK(ccord_user_expressions_decode_b64(e, strlen(e), &x, &err) == CCORD_OK);
    check_fixture(&x);
    ccord_user_expressions_free(&x);
    free(e);
  }

  // whitespace/newlines are ignored
  char *e = b64_encode(b.d, b.n, true, false);
  size_t el = strlen(e);
  char *w = malloc(el * 2 + 4);
  if (!w) abort();
  size_t k = 0;
  for (size_t i = 0; i < el; i++) {
    w[k++] = e[i];
    if (i % 17 == 16) w[k++] = '\n';
  }
  CT_CHECK(ccord_user_expressions_decode_b64(w, k, &x, &err) == CCORD_OK);
  check_fixture(&x);
  ccord_user_expressions_free(&x);
  free(w);

  // invalid characters / data after padding / impossible length
  char *bad_chr = ccord_strdup(e);
  if (!bad_chr) abort();
  bad_chr[3] = '*';
  CT_CHECK(ccord_user_expressions_decode_b64(bad_chr, el, &x, &err) == CCORD_ERR_INVALID);
  ccord_free(bad_chr);
  CT_CHECK(ccord_user_expressions_decode_b64("QQ==QQ", 6, &x, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_user_expressions_decode_b64("Q", 1, &x, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_user_expressions_decode_b64("", 0, &x, &err) == CCORD_OK);
  ccord_user_expressions_free(&x);
  CT_CHECK(ccord_user_expressions_decode_b64(nullptr, 3, &x, &err) == CCORD_ERR_INVALID);
  free(e);

  // a base64 string that decodes to a truncated protobuf is an error
  char *t = b64_encode(b.d, b.n - 1, true, false);
  CT_CHECK(ccord_user_expressions_decode_b64(t, strlen(t), &x, &err) == CCORD_ERR_INVALID);
  free(t);
  bb_free(&b);
}

static void test_json_wrapper(void)
{
  bb b = {};
  build_fixture(&b);
  char *e = b64_encode(b.d, b.n, true, false);
  size_t jl = strlen(e) + 64;
  char *json = malloc(jl);
  if (!json) abort();
  int n = snprintf(json, jl, "{\"settings\":\"%s\",\"extra\":[1,2,3]}", e);
  CT_CHECK(n > 0);
  ccord_user_expressions x;
  ccord_error err;
  CT_CHECK(ccord_user_expressions_from_settings_json(json, (size_t)n, &x, &err) == CCORD_OK);
  check_fixture(&x);
  ccord_user_expressions_free(&x);
  free(json);
  free(e);
  bb_free(&b);

  static const char empty[] = "{\"settings\":\"\"}";
  CT_CHECK(ccord_user_expressions_from_settings_json(empty, sizeof empty - 1, &x, &err) == CCORD_OK);
  CT_CHECK(x.gif_count == 0);
  ccord_user_expressions_free(&x);

  static const char missing[] = "{}";
  CT_CHECK(ccord_user_expressions_from_settings_json(missing, sizeof missing - 1, &x, &err) == CCORD_ERR_INVALID);
  static const char wrong_type[] = "{\"settings\":5}";
  CT_CHECK(ccord_user_expressions_from_settings_json(wrong_type, sizeof wrong_type - 1, &x, &err) == CCORD_ERR_INVALID);
  static const char broken[] = "{\"settings\":";
  CT_CHECK(ccord_user_expressions_from_settings_json(broken, sizeof broken - 1, &x, &err) != CCORD_OK);
  static const char badb64[] = "{\"settings\":\"!!!!\"}";
  CT_CHECK(ccord_user_expressions_from_settings_json(badb64, sizeof badb64 - 1, &x, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_user_expressions_from_settings_json(nullptr, 0, &x, &err) == CCORD_ERR_INVALID);
}

// ---- raw protobuf scanning helpers for the writer tests --------------------------------------------------------

static bool raw_read_varint(const uint8_t **pp, const uint8_t *end, uint64_t *out)
{
  uint64_t v = 0;
  for (unsigned i = 0; i < 10; i++) {
    if (*pp >= end) return false;
    uint8_t b = *(*pp)++;
    if (i == 9 && b > 1) return false;
    v |= (uint64_t)(b & 0x7fu) << (7u * i);
    if (!(b & 0x80u)) {
      *out = v;
      return true;
    }
  }
  return false;
}

static bool raw_record(const uint8_t **pp, const uint8_t *end, uint32_t *field, size_t *len)
{
  const uint8_t *start = *pp;
  uint64_t tag;
  if (!raw_read_varint(pp, end, &tag)) return false;
  if (tag > UINT32_MAX || (tag >> 3) == 0 || (tag >> 3) > 0x1fffffffu) return false;
  *field = (uint32_t)(tag >> 3);
  size_t payload = 0;
  switch (tag & 7u) {
    case 0: {
      uint64_t v;
      if (!raw_read_varint(pp, end, &v)) return false;
      break;
    }
    case 1: payload = 8; break;
    case 2: {
      uint64_t n;
      if (!raw_read_varint(pp, end, &n)) return false;
      if (n > (uint64_t)(end - *pp)) return false;
      payload = (size_t)n;
      break;
    }
    case 5: payload = 4; break;
    default: return false;
  }
  if (payload > (size_t)(end - *pp)) return false;
  *pp += payload;
  *len = (size_t)(*pp - start);
  return true;
}

// Appends every top-level record whose field number is not `skip` to `out`, byte for byte. Returns the
// number of skipped records, or (size_t)-1 when the blob is malformed.
static size_t collect_non_field(const uint8_t *d, size_t n, uint32_t skip, bb *out)
{
  const uint8_t *p = d, *end = d + n;
  size_t skipped = 0;
  while (p < end) {
    const uint8_t *start = p;
    uint32_t field;
    size_t len;
    if (!raw_record(&p, end, &field, &len)) return (size_t)-1;
    if (field == skip) {
      skipped++;
      continue;
    }
    bb_put(out, start, len);
  }
  return skipped;
}

static void build_preserve_fixture(bb *out)
{
  bb ver = {};
  put_u(&ver, 1, 7);
  put_u(&ver, 2, 1);
  put_msg(out, 1, &ver);

  put_u(out, 99, 0x1122334455667788ull);                // unknown varint
  put_fixed64(out, 100, 0x0102030405060708ull);         // unknown fixed64
  bb ld = {};
  put_str(&ld, 1, "opaque payload");
  put_msg(out, 101, &ld);                               // unknown length-delimited
  put_tag(out, 102, 5);                                 // unknown fixed32
  uint8_t f32[4] = {9, 8, 7, 6};
  bb_put(out, f32, sizeof f32);

  bb ef = {};
  put_emoji_entry(&ef, "wave", 1, 10, 50, true, false);
  put_msg(out, 6, &ef);                                 // emoji frecency

  bb old = {};
  put_gif_entry(&old, "https://old.invalid/gif", "https://old.invalid/gif.gif", 1, 11, 22, 33, false);
  put_msg(out, 2, &old);                                // favorite_gifs: replaced

  put_u(out, 103, 42);                                  // trailing unknown varint
}

static void test_encode_preserves_other_fields(void)
{
  bb existing = {};
  build_preserve_fixture(&existing);
  ccord_proto_gif gifs[2] = {
      {.url = "https://one.invalid/gif", .src = "https://cdn.invalid/one.gif", .format = 1,
       .width = 320, .height = 240, .order = 3},
      {.url = "https://two.invalid/gif", .src = "", .format = 2, .width = 640, .height = 360, .order = 9},
  };
  uint8_t *raw = nullptr;
  size_t raw_len = 0;
  ccord_error err;
  CT_CHECK(ccord_user_expressions_encode_gifs(gifs, 2, existing.d, existing.n, &raw, &raw_len, &err) == CCORD_OK);
  CT_CHECK(raw != nullptr && raw_len > 0);

  bb want = {}, got = {};
  size_t old_field2 = collect_non_field(existing.d, existing.n, 2, &want);
  size_t new_field2 = collect_non_field(raw, raw_len, 2, &got);
  CT_CHECK(old_field2 == 1 && new_field2 == 1);
  CT_CHECK(want.n == got.n && (want.n == 0 || memcmp(want.d, got.d, want.n) == 0));

  ccord_user_expressions x;
  CT_CHECK(ccord_user_expressions_decode(raw, raw_len, &x, &err) == CCORD_OK);
  CT_CHECK(x.gif_count == 2);
  if (x.gif_count == 2) {
    CT_CHECK(strcmp(x.gifs[0].url, "https://one.invalid/gif") == 0);
    CT_CHECK(strcmp(x.gifs[0].src, "https://cdn.invalid/one.gif") == 0);
    CT_CHECK(x.gifs[0].format == 1 && x.gifs[0].width == 320 && x.gifs[0].height == 240 && x.gifs[0].order == 3);
    CT_CHECK(strcmp(x.gifs[1].url, "https://two.invalid/gif") == 0);
    CT_CHECK(x.gifs[1].src[0] == '\0');
    CT_CHECK(x.gifs[1].format == 2 && x.gifs[1].width == 640 && x.gifs[1].height == 360 && x.gifs[1].order == 9);
  }
  ccord_user_expressions_free(&x);

  // Clearing the favorites keeps every non-field-2 record and still emits a (empty) field 2.
  uint8_t *cleared = nullptr;
  size_t cleared_len = 0;
  CT_CHECK(ccord_user_expressions_encode_gifs(nullptr, 0, raw, raw_len, &cleared, &cleared_len, &err) == CCORD_OK);
  bb cleared_rest = {};
  CT_CHECK(collect_non_field(cleared, cleared_len, 2, &cleared_rest) == 1);
  CT_CHECK(want.n == cleared_rest.n && (want.n == 0 || memcmp(want.d, cleared_rest.d, want.n) == 0));
  CT_CHECK(ccord_user_expressions_decode(cleared, cleared_len, &x, &err) == CCORD_OK);
  CT_CHECK(x.gif_count == 0);
  ccord_user_expressions_free(&x);

  ccord_free(raw);
  ccord_free(cleared);
  bb_free(&want);
  bb_free(&got);
  bb_free(&cleared_rest);
  bb_free(&existing);
}

static void test_encode_favorite_gifs_roundtrip(void)
{
  char url[513], src[513];
  memset(url, 'u', 512);
  url[512] = '\0';
  memset(src, 's', 512);
  src[512] = '\0';
  ccord_proto_gif gifs[2] = {
      {.url = url, .src = src, .format = 2, .width = 498, .height = 280, .order = 7},
      {.url = "https://example.invalid/gifs/new", .src = "", .format = 1,
       .width = 320, .height = 240, .order = 2},
  };
  char *json = nullptr;
  ccord_error err;
  CT_CHECK(ccord_user_expressions_encode_gifs_json(gifs, 2, nullptr, 0, &json, &err) == CCORD_OK);
  CT_CHECK(json != nullptr);
  if (!json) return;
  CT_CHECK(strncmp(json, "{\"settings\":\"", 12) == 0);
  CT_CHECK(strlen(json) > 13 && json[strlen(json) - 1] == '}');

  ccord_user_expressions x;
  CT_CHECK(ccord_user_expressions_from_settings_json(json, strlen(json), &x, &err) == CCORD_OK);
  CT_CHECK(x.gif_count == 2);
  if (x.gif_count == 2) {
    CT_CHECK(strcmp(x.gifs[0].url, "https://example.invalid/gifs/new") == 0);
    CT_CHECK(x.gifs[0].src[0] == '\0');
    CT_CHECK(x.gifs[0].format == 1 && x.gifs[0].width == 320 && x.gifs[0].height == 240 && x.gifs[0].order == 2);
    CT_CHECK(strlen(x.gifs[1].url) == 512 && strlen(x.gifs[1].src) == 512);
    CT_CHECK(memcmp(x.gifs[1].url, url, 512) == 0);
    CT_CHECK(memcmp(x.gifs[1].src, src, 512) == 0);
    CT_CHECK(x.gifs[1].format == 2 && x.gifs[1].width == 498 && x.gifs[1].height == 280 && x.gifs[1].order == 7);
  }
  ccord_user_expressions_free(&x);
  ccord_free(json);
}

static void test_encode_json_uses_existing(void)
{
  bb old = {};
  build_fixture(&old);
  char *eb64 = b64_encode(old.d, old.n, true, false);
  size_t jl = strlen(eb64) + 64;
  char *json = malloc(jl);
  if (!json) abort();
  int n = snprintf(json, jl, "{\"settings\":\"%s\",\"extra\":true}", eb64);
  CT_CHECK(n > 0 && (size_t)n < jl);

  ccord_proto_gif g = {.url = "https://new.invalid/gif", .src = "https://new.invalid/gif.gif",
                       .format = 2, .width = 100, .height = 50, .order = 1};
  char *out = nullptr;
  ccord_error err;
  CT_CHECK(ccord_user_expressions_encode_gifs_json(&g, 1, json, (size_t)n, &out, &err) == CCORD_OK);
  ccord_user_expressions x;
  CT_CHECK(ccord_user_expressions_from_settings_json(out, strlen(out), &x, &err) == CCORD_OK);
  CT_CHECK(x.gif_count == 1);
  if (x.gif_count == 1) {
    CT_CHECK(strcmp(x.gifs[0].url, "https://new.invalid/gif") == 0);
    CT_CHECK(strcmp(x.gifs[0].src, "https://new.invalid/gif.gif") == 0);
    CT_CHECK(x.gifs[0].format == 2 && x.gifs[0].width == 100 && x.gifs[0].height == 50 && x.gifs[0].order == 1);
  }
  // Non-field-2 collections from the existing GET body were preserved by the merge.
  CT_CHECK(x.favorite_emoji_count == 2);
  CT_CHECK(x.sticker_frecency_count == 2);
  CT_CHECK(x.emoji_frecency_count == 3);
  ccord_user_expressions_free(&x);

  ccord_free(out);
  free(json);
  free(eb64);
  bb_free(&old);
}

static void test_encode_b64_and_errors(void)
{
  bb old = {};
  bb ver = {};
  put_u(&ver, 1, 7);
  put_msg(&old, 1, &ver);
  char *eb64 = b64_encode(old.d, old.n, true, false);
  ccord_proto_gif g = {.url = "https://b64.invalid/gif", .src = "https://b64.invalid/gif.gif",
                       .format = 1, .width = 12, .height = 34, .order = 5};
  char *out = nullptr;
  ccord_error err;
  CT_CHECK(ccord_user_expressions_encode_gifs_b64(&g, 1, eb64, strlen(eb64), &out, &err) == CCORD_OK);
  ccord_user_expressions x;
  CT_CHECK(ccord_user_expressions_decode_b64(out, strlen(out), &x, &err) == CCORD_OK);
  CT_CHECK(x.gif_count == 1);
  if (x.gif_count == 1) {
    CT_CHECK(strcmp(x.gifs[0].url, "https://b64.invalid/gif") == 0);
    CT_CHECK(x.gifs[0].format == 1 && x.gifs[0].width == 12 && x.gifs[0].height == 34 && x.gifs[0].order == 5);
  }
  ccord_user_expressions_free(&x);
  ccord_free(out);
  free(eb64);
  bb_free(&old);

  // Malformed existing protobuf is rejected instead of being silently dropped.
  const uint8_t truncated[] = {0x0a, 0x05, 'x'};
  uint8_t *raw = nullptr;
  size_t raw_len = 99;
  CT_CHECK(ccord_user_expressions_encode_gifs(nullptr, 0, truncated, sizeof truncated, &raw, &raw_len, &err) ==
           CCORD_ERR_INVALID);
  CT_CHECK(raw == nullptr && raw_len == 0);

  // Missing/empty url, missing src, invalid UTF-8, and bad arguments are rejected.
  ccord_proto_gif bad = {.url = nullptr, .src = "x"};
  out = nullptr;
  CT_CHECK(ccord_user_expressions_encode_gifs_json(&bad, 1, nullptr, 0, &out, &err) == CCORD_ERR_INVALID);
  bad.url = "";
  CT_CHECK(ccord_user_expressions_encode_gifs_json(&bad, 1, nullptr, 0, &out, &err) == CCORD_ERR_INVALID);
  bad.url = "https://ok.invalid/gif";
  bad.src = nullptr;
  CT_CHECK(ccord_user_expressions_encode_gifs_json(&bad, 1, nullptr, 0, &out, &err) == CCORD_ERR_INVALID);
  bad.src = "\xff\xfe";
  CT_CHECK(ccord_user_expressions_encode_gifs_json(&bad, 1, nullptr, 0, &out, &err) == CCORD_ERR_INVALID);
  bad.src = "ok";
  CT_CHECK(ccord_user_expressions_encode_gifs_json(nullptr, 1, nullptr, 0, &out, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_user_expressions_encode_gifs_json(&bad, 1, nullptr, 0, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_user_expressions_encode_gifs_b64(&bad, 1, nullptr, 1, &out, &err) == CCORD_ERR_INVALID);
}

// ---- status (type-1 settings proto, field 11) --------------------------------------------------------------------

static void put_string_value(bb *b, uint32_t field, const char *s)
{
  bb v = {};
  put_str(&v, 1, s);
  put_msg(b, field, &v);
}

static void build_status_sub(bb *sub, const char *status, bool custom, bool extra)
{
  if (status) put_string_value(sub, 1, status);
  if (custom) {
    bb c = {};
    put_str(&c, 1, "brb lunch");
    put_fixed64(&c, 2, 1234567890123456789ull);
    put_str(&c, 3, "taco");
    put_fixed64(&c, 4, 1700000000000ull);
    put_fixed64(&c, 5, 1699999999999ull);  // created_at_ms
    put_msg(sub, 2, &c);
  }
  if (extra) {
    bb g = {};
    put_u(&g, 1, 1);
    put_msg(sub, 3, &g);                // show_current_game
    put_u(sub, 99, 7);                  // unknown varint subfield
    put_str(sub, 100, "future");        // unknown bytes subfield
  }
  put_fixed64(sub, 4, 1800000000000ull);
}

static void test_status_decode(void)
{
  ccord_error err;
  ccord_proto_status s;

  // Empty input and a proto without field 11: not present.
  CT_CHECK(ccord_user_status_decode(nullptr, 0, &s, &err) == CCORD_OK);
  CT_CHECK(!s.present && !s.has_status && !s.has_custom);
  bb other = {};
  put_u(&other, 1, 5);
  bb g = {};
  put_u(&g, 1, 1);
  put_msg(&other, 8, &g);
  CT_CHECK(ccord_user_status_decode(other.d, other.n, &s, &err) == CCORD_OK);
  CT_CHECK(!s.present && !s.has_status && !s.has_custom && s.status[0] == '\0');

  // Full status submessage between other fields.
  bb sub = {};
  build_status_sub(&sub, "dnd", true, true);
  bb root = {};
  bb_put(&root, other.d, other.n);
  put_msg(&root, 11, &sub);
  put_str(&root, 50, "tail");
  CT_CHECK(ccord_user_status_decode(root.d, root.n, &s, &err) == CCORD_OK);
  CT_CHECK(s.present && s.has_status && strcmp(s.status, "dnd") == 0);
  CT_CHECK(s.status_expires_at_ms == 1800000000000ull);
  CT_CHECK(s.has_custom && strcmp(s.custom_text, "brb lunch") == 0);
  CT_CHECK(s.emoji_id == 1234567890123456789ull && strcmp(s.emoji_name, "taco") == 0);
  CT_CHECK(s.custom_expires_at_ms == 1700000000000ull);

  // Truncated status payload and a string with a NUL are rejected, with *out cleared.
  CT_CHECK(ccord_user_status_decode(root.d, root.n - 8, &s, &err) == CCORD_ERR_INVALID);
  CT_CHECK(!s.present);
  bb nul = {};
  bb sv = {};
  put_bytes(&sv, 1, "o\0k", 3);
  bb st = {};
  put_msg(&st, 1, &sv);
  put_msg(&nul, 11, &st);
  CT_CHECK(ccord_user_status_decode(nul.d, nul.n, &s, &err) == CCORD_ERR_INVALID);
  // A status-only message: custom absent.
  bb sub2 = {};
  put_string_value(&sub2, 1, "idle");
  bb root2 = {};
  put_msg(&root2, 11, &sub2);
  CT_CHECK(ccord_user_status_decode(root2.d, root2.n, &s, &err) == CCORD_OK);
  CT_CHECK(s.present && strcmp(s.status, "idle") == 0 && !s.has_custom && s.status_expires_at_ms == 0);
  // Over-long custom text is a range error.
  bb long_c = {};
  char *big = malloc(CCORD_PROTO_CUSTOM_TEXT_MAX + 10);
  memset(big, 'a', CCORD_PROTO_CUSTOM_TEXT_MAX + 9);
  big[CCORD_PROTO_CUSTOM_TEXT_MAX + 9] = '\0';
  put_str(&long_c, 1, big);
  bb lsub = {};
  put_msg(&lsub, 2, &long_c);
  bb lroot = {};
  put_msg(&lroot, 11, &lsub);
  CT_CHECK(ccord_user_status_decode(lroot.d, lroot.n, &s, &err) == CCORD_ERR_RANGE);
  free(big);

  bb_free(&other);
  bb_free(&root);
  bb_free(&nul);
  bb_free(&root2);
  bb_free(&lroot);
}

static void test_status_patch(void)
{
  ccord_error err;
  ccord_proto_status s;

  // Base: other top-level fields (including a gif container), then a status with known and unknown subfields.
  bb pre = {};
  put_u(&pre, 1, 5);
  put_str(&pre, 2, "abc");
  bb sub = {};
  build_status_sub(&sub, "online", true, true);
  bb base = {};
  bb_put(&base, pre.d, pre.n);
  put_msg(&base, 11, &sub);
  put_fixed64(&base, 60, 42);  // after the status: must survive too

  // The exact bytes of the preserved status subfields (3, 99, 100).
  bb keep = {};
  {
    bb g = {};
    put_u(&g, 1, 1);
    put_msg(&keep, 3, &g);
    put_u(&keep, 99, 7);
    put_str(&keep, 100, "future");
  }

  ccord_proto_status want = {};
  want.has_status = true;
  strcpy(want.status, "idle");
  want.has_custom = true;
  strcpy(want.custom_text, "new text");
  want.emoji_id = 777;
  strcpy(want.emoji_name, "wave");
  want.custom_expires_at_ms = 1900000000000ull;
  want.status_expires_at_ms = 0;

  uint8_t *out = nullptr;
  size_t out_len = 0;
  CT_CHECK(ccord_user_status_patch(base.d, base.n, &want, &out, &out_len, &err) == CCORD_OK);
  CT_CHECK(out != nullptr);
  if (out) {
    // Top-level fields other than 11 are byte-identical and in order (pre first); the status record is rewritten last.
    bb t = {};
    put_fixed64(&t, 60, 42);
    CT_CHECK(out_len > pre.n + t.n);
    CT_CHECK(memcmp(out, pre.d, pre.n) == 0);
    // The patched record is decodable, carries the new values and the untouched subfields verbatim.
    CT_CHECK(ccord_user_status_decode(out, out_len, &s, &err) == CCORD_OK);
    CT_CHECK(s.present && strcmp(s.status, "idle") == 0 && strcmp(s.custom_text, "new text") == 0);
    CT_CHECK(s.emoji_id == 777 && strcmp(s.emoji_name, "wave") == 0);
    CT_CHECK(s.custom_expires_at_ms == 1900000000000ull && s.status_expires_at_ms == 0);
    bool found = false;
    for (size_t i = 0; i + keep.n <= out_len; i++)
      if (memcmp(out + i, keep.d, keep.n) == 0) found = true;
    CT_CHECK(found);
    bool tail_found = false;
    for (size_t i = 0; i + t.n <= out_len; i++)
      if (memcmp(out + i, t.d, t.n) == 0) tail_found = true;
    CT_CHECK(tail_found);
    // Patching again with the decoded state is a fixed point.
    uint8_t *again = nullptr;
    size_t again_len = 0;
    CT_CHECK(ccord_user_status_patch(out, out_len, &s, &again, &again_len, &err) == CCORD_OK);
    CT_CHECK(again_len == out_len && again && memcmp(again, out, out_len) == 0);
    ccord_free(again);

    // Clearing the custom status removes it, keeps the status string and the unknown subfields.
    ccord_proto_status clr = s;
    clr.has_custom = false;
    memset(clr.custom_text, 0, sizeof clr.custom_text);
    memset(clr.emoji_name, 0, sizeof clr.emoji_name);
    clr.emoji_id = clr.custom_expires_at_ms = 0;
    uint8_t *cleared = nullptr;
    size_t cleared_len = 0;
    CT_CHECK(ccord_user_status_patch(out, out_len, &clr, &cleared, &cleared_len, &err) == CCORD_OK);
    ccord_proto_status d;
    CT_CHECK(cleared && ccord_user_status_decode(cleared, cleared_len, &d, &err) == CCORD_OK);
    CT_CHECK(d.present && !d.has_custom && d.custom_text[0] == '\0' && strcmp(d.status, "idle") == 0);
    CT_CHECK(cleared_len < out_len);
    found = false;
    for (size_t i = 0; cleared && i + keep.n <= cleared_len; i++)
      if (memcmp(cleared + i, keep.d, keep.n) == 0) found = true;
    CT_CHECK(found);
    ccord_free(cleared);
    bb_free(&t);
  }
  ccord_free(out);

  // Patching an empty base builds just the status record; no status is not-present after decode.
  out = nullptr;
  CT_CHECK(ccord_user_status_patch(nullptr, 0, &want, &out, &out_len, &err) == CCORD_OK);
  CT_CHECK(out && ccord_user_status_decode(out, out_len, &s, &err) == CCORD_OK && s.present &&
           strcmp(s.status, "idle") == 0);
  ccord_free(out);
  ccord_proto_status none = {};
  out = nullptr;
  CT_CHECK(ccord_user_status_patch(nullptr, 0, &none, &out, &out_len, &err) == CCORD_OK && out_len == 0);
  ccord_free(out);

  // Status-only change: invisible with expiry; the b64 variants round-trip.
  ccord_proto_status inv = {.has_status = true, .status_expires_at_ms = 1234};
  strcpy(inv.status, "invisible");
  char *b64 = nullptr;
  CT_CHECK(ccord_user_status_patch_b64(nullptr, 0, &inv, &b64, &err) == CCORD_OK && b64);
  CT_CHECK(b64 && ccord_user_status_decode_b64(b64, strlen(b64), &s, &err) == CCORD_OK);
  CT_CHECK(strcmp(s.status, "invisible") == 0 && s.status_expires_at_ms == 1234 && !s.has_custom);
  ccord_free(b64);

  // Bad arguments: unknown status string, invalid UTF-8, malformed base, wrong wire type for field 11.
  ccord_proto_status badst = {.has_status = true};
  strcpy(badst.status, "away");
  CT_CHECK(ccord_user_status_patch(nullptr, 0, &badst, &out, &out_len, &err) == CCORD_ERR_INVALID && out == nullptr);
  strcpy(badst.status, "idle");
  badst.has_custom = true;
  strcpy(badst.custom_text, "\xff\xfe");
  CT_CHECK(ccord_user_status_patch(nullptr, 0, &badst, &out, &out_len, &err) == CCORD_ERR_INVALID);
  const uint8_t trunc[] = {0x5a, 0x05, 'x'};
  CT_CHECK(ccord_user_status_patch(trunc, sizeof trunc, &want, &out, &out_len, &err) == CCORD_ERR_INVALID);
  const uint8_t wrongwt[] = {0x58, 0x01};  // field 11 as varint
  CT_CHECK(ccord_user_status_patch(wrongwt, sizeof wrongwt, &want, &out, &out_len, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_user_status_patch(nullptr, 0, nullptr, &out, &out_len, &err) == CCORD_ERR_INVALID);

  bb_free(&pre);
  bb_free(&base);
  bb_free(&keep);
}

int main(void)
{
  test_decode_fixture();
  test_empty_and_args();
  test_packed_and_unpacked_equivalence();
  test_truncation_errors();
  test_mutation_fuzz();
  test_alloc_failures();
  test_unknown_and_bad_wire_types();
  test_limits_and_string_validation();
  test_base64_variants();
  test_json_wrapper();
  test_encode_preserves_other_fields();
  test_encode_favorite_gifs_roundtrip();
  test_encode_json_uses_existing();
  test_encode_b64_and_errors();
  test_status_decode();
  test_status_patch();
  return CT_RESULT("test_discord_proto");
}
