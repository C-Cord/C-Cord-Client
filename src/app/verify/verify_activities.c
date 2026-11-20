// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// End-to-end headless verification of Embedded Activities
// (`ccord --headless --verify-slice activities`).
//
// Drives the REAL app pipeline (live_transport + session + store + ui + renderer) against forked loopback Discord
// HTTP + Main Gateway servers (tests/support/fake_discord.h + fake_gateway.h), the same machinery as the other
// parity slices (verify_membership_screening.c). The fakes behave like Discord does for an activity-capable
// guild voice channel (3121 in guild 2001):
//   - GET /activities/shelf?guild_id=2001 answers three web configs in scrambled order (ranks 3, 1, 2);
//   - POST /activities/3121/<app> only succeeds (204) when its body carries the READY session id
//     ("sess-act-1"); otherwise 400. The instance itself arrives through the gateway, never in the answer;
//   - POST /applications/<app>/activities/3121/instances/<id>/leave answers 204;
//   - the gateway answers op 4 with a self VOICE_STATE_UPDATE (join / leave) and dispatches
//     EMBEDDED_ACTIVITY_UPDATE_V2 when a marker file appears: `launched` (made by the HTTP child on a valid
//     launch) -> instance 880001 with self; `second` (parent) -> self + user 7; `left` (HTTP child on the leave
//     POST of 880001) -> only user 7; `ended` (parent) -> no participants; `inst2` (parent) -> instance 880002
//     with self + user 7.
// The forked servers share state through marker files in a mkdtemp dir under $TMPDIR, so every launch/leave
// dispatch is caused by a real request. A last leg runs launch/leave/voice-disconnect over the offline mock
// transport (private bus/store/session) to prove live/mock parity.
//
// Output: `activities: PASS checks=10` on success; `ccord: --verify-slice activities: FAILED <what>` on stderr
// and a non-zero exit otherwise.
#include "fake_discord.h"
#undef _POSIX_C_SOURCE
#undef _DEFAULT_SOURCE
#include "fake_gateway.h"

#include "verify_parity_slices.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ccord/core/alloc.h>
#include <ccord/core/event_bus.h>
#include <ccord/net/admin.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>

// The activity debug seams, the stage channel and the host opener are UI-module internals.
#include "ui_internal.h"

#define AV_SLICE "activities"
#define AV_GUILD 2001ULL
#define AV_TEXT_CHANNEL 3120ULL
#define AV_VOICE 3121ULL
#define AV_SELF 42ULL
#define AV_OTHER 7ULL
#define AV_SESSION "sess-act-1"
#define AV_APP1 97001ULL
#define AV_APP2 97002ULL
#define AV_APP3 97003ULL
#define AV_INST1 880001ULL
#define AV_INST2 880002ULL
#define AV_WAIT_MAX 1200                // pump ticks per wait loop (100 ms virtual + ~5 ms real each)
#define AV_SETTLE_TICKS 30              // ticks that prove a request did NOT happen
#define AV_MAX_HITS 256
#define AV_MARK_MAX 512

static const ccord_verify_parity_ctx *a_ctx;
static fd_server *a_http_server;
static fd_hit a_hits[AV_MAX_HITS];
static size_t a_nhits;

// ---- small helpers (same shapes as the ms_* scaffolding in verify_membership_screening.c) ---------------------

static void av_sleep_ms(unsigned ms)
{
  struct timespec ts = {.tv_sec = (time_t)(ms / 1000u), .tv_nsec = (long)(ms % 1000u) * 1000000L};
  nanosleep(&ts, nullptr);
}

static int av_fail(const char *what)
{
  fprintf(stderr, "ccord: --verify-slice " AV_SLICE ": FAILED %s\n", what);
  return 1;
}

// The fd child writes one fd_hit per request into a pipe the parent normally only drains at fd_stop. Drain it
// incrementally (records are <= PIPE_BUF, so writes are atomic) so a long slice can never block the child.
static void av_drain_hits(void)
{
  if (!a_http_server) return;
  for (;;) {
    fd_hit tmp;
    size_t got = 0;
    while (got < sizeof tmp) {
      ssize_t n = read(a_http_server->pipe_rd, (char *)&tmp + got, sizeof tmp - got);
      if (n <= 0) break;
      got += (size_t)n;
    }
    if (got < sizeof tmp) break;
    if (a_nhits < AV_MAX_HITS) a_hits[a_nhits++] = tmp;
  }
}

// One virtual frame + a real-time breather so the forked servers can answer.
static void av_tick(void)
{
  a_ctx->frames(a_ctx->ud, 1);
  av_sleep_ms(5);
  av_drain_hits();
}

static void av_ticks(int n)
{
  for (int i = 0; i < n; i++) av_tick();
}

