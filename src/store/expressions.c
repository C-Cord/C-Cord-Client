// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/idmap.h>
#include <ccord/core/json.h>

#include "expr_data.h"

#define EXPR_BLOB_MAX 96u
#define EXPR_BLOB_BYTES_MAX (6u * 1024u * 1024u)
#define EXPR_BLOB_TOTAL_BYTES_MAX (8u * 1024u * 1024u)
#define EXPR_KEY_MAX 160u
#define EXPR_EMOJI_IDX_BITS 16u
#define EXPR_EMOJI_IDX_MASK (((size_t)1u << EXPR_EMOJI_IDX_BITS) - 1u)
#define EXPR_MAX_FREC_EMOJIS 128u
#define EXPR_MAX_FAV_EMOJIS 128u
#define EXPR_MAX_FREC_STICKERS 64u
#define EXPR_MAX_FAV_STICKERS 64u
#define EXPR_MAX_STICKER_PACKS 32u
#define EXPR_MAX_GIF_CATS 64u
#define EXPR_MAX_GIFS 128u
#define EXPR_MAX_FAV_GIFS 128u

typedef struct expr_emoji_frec {
  char key[64];
  int32_t score;
} expr_emoji_frec;

typedef struct expr_sticker_frec {
  ccord_id id;
  int32_t score;
} expr_sticker_frec;

typedef struct expr_guild {
  ccord_id guild_id;
  ccord_emoji *emojis;
  size_t nemojis, capemojis;
  ccord_sticker *stickers;
  size_t nstickers, capstickers;
  ccord_soundboard_sound *sounds;
  size_t nsounds, capsounds;
  bool emojis_admin_loaded;
  bool stickers_admin_loaded;
  bool soundboard_admin_loaded;
} expr_guild;

typedef struct expr_blob {
  unsigned kind;
  const char *key;
  char *body;
  size_t len;
  ccord_expr_blob_state state;
  uint64_t tick;
} expr_blob;

struct ccord_expr_data {
  expr_guild *g;
  size_t ng, capg;
  ccord_idmap guild_by_id;
  bool guild_ix_ok;
  ccord_idmap emoji_by_id;   // emoji_id -> (guild_idx << 16) | emoji_idx
  bool emoji_ix_ok;
  expr_blob blobs[EXPR_BLOB_MAX];
  size_t nblobs;
  size_t blob_bytes;
  expr_emoji_frec emoji_frec[EXPR_MAX_FREC_EMOJIS];
  size_t nemoji_frec;
  char fav_emojis[EXPR_MAX_FAV_EMOJIS][64];
  size_t nfav_emojis;
  expr_sticker_frec sticker_frec[EXPR_MAX_FREC_STICKERS];
  size_t nsticker_frec;
  ccord_id fav_stickers[EXPR_MAX_FAV_STICKERS];
  size_t nfav_stickers;
  ccord_sticker_pack sticker_packs[EXPR_MAX_STICKER_PACKS];
  size_t nsticker_packs;
  ccord_gif_category *gif_cats;   // lazily allocated (EXPR_MAX_GIF_CATS) on first write
  size_t ngif_cats;
  ccord_gif_item *gif_trending;   // lazily allocated (EXPR_MAX_GIFS) on first write
  size_t ngif_trending;
  ccord_gif_item *gif_search;     // lazily allocated (EXPR_MAX_GIFS) on first write
  size_t ngif_search;
  ccord_gif_item *fav_gifs;       // lazily allocated (EXPR_MAX_FAV_GIFS) on first write
  size_t nfav_gifs;
  uint8_t *settings_proto;   // raw FrecencyUserSettings bytes, kept losslessly for the favorite-GIF PATCH
  size_t settings_proto_len;
  uint64_t revision, tick;
};

static const char k_empty_str[] = "";

static void free_cstr(const char **p)
{
  if (!p || !*p || *p == k_empty_str) {
    if (p) *p = nullptr;
    return;
  }
  union { const char *c; void *v; } u = {.c = *p};
  ccord_free(u.v);
  *p = nullptr;
}

static void emoji_item_free(ccord_emoji *e)
{
  free_cstr(&e->name);
  free_cstr(&e->user_name);
}

static void sticker_item_free(ccord_sticker *e)
{
  free_cstr(&e->name);
  free_cstr(&e->description);
  free_cstr(&e->tags);
  free_cstr(&e->user_name);
}

static void sound_item_free(ccord_soundboard_sound *e)
{
  free_cstr(&e->name);
  free_cstr(&e->emoji_name);
  free_cstr(&e->user_name);
}

static void guild_emojis_free(expr_guild *g)
{
  for (size_t i = 0; i < g->nemojis; i++) emoji_item_free(&g->emojis[i]);
  ccord_free(g->emojis);
  g->emojis = nullptr;
  g->nemojis = 0;
  g->capemojis = 0;
}

static void guild_stickers_free(expr_guild *g)
{
  for (size_t i = 0; i < g->nstickers; i++) sticker_item_free(&g->stickers[i]);
  ccord_free(g->stickers);
  g->stickers = nullptr;
  g->nstickers = 0;
  g->capstickers = 0;
}

static void guild_sounds_free(expr_guild *g)
{
  for (size_t i = 0; i < g->nsounds; i++) sound_item_free(&g->sounds[i]);
  ccord_free(g->sounds);
  g->sounds = nullptr;
  g->nsounds = 0;
  g->capsounds = 0;
}

static void blob_free(expr_blob *b)
{
  free_cstr(&b->key);
  ccord_free(b->body);
  memset(b, 0, sizeof *b);
}

static void sticker_packs_free(ccord_expr_data *d)
{
  for (size_t p = 0; p < d->nsticker_packs; p++) {
    ccord_sticker_pack *pk = &d->sticker_packs[p];
    for (size_t i = 0; i < pk->sticker_count; i++) sticker_item_free(&pk->stickers[i]);
    ccord_free(pk->stickers);
    pk->stickers = nullptr;
    pk->sticker_count = 0;
  }
  d->nsticker_packs = 0;
}

