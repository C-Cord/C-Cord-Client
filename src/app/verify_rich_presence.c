// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// End-to-end headless Rich Presence verification (`ccord --headless --verify-slice rich-presence`).
//
// Runs the real app_rpc + session + live_transport pipeline against a forked loopback HTTP server and
// a slice-local fake Main Gateway that writes every op 3 payload it receives to <scratch>/op3-N.json.
// An in-process AF_UNIX client speaks the Discord RPC frame protocol to the app_rpc server bound in
// the same scratch directory (coalesce 0), so every check observes what actually reached the gateway
// wire: handshake/READY, PING/PONG, SET_ACTIVITY -> op 3 with all fields (seconds -> ms, buttons),
// survival across presence changes, invisible omission, two-client merge, CLOSE/EOF/clear removal,
// and bad-handshake rejection.
#include "fake_discord.h"
#undef _POSIX_C_SOURCE
#undef _DEFAULT_SOURCE
#include "fake_gateway.h"

#include "verify_rich_presence.h"

#include "app_rpc.h"

#include <poll.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <ccord/core/arena.h>
#include <ccord/core/json.h>
#include <ccord/net/live_transport.h>
#include <ccord/rpc/activity.h>

#define RP_OP3_CAP 16384u   // one op 3 payload: 1024 B presence + at most 8192 B of activities

typedef struct rp {
  const ccord_verify_rich_presence_ctx *c;
  char dir[512];
  fd_server http;
  fg_server gw;
  ccord_app_rpc *rpc;
  ccord_arena arena;
  char op3[RP_OP3_CAP];
  unsigned op3_seen;
  int checks;
} rp;

static int rp_fail(const char *what)
{
  fprintf(stderr, "ccord: --verify-slice rich-presence: FAILED %s\n", what);
  return 1;
}

static void rp_sleep_ms(unsigned ms)
{
  struct timespec ts = {.tv_sec = (time_t)(ms / 1000u), .tv_nsec = (long)(ms % 1000u) * 1000000L};
  nanosleep(&ts, nullptr);
}

// One virtual-time step: advance the clock, run a real app frame (session pump), then service the
// RPC server on the same virtual clock the app uses.
static void rp_step(rp *s)
{
  s->c->advance_ms(s->c->ud, 20);
  s->c->frames(s->c->ud, 1);
  ccord_app_rpc_poll(s->rpc, s->c->now_ms(s->c->ud));
  rp_sleep_ms(2);
}

// ---------------------------------------------------------------- fake servers

static size_t respond_http(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)ud;
  (void)idx;
  if (req && strstr(req->path, "/messages") != nullptr) {
    return fd_reply(out, cap, 200, "OK", nullptr, "[]");
  }
  if (req && strstr(req->path, "/profile") != nullptr) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"user\":{\"id\":\"42\",\"username\":\"me\"}}");
  }
  return fd_reply(out, cap, 204, "No Content", nullptr, "");
}

// Slice-local Main Gateway: IDENTIFY -> READY (user 42, no guilds), heartbeats acked, and every
// presence update (op 3) written verbatim to <scratch>/op3-N.json in arrival order.
static void script_rp_gw(void *ud, fg_conn *c)
{
  const char *dir = ud;
  if (!fg_handshake(c)) return;
  fg_log(c, "HANDSHAKE");
  if (!fg_send(c, "{\"op\":10,\"d\":{\"heartbeat_interval\":60000}}")) return;
  char buf[RP_OP3_CAP];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) return;
  if (strstr(buf, "\"op\":2") == nullptr) return;
  fg_log(c, "IDENTIFY");
  char ready[512];
  (void)snprintf(ready, sizeof ready, FG_READY, c->self_url);
  if (!fg_send(c, ready)) return;

  static unsigned op3_count = 0;   // child process: survives reconnects, files stay numbered
  for (;;) {
    int r = fg_recv(c, buf, sizeof buf, 100);
    if (r == FG_TIMEOUT) continue;
    if (r != FG_GOT_TEXT) {
      fg_log(c, r == FG_CLIENT_CLOSE ? "CLIENT-CLOSE" : "CLIENT-GONE");
      break;
    }
    if (strstr(buf, "\"op\":1,") != nullptr || strstr(buf, "\"op\":1}") != nullptr) {
      if (!fg_send(c, "{\"op\":11}")) break;
      continue;
    }
    if (strstr(buf, "\"op\":3") != nullptr) {
      char path[640];
      (void)snprintf(path, sizeof path, "%s/op3-%u.json", dir, op3_count);
      FILE *f = fopen(path, "w");
      if (f) {
        (void)fwrite(buf, 1, strlen(buf), f);
        (void)fclose(f);
      }
      fg_log(c, "OP3 %u", op3_count);
      op3_count++;
      continue;
    }
    fg_log(c, "OTHER %.60s", buf);
  }
}

