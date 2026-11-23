// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// A local fake Discord *gateway* for tests: a forked loopback WebSocket server that runs a script per connection and
// reports what it saw (each connection's handshake, every payload the client sent, with arrival times) back to the
// test. Like fake_discord.h, tests assert on what reached the server, which is what Discord would observe.
//
//   fg_server srv;
//   fg_start(&srv, script_fn, &my_state);        // fork; srv.url is "ws://127.0.0.1:<port>"
//   ... point the client's gateway_url at srv.url, drive it ...
//   fg_event ev[64]; size_t n = fg_stop(&srv, ev, 64);   // kills the server, returns what it logged
//
// The script runs in the child process (so it sees the test's memory as it was at fg_start), once per accepted connection (connection 0, 1, 2 ... in order). It calls
// fg_handshake() itself, so a script that does not simply drops the connection (the client sees a failed connect).
// WebSocket framing here is the minimum the client needs: unfragmented text frames, close, ping/pong.
#pragma once
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
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

#include <openssl/evp.h>

#define FG_MAX_EVENTS 256

typedef struct fg_event {
  uint64_t ms;       // CLOCK_MONOTONIC milliseconds
  int conn;          // which connection (0-based, in accept order)
  char text[200];    // what the script logged ("IDENTIFY", "HEARTBEAT", "RESUME seq=7", ...)
} fg_event;

typedef struct fg_conn {
  int fd;
  int idx;
  int log_fd;
  char path[256];    // request target from the handshake, e.g. "/?v=9&encoding=json&resume=1"
  char self_url[64]; // this server's own ws:// URL (what READY advertises as resume_gateway_url)
  int port;
  int *lfd;          // the listening socket (see fg_listener_off / fg_listener_on)
} fg_conn;

typedef void (*fg_script_fn)(void *ud, fg_conn *c);

typedef struct fg_server {
  pid_t pid;
  int pipe_rd;
  int port;
  char url[64];
} fg_server;

[[maybe_unused]] static uint64_t fg_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

// Appends one line to the log the test reads back. Never contains a tab or newline.
#if defined(__has_c_attribute)
#if __has_c_attribute(gnu::format)
[[gnu::format(printf, 2, 3)]]
#endif
#endif
[[maybe_unused]] static void fg_log(const fg_conn *c, const char *fmt, ...)
{
  char text[190];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  for (char *p = text; *p; p++) {
    if (*p == '\t' || *p == '\n') *p = ' ';
  }
  char line[256];
  int n = snprintf(line, sizeof line, "%llu\t%d\t%s\n", (unsigned long long)fg_now_ms(), c->idx, text);
  if (n > 0 && (size_t)n < sizeof line) (void)!write(c->log_fd, line, (size_t)n);
}

