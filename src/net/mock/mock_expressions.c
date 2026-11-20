// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "mock_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void pb_varint(ccord_buf *b, uint64_t v)
{
  uint8_t t[10];
  size_t n = 0;
  while (v >= 0x80u) {
    t[n++] = (uint8_t)(v | 0x80u);
    v >>= 7u;
  }
  t[n++] = (uint8_t)v;
  (void)ccord_buf_append(b, t, n);
}

static void pb_u(ccord_buf *b, uint32_t field, uint64_t v)
{
  pb_varint(b, ((uint64_t)field << 3u) | 0u);
  pb_varint(b, v);
}

static void pb_fixed64(ccord_buf *b, uint32_t field, uint64_t v)
{
  pb_varint(b, ((uint64_t)field << 3u) | 1u);
  uint8_t t[8];
  for (unsigned i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8u * i));
  (void)ccord_buf_append(b, t, 8);
}

static void pb_bytes(ccord_buf *b, uint32_t field, const void *p, size_t n)
{
  pb_varint(b, ((uint64_t)field << 3u) | 2u);
  pb_varint(b, n);
  (void)ccord_buf_append(b, p, n);
}

static void pb_str(ccord_buf *b, uint32_t field, const char *s)
{
  pb_bytes(b, field, s, strlen(s));
}

static void pb_gif(ccord_buf *gifs, const char *url, const char *src, uint32_t w, uint32_t h, uint32_t order)
{
  ccord_buf v, e;
  ccord_buf_init(&v, 0);
  ccord_buf_init(&e, 0);
  pb_u(&v, 1, 1);
  pb_str(&v, 2, src);
  pb_u(&v, 3, w);
  pb_u(&v, 4, h);
  pb_u(&v, 5, order);
  pb_str(&e, 1, url);
  pb_bytes(&e, 2, v.data, v.len);
  pb_bytes(gifs, 1, e.data, e.len);
  ccord_buf_free(&v);
  ccord_buf_free(&e);
}

static void pb_emoji_frec(ccord_buf *ef, const char *key, uint32_t score)
{
  ccord_buf v, e;
  ccord_buf_init(&v, 0);
  ccord_buf_init(&e, 0);
  pb_u(&v, 1, 5);
  pb_u(&v, 3, score);
  pb_u(&v, 4, score);
  pb_str(&e, 1, key);
  pb_bytes(&e, 2, v.data, v.len);
  pb_bytes(ef, 1, e.data, e.len);
  ccord_buf_free(&v);
  ccord_buf_free(&e);
}

static void pb_sticker_frec(ccord_buf *sf, uint64_t id, uint32_t score)
{
  ccord_buf v, e;
  ccord_buf_init(&v, 0);
  ccord_buf_init(&e, 0);
  pb_u(&v, 1, 3);
  pb_u(&v, 3, score);
  pb_u(&v, 4, score);
  pb_fixed64(&e, 1, id);
  pb_bytes(&e, 2, v.data, v.len);
  pb_bytes(sf, 1, e.data, e.len);
  ccord_buf_free(&v);
  ccord_buf_free(&e);
}

// {"settings":"<base64 of the raw protobuf>"} — the GET/PATCH /users/@me/settings-proto/2 answer shape.
static void settings_proto_json(ccord_buf *out, const uint8_t *d, size_t n)
{
  (void)ccord_buf_append_str(out, "{\"settings\":\"");
  size_t enc_len = 0;
  char *enc = ccord_base64_encode_alloc(d, n, &enc_len);
  if (enc) {
    (void)ccord_buf_append(out, enc, enc_len);
    ccord_free(enc);
  }
  (void)ccord_buf_append_str(out, "\"}");
}

