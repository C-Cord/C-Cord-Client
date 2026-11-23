// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// End-to-end headless parity verification for Results 1-3
// (`ccord --headless --verify-slice messaging|settings-proto|events-stage`).
//
// Each slice drives the REAL app pipeline (live_transport + session + store + ui + renderer) against forked
// loopback Discord HTTP + Main Gateway servers (tests/support/fake_discord.h + fake_gateway.h), exactly like
// verify_expressions.c, and asserts what actually reached the servers plus the resulting UI/store state before
// printing its `...=1` result lines. The settings-proto slice additionally runs the same favorite-GIF writeback
// flow over the offline mock transport (private bus/store/session) to prove live/mock parity. All scratch files
// live in a mkdtemp dir under $TMPDIR.
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
#include <ccord/core/paths.h>
#include <ccord/net/admin.h>
#include <ccord/net/discord_proto.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>
#include <ccord/store/expressions.h>

// The menu / saved-messages / forward-picker / events-modal inspection seams are UI-module internals; the
// harness reads them the same way src/ui/tests/test_ui_interaction.c does.
#include "ui_internal.h"

#define PS_GUILD 2001ULL
#define PS_SELF 42ULL
#define PS_MEMBER 1002ULL
#define PS_CH_GENERAL 3121ULL
#define PS_CH_OFFTOPIC 3122ULL
#define PS_CH_ANN 3123ULL
#define PS_CH_STAGE 3124ULL
#define PS_CH_VOICE 3125ULL
#define PS_WAIT_MAX 1200                // pump ticks per wait loop (100 ms virtual + ~5 ms real each)
#define PS_MAX_HITS 256

static const ccord_verify_parity_ctx *p_ctx;
static fd_server *p_http_server;
static fd_hit *p_hits;
static size_t p_nhits;

// ---- small helpers -----------------------------------------------------------------------------------------

static void ps_sleep_ms(unsigned ms)
{
  struct timespec ts = {.tv_sec = (time_t)(ms / 1000u), .tv_nsec = (long)(ms % 1000u) * 1000000L};
  nanosleep(&ts, nullptr);
}

static int ps_fail(const char *slice, const char *what)
{
  fprintf(stderr, "ccord: --verify-slice %s: FAILED %s\n", slice, what);
  return 1;
}

// The fd child writes one fd_hit per request into a pipe the parent normally only drains at fd_stop. Drain it
// incrementally (records are <= PIPE_BUF, so writes are atomic) so a long slice can never block the child.
static void ps_drain_hits(void)
{
  if (!p_http_server) return;
  for (;;) {
    fd_hit tmp;
    size_t got = 0;
    while (got < sizeof tmp) {
      ssize_t n = read(p_http_server->pipe_rd, (char *)&tmp + got, sizeof tmp - got);
      if (n <= 0) break;
      got += (size_t)n;
    }
    if (got < sizeof tmp) break;
    if (p_nhits < PS_MAX_HITS) p_hits[p_nhits++] = tmp;
  }
}

// One virtual frame + a real-time breather so the forked servers can answer.
static void ps_tick(void)
{
  p_ctx->frames(p_ctx->ud, 1);
  ps_sleep_ms(5);
  ps_drain_hits();
}

// Let in-flight REST jobs finish before teardown so the queue destroy does not log spurious IO failures.
static void ps_settle(void)
{
  for (int i = 0; i < 40; i++) ps_tick();
}

static void ps_click(float x, float y)
{
  ccord_input_event ev = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  p_ctx->input(p_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  p_ctx->input(p_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y};
  p_ctx->input(p_ctx->ud, &ev);
}

static void ps_key(ccord_key k, uint8_t mods)
{
  ccord_input_event ev = {.type = CCORD_IN_KEY_DOWN, .key = k, .mods = mods};
  p_ctx->input(p_ctx->ud, &ev);
}

static void ps_type(const char *text)
{
  size_t left = strlen(text);
  while (left > 0) {
    ccord_input_event ev = {.type = CCORD_IN_TEXT};
    size_t take = left < sizeof ev.text - 1u ? left : sizeof ev.text - 1u;
    while (take < left && take > 1 && ((uint8_t)text[take] & 0xC0u) == 0x80u) take--;
    memcpy(ev.text, text, take);
    p_ctx->input(p_ctx->ud, &ev);
    text += take;
    left -= take;
  }
}

static void ps_clear_composer(void)
{
  if (p_ctx->ui->autocomplete_open) ps_key(CCORD_KEY_ESCAPE, 0);
  ps_key(CCORD_KEY_A, CCORD_MOD_CTRL);
  ps_key(CCORD_KEY_BACKSPACE, 0);
}

// Click one labelled row of the currently open context menu (same walk as test_ui_interaction.c).
static bool ps_menu_click_label(const char *label)
{
  ccord_ui *ui = p_ctx->ui;
  for (size_t i = 0; i < ui->menu.count; i++) {
    const menu_item *it = &ui->menu.items[i];
    if (strcmp(it->label, label) != 0) continue;
    if (it->parent >= 0 && it->parent != ui->menu.open_sub) continue;
    // A tall menu clamps to the window and scrolls; wheel the row into the visible interior before clicking.
    if (it->parent < 0) {
      for (int g = 0; g < 80; g++) {
        ccord_rect r = ui_menu_item_rect(ui, i);
        float cy = r.y + r.h * 0.5f;
        if (cy >= ui->menu.y + 2.0f && cy <= ui->menu.y + ui->menu.h - 2.0f) break;
        ccord_input_event ev = {.type = CCORD_IN_WHEEL, .x = ui->menu.x + ui->menu.w * 0.5f,
                                .y = ui->menu.y + ui->menu.h * 0.5f, .wheel = cy > ui->menu.y ? -3.0f : 3.0f};
        p_ctx->input(p_ctx->ud, &ev);
        ps_tick();
      }
    }
    ccord_rect r = ui_menu_item_rect(ui, i);
    ps_click(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    return true;
  }
  return false;
}

static bool ps_open_menu_and_click(const ccord_menu_context *ctx, const char *label)
{
  ui_menu_open(p_ctx->ui, 420.0f, 300.0f, ctx);
  ps_tick();
  if (!p_ctx->ui->menu.open) return false;
  return ps_menu_click_label(label);
}

// Send a composer message and wait until the server echo confirms it; returns the confirmed id (0 on failure).
static ccord_id ps_send_confirmed(ccord_id ch, const char *text)
{
  ps_clear_composer();
  ps_type(text);
  ps_key(CCORD_KEY_RETURN, 0);
  for (int i = 0; i < PS_WAIT_MAX; i++) {
    ps_tick();
    size_t n = ccord_store_message_count(p_ctx->store, ch);
    const ccord_message *m = n ? ccord_store_message_at(p_ctx->store, ch, n - 1) : nullptr;
    if (m && m->send_state == CCORD_MSG_SEND_CONFIRMED && strcmp(m->content, text) == 0) return m->id;
  }
  return CCORD_ID_NONE;
}

// ---- base64 / JSON helpers ---------------------------------------------------------------------------------

static size_t ps_b64(const uint8_t *in, size_t len, char *out)
{
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0, i = 0;
  for (; i + 2 < len; i += 3) {
    uint32_t v = ((uint32_t)in[i] << 16u) | ((uint32_t)in[i + 1] << 8u) | in[i + 2];
    out[o++] = tbl[(v >> 18u) & 63u];
    out[o++] = tbl[(v >> 12u) & 63u];
    out[o++] = tbl[(v >> 6u) & 63u];
    out[o++] = tbl[v & 63u];
  }
  if (i < len) {
    uint32_t v = (uint32_t)in[i] << 16u;
    if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8u;
    out[o++] = tbl[(v >> 18u) & 63u];
    out[o++] = tbl[(v >> 12u) & 63u];
    out[o++] = i + 1 < len ? tbl[(v >> 6u) & 63u] : '=';
    out[o++] = '=';
  }
  out[o] = '\0';
  return o;
}

static size_t ps_b64_decode(const char *in, size_t len, uint8_t *out, size_t cap)
{
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int rev[256];
  for (int i = 0; i < 256; i++) rev[i] = -1;
  for (int i = 0; i < 64; i++) rev[(unsigned char)tbl[i]] = i;
  size_t o = 0;
  uint32_t buf = 0;
  int bits = 0;
  for (size_t i = 0; i < len; i++) {
    char c = in[i];
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    int v = rev[(unsigned char)c];
    if (v < 0) continue;
    buf = (buf << 6u) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o < cap) out[o++] = (uint8_t)(buf >> bits);
    }
  }
  return o;
}

static bool ps_contains_bytes(const uint8_t *hay, size_t hlen, const uint8_t *needle, size_t nlen)
{
  if (nlen == 0 || hlen < nlen) return false;
  for (size_t i = 0; i + nlen <= hlen; i++) {
    if (memcmp(hay + i, needle, nlen) == 0) return true;
  }
  return false;
}

// Extract a plain JSON string value ("key":"...") from a request/response body. Test payloads are ASCII with
// no embedded quotes; a backslash escape copies the escaped character literally.
static bool ps_json_str(const char *body, const char *key, char *out, size_t cap)
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

