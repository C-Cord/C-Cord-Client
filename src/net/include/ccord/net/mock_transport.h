// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/net/gateway_pace.h>
#include <ccord/net/transport.h>
#include <ccord/net/voice_reconnect.h>
typedef struct ccord_mock_config {
  uint64_t seed;                 // 0 -> fixed default 0xC0FFEE; same seed => identical data
  uint32_t latency_ms;           // delay before each reply becomes pollable; 0 = next poll
  uint32_t big_channel_messages; // size of the stress channel (#general of server 1); 0 -> 5000
  uint32_t ambient_interval_ms;  // >0: a bot posts into #general of server 1 every N ms; 0 = off (default)
  int64_t base_time_ms;          // timestamp of the newest seeded message; 0 -> 2026-11-01T12:00:00Z
  uint64_t (*now_ms)(void *ud);  // injectable monotonic clock; nullptr -> ccord_clock_ms
  void *clock_ud;
  bool refuse_writes;            // send/reply/edit are accepted by the call, then refused like a 403 (CCORD_NET_ACTION_FAILED + draft)
} ccord_mock_config;
// Dataset (deterministic): 3 guilds ("C-Cord HQ", "Lua Lounge", "Pixel Garden"), each with 2 categories
// (TEXT CHANNELS, VOICE CHANNELS), 4-6 text and 2 voice channels (names like general, announcements, dev, off-topic;
// "General", "Gaming"), 12 users incl. self "You" and 2 bots, mixed presence, roles Admins/Moderators/Members.
// Server 1 #general has big_channel_messages (multi-line, long-word, URL-like and emoji-free texts, bursts by one author for grouping);
// other text channels 20-200 messages; channel #empty in server 3 has 0 messages. Message i of channel c always has the same text for a seed.
// send_message echoes a message authored by self_id (id = last id + 1, timestamp = base_time + elapsed).
ccord_transport *ccord_mock_transport_create(const ccord_mock_config *cfg, ccord_error *err);   // cfg may be nullptr
// Test hook: from now on writes are refused (see ccord_mock_config.refuse_writes) or accepted again. `t` must be a mock.
void ccord_mock_set_refuse_writes(ccord_transport *t, bool refuse);
// Test hook: create_poll is accepted, then fails later (CCORD_NET_ACTION_FAILED, CCORD_ACTION_CREATE_POLL) with `reason`.
// nullptr or "" turns it off. `t` must be a mock.
void ccord_mock_set_late_poll_failure(ccord_transport *t, const char *reason);
// Report flow test hooks. Counts: menu GETs so far / submits accepted so far. last_report: deep copy of the last accepted request
// (nullptr before the first); a late failure records nothing.
size_t ccord_mock_report_menu_count(const ccord_transport *t);
size_t ccord_mock_report_submit_count(const ccord_transport *t);
const ccord_report_request *ccord_mock_last_report(const ccord_transport *t);
void ccord_mock_set_report_menu_failure(ccord_transport *t, const char *reason);   // menu GET fails later (ACTION_FAILED REPORT_MENU); ""/nullptr = off
void ccord_mock_set_late_report_failure(ccord_transport *t, const char *reason);   // submit accepted, then ACTION_FAILED REPORT with reason
void ccord_mock_set_report_broken_link(ccord_transport *t, bool on);               // message menu node 1 gets a child to missing node 999
// Voice outage simulation. The mock has no voice WebSocket, so ccord_mock_voice_drop computes the whole outage now,
// with the real ccord_vc policy (voice_reconnect.h) on the mock clock, and queues timed events: VOICE_LINK
// RECONNECTING (attempt 1 now, one per further attempt), then CONNECTED when an attempt gets through, or the give-up
// (own VOICE_STATE_UPDATE channel 0, VOICE_LINK FAILED, ACTION_FAILED/CCORD_ACTION_VOICE with the reason).
// A REJOIN success also queues VOICE_STATE_UPDATE + VOICE_SERVER_UPDATE (token "mock-voice-token-2") and, if self
// streams, a STREAM_SIGNAL DELETE ("voice reconnected"). Leaving or joining voice cancels the queued outage.
typedef enum ccord_mock_vdrop_kind : uint8_t {
  CCORD_MOCK_VDROP_RESUMABLE,   // WS lost, no close code: resume works once down_ms has passed and fail_attempts attempts failed
  CCORD_MOCK_VDROP_REJOIN,      // close 4006: resume impossible; the server re-issues VOICE_STATE_UPDATE + VOICE_SERVER_UPDATE
  CCORD_MOCK_VDROP_AWAIT_MAIN,  // close 4014: no verdict arrives; give up after 3 s without leaving
  CCORD_MOCK_VDROP_FATAL        // close 4021: give up at once
} ccord_mock_vdrop_kind;
typedef struct ccord_mock_vdrop {
  ccord_mock_vdrop_kind kind;
  uint32_t down_ms;             // how long the "network" stays down: an attempt waits for it (up to CCORD_VC_CONNECT_TIMEOUT_MS), longer ones time out
  uint32_t fail_attempts;       // RESUMABLE/REJOIN: this many attempts fail before one can succeed; UINT32_MAX = never
} ccord_mock_vdrop;
typedef struct ccord_mock_vlink_stats {
  uint32_t drops;                         // drop calls so far
  uint32_t attempts, resumes, rejoins, give_ups;   // of the last drop call (computed when it is called)
  uint64_t attempt_ms[8];                 // mock-clock time of each attempt of the last drop call
  ccord_vc_action attempt_action[8];
  uint32_t leaves_sent;                   // of the last drop call: OP 4 leaves the simulated client sent (give-up with tell_server)
} ccord_mock_vlink_stats;
// CCORD_ERR_NOT_FOUND if self is not in a voice channel.
[[nodiscard]] ccord_status ccord_mock_voice_drop(ccord_transport *t, const ccord_mock_vdrop *d, ccord_error *err);
void ccord_mock_voice_link_stats(const ccord_transport *t, ccord_mock_vlink_stats *out);
// Gateway op 3 (set_presence / set_presence_ex) as the mock received it: how many, and the last one's presence and
// custom status text ("" when none; valid until the next call).
size_t ccord_mock_presence_send_count(const ccord_transport *t);
ccord_presence ccord_mock_last_presence_send(const ccord_transport *t, const char **custom_text);
// Queues CCORD_NET_SESSIONS_REPLACE carrying `overall` (the account's overall status as the session list reports it).
[[nodiscard]] ccord_status ccord_mock_inject_sessions_replace(ccord_transport *t, ccord_presence overall, ccord_error *err);
// The account's own "Playing <name>" as Discord reports it (nullptr = none). It becomes part of the mock's account, so a
// later SNAPSHOT (a fresh READY) carries it; with `echo` a PRESENCE_UPDATE for the own user follows.
// Default true: set_presence / set_presence_ex answer with a PRESENCE_UPDATE for the own user. False models Discord not
// sending a user its own presence (only the sessions list and READY tell it).
void ccord_mock_set_own_presence_echo(ccord_transport *t, bool echo);
[[nodiscard]] ccord_status ccord_mock_set_self_activity(ccord_transport *t, const char *name, bool echo, ccord_error *err);
// SESSIONS_REPLACE that lists the account's activity: another session started (name) or stopped (nullptr) it. Updates the
// mock's account the same way as ccord_mock_set_self_activity.
[[nodiscard]] ccord_status ccord_mock_inject_sessions_replace_activity(ccord_transport *t, ccord_presence overall,
                                                                       const char *activity_name, ccord_error *err);
