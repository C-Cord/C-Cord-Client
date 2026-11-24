// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/link_event.h>
#include <ccord/core/os.h>
#include <ccord/model/model.h>
#include <ccord/model/forum.h>
#include <ccord/model/notify.h>
#include <ccord/model/report.h>
#include <ccord/model/rich.h>
#include <ccord/model/search.h>
#include <ccord/net/admin.h>
#include <ccord/net/discord_proto.h>
// Rich Presence (Discord RPC SET_ACTIVITY) wire limits (shared across live gateway, mock transport, and app RPC).
#define CCORD_GATEWAY_RICH_MAX_BYTES 8192u
#define CCORD_GATEWAY_RICH_MAX_ACTIVITIES 4u
// Page request for a forum's posts (GET /channels/{id}/threads/search).
typedef struct ccord_forum_query {
  ccord_id forum_id;
  uint32_t offset;           // threads to skip
  uint32_t limit;            // 1..CCORD_FORUM_PAGE_SIZE (0 -> default)
  ccord_forum_sort sort;
  const char *name;          // optional server-side title search; nullptr / "" = none
  ccord_id tag_ids[CCORD_FORUM_MAX_APPLIED_TAGS]; // optional tag filter(s)
  uint8_t tag_count;
} ccord_forum_query;
// Transport = the only door to a "server". All calls are non-blocking, main thread.
typedef enum ccord_connection_state : uint8_t { CCORD_CONN_DISCONNECTED, CCORD_CONN_CONNECTING, CCORD_CONN_CONNECTED, CCORD_CONN_FAILED } ccord_connection_state;
typedef enum ccord_net_event_type : uint8_t {
  CCORD_NET_NONE, CCORD_NET_STATE, CCORD_NET_SNAPSHOT, CCORD_NET_HISTORY, CCORD_NET_MESSAGE_CREATE, CCORD_NET_ERROR,
  CCORD_NET_MESSAGE_UPDATE,    // messages[0] is the edited message
  CCORD_NET_MESSAGE_DELETE,    // channel_id, message_id
  CCORD_NET_PRESENCE_UPDATE,   // user_id, presence, status_text
  CCORD_NET_TYPING_START,      // guild_id, channel_id, user_id, timestamp_ms = expiry
  CCORD_NET_CHANNEL_UPSERT,    // channel
  CCORD_NET_CHANNEL_DELETE,    // guild_id, channel_id
  CCORD_NET_REACTION_UPDATE,   // channel_id, message_id, user_id, reaction_count, self_reacted
  CCORD_NET_VOICE_STATE_UPDATE, // voice_state (guild/channel/user + self flags)
  CCORD_NET_VOICE_SERVER_UPDATE, // voice_server (guild/channel/token/endpoint)
  CCORD_NET_STREAM_UPDATE,     // stream (guild/channel/user, active, config)
  CCORD_NET_MESSAGE_PIN_UPDATE, // channel_id, message_id, pinned
  CCORD_NET_GUILD_UPSERT,      // guild + guild_channels + guild_members + users
  CCORD_NET_GUILD_DELETE,      // guild_id
  CCORD_NET_GUILD_MEMBERS,     // guild_id + guild_members + users
  CCORD_NET_PIN_LIST,          // channel_id + messages (cached pins)
  CCORD_NET_MEDIA_LOADED,      // media_url + media_path + media_data + media_len; status != OK = the download failed (no data)
  CCORD_NET_SEARCH_RESULTS,    // messages + message_count
  CCORD_NET_MESSAGE_DELETE_BULK, // channel_id, guild_id, message_ids + message_id_count
  CCORD_NET_CHANNEL_PINS_UPDATE, // channel_id, guild_id, timestamp_ms
  CCORD_NET_MESSAGE_ACK,       // channel_id, message_id, mention_count
  CCORD_NET_RELATIONSHIP_UPDATE, // user_id, relationship_kind (+ optional users[0])
  CCORD_NET_HISTORY_FAILED,    // channel_id, status, text
  CCORD_NET_USER_PROFILE,      // user_id, user_profile (+ users[0])
  CCORD_NET_STREAM_SIGNAL,     // stream_signal (Go Live STREAM_CREATE / STREAM_SERVER_UPDATE / STREAM_UPDATE / STREAM_DELETE)
  CCORD_NET_FORUM_POSTS,       // forum_posts (+ users): channel_id = forum for a thread-search page (forum_offset/total/has_more), 0 for a gateway sync
  CCORD_NET_FORUM_POST_CREATED, // channel (new thread) + forum_posts[0] + messages[0] (starter message) + users
  CCORD_NET_FORUM_FAILED,      // channel_id = forum, status, text, forum_creating (create vs list), forum_offset
  CCORD_NET_GUILD_EXPRESSIONS, // guild_id + emojis/stickers (replace_emojis / replace_stickers say which lists are authoritative)
  CCORD_NET_EXPRESSION_DATA,   // expr_kind + expr_arg + expr_body (raw JSON); status != OK = request failed
  CCORD_NET_SPEAKING,          // user_id + speaking (voice activity state transition)
  CCORD_NET_ACTION_FAILED,     // action + channel_id/message_id + status + text (reason) [+ draft]: a user action did not go through
  CCORD_NET_MEMBER_REMOVE,     // guild_id, user_id: a member left, was kicked or banned
  CCORD_NET_ROLE_UPSERT,       // roles[0] (guild_id inside): a role was created or changed
  CCORD_NET_ROLE_DELETE,       // guild_id, role_id
  CCORD_NET_MEMBER_ROLE_CHANGE, // guild_id, user_id, role_id, role_added: one role given to / taken from a member
  CCORD_NET_BAN_LIST,          // guild_id + bans (the answer to LIST_BANS; replaces the cached list)
  CCORD_NET_BAN_REMOVE,        // guild_id, user_id: a ban was lifted
  CCORD_NET_INVITE_LIST,       // guild_id + invites (the answer to LIST_INVITES; replaces the cached list)
  CCORD_NET_INVITE_UPSERT,     // guild_id + invites[0]: an invite was created
  CCORD_NET_INVITE_DELETE,     // guild_id + invite_code: an invite was revoked
  CCORD_NET_AUDIT_LOG,         // guild_id + audit (newest first; the answer to LIST_AUDIT_LOG)
  CCORD_NET_NOTIFY_SETTINGS,   // guild_id + notify rows: that server's mute / notification settings, whole (replace)
  CCORD_NET_POLL_VOTE,         // guild_id, channel_id, message_id, user_id, poll_answer_id, poll_vote_added
  CCORD_NET_INTERACTION_MODAL, // modal: a bot answered an interaction with INTERACTION_MODAL_CREATE
  CCORD_NET_SLASH_COMMANDS,    // guild_id, channel_id, slash_commands, slash_command_count
  CCORD_NET_SLASH_AUTOCOMPLETE,// slash_choices, slash_choice_count, interaction_nonce
  CCORD_NET_REACTION_USERS,    // channel_id, message_id, reaction_emoji, users, user_count
  CCORD_NET_THREAD_MEMBER_SELF,// guild_id, channel_id, user_id, thread_joined
  CCORD_NET_GUILD_LAYOUT,      // folder_items, folder_item_count: USER_SETTINGS guild_folders update
  CCORD_NET_CHANNEL_POSITIONS, // guild_id, reorder_ids/positions/parents/has_parent/reorder_count
  CCORD_NET_INVITE_PREVIEW,    // invite_preview (GET /invites/{code}?with_counts=true)
  CCORD_NET_INVITE_ACCEPTED,   // invite_preview (POST /invites/{code} -> guild_id/channel_id)
  CCORD_NET_DISCOVERY_RESULTS, // discovery_guilds, discovery_count, discovery_total, discovery_offset, discovery_category_id
  CCORD_NET_DM_RECIPIENT_CHANGE, // channel_id, user_id, recipient_added (+ optional users[0])
  CCORD_NET_MESSAGE_REQUEST_ACCEPTED, // channel_id (DM message request accepted)
  CCORD_NET_USER_NOTE_UPDATE,  // user_id, text (note string; "" = cleared)
  CCORD_NET_CUSTOM_STATUS_UPDATE, // custom_status_emoji, custom_status_text, custom_status_expires_ms, custom_status_clear
  CCORD_NET_GUILD_INCIDENTS,   // guild_id, invites_disabled_until_ms, dms_disabled_until_ms
  CCORD_NET_GUILD_VANITY,      // guild_id, invite_code ("" = cleared)
  CCORD_NET_EMOJI_UPSERT,      // guild_id, emojis[0]
  CCORD_NET_EMOJI_DELETE,      // guild_id, expr_id
  CCORD_NET_STICKER_UPSERT,    // guild_id, stickers[0]
  CCORD_NET_STICKER_DELETE,    // guild_id, expr_id
  CCORD_NET_SOUNDBOARD_UPSERT, // guild_id, soundboard[0]
  CCORD_NET_SOUNDBOARD_DELETE, // guild_id, expr_id
  CCORD_NET_PRUNE_RESULT,      // guild_id, prune_info
  CCORD_NET_AUTOMOD_LIST,      // guild_id, automod_rules, automod_rule_count
  CCORD_NET_AUTOMOD_UPSERT,    // guild_id, automod_rules[0]
  CCORD_NET_AUTOMOD_DELETE,    // guild_id, expr_id
  CCORD_NET_WEBHOOK_LIST,      // guild_id, webhooks, webhook_count
  CCORD_NET_WEBHOOK_UPSERT,    // guild_id, webhooks[0]
  CCORD_NET_WEBHOOK_DELETE,    // guild_id, expr_id
  CCORD_NET_WEBHOOKS_UPDATE,   // guild_id, channel_id (gateway WEBHOOKS_UPDATE)
  CCORD_NET_CHANNEL_FOLLOWER_ADDED, // guild_id, channel_id (source), target_channel_id, expr_id (webhook_id)
  CCORD_NET_APP_CMD_PERMS_LIST, // guild_id, app_cmd_perms, app_cmd_perms_count
  CCORD_NET_WELCOME_SCREEN,    // guild_id, welcome_screen
  CCORD_NET_ONBOARDING,        // guild_id, onboarding
  CCORD_NET_CHANNEL_OVERWRITE_UPSERT, // guild_id, channel_id, overwrite_item
  CCORD_NET_CHANNEL_OVERWRITE_DELETE, // guild_id, channel_id, overwrite_item (.id)
  CCORD_NET_CHANNEL_INVITE_LIST,      // guild_id, channel_id, invites, invite_count
  CCORD_NET_CALL_CREATE,              // call (channel_id, region, ringing[], active) + voice_states
  CCORD_NET_CALL_UPDATE,              // call (ringing[] changed)
  CCORD_NET_CALL_DELETE,              // channel_id (call ended)
  CCORD_NET_STAGE_INSTANCE_UPDATE,    // stage_instance; stage_deleted = true on STAGE_INSTANCE_DELETE
  CCORD_NET_VOICE_STATUS_UPDATE,      // channel_id, voice_status (PUT /channels/{c}/voice-status echo)
  // M6 account settings, devices & connections. Everything rides the existing
  // admin_action hook: no vtable additions.
  CCORD_NET_USER_SETTINGS,            // user_settings: the full object (REST answer or gateway USER_SETTINGS_UPDATE)
  CCORD_NET_AUTH_SESSIONS,            // auth_sessions + auth_session_count (GET /auth/sessions answer)
  CCORD_NET_AUTH_SESSIONS_LOGOUT,     // session_hashes + session_hash_count (204 ack; drop exactly those entries)
  CCORD_NET_CONNECTIONS,              // connections + connection_count (GET /users/@me/connections answer)
  CCORD_NET_CONNECTION_UPSERT,        // connection: one changed entry (PATCH answer; never a fake full list)
  CCORD_NET_CONNECTION_DELETE,        // connection_name + connection_id (204 ack; drop that entry)
  // Guild scheduled events. LIST is the REST answer (replaces the guild's cached list); UPDATE covers gateway
  // GUILD_SCHEDULED_EVENT_CREATE / UPDATE / DELETE (scheduled_event_deleted says which); USER_CHANGE covers
  // GUILD_SCHEDULED_EVENT_USER_ADD / REMOVE (scheduled_event_user_added says which).
  CCORD_NET_SCHEDULED_EVENT_LIST,     // guild_id + scheduled_events + scheduled_event_count
  CCORD_NET_SCHEDULED_EVENT_UPDATE,   // guild_id + scheduled_events[0]; scheduled_event_deleted = true on DELETE
  CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, // guild_id, scheduled_event_id, user_id, scheduled_event_user_added
  CCORD_NET_MEMBER_VERIFICATION,      // guild_id + member_verification (GET /guilds/{g}/member-verification answer)
  // Embedded activities. SHELF is the REST answer (replaces the guild's cached shelf); INSTANCE_UPDATE covers gateway
  // EMBEDDED_ACTIVITY_UPDATE_V2 for one voice channel. participant_count == 0 means the instance ended.
  CCORD_NET_ACTIVITY_SHELF,           // guild_id + activity_apps + activity_app_count
  CCORD_NET_ACTIVITY_INSTANCE_UPDATE, // activity_instance (participant_count 0 = ended)
  CCORD_NET_MEDIA_RANGE,              // media_url (echo of req->url) + media_data/media_len (the bytes) + range_* ; status != OK = failed
  CCORD_NET_VOICE_LINK,               // voice_link, voice_link_attempt, voice_link_retry_ms, text (reason, static string)
  CCORD_NET_REPORT_MENU,              // expr_arg = report type name, expr_body/expr_body_len = the raw menu JSON (NUL terminated)
  CCORD_NET_REPORT_SUBMITTED,         // expr_arg = report type name, report_id (0 when the answer carried none)
  CCORD_NET_USER_SETTINGS_PROTO,      // settings_proto1 (+ _len, _partial): a type-1 USER_SETTINGS_PROTO_UPDATE with no guild folders;
                                      // a type-2 one carries raw_proto/raw_proto_len instead (+ settings_proto1_partial) and no settings_proto1
  CCORD_NET_SESSIONS_REPLACE,         // presence: the account's overall status from SESSIONS_REPLACE (the only place its own status is sent)
  CCORD_NET_GUILD_PROFILE,            // guild_id + guild_profile (GET /guilds/{g}/profile answer)
  CCORD_NET_VOICE_STATES,             // guild_id (0 = multi-guild) + voice_states + voice_state_count + replace_voice_states (+ optional users)
  CCORD_NET_VIDEO_RESOLVED,           // resolve_id + media_url (page) + resolved_video_url + resolved_audio_url + resolved_duration_ms; status != OK = failed
  CCORD_NET_RAW_GATEWAY,              // expr_arg = gateway event name ("t"), expr_body/expr_body_len = raw "d" JSON (when raw tap enabled)
  CCORD_NET_ACCOUNT_PRIVATE           // account_private (+ optional users[0] on USER_UPDATE): RAM-only private self-account metadata
} ccord_net_event_type;

