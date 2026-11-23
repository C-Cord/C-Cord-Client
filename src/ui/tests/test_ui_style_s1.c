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

// WP-S1 (ui-style): every Settings page draws its buttons, inputs, toggles, chips, selects and sliders
// through the shared helpers in ui_style.h. Each page is opened on the real UI with a mock session,
// the audit is reset, and only the Settings page is drawn (ui_settings_draw, so the chat underneath
// cannot count). The audit must show the page's controls: fixed pages must match their count exactly,
// and a helper draw that goes missing fails. Every recorded radius, control height and font size must be
// a token value from ui_style.h. The My Account footer buttons are checked in each account state, and the
// signed-out login screen of My Account is drawn on its own UI. A source scan of the Settings files then
// fails on any raw ccord_draw_rrect outside the named chrome functions below. The scan does not see
// ccord_draw_rect or ccord_draw_circle, so a control drawn with those primitives would not be caught.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/event_bus.h>
#include <ccord/core/paths.h>
#include <ccord/model/contrib.h>
#include <ccord/net/admin.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "ui_style.h"

#include "ccord_test.h"

#define DATA_DIR "ui_style_s1_data"

// Mirrors `enum settings_page` in settings.c (that enum is file-local): same order, same count. TP_RETIRED_1 is the
// removed Profiles page's slot: it has no page, so it has no k_expect row.
enum {
  TP_ACCOUNT, TP_RETIRED_1, TP_APPEARANCE, TP_VOICE, TP_STREAMING, TP_TEXT_IMAGES, TP_NOTIFICATIONS, TP_KEYBINDS,
  TP_PLUGINS, TP_PRIVACY, TP_DEVICES, TP_CONNECTIONS, TP_ACCESSIBILITY, TP_STREAMER, TP_LANGUAGE, TP_ADVANCED,
  TP_CCORD,
  TP_COUNT
};

// Helper draws of each kind a page records in one draw. Each count is exact: a control added or dropped on a page
// fails the test by name, so a raw control added beside the helper ones cannot pass unnoticed.
typedef struct page_expect {
  const char *name;
  int page;
  uint32_t buttons, inputs, toggles, chips, selects, sliders;
} page_expect;

static const page_expect k_expect[] = {
  // Signed out with no sign-in configured: the account page without a footer button (k_footer_* cover the footer).
  {"My Account", TP_ACCOUNT, 10, 3, 0, 0, 0, 0},
  {"Appearance", TP_APPEARANCE, 3, 1, 3, 6, 0, 1},
  {"Voice & Video", TP_VOICE, 6, 0, 4, 2, 4, 3},
  {"Streaming", TP_STREAMING, 0, 0, 0, 21, 0, 0},
  {"Text & Images", TP_TEXT_IMAGES, 0, 0, 2, 3, 0, 0},
  {"Notifications", TP_NOTIFICATIONS, 1, 0, 5, 4, 0, 0},
  {"Keybinds", TP_KEYBINDS, 5, 0, 0, 0, 0, 0},   // one key-cap button per keybind row: five rows
  {"Plugins", TP_PLUGINS, 1, 0, 1, 0, 0, 0},   // Sync button (sub-tabs & filter chips use uis_tab) + Extensions toggle
  // Privacy & Safety: 6 toggles (DM, 3 friend sources, age, share profile updates), 6 chips (3 spam filter + 3 profile
  // privacy), and 1 button (Request My Data).
  {"Privacy", TP_PRIVACY, 1, 0, 6, 6, 0, 0},
  // Seeded by the mock on sign-in: two sessions (one is this device, so it has no Log Out), so the footer and one
  // Log Out button; the list is fetched before the audited draw (test_page).
  {"Devices", TP_DEVICES, 2, 0, 0, 0, 0, 0},
  // Seeded by the mock: three connections, each with a Disconnect button and three toggles, plus the footer button.
  {"Connections", TP_CONNECTIONS, 4, 0, 9, 0, 0, 0},
  {"Accessibility", TP_ACCESSIBILITY, 0, 0, 4, 8, 0, 0},
  {"Streamer Mode", TP_STREAMER, 0, 0, 6, 0, 0, 0},
  {"Language", TP_LANGUAGE, 0, 0, 0, 3, 1, 0},
  {"Advanced", TP_ADVANCED, 0, 0, 3, 0, 0, 0},
  // Jump-threshold stepper (two buttons + slider), game detection, tray badge, suppress-when-focused.
  {"C-Cord", TP_CCORD, 2, 0, 4, 0, 0, 1},
};

