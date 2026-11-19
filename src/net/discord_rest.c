// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif
#include <ccord/net/live_transport.h>

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__ANDROID__) && !defined(__APPLE__) && !defined(_WIN32) && defined(__has_include)
#if __has_include(<threads.h>) && !defined(__STDC_NO_THREADS__)
#define CCORD_REST_HAVE_C11_THREADS 1
#include <threads.h>
#endif
#endif

#include <curl/curl.h>
#include "tls_internal.h"

#include <ccord/core/clock.h>
#include <ccord/core/log.h>
#include <ccord/core/alloc.h>
#include <ccord/core/json.h>

static bool ascii_eq_ci(const char *a, const char *b, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

static bool forbidden_segment(const char *s, size_t n)
{
  static const char *const words[] = {
    "store", "stores", "payment", "payments", "billing", "billings",
    "subscription", "subscriptions", "purchase", "purchases", "checkout",
    "gift", "gifts", "nitro",
  };
  for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
    size_t w = strlen(words[i]);
    if (w == n && ascii_eq_ci(s, words[i], n)) return true;
  }
  return false;
}

bool ccord_live_rest_path_allowed(const char *path)
{
  if (!path || path[0] != '/') return false;
  size_t i = 1;
  while (path[i] != '\0') {
    while (path[i] == '/') i++;
    size_t start = i;
    while (path[i] != '\0' && path[i] != '/' && path[i] != '?' && path[i] != '#') i++;
    if (forbidden_segment(path + start, i - start)) return false;
    if (path[i] == '?' || path[i] == '#') {
      // Query strings cannot smuggle a forbidden route: the path itself is checked above.
      break;
    }
  }
  return true;
}

static bool allowed_media_host(const char *host, size_t n)
{
  static const char *const hosts[] = {
    "cdn.discordapp.com",
    "media.discordapp.net",
    "media.discordapp.com",
    // Discord's external-media proxy: every embed image/thumbnail/video and GIF-picker cover is served from here.
    "images-ext-1.discordapp.net",
    "images-ext-2.discordapp.net",
    // GIF providers behind Discord's picker (Klipy today, Tenor before it).
    "static.klipy.com",
    "media.tenor.com",
    "media1.tenor.com",
    "c.tenor.com",
    // Third-party GIF providers the official client also loads (exact hosts only; no wildcard or suffix rule).
    "media.giphy.com",
    "media0.giphy.com",
    "media1.giphy.com",
    "media2.giphy.com",
    "media3.giphy.com",
    "media4.giphy.com",
    "i.giphy.com",
    "i.imgur.com",
    "media.tenor.co",
  };
  for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
    size_t hlen = strlen(hosts[i]);
    if (hlen == n && ascii_eq_ci(host, hosts[i], n)) return true;
  }
  return false;
}

bool ccord_live_media_url_allowed(const char *url)
{
  if (!url || !ascii_eq_ci(url, "https://", 8)) return false;
  const char *host = url + 8;
  const char *slash = strchr(host, '/');
  if (!slash || slash == host) return false;
  if (slash[1] == '\0' || slash[1] == '?' || slash[1] == '#') return false;
  const char *host_end = slash;
  const char *colon = memchr(host, ':', (size_t)(slash - host));
  if (colon) {
    if (colon == host || colon + 1 == slash) return false;
    for (const char *p = colon + 1; p < slash; p++) {
      if (*p < '0' || *p > '9') return false;
    }
    host_end = colon;
  }
  if (!allowed_media_host(host, (size_t)(host_end - host))) return false;
  return ccord_live_rest_path_allowed(slash);
}

// A DNS host name for a user-clicked video: letters, digits, '-' and '.', every label non-empty, at least one dot,
// a last label that is not all digits (so no dotted-quad or shorthand IPv4 literal), and not localhost / .local.
static bool user_video_host_ok(const char *host, size_t n)
{
  if (n == 0 || n > 253) return false;
  bool dot = false, last_numeric = true;
  size_t label = 0;
  for (size_t i = 0; i < n; i++) {
    char c = host[i];
    if (c == '.') {
      if (label == 0) return false;
      dot = true;
      label = 0;
      last_numeric = true;
      continue;
    }
    bool digit = c >= '0' && c <= '9';
    bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!digit && !alpha && c != '-') return false;
    if (!digit) last_numeric = false;
    if (++label > 63) return false;
  }
  if (!dot || label == 0 || last_numeric) return false;
  static const char *const local[] = {"localhost", ".localhost", ".local"};
  for (size_t i = 0; i < sizeof local / sizeof local[0]; i++) {
    size_t ln = strlen(local[i]);
    if (n >= ln && ascii_eq_ci(host + (n - ln), local[i], ln) && (local[i][0] == '.' || n == ln)) return false;
  }
  return true;
}

bool ccord_live_user_video_url_allowed(const char *url)
{
  if (ccord_live_media_url_allowed(url)) return true;
  if (!url || !ascii_eq_ci(url, "https://", 8)) return false;
  const char *host = url + 8;
  const char *slash = strchr(host, '/');
  if (!slash || slash == host) return false;
  if (slash[1] == '\0' || slash[1] == '?' || slash[1] == '#') return false;
  const char *host_end = slash;
  const char *colon = memchr(host, ':', (size_t)(slash - host));
  if (colon) {
    if (slash - colon - 1 != 3 || memcmp(colon + 1, "443", 3) != 0) return false;
    host_end = colon;
  }
  if (!user_video_host_ok(host, (size_t)(host_end - host))) return false;
  return ccord_live_rest_path_allowed(slash);
}

ccord_status ccord_live_rest_build_url(const char *base_url, const char *path, ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !path || path[0] != '/') return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad path");
  const char *base = base_url && base_url[0] ? base_url : CCORD_LIVE_API_BASE;
  size_t n = strlen(base);
  while (n > 0 && base[n - 1] == '/') n--;
  if (n == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad base URL");
  if (ccord_buf_append(out, base, n) != CCORD_OK) return ccord_error_set(err, CCORD_ERR_RANGE, "rest: URL too long");
  if (ccord_buf_append_str(out, path) != CCORD_OK) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "rest: URL too long");
  }
  return CCORD_OK;
}

typedef struct rest_sink {
  ccord_buf *buf;
  size_t max;
  bool too_large;
  // Ranged stream jobs: a 200 answer (Range ignored) keeps only the first `max` bytes and then aborts the transfer;
  // a 3xx body is dropped. `easy` is read for the status line the body belongs to.
  CURL *easy;
  bool stream;
  bool truncated;
} rest_sink;

static size_t rest_write(void *contents, size_t size, size_t nmemb, void *ud)
{
  rest_sink *s = ud;
  if (size != 0 && nmemb > SIZE_MAX / size) {
    s->too_large = true;
    return 0;
  }
  size_t n = size * nmemb;
  if (s->stream && s->easy) {
    long code = 0;
    (void)curl_easy_getinfo(s->easy, CURLINFO_RESPONSE_CODE, &code);
    if (code >= 300 && code < 400) return n;   // a redirect's own body is never media
    if (code == 200) {
      size_t room = s->max - s->buf->len;
      size_t take = n < room ? n : room;
      if (take > 0 && ccord_buf_append(s->buf, contents, take) != CCORD_OK) {
        s->too_large = true;
        return 0;
      }
      if (take < n) {
        s->truncated = true;   // the server ignored Range: stop here, at most one chunk was kept
        return 0;
      }
      return n;
    }
  }
  if (n > s->max || s->buf->len > s->max - n) {
    s->too_large = true;
    return 0;
  }
  if (ccord_buf_append(s->buf, contents, n) != CCORD_OK) {
    s->too_large = true;
    return 0;
  }
  return n;
}

static CURLcode set_method(CURL *curl, ccord_rest_method method, const char *body, size_t len)
{
  switch (method) {
    case CCORD_REST_GET:
      return curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    case CCORD_REST_POST:
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)len);
      return CURLE_OK;
    case CCORD_REST_PATCH:
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)len);
      return CURLE_OK;
    case CCORD_REST_PUT:
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)len);
      return CURLE_OK;
    case CCORD_REST_DELETE:
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
      if (body && len > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)len);
      }
      return CURLE_OK;
  }
  return CURLE_UNSUPPORTED_PROTOCOL;
}

