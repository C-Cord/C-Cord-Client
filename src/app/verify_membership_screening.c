// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// End-to-end headless verification of membership screening
// (`ccord --headless --verify-slice membership-screening`).
//
// Drives the REAL app pipeline (live_transport + session + store + ui + renderer) against forked loopback Discord
// HTTP + Main Gateway servers (tests/support/fake_discord.h + fake_gateway.h), the same machinery as the other
// parity slices (verify_messaging_events.c). The fakes behave like Discord does for a guild with member
// verification:
//   - POST /invites/<code> answers the invite; the next gateway heartbeat dispatches GUILD_CREATE for the joined
//     guild with the signed-in member `pending: true`;
//   - GET /guilds/<g>/member-verification?with_guild=true serves the rules form;
//   - PUT /guilds/<g>/requests/@me only succeeds for a body that accepts the served form version (`response:true`);
//     it answers a join-request object (not a member), and the next gateway heartbeat dispatches
//     GUILD_MEMBER_UPDATE with `pending: false`;
//   - a message POST in the guild is refused (403) until that PUT succeeded.
// The forked servers share state through marker files in a mkdtemp dir under $TMPDIR (the HTTP child creates a
// marker, the gateway child polls for it on each heartbeat), so every gateway dispatch is caused by a real request
// and paced by the client's heartbeats. A final leg runs the canned 'mv-test' invite flow over the offline mock
// transport (private bus/store/session) to prove live/mock parity.
//
// Output: `membership-screening: PASS checks=7` on success; `ccord: --verify-slice membership-screening: FAILED
// <what>` on stderr and a non-zero exit otherwise.
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

// The modal / composer-lock inspection seams are UI-module internals (ui->mv, ui_composer_lock_reason); the harness
// reads them the same way src/ui/tests/test_ui_interaction.c does.
#include "ui_internal.h"

#define MS_SLICE "membership-screening"
#define MS_HOME_GUILD 2001ULL           // an ordinary guild from READY: its sends must never be gated
#define MS_HOME_CHANNEL 3121ULL
#define MS_SELF 42ULL
#define MS_GUILD 9201ULL                // the guild joined through the invite (requires member verification)
#define MS_GUILD_STR "9201"
#define MS_CHANNEL 9211ULL
#define MS_CHANNEL_STR "9211"
#define MS_INVITE "vrules"
#define MS_VERSION "2026-11-14T09:30:00.000000+00:00"
#define MS_WAIT_MAX 1200                // pump ticks per wait loop (100 ms virtual + ~5 ms real each)
#define MS_SETTLE_TICKS 40              // ticks that prove a request did NOT happen
#define MS_MAX_HITS 256
#define MS_MARK_MAX 512

static const ccord_verify_parity_ctx *m_ctx;
static fd_server *m_http_server;
static fd_hit m_hits[MS_MAX_HITS];
static size_t m_nhits;

// ---- small helpers (same shapes as the ps_* scaffolding in verify_messaging_events.c) -----------------------

static void ms_sleep_ms(unsigned ms)
{
  struct timespec ts = {.tv_sec = (time_t)(ms / 1000u), .tv_nsec = (long)(ms % 1000u) * 1000000L};
  nanosleep(&ts, nullptr);
}

static int ms_fail(const char *what)
{
  fprintf(stderr, "ccord: --verify-slice " MS_SLICE ": FAILED %s\n", what);
  return 1;
}

// The fd child writes one fd_hit per request into a pipe the parent normally only drains at fd_stop. Drain it
// incrementally (records are <= PIPE_BUF, so writes are atomic) so a long slice can never block the child.
static void ms_drain_hits(void)
{
  if (!m_http_server) return;
  for (;;) {
    fd_hit tmp;
    size_t got = 0;
    while (got < sizeof tmp) {
      ssize_t n = read(m_http_server->pipe_rd, (char *)&tmp + got, sizeof tmp - got);
      if (n <= 0) break;
      got += (size_t)n;
    }
    if (got < sizeof tmp) break;
    if (m_nhits < MS_MAX_HITS) m_hits[m_nhits++] = tmp;
  }
}

// One virtual frame + a real-time breather so the forked servers can answer.
static void ms_tick(void)
{
  m_ctx->frames(m_ctx->ud, 1);
  ms_sleep_ms(5);
  ms_drain_hits();
}

static void ms_ticks(int n)
{
  for (int i = 0; i < n; i++) ms_tick();
}

