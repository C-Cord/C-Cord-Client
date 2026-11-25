// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "web_internal.h"

#include <ccord/core/base64.h>
#include <ccord/core/os.h>

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- TLS CA bundle resolution (required on Android where libcurl has no default CA store) ----

typedef enum web_tls_ca_mode : uint8_t {
  WEB_TLS_CA_DEFAULT = 0,
  WEB_TLS_CA_FILE = 1,
  WEB_TLS_CA_DIR = 2,
} web_tls_ca_mode;

static _Atomic int g_web_tls_state = 0;
static web_tls_ca_mode g_web_tls_mode = WEB_TLS_CA_DEFAULT;
static char g_web_tls_path[4096];

static void web_tls_resolve_once(void)
{
  if (atomic_load_explicit(&g_web_tls_state, memory_order_acquire) == 2) return;
  int expected = 0;
  if (atomic_compare_exchange_strong_explicit(&g_web_tls_state, &expected, 1,
                                              memory_order_acq_rel, memory_order_acquire)) {
    const char *cafile = getenv("CCORD_CA_BUNDLE");
    if (!cafile || !cafile[0]) cafile = getenv("SSL_CERT_FILE");
#if CCORD_OS_ANDROID
    if (cafile && cafile[0] && !ccord_path_is_file(cafile)) {
      for (int i = 0; i < 40 && !ccord_path_is_file(cafile); i++) ccord_sleep_ms(5);
    }
#endif
    if (cafile && cafile[0] && ccord_path_is_file(cafile)) {
      size_t n = strlen(cafile);
      if (n < sizeof g_web_tls_path) {
        memcpy(g_web_tls_path, cafile, n + 1u);
        g_web_tls_mode = WEB_TLS_CA_FILE;
      }
    }
#if CCORD_OS_ANDROID
    if (g_web_tls_mode == WEB_TLS_CA_DEFAULT) {
      static const char k_apex[] = "/apex/com.android.conscrypt/cacerts";
      static const char k_legacy[] = "/system/etc/security/cacerts";
      if (ccord_path_is_dir(k_apex)) {
        memcpy(g_web_tls_path, k_apex, sizeof k_apex);
        g_web_tls_mode = WEB_TLS_CA_DIR;
      } else if (ccord_path_is_dir(k_legacy)) {
        memcpy(g_web_tls_path, k_legacy, sizeof k_legacy);
        g_web_tls_mode = WEB_TLS_CA_DIR;
      }
    }
#endif
    atomic_store_explicit(&g_web_tls_state, 2, memory_order_release);
  } else {
    while (atomic_load_explicit(&g_web_tls_state, memory_order_acquire) != 2) ccord_sleep_ms(1);
  }
}