static bool fg_read_exact(int fd, void *buf, size_t n, int timeout_ms)
{
  size_t got = 0;
  while (got < n) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return false;
    ssize_t r = read(fd, (char *)buf + got, n - got);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

// Reads the HTTP upgrade request and answers 101. False when the client went away.
[[maybe_unused]] static bool fg_handshake(fg_conn *c)
{
  char req[4096];
  size_t len = 0;
  while (len + 1 < sizeof req) {
    struct pollfd pfd = {.fd = c->fd, .events = POLLIN};
    if (poll(&pfd, 1, 3000) <= 0) return false;
    ssize_t n = read(c->fd, req + len, sizeof req - 1 - len);
    if (n <= 0) return false;
    len += (size_t)n;
    req[len] = '\0';
    if (strstr(req, "\r\n\r\n")) break;
  }
  (void)sscanf(req, "GET %255s", c->path);
  char key[64] = "";
  for (char *p = req; *p;) {
    char *nl = strstr(p, "\r\n");
    if (!nl) break;
    if (strncasecmp(p, "Sec-WebSocket-Key:", 18) == 0) {
      char *v = p + 18;
      while (*v == ' ') v++;
      size_t vl = (size_t)(nl - v);
      if (vl >= sizeof key) return false;
      memcpy(key, v, vl);
      key[vl] = '\0';
    }
    p = nl + 2;
  }
  if (key[0] == '\0') return false;
  char cat[128];
  int cn = snprintf(cat, sizeof cat, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
  unsigned char sha[EVP_MAX_MD_SIZE];
  unsigned int shalen = 0;
  if (cn <= 0 || EVP_Digest(cat, (size_t)cn, sha, &shalen, EVP_sha1(), nullptr) != 1) return false;
  unsigned char b64[64];
  int bl = EVP_EncodeBlock(b64, sha, (int)shalen);
  if (bl <= 0) return false;
  char resp[256];
  int rn = snprintf(resp, sizeof resp,
                    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: %.*s\r\n\r\n",
                    bl, (const char *)b64);
  return rn > 0 && write(c->fd, resp, (size_t)rn) == rn;
}

static bool fg_write_all(int fd, const void *buf, size_t n)
{
  const char *p = buf;
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, p + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (w == 0) return false;
    off += (size_t)w;
  }
  return true;
}

// Sends one unfragmented, unmasked frame (opcode 1 text, 8 close, 9 ping, 10 pong).
static bool fg_send_frame(fg_conn *c, unsigned opcode, const void *data, size_t len)
{
  unsigned char hdr[10];
  size_t h = 0;
  hdr[h++] = (unsigned char)(0x80u | opcode);
  if (len < 126) {
    hdr[h++] = (unsigned char)len;
  } else if (len <= 0xFFFF) {
    hdr[h++] = 126;
    hdr[h++] = (unsigned char)(len >> 8);
    hdr[h++] = (unsigned char)len;
  } else {
    hdr[h++] = 127;
    for (int i = 7; i >= 0; i--) hdr[h++] = (unsigned char)((uint64_t)len >> (8 * i));
  }
  if (len <= 512) {
    unsigned char pkt[10 + 512];
    memcpy(pkt, hdr, h);
    if (len > 0) memcpy(pkt + h, data, len);
    return fg_write_all(c->fd, pkt, h + len);
  }
  if (!fg_write_all(c->fd, hdr, h)) return false;
  return len == 0 || fg_write_all(c->fd, data, len);
}

[[maybe_unused]] static bool fg_send(fg_conn *c, const char *json)
{
  return fg_send_frame(c, 1, json, strlen(json));
}

// Closes with a WebSocket close code, as Discord does ("4004" = authentication failed, etc.).
// Half-closes writes and drains any unread client bytes (e.g. an in-flight OP 15 / heartbeat / close echo) until
// the peer closes so close(cfd) never triggers a Linux loopback TCP RST that drops the unread Close frame.
[[maybe_unused]] static void fg_close(fg_conn *c, unsigned code)
{
  unsigned char payload[2] = {(unsigned char)(code >> 8), (unsigned char)code};
  (void)fg_send_frame(c, 8, payload, 2);
  (void)shutdown(c->fd, SHUT_WR);
  uint64_t deadline = fg_now_ms() + 1000u;
  char discard[1024];
  for (;;) {
    uint64_t now = fg_now_ms();
    if (now >= deadline) break;
    struct pollfd pfd = {.fd = c->fd, .events = POLLIN, .revents = 0};
    int pr = poll(&pfd, 1, (int)(deadline - now));
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) break;
    ssize_t r = read(c->fd, discard, sizeof discard);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
  }
}

enum { FG_GOT_TEXT = 1, FG_TIMEOUT = 0, FG_GONE = -1, FG_CLIENT_CLOSE = -2 };

