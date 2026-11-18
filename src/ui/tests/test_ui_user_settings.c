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

// M6 preference settings pages (m6-ui-prefs): Appearance theme/zoom host callbacks + clamp + INI
// round-trip, Notifications capability gating, Accessibility/Language/Advanced governed PATCHes,
// and Streamer Mode masking. Real store + mock transport + session + renderer, fake OS host with
// recording apply_theme/set_zoom/desktop_capabilities callbacks, injected input events. Governed
// Discord state must only change after the mock answers: every PATCH is asserted before and after
// the pump (design D4: no optimistic mutation).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

#include "../settings_m6_prefs.h"

#include "ccord_test.h"

#define CAP_ALL (CCORD_UI_CAP_AUDIO_PLAYBACK | CCORD_UI_CAP_NOTIFICATIONS | CCORD_UI_CAP_TRAY | CCORD_UI_CAP_GLOBAL_SHORTCUTS)

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
  uint32_t caps;
  int theme_calls, zoom_calls;
  char last_theme[32];
  float last_zoom;
} fixture;

static fixture F;

static uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

static void host_clip_set(void *ud, const char *s) { (void)ud; (void)s; }
static void host_apply_theme(void *ud, const char *id)
{
  (void)ud;
  F.theme_calls++;
  snprintf(F.last_theme, sizeof F.last_theme, "%s", id);
}
static void host_set_zoom(void *ud, float z)
{
  (void)ud;
  F.zoom_calls++;
  F.last_zoom = z;
}
static uint32_t host_caps(void *ud)
{
  (void)ud;
  return F.caps;
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

static void send(ccord_input_event ev) { (void)ccord_ui_handle_input(F.ui, &ev); }

static void mouse(ccord_input_type t, uint8_t button, float x, float y)
{
  send((ccord_input_event){.type = t, .button = button, .clicks = 1, .x = x, .y = y});
}

// Click without pumping: lets a test assert the request went out and nothing changed locally yet.
static void click_raw(float x, float y)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
}

static void click(float x, float y)
{
  click_raw(x, y);
  frame(2);
}

static void click_rect(ccord_rect r)
{
  click(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}

static ccord_ui_settings_debug sdbg(void)
{
  ccord_ui_settings_debug s;
  ccord_ui_get_settings_debug(F.ui, &s);
  return s;
}

static ccord_settings_m6_prefs_debug pdbg(void)
{
  ccord_settings_m6_prefs_debug d;
  ui_settings_m6_prefs_get_debug(F.ui, &d);
  return d;
}

static ccord_ui_debug udbg(void)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(F.ui, &d);
  return d;
}

static bool setup(const char *argv0, bool wipe)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_user_settings_data"));
  if (wipe) {
    char p1[1024], p2[1024];
    snprintf(p1, sizeof p1, "%s/settings.ini", ccord_paths_data_dir());
    snprintf(p2, sizeof p2, "%s/drafts.json", ccord_paths_data_dir());
    remove(p1);
    remove(p2);
  }
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK)
    return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 1, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session || !F.transport) return false;
  ccord_platform_config pc = {.title = "user-settings-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  F.caps = CAP_ALL;
  F.theme_calls = 0;
  F.zoom_calls = 0;
  F.last_theme[0] = '\0';
  F.last_zoom = 0.0f;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .host = {.clipboard_set = host_clip_set, .apply_theme = host_apply_theme,
                              .set_zoom = host_set_zoom, .desktop_capabilities = host_caps},
                     .fixed_now_unix_ms = 1793534400000LL};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  if (ccord_session_start(F.session, &err) != CCORD_OK) return false;
  frame(6);   // let the mock snapshot land so self_id is set (no login screen)
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
  F.ui = nullptr;
  F.session = nullptr;
  F.transport = nullptr;
}

