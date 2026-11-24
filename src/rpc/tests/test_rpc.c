// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE   // POSIX/GNU declarations (usleep, mkdtemp, setenv) that -std=c2x hides
#include <ccord/rpc/rpc.h>

#include <ccord/core/arena.h>
#include <ccord/core/json.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ccord_test.h"

// Every socket lives in a private mkdtemp directory under $TMPDIR (never a hard-coded /tmp). The server is poll-driven,
// so each helper that waits for a reply keeps polling the server on the test's fake clock.

static char g_dir[PATH_MAX / 2];
static ccord_arena g_arena;

#define HS "{\"v\":1,\"client_id\":\"1234567890123456789\"}"
#define T0 1000u   // fake poll clock start

typedef struct hook_state {
  int get_user_calls;
  int ready;
  int activity_calls;
  int cleared;                  // on_activity(NULL) calls
  int disconnects;
  char ready_client_id[32];
  ccord_rpc_activity last;      // copy of the last non-null activity
} hook_state;

static bool hook_get_user(void *ctx, ccord_rpc_user *out)
{
  hook_state *h = ctx;
  h->get_user_calls++;
  snprintf(out->id, sizeof out->id, "42");
  snprintf(out->username, sizeof out->username, "tester");
  snprintf(out->discriminator, sizeof out->discriminator, "0");
  snprintf(out->global_name, sizeof out->global_name, "Tester");
  snprintf(out->avatar, sizeof out->avatar, "abc123");
  return true;
}

static void hook_ready(void *ctx, ccord_rpc_conn *conn)
{
  hook_state *h = ctx;
  h->ready++;
  snprintf(h->ready_client_id, sizeof h->ready_client_id, "%s", ccord_rpc_conn_client_id(conn));
}

static void hook_activity(void *ctx, ccord_rpc_conn *conn, const ccord_rpc_activity *act)
{
  hook_state *h = ctx;
  (void)conn;
  h->activity_calls++;
  if (act) h->last = *act; else h->cleared++;
}

static void hook_disconnect(void *ctx, ccord_rpc_conn *conn)
{
  hook_state *h = ctx;
  (void)conn;
  h->disconnects++;
}

static ccord_rpc_server *make_server_in(const char *dir, hook_state *h)
{
  ccord_rpc_config cfg = {
    .dir = dir,
    .hooks = {.get_user = hook_get_user, .on_ready = hook_ready, .on_activity = hook_activity,
              .on_disconnect = hook_disconnect},
    .ctx = h,
  };
  ccord_error err;
  ccord_rpc_server *s = ccord_rpc_server_create(&cfg, &err);
  if (!s) fprintf(stderr, "ccord_rpc_server_create: %s\n", err.message);
  return s;
}

static ccord_rpc_server *make_server(hook_state *h)
{
  return make_server_in(g_dir, h);
}

// ---------------------------------------------------------------- client side

static int client_connect(const char *path)
{
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void client_send_raw(int fd, const void *data, size_t n)
{
  const char *p = data;
  while (n > 0) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0) return;
    p += w;
    n -= (size_t)w;
  }
}

static void client_send(int fd, uint32_t op, const char *payload)
{
  char frame[8 + CCORD_RPC_FRAME_MAX_PAYLOAD];
  size_t len = strlen(payload);
  CT_CHECK(ccord_rpc_frame_encode(frame, sizeof frame, op, payload, len) == CCORD_OK);
  client_send_raw(fd, frame, 8 + len);
}

static bool read_exact(int fd, char *buf, size_t n)
{
  size_t got = 0;
  while (got < n) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, 1000) <= 0) return false;
    ssize_t r = read(fd, buf + got, n - got);
    if (r <= 0) return false;   // EOF or ECONNRESET
    got += (size_t)r;
  }
  return true;
}

typedef struct reply {
  uint32_t op;
  char payload[CCORD_RPC_FRAME_MAX_PAYLOAD + 1];
  size_t len;
} reply;

static reply g_reply;

static void pump(ccord_rpc_server *s, uint64_t now, int rounds)
{
  for (int i = 0; i < rounds; i++) {
    (void)ccord_rpc_server_poll(s, now);
    usleep(2000);
  }
}

// Polls the server until a whole frame arrives on `fd`. False when none comes (or the peer closed first).
static bool expect_frame(ccord_rpc_server *s, int fd, uint64_t now)
{
  for (int i = 0; i < 100; i++) {
    (void)ccord_rpc_server_poll(s, now);
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, 10) <= 0) continue;
    char hdr[8];
    if (!read_exact(fd, hdr, sizeof hdr)) return false;
    uint32_t len = (uint32_t)(unsigned char)hdr[4] | ((uint32_t)(unsigned char)hdr[5] << 8) |
                   ((uint32_t)(unsigned char)hdr[6] << 16) | ((uint32_t)(unsigned char)hdr[7] << 24);
    g_reply.op = (uint32_t)(unsigned char)hdr[0];
    if (len > CCORD_RPC_FRAME_MAX_PAYLOAD || !read_exact(fd, g_reply.payload, len)) return false;
    g_reply.payload[len] = '\0';
    g_reply.len = len;
    return true;
  }
  return false;
}

