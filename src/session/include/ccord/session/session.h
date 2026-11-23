// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/event_bus.h>
#include <ccord/net/profile_edit.h>
#include <ccord/net/transport.h>
#include <ccord/net/video_resolve.h>
#include <ccord/store/expressions.h>
#include <ccord/store/store.h>
// Controller: the only code allowed to call both the transport and the store.
// UI and plugins call intents; network results are applied to the store in pump().
typedef struct ccord_session ccord_session;
// History asked from the transport per channel chunk (initial open and pagination).
#define CCORD_SESSION_HISTORY_LIMIT 50u
// Takes ownership of `transport` (destroyed in session_destroy, also on failure of create).
ccord_session *ccord_session_create(ccord_bus *bus, ccord_store *store, ccord_transport *transport, ccord_error *err);
void ccord_session_destroy(ccord_session *s);
[[nodiscard]] ccord_status ccord_session_start(ccord_session *s, ccord_error *err);       // transport connect
// Hot-swaps the session's underlying transport at runtime (takes ownership of `transport`, destroys old transport, and connects).
[[nodiscard]] ccord_status ccord_session_set_transport(ccord_session *s, ccord_transport *transport, ccord_error *err);
// While stale (store seeded from disk, no READY yet): reads and navigation work, mutating intents return
// CCORD_ERR_INVALID "Reconnecting - sending is available in a moment", no history/profile/pin requests are issued.
// Cleared by the next CCORD_NET_SNAPSHOT. ccord_session_set_transport does NOT clear it.
void ccord_session_set_stale(ccord_session *s, bool stale);
// The machine woke or the network changed: tells the transport to reconnect now instead of waiting for a missed
// heartbeat. No stale guard (a warm start may be sitting in backoff); it sends no request. Returns
// CCORD_ERR_UNSUPPORTED when the transport has no hook. Main thread, never blocks.
[[nodiscard]] ccord_status ccord_session_link_event(ccord_session *s, ccord_link_event kind, ccord_error *err);
// Tells the transport whether the app is backgrounded (suppresses OP 14 guild subscriptions & slows reconnect backoff)
// or suspended for power saving (closes socket while preserving session_id/seq for fast RESUME on foreground).
void ccord_session_set_background(ccord_session *s, bool background);
void ccord_session_set_suspended(ccord_session *s, bool suspended);
bool ccord_session_is_stale(const ccord_session *s);
// Applies up to 64 transport events to the store; posts connection.state. Call once per frame. Returns events applied.
size_t ccord_session_pump(ccord_session *s);
// Reports readable transport sockets and the next timer deadline so an idle/background loop can sleep in kernel poll().
// Returns true when the underlying transport supports wait_info (false for mock/idle transports).
bool ccord_session_wait_info(const ccord_session *s, ccord_wait_info *out);

