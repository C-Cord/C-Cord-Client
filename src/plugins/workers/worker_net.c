// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "workers_internal.h"

// ---------------------------------------------------------------- Built-in loopback TCP HTTP/1.1

static bool valid_http_method(const char *m)
{
  if (!m || !m[0]) return false;
  size_t n = 0;
  for (const char *p = m; *p; p++) {
    if (*p < 'A' || *p > 'Z') return false;
    if (++n > 15u) return false;
  }
  return n >= 1u;
}

#if !CCORD_OS_WINDOWS
#ifdef MSG_NOSIGNAL
#define PW_SEND_FLAGS MSG_NOSIGNAL
#else
#define PW_SEND_FLAGS 0
#endif

static bool valid_http_path(const char *path)
{
  if (!path || path[0] != '/') return false;
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
    if (*p <= 0x20u || *p >= 0x7fu) return false;
  }
  return true;
}

static bool valid_hdr_name(const char *k)
{
  if (!k || !k[0]) return false;
  for (const unsigned char *p = (const unsigned char *)k; *p; p++) {
    if (*p <= 0x20u || *p >= 0x7fu || *p == ':') return false;
  }
  if (ccord_strcasecmp(k, "host") == 0 || ccord_strcasecmp(k, "content-length") == 0 ||
      ccord_strcasecmp(k, "transfer-encoding") == 0 || ccord_strcasecmp(k, "connection") == 0) {
    return false;
  }
  return true;
}

static bool valid_hdr_val(const char *v, size_t vl)
{
  for (size_t i = 0; i < vl; i++) {
    unsigned char c = (unsigned char)v[i];
    if ((c < 0x20u && c != '\t') || c == 0x7fu) return false;
  }
  return true;
}

static int tcp_connect_timeout(const char *host, int port, uint32_t timeout_ms)
{
  struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
  struct addrinfo *res = nullptr;
  char pstr[16];
  snprintf(pstr, sizeof pstr, "%d", port);
  if (getaddrinfo(host, pstr, &hints, &res) != 0 || !res) return -1;
  int fd = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    ccord_sock_set_nosigpipe(fd);
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) {
      if (fl >= 0) (void)fcntl(fd, F_SETFL, fl);
      break;
    }
    if (errno == EINPROGRESS) {
      struct pollfd pf = {.fd = fd, .events = POLLOUT};
      if (poll(&pf, 1, (int)timeout_ms) > 0) {
        int soerr = 0; socklen_t sl = sizeof soerr;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0) {
          if (fl >= 0) (void)fcntl(fd, F_SETFL, fl);
          break;
        }
      }
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}
#endif

typedef struct http_job {
  plugin_workers *w;
  char owner[65];
  uint64_t req_id;
  char url[2048];
  char method[16];
  char dest_path[CCORD_PATH_MAX]; // non-empty => streaming download mode
  char *headers_json;
  uint8_t *body;
  size_t body_len;
  uint32_t timeout_ms;
  size_t max_bytes;
  bool binary;
  bool follow_redirects;
  bool local_only;
  bool allow_local;
  bool initial_https;
  bool initial_loopback;
  bool has_host_policy;
  ccord_manifest host_policy; // thread-safe deep copy of allowed_hosts + allowed_host_count
} http_job;

static bool http_job_allow_url_const(const http_job *j, const char *next_url, char *err_buf, size_t err_cap)
{
  if (!j || !next_url || !next_url[0]) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "invalid redirect URL");
    return false;
  }
  char scheme[16] = "", host[256] = "", path[1024] = "";
  int port = 80;
  if (!pw_extract_url_host_port(next_url, scheme, sizeof scheme, host, sizeof host, &port, path, sizeof path)) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "redirect target has invalid URL");
    return false;
  }
  bool is_loc = pw_is_localhost_host(host);
  if (j->initial_https && strcmp(scheme, "https") != 0) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "redirect blocked: HTTPS to HTTP downgrade");
    return false;
  }
  if (strcmp(scheme, "https") != 0 && !(strcmp(scheme, "http") == 0 && is_loc && j->allow_local)) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "redirect target has disallowed scheme");
    return false;
  }
  if (!j->initial_loopback && is_loc) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "redirect blocked: public request cannot redirect to loopback");
    return false;
  }
  if (j->initial_loopback && !is_loc) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "redirect blocked: loopback request cannot redirect to external host");
    return false;
  }
  if (j->local_only && !is_loc) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "network.local only allows localhost/127.0.0.1");
    return false;
  }
  if (!is_loc && j->has_host_policy && !ccord_manifest_host_allowed(&j->host_policy, host)) {
    if (err_buf && err_cap) {
      snprintf(err_buf, err_cap, "redirect host '%.160s' not permitted by plugin.json allowed_hosts", host);
    }
    return false;
  }
  return true;
}

static bool http_job_allow_url(void *ud, const char *next_url, char *err_buf, size_t err_cap)
{
  return http_job_allow_url_const((const http_job *)ud, next_url, err_buf, err_cap);
}

#if !CCORD_OS_WINDOWS
static bool lb_is_redirect_status(long code)
{
  return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}

static bool lb_resolve_redirect_url(const char *cur_scheme, const char *cur_host, int cur_port,
                                    const char *cur_path, const char *loc, char *out, size_t cap)
{
  if (!loc || !loc[0] || !out || cap == 0) return false;
  if (strstr(loc, "://") != nullptr) {
    return snprintf(out, cap, "%s", loc) < (int)cap;
  }
  if (loc[0] == '/' && loc[1] == '/') {
    return snprintf(out, cap, "%s:%s", cur_scheme, loc) < (int)cap;
  }
  bool is_v6 = strchr(cur_host, ':') != nullptr;
  char host_fmt[270];
  if (is_v6) snprintf(host_fmt, sizeof host_fmt, "[%s]", cur_host);
  else snprintf(host_fmt, sizeof host_fmt, "%s", cur_host);
  if (loc[0] == '/') {
    return snprintf(out, cap, "%s://%s:%d%s", cur_scheme, host_fmt, cur_port, loc) < (int)cap;
  }
  const char *slash = strrchr(cur_path ? cur_path : "/", '/');
  size_t base_len = slash ? (size_t)(slash - cur_path + 1) : 1u;
  return snprintf(out, cap, "%s://%s:%d%.*s%s", cur_scheme, host_fmt, cur_port,
                  (int)base_len, cur_path ? cur_path : "/", loc) < (int)cap;
}

static bool lb_send_http_request(int fd, const char *host, int port, const char *path, const char *method,
                                 const char *headers_json, bool strip_creds, const uint8_t *body, size_t body_len,
                                 char *err_buf, size_t err_cap)
{
  ccord_buf req; ccord_buf_init(&req, 256u);
  char port_s[16], clen_s[32];
  snprintf(port_s, sizeof port_s, "%d", port);
  snprintf(clen_s, sizeof clen_s, "%zu", body_len);
  bool is_v6 = strchr(host, ':') != nullptr;
  (void)ccord_buf_append_str(&req, method);
  (void)ccord_buf_append(&req, " ", 1);
  (void)ccord_buf_append_str(&req, path);
  (void)ccord_buf_append_str(&req, " HTTP/1.1\r\nHost: ");
  if (is_v6) (void)ccord_buf_append(&req, "[", 1);
  (void)ccord_buf_append_str(&req, host);
  if (is_v6) (void)ccord_buf_append(&req, "]", 1);
  (void)ccord_buf_append(&req, ":", 1);
  (void)ccord_buf_append_str(&req, port_s);
  (void)ccord_buf_append_str(&req, "\r\nConnection: close\r\nContent-Length: ");
  (void)ccord_buf_append_str(&req, clen_s);
  (void)ccord_buf_append(&req, "\r\n", 2);
  if (headers_json && headers_json[0]) {
    ccord_arena ar; ccord_arena_init(&ar, 1024u);
    ccord_error pe = {0};
    const ccord_json_limits lim = {4, 256, 16384u};
    const ccord_json *hj = ccord_json_parse(&ar, headers_json, strlen(headers_json), &lim, &pe);
    if (hj && ccord_json_type_of(hj) == CCORD_JSON_OBJECT) {
      for (size_t i = 0; i < ccord_json_object_len(hj); i++) {
        const ccord_json *v = nullptr;
        const char *k = ccord_json_object_key(hj, i, &v);
        size_t vl = 0;
        const char *vs = v ? ccord_json_string(v, &vl) : nullptr;
        if (k && vs && valid_hdr_name(k) && valid_hdr_val(vs, vl)) {
          if (strip_creds &&
              (ccord_strcasecmp(k, "authorization") == 0 ||
               ccord_strcasecmp(k, "proxy-authorization") == 0 ||
               ccord_strcasecmp(k, "cookie") == 0)) {
            continue;
          }
          (void)ccord_buf_append_str(&req, k);
          (void)ccord_buf_append(&req, ": ", 2);
          (void)ccord_buf_append(&req, vs, vl);
          (void)ccord_buf_append(&req, "\r\n", 2);
        }
      }
    }
    ccord_arena_free(&ar);
  }
  (void)ccord_buf_append(&req, "\r\n", 2);
  if (body && body_len > 0) (void)ccord_buf_append(&req, (const char *)body, body_len);
  if (req.failed) {
    ccord_buf_free(&req);
    snprintf(err_buf, err_cap, "out of memory");
    return false;
  }

  size_t sent = 0;
  while (sent < req.len) {
    ssize_t n = send(fd, req.data + sent, req.len - sent, PW_SEND_FLAGS);
    if (n <= 0) {
      ccord_buf_free(&req);
      snprintf(err_buf, err_cap, "send failed");
      return false;
    }
    sent += (size_t)n;
  }
  ccord_buf_free(&req);
  return true;
}