// True once the server has closed `fd`: read() gives EOF or ECONNRESET (an unread-data close on AF_UNIX).
static bool expect_closed(ccord_rpc_server *s, int fd, uint64_t now)
{
  for (int i = 0; i < 100; i++) {
    (void)ccord_rpc_server_poll(s, now);
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, 10) <= 0) continue;
    char b[64];
    ssize_t r = read(fd, b, sizeof b);
    if (r == 0 || (r < 0 && errno == ECONNRESET)) return true;
  }
  return false;
}

// ---------------------------------------------------------------- reply inspection

static const ccord_json *reply_json(void)
{
  ccord_error err;
  return ccord_json_parse(&g_arena, g_reply.payload, g_reply.len, nullptr, &err);
}

static const char *jstr(const ccord_json *o, const char *key)
{
  const ccord_json *v = o ? ccord_json_get(o, key) : nullptr;
  return v ? ccord_json_string(v, nullptr) : nullptr;
}

static int64_t jint(const ccord_json *o, const char *key)
{
  const ccord_json *v = o ? ccord_json_get(o, key) : nullptr;
  int64_t out = -1;
  return v && ccord_json_int64(v, &out) ? out : -1;
}

static bool jis_null(const ccord_json *o, const char *key)
{
  const ccord_json *v = o ? ccord_json_get(o, key) : nullptr;
  return v && ccord_json_type_of(v) == CCORD_JSON_NULL;
}

static bool str_eq(const char *a, const char *b)
{
  return a && b && strcmp(a, b) == 0;
}

static bool ends_with(const char *s, const char *suffix)
{
  size_t n = strlen(s), m = strlen(suffix);
  return n >= m && strcmp(s + n - m, suffix) == 0;
}

// The last reply must be a CLOSE frame carrying `code`.
static bool is_close(int64_t code)
{
  if (g_reply.op != 2) return false;
  const ccord_json *j = reply_json();
  return j && jint(j, "code") == code;
}

// The last reply must be an evt ERROR frame with data.code 4000.
static bool is_error(const char *message)
{
  if (g_reply.op != 1) return false;
  const ccord_json *j = reply_json();
  const ccord_json *data = j ? ccord_json_get(j, "data") : nullptr;
  return j && str_eq(jstr(j, "evt"), "ERROR") && jint(data, "code") == CCORD_RPC_ERROR_CODE &&
         (!message || str_eq(jstr(data, "message"), message));
}

static int connect_ready(ccord_rpc_server *s, const char *path)
{
  int fd = client_connect(path);
  CT_CHECK(fd >= 0);
  if (fd < 0) return -1;
  client_send(fd, 0, HS);
  CT_CHECK(expect_frame(s, fd, T0));
  CT_CHECK(g_reply.op == 1);
  return fd;
}

// ---------------------------------------------------------------- tests

static void test_socket_slots(void)
{
  mode_t old_mask = umask(0);
  umask(old_mask);

  hook_state h = {0};
  ccord_rpc_server *a = make_server(&h);
  CT_CHECK(a != nullptr);
  if (!a) return;
  char path_a[PATH_MAX];
  snprintf(path_a, sizeof path_a, "%s", ccord_rpc_server_path(a));
  CT_CHECK(ends_with(path_a, "/discord-ipc-0"));
  CT_CHECK(strncmp(path_a, g_dir, strlen(g_dir)) == 0);

  struct stat st;
  CT_CHECK(stat(path_a, &st) == 0);
  CT_CHECK(S_ISSOCK(st.st_mode));
  CT_CHECK((st.st_mode & 0777) == 0600);   // owner-only, regardless of the process umask
  mode_t after = umask(0);
  umask(after);
  CT_CHECK(after == old_mask);             // the temporary umask(0177) was restored

  // A second server finds slot 0 held by a live peer and takes discord-ipc-1; the third takes -2.
  ccord_rpc_server *b = make_server(&h);
  CT_CHECK(b != nullptr);
  if (b) {
    CT_CHECK(ends_with(ccord_rpc_server_path(b), "/discord-ipc-1"));
    CT_CHECK(stat(ccord_rpc_server_path(b), &st) == 0 && (st.st_mode & 0777) == 0600);
  }
  ccord_rpc_server *c = make_server(&h);
  CT_CHECK(c != nullptr);
  if (c) CT_CHECK(ends_with(ccord_rpc_server_path(c), "/discord-ipc-2"));

  // The probe must not have disturbed the live first server: it still serves a client.
  int fd = connect_ready(a, path_a);
  CT_CHECK(h.ready == 1);
  close(fd);
  pump(a, T0, 5);
  CT_CHECK(h.disconnects == 1);

  // Destroy unlinks; the freed lowest slot is reused.
  ccord_rpc_server_destroy(a);
  CT_CHECK(stat(path_a, &st) != 0 && errno == ENOENT);
  ccord_rpc_server *a2 = make_server(&h);
  CT_CHECK(a2 && str_eq(ccord_rpc_server_path(a2), path_a));
  char path_b[PATH_MAX] = "", path_c[PATH_MAX] = "";
  snprintf(path_b, sizeof path_b, "%s", ccord_rpc_server_path(b));
  snprintf(path_c, sizeof path_c, "%s", ccord_rpc_server_path(c));
  ccord_rpc_server_destroy(a2);
  ccord_rpc_server_destroy(b);
  ccord_rpc_server_destroy(c);
  CT_CHECK(stat(path_b, &st) != 0 && stat(path_c, &st) != 0);
  CT_CHECK(str_eq(ccord_rpc_server_path(nullptr), ""));
  ccord_rpc_server_destroy(nullptr);   // accepts nullptr
}

