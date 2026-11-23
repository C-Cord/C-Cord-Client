// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// End-to-end headless Components v2 verification (`ccord --headless --mock --verify-slice components-v2`).
//
// Drives the REAL app pipeline (live_transport + gateway JSON parser + session + store + ui + renderer) against a
// forked loopback Discord HTTP server and Main Gateway (tests/support/fake_discord.h + fake_gateway.h), like
// verify_messaging_events.c. The gateway delivers two MESSAGE_CREATE dispatches after the session is up:
//   - #general: a flag-32768 (IS_COMPONENTS_V2) message from a bot with a Container (accent bar) holding a
//     TextDisplay, a Section with a Thumbnail accessory, a Separator, a MediaGallery, a File and two ActionRows
//     (button, string select), plus a top-level TextDisplay and a Section with a Button accessory. The same message
//     also carries message-level attachments and an embed: a v2 message draws none of them on its own (the
//     attachments only feed the tree's attachment:// media).
//   - #off-topic: a legacy v1 message (content, attachment, embed, two action rows) that must keep rendering and
//     dispatching exactly as before.
// Every check asserts what the parser stored, what the renderer drew (UI debug counters) and what actually reached the
// fake Discord over HTTP, then prints `<name>=1` per check and `components-v2: PASS checks=6` when all six pass.
//
// Media note: the fixture's image URLs sit on a host outside the live transport's CDN allowlist
// (ccord_live_media_url_allowed), so the app refuses to fetch them immediately: the slice never touches the real
// network, and tiles draw as placeholders (the UI draw counters do not depend on decoded pixels).
//
// The comp_hits / dbg_cv2_* inspection seams are UI-module internals; the harness reads them the same way
// src/ui/tests/test_ui_components_v2.c does. All scratch files live in a mkdtemp dir under $TMPDIR.
#include "fake_discord.h"
#undef _POSIX_C_SOURCE
#undef _DEFAULT_SOURCE
#include "fake_gateway.h"

#include "verify_parity_slices.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ccord/core/paths.h>
#include <ccord/model/rich.h>
#include <ccord/net/live_transport.h>

#include "ui_internal.h"

#define CV_GUILD 2001ULL
#define CV_CH_GENERAL 3121ULL
#define CV_CH_OFFTOPIC 3122ULL
#define CV_MSG_V2 7001ULL
#define CV_MSG_LEGACY 7002ULL
#define CV_APP 888001ULL
#define CV_WAIT_MAX 1200                // pump ticks per wait loop (100 ms virtual + ~5 ms real each)
#define CV_MAX_HITS 256
#define CV_CHECKS 6

// Attachment / media URLs on a host the live transport will not fetch from (see the media note above).
#define CV_CDN "https://cdn.fixture.invalid"
#define CV_URL_SHOT CV_CDN "/attachments/3121/9101/shot.png"
#define CV_URL_PDF CV_CDN "/attachments/3121/9102/report.pdf"

static const ccord_verify_parity_ctx *c_ctx;
static fd_server *c_http_server;
static fd_hit *c_hits;
static size_t c_nhits;

// ---- fixtures ------------------------------------------------------------------------------------------------------