static void web_apply_tls(CURL *easy)
{
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(easy, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
  web_tls_resolve_once();
  if (g_web_tls_mode == WEB_TLS_CA_FILE) {
    curl_easy_setopt(easy, CURLOPT_CAINFO, g_web_tls_path);
  } else if (g_web_tls_mode == WEB_TLS_CA_DIR) {
    curl_easy_setopt(easy, CURLOPT_CAPATH, g_web_tls_path);
  }
}

// ---- URL parsing & relative resolution ----

static void copy_trunc(char *dst, size_t cap, const char *src, size_t len)
{
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  if (len >= cap) len = cap - 1u;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static bool has_scheme(const char *s, size_t *out_colon)
{
  if (!s || !isalpha((unsigned char)s[0])) return false;
  for (size_t i = 1; s[i]; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == ':') {
      if (out_colon) *out_colon = i;
      return true;
    }
    if (!isalnum(c) && c != '+' && c != '-' && c != '.') return false;
  }
  return false;
}

static void normalize_path(const char *in, char *out, size_t cap)
{
  const char *segs[64];
  size_t seg_lens[64];
  size_t n = 0;
  const char *p = in;
  bool trailing_slash = false;
  while (*p) {
    while (*p == '/') p++;
    if (!*p) { trailing_slash = true; break; }
    const char *start = p;
    while (*p && *p != '/') p++;
    size_t len = (size_t)(p - start);
    if (len == 1 && start[0] == '.') {
      trailing_slash = (*p == '/');
      continue;
    }
    if (len == 2 && start[0] == '.' && start[1] == '.') {
      if (n > 0) n--;
      trailing_slash = (*p == '/');
      continue;
    }
    if (n < 64) {
      segs[n] = start;
      seg_lens[n] = len;
      n++;
    }
    trailing_slash = (*p == '/');
  }
  size_t pos = 0;
  if (n == 0) {
    copy_trunc(out, cap, "/", 1);
    return;
  }
  for (size_t i = 0; i < n; i++) {
    if (pos + 1u + seg_lens[i] >= cap) break;
    out[pos++] = '/';
    memcpy(out + pos, segs[i], seg_lens[i]);
    pos += seg_lens[i];
  }
  if (trailing_slash && pos + 1u < cap) out[pos++] = '/';
  out[pos] = '\0';
}

bool web_url_parse(const char *input, const web_url *base, web_url *out)
{
  if (!out) return false;
  web_url *base_copy = nullptr;
  if (base == out) {
    base_copy = ccord_malloc(sizeof *base_copy);
    if (!base_copy) return false;
    *base_copy = *base;
    base = base_copy;
  }
  memset(out, 0, sizeof *out);
  if (!input) { ccord_free(base_copy); return false; }
  while (*input == ' ' || *input == '\t' || *input == '\r' || *input == '\n') input++;
  if (!*input) { ccord_free(base_copy); return false; }

  char *resolved = ccord_malloc(8192u);
  if (!resolved) { ccord_free(base_copy); return false; }
  bool ok = false;
  size_t colon = 0;
  if (has_scheme(input, &colon)) {
    // Special non-hierarchical schemes (data:, blob:, about:, javascript:)
    if (strncmp(input + colon, "://", 3) != 0) {
      copy_trunc(out->href, sizeof out->href, input, strlen(input));
      copy_trunc(out->protocol, sizeof out->protocol, input, colon + 1u);
      for (char *q = out->protocol; *q; q++) *q = (char)tolower((unsigned char)*q);
      if (strcmp(out->protocol, "blob:") == 0) {
        web_url *inner = web_url_alloc_parse(input + 5, nullptr);
        if (inner && inner->origin[0]) {
          snprintf(out->origin, sizeof out->origin, "%s", inner->origin);
        } else {
          snprintf(out->origin, sizeof out->origin, "null");
        }
        ccord_free(inner);
      } else {
        snprintf(out->origin, sizeof out->origin, "null");
      }
      copy_trunc(out->pathname, sizeof out->pathname, input + colon + 1u, strlen(input + colon + 1u));
      ok = true;
      goto done;
    }
    copy_trunc(resolved, 8192u, input, strlen(input));
  } else if (input[0] == '/' && input[1] == '/') {
    const char *proto = (base && base->protocol[0]) ? base->protocol : "https:";
    snprintf(resolved, 8192u, "%s%s", proto, input);
  } else if (base && base->origin[0] && strcmp(base->origin, "null") != 0) {
    if (input[0] == '#') {
      snprintf(resolved, 8192u, "%s%s%s%s", base->origin,
               base->pathname[0] ? base->pathname : "/", base->search, input);
    } else if (input[0] == '?') {
      snprintf(resolved, 8192u, "%s%s%s", base->origin,
               base->pathname[0] ? base->pathname : "/", input);
    } else if (input[0] == '/') {
      snprintf(resolved, 8192u, "%s%s", base->origin, input);
    } else {
      char *dir = ccord_malloc(2048u);
      if (!dir) goto done;
      snprintf(dir, 2048u, "%s", base->pathname[0] ? base->pathname : "/");
      char *slash = strrchr(dir, '/');
      if (slash) slash[1] = '\0';
      else snprintf(dir, 2048u, "/");
      snprintf(resolved, 8192u, "%s%s%s", base->origin, dir, input);
      ccord_free(dir);
    }
  } else {
    goto done;
  }

  const char *sep = strstr(resolved, "://");
  if (!sep) goto done;
  size_t plen = (size_t)(sep - resolved) + 1u;
  copy_trunc(out->protocol, sizeof out->protocol, resolved, plen);
  for (char *q = out->protocol; *q; q++) *q = (char)tolower((unsigned char)*q);

  const char *auth = sep + 3;
  const char *path_start = auth;
  while (*path_start && *path_start != '/' && *path_start != '?' && *path_start != '#') path_start++;
  size_t alen = (size_t)(path_start - auth);
  if (alen == 0) goto done;
  copy_trunc(out->host, sizeof out->host, auth, alen);
  // Strip userinfo if present
  const char *at = strrchr(out->host, '@');
  if (at) {
    memmove(out->host, at + 1, strlen(at + 1) + 1u);
  }
  const char *port_c = strrchr(out->host, ':');
  if (port_c && strchr(out->host, ']') < port_c) {
    copy_trunc(out->hostname, sizeof out->hostname, out->host, (size_t)(port_c - out->host));
    copy_trunc(out->port, sizeof out->port, port_c + 1, strlen(port_c + 1));
  } else {
    copy_trunc(out->hostname, sizeof out->hostname, out->host, strlen(out->host));
    out->port[0] = '\0';
  }
  for (char *q = out->hostname; *q; q++) *q = (char)tolower((unsigned char)*q);
  for (char *q = out->host; *q; q++) *q = (char)tolower((unsigned char)*q);

  // Split path / search / hash
  const char *hash_p = strchr(path_start, '#');
  const char *q_p = strchr(path_start, '?');
  if (hash_p && q_p && q_p > hash_p) q_p = nullptr;

  const char *path_end = q_p ? q_p : (hash_p ? hash_p : path_start + strlen(path_start));
  char *raw_path = ccord_malloc(2048u);
  if (!raw_path) goto done;
  if (path_end > path_start) {
    copy_trunc(raw_path, 2048u, path_start, (size_t)(path_end - path_start));
  } else {
    snprintf(raw_path, 2048u, "/");
  }
  normalize_path(raw_path, out->pathname, sizeof out->pathname);
  ccord_free(raw_path);

  if (q_p) {
    const char *q_end = hash_p ? hash_p : q_p + strlen(q_p);
    copy_trunc(out->search, sizeof out->search, q_p, (size_t)(q_end - q_p));
  }
  if (hash_p) {
    copy_trunc(out->hash, sizeof out->hash, hash_p, strlen(hash_p));
  }

  snprintf(out->origin, sizeof out->origin, "%s//%s", out->protocol, out->host);
  snprintf(out->href, sizeof out->href, "%s%s%s%s", out->origin, out->pathname, out->search, out->hash);
  ok = true;

done:
  ccord_free(resolved);
  ccord_free(base_copy);
  return ok;
}

web_url *web_url_alloc_parse(const char *input, const web_url *base)
{
  web_url *u = ccord_calloc(1, sizeof *u);
  if (!u) return nullptr;
  if (!web_url_parse(input, base, u)) {
    ccord_free(u);
    return nullptr;
  }
  return u;
}

// ---- Policy & data: URI decoding ----

bool web_net_url_allowed(const ccord_web_view *v, const char *url)
{
  if (!v || !url || !url[0]) return false;
  if (strncmp(url, "data:", 5) == 0 || strncmp(url, "blob:", 5) == 0 || strcmp(url, "about:blank") == 0) {
    return true;
  }
  if (!v->cfg.allow_network && !v->cfg.allow_loopback_http) return false;
  web_url *u = web_url_alloc_parse(url, nullptr);
  if (!u) return false;
  bool ok = false;
  if (strcmp(u->protocol, "https:") == 0) {
    ok = v->cfg.allow_network;
  } else if (strcmp(u->protocol, "http:") == 0 && v->cfg.allow_loopback_http) {
    ok = strcmp(u->hostname, "127.0.0.1") == 0 || strcmp(u->hostname, "localhost") == 0;
  }
  ccord_free(u);
  return ok;
}

static int hex_val(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool web_net_decode_data_uri(const char *uri, uint8_t **out_bytes, size_t *out_len, char *out_mime, size_t mime_cap)
{
  if (out_bytes) *out_bytes = nullptr;
  if (out_len) *out_len = 0;
  if (out_mime && mime_cap > 0) out_mime[0] = '\0';
  if (!uri || strncmp(uri, "data:", 5) != 0) return false;
  const char *comma = strchr(uri + 5, ',');
  if (!comma) return false;
  const char *meta = uri + 5;
  size_t mlen = (size_t)(comma - meta);
  bool is_b64 = (mlen >= 7 && strncmp(comma - 7, ";base64", 7) == 0);
  if (out_mime && mime_cap > 0) {
    size_t mime_len = is_b64 ? mlen - 7u : mlen;
    const char *semi = memchr(meta, ';', mime_len);
    if (semi) mime_len = (size_t)(semi - meta);
    if (mime_len == 0) snprintf(out_mime, mime_cap, "text/plain");
    else copy_trunc(out_mime, mime_cap, meta, mime_len);
  }
  const char *payload = comma + 1;
  size_t plen = strlen(payload);
  if (is_b64) {
    size_t dlen = 0;
    uint8_t *dec = ccord_base64_decode_alloc(payload, plen, &dlen);
    if (!dec) return false;
    if (out_bytes) *out_bytes = dec;
    else ccord_free(dec);
    if (out_len) *out_len = dlen;
    return true;
  }
  // Percent-decode
  uint8_t *buf = ccord_malloc(plen + 1u);
  if (!buf) return false;
  size_t w = 0;
  for (size_t i = 0; i < plen; i++) {
    if (payload[i] == '%' && i + 2u < plen) {
      int hi = hex_val(payload[i + 1u]), lo = hex_val(payload[i + 2u]);
      if (hi >= 0 && lo >= 0) {
        buf[w++] = (uint8_t)((hi << 4) | lo);
        i += 2u;
        continue;
      }
    }
    buf[w++] = (uint8_t)payload[i];
  }
  buf[w] = '\0';
  if (out_bytes) *out_bytes = buf;
  else ccord_free(buf);
  if (out_len) *out_len = w;
  return true;
}

// ---- curl_multi non-blocking HTTP engine ----

static size_t fetch_write_cb(void *ptr, size_t size, size_t nmemb, void *ud)
{
  web_fetch *ft = ud;
  if (size != 0 && nmemb > SIZE_MAX / size) return 0;
  size_t n = size * nmemb;
  if (ft->resp_body.len + n > WEB_MAX_RESP_BYTES) return 0;
  if (ccord_buf_append(&ft->resp_body, ptr, n) != CCORD_OK) return 0;
  return n;
}

static size_t fetch_hdr_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
  web_fetch *ft = ud;
  if (size != 0 && nmemb > SIZE_MAX / size) return 0;
  size_t n = size * nmemb;
  if (n >= 5 && memcmp(ptr, "HTTP/", 5) == 0) {
    ft->resp_headers.len = 0;
    if (ft->resp_headers.data) ft->resp_headers.data[0] = '\0';
  } else if (ft->resp_headers.len + n < 65536u) {
    (void)ccord_buf_append(&ft->resp_headers, ptr, n);
  }
  return n;
}

static void fetch_free(web_fetch *ft)
{
  if (!ft) return;
  if (ft->easy) curl_easy_cleanup(ft->easy);
  if (ft->req_headers) curl_slist_free_all(ft->req_headers);
  ccord_free(ft->req_body);
  ccord_buf_free(&ft->resp_body);
  ccord_buf_free(&ft->resp_headers);
  if (ft->view && web_frame_alive(ft->view, ft->frame_id) && ft->frame && ft->frame->ctx) {
    if (!JS_IsUndefined(ft->resolve_fn)) JS_FreeValue(ft->frame->ctx, ft->resolve_fn);
    if (!JS_IsUndefined(ft->reject_fn)) JS_FreeValue(ft->frame->ctx, ft->reject_fn);
    if (!JS_IsUndefined(ft->xhr_obj)) JS_FreeValue(ft->frame->ctx, ft->xhr_obj);
  }
  ccord_free(ft);
}

bool web_net_init(ccord_web_view *v)
{
  static bool s_curl_inited = false;
  if (!s_curl_inited) {
    (void)curl_global_init(CURL_GLOBAL_DEFAULT);
    s_curl_inited = true;
  }
  v->multi = curl_multi_init();
  v->cookie_share = curl_share_init();
  if (!v->multi || !v->cookie_share) {
    web_net_shutdown(v);
    return false;
  }
  (void)curl_share_setopt(v->cookie_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
  (void)curl_multi_setopt(v->multi, CURLMOPT_MAXCONNECTS, 8L);
  return true;
}

void web_net_shutdown(ccord_web_view *v)
{
  if (!v) return;
  web_fetch *ft = v->fetches;
  v->fetches = nullptr;
  v->nfetches = 0;
  while (ft) {
    web_fetch *next = ft->next;
    if (ft->easy && v->multi) curl_multi_remove_handle(v->multi, ft->easy);
    fetch_free(ft);
    ft = next;
  }
  if (v->multi) {
    curl_multi_cleanup(v->multi);
    v->multi = nullptr;
  }
  if (v->cookie_share) {
    curl_share_cleanup(v->cookie_share);
    v->cookie_share = nullptr;
  }
}

void web_net_cancel_frame(ccord_web_view *v, web_frame *f)
{
  if (!v || !f) return;
  web_fetch **pp = &v->fetches;
  while (*pp) {
    web_fetch *ft = *pp;
    if (ft->frame == f) {
      *pp = ft->next;
      if (v->nfetches > 0) v->nfetches--;
      if (ft->easy && v->multi) curl_multi_remove_handle(v->multi, ft->easy);
      fetch_free(ft);
    } else {
      pp = &ft->next;
    }
  }
}

web_fetch *web_net_start(ccord_web_view *v, web_frame *f, web_fetch_kind kind, const char *url,
                         const char *method, const char *const *hdr_kv, size_t nhdrs,
                         const char *body, size_t body_len)
{
  if (!v || !f || !url || !url[0]) return nullptr;
  if (v->nfetches >= WEB_MAX_FETCHES) {
    web_diag_append(v, "warn", "net: fetch queue full for %s", url);
    return nullptr;
  }
  web_url *resolved = web_url_alloc_parse(url, &f->url);
  if (!resolved) {
    web_diag_append(v, "warn", "net: invalid URL %s", url);
    return nullptr;
  }
  if (!web_net_url_allowed(v, resolved->href)) {
    web_diag_append(v, "warn", "net: blocked URL %s", resolved->href);
    ccord_free(resolved);
    return nullptr;
  }

  web_fetch *ft = ccord_calloc(1, sizeof *ft);
  if (!ft) { ccord_free(resolved); return nullptr; }
  ft->kind = kind;
  ft->view = v;
  ft->frame = f;
  ft->frame_id = f->id;
  ft->resolve_fn = JS_UNDEFINED;
  ft->reject_fn = JS_UNDEFINED;
  ft->xhr_obj = JS_UNDEFINED;
  snprintf(ft->url, sizeof ft->url, "%s", resolved->href);
  snprintf(ft->final_url, sizeof ft->final_url, "%s", resolved->href);
  snprintf(ft->method, sizeof ft->method, "%s", (method && method[0]) ? method : "GET");
  char resolved_origin[256];
  snprintf(resolved_origin, sizeof resolved_origin, "%s", resolved->origin);
  ccord_free(resolved);
  ccord_buf_init(&ft->resp_body, 0);
  ccord_buf_init(&ft->resp_headers, 0);

  // Local immediate schemes: data: and blob:
  if (strncmp(ft->url, "data:", 5) == 0) {
    uint8_t *dec = nullptr;
    size_t dlen = 0;
    char mime[64] = "";
    if (web_net_decode_data_uri(ft->url, &dec, &dlen, mime, sizeof mime)) {
      if (dlen > 0) (void)ccord_buf_append(&ft->resp_body, dec, dlen);
      ccord_free(dec);
      char hdr[128];
      snprintf(hdr, sizeof hdr, "Content-Type: %s\r\n", mime);
      (void)ccord_buf_append_str(&ft->resp_headers, hdr);
      ft->http_status = 200;
    } else {
      ft->failed = true;
    }
    ft->done = true;
    ft->next = v->fetches;
    v->fetches = ft;
    v->nfetches++;
    return ft;
  }
  if (strncmp(ft->url, "blob:", 5) == 0) {
    const web_blob_entry *be = web_blob_find(v, ft->url);
    if (be) {
      if (be->len > 0) (void)ccord_buf_append(&ft->resp_body, be->data, be->len);
      char hdr[128];
      snprintf(hdr, sizeof hdr, "Content-Type: %s\r\n", be->mime[0] ? be->mime : "application/octet-stream");
      (void)ccord_buf_append_str(&ft->resp_headers, hdr);
      ft->http_status = 200;
    } else {
      ft->failed = true;
      ft->http_status = 404;
    }
    ft->done = true;
    ft->next = v->fetches;
    v->fetches = ft;
    v->nfetches++;
    return ft;
  }

  CURL *easy = curl_easy_init();
  if (!easy) {
    fetch_free(ft);
    return nullptr;
  }
  ft->easy = easy;

  if (body && body_len > 0) {
    ft->req_body = ccord_malloc(body_len + 1u);
    if (!ft->req_body) {
      fetch_free(ft);
      return nullptr;
    }
    memcpy(ft->req_body, body, body_len);
    ft->req_body[body_len] = '\0';
    ft->req_body_len = body_len;
  }

  bool has_origin = false, has_referer = false, has_accept = false;
  for (size_t i = 0; i + 1u < nhdrs; i += 2u) {
    const char *k = hdr_kv[i], *val = hdr_kv[i + 1u];
    if (!k || !val) continue;
    if (ccord_strncasecmp(k, "Origin", 6) == 0 && k[6] == '\0') has_origin = true;
    if (ccord_strncasecmp(k, "Referer", 7) == 0 && k[7] == '\0') has_referer = true;
    if (ccord_strncasecmp(k, "Accept", 6) == 0 && k[6] == '\0') has_accept = true;
    size_t ln = strlen(k) + strlen(val) + 4u;
    char *line = ccord_malloc(ln);
    if (line) {
      snprintf(line, ln, "%s: %s", k, val);
      ft->req_headers = curl_slist_append(ft->req_headers, line);
      ccord_free(line);
    }
  }
  if (!has_accept) {
    ft->req_headers = curl_slist_append(ft->req_headers, "Accept: */*");
  }
  ft->req_headers = curl_slist_append(ft->req_headers, "Accept-Language: en-US,en;q=0.9");
  const web_url *ref_url = (kind == WEB_FETCH_IFRAME && f->parent) ? &f->parent->url : &f->url;
  if (!has_referer && ref_url->origin[0] && strncmp(ref_url->origin, "http", 4) == 0) {
    char *ref = ccord_malloc(4400u);
    if (ref) {
      snprintf(ref, 4400u, "Referer: %s%s%s", ref_url->origin,
               ref_url->pathname[0] ? ref_url->pathname : "/", ref_url->search);
      ft->req_headers = curl_slist_append(ft->req_headers, ref);
      ccord_free(ref);
    }
  }
  bool need_origin = (strcmp(ft->method, "GET") != 0) ||
                     ((kind == WEB_FETCH_JS_FETCH || kind == WEB_FETCH_XHR) &&
                      strcmp(resolved_origin, f->url.origin) != 0);
  if (!has_origin && need_origin && f->url.origin[0] && strcmp(f->url.origin, "null") != 0) {
    char orig[300];
    snprintf(orig, sizeof orig, "Origin: %s", f->url.origin);
    ft->req_headers = curl_slist_append(ft->req_headers, orig);
  }

  curl_easy_setopt(easy, CURLOPT_URL, ft->url);
  curl_easy_setopt(easy, CURLOPT_USERAGENT, v->user_agent);
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, ft->req_headers);
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, fetch_write_cb);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, ft);
  curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, fetch_hdr_cb);
  curl_easy_setopt(easy, CURLOPT_HEADERDATA, ft);
  curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 25000L);
  curl_easy_setopt(easy, CURLOPT_COOKIEFILE, "");
  curl_easy_setopt(easy, CURLOPT_SHARE, v->cookie_share);
  const char *protos = v->cfg.allow_loopback_http ? "http,https" : "https";
  curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, protos);
  curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, protos);
  web_apply_tls(easy);

  if (ccord_strncasecmp(ft->method, "POST", 4) == 0) {
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, ft->req_body ? ft->req_body : "");
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)ft->req_body_len);
  } else if (ccord_strncasecmp(ft->method, "GET", 3) != 0) {
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, ft->method);
    if (ft->req_body) {
      curl_easy_setopt(easy, CURLOPT_POSTFIELDS, ft->req_body);
      curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)ft->req_body_len);
    }
  }

  if (curl_multi_add_handle(v->multi, easy) != CURLM_OK) {
    fetch_free(ft);
    return nullptr;
  }
  ft->next = v->fetches;
  v->fetches = ft;
  v->nfetches++;
  return ft;
}