static const char *method_name(ccord_rest_method method)
{
  switch (method) {
    case CCORD_REST_GET: return "GET";
    case CCORD_REST_POST: return "POST";
    case CCORD_REST_PATCH: return "PATCH";
    case CCORD_REST_PUT: return "PUT";
    case CCORD_REST_DELETE: return "DELETE";
  }
  return "GET";
}

typedef enum tls_ca_mode : uint8_t {
  TLS_CA_DEFAULT = 0,
  TLS_CA_FILE = 1,
  TLS_CA_DIR = 2,
} tls_ca_mode;

static tls_ca_mode g_tls_ca_mode = TLS_CA_DEFAULT;
static char g_tls_ca_path[4096];

static void tls_ca_resolve_impl(void)
{
  const char *cafile = getenv("CCORD_CA_BUNDLE");
  if (!cafile || !cafile[0]) cafile = getenv("SSL_CERT_FILE");
  if (cafile && cafile[0] && ccord_path_is_file(cafile)) {
    size_t n = strlen(cafile);
    if (n < sizeof g_tls_ca_path) {
      memcpy(g_tls_ca_path, cafile, n + 1u);
      g_tls_ca_mode = TLS_CA_FILE;
      return;
    }
  }
#if CCORD_OS_ANDROID
  static const char k_apex[] = "/apex/com.android.conscrypt/cacerts";
  static const char k_legacy[] = "/system/etc/security/cacerts";
  if (ccord_path_is_dir(k_apex)) {
    memcpy(g_tls_ca_path, k_apex, sizeof k_apex);
    g_tls_ca_mode = TLS_CA_DIR;
  } else if (ccord_path_is_dir(k_legacy)) {
    memcpy(g_tls_ca_path, k_legacy, sizeof k_legacy);
    g_tls_ca_mode = TLS_CA_DIR;
  }
#endif
}

#if defined(CCORD_REST_HAVE_C11_THREADS)
static once_flag g_curl_init_once = ONCE_FLAG_INIT;
static once_flag g_tls_ca_once = ONCE_FLAG_INIT;

static void curl_global_init_cb(void) { (void)curl_global_init(CURL_GLOBAL_DEFAULT); }

void ccord_curl_global_init_once(void) { call_once(&g_curl_init_once, curl_global_init_cb); }

static void tls_ca_ensure_resolved(void) { call_once(&g_tls_ca_once, tls_ca_resolve_impl); }
#else
static _Atomic int g_curl_init_state = 0;  // 0 = uninit, 1 = running, 2 = done
static _Atomic int g_tls_ca_state = 0;

void ccord_curl_global_init_once(void)
{
  if (atomic_load_explicit(&g_curl_init_state, memory_order_acquire) == 2) return;
  int expected = 0;
  if (atomic_compare_exchange_strong_explicit(&g_curl_init_state, &expected, 1,
                                              memory_order_acq_rel, memory_order_acquire)) {
    (void)curl_global_init(CURL_GLOBAL_DEFAULT);
    atomic_store_explicit(&g_curl_init_state, 2, memory_order_release);
  } else {
    while (atomic_load_explicit(&g_curl_init_state, memory_order_acquire) != 2) {
      ccord_sleep_ms(1);
    }
  }
}

static void tls_ca_ensure_resolved(void)
{
  if (atomic_load_explicit(&g_tls_ca_state, memory_order_acquire) == 2) return;
  int expected = 0;
  if (atomic_compare_exchange_strong_explicit(&g_tls_ca_state, &expected, 1,
                                              memory_order_acq_rel, memory_order_acquire)) {
    tls_ca_resolve_impl();
    atomic_store_explicit(&g_tls_ca_state, 2, memory_order_release);
  } else {
    while (atomic_load_explicit(&g_tls_ca_state, memory_order_acquire) != 2) {
      ccord_sleep_ms(1);
    }
  }
}
#endif

void ccord_curl_apply_tls(CURL *curl)
{
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  tls_ca_ensure_resolved();
  if (g_tls_ca_mode == TLS_CA_FILE) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, g_tls_ca_path);
  } else if (g_tls_ca_mode == TLS_CA_DIR) {
    curl_easy_setopt(curl, CURLOPT_CAPATH, g_tls_ca_path);
  }
}

static void ensure_curl(void) { ccord_curl_global_init_once(); }

static void wipe_cstr(char *s)
{
  if (s && s[0] != '\0') {
    ccord_live_wipe(s, strlen(s));
    s[0] = '\0';
  }
}

static bool rest_is_sensitive(bool raw, bool pre_auth, ccord_rest_method method, const char *path)
{
  if (pre_auth) return true;
  if (raw || !path) return false;
  if (strncmp(path, "/auth/", 6) == 0) return true;
  if (strstr(path, "mfa") != nullptr || strstr(path, "token") != nullptr ||
      strstr(path, "sessions/logout") != nullptr || strstr(path, "/webhooks") != nullptr) {
    return true;
  }
  if (method != CCORD_REST_GET &&
      (strcmp(path, "/users/@me") == 0 || strncmp(path, "/users/@me/delete", 17) == 0 ||
       strncmp(path, "/users/@me/disable", 18) == 0)) {
    return true;
  }
  return false;
}

ccord_status ccord_live_rest_request(const ccord_rest_config *cfg, ccord_rest_method method,
                                     const char *path, const char *json_body,
                                     ccord_buf *response, long *http_status, ccord_error *err)
{
  ccord_error_clear(err);
  if (!cfg || !path || !response) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null argument");
  if (!ccord_live_rest_path_allowed(path)) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "rest: payment endpoint blocked");
  }
  size_t body_len = json_body ? strlen(json_body) : 0;
  if (body_len > (size_t)LONG_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "rest: body too large");

  ccord_buf url;
  ccord_buf_init(&url, 8192);
  ccord_status st = ccord_live_rest_build_url(cfg->base_url, path, &url, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&url);
    return st;
  }

  ensure_curl();
  CURL *curl = curl_easy_init();
  if (!curl) {
    ccord_buf_free(&url);
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "rest: curl init failed");
  }
  rest_sink sink = {
    .buf = response,
    .max = cfg->max_response_bytes ? cfg->max_response_bytes : CCORD_LIVE_MAX_RESPONSE,
    .too_large = false,
  };
  char auth[CCORD_LIVE_TOKEN_MAX + 32];
  auth[0] = '\0';
  bool have_auth = false;
  if (cfg->token && cfg->token[0] != '\0') {
    int n = snprintf(auth, sizeof auth, "Authorization: %s", cfg->token);
    if (n < 0 || (size_t)n >= sizeof auth) {
      curl_easy_cleanup(curl);
      wipe_cstr(auth);
      ccord_buf_free(&url);
      return ccord_error_set(err, CCORD_ERR_RANGE, "rest: token too long");
    }
    have_auth = true;
  }
  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  char ua_hdr[256];
  int uan = snprintf(ua_hdr, sizeof ua_hdr, "User-Agent: %s", ccord_live_user_agent());
  if (uan < 0 || (size_t)uan >= sizeof ua_hdr) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    wipe_cstr(auth);
    ccord_buf_free(&url);
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "rest: user agent too long");
  }
  headers = curl_slist_append(headers, ua_hdr);
  const char *super = ccord_live_super_properties();
  char super_header[1200];
  int sn = snprintf(super_header, sizeof super_header, "X-Super-Properties: %s", super);
  if (sn < 0 || (size_t)sn >= sizeof super_header) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    wipe_cstr(auth);
    ccord_buf_free(&url);
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "rest: super properties too long");
  }
  headers = curl_slist_append(headers, super_header);
  if (have_auth) headers = curl_slist_append(headers, auth);

  curl_easy_setopt(curl, CURLOPT_URL, ccord_buf_cstr(&url));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, rest_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  ccord_curl_apply_tls(curl);
  uint32_t timeout = cfg->timeout_ms ? cfg->timeout_ms : CCORD_LIVE_TIMEOUT_DEFAULT_MS;
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout);
  CURLcode mc = set_method(curl, method, json_body, body_len);
  if (mc != CURLE_OK) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    wipe_cstr(auth);
    ccord_buf_free(&url);
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "rest: unsupported method");
  }

  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  if (rc == CURLE_OK) (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (http_status) *http_status = status;
  if (rc != CURLE_OK) {
    if (rc == CURLE_OPERATION_TIMEDOUT) {
      st = ccord_error_set(err, CCORD_ERR_TIMEOUT, "rest: request timed out");
    } else if (rc == CURLE_WRITE_ERROR && sink.too_large) {
      st = ccord_error_set(err, CCORD_ERR_RANGE, "rest: response too large");
    } else if (rc == CURLE_UNSUPPORTED_PROTOCOL) {
      st = ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "rest: unsupported protocol");
    } else {
      st = ccord_error_set(err, CCORD_ERR_IO, "rest: %s", method_name(method));
    }
  } else {
    st = CCORD_OK;
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  wipe_cstr(auth);
  ccord_buf_free(&url);
  return st;
}

