// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Hand-written, bounds-checked reader/writer for Discord's FrecencyUserSettings protobuf: the `settings`
// field (base64) of `GET`/`PATCH /users/@me/settings-proto/2`. Pure C, no network, no SDL.
//
// Schema (verified 2026-11 against):
//   https://github.com/discord-userdoccers/discord-protos
//     raw: discord_protos/discord_users/v1/FrecencyUserSettings.proto  (proto3, package discord_protos.discord_users.v1)
//   https://docs.discord.food/resources/user-settings-proto   ("Favorite Emojis / Emoji Frecency Structure")
// Only the following fields are decoded; everything else is skipped:
//   FrecencyUserSettings { favorite_gifs=2 (msg), favorite_stickers=3 (msg), sticker_frecency=4 (msg),
//                          favorite_emojis=5 (msg), emoji_frecency=6 (msg) }  (versions=1, others 7..14 ignored)
//   FavoriteGIFs     { map<string, FavoriteGIF> gifs = 1; bool hide_tooltip = 2; }      map key = GIF page URL
//   FavoriteGIF      { GIFType format = 1 (varint enum: 0 none, 1 image, 2 video); string src = 2;
//                      uint32 width = 3; uint32 height = 4; uint32 order = 5; }
//   FavoriteStickers { repeated fixed64 sticker_ids = 1; }       proto3 default = packed (wire type 2); unpacked
//                                                                (wire type 1, one record each) is accepted too
//   StickerFrecency  { map<fixed64, FrecencyItem> stickers = 1; }  entry key = wire type 1 (8 bytes LE)
//   FavoriteEmojis   { repeated string emojis = 1 [packed = false]; }
//   EmojiFrecency    { map<string, FrecencyItem> emojis = 1; }
//   FrecencyItem     { uint32 total_uses = 1; repeated uint64 recent_uses = 2; int32 frecency = 3; int32 score = 4; }
//                    (frecency/score are *int32* varints, not floats; recent_uses is skipped)
// Emoji keys (favorite_emojis entries and emoji_frecency map keys) are plain strings: a custom emoji is its
// snowflake id as a decimal string ("123456789012345678"), a unicode emoji is the unicode character(s) itself
// (docs.discord.food also mentions "friendly names" such as skull_crossbones). ccord_proto_frecency.id is set
// to the numeric value when the key is an all-digit string that fits in uint64 (a custom emoji), else 0.
// Map entries are messages {1: key, 2: value} in any order; duplicate keys: last one wins.
// Not independently verifiable offline: the UI-level meaning of `order` (see below).
#include <ccord/core/arena.h>
#include <ccord/core/error.h>

#define CCORD_PROTO_MAX_INPUT   (1024u * 1024u)  // decoded bytes accepted
#define CCORD_PROTO_MAX_ITEMS   4096u            // per collection
#define CCORD_PROTO_MAX_STRING  4096u            // bytes per string

typedef struct ccord_proto_gif {
  const char *url;     // map key: the GIF's page URL
  const char *src;     // media URL (may be empty)
  uint32_t format;     // 0 none, 1 image, 2 video
  uint32_t width, height;
  uint32_t order;      // client-assigned position; the array is sorted by this ascending (stable: ties keep wire order, like lodash sortBy;
                       // the official client then reverses the whole list, so callers walk it from the end).
                       // Discord assigns new favorites a larger `order`; callers wanting newest-first iterate backwards.
} ccord_proto_gif;

typedef struct ccord_proto_frecency {
  const char *key;     // emoji string (unicode char / snowflake decimal / name) or decimal sticker id
  uint64_t id;         // sticker id, or numeric emoji id for custom emoji (all-digit key); else 0
  uint32_t total_uses;
  int32_t frecency;
  int32_t score;
} ccord_proto_frecency;

typedef struct ccord_user_expressions {
  ccord_proto_gif *gifs; size_t gif_count;                                 // sorted by order ascending
  bool has_favorite_gifs;                                                  // favorite_gifs (root field 2) was on the wire, even if empty.
                                                                           // false: the list says nothing (not "no favorites")
  uint64_t *favorite_stickers; size_t favorite_sticker_count;              // wire order (de-duplicated not applied)
  ccord_proto_frecency *sticker_frecency; size_t sticker_frecency_count;   // score desc, frecency desc, total_uses desc, key
  const char **favorite_emojis; size_t favorite_emoji_count;               // wire order (the user's favorite order)
  ccord_proto_frecency *emoji_frecency; size_t emoji_frecency_count;       // score desc, frecency desc, total_uses desc, key
  ccord_arena arena;                                                       // owns every pointer above
} ccord_user_expressions;

