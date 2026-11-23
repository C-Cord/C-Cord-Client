// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/idmap.h>
#include "store_internal.h"

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
  ccord_gif_category *gif_cats;   // dynamically grown up to EXPR_MAX_GIF_CATS on write
  size_t ngif_cats, cap_gif_cats;
  ccord_gif_item *gif_trending;   // dynamically grown up to EXPR_MAX_GIFS on write
  size_t ngif_trending, cap_gif_trending;
  ccord_gif_item *gif_search;     // dynamically grown up to EXPR_MAX_GIFS on write
  size_t ngif_search, cap_gif_search;
  ccord_gif_item *fav_gifs;       // dynamically grown up to EXPR_MAX_FAV_GIFS on write
  size_t nfav_gifs, cap_fav_gifs;
  uint8_t *settings_proto;   // raw FrecencyUserSettings bytes, kept losslessly for the favorite-GIF PATCH
  size_t settings_proto_len;
  uint64_t revision, tick;
};


void expr_blob_free(expr_blob *b);
void expr_sticker_packs_free(ccord_expr_data *d);
ccord_expr_data *expr_data_get(ccord_store *s, bool create);
const ccord_expr_data *expr_data_peek(const ccord_store *s);
ccord_gif_category *expr_ensure_gif_cats(ccord_expr_data *d, size_t need);
ccord_gif_item *expr_ensure_gif_buf(ccord_gif_item **slot, size_t *cap_slot, size_t need, size_t max_cap);
const char *expr_dup_or_empty(const char *s, size_t max);
