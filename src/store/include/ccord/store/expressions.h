// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/model/model.h>
// Expression data held by the store: per-guild custom emojis + stickers (GUILD_* payloads) and a small cache
// of raw REST responses for the expression picker (GIF lists, sticker packs, FrecencyUserSettings proto).
// Mutators deep-copy; readers are borrowed and valid until the next mutator call.
typedef struct ccord_store ccord_store;

[[nodiscard]] ccord_status ccord_store_set_guild_emojis(ccord_store *s, ccord_id guild_id, const ccord_emoji *emojis,
                                                        size_t n, ccord_error *err);
[[nodiscard]] ccord_status ccord_store_set_guild_emojis_admin(ccord_store *s, ccord_id guild_id,
                                                              const ccord_emoji *emojis, size_t n, ccord_error *err);
bool ccord_store_guild_emojis_admin_loaded(const ccord_store *s, ccord_id guild_id);
[[nodiscard]] ccord_status ccord_store_upsert_guild_emoji(ccord_store *s, ccord_id guild_id, const ccord_emoji *emoji,
                                                          ccord_error *err);
bool ccord_store_remove_guild_emoji(ccord_store *s, ccord_id guild_id, ccord_id emoji_id);

[[nodiscard]] ccord_status ccord_store_set_guild_stickers(ccord_store *s, ccord_id guild_id,
                                                          const ccord_sticker *stickers, size_t n, ccord_error *err);
[[nodiscard]] ccord_status ccord_store_set_guild_stickers_admin(ccord_store *s, ccord_id guild_id,
                                                                const ccord_sticker *stickers, size_t n,
                                                                ccord_error *err);
bool ccord_store_guild_stickers_admin_loaded(const ccord_store *s, ccord_id guild_id);
[[nodiscard]] ccord_status ccord_store_upsert_guild_sticker(ccord_store *s, ccord_id guild_id,
                                                            const ccord_sticker *sticker, ccord_error *err);
bool ccord_store_remove_guild_sticker(ccord_store *s, ccord_id guild_id, ccord_id sticker_id);

[[nodiscard]] ccord_status ccord_store_set_guild_soundboard(ccord_store *s, ccord_id guild_id,
                                                            const ccord_soundboard_sound *sounds, size_t n,
                                                            ccord_error *err);
[[nodiscard]] ccord_status ccord_store_set_guild_soundboard_admin(ccord_store *s, ccord_id guild_id,
                                                                  const ccord_soundboard_sound *sounds, size_t n,
                                                                  ccord_error *err);
bool ccord_store_guild_soundboard_admin_loaded(const ccord_store *s, ccord_id guild_id);
[[nodiscard]] ccord_status ccord_store_upsert_guild_soundboard(ccord_store *s, ccord_id guild_id,
                                                               const ccord_soundboard_sound *sound, ccord_error *err);
bool ccord_store_remove_guild_soundboard(ccord_store *s, ccord_id guild_id, ccord_id sound_id);
size_t ccord_store_guild_soundboard_count(const ccord_store *s, ccord_id guild_id);
const ccord_soundboard_sound *ccord_store_guild_soundboard_at(const ccord_store *s, ccord_id guild_id, size_t i);
const ccord_soundboard_sound *ccord_store_find_soundboard(const ccord_store *s, ccord_id sound_id);

void ccord_store_forget_guild_expression_admin(ccord_store *s, ccord_id guild_id);
void ccord_store_clear_guild_expressions(ccord_store *s);   // READY replaces everything
void ccord_store_clear_all_expressions(ccord_store *s);
size_t ccord_store_guild_emoji_count(const ccord_store *s, ccord_id guild_id);
const ccord_emoji *ccord_store_guild_emoji_at(const ccord_store *s, ccord_id guild_id, size_t i);
size_t ccord_store_guild_sticker_count(const ccord_store *s, ccord_id guild_id);
const ccord_sticker *ccord_store_guild_sticker_at(const ccord_store *s, ccord_id guild_id, size_t i);
const ccord_emoji *ccord_store_find_emoji(const ccord_store *s, ccord_id emoji_id);       // any guild
const ccord_emoji *ccord_store_find_emoji_by_name(const ccord_store *s, const char *name);
const ccord_sticker *ccord_store_find_sticker(const ccord_store *s, ccord_id sticker_id); // any guild or pack
const ccord_sticker *ccord_store_find_sticker_by_name(const ccord_store *s, const char *name);
// Guild ids that currently have emojis or stickers, in first-seen order.
size_t ccord_store_expression_guild_count(const ccord_store *s);
ccord_id ccord_store_expression_guild_at(const ccord_store *s, size_t i);

typedef struct ccord_gif_item {
  char url[CCORD_URL_MAX_BYTES + 1];
  char src[CCORD_URL_MAX_BYTES + 1];
  uint32_t width;
  uint32_t height;
  uint32_t format;   // Discord media format value, kept verbatim so favorites round-trip losslessly (0 = unknown)
  uint32_t order;    // position in the favorite list (FrecencyUserSettings favorite_gifs order; 0 = first)
} ccord_gif_item;

