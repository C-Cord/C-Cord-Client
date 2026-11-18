// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/net/governor.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/error.h>
#include <ccord/core/link_event.h>
#include <ccord/net/gateway_pace.h>
#include <ccord/net/transport.h>
#include <ccord/net/voice_reconnect.h>

// Live Discord transport: HTTPS REST v9 + Gateway v9 WebSocket, plus the
// standalone C-Cord token loader. No payment or store route is ever reachable.

#define CCORD_LIVE_TOKEN_MAX 512u
#define CCORD_LIVE_API_BASE "https://discord.com/api/v9"
#define CCORD_LIVE_GATEWAY_URL "wss://gateway.discord.gg/?v=9&encoding=json"
#define CCORD_LIVE_HISTORY_DEFAULT 50u
#define CCORD_LIVE_TIMEOUT_DEFAULT_MS 15000u
#define CCORD_LIVE_MAX_RESPONSE (8u * 1024u * 1024u)
// Largest media body (image, GIF, video, audio) fetched for inline display; matches the decoders' input limit.
#define CCORD_LIVE_MAX_MEDIA (32u * 1024u * 1024u)
// Web-client capability bitmask sent in IDENTIFY (user accounts need these to
// receive lazy guild payloads, replies and attachment/embed metadata).
#define CCORD_LIVE_CAPABILITIES 1734653u

typedef struct ccord_live_config {
  const char *token;             // explicit token; null/empty -> env or standalone token file
  const char *token_file;        // optional token file override; null -> $XDG_CONFIG_HOME/ccord/token
  const char *api_base;          // null -> CCORD_LIVE_API_BASE
  const char *gateway_url;       // null -> CCORD_LIVE_GATEWAY_URL
  uint32_t connect_timeout_ms;   // 0 -> default
  uint32_t request_timeout_ms;   // 0 -> default
  const char *cache_dir;         // per-account dir (<data>/accounts/<fp>): scopes the API cache to <dir>/api and enables
                                 // warm history files; null = legacy shared API cache, no warm history
  bool disable_gateway;          // tests/offline: no socket is opened
  uint64_t (*now_ms)(void *ud);  // injectable clock; null -> ccord_clock_ms
  void *clock_ud;
} ccord_live_config;

// Wraps the token in a ccord_transport vtable. The token is copied and wiped on
// destroy; it is never logged.
[[nodiscard]] ccord_transport *ccord_live_transport_create(const ccord_live_config *cfg, ccord_error *err);

// "<cache_dir>/warm/history/<channel_id>.json"; false on null/empty/overflow. Written by the transport when a
// channel's initial history page arrives, read back by the warm-start loader.
[[nodiscard]] bool ccord_live_history_cache_path(const char *cache_dir, ccord_id channel_id, char *out, size_t cap);

// Environment variable checked before the standalone token file.
#define CCORD_LIVE_TOKEN_ENV "CCORD_DISCORD_TOKEN"