// ---- non-blocking curl_multi queue, scheduled by the request governor ----
//
// Submitting only queues the request. ccord_rest_queue_pump (called every frame) asks the governor which pending
// requests may start, in priority order, feeds each response's rate-limit headers back to it, and re-queues
// requests that were rate limited or hit a transient server failure.

#define CCORD_REST_MAX_RETRIES 6        // rate-limit (429) retries per request
#define CCORD_REST_MAX_FAIL_RETRIES 2   // 5xx / network retries, GET only (never re-send a possibly applied write)
#define CCORD_REST_MAX_JOBS 256u
#define CCORD_REST_REASON_HEADER_MAX 1700u   // "X-Audit-Log-Reason: " + a 512-byte reason, every byte escaped
#define CCORD_REST_MAX_WAIT_MS 45000u   // non-user work that cannot start within this long is dropped
#define CCORD_REST_MAX_RAW_INFLIGHT 4u
#define CCORD_REST_BULK_MIN_BYTES_PER_S 2048u   // a media download slower than this ...
#define CCORD_REST_BULK_STALL_S 20u             // ... for this long has stalled
#define CCORD_REST_BULK_TIMEOUT_MS 300000u      // hard ceiling for one media download
#define CCORD_REST_HANDLE_POOL_MAX 8u           // reusable CURL easy handles kept warm per queue

typedef enum rest_job_state : uint8_t { JOB_PENDING = 0, JOB_RUNNING } rest_job_state;

typedef struct rest_follower {
  ccord_rest_callback cb;
  void *ud;
  struct rest_follower *next;
} rest_follower;

typedef struct ccord_rest_job {
  CURL *easy;
  struct curl_slist *headers;
  ccord_buf url, body, resp;
  rest_sink sink;
  char auth[CCORD_LIVE_TOKEN_MAX + 32];
  bool have_auth;
  long http_status;
  int retries;
  int fail_retries;
  uint64_t not_before_ms;
  uint64_t enqueued_ms;  // 0 until the first pump after submission
  rest_job_state state;
  bool raw;              // absolute URL: not governed
  bool bulk;             // media download: stall timeout instead of a total-transfer timeout
  bool sensitive;        // auth / MFA / token / credentials: wipe body & response on free
  bool wait_inflight;    // waiting for a governor inflight slot: wake immediately when a governed job finishes
  size_t max_response;   // per-job response cap (0 = the queue default)
  bool pre_auth;         // login/MFA: no Authorization header; 401 does not latch the governor
  ccord_rest_method method;
  char *route_path;      // API path handed to the governor (null for raw)
  char *content_type;
  char *audit_reason;
  ccord_gov_priority prio;
  ccord_gov_ticket ticket;
  ccord_gov_response hdrs;
  ccord_rest_callback cb;
  ccord_rest_progress_cb progress_cb;
  uint64_t last_ulnow, last_ultotal;
  bool progress_reported;
  void *ud;
  rest_follower *followers;
  // Ranged stream job (see ccord_rest_request.stream_range). cr_* is rebuilt from every response's headers, so after
  // a redirect it describes the final answer only.
  bool stream;
  int64_t range_start;
  size_t range_len;
  char if_range[96];
  ccord_rest_range_info *range_info;
  bool cr_seen;          // a Content-Range header
  int64_t cr_start, cr_end, cr_total;   // "bytes a-b/T" (start/end -1 for "bytes */T"; total -1 for "*")
  bool accept_ranges;
  char etag[96];
  struct ccord_rest_job *next;
} ccord_rest_job;

struct ccord_rest_queue {
  CURLM *multi;
  CURL *easy_pool[CCORD_REST_HANDLE_POOL_MAX];
  size_t easy_pool_count;
  ccord_rest_job *jobs;
  size_t njobs;
  size_t running_jobs;
  ccord_rest_job *notifying_job;
  ccord_gov *gov;
  uint32_t raw_inflight;
  uint32_t timeout_ms;
  size_t max_response;
  char *base_url;
  char *token;
  bool test_allow_http_streams;
};

static CURL *rest_pool_take(ccord_rest_queue *q)
{
  if (q && q->easy_pool_count > 0) {
    CURL *e = q->easy_pool[--q->easy_pool_count];
    q->easy_pool[q->easy_pool_count] = nullptr;
    return e;
  }
  return curl_easy_init();
}

static void rest_pool_put(ccord_rest_queue *q, CURL *easy)
{
  if (!easy) return;
  if (q && q->easy_pool_count < CCORD_REST_HANDLE_POOL_MAX) {
    curl_easy_reset(easy);
    q->easy_pool[q->easy_pool_count++] = easy;
    return;
  }
  curl_easy_cleanup(easy);
}

static void rest_job_detach_easy(ccord_rest_queue *q, ccord_rest_job *job, bool pool)
{
  if (!job) return;
  if (job->easy) {
    if (pool) rest_pool_put(q, job->easy);
    else curl_easy_cleanup(job->easy);
    job->easy = nullptr;
    job->sink.easy = nullptr;
  }
  if (job->headers) {
    curl_slist_free_all(job->headers);
    job->headers = nullptr;
  }
  wipe_cstr(job->auth);
  job->have_auth = false;
}

static void rest_job_free(ccord_rest_job *job)
{
  if (!job) return;
  if (job->easy) curl_easy_cleanup(job->easy);
  if (job->headers) curl_slist_free_all(job->headers);
  ccord_buf_free(&job->url);
  if (job->sensitive) {
    if (job->body.data && job->body.len > 0) ccord_live_wipe(job->body.data, job->body.len);
    if (job->resp.data && job->resp.len > 0) ccord_live_wipe(job->resp.data, job->resp.len);
  }
  ccord_buf_free(&job->body);
  ccord_buf_free(&job->resp);
  wipe_cstr(job->auth);
  ccord_free(job->route_path);
  ccord_free(job->content_type);
  ccord_free(job->audit_reason);
  for (rest_follower *f = job->followers; f;) {
    rest_follower *next = f->next;
    ccord_free(f);
    f = next;
  }
  ccord_free(job);
}

// If header line `buf` is `name:` (case-insensitive), returns its trimmed value (not NUL-terminated).
static const char *rest_header_value(const char *buf, size_t n, const char *name, size_t *out_len)
{
  size_t nl = strlen(name);
  if (n <= nl + 1 || !ascii_eq_ci(buf, name, nl) || buf[nl] != ':') return nullptr;
  const char *v = buf + nl + 1;
  const char *end = buf + n;
  while (v < end && (*v == ' ' || *v == '\t')) v++;
  while (end > v && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) end--;
  *out_len = (size_t)(end - v);
  return v;
}

static bool rest_parse_i64(const char **pp, const char *end, int64_t *out)
{
  const char *p = *pp;
  if (p >= end || *p < '0' || *p > '9') return false;
  int64_t v = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    if (v > (INT64_MAX - 9) / 10) return false;
    v = v * 10 + (*p - '0');
    p++;
  }
  *out = v;
  *pp = p;
  return true;
}

