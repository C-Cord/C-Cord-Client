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

// WP-S2a check: every control on every Server Settings page is drawn through the shared style helpers (ui_style.h)
// with token values. Opens every sidebar page, Danger Zone included, for C-Cord HQ (2001): the mock account is made its
// owner with every permission, so no page is locked. Per page it resets the uis_audit table, draws a frame and checks
// each recorded control: radius, height and label size must be tokens, and the control kinds that page draws must have
// been recorded. A source scan of server_settings.c fails any ccord_draw_rect / ccord_draw_rrect line that is not on the
// named allow-list of non-control draws. Real store + mock transport + session + renderer, injected input events (same
// harness as test_ui_components_v2.c).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/alloc.h>
#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>
#include <ccord/store/store.h>
#include <ccord/platform/platform.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "../ui_style.h"

#include "ccord_test.h"

#define S2_DATA_DIR "ui_s2_test_data"

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

static void host_clip_set(void *ud, const char *s)
{
  (void)ud, (void)s;
}

static void host_text_input(void *ud, bool active, float x, float y, float w, float h)
{
  (void)ud, (void)active, (void)x, (void)y, (void)w, (void)h;
}

static bool host_open_url(void *ud, const char *url)
{
  (void)ud, (void)url;
  return true;
}

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

static void mouse(ccord_input_type t, uint8_t button, float x, float y)
{
  ccord_input_event ev = {.type = t, .button = button, .clicks = 1, .x = x, .y = y};
  (void)ccord_ui_handle_input(F.ui, &ev);
}

static void escape(void)
{
  ccord_input_event ev = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_ESCAPE};
  (void)ccord_ui_handle_input(F.ui, &ev);
  frame(2);
}

static void click(float x, float y)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  frame(2);
}

// Clicks the centre of a named control of the Server Settings overlay; false when it is not on screen.
static bool ss_click(const char *name)
{
  ccord_rect r;
  if (!ccord_ui_server_settings_find(F.ui, name, &r)) return false;
  click(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  return true;
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_s2_test_data"));
remove(S2_DATA_DIR "/settings.ini");
  remove(S2_DATA_DIR "/drafts.json");
  remove(S2_DATA_DIR);
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK) return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.transport || !F.session) return false;
  ccord_platform_config pc = {.title = "ui-s2-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .host = {.clipboard_set = host_clip_set, .text_input = host_text_input, .open_url = host_open_url},
                     .fixed_now_unix_ms = 1793534400000LL + 30 * 60 * 1000};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  if (ccord_session_start(F.session, &err) != CCORD_OK) return false;
  frame(3);
  return true;
}

static bool near(float a, float b)
{
  return fabsf(a - b) < 0.01f;
}

static bool is_one_of(float v, const float *set, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    if (near(v, set[i])) return true;
  }
  return false;
}

// Every kind is listed in each switch below with no default label, so a kind added to uis_kind fails the build (-Wswitch)
// until its radius, height and font rules are written here.
// The font a record must carry: the token size its kind draws its label in, or 0 for a kind that draws no text. A
// button's label is fs_body on the large size (UIS_H_BTN_LG) and fs_control otherwise; a chip is caption text. Returns
// -1 for a count, which is not a kind.
static float kind_font(const uis_tokens *t, const uis_audit_record *r)
{
  switch (r->kind) {
    case UIS_K_BUTTON: return near(r->rect.h, UIS_H_BTN_LG) ? t->fs_body : t->fs_control;
    case UIS_K_MENU_ROW: case UIS_K_TOOLTIP: case UIS_K_TAB: case UIS_K_RADIO: return t->fs_control;
    case UIS_K_INPUT: case UIS_K_SELECT: return t->fs_body;
    case UIS_K_CHIP: return t->fs_caption;
    case UIS_K_ICON_BUTTON: case UIS_K_LIST_ROW: case UIS_K_POPOVER: case UIS_K_MODAL: case UIS_K_SURFACE:
    case UIS_K_TOGGLE: case UIS_K_SWATCH: case UIS_K_VDIVIDER: case UIS_K_SLIDER: case UIS_K_CARD: case UIS_K_SCROLLBAR: case UIS_K_PLATE:
      return 0.0f;
    case UIS_K_COUNT:
      break;
  }
  return -1.0f;
}

