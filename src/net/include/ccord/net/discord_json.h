// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/error.h>
#include <ccord/net/transport.h>

#define CCORD_LIVE_GUILD_NONE 0
#define CCORD_LIVE_GUILD_UPSERT 1   // GUILD_CREATE / GUILD_UPDATE
#define CCORD_LIVE_GUILD_DELETE 2   // GUILD_DELETE (ev.ev.guild_id)
#define CCORD_LIVE_GUILD_MEMBERS 3  // GUILD_MEMBERS_CHUNK (ev.ev.guild_id + members)

typedef struct ccord_live_event {
  ccord_net_event ev;            // public event; pointers below keep it alive
  int op;                        // Gateway opcode
  int64_t seq;                   // dispatch sequence, -1 when absent
  bool hello;                    // op 10 seen
  bool reconnect;                // op 7 seen
  bool invalid_session;          // op 9 seen
  bool resumable;                // op 9 d == true
  bool resumed;                  // RESUMED dispatch
  int guild_op;                  // CCORD_LIVE_GUILD_* for GUILD_* dispatch
  uint32_t heartbeat_interval_ms;
  const char *session_id;        // READY: session id for RESUME
  const char *resume_gateway_url; // READY: base URL for RESUME
  const char *voice_session_id;  // VOICE_STATE_UPDATE: self voice session id
  ccord_message *messages;
  size_t message_count;
  ccord_channel *channels;
  size_t channel_count;
  ccord_snapshot snapshot;
  ccord_user *users;
  size_t user_count;
  ccord_user_profile *user_profile;
  ccord_guild *guilds;
  size_t guild_count;
  ccord_channel *snap_channels;
  size_t snap_channel_count;
  ccord_member *members;
  size_t member_count;
  ccord_voice_state *voice_states;   // GUILD_* / READY voice participants
  size_t voice_state_count;
  ccord_voice_state *snap_voices;
  size_t snap_voice_count;
  ccord_relationship *relationships; // READY relationships
  size_t relationship_count;
  ccord_id *message_ids;             // MESSAGE_DELETE_BULK
  size_t message_id_count;
  ccord_ban *bans;                   // BAN_LIST
  size_t ban_count;
  ccord_invite *invites;             // INVITE_LIST / INVITE_UPSERT
  size_t invite_count;
  ccord_audit_entry *audit;          // AUDIT_LOG
  size_t audit_count;
  ccord_automod_rule *automod_rules; // AUTOMOD_LIST / AUTOMOD_UPSERT
  size_t automod_rule_count;
  ccord_webhook *webhooks;           // WEBHOOK_LIST / WEBHOOK_UPSERT
  size_t webhook_count;
  ccord_app_cmd_perms *app_cmd_perms; // APP_CMD_PERMS_LIST
  size_t app_cmd_perms_count;
  ccord_welcome_screen *welcome_screen; // WELCOME_SCREEN
  ccord_onboarding *onboarding;      // ONBOARDING
  bool has_user_settings;            // user_settings holds a decoded M6 account settings object
  ccord_user_settings user_settings; // READY.user_settings / USER_SETTINGS_UPDATE / REST /users/@me/settings
  ccord_auth_session *auth_sessions; // GET /auth/sessions answer
  size_t auth_session_count;
  ccord_connection *connections;     // GET /users/@me/connections list or a single PATCH answer (count 1)
  size_t connection_count;
  ccord_arena arena;
} ccord_live_event;

void ccord_live_event_init(ccord_live_event *ev);
void ccord_live_event_free(ccord_live_event *ev);

// Parses one Gateway v9 JSON payload. Control payloads (HELLO/ACK) set `op`
// and `hello`; dispatch payloads fill ev.type when recognised.
[[nodiscard]] ccord_status ccord_live_gateway_decode(const char *json, size_t len, ccord_live_event *out,
                                                     ccord_error *err);
[[nodiscard]] ccord_status ccord_live_gateway_decode_ex(const char *json, size_t len, ccord_id self_id,
                                                        ccord_live_event *out, ccord_error *err);