static bool decode_image_payload(web_frame *f, web_image *img, const uint8_t *bytes, size_t len)
{
  if (!img || !bytes || len == 0) return false;
  // Check for SVG XML payload (e.g. data:image/svg+xml or .svg asset)
  const char *body_p = (const char *)bytes;
  size_t probe_len = len < 256u ? len : 256u;
  bool is_svg = false;
  for (size_t k = 0; k + 4u <= probe_len; k++) {
    if (ccord_strncasecmp(body_p + k, "<svg", 4) == 0) { is_svg = true; break; }
  }
  if (is_svg) {
    ccord_free(img->svg_xml);
    img->svg_xml = ccord_malloc(len + 1u);
    if (!img->svg_xml) return false;
    memcpy(img->svg_xml, body_p, len);
    img->svg_xml[len] = '\0';
    img->svg_root = nullptr;
    img->loaded = true;
    img->failed = false;
    img->w = 48;
    img->h = 48;
    if (f) {
      f->layout_dirty = true;
      web_mark_dirty(f->view);
    }
    return true;
  }
  ccord_error ierr = {0};
  uint8_t *rgba = nullptr;
  int w = 0, h = 0;
  if (ccord_image_decode_rgba(bytes, len, &rgba, &w, &h, &ierr) == CCORD_OK) {
    ccord_free(img->rgba);
    ccord_free(img->svg_xml);
    img->svg_xml = nullptr;
    img->svg_root = nullptr;
    img->rgba = rgba;
    img->w = w;
    img->h = h;
    img->loaded = true;
    img->failed = false;
    img->uploaded = false;
    if (f) {
      f->layout_dirty = true;
      web_mark_dirty(f->view);
    }
    return true;
  }
  return false;
}

