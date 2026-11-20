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

void expr_blob_free(expr_blob *b)
{
  free_cstr(&b->key);
  ccord_free(b->body);
  memset(b, 0, sizeof *b);
}

void expr_sticker_packs_free(ccord_expr_data *d)
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
  expr_sticker_packs_free(d);
  for (size_t i = 0; i < d->nblobs; i++) expr_blob_free(&d->blobs[i]);
  ccord_free(d->gif_cats);
  ccord_free(d->gif_trending);
  ccord_free(d->gif_search);
  ccord_free(d->fav_gifs);
  ccord_free(d->settings_proto);
  ccord_free(d);
}

ccord_expr_data *expr_data_get(ccord_store *s, bool create)
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

const ccord_expr_data *expr_data_peek(const ccord_store *s)
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

ccord_gif_category *expr_ensure_gif_cats(ccord_expr_data *d)
{
  if (!d->gif_cats) d->gif_cats = ccord_calloc(EXPR_MAX_GIF_CATS, sizeof *d->gif_cats);
  return d->gif_cats;
}

ccord_gif_item *expr_ensure_gif_buf(ccord_gif_item **slot, size_t cap)
{
  if (!*slot) *slot = ccord_calloc(cap, sizeof **slot);
  return *slot;
}

const char *expr_dup_or_empty(const char *s, size_t max)
{
  if (!s || s[0] == '\0' || max == 0) return k_empty_str;
  size_t n = 0;
  while (n < max && s[n] != '\0') n++;
  while (n > 0 && !ccord_utf8_valid(s, n)) n--;   // never cut a UTF-8 sequence
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
  dst->name = expr_dup_or_empty(src->name, CCORD_NAME_MAX_BYTES);
  dst->user_name = expr_dup_or_empty(uname, CCORD_NAME_MAX_BYTES);
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
  dst->name = expr_dup_or_empty(src->name, CCORD_NAME_MAX_BYTES);
  dst->description = expr_dup_or_empty(src->description, 256);
  dst->tags = expr_dup_or_empty(src->tags, 200);
  dst->user_name = expr_dup_or_empty(uname, CCORD_NAME_MAX_BYTES);
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
  dst->name = expr_dup_or_empty(src->name, CCORD_NAME_MAX_BYTES);
  dst->emoji_name = expr_dup_or_empty(src->emoji_name, CCORD_EMOJI_MAX_BYTES);
  dst->user_name = expr_dup_or_empty(uname, CCORD_NAME_MAX_BYTES);
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
  ccord_expr_data *d = expr_data_get(s, true);
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
    expr_guild *g = guild_find(expr_data_get(s, false), guild_id);
    if (g) g->emojis_admin_loaded = true;
  }
  return st;
}

bool ccord_store_guild_emojis_admin_loaded(const ccord_store *s, ccord_id guild_id)
{
  const expr_guild *g = guild_find(expr_data_peek(s), guild_id);
  return g && g->emojis_admin_loaded;
}

ccord_status ccord_store_upsert_guild_emoji(ccord_store *s, ccord_id guild_id, const ccord_emoji *emoji,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE || !emoji || emoji->id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: emoji upsert");
  }
  ccord_expr_data *d = expr_data_get(s, true);
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
  ccord_expr_data *d = expr_data_get(s, false);
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
  ccord_expr_data *d = expr_data_get(s, true);
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
    expr_guild *g = guild_find(expr_data_get(s, false), guild_id);
    if (g) g->stickers_admin_loaded = true;
  }
  return st;
}

bool ccord_store_guild_stickers_admin_loaded(const ccord_store *s, ccord_id guild_id)
{
  const expr_guild *g = guild_find(expr_data_peek(s), guild_id);
  return g && g->stickers_admin_loaded;
}

