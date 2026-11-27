// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/net/http_fetch.h>
#include <ccord/net/transport.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>
#include <ccord/core/random.h>

#include <ctype.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#endif
#else
#include <io.h>
#include <windows.h>
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

static bool is_all_ipv4_chars(const char *s)
{
  if (!s || !s[0]) return false;
  for (const char *p = s; *p; p++) {
    if (!((*p >= '0' && *p <= '9') || *p == '.')) return false;
  }
  return true;
}

static bool url_is_loopback_host(const char *host)
{
  if (!host || !host[0]) return false;
  size_t raw_len = strlen(host);
  if (raw_len >= 256u) return false;
  size_t start = 0;
  size_t end = raw_len;
  if (raw_len >= 2u && host[0] == '[' && host[raw_len - 1u] == ']') {
    start = 1u;
    end = raw_len - 1u;
  }
  size_t len = end - start;
  if (len == 0) return false;
  char h[256];
  for (size_t i = 0; i < len; i++) {
    h[i] = (char)tolower((unsigned char)host[start + i]);
  }
  if (len > 1u && h[len - 1u] == '.') len--;
  h[len] = '\0';

  if (strcmp(h, "localhost") == 0) return true;
  if (len > 10u && strcmp(h + len - 10u, ".localhost") == 0) return true;
  if (strcmp(h, "::1") == 0 || strcmp(h, "::") == 0) return true;
  if (strcmp(h, "0.0.0.0") == 0 || strcmp(h, "0") == 0) return true;
  if (strncmp(h, "127.", 4) == 0 && is_all_ipv4_chars(h)) return true;
  if (strncmp(h, "0.", 2) == 0 && is_all_ipv4_chars(h)) return true;
  if (strncmp(h, "::ffff:127.", 11) == 0 || strncmp(h, "::ffff:0.", 9) == 0) return true;

#if !defined(_WIN32)
  struct in_addr a4;
  if (inet_pton(AF_INET, h, &a4) == 1) {
    uint32_t ip = ntohl(a4.s_addr);
    if ((ip & 0xFF000000u) == 0x7F000000u || (ip & 0xFF000000u) == 0x00000000u) return true;
  }
  struct in6_addr a6;
  if (inet_pton(AF_INET6, h, &a6) == 1) {
    const uint8_t *b = a6.s6_addr;
    bool z12 = true;
    for (int i = 0; i < 12; i++) {
      if (b[i] != 0) { z12 = false; break; }
    }
    if (z12 && b[12] == 0 && b[13] == 0 && b[14] == 0 && (b[15] == 0 || b[15] == 1)) return true;
    bool v4m = true;
    for (int i = 0; i < 10; i++) {
      if (b[i] != 0) { v4m = false; break; }
    }
    if (v4m && b[10] == 0xff && b[11] == 0xff) {
      if (b[12] == 127 || b[12] == 0) return true;
    }
  }
#endif
  return false;
}

static bool parse_general_url(const char *url, bool allow_plain_loopback, bool ws_mode,
                              bool *out_plain, bool *out_is_loopback)
{
  if (out_plain) *out_plain = false;
  if (out_is_loopback) *out_is_loopback = false;
  if (!url || !url[0]) return false;
  for (const unsigned char *q = (const unsigned char *)url; *q; q++) {
    if (*q < 0x20u || *q == 0x7fu) return false;
  }
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
  bool is_lb = url_is_loopback_host(host);
  if (plain && !is_lb) return false;
  if (out_plain) *out_plain = plain;
  if (out_is_loopback) *out_is_loopback = is_lb;
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

static struct curl_slist *append_json_headers(struct curl_slist *hdrs, const char *headers_json, bool strip_creds)
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
      if (strip_creds &&
          (ccord_strcasecmp(k, "authorization") == 0 ||
           ccord_strcasecmp(k, "proxy-authorization") == 0 ||
           ccord_strcasecmp(k, "cookie") == 0)) {
        continue;
      }
      char line[1024];
      if (snprintf(line, sizeof line, "%s: %s", k, v) < (int)sizeof line) {
        hdrs = curl_slist_append(hdrs, line);
      }
    }
  }
  ccord_arena_free(&arena);
  return hdrs;
}

