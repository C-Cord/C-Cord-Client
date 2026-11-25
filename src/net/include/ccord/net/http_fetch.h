// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ccord_http_get_result {
  long status_code;
  char *body;          // owned (ccord_free), null-terminated when present
  size_t body_len;
  char etag[128];
  bool not_modified;
  char error[256];
} ccord_http_get_result;

// HTTPS-only GET with strict TLS 1.2/1.3 verification, host allowlisting, and optional If-None-Match ETag.
// Returns true on 200 OK or 304 Not Modified (in which case out->not_modified is true and out->body is nullptr).
bool ccord_http_get(const char *url, const char *if_none_match, size_t max_bytes, ccord_http_get_result *out);
// HTTPS-only JSON POST with strict TLS 1.2/1.3 verification and host allowlisting.
bool ccord_http_post_json(const char *url, const char *json_body, size_t body_len, size_t max_bytes,
                          ccord_http_get_result *out);
void ccord_http_get_result_free(ccord_http_get_result *r);

// HTTPS-only streaming file download to dest_path with strict TLS and size cap.
bool ccord_http_download_file(const char *url, const char *dest_path, size_t max_bytes, char *err_buf, size_t err_cap);

// Returns true when url is HTTPS and targets an allowlisted catalog / GitHub release host.
bool ccord_http_url_allowed(const char *url);

// Optional per-hop URL validator invoked before following a redirect (returns false and populates err_buf to block).
typedef bool (*ccord_http_url_check_fn)(void *ud, const char *url, char *err_buf, size_t err_cap);

// Generalized HTTP request (used by Class 3 plugin network.http / network.local).
// External URLs require strict HTTPS (TLS 1.2/1.3). Plain HTTP is permitted ONLY when
// allow_localhost_http == true and the host is loopback (127.0.0.1, localhost, or ::1).
typedef struct ccord_http_request_opts {
  const char *url;
  const char *method;            // "GET"|"POST"|"PUT"|"PATCH"|"DELETE"|"HEAD" (nullptr -> "GET")
  const char *headers_json;      // optional JSON object of request headers
  const uint8_t *body;
  size_t body_len;
  uint32_t timeout_ms;           // 0 -> 15000 ms
  size_t max_bytes;              // 0 -> 16 MiB
  bool allow_localhost_http;
  bool follow_redirects;         // default false; validated hop-by-hop redirects (never CURLOPT_FOLLOWLOCATION)
  uint8_t max_redirects;         // 0 -> 5 when follow_redirects == true (clamped to 5)
  ccord_http_url_check_fn allow_url; // optional per-hop URL/host validator
  void *allow_url_ud;
} ccord_http_request_opts;

typedef struct ccord_http_response {
  long status_code;
  uint8_t *body;                 // owned (ccord_free), NUL-terminated when present
  size_t body_len;
  char *headers_json;            // owned (ccord_free), JSON object of response headers
  char final_url[2048];          // populated when one or more redirects were followed
  char error[256];
} ccord_http_response;

bool ccord_http_request(const ccord_http_request_opts *req, ccord_http_response *out);
void ccord_http_response_free(ccord_http_response *r);

// Streaming HTTP(S) download directly to disk via sibling temp file + atomic rename.
// Only writes dest_path on 2xx; non-2xx responses unlink the temp file and return true with bytes_written == 0.
typedef struct ccord_http_download_opts {
  const char *url;
  const char *dest_path;
  const char *headers_json;      // optional JSON object of request headers
  uint32_t timeout_ms;           // 0 -> 30000 ms
  size_t max_bytes;              // 0 -> 64 MiB
  bool allow_localhost_http;
  bool follow_redirects;         // default false; validated hop-by-hop redirects
  uint8_t max_redirects;         // 0 -> 5 when follow_redirects == true (clamped to 5)
  ccord_http_url_check_fn allow_url; // optional per-hop URL/host validator
  void *allow_url_ud;
} ccord_http_download_opts;

typedef struct ccord_http_download_resp {
  long status_code;
  size_t bytes_written;          // 0 when status_code is non-2xx
  char content_type[128];        // lowercased Content-Type header if present
  char final_url[2048];          // populated when one or more redirects were followed
  char error[256];
} ccord_http_download_resp;

bool ccord_http_download(const ccord_http_download_opts *req, ccord_http_download_resp *out);

// External WebSocket client (strict WSS TLS 1.2/1.3, or ws:// on loopback only).
typedef struct ccord_ws_client ccord_ws_client;
ccord_ws_client *ccord_ws_client_open(const char *url, const char *headers_json, char *err_buf, size_t err_cap);
bool ccord_ws_client_send(ccord_ws_client *ws, const uint8_t *data, size_t len, bool binary);
// Returns >0 bytes received, 0 on timeout or wake_fd signaled, -1 on close/error.
int ccord_ws_client_recv(ccord_ws_client *ws, uint8_t *buf, size_t cap, bool *out_binary, int timeout_ms);
int ccord_ws_client_recv_wake(ccord_ws_client *ws, uint8_t *buf, size_t cap, bool *out_binary,
                              int wake_fd, int timeout_ms);
void ccord_ws_client_close(ccord_ws_client *ws, uint16_t code, const char *reason);
