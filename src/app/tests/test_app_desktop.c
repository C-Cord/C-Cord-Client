// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M6 desktop glue (m6-app): startup INI prefs, builtin theme load/fallback, the ui host callbacks
// (apply_theme/set_zoom/desktop_capabilities), SNI unread badge sync, notification eligibility +
// suppression and the GlobalShortcuts mute/deafen actions. Real store + mock transport + session +
// headless platform/renderer/ui; no live Discord host is contacted.
// Result 5/6 additions: live QuickCSS (desktop + Appearance editor), Close-to-Tray, rebindable
// keybinds, the saved-account switcher roster and the single-instance UNIX control socket.
#define _POSIX_C_SOURCE 200809L
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/capture.h>
#include <ccord/platform/platform.h>
#include <ccord/session/session.h>
#include <ccord/store/notify.h>
#include <ccord/ui/ui.h>

#include "app_desktop.h"
#include "settings_m6_prefs.h"

#include "ccord_test.h"

#define HQ 2001u
#define ADA 1002u
#define HQ_VOICE 3121u

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
  ccord_app_desktop *desktop;
  uint64_t now;
} fixture;

static fixture F;
static ccord_ui_deps F_deps;   // kept so tests can recreate the UI against a persisted settings.ini

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
static void host_apply_theme(void *ud, const char *id) { ccord_app_desktop_apply_theme(ud, id); }
static void host_set_zoom(void *ud, float z) { ccord_app_desktop_set_zoom(ud, z); }
static uint32_t host_caps(void *ud) { return ccord_app_desktop_capabilities(ud); }

// Result 5/6 host seams: record what the settings UI asked the app to do, then apply it for real.
static int s_tray_calls, s_css_calls, s_kb_calls, s_switch_calls, s_remove_calls;
static bool s_tray_last, s_css_enabled_last;
static char s_css_last[256];
static uint64_t s_switch_id, s_remove_id;
static void host_set_close_to_tray(void *ud, bool on)
{
  (void)ud;
  s_tray_calls++;
  s_tray_last = on;
  ccord_platform_set_close_to_tray(F.platform, on);
}
static void host_apply_quick_css(void *ud, bool enabled, const char *css)
{
  s_css_calls++;
  s_css_enabled_last = enabled;
  snprintf(s_css_last, sizeof s_css_last, "%s", css ? css : "");
  ccord_app_desktop_apply_quick_css(ud, enabled, css);
}
static void host_keybinds_changed(void *ud)
{
  s_kb_calls++;
  (void)ccord_app_desktop_bind_shortcuts(ud);   // headless: no portal, but the rebind path runs
}
static void host_account_switch(void *ud, uint64_t user_id) { (void)ud; s_switch_calls++; s_switch_id = user_id; }
static void host_account_remove(void *ud, uint64_t user_id) { (void)ud; s_remove_calls++; s_remove_id = user_id; }

static void frame(int n)
{
  for (int i = 0; i < n; i++) {
    F.now += 50;
    (void)ccord_session_pump(F.session);
    (void)ccord_bus_drain(F.bus);
    (void)ccord_ui_update(F.ui, F.now);
  }
}

static bool write_text(const char *path, const char *text)
{
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  size_t n = strlen(text);
  bool ok = fwrite(text, 1, n, f) == n;
  fclose(f);
  return ok;
}

static ccord_app_desktop_debug dbg(void)
{
  ccord_app_desktop_debug d;
  ccord_app_desktop_get_debug(F.desktop, &d);
  return d;
}

// ---------------------------------------------------------------- settings UI input helpers

static void send_ev(ccord_input_event ev) { (void)ccord_ui_handle_input(F.ui, &ev); }