// Type-1 settings proto (status). The mock keeps the "server" bytes (initially empty): set_status_proto patches them
// and records each PATCH. `_patch_at` is the raw proto sent, `_patch_body` the {"settings":"<base64>"} JSON.
void ccord_mock_set_status_proto_base(ccord_transport *t, const uint8_t *proto, size_t len);
const uint8_t *ccord_mock_status_proto_base(const ccord_transport *t, size_t *len);
size_t ccord_mock_status_proto_patch_count(const ccord_transport *t);
const uint8_t *ccord_mock_status_proto_patch_at(const ccord_transport *t, size_t i, size_t *len);
const char *ccord_mock_status_proto_patch_body(const ccord_transport *t, size_t i);
// Another client changed the account's type-1 settings: queues CCORD_NET_USER_SETTINGS_PROTO carrying `proto`
// (`partial` = a delta; otherwise the whole proto, which also becomes the mock's server bytes).
[[nodiscard]] ccord_status ccord_mock_inject_status_proto_update(ccord_transport *t, const uint8_t *proto, size_t len,
                                                                 bool partial, ccord_error *err);
// Another client changed the account's type-2 settings (favourite GIFs): queues CCORD_NET_USER_SETTINGS_PROTO carrying
// `proto` as raw_proto (`partial` = a delta; otherwise the whole proto, which also becomes the mock's server bytes).
[[nodiscard]] ccord_status ccord_mock_inject_settings_proto_update(ccord_transport *t, const uint8_t *proto, size_t len,
                                                                   bool partial, ccord_error *err);
