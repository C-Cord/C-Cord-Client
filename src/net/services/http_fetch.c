// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/net/http_fetch.h>
#include <ccord/net/transport.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <poll.h>
#include <strings.h>
#endif

#if defined(CCORD_HTTP_FETCH_LIVE)
#include <curl/curl.h>
#include "common/tls_internal.h"
#endif

static void set_err(char *dst, size_t cap, const char *msg)
{
  if (dst && cap) snprintf(dst, cap, "%s", msg ? msg : "http error");
}

static bool host_matches(const char *host, size_t hlen, const char *allowed)
{
  size_t alen = strlen(allowed);
  if (hlen != alen) return false;
  for (size_t i = 0; i < hlen; i++) {
    if (tolower((unsigned char)host[i]) != tolower((unsigned char)allowed[i])) return false;
  }
  return true;
}

static bool extract_strict_https_host(const char *url, char *host_out, size_t host_cap)
{
  if (!url || strncmp(url, "https://", 8) != 0) return false;
  const char *auth = url + 8;
  const char *auth_end = auth;
  while (*auth_end && *auth_end != '/' && *auth_end != '?' && *auth_end != '#') {
    unsigned char c = (unsigned char)*auth_end;
    // Reject userinfo ('@'), backslashes, whitespace, and control chars inside authority.
    if (c == '@' || c == '\\' || c <= 0x20 || c >= 0x7f) return false;
    auth_end++;
  }
  const char *colon = nullptr;
  for (const char *p = auth; p < auth_end; p++) {
    if (*p == ':') {
      if (colon) return false;  // reject multiple colons / IPv6 literals
      colon = p;
    }
  }
  const char *host_end = colon ? colon : auth_end;
  if (colon) {
    // Only allow explicit :443 if a port is specified.
    size_t plen = (size_t)(auth_end - (colon + 1));
    if (plen != 3 || memcmp(colon + 1, "443", 3) != 0) return false;
  }
  size_t hlen = (size_t)(host_end - auth);
  if (hlen == 0 || hlen > 253 || hlen >= host_cap) return false;
  memcpy(host_out, auth, hlen);
  host_out[hlen] = '\0';

#if defined(CCORD_HTTP_FETCH_LIVE)
  // Cross-check with libcurl's own RFC 3986 URL parser so host parsing can never disagree with libcurl.
  CURLU *u = curl_url();
  if (!u) return false;
  bool curl_ok = false;
  if (curl_url_set(u, CURLUPART_URL, url, 0) == CURLUE_OK) {
    char *scheme = nullptr, *user = nullptr, *pass = nullptr, *chost = nullptr, *port = nullptr;
    bool has_scheme = curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK;
    bool has_user = curl_url_get(u, CURLUPART_USER, &user, 0) == CURLUE_OK;
    bool has_pass = curl_url_get(u, CURLUPART_PASSWORD, &pass, 0) == CURLUE_OK;
    bool has_host = curl_url_get(u, CURLUPART_HOST, &chost, 0) == CURLUE_OK;
    bool has_port = curl_url_get(u, CURLUPART_PORT, &port, 0) == CURLUE_OK;
    if (has_scheme && scheme && host_matches(scheme, strlen(scheme), "https") &&
        !has_user && !has_pass && has_host && chost &&
        (!has_port || (port && strcmp(port, "443") == 0)) &&
        host_matches(chost, strlen(chost), host_out)) {
      curl_ok = true;
    }
    curl_free(scheme);
    curl_free(user);
    curl_free(pass);
    curl_free(chost);
    curl_free(port);
  }
  curl_url_cleanup(u);
  if (!curl_ok) return false;
#endif
  return true;
}