void ccord_expr_data_free(ccord_expr_data *d)
{
  if (!d) return;
  for (size_t i = 0; i < d->ng; i++) {
    guild_emojis_free(&d->g[i]);
    guild_stickers_free(&d->g[i]);
    guild_sounds_free(&d->g[i]);
  }
  ccord_free(d->g);
  ccord_idmap_free(&d->guild_by_id);
  ccord_idmap_free(&d->emoji_by_id);
  sticker_packs_free(d);
  for (size_t i = 0; i < d->nblobs; i++) blob_free(&d->blobs[i]);
  ccord_free(d->gif_cats);
  ccord_free(d->gif_trending);
  ccord_free(d->gif_search);
  ccord_free(d->fav_gifs);
  ccord_free(d->settings_proto);
  ccord_free(d);
}

static ccord_expr_data *data_get(ccord_store *s, bool create)
{
  ccord_expr_data **slot = ccord_store_expr_slot_(s);
  if (!slot) return nullptr;
  if (!*slot && create) {
    *slot = ccord_calloc(1, sizeof **slot);
    if (*slot) {
      (*slot)->guild_ix_ok = true;
      (*slot)->emoji_ix_ok = true;
    }
  }
  return *slot;
}

static const ccord_expr_data *data_peek(const ccord_store *s)
{
  ccord_expr_data *const *slot = ccord_store_expr_slot_const_(s);
  return slot ? *slot : nullptr;
}

static expr_guild *guild_find(const ccord_expr_data *d, ccord_id gid)
{
  if (!d) return nullptr;
  if (d->guild_ix_ok && gid != CCORD_ID_NONE) {
    size_t at = 0;
    return ccord_idmap_find(&d->guild_by_id, gid, &at) ? &d->g[at] : nullptr;
  }
  for (size_t i = 0; i < d->ng; i++) {
    if (d->g[i].guild_id == gid) return &d->g[i];
  }
  return nullptr;
}

static expr_guild *guild_get(ccord_expr_data *d, ccord_id gid)
{
  expr_guild *g = guild_find(d, gid);
  if (g) return g;
  if (d->ng == d->capg) {
    size_t ncap = d->capg ? d->capg * 2 : 16;
    expr_guild *n = ccord_realloc_array(d->g, ncap, sizeof *n);
    if (!n) return nullptr;
    d->g = n;
    d->capg = ncap;
  }
  size_t idx = d->ng++;
  g = &d->g[idx];
  *g = (expr_guild){.guild_id = gid};
  if (d->guild_ix_ok && gid != CCORD_ID_NONE && !ccord_idmap_put(&d->guild_by_id, gid, idx)) {
    d->guild_ix_ok = false;
  }
  return g;
}

static void emoji_index_add(ccord_expr_data *d, size_t gi, size_t ei)
{
  if (!d->emoji_ix_ok) return;
  if (ei > EXPR_EMOJI_IDX_MASK || gi >= (SIZE_MAX >> EXPR_EMOJI_IDX_BITS)) {
    d->emoji_ix_ok = false;
    return;
  }
  ccord_id eid = d->g[gi].emojis[ei].id;
  if (eid == CCORD_ID_NONE) return;
  size_t existing = 0;
  if (!ccord_idmap_find(&d->emoji_by_id, eid, &existing)) {
    size_t packed = (gi << EXPR_EMOJI_IDX_BITS) | ei;
    if (!ccord_idmap_put(&d->emoji_by_id, eid, packed)) d->emoji_ix_ok = false;
  }
}

static void emoji_reindex_all(ccord_expr_data *d)
{
  ccord_idmap_clear(&d->emoji_by_id);
  size_t total = 0;
  for (size_t gi = 0; gi < d->ng; gi++) total += d->g[gi].nemojis;
  d->emoji_ix_ok = ccord_idmap_reserve(&d->emoji_by_id, total);
  for (size_t gi = 0; d->emoji_ix_ok && gi < d->ng; gi++) {
    for (size_t ei = 0; d->emoji_ix_ok && ei < d->g[gi].nemojis; ei++) {
      emoji_index_add(d, gi, ei);
    }
  }
}

static ccord_gif_category *ensure_gif_cats(ccord_expr_data *d)
{
  if (!d->gif_cats) d->gif_cats = ccord_calloc(EXPR_MAX_GIF_CATS, sizeof *d->gif_cats);
  return d->gif_cats;
}

static ccord_gif_item *ensure_gif_buf(ccord_gif_item **slot, size_t cap)
{
  if (!*slot) *slot = ccord_calloc(cap, sizeof **slot);
  return *slot;
}

static const char *dup_or_empty(const char *s, size_t max)
{
  if (!s || s[0] == '\0' || max == 0) return k_empty_str;
  size_t n = 0;
  while (n < max && s[n] != '\0') n++;
  while (n > 0 && n < max && ((unsigned char)s[n] & 0xC0u) == 0x80u) n--;   // never cut a UTF-8 sequence
  if (n == 0) return k_empty_str;
  return ccord_strndup(s, n);
}

static bool copy_emoji_item(ccord_emoji *dst, const ccord_emoji *src, ccord_id guild_id, const ccord_emoji *prev)
{
  *dst = *src;
  dst->guild_id = guild_id;
  dst->name = nullptr;
  dst->user_name = nullptr;
  const char *uname = src->user_name;
  if (dst->user_id == CCORD_ID_NONE && prev && prev->user_id != CCORD_ID_NONE) {
    dst->user_id = prev->user_id;
    uname = prev->user_name;
  }
  dst->name = dup_or_empty(src->name, CCORD_NAME_MAX_BYTES);
  dst->user_name = dup_or_empty(uname, CCORD_NAME_MAX_BYTES);
  if (!dst->name || !dst->user_name) {
    emoji_item_free(dst);
    return false;
  }
  return true;
}

