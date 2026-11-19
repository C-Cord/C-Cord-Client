// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>
typedef uint64_t ccord_id;                       // snowflake-like; 0 = none
#define CCORD_ID_NONE ((ccord_id)0)
#define CCORD_MESSAGE_MAX_BYTES 4000u            // incoming
#define CCORD_SEND_MAX_BYTES 2000u               // outgoing (composer / session_send)
#define CCORD_NAME_MAX_BYTES 100u
// Discord lets an account join at most 200 servers (100 without Nitro). The DM pseudo guild below takes one more slot,
// so every real server fits and the DM Home entry is never squeezed out.
#define CCORD_MAX_REAL_GUILDS 200u
#define CCORD_MAX_GUILDS (CCORD_MAX_REAL_GUILDS + 1u)
#define CCORD_MAX_CHANNELS_PER_GUILD 1000u
#define CCORD_MAX_MEMBERS_PER_GUILD 5000u
#define CCORD_MAX_MESSAGES_PER_CHANNEL 100000u
#define CCORD_MAX_REACTIONS_PER_MESSAGE 48u
#define CCORD_EMOJI_MAX_BYTES 64u
#define CCORD_BIO_MAX_BYTES 512u
#define CCORD_USER_BIO_MAX_BYTES CCORD_BIO_MAX_BYTES
#define CCORD_PRONOUNS_MAX_BYTES 64u
#define CCORD_USER_PRONOUNS_MAX_BYTES CCORD_PRONOUNS_MAX_BYTES
#define CCORD_URL_MAX_BYTES 512u
#define CCORD_MAX_RELATIONSHIPS 5000u
#define CCORD_MAX_DM_RECIPIENTS 10u
#define CCORD_ACTIVITY_TEXT_MAX_BYTES 128u
#define CCORD_USER_NOTE_MAX_BYTES 256u
#define CCORD_MAX_USER_NOTES 2000u
#define CCORD_PROFILE_MAX_CONNECTIONS 16u
#define CCORD_PROFILE_MAX_MUTUAL_GUILDS 32u
#define CCORD_PROFILE_MAX_MUTUAL_FRIENDS 32u
#define CCORD_PROFILE_MAX_BADGES 16u
// Pseudo guild that owns DM/group-DM channels. Real Discord guild ids are
// snowflakes, so the all-ones value cannot collide with a real guild.
#define CCORD_DM_GUILD_ID ((ccord_id)UINT64_MAX)
// (The extra slot in CCORD_MAX_GUILDS is for CCORD_DM_GUILD_ID.)
#define CCORD_DM_GUILD_NAME "Direct Messages"
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
} ccord_role;
// Server-administration lists (Server Settings). Strings are borrowed; the store deep-copies them.
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
  ccord_member_group_kind kind;
  ccord_id role_id;              // CCORD_MEMBER_GROUP_ROLE only
  uint32_t count;
} ccord_member_group;
typedef struct ccord_channel_overwrites {
  ccord_id channel_id;
  const ccord_overwrite *items; size_t count;
} ccord_channel_overwrites;
#define CCORD_MAX_ROLES_PER_GUILD 250u
#define CCORD_MAX_OVERWRITES_PER_CHANNEL 500u
typedef enum ccord_channel_kind : uint8_t { CCORD_CHANNEL_CATEGORY, CCORD_CHANNEL_TEXT, CCORD_CHANNEL_VOICE, CCORD_CHANNEL_STAGE } ccord_channel_kind;
typedef enum ccord_presence : uint8_t { CCORD_PRESENCE_OFFLINE, CCORD_PRESENCE_ONLINE, CCORD_PRESENCE_IDLE, CCORD_PRESENCE_DND } ccord_presence;
// Voice and live-streaming configuration. The encoder is the GPU/codec backend,
// the codec the wire format, and the framework how frames are captured.
typedef enum ccord_stream_encoder : uint8_t {
  CCORD_STREAM_ENCODER_NVENC = 0,     // NVIDIA NVENC (H.264/HEVC/AV1)
  CCORD_STREAM_ENCODER_VAAPI,         // Linux VA-API
  CCORD_STREAM_ENCODER_AMF,           // AMD AMF
  CCORD_STREAM_ENCODER_VULKAN,        // Vulkan video
  CCORD_STREAM_ENCODER_OPENH264,      // software OpenH264
  CCORD_STREAM_ENCODER_X264,          // software libx264 (ultrafast/zerolatency)
  CCORD_STREAM_ENCODER_AUTO,          // probe best native hardware encoder at runtime (default)
  CCORD_STREAM_ENCODER_QSV,           // Intel Quick Sync Video (Windows/Linux)
  CCORD_STREAM_ENCODER_MF,            // Windows Media Foundation hardware H.264
  CCORD_STREAM_ENCODER_VIDEOTOOLBOX,  // macOS Apple VideoToolbox hardware H.264
} ccord_stream_encoder;
typedef enum ccord_stream_codec : uint8_t {
  CCORD_STREAM_CODEC_H264 = 0,
  CCORD_STREAM_CODEC_HEVC,
  CCORD_STREAM_CODEC_AV1,
} ccord_stream_codec;
typedef enum ccord_stream_framework : uint8_t {
  CCORD_STREAM_FRAMEWORK_PIPEWIRE = 0, // Linux xdg-desktop-portal + PipeWire
  CCORD_STREAM_FRAMEWORK_DMABUF,       // Linux DMA-BUF
  CCORD_STREAM_FRAMEWORK_NVFBC,        // Linux NVFBC
  CCORD_STREAM_FRAMEWORK_VULKAN,       // Linux Vulkan
  CCORD_STREAM_FRAMEWORK_X11,          // Linux X11 XShm/XComposite
  CCORD_STREAM_FRAMEWORK_AUTO,         // OS-native automatic capture backend (default)
  CCORD_STREAM_FRAMEWORK_WGC,          // Windows Graphics Capture (Win10+)
  CCORD_STREAM_FRAMEWORK_DXGI,         // Windows DXGI Desktop Duplication
  CCORD_STREAM_FRAMEWORK_GDI,          // Windows GDI compatibility capture
  CCORD_STREAM_FRAMEWORK_SCK,          // macOS ScreenCaptureKit
  CCORD_STREAM_FRAMEWORK_CG,           // macOS CoreGraphics Display Capture
} ccord_stream_framework;
typedef enum ccord_stream_resolution : uint8_t {
  CCORD_STREAM_RES_720P = 0,
  CCORD_STREAM_RES_1080P,
  CCORD_STREAM_RES_1440P,
  CCORD_STREAM_RES_SOURCE,
} ccord_stream_resolution;
typedef enum ccord_stream_fps : uint8_t {
  CCORD_STREAM_FPS_15 = 0,
  CCORD_STREAM_FPS_30,
  CCORD_STREAM_FPS_60,
} ccord_stream_fps;
// The framework value CCORD_STREAM_FRAMEWORK_PIPEWIRE means the xdg-desktop-portal ScreenCast + PipeWire path.
typedef struct ccord_stream_config {
  ccord_stream_encoder encoder;
  ccord_stream_codec codec;
  ccord_stream_framework framework;
  ccord_stream_resolution resolution;
  ccord_stream_fps fps;
} ccord_stream_config;
typedef struct ccord_voice_state {
  ccord_id guild_id, channel_id, user_id;      // channel_id = 0 means disconnected
  bool self_mute, self_deaf, self_video, self_stream;
  bool mute, deaf;                             // server-enforced mute / deafen
  bool suppress;                               // stage audience: mic suppressed until invited to speak
  int64_t request_to_speak_timestamp;          // unix ms of the pending stage ask (0 = none)
  bool omit_suppress, omit_request_to_speak;   // true when partial gateway payload omitted suppress / request_to_speak_timestamp
} ccord_voice_state;
typedef struct ccord_voice_server {
  ccord_id guild_id, channel_id;
  const char *token;                           // voice handshake token; never logged
  const char *endpoint;
} ccord_voice_server;
typedef struct ccord_stream_state {
  ccord_id guild_id, channel_id, user_id;      // user_id = broadcaster
  bool active;
  bool paused;
  ccord_stream_config config;
} ccord_stream_state;
// M5 voice & video state. Call state is for DM/group calls
// ringing[] lists users whose clients still ring; active=false once the call ended.
typedef struct ccord_call_state {
  ccord_id channel_id; char region[64];
  ccord_id ringing[8]; size_t ringing_count; bool active;
} ccord_call_state;
typedef struct ccord_stage_instance {
  ccord_id channel_id, guild_id; char topic[125];
  ccord_id id;                                 // stage instance snowflake (0 = not reported yet)
  uint8_t privacy_level;                       // Discord stage privacy level (2 = GUILD_ONLY; 0 = not reported)
  bool discoverable_disabled;                  // true when the stage is hidden from discovery
  ccord_id guild_scheduled_event_id;           // linked scheduled event (0 = none)
} ccord_stage_instance;
// Guild scheduled events (GUILD_SCHEDULED_EVENT_* gateway + /guilds/{g}/scheduled-events REST). The enums keep
// Discord's wire values; a zero value means "not reported".
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
typedef enum ccord_call_action : uint8_t { CCORD_CALL_JOIN = 0, CCORD_CALL_LEAVE } ccord_call_action;
// Local voice preferences (PTT vs VAD, sensitivity, DSP toggles); never sent as Discord state.
typedef struct ccord_voice_config { bool ptt; uint32_t vad_sensitivity_pct;
                                  bool ns, aec, agc; } ccord_voice_config;