// Checks one audit record against the tokens, in both directions: a text-bearing kind must record a token font
// size, a text-less kind must record font 0.
static void check_record(const uis_tokens *t, const uis_audit_record *r, const char *where)
{
  const uis_kind k = r->kind;
  const char *kn = uis_kind_name(k);
  bool rad_ok = false;
  switch (k) {
    // Controls on the control radius (theme channel_item_radius), a menu on the menu radius, surfaces on the surface radius.
    case UIS_K_BUTTON: case UIS_K_ICON_BUTTON: case UIS_K_MENU_ROW: case UIS_K_LIST_ROW: case UIS_K_INPUT:
    case UIS_K_TAB: case UIS_K_SELECT: case UIS_K_RADIO: case UIS_K_PLATE:
      rad_ok = near(r->radius, t->r_control);
      break;
    case UIS_K_TOOLTIP:
      rad_ok = near(r->radius, t->r_menu);
      break;
    case UIS_K_POPOVER: case UIS_K_MODAL: case UIS_K_SURFACE: case UIS_K_CARD:
      rad_ok = near(r->radius, t->r_surface);
      break;
    // Pills: the radius is half the token height (or width for the scrollbar track).
    case UIS_K_CHIP:       // a role pill
      rad_ok = near(r->radius, UIS_H_CHIP * 0.5f);
      break;
    case UIS_K_TOGGLE:     // the switch track
      rad_ok = near(r->radius, UIS_TOGGLE_H * 0.5f);
      break;
    case UIS_K_SLIDER:     // the slider track
      rad_ok = near(r->radius, UIS_SLIDER_H * 0.5f);
      break;
    case UIS_K_SCROLLBAR:  // a track as wide as it is
      rad_ok = near(r->radius, r->rect.w * 0.5f);
      break;
    case UIS_K_SWATCH:     // a circle
      rad_ok = near(r->radius, UIS_SWATCH_D * 0.5f);
      break;
    case UIS_K_VDIVIDER:   // a hairline
      rad_ok = near(r->radius, 0.0f);
      break;
    case UIS_K_COUNT:
      break;
  }
  if (!rad_ok) fprintf(stderr, "%s: %s radius %.2f is not a token\n", where, kn, (double)r->radius);
  CT_CHECK(rad_ok);

  // Both directions: a text kind must record exactly its token font (never 0), a text-less kind exactly 0.
  const float want_fs = kind_font(t, r);
  const bool fs_ok = want_fs >= 0.0f && near(r->font_px, want_fs);
  if (!fs_ok) fprintf(stderr, "%s: %s label %.2f px does not match its kind\n", where, kn, (double)r->font_px);
  CT_CHECK(fs_ok);

  bool h_ok = false;
  const float btn_h[] = {UIS_H_BTN_SM, UIS_H_BTN, UIS_H_BTN_LG};
  const float h = r->rect.h;
  switch (k) {
    case UIS_K_BUTTON: case UIS_K_RADIO: h_ok = is_one_of(h, btn_h, 3); break;
    case UIS_K_ICON_BUTTON: h_ok = near(h, UIS_ICON_BTN); break;
    case UIS_K_INPUT:
    case UIS_K_SELECT: h_ok = near(h, UIS_H_INPUT); break;
    case UIS_K_TOGGLE: h_ok = near(h, UIS_TOGGLE_H); break;
    case UIS_K_CHIP: h_ok = near(h, UIS_H_CHIP); break;
    case UIS_K_TAB: h_ok = near(h, UIS_H_BTN_SM); break;
    case UIS_K_SWATCH: h_ok = near(h, UIS_SWATCH_D); break;
    case UIS_K_SLIDER: h_ok = near(h, UIS_SLIDER_H); break;
    case UIS_K_MENU_ROW: h_ok = near(h, UIS_H_BTN); break;
    // Surfaces, rows, popovers, modals, tooltips, cards and dividers size to their content, so the token they must
    // follow is the spacing grid: a whole number of UIS_SP_1.
    case UIS_K_SURFACE: case UIS_K_LIST_ROW: case UIS_K_POPOVER: case UIS_K_MODAL: case UIS_K_TOOLTIP:
    case UIS_K_VDIVIDER: case UIS_K_CARD: case UIS_K_PLATE:
      h_ok = near(h, roundf(h / UIS_SP_1) * UIS_SP_1);
      break;
    // A scrollbar track spans the page, so its height has no token; its thickness (width) must sit on the grid.
    case UIS_K_SCROLLBAR:
      h_ok = h > 0.0f && near(r->rect.w, roundf(r->rect.w / UIS_SP_1) * UIS_SP_1);
      break;
    case UIS_K_COUNT:
      break;
  }
  if (!h_ok) fprintf(stderr, "%s: %s height %.2f is not a token\n", where, kn, (double)r->rect.h);
  CT_CHECK(h_ok);
}