static void test_stale_socket(void)
{
  char path0[PATH_MAX];
  snprintf(path0, sizeof path0, "%s/discord-ipc-0", g_dir);

  // A crashed instance leaves its socket node behind: bound, never unlinked, nobody listening.
  int dead = socket(AF_UNIX, SOCK_STREAM, 0);
  CT_CHECK(dead >= 0);
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  CT_CHECK(strlen(path0) < sizeof addr.sun_path);   // g_dir is short enough for sockaddr_un
  memcpy(addr.sun_path, path0, strnlen(path0, sizeof addr.sun_path - 1));
  CT_CHECK(bind(dead, (struct sockaddr *)&addr, sizeof addr) == 0);
  close(dead);
  struct stat st;
  CT_CHECK(stat(path0, &st) == 0);

  hook_state h = {0};
  ccord_rpc_server *s = make_server(&h);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(str_eq(ccord_rpc_server_path(s), path0));   // rebound in place, not pushed to slot 1
  int fd = connect_ready(s, path0);                    // and it really listens
  CT_CHECK(h.ready == 1);
  if (fd >= 0) close(fd);
  ccord_rpc_server_destroy(s);

  // Something that is not a socket in slot 0 is never unlinked: the server moves on to slot 1.
  FILE *f = fopen(path0, "w");
  CT_CHECK(f != nullptr);
  if (f) fclose(f);
  s = make_server(&h);
  CT_CHECK(s && ends_with(ccord_rpc_server_path(s), "/discord-ipc-1"));
  CT_CHECK(stat(path0, &st) == 0 && S_ISREG(st.st_mode));
  ccord_rpc_server_destroy(s);
  CT_CHECK(remove(path0) == 0);
}

static void test_dir_resolution(void)
{
  char *old_xdg = getenv("XDG_RUNTIME_DIR") ? strdup(getenv("XDG_RUNTIME_DIR")) : nullptr;
  char *old_tmp = getenv("TMPDIR") ? strdup(getenv("TMPDIR")) : nullptr;
  hook_state h = {0};
  ccord_error err;
  ccord_rpc_config cfg = {.ctx = &h};

  // Order: cfg.dir > $XDG_RUNTIME_DIR > $TMPDIR; with none of them the server is disabled (no /tmp fallback).
  CT_CHECK(setenv("XDG_RUNTIME_DIR", g_dir, 1) == 0);
  CT_CHECK(setenv("TMPDIR", "/nonexistent-ccord-tmp", 1) == 0);
  ccord_rpc_server *s = ccord_rpc_server_create(&cfg, &err);
  CT_CHECK(s && strncmp(ccord_rpc_server_path(s), g_dir, strlen(g_dir)) == 0);
  ccord_rpc_server_destroy(s);

  CT_CHECK(setenv("XDG_RUNTIME_DIR", "/nonexistent-ccord-xdg", 1) == 0);
  cfg.dir = g_dir;
  s = ccord_rpc_server_create(&cfg, &err);
  CT_CHECK(s && strncmp(ccord_rpc_server_path(s), g_dir, strlen(g_dir)) == 0);
  ccord_rpc_server_destroy(s);
  cfg.dir = nullptr;

  unsetenv("XDG_RUNTIME_DIR");
  CT_CHECK(setenv("TMPDIR", g_dir, 1) == 0);
  s = ccord_rpc_server_create(&cfg, &err);
  CT_CHECK(s && strncmp(ccord_rpc_server_path(s), g_dir, strlen(g_dir)) == 0);
  ccord_rpc_server_destroy(s);

  unsetenv("TMPDIR");
  s = ccord_rpc_server_create(&cfg, &err);
  CT_CHECK(s == nullptr && err.code == CCORD_ERR_UNSUPPORTED);
  ccord_rpc_server_destroy(s);

  // A directory that cannot hold the socket path, and one that does not exist, fail with distinct errors.
  char longdir[160];
  memset(longdir, 'd', sizeof longdir - 1);
  longdir[0] = '/';
  longdir[sizeof longdir - 1] = '\0';
  cfg.dir = longdir;
  s = ccord_rpc_server_create(&cfg, &err);
  CT_CHECK(s == nullptr && err.code == CCORD_ERR_INVALID);
  cfg.dir = "/nonexistent-ccord-dir";
  s = ccord_rpc_server_create(&cfg, &err);
  CT_CHECK(s == nullptr && err.code == CCORD_ERR_IO);

  if (old_xdg) (void)setenv("XDG_RUNTIME_DIR", old_xdg, 1); else unsetenv("XDG_RUNTIME_DIR");
  if (old_tmp) (void)setenv("TMPDIR", old_tmp, 1); else unsetenv("TMPDIR");
  free(old_xdg);
  free(old_tmp);
}