// Open Settings (if closed) via the user-panel gear.
static void open_settings(void)
{
  if (sdbg().open) return;
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  click(l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
  CT_CHECK(sdbg().open);
}

// Navigate to a legacy page (index < 9) through the fixed debug tabs.
static void goto_legacy(int page)
{
  open_settings();
  ccord_ui_settings_debug s = sdbg();
  click_raw(s.tabs[page].x + 10.0f, s.tabs[page].y + 10.0f);
  frame(1);
  CT_CHECK(sdbg().page == page);
}

// Navigate to one of the five M6 preference pages through the host tab seam.
static void goto_prefs(int page)
{
  open_settings();
  ccord_rect tabs[CCORD_M6_PREFS_COUNT];
  ui_settings_m6_prefs_host_rects(F.ui, tabs);
  click_raw(tabs[page].x + 10.0f, tabs[page].y + 10.0f);
  frame(1);
  CT_CHECK(pdbg().drawn_page == page);
}

static const ccord_admin_request *last_admin(void)
{
  const ccord_admin_request *r = ccord_mock_last_admin(F.transport);
  static const ccord_admin_request empty;   // zeroed (op NONE) so a missing request fails checks, not segfaults
  return r ? r : &empty;
}

// ------------------------------------------------------------------ Appearance theme / zoom

static void test_appearance_theme_zoom(void)
{
  goto_legacy(2);   // SP_APPEARANCE
  ccord_settings_m6_prefs_debug d = pdbg();
  CT_CHECK(d.theme_chips[0].w > 0.0f && d.zoom_track.w > 0.0f && d.zoom_reset.w > 0.0f);
  CT_CHECK(sdbg().zoom == 1.0f);
  CT_CHECK(strcmp(sdbg().theme_id, "c-cord-dark") == 0);
  int theme_before = F.theme_calls;

  // Theme chips call host.apply_theme with the builtin id; the same chip is a no-op.
  click_rect(d.theme_chips[1]);
  CT_CHECK(F.theme_calls == theme_before + 1);
  CT_CHECK(strcmp(F.last_theme, "c-cord-light") == 0);
  CT_CHECK(strcmp(sdbg().theme_id, "c-cord-light") == 0);
  click_rect(pdbg().theme_chips[1]);
  CT_CHECK(F.theme_calls == theme_before + 1);   // unchanged: no redundant callback
  click_rect(pdbg().theme_chips[2]);
  CT_CHECK(strcmp(F.last_theme, "c-cord-amoled") == 0);
  CT_CHECK(strcmp(sdbg().theme_id, "c-cord-amoled") == 0);

  // Zoom +/- step 0.05 and call host.set_zoom.
  int zoom_before = F.zoom_calls;
  click_rect(pdbg().zoom_plus);
  CT_CHECK(F.zoom_calls == zoom_before + 1);
  CT_CHECK(F.last_zoom > 1.04f && F.last_zoom < 1.06f);
  CT_CHECK(sdbg().zoom > 1.04f && sdbg().zoom < 1.06f);
  click_rect(pdbg().zoom_minus);
  CT_CHECK(sdbg().zoom > 0.99f && sdbg().zoom < 1.01f);

  // Track extremes clamp to 0.75 / 2.0 (click just inside each end of the track).
  ccord_rect tr = pdbg().zoom_track;
  click(tr.x + 1.0f, tr.y + tr.h * 0.5f);   // near the left end -> clamps to min
  CT_CHECK(sdbg().zoom == 0.75f);
  CT_CHECK(F.last_zoom == 0.75f);
  click(tr.x + tr.w - 1.0f, tr.y + tr.h * 0.5f);   // near the right end -> clamps to max
  CT_CHECK(sdbg().zoom == 2.0f);
  CT_CHECK(F.last_zoom == 2.0f);
}

static void test_zoom_clamp_and_reset(void)
{
  goto_legacy(2);
  ccord_rect tr = pdbg().zoom_track;
  click(tr.x + tr.w - 1.0f, tr.y + tr.h * 0.5f);   // right end -> clamps to max
  CT_CHECK(sdbg().zoom == 2.0f);
  click_rect(pdbg().zoom_minus);   // 2.0 -> 1.95
  CT_CHECK(sdbg().zoom > 1.94f && sdbg().zoom < 1.96f);
  click(tr.x + 1.0f, tr.y + tr.h * 0.5f);   // left end -> clamps to min
  CT_CHECK(sdbg().zoom == 0.75f);
  click_rect(pdbg().zoom_minus);   // already at the floor
  CT_CHECK(sdbg().zoom == 0.75f);
  click_rect(pdbg().zoom_reset);   // reset -> 1.0
  CT_CHECK(sdbg().zoom == 1.0f);
  CT_CHECK(F.last_zoom == 1.0f);
}

// ------------------------------------------------------------------ INI round-trip + validation

static void test_ini_roundtrip(void)
{
  goto_legacy(2);
  click_rect(pdbg().theme_chips[2]);   // amoled
  ccord_rect tr = pdbg().zoom_track;
  click(tr.x + tr.w * 0.5f, tr.y + tr.h * 0.5f);   // ~1.375 -> snaps to 1.40 or 1.35
  float zoom_saved = sdbg().zoom;
  goto_prefs(CCORD_M6_PREFS_STREAMER);
  click_rect(pdbg().streamer_toggle);   // streamer on
  CT_CHECK(sdbg().streamer_mode);
  goto_prefs(CCORD_M6_PREFS_CCORD);
  click_rect(pdbg().notify_badge);      // badge off (default on)
  CT_CHECK(!pdbg().notify_badge_on);
  click_rect(pdbg().notify_focus);      // focus suppress on
  CT_CHECK(pdbg().notify_focus_suppress);

  // Reload the whole UI from the persisted INI (no wipe): values and host callbacks survive.
  teardown();
  CT_CHECK(setup("test_ui_user_settings", false));
  CT_CHECK(strcmp(sdbg().theme_id, "c-cord-amoled") == 0);
  CT_CHECK(sdbg().zoom == zoom_saved);
  CT_CHECK(sdbg().streamer_mode);
  // apply_loaded fired the host callbacks for the persisted theme/zoom during creation.
  CT_CHECK(F.theme_calls >= 1 && strcmp(F.last_theme, "c-cord-amoled") == 0);
  CT_CHECK(F.zoom_calls >= 1 && F.last_zoom == zoom_saved);
  goto_prefs(CCORD_M6_PREFS_CCORD);
  CT_CHECK(!pdbg().notify_badge_on);
  CT_CHECK(pdbg().notify_focus_suppress);
}

static void write_ini(const char *body)
{
  char path[1024];
  ccord_error err;
  if (ccord_paths_join(path, sizeof path, ccord_paths_data_dir(), "settings.ini", &err) != CCORD_OK) return;
  (void)mkdir(ccord_paths_data_dir(), 0700);
  FILE *f = fopen(path, "wb");
  CT_CHECK(f != nullptr);
  if (!f) return;
  fprintf(f, "%s", body);
  fclose(f);
}

static void test_ini_validation(void)
{
  // Unknown theme falls back to c-cord-dark; out-of-range zoom clamps; non-numeric zoom is ignored.
  teardown();
  CT_CHECK(setup("test_ui_user_settings", true));
  teardown();
  write_ini("[ui]\ntheme = totally-bogus\nzoom = 9.5\nstreamer_mode = 1\n");
  CT_CHECK(setup("test_ui_user_settings", false));
  CT_CHECK(strcmp(sdbg().theme_id, "c-cord-dark") == 0);
  CT_CHECK(sdbg().zoom == 2.0f);
  CT_CHECK(sdbg().streamer_mode);
  CT_CHECK(strcmp(F.last_theme, "c-cord-dark") == 0);   // fallback still applied through the host
  CT_CHECK(F.last_zoom == 2.0f);

  teardown();
  write_ini("[ui]\ntheme = c-cord-light\nzoom = 0.1\n");
  CT_CHECK(setup("test_ui_user_settings", false));
  CT_CHECK(strcmp(sdbg().theme_id, "c-cord-light") == 0);
  CT_CHECK(sdbg().zoom == 0.75f);

  teardown();
  write_ini("[ui]\nzoom = not-a-number\n");
  CT_CHECK(setup("test_ui_user_settings", false));
  CT_CHECK(sdbg().zoom == 1.0f);   // invalid zoom leaves the default
  CT_CHECK(F.zoom_calls == 0);     // nothing to apply
}

// ------------------------------------------------------------------ Notifications capability gating

static void test_notifications_caps(void)
{
  // No capabilities reported: desktop/sounds/badge rows are disabled and inert.
  F.caps = 0;
  goto_prefs(CCORD_M6_PREFS_NOTIFICATIONS);
  ccord_settings_m6_prefs_debug d = pdbg();
  CT_CHECK(d.caps == 0);
  bool desktop = sdbg().notify_desktop;
  bool sounds = sdbg().notify_sounds;
  bool badge = d.notify_badge_on;
  size_t before = ccord_mock_admin_count(F.transport);
  click_rect(d.notify_desktop);
  click_rect(d.notify_sounds);
  CT_CHECK(sdbg().notify_desktop == desktop);   // gated: unchanged
  CT_CHECK(sdbg().notify_sounds == sounds);
  goto_prefs(CCORD_M6_PREFS_CCORD);   // the unread tray badge lives on the C-CORD page; gated too
  click_rect(pdbg().notify_badge);
  CT_CHECK(pdbg().notify_badge_on == badge);
  goto_prefs(CCORD_M6_PREFS_NOTIFICATIONS);
  d = pdbg();
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);
  // Mentions/messages/timeout/focus are local and NOT capability-gated.
  bool mentions0 = F.ui->settings_notify_mentions;
  click_rect(d.notify_mentions);
  CT_CHECK(F.ui->settings_notify_mentions == !mentions0);
  CT_CHECK(sdbg().page == 6);   // still on Notifications
  bool messages0 = F.ui->settings_notify_messages;
  click_rect(d.notify_messages);
  CT_CHECK(F.ui->settings_notify_messages == !messages0);
  // Timeout chips pick a fixed duration (local, no REST request).
  click_rect(d.notify_timeout[2]);   // "10s"
  CT_CHECK(sdbg().notify_timeout_ms == 10000);
  click_rect(pdbg().notify_timeout[3]);   // "Never"
  CT_CHECK(sdbg().notify_timeout_ms == 0);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);
  goto_prefs(CCORD_M6_PREFS_CCORD);
  bool focus0 = pdbg().notify_focus_suppress;
  click_rect(pdbg().notify_focus);
  CT_CHECK(pdbg().notify_focus_suppress == !focus0);
  goto_prefs(CCORD_M6_PREFS_NOTIFICATIONS);

  // A null host capability callback means "all available" (offline/demo fixtures keep working).
  uint32_t (*saved_caps)(void *) = F.ui->d.host.desktop_capabilities;
  F.ui->d.host.desktop_capabilities = nullptr;
  CT_CHECK(pdbg().caps == CAP_ALL);
  F.ui->d.host.desktop_capabilities = saved_caps;

  // With the capabilities present the same rows toggle.
  F.caps = CAP_ALL;
  frame(1);
  d = pdbg();
  CT_CHECK(d.caps == CAP_ALL);
  click_rect(d.notify_desktop);
  CT_CHECK(sdbg().notify_desktop == !desktop);
  click_rect(d.notify_sounds);
  CT_CHECK(sdbg().notify_sounds == !sounds);
  goto_prefs(CCORD_M6_PREFS_CCORD);
  click_rect(pdbg().notify_badge);
  CT_CHECK(pdbg().notify_badge_on == !badge);
  goto_prefs(CCORD_M6_PREFS_NOTIFICATIONS);
  d = pdbg();

  // /tts command is a governed PATCH (enable_tts_command), post-response only.
  const ccord_user_settings *us0 = ccord_store_user_settings(F.store);
  bool tts0 = us0 ? us0->enable_tts_command : false;
  click_raw(d.tts_toggle.x + d.tts_toggle.w - 8.0f, d.tts_toggle.y + 12.0f);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_USER_SETTINGS_PATCH);
  CT_CHECK(last_admin()->settings_patch.enable_tts_command == (tts0 ? 0 : 1));
  frame(2);
  const ccord_user_settings *us = ccord_store_user_settings(F.store);
  CT_CHECK(us && us->enable_tts_command == !tts0);
  CT_CHECK(sdbg().enable_tts_command == !tts0);   // debug getter reports the confirmed store state
}