// Poll `cond` (re-evaluated after each tick) until it holds or the wait budget runs out.
#define AV_WAIT_UNTIL(cond)                                                                                         \
  do {                                                                                                              \
    for (int w_ = 0; w_ < AV_WAIT_MAX && !(cond); w_++) av_tick();                                                  \
  } while (0)

static void av_click(float x, float y)
{
  ccord_input_event ev = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  a_ctx->input(a_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  a_ctx->input(a_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y};
  a_ctx->input(a_ctx->ud, &ev);
}

static void av_click_rect(ccord_rect r) { av_click(r.x + r.w * 0.5f, r.y + r.h * 0.5f); }

// Extract a plain JSON string value ("key":"...") from a request body. Test payloads are ASCII with no quotes.
static bool av_json_str(const char *body, const char *key, char *out, size_t cap)
{
  char pat[64];
  (void)snprintf(pat, sizeof pat, "\"%s\":\"", key);
  const char *p = strstr(body, pat);
  if (!p) return false;
  p += strlen(pat);
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
  out[n] = '\0';
  return true;
}

static const fd_hit *av_find_hit(const char *method, const char *path)
{
  for (size_t i = 0; i < a_nhits; i++) {
    if (strcmp(a_hits[i].method, method) == 0 && strcmp(a_hits[i].path, path) == 0) return &a_hits[i];
  }
  return nullptr;
}

static size_t av_count_hits(const char *method, const char *path_substr)
{
  size_t n = 0;
  for (size_t i = 0; i < a_nhits; i++) {
    if (strcmp(a_hits[i].method, method) == 0 && strstr(a_hits[i].path, path_substr) != nullptr) n++;
  }
  return n;
}

static bool av_make_scratch(char *out, size_t cap)
{
  const char *base = getenv("TMPDIR");
  if (!base || !base[0]) base = ".";
  int n = snprintf(out, cap, "%s/ccord-activities-XXXXXX", base);
  if (n <= 0 || (size_t)n >= cap) return false;
  return mkdtemp(out) != nullptr;
}

static void av_touch(const char *path)
{
  int fd = open(path, O_CREAT | O_WRONLY, 0600);
  if (fd >= 0) (void)close(fd);
}

static bool av_exists(const char *path) { return access(path, F_OK) == 0; }

// ---- fake HTTP server (runs in the forked child) -----------------------------------------------------------

#define AV_LAUNCH_PATH "/activities/" "3121" "/" "97001"
#define AV_LEAVE1_PATH "/applications/97001/activities/3121/instances/880001/leave"
#define AV_LEAVE2_PATH "/applications/97002/activities/3121/instances/880002/leave"

typedef struct av_http_ud {
  char launched_marker[AV_MARK_MAX];   // created when a launch POST carries the READY session id
  char left_marker[AV_MARK_MAX];       // created when instance 880001 is left
} av_http_ud;

static size_t av_respond(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  av_http_ud *u = ud;
  (void)idx;
  const char *p = req->path;
  bool get = strcmp(req->method, "GET") == 0;
  bool post = strcmp(req->method, "POST") == 0;

  if (get && strcmp(p, "/activities/shelf?guild_id=2001") == 0) {
    // Scrambled on purpose: the client must order the shelf by shelf_rank (1, 2, 3).
    return fd_reply(out, cap, 200, "OK", nullptr,
      "{\"activities\":["
      "{\"application_id\":\"97003\",\"shelf_rank\":3,\"platform\":\"web\"},"
      "{\"application_id\":\"97001\",\"shelf_rank\":1,\"platform\":\"web\"},"
      "{\"application_id\":\"97002\",\"shelf_rank\":2,\"platform\":\"web\"}],"
      "\"applications\":["
      "{\"id\":\"97001\",\"name\":\"Poker Night\",\"description\":\"Cards with friends\"},"
      "{\"id\":\"97002\",\"name\":\"Watch Together\",\"description\":\"Watch videos in sync\"},"
      "{\"id\":\"97003\",\"name\":\"Sketch Pad\",\"description\":\"Draw and guess\"}],"
      "\"assets\":[]}");
  }

  if (post && strcmp(p, AV_LAUNCH_PATH) == 0) {
    // Discord refuses a launch that does not name the gateway session it joins from.
    if (strstr(req->body, "\"session_id\":\"" AV_SESSION "\"") == nullptr) {
      return fd_reply(out, cap, 400, "Bad Request", nullptr, "{\"message\":\"Invalid Form Body\",\"code\":50035}");
    }
    av_touch(u->launched_marker);
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }

  if (post && strcmp(p, AV_LEAVE1_PATH) == 0) {
    av_touch(u->left_marker);
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }

  if (post && strstr(p, "/ack") != nullptr) return fd_reply(out, cap, 200, "OK", nullptr, "{\"token\":null}");
  if (get && strstr(p, "/messages?limit=") != nullptr) return fd_reply(out, cap, 200, "OK", nullptr, "[]");
  return fd_reply(out, cap, 204, "No Content", nullptr, "");
}

// ---- fake Main Gateway (runs in the forked child) ----------------------------------------------------------

typedef struct av_gw_ud {
  char launched_marker[AV_MARK_MAX];
  char left_marker[AV_MARK_MAX];
  char second_marker[AV_MARK_MAX];     // parent: another user joins instance 880001
  char ended_marker[AV_MARK_MAX];      // parent: everybody left instance 880001
  char inst2_marker[AV_MARK_MAX];      // parent: instance 880002 starts with self + user 7
} av_gw_ud;

static bool av_send_v2(fg_conn *c, int seq, unsigned long long inst, unsigned long long app, const char *participants)
{
  char ev[900];
  int n = snprintf(ev, sizeof ev,
    "{\"op\":0,\"t\":\"EMBEDDED_ACTIVITY_UPDATE_V2\",\"s\":%d,\"d\":{\"instance_id\":\"%llu\","
    "\"application_id\":\"%llu\",\"location\":{\"id\":\"3121\",\"kind\":\"voice_channel\",\"channel_id\":\"3121\","
    "\"guild_id\":\"2001\"},\"participants\":[%s]}}",
    seq, inst, app, participants);
  if (n <= 0 || (size_t)n >= sizeof ev) return false;
  return fg_send(c, ev);
}

static void av_script_gw(void *ud, fg_conn *c)
{
  const av_gw_ud *u = ud;
  if (!fg_handshake(c)) return;
  if (!fg_send(c, "{\"op\":10,\"d\":{\"heartbeat_interval\":500}}")) return;
  char buf[4096];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) return;
  if (strstr(buf, "\"op\":2") == nullptr) return;

  // The signed-in user (42) owns the guild, so every permission (Use Embedded Activities included) applies.
  char ready[2400];
  (void)snprintf(ready, sizeof ready,
    "{\"op\":0,\"t\":\"READY\",\"s\":1,\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
    "\"session_id\":\"" AV_SESSION "\",\"resume_gateway_url\":\"%s\","
    "\"guilds\":[{\"id\":\"2001\",\"name\":\"Parity HQ\",\"owner_id\":\"42\",\"channels\":["
    "{\"id\":\"3120\",\"type\":0,\"name\":\"general\",\"guild_id\":\"2001\"},"
    "{\"id\":\"3121\",\"type\":2,\"name\":\"Lounge\",\"guild_id\":\"2001\"}],"
    "\"members\":[{\"user\":{\"id\":\"42\",\"username\":\"me\"}},{\"user\":{\"id\":\"7\",\"username\":\"ada\"}}],"
    "\"voice_states\":[]}],"
    "\"private_channels\":[]}}",
    c->self_url);
  if (!fg_send(c, ready)) return;

  int seq = 2;
  bool sent_launch = false, sent_second = false, sent_left = false, sent_ended = false, sent_inst2 = false;
  for (;;) {
    int r = fg_recv(c, buf, sizeof buf, 50);
    if (r == FG_GOT_TEXT) {
      if (strstr(buf, "\"op\":1,") != nullptr || strstr(buf, "\"op\":1}") != nullptr) {
        if (!fg_send(c, "{\"op\":11}")) break;
      } else if (strstr(buf, "\"op\":4") != nullptr) {
        // Voice join/leave: echo the self voice state. No VOICE_SERVER_UPDATE, so no voice connection is made.
        bool chan_null = strstr(buf, "\"channel_id\":null") != nullptr;
        char ev[400];
        (void)snprintf(ev, sizeof ev,
          "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":\"2001\",\"channel_id\":%s,"
          "\"user_id\":\"42\",\"session_id\":\"vs-1\",\"self_stream\":false}}",
          seq++, chan_null ? "null" : "\"3121\"");
        if (!fg_send(c, ev)) break;
      }
    } else if (r != FG_TIMEOUT) {
      break;
    }
    if (!sent_launch && av_exists(u->launched_marker)) {
      sent_launch = true;
      if (!av_send_v2(c, seq++, AV_INST1, AV_APP1, "{\"user_id\":\"42\"}")) break;
    }
    if (!sent_second && av_exists(u->second_marker)) {
      sent_second = true;
      if (!av_send_v2(c, seq++, AV_INST1, AV_APP1, "{\"user_id\":\"42\"},{\"user_id\":\"7\"}")) break;
    }
    if (!sent_left && av_exists(u->left_marker)) {
      sent_left = true;
      if (!av_send_v2(c, seq++, AV_INST1, AV_APP1, "{\"user_id\":\"7\"}")) break;
    }
    if (!sent_ended && av_exists(u->ended_marker)) {
      sent_ended = true;
      if (!av_send_v2(c, seq++, AV_INST1, AV_APP1, "")) break;
    }
    if (!sent_inst2 && av_exists(u->inst2_marker)) {
      sent_inst2 = true;
      if (!av_send_v2(c, seq++, AV_INST2, AV_APP2, "{\"user_id\":\"42\"},{\"user_id\":\"7\"}")) break;
    }
  }
}

