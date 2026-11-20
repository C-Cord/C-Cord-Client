// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "store_expr_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>

// ---- Emoji & Sticker Frecency / Favorites ----

static void sort_emoji_frec(ccord_expr_data *d)
{
  for (size_t i = 1; i < d->nemoji_frec; i++) {
    expr_emoji_frec cur = d->emoji_frec[i];
    size_t j = i;
    while (j > 0 && d->emoji_frec[j - 1].score < cur.score) {
      d->emoji_frec[j] = d->emoji_frec[j - 1];
      j--;
    }
    d->emoji_frec[j] = cur;
  }
}

void ccord_store_seed_emoji_frecency(ccord_store *s, const char *key, int32_t score)
{
  if (!s || !key || !key[0]) return;
  ccord_expr_data *d = expr_data_get(s, true);
  if (!d) return;
  for (size_t i = 0; i < d->nemoji_frec; i++) {
    if (strcmp(d->emoji_frec[i].key, key) == 0) {
      if (score > d->emoji_frec[i].score) d->emoji_frec[i].score = score;
      sort_emoji_frec(d);
      d->revision++;
      return;
    }
  }
  if (d->nemoji_frec >= EXPR_MAX_FREC_EMOJIS) return;
  expr_emoji_frec *e = &d->emoji_frec[d->nemoji_frec++];
  strncpy(e->key, key, sizeof e->key - 1);
  e->key[sizeof e->key - 1] = '\0';
  e->score = score;
  sort_emoji_frec(d);
  d->revision++;
}

void ccord_store_record_emoji_use(ccord_store *s, const char *key)
{
  if (!s || !key || !key[0]) return;
  ccord_expr_data *d = expr_data_get(s, true);
  if (!d) return;
  int32_t max_score = 100;
  for (size_t i = 0; i < d->nemoji_frec; i++) {
    if (d->emoji_frec[i].score >= max_score) max_score = d->emoji_frec[i].score + 10;
  }
  for (size_t i = 0; i < d->nemoji_frec; i++) {
    if (strcmp(d->emoji_frec[i].key, key) == 0) {
      d->emoji_frec[i].score = max_score;
      sort_emoji_frec(d);
      d->revision++;
      return;
    }
  }
  if (d->nemoji_frec >= EXPR_MAX_FREC_EMOJIS) {
    d->nemoji_frec = EXPR_MAX_FREC_EMOJIS - 1;
  }
  expr_emoji_frec *e = &d->emoji_frec[d->nemoji_frec++];
  strncpy(e->key, key, sizeof e->key - 1);
  e->key[sizeof e->key - 1] = '\0';
  e->score = max_score;
  sort_emoji_frec(d);
  d->revision++;
}

size_t ccord_store_frequent_emoji_count(const ccord_store *s)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return d ? d->nemoji_frec : 0;
}

const char *ccord_store_frequent_emoji_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return (d && i < d->nemoji_frec) ? d->emoji_frec[i].key : nullptr;
}

size_t ccord_store_favorite_emoji_count(const ccord_store *s)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return d ? d->nfav_emojis : 0;
}

const char *ccord_store_favorite_emoji_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return (d && i < d->nfav_emojis) ? d->fav_emojis[i] : nullptr;
}

bool ccord_store_is_favorite_emoji(const ccord_store *s, const char *key)
{
  const ccord_expr_data *d = expr_data_peek(s);
  if (!d || !key || !key[0]) return false;
  for (size_t i = 0; i < d->nfav_emojis; i++) {
    if (strcmp(d->fav_emojis[i], key) == 0) return true;
  }
  return false;
}

bool ccord_store_toggle_favorite_emoji(ccord_store *s, const char *key)
{
  if (!s || !key || !key[0]) return false;
  ccord_expr_data *d = expr_data_get(s, true);
  if (!d) return false;
  for (size_t i = 0; i < d->nfav_emojis; i++) {
    if (strcmp(d->fav_emojis[i], key) == 0) {
      memmove(&d->fav_emojis[i], &d->fav_emojis[i + 1], (d->nfav_emojis - i - 1) * sizeof d->fav_emojis[0]);
      d->nfav_emojis--;
      d->revision++;
      return false;
    }
  }
  if (d->nfav_emojis >= EXPR_MAX_FAV_EMOJIS) return false;
  strncpy(d->fav_emojis[d->nfav_emojis], key, sizeof d->fav_emojis[0] - 1);
  d->fav_emojis[d->nfav_emojis][sizeof d->fav_emojis[0] - 1] = '\0';
  d->nfav_emojis++;
  d->revision++;
  return true;
}

