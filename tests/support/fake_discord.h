// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// A local fake Discord for tests: a forked loopback HTTP server that answers from a callback and reports every
// request it saw (method, path, body, arrival time) back to the test. Tests assert on what *reached the server*, which
// is what Discord would observe, not on what the client believes it sent.
//
//   fd_server srv;
//   fd_start(&srv, respond_fn, &my_state);       // fork; srv.base is "http://127.0.0.1:<port>"
//   ... point the client at srv.base, drive it ...
//   fd_hit hits[32]; size_t n = fd_stop(&srv, hits, 32);   // kills the server, returns what it saw
//
// The callback runs in the child process, so it must not rely on parent state changing after fd_start.
#pragma once
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FD_MAX_HITS 64

typedef struct fd_hit {
  char method[8];
  char path[160];
  char audit[160];        // the X-Audit-Log-Reason header exactly as sent on the wire ("" when absent)
  char content_type[96];  // Content-Type header value
  char authorization[160];  // Authorization header value ("" when absent; truncated to fit)
  bool super_properties;  // an X-Super-Properties header was present
  size_t body_len;        // exact Content-Length received
  char body[640];         // request body (NUL-terminated; binary-copied up to sizeof body - 1)
  uint64_t ms;            // CLOCK_MONOTONIC milliseconds, comparable with the test's own clock
} fd_hit;

// Fills `out` with a full HTTP response and returns its length (0 on overflow). `idx` is the 0-based request number.
typedef size_t (*fd_respond_fn)(void *ud, int idx, const fd_hit *req, char *out, size_t cap);

typedef struct fd_server {
  pid_t pid;
  int pipe_rd;
  int port;
  char base[64];
} fd_server;

[[maybe_unused]] static uint64_t fd_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