static void click_raw(float x, float y)
{
  send_ev((ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  send_ev((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  send_ev((ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y});
}

static void click_rect(ccord_rect r)
{
  click_raw(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  frame(1);
}

static void key(ccord_key k, uint8_t mods)
{
  send_ev((ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = k, .mods = mods});
}

static void text(const char *s)
{
  ccord_input_event ev = {.type = CCORD_IN_TEXT};
  snprintf(ev.text, sizeof ev.text, "%s", s);
  send_ev(ev);
}

static ccord_ui_settings_debug sdbg(void)
{
  ccord_ui_settings_debug s;
  ccord_ui_get_settings_debug(F.ui, &s);
  return s;
}

static ccord_ui_debug udbg(void)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(F.ui, &d);
  return d;
}

static void draw_once(void)
{
  ccord_render_begin(F.renderer, (ccord_color){0x31, 0x33, 0x38, 255});
  ccord_ui_draw(F.ui, F.renderer);
  ccord_render_end(F.renderer);
}

// Open Settings (if closed) via the user-panel gear.
static void open_settings(void)
{
  if (sdbg().open) return;
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  click_raw(l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
  frame(1);
  CT_CHECK(sdbg().open);
}

// Legacy sidebar pages (debug tabs 0..8): 0 My Account, 2 Appearance, 7 Keybinds.
static void goto_page(int page)
{
  open_settings();
  ccord_ui_settings_debug s = sdbg();
  click_raw(s.tabs[page].x + 10.0f, s.tabs[page].y + 10.0f);
  frame(1);
  CT_CHECK(sdbg().page == page);
  draw_once();   // exercise the page's draw path (roster, snippet editor, keybind rows)
}

// Advanced is an M6 preference page: navigate through the host tab seam and draw once so the
// module-local toggle rect (close-to-tray) is populated.
static void goto_advanced(void)
{
  open_settings();
  ccord_rect tabs[CCORD_M6_PREFS_COUNT];
  ui_settings_m6_prefs_host_rects(F.ui, tabs);
  click_raw(tabs[CCORD_M6_PREFS_ADVANCED].x + 10.0f, tabs[CCORD_M6_PREFS_ADVANCED].y + 10.0f);
  frame(1);
  draw_once();
}

static void close_settings(void)
{
  if (!sdbg().open) return;
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!sdbg().open);
}

// Reads a whole small file into a static buffer (persistence assertions).
static const char *read_file(const char *path)
{
  static char buf[8192];
  buf[0] = '\0';
  FILE *f = fopen(path, "rb");
  if (!f) return buf;
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = '\0';
  fclose(f);
  return buf;
}

// Recreates the UI so ui_settings_init re-reads settings.ini (startup-pref application path).
static bool recreate_ui(void)
{
  ccord_error err;
  ccord_ui_destroy(F.ui);
  F.ui = ccord_ui_create(&F_deps, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  ccord_app_desktop_attach_ui(F.desktop, F.ui);
  return true;
}

static bool color_is(ccord_color c, uint8_t r, uint8_t g, uint8_t b)
{
  return c.r == r && c.g == g && c.b == b && c.a == 255;
}

// ---------------------------------------------------------------- startup INI pre-read

static void test_startup_ini(void)
{
  ccord_app_startup s;
  ccord_app_desktop_startup_defaults(&s);
  CT_CHECK(strcmp(s.theme_id, "c-cord-dark") == 0);
  CT_CHECK(s.zoom == 1.0f);
  CT_CHECK(s.hardware_accel);

  // Missing file keeps every default.
  ccord_app_desktop_startup_read(&s, "app_desktop_missing.ini");
  CT_CHECK(strcmp(s.theme_id, "c-cord-dark") == 0 && s.zoom == 1.0f && s.hardware_accel);

  const char *path = "app_desktop_settings.ini";
  CT_CHECK(write_text(path,
                      "# saved UI prefs\n[ui]\ntheme = c-cord-light\nzoom = 1.5\nhardware_accel = 0\n"
                      "notify_sounds = 0\n"));
  ccord_app_desktop_startup_read(&s, path);
  CT_CHECK(strcmp(s.theme_id, "c-cord-light") == 0);
  CT_CHECK(fabsf(s.zoom - 1.5f) < 0.001f);
  CT_CHECK(!s.hardware_accel);

  // Out-of-range zoom is clamped to the renderer's 0.75..2.0 range.
  CT_CHECK(write_text(path, "[ui]\nzoom = 0.2\n"));
  ccord_app_desktop_startup_read(&s, path);
  CT_CHECK(fabsf(s.zoom - CCORD_ZOOM_MIN) < 0.001f);
  CT_CHECK(write_text(path, "[ui]\nzoom = 9\n"));
  ccord_app_desktop_startup_read(&s, path);
  CT_CHECK(fabsf(s.zoom - CCORD_ZOOM_MAX) < 0.001f);
  CT_CHECK(write_text(path, "[ui]\nzoom = NaN\n"));
  ccord_app_desktop_startup_read(&s, path);
  CT_CHECK(fabsf(s.zoom - 1.0f) < 0.001f);
  CT_CHECK(write_text(path, "[ui]\nzoom = garbage\n"));
  ccord_app_desktop_startup_read(&s, path);
  CT_CHECK(fabsf(s.zoom - 1.0f) < 0.001f);

  // Unknown keys/sections and unsafe theme ids are ignored (no path or INI injection).
  CT_CHECK(write_text(path, "[other]\ntheme = c-cord-amoled\n\n[ui]\ntheme = ../escape\n"));
  ccord_app_desktop_startup_read(&s, path);
  CT_CHECK(strcmp(s.theme_id, "c-cord-dark") == 0);

  // hardware_accel accepts the INI booleans.
  CT_CHECK(write_text(path, "[ui]\nhardware_accel = yes\n"));
  ccord_app_desktop_startup_read(&s, path);
  CT_CHECK(s.hardware_accel);

  // Result 5/6 startup prefs: close-to-tray and the QuickCSS snippet.
  CT_CHECK(s.close_to_tray && !s.quickcss_enabled && s.quickcss[0] == '\0');
  CT_CHECK(write_text(path, "[ui]\nclose_to_tray = 1\nquickcss_enabled = 1\n"
                            "quickcss = --background-primary: #112233;\n"));
  ccord_app_desktop_startup_read(&s, path);
  CT_CHECK(s.close_to_tray);
  CT_CHECK(s.quickcss_enabled);
  CT_CHECK(strcmp(s.quickcss, "--background-primary: #112233;") == 0);
  CT_CHECK(write_text(path, "[ui]\nclose_to_tray = 0\nquickcss_enabled = 0\n"));
  ccord_app_desktop_startup_read(&s, path);
  CT_CHECK(!s.close_to_tray && !s.quickcss_enabled);
  remove(path);
}

// ---------------------------------------------------------------- builtin theme load / fallback

static void test_startup_theme(void)
{
  ccord_error err = {0};
  ccord_theme t;
  memset(&t, 0, sizeof t);
  ccord_app_startup s;
  ccord_app_desktop_startup_defaults(&s);

  snprintf(s.theme_id, sizeof s.theme_id, "c-cord-light");
  CT_CHECK(ccord_app_desktop_load_theme(&t, &s, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(t.id, "c-cord-light") == 0);

  snprintf(s.theme_id, sizeof s.theme_id, "not-a-builtin");
  CT_CHECK(ccord_app_desktop_load_theme(&t, &s, nullptr, &err) == CCORD_OK);
  CT_CHECK(strcmp(t.id, "c-cord-dark") == 0);

  // --theme still overrides the saved id (the file wins).
  char path[PATH_MAX];
  CT_CHECK(ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-amoled.theme", &err) ==
           CCORD_OK);
  snprintf(s.theme_id, sizeof s.theme_id, "c-cord-light");
  CT_CHECK(ccord_app_desktop_load_theme(&t, &s, path, &err) == CCORD_OK);
  CT_CHECK(strcmp(t.id, "c-cord-amoled") == 0);

  // An unreadable --theme path stays a hard error (the pre-M6 CLI contract).
  CT_CHECK(ccord_app_desktop_load_theme(&t, &s, "app_desktop_no_such.theme", &err) != CCORD_OK);
}

// ---------------------------------------------------------------- host callbacks

static void test_host_callbacks(void)
{
  ccord_app_desktop_debug before = dbg();
  CT_CHECK(before.ui_attached);
  CT_CHECK(!ccord_app_desktop_take_redraw(F.desktop));

  // apply_theme loads the asset file (or the builtin) in place and asks for one redraw.
  host_apply_theme(F.desktop, "c-cord-light");
  CT_CHECK(strcmp(F.theme.id, "c-cord-light") == 0);
  CT_CHECK(ccord_app_desktop_take_redraw(F.desktop));
  CT_CHECK(!ccord_app_desktop_take_redraw(F.desktop));

  // Unknown id falls back to c-cord-dark instead of leaving the previous theme.
  host_apply_theme(F.desktop, "nope");
  CT_CHECK(strcmp(F.theme.id, "c-cord-dark") == 0);
  // An unsafe id (path/INI injection) also falls back instead of reaching the asset path.
  host_apply_theme(F.desktop, "../escape");
  CT_CHECK(strcmp(F.theme.id, "c-cord-dark") == 0);

  // set_zoom clamps and drives the renderer; NaN maps to the minimum.
  host_set_zoom(F.desktop, 5.0f);
  CT_CHECK(ccord_render_get_zoom(F.renderer) == CCORD_ZOOM_MAX);
  host_set_zoom(F.desktop, 0.1f);
  CT_CHECK(ccord_render_get_zoom(F.renderer) == CCORD_ZOOM_MIN);
  host_set_zoom(F.desktop, NAN);
  CT_CHECK(ccord_render_get_zoom(F.renderer) == CCORD_ZOOM_MIN);
  host_set_zoom(F.desktop, 1.25f);
  CT_CHECK(fabsf(ccord_render_get_zoom(F.renderer) - 1.25f) < 0.001f);
  CT_CHECK(ccord_app_desktop_take_redraw(F.desktop));
  host_set_zoom(F.desktop, 1.0f);

  // Headless: no session bus, so the portal/tray/notification bits are honestly absent.
  uint32_t caps = host_caps(F.desktop);
  CT_CHECK((caps & CCORD_UI_CAP_NOTIFICATIONS) == 0);
  CT_CHECK((caps & CCORD_UI_CAP_TRAY) == 0);
  CT_CHECK((caps & CCORD_UI_CAP_GLOBAL_SHORTCUTS) == 0);
  CT_CHECK((caps & ~(uint32_t)(CCORD_UI_CAP_AUDIO_PLAYBACK | CCORD_UI_CAP_NOTIFICATIONS | CCORD_UI_CAP_TRAY |
                               CCORD_UI_CAP_GLOBAL_SHORTCUTS)) == 0);
  CT_CHECK(dbg().capabilities == caps);
}

// ---------------------------------------------------------------- startup prefs reach the host

static void test_startup_prefs_apply(void)
{
  // Persisted close-to-tray + QuickCSS are applied during ui_settings_init (ui create).
  CT_CHECK(write_text("app_desktop_data/settings.ini",
                      "[ui]\nclose_to_tray = 1\nquickcss_enabled = 1\n"
                      "quickcss = --background-primary: #112233;\n"));
  int tray_calls = s_tray_calls, css_calls = s_css_calls;
  CT_CHECK(recreate_ui());
  CT_CHECK(s_tray_calls == tray_calls + 1 && s_tray_last);
  CT_CHECK(ccord_platform_close_to_tray(F.platform));
  CT_CHECK(s_css_calls == css_calls + 1 && s_css_enabled_last);
  CT_CHECK(strcmp(s_css_last, "--background-primary: #112233;") == 0);
  CT_CHECK(color_is(F.theme.c.bg_primary, 0x11, 0x22, 0x33));

  // Restore the defaults for the remaining tests.
  remove("app_desktop_data/settings.ini");
  CT_CHECK(recreate_ui());
  ccord_platform_set_close_to_tray(F.platform, false);
  ccord_app_desktop_apply_quick_css(F.desktop, false, "");
}

// ---------------------------------------------------------------- tray badge

static void test_badge_sync(void)
{
  const ccord_channel *first = ccord_store_channel_at(F.store, HQ, 1);   // first text channel
  CT_CHECK(first != nullptr);
  const ccord_guild *g2 = ccord_store_guild_at(F.store, 1);
  CT_CHECK(g2 != nullptr);
  const ccord_channel *second = ccord_store_channel_at(F.store, g2 ? g2->id : 0, 1);
  CT_CHECK(second != nullptr);
  // Copy the ids before any upsert: the channel vector may reallocate and invalidate the pointers.
  ccord_id first_id = first ? first->id : CCORD_ID_NONE;
  ccord_id second_id = second ? second->id : CCORD_ID_NONE;
  ccord_error err;

  // A DM channel lives in the synthetic DM guild, which guild_count does not include.
  ccord_channel dm = {.id = 900001, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                      .name = "Ada", .topic = "", .user_id = ADA};
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm, &err) == CCORD_OK);

  uint32_t want_unread = 0, want_mentions = 0;
  if (first_id != CCORD_ID_NONE) {
    CT_CHECK(ccord_store_set_channel_counts(F.store, first_id, 3, 1, &err) == CCORD_OK);
    want_unread += 3;
    want_mentions += 1;
  }
  if (second_id != CCORD_ID_NONE) {
    CT_CHECK(ccord_store_set_channel_counts(F.store, second_id, 2, 0, &err) == CCORD_OK);
    want_unread += 2;
  }
  CT_CHECK(ccord_store_set_channel_counts(F.store, dm.id, 4, 2, &err) == CCORD_OK);
  want_unread += 4;
  want_mentions += 2;

  ccord_app_desktop_sync_badge(F.desktop);
  uint32_t unread = 0, mentions = 0;
  ccord_platform_unread_badge(F.platform, &unread, &mentions);
  CT_CHECK(unread == want_unread);
  CT_CHECK(mentions == want_mentions);

  // Unchanged revision: no recompute, no new platform write.
  uint64_t computed = dbg().badge_computations;
  ccord_app_desktop_sync_badge(F.desktop);
  CT_CHECK(dbg().badge_computations == computed);

  // Ephemeral mutations (typing, presence, speaking) do NOT bump badge_revision -> no badge recompute.
  if (first_id != CCORD_ID_NONE) {
    CT_CHECK(ccord_store_set_typing(F.store, first_id, ADA, 5000, &err) == CCORD_OK);
  }
  CT_CHECK(ccord_store_update_presence(F.store, ADA, CCORD_PRESENCE_IDLE, "away", &err) == CCORD_OK);
  ccord_store_set_speaking(F.store, ADA, true);
  ccord_store_set_speaking(F.store, ADA, false);
  ccord_app_desktop_sync_badge(F.desktop);
  CT_CHECK(dbg().badge_computations == computed);

  // A store mutation moves the revision and the badge follows.
  CT_CHECK(ccord_store_set_channel_counts(F.store, dm.id, 5, 0, &err) == CCORD_OK);
  ccord_app_desktop_sync_badge(F.desktop);
  ccord_platform_unread_badge(F.platform, &unread, &mentions);
  CT_CHECK(unread == want_unread + 1);
  CT_CHECK(mentions == 1);
  CT_CHECK(dbg().badge_computations == computed + 1);

  // Muted guild channel: unreads are excluded, mentions still count.
  if (first_id != CCORD_ID_NONE) {
    ccord_notify_scope mute_first = {.guild_id = HQ, .channel_id = first_id, .level = CCORD_NOTIFY_INHERIT, .muted = true};
    CT_CHECK(ccord_store_set_notify(F.store, HQ, &mute_first, 1, &err) == CCORD_OK);
    ccord_app_desktop_sync_badge(F.desktop);
    ccord_platform_unread_badge(F.platform, &unread, &mentions);
    CT_CHECK(unread == want_unread + 1 - 3);
    CT_CHECK(mentions == 1);
    CT_CHECK(ccord_store_set_notify(F.store, HQ, nullptr, 0, &err) == CCORD_OK);
  }

  // Message request & spam DMs never inflate unread or mentions.
  ccord_channel dm_req = {.id = 900002, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                          .name = "Req", .topic = "", .is_message_request = true};
  ccord_channel dm_spam = {.id = 900003, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                           .name = "Spam", .topic = "", .is_message_request = true, .is_spam = true};
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm_req, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm_spam, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, dm_req.id, 10, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, dm_spam.id, 7, 2, &err) == CCORD_OK);

  // Muted DM: ordinary unreads are excluded from unread & tray counts.
  ccord_notify_scope mute_dm = {.guild_id = CCORD_DM_GUILD_ID, .channel_id = dm.id, .level = CCORD_NOTIFY_INHERIT, .muted = true};
  CT_CHECK(ccord_store_set_notify(F.store, CCORD_DM_GUILD_ID, &mute_dm, 1, &err) == CCORD_OK);
  ccord_app_desktop_sync_badge(F.desktop);
  ccord_platform_unread_badge(F.platform, &unread, &mentions);
  CT_CHECK(unread == want_unread - 4);
  CT_CHECK(mentions == 1);

  // Restore clean state for subsequent tests.
  CT_CHECK(ccord_store_set_notify(F.store, CCORD_DM_GUILD_ID, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, dm.id, 0, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, dm_req.id, 0, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, dm_spam.id, 0, 0, &err) == CCORD_OK);
  if (first_id != CCORD_ID_NONE) CT_CHECK(ccord_store_set_channel_counts(F.store, first_id, 0, 0, &err) == CCORD_OK);
  if (second_id != CCORD_ID_NONE) CT_CHECK(ccord_store_set_channel_counts(F.store, second_id, 0, 0, &err) == CCORD_OK);
  ccord_app_desktop_sync_badge(F.desktop);
}

// ---------------------------------------------------------------- notification gating

static void test_notify_truth_table(void)
{
  ccord_notification n = {.guild_id = HQ, .channel_id = 0, .message_id = 1, .author_id = ADA,
                          .is_dm = false, .is_mention = true};
  const ccord_channel *ch = ccord_store_channel_at(F.store, HQ, 1);
  CT_CHECK(ch != nullptr);
  if (!ch) return;
  n.channel_id = ch->id;
  ccord_app_notify_prefs p = {.notify_desktop = true, .notify_sounds = true, .streamer_mode = false,
                              .stream_notifications_enabled = false, .focused = false,
                              .self_id = ccord_store_self_id(F.store)};
  int64_t now = 1793534400000LL;

  CT_CHECK(ccord_app_desktop_notify_eligible(&n, &p, F.store, now));
  p.focused = true;
  CT_CHECK(!ccord_app_desktop_notify_eligible(&n, &p, F.store, now));
  p.focused = false;

  p.self_id = ADA;   // self-authored
  CT_CHECK(!ccord_app_desktop_notify_eligible(&n, &p, F.store, now));
  p.self_id = ccord_store_self_id(F.store);

  p.notify_desktop = false;
  CT_CHECK(!ccord_app_desktop_notify_eligible(&n, &p, F.store, now));
  p.notify_desktop = true;

  p.streamer_mode = true;
  CT_CHECK(!ccord_app_desktop_notify_eligible(&n, &p, F.store, now));
  p.stream_notifications_enabled = true;
  CT_CHECK(ccord_app_desktop_notify_eligible(&n, &p, F.store, now));
  p.streamer_mode = false;
  p.stream_notifications_enabled = false;

  // A muted server suppresses; so does a muted channel row.
  ccord_error err;
  ccord_notify_scope guild_mute = {.guild_id = HQ, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_ALL,
                                   .muted = true};
  CT_CHECK(ccord_store_set_notify(F.store, HQ, &guild_mute, 1, &err) == CCORD_OK);
  CT_CHECK(!ccord_app_desktop_notify_eligible(&n, &p, F.store, now));
  // A notification with no channel (rare, but the guard exists) still sees the muted server.
  ccord_notification guild_only = n;
  guild_only.channel_id = CCORD_ID_NONE;
  CT_CHECK(!ccord_app_desktop_notify_eligible(&guild_only, &p, F.store, now));
  CT_CHECK(ccord_store_set_notify(F.store, HQ, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_app_desktop_notify_eligible(&n, &p, F.store, now));

  ccord_notify_scope chan_mute = {.guild_id = HQ, .channel_id = ch->id, .level = CCORD_NOTIFY_INHERIT,
                                  .muted = true};
  CT_CHECK(ccord_store_set_notify(F.store, HQ, &chan_mute, 1, &err) == CCORD_OK);
  CT_CHECK(!ccord_app_desktop_notify_eligible(&n, &p, F.store, now));
  CT_CHECK(ccord_store_set_notify(F.store, HQ, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_app_desktop_notify_eligible(&n, &p, F.store, now));

  // A blocked author never notifies; unblocking restores it.
  CT_CHECK(ccord_store_set_relationship(F.store, ADA, CCORD_REL_BLOCKED, &err) == CCORD_OK);
  CT_CHECK(!ccord_app_desktop_notify_eligible(&n, &p, F.store, now));
  CT_CHECK(ccord_store_set_relationship(F.store, ADA, CCORD_REL_NONE, &err) == CCORD_OK);
  CT_CHECK(ccord_app_desktop_notify_eligible(&n, &p, F.store, now));

  // Null notification/prefs and a null store (no mute data) are handled.
  CT_CHECK(!ccord_app_desktop_notify_eligible(nullptr, &p, F.store, now));
  CT_CHECK(!ccord_app_desktop_notify_eligible(&n, nullptr, F.store, now));
  CT_CHECK(ccord_app_desktop_notify_eligible(&n, &p, nullptr, now));
}

static void test_notify_delivery(void)
{
  const ccord_channel *ch = ccord_store_channel_at(F.store, HQ, 1);
  CT_CHECK(ch != nullptr);
  if (!ch) return;
  ccord_notification n = {.guild_id = HQ, .channel_id = ch->id, .message_id = 1, .author_id = ADA,
                          .is_dm = false, .is_mention = true};
  ccord_app_desktop_debug before = dbg();

  // Unfocused mention reaches the deterministic platform sink and requests the sound.
  ccord_platform_set_window_focused(F.platform, false);
  CT_CHECK(ccord_app_desktop_deliver(F.desktop, &n, 1793534400000LL));
  CT_CHECK(ccord_platform_notify_count(F.platform) == 1);
  CT_CHECK(ccord_platform_last_notify_title(F.platform)[0] != '\0');
  CT_CHECK(dbg().notifications_delivered == before.notifications_delivered + 1);
  CT_CHECK(dbg().beep_requests == before.beep_requests + 1);

  // A focused window is already showing the message: the app gate rejects it before preparation.
  ccord_platform_set_window_focused(F.platform, true);
  ccord_app_desktop_debug focused_before = dbg();
  CT_CHECK(!ccord_app_desktop_deliver(F.desktop, &n, 1793534400000LL));
  CT_CHECK(dbg().notifications_gated == focused_before.notifications_gated + 1);
  CT_CHECK(ccord_platform_notify_count(F.platform) == 1);
  CT_CHECK(dbg().notifications_delivered == before.notifications_delivered + 1);
  CT_CHECK(dbg().beep_requests == before.beep_requests + 1);
  CT_CHECK(dbg().notifications_suppressed >= 1);
  ccord_platform_set_window_focused(F.platform, false);

  // The pump drains the session queue under a virtual clock without delivering (deterministic shots).
  ccord_app_desktop_pump(F.desktop, true, 1000, 1793534400000LL);
  CT_CHECK(dbg().notifications_delivered == before.notifications_delivered + 1);
}

// Active OS notifications for the open DM channel clear automatically when the user pops the client
// back up and focuses it, and notifications for any channel clear when its read state advances elsewhere.
static void test_notify_auto_clear_on_focus_and_read(void)
{
  ccord_error err;
  ccord_id prev_guild = ccord_store_selected_guild(F.store);
  ccord_id prev_chan = ccord_store_selected_channel(F.store);
  // Clear any leftover entries from earlier notification delivery tests.
  const ccord_channel *prev_ch = ccord_store_channel_at(F.store, HQ, 1);
  if (prev_ch) (void)ccord_platform_close_channel_notifications(F.platform, prev_ch->id, 0);

  // Open a DM with Ada, select it, and actively view it at the bottom.
  CT_CHECK(ccord_session_open_dm(F.session, ADA, &err) == CCORD_OK);
  frame(6);
  ccord_id dm_ch = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, CCORD_DM_GUILD_ID); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, i);
    if (c && c->user_id == ADA) { dm_ch = c->id; break; }
  }
  CT_CHECK(dm_ch != CCORD_ID_NONE);
  if (dm_ch == CCORD_ID_NONE) return;
  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, dm_ch, &err) == CCORD_OK);
  ccord_platform_set_window_focused(F.platform, true);
  send_ev((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  frame(10);
  CT_CHECK(ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID && ccord_store_selected_channel(F.store) == dm_ch);

  // Unfocus / minimize while viewing that DM, then receive a new DM message and deliver its OS notification.
  send_ev((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = false});
  ccord_platform_set_window_focused(F.platform, false);
  ccord_id dm_msg = CCORD_ID_NONE;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, dm_ch, ADA, "ping while minimized", nullptr, &dm_msg, &err) ==
           CCORD_OK);
  frame(4);
  const ccord_channel *dc = ccord_store_channel(F.store, dm_ch);
  CT_CHECK(dc && dc->unread_count >= 1 && dc->mention_count >= 1);
  ccord_app_desktop_pump(F.desktop, false, 1000, 1793534400000LL);
  uint32_t dm_nid = ccord_platform_last_notify_id(F.platform);
  ccord_notify_target got = {0};
  CT_CHECK(dm_nid != 0 && ccord_platform_notify_target_for(F.platform, dm_nid, &got) && got.channel_id == dm_ch);
  // Still unfocused: notification must remain active.
  ccord_app_desktop_sync_notifications(F.desktop);
  CT_CHECK(ccord_platform_notify_target_for(F.platform, dm_nid, &got));

  // Pop the client back up / focus while still viewing that same DM: auto-ack + notification clear.
  ccord_app_desktop_debug before = dbg();
  ccord_platform_set_window_focused(F.platform, true);
  send_ev((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  frame(20);
  ccord_app_desktop_sync_badge(F.desktop);
  ccord_app_desktop_sync_notifications(F.desktop);
  dc = ccord_store_channel(F.store, dm_ch);
  CT_CHECK(dc && dc->unread_count == 0 && dc->mention_count == 0);
  CT_CHECK(!ccord_platform_notify_target_for(F.platform, dm_nid, &got));
  CT_CHECK(dbg().notifications_closed >= before.notifications_closed + 1);

  // Rule 2: notification in a non-selected channel clears even while unfocused once last_read_id advances.
  const ccord_channel *other = ccord_store_channel_at(F.store, HQ, 1);
  CT_CHECK(other != nullptr);
  if (other) {
    ccord_id other_id = other->id;
    ccord_id next_msg = other->last_read_id + 100u;
    send_ev((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = false});
    ccord_platform_set_window_focused(F.platform, false);
    ccord_notification on = {.guild_id = HQ, .channel_id = other_id, .message_id = next_msg, .author_id = ADA,
                             .is_dm = false, .is_mention = true};
    CT_CHECK(ccord_app_desktop_deliver(F.desktop, &on, 1793534401000LL));
    uint32_t onid = ccord_platform_last_notify_id(F.platform);
    CT_CHECK(ccord_platform_notify_target_for(F.platform, onid, &got) && got.channel_id == other_id);
    // Unread hasn't advanced yet: stays active.
    ccord_app_desktop_sync_notifications(F.desktop);
    CT_CHECK(ccord_platform_notify_target_for(F.platform, onid, &got));
    // Read on another device (or Mark as Read): last_read_id moves past next_msg -> notification dismissed.
    CT_CHECK(ccord_store_apply_read(F.store, other_id, next_msg, 0, &err) == CCORD_OK);
    ccord_app_desktop_sync_notifications(F.desktop);
    CT_CHECK(!ccord_platform_notify_target_for(F.platform, onid, &got));
  }

  // Restore focus and prior channel selection for subsequent tests.
  ccord_platform_set_window_focused(F.platform, true);
  send_ev((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  if (prev_guild != CCORD_ID_NONE) (void)ccord_session_select_guild(F.session, prev_guild, &err);
  if (prev_chan != CCORD_ID_NONE) (void)ccord_session_select_channel(F.session, prev_chan, &err);
  frame(4);
}

static int g_hook_calls;
static bool g_hook_mention;
static bool g_hook_handles = true;
static bool sound_hook(const ccord_notification *n, const ccord_ui *ui, const ccord_store *store, int64_t now)
{
  (void)ui;
  (void)store;
  (void)now;
  g_hook_calls++;
  g_hook_mention = n->is_mention;
  return g_hook_handles;
}

// With a sound hook the delivered notification asks the hook (once) instead of writing the legacy tone; a
// declining hook, or none, keeps the tone. The beep_requests contract is unchanged.
static void test_notify_sound_hook(void)
{
  const ccord_channel *ch = ccord_store_channel_at(F.store, HQ, 1);
  CT_CHECK(ch != nullptr);
  if (!ch) return;
  ccord_notification n = {.guild_id = HQ, .channel_id = ch->id, .message_id = 1, .author_id = ADA,
                          .is_dm = false, .is_mention = true};
  ccord_platform_set_window_focused(F.platform, false);

  ccord_app_desktop_debug b = dbg();
  CT_CHECK(ccord_app_desktop_deliver(F.desktop, &n, 1793534400000LL));   // no hook: the legacy tone
  CT_CHECK(dbg().beep_requests == b.beep_requests + 1);
  CT_CHECK(dbg().tone_requests == b.tone_requests + 1);

  ccord_app_desktop_set_sound_hook(F.desktop, sound_hook);
  b = dbg();
  g_hook_calls = 0;
  CT_CHECK(ccord_app_desktop_deliver(F.desktop, &n, 1793534400000LL));
  CT_CHECK(g_hook_calls == 1 && g_hook_mention);
  CT_CHECK(dbg().beep_requests == b.beep_requests + 1);
  CT_CHECK(dbg().tone_requests == b.tone_requests);

  g_hook_handles = false;   // a declining hook falls back to the tone
  b = dbg();
  CT_CHECK(ccord_app_desktop_deliver(F.desktop, &n, 1793534400000LL));
  CT_CHECK(dbg().tone_requests == b.tone_requests + 1);
  g_hook_handles = true;

  n.author_id = ccord_store_self_id(F.store);   // gated notifications never reach the hook
  g_hook_calls = 0;
  CT_CHECK(!ccord_app_desktop_deliver(F.desktop, &n, 1793534400000LL));
  CT_CHECK(g_hook_calls == 0);
  ccord_app_desktop_set_sound_hook(F.desktop, nullptr);
}

// ---------------------------------------------------------------- global shortcuts

static void test_hotkeys(void)
{
  ccord_error err;
  CT_CHECK(!ccord_app_desktop_handle_hotkey(F.desktop, "toggle-mute", &err));      // not in voice
  CT_CHECK(!ccord_app_desktop_handle_hotkey(F.desktop, "toggle-deafen", &err));
  CT_CHECK(!ccord_app_desktop_handle_hotkey(F.desktop, "unknown-shortcut", &err));
  // Portal push-to-talk is held state, not a toggle: Activated sets, Deactivated and a lost session clear.
  CT_CHECK(!ccord_platform_global_ptt_held(F.platform));
  CT_CHECK(ccord_platform_inject_hotkey(F.platform, CCORD_SHORTCUT_PUSH_TO_TALK, &err) == CCORD_OK);
  CT_CHECK(ccord_platform_global_ptt_held(F.platform));
  CT_CHECK(!ccord_app_desktop_handle_hotkey(F.desktop, CCORD_SHORTCUT_PUSH_TO_TALK, &err));
  CT_CHECK(ccord_platform_inject_shortcut_deactivated(F.platform, CCORD_SHORTCUT_PUSH_TO_TALK, &err) == CCORD_OK);
  CT_CHECK(!ccord_platform_global_ptt_held(F.platform));
  CT_CHECK(ccord_platform_inject_hotkey(F.platform, CCORD_SHORTCUT_PUSH_TO_TALK, &err) == CCORD_OK);
  ccord_platform_inject_portal_session_closed(F.platform);
  CT_CHECK(!ccord_platform_global_ptt_held(F.platform));
  ccord_app_desktop_sync_ptt_shortcut(F.desktop);   // no portal bound in this fixture: a harmless no-op
  CT_CHECK(ccord_store_active_voice_channel(F.store) == CCORD_ID_NONE);

  CT_CHECK(ccord_session_join_voice(F.session, HQ, HQ_VOICE, &err) == CCORD_OK);
  frame(3);
  ccord_id self = ccord_store_self_id(F.store);
  const ccord_voice_state *vs = ccord_store_voice_user(F.store, HQ, self);
  CT_CHECK(vs && vs->channel_id == HQ_VOICE);
  CT_CHECK(vs && !vs->self_mute && !vs->self_deaf);
  if (!vs) return;

  // toggle-mute flips the session self-mute flag (acknowledged by the mock transport).
  CT_CHECK(ccord_app_desktop_handle_hotkey(F.desktop, "toggle-mute", &err));
  frame(2);
  vs = ccord_store_voice_user(F.store, HQ, self);
  CT_CHECK(vs && vs->self_mute);
  CT_CHECK(ccord_app_desktop_handle_hotkey(F.desktop, "toggle-mute", &err));
  frame(2);
  vs = ccord_store_voice_user(F.store, HQ, self);
  CT_CHECK(vs && !vs->self_mute);

  // toggle-deafen flips self-deafen (and Discord's implied mute), then turns deafen back off.
  CT_CHECK(ccord_app_desktop_handle_hotkey(F.desktop, "toggle-deafen", &err));
  frame(2);
  vs = ccord_store_voice_user(F.store, HQ, self);
  CT_CHECK(vs && vs->self_deaf && vs->self_mute);
  CT_CHECK(ccord_app_desktop_handle_hotkey(F.desktop, "toggle-deafen", &err));
  frame(2);
  vs = ccord_store_voice_user(F.store, HQ, self);
  CT_CHECK(vs && !vs->self_deaf);

  CT_CHECK(ccord_session_leave_voice(F.session, HQ, &err) == CCORD_OK);
  frame(3);
}

// ---------------------------------------------------------------- live QuickCSS (desktop glue)

static void test_quick_css_desktop(void)
{
  ccord_color base = F.theme.c.bg_primary;
  uint64_t applies = dbg().quick_css_applies;

  // Enabled snippet overlays the base theme live and asks for one redraw.
  ccord_app_desktop_apply_quick_css(F.desktop, true, ":root { --background-primary: #123456; }");
  CT_CHECK(color_is(F.theme.c.bg_primary, 0x12, 0x34, 0x56));
  CT_CHECK(dbg().quick_css_applies == applies + 1);
  CT_CHECK(ccord_app_desktop_take_redraw(F.desktop));

  // Disabling restores the pristine base (not the previously overlaid color).
  ccord_app_desktop_apply_quick_css(F.desktop, false, ":root { --background-primary: #123456; }");
  CT_CHECK(color_is(F.theme.c.bg_primary, base.r, base.g, base.b));
  CT_CHECK(dbg().quick_css_applies == applies + 1);   // restore alone is not an apply

  // Editing the snippet re-applies immediately.
  ccord_app_desktop_apply_quick_css(F.desktop, true, "--background-primary: #654321;");
  CT_CHECK(color_is(F.theme.c.bg_primary, 0x65, 0x43, 0x21));
  CT_CHECK(dbg().quick_css_applies == applies + 2);

  // A base theme swap keeps the remembered snippet on top of the new base.
  host_apply_theme(F.desktop, "c-cord-light");
  CT_CHECK(strcmp(F.theme.id, "c-cord-light") == 0);
  CT_CHECK(color_is(F.theme.c.bg_primary, 0x65, 0x43, 0x21));
  CT_CHECK(dbg().quick_css_applies == applies + 3);
  ccord_app_desktop_apply_quick_css(F.desktop, false, "");
  CT_CHECK(!color_is(F.theme.c.bg_primary, 0x65, 0x43, 0x21));   // light base, snippet gone
  ccord_color light_base = F.theme.c.bg_primary;

  // Malformed snippets never break the base theme (the call is counted, nothing is applied).
  ccord_app_desktop_apply_quick_css(F.desktop, true, "garbage {{{ --nope:");
  CT_CHECK(color_is(F.theme.c.bg_primary, light_base.r, light_base.g, light_base.b));
  CT_CHECK(dbg().quick_css_applies == applies + 4);

  // Restore the fixture's dark base for the following tests.
  ccord_app_desktop_apply_quick_css(F.desktop, false, "");
  host_apply_theme(F.desktop, "c-cord-dark");
  CT_CHECK(color_is(F.theme.c.bg_primary, base.r, base.g, base.b));
}

// ---------------------------------------------------------------- QuickCSS in Settings > Appearance

static void test_quick_css_ui(void)
{
  goto_page(2);   // Appearance
  CT_CHECK(!sdbg().quickcss_enabled);
  uint64_t applies = dbg().quick_css_applies;

  // The toggle applies live through the host (empty snippet: base unchanged).
  click_rect(sdbg().quickcss_toggle);
  CT_CHECK(sdbg().quickcss_enabled);
  CT_CHECK(s_css_enabled_last);

  // Clicking the snippet box focuses the editor; typing applies every edit immediately.
  // (The editor starts with the "/* Custom Theme Overrides */" placeholder: select-all first.)
  click_rect(sdbg().quickcss_box);
  CT_CHECK(sdbg().quickcss_focused);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  text("--background-primary: #0a0b0c;");
  CT_CHECK(color_is(F.theme.c.bg_primary, 0x0a, 0x0b, 0x0c));
  CT_CHECK(dbg().quick_css_applies > applies);
  CT_CHECK(strcmp(s_css_last, "--background-primary: #0a0b0c;") == 0);

  // Backspace edits re-apply too (the trailing ';' is optional for the last declaration).
  key(CCORD_KEY_BACKSPACE, 0);
  CT_CHECK(color_is(F.theme.c.bg_primary, 0x0a, 0x0b, 0x0c));
  CT_CHECK(strcmp(s_css_last, "--background-primary: #0a0b0c") == 0);

  // Ctrl+A + replacement text swaps the whole snippet.
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  text("--background-primary: #332211;");
  CT_CHECK(color_is(F.theme.c.bg_primary, 0x33, 0x22, 0x11));

  // The edit persisted to settings.ini.
  const char *ini = read_file("app_desktop_data/settings.ini");
  CT_CHECK(strstr(ini, "quickcss_enabled = 1") != nullptr);
  CT_CHECK(strstr(ini, "quickcss = --background-primary: #332211;") != nullptr);

  // Clicking away commits and unfocuses; the toggle off restores the base theme.
  ccord_rect box = sdbg().quickcss_box;
  click_raw(box.x + box.w * 0.5f, box.y - 40.0f);
  frame(1);
  CT_CHECK(!sdbg().quickcss_focused);
  ccord_color base = F.theme.c.bg_primary;   // currently overlaid
  click_rect(sdbg().quickcss_toggle);
  CT_CHECK(!sdbg().quickcss_enabled);
  CT_CHECK(!color_is(F.theme.c.bg_primary, base.r, base.g, base.b));
  close_settings();
}

// ---------------------------------------------------------------- Close-to-Tray (Settings > Advanced)

static void push_window_event(Uint32 type)
{
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = type;
  e.window.windowID = SDL_GetWindowID((SDL_Window *)ccord_platform_sdl_window(F.platform));
  (void)SDL_PushEvent(&e);
}

static bool drain_saw_quit(void)
{
  ccord_input_event ev;
  bool quit = false;
  while (ccord_platform_next_event(F.platform, &ev, 0)) {
    if (ev.type == CCORD_IN_QUIT) quit = true;
  }
  return quit;
}

static void test_close_to_tray(void)
{
  goto_advanced();
  ccord_rect toggle = sdbg().close_to_tray_toggle;
  CT_CHECK(toggle.w > 0.0f);
  CT_CHECK(sdbg().close_to_tray);   // default ON
  click_rect(toggle);               // off first, so the rest of the test exercises the off -> on path
  CT_CHECK(!sdbg().close_to_tray);
  int calls = s_tray_calls;

  // The Advanced toggle drives the platform through the host callback and persists.
  click_rect(toggle);
  CT_CHECK(sdbg().close_to_tray);
  CT_CHECK(s_tray_calls == calls + 1 && s_tray_last);
  CT_CHECK(ccord_platform_close_to_tray(F.platform));
  CT_CHECK(strstr(read_file("app_desktop_data/settings.ini"), "close_to_tray = 1") != nullptr);

  ccord_platform_set_tray_override(F.platform, 1);
  // A window-manager close request now hides to the tray instead of emitting quit.
  ccord_platform_set_window_visible(F.platform, true);
  (void)drain_saw_quit();
  push_window_event(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
  CT_CHECK(!drain_saw_quit());
  CT_CHECK(!ccord_platform_window_visible(F.platform));

  // The tray/IPC "show" path restores the window; SDL_EVENT_QUIT still quits.
  ccord_platform_set_window_visible(F.platform, true);
  CT_CHECK(ccord_platform_window_visible(F.platform));
  (void)drain_saw_quit();
  SDL_Event q;
  memset(&q, 0, sizeof q);
  q.type = SDL_EVENT_QUIT;
  (void)SDL_PushEvent(&q);
  CT_CHECK(drain_saw_quit());

  // Toggle off: close requests quit again (legacy behavior).
  goto_advanced();
  click_rect(sdbg().close_to_tray_toggle);
  CT_CHECK(!sdbg().close_to_tray);
  CT_CHECK(!ccord_platform_close_to_tray(F.platform));
  ccord_platform_set_window_visible(F.platform, true);
  (void)drain_saw_quit();
  push_window_event(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
  CT_CHECK(drain_saw_quit());
  close_settings();
}

// ---------------------------------------------------------------- rebindable keybinds

// The mock transport keeps self_mute/self_deaf across leave/join, and undeafen keeps the implied
// mute flag; trigger/sync tests normalize to an unmuted/undeafened baseline after joining.
static void voice_clear_flags(void)
{
  ccord_error err;
  ccord_id self = ccord_store_self_id(F.store);
  const ccord_voice_state *vs = ccord_store_voice_user(F.store, HQ, self);
  if (vs && vs->self_deaf) {
    CT_CHECK(ccord_app_desktop_handle_hotkey(F.desktop, "toggle-deafen", &err));
    frame(2);
  }
  vs = ccord_store_voice_user(F.store, HQ, self);
  if (vs && vs->self_mute) {
    CT_CHECK(ccord_app_desktop_handle_hotkey(F.desktop, "toggle-mute", &err));
    frame(2);
  }
}

static void test_keybinds(void)
{
  // Factory defaults: Ctrl+Shift+M / Ctrl+Shift+D / Ctrl+K; portal trigger formatting.
  ccord_ui_keybind kb[CCORD_UI_KEYBIND_COUNT];
  ccord_ui_get_keybinds(F.ui, kb);
  CT_CHECK(kb[CCORD_UI_KEYBIND_MUTE].scancode == SDL_SCANCODE_M);
  CT_CHECK(kb[CCORD_UI_KEYBIND_MUTE].mods == (CCORD_MOD_CTRL | CCORD_MOD_SHIFT));
  CT_CHECK(kb[CCORD_UI_KEYBIND_DEAFEN].scancode == SDL_SCANCODE_D);
  CT_CHECK(kb[CCORD_UI_KEYBIND_QUICK_SWITCHER].scancode == SDL_SCANCODE_K);
  CT_CHECK(kb[CCORD_UI_KEYBIND_QUICK_SWITCHER].mods == CCORD_MOD_CTRL);
  char trigger[48];
  ccord_ui_keybind_trigger(&kb[CCORD_UI_KEYBIND_MUTE], trigger, sizeof trigger);
  CT_CHECK(strcmp(trigger, "CTRL+SHIFT+M") == 0);

  // Capture a new Mute chord on the Keybinds page: click the row, press Ctrl+Alt+T.
  goto_page(7);   // Keybinds
  CT_CHECK(sdbg().keybind_capture == -1);
  click_rect(sdbg().keybind_rows[1]);   // row 1 = Mute
  CT_CHECK(sdbg().keybind_capture == 1);
  int kb_calls = s_kb_calls;
  key(CCORD_KEY_T, CCORD_MOD_CTRL | CCORD_MOD_ALT);
  frame(1);
  CT_CHECK(sdbg().keybind_capture == -1);
  CT_CHECK(s_kb_calls == kb_calls + 1);   // host re-registers the portal shortcuts
  ccord_ui_get_keybinds(F.ui, kb);
  CT_CHECK(kb[CCORD_UI_KEYBIND_MUTE].scancode == SDL_SCANCODE_T);
  CT_CHECK(kb[CCORD_UI_KEYBIND_MUTE].mods == (CCORD_MOD_CTRL | CCORD_MOD_ALT));
  ccord_ui_keybind_trigger(&kb[CCORD_UI_KEYBIND_MUTE], trigger, sizeof trigger);
  CT_CHECK(strcmp(trigger, "CTRL+ALT+T") == 0);

  // The capture persisted as "scancode,mods".
  char want[64];
  snprintf(want, sizeof want, "keybind_mute = %d,%d", SDL_SCANCODE_T, CCORD_MOD_CTRL | CCORD_MOD_ALT);
  CT_CHECK(strstr(read_file("app_desktop_data/settings.ini"), want) != nullptr);

  // The new chord toggles voice mute; the old default no longer does.
  close_settings();
  ccord_error err;
  CT_CHECK(ccord_session_join_voice(F.session, HQ, HQ_VOICE, &err) == CCORD_OK);
  frame(3);
  voice_clear_flags();
  ccord_id self = ccord_store_self_id(F.store);
  key(CCORD_KEY_M, CCORD_MOD_CTRL | CCORD_MOD_SHIFT);   // old default: ignored now
  frame(2);
  const ccord_voice_state *vs = ccord_store_voice_user(F.store, HQ, self);
  CT_CHECK(vs && !vs->self_mute);
  key(CCORD_KEY_T, CCORD_MOD_CTRL | CCORD_MOD_ALT);
  frame(2);
  vs = ccord_store_voice_user(F.store, HQ, self);
  CT_CHECK(vs && vs->self_mute);
  key(CCORD_KEY_T, CCORD_MOD_CTRL | CCORD_MOD_ALT);
  frame(2);
  vs = ccord_store_voice_user(F.store, HQ, self);
  CT_CHECK(vs && !vs->self_mute);

  // Default Deafen chord still works (Ctrl+Shift+D implies mute).
  key(CCORD_KEY_D, CCORD_MOD_CTRL | CCORD_MOD_SHIFT);
  frame(2);
  vs = ccord_store_voice_user(F.store, HQ, self);
  CT_CHECK(vs && vs->self_deaf && vs->self_mute);
  key(CCORD_KEY_D, CCORD_MOD_CTRL | CCORD_MOD_SHIFT);
  frame(2);
  vs = ccord_store_voice_user(F.store, HQ, self);
  CT_CHECK(vs && !vs->self_deaf);
  voice_clear_flags();   // undeafen keeps the implied mute flag; start the next section clean

  // Quick Switcher: default Ctrl+K opens it, Escape closes it; rebinding frees Ctrl+K.
  key(CCORD_KEY_K, CCORD_MOD_CTRL);
  CT_CHECK(udbg().quick_switcher_open);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!udbg().quick_switcher_open);
  goto_page(7);
  click_rect(sdbg().keybind_rows[3]);   // row 3 = Quick Switcher
  CT_CHECK(sdbg().keybind_capture == 3);
  key(CCORD_KEY_K, CCORD_MOD_CTRL | CCORD_MOD_SHIFT);
  frame(1);
  close_settings();
  key(CCORD_KEY_K, CCORD_MOD_CTRL);
  CT_CHECK(!udbg().quick_switcher_open);
  key(CCORD_KEY_K, CCORD_MOD_CTRL | CCORD_MOD_SHIFT);
  CT_CHECK(udbg().quick_switcher_open);
  key(CCORD_KEY_ESCAPE, 0);   // closes the switcher overlay
  CT_CHECK(!udbg().quick_switcher_open);

  // A bare-key bind never steals composer typing.
  goto_page(7);
  click_rect(sdbg().keybind_rows[1]);
  key(CCORD_KEY_B, 0);
  frame(1);
  close_settings();
  text("hello");
  key(CCORD_KEY_B, 0);
  frame(2);
  vs = ccord_store_voice_user(F.store, HQ, self);
  CT_CHECK(vs && !vs->self_mute);   // composer held text: the bare bind stayed inert
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);      // clear the composer
  key(CCORD_KEY_B, 0);
  frame(2);
  vs = ccord_store_voice_user(F.store, HQ, self);
  CT_CHECK(vs && vs->self_mute);    // empty composer: the bare bind fires

  // Restore the factory Mute bind for the remaining tests.
  goto_page(7);
  click_rect(sdbg().keybind_rows[1]);
  key(CCORD_KEY_M, CCORD_MOD_CTRL | CCORD_MOD_SHIFT);
  frame(1);
  close_settings();
  CT_CHECK(ccord_session_leave_voice(F.session, HQ, &err) == CCORD_OK);
  frame(3);
}

// ---------------------------------------------------------------- saved-account switcher UI

static void test_saved_accounts_ui(void)
{
  ccord_ui_saved_account list[3] = {
    {.user_id = 11, .display_name = "Alpha", .username = "alpha", .active = true},
    {.user_id = 22, .display_name = "Beta", .username = "beta", .active = false},
    {.user_id = 33, .display_name = "Gamma", .username = "gamma", .active = false},
  };
  ccord_ui_set_saved_accounts(F.ui, list, 3);
  goto_page(0);   // My Account
  ccord_ui_settings_debug s = sdbg();
  CT_CHECK(s.saved_account_count == 3);
  CT_CHECK(s.saved_account_ids[0] == 11 && s.saved_account_ids[1] == 22 && s.saved_account_ids[2] == 33);

  // Switch/Remove dispatch the row's user id to the app host (keyring + session live there).
  int sw = s_switch_calls, rm = s_remove_calls;
  click_rect(s.saved_account_switch[1]);
  CT_CHECK(s_switch_calls == sw + 1 && s_switch_id == 22);
  click_rect(s.saved_account_remove[2]);
  CT_CHECK(s_remove_calls == rm + 1 && s_remove_id == 33);
  // The active row has no Switch action.
  click_rect(s.saved_account_switch[0]);
  CT_CHECK(s_switch_calls == sw + 1);

  // Add Account focuses the token box for a new sign-in.
  click_rect(s.account_add_btn);
  text("new-token");
  CT_CHECK(strcmp(ccord_ui_account_token_text(F.ui), "new-token") == 0);
  ccord_ui_clear_account_token_text(F.ui);
  key(CCORD_KEY_ESCAPE, 0);   // leave the token field
  close_settings();
  ccord_ui_set_saved_accounts(F.ui, nullptr, 0);
}

// ---------------------------------------------------------------- tray voice-state sync

static void test_voice_state_sync(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_join_voice(F.session, HQ, HQ_VOICE, &err) == CCORD_OK);
  frame(3);
  voice_clear_flags();
  ccord_app_desktop_pump(F.desktop, true, 1000, 1793534400000LL);   // sync the clean baseline
  uint64_t pushes = dbg().voice_state_pushes;
  CT_CHECK(!dbg().last_voice_muted && !dbg().last_voice_deafened);
  ccord_app_desktop_pump(F.desktop, true, 1000, 1793534400000LL);
  CT_CHECK(dbg().voice_state_pushes == pushes);   // no change: no platform write

  CT_CHECK(ccord_app_desktop_handle_hotkey(F.desktop, "toggle-mute", &err));
  frame(2);
  ccord_app_desktop_pump(F.desktop, true, 1000, 1793534400000LL);
  CT_CHECK(dbg().voice_state_pushes == pushes + 1);
  CT_CHECK(dbg().last_voice_muted && !dbg().last_voice_deafened);
  ccord_app_desktop_pump(F.desktop, true, 1000, 1793534400000LL);
  CT_CHECK(dbg().voice_state_pushes == pushes + 1);   // no change: no write

  CT_CHECK(ccord_app_desktop_handle_hotkey(F.desktop, "toggle-deafen", &err));
  frame(2);
  ccord_app_desktop_pump(F.desktop, true, 1000, 1793534400000LL);
  CT_CHECK(dbg().voice_state_pushes == pushes + 2);
  CT_CHECK(dbg().last_voice_muted && dbg().last_voice_deafened);   // deafen implies mute

  CT_CHECK(ccord_session_leave_voice(F.session, HQ, &err) == CCORD_OK);
  frame(3);
  ccord_app_desktop_pump(F.desktop, true, 1000, 1793534400000LL);
  CT_CHECK(dbg().voice_state_pushes == pushes + 3);
  CT_CHECK(!dbg().last_voice_muted && !dbg().last_voice_deafened);
}

// ---------------------------------------------------------------- single-instance control socket

static void test_ipc(void)
{
  char buf[PATH_MAX], want[PATH_MAX];
  ccord_app_ipc_resolve_path(buf, sizeof buf, "override.sock");
  CT_CHECK(strcmp(buf, "override.sock") == 0);

  // Socket directory: $TMPDIR when set, else the working directory (never a hard-coded path).
  char dir[PATH_MAX / 2];
  const char *tmp = getenv("TMPDIR");
  snprintf(dir, sizeof dir, "%s/ccord_ipc_XXXXXX", (tmp && tmp[0]) ? tmp : ".");
  CT_CHECK(mkdtemp(dir) != nullptr);
  char sock[PATH_MAX];
  snprintf(sock, sizeof sock, "%s/control.sock", dir);

  // Resolution order: override > $XDG_RUNTIME_DIR/ccord > $TMPDIR (env saved/restored).
  char *old_runtime = getenv("XDG_RUNTIME_DIR") ? strdup(getenv("XDG_RUNTIME_DIR")) : nullptr;
  char *old_tmpdir = getenv("TMPDIR") ? strdup(getenv("TMPDIR")) : nullptr;
  CT_CHECK(setenv("XDG_RUNTIME_DIR", dir, 1) == 0);
  ccord_app_ipc_resolve_path(buf, sizeof buf, nullptr);
  snprintf(want, sizeof want, "%s/ccord/control.sock", dir);
  CT_CHECK(strcmp(buf, want) == 0);
  unsetenv("XDG_RUNTIME_DIR");
  CT_CHECK(setenv("TMPDIR", dir, 1) == 0);
  ccord_app_ipc_resolve_path(buf, sizeof buf, nullptr);
  snprintf(want, sizeof want, "%s/ccord-control-%ld.sock", dir, (long)getuid());
  CT_CHECK(strcmp(buf, want) == 0);
  if (old_runtime) (void)setenv("XDG_RUNTIME_DIR", old_runtime, 1); else unsetenv("XDG_RUNTIME_DIR");
  if (old_tmpdir) (void)setenv("TMPDIR", old_tmpdir, 1); else unsetenv("TMPDIR");
  free(old_runtime);
  free(old_tmpdir);

  // Listener: 0600 socket node; a second live listener is rejected.
  CT_CHECK(ccord_app_ipc_wait_fd(nullptr) == CCORD_INVALID_SOCKET);
  ccord_error err;
  ccord_app_ipc *ipc = ccord_app_ipc_listen(sock, &err);
  CT_CHECK(ipc != nullptr);
  if (!ipc) { remove(sock); rmdir(dir); return; }
  CT_CHECK(ccord_app_ipc_wait_fd(ipc) != CCORD_INVALID_SOCKET);
  CT_CHECK(strcmp(ccord_app_ipc_path(ipc), sock) == 0);
  struct stat st;
  CT_CHECK(stat(sock, &st) == 0);
  CT_CHECK((st.st_mode & 0777) == 0600);
  ccord_error err2;
  CT_CHECK(ccord_app_ipc_listen(sock, &err2) == nullptr);
  CT_CHECK(err2.code == CCORD_ERR_PROTOCOL);

  // Malformed commands are rejected before any socket traffic.
  CT_CHECK(!ccord_app_ipc_send(sock, "bad cmd", &err2));
  CT_CHECK(!ccord_app_ipc_send(sock, "", &err2));

  // A forked client sends toggle-mic/toggle-deafen/open-url; the server polls the command and acks.
  const char *cmds[3] = {"toggle-mic", "toggle-deafen", "open-url https://discord.gg/c23-study"};
  for (int c = 0; c < 3; c++) {
    pid_t pid = fork();
    CT_CHECK(pid >= 0);
    if (pid == 0) {
      ccord_error cerr;
      bool ok = ccord_app_ipc_send(sock, cmds[c], &cerr);
      _exit(ok ? 0 : 1);   // child: no stdio flush, no SDL teardown
    }
    char cmd[256] = {0};
    bool got = false;
    for (int i = 0; i < 300 && !got; i++) {
      got = ccord_app_ipc_poll(ipc, cmd, sizeof cmd);
      if (!got) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
        (void)nanosleep(&ts, nullptr);
      }
    }
    CT_CHECK(got);
    CT_CHECK(strcmp(cmd, cmds[c]) == 0);
    int status = 0;
    CT_CHECK(waitpid(pid, &status, 0) == pid);
    CT_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);   // client received ipc_ack=<cmd>
  }

  // Secondary launch handoff: a second ccord_app_ipc_listen leaves an empty probe in the backlog,
  // then ccord_app_ipc_handoff sends "show" and ONE ccord_app_ipc_poll drains the probe and acks "show".
  CT_CHECK(ccord_app_ipc_listen(sock, &err2) == nullptr);
  CT_CHECK(err2.code == CCORD_ERR_PROTOCOL);
  {
    pid_t pid = fork();
    CT_CHECK(pid >= 0);
    if (pid == 0) {
      ccord_error cerr;
      bool ok = ccord_app_ipc_handoff(sock, &cerr);
      _exit(ok ? 0 : 1);
    }
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 25 * 1000 * 1000};
    (void)nanosleep(&ts, nullptr);   // let child connect + write "show\n" behind the empty probe
    char cmd[32] = {0};
    CT_CHECK(ccord_app_ipc_poll(ipc, cmd, sizeof cmd));   // single poll drains probe + reads "show"
    CT_CHECK(strcmp(cmd, "show") == 0);
    int status = 0;
    CT_CHECK(waitpid(pid, &status, 0) == pid);
    CT_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }

  // Destroy closes and unlinks; sending afterwards reports "no running instance".
  ccord_app_ipc_destroy(ipc);
  CT_CHECK(stat(sock, &st) != 0);
  CT_CHECK(!ccord_app_ipc_send(sock, "show", &err2));
  CT_CHECK(err2.code == CCORD_ERR_NOT_FOUND);

  // A regular file at the socket path must NOT be unlinked.
  CT_CHECK(write_text(sock, "not-a-socket"));
  CT_CHECK(ccord_app_ipc_listen(sock, &err) == nullptr);
  CT_CHECK(stat(sock, &st) == 0 && S_ISREG(st.st_mode));
  remove(sock);

  // A stale socket file (crashed instance, no listener) is recovered and re-bound.
  {
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    CT_CHECK(sfd >= 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    size_t slen = strlen(sock);
    CT_CHECK(slen < sizeof sa.sun_path);
    if (slen < sizeof sa.sun_path) memcpy(sa.sun_path, sock, slen + 1u);
    CT_CHECK(bind(sfd, (struct sockaddr *)&sa, sizeof sa) == 0);
    close(sfd);
  }
  ipc = ccord_app_ipc_listen(sock, &err);
  CT_CHECK(ipc != nullptr);
  if (ipc) ccord_app_ipc_destroy(ipc);
  remove(sock);
  rmdir(dir);
}

