// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/model/types.h>

typedef enum ccord_scheduled_event_status : uint8_t {
  CCORD_SCHEDULED_EVENT_SCHEDULED = 1,
  CCORD_SCHEDULED_EVENT_ACTIVE = 2,
  CCORD_SCHEDULED_EVENT_COMPLETED = 3,
  CCORD_SCHEDULED_EVENT_CANCELED = 4,
} ccord_scheduled_event_status;
typedef enum ccord_scheduled_event_entity_type : uint8_t {
  CCORD_SCHEDULED_EVENT_STAGE_INSTANCE = 1,
  CCORD_SCHEDULED_EVENT_VOICE = 2,
  CCORD_SCHEDULED_EVENT_EXTERNAL = 3,
} ccord_scheduled_event_entity_type;
typedef struct ccord_scheduled_event {
  ccord_id id, guild_id, channel_id, creator_id;   // channel_id = 0 for EXTERNAL events
  char name[104];
  char description[512];                           // "" when none
  char location[104];                              // EXTERNAL events: where it happens ("" otherwise)
  int64_t scheduled_start_ms, scheduled_end_ms;    // unix ms (end 0 = none)
  uint8_t privacy_level;                           // 2 = GUILD_ONLY (the only value Discord uses)
  uint8_t status;                                  // ccord_scheduled_event_status
  uint8_t entity_type;                             // ccord_scheduled_event_entity_type
  uint32_t user_count;                             // interested users (REST ?with_user_count=true; 0 = unknown)
  bool me_rsvped;                                  // the signed-in user RSVP'd (event users list)
} ccord_scheduled_event;
// Embedded Activities (GET /activities/shelf + gateway EMBEDDED_ACTIVITY_UPDATE_V2). An "app" is one entry on a
// guild's activity shelf; an "instance" is one running copy of an app in a voice channel. Both are transport answers
// only: the store keeps instances per voice channel, the session keeps the shelf per guild.
#define CCORD_ACTIVITY_MAX_PARTICIPANTS 25u
#define CCORD_ACTIVITY_DESC_MAX_BYTES 256u
#define CCORD_ACTIVITY_HASH_MAX_BYTES 64u
typedef struct ccord_activity_app {
  ccord_id application_id;
  char name[CCORD_NAME_MAX_BYTES + 1];
  char description[CCORD_ACTIVITY_DESC_MAX_BYTES + 1];   // "" when none
  char icon_hash[CCORD_ACTIVITY_HASH_MAX_BYTES + 1];     // "" when none
  char cover_hash[CCORD_ACTIVITY_HASH_MAX_BYTES + 1];    // "" when none
  uint32_t max_participants;                             // 0 = not reported
  uint32_t shelf_rank;                                   // 0 = not reported (lowest rank)
} ccord_activity_app;
typedef struct ccord_activity_instance {
  ccord_id instance_id;
  ccord_id application_id;
  ccord_id guild_id;
  ccord_id channel_id;
  ccord_id location_id;                                  // path segment for the leave/update calls
  ccord_id participants[CCORD_ACTIVITY_MAX_PARTICIPANTS];
  size_t participant_count;                              // 0 (or absent from the payload) = the instance ended
} ccord_activity_instance;
// Join/leave actions for ccord_transport.call_action; ringing itself is passive (CALL_CREATE).

// Discord system_channel_flags bits (https://discord.com/developers/docs/resources/guild#guild-object-system-channel-flags).
#define CCORD_SYSCHAN_SUPPRESS_JOIN_NOTIFICATIONS           (1u << 0)
#define CCORD_SYSCHAN_SUPPRESS_PREMIUM_SUBSCRIPTIONS        (1u << 1)
#define CCORD_SYSCHAN_SUPPRESS_GUILD_REMINDER_NOTIFICATIONS (1u << 2)
#define CCORD_SYSCHAN_SUPPRESS_JOIN_NOTIFICATION_REPLIES    (1u << 3)
#define CCORD_SYSCHAN_KNOWN_MASK                            0x0Fu
typedef struct ccord_guild {
  ccord_id id;
  const char *name;
  const char *icon_url;                        // CDN URL or "" / nullptr if none
  ccord_id owner_id;                           // guild owner user id (0 when unknown)
  uint64_t self_permissions;                   // computed guild permissions for self (0 when unknown)
  ccord_id folder_id;                          // server folder id (0 when ungrouped)
  char folder_name[64];                        // server folder display name
  uint32_t folder_color;                       // 0xRRGGBB folder tint (0 = default)
  char banner_url[256];                        // CDN banner URL (empty when none)
  ccord_id system_channel_id;                  // system messages channel (0 = none)
  uint32_t system_channel_flags;               // CCORD_SYSCHAN_* bitmask
  uint8_t default_message_notifications;       // 0 = all messages, 1 = only @mentions
  ccord_id afk_channel_id;                     // inactive voice channel (0 = none)
  uint32_t afk_timeout;                        // 60 / 300 / 900 / 1800 / 3600 seconds (0 = unset)
  uint8_t verification_level;                  // 0..4 (None, Low, Medium, High, Highest)
  uint8_t explicit_content_filter;             // 0..2 (Disabled, Members without roles, All members)
  uint8_t premium_tier;                        // 0..3 boost tier
  uint32_t premium_subscription_count;         // total server boosts
  char vanity_url_code[64];                    // vanity invite slug (empty when none)
  ccord_id rules_channel_id;                   // community rules channel (0 = none)
  ccord_id public_updates_channel_id;          // community public updates channel (0 = none)
  bool feature_community;                      // COMMUNITY in guild.features
  bool feature_vanity_url;                     // VANITY_URL in guild.features
  bool feature_banner;                         // BANNER in guild.features
  bool has_guild_settings;                     // true when overview/safety fields were populated
  bool has_incidents_data;                     // true when incidents_data was populated
  int64_t invites_disabled_until_ms;           // unix ms (0 = not paused)
  int64_t dms_disabled_until_ms;               // unix ms (0 = not paused)
} ccord_guild;
// Ordered entry in the user's server rail / folder layout (guild_folders in user settings).
typedef struct ccord_guild_folder_item {
  ccord_id guild_id;
  ccord_id folder_id;                          // 0 when ungrouped / standalone
  char folder_name[64];
  uint32_t folder_color;                       // 0xRRGGBB (0 = default)
} ccord_guild_folder_item;
// Invite preview metadata from GET /invites/{code}?with_counts=true.
typedef struct ccord_invite_preview {
  char code[64];
  ccord_id guild_id;
  char guild_name[CCORD_NAME_MAX_BYTES + 1];
  char guild_icon_url[256];
  char guild_description[256];
  ccord_id channel_id;
  char channel_name[CCORD_NAME_MAX_BYTES + 1];
  uint32_t online_count;
  uint32_t member_count;
} ccord_invite_preview;
// Discoverable server card from GET /discoverable-guilds.
#define CCORD_DISCOVERY_PAGE_MAX 24u
typedef struct ccord_discover_guild {
  ccord_id guild_id;
  char name[CCORD_NAME_MAX_BYTES + 1];
  char description[256];
  char icon_url[256];
  char vanity_code[64];
  uint32_t primary_category_id;
  uint32_t online_count;
  uint32_t member_count;
} ccord_discover_guild;
// Guild profile summary from GET /guilds/{guild_id}/profile (e.g. clan tag badge card).
typedef struct ccord_guild_profile {
  ccord_id guild_id;
  char name[CCORD_NAME_MAX_BYTES + 1];
  char description[256];
  char icon_url[256];
  char banner_url[256];
  char tag[16];
  char badge_url[256];
  uint32_t brand_color;
  bool has_brand_color;
  uint32_t online_count;
  uint32_t member_count;
  bool unavailable;
} ccord_guild_profile;