static bool loopback_http_once(const char *host, int port, const char *path, const char *method,
                               const char *headers_json, bool strip_creds, const uint8_t *body, size_t body_len,
                               uint32_t timeout_ms, size_t max_bytes, ccord_plugin_http_resp *resp,
                               char *loc_out, size_t loc_cap)
{
  if (loc_out && loc_cap) loc_out[0] = '\0';
  const char *eff_method = (method && method[0]) ? method : "GET";
  if (!valid_http_method(eff_method)) {
    snprintf(resp->error, sizeof resp->error, "invalid HTTP method");
    return false;
  }
  if (!valid_http_path(path)) {
    snprintf(resp->error, sizeof resp->error, "invalid HTTP request path");
    return false;
  }
  uint32_t tmo = timeout_ms ? timeout_ms : 5000u;
  int fd = tcp_connect_timeout(host, port, tmo);
  if (fd < 0) { snprintf(resp->error, sizeof resp->error, "connect failed"); return false; }
  struct timeval tv = {.tv_sec = (time_t)(tmo / 1000u), .tv_usec = (suseconds_t)((tmo % 1000u) * 1000u)};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

  if (!lb_send_http_request(fd, host, port, path, eff_method, headers_json, strip_creds, body, body_len,
                            resp->error, sizeof resp->error)) {
    close(fd);
    return false;
  }

  size_t lim = max_bytes ? max_bytes : PW_DEFAULT_HTTP_BODY;
  ccord_buf raw; ccord_buf_init(&raw, 0);
  char tmp[16384];
  bool overflow = false;
  size_t hdr_end = 0;
  bool found_hdr = false;
  for (;;) {
    ssize_t n = recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) break;
    if (!found_hdr) {
      if (raw.len + (size_t)n > lim + 65536u) { overflow = true; break; }
      if (ccord_buf_append(&raw, tmp, (size_t)n) != CCORD_OK) { overflow = true; break; }
      for (size_t i = 0; i + 3 < raw.len; i++) {
        if (raw.data[i] == '\r' && raw.data[i + 1] == '\n' && raw.data[i + 2] == '\r' && raw.data[i + 3] == '\n') {
          hdr_end = i;
          found_hdr = true;
          break;
        }
      }
      if (!found_hdr && raw.len > 65536u) { overflow = true; break; }
      if (found_hdr && raw.len - (hdr_end + 4u) > lim) { overflow = true; break; }
    } else {
      size_t cur_body = raw.len - (hdr_end + 4u);
      if ((size_t)n > lim || cur_body > lim - (size_t)n) { overflow = true; break; }
      if (ccord_buf_append(&raw, tmp, (size_t)n) != CCORD_OK) { overflow = true; break; }
    }
  }
  close(fd);
  if (overflow) {
    ccord_buf_free(&raw);
    snprintf(resp->error, sizeof resp->error, "response exceeded maximum size");
    return false;
  }
  if (raw.len == 0) { ccord_buf_free(&raw); snprintf(resp->error, sizeof resp->error, "empty response"); return false; }
  if (!found_hdr) { ccord_buf_free(&raw); snprintf(resp->error, sizeof resp->error, "malformed HTTP response"); return false; }

  int status = 0;
  (void)sscanf(ccord_buf_cstr(&raw), "HTTP/%*d.%*d %d", &status);
  resp->status_code = status;
  const char *sep = raw.data + hdr_end;
  const char *bstart = sep + 4;
  size_t blen = raw.len - (hdr_end + 4u);
  resp->body = ccord_malloc(blen + 1u);
  if (resp->body) {
    if (blen) memcpy(resp->body, bstart, blen);
    resp->body[blen] = 0;
    resp->body_len = blen;
  }
  // Parse response headers into JSON
  ccord_buf hbuf; ccord_buf_init(&hbuf, 0);
  ccord_jw hw; ccord_jw_init(&hw, &hbuf);
  ccord_jw_begin_object(&hw);
  const char *line = strstr(raw.data, "\r\n");
  if (line && line < sep) line += 2;
  else line = sep;
  while (line < sep) {
    const char *eol = strstr(line, "\r\n");
    if (!eol || eol > sep) eol = sep;
    const char *col = memchr(line, ':', (size_t)(eol - line));
    if (col && col > line) {
      char kbuf[128];
      size_t kl = (size_t)(col - line);
      if (kl >= sizeof kbuf) kl = sizeof kbuf - 1u;
      for (size_t i = 0; i < kl; i++) kbuf[i] = (char)tolower((unsigned char)line[i]);
      kbuf[kl] = '\0';
      const char *vs = col + 1;
      while (vs < eol && (*vs == ' ' || *vs == '\t')) vs++;
      size_t vl = (size_t)(eol - vs);
      while (vl > 0 && (vs[vl - 1] == ' ' || vs[vl - 1] == '\t')) vl--;
      if (loc_out && loc_cap && strcmp(kbuf, "location") == 0 && vl > 0 && vl < loc_cap) {
        memcpy(loc_out, vs, vl);
        loc_out[vl] = '\0';
      }
      ccord_jw_key(&hw, kbuf);
      ccord_jw_string(&hw, vs, vl);
    }
    line = eol < sep ? eol + 2 : sep;
  }
  ccord_jw_end_object(&hw);
  if (ccord_jw_ok(&hw)) resp->headers_json = ccord_strndup(ccord_buf_cstr(&hbuf), hbuf.len);
  ccord_buf_free(&hbuf);
  ccord_buf_free(&raw);
  return true;
}

static bool loopback_http_do(const http_job *j, ccord_plugin_http_resp *resp)
{
  char cur_url[2048];
  snprintf(cur_url, sizeof cur_url, "%s", j->url);
  char cur_method[16];
  snprintf(cur_method, sizeof cur_method, "%s", j->method);
  const uint8_t *cur_body = j->body;
  size_t cur_body_len = j->body_len;
  bool strip_creds = false;
  uint8_t max_hops = j->follow_redirects ? PW_MAX_REDIRECTS : 0u;

  for (uint8_t hop = 0; hop <= max_hops; hop++) {
    char scheme[16] = "", host[256] = "", path[1024] = "";
    int port = 80;
    if (!pw_extract_url_host_port(cur_url, scheme, sizeof scheme, host, sizeof host, &port, path, sizeof path)) {
      snprintf(resp->error, sizeof resp->error, "invalid URL");
      return false;
    }
    char loc[2048] = "";
    ccord_plugin_http_resp hop_resp = {0};
    if (!loopback_http_once(host, port, path, cur_method, j->headers_json, strip_creds,
                            cur_body, cur_body_len, j->timeout_ms, j->max_bytes, &hop_resp, loc, sizeof loc)) {
      snprintf(resp->error, sizeof resp->error, "%s", hop_resp.error);
      ccord_free(hop_resp.body);
      ccord_free(hop_resp.headers_json);
      return false;
    }
    if (j->follow_redirects && lb_is_redirect_status(hop_resp.status_code) && loc[0]) {
      ccord_free(hop_resp.body);
      ccord_free(hop_resp.headers_json);
      if (hop == max_hops) {
        snprintf(resp->error, sizeof resp->error, "too many redirects");
        return false;
      }
      char next_url[2048] = "";
      if (!lb_resolve_redirect_url(scheme, host, port, path, loc, next_url, sizeof next_url)) {
        snprintf(resp->error, sizeof resp->error, "redirect URL too long");
        return false;
      }
      if (!http_job_allow_url_const(j, next_url, resp->error, sizeof resp->error)) {
        return false;
      }
      char nscheme[16] = "", nhost[256] = "", npath[1024] = "";
      int nport = 80;
      if (!pw_extract_url_host_port(next_url, nscheme, sizeof nscheme, nhost, sizeof nhost, &nport, npath, sizeof npath) ||
          strcmp(nscheme, "http") != 0) {
        snprintf(resp->error, sizeof resp->error, "redirect target unsupported in loopback mode");
        return false;
      }
      if (ccord_strcasecmp(host, nhost) != 0 || port != nport) strip_creds = true;
      if (hop_resp.status_code == 303 ||
          ((hop_resp.status_code == 301 || hop_resp.status_code == 302) && ccord_strcasecmp(cur_method, "POST") == 0)) {
        snprintf(cur_method, sizeof cur_method, "GET");
        cur_body = nullptr;
        cur_body_len = 0;
      }
      snprintf(cur_url, sizeof cur_url, "%s", next_url);
      continue;
    }
    *resp = hop_resp;
    if (hop > 0) snprintf(resp->final_url, sizeof resp->final_url, "%s", cur_url);
    return true;
  }
  snprintf(resp->error, sizeof resp->error, "too many redirects");
  return false;
}