// Signed out, both login tabs + secondary 2FA popup: the Token tab (default) draws two tab buttons, Paste and Save &
// Connect, and the token input; the Email & Password tab draws two tab buttons, Connect, and email + password inputs;
// the 2FA popup adds the 6-digit code input plus Verify and Back buttons on top.
static const page_expect k_login_token = {"Login (token tab)", TP_ACCOUNT, 4, 1, 0, 0, 0, 0};
static const page_expect k_login_creds = {"Login (email tab)", TP_ACCOUNT, 3, 2, 0, 0, 0, 0};
static const page_expect k_login_mfa = {"Login (2FA popup)", TP_ACCOUNT, 5, 3, 0, 0, 0, 0};

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
static void host_apply_theme(void *ud, const char *id) { (void)ud; (void)id; }
static void host_set_zoom(void *ud, float z) { (void)ud; (void)z; }
static uint32_t host_caps(void *ud)
{
  (void)ud;
  return CCORD_UI_CAP_AUDIO_PLAYBACK | CCORD_UI_CAP_NOTIFICATIONS | CCORD_UI_CAP_TRAY | CCORD_UI_CAP_GLOBAL_SHORTCUTS;
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

// One draw of the Settings page alone: the audit then holds only what the page drew.
static void draw_settings_page(void)
{
  F.now += 50;
  (void)ccord_ui_update(F.ui, F.now);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ui_settings_draw(F.ui, F.renderer);
  ccord_render_end(F.renderer);
}

// Setup codes: SETUP_OK, SETUP_FAIL (a broken theme, bus, renderer or UI fails the test) and SETUP_SKIP (no
// headless platform can be created on this machine: ctest reports 77, Skipped).
enum { SETUP_OK = 0, SETUP_FAIL = 1, SETUP_SKIP = 2 };

// signed_in: start the mock session and let its snapshot land (the account page then shows the account).
// Otherwise the session stays stopped, the store has no self id, and Settings opens on the login screen.
static int setup(const char *argv0, bool signed_in)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return SETUP_FAIL;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_s1_test_data"));
remove(DATA_DIR "/settings.ini");
  remove(DATA_DIR "/drafts.json");
  remove(DATA_DIR);
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK)
    return SETUP_FAIL;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return SETUP_FAIL;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 1, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session || !F.transport) return SETUP_FAIL;
  ccord_platform_config pc = {.title = "ui-style-s1-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return SETUP_SKIP;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return SETUP_FAIL;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .host = {.clipboard_set = host_clip_set, .apply_theme = host_apply_theme,
                              .set_zoom = host_set_zoom, .desktop_capabilities = host_caps},
                     .fixed_now_unix_ms = 1793534400000LL};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return SETUP_FAIL;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  if (signed_in) {
    if (ccord_session_start(F.session, &err) != CCORD_OK) return SETUP_FAIL;
    frame(6);
  }
  return SETUP_OK;
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
  F.ui = nullptr;
  F.session = nullptr;
  F.transport = nullptr;
}

static bool near(float a, float b) { return a - b < 0.01f && b - a < 0.01f; }

// A radius is a theme token (control, surface or menu radius) for every kind. Only the kinds that are pills by design
// may use a pill radius: a chip, toggle or slider (half its height), a swatch (half its shorter side) and a scrollbar
// track (half its width). A button, input or card with a pill radius fails.
static bool radius_is_token(uis_kind k, float rad, const uis_tokens *t, ccord_rect rc)
{
  if (near(rad, t->r_control) || near(rad, t->r_surface) || near(rad, t->r_menu)) return true;
  switch (k) {
    case UIS_K_CHIP:
    case UIS_K_TOGGLE:
    case UIS_K_SLIDER: return near(rad, rc.h * 0.5f);
    case UIS_K_SWATCH: return near(rad, (rc.w < rc.h ? rc.w : rc.h) * 0.5f);
    case UIS_K_SCROLLBAR: return near(rad, rc.w * 0.5f);
    default: return false;
  }
}