// Voice connection health, as CCORD_NET_VOICE_LINK reports it.
typedef enum ccord_voice_link : uint8_t { CCORD_VLINK_CONNECTED, CCORD_VLINK_RECONNECTING, CCORD_VLINK_FAILED } ccord_voice_link;
// What the user asked for when CCORD_NET_ACTION_FAILED is reported. Actions run asynchronously: the call that
// started one returned CCORD_OK, so a later failure is delivered here instead of being lost.
typedef enum ccord_action_kind : uint8_t {
  CCORD_ACTION_NONE = 0,
  CCORD_ACTION_SEND,          // a message
  CCORD_ACTION_REPLY,
  CCORD_ACTION_EDIT,
  CCORD_ACTION_DELETE,
  CCORD_ACTION_REACTION,
  CCORD_ACTION_PIN,
  CCORD_ACTION_UPLOAD,        // a message with an attachment
  CCORD_ACTION_STICKER,
  CCORD_ACTION_OPEN_DM,
  CCORD_ACTION_RELATIONSHIP,  // friend request / accept / block / remove
  CCORD_ACTION_ADMIN,         // a server-administration call; ccord_net_event.admin_op says which
  CCORD_ACTION_POLL_VOTE,     // PUT /channels/{ch}/polls/{msg}/answers/@me
  CCORD_ACTION_INTERACTION,   // POST /interactions (component / modal)
  CCORD_ACTION_CREATE_POLL,   // POST /channels/{ch}/messages {"poll":...}; no draft: the UI keeps the form
  CCORD_ACTION_VOICE,         // voice connection given up; `text` = reason, no draft
  CCORD_ACTION_REPORT_MENU,   // GET  /reporting/menu/{type}; channel_id/message_id/user_id echo the target
  CCORD_ACTION_REPORT         // POST /reporting/{type}; same ids; no draft: the UI keeps the form
} ccord_action_kind;

typedef enum ccord_interaction_type : uint8_t {
  CCORD_INTERACTION_APPLICATION_COMMAND = 2,
  CCORD_INTERACTION_MESSAGE_COMPONENT = 3,
  CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE = 4,
  CCORD_INTERACTION_MODAL_SUBMIT = 5,
} ccord_interaction_type;

// One staged attachment for multi-file upload (up to CCORD_RICH_MAX_ATTACHMENTS per message).
typedef struct ccord_upload_file {
  const char *filename;
  const void *data;
  size_t size;
  bool spoiler;
  // Voice message: 0 for an ordinary file, else the clip length in ms and the message is flagged
  // CCORD_MSGFLAG_VOICE_MESSAGE with `waveform_b64` as the attachment waveform (base64, 256 bytes decoded at most;
  // nullptr/"" -> Discord draws no waveform). A voice file never degrades to the plain single-file call.
  uint32_t duration_ms;
  const char *waveform_b64;
} ccord_upload_file;

typedef enum ccord_upload_phase : uint8_t {
  CCORD_UPLOAD_SLOT = 0,       // requesting pre-signed upload slots
  CCORD_UPLOAD_PUT = 1,        // streaming attachment bytes to cloud storage
  CCORD_UPLOAD_FINALIZE = 2,   // posting message with uploaded attachment refs
} ccord_upload_phase;

typedef struct ccord_upload_progress {
  uint64_t upload_id;          // monotonic id per upload operation
  ccord_id channel_id;
  ccord_upload_phase phase;
  uint32_t file_index;         // 0-based index of the file currently uploading
  uint32_t file_count;         // total files in this message upload
  uint64_t uploaded_bytes;     // cumulative bytes uploaded across all files in this operation
  uint64_t total_bytes;        // total bytes across all files in this operation
  char filename[128];          // current (or primary) filename being uploaded
} ccord_upload_progress;

// A poll to post (POST /channels/{id}/messages {"poll":{...}}). Texts are UTF-8, NUL-terminated.
#define CCORD_POLL_QUESTION_MAX 300u   // characters Discord accepts in the question
#define CCORD_POLL_ANSWER_MAX 55u      // characters Discord accepts in one answer
#define CCORD_POLL_MIN_ANSWERS 2u
typedef struct ccord_poll_create {
  const char *question;
  const char *const *answers;
  size_t answer_count;            // CCORD_POLL_MIN_ANSWERS .. CCORD_RICH_MAX_POLL_ANSWERS
  uint32_t duration_hours;        // 1 .. 768 (Discord allows 1h to 32 days; the UI offers 1h .. 2 weeks)
  bool allow_multiselect;
} ccord_poll_create;

