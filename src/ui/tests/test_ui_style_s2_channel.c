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

// WP-S2b of the shared style pass: Channel Settings draws its controls through the ui_style.c helpers. The test opens
// Channel Settings on text channel 3111 (#general, C-Cord HQ) with the mock account made owner, visits every page the sidebar
// lists, and on each page resets the style audit, draws one frame and checks the control kinds that page draws (exact
// counts per kind) and that every record sits on a token radius, font and height. The Permissions page checks the fills
// of an active Inherit, deny and allow tri-state button. A source scan of channel_settings.c fails any rect or rounded
// rect draw that is not on an allow-list of non-control draws. Real store + mock transport + session + renderer.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/paths.h>
#include <ccord/model/permissions.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "../ui_style.h"

#include "ccord_test.h"

#define S2C_DATA_DIR "ui_style_s2c_data"
#define S2C_GUILD 2001u   // C-Cord HQ
#define S2C_CHANNEL 3111u // #general of C-Cord HQ, a text channel (3121 in this mock is a voice channel)

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

static void frame(int n)
{
  for (int i = 0; i < n; i++) {
    F.now += 50;
    (void)ccord_session_pump(F.session);
    (void)ccord_bus_drain(F.bus);
    (void)ccord_ui_update(F.ui, F.now);
    ccord_render_begin(F.renderer, F.theme.c.bg_primary);
    ccord_ui_draw(F.ui, F.renderer);
    ccord_render_end(F.renderer);
  }
}

static void send(ccord_input_event ev) { (void)ccord_ui_handle_input(F.ui, &ev); }

static void mouse(ccord_input_type t, uint8_t button, float x, float y)
{
  send((ccord_input_event){.type = t, .button = button, .clicks = 1, .x = x, .y = y});
}

static void click_rect(ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  frame(2);
}

// Clicks the centre of a named control of the Channel Settings overlay; false when it is not on screen.
static bool cs_click(const char *name)
{
  ccord_rect r;
  if (!ccord_ui_channel_settings_find(F.ui, name, &r)) return false;
  click_rect(r);
  return true;
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_s2_channel_test_data"));
remove(S2C_DATA_DIR "/settings.ini");
  remove(S2C_DATA_DIR "/drafts.json");
  remove(S2C_DATA_DIR);
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
  ccord_platform_config pc = {.title = "ui-style-s2c-test", .width = 1280, .height = 800, .headless = true};
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
  remove(S2C_DATA_DIR "/settings.ini");
  remove(S2C_DATA_DIR "/drafts.json");
  remove(S2C_DATA_DIR);
}

static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }

