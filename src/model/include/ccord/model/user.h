// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/model/types.h>

typedef enum ccord_presence : uint8_t { CCORD_PRESENCE_OFFLINE, CCORD_PRESENCE_ONLINE, CCORD_PRESENCE_IDLE, CCORD_PRESENCE_DND } ccord_presence;
// Voice and live-streaming configuration. The encoder is the GPU/codec backend,
// the codec the wire format, and the framework how frames are captured.

typedef enum ccord_activity_type : uint8_t {
  CCORD_ACTIVITY_PLAYING = 0,
  CCORD_ACTIVITY_STREAMING = 1,
  CCORD_ACTIVITY_LISTENING = 2,
  CCORD_ACTIVITY_WATCHING = 3,
  CCORD_ACTIVITY_CUSTOM = 4,
  CCORD_ACTIVITY_COMPETING = 5,
} ccord_activity_type;
typedef struct ccord_user {
  ccord_id id;
  union {
    const char *name;                          // display name
    const char *display_name;
  };
  union {
    const char *handle;                        // username / handle, "" / nullptr if none
    const char *username;
  };
  const char *status_text;                     // "" if none
  const char *avatar_url;                      // CDN URL or "" / nullptr if none
  const char *avatar_decoration_url;           // CDN URL or "" / nullptr if none
  ccord_id avatar_decoration_sku_id;           // avatar_decoration_data.sku_id (0 when none/unknown)
  const char *banner_url;                      // CDN URL or "" / nullptr if none
  const char *bio;                             // profile bio / about me, "" / nullptr if none
  const char *pronouns;                        // profile pronouns, "" / nullptr if none
  ccord_id clan_guild_id;
  ccord_id nameplate_sku_id;                   // collectibles.nameplate.sku_id (0 when none/unknown)
  int64_t custom_status_expires_ms;            // unix ms (0 = never / unset)
  int64_t activity_start_ms;                   // unix ms (0 = unset)
  int64_t activity_end_ms;                     // unix ms (0 = unset)
  uint32_t accent_color;                       // 0xRRGGBB (0 if none)
  ccord_presence presence;
  bool has_accent_color;
  bool has_presence;
  bool is_bot;
  uint8_t avatar_palette;                      // index into theme avatar_0..5 (taken mod 6)
  bool has_activity;
  uint8_t activity_type;                       // ccord_activity_type (0, 1, 2, 3, 5)
  char clan_tag[16];                           // server identity / clan tag badge (empty when none)
  char clan_badge[64];
  char nameplate_asset[128];                   // collectibles.nameplate asset path or CDN URL (empty when none)
  char nameplate_palette[24];                  // collectibles.nameplate palette slug (e.g. "cobalt", empty when none)
  // Custom status (activity type 4 / user_settings.custom_status) and primary rich-presence activity (types 0..3, 5).
  char custom_status_emoji[CCORD_EMOJI_MAX_BYTES + 1];
  char custom_status_text[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  char activity_name[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  char activity_details[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  char activity_state[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
} ccord_user;
typedef struct ccord_profile_connection {
  char type[32];                               // platform slug ("github", "steam", "twitch", "spotify", ...)
  char name[CCORD_NAME_MAX_BYTES + 1];
  bool verified;
} ccord_profile_connection;
typedef struct ccord_mutual_friend {
  ccord_id user_id;
  char name[CCORD_NAME_MAX_BYTES + 1];
  char handle[CCORD_NAME_MAX_BYTES + 1];
  char avatar_url[256];
} ccord_mutual_friend;
// One badge from user.profile_badges / public_flags on the profile payload.
typedef struct ccord_profile_badge {
  char id[64];                                 // badge id slug (profile badge "id")
  char description[128];                       // badge tooltip text ("description")
  char icon_url[256];                          // badge icon CDN URL ("icon")
} ccord_profile_badge;
#define CCORD_PROFILE_EFFECT_MAX_LAYERS 4u
// One animated layer of a Discord profile effect (GET /user-profile-effects -> profile_effect_configs[].effects[]).
typedef struct ccord_profile_effect_layer {
  char src[256];                               // APNG/PNG CDN URL
  bool loop;
  int32_t width, height;                       // reference dimensions (typically 450x880)
  int32_t start_ms;                            // start delay from profile open (ms)
  int32_t duration_ms;                         // active duration (ms; 0 = unbounded)
  int32_t loop_delay_ms;                       // pause between loops (ms)
  int32_t pos_x, pos_y;                        // position offset in reference coordinates
  int32_t z_index;                             // layer draw order
} ccord_profile_effect_layer;
typedef struct ccord_profile_effect_config {
  ccord_id id;
  ccord_id sku_id;
  char title[96];                              // display title ("title" / "name")
  char description[160];                       // accessibility / description text
  char thumbnail_url[256];                     // thumbnailPreviewSrc (APNG/PNG)
  char reduced_motion_url[256];                // reducedMotionSrc (APNG/PNG)
  char static_frame_url[256];                  // staticFrameSrc (PNG)
  size_t layer_count;
  ccord_profile_effect_layer layers[CCORD_PROFILE_EFFECT_MAX_LAYERS];
} ccord_profile_effect_config;

#define CCORD_PROFILE_FRAME_MAX_LAYERS 4u
// One layer of a Discord profile frame (PROFILE_FRAME collectible item layers[]).
typedef struct ccord_profile_frame_layer {
  ccord_id id;                                 // layer asset snowflake id
  char url[256];                               // animated or primary CDN URL
  char static_url[256];                        // static fallback CDN URL
  int32_t type;
  int32_t order;                               // render order
  int32_t anchor;                              // 0 = TOP, 1 = BOTTOM
  bool responsive;
} ccord_profile_frame_layer;
typedef struct ccord_profile_frame_config {
  ccord_id sku_id;
  int32_t inner_width;                         // reference inner width (e.g. 300)
  int32_t overflow_top;                        // top overflow outside profile card
  int32_t overflow_bottom;                     // bottom overflow outside profile card
  int32_t overflow_horizontal;                 // horizontal overflow on each side
  char static_url[256];                        // fallback full-frame static URL
  char animated_url[256];                      // fallback full-frame animated URL
  size_t layer_count;
  ccord_profile_frame_layer layers[CCORD_PROFILE_FRAME_MAX_LAYERS];
} ccord_profile_frame_config;

// Owned Discord Collectible item (from GET /users/@me/collectibles-purchases + /user-profile-effects).
#define CCORD_COLLECTIBLES_MAX 256u
typedef enum ccord_collectible_kind : uint8_t {
  CCORD_COLLECTIBLE_AVATAR_DECORATION = 0,     // Discord type 0
  CCORD_COLLECTIBLE_PROFILE_EFFECT = 1,        // Discord type 1
  CCORD_COLLECTIBLE_NAMEPLATE = 2,             // Discord type 2
  CCORD_COLLECTIBLE_PROFILE_FRAME = 3,         // Discord type 3
} ccord_collectible_kind;

typedef struct ccord_owned_collectible {
  ccord_collectible_kind kind;
  ccord_id sku_id;                             // SKU ID sent in PATCH /users/@me (deco/nameplate) or /users/@me/profile
  ccord_id item_id;                            // profile_effect_id (for PROFILE_EFFECT; equals sku_id when identical)
  char name[96];                               // human-readable display name
  char category[48];                           // optional category/bundle name or palette label
  char asset[128];                             // deco asset hash or nameplate asset path (e.g. "nameplates/...")
  char palette[24];                            // nameplate palette slug (e.g. "cobalt", "berry", "crimson")
  char cdn_url[256];                           // animated / primary CDN URL (deco passthrough=true or effect/frame thumb)
  char static_url[256];                        // static / reduced-motion CDN URL (deco passthrough=false or effect/frame static)
  int64_t expires_at_ms;                       // unix ms when rental/quest reward expires (0 = permanent)
  bool has_effect;                             // true when `effect` holds resolved profile effect layers/URLs
  bool has_frame;                              // true when `frame` holds resolved profile frame layers/overflow
  union {
    ccord_profile_effect_config effect;
    ccord_profile_frame_config frame;
  };
} ccord_owned_collectible;
typedef struct ccord_user_profile {
  union {
    ccord_id user_id;
    ccord_id id;
  };
  union {
    const char *display_name;                  // global_name / display name, "" / nullptr if none
    const char *name;
  };
  union {
    const char *handle;                        // username / handle, "" / nullptr if none
    const char *username;
  };
  const char *avatar_url;                      // CDN URL or "" / nullptr if none
  const char *banner_url;                      // CDN URL or "" / nullptr if none
  uint32_t accent_color;                       // 0xRRGGBB (0 if none)
  bool has_accent_color;
  bool has_banner_color;                       // true when banner_color is set (profile "banner_color")
  uint32_t banner_color;                       // 0xRRGGBB, profile "banner_color" (hex string)
  bool has_theme;                              // true when theme_primary / theme_accent are set
  uint32_t theme_primary;                      // 0xRRGGBB, profile "theme_colors"[0]
  uint32_t theme_accent;                       // 0xRRGGBB, profile "theme_colors"[1]
  const char *bio;                             // profile bio / about me, "" / nullptr if none
  const char *pronouns;                        // profile pronouns, "" / nullptr if none
  char clan_tag[16];
  ccord_id clan_guild_id;
  char clan_badge[64];
  size_t connection_count;
  ccord_profile_connection connections[CCORD_PROFILE_MAX_CONNECTIONS];
  size_t mutual_guild_count;
  ccord_id mutual_guild_ids[CCORD_PROFILE_MAX_MUTUAL_GUILDS];
  size_t mutual_friend_count;
  uint32_t mutual_friend_total;
  ccord_mutual_friend mutual_friends[CCORD_PROFILE_MAX_MUTUAL_FRIENDS];
  // Per-server profile (profile "guild_id" / "guild_member_profile"). guild_id 0 = no server profile.
  ccord_id guild_id;
  const char *guild_nick;                      // guild_member_profile "nick", "" / nullptr if none
  const char *guild_avatar_url;                // guild_member_profile "avatar", CDN URL or "" / nullptr
  const char *guild_banner_url;                // guild_member_profile "banner", CDN URL or "" / nullptr
  const char *guild_bio;                       // guild_member_profile "bio", "" / nullptr if none
  const char *guild_pronouns;                  // guild_member_profile "pronouns", "" / nullptr if none
  bool guild_has_theme;                        // true when guild_member_profile has theme_colors
  uint32_t guild_theme_primary;                // 0xRRGGBB, guild_member_profile "theme_colors"[0]
  uint32_t guild_theme_accent;                 // 0xRRGGBB, guild_member_profile "theme_colors"[1]
  size_t badge_count;
  ccord_profile_badge badges[CCORD_PROFILE_MAX_BADGES];  // profile "badges", capped at 16
  const char *avatar_decoration_url;           // user "avatar_decoration_data" asset URL, "" / nullptr if none
  ccord_id avatar_decoration_sku_id;           // user "avatar_decoration_data.sku_id" (0 = none/unknown)
  const char *guild_avatar_decoration_url;     // guild_member "avatar_decoration_data" asset URL, "" / nullptr if none
  ccord_id guild_avatar_decoration_sku_id;     // guild_member "avatar_decoration_data.sku_id" (0 = none/unknown)
  ccord_id nameplate_sku_id;                   // user collectibles.nameplate.sku_id
  char nameplate_asset[128];                   // user collectibles.nameplate asset path
  char nameplate_palette[24];                  // user collectibles.nameplate palette
  ccord_id guild_nameplate_sku_id;             // guild_member collectibles.nameplate.sku_id
  char guild_nameplate_asset[128];             // guild_member collectibles.nameplate asset path
  char guild_nameplate_palette[24];            // guild_member collectibles.nameplate palette
  ccord_id profile_effect_id;                  // user_profile.profile_effect.id (0 = none)
  ccord_id guild_profile_effect_id;            // guild_member_profile.profile_effect.id (0 = none)
  ccord_profile_effect_config profile_effect;  // resolved effect layers/URLs (id == 0 when unresolved/none)
  ccord_id profile_frame_sku_id;               // user_profile/collectibles profile_frame sku_id (0 = none)
  ccord_id guild_profile_frame_sku_id;         // guild_member_profile/collectibles profile_frame sku_id (0 = none)
  ccord_profile_frame_config profile_frame;    // resolved frame layers/overflow (sku_id == 0 when unresolved/none)
  int64_t guild_joined_at_ms;                  // guild_member "joined_at" as unix ms (0 = unknown)
  int64_t premium_since_ms;                    // guild_member "premium_since" as unix ms (0 = unknown / not boosting)
} ccord_user_profile;
typedef struct ccord_user_note {
  ccord_id user_id;
  const char *note;                            // borrowed UTF-8 note text (max CCORD_USER_NOTE_MAX_BYTES)
} ccord_user_note;
// M6 account settings. These are Discord account state, not local
// preferences: the session writes them only from a REST answer or USER_SETTINGS_UPDATE.

typedef enum ccord_relationship_kind : uint8_t {
  CCORD_RELATIONSHIP_NONE = 0,
  CCORD_RELATIONSHIP_FRIEND = 1,
  CCORD_RELATIONSHIP_BLOCKED = 2,
  CCORD_RELATIONSHIP_PENDING_INCOMING = 3,
  CCORD_RELATIONSHIP_PENDING_OUTGOING = 4,
  CCORD_RELATIONSHIP_IGNORED = 5,
  CCORD_REL_NONE = CCORD_RELATIONSHIP_NONE,
  CCORD_REL_FRIEND = CCORD_RELATIONSHIP_FRIEND,
  CCORD_REL_BLOCKED = CCORD_RELATIONSHIP_BLOCKED,
  CCORD_REL_INCOMING_REQUEST = CCORD_RELATIONSHIP_PENDING_INCOMING,
  CCORD_REL_OUTGOING_REQUEST = CCORD_RELATIONSHIP_PENDING_OUTGOING,
  CCORD_REL_IGNORED = CCORD_RELATIONSHIP_IGNORED,
} ccord_relationship_kind;
typedef struct ccord_relationship {
  ccord_id user_id;
  ccord_relationship_kind kind;
} ccord_relationship;