static void fetch_complete_one(ccord_web_view *v, web_fetch *ft)
{
  if (!v || !ft || v->destroying) return;
  if (ft->kind != WEB_FETCH_TOP_DOC && !web_frame_alive(v, ft->frame_id)) return;
  switch (ft->kind) {
    case WEB_FETCH_SCRIPT:
    case WEB_FETCH_STYLE:
    case WEB_FETCH_IFRAME:
      web_dom_on_fetch_done(ft);
      break;
    case WEB_FETCH_IMAGE:
      if (ft->img) {
        ft->img->loading = false;
        if (!ft->failed && ft->http_status >= 200 && ft->http_status < 300) {
          bool ok = (ft->img->loaded && ft->resp_body.len == 0) ||
                    (ft->resp_body.len > 0 &&
                     decode_image_payload(ft->frame, ft->img, (const uint8_t *)ft->resp_body.data, ft->resp_body.len));
          if (ok) {
            if (ft->node && ft->frame) web_dom_dispatch_simple(ft->frame, ft->node, "load", false);
          } else {
            ft->img->failed = true;
            if (ft->node && ft->frame) web_dom_dispatch_simple(ft->frame, ft->node, "error", false);
          }
        } else {
          ft->img->failed = true;
          if (ft->node && ft->frame) web_dom_dispatch_simple(ft->frame, ft->node, "error", false);
        }
      }
      break;
    case WEB_FETCH_JS_FETCH:
      web_js_on_fetch_done(ft);
      break;
    case WEB_FETCH_XHR:
      web_js_on_xhr_done(ft);
      break;
    case WEB_FETCH_WORKER:
      web_js_on_worker_script_done(ft);
      break;
    case WEB_FETCH_TOP_DOC:
      if (!ft->failed && ft->http_status >= 200 && ft->http_status < 400 && ft->resp_body.data) {
        ccord_error err = {0};
        (void)ccord_web_view_load_html(v, ft->resp_body.data, ft->resp_body.len, ft->final_url, &err);
      } else {
        web_diag_append(v, "error", "Failed to load %s (HTTP %ld)", ft->url, ft->http_status);
      }
      break;
  }
}