// Content-Range: "bytes a-b/T", "bytes a-b/*" or "bytes */T" (a 416). Anything else is left unseen.
static void rest_parse_content_range(ccord_rest_job *job, const char *v, size_t n)
{
  const char *end = v + n;
  if (n < 7 || !ascii_eq_ci(v, "bytes ", 6)) return;
  const char *p = v + 6;
  int64_t a = -1, b = -1, t = -1;
  if (p < end && *p == '*') {
    p++;
  } else if (!rest_parse_i64(&p, end, &a) || p >= end || *p++ != '-' || !rest_parse_i64(&p, end, &b) || b < a) {
    return;
  }
  if (p >= end || *p++ != '/') return;
  if (p < end && *p == '*') {
    p++;
  } else if (!rest_parse_i64(&p, end, &t)) {
    return;
  }
  if (p != end) return;
  job->cr_seen = true;
  job->cr_start = a;
  job->cr_end = b;
  job->cr_total = t;
}

static void rest_stream_header(ccord_rest_job *job, const char *buf, size_t n)
{
  size_t vl = 0;
  const char *v;
  if ((v = rest_header_value(buf, n, "Content-Range", &vl))) {
    rest_parse_content_range(job, v, vl);
  } else if ((v = rest_header_value(buf, n, "Accept-Ranges", &vl))) {
    if (vl >= 5 && ascii_eq_ci(v, "bytes", 5)) job->accept_ranges = true;
  } else if ((v = rest_header_value(buf, n, "ETag", &vl))) {
    // Only a strong validator is usable for If-Range.
    if (vl >= 2 && vl < sizeof job->etag && v[0] == '"') {
      memcpy(job->etag, v, vl);
      job->etag[vl] = '\0';
    } else {
      job->etag[0] = '\0';
    }
  }
}

static void rest_stream_reset_response(ccord_rest_job *job)
{
  job->cr_seen = false;
  job->cr_start = job->cr_end = job->cr_total = -1;
  job->accept_ranges = false;
  job->etag[0] = '\0';
}

static int rest_xferinfo_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
  (void)dltotal;
  (void)dlnow;
  ccord_rest_job *job = ud;
  if (!job || !job->progress_cb) return 0;
  uint64_t total = ultotal > 0 ? (uint64_t)ultotal : (uint64_t)job->body.len;
  uint64_t now = ulnow > 0 ? (uint64_t)ulnow : 0u;
  if (total > 0 && now > total) now = total;
  if (now != job->last_ulnow || total != job->last_ultotal || !job->progress_reported) {
    job->progress_reported = true;
    job->last_ulnow = now;
    job->last_ultotal = total;
    job->progress_cb(job->ud, now, total);
  }
  return 0;
}

static size_t rest_header_cb(char *buf, size_t size, size_t nitems, void *ud)
{
  ccord_rest_job *job = ud;
  if (size != 0 && nitems > SIZE_MAX / size) return 0;
  size_t n = size * nitems;
  if (n >= 5 && memcmp(buf, "HTTP/", 5) == 0) {
    ccord_gov_response_init(&job->hdrs, 0);  // new response (redirect etc.)
    if (job->stream) rest_stream_reset_response(job);
  } else {
    ccord_gov_response_header(&job->hdrs, buf, n);
    if (job->stream) rest_stream_header(job, buf, n);
  }
  return n;
}

static bool rest_path_ok(const char *path)
{
  return path && path[0] == '/' && ccord_live_rest_path_allowed(path);
}

ccord_rest_queue *ccord_rest_queue_create(const ccord_rest_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  ensure_curl();
  ccord_rest_queue *q = ccord_calloc(1, sizeof *q);
  if (!q) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    return nullptr;
  }
  q->multi = curl_multi_init();
  q->gov = ccord_gov_create(cfg ? cfg->governor : nullptr);
  if (!q->multi || !q->gov) {
    ccord_rest_queue_destroy(q);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "rest: queue init failed");
    return nullptr;
  }
  const char *base = cfg && cfg->base_url && cfg->base_url[0] ? cfg->base_url : CCORD_LIVE_API_BASE;
  size_t bn = strlen(base);
  q->base_url = ccord_strdup(base);
  q->token = cfg && cfg->token ? ccord_strdup(cfg->token) : nullptr;
  q->timeout_ms = cfg && cfg->timeout_ms ? cfg->timeout_ms : CCORD_LIVE_TIMEOUT_DEFAULT_MS;
  q->max_response = cfg && cfg->max_response_bytes ? cfg->max_response_bytes : CCORD_LIVE_MAX_RESPONSE;
  q->test_allow_http_streams = cfg && cfg->test_allow_http_streams;
  if (!q->base_url || (cfg && cfg->token && !q->token) || bn == 0) {
    ccord_rest_queue_destroy(q);
    ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    return nullptr;
  }
  return q;
}

char *ccord_rest_take_active_resp(struct ccord_rest_queue *q, size_t *out_len)
{
  if (out_len) *out_len = 0;
  if (!q || !q->notifying_job) return nullptr;
  ccord_rest_job *job = q->notifying_job;
  if (job->followers || job->sensitive || !job->resp.data || job->resp.len == 0) return nullptr;
  char *data = job->resp.data;
  size_t len = job->resp.len;
  job->resp.data = nullptr;
  job->resp.len = 0;
  job->resp.cap = 0;
  if (out_len) *out_len = len;
  return data;
}

static void rest_notify(ccord_rest_queue *q, ccord_rest_job *job, ccord_status st, long status)
{
  if (job->cb) {
    ccord_rest_job *prev = q->notifying_job;
    q->notifying_job = (!job->followers && !job->sensitive) ? job : nullptr;
    job->cb(job->ud, st, status, job->resp.data, job->resp.len);
    q->notifying_job = prev;
  }
  for (rest_follower *f = job->followers; f; f = f->next) {
    if (f->cb) f->cb(f->ud, st, status, job->resp.data, job->resp.len);
  }
}

void ccord_rest_queue_destroy(ccord_rest_queue *q)
{
  if (!q) return;
  ccord_rest_job *job = q->jobs;
  q->jobs = nullptr;
  q->njobs = 0;
  q->running_jobs = 0;
  while (job) {
    ccord_rest_job *next = job->next;
    if (job->state == JOB_RUNNING && job->easy && q->multi) curl_multi_remove_handle(q->multi, job->easy);
    if (job->ticket.active) ccord_gov_cancel(q->gov, &job->ticket);
    rest_notify(q, job, CCORD_ERR_IO, 0);
    rest_job_free(job);
    job = next;
  }
  for (size_t i = 0; i < q->easy_pool_count; i++) {
    curl_easy_cleanup(q->easy_pool[i]);
    q->easy_pool[i] = nullptr;
  }
  q->easy_pool_count = 0;
  if (q->multi) curl_multi_cleanup(q->multi);
  ccord_gov_destroy(q->gov);
  if (q->token) {
    ccord_live_wipe(q->token, strlen(q->token));
    ccord_free(q->token);
  }
  ccord_free(q->base_url);
  ccord_free(q);
}

// Discord wants the audit-log reason percent-encoded UTF-8. Everything outside the unreserved set is escaped, which
// also makes it impossible for a reason to carry a CR/LF and inject another header.
static bool rest_audit_header(const char *reason, char *out, size_t cap)
{
  static const char hex[] = "0123456789ABCDEF";
  static const char prefix[] = "X-Audit-Log-Reason: ";
  size_t n = sizeof prefix - 1u;
  if (cap <= n) return false;
  memcpy(out, prefix, n);
  for (const unsigned char *p = (const unsigned char *)reason; *p; p++) {
    bool plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' ||
                 *p == '_' || *p == '.' || *p == '~';
    if (n + (plain ? 1u : 3u) >= cap) return false;
    if (plain) {
      out[n++] = (char)*p;
    } else {
      out[n++] = '%';
      out[n++] = hex[*p >> 4];
      out[n++] = hex[*p & 0x0Fu];
    }
  }
  out[n] = '\0';
  return true;
}

static bool rest_method_valid(ccord_rest_method method)
{
  switch (method) {
    case CCORD_REST_GET:
    case CCORD_REST_POST:
    case CCORD_REST_PATCH:
    case CCORD_REST_PUT:
    case CCORD_REST_DELETE:
      return true;
  }
  return false;
}