static bool lb_dl_open_sibling_tmp(const char *dest_path, char *tmp_path, size_t tmp_cap, FILE **out_fp,
                                   char *err_buf, size_t err_cap)
{
  *out_fp = nullptr;
  tmp_path[0] = '\0';
  const char *slash = strrchr(dest_path, '/');
  size_t dir_len = slash ? (size_t)(slash - dest_path) : 0u;
  static const char k_hex[] = "0123456789abcdef";
  for (int attempt = 0; attempt < 4; attempt++) {
    uint8_t rnd[8] = {0};
    if (ccord_random_bytes(rnd, sizeof rnd) != CCORD_OK) {
      snprintf(err_buf, err_cap, "failed to generate temporary filename");
      return false;
    }
    char hex[17];
    for (int i = 0; i < 8; i++) {
      hex[i * 2] = k_hex[(rnd[i] >> 4) & 0x0Fu];
      hex[i * 2 + 1] = k_hex[rnd[i] & 0x0Fu];
    }
    hex[16] = '\0';
    int n = slash
      ? snprintf(tmp_path, tmp_cap, "%.*s/.ccord-dl-%s.tmp", (int)dir_len, dest_path, hex)
      : snprintf(tmp_path, tmp_cap, ".ccord-dl-%s.tmp", hex);
    if (n <= 0 || (size_t)n >= tmp_cap) {
      snprintf(err_buf, err_cap, "destination path too long");
      return false;
    }
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
      snprintf(err_buf, err_cap, "fdopen failed");
      return false;
    }
    *out_fp = fp;
    return true;
  }
  tmp_path[0] = '\0';
  snprintf(err_buf, err_cap, "could not allocate unique temporary download file");
  return false;
}

static bool loopback_http_download(const http_job *j, ccord_plugin_http_dl_resp *out)
{
  memset(out, 0, sizeof *out);
  struct stat dst_st;
  if (lstat(j->dest_path, &dst_st) == 0 && S_ISLNK(dst_st.st_mode)) {
    snprintf(out->error, sizeof out->error, "destination path is a symbolic link");
    return false;
  }

  char cur_url[2048];
  snprintf(cur_url, sizeof cur_url, "%s", j->url);
  bool strip_creds = false;
  uint8_t max_hops = j->follow_redirects ? PW_MAX_REDIRECTS : 0u;
  size_t lim = j->max_bytes ? j->max_bytes : PW_DEFAULT_DL_BYTES;
  uint32_t tmo = j->timeout_ms ? j->timeout_ms : 30000u;

  for (uint8_t hop = 0; hop <= max_hops; hop++) {
    char scheme[16] = "", host[256] = "", path[1024] = "";
    int port = 80;
    if (!pw_extract_url_host_port(cur_url, scheme, sizeof scheme, host, sizeof host, &port, path, sizeof path) ||
        !valid_http_path(path)) {
      snprintf(out->error, sizeof out->error, "invalid URL");
      return false;
    }
    char tmp_path[CCORD_PATH_MAX + 64];
    FILE *fp = nullptr;
    if (!lb_dl_open_sibling_tmp(j->dest_path, tmp_path, sizeof tmp_path, &fp, out->error, sizeof out->error)) {
      return false;
    }
    int fd = tcp_connect_timeout(host, port, tmo);
    if (fd < 0) {
      fclose(fp);
      (void)unlink(tmp_path);
      snprintf(out->error, sizeof out->error, "connect failed");
      return false;
    }
    struct timeval tv = {.tv_sec = (time_t)(tmo / 1000u), .tv_usec = (suseconds_t)((tmo % 1000u) * 1000u)};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    if (!lb_send_http_request(fd, host, port, path, "GET", j->headers_json, strip_creds, nullptr, 0,
                              out->error, sizeof out->error)) {
      close(fd);
      fclose(fp);
      (void)unlink(tmp_path);
      return false;
    }

    ccord_buf hbuf; ccord_buf_init(&hbuf, 0);
    bool found_hdr = false, overflow = false, write_err = false;
    size_t written = 0;
    long status = 0;
    char ctype[128] = "", loc[2048] = "";
    char rbuf[16384];
    for (;;) {
      ssize_t n = recv(fd, rbuf, sizeof rbuf, 0);
      if (n <= 0) break;
      if (!found_hdr) {
        if (hbuf.len + (size_t)n > 65536u + 16384u) { overflow = true; break; }
        if (ccord_buf_append(&hbuf, rbuf, (size_t)n) != CCORD_OK) { write_err = true; break; }
        size_t hend = 0;
        for (size_t i = 0; i + 3 < hbuf.len; i++) {
          if (hbuf.data[i] == '\r' && hbuf.data[i + 1] == '\n' &&
              hbuf.data[i + 2] == '\r' && hbuf.data[i + 3] == '\n') {
            hend = i;
            found_hdr = true;
            break;
          }
        }
        if (!found_hdr) {
          if (hbuf.len > 65536u) { overflow = true; break; }
          continue;
        }
        int st_i = 0;
        (void)sscanf(ccord_buf_cstr(&hbuf), "HTTP/%*d.%*d %d", &st_i);
        status = st_i;
        const char *sep = hbuf.data + hend;
        const char *line = strstr(hbuf.data, "\r\n");
        if (line && line < sep) line += 2;
        else line = sep;
        while (line < sep) {
          const char *eol = strstr(line, "\r\n");
          if (!eol || eol > sep) eol = sep;
          const char *col = memchr(line, ':', (size_t)(eol - line));
          if (col && col > line) {
            size_t kl = (size_t)(col - line);
            const char *vs = col + 1;
            while (vs < eol && (*vs == ' ' || *vs == '\t')) vs++;
            size_t vl = (size_t)(eol - vs);
            while (vl > 0 && (vs[vl - 1] == ' ' || vs[vl - 1] == '\t')) vl--;
            if (kl == 12u && strncasecmp(line, "content-type", 12) == 0 && vl > 0) {
              size_t cl = vl < sizeof ctype - 1u ? vl : sizeof ctype - 1u;
              for (size_t i = 0; i < cl; i++) ctype[i] = (char)tolower((unsigned char)vs[i]);
              ctype[cl] = '\0';
            } else if (kl == 8u && strncasecmp(line, "location", 8) == 0 && vl > 0 && vl < sizeof loc) {
              memcpy(loc, vs, vl);
              loc[vl] = '\0';
            }
          }
          line = eol < sep ? eol + 2 : sep;
        }
        size_t rem = hbuf.len - (hend + 4u);
        if (rem > 0) {
          if (rem > lim) { overflow = true; break; }
          if (fwrite(hbuf.data + hend + 4u, 1, rem, fp) != rem) { write_err = true; break; }
          written += rem;
        }
        ccord_buf_free(&hbuf);
      } else {
        if ((size_t)n > lim || written > lim - (size_t)n) { overflow = true; break; }
        if (fwrite(rbuf, 1, (size_t)n, fp) != (size_t)n) { write_err = true; break; }
        written += (size_t)n;
      }
    }
    ccord_buf_free(&hbuf);
    close(fd);
    if (fflush(fp) != 0 || ferror(fp)) write_err = true;
    if (fclose(fp) != 0) write_err = true;

    if (overflow) {
      (void)unlink(tmp_path);
      snprintf(out->error, sizeof out->error, "download exceeded maximum size");
      return false;
    }
    if (!found_hdr) {
      (void)unlink(tmp_path);
      snprintf(out->error, sizeof out->error, "malformed or empty HTTP response");
      return false;
    }
    if (j->follow_redirects && lb_is_redirect_status(status) && loc[0]) {
      (void)unlink(tmp_path);
      if (hop == max_hops) {
        snprintf(out->error, sizeof out->error, "too many redirects");
        return false;
      }
      char next_url[2048] = "";
      if (!lb_resolve_redirect_url(scheme, host, port, path, loc, next_url, sizeof next_url)) {
        snprintf(out->error, sizeof out->error, "redirect URL too long");
        return false;
      }
      if (!http_job_allow_url_const(j, next_url, out->error, sizeof out->error)) {
        return false;
      }
      char nscheme[16] = "", nhost[256] = "", npath[1024] = "";
      int nport = 80;
      if (!pw_extract_url_host_port(next_url, nscheme, sizeof nscheme, nhost, sizeof nhost, &nport, npath, sizeof npath) ||
          strcmp(nscheme, "http") != 0) {
        snprintf(out->error, sizeof out->error, "redirect target unsupported in loopback mode");
        return false;
      }
      if (ccord_strcasecmp(host, nhost) != 0 || port != nport) strip_creds = true;
      snprintf(cur_url, sizeof cur_url, "%s", next_url);
      continue;
    }

    out->status_code = status;
    if (ctype[0]) snprintf(out->content_type, sizeof out->content_type, "%s", ctype);
    if (hop > 0) snprintf(out->final_url, sizeof out->final_url, "%s", cur_url);
    if (status < 200 || status >= 300) {
      (void)unlink(tmp_path);
      out->bytes_written = 0;
      return true;
    }
    if (write_err) {
      (void)unlink(tmp_path);
      snprintf(out->error, sizeof out->error, "failed writing downloaded file to disk");
      return false;
    }
    if (lstat(j->dest_path, &dst_st) == 0 && S_ISLNK(dst_st.st_mode)) {
      (void)unlink(tmp_path);
      snprintf(out->error, sizeof out->error, "destination path is a symbolic link");
      return false;
    }
    if (rename(tmp_path, j->dest_path) != 0) {
      int e = errno;
      (void)unlink(tmp_path);
      snprintf(out->error, sizeof out->error, "cannot move downloaded file to destination: %s", strerror(e));
      return false;
    }
    out->bytes_written = written;
    return true;
  }
  snprintf(out->error, sizeof out->error, "too many redirects");
  return false;
}
#endif