typedef struct ccord_gif_category {
  char name[CCORD_NAME_MAX_BYTES + 1];
  char src[CCORD_URL_MAX_BYTES + 1];
} ccord_gif_category;

typedef struct ccord_sticker_pack {
  ccord_id id;
  char name[CCORD_NAME_MAX_BYTES + 1];
  ccord_sticker *stickers;
  size_t sticker_count;
} ccord_sticker_pack;

// Emoji frecency & favorites
void ccord_store_record_emoji_use(ccord_store *s, const char *key);
void ccord_store_seed_emoji_frecency(ccord_store *s, const char *key, int32_t score);
size_t ccord_store_frequent_emoji_count(const ccord_store *s);
const char *ccord_store_frequent_emoji_at(const ccord_store *s, size_t i);
size_t ccord_store_favorite_emoji_count(const ccord_store *s);
const char *ccord_store_favorite_emoji_at(const ccord_store *s, size_t i);
bool ccord_store_is_favorite_emoji(const ccord_store *s, const char *key);
bool ccord_store_toggle_favorite_emoji(ccord_store *s, const char *key);

// Sticker frecency, favorites & standard sticker packs
void ccord_store_record_sticker_use(ccord_store *s, ccord_id sticker_id);
void ccord_store_seed_sticker_frecency(ccord_store *s, ccord_id sticker_id, int32_t score);
size_t ccord_store_frequent_sticker_count(const ccord_store *s);
ccord_id ccord_store_frequent_sticker_at(const ccord_store *s, size_t i);
size_t ccord_store_favorite_sticker_count(const ccord_store *s);
ccord_id ccord_store_favorite_sticker_at(const ccord_store *s, size_t i);
bool ccord_store_is_favorite_sticker(const ccord_store *s, ccord_id sticker_id);
bool ccord_store_toggle_favorite_sticker(ccord_store *s, ccord_id sticker_id);
size_t ccord_store_sticker_pack_count(const ccord_store *s);
const ccord_sticker_pack *ccord_store_sticker_pack_at(const ccord_store *s, size_t i);

// GIF categories, trending, search & favorites
size_t ccord_store_gif_category_count(const ccord_store *s);
const ccord_gif_category *ccord_store_gif_category_at(const ccord_store *s, size_t i);
size_t ccord_store_gif_trending_count(const ccord_store *s);
const ccord_gif_item *ccord_store_gif_trending_at(const ccord_store *s, size_t i);
size_t ccord_store_gif_search_count(const ccord_store *s);
const ccord_gif_item *ccord_store_gif_search_at(const ccord_store *s, size_t i);
size_t ccord_store_favorite_gif_count(const ccord_store *s);
// Favorites are newest first: index 0 has the highest order (the Favorites tab draws in this order).
const ccord_gif_item *ccord_store_favorite_gif_at(const ccord_store *s, size_t i);
bool ccord_store_is_favorite_gif(const ccord_store *s, const char *url);
bool ccord_store_toggle_favorite_gif(ccord_store *s, const ccord_gif_item *gif);
// Re-orders an existing favorite to `order` (a settings update). True when its position changed.
bool ccord_store_set_favorite_gif_order(ccord_store *s, const char *url, uint32_t order);

typedef enum ccord_expr_blob_state : uint8_t {
  CCORD_EXPR_BLOB_NONE, CCORD_EXPR_BLOB_PENDING, CCORD_EXPR_BLOB_READY, CCORD_EXPR_BLOB_FAILED
} ccord_expr_blob_state;
// `kind` is a ccord_expr_kind value; `key` is the request argument ("" / nullptr when none).
ccord_expr_blob_state ccord_store_expr_blob_state(const ccord_store *s, unsigned kind, const char *key);
void ccord_store_expr_blob_mark_pending(ccord_store *s, unsigned kind, const char *key);
[[nodiscard]] ccord_status ccord_store_set_expr_blob(ccord_store *s, unsigned kind, const char *key, const char *body,
                                                     size_t len, bool ok, ccord_error *err);
const char *ccord_store_expr_blob(const ccord_store *s, unsigned kind, const char *key, size_t *out_len);   // READY only
// Raw FrecencyUserSettings protobuf bytes (GET /users/@me/settings-proto/2 "settings", base64-decoded). Kept
// losslessly so favorite-GIF edits can be re-encoded without dropping unknown wire fields, then PATCHed back.
// `data` is deep-copied; len 0 / nullptr clears the cache. The getter returns nullptr until the first set.
[[nodiscard]] ccord_status ccord_store_set_raw_settings_proto(ccord_store *s, const uint8_t *data, size_t len,
                                                              ccord_error *err);
const uint8_t *ccord_store_get_raw_settings_proto(const ccord_store *s, size_t *out_len);
// Bumps whenever any expression data changes (guild lists or blobs); the picker re-parses on change.
uint64_t ccord_store_expr_revision(const ccord_store *s);