// ---- server lifecycle --------------------------------------------------------------------------------------

typedef struct av_servers {
  fd_server http;
  fg_server gw;
  av_http_ud hud;
  av_gw_ud gud;
} av_servers;

static bool av_servers_start(av_servers *s, const char *dir)
{
  memset(s, 0, sizeof *s);
  (void)snprintf(s->hud.launched_marker, sizeof s->hud.launched_marker, "%s/launched", dir);
  (void)snprintf(s->hud.left_marker, sizeof s->hud.left_marker, "%s/left", dir);
  (void)snprintf(s->gud.launched_marker, sizeof s->gud.launched_marker, "%s/launched", dir);
  (void)snprintf(s->gud.left_marker, sizeof s->gud.left_marker, "%s/left", dir);
  (void)snprintf(s->gud.second_marker, sizeof s->gud.second_marker, "%s/second", dir);
  (void)snprintf(s->gud.ended_marker, sizeof s->gud.ended_marker, "%s/ended", dir);
  (void)snprintf(s->gud.inst2_marker, sizeof s->gud.inst2_marker, "%s/inst2", dir);

  // SDL installs a SIGTERM handler (quit event); the forked fake servers would inherit it and survive
  // fg_stop/fd_stop's SIGTERM until their alarm fires. Fork with the default disposition, then restore.
  struct sigaction sa_dfl, sa_old;
  memset(&sa_dfl, 0, sizeof sa_dfl);
  sa_dfl.sa_handler = SIG_DFL;
  sigemptyset(&sa_dfl.sa_mask);
  (void)sigaction(SIGTERM, &sa_dfl, &sa_old);

  if (!fd_start(&s->http, av_respond, &s->hud)) {
    (void)sigaction(SIGTERM, &sa_old, nullptr);
    return false;
  }
  a_http_server = &s->http;
  int flags = fcntl(s->http.pipe_rd, F_GETFL, 0);
  if (flags >= 0) (void)fcntl(s->http.pipe_rd, F_SETFL, flags | O_NONBLOCK);

  if (!fg_start(&s->gw, av_script_gw, &s->gud)) {
    fd_stop(&s->http, nullptr, 0);
    a_http_server = nullptr;
    (void)sigaction(SIGTERM, &sa_old, nullptr);
    return false;
  }
  (void)sigaction(SIGTERM, &sa_old, nullptr);
  return true;
}