// ---------------------------------------------------------------- Async HTTP job

static void *http_worker_fn(void *arg)
{
  http_job *j = arg;
  plugin_workers *w = j->w;
  char scheme[16] = "", host[256] = "", path[1024] = "";
  int port = 80;
  bool url_ok = pw_extract_url_host_port(j->url, scheme, sizeof scheme, host, sizeof host, &port, path, sizeof path);
  bool is_loc = url_ok && pw_is_localhost_host(host);

  if (j->dest_path[0]) {
    // Streaming file download mode
    ccord_plugin_http_dl_resp dl = {0};
    bool ok = false;
    if (!url_ok) {
      snprintf(dl.error, sizeof dl.error, "invalid URL");
    } else if (is_loc && !j->allow_local) {
      snprintf(dl.error, sizeof dl.error, "localhost access requires network.local permission");
    } else if (w->ops.http_download) {
      ccord_plugin_http_dl_req req = {
        .url = j->url,
        .dest_path = j->dest_path,
        .headers_json = j->headers_json,
        .timeout_ms = j->timeout_ms ? j->timeout_ms : 30000u,
        .max_bytes = j->max_bytes ? j->max_bytes : PW_DEFAULT_DL_BYTES,
        .allow_localhost_http = (j->allow_local && is_loc),
        .follow_redirects = j->follow_redirects,
        .max_redirects = j->follow_redirects ? PW_MAX_REDIRECTS : 0u,
        .allow_url = http_job_allow_url,
        .allow_url_ud = j,
      };
      ok = w->ops.http_download(w->ops.ud, &req, &dl);
    } else if (is_loc && strcmp(scheme, "http") == 0) {
#if !CCORD_OS_WINDOWS
      ok = loopback_http_download(j, &dl);
#else
      snprintf(dl.error, sizeof dl.error, "loopback HTTP download unsupported on this build");
#endif
    } else {
      snprintf(dl.error, sizeof dl.error, "HTTP download unavailable in offline build");
    }

    if (!atomic_load(&w->shutting_down)) {
      pw_event ev = {.kind = PW_EV_ASYNC_REPLY, .id = j->req_id, .ok = ok};
      snprintf(ev.owner, sizeof ev.owner, "%s", j->owner);
      if (ok) {
        ccord_buf out; ccord_buf_init(&out, 0);
        ccord_jw jw; ccord_jw_init(&jw, &out);
        ccord_jw_begin_object(&jw);
        ccord_jw_key(&jw, "status"); ccord_jw_int(&jw, (int64_t)dl.status_code);
        ccord_jw_key(&jw, "ok"); ccord_jw_bool(&jw, dl.status_code >= 200 && dl.status_code < 300);
        ccord_jw_key(&jw, "bytes"); ccord_jw_int(&jw, (int64_t)dl.bytes_written);
        if (dl.content_type[0]) {
          ccord_jw_key(&jw, "content_type"); ccord_jw_cstr(&jw, dl.content_type);
        }
        if (dl.final_url[0]) {
          ccord_jw_key(&jw, "final_url"); ccord_jw_cstr(&jw, dl.final_url);
        }
        ccord_jw_end_object(&jw);
        if (ccord_jw_ok(&jw)) ev.json = ccord_strndup(ccord_buf_cstr(&out), out.len);
        ccord_buf_free(&out);
      } else {
        snprintf(ev.error, sizeof ev.error, "%s", dl.error[0] ? dl.error : "http download error");
      }
      pw_push_ev(w, &ev);
    }
  } else {
    // In-memory HTTP request mode (text or binary-safe, chunked on main thread if > 512 KiB)
    ccord_plugin_http_resp resp = {0};
    bool ok = false;
    if (!url_ok) {
      snprintf(resp.error, sizeof resp.error, "invalid URL");
    } else if (is_loc && !j->allow_local) {
      snprintf(resp.error, sizeof resp.error, "localhost access requires network.local permission");
    } else if (w->ops.http_request) {
      ccord_plugin_http_req req = {
        .url = j->url, .method = j->method, .headers_json = j->headers_json,
        .body = j->body, .body_len = j->body_len,
        .timeout_ms = j->timeout_ms ? j->timeout_ms : 15000u,
        .max_bytes = j->max_bytes ? j->max_bytes : PW_DEFAULT_HTTP_BODY,
        .allow_localhost_http = (j->allow_local && is_loc),
        .follow_redirects = j->follow_redirects,
        .max_redirects = j->follow_redirects ? PW_MAX_REDIRECTS : 0u,
        .allow_url = http_job_allow_url,
        .allow_url_ud = j,
      };
      ok = w->ops.http_request(w->ops.ud, &req, &resp);
    } else if (is_loc && strcmp(scheme, "http") == 0) {
#if !CCORD_OS_WINDOWS
      ok = loopback_http_do(j, &resp);
#else
      snprintf(resp.error, sizeof resp.error, "loopback HTTP unsupported on this build");
#endif
    } else {
      snprintf(resp.error, sizeof resp.error, "HTTP client unavailable in offline build");
    }

    if (!atomic_load(&w->shutting_down)) {
      pw_event ev = {
        .kind = PW_EV_HTTP_REPLY,
        .id = j->req_id,
        .ok = ok,
        .http_status = resp.status_code,
        .http_binary = j->binary,
      };
      snprintf(ev.owner, sizeof ev.owner, "%s", j->owner);
      if (ok) {
        ev.raw_body = resp.body;
        ev.raw_body_len = resp.body_len;
        resp.body = nullptr;
        ev.json = resp.headers_json;
        resp.headers_json = nullptr;
        if (resp.final_url[0]) ev.final_url = ccord_strdup(resp.final_url);
      } else {
        snprintf(ev.error, sizeof ev.error, "%s", resp.error[0] ? resp.error : "http error");
      }
      pw_push_ev(w, &ev);
    }
    ccord_free(resp.body);
    ccord_free(resp.headers_json);
  }

  ccord_free(j->headers_json);
  ccord_free(j->body);
  ccord_free(j);
  atomic_fetch_sub(&w->active_jobs, 1);
  return nullptr;
}