// Reads CCORD_DISCORD_TOKEN, else reads standalone token file at
// $XDG_CONFIG_HOME/ccord/token (or explicit token_path_or_null when provided).
// `out` is only written on success. `out` is NOT wiped on failure.
[[nodiscard]] ccord_status ccord_live_load_token(const char *token_path_or_null, char *out, size_t cap, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_token_path(char *out, size_t cap, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_save_token(const char *path_or_null, const char *token, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_delete_token(const char *path_or_null, ccord_error *err);
void ccord_live_wipe(void *p, size_t n);
const char *ccord_live_user_agent(void);

// Multi-account keyring ------------------------------------------------------
// Account metadata lives in $XDG_CONFIG_HOME/ccord/accounts.ini (0600); each
// account's token lives in $XDG_CONFIG_HOME/ccord/tokens/<slot_id> (0600) and
// the active account's token is mirrored atomically to the legacy single-token
// file so ccord_live_load_token/ccord_live_save_token keep working unchanged.
// The first list/switch/upsert/remove migrates an existing single token file to
// a "legacy" account without deleting or rewriting the original token.
#define CCORD_MAX_SAVED_ACCOUNTS 8u
#define CCORD_SAVED_ACCOUNT_NAME_MAX 128u
#define CCORD_SAVED_ACCOUNT_SLOT_MAX 64u
#define CCORD_SAVED_ACCOUNT_AVATAR_MAX 128u
#define CCORD_NO_ACTIVE_ACCOUNT (-1)

typedef struct ccord_saved_account {
  uint64_t user_id;                                  // 0 for a migrated token whose user is unknown
  char username[CCORD_SAVED_ACCOUNT_NAME_MAX];
  char display_name[CCORD_SAVED_ACCOUNT_NAME_MAX];
  char avatar_hash[CCORD_SAVED_ACCOUNT_AVATAR_MAX];
  char slot_id[CCORD_SAVED_ACCOUNT_SLOT_MAX];        // decimal user id unless the caller supplied one
  bool active;
} ccord_saved_account;

// Copies at most `cap` saved accounts into `out` (all of them when cap is
// large enough) and reports the total saved and the active index (-1 when no
// account is active). Implemented by token_store.c when CCORD_LIVE_ACCOUNT=1.
[[nodiscard]] ccord_status ccord_live_accounts_list(ccord_saved_account *out, size_t cap, size_t *out_count,
                                                    int *out_active_index, ccord_error *err);
// Inserts or updates an account and writes its token file. When make_active is
// true, or no account is active yet, the account becomes active and its token
// is synced to $XDG_CONFIG_HOME/ccord/token.
[[nodiscard]] ccord_status ccord_live_account_upsert(const ccord_saved_account *account, const char *token,
                                                     bool make_active, ccord_error *err);
// Selects an account by slot_id or decimal user id, writes its token to
// `out_token`, makes it active and syncs the legacy token file.
[[nodiscard]] ccord_status ccord_live_account_switch(const char *user_id_or_slot, char *out_token, size_t cap,
                                                     ccord_error *err);
// Reads a saved account's token into `out_token` without changing the active
// account and without writing any file. Fails with CCORD_ERR_NOT_FOUND for an
// unknown account.
[[nodiscard]] ccord_status ccord_live_account_peek_token(const char *user_id_or_slot, char *out_token, size_t cap,
                                                         ccord_error *err);
// Removes an account and its token. Removing the active account selects the
// next remaining one, or deletes the legacy token file when none remain.
[[nodiscard]] ccord_status ccord_live_account_remove(const char *user_id_or_slot, ccord_error *err);

// REST v9 -------------------------------------------------------------------
typedef enum ccord_rest_method {
  CCORD_REST_GET = 0,
  CCORD_REST_POST,
  CCORD_REST_PATCH,
  CCORD_REST_PUT,
  CCORD_REST_DELETE
} ccord_rest_method;

typedef struct ccord_rest_config {
  const char *base_url;          // null -> CCORD_LIVE_API_BASE
  const char *token;             // bearer token; may be null for unauthenticated calls
  uint32_t timeout_ms;           // 0 -> default
  size_t max_response_bytes;     // 0 -> CCORD_LIVE_MAX_RESPONSE
  const ccord_gov_config *governor;  // request-governor policy for the queue; null -> defaults
  bool test_allow_http_streams;  // tests only: stream_range accepts any http/https URL (loopback servers)
} ccord_rest_config;

// False for payment, billing, store and subscription paths. Callers must reject
// those with CCORD_ERR_PERMISSION before any network activity.
[[nodiscard]] bool ccord_live_rest_path_allowed(const char *path);
// True only for HTTPS URLs on an exact-match host list (Discord's CDN/media/external-proxy hosts, the GIF providers
// Klipy/Tenor/Giphy/Imgur; no wildcard or suffix rule) whose path passes the REST allowlist.
[[nodiscard]] bool ccord_live_media_url_allowed(const char *url);
// A signed (?ex=) attachment URL on Discord's own CDN hosts: the CDN answers it 404/403 once `ex` has passed, and the
// transport then asks POST /attachments/refresh-urls for a fresh signature and fetches that (same allow-list gate).
[[nodiscard]] bool ccord_live_media_url_refreshable(const char *url);
// The request body {"attachment_urls":[url]} and the answer's {"refreshed_urls":[{"original","refreshed"}]} entry for
// `original` (copied into out; false when there is none that fits).
[[nodiscard]] ccord_status ccord_live_media_refresh_body(const char *url, ccord_buf *out, ccord_error *err);
[[nodiscard]] bool ccord_live_media_refresh_parse(const char *json, size_t len, const char *original, char *out,
                                                   size_t cap);
// The gate for a video the user clicked: everything ccord_live_media_url_allowed accepts, plus any https URL on a
// DNS name with a dot (no userinfo, IP literal, localhost, .local), port absent or 443, and a non-empty path whose
// segments pass the REST allowlist. Automatic loads must keep using ccord_live_media_url_allowed.
[[nodiscard]] bool ccord_live_user_video_url_allowed(const char *url);
// Base64 JSON sent as X-Super-Properties. Static storage; never contains a token.
[[nodiscard]] const char *ccord_live_super_properties(void);
[[nodiscard]] ccord_status ccord_live_rest_build_url(const char *base_url, const char *path, ccord_buf *out,
                                                     ccord_error *err);
// Blocking HTTPS call with a bounded timeout; used by the transport action hooks.
// `response` must be initialised with ccord_buf_init. `http_status` may be null.
[[nodiscard]] ccord_status ccord_live_rest_request(const ccord_rest_config *cfg, ccord_rest_method method,
                                                   const char *path, const char *json_body,
                                                   ccord_buf *response, long *http_status, ccord_error *err);

// Completes 2FA/TOTP login; see ccord_live_auth_login / ccord_live_auth_mfa_totp below.
[[nodiscard]] ccord_status ccord_live_json_auth_response(const char *json, size_t len, long http_status,
                                                         char *out_token, size_t token_cap,
                                                         char *out_mfa_ticket, size_t ticket_cap, ccord_error *err);

// Non-blocking REST queue driven by curl_multi. Results are delivered from
// ccord_rest_queue_pump on the caller's thread; HTTP 429 is retried after the
// server's retry_after delay (bounded number of attempts).
typedef struct ccord_rest_queue ccord_rest_queue;
typedef void (*ccord_rest_callback)(void *ud, ccord_status status, long http_status,
                                    const char *body, size_t body_len);
typedef void (*ccord_rest_progress_cb)(void *ud, uint64_t uploaded_bytes, uint64_t total_bytes);
[[nodiscard]] ccord_rest_queue *ccord_rest_queue_create(const ccord_rest_config *cfg, ccord_error *err);
void ccord_rest_queue_destroy(ccord_rest_queue *q);
// `path` and `json_body` are copied. The callback may be null for fire-and-forget.
[[nodiscard]] ccord_status ccord_rest_queue_add(ccord_rest_queue *q, ccord_rest_method method, const char *path,
                                                const char *json_body, ccord_rest_callback cb, void *ud,
                                                ccord_error *err);
// Enqueues a request to an absolute URL (such as a signed attachment upload URL
// or a CDN media URL) without Discord Authorization headers.
[[nodiscard]] ccord_status ccord_rest_queue_add_raw(ccord_rest_queue *q, ccord_rest_method method,
                                                    const char *full_url, const char *content_type,
                                                    const void *body, size_t body_len,
                                                    ccord_rest_callback cb, void *ud, ccord_error *err);
void ccord_rest_queue_pump(ccord_rest_queue *q, uint64_t now_ms);

// Full-control submission. Every API request is scheduled by the request governor (see governor.h): it starts only
// when the route's rate-limit bucket, the global limit, the in-flight cap and the pacing budget all allow it, in
// priority order (USER, then VISIBLE, then BACKGROUND; FIFO within a class). Non-user work that cannot start within
// 45 s is dropped with CCORD_ERR_TIMEOUT. Raw (absolute-URL) requests are not governed, only capped at 4 in flight;
// time a raw request spends waiting for one of those slots does not count toward the 45 s.
typedef struct ccord_rest_range_info {
  long    http_status;     // final status after redirects
  int64_t range_start;     // Content-Range start (0 for a 200)
  int64_t entity_total;    // from Content-Range "/T" or a 200's Content-Length; -1 unknown
  bool    accept_ranges;   // 206 or "Accept-Ranges: bytes"
  bool    truncated;       // a 200 body was cut at range_len
  char    etag[96];        // strong ETag or ""
  uint8_t redirects;       // hops followed
} ccord_rest_range_info;

typedef struct ccord_rest_request {
  ccord_rest_method method;
  const char *path;            // relative API path; ignored when full_url is set
  const char *full_url;        // absolute URL (CDN, signed upload): no Discord auth, not governed
  const char *json_body;       // for API requests
  const char *content_type;    // for raw requests
  const void *body;            // for raw requests
  size_t body_len;
  ccord_gov_priority priority; // always set explicitly; use ccord_rest_request_make
  bool pre_auth;               // login/MFA: send no Authorization header; a 401 is bad credentials, not a dead token
  bool dedupe;                 // an identical in-flight GET is shared instead of sent twice
  bool bulk;                   // raw GET/PUT: a media download or upload; times out when it stalls, not after timeout_ms
  size_t max_response;         // response size cap for this request; 0 = the queue default
  const char *audit_reason;    // sent as X-Audit-Log-Reason (shown in the server's audit log); null/"" = none
  ccord_rest_callback cb;      // may be null
  ccord_rest_progress_cb progress_cb; // optional upload progress callback (invoked during curl_multi_perform)
  void *ud;
  // Ranged media (stream jobs). Appended last; zero/null/false for every other request.
  bool stream_range;                 // raw GET of one byte range of a clicked video; requires full_url
  int64_t range_start;
  size_t range_len;                  // 1 .. CCORD_STREAM_RANGE_MAX_BYTES; also the response cap
  const char *if_range;
  ccord_rest_range_info *range_info; // out; filled before cb runs; must outlive the job until cb
} ccord_rest_request;
[[nodiscard]] ccord_rest_request ccord_rest_request_make(ccord_rest_method method, const char *path,
                                                         ccord_gov_priority priority);
[[nodiscard]] ccord_status ccord_rest_queue_submit(ccord_rest_queue *q, const ccord_rest_request *req,
                                                   ccord_error *err);
[[nodiscard]] ccord_gov *ccord_rest_queue_governor(ccord_rest_queue *q);
// Request governor behind a live transport's REST queue (for `--net-stats`), or null when `t` is not a live
// transport. The governor is owned by the transport and must not outlive it.
[[nodiscard]] ccord_gov *ccord_live_transport_governor(ccord_transport *t);
[[nodiscard]] size_t ccord_rest_queue_pending(const ccord_rest_queue *q);
// True once a request was answered with HTTP 401 (the token is no longer accepted).
[[nodiscard]] bool ccord_rest_queue_auth_failed(const ccord_rest_queue *q);
void ccord_rest_queue_reset_auth(ccord_rest_queue *q);
// Replaces the Authorization token (wiping the old one) and clears the 401 latch; call after a successful
// login. null/"" removes the token. Jobs already prepared keep their old headers until they are resubmitted.
void ccord_rest_queue_set_token(ccord_rest_queue *q, const char *token);

// Authenticates with Discord through the governed queue (POST /auth/login, USER priority, no Authorization
// header). The callback receives the raw JSON: parse it with ccord_live_json_auth_response, then hand any
// token to ccord_rest_queue_set_token. On 2FA the response carries a ticket instead; complete it with
// ccord_live_auth_mfa_totp. The body is copied, so callers may wipe their credentials right after submit.
[[nodiscard]] ccord_status ccord_live_auth_login(ccord_rest_queue *q, const char *login, const char *password,
                                                 ccord_rest_callback cb, void *ud, ccord_error *err);
// Completes 2FA/TOTP login (POST /auth/mfa/totp) using the ticket from ccord_live_auth_login.
[[nodiscard]] ccord_status ccord_live_auth_mfa_totp(ccord_rest_queue *q, const char *ticket, const char *code,
                                                    ccord_rest_callback cb, void *ud, ccord_error *err);

// Gateway payload mapping ----------------------------------------------------
// Guild dispatch carried by GUILD_CREATE/UPDATE/DELETE and GUILD_MEMBERS_CHUNK.
// These events keep ev.type == CCORD_NET_NONE and fill the arrays below.
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
typedef struct ccord_gateway ccord_gateway;
[[nodiscard]] ccord_gateway *ccord_gateway_create(const ccord_live_config *cfg, const char *token, ccord_error *err);
void ccord_gateway_destroy(ccord_gateway *g);
[[nodiscard]] ccord_status ccord_gateway_start(ccord_gateway *g, ccord_error *err);
void ccord_gateway_stop(ccord_gateway *g);
// The machine woke from sleep or the network changed: drops the socket and reconnects at once (RESUME when a session
// exists), unless the pace says it is too soon. IGNORED unless the gateway is connected or connecting. Never blocks.
ccord_gwpace_wake_verdict ccord_gateway_wake(ccord_gateway *g, ccord_link_event kind);
// Frees the previous returned event, drains the socket without blocking, and
// returns the next queued event. Pointers are valid until the next call.
bool ccord_gateway_poll(ccord_gateway *g, ccord_net_event *out);
ccord_connection_state ccord_gateway_state(const ccord_gateway *g);
const char *ccord_gateway_session_id(const ccord_gateway *g);
const char *ccord_gateway_voice_session_id(const ccord_gateway *g);
[[nodiscard]] ccord_status ccord_gateway_select_guild(ccord_gateway *g, ccord_id guild_id, ccord_error *err);

// Gateway payload builders, exposed so the wire format is unit-testable.
// `out` must be initialised with ccord_buf_init and is never wiped here.
[[nodiscard]] ccord_status ccord_gateway_build_identify(const char *token, ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_build_resume(const char *token, const char *session_id, int64_t seq,
                                                      ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_build_lazy_request(ccord_id guild_id, ccord_buf *out, ccord_error *err);
// Opcode 4 VOICE_STATE_UPDATE. channel_id 0 means "disconnect from voice";
// guild_id CCORD_DM_GUILD_ID sends guild_id:null (join/leave a DM / group call).
[[nodiscard]] ccord_status ccord_gateway_build_voice_state(ccord_id guild_id, ccord_id channel_id, bool self_mute,
                                                           bool self_deaf, bool self_video, bool self_stream,
                                                           ccord_buf *out, ccord_error *err);
// Opcode 3 PRESENCE_UPDATE. `status_text` is an optional custom activity (type 4).
[[nodiscard]] ccord_status ccord_gateway_build_presence(ccord_presence presence, const char *status_text,
                                                        ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_build_presence_ex(ccord_presence presence, const char *emoji_name,
                                                           ccord_id emoji_id, const char *status_text,
                                                           ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_presence(ccord_gateway *g, ccord_presence presence,
                                                       const char *status_text, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_presence_ex(ccord_gateway *g, ccord_presence presence,
                                                          const char *emoji_name, ccord_id emoji_id,
                                                          const char *status_text, ccord_error *err);
// Rich Presence (Discord RPC SET_ACTIVITY): a JSON array of up to CCORD_GATEWAY_RICH_MAX_ACTIVITIES activity
// objects (each with a string "name" and an integral "type"), at most CCORD_GATEWAY_RICH_MAX_BYTES bytes.
// ccord_gateway_set_rich_activities validates and stores it; every op 3 from ccord_gateway_send_presence_ex
// splices it raw after the custom-status activity. Invisible presence omits it. nullptr/0 clears.
#define CCORD_GATEWAY_RICH_MAX_BYTES 8192u
#define CCORD_GATEWAY_RICH_MAX_ACTIVITIES 4u
[[nodiscard]] ccord_status ccord_gateway_set_rich_activities(ccord_gateway *g, const char *json, size_t len,
                                                             ccord_error *err);
// Opcode 3 builder with the rich-activities array spliced after the custom status (same validation as the setter).
[[nodiscard]] ccord_status ccord_gateway_build_presence_rich(ccord_presence presence, const char *emoji_name,
                                                             ccord_id emoji_id, const char *status_text,
                                                             const char *rich_json, size_t rich_len,
                                                             ccord_buf *out, ccord_error *err);
// Opcode 18 Stream Create with the chosen encoder/codec/framework/resolution/fps.
[[nodiscard]] ccord_status ccord_gateway_build_stream_create(ccord_id guild_id, ccord_id channel_id,
                                                             const ccord_stream_config *config, ccord_buf *out,
                                                             ccord_error *err);
// Opcode 19 Stream Delete for the guild's active stream.
[[nodiscard]] ccord_status ccord_gateway_build_stream_delete(ccord_id guild_id, ccord_id channel_id, ccord_buf *out,
                                                             ccord_error *err);
// Formats/parses a Discord stream_key ("guild:<gid>:<cid>:<uid>" or "call:<cid>:<uid>").
void ccord_stream_key_format(ccord_id guild_id, ccord_id channel_id, ccord_id user_id, char out[96]);
bool ccord_stream_key_parse(const char *stream_key, ccord_id *out_guild_id, ccord_id *out_channel_id,
                            ccord_id *out_user_id);
// Opcode 19 Stream Delete by stream_key.
[[nodiscard]] ccord_status ccord_gateway_build_stream_delete_key(const char *stream_key, ccord_buf *out,
                                                                 ccord_error *err);
// Opcode 20 Stream Watch by stream_key.
[[nodiscard]] ccord_status ccord_gateway_build_stream_watch(const char *stream_key, ccord_buf *out,
                                                            ccord_error *err);
// Opcode 21 Stream Ping by stream_key.
[[nodiscard]] ccord_status ccord_gateway_build_stream_ping(const char *stream_key, ccord_buf *out, ccord_error *err);
// Opcode 22 Stream Set Paused by stream_key.
[[nodiscard]] ccord_status ccord_gateway_build_stream_set_paused(const char *stream_key, bool paused,
                                                                 ccord_buf *out, ccord_error *err);
// Send helpers used by the live transport action hooks. They require an open gateway.
[[nodiscard]] ccord_status ccord_gateway_send_voice_state(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id,
                                                          bool self_mute, bool self_deaf, bool self_video,
                                                          bool self_stream, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_stream_create(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id,
                                                            const ccord_stream_config *config, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_stream_delete(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id,
                                                            ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_stream_delete_key(ccord_gateway *g, const char *stream_key,
                                                                ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_stream_watch(ccord_gateway *g, const char *stream_key,
                                                           ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_stream_ping(ccord_gateway *g, const char *stream_key, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_stream_set_paused(ccord_gateway *g, const char *stream_key, bool paused,
                                                                ccord_error *err);

// Voice Gateway v8 ------------------------------------------------------------
#define CCORD_VOICE_GATEWAY_VERSION 8u
#define CCORD_VOICE_SAMPLE_RATE 48000u
#define CCORD_VOICE_CHANNELS 2u
#define CCORD_VOICE_FRAME_SAMPLES 960u   // 20 ms at 48 kHz
#define CCORD_VOICE_MAX_SECRET 32u
#define CCORD_VOICE_MAX_MODES 8u
#define CCORD_VOICE_MODE_MAX 32u
#define CCORD_VOICE_ENDPOINT_MAX 256u
#define CCORD_VOICE_RTP_HEADER_SIZE 12u
#define CCORD_VOICE_RTP_PAYLOAD_TYPE 120u
#define CCORD_VOICE_IP_DISCOVERY_SIZE 74u

typedef enum ccord_voice_opcode : uint8_t {
  CCORD_VOICE_OP_IDENTIFY = 0,
  CCORD_VOICE_OP_SELECT_PROTOCOL = 1,
  CCORD_VOICE_OP_READY = 2,
  CCORD_VOICE_OP_HEARTBEAT = 3,
  CCORD_VOICE_OP_SESSION_DESCRIPTION = 4,
  CCORD_VOICE_OP_SPEAKING = 5,
  CCORD_VOICE_OP_HEARTBEAT_ACK = 6,
  CCORD_VOICE_OP_HELLO = 8,
  CCORD_VOICE_OP_VIDEO = 12,
  CCORD_VOICE_OP_RESUME = 7,
  CCORD_VOICE_OP_RESUMED = 9,
} ccord_voice_opcode;

typedef struct ccord_voice_ready_info {
  uint32_t ssrc;
  char ip[64];
  uint16_t port;
  char modes[CCORD_VOICE_MAX_MODES][CCORD_VOICE_MODE_MAX];
  size_t mode_count;
} ccord_voice_ready_info;

// One decoded Voice Gateway v8 payload. Fixed storage keeps this type free of
// arena lifetime rules; secret_key is wiped by the state machine on teardown.
typedef struct ccord_voice_event {
  int op;
  int64_t seq;
  bool hello;
  bool heartbeat_ack;
  bool ready;
  bool session_description;
  bool speaking;
  bool video;
  uint32_t heartbeat_interval_ms;
  ccord_voice_ready_info ready_info;
  char mode[CCORD_VOICE_MODE_MAX];
  uint8_t secret_key[CCORD_VOICE_MAX_SECRET];
  size_t secret_key_len;
  uint8_t speaking_flag;
  uint32_t ssrc;
  uint32_t audio_ssrc, video_ssrc, rtx_ssrc;
  uint32_t stream_ssrcs[8];
  uint32_t stream_rtx_ssrcs[8];
  size_t stream_ssrc_count;
  ccord_id user_id;
  uint16_t dave_protocol_version;
  uint16_t transition_id;
  uint32_t epoch;
  ccord_id connected_users[256];
  size_t connected_user_count;
} ccord_voice_event;

// Decodes one Voice Gateway payload (op 8/2/4/6/5/12 + DAVE ops 11/13/21/22/24). Unknown ops are valid
// and only set `op`. No network activity.
[[nodiscard]] ccord_status ccord_voice_gateway_decode(const char *json, size_t len, ccord_voice_event *out,
                                                      ccord_error *err);

// Wire builders, exposed for unit tests.
[[nodiscard]] ccord_status ccord_voice_build_url(const char *endpoint, char *out, size_t cap, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_identify(ccord_id server_id, ccord_id user_id, const char *session_id,
                                                      const char *token, ccord_buf *out, ccord_error *err);
#ifndef CCORD_VOICE_VIDEO_DESC_DECLARED
#define CCORD_VOICE_VIDEO_DESC_DECLARED
typedef struct ccord_voice_video_desc {
  uint32_t width, height, fps, max_bitrate;
  uint8_t payload_type, rtx_payload_type;
} ccord_voice_video_desc;
#endif
[[nodiscard]] ccord_status ccord_voice_build_select_protocol(const char *address, uint16_t port, const char *mode,
                                                             ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_select_protocol_ex(const char *address, uint16_t port, const char *mode,
                                                                uint8_t video_pt, uint8_t video_rtx_pt,
                                                                ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_heartbeat(ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_heartbeat_v8(uint64_t nonce, int64_t seq_ack, ccord_buf *out,
                                                          ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_speaking(uint32_t ssrc, uint8_t speaking, uint32_t delay_ms,
                                                      ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_video(uint32_t audio_ssrc, uint32_t video_ssrc, uint32_t rtx_ssrc,
                                                   ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_video_ex(uint32_t audio_ssrc, uint32_t video_ssrc, uint32_t rtx_ssrc,
                                                      const ccord_voice_video_desc *desc, ccord_buf *out,
                                                      ccord_error *err);

// Discord 74-byte UDP IP Discovery packet (0x0001 request, 0x0002 response).
size_t ccord_voice_ip_discovery_build(uint32_t ssrc, uint8_t *out);
[[nodiscard]] ccord_status ccord_voice_ip_discovery_parse(const uint8_t *pkt, size_t len, uint32_t *out_ssrc,
                                                          char *out_ip, uint16_t *out_port,
                                                          ccord_error *err);

typedef enum ccord_voice_connection_state : uint8_t {
  CCORD_VOICE_IDLE = 0,
  CCORD_VOICE_CONNECTING,
  CCORD_VOICE_HELLO,
  CCORD_VOICE_READY,
  CCORD_VOICE_ESTABLISHED,
  CCORD_VOICE_FAILED,
  CCORD_VOICE_RECONNECTING,   // WebSocket lost, UDP/keys kept, resuming (appended: keeps existing ordinals)
} ccord_voice_connection_state;

typedef struct ccord_voice_gateway ccord_voice_gateway;
typedef struct ccord_voice_gateway_config {
  ccord_id server_id;             // guild id
  ccord_id channel_id;            // voice channel id (MLS group id; 0 falls back to server_id)
  ccord_id user_id;               // self id
  const char *session_id;         // VOICE_STATE_UPDATE session id (or READY fallback)
  const char *token;              // VOICE_SERVER_UPDATE token; never logged
  const char *endpoint;           // VOICE_SERVER_UPDATE endpoint host[:port]
  uint32_t connect_timeout_ms;    // 0 -> default
  uint64_t (*now_ms)(void *ud);   // injectable clock; null -> ccord_clock_ms
  void *clock_ud;
  // Test/embedding hook: when set, frames are delivered here instead of curl.
  ccord_status (*send)(void *ud, const char *data, size_t len, ccord_error *err);
  ccord_status (*send_bin)(void *ud, const uint8_t *data, size_t len, ccord_error *err);
  void *send_ud;
} ccord_voice_gateway_config;

[[nodiscard]] ccord_voice_gateway *ccord_voice_gateway_create(const ccord_voice_gateway_config *cfg,
                                                              ccord_error *err);
void ccord_voice_gateway_destroy(ccord_voice_gateway *g);
[[nodiscard]] ccord_status ccord_voice_gateway_start(ccord_voice_gateway *g, ccord_error *err);
void ccord_voice_gateway_stop(ccord_voice_gateway *g);
// Drains the socket, pumps UDP audio & IP discovery, sends due heartbeats, and returns the next decoded event.
bool ccord_voice_gateway_poll(ccord_voice_gateway *g, ccord_voice_event *out);
// Applies one payload and emits Identify/Select Protocol as the state machine
// requires. Exposed so the handshake is unit-testable without a socket.
[[nodiscard]] ccord_status ccord_voice_gateway_handle(ccord_voice_gateway *g, const char *json, size_t len,
                                                      ccord_voice_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_gateway_handle_binary(ccord_voice_gateway *g, const uint8_t *data, size_t len,
                                                             ccord_error *err);
ccord_voice_connection_state ccord_voice_gateway_state(const ccord_voice_gateway *g);
// Voice reconnect. Implemented by the gateway resume task.
[[nodiscard]] ccord_status ccord_voice_build_resume(ccord_id server_id, const char *session_id, const char *token,
                                                    int64_t seq_ack, ccord_buf *out, ccord_error *err);
typedef struct ccord_voice_failure {
  uint16_t close_code;          // 0 = no close frame
  ccord_vc_cause cause;
  bool was_established;
  ccord_vc state;               // policy state after the decision; the owner adopts it
  ccord_vc_plan plan;           // never CCORD_VC_RESUME (those are handled inside the gateway)
} ccord_voice_failure;
bool ccord_voice_gateway_failure(const ccord_voice_gateway *g, ccord_voice_failure *out);   // true only in FAILED
void ccord_voice_gateway_network_event(ccord_voice_gateway *g);   // ESTABLISHED: lose WS now (RESUME, delay 0); RECONNECTING: attempt now
void ccord_voice_gateway_inject_close(ccord_voice_gateway *g, uint16_t close_code);   // test hook, 0 = abnormal drop
uint32_t ccord_voice_gateway_ssrc(const ccord_voice_gateway *g);
int ccord_voice_gateway_udp_fd(const ccord_voice_gateway *g);
size_t ccord_voice_gateway_secret(const ccord_voice_gateway *g, uint8_t out[CCORD_VOICE_MAX_SECRET]);
[[nodiscard]] ccord_status ccord_voice_gateway_set_speaking(ccord_voice_gateway *g, uint8_t speaking,
                                                            uint32_t delay_ms, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_gateway_set_video(ccord_voice_gateway *g, bool enabled, ccord_error *err);
// Descriptor advertised by Voice OP 12 (max_bitrate / max_framerate / max_resolution) and the H.264 payload
// types. Go Live vstreams set their own at open; the standalone camera sets it before enabling video.
// nullptr clears it. No network activity.
[[nodiscard]] ccord_status ccord_voice_gateway_set_video_desc(ccord_voice_gateway *g,
                                                              const ccord_voice_video_desc *desc, ccord_error *err);
#ifndef CCORD_VOICE_GATEWAY_VIDEO_DECLARED
#define CCORD_VOICE_GATEWAY_VIDEO_DECLARED
[[nodiscard]] ccord_status ccord_voice_gateway_send_video_op(ccord_voice_gateway *g, uint32_t video_ssrc,
                                                             uint32_t rtx_ssrc, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_gateway_send_h264(ccord_voice_gateway *g, const uint8_t *annexb, size_t len,
                                                         uint32_t timestamp_90khz, ccord_error *err);
bool ccord_voice_gateway_poll_video(ccord_voice_gateway *g, const uint8_t **out_annexb, size_t *out_len,
                                    bool *out_keyframe);
bool ccord_voice_gateway_consume_keyframe_request(ccord_voice_gateway *g);
#endif
bool ccord_voice_gateway_user_speaking(const ccord_voice_gateway *g, ccord_id user_id);
bool ccord_voice_gateway_self_speaking(const ccord_voice_gateway *g);
bool ccord_voice_gateway_poll_speaking(ccord_voice_gateway *g, ccord_id *out_user_id, bool *out_speaking);
// Queues 48kHz stereo S16LE PCM frames for 20ms Opus+RTP+AEAD UDP transmission.
[[nodiscard]] ccord_status ccord_voice_gateway_push_pcm(ccord_voice_gateway *g, const int16_t *pcm,
                                                        size_t stereo_samples, ccord_error *err);
// Reads decoded 48kHz stereo S16LE PCM frames received from the UDP voice socket.
size_t ccord_voice_gateway_pull_pcm(ccord_voice_gateway *g, int16_t *out_pcm, size_t max_stereo_samples);

// ---- M5 voice controls (implemented by m5-voice-core / m5-live-transport) --------------------------------------
// Per-user playback gain applied before mixing; pct is clamped to 0..200 (100 = unity). Unknown users are
// remembered until CCORD_SPEAK_MAX entries. No network activity.
void ccord_voice_gateway_set_user_volume(ccord_voice_gateway *g, ccord_id user_id, uint8_t pct);
// Push-to-talk gate: while `held` is false and PTT is enabled (ccord_voice_gateway_set_dsp), mic frames are
// dropped (silence frames are still sent on gate close). No network activity.
void ccord_voice_gateway_set_ptt(ccord_voice_gateway *g, bool held);
// Local deafen state: clears buffered RX audio and drops incoming RX PCM while deafened.
void ccord_voice_gateway_set_deafened(ccord_voice_gateway *g, bool deafened);
// Maps the 0..100 input-sensitivity slider to the VAD threshold (ccord_vad_threshold_from_sensitivity).
void ccord_voice_gateway_set_vad_sensitivity(ccord_voice_gateway *g, uint32_t pct);
// Queues 48kHz stereo S16LE soundboard PCM; it is mixed with the mic and transmitted with SPEAKING flags 2.
[[nodiscard]] ccord_status ccord_voice_gateway_push_soundboard_pcm(ccord_voice_gateway *g, const int16_t *pcm,
                                                                   size_t stereo_samples, ccord_error *err);
// Snapshot of connection/media counters for the RTC debug panel. Zeroed when idle. No network activity.
void ccord_voice_gateway_stats(const ccord_voice_gateway *g, ccord_rtc_stats *out);
// Applies input-processing settings (PTT mode, VAD sensitivity, NS/AEC/AGC). nullptr restores defaults.
void ccord_voice_gateway_set_dsp(ccord_voice_gateway *g, const ccord_voice_config *cfg);
// Copies up to `cap` recognized DAVE peer user IDs into `out_ids` and returns total peer count.
size_t ccord_voice_gateway_dave_peers(ccord_voice_gateway *g, ccord_id *out_ids, size_t cap);

// Opus + RTP/xsalsa20_poly1305 media framing (linked when CCORD_LIVE_ACCOUNT=ON).
typedef struct ccord_voice_codec ccord_voice_codec;
[[nodiscard]] ccord_voice_codec *ccord_voice_codec_create(ccord_error *err);
void ccord_voice_codec_destroy(ccord_voice_codec *c);
// Encodes `samples` interleaved S16 stereo frames (20 ms = 960 at 48 kHz).
[[nodiscard]] ccord_status ccord_voice_encode(ccord_voice_codec *c, const int16_t *pcm, size_t samples,
                                              uint8_t *out, size_t cap, size_t *out_len, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_decode(ccord_voice_codec *c, const uint8_t *packet, size_t len,
                                              int16_t *pcm, size_t samples, size_t *out_samples, ccord_error *err);
// Writes a 12-byte RTP v2 header in network byte order; returns bytes written.
size_t ccord_voice_rtp_header(uint8_t *out, size_t cap, uint16_t seq, uint32_t timestamp, uint32_t ssrc);
// Discord xsalsa20_poly1305: 12-byte RTP header || MAC(16) || ciphertext(Opus).
// `out` must hold header_len + frame_len + 16 bytes.
[[nodiscard]] ccord_status ccord_voice_encrypt(const uint8_t key[CCORD_VOICE_MAX_SECRET], const uint8_t *header,
                                               size_t header_len, const uint8_t *frame, size_t frame_len,
                                               uint8_t *out, size_t cap, size_t *out_len, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_decrypt(const uint8_t key[CCORD_VOICE_MAX_SECRET], const uint8_t *packet,
                                               size_t len, size_t header_len, uint8_t *out, size_t cap,
                                               size_t *out_len, ccord_error *err);