// The v2 message: flags 32768, no content, a tree of all seven v2 kinds, plus message-level attachments and an embed.
static const char k_cv_msg_v2[] =
  "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"s\":2,\"d\":{\"id\":\"7001\",\"channel_id\":\"3121\",\"guild_id\":\"2001\","
  "\"author\":{\"id\":\"8001\",\"username\":\"Cv2Bot\",\"bot\":true},\"application_id\":\"888001\","
  "\"content\":\"\",\"timestamp\":\"2026-11-15T12:00:00.000Z\",\"flags\":32768,"
  "\"attachments\":["
  "{\"id\":\"9101\",\"filename\":\"shot.png\",\"url\":\"" CV_URL_SHOT "\",\"content_type\":\"image/png\","
  "\"width\":320,\"height\":200,\"size\":4096},"
  "{\"id\":\"9102\",\"filename\":\"report.pdf\",\"url\":\"" CV_URL_PDF "\",\"content_type\":\"application/pdf\","
  "\"size\":2048}],"
  "\"embeds\":[{\"type\":\"rich\",\"title\":\"embed a v2 message must not draw\",\"description\":\"hidden\"}],"
  "\"components\":["
  "{\"type\":17,\"accent_color\":5793266,\"components\":["
  "{\"type\":10,\"content\":\"# Release notes\"},"
  "{\"type\":9,\"components\":[{\"type\":10,\"content\":\"Section **body** text\"}],"
  "\"accessory\":{\"type\":11,\"media\":{\"url\":\"" CV_CDN "/thumb.png\",\"width\":128,\"height\":128,"
  "\"content_type\":\"image/png\"},\"description\":\"thumb\"}},"
  "{\"type\":14,\"divider\":true,\"spacing\":2},"
  "{\"type\":12,\"items\":["
  "{\"media\":{\"url\":\"attachment://shot.png\"}},"
  "{\"media\":{\"url\":\"" CV_CDN "/g2.png\",\"width\":320,\"height\":200,\"content_type\":\"image/png\"}}]},"
  "{\"type\":13,\"file\":{\"url\":\"attachment://report.pdf\"},\"name\":\"report.pdf\",\"size\":2048},"
  "{\"type\":1,\"components\":[{\"type\":2,\"style\":1,\"custom_id\":\"v2_btn\",\"label\":\"Nested\"}]},"
  "{\"type\":1,\"components\":[{\"type\":3,\"custom_id\":\"v2_sel\",\"placeholder\":\"Pick one\","
  "\"min_values\":1,\"max_values\":1,\"options\":["
  "{\"label\":\"Alpha\",\"value\":\"v2_alpha\"},{\"label\":\"Beta\",\"value\":\"v2_beta\"}]}]}]},"
  "{\"type\":10,\"content\":\"outside the container\"},"
  "{\"type\":9,\"components\":[{\"type\":10,\"content\":\"button accessory\"}],"
  "\"accessory\":{\"type\":2,\"style\":3,\"custom_id\":\"v2_acc\",\"label\":\"Accessory\"}}"
  "]}}";

// A legacy v1 message: content, an attachment, an embed and two action rows (a button, a string select).
static const char k_cv_msg_legacy[] =
  "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"s\":3,\"d\":{\"id\":\"7002\",\"channel_id\":\"3122\",\"guild_id\":\"2001\","
  "\"author\":{\"id\":\"8001\",\"username\":\"Cv2Bot\",\"bot\":true},\"application_id\":\"888001\","
  "\"content\":\"legacy message\",\"timestamp\":\"2026-11-15T12:00:05.000Z\",\"flags\":0,"
  "\"attachments\":[{\"id\":\"9201\",\"filename\":\"legacy.png\",\"url\":\"" CV_CDN "/attachments/3122/9201/legacy.png\","
  "\"content_type\":\"image/png\",\"width\":64,\"height\":64,\"size\":512}],"
  "\"embeds\":[{\"type\":\"rich\",\"title\":\"legacy embed\",\"description\":\"still drawn\"}],"
  "\"components\":["
  "{\"type\":1,\"components\":[{\"type\":2,\"style\":1,\"custom_id\":\"legacy_btn\",\"label\":\"Legacy\"}]},"
  "{\"type\":1,\"components\":[{\"type\":3,\"custom_id\":\"legacy_sel\",\"placeholder\":\"Legacy pick\","
  "\"min_values\":1,\"max_values\":1,\"options\":["
  "{\"label\":\"One\",\"value\":\"legacy_one\"},{\"label\":\"Two\",\"value\":\"legacy_two\"}]}]}"
  "]}}";

// ---- small helpers -------------------------------------------------------------------------------------------------

static void cv_sleep_ms(unsigned ms)
{
  struct timespec ts = {.tv_sec = (time_t)(ms / 1000u), .tv_nsec = (long)(ms % 1000u) * 1000000L};
  nanosleep(&ts, nullptr);
}

static int cv_fail(const char *what)
{
  fprintf(stderr, "ccord: --verify-slice components-v2: FAILED %s\n", what);
  return 1;
}

// The fd child writes one fd_hit per request into a pipe the parent normally only drains at fd_stop. Drain it
// incrementally (records are <= PIPE_BUF, so writes are atomic) so the child can never block on a full pipe.
static void cv_drain_hits(void)
{
  if (!c_http_server) return;
  for (;;) {
    fd_hit tmp;
    size_t got = 0;
    while (got < sizeof tmp) {
      ssize_t n = read(c_http_server->pipe_rd, (char *)&tmp + got, sizeof tmp - got);
      if (n <= 0) break;
      got += (size_t)n;
    }
    if (got < sizeof tmp) break;
    if (c_nhits < CV_MAX_HITS) c_hits[c_nhits++] = tmp;
  }
}