static void sort_sticker_frec(ccord_expr_data *d)
{
  for (size_t i = 1; i < d->nsticker_frec; i++) {
    expr_sticker_frec cur = d->sticker_frec[i];
    size_t j = i;
    while (j > 0 && d->sticker_frec[j - 1].score < cur.score) {
      d->sticker_frec[j] = d->sticker_frec[j - 1];
      j--;
    }
    d->sticker_frec[j] = cur;
  }
}

void ccord_store_seed_sticker_frecency(ccord_store *s, ccord_id sticker_id, int32_t score)
{
  if (!s || sticker_id == CCORD_ID_NONE) return;
  ccord_expr_data *d = expr_data_get(s, true);
  if (!d) return;
  for (size_t i = 0; i < d->nsticker_frec; i++) {
    if (d->sticker_frec[i].id == sticker_id) {
      if (score > d->sticker_frec[i].score) d->sticker_frec[i].score = score;
      sort_sticker_frec(d);
      d->revision++;
      return;
    }
  }
  if (d->nsticker_frec >= EXPR_MAX_FREC_STICKERS) return;
  d->sticker_frec[d->nsticker_frec++] = (expr_sticker_frec){.id = sticker_id, .score = score};
  sort_sticker_frec(d);
  d->revision++;
}

void ccord_store_record_sticker_use(ccord_store *s, ccord_id sticker_id)
{
  if (!s || sticker_id == CCORD_ID_NONE) return;
  ccord_expr_data *d = expr_data_get(s, true);
  if (!d) return;
  int32_t max_score = 100;
  for (size_t i = 0; i < d->nsticker_frec; i++) {
    if (d->sticker_frec[i].score >= max_score) max_score = d->sticker_frec[i].score + 10;
  }
  for (size_t i = 0; i < d->nsticker_frec; i++) {
    if (d->sticker_frec[i].id == sticker_id) {
      d->sticker_frec[i].score = max_score;
      sort_sticker_frec(d);
      d->revision++;
      return;
    }
  }
  if (d->nsticker_frec >= EXPR_MAX_FREC_STICKERS) d->nsticker_frec = EXPR_MAX_FREC_STICKERS - 1;
  d->sticker_frec[d->nsticker_frec++] = (expr_sticker_frec){.id = sticker_id, .score = max_score};
  sort_sticker_frec(d);
  d->revision++;
}

size_t ccord_store_frequent_sticker_count(const ccord_store *s)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return d ? d->nsticker_frec : 0;
}

ccord_id ccord_store_frequent_sticker_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return (d && i < d->nsticker_frec) ? d->sticker_frec[i].id : CCORD_ID_NONE;
}

size_t ccord_store_favorite_sticker_count(const ccord_store *s)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return d ? d->nfav_stickers : 0;
}

ccord_id ccord_store_favorite_sticker_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return (d && i < d->nfav_stickers) ? d->fav_stickers[i] : CCORD_ID_NONE;
}

bool ccord_store_is_favorite_sticker(const ccord_store *s, ccord_id sticker_id)
{
  const ccord_expr_data *d = expr_data_peek(s);
  if (!d || sticker_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < d->nfav_stickers; i++) {
    if (d->fav_stickers[i] == sticker_id) return true;
  }
  return false;
}

bool ccord_store_toggle_favorite_sticker(ccord_store *s, ccord_id sticker_id)
{
  if (!s || sticker_id == CCORD_ID_NONE) return false;
  ccord_expr_data *d = expr_data_get(s, true);
  if (!d) return false;
  for (size_t i = 0; i < d->nfav_stickers; i++) {
    if (d->fav_stickers[i] == sticker_id) {
      memmove(&d->fav_stickers[i], &d->fav_stickers[i + 1], (d->nfav_stickers - i - 1) * sizeof d->fav_stickers[0]);
      d->nfav_stickers--;
      d->revision++;
      return false;
    }
  }
  if (d->nfav_stickers >= EXPR_MAX_FAV_STICKERS) return false;
  d->fav_stickers[d->nfav_stickers++] = sticker_id;
  d->revision++;
  return true;
}