// Waits up to timeout_ms for the next client payload. Returns FG_GOT_TEXT (text in buf), FG_TIMEOUT, FG_GONE (the
// client dropped the connection) or FG_CLIENT_CLOSE. Pings are answered; pongs are ignored.
[[maybe_unused]] static int fg_recv(fg_conn *c, char *buf, size_t cap, int timeout_ms)
{
  for (;;) {
    struct pollfd pfd = {.fd = c->fd, .events = POLLIN};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr == 0) return FG_TIMEOUT;
    if (pr < 0) return FG_GONE;
    unsigned char h2[2];
    if (!fg_read_exact(c->fd, h2, 2, 1000)) return FG_GONE;
    unsigned opcode = h2[0] & 0x0Fu;
    bool masked = (h2[1] & 0x80u) != 0;
    uint64_t len = h2[1] & 0x7Fu;
    if (len == 126) {
      unsigned char e[2];
      if (!fg_read_exact(c->fd, e, 2, 1000)) return FG_GONE;
      len = ((uint64_t)e[0] << 8) | e[1];
    } else if (len == 127) {
      unsigned char e[8];
      if (!fg_read_exact(c->fd, e, 8, 1000)) return FG_GONE;
      len = 0;
      for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
    }
    unsigned char mask[4] = {0};
    if (masked && !fg_read_exact(c->fd, mask, 4, 1000)) return FG_GONE;
    if (len >= cap) return FG_GONE;   // a script never expects frames this large
    if (len > 0 && !fg_read_exact(c->fd, buf, (size_t)len, 1000)) return FG_GONE;
    for (uint64_t i = 0; masked && i < len; i++) buf[i] = (char)((unsigned char)buf[i] ^ mask[i % 4]);
    buf[len] = '\0';
    if (opcode == 8) return FG_CLIENT_CLOSE;
    if (opcode == 9) {
      (void)fg_send_frame(c, 10, buf, (size_t)len);
      continue;
    }
    if (opcode == 10) continue;
    return FG_GOT_TEXT;
  }
}

// Stops accepting connections: new attempts are refused (ECONNREFUSED), like a network that is down.
[[maybe_unused]] static void fg_listener_off(fg_conn *c)
{
  if (*c->lfd >= 0) close(*c->lfd);
  *c->lfd = -1;
}

// Starts accepting again on the same port.
[[maybe_unused]] static void fg_listener_on(fg_conn *c)
{
  int l = socket(AF_INET, SOCK_STREAM, 0);
  if (l < 0) _exit(1);
  int opt = 1;
  (void)setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  struct sockaddr_in sin = {0};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sin.sin_port = htons((uint16_t)c->port);
  if (bind(l, (struct sockaddr *)&sin, sizeof sin) != 0 || listen(l, 16) != 0) _exit(1);
  *c->lfd = l;
}

static void fg_serve(int lfd, int wfd, int port, const char *url, fg_script_fn fn, void *ud)
{
  alarm(40);  // never outlive a crashed test
  for (int idx = 0;; idx++) {
    int cfd = accept(lfd, nullptr, nullptr);
    if (cfd < 0) _exit(0);
    fg_conn c = {.fd = cfd, .idx = idx, .log_fd = wfd, .port = port, .lfd = &lfd};
    (void)snprintf(c.self_url, sizeof c.self_url, "%s", url);
    fn(ud, &c);
    close(cfd);
  }
}

[[maybe_unused]] static bool fg_start(fg_server *s, fg_script_fn fn, void *ud)
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
  (void)snprintf(s->url, sizeof s->url, "ws://127.0.0.1:%d", s->port);   // known before the fork: the child needs it
  s->pid = fork();
  if (s->pid == 0) {
    close(pfd[0]);
    fg_serve(lfd, pfd[1], s->port, s->url, fn, ud);
    _exit(0);
  }
  close(lfd);
  close(pfd[1]);
  s->pipe_rd = pfd[0];
  return s->pid > 0;
}

// Stops the server and returns everything the scripts logged, in order.
[[maybe_unused]] static size_t fg_stop(fg_server *s, fg_event *out, size_t cap)
{
  (void)kill(s->pid, SIGTERM);
  (void)waitpid(s->pid, nullptr, 0);
  const size_t cap_bytes = FG_MAX_EVENTS * 256u;
  char *data = (char *)malloc(cap_bytes);
  size_t len = 0;
  for (;;) {
    if (!data || cap_bytes - 1u <= len) break;
    ssize_t n = read(s->pipe_rd, data + len, cap_bytes - 1u - len);
    if (n <= 0) break;
    len += (size_t)n;
  }
  close(s->pipe_rd);
  if (!data) return 0;
  data[len] = '\0';
  size_t count = 0;
  for (char *p = data; *p && count < cap;) {
    char *nl = strchr(p, '\n');
    if (!nl) break;
    *nl = '\0';
    char *t1 = strchr(p, '\t');
    char *t2 = t1 ? strchr(t1 + 1, '\t') : nullptr;
    if (t2) {
      *t1 = *t2 = '\0';
      out[count].ms = strtoull(p, nullptr, 10);
      out[count].conn = atoi(t1 + 1);
      (void)snprintf(out[count].text, sizeof out[count].text, "%s", t2 + 1);
      count++;
    }
    p = nl + 1;
  }
  free(data);
  return count;
}