// Poll `cond` (re-evaluated after each tick) until it holds or the wait budget runs out.
#define MS_WAIT_UNTIL(cond)                                                                                         \
  do {                                                                                                              \
    for (int w_ = 0; w_ < MS_WAIT_MAX && !(cond); w_++) ms_tick();                                                  \
  } while (0)

static void ms_click(float x, float y)
{
  ccord_input_event ev = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  m_ctx->input(m_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  m_ctx->input(m_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y};
  m_ctx->input(m_ctx->ud, &ev);
}

static void ms_click_rect(ccord_rect r) { ms_click(r.x + r.w * 0.5f, r.y + r.h * 0.5f); }

static void ms_key(ccord_key k, uint8_t mods)
{
  ccord_input_event ev = {.type = CCORD_IN_KEY_DOWN, .key = k, .mods = mods};
  m_ctx->input(m_ctx->ud, &ev);
}

static void ms_type(const char *text)
{
  size_t left = strlen(text);
  while (left > 0) {
    ccord_input_event ev = {.type = CCORD_IN_TEXT};
    size_t take = left < sizeof ev.text - 1u ? left : sizeof ev.text - 1u;
    while (take < left && take > 1 && ((uint8_t)text[take] & 0xC0u) == 0x80u) take--;
    memcpy(ev.text, text, take);
    m_ctx->input(m_ctx->ud, &ev);
    text += take;
    left -= take;
  }
}

static void ms_clear_composer(void)
{
  if (m_ctx->ui->autocomplete_open) ms_key(CCORD_KEY_ESCAPE, 0);
  ms_key(CCORD_KEY_A, CCORD_MOD_CTRL);
  ms_key(CCORD_KEY_BACKSPACE, 0);
}

// Type into the composer and send with Enter; wait until the server echo confirms it (false on timeout).
static bool ms_send_confirmed(ccord_id ch, const char *text)
{
  ms_clear_composer();
  ms_type(text);
  ms_key(CCORD_KEY_RETURN, 0);
  for (int i = 0; i < MS_WAIT_MAX; i++) {
    ms_tick();
    size_t n = ccord_store_message_count(m_ctx->store, ch);
    const ccord_message *m = n ? ccord_store_message_at(m_ctx->store, ch, n - 1) : nullptr;
    if (m && m->send_state == CCORD_MSG_SEND_CONFIRMED && strcmp(m->content, text) == 0) return true;
  }
  return false;
}

// Extract a plain JSON string value ("key":"...") from a request body. Test payloads are ASCII with no embedded
// quotes; a backslash escape copies the escaped character literally.
static bool ms_json_str(const char *body, const char *key, char *out, size_t cap)
{
  char pat[64];
  (void)snprintf(pat, sizeof pat, "\"%s\":\"", key);
  const char *p = strstr(body, pat);
  if (!p) return false;
  p += strlen(pat);
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < cap) {
    if (*p == '\\' && p[1]) p++;
    out[n++] = *p++;
  }
  out[n] = '\0';
  return true;
}

static const fd_hit *ms_find_hit(const char *method, const char *path)
{
  for (size_t i = 0; i < m_nhits; i++) {
    if (strcmp(m_hits[i].method, method) == 0 && strcmp(m_hits[i].path, path) == 0) return &m_hits[i];
  }
  return nullptr;
}

static size_t ms_count_hits(const char *method, const char *path_substr)
{
  size_t n = 0;
  for (size_t i = 0; i < m_nhits; i++) {
    if (strcmp(m_hits[i].method, method) == 0 && strstr(m_hits[i].path, path_substr) != nullptr) n++;
  }
  return n;
}

static bool ms_make_scratch(char *out, size_t cap)
{
  const char *base = getenv("TMPDIR");
  if (!base || !base[0]) base = ".";
  int n = snprintf(out, cap, "%s/ccord-screening-XXXXXX", base);
  if (n <= 0 || (size_t)n >= cap) return false;
  return mkdtemp(out) != nullptr;
}

static void ms_touch(const char *path)
{
  int fd = open(path, O_CREAT | O_WRONLY, 0600);
  if (fd >= 0) (void)close(fd);
}

static bool ms_exists(const char *path) { return access(path, F_OK) == 0; }

// ---- fake HTTP server (runs in the forked child) -----------------------------------------------------------

#define MS_TS "2026-11-14T12:00:00.000Z"

