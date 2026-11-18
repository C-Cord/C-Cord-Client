// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Server-administration requests: the exact Discord REST call behind each admin action, built and validated in one
// place. Pure logic (no I/O, no clock, no globals), so every endpoint, body and limit is unit-testable.
//
// Callers (the session, the UI) never assemble paths or JSON. They build a ccord_admin_request with one of the
// functions below and hand it to the transport (`ccord_transport_admin_action`), which sends it through the request
// governor at user priority. Nothing here changes local state: the store only changes when Discord answers.
#pragma once
#include <stdio.h>
#include <string.h>

#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>
#include <ccord/model/model.h>
#include <ccord/model/forum.h>
#include <ccord/model/notify.h>

typedef enum ccord_admin_op : uint8_t {
  CCORD_ADMIN_NONE = 0,
  CCORD_ADMIN_CREATE_CHANNEL,   // POST   /guilds/{g}/channels
  CCORD_ADMIN_EDIT_CHANNEL,     // PATCH  /channels/{c}
  CCORD_ADMIN_DELETE_CHANNEL,   // DELETE /channels/{c}
  CCORD_ADMIN_CREATE_THREAD,    // POST   /channels/{c}/threads
  CCORD_ADMIN_EDIT_GUILD,       // PATCH  /guilds/{g}
  CCORD_ADMIN_SET_NICK,         // PATCH  /guilds/{g}/members/{u}  (or /members/@me for yourself)
  CCORD_ADMIN_TIMEOUT,          // PATCH  /guilds/{g}/members/{u}  {communication_disabled_until}
  CCORD_ADMIN_KICK,             // DELETE /guilds/{g}/members/{u}
  CCORD_ADMIN_BAN,              // PUT    /guilds/{g}/bans/{u}
  CCORD_ADMIN_CLOSE_DM,         // DELETE /channels/{c}  (hides a DM; it is not a server channel)
  CCORD_ADMIN_CREATE_ROLE,      // POST   /guilds/{g}/roles
  CCORD_ADMIN_EDIT_ROLE,        // PATCH  /guilds/{g}/roles/{r}
  CCORD_ADMIN_DELETE_ROLE,      // DELETE /guilds/{g}/roles/{r}
  CCORD_ADMIN_REORDER_ROLES,    // PATCH  /guilds/{g}/roles  [{id, position}]
  CCORD_ADMIN_ADD_MEMBER_ROLE,  // PUT    /guilds/{g}/members/{u}/roles/{r}
  CCORD_ADMIN_REMOVE_MEMBER_ROLE, // DELETE /guilds/{g}/members/{u}/roles/{r}
  CCORD_ADMIN_UNBAN,            // DELETE /guilds/{g}/bans/{u}
  CCORD_ADMIN_CREATE_INVITE,    // POST   /channels/{c}/invites
  CCORD_ADMIN_REVOKE_INVITE,    // DELETE /invites/{code}
  CCORD_ADMIN_LIST_BANS,        // GET    /guilds/{g}/bans
  CCORD_ADMIN_LIST_INVITES,     // GET    /guilds/{g}/invites
  CCORD_ADMIN_LIST_AUDIT_LOG,   // GET    /guilds/{g}/audit-logs
  CCORD_ADMIN_SET_NOTIFY,       // PATCH  /users/@me/guilds/{g|@me}/settings  (mute, notification level; not an admin power)
  CCORD_ADMIN_CREATE_THREAD_MSG,// POST   /channels/{c}/messages/{m}/threads
  CCORD_ADMIN_ARCHIVE_THREAD,   // PATCH  /channels/{c}  {"archived":true|false}
  CCORD_ADMIN_JOIN_THREAD,      // PUT    /channels/{c}/thread-members/@me
  CCORD_ADMIN_LEAVE_THREAD,     // DELETE /channels/{c}/thread-members/@me
  CCORD_ADMIN_SET_GUILD_FOLDERS,// PATCH  /users/@me/settings  {"guild_folders":[...]}
  CCORD_ADMIN_REORDER_CHANNELS, // PATCH  /guilds/{g}/channels  [{id, position, parent_id?}]
  CCORD_ADMIN_CREATE_GUILD,     // POST   /guilds  {"name":"..."}
  CCORD_ADMIN_PREVIEW_INVITE,   // GET    /invites/{code}?with_counts=true
  CCORD_ADMIN_ACCEPT_INVITE,    // POST   /invites/{code}
  CCORD_ADMIN_CREATE_GROUP_DM,  // POST   /users/@me/channels  {"recipients":[...]}
  CCORD_ADMIN_ADD_DM_RECIPIENT, // PUT    /channels/{c}/recipients/{u}
  CCORD_ADMIN_REMOVE_DM_RECIPIENT, // DELETE /channels/{c}/recipients/{u}
  CCORD_ADMIN_EDIT_GROUP_DM,    // PATCH  /channels/{c}  {"name":...,"icon":...}
  CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST, // PUT /channels/{c}/recipients/@me  {"consent_status":2}
  CCORD_ADMIN_REJECT_MESSAGE_REQUEST, // DELETE /channels/{c}
  CCORD_ADMIN_SET_USER_NOTE,    // PUT    /users/@me/notes/{u}  {"note":"..."}
  CCORD_ADMIN_SET_CUSTOM_STATUS,// PATCH  /users/@me/settings  {"custom_status":{...}|null}
  CCORD_ADMIN_SET_INCIDENT_ACTIONS, // PUT /guilds/{g}/incident-actions  {"invites_disabled_until":...,"dms_disabled_until":...}
  CCORD_ADMIN_SET_VANITY_URL,   // PATCH  /guilds/{g}/vanity-url  {"code":"..."|null}
  CCORD_ADMIN_LEAVE_GUILD,      // DELETE /users/@me/guilds/{g}
  CCORD_ADMIN_DELETE_GUILD,     // DELETE /guilds/{g}
  CCORD_ADMIN_TRANSFER_OWNERSHIP, // PATCH /guilds/{g}  {"owner_id":"..."}
  CCORD_ADMIN_LIST_EMOJIS,      // GET    /guilds/{g}/emojis
  CCORD_ADMIN_CREATE_EMOJI,     // POST   /guilds/{g}/emojis
  CCORD_ADMIN_EDIT_EMOJI,       // PATCH  /guilds/{g}/emojis/{e}
  CCORD_ADMIN_DELETE_EMOJI,     // DELETE /guilds/{g}/emojis/{e}
  CCORD_ADMIN_LIST_STICKERS,    // GET    /guilds/{g}/stickers
  CCORD_ADMIN_CREATE_STICKER,   // POST   /guilds/{g}/stickers  (multipart/form-data)
  CCORD_ADMIN_EDIT_STICKER,     // PATCH  /guilds/{g}/stickers/{s}
  CCORD_ADMIN_DELETE_STICKER,   // DELETE /guilds/{g}/stickers/{s}
  CCORD_ADMIN_LIST_SOUNDBOARD,  // GET    /guilds/{g}/soundboard-sounds
  CCORD_ADMIN_CREATE_SOUNDBOARD,// POST   /guilds/{g}/soundboard-sounds
  CCORD_ADMIN_EDIT_SOUNDBOARD,  // PATCH  /guilds/{g}/soundboard-sounds/{s}
  CCORD_ADMIN_DELETE_SOUNDBOARD,// DELETE /guilds/{g}/soundboard-sounds/{s}
  CCORD_ADMIN_PREVIEW_PRUNE,    // GET    /guilds/{g}/prune?days={d}[&include_roles=...]
  CCORD_ADMIN_EXECUTE_PRUNE,    // POST   /guilds/{g}/prune
  CCORD_ADMIN_LIST_AUTOMOD_RULES,   // GET    /guilds/{g}/auto-moderation/rules
  CCORD_ADMIN_CREATE_AUTOMOD_RULE,  // POST   /guilds/{g}/auto-moderation/rules
  CCORD_ADMIN_EDIT_AUTOMOD_RULE,    // PATCH  /guilds/{g}/auto-moderation/rules/{r}
  CCORD_ADMIN_DELETE_AUTOMOD_RULE,  // DELETE /guilds/{g}/auto-moderation/rules/{r}
  CCORD_ADMIN_LIST_WEBHOOKS,        // GET    /guilds/{g}/webhooks
  CCORD_ADMIN_CREATE_WEBHOOK,       // POST   /channels/{c}/webhooks
  CCORD_ADMIN_EDIT_WEBHOOK,         // PATCH  /webhooks/{w}
  CCORD_ADMIN_DELETE_WEBHOOK,       // DELETE /webhooks/{w}
  CCORD_ADMIN_FOLLOW_CHANNEL,       // POST   /channels/{c}/followers
  CCORD_ADMIN_LIST_APP_CMD_PERMS,   // GET    /guilds/{g}/application-command-permissions
  CCORD_ADMIN_GET_WELCOME_SCREEN,   // GET    /guilds/{g}/welcome-screen
  CCORD_ADMIN_EDIT_WELCOME_SCREEN,  // PATCH  /guilds/{g}/welcome-screen
  CCORD_ADMIN_GET_ONBOARDING,       // GET    /guilds/{g}/onboarding
  CCORD_ADMIN_PUT_ONBOARDING,       // PUT    /guilds/{g}/onboarding
  CCORD_ADMIN_PUT_CHANNEL_OVERWRITE,    // PUT    /channels/{c}/permissions/{overwrite_id}
  CCORD_ADMIN_DELETE_CHANNEL_OVERWRITE, // DELETE /channels/{c}/permissions/{overwrite_id}
  CCORD_ADMIN_LIST_CHANNEL_INVITES,     // GET    /channels/{c}/invites
  CCORD_ADMIN_CLONE_CHANNEL,            // POST   /guilds/{g}/channels  (copying config + permission_overwrites[])
  CCORD_ADMIN_SET_VOICE_MUTE,           // PATCH  /guilds/{g}/members/{u}  {"mute":true|false}
  CCORD_ADMIN_SET_VOICE_DEAF,           // PATCH  /guilds/{g}/members/{u}  {"deaf":true|false}
  CCORD_ADMIN_MOVE_VOICE_MEMBER,        // PATCH  /guilds/{g}/members/{u}  {"channel_id":"..."|null}
  CCORD_ADMIN_VOICE_STATUS,             // PUT    /channels/{c}/voice-status  {"status":"..."|null}
  CCORD_ADMIN_STAGE_REQUEST_SPEAK,      // PATCH  /guilds/{g}/voice-states/@me  {channel_id, request_to_speak_timestamp}
  CCORD_ADMIN_STAGE_SET_SPEAKER,        // PATCH  /guilds/{g}/voice-states/{u}  {channel_id, suppress}
  CCORD_ADMIN_STAGE_TOPIC,              // PATCH  /stage-instances/{channel_id}  {"topic":"..."|null}
  CCORD_ADMIN_USER_SETTINGS_PATCH,      // PATCH  /users/@me/settings  (partial; unchanged fields omitted)
  CCORD_ADMIN_AUTH_SESSIONS_LIST,       // GET    /auth/sessions
  CCORD_ADMIN_AUTH_SESSIONS_LOGOUT,     // POST   /auth/sessions/logout  {"session_id_hashes":[...]}
  CCORD_ADMIN_CONNECTIONS_LIST,         // GET    /users/@me/connections
  CCORD_ADMIN_CONNECTION_EDIT,          // PATCH  /users/@me/connections/{name}/{id}
  CCORD_ADMIN_CONNECTION_DELETE,        // DELETE /users/@me/connections/{name}/{id}
  CCORD_ADMIN_CALL_STOP_RINGING,        // POST   /channels/{c}/call/stop-ringing  (decline an incoming DM/group call)
  CCORD_ADMIN_STAGE_CREATE,             // POST   /stage-instances  {channel_id, topic, privacy_level}
  CCORD_ADMIN_STAGE_DELETE,             // DELETE /stage-instances/{channel_id}
  CCORD_ADMIN_EVENT_LIST,               // GET    /guilds/{g}/scheduled-events?with_user_count=true
  CCORD_ADMIN_EVENT_CREATE,             // POST   /guilds/{g}/scheduled-events
  CCORD_ADMIN_EVENT_MODIFY,             // PATCH  /guilds/{g}/scheduled-events/{e}
  CCORD_ADMIN_EVENT_DELETE,             // DELETE /guilds/{g}/scheduled-events/{e}
  CCORD_ADMIN_EVENT_RSVP_ADD,           // PUT    /guilds/{g}/scheduled-events/{e}/users/@me
  CCORD_ADMIN_EVENT_RSVP_REMOVE,        // DELETE /guilds/{g}/scheduled-events/{e}/users/@me
  CCORD_ADMIN_GET_MEMBER_VERIFICATION,  // GET    /guilds/{g}/member-verification?with_guild=true
  CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION, // PUT  /guilds/{g}/requests/@me  {form_fields:[...]}
  CCORD_ADMIN_ACTIVITY_SHELF,           // GET    /activities/shelf?guild_id={g}
  CCORD_ADMIN_ACTIVITY_LAUNCH,          // POST   /activities/{c}/{app}  {"session_id":"..."}  (204)
  CCORD_ADMIN_ACTIVITY_LEAVE,           // POST   /applications/{app}/activities/{location}/instances/{instance}/leave
  CCORD_ADMIN_GET_GUILD_PROFILE,        // GET    /guilds/{g}/profile
  CCORD_ADMIN_OP_COUNT
} ccord_admin_op;

