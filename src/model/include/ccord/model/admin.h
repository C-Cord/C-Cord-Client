// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/model/types.h>
#include <ccord/model/perm_bits.h>

typedef struct ccord_ban {
  ccord_id user_id;
  const char *name;                            // display name of the banned user ("" when unknown)
  const char *reason;                          // "" when none was given
} ccord_ban;
typedef struct ccord_invite {
  const char *code;
  ccord_id channel_id;
  const char *channel_name;                    // "" when unknown
  ccord_id inviter_id;
  const char *inviter_name;                    // "" when unknown
  uint32_t uses;
  uint32_t max_uses;                           // 0 = unlimited
  uint32_t max_age_s;                          // 0 = never expires
  bool temporary;                              // members are removed when they go offline unless given a role
  int64_t created_ms;                          // unix ms, 0 when unknown
  int64_t expires_ms;                          // unix ms, 0 = never
} ccord_invite;
// One audit-log entry. `action` is Discord's numeric action type (see ccord/model/audit.h).
#define CCORD_AUDIT_MAX_CHANGES 8u
typedef struct ccord_audit_change {
  char key[48];
  char old_val[96];
  char new_val[96];
} ccord_audit_change;
typedef struct ccord_audit_entry {
  ccord_id id;                                 // snowflake; larger is newer
  int64_t time_ms;                             // unix ms (from the snowflake), 0 when unknown
  ccord_id user_id;                            // who did it
  const char *user_name;                       // "" when unknown
  ccord_id target_id;                          // what it was done to (user, role, channel ... per action), 0 when none
  const char *target_name;                     // "" when unknown
  uint16_t action;
  const char *reason;                          // "" when none
  const char *detail;                          // short summary of the change, "" when none
  size_t change_count;                         // structured expandable diff rows (capped at CCORD_AUDIT_MAX_CHANGES)
  ccord_audit_change changes[CCORD_AUDIT_MAX_CHANGES];
} ccord_audit_entry;

// Discord AutoMod rule (https://discord.com/developers/docs/resources/auto-moderation).
typedef enum ccord_automod_trigger_type : uint8_t {
  CCORD_AUTOMOD_TRIGGER_KEYWORD = 1,
  CCORD_AUTOMOD_TRIGGER_SPAM = 3,
  CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET = 4,
  CCORD_AUTOMOD_TRIGGER_MENTION_SPAM = 5,
  CCORD_AUTOMOD_TRIGGER_MEMBER_PROFILE = 6,
} ccord_automod_trigger_type;

typedef enum ccord_automod_event_type : uint8_t {
  CCORD_AUTOMOD_EVENT_MESSAGE_SEND = 1,
  CCORD_AUTOMOD_EVENT_MEMBER_UPDATE = 2,
} ccord_automod_event_type;

typedef enum ccord_automod_action_type : uint8_t {
  CCORD_AUTOMOD_ACTION_BLOCK_MESSAGE = 1,
  CCORD_AUTOMOD_ACTION_SEND_ALERT_MESSAGE = 2,
  CCORD_AUTOMOD_ACTION_TIMEOUT = 3,
  CCORD_AUTOMOD_ACTION_BLOCK_MEMBER_INTERACTION = 4,
} ccord_automod_action_type;

// Bitmask corresponding to Discord's keyword preset types (1 = profanity, 2 = sexual_content, 3 = slurs).
#define CCORD_AUTOMOD_PRESET_PROFANITY      (1u << 0)
#define CCORD_AUTOMOD_PRESET_SEXUAL_CONTENT (1u << 1)
#define CCORD_AUTOMOD_PRESET_SLURS          (1u << 2)
#define CCORD_AUTOMOD_PRESET_KNOWN_MASK     0x07u

#define CCORD_AUTOMOD_MAX_KEYWORDS          128u
#define CCORD_AUTOMOD_MAX_REGEX             10u
#define CCORD_AUTOMOD_MAX_ALLOW_LIST        128u
#define CCORD_AUTOMOD_KEYWORD_MAX_BYTES     60u
#define CCORD_AUTOMOD_REGEX_MAX_BYTES       260u
#define CCORD_AUTOMOD_CUSTOM_MSG_MAX_BYTES  150u
#define CCORD_AUTOMOD_TIMEOUT_MAX_S         (28u * 24u * 60u * 60u)
#define CCORD_AUTOMOD_MENTION_LIMIT_MAX     50u
#define CCORD_AUTOMOD_MAX_EXEMPT_ROLES      20u
#define CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS   50u