// Resets the audit, draws one frame, checks every recorded draw in order and returns the per-kind counts of that frame.
static void audit_frame(const char *where, uint32_t counts[UIS_K_COUNT])
{
  frame(1);   // the first frame settles the layout; the audit covers the next single frame
  uis_audit_reset();
  frame(1);
  const uis_audit *a = uis_audit_get();
  uis_tokens t = uis_tokens_of(F.ui);
  uint32_t total = 0;
  for (int k = 0; k < UIS_K_COUNT; k++) {
    total += a->kind[k].count;
    counts[k] = a->kind[k].count;
  }
  CT_CHECK(total > 0);
  // rec[] holds the draws in order. Every draw must be recorded, so the walk below checks the whole frame.
  CT_CHECK(total <= UIS_AUDIT_MAX);
  CT_CHECK(a->kept == total);
  for (uint32_t i = 0; i < a->kept; i++) check_record(&t, &a->rec[i], where);
}

// ---- source scan: server_settings.c draws through the helpers ----

// The non-control rect draws that stay hand-drawn: the sidebar and page backgrounds, a usage meter, and the accent
// rules that head a section. Each line is matched by its text and must occur exactly `count` times.
typedef struct raw_allow {
  const char *text;
  int count;
  const char *why;
} raw_allow;

static const raw_allow k_raw_allow[] = {
  {"ccord_draw_rect(c->r, (ccord_rect){0.0f, 0.0f, g->sb_w, ui->win_h}, COL(bg_secondary));", 1,
   "sidebar background, full height, square"},
  {"ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h}, COL(bg_primary));", 1,
   "page background, full window"},
  {"ccord_draw_rrect(c->r, bar, SS_R_CTRL(c), COL(bg_tertiary));", 1, "usage meter track"},
  {"ccord_draw_rrect(c->r, (ccord_rect){bar.x, bar.y, fmaxf(8.0f, bar.w * frac), bar.h}, SS_R_CTRL(c),", 1,
   "usage meter fill"},
  {"ccord_draw_rrect(c->r, acc, acc.h * 0.5f, COL(danger));", 1, "Prune Members section accent rule"},
  {"ccord_draw_rrect(c->r, acc, acc.h * 0.5f, COL(brand));", 1, "AutoMod section accent rule"},
  {"ccord_draw_rect(c->r, (ccord_rect){row.x + 4.0f, ly + 2.0f, 3.0f, lh - 2.0f}, COL(brand));", 1,
   "audit diff accent rule"},
};

static bool source_path(const char *name, char *out, size_t n)
{
  const char *f = __FILE__;
  const char *t = strstr(f, "tests/test_ui_style_s2.c");
  if (!t) return false;
  size_t dir = (size_t)(t - f);
  if (dir + strlen(name) + 1 > n) return false;
  memcpy(out, f, dir);
  snprintf(out + dir, n - dir, "%s", name);
  return true;
}

