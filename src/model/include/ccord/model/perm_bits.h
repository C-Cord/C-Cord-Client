// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/model/types.h>

// Discord permission bits (https://discord.com/developers/docs/topics/permissions). Values are 64-bit.
#define CCORD_PERM_CREATE_INSTANT_INVITE (1ULL << 0)
#define CCORD_PERM_KICK_MEMBERS          (1ULL << 1)
#define CCORD_PERM_BAN_MEMBERS           (1ULL << 2)
#define CCORD_PERM_ADMINISTRATOR         (1ULL << 3)
#define CCORD_PERM_MANAGE_CHANNELS       (1ULL << 4)
#define CCORD_PERM_MANAGE_GUILD          (1ULL << 5)
#define CCORD_PERM_ADD_REACTIONS         (1ULL << 6)
#define CCORD_PERM_VIEW_AUDIT_LOG        (1ULL << 7)
#define CCORD_PERM_PRIORITY_SPEAKER      (1ULL << 8)
#define CCORD_PERM_STREAM                (1ULL << 9)
#define CCORD_PERM_VIEW_CHANNEL          (1ULL << 10)
#define CCORD_PERM_SEND_MESSAGES         (1ULL << 11)
#define CCORD_PERM_SEND_TTS_MESSAGES     (1ULL << 12)
#define CCORD_PERM_MANAGE_MESSAGES       (1ULL << 13)
#define CCORD_PERM_EMBED_LINKS           (1ULL << 14)
#define CCORD_PERM_ATTACH_FILES          (1ULL << 15)
#define CCORD_PERM_READ_MESSAGE_HISTORY  (1ULL << 16)
#define CCORD_PERM_MENTION_EVERYONE      (1ULL << 17)
#define CCORD_PERM_USE_EXTERNAL_EMOJIS   (1ULL << 18)
#define CCORD_PERM_VIEW_GUILD_INSIGHTS   (1ULL << 19)
#define CCORD_PERM_CONNECT               (1ULL << 20)
#define CCORD_PERM_SPEAK                 (1ULL << 21)
#define CCORD_PERM_MUTE_MEMBERS          (1ULL << 22)
#define CCORD_PERM_DEAFEN_MEMBERS        (1ULL << 23)
#define CCORD_PERM_MOVE_MEMBERS          (1ULL << 24)
#define CCORD_PERM_USE_VAD               (1ULL << 25)
#define CCORD_PERM_CHANGE_NICKNAME       (1ULL << 26)
#define CCORD_PERM_MANAGE_NICKNAMES      (1ULL << 27)
#define CCORD_PERM_MANAGE_ROLES          (1ULL << 28)
#define CCORD_PERM_MANAGE_WEBHOOKS       (1ULL << 29)
#define CCORD_PERM_MANAGE_EXPRESSIONS    (1ULL << 30)
#define CCORD_PERM_USE_APPLICATION_COMMANDS (1ULL << 31)
#define CCORD_PERM_REQUEST_TO_SPEAK      (1ULL << 32)
#define CCORD_PERM_MANAGE_EVENTS         (1ULL << 33)
#define CCORD_PERM_MANAGE_THREADS        (1ULL << 34)
#define CCORD_PERM_CREATE_PUBLIC_THREADS (1ULL << 35)
#define CCORD_PERM_CREATE_PRIVATE_THREADS (1ULL << 36)
#define CCORD_PERM_USE_EXTERNAL_STICKERS (1ULL << 37)
#define CCORD_PERM_SEND_MESSAGES_IN_THREADS (1ULL << 38)
#define CCORD_PERM_USE_EMBEDDED_ACTIVITIES (1ULL << 39)
#define CCORD_PERM_MODERATE_MEMBERS      (1ULL << 40)
#define CCORD_PERM_VIEW_CREATOR_ANALYTICS (1ULL << 41)
#define CCORD_PERM_USE_SOUNDBOARD        (1ULL << 42)
#define CCORD_PERM_CREATE_EXPRESSIONS    (1ULL << 43)
#define CCORD_PERM_CREATE_EVENTS         (1ULL << 44)
#define CCORD_PERM_USE_EXTERNAL_SOUNDS   (1ULL << 45)
#define CCORD_PERM_SEND_VOICE_MESSAGES   (1ULL << 46)
#define CCORD_PERM_SET_VOICE_CHANNEL_STATUS (1ULL << 48)
#define CCORD_PERM_SEND_POLLS            (1ULL << 49)
#define CCORD_PERM_USE_EXTERNAL_APPS     (1ULL << 50)
#define CCORD_PERM_PIN_MESSAGES          (1ULL << 51)
#define CCORD_PERM_BYPASS_SLOWMODE       (1ULL << 52)
// A role (guild-wide permission bundle). Strings are borrowed; the store deep-copies them. The @everyone role has
// the same id as its guild. `position` orders the role hierarchy (higher outranks lower; @everyone is 0).
typedef struct ccord_role {
  ccord_id id;
  ccord_id guild_id;                           // owning guild (set when roles travel in a list that spans guilds)
  const char *name;
  uint32_t color;                              // 0xRRGGBB, 0 = no colour
  int32_t position;
  uint64_t permissions;
  bool hoist;                                  // listed separately in the member list
  bool mentionable;
  bool managed;                                // belongs to a bot or integration: cannot be assigned by hand
  char unicode_emoji[16];                      // optional UTF-8 role icon emoji ("" when none)
} ccord_role;
// Server-administration lists (Server Settings). Strings are borrowed; the store deep-copies them.

typedef enum ccord_overwrite_kind : uint8_t { CCORD_OVERWRITE_ROLE = 0, CCORD_OVERWRITE_MEMBER = 1 } ccord_overwrite_kind;
typedef struct ccord_overwrite {
  ccord_id id;                                 // role id or user id, per `kind`
  ccord_overwrite_kind kind;
  uint64_t allow, deny;
} ccord_overwrite;
// Which roles a member holds, and a channel's overwrites, as they travel in events. Arrays are borrowed; the store copies.
typedef struct ccord_member_roles {
  ccord_id guild_id, user_id;
  const ccord_id *role_ids; size_t count;      // excludes @everyone (its id is the guild id)
} ccord_member_roles;
// How many members the server says a member-list group holds (the member list is loaded a piece at a time, so the
// rows we hold are not the whole group). A group is a hoisted role, everyone else online, or everyone offline.
typedef enum ccord_member_group_kind : uint8_t {
  CCORD_MEMBER_GROUP_ROLE = 0,   // role_id says which
  CCORD_MEMBER_GROUP_ONLINE,     // online members with no hoisted role
  CCORD_MEMBER_GROUP_OFFLINE,
} ccord_member_group_kind;
typedef struct ccord_member_group {
  ccord_id role_id;              // CCORD_MEMBER_GROUP_ROLE only
  uint32_t count;
  ccord_member_group_kind kind;
} ccord_member_group;
typedef struct ccord_channel_overwrites {
  ccord_id channel_id;
  const ccord_overwrite *items; size_t count;
} ccord_channel_overwrites;
#define CCORD_MAX_ROLES_PER_GUILD 250u
#define CCORD_MAX_OVERWRITES_PER_CHANNEL 500u