// Extract a flat JSON object ("key":{...}) verbatim (used to echo message_reference on a forward).
static bool ps_json_obj(const char *body, const char *key, char *out, size_t cap)
{
  char pat[64];
  (void)snprintf(pat, sizeof pat, "\"%s\":{", key);
  const char *p = strstr(body, pat);
  if (!p) return false;
  p += strlen(pat) - 1u;   // point at '{'
  size_t n = 0;
  int depth = 0;
  while (*p && n + 1 < cap) {
    char c = *p;
    out[n++] = c;
    if (c == '{') depth++;
    else if (c == '}') {
      depth--;
      if (depth == 0) { p++; break; }
    }
    p++;
  }
  out[n] = '\0';
  return depth == 0;
}

// ---- scratch directory -------------------------------------------------------------------------------------

static bool ps_make_scratch(char *out, size_t cap)
{
  const char *base = getenv("TMPDIR");
  if (!base || !base[0]) base = ".";
  int n = snprintf(out, cap, "%s/ccord-parity-XXXXXX", base);
  if (n <= 0 || (size_t)n >= cap) return false;
  return mkdtemp(out) != nullptr;
}

// ---- fake HTTP server (runs in the forked child) -----------------------------------------------------------

typedef struct ps_http_ud {
  fd_server *srv;             // fd_start fills srv->port BEFORE forking: the child copy knows its own port
  int slice;                  // 1 messaging, 2 settings-proto, 3 events-stage
  uint8_t seed_raw[1024];     // settings-proto/2 GET seed (slice 2)
  size_t seed_len;
  // Child-local echo table: every message POSTed through this server, so forwards/crossposts can quote it.
  struct { uint64_t id; char content[256]; } msgs[64];
  size_t nmsgs;
  uint64_t next_id;
} ps_http_ud;

static const char *ps_msg_content(const ps_http_ud *u, uint64_t id)
{
  for (size_t i = 0; i < u->nmsgs; i++) {
    if (u->msgs[i].id == id) return u->msgs[i].content;
  }
  return "";
}

static void ps_msg_remember(ps_http_ud *u, uint64_t id, const char *content)
{
  if (u->nmsgs >= sizeof u->msgs / sizeof u->msgs[0]) return;
  u->msgs[u->nmsgs].id = id;
  (void)snprintf(u->msgs[u->nmsgs].content, sizeof u->msgs[u->nmsgs].content, "%s", content);
  u->nmsgs++;
}

#define PS_TS "2026-11-15T12:00:00.000Z"

static size_t ps_respond(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  ps_http_ud *u = ud;
  (void)idx;
  const char *m = req->method;
  const char *p = req->path;
  bool get = strcmp(m, "GET") == 0;
  bool post = strcmp(m, "POST") == 0;
  bool patch = strcmp(m, "PATCH") == 0;
  bool put = strcmp(m, "PUT") == 0;
  bool del = strcmp(m, "DELETE") == 0;

  // settings-proto/2: GET answers the seed; PATCH echoes the request body (as Discord returns the new blob).
  if (strcmp(p, "/users/@me/settings-proto/2") == 0) {
    if (get) {
      char b64[1600];
      (void)ps_b64(u->seed_raw, u->seed_len, b64);
      char body[1800];
      int n = snprintf(body, sizeof body, "{\"settings\":\"%s\"}", b64);
      if (n <= 0 || (size_t)n >= sizeof body) return 0;
      return fd_reply(out, cap, 200, "OK", nullptr, body);
    }
    if (patch) return fd_reply(out, cap, 200, "OK", nullptr, req->body);
  }

  // Slash / context-menu application command index.
  if (get && strstr(p, "/application-command-index") != nullptr) {
    return fd_reply(out, cap, 200, "OK", nullptr,
      "[{\"id\":\"9001\",\"application_id\":\"8001\",\"version\":\"9002\",\"type\":1,\"name\":\"config\","
      "\"description\":\"Config\",\"options\":["
      "{\"type\":1,\"name\":\"show\",\"description\":\"Show\",\"options\":[]},"
      "{\"type\":2,\"name\":\"theme\",\"description\":\"Theme\",\"options\":["
      "{\"type\":1,\"name\":\"set\",\"description\":\"Set\",\"options\":["
      "{\"type\":3,\"name\":\"preset\",\"description\":\"Preset\",\"required\":true}]},"
      "{\"type\":1,\"name\":\"reset\",\"description\":\"Reset\",\"options\":[]}]}]},"
      "{\"id\":\"9101\",\"application_id\":\"8001\",\"version\":\"9102\",\"type\":2,\"name\":\"UserInfo\",\"description\":\"\"},"
      "{\"id\":\"9102\",\"application_id\":\"8001\",\"version\":\"9103\",\"type\":3,\"name\":\"QuoteMessage\",\"description\":\"\"}]");
  }

  if (post && strcmp(p, "/interactions") == 0) return fd_reply(out, cap, 204, "No Content", nullptr, "");

  // Message send / tts / forward.
  if (post && strstr(p, "/messages") != nullptr && strstr(p, "/ack") == nullptr && strstr(p, "/crosspost") == nullptr) {
    unsigned long long ch = 0;
    if (sscanf(p, "/channels/%llu/messages", &ch) != 1) return fd_reply(out, cap, 404, "Not Found", nullptr, "{}");
    char content[256] = "", nonce[64] = "";
    (void)ps_json_str(req->body, "content", content, sizeof content);
    bool has_nonce = ps_json_str(req->body, "nonce", nonce, sizeof nonce);
    bool tts = strstr(req->body, "\"tts\":true") != nullptr;
    uint64_t id = u->next_id++;
    char noncefrag[80] = "";
    if (has_nonce) (void)snprintf(noncefrag, sizeof noncefrag, ",\"nonce\":\"%s\"", nonce);
    char body[1600];
    if (strstr(req->body, "\"message_reference\"") != nullptr) {
      // Forward: echo the type-1 reference and quote the original message in a snapshot.
      char ref[512], midstr[32] = "";
      if (!ps_json_obj(req->body, "message_reference", ref, sizeof ref)) return fd_reply(out, cap, 400, "Bad Request", nullptr, "{}");
      (void)ps_json_str(ref, "message_id", midstr, sizeof midstr);
      uint64_t src_id = strtoull(midstr, nullptr, 10);
      const char *src_content = ps_msg_content(u, src_id);
      int n = snprintf(body, sizeof body,
        "{\"id\":\"%llu\",\"channel_id\":\"%llu\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
        "\"content\":\"\",\"timestamp\":\"%s\"%s,\"message_reference\":%s,"
        "\"message_snapshots\":[{\"message\":{\"content\":\"%s\",\"timestamp\":\"%s\",\"attachments\":[],\"embeds\":[]}}]}",
        (unsigned long long)id, ch, PS_TS, noncefrag, ref, src_content, PS_TS);
      if (n <= 0 || (size_t)n >= sizeof body) return 0;
    } else {
      ps_msg_remember(u, id, content);
      int n = snprintf(body, sizeof body,
        "{\"id\":\"%llu\",\"channel_id\":\"%llu\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
        "\"content\":\"%s\",\"timestamp\":\"%s\"%s%s}",
        (unsigned long long)id, ch, content, PS_TS, noncefrag, tts ? ",\"tts\":true" : "");
      if (n <= 0 || (size_t)n >= sizeof body) return 0;
    }
    return fd_reply(out, cap, 200, "OK", nullptr, body);
  }

  // Announcement crosspost: the message comes back with the CROSSPOSTED flag.
  if (post && strstr(p, "/crosspost") != nullptr) {
    unsigned long long ch = 0, mid = 0;
    if (sscanf(p, "/channels/%llu/messages/%llu/crosspost", &ch, &mid) != 2) return fd_reply(out, cap, 404, "Not Found", nullptr, "{}");
    char body[600];
    int n = snprintf(body, sizeof body,
      "{\"id\":\"%llu\",\"channel_id\":\"%llu\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
      "\"content\":\"%s\",\"flags\":1,\"timestamp\":\"%s\"}",
      mid, ch, ps_msg_content(u, mid), PS_TS);
    if (n <= 0 || (size_t)n >= sizeof body) return 0;
    return fd_reply(out, cap, 200, "OK", nullptr, body);
  }

  // Mark Unread / read ack.
  if (post && strstr(p, "/ack") != nullptr) return fd_reply(out, cap, 200, "OK", nullptr, "{\"token\":null}");

  // Channel history.
  if (get && strstr(p, "/messages?limit=") != nullptr) return fd_reply(out, cap, 200, "OK", nullptr, "[]");

  // ---- Scheduled Events REST (slice 3) ----
  if (get && strstr(p, "/scheduled-events?with_user_count=true") != nullptr) {
    return fd_reply(out, cap, 200, "OK", nullptr,
      "[{\"id\":\"7701\",\"guild_id\":\"2001\",\"channel_id\":\"3124\",\"creator_id\":\"42\","
      "\"name\":\"Town Hall Event\",\"description\":\"Q&A\","
      "\"scheduled_start_time\":\"2026-11-15T18:00:00.000Z\","
      "\"privacy_level\":2,\"status\":1,\"entity_type\":1,\"user_count\":5}]");
  }
  if (post && strcmp(p, "/guilds/2001/scheduled-events") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
      "{\"id\":\"7702\",\"guild_id\":\"2001\",\"channel_id\":\"3124\",\"creator_id\":\"42\","
      "\"name\":\"Launch Party\",\"description\":\"Bring snacks\","
      "\"scheduled_start_time\":\"2026-11-16T18:00:00.000Z\","
      "\"privacy_level\":2,\"status\":1,\"entity_type\":1,\"user_count\":1}");
  }
  if (patch && strcmp(p, "/guilds/2001/scheduled-events/7702") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
      "{\"id\":\"7702\",\"guild_id\":\"2001\",\"channel_id\":\"3124\",\"creator_id\":\"42\","
      "\"name\":\"Launch Party Live\",\"description\":\"Bring snacks\","
      "\"scheduled_start_time\":\"2026-11-16T18:00:00.000Z\","
      "\"privacy_level\":2,\"status\":2,\"entity_type\":1,\"user_count\":4}");
  }
  if ((put || del) && strcmp(p, "/guilds/2001/scheduled-events/7702/users/@me") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{}");
  }
  if (del && strcmp(p, "/guilds/2001/scheduled-events/7702") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }

  // ---- Stage instances REST (slice 3) ----
  if (post && strcmp(p, "/stage-instances") == 0) {
    char topic[160] = "Parity Stage";
    (void)ps_json_str(req->body, "topic", topic, sizeof topic);
    char body[400];
    int n = snprintf(body, sizeof body,
      "{\"id\":\"6601\",\"guild_id\":\"2001\",\"channel_id\":\"3124\",\"topic\":\"%s\",\"privacy_level\":2}", topic);
    if (n <= 0 || (size_t)n >= sizeof body) return 0;
    return fd_reply(out, cap, 201, "Created", nullptr, body);
  }
  if (patch && strcmp(p, "/stage-instances/3124") == 0) {
    char topic[160] = "";
    (void)ps_json_str(req->body, "topic", topic, sizeof topic);
    char body[400];
    int n = snprintf(body, sizeof body,
      "{\"id\":\"6601\",\"guild_id\":\"2001\",\"channel_id\":\"3124\",\"topic\":\"%s\",\"privacy_level\":2}", topic);
    if (n <= 0 || (size_t)n >= sizeof body) return 0;
    return fd_reply(out, cap, 200, "OK", nullptr, body);
  }
  if (del && strcmp(p, "/stage-instances/3124") == 0) return fd_reply(out, cap, 204, "No Content", nullptr, "");

  if (strstr(p, "/profile") != nullptr) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"user\":{\"id\":\"42\",\"username\":\"me\"}}");
  }
  return fd_reply(out, cap, 204, "No Content", nullptr, "");
}