// ------------------------------------------------------------------ Accessibility governed PATCHes

static void test_accessibility(void)
{
  goto_prefs(CCORD_M6_PREFS_ACCESSIBILITY);
  ccord_settings_m6_prefs_debug d = pdbg();
  CT_CHECK(d.gif_toggle.w > 0.0f && d.sticker_chips[2].w > 0.0f);

  // GIF autoplay: governed PATCH flips the confirmed value; no optimistic store write before the
  // mock answers. (The store may already hold settings from an earlier PATCH, so read the current
  // confirmed value rather than assuming it is absent.)
  const ccord_user_settings *us = ccord_store_user_settings(F.store);
  bool gif0 = us ? us->gif_auto_play : true;   // Discord default: on
  click_raw(d.gif_toggle.x + d.gif_toggle.w - 8.0f, d.gif_toggle.y + 12.0f);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_USER_SETTINGS_PATCH);
  CT_CHECK(last_admin()->settings_patch.gif_auto_play == (gif0 ? 0 : 1));
  CT_CHECK(last_admin()->settings_patch.animate_emoji == -1);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(!us || us->gif_auto_play == gif0);   // unchanged until the answer lands
  frame(2);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(us && us->gif_auto_play == !gif0);
  CT_CHECK(sdbg().gif_auto_play == !gif0);

  // Animated emoji toggle.
  us = ccord_store_user_settings(F.store);
  bool emoji0 = us ? us->animate_emoji : true;
  click_rect(pdbg().emoji_toggle);
  CT_CHECK(last_admin()->settings_patch.animate_emoji == (emoji0 ? 0 : 1));
  frame(2);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(us && us->animate_emoji == !emoji0);

  // Sticker chips: pick a value different from the confirmed one; the same chip is a no-op.
  us = ccord_store_user_settings(F.store);
  int stick_target = (us && us->animate_stickers == 2) ? 1 : 2;
  click_rect(pdbg().sticker_chips[stick_target]);
  CT_CHECK(last_admin()->settings_patch.animate_stickers == stick_target);
  frame(2);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(us && us->animate_stickers == stick_target);
  size_t before = ccord_mock_admin_count(F.transport);
  click_rect(pdbg().sticker_chips[stick_target]);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);

  // A refused PATCH keeps the confirmed value and surfaces a notice.
  us = ccord_store_user_settings(F.store);
  bool gif_before = us->gif_auto_play;
  ccord_mock_fail_next_admin(F.transport, true);
  click_rect(pdbg().gif_toggle);
  frame(2);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(us && us->gif_auto_play == gif_before);   // unchanged
  CT_CHECK(strstr(udbg().notice, "Couldn't") != nullptr);

  // Reduced motion + message display are local (no REST request).
  before = ccord_mock_admin_count(F.transport);
  bool rm0 = sdbg().reduced_motion;
  click_rect(pdbg().reduced_motion_toggle);
  CT_CHECK(sdbg().reduced_motion == !rm0);
  CT_CHECK(F.ui->anim.reduced_motion == !rm0);
  click_rect(pdbg().display_chips[0]);   // compact
  CT_CHECK(sdbg().display_mode_value == 0);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);
}