// One virtual frame + a real-time breather so the forked servers can answer.
static void cv_tick(void)
{
  c_ctx->frames(c_ctx->ud, 1);
  cv_sleep_ms(5);
  cv_drain_hits();
}

static void cv_ticks(int n)
{
  for (int i = 0; i < n; i++) cv_tick();
}

static void cv_click(float x, float y)
{
  ccord_input_event ev = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  c_ctx->input(c_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  c_ctx->input(c_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y};
  c_ctx->input(c_ctx->ud, &ev);
  cv_ticks(2);
}

static void cv_click_rect(ccord_rect r)
{
  cv_click(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}

static ccord_ui_debug cv_dbg(void)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(c_ctx->ui, &d);
  return d;
}

// The message component the UI hit-tests under `custom_id` in the last drawn frame (v1 rows and the v2 tree alike).
static bool cv_hit_rect(const char *custom_id, ccord_rect *out)
{
  const ccord_ui *ui = c_ctx->ui;
  for (size_t i = 0; i < ui->ncomp_hits; i++) {
    if (strcmp(ui->comp_hits[i].custom_id, custom_id) != 0) continue;
    if (ui->comp_hits[i].rect.w <= 0.0f || ui->comp_hits[i].rect.h <= 0.0f) return false;
    *out = ui->comp_hits[i].rect;
    return true;
  }
  return false;
}

static bool cv_click_component(const char *custom_id)
{
  ccord_rect r;
  if (!cv_hit_rect(custom_id, &r)) return false;
  cv_click_rect(r);
  return true;
}

// How many POST /interactions requests reached the server whose body carries every fragment (NULL-terminated list).
static size_t cv_count_interactions(const char *const *frags)
{
  size_t n = 0;
  for (size_t i = 0; i < c_nhits; i++) {
    if (strcmp(c_hits[i].method, "POST") != 0 || strcmp(c_hits[i].path, "/interactions") != 0) continue;
    bool all = true;
    for (size_t f = 0; frags[f]; f++) all = all && strstr(c_hits[i].body, frags[f]) != nullptr;
    if (all) n++;
  }
  return n;
}

// Wait (up to CV_WAIT_MAX ticks) for at least one matching interaction; returns how many arrived.
static size_t cv_wait_interaction(const char *const *frags)
{
  size_t n = 0;
  for (int i = 0; i < CV_WAIT_MAX; i++) {
    cv_tick();
    n = cv_count_interactions(frags);
    if (n > 0) break;
  }
  cv_ticks(10);   // a duplicated dispatch would land within a few more ticks
  return cv_count_interactions(frags);
}

// Open the select `custom_id`, pick option `idx` (a single-choice select submits on the pick).
static bool cv_pick_select_option(const char *custom_id, size_t idx, size_t want_options)
{
  if (!cv_click_component(custom_id)) return false;
  ccord_ui_debug d = cv_dbg();
  if (!d.select_dropdown_open || d.select_dropdown_option_count != want_options || idx >= want_options) return false;
  cv_click_rect(d.select_option_rects[idx]);
  return !cv_dbg().select_dropdown_open;
}

static const ccord_message *cv_find_message(ccord_id ch, ccord_id id)
{
  return ccord_store_find_message(c_ctx->store, ch, id);
}

static void cv_rm_rf(const char *path)
{
  DIR *d = opendir(path);
  if (!d) {
    (void)unlink(path);
    return;
  }
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char child[1024];
    if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= (int)sizeof child) continue;
    if (e->d_type == DT_DIR) cv_rm_rf(child);
    else (void)unlink(child);
  }
  closedir(d);
  (void)rmdir(path);
}

static bool cv_make_scratch(char *out, size_t cap)
{
  const char *base = getenv("TMPDIR");
  if (!base || !base[0]) base = ".";
  int n = snprintf(out, cap, "%s/ccord-cv2-XXXXXX", base);
  if (n <= 0 || (size_t)n >= cap) return false;
  return mkdtemp(out) != nullptr;
}

// ---- fake HTTP server (runs in the forked child) -------------------------------------------------------------------

static size_t cv_respond(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)ud;
  (void)idx;
  // Channel history: empty (the messages under test arrive over the gateway).
  if (strcmp(req->method, "GET") == 0 && strstr(req->path, "/messages?limit=") != nullptr) {
    return fd_reply(out, cap, 200, "OK", nullptr, "[]");
  }
  if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/interactions") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  return fd_reply(out, cap, 204, "No Content", nullptr, "");
}

