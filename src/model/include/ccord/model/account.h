// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/model/types.h>

typedef struct ccord_user_settings {
  bool default_guilds_restricted;          // Privacy: DMs from server members (inverted in UI)
  uint8_t explicit_content_filter;         // 0 off, 1 non-friends, 2 all
  bool friend_source_all, friend_source_mutual_friends, friend_source_mutual_guilds;
  bool allow_access_to_age_restricted_servers;
  bool developer_mode, gif_auto_play, animate_emoji, enable_tts_command;
  bool stream_notifications_enabled, message_display_compact;
  uint8_t animate_stickers;                // 0 always, 1 hover/focus, 2 never
  char locale[16];                         // "en-US" etc.
  uint8_t profile_privacy_scope;           // 0 friends & all servers, 1 friends & small servers, 2 friends only
  bool share_profile_updates;              // share when updating profile
} ccord_user_settings;

typedef enum ccord_authenticator_type : uint32_t {
  CCORD_AUTH_TYPE_WEBAUTHN = 1u << 0,      // Discord authenticator_types value 1
  CCORD_AUTH_TYPE_TOTP     = 1u << 1,      // Discord authenticator_types value 2
  CCORD_AUTH_TYPE_SMS      = 1u << 2,      // Discord authenticator_types value 3
} ccord_authenticator_type;

// Private self-account metadata kept strictly in RAM inside the session vault.
// Never stored in ccord_store, never persisted to cache, never exposed to plugins without Class 3 account.read.
typedef struct ccord_account_private {
  bool valid;
  char email[128];
  char phone[32];
  bool verified;
  bool mfa_enabled;
  bool sms_mfa_enabled;
  uint32_t authenticator_types;            // bitmask of ccord_authenticator_type
  bool nsfw_allowed;
  uint8_t premium_type;                    // 0 none, 1 Nitro Classic, 2 Nitro, 3 Nitro Basic
} ccord_account_private;

typedef enum ccord_account_standing_tier : uint8_t {
  CCORD_STANDING_ALL_GOOD   = 0,
  CCORD_STANDING_LIMITED    = 1,
  CCORD_STANDING_VERY_LIM   = 2,
  CCORD_STANDING_AT_RISK    = 3,
  CCORD_STANDING_SUSPENDED  = 4,
} ccord_account_standing_tier;

#define CCORD_STANDING_MAX_VIOLATIONS 8u
typedef struct ccord_standing_violation {
  char id[64];
  char title[96];
  char detail[192];
  int64_t expires_at_ms;
} ccord_standing_violation;

typedef struct ccord_account_standing {
  bool loaded;
  ccord_account_standing_tier tier;
  size_t violation_count;
  ccord_standing_violation violations[CCORD_STANDING_MAX_VIOLATIONS];
} ccord_account_standing;

#define CCORD_WEBAUTHN_MAX_CREDS 16u
typedef struct ccord_webauthn_cred {
  char id[64];
  char name[64];
  int64_t created_at_ms;
} ccord_webauthn_cred;

typedef struct ccord_auth_session {
  char id_hash[64]; char os[32]; char browser[32]; char device[64];
  char location[64]; int64_t approx_last_used_ms; bool current;
} ccord_auth_session;
typedef struct ccord_connection {
  char id[64]; char name[64]; char type[32];
  bool revoked, verified, friend_sync, show_activity, two_way_link;
  uint8_t visibility;                      // 0 none, 1 everyone
} ccord_connection;
// PATCH /users/@me/settings payload: -1 / empty = unchanged sentinel per field.
typedef struct ccord_user_settings_patch {
  int8_t default_guilds_restricted, friend_source_all, friend_source_mutual_friends,
         friend_source_mutual_guilds, allow_access_to_age_restricted_servers,
         developer_mode, gif_auto_play, animate_emoji, enable_tts_command,
         stream_notifications_enabled, share_profile_updates;     // -1 unchanged, 0/1 value
  int16_t explicit_content_filter, animate_stickers, profile_privacy_scope;   // -1 unchanged
  char locale[16];                         // "" unchanged
} ccord_user_settings_patch;
#define CCORD_USER_SETTINGS_PATCH_INIT { .default_guilds_restricted = -1, .friend_source_all = -1, \
  .friend_source_mutual_friends = -1, .friend_source_mutual_guilds = -1, \
  .allow_access_to_age_restricted_servers = -1, .developer_mode = -1, .gif_auto_play = -1, \
  .animate_emoji = -1, .enable_tts_command = -1, .stream_notifications_enabled = -1, \
  .share_profile_updates = -1, .explicit_content_filter = -1, .animate_stickers = -1, \
  .profile_privacy_scope = -1, .locale = "" }
// PATCH /users/@me/connections/{name}/{id}: -1 unchanged, 0/1 value.
typedef struct ccord_connection_edit { int8_t visibility, friend_sync, show_activity; } ccord_connection_edit;