static void av_servers_stop(av_servers *s)
{
  fg_stop(&s->gw, nullptr, 0);
  av_drain_hits();
  fd_hit tail[64];
  size_t nt = fd_stop(&s->http, tail, 64);
  for (size_t i = 0; i < nt && a_nhits < AV_MAX_HITS; i++) a_hits[a_nhits++] = tail[i];
  a_http_server = nullptr;
}

// Build the live transport pointed at the fake servers and hand it to the app session.
static ccord_status av_connect_live(const av_servers *s, ccord_error *err)
{
  ccord_live_config lcfg = {
    .token = "REDACTED_VERIFY_TOKEN",
    .api_base = s->http.base,
    .gateway_url = s->gw.url,
    .request_timeout_ms = 3000,
    .now_ms = a_ctx->now_ms,
    .clock_ud = a_ctx->ud,
  };
  ccord_transport *tr = ccord_live_transport_create(&lcfg, err);
  if (!tr) return err->code ? err->code : CCORD_ERR_IO;
  return a_ctx->set_transport(a_ctx->ud, tr, err);
}

static bool av_wait_connected(void)
{
  for (int i = 0; i < AV_WAIT_MAX; i++) {
    av_tick();
    if (ccord_session_state(a_ctx->session) == CCORD_CONN_CONNECTED &&
        ccord_store_channel(a_ctx->store, AV_VOICE) != nullptr &&
        ccord_store_selected_channel(a_ctx->store) == AV_TEXT_CHANNEL) {
      return true;
    }
  }
  return false;
}

static ui_activity_debug av_dbg(void)
{
  ui_activity_debug d;
  ui_activity_get_debug(a_ctx->ui, &d);
  return d;
}

static bool av_has_user(const ccord_activity_instance *inst, ccord_id user)
{
  for (size_t i = 0; inst && i < inst->participant_count; i++) {
    if (inst->participants[i] == user) return true;
  }
  return false;
}