static bool copy_sticker_item(ccord_sticker *dst, const ccord_sticker *src, ccord_id guild_id, const ccord_sticker *prev)
{
  *dst = *src;
  dst->guild_id = guild_id;
  dst->name = nullptr;
  dst->description = nullptr;
  dst->tags = nullptr;
  dst->user_name = nullptr;
  const char *uname = src->user_name;
  if (dst->user_id == CCORD_ID_NONE && prev && prev->user_id != CCORD_ID_NONE) {
    dst->user_id = prev->user_id;
    uname = prev->user_name;
  }
  dst->name = dup_or_empty(src->name, CCORD_NAME_MAX_BYTES);
  dst->description = dup_or_empty(src->description, 256);
  dst->tags = dup_or_empty(src->tags, 200);
  dst->user_name = dup_or_empty(uname, CCORD_NAME_MAX_BYTES);
  if (!dst->name || !dst->description || !dst->tags || !dst->user_name) {
    sticker_item_free(dst);
    return false;
  }
  return true;
}

static bool copy_sound_item(ccord_soundboard_sound *dst, const ccord_soundboard_sound *src, ccord_id guild_id,
                            const ccord_soundboard_sound *prev)
{
  *dst = *src;
  dst->guild_id = guild_id;
  if (dst->volume_pct > 100u) dst->volume_pct = 100u;
  dst->name = nullptr;
  dst->emoji_name = nullptr;
  dst->user_name = nullptr;
  const char *uname = src->user_name;
  if (dst->user_id == CCORD_ID_NONE && prev && prev->user_id != CCORD_ID_NONE) {
    dst->user_id = prev->user_id;
    uname = prev->user_name;
  }
  dst->name = dup_or_empty(src->name, CCORD_NAME_MAX_BYTES);
  dst->emoji_name = dup_or_empty(src->emoji_name, CCORD_EMOJI_MAX_BYTES);
  dst->user_name = dup_or_empty(uname, CCORD_NAME_MAX_BYTES);
  if (!dst->name || !dst->emoji_name || !dst->user_name) {
    sound_item_free(dst);
    return false;
  }
  return true;
}

static const ccord_emoji *find_prev_emoji(const ccord_emoji *arr, size_t n, ccord_id id)
{
  for (size_t i = 0; i < n; i++) {
    if (arr[i].id == id) return &arr[i];
  }
  return nullptr;
}

static const ccord_sticker *find_prev_sticker(const ccord_sticker *arr, size_t n, ccord_id id)
{
  for (size_t i = 0; i < n; i++) {
    if (arr[i].id == id) return &arr[i];
  }
  return nullptr;
}

static const ccord_soundboard_sound *find_prev_sound(const ccord_soundboard_sound *arr, size_t n, ccord_id id)
{
  for (size_t i = 0; i < n; i++) {
    if (arr[i].id == id) return &arr[i];
  }
  return nullptr;
}