// ---- fake Main Gateway (runs in the forked child) ----------------------------------------------------------

typedef struct ps_gw_ud {
  int slice;
} ps_gw_ud;

static void ps_script_gw(void *ud, fg_conn *c)
{
  const ps_gw_ud *u = ud;
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
    "{\"id\":\"3121\",\"type\":0,\"name\":\"general\",\"guild_id\":\"2001\"},"
    "{\"id\":\"3122\",\"type\":0,\"name\":\"off-topic\",\"guild_id\":\"2001\"},"
    "{\"id\":\"3123\",\"type\":5,\"name\":\"announcements\",\"guild_id\":\"2001\"},"
    "{\"id\":\"3124\",\"type\":13,\"name\":\"Stage Room\",\"guild_id\":\"2001\"},"
    "{\"id\":\"3125\",\"type\":2,\"name\":\"Lounge\",\"guild_id\":\"2001\"}],"
    "\"members\":[{\"user\":{\"id\":\"42\",\"username\":\"me\"}},{\"user\":{\"id\":\"1002\",\"username\":\"ada\"}}]}],"
    "\"private_channels\":[]}}",
    c->self_url);
  if (!fg_send(c, ready)) return;

  unsigned beats = 0;
  for (;;) {
    int r = fg_recv(c, buf, sizeof buf, 100);
    if (r == FG_TIMEOUT) continue;
    if (r != FG_GOT_TEXT) break;
    if (strstr(buf, "\"op\":1,") != nullptr || strstr(buf, "\"op\":1}") != nullptr) {
      if (!fg_send(c, "{\"op\":11}")) break;
      beats++;
      // Events slice: pace one GUILD_SCHEDULED_EVENT_* dispatch per heartbeat after the session is up, so the
      // parent can assert every intermediate store state (CREATE -> UPDATE -> USER_ADD -> USER_REMOVE -> DELETE).
      if (u->slice == 3) {
        char ev[1024];
        const char *payload = nullptr;
        if (beats == 5) {
          payload = "{\"op\":0,\"t\":\"GUILD_SCHEDULED_EVENT_CREATE\",\"s\":2,\"d\":{\"id\":\"97001\",\"guild_id\":\"2001\","
                    "\"channel_id\":\"3124\",\"creator_id\":\"42\",\"name\":\"Gateway Event\",\"description\":\"\","
                    "\"scheduled_start_time\":\"2026-11-20T18:00:00.000Z\",\"privacy_level\":2,\"status\":1,"
                    "\"entity_type\":1,\"user_count\":5}}";
        } else if (beats == 6) {
          payload = "{\"op\":0,\"t\":\"GUILD_SCHEDULED_EVENT_UPDATE\",\"s\":3,\"d\":{\"id\":\"97001\",\"guild_id\":\"2001\","
                    "\"channel_id\":\"3124\",\"creator_id\":\"42\",\"name\":\"Gateway Event Live\",\"description\":\"\","
                    "\"scheduled_start_time\":\"2026-11-20T18:00:00.000Z\",\"privacy_level\":2,\"status\":2,"
                    "\"entity_type\":1,\"user_count\":5}}";
        } else if (beats == 7) {
          payload = "{\"op\":0,\"t\":\"GUILD_SCHEDULED_EVENT_USER_ADD\",\"s\":4,\"d\":{\"guild_scheduled_event_id\":\"97001\","
                    "\"guild_id\":\"2001\",\"user_id\":\"1002\"}}";
        } else if (beats == 8) {
          payload = "{\"op\":0,\"t\":\"GUILD_SCHEDULED_EVENT_USER_REMOVE\",\"s\":5,\"d\":{\"guild_scheduled_event_id\":\"97001\","
                    "\"guild_id\":\"2001\",\"user_id\":\"1002\"}}";
        } else if (beats == 9) {
          payload = "{\"op\":0,\"t\":\"GUILD_SCHEDULED_EVENT_DELETE\",\"s\":6,\"d\":{\"id\":\"97001\",\"guild_id\":\"2001\"}}";
        }
        if (payload) {
          (void)snprintf(ev, sizeof ev, "%s", payload);
          if (!fg_send(c, ev)) break;
        }
      }
      continue;
    }
    if (strstr(buf, "\"op\":3") != nullptr || strstr(buf, "\"op\":14") != nullptr) continue;
  }
}

// ---- server lifecycle --------------------------------------------------------------------------------------

typedef struct ps_servers {
  fd_server http;
  fg_server gw;
  ps_http_ud hud;
  ps_gw_ud gud;
} ps_servers;

static bool ps_servers_start(ps_servers *s, int slice, const uint8_t *seed, size_t seed_len)
{
  memset(s, 0, sizeof *s);
  s->hud.srv = &s->http;
  s->hud.slice = slice;
  s->hud.next_id = 900001;
  if (seed && seed_len > 0 && seed_len <= sizeof s->hud.seed_raw) {
    memcpy(s->hud.seed_raw, seed, seed_len);
    s->hud.seed_len = seed_len;
  }
  s->gud.slice = slice;

  // SDL installs a SIGTERM handler (quit event); the forked fake servers would inherit it and survive
  // fg_stop/fd_stop's SIGTERM until their alarm fires. Fork with the default disposition, then restore.
  struct sigaction sa_dfl, sa_old;
  memset(&sa_dfl, 0, sizeof sa_dfl);
  sa_dfl.sa_handler = SIG_DFL;
  sigemptyset(&sa_dfl.sa_mask);
  (void)sigaction(SIGTERM, &sa_dfl, &sa_old);

  if (!fd_start(&s->http, ps_respond, &s->hud)) {
    (void)sigaction(SIGTERM, &sa_old, nullptr);
    return false;
  }
  p_http_server = &s->http;
  int flags = fcntl(s->http.pipe_rd, F_GETFL, 0);
  if (flags >= 0) (void)fcntl(s->http.pipe_rd, F_SETFL, flags | O_NONBLOCK);

  if (!fg_start(&s->gw, ps_script_gw, &s->gud)) {
    fd_stop(&s->http, nullptr, 0);
    p_http_server = nullptr;
    (void)sigaction(SIGTERM, &sa_old, nullptr);
    return false;
  }
  (void)sigaction(SIGTERM, &sa_old, nullptr);
  return true;
}