static bool same_color(ccord_color a, ccord_color b)
{
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// Draws one frame with a fresh audit and returns the audit of that frame.
static const uis_audit *audited_frame(void)
{
  frame(3);
  uis_audit_reset();
  frame(1);
  return uis_audit_get();
}

// Every record of the frame sits on a token: the radius of its kind, the font of its text and the height of its control.
// A record that draws no text (icon button, list row, toggle, popover, modal, surface) has no font: the audit reports 0.
static void check_record(const char *tag, const uis_audit_record *rec, const uis_tokens *t)
{
  bool ok = true;
  switch (rec->kind) {
    case UIS_K_BUTTON: {
      // The label is body (16) on a large button and control (14) on a small or medium one, as uis_button draws it.
      float want_fs = near(rec->rect.h, UIS_H_BTN_LG) ? t->fs_body : t->fs_control;
      ok = near(rec->radius, t->r_control) && near(rec->font_px, want_fs) &&
           (near(rec->rect.h, UIS_H_BTN_SM) || near(rec->rect.h, UIS_H_BTN) || near(rec->rect.h, UIS_H_BTN_LG));
      break;
    }
    case UIS_K_ICON_BUTTON:
      ok = near(rec->radius, t->r_control) && near(rec->rect.w, UIS_ICON_BTN) && near(rec->rect.h, UIS_ICON_BTN) &&
           near(rec->font_px, 0.0f);
      break;
    case UIS_K_LIST_ROW:
      ok = near(rec->radius, t->r_control) && near(rec->rect.h, t->list_row_h) && near(rec->font_px, 0.0f);
      break;
    case UIS_K_MENU_ROW:
      ok = near(rec->radius, t->r_control) && near(rec->font_px, t->fs_control) && near(rec->rect.h, t->menu_row_h);
      break;
    case UIS_K_CHIP:
      ok = near(rec->rect.h, UIS_H_CHIP) && near(rec->radius, rec->rect.h * 0.5f) && near(rec->font_px, t->fs_caption);
      break;
    case UIS_K_INPUT:
      ok = near(rec->radius, t->r_control) && near(rec->font_px, t->fs_body) && near(rec->rect.h, UIS_H_INPUT);
      break;
    case UIS_K_TOGGLE:
      ok = near(rec->rect.w, UIS_TOGGLE_W) && near(rec->rect.h, UIS_TOGGLE_H) && near(rec->radius, rec->rect.h * 0.5f) &&
           near(rec->font_px, 0.0f);
      break;
    case UIS_K_POPOVER:
    case UIS_K_MODAL:
    case UIS_K_SURFACE: ok = near(rec->radius, t->r_surface) && near(rec->font_px, 0.0f); break;
    default: ok = false; break;   // a kind Channel Settings never draws
  }
  if (!ok) {
    fprintf(stderr, "%s: %s not on a token (radius %.2f, font %.2f, w %.2f, h %.2f)\n", tag,
            uis_kind_name(rec->kind), (double)rec->radius, (double)rec->font_px, (double)rec->rect.w,
            (double)rec->rect.h);
  }
  CT_CHECK(ok);
}

// What the chat area behind the drawer draws (header icon buttons, composer surface). It is drawn before the drawer, so its
// records lead the frame's list; WP-C's test_ui_style_c checks them. The drawer's own counts are what is left.
static uint32_t g_base_count[UIS_K_COUNT];
static uint32_t g_base_kept;

// The page's expected draw count per control kind (every kind, zero where the page draws none), then each record.
static void check_page(const char *tag, const uint32_t want[UIS_K_COUNT])
{
  const uis_audit *a = audited_frame();
  uis_tokens t = uis_tokens_of(F.ui);
  CT_CHECK(a->total == a->kept);
  for (int k = 0; k < UIS_K_COUNT; k++) {
    if (a->kind[k].count != want[k] + g_base_count[k]) {
      fprintf(stderr, "%s: %s drawn %u time(s), want %u\n", tag, uis_kind_name((uis_kind)k), a->kind[k].count,
              want[k] + g_base_count[k]);
    }
    CT_CHECK(a->kind[k].count == want[k] + g_base_count[k]);
  }
  for (uint32_t i = g_base_kept; i < a->kept; i++) check_record(tag, &a->rec[i], &t);
}

// ---- the pages ----

// Overview: name and topic fields, the age-restricted toggle, 12 slowmode and 4 archive chips; no buttons while nothing is dirty.
static const uint32_t k_overview[UIS_K_COUNT] = {
  [UIS_K_ICON_BUTTON] = 1, [UIS_K_LIST_ROW] = 3, [UIS_K_MENU_ROW] = 1,
  [UIS_K_INPUT] = 2, [UIS_K_TOGGLE] = 1, [UIS_K_CHIP] = 16,
};

// Permissions: the add-overwrite button, and Inherit / deny / allow for each of the 10 permission rows that fit the window;
// one overwrite target chip (@everyone).
static const uint32_t k_permissions[UIS_K_COUNT] = {
  [UIS_K_ICON_BUTTON] = 1, [UIS_K_LIST_ROW] = 3, [UIS_K_MENU_ROW] = 1,
  [UIS_K_BUTTON] = 31, [UIS_K_CHIP] = 1,
};

// Invites: Refresh, Create Invite, and Copy / Revoke for each of the 2 invites; the expiry and max-uses chips.
static const uint32_t k_invites[UIS_K_COUNT] = {
  [UIS_K_ICON_BUTTON] = 1, [UIS_K_LIST_ROW] = 3, [UIS_K_MENU_ROW] = 1,
  [UIS_K_BUTTON] = 6, [UIS_K_CHIP] = 2,
};

static void test_channel_settings_pages(void)
{
  const ccord_channel *ch = ccord_store_channel(F.store, S2C_CHANNEL);
  CT_CHECK(ch != nullptr && ch->kind == CCORD_CHANNEL_TEXT);
  {
    const uis_audit *b = audited_frame();   // the chat alone, drawer closed
    g_base_kept = b->kept;
    for (int k = 0; k < UIS_K_COUNT; k++) g_base_count[k] = b->kind[k].count;
  }
  CT_CHECK(ccord_ui_open_channel_settings(F.ui, S2C_CHANNEL));
  frame(3);
  static const char *const k_pages[] = {"overview", "permissions", "invites"};
  static const uint32_t *const k_want[] = {k_overview, k_permissions, k_invites};
  for (size_t i = 0; i < sizeof k_pages / sizeof k_pages[0]; i++) {
    char name[64];
    snprintf(name, sizeof name, "page:%s", k_pages[i]);
    CT_CHECK(cs_click(name));   // the page opens: the sidebar entry is on screen and clicking it succeeds
    frame(3);
    ccord_ui_channel_settings_debug d;
    ccord_ui_get_channel_settings_debug(F.ui, &d);
    CT_CHECK(d.page == (int)i);
    check_page(name, k_want[i]);
  }
}

// ---- the tri-state permission buttons ----

// The drawn button whose rect is `r` in the frame's audit, or null when that control drew no button.
static const uis_audit_record *button_at(const uis_audit *a, ccord_rect r)
{
  for (uint32_t i = 0; i < a->kept; i++) {
    const uis_audit_record *rec = &a->rec[i];
    if (rec->kind == UIS_K_BUTTON && near(rec->rect.x, r.x) && near(rec->rect.y, r.y) && near(rec->rect.w, r.w)) {
      return rec;
    }
  }
  return nullptr;
}

// Moves the pointer off the window and lets the hover fade settle, so a button's fill is its resting fill.
static void leave_pointer(void)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, -20.0f, -20.0f);
  frame(20);
}