// Server administration (admin_action): the mock applies the change to its dataset and emits the events a real server
// would. Test hooks report what the transport was asked to do (the last request, and how many there were).
size_t ccord_mock_admin_count(const ccord_transport *t);
const ccord_admin_request *ccord_mock_last_admin(const ccord_transport *t);   // nullptr before the first request
// Request-budget hooks: how many times the client asked for each read-style request. UI tests use them to prove
// that typing in the search box, scrolling a busy channel and similar activity stay polite.
typedef enum ccord_mock_call : uint8_t {
  CCORD_MOCK_CALL_SEARCH = 0,
  CCORD_MOCK_CALL_ACK,
  CCORD_MOCK_CALL_HISTORY,
  CCORD_MOCK_CALL_TYPING,
  CCORD_MOCK_CALL_MEMBERS,     // requests for more of the open channel's member list
  CCORD_MOCK_CALL_SLASH_INDEX, // GET /channels/{id}/application-command-index
  CCORD_MOCK_CALL_PINS,        // GET /channels/{id}/pins or /messages/pins
  CCORD_MOCK_CALL_REACTION_USERS, // GET /channels/{c}/messages/{m}/reactions/{e}
  CCORD_MOCK_CALL_DISCOVERY,   // GET /discoverable-guilds
  CCORD_MOCK_CALL_PROFILE,     // GET /users/{id}/profile
  CCORD_MOCK_CALL_RING,        // POST /channels/{c}/call/ring: a DM / group call was started with nobody else in it
  CCORD_MOCK_CALL_VOICE_JOIN,  // a voice-state join: DM call (call_action JOIN) or guild voice (join_voice)
  CCORD_MOCK_CALL_COUNT
} ccord_mock_call;
size_t ccord_mock_call_count(const ccord_transport *t, ccord_mock_call which);
// The channel the last ring request (CCORD_MOCK_CALL_RING) was for; 0 before the first.
ccord_id ccord_mock_last_ring_channel(const ccord_transport *t);
// Test hook: the next rings are refused after acceptance, like a Discord 4xx (CCORD_NET_ACTION_FAILED, CCORD_ACTION_ADMIN)
// with `reason`; the caller stays in the call. nullptr or "" turns it off. `t` must be a mock.
void ccord_mock_set_ring_refusal(ccord_transport *t, const char *reason);
// How many hundreds of the member list the last CCORD_MOCK_CALL_MEMBERS request asked for (0 before the first).
uint32_t ccord_mock_last_member_chunks(const ccord_transport *t);
// Appends a rich message (poll, components, embeds, attachments) into a channel of the mock server and queues MESSAGE_CREATE.
[[nodiscard]] ccord_status ccord_mock_post_rich_message(ccord_transport *t, ccord_id channel_id, ccord_id author_id,
                                                        const char *content, const ccord_message_rich *rich,
                                                        ccord_id *out_id, ccord_error *err);