static void ps_servers_stop(ps_servers *s)
{
  fg_stop(&s->gw, nullptr, 0);
  ps_drain_hits();
  fd_hit tail[64];
  size_t nt = fd_stop(&s->http, tail, 64);
  for (size_t i = 0; i < nt && p_nhits < PS_MAX_HITS; i++) p_hits[p_nhits++] = tail[i];
  p_http_server = nullptr;
}

// Build the live transport pointed at the fake servers and hand it to the app session.
static ccord_status ps_connect_live(const ps_servers *s, ccord_error *err)
{
  ccord_live_config lcfg = {
    .token = "REDACTED_VERIFY_TOKEN",
    .api_base = s->http.base,
    .gateway_url = s->gw.url,
    .request_timeout_ms = 3000,
    .now_ms = p_ctx->now_ms,
    .clock_ud = p_ctx->ud,
  };
  ccord_transport *tr = ccord_live_transport_create(&lcfg, err);
  if (!tr) return err->code ? err->code : CCORD_ERR_IO;
  return p_ctx->set_transport(p_ctx->ud, tr, err);
}

// Wait until the gateway is connected and the first text channel is auto-selected.
static bool ps_wait_connected(void)
{
  for (int i = 0; i < PS_WAIT_MAX; i++) {
    ps_tick();
    if (ccord_session_state(p_ctx->session) == CCORD_CONN_CONNECTED &&
        ccord_store_channel(p_ctx->store, PS_CH_GENERAL) != nullptr &&
        ccord_store_selected_channel(p_ctx->store) == PS_CH_GENERAL) {
      return true;
    }
  }
  return false;
}

// Find a recorded request by method + exact path (arrival order).
static const fd_hit *ps_find_hit(const char *method, const char *path)
{
  for (size_t i = 0; i < p_nhits; i++) {
    if (strcmp(p_hits[i].method, method) == 0 && strcmp(p_hits[i].path, path) == 0) return &p_hits[i];
  }
  return nullptr;
}

static size_t ps_count_hits(const char *method, const char *path_substr)
{
  size_t n = 0;
  for (size_t i = 0; i < p_nhits; i++) {
    if (strcmp(p_hits[i].method, method) == 0 && strstr(p_hits[i].path, path_substr) != nullptr) n++;
  }
  return n;
}

// ---- Result 1: messaging parity ----------------------------------------------------------------------------