// A user action (send, edit, delete, react, pin, ...) that Discord did not accept. Actions run asynchronously, so
// the call that started one already returned CCORD_OK; the failure is queued here and surfaces through pump().
// Nothing is retried on the user's behalf: a write that timed out may have been applied, so the user decides.
typedef struct ccord_action_failure {
  ccord_action_kind action;
  ccord_id channel_id;
  ccord_id message_id;        // edit/delete/react/pin: the message; reply: the message replied to
  ccord_id guild_id;          // admin: the server
  ccord_id user_id;           // admin: the member it was about
  ccord_id expr_id;           // admin: the expression (emoji/sticker/soundboard) it was about
  ccord_admin_op admin_op;    // admin (action == CCORD_ACTION_ADMIN): which call
  ccord_status status;
  char reason[192];           // short sentence for the user
  char *draft;                // what the user wrote (send/reply/edit/upload), null otherwise; owned by the caller
  size_t draft_len;
} ccord_action_failure;
// Pops the oldest failure. The caller owns out->draft: release it with ccord_action_failure_free.
[[nodiscard]] bool ccord_session_pop_failure(ccord_session *s, ccord_action_failure *out);
void ccord_action_failure_free(ccord_action_failure *f);
// Report flow. A menu or a sent report arrives as an event; a refusal arrives
// through ccord_session_pop_failure with action CCORD_ACTION_REPORT_MENU / CCORD_ACTION_REPORT (no draft).
typedef enum ccord_report_event_kind : uint8_t { CCORD_REPORT_EV_MENU, CCORD_REPORT_EV_SUBMITTED } ccord_report_event_kind;
typedef struct ccord_report_event {
  ccord_report_event_kind kind;
  ccord_report_type type;
  char *menu_json;            // MENU: the raw menu JSON, NUL terminated; owned by the event (ccord_report_event_free)
  size_t menu_len;
  ccord_id report_id;         // SUBMITTED: the report id (0 = none returned)
} ccord_report_event;
// Pops the oldest report event (queue of 4, newest dropped when full). The caller owns out: ccord_report_event_free.
[[nodiscard]] bool ccord_session_pop_report_event(ccord_session *s, ccord_report_event *out);
void ccord_report_event_free(ccord_report_event *e);
ccord_connection_state ccord_session_state(const ccord_session *s);
// Intents. All validate input and return a status; none block.
[[nodiscard]] ccord_status ccord_session_select_guild(ccord_session *s, ccord_id guild_id, ccord_error *err);
// Selects in store; if history not loaded yet and not already requested, asks the transport for the last
// CCORD_SESSION_HISTORY_LIMIT messages (once per channel).
[[nodiscard]] ccord_status ccord_session_select_channel(ccord_session *s, ccord_id channel_id, ccord_error *err);
// Text must be valid UTF-8, no NUL, <= CCORD_SEND_MAX_BYTES, non-empty after trimming spaces/newlines (else CCORD_ERR_INVALID).
// M0-3: inserts a visibly-pending local echo (send_state CCORD_MSG_SEND_PENDING) at once; the server's nonce-matched
// MESSAGE_CREATE confirms it, a refusal marks it failed (CCORD_MSG_SEND_FAILED). Unread/mention counts never move for
// the echo. Transports without the nonce hook keep the legacy path: the message appears only when the transport reports it.
[[nodiscard]] ccord_status ccord_session_send_message(ccord_session *s, ccord_id channel_id, const char *utf8, size_t len, ccord_error *err);
// Resends a failed local echo with its ORIGINAL nonce (Discord dedupes a send that may have gone through after all).
// The echo returns to pending; the server's answer confirms or fails it again. CCORD_ERR_NOT_FOUND when message_id is
// not a failed echo of channel_id (confirmed messages and unknown ids included).
[[nodiscard]] ccord_status ccord_session_retry_message(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err);
// Live actions. They validate against the store, then call the transport's optional hook
// (CCORD_ERR_UNSUPPORTED when the transport does not implement it).
[[nodiscard]] ccord_status ccord_session_edit_message(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                                      const char *utf8, size_t len, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_delete_message(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_send_typing(ccord_session *s, ccord_id channel_id, ccord_error *err);
// Toggles the signed-in user's reaction on a message (adds when not present). emoji must be non-empty UTF-8.
[[nodiscard]] ccord_status ccord_session_toggle_reaction(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                                         const char *emoji, size_t len, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_ack_message(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err);
// Tells the session which channel the user is reading right now: the open channel while the window has focus and the list
// is at its newest message. Messages that arrive there are read at once; in any other channel, the open one included while
// the user is away or scrolled up, they count as unread. 0 = none. Until this is called the open channel counts as read.
void ccord_session_set_reading(ccord_session *s, ccord_id channel_id);
// The member list of a big server is loaded a hundred positions at a time. The interface calls this (with its clock, in
// milliseconds) when the user has scrolled near the end of the rows it holds; it asks for the next hundred and returns true
// when a request went out. It does nothing before the server has said how big the list is, when everything it holds is
// already here, when a piece is still awaited, or less than a second after the last request, and it starts again at the
// top for another channel.
bool ccord_session_more_members(ccord_session *s, uint64_t now_ms);
// "Mark As Read": acknowledges the newest message of a channel, of every channel in a category, or of every channel of a
// server (`channel_id` 0; `guild_id` may be CCORD_DM_GUILD_ID for all conversations). Only channels that have unread or
// mention counts are touched, and at most CCORD_MARK_READ_MAX per call so one click cannot flood Discord (the rest stay
// unread and a second call takes them). The acknowledgements go through the governed queue at background priority.
// `acked` (optional) receives how many were sent. CCORD_ERR_NOT_FOUND for an unknown server or channel,
// CCORD_ERR_INVALID for a channel of another server; a transport failure on the first acknowledgement is returned,
// on a later one the call stops and reports the ones that did go out.
#define CCORD_MARK_READ_MAX 50u
[[nodiscard]] ccord_status ccord_session_mark_read(ccord_session *s, ccord_id guild_id, ccord_id channel_id, size_t *acked,
                                                   ccord_error *err);
// ---- REST / gateway parity ----
// Asks the transport for the channel's pinned messages (GET /channels/{id}/pins). The result
// arrives through pump() as PIN_LIST and is merged into the store history.
[[nodiscard]] ccord_status ccord_session_request_pins(ccord_session *s, ccord_id channel_id, ccord_error *err);
// Requests the next page of older pinned messages using the stored `before` cursor. No-op when !has_more or in flight.
[[nodiscard]] ccord_status ccord_session_request_more_pins(ccord_session *s, ccord_id channel_id, ccord_error *err);
bool ccord_session_pins_loading(const ccord_session *s, ccord_id channel_id);
// Asks for messages older than `before_id` (before=<id> pagination). Result arrives as HISTORY.
[[nodiscard]] ccord_status ccord_session_request_history_before(ccord_session *s, ccord_id channel_id,
                                                                ccord_id before_id, ccord_error *err);
// True when the channel may have older history messages available to paginate via request_history_before.
bool ccord_session_history_has_more(const ccord_session *s, ccord_id channel_id);
// Broadcasts a presence update (Gateway Opcode 3). Rejects OFFLINE and null/invalid text.
[[nodiscard]] ccord_status ccord_session_set_presence(ccord_session *s, ccord_presence presence,
                                                      const char *status_text, ccord_error *err);
// Rich Presence (Discord RPC SET_ACTIVITY): a JSON array of activity objects (string "name" + integral "type",
// at most 4 entries / 8192 bytes, validated by the transport) stored in the session and spliced into every
// presence update after the custom status. While CONNECTED the presence is resent at once; the array is
// re-pushed on reconnect (SNAPSHOT) so it survives presence rebuilds. nullptr (or "") clears it and resends.
// A rejected set keeps the previous array. Transports without the hook return CCORD_ERR_UNSUPPORTED.
[[nodiscard]] ccord_status ccord_session_set_rich_presence(ccord_session *s, const char *activities_json,
                                                           ccord_error *err);
// Turns the self camera on/off while connected to voice (VOICE_STATE_UPDATE self_video).
[[nodiscard]] ccord_status ccord_session_set_voice_camera(ccord_session *s, bool enabled, ccord_error *err);
// ---- voice / streaming / replies / pins ----
// The guild must exist and channel must be one of its voice channels.
[[nodiscard]] ccord_status ccord_session_join_voice(ccord_session *s, ccord_id guild_id, ccord_id channel_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_leave_voice(ccord_session *s, ccord_id guild_id, ccord_error *err);
// Updates self mute/deafen state (sends OP 4 immediately while joined or pending, else stored for next join).
[[nodiscard]] ccord_status ccord_session_set_voice_mute(ccord_session *s, bool muted, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_set_voice_deafen(ccord_session *s, bool deafened, ccord_error *err);
bool ccord_session_voice_muted(const ccord_session *s);
bool ccord_session_voice_deafened(const ccord_session *s);
// Requires an active voice connection in the guild; config selects encoder/framework/resolution/fps.
[[nodiscard]] ccord_status ccord_session_start_stream(ccord_session *s, ccord_id guild_id, ccord_id channel_id,
                                                      const ccord_stream_config *config, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_stop_stream(ccord_session *s, ccord_id guild_id, ccord_error *err);

typedef struct ccord_watched_stream {
  ccord_id guild_id;
  ccord_id channel_id;
  ccord_id user_id;
  bool active;
  bool paused;
} ccord_watched_stream;

[[nodiscard]] ccord_status ccord_session_watch_stream(ccord_session *s, ccord_id guild_id, ccord_id channel_id,
                                                      ccord_id user_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_stop_watching(ccord_session *s, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_set_stream_paused(ccord_session *s, bool paused, ccord_error *err);
const ccord_watched_stream *ccord_session_watched_stream(const ccord_session *s);
// reply_to_id must be a message in the same channel; text follows send_message rules.
[[nodiscard]] ccord_status ccord_session_send_reply(ccord_session *s, ccord_id channel_id, ccord_id reply_to_id,
                                                    const char *utf8, size_t len, ccord_error *err);
// Toggles the pinned flag of an existing message.
[[nodiscard]] ccord_status ccord_session_toggle_pin(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err);

// Server administration. `req` comes from the ccord_admin_* builders (ccord/net/admin.h). The request is checked against
// what the store knows (the server/channel exists, you are not kicking yourself or the owner), then sent; the call
// returns at once. Nothing changes locally until Discord confirms: success arrives as the usual events (channel,
// member and server updates), failure as an action failure (see ccord_session_pop_failure).
// Permissions are deliberately not pre-checked here: the store's view is only an approximation (channel overwrites),
// and Discord's answer ("Missing Permissions") is the authority.
[[nodiscard]] ccord_status ccord_session_admin(ccord_session *s, const ccord_admin_request *req, ccord_error *err);
// Same, with an explicit monotonic clock (ms) for the invite-preview cache; 0 means "use the real clock".
[[nodiscard]] ccord_status ccord_session_admin_ex(ccord_session *s, const ccord_admin_request *req, uint64_t now_ms,
                                                  ccord_error *err);

// ---- M6 account settings, active devices, and linked connections ----
// Discord owns this state: a settings PATCH goes out through the governed admin queue and the store keeps
// the old object until the full answer (or a USER_SETTINGS_UPDATE) arrives. Never a local optimistic write.
#define CCORD_SESSION_ACCOUNT_TTL_MS 60000u    // a fetched list is reused this long before it is fetched again
#define CCORD_SESSION_ACCOUNT_WAIT_MS 15000u   // an unanswered list request blocks an identical one this long
#define CCORD_SESSION_MAX_AUTH_SESSIONS 16u
#define CCORD_SESSION_MAX_CONNECTIONS 32u
[[nodiscard]] ccord_status ccord_session_user_settings_patch(ccord_session *s, const ccord_user_settings_patch *patch,
                                                             ccord_error *err);
// Active device sessions (GET /auth/sessions) and linked accounts (GET /users/@me/connections).
// A call while the list is loading (or still fresh) is a no-op; `_ex` takes a clock for tests (0 = real clock).
[[nodiscard]] ccord_status ccord_session_fetch_auth_sessions(ccord_session *s, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_fetch_auth_sessions_ex(ccord_session *s, uint64_t now_ms, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_fetch_connections(ccord_session *s, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_fetch_connections_ex(ccord_session *s, uint64_t now_ms, ccord_error *err);
// Logs out the listed session hashes in batches of CCORD_ADMIN_SESSION_LOGOUT_MAX so "log out everywhere else"
// works past one request. The cached list drops only the entries named by a 204 ack (never before); a failure
// on a later batch leaves earlier batches applied and is reported as an action failure.
[[nodiscard]] ccord_status ccord_session_logout_auth_sessions(ccord_session *s, const char hashes[][64],
                                                              size_t count, ccord_error *err);
// A successful edit arrival replaces only that cached entry; a successful delete arrival drops only that one.
[[nodiscard]] ccord_status ccord_session_edit_connection(ccord_session *s, const char *name, const char *id,
                                                          const ccord_connection_edit *edit, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_delete_connection(ccord_session *s, const char *name, const char *id,
                                                            ccord_error *err);
const ccord_auth_session *ccord_session_auth_sessions(const ccord_session *s, size_t *count);
const ccord_connection *ccord_session_connections(const ccord_session *s, size_t *count);
ccord_fetch_state ccord_session_auth_sessions_state(const ccord_session *s);
ccord_fetch_state ccord_session_connections_state(const ccord_session *s);

// ---- media, file upload, search, DMs, notifications, voice PCM ----
typedef struct ccord_media_item {
  const char *url;
  const char *cache_path;
  const uint8_t *data;
  size_t len;
} ccord_media_item;

typedef struct ccord_notification {
  ccord_id guild_id;
  ccord_id channel_id;
  ccord_id message_id;
  ccord_id author_id;
  bool is_dm;
  bool is_mention;
} ccord_notification;

[[nodiscard]] ccord_status ccord_session_fetch_media(ccord_session *s, const char *url, ccord_error *err);
// Like ccord_session_fetch_media, for a video the user clicked: the transport may allow third-party https hosts.
[[nodiscard]] ccord_status ccord_session_fetch_media_user(ccord_session *s, const char *url, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_request_media(ccord_session *s, const char *url, ccord_error *err);
bool ccord_session_pop_media(ccord_session *s, ccord_media_item *out);
// Pops the oldest unread media item whose URL contains `url_substring` (e.g. "/soundboard-sounds/") without
// touching, re-queuing, or updating LRU ticks of any other unread items. Returns false in O(1) when none are unread.
[[nodiscard]] bool ccord_session_pop_media_matching(ccord_session *s, const char *url_substring,
                                                    const char **url, const uint8_t **data, size_t *len);
// The returned item is valid until the next ccord_session_pump (a newer download may evict it).
const ccord_media_item *ccord_session_get_media(const ccord_session *s, const char *url);
// True when the most recent attempt to load `url` failed (blocked host, HTTP error, too large, timed out). A failed
// URL is no longer pending, so ccord_session_fetch_media asks the transport again: callers pace their retries.
bool ccord_session_media_failed(const ccord_session *s, const char *url);
// Reports live download byte progress for an in-flight fetch_media(url). Returns true when progress is active.
bool ccord_session_media_progress(const ccord_session *s, const char *url, uint64_t *out_now, uint64_t *out_total);

// Resolves a third-party video page URL (YouTube, X/Twitter, etc.) into direct HTTPS stream URL(s).
// Non-blocking: assigns `*out_req_id` and queues work on the transport; results arrive via
// ccord_session_pop_video_resolved().
[[nodiscard]] ccord_status ccord_session_resolve_video(ccord_session *s, const char *url, uint64_t *out_req_id,
                                                       ccord_error *err);
bool ccord_session_pop_video_resolved(ccord_session *s, ccord_video_resolved *out);

// Posts a poll (POST /channels/{id}/messages {"poll":...}) with the same channel checks as an ordinary send. Returns
// once the request is queued; a later refusal surfaces as the usual failed-send notice.
// Ask for the report menu of a type. Refused with the 'Reconnecting' error while stale.
[[nodiscard]] ccord_status ccord_session_report_menu_request(ccord_session *s, ccord_report_type type, ccord_error *err);
// Submit a report built from the menu walk. CCORD_ERR_INVALID (with a sentence for the user) for your own message or
// yourself, an unknown or unconfirmed message, or a missing user id.
[[nodiscard]] ccord_status ccord_session_report_submit(ccord_session *s, const ccord_report_request *req, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_send_poll(ccord_session *s, ccord_id channel_id, const ccord_poll_create *poll,
                                                   ccord_error *err);
[[nodiscard]] ccord_status ccord_session_send_message_with_file(ccord_session *s, ccord_id channel_id,
                                                                const char *utf8, size_t len,
                                                                const char *filename, const void *file_data,
                                                                size_t file_size, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_send_message_with_files(ccord_session *s, ccord_id channel_id,
                                                                 ccord_id reply_to_id, bool mention_author,
                                                                 const char *utf8, size_t len,
                                                                 const ccord_upload_file *files, size_t file_count,
                                                                 ccord_error *err);
// Voice message: uploads one Ogg/Opus clip as a message flagged CCORD_MSGFLAG_VOICE_MESSAGE with `duration_ms` and
// the base64 `waveform_b64` (empty/null for none). Permission and channel checks are the ones for an ordinary send.
[[nodiscard]] ccord_status ccord_session_send_voice_message(ccord_session *s, ccord_id channel_id,
                                                            const uint8_t *ogg, size_t len, uint32_t duration_ms,
                                                            const char *waveform_b64, ccord_error *err);
// Active media/file uploads in flight (`channel_id == CCORD_ID_NONE` = all channels; else filtered to `channel_id`).
// Copies up to `max` entries into `out` (when `out` is non-null and `max > 0`) and returns the matching active count.
size_t ccord_session_active_uploads(const ccord_session *s, ccord_id channel_id,
                                    ccord_upload_progress *out, size_t max);
[[nodiscard]] ccord_status ccord_session_send_reply_ex(ccord_session *s, ccord_id channel_id, ccord_id reply_to_id,
                                                       bool mention_author, const char *utf8, size_t len,
                                                       ccord_error *err);
[[nodiscard]] ccord_status ccord_session_dismiss_ephemeral(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                                           ccord_error *err);
[[nodiscard]] ccord_status ccord_session_search_messages(ccord_session *s, ccord_id guild_id, ccord_id channel_id,
                                                         const char *query, size_t query_len, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_search_messages_page(ccord_session *s, ccord_id guild_id, ccord_id channel_id,
                                                              const char *query, size_t query_len, uint32_t offset,
                                                              ccord_error *err);
size_t ccord_session_search_result_count(const ccord_session *s);
const ccord_message *ccord_session_search_result_at(const ccord_session *s, size_t index);
const ccord_message *ccord_session_search_results(const ccord_session *s, size_t *out_count);
uint32_t ccord_session_search_total_results(const ccord_session *s);
uint32_t ccord_session_search_offset(const ccord_session *s);
bool ccord_session_search_loading(const ccord_session *s);

// Opens (materialising if needed) and selects a thread channel, requesting its message history.
[[nodiscard]] ccord_status ccord_session_open_thread(ccord_session *s, ccord_id guild_id, ccord_id parent_id,
                                                     ccord_id thread_id, const char *name, ccord_error *err);

// Requests the users who reacted with `emoji` on a message (GET /channels/{c}/messages/{m}/reactions/{emoji}?limit=N).
[[nodiscard]] ccord_status ccord_session_request_reaction_users(ccord_session *s, ccord_id channel_id,
                                                                ccord_id message_id, const char *emoji,
                                                                uint32_t limit, ccord_error *err);
const ccord_user *ccord_session_reaction_users(const ccord_session *s, ccord_id *out_channel_id,
                                               ccord_id *out_message_id, const char **out_emoji,
                                               size_t *out_count, bool *out_loading);

[[nodiscard]] ccord_status ccord_session_open_dm(ccord_session *s, ccord_id recipient_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_request_profile(ccord_session *s, ccord_id user_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_set_relationship(ccord_session *s, ccord_id user_id,
                                                          ccord_relationship_kind kind, ccord_error *err);
bool ccord_session_pop_notification(ccord_session *s, ccord_notification *out);

[[nodiscard]] ccord_status ccord_session_push_voice_pcm(ccord_session *s, const int16_t *pcm,
                                                        size_t samples, ccord_error *err);
size_t ccord_session_pull_voice_pcm(ccord_session *s, int16_t *pcm, size_t max_samples);

// ---- expression picker (emoji / GIF / sticker data) ----
// Asks the transport for REST-backed picker data. The reply lands in the store's expression blob cache
// (ccord_store_expr_blob*, store/expressions.h). Duplicate requests while one is pending are dropped; READY data is
// only refetched when `force` is set. Unsupported transports mark the entry FAILED.
[[nodiscard]] ccord_status ccord_session_request_expression_data(ccord_session *s, ccord_expr_kind kind, const char *arg,
                                                                 bool force, ccord_error *err);
// Sends a sticker-only message (POST /channels/{id}/messages with sticker_ids).
[[nodiscard]] ccord_status ccord_session_send_sticker(ccord_session *s, ccord_id channel_id, ccord_id sticker_id,
                                                      ccord_error *err);
// Toggles `gif` in the store's favorite-GIF list and writes the updated list back to Discord via lossless
// FrecencyUserSettings re-encoding (PATCH /users/@me/settings-proto/2, preserving every other wire field).
[[nodiscard]] ccord_status ccord_session_toggle_favorite_gif(ccord_session *s, const ccord_gif_item *gif,
                                                             ccord_error *err);

// ---- forum channels (type 15 / 16) ----
[[nodiscard]] ccord_status ccord_session_request_forum_posts(ccord_session *s, const ccord_forum_query *query,
                                                             ccord_error *err);
[[nodiscard]] ccord_status ccord_session_create_forum_post(ccord_session *s, ccord_id forum_id,
                                                           const char *title, size_t title_len,
                                                           const char *content, size_t content_len,
                                                           const ccord_id *tag_ids, size_t tag_count,
                                                           ccord_error *err);
[[nodiscard]] ccord_status ccord_session_open_forum_post(ccord_session *s, ccord_id thread_id, ccord_error *err);

// ---- polls, message components, interaction modals, and slash commands ----
[[nodiscard]] ccord_status ccord_session_vote_poll(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                                   const uint32_t *answer_ids, size_t answer_count,
                                                   ccord_error *err);
[[nodiscard]] ccord_status ccord_session_send_interaction(ccord_session *s, const ccord_interaction_request *req,
                                                          ccord_error *err);
bool ccord_session_pop_interaction_modal(ccord_session *s, ccord_interaction_modal *out);
bool ccord_session_ensure_slash_commands(ccord_session *s, ccord_id guild_id, ccord_id channel_id, uint64_t now_ms);
const ccord_slash_choice *ccord_session_slash_choices(const ccord_session *s, size_t *out_count, uint64_t *out_seq);
void ccord_session_clear_slash_choices(ccord_session *s);

// ---- server folders, invite preview, and server discovery (M3b) ----
#define CCORD_DISCOVERY_TTL_MS 60000u
const ccord_invite_preview *ccord_session_invite_preview(const ccord_session *s, bool *out_loading);
void ccord_session_clear_invite_preview(ccord_session *s);
[[nodiscard]] ccord_status ccord_session_invite_fetch(ccord_session *s, const char *code_or_url, ccord_error *err);
const ccord_invite_preview *ccord_session_invite_lookup(const ccord_session *s, const char *code_or_url,
                                                        bool *out_loading, bool *out_failed);
[[nodiscard]] ccord_status ccord_session_request_discovery(ccord_session *s, const char *query, uint32_t category_id,
                                                           uint32_t offset, uint64_t now_ms, bool force,
                                                           ccord_error *err);
const ccord_discover_guild *ccord_session_discovery_results(const ccord_session *s, size_t *out_count,
                                                            uint32_t *out_total, uint32_t *out_offset,
                                                            bool *out_loading);

// ---- social parity: full user profiles & custom status (M3c) ----
#define CCORD_PROFILE_TTL_MS 60000u
[[nodiscard]] ccord_status ccord_session_request_profile_ex(ccord_session *s, ccord_id user_id, ccord_id guild_id,
                                                            uint64_t now_ms, bool force, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_set_custom_status(ccord_session *s, const char *emoji_name, ccord_id emoji_id,
                                                           const char *text, int64_t expires_unix_ms,
                                                           ccord_error *err);

// ---- own profile edits ----
// Attaches the REST backend that carries PATCH /users/@me and /users/@me/profile (ccord_profile_rest_create_live
// for a real account, ccord_profile_rest_create_mock under --mock). The session takes ownership, destroys any
// previous one, and pumps it from ccord_session_pump. Pass null to detach.
[[nodiscard]] ccord_status ccord_session_set_profile_rest(ccord_session *s, ccord_profile_rest *rest);
// Sends only the fields flagged in `edit` (see profile_edit.h for limits). Returns CCORD_OK once queued; `cb` then
// runs exactly once from ccord_session_pump with Discord's answer. When Discord accepted fields, the store's own
// user/profile already holds them when `cb` runs; a refusal (res->status != CCORD_OK, res->http_status, per-field
// res->field_errors) leaves the store unchanged. Any error return means nothing was sent and `cb` will not run:
// CCORD_ERR_INVALID/RANGE (validation), CCORD_ERR_UNSUPPORTED (no backend attached), CCORD_ERR_NOT_FOUND (no self).
[[nodiscard]] ccord_status ccord_session_edit_profile(ccord_session *s, const ccord_profile_edit *edit,
                                                      ccord_profile_edit_cb cb, void *ud, ccord_error *err);
// Owned Discord Collectibles inventory (GET /users/@me/collectibles-purchases + /user-profile-effects).
// TTL-guarded (120 s) unless `force` is true; deduplicates in-flight fetches.
[[nodiscard]] ccord_status ccord_session_fetch_collectibles(ccord_session *s, bool force, ccord_error *err);
ccord_fetch_state ccord_session_collectibles_state(const ccord_session *s);
const ccord_owned_collectible *ccord_session_collectibles(const ccord_session *s, size_t *out_count);
const char *ccord_session_collectibles_error(const ccord_session *s);
const ccord_profile_effect_config *ccord_session_find_owned_effect(const ccord_session *s, ccord_id effect_id);
const ccord_profile_frame_config *ccord_session_find_owned_frame(const ccord_session *s, ccord_id sku_id);

// ---- M5 voice & video intents (implemented by m5-session-mock / m5-live-transport) -----------------------------
// DM calls: OP 4 VOICE_STATE_UPDATE {guild_id:null, channel_id} joins/leaves; ringing is passive (CALL_CREATE).
[[nodiscard]] ccord_status ccord_session_call_join(ccord_session *s, ccord_id channel_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_call_leave(ccord_session *s, ccord_id channel_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_session_play_soundboard(ccord_session *s, ccord_id guild_id, ccord_id sound_id,
                                                         ccord_error *err);
// Per-user playback gain, 0..200 (100 = unity); forwarded to the live voice connection.
[[nodiscard]] ccord_status ccord_session_set_user_volume(ccord_session *s, ccord_id user_id, uint8_t pct,
                                                         ccord_error *err);
[[nodiscard]] ccord_status ccord_session_set_voice_config(ccord_session *s, const ccord_voice_config *cfg,
                                                          ccord_error *err);
// Per-frame push-to-talk state from the app's key polling (no REST/gateway traffic).
[[nodiscard]] ccord_status ccord_session_set_ptt_held(ccord_session *s, bool held, ccord_error *err);
// The last key state given to ccord_session_set_ptt_held (false before any). Read-only; earcons use it for the PTT tones.
bool ccord_session_ptt_held(const ccord_session *s);
// Go Live: send one encoded H.264 access unit / poll the next received one (render queue). The frame poll
// returns a pointer into a session-owned scratch buffer (valid until the next poll / session destroy).
[[nodiscard]] ccord_status ccord_session_stream_send_frame(ccord_session *s, const uint8_t *annexb, size_t len,
                                                           uint32_t ts90k, bool keyframe, ccord_error *err);
bool ccord_session_stream_poll_frame(ccord_session *s, const uint8_t **annexb, size_t *len, bool *keyframe);
[[nodiscard]] ccord_status ccord_session_voice_send_frame(ccord_session *s, const uint8_t *annexb, size_t len,
                                                          uint32_t ts90k, bool keyframe, ccord_error *err);
bool ccord_session_consume_keyframe_request(ccord_session *s);
// Remote voice cameras of the call self is in. Both return 0/false outside voice.
// camera_senders lists the announced senders (`decoding` ones are the at most CCORD_CAM_DECODE_MAX delivered ones);
// camera_poll_frame pops the oldest H.264 Annex-B access unit of one decoding sender into a session-owned buffer valid
// until the next camera poll / session destroy (separate from the Go Live buffer). The texture key is
// "__cam__:<decimal user id>" (cap >= CCORD_CAM_TEXTURE_KEY_MAX).
#define CCORD_CAM_TEXTURE_KEY_MAX 40u
size_t ccord_session_camera_senders(ccord_session *s, ccord_camera_rx *out, size_t max);
bool ccord_session_camera_poll_frame(ccord_session *s, ccord_id user_id, const uint8_t **annexb, size_t *len,
                                     bool *keyframe);
void ccord_session_camera_texture_key(ccord_id user_id, char *out, size_t cap);
// Go Live stream audio: 48 kHz mono S16LE. Send is gated on an active broadcast; poll is gated on a
// watched broadcast and returns the number of samples written (0 when nothing is queued).
[[nodiscard]] ccord_status ccord_session_stream_send_pcm(ccord_session *s, const int16_t *pcm, size_t samples,
                                                         ccord_error *err);
size_t ccord_session_stream_poll_pcm(ccord_session *s, int16_t *out_pcm, size_t cap_samples);
// Incoming DM call ring state (nullptr when none) and the stage instance of a channel (nullptr when none).
const ccord_call_state *ccord_session_incoming_call(const ccord_session *s);
const ccord_stage_instance *ccord_session_stage_instance(const ccord_session *s, ccord_id channel_id);
// RTC debug panel snapshots; zeroed when no voice connection is active.
void ccord_session_voice_stats(const ccord_session *s, ccord_rtc_stats *out);
void ccord_session_dave_status(const ccord_session *s, ccord_dave_status *out);

// ---- messaging parity: TTS, forward, crosspost, mark unread, saved messages ----
// Sends a text-to-speech message (POST /channels/{c}/messages with tts=true). Text rules as send_message;
// Discord requires SEND_TTS_MESSAGES. Transports without the hook return CCORD_ERR_UNSUPPORTED.
[[nodiscard]] ccord_status ccord_session_send_tts_message(ccord_session *s, ccord_id channel_id, const char *utf8,
                                                          size_t len, ccord_error *err);
// Forwards a message into target_channel_id (message_reference type 1). The source message must be known to the
// store; the forward appears when Discord echoes the MESSAGE_CREATE.
[[nodiscard]] ccord_status ccord_session_forward_message(ccord_session *s, ccord_id target_channel_id,
                                                         ccord_id source_channel_id, ccord_id message_id,
                                                         ccord_error *err);
// Crossposts an announcement-channel message to its followers (POST /channels/{c}/messages/{m}/crosspost).
// The crossposted flag arrives with the MESSAGE_UPDATE; nothing changes locally before that.
[[nodiscard]] ccord_status ccord_session_crosspost_message(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                                           ccord_error *err);
// Marks the channel unread from message_id onward: acknowledges the preceding message (POST .../ack) and moves
// the local read position (ccord_store_mark_unread). message_id must be a known message of the channel.
[[nodiscard]] ccord_status ccord_session_mark_unread(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                                     ccord_error *err);
// Bookmarks / unbookmarks a message locally (the Saved Messages popover). The message must be known to the store;
// the saved list is deep-copied and persisted by the app across runs.
[[nodiscard]] ccord_status ccord_session_save_message(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                                      ccord_error *err);
[[nodiscard]] ccord_status ccord_session_unsave_message(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                                        ccord_error *err);
bool ccord_session_message_saved(const ccord_session *s, ccord_id channel_id, ccord_id message_id);
size_t ccord_session_saved_message_count(const ccord_session *s);
const ccord_message *ccord_session_saved_message_at(const ccord_session *s, size_t i);   // 0 = most recently saved

// ---- stage lifecycle & guild scheduled events ----
// Starts a stage instance in a stage channel (POST /stage-instances via the governed admin queue). `topic` may be
// nullptr / ""; privacy level is GUILD_ONLY. Nothing changes locally until Discord confirms (STAGE_INSTANCE_UPDATE).
[[nodiscard]] ccord_status ccord_session_stage_start(ccord_session *s, ccord_id guild_id, ccord_id channel_id,
                                                     const char *topic, ccord_error *err);
// Ends the channel's stage instance (DELETE /stage-instances/{channel_id}).
[[nodiscard]] ccord_status ccord_session_stage_end(ccord_session *s, ccord_id channel_id, ccord_error *err);
// Requests the guild's scheduled-event list (GET /guilds/{g}/scheduled-events?with_user_count=true); the answer
// arrives through pump() as SCHEDULED_EVENT_LIST. Create / modify / delete ride ccord_session_admin with the
// ccord_admin_event_* builders; their answers arrive as SCHEDULED_EVENT_UPDATE.
[[nodiscard]] ccord_status ccord_session_request_scheduled_events(ccord_session *s, ccord_id guild_id,
                                                                  ccord_error *err);
// RSVPs (rsvp=true) or withdraws the signed-in user's RSVP (PUT / DELETE .../scheduled-events/{e}/users/@me).
[[nodiscard]] ccord_status ccord_session_rsvp_scheduled_event(ccord_session *s, ccord_id guild_id, ccord_id event_id,
                                                              bool rsvp, ccord_error *err);

// ---- membership screening ----
// GET /guilds/{g}/member-verification?with_guild=true through the governed admin queue. The answer arrives through
// pump() as CCORD_NET_MEMBER_VERIFICATION; until then the cached form (if any) is unchanged.
[[nodiscard]] ccord_status ccord_session_member_verification_fetch(ccord_session *s, ccord_id guild_id,
                                                                   ccord_error *err);
// The guild's cached form, or nullptr when none is known. Borrowed from the session; valid until the next pump().
const ccord_member_verification *ccord_session_member_verification(const ccord_session *s, ccord_id guild_id);
// PUT /guilds/{g}/requests/@me with the cached form. Discord's confirmation clears the pending flag; nothing
// changes locally before that.
[[nodiscard]] ccord_status ccord_session_member_verification_submit(ccord_session *s, ccord_id guild_id,
                                                                    ccord_error *err);
// True while the signed-in member still has to pass screening in the guild (ccord_member.pending).
bool ccord_session_self_pending(const ccord_session *s, ccord_id guild_id);

// ---- embedded activities ----
// Requests the guild's activity shelf (GET /activities/shelf?guild_id={g}); the answer arrives through pump() as
// CCORD_NET_ACTIVITY_SHELF and is cached per guild. A call is a no-op (CCORD_OK, nothing sent) while the same guild's
// shelf is loading, and for 60 s after an answer: the shelf rarely changes, so a UI may call this every time its
// shelf opens. The cache is dropped on a reconnect/snapshot and on a transport switch.
[[nodiscard]] ccord_status ccord_session_activity_shelf_fetch(ccord_session *s, ccord_id guild_id, ccord_error *err);
// The cached shelf of `guild_id` (borrowed; null / 0 until the first answer).
const ccord_activity_app *ccord_session_activity_shelf(const ccord_session *s, ccord_id guild_id, size_t *out_count);
// Launches the app in the voice channel (POST /activities/{c}/{app}); launching also joins the instance. The running
// instance arrives through pump() as CCORD_NET_ACTIVITY_INSTANCE_UPDATE.
[[nodiscard]] ccord_status ccord_session_activity_launch(ccord_session *s, ccord_id channel_id, ccord_id application_id,
                                                         ccord_error *err);
// Leaves the instance (POST .../instances/{instance}/leave). `instance` must carry instance_id / application_id /
// location_id (the channel_id); the local row disappears when Discord confirms.
[[nodiscard]] ccord_status ccord_session_activity_leave(ccord_session *s, const ccord_activity_instance *instance,
                                                        ccord_error *err);
// The channel's running instances (borrowed store rows; null / 0 when none).
const ccord_activity_instance *ccord_session_activity_instances(const ccord_session *s, ccord_id channel_id,
                                                                size_t *out_count);

// ---- guild profile (clan tag card) ----
// Fetches GET /guilds/{guild_id}/profile via the governed admin queue (60s TTL + in-flight cache).
[[nodiscard]] ccord_status ccord_session_guild_profile_fetch(ccord_session *s, ccord_id guild_id, ccord_error *err);
const ccord_guild_profile *ccord_session_guild_profile(const ccord_session *s, ccord_id guild_id);
bool ccord_session_guild_profile_loading(const ccord_session *s, ccord_id guild_id);

// ---- Plugin API v3 raw gateway dispatch tap ----
typedef void (*ccord_session_raw_gateway_cb)(void *ud, const char *event_name, const char *payload_json);
void ccord_session_set_raw_gateway_cb(ccord_session *s, ccord_session_raw_gateway_cb cb, void *ud);