// The channel's first instance row (a copy is not needed: the caller reads it before the next tick).
static const ccord_activity_instance *av_instance(size_t *n)
{
  size_t cnt = 0;
  const ccord_activity_instance *rows = ccord_session_activity_instances(a_ctx->session, AV_VOICE, &cnt);
  if (n) *n = cnt;
  return rows;
}

// ---- mock-transport parity leg -----------------------------------------------------------------------------

// Private clock: advancing the shared app clock here would outrun the live gateway's heartbeat acks.
static uint64_t a_mock_ms;
static uint64_t av_mock_now_ms(void *ud) { (void)ud; return a_mock_ms; }

static void av_mock_pump(ccord_session *s, int n)
{
  for (int i = 0; i < n; i++) {
    (void)ccord_session_pump(s);
    a_mock_ms += 20;
  }
}

// The same flow over the offline mock: launching needs voice; the shelf arrives ordered; a launch yields an
// instance with self; Leave takes it away; a second instance is left by the voice disconnect.
// Returns nullptr on success or a short description of the first failure.
static const char *av_mock_parity(void)
{
  enum { MOCK_GUILD = 2001, MOCK_VOICE = 3121 };
  const char *fail = nullptr;
  ccord_error err = {0};
  a_mock_ms = 1000;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = bus ? ccord_store_create(bus, &err) : nullptr;
  ccord_mock_config mc = {.latency_ms = 0, .now_ms = av_mock_now_ms, .clock_ud = nullptr};
  ccord_transport *mock = (bus && store) ? ccord_mock_transport_create(&mc, &err) : nullptr;
  ccord_session *s = (bus && store && mock) ? ccord_session_create(bus, store, mock, &err) : nullptr;
  if (!s) {
    if (mock) ccord_transport_destroy(mock);
    fail = "mock: could not build a private session";
    goto out;
  }
  if (ccord_session_start(s, &err) != CCORD_OK) { fail = "mock: session start"; goto out; }
  av_mock_pump(s, 50);

  ccord_error lerr;
  if (ccord_session_activity_launch(s, MOCK_VOICE, AV_APP1, &lerr) == CCORD_OK) {
    fail = "mock: launch was accepted outside the voice channel";
    goto out;
  }
  if (ccord_session_join_voice(s, MOCK_GUILD, MOCK_VOICE, &err) != CCORD_OK) { fail = "mock: join voice"; goto out; }
  av_mock_pump(s, 50);

  if (ccord_session_activity_shelf_fetch(s, MOCK_GUILD, &err) != CCORD_OK) { fail = "mock: shelf fetch"; goto out; }
  av_mock_pump(s, 50);
  size_t napps = 0;
  const ccord_activity_app *apps = ccord_session_activity_shelf(s, MOCK_GUILD, &napps);
  if (!apps || napps < 2) { fail = "mock: shelf not cached"; goto out; }
  for (size_t i = 1; i < napps; i++) {
    if (apps[i].shelf_rank != 0 && apps[i - 1].shelf_rank > apps[i].shelf_rank) { fail = "mock: shelf not in rank order"; goto out; }
  }
  ccord_id app = apps[0].application_id;
  ccord_id self = ccord_store_self_id(store);

  if (ccord_session_activity_launch(s, MOCK_VOICE, app, &err) != CCORD_OK) { fail = "mock: launch refused in voice"; goto out; }
  const ccord_admin_request *last = ccord_mock_last_admin(mock);
  if (!last || last->op != CCORD_ADMIN_ACTIVITY_LAUNCH || last->application_id != app ||
      last->session_id[0] == '\0') { fail = "mock: launch did not reach the transport with a session id"; goto out; }
  av_mock_pump(s, 50);
  size_t n = 0;
  const ccord_activity_instance *inst = ccord_session_activity_instances(s, MOCK_VOICE, &n);
  if (!inst || n != 1 || inst[0].application_id != app || !av_has_user(&inst[0], self)) {
    fail = "mock: no instance with self after the launch";
    goto out;
  }

  ccord_activity_instance copy = inst[0];
  if (ccord_session_activity_leave(s, &copy, &err) != CCORD_OK) { fail = "mock: leave refused"; goto out; }
  last = ccord_mock_last_admin(mock);
  if (!last || last->op != CCORD_ADMIN_ACTIVITY_LEAVE || last->instance_id != copy.instance_id) {
    fail = "mock: leave did not reach the transport";
    goto out;
  }
  av_mock_pump(s, 50);
  if (ccord_store_activity_instance_count(store, MOCK_VOICE) != 0) { fail = "mock: instance still listed after leave"; goto out; }

  // Voice disconnect leaves the instance the user is still in.
  if (ccord_session_activity_launch(s, MOCK_VOICE, app, &err) != CCORD_OK) { fail = "mock: second launch"; goto out; }
  av_mock_pump(s, 50);
  if (ccord_store_activity_instance_count(store, MOCK_VOICE) != 1) { fail = "mock: second instance missing"; goto out; }
  if (ccord_session_leave_voice(s, MOCK_GUILD, &err) != CCORD_OK) { fail = "mock: leave voice"; goto out; }
  av_mock_pump(s, 50);
  last = ccord_mock_last_admin(mock);
  if (!last || last->op != CCORD_ADMIN_ACTIVITY_LEAVE) { fail = "mock: voice disconnect did not leave the activity"; goto out; }
  const ccord_activity_instance *left = ccord_store_activity_instance_at(store, MOCK_VOICE, 0);
  if (left && av_has_user(left, self)) { fail = "mock: self still a participant after the disconnect"; goto out; }

out:
  if (s) ccord_session_destroy(s);   // owns the mock transport
  if (store) ccord_store_destroy(store);
  if (bus) ccord_bus_destroy(bus);
  return fail;
}