int ccord_verify_messaging_slice(const ccord_verify_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->renderer || !ctx->platform || !ctx->frames ||
      !ctx->advance_ms || !ctx->now_ms || !ctx->input || !ctx->set_transport) {
    return ps_fail("messaging", "invalid context");
  }
  p_ctx = ctx;
  p_hits = calloc(PS_MAX_HITS, sizeof *p_hits);
  if (!p_hits) return ps_fail("messaging", "out of memory");
  p_nhits = 0;
  ccord_store *store = ctx->store;
  ccord_ui *ui = ctx->ui;
  ccord_error err = {0};

  // Saved Messages persistence writes settings.ini under the data dir: point it at a scratch dir.
  char scratch[512];
  if (!ps_make_scratch(scratch, sizeof scratch)) {
    free(p_hits);
    p_hits = nullptr;
    return ps_fail("messaging", "mkdtemp scratch dir");
  }
  char orig_data_dir[512];
  (void)snprintf(orig_data_dir, sizeof orig_data_dir, "%s", ccord_paths_data_dir());
  ccord_paths_set_data_dir(scratch);

  ps_servers srv;
  if (!ps_servers_start(&srv, 1, nullptr, 0)) {
    free(p_hits);
    p_hits = nullptr;
    ccord_paths_set_data_dir(orig_data_dir);
    return ps_fail("messaging", "start fake servers");
  }

  int rc = 1;
  bool forward_ok = false, crosspost_ok = false, copy_link_ok = false, mark_unread_ok = false;
  bool tts_ok = false, saved_ok = false, slash_ok = false;

  if (ps_connect_live(&srv, &err) != CCORD_OK) { ps_fail("messaging", err.message); goto cleanup; }
  if (!ps_wait_connected()) { ps_fail("messaging", "gateway did not connect"); goto cleanup; }
  if (ccord_session_select_guild(ctx->session, PS_GUILD, &err) != CCORD_OK) { ps_fail("messaging", err.message); goto cleanup; }

  // Two confirmed messages: the second is the target for link / unread / save / forward.
  ccord_id m0 = ps_send_confirmed(PS_CH_GENERAL, "first parity message");
  ccord_id m1 = ps_send_confirmed(PS_CH_GENERAL, "parity target message");
  if (m0 == CCORD_ID_NONE || m1 == CCORD_ID_NONE) { ps_fail("messaging", "composer send not confirmed"); goto cleanup; }
  ccord_menu_context mctx = {.target = CCORD_MENU_MESSAGE, .guild_id = PS_GUILD, .channel_id = PS_CH_GENERAL,
                             .message_id = m1, .user_id = PS_SELF};

  // Copy Message Link -> clipboard carries discord.com/channels/<guild>/<channel>/<message>.
  if (!ps_open_menu_and_click(&mctx, "Copy Message Link")) { ps_fail("messaging", "Copy Message Link row"); goto cleanup; }
  ps_tick();
  {
    char *clip = ccord_platform_clipboard_get(ctx->platform);
    char want[128];
    (void)snprintf(want, sizeof want, "https://discord.com/channels/%llu/%llu/%llu",
                   (unsigned long long)PS_GUILD, (unsigned long long)PS_CH_GENERAL, (unsigned long long)m1);
    copy_link_ok = clip && strcmp(clip, want) == 0;
    ccord_free(clip);
    if (!copy_link_ok) { ps_fail("messaging", "clipboard link mismatch"); goto cleanup; }
  }

  // Mark Unread -> POST .../<prev>/ack + the channel reports unread again.
  if (!ps_open_menu_and_click(&mctx, "Mark Unread")) { ps_fail("messaging", "Mark Unread row"); goto cleanup; }
  {
    char ackpath[96];
    (void)snprintf(ackpath, sizeof ackpath, "/channels/%llu/messages/%llu/ack",
                   (unsigned long long)PS_CH_GENERAL, (unsigned long long)m0);
    // The store flips to unread synchronously but the manual ack POST travels asynchronously;
    // wait for BOTH the store state and the server-side hit (auto read-acks carry {"token":null}).
    const ccord_channel *c = nullptr;
    const fd_hit *ack = nullptr;
    for (int i = 0; i < PS_WAIT_MAX; i++) {
      ps_tick();
      c = ccord_store_channel(store, PS_CH_GENERAL);
      ack = nullptr;
      for (size_t j = 0; j < p_nhits; j++) {
        if (strcmp(p_hits[j].method, "POST") == 0 && strcmp(p_hits[j].path, ackpath) == 0 &&
            strstr(p_hits[j].body, "\"manual\":true") != nullptr) { ack = &p_hits[j]; break; }
      }
      if (c && c->unread_count >= 1 && c->last_read_id < m1 && ack) break;
    }
    mark_unread_ok = c && c->unread_count >= 1 && c->last_read_id < m1 && ack != nullptr;
    if (!mark_unread_ok) { ps_fail("messaging", "mark unread ack / unread state"); goto cleanup; }
  }

  // Save Message -> bookmark; persist to settings.ini; a fresh in-memory list reloads from disk.
  if (!ps_open_menu_and_click(&mctx, "Save Message")) { ps_fail("messaging", "Save Message row"); goto cleanup; }
  ps_tick();
  if (!ui_saved_messages_contains(ui, PS_CH_GENERAL, m1) || !ccord_store_message_saved(store, PS_CH_GENERAL, m1)) {
    ps_fail("messaging", "save did not bookmark"); goto cleanup;
  }
  ui_settings_save(ui);
  ui->nsaved_msgs = 0;   // simulate a fresh process, then reload from disk
  ui_settings_init(ui);
  ps_tick();
  if (ui->nsaved_msgs != 1 || !ui_saved_messages_contains(ui, PS_CH_GENERAL, m1) ||
      !ccord_store_message_saved(store, PS_CH_GENERAL, m1)) {
    ps_fail("messaging", "saved messages did not persist/reload"); goto cleanup;
  }
  {
    ui_saved_msgs_debug sd;
    ui_saved_messages_get_debug(ui, &sd);
    if (sd.header_btn.w <= 0.0f) { ps_fail("messaging", "saved header button"); goto cleanup; }
    ps_click(sd.header_btn.x + sd.header_btn.w * 0.5f, sd.header_btn.y + sd.header_btn.h * 0.5f);
    ps_tick();
    ui_saved_messages_get_debug(ui, &sd);
    if (!sd.open || sd.count != 1 || sd.hit_count < 1 || sd.hits[0].message_id != m1) {
      ps_fail("messaging", "saved popover did not show the bookmark"); goto cleanup;
    }
    ps_key(CCORD_KEY_ESCAPE, 0);
    ps_tick();
  }
  saved_ok = true;

  // Forward -> channel picker -> POST /channels/<dst>/messages with message_reference type 1.
  if (!ps_open_menu_and_click(&mctx, "Forward")) { ps_fail("messaging", "Forward row"); goto cleanup; }
  ps_tick();
  if (!ui_forward_picker_active(ui)) { ps_fail("messaging", "forward picker not open"); goto cleanup; }
  ps_type("off-topic");
  ps_tick();
  ps_key(CCORD_KEY_RETURN, 0);
  {
    char path[64];
    (void)snprintf(path, sizeof path, "/channels/%llu/messages", (unsigned long long)PS_CH_OFFTOPIC);
    // fwd_do closes the picker + navigates synchronously; the forward POST + its echo are async,
    // so wait for the confirmed forward in the destination store AND the server-side hit.
    const fd_hit *fwd = nullptr;
    const ccord_message *fm = nullptr;
    for (int i = 0; i < PS_WAIT_MAX; i++) {
      ps_tick();
      fwd = ps_find_hit("POST", path);
      size_t n = ccord_store_message_count(store, PS_CH_OFFTOPIC);
      fm = n ? ccord_store_message_at(store, PS_CH_OFFTOPIC, n - 1) : nullptr;
      bool store_ok = !ui_forward_picker_active(ui) && ccord_store_selected_channel(store) == PS_CH_OFFTOPIC &&
                      fm && fm->rich && fm->rich->reference.kind == CCORD_REF_FORWARD &&
                      fm->rich->reference.message_id == m1 && fm->rich->forward.present &&
                      strcmp(fm->rich->forward.content, "parity target message") == 0;
      if (store_ok && fwd) break;
    }
    forward_ok = fwd && strstr(fwd->body, "\"message_reference\"") != nullptr &&
                 strstr(fwd->body, "\"type\":1") != nullptr &&
                 fm && fm->rich && fm->rich->reference.kind == CCORD_REF_FORWARD &&
                 fm->rich->reference.message_id == m1 && fm->rich->forward.present &&
                 strcmp(fm->rich->forward.content, "parity target message") == 0;
    if (!forward_ok) { ps_fail("messaging", "forward reference / snapshot"); goto cleanup; }
  }

  // Crosspost an announcement -> POST .../crosspost + the CROSSPOSTED flag lands on the message.
  if (ccord_session_select_channel(ctx->session, PS_CH_ANN, &err) != CCORD_OK) { ps_fail("messaging", err.message); goto cleanup; }
  ps_tick();
  ccord_id ann = ps_send_confirmed(PS_CH_ANN, "publish to followers");
  if (ann == CCORD_ID_NONE) { ps_fail("messaging", "announcement send"); goto cleanup; }
  ccord_menu_context actx = {.target = CCORD_MENU_MESSAGE, .guild_id = PS_GUILD, .channel_id = PS_CH_ANN,
                             .message_id = ann, .user_id = PS_SELF};
  if (!ps_open_menu_and_click(&actx, "Publish Message")) { ps_fail("messaging", "Publish Message row"); goto cleanup; }
  for (int i = 0; i < PS_WAIT_MAX; i++) {
    ps_tick();
    const ccord_message *m = ccord_store_find_message(store, PS_CH_ANN, ann);
    if (m && (ccord_message_flags(m) & CCORD_MSGFLAG_CROSSPOSTED) != 0u) break;
  }
  {
    char path[96];
    (void)snprintf(path, sizeof path, "/channels/%llu/messages/%llu/crosspost",
                   (unsigned long long)PS_CH_ANN, (unsigned long long)ann);
    const ccord_message *m = ccord_store_find_message(store, PS_CH_ANN, ann);
    crosspost_ok = ps_find_hit("POST", path) != nullptr && m && (ccord_message_flags(m) & CCORD_MSGFLAG_CROSSPOSTED) != 0u;
    if (!crosspost_ok) { ps_fail("messaging", "crosspost flag / request"); goto cleanup; }
  }

  // /tts -> POST /channels/<c>/messages with tts:true.
  if (ccord_session_select_channel(ctx->session, PS_CH_GENERAL, &err) != CCORD_OK) { ps_fail("messaging", err.message); goto cleanup; }
  ps_tick();
  ps_clear_composer();
  ps_type("/tts speak this aloud");
  ps_key(CCORD_KEY_RETURN, 0);
  for (int i = 0; i < PS_WAIT_MAX; i++) {
    ps_tick();
    size_t n = ccord_store_message_count(store, PS_CH_GENERAL);
    const ccord_message *m = n ? ccord_store_message_at(store, PS_CH_GENERAL, n - 1) : nullptr;
    if (m && m->tts && strcmp(m->content, "speak this aloud") == 0) break;
  }
  {
    size_t n = ccord_store_message_count(store, PS_CH_GENERAL);
    const ccord_message *m = n ? ccord_store_message_at(store, PS_CH_GENERAL, n - 1) : nullptr;
    const fd_hit *tts = nullptr;
    for (size_t i = 0; i < p_nhits; i++) {
      if (strcmp(p_hits[i].method, "POST") == 0 && strstr(p_hits[i].path, "/messages") != nullptr &&
          strstr(p_hits[i].body, "\"tts\":true") != nullptr) { tts = &p_hits[i]; break; }
    }
    tts_ok = m && m->tts && strcmp(m->content, "speak this aloud") == 0 && tts != nullptr;
    if (!tts_ok) { ps_fail("messaging", "tts message / request"); goto cleanup; }
  }

  // Slash SUB_COMMAND_GROUP/SUB_COMMAND + USER/MESSAGE context-menu application commands.
  if (ccord_session_select_channel(ctx->session, PS_CH_GENERAL, &err) != CCORD_OK) { ps_fail("messaging", err.message); goto cleanup; }
  ps_tick();
  (void)ccord_session_ensure_slash_commands(ctx->session, PS_GUILD, PS_CH_GENERAL, ctx->now_ms(ctx->ud));
  for (int i = 0; i < PS_WAIT_MAX; i++) {
    ps_tick();
    size_t ncmd = 0;
    (void)ccord_store_slash_commands(store, PS_GUILD, &ncmd);
    if (ncmd >= 3) break;
  }
  ps_clear_composer();
  ps_type("/config theme set preset:dark");
  ps_key(CCORD_KEY_RETURN, 0);
  for (int i = 0; i < PS_WAIT_MAX; i++) {
    ps_tick();
    if (ps_count_hits("POST", "/interactions") >= 1) break;
  }
  // MESSAGE (type 3) command on m1.
  if (ccord_session_select_channel(ctx->session, PS_CH_GENERAL, &err) != CCORD_OK) { ps_fail("messaging", err.message); goto cleanup; }
  ps_tick();
  if (!ps_open_menu_and_click(&mctx, "QuoteMessage")) { ps_fail("messaging", "QuoteMessage row"); goto cleanup; }
  for (int i = 0; i < PS_WAIT_MAX; i++) { ps_tick(); if (ps_count_hits("POST", "/interactions") >= 2) break; }
  // USER (type 2) command on member 1002.
  ccord_menu_context uctx = {.target = CCORD_MENU_MEMBER, .guild_id = PS_GUILD, .channel_id = PS_CH_GENERAL, .user_id = PS_MEMBER};
  if (!ps_open_menu_and_click(&uctx, "UserInfo")) { ps_fail("messaging", "UserInfo row"); goto cleanup; }
  for (int i = 0; i < PS_WAIT_MAX; i++) { ps_tick(); if (ps_count_hits("POST", "/interactions") >= 3) break; }
  {
    bool sub = false, msgcmd = false, usercmd = false;
    char m1str[64];
    (void)snprintf(m1str, sizeof m1str, "\"target_id\":\"%llu\"", (unsigned long long)m1);
    for (size_t i = 0; i < p_nhits; i++) {
      if (strcmp(p_hits[i].method, "POST") != 0 || strcmp(p_hits[i].path, "/interactions") != 0) continue;
      const char *b = p_hits[i].body;
      if (strstr(b, "\"name\":\"config\"") && strstr(b, "\"name\":\"theme\"") && strstr(b, "\"name\":\"set\"") &&
          strstr(b, "\"name\":\"preset\"") && strstr(b, "\"value\":\"dark\"")) sub = true;
      if (strstr(b, "\"type\":3") && strstr(b, m1str)) msgcmd = true;
      if (strstr(b, "\"type\":2") && strstr(b, "\"target_id\":\"1002\"")) usercmd = true;
    }
    slash_ok = sub && msgcmd && usercmd;
    if (!slash_ok) { ps_fail("messaging", "slash / context interaction payloads"); goto cleanup; }
  }

  ps_settle();
  rc = 0;

cleanup:
  ps_servers_stop(&srv);
  free(p_hits);
  p_hits = nullptr;
  ccord_paths_set_data_dir(orig_data_dir);
  if (rc != 0) return 1;
  printf("forward_msg_ref_type1=%d\n", forward_ok ? 1 : 0);
  printf("crosspost_published=%d\n", crosspost_ok ? 1 : 0);
  printf("copy_message_link=%d\n", copy_link_ok ? 1 : 0);
  printf("mark_unread_ack=%d\n", mark_unread_ok ? 1 : 0);
  printf("tts_message_sent=%d\n", tts_ok ? 1 : 0);
  printf("saved_messages_persisted=%d\n", saved_ok ? 1 : 0);
  printf("slash_subcommand_and_ctx_cmd=%d\n", slash_ok ? 1 : 0);
  return 0;
}