static bool http_job_validate_initial(plugin_workers *w, const char *owner, uint64_t req_id,
                                      const ccord_manifest *manifest, const char *url,
                                      bool local_only, bool allow_local,
                                      bool *out_https, bool *out_loopback)
{
  char scheme[16] = "", host[256] = "", path[1024] = "";
  int port = 80;
  if (!pw_extract_url_host_port(url, scheme, sizeof scheme, host, sizeof host, &port, path, sizeof path)) {
    pw_event ev = {.kind = PW_EV_ASYNC_REPLY, .id = req_id, .ok = false};
    snprintf(ev.owner, sizeof ev.owner, "%s", owner);
    snprintf(ev.error, sizeof ev.error, "invalid URL");
    pw_push_ev(w, &ev);
    return false;
  }
  bool is_loc = pw_is_localhost_host(host);
  if (is_loc && !allow_local) {
    pw_event ev = {.kind = PW_EV_ASYNC_REPLY, .id = req_id, .ok = false};
    snprintf(ev.owner, sizeof ev.owner, "%s", owner);
    snprintf(ev.error, sizeof ev.error, "localhost access requires network.local permission");
    pw_push_ev(w, &ev);
    return false;
  }
  if (strcmp(scheme, "https") != 0 && !(strcmp(scheme, "http") == 0 && is_loc)) {
    pw_event ev = {.kind = PW_EV_ASYNC_REPLY, .id = req_id, .ok = false};
    snprintf(ev.owner, sizeof ev.owner, "%s", owner);
    snprintf(ev.error, sizeof ev.error, "only https:// (or localhost http://) is allowed");
    pw_push_ev(w, &ev);
    return false;
  }
  if (local_only && !is_loc) {
    pw_event ev = {.kind = PW_EV_ASYNC_REPLY, .id = req_id, .ok = false};
    snprintf(ev.owner, sizeof ev.owner, "%s", owner);
    snprintf(ev.error, sizeof ev.error, "network.local only allows localhost/127.0.0.1");
    pw_push_ev(w, &ev);
    return false;
  }
  if (!is_loc && manifest && !ccord_manifest_host_allowed(manifest, host)) {
    pw_event ev = {.kind = PW_EV_ASYNC_REPLY, .id = req_id, .ok = false};
    snprintf(ev.owner, sizeof ev.owner, "%s", owner);
    snprintf(ev.error, sizeof ev.error, "host '%.180s' not permitted by plugin.json allowed_hosts", host);
    pw_push_ev(w, &ev);
    return false;
  }
  *out_https = strcmp(scheme, "https") == 0;
  *out_loopback = is_loc;
  return true;
}

static void http_job_copy_host_policy(http_job *j, const ccord_manifest *manifest)
{
  if (!manifest) return;
  j->has_host_policy = true;
  j->host_policy.allowed_host_count = manifest->allowed_host_count;
  if (j->host_policy.allowed_host_count > CCORD_MANIFEST_MAX_HOSTS) {
    j->host_policy.allowed_host_count = CCORD_MANIFEST_MAX_HOSTS;
  }
  for (size_t i = 0; i < j->host_policy.allowed_host_count; i++) {
    snprintf(j->host_policy.allowed_hosts[i], sizeof j->host_policy.allowed_hosts[i], "%s",
             manifest->allowed_hosts[i]);
  }
}

bool pw_start_http(plugin_workers *w, const char *owner, uint64_t req_id, const ccord_manifest *manifest,
                   const char *url, const char *method, const char *headers_json,
                   const uint8_t *body, size_t body_len, uint32_t timeout_ms, size_t max_bytes,
                   bool binary, bool follow_redirects, bool local_only, bool allow_local)
{
  if (!w || !owner || !url) return false;
  if (strlen(url) >= 2048u) return false;
  const char *eff_method = (method && method[0]) ? method : "GET";
  if (!valid_http_method(eff_method)) {
    pw_event ev = {.kind = PW_EV_ASYNC_REPLY, .id = req_id, .ok = false};
    snprintf(ev.owner, sizeof ev.owner, "%s", owner);
    snprintf(ev.error, sizeof ev.error, "invalid HTTP method");
    pw_push_ev(w, &ev);
    return true;
  }
  bool init_https = false, init_lb = false;
  if (!http_job_validate_initial(w, owner, req_id, manifest, url, local_only, allow_local, &init_https, &init_lb)) {
    return true;
  }
  http_job *j = ccord_calloc(1, sizeof *j);
  if (!j) return false;
  j->w = w;
  snprintf(j->owner, sizeof j->owner, "%s", owner);
  j->req_id = req_id;
  snprintf(j->url, sizeof j->url, "%s", url);
  snprintf(j->method, sizeof j->method, "%s", eff_method);
  if (headers_json && headers_json[0]) j->headers_json = ccord_strdup(headers_json);
  if (body && body_len > 0) {
    j->body = ccord_malloc(body_len);
    if (j->body) { memcpy(j->body, body, body_len); j->body_len = body_len; }
  }
  j->timeout_ms = timeout_ms;
  size_t lim = max_bytes ? max_bytes : PW_DEFAULT_HTTP_BODY;
  if (lim > PW_MAX_HTTP_BODY_CAP) lim = PW_MAX_HTTP_BODY_CAP;
  j->max_bytes = lim;
  j->binary = binary;
  j->follow_redirects = follow_redirects;
  j->local_only = local_only;
  j->allow_local = allow_local;
  j->initial_https = init_https;
  j->initial_loopback = init_lb;
  http_job_copy_host_policy(j, manifest);

  atomic_fetch_add(&w->active_jobs, 1);
  pthread_t t;
  if (pthread_create(&t, nullptr, http_worker_fn, j) != 0) {
    atomic_fetch_sub(&w->active_jobs, 1);
    ccord_free(j->headers_json); ccord_free(j->body); ccord_free(j);
    return false;
  }
  pthread_detach(t);
  return true;
}

bool pw_start_http_download(plugin_workers *w, const char *owner, uint64_t req_id, const ccord_manifest *manifest,
                            const char *url, const char *dest_path, const char *headers_json,
                            uint32_t timeout_ms, size_t max_bytes, bool follow_redirects,
                            bool local_only, bool allow_local)
{
  if (!w || !owner || !url || !dest_path || !dest_path[0]) return false;
  if (strlen(url) >= 2048u || strlen(dest_path) >= CCORD_PATH_MAX) return false;
  bool init_https = false, init_lb = false;
  if (!http_job_validate_initial(w, owner, req_id, manifest, url, local_only, allow_local, &init_https, &init_lb)) {
    return true;
  }
  http_job *j = ccord_calloc(1, sizeof *j);
  if (!j) return false;
  j->w = w;
  snprintf(j->owner, sizeof j->owner, "%s", owner);
  j->req_id = req_id;
  snprintf(j->url, sizeof j->url, "%s", url);
  snprintf(j->method, sizeof j->method, "GET");
  snprintf(j->dest_path, sizeof j->dest_path, "%s", dest_path);
  if (headers_json && headers_json[0]) j->headers_json = ccord_strdup(headers_json);
  uint32_t tmo = timeout_ms ? timeout_ms : 30000u;
  if (tmo < 1000u) tmo = 1000u;
  if (tmo > 300000u) tmo = 300000u;
  j->timeout_ms = tmo;
  size_t lim = max_bytes ? max_bytes : PW_DEFAULT_DL_BYTES;
  if (lim > PW_MAX_DL_BYTES) lim = PW_MAX_DL_BYTES;
  j->max_bytes = lim;
  j->follow_redirects = follow_redirects;
  j->local_only = local_only;
  j->allow_local = allow_local;
  j->initial_https = init_https;
  j->initial_loopback = init_lb;
  http_job_copy_host_policy(j, manifest);

  atomic_fetch_add(&w->active_jobs, 1);
  pthread_t t;
  if (pthread_create(&t, nullptr, http_worker_fn, j) != 0) {
    atomic_fetch_sub(&w->active_jobs, 1);
    ccord_free(j->headers_json); ccord_free(j);
    return false;
  }
  pthread_detach(t);
  return true;
}

// ---------------------------------------------------------------- Async Unix socket send

static bool strstr_ci(const char *hay, const char *needle)
{
  if (!hay || !needle || !needle[0]) return false;
  size_t nlen = strlen(needle);
  for (const char *h = hay; *h; h++) {
    size_t i = 0;
    while (i < nlen && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) i++;
    if (i == nlen) return true;
  }
  return false;
}

static bool unix_path_matches_blocklist(const char *p)
{
  if (!p || !p[0]) return true;
  static const char *const k_blocked[] = {
    "docker.sock",
    "podman.sock",
    "containerd.sock",
    "dbus",
    "systemd",
    "ssh-agent",
    "agent.",
    "gpg-agent",
    "s.gpg-agent",
    "discord-ipc-",
    "ccord-",
  };
  for (size_t i = 0; i < sizeof k_blocked / sizeof k_blocked[0]; i++) {
    if (strstr_ci(p, k_blocked[i])) return true;
  }
  return false;
}

static bool unix_path_eq_resolved(const char *a, const char *a_real, const char *b)
{
  if (!b || !b[0]) return false;
  if (strcmp(a, b) == 0 || (a_real && a_real[0] && strcmp(a_real, b) == 0)) return true;
#if !CCORD_OS_WINDOWS
  char b_real[CCORD_PATH_MAX];
  if (realpath(b, b_real)) {
    if (strcmp(a, b_real) == 0 || (a_real && a_real[0] && strcmp(a_real, b_real) == 0)) return true;
  }
#endif
  return false;
}