typedef enum ccord_admin_method : uint8_t {
  CCORD_ADMIN_POST, CCORD_ADMIN_PATCH, CCORD_ADMIN_PUT, CCORD_ADMIN_DELETE, CCORD_ADMIN_GET
} ccord_admin_method;

// Limits Discord enforces. Checking them here means an invalid request never costs an API call (and never counts
// toward the invalid-request budget).
#define CCORD_ADMIN_CHANNEL_NAME_MAX 100u
#define CCORD_ADMIN_GUILD_NAME_MIN 2u
#define CCORD_ADMIN_GUILD_NAME_MAX 100u
#define CCORD_ADMIN_NICK_MAX 32u
#define CCORD_ADMIN_TOPIC_MAX 1024u
#define CCORD_ADMIN_SLOWMODE_MAX_S 21600
#define CCORD_ADMIN_TIMEOUT_MAX_MS (28LL * 24 * 60 * 60 * 1000)
#define CCORD_ADMIN_BAN_DELETE_MAX_S 604800   // 7 days
#define CCORD_ADMIN_REASON_MAX 512u           // audit-log reason, in bytes

#define CCORD_ADMIN_ROLE_NAME_MAX 100u
#define CCORD_ADMIN_ROLE_REORDER_MAX 40u      // entries in one reorder call (each is about 40 bytes of body)
#define CCORD_ADMIN_CHANNEL_REORDER_MAX 64u   // entries in one channel reorder call
#define CCORD_ADMIN_FOLDER_ITEMS_MAX CCORD_MAX_REAL_GUILDS
#define CCORD_ADMIN_INVITE_MAX_AGE_S 604800   // 7 days
#define CCORD_ADMIN_INVITE_MAX_USES 100u
#define CCORD_ADMIN_INVITE_CODE_MAX 32u
#define CCORD_ADMIN_LIST_BANS_LIMIT 200u
#define CCORD_ADMIN_AUDIT_LIMIT 50u
#define CCORD_ADMIN_MUTE_MAX_MS (366LL * 24 * 60 * 60 * 1000)   // longest timed mute
#define CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX 9u                  // + self = 10 max participants
#define CCORD_ADMIN_CUSTOM_STATUS_MAX 128u                      // characters

#define CCORD_ADMIN_PATH_MAX 512u
#define CCORD_ADMIN_BODY_MAX 65536u
#define CCORD_ADMIN_PRUNE_DAYS_MIN 1u
#define CCORD_ADMIN_PRUNE_DAYS_MAX 30u
#define CCORD_ADMIN_INCIDENT_MAX_MS (24LL * 60 * 60 * 1000)   // Discord caps raid-protection pauses at 24 h
#define CCORD_ADMIN_EMOJI_NAME_MIN 2u
#define CCORD_ADMIN_EMOJI_NAME_MAX 32u
#define CCORD_ADMIN_STICKER_NAME_MIN 2u
#define CCORD_ADMIN_STICKER_NAME_MAX 30u
#define CCORD_ADMIN_STICKER_DESC_MAX 100u
#define CCORD_ADMIN_STICKER_TAGS_MAX 200u
#define CCORD_ADMIN_SOUNDBOARD_NAME_MIN 2u
#define CCORD_ADMIN_SOUNDBOARD_NAME_MAX 32u
#define CCORD_ADMIN_BITRATE_MIN 8000
#define CCORD_ADMIN_BITRATE_MAX 384000
#define CCORD_ADMIN_USER_LIMIT_MAX 99
#define CCORD_ADMIN_FORUM_TAG_NAME_MAX 20u
#define CCORD_ADMIN_CLONE_OVERWRITES_MAX 100u
#define CCORD_ADMIN_VOICE_STATUS_MAX 500u     // characters in a voice-channel status
#define CCORD_ADMIN_STAGE_TOPIC_MAX 120u      // characters in a stage topic
#define CCORD_ADMIN_SESSION_LOGOUT_MAX 8u     // session hashes in one logout request
#define CCORD_ADMIN_CONNECTION_NAME_MAX 31u   // connection service type (fits connection_name[32])
#define CCORD_ADMIN_CONNECTION_ID_MAX 63u     // connection account id (fits connection_id[64])
#define CCORD_ADMIN_EVENT_NAME_MAX 100u       // characters in a scheduled-event name
#define CCORD_ADMIN_EVENT_DESC_MAX 511u       // bytes: fits ccord_scheduled_event.description[512]
#define CCORD_ADMIN_EVENT_LOCATION_MAX 100u   // characters in an EXTERNAL event's location
#define CCORD_ADMIN_ACTIVITY_SESSION_MAX 128u // bytes in the gateway session id sent when launching an activity