bool ccord_http_url_allowed(const char *url)
{
  char host[256];
  if (!extract_strict_https_host(url, host, sizeof host)) return false;
  size_t hlen = strlen(host);

  static const char *const k_hosts[] = {
    "plugins.c-cord.org",
    "c-cord.org",
    "c-cord.github.io",
    "github.com",
    "api.github.com",
    "raw.githubusercontent.com",
    "objects.githubusercontent.com",
    "release-assets.githubusercontent.com",
  };
  for (size_t i = 0; i < sizeof k_hosts / sizeof k_hosts[0]; i++) {
    if (host_matches(host, hlen, k_hosts[i])) return true;
  }
  // Allow an explicit user/developer CCORD_CATALOG_URL host override if HTTPS.
  const char *override = getenv("CCORD_CATALOG_URL");
  char ohost[256];
  if (override && extract_strict_https_host(override, ohost, sizeof ohost)) {
    if (host_matches(host, hlen, ohost)) return true;
  }
  return false;
}

void ccord_http_get_result_free(ccord_http_get_result *r)
{
  if (!r) return;
  ccord_free(r->body);
  r->body = nullptr;
  r->body_len = 0;
}

void ccord_http_response_free(ccord_http_response *r)
{
  if (!r) return;
  ccord_free(r->body);
  ccord_free(r->headers_json);
  r->body = nullptr;
  r->headers_json = nullptr;
  r->body_len = 0;
}

#if defined(CCORD_HTTP_FETCH_LIVE)

static bool url_is_loopback_host(const char *host)
{
  if (!host) return false;
  return strcasecmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0 ||
         strcmp(host, "::1") == 0 || strcmp(host, "[::1]") == 0;
}

static bool parse_general_url(const char *url, bool allow_plain_loopback, bool ws_mode, bool *out_plain)
{
  if (out_plain) *out_plain = false;
  if (!url || !url[0]) return false;
  bool plain = false;
  const char *auth = nullptr;
  if (ws_mode) {
    if (strncmp(url, "wss://", 6) == 0) auth = url + 6;
    else if (allow_plain_loopback && strncmp(url, "ws://", 5) == 0) { auth = url + 5; plain = true; }
    else return false;
  } else {
    if (strncmp(url, "https://", 8) == 0) auth = url + 8;
    else if (allow_plain_loopback && strncmp(url, "http://", 7) == 0) { auth = url + 7; plain = true; }
    else return false;
  }
  const char *end = auth;
  while (*end && *end != '/' && *end != '?' && *end != '#') {
    unsigned char c = (unsigned char)*end;
    if (c == '@' || c == '\\' || c <= 0x20 || c >= 0x7f) return false;
    end++;
  }
  const char *hend = end;
  if (*auth == '[') {
    const char *rb = memchr(auth, ']', (size_t)(end - auth));
    if (!rb) return false;
    hend = rb + 1;
  } else {
    const char *col = memchr(auth, ':', (size_t)(end - auth));
    if (col) hend = col;
  }
  size_t hlen = (size_t)(hend - auth);
  if (hlen == 0 || hlen > 253) return false;
  char host[256];
  memcpy(host, auth, hlen);
  host[hlen] = '\0';
  if (plain && !url_is_loopback_host(host)) return false;
  if (out_plain) *out_plain = plain;
  return true;
}

typedef struct mem_sink {
  char *data;
  size_t len;
  size_t cap;
  size_t max_bytes;
  bool overflow;
} mem_sink;

static size_t write_mem_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
  mem_sink *s = ud;
  if (size != 0 && nmemb > SIZE_MAX / size) { s->overflow = true; return 0; }
  size_t n = size * nmemb;
  if (n == 0) return 0;
  if (n > s->max_bytes || s->len > s->max_bytes - n) { s->overflow = true; return 0; }
  if (s->len + n + 1 > s->cap) {
    size_t ncap = s->cap ? s->cap * 2u : 4096u;
    while (ncap < s->len + n + 1) {
      if (ncap > SIZE_MAX / 2u) { ncap = s->len + n + 1; break; }
      ncap *= 2u;
    }
    if (s->max_bytes < SIZE_MAX && ncap > s->max_bytes + 1u) ncap = s->max_bytes + 1u;
    char *nb = ccord_realloc(s->data, ncap);
    if (!nb) { s->overflow = true; return 0; }
    s->data = nb;
    s->cap = ncap;
  }
  memcpy(s->data + s->len, ptr, n);
  s->len += n;
  s->data[s->len] = '\0';
  return n;
}