// ---- fake Main Gateway (runs in the forked child) ------------------------------------------------------------------

// READY, then answer every heartbeat. Once the session has been up for a few heartbeats (the app's heartbeat timer runs on
// the virtual clock, so this is paced by the parent's frames) deliver the v2 message, then the legacy one.
static void cv_script_gw(void *ud, fg_conn *c)
{
  (void)ud;
  if (!fg_handshake(c)) return;
  if (!fg_send(c, "{\"op\":10,\"d\":{\"heartbeat_interval\":500}}")) return;
  char buf[4096];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) return;
  if (strstr(buf, "\"op\":2") == nullptr) return;

  char ready[2048];
  (void)snprintf(ready, sizeof ready,
    "{\"op\":0,\"t\":\"READY\",\"s\":1,\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
    "\"session_id\":\"sess-1\",\"resume_gateway_url\":\"%s\","
    "\"guilds\":[{\"id\":\"2001\",\"name\":\"Cv2 HQ\",\"owner_id\":\"42\",\"channels\":["
    "{\"id\":\"3121\",\"type\":0,\"name\":\"general\",\"guild_id\":\"2001\"},"
    "{\"id\":\"3122\",\"type\":0,\"name\":\"off-topic\",\"guild_id\":\"2001\"}],"
    "\"members\":[{\"user\":{\"id\":\"42\",\"username\":\"me\"}}]}],"
    "\"private_channels\":[]}}",
    c->self_url);
  if (!fg_send(c, ready)) return;

  unsigned beats = 0;
  for (;;) {
    int r = fg_recv(c, buf, sizeof buf, 100);
    if (r == FG_TIMEOUT) continue;
    if (r != FG_GOT_TEXT) break;
    if (strstr(buf, "\"op\":1,") == nullptr && strstr(buf, "\"op\":1}") == nullptr) continue;
    if (!fg_send(c, "{\"op\":11}")) break;
    beats++;
    if (beats == 3 && !fg_send(c, k_cv_msg_v2)) break;
    if (beats == 4 && !fg_send(c, k_cv_msg_legacy)) break;
  }
}

// ---- server lifecycle ----------------------------------------------------------------------------------------------

typedef struct cv_servers {
  fd_server http;
  fg_server gw;
} cv_servers;

static bool cv_servers_start(cv_servers *s)
{
  memset(s, 0, sizeof *s);
  // SDL installs a SIGTERM handler (quit event); the forked fake servers would inherit it and survive
  // fg_stop/fd_stop's SIGTERM until their alarm fires. Fork with the default disposition, then restore.
  struct sigaction sa_dfl, sa_old;
  memset(&sa_dfl, 0, sizeof sa_dfl);
  sa_dfl.sa_handler = SIG_DFL;
  sigemptyset(&sa_dfl.sa_mask);
  (void)sigaction(SIGTERM, &sa_dfl, &sa_old);

  if (!fd_start(&s->http, cv_respond, nullptr)) {
    (void)sigaction(SIGTERM, &sa_old, nullptr);
    return false;
  }
  c_http_server = &s->http;
  int flags = fcntl(s->http.pipe_rd, F_GETFL, 0);
  if (flags >= 0) (void)fcntl(s->http.pipe_rd, F_SETFL, flags | O_NONBLOCK);

  if (!fg_start(&s->gw, cv_script_gw, nullptr)) {
    (void)fd_stop(&s->http, nullptr, 0);
    c_http_server = nullptr;
    (void)sigaction(SIGTERM, &sa_old, nullptr);
    return false;
  }
  (void)sigaction(SIGTERM, &sa_old, nullptr);
  return true;
}

static void cv_servers_stop(cv_servers *s)
{
  (void)fg_stop(&s->gw, nullptr, 0);
  cv_drain_hits();
  fd_hit tail[64];
  size_t nt = fd_stop(&s->http, tail, 64);
  for (size_t i = 0; i < nt && c_nhits < CV_MAX_HITS; i++) c_hits[c_nhits++] = tail[i];
  c_http_server = nullptr;
}