// Counts the lines of server_settings.c that draw by hand outside the allow-list, and the allow-list entries whose
// count is wrong. Returns the number of problems found.
static int scan_source(const char *path)
{
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    fprintf(stderr, "cannot open %s\n", path);
    return 1;
  }
  static const char *const banned[] = {"ui_draw_switch(", "ui_state_fill(", "ui_row_state_fill(", "COL(divider)",
                                       "uis_frame("};
  int seen[sizeof k_raw_allow / sizeof k_raw_allow[0]] = {0};
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
    if (!strstr(line, "ccord_draw_rrect(") && !strstr(line, "ccord_draw_rect(")) continue;
    bool allowed = false;
    for (size_t i = 0; i < sizeof k_raw_allow / sizeof k_raw_allow[0]; i++) {
      if (strstr(line, k_raw_allow[i].text)) {
        seen[i]++;
        allowed = true;
        break;
      }
    }
    if (!allowed) {
      fprintf(stderr, "%s:%d: hand-drawn rect or rounded rect outside the allow-list; use a uis_ helper\n", path, ln);
      bad++;
    }
  }
  fclose(fp);
  for (size_t i = 0; i < sizeof k_raw_allow / sizeof k_raw_allow[0]; i++) {
    if (seen[i] != k_raw_allow[i].count) {
      fprintf(stderr, "%s: allow-list entry '%s' (%s) found %d times, expected %d\n", path, k_raw_allow[i].text,
              k_raw_allow[i].why, seen[i], k_raw_allow[i].count);
      bad++;
    }
  }
  return bad;
}

static void test_source_draws_use_helpers(void)
{
  char path[1024];
  CT_CHECK(source_path("server_settings.c", path, sizeof path));
  CT_CHECK(scan_source(path) == 0);
}

// ---- the pages ----

// The sidebar pages in enum order (ss_page in server_settings.c); the sidebar's names are "page:<name>".
static const char *const k_ss_pages[] = {
  "overview", "roles", "invites", "bans", "audit", "safety", "danger",
  "emoji", "stickers", "soundboard", "members", "automod", "integrations", "community"
};

// The control kinds each page draws, with the least count it must draw in one settled frame (the largest over its
// scroll range). A kind that goes missing, for example a radio row drawn without uis_radio, fails the page.
typedef struct page_need {
  int page;
  uis_kind kind;
  uint32_t min;
} page_need;