// The seeded FrecencyUserSettings protobuf (favorite GIFs, sticker favorites/frecency, emoji favorites/frecency).
static void build_mock_settings_proto_raw(ccord_buf *raw)
{
  ccord_buf sub;
  ccord_buf_init(raw, 0);

  ccord_buf_init(&sub, 0);
  pb_gif(&sub, "https://tenor.com/view/wumpus-wave-gif-1001",
         "https://media.discordapp.net/attachments/2001/gif_wumpus_wave.gif", 320, 240, 1);
  pb_gif(&sub, "https://tenor.com/view/ccord-ship-it-gif-1002",
         "https://media.discordapp.net/attachments/2001/gif_ship_it.gif", 320, 220, 2);
  pb_bytes(raw, 2, sub.data, sub.len);
  ccord_buf_free(&sub);

  ccord_buf_init(&sub, 0);
  pb_fixed64(&sub, 1, 7001);
  pb_fixed64(&sub, 1, 7002);
  pb_bytes(raw, 3, sub.data, sub.len);
  ccord_buf_free(&sub);

  ccord_buf_init(&sub, 0);
  pb_sticker_frec(&sub, 7001, 90);
  pb_sticker_frec(&sub, 7002, 80);
  pb_sticker_frec(&sub, 7005, 70);
  pb_bytes(raw, 4, sub.data, sub.len);
  ccord_buf_free(&sub);

  ccord_buf_init(&sub, 0);
  pb_str(&sub, 1, "fire");
  pb_str(&sub, 1, "thumbsup");
  pb_str(&sub, 1, "5001");
  pb_str(&sub, 1, "5004");
  pb_bytes(raw, 5, sub.data, sub.len);
  ccord_buf_free(&sub);

  ccord_buf_init(&sub, 0);
  pb_emoji_frec(&sub, "fire", 95);
  pb_emoji_frec(&sub, "thumbsup", 90);
  pb_emoji_frec(&sub, "5001", 88);
  pb_emoji_frec(&sub, "joy", 85);
  pb_emoji_frec(&sub, "heart", 80);
  pb_emoji_frec(&sub, "tada", 75);
  pb_emoji_frec(&sub, "rocket", 70);
  pb_emoji_frec(&sub, "eyes", 65);
  pb_bytes(raw, 6, sub.data, sub.len);
  ccord_buf_free(&sub);
}

static const char k_mock_gif_categories_json[] =
  "{\"categories\":["
  "{\"name\":\"Applause\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_applause.gif\"},"
  "{\"name\":\"Ship It\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_shipit.gif\"},"
  "{\"name\":\"Happy\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_happy.gif\"},"
  "{\"name\":\"High Five\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_highfive.gif\"},"
  "{\"name\":\"Popcorn\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_popcorn.gif\"},"
  "{\"name\":\"Mind Blown\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_mindblown.gif\"},"
  "{\"name\":\"Dance\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_dance.gif\"},"
  "{\"name\":\"Thumbs Up\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_thumbsup.gif\"}"
  "],\"gifs\":["
  "{\"url\":\"https://tenor.com/view/wumpus-wave-gif-1001\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_wumpus_wave.gif\",\"width\":320,\"height\":240},"
  "{\"url\":\"https://tenor.com/view/ccord-ship-it-gif-1002\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_ship_it.gif\",\"width\":320,\"height\":220},"
  "{\"url\":\"https://tenor.com/view/zero-leaks-dance-gif-1003\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_zero_leaks.gif\",\"width\":320,\"height\":260},"
  "{\"url\":\"https://tenor.com/view/party-blob-spin-gif-1004\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_party_blob.gif\",\"width\":320,\"height\":240},"
  "{\"url\":\"https://tenor.com/view/pixel-sunset-vibes-gif-1005\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_pixel_sunset.gif\",\"width\":320,\"height\":200},"
  "{\"url\":\"https://tenor.com/view/lua-rocket-launch-gif-1006\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_lua_rocket.gif\",\"width\":320,\"height\":240}"
  "]}";