// ------------------------------------------------------------------ Language governed PATCH

static void test_language(void)
{
  goto_prefs(CCORD_M6_PREFS_LANGUAGE);
  ccord_settings_m6_prefs_debug d = pdbg();
  CT_CHECK(d.language_row.w > 0.0f);
  CT_CHECK(d.locale_index == 0);   // en-US default
  // Cycle to the next locale (en-GB): governed PATCH, post-response only.
  const ccord_user_settings *us = ccord_store_user_settings(F.store);
  char loc0[32];
  snprintf(loc0, sizeof loc0, "%s", us ? us->locale : "en-US");
  click_raw(d.language_row.x + 20.0f, d.language_row.y + 12.0f);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_USER_SETTINGS_PATCH);
  CT_CHECK(strcmp(last_admin()->settings_patch.locale, "en-GB") == 0);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(!us || strcmp(us->locale, loc0) == 0);   // no optimistic write
  frame(2);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(us && strcmp(us->locale, "en-GB") == 0);
  CT_CHECK(strcmp(sdbg().locale, "en-GB") == 0);
  CT_CHECK(pdbg().locale_index == 1);
  // Cycle again -> de.
  click_rect(pdbg().language_row);
  CT_CHECK(strcmp(last_admin()->settings_patch.locale, "de") == 0);
  frame(2);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(us && strcmp(us->locale, "de") == 0);
}