// Kinds whose helper draws a text label, so every record must carry a token font size. The other kinds draw no
// text and must record font 0.
static bool kind_draws_text(uis_kind k)
{
  switch (k) {
    case UIS_K_BUTTON:
    case UIS_K_MENU_ROW:
    case UIS_K_TOOLTIP:
    case UIS_K_INPUT:
    case UIS_K_TAB:
    case UIS_K_CHIP:
    case UIS_K_SELECT:
    case UIS_K_RADIO: return true;
    default: return false;
  }
}

static bool font_is_token(float px, const uis_tokens *t)
{
  return near(px, t->fs_title) || near(px, t->fs_body) || near(px, t->fs_control) || near(px, t->fs_caption) ||
         near(px, t->fs_overline);
}

// The control heights each helper kind may use (ui_style.h UIS_H_* tokens).
static bool height_is_token(uis_kind k, float h)
{
  switch (k) {
    case UIS_K_BUTTON:
    case UIS_K_ICON_BUTTON:   // square icon buttons, and the scrollbar arrows (a token height of their own)
      return near(h, UIS_H_BTN_SM) || near(h, UIS_H_BTN) || near(h, UIS_H_BTN_LG) || near(h, UIS_SCROLL_ARROW_H);
    case UIS_K_INPUT: return near(h, UIS_H_INPUT);
    case UIS_K_TOGGLE: return near(h, UIS_TOGGLE_H);
    case UIS_K_CHIP: return near(h, UIS_H_CHIP);
    case UIS_K_SELECT: return near(h, UIS_H_BTN_SM);
    case UIS_K_SLIDER: return near(h, UIS_SLIDER_H);
    case UIS_K_LIST_ROW: return near(h, UIS_H_BTN);
    default: return true;   // cards, popovers and scrollbar tracks have no fixed control height
  }
}

// Checks every draw the page recorded (rec[0 .. kept)), not only the last draw of each kind. A draw past the record
// table would go unchecked, so the page must fit in it.
static void check_tokens(const char *name)
{
  const uis_audit *a = uis_audit_get();
  uis_tokens t = uis_tokens_of(F.ui);
  if (a->total > UIS_AUDIT_MAX || a->kept != a->total) {
    fprintf(stderr, "%s: %u draws, only %u recorded (the record table is %d)\n", name, a->total, a->kept,
            UIS_AUDIT_MAX);
    CT_CHECK(0);
  }
  for (uint32_t i = 0; i < a->kept; i++) {
    const uis_audit_record *rec = &a->rec[i];
    const char *kn = uis_kind_name(rec->kind);
    if (!radius_is_token(rec->kind, rec->radius, &t, rec->rect)) {
      fprintf(stderr, "%s: %s radius %.2f is not a token\n", name, kn, (double)rec->radius);
      CT_CHECK(0);
    }
    if (!height_is_token(rec->kind, rec->rect.h)) {
      fprintf(stderr, "%s: %s height %.2f is not a token\n", name, kn, (double)rec->rect.h);
      CT_CHECK(0);
    }
    if (kind_draws_text(rec->kind)) {
      if (!font_is_token(rec->font_px, &t)) {
        fprintf(stderr, "%s: %s font %.2f is not a token\n", name, kn, (double)rec->font_px);
        CT_CHECK(0);
      }
    } else if (rec->font_px != 0.0f) {
      fprintf(stderr, "%s: %s draws no text but records font %.2f\n", name, kn, (double)rec->font_px);
      CT_CHECK(0);
    }
  }
}

static void check_count(const char *name, uis_kind k, uint32_t want)
{
  uint32_t got = uis_audit_get()->kind[k].count;
  if (got != want) {
    fprintf(stderr, "%s: %s draws %u, expected exactly %u (a control added or dropped on this page)\n", name,
            uis_kind_name(k), got, want);
    CT_CHECK(0);
  }
}

static void check_page(const page_expect *e)
{
  check_count(e->name, UIS_K_BUTTON, e->buttons);
  check_count(e->name, UIS_K_INPUT, e->inputs);
  check_count(e->name, UIS_K_TOGGLE, e->toggles);
  check_count(e->name, UIS_K_CHIP, e->chips);
  check_count(e->name, UIS_K_SELECT, e->selects);
  check_count(e->name, UIS_K_SLIDER, e->sliders);
  check_tokens(e->name);
}

