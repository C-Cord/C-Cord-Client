// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Ranged media requests (stream_range jobs of the REST queue) against a forked loopback HTTP server.
//
// The server records the raw request head of every connection in shared memory, so the asserts are about what
// reached the wire: the Range header, identity encoding, no Authorization, no Cookie, how many hops were followed.
// test_allow_http_streams lets the queue talk http to 127.0.0.1 (the only place http is ever accepted); the gate
// refusals are checked on a queue without it.
#define _GNU_SOURCE
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ccord/core/clock.h>
#include <ccord/net/live_transport.h>

#include "ccord_test.h"

#define FILE_SIZE (2u * 1024u * 1024u + 4097u)
#define BIG_SIZE (3u * 1024u * 1024u)
#define MAX_LOG 64
#define HEAD_MAX 1536

typedef struct logged {
  char path[96];
  char head[HEAD_MAX];   // the whole request head as received
  bool tls;              // the connection began with a TLS ClientHello, not an HTTP request
} logged;

typedef struct shared_log {
  volatile int count;
  logged hits[MAX_LOG];
} shared_log;

typedef struct server {
  pid_t pid;
  int port;
  shared_log *log;
} server;

static uint8_t file_byte(size_t i) { return (uint8_t)(i * 131u + (i >> 8)); }

static void send_all(int fd, const char *p, size_t n)
{
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w <= 0) return;
    p += w;
    n -= (size_t)w;
  }
}

static void send_text(int fd, const char *s) { send_all(fd, s, strlen(s)); }

// "bytes=a-b" -> a, b. Returns false when absent or malformed.
static bool parse_range(const char *head, long long *a, long long *b)
{
  const char *r = strcasestr(head, "\r\nRange: bytes=");
  if (!r) return false;
  return sscanf(r + 15, "%lld-%lld", a, b) == 2;
}

static void send_body_bytes(int fd, size_t from, size_t n)
{
  char chunk[16384];
  while (n > 0) {
    size_t k = n < sizeof chunk ? n : sizeof chunk;
    for (size_t i = 0; i < k; i++) chunk[i] = (char)file_byte(from + i);
    send_all(fd, chunk, k);
    from += k;
    n -= k;
  }
}