// The button drawn for the named tri-state control, from one audited frame.
static const uis_audit_record *tristate_record(const char *name)
{
  ccord_rect r;
  CT_CHECK(ccord_ui_channel_settings_find(F.ui, name, &r));
  const uis_audit_record *rec = button_at(audited_frame(), r);
  CT_CHECK(rec != nullptr);
  return rec;
}

// The Permissions page: the three tri-state buttons of one permission. An active Inherit is a selected secondary button
// (bg_item_selected), deny is the danger fill, allow the success fill; inactive ones are the plain secondary fill.
static void test_tristate_fills(void)
{
  CT_CHECK(cs_click("page:permissions"));
  frame(3);
  // @everyone has no stored overwrite, so every bit starts at Inherit.
  CT_CHECK(cs_click("ow_target:2001"));
  frame(3);
  size_t n = 0;
  const ccord_perm_info *tbl = ccord_perm_table(&n);
  CT_CHECK(n > 0);
  if (n == 0) return;
  uint64_t bit = tbl[0].bit;
  char inh[64], den[64], alw[64];
  snprintf(inh, sizeof inh, "ow_inherit:%llx", (unsigned long long)bit);
  snprintf(den, sizeof den, "ow_deny:%llx", (unsigned long long)bit);
  snprintf(alw, sizeof alw, "ow_allow:%llx", (unsigned long long)bit);
  const uis_tokens t = uis_tokens_of(F.ui);

  const uis_audit_record *rec = tristate_record(inh);
  CT_CHECK(same_color(rec->fill, F.theme.c.bg_item_selected));
  CT_CHECK(near(rec->radius, t.r_control));

  CT_CHECK(cs_click(den));
  leave_pointer();
  ccord_ui_channel_settings_debug d;
  ccord_ui_get_channel_settings_debug(F.ui, &d);
  CT_CHECK((d.work_deny & bit) != 0);
  rec = tristate_record(den);
  CT_CHECK(same_color(rec->fill, F.theme.c.danger));
  CT_CHECK(near(rec->radius, t.r_control));

  CT_CHECK(cs_click(alw));
  leave_pointer();
  ccord_ui_get_channel_settings_debug(F.ui, &d);
  CT_CHECK((d.work_allow & bit) != 0);
  rec = tristate_record(alw);
  CT_CHECK(same_color(rec->fill, F.theme.c.status_online));
  CT_CHECK(near(rec->radius, t.r_control));

  // Inherit is no longer the active choice: the plain secondary fill.
  rec = tristate_record(inh);
  CT_CHECK(same_color(rec->fill, F.theme.c.bg_tertiary));
}

