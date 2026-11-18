// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE   // POSIX/GNU declarations (mkdtemp, setenv) that -std=c2x hides
// App-side Rich Presence wiring: per-connection activity slots, trailing-edge coalescing and the
// clear paths, exercised over a real AF_UNIX RPC socket against a CONNECTED mock-transport session
// (the mock records exactly what ccord_session_set_rich_presence pushed).
#include "app_rpc.h"

#include <ccord/core/event_bus.h>
#include <ccord/net/mock_transport.h>
#include <ccord/rpc/activity.h>
#include <ccord/rpc/rpc.h>
#include <ccord/store/store.h>

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "ccord_test.h"

// Every socket lives in a private mkdtemp directory under $TMPDIR (never a hard-coded /tmp).
static char g_dir[256];
static uint64_t g_clock = 1000;

static uint64_t fake_now(void *ud)
{
  (void)ud;
  return g_clock;
}

typedef struct env {
  ccord_bus *bus;
  ccord_store *store;
  ccord_transport *tr;
  ccord_session *session;
  ccord_app_rpc *rpc;
} env;

static bool env_setup(env *e, uint32_t coalesce_ms)
{
  memset(e, 0, sizeof *e);
  ccord_error err;
  e->bus = ccord_bus_create(&err);
  e->store = e->bus ? ccord_store_create(e->bus, &err) : nullptr;
  ccord_mock_config mcfg = {.seed = 11, .now_ms = fake_now};
  e->tr = e->store ? ccord_mock_transport_create(&mcfg, &err) : nullptr;
  e->session = e->tr ? ccord_session_create(e->bus, e->store, e->tr, &err) : nullptr;
  if (!e->session) return false;
  if (ccord_session_start(e->session, &err) != CCORD_OK) return false;
  for (int i = 0; i < 16 && ccord_session_state(e->session) != CCORD_CONN_CONNECTED; i++) {
    g_clock += 10;
    (void)ccord_session_pump(e->session);
  }
  if (ccord_session_state(e->session) != CCORD_CONN_CONNECTED) return false;
  ccord_app_rpc_config cfg = {.session = e->session, .dir = g_dir, .coalesce_ms = coalesce_ms};
  e->rpc = ccord_app_rpc_create(&cfg, &err);
  if (!e->rpc) fprintf(stderr, "ccord_app_rpc_create: %s\n", err.message);
  return e->rpc != nullptr;
}

static void env_teardown(env *e)
{
  ccord_app_rpc_destroy(e->rpc);
  e->rpc = nullptr;
  ccord_session_destroy(e->session);   // owns the transport
  ccord_store_destroy(e->store);
  ccord_bus_destroy(e->bus);
}

static void poll_at(env *e, uint64_t now)
{
  g_clock = now;
  ccord_app_rpc_poll(e->rpc, g_clock);
}

static const char *rich(env *e)
{
  size_t len = 0;
  return ccord_mock_rich_activities(e->tr, &len);
}

// ---------------------------------------------------------------- client side

static int cli_connect(const char *path)
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

static void cli_send(int fd, uint32_t op, const char *json)
{
  char frame[4096];
  size_t len = strlen(json);
  CT_CHECK(8u + len <= sizeof frame);
  CT_CHECK(ccord_rpc_frame_encode(frame, sizeof frame, op, json, len) == CCORD_OK);
  size_t off = 0;
  while (off < 8u + len) {
    ssize_t n = send(fd, frame + off, 8u + len - off, MSG_NOSIGNAL);
    if (n <= 0) return;
    off += (size_t)n;
  }
}

// Reads one reply frame, keeping the poll-driven server moving while waiting. The server is serviced
// at the CURRENT fake clock (never advanced here), so a change lands at the time the test set.
static bool cli_recv(env *e, int fd, uint32_t *op, char *out, size_t cap)
{
  unsigned char hdr[8];
  size_t got = 0;
  for (int round = 0; round < 1000 && got < sizeof hdr; round++) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int pr = poll(&pfd, 1, 5);
    if (pr < 0) return false;
    if (pr == 0) {
      poll_at(e, g_clock);
      continue;
    }
    ssize_t r = read(fd, hdr + got, sizeof hdr - got);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  if (got < sizeof hdr) return false;
  uint32_t o = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
  uint32_t len = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
  if (len >= cap) return false;
  got = 0;
  while (got < len) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, 1000) <= 0) return false;
    ssize_t r = read(fd, out + got, len - got);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  out[len] = '\0';
  *op = o;
  return true;
}