// RTC connection snapshot for the debug panel; state is a ccord_voice_connection_state value.
typedef struct ccord_rtc_stats { int state; uint32_t ssrc; int ping_ms;
                                 size_t jitter_depth; uint64_t packets_lost;
                                 uint32_t bitrate_kbps; uint32_t dave_epoch; } ccord_rtc_stats;
typedef struct ccord_dave_status { bool active; bool verified;
                                   uint16_t protocol_version;
                                   char privacy_code[64]; } ccord_dave_status;
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
  const char *banner_url;                      // CDN URL or "" / nullptr if none
  uint32_t accent_color;                       // 0xRRGGBB (0 if none)
  bool has_accent_color;
  bool has_presence;
  const char *bio;                             // profile bio / about me, "" / nullptr if none
  const char *pronouns;                        // profile pronouns, "" / nullptr if none
  ccord_presence presence; bool is_bot;
  uint8_t avatar_palette;                      // index into theme avatar_0..5 (taken mod 6)
  char clan_tag[16];                           // server identity / clan tag badge (empty when none)
  ccord_id clan_guild_id;
  char clan_badge[64];
  // Custom status (activity type 4 / user_settings.custom_status) and primary rich-presence activity (types 0..3, 5).
  char custom_status_emoji[CCORD_EMOJI_MAX_BYTES + 1];
  char custom_status_text[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  int64_t custom_status_expires_ms;            // unix ms (0 = never / unset)
  bool has_activity;
  uint8_t activity_type;                       // ccord_activity_type (0, 1, 2, 3, 5)
  char activity_name[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  char activity_details[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  char activity_state[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  int64_t activity_start_ms;                   // unix ms (0 = unset)
  int64_t activity_end_ms;                     // unix ms (0 = unset)
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
  size_t badge_count;
  ccord_profile_badge badges[CCORD_PROFILE_MAX_BADGES];  // profile "badges", capped at 16
  const char *avatar_decoration_url;           // user "avatar_decoration_data" asset URL, "" / nullptr if none
  int64_t guild_joined_at_ms;                  // guild_member "joined_at" as unix ms (0 = unknown)
  int64_t premium_since_ms;                    // guild_member "premium_since" as unix ms (0 = unknown / not boosting)
} ccord_user_profile;
typedef struct ccord_user_note {
  ccord_id user_id;
  const char *note;                            // borrowed UTF-8 note text (max CCORD_USER_NOTE_MAX_BYTES)
} ccord_user_note;
// M6 account settings. These are Discord account state, not local
// preferences: the session writes them only from a REST answer or USER_SETTINGS_UPDATE.
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
typedef enum ccord_fetch_state : uint8_t { CCORD_FETCH_IDLE, CCORD_FETCH_LOADING, CCORD_FETCH_READY, CCORD_FETCH_FAILED } ccord_fetch_state;
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
// Guild expressions (custom emoji, stickers, soundboard sounds). Strings are borrowed; the store deep-copies them.
#define CCORD_MAX_EMOJIS_PER_GUILD 1000u
#define CCORD_MAX_STICKERS_PER_GUILD 200u
#define CCORD_MAX_SOUNDBOARD_PER_GUILD 128u
typedef struct ccord_emoji {
  ccord_id id, guild_id;                       // guild_id 0 = unknown/global
  const char *name;                            // without colons, e.g. "blobwave"
  bool animated, available;
  ccord_id user_id;                            // uploader user id (0 when omitted by gateway)
  const char *user_name;                       // uploader display/username ("" / nullptr when unknown)
} ccord_emoji;
typedef enum ccord_sticker_format : uint8_t {
  CCORD_STICKER_PNG = 1, CCORD_STICKER_APNG = 2, CCORD_STICKER_LOTTIE = 3, CCORD_STICKER_GIF = 4
} ccord_sticker_format;
typedef struct ccord_sticker {
  ccord_id id, guild_id, pack_id;              // guild_id 0 = standard sticker (pack_id set)
  const char *name, *description, *tags;
  ccord_sticker_format format;
  bool available;
  ccord_id user_id;                            // uploader user id (0 when omitted by gateway)
  const char *user_name;                       // uploader display/username ("" / nullptr when unknown)
} ccord_sticker;
typedef struct ccord_soundboard_sound {
  ccord_id id, guild_id;                       // guild_id 0 = Discord default sound
  const char *name;
  uint8_t volume_pct;                          // 0..100 (Discord 0.0..1.0 * 100)
  ccord_id emoji_id;                           // custom emoji id (0 when none / unicode)
  const char *emoji_name;                      // unicode emoji or custom emoji name ("" / nullptr when none)
  ccord_id user_id;                            // uploader user id (0 when unknown)
  const char *user_name;                       // uploader display/username ("" / nullptr when unknown)
  bool available;
} ccord_soundboard_sound;
// Per-boost-tier guild expression slot limits (tier 0..3+; emoji limit applies separately to static and animated).
[[nodiscard]] uint32_t ccord_guild_emoji_slot_limit(uint8_t premium_tier);
[[nodiscard]] uint32_t ccord_guild_sticker_slot_limit(uint8_t premium_tier);
[[nodiscard]] uint32_t ccord_guild_soundboard_slot_limit(uint8_t premium_tier);
typedef struct ccord_channel {
  ccord_id id, guild_id, parent_id;            // parent_id = category id, parent text channel id (thread), or 0
  ccord_channel_kind kind; const char *name; const char *topic;   // topic "" if none
  int32_t position;                            // Discord display position within guild/category
  ccord_id last_message_id;                    // most recent message snowflake (0 if none)
  ccord_id last_read_id;                       // newest message the user has read (acknowledged); 0 = not known
  uint32_t unread_count;                       // messages since the last ack
  uint32_t mention_count;                      // unread messages that mention self
  union {
    ccord_id user_id;                          // primary recipient user id for DM channels (0 if none)
    ccord_id recipient_id;
  };
  union {
    const char *icon_url;                      // DM recipient or channel icon URL ("" / nullptr if none)
    const char *avatar_url;
  };
  // Raw Discord channel type (0 text, 1 DM, 2 voice, 3 group DM, 4 category, 5 announcement,
  // 10/11/12 threads, 13 stage, 15 forum, 16 media). Zero-initialised channels (mock data) read as
  // GUILD_TEXT, so use `kind` for the text/voice/category split and this field only for
  // forum / group-DM / thread specifics. CCORD_DISCORD_CHANNEL_UNKNOWN marks synthesized channels.
  uint8_t discord_type;
  uint16_t recipient_count;                    // DMs / group DMs: recipients excluding self (0 = unknown)
  // DM / group DM recipients excluding self, first recipient_count (capped) entries valid. Inline so
  // the store's struct copies stay deep; Discord caps group DMs at 10 participants including self.
  ccord_id recipient_ids[CCORD_MAX_DM_RECIPIENTS];
  ccord_id owner_id;                           // group DM owner user id (0 when unset)
  bool is_message_request;                     // DM / group DM awaiting message-request consent
  bool is_spam;                                // message request flagged as spam
  uint32_t rate_limit_per_user;                // slowmode cooldown in seconds (0 = off)
  bool nsfw;                                   // age-restricted channel
  uint32_t bitrate;                            // voice / stage bitrate in bps (0 = unset)
  uint32_t user_limit;                         // voice / stage max participants (0 = unlimited)
  uint32_t default_auto_archive_duration;      // default thread auto-archive minutes (60/1440/4320/10080, 0 = unset)
  bool thread_archived;                        // thread_metadata.archived
  bool thread_locked;                          // thread_metadata.locked
  bool thread_joined;                          // signed-in user is a member of this thread
  bool thread_has_member;                      // payload carried explicit thread membership state
  uint32_t thread_auto_archive_mins;           // thread_metadata.auto_archive_duration (minutes; 0 = unset)
  uint32_t thread_message_count;               // approximate thread message count (0 = unknown)
} ccord_channel;
#define CCORD_DISCORD_CHANNEL_UNKNOWN 0xFFu
#define CCORD_DISCORD_CHANNEL_GUILD_TEXT 0u
#define CCORD_DISCORD_CHANNEL_DM 1u
#define CCORD_DISCORD_CHANNEL_GUILD_VOICE 2u
#define CCORD_DISCORD_CHANNEL_GROUP_DM 3u
#define CCORD_DISCORD_CHANNEL_GUILD_CATEGORY 4u
#define CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT 5u
#define CCORD_DISCORD_CHANNEL_ANNOUNCEMENT_THREAD 10u
#define CCORD_DISCORD_CHANNEL_PUBLIC_THREAD 11u
#define CCORD_DISCORD_CHANNEL_PRIVATE_THREAD 12u
#define CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE 13u
#define CCORD_DISCORD_CHANNEL_GUILD_FORUM 15u
#define CCORD_DISCORD_CHANNEL_GUILD_MEDIA 16u
static inline bool ccord_discord_type_is_thread(uint8_t t)
{
  return t == CCORD_DISCORD_CHANNEL_ANNOUNCEMENT_THREAD || t == CCORD_DISCORD_CHANNEL_PUBLIC_THREAD ||
         t == CCORD_DISCORD_CHANNEL_PRIVATE_THREAD;
}
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
} ccord_member;
typedef struct ccord_reaction {
  const char *emoji;                           // UTF-8 emoji name (e.g. "👍" or "name:id")
  ccord_id emoji_id;                           // custom emoji snowflake id (0 = standard Unicode emoji)
  uint32_t count;                              // users who reacted with this emoji
  uint32_t burst_count;                        // super/burst reaction count (count_details.burst)
  bool self_reacted;                           // the signed-in user reacted with this emoji
  bool me_burst;                               // the signed-in user super-reacted with this emoji
  bool animated;                               // true when custom emoji is animated
} ccord_reaction;
struct ccord_message_rich;                    // ccord/model/rich.h
typedef struct ccord_message {
  ccord_id id, channel_id, author_id;          // id increases with time within a channel
  int64_t timestamp_ms;                        // unix ms
  const char *content;
  int64_t edited_timestamp_ms;                 // 0 = never edited
  uint32_t reaction_count;                     // total reactions on the message
  bool self_reacted;                           // the signed-in user has reacted
  const ccord_reaction *reactions;             // per-emoji breakdown, owned by whoever owns the message
  size_t reaction_kind_count;                  // entries in reactions; 0 means only the aggregate is known
  bool edited;                                 // true once an update was applied
  ccord_id reply_to_id;                        // referenced message id; 0 = not a reply
  bool pinned;                                 // true while pinned in its channel
  // Attachments, embeds, stickers, mentions, reply preview, forward, poll, thread and the message type. nullptr for a
  // plain text message. Borrowed like the other pointers here: the store takes its own deep copy.
  const struct ccord_message_rich *rich;
  // M0-3 local send lifecycle. nonce is the client-generated dedup key Discord echoes back on MESSAGE_CREATE
  // (empty when the message carries none); send_state is one of CCORD_MSG_SEND_* below. Only local echoes are
  // ever pending/failed; everything the server reports is confirmed.
  char nonce[32];
  uint8_t send_state;
  bool tts;                                      // true when the message was sent with text-to-speech
  bool omit_content;                             // partial MESSAGE_UPDATE omitted "content"
  bool has_pinned;                               // partial MESSAGE_UPDATE carried explicit "pinned"
  bool omit_tts;                                 // partial MESSAGE_UPDATE omitted "tts"
} ccord_message;
#define CCORD_MSG_SEND_CONFIRMED 0u   // the server reported the message
#define CCORD_MSG_SEND_PENDING 1u     // local echo, waiting for the server's nonce-matched MESSAGE_CREATE
#define CCORD_MSG_SEND_FAILED 2u      // local echo the server refused; retry resubmits the same nonce
// Everything the server tells the client at connect time. Arrays are borrowed; channels/members keep display order per guild.
typedef struct ccord_snapshot {
  ccord_id self_id;
  const ccord_user *users; size_t user_count;
  const ccord_guild *guilds; size_t guild_count;
  const ccord_channel *channels; size_t channel_count;
  const ccord_member *members; size_t member_count;
  const ccord_voice_state *voice_states; size_t voice_state_count;
  const ccord_relationship *relationships; size_t relationship_count;
  const ccord_user_note *notes; size_t note_count;
} ccord_snapshot;
// Validation of external data: non-zero unique IDs, non-null valid-UTF-8 strings within limits,
// referential integrity (channel->guild/parent, member->guild/user, self_id in users), caps above.
[[nodiscard]] ccord_status ccord_snapshot_validate(const ccord_snapshot *s, ccord_error *err);
[[nodiscard]] ccord_status ccord_message_validate(const ccord_message *m, ccord_error *err);
[[nodiscard]] ccord_status ccord_user_profile_validate(const ccord_user_profile *p, ccord_error *err);
[[nodiscard]] bool ccord_id_parse(const char *s, size_t len, ccord_id *out);   // decimal, 1..20 digits, no overflow, non-zero
int ccord_id_format(ccord_id id, char out[static 21]);                        // returns length