static bool url_extract_origin(const char *url, char *scheme_out, size_t scap, char *host_out, size_t hcap, int *port_out)
{
  if (!url || !scheme_out || !host_out || !port_out) return false;
  const char *auth = nullptr;
  int def_port = 443;
  if (strncmp(url, "https://", 8) == 0) {
    if (scap < 6u) return false;
    memcpy(scheme_out, "https", 6);
    auth = url + 8;
    def_port = 443;
  } else if (strncmp(url, "http://", 7) == 0) {
    if (scap < 5u) return false;
    memcpy(scheme_out, "http", 5);
    auth = url + 7;
    def_port = 80;
  } else {
    return false;
  }
  const char *end = auth;
  while (*end && *end != '/' && *end != '?' && *end != '#') end++;
  const char *hs = auth, *he = end, *pstart = nullptr;
  if (hs < end && *hs == '[') {
    const char *rb = memchr(hs, ']', (size_t)(end - hs));
    if (!rb) return false;
    hs++;
    he = rb;
    if (rb + 1 < end && rb[1] == ':') pstart = rb + 2;
  } else {
    const char *col = memchr(hs, ':', (size_t)(end - hs));
    if (col) {
      he = col;
      pstart = col + 1;
    }
  }
  size_t hl = (size_t)(he - hs);
  if (hl == 0 || hl >= hcap) return false;
  for (size_t i = 0; i < hl; i++) host_out[i] = (char)tolower((unsigned char)hs[i]);
  host_out[hl] = '\0';
  int port = def_port;
  if (pstart && pstart < end) {
    unsigned long pv = 0;
    for (const char *q = pstart; q < end; q++) {
      if (*q < '0' || *q > '9') return false;
      pv = pv * 10ul + (unsigned long)(*q - '0');
      if (pv > 65535ul) return false;
    }
    if (pv == 0) return false;
    port = (int)pv;
  }
  *port_out = port;
  return true;
}

static bool urls_same_origin(const char *a, const char *b)
{
  char sa[16], ha[256], sb[16], hb[256];
  int pa = 0, pb = 0;
  if (!url_extract_origin(a, sa, sizeof sa, ha, sizeof ha, &pa)) return false;
  if (!url_extract_origin(b, sb, sizeof sb, hb, sizeof hb, &pb)) return false;
  return strcmp(sa, sb) == 0 && strcmp(ha, hb) == 0 && pa == pb;
}

static bool validate_redirect_hop(bool initial_https, bool initial_loopback, bool allow_localhost_http,
                                  ccord_http_url_check_fn allow_url, void *allow_url_ud, const char *next_url,
                                  bool *out_plain, bool *out_loopback, char *err_buf, size_t err_cap)
{
  bool next_plain = false, next_loopback = false;
  if (!parse_general_url(next_url, allow_localhost_http, false, &next_plain, &next_loopback)) {
    set_err(err_buf, err_cap, "redirect target blocked: invalid URL or disallowed scheme");
    return false;
  }
  if (initial_https && next_plain) {
    set_err(err_buf, err_cap, "redirect blocked: HTTPS to HTTP downgrade");
    return false;
  }
  if (!initial_loopback && next_loopback) {
    set_err(err_buf, err_cap, "redirect blocked: public request cannot redirect to loopback");
    return false;
  }
  if (initial_loopback && !next_loopback) {
    set_err(err_buf, err_cap, "redirect blocked: loopback request cannot redirect to external host");
    return false;
  }
  if (allow_url && !allow_url(allow_url_ud, next_url, err_buf, err_cap)) {
    if (err_buf && err_cap && !err_buf[0]) {
      set_err(err_buf, err_cap, "redirect target blocked by host policy");
    }
    return false;
  }
  if (out_plain) *out_plain = next_plain;
  if (out_loopback) *out_loopback = next_loopback;
  return true;
}