// ---------------------------------------------------------------- in-process RPC client

static uint32_t rp_rd_le32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int rp_client_connect(const char *path)
{
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  (void)snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static bool rp_send_frame(int fd, uint32_t op, const char *json)
{
  char frame[4096];
  size_t len = strlen(json);
  if (8u + len > sizeof frame) return false;
  if (ccord_rpc_frame_encode(frame, sizeof frame, op, json, len) != CCORD_OK) return false;
  size_t off = 0;
  while (off < 8u + len) {
    ssize_t n = send(fd, frame + off, 8u + len - off, MSG_NOSIGNAL);
    if (n <= 0) return false;
    off += (size_t)n;
  }
  return true;
}

// Reads exactly n bytes, keeping the app and RPC server moving while waiting (the server only
// progresses inside poll). Real-time deadline via the monotonic clock.
static bool rp_read_exact(rp *s, int fd, void *buf, size_t n, uint64_t deadline_ms)
{
  size_t got = 0;
  while (got < n) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int pr = poll(&pfd, 1, 10);
    if (pr < 0) return false;
    if (pr > 0) {
      ssize_t r = read(fd, (char *)buf + got, n - got);
      if (r <= 0) return false;
      got += (size_t)r;
      continue;
    }
    if (fg_now_ms() >= deadline_ms) return false;
    rp_step(s);
  }
  return true;
}

static bool rp_recv_frame(rp *s, int fd, uint32_t *op, char *out, size_t cap)
{
  unsigned char hdr[8];
  uint64_t deadline = fg_now_ms() + 8000;
  if (!rp_read_exact(s, fd, hdr, sizeof hdr, deadline)) return false;
  uint32_t o = rp_rd_le32(hdr), len = rp_rd_le32(hdr + 4);
  if (o > CCORD_RPC_FRAME_MAX_OP || len > CCORD_RPC_FRAME_MAX_PAYLOAD || (size_t)len >= cap) return false;
  if (len > 0 && !rp_read_exact(s, fd, out, len, deadline)) return false;
  out[len] = '\0';
  *op = o;
  return true;
}

static bool rp_handshake(rp *s, int fd, const char *client_id)
{
  char hs[96];
  (void)snprintf(hs, sizeof hs, "{\"v\":1,\"client_id\":\"%s\"}", client_id);
  if (!rp_send_frame(fd, 0, hs)) return false;
  uint32_t op = 0;
  char reply[4096];
  if (!rp_recv_frame(s, fd, &op, reply, sizeof reply)) return false;
  return op == 1 && strstr(reply, "\"evt\":\"READY\"") != nullptr;
}

// SET_ACTIVITY with `activity_json` (or null to clear); waits for the command reply.
static bool rp_set_activity(rp *s, int fd, const char *activity_json, const char *nonce)
{
  char cmd[2048];
  int n = snprintf(cmd, sizeof cmd, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%s\",\"args\":{\"pid\":4242,\"activity\":%s}}",
                   nonce, activity_json ? activity_json : "null");
  if (n < 0 || (size_t)n >= sizeof cmd) return false;
  if (!rp_send_frame(fd, 1, cmd)) return false;
  uint32_t op = 0;
  char reply[4096];
  if (!rp_recv_frame(s, fd, &op, reply, sizeof reply)) return false;
  return op == 1 && strstr(reply, "\"cmd\":\"SET_ACTIVITY\"") != nullptr;
}