typedef struct ms_http_ud {
  char joined_marker[MS_MARK_MAX];      // created when POST /invites/<code> succeeds
  char submitted_marker[MS_MARK_MAX];   // created when PUT .../requests/@me accepts the form
  uint64_t next_id;
} ms_http_ud;

static size_t ms_respond(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  ms_http_ud *u = ud;
  (void)idx;
  const char *p = req->path;
  bool get = strcmp(req->method, "GET") == 0;
  bool post = strcmp(req->method, "POST") == 0;
  bool put = strcmp(req->method, "PUT") == 0;

  if (post && strcmp(p, "/invites/" MS_INVITE) == 0) {
    ms_touch(u->joined_marker);
    return fd_reply(out, cap, 200, "OK", nullptr,
      "{\"code\":\"" MS_INVITE "\",\"guild\":{\"id\":\"" MS_GUILD_STR "\",\"name\":\"Rules Lounge\"},"
      "\"channel\":{\"id\":\"9211\",\"name\":\"general\"},\"approximate_member_count\":2}");
  }

  if (get && strcmp(p, "/guilds/" MS_GUILD_STR "/member-verification?with_guild=true") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
      "{\"version\":\"" MS_VERSION "\",\"description\":\"Welcome to Rules Lounge. Please read the rules.\","
      "\"form_fields\":[{\"field_type\":\"TERMS\",\"label\":\"Server rules\",\"required\":true,"
      "\"values\":[\"Be respectful\",\"No spam\",\"Keep it on topic\"]}],"
      "\"guild\":{\"id\":\"" MS_GUILD_STR "\",\"name\":\"Rules Lounge\"}}");
  }

  if (put && strcmp(p, "/guilds/" MS_GUILD_STR "/requests/@me") == 0) {
    // Discord rejects a form that does not accept the current version of the rules.
    bool ok = strstr(req->body, "\"version\":\"" MS_VERSION "\"") != nullptr &&
              strstr(req->body, "\"field_type\":\"TERMS\"") != nullptr &&
              strstr(req->body, "\"response\":true") != nullptr;
    if (!ok) {
      return fd_reply(out, cap, 400, "Bad Request", nullptr, "{\"message\":\"Invalid Form Body\",\"code\":50035}");
    }
    ms_touch(u->submitted_marker);
    // The real answer is a join-request object, not a member: pending clears through the gateway update.
    return fd_reply(out, cap, 200, "OK", nullptr,
      "{\"application_status\":\"APPROVED\",\"guild_id\":\"" MS_GUILD_STR "\",\"user_id\":\"42\"}");
  }

  // Message send. A pending member gets Discord's 403 in the screened guild.
  if (post && strstr(p, "/messages") != nullptr && strstr(p, "/ack") == nullptr) {
    unsigned long long ch = 0;
    if (sscanf(p, "/channels/%llu/messages", &ch) != 1) return fd_reply(out, cap, 404, "Not Found", nullptr, "{}");
    if (ch == MS_CHANNEL && !ms_exists(u->submitted_marker)) {
      return fd_reply(out, cap, 403, "Forbidden", nullptr, "{\"message\":\"Missing Access\",\"code\":50001}");
    }
    char content[256] = "", nonce[64] = "";
    (void)ms_json_str(req->body, "content", content, sizeof content);
    bool has_nonce = ms_json_str(req->body, "nonce", nonce, sizeof nonce);
    char noncefrag[80] = "";
    if (has_nonce) (void)snprintf(noncefrag, sizeof noncefrag, ",\"nonce\":\"%s\"", nonce);
    char body[900];
    int n = snprintf(body, sizeof body,
      "{\"id\":\"%llu\",\"channel_id\":\"%llu\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
      "\"content\":\"%s\",\"timestamp\":\"%s\"%s}",
      (unsigned long long)u->next_id++, ch, content, MS_TS, noncefrag);
    if (n <= 0 || (size_t)n >= sizeof body) return 0;
    return fd_reply(out, cap, 200, "OK", nullptr, body);
  }

  if (post && strstr(p, "/ack") != nullptr) return fd_reply(out, cap, 200, "OK", nullptr, "{\"token\":null}");
  if (get && strstr(p, "/messages?limit=") != nullptr) return fd_reply(out, cap, 200, "OK", nullptr, "[]");
  return fd_reply(out, cap, 204, "No Content", nullptr, "");
}