static void test_handshake_ready(void)
{
  hook_state h = {0};
  ccord_rpc_server *s = make_server(&h);
  CT_CHECK(s != nullptr);
  if (!s) return;
  int fd = client_connect(ccord_rpc_server_path(s));
  CT_CHECK(fd >= 0);
  client_send(fd, 0, HS);
  CT_CHECK(expect_frame(s, fd, T0));
  CT_CHECK(g_reply.op == 1);
  const ccord_json *j = reply_json();
  CT_CHECK(j != nullptr);
  CT_CHECK(str_eq(jstr(j, "cmd"), "DISPATCH"));
  CT_CHECK(str_eq(jstr(j, "evt"), "READY"));
  CT_CHECK(jis_null(j, "nonce"));
  const ccord_json *data = ccord_json_get(j, "data");
  CT_CHECK(jint(data, "v") == 1);
  const ccord_json *config = data ? ccord_json_get(data, "config") : nullptr;
  CT_CHECK(str_eq(jstr(config, "cdn_host"), "cdn.discordapp.com"));
  CT_CHECK(str_eq(jstr(config, "api_endpoint"), "//discord.com/api"));
  const ccord_json *user = data ? ccord_json_get(data, "user") : nullptr;
  CT_CHECK(str_eq(jstr(user, "id"), "42"));
  CT_CHECK(str_eq(jstr(user, "username"), "tester"));
  CT_CHECK(str_eq(jstr(user, "avatar"), "abc123"));
  CT_CHECK(h.get_user_calls == 1 && h.ready == 1);
  CT_CHECK(str_eq(h.ready_client_id, "1234567890123456789"));
  CT_CHECK(ccord_rpc_server_conn_count(s) == 1);
  close(fd);
  ccord_rpc_server_destroy(s);

  // Without a logged-in user READY still arrives, with user: null.
  hook_state h2 = {0};
  ccord_rpc_config cfg = {.dir = g_dir, .hooks = {.on_ready = hook_ready}, .ctx = &h2};
  ccord_error err;
  s = ccord_rpc_server_create(&cfg, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  fd = client_connect(ccord_rpc_server_path(s));
  client_send(fd, 0, HS);
  CT_CHECK(expect_frame(s, fd, T0));
  j = reply_json();
  data = j ? ccord_json_get(j, "data") : nullptr;
  CT_CHECK(jis_null(data, "user") && h2.ready == 1);
  close(fd);
  ccord_rpc_server_destroy(s);
}

static void check_handshake_close(const char *payload, int64_t code, const char *what)
{
  hook_state h = {0};
  ccord_rpc_server *s = make_server(&h);
  CT_CHECK(s != nullptr);
  if (!s) return;
  int fd = client_connect(ccord_rpc_server_path(s));
  client_send(fd, 0, payload);
  bool got = expect_frame(s, fd, T0);
  if (!got) fprintf(stderr, "no CLOSE reply for %s\n", what);
  CT_CHECK(got && is_close(code));
  if (got && !is_close(code)) fprintf(stderr, "wrong close for %s: %s\n", what, g_reply.payload);
  CT_CHECK(expect_closed(s, fd, T0));
  CT_CHECK(h.ready == 0 && h.disconnects == 0);   // never ready, so never reported
  CT_CHECK(ccord_rpc_server_conn_count(s) == 0);
  close(fd);
  ccord_rpc_server_destroy(s);
}

static void test_handshake_errors(void)
{
  check_handshake_close("{\"v\":2,\"client_id\":\"123\"}", CCORD_RPC_CLOSE_INVALID_VERSION, "v=2");
  check_handshake_close("{\"client_id\":\"123\"}", CCORD_RPC_CLOSE_INVALID_VERSION, "missing v");
  check_handshake_close("{\"v\":\"1\",\"client_id\":\"123\"}", CCORD_RPC_CLOSE_INVALID_VERSION, "string v");
  check_handshake_close("{nope", CCORD_RPC_CLOSE_INVALID_ENCODING, "bad JSON");
  check_handshake_close("", CCORD_RPC_CLOSE_INVALID_ENCODING, "empty payload");
  check_handshake_close("[1]", CCORD_RPC_CLOSE_INVALID_ENCODING, "non-object");
  check_handshake_close("{\"v\":1,\"client_id\":\"12ab\"}", CCORD_RPC_CLOSE_INVALID_CLIENT_ID, "non-digit client_id");
  check_handshake_close("{\"v\":1}", CCORD_RPC_CLOSE_INVALID_CLIENT_ID, "missing client_id");
  check_handshake_close("{\"v\":1,\"client_id\":123}", CCORD_RPC_CLOSE_INVALID_CLIENT_ID, "numeric client_id");
  check_handshake_close("{\"v\":1,\"client_id\":\"123456789012345678901\"}", CCORD_RPC_CLOSE_INVALID_CLIENT_ID,
                        "21-digit client_id");

  hook_state h = {0};
  ccord_rpc_server *s = make_server(&h);
  CT_CHECK(s != nullptr);
  if (!s) return;

  // A FRAME before the handshake is closed with 1006.
  int fd = client_connect(ccord_rpc_server_path(s));
  client_send(fd, 1, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{},\"nonce\":\"n\"}");
  CT_CHECK(expect_frame(s, fd, T0) && is_close(CCORD_RPC_CLOSE_ABNORMAL));
  CT_CHECK(expect_closed(s, fd, T0));
  close(fd);
  CT_CHECK(h.activity_calls == 0);

  // A second HANDSHAKE on a ready connection is closed with 1006 and reported as a disconnect.
  fd = connect_ready(s, ccord_rpc_server_path(s));
  CT_CHECK(h.ready == 1);
  client_send(fd, 0, HS);
  CT_CHECK(expect_frame(s, fd, T0) && is_close(CCORD_RPC_CLOSE_ABNORMAL));
  CT_CHECK(expect_closed(s, fd, T0));
  CT_CHECK(h.disconnects == 1 && ccord_rpc_server_conn_count(s) == 0);
  close(fd);

  // A frame header with an unknown op is a protocol error (4005), not a hang.
  fd = client_connect(ccord_rpc_server_path(s));
  unsigned char bad[8] = {9, 0, 0, 0, 0, 0, 0, 0};
  client_send_raw(fd, bad, sizeof bad);
  CT_CHECK(expect_frame(s, fd, T0) && is_close(CCORD_RPC_CLOSE_INVALID_ENCODING));
  close(fd);
  ccord_rpc_server_destroy(s);
}

static void test_ping_pong(void)
{
  hook_state h = {0};
  ccord_rpc_server *s = make_server(&h);
  CT_CHECK(s != nullptr);
  if (!s) return;
  int fd = connect_ready(s, ccord_rpc_server_path(s));
  const char *ping = "{\"nonce\":\"abc-123\"}";
  client_send(fd, 3, ping);
  CT_CHECK(expect_frame(s, fd, T0));
  CT_CHECK(g_reply.op == 4 && g_reply.len == strlen(ping) && memcmp(g_reply.payload, ping, g_reply.len) == 0);

  // A frame split across writes, and two frames in one write, are both reassembled.
  char frame[8 + 16];
  CT_CHECK(ccord_rpc_frame_encode(frame, sizeof frame, 3, "split-payload", 13) == CCORD_OK);
  client_send_raw(fd, frame, 5);
  pump(s, T0, 3);
  client_send_raw(fd, frame + 5, 8 + 13 - 5);
  CT_CHECK(expect_frame(s, fd, T0));
  CT_CHECK(g_reply.op == 4 && g_reply.len == 13 && memcmp(g_reply.payload, "split-payload", 13) == 0);

  char two[2 * (8 + 1)];
  CT_CHECK(ccord_rpc_frame_encode(two, sizeof two, 3, "a", 1) == CCORD_OK);
  CT_CHECK(ccord_rpc_frame_encode(two + 9, sizeof two - 9, 3, "b", 1) == CCORD_OK);
  client_send_raw(fd, two, sizeof two);
  CT_CHECK(expect_frame(s, fd, T0) && g_reply.op == 4 && g_reply.payload[0] == 'a');
  CT_CHECK(expect_frame(s, fd, T0) && g_reply.op == 4 && g_reply.payload[0] == 'b');

  // PING also works before the handshake.
  int fd2 = client_connect(ccord_rpc_server_path(s));
  client_send(fd2, 3, "early");
  CT_CHECK(expect_frame(s, fd2, T0) && g_reply.op == 4 && g_reply.len == 5);
  close(fd2);
  close(fd);
  ccord_rpc_server_destroy(s);
}

static void test_set_activity(void)
{
  hook_state h = {0};
  ccord_rpc_server *s = make_server(&h);
  CT_CHECK(s != nullptr);
  if (!s) return;
  int fd = connect_ready(s, ccord_rpc_server_path(s));

  client_send(fd, 1,
              "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"set-1\",\"args\":{\"pid\":4242,\"activity\":{"
              "\"details\":\"In the lobby\",\"state\":\"Waiting\",\"timestamps\":{\"start\":1700000000},"
              "\"assets\":{\"large_image\":\"logo\"},\"party\":{\"id\":\"p1\",\"size\":[1,4]}}}}");
  CT_CHECK(expect_frame(s, fd, T0));
  CT_CHECK(g_reply.op == 1);
  CT_CHECK(h.activity_calls == 1 && h.cleared == 0);
  CT_CHECK(str_eq(h.last.details, "In the lobby") && str_eq(h.last.state, "Waiting"));
  CT_CHECK(str_eq(h.last.large_image, "logo") && h.last.party_size_cur == 1 && h.last.party_size_max == 4);
  CT_CHECK(h.last.start_ms == 1700000000000LL);
  const ccord_json *j = reply_json();
  CT_CHECK(j && str_eq(jstr(j, "cmd"), "SET_ACTIVITY") && str_eq(jstr(j, "nonce"), "set-1") && jis_null(j, "evt"));
  const ccord_json *data = j ? ccord_json_get(j, "data") : nullptr;
  CT_CHECK(str_eq(jstr(data, "details"), "In the lobby") && str_eq(jstr(data, "state"), "Waiting"));
  CT_CHECK(str_eq(jstr(data, "name"), "Rich Presence"));

  // args.activity null clears; so does an args object without the key. data is null and the nonce is echoed.
  client_send(fd, 1, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"clr-1\",\"args\":{\"pid\":4242,\"activity\":null}}");
  CT_CHECK(expect_frame(s, fd, T0));
  CT_CHECK(h.activity_calls == 2 && h.cleared == 1);
  j = reply_json();
  CT_CHECK(j && jis_null(j, "data") && str_eq(jstr(j, "nonce"), "clr-1") && str_eq(jstr(j, "cmd"), "SET_ACTIVITY"));
  client_send(fd, 1, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"clr-2\",\"args\":{\"pid\":4242}}");
  CT_CHECK(expect_frame(s, fd, T0));
  CT_CHECK(h.activity_calls == 3 && h.cleared == 2);
  j = reply_json();
  CT_CHECK(j && jis_null(j, "data") && str_eq(jstr(j, "nonce"), "clr-2"));

  // Invalid input is an evt ERROR, never a hook call, and the connection stays usable.
  client_send(fd, 1, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"bad-1\",\"args\":{\"activity\":{\"type\":4}}}");
  CT_CHECK(expect_frame(s, fd, T0) && is_error(nullptr));
  j = reply_json();
  CT_CHECK(j && str_eq(jstr(j, "nonce"), "bad-1") && str_eq(jstr(j, "cmd"), "SET_ACTIVITY"));
  client_send(fd, 1, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"bad-2\",\"args\":{\"activity\":\"text\"}}");
  CT_CHECK(expect_frame(s, fd, T0) && is_error(nullptr));
  client_send(fd, 1, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"bad-3\"}");
  CT_CHECK(expect_frame(s, fd, T0) && is_error("Invalid arguments"));
  client_send(fd, 1,
              "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"bad-4\",\"args\":{\"activity\":{\"buttons\":["
              "{\"label\":\"a\",\"url\":\"https://a.test\"},{\"label\":\"b\",\"url\":\"https://b.test\"},"
              "{\"label\":\"c\",\"url\":\"https://c.test\"}]}}}");
  CT_CHECK(expect_frame(s, fd, T0) && is_error(nullptr));
  client_send(fd, 1, "{not json");
  CT_CHECK(expect_frame(s, fd, T0) && is_error("Invalid payload"));
  client_send(fd, 1, "{\"nonce\":\"no-cmd\"}");
  CT_CHECK(expect_frame(s, fd, T0) && is_error("Invalid payload"));
  j = reply_json();
  CT_CHECK(j && str_eq(jstr(j, "nonce"), "no-cmd"));
  CT_CHECK(h.activity_calls == 3);

  // Unsupported commands and acked subscriptions.
  client_send(fd, 1, "{\"cmd\":\"GET_GUILDS\",\"nonce\":\"u-1\",\"args\":{}}");
  CT_CHECK(expect_frame(s, fd, T0) && is_error("Unsupported command"));
  j = reply_json();
  CT_CHECK(j && str_eq(jstr(j, "cmd"), "GET_GUILDS") && str_eq(jstr(j, "nonce"), "u-1"));
  client_send(fd, 1, "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"nonce\":\"sub-1\",\"args\":{}}");
  CT_CHECK(expect_frame(s, fd, T0) && g_reply.op == 1);
  j = reply_json();
  data = j ? ccord_json_get(j, "data") : nullptr;
  CT_CHECK(j && str_eq(jstr(j, "cmd"), "SUBSCRIBE") && str_eq(jstr(j, "nonce"), "sub-1") && jis_null(j, "evt"));
  CT_CHECK(str_eq(jstr(data, "evt"), "ACTIVITY_JOIN"));
  client_send(fd, 1, "{\"cmd\":\"UNSUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"nonce\":\"sub-2\"}");
  CT_CHECK(expect_frame(s, fd, T0) && g_reply.op == 1);
  j = reply_json();
  CT_CHECK(j && str_eq(jstr(j, "cmd"), "UNSUBSCRIBE") && jis_null(j, "evt") && str_eq(jstr(j, "nonce"), "sub-2"));

  close(fd);
  ccord_rpc_server_destroy(s);
}

static void test_disconnect(void)
{
  hook_state h = {0};
  ccord_rpc_server *s = make_server(&h);
  CT_CHECK(s != nullptr);
  if (!s) return;

  // EOF on a ready connection reports on_disconnect exactly once.
  int fd = connect_ready(s, ccord_rpc_server_path(s));
  CT_CHECK(h.ready == 1 && h.disconnects == 0);
  close(fd);
  pump(s, T0, 5);
  CT_CHECK(h.disconnects == 1 && ccord_rpc_server_conn_count(s) == 0);
  pump(s, T0, 3);
  CT_CHECK(h.disconnects == 1);

  // So does a client CLOSE frame (the connection is then dropped by the server).
  fd = connect_ready(s, ccord_rpc_server_path(s));
  client_send(fd, 2, "{}");
  pump(s, T0, 5);
  CT_CHECK(h.ready == 2 && h.disconnects == 2 && ccord_rpc_server_conn_count(s) == 0);
  CT_CHECK(expect_closed(s, fd, T0));
  close(fd);

  // A connection that never handshook is dropped silently.
  fd = client_connect(ccord_rpc_server_path(s));
  pump(s, T0, 3);
  CT_CHECK(ccord_rpc_server_conn_count(s) == 1);
  close(fd);
  pump(s, T0, 5);
  CT_CHECK(ccord_rpc_server_conn_count(s) == 0 && h.disconnects == 2);

  // Destroying the server reports the connections that are still ready.
  int live = connect_ready(s, ccord_rpc_server_path(s));
  CT_CHECK(h.ready == 3);
  ccord_rpc_server_destroy(s);
  CT_CHECK(h.disconnects == 3);
  close(live);
}

static void test_connection_limit(void)
{
  hook_state h = {0};
  ccord_rpc_server *s = make_server(&h);
  CT_CHECK(s != nullptr);
  if (!s) return;
  int fds[CCORD_RPC_MAX_CONNECTIONS];
  for (unsigned i = 0; i < CCORD_RPC_MAX_CONNECTIONS; i++) {
    fds[i] = client_connect(ccord_rpc_server_path(s));
    CT_CHECK(fds[i] >= 0);
  }
  pump(s, T0, 3);
  CT_CHECK(ccord_rpc_server_conn_count(s) == CCORD_RPC_MAX_CONNECTIONS);

  // The 9th is told 1006 and dropped; the eight accepted ones are untouched.
  int ninth = client_connect(ccord_rpc_server_path(s));
  CT_CHECK(ninth >= 0);
  CT_CHECK(expect_frame(s, ninth, T0) && is_close(CCORD_RPC_CLOSE_ABNORMAL));
  CT_CHECK(ccord_rpc_server_conn_count(s) == CCORD_RPC_MAX_CONNECTIONS);
  close(ninth);

  // A freed slot is usable again.
  close(fds[0]);
  pump(s, T0, 5);
  CT_CHECK(ccord_rpc_server_conn_count(s) == CCORD_RPC_MAX_CONNECTIONS - 1u);
  int again = connect_ready(s, ccord_rpc_server_path(s));
  CT_CHECK(h.ready == 1);
  close(again);
  for (unsigned i = 1; i < CCORD_RPC_MAX_CONNECTIONS; i++) close(fds[i]);
  ccord_rpc_server_destroy(s);
}

static void test_handshake_timeout(void)
{
  hook_state h = {0};
  ccord_rpc_server *s = make_server(&h);
  CT_CHECK(s != nullptr);
  if (!s) return;
  int idle = client_connect(ccord_rpc_server_path(s));
  CT_CHECK(idle >= 0);
  (void)ccord_rpc_server_poll(s, T0);   // accepted at T0 on the poll clock
  CT_CHECK(ccord_rpc_server_conn_count(s) == 1);

  // A client that handshakes in time is not affected by the deadline.
  int good = connect_ready(s, ccord_rpc_server_path(s));
  CT_CHECK(h.ready == 1);

  (void)ccord_rpc_server_poll(s, T0 + CCORD_RPC_HANDSHAKE_TIMEOUT_MS - 1u);
  CT_CHECK(ccord_rpc_server_conn_count(s) == 2);
  (void)ccord_rpc_server_poll(s, T0 + CCORD_RPC_HANDSHAKE_TIMEOUT_MS);
  CT_CHECK(ccord_rpc_server_conn_count(s) == 1);
  CT_CHECK(expect_frame(s, idle, T0 + CCORD_RPC_HANDSHAKE_TIMEOUT_MS) && is_close(CCORD_RPC_CLOSE_ABNORMAL));
  CT_CHECK(h.disconnects == 0 && ccord_rpc_server_conn_count(s) == 1);
  close(idle);
  close(good);
  ccord_rpc_server_destroy(s);
}

static void test_dead_pid_clears_activity(void)
{
  // Case A: explicit args.pid exits (zombie then reaped) while the IPC socket FD remains open.
  hook_state h = {0};
  ccord_rpc_server *s = make_server(&h);
  CT_CHECK(s != nullptr);
  if (!s) return;
  int fd = connect_ready(s, ccord_rpc_server_path(s));
  CT_CHECK(fd >= 0);

  int pfd[2];
  CT_CHECK(pipe(pfd) == 0);
  pid_t child = fork();
  CT_CHECK(child >= 0);
  if (child == 0) {
    close(pfd[1]);
    char b;
    (void)read(pfd[0], &b, 1);
    close(pfd[0]);
    _exit(0);
  }
  close(pfd[0]);

  char req[256];
  snprintf(req, sizeof req,
           "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":{\"details\":\"In Game\"}},\"nonce\":\"p1\"}",
           (long)child);
  client_send(fd, 1, req);
  CT_CHECK(expect_frame(s, fd, T0) && h.activity_calls == 1 && h.cleared == 0);

  // Child exits and becomes a zombie (not reaped yet); socket FD is still open in parent.
  close(pfd[1]);
  // Give the child a brief moment to transition to 'Z' in /proc/<pid>/stat.
  for (int i = 0; i < 50; i++) {
    (void)ccord_rpc_server_poll(s, T0 + 5000u + (uint64_t)i * 5000u);
    if (h.cleared == 1) break;
    usleep(2000);
  }
  CT_CHECK(h.cleared == 1 && h.activity_calls == 2 && ccord_rpc_server_conn_count(s) == 1);
  (void)waitpid(child, nullptr, 0);
  close(fd);
  ccord_rpc_server_destroy(s);

  // Case B: SO_PEERCRED fallback when args.pid is omitted and a helper child holds the socket FD open.
  h = (hook_state){0};
  s = make_server(&h);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(pipe(pfd) == 0);
  const char *spath = ccord_rpc_server_path(s);
  pid_t peer = fork();
  CT_CHECK(peer >= 0);
  if (peer == 0) {
    close(pfd[1]);
    int cfd = client_connect(spath);
    if (cfd < 0) _exit(1);
    client_send(cfd, 0, HS);
    char hdr[8], body[2048];
    if (!read_exact(cfd, hdr, sizeof hdr)) _exit(2);
    uint32_t rlen = (uint32_t)(unsigned char)hdr[4] | ((uint32_t)(unsigned char)hdr[5] << 8) |
                    ((uint32_t)(unsigned char)hdr[6] << 16) | ((uint32_t)(unsigned char)hdr[7] << 24);
    if (rlen > sizeof body || !read_exact(cfd, body, rlen)) _exit(3);
    pid_t holder = fork();
    if (holder == 0) {
      char b;
      (void)read(pfd[0], &b, 1);
      close(pfd[0]);
      close(cfd);
      _exit(0);
    }
    close(pfd[0]);
    client_send(cfd, 1, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"activity\":{\"details\":\"Game\"}},\"nonce\":\"p2\"}");
    if (!read_exact(cfd, hdr, sizeof hdr)) _exit(4);
    rlen = (uint32_t)(unsigned char)hdr[4] | ((uint32_t)(unsigned char)hdr[5] << 8) |
           ((uint32_t)(unsigned char)hdr[6] << 16) | ((uint32_t)(unsigned char)hdr[7] << 24);
    if (rlen <= sizeof body) (void)read_exact(cfd, body, rlen);
    close(cfd);
    _exit(0);
  }
  close(pfd[0]);
  for (int i = 0; i < 100 && h.activity_calls == 0; i++) {
    (void)ccord_rpc_server_poll(s, T0);
    usleep(2000);
  }
  CT_CHECK(h.activity_calls == 1 && h.cleared == 0);
  (void)waitpid(peer, nullptr, 0);
  (void)ccord_rpc_server_poll(s, T0 + 1000u);   // throttled to 5 s interval
  CT_CHECK(h.cleared == 0);
  (void)ccord_rpc_server_poll(s, T0 + 5000u);
  CT_CHECK(h.cleared == 1 && h.activity_calls == 2 && ccord_rpc_server_conn_count(s) == 1);
  close(pfd[1]);
  for (int i = 0; i < 100 && ccord_rpc_server_conn_count(s) > 0; i++) {
    (void)ccord_rpc_server_poll(s, T0 + 5000u);
    usleep(2000);
  }
  CT_CHECK(ccord_rpc_server_conn_count(s) == 0 && h.disconnects == 1);
  ccord_rpc_server_destroy(s);

  // Case C: nonexistent (e.g. Windows) PID in args.pid does not arm /proc monitoring or falsely clear.
  h = (hook_state){0};
  s = make_server(&h);
  CT_CHECK(s != nullptr);
  if (!s) return;
  fd = connect_ready(s, ccord_rpc_server_path(s));
  CT_CHECK(fd >= 0);
  client_send(fd, 1,
              "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":99999999,\"activity\":{\"details\":\"Wine\"}},\"nonce\":\"p3\"}");
  CT_CHECK(expect_frame(s, fd, T0) && h.activity_calls == 1 && h.cleared == 0);
  (void)ccord_rpc_server_poll(s, T0 + 6000u);
  CT_CHECK(h.cleared == 0 && ccord_rpc_server_conn_count(s) == 1);
  close(fd);
  ccord_rpc_server_destroy(s);
}

static void test_wait_info(void)
{
  ccord_socket_t fds[16];
  uint32_t to = 0;
  CT_CHECK(ccord_rpc_server_wait_info(nullptr, fds, 16, &to) == 0 && to == UINT32_MAX);

  hook_state h = {0};
  ccord_rpc_server *s = make_server(&h);
  CT_CHECK(s != nullptr);
  if (!s) return;
  (void)ccord_rpc_server_poll(s, T0);
  size_t nf = ccord_rpc_server_wait_info(s, fds, 16, &to);
  CT_CHECK(nf == 1 && fds[0] != CCORD_INVALID_SOCKET && to == UINT32_MAX);

  int fd = client_connect(ccord_rpc_server_path(s));
  CT_CHECK(fd >= 0);
  (void)ccord_rpc_server_poll(s, T0);
  nf = ccord_rpc_server_wait_info(s, fds, 16, &to);
  CT_CHECK(nf == 2 && to <= CCORD_RPC_HANDSHAKE_TIMEOUT_MS);

  client_send(fd, 0, HS);
  CT_CHECK(expect_frame(s, fd, T0));
  nf = ccord_rpc_server_wait_info(s, fds, 16, &to);
  CT_CHECK(nf == 2 && to == UINT32_MAX);

  char req[256];
  snprintf(req, sizeof req,
           "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":{\"details\":\"Play\"}},\"nonce\":\"w1\"}",
           (long)getpid());
  client_send(fd, 1, req);
  CT_CHECK(expect_frame(s, fd, T0));
  nf = ccord_rpc_server_wait_info(s, fds, 16, &to);
  CT_CHECK(nf == 2 && to <= 5000u);

  close(fd);
  ccord_rpc_server_destroy(s);
}

int main(void)
{
  const char *tmp = getenv("TMPDIR");
  snprintf(g_dir, sizeof g_dir, "%s/ccord_rpc_XXXXXX", (tmp && tmp[0]) ? tmp : ".");
  if (!mkdtemp(g_dir)) {
    perror("mkdtemp");
    return 1;
  }
  ccord_arena_init(&g_arena, 0);

  test_socket_slots();
  test_stale_socket();
  test_dir_resolution();
  test_handshake_ready();
  test_handshake_errors();
  test_ping_pong();
  test_set_activity();
  test_disconnect();
  test_connection_limit();
  test_handshake_timeout();
  test_dead_pid_clears_activity();
  test_wait_info();

  ccord_arena_free(&g_arena);
  (void)rmdir(g_dir);
  return CT_RESULT("test_rpc");
}