size_t ccord_store_sticker_pack_count(const ccord_store *s)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return d ? d->nsticker_packs : 0;
}

const ccord_sticker_pack *ccord_store_sticker_pack_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return (d && i < d->nsticker_packs) ? &d->sticker_packs[i] : nullptr;
}

// ---- GIF categories, trending, search & favorites ----

size_t ccord_store_gif_category_count(const ccord_store *s)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return d ? d->ngif_cats : 0;
}

const ccord_gif_category *ccord_store_gif_category_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return (d && d->gif_cats && i < d->ngif_cats) ? &d->gif_cats[i] : nullptr;
}

size_t ccord_store_gif_trending_count(const ccord_store *s)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return d ? d->ngif_trending : 0;
}

const ccord_gif_item *ccord_store_gif_trending_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return (d && d->gif_trending && i < d->ngif_trending) ? &d->gif_trending[i] : nullptr;
}

size_t ccord_store_gif_search_count(const ccord_store *s)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return d ? d->ngif_search : 0;
}

const ccord_gif_item *ccord_store_gif_search_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return (d && d->gif_search && i < d->ngif_search) ? &d->gif_search[i] : nullptr;
}

size_t ccord_store_favorite_gif_count(const ccord_store *s)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return d ? d->nfav_gifs : 0;
}

const ccord_gif_item *ccord_store_favorite_gif_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return (d && d->fav_gifs && i < d->nfav_gifs) ? &d->fav_gifs[i] : nullptr;
}

bool ccord_store_is_favorite_gif(const ccord_store *s, const char *url)
{
  const ccord_expr_data *d = expr_data_peek(s);
  if (!d || !d->fav_gifs || !url || !url[0]) return false;
  for (size_t i = 0; i < d->nfav_gifs; i++) {
    if (strcmp(d->fav_gifs[i].url, url) == 0) return true;
  }
  return false;
}

// Favorites are kept newest first: index 0 holds the highest order, as Discord shows them. A tie keeps insertion
// order, so an entry goes after every entry whose order is at least its own.
static void fav_gif_insert(ccord_expr_data *d, const ccord_gif_item *g)
{
  size_t at = 0;
  while (at < d->nfav_gifs && d->fav_gifs[at].order >= g->order) at++;
  memmove(&d->fav_gifs[at + 1], &d->fav_gifs[at], (d->nfav_gifs - at) * sizeof d->fav_gifs[0]);
  d->fav_gifs[at] = *g;
  d->nfav_gifs++;
}

bool ccord_store_toggle_favorite_gif(ccord_store *s, const ccord_gif_item *gif)
{
  if (!s || !gif || !gif->url[0]) return false;
  ccord_expr_data *d = expr_data_get(s, true);
  if (!d) return false;
  for (size_t i = 0; i < d->nfav_gifs; i++) {
    if (strcmp(d->fav_gifs[i].url, gif->url) == 0) {
      memmove(&d->fav_gifs[i], &d->fav_gifs[i + 1], (d->nfav_gifs - i - 1) * sizeof d->fav_gifs[0]);
      d->nfav_gifs--;
      d->revision++;
      return false;
    }
  }
  if (d->nfav_gifs >= EXPR_MAX_FAV_GIFS) return false;
  if (!expr_ensure_gif_buf(&d->fav_gifs, EXPR_MAX_FAV_GIFS)) return false;
  ccord_gif_item fav = *gif;
  if (!fav.src[0]) (void)snprintf(fav.src, sizeof fav.src, "%s", gif->url);
  if (fav.width == 0) fav.width = 320;
  if (fav.height == 0) fav.height = 240;
  // Lossless round-trip: keep the wire format/order when given; a brand-new favorite defaults to an image
  // (format 1) and max(existing order)+1, 1 for the first (the official client: order = max(orders) ?? 0, plus 1).
  if (fav.format == 0) fav.format = 1;
  if (gif->order == 0) {
    uint32_t max_order = 0;
    for (size_t i = 0; i < d->nfav_gifs; i++) {
      if (d->fav_gifs[i].order > max_order) max_order = d->fav_gifs[i].order;
    }
    fav.order = max_order + 1u;
  }
  fav_gif_insert(d, &fav);   // a new favorite with the highest order lands at index 0: drawn first
  d->revision++;
  return true;
}