static int g_wake_hook_calls;
static bool g_wake_hook_last_call;
static void test_wake_hook_cb(void *ud, bool is_call)
{
  (void)ud;
  g_wake_hook_calls++;
  g_wake_hook_last_call = is_call;
}

static void test_notify_wake_hook(void)
{
  const ccord_channel *ch = ccord_store_channel_at(F.store, HQ, 1);
  CT_CHECK(ch != nullptr);
  if (!ch) return;

  g_wake_hook_calls = 0;
  g_wake_hook_last_call = true;
  ccord_app_desktop_set_wake_hook(F.desktop, test_wake_hook_cb, nullptr);

  ccord_notification n = {.guild_id = HQ, .channel_id = ch->id, .message_id = 990001u,
                          .author_id = ADA, .is_dm = false, .is_mention = true};

  // Unfocused with desktop notifications enabled -> delivers notification and fires wake hook.
  ccord_platform_set_window_focused(F.platform, false);
  CT_CHECK(ccord_app_desktop_deliver(F.desktop, &n, 1793534400000LL));
  CT_CHECK(g_wake_hook_calls == 1 && !g_wake_hook_last_call);

  // Focused -> suppressed, wake hook must NOT fire.
  ccord_platform_set_window_focused(F.platform, true);
  CT_CHECK(!ccord_app_desktop_deliver(F.desktop, &n, 1793534400000LL));
  CT_CHECK(g_wake_hook_calls == 1);

  // Unfocused with local Desktop Notifications toggled OFF -> local popup suppressed,
  // but Companion Wake MUST still fire so the mobile companion wakes up!
  ccord_platform_set_window_focused(F.platform, false);
  CT_CHECK(write_text("app_desktop_data/settings.ini", "[ui]\nnotify_desktop = 0\n"));
  CT_CHECK(recreate_ui());
  CT_CHECK(!sdbg().notify_desktop);

  CT_CHECK(!ccord_app_desktop_deliver(F.desktop, &n, 1793534400000LL));
  CT_CHECK(g_wake_hook_calls == 2 && !g_wake_hook_last_call);

  // Self-authored message must still NOT fire wake hook even with desktop notifications off.
  ccord_notification self_n = n;
  self_n.author_id = ccord_store_self_id(F.store);
  CT_CHECK(!ccord_app_desktop_deliver(F.desktop, &self_n, 1793534400000LL));
  CT_CHECK(g_wake_hook_calls == 2);

  // Restore desktop notifications ON.
  remove("app_desktop_data/settings.ini");
  CT_CHECK(recreate_ui());
  CT_CHECK(sdbg().notify_desktop);

  // Expired timed mute (mute_until_ms in the past) must NOT suppress delivery or Companion Wake
  // when ccord_app_desktop_pump() runs with a small monotonic clock alongside real Unix epoch ms.
  ccord_error err;
  ccord_notify_scope expired_mute = {.guild_id = HQ, .channel_id = ch->id, .level = CCORD_NOTIFY_INHERIT,
                                     .muted = true, .mute_until_ms = 1700000000000LL};
  CT_CHECK(ccord_store_set_notify(F.store, HQ, &expired_mute, 1, &err) == CCORD_OK);
  ccord_platform_set_window_focused(F.platform, false);
  CT_CHECK(ccord_app_desktop_deliver_at(F.desktop, &n, 1793534400000LL, 50000));
  CT_CHECK(g_wake_hook_calls == 3 && !g_wake_hook_last_call);

  // Active future timed mute MUST still suppress both delivery and Companion Wake.
  ccord_notify_scope active_mute = {.guild_id = HQ, .channel_id = ch->id, .level = CCORD_NOTIFY_INHERIT,
                                    .muted = true, .mute_until_ms = 1793534460000LL};
  CT_CHECK(ccord_store_set_notify(F.store, HQ, &active_mute, 1, &err) == CCORD_OK);
  CT_CHECK(!ccord_app_desktop_deliver_at(F.desktop, &n, 1793534400000LL, 50000));
  CT_CHECK(g_wake_hook_calls == 3);

  CT_CHECK(ccord_store_set_notify(F.store, HQ, nullptr, 0, &err) == CCORD_OK);
  ccord_app_desktop_set_wake_hook(F.desktop, nullptr, nullptr);
  (void)ccord_platform_close_channel_notifications(F.platform, ch->id, 0);
}