// Ranged stream job (a clicked video): one `Range: bytes=a-b` GET, identity encoding, no Authorization, no cookie
// engine, no netrc, TLS verified. Redirects are followed (5 hops) but only to https: a `file:`, `ftp:` or `http:`
// target ends the job with CURLE_UNSUPPORTED_PROTOCOL. The two test_allow_http_streams settings are the only place
// http is accepted, so a loopback server can be used. CURLOPT_RANGE is re-sent on every hop.
static curl_socket_t rest_opensocket_cb(void *clientp, curlsocktype purpose, struct curl_sockaddr *address)
{
  (void)clientp;
  (void)purpose;
  if (address->family == AF_INET && address->addrlen >= sizeof(struct sockaddr_in)) {
    struct sockaddr_in sin;
    memcpy(&sin, &address->addr, sizeof sin);
    uint32_t ip = ntohl(sin.sin_addr.s_addr);
    // 127.0.0.0/8
    if ((ip & 0xFF000000u) == 0x7F000000u) return CURL_SOCKET_BAD;
    // 10.0.0.0/8
    if ((ip & 0xFF000000u) == 0x0A000000u) return CURL_SOCKET_BAD;
    // 172.16.0.0/12
    if ((ip & 0xFFF00000u) == 0xAC100000u) return CURL_SOCKET_BAD;
    // 192.168.0.0/16
    if ((ip & 0xFFFF0000u) == 0xC0A80000u) return CURL_SOCKET_BAD;
    // 169.254.0.0/16 (link-local)
    if ((ip & 0xFFFF0000u) == 0xA9FE0000u) return CURL_SOCKET_BAD;
    // 0.0.0.0 (unspecified)
    if (ip == 0) return CURL_SOCKET_BAD;
  } else if (address->family == AF_INET6 && address->addrlen >= sizeof(struct sockaddr_in6)) {
    struct sockaddr_in6 sin6;
    memcpy(&sin6, &address->addr, sizeof sin6);
    const uint8_t *b = sin6.sin6_addr.s6_addr;
    // ::1 loopback
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0 &&
        b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0 &&
        b[8] == 0 && b[9] == 0 && b[10] == 0 && b[11] == 0 &&
        b[12] == 0 && b[13] == 0 && b[14] == 0 && b[15] == 1) return CURL_SOCKET_BAD;
    // :: unspecified
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0 &&
        b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0 &&
        b[8] == 0 && b[9] == 0 && b[10] == 0 && b[11] == 0 &&
        b[12] == 0 && b[13] == 0 && b[14] == 0 && b[15] == 0) return CURL_SOCKET_BAD;
    // fe80::/10 link-local
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return CURL_SOCKET_BAD;
  }
  return socket(address->family, address->socktype, address->protocol);
}