// ---- Result 2: settings-proto favorite-GIF writeback -------------------------------------------------------

// The unknown wire field the seed carries (field 99, varint 0x42): every PATCH must preserve it verbatim.
static const uint8_t k_ps_unknown[] = {0x98u, 0x06u, 0x42u};

// Assert a settings-proto blob (raw bytes) keeps the unknown field and decodes to the expected favorite URLs.
static bool ps_proto_check(const uint8_t *raw, size_t len, const char *const *urls, size_t nurls)
{
  if (!ps_contains_bytes(raw, len, k_ps_unknown, sizeof k_ps_unknown)) return false;
  ccord_user_expressions ux;
  ccord_error err = {0};
  if (ccord_user_expressions_decode(raw, len, &ux, &err) != CCORD_OK) return false;
  bool ok = ux.gif_count == nurls;
  for (size_t i = 0; ok && i < nurls; i++) {
    bool found = false;
    for (size_t j = 0; j < ux.gif_count; j++) {
      if (ux.gifs[j].url && strcmp(ux.gifs[j].url, urls[i]) == 0) { found = true; break; }
    }
    ok = found;
  }
  ccord_user_expressions_free(&ux);
  return ok;
}

// Private clock for the offline mock-parity session: advancing the shared app clock here would outrun the live
// gateway's heartbeat acks, so the mock runs on its own monotonic timebase.
static uint64_t p_mock_ms;
static uint64_t ps_mock_now_ms(void *ud) { (void)ud; return p_mock_ms; }

// Run the favorite-GIF add/remove writeback over the mock transport in a private bus/store/session, proving the
// same lossless flow works offline. Returns true when every PATCH the mock received preserved the unknown field.
static bool ps_settings_mock_parity(const uint8_t *seed, size_t seed_len)
{
  ccord_error err = {0};
  p_mock_ms = 1000;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = bus ? ccord_store_create(bus, &err) : nullptr;
  ccord_mock_config mc = {.latency_ms = 0, .now_ms = ps_mock_now_ms, .clock_ud = nullptr};
  ccord_transport *mock = (bus && store) ? ccord_mock_transport_create(&mc, &err) : nullptr;
  ccord_session *priv = (bus && store && mock) ? ccord_session_create(bus, store, mock, &err) : nullptr;
  bool ok = false;
  if (!priv) {
    if (mock) ccord_transport_destroy(mock);
    goto out;
  }
  if (ccord_session_start(priv, &err) != CCORD_OK) goto out;

  // Seed the mock's settings-proto with the same unknown-field + favorite blob the live server served.
  if (ccord_transport_patch_settings_proto(mock, seed, seed_len, &err) != CCORD_OK) goto out;
  for (int i = 0; i < 200; i++) {
    (void)ccord_session_pump(priv);
    p_mock_ms += 20;
    if (ccord_store_favorite_gif_count(store) >= 1) break;
  }
  if (ccord_store_favorite_gif_count(store) != 1) goto out;

  const char *url_a = "https://tenor.com/view/fav-seed";
  const char *url_b = "https://tenor.com/view/fav-new";
  ccord_gif_item gif_b = {.width = 480, .height = 270, .format = 1, .order = 2};
  (void)snprintf(gif_b.url, sizeof gif_b.url, "%s", url_b);
  (void)snprintf(gif_b.src, sizeof gif_b.src, "https://media.tenor.com/fav-new.gif");

  // Add B: the session re-encodes over the cached seed and PATCHes the mock.
  if (ccord_session_toggle_favorite_gif(priv, &gif_b, &err) != CCORD_OK) goto out;
  for (int i = 0; i < 200; i++) { (void)ccord_session_pump(priv); p_mock_ms += 20; }
  // Read back what the mock persisted (a fresh GET) and check the unknown field + both favorites survived.
  if (ccord_session_request_expression_data(priv, CCORD_EXPR_SETTINGS_PROTO, "", true, &err) != CCORD_OK) goto out;
  for (int i = 0; i < 200; i++) { (void)ccord_session_pump(priv); p_mock_ms += 20; }
  {
    size_t rlen = 0;
    const uint8_t *raw = ccord_store_get_raw_settings_proto(store, &rlen);
    const char *want_ab[] = {url_a, url_b};
    if (!raw || !ps_proto_check(raw, rlen, want_ab, 2)) goto out;
  }

  // Remove B: back to just the seed favorite, unknown field still intact.
  if (ccord_session_toggle_favorite_gif(priv, &gif_b, &err) != CCORD_OK) goto out;
  for (int i = 0; i < 200; i++) { (void)ccord_session_pump(priv); p_mock_ms += 20; }
  if (ccord_session_request_expression_data(priv, CCORD_EXPR_SETTINGS_PROTO, "", true, &err) != CCORD_OK) goto out;
  for (int i = 0; i < 200; i++) { (void)ccord_session_pump(priv); p_mock_ms += 20; }
  {
    size_t rlen = 0;
    const uint8_t *raw = ccord_store_get_raw_settings_proto(store, &rlen);
    const char *want_a[] = {url_a};
    if (!raw || !ps_proto_check(raw, rlen, want_a, 1)) goto out;
  }
  ok = true;

out:
  if (priv) ccord_session_destroy(priv);   // owns the mock transport
  if (store) ccord_store_destroy(store);
  if (bus) ccord_bus_destroy(bus);
  return ok;
}

int ccord_verify_settings_proto_slice(const ccord_verify_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->renderer || !ctx->frames ||
      !ctx->advance_ms || !ctx->now_ms || !ctx->input || !ctx->set_transport) {
    return ps_fail("settings-proto", "invalid context");
  }
  p_ctx = ctx;
  p_hits = calloc(PS_MAX_HITS, sizeof *p_hits);
  if (!p_hits) return ps_fail("settings-proto", "out of memory");
  p_nhits = 0;
  ccord_store *store = ctx->store;
  ccord_error err = {0};

  // Build the seed: one favorite GIF over the unknown wire field, using the production encoder.
  const char *url_a = "https://tenor.com/view/fav-seed";
  ccord_proto_gif seed_gif = {.url = url_a, .src = "https://media.tenor.com/fav-seed.gif",
                              .format = 1, .width = 320, .height = 240, .order = 1};
  uint8_t *seed = nullptr;
  size_t seed_len = 0;
  if (ccord_user_expressions_encode_gifs(&seed_gif, 1, k_ps_unknown, sizeof k_ps_unknown, &seed, &seed_len, &err) != CCORD_OK) {
    free(p_hits);
    p_hits = nullptr;
    return ps_fail("settings-proto", err.message);
  }

  ps_servers srv;
  if (!ps_servers_start(&srv, 2, seed, seed_len)) {
    free(p_hits);
    p_hits = nullptr;
    ccord_free(seed);
    return ps_fail("settings-proto", "start fake servers");
  }

  int rc = 1;
  bool patch_sent = false, unknown_preserved = false, roundtrip = false, mock_parity = false;

  if (ps_connect_live(&srv, &err) != CCORD_OK) { ps_fail("settings-proto", err.message); goto cleanup; }
  if (!ps_wait_connected()) { ps_fail("settings-proto", "gateway did not connect"); goto cleanup; }

  // Load the seed over live REST; the store caches the raw wire bytes and the favorite.
  if (ccord_session_request_expression_data(ctx->session, CCORD_EXPR_SETTINGS_PROTO, "", true, &err) != CCORD_OK) {
    ps_fail("settings-proto", err.message); goto cleanup;
  }
  for (int i = 0; i < PS_WAIT_MAX; i++) {
    ps_tick();
    if (ccord_store_expr_blob_state(store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_READY &&
        ccord_store_favorite_gif_count(store) >= 1) break;
  }
  if (ccord_store_favorite_gif_count(store) != 1 || !ccord_store_is_favorite_gif(store, url_a)) {
    ps_fail("settings-proto", "seed favorite did not load"); goto cleanup;
  }
  {
    size_t rlen = 0;
    const uint8_t *raw = ccord_store_get_raw_settings_proto(store, &rlen);
    if (!raw || !ps_contains_bytes(raw, rlen, k_ps_unknown, sizeof k_ps_unknown)) {
      ps_fail("settings-proto", "seed unknown field not cached"); goto cleanup;
    }
  }

  // Toggle a second favorite ON: the session PATCHes the re-encoded blob.
  ccord_gif_item gif_b = {.width = 480, .height = 270, .format = 1, .order = 2};
  (void)snprintf(gif_b.url, sizeof gif_b.url, "https://tenor.com/view/fav-new");
  (void)snprintf(gif_b.src, sizeof gif_b.src, "https://media.tenor.com/fav-new.gif");
  uint64_t rev0 = ccord_store_expr_revision(store);
  if (ccord_session_toggle_favorite_gif(ctx->session, &gif_b, &err) != CCORD_OK) { ps_fail("settings-proto", err.message); goto cleanup; }
  for (int i = 0; i < PS_WAIT_MAX; i++) { ps_tick(); if (ps_count_hits("PATCH", "/users/@me/settings-proto/2") >= 1) break; }
  if (ccord_store_favorite_gif_count(store) != 2) { ps_fail("settings-proto", "add toggle not in store"); goto cleanup; }
  // The SETTINGS_PROTO ingest is additive; let the add PATCH response land before toggling off, or it would
  // re-add the GIF we just removed. The raw-cache revision bumps once optimistically and again on ingest.
  for (int i = 0; i < PS_WAIT_MAX; i++) { ps_tick(); if (ccord_store_expr_revision(store) >= rev0 + 2) break; }

  // Toggle it OFF again: a second PATCH returns to just the seed favorite.
  if (ccord_session_toggle_favorite_gif(ctx->session, &gif_b, &err) != CCORD_OK) { ps_fail("settings-proto", err.message); goto cleanup; }
  for (int i = 0; i < PS_WAIT_MAX; i++) { ps_tick(); if (ps_count_hits("PATCH", "/users/@me/settings-proto/2") >= 2) break; }
  if (ccord_store_favorite_gif_count(store) != 1) { ps_fail("settings-proto", "remove toggle not in store"); goto cleanup; }

  // Server-side truth: inspect every PATCH body that actually reached the fake Discord.
  {
    size_t npatch = 0;
    bool all_unknown = true, add_ok = false, remove_ok = false;
    const char *want_ab[] = {url_a, "https://tenor.com/view/fav-new"};
    const char *want_a[] = {url_a};
    for (size_t i = 0; i < p_nhits; i++) {
      if (strcmp(p_hits[i].method, "PATCH") != 0 || strcmp(p_hits[i].path, "/users/@me/settings-proto/2") != 0) continue;
      char b64[1600];
      if (!ps_json_str(p_hits[i].body, "settings", b64, sizeof b64)) { all_unknown = false; continue; }
      uint8_t raw[1200];
      size_t rlen = ps_b64_decode(b64, strlen(b64), raw, sizeof raw);
      npatch++;
      if (!ps_contains_bytes(raw, rlen, k_ps_unknown, sizeof k_ps_unknown)) all_unknown = false;
      if (npatch == 1) add_ok = ps_proto_check(raw, rlen, want_ab, 2);
      if (npatch == 2) remove_ok = ps_proto_check(raw, rlen, want_a, 1);
    }
    patch_sent = npatch >= 2;
    unknown_preserved = patch_sent && all_unknown;
    roundtrip = add_ok && remove_ok;
    if (!patch_sent || !unknown_preserved || !roundtrip) {
      ps_fail("settings-proto", "PATCH bodies lost the unknown field / favorites"); goto cleanup;
    }
  }

  // Live/mock parity: the identical writeback over the offline mock transport.
  mock_parity = ps_settings_mock_parity(seed, seed_len);
  if (!mock_parity) { ps_fail("settings-proto", "mock transport parity"); goto cleanup; }

  ps_settle();
  rc = 0;

cleanup:
  ps_servers_stop(&srv);
  free(p_hits);
  p_hits = nullptr;
  ccord_free(seed);
  if (rc != 0) return 1;
  printf("settings_proto_patch_sent=%d\n", patch_sent ? 1 : 0);
  printf("settings_proto_unknown_fields_preserved=%d\n", unknown_preserved ? 1 : 0);
  printf("settings_proto_favorites_roundtrip=%d\n", roundtrip ? 1 : 0);
  printf("settings_proto_mock_parity=%d\n", mock_parity ? 1 : 0);
  return 0;
}