// Decodes raw protobuf bytes. *out is zeroed first (and holds nothing to free on failure).
// Errors: CCORD_ERR_INVALID (malformed/truncated/embedded NUL/invalid UTF-8/group wire types),
//         CCORD_ERR_RANGE (input > 1 MiB, > 4096 items in a collection, string > 4 KiB), CCORD_ERR_NOMEM.
// An empty buffer is valid and yields an empty result.
[[nodiscard]] ccord_status ccord_user_expressions_decode(const uint8_t *buf, size_t len, ccord_user_expressions *out,
                                                         ccord_error *err);
// Standard base64 (with or without '=' padding) or the url-safe alphabet; ASCII whitespace is ignored.
[[nodiscard]] ccord_status ccord_user_expressions_decode_b64(const char *b64, size_t len, ccord_user_expressions *out,
                                                             ccord_error *err);
// Body of GET /users/@me/settings-proto/2: {"settings":"<base64>"}. An empty "settings" string is valid (empty result).
[[nodiscard]] ccord_status ccord_user_expressions_from_settings_json(const char *json, size_t len,
                                                                     ccord_user_expressions *out, ccord_error *err);

// ---- Favorite GIF writer (PATCH /users/@me/settings-proto/2) -------------------------------------------------
// Serializes FrecencyUserSettings with favorite_gifs (root field 2) replaced by `gifs`. Every other top-level
// record is copied verbatim from the optional existing settings, so versions (field 1), emoji/sticker
// frecency, and unknown or future fields (varint, length-delimited, fixed32, fixed64) survive a toggle.
// Field 2 is always emitted, even when count == 0 (clearing the favorites), as one repeated field-1 map
// entry per GIF.
//
// `gifs` may be NULL only when count == 0. url/src must be non-NULL valid UTF-8 (src may be empty); url must
// not be empty because the decoder drops empty map keys. Strings are capped at CCORD_PROTO_MAX_STRING and
// count at CCORD_PROTO_MAX_ITEMS, like the reader.
//
// Existing input is optional: NULL (with length 0) starts from an empty settings message. When supplied,
// malformed existing input is CCORD_ERR_INVALID rather than being silently dropped.
//   ..._encode_gifs       existing is raw protobuf bytes, *out is raw bytes, *out_len its length (may be NULL).
//   ..._encode_gifs_b64   existing is a bare base64 blob, *out is padded standard base64.
//   ..._encode_gifs_json  existing is the GET body {"settings":"<base64>"}, *out is the same JSON shape.
// *out is NULL-terminated on success and left NULL on failure; free it with ccord_free.
// Errors: CCORD_ERR_INVALID (missing/invalid strings or existing input), CCORD_ERR_RANGE (limits),
//         CCORD_ERR_NOMEM.
[[nodiscard]] ccord_status ccord_user_expressions_encode_gifs(const ccord_proto_gif *gifs, size_t count,
                                                              const uint8_t *existing, size_t existing_len,
                                                              uint8_t **out, size_t *out_len, ccord_error *err);
[[nodiscard]] ccord_status ccord_user_expressions_encode_gifs_b64(const ccord_proto_gif *gifs, size_t count,
                                                                  const char *existing_b64, size_t existing_b64_len,
                                                                  char **out, ccord_error *err);
[[nodiscard]] ccord_status ccord_user_expressions_encode_gifs_json(const ccord_proto_gif *gifs, size_t count,
                                                                   const char *existing_json, size_t existing_json_len,
                                                                   char **out, ccord_error *err);