ccord_status ccord_store_upsert_guild_sticker(ccord_store *s, ccord_id guild_id, const ccord_sticker *sticker,
                                              ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE || !sticker || sticker->id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: sticker upsert");
  }
  ccord_expr_data *d = expr_data_get(s, true);
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
  ccord_expr_data *d = expr_data_get(s, false);
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
  ccord_expr_data *d = expr_data_get(s, true);
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
    expr_guild *g = guild_find(expr_data_get(s, false), guild_id);
    if (g) g->soundboard_admin_loaded = true;
  }
  return st;
}

bool ccord_store_guild_soundboard_admin_loaded(const ccord_store *s, ccord_id guild_id)
{
  const expr_guild *g = guild_find(expr_data_peek(s), guild_id);
  return g && g->soundboard_admin_loaded;
}

ccord_status ccord_store_upsert_guild_soundboard(ccord_store *s, ccord_id guild_id,
                                                 const ccord_soundboard_sound *sound, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || guild_id == CCORD_ID_NONE || !sound || sound->id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: soundboard upsert");
  }
  ccord_expr_data *d = expr_data_get(s, true);
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
  ccord_expr_data *d = expr_data_get(s, false);
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
  const expr_guild *g = guild_find(expr_data_peek(s), guild_id);
  return g ? g->nsounds : 0;
}

const ccord_soundboard_sound *ccord_store_guild_soundboard_at(const ccord_store *s, ccord_id guild_id, size_t i)
{
  const expr_guild *g = guild_find(expr_data_peek(s), guild_id);
  return (g && i < g->nsounds) ? &g->sounds[i] : nullptr;
}

const ccord_soundboard_sound *ccord_store_find_soundboard(const ccord_store *s, ccord_id sound_id)
{
  const ccord_expr_data *d = expr_data_peek(s);
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
  expr_guild *g = guild_find(expr_data_get(s, false), guild_id);
  if (!g) return;
  g->emojis_admin_loaded = false;
  g->stickers_admin_loaded = false;
  g->soundboard_admin_loaded = false;
}

void ccord_store_clear_guild_expressions(ccord_store *s)
{
  ccord_expr_data *d = expr_data_get(s, false);
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
  ccord_expr_data *d = expr_data_get(s, false);
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
  expr_sticker_packs_free(d);
  for (size_t i = 0; i < d->nblobs; i++) expr_blob_free(&d->blobs[i]);
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
  const expr_guild *g = guild_find(expr_data_peek(s), guild_id);
  return g ? g->nemojis : 0;
}

const ccord_emoji *ccord_store_guild_emoji_at(const ccord_store *s, ccord_id guild_id, size_t i)
{
  const expr_guild *g = guild_find(expr_data_peek(s), guild_id);
  return (g && i < g->nemojis) ? &g->emojis[i] : nullptr;
}

size_t ccord_store_guild_sticker_count(const ccord_store *s, ccord_id guild_id)
{
  const expr_guild *g = guild_find(expr_data_peek(s), guild_id);
  return g ? g->nstickers : 0;
}

const ccord_sticker *ccord_store_guild_sticker_at(const ccord_store *s, ccord_id guild_id, size_t i)
{
  const expr_guild *g = guild_find(expr_data_peek(s), guild_id);
  return (g && i < g->nstickers) ? &g->stickers[i] : nullptr;
}

const ccord_emoji *ccord_store_find_emoji(const ccord_store *s, ccord_id emoji_id)
{
  const ccord_expr_data *d = expr_data_peek(s);
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
  const ccord_expr_data *d = expr_data_peek(s);
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
  const ccord_expr_data *d = expr_data_peek(s);
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
  const ccord_expr_data *d = expr_data_peek(s);
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
  const ccord_expr_data *d = expr_data_peek(s);
  return d ? d->ng : 0;
}

ccord_id ccord_store_expression_guild_at(const ccord_store *s, size_t i)
{
  const ccord_expr_data *d = expr_data_peek(s);
  return (d && i < d->ng) ? d->g[i].guild_id : CCORD_ID_NONE;
}