// Build the live transport pointed at the fake servers and hand it to the app session.
static ccord_status cv_connect_live(const cv_servers *s, ccord_error *err)
{
  ccord_live_config lcfg = {
    .token = "REDACTED_VERIFY_TOKEN",
    .api_base = s->http.base,
    .gateway_url = s->gw.url,
    .request_timeout_ms = 3000,
    .now_ms = c_ctx->now_ms,
    .clock_ud = c_ctx->ud,
  };
  ccord_transport *tr = ccord_live_transport_create(&lcfg, err);
  if (!tr) return err->code ? err->code : CCORD_ERR_IO;
  return c_ctx->set_transport(c_ctx->ud, tr, err);
}

static bool cv_wait_connected(void)
{
  for (int i = 0; i < CV_WAIT_MAX; i++) {
    cv_tick();
    if (ccord_session_state(c_ctx->session) == CCORD_CONN_CONNECTED &&
        ccord_store_channel(c_ctx->store, CV_CH_GENERAL) != nullptr &&
        ccord_store_channel(c_ctx->store, CV_CH_OFFTOPIC) != nullptr) {
      return true;
    }
  }
  return false;
}

static bool cv_wait_message(ccord_id ch, ccord_id id)
{
  for (int i = 0; i < CV_WAIT_MAX; i++) {
    cv_tick();
    if (cv_find_message(ch, id) != nullptr) return true;
  }
  return false;
}

// ---- checks --------------------------------------------------------------------------------------------------------