// ------------------------------------------------------------------ Advanced governed PATCH + local

static void test_advanced(void)
{
  goto_prefs(CCORD_M6_PREFS_ADVANCED);
  ccord_settings_m6_prefs_debug d = pdbg();
  CT_CHECK(d.developer_toggle.w > 0.0f && d.hardware_toggle.w > 0.0f);
  // Developer Mode: governed PATCH flips the confirmed value; no optimistic store write.
  const ccord_user_settings *us = ccord_store_user_settings(F.store);
  bool dev0 = us ? us->developer_mode : false;   // Discord default: off
  click_raw(d.developer_toggle.x + d.developer_toggle.w - 8.0f, d.developer_toggle.y + 12.0f);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_USER_SETTINGS_PATCH);
  CT_CHECK(last_admin()->settings_patch.developer_mode == (dev0 ? 0 : 1));
  us = ccord_store_user_settings(F.store);
  CT_CHECK(!us || us->developer_mode == dev0);   // unchanged until the answer lands
  frame(2);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(us && us->developer_mode == !dev0);
  CT_CHECK(sdbg().developer_mode == !dev0);

  // Hardware acceleration is local INI (no REST request), toggles immediately.
  size_t before = ccord_mock_admin_count(F.transport);
  bool hw = sdbg().hardware_accel;
  click_rect(pdbg().hardware_toggle);
  CT_CHECK(sdbg().hardware_accel == !hw);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);
}