ccord_status ccord_store_set_guild_emojis(ccord_store *s, ccord_id guild_id, const ccord_emoji *emojis, size_t n,
                                          ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: emoji guild");
  if (n > 0 && !emojis) return ccord_error_set(err, CCORD_ERR_INVALID, "store: emoji list");
  if (n > CCORD_MAX_EMOJIS_PER_GUILD) n = CCORD_MAX_EMOJIS_PER_GUILD;
  ccord_expr_data *d = data_get(s, true);
  if (!d) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  expr_guild *g = guild_get(d, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  size_t gi = (size_t)(g - d->g);
  size_t had_prev = g->nemojis;
  ccord_emoji *fresh = nullptr;
  size_t nfresh = 0;
  if (n > 0) {
    fresh = ccord_calloc(n, sizeof *fresh);
    if (!fresh) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    ccord_idmap prev_map = {0};
    bool use_prev_map = false;
    if (had_prev > 16u && !d->emoji_ix_ok && ccord_idmap_reserve(&prev_map, had_prev)) {
      use_prev_map = true;
      for (size_t i = 0; i < had_prev; i++) {
        if (!ccord_idmap_put(&prev_map, g->emojis[i].id, i)) {
          use_prev_map = false;
          break;
        }
      }
    }
    for (size_t i = 0; i < n; i++) {
      if (emojis[i].id == CCORD_ID_NONE) continue;
      const ccord_emoji *prev = nullptr;
      if (had_prev > 0 && emojis[i].user_id == CCORD_ID_NONE) {
        if (d->emoji_ix_ok) {
          size_t packed = 0;
          if (ccord_idmap_find(&d->emoji_by_id, emojis[i].id, &packed)) {
            size_t pgi = packed >> EXPR_EMOJI_IDX_BITS;
            size_t pei = packed & EXPR_EMOJI_IDX_MASK;
            if (pgi == gi && pei < had_prev && g->emojis[pei].id == emojis[i].id) {
              prev = &g->emojis[pei];
            } else {
              prev = find_prev_emoji(g->emojis, had_prev, emojis[i].id);
            }
          }
        } else if (use_prev_map) {
          size_t pei = 0;
          if (ccord_idmap_find(&prev_map, emojis[i].id, &pei)) prev = &g->emojis[pei];
        } else {
          prev = find_prev_emoji(g->emojis, had_prev, emojis[i].id);
        }
      }
      if (!copy_emoji_item(&fresh[nfresh], &emojis[i], guild_id, prev)) break;
      nfresh++;
    }
    ccord_idmap_free(&prev_map);
  }
  guild_emojis_free(g);
  g->emojis = fresh;
  g->nemojis = nfresh;
  g->capemojis = fresh ? n : 0;
  if (had_prev == 0) {
    for (size_t i = 0; d->emoji_ix_ok && i < nfresh; i++) emoji_index_add(d, gi, i);
  } else {
    emoji_reindex_all(d);
  }
  d->revision++;
  ccord_store_expr_mutated_(s);
  return CCORD_OK;
}

ccord_status ccord_store_set_guild_emojis_admin(ccord_store *s, ccord_id guild_id, const ccord_emoji *emojis, size_t n,
                                                ccord_error *err)
{
  ccord_status st = ccord_store_set_guild_emojis(s, guild_id, emojis, n, err);
  if (st == CCORD_OK) {
    expr_guild *g = guild_find(data_get(s, false), guild_id);
    if (g) g->emojis_admin_loaded = true;
  }
  return st;
}

bool ccord_store_guild_emojis_admin_loaded(const ccord_store *s, ccord_id guild_id)
{
  const expr_guild *g = guild_find(data_peek(s), guild_id);
  return g && g->emojis_admin_loaded;
}

ccord_status ccord_store_upsert_guild_emoji(ccord_store *s, ccord_id guild_id, const ccord_emoji *emoji,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE || !emoji || emoji->id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: emoji upsert");
  }
  ccord_expr_data *d = data_get(s, true);
  if (!d) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  expr_guild *g = guild_get(d, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  size_t gi = (size_t)(g - d->g);
  size_t found = SIZE_MAX;
  if (d->emoji_ix_ok) {
    size_t packed = 0;
    if (ccord_idmap_find(&d->emoji_by_id, emoji->id, &packed)) {
      size_t pgi = packed >> EXPR_EMOJI_IDX_BITS;
      size_t pei = packed & EXPR_EMOJI_IDX_MASK;
      if (pgi == gi && pei < g->nemojis && g->emojis[pei].id == emoji->id) {
        found = pei;
      } else {
        for (size_t i = 0; i < g->nemojis; i++) {
          if (g->emojis[i].id == emoji->id) { found = i; break; }
        }
      }
    }
  } else {
    for (size_t i = 0; i < g->nemojis; i++) {
      if (g->emojis[i].id == emoji->id) { found = i; break; }
    }
  }
  if (found != SIZE_MAX) {
    ccord_emoji copy;
    if (!copy_emoji_item(&copy, emoji, guild_id, &g->emojis[found])) {
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    emoji_item_free(&g->emojis[found]);
    g->emojis[found] = copy;
    d->revision++;
    ccord_store_expr_mutated_(s);
    return CCORD_OK;
  }
  if (g->nemojis >= CCORD_MAX_EMOJIS_PER_GUILD) return ccord_error_set(err, CCORD_ERR_INVALID, "store: emoji limit");
  if (g->nemojis == g->capemojis) {
    size_t ncap = g->capemojis ? g->capemojis * 2u : 16u;
    if (ncap > CCORD_MAX_EMOJIS_PER_GUILD) ncap = CCORD_MAX_EMOJIS_PER_GUILD;
    ccord_emoji *grown = ccord_realloc_array(g->emojis, ncap, sizeof *grown);
    if (!grown) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    g->emojis = grown;
    g->capemojis = ncap;
  }
  if (!copy_emoji_item(&g->emojis[g->nemojis], emoji, guild_id, nullptr)) {
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  size_t new_ei = g->nemojis++;
  emoji_index_add(d, gi, new_ei);
  d->revision++;
  ccord_store_expr_mutated_(s);
  return CCORD_OK;
}

bool ccord_store_remove_guild_emoji(ccord_store *s, ccord_id guild_id, ccord_id emoji_id)
{
  if (!s || guild_id == CCORD_ID_NONE || emoji_id == CCORD_ID_NONE) return false;
  ccord_expr_data *d = data_get(s, false);
  expr_guild *g = guild_find(d, guild_id);
  if (!g) return false;
  size_t gi = (size_t)(g - d->g);
  size_t found = SIZE_MAX;
  if (d->emoji_ix_ok) {
    size_t packed = 0;
    if (ccord_idmap_find(&d->emoji_by_id, emoji_id, &packed)) {
      size_t pgi = packed >> EXPR_EMOJI_IDX_BITS;
      size_t pei = packed & EXPR_EMOJI_IDX_MASK;
      if (pgi == gi && pei < g->nemojis && g->emojis[pei].id == emoji_id) {
        found = pei;
      } else {
        for (size_t i = 0; i < g->nemojis; i++) {
          if (g->emojis[i].id == emoji_id) { found = i; break; }
        }
      }
    }
  } else {
    for (size_t i = 0; i < g->nemojis; i++) {
      if (g->emojis[i].id == emoji_id) { found = i; break; }
    }
  }
  if (found == SIZE_MAX) return false;
  emoji_item_free(&g->emojis[found]);
  memmove(&g->emojis[found], &g->emojis[found + 1], (g->nemojis - found - 1u) * sizeof *g->emojis);
  g->nemojis--;
  emoji_reindex_all(d);
  d->revision++;
  ccord_store_expr_mutated_(s);
  return true;
}

ccord_status ccord_store_set_guild_stickers(ccord_store *s, ccord_id guild_id, const ccord_sticker *stickers, size_t n,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: sticker guild");
  if (n > 0 && !stickers) return ccord_error_set(err, CCORD_ERR_INVALID, "store: sticker list");
  if (n > CCORD_MAX_STICKERS_PER_GUILD) n = CCORD_MAX_STICKERS_PER_GUILD;
  ccord_expr_data *d = data_get(s, true);
  if (!d) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  expr_guild *g = guild_get(d, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  ccord_sticker *fresh = nullptr;
  size_t nfresh = 0;
  if (n > 0) {
    fresh = ccord_calloc(n, sizeof *fresh);
    if (!fresh) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    for (size_t i = 0; i < n; i++) {
      if (stickers[i].id == CCORD_ID_NONE) continue;
      const ccord_sticker *prev = find_prev_sticker(g->stickers, g->nstickers, stickers[i].id);
      if (!copy_sticker_item(&fresh[nfresh], &stickers[i], guild_id, prev)) break;
      nfresh++;
    }
  }
  guild_stickers_free(g);
  g->stickers = fresh;
  g->nstickers = nfresh;
  g->capstickers = fresh ? n : 0;
  d->revision++;
  ccord_store_expr_mutated_(s);
  return CCORD_OK;
}

ccord_status ccord_store_set_guild_stickers_admin(ccord_store *s, ccord_id guild_id, const ccord_sticker *stickers,
                                                  size_t n, ccord_error *err)
{
  ccord_status st = ccord_store_set_guild_stickers(s, guild_id, stickers, n, err);
  if (st == CCORD_OK) {
    expr_guild *g = guild_find(data_get(s, false), guild_id);
    if (g) g->stickers_admin_loaded = true;
  }
  return st;
}

bool ccord_store_guild_stickers_admin_loaded(const ccord_store *s, ccord_id guild_id)
{
  const expr_guild *g = guild_find(data_peek(s), guild_id);
  return g && g->stickers_admin_loaded;
}

ccord_status ccord_store_upsert_guild_sticker(ccord_store *s, ccord_id guild_id, const ccord_sticker *sticker,
                                              ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE || !sticker || sticker->id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: sticker upsert");
  }
  ccord_expr_data *d = data_get(s, true);
  if (!d) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  expr_guild *g = guild_get(d, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  for (size_t i = 0; i < g->nstickers; i++) {
    if (g->stickers[i].id != sticker->id) continue;
    ccord_sticker copy;
    if (!copy_sticker_item(&copy, sticker, guild_id, &g->stickers[i])) {
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    sticker_item_free(&g->stickers[i]);
    g->stickers[i] = copy;
    d->revision++;
    ccord_store_expr_mutated_(s);
    return CCORD_OK;
  }
  if (g->nstickers >= CCORD_MAX_STICKERS_PER_GUILD) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: sticker limit");
  }
  if (g->nstickers == g->capstickers) {
    size_t ncap = g->capstickers ? g->capstickers * 2u : 8u;
    if (ncap > CCORD_MAX_STICKERS_PER_GUILD) ncap = CCORD_MAX_STICKERS_PER_GUILD;
    ccord_sticker *grown = ccord_realloc_array(g->stickers, ncap, sizeof *grown);
    if (!grown) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    g->stickers = grown;
    g->capstickers = ncap;
  }
  if (!copy_sticker_item(&g->stickers[g->nstickers], sticker, guild_id, nullptr)) {
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  g->nstickers++;
  d->revision++;
  ccord_store_expr_mutated_(s);
  return CCORD_OK;
}

bool ccord_store_remove_guild_sticker(ccord_store *s, ccord_id guild_id, ccord_id sticker_id)
{
  if (!s || guild_id == CCORD_ID_NONE || sticker_id == CCORD_ID_NONE) return false;
  ccord_expr_data *d = data_get(s, false);
  expr_guild *g = guild_find(d, guild_id);
  if (!g) return false;
  for (size_t i = 0; i < g->nstickers; i++) {
    if (g->stickers[i].id != sticker_id) continue;
    sticker_item_free(&g->stickers[i]);
    memmove(&g->stickers[i], &g->stickers[i + 1], (g->nstickers - i - 1u) * sizeof *g->stickers);
    g->nstickers--;
    d->revision++;
    ccord_store_expr_mutated_(s);
    return true;
  }
  return false;
}

ccord_status ccord_store_set_guild_soundboard(ccord_store *s, ccord_id guild_id, const ccord_soundboard_sound *sounds,
                                              size_t n, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: soundboard guild");
  if (n > 0 && !sounds) return ccord_error_set(err, CCORD_ERR_INVALID, "store: soundboard list");
  if (n > CCORD_MAX_SOUNDBOARD_PER_GUILD) n = CCORD_MAX_SOUNDBOARD_PER_GUILD;
  ccord_expr_data *d = data_get(s, true);
  if (!d) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  expr_guild *g = guild_get(d, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  ccord_soundboard_sound *fresh = nullptr;
  size_t nfresh = 0;
  if (n > 0) {
    fresh = ccord_calloc(n, sizeof *fresh);
    if (!fresh) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    for (size_t i = 0; i < n; i++) {
      if (sounds[i].id == CCORD_ID_NONE) continue;
      const ccord_soundboard_sound *prev = find_prev_sound(g->sounds, g->nsounds, sounds[i].id);
      if (!copy_sound_item(&fresh[nfresh], &sounds[i], guild_id, prev)) break;
      nfresh++;
    }
  }
  guild_sounds_free(g);
  g->sounds = fresh;
  g->nsounds = nfresh;
  g->capsounds = fresh ? n : 0;
  d->revision++;
  ccord_store_expr_mutated_(s);
  return CCORD_OK;
}

ccord_status ccord_store_set_guild_soundboard_admin(ccord_store *s, ccord_id guild_id,
                                                    const ccord_soundboard_sound *sounds, size_t n, ccord_error *err)
{
  ccord_status st = ccord_store_set_guild_soundboard(s, guild_id, sounds, n, err);
  if (st == CCORD_OK) {
    expr_guild *g = guild_find(data_get(s, false), guild_id);
    if (g) g->soundboard_admin_loaded = true;
  }
  return st;
}

bool ccord_store_guild_soundboard_admin_loaded(const ccord_store *s, ccord_id guild_id)
{
  const expr_guild *g = guild_find(data_peek(s), guild_id);
  return g && g->soundboard_admin_loaded;
}

ccord_status ccord_store_upsert_guild_soundboard(ccord_store *s, ccord_id guild_id,
                                                 const ccord_soundboard_sound *sound, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE || !sound || sound->id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: soundboard upsert");
  }
  ccord_expr_data *d = data_get(s, true);
  if (!d) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  expr_guild *g = guild_get(d, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  for (size_t i = 0; i < g->nsounds; i++) {
    if (g->sounds[i].id != sound->id) continue;
    ccord_soundboard_sound copy;
    if (!copy_sound_item(&copy, sound, guild_id, &g->sounds[i])) {
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    sound_item_free(&g->sounds[i]);
    g->sounds[i] = copy;
    d->revision++;
    ccord_store_expr_mutated_(s);
    return CCORD_OK;
  }
  if (g->nsounds >= CCORD_MAX_SOUNDBOARD_PER_GUILD) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: soundboard limit");
  }
  if (g->nsounds == g->capsounds) {
    size_t ncap = g->capsounds ? g->capsounds * 2u : 8u;
    if (ncap > CCORD_MAX_SOUNDBOARD_PER_GUILD) ncap = CCORD_MAX_SOUNDBOARD_PER_GUILD;
    ccord_soundboard_sound *grown = ccord_realloc_array(g->sounds, ncap, sizeof *grown);
    if (!grown) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    g->sounds = grown;
    g->capsounds = ncap;
  }
  if (!copy_sound_item(&g->sounds[g->nsounds], sound, guild_id, nullptr)) {
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  g->nsounds++;
  d->revision++;
  ccord_store_expr_mutated_(s);
  return CCORD_OK;
}

bool ccord_store_remove_guild_soundboard(ccord_store *s, ccord_id guild_id, ccord_id sound_id)
{
  if (!s || guild_id == CCORD_ID_NONE || sound_id == CCORD_ID_NONE) return false;
  ccord_expr_data *d = data_get(s, false);
  expr_guild *g = guild_find(d, guild_id);
  if (!g) return false;
  for (size_t i = 0; i < g->nsounds; i++) {
    if (g->sounds[i].id != sound_id) continue;
    sound_item_free(&g->sounds[i]);
    memmove(&g->sounds[i], &g->sounds[i + 1], (g->nsounds - i - 1u) * sizeof *g->sounds);
    g->nsounds--;
    d->revision++;
    ccord_store_expr_mutated_(s);
    return true;
  }
  return false;
}

size_t ccord_store_guild_soundboard_count(const ccord_store *s, ccord_id guild_id)
{
  const expr_guild *g = guild_find(data_peek(s), guild_id);
  return g ? g->nsounds : 0;
}

const ccord_soundboard_sound *ccord_store_guild_soundboard_at(const ccord_store *s, ccord_id guild_id, size_t i)
{
  const expr_guild *g = guild_find(data_peek(s), guild_id);
  return (g && i < g->nsounds) ? &g->sounds[i] : nullptr;
}

const ccord_soundboard_sound *ccord_store_find_soundboard(const ccord_store *s, ccord_id sound_id)
{
  const ccord_expr_data *d = data_peek(s);
  if (!d || sound_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < d->ng; i++) {
    for (size_t k = 0; k < d->g[i].nsounds; k++) {
      if (d->g[i].sounds[k].id == sound_id) return &d->g[i].sounds[k];
    }
  }
  return nullptr;
}

void ccord_store_forget_guild_expression_admin(ccord_store *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE) return;
  expr_guild *g = guild_find(data_get(s, false), guild_id);
  if (!g) return;
  g->emojis_admin_loaded = false;
  g->stickers_admin_loaded = false;
  g->soundboard_admin_loaded = false;
}

void ccord_store_clear_guild_expressions(ccord_store *s)
{
  ccord_expr_data *d = data_get(s, false);
  if (!d) return;
  for (size_t i = 0; i < d->ng; i++) {
    guild_emojis_free(&d->g[i]);
    guild_stickers_free(&d->g[i]);
    guild_sounds_free(&d->g[i]);
  }
  d->ng = 0;
  ccord_idmap_clear(&d->guild_by_id);
  d->guild_ix_ok = true;
  ccord_idmap_clear(&d->emoji_by_id);
  d->emoji_ix_ok = true;
  d->revision++;
  ccord_store_expr_mutated_(s);
}

void ccord_store_clear_all_expressions(ccord_store *s)
{
  ccord_expr_data *d = data_get(s, false);
  if (!d) return;
  for (size_t i = 0; i < d->ng; i++) {
    guild_emojis_free(&d->g[i]);
    guild_stickers_free(&d->g[i]);
    guild_sounds_free(&d->g[i]);
  }
  ccord_free(d->g);
  d->g = nullptr;
  d->ng = 0;
  d->capg = 0;
  ccord_idmap_free(&d->guild_by_id);
  d->guild_ix_ok = true;
  ccord_idmap_free(&d->emoji_by_id);
  d->emoji_ix_ok = true;
  sticker_packs_free(d);
  for (size_t i = 0; i < d->nblobs; i++) blob_free(&d->blobs[i]);
  d->nblobs = 0;
  d->blob_bytes = 0;
  memset(d->emoji_frec, 0, sizeof(d->emoji_frec));
  d->nemoji_frec = 0;
  memset(d->fav_emojis, 0, sizeof(d->fav_emojis));
  d->nfav_emojis = 0;
  memset(d->sticker_frec, 0, sizeof(d->sticker_frec));
  d->nsticker_frec = 0;
  memset(d->fav_stickers, 0, sizeof(d->fav_stickers));
  d->nfav_stickers = 0;
  ccord_free(d->gif_cats);
  d->gif_cats = nullptr;
  d->ngif_cats = 0;
  ccord_free(d->gif_trending);
  d->gif_trending = nullptr;
  d->ngif_trending = 0;
  ccord_free(d->gif_search);
  d->gif_search = nullptr;
  d->ngif_search = 0;
  ccord_free(d->fav_gifs);
  d->fav_gifs = nullptr;
  d->nfav_gifs = 0;
  ccord_free(d->settings_proto);
  d->settings_proto = nullptr;
  d->settings_proto_len = 0;
  d->revision++;
  ccord_store_expr_mutated_(s);
}

size_t ccord_store_guild_emoji_count(const ccord_store *s, ccord_id guild_id)
{
  const expr_guild *g = guild_find(data_peek(s), guild_id);
  return g ? g->nemojis : 0;
}

const ccord_emoji *ccord_store_guild_emoji_at(const ccord_store *s, ccord_id guild_id, size_t i)
{
  const expr_guild *g = guild_find(data_peek(s), guild_id);
  return (g && i < g->nemojis) ? &g->emojis[i] : nullptr;
}

size_t ccord_store_guild_sticker_count(const ccord_store *s, ccord_id guild_id)
{
  const expr_guild *g = guild_find(data_peek(s), guild_id);
  return g ? g->nstickers : 0;
}

const ccord_sticker *ccord_store_guild_sticker_at(const ccord_store *s, ccord_id guild_id, size_t i)
{
  const expr_guild *g = guild_find(data_peek(s), guild_id);
  return (g && i < g->nstickers) ? &g->stickers[i] : nullptr;
}

const ccord_emoji *ccord_store_find_emoji(const ccord_store *s, ccord_id emoji_id)
{
  const ccord_expr_data *d = data_peek(s);
  if (!d || emoji_id == CCORD_ID_NONE) return nullptr;
  if (d->emoji_ix_ok) {
    size_t packed = 0;
    if (!ccord_idmap_find(&d->emoji_by_id, emoji_id, &packed)) return nullptr;
    size_t gi = packed >> EXPR_EMOJI_IDX_BITS;
    size_t ei = packed & EXPR_EMOJI_IDX_MASK;
    if (gi < d->ng && ei < d->g[gi].nemojis && d->g[gi].emojis[ei].id == emoji_id) {
      return &d->g[gi].emojis[ei];
    }
  }
  for (size_t i = 0; i < d->ng; i++) {
    for (size_t k = 0; k < d->g[i].nemojis; k++) {
      if (d->g[i].emojis[k].id == emoji_id) return &d->g[i].emojis[k];
    }
  }
  return nullptr;
}

const ccord_emoji *ccord_store_find_emoji_by_name(const ccord_store *s, const char *name)
{
  const ccord_expr_data *d = data_peek(s);
  if (!d || !name || !name[0]) return nullptr;
  for (size_t i = 0; i < d->ng; i++) {
    for (size_t k = 0; k < d->g[i].nemojis; k++) {
      if (d->g[i].emojis[k].name && strcmp(d->g[i].emojis[k].name, name) == 0) {
        return &d->g[i].emojis[k];
      }
    }
  }
  return nullptr;
}

const ccord_sticker *ccord_store_find_sticker(const ccord_store *s, ccord_id sticker_id)
{
  const ccord_expr_data *d = data_peek(s);
  if (!d || sticker_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < d->ng; i++) {
    for (size_t k = 0; k < d->g[i].nstickers; k++) {
      if (d->g[i].stickers[k].id == sticker_id) return &d->g[i].stickers[k];
    }
  }
  for (size_t p = 0; p < d->nsticker_packs; p++) {
    for (size_t k = 0; k < d->sticker_packs[p].sticker_count; k++) {
      if (d->sticker_packs[p].stickers[k].id == sticker_id) return &d->sticker_packs[p].stickers[k];
    }
  }
  return nullptr;
}

const ccord_sticker *ccord_store_find_sticker_by_name(const ccord_store *s, const char *name)
{
  const ccord_expr_data *d = data_peek(s);
  if (!d || !name || !name[0]) return nullptr;
  for (size_t i = 0; i < d->ng; i++) {
    for (size_t k = 0; k < d->g[i].nstickers; k++) {
      if (d->g[i].stickers[k].name && strcmp(d->g[i].stickers[k].name, name) == 0) {
        return &d->g[i].stickers[k];
      }
    }
  }
  for (size_t p = 0; p < d->nsticker_packs; p++) {
    for (size_t k = 0; k < d->sticker_packs[p].sticker_count; k++) {
      if (d->sticker_packs[p].stickers[k].name && strcmp(d->sticker_packs[p].stickers[k].name, name) == 0) {
        return &d->sticker_packs[p].stickers[k];
      }
    }
  }
  return nullptr;
}

size_t ccord_store_expression_guild_count(const ccord_store *s)
{
  const ccord_expr_data *d = data_peek(s);
  return d ? d->ng : 0;
}

ccord_id ccord_store_expression_guild_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = data_peek(s);
  return (d && i < d->ng) ? d->g[i].guild_id : CCORD_ID_NONE;
}

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
  ccord_expr_data *d = data_get(s, true);
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
  ccord_expr_data *d = data_get(s, true);
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
  const ccord_expr_data *d = data_peek(s);
  return d ? d->nemoji_frec : 0;
}

const char *ccord_store_frequent_emoji_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = data_peek(s);
  return (d && i < d->nemoji_frec) ? d->emoji_frec[i].key : nullptr;
}

size_t ccord_store_favorite_emoji_count(const ccord_store *s)
{
  const ccord_expr_data *d = data_peek(s);
  return d ? d->nfav_emojis : 0;
}

const char *ccord_store_favorite_emoji_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = data_peek(s);
  return (d && i < d->nfav_emojis) ? d->fav_emojis[i] : nullptr;
}

bool ccord_store_is_favorite_emoji(const ccord_store *s, const char *key)
{
  const ccord_expr_data *d = data_peek(s);
  if (!d || !key || !key[0]) return false;
  for (size_t i = 0; i < d->nfav_emojis; i++) {
    if (strcmp(d->fav_emojis[i], key) == 0) return true;
  }
  return false;
}

bool ccord_store_toggle_favorite_emoji(ccord_store *s, const char *key)
{
  if (!s || !key || !key[0]) return false;
  ccord_expr_data *d = data_get(s, true);
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
  ccord_expr_data *d = data_get(s, true);
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
  ccord_expr_data *d = data_get(s, true);
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
  const ccord_expr_data *d = data_peek(s);
  return d ? d->nsticker_frec : 0;
}

ccord_id ccord_store_frequent_sticker_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = data_peek(s);
  return (d && i < d->nsticker_frec) ? d->sticker_frec[i].id : CCORD_ID_NONE;
}

size_t ccord_store_favorite_sticker_count(const ccord_store *s)
{
  const ccord_expr_data *d = data_peek(s);
  return d ? d->nfav_stickers : 0;
}

ccord_id ccord_store_favorite_sticker_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = data_peek(s);
  return (d && i < d->nfav_stickers) ? d->fav_stickers[i] : CCORD_ID_NONE;
}

