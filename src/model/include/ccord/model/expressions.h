// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/model/types.h>

// Guild expressions (custom emoji, stickers, soundboard sounds). Strings are borrowed; the store deep-copies them.
#define CCORD_MAX_EMOJIS_PER_GUILD 1000u
#define CCORD_MAX_STICKERS_PER_GUILD 200u
#define CCORD_MAX_SOUNDBOARD_PER_GUILD 128u
typedef struct ccord_emoji {
  ccord_id id, guild_id;                       // guild_id 0 = unknown/global
  const char *name;                            // without colons, e.g. "blobwave"
  ccord_id user_id;                            // uploader user id (0 when omitted by gateway)
  const char *user_name;                       // uploader display/username ("" / nullptr when unknown)
  bool animated, available;
} ccord_emoji;
typedef enum ccord_sticker_format : uint8_t {
  CCORD_STICKER_PNG = 1, CCORD_STICKER_APNG = 2, CCORD_STICKER_LOTTIE = 3, CCORD_STICKER_GIF = 4
} ccord_sticker_format;
typedef struct ccord_sticker {
  ccord_id id, guild_id, pack_id;              // guild_id 0 = standard sticker (pack_id set)
  const char *name, *description, *tags;
  ccord_id user_id;                            // uploader user id (0 when omitted by gateway)
  const char *user_name;                       // uploader display/username ("" / nullptr when unknown)
  ccord_sticker_format format;
  bool available;
} ccord_sticker;
typedef struct ccord_soundboard_sound {
  ccord_id id, guild_id;                       // guild_id 0 = Discord default sound
  const char *name;
  ccord_id emoji_id;                           // custom emoji id (0 when none / unicode)
  const char *emoji_name;                      // unicode emoji or custom emoji name ("" / nullptr when none)
  ccord_id user_id;                            // uploader user id (0 when unknown)
  const char *user_name;                       // uploader display/username ("" / nullptr when unknown)
  uint8_t volume_pct;                          // 0..100 (Discord 0.0..1.0 * 100)
  bool available;
} ccord_soundboard_sound;
// Per-boost-tier guild expression slot limits (tier 0..3+; emoji limit applies separately to static and animated).
[[nodiscard]] uint32_t ccord_guild_emoji_slot_limit(uint8_t premium_tier);
[[nodiscard]] uint32_t ccord_guild_sticker_slot_limit(uint8_t premium_tier);
[[nodiscard]] uint32_t ccord_guild_soundboard_slot_limit(uint8_t premium_tier);
