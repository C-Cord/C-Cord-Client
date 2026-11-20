// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// M6 preference settings pages: Notifications (rework), Accessibility, Streamer Mode, Language and
// Advanced, plus the Appearance theme/zoom block and the M6 local-pref INI keys
//. settings.c owns the sidebar, the page enum and the dispatch;
// the page bodies live here. Governed controls (Discord account settings) render from the store
// ONLY after the answer and change only through ccord_session_user_settings_patch (design D4: no
// optimistic mutation). Local prefs (theme, zoom, streamer mode, notification toggles, hardware
// acceleration, reduced motion, message display) apply immediately and persist to settings.ini —
// they are real local behavior, never presented as Discord state.
#include <stdio.h>

#include <ccord/render/render.h>
#include <ccord/ui/ui.h>

// Module-local page ids; settings.c maps SP_NOTIFICATIONS / SP_ACCESSIBILITY / SP_STREAMER /
// SP_LANGUAGE / SP_ADVANCED onto these.
enum {
  CCORD_M6_PREFS_NOTIFICATIONS = 0,
  CCORD_M6_PREFS_ACCESSIBILITY,
  CCORD_M6_PREFS_STREAMER,
  CCORD_M6_PREFS_LANGUAGE,
  CCORD_M6_PREFS_ADVANCED,
  CCORD_M6_PREFS_CCORD,   // C-CORD sidebar section: settings with no Discord counterpart
  CCORD_M6_PREFS_COUNT,
};

// Resets the transient module state (local notification prefs keep their defaults, the drawn-page
// and masking seams clear). Called from ui_settings_init.
void ui_settings_m6_prefs_reset(void);
// Draws one page body. `content` / `panel` are the settings-modal rects.
void ui_settings_m6_prefs_draw(ccord_ui *ui, ccord_renderer *r, int page, ccord_rect content, ccord_rect panel);
// Hit-tests a left click; true when the page consumed it.
bool ui_settings_m6_prefs_click(ccord_ui *ui, int page, ccord_rect content, ccord_rect panel, float x, float y);

// Appearance theme/zoom block (rows 1-3 of the Appearance page; settings.c keeps the rest of the
// page). Theme chips call host.apply_theme, the zoom slider/reset call host.set_zoom; both persist
// to settings.ini through the INI hooks below.
void ui_settings_m6_appearance_draw(ccord_ui *ui, ccord_renderer *r, ccord_rect content);
bool ui_settings_m6_appearance_click(ccord_ui *ui, ccord_rect content, float x, float y);

// INI hooks wired into settings.c settings_visit / settings_save. Only the [ui] keys this module
// owns are consumed (theme, zoom, streamer_mode, notify_badge, notify_suppress_focused); the visit
// returns true when it handled the key. Theme ids are validated against the three builtins
// (fallback c-cord-dark) and zoom is clamped to CCORD_ZOOM_MIN..CCORD_ZOOM_MAX.
bool ui_settings_m6_prefs_ini_visit(ccord_ui *ui, const char *key, const char *value);
void ui_settings_m6_prefs_ini_save(const ccord_ui *ui, FILE *f);
// Applies the persisted theme/zoom through the host callbacks once after settings_load (only for
// keys the INI actually provided). The app also pre-reads these at startup; both are idempotent.
void ui_settings_m6_prefs_apply_loaded(ccord_ui *ui);

// C-Cord client prefs (settings.ini [ui] keys jump_present_messages and game_detection). They are
// process-wide like the other local prefs, so no ui argument. Defaults: 10 and true. The setter and
// the INI visit clamp the jump threshold to 1..100.
int ccord_ui_pref_jump_present_messages(void);
void ccord_ui_pref_set_jump_present_messages(int n);
void ccord_ui_pref_set_game_detection(bool on);
// Close-to-background (settings.ini key close_to_tray, default ON): the window close button hides to the
// tray instead of quitting. Same pref as the Advanced "CLOSE TO TRAY" row; the app is told through
// host.set_close_to_tray.
bool ccord_ui_pref_close_to_background(void);
void ccord_ui_pref_set_close_to_background(bool on);