// Moves an existing favorite to the position its new order gives (a settings update re-orders it). Returns true
// when the position changed.
bool ccord_store_set_favorite_gif_order(ccord_store *s, const char *url, uint32_t order)
{
  ccord_expr_data *d = expr_data_get(s, false);
  if (!d || !d->fav_gifs || !url || !url[0]) return false;
  for (size_t i = 0; i < d->nfav_gifs; i++) {
    if (strcmp(d->fav_gifs[i].url, url) != 0) continue;
    if (d->fav_gifs[i].order == order) return false;
    ccord_gif_item g = d->fav_gifs[i];
    g.order = order;
    memmove(&d->fav_gifs[i], &d->fav_gifs[i + 1], (d->nfav_gifs - i - 1) * sizeof d->fav_gifs[0]);
    d->nfav_gifs--;
    fav_gif_insert(d, &g);
    d->revision++;
    return true;
  }
  return false;
}

// ---- REST blobs ----

static const char *j_str(const ccord_json *obj, const char *key)
{
  const ccord_json *v = obj ? ccord_json_get(obj, key) : nullptr;
  return v ? ccord_json_string(v, nullptr) : nullptr;
}

static bool j_i64(const ccord_json *obj, const char *key, int64_t *out)
{
  const ccord_json *v = obj ? ccord_json_get(obj, key) : nullptr;
  return v ? ccord_json_int64(v, out) : false;
}

// Discord answers the GIF routes either with a bare array or with an object wrapping one. Accept both (and the
// wrapper names seen in the wild) so a shape change never silently empties the picker.
static const ccord_json *gif_list_from_json(const ccord_json *root)
{
  if (!root) return nullptr;
  ccord_json_type t = ccord_json_type_of(root);
  if (t == CCORD_JSON_ARRAY) return root;
  if (t != CCORD_JSON_OBJECT) return nullptr;
  static const char *const keys[] = {"gifs", "results", "data"};
  for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
    const ccord_json *v = ccord_json_get(root, keys[i]);
    if (v && ccord_json_type_of(v) == CCORD_JSON_ARRAY) return v;
  }
  return nullptr;
}

static void parse_gif_array(const ccord_json *arr, ccord_gif_item *out, size_t cap, size_t *out_n)
{
  *out_n = 0;
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) return;
  size_t len = ccord_json_array_len(arr);
  for (size_t i = 0; i < len && *out_n < cap; i++) {
    const ccord_json *it = ccord_json_array_at(arr, i);
    if (!it || ccord_json_type_of(it) != CCORD_JSON_OBJECT) continue;
    const char *url = j_str(it, "url");
    const char *gif_src = j_str(it, "gif_src");
    const char *src = j_str(it, "src");
    if (!gif_src || !gif_src[0]) gif_src = src;
    if (!url || !url[0]) url = gif_src;
    if (!gif_src || !gif_src[0]) gif_src = url;
    if (!url || !url[0]) continue;
    ccord_gif_item *g = &out[(*out_n)++];
    memset(g, 0, sizeof *g);
    strncpy(g->url, url, sizeof g->url - 1);
    strncpy(g->src, gif_src ? gif_src : url, sizeof g->src - 1);
    int64_t w = 0, h = 0;
    if (j_i64(it, "width", &w) && w > 0) g->width = (uint32_t)w;
    else g->width = 320;
    if (j_i64(it, "height", &h) && h > 0) g->height = (uint32_t)h;
    else g->height = 240;
    int64_t fmt = 0;
    if (j_i64(it, "format", &fmt) && fmt > 0) g->format = (uint32_t)fmt;   // kept verbatim for lossless favorites
  }
}

static uint64_t parse_u64_str(const char *s)
{
  if (!s || !s[0]) return 0;
  uint64_t v = 0;
  for (size_t i = 0; s[i] >= '0' && s[i] <= '9'; i++) {
    v = v * 10u + (uint64_t)(s[i] - '0');
  }
  return v;
}

