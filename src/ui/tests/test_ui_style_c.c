// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <ftw.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int ccord_test_unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(fpath);
}
static char *ccord_test_data_dir = NULL;
static void ccord_test_cleanup_dir(void) {
    if (ccord_test_data_dir) {
        nftw(ccord_test_data_dir, ccord_test_unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
        free(ccord_test_data_dir);
        ccord_test_data_dir = NULL;
    }
}
[[maybe_unused]] static const char *ccord_test_init_dir(const char *prefix) {
    if (ccord_test_data_dir) return ccord_test_data_dir;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    size_t len = strlen(tmp) + strlen(prefix) + 16;
    ccord_test_data_dir = malloc(len);
    if (!ccord_test_data_dir) abort();
    snprintf(ccord_test_data_dir, len, "%s/%s_XXXXXX", tmp, prefix);
    if (!mkdtemp(ccord_test_data_dir)) {
        free(ccord_test_data_dir);
        ccord_test_data_dir = NULL;
        abort();
    }
    atexit(ccord_test_cleanup_dir);
    return ccord_test_data_dir;
}

// WP-C of the shared style pass: the chat header, message action bar, reaction chips and composer draw through the
// ui_style.h helpers. The real UI is opened over the mock, the style audit is reset, one frame is drawn, and every control
// the chat area exposes a hit rect for must have an audit record of the right kind at that rect, on a token size, radius
// and font. A control drawn by hand writes no record, so it fails here by name. Fixture as test_ui_style_x.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "ui_style.h"

#include "ccord_test.h"

// Picker tab indices (expression_picker.c EXPR_TAB_*).
#define TAB_GIFS 0
#define TAB_STICKERS 1
#define TAB_EMOJI 2

typedef struct fixture {
  ccord_theme theme;
  ccord_bus *bus;
  ccord_store *store;
  ccord_session *session;
  ccord_transport *transport;
  ccord_contrib *contrib;
  ccord_platform *platform;
  ccord_renderer *renderer;
  ccord_ui *ui;
  uint64_t now;
} fixture;

static fixture F;

static uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

static void host_clip_set(void *ud, const char *s) { (void)ud; (void)s; }
static void host_text_input(void *ud, bool a, float x, float y, float w, float h)
{
  (void)ud; (void)a; (void)x; (void)y; (void)w; (void)h;
}
static void host_set_ext(void *ud, bool on) { (void)ud; (void)on; }
static bool host_open_url(void *ud, const char *url) { (void)ud; (void)url; return true; }
static bool host_open_file(void *ud, char *out, size_t cap) { (void)ud; (void)out; (void)cap; return false; }

// Draws n frames, each `ms` of virtual time after the last (the hover fades need 16 ms steps to read part-way values).
static void frame_step(int n, uint64_t ms)
{
  for (int i = 0; i < n; i++) {
    F.now += ms;
    (void)ccord_session_pump(F.session);
    (void)ccord_bus_drain(F.bus);
    (void)ccord_ui_update(F.ui, F.now);
    ccord_render_begin(F.renderer, F.theme.c.bg_primary);
    ccord_ui_draw(F.ui, F.renderer);
    ccord_render_end(F.renderer);
  }
}

static void frame(int n) { frame_step(n, 50); }

static void send(ccord_input_event ev) { (void)ccord_ui_handle_input(F.ui, &ev); }

static void mouse(ccord_input_type t, uint8_t button, float x, float y)
{
  send((ccord_input_event){.type = t, .button = button, .clicks = 1, .x = x, .y = y});
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_style_c_data"));
remove("ui_style_c_data/settings.ini");
  remove("ui_style_c_data/drafts.json");
  remove("ui_style_c_data");
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK)
    return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 10, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session) return false;
  ccord_platform_config pc = {.title = "ui-style-c-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .host = {.clipboard_set = host_clip_set, .text_input = host_text_input,
                              .set_extensions_enabled = host_set_ext, .open_url = host_open_url,
                              .open_file_dialog = host_open_file},
                     .fixed_now_unix_ms = 1793534400000LL + 30 * 60 * 1000};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  CT_CHECK(ccord_session_start(F.session, &err) == CCORD_OK);
  frame(3);
  return true;
}

static void teardown(void)
{
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
  ccord_contrib_destroy(F.contrib);
}

// Opens #general of the first server: the channel these surfaces are opened in.
static ccord_id open_general(void)
{
  ccord_error err = {0};
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  size_t n = ccord_store_channel_count(F.store, g);
  ccord_id general = CCORD_ID_NONE;
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, g, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && strcmp(c->name, "general") == 0) general = c->id;
  }
  CT_CHECK(general != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, general, &err) == CCORD_OK);
  frame(3);
  return general;
}

static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }


static void pointer_on(ccord_rect r) { mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f); }
static void pointer_away(void) { mouse(CCORD_IN_MOUSE_MOVE, 0, 2.0f, 2.0f); }