// Slash command, option autocomplete, component click/select or modal submission sent via POST /interactions.
typedef struct ccord_interaction_request {
  ccord_interaction_type type;
  ccord_id application_id;
  ccord_id guild_id;             // 0 or CCORD_DM_GUILD_ID in DMs
  ccord_id channel_id;
  ccord_id message_id;           // MESSAGE_COMPONENT
  uint32_t message_flags;        // MESSAGE_COMPONENT
  uint8_t component_type;        // ccord_component_kind (2, 3, 5, 6, 7, 8) for MESSAGE_COMPONENT
  const char *custom_id;         // component custom_id or modal custom_id
  const char *const *values;     // select menu chosen values (MESSAGE_COMPONENT, component_type != 2)
  size_t value_count;
  ccord_id modal_id;             // MODAL_SUBMIT: interaction id from INTERACTION_MODAL_CREATE (optional)
  const char *const *field_ids;  // MODAL_SUBMIT: custom_id per field
  const char *const *field_values; // MODAL_SUBMIT: UTF-8 value per field
  size_t field_count;
  ccord_id command_id;           // APPLICATION_COMMAND / AUTOCOMPLETE
  ccord_id command_version;      // APPLICATION_COMMAND / AUTOCOMPLETE (0 -> command_id)
  const char *command_name;      // APPLICATION_COMMAND / AUTOCOMPLETE
  const char *const *option_names;  // APPLICATION_COMMAND / AUTOCOMPLETE
  const uint8_t *option_kinds;      // ccord_slash_opt_kind per option
  const char *const *option_values; // UTF-8 text representation per option
  size_t option_count;
  int32_t focused_option;        // AUTOCOMPLETE: index in 0..option_count-1 (-1 otherwise)
  uint8_t command_type;          // APPLICATION_COMMAND: 0/1 = CHAT_INPUT, 2 = USER, 3 = MESSAGE (menu command)
  ccord_id target_id;            // APPLICATION_COMMAND type 2/3: the user / message the command runs on
  const char *subcommand_group;  // APPLICATION_COMMAND: SUB_COMMAND_GROUP name (nullptr / "" = none)
  const char *subcommand;        // APPLICATION_COMMAND: SUB_COMMAND name (nullptr / "" = none)
} ccord_interaction_request;
// Go Live signalling (gateway dispatches STREAM_CREATE / STREAM_SERVER_UPDATE / STREAM_UPDATE / STREAM_DELETE).
// Stream key format: "guild:<guild_id>:<channel_id>:<owner_id>" or "call:<channel_id>:<owner_id>".
typedef enum ccord_stream_signal_kind : uint8_t {
  CCORD_STREAM_SIG_CREATE = 1,   // stream exists; rtc_server_id / rtc_channel_id identify the stream media connection
  CCORD_STREAM_SIG_SERVER_UPDATE,// token + endpoint (+ session_id) to connect the stream media connection; endpoint "" = server gone
  CCORD_STREAM_SIG_UPDATE,       // paused / viewer count changed
  CCORD_STREAM_SIG_DELETE,       // stream ended (reason) or became unavailable
} ccord_stream_signal_kind;
typedef struct ccord_stream_signal {
  ccord_stream_signal_kind kind;
  const char *stream_key;
  bool is_call;                  // key type "call" (DM / group call) instead of "guild"
  ccord_id guild_id, channel_id, user_id, owner_id;   // parsed from the key (guild_id = 0 for calls)
  ccord_id rtc_server_id, rtc_channel_id;    // CREATE only
  const char *token;             // SERVER_UPDATE only; never logged
  const char *endpoint;          // SERVER_UPDATE only ("" when the payload endpoint is null)
  const char *session_id;        // SERVER_UPDATE: our main voice session id (filled in by the transport), "" if unknown
  const char *reason;            // DELETE only ("" if absent)
  bool server_update, deleted, paused, unavailable;
  uint32_t viewer_count;         // CREATE / UPDATE
  const ccord_id *viewer_ids;    // CREATE / UPDATE: arena-backed viewer ids (nullable)
  size_t viewer_id_count;
} ccord_stream_signal;
// REST-backed expression picker data (GIFs, sticker packs, FrecencyUserSettings proto).
typedef enum ccord_expr_kind : uint8_t {
  CCORD_EXPR_SETTINGS_PROTO,   // GET /users/@me/settings-proto/2          -> {"settings":"<base64 protobuf>"}
  CCORD_EXPR_GIF_CATEGORIES,   // GET /gifs/trending                       -> {"categories":[..],"gifs":[..]}
  CCORD_EXPR_GIF_TRENDING,     // GET /gifs/trending-gifs                  -> [gif, ...]
  CCORD_EXPR_GIF_SEARCH,       // GET /gifs/search?q=<arg>                 -> [gif, ...]
  CCORD_EXPR_GIF_SUGGEST,      // GET /gifs/suggest?q=<arg>                -> ["term", ...]
  CCORD_EXPR_STICKER_PACKS,    // GET /sticker-packs                       -> {"sticker_packs":[..]}
  CCORD_EXPR_KIND_COUNT
} ccord_expr_kind;
typedef struct ccord_net_event {
  ccord_net_event_type type;
  ccord_connection_state state;            // STATE
  const ccord_snapshot *snapshot;          // SNAPSHOT
  ccord_id channel_id;                     // HISTORY, MESSAGE_*, TYPING_START, CHANNEL_*
  const ccord_message *messages;           // HISTORY (ascending id) ; MESSAGE_CREATE/UPDATE: exactly 1
  size_t message_count;
  ccord_status status; const char *text;   // ERROR
  // Live state fields; unset fields are zero/null.
  ccord_id guild_id;                       // TYPING_START, CHANNEL_*, MESSAGE_UPDATE/DELETE, REACTION_UPDATE
  ccord_id message_id;                     // MESSAGE_DELETE, REACTION_UPDATE, MESSAGE_PIN_UPDATE
  ccord_id user_id;                        // PRESENCE_UPDATE, TYPING_START, REACTION_UPDATE
  const ccord_channel *channel;            // CHANNEL_UPSERT
  ccord_presence presence;                 // PRESENCE_UPDATE
  const char *status_text;                 // PRESENCE_UPDATE (nullable)
  uint32_t reaction_count;                 // REACTION_UPDATE
  bool self_reacted;                       // REACTION_UPDATE
  int64_t timestamp_ms;                    // TYPING_START expiry (unix ms)
  const ccord_voice_state *voice_state;    // VOICE_STATE_UPDATE
  const ccord_voice_server *voice_server;  // VOICE_SERVER_UPDATE
  const ccord_stream_state *stream;        // STREAM_UPDATE
  bool pinned;                             // MESSAGE_PIN_UPDATE
  const char *reaction_emoji;              // REACTION_UPDATE (nullable when the payload has no emoji)
  const ccord_guild *guild;                // GUILD_UPSERT
  const ccord_channel *guild_channels;     // GUILD_UPSERT
  size_t guild_channel_count;
  const ccord_member *guild_members;       // GUILD_UPSERT / GUILD_MEMBERS
  size_t guild_member_count;
  // GUILD_MEMBERS from a member list: how many members the whole list holds and how many are online (0 = not told), and
  // the size of each group in it. Borrowed; the session hands them to the store.
  uint32_t member_total, member_online;
  uint32_t member_synced_to;               // ... and how far down the list a SYNC of ranges in it reached (positions; 0 = none)
  const ccord_member_group *member_groups;
  size_t member_group_count;
  const ccord_user *users;                 // GUILD_UPSERT / GUILD_MEMBERS (presences)
  size_t user_count;
  const char *media_url;                   // MEDIA_LOADED
  const char *media_path;                  // MEDIA_LOADED (disk cache path)
  const uint8_t *media_data;               // MEDIA_LOADED
  size_t media_len;                        // MEDIA_LOADED
  int32_t reaction_delta;                  // REACTION_UPDATE (+1/-1 delta when count not authoritative)
  const ccord_id *message_ids;             // MESSAGE_DELETE_BULK
  size_t message_id_count;                 // MESSAGE_DELETE_BULK
  ccord_relationship_kind relationship_kind; // RELATIONSHIP_UPDATE
  uint32_t mention_count;                  // MESSAGE_ACK
  const ccord_user_profile *user_profile;  // USER_PROFILE
  const ccord_stream_signal *stream_signal; // STREAM_SIGNAL
  // Forum channels (see ccord/model/forum.h). Metas ride on SNAPSHOT / GUILD_UPSERT / CHANNEL_UPSERT; posts on
  // SNAPSHOT / GUILD_UPSERT (active threads) / CHANNEL_UPSERT (thread events) / FORUM_POSTS / FORUM_POST_CREATED.
  const ccord_forum_meta *forum_metas;
  size_t forum_meta_count;
  const ccord_forum_post *forum_posts;
  size_t forum_post_count;
  uint32_t forum_total;                    // FORUM_POSTS: total_results (0 = unknown)
  uint32_t forum_offset;                   // FORUM_POSTS / FORUM_FAILED: offset of the page
  bool forum_has_more;                     // FORUM_POSTS: the server reported another page
  bool forum_creating;                     // FORUM_FAILED: the failed request was a post creation
  const ccord_emoji *emojis;               // SNAPSHOT / GUILD_UPSERT (any guild, each carries guild_id) / GUILD_EXPRESSIONS / EMOJI_UPSERT
  size_t emoji_count;
  const ccord_sticker *stickers;           // same events as emojis / STICKER_UPSERT
  size_t sticker_count;
  const ccord_soundboard_sound *soundboard; // SNAPSHOT / GUILD_UPSERT / GUILD_EXPRESSIONS / SOUNDBOARD_UPSERT
  size_t soundboard_count;
  bool replace_emojis, replace_stickers, replace_soundboard; // GUILD_EXPRESSIONS: which of the lists replace the stored ones
  bool expressions_admin;                  // GUILD_EXPRESSIONS: authoritative admin list (with uploader metadata)
  ccord_id expr_id;                        // *_DELETE / ACTION_FAILED: emoji, sticker, or soundboard id
  ccord_expr_kind expr_kind;               // EXPRESSION_DATA
  const char *expr_arg;                    // EXPRESSION_DATA (nullable): the request argument (search query)
  const char *expr_body;                   // EXPRESSION_DATA: response body (NUL terminated, expr_body_len bytes)
  size_t expr_body_len;
  const uint8_t *raw_proto;                // EXPRESSION_DATA: raw protobuf bytes (SETTINGS_PROTO), nullable
  // Raw type-1 PreloadedUserSettings bytes (nullable, settings_proto1_len 0 = none): USER_SETTINGS_PROTO (and GUILD_LAYOUT
  // when the same update also carries folders) from a gateway USER_SETTINGS_PROTO_UPDATE of type 1, and SNAPSHOT from READY's
  // user_settings_proto. _partial = the update said "partial": the bytes are a delta, not the whole proto.
  const uint8_t *settings_proto1;
  size_t settings_proto1_len;
  bool settings_proto1_partial;
  size_t raw_proto_len;                    // EXPRESSION_DATA: byte count of raw_proto (0 = none)
  bool speaking;                           // SPEAKING
  ccord_action_kind action;                // ACTION_FAILED
  const char *draft;                       // ACTION_FAILED: the text the user wrote (SEND/REPLY/EDIT/UPLOAD), nullable
  size_t draft_len;
  char action_nonce[32];                   // ACTION_FAILED: nonce of the failed send ("" when unknown/legacy);
                                           // lets the session fail exactly the pending echo this refusal belongs to
  ccord_admin_op admin_op;                 // ACTION_FAILED with action == CCORD_ACTION_ADMIN (guild_id/channel_id/user_id say what)
  // Permission data (see ccord/model/permissions.h). Roles ride on SNAPSHOT / GUILD_UPSERT (each role carries its guild_id,
  // grouped by guild) and ROLE_UPSERT; member role ids on SNAPSHOT / GUILD_UPSERT / GUILD_MEMBERS; channel overwrites on
  // SNAPSHOT / GUILD_UPSERT / CHANNEL_UPSERT. A payload that does not say (no "roles" key) leaves these empty, never wrong.
  const ccord_role *roles;
  size_t role_count;
  const ccord_member_roles *member_roles;
  size_t member_role_count;
  const ccord_channel_overwrites *overwrites;
  size_t overwrite_count;
  ccord_id role_id;                        // ROLE_DELETE / MEMBER_ROLE_CHANGE
  bool role_added;                         // MEMBER_ROLE_CHANGE: true = given, false = taken away
  // Server Settings lists, delivered only when the user opened the matching page (Discord's own answer, never pushed).
  const ccord_ban *bans;                   // BAN_LIST
  size_t ban_count;
  const ccord_invite *invites;             // INVITE_LIST / INVITE_UPSERT
  size_t invite_count;
  const ccord_audit_entry *audit;          // AUDIT_LOG
  size_t audit_count;
  const char *invite_code;                 // INVITE_DELETE
  // Notification settings (see ccord/model/notify.h). SNAPSHOT carries every server's rows (grouped by guild_id); NOTIFY_SETTINGS
  // carries one server's (guild_id; zero rows = back to defaults). Each guild's rows replace what was known of it.
  const ccord_notify_scope *notify;
  size_t notify_count;
  uint32_t poll_answer_id;                 // POLL_VOTE: answer_id
  bool poll_vote_added;                    // POLL_VOTE: true = MESSAGE_POLL_VOTE_ADD, false = REMOVE
  const ccord_interaction_modal *modal;    // INTERACTION_MODAL
  const ccord_slash_command *slash_commands; // SLASH_COMMANDS
  size_t slash_command_count;
  const ccord_slash_choice *slash_choices; // SLASH_AUTOCOMPLETE
  size_t slash_choice_count;
  const char *interaction_nonce;           // SLASH_AUTOCOMPLETE (nullable)
  uint32_t search_total_results;           // SEARCH_RESULTS: Discord's total_results
  uint32_t search_offset;                  // SEARCH_RESULTS: requested offset
  bool pins_has_more;                      // PIN_LIST: server reported has_more
  bool pins_append;                        // PIN_LIST: true when fetched with a before cursor
  const char *pins_next_cursor;            // PIN_LIST: cursor for the next page (pinned_at ISO or snowflake), nullable
  bool reaction_burst;                     // REACTION_UPDATE: super/burst reaction
  bool thread_joined;                      // THREAD_MEMBER_SELF: whether self is now joined to channel_id
  const ccord_guild_folder_item *folder_items; // GUILD_LAYOUT / SNAPSHOT
  size_t folder_item_count;
  const ccord_id *reorder_ids;             // CHANNEL_POSITIONS
  const int32_t *reorder_positions;        // CHANNEL_POSITIONS
  const ccord_id *reorder_parents;         // CHANNEL_POSITIONS
  const uint8_t *reorder_has_parent;       // CHANNEL_POSITIONS
  size_t reorder_count;                    // CHANNEL_POSITIONS
  const ccord_invite_preview *invite_preview; // INVITE_PREVIEW / INVITE_ACCEPTED
  const ccord_discover_guild *discovery_guilds; // DISCOVERY_RESULTS
  size_t discovery_count;
  uint32_t discovery_total;
  uint32_t discovery_offset;
  uint32_t discovery_category_id;
  const char *discovery_query;             // DISCOVERY_RESULTS (nullable)
  bool recipient_added;                    // DM_RECIPIENT_CHANGE: true = added, false = removed
  int64_t invites_disabled_until_ms;       // GUILD_INCIDENTS (0 = unpaused)
  int64_t dms_disabled_until_ms;           // GUILD_INCIDENTS (0 = unpaused)
  ccord_prune_info prune_info;             // PRUNE_RESULT
  const ccord_automod_rule *automod_rules; // AUTOMOD_LIST / AUTOMOD_UPSERT
  size_t automod_rule_count;
  ccord_id target_channel_id;              // CHANNEL_FOLLOWER_ADDED: destination webhook_channel_id
  const ccord_webhook *webhooks;           // WEBHOOK_LIST / WEBHOOK_UPSERT
  size_t webhook_count;
  const ccord_app_cmd_perms *app_cmd_perms; // APP_CMD_PERMS_LIST
  size_t app_cmd_perms_count;
  const ccord_welcome_screen *welcome_screen; // WELCOME_SCREEN
  const ccord_onboarding *onboarding;      // ONBOARDING
  const ccord_member_verification *member_verification; // MEMBER_VERIFICATION (GET form answer; valid until next poll)
  ccord_overwrite overwrite_item;          // CHANNEL_OVERWRITE_UPSERT / CHANNEL_OVERWRITE_DELETE
  const ccord_call_state *call;            // CALL_CREATE / CALL_UPDATE
  const ccord_stage_instance *stage_instance; // STAGE_INSTANCE_UPDATE
  bool stage_deleted;                      // STAGE_INSTANCE_UPDATE: true = STAGE_INSTANCE_DELETE
  const ccord_scheduled_event *scheduled_events; // SCHEDULED_EVENT_LIST / SCHEDULED_EVENT_UPDATE
  size_t scheduled_event_count;
  ccord_id scheduled_event_id;             // SCHEDULED_EVENT_UPDATE (delete) / SCHEDULED_EVENT_USER_CHANGE
  bool scheduled_event_deleted;            // SCHEDULED_EVENT_UPDATE: true = GUILD_SCHEDULED_EVENT_DELETE
  bool scheduled_event_user_added;         // SCHEDULED_EVENT_USER_CHANGE: true = USER_ADD, false = USER_REMOVE
  const ccord_activity_app *activity_apps; // ACTIVITY_SHELF: the guild's activities in shelf order
  size_t activity_app_count;
  const char *voice_status;                // VOICE_STATUS_UPDATE (nullptr / "" = cleared)
  // M6 account settings, devices & connections.
  const ccord_user_settings *user_settings; // USER_SETTINGS: full object (REST answer or gateway push)
  const ccord_auth_session *auth_sessions;  // AUTH_SESSIONS
  size_t auth_session_count;
  const ccord_connection *connections;      // CONNECTIONS
  size_t connection_count;
  // Mutually exclusive event-specific value payloads (anonymous union preserves every field name).
  union {
    ccord_guild_profile guild_profile;          // GUILD_PROFILE (GET /guilds/{g}/profile answer)
    ccord_activity_instance activity_instance;  // ACTIVITY_INSTANCE_UPDATE (value copy; participant_count 0 = ended)
    struct {
      char session_hashes[CCORD_ADMIN_SESSION_LOGOUT_MAX][64]; // AUTH_SESSIONS_LOGOUT: hashes the server logged out
      size_t session_hash_count;
    };
    ccord_connection connection;                // CONNECTION_UPSERT: the changed entry (value copy)
    struct {
      char connection_name[32];                 // CONNECTION_DELETE: service type (the request path segment)
      char connection_id[64];                   // CONNECTION_DELETE: account id
    };
    struct {
      bool has_presence_detail;                 // PRESENCE_UPDATE / SESSIONS_REPLACE: activity & custom_status fields below are authoritative
      uint8_t client_status_bits;               // PRESENCE_UPDATE / SESSIONS_REPLACE: packed desktop/mobile/web ccord_presence
      char custom_status_emoji[65];             // PRESENCE_UPDATE / CUSTOM_STATUS_UPDATE
      char custom_status_text[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
      int64_t custom_status_expires_ms;         // PRESENCE_UPDATE / CUSTOM_STATUS_UPDATE (0 = none)
      bool custom_status_clear;                 // CUSTOM_STATUS_UPDATE: true when custom_status cleared to null
      bool has_activity;                        // PRESENCE_UPDATE / SESSIONS_REPLACE: non-custom rich presence activity present
      ccord_activity_type activity_type;
      char activity_name[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
      char activity_details[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
      char activity_state[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
      int64_t activity_start_ms;
      int64_t activity_end_ms;
      const ccord_user_activities *activities_ext; // PRESENCE_UPDATE / SESSIONS_REPLACE: full activities array (arena-backed, nullable)
    };
    struct {
      uint64_t range_id;         // MEDIA_RANGE: the id from ccord_media_range_req
      int64_t  range_start;      // MEDIA_RANGE: file offset of media_data[0]
      int64_t  range_total;      // MEDIA_RANGE: whole-file length, -1 unknown
      uint32_t range_flags;      // MEDIA_RANGE: ccord_range_flags
      int32_t  range_http;       // MEDIA_RANGE: final HTTP status after redirects, 0 when none
      uint8_t  range_redirects;  // MEDIA_RANGE: hops followed
      char     range_etag[96];   // MEDIA_RANGE: strong ETag ("" if none or weak)
    };
    struct {
      uint64_t    resolve_id;              // VIDEO_RESOLVED: the req_id passed to resolve_video
      const char *resolved_video_url;      // VIDEO_RESOLVED: direct HTTPS video URL ("" on failure)
      const char *resolved_audio_url;      // VIDEO_RESOLVED: optional direct HTTPS audio URL ("" when muxed)
      uint32_t    resolved_duration_ms;    // VIDEO_RESOLVED: duration in ms (0 when unknown)
      bool        resolved_missing_helper; // VIDEO_RESOLVED: true when external helper (yt-dlp) is not installed
    };
  };
  // Voice link (CCORD_NET_VOICE_LINK). Appended last; value-initialised everywhere.
  uint8_t  voice_link;          // VOICE_LINK: ccord_voice_link
  uint32_t voice_link_attempt;  // VOICE_LINK: 1-based attempt number (0 when none)
  uint32_t voice_link_retry_ms; // VOICE_LINK: delay before that attempt
  ccord_id report_id;        // REPORT_SUBMITTED: the report id (0 when the answer carried none)
  const ccord_voice_state *voice_states; // VOICE_STATES / GUILD_UPSERT: batch voice states
  size_t voice_state_count;              // VOICE_STATES / GUILD_UPSERT
  bool replace_voice_states;             // VOICE_STATES / GUILD_UPSERT: true = authoritative full list for guild_id
  const ccord_account_private *account_private; // ACCOUNT_PRIVATE: RAM-only self-account metadata
} ccord_net_event;                         // all pointers valid only until the next poll() on that transport

typedef enum ccord_range_flags : uint32_t {
  CCORD_RANGE_ACCEPTS = 1u << 0,   // 206 or Accept-Ranges: bytes seen
  CCORD_RANGE_UNRANGED = 1u << 1,  // the server answered 200: Range ignored, media_data is empty
  CCORD_RANGE_EOF = 1u << 2,       // the body reaches the end of the file
} ccord_range_flags;

#define CCORD_STREAM_RANGE_MAX_BYTES (1024u * 1024u)
typedef struct ccord_media_range_req {
  uint64_t id;               // unique per request, never reused within a session; echoed back
  const char *url;           // https URL exactly as in the message (query included); copied
  int64_t start;             // >= 0
  size_t len;                // 1 .. CCORD_STREAM_RANGE_MAX_BYTES
  const char *if_range;      // strong ETag from an earlier response, or nullptr
} ccord_media_range_req;
typedef struct ccord_transport ccord_transport;
// Optional hooks may be nullptr; the wrappers below return CCORD_ERR_UNSUPPORTED then.
// Remote voice cameras. The three limits mirror CCORD_VOICE_CAM_* in voice_camera.h (live_transport.c and the net
// tests assert they match); the mock applies the same admission rule with them.
#define CCORD_CAM_RX_MAX     16u              // tracked senders (== CCORD_VOICE_CAM_MAX)
#define CCORD_CAM_DECODE_MAX 4u               // senders whose frames are delivered (== CCORD_VOICE_CAM_DECODE_MAX)
#define CCORD_CAM_FRAME_MAX  (512u * 1024u)   // largest access unit (== CCORD_VOICE_CAM_AU_MAX)
typedef struct ccord_camera_rx {
  ccord_id user_id;
  uint32_t generation;   // never 0; changes whenever the unit stream of this user is discontinuous
  bool decoding;         // false = waiting for one of the CCORD_CAM_DECODE_MAX slots
} ccord_camera_rx;

#define CCORD_WAIT_MAX_FDS 16u
typedef struct ccord_wait_info {
  ccord_socket_t fds[CCORD_WAIT_MAX_FDS];   // readable sockets that should wake an idle/background poll()
  size_t fd_count;
  uint32_t timeout_ms;                      // max ms before poll() must run again (UINT32_MAX = no timer)
  bool immediate;                           // true: already-queued or buffered work must be pumped immediately
} ccord_wait_info;

typedef struct ccord_transport_vtable {
  const char *name;
  ccord_status (*connect)(ccord_transport *t, ccord_error *err);          // result arrives via poll(): STATE then SNAPSHOT
  void (*disconnect)(ccord_transport *t);
  ccord_status (*request_history)(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err);   // -> HISTORY
  ccord_status (*send_message)(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len, ccord_error *err);  // -> MESSAGE_CREATE (echo)
  bool (*poll)(ccord_transport *t, ccord_net_event *out);                 // pop next ready event; false = none
  void (*destroy)(ccord_transport *t);
  ccord_status (*edit_message)(ccord_transport *t, ccord_id channel_id, ccord_id message_id, const char *utf8, size_t len, ccord_error *err);  // -> MESSAGE_UPDATE
  ccord_status (*delete_message)(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err);  // -> MESSAGE_DELETE
  ccord_status (*send_typing)(ccord_transport *t, ccord_id channel_id, ccord_error *err);                         // -> TYPING_START
  ccord_status (*toggle_reaction)(ccord_transport *t, ccord_id channel_id, ccord_id message_id, const char *emoji, size_t len, bool add, ccord_error *err); // -> REACTION_UPDATE
  ccord_status (*ack_message)(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err);     // mark read
  ccord_status (*select_guild)(ccord_transport *t, ccord_id guild_id, ccord_error *err);                           // subscribe/presence scope
  ccord_status (*join_voice)(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err);        // -> VOICE_STATE_UPDATE / VOICE_SERVER_UPDATE
  ccord_status (*leave_voice)(ccord_transport *t, ccord_id guild_id, ccord_error *err);                           // -> VOICE_STATE_UPDATE (channel 0)
  ccord_status (*update_voice_state)(ccord_transport *t, ccord_id guild_id, bool self_mute, bool self_deaf, ccord_error *err); // -> VOICE_STATE_UPDATE
  ccord_status (*start_stream)(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, const ccord_stream_config *config, ccord_error *err); // -> STREAM_UPDATE
  ccord_status (*stop_stream)(ccord_transport *t, ccord_id guild_id, ccord_error *err);                           // -> STREAM_UPDATE (inactive)
  ccord_status (*send_reply)(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id, const char *utf8, size_t len, ccord_error *err); // -> MESSAGE_CREATE
  ccord_status (*toggle_pin)(ccord_transport *t, ccord_id channel_id, ccord_id message_id, bool pinned, ccord_error *err); // -> MESSAGE_PIN_UPDATE
  // Non-blocking REST intents. Results arrive through poll() as HISTORY/PIN_LIST.
  ccord_status (*request_history_before)(ccord_transport *t, ccord_id channel_id, ccord_id before_id, uint32_t limit, ccord_error *err);
  ccord_status (*request_pins)(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err);
  // Gateway Opcode 3 PRESENCE_UPDATE; status_text may be null/empty.
  ccord_status (*set_presence)(ccord_transport *t, ccord_presence presence, const char *status_text, ccord_error *err);
  // Camera/screen signalling; requires an active voice connection in the guild.
  ccord_status (*set_voice_camera)(ccord_transport *t, ccord_id guild_id, bool enabled, ccord_error *err);
  // 3-step file/image attachment upload + message send.
  ccord_status (*send_message_with_file)(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                         const char *filename, const void *file_data, size_t file_size,
                                         ccord_error *err);
  // Async CDN media download + disk cache in $XDG_CACHE_HOME/ccord/media/.
  ccord_status (*fetch_media)(ccord_transport *t, const char *url, ccord_error *err);
  // Optional. Same download for a URL the user asked for by clicking a video: a transport may accept third-party
  // https hosts here that fetch_media (automatic loads) keeps refused. Null: ccord_transport_fetch_media_user
  // falls back to fetch_media.
  ccord_status (*fetch_media_user)(ccord_transport *t, const char *url, ccord_error *err);
  // Server-side message search (GET /guilds/{id}/messages/search or /channels/{id}/messages/search).
  ccord_status (*search_messages)(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                  const char *query, size_t query_len, ccord_error *err);
  // Direct-message channel creation (POST /users/@me/channels).
  ccord_status (*open_dm)(ccord_transport *t, ccord_id recipient_id, ccord_error *err);
  // Real-time 48kHz stereo S16LE voice audio capture/playback hooks.
  ccord_status (*push_voice_pcm)(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err);
  size_t (*pull_voice_pcm)(ccord_transport *t, int16_t *pcm, size_t max_samples);
  // Friend/block relationship mutations (PUT/DELETE /users/@me/relationships/{id}).
  ccord_status (*set_relationship)(ccord_transport *t, ccord_id user_id, ccord_relationship_kind kind, ccord_error *err);
  // On-demand user profile fetch (GET /users/{id}/profile -> CCORD_NET_USER_PROFILE).
  ccord_status (*request_profile)(ccord_transport *t, ccord_id user_id, ccord_error *err);
  // Gateway Opcode 14 channel member list range subscription.
  ccord_status (*select_channel)(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err);
  // More of the open channel's member list: the first `chunks` hundreds of positions. Results arrive as GUILD_MEMBERS.
  ccord_status (*request_members)(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, uint32_t chunks, ccord_error *err);
  // GET /channels/{id}: resolves a channel the client has not seen (e.g. a group DM that only
  // surfaced through MESSAGE_CREATE). Result arrives as CHANNEL_UPSERT.
  ccord_status (*request_channel)(ccord_transport *t, ccord_id channel_id, ccord_error *err);
  // Go Live viewing (op 20 STREAM_WATCH / op 19 STREAM_DELETE / op 22 STREAM_SET_PAUSED). Results: STREAM_SIGNAL events.
  ccord_status (*watch_stream)(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id, ccord_error *err);
  ccord_status (*stop_watching)(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id, ccord_error *err);
  ccord_status (*set_stream_paused)(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id, bool paused, ccord_error *err);
  // GET /channels/{forum}/threads/search (one page of posts) -> FORUM_POSTS, or FORUM_FAILED.
  ccord_status (*request_forum_posts)(ccord_transport *t, const ccord_forum_query *query, ccord_error *err);
  // POST /channels/{forum}/threads {name, message:{content}, applied_tags} -> FORUM_POST_CREATED, or FORUM_FAILED.
  ccord_status (*create_forum_post)(ccord_transport *t, ccord_id forum_id, const char *title, size_t title_len,
                                    const char *content, size_t content_len, const ccord_id *tag_ids, size_t tag_count,
                                    ccord_error *err);
  // Expression picker REST data -> EXPRESSION_DATA event (arg = search query for GIF_SEARCH/GIF_SUGGEST).
  ccord_status (*request_expression_data)(ccord_transport *t, ccord_expr_kind kind, const char *arg, size_t arg_len, ccord_error *err);
  // POST /channels/{id}/messages {"sticker_ids":[id]} -> MESSAGE_CREATE.
  ccord_status (*send_sticker)(ccord_transport *t, ccord_id channel_id, ccord_id sticker_id, ccord_error *err);
  // Server administration (see ccord/net/admin.h). Returns once the request is queued; the result arrives through
  // poll(): the usual state events (CHANNEL_UPSERT, CHANNEL_DELETE, GUILD_UPSERT, GUILD_MEMBERS, MEMBER_REMOVE) on
  // success, CCORD_NET_ACTION_FAILED (action ADMIN) otherwise.
  ccord_status (*admin_action)(ccord_transport *t, const ccord_admin_request *req, ccord_error *err);
  // PUT /channels/{channel_id}/polls/{message_id}/answers/@me {"answer_ids":[...]} (0 answers clears one's vote).
  ccord_status (*vote_poll)(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                            const uint32_t *answer_ids, size_t answer_count, ccord_error *err);
  // POST /interactions (slash commands, option autocomplete, component clicks/selects and modal submissions).
  ccord_status (*send_interaction)(ccord_transport *t, const ccord_interaction_request *req, ccord_error *err);
  // Reply with explicit mention_author toggle (allowed_mentions.replied_user).
  ccord_status (*send_reply_ex)(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id,
                                bool mention_author, const char *utf8, size_t len, ccord_error *err);
  // Multi-file 3-step cloud attachment upload + optional reply reference.
  ccord_status (*send_message_with_files)(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id,
                                          bool mention_author, const char *utf8, size_t len,
                                          const ccord_upload_file *files, size_t file_count, ccord_error *err);
  // POST /channels/{channel_id}/messages with a poll body (see ccord_poll_build_body). Returns once the request is
  // queued; a refusal arrives later as CCORD_NET_ACTION_FAILED (action SEND).
  ccord_status (*create_poll)(ccord_transport *t, ccord_id channel_id, const ccord_poll_create *poll, ccord_error *err);
  // GET /channels/{channel_id}/application-command-index -> SLASH_COMMANDS.
  ccord_status (*request_slash_commands)(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err);
  // Structured server-side search with operators & offset pagination -> SEARCH_RESULTS.
  ccord_status (*search_messages_ex)(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                     const ccord_search_query *query, ccord_error *err);
  // Paginated channel pins before a cursor (GET /channels/{id}/messages/pins?limit=N&before=<cursor>) -> PIN_LIST.
  ccord_status (*request_pins_before)(ccord_transport *t, ccord_id channel_id, const char *before_cursor,
                                      uint32_t limit, ccord_error *err);
  // GET /channels/{channel_id}/messages/{message_id}/reactions/{emoji}?limit=N -> REACTION_USERS.
  ccord_status (*request_reaction_users)(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                         const char *emoji, size_t emoji_len, uint32_t limit, ccord_error *err);
  // GET /discoverable-guilds?offset=...&limit=... -> DISCOVERY_RESULTS.
  ccord_status (*request_discoverable_guilds)(ccord_transport *t, const char *query, uint32_t category_id,
                                              uint32_t offset, uint32_t limit, ccord_error *err);
  // Context-aware user profile fetch (GET /users/{id}/profile?with_mutual_guilds=true&with_mutual_friends=true... [&guild_id=...]).
  ccord_status (*request_profile_ex)(ccord_transport *t, ccord_id user_id, ccord_id guild_id, ccord_error *err);
  // Gateway Opcode 3 PRESENCE_UPDATE with optional custom status emoji + text.
  ccord_status (*set_presence_ex)(ccord_transport *t, ccord_presence presence, const char *emoji_name,
                                  ccord_id emoji_id, const char *status_text, ccord_error *err);
  // M5 voice & video. Ringing is passive: CALL_CREATE arrives unprompted.
  ccord_status (*call_action)(ccord_transport *t, ccord_id channel_id,
                              ccord_call_action act, ccord_error *err); // JOIN/LEAVE a DM/group call
  ccord_status (*play_soundboard)(ccord_transport *t, ccord_id guild_id,
                                  ccord_id sound_id, ccord_error *err);
  ccord_status (*set_user_volume)(ccord_transport *t, ccord_id user_id,
                                  uint8_t pct, ccord_error *err);
  ccord_status (*set_voice_config)(ccord_transport *t, const ccord_voice_config *cfg,
                                   ccord_error *err);
  // M0-3: plain-text send carrying a client nonce. The nonce lets the session
  // confirm its pending local echo and lets Discord dedupe a retry. Optional: the wrapper falls back to
  // send_message when null (the nonce is then not transmitted). Replies/uploads/stickers keep their legacy paths.
  ccord_status (*send_message_with_nonce)(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                          const char *nonce, size_t nonce_len, ccord_error *err);
  // Go Live media plane: the outbound H.264 Annex-B access units and the 48 kHz mono
  // S16LE stream audio of an active broadcast. send_* queue media for the transport (the mock hands it back through
  // poll_* so tests can inspect it; a real transport feeds the RTC peer connection).
  ccord_status (*send_stream_frame)(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k,
                                    bool keyframe, ccord_error *err);
  bool (*poll_stream_frame)(ccord_transport *t, uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t *out_ts90k,
                            bool *out_keyframe);
  ccord_status (*send_stream_pcm)(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err);
  size_t (*poll_stream_pcm)(ccord_transport *t, int16_t *out_pcm, size_t cap_samples);
  // Standalone voice camera H.264 TX (over the main voice connection) + keyframe request polling (ESTABLISHED / PLI / FIR).
  ccord_status (*send_voice_video)(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k,
                                   bool keyframe, ccord_error *err);
  bool (*consume_keyframe_request)(ccord_transport *t);
  // Remote voice cameras. camera_senders lists the senders announced in the
  // current voice call (arrival order, at most `max`); only `decoding` ones (at most CCORD_CAM_DECODE_MAX) have their
  // access units queued. poll_camera_frame pops the oldest queued H.264 Annex-B access unit of one decoding sender.
  size_t (*camera_senders)(ccord_transport *t, ccord_camera_rx *out, size_t max);
  bool (*poll_camera_frame)(ccord_transport *t, ccord_id user_id, uint8_t *buf, size_t buf_cap, size_t *out_len,
                            uint32_t *out_ts90k, bool *out_keyframe);
  // Messaging parity. send_tts_message: POST /channels/{c}/messages with tts=true -> MESSAGE_CREATE.
  ccord_status (*send_tts_message)(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                   ccord_error *err);
  // forward_message: POST /channels/{c}/messages with message_reference type 1 (forward) pointing at the source
  // message -> MESSAGE_CREATE. crosspost_message: POST /channels/{c}/messages/{m}/crosspost (announcement
  // channels) -> MESSAGE_UPDATE with CCORD_MSGFLAG_CROSSPOSTED.
  ccord_status (*forward_message)(ccord_transport *t, ccord_id channel_id, ccord_id source_channel_id,
                                  ccord_id message_id, ccord_error *err);
  ccord_status (*crosspost_message)(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err);
  // mark_unread: POST /channels/{c}/messages/{m}/ack for the message BEFORE `message_id` (Discord marks unread
  // from a message by acknowledging the one preceding it), so `message_id` and everything after it stay unread.
  ccord_status (*mark_unread)(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err);
  // patch_settings_proto: PATCH /users/@me/settings-proto/2 {"settings":"<base64 of proto>"} (FrecencyUserSettings,
  // lossless re-encode) -> EXPRESSION_DATA (SETTINGS_PROTO) carrying the server's answer.
  ccord_status (*patch_settings_proto)(ccord_transport *t, const uint8_t *proto, size_t proto_len, ccord_error *err);
  // set_status_proto: writes the account's status and custom status through PATCH /users/@me/settings-proto/1 so
  // other clients follow. `st` is the desired state of status, custom status and status expiry (see
  // ccord_user_status_patch). The transport builds the PATCH on the account's current type-1 bytes (READY's proto,
  // a gateway update, else GET /users/@me/settings-proto/1) so every other field survives. Optional.
  ccord_status (*set_status_proto)(ccord_transport *t, const ccord_proto_status *st, ccord_error *err);
  // Rich Presence (Discord RPC SET_ACTIVITY): a validated JSON array of activities spliced into every gateway
  // op 3 after the custom status (omitted while invisible). nullptr/0 clears. Optional.
  ccord_status (*set_rich_activities)(ccord_transport *t, const char *json, size_t len, ccord_error *err);
  // Optional. Ranged download of one byte range of a clicked video; the answer is a CCORD_NET_MEDIA_RANGE event.
  ccord_status (*fetch_media_range)(ccord_transport *t, const ccord_media_range_req *req, ccord_error *err);
  // Optional. The machine woke from sleep or the network changed (CCORD_LINK_RESUMED / CCORD_LINK_CHANGED): the
  // transport should drop connections the event may have killed and reconnect now instead of waiting for a missed
  // heartbeat. Main thread, never blocks; sends no request. Nothing to do (not connected) is still CCORD_OK.
  ccord_status (*network_event)(ccord_transport *t, ccord_link_event kind, ccord_error *err);
  // Optional. GET /reporting/menu/{type}: queued; the answer is CCORD_NET_REPORT_MENU, a refusal CCORD_NET_ACTION_FAILED (REPORT_MENU).
  ccord_status (*request_report_menu)(ccord_transport *t, ccord_report_type type, ccord_error *err);
  // Optional. POST /reporting/{type}: queued; the answer is CCORD_NET_REPORT_SUBMITTED, a refusal CCORD_NET_ACTION_FAILED (REPORT).
  ccord_status (*submit_report)(ccord_transport *t, const ccord_report_request *req, ccord_error *err);
  // Optional. Active attachment/media uploads in flight (arrival order, writes up to `max` into `out` when non-null,
  // always returns the total active count). Main thread, non-blocking.
  size_t (*active_uploads)(ccord_transport *t, ccord_upload_progress *out, size_t max);
  // Optional. Resolves a third-party video page URL (YouTube, X/Twitter, etc.) into direct HTTPS stream URL(s).
  // Answer arrives through poll() as CCORD_NET_VIDEO_RESOLVED echoing `req_id` and `url`.
  ccord_status (*resolve_video)(ccord_transport *t, uint64_t req_id, const char *url, ccord_error *err);
  // Optional. Reports readable sockets and the next timer deadline so an idle/background main loop can sleep in
  // kernel poll() instead of waking at a fixed interval. Main thread, non-blocking.
  bool (*wait_info)(const ccord_transport *t, ccord_wait_info *out);
  // Optional. Toggles background efficiency mode (unsubscribes OP 14 typing/presence/member-list streams, raises
  // reconnect backoff cap) and background power-saving suspend (closes socket while preserving session for RESUME).
  void (*set_background)(ccord_transport *t, bool background);
  void (*set_suspended)(ccord_transport *t, bool suspended);
  // Optional. Live download byte progress for an in-flight fetch_media(url). Returns true while active.
  bool (*media_progress)(const ccord_transport *t, const char *url, uint64_t *out_now, uint64_t *out_total);
  // Optional. POST /channels/{id}/greet {"sticker_ids":[id], "message_reference":{...}} -> MESSAGE_CREATE.
  ccord_status (*send_greet)(ccord_transport *t, ccord_id channel_id, ccord_id sticker_id,
                             ccord_id reply_to_id, ccord_error *err);
  // Optional. Enables or pauses remote voice camera receive (OP 15 sink_wants) when the video grid is hidden/visible.
  void (*set_camera_rx)(ccord_transport *t, bool enabled);
  // Optional. Zero-copy ownership handoff of the current event's heap-owned media_data buffer when it matches
  // `expected`. Returns the heap buffer (caller frees with ccord_free) and detaches it from the transport,
  // or nullptr when the bytes are not separately heap-owned or do not match.
  uint8_t *(*take_media_data)(ccord_transport *t, const uint8_t *expected);
} ccord_transport_vtable;
struct ccord_transport { const ccord_transport_vtable *vt; };           // implementations embed this as first member
[[nodiscard]] static inline ccord_status ccord_transport_connect(ccord_transport *t, ccord_error *e) { return t->vt->connect(t, e); }
static inline void ccord_transport_disconnect(ccord_transport *t) { t->vt->disconnect(t); }
[[nodiscard]] static inline ccord_status ccord_transport_request_history(ccord_transport *t, ccord_id c, uint32_t n, ccord_error *e) { return t->vt->request_history(t, c, n, e); }
[[nodiscard]] static inline ccord_status ccord_transport_send_message(ccord_transport *t, ccord_id c, const char *s, size_t n, ccord_error *e) { return t->vt->send_message(t, c, s, n, e); }
static inline bool ccord_transport_poll(ccord_transport *t, ccord_net_event *o) { return t->vt->poll(t, o); }
static inline void ccord_transport_destroy(ccord_transport *t) { if (t) { t->vt->destroy(t); } }

#define CCORD_TRANSPORT_OPTIONAL(t, hook) \
  ((t) && (t)->vt && (t)->vt->hook)

[[nodiscard]] static inline ccord_status ccord_transport_edit_message(ccord_transport *t, ccord_id c, ccord_id m,
                                                                      const char *s, size_t n, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, edit_message)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: edit not supported");
  return t->vt->edit_message(t, c, m, s, n, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_delete_message(ccord_transport *t, ccord_id c, ccord_id m, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, delete_message)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: delete not supported");
  return t->vt->delete_message(t, c, m, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_send_typing(ccord_transport *t, ccord_id c, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, send_typing)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: typing not supported");
  return t->vt->send_typing(t, c, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_toggle_reaction(ccord_transport *t, ccord_id c, ccord_id m,
                                                                        const char *emoji, size_t n, bool add, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, toggle_reaction)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: reaction not supported");
  return t->vt->toggle_reaction(t, c, m, emoji, n, add, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_ack_message(ccord_transport *t, ccord_id c, ccord_id m, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, ack_message)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: ack not supported");
  return t->vt->ack_message(t, c, m, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_select_guild(ccord_transport *t, ccord_id g, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, select_guild)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: guild select not supported");
  return t->vt->select_guild(t, g, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_join_voice(ccord_transport *t, ccord_id g, ccord_id c, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, join_voice)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: voice join not supported");
  return t->vt->join_voice(t, g, c, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_leave_voice(ccord_transport *t, ccord_id g, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, leave_voice)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: voice leave not supported");
  return t->vt->leave_voice(t, g, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_update_voice_state(ccord_transport *t, ccord_id g, bool self_mute,
                                                                           bool self_deaf, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, update_voice_state)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: voice state not supported");
  return t->vt->update_voice_state(t, g, self_mute, self_deaf, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_start_stream(ccord_transport *t, ccord_id g, ccord_id c,
                                                                      const ccord_stream_config *cfg, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, start_stream)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: stream start not supported");
  return t->vt->start_stream(t, g, c, cfg, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_stop_stream(ccord_transport *t, ccord_id g, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, stop_stream)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: stream stop not supported");
  return t->vt->stop_stream(t, g, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_send_stream_frame(ccord_transport *t, const uint8_t *annexb,
                                                                          size_t len, uint32_t ts90k, bool keyframe,
                                                                          ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, send_stream_frame))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: stream frame send not supported");
  return t->vt->send_stream_frame(t, annexb, len, ts90k, keyframe, e);
}
static inline bool ccord_transport_poll_stream_frame(ccord_transport *t, uint8_t *buf, size_t buf_cap, size_t *out_len,
                                                     uint32_t *out_ts90k, bool *out_keyframe) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, poll_stream_frame)) return false;
  return t->vt->poll_stream_frame(t, buf, buf_cap, out_len, out_ts90k, out_keyframe);
}
[[nodiscard]] static inline ccord_status ccord_transport_send_stream_pcm(ccord_transport *t, const int16_t *pcm,
                                                                         size_t samples, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, send_stream_pcm))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: stream pcm send not supported");
  return t->vt->send_stream_pcm(t, pcm, samples, e);
}
static inline size_t ccord_transport_poll_stream_pcm(ccord_transport *t, int16_t *out_pcm, size_t cap_samples) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, poll_stream_pcm)) return 0;
  return t->vt->poll_stream_pcm(t, out_pcm, cap_samples);
}
[[nodiscard]] static inline ccord_status ccord_transport_send_reply(ccord_transport *t, ccord_id c, ccord_id reply_to,
                                                                   const char *s, size_t n, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, send_reply)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: reply not supported");
  return t->vt->send_reply(t, c, reply_to, s, n, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_toggle_pin(ccord_transport *t, ccord_id c, ccord_id m,
                                                                    bool pinned, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, toggle_pin)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: pin not supported");
  return t->vt->toggle_pin(t, c, m, pinned, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_request_history_before(ccord_transport *t, ccord_id c,
                                                                               ccord_id before, uint32_t n, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, request_history_before))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: history pagination not supported");
  return t->vt->request_history_before(t, c, before, n, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_request_pins(ccord_transport *t, ccord_id c, uint32_t n,
                                                                     ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, request_pins))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: pins not supported");
  return t->vt->request_pins(t, c, n, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_set_presence(ccord_transport *t, ccord_presence p,
                                                                     const char *text, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, set_presence))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: presence not supported");
  return t->vt->set_presence(t, p, text, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_set_voice_camera(ccord_transport *t, ccord_id g, bool on,
                                                                         ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, set_voice_camera))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: camera not supported");
  return t->vt->set_voice_camera(t, g, on, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_send_message_with_file(ccord_transport *t, ccord_id c,
                                                                               const char *utf8, size_t len,
                                                                               const char *filename,
                                                                               const void *file_data, size_t file_size,
                                                                               ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, send_message_with_file))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: file upload not supported");
  return t->vt->send_message_with_file(t, c, utf8, len, filename, file_data, file_size, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_fetch_media(ccord_transport *t, const char *url,
                                                                    ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, fetch_media))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: media fetch not supported");
  return t->vt->fetch_media(t, url, e);
}
// A download the user asked for (clicking a video). Falls back to fetch_media when the transport has no user variant.
[[nodiscard]] static inline ccord_status ccord_transport_fetch_media_user(ccord_transport *t, const char *url,
                                                                         ccord_error *e) {
  if (CCORD_TRANSPORT_OPTIONAL(t, fetch_media_user)) return t->vt->fetch_media_user(t, url, e);
  return ccord_transport_fetch_media(t, url, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_fetch_media_range(ccord_transport *t,
                                                                          const ccord_media_range_req *req,
                                                                          ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, fetch_media_range))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: ranged media fetch not supported");
  return t->vt->fetch_media_range(t, req, e);
}
static inline bool ccord_transport_media_progress(const ccord_transport *t, const char *url,
                                                  uint64_t *out_now, uint64_t *out_total) {
  if (out_now) *out_now = 0;
  if (out_total) *out_total = 0;
  if (!url || !url[0] || !CCORD_TRANSPORT_OPTIONAL(t, media_progress)) return false;
  return t->vt->media_progress(t, url, out_now, out_total);
}
[[nodiscard]] static inline ccord_status ccord_transport_network_event(ccord_transport *t, ccord_link_event kind,
                                                                     ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, network_event))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: network event not supported");
  return t->vt->network_event(t, kind, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_search_messages(ccord_transport *t, ccord_id g, ccord_id c,
                                                                        const char *query, size_t query_len,
                                                                        ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, search_messages))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: search not supported");
  return t->vt->search_messages(t, g, c, query, query_len, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_open_dm(ccord_transport *t, ccord_id recipient_id,
                                                                ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, open_dm))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: open dm not supported");
  return t->vt->open_dm(t, recipient_id, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_push_voice_pcm(ccord_transport *t, const int16_t *pcm,
                                                                       size_t samples, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, push_voice_pcm))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: voice pcm push not supported");
  return t->vt->push_voice_pcm(t, pcm, samples, e);
}
static inline size_t ccord_transport_pull_voice_pcm(ccord_transport *t, int16_t *pcm, size_t max_samples) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, pull_voice_pcm)) return 0;
  return t->vt->pull_voice_pcm(t, pcm, max_samples);
}
[[nodiscard]] static inline ccord_status ccord_transport_set_relationship(ccord_transport *t, ccord_id user_id,
                                                                          ccord_relationship_kind kind,
                                                                          ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, set_relationship))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: relationships not supported");
  return t->vt->set_relationship(t, user_id, kind, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_request_profile(ccord_transport *t, ccord_id user_id,
                                                                         ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, request_profile))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: user profile not supported");
  return t->vt->request_profile(t, user_id, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_select_channel(ccord_transport *t, ccord_id g, ccord_id c,
                                                                        ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, select_channel)) return ccord_transport_select_guild(t, g, e);
  return t->vt->select_channel(t, g, c, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_request_members(ccord_transport *t, ccord_id g, ccord_id c,
                                                                         uint32_t chunks, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, request_members))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: member list paging not supported");
  return t->vt->request_members(t, g, c, chunks, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_watch_stream(ccord_transport *t, ccord_id g, ccord_id c,
                                                                      ccord_id u, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, watch_stream)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: stream watch not supported");
  return t->vt->watch_stream(t, g, c, u, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_stop_watching(ccord_transport *t, ccord_id g, ccord_id c,
                                                                       ccord_id u, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, stop_watching)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: stream watch not supported");
  return t->vt->stop_watching(t, g, c, u, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_set_stream_paused(ccord_transport *t, ccord_id g, ccord_id c,
                                                                           ccord_id u, bool paused, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, set_stream_paused)) return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: stream pause not supported");
  return t->vt->set_stream_paused(t, g, c, u, paused, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_request_channel(ccord_transport *t, ccord_id c, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, request_channel))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: channel lookup not supported");
  return t->vt->request_channel(t, c, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_request_forum_posts(ccord_transport *t,
                                                                            const ccord_forum_query *q, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, request_forum_posts))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: forum posts not supported");
  return t->vt->request_forum_posts(t, q, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_create_forum_post(ccord_transport *t, ccord_id forum_id,
                                                                           const char *title, size_t title_len,
                                                                           const char *content, size_t content_len,
                                                                           const ccord_id *tags, size_t ntags,
                                                                           ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, create_forum_post))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: creating forum posts not supported");
  return t->vt->create_forum_post(t, forum_id, title, title_len, content, content_len, tags, ntags, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_request_expression_data(ccord_transport *t, ccord_expr_kind kind,
                                                                                const char *arg, size_t arg_len,
                                                                                ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, request_expression_data))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: expression data not supported");
  return t->vt->request_expression_data(t, kind, arg, arg_len, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_send_sticker(ccord_transport *t, ccord_id c, ccord_id sticker,
                                                                      ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, send_sticker))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: stickers not supported");
  return t->vt->send_sticker(t, c, sticker, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_send_greet(ccord_transport *t, ccord_id c, ccord_id sticker,
                                                                    ccord_id reply_to, ccord_error *e) {
  if (CCORD_TRANSPORT_OPTIONAL(t, send_greet))
    return t->vt->send_greet(t, c, sticker, reply_to, e);
  if (CCORD_TRANSPORT_OPTIONAL(t, send_sticker))
    return t->vt->send_sticker(t, c, sticker, e);
  return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: greet stickers not supported");
}
[[nodiscard]] static inline ccord_status ccord_transport_admin_action(ccord_transport *t, const ccord_admin_request *req,
                                                                      ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, admin_action))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: server administration not supported");
  return t->vt->admin_action(t, req, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_vote_poll(ccord_transport *t, ccord_id c, ccord_id m,
                                                                   const uint32_t *answer_ids, size_t n,
                                                                   ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, vote_poll))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: poll voting not supported");
  return t->vt->vote_poll(t, c, m, answer_ids, n, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_send_interaction(ccord_transport *t,
                                                                          const ccord_interaction_request *req,
                                                                          ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, send_interaction))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: interactions not supported");
  return t->vt->send_interaction(t, req, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_send_reply_ex(ccord_transport *t, ccord_id c,
                                                                       ccord_id reply_to, bool mention_author,
                                                                       const char *s, size_t n, ccord_error *e) {
  if (CCORD_TRANSPORT_OPTIONAL(t, send_reply_ex)) {
    return t->vt->send_reply_ex(t, c, reply_to, mention_author, s, n, e);
  }
  if (mention_author && CCORD_TRANSPORT_OPTIONAL(t, send_reply)) {
    return t->vt->send_reply(t, c, reply_to, s, n, e);
  }
  return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: reply not supported");
}
[[nodiscard]] static inline ccord_status ccord_transport_send_message_with_files(
    ccord_transport *t, ccord_id c, ccord_id reply_to, bool mention_author, const char *s, size_t n,
    const ccord_upload_file *files, size_t file_count, ccord_error *e) {
  if (CCORD_TRANSPORT_OPTIONAL(t, send_message_with_files)) {
    return t->vt->send_message_with_files(t, c, reply_to, mention_author, s, n, files, file_count, e);
  }
  // A voice message carries duration/waveform/flags the plain single-file call cannot express: never degrade it.
  if (file_count == 1 && reply_to == CCORD_ID_NONE && files && !files[0].spoiler && files[0].duration_ms == 0 &&
      CCORD_TRANSPORT_OPTIONAL(t, send_message_with_file)) {
    return t->vt->send_message_with_file(t, c, s, n, files[0].filename, files[0].data, files[0].size, e);
  }
  return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: multi-file upload not supported");
}
[[nodiscard]] static inline ccord_status ccord_transport_request_slash_commands(ccord_transport *t, ccord_id g,
                                                                                ccord_id c, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, request_slash_commands))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: slash commands not supported");
  return t->vt->request_slash_commands(t, g, c, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_search_messages_ex(ccord_transport *t, ccord_id g,
                                                                            ccord_id c,
                                                                            const ccord_search_query *q,
                                                                            ccord_error *e) {
  if (!q) return ccord_error_set(e, CCORD_ERR_INVALID, "transport: null search query");
  if (CCORD_TRANSPORT_OPTIONAL(t, search_messages_ex)) {
    return t->vt->search_messages_ex(t, g, c, q, e);
  }
  if (!q->has_operators && q->offset == 0 && q->content[0] != '\0' &&
      CCORD_TRANSPORT_OPTIONAL(t, search_messages)) {
    size_t clen = 0;
    while (q->content[clen] != '\0') clen++;
    return t->vt->search_messages(t, g, c, q->content, clen, e);
  }
  return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: structured search not supported");
}
[[nodiscard]] static inline ccord_status ccord_transport_request_pins_before(ccord_transport *t, ccord_id c,
                                                                             const char *before_cursor,
                                                                             uint32_t limit, ccord_error *e) {
  if (CCORD_TRANSPORT_OPTIONAL(t, request_pins_before)) {
    return t->vt->request_pins_before(t, c, before_cursor, limit, e);
  }
  if ((!before_cursor || before_cursor[0] == '\0') && CCORD_TRANSPORT_OPTIONAL(t, request_pins)) {
    return t->vt->request_pins(t, c, limit, e);
  }
  return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: paginated pins not supported");
}
[[nodiscard]] static inline ccord_status ccord_transport_request_reaction_users(ccord_transport *t, ccord_id c,
                                                                                ccord_id m, const char *emoji,
                                                                                size_t emoji_len, uint32_t limit,
                                                                                ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, request_reaction_users))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: reaction users not supported");
  return t->vt->request_reaction_users(t, c, m, emoji, emoji_len, limit, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_request_discoverable_guilds(
    ccord_transport *t, const char *query, uint32_t category_id, uint32_t offset, uint32_t limit, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, request_discoverable_guilds))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: server discovery not supported");
  return t->vt->request_discoverable_guilds(t, query, category_id, offset, limit, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_request_profile_ex(ccord_transport *t, ccord_id user_id,
                                                                            ccord_id guild_id, ccord_error *e) {
  if (CCORD_TRANSPORT_OPTIONAL(t, request_profile_ex)) {
    return t->vt->request_profile_ex(t, user_id, guild_id, e);
  }
  return ccord_transport_request_profile(t, user_id, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_set_presence_ex(ccord_transport *t, ccord_presence p,
                                                                         const char *emoji_name, ccord_id emoji_id,
                                                                         const char *status_text, ccord_error *e) {
  if (CCORD_TRANSPORT_OPTIONAL(t, set_presence_ex)) {
    return t->vt->set_presence_ex(t, p, emoji_name, emoji_id, status_text, e);
  }
  return ccord_transport_set_presence(t, p, status_text, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_call_action(ccord_transport *t, ccord_id c,
                                                                     ccord_call_action act, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, call_action))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: calls not supported");
  return t->vt->call_action(t, c, act, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_play_soundboard(ccord_transport *t, ccord_id g,
                                                                         ccord_id sound, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, play_soundboard))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: soundboard not supported");
  return t->vt->play_soundboard(t, g, sound, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_set_user_volume(ccord_transport *t, ccord_id u,
                                                                         uint8_t pct, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, set_user_volume))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: per-user volume not supported");
  return t->vt->set_user_volume(t, u, pct, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_set_voice_config(ccord_transport *t,
                                                                          const ccord_voice_config *cfg,
                                                                          ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, set_voice_config))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: voice config not supported");
  return t->vt->set_voice_config(t, cfg, e);
}
// M0-3 send with a client nonce. Deliberate deviation from the UNSUPPORTED convention: a null hook FALLS BACK to
// plain send_message — send is a core capability, and hook-less transports keep working (without a nonce).
[[nodiscard]] static inline ccord_status ccord_transport_send_message_with_nonce(ccord_transport *t, ccord_id c,
                                                                                 const char *s, size_t n,
                                                                                 const char *nonce, size_t nonce_len,
                                                                                 ccord_error *e) {
  if (CCORD_TRANSPORT_OPTIONAL(t, send_message_with_nonce)) {
    return t->vt->send_message_with_nonce(t, c, s, n, nonce, nonce_len, e);
  }
  return ccord_transport_send_message(t, c, s, n, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_send_voice_video(ccord_transport *t, const uint8_t *annexb,
                                                                          size_t len, uint32_t ts90k, bool keyframe,
                                                                          ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, send_voice_video))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: voice video send not supported");
  return t->vt->send_voice_video(t, annexb, len, ts90k, keyframe, e);
}
static inline bool ccord_transport_consume_keyframe_request(ccord_transport *t) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, consume_keyframe_request)) return false;
  return t->vt->consume_keyframe_request(t);
}
[[nodiscard]] static inline ccord_status ccord_transport_send_tts_message(ccord_transport *t, ccord_id c,
                                                                          const char *s, size_t n, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, send_tts_message))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: tts send not supported");
  return t->vt->send_tts_message(t, c, s, n, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_forward_message(ccord_transport *t, ccord_id c,
                                                                         ccord_id source_channel, ccord_id m,
                                                                         ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, forward_message))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: forward not supported");
  return t->vt->forward_message(t, c, source_channel, m, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_crosspost_message(ccord_transport *t, ccord_id c,
                                                                           ccord_id m, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, crosspost_message))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: crosspost not supported");
  return t->vt->crosspost_message(t, c, m, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_mark_unread(ccord_transport *t, ccord_id c, ccord_id m,
                                                                     ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, mark_unread))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: mark unread not supported");
  return t->vt->mark_unread(t, c, m, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_patch_settings_proto(ccord_transport *t,
                                                                              const uint8_t *proto, size_t proto_len,
                                                                              ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, patch_settings_proto))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: settings proto patch not supported");
  return t->vt->patch_settings_proto(t, proto, proto_len, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_set_status_proto(ccord_transport *t,
                                                                          const ccord_proto_status *st, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, set_status_proto))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: status proto not supported");
  return t->vt->set_status_proto(t, st, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_set_rich_activities(ccord_transport *t, const char *json,
                                                                             size_t len, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, set_rich_activities))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: rich activities not supported");
  return t->vt->set_rich_activities(t, json, len, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_request_report_menu(ccord_transport *t, ccord_report_type type,
                                                                            ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, request_report_menu))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: reports not supported");
  return t->vt->request_report_menu(t, type, e);
}
[[nodiscard]] static inline ccord_status ccord_transport_submit_report(ccord_transport *t, const ccord_report_request *req,
                                                                      ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, submit_report))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: reports not supported");
  return t->vt->submit_report(t, req, e);
}
static inline size_t ccord_transport_camera_senders(ccord_transport *t, ccord_camera_rx *out, size_t max) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, camera_senders) || !out || max == 0) return 0;
  return t->vt->camera_senders(t, out, max);
}
static inline bool ccord_transport_poll_camera_frame(ccord_transport *t, ccord_id user_id, uint8_t *buf, size_t buf_cap,
                                                     size_t *out_len, uint32_t *out_ts90k, bool *out_keyframe) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, poll_camera_frame) || !buf || buf_cap == 0 || !out_len) return false;
  return t->vt->poll_camera_frame(t, user_id, buf, buf_cap, out_len, out_ts90k, out_keyframe);
}
static inline size_t ccord_transport_active_uploads(ccord_transport *t, ccord_upload_progress *out, size_t max) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, active_uploads)) return 0;
  return t->vt->active_uploads(t, out, max);
}
[[nodiscard]] static inline ccord_status ccord_transport_resolve_video(ccord_transport *t, uint64_t req_id,
                                                                       const char *url, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, resolve_video))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: video resolve not supported");
  return t->vt->resolve_video(t, req_id, url, e);
}
static inline bool ccord_transport_wait_info(const ccord_transport *t, ccord_wait_info *out) {
  if (!out) return false;
  *out = (ccord_wait_info){.timeout_ms = UINT32_MAX};
  if (!CCORD_TRANSPORT_OPTIONAL(t, wait_info)) return false;
  return t->vt->wait_info(t, out);
}
static inline void ccord_transport_set_background(ccord_transport *t, bool background) {
  if (CCORD_TRANSPORT_OPTIONAL(t, set_background)) t->vt->set_background(t, background);
}
static inline void ccord_transport_set_suspended(ccord_transport *t, bool suspended) {
  if (CCORD_TRANSPORT_OPTIONAL(t, set_suspended)) t->vt->set_suspended(t, suspended);
}
static inline void ccord_transport_set_camera_rx(ccord_transport *t, bool enabled) {
  if (CCORD_TRANSPORT_OPTIONAL(t, set_camera_rx)) t->vt->set_camera_rx(t, enabled);
}
static inline uint8_t *ccord_transport_take_media_data(ccord_transport *t, const uint8_t *expected) {
  if (!expected || !CCORD_TRANSPORT_OPTIONAL(t, take_media_data)) return nullptr;
  return t->vt->take_media_data(t, expected);
}
[[nodiscard]] static inline ccord_status ccord_transport_create_poll(ccord_transport *t, ccord_id c,
                                                                     const ccord_poll_create *poll, ccord_error *e) {
  if (!CCORD_TRANSPORT_OPTIONAL(t, create_poll))
    return ccord_error_set(e, CCORD_ERR_UNSUPPORTED, "transport: polls not supported");
  return t->vt->create_poll(t, c, poll, e);
}
typedef struct ccord_buf ccord_buf;
// The exact JSON body of a poll send: {"poll":{"question":{"text":..},"answers":[{"poll_media":{"text":..}}],
// "duration":N,"allow_multiselect":B,"layout_type":1}}. Validates the poll (counts, lengths, UTF-8) and needs no network.
[[nodiscard]] ccord_status ccord_poll_build_body(const ccord_poll_create *poll, ccord_buf *out, ccord_error *err);
typedef struct ccord_gateway ccord_gateway;
typedef struct ccord_live_event ccord_live_event;
[[nodiscard]] ccord_status ccord_gateway_build_lazy_request_channel(ccord_id guild_id, ccord_id channel_id,
                                                                    ccord_buf *out, ccord_error *err);