bool ccord_http_request(const ccord_http_request_opts *req, ccord_http_response *out)
{
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!req || !req->url || !req->url[0]) {
    set_err(out->error, sizeof out->error, "missing URL");
    return false;
  }
  bool cur_plain = false;
  bool cur_loopback = false;
  if (!parse_general_url(req->url, req->allow_localhost_http, false, &cur_plain, &cur_loopback)) {
    set_err(out->error, sizeof out->error, "URL must use HTTPS (plain HTTP allowed only for localhost)");
    return false;
  }
  const bool initial_https = !cur_plain;
  const bool initial_loopback = cur_loopback;

  const char *meth = (req->method && req->method[0]) ? req->method : "GET";
  {
    size_t mlen = 0;
    for (const char *mp = meth; *mp; mp++) {
      if (!((*mp >= 'A' && *mp <= 'Z') || (*mp >= 'a' && *mp <= 'z')) || ++mlen > 15u) {
        set_err(out->error, sizeof out->error, "invalid HTTP method");
        return false;
      }
    }
  }
  char cur_url[2048];
  if (snprintf(cur_url, sizeof cur_url, "%s", req->url) >= (int)sizeof cur_url) {
    set_err(out->error, sizeof out->error, "URL too long");
    return false;
  }
  if (req->allow_url && !req->allow_url(req->allow_url_ud, cur_url, out->error, sizeof out->error)) {
    if (!out->error[0]) set_err(out->error, sizeof out->error, "URL blocked by host policy");
    return false;
  }
  char cur_method[16];
  snprintf(cur_method, sizeof cur_method, "%s", meth);
  const uint8_t *cur_body = req->body;
  size_t cur_body_len = req->body_len;
  bool strip_creds = false;

  uint8_t max_hops = 0;
  if (req->follow_redirects) {
    max_hops = req->max_redirects ? (req->max_redirects > 5u ? 5u : req->max_redirects) : 5u;
  }

  ccord_curl_global_init_once();

  for (uint8_t hop = 0; hop <= max_hops; hop++) {
    CURL *curl = curl_easy_init();
    if (!curl) {
      set_err(out->error, sizeof out->error, "libcurl initialization failed");
      return false;
    }

    mem_sink ms = {.max_bytes = req->max_bytes ? req->max_bytes : (16u * 1024u * 1024u)};
    resp_hdr_sink hs = {0};
    struct curl_slist *hdrs = append_json_headers(nullptr, req->headers_json, strip_creds);

    curl_easy_setopt(curl, CURLOPT_URL, cur_url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "C-Cord-Plugin/3.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    long timeout_ms = req->timeout_ms ? (long)req->timeout_ms : 15000L;
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms < 10000L ? timeout_ms : 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (!cur_plain) {
      ccord_curl_apply_tls(curl);
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
      curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
      if (!req->allow_localhost_http || !cur_loopback) {
        ccord_curl_apply_public_opensocket(curl);
      }
    } else {
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http");
    }

    if (strcasecmp(cur_method, "HEAD") == 0) {
      curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (strcasecmp(cur_method, "GET") != 0) {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, cur_method);
    }
    if (cur_body && cur_body_len > 0) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, cur_body);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)cur_body_len);
    }
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ms);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, resp_hdr_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hs);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    char *redir = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (rc == CURLE_OK && req->follow_redirects && is_redirect_status(code)) {
      curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redir);
    }
    char next_url[2048] = "";
    bool has_redir = false;
    if (redir && redir[0]) {
      if (snprintf(next_url, sizeof next_url, "%s", redir) < (int)sizeof next_url) has_redir = true;
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
      ccord_free(ms.data);
      ccord_buf_free(&hs.buf);
      if (ms.overflow) set_err(out->error, sizeof out->error, "response exceeded maximum size");
      else snprintf(out->error, sizeof out->error, "network error: %s", curl_easy_strerror(rc));
      return false;
    }

    if (req->follow_redirects && is_redirect_status(code) && has_redir) {
      ccord_free(ms.data);
      ccord_buf_free(&hs.buf);
      if (hop == max_hops) {
        set_err(out->error, sizeof out->error, "too many redirects");
        return false;
      }
      if (!validate_redirect_hop(initial_https, initial_loopback, req->allow_localhost_http,
                                 req->allow_url, req->allow_url_ud, next_url,
                                 &cur_plain, &cur_loopback, out->error, sizeof out->error)) {
        return false;
      }
      if (!urls_same_origin(cur_url, next_url)) strip_creds = true;
      if (code == 303 || ((code == 301 || code == 302) && strcasecmp(cur_method, "POST") == 0)) {
        snprintf(cur_method, sizeof cur_method, "GET");
        cur_body = nullptr;
        cur_body_len = 0;
      }
      snprintf(cur_url, sizeof cur_url, "%s", next_url);
      continue;
    }

    out->status_code = code;
    if (hop > 0) snprintf(out->final_url, sizeof out->final_url, "%s", cur_url);
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

  set_err(out->error, sizeof out->error, "too many redirects");
  return false;
}

// ---------------------------------------------------------------- Streaming download (atomic temp file + rename)

typedef struct dl_hdr_sink {
  char content_type[128];
} dl_hdr_sink;

static size_t dl_hdr_cb(char *buf, size_t size, size_t nitems, void *ud)
{
  dl_hdr_sink *h = ud;
  if (size != 0 && nitems > SIZE_MAX / size) return 0;
  size_t n = size * nitems;
  if (n > 13u && strncasecmp(buf, "content-type:", 13) == 0) {
    const char *v = buf + 13;
    size_t rem = n - 13u;
    while (rem && (*v == ' ' || *v == '\t')) { v++; rem--; }
    while (rem && (v[rem - 1] == '\r' || v[rem - 1] == '\n' || v[rem - 1] == ' ' || v[rem - 1] == '\t')) rem--;
    if (rem >= sizeof h->content_type) rem = sizeof h->content_type - 1u;
    for (size_t i = 0; i < rem; i++) h->content_type[i] = (char)tolower((unsigned char)v[i]);
    h->content_type[rem] = '\0';
  }
  return n;
}