// ---- fake Main Gateway (runs in the forked child) ----------------------------------------------------------

typedef struct ms_gw_ud {
  char joined_marker[MS_MARK_MAX];
  char submitted_marker[MS_MARK_MAX];
} ms_gw_ud;

static void ms_script_gw(void *ud, fg_conn *c)
{
  const ms_gw_ud *u = ud;
  if (!fg_handshake(c)) return;
  if (!fg_send(c, "{\"op\":10,\"d\":{\"heartbeat_interval\":500}}")) return;
  char buf[4096];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) return;
  if (strstr(buf, "\"op\":2") == nullptr) return;

  char ready[2048];
  (void)snprintf(ready, sizeof ready,
    "{\"op\":0,\"t\":\"READY\",\"s\":1,\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
    "\"session_id\":\"sess-1\",\"resume_gateway_url\":\"%s\","
    "\"guilds\":[{\"id\":\"2001\",\"name\":\"Parity HQ\",\"owner_id\":\"42\",\"channels\":["
    "{\"id\":\"3121\",\"type\":0,\"name\":\"general\",\"guild_id\":\"2001\"}],"
    "\"members\":[{\"user\":{\"id\":\"42\",\"username\":\"me\"}}]}],"
    "\"private_channels\":[]}}",
    c->self_url);
  if (!fg_send(c, ready)) return;

  bool created = false, updated = false;
  for (;;) {
    int r = fg_recv(c, buf, sizeof buf, 100);
    if (r == FG_TIMEOUT) continue;
    if (r != FG_GOT_TEXT) break;
    if (strstr(buf, "\"op\":1,") == nullptr && strstr(buf, "\"op\":1}") == nullptr) continue;
    if (!fg_send(c, "{\"op\":11}")) break;
    // One dispatch per heartbeat, each caused by a request the HTTP child saw. The owner (user 7) is not the
    // signed-in member; @everyone may view and send, so only `pending` can stop the composer.
    if (!created && ms_exists(u->joined_marker)) {
      created = true;
      if (!fg_send(c,
            "{\"op\":0,\"t\":\"GUILD_CREATE\",\"s\":2,\"d\":{\"id\":\"" MS_GUILD_STR "\",\"name\":\"Rules Lounge\","
            "\"owner_id\":\"7\",\"roles\":[{\"id\":\"" MS_GUILD_STR "\",\"name\":\"@everyone\",\"permissions\":\"68608\"}],"
            "\"channels\":[{\"id\":\"9211\",\"type\":0,\"name\":\"general\",\"guild_id\":\"" MS_GUILD_STR "\"}],"
            "\"members\":[{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"roles\":[],\"pending\":true},"
            "{\"user\":{\"id\":\"7\",\"username\":\"owner\"},\"roles\":[]}]}}")) break;
    } else if (created && !updated && ms_exists(u->submitted_marker)) {
      updated = true;
      if (!fg_send(c,
            "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"s\":3,\"d\":{\"guild_id\":\"" MS_GUILD_STR "\","
            "\"user\":{\"id\":\"42\",\"username\":\"me\"},\"roles\":[],\"pending\":false}}")) break;
    }
  }
}

// ---- server lifecycle --------------------------------------------------------------------------------------

typedef struct ms_servers {
  fd_server http;
  fg_server gw;
  ms_http_ud hud;
  ms_gw_ud gud;
} ms_servers;

static bool ms_servers_start(ms_servers *s, const char *joined, const char *submitted)
{
  memset(s, 0, sizeof *s);
  s->hud.next_id = 900001;
  (void)snprintf(s->hud.joined_marker, sizeof s->hud.joined_marker, "%s", joined);
  (void)snprintf(s->hud.submitted_marker, sizeof s->hud.submitted_marker, "%s", submitted);
  (void)snprintf(s->gud.joined_marker, sizeof s->gud.joined_marker, "%s", joined);
  (void)snprintf(s->gud.submitted_marker, sizeof s->gud.submitted_marker, "%s", submitted);

  // SDL installs a SIGTERM handler (quit event); the forked fake servers would inherit it and survive
  // fg_stop/fd_stop's SIGTERM until their alarm fires. Fork with the default disposition, then restore.
  struct sigaction sa_dfl, sa_old;
  memset(&sa_dfl, 0, sizeof sa_dfl);
  sa_dfl.sa_handler = SIG_DFL;
  sigemptyset(&sa_dfl.sa_mask);
  (void)sigaction(SIGTERM, &sa_dfl, &sa_old);

  if (!fd_start(&s->http, ms_respond, &s->hud)) {
    (void)sigaction(SIGTERM, &sa_old, nullptr);
    return false;
  }
  m_http_server = &s->http;
  int flags = fcntl(s->http.pipe_rd, F_GETFL, 0);
  if (flags >= 0) (void)fcntl(s->http.pipe_rd, F_SETFL, flags | O_NONBLOCK);

  if (!fg_start(&s->gw, ms_script_gw, &s->gud)) {
    fd_stop(&s->http, nullptr, 0);
    m_http_server = nullptr;
    (void)sigaction(SIGTERM, &sa_old, nullptr);
    return false;
  }
  (void)sigaction(SIGTERM, &sa_old, nullptr);
  return true;
}