typedef struct ccord_automod_rule {
  ccord_id id, guild_id, creator_id;
  const char *name;                            // rule name (1..100 bytes; store deep-copies)
  uint8_t event_type;                          // ccord_automod_event_type (default 1 = MESSAGE_SEND)
  uint8_t trigger_type;                        // ccord_automod_trigger_type (1, 3, 4, 5, 6)
  bool enabled;
  // trigger_metadata
  const char *const *keywords;                 // KEYWORD / MEMBER_PROFILE keyword_filter
  size_t keyword_count;
  const char *const *regex_patterns;           // KEYWORD / MEMBER_PROFILE regex_patterns
  size_t regex_count;
  const char *const *allow_list;               // KEYWORD / KEYWORD_PRESET / MEMBER_PROFILE allow_list
  size_t allow_count;
  uint32_t presets;                            // KEYWORD_PRESET: CCORD_AUTOMOD_PRESET_* bitmask
  uint8_t mention_total_limit;                 // MENTION_SPAM: 1..50
  bool mention_raid_protection;                // MENTION_SPAM: mention_raid_protection_enabled
  // actions[]
  bool action_block;                           // BLOCK_MESSAGE (type 1)
  const char *custom_message;                  // optional block message (<= 150 bytes; "" when none)
  bool action_alert;                           // SEND_ALERT_MESSAGE (type 2)
  ccord_id alert_channel_id;                   // channel id when action_alert is true
  bool action_timeout;                         // TIMEOUT (type 3)
  uint32_t timeout_seconds;                    // >0..2419200 when action_timeout is true
  bool action_block_interaction;               // BLOCK_MEMBER_INTERACTION (type 4)
  // exemptions
  size_t exempt_role_count;
  ccord_id exempt_role_ids[CCORD_AUTOMOD_MAX_EXEMPT_ROLES];
  size_t exempt_channel_count;
  ccord_id exempt_channel_ids[CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS];
} ccord_automod_rule;

// Result of GET /guilds/{g}/prune (preview) or POST /guilds/{g}/prune (execute).
#define CCORD_PRUNE_MAX_ROLES 20u
typedef struct ccord_prune_info {
  int32_t pruned;                              // >= 0 member count, or -1 when compute_prune_count=false returned null
  uint8_t days;                                // 1..30 days requested
  bool executed;                               // false = preview (GET), true = executed (POST)
  bool has_result;                             // true once an answer arrived for this guild
} ccord_prune_info;

// ---- Integrations: webhooks, followed announcement channels, app command permissions (M4c) ----
typedef enum ccord_webhook_type : uint8_t {
  CCORD_WEBHOOK_INCOMING         = 1,
  CCORD_WEBHOOK_CHANNEL_FOLLOWER = 2,
  CCORD_WEBHOOK_APPLICATION      = 3,
} ccord_webhook_type;

#define CCORD_WEBHOOK_NAME_MAX_BYTES 80u

typedef struct ccord_webhook {
  ccord_id id, guild_id, channel_id, creator_id, application_id;
  uint8_t type;                                // ccord_webhook_type (1..3)
  const char *name;                            // 1..80 bytes (store deep-copies)
  const char *avatar;                          // avatar hash or "" (store deep-copies)
  const char *token;                           // incoming webhook secret token or "" (store deep-copies)
  const char *creator_name;                    // creator display/username or "" (store deep-copies)
  ccord_id source_guild_id;                    // type 2: followed guild id (0 when none)
  const char *source_guild_name;               // type 2: followed guild name or "" (store deep-copies)
  ccord_id source_channel_id;                  // type 2: followed announcement channel id (0 when none)
  const char *source_channel_name;             // type 2: followed announcement channel name or "" (store deep-copies)
} ccord_webhook;

typedef struct ccord_followed_channel {
  ccord_id source_channel_id;                  // announcement channel being followed
  ccord_id target_channel_id;                  // destination text channel receiving posts
  ccord_id webhook_id;                         // created follower webhook id
} ccord_followed_channel;

typedef enum ccord_app_cmd_perm_type : uint8_t {
  CCORD_APP_PERM_ROLE    = 1,
  CCORD_APP_PERM_USER    = 2,
  CCORD_APP_PERM_CHANNEL = 3,
} ccord_app_cmd_perm_type;