// ---------------------------------------------------------------- op 3 readback

// Pumps until <scratch>/op3-<idx>.json exists and parses; the returned value lives in s->arena
// until the next call.
static const ccord_json *rp_wait_op3(rp *s, unsigned idx)
{
  char path[640];
  (void)snprintf(path, sizeof path, "%s/op3-%u.json", s->dir, idx);
  uint64_t deadline = fg_now_ms() + 8000;
  while (fg_now_ms() < deadline) {
    rp_step(s);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    size_t n = fread(s->op3, 1, sizeof s->op3 - 1u, f);
    (void)fclose(f);
    if (n == 0) continue;   // the child may still be writing: retry
    s->op3[n] = '\0';
    ccord_arena_reset(&s->arena);
    ccord_error err;
    const ccord_json *j = ccord_json_parse(&s->arena, s->op3, n, nullptr, &err);
    if (j) {
      if (idx >= s->op3_seen) s->op3_seen = idx + 1u;
      return j;
    }
  }
  return nullptr;
}

static bool rp_jstr_is(const ccord_json *obj, const char *key, const char *want)
{
  const ccord_json *v = ccord_json_get(obj, key);
  const char *s = v ? ccord_json_string(v, nullptr) : nullptr;
  return s && strcmp(s, want) == 0;
}

static bool rp_jint_is(const ccord_json *obj, const char *key, int64_t want)
{
  const ccord_json *v = ccord_json_get(obj, key);
  int64_t x = 0;
  return v && ccord_json_int64(v, &x) && x == want;
}

// Direct value checks for array elements (which are not objects with keys).
static bool rp_str_is(const ccord_json *v, const char *want)
{
  const char *s = v ? ccord_json_string(v, nullptr) : nullptr;
  return s && strcmp(s, want) == 0;
}

static bool rp_int_is(const ccord_json *v, int64_t want)
{
  int64_t x = 0;
  return v && ccord_json_int64(v, &x) && x == want;
}

// The activities array of an op 3 payload (nullptr when the payload is not a well-formed op 3).
static const ccord_json *rp_op3_activities(const ccord_json *j, size_t *n)
{
  if (!j || !rp_jint_is(j, "op", 3)) return nullptr;
  const ccord_json *d = ccord_json_get(j, "d");
  const ccord_json *acts = d ? ccord_json_get(d, "activities") : nullptr;
  if (!acts || ccord_json_type_of(acts) != CCORD_JSON_ARRAY) return nullptr;
  *n = ccord_json_array_len(acts);
  return acts;
}

// ---------------------------------------------------------------- the slice