static bool cli_handshake(env *e, int fd)
{
  cli_send(fd, 0, "{\"v\":1,\"client_id\":\"1234567890123456789\"}");
  uint32_t op = 0;
  char reply[4096];
  return cli_recv(e, fd, &op, reply, sizeof reply) && op == 1 && strstr(reply, "\"evt\":\"READY\"") != nullptr;
}

// SET_ACTIVITY with an activity object (or null to clear); drains the command reply.
static bool cli_set_activity(env *e, int fd, const char *activity_json)
{
  char cmd[1024];
  snprintf(cmd, sizeof cmd, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"activity\":%s}}",
           activity_json ? activity_json : "null");
  cli_send(fd, 1, cmd);
  uint32_t op = 0;
  char reply[4096];
  return cli_recv(e, fd, &op, reply, sizeof reply) && op == 1 && strstr(reply, "\"cmd\":\"SET_ACTIVITY\"") != nullptr;
}

// ---------------------------------------------------------------- tests

// Trailing-edge coalescing: nothing is pushed until the window elapses, and every new change
// restarts the window from its own arrival time.
static void test_coalescing(void)
{
  env e;
  CT_CHECK(env_setup(&e, 2000));
  if (!e.rpc) return;
  int fd = cli_connect(ccord_app_rpc_path(e.rpc));
  CT_CHECK(fd >= 0);
  CT_CHECK(cli_handshake(&e, fd));
  g_clock = 10000;

  static const char game1[] = "[{\"name\":\"Game\",\"type\":0,\"instance\":false}]";
  CT_CHECK(cli_set_activity(&e, fd, "{\"name\":\"Game\",\"type\":0}"));   // lands at 10000: window runs to 12000
  poll_at(&e, 10000);
  CT_CHECK(rich(&e) == nullptr);
  poll_at(&e, 11999);
  CT_CHECK(rich(&e) == nullptr);
  poll_at(&e, 12000);
  CT_CHECK(rich(&e) != nullptr && strcmp(rich(&e), game1) == 0);

  // A change at 13000 opens a window to 15000; a second change at 14500 restarts it, so the flush
  // follows the LAST change (16500), not the first (which would have fired at 15000).
  g_clock = 13000;
  CT_CHECK(cli_set_activity(&e, fd, "{\"name\":\"Game2\",\"type\":0}"));
  poll_at(&e, 14499);
  CT_CHECK(strcmp(rich(&e), game1) == 0);
  g_clock = 14500;
  CT_CHECK(cli_set_activity(&e, fd, "{\"name\":\"Game3\",\"type\":0}"));
  poll_at(&e, 15000);
  CT_CHECK(strcmp(rich(&e), game1) == 0);
  poll_at(&e, 16499);
  CT_CHECK(strcmp(rich(&e), game1) == 0);
  poll_at(&e, 16500);
  CT_CHECK(rich(&e) != nullptr && strcmp(rich(&e), "[{\"name\":\"Game3\",\"type\":0,\"instance\":false}]") == 0);

  close(fd);
  env_teardown(&e);
}