static void test_zoom_event_scaling(void)
{
  const float zooms[] = {1.5f, 0.8f, 1.0f, 0.0f, -1.0f};
  for (size_t zi = 0; zi < sizeof zooms / sizeof zooms[0]; zi++) {
    float z = zooms[zi];
    float div = (z > 0.0f) ? z : 1.0f;

    const ccord_input_type xy_types[] = {
      CCORD_IN_MOUSE_DOWN, CCORD_IN_MOUSE_MOVE, CCORD_IN_MOUSE_UP,
      CCORD_IN_TOUCH_DOWN, CCORD_IN_TOUCH_MOVE, CCORD_IN_TOUCH_UP, CCORD_IN_TOUCH_CANCEL,
      CCORD_IN_RESIZE,
      (ccord_input_type)CCORD_EV_DROP_POSITION, (ccord_input_type)CCORD_EV_DROP_FILE,
    };
    for (size_t ti = 0; ti < sizeof xy_types / sizeof xy_types[0]; ti++) {
      ccord_input_event ev = {.type = xy_types[ti], .x = 300.0f, .y = 600.0f};
      ccord_app_desktop_scale_event_for_zoom(&ev, z);
      CT_CHECK(fabsf(ev.x - 300.0f / div) < 1e-4f);
      CT_CHECK(fabsf(ev.y - 600.0f / div) < 1e-4f);
    }

    ccord_input_event wheel = {.type = CCORD_IN_WHEEL, .x = 240.0f, .y = 480.0f, .wheel = -3.0f};
    ccord_app_desktop_scale_event_for_zoom(&wheel, z);
    CT_CHECK(fabsf(wheel.x - 240.0f / div) < 1e-4f);
    CT_CHECK(fabsf(wheel.y - 480.0f / div) < 1e-4f);
    CT_CHECK(fabsf(wheel.wheel - (-3.0f)) < 1e-4f);

    ccord_input_event safe = {
      .type = CCORD_IN_SAFE_AREA,
      .inset_top = 48.0f,
      .inset_right = 12.0f,
      .inset_bottom = 36.0f,
      .inset_left = 18.0f,
    };
    ccord_app_desktop_scale_event_for_zoom(&safe, z);
    CT_CHECK(fabsf(safe.inset_top - 48.0f / div) < 1e-4f);
    CT_CHECK(fabsf(safe.inset_right - 12.0f / div) < 1e-4f);
    CT_CHECK(fabsf(safe.inset_bottom - 36.0f / div) < 1e-4f);
    CT_CHECK(fabsf(safe.inset_left - 18.0f / div) < 1e-4f);

    ccord_input_event kb = {.type = CCORD_IN_KEYBOARD_INSET, .inset_bottom = 300.0f};
    ccord_app_desktop_scale_event_for_zoom(&kb, z);
    CT_CHECK(fabsf(kb.inset_bottom - 300.0f / div) < 1e-4f);
  }
  ccord_app_desktop_scale_event_for_zoom(nullptr, 1.5f);
}

