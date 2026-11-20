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
} ccord_user_settings;
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
         stream_notifications_enabled;     // -1 unchanged, 0/1 value
  int16_t explicit_content_filter, animate_stickers;   // -1 unchanged
  char locale[16];                         // "" unchanged
} ccord_user_settings_patch;
#define CCORD_USER_SETTINGS_PATCH_INIT { .default_guilds_restricted = -1, .friend_source_all = -1, \
  .friend_source_mutual_friends = -1, .friend_source_mutual_guilds = -1, \
  .allow_access_to_age_restricted_servers = -1, .developer_mode = -1, .gif_auto_play = -1, \
  .animate_emoji = -1, .enable_tts_command = -1, .stream_notifications_enabled = -1, \
  .explicit_content_filter = -1, .animate_stickers = -1, .locale = "" }
// PATCH /users/@me/connections/{name}/{id}: -1 unchanged, 0/1 value.
typedef struct ccord_connection_edit { int8_t visibility, friend_sync, show_activity; } ccord_connection_edit;