// How many logged events on connection `conn` start with `prefix` (convenience for assertions).
[[maybe_unused]] static size_t fg_count(const fg_event *ev, size_t n, int conn, const char *prefix)
{
  size_t k = 0;
  for (size_t i = 0; i < n; i++) k += ev[i].conn == conn && strncmp(ev[i].text, prefix, strlen(prefix)) == 0;
  return k;
}

// How many distinct connections the server accepted.
[[maybe_unused]] static int fg_connections(const fg_event *ev, size_t n)
{
  int hi = -1;
  for (size_t i = 0; i < n; i++) {
    if (ev[i].conn > hi) hi = ev[i].conn;
  }
  return hi + 1;
}

// ---- common script pieces -------------------------------------------------------------------------------------------

#define FG_READY                                                                                               \
  "{\"op\":0,\"t\":\"READY\",\"s\":1,\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"                    \
  "\"session_id\":\"sess-1\",\"resume_gateway_url\":\"%s\",\"guilds\":[],\"private_channels\":[]}}"

// Handshake, HELLO with `interval_ms`, then wait for the client's first payload and log what it is (IDENTIFY,
// RESUME seq=N). On IDENTIFY answers READY (with resume_gateway_url = this server); on RESUME answers RESUMED.
// Returns false when the connection died on the way.
[[maybe_unused]] static bool fg_open_session(fg_conn *c, unsigned interval_ms)
{
  if (!fg_handshake(c)) return false;
  fg_log(c, "HANDSHAKE %s", c->path);
  char hello[96];
  (void)snprintf(hello, sizeof hello, "{\"op\":10,\"d\":{\"heartbeat_interval\":%u}}", interval_ms);
  if (!fg_send(c, hello)) return false;
  char buf[2048];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) return false;
  if (strstr(buf, "\"op\":2")) {
    fg_log(c, "IDENTIFY");
    char ready[512];
    (void)snprintf(ready, sizeof ready, FG_READY, c->self_url);
    return fg_send(c, ready);
  }
  if (strstr(buf, "\"op\":6")) {
    const char *seq = strstr(buf, "\"seq\":");
    fg_log(c, "RESUME seq=%d session=%s", seq ? atoi(seq + 6) : -1, strstr(buf, "sess-1") ? "sess-1" : "other");
    return fg_send(c, "{\"op\":0,\"t\":\"RESUMED\",\"s\":2,\"d\":{}}");
  }
  fg_log(c, "UNEXPECTED %.60s", buf);
  return false;
}

// Serves the open session: logs every payload (HEARTBEAT d=<seq or null>, PRESENCE, LAZY <channels>, OTHER) and acknowledges
// heartbeats when `ack` is set, until the client leaves, `until_ms` (monotonic) passes, or `stop_after_beats`
// heartbeats have been seen (0 = no limit).
[[maybe_unused]] static void fg_serve_session(fg_conn *c, bool ack, uint64_t until_ms, unsigned stop_after_beats)
{
  unsigned beats = 0;
  char buf[2048];
  while (fg_now_ms() < until_ms) {
    int r = fg_recv(c, buf, sizeof buf, 25);
    if (r == FG_TIMEOUT) continue;
    if (r != FG_GOT_TEXT) {
      fg_log(c, r == FG_CLIENT_CLOSE ? "CLIENT-CLOSE" : "CLIENT-GONE");
      return;
    }
    if (strstr(buf, "\"op\":1,") || strstr(buf, "\"op\":1}")) {
      const char *d = strstr(buf, "\"d\":");
      fg_log(c, "HEARTBEAT d=%.12s", d ? d + 4 : "?");
      beats++;
      if (ack) (void)fg_send(c, "{\"op\":11}");
      if (stop_after_beats && beats >= stop_after_beats) return;
    } else if (strstr(buf, "\"op\":3")) {
      fg_log(c, "PRESENCE");
    } else if (strstr(buf, "\"op\":14")) {
      const char *ch = strstr(buf, "\"channels\":");   // a member-list subscription: log the channels and their ranges
      fg_log(c, "LAZY %.150s", ch ? ch + 11 : "?");
    } else {
      fg_log(c, "OTHER %.60s", buf);
    }
  }
}