// ---- Result 3: scheduled events + stage instances ----------------------------------------------------------

int ccord_verify_events_stage_slice(const ccord_verify_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->renderer || !ctx->frames ||
      !ctx->advance_ms || !ctx->now_ms || !ctx->input || !ctx->set_transport) {
    return ps_fail("events-stage", "invalid context");
  }
  p_ctx = ctx;
  p_hits = calloc(PS_MAX_HITS, sizeof *p_hits);
  if (!p_hits) return ps_fail("events-stage", "out of memory");
  p_nhits = 0;
  ccord_store *store = ctx->store;
  ccord_ui *ui = ctx->ui;
  ccord_error err = {0};

  ps_servers srv;
  if (!ps_servers_start(&srv, 3, nullptr, 0)) {
    free(p_hits);
    p_hits = nullptr;
    return ps_fail("events-stage", "start fake servers");
  }

  int rc = 1;
  bool crud_ok = false, rsvp_ok = false, gw_sync_ok = false, stage_ok = false;

  if (ps_connect_live(&srv, &err) != CCORD_OK) { ps_fail("events-stage", err.message); goto cleanup; }
  if (!ps_wait_connected()) { ps_fail("events-stage", "gateway did not connect"); goto cleanup; }
  if (ccord_session_select_guild(ctx->session, PS_GUILD, &err) != CCORD_OK) { ps_fail("events-stage", err.message); goto cleanup; }

  // Gateway sync: assert every intermediate store state as the paced dispatches arrive.
  {
    bool created = false, updated = false, added = false, removed = false, deleted = false;
    for (int i = 0; i < PS_WAIT_MAX && !created; i++) {
      ps_tick();
      const ccord_scheduled_event *e = ccord_store_find_scheduled_event(store, 97001);
      created = e && strcmp(e->name, "Gateway Event") == 0 && e->user_count == 5;
    }
    for (int i = 0; i < PS_WAIT_MAX && !updated; i++) {
      ps_tick();
      const ccord_scheduled_event *e = ccord_store_find_scheduled_event(store, 97001);
      updated = e && strcmp(e->name, "Gateway Event Live") == 0;
    }
    for (int i = 0; i < PS_WAIT_MAX && !added; i++) {
      ps_tick();
      const ccord_scheduled_event *e = ccord_store_find_scheduled_event(store, 97001);
      added = e && e->user_count == 6;
    }
    for (int i = 0; i < PS_WAIT_MAX && !removed; i++) {
      ps_tick();
      const ccord_scheduled_event *e = ccord_store_find_scheduled_event(store, 97001);
      removed = e && e->user_count == 5;
    }
    for (int i = 0; i < PS_WAIT_MAX && !deleted; i++) {
      ps_tick();
      deleted = ccord_store_find_scheduled_event(store, 97001) == nullptr;
    }
    gw_sync_ok = created && updated && added && removed && deleted;
    if (!gw_sync_ok) { ps_fail("events-stage", "gateway GUILD_SCHEDULED_EVENT_* sync"); goto cleanup; }
  }

  // Events modal: opening it requests the list over REST.
  ui_scheduled_events_open(ui, PS_GUILD);
  for (int i = 0; i < PS_WAIT_MAX; i++) {
    ps_tick();
    if (ccord_store_scheduled_event_count(store, PS_GUILD) >= 1 &&
        ccord_store_find_scheduled_event(store, 7701) != nullptr) break;
  }
  if (ccord_store_find_scheduled_event(store, 7701) == nullptr) { ps_fail("events-stage", "event list load"); goto cleanup; }

  // Create via the session/admin builder -> POST /guilds/<g>/scheduled-events.
  {
    ccord_admin_request req;
    ccord_scheduled_event ev = {0};
    ev.guild_id = PS_GUILD;
    ev.channel_id = PS_CH_STAGE;
    ev.entity_type = CCORD_SCHEDULED_EVENT_STAGE_INSTANCE;
    ev.scheduled_start_ms = 1800000000000LL;
    (void)snprintf(ev.name, sizeof ev.name, "Launch Party");
    if (ccord_admin_event_create(&req, PS_GUILD, &ev, nullptr, &err) != CCORD_OK) { ps_fail("events-stage", err.message); goto cleanup; }
    if (ccord_session_admin(ctx->session, &req, &err) != CCORD_OK) { ps_fail("events-stage", err.message); goto cleanup; }
  }
  for (int i = 0; i < PS_WAIT_MAX; i++) { ps_tick(); if (ccord_store_find_scheduled_event(store, 7702) != nullptr) break; }
  if (ccord_store_find_scheduled_event(store, 7702) == nullptr) { ps_fail("events-stage", "event create"); goto cleanup; }

  // Modify via the session/admin builder -> PATCH /guilds/<g>/scheduled-events/<e>.
  {
    ccord_admin_request req;
    ccord_admin_event_edit edit = CCORD_ADMIN_EVENT_EDIT_INIT;
    edit.name = "Launch Party Live";
    if (ccord_admin_event_modify(&req, PS_GUILD, 7702, &edit, nullptr, &err) != CCORD_OK) { ps_fail("events-stage", err.message); goto cleanup; }
    if (ccord_session_admin(ctx->session, &req, &err) != CCORD_OK) { ps_fail("events-stage", err.message); goto cleanup; }
  }
  for (int i = 0; i < PS_WAIT_MAX; i++) {
    ps_tick();
    const ccord_scheduled_event *e = ccord_store_find_scheduled_event(store, 7702);
    if (e && strcmp(e->name, "Launch Party Live") == 0) break;
  }
  {
    const ccord_scheduled_event *e = ccord_store_find_scheduled_event(store, 7702);
    if (!e || strcmp(e->name, "Launch Party Live") != 0) { ps_fail("events-stage", "event modify"); goto cleanup; }
  }

  // RSVP through the modal: click the row's Interested button (PUT), then again (DELETE).
  {
    ui_events_debug d;
    int row = -1;
    for (int i = 0; i < PS_WAIT_MAX; i++) {
      ps_tick();
      ui_scheduled_events_get_debug(ui, &d);
      row = -1;
      for (size_t r = 0; r < d.row_count; r++) if (d.rows[r].event_id == 7702 && d.rows[r].rsvp.w > 0.0f) row = (int)r;
      if (row >= 0) break;
    }
    if (row < 0) { ps_fail("events-stage", "event row / rsvp button"); goto cleanup; }
    ps_click(d.rows[row].rsvp.x + d.rows[row].rsvp.w * 0.5f, d.rows[row].rsvp.y + d.rows[row].rsvp.h * 0.5f);
    for (int i = 0; i < PS_WAIT_MAX; i++) {
      ps_tick();
      const ccord_scheduled_event *e = ccord_store_find_scheduled_event(store, 7702);
      if (e && e->me_rsvped) break;
    }
    bool rsvp_on = false;
    { const ccord_scheduled_event *e = ccord_store_find_scheduled_event(store, 7702); rsvp_on = e && e->me_rsvped; }
    // Re-read geometry (the row may have redrawn) and click again to withdraw.
    ui_scheduled_events_get_debug(ui, &d);
    row = -1;
    for (size_t r = 0; r < d.row_count; r++) if (d.rows[r].event_id == 7702 && d.rows[r].rsvp.w > 0.0f) row = (int)r;
    if (row < 0) { ps_fail("events-stage", "rsvp row after toggle"); goto cleanup; }
    ps_click(d.rows[row].rsvp.x + d.rows[row].rsvp.w * 0.5f, d.rows[row].rsvp.y + d.rows[row].rsvp.h * 0.5f);
    for (int i = 0; i < PS_WAIT_MAX; i++) {
      ps_tick();
      const ccord_scheduled_event *e = ccord_store_find_scheduled_event(store, 7702);
      if (e && !e->me_rsvped) break;
    }
    bool rsvp_off = false;
    { const ccord_scheduled_event *e = ccord_store_find_scheduled_event(store, 7702); rsvp_off = e && !e->me_rsvped; }
    rsvp_ok = rsvp_on && rsvp_off &&
              ps_find_hit("PUT", "/guilds/2001/scheduled-events/7702/users/@me") != nullptr &&
              ps_find_hit("DELETE", "/guilds/2001/scheduled-events/7702/users/@me") != nullptr;
    if (!rsvp_ok) { ps_fail("events-stage", "rsvp PUT/DELETE / state"); goto cleanup; }
  }

  // Delete through the modal: first click arms, second confirms -> DELETE /guilds/<g>/scheduled-events/<e>.
  {
    ui_events_debug d;
    int row = -1;
    for (int i = 0; i < PS_WAIT_MAX; i++) {
      ps_tick();
      ui_scheduled_events_get_debug(ui, &d);
      row = -1;
      for (size_t r = 0; r < d.row_count; r++) if (d.rows[r].event_id == 7702 && d.rows[r].del.w > 0.0f) row = (int)r;
      if (row >= 0) break;
    }
    if (row < 0) { ps_fail("events-stage", "event delete button"); goto cleanup; }
    ps_click(d.rows[row].del.x + d.rows[row].del.w * 0.5f, d.rows[row].del.y + d.rows[row].del.h * 0.5f);
    ps_tick();
    ui_scheduled_events_get_debug(ui, &d);
    row = -1;
    for (size_t r = 0; r < d.row_count; r++) if (d.rows[r].event_id == 7702 && d.rows[r].del.w > 0.0f) row = (int)r;
    if (row < 0) { ps_fail("events-stage", "armed delete row"); goto cleanup; }
    ps_click(d.rows[row].del.x + d.rows[row].del.w * 0.5f, d.rows[row].del.y + d.rows[row].del.h * 0.5f);
    for (int i = 0; i < PS_WAIT_MAX; i++) { ps_tick(); if (ccord_store_find_scheduled_event(store, 7702) == nullptr) break; }
    crud_ok = ccord_store_find_scheduled_event(store, 7702) == nullptr &&
              ps_find_hit("POST", "/guilds/2001/scheduled-events") != nullptr &&
              ps_find_hit("PATCH", "/guilds/2001/scheduled-events/7702") != nullptr &&
              ps_find_hit("DELETE", "/guilds/2001/scheduled-events/7702") != nullptr &&
              ps_count_hits("GET", "/scheduled-events?with_user_count=true") >= 1;
    if (!crud_ok) { ps_fail("events-stage", "event CRUD requests / state"); goto cleanup; }
  }
  ps_key(CCORD_KEY_ESCAPE, 0);   // close the events modal
  ps_tick();

  // Stage: start prompt -> POST /stage-instances; topic -> PATCH; end confirm -> DELETE.
  // (A stage channel is not text-selectable; the start prompt targets it directly.)
  ui_stage_start_prompt_open(ui, PS_GUILD, PS_CH_STAGE);
  ps_tick();
  {
    ui_events_debug d;
    ui_scheduled_events_get_debug(ui, &d);
    if (d.view != UI_EVENTS_STAGE_START || d.stage_input.w <= 0.0f) { ps_fail("events-stage", "stage start prompt"); goto cleanup; }
    ps_click(d.stage_input.x + d.stage_input.w * 0.5f, d.stage_input.y + d.stage_input.h * 0.5f);
    ps_type("Parity Stage Topic");
    ps_tick();
    ui_scheduled_events_get_debug(ui, &d);
    if (d.stage_ok.w <= 0.0f) { ps_fail("events-stage", "stage ok button"); goto cleanup; }
    ps_click(d.stage_ok.x + d.stage_ok.w * 0.5f, d.stage_ok.y + d.stage_ok.h * 0.5f);
  }
  for (int i = 0; i < PS_WAIT_MAX; i++) {
    ps_tick();
    const ccord_stage_instance *si = ccord_store_stage_instance(store, PS_CH_STAGE);
    if (si && strcmp(si->topic, "Parity Stage Topic") == 0) break;
  }
  {
    const ccord_stage_instance *si = ccord_store_stage_instance(store, PS_CH_STAGE);
    if (!si || strcmp(si->topic, "Parity Stage Topic") != 0 || ps_find_hit("POST", "/stage-instances") == nullptr) {
      ps_fail("events-stage", "stage create"); goto cleanup;
    }
  }
  // Topic update through the admin builder -> PATCH /stage-instances/<c>.
  {
    ccord_admin_request req;
    if (ccord_admin_stage_topic(&req, PS_CH_STAGE, "Updated Parity Topic", nullptr, &err) != CCORD_OK) { ps_fail("events-stage", err.message); goto cleanup; }
    if (ccord_session_admin(ctx->session, &req, &err) != CCORD_OK) { ps_fail("events-stage", err.message); goto cleanup; }
  }
  for (int i = 0; i < PS_WAIT_MAX; i++) {
    ps_tick();
    const ccord_stage_instance *si = ccord_store_stage_instance(store, PS_CH_STAGE);
    if (si && strcmp(si->topic, "Updated Parity Topic") == 0) break;
  }
  {
    const ccord_stage_instance *si = ccord_store_stage_instance(store, PS_CH_STAGE);
    if (!si || strcmp(si->topic, "Updated Parity Topic") != 0 || ps_find_hit("PATCH", "/stage-instances/3124") == nullptr) {
      ps_fail("events-stage", "stage topic update"); goto cleanup;
    }
  }
  // End through the confirm prompt -> DELETE /stage-instances/<c>.
  ui_stage_end_confirm_open(ui, PS_GUILD, PS_CH_STAGE);
  ps_tick();
  {
    ui_events_debug d;
    ui_scheduled_events_get_debug(ui, &d);
    if (d.view != UI_EVENTS_STAGE_END || d.stage_ok.w <= 0.0f) { ps_fail("events-stage", "stage end prompt"); goto cleanup; }
    ps_click(d.stage_ok.x + d.stage_ok.w * 0.5f, d.stage_ok.y + d.stage_ok.h * 0.5f);
  }
  for (int i = 0; i < PS_WAIT_MAX; i++) { ps_tick(); if (ccord_store_stage_instance(store, PS_CH_STAGE) == nullptr) break; }
  stage_ok = ccord_store_stage_instance(store, PS_CH_STAGE) == nullptr &&
             ps_find_hit("DELETE", "/stage-instances/3124") != nullptr;
  if (!stage_ok) { ps_fail("events-stage", "stage delete"); goto cleanup; }

  ps_settle();
  rc = 0;

cleanup:
  ps_servers_stop(&srv);
  free(p_hits);
  p_hits = nullptr;
  if (rc != 0) return 1;
  printf("scheduled_event_crud=%d\n", crud_ok ? 1 : 0);
  printf("scheduled_event_rsvp=%d\n", rsvp_ok ? 1 : 0);
  printf("scheduled_event_gateway_sync=%d\n", gw_sync_ok ? 1 : 0);
  printf("stage_instance_create_patch_delete=%d\n", stage_ok ? 1 : 0);
  return 0;
}