static void ms_servers_stop(ms_servers *s)
{
  fg_stop(&s->gw, nullptr, 0);
  ms_drain_hits();
  fd_hit tail[64];
  size_t nt = fd_stop(&s->http, tail, 64);
  for (size_t i = 0; i < nt && m_nhits < MS_MAX_HITS; i++) m_hits[m_nhits++] = tail[i];
  m_http_server = nullptr;
}

// Build the live transport pointed at the fake servers and hand it to the app session.
static ccord_status ms_connect_live(const ms_servers *s, ccord_error *err)
{
  ccord_live_config lcfg = {
    .token = "REDACTED_VERIFY_TOKEN",
    .api_base = s->http.base,
    .gateway_url = s->gw.url,
    .request_timeout_ms = 3000,
    .now_ms = m_ctx->now_ms,
    .clock_ud = m_ctx->ud,
  };
  ccord_transport *tr = ccord_live_transport_create(&lcfg, err);
  if (!tr) return err->code ? err->code : CCORD_ERR_IO;
  return m_ctx->set_transport(m_ctx->ud, tr, err);
}

// Wait until the gateway is connected and the home guild's first text channel is auto-selected.
static bool ms_wait_connected(void)
{
  for (int i = 0; i < MS_WAIT_MAX; i++) {
    ms_tick();
    if (ccord_session_state(m_ctx->session) == CCORD_CONN_CONNECTED &&
        ccord_store_channel(m_ctx->store, MS_HOME_CHANNEL) != nullptr &&
        ccord_store_selected_channel(m_ctx->store) == MS_HOME_CHANNEL) {
      return true;
    }
  }
  return false;
}

// ---- mock-transport parity leg -----------------------------------------------------------------------------

// Private clock: advancing the shared app clock here would outrun the live gateway's heartbeat acks.
static uint64_t m_mock_ms;
static uint64_t ms_mock_now_ms(void *ud) { (void)ud; return m_mock_ms; }

static void ms_mock_pump(ccord_session *s, int n)
{
  for (int i = 0; i < n; i++) {
    (void)ccord_session_pump(s);
    m_mock_ms += 20;
  }
}