static bool rest_job_prepare_stream(ccord_rest_queue *q, ccord_rest_job *job)
{
  job->headers = curl_slist_append(job->headers, "Accept-Encoding: identity");
  if (job->if_range[0] != '\0') {
    char hdr[128];
    int n = snprintf(hdr, sizeof hdr, "If-Range: %s", job->if_range);
    if (n < 0 || (size_t)n >= sizeof hdr) return false;
    job->headers = curl_slist_append(job->headers, hdr);
  }
  if (!job->headers) return false;
  curl_easy_setopt(job->easy, CURLOPT_HTTPHEADER, job->headers);
  char range[64];
  int rn = snprintf(range, sizeof range, "%lld-%lld", (long long)job->range_start,
                    (long long)(job->range_start + (int64_t)job->range_len - 1));
  if (rn < 0 || (size_t)rn >= sizeof range) return false;
  curl_easy_setopt(job->easy, CURLOPT_RANGE, range);   // curl copies the string
  const char *protos = q->test_allow_http_streams ? "http,https" : "https";
  curl_easy_setopt(job->easy, CURLOPT_PROTOCOLS_STR, protos);
  curl_easy_setopt(job->easy, CURLOPT_REDIR_PROTOCOLS_STR, protos);
  curl_easy_setopt(job->easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(job->easy, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(job->easy, CURLOPT_NETRC, (long)CURL_NETRC_IGNORED);
  curl_easy_setopt(job->easy, CURLOPT_AUTOREFERER, 0L);
  if (!q->test_allow_http_streams) {
    curl_easy_setopt(job->easy, CURLOPT_OPENSOCKETFUNCTION, rest_opensocket_cb);
  }
  return true;
}

// Acquires a pooled CURL handle and binds headers/options right as a job transitions to in-flight.
static bool rest_job_attach(ccord_rest_queue *q, ccord_rest_job *job)
{
  rest_job_detach_easy(q, job, true);
  job->easy = rest_pool_take(q);
  if (!job->easy) return false;
  job->sink.buf = &job->resp;
  job->sink.max = job->max_response ? job->max_response : q->max_response;
  job->sink.too_large = false;
  job->sink.truncated = false;
  job->sink.stream = job->stream;
  job->sink.easy = job->easy;

  bool include_auth = !job->raw;
  job->auth[0] = '\0';
  job->have_auth = false;
  if (include_auth && !job->pre_auth && q->token && q->token[0] != '\0') {
    int n = snprintf(job->auth, sizeof job->auth, "Authorization: %s", q->token);
    if (n < 0 || (size_t)n >= sizeof job->auth) {
      rest_job_detach_easy(q, job, true);
      return false;
    }
    job->have_auth = true;
  }
  const char *content_type =
      job->raw ? job->content_type
               : (job->content_type && job->content_type[0] != '\0' ? job->content_type : "application/json");
  if (content_type && content_type[0] != '\0') {
    char ct_hdr[128];
    int cn = snprintf(ct_hdr, sizeof ct_hdr, "Content-Type: %s", content_type);
    if (cn < 0 || (size_t)cn >= sizeof ct_hdr) {
      rest_job_detach_easy(q, job, true);
      return false;
    }
    job->headers = curl_slist_append(job->headers, ct_hdr);
  }
  char ua_hdr[256];
  int uan = snprintf(ua_hdr, sizeof ua_hdr, "User-Agent: %s", ccord_live_user_agent());
  if (uan < 0 || (size_t)uan >= sizeof ua_hdr) {
    rest_job_detach_easy(q, job, true);
    return false;
  }
  job->headers = curl_slist_append(job->headers, ua_hdr);
  if (include_auth || job->pre_auth) {
    char super_header[1200];
    int sn = snprintf(super_header, sizeof super_header, "X-Super-Properties: %s", ccord_live_super_properties());
    if (sn < 0 || (size_t)sn >= sizeof super_header) {
      rest_job_detach_easy(q, job, true);
      return false;
    }
    job->headers = curl_slist_append(job->headers, super_header);
    if (include_auth) {
      if (job->have_auth) job->headers = curl_slist_append(job->headers, job->auth);
      if (job->audit_reason && job->audit_reason[0] != '\0') {
        char reason_hdr[CCORD_REST_REASON_HEADER_MAX];
        if (!rest_audit_header(job->audit_reason, reason_hdr, sizeof reason_hdr)) {
          rest_job_detach_easy(q, job, true);
          return false;
        }
        job->headers = curl_slist_append(job->headers, reason_hdr);
      }
    }
  }
  if (!job->headers) {
    rest_job_detach_easy(q, job, true);
    return false;
  }

  curl_easy_setopt(job->easy, CURLOPT_URL, ccord_buf_cstr(&job->url));
  curl_easy_setopt(job->easy, CURLOPT_HTTPHEADER, job->headers);
  curl_easy_setopt(job->easy, CURLOPT_WRITEFUNCTION, rest_write);
  curl_easy_setopt(job->easy, CURLOPT_WRITEDATA, &job->sink);
  curl_easy_setopt(job->easy, CURLOPT_HEADERFUNCTION, rest_header_cb);
  curl_easy_setopt(job->easy, CURLOPT_HEADERDATA, job);
  if (job->progress_cb) {
    curl_easy_setopt(job->easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(job->easy, CURLOPT_XFERINFOFUNCTION, rest_xferinfo_cb);
    curl_easy_setopt(job->easy, CURLOPT_XFERINFODATA, job);
  }
  curl_easy_setopt(job->easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(job->easy, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(job->easy, CURLOPT_MAXREDIRS, 0L);
  if (!job->stream) curl_easy_setopt(job->easy, CURLOPT_ACCEPT_ENCODING, "");
  ccord_curl_apply_tls(job->easy);
  curl_easy_setopt(job->easy, CURLOPT_CONNECTTIMEOUT_MS, (long)q->timeout_ms);
  if (job->bulk) {
    // A large download is healthy for as long as bytes keep arriving: give up when it stalls, with a generous
    // overall ceiling, instead of cutting every transfer that needs more than the API timeout.
    curl_easy_setopt(job->easy, CURLOPT_LOW_SPEED_LIMIT, (long)CCORD_REST_BULK_MIN_BYTES_PER_S);
    curl_easy_setopt(job->easy, CURLOPT_LOW_SPEED_TIME, (long)CCORD_REST_BULK_STALL_S);
    curl_easy_setopt(job->easy, CURLOPT_TIMEOUT_MS, (long)CCORD_REST_BULK_TIMEOUT_MS);
  } else {
    curl_easy_setopt(job->easy, CURLOPT_TIMEOUT_MS, (long)q->timeout_ms);
  }
  if (job->body.len > (size_t)LONG_MAX || set_method(job->easy, job->method, job->body.data, job->body.len) != CURLE_OK) {
    rest_job_detach_easy(q, job, true);
    return false;
  }
  if (job->stream && !rest_job_prepare_stream(q, job)) {
    rest_job_detach_easy(q, job, true);
    return false;
  }
  return true;
}

static bool full_url_allowed(const char *url)
{
  if (!url) return false;
  const char *p = nullptr;
  if (strncmp(url, "https://", 8) == 0) p = url + 8;
  else if (strncmp(url, "http://", 7) == 0) p = url + 7;
  else return false;
  const char *slash = strchr(p, '/');
  if (!slash) return true;
  return ccord_live_rest_path_allowed(slash);
}

ccord_rest_request ccord_rest_request_make(ccord_rest_method method, const char *path, ccord_gov_priority priority)
{
  ccord_rest_request r = {0};
  r.method = method;
  r.path = path;
  r.priority = priority;
  return r;
}

static void rest_append_job(ccord_rest_queue *q, ccord_rest_job *job)
{
  ccord_rest_job **slot = &q->jobs;
  while (*slot) slot = &(*slot)->next;
  *slot = job;
  q->njobs++;
}

static void rest_unlink_job(ccord_rest_queue *q, ccord_rest_job *job)
{
  for (ccord_rest_job **slot = &q->jobs; *slot; slot = &(*slot)->next) {
    if (*slot == job) {
      *slot = job->next;
      job->next = nullptr;
      if (q->njobs > 0) q->njobs--;
      return;
    }
  }
}

ccord_status ccord_rest_queue_submit(ccord_rest_queue *q, const ccord_rest_request *req, ccord_error *err)
{
  ccord_error_clear(err);
  if (!q || !req) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null argument");
  if (req->priority >= CCORD_GOV_PRIORITY_COUNT) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad priority");
  if (!rest_method_valid(req->method)) return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "rest: unsupported method");
  bool raw = req->full_url != nullptr;
  if (req->body_len > 0 && !req->body) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null body");
  if (req->body_len > (size_t)LONG_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "rest: body too large");
  if (req->stream_range) {
    // A clicked video, one byte range: only the user-video gate (https, DNS host, port 443), or a loopback URL in
    // tests. No byte <= 0x20 or 0x7f and at most 2048 bytes, so the URL cannot smuggle a header or a second request.
    if (!raw || req->method != CCORD_REST_GET || req->body_len > 0 || req->range_start < 0 || req->range_len < 1 ||
        req->range_len > CCORD_STREAM_RANGE_MAX_BYTES || req->range_start > INT64_MAX - (int64_t)req->range_len) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad range request");
    }
    size_t ul = strlen(req->full_url);
    bool clean = ul > 0 && ul <= 2048u;
    for (size_t i = 0; clean && i < ul; i++) {
      unsigned char c = (unsigned char)req->full_url[i];
      if (c <= 0x20 || c == 0x7f) clean = false;
    }
    if (!clean || !(q->test_allow_http_streams ? full_url_allowed(req->full_url)
                                               : ccord_live_user_video_url_allowed(req->full_url))) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION, "rest: URL blocked");
    }
    if (req->if_range) {
      size_t il = strlen(req->if_range);
      if (il >= 96u) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad If-Range");
      for (size_t i = 0; i < il; i++) {
        unsigned char c = (unsigned char)req->if_range[i];
        if (c < 0x20 || c == 0x7f) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad If-Range");
      }
    }
  } else if (raw) {
    if (!full_url_allowed(req->full_url)) return ccord_error_set(err, CCORD_ERR_PERMISSION, "rest: URL blocked");
  } else if (!req->path) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null argument");
  } else if (!rest_path_ok(req->path)) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "rest: payment endpoint blocked");
  }
  if (!raw && !req->pre_auth && q->token && q->token[0] != '\0' &&
      strlen(q->token) + 15u >= sizeof((ccord_rest_job *)nullptr)->auth) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "rest: token too long");
  }
  const char *ct = raw ? req->content_type
                       : (req->content_type && req->content_type[0] != '\0' ? req->content_type : "application/json");
  if (ct && ct[0] != '\0' && strlen(ct) + 14u >= 128u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "rest: content-type too long");
  }
  if (!raw && req->audit_reason && req->audit_reason[0] != '\0') {
    char reason_hdr[CCORD_REST_REASON_HEADER_MAX];
    if (!rest_audit_header(req->audit_reason, reason_hdr, sizeof reason_hdr)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "rest: audit reason too long");
    }
  }
  if (q->njobs >= CCORD_REST_MAX_JOBS && req->priority != CCORD_GOV_USER) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "rest: queue full");
  }

  // Coalesce an identical in-flight GET before allocating a new job: the caller's callback joins the existing request.
  if (req->dedupe && !raw && req->method == CCORD_REST_GET) {
    for (ccord_rest_job *other = q->jobs; other; other = other->next) {
      if (other->raw || other->method != CCORD_REST_GET || !other->route_path) continue;
      if (strcmp(other->route_path, req->path) != 0) continue;
      rest_follower *f = req->cb ? ccord_calloc(1, sizeof *f) : nullptr;
      if (req->cb && !f) return ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      if (f) {
        f->cb = req->cb;
        f->ud = req->ud;
        f->next = other->followers;
        other->followers = f;
      }
      if (req->priority < other->prio) other->prio = req->priority;  // a more urgent asker upgrades the shared job
      return CCORD_OK;
    }
  }

  ccord_rest_job *job = ccord_calloc(1, sizeof *job);
  if (!job) return ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
  ccord_buf_init(&job->url, 8192);
  ccord_buf_init(&job->body, 0);
  ccord_buf_init(&job->resp, 0);
  ccord_gov_response_init(&job->hdrs, 0);
  job->cb = req->cb;
  job->progress_cb = req->progress_cb;
  job->ud = req->ud;
  job->raw = raw;
  job->bulk = raw && ((req->bulk && (req->method == CCORD_REST_GET || req->method == CCORD_REST_PUT)) ||
                      (req->stream_range && req->method == CCORD_REST_GET));
  job->max_response = req->max_response;
  if (req->stream_range) {
    job->stream = true;
    job->range_start = req->range_start;
    job->range_len = req->range_len;
    if (req->if_range && req->if_range[0] != '\0') {
      size_t il = strlen(req->if_range);
      memcpy(job->if_range, req->if_range, il + 1u);
    }
    job->range_info = req->range_info;
    job->max_response = req->range_len;   // the response cap is the requested length
    rest_stream_reset_response(job);
  }
  job->pre_auth = !raw && req->pre_auth;
  job->sensitive = rest_is_sensitive(raw, job->pre_auth, req->method, req->path);
  job->method = req->method;
  job->prio = req->priority;
  ccord_status st = CCORD_OK;
  if (raw) {
    st = ccord_buf_append_str(&job->url, req->full_url);
    if (st != CCORD_OK) st = ccord_error_set(err, CCORD_ERR_RANGE, "rest: URL too long");
    if (st == CCORD_OK && req->body_len > 0 && ccord_buf_append(&job->body, req->body, req->body_len) != CCORD_OK) {
      st = ccord_error_set(err, CCORD_ERR_RANGE, "rest: body too large");
    }
  } else {
    st = ccord_live_rest_build_url(q->base_url, req->path, &job->url, err);
    if (st == CCORD_OK) {
      if (req->body_len > 0) {
        if (ccord_buf_append(&job->body, req->body, req->body_len) != CCORD_OK) {
          st = ccord_error_set(err, CCORD_ERR_RANGE, "rest: body too large");
        }
      } else if (req->json_body && ccord_buf_append_str(&job->body, req->json_body) != CCORD_OK) {
        st = ccord_error_set(err, CCORD_ERR_RANGE, "rest: body too large");
      }
    }
    if (st == CCORD_OK) {
      job->route_path = ccord_strdup(req->path);
      if (!job->route_path) st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    }
  }

  if (st == CCORD_OK && req->content_type && req->content_type[0] != '\0') {
    job->content_type = ccord_strdup(req->content_type);
    if (!job->content_type) st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
  }
  if (st == CCORD_OK && !raw && req->audit_reason && req->audit_reason[0] != '\0') {
    job->audit_reason = ccord_strdup(req->audit_reason);
    if (!job->audit_reason) st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
  }
  if (st != CCORD_OK) {
    rest_job_free(job);
    return st;
  }
  rest_append_job(q, job);
  return CCORD_OK;
}