// ---- chip hover ----

// The chip drawn for the hit rect `r`: its pill is centred vertically in the 28 px hit rect, at the same x and width.
static const uis_audit_record *chip_at(const uis_audit *a, ccord_rect r)
{
  for (uint32_t i = 0; i < a->kept; i++) {
    const uis_audit_record *rec = &a->rec[i];
    if (rec->kind == UIS_K_CHIP && near(rec->rect.x, r.x) && near(rec->rect.y, r.y + (r.h - UIS_H_CHIP) * 0.5f) &&
        near(rec->rect.w, r.w)) {
      return rec;
    }
  }
  return nullptr;
}

// The fill of the named chip from one settled frame, or from the single frame drawn after the audit reset.
static ccord_color chip_fill(const char *name, bool one_frame)
{
  ccord_rect r;
  CT_CHECK(ccord_ui_channel_settings_find(F.ui, name, &r));
  const uis_audit *a;
  if (one_frame) {
    uis_audit_reset();
    frame(1);
    a = uis_audit_get();
  } else {
    a = audited_frame();
  }
  const uis_audit_record *rec = chip_at(a, r);
  CT_CHECK(rec != nullptr);
  return rec ? rec->fill : F.theme.c.bg_tertiary;
}

// The Overview slowmode chips: an unselected chip eases to a hover fill, and under reduced motion that fill is reached
// in one frame. The current choice is brand, so the first chip whose rest fill is not brand is an unselected one.
static void test_chip_hover(void)
{
  CT_CHECK(cs_click("page:overview"));
  frame(3);
  static const uint32_t k_slow[] = {0, 5, 10, 15, 30, 60, 120, 300, 600, 900, 3600, 21600};
  char pick[32] = "";
  ccord_color rest = F.theme.c.brand;
  for (size_t i = 0; i < sizeof k_slow / sizeof k_slow[0] && pick[0] == '\0'; i++) {
    char nm[32];
    snprintf(nm, sizeof nm, "slowmode:%u", (unsigned)k_slow[i]);
    leave_pointer();
    ccord_color c = chip_fill(nm, false);
    if (!same_color(c, F.theme.c.brand)) {
      snprintf(pick, sizeof pick, "%s", nm);
      rest = c;
    }
  }
  CT_CHECK(pick[0] != '\0');
  if (pick[0] == '\0') return;

  ccord_rect r;
  CT_CHECK(ccord_ui_channel_settings_find(F.ui, pick, &r));
  leave_pointer();
  mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  ccord_color hover = chip_fill(pick, false);
  CT_CHECK(!same_color(hover, rest));

  // Reduced motion: leave, then hover, and the first frame already shows the hover fill.
  bool rm = F.ui->settings_reduced_motion, am = F.ui->anim.reduced_motion;
  F.ui->settings_reduced_motion = true;
  F.ui->anim.reduced_motion = true;
  leave_pointer();
  mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  CT_CHECK(same_color(chip_fill(pick, true), hover));
  F.ui->settings_reduced_motion = rm;
  F.ui->anim.reduced_motion = am;
  leave_pointer();
}