typedef struct ccord_admin_request {
  ccord_admin_op op;
  ccord_admin_method method;
  char path[CCORD_ADMIN_PATH_MAX];   // relative API path, e.g. "/guilds/123/members/456"
  char body[CCORD_ADMIN_BODY_MAX];   // compact JSON, "" when the call has no body
  char reason[CCORD_ADMIN_REASON_MAX + 1];   // X-Audit-Log-Reason, "" for none
  // What the request is about. The live transport uses these to report a failure; a simulated server (the mock
  // transport) uses them to apply the change.
  ccord_id guild_id;                 // 0 / CCORD_DM_GUILD_ID for a DM
  ccord_id channel_id;
  ccord_id user_id;
  ccord_id parent_id;                // CREATE_CHANNEL: the category it goes into (0 = none)
  uint8_t channel_type;              // CREATE_CHANNEL: the Discord channel type
  int64_t until_ms;                  // TIMEOUT: when it ends, unix ms (0 = lifted)
  ccord_id role_id;                  // *_ROLE, *_MEMBER_ROLE: the role
  int64_t role_permissions;          // CREATE/EDIT_ROLE: -1 = unchanged
  int32_t role_color;                // CREATE/EDIT_ROLE: -1 = unchanged
  int8_t role_hoist, role_mentionable;   // CREATE/EDIT_ROLE: -1 = unchanged
  size_t reorder_count;              // REORDER_ROLES / REORDER_CHANNELS: the (id, position[, parent]) entries
  ccord_id reorder_ids[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  int32_t reorder_positions[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  ccord_id reorder_parents[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  uint8_t reorder_has_parent[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  size_t folder_item_count;          // SET_GUILD_FOLDERS
  ccord_guild_folder_item folder_items[CCORD_ADMIN_FOLDER_ITEMS_MAX];
  char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];   // REVOKE_INVITE: the invite code
  uint32_t invite_max_age_s, invite_max_uses;   // CREATE_INVITE
  bool invite_temporary;
  uint16_t audit_action;             // LIST_AUDIT_LOG: action filter (0 = all); user_id is the actor filter
  // SET_NOTIFY (guild_id is the server, CCORD_DM_GUILD_ID for direct messages; channel_id 0 = the server row itself;
  // until_ms is the end of a timed mute, 0 = until turned back on). -1 = the field is not part of the request.
  int8_t notify_level, notify_muted, notify_suppress_everyone, notify_suppress_roles;
  char name[CCORD_NAME_MAX_BYTES + 1];   // CREATE_*, EDIT_*: the new name ("" = unchanged)
  char topic[CCORD_ADMIN_TOPIC_MAX + 1]; // EDIT_CHANNEL: the new topic
  bool has_topic;
  bool has_slowmode;
  uint32_t slowmode_s;
  bool has_nsfw;
  bool nsfw;
  ccord_id message_id;                   // CREATE_THREAD_MSG: starter message id
  bool thread_archived;                  // ARCHIVE_THREAD: target archived state
  size_t dm_recipient_count;             // CREATE_GROUP_DM
  ccord_id dm_recipient_ids[CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX];
  bool has_group_dm_name;                // EDIT_GROUP_DM
  bool has_group_dm_icon;                // EDIT_GROUP_DM
  bool clear_group_dm_icon;              // EDIT_GROUP_DM: icon explicitly cleared to null
  char note[CCORD_USER_NOTE_MAX_BYTES + 1]; // SET_USER_NOTE
  char custom_status_emoji[CCORD_EMOJI_MAX_BYTES + 1]; // SET_CUSTOM_STATUS
  char custom_status_text[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1]; // SET_CUSTOM_STATUS
  ccord_id custom_status_emoji_id;       // SET_CUSTOM_STATUS (0 = Unicode or none)
  bool custom_status_clear;              // SET_CUSTOM_STATUS: true when clearing to null
  // EDIT_GUILD extended fields + SET_INCIDENT_ACTIONS
  bool has_guild_name;
  bool has_guild_icon, clear_guild_icon;
  bool has_guild_banner, clear_guild_banner;
  bool has_system_channel;
  ccord_id system_channel_id;
  int32_t guild_system_flags;            // -1 = unchanged
  int8_t guild_default_notify;           // -1 = unchanged
  bool has_afk_channel;
  ccord_id afk_channel_id;
  int32_t guild_afk_timeout;             // -1 = unchanged
  int8_t guild_verification_level;       // -1 = unchanged
  int8_t guild_explicit_filter;          // -1 = unchanged
  int64_t invites_disabled_until_ms;     // SET_INCIDENT_ACTIONS (0 = unpaused)
  int64_t dms_disabled_until_ms;         // SET_INCIDENT_ACTIONS (0 = unpaused)
  // Binary / multipart body override + guild expressions (M4b)
  size_t body_len;                       // >0 when `body` holds exact binary bytes (e.g. multipart/form-data)
  char content_type[96];                 // "" = application/json; else explicit Content-Type header
  ccord_id expr_id;                      // emoji_id / sticker_id / sound_id
  bool emoji_animated;                   // CREATE_EMOJI: true when image_data_uri is image/gif
  uint8_t sticker_format;                // CREATE_STICKER: ccord_sticker_format
  bool has_sticker_desc;                 // CREATE/EDIT_STICKER
  bool has_sticker_tags;                 // CREATE/EDIT_STICKER
  char sticker_desc[257];
  char sticker_tags[204];
  int16_t sound_volume_pct;              // CREATE/EDIT_SOUNDBOARD: -1 = unchanged / default, else 0..100
  bool has_sound_emoji;                  // CREATE/EDIT_SOUNDBOARD
  ccord_id sound_emoji_id;
  char sound_emoji_name[CCORD_EMOJI_MAX_BYTES + 1];
  // Prune (M4c)
  uint8_t prune_days;                    // PREVIEW/EXECUTE_PRUNE: 1..30
  bool prune_compute_count;              // EXECUTE_PRUNE
  size_t prune_role_count;
  ccord_id prune_role_ids[CCORD_PRUNE_MAX_ROLES];
  // Integrations & Community (M4c)
  bool has_community, feature_community; // EDIT_GUILD: toggle COMMUNITY in features[]
  bool has_rules_channel;                // EDIT_GUILD: send rules_channel_id (0 = null)
  ccord_id rules_channel_id;
  bool has_public_updates_channel;       // EDIT_GUILD: send public_updates_channel_id (0 = null)
  ccord_id public_updates_channel_id;
  ccord_id target_channel_id;            // FOLLOW_CHANNEL: webhook_channel_id; EDIT_WEBHOOK / MOVE_VOICE_MEMBER: channel_id
  bool has_webhook_avatar, clear_webhook_avatar;
  // Channel settings, overwrites, clone & voice moderation (M4d)
  bool has_bitrate;
  uint32_t bitrate;
  bool has_user_limit;
  uint32_t user_limit;
  bool has_auto_archive;
  uint32_t default_auto_archive_duration;
  bool has_available_tags;
  uint8_t available_tag_count;
  ccord_forum_tag available_tags[CCORD_FORUM_MAX_TAGS];
  ccord_overwrite overwrite;             // PUT/DELETE_CHANNEL_OVERWRITE (overwrite.id is overwrite_id)
  size_t clone_overwrite_count;          // CLONE_CHANNEL
  ccord_overwrite clone_overwrites[CCORD_ADMIN_CLONE_OVERWRITES_MAX];
  bool voice_muted;                      // SET_VOICE_MUTE
  bool voice_deafened;                   // SET_VOICE_DEAF
  // Voice status & stage moderation (M5). STAGE_TOPIC reuses topic[]/has_topic ("" = clear to null).
  char voice_status[CCORD_ADMIN_VOICE_STATUS_MAX * 4u + 1u]; // VOICE_STATUS ("" = cleared; up to 500 4-byte UTF-8 chars)
  int64_t request_to_speak_ms;           // STAGE_REQUEST_SPEAK: unix ms (0 = cancel the ask)
  bool stage_speaker;                    // STAGE_SET_SPEAKER: true = invite to speak (suppress=false)
  // Account settings, devices & connections
  ccord_user_settings_patch settings_patch;   // USER_SETTINGS_PATCH: -1 / "" = unchanged
  char connection_name[32];                   // CONNECTION_EDIT/DELETE: the service type
  char connection_id[64];                     // CONNECTION_EDIT/DELETE: the account id
  char session_hashes[CCORD_ADMIN_SESSION_LOGOUT_MAX][64];   // AUTH_SESSIONS_LOGOUT
  size_t session_hash_count;
  ccord_connection_edit connection_edit;      // CONNECTION_EDIT: -1 = unchanged
  // Stage lifecycle & guild scheduled events. EVENT_CREATE: `event` is the event to create (id / creator_id /
  // user_count / me_rsvped are ignored). EVENT_MODIFY: `event` holds the new values of the fields flagged in
  // event_has (CCORD_ADMIN_EVENT_HAS_*); the body JSON is authoritative on the wire and also carries the cover
  // image, which is not stored here.
  ccord_scheduled_event event;
  uint32_t event_has;
  ccord_id event_id;                          // EVENT_MODIFY / EVENT_DELETE / EVENT_RSVP_*: the event
  // Embedded activities. The mock applies ACTIVITY_LAUNCH / ACTIVITY_LEAVE from these typed fields (it never parses
  // path[]), so a builder fills them alongside the REST path/body.
  ccord_id application_id;                    // ACTIVITY_LAUNCH / ACTIVITY_LEAVE: the app
  ccord_id location_id;                       // ACTIVITY_LEAVE: the location path segment (the voice channel)
  ccord_id instance_id;                       // ACTIVITY_LEAVE: the running instance
  char session_id[CCORD_ADMIN_ACTIVITY_SESSION_MAX + 1]; // ACTIVITY_LAUNCH: the gateway READY session id
} ccord_admin_request;

// Which fields of ccord_admin_request.event an EVENT_MODIFY request carries.
enum {
  CCORD_ADMIN_EVENT_HAS_NAME = 1u << 0,
  CCORD_ADMIN_EVENT_HAS_DESCRIPTION = 1u << 1,
  CCORD_ADMIN_EVENT_HAS_LOCATION = 1u << 2,
  CCORD_ADMIN_EVENT_HAS_CHANNEL = 1u << 3,
  CCORD_ADMIN_EVENT_HAS_START = 1u << 4,
  CCORD_ADMIN_EVENT_HAS_END = 1u << 5,
  CCORD_ADMIN_EVENT_HAS_PRIVACY = 1u << 6,
  CCORD_ADMIN_EVENT_HAS_STATUS = 1u << 7,
  CCORD_ADMIN_EVENT_HAS_ENTITY_TYPE = 1u << 8,
  CCORD_ADMIN_EVENT_HAS_IMAGE = 1u << 9,      // cover image: body only ("" data URI = clear)
};

// Short past-tense-free description for failure notices: "create the channel", "kick the member", ...
[[nodiscard]] const char *ccord_admin_op_verb(ccord_admin_op op);

// Channel types accepted for creation: 0 text, 2 voice, 4 category, 5 announcement, 13 stage, 15 forum.
// `parent_id` places it in a category (ignored for categories); `topic` may be nullptr.
[[nodiscard]] ccord_status ccord_admin_create_channel(ccord_admin_request *out, ccord_id guild_id, const char *name,
                                                      uint8_t discord_type, ccord_id parent_id, const char *topic,
                                                      const char *reason, ccord_error *err);

// Only the fields that are set are sent. At least one must be.
typedef struct ccord_admin_channel_edit {
  const char *name;                      // nullptr = unchanged
  const char *topic;                     // nullptr = unchanged, "" = clear
  int8_t nsfw;                           // -1 = unchanged
  int32_t slowmode_s;                    // -1 = unchanged, else 0..CCORD_ADMIN_SLOWMODE_MAX_S
  int32_t bitrate;                       // -1 = unchanged, else CCORD_ADMIN_BITRATE_MIN..CCORD_ADMIN_BITRATE_MAX
  int32_t user_limit;                    // -1 = unchanged, else 0..CCORD_ADMIN_USER_LIMIT_MAX (0 = unlimited)
  int32_t default_auto_archive_duration; // -1 = unchanged, else 60 / 1440 / 4320 / 10080
  bool has_available_tags;               // true -> send available_tags[] (0..CCORD_FORUM_MAX_TAGS)
  const ccord_forum_tag *available_tags;
  size_t available_tag_count;
} ccord_admin_channel_edit;
#define CCORD_ADMIN_CHANNEL_EDIT_INIT { \
  .nsfw = -1, .slowmode_s = -1, .bitrate = -1, .user_limit = -1, .default_auto_archive_duration = -1 \
}
[[nodiscard]] ccord_status ccord_admin_edit_channel(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                                    const ccord_admin_channel_edit *edit, const char *reason,
                                                    ccord_error *err);

[[nodiscard]] ccord_status ccord_admin_delete_channel(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                                      const char *reason, ccord_error *err);

// A public thread (24 h auto-archive) in a text channel, started without a message.
[[nodiscard]] ccord_status ccord_admin_create_thread(ccord_admin_request *out, ccord_id guild_id,
                                                     ccord_id parent_channel_id, const char *name, ccord_error *err);

// A thread started from an existing message (POST /channels/{c}/messages/{m}/threads).
[[nodiscard]] ccord_status ccord_admin_create_thread_from_message(ccord_admin_request *out, ccord_id guild_id,
                                                                  ccord_id channel_id, ccord_id message_id,
                                                                  const char *name, ccord_error *err);

[[nodiscard]] ccord_status ccord_admin_archive_thread(ccord_admin_request *out, ccord_id guild_id, ccord_id thread_id,
                                                      bool archived, const char *reason, ccord_error *err);

[[nodiscard]] ccord_status ccord_admin_join_thread(ccord_admin_request *out, ccord_id guild_id, ccord_id thread_id,
                                                   ccord_error *err);

[[nodiscard]] ccord_status ccord_admin_leave_thread(ccord_admin_request *out, ccord_id guild_id, ccord_id thread_id,
                                                    ccord_error *err);

typedef struct ccord_admin_guild_edit {
  const char *name;                // nullptr = unchanged; else 2..100 chars
  const char *icon_data_uri;       // nullptr = unchanged; "" = clear (null); else "data:image/...;base64,..."
  const char *banner_data_uri;     // nullptr = unchanged; "" = clear (null); else "data:image/...;base64,..."
  bool has_system_channel;         // true -> send system_channel_id (0 = null)
  ccord_id system_channel_id;
  int32_t system_channel_flags;    // -1 = unchanged; else bitmask within CCORD_SYSCHAN_KNOWN_MASK
  int8_t default_notifications;    // -1 = unchanged; else 0 (all) or 1 (@mentions)
  bool has_afk_channel;            // true -> send afk_channel_id (0 = null)
  ccord_id afk_channel_id;
  int32_t afk_timeout;             // -1 = unchanged; else 60 / 300 / 900 / 1800 / 3600
  int8_t verification_level;       // -1 = unchanged; else 0..4
  int8_t explicit_content_filter;  // -1 = unchanged; else 0..2
  bool has_community;              // true -> send features[] toggling COMMUNITY
  bool feature_community;          // desired COMMUNITY state when has_community == true
  bool preserve_vanity_feature;    // keep VANITY_URL in features[] when has_community == true
  bool preserve_banner_feature;    // keep BANNER in features[] when has_community == true
  bool has_rules_channel;          // true -> send rules_channel_id (0 = null)
  ccord_id rules_channel_id;
  bool has_public_updates_channel; // true -> send public_updates_channel_id (0 = null)
  ccord_id public_updates_channel_id;
} ccord_admin_guild_edit;
#define CCORD_ADMIN_GUILD_EDIT_INIT { \
  .system_channel_flags = -1, .default_notifications = -1, .afk_timeout = -1, \
  .verification_level = -1, .explicit_content_filter = -1 \
}
[[nodiscard]] ccord_status ccord_admin_edit_guild_ex(ccord_admin_request *out, ccord_id guild_id,
                                                     const ccord_admin_guild_edit *edit, const char *reason,
                                                     ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_edit_guild(ccord_admin_request *out, ccord_id guild_id, const char *name,
                                                  const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_set_incident_actions(ccord_admin_request *out, ccord_id guild_id,
                                                            int64_t invites_disabled_until_ms,
                                                            int64_t dms_disabled_until_ms, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_set_vanity_url(ccord_admin_request *out, ccord_id guild_id, const char *code,
                                                      const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_leave_guild(ccord_admin_request *out, ccord_id guild_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_delete_guild(ccord_admin_request *out, ccord_id guild_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_transfer_ownership(ccord_admin_request *out, ccord_id guild_id,
                                                          ccord_id new_owner_id, const char *reason,
                                                          ccord_error *err);

// An empty `nick` clears the nickname. When `user_id == self_id` the "@me" endpoint is used (which needs only
// Change Nickname, not Manage Nicknames).
[[nodiscard]] ccord_status ccord_admin_set_nick(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                                                ccord_id self_id, const char *nick, const char *reason,
                                                ccord_error *err);

// Times a member out for `duration_ms` from `now_unix_ms` (at most 28 days); 0 lifts an active timeout.
[[nodiscard]] ccord_status ccord_admin_timeout(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                                               int64_t now_unix_ms, int64_t duration_ms, const char *reason,
                                               ccord_error *err);

[[nodiscard]] ccord_status ccord_admin_kick(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                                            const char *reason, ccord_error *err);

// `delete_message_seconds`: how much of the member's recent history to remove (0..7 days).
[[nodiscard]] ccord_status ccord_admin_ban(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                                           int32_t delete_message_seconds, const char *reason, ccord_error *err);

// Hides a DM or group DM from the list (Discord keeps the conversation; a new message brings it back).
[[nodiscard]] ccord_status ccord_admin_close_dm(ccord_admin_request *out, ccord_id channel_id, ccord_error *err);

// "2026-11-10T12:30:00.000Z" for a unix-millisecond instant. Returns false when it does not fit / is out of range.
[[nodiscard]] bool ccord_admin_format_iso(int64_t unix_ms, char *out, size_t cap);

// ---- roles ----
// Only the fields that are set are sent (a create needs none: Discord names it "new role").
typedef struct ccord_admin_role_edit {
  const char *name;         // nullptr = unchanged
  int64_t permissions;      // -1 = unchanged; else the permission bits (unknown bits are an error)
  int32_t color;            // -1 = unchanged; else 0xRRGGBB (0 = no colour)
  int8_t hoist;             // -1 = unchanged
  int8_t mentionable;       // -1 = unchanged
} ccord_admin_role_edit;
#define CCORD_ADMIN_ROLE_EDIT_INIT {.permissions = -1, .color = -1, .hoist = -1, .mentionable = -1}
[[nodiscard]] ccord_status ccord_admin_create_role(ccord_admin_request *out, ccord_id guild_id,
                                                   const ccord_admin_role_edit *init, const char *reason,
                                                   ccord_error *err);
// @everyone (role id == guild id) can only have its permissions changed; Discord refuses a rename.
[[nodiscard]] ccord_status ccord_admin_edit_role(ccord_admin_request *out, ccord_id guild_id, ccord_id role_id,
                                                 const ccord_admin_role_edit *edit, const char *reason,
                                                 ccord_error *err);
// @everyone cannot be deleted.
[[nodiscard]] ccord_status ccord_admin_delete_role(ccord_admin_request *out, ccord_id guild_id, ccord_id role_id,
                                                   const char *reason, ccord_error *err);
// Gives each listed role the position beside it (the caller works out the minimal set that moves what it wants moved).
[[nodiscard]] ccord_status ccord_admin_reorder_roles(ccord_admin_request *out, ccord_id guild_id, const ccord_id *role_ids,
                                                     const int32_t *positions, size_t count, const char *reason,
                                                     ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_add_member_role(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                                                       ccord_id role_id, const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_remove_member_role(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                                                          ccord_id role_id, const char *reason, ccord_error *err);

// ---- bans, invites, audit log ----
[[nodiscard]] ccord_status ccord_admin_unban(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                                             const char *reason, ccord_error *err);
// `max_age_s` 0..7 days (0 = never), `max_uses` 0..100 (0 = unlimited).
[[nodiscard]] ccord_status ccord_admin_create_invite(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                                     uint32_t max_age_s, uint32_t max_uses, bool temporary,
                                                     ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_revoke_invite(ccord_admin_request *out, ccord_id guild_id, const char *code,
                                                     const char *reason, ccord_error *err);
// Reads. The answers arrive as CCORD_NET_BAN_LIST / INVITE_LIST / AUDIT_LOG events.
[[nodiscard]] ccord_status ccord_admin_list_bans(ccord_admin_request *out, ccord_id guild_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_list_invites(ccord_admin_request *out, ccord_id guild_id, ccord_error *err);
// `user_id` / `action` narrow the log (CCORD_ID_NONE / 0 = everything).
[[nodiscard]] ccord_status ccord_admin_list_audit_log(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                                                      uint16_t action, ccord_error *err);
// ---- notification settings (the user's own, per server / category / channel) ----
// Only the fields that are set are sent; at least one must be.
typedef struct ccord_admin_notify_edit {
  ccord_id channel_id;           // 0 = the server (or the direct messages) itself; else a channel / category override
  int8_t level;                  // -1 unchanged; else a ccord_notify_level (INHERIT, "use the one above", for channels only)
  int8_t muted;                  // -1 unchanged; 1 mute, 0 unmute
  int64_t mute_duration_ms;      // with muted == 1: 0 = until turned back on, else for this long from `now`
  int8_t suppress_everyone;      // -1 unchanged; server row only
  int8_t suppress_roles;         // -1 unchanged; server row only
} ccord_admin_notify_edit;
#define CCORD_ADMIN_NOTIFY_EDIT_INIT {.level = -1, .muted = -1, .suppress_everyone = -1, .suppress_roles = -1}
// `guild_id` may be CCORD_DM_GUILD_ID (direct messages). `now_unix_ms` dates a timed mute.
[[nodiscard]] ccord_status ccord_admin_set_notify(ccord_admin_request *out, ccord_id guild_id,
                                                  const ccord_admin_notify_edit *edit, int64_t now_unix_ms,
                                                  ccord_error *err);

// True for ops that only read (no body, no audit reason, nothing changes on Discord).
[[nodiscard]] bool ccord_admin_op_is_read(ccord_admin_op op);

// ---- server rail layout, channel reorder, create/join server ----
// Extracts and validates an invite code from a bare code or discord.gg / discord.com/invite URL.
[[nodiscard]] ccord_status ccord_admin_parse_invite_code(const char *input,
                                                         char out[CCORD_ADMIN_INVITE_CODE_MAX + 1],
                                                         ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_set_guild_folders(ccord_admin_request *out,
                                                         const ccord_guild_folder_item *items, size_t count,
                                                         ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_reorder_channels(ccord_admin_request *out, ccord_id guild_id,
                                                        const ccord_id *channel_ids, const int32_t *positions,
                                                        const ccord_id *parent_ids, const uint8_t *has_parent,
                                                        size_t count, const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_create_guild(ccord_admin_request *out, const char *name, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_preview_invite(ccord_admin_request *out, const char *invite_or_url,
                                                      ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_accept_invite(ccord_admin_request *out, const char *invite_or_url,
                                                     ccord_error *err);

// ---- group DMs, message requests, user notes, custom status (M3c) ----
[[nodiscard]] ccord_status ccord_admin_create_group_dm(ccord_admin_request *out, const ccord_id *recipient_ids,
                                                       size_t count, const char *optional_name, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_add_dm_recipient(ccord_admin_request *out, ccord_id channel_id,
                                                        ccord_id user_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_remove_dm_recipient(ccord_admin_request *out, ccord_id channel_id,
                                                           ccord_id user_id, ccord_error *err);
typedef struct ccord_admin_group_dm_edit {
  const char *name;            // nullptr = unchanged, "" = clear custom name, else 1..100 chars
  const char *icon_data_uri;   // nullptr = unchanged, "" = clear icon, else "data:image/..."
} ccord_admin_group_dm_edit;
[[nodiscard]] ccord_status ccord_admin_edit_group_dm(ccord_admin_request *out, ccord_id channel_id,
                                                     const ccord_admin_group_dm_edit *edit, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_accept_message_request(ccord_admin_request *out, ccord_id channel_id,
                                                              ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_reject_message_request(ccord_admin_request *out, ccord_id channel_id,
                                                              ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_set_user_note(ccord_admin_request *out, ccord_id user_id, const char *note,
                                                     ccord_error *err);
// Both `emoji_name` and `text` empty (and `emoji_id == 0`) clears the custom status (`{"custom_status":null}`).
// `expires_unix_ms` == 0 means "don't clear".
[[nodiscard]] ccord_status ccord_admin_set_custom_status(ccord_admin_request *out, const char *emoji_name,
                                                         ccord_id emoji_id, const char *text,
                                                         int64_t expires_unix_ms, ccord_error *err);

// Persists the online status (PATCH /users/@me/settings {"status":"online|idle|dnd|invisible"}). A gateway op 3 only
// changes this one connection; the account setting is what survives a reconnect and what the web client's other
// sessions follow. CCORD_PRESENCE_OFFLINE means Invisible. Shares CCORD_ADMIN_USER_SETTINGS_PATCH (answered with the
// full settings object); settings_patch is left "unchanged" everywhere.
[[nodiscard]] ccord_status ccord_admin_set_status(ccord_admin_request *out, ccord_presence presence, ccord_error *err);

// ---- guild expressions: emoji, stickers, soundboard (M4b) ----
[[nodiscard]] ccord_status ccord_admin_list_emojis(ccord_admin_request *out, ccord_id guild_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_create_emoji(ccord_admin_request *out, ccord_id guild_id, const char *name,
                                                    const char *image_data_uri, const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_edit_emoji(ccord_admin_request *out, ccord_id guild_id, ccord_id emoji_id,
                                                  const char *name, const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_delete_emoji(ccord_admin_request *out, ccord_id guild_id, ccord_id emoji_id,
                                                    const char *reason, ccord_error *err);

typedef struct ccord_admin_sticker_edit {
  const char *name;          // nullptr = unchanged; else 2..30 chars
  const char *description;   // nullptr = unchanged; "" = clear; else <= 100 chars
  const char *tags;          // nullptr = unchanged; else 1..200 chars
} ccord_admin_sticker_edit;
[[nodiscard]] ccord_status ccord_admin_list_stickers(ccord_admin_request *out, ccord_id guild_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_create_sticker(ccord_admin_request *out, ccord_id guild_id, const char *name,
                                                      const char *description, const char *tags,
                                                      const void *file_data, size_t file_size, const char *mime_type,
                                                      const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_edit_sticker(ccord_admin_request *out, ccord_id guild_id, ccord_id sticker_id,
                                                    const ccord_admin_sticker_edit *edit, const char *reason,
                                                    ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_delete_sticker(ccord_admin_request *out, ccord_id guild_id, ccord_id sticker_id,
                                                      const char *reason, ccord_error *err);

typedef struct ccord_admin_soundboard_edit {
  const char *name;          // nullptr = unchanged; else 2..32 chars
  int16_t volume_pct;        // -1 = unchanged; else 0..100
  bool has_emoji;            // true -> update emoji_id / emoji_name (0 + "" clears both to null)
  ccord_id emoji_id;
  const char *emoji_name;
} ccord_admin_soundboard_edit;
#define CCORD_ADMIN_SOUNDBOARD_EDIT_INIT {.volume_pct = -1}
[[nodiscard]] ccord_status ccord_admin_list_soundboard(ccord_admin_request *out, ccord_id guild_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_create_soundboard(ccord_admin_request *out, ccord_id guild_id, const char *name,
                                                         const char *sound_data_uri, int16_t volume_pct,
                                                         ccord_id emoji_id, const char *emoji_name, const char *reason,
                                                         ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_edit_soundboard(ccord_admin_request *out, ccord_id guild_id, ccord_id sound_id,
                                                       const ccord_admin_soundboard_edit *edit, const char *reason,
                                                       ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_delete_soundboard(ccord_admin_request *out, ccord_id guild_id, ccord_id sound_id,
                                                         const char *reason, ccord_error *err);

// ---- member prune & AutoMod rules (M4c) ----
[[nodiscard]] ccord_status ccord_admin_preview_prune(ccord_admin_request *out, ccord_id guild_id, uint8_t days,
                                                     const ccord_id *include_role_ids, size_t role_count,
                                                     ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_execute_prune(ccord_admin_request *out, ccord_id guild_id, uint8_t days,
                                                     bool compute_prune_count, const ccord_id *include_role_ids,
                                                     size_t role_count, const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_list_automod_rules(ccord_admin_request *out, ccord_id guild_id,
                                                          ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_create_automod_rule(ccord_admin_request *out, ccord_id guild_id,
                                                           const ccord_automod_rule *rule, const char *reason,
                                                           ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_edit_automod_rule(ccord_admin_request *out, ccord_id guild_id,
                                                         ccord_id rule_id, const ccord_automod_rule *rule,
                                                         const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_delete_automod_rule(ccord_admin_request *out, ccord_id guild_id,
                                                           ccord_id rule_id, const char *reason, ccord_error *err);

// ---- integrations & community (M4c) ----
#define CCORD_ADMIN_WEBHOOK_NAME_MAX CCORD_WEBHOOK_NAME_MAX_BYTES

typedef struct ccord_admin_webhook_edit {
  const char *name;            // nullptr = unchanged; else 1..80 chars (cannot contain "clyde" or "discord")
  const char *avatar_data_uri; // nullptr = unchanged; "" = clear (null); else "data:image/..."
  ccord_id channel_id;         // 0 = unchanged; else target channel in the same guild
} ccord_admin_webhook_edit;

[[nodiscard]] ccord_status ccord_admin_list_webhooks(ccord_admin_request *out, ccord_id guild_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_create_webhook(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                                      const char *name, const char *avatar_data_uri,
                                                      const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_edit_webhook(ccord_admin_request *out, ccord_id guild_id, ccord_id webhook_id,
                                                    const ccord_admin_webhook_edit *edit, const char *reason,
                                                    ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_delete_webhook(ccord_admin_request *out, ccord_id guild_id, ccord_id webhook_id,
                                                      const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_follow_channel(ccord_admin_request *out, ccord_id guild_id,
                                                      ccord_id announcement_channel_id, ccord_id webhook_channel_id,
                                                      const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_list_app_cmd_perms(ccord_admin_request *out, ccord_id guild_id,
                                                          ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_get_welcome_screen(ccord_admin_request *out, ccord_id guild_id,
                                                          ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_edit_welcome_screen(ccord_admin_request *out, ccord_id guild_id,
                                                           const ccord_welcome_screen *ws, const char *reason,
                                                           ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_get_onboarding(ccord_admin_request *out, ccord_id guild_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_put_onboarding(ccord_admin_request *out, ccord_id guild_id,
                                                      const ccord_onboarding *ob, const char *reason,
                                                      ccord_error *err);

// ---- channel permission overwrites, channel invites, clone channel & voice moderation (M4d) ----
typedef struct ccord_admin_channel_clone {
  const char *name;                      // required, 1..CCORD_ADMIN_CHANNEL_NAME_MAX
  uint8_t discord_type;                  // 0 text, 2 voice, 4 category, 5 announcement, 13 stage, 15 forum
  ccord_id parent_id;                    // 0 = none (ignored for categories)
  const char *topic;                     // nullptr / "" = none
  bool nsfw;
  uint32_t slowmode_s;                   // 0..CCORD_ADMIN_SLOWMODE_MAX_S
  uint32_t bitrate;                      // 0 = omit, else CCORD_ADMIN_BITRATE_MIN..CCORD_ADMIN_BITRATE_MAX
  uint32_t user_limit;                   // 0 = omit / unlimited, else 1..CCORD_ADMIN_USER_LIMIT_MAX
  uint32_t default_auto_archive_duration;// 0 = omit, else 60 / 1440 / 4320 / 10080
  const ccord_overwrite *overwrites;     // 0..CCORD_ADMIN_CLONE_OVERWRITES_MAX
  size_t overwrite_count;
  const ccord_forum_tag *available_tags; // optional forum tags (0..CCORD_FORUM_MAX_TAGS)
  size_t available_tag_count;
} ccord_admin_channel_clone;

[[nodiscard]] ccord_status ccord_admin_put_channel_overwrite(ccord_admin_request *out, ccord_id guild_id,
                                                             ccord_id channel_id, const ccord_overwrite *ow,
                                                             const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_delete_channel_overwrite(ccord_admin_request *out, ccord_id guild_id,
                                                                ccord_id channel_id, ccord_id overwrite_id,
                                                                const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_list_channel_invites(ccord_admin_request *out, ccord_id guild_id,
                                                            ccord_id channel_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_clone_channel(ccord_admin_request *out, ccord_id guild_id,
                                                     const ccord_admin_channel_clone *clone, const char *reason,
                                                     ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_set_voice_mute(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                                                      bool mute, const char *reason, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_set_voice_deaf(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                                                      bool deaf, const char *reason, ccord_error *err);
// `channel_id == CCORD_ID_NONE` sends `{"channel_id":null}` to disconnect the member from voice.
[[nodiscard]] ccord_status ccord_admin_move_voice_member(ccord_admin_request *out, ccord_id guild_id, ccord_id user_id,
                                                         ccord_id channel_id, const char *reason, ccord_error *err);

// ---- voice status & stage moderation ----
// Defined inline here so the contract ships without editing admin.c (owned by m5-live-transport).
// They mirror admin.c's init_request/set_path/set_body/check_reason behaviour exactly.

// Same zeroing + "-1 = unchanged" sentinels as admin.c's init_request.
static inline void ccord_admin_m5_init_request(ccord_admin_request *out, ccord_admin_op op, ccord_admin_method method)
{
  memset(out, 0, sizeof *out);
  out->op = op;
  out->method = method;
  out->guild_system_flags = -1;
  out->guild_default_notify = -1;
  out->guild_afk_timeout = -1;
  out->guild_verification_level = -1;
  out->guild_explicit_filter = -1;
  out->sound_volume_pct = -1;
}

// Code points of a valid-UTF-8 string (continuation bytes don't start a character).
static inline size_t ccord_admin_m5_text_chars(const char *s, size_t len)
{
  size_t n = 0;
  for (size_t i = 0; i < len; i++) {
    if (((unsigned char)s[i] & 0xC0u) != 0x80u) n++;
  }
  return n;
}

static inline ccord_status ccord_admin_m5_reason(ccord_admin_request *out, const char *reason, ccord_error *err)
{
  if (!reason || reason[0] == '\0') return CCORD_OK;
  size_t n = strlen(reason);
  if (n > CCORD_ADMIN_REASON_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "reason is too long");
  if (!ccord_utf8_valid_text(reason, n) || strpbrk(reason, "\r\n")) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "reason has invalid characters");
  }
  (void)snprintf(out->reason, sizeof out->reason, "%s", reason);
  return CCORD_OK;
}

// Copies a finished JSON writer buffer into the request body (admin.c set_body semantics).
static inline ccord_status ccord_admin_m5_commit_body(ccord_admin_request *out, ccord_buf *b, const ccord_jw *w,
                                                      ccord_error *err)
{
  ccord_status st = CCORD_OK;
  if (!ccord_jw_ok(w) || b->len >= sizeof out->body) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  } else {
    memcpy(out->body, ccord_buf_cstr(b), b->len + 1u);
  }
  ccord_buf_free(b);
  return st;
}

// The PUT /channels/{c}/voice-status body: {"status":"..."} (nullptr / "" -> {"status":null}).
// Validates the channel id and the 500-character limit; writes compact JSON into `out`.
[[nodiscard]] static inline ccord_status ccord_admin_build_voice_status(ccord_buf *out, ccord_id channel_id,
                                                                        const char *status, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null buffer");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing channel");
  size_t len = 0;
  if (status && status[0] != '\0') {
    len = strlen(status);
    if (!ccord_utf8_valid(status, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "status has invalid characters");
    if (ccord_admin_m5_text_chars(status, len) > CCORD_ADMIN_VOICE_STATUS_MAX) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "status is too long (max %u)", CCORD_ADMIN_VOICE_STATUS_MAX);
    }
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "status");
  if (len == 0) ccord_jw_null(&w);
  else ccord_jw_string(&w, status, len);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  return CCORD_OK;
}

// PUT /channels/{c}/voice-status. `status` nullptr / "" clears it. No audit reason (Discord takes none here).
[[nodiscard]] static inline ccord_status ccord_admin_set_voice_status(ccord_admin_request *out, ccord_id channel_id,
                                                                      const char *status, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  ccord_admin_m5_init_request(out, CCORD_ADMIN_VOICE_STATUS, CCORD_ADMIN_PUT);
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  ccord_status st = ccord_admin_build_voice_status(&b, channel_id, status, err);
  if (st != CCORD_OK) { ccord_buf_free(&b); return st; }
  if (b.len >= sizeof out->body) {
    ccord_buf_free(&b);
    return ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  }
  memcpy(out->body, ccord_buf_cstr(&b), b.len + 1u);
  ccord_buf_free(&b);
  int n = snprintf(out->path, sizeof out->path, "/channels/%llu/voice-status", (unsigned long long)channel_id);
  if (n < 0 || (size_t)n >= sizeof out->path) {
    out->body[0] = '\0';
    return ccord_error_set(err, CCORD_ERR_RANGE, "request path too long");
  }
  out->channel_id = channel_id;
  if (status && status[0] != '\0') (void)snprintf(out->voice_status, sizeof out->voice_status, "%s", status);
  return CCORD_OK;
}

// PATCH /guilds/{g}/voice-states/@me: ask to speak on a stage (request_to_speak_ms > 0) or cancel the ask (0 -> null).
[[nodiscard]] static inline ccord_status ccord_admin_stage_request_speak(ccord_admin_request *out, ccord_id guild_id,
                                                                         ccord_id channel_id, int64_t request_to_speak_ms,
                                                                         ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  ccord_admin_m5_init_request(out, CCORD_ADMIN_STAGE_REQUEST_SPEAK, CCORD_ADMIN_PATCH);
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "that isn't a server");
  }
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing channel");
  char iso[40];
  if (request_to_speak_ms != 0 && !ccord_admin_format_iso(request_to_speak_ms, iso, sizeof iso)) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad time");
  }
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id");
  ccord_jw_id(&w, channel_id);
  ccord_jw_key(&w, "request_to_speak_timestamp");
  if (request_to_speak_ms == 0) ccord_jw_null(&w);
  else ccord_jw_string(&w, iso, strlen(iso));
  ccord_jw_end_object(&w);
  ccord_status st = ccord_admin_m5_commit_body(out, &b, &w, err);
  if (st != CCORD_OK) return st;
  int n = snprintf(out->path, sizeof out->path, "/guilds/%llu/voice-states/@me", (unsigned long long)guild_id);
  if (n < 0 || (size_t)n >= sizeof out->path) {
    out->body[0] = '\0';
    return ccord_error_set(err, CCORD_ERR_RANGE, "request path too long");
  }
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  out->request_to_speak_ms = request_to_speak_ms;
  return CCORD_OK;
}

// PATCH /guilds/{g}/voice-states/{u}: speaker=true invites the member to speak (suppress=false),
// speaker=false returns them to the audience (suppress=true).
[[nodiscard]] static inline ccord_status ccord_admin_stage_set_speaker(ccord_admin_request *out, ccord_id guild_id,
                                                                       ccord_id channel_id, ccord_id user_id,
                                                                       bool speaker, const char *reason,
                                                                       ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  ccord_admin_m5_init_request(out, CCORD_ADMIN_STAGE_SET_SPEAKER, CCORD_ADMIN_PATCH);
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "that isn't a server");
  }
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing channel");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing member");
  ccord_status st = ccord_admin_m5_reason(out, reason, err);
  if (st != CCORD_OK) return st;
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id");
  ccord_jw_id(&w, channel_id);
  ccord_jw_key(&w, "suppress");
  ccord_jw_bool(&w, !speaker);
  ccord_jw_end_object(&w);
  st = ccord_admin_m5_commit_body(out, &b, &w, err);
  if (st != CCORD_OK) return st;
  int n = snprintf(out->path, sizeof out->path, "/guilds/%llu/voice-states/%llu", (unsigned long long)guild_id,
                   (unsigned long long)user_id);
  if (n < 0 || (size_t)n >= sizeof out->path) {
    out->body[0] = '\0';
    return ccord_error_set(err, CCORD_ERR_RANGE, "request path too long");
  }
  out->guild_id = guild_id;
  out->channel_id = channel_id;
  out->user_id = user_id;
  out->stage_speaker = speaker;
  return CCORD_OK;
}

// PATCH /stage-instances/{channel_id}: set the stage topic (nullptr / "" clears it to null).
// The topic rides on the shared topic[]/has_topic fields (has_topic && topic[0]=='\0' = clear).
[[nodiscard]] static inline ccord_status ccord_admin_stage_topic(ccord_admin_request *out, ccord_id channel_id,
                                                                 const char *topic, const char *reason,
                                                                 ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  ccord_admin_m5_init_request(out, CCORD_ADMIN_STAGE_TOPIC, CCORD_ADMIN_PATCH);
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing channel");
  ccord_status st = ccord_admin_m5_reason(out, reason, err);
  if (st != CCORD_OK) return st;
  size_t len = 0;
  if (topic && topic[0] != '\0') {
    len = strlen(topic);
    if (!ccord_utf8_valid(topic, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "topic has invalid characters");
    if (ccord_admin_m5_text_chars(topic, len) > CCORD_ADMIN_STAGE_TOPIC_MAX) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "topic is too long (max %u)", CCORD_ADMIN_STAGE_TOPIC_MAX);
    }
  }
  ccord_buf b;
  ccord_buf_init(&b, CCORD_ADMIN_BODY_MAX);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "topic");
  if (len == 0) ccord_jw_null(&w);
  else ccord_jw_string(&w, topic, len);
  ccord_jw_end_object(&w);
  st = ccord_admin_m5_commit_body(out, &b, &w, err);
  if (st != CCORD_OK) return st;
  int n = snprintf(out->path, sizeof out->path, "/stage-instances/%llu", (unsigned long long)channel_id);
  if (n < 0 || (size_t)n >= sizeof out->path) {
    out->body[0] = '\0';
    return ccord_error_set(err, CCORD_ERR_RANGE, "request path too long");
  }
  out->channel_id = channel_id;
  out->has_topic = true;
  if (len != 0) (void)snprintf(out->topic, sizeof out->topic, "%s", topic);
  return CCORD_OK;
}

// POST /channels/{c}/call/stop-ringing: decline an incoming DM / group-DM call without touching voice state.
// Discord answers 204; the live transport drops self from the cached ringing[] and emits CALL_UPDATE on completion.
// This is deliberately NOT a Gateway OP 4 disconnect: sending {guild_id:null, channel_id:null} would also drop any
// active guild voice the user is in while the phone rings.
[[nodiscard]] static inline ccord_status ccord_admin_call_stop_ringing(ccord_admin_request *out, ccord_id channel_id,
                                                                       ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  ccord_admin_m5_init_request(out, CCORD_ADMIN_CALL_STOP_RINGING, CCORD_ADMIN_POST);
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "missing channel");
  int n = snprintf(out->path, sizeof out->path, "/channels/%llu/call/stop-ringing", (unsigned long long)channel_id);
  if (n < 0 || (size_t)n >= sizeof out->path) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "request path too long");
  }
  out->channel_id = channel_id;
  out->guild_id = CCORD_DM_GUILD_ID;
  out->body[0] = '{';
  out->body[1] = '}';
  out->body[2] = '\0';
  return CCORD_OK;
}

// ---- account settings, devices & connections ----
// Pure body builders: write compact JSON into an already-initialised ccord_buf. Every unchanged
// field (-1 / "") is omitted so a PATCH only carries what the user actually changed.
[[nodiscard]] ccord_status ccord_admin_build_user_settings_patch(ccord_buf *out,
                                                                 const ccord_user_settings_patch *patch,
                                                                 ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_build_sessions_logout(ccord_buf *out, const char hashes[][64],
                                                             size_t n, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_build_connection_edit(ccord_buf *out,
                                                             const ccord_connection_edit *edit,
                                                             ccord_error *err);

[[nodiscard]] ccord_status ccord_admin_user_settings_patch(ccord_admin_request *out,
                                                           const ccord_user_settings_patch *patch,
                                                           ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_auth_sessions_list(ccord_admin_request *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_auth_sessions_logout(ccord_admin_request *out,
                                                            const char hashes[][64], size_t n,
                                                            ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_connections_list(ccord_admin_request *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_connection_edit(ccord_admin_request *out, const char *name,
                                                       const char *id, const ccord_connection_edit *edit,
                                                       ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_connection_delete(ccord_admin_request *out, const char *name,
                                                         const char *id, ccord_error *err);

// ---- stage lifecycle & guild scheduled events ----
// POST /stage-instances: start a stage in a stage channel. `topic` may be nullptr / "" (none); privacy_level 0
// defaults to 2 (GUILD_ONLY), the only value Discord currently accepts. The answer arrives as STAGE_INSTANCE_UPDATE.
[[nodiscard]] ccord_status ccord_admin_stage_create(ccord_admin_request *out, ccord_id guild_id, ccord_id channel_id,
                                                    const char *topic, uint8_t privacy_level, const char *reason,
                                                    ccord_error *err);
// DELETE /stage-instances/{channel_id}: end the stage. The answer arrives as STAGE_INSTANCE_UPDATE (stage_deleted).
[[nodiscard]] ccord_status ccord_admin_stage_delete(ccord_admin_request *out, ccord_id channel_id, const char *reason,
                                                    ccord_error *err);

// GET /guilds/{g}/scheduled-events?with_user_count=true. The answer arrives as CCORD_NET_SCHEDULED_EVENT_LIST.
[[nodiscard]] ccord_status ccord_admin_event_list(ccord_admin_request *out, ccord_id guild_id, ccord_error *err);
// POST /guilds/{g}/scheduled-events. `event` supplies name, entity_type and scheduled_start_ms (required),
// channel_id (required for STAGE_INSTANCE / VOICE), location + scheduled_end_ms (required for EXTERNAL), and
// optionally description / privacy_level (0 -> 2). status / user_count / me_rsvped are ignored: Discord assigns
// them. The answer arrives as CCORD_NET_SCHEDULED_EVENT_UPDATE.
[[nodiscard]] ccord_status ccord_admin_event_create(ccord_admin_request *out, ccord_id guild_id,
                                                    const ccord_scheduled_event *event, const char *reason,
                                                    ccord_error *err);
// EVENT_MODIFY fields: only what is set is sent. nullptr / -1 = unchanged; "" clears a text field.
typedef struct ccord_admin_event_edit {
  const char *name;            // nullptr = unchanged; else 1..CCORD_ADMIN_EVENT_NAME_MAX chars
  const char *description;     // nullptr = unchanged; "" = clear; else <= CCORD_ADMIN_EVENT_DESC_MAX bytes
  const char *location;        // nullptr = unchanged; "" = clear; else 1..CCORD_ADMIN_EVENT_LOCATION_MAX chars
  int64_t channel_id;          // -1 = unchanged; 0 = null (EXTERNAL); else the stage / voice channel id
  int64_t start_ms;            // -1 = unchanged; else the new scheduled_start_time (unix ms)
  int64_t end_ms;              // -1 = unchanged; 0 = null; else the new scheduled_end_time (unix ms)
  int8_t privacy_level;        // -1 = unchanged; else 2 (GUILD_ONLY)
  int8_t status;               // -1 = unchanged; else a ccord_scheduled_event_status (1..4)
  int8_t entity_type;          // -1 = unchanged; else a ccord_scheduled_event_entity_type (1..3)
  const char *image_data_uri;  // nullptr = unchanged; "" = clear; else "data:image/...;base64,..."
} ccord_admin_event_edit;
#define CCORD_ADMIN_EVENT_EDIT_INIT { \
  .channel_id = -1, .start_ms = -1, .end_ms = -1, .privacy_level = -1, .status = -1, .entity_type = -1 \
}
// PATCH /guilds/{g}/scheduled-events/{e}: only the fields set in `edit` are sent (at least one must be).
[[nodiscard]] ccord_status ccord_admin_event_modify(ccord_admin_request *out, ccord_id guild_id, ccord_id event_id,
                                                    const ccord_admin_event_edit *edit, const char *reason,
                                                    ccord_error *err);
// DELETE /guilds/{g}/scheduled-events/{e}.
[[nodiscard]] ccord_status ccord_admin_event_delete(ccord_admin_request *out, ccord_id guild_id, ccord_id event_id,
                                                    const char *reason, ccord_error *err);
// PUT / DELETE /guilds/{g}/scheduled-events/{e}/users/@me: RSVP / withdraw the signed-in user's RSVP.
[[nodiscard]] ccord_status ccord_admin_event_rsvp_add(ccord_admin_request *out, ccord_id guild_id, ccord_id event_id,
                                                      ccord_error *err);
[[nodiscard]] ccord_status ccord_admin_event_rsvp_remove(ccord_admin_request *out, ccord_id guild_id, ccord_id event_id,
                                                         ccord_error *err);

// ---- membership screening ----
// GET /guilds/{g}/member-verification?with_guild=true. The answer arrives as CCORD_NET_MEMBER_VERIFICATION and is
// owned by the transport until the next poll(); the store deep-copies it.
[[nodiscard]] ccord_status ccord_admin_get_member_verification(ccord_admin_request *out, ccord_id guild_id,
                                                               ccord_error *err);
// PUT /guilds/{g}/requests/@me with the filled form ({"form_fields":[...]}). The member's `pending` flag clears
// when Discord confirms (gateway GUILD_MEMBER_UPDATE / the mock's answer), never optimistically here.
[[nodiscard]] ccord_status ccord_admin_submit_member_verification(ccord_admin_request *out, ccord_id guild_id,
                                                                  const ccord_member_verification *form,
                                                                  ccord_error *err);

// ---- embedded activities ----
// These ride the existing admin queue (`ccord_transport_admin_action`); the transport vtable gains nothing.
// GET /activities/shelf?guild_id={g}: the guild's activity shelf. The answer arrives as CCORD_NET_ACTIVITY_SHELF.
[[nodiscard]] ccord_status ccord_admin_activity_shelf(ccord_admin_request *out, ccord_id guild_id, ccord_error *err);
// POST /activities/{c}/{app} with {"session_id":"..."} (the gateway READY session id). Discord answers 204; launching
// also joins the caller's instance. The running instance arrives as CCORD_NET_ACTIVITY_INSTANCE_UPDATE.
[[nodiscard]] ccord_status ccord_admin_activity_launch(ccord_admin_request *out, ccord_id channel_id,
                                                       ccord_id application_id, const char *session_id,
                                                       ccord_error *err);
// POST /applications/{app}/activities/{location}/instances/{instance}/leave. Discord answers 204; the instance ends.
[[nodiscard]] ccord_status ccord_admin_activity_leave(ccord_admin_request *out, ccord_id application_id,
                                                      ccord_id location_id, ccord_id instance_id, ccord_error *err);

// ---- guild profile ----
// GET /guilds/{g}/profile: the guild's public profile card (used by clan tag popovers).
[[nodiscard]] ccord_status ccord_admin_get_guild_profile(ccord_admin_request *out, ccord_id guild_id, ccord_error *err);