ccord_status ccord_rest_queue_add(ccord_rest_queue *q, ccord_rest_method method, const char *path,
                                  const char *json_body, ccord_rest_callback cb, void *ud, ccord_error *err)
{
  ccord_rest_request r = ccord_rest_request_make(method, path, CCORD_GOV_VISIBLE);
  r.json_body = json_body;
  r.cb = cb;
  r.ud = ud;
  return ccord_rest_queue_submit(q, &r, err);
}

ccord_status ccord_rest_queue_add_raw(ccord_rest_queue *q, ccord_rest_method method, const char *full_url,
                                      const char *content_type, const void *body, size_t body_len,
                                      ccord_rest_callback cb, void *ud, ccord_error *err)
{
  if (!full_url) {
    ccord_error_clear(err);
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null argument");
  }
  ccord_rest_request r = ccord_rest_request_make(method, nullptr, CCORD_GOV_VISIBLE);
  r.full_url = full_url;
  r.content_type = content_type;
  r.body = body;
  r.body_len = body_len;
  r.cb = cb;
  r.ud = ud;
  return ccord_rest_queue_submit(q, &r, err);
}

ccord_gov *ccord_rest_queue_governor(ccord_rest_queue *q) { return q ? q->gov : nullptr; }
size_t ccord_rest_queue_pending(const ccord_rest_queue *q) { return q ? q->njobs : 0; }
bool ccord_rest_queue_auth_failed(const ccord_rest_queue *q) { return q && ccord_gov_auth_failed(q->gov); }
void ccord_rest_queue_reset_auth(ccord_rest_queue *q)
{
  if (q) ccord_gov_reset_auth(q->gov);
}

void ccord_rest_queue_set_token(ccord_rest_queue *q, const char *token)
{
  if (!q) return;
  bool have = token && token[0] != '\0';
  char *next = have ? ccord_strdup(token) : nullptr;
  if (have && !next) return;  // out of memory: keep the old token rather than silently drop auth
  if (q->token) {
    ccord_live_wipe(q->token, strlen(q->token));
    ccord_free(q->token);
  }
  q->token = next;
  ccord_gov_reset_auth(q->gov);
  // Pending jobs do not hold a CURL handle or headers until rest_start(), so they pick up q->token automatically.
}

static ccord_status rest_result_status(CURLcode rc, const rest_sink *sink)
{
  if (rc == CURLE_OK) return CCORD_OK;
  if (rc == CURLE_OPERATION_TIMEDOUT) return CCORD_ERR_TIMEOUT;
  if (rc == CURLE_WRITE_ERROR && sink->truncated) return CCORD_OK;   // stream job: a 200 cut at the requested length
  if (rc == CURLE_WRITE_ERROR && sink->too_large) return CCORD_ERR_RANGE;
  if (rc == CURLE_UNSUPPORTED_PROTOCOL || rc == CURLE_TOO_MANY_REDIRECTS) return CCORD_ERR_UNSUPPORTED;
  return CCORD_ERR_IO;
}

// Unlinks the job, delivers the result and frees it. Safe against callbacks that submit new requests.
static void rest_finish(ccord_rest_queue *q, ccord_rest_job *job, ccord_status st, long status)
{
  rest_unlink_job(q, job);
  if (job->state == JOB_RUNNING) {
    curl_multi_remove_handle(q->multi, job->easy);
    if (q->running_jobs > 0) q->running_jobs--;
    if (job->raw && q->raw_inflight > 0) q->raw_inflight--;
    job->state = JOB_PENDING;
  }
  rest_job_detach_easy(q, job, true);
  if (job->ticket.active) ccord_gov_cancel(q->gov, &job->ticket);
  rest_notify(q, job, st, status);
  rest_job_free(job);
}

static bool rest_start(ccord_rest_queue *q, ccord_rest_job *job)
{
  ccord_buf_clear(&job->resp);
  ccord_gov_response_init(&job->hdrs, 0);
  if (job->stream) rest_stream_reset_response(job);
  if (!rest_job_attach(q, job)) return false;
  if (curl_multi_add_handle(q->multi, job->easy) != CURLM_OK) {
    rest_job_detach_easy(q, job, true);
    return false;
  }
  job->state = JOB_RUNNING;
  job->wait_inflight = false;
  q->running_jobs++;
  if (job->raw) q->raw_inflight++;
  return true;
}