// ------------------------------------------------------------------ Streamer Mode + masking

static void set_account_view(void)
{
  ccord_account_view v = {0};
  v.mode = CCORD_ACCOUNT_SIGNED_IN;
  v.signed_in = true;
  v.status = "Signed in";
  v.display_name = "Alex";
  v.username = "alex";
  snprintf(v.user_id_text, sizeof v.user_id_text, "123456789012345678");
  ccord_ui_set_account_view(F.ui, &v);
}

static void test_streamer_masking(void)
{
  set_account_view();
  // Direct helper: off returns the input; on returns a same-length '*' mask.
  char buf[64];
  F.ui->settings_streamer_mode = false;
  const char *plain = ui_settings_m6_mask(F.ui, "alex", buf, sizeof buf);
  CT_CHECK(strcmp(plain, "alex") == 0);
  F.ui->settings_streamer_mode = true;
  const char *masked = ui_settings_m6_mask(F.ui, "alex", buf, sizeof buf);
  CT_CHECK(strcmp(masked, "****") == 0);
  CT_CHECK(strcmp(pdbg().mask_last, "****") == 0);
  // Empty input is never masked.
  CT_CHECK(strcmp(ui_settings_m6_mask(F.ui, "", buf, sizeof buf), "") == 0);

  // The My Account page draws the masked identity while Streamer Mode is on.
  F.ui->settings_streamer_mode = false;
  goto_legacy(0);   // SP_ACCOUNT
  frame(1);
  CT_CHECK(pdbg().mask_last[0] == '\0');   // nothing masked when off
  F.ui->settings_streamer_mode = true;
  frame(1);
  CT_CHECK(pdbg().mask_last[0] == '*');   // the drawn identity is masked
  bool all_stars = true;
  for (const char *p = pdbg().mask_last; *p; p++) {
    if (*p != '*') all_stars = false;
  }
  CT_CHECK(all_stars);
}

// ui_streamer_mask_invites / _premask / _mask_inplace: the invite matcher behind Hide Instant Invite Links.
static size_t mask_invites(const char *text, char *out, size_t cap)
{
  return ui_streamer_mask_invites(F.ui, text, strlen(text), out, cap, false);
}