// The same flow over the offline mock: the canned 'mv-test' invite joins "Rules Lounge" pending, sends are refused,
// the rules form is fetched and submitted, the transport's confirmation clears pending and sends work again.
// Returns nullptr on success or a short description of the first failure.
static const char *ms_mock_parity(void)
{
  enum { MV_GUILD = 9101 };
  const char *fail = nullptr;
  ccord_error err = {0};
  m_mock_ms = 1000;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = bus ? ccord_store_create(bus, &err) : nullptr;
  ccord_mock_config mc = {.latency_ms = 0, .now_ms = ms_mock_now_ms, .clock_ud = nullptr};
  ccord_transport *mock = (bus && store) ? ccord_mock_transport_create(&mc, &err) : nullptr;
  ccord_session *s = (bus && store && mock) ? ccord_session_create(bus, store, mock, &err) : nullptr;
  if (!s) {
    if (mock) ccord_transport_destroy(mock);
    fail = "mock: could not build a private session";
    goto out;
  }
  if (ccord_session_start(s, &err) != CCORD_OK) { fail = "mock: session start"; goto out; }
  ms_mock_pump(s, 50);

  ccord_admin_request req;
  if (ccord_admin_accept_invite(&req, "mv-test", &err) != CCORD_OK ||
      ccord_session_admin(s, &req, &err) != CCORD_OK) { fail = "mock: accepting the invite"; goto out; }
  ms_mock_pump(s, 50);
  const ccord_member *self_m = ccord_store_find_member(store, MV_GUILD, ccord_store_self_id(store));
  if (!ccord_store_guild(store, MV_GUILD) || !ccord_session_self_pending(s, MV_GUILD) || !self_m || !self_m->pending) {
    fail = "mock: invite did not join a guild with self pending";
    goto out;
  }
  ccord_id text_ch = CCORD_ID_NONE;
  for (size_t i = 0; ccord_store_channel_at(store, MV_GUILD, i) != nullptr; i++) {
    const ccord_channel *c = ccord_store_channel_at(store, MV_GUILD, i);
    if (c->kind == CCORD_CHANNEL_TEXT) text_ch = c->id;
  }
  if (text_ch == CCORD_ID_NONE) { fail = "mock: no text channel in the joined guild"; goto out; }

  ccord_error serr;
  if (ccord_session_send_message(s, text_ch, "hello", 5, &serr) != CCORD_ERR_PERMISSION ||
      strstr(serr.message, "screening") == nullptr) { fail = "mock: pending send was not refused"; goto out; }

  if (ccord_session_member_verification_fetch(s, MV_GUILD, &err) != CCORD_OK) { fail = "mock: form fetch"; goto out; }
  ms_mock_pump(s, 50);
  const ccord_member_verification *form = ccord_session_member_verification(s, MV_GUILD);
  if (!form || form->field_count != 1 || form->fields[0].field_type != CCORD_MEMBER_VERIFICATION_TERMS ||
      form->fields[0].value_count != 3) { fail = "mock: rules form not cached"; goto out; }

  if (ccord_session_member_verification_submit(s, MV_GUILD, &err) != CCORD_OK) { fail = "mock: form submit"; goto out; }
  const ccord_admin_request *last = ccord_mock_last_admin(mock);
  if (!last || last->op != CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION || last->guild_id != MV_GUILD) {
    fail = "mock: submit did not reach the transport";
    goto out;
  }
  ms_mock_pump(s, 50);
  if (ccord_session_self_pending(s, MV_GUILD)) { fail = "mock: pending not cleared"; goto out; }
  if (ccord_session_send_message(s, text_ch, "hello", 5, &serr) != CCORD_OK) { fail = "mock: send after screening"; goto out; }

out:
  if (s) ccord_session_destroy(s);   // owns the mock transport
  if (store) ccord_store_destroy(store);
  if (bus) ccord_bus_destroy(bus);
  return fail;
}

// ---- the slice ---------------------------------------------------------------------------------------------

static void ms_pass(int *checks, const char *name)
{
  (*checks)++;
  printf(MS_SLICE ": check %d %s ok\n", *checks, name);
}