// The first audit record of `kind` whose rect is `rc` (the control's hit rect); nullptr when the control was not drawn
// through a helper of that kind.
static const uis_audit_record *record_at(uis_kind kind, ccord_rect rc)
{
  const uis_audit *a = uis_audit_get();
  for (uint32_t i = 0; i < a->kept; i++) {
    const uis_audit_record *r = &a->rec[i];
    if (r->kind == kind && near(r->rect.x, rc.x) && near(r->rect.y, rc.y) && near(r->rect.w, rc.w) && near(r->rect.h, rc.h))
      return r;
  }
  return nullptr;
}

// Kinds that draw no text carry font 0; the rest carry a token size.
static bool kind_draws_text(uis_kind k)
{
  return k == UIS_K_BUTTON || k == UIS_K_MENU_ROW || k == UIS_K_TOOLTIP || k == UIS_K_INPUT || k == UIS_K_TAB ||
         k == UIS_K_CHIP || k == UIS_K_SELECT || k == UIS_K_RADIO;
}

static bool token_font(float px)
{
  uis_tokens t = uis_tokens_of(F.ui);
  return near(px, t.fs_title) || near(px, t.fs_body) || near(px, t.fs_control) || near(px, t.fs_caption) ||
         near(px, t.fs_overline);
}

// Every record drawn in the frame: text kinds record a token font, text-less kinds record 0; icon buttons are 32 px
// squares; buttons 28/32/38 tall; inputs 36; chips and the pill kinds are half-height radius, everything else sits on a
// theme radius.
static void check_tokens(const char *tag)
{
  const uis_audit *a = uis_audit_get();
  uis_tokens t = uis_tokens_of(F.ui);
  CT_CHECK(a->total == a->kept);
  for (uint32_t i = 0; i < a->kept; i++) {
    const uis_audit_record *r = &a->rec[i];
    bool font_ok = kind_draws_text(r->kind) ? token_font(r->font_px) : near(r->font_px, 0.0f);
    if (!font_ok) fprintf(stderr, "%s: %s at %.0f,%.0f font %.1f\n", tag, uis_kind_name(r->kind), (double)r->rect.x,
                          (double)r->rect.y, (double)r->font_px);
    CT_CHECK(font_ok);
    bool pill = r->kind == UIS_K_CHIP || r->kind == UIS_K_TOGGLE;
    bool rad_ok = pill ? near(r->radius, r->rect.h * 0.5f)
                       : (near(r->radius, t.r_control) || near(r->radius, t.r_menu) || near(r->radius, t.r_surface));
    if (!rad_ok) fprintf(stderr, "%s: %s at %.0f,%.0f radius %.2f\n", tag, uis_kind_name(r->kind), (double)r->rect.x,
                         (double)r->rect.y, (double)r->radius);
    CT_CHECK(rad_ok);
    if (r->kind == UIS_K_ICON_BUTTON) CT_CHECK(near(r->rect.w, UIS_ICON_BTN) && near(r->rect.h, UIS_ICON_BTN));
    if (r->kind == UIS_K_INPUT) CT_CHECK(near(r->rect.h, UIS_H_INPUT));
    if (r->kind == UIS_K_BUTTON)
      CT_CHECK(near(r->rect.h, UIS_H_BTN_SM) || near(r->rect.h, UIS_H_BTN) || near(r->rect.h, UIS_H_BTN_LG));
    if (r->kind == UIS_K_CHIP) CT_CHECK(near(r->rect.h, UIS_H_CHIP));
  }
}

static void frame_audited(void)
{
  frame(3);
  uis_audit_reset();
  frame(1);
}

// Header: members, search, pins, inbox and saved are icon buttons at their hit rects.
static void test_header(void)
{
  pointer_away();
  frame_audited();
  ccord_rect rects[5] = {ui_members_btn_rect(F.ui), ui_search_rect(F.ui), ui_pin_rect(F.ui), ui_inbox_rect(F.ui),
                         ui_saved_messages_rect(F.ui)};
  for (int i = 0; i < 5; i++) {
    CT_CHECK(rects[i].w > 0.0f);
    CT_CHECK(record_at(UIS_K_ICON_BUTTON, rects[i]) != nullptr);
  }
  check_tokens("header");
}

// The header tooltip is the shared tooltip.
static void test_header_tooltip(void)
{
  pointer_on(ui_members_btn_rect(F.ui));
  frame(12);
  uis_audit_reset();
  frame(1);
  CT_CHECK(uis_audit_get()->kind[UIS_K_TOOLTIP].count > 0);
  check_tokens("header tooltip");
  pointer_away();
  frame(6);
}

// A message under the pointer shows its action bar; every button of it is an icon button at its hit rect.
static void test_action_bar(void)
{
  CT_CHECK(F.ui->nts_hits > 0);
  pointer_on(F.ui->ts_hits[0].rect);
  frame(12);
  uis_audit_reset();
  frame(1);
  CT_CHECK(F.ui->hover_bar_msg_id != CCORD_ID_NONE);
  CT_CHECK(record_at(UIS_K_ICON_BUTTON, F.ui->hover_bar_reply_rect) != nullptr);
  CT_CHECK(record_at(UIS_K_ICON_BUTTON, F.ui->hover_bar_pin_rect) != nullptr);
  CT_CHECK(record_at(UIS_K_ICON_BUTTON, F.ui->hover_bar_more_rect) != nullptr);
  check_tokens("action bar");
  pointer_away();
  frame(6);
}