static void parse_expr_blob_json(ccord_expr_data *d, unsigned kind, const char *body, size_t len)
{
  if (!d || !body || len == 0) return;
  ccord_arena ar;
  ccord_arena_init(&ar, 0);
  ccord_json_limits lim = {.max_depth = 16, .max_nodes = 32768, .max_string = 65536};
  ccord_error jerr;
  const ccord_json *root = ccord_json_parse(&ar, body, len, &lim, &jerr);
  if (!root) {
    ccord_arena_free(&ar);
    return;
  }
  if (kind == 1u) { // CCORD_EXPR_GIF_CATEGORIES
    if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
      const ccord_json *cats = ccord_json_get(root, "categories");
      if (cats && ccord_json_type_of(cats) == CCORD_JSON_ARRAY) {
        d->ngif_cats = 0;
        size_t n = ccord_json_array_len(cats);
        if (n > 0 && expr_ensure_gif_cats(d)) {
          for (size_t i = 0; i < n && d->ngif_cats < EXPR_MAX_GIF_CATS; i++) {
            const ccord_json *c = ccord_json_array_at(cats, i);
            if (!c || ccord_json_type_of(c) != CCORD_JSON_OBJECT) continue;
            const char *name = j_str(c, "name");
            const char *src = j_str(c, "src");
            if (!name || !name[0]) continue;
            ccord_gif_category *gc = &d->gif_cats[d->ngif_cats++];
            memset(gc, 0, sizeof *gc);
            strncpy(gc->name, name, sizeof gc->name - 1);
            if (src) strncpy(gc->src, src, sizeof gc->src - 1);
          }
        }
      }
    }
    // /gifs/trending also carries the first trending page; use it only until trending-gifs answers with rows.
    const ccord_json *gifs = gif_list_from_json(root);
    if (gifs && d->ngif_trending == 0 && ccord_json_array_len(gifs) > 0 &&
        expr_ensure_gif_buf(&d->gif_trending, EXPR_MAX_GIFS)) {
      parse_gif_array(gifs, d->gif_trending, EXPR_MAX_GIFS, &d->ngif_trending);
    }
  } else if (kind == 2u) { // CCORD_EXPR_GIF_TRENDING
    const ccord_json *arr = gif_list_from_json(root);
    // An empty answer must not wipe the fallback rows already ingested from /gifs/trending.
    if (arr && ccord_json_array_len(arr) > 0 && expr_ensure_gif_buf(&d->gif_trending, EXPR_MAX_GIFS)) {
      parse_gif_array(arr, d->gif_trending, EXPR_MAX_GIFS, &d->ngif_trending);
    }
  } else if (kind == 3u) { // CCORD_EXPR_GIF_SEARCH
    const ccord_json *arr = gif_list_from_json(root);
    if (arr && ccord_json_array_len(arr) > 0 && expr_ensure_gif_buf(&d->gif_search, EXPR_MAX_GIFS)) {
      parse_gif_array(arr, d->gif_search, EXPR_MAX_GIFS, &d->ngif_search);
    } else {
      d->ngif_search = 0;
    }
  } else if (kind == 4u || kind == 5u) { // CCORD_EXPR_STICKER_PACKS
    const ccord_json *packs = root;
    if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
      packs = ccord_json_get(root, "sticker_packs");
    }
    if (packs && ccord_json_type_of(packs) == CCORD_JSON_ARRAY) {
      expr_sticker_packs_free(d);
      size_t np = ccord_json_array_len(packs);
      for (size_t p = 0; p < np && d->nsticker_packs < EXPR_MAX_STICKER_PACKS; p++) {
        const ccord_json *pk_node = ccord_json_array_at(packs, p);
        if (!pk_node || ccord_json_type_of(pk_node) != CCORD_JSON_OBJECT) continue;
        const char *pk_name = j_str(pk_node, "name");
        const char *pk_id_s = j_str(pk_node, "id");
        const ccord_json *st_arr = ccord_json_get(pk_node, "stickers");
        if (!pk_name || !st_arr || ccord_json_type_of(st_arr) != CCORD_JSON_ARRAY) continue;
        size_t ns = ccord_json_array_len(st_arr);
        if (ns == 0) continue;
        if (ns > 64) ns = 64;
        ccord_sticker *stickers = ccord_calloc(ns, sizeof *stickers);
        if (!stickers) break;
        size_t valid_s = 0;
        for (size_t k = 0; k < ns; k++) {
          const ccord_json *sn = ccord_json_array_at(st_arr, k);
          if (!sn || ccord_json_type_of(sn) != CCORD_JSON_OBJECT) continue;
          const char *sid_s = j_str(sn, "id");
          const char *sname = j_str(sn, "name");
          const char *sdesc = j_str(sn, "description");
          const char *stags = j_str(sn, "tags");
          int64_t fmt = 1;
          (void)j_i64(sn, "format_type", &fmt);
          ccord_id sid = parse_u64_str(sid_s);
          if (sid == CCORD_ID_NONE || !sname || !sname[0]) continue;
          ccord_sticker *dst = &stickers[valid_s];
          dst->id = sid;
          dst->guild_id = CCORD_ID_NONE;
          dst->name = expr_dup_or_empty(sname, CCORD_NAME_MAX_BYTES);
          dst->description = expr_dup_or_empty(sdesc, 256);
          dst->tags = expr_dup_or_empty(stags, 200);
          dst->format = (fmt >= 1 && fmt <= 4) ? (ccord_sticker_format)fmt : CCORD_STICKER_PNG;
          dst->available = true;
          valid_s++;
        }
        if (valid_s == 0) {
          ccord_free(stickers);
          continue;
        }
        ccord_sticker_pack *pk = &d->sticker_packs[d->nsticker_packs++];
        memset(pk, 0, sizeof *pk);
        pk->id = parse_u64_str(pk_id_s);
        strncpy(pk->name, pk_name, sizeof pk->name - 1);
        pk->stickers = stickers;
        pk->sticker_count = valid_s;
      }
    }
  }
  ccord_arena_free(&ar);
}