void web_net_pump(ccord_web_view *v)
{
  if (!v || !v->multi) return;

  // Complete any immediate (data:/blob:) fetches first.
  bool progress = true;
  while (progress) {
    progress = false;
    web_fetch **pp = &v->fetches;
    while (*pp) {
      web_fetch *ft = *pp;
      if (ft->done) {
        *pp = ft->next;
        if (v->nfetches > 0) v->nfetches--;
        fetch_complete_one(v, ft);
        fetch_free(ft);
        progress = true;
        break;
      }
      pp = &ft->next;
    }
  }

  int running = 0;
  (void)curl_multi_perform(v->multi, &running);
  CURLMsg *msg = nullptr;
  int left = 0;
  while ((msg = curl_multi_info_read(v->multi, &left)) != nullptr) {
    if (msg->msg != CURLMSG_DONE) continue;
    CURL *easy = msg->easy_handle;
    web_fetch **pp = &v->fetches;
    web_fetch *ft = nullptr;
    while (*pp) {
      if ((*pp)->easy == easy) {
        ft = *pp;
        *pp = ft->next;
        if (v->nfetches > 0) v->nfetches--;
        break;
      }
      pp = &(*pp)->next;
    }
    curl_multi_remove_handle(v->multi, easy);
    if (!ft) continue;

    CURLcode rc = msg->data.result;
    if (rc == CURLE_OK) {
      (void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &ft->http_status);
      const char *eff = nullptr;
      if (curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff && eff[0]) {
        const char *orig_hash = strchr(ft->url, '#');
        if (orig_hash && !strchr(eff, '#')) {
          snprintf(ft->final_url, sizeof ft->final_url, "%s%s", eff, orig_hash);
        } else {
          snprintf(ft->final_url, sizeof ft->final_url, "%s", eff);
        }
      }
      if (ft->kind == WEB_FETCH_SCRIPT || ft->kind == WEB_FETCH_IFRAME ||
          ft->kind == WEB_FETCH_JS_FETCH || ft->kind == WEB_FETCH_XHR || ft->kind == WEB_FETCH_WORKER) {
        static const char *const k_kn[] = {"script", "style", "iframe", "img", "fetch", "xhr", "worker", "doc"};
        web_diag_append(v, "info", "net[%s] %s %.160s -> HTTP %ld (%zu B)",
                        k_kn[ft->kind], ft->method, ft->url, ft->http_status, ft->resp_body.len);
      }
    } else {
      ft->failed = true;
      web_diag_append(v, "warn", "net: curl err %d (%s) for %.160s", (int)rc, curl_easy_strerror(rc), ft->url);
    }
    ft->done = true;
    fetch_complete_one(v, ft);
    fetch_free(ft);
    web_drain_jobs(v);
  }
}