typedef struct hdr_sink {
  char etag[128];
} hdr_sink;

static size_t header_cb(char *buf, size_t size, size_t nitems, void *ud)
{
  hdr_sink *h = ud;
  if (size != 0 && nitems > SIZE_MAX / size) return 0;
  size_t n = size * nitems;
  if (n > 5 && strncasecmp(buf, "etag:", 5) == 0) {
    const char *v = buf + 5;
    size_t rem = n - 5;
    while (rem && (*v == ' ' || *v == '\t')) { v++; rem--; }
    while (rem && (v[rem - 1] == '\r' || v[rem - 1] == '\n' || v[rem - 1] == ' ')) rem--;
    if (rem > 0 && rem < sizeof h->etag) {
      memcpy(h->etag, v, rem);
      h->etag[rem] = '\0';
    }
  }
  return n;
}

typedef struct file_sink {
  FILE *fp;
  size_t written;
  size_t max_bytes;
  bool overflow;
} file_sink;

static size_t write_file_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
  file_sink *s = ud;
  if (size != 0 && nmemb > SIZE_MAX / size) { s->overflow = true; return 0; }
  size_t n = size * nmemb;
  if (n == 0) return 0;
  if (n > s->max_bytes || s->written > s->max_bytes - n) { s->overflow = true; return 0; }
  size_t w = fwrite(ptr, 1, n, s->fp);
  s->written += w;
  return w;
}

static void apply_common_tls(CURL *curl, const char *url)
{
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "C-Cord-Catalog/1.0");
  // Never auto-follow redirects: each hop is validated against ccord_http_url_allowed() before connecting.
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  ccord_curl_apply_tls(curl);
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
}

static bool is_redirect_status(long code)
{
  return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}

bool ccord_http_get(const char *url, const char *if_none_match, size_t max_bytes, ccord_http_get_result *out)
{
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!ccord_http_url_allowed(url)) {
    set_err(out->error, sizeof out->error, "URL blocked by HTTPS catalog host allowlist");
    return false;
  }
  ccord_curl_global_init_once();

  char cur_url[2048];
  if (snprintf(cur_url, sizeof cur_url, "%s", url) >= (int)sizeof cur_url) {
    set_err(out->error, sizeof out->error, "URL too long");
    return false;
  }

  for (int hop = 0; hop <= 5; hop++) {
    CURL *curl = curl_easy_init();
    if (!curl) {
      set_err(out->error, sizeof out->error, "libcurl initialization failed");
      return false;
    }

    mem_sink ms = {.max_bytes = max_bytes ? max_bytes : (8u * 1024u * 1024u)};
    hdr_sink hs = {0};
    struct curl_slist *hdrs = nullptr;
    if (hop == 0 && if_none_match && if_none_match[0]) {
      char inm[192];
      snprintf(inm, sizeof inm, "If-None-Match: %s", if_none_match);
      hdrs = curl_slist_append(hdrs, inm);
    }
    hdrs = curl_slist_append(hdrs, "Accept: application/json");

    apply_common_tls(curl, cur_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ms);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hs);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    char *redir = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (rc == CURLE_OK && is_redirect_status(code)) {
      curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redir);
    }
    out->status_code = code;
    if (hs.etag[0]) snprintf(out->etag, sizeof out->etag, "%s", hs.etag);

    char next_url[2048] = "";
    bool has_redir = false;
    if (redir && redir[0]) {
      if (snprintf(next_url, sizeof next_url, "%s", redir) < (int)sizeof next_url) has_redir = true;
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
      ccord_free(ms.data);
      if (ms.overflow) set_err(out->error, sizeof out->error, "catalog response exceeded maximum size");
      else snprintf(out->error, sizeof out->error, "network error: %s", curl_easy_strerror(rc));
      return false;
    }
    if (is_redirect_status(code)) {
      ccord_free(ms.data);
      if (hop == 5 || !has_redir || !ccord_http_url_allowed(next_url)) {
        set_err(out->error, sizeof out->error, "redirect target blocked by HTTPS host allowlist");
        return false;
      }
      snprintf(cur_url, sizeof cur_url, "%s", next_url);
      continue;
    }
    if (code == 304) {
      ccord_free(ms.data);
      out->not_modified = true;
      return true;
    }
    if (code < 200 || code >= 300) {
      ccord_free(ms.data);
      snprintf(out->error, sizeof out->error, "catalog server returned HTTP %ld", code);
      return false;
    }
    out->body = ms.data;
    out->body_len = ms.len;
    return true;
  }
  set_err(out->error, sizeof out->error, "too many redirects");
  return false;
}