// A failed requirement names itself on stderr, so a red slice says which expectation broke.
#define CV_REQUIRE(cond)                                                                                         \
  do {                                                                                                           \
    if (!(cond)) {                                                                                               \
      fprintf(stderr, "ccord: --verify-slice components-v2: line %d: expected %s\n", __LINE__, #cond);            \
      return false;                                                                                              \
    }                                                                                                            \
  } while (0)

static void cv_count_kinds(const ccord_component *nodes, size_t count, size_t kinds[24])
{
  for (size_t i = 0; i < count; i++) {
    if ((size_t)nodes[i].kind < 24u) kinds[nodes[i].kind]++;
    if (nodes[i].children && nodes[i].child_count) cv_count_kinds(nodes[i].children, nodes[i].child_count, kinds);
    if (nodes[i].accessory) cv_count_kinds(nodes[i].accessory, 1, kinds);
  }
}

// cv2_parse: the gateway dispatch became the full v2 tree in the store (kinds, fields, custom_id lookup, attachment://).
static bool cv_check_parse(void)
{
  const ccord_message *m = cv_find_message(CV_CH_GENERAL, CV_MSG_V2);
  const ccord_message_rich *r = m ? m->rich : nullptr;
  CV_REQUIRE(r != nullptr);
  CV_REQUIRE((ccord_message_flags(m) & CCORD_MSGFLAG_IS_COMPONENTS_V2) != 0u);
  CV_REQUIRE((r->present & CCORD_RICH_HAS_COMPONENTS) != 0u && r->component_row_count == 0);
  CV_REQUIRE(r->v2_component_count == 3);
  // The message-level media stays in the model (the renderer is what must not draw it a second time).
  CV_REQUIRE(r->attachment_count == 2 && r->embed_count == 1 && r->application_id == CV_APP);

  const ccord_component *top = r->v2_components;
  CV_REQUIRE(top[0].kind == CCORD_COMP_CONTAINER && top[1].kind == CCORD_COMP_TEXT_DISPLAY &&
             top[2].kind == CCORD_COMP_SECTION);
  size_t kinds[24] = {0};
  cv_count_kinds(top, r->v2_component_count, kinds);
  CV_REQUIRE(kinds[CCORD_COMP_CONTAINER] == 1 && kinds[CCORD_COMP_SECTION] == 2 && kinds[CCORD_COMP_TEXT_DISPLAY] == 4);
  CV_REQUIRE(kinds[CCORD_COMP_THUMBNAIL] == 1 && kinds[CCORD_COMP_MEDIA_GALLERY] == 1 && kinds[CCORD_COMP_FILE] == 1);
  CV_REQUIRE(kinds[CCORD_COMP_SEPARATOR] == 1 && kinds[CCORD_COMP_ACTION_ROW] == 2);
  CV_REQUIRE(kinds[CCORD_COMP_BUTTON] == 2 && kinds[CCORD_COMP_STRING_SELECT] == 1);

  const ccord_component *cont = &top[0];
  CV_REQUIRE(cont->has_accent_color && cont->accent_color == 5793266u && !cont->spoiler && cont->child_count == 7);
  const ccord_component *sec = &cont->children[1];
  const ccord_component *sep = &cont->children[2];
  const ccord_component *gal = &cont->children[3];
  const ccord_component *file = &cont->children[4];
  CV_REQUIRE(sec->kind == CCORD_COMP_SECTION && sec->child_count == 1 && sec->accessory != nullptr &&
             sec->accessory->kind == CCORD_COMP_THUMBNAIL);
  CV_REQUIRE(sep->kind == CCORD_COMP_SEPARATOR && sep->divider && sep->spacing == 2);
  CV_REQUIRE(gal->kind == CCORD_COMP_MEDIA_GALLERY && gal->gallery_count == 2);
  CV_REQUIRE(strcmp(ccord_rich_media_url(r, &gal->gallery[0]), CV_URL_SHOT) == 0);   // attachment://shot.png
  CV_REQUIRE(strcmp(ccord_rich_media_url(r, &gal->gallery[1]), CV_CDN "/g2.png") == 0);
  CV_REQUIRE(file->kind == CCORD_COMP_FILE && strcmp(file->file_name, "report.pdf") == 0 && file->file_size == 2048);
  CV_REQUIRE(strcmp(ccord_rich_media_url(r, &file->media), CV_URL_PDF) == 0);        // attachment://report.pdf

  const ccord_component *btn = ccord_rich_find_component(r, "v2_btn");
  const ccord_component *sel = ccord_rich_find_component(r, "v2_sel");
  const ccord_component *acc = ccord_rich_find_component(r, "v2_acc");
  CV_REQUIRE(btn != nullptr && btn->kind == CCORD_COMP_BUTTON && strcmp(btn->label, "Nested") == 0);
  CV_REQUIRE(sel != nullptr && sel->kind == CCORD_COMP_STRING_SELECT && sel->option_count == 2);
  CV_REQUIRE(strcmp(sel->options[1].value, "v2_beta") == 0);
  CV_REQUIRE(acc != nullptr && acc->kind == CCORD_COMP_BUTTON && top[2].accessory == acc);
  return true;
}

// cv2_render: every v2 kind went through the renderer with a laid-out height, and the interactive parts drew.
static bool cv_check_render(void)
{
  ccord_ui_debug d = cv_dbg();
  const ccord_ui *ui = c_ctx->ui;
  const ccord_component_kind kinds[] = {CCORD_COMP_CONTAINER, CCORD_COMP_SECTION, CCORD_COMP_TEXT_DISPLAY,
                                        CCORD_COMP_THUMBNAIL, CCORD_COMP_MEDIA_GALLERY, CCORD_COMP_FILE,
                                        CCORD_COMP_SEPARATOR, CCORD_COMP_ACTION_ROW};
  for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
    CV_REQUIRE(ui->dbg_cv2_drawn[kinds[i]] >= 1u && ui->dbg_cv2_height[kinds[i]] > 0.0f);
  }
  // Container 1, Section 2, TextDisplay 4 (a Section's own text counts), Thumbnail 1, Separator 1 (spacing 2 = 16 px).
  CV_REQUIRE(ui->dbg_cv2_drawn[CCORD_COMP_CONTAINER] == 1u && ui->dbg_cv2_drawn[CCORD_COMP_SECTION] == 2u);
  CV_REQUIRE(ui->dbg_cv2_drawn[CCORD_COMP_TEXT_DISPLAY] == 4u && ui->dbg_cv2_drawn[CCORD_COMP_THUMBNAIL] == 1u);
  CV_REQUIRE(ui->dbg_cv2_drawn[CCORD_COMP_SEPARATOR] == 1u && ui->dbg_cv2_height[CCORD_COMP_SEPARATOR] == 16.0f);
  // Two action rows (button / select), the nested button plus the Section's accessory button, and one select.
  CV_REQUIRE(d.component_rows_drawn == 2 && d.component_buttons_drawn == 2 && d.component_selects_drawn == 1);
  ccord_rect r;
  CV_REQUIRE(cv_hit_rect("v2_btn", &r) && cv_hit_rect("v2_sel", &r) && cv_hit_rect("v2_acc", &r));
  return true;
}