static bool dl_path_is_symlink(const char *path)
{
  if (!path || !path[0]) return false;
#if !defined(_WIN32)
  struct stat st;
  return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
#else
  int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
  if (wlen <= 0 || wlen > 4096) return false;
  wchar_t wbuf[4096];
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wbuf, wlen) <= 0) return false;
  DWORD attr = GetFileAttributesW(wbuf);
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#endif
}

static void dl_unlink_file(const char *path)
{
  if (!path || !path[0]) return;
#if !defined(_WIN32)
  (void)unlink(path);
#else
  int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
  if (wlen > 0 && wlen <= 4096) {
    wchar_t wbuf[4096];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wbuf, wlen) > 0) (void)DeleteFileW(wbuf);
  }
#endif
}

static bool dl_open_sibling_tmp(const char *dest_path, char *tmp_path, size_t tmp_cap, FILE **out_fp,
                                char *err_buf, size_t err_cap)
{
  *out_fp = nullptr;
  tmp_path[0] = '\0';
  const char *slash = strrchr(dest_path, '/');
#if defined(_WIN32)
  const char *bslash = strrchr(dest_path, '\\');
  if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
  size_t dir_len = slash ? (size_t)(slash - dest_path) : 0u;
  static const char k_hex[] = "0123456789abcdef";

  for (int attempt = 0; attempt < 4; attempt++) {
    uint8_t rnd[8] = {0};
    if (ccord_random_bytes(rnd, sizeof rnd) != CCORD_OK) {
      set_err(err_buf, err_cap, "failed to generate temporary filename");
      return false;
    }
    char hex[17];
    for (int i = 0; i < 8; i++) {
      hex[i * 2] = k_hex[(rnd[i] >> 4) & 0x0Fu];
      hex[i * 2 + 1] = k_hex[rnd[i] & 0x0Fu];
    }
    hex[16] = '\0';
    int n = 0;
    if (slash) {
      n = snprintf(tmp_path, tmp_cap, "%.*s/.ccord-dl-%s.tmp", (int)dir_len, dest_path, hex);
    } else {
      n = snprintf(tmp_path, tmp_cap, ".ccord-dl-%s.tmp", hex);
    }
    if (n <= 0 || (size_t)n >= tmp_cap) {
      set_err(err_buf, err_cap, "destination path too long");
      return false;
    }
#if !defined(_WIN32)
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
      if (errno == EEXIST) continue;
      snprintf(err_buf, err_cap, "cannot create download file: %s", strerror(errno));
      tmp_path[0] = '\0';
      return false;
    }
    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
      close(fd);
      (void)unlink(tmp_path);
      tmp_path[0] = '\0';
      set_err(err_buf, err_cap, "fdopen failed");
      return false;
    }
    *out_fp = fp;
    return true;
#else
    int wlen = MultiByteToWideChar(CP_UTF8, 0, tmp_path, -1, nullptr, 0);
    if (wlen <= 0 || wlen > 4096) {
      set_err(err_buf, err_cap, "destination path invalid");
      tmp_path[0] = '\0';
      return false;
    }
    wchar_t wtmp[4096];
    if (MultiByteToWideChar(CP_UTF8, 0, tmp_path, -1, wtmp, wlen) <= 0) {
      set_err(err_buf, err_cap, "destination path invalid");
      tmp_path[0] = '\0';
      return false;
    }
    HANDLE h = CreateFileW(wtmp, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      if (GetLastError() == ERROR_FILE_EXISTS) continue;
      set_err(err_buf, err_cap, "cannot create download file");
      tmp_path[0] = '\0';
      return false;
    }
    int fd = _open_osfhandle((intptr_t)h, 0);
    if (fd < 0) {
      CloseHandle(h);
      (void)DeleteFileW(wtmp);
      tmp_path[0] = '\0';
      set_err(err_buf, err_cap, "failed to open download file descriptor");
      return false;
    }
    FILE *fp = _fdopen(fd, "wb");
    if (!fp) {
      _close(fd);
      (void)DeleteFileW(wtmp);
      tmp_path[0] = '\0';
      set_err(err_buf, err_cap, "fdopen failed");
      return false;
    }
    *out_fp = fp;
    return true;
#endif
  }
  tmp_path[0] = '\0';
  set_err(err_buf, err_cap, "could not allocate unique temporary download file");
  return false;
}