static const page_need k_needs[] = {
  {0, UIS_K_BUTTON, 5},  // overview button
  {0, UIS_K_ICON_BUTTON, 1},  // overview icon-button
  {0, UIS_K_LIST_ROW, 14},  // overview list-row
  {0, UIS_K_INPUT, 1},  // overview input
  {0, UIS_K_TOGGLE, 4},  // overview toggle
  {0, UIS_K_CHIP, 5},  // overview chip
  {0, UIS_K_SELECT, 2},  // overview select
  {0, UIS_K_RADIO, 2},  // overview radio
  {0, UIS_K_VDIVIDER, 1},  // overview divider
  {1, UIS_K_BUTTON, 2},  // roles button
  {1, UIS_K_ICON_BUTTON, 1},  // roles icon-button
  {1, UIS_K_LIST_ROW, 18},  // roles list-row
  {1, UIS_K_INPUT, 1},  // roles input
  {1, UIS_K_TAB, 3},  // roles tab
  {1, UIS_K_SURFACE, 1},  // roles surface
  {1, UIS_K_VDIVIDER, 1},  // roles divider
  {2, UIS_K_BUTTON, 8},  // invites button
  {2, UIS_K_ICON_BUTTON, 1},  // invites icon-button
  {2, UIS_K_LIST_ROW, 14},  // invites list-row
  {2, UIS_K_INPUT, 1},  // invites input
  {2, UIS_K_TOGGLE, 1},  // invites toggle
  {2, UIS_K_CHIP, 14},  // invites chip
  {2, UIS_K_SELECT, 1},  // invites select
  {2, UIS_K_VDIVIDER, 1},  // invites divider
  {3, UIS_K_BUTTON, 3},  // bans button
  {3, UIS_K_ICON_BUTTON, 1},  // bans icon-button
  {3, UIS_K_LIST_ROW, 14},  // bans list-row
  {3, UIS_K_INPUT, 1},  // bans input
  {3, UIS_K_VDIVIDER, 1},  // bans divider
  {4, UIS_K_BUTTON, 1},  // audit button
  {4, UIS_K_ICON_BUTTON, 1},  // audit icon-button
  {4, UIS_K_LIST_ROW, 16},  // audit list-row
  {4, UIS_K_TAB, 7},  // audit filter tab (selected filter is bold)
  {4, UIS_K_SELECT, 2},  // audit select
  {4, UIS_K_VDIVIDER, 1},  // audit divider
  {5, UIS_K_ICON_BUTTON, 1},  // safety icon-button
  {5, UIS_K_LIST_ROW, 22},  // safety list-row
  {5, UIS_K_CHIP, 8},  // safety chip
  {5, UIS_K_RADIO, 8},  // safety radio
  {5, UIS_K_VDIVIDER, 1},  // safety divider
  {6, UIS_K_BUTTON, 2},  // danger button
  {6, UIS_K_ICON_BUTTON, 1},  // danger icon-button
  {6, UIS_K_LIST_ROW, 14},  // danger list-row
  {6, UIS_K_SELECT, 1},  // danger select
  {6, UIS_K_VDIVIDER, 1},  // danger divider
  {7, UIS_K_BUTTON, 13},  // emoji button
  {7, UIS_K_ICON_BUTTON, 1},  // emoji icon-button
  {7, UIS_K_LIST_ROW, 14},  // emoji list-row
  {7, UIS_K_INPUT, 2},  // emoji input
  {7, UIS_K_VDIVIDER, 1},  // emoji divider
  {8, UIS_K_BUTTON, 11},  // stickers button
  {8, UIS_K_ICON_BUTTON, 1},  // stickers icon-button
  {8, UIS_K_LIST_ROW, 14},  // stickers list-row
  {8, UIS_K_INPUT, 4},  // stickers input
  {8, UIS_K_VDIVIDER, 1},  // stickers divider
  {9, UIS_K_BUTTON, 9},  // soundboard button
  {9, UIS_K_ICON_BUTTON, 1},  // soundboard icon-button
  {9, UIS_K_LIST_ROW, 14},  // soundboard list-row
  {9, UIS_K_INPUT, 3},  // soundboard input
  {9, UIS_K_CHIP, 4},  // soundboard chip
  {9, UIS_K_VDIVIDER, 1},  // soundboard divider
  {10, UIS_K_BUTTON, 65},  // members button
  {10, UIS_K_ICON_BUTTON, 1},  // members icon-button
  {10, UIS_K_LIST_ROW, 14},  // members list-row
  {10, UIS_K_INPUT, 1},  // members input
  {10, UIS_K_CHIP, 12},  // members chip
  {10, UIS_K_VDIVIDER, 1},  // members divider
  {11, UIS_K_BUTTON, 10},  // automod button
  {11, UIS_K_ICON_BUTTON, 1},  // automod icon-button
  {11, UIS_K_LIST_ROW, 14},  // automod list-row
  {11, UIS_K_TOGGLE, 2},  // automod toggle
  {11, UIS_K_VDIVIDER, 1},  // automod divider
  {12, UIS_K_BUTTON, 9},  // integrations button
  {12, UIS_K_ICON_BUTTON, 1},  // integrations icon-button
  {12, UIS_K_LIST_ROW, 14},  // integrations list-row
  {12, UIS_K_INPUT, 1},  // integrations input
  {12, UIS_K_SURFACE, 2},  // integrations surface
  {12, UIS_K_VDIVIDER, 1},  // integrations divider
  {13, UIS_K_BUTTON, 12},  // community button
  {13, UIS_K_ICON_BUTTON, 1},  // community icon-button
  {13, UIS_K_LIST_ROW, 14},  // community list-row
  {13, UIS_K_INPUT, 3},  // community input
  {13, UIS_K_TOGGLE, 2},  // community toggle
  {13, UIS_K_SURFACE, 3},  // community surface
  {13, UIS_K_CHIP, 10},  // community chip
  {13, UIS_K_VDIVIDER, 1},  // community divider
};