// cv2_no_dup_attachments: the message's own attachments and embed are not drawn next to the tree that references them.
static bool cv_check_no_dup(void)
{
  ccord_ui_debug d = cv_dbg();
  const ccord_ui *ui = c_ctx->ui;
  // 2 gallery tiles + 1 file card are the only attachment cards (the message-level shot.png / report.pdf would make 5).
  CV_REQUIRE(d.gallery_cards == 1 && d.attachment_cards == 3);
  CV_REQUIRE(d.embed_cards == 0 && d.sticker_cards == 0 && d.poll_cards == 0 && d.reply_cards == 0);
  // Thumbnail + both gallery tiles are the only clickable images; the gallery resolved attachment://shot.png.
  CV_REQUIRE(ui->nimg_hits == 3 && ui->img_hits[1].count == 2);
  CV_REQUIRE(strcmp(ui->img_hits[1].urls[0], CV_URL_SHOT) == 0);
  return true;
}

// cv2_button_interaction: clicking the nested button and the Section accessory button POSTs /interactions with
// component_type 2, the custom_id and the v2 message flag.
static bool cv_check_button(void)
{
  const char *const nested[] = {"\"component_type\":2", "\"custom_id\":\"v2_btn\"", "\"message_flags\":32768,",
                                "\"message_id\":\"7001\"", "\"application_id\":\"888001\"", "\"channel_id\":\"3121\"", nullptr};
  const char *const accessory[] = {"\"component_type\":2", "\"custom_id\":\"v2_acc\"", "\"message_flags\":32768,",
                                   "\"message_id\":\"7001\"", "\"application_id\":\"888001\"", nullptr};
  CV_REQUIRE(cv_click_component("v2_btn"));
  CV_REQUIRE(cv_wait_interaction(nested) == 1);
  CV_REQUIRE(cv_click_component("v2_acc"));
  CV_REQUIRE(cv_wait_interaction(accessory) == 1);
  return true;
}

// cv2_select_interaction: the select nested in a Container > ActionRow opens its own options and submits the pick.
static bool cv_check_select(void)
{
  const char *const frags[] = {"\"component_type\":3", "\"custom_id\":\"v2_sel\"", "\"values\":[\"v2_beta\"]",
                               "\"message_flags\":32768,", "\"message_id\":\"7001\"", "\"application_id\":\"888001\"", nullptr};
  CV_REQUIRE(cv_pick_select_option("v2_sel", 1, 2));
  CV_REQUIRE(cv_wait_interaction(frags) == 1);
  return true;
}

// cv2_legacy_unchanged: a v1 message keeps its v1 rows, draws its content / attachment / embed and the v1 rows (no v2
// walker), and its button / select still dispatch with message_flags 0.
static bool cv_check_legacy(ccord_error *err)
{
  CV_REQUIRE(ccord_session_select_channel(c_ctx->session, CV_CH_OFFTOPIC, err) == CCORD_OK);
  CV_REQUIRE(cv_wait_message(CV_CH_OFFTOPIC, CV_MSG_LEGACY));
  cv_ticks(6);

  const ccord_message *m = cv_find_message(CV_CH_OFFTOPIC, CV_MSG_LEGACY);
  const ccord_message_rich *r = m ? m->rich : nullptr;
  CV_REQUIRE(r != nullptr && strcmp(m->content, "legacy message") == 0);
  CV_REQUIRE((ccord_message_flags(m) & CCORD_MSGFLAG_IS_COMPONENTS_V2) == 0u && r->v2_component_count == 0);
  CV_REQUIRE(r->component_row_count == 2);
  const ccord_component *btn = ccord_rich_find_component(r, "legacy_btn");
  const ccord_component *sel = ccord_rich_find_component(r, "legacy_sel");
  CV_REQUIRE(btn != nullptr && btn->kind == CCORD_COMP_BUTTON);
  CV_REQUIRE(sel != nullptr && sel->kind == CCORD_COMP_STRING_SELECT && sel->option_count == 2);

  ccord_ui_debug d = cv_dbg();
  const ccord_ui *ui = c_ctx->ui;
  // The v1 path draws the message's own attachment (an image: a one-tile gallery part) and its embed.
  CV_REQUIRE(d.attachment_cards == 1 && d.gallery_cards == 1 && d.embed_cards == 1);
  CV_REQUIRE(d.component_rows_drawn == 2 && d.component_buttons_drawn == 1 && d.component_selects_drawn == 1);
  for (size_t k = 0; k < UI_CV2_KIND_SLOTS; k++) {
    CV_REQUIRE(ui->dbg_cv2_drawn[k] == 0u);   // the v2 walker never runs for a v1 message
  }

  const char *const bfrags[] = {"\"component_type\":2", "\"custom_id\":\"legacy_btn\"", "\"message_flags\":0,",
                                "\"message_id\":\"7002\"", "\"application_id\":\"888001\"", nullptr};
  CV_REQUIRE(cv_click_component("legacy_btn"));
  CV_REQUIRE(cv_wait_interaction(bfrags) == 1);
  const char *const sfrags[] = {"\"component_type\":3", "\"custom_id\":\"legacy_sel\"", "\"values\":[\"legacy_two\"]",
                                "\"message_flags\":0,", "\"message_id\":\"7002\"", nullptr};
  CV_REQUIRE(cv_pick_select_option("legacy_sel", 1, 2));
  CV_REQUIRE(cv_wait_interaction(sfrags) == 1);
  return true;
}