#define CCORD_APP_CMD_MAX_PERMS 32u

typedef struct ccord_app_cmd_perm_rule {
  ccord_id id;                                 // role_id, user_id, or channel_id (guild_id - 1 = all channels)
  uint8_t type;                                // ccord_app_cmd_perm_type (1..3)
  bool permission;                             // true = allow, false = deny
} ccord_app_cmd_perm_rule;

typedef struct ccord_app_cmd_perms {
  ccord_id id;                                 // command_id, or application_id for app-wide defaults
  ccord_id application_id;
  ccord_id guild_id;
  size_t rule_count;
  ccord_app_cmd_perm_rule rules[CCORD_APP_CMD_MAX_PERMS];
} ccord_app_cmd_perms;

// ---- Community: Welcome Screen & Server Onboarding (M4c) ----
#define CCORD_WELCOME_MAX_CHANNELS       5u
#define CCORD_WELCOME_DESC_MAX_BYTES     140u
#define CCORD_WELCOME_CH_DESC_MAX_BYTES  50u

typedef struct ccord_welcome_channel {
  ccord_id channel_id;
  const char *description;                     // 1..50 bytes (store deep-copies)
  ccord_id emoji_id;                           // custom emoji id (0 when unicode or none)
  const char *emoji_name;                      // unicode emoji or custom emoji name ("" when none; store deep-copies)
} ccord_welcome_channel;

typedef struct ccord_welcome_screen {
  bool enabled;
  const char *description;                     // <= 140 bytes ("" when none; store deep-copies)
  const ccord_welcome_channel *channels;       // <= CCORD_WELCOME_MAX_CHANNELS (store deep-copies)
  size_t channel_count;
} ccord_welcome_screen;

typedef enum ccord_onboarding_prompt_type : uint8_t {
  CCORD_ONBOARDING_MULTIPLE_CHOICE = 0,
  CCORD_ONBOARDING_DROPDOWN        = 1,
} ccord_onboarding_prompt_type;

typedef enum ccord_onboarding_mode : uint8_t {
  CCORD_ONBOARDING_DEFAULT  = 0,               // counts default channels only
  CCORD_ONBOARDING_ADVANCED = 1,               // counts default channels and prompt questions
} ccord_onboarding_mode;

#define CCORD_ONBOARDING_MAX_PROMPTS          8u
#define CCORD_ONBOARDING_MAX_OPTIONS          12u
#define CCORD_ONBOARDING_MAX_OPT_CHANNELS     16u
#define CCORD_ONBOARDING_MAX_OPT_ROLES        16u
#define CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS 32u
#define CCORD_ONBOARDING_TITLE_MAX_BYTES      100u
#define CCORD_ONBOARDING_DESC_MAX_BYTES       100u

typedef struct ccord_onboarding_option {
  ccord_id id;
  const char *title;                           // 1..100 bytes (store deep-copies)
  const char *description;                     // <= 100 bytes ("" when none; store deep-copies)
  ccord_id emoji_id;
  const char *emoji_name;                      // unicode or custom emoji name ("" when none; store deep-copies)
  size_t channel_count;
  ccord_id channel_ids[CCORD_ONBOARDING_MAX_OPT_CHANNELS];
  size_t role_count;
  ccord_id role_ids[CCORD_ONBOARDING_MAX_OPT_ROLES];
} ccord_onboarding_option;

typedef struct ccord_onboarding_prompt {
  ccord_id id;
  uint8_t type;                                // ccord_onboarding_prompt_type (0 or 1)
  const char *title;                           // 1..100 bytes (store deep-copies)
  bool single_select;
  bool required;
  bool in_onboarding;
  const ccord_onboarding_option *options;      // 1..CCORD_ONBOARDING_MAX_OPTIONS (store deep-copies)
  size_t option_count;
} ccord_onboarding_prompt;

typedef struct ccord_onboarding {
  ccord_id guild_id;
  bool enabled;
  uint8_t mode;                                // ccord_onboarding_mode (0 or 1)
  size_t default_channel_count;
  ccord_id default_channel_ids[CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS];
  const ccord_onboarding_prompt *prompts;      // <= CCORD_ONBOARDING_MAX_PROMPTS (store deep-copies)
  size_t prompt_count;
} ccord_onboarding;

// A channel permission overwrite: allow/deny bits for one role or one member.