bool ccord_store_is_favorite_sticker(const ccord_store *s, ccord_id sticker_id)
{
  const ccord_expr_data *d = data_peek(s);
  if (!d || sticker_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < d->nfav_stickers; i++) {
    if (d->fav_stickers[i] == sticker_id) return true;
  }
  return false;
}

bool ccord_store_toggle_favorite_sticker(ccord_store *s, ccord_id sticker_id)
{
  if (!s || sticker_id == CCORD_ID_NONE) return false;
  ccord_expr_data *d = data_get(s, true);
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
  const ccord_expr_data *d = data_peek(s);
  return d ? d->nsticker_packs : 0;
}

const ccord_sticker_pack *ccord_store_sticker_pack_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = data_peek(s);
  return (d && i < d->nsticker_packs) ? &d->sticker_packs[i] : nullptr;
}

// ---- GIF categories, trending, search & favorites ----

size_t ccord_store_gif_category_count(const ccord_store *s)
{
  const ccord_expr_data *d = data_peek(s);
  return d ? d->ngif_cats : 0;
}

const ccord_gif_category *ccord_store_gif_category_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = data_peek(s);
  return (d && d->gif_cats && i < d->ngif_cats) ? &d->gif_cats[i] : nullptr;
}

size_t ccord_store_gif_trending_count(const ccord_store *s)
{
  const ccord_expr_data *d = data_peek(s);
  return d ? d->ngif_trending : 0;
}