static void test_page(const page_expect *e)
{
  ui_open_settings(F.ui, true);
  F.ui->settings_page = e->page;
  F.ui->dirty = true;
  if (e->page == TP_DEVICES || e->page == TP_CONNECTIONS) {
    // The first draw asks the mock for the list; pump until it lands, or the cards (and their buttons) are not drawn.
    draw_settings_page();
    frame(3);
    F.ui->dirty = true;
  }
  uis_audit_reset();
  draw_settings_page();
  check_page(e);
}

static void test_login_page(void)
{
  ui_open_settings(F.ui, true);
  F.ui->settings_page = TP_ACCOUNT;
  for (int tab = 0; tab < 2; tab++) {
    F.ui->login_tab = tab;
    F.ui->dirty = true;
    uis_audit_reset();
    draw_settings_page();
    ccord_ui_settings_debug sd = {0};
    ccord_ui_get_settings_debug(F.ui, &sd);
    CT_CHECK(sd.login_screen && sd.login_tab == tab);
    check_page(tab == 0 ? &k_login_token : &k_login_creds);
  }
  ccord_ui_set_login_status(F.ui, "Enter your 6-digit code.", true);
  uis_audit_reset();
  draw_settings_page();
  ccord_ui_settings_debug sd = {0};
  ccord_ui_get_settings_debug(F.ui, &sd);
  CT_CHECK(sd.login_screen && sd.account_mfa_required && sd.mfa_popup_rect.w > 0.0f);
  check_page(&k_login_mfa);
  ccord_ui_clear_account_token_text(F.ui);
}

// The My Account footer, drawn by settings.c account_buttons: a 156 px primary button and a 132 px secondary button
// at the foot of the page. Signed in, Refresh (primary) and Sign out (secondary) are drawn. Signed out with sign-in
// configured, Sign in with Discord (primary) is drawn. Each state is drawn on the real page, so the footer buttons
// reach the audit and check_tokens checks their heights, radii and fonts like every other control.
static bool audit_has_button_width(float w)
{
  const uis_audit *a = uis_audit_get();
  for (uint32_t i = 0; i < a->kept; i++)
    if (a->rec[i].kind == UIS_K_BUTTON && near(a->rec[i].rect.w, w)) return true;
  return false;
}

static void test_account_footer(void)
{
  ui_open_settings(F.ui, true);
  F.ui->settings_page = TP_ACCOUNT;

  ccord_identity id;
  memset(&id, 0, sizeof id);
  id.user_id = 1;
  snprintf(id.username, sizeof id.username, "%s", "alice");
  snprintf(id.display_name, sizeof id.display_name, "%s", "Alice");

  ccord_account_view v;
  ccord_account_view_build(&v, CCORD_ACCOUNT_SIGNED_IN, CCORD_ACCOUNT_ERROR_NONE, true, &id);
  ccord_ui_set_account_view(F.ui, &v);
  F.ui->dirty = true;
  uis_audit_reset();
  draw_settings_page();
  CT_CHECK(audit_has_button_width(156.0f));   // Refresh
  CT_CHECK(audit_has_button_width(132.0f));   // Sign out
  check_tokens("My Account (signed in)");

  ccord_account_view_build(&v, CCORD_ACCOUNT_SIGNED_OUT, CCORD_ACCOUNT_ERROR_NONE, true, nullptr);
  ccord_ui_set_account_view(F.ui, &v);
  F.ui->dirty = true;
  uis_audit_reset();
  draw_settings_page();
  CT_CHECK(audit_has_button_width(156.0f));   // Sign in with Discord
  check_tokens("My Account (can sign in)");
}

// ---------------------------------------------------------------- raw-draw scan

// Raw rounded-rect draws (ccord_draw_rrect outside a helper) allowed per function, exact counts. Each one is
// non-control chrome: a progress bar, a scrollbar track, a badge, a banner accent, a card ring or a swatch.
// A control drawn raw instead of through a helper would add a count, and the scan then fails.
typedef struct raw_expect {
  const char *fn;
  int count;
} raw_expect;

// Clickable scrollbars (track, thumb, arrows) go through uis_scrollbar and uis_scroll_arrow and are not listed.
static const raw_expect k_raw[] = {
  {"st_draw_account", 2},                // settings_account.c: account busy progress bar (track and fill), not clickable
  {"st_draw_login_screen", 5},           // settings_account.c: login card ring, header accent, badge, banner accents, not clickable
  {"ui_settings_m6_appearance_draw", 2}, // settings_m6_prefs.c: theme palette preview (ring and fill), read-only; the
                                         // theme chooser itself is uis_chip (p_chip_row)
};

