// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/buf.h>
#include <ccord/core/error.h>
#include <ccord/net/governor.h>
#include <ccord/net/transport.h>

#ifndef CCORD_LIVE_API_BASE
#define CCORD_LIVE_API_BASE "https://discord.com/api/v9"
#endif
#ifndef CCORD_LIVE_TIMEOUT_DEFAULT_MS
#define CCORD_LIVE_TIMEOUT_DEFAULT_MS 15000u
#endif
#ifndef CCORD_LIVE_MAX_RESPONSE
#define CCORD_LIVE_MAX_RESPONSE (8u * 1024u * 1024u)
#endif
#ifndef CCORD_LIVE_MAX_MEDIA
#define CCORD_LIVE_MAX_MEDIA (32u * 1024u * 1024u)
#endif

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
  bool enable_cookies;           // in-memory cookie jar shared across queue requests (used by pre-auth login)
} ccord_rest_config;

#define CCORD_CAPTCHA_SERVICE_MAX    32u
#define CCORD_CAPTCHA_SITEKEY_MAX    128u
#define CCORD_CAPTCHA_RQDATA_MAX     8192u
#define CCORD_CAPTCHA_RQTOKEN_MAX    2048u
#define CCORD_CAPTCHA_SESSION_ID_MAX 128u
#define CCORD_CAPTCHA_KEY_MAX        8192u
#define CCORD_FINGERPRINT_MAX        256u

// Parsed Discord CAPTCHA challenge payload (HTTP 400 with captcha_key / captcha_sitekey).
typedef struct ccord_captcha_challenge {
  bool required;
  char service[CCORD_CAPTCHA_SERVICE_MAX];       // e.g. "hcaptcha"
  char sitekey[CCORD_CAPTCHA_SITEKEY_MAX];       // captcha_sitekey
  char rqdata[CCORD_CAPTCHA_RQDATA_MAX];         // captcha_rqdata (optional hCaptcha Enterprise payload)
  char rqtoken[CCORD_CAPTCHA_RQTOKEN_MAX];       // captcha_rqtoken (echoed as X-Captcha-Rqtoken)
  char session_id[CCORD_CAPTCHA_SESSION_ID_MAX]; // captcha_session_id (echoed as X-Captcha-Session-Id)
} ccord_captcha_challenge;

// User-completed CAPTCHA response headers to attach when retrying a CAPTCHA-gated endpoint.
typedef struct ccord_captcha_solution {
  const char *key;         // solved provider token -> X-Captcha-Key
  const char *rqtoken;     // echoed captcha_rqtoken -> X-Captcha-Rqtoken (optional)
  const char *session_id;  // echoed captcha_session_id -> X-Captcha-Session-Id (optional)
} ccord_captcha_solution;

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

// Parses a Discord CAPTCHA challenge JSON body (captcha_key / captcha_sitekey / captcha_service /
// captcha_rqdata / captcha_rqtoken / captcha_session_id). Returns true if a CAPTCHA challenge was found.
[[nodiscard]] bool ccord_live_json_parse_captcha(const char *json, size_t len, ccord_captcha_challenge *out);
// Parses {"fingerprint":"..."} from GET /experiments. Returns true if a non-empty fingerprint was extracted.
[[nodiscard]] bool ccord_live_json_parse_fingerprint(const char *json, size_t len, char *out, size_t cap);

// Completes 2FA/TOTP login; see ccord_live_auth_login / ccord_live_auth_mfa_totp below.
[[nodiscard]] ccord_status ccord_live_json_auth_response(const char *json, size_t len, long http_status,
                                                         char *out_token, size_t token_cap,
                                                         char *out_mfa_ticket, size_t ticket_cap, ccord_error *err);
// Extended variant that also extracts Discord's CAPTCHA challenge metadata when HTTP 400 demands a CAPTCHA.
[[nodiscard]] ccord_status ccord_live_json_auth_response_ex(const char *json, size_t len, long http_status,
                                                            char *out_token, size_t token_cap,
                                                            char *out_mfa_ticket, size_t ticket_cap,
                                                            ccord_captcha_challenge *out_captcha, ccord_error *err);

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
  const char *mfa_auth_token;        // optional X-Discord-MFA-Authorization header (wiped on job free); null/"" = none
  const char *captcha_key;           // optional X-Captcha-Key header (wiped on job free); null/"" = none
  const char *captcha_rqtoken;       // optional X-Captcha-Rqtoken header (wiped on job free); null/"" = none
  const char *captcha_session_id;    // optional X-Captcha-Session-Id header (wiped on job free); null/"" = none
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
// Reports active readable REST socket fds and the exact next timer deadline (curl_multi timeout, rate-limit/retry
// not_before_ms, or non-user expiry) so an idle/background loop can sleep in kernel poll() without polling.
// `now_ms` should match the clock passed to ccord_rest_queue_pump() (0 falls back to ccord_clock_ms()).
size_t ccord_rest_queue_wait_info(const ccord_rest_queue *q, uint64_t now_ms, ccord_socket_t *out_fds, size_t cap,
                                  uint32_t *out_timeout_ms, bool *out_has_pending);
// True once a request was answered with HTTP 401 (the token is no longer accepted).
[[nodiscard]] bool ccord_rest_queue_auth_failed(const ccord_rest_queue *q);
void ccord_rest_queue_reset_auth(ccord_rest_queue *q);
// Replaces the Authorization token (wiping the old one) and clears the 401 latch; call after a successful
// login. null/"" removes the token. Jobs already prepared keep their old headers until they are resubmitted.
void ccord_rest_queue_set_token(ccord_rest_queue *q, const char *token);
// Sets or clears the pre-auth X-Fingerprint header (from GET /experiments) carried on subsequent API requests.
void ccord_rest_queue_set_fingerprint(ccord_rest_queue *q, const char *fingerprint);

// Fetches pre-auth Discord session fingerprint + cookies (GET /experiments, pre-auth USER priority).
[[nodiscard]] ccord_status ccord_live_auth_fetch_fingerprint(ccord_rest_queue *q, ccord_rest_callback cb, void *ud,
                                                             ccord_error *err);
// Authenticates with Discord through the governed queue (POST /auth/login, USER priority, no Authorization
// header). The callback receives the raw JSON: parse it with ccord_live_json_auth_response_ex, then hand any
// token to ccord_rest_queue_set_token. On 2FA the response carries a ticket instead; complete it with
// ccord_live_auth_mfa_totp. On CAPTCHA, display the challenge to the user and retry with ccord_live_auth_login_captcha.
// The body is copied, so callers may wipe their credentials right after submit.
[[nodiscard]] ccord_status ccord_live_auth_login(ccord_rest_queue *q, const char *login, const char *password,
                                                 ccord_rest_callback cb, void *ud, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_auth_login_captcha(ccord_rest_queue *q, const char *login, const char *password,
                                                         const ccord_captcha_solution *captcha,
                                                         ccord_rest_callback cb, void *ud, ccord_error *err);
// Completes 2FA/TOTP login (POST /auth/mfa/totp) using the ticket from ccord_live_auth_login.
[[nodiscard]] ccord_status ccord_live_auth_mfa_totp(ccord_rest_queue *q, const char *ticket, const char *code,
                                                    ccord_rest_callback cb, void *ud, ccord_error *err);

// Gateway payload mapping ----------------------------------------------------
// Guild dispatch carried by GUILD_CREATE/UPDATE/DELETE and GUILD_MEMBERS_CHUNK.
// These events keep ev.type == CCORD_NET_NONE and fill the arrays below.