const ccord_gif_item *ccord_store_gif_trending_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = data_peek(s);
  return (d && d->gif_trending && i < d->ngif_trending) ? &d->gif_trending[i] : nullptr;
}

size_t ccord_store_gif_search_count(const ccord_store *s)
{
  const ccord_expr_data *d = data_peek(s);
  return d ? d->ngif_search : 0;
}

const ccord_gif_item *ccord_store_gif_search_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = data_peek(s);
  return (d && d->gif_search && i < d->ngif_search) ? &d->gif_search[i] : nullptr;
}

size_t ccord_store_favorite_gif_count(const ccord_store *s)
{
  const ccord_expr_data *d = data_peek(s);
  return d ? d->nfav_gifs : 0;
}

const ccord_gif_item *ccord_store_favorite_gif_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = data_peek(s);
  return (d && d->fav_gifs && i < d->nfav_gifs) ? &d->fav_gifs[i] : nullptr;
}

bool ccord_store_is_favorite_gif(const ccord_store *s, const char *url)
{
  const ccord_expr_data *d = data_peek(s);
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
  ccord_expr_data *d = data_get(s, true);
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
  if (!ensure_gif_buf(&d->fav_gifs, EXPR_MAX_FAV_GIFS)) return false;
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
  ccord_expr_data *d = data_get(s, false);
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
        if (n > 0 && ensure_gif_cats(d)) {
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
        ensure_gif_buf(&d->gif_trending, EXPR_MAX_GIFS)) {
      parse_gif_array(gifs, d->gif_trending, EXPR_MAX_GIFS, &d->ngif_trending);
    }
  } else if (kind == 2u) { // CCORD_EXPR_GIF_TRENDING
    const ccord_json *arr = gif_list_from_json(root);
    // An empty answer must not wipe the fallback rows already ingested from /gifs/trending.
    if (arr && ccord_json_array_len(arr) > 0 && ensure_gif_buf(&d->gif_trending, EXPR_MAX_GIFS)) {
      parse_gif_array(arr, d->gif_trending, EXPR_MAX_GIFS, &d->ngif_trending);
    }
  } else if (kind == 3u) { // CCORD_EXPR_GIF_SEARCH
    const ccord_json *arr = gif_list_from_json(root);
    if (arr && ccord_json_array_len(arr) > 0 && ensure_gif_buf(&d->gif_search, EXPR_MAX_GIFS)) {
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
      sticker_packs_free(d);
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
          dst->name = dup_or_empty(sname, CCORD_NAME_MAX_BYTES);
          dst->description = dup_or_empty(sdesc, 256);
          dst->tags = dup_or_empty(stags, 200);
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

static const expr_blob *blob_find(const ccord_expr_data *d, unsigned kind, const char *key)
{
  if (!d) return nullptr;
  if (!key) key = "";
  for (size_t i = 0; i < d->nblobs; i++) {
    const expr_blob *b = &d->blobs[i];
    if (b->kind == kind && strcmp(b->key, key) == 0) return b;
  }
  return nullptr;
}

static expr_blob *blob_get(ccord_expr_data *d, unsigned kind, const char *key)
{
  if (!key) key = "";
  expr_blob *b = nullptr;
  for (size_t i = 0; i < d->nblobs; i++) {
    if (d->blobs[i].kind == kind && strcmp(d->blobs[i].key, key) == 0) {
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
    blob_free(&d->blobs[evict]);
    d->blobs[evict] = d->blobs[d->nblobs - 1];
    memset(&d->blobs[d->nblobs - 1], 0, sizeof d->blobs[0]);
    d->nblobs--;
  }
  const char *k = dup_or_empty(key, EXPR_KEY_MAX);
  if (!k) return nullptr;
  b = &d->blobs[d->nblobs++];
  *b = (expr_blob){.kind = kind, .key = k};
  return b;
}

ccord_expr_blob_state ccord_store_expr_blob_state(const ccord_store *s, unsigned kind, const char *key)
{
  const expr_blob *b = blob_find(data_peek(s), kind, key);
  return b ? b->state : CCORD_EXPR_BLOB_NONE;
}

void ccord_store_expr_blob_mark_pending(ccord_store *s, unsigned kind, const char *key)
{
  ccord_expr_data *d = data_get(s, true);
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
  if (len > EXPR_BLOB_BYTES_MAX) ok = false;
  ccord_expr_data *d = data_get(s, true);
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
      blob_free(&d->blobs[evict]);
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
  const expr_blob *b = blob_find(data_peek(s), kind, key);
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
  ccord_expr_data *d = data_get(s, true);
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
  const ccord_expr_data *d = data_peek(s);
  if (!d || !d->settings_proto) {
    if (out_len) *out_len = 0;
    return nullptr;
  }
  if (out_len) *out_len = d->settings_proto_len;
  return d->settings_proto;
}

uint64_t ccord_store_expr_revision(const ccord_store *s)
{
  const ccord_expr_data *d = data_peek(s);
  return d ? d->revision : 0;
}