bool ccord_http_post_json(const char *url, const char *json_body, size_t body_len, size_t max_bytes,
                          ccord_http_get_result *out)
{
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!ccord_http_url_allowed(url)) {
    set_err(out->error, sizeof out->error, "URL blocked by HTTPS catalog host allowlist");
    return false;
  }
  ccord_curl_global_init_once();
  CURL *curl = curl_easy_init();
  if (!curl) {
    set_err(out->error, sizeof out->error, "libcurl initialization failed");
    return false;
  }

  mem_sink ms = {.max_bytes = max_bytes ? max_bytes : (2u * 1024u * 1024u)};
  hdr_sink hs = {0};
  struct curl_slist *hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Accept: application/json");
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

  apply_common_tls(curl, url);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body ? json_body : "{}");
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)(json_body ? body_len : 2u));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ms);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hs);

  CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  out->status_code = code;
  if (hs.etag[0]) snprintf(out->etag, sizeof out->etag, "%s", hs.etag);

  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    ccord_free(ms.data);
    if (ms.overflow) set_err(out->error, sizeof out->error, "catalog response exceeded maximum size");
    else snprintf(out->error, sizeof out->error, "network error: %s", curl_easy_strerror(rc));
    return false;
  }
  if (code < 200 || code >= 300) {
    ccord_free(ms.data);
    snprintf(out->error, sizeof out->error, "catalog server returned HTTP %ld", code);
    return false;
  }
  out->body = ms.data;
  out->body_len = ms.len;
  return true;
}

bool ccord_http_download_file(const char *url, const char *dest_path, size_t max_bytes, char *err_buf, size_t err_cap)
{
  if (!dest_path || !dest_path[0]) {
    set_err(err_buf, err_cap, "invalid destination path");
    return false;
  }
  if (!ccord_http_url_allowed(url)) {
    set_err(err_buf, err_cap, "download URL blocked by HTTPS host allowlist");
    return false;
  }
  ccord_curl_global_init_once();

  char cur_url[2048];
  if (snprintf(cur_url, sizeof cur_url, "%s", url) >= (int)sizeof cur_url) {
    set_err(err_buf, err_cap, "download URL too long");
    return false;
  }

  for (int hop = 0; hop <= 5; hop++) {
    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
      set_err(err_buf, err_cap, "cannot create temporary download file");
      return false;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
      fclose(fp);
      remove(dest_path);
      set_err(err_buf, err_cap, "libcurl initialization failed");
      return false;
    }

    file_sink fs = {.fp = fp, .max_bytes = max_bytes ? max_bytes : (25u * 1024u * 1024u)};
    apply_common_tls(curl, cur_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fs);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    char *redir = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (rc == CURLE_OK && is_redirect_status(code)) {
      curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redir);
    }

    char next_url[2048] = "";
    bool has_redir = false;
    if (redir && redir[0]) {
      if (snprintf(next_url, sizeof next_url, "%s", redir) < (int)sizeof next_url) has_redir = true;
    }

    fclose(fp);
    curl_easy_cleanup(curl);

    if (rc == CURLE_OK && is_redirect_status(code)) {
      remove(dest_path);
      if (hop == 5 || !has_redir || !ccord_http_url_allowed(next_url)) {
        set_err(err_buf, err_cap, "redirect target blocked by HTTPS host allowlist");
        return false;
      }
      snprintf(cur_url, sizeof cur_url, "%s", next_url);
      continue;
    }
    if (rc != CURLE_OK || code < 200 || code >= 300) {
      remove(dest_path);
      if (fs.overflow) set_err(err_buf, err_cap, "downloaded .cce package exceeds 25 MiB limit");
      else if (rc != CURLE_OK) snprintf(err_buf, err_cap, "download failed: %s", curl_easy_strerror(rc));
      else snprintf(err_buf, err_cap, "download failed: HTTP %ld", code);
      return false;
    }
    return true;
  }
  remove(dest_path);
  set_err(err_buf, err_cap, "too many redirects");
  return false;
}