static void respond(int cfd, int port, const char *path, const char *head)
{
  char buf[512];
  if (strcmp(path, "/file.mp4") == 0 || strcmp(path, "/nostar.mp4") == 0) {
    long long a = 0, b = 0;
    bool star = strcmp(path, "/nostar.mp4") == 0;
    if (!parse_range(head, &a, &b)) {
      snprintf(buf, sizeof buf, "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", FILE_SIZE);
      send_text(cfd, buf);
      send_body_bytes(cfd, 0, FILE_SIZE);
      return;
    }
    if (a >= (long long)FILE_SIZE) {
      snprintf(buf, sizeof buf,
               "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%u\r\nContent-Length: 0\r\n"
               "Connection: close\r\n\r\n",
               FILE_SIZE);
      send_text(cfd, buf);
      return;
    }
    if (b >= (long long)FILE_SIZE) b = (long long)FILE_SIZE - 1;
    char total[24];
    if (star) snprintf(total, sizeof total, "*");
    else snprintf(total, sizeof total, "%u", FILE_SIZE);
    snprintf(buf, sizeof buf,
             "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %lld-%lld/%s\r\nAccept-Ranges: bytes\r\n"
             "ETag: \"v1\"\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n",
             a, b, total, b - a + 1);
    send_text(cfd, buf);
    send_body_bytes(cfd, (size_t)a, (size_t)(b - a + 1));
  } else if (strcmp(path, "/weak.mp4") == 0) {
    send_text(cfd, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-3/100\r\nETag: W/\"x\"\r\n"
                   "Content-Length: 4\r\nConnection: close\r\n\r\nabcd");
  } else if (strcmp(path, "/ignore.mp4") == 0) {
    snprintf(buf, sizeof buf, "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", BIG_SIZE);
    send_text(cfd, buf);
    send_body_bytes(cfd, 0, BIG_SIZE);   // the client hangs up long before the end
  } else if (strcmp(path, "/ignore-small.mp4") == 0) {
    send_text(cfd, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\n0123456789");
  } else if (strcmp(path, "/badstart.mp4") == 0) {
    send_text(cfd, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 7-10/100\r\nContent-Length: 4\r\n"
                   "Connection: close\r\n\r\nabcd");
  } else if (strcmp(path, "/nocr.mp4") == 0) {
    send_text(cfd, "HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\nConnection: close\r\n\r\nabcd");
  } else if (strcmp(path, "/toolong.mp4") == 0) {
    char body[256];
    memset(body, 'x', sizeof body);
    snprintf(buf, sizeof buf,
             "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-255/1000\r\nContent-Length: 256\r\n"
             "Connection: close\r\n\r\n");
    send_text(cfd, buf);
    send_all(cfd, body, sizeof body);
  } else if (strcmp(path, "/gone.mp4") == 0) {
    send_text(cfd, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */1000\r\nContent-Length: 0\r\n"
                   "Connection: close\r\n\r\n");
  } else if (strcmp(path, "/forbidden.mp4") == 0) {
    send_text(cfd, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  } else if (strcmp(path, "/missing.mp4") == 0) {
    send_text(cfd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  } else if (strcmp(path, "/r1") == 0) {
    send_text(cfd, "HTTP/1.1 301 Moved Permanently\r\nLocation: /r2\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  } else if (strcmp(path, "/r2") == 0) {
    send_text(cfd, "HTTP/1.1 302 Found\r\nLocation: /file.mp4\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  } else if (strcmp(path, "/loop") == 0) {
    send_text(cfd, "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  } else if (strcmp(path, "/to-ftp") == 0) {
    send_text(cfd, "HTTP/1.1 302 Found\r\nLocation: ftp://127.0.0.1/x.mp4\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  } else if (strcmp(path, "/to-file") == 0) {
    send_text(cfd, "HTTP/1.1 302 Found\r\nLocation: file:///etc/hostname\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  } else if (strcmp(path, "/to-http") == 0) {
    snprintf(buf, sizeof buf,
             "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%d/file.mp4\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
             port);
    send_text(cfd, buf);
  } else if (strcmp(path, "/to-tls-ip") == 0) {
    snprintf(buf, sizeof buf,
             "HTTP/1.1 302 Found\r\nLocation: https://127.0.0.1:%d/file.mp4\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
             port);
    send_text(cfd, buf);
  } else {
    send_text(cfd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  }
}

static void serve(int lfd, int port, shared_log *log)
{
  alarm(60);                       // never outlive a crashed test
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  for (;;) {
    int cfd = accept(lfd, nullptr, nullptr);
    if (cfd < 0) _exit(0);
    char head[HEAD_MAX];
    size_t len = 0;
    bool tls = false;
    while (len + 1 < sizeof head) {
      ssize_t n = read(cfd, head + len, sizeof head - 1 - len);
      if (n <= 0) break;
      len += (size_t)n;
      head[len] = '\0';
      if (head[0] == 0x16) {   // a TLS ClientHello: record the connection and hang up
        tls = true;
        break;
      }
      if (strstr(head, "\r\n\r\n")) break;
    }
    head[len] = '\0';
    char path[96] = "";
    if (!tls) (void)sscanf(head, "GET %95s", path);
    int idx = log->count;
    if (idx < MAX_LOG) {
      logged *h = &log->hits[idx];
      snprintf(h->path, sizeof h->path, "%s", tls ? "<tls>" : path);
      memcpy(h->head, tls ? "" : head, tls ? 1 : len + 1);
      h->tls = tls;
      log->count = idx + 1;
    }
    if (!tls) respond(cfd, port, path, head);
    close(cfd);
  }
}

static bool server_start(server *s)
{
  memset(s, 0, sizeof *s);
  (void)signal(SIGPIPE, SIG_IGN);
  s->log = mmap(nullptr, sizeof *s->log, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (s->log == MAP_FAILED) return false;
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
  s->pid = fork();
  if (s->pid == 0) {
    serve(lfd, s->port, s->log);
    _exit(0);
  }
  close(lfd);
  return s->pid > 0;
}

static void server_stop(server *s)
{
  if (s->pid > 0) {
    (void)kill(s->pid, SIGTERM);
    (void)waitpid(s->pid, nullptr, 0);
    s->pid = 0;
  }
}

static int server_hits(const server *s) { return s->log->count; }

// ---- client side -----------------------------------------------------------------------------------------------

#define BODY_CAP (1024u * 1024u + 16u)

typedef struct result {
  int calls;
  ccord_status st;
  long http;
  size_t len;
  uint8_t *body;
  ccord_rest_range_info info;
} result;

static void on_result(void *ud, ccord_status st, long http, const char *body, size_t len)
{
  result *r = ud;
  r->calls++;
  r->st = st;
  r->http = http;
  r->len = len < BODY_CAP ? len : BODY_CAP;
  if (body && r->len > 0) memcpy(r->body, body, r->len);
}

static ccord_rest_queue *make_queue(bool allow_http)
{
  ccord_rest_config cfg = {.base_url = "http://127.0.0.1:1", .token = "REDACTED_TEST_TOKEN", .timeout_ms = 5000,
                           .test_allow_http_streams = allow_http};
  ccord_error err;
  return ccord_rest_queue_create(&cfg, &err);
}

static void reset(result *r)
{
  uint8_t *b = r->body;
  memset(r, 0, sizeof *r);
  r->body = b;
}

static bool wait_done(ccord_rest_queue *q, result *r)
{
  uint64_t end = ccord_clock_ms() + 15000;
  while (ccord_clock_ms() < end) {
    ccord_rest_queue_pump(q, ccord_clock_ms());
    if (r->calls > 0) return true;
    usleep(2000);
  }
  return false;
}

static ccord_status submit_range(ccord_rest_queue *q, const server *s, const char *path, int64_t start, size_t len,
                                 const char *if_range, result *r)
{
  char url[256];
  snprintf(url, sizeof url, "http://127.0.0.1:%d%s", s->port, path);
  ccord_rest_request req = ccord_rest_request_make(CCORD_REST_GET, nullptr, CCORD_GOV_USER);
  req.full_url = url;
  req.cb = on_result;
  req.ud = r;
  req.stream_range = true;
  req.range_start = start;
  req.range_len = len;
  req.if_range = if_range;
  req.range_info = &r->info;
  ccord_error err;
  return ccord_rest_queue_submit(q, &req, &err);
}

// Runs one ranged request to completion.
static bool run_range(ccord_rest_queue *q, const server *s, const char *path, int64_t start, size_t len,
                      const char *if_range, result *r)
{
  reset(r);
  return submit_range(q, s, path, start, len, if_range, r) == CCORD_OK && wait_done(q, r);
}

static bool body_matches(const result *r, size_t from)
{
  for (size_t i = 0; i < r->len; i++) {
    if (r->body[i] != file_byte(from + i)) return false;
  }
  return true;
}

static const logged *hit(const server *s, int i) { return &s->log->hits[i]; }

// ---- tests -----------------------------------------------------------------------------------------------------

static void test_ranged_request(ccord_rest_queue *q, const server *s, result *r)
{
  int before = server_hits(s);
  CT_CHECK(run_range(q, s, "/file.mp4", 4096, 65536, nullptr, r));
  CT_CHECK(r->st == CCORD_OK && r->http == 206);
  CT_CHECK(r->len == 65536 && body_matches(r, 4096));
  CT_CHECK(r->info.http_status == 206 && r->info.range_start == 4096 && r->info.entity_total == FILE_SIZE);
  CT_CHECK(r->info.accept_ranges && !r->info.truncated && r->info.redirects == 0);
  CT_CHECK(strcmp(r->info.etag, "\"v1\"") == 0);
  CT_CHECK(server_hits(s) == before + 1);
  const logged *h = hit(s, before);
  CT_CHECK(strcasestr(h->head, "\r\nRange: bytes=4096-69631\r\n") != nullptr);
  CT_CHECK(strcasestr(h->head, "\r\nAccept-Encoding: identity\r\n") != nullptr);
  char ua_hdr[512];
  snprintf(ua_hdr, sizeof ua_hdr, "\r\nUser-Agent: %s\r\n", ccord_live_user_agent());
  CT_CHECK(strcasestr(h->head, ua_hdr) != nullptr);
  CT_CHECK(strcasestr(h->head, "If-Range:") == nullptr);

  // The last chunk of the file is short: the server clamps, Content-Range names the real end.
  size_t tail_start = FILE_SIZE - 1000;
  CT_CHECK(run_range(q, s, "/file.mp4", (int64_t)tail_start, 65536, "\"v1\"", r));
  CT_CHECK(r->st == CCORD_OK && r->http == 206 && r->len == 1000 && body_matches(r, tail_start));
  CT_CHECK(r->info.range_start == (int64_t)tail_start && r->info.entity_total == FILE_SIZE);
  CT_CHECK(strcasestr(hit(s, server_hits(s) - 1)->head, "\r\nIf-Range: \"v1\"\r\n") != nullptr);

  // A full-size 1 MiB chunk (the largest allowed).
  CT_CHECK(run_range(q, s, "/file.mp4", 0, CCORD_STREAM_RANGE_MAX_BYTES, nullptr, r));
  CT_CHECK(r->st == CCORD_OK && r->len == CCORD_STREAM_RANGE_MAX_BYTES && body_matches(r, 0));

  // Unknown total ("bytes a-b/*") is reported as -1; a weak ETag is not offered back.
  CT_CHECK(run_range(q, s, "/nostar.mp4", 100, 50, nullptr, r));
  CT_CHECK(r->st == CCORD_OK && r->len == 50 && r->info.entity_total == -1 && body_matches(r, 100));
  CT_CHECK(run_range(q, s, "/weak.mp4", 0, 4, nullptr, r));
  CT_CHECK(r->st == CCORD_OK && r->len == 4 && r->info.etag[0] == '\0' && r->info.entity_total == 100);
}

static void test_range_ignored(ccord_rest_queue *q, const server *s, result *r)
{
  // The server answers 200 with a 3 MiB body: at most the requested 4096 bytes are kept, the rest is never read.
  CT_CHECK(run_range(q, s, "/ignore.mp4", 0, 4096, nullptr, r));
  CT_CHECK(r->st == CCORD_OK && r->http == 200);
  CT_CHECK(r->len == 4096 && body_matches(r, 0));
  CT_CHECK(r->info.http_status == 200 && r->info.truncated && r->info.entity_total == BIG_SIZE);
  CT_CHECK(!r->info.accept_ranges);
  // A 200 that is shorter than the request is not truncated.
  CT_CHECK(run_range(q, s, "/ignore-small.mp4", 0, 4096, nullptr, r));
  CT_CHECK(r->st == CCORD_OK && r->http == 200 && r->len == 10 && !r->info.truncated && r->info.entity_total == 10);
}

static void test_bad_answers(ccord_rest_queue *q, const server *s, result *r)
{
  CT_CHECK(run_range(q, s, "/badstart.mp4", 0, 4, nullptr, r));       // Content-Range starts at 7, not 0
  CT_CHECK(r->st == CCORD_ERR_PROTOCOL && r->http == 206);
  CT_CHECK(run_range(q, s, "/nocr.mp4", 0, 4, nullptr, r));           // 206 without Content-Range
  CT_CHECK(r->st == CCORD_ERR_PROTOCOL);
  CT_CHECK(run_range(q, s, "/toolong.mp4", 0, 100, nullptr, r));      // more bytes than asked for
  CT_CHECK(r->st != CCORD_OK);
  CT_CHECK(run_range(q, s, "/gone.mp4", 5000, 100, nullptr, r));      // 416: no bytes, total from "*/T"
  CT_CHECK(r->st == CCORD_OK && r->http == 416 && r->len == 0 && r->info.entity_total == 1000);
  CT_CHECK(run_range(q, s, "/forbidden.mp4", 0, 100, nullptr, r));
  CT_CHECK(r->st == CCORD_OK && r->http == 403 && r->len == 0);       // HTTP statuses are the caller's to map
  CT_CHECK(run_range(q, s, "/missing.mp4", 0, 100, nullptr, r));
  CT_CHECK(r->st == CCORD_OK && r->http == 404);
}

static void test_redirects(ccord_rest_queue *q, const server *s, result *r)
{
  // 301 -> 302 -> 206: the Range is re-sent on every hop and the chain is counted.
  int before = server_hits(s);
  CT_CHECK(run_range(q, s, "/r1", 8192, 2048, nullptr, r));
  CT_CHECK(r->st == CCORD_OK && r->http == 206 && r->len == 2048 && body_matches(r, 8192));
  CT_CHECK(r->info.redirects == 2 && r->info.range_start == 8192);
  CT_CHECK(server_hits(s) == before + 3);
  for (int i = before; i < before + 3; i++) {
    CT_CHECK(strcasestr(hit(s, i)->head, "\r\nRange: bytes=8192-10239\r\n") != nullptr);
  }

  // Six hops is one too many (limit 5): refused, nothing delivered.
  before = server_hits(s);
  CT_CHECK(run_range(q, s, "/loop", 0, 100, nullptr, r));
  CT_CHECK(r->st == CCORD_ERR_UNSUPPORTED && r->len == 0);
  CT_CHECK(server_hits(s) == before + 6);

  // ftp: and file: targets are never followed (the server sees only the first request).
  before = server_hits(s);
  CT_CHECK(run_range(q, s, "/to-ftp", 0, 100, nullptr, r));
  CT_CHECK(r->st == CCORD_ERR_UNSUPPORTED && r->len == 0);
  CT_CHECK(run_range(q, s, "/to-file", 0, 100, nullptr, r));
  CT_CHECK(r->st == CCORD_ERR_UNSUPPORTED && r->len == 0);
  CT_CHECK(server_hits(s) == before + 2);

  // Only the test setting lets a stream job follow an http: redirect.
  CT_CHECK(run_range(q, s, "/to-http", 0, 100, nullptr, r));
  CT_CHECK(r->st == CCORD_OK && r->http == 206 && r->info.redirects == 1);

  // https to an IP literal is followed (the gate checks the first URL only): the server sees a TLS handshake arrive
  // at the redirect target, which then fails, because the loopback server does not speak TLS.
  before = server_hits(s);
  CT_CHECK(run_range(q, s, "/to-tls-ip", 0, 100, nullptr, r));
  CT_CHECK(r->st != CCORD_OK || r->http != 206);
  CT_CHECK(server_hits(s) == before + 2 && hit(s, before + 1)->tls);
}

// A job that is not a stream job keeps FOLLOWLOCATION 0: the 301 is the answer and nothing is requested after it.
static void test_whole_body_no_redirect(ccord_rest_queue *q, const server *s, result *r)
{
  reset(r);
  char url[128];
  snprintf(url, sizeof url, "http://127.0.0.1:%d/r1", s->port);
  int before = server_hits(s);
  ccord_error err;
  CT_CHECK(ccord_rest_queue_add_raw(q, CCORD_REST_GET, url, nullptr, nullptr, 0, on_result, r, &err) == CCORD_OK);
  CT_CHECK(wait_done(q, r));
  CT_CHECK(r->st == CCORD_OK && r->http == 301);
  CT_CHECK(server_hits(s) == before + 1);
  CT_CHECK(strcasestr(hit(s, before)->head, "Range:") == nullptr);
}

static void test_no_credentials_on_the_wire(const server *s)
{
  int n = server_hits(s);
  CT_CHECK(n > 20);
  for (int i = 0; i < n && i < MAX_LOG; i++) {
    const logged *h = hit(s, i);
    CT_CHECK(strcasestr(h->head, "Authorization") == nullptr);
    CT_CHECK(strcasestr(h->head, "Cookie") == nullptr);
    CT_CHECK(strcasestr(h->head, "X-Super-Properties") == nullptr);
    CT_CHECK(strcasestr(h->head, "REDACTED_TEST_TOKEN") == nullptr);
  }
}

static void test_refused_at_submit(const server *s, result *r)
{
  ccord_rest_queue *q = make_queue(false);   // no test setting: the real gate
  CT_CHECK(q != nullptr);
  ccord_error err;
  const char *urls[] = {
      "http://127.0.0.1:1/a.mp4",                  // plain http
      "http://videos.example.org/a.mp4",
      "https://203.0.113.5/a.mp4",                 // IP literals
      "https://127.0.0.1/a.mp4",
      "https://[::1]/a.mp4",
      "https://localhost/a.mp4",
      "https://videos.example.org:8443/a.mp4",     // a port other than 443
      "https://videos.example.org/a b.mp4",        // a space
      "https://videos.example.org/a\r\nX: y.mp4",  // header injection
      "https://videos.example.org/",               // empty path
      "ftp://videos.example.org/a.mp4",
      "file:///etc/hostname",
  };
  for (size_t i = 0; i < sizeof urls / sizeof urls[0]; i++) {
    reset(r);
    ccord_rest_request req = ccord_rest_request_make(CCORD_REST_GET, nullptr, CCORD_GOV_USER);
    req.full_url = urls[i];
    req.cb = on_result;
    req.ud = r;
    req.stream_range = true;
    req.range_len = 100;
    CT_CHECK(ccord_rest_queue_submit(q, &req, &err) == CCORD_ERR_PERMISSION);
    CT_CHECK(r->calls == 0 && ccord_rest_queue_pending(q) == 0);
  }
  char longurl[2200] = "https://videos.example.org/";
  memset(longurl + strlen(longurl), 'a', 2100);
  ccord_rest_request req = ccord_rest_request_make(CCORD_REST_GET, nullptr, CCORD_GOV_USER);
  req.full_url = longurl;
  req.stream_range = true;
  req.range_len = 100;
  CT_CHECK(ccord_rest_queue_submit(q, &req, &err) == CCORD_ERR_PERMISSION);

  // Malformed ranges, and a stream job without an absolute URL.
  struct { int64_t start; size_t len; } bad[] = {{-1, 10}, {0, 0}, {0, CCORD_STREAM_RANGE_MAX_BYTES + 1u}, {INT64_MAX, 2}};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    req = ccord_rest_request_make(CCORD_REST_GET, nullptr, CCORD_GOV_USER);
    req.full_url = "https://videos.example.org/a.mp4";
    req.stream_range = true;
    req.range_start = bad[i].start;
    req.range_len = bad[i].len;
    CT_CHECK(ccord_rest_queue_submit(q, &req, &err) == CCORD_ERR_INVALID);
  }
  req = ccord_rest_request_make(CCORD_REST_GET, "/channels/1/messages", CCORD_GOV_USER);
  req.stream_range = true;
  req.range_len = 10;
  CT_CHECK(ccord_rest_queue_submit(q, &req, &err) == CCORD_ERR_INVALID);
  req = ccord_rest_request_make(CCORD_REST_GET, nullptr, CCORD_GOV_USER);
  req.full_url = "https://videos.example.org/a.mp4";
  req.stream_range = true;
  req.range_len = 10;
  req.if_range = "\"a\r\nCookie: b\"";
  CT_CHECK(ccord_rest_queue_submit(q, &req, &err) == CCORD_ERR_INVALID);

  // A good https URL is accepted at submit (destroyed before any pump, so nothing leaves the machine).
  reset(r);
  req = ccord_rest_request_make(CCORD_REST_GET, nullptr, CCORD_GOV_USER);
  req.full_url = "https://videos.example.org/clip/a.mp4?ex=1&hm=abc";
  req.cb = on_result;
  req.ud = r;
  req.stream_range = true;
  req.range_len = 100;
  req.range_info = &r->info;
  CT_CHECK(ccord_rest_queue_submit(q, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_rest_queue_pending(q) == 1);
  ccord_rest_queue_destroy(q);
  CT_CHECK(r->calls == 1 && r->st == CCORD_ERR_IO);   // a job that never ran is answered, not dropped silently
  (void)s;
}

int main(void)
{
  server s;
  if (!server_start(&s)) {
    fprintf(stderr, "test_rest_range: cannot start the loopback server\n");
    return 1;
  }
  result r = {0};
  r.body = malloc(BODY_CAP);
  ccord_rest_queue *q = make_queue(true);
  CT_CHECK(q != nullptr && r.body != nullptr);
  if (q && r.body) {
    test_ranged_request(q, &s, &r);
    test_range_ignored(q, &s, &r);
    test_bad_answers(q, &s, &r);
    test_redirects(q, &s, &r);
    test_whole_body_no_redirect(q, &s, &r);
    test_no_credentials_on_the_wire(&s);
    ccord_rest_queue_destroy(q);
    test_refused_at_submit(&s, &r);
  }
  server_stop(&s);
  free(r.body);
  return CT_RESULT("test_rest_range");
}