// Streamer-mode masking for sensitive account/invite strings: returns `text` unchanged when
// streamer mode is off (or text is empty), otherwise a '*' mask of the same byte length in `out`.
// Used by the Account / Profiles pages; invite surfaces can adopt the same helper.
const char *ui_settings_m6_mask(const ccord_ui *ui, const char *text, char *out, size_t cap);

// Test/introspection seam: the control rects as last drawn, the capability bitmask, the local
// notification prefs and the last masked account string. Zeroed before the first draw. Not part of
// the fixed-size ccord_ui_settings_debug snapshot, which predates these pages.
typedef struct ccord_settings_m6_prefs_debug {
  int drawn_page;                     // module-local id of the last drawn page, -1 before the first draw
  ccord_rect content;                 // content rect the rects below were computed against
  uint32_t caps;                      // desktop_capabilities() as last read (all bits when host is null)
  // Appearance theme/zoom block
  ccord_rect theme_chips[3];          // c-cord-dark / c-cord-light / c-cord-amoled
  ccord_rect zoom_minus, zoom_track, zoom_plus, zoom_reset;
  // Notifications
  ccord_rect notify_mentions, notify_messages, notify_sounds, notify_desktop;
  ccord_rect notify_timeout[4];
  ccord_rect tts_toggle;
  // C-Cord page (CCORD_M6_PREFS_CCORD)
  ccord_rect jump_minus, jump_track, jump_plus, game_toggle, notify_badge, notify_focus, close_background;
  int jump_value;
  bool game_detection_on, close_background_on;
  bool notify_badge_on, notify_focus_suppress;
  // Accessibility
  ccord_rect gif_toggle, emoji_toggle, sticker_chips[3], reduced_motion_toggle, display_chips[2];
  // Streamer Mode
  ccord_rect streamer_toggle, stream_notify_toggle;
  // Language
  ccord_rect language_row;
  int locale_index;                   // index of the confirmed locale in the fixed list, -1 when unknown
  // Advanced
  ccord_rect developer_toggle, hardware_toggle;
  // Last masked account string produced while streamer mode is on ("" when off / not drawn).
  char mask_last[64];
} ccord_settings_m6_prefs_debug;
void ui_settings_m6_prefs_get_debug(const ccord_ui *ui, ccord_settings_m6_prefs_debug *out);

// Implemented in settings.c (the sidebar wiring): the tab rects of the five preference pages
// (Notifications, Accessibility, Streamer Mode, Language, Advanced order).
void ui_settings_m6_prefs_host_rects(const ccord_ui *ui, ccord_rect out_tabs[CCORD_M6_PREFS_COUNT]);

// One settings control: a stable id and its on-screen label. The C-CORD page's controls come from
// ui_settings_ccord_controls; settings.c adds the other pages' controls to ui_settings_control_table.
typedef struct ui_settings_control {
  const char *id;
  const char *label;
} ui_settings_control;
size_t ui_settings_ccord_controls(const ui_settings_control **out);
// Every known control on every page: page index (sidebar page enum), id, label. Returns the count
// written (up to cap). Used by --verify-slice ccord-settings to assert no control sits on two pages.
typedef struct ui_settings_control_entry {
  int page;
  ui_settings_control c;
} ui_settings_control_entry;
size_t ui_settings_control_table(ui_settings_control_entry *out, size_t cap);
// Sidebar introspection: category headers in display order with the pages under each.
// Returns the number of pages written; for page k, out_category[k] is the heading text and out_rect[k] its row.
size_t ui_settings_sidebar_pages(const ccord_ui *ui, const char **out_category, ccord_rect *out_rect, int *out_page, size_t cap);
