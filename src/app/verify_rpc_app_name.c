// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE   // mkdtemp, nanosleep: -std=c2x hides them
// `ccord --headless --mock --verify-slice rpc-app-name`: a local RPC client's activity shows under the real application
// name. A fake discord-ipc client handshakes with a client_id and sends SET_ACTIVITY without a name; the application
// is resolved from the client_id (fixture application in --mock), published with name + application_id, and the
// user-panel activity line reads "Playing <name>". Also: nothing is published while the lookup is pending or when it
// failed with no client name, a client name is used after a failed lookup, a failed lookup is retried with backoff,
// and a second SET_ACTIVITY from the same client_id makes no second lookup.
#include "verify_ui_parity.h"

#include "app_rpc.h"
#include "rpc_app_lookup.h"
#include "ui_internal.h"

#include <ccord/core/arena.h>
#include <ccord/core/json.h>
#include <ccord/net/app_lookup.h>
#include <ccord/net/mock_transport.h>
#include <ccord/rpc/activity.h>

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define ID_A "1234567890123456780"   // resolves through the lookup, no client name
#define ID_B "1234567890123456781"   // lookup fails first, no client name
#define ID_C "1234567890123456782"   // lookup fails, client sends a name

// A scripted source in front of the --mock fixture: counts lookups, can hold answers back, and can fail them.
typedef struct script {
  ccord_app_lookup_source inner;
  unsigned gets;
  bool hold;   // deliver nothing while true (the lookup stays pending)
  bool fail;   // get() fails while true
} script;

static ccord_status script_get(void *ctx, const char *id, ccord_app_lookup_done done, void *ud, ccord_error *err)
{
  script *s = ctx;
  s->gets++;
  if (s->fail) return ccord_error_set(err, CCORD_ERR_IO, "scripted lookup failure");
  return s->inner.get(s->inner.ctx, id, done, ud, err);
}

static void script_pump(void *ctx, uint64_t now_ms)
{
  script *s = ctx;
  if (!s->hold) s->inner.pump(s->inner.ctx, now_ms);
}

static void script_destroy(void *ctx)
{
  script *s = ctx;
  s->inner.destroy(s->inner.ctx);
}

typedef struct st {
  const ccord_verify_ui_parity_ctx *c;
  ccord_app_rpc *rpc;
  uint64_t now;      // virtual clock, ms
  int checks;
  int fd;            // current fake client
} st;

static int fail(const char *what)
{
  printf("rpc-app-name: FAIL %s\n", what);
  fflush(stdout);
  return 1;
}

#define CHECK(cond, what)         \
  do {                            \
    if (!(cond)) return fail(what); \
    s->checks++;                  \
  } while (0)

static void step(st *s, uint64_t advance_ms)
{
  s->now += advance_ms;
  ccord_app_rpc_poll(s->rpc, s->now);
  struct timespec ts = {.tv_nsec = 1000000L};
  nanosleep(&ts, nullptr);
}

static int client_connect(const char *path)
{
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  size_t plen = strlen(path);
  if (plen >= sizeof addr.sun_path) {
    close(fd);
    return -1;
  }
  memcpy(addr.sun_path, path, plen);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static bool send_frame(int fd, uint32_t op, const char *json)
{
  char frame[4096];
  size_t len = strlen(json);
  if (ccord_rpc_frame_encode(frame, sizeof frame, op, json, len) != CCORD_OK) return false;
  size_t off = 0;
  while (off < 8u + len) {
    ssize_t n = send(fd, frame + off, 8u + len - off, MSG_NOSIGNAL);
    if (n <= 0) return false;
    off += (size_t)n;
  }
  return true;
}

// Waits (servicing the RPC server) for one reply frame; true when it arrived.
static bool recv_frame(st *s, int fd, char *out, size_t cap)
{
  unsigned char hdr[8];
  size_t got = 0, want = sizeof hdr, len = 0;
  char *dst = (char *)hdr;
  for (int spins = 0; spins < 2000; spins++) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, 1) > 0) {
      ssize_t r = read(fd, dst + got, want - got);
      if (r <= 0) return false;
      got += (size_t)r;
      if (got == want && dst == (char *)hdr) {
        len = (size_t)hdr[4] | ((size_t)hdr[5] << 8) | ((size_t)hdr[6] << 16) | ((size_t)hdr[7] << 24);
        if (len >= cap) return false;
        if (len == 0) {
          out[0] = '\0';
          return true;
        }
        dst = out;
        got = 0;
        want = len;
      } else if (got == want) {
        out[len] = '\0';
        return true;
      }
      continue;
    }
    step(s, 10);
  }
  return false;
}