static bool dl_finalize_rename(const char *tmp_path, const char *dest_path, char *err_buf, size_t err_cap)
{
  if (dl_path_is_symlink(dest_path)) {
    dl_unlink_file(tmp_path);
    set_err(err_buf, err_cap, "destination path is a symbolic link");
    return false;
  }
#if !defined(_WIN32)
  if (rename(tmp_path, dest_path) != 0) {
    int e = errno;
    dl_unlink_file(tmp_path);
    snprintf(err_buf, err_cap, "cannot move downloaded file to destination: %s", strerror(e));
    return false;
  }
  return true;
#else
  int wt = MultiByteToWideChar(CP_UTF8, 0, tmp_path, -1, nullptr, 0);
  int wd = MultiByteToWideChar(CP_UTF8, 0, dest_path, -1, nullptr, 0);
  if (wt <= 0 || wt > 4096 || wd <= 0 || wd > 4096) {
    dl_unlink_file(tmp_path);
    set_err(err_buf, err_cap, "destination path invalid");
    return false;
  }
  wchar_t wtmp[4096], wdest[4096];
  MultiByteToWideChar(CP_UTF8, 0, tmp_path, -1, wtmp, wt);
  MultiByteToWideChar(CP_UTF8, 0, dest_path, -1, wdest, wd);
  if (!MoveFileExW(wtmp, wdest, MOVEFILE_REPLACE_EXISTING)) {
    dl_unlink_file(tmp_path);
    set_err(err_buf, err_cap, "cannot move downloaded file to destination");
    return false;
  }
  return true;
#endif
}

bool ccord_http_download(const ccord_http_download_opts *req, ccord_http_download_resp *out)
{
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!req || !req->url || !req->url[0] || !req->dest_path || !req->dest_path[0]) {
    set_err(out->error, sizeof out->error, "missing URL or destination path");
    return false;
  }
  if (dl_path_is_symlink(req->dest_path)) {
    set_err(out->error, sizeof out->error, "destination path is a symbolic link");
    return false;
  }
  bool cur_plain = false;
  bool cur_loopback = false;
  if (!parse_general_url(req->url, req->allow_localhost_http, false, &cur_plain, &cur_loopback)) {
    set_err(out->error, sizeof out->error, "URL must use HTTPS (plain HTTP allowed only for localhost)");
    return false;
  }
  const bool initial_https = !cur_plain;
  const bool initial_loopback = cur_loopback;

  char cur_url[2048];
  if (snprintf(cur_url, sizeof cur_url, "%s", req->url) >= (int)sizeof cur_url) {
    set_err(out->error, sizeof out->error, "URL too long");
    return false;
  }
  if (req->allow_url && !req->allow_url(req->allow_url_ud, cur_url, out->error, sizeof out->error)) {
    if (!out->error[0]) set_err(out->error, sizeof out->error, "URL blocked by host policy");
    return false;
  }

  uint8_t max_hops = 0;
  if (req->follow_redirects) {
    max_hops = req->max_redirects ? (req->max_redirects > 5u ? 5u : req->max_redirects) : 5u;
  }
  size_t max_bytes = req->max_bytes ? req->max_bytes : (64u * 1024u * 1024u);
  bool strip_creds = false;

  ccord_curl_global_init_once();

  for (uint8_t hop = 0; hop <= max_hops; hop++) {
    char tmp_path[CCORD_PATH_MAX + 64];
    FILE *fp = nullptr;
    if (!dl_open_sibling_tmp(req->dest_path, tmp_path, sizeof tmp_path, &fp, out->error, sizeof out->error)) {
      return false;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
      fclose(fp);
      dl_unlink_file(tmp_path);
      set_err(out->error, sizeof out->error, "libcurl initialization failed");
      return false;
    }

    file_sink fs = {.fp = fp, .max_bytes = max_bytes};
    dl_hdr_sink hs = {0};
    struct curl_slist *hdrs = append_json_headers(nullptr, req->headers_json, strip_creds);

    curl_easy_setopt(curl, CURLOPT_URL, cur_url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "C-Cord-Plugin/3.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    long timeout_ms = req->timeout_ms ? (long)req->timeout_ms : 30000L;
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms < 15000L ? timeout_ms : 15000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    // Leave CURLOPT_ACCEPT_ENCODING unset so downloaded media bytes are preserved verbatim unless caller passes Accept-Encoding.
    if (!cur_plain) {
      ccord_curl_apply_tls(curl);
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
      curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
      if (!req->allow_localhost_http || !cur_loopback) {
        ccord_curl_apply_public_opensocket(curl);
      }
    } else {
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http");
    }
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fs);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, dl_hdr_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hs);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    char *redir = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (rc == CURLE_OK && req->follow_redirects && is_redirect_status(code)) {
      curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redir);
    }
    char next_url[2048] = "";
    bool has_redir = false;
    if (redir && redir[0]) {
      if (snprintf(next_url, sizeof next_url, "%s", redir) < (int)sizeof next_url) has_redir = true;
    }

    bool write_ok = (fflush(fp) == 0) && !ferror(fp);
    if (fclose(fp) != 0) write_ok = false;
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
      dl_unlink_file(tmp_path);
      if (fs.overflow) set_err(out->error, sizeof out->error, "download exceeded maximum size");
      else snprintf(out->error, sizeof out->error, "network error: %s", curl_easy_strerror(rc));
      return false;
    }

    if (req->follow_redirects && is_redirect_status(code) && has_redir) {
      dl_unlink_file(tmp_path);
      if (hop == max_hops) {
        set_err(out->error, sizeof out->error, "too many redirects");
        return false;
      }
      if (!validate_redirect_hop(initial_https, initial_loopback, req->allow_localhost_http,
                                 req->allow_url, req->allow_url_ud, next_url,
                                 &cur_plain, &cur_loopback, out->error, sizeof out->error)) {
        return false;
      }
      if (!urls_same_origin(cur_url, next_url)) strip_creds = true;
      snprintf(cur_url, sizeof cur_url, "%s", next_url);
      continue;
    }

    out->status_code = code;
    if (hs.content_type[0]) snprintf(out->content_type, sizeof out->content_type, "%s", hs.content_type);
    if (hop > 0) snprintf(out->final_url, sizeof out->final_url, "%s", cur_url);

    if (code < 200 || code >= 300) {
      dl_unlink_file(tmp_path);
      out->bytes_written = 0;
      return true;
    }
    if (!write_ok) {
      dl_unlink_file(tmp_path);
      set_err(out->error, sizeof out->error, "failed writing downloaded file to disk");
      return false;
    }
    if (!dl_finalize_rename(tmp_path, req->dest_path, out->error, sizeof out->error)) {
      return false;
    }
    out->bytes_written = fs.written;
    return true;
  }

  set_err(out->error, sizeof out->error, "too many redirects");
  return false;
}