// Fills the caller's range_info for a finished stream job and checks a 206 against what was asked: the server must
// start at the requested offset and send exactly the bytes its Content-Range names, no more than requested.
// Returns CCORD_ERR_PROTOCOL for a 206 that does not match (never trust a mismatched range as video bytes).
static ccord_status rest_stream_result(ccord_rest_job *job, CURLcode rc, long status)
{
  ccord_rest_range_info *ri = job->range_info;
  ccord_status st = CCORD_OK;
  if (rc == CURLE_OK && status == 206) {
    int64_t span = job->cr_seen && job->cr_start >= 0 ? job->cr_end - job->cr_start + 1 : -1;
    if (!job->cr_seen || job->cr_start != job->range_start || span < 1 || span > (int64_t)job->range_len ||
        (int64_t)job->resp.len != span) {
      st = CCORD_ERR_PROTOCOL;
    }
  }
  if (!ri) return st;
  memset(ri, 0, sizeof *ri);
  ri->http_status = status;
  ri->entity_total = -1;
  ri->truncated = job->sink.truncated;
  ri->accept_ranges = status == 206 || job->accept_ranges;
  (void)snprintf(ri->etag, sizeof ri->etag, "%s", job->etag);
  long hops = 0;
  (void)curl_easy_getinfo(job->easy, CURLINFO_REDIRECT_COUNT, &hops);
  ri->redirects = hops < 0 ? 0 : hops > 255 ? 255 : (uint8_t)hops;
  if (status == 206 && st == CCORD_OK) {
    ri->range_start = job->cr_start;
    ri->entity_total = job->cr_total;
  } else if (status == 200) {
    curl_off_t cl = -1;
    (void)curl_easy_getinfo(job->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    ri->entity_total = cl >= 0 ? (int64_t)cl : -1;
  } else if (status == 416 && job->cr_seen) {
    ri->entity_total = job->cr_total;
  }
  return st;
}

void ccord_rest_queue_pump(ccord_rest_queue *q, uint64_t now_ms)
{
  if (!q || q->njobs == 0) return;

  // Expire non-user work that has waited too long (for example behind a rate limit or an edge block). A raw
  // download (media) is not governed: it only waits for one of the raw slots, held by other downloads that
  // are making progress (each ends or stalls out on its own), so a video queued behind them is kept, not dropped:
  // the time spent waiting for a slot does not count.
  bool raw_slots_full = q->raw_inflight >= CCORD_REST_MAX_RAW_INFLIGHT;
  for (ccord_rest_job *job = q->jobs, *next; job; job = next) {
    next = job->next;
    if (job->enqueued_ms == 0) job->enqueued_ms = now_ms ? now_ms : 1u;
    if (job->state == JOB_PENDING && job->raw && raw_slots_full) {
      job->enqueued_ms = now_ms ? now_ms : 1u;
      continue;
    }
    if (job->state == JOB_PENDING && job->prio != CCORD_GOV_USER && now_ms > job->enqueued_ms &&
        now_ms - job->enqueued_ms > CCORD_REST_MAX_WAIT_MS) {
      rest_finish(q, job, CCORD_ERR_TIMEOUT, 0);
    }
  }

  // Start what the governor allows: user actions first, then visible state, then background; FIFO within a class.
  for (int cls = 0; cls < (int)CCORD_GOV_PRIORITY_COUNT; cls++) {
    for (ccord_rest_job *job = q->jobs, *next; job; job = next) {
      next = job->next;
      if (job->state != JOB_PENDING || (int)job->prio != cls || now_ms < job->not_before_ms) continue;
      if (job->raw) {
        if (q->raw_inflight >= CCORD_REST_MAX_RAW_INFLIGHT) continue;
        if (!rest_start(q, job)) job->not_before_ms = now_ms + 1000u;
        continue;
      }
      ccord_gov_decision d = job->pre_auth
                                 ? ccord_gov_acquire_preauth(q->gov, method_name(job->method), job->route_path,
                                                             job->prio, now_ms, &job->ticket)
                                 : ccord_gov_acquire(q->gov, method_name(job->method), job->route_path, job->prio,
                                                     now_ms, &job->ticket);
      if (d.verdict == CCORD_GOV_WAIT) {
        if (d.retry_at_ms > job->not_before_ms) job->not_before_ms = d.retry_at_ms;
        if (d.reason == CCORD_GOV_REASON_INFLIGHT ||
            (d.reason == CCORD_GOV_REASON_BUCKET && d.retry_at_ms == now_ms + 20u) ||
            (d.reason == CCORD_GOV_REASON_EDGE && job->prio == CCORD_GOV_USER)) {
          job->wait_inflight = true;
        }
        continue;
      }
      if (d.verdict == CCORD_GOV_REFUSE) {
        rest_finish(q, job, CCORD_ERR_PERMISSION, ccord_gov_auth_failed(q->gov) ? 401 : 0);
        continue;
      }
      if (!rest_start(q, job)) {
        ccord_gov_cancel(q->gov, &job->ticket);
        job->not_before_ms = now_ms + 1000u;
      }
    }
  }

  if (q->running_jobs == 0) return;
  int running = 0;
  if (curl_multi_perform(q->multi, &running) != CURLM_OK) return;
  bool wake_inflight = false;
  CURLMsg *msg = nullptr;
  int left = 0;
  while ((msg = curl_multi_info_read(q->multi, &left)) != nullptr) {
    if (msg->msg != CURLMSG_DONE) continue;
    CURL *easy = msg->easy_handle;
    ccord_rest_job *job = nullptr;
    for (ccord_rest_job *j = q->jobs; j; j = j->next) {
      if (j->easy == easy) {
        job = j;
        break;
      }
    }
    if (!job) {
      curl_multi_remove_handle(q->multi, easy);
      continue;
    }
    CURLcode rc = msg->data.result;
    long status = 0;
    if (rc == CURLE_OK || (job->stream && job->sink.truncated)) {
      (void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    }
    job->http_status = status;
    ccord_status stream_st = job->stream ? rest_stream_result(job, rc, status) : CCORD_OK;

    if (!job->raw) {
      ccord_gov_response r = job->hdrs;
      r.status = (int)status;
      r.network_error = rc != CURLE_OK;
      r.pre_auth = job->pre_auth;  // hdrs was re-zeroed per response line; carry the job flag into the governor
      if (status == 429) ccord_gov_response_body(&r, job->resp.data, job->resp.len);
      ccord_gov_completion done = ccord_gov_complete(q->gov, &job->ticket, &r, now_ms);
      wake_inflight = true;
      bool retry = false;
      if (done.should_retry) {
        if (status == 429 && job->retries < CCORD_REST_MAX_RETRIES) {
          job->retries++;
          retry = true;
        } else if ((status >= 500 || rc != CURLE_OK) && job->method == CCORD_REST_GET &&
                   job->fail_retries < CCORD_REST_MAX_FAIL_RETRIES) {
          job->fail_retries++;
          retry = true;
        }
      }
      if (retry) {
        curl_multi_remove_handle(q->multi, easy);
        if (q->running_jobs > 0) q->running_jobs--;
        rest_job_detach_easy(q, job, true);
        job->state = JOB_PENDING;
        job->wait_inflight = false;
        job->not_before_ms = done.not_before_ms;
        continue;
      }
    }
    ccord_status st = rest_result_status(rc, &job->sink);
    if (st == CCORD_OK) st = stream_st;
    if (status == 429 && job->retries >= CCORD_REST_MAX_RETRIES) st = CCORD_ERR_RANGE;
    rest_finish(q, job, st, status);
  }
  if (wake_inflight) {
    for (ccord_rest_job *j = q->jobs; j; j = j->next) {
      if (j->state == JOB_PENDING && j->wait_inflight) {
        j->not_before_ms = 0;
        j->wait_inflight = false;
      }
    }
  }
}

// Login/MFA share this path: a governed USER-priority POST with no Authorization header. The body is wiped
// after submit (the queue copies json_body), so credentials never outlive the call.
static ccord_status auth_submit(ccord_rest_queue *q, const char *path, ccord_buf *body, ccord_rest_callback cb,
                                void *ud, ccord_error *err)
{
  ccord_rest_request r = ccord_rest_request_make(CCORD_REST_POST, path, CCORD_GOV_USER);
  r.pre_auth = true;
  r.json_body = ccord_buf_cstr(body);
  r.cb = cb;
  r.ud = ud;
  ccord_status st = ccord_rest_queue_submit(q, &r, err);
  if (body->data && body->len > 0) ccord_live_wipe(body->data, body->len);
  ccord_buf_free(body);
  return st;
}

ccord_status ccord_live_auth_login(ccord_rest_queue *q, const char *login, const char *password,
                                   ccord_rest_callback cb, void *ud, ccord_error *err)
{
  ccord_error_clear(err);
  if (!q || !login || login[0] == '\0' || !password || password[0] == '\0') {
    return ccord_error_set(err, CCORD_ERR_INVALID, "auth: login and password are required");
  }

  ccord_buf body;
  ccord_buf_init(&body, 4096);
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "login");
  ccord_jw_cstr(&w, login);
  ccord_jw_key(&w, "password");
  ccord_jw_cstr(&w, password);
  ccord_jw_key(&w, "undelete");
  ccord_jw_bool(&w, false);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    if (body.data && body.len > 0) ccord_live_wipe(body.data, body.len);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_RANGE, "auth: login payload too large");
  }
  return auth_submit(q, "/auth/login", &body, cb, ud, err);
}

ccord_status ccord_live_auth_mfa_totp(ccord_rest_queue *q, const char *ticket, const char *code,
                                      ccord_rest_callback cb, void *ud, ccord_error *err)
{
  ccord_error_clear(err);
  if (!q || !ticket || ticket[0] == '\0' || !code || code[0] == '\0') {
    return ccord_error_set(err, CCORD_ERR_INVALID, "auth: ticket and 2FA code are required");
  }

  ccord_buf body;
  ccord_buf_init(&body, 4096);
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "code");
  ccord_jw_cstr(&w, code);
  ccord_jw_key(&w, "ticket");
  ccord_jw_cstr(&w, ticket);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    if (body.data && body.len > 0) ccord_live_wipe(body.data, body.len);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_RANGE, "auth: MFA payload too large");
  }
  return auth_submit(q, "/auth/mfa/totp", &body, cb, ud, err);
}