typedef struct resp_hdr_sink {
  ccord_buf buf;
  ccord_jw jw;
  bool started;
} resp_hdr_sink;

static size_t resp_hdr_cb(char *buf, size_t size, size_t nitems, void *ud)
{
  resp_hdr_sink *h = ud;
  if (size != 0 && nitems > SIZE_MAX / size) return 0;
  size_t n = size * nitems;
  const char *col = memchr(buf, ':', n);
  if (!col || col == buf) return n;
  size_t klen = (size_t)(col - buf);
  if (klen >= 128) return n;
  char key[128];
  for (size_t i = 0; i < klen; i++) key[i] = (char)tolower((unsigned char)buf[i]);
  key[klen] = '\0';
  const char *v = col + 1;
  size_t vlen = n - klen - 1;
  while (vlen && (*v == ' ' || *v == '\t')) { v++; vlen--; }
  while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' || v[vlen - 1] == ' ')) vlen--;
  if (!h->started) {
    ccord_jw_init(&h->jw, &h->buf);
    ccord_jw_begin_object(&h->jw);
    h->started = true;
  }
  ccord_jw_key(&h->jw, key);
  ccord_jw_string(&h->jw, v, vlen);
  return n;
}

static struct curl_slist *append_json_headers(struct curl_slist *hdrs, const char *headers_json)
{
  if (!headers_json || !headers_json[0]) return hdrs;
  ccord_arena arena = {0};
  ccord_arena_init(&arena, 2048);
  ccord_error err = {0};
  const ccord_json *obj = ccord_json_parse(&arena, headers_json, strlen(headers_json), nullptr, &err);
  if (obj && ccord_json_type_of(obj) == CCORD_JSON_OBJECT) {
    size_t n = ccord_json_object_len(obj);
    for (size_t i = 0; i < n; i++) {
      const ccord_json *jv = nullptr;
      const char *k = ccord_json_object_key(obj, i, &jv);
      const char *v = ccord_json_string(jv, nullptr);
      if (!k || !k[0] || !v) continue;
      // Reject CR/LF header injection
      if (strpbrk(k, "\r\n:") || strpbrk(v, "\r\n")) continue;
      char line[1024];
      if (snprintf(line, sizeof line, "%s: %s", k, v) < (int)sizeof line) {
        hdrs = curl_slist_append(hdrs, line);
      }
    }
  }
  ccord_arena_free(&arena);
  return hdrs;
}