struct ccord_ws_client {
  CURL *curl;
#if defined(__linux__) && defined(CLOCK_BOOTTIME)
  int timer_fd;
#endif
};

ccord_ws_client *ccord_ws_client_open(const char *url, const char *headers_json, char *err_buf, size_t err_cap)
{
#if defined(CURLWS_TEXT)
  bool plain = false;
  bool is_loopback = false;
  if (!parse_general_url(url, true, true, &plain, &is_loopback)) {
    set_err(err_buf, err_cap, "WebSocket URL must use wss:// (ws:// allowed only for localhost)");
    return nullptr;
  }
  ccord_curl_global_init_once();
  CURL *curl = curl_easy_init();
  if (!curl) {
    set_err(err_buf, err_cap, "libcurl initialization failed");
    return nullptr;
  }
  struct curl_slist *hdrs = append_json_headers(nullptr, headers_json, false);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
#if defined(CURLWS_NOAUTOPONG)
  // Handle WebSocket PING explicitly in ws_try_recv_frame(): in CONNECT_ONLY=2 mode, libcurl's
  // auto-pong can sit buffered in ws->sendbuf until the next curl_ws_send(), starving server pings.
  curl_easy_setopt(curl, CURLOPT_WS_OPTIONS, (long)CURLWS_NOAUTOPONG);
#endif
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 45L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 10L);
  if (!plain) {
    ccord_curl_apply_tls(curl);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "wss");
    if (!is_loopback) {
      ccord_curl_apply_public_opensocket(curl);
    }
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
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
  curl_socket_t sock = CURL_SOCKET_BAD;
  if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock) == CURLE_OK && sock != CURL_SOCKET_BAD) {
    (void)ccord_sock_set_nonblock((ccord_socket_t)sock);
#if defined(__linux__) && defined(TCP_USER_TIMEOUT)
    unsigned int user_timeout_ms = 15000u;
    (void)setsockopt(sock, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout_ms, sizeof user_timeout_ms);
#endif
  }
  ccord_ws_client *ws = ccord_calloc(1, sizeof *ws);
  if (!ws) {
    curl_easy_cleanup(curl);
    return nullptr;
  }
  ws->curl = curl;
#if defined(__linux__) && defined(CLOCK_BOOTTIME)
  ws->timer_fd = timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK | TFD_CLOEXEC);
#endif
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
  size_t total = 0;
  uint64_t deadline = 0;
  do {
    size_t sent = 0;
    const uint8_t *ptr = data ? (data + total) : (const uint8_t *)"";
    CURLcode rc = curl_ws_send(ws->curl, ptr, len - total, &sent, 0,
                               binary ? CURLWS_BINARY : CURLWS_TEXT);
    if (rc == CURLE_AGAIN) {
      uint64_t now = ccord_clock_ms();
      if (deadline == 0) deadline = now + 500u;
      if (now >= deadline) return false;
      curl_socket_t sock = CURL_SOCKET_BAD;
      if (curl_easy_getinfo(ws->curl, CURLINFO_ACTIVESOCKET, &sock) == CURLE_OK && sock != CURL_SOCKET_BAD) {
        int rev = ccord_sock_poll1((ccord_socket_t)sock, CCORD_POLL_OUT, (int)(deadline - now));
        if (rev <= 0 || (rev & (CCORD_POLL_ERR | CCORD_POLL_HUP)) != 0) return false;
      } else {
        ccord_sleep_ms(10);
      }
      continue;
    }
    if (rc != CURLE_OK) return false;
    if (len > 0 && sent == 0) return false;
    total += sent;
  } while (total < len);
  return true;