[[maybe_unused]] static size_t fd_reply(char *out, size_t cap, int status, const char *reason, const char *extra_headers,
                                        const char *body)
{
  int n = snprintf(out, cap,
                   "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n%sContent-Length: %zu\r\n"
                   "Connection: close\r\n\r\n%s",
                   status, reason, extra_headers ? extra_headers : "", strlen(body), body);
  return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

// Reads one request (headers + Content-Length body) so closing never resets a half-read connection. Returns the
// offset of the body in buf, or 0 on failure.
static size_t fd_read_request(int fd, char *buf, size_t cap, size_t *out_body_len)
{
  size_t len = 0, need = 0, body_at = 0, body_len = 0;
  while (len + 1 < cap) {
    ssize_t n = read(fd, buf + len, cap - 1 - len);
    if (n <= 0) return 0;
    len += (size_t)n;
    buf[len] = '\0';
    char *end = body_at ? (buf + body_at - 4) : strstr(buf, "\r\n\r\n");
    if (!end) continue;
    if (need == 0) {
      for (char *p = buf; p < end;) {
        char *nl = strstr(p, "\r\n");
        if (!nl) break;
        if (strncasecmp(p, "Content-Length:", 15) == 0) body_len = (size_t)strtoul(p + 15, nullptr, 10);
        p = nl + 2;
      }
      body_at = (size_t)(end + 4 - buf);
      need = body_at + body_len;
    }
    if (len >= need) {
      if (out_body_len) *out_body_len = body_len;
      return body_at;
    }
  }
  return 0;
}

static void fd_serve(int lfd, int wfd, fd_respond_fn fn, void *ud)
{
  alarm(30);  // never outlive a crashed test
  for (int idx = 0;; idx++) {
    int cfd = accept(lfd, nullptr, nullptr);
    if (cfd < 0) _exit(0);
    char buf[32768];
    size_t body_len = 0;
    size_t body_at = fd_read_request(cfd, buf, sizeof buf, &body_len);
    if (body_at == 0) {
      close(cfd);
      idx--;
      continue;
    }
    fd_hit req = {0};
    (void)sscanf(buf, "%7s %159s", req.method, req.path);
    req.ms = fd_now_ms();
    req.body_len = body_len;
    size_t copy_len = body_len < sizeof req.body - 1u ? body_len : sizeof req.body - 1u;
    memcpy(req.body, buf + body_at, copy_len);
    req.body[copy_len] = '\0';
    for (char *p = buf; p < buf + body_at - 2;) {
      char *nl = strstr(p, "\r\n");
      if (!nl || nl >= buf + body_at) break;
      if (strncasecmp(p, "X-Audit-Log-Reason:", 19) == 0) {
        char *v = p + 19;
        while (*v == ' ') v++;
        size_t vl = (size_t)(nl - v);
        if (vl >= sizeof req.audit) vl = sizeof req.audit - 1u;
        memcpy(req.audit, v, vl);
        req.audit[vl] = '\0';
      } else if (strncasecmp(p, "Content-Type:", 13) == 0) {
        char *v = p + 13;
        while (*v == ' ') v++;
        size_t vl = (size_t)(nl - v);
        if (vl >= sizeof req.content_type) vl = sizeof req.content_type - 1u;
        memcpy(req.content_type, v, vl);
        req.content_type[vl] = '\0';
      } else if (strncasecmp(p, "Authorization:", 14) == 0) {
        char *v = p + 14;
        while (*v == ' ') v++;
        size_t vl = (size_t)(nl - v);
        if (vl >= sizeof req.authorization) vl = sizeof req.authorization - 1u;
        memcpy(req.authorization, v, vl);
        req.authorization[vl] = '\0';
      } else if (strncasecmp(p, "X-Super-Properties:", 19) == 0) {
        req.super_properties = true;
      }
      p = nl + 2;
    }
    (void)!write(wfd, &req, sizeof req);
    char out[262144];
    size_t n = fn(ud, idx, &req, out, sizeof out);
    if (n > 0) (void)!write(cfd, out, n);
    close(cfd);
  }
}

[[maybe_unused]] static bool fd_start(fd_server *s, fd_respond_fn fn, void *ud)
{
  memset(s, 0, sizeof *s);
  (void)signal(SIGPIPE, SIG_IGN);
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) return false;
  int opt = 1;
  (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  struct sockaddr_in sin = {0};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t slen = sizeof sin;
  if (bind(lfd, (struct sockaddr *)&sin, sizeof sin) != 0 || listen(lfd, 16) != 0 ||
      getsockname(lfd, (struct sockaddr *)&sin, &slen) != 0) {
    close(lfd);
    return false;
  }
  s->port = ntohs(sin.sin_port);
  int pfd[2];
  if (pipe(pfd) != 0) {
    close(lfd);
    return false;
  }
  s->pid = fork();
  if (s->pid == 0) {
    close(pfd[0]);
    fd_serve(lfd, pfd[1], fn, ud);
    _exit(0);
  }
  close(lfd);
  close(pfd[1]);
  s->pipe_rd = pfd[0];
  (void)snprintf(s->base, sizeof s->base, "http://127.0.0.1:%d", s->port);
  return s->pid > 0;
}

// Stops the server and returns every request it saw, in arrival order.
[[maybe_unused]] static size_t fd_stop(fd_server *s, fd_hit *hits, size_t cap)
{
  (void)kill(s->pid, SIGTERM);
  (void)waitpid(s->pid, nullptr, 0);
  size_t count = 0;
  for (;;) {
    fd_hit tmp;
    size_t got = 0;
    while (got < sizeof tmp) {
      ssize_t n = read(s->pipe_rd, (char *)&tmp + got, sizeof tmp - got);
      if (n <= 0) break;
      got += (size_t)n;
    }
    if (got < sizeof tmp) break;
    if (count < cap) hits[count++] = tmp;
  }
  close(s->pipe_rd);
  return count;
}

// ---- own-profile routes -----------------------------------------------------------------------------------------
// A ready-made responder (pass it to fd_start) for PATCH /users/@me and PATCH /users/@me/profile. It keeps a tiny
// account in the server process, applies the fields of each body the way Discord does (null clears, a data URI
// becomes a fresh hash "fakeav<N>" / "fakebn<N>") and answers with the object Discord returns. Any other request
// gets 404. Set `fail_idx` (request number, -1 = never) to answer that one request with `fail_status`/`fail_body`
// instead, without applying it.
typedef struct fd_profile_cfg {
  unsigned long long self_id;
  int fail_idx;
  int fail_status;
  const char *fail_body;
} fd_profile_cfg;

// Finds "key": in a flat JSON body. Returns 0 absent, 1 string (copied unescaped-as-is), 2 null, 3 number (in *num).
[[maybe_unused]] static int fd_body_field(const char *body, const char *key, char *out, size_t cap, long *num)
{
  char pat[48];
  (void)snprintf(pat, sizeof pat, "\"%s\":", key);
  const char *p = strstr(body, pat);
  if (!p) return 0;
  p += strlen(pat);
  if (strncmp(p, "null", 4) == 0) return 2;
  if (*p == '"') {
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
      if (*p == '\\' && p[1]) p++;
      out[n++] = *p++;
    }
    out[n] = '\0';
    return 1;
  }
  if (num) *num = strtol(p, nullptr, 10);
  return 3;
}