// The member list is asked for a hundred positions at a time. `chunks` hundreds are wanted: this builds the op 14 for them
// (ccord_gateway_member_ranges says which ranges), and ccord_gateway_member_ranges fills `ranges` (up to three) with the
// [first, last] positions of the chunks to subscribe to, returning how many.
#define CCORD_MEMBER_CHUNK 100u
#define CCORD_MEMBER_CHUNKS_MAX 50u
[[nodiscard]] ccord_status ccord_gateway_build_lazy_request_chunks(ccord_id guild_id, ccord_id channel_id, uint32_t chunks,
                                                                   ccord_buf *out, ccord_error *err);
size_t ccord_gateway_member_ranges(uint32_t chunks, uint32_t ranges[3][2]);
// Asks for more of the open channel's member list (the first `chunks` hundreds of it). Never asks for less than is already
// wanted, nor for a list that is not the open one.
[[nodiscard]] ccord_status ccord_gateway_request_member_chunks(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id,
                                                               uint32_t chunks, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_select_channel(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id,
                                                        ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_user_profile(const char *json, size_t len, ccord_id user_id,
                                                        ccord_live_event *out, ccord_error *err);
// Parses GET /user-profile-effects -> {"profile_effect_configs":[...]}. Caller frees *out_items with ccord_free.
[[nodiscard]] ccord_status ccord_live_json_profile_effects(const char *json, size_t len,
                                                           ccord_profile_effect_config **out_items,
                                                           size_t *out_count, ccord_error *err);
// Parses GET /collectibles-products/{sku_id} for PROFILE_EFFECT (type 1) or PROFILE_FRAME (type 3) items.
[[nodiscard]] ccord_status ccord_live_json_collectible_product(const char *json, size_t len, ccord_id sku_id,
                                                               ccord_profile_effect_config *out_effect, bool *has_effect,
                                                               ccord_profile_frame_config *out_frame, bool *has_frame,
                                                               ccord_error *err);
// Parses GET /users/@me/collectibles-purchases into the user's owned Avatar Decorations, Profile Effects and Nameplates.
// Caller frees *out_items with ccord_free.
[[nodiscard]] ccord_status ccord_live_json_collectibles_purchases(const char *json, size_t len,
                                                                  ccord_owned_collectible **out_items,
                                                                  size_t *out_count, ccord_error *err);
// Resolves the deterministic disk cache path under $XDG_CACHE_HOME/ccord/media/ for a CDN URL.
[[nodiscard]] ccord_status ccord_media_cache_path(const char *url, char *out_path, size_t cap, ccord_error *err);
// Enables/disables emitting CCORD_NET_RAW_GATEWAY for op 0 dispatch frames (zero overhead while disabled).
void ccord_gateway_set_raw_tap(bool enabled);
bool ccord_gateway_raw_tap_enabled(void);