bool pw_unix_socket_path_is_blocked(const char *sock_path)
{
  if (!sock_path || !sock_path[0] || sock_path[0] == '@') return true;
  for (const unsigned char *q = (const unsigned char *)sock_path; *q; q++) {
    if (*q < 0x20u || *q == 0x7fu) return true;
  }
  // Reject ".." path traversal segments.
  for (const char *seg = sock_path; *seg;) {
    const char *end = seg;
    while (*end && *end != '/' && *end != '\\') end++;
    if ((size_t)(end - seg) == 2u && seg[0] == '.' && seg[1] == '.') return true;
    seg = *end ? end + 1 : end;
  }
  if (unix_path_matches_blocklist(sock_path)) return true;

  const char *resolved = nullptr;
#if !CCORD_OS_WINDOWS
  char rbuf[CCORD_PATH_MAX];
  if (realpath(sock_path, rbuf)) {
    resolved = rbuf;
    if (unix_path_matches_blocklist(resolved)) return true;
  }
#endif

  const char *ssh_sock = getenv("SSH_AUTH_SOCK");
  if (ssh_sock && ssh_sock[0] && unix_path_eq_resolved(sock_path, resolved, ssh_sock)) return true;

  const char *dbus_addr = getenv("DBUS_SESSION_BUS_ADDRESS");
  if (dbus_addr && dbus_addr[0]) {
    const char *p = dbus_addr;
    while ((p = strstr(p, "path=")) != nullptr) {
      p += 5;
      const char *end = p;
      while (*end && *end != ',' && *end != ';') end++;
      size_t plen = (size_t)(end - p);
      if (plen > 0 && plen < CCORD_PATH_MAX) {
        char dpath[CCORD_PATH_MAX];
        memcpy(dpath, p, plen);
        dpath[plen] = '\0';
        if (unix_path_eq_resolved(sock_path, resolved, dpath)) return true;
      }
      p = end;
    }
  }
  return false;
}

typedef struct unix_job {
  plugin_workers *w;
  char owner[65];
  uint64_t req_id;
  char path[256];
  uint8_t *data;
  size_t len;
} unix_job;

static void *unix_worker_fn(void *arg)
{
  unix_job *j = arg;
  plugin_workers *w = j->w;
  bool ok = false;
  char ebuf[128] = "unix socket unsupported";
  ccord_buf resp; ccord_buf_init(&resp, 0);
#if !CCORD_OS_WINDOWS
  struct stat ust;
  if (pw_unix_socket_path_is_blocked(j->path)) {
    snprintf(ebuf, sizeof ebuf, "unix socket path is blocked");
  } else if (lstat(j->path, &ust) == 0 && S_ISLNK(ust.st_mode)) {
    snprintf(ebuf, sizeof ebuf, "symbolic link unix socket paths are not allowed");
  } else {
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    size_t plen = strlen(j->path);
    if (plen >= sizeof sa.sun_path) {
      snprintf(ebuf, sizeof ebuf, "unix socket path too long");
    } else {
      int fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0) {
        snprintf(ebuf, sizeof ebuf, "unix socket creation failed");
      } else {
        ccord_sock_set_nosigpipe(fd);
        memcpy(sa.sun_path, j->path, plen + 1u);
        struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
          size_t off = 0;
          bool wr_ok = true;
          while (off < j->len) {
            ssize_t n = send(fd, j->data + off, j->len - off, PW_SEND_FLAGS);
            if (n <= 0) { wr_ok = false; break; }
            off += (size_t)n;
          }
          if (wr_ok) {
            (void)shutdown(fd, SHUT_WR);
            char tmp[4096];
            for (;;) {
              ssize_t n = recv(fd, tmp, sizeof tmp, 0);
              if (n <= 0) break;
              if (resp.len + (size_t)n > 256u * 1024u) break;
              (void)ccord_buf_append(&resp, tmp, (size_t)n);
            }
            ok = true;
          } else {
            snprintf(ebuf, sizeof ebuf, "unix send failed");
          }
        } else {
          snprintf(ebuf, sizeof ebuf, "unix connect failed");
        }
        close(fd);
      }
    }
  }
#endif
  if (!atomic_load(&w->shutting_down)) {
    pw_event ev = {.kind = PW_EV_ASYNC_REPLY, .id = j->req_id, .ok = ok};
    snprintf(ev.owner, sizeof ev.owner, "%s", j->owner);
    if (ok) {
      ccord_buf out; ccord_buf_init(&out, 0); ccord_jw jw; ccord_jw_init(&jw, &out);
      ccord_jw_begin_object(&jw);
      ccord_jw_key(&jw, "data"); ccord_jw_string(&jw, resp.data ? resp.data : "", resp.len);
      ccord_jw_end_object(&jw);
      if (ccord_jw_ok(&jw)) ev.json = ccord_strndup(ccord_buf_cstr(&out), out.len);
      ccord_buf_free(&out);
    } else {
      snprintf(ev.error, sizeof ev.error, "%s", ebuf);
    }
    pw_push_ev(w, &ev);
  }
  ccord_buf_free(&resp);
  ccord_free(j->data);
  ccord_free(j);
  atomic_fetch_sub(&w->active_jobs, 1);
  return nullptr;
}

bool pw_start_unix_send(plugin_workers *w, const char *owner, uint64_t req_id, const char *sock_path,
                        const uint8_t *data, size_t len)
{
  if (!w || !owner || !sock_path) return false;
  if (pw_unix_socket_path_is_blocked(sock_path)) {
    pw_event ev = {.kind = PW_EV_ASYNC_REPLY, .id = req_id, .ok = false};
    snprintf(ev.owner, sizeof ev.owner, "%s", owner);
    snprintf(ev.error, sizeof ev.error, "unix socket path is blocked");
    pw_push_ev(w, &ev);
    return true;
  }
  if (strlen(sock_path) >= 256u) return false;
  unix_job *j = ccord_calloc(1, sizeof *j);
  if (!j) return false;
  j->w = w; snprintf(j->owner, sizeof j->owner, "%s", owner); j->req_id = req_id;
  snprintf(j->path, sizeof j->path, "%s", sock_path);
  if (data && len) { j->data = ccord_malloc(len); if (j->data) { memcpy(j->data, data, len); j->len = len; } }
  atomic_fetch_add(&w->active_jobs, 1);
  pthread_t t;
  if (pthread_create(&t, nullptr, unix_worker_fn, j) != 0) {
    atomic_fetch_sub(&w->active_jobs, 1); ccord_free(j->data); ccord_free(j); return false;
  }
  pthread_detach(t);
  return true;
}

// ---------------------------------------------------------------- WebSocket worker (RFC 6455 loopback + ops.ws_* for wss://)

#if !CCORD_OS_WINDOWS
bool pw_ws_send_tcp_frame(int fd, uint8_t opcode, const uint8_t *data, size_t len)
{
  uint8_t hdr[14];
  size_t hl = 0;
  hdr[hl++] = (uint8_t)(0x80u | (opcode & 0x0fu));
  if (len < 126u) {
    hdr[hl++] = (uint8_t)(0x80u | len);
  } else if (len <= 65535u) {
    hdr[hl++] = 0x80u | 126u;
    hdr[hl++] = (uint8_t)((len >> 8) & 0xffu);
    hdr[hl++] = (uint8_t)(len & 0xffu);
  } else {
    hdr[hl++] = 0x80u | 127u;
    for (int k = 7; k >= 0; k--) hdr[hl++] = (uint8_t)((len >> (k * 8)) & 0xffu);
  }
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  (void)ccord_random_bytes(mask, sizeof mask);
  memcpy(hdr + hl, mask, 4); hl += 4;
  uint8_t *buf = ccord_malloc(hl + len);
  if (!buf) return false;
  memcpy(buf, hdr, hl);
  for (size_t i = 0; i < len; i++) buf[hl + i] = data[i] ^ mask[i & 3u];
  size_t off = 0;
  bool ok = true;
  while (off < hl + len) {
    ssize_t n = send(fd, buf + off, hl + len - off, PW_SEND_FLAGS);
    if (n <= 0) { ok = false; break; }
    off += (size_t)n;
  }
  ccord_free(buf);
  return ok;
}