// ---- slice entry ---------------------------------------------------------------------------------------------------

int ccord_verify_components_v2_slice(const ccord_verify_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->frames || !ctx->now_ms || !ctx->input ||
      !ctx->set_transport) {
    return cv_fail("invalid context");
  }
  c_ctx = ctx;
  c_hits = calloc(CV_MAX_HITS, sizeof *c_hits);
  if (!c_hits) return cv_fail("out of memory");
  c_nhits = 0;
  ccord_error err = {0};

  // The UI persists settings / drafts under the data dir: point it at a scratch dir.
  char scratch[512];
  if (!cv_make_scratch(scratch, sizeof scratch)) {
    free(c_hits);
    c_hits = nullptr;
    return cv_fail("mkdtemp scratch dir");
  }
  char orig_data_dir[512];
  (void)snprintf(orig_data_dir, sizeof orig_data_dir, "%s", ccord_paths_data_dir());
  ccord_paths_set_data_dir(scratch);

  cv_servers srv;
  if (!cv_servers_start(&srv)) {
    free(c_hits);
    c_hits = nullptr;
    ccord_paths_set_data_dir(orig_data_dir);
    cv_rm_rf(scratch);
    return cv_fail("start fake servers");
  }

  int rc = 1;
  bool parse_ok = false, render_ok = false, nodup_ok = false, button_ok = false, select_ok = false, legacy_ok = false;

  if (cv_connect_live(&srv, &err) != CCORD_OK) { (void)cv_fail(err.message); goto cleanup; }
  if (!cv_wait_connected()) { (void)cv_fail("gateway did not connect"); goto cleanup; }
  if (ccord_session_select_guild(ctx->session, CV_GUILD, &err) != CCORD_OK ||
      ccord_session_select_channel(ctx->session, CV_CH_GENERAL, &err) != CCORD_OK) {
    (void)cv_fail(err.message);
    goto cleanup;
  }
  if (!cv_wait_message(CV_CH_GENERAL, CV_MSG_V2)) { (void)cv_fail("v2 MESSAGE_CREATE never reached the store"); goto cleanup; }
  cv_ticks(8);   // layout + draw settle

  parse_ok = cv_check_parse();
  render_ok = cv_check_render();
  nodup_ok = cv_check_no_dup();
  button_ok = cv_check_button();
  select_ok = cv_check_select();
  legacy_ok = cv_check_legacy(&err);
  cv_ticks(40);   // let in-flight REST jobs finish before teardown so the queue destroy does not log spurious failures
  rc = 0;

cleanup:
  cv_servers_stop(&srv);
  ccord_paths_set_data_dir(orig_data_dir);
  cv_rm_rf(scratch);
  if (rc != 0) {
    free(c_hits);
    c_hits = nullptr;
    return 1;
  }

  const bool results[CV_CHECKS] = {parse_ok, render_ok, nodup_ok, button_ok, select_ok, legacy_ok};
  const char *const names[CV_CHECKS] = {"cv2_parse", "cv2_render", "cv2_no_dup_attachments", "cv2_button_interaction",
                                        "cv2_select_interaction", "cv2_legacy_unchanged"};
  int passed = 0;
  for (int i = 0; i < CV_CHECKS; i++) {
    printf("%s=%d\n", names[i], results[i] ? 1 : 0);
    passed += results[i] ? 1 : 0;
  }
  free(c_hits);
  c_hits = nullptr;
  if (passed != CV_CHECKS) {
    fprintf(stderr, "ccord: --verify-slice components-v2: FAILED %d of %d checks\n", CV_CHECKS - passed, CV_CHECKS);
    return 1;
  }
  printf("components-v2: PASS checks=%d\n", CV_CHECKS);
  return 0;
}