static bool handshake(st *s, int fd, const char *client_id)
{
  char hs[96], reply[4096];
  (void)snprintf(hs, sizeof hs, "{\"v\":1,\"client_id\":\"%s\"}", client_id);
  return send_frame(fd, 0, hs) && recv_frame(s, fd, reply, sizeof reply) && strstr(reply, "\"evt\":\"READY\"");
}

static bool set_activity(st *s, int fd, const char *activity_json)
{
  char cmd[2048], reply[4096];
  (void)snprintf(cmd, sizeof cmd, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"n\",\"args\":{\"pid\":4242,\"activity\":%s}}",
                 activity_json ? activity_json : "null");
  return send_frame(fd, 1, cmd) && recv_frame(s, fd, reply, sizeof reply) && strstr(reply, "SET_ACTIVITY");
}

// What the app handed the session last (the activities array the gateway would send), or null when none.
static const char *published(const st *s, size_t *len)
{
  return ccord_mock_rich_activities(s->c->transport, len);
}

// Services the app for `ms` of virtual time.
static void run_for(st *s, uint64_t ms)
{
  for (uint64_t t = 0; t < ms; t += 100) step(s, 100);
}

// Steps until something is published (true) or `ms` of virtual time pass.
static bool wait_published(st *s, uint64_t ms)
{
  for (uint64_t t = 0; t <= ms; t += 100) {
    size_t n = 0;
    const char *j = published(s, &n);
    if (j && n > 0) return true;
    step(s, 100);
  }
  return false;
}

static bool nothing_published(const st *s)
{
  size_t n = 0;
  const char *j = published(s, &n);
  return !j || n == 0;
}

static const char *jstr(const ccord_json *o, const char *key)
{
  const ccord_json *v = ccord_json_get(o, key);
  return v ? ccord_json_string(v, nullptr) : nullptr;
}

// First activity of the published array, parsed into `arena`.
static const ccord_json *first_activity(const st *s, ccord_arena *arena)
{
  size_t n = 0;
  const char *j = published(s, &n);
  if (!j || n == 0) return nullptr;
  const ccord_json *arr = ccord_json_parse(arena, j, n, nullptr, nullptr);
  return arr ? ccord_json_array_at(arr, 0) : nullptr;
}