// Interaction test hooks (send_interaction). For MODAL_SUBMIT, values hold the submitted field_values in order.
size_t ccord_mock_interaction_count(const ccord_transport *t);
uint8_t ccord_mock_last_interaction_type(const ccord_transport *t);
uint8_t ccord_mock_last_interaction_component_type(const ccord_transport *t);
const char *ccord_mock_last_interaction_custom_id(const ccord_transport *t);
size_t ccord_mock_last_interaction_value_count(const ccord_transport *t);
const char *ccord_mock_last_interaction_value(const ccord_transport *t, size_t index);
// Simulates an incoming DM message request from `sender_id` (optionally with an initial message and spam flag)
// by creating/upserting the DM channel with is_message_request=true and queueing CHANNEL_UPSERT (+ MESSAGE_CREATE).
[[nodiscard]] ccord_status ccord_mock_push_message_request(ccord_transport *t, ccord_id sender_id,
                                                           const char *initial_message, bool is_spam,
                                                           ccord_id *out_channel_id, ccord_error *err);
// Simulates `user_id` joining `guild_id` on the mock server and queues a GUILD_MEMBERS event.
[[nodiscard]] ccord_status ccord_mock_add_guild_member(ccord_transport *t, ccord_id guild_id, ccord_id user_id,
                                                       ccord_error *err);
// ---- M5 voice & video simulation ----
// Simulates an incoming DM call from `recipient_id`: creates/reuses the DM channel (CHANNEL_UPSERT),
// queues CALL_CREATE ringing self plus the caller's VOICE_STATE_UPDATE. Ringing is passive: the client
// answers with ccord_transport_call_action(JOIN/LEAVE). out_channel_id (optional) gets the DM channel id.
[[nodiscard]] ccord_status ccord_mock_push_incoming_call(ccord_transport *t, ccord_id recipient_id,
                                                         ccord_id *out_channel_id, ccord_error *err);
// Simulates the remote side hanging up: every participant's VOICE_STATE_UPDATE (channel 0) then CALL_DELETE.
[[nodiscard]] ccord_status ccord_mock_end_call(ccord_transport *t, ccord_id channel_id, ccord_error *err);
// DAVE/RTC snapshots of the simulated voice connection (zeroed when not connected to voice). The mock
// never claims a verified E2EE checkmark: dave.verified stays false.
void ccord_mock_dave_status(const ccord_transport *t, ccord_dave_status *out);
void ccord_mock_rtc_stats(const ccord_transport *t, ccord_rtc_stats *out);
// Local audio prefs as last set through the transport hooks (volume defaults to 100; config false until set).
uint8_t ccord_mock_user_volume(const ccord_transport *t, ccord_id user_id);
bool ccord_mock_voice_config(const ccord_transport *t, ccord_voice_config *out);
// Outbound Go Live media still queued (0 after stop_stream/leave_voice/disconnect). Tests use these to prove the
// media plane is bounded and cleared, not just that poll_stream_* comes back empty.
size_t ccord_mock_stream_frame_count(const ccord_transport *t);
size_t ccord_mock_stream_pcm_count(const ccord_transport *t);
// ---- Remote voice cameras ----
// on=true: puts `user_id` (a seeded mock user other than self; 1005..1009 are in no call at seed time) into voice
// `channel_id` of `guild_id` (moves it if it is elsewhere in the guild) with self_video=true, queues the voice-state
// event and registers it as a camera sender: decoding while fewer than CCORD_CAM_DECODE_MAX senders are, else waiting.
// on=false: self_video=false, event queued, sender and FIFO removed, the earliest waiter promoted (new generation).
// Requires connected(). Senders are only listed to the session while self is in the same call.
[[nodiscard]] ccord_status ccord_mock_set_remote_camera(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                                        ccord_id user_id, bool on, ccord_error *err);
// Queues one H.264 Annex-B access unit from `user_id` (FIFO depth 4, AU <= CCORD_CAM_FRAME_MAX). A unit for a sender
// that is not decoding is dropped and reported OK (mirrors the gateway); an unknown sender is CCORD_ERR_NOT_FOUND;
// a full FIFO is CCORD_ERR_RANGE. The mock never encodes: it hands back the exact bytes pushed.
[[nodiscard]] ccord_status ccord_mock_push_camera_frame(ccord_transport *t, ccord_id user_id, const uint8_t *annexb,
                                                        size_t len, uint32_t ts90k, bool keyframe, ccord_error *err);
