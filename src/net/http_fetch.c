// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/net/http_fetch.h>

#include <ccord/core/alloc.h>
#include <ccord/core/os.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#endif

#if defined(CCORD_HTTP_FETCH_LIVE)
#include <curl/curl.h>
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

bool ccord_http_url_allowed(const char *url)
{
  if (!url) return false;
  const char *p = url;
  bool is_https = strncmp(p, "https://", 8) == 0;
  if (!is_https) return false;
  p += 8;
  const char *end = p;
  while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#') end++;
  size_t hlen = (size_t)(end - p);
  if (hlen == 0 || hlen > 253) return false;

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
    if (host_matches(p, hlen, k_hosts[i])) return true;
  }
  // Allow an explicit user/developer CCORD_CATALOG_URL host override if HTTPS.
  const char *override = getenv("CCORD_CATALOG_URL");
  if (override && strncmp(override, "https://", 8) == 0) {
    const char *op = override + 8;
    const char *oe = op;
    while (*oe && *oe != '/' && *oe != ':' && *oe != '?' && *oe != '#') oe++;
    size_t olen = (size_t)(oe - op);
    if (olen == hlen && strncasecmp(p, op, hlen) == 0) return true;
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

#if defined(CCORD_HTTP_FETCH_LIVE)

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
  if (s->len + n > s->max_bytes) { s->overflow = true; return 0; }
  if (s->len + n + 1 > s->cap) {
    size_t ncap = s->cap ? s->cap * 2u : 4096u;
    while (ncap < s->len + n + 1) {
      if (ncap > SIZE_MAX / 2u) { ncap = s->len + n + 1; break; }
      ncap *= 2u;
    }
    if (ncap > s->max_bytes + 1u) ncap = s->max_bytes + 1u;
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
  if (s->written + n > s->max_bytes) { s->overflow = true; return 0; }
  size_t w = fwrite(ptr, 1, n, s->fp);
  s->written += w;
  return w;
}

static void apply_common_tls(CURL *curl, const char *url)
{
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "C-Cord-Catalog/1.0");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
}

bool ccord_http_get(const char *url, const char *if_none_match, size_t max_bytes, ccord_http_get_result *out)
{
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!ccord_http_url_allowed(url)) {
    set_err(out->error, sizeof out->error, "URL blocked by HTTPS catalog host allowlist");
    return false;
  }
  CURL *curl = curl_easy_init();
  if (!curl) {
    set_err(out->error, sizeof out->error, "libcurl initialization failed");
    return false;
  }

  mem_sink ms = {.max_bytes = max_bytes ? max_bytes : (8u * 1024u * 1024u)};
  hdr_sink hs = {0};
  struct curl_slist *hdrs = nullptr;
  if (if_none_match && if_none_match[0]) {
    char inm[192];
    snprintf(inm, sizeof inm, "If-None-Match: %s", if_none_match);
    hdrs = curl_slist_append(hdrs, inm);
  }
  hdrs = curl_slist_append(hdrs, "Accept: application/json");

  apply_common_tls(curl, url);
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

bool ccord_http_post_json(const char *url, const char *json_body, size_t body_len, size_t max_bytes,
                          ccord_http_get_result *out)
{
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!ccord_http_url_allowed(url)) {
    set_err(out->error, sizeof out->error, "URL blocked by HTTPS catalog host allowlist");
    return false;
  }
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
  apply_common_tls(curl, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fs);

  CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  char *eff_url = nullptr;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff_url);

  fclose(fp);
  bool eff_ok = !eff_url || ccord_http_url_allowed(eff_url);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK || !eff_ok || code < 200 || code >= 300) {
    remove(dest_path);
    if (!eff_ok) set_err(err_buf, err_cap, "redirect target blocked by HTTPS host allowlist");
    else if (fs.overflow) set_err(err_buf, err_cap, "downloaded .cce package exceeds 25 MiB limit");
    else if (rc != CURLE_OK) snprintf(err_buf, err_cap, "download failed: %s", curl_easy_strerror(rc));
    else snprintf(err_buf, err_cap, "download failed: HTTP %ld", code);
    return false;
  }
  return true;
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

#endif