// ---- Status (PreloadedUserSettings, GET/PATCH /users/@me/settings-proto/1) ------------------------------------
// Schema (checked 2026-11 against https://github.com/discord-userdoccers/discord-protos, raw:
// discord_protos/discord_users/v1/PreloadedUserSettings.proto; matches docs.discord.food/resources/user-settings-proto):
//   PreloadedUserSettings { StatusSettings status = 11; ... }  (all other top-level fields are left alone)
//   StatusSettings { StringValue status = 1;            // "online" | "idle" | "dnd" | "invisible"
//                    CustomStatus custom_status = 2;
//                    BoolValue show_current_game = 3;   // preserved, not decoded
//                    fixed64 status_expires_at_ms = 4;
//                    UInt64Value status_created_at_ms = 5; }  // preserved, not decoded
//   CustomStatus   { string text = 1; fixed64 emoji_id = 2; string emoji_name = 3; fixed64 expires_at_ms = 4;
//                    fixed64 created_at_ms = 5; StringValue label = 6; }
// StringValue is a message {1: string}. 0 / empty means "unset" for the fixed64 and string fields (proto3).
#define CCORD_PROTO_STATUS_MAX       15u    // bytes of the status string
#define CCORD_PROTO_CUSTOM_TEXT_MAX  512u   // bytes of the custom status text (Discord allows 128 characters)
#define CCORD_PROTO_EMOJI_NAME_MAX   128u   // bytes of the custom status emoji name

typedef struct ccord_proto_status {
  bool present;                                      // decode: the status submessage (field 11) was on the wire
  bool has_status;                                   // status StringValue present
  char status[CCORD_PROTO_STATUS_MAX + 1];           // "online" | "idle" | "dnd" | "invisible"
  uint64_t status_expires_at_ms;                     // 0 = none
  bool has_custom;                                   // custom_status present
  char custom_text[CCORD_PROTO_CUSTOM_TEXT_MAX + 1];
  uint64_t emoji_id;                                 // 0 = unicode or no emoji
  char emoji_name[CCORD_PROTO_EMOJI_NAME_MAX + 1];   // unicode emoji, or the custom emoji's name
  uint64_t custom_expires_at_ms;                     // 0 = never
} ccord_proto_status;

// Decodes the status submessage of raw type-1 settings bytes. *out is zeroed first; an empty buffer or one
// without field 11 yields present == false. Repeated field-11 records are merged (later fields win).
// Errors: CCORD_ERR_INVALID (malformed, NUL / invalid UTF-8 in a string), CCORD_ERR_RANGE (input > 1 MiB or a
// string longer than its buffer).
[[nodiscard]] ccord_status ccord_user_status_decode(const uint8_t *buf, size_t len, ccord_proto_status *out,
                                                    ccord_error *err);
[[nodiscard]] ccord_status ccord_user_status_decode_b64(const char *b64, size_t len, ccord_proto_status *out,
                                                        ccord_error *err);

// Returns type-1 settings bytes in which only the status submessage differs from `base` (may be NULL with
// length 0). `st` is the desired state of status (1), custom_status (2) and status_expires_at_ms (4):
//   has_status false  -> field 1 omitted;   has_custom false -> custom_status removed (clearing it);
//   status_expires_at_ms / emoji_id / custom_expires_at_ms 0 and empty strings are omitted.
// Every other top-level record, and every other subfield of the status submessage (show_current_game,
// status_created_at_ms, unknown ones) is copied byte for byte, in order. The submessage is written last. The
// old custom_status is replaced wholesale, so its created_at_ms / label are not carried over. `st->present` is ignored.
// If has_status, status must be online, idle, dnd or invisible. Text/emoji strings must be valid UTF-8 without NUL.
// *out is NULL-terminated on success, NULL on failure; free with ccord_free.
// Errors: CCORD_ERR_INVALID (bad status string/strings/base), CCORD_ERR_RANGE, CCORD_ERR_NOMEM.
[[nodiscard]] ccord_status ccord_user_status_patch(const uint8_t *base, size_t base_len, const ccord_proto_status *st,
                                                   uint8_t **out, size_t *out_len, ccord_error *err);
// Same with a bare base64 blob in (may be NULL/0) and padded standard base64 out.
[[nodiscard]] ccord_status ccord_user_status_patch_b64(const char *base_b64, size_t base_b64_len,
                                                       const ccord_proto_status *st, char **out, ccord_error *err);

// Safe on a zeroed or already-freed struct.
void ccord_user_expressions_free(ccord_user_expressions *x);