static bool recv_exact(int fd, int wake_fd, uint8_t *dst, size_t n,
                       const _Atomic bool *stop, const _Atomic bool *shutting_down)
{
  size_t got = 0;
  while (got < n && !atomic_load(stop) && !atomic_load(shutting_down)) {
    struct pollfd pfs[2];
    nfds_t np = 0;
    pfs[np++] = (struct pollfd){.fd = fd, .events = POLLIN};
    if (wake_fd >= 0) pfs[np++] = (struct pollfd){.fd = wake_fd, .events = POLLIN};
    int pr = poll(pfs, np, wake_fd >= 0 ? -1 : 100);
    if (pr == 0) continue;
    if (pr < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (np > 1 && (pfs[1].revents & (POLLIN | POLLHUP | POLLERR))) return false;
    if (!(pfs[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
    ssize_t r = recv(fd, dst + got, n - got, 0);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return got == n;
}
#endif

static void ws_emit(plugin_workers *w, const char *owner, uint64_t ws_id, const char *sub_kind, const char *json)
{
  if (atomic_load(&w->shutting_down)) return;
  pw_event ev = {.kind = PW_EV_WS, .id = ws_id, .ok = true};
  snprintf(ev.owner, sizeof ev.owner, "%s", owner);
  snprintf(ev.sub_kind, sizeof ev.sub_kind, "%s", sub_kind);
  ev.json = ccord_strdup(json ? json : "{}");
  pw_push_ev(w, &ev);
}

static void *ws_worker_fn(void *arg)
{
  pw_ws_slot *s = arg;
  plugin_workers *w = s->parent;
  char scheme[16] = "", host[256] = "", path[1024] = "";
  int port = 80;
  if (!pw_extract_url_host_port(s->url, scheme, sizeof scheme, host, sizeof host, &port, path, sizeof path)) {
    ws_emit(w, s->owner, s->ws_id, "error", "{\"message\":\"invalid WebSocket URL\"}");
    ws_emit(w, s->owner, s->ws_id, "close", "{\"code\":1006,\"reason\":\"invalid URL\"}");
    return nullptr;
  }
  bool is_loc = pw_is_localhost_host(host);
  if (is_loc && !s->allow_local) {
    ws_emit(w, s->owner, s->ws_id, "error", "{\"message\":\"localhost access requires network.local permission\"}");
    ws_emit(w, s->owner, s->ws_id, "close", "{\"code\":1008,\"reason\":\"localhost access requires network.local permission\"}");
    return nullptr;
  }
  if (strcmp(scheme, "wss") == 0 && w->ops.ws_open) {
    char ebuf[256] = "";
    void *h = w->ops.ws_open(w->ops.ud, s->url, s->headers_json, ebuf, sizeof ebuf);
    if (!h) {
      ccord_buf b; ccord_buf_init(&b, 0); ccord_jw jw; ccord_jw_init(&jw, &b);
      ccord_jw_begin_object(&jw); ccord_jw_key(&jw, "message"); ccord_jw_cstr(&jw, ebuf[0] ? ebuf : "connect failed"); ccord_jw_end_object(&jw);
      ws_emit(w, s->owner, s->ws_id, "error", ccord_buf_cstr(&b));
      ws_emit(w, s->owner, s->ws_id, "close", "{\"code\":1006,\"reason\":\"connect failed\"}");
      ccord_buf_free(&b);
      return nullptr;
    }
    pthread_mutex_lock(&s->send_mu);
    s->ext_handle = h;
    pthread_mutex_unlock(&s->send_mu);
    ws_emit(w, s->owner, s->ws_id, "open", "{}");
    uint8_t *buf = ccord_malloc(65536u);
    while (!atomic_load(&s->stop) && !atomic_load(&w->shutting_down) && buf) {
      bool bin = false;
      int n = w->ops.ws_recv(w->ops.ud, h, buf, 65536u, &bin, 100);
      if (n == 0) continue;
      if (n < 0) break;
      ccord_buf out; ccord_buf_init(&out, 0); ccord_jw jw; ccord_jw_init(&jw, &out);
      ccord_jw_begin_object(&jw);
      ccord_jw_key(&jw, "binary"); ccord_jw_bool(&jw, bin);
      if (bin) {
        ccord_buf b64; ccord_buf_init(&b64, ((size_t)n * 4u) / 3u + 8u);
        pw_b64_encode(buf, (size_t)n, &b64);
        ccord_jw_key(&jw, "b64"); ccord_jw_string(&jw, b64.data ? b64.data : "", b64.len);
        ccord_buf_free(&b64);
      } else {
        ccord_jw_key(&jw, "data"); ccord_jw_string(&jw, (const char *)buf, (size_t)n);
      }
      ccord_jw_end_object(&jw);
      if (ccord_jw_ok(&jw)) ws_emit(w, s->owner, s->ws_id, "message", ccord_buf_cstr(&out));
      ccord_buf_free(&out);
    }
    ccord_free(buf);
    pthread_mutex_lock(&s->send_mu);
    if (s->ext_handle) { w->ops.ws_close(w->ops.ud, s->ext_handle, 1000, ""); s->ext_handle = nullptr; }
    pthread_mutex_unlock(&s->send_mu);
    ws_emit(w, s->owner, s->ws_id, "close", "{\"code\":1000,\"reason\":\"closed\"}");
    return nullptr;
  }

#if !CCORD_OS_WINDOWS
  if (strcmp(scheme, "ws") == 0 && is_loc && s->allow_local) {
    if (!valid_http_path(path)) {
      ws_emit(w, s->owner, s->ws_id, "error", "{\"message\":\"invalid WebSocket request path\"}");
      ws_emit(w, s->owner, s->ws_id, "close", "{\"code\":1008,\"reason\":\"invalid path\"}");
      return nullptr;
    }
    int fd = tcp_connect_timeout(host, port, 5000u);
    if (fd < 0) {
      ws_emit(w, s->owner, s->ws_id, "error", "{\"message\":\"connect failed\"}");
      ws_emit(w, s->owner, s->ws_id, "close", "{\"code\":1006,\"reason\":\"connect failed\"}");
      return nullptr;
    }
    uint8_t nonce[16] = {0};
    (void)ccord_random_bytes(nonce, sizeof nonce);
    ccord_buf key_b64; ccord_buf_init(&key_b64, 32u);
    pw_b64_encode(nonce, sizeof nonce, &key_b64);
    ccord_buf hs_buf; ccord_buf_init(&hs_buf, 256u);
    char port_s[16];
    snprintf(port_s, sizeof port_s, "%d", port);
    bool is_v6 = strchr(host, ':') != nullptr;
    (void)ccord_buf_append_str(&hs_buf, "GET ");
    (void)ccord_buf_append_str(&hs_buf, path);
    (void)ccord_buf_append_str(&hs_buf, " HTTP/1.1\r\nHost: ");
    if (is_v6) (void)ccord_buf_append(&hs_buf, "[", 1);
    (void)ccord_buf_append_str(&hs_buf, host);
    if (is_v6) (void)ccord_buf_append(&hs_buf, "]", 1);
    (void)ccord_buf_append(&hs_buf, ":", 1);
    (void)ccord_buf_append_str(&hs_buf, port_s);
    (void)ccord_buf_append_str(&hs_buf, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ");
    (void)ccord_buf_append_str(&hs_buf, ccord_buf_cstr(&key_b64));
    (void)ccord_buf_append_str(&hs_buf, "\r\nSec-WebSocket-Version: 13\r\n\r\n");
    ccord_buf_free(&key_b64);
    bool hs_sent = !hs_buf.failed;
    size_t hs_off = 0;
    while (hs_sent && hs_off < hs_buf.len) {
      ssize_t wn = send(fd, hs_buf.data + hs_off, hs_buf.len - hs_off, PW_SEND_FLAGS);
      if (wn <= 0) { hs_sent = false; break; }
      hs_off += (size_t)wn;
    }
    ccord_buf_free(&hs_buf);
    if (!hs_sent) {
      close(fd);
      ws_emit(w, s->owner, s->ws_id, "error", "{\"message\":\"handshake write failed\"}");
      ws_emit(w, s->owner, s->ws_id, "close", "{\"code\":1006,\"reason\":\"handshake failed\"}");
      return nullptr;
    }
    // Read HTTP 101 response header up to \r\n\r\n
    char rbuf[2048];
    size_t rlen = 0;
    bool hs_ok = false;
    while (rlen + 1 < sizeof rbuf && !atomic_load(&s->stop) && !atomic_load(&w->shutting_down)) {
      uint8_t ch = 0;
      if (!recv_exact(fd, s->wake_r, &ch, 1, &s->stop, &w->shutting_down)) break;
      rbuf[rlen++] = (char)ch; rbuf[rlen] = '\0';
      if (rlen >= 4 && memcmp(rbuf + rlen - 4, "\r\n\r\n", 4) == 0) {
        hs_ok = strstr(rbuf, " 101 ") != nullptr;
        break;
      }
    }
    if (!hs_ok) {
      close(fd);
      ws_emit(w, s->owner, s->ws_id, "error", "{\"message\":\"websocket upgrade rejected\"}");
      ws_emit(w, s->owner, s->ws_id, "close", "{\"code\":1006,\"reason\":\"upgrade rejected\"}");
      return nullptr;
    }
    pthread_mutex_lock(&s->send_mu);
    s->tcp_fd = fd;
    pthread_mutex_unlock(&s->send_mu);
    ws_emit(w, s->owner, s->ws_id, "open", "{}");

    uint16_t close_code = 1000;
    while (!atomic_load(&s->stop) && !atomic_load(&w->shutting_down)) {
      uint8_t h2[2];
      if (!recv_exact(fd, s->wake_r, h2, 2, &s->stop, &w->shutting_down)) break;
      uint8_t op = h2[0] & 0x0fu;
      bool masked = (h2[1] & 0x80u) != 0;
      uint64_t plen = h2[1] & 0x7fu;
      if (plen == 126u) {
        uint8_t e2[2];
        if (!recv_exact(fd, s->wake_r, e2, 2, &s->stop, &w->shutting_down)) break;
        plen = ((uint64_t)e2[0] << 8) | (uint64_t)e2[1];
      } else if (plen == 127u) {
        uint8_t e8[8];
        if (!recv_exact(fd, s->wake_r, e8, 8, &s->stop, &w->shutting_down)) break;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | e8[i];
      }
      if (plen > 512u * 1024u) break;
      uint8_t mk[4] = {0};
      if (masked && !recv_exact(fd, s->wake_r, mk, 4, &s->stop, &w->shutting_down)) break;
      uint8_t *pay = plen ? ccord_malloc((size_t)plen + 1u) : nullptr;
      if (plen && (!pay || !recv_exact(fd, s->wake_r, pay, (size_t)plen, &s->stop, &w->shutting_down))) { ccord_free(pay); break; }
      if (pay) {
        if (masked) { for (size_t i = 0; i < (size_t)plen; i++) pay[i] ^= mk[i & 3u]; }
        pay[plen] = 0;
      }
      if (op == 8u) {
        if (pay && plen >= 2) close_code = (uint16_t)(((uint16_t)pay[0] << 8) | pay[1]);
        ccord_free(pay);
        break;
      } else if (op == 9u) {
        pthread_mutex_lock(&s->send_mu);
        if (s->tcp_fd >= 0) (void)pw_ws_send_tcp_frame(s->tcp_fd, 10u, pay, (size_t)plen);
        pthread_mutex_unlock(&s->send_mu);
      } else if (op == 1u || op == 2u) {
        ccord_buf out; ccord_buf_init(&out, 0); ccord_jw jw; ccord_jw_init(&jw, &out);
        ccord_jw_begin_object(&jw);
        ccord_jw_key(&jw, "binary"); ccord_jw_bool(&jw, op == 2u);
        if (op == 2u) {
          ccord_buf b64; ccord_buf_init(&b64, ((size_t)plen * 4u) / 3u + 8u);
          if (pay) pw_b64_encode(pay, (size_t)plen, &b64);
          ccord_jw_key(&jw, "b64"); ccord_jw_string(&jw, b64.data ? b64.data : "", b64.len);
          ccord_buf_free(&b64);
        } else {
          ccord_jw_key(&jw, "data"); ccord_jw_string(&jw, pay ? (const char *)pay : "", (size_t)plen);
        }
        ccord_jw_end_object(&jw);
        if (ccord_jw_ok(&jw)) ws_emit(w, s->owner, s->ws_id, "message", ccord_buf_cstr(&out));
        ccord_buf_free(&out);
      }
      ccord_free(pay);
    }
    pthread_mutex_lock(&s->send_mu);
    if (s->tcp_fd >= 0) { close(s->tcp_fd); s->tcp_fd = -1; }
    pthread_mutex_unlock(&s->send_mu);
    char cbuf[96];
    snprintf(cbuf, sizeof cbuf, "{\"code\":%u,\"reason\":\"closed\"}", (unsigned)close_code);
    ws_emit(w, s->owner, s->ws_id, "close", cbuf);
    return nullptr;
  }
#endif
  ws_emit(w, s->owner, s->ws_id, "error", "{\"message\":\"WebSocket transport unavailable\"}");
  ws_emit(w, s->owner, s->ws_id, "close", "{\"code\":1006,\"reason\":\"unsupported\"}");
  return nullptr;
}

bool pw_ws_connect(plugin_workers *w, const char *owner, uint64_t ws_id, const ccord_manifest *manifest,
                   const char *url, const char *headers_json, bool allow_local)
{
  if (!w || !owner || !url) return false;
  if (strlen(url) >= 1024u) return false;
  char scheme[16] = "", host[256] = "", path[1024] = "";
  int port = 80;
  if (!pw_extract_url_host_port(url, scheme, sizeof scheme, host, sizeof host, &port, path, sizeof path)) return false;
  bool is_loc = pw_is_localhost_host(host);
  if (is_loc && !allow_local) {
    ws_emit(w, owner, ws_id, "error", "{\"message\":\"localhost access requires network.local permission\"}");
    ws_emit(w, owner, ws_id, "close", "{\"code\":1008,\"reason\":\"localhost access requires network.local permission\"}");
    return true;
  }
  if (strcmp(scheme, "wss") != 0 && !(strcmp(scheme, "ws") == 0 && is_loc)) {
    ws_emit(w, owner, ws_id, "error", "{\"message\":\"only wss:// (or localhost ws://) is allowed\"}");
    ws_emit(w, owner, ws_id, "close", "{\"code\":1008,\"reason\":\"invalid scheme\"}");
    return true;
  }
  if (!is_loc && manifest && !ccord_manifest_host_allowed(manifest, host)) {
    ws_emit(w, owner, ws_id, "error", "{\"message\":\"host blocked by allowed_hosts\"}");
    ws_emit(w, owner, ws_id, "close", "{\"code\":1008,\"reason\":\"host blocked\"}");
    return true;
  }
  pw_ws_slot *s = ccord_calloc(1, sizeof *s);
  if (!s) return false;
  s->active = true;
  s->tcp_fd = -1;
#if !CCORD_OS_WINDOWS
  pw_make_wake_pipe(&s->wake_r, &s->wake_w);
#endif
  s->ws_id = ws_id;
  s->allow_local = allow_local;
  s->parent = w;
  snprintf(s->owner, sizeof s->owner, "%s", owner);
  snprintf(s->url, sizeof s->url, "%s", url);
  if (headers_json && headers_json[0]) s->headers_json = ccord_strdup(headers_json);
  pthread_mutex_init(&s->send_mu, nullptr);

  pthread_mutex_lock(&w->slot_mu);
  size_t slot = PW_MAX_WS;
  for (size_t i = 0; i < PW_MAX_WS; i++) {
    if (!w->ws[i]) { slot = i; w->ws[i] = s; break; }
  }
  pthread_mutex_unlock(&w->slot_mu);
  if (slot == PW_MAX_WS) {
#if !CCORD_OS_WINDOWS
    pw_close_wake_pipe(&s->wake_r, &s->wake_w);
#endif
    pthread_mutex_destroy(&s->send_mu); ccord_free(s->headers_json); ccord_free(s);
    return false;
  }
  if (pthread_create(&s->thr, nullptr, ws_worker_fn, s) != 0) {
    pthread_mutex_lock(&w->slot_mu); w->ws[slot] = nullptr; pthread_mutex_unlock(&w->slot_mu);
#if !CCORD_OS_WINDOWS
    pw_close_wake_pipe(&s->wake_r, &s->wake_w);
#endif
    pthread_mutex_destroy(&s->send_mu); ccord_free(s->headers_json); ccord_free(s);
    return false;
  }
  return true;
}

bool pw_ws_send(plugin_workers *w, const char *owner, uint64_t ws_id, const uint8_t *data, size_t len, bool binary)
{
  if (!w || !owner) return false;
  bool ok = false;
  pthread_mutex_lock(&w->slot_mu);
  for (size_t i = 0; i < PW_MAX_WS; i++) {
    pw_ws_slot *s = w->ws[i];
    if (s && s->ws_id == ws_id && strcmp(s->owner, owner) == 0) {
      pthread_mutex_lock(&s->send_mu);
      if (s->ext_handle && w->ops.ws_send) {
        ok = w->ops.ws_send(w->ops.ud, s->ext_handle, data, len, binary);
      }
#if !CCORD_OS_WINDOWS
      else if (s->tcp_fd >= 0) {
        ok = pw_ws_send_tcp_frame(s->tcp_fd, binary ? 2u : 1u, data, len);
      }
#endif
      pthread_mutex_unlock(&s->send_mu);
      break;
    }
  }
  pthread_mutex_unlock(&w->slot_mu);
  return ok;
}

void pw_ws_close(plugin_workers *w, const char *owner, uint64_t ws_id, uint16_t code, const char *reason)
{
  (void)code; (void)reason;
  if (!w || !owner) return;
  pthread_mutex_lock(&w->slot_mu);
  for (size_t i = 0; i < PW_MAX_WS; i++) {
    pw_ws_slot *s = w->ws[i];
    if (s && s->ws_id == ws_id && strcmp(s->owner, owner) == 0) {
      atomic_store(&s->stop, true);
#if !CCORD_OS_WINDOWS
      pthread_mutex_lock(&s->send_mu);
      if (s->tcp_fd >= 0) {
        uint8_t payload[2] = {(uint8_t)(code >> 8), (uint8_t)(code & 0xffu)};
        (void)pw_ws_send_tcp_frame(s->tcp_fd, 8u, payload, 2);
        shutdown(s->tcp_fd, SHUT_RDWR);
      }
      pthread_mutex_unlock(&s->send_mu);
      pw_signal_wake(s->wake_w);
#endif
      break;
    }
  }
  pthread_mutex_unlock(&w->slot_mu);
}