void web_net_fetch_image(web_frame *f, web_node *node, web_image *img, const char *raw_url)
{
  if (!f || !img || !raw_url || !raw_url[0]) return;
  web_url *u = web_url_alloc_parse(raw_url, &f->url);
  if (!u) return;
  if (img->url[0] && strcmp(img->url, u->href) == 0 && (img->loading || img->loaded)) {
    ccord_free(u);
    return;
  }
  copy_trunc(img->url, sizeof img->url, u->href, strlen(u->href));
  img->loading = true;
  img->loaded = false;
  img->failed = false;
  img->uploaded = false;

  // Decode local data:/blob: images synchronously so CSS data:image/svg+xml backgrounds
  // and inline data-URI <img> tags paint on the very first frame without a pump delay.
  if (strncmp(u->href, "data:", 5) == 0 || strncmp(u->href, "blob:", 5) == 0) {
    bool decoded = false;
    if (strncmp(u->href, "data:", 5) == 0) {
      uint8_t *dec = nullptr;
      size_t dlen = 0;
      if (web_net_decode_data_uri(u->href, &dec, &dlen, nullptr, 0)) {
        decoded = decode_image_payload(f, img, dec, dlen);
        ccord_free(dec);
      }
    } else if (f->view) {
      const web_blob_entry *be = web_blob_find(f->view, u->href);
      if (be && be->data && be->len > 0) {
        decoded = decode_image_payload(f, img, be->data, be->len);
      }
    }
    img->loading = false;
    if (!decoded) img->failed = true;
    // For DOM <img> elements, still schedule async load/error delivery on next pump
    if (node && f->view && f->view->nfetches < WEB_MAX_FETCHES) {
      web_fetch *ft = ccord_calloc(1, sizeof *ft);
      if (ft) {
        ft->kind = WEB_FETCH_IMAGE;
        ft->view = f->view;
        ft->frame = f;
        ft->frame_id = f->id;
        ft->node = node;
        ft->img = img;
        ft->resolve_fn = JS_UNDEFINED;
        ft->reject_fn = JS_UNDEFINED;
        ft->xhr_obj = JS_UNDEFINED;
        ft->http_status = decoded ? 200 : 400;
        ft->failed = !decoded;
        ft->done = true;
        ccord_buf_init(&ft->resp_body, 0);
        ccord_buf_init(&ft->resp_headers, 0);
        ft->next = f->view->fetches;
        f->view->fetches = ft;
        f->view->nfetches++;
      }
    }
    ccord_free(u);
    return;
  }

  web_fetch *ft = web_net_start(f->view, f, WEB_FETCH_IMAGE, u->href, "GET", nullptr, 0, nullptr, 0);
  ccord_free(u);
  if (ft) {
    ft->node = node;
    ft->img = img;
  } else {
    img->loading = false;
    img->failed = true;
  }
}