bool ccord_http_request(const ccord_http_request_opts *req, ccord_http_response *out)
{
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!req || !req->url || !req->url[0]) {
    set_err(out->error, sizeof out->error, "missing URL");
    return false;
  }
  bool plain = false;
  if (!parse_general_url(req->url, req->allow_localhost_http, false, &plain)) {
    set_err(out->error, sizeof out->error, "URL must use HTTPS (plain HTTP allowed only for localhost)");
    return false;
  }
  ccord_curl_global_init_once();
  CURL *curl = curl_easy_init();
  if (!curl) {
    set_err(out->error, sizeof out->error, "libcurl initialization failed");
    return false;
  }

  mem_sink ms = {.max_bytes = req->max_bytes ? req->max_bytes : (16u * 1024u * 1024u)};
  resp_hdr_sink hs = {0};
  struct curl_slist *hdrs = append_json_headers(nullptr, req->headers_json);

  curl_easy_setopt(curl, CURLOPT_URL, req->url);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "C-Cord-Plugin/3.0");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  long timeout_ms = req->timeout_ms ? (long)req->timeout_ms : 15000L;
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms < 10000L ? timeout_ms : 10000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  if (!plain) {
    ccord_curl_apply_tls(curl);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  } else {
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http");
  }

  const char *meth = (req->method && req->method[0]) ? req->method : "GET";
  if (strcasecmp(meth, "HEAD") == 0) {
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  } else if (strcasecmp(meth, "GET") != 0) {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, meth);
  }
  if (req->body && req->body_len > 0) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body_len);
  }
  if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ms);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, resp_hdr_cb);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hs);

  CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  out->status_code = code;

  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    ccord_free(ms.data);
    ccord_buf_free(&hs.buf);
    if (ms.overflow) set_err(out->error, sizeof out->error, "response exceeded maximum size");
    else snprintf(out->error, sizeof out->error, "network error: %s", curl_easy_strerror(rc));
    return false;
  }
  if (hs.started) {
    ccord_jw_end_object(&hs.jw);
    if (hs.buf.data && ccord_jw_ok(&hs.jw)) {
      out->headers_json = ccord_strdup((const char *)hs.buf.data);
    }
    ccord_buf_free(&hs.buf);
  } else {
    out->headers_json = ccord_strdup("{}");
  }
  out->body = (uint8_t *)ms.data;
  out->body_len = ms.len;
  return true;
}

struct ccord_ws_client {
  CURL *curl;
};

ccord_ws_client *ccord_ws_client_open(const char *url, const char *headers_json, char *err_buf, size_t err_cap)
{
#if defined(CURLWS_TEXT)
  bool plain = false;
  if (!parse_general_url(url, true, true, &plain)) {
    set_err(err_buf, err_cap, "WebSocket URL must use wss:// (ws:// allowed only for localhost)");
    return nullptr;
  }
  ccord_curl_global_init_once();
  CURL *curl = curl_easy_init();
  if (!curl) {
    set_err(err_buf, err_cap, "libcurl initialization failed");
    return nullptr;
  }
  struct curl_slist *hdrs = append_json_headers(nullptr, headers_json);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (!plain) {
    ccord_curl_apply_tls(curl);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "wss");
  } else {
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "ws");
  }
  if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

  CURLcode rc = curl_easy_perform(curl);
  curl_slist_free_all(hdrs);
  if (rc != CURLE_OK) {
    snprintf(err_buf, err_cap, "ws connect failed: %s", curl_easy_strerror(rc));
    curl_easy_cleanup(curl);
    return nullptr;
  }
  ccord_ws_client *ws = ccord_calloc(1, sizeof *ws);
  if (!ws) {
    curl_easy_cleanup(curl);
    return nullptr;
  }
  ws->curl = curl;
  return ws;
#else
  (void)url; (void)headers_json;
  set_err(err_buf, err_cap, "libcurl built without WebSocket support");
  return nullptr;
#endif
}

bool ccord_ws_client_send(ccord_ws_client *ws, const uint8_t *data, size_t len, bool binary)
{
#if defined(CURLWS_TEXT)
  if (!ws || !ws->curl) return false;
  size_t sent = 0;
  CURLcode rc = curl_ws_send(ws->curl, data ? data : (const uint8_t *)"", len, &sent, 0,
                             binary ? CURLWS_BINARY : CURLWS_TEXT);
  return rc == CURLE_OK;
#else
  (void)ws; (void)data; (void)len; (void)binary;
  return false;
#endif
}

int ccord_ws_client_recv(ccord_ws_client *ws, uint8_t *buf, size_t cap, bool *out_binary, int timeout_ms)
{
#if defined(CURLWS_TEXT)
  if (!ws || !ws->curl || !buf || cap == 0) return -1;
#if !defined(_WIN32)
  if (timeout_ms > 0) {
    curl_socket_t sock = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(ws->curl, CURLINFO_ACTIVESOCKET, &sock) == CURLE_OK && sock != CURL_SOCKET_BAD) {
      struct pollfd pfd = {.fd = sock, .events = POLLIN};
      int pr = poll(&pfd, 1, timeout_ms);
      if (pr == 0) return 0;
      if (pr < 0) return 0;
    }
  }