// ---------------------------------------------------------------- fixture

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
  (void)mkdir("app_desktop_data", 0700);
  remove("app_desktop_data/settings.ini");
  remove("app_desktop_data/drafts.json");
  ccord_paths_set_data_dir("app_desktop_data");
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK)
    return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 2, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session) return false;
  ccord_platform_config pc = {.title = "app-desktop-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_app_desktop_deps deps = {.store = F.store, .session = F.session, .platform = F.platform,
                                 .renderer = F.renderer, .theme = &F.theme, .asset_dir = nullptr};
  F.desktop = ccord_app_desktop_create(&deps, &err);
  if (!F.desktop) return false;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .host = {.ud = F.desktop, .clipboard_set = host_clip_set, .text_input = host_text_input,
                              .set_extensions_enabled = host_set_ext, .open_url = host_open_url,
                              .open_file_dialog = host_open_file, .apply_theme = host_apply_theme,
                              .set_zoom = host_set_zoom, .desktop_capabilities = host_caps,
                              .set_close_to_tray = host_set_close_to_tray, .apply_quick_css = host_apply_quick_css,
                              .keybinds_changed = host_keybinds_changed,
                              .account_switch = host_account_switch, .account_remove = host_account_remove},
                     .fixed_now_unix_ms = 1793534400000LL + 30 * 60 * 1000};
  F_deps = d;
  F.ui = ccord_ui_create(&F_deps, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  ccord_app_desktop_attach_ui(F.desktop, F.ui);
  CT_CHECK(ccord_session_start(F.session, &err) == CCORD_OK);
  frame(3);
  return true;
}

static void teardown(void)
{
  ccord_app_desktop_destroy(F.desktop);
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
  ccord_contrib_destroy(F.contrib);
  remove("app_desktop_data/settings.ini");
  remove("app_desktop_data/drafts.json");
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_app_desktop")) return 77;
  test_startup_ini();
  test_startup_theme();
  test_host_callbacks();
  test_startup_prefs_apply();
  test_badge_sync();
  test_notify_truth_table();
  test_notify_delivery();
  test_notify_auto_clear_on_focus_and_read();
  test_notify_sound_hook();
  test_notify_wake_hook();
  test_hotkeys();
  test_voice_state_sync();
  test_quick_css_desktop();
  test_quick_css_ui();
  test_close_to_tray();
  test_keybinds();
  test_saved_accounts_ui();
  test_ipc();
  test_zoom_event_scaling();
  CT_CHECK(ccord_app_desktop_next_wait_ms(F.desktop, 10000) == UINT32_MAX);
  teardown();
  return CT_RESULT("test_app_desktop");
}