// Queued, undelivered units of `user_id` (0 for an unknown sender).
size_t ccord_mock_camera_frame_count(const ccord_transport *t, ccord_id user_id);
// ---- M6 account settings, devices & connections ----
// The mock's current account settings (the seeded Discord defaults until a PATCH changes them).
bool ccord_mock_user_settings(const ccord_transport *t, ccord_user_settings *out);
// Simulates a gateway USER_SETTINGS_UPDATE: stores `settings` and queues CCORD_NET_USER_SETTINGS (full object).
[[nodiscard]] ccord_status ccord_mock_push_user_settings(ccord_transport *t, const ccord_user_settings *settings,
                                                         ccord_error *err);
// ---- Embedded activities ----
// The shelf (GET /activities/shelf) holds three apps for every guild: application ids 97001 "Sketch Heads", 97002
// "Poker Night" and 97003 "Watch Together". Launching one in a voice channel the user is connected to creates (or
// joins) an instance with the user as participant; leaving removes the user, and the last one out ends it. A voice
// disconnect does NOT drop the user from an instance by itself: the client has to send the leave, as on Discord.
// Simulates another user's activity: sets the exact participant list (the signed-in user is NOT added) of the
// instance of `application_id` in the voice channel, creating it when none runs, and queues ACTIVITY_INSTANCE_UPDATE.
// An empty list ends a running instance. out_instance_id (optional) gets the instance id.
[[nodiscard]] ccord_status ccord_mock_push_activity_instance(ccord_transport *t, ccord_id channel_id,
                                                             ccord_id application_id, const ccord_id *participants,
                                                             size_t count, ccord_id *out_instance_id, ccord_error *err);
// Makes the next admin request fail like a Discord error (CCORD_NET_ACTION_FAILED); consumed once.
void ccord_mock_fail_next_admin(ccord_transport *t, bool fail);
// Media downloads whose URL contains `url_part` fail (a MEDIA_LOADED event with an error status and no bytes)
// until this is called again with nullptr or "".
void ccord_mock_fail_media(ccord_transport *t, const char *url_part);
// Rich Presence: the activities array last set through set_rich_activities (nullptr/0 before the first set or
// after a clear). The pointer is valid until the next set_rich_activities or destroy.
const char *ccord_mock_rich_activities(const ccord_transport *t, size_t *len);
// ---- Ranged media ----
// A registered file is a total length plus a callback that synthesises any byte range, so a file of 40+ MiB needs no
// buffer. A URL containing `url_part` is served by byte range as CCORD_NET_MEDIA_RANGE events (https:// only; http://
// is refused with CCORD_ERR_PERMISSION): 206 -> status OK, range_http 206, ACCEPTS (+ EOF when the body reaches the end).
// A start at or past the end answers like a 416: OK, range_http 416, empty body, ACCEPTS|EOF, range_total = the length.
// A URL that is not registered answers like a server that ignores Range: OK, UNRANGED, empty body, range_total -1.
typedef void (*ccord_mock_range_read_fn)(void *ud, int64_t off, uint8_t *dst, size_t n);   // fills file bytes [off, off+n)
typedef enum ccord_mock_range_flags : uint32_t {
  CCORD_MOCK_RANGE_IGNORE = 1u << 0,         // answer 200 (Range ignored): UNRANGED, empty body
  CCORD_MOCK_RANGE_UNKNOWN_TOTAL = 1u << 1,  // Content-Range total is "*": range_total -1, EOF only on a short body
  CCORD_MOCK_RANGE_REDIRECT_HTTP = 1u << 2,  // the redirect chain ends at an http:// URL: fails like curl (CCORD_ERR_IO)
} ccord_mock_range_flags;
typedef struct ccord_mock_range_file {
  const char *url_part;            // matched with strstr on the request url (copied; under 128 bytes)
  int64_t total;                   // virtual file length, >= 0
  ccord_mock_range_read_fn read;   // required
  void *ud;
  uint32_t flags;                  // ccord_mock_range_flags
  uint8_t redirect_hops;           // events report this many redirects (range_redirects)
  uint32_t fail_after_requests;    // 0 = never; else request number N and later answer fail_status
  int32_t fail_status;             // e.g. 403 (401/403/410 -> CCORD_ERR_PERMISSION, others CCORD_ERR_IO); range_http carries it
  char etag[96];                   // "" = none
} ccord_mock_range_file;
// At most 16 files; registering the same url_part again replaces it and resets its request count.
[[nodiscard]] ccord_status ccord_mock_serve_range_file(ccord_transport *t, const ccord_mock_range_file *f);