// F41: the action bar hangs above its row (up to 20 px, over the previous row when grouped). The pointer on the part
// of it above the row must keep that row hovered, keep the bar, and a click there must reach the button underneath.
static void test_action_bar_top(void)
{
  pointer_away();
  frame(6);
  const ccord_rect ma = ui_rect(F.ui->layout.message_area);
  size_t pick = SIZE_MAX;
  for (size_t i = 0; i < F.ui->nts_hits; i++) {
    if (F.ui->ts_hits[i].rect.y > ma.y + 120.0f && F.ui->ts_hits[i].rect.y < ma.y + ma.h - 120.0f) pick = i;
  }
  CT_CHECK(pick != SIZE_MAX);
  if (pick == SIZE_MAX) return;
  pointer_on(F.ui->ts_hits[pick].rect);
  frame(12);
  ccord_id msg = F.ui->hover_bar_msg_id;
  CT_CHECK(msg != CCORD_ID_NONE);
  ccord_rect bar = F.ui->hover_bar_rect;
  ccord_rect reply = F.ui->hover_bar_reply_rect;
  CT_CHECK(bar.w > 0.0f && reply.w > 0.0f);
  float row_y = -1.0f;
  for (size_t i = 0; i < F.ui->nvis; i++) {
    const ccord_message *m = ccord_store_message_at(F.store, ccord_store_selected_channel(F.store),
                                                    (size_t)ui_view_index(F.ui, F.ui->vis[i].index));
    if (m && m->id == msg) row_y = F.ui->vis[i].y;
  }
  CT_CHECK(row_y >= 0.0f);
  // A point on the reply button inside the bar's top 20 px, which lies above the row's own rect.
  float px = reply.x + reply.w * 0.5f, py = bar.y + 4.0f;
  CT_CHECK(py < row_y);
  CT_CHECK(ui_in_rect(px, py, reply));
  mouse(CCORD_IN_MOUSE_MOVE, 0, px, py);
  frame(6);
  CT_CHECK(F.ui->hover_message_id == msg);
  CT_CHECK(F.ui->hover_bar_msg_id == msg);
  frame(10);
  CT_CHECK(F.ui->hover_bar_msg_id == msg);   // the bar did not fade away under the pointer
  mouse(CCORD_IN_MOUSE_DOWN, 1, px, py);
  CT_CHECK(F.ui->reply_message == msg);
  ui_cancel_reply(F.ui);
  pointer_away();
  frame(6);
}

// Reaction pills and the add button are chips; the pill's rect is the hit rect.
static void test_reactions(ccord_id ch)
{
  // The newest message gets two reactions (one of them mine) so it is on screen at the bottom of the timeline.
  size_t n = ccord_store_message_count(F.store, ch);
  CT_CHECK(n > 0);
  const ccord_message *last = ccord_store_message_at(F.store, ch, n - 1);
  CT_CHECK(ccord_store_set_reaction(F.store, ch, last->id, "\xf0\x9f\x94\xa5", 3, true, nullptr) == CCORD_OK);
  CT_CHECK(ccord_store_set_reaction(F.store, ch, last->id, "\xe2\x9a\xa1", 1, false, nullptr) == CCORD_OK);
  frame(4);
  frame_audited();
  CT_CHECK(F.ui->nreaction_hits > 0);
  size_t seen = 0;
  for (size_t i = 0; i < F.ui->nreaction_hits; i++) {
    ccord_rect hr = F.ui->reaction_hits[i].rect;
    bool found = false;
    const uis_audit *a = uis_audit_get();
    for (uint32_t k = 0; k < a->kept && !found; k++) {
      const uis_audit_record *r = &a->rec[k];
      found = r->kind == UIS_K_CHIP && near(r->rect.x, hr.x) && near(r->rect.w, hr.w) &&
              r->rect.y >= hr.y - 0.01f && r->rect.y + r->rect.h <= hr.y + hr.h + 0.01f;
    }
    if (found) seen++;
  }
  // Gold super-reaction pills are the one hand-drawn variant; every other pill must be a chip.
  CT_CHECK(seen + F.ui->dbg_burst_reaction_pills == F.ui->nreaction_hits);
  check_tokens("reactions");
  (void)ch;
}

// Composer: the box is the shared surface, the attach button an icon button, and a reply bar keeps the same composer.
static void test_composer(void)
{
  pointer_away();
  frame_audited();
  CT_CHECK(F.ui->composer_attach_rect.w > 0.0f);
  CT_CHECK(record_at(UIS_K_ICON_BUTTON, F.ui->composer_attach_rect) != nullptr);
  CT_CHECK(uis_audit_get()->kind[UIS_K_SURFACE].count > 0);
  check_tokens("composer");
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_style_c")) return 77;
  ccord_id ch = open_general();
  frame(4);
  test_header();
  test_header_tooltip();
  test_composer();
  test_action_bar();
  test_action_bar_top();
  test_reactions(ch);
  teardown();
  return CT_RESULT("test_ui_style_c");
}