#else
  (void)timeout_ms;
#endif
  size_t rlen = 0;
  const struct curl_ws_frame *meta = nullptr;
  CURLcode rc = curl_ws_recv(ws->curl, buf, cap, &rlen, &meta);
  if (rc == CURLE_AGAIN) return 0;
  if (rc != CURLE_OK) return -1;
  if (meta && (meta->flags & CURLWS_CLOSE)) return -1;
  if (out_binary) *out_binary = meta && (meta->flags & CURLWS_BINARY) != 0;
  return (int)rlen;
#else
  (void)ws; (void)buf; (void)cap; (void)out_binary; (void)timeout_ms;
  return -1;
#endif
}

void ccord_ws_client_close(ccord_ws_client *ws, uint16_t code, const char *reason)
{
  if (!ws) return;
#if defined(CURLWS_TEXT)
  if (ws->curl) {
    uint8_t payload[128];
    payload[0] = (uint8_t)(code >> 8);
    payload[1] = (uint8_t)(code & 0xFFu);
    size_t rlen = reason ? strlen(reason) : 0u;
    if (rlen > sizeof payload - 2u) rlen = sizeof payload - 2u;
    if (rlen > 0) memcpy(payload + 2, reason, rlen);
    size_t sent = 0;
    (void)curl_ws_send(ws->curl, payload, 2u + rlen, &sent, 0, CURLWS_CLOSE);
    curl_easy_cleanup(ws->curl);
  }
#else
  (void)code; (void)reason;
#endif
  ccord_free(ws);
}

#else

bool ccord_http_get(const char *url, const char *if_none_match, size_t max_bytes, ccord_http_get_result *out)
{
  (void)url; (void)if_none_match; (void)max_bytes;
  if (!out) return false;
  memset(out, 0, sizeof *out);
  set_err(out->error, sizeof out->error, "offline build: network catalog unavailable");
  return false;
}

bool ccord_http_post_json(const char *url, const char *json_body, size_t body_len, size_t max_bytes,
                          ccord_http_get_result *out)
{
  (void)url; (void)json_body; (void)body_len; (void)max_bytes;
  if (!out) return false;
  memset(out, 0, sizeof *out);
  set_err(out->error, sizeof out->error, "offline build: network catalog unavailable");
  return false;
}

bool ccord_http_download_file(const char *url, const char *dest_path, size_t max_bytes, char *err_buf, size_t err_cap)
{
  (void)url; (void)dest_path; (void)max_bytes;
  set_err(err_buf, err_cap, "offline build: network download unavailable");
  return false;
}

bool ccord_http_request(const ccord_http_request_opts *req, ccord_http_response *out)
{
  (void)req;
  if (!out) return false;
  memset(out, 0, sizeof *out);
  set_err(out->error, sizeof out->error, "offline build: HTTP client unavailable");
  return false;
}

ccord_ws_client *ccord_ws_client_open(const char *url, const char *headers_json, char *err_buf, size_t err_cap)
{
  (void)url; (void)headers_json;
  set_err(err_buf, err_cap, "offline build: WebSocket client unavailable");
  return nullptr;
}

bool ccord_ws_client_send(ccord_ws_client *ws, const uint8_t *data, size_t len, bool binary)
{
  (void)ws; (void)data; (void)len; (void)binary;
  return false;
}

int ccord_ws_client_recv(ccord_ws_client *ws, uint8_t *buf, size_t cap, bool *out_binary, int timeout_ms)
{
  (void)ws; (void)buf; (void)cap; (void)out_binary; (void)timeout_ms;
  return -1;
}

void ccord_ws_client_close(ccord_ws_client *ws, uint16_t code, const char *reason)
{
  (void)ws; (void)code; (void)reason;
}

#endif