// Membership screening: the form a joining member must accept before Discord lets them send messages. The GET
// answer (see ccord/net/admin.h) fills this; the store deep-copies every string. Longer text and extra entries are
// truncated by the parser to the caps below (Discord's own form is far smaller), never rejected outright.
#define CCORD_MEMBER_VERIFICATION_MAX_FIELDS        8u
#define CCORD_MEMBER_VERIFICATION_MAX_VALUES        16u
#define CCORD_MEMBER_VERIFICATION_VERSION_MAX_BYTES 32u
#define CCORD_MEMBER_VERIFICATION_DESC_MAX_BYTES    512u
#define CCORD_MEMBER_VERIFICATION_LABEL_MAX_BYTES   100u
#define CCORD_MEMBER_VERIFICATION_VALUE_MAX_BYTES   100u
typedef enum ccord_member_verification_field_type : uint8_t {
  CCORD_MEMBER_VERIFICATION_TERMS      = 0,   // accept the terms in values[]
  CCORD_MEMBER_VERIFICATION_TEXT_INPUT = 1,   // free-text answer
} ccord_member_verification_field_type;
typedef struct ccord_member_verification_field {
  uint8_t field_type;                          // ccord_member_verification_field_type (0 = TERMS, 1 = TEXT_INPUT)
  const char *label;                           // <= CCORD_MEMBER_VERIFICATION_LABEL_MAX_BYTES (store deep-copies)
  const char *values[CCORD_MEMBER_VERIFICATION_MAX_VALUES]; // <= 16 entries, each <= the value byte cap
  size_t value_count;
  bool required;
} ccord_member_verification_field;
typedef struct ccord_member_verification {
  const char *version;                         // <= CCORD_MEMBER_VERIFICATION_VERSION_MAX_BYTES (store deep-copies)
  const char *description;                     // <= CCORD_MEMBER_VERIFICATION_DESC_MAX_BYTES ("" when none)
  ccord_member_verification_field fields[CCORD_MEMBER_VERIFICATION_MAX_FIELDS]; // <= 8 (store deep-copies)
  size_t field_count;
} ccord_member_verification;
typedef struct ccord_member {
  ccord_id guild_id, user_id;
  const char *role_name;                       // group header in member list, e.g. "Admins"
  uint32_t role_color;                         // 0xRRGGBB, 0 = default text colour
  uint8_t role_rank;                           // lower = listed first; offline members are grouped last by UI
  char nick[CCORD_NAME_MAX_BYTES + 1];         // server-specific nickname (empty when none set)
  uint64_t permissions;                        // computed member permissions in the guild (0 when unset)
  int64_t timeout_until_ms;                    // communication_disabled_until unix ms (0 when not timed out)
  int64_t joined_at_ms;                        // joined_at unix ms (0 when unknown)
  bool mute, deaf;                             // server-enforced voice mute / deafen
  bool omit_mute, omit_deaf;                   // true when partial gateway payload omitted mute / deaf
  bool pending;                                // membership screening not completed yet; self cannot send until accepted
  bool omit_pending;                           // true when a partial gateway payload omitted pending
  char avatar_decoration_url[256];             // guild_member avatar_decoration_data URL (empty when none)
  char nameplate_asset[128];                   // guild_member collectibles.nameplate asset (empty when none)
  char nameplate_palette[24];                  // guild_member collectibles.nameplate palette (empty when none)
} ccord_member;