static const char k_mock_gif_trending_json[] =
  "["
  "{\"url\":\"https://tenor.com/view/wumpus-wave-gif-1001\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_wumpus_wave.gif\",\"width\":320,\"height\":240},"
  "{\"url\":\"https://tenor.com/view/ccord-ship-it-gif-1002\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_ship_it.gif\",\"width\":320,\"height\":220},"
  "{\"url\":\"https://tenor.com/view/zero-leaks-dance-gif-1003\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_zero_leaks.gif\",\"width\":320,\"height\":260},"
  "{\"url\":\"https://tenor.com/view/party-blob-spin-gif-1004\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_party_blob.gif\",\"width\":320,\"height\":240},"
  "{\"url\":\"https://tenor.com/view/pixel-sunset-vibes-gif-1005\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_pixel_sunset.gif\",\"width\":320,\"height\":200},"
  "{\"url\":\"https://tenor.com/view/lua-rocket-launch-gif-1006\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_lua_rocket.gif\",\"width\":320,\"height\":240}"
  "]";

static const char k_mock_sticker_packs_json[] =
  "{\"sticker_packs\":["
  "{\"id\":\"9001\",\"name\":\"Wumpus & Friends\",\"stickers\":["
  "{\"id\":\"8001\",\"name\":\"WumpusHi\",\"description\":\"Say hello\",\"tags\":\"wave,wumpus\",\"format_type\":1},"
  "{\"id\":\"8002\",\"name\":\"WumpusHype\",\"description\":\"Hyped up\",\"tags\":\"hype,party\",\"format_type\":2},"
  "{\"id\":\"8003\",\"name\":\"WumpusSleep\",\"description\":\"Zzz\",\"tags\":\"sleep,nap\",\"format_type\":1}"
  "]},"
  "{\"id\":\"9002\",\"name\":\"Clyde Classics\",\"stickers\":["
  "{\"id\":\"8004\",\"name\":\"ClydeBot\",\"description\":\"Beep boop\",\"tags\":\"robot,clyde\",\"format_type\":1},"
  "{\"id\":\"8005\",\"name\":\"ClydeThumbsUp\",\"description\":\"Approved\",\"tags\":\"ok,thumbsup\",\"format_type\":1},"
  "{\"id\":\"8006\",\"name\":\"ClydeSparkle\",\"description\":\"Shiny\",\"tags\":\"sparkle,lottie\",\"format_type\":3}"
  "]}"
  "]}";