// ---- the slice ---------------------------------------------------------------------------------------------

static void av_pass(int *checks, const char *name)
{
  (*checks)++;
  printf(AV_SLICE ": check %d %s ok\n", *checks, name);
}

static char a_opened_url[256];
static int a_opened_count;
static bool av_capture_open_url(void *ud, const char *url)
{
  (void)ud;
  (void)snprintf(a_opened_url, sizeof a_opened_url, "%s", url);
  a_opened_count++;
  return true;
}

int ccord_verify_activities_slice(const ccord_verify_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->renderer || !ctx->frames || !ctx->advance_ms ||
      !ctx->now_ms || !ctx->input || !ctx->set_transport) {
    return av_fail("invalid context");
  }
  a_ctx = ctx;
  a_nhits = 0;
  ccord_store *store = ctx->store;
  ccord_ui *ui = ctx->ui;
  ccord_session *session = ctx->session;
  ccord_error err = {0};

  char scratch[AV_MARK_MAX - 32];
  if (!av_make_scratch(scratch, sizeof scratch)) return av_fail("mkdtemp scratch dir");
  char second[AV_MARK_MAX], ended[AV_MARK_MAX], inst2[AV_MARK_MAX], launched[AV_MARK_MAX], left[AV_MARK_MAX];
  (void)snprintf(second, sizeof second, "%s/second", scratch);
  (void)snprintf(ended, sizeof ended, "%s/ended", scratch);
  (void)snprintf(inst2, sizeof inst2, "%s/inst2", scratch);
  (void)snprintf(launched, sizeof launched, "%s/launched", scratch);
  (void)snprintf(left, sizeof left, "%s/left", scratch);

  av_servers srv;
  if (!av_servers_start(&srv, scratch)) {
    (void)rmdir(scratch);
    return av_fail("start fake servers");
  }

  int rc = 1;
  int checks = 0;
  const char *mock_fail = nullptr;
  char what[160];
  bool host_swapped = false;
  bool (*saved_open)(void *, const char *) = nullptr;

  if (av_connect_live(&srv, &err) != CCORD_OK) { (void)av_fail(err.message); goto cleanup; }
  if (!av_wait_connected()) { (void)av_fail("gateway did not connect"); goto cleanup; }

  // 1. launch_requires_voice: outside the voice channel the stage view offers no Activities chip and the session
  // refuses to launch, so nothing is POSTed.
  {
    ui->stage_channel = AV_VOICE;
    ui->dirty = true;
    av_ticks(5);
    ccord_error lerr;
    ccord_status st = ccord_session_activity_launch(session, AV_VOICE, AV_APP1, &lerr);
    av_ticks(AV_SETTLE_TICKS);
    ui_activity_debug d = av_dbg();
    if (st == CCORD_OK || d.chip.w > 0.0f || ui_self_voice(ui) != nullptr ||
        av_count_hits("POST", "/activities/") != 0) {
      (void)av_fail("launch_requires_voice: a launch outside voice was offered or sent");
      goto cleanup;
    }
    av_pass(&checks, "launch_requires_voice");
  }

  // 2. shelf_get: join the voice channel; the chip opens the shelf, the client GETs /activities/shelf?guild_id=
  // and the modal lists the entries in shelf_rank order (the fake serves them as 3, 1, 2).
  {
    if (ccord_session_join_voice(session, AV_GUILD, AV_VOICE, &err) != CCORD_OK) { (void)av_fail(err.message); goto cleanup; }
    AV_WAIT_UNTIL(ui_self_voice(ui) != nullptr && ui_self_voice(ui)->channel_id == AV_VOICE && av_dbg().chip.w > 0.0f);
    ui_activity_debug d = av_dbg();
    if (!ui_self_voice(ui) || d.chip.w <= 0.0f) { (void)av_fail("shelf_get: no Activities chip in the voice stage"); goto cleanup; }
    av_click_rect(d.chip);
    AV_WAIT_UNTIL(av_dbg().row_count >= 3);
    d = av_dbg();
    size_t n = 0;
    const ccord_activity_app *shelf = ccord_session_activity_shelf(session, AV_GUILD, &n);
    bool ok = av_find_hit("GET", "/activities/shelf?guild_id=2001") != nullptr && d.open && d.guild_id == AV_GUILD &&
              d.channel_id == AV_VOICE && d.row_count == 3 && shelf && n == 3 &&
              d.rows[0].application_id == AV_APP1 && strcmp(d.rows[0].name, "Poker Night") == 0 &&
              d.rows[1].application_id == AV_APP2 && strcmp(d.rows[1].name, "Watch Together") == 0 &&
              d.rows[2].application_id == AV_APP3 && strcmp(d.rows[2].name, "Sketch Pad") == 0 &&
              shelf[0].shelf_rank == 1 && shelf[1].shelf_rank == 2 && shelf[2].shelf_rank == 3 &&
              d.rows[0].row.w > 0.0f;
    if (!ok) { (void)av_fail("shelf_get: no GET or the shelf is not listed in shelf_rank order"); goto cleanup; }
    av_pass(&checks, "shelf_get");
  }

  // 3. launch_post: clicking the first entry POSTs /activities/<channel>/<app> carrying the READY session id.
  {
    ui_activity_debug d = av_dbg();
    av_click_rect(d.rows[0].row);
    const fd_hit *post = nullptr;
    AV_WAIT_UNTIL((post = av_find_hit("POST", AV_LAUNCH_PATH)) != nullptr);
    char sid[64] = "";
    bool ok = post && av_json_str(post->body, "session_id", sid, sizeof sid) && strcmp(sid, AV_SESSION) == 0 &&
              !av_dbg().open;
    if (!ok) { (void)av_fail("launch_post: no POST /activities/<channel>/<app> with the READY session_id"); goto cleanup; }
    av_pass(&checks, "launch_post");
  }

  // 4. v2_update_join: the gateway's EMBEDDED_ACTIVITY_UPDATE_V2 (only sent after the valid launch) puts the
  // instance in the store and the stage view, with self as its participant and the Leave / Open buttons.
  {
    AV_WAIT_UNTIL(ccord_store_activity_instance_count(store, AV_VOICE) == 1 && av_dbg().instance_count == 1);
    size_t n = 0;
    const ccord_activity_instance *inst = av_instance(&n);
    ui_activity_debug d = av_dbg();
    bool ok = inst && n == 1 && inst[0].instance_id == AV_INST1 && inst[0].application_id == AV_APP1 &&
              inst[0].participant_count == 1 && av_has_user(&inst[0], AV_SELF) && d.instance_count == 1 &&
              d.instances[0].instance_id == AV_INST1 && d.instances[0].application_id == AV_APP1 &&
              d.instances[0].self_in && d.instances[0].participant_count == 1 && d.banner_visible &&
              d.instances[0].card.w > 0.0f && d.instances[0].leave.w > 0.0f && d.instances[0].open_browser.w > 0.0f;
    if (!ok) { (void)av_fail("v2_update_join: the instance with self is not shown after the gateway update"); goto cleanup; }
    av_pass(&checks, "v2_update_join");
  }

  // 5. open_in_browser_url: the button hands the channel URL to the host opener; nothing is embedded or sent.
  {
    ui_activity_debug d = av_dbg();
    saved_open = ui->d.host.open_url;
    ui->d.host.open_url = av_capture_open_url;   // never launch a real browser from a verification run
    host_swapped = true;
    a_opened_count = 0;
    a_opened_url[0] = '\0';
    av_click_rect(d.instances[0].open_browser);
    av_ticks(3);
    ui->d.host.open_url = saved_open;
    host_swapped = false;
    if (a_opened_count != 1 || strcmp(a_opened_url, "https://discord.com/channels/2001/3121") != 0 ||
        ccord_store_activity_instance_count(store, AV_VOICE) != 1) {
      (void)av_fail("open_in_browser_url: wrong URL opened (expected https://discord.com/channels/2001/3121)");
      goto cleanup;
    }
    av_pass(&checks, "open_in_browser_url");
  }

  // 6. second_user_join: an update adding user 7 shows both participants.
  {
    av_touch(second);
    AV_WAIT_UNTIL(av_dbg().instance_count == 1 && av_dbg().instances[0].participant_count == 2);
    size_t n = 0;
    const ccord_activity_instance *inst = av_instance(&n);
    ui_activity_debug d = av_dbg();
    bool ok = inst && n == 1 && inst[0].instance_id == AV_INST1 && inst[0].participant_count == 2 &&
              av_has_user(&inst[0], AV_SELF) && av_has_user(&inst[0], AV_OTHER) && d.instance_count == 1 &&
              d.instances[0].participant_count == 2 && d.instances[0].self_in && d.instances[0].leave.w > 0.0f;
    if (!ok) { (void)av_fail("second_user_join: the second participant is not shown"); goto cleanup; }
    av_pass(&checks, "second_user_join");
  }

  // 7. leave_post: clicking Leave POSTs .../instances/<id>/leave; once the gateway reports the new participant
  // list the instance stays for user 7 and offers Join again.
  {
    ui_activity_debug d = av_dbg();
    av_click_rect(d.instances[0].leave);
    AV_WAIT_UNTIL(av_find_hit("POST", AV_LEAVE1_PATH) != nullptr);
    AV_WAIT_UNTIL(av_dbg().instance_count == 1 && !av_dbg().instances[0].self_in);
    size_t n = 0;
    const ccord_activity_instance *inst = av_instance(&n);
    d = av_dbg();
    bool ok = av_find_hit("POST", AV_LEAVE1_PATH) != nullptr && inst && n == 1 && inst[0].participant_count == 1 &&
              !av_has_user(&inst[0], AV_SELF) && av_has_user(&inst[0], AV_OTHER) && d.instance_count == 1 &&
              !d.instances[0].self_in && d.instances[0].join.w > 0.0f && d.instances[0].leave.w == 0.0f;
    if (!ok) { (void)av_fail("leave_post: no leave POST or the user is still shown in the instance"); goto cleanup; }
    av_pass(&checks, "leave_post");
  }

  // 8. ended_removed: an update with no participants removes the instance and its stage block.
  {
    av_touch(ended);
    AV_WAIT_UNTIL(ccord_store_activity_instance_count(store, AV_VOICE) == 0 && av_dbg().instance_count == 0);
    ui_activity_debug d = av_dbg();
    if (ccord_store_activity_instance_count(store, AV_VOICE) != 0 || d.instance_count != 0 || d.banner_visible) {
      (void)av_fail("ended_removed: the ended instance is still listed");
      goto cleanup;
    }
    av_pass(&checks, "ended_removed");
  }

  // 9. voice_disconnect_leaves: while self is in another running instance, leaving voice makes the session POST
  // that instance's leave (nothing was sent for it before).
  {
    av_touch(inst2);
    AV_WAIT_UNTIL(ccord_store_activity_instance_count(store, AV_VOICE) == 1);
    size_t n = 0;
    const ccord_activity_instance *inst = av_instance(&n);
    if (!inst || n != 1 || inst[0].instance_id != AV_INST2 || !av_has_user(&inst[0], AV_SELF)) {
      (void)av_fail("voice_disconnect_leaves: the second instance with self did not appear");
      goto cleanup;
    }
    if (av_find_hit("POST", AV_LEAVE2_PATH) != nullptr) {
      (void)av_fail("voice_disconnect_leaves: leave sent before the disconnect");
      goto cleanup;
    }
    if (ccord_session_leave_voice(session, AV_GUILD, &err) != CCORD_OK) { (void)av_fail(err.message); goto cleanup; }
    AV_WAIT_UNTIL(av_find_hit("POST", AV_LEAVE2_PATH) != nullptr);
    if (av_find_hit("POST", AV_LEAVE2_PATH) == nullptr || ui_self_voice(ui) != nullptr) {
      (void)av_fail("voice_disconnect_leaves: no leave POST after leaving the voice channel");
      goto cleanup;
    }
    av_pass(&checks, "voice_disconnect_leaves");
  }
  av_ticks(AV_SETTLE_TICKS);   // let in-flight REST jobs finish before teardown so the queue destroy stays quiet

  // 10. mock_parity: launch / leave / voice disconnect over the offline mock transport.
  mock_fail = av_mock_parity();
  if (mock_fail) {
    (void)snprintf(what, sizeof what, "mock_parity: %s", mock_fail);
    (void)av_fail(what);
    goto cleanup;
  }
  av_pass(&checks, "mock_parity");
  rc = 0;

cleanup:
  if (host_swapped) ui->d.host.open_url = saved_open;
  ui->stage_channel = CCORD_ID_NONE;
  av_servers_stop(&srv);
  (void)unlink(launched);
  (void)unlink(left);
  (void)unlink(second);
  (void)unlink(ended);
  (void)unlink(inst2);
  (void)rmdir(scratch);
  if (rc != 0) return 1;
  printf(AV_SLICE ": PASS checks=%d\n", checks);
  return 0;
}