#else
  (void)ws; (void)data; (void)len; (void)binary;
  return false;
#endif
}

#if defined(CURLWS_TEXT)
static void ws_send_pong(ccord_ws_client *ws, const uint8_t *data, size_t len)
{
  if (!ws || !ws->curl) return;
  if (len > 125u) len = 125u;   // RFC 6455 control frame payload limit
  size_t total = 0;
  uint64_t deadline = 0;
  do {
    size_t sent = 0;
    const uint8_t *ptr = data ? (data + total) : (const uint8_t *)"";
    CURLcode rc = curl_ws_send(ws->curl, ptr, len - total, &sent, 0, CURLWS_PONG);
    if (rc == CURLE_AGAIN) {
      uint64_t now = ccord_clock_ms();
      if (deadline == 0) deadline = now + 250u;
      if (now >= deadline) return;
      curl_socket_t sock = CURL_SOCKET_BAD;
      if (curl_easy_getinfo(ws->curl, CURLINFO_ACTIVESOCKET, &sock) == CURLE_OK && sock != CURL_SOCKET_BAD) {
        int rev = ccord_sock_poll1((ccord_socket_t)sock, CCORD_POLL_OUT, (int)(deadline - now));
        if (rev <= 0 || (rev & (CCORD_POLL_ERR | CCORD_POLL_HUP)) != 0) return;
      } else {
        ccord_sleep_ms(5);
      }
      continue;
    }
    if (rc != CURLE_OK) return;
    if (len > 0 && sent == 0) return;
    total += sent;
  } while (total < len);
}