static void norm_expr_key(const char *key, char out[static EXPR_KEY_MAX + 1u])
{
  if (!key || key[0] == '\0') {
    out[0] = '\0';
    return;
  }
  size_t n = 0;
  while (n < EXPR_KEY_MAX && key[n] != '\0') n++;
  while (n > 0 && !ccord_utf8_valid(key, n)) n--;
  if (n > 0) memcpy(out, key, n);
  out[n] = '\0';
}

static const expr_blob *blob_find(const ccord_expr_data *d, unsigned kind, const char *key)
{
  if (!d) return nullptr;
  char nkey[EXPR_KEY_MAX + 1u];
  norm_expr_key(key, nkey);
  for (size_t i = 0; i < d->nblobs; i++) {
    const expr_blob *b = &d->blobs[i];
    if (b->kind == kind && strcmp(b->key, nkey) == 0) return b;
  }
  return nullptr;
}

static expr_blob *blob_get(ccord_expr_data *d, unsigned kind, const char *key)
{
  char nkey[EXPR_KEY_MAX + 1u];
  norm_expr_key(key, nkey);
  expr_blob *b = nullptr;
  for (size_t i = 0; i < d->nblobs; i++) {
    if (d->blobs[i].kind == kind && strcmp(d->blobs[i].key, nkey) == 0) {
      b = &d->blobs[i];
      break;
    }
  }
  if (b) return b;
  if (d->nblobs == EXPR_BLOB_MAX) {
    size_t evict = 0;
    for (size_t i = 1; i < d->nblobs; i++) {
      if (d->blobs[i].tick < d->blobs[evict].tick) evict = i;
    }
    if (d->blob_bytes >= d->blobs[evict].len) d->blob_bytes -= d->blobs[evict].len;
    else d->blob_bytes = 0;
    expr_blob_free(&d->blobs[evict]);
    d->blobs[evict] = d->blobs[d->nblobs - 1];
    memset(&d->blobs[d->nblobs - 1], 0, sizeof d->blobs[0]);
    d->nblobs--;
  }
  const char *k = expr_dup_or_empty(nkey, EXPR_KEY_MAX);
  if (!k) return nullptr;
  b = &d->blobs[d->nblobs++];
  *b = (expr_blob){.kind = kind, .key = k};
  return b;
}

ccord_expr_blob_state ccord_store_expr_blob_state(const ccord_store *s, unsigned kind, const char *key)
{
  const expr_blob *b = blob_find(expr_data_peek(s), kind, key);
  return b ? b->state : CCORD_EXPR_BLOB_NONE;
}