static void test_streamer_invite_matcher(void)
{
  char out[256];
  bool prev = F.ui->settings_streamer_mode;
  F.ui->settings_streamer_mode = false;
  CT_CHECK(mask_invites("join discord.gg/abc123", out, sizeof out) == 0);   // Streamer Mode off: untouched
  F.ui->settings_streamer_mode = true;
  CT_CHECK(ui_streamer_get_opts(F.ui).hide_invites);   // default on

  // Positives: every URL form, optional scheme / www, any case, '-' and '_' in the code.
  CT_CHECK(mask_invites("discord.gg/abc123", out, sizeof out) > 0 && strcmp(out, "discord.gg/******") == 0);
  CT_CHECK(mask_invites("see https://discord.com/invite/Ab-c_1 now", out, sizeof out) > 0 &&
           strcmp(out, "see https://discord.com/invite/****** now") == 0);
  CT_CHECK(mask_invites("http://www.discord.gg/xyz", out, sizeof out) > 0 && strcmp(out, "http://www.discord.gg/***") == 0);
  CT_CHECK(mask_invites("DISCORD.GG/AbC", out, sizeof out) > 0 && strcmp(out, "DISCORD.GG/***") == 0);
  CT_CHECK(mask_invites("discordapp.com/invite/qq", out, sizeof out) > 0 && strcmp(out, "discordapp.com/invite/**") == 0);
  // A trailing '.' ends the sentence, not the code; several invites in one string all mask.
  CT_CHECK(mask_invites("discord.gg/abc. and discord.gg/def", out, sizeof out) > 0 &&
           strcmp(out, "discord.gg/***. and discord.gg/***") == 0);

  // Negatives: part of a longer host, nothing after the slash, other paths.
  CT_CHECK(mask_invites("notdiscord.gg/abc", out, sizeof out) == 0);
  CT_CHECK(mask_invites("sub.discord.gg/abc", out, sizeof out) == 0);
  CT_CHECK(mask_invites("discord.gg/", out, sizeof out) == 0);
  CT_CHECK(mask_invites("discord.gg/ abc", out, sizeof out) == 0);
  CT_CHECK(mask_invites("discord.com/channels/1/2", out, sizeof out) == 0);
  CT_CHECK(mask_invites("discord.gg", out, sizeof out) == 0);

  // A too-small output buffer truncates but never leaks the code.
  size_t n = mask_invites("discord.gg/secret99", out, 14);
  CT_CHECK(n > 0 && strstr(out, "secret") == nullptr);

  // Whole-line premask: a code the markdown tokenizer would split ('__') is one masked run.
  char marked[64], stars[64];
  const char *line = "x discord.gg/a__b y";
  CT_CHECK(ui_streamer_premask(line, strlen(line), marked, stars));
  CT_CHECK(memcmp(stars, "x discord.gg/**** y", 19) == 0);
  CT_CHECK(marked[13] == UI_STREAMER_MASK_BYTE && marked[16] == UI_STREAMER_MASK_BYTE && marked[17] == ' ');
  CT_CHECK(!ui_streamer_premask("no invite here", 14, marked, stars));

  // In-place variant (previews): same length, code replaced.
  char prev_line[64] = "reply to discord.gg/abc123!";
  CT_CHECK(ui_streamer_mask_inplace(F.ui, prev_line, strlen(prev_line), false) == strlen(prev_line));
  CT_CHECK(strcmp(prev_line, "reply to discord.gg/******!") == 0);

  // Hide Instant Invite Links off: nothing is masked even with Streamer Mode on; back on re-masks.
  CT_CHECK(ui_settings_m6_prefs_ini_visit(F.ui, "streamer_hide_invites", "0"));
  CT_CHECK(mask_invites("discord.gg/abc123", out, sizeof out) == 0);
  CT_CHECK(!ui_streamer_invites_active(F.ui));
  CT_CHECK(ui_settings_m6_prefs_ini_visit(F.ui, "streamer_hide_invites", "1"));
  CT_CHECK(mask_invites("discord.gg/abc123", out, sizeof out) > 0);
  F.ui->settings_streamer_mode = prev;
}

static void test_streamer_page(void)
{
  goto_prefs(CCORD_M6_PREFS_STREAMER);
  ccord_settings_m6_prefs_debug d = pdbg();
  CT_CHECK(d.streamer_toggle.w > 0.0f && d.stream_notify_toggle.w > 0.0f);
  // Streamer Mode is local: toggles immediately, no REST request, persists to INI. (Earlier tests
  // may have left it on, so assert the flip rather than a fixed starting value.)
  size_t before = ccord_mock_admin_count(F.transport);
  bool sm0 = sdbg().streamer_mode;
  click_rect(d.streamer_toggle);
  CT_CHECK(sdbg().streamer_mode == !sm0);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);

  // "Enable notifications while streaming" is a governed PATCH (stream_notifications_enabled).
  const ccord_user_settings *us = ccord_store_user_settings(F.store);
  bool sn0 = us ? us->stream_notifications_enabled : false;   // Discord default: off
  click_raw(d.stream_notify_toggle.x + d.stream_notify_toggle.w - 8.0f, d.stream_notify_toggle.y + 12.0f);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_USER_SETTINGS_PATCH);
  CT_CHECK(last_admin()->settings_patch.stream_notifications_enabled == (sn0 ? 0 : 1));
  CT_CHECK(ccord_mock_admin_count(F.transport) == before + 1);
  frame(2);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(us && us->stream_notifications_enabled == !sn0);
  CT_CHECK(sdbg().stream_notifications_enabled == !sn0);
}