ccord_status mock_request_expression_data(ccord_transport *t, ccord_expr_kind kind, const char *arg,
                                                 size_t arg_len, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (kind >= CCORD_EXPR_KIND_COUNT) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad expression kind");
  if (!arg) {
    arg = "";
    arg_len = 0;
  }

  ccord_buf body;
  ccord_buf_init(&body, 0);
  uint8_t *raw_proto_own = nullptr;
  size_t raw_proto_len = 0;
  if (kind == CCORD_EXPR_SETTINGS_PROTO) {
    if (m->has_settings_proto) {
      raw_proto_len = m->settings_proto_len;
      raw_proto_own = ccord_alloc_array(raw_proto_len ? raw_proto_len : 1u, 1);
      if (!raw_proto_own) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      }
      if (raw_proto_len > 0) memcpy(raw_proto_own, m->settings_proto, raw_proto_len);
      settings_proto_json(&body, m->settings_proto, raw_proto_len);
    } else {
      ccord_buf raw;
      build_mock_settings_proto_raw(&raw);
      raw_proto_len = raw.len;
      raw_proto_own = ccord_alloc_array(raw_proto_len ? raw_proto_len : 1u, 1);
      if (!raw_proto_own) {
        ccord_buf_free(&raw);
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      }
      if (raw_proto_len > 0) memcpy(raw_proto_own, raw.data, raw_proto_len);
      settings_proto_json(&body, (const uint8_t *)raw.data, raw.len);
      ccord_buf_free(&raw);
    }
  } else if (kind == CCORD_EXPR_GIF_CATEGORIES) {
    (void)ccord_buf_append_str(&body, k_mock_gif_categories_json);
  } else if (kind == CCORD_EXPR_GIF_TRENDING) {
    (void)ccord_buf_append_str(&body, k_mock_gif_trending_json);
  } else if (kind == CCORD_EXPR_GIF_SEARCH) {
    char clean[64] = {0};
    size_t clen = 0;
    for (size_t i = 0; i < arg_len && clen + 1 < sizeof clean; i++) {
      char c = arg[i];
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
        clean[clen++] = (char)tolower((unsigned char)c);
      } else if (c == ' ') {
        clean[clen++] = '-';
      }
    }
    if (clen == 0) memcpy(clean, "search", 6);
    (void)ccord_buf_appendf(&body,
      "["
      "{\"url\":\"https://tenor.com/view/%s-reaction-gif-2001\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_%s_1.gif\",\"width\":320,\"height\":240},"
      "{\"url\":\"https://tenor.com/view/%s-party-gif-2002\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_%s_2.gif\",\"width\":320,\"height\":220},"
      "{\"url\":\"https://tenor.com/view/%s-epic-gif-2003\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_%s_3.gif\",\"width\":320,\"height\":260},"
      "{\"url\":\"https://tenor.com/view/%s-vibes-gif-2004\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_%s_4.gif\",\"width\":320,\"height\":240}"
      "]",
      clean, clean, clean, clean, clean, clean, clean, clean);
  } else if (kind == CCORD_EXPR_STICKER_PACKS) {
    (void)ccord_buf_append_str(&body, k_mock_sticker_packs_json);
  }

  char *arg_copy = ccord_strndup(arg, arg_len);
  size_t body_len = 0;
  char *body_str = ccord_buf_take(&body, &body_len);
  if (!arg_copy || !body_str) {
    ccord_free(raw_proto_own);
    ccord_free(arg_copy);
    ccord_free(body_str);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  mk_retire_content(m, arg_copy);
  mk_retire_content(m, body_str);

  mock_item it = mk_make_item(CCORD_NET_EXPRESSION_DATA, mk_now_of(m) + m->cfg.latency_ms);
  it.raw_proto_own = raw_proto_own;
  it.ev.status = CCORD_OK;
  it.ev.expr_kind = kind;
  it.ev.expr_arg = arg_copy;
  it.ev.expr_body = body_str;
  it.ev.expr_body_len = body_len;
  it.ev.raw_proto = raw_proto_own;
  it.ev.raw_proto_len = raw_proto_len;
  return mk_push_item(m, &it, err);
}