int ccord_verify_membership_screening_slice(const ccord_verify_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->renderer || !ctx->frames || !ctx->advance_ms ||
      !ctx->now_ms || !ctx->input || !ctx->set_transport) {
    return ms_fail("invalid context");
  }
  m_ctx = ctx;
  m_nhits = 0;
  ccord_store *store = ctx->store;
  ccord_ui *ui = ctx->ui;
  ccord_session *session = ctx->session;
  ccord_error err = {0};

  char scratch[MS_MARK_MAX - 32];
  if (!ms_make_scratch(scratch, sizeof scratch)) return ms_fail("mkdtemp scratch dir");
  char joined[MS_MARK_MAX], submitted[MS_MARK_MAX];
  (void)snprintf(joined, sizeof joined, "%s/joined", scratch);
  (void)snprintf(submitted, sizeof submitted, "%s/submitted", scratch);

  ms_servers srv;
  if (!ms_servers_start(&srv, joined, submitted)) {
    (void)rmdir(scratch);
    return ms_fail("start fake servers");
  }

  int rc = 1;
  int checks = 0;
  const char *mock_fail = nullptr;
  char what[160];
  char serr_msg[sizeof err.message];

  if (ms_connect_live(&srv, &err) != CCORD_OK) { (void)ms_fail(err.message); goto cleanup; }
  if (!ms_wait_connected()) { (void)ms_fail("gateway did not connect"); goto cleanup; }
  if (ccord_store_guild(store, MS_GUILD) != nullptr) { (void)ms_fail("screened guild present before the invite"); goto cleanup; }

  // 1. invite-join: accepting the invite (POST /invites/<code>) yields the guild, and self is pending in it. The
  // session selects the joined guild/channel on its own once the gateway's GUILD_CREATE lands.
  {
    ccord_admin_request req;
    if (ccord_admin_accept_invite(&req, MS_INVITE, &err) != CCORD_OK ||
        ccord_session_admin(session, &req, &err) != CCORD_OK) {
      (void)ms_fail("accepting the invite");
      goto cleanup;
    }
    const ccord_member *me = nullptr;
    MS_WAIT_UNTIL((me = ccord_store_find_member(store, MS_GUILD, MS_SELF)) != nullptr &&
                  ccord_store_selected_channel(store) == MS_CHANNEL);
    const ccord_guild *g = ccord_store_guild(store, MS_GUILD);
    bool ok = ms_find_hit("POST", "/invites/" MS_INVITE) != nullptr && g && strcmp(g->name, "Rules Lounge") == 0 &&
              me && me->pending && ccord_session_self_pending(session, MS_GUILD) &&
              ccord_store_selected_guild(store) == MS_GUILD && ccord_store_selected_channel(store) == MS_CHANNEL;
    if (!ok) { (void)ms_fail("invite-join: guild with self pending not joined/selected"); goto cleanup; }
    ms_pass(&checks, "invite-join");
  }

  // 2. pending-blocked: the composer is a locked banner and the session refuses to send; nothing reaches the
  // server for the screened channel. A send in the ordinary guild still goes out (the gate keys on `pending`).
  {
    ms_ticks(10);   // let a frame draw the locked composer
    const char *why = ui_composer_lock_reason(ui);
    if (!why || strstr(why, "rules") == nullptr) { (void)ms_fail("pending-blocked: composer not locked for the rules"); goto cleanup; }
    if (ui->mv.composer_btn.w <= 0.0f) { (void)ms_fail("pending-blocked: no 'Complete the rules' button drawn"); goto cleanup; }
    ccord_error serr;
    ccord_status st = ccord_session_send_message(session, MS_CHANNEL, "should be blocked", 17, &serr);
    (void)snprintf(serr_msg, sizeof serr_msg, "%s", st == CCORD_OK ? "" : serr.message);
    if (st != CCORD_ERR_PERMISSION || strstr(serr_msg, "screening") == nullptr) {
      (void)ms_fail("pending-blocked: session send was not rejected by the screening gate");
      goto cleanup;
    }
    ccord_error cerr;
    if (ccord_session_send_message(session, MS_HOME_CHANNEL, "control", 7, &cerr) != CCORD_OK) {
      (void)ms_fail("pending-blocked: send in an unscreened guild was gated too");
      goto cleanup;
    }
    ms_ticks(MS_SETTLE_TICKS);
    if (ms_count_hits("POST", "/channels/" MS_CHANNEL_STR "/messages") != 0) {
      (void)ms_fail("pending-blocked: a message POST for the screened channel reached the server");
      goto cleanup;
    }
    if (ms_count_hits("POST", "/channels/3121/messages") < 1) {
      (void)ms_fail("pending-blocked: the control send never reached the server");
      goto cleanup;
    }
    ms_pass(&checks, "pending-blocked");
  }

  // 3. form-GET: the session fetched GET /guilds/<g>/member-verification and the modal shows the rules.
  {
    const ccord_member_verification *form = nullptr;
    MS_WAIT_UNTIL((form = ccord_session_member_verification(session, MS_GUILD)) != nullptr &&
                  ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION && ui->mv.checks[0].w > 0.0f);
    bool ok = ms_find_hit("GET", "/guilds/" MS_GUILD_STR "/member-verification?with_guild=true") != nullptr &&
              form && form->field_count == 1 && form->fields[0].field_type == CCORD_MEMBER_VERIFICATION_TERMS &&
              form->fields[0].value_count == 3 && strcmp(form->fields[0].values[0], "Be respectful") == 0 &&
              strcmp(form->fields[0].values[2], "Keep it on topic") == 0 && form->version &&
              strcmp(form->version, MS_VERSION) == 0 && ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION &&
              ui->mv.guild_id == MS_GUILD && ui->mv.box.w > 0.0f && ui->mv.checks[0].w > 0.0f;
    if (!ok) { (void)ms_fail("form-GET: no GET or the modal does not show the rules"); goto cleanup; }
    ms_pass(&checks, "form-GET");
  }

  // 4. submit-gated: Submit is disabled and ignores clicks until the checkbox is ticked.
  {
    ms_tick();
    if (ui->mv.submit_enabled || ui->mv.agreed[0]) { (void)ms_fail("submit-gated: Submit enabled before the checkbox"); goto cleanup; }
    ms_click_rect(ui->mv.submit);
    ms_ticks(MS_SETTLE_TICKS);
    if (ms_find_hit("PUT", "/guilds/" MS_GUILD_STR "/requests/@me") != nullptr ||
        ui->modal_kind != UI_MODAL_MEMBER_VERIFICATION) {
      (void)ms_fail("submit-gated: a click on the disabled Submit sent the form or closed the modal");
      goto cleanup;
    }
    ms_click_rect(ui->mv.checks[0]);
    ms_ticks(2);
    if (!ui->mv.agreed[0] || !ui->mv.submit_enabled) { (void)ms_fail("submit-gated: ticking the box did not enable Submit"); goto cleanup; }
    ms_pass(&checks, "submit-gated");
  }

  // 5. PUT-body: Submit sends PUT /guilds/<g>/requests/@me accepting the served form version.
  {
    ms_click_rect(ui->mv.submit);
    const fd_hit *put = nullptr;
    MS_WAIT_UNTIL((put = ms_find_hit("PUT", "/guilds/" MS_GUILD_STR "/requests/@me")) != nullptr);
    char ver[64] = "";
    const ccord_member_verification *form = ccord_session_member_verification(session, MS_GUILD);
    bool ok = put && strstr(put->body, "\"response\":true") != nullptr &&
              strstr(put->body, "\"field_type\":\"TERMS\"") != nullptr && ms_json_str(put->body, "version", ver, sizeof ver) &&
              strcmp(ver, MS_VERSION) == 0 && form && form->version && strcmp(ver, form->version) == 0;
    if (!ok) { (void)ms_fail("PUT-body: no PUT carrying response:true and the served version"); goto cleanup; }
    ms_pass(&checks, "PUT-body");
  }

  // 6. cleared+send: the gateway's GUILD_MEMBER_UPDATE clears pending, the composer unlocks and a message POST
  // succeeds and is confirmed.
  {
    const ccord_member *me = nullptr;
    MS_WAIT_UNTIL(!ccord_session_self_pending(session, MS_GUILD) && ui->modal_kind != UI_MODAL_MEMBER_VERIFICATION &&
                  ui->mv.composer_btn.w <= 0.0f);
    me = ccord_store_find_member(store, MS_GUILD, MS_SELF);
    if (!me || me->pending || ccord_session_self_pending(session, MS_GUILD)) {
      (void)ms_fail("cleared+send: pending was not cleared");
      goto cleanup;
    }
    if (ui_composer_lock_reason(ui) != nullptr || ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION) {
      (void)ms_fail("cleared+send: composer still locked after the screening cleared");
      goto cleanup;
    }
    if (!ms_send_confirmed(MS_CHANNEL, "hello after the rules")) {
      (void)ms_fail("cleared+send: message was not confirmed");
      goto cleanup;
    }
    bool posted = false;
    for (size_t i = 0; i < m_nhits; i++) {
      if (strcmp(m_hits[i].method, "POST") == 0 && strcmp(m_hits[i].path, "/channels/" MS_CHANNEL_STR "/messages") == 0 &&
          strstr(m_hits[i].body, "hello after the rules") != nullptr) posted = true;
    }
    if (!posted) { (void)ms_fail("cleared+send: no message POST for the screened channel"); goto cleanup; }
    ms_pass(&checks, "cleared+send");
  }
  ms_ticks(MS_SETTLE_TICKS);   // let in-flight REST jobs finish before teardown so the queue destroy stays quiet

  // 7. mock-parity: the 'mv-test' invite flow over the offline mock transport.
  mock_fail = ms_mock_parity();
  if (mock_fail) {
    (void)snprintf(what, sizeof what, "mock-parity: %s", mock_fail);
    (void)ms_fail(what);
    goto cleanup;
  }
  ms_pass(&checks, "mock-parity");
  rc = 0;

cleanup:
  ms_servers_stop(&srv);
  (void)unlink(joined);
  (void)unlink(submitted);
  (void)rmdir(scratch);
  if (rc != 0) return 1;
  printf(MS_SLICE ": PASS checks=%d\n", checks);
  return 0;
}