// C-Cord client prefs: jump_present_messages (1..100, default 10) and game_detection (default on). Saved
// through the real settings.ini writer and read back by a fresh UI instance; out-of-range values clamp.
static void test_client_prefs(void)
{
  CT_CHECK(ccord_ui_pref_jump_present_messages() == 10);
  CT_CHECK(ccord_ui_pref_game_detection());
  ccord_ui_pref_set_jump_present_messages(25);
  ccord_ui_pref_set_game_detection(false);
  ui_settings_save(F.ui);
  teardown();
  CT_CHECK(setup("test_ui_user_settings", false));
  CT_CHECK(ccord_ui_pref_jump_present_messages() == 25);
  CT_CHECK(!ccord_ui_pref_game_detection());

  ccord_ui_pref_set_jump_present_messages(0);
  CT_CHECK(ccord_ui_pref_jump_present_messages() == 1);
  ccord_ui_pref_set_jump_present_messages(1000);
  CT_CHECK(ccord_ui_pref_jump_present_messages() == 100);
  CT_CHECK(ui_settings_m6_prefs_ini_visit(F.ui, "jump_present_messages", "-7"));
  CT_CHECK(ccord_ui_pref_jump_present_messages() == 1);
  CT_CHECK(ui_settings_m6_prefs_ini_visit(F.ui, "jump_present_messages", "99999999999999"));
  CT_CHECK(ccord_ui_pref_jump_present_messages() == 100);
  CT_CHECK(ui_settings_m6_prefs_ini_visit(F.ui, "jump_present_messages", "abc"));
  CT_CHECK(ccord_ui_pref_jump_present_messages() == 100);   // non-numbers are ignored, value unchanged
  ccord_ui_pref_set_jump_present_messages(10);
  ccord_ui_pref_set_game_detection(true);
}

// Close-to-background: default ON, the C-Cord page toggle flips it, it round-trips OFF through settings.ini.
static void test_close_to_background(void)
{
  CT_CHECK(ccord_ui_pref_close_to_background());
  goto_prefs(CCORD_M6_PREFS_CCORD);
  CT_CHECK(pdbg().close_background.w > 0.0f && pdbg().close_background_on);
  click_rect(pdbg().close_background);
  CT_CHECK(!ccord_ui_pref_close_to_background() && !pdbg().close_background_on);
  teardown();
  CT_CHECK(setup("test_ui_user_settings", false));
  CT_CHECK(!ccord_ui_pref_close_to_background());
  ccord_ui_pref_set_close_to_background(true);
  ui_settings_save(F.ui);
}

// Stub overlays: open marks open, a typed key/text does not reach the composer, Escape closes.
static void test_overlay_stubs(void)
{
  typedef struct {
    void (*open)(ccord_ui *);
    bool (*is_open)(const ccord_ui *);
  } ov;
  const ov ovs[] = {{ui_plus_menu_open, ui_plus_menu_is_open},
                    {ui_poll_composer_open, ui_poll_composer_is_open},
                    {ui_schedule_dialog_open, ui_schedule_dialog_is_open}};
  for (size_t i = 0; i < 3; i++) {
    size_t before = strlen(ccord_ui_composer_text(F.ui));
    ovs[i].open(F.ui);
    CT_CHECK(ovs[i].is_open(F.ui));
    ccord_input_event t = {.type = CCORD_IN_TEXT};
    snprintf(t.text, sizeof t.text, "z");
    CT_CHECK(ccord_ui_handle_input(F.ui, &t));
    ccord_input_event k = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_A};
    CT_CHECK(ccord_ui_handle_input(F.ui, &k));
    CT_CHECK(strlen(ccord_ui_composer_text(F.ui)) == before);
    CT_CHECK(ovs[i].is_open(F.ui));
    ccord_input_event e = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_ESCAPE};
    CT_CHECK(ccord_ui_handle_input(F.ui, &e));
    CT_CHECK(!ovs[i].is_open(F.ui));
  }
}

int main(int argc, char **argv)
{
  (void)argc;
  if (!setup(argv[0], true)) return 77;
  test_appearance_theme_zoom();
  test_zoom_clamp_and_reset();
  test_ini_roundtrip();
  test_ini_validation();
  test_notifications_caps();
  test_accessibility();
  test_language();
  test_advanced();
  test_streamer_masking();
  test_streamer_invite_matcher();
  test_streamer_page();
  test_client_prefs();
  test_close_to_background();
  test_overlay_stubs();
  teardown();
  return CT_RESULT("test_ui_user_settings");
}