ccord_status mock_patch_settings_proto(ccord_transport *t, const uint8_t *proto, size_t proto_len,
                                              ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!proto) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null settings proto");
  if (proto_len > MOCK_SETTINGS_PROTO_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: settings proto too large");
  if (m->cfg.refuse_writes) {
    mock_item it = mk_make_item(CCORD_NET_EXPRESSION_DATA, mk_now_of(m) + m->cfg.latency_ms);
    it.ev.status = CCORD_ERR_PERMISSION;
    it.ev.expr_kind = CCORD_EXPR_SETTINGS_PROTO;
    it.ev.text = "You don't have permission to do that. (Missing Permissions)";
    return mk_push_item(m, &it, err);
  }
  uint8_t *stored = ccord_alloc_array(proto_len ? proto_len : 1u, 1);
  uint8_t *ev_proto = ccord_alloc_array(proto_len ? proto_len : 1u, 1);
  if (!stored || !ev_proto) {
    ccord_free(stored);
    ccord_free(ev_proto);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  if (proto_len > 0) {
    memcpy(stored, proto, proto_len);
    memcpy(ev_proto, proto, proto_len);
  }
  ccord_buf body;
  ccord_buf_init(&body, 0);
  settings_proto_json(&body, proto, proto_len);
  size_t body_len = 0;
  char *body_str = ccord_buf_take(&body, &body_len);
  char *arg_copy = ccord_strndup("", 0);
  if (!body_str || !arg_copy) {
    ccord_free(stored);
    ccord_free(ev_proto);
    ccord_free(body_str);
    ccord_free(arg_copy);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  ccord_free(m->settings_proto);
  m->settings_proto = stored;
  m->settings_proto_len = proto_len;
  m->has_settings_proto = true;

  mk_retire_content(m, arg_copy);
  mk_retire_content(m, body_str);

  mock_item it = mk_make_item(CCORD_NET_EXPRESSION_DATA, mk_now_of(m) + m->cfg.latency_ms);
  it.raw_proto_own = ev_proto;
  it.ev.status = CCORD_OK;
  it.ev.expr_kind = CCORD_EXPR_SETTINGS_PROTO;
  it.ev.expr_arg = arg_copy;
  it.ev.expr_body = body_str;
  it.ev.expr_body_len = body_len;
  it.ev.raw_proto = ev_proto;
  it.ev.raw_proto_len = proto_len;
  return mk_push_item(m, &it, err);
}

// set_status_proto: patch the server's type-1 bytes (the mock is the server, so its bytes are the base a live
// transport would have fetched), record the PATCH, keep the result as the new server state. No echo event: a real
// server's USER_SETTINGS_PROTO_UPDATE for our own write is an echo, never news.
ccord_status mock_set_status_proto(ccord_transport *t, const ccord_proto_status *st, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!st) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null status");
  if (m->cfg.refuse_writes) return ccord_error_set(err, CCORD_ERR_PERMISSION, "mock: status proto refused");
  uint8_t *out = nullptr;
  size_t out_len = 0;
  ccord_status s = ccord_user_status_patch(m->status1, m->status1_len, st, &out, &out_len, err);
  if (s != CCORD_OK) return s;
  ccord_buf body;
  ccord_buf_init(&body, 0);
  settings_proto_json(&body, out, out_len);
  size_t blen = 0;
  char *body_str = ccord_buf_take(&body, &blen);
  struct mock_status1_patch *grown =
    ccord_realloc_array(m->status1_patches, m->nstatus1_patches + 1u, sizeof *grown);
  if (!body_str || !grown) {
    if (grown) m->status1_patches = grown;
    ccord_free(body_str);
    ccord_free(out);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  m->status1_patches = grown;
  uint8_t *copy = ccord_alloc_array(out_len ? out_len : 1u, 1);
  if (!copy) {
    ccord_free(body_str);
    ccord_free(out);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  if (out_len > 0) memcpy(copy, out, out_len);
  m->status1_patches[m->nstatus1_patches++] = (struct mock_status1_patch){.raw = copy, .len = out_len, .body = body_str};
  ccord_free(m->status1);
  m->status1 = out;
  m->status1_len = out_len;
  return CCORD_OK;
}

ccord_status mock_send_sticker(ccord_transport *t, ccord_id channel_id, ccord_id sticker_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (sticker_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid sticker id");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;

  const char *sname = "Sticker";
  int sfmt = 1;
  bool found_custom = false;
  for (size_t i = 0; i < m->nstickers; i++) {
    if (m->stickers[i].id == sticker_id) {
      sname = m->stickers[i].name;
      sfmt = (int)m->stickers[i].format;
      found_custom = true;
      break;
    }
  }
  if (!found_custom) {
    if (sticker_id == 8001) { sname = "WumpusHi"; sfmt = 1; }
    else if (sticker_id == 8002) { sname = "WumpusHype"; sfmt = 2; }
    else if (sticker_id == 8003) { sname = "WumpusSleep"; sfmt = 1; }
    else if (sticker_id == 8004) { sname = "ClydeBot"; sfmt = 1; }
    else if (sticker_id == 8005) { sname = "ClydeThumbsUp"; sfmt = 1; }
    else if (sticker_id == 8006) { sname = "ClydeSparkle"; sfmt = 3; }
  }

  ccord_msg_sticker stk = {.id = sticker_id, .name = sname, .format = (uint8_t)sfmt};
  ccord_message_rich rich = {.present = CCORD_RICH_ALL, .stickers = &stk, .sticker_count = 1};
  ccord_message msg = mk_store_new_rich(m, ci, m->self_id, "", 0, &rich, err, &st);
  if (st != CCORD_OK) return st;
  return mk_queue_message_create(m, msg, mk_now_of(m) + m->cfg.latency_ms, err);
}

static bool mock_post_matches_query(const ccord_forum_post *p, const ccord_forum_query *q)
{
  if (p->forum_id != q->forum_id) return false;
  size_t tc = q->tag_count < CCORD_FORUM_MAX_APPLIED_TAGS ? q->tag_count : CCORD_FORUM_MAX_APPLIED_TAGS;
  for (size_t i = 0; i < tc; i++) {
    ccord_id want = q->tag_ids[i];
    if (want == CCORD_ID_NONE) continue;
    bool found = false;
    for (uint8_t k = 0; k < p->applied_tag_count; k++) {
      if (p->applied_tags[k] == want) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  if (q->name && q->name[0] != '\0') {
    size_t nlen = strlen(q->name);
    const char *title = p->title ? p->title : "";
    const char *first = p->first_content ? p->first_content : "";
    if (!mk_has_ci_substr(title, q->name, nlen) && !mk_has_ci_substr(first, q->name, nlen)) {
      return false;
    }
  }
  return true;
}

size_t mock_find_forum_meta(const mock *m, ccord_id forum_id)
{
  for (size_t i = 0; i < m->nforum_metas; i++) {
    if (m->forum_metas[i].forum_id == forum_id) return i;
  }
  return SIZE_MAX;
}

ccord_status mock_request_forum_posts(ccord_transport *t, const ccord_forum_query *query, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!query || query->forum_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid forum query");
  }
  if (mock_find_forum_meta(m, query->forum_id) == SIZE_MAX) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown forum channel");
  }
  ccord_id fgid = 2001;
  for (size_t i = 0; i < m->nchannels; i++) {
    if (m->channels[i].id == query->forum_id) {
      fgid = m->channels[i].guild_id;
      break;
    }
  }

  uint32_t limit = query->limit ? query->limit : CCORD_FORUM_PAGE_SIZE;
  size_t total_matches = 0;
  for (size_t i = 0; i < m->nforum_posts; i++) {
    if (mock_post_matches_query(&m->forum_posts[i], query)) total_matches++;
  }

  mock_item it = mk_make_item(CCORD_NET_FORUM_POSTS, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.channel_id = query->forum_id;
  it.ev.guild_id = fgid;
  it.ev.forum_offset = query->offset;
  it.ev.forum_total = (uint32_t)total_matches;

  size_t cap = total_matches > 0 ? total_matches : 1;
  it.fposts = ccord_alloc_array(cap, sizeof *it.fposts);
  if (!it.fposts) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");

  size_t seen = 0;
  size_t out_n = 0;
  for (size_t i = 0; i < m->nforum_posts; i++) {
    if (!mock_post_matches_query(&m->forum_posts[i], query)) continue;
    if (seen >= query->offset && out_n < limit) {
      it.fposts[out_n++] = m->forum_posts[i];
    }
    seen++;
  }
  it.ev.forum_posts = it.fposts;
  it.ev.forum_post_count = out_n;
  it.ev.forum_has_more = (query->offset + out_n) < total_matches;
  return mk_push_item(m, &it, err);
}

ccord_status mock_create_forum_post(ccord_transport *t, ccord_id forum_id,
                                           const char *title, size_t title_len,
                                           const char *content, size_t content_len,
                                           const ccord_id *tag_ids, size_t tag_count,
                                           ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (forum_id == CCORD_ID_NONE || mock_find_forum_meta(m, forum_id) == SIZE_MAX) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown forum channel");
  }
  ccord_id fgid = 2001;
  for (size_t i = 0; i < m->nchannels; i++) {
    if (m->channels[i].id == forum_id) {
      fgid = m->channels[i].guild_id;
      break;
    }
  }
  if (!title || title_len == 0 || title_len > CCORD_FORUM_TITLE_BYTES || !ccord_utf8_valid_text(title, title_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid forum post title");
  }
  if (!content || content_len == 0 || content_len > CCORD_SEND_MAX_BYTES || !ccord_utf8_valid_text(content, content_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid forum post content");
  }
  if (tag_count > CCORD_FORUM_MAX_APPLIED_TAGS || (tag_count > 0 && !tag_ids)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid forum post tags");
  }

  char *title_copy = ccord_strndup(title, title_len);
  if (!title_copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  mk_retire_content(m, title_copy);

  size_t n = m->nchannels + 1;
  ccord_channel *nc = ccord_realloc_array(m->channels, n, sizeof *nc);
  ccord_message **nmsgs = ccord_realloc_array(m->chmsgs, n, sizeof *nmsgs);
  size_t *nchn = ccord_realloc_array(m->chn, n, sizeof *nchn);
  size_t *nchcap = ccord_realloc_array(m->chcap, n, sizeof *nchcap);
  if (!nc || !nmsgs || !nchn || !nchcap) {
    if (nc) m->channels = nc;
    if (nmsgs) m->chmsgs = nmsgs;
    if (nchn) m->chn = nchn;
    if (nchcap) m->chcap = nchcap;
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  m->channels = nc;
  m->chmsgs = nmsgs;
  m->chn = nchn;
  m->chcap = nchcap;
  m->snap.channels = m->channels;

  ccord_id tid = m->next_thread_id++;
  size_t tci = m->nchannels++;
  m->channels[tci] = (ccord_channel){
    .id = tid,
    .guild_id = fgid,
    .parent_id = forum_id,
    .kind = CCORD_CHANNEL_TEXT,
    .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD,
    .name = title_copy,
    .topic = "",
  };
  m->chmsgs[tci] = nullptr;
  m->chn[tci] = 0;
  m->chcap[tci] = 0;

  ccord_status st = CCORD_OK;
  ccord_message msg = mk_store_new(m, tci, m->self_id, content, content_len, err, &st);
  if (st != CCORD_OK) return st;

  ccord_forum_post fp = {
    .thread_id = tid,
    .forum_id = forum_id,
    .guild_id = fgid,
    .owner_id = m->self_id,
    .title = title_copy,
    .message_count = 0,
    .create_ts_ms = msg.timestamp_ms,
    .last_activity_ms = msg.timestamp_ms,
    .last_message_id = msg.id,
    .first_message_id = msg.id,
    .first_author_id = m->self_id,
    .first_content = msg.content,
    .thumbnail_url = "",
    .reaction_emoji = "",
  };
  for (size_t i = 0; i < tag_count; i++) {
    if (tag_ids[i] != CCORD_ID_NONE && fp.applied_tag_count < CCORD_FORUM_MAX_APPLIED_TAGS) {
      fp.applied_tags[fp.applied_tag_count++] = tag_ids[i];
    }
  }

  if (m->nforum_posts == m->capforum_posts) {
    size_t ncap = m->capforum_posts ? m->capforum_posts * 2 : 16;
    ccord_forum_post *g = ccord_realloc_array(m->forum_posts, ncap, sizeof *g);
    if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    m->forum_posts = g;
    m->capforum_posts = ncap;
  }
  m->forum_posts[m->nforum_posts++] = fp;

  mock_item it = mk_make_item(CCORD_NET_FORUM_POST_CREATED, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.channel_id = forum_id;
  it.ev.guild_id = fgid;
  it.chan = ccord_alloc_array(1, sizeof *it.chan);
  it.fposts = ccord_alloc_array(1, sizeof *it.fposts);
  it.msgs = ccord_alloc_array(1, sizeof *it.msgs);
  if (!it.chan || !it.fposts || !it.msgs) {
    ccord_free(it.chan);
    ccord_free(it.fposts);
    ccord_free(it.msgs);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  *it.chan = m->channels[tci];
  *it.fposts = fp;
  *it.msgs = msg;
  it.ev.channel = it.chan;
  it.ev.forum_posts = it.fposts;
  it.ev.forum_post_count = 1;
  it.ev.messages = it.msgs;
  it.ev.message_count = 1;
  return mk_push_item(m, &it, err);
}