// ---- source scan: channel_settings.c draws its controls through the shared helpers ----

// The draws that are not controls, each matched by one whole source line and each present exactly once: the window and
// sidebar backdrops, and the card rows of the Overview tags and the Invites list.
static const char *const k_allowed_draws[] = {
  "ccord_draw_rect(r, (ccord_rect){0, 0, ww, wh}, COL(bg_primary));",
  "ccord_draw_rect(r, (ccord_rect){0, 0, side_w, wh}, COL(bg_secondary));",
  "ccord_draw_rrect(c->r, rr, uis_tokens_of(ui).r_control, COL(bg_secondary));",
  "ccord_draw_rrect(c->r, row, uis_tokens_of(ui).r_control, COL(bg_secondary));",
};

static bool source_path(const char *name, char *out, size_t n)
{
  const char *f = __FILE__;
  const char *t = strstr(f, "tests/test_ui_style_s2_channel.c");
  if (!t) return false;
  size_t dir = (size_t)(t - f);
  if (dir + strlen(name) + 1 > n) return false;
  memcpy(out, f, dir);
  snprintf(out + dir, n - dir, "%s", name);
  return true;
}

// Counts the lines of channel_settings.c that break the rule: a rect or rounded-rect draw that is not on the allow-list,
// a banned legacy helper, or an allow-list entry that no longer matches exactly once.
static int scan_source(const char *path)
{
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    fprintf(stderr, "cannot open %s\n", path);
    return 1;
  }
  static const char *const banned[] = {"ui_draw_switch(", "ui_state_fill(", "ui_row_state_fill(", "COL(divider)"};
  const size_t nallow = sizeof k_allowed_draws / sizeof k_allowed_draws[0];
  unsigned used[sizeof k_allowed_draws / sizeof k_allowed_draws[0]] = {0};
  char line[2048];
  int bad = 0, ln = 0;
  while (fgets(line, sizeof line, fp)) {
    ln++;
    for (size_t i = 0; i < sizeof banned / sizeof banned[0]; i++) {
      if (strstr(line, banned[i])) {
        fprintf(stderr, "%s:%d: %s bypasses the shared style helpers\n", path, ln, banned[i]);
        bad++;
      }
    }
    if (!strstr(line, "ccord_draw_rect(") && !strstr(line, "ccord_draw_rrect(")) continue;
    bool allowed = false;
    for (size_t i = 0; i < nallow && !allowed; i++) {
      if (strstr(line, k_allowed_draws[i])) {
        used[i]++;
        allowed = true;
      }
    }
    if (!allowed) {
      fprintf(stderr, "%s:%d: a raw rect draw is not a uis_ helper or an allow-listed backdrop\n", path, ln);
      bad++;
    }
  }
  fclose(fp);
  for (size_t i = 0; i < nallow; i++) {
    if (used[i] != 1) {
      fprintf(stderr, "%s: allow-list entry \"%s\" matches %u line(s), want 1\n", path, k_allowed_draws[i], used[i]);
      bad++;
    }
  }
  return bad;
}

static void test_source_draws_use_helpers(void)
{
  char path[1024];
  CT_CHECK(source_path("channel_settings.c", path, sizeof path));
  CT_CHECK(scan_source(path) == 0);
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_style_s2_channel")) return 77;
  test_source_draws_use_helpers();
  test_channel_settings_pages();
  test_chip_hover();
  test_tristate_fills();
  ui_channel_settings_close(F.ui);
  teardown();
  return CT_RESULT("test_ui_style_s2_channel");
}