// Scrolls the open page to its end, auditing every position: rows below the first screen are only drawn (and only
// recorded) once they are on screen. Returns the largest count of each kind seen at any position. The page is audited
// at each position with every record checked, so a bad control anywhere on the page fails the test.
static void audit_page(const char *where, uint32_t best[UIS_K_COUNT])
{
  uint32_t counts[UIS_K_COUNT];
  memset(best, 0, sizeof(uint32_t) * UIS_K_COUNT);
  float last = -1.0f;
  bool at_end = false;
  for (int step = 0; step < 80; step++) {
    audit_frame(where, counts);
    for (int k = 0; k < UIS_K_COUNT; k++) {
      if (counts[k] > best[k]) best[k] = counts[k];
    }
    ccord_ui_server_settings_debug d;
    ccord_ui_get_server_settings_debug(F.ui, &d);
    if (d.scroll <= last + 0.5f) {   // the page does not move any further: it is at its end
      at_end = true;
      break;
    }
    last = d.scroll;
    ccord_input_event ev = {.type = CCORD_IN_WHEEL, .wheel = -4.0f, .x = 640.0f, .y = 400.0f};
    (void)ccord_ui_handle_input(F.ui, &ev);
    frame(2);
  }
  // A page that still moves after 80 wheel steps has an unaudited tail: fail rather than stop auditing early.
  if (!at_end) fprintf(stderr, "%s: still scrolling after 80 steps; the tail is not audited\n", where);
  CT_CHECK(at_end);
}

static void test_server_settings_pages(void)
{
  // C-Cord HQ (2001): the mock account becomes its owner with every permission (as test_ui_interaction does), because
  // the Danger Zone page is owner-only. Then every page the sidebar lists must open; a missing page fails the test.
  ccord_error err;
  const ccord_guild *hq = ccord_store_guild(F.store, 2001);
  CT_CHECK(hq != nullptr);
  if (!hq) return;
  ccord_guild own = *hq;
  own.owner_id = ccord_store_self_id(F.store);
  own.self_permissions = UINT64_MAX;
  CT_CHECK(ccord_store_upsert_guild(F.store, &own, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, 2001));
  frame(3);
  uint32_t per_page[sizeof k_ss_pages / sizeof k_ss_pages[0]][UIS_K_COUNT];
  for (size_t i = 0; i < sizeof k_ss_pages / sizeof k_ss_pages[0]; i++) {
    char name[64];
    snprintf(name, sizeof name, "page:%s", k_ss_pages[i]);
    CT_CHECK(ss_click(name));
    frame(3);
    ccord_ui_server_settings_debug d;
    ccord_ui_get_server_settings_debug(F.ui, &d);
    CT_CHECK(d.page == (int)i);   // the page the sidebar names is the one on screen
    audit_page(name, per_page[i]);
  }
  for (size_t n = 0; n < sizeof k_needs / sizeof k_needs[0]; n++) {
    const page_need *need = &k_needs[n];
    uint32_t got = per_page[need->page][need->kind];
    if (got < need->min) {
      fprintf(stderr, "page %s draws %s %u times, expected at least %u\n", k_ss_pages[need->page],
              uis_kind_name(need->kind), (unsigned)got, (unsigned)need->min);
    }
    CT_CHECK(got >= need->min);
  }
  escape();
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_style_s2")) {
    fprintf(stderr, "ui style s2 test: setup failed or no display driver; skipping\n");
    return 77;
  }
  test_source_draws_use_helpers();
  test_server_settings_pages();
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_contrib_destroy(F.contrib);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
  remove(S2_DATA_DIR "/settings.ini");
  remove(S2_DATA_DIR "/drafts.json");
  remove(S2_DATA_DIR);
  return CT_RESULT("test_ui_style_s2");
}