int ccord_verify_rich_presence_run(const ccord_verify_rich_presence_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->frames || !ctx->advance_ms || !ctx->now_ms || !ctx->set_transport) {
    return rp_fail("invalid context");
  }
  rp s = {.c = ctx};
  int rc = 1;
  int clients[4] = {-1, -1, -1, -1};   // A, B, C (bad handshake), D
  bool servers_up = false;
  ccord_arena_init(&s.arena, 0);

  // Scratch dir under $TMPDIR (never a hard-coded /tmp); the socket path must fit sockaddr_un.
  const char *tmp = getenv("TMPDIR");
  (void)snprintf(s.dir, sizeof s.dir, "%s/ccord_rp_XXXXXX", (tmp && tmp[0]) ? tmp : ".");
  if (!mkdtemp(s.dir)) return rp_fail("mkdtemp scratch dir");
  if (strlen(s.dir) + strlen("/discord-ipc-0") >= sizeof((struct sockaddr_un *)nullptr)->sun_path) {
    (void)rmdir(s.dir);
    return rp_fail("TMPDIR path too long for an AF_UNIX socket");
  }

  // SDL installs a SIGTERM handler (quit event); the forked fake servers would inherit it and
  // survive fg_stop/fd_stop's SIGTERM until their alarm fires. Fork with the default disposition,
  // then restore the app's handler (same as verify_expressions.c).
  struct sigaction sa_dfl, sa_old;
  memset(&sa_dfl, 0, sizeof sa_dfl);
  sa_dfl.sa_handler = SIG_DFL;
  sigemptyset(&sa_dfl.sa_mask);
  (void)sigaction(SIGTERM, &sa_dfl, &sa_old);
  bool http_up = fd_start(&s.http, respond_http, nullptr);
  bool gw_up = http_up && fg_start(&s.gw, script_rp_gw, s.dir);
  (void)sigaction(SIGTERM, &sa_old, nullptr);
  if (!gw_up) {
    if (http_up) (void)fd_stop(&s.http, nullptr, 0);
    (void)rmdir(s.dir);
    return rp_fail("start loopback HTTP/gateway servers");
  }
  servers_up = true;

  ccord_error err = {0};
  ccord_live_config lcfg = {
    .token = "REDACTED_VERIFY_TOKEN",
    .api_base = s.http.base,
    .gateway_url = s.gw.url,
    .request_timeout_ms = 3000,
    .now_ms = ctx->now_ms,
    .clock_ud = ctx->ud,
  };
  ccord_transport *tr = ccord_live_transport_create(&lcfg, &err);
  if (!tr) {
    rp_fail(err.message);
    goto cleanup;
  }
  if (ctx->set_transport(ctx->ud, tr, &err) != CCORD_OK) {
    rp_fail(err.message);
    goto cleanup;
  }

  // Wait for READY before the RPC server exists: every later op 3 is then caused by this slice.
  for (int i = 0; i < 400; i++) {
    ctx->frames(ctx->ud, 1);
    if (ccord_session_state(ctx->session) == CCORD_CONN_CONNECTED) break;
    rp_sleep_ms(5);
  }
  if (ccord_session_state(ctx->session) != CCORD_CONN_CONNECTED) {
    rp_fail("gateway did not connect");
    goto cleanup;
  }

  ccord_app_rpc_config rcfg = {.session = ctx->session, .dir = s.dir, .coalesce_ms = 0};
  s.rpc = ccord_app_rpc_create(&rcfg, &err);
  if (!s.rpc) {
    rp_fail(err.message);
    goto cleanup;
  }

  // 1. rpc_socket_bound: discord-ipc-0 exists in the scratch dir and is the server's bound path.
  {
    char path[640];
    (void)snprintf(path, sizeof path, "%s/discord-ipc-0", s.dir);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode) || strcmp(ccord_app_rpc_path(s.rpc), path) != 0) {
      rp_fail("rpc_socket_bound");
      goto cleanup;
    }
    s.checks++;
  }

  // 2. rpc_handshake_ready: HANDSHAKE -> DISPATCH/READY (v 1; no session user is exposed, so null).
  clients[0] = rp_client_connect(ccord_app_rpc_path(s.rpc));
  if (clients[0] < 0 || !rp_handshake(&s, clients[0], "1111111111111111111")) {
    rp_fail("rpc_handshake_ready");
    goto cleanup;
  }
  s.checks++;

  // 3. rpc_ping_pong: PING payload echoed back as PONG.
  {
    uint32_t op = 0;
    char reply[256];
    if (!rp_send_frame(clients[0], 3, "ping-123") || !rp_recv_frame(&s, clients[0], &op, reply, sizeof reply) ||
        op != 4 || strcmp(reply, "ping-123") != 0) {
      rp_fail("rpc_ping_pong");
      goto cleanup;
    }
    s.checks++;
  }

  // 4. rpc_set_activity_op3: every field on the wire, seconds converted to ms, buttons as labels
  // plus metadata.button_urls.
  {
    static const char activity[] =
      "{\"name\":\"C-Cord Verify\",\"type\":0,\"details\":\"editing presence\",\"state\":\"in the slice\","
      "\"timestamps\":{\"start\":1700000000,\"end\":1700003600},"
      "\"assets\":{\"large_image\":\"cover\",\"large_text\":\"Large art\",\"small_image\":\"badge\","
      "\"small_text\":\"Small art\"},"
      "\"party\":{\"id\":\"party-1\",\"size\":[2,5]},"
      "\"secrets\":{\"join\":\"join-secret\",\"spectate\":\"spectate-secret\",\"match\":\"match-secret\"},"
      "\"buttons\":[{\"label\":\"Play\",\"url\":\"https://example.com/play\"},"
      "{\"label\":\"Source\",\"url\":\"https://example.com/src\"}],\"instance\":true}";
    if (!rp_set_activity(&s, clients[0], activity, "n-set")) {
      rp_fail("rpc_set_activity_op3: SET_ACTIVITY reply");
      goto cleanup;
    }
    const ccord_json *j = rp_wait_op3(&s, 0);
    size_t n = 0;
    const ccord_json *acts = rp_op3_activities(j, &n);
    const ccord_json *d = j ? ccord_json_get(j, "d") : nullptr;
    if (!acts || n != 1 || !rp_jstr_is(d, "status", "online")) {
      rp_fail("rpc_set_activity_op3: op 3 shape");
      goto cleanup;
    }
    const ccord_json *a = ccord_json_array_at(acts, 0);
    const ccord_json *ts = ccord_json_get(a, "timestamps");
    const ccord_json *assets = ccord_json_get(a, "assets");
    const ccord_json *party = ccord_json_get(a, "party");
    const ccord_json *size = party ? ccord_json_get(party, "size") : nullptr;
    const ccord_json *secrets = ccord_json_get(a, "secrets");
    const ccord_json *buttons = ccord_json_get(a, "buttons");
    const ccord_json *meta = ccord_json_get(a, "metadata");
    const ccord_json *urls = meta ? ccord_json_get(meta, "button_urls") : nullptr;
    const ccord_json *inst = ccord_json_get(a, "instance");
    bool ok = rp_jstr_is(a, "name", "C-Cord Verify") && rp_jint_is(a, "type", 0) &&
              rp_jstr_is(a, "details", "editing presence") && rp_jstr_is(a, "state", "in the slice") &&
              ts && rp_jint_is(ts, "start", 1700000000000LL) && rp_jint_is(ts, "end", 1700003600000LL) &&
              assets && rp_jstr_is(assets, "large_image", "cover") && rp_jstr_is(assets, "large_text", "Large art") &&
              rp_jstr_is(assets, "small_image", "badge") && rp_jstr_is(assets, "small_text", "Small art") &&
              party && rp_jstr_is(party, "id", "party-1") && size && ccord_json_array_len(size) == 2 &&
              rp_int_is(ccord_json_array_at(size, 0), 2) && rp_int_is(ccord_json_array_at(size, 1), 5) &&
              secrets && rp_jstr_is(secrets, "join", "join-secret") &&
              rp_jstr_is(secrets, "spectate", "spectate-secret") && rp_jstr_is(secrets, "match", "match-secret") &&
              buttons && ccord_json_array_len(buttons) == 2 &&
              rp_str_is(ccord_json_array_at(buttons, 0), "Play") &&
              rp_str_is(ccord_json_array_at(buttons, 1), "Source") &&
              urls && ccord_json_array_len(urls) == 2 &&
              rp_str_is(ccord_json_array_at(urls, 0), "https://example.com/play") &&
              rp_str_is(ccord_json_array_at(urls, 1), "https://example.com/src") &&
              inst && ccord_json_type_of(inst) == CCORD_JSON_BOOL && ccord_json_bool(inst, false);
    if (!ok) {
      rp_fail("rpc_set_activity_op3: activity fields on the wire");
      goto cleanup;
    }
    s.checks++;
  }

  // 5. rpc_activity_survives_presence_change: a status + custom-status change keeps the activity,
  // spliced after the type-4 custom status.
  {
    if (ccord_session_set_presence(ctx->session, CCORD_PRESENCE_DND, "busy", &err) != CCORD_OK) {
      rp_fail("rpc_activity_survives_presence_change: set_presence");
      goto cleanup;
    }
    const ccord_json *j = rp_wait_op3(&s, 1);
    size_t n = 0;
    const ccord_json *acts = rp_op3_activities(j, &n);
    const ccord_json *d = j ? ccord_json_get(j, "d") : nullptr;
    if (!acts || n != 2 || !rp_jstr_is(d, "status", "dnd") ||
        !rp_jstr_is(ccord_json_array_at(acts, 0), "name", "Custom Status") ||
        !rp_jint_is(ccord_json_array_at(acts, 0), "type", 4) ||
        !rp_jstr_is(ccord_json_array_at(acts, 0), "state", "busy") ||
        !rp_jstr_is(ccord_json_array_at(acts, 1), "name", "C-Cord Verify")) {
      rp_fail("rpc_activity_survives_presence_change");
      goto cleanup;
    }
    s.checks++;
  }

  // 6. rpc_invisible_omits_activity: invisible drops the activities; going online restores them.
  {
    if (ccord_session_set_presence(ctx->session, CCORD_PRESENCE_OFFLINE, nullptr, &err) != CCORD_OK) {
      rp_fail("rpc_invisible_omits_activity: set invisible");
      goto cleanup;
    }
    const ccord_json *j = rp_wait_op3(&s, 2);
    size_t n = 0;
    const ccord_json *acts = rp_op3_activities(j, &n);
    const ccord_json *d = j ? ccord_json_get(j, "d") : nullptr;
    if (!acts || n != 0 || !rp_jstr_is(d, "status", "invisible")) {
      rp_fail("rpc_invisible_omits_activity");
      goto cleanup;
    }
    if (ccord_session_set_presence(ctx->session, CCORD_PRESENCE_ONLINE, nullptr, &err) != CCORD_OK) {
      rp_fail("rpc_invisible_omits_activity: restore online");
      goto cleanup;
    }
    j = rp_wait_op3(&s, 3);
    acts = rp_op3_activities(j, &n);
    if (!acts || n != 1 || !rp_jstr_is(ccord_json_array_at(acts, 0), "name", "C-Cord Verify")) {
      rp_fail("rpc_invisible_omits_activity: activity did not return online");
      goto cleanup;
    }
    s.checks++;
  }

  // 7. rpc_two_clients_merge: a second client's activity joins the first, in connection order.
  clients[1] = rp_client_connect(ccord_app_rpc_path(s.rpc));
  if (clients[1] < 0 || !rp_handshake(&s, clients[1], "2222222222222222222") ||
      !rp_set_activity(&s, clients[1], "{\"name\":\"Second App\",\"type\":2,\"state\":\"listening\"}", "n-b")) {
    rp_fail("rpc_two_clients_merge: second client");
    goto cleanup;
  }
  {
    const ccord_json *j = rp_wait_op3(&s, 4);
    size_t n = 0;
    const ccord_json *acts = rp_op3_activities(j, &n);
    if (!acts || n != 2 || !rp_jstr_is(ccord_json_array_at(acts, 0), "name", "C-Cord Verify") ||
        !rp_jstr_is(ccord_json_array_at(acts, 1), "name", "Second App") ||
        !rp_jint_is(ccord_json_array_at(acts, 1), "type", 2)) {
      rp_fail("rpc_two_clients_merge");
      goto cleanup;
    }
    s.checks++;
  }

  // 8. rpc_close_clears: client A's CLOSE frame removes only A's activity.
  if (!rp_send_frame(clients[0], 2, "{\"code\":1000,\"message\":\"bye\"}")) {
    rp_fail("rpc_close_clears: send CLOSE");
    goto cleanup;
  }
  {
    const ccord_json *j = rp_wait_op3(&s, 5);
    size_t n = 0;
    const ccord_json *acts = rp_op3_activities(j, &n);
    if (!acts || n != 1 || !rp_jstr_is(ccord_json_array_at(acts, 0), "name", "Second App")) {
      rp_fail("rpc_close_clears");
      goto cleanup;
    }
    close(clients[0]);
    clients[0] = -1;
    s.checks++;
  }

  // 9. rpc_disconnect_clears: client B's EOF removes the last activity (presence cleared).
  close(clients[1]);
  clients[1] = -1;
  {
    const ccord_json *j = rp_wait_op3(&s, 6);
    size_t n = 0;
    const ccord_json *acts = rp_op3_activities(j, &n);
    if (!acts || n != 0) {
      rp_fail("rpc_disconnect_clears");
      goto cleanup;
    }
    s.checks++;
  }

  // 10. rpc_bad_handshake_rejected: a wrong protocol version gets CLOSE 4004, no READY.
  clients[2] = rp_client_connect(ccord_app_rpc_path(s.rpc));
  if (clients[2] < 0 || !rp_send_frame(clients[2], 0, "{\"v\":9,\"client_id\":\"3333333333333333333\"}")) {
    rp_fail("rpc_bad_handshake_rejected: send");
    goto cleanup;
  }
  {
    uint32_t op = 0;
    char reply[256];
    if (!rp_recv_frame(&s, clients[2], &op, reply, sizeof reply) || op != 2 ||
        strstr(reply, "\"code\":4004") == nullptr) {
      rp_fail("rpc_bad_handshake_rejected");
      goto cleanup;
    }
    close(clients[2]);
    clients[2] = -1;
    s.checks++;
  }

  // 11. rpc_explicit_clear: SET_ACTIVITY with activity null clears that client's activity.
  clients[3] = rp_client_connect(ccord_app_rpc_path(s.rpc));
  if (clients[3] < 0 || !rp_handshake(&s, clients[3], "4444444444444444444") ||
      !rp_set_activity(&s, clients[3], "{\"name\":\"Third App\",\"type\":0}", "n-d")) {
    rp_fail("rpc_explicit_clear: set");
    goto cleanup;
  }
  {
    const ccord_json *j = rp_wait_op3(&s, 7);
    size_t n = 0;
    const ccord_json *acts = rp_op3_activities(j, &n);
    if (!acts || n != 1 || !rp_jstr_is(ccord_json_array_at(acts, 0), "name", "Third App")) {
      rp_fail("rpc_explicit_clear: activity missing before clear");
      goto cleanup;
    }
  }
  if (!rp_set_activity(&s, clients[3], nullptr, "n-clear")) {
    rp_fail("rpc_explicit_clear: clear reply");
    goto cleanup;
  }
  {
    const ccord_json *j = rp_wait_op3(&s, 8);
    size_t n = 0;
    const ccord_json *acts = rp_op3_activities(j, &n);
    if (!acts || n != 0) {
      rp_fail("rpc_explicit_clear");
      goto cleanup;
    }
    s.checks++;
  }
  rc = 0;

cleanup:
  ccord_app_rpc_destroy(s.rpc);
  for (size_t i = 0; i < 4; i++) {
    if (clients[i] >= 0) close(clients[i]);
  }
  if (servers_up) {
    (void)fg_stop(&s.gw, nullptr, 0);
    (void)fd_stop(&s.http, nullptr, 0);
  }
  for (unsigned i = 0; i < s.op3_seen + 2u; i++) {
    char path[640];
    (void)snprintf(path, sizeof path, "%s/op3-%u.json", s.dir, i);
    (void)unlink(path);
  }
  (void)rmdir(s.dir);
  ccord_arena_free(&s.arena);
  if (rc != 0) return 1;
  printf("rich-presence: op3_updates=%u clients=4\n", s.op3_seen);
  printf("rich-presence: PASS checks=%d\n", s.checks);
  return 0;
}