// Maps a REST message object (or a history array) into an owned event.
[[nodiscard]] ccord_status ccord_live_json_message(const char *json, size_t len, ccord_net_event_type type,
                                                   ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_history(const char *json, size_t len, ccord_id channel_id,
                                                   ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_search(const char *json, size_t len, ccord_live_event *out,
                                                  ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_member(const char *json, size_t len, ccord_id guild_id,
                                                  ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_guild(const char *json, size_t len, ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_guild_profile(const char *json, size_t len, ccord_id guild_id,
                                                         ccord_live_event *out, ccord_error *err);
// REST answers of the administration calls, as the events the gateway would have produced (idempotent for the store).
// A role object (POST/PATCH /guilds/{g}/roles[/{r}]) or an array of them (PATCH /guilds/{g}/roles, the reorder answer)
// becomes ROLE_UPSERT with one entry per role.
[[nodiscard]] ccord_status ccord_live_json_role(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                                ccord_error *err);
// PATCH /users/@me/guilds/{g}/settings answers with the updated settings entry -> NOTIFY_SETTINGS.
[[nodiscard]] ccord_status ccord_live_json_notify_settings(const char *json, size_t len, ccord_live_event *out,
                                                           ccord_error *err);
// GET /guilds/{g}/bans -> BAN_LIST.
[[nodiscard]] ccord_status ccord_live_json_bans(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                                ccord_error *err);
// GET /guilds/{g}/invites (an array) -> INVITE_LIST; POST /channels/{c}/invites (an object) -> INVITE_UPSERT.
[[nodiscard]] ccord_status ccord_live_json_invites(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                                   ccord_error *err);
// GET /channels/{c}/invites -> CHANNEL_INVITE_LIST.
[[nodiscard]] ccord_status ccord_live_json_channel_invites(const char *json, size_t len, ccord_id guild_id,
                                                           ccord_id channel_id, ccord_live_event *out,
                                                           ccord_error *err);
// GET /guilds/{g}/audit-logs -> AUDIT_LOG.
[[nodiscard]] ccord_status ccord_live_json_audit_log(const char *json, size_t len, ccord_id guild_id,
                                                     ccord_live_event *out, ccord_error *err);
// One-line text of a Discord error body ("Missing Permissions"). False when the body has none.
[[nodiscard]] bool ccord_live_json_error_message(const char *json, size_t len, char *out, size_t cap);
[[nodiscard]] ccord_status ccord_live_json_channel(const char *json, size_t len, ccord_id guild_hint,
                                                   ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_forum_posts(const char *json, size_t len, ccord_id forum_id,
                                                       uint32_t offset, ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_forum_post_created(const char *json, size_t len, ccord_id forum_id,
                                                              ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_upload_slot(const char *json, size_t len, char *out_upload_url,
                                                       size_t url_cap, char *out_upload_filename,
                                                       size_t filename_cap, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_upload_slots(const char *json, size_t len, size_t expected_count,
                                                        char out_upload_urls[][2048],
                                                        char out_upload_filenames[][256], ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_slash_commands(const char *json, size_t len, ccord_id guild_id,
                                                          ccord_id channel_id, ccord_live_event *out,
                                                          ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_pins(const char *json, size_t len, ccord_id channel_id,
                                                bool append, ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_reaction_users(const char *json, size_t len, ccord_id channel_id,
                                                          ccord_id message_id, const char *emoji,
                                                          ccord_live_event *out, ccord_error *err);
// GET/PATCH /users/@me/settings, READY.user_settings and USER_SETTINGS_UPDATE. Fills `has_user_settings`
// + `user_settings` (missing/null fields keep Discord defaults) and still emits the existing
// guild_folders / custom_status mapping. A bare settings object without guild_folders is accepted.
[[nodiscard]] ccord_status ccord_live_json_user_settings(const char *json, size_t len, ccord_live_event *out,
                                                         ccord_error *err);
// GET /auth/sessions answer (array). Entries without an id_hash are dropped; the list is bounded by
// CCORD_LIVE_MAX_AUTH_SESSIONS. Strings are UTF-8-safe truncated to their model buffers.
#define CCORD_LIVE_MAX_AUTH_SESSIONS 16u
[[nodiscard]] ccord_status ccord_live_json_auth_sessions(const char *json, size_t len, ccord_live_event *out,
                                                         ccord_error *err);
// GET /users/@me/connections answer (array) or a single PATCH connection object. Entries without an
// id/name are dropped; the list is bounded by CCORD_LIVE_MAX_CONNECTIONS.
#define CCORD_LIVE_MAX_CONNECTIONS 32u
[[nodiscard]] ccord_status ccord_live_json_connections(const char *json, size_t len, ccord_live_event *out,
                                                       ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_custom_status(const char *json, size_t len, ccord_live_event *out,
                                                         ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_user_note(const char *json, size_t len, ccord_id fallback_uid,
                                                     ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_invite_preview(const char *json, size_t len, bool accepted,
                                                          ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_discoverable_guilds(const char *json, size_t len, const char *query,
                                                               uint32_t category_id, uint32_t offset,
                                                               ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_incident_actions(const char *json, size_t len, ccord_id guild_id,
                                                            ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_vanity_url(const char *json, size_t len, ccord_id guild_id,
                                                      ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_guild_emojis(const char *json, size_t len, ccord_id guild_id,
                                                        ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_guild_stickers(const char *json, size_t len, ccord_id guild_id,
                                                          ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_guild_soundboard(const char *json, size_t len, ccord_id guild_id,
                                                            ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_prune(const char *json, size_t len, ccord_id guild_id, uint8_t days,
                                                 bool executed, ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_automod_rules(const char *json, size_t len, ccord_id guild_id,
                                                         ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_webhooks(const char *json, size_t len, ccord_id guild_id,
                                                    ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_followed_channel(const char *json, size_t len, ccord_id guild_id,
                                                            ccord_id fallback_src_ch, ccord_id target_ch,
                                                            ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_app_cmd_perms(const char *json, size_t len, ccord_id guild_id,
                                                         ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_welcome_screen(const char *json, size_t len, ccord_id guild_id,
                                                          ccord_live_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_json_onboarding(const char *json, size_t len, ccord_id guild_id,
                                                      ccord_live_event *out, ccord_error *err);
// GET /guilds/{g}/member-verification answer -> CCORD_NET_MEMBER_VERIFICATION (ev.member_verification, arena-owned).
[[nodiscard]] ccord_status ccord_live_json_member_verification(const char *json, size_t len, ccord_id guild_id,
                                                               ccord_live_event *out, ccord_error *err);
// PATCH /stage-instances/{channel_id} answer (a stage instance object) -> STAGE_INSTANCE_UPDATE (stage_deleted=false).
// guild_id/channel_id are hints for payloads that omit them; the payload's own id/guild_id win.
[[nodiscard]] ccord_status ccord_live_json_stage_instance(const char *json, size_t len, ccord_id guild_id,
                                                          ccord_id channel_id, ccord_live_event *out,
                                                          ccord_error *err);
// GET /guilds/{g}/scheduled-events/{e} answer (one event object) -> SCHEDULED_EVENT_UPDATE (deleted=false).
// guild_id is a hint for payloads that omit it; the payload's own guild_id wins.
[[nodiscard]] ccord_status ccord_live_json_scheduled_event(const char *json, size_t len, ccord_id guild_id,
                                                           ccord_live_event *out, ccord_error *err);
// GET /guilds/{g}/scheduled-events answer (an array of event objects) -> SCHEDULED_EVENT_LIST.
// The list is bounded by CCORD_LIVE_MAX_SCHEDULED_EVENTS; malformed entries are dropped.
#define CCORD_LIVE_MAX_SCHEDULED_EVENTS 100u
[[nodiscard]] ccord_status ccord_live_json_scheduled_event_list(const char *json, size_t len, ccord_id guild_id,
                                                                ccord_live_event *out, ccord_error *err);
// GET /activities/shelf?guild_id={g} answer ({"activities":[config],"applications":[partial app],"assets":...})
// -> ACTIVITY_SHELF. Configs are joined to their application by application_id, non-web entries are dropped and
// the shelf is sorted by shelf_rank (ascending; unranked entries trail). The list is bounded by CCORD_LIVE_MAX_ACTIVITY_APPS; an empty shelf is a
// valid answer (count 0). Shapes come from unofficial docs, so absent fields are tolerated.
#define CCORD_LIVE_MAX_ACTIVITY_APPS 50u
[[nodiscard]] ccord_status ccord_live_json_activity_shelf(const char *json, size_t len, ccord_id guild_id,
                                                          ccord_live_event *out, ccord_error *err);
// Wire JSON / query path builders for replies, multi-file uploads, poll votes, interactions, and structured search.
[[nodiscard]] ccord_status ccord_live_build_search_path(ccord_id guild_id, ccord_id channel_id,
                                                        const ccord_search_query *query, ccord_buf *out,
                                                        ccord_error *err);
[[nodiscard]] ccord_status ccord_live_build_reply_body(const char *utf8, size_t len, ccord_id reply_to,
                                                       bool mention_author, ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_build_upload_slots_body(const ccord_upload_file *files, size_t file_count,
                                                              ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_build_upload_message_body(const char *utf8, size_t len, ccord_id reply_to,
                                                                bool mention_author, const ccord_upload_file *files,
                                                                const char uploaded_names[][256], size_t file_count,
                                                                ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_build_poll_vote_body(const uint32_t *answer_ids, size_t answer_count,
                                                           ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_build_interaction_body(const ccord_interaction_request *req,
                                                             const char *session_id, const char *nonce,
                                                             ccord_buf *out, ccord_error *err);

// Gateway connection (internal to live_transport.c, exposed for that unit) ----