#define RAW_FNS_MAX 64
typedef struct raw_count {
  char fn[64];
  int n;
} raw_count;

// Counts ccord_draw_rrect lines per top-level function in one source file. A definition header starts in
// column 0 with an identifier and a '(' and has no ';'. Returns the number of functions seen, or -1.
static int scan_raw(const char *path, raw_count *out, int *nout)
{
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  char line[1024], cur[64] = "";
  while (fgets(line, sizeof line, f)) {
    if (isalpha((unsigned char)line[0]) && strchr(line, '(') && !strchr(line, ';')) {
      const char *p = strchr(line, '(');
      const char *b = p;
      while (b > line && (isalnum((unsigned char)b[-1]) || b[-1] == '_')) b--;
      snprintf(cur, sizeof cur, "%.*s", (int)(p - b), b);
    }
    if (!strstr(line, "ccord_draw_rrect(")) continue;
    int i = 0;
    while (i < *nout && strcmp(out[i].fn, cur) != 0) i++;
    if (i == *nout) {
      if (*nout == RAW_FNS_MAX) {
        fclose(f);
        return -1;
      }
      snprintf(out[i].fn, sizeof out[i].fn, "%s", cur);
      out[i].n = 0;
      (*nout)++;
    }
    out[i].n++;
  }
  fclose(f);
  return 0;
}

// Path of a sibling source file: this test lives in <ui>/tests, so <ui>/<name> is the Settings source.
static bool src_path(char *out, size_t n, const char *name)
{
  const char *t = strstr(__FILE__, "/tests/");
  if (!t) return false;
  snprintf(out, n, "%.*s/%s", (int)(t - __FILE__), __FILE__, name);
  return true;
}

static void test_raw_draws(void)
{
  static const char *const files[] = {
    "settings/settings_core.c",       "settings/settings_ini.c",
    "settings/settings_input.c",      "settings/settings_pages.c",
    "settings/settings_plugins.c",    "settings/settings_account.c",
    "settings/settings_m6_devices.c", "settings/settings_m6_prefs.c",
    "settings/streamer_mask.c",
  };
  raw_count seen[RAW_FNS_MAX];
  int nseen = 0;
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    char path[1024];
    CT_CHECK(src_path(path, sizeof path, files[i]));
    if (scan_raw(path, seen, &nseen) != 0) {
      fprintf(stderr, "cannot scan %s\n", path);
      CT_CHECK(0);
      return;
    }
  }
  for (int i = 0; i < nseen; i++) {
    int want = -1;
    for (size_t k = 0; k < sizeof k_raw / sizeof k_raw[0]; k++)
      if (strcmp(k_raw[k].fn, seen[i].fn) == 0) want = k_raw[k].count;
    if (want != seen[i].n) {
      fprintf(stderr, "%s: %d raw rrect draw(s), allowed %d (draw controls through a helper)\n",
              seen[i].fn[0] ? seen[i].fn : "(top level)", seen[i].n, want);
      CT_CHECK(0);
    }
  }
  for (size_t k = 0; k < sizeof k_raw / sizeof k_raw[0]; k++) {
    int found = 0;
    for (int i = 0; i < nseen; i++)
      if (strcmp(k_raw[k].fn, seen[i].fn) == 0) found = 1;
    if (!found) {
      fprintf(stderr, "%s: allowed raw draw is gone; remove it from k_raw\n", k_raw[k].fn);
      CT_CHECK(0);
    }
  }
}

// A failed setup fails the test; only a missing headless platform is a skip (77).
static int setup_exit(int rc) { return rc == SETUP_SKIP ? 77 : 1; }

int main(int argv_n, char **argv)
{
  (void)argv_n;
  int rc = setup(argv[0], true);
  if (rc != SETUP_OK) return setup_exit(rc);
  for (size_t i = 0; i < sizeof k_expect / sizeof k_expect[0]; i++) test_page(&k_expect[i]);
  test_account_footer();
  teardown();
  rc = setup(argv[0], false);
  if (rc != SETUP_OK) return setup_exit(rc);
  test_login_page();
  teardown();
  test_raw_draws();
  return CT_RESULT("test_ui_style_s1");
}