[[maybe_unused]] static void fd_copy(char *dst, size_t cap, const char *src)
{
  size_t n = 0;
  while (src[n] && n + 1 < cap) {
    dst[n] = src[n];
    n++;
  }
  dst[n] = '\0';
}

[[maybe_unused]] static size_t fd_profile_respond(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  static char name[256], avatar[64], banner[64], bio[1024], pronouns[256];
  static long accent = -1;
  static unsigned seq;
  const fd_profile_cfg *cfg = ud;
  bool me = strcmp(req->path, "/api/v9/users/@me") == 0 || strcmp(req->path, "/users/@me") == 0;
  bool profile = strcmp(req->path, "/api/v9/users/@me/profile") == 0 || strcmp(req->path, "/users/@me/profile") == 0;
  if (strcmp(req->method, "PATCH") != 0 || (!me && !profile)) return fd_reply(out, cap, 404, "Not Found", nullptr, "{\"message\":\"404: Not Found\",\"code\":0}");
  if (cfg && cfg->fail_idx == idx) {
    return fd_reply(out, cap, cfg->fail_status, cfg->fail_status == 429 ? "Too Many Requests" : "Bad Request", nullptr,
                    cfg->fail_body ? cfg->fail_body : "{}");
  }
  char val[1024];
  long num = 0;
  char body[2400];
  if (me) {
    int k = fd_body_field(req->body, "global_name", val, sizeof val, &num);
    if (k == 2) name[0] = '\0';
    else if (k == 1) fd_copy(name, sizeof name, val);
    k = fd_body_field(req->body, "avatar", val, sizeof val, &num);
    if (k == 2) avatar[0] = '\0';
    else if (k == 1) (void)snprintf(avatar, sizeof avatar, "fakeav%u", ++seq);
    k = fd_body_field(req->body, "banner", val, sizeof val, &num);
    if (k == 2) banner[0] = '\0';
    else if (k == 1) (void)snprintf(banner, sizeof banner, "fakebn%u", ++seq);
    char gn[300], av[96], bn[96];
    (void)snprintf(gn, sizeof gn, name[0] ? "\"%s\"" : "null", name);
    (void)snprintf(av, sizeof av, avatar[0] ? "\"%s\"" : "null", avatar);
    (void)snprintf(bn, sizeof bn, banner[0] ? "\"%s\"" : "null", banner);
    (void)snprintf(body, sizeof body, "{\"id\":\"%llu\",\"username\":\"fake\",\"global_name\":%s,\"avatar\":%s,\"banner\":%s}",
                   cfg ? cfg->self_id : 0ull, gn, av, bn);
  } else {
    int k = fd_body_field(req->body, "bio", val, sizeof val, &num);
    if (k == 1) (void)snprintf(bio, sizeof bio, "%s", val);
    else if (k == 2) bio[0] = '\0';
    k = fd_body_field(req->body, "pronouns", val, sizeof val, &num);
    if (k == 1) fd_copy(pronouns, sizeof pronouns, val);
    else if (k == 2) pronouns[0] = '\0';
    k = fd_body_field(req->body, "accent_color", val, sizeof val, &num);
    if (k == 2) accent = -1;
    else if (k == 3) accent = num;
    char ac[24];
    (void)snprintf(ac, sizeof ac, accent >= 0 ? "%ld" : "null", accent);
    (void)snprintf(body, sizeof body, "{\"bio\":\"%s\",\"pronouns\":\"%s\",\"accent_color\":%s}", bio, pronouns, ac);
  }
  return fd_reply(out, cap, 200, "OK", nullptr, body);
}