int ccord_verify_rpc_app_name_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->transport || !ctx->store || !ctx->ui || !ctx->frames) return fail("invalid context");
  st state = {.c = ctx, .fd = -1};
  st *s = &state;
  int rc = 1;
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_rpc_app_lookup *lookup = nullptr;
  script scr = {0};
  int fd_a = -1, fd_b = -1, fd_c = -1;
  const ccord_json *act = nullptr;
  const char *nm = nullptr, *aid = nullptr;
  unsigned gets0 = 0, gets1 = 0;

  ctx->frames(ctx->ud, 2);   // the mock session is connected: it accepts rich presence

  char dir[512];
  const char *tmp = getenv("TMPDIR");
  (void)snprintf(dir, sizeof dir, "%s/ccord_ran_XXXXXX", (tmp && tmp[0]) ? tmp : ".");
  if (!mkdtemp(dir)) return fail("mkdtemp scratch dir");
  if (strlen(dir) + strlen("/discord-ipc-0") >= sizeof((struct sockaddr_un *)nullptr)->sun_path) {
    (void)rmdir(dir);
    return fail("TMPDIR path too long for an AF_UNIX socket");
  }

  scr.inner = ccord_app_lookup_mock_source();
  lookup = ccord_rpc_app_lookup_create((ccord_app_lookup_source){
    .get = script_get, .pump = script_pump, .destroy = script_destroy, .ctx = &scr});
  ccord_error err = {0};
  ccord_app_rpc_config cfg = {.session = ctx->session, .dir = dir, .coalesce_ms = 0, .lookup = lookup};
  s->rpc = lookup ? ccord_app_rpc_create(&cfg, &err) : nullptr;
  if (!s->rpc) {
    printf("rpc-app-name: FAIL start rpc server: %s\n", err.message);
    goto cleanup;
  }
  s->now = 10000;
  char path[600];
  (void)snprintf(path, sizeof path, "%s", ccord_app_rpc_path(s->rpc));

  // ---- 1. resolved name. The lookup is held back first: while pending nothing is published.
  scr.hold = true;
  fd_a = client_connect(path);
  if (fd_a < 0 || !handshake(s, fd_a, ID_A)) {
    rc = fail("client A handshake");
    goto cleanup;
  }
  static const char act_a[] =
    "{\"type\":0,\"details\":\"In the lobby\",\"state\":\"Waiting\",\"timestamps\":{\"start\":1700000000},"
    "\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Big\"},\"party\":{\"id\":\"p1\",\"size\":[1,4]},"
    "\"buttons\":[{\"label\":\"Join\",\"url\":\"https://example.test/j\"}]}";
  if (!set_activity(s, fd_a, act_a)) {
    rc = fail("client A SET_ACTIVITY");
    goto cleanup;
  }
  run_for(s, 500);
  if (!nothing_published(s)) {
    rc = fail("an activity was published while its application lookup was pending");
    goto cleanup;
  }
  s->checks++;
  if (scr.gets != 1) {
    rc = fail("pending activity did not start exactly one lookup");
    goto cleanup;
  }
  s->checks++;
  scr.hold = false;
  if (!wait_published(s, 3000)) {
    rc = fail("no activity published after the lookup finished");
    goto cleanup;
  }
  act = first_activity(s, &arena);
  nm = act ? jstr(act, "name") : nullptr;
  aid = act ? jstr(act, "application_id") : nullptr;
  if (!nm || strcmp(nm, CCORD_APP_LOOKUP_FIXTURE_NAME) != 0 || !aid || strcmp(aid, ID_A) != 0) {
    rc = fail("published activity lacks the fixture application's name / the client_id as application_id");
    goto cleanup;
  }
  s->checks += 2;
  {
    const ccord_json *ts = ccord_json_get(act, "timestamps");
    const ccord_json *as = ccord_json_get(act, "assets");
    const ccord_json *pa = ccord_json_get(act, "party");
    const char *det = jstr(act, "details");
    const char *sta = jstr(act, "state");
    const char *li = as ? jstr(as, "large_image") : nullptr;
    const ccord_json *btn = ccord_json_get(act, "buttons");
    if (!det || strcmp(det, "In the lobby") != 0 || !sta || strcmp(sta, "Waiting") != 0 || !ts || !as || !li ||
        strcmp(li, "logo") != 0 || !pa || !btn || ccord_json_array_len(btn) != 1) {
      rc = fail("details/state/timestamps/assets/party/buttons were not kept");
      goto cleanup;
    }
    s->checks++;
  }
  printf("rpc-app-name: resolved name from client_id\n");

  // ---- 2. the user-panel line: apply what was sent to the own user the way the gateway echo does.
  {
    ccord_id self = ccord_store_self_id(ctx->store);
    const ccord_user *me = ccord_store_user(ctx->store, self);
    if (!me) {
      rc = fail("no own user in the store");
      goto cleanup;
    }
    ccord_error uerr = {0};
    if (ccord_store_update_presence_detail(ctx->store, self, me->presence, me->status_text, "", "", 0, true,
                                           (ccord_activity_type)0, nm, jstr(act, "details"), jstr(act, "state"), 0, 0,
                                           &uerr) != CCORD_OK) {
      printf("rpc-app-name: FAIL apply presence: %s\n", uerr.message);
      goto cleanup;
    }
    char line[256];
    char want[256];
    (void)snprintf(want, sizeof want, "Playing %s", CCORD_APP_LOOKUP_FIXTURE_NAME);
    me = ccord_store_user(ctx->store, self);
    bool ok = me && ui_format_user_activity_line(ctx->ui, me, line, sizeof line);
    if (!ok || strcmp(line, want) != 0 || strstr(line, "Rich Presence") || strstr(line, "UNKNOWN")) {
      rc = fail("user panel line is not 'Playing <fixture name>'");
      goto cleanup;
    }
    s->checks++;
    printf("rpc-app-name: user panel shows application name\n");
  }

  // ---- 3. cache: a second SET_ACTIVITY from the same client_id makes no second lookup.
  if (!set_activity(s, fd_a, "{\"type\":0,\"details\":\"Round 2\"}")) {
    rc = fail("client A second SET_ACTIVITY");
    goto cleanup;
  }
  run_for(s, 300);
  act = first_activity(s, &arena);
  if (scr.gets != 1 || !act || !jstr(act, "details") || strcmp(jstr(act, "details"), "Round 2") != 0 ||
      !jstr(act, "name") || strcmp(jstr(act, "name"), CCORD_APP_LOOKUP_FIXTURE_NAME) != 0) {
    rc = fail("second SET_ACTIVITY from the same client_id looked the application up again or lost its name");
    goto cleanup;
  }
  s->checks++;
  // Client A leaves: its activity is cleared.
  close(fd_a);
  fd_a = -1;
  run_for(s, 500);
  if (!nothing_published(s)) {
    rc = fail("activity not cleared after client A disconnected");
    goto cleanup;
  }
  s->checks++;

  // ---- 4. failing lookup: nothing for a nameless activity, the client's name when it sent one, retry with backoff.
  scr.fail = true;
  gets0 = scr.gets;
  fd_b = client_connect(path);
  if (fd_b < 0 || !handshake(s, fd_b, ID_B) || !set_activity(s, fd_b, "{\"type\":0,\"details\":\"No name\"}")) {
    rc = fail("client B handshake / SET_ACTIVITY");
    goto cleanup;
  }
  run_for(s, 1000);
  if (scr.gets != gets0 + 1 || !nothing_published(s)) {
    rc = fail("a failed lookup with no client name must publish nothing");
    goto cleanup;
  }
  s->checks++;
  fd_c = client_connect(path);
  if (fd_c < 0 || !handshake(s, fd_c, ID_C) ||
      !set_activity(s, fd_c, "{\"name\":\"Client Named\",\"type\":0,\"details\":\"mine\"}")) {
    rc = fail("client C handshake / SET_ACTIVITY");
    goto cleanup;
  }
  run_for(s, 500);
  act = first_activity(s, &arena);
  if (!act || !jstr(act, "name") || strcmp(jstr(act, "name"), "Client Named") != 0 || !jstr(act, "application_id") ||
      strcmp(jstr(act, "application_id"), ID_C) != 0) {
    rc = fail("the client's own name is not used after a failed lookup");
    goto cleanup;
  }
  s->checks++;
  {
    size_t n = 0;
    const char *j = published(s, &n);
    const ccord_json *arr = j ? ccord_json_parse(&arena, j, n, nullptr, nullptr) : nullptr;
    if (!arr || ccord_json_array_len(arr) != 1) {
      rc = fail("the nameless activity of client B must not be published next to client C's");
      goto cleanup;
    }
    s->checks++;
  }
  // Same client_id again: still no new lookup inside the backoff window.
  gets1 = scr.gets;
  if (!set_activity(s, fd_c, "{\"name\":\"Client Named\",\"type\":0,\"details\":\"again\"}")) {
    rc = fail("client C second SET_ACTIVITY");
    goto cleanup;
  }
  run_for(s, 1000);
  if (scr.gets != gets1) {
    rc = fail("a second SET_ACTIVITY inside the backoff window started a lookup");
    goto cleanup;
  }
  s->checks++;
  // The service recovers: after the backoff both failed clients are retried; B's activity now appears under its name.
  scr.fail = false;
  run_for(s, 6000);
  if (scr.gets <= gets1) {
    rc = fail("a failed lookup was not retried after the backoff");
    goto cleanup;
  }
  s->checks++;
  {
    size_t n = 0;
    const char *j = published(s, &n);
    const ccord_json *arr = j ? ccord_json_parse(&arena, j, n, nullptr, nullptr) : nullptr;
    bool b_named = false;
    for (size_t i = 0; arr && i < ccord_json_array_len(arr); i++) {
      const ccord_json *a = ccord_json_array_at(arr, i);
      if (jstr(a, "details") && strcmp(jstr(a, "details"), "No name") == 0 && jstr(a, "name") &&
          strcmp(jstr(a, "name"), CCORD_APP_LOOKUP_FIXTURE_NAME) == 0) {
        b_named = true;
      }
    }
    if (!b_named) {
      rc = fail("client B's activity did not appear under the application name after the retry succeeded");
      goto cleanup;
    }
    s->checks++;
  }
  printf("rpc-app-name: fallback and cache ok\n");
  printf("rpc-app-name: PASS checks=%d\n", s->checks);
  rc = 0;

cleanup:
  const int fds[3] = {fd_a, fd_b, fd_c};
  for (int i = 0; i < 3; i++) {
    if (fds[i] >= 0) close(fds[i]);
  }
  if (s->rpc) ccord_app_rpc_destroy(s->rpc);
  ccord_rpc_app_lookup_destroy(lookup);   // destroys the scripted source and the fixture inside it
  ccord_arena_free(&arena);
  {
    char sock[640];
    for (int i = 0; i < 10; i++) {
      (void)snprintf(sock, sizeof sock, "%s/discord-ipc-%d", dir, i);
      (void)unlink(sock);
    }
    (void)rmdir(dir);
  }
  fflush(stdout);
  return rc;
}