void ccord_store_expr_blob_mark_pending(ccord_store *s, unsigned kind, const char *key)
{
  ccord_expr_data *d = expr_data_get(s, true);
  if (!d) return;
  expr_blob *b = blob_get(d, kind, key);
  if (!b) return;
  if (b->state == CCORD_EXPR_BLOB_NONE || b->state == CCORD_EXPR_BLOB_FAILED) b->state = CCORD_EXPR_BLOB_PENDING;
  b->tick = ++d->tick;
}

ccord_status ccord_store_set_expr_blob(ccord_store *s, unsigned kind, const char *key, const char *body, size_t len,
                                       bool ok, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null store");
  if (ok && len > 0 && !body) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null expression blob");
  if (len > EXPR_BLOB_BYTES_MAX) ok = false;
  ccord_expr_data *d = expr_data_get(s, true);
  if (!d) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  expr_blob *b = blob_get(d, kind, key);
  if (!b) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  if (d->blob_bytes >= b->len) d->blob_bytes -= b->len;
  else d->blob_bytes = 0;
  ccord_free(b->body);
  b->body = nullptr;
  b->len = 0;
  if (ok) {
    // Keep total cached REST blob bytes within EXPR_BLOB_TOTAL_BYTES_MAX via LRU eviction of other slots.
    size_t bi = (size_t)(b - d->blobs);
    while (d->blob_bytes + len > EXPR_BLOB_TOTAL_BYTES_MAX && d->nblobs > 1u) {
      size_t evict = SIZE_MAX;
      for (size_t i = 0; i < d->nblobs; i++) {
        if (i == bi || d->blobs[i].len == 0) continue;
        if (evict == SIZE_MAX || d->blobs[i].tick < d->blobs[evict].tick) evict = i;
      }
      if (evict == SIZE_MAX) break;
      if (d->blob_bytes >= d->blobs[evict].len) d->blob_bytes -= d->blobs[evict].len;
      else d->blob_bytes = 0;
      expr_blob_free(&d->blobs[evict]);
      size_t last = d->nblobs - 1u;
      if (evict != last) {
        d->blobs[evict] = d->blobs[last];
        if (bi == last) bi = evict;
      }
      memset(&d->blobs[last], 0, sizeof d->blobs[0]);
      d->nblobs--;
    }
    b = &d->blobs[bi];
    b->body = ccord_alloc_array(len + 1u, 1);
    if (!b->body) {
      b->state = CCORD_EXPR_BLOB_FAILED;
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    if (len && body) memcpy(b->body, body, len);
    b->body[len] = '\0';
    b->len = len;
    d->blob_bytes += len;
    b->state = CCORD_EXPR_BLOB_READY;
    parse_expr_blob_json(d, kind, b->body, b->len);
  } else {
    b->state = CCORD_EXPR_BLOB_FAILED;
  }
  b->tick = ++d->tick;
  d->revision++;
  return CCORD_OK;
}

const char *ccord_store_expr_blob(const ccord_store *s, unsigned kind, const char *key, size_t *out_len)
{
  const expr_blob *b = blob_find(expr_data_peek(s), kind, key);
  if (!b || b->state != CCORD_EXPR_BLOB_READY) {
    if (out_len) *out_len = 0;
    return nullptr;
  }
  if (out_len) *out_len = b->len;
  return b->body;
}

ccord_status ccord_store_set_raw_settings_proto(ccord_store *s, const uint8_t *data, size_t len, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null store");
  if (len > EXPR_BLOB_BYTES_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "store: settings proto too large");
  if (len > 0 && !data) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null settings proto");
  ccord_expr_data *d = expr_data_get(s, true);
  if (!d) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  uint8_t *copy = nullptr;
  if (len > 0) {
    copy = ccord_alloc_array(len, 1);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    memcpy(copy, data, len);
  }
  ccord_free(d->settings_proto);
  d->settings_proto = copy;
  d->settings_proto_len = len;
  d->revision++;
  return CCORD_OK;
}

const uint8_t *ccord_store_get_raw_settings_proto(const ccord_store *s, size_t *out_len)
{
  const ccord_expr_data *d = expr_data_peek(s);
  if (!d || !d->settings_proto) {
    if (out_len) *out_len = 0;
    return nullptr;
  }
  if (out_len) *out_len = d->settings_proto_len;
  return d->settings_proto;
}

uint64_t ccord_store_expr_revision(const ccord_store *s)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return d ? d->revision : 0;
}