typedef struct ccord_mock_range_stats {
  uint32_t requests;               // valid fetch_media_range calls for registered files (failed answers included)
  uint64_t bytes_served;           // body bytes handed out
  int64_t  max_len;                // largest len asked
  uint32_t log_count;              // ring of the last 16 requests, oldest first
  int64_t  log_start[16];
  uint32_t log_len[16];
} ccord_mock_range_stats;
void ccord_mock_range_stats_get(const ccord_transport *t, ccord_mock_range_stats *out);

// ---- Gateway link model ----
// The mock models the gateway connection just enough to drive the real ccord_gwpace on the mock clock (cfg.now_ms):
// a heartbeat that notices a silently dead socket (zombie after one unanswered interval), reconnect planning,
// RESUME versus a fresh IDENTIFY (a fresh one delivers a new SNAPSHOT), and ccord_transport_network_event, which runs
// ccord_gwpace_plan_wake. Nothing changes until a fault below is injected or a network event arrives. While the link is
// dead or down the mock delivers only its own CONNECTING state events; everything else stays queued until it is back.
typedef struct ccord_mock_link_stats {
  uint32_t wakes_seen, wakes_accepted;   // network_event calls (IGNORED included) / those with verdict ACCEPTED
  ccord_gwpace_wake_verdict last_verdict;   // verdict of the latest network_event (IGNORED before the first)
  uint32_t attempts;                     // reconnect attempts started (a failed or refused one counts)
  uint32_t resumes, identifies;          // attempts that sent RESUME / a fresh IDENTIFY (a refused RESUME counts in neither)
  uint64_t next_attempt_in_ms;           // ms until the pending attempt; 0 when none is pending or it is due
  bool dead_silent;                      // the socket is dead and nothing has noticed yet
} ccord_mock_link_stats;
void ccord_mock_link_stats_get(const ccord_transport *t, ccord_mock_link_stats *out);
// Silent: the connection stops delivering events and only the heartbeat model notices (zombie within 1-2 heartbeat
// intervals, about 41-82 s of mock time). Loud: a close or receive error; CONNECTING at once and a backoff-paced
// reconnect. No effect unless the mock is connected and its link is up.
void ccord_mock_drop_gateway(ccord_transport *t, bool silent);
// While false every reconnect attempt fails like a refused socket open: the backoff grows. Default true.
void ccord_mock_set_network(ccord_transport *t, bool up);
// How long a dropped session stays resumable (default 120000 ms). After that a resume attempt is answered with op 9
// d:false, the reconnect waits 1-5 s, then sends a fresh IDENTIFY and a new SNAPSHOT follows.
void ccord_mock_set_session_ttl(ccord_transport *t, uint64_t ms);