// Two connections merge in slot order; an explicit null, a CLOSE frame and an EOF each remove only
// the activity of the connection that sent them.
static void test_merge_and_per_connection_clear(void)
{
  env e;
  CT_CHECK(env_setup(&e, 0));   // coalesce 0: every change flushes on its poll
  if (!e.rpc) return;
  int a = cli_connect(ccord_app_rpc_path(e.rpc));
  int b = cli_connect(ccord_app_rpc_path(e.rpc));
  CT_CHECK(a >= 0 && b >= 0);
  CT_CHECK(cli_handshake(&e, a));
  CT_CHECK(cli_handshake(&e, b));

  CT_CHECK(cli_set_activity(&e, a, "{\"name\":\"Alpha\",\"type\":0}"));
  poll_at(&e, g_clock + 10);
  CT_CHECK(rich(&e) != nullptr && strcmp(rich(&e), "[{\"name\":\"Alpha\",\"type\":0,\"instance\":false}]") == 0);

  CT_CHECK(cli_set_activity(&e, b, "{\"name\":\"Beta\",\"type\":2}"));
  poll_at(&e, g_clock + 10);
  CT_CHECK(rich(&e) != nullptr &&
           strcmp(rich(&e), "[{\"name\":\"Alpha\",\"type\":0,\"instance\":false},"
                            "{\"name\":\"Beta\",\"type\":2,\"instance\":false}]") == 0);

  // Explicit clear on A keeps B.
  CT_CHECK(cli_set_activity(&e, a, nullptr));
  poll_at(&e, g_clock + 10);
  CT_CHECK(rich(&e) != nullptr && strcmp(rich(&e), "[{\"name\":\"Beta\",\"type\":2,\"instance\":false}]") == 0);

  // EOF on B clears the last activity.
  close(b);
  poll_at(&e, g_clock + 10);
  CT_CHECK(rich(&e) == nullptr);

  // CLOSE frame on A removes its activity too.
  CT_CHECK(cli_set_activity(&e, a, "{\"name\":\"Alpha2\",\"type\":0}"));
  poll_at(&e, g_clock + 10);
  CT_CHECK(rich(&e) != nullptr && strcmp(rich(&e), "[{\"name\":\"Alpha2\",\"type\":0,\"instance\":false}]") == 0);
  cli_send(a, 2, "{\"code\":1000,\"message\":\"bye\"}");
  poll_at(&e, g_clock + 10);
  CT_CHECK(rich(&e) == nullptr);

  close(a);
  env_teardown(&e);
}

// Destroying the app_rpc disconnects the clients and clears their activities from the session while
// it is still alive; the socket node is unlinked.
static void test_destroy_clears(void)
{
  env e;
  CT_CHECK(env_setup(&e, 0));
  if (!e.rpc) return;
  char path[256];
  snprintf(path, sizeof path, "%s", ccord_app_rpc_path(e.rpc));
  int fd = cli_connect(path);
  CT_CHECK(fd >= 0);
  CT_CHECK(cli_handshake(&e, fd));
  CT_CHECK(cli_set_activity(&e, fd, "{\"name\":\"Gone\",\"type\":0}"));
  poll_at(&e, g_clock + 10);
  CT_CHECK(rich(&e) != nullptr);

  ccord_app_rpc_destroy(e.rpc);
  e.rpc = nullptr;
  CT_CHECK(rich(&e) == nullptr);
  struct stat st;
  CT_CHECK(stat(path, &st) != 0);

  close(fd);
  env_teardown(&e);
}

static void test_create_rejects(void)
{
  ccord_error err;
  CT_CHECK(ccord_app_rpc_create(nullptr, &err) == nullptr && err.code == CCORD_ERR_INVALID);
  ccord_app_rpc_config cfg = {.session = nullptr, .dir = g_dir};
  CT_CHECK(ccord_app_rpc_create(&cfg, &err) == nullptr && err.code == CCORD_ERR_INVALID);
  ccord_app_rpc_poll(nullptr, 0);      // null-safe
  ccord_app_rpc_destroy(nullptr);
  CT_CHECK(strcmp(ccord_app_rpc_path(nullptr), "") == 0);
}

int main(void)
{
  const char *tmp = getenv("TMPDIR");
  snprintf(g_dir, sizeof g_dir, "%s/ccord_app_rpc_XXXXXX", (tmp && tmp[0]) ? tmp : ".");
  if (!mkdtemp(g_dir)) {
    perror("mkdtemp");
    return 1;
  }
  test_create_rejects();
  test_coalescing();
  test_merge_and_per_connection_clear();
  test_destroy_clears();
  (void)rmdir(g_dir);
  return CT_RESULT("test_app_rpc");
}