// Attempts a non-blocking WebSocket frame read from libcurl/TLS buffers.
// Returns >0 on a complete data frame, 0 when no frame is buffered yet (CURLE_AGAIN),
// or -1 on close frame, EOF, or transport error.
static int ws_try_recv_frame(ccord_ws_client *ws, uint8_t *buf, size_t cap, bool *out_binary)
{
  for (int tries = 0; tries < 16; tries++) {
    size_t rlen = 0;
    const struct curl_ws_frame *meta = nullptr;
    CURLcode rc = curl_ws_recv(ws->curl, buf, cap, &rlen, &meta);
    if (rc == CURLE_AGAIN) {
#if !defined(CURLWS_NOAUTOPONG)
      // Older libcurl (< 8.14) auto-queues PONG on PING without always flushing sendbuf in
      // CONNECT_ONLY=2 mode; trigger ws_flush() safely (no-op if sendbuf is empty).
      size_t flushed = 0;
      (void)curl_ws_send(ws->curl, "", 0, &flushed, 0, 0);
#endif
      return 0;
    }
    if (rc != CURLE_OK) return -1;
    if (!meta) return -1;   // clean TCP EOF without WS CLOSE
    if (meta->flags & CURLWS_CLOSE) return -1;
    if (meta->flags & CURLWS_PING) {
      ws_send_pong(ws, buf, rlen);
      continue;
    }
    if (meta->flags & CURLWS_PONG) continue;
    bool is_bin = (meta->flags & CURLWS_BINARY) != 0;
    size_t total = rlen;
    curl_off_t left = meta->bytesleft;
    bool cont = (meta->flags & CURLWS_CONT) != 0;
    // Drain remaining chunks of the same WS message if fragmented or larger than one read.
    while ((left > 0 || cont) && total < cap) {
      size_t m = 0;
      const struct curl_ws_frame *m2 = nullptr;
      CURLcode rc2 = curl_ws_recv(ws->curl, buf + total, cap - total, &m, &m2);
      if (rc2 == CURLE_AGAIN) {
        curl_socket_t sock = CURL_SOCKET_BAD;
        if (curl_easy_getinfo(ws->curl, CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK || sock == CURL_SOCKET_BAD) break;
        int rev = ccord_sock_poll1((ccord_socket_t)sock, CCORD_POLL_IN, 250);
        if (rev <= 0) break;
        continue;
      }
      if (rc2 != CURLE_OK || !m2) return -1;
      if (m2->flags & CURLWS_CLOSE) return -1;
      if (m2->flags & CURLWS_PING) {
        ws_send_pong(ws, buf + total, m);
        continue;
      }
      if (m2->flags & CURLWS_PONG) continue;
      total += m;
      left = m2->bytesleft;
      cont = (m2->flags & CURLWS_CONT) != 0;
    }
    if (total == 0) continue;
    if (out_binary) *out_binary = is_bin;
    return (int)total;
  }
  return 0;
}
#endif

int ccord_ws_client_recv_wake(ccord_ws_client *ws, uint8_t *buf, size_t cap, bool *out_binary,
                              int wake_fd, int timeout_ms)
{
#if defined(CURLWS_TEXT)
  if (!ws || !ws->curl || !buf || cap == 0) return -1;
  // Always drain libcurl/TLS buffered bytes BEFORE poll(): a frame coalesced into an earlier TLS record
  // (e.g. HTTP 101 + hello, or PING/ack + wake) leaves 0 unread bytes in the kernel socket.
  int first = ws_try_recv_frame(ws, buf, cap, out_binary);
  if (first != 0) return first;
  if (timeout_ms <= 0) return 0;

#if !defined(_WIN32)
  curl_socket_t sock = CURL_SOCKET_BAD;
  if (curl_easy_getinfo(ws->curl, CURLINFO_ACTIVESOCKET, &sock) == CURLE_OK && sock != CURL_SOCKET_BAD) {
    struct pollfd pfds[3];
    nfds_t nfds = 0;
    pfds[nfds++] = (struct pollfd){.fd = sock, .events = POLLIN};
    int wake_idx = -1;
    if (wake_fd >= 0) {
      wake_idx = (int)nfds;
      pfds[nfds++] = (struct pollfd){.fd = wake_fd, .events = POLLIN};
    }
    int timer_idx = -1;
    int wait_ms = timeout_ms;
#if defined(__linux__) && defined(CLOCK_BOOTTIME)
    if (ws->timer_fd >= 0) {
      struct itimerspec its = {0};
      its.it_value.tv_sec = (time_t)(timeout_ms / 1000);
      its.it_value.tv_nsec = (long)((timeout_ms % 1000) * 1000000L);
      if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0) its.it_value.tv_nsec = 1;
      if (timerfd_settime(ws->timer_fd, 0, &its, nullptr) == 0) {
        timer_idx = (int)nfds;
        pfds[nfds++] = (struct pollfd){.fd = ws->timer_fd, .events = POLLIN};
        wait_ms = -1;   // CLOCK_BOOTTIME timerfd advances across Android screen-off SoC naps
      }
    }
#endif
    int pr = poll(pfds, nfds, wait_ms);
#if defined(__linux__) && defined(CLOCK_BOOTTIME)
    if (timer_idx >= 0) {
      if (pr > 0 && (pfds[timer_idx].revents & POLLIN) != 0) {
        uint64_t exp = 0;
        while (read(ws->timer_fd, &exp, sizeof exp) > 0) {}
      }
      struct itimerspec disarm = {0};
      (void)timerfd_settime(ws->timer_fd, 0, &disarm, nullptr);
    }
#else
    (void)timer_idx;
#endif
    if (pr < 0) return errno == EINTR ? 0 : -1;
    if (pr == 0) return 0;
    short srev = pfds[0].revents;
    if ((srev & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) == 0) {
      (void)wake_idx;
      return 0;
    }
    if ((srev & (POLLERR | POLLNVAL)) != 0 && (srev & POLLIN) == 0) {
      return -1;
    }
    int got = ws_try_recv_frame(ws, buf, cap, out_binary);
    if (got != 0) return got;
    if ((srev & (POLLERR | POLLHUP)) != 0) return -1;
    return 0;
  }
#else
  (void)wake_fd;
  (void)timeout_ms;
#endif
  return ws_try_recv_frame(ws, buf, cap, out_binary);
#else
  (void)ws; (void)buf; (void)cap; (void)out_binary; (void)wake_fd; (void)timeout_ms;
  return -1;
#endif
}

int ccord_ws_client_recv(ccord_ws_client *ws, uint8_t *buf, size_t cap, bool *out_binary, int timeout_ms)
{
  return ccord_ws_client_recv_wake(ws, buf, cap, out_binary, -1, timeout_ms);
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
#if defined(__linux__) && defined(CLOCK_BOOTTIME)
  if (ws->timer_fd >= 0) close(ws->timer_fd);
#endif
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

bool ccord_http_download(const ccord_http_download_opts *req, ccord_http_download_resp *out)
{
  (void)req;
  if (!out) return false;
  memset(out, 0, sizeof *out);
  set_err(out->error, sizeof out->error, "offline build: HTTP download unavailable");
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

int ccord_ws_client_recv_wake(ccord_ws_client *ws, uint8_t *buf, size_t cap, bool *out_binary,
                              int wake_fd, int timeout_ms)
{
  (void)ws; (void)buf; (void)cap; (void)out_binary; (void)wake_fd; (void)timeout_ms;
  return -1;
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
