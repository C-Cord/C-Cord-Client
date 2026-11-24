// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M6 preference settings pages: Notifications (rework), Accessibility, Streamer Mode, Language and
// Advanced, plus the Appearance theme/zoom block and the M6 local-pref INI keys (m6-ui-prefs).
// See settings_m6_prefs.h for the contract. Governed account settings render from the store and
// only change through ccord_session_user_settings_patch after the answer lands (design D4); local
// prefs apply immediately and persist to settings.ini. Capability-gated rows disable-with-reason
// using the host desktop_capabilities bitmask (a null host callback is treated as "all available"
// so offline/demo fixtures keep working; the real app always reports its capabilities).
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_internal.h"
#include "ui_style.h"

#include "settings_m6_prefs.h"

#include <ccord/core/paths.h>
#include <ccord/ui/timefmt.h>
#include <SDL3/SDL_video.h>

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

// ---- geometry (matches the settings.c row grid so the legacy debug rects stay valid) ----
#define P_TITLE_H 44.0f
#define P_ROW_H 54.0f

static float p_row_h(const ccord_ui *ui)
{
  return (ui && ui->mobile_layout) ? 74.0f : P_ROW_H;
}

static float p_row_y(const ccord_ui *ui, ccord_rect c, int row)
{
  return c.y + P_TITLE_H + (float)row * p_row_h(ui);
}

// The row a toggle sits in: UIS_H_BTN_SM (or UIS_MOB_H_BTN_SM on mobile) tall; switch flush right.
static ccord_rect p_row_rect(const ccord_ui *ui, ccord_rect c, int row)
{
  return (ccord_rect){c.x, p_row_y(ui, c, row), c.w, uis_btn_h_for(ui, UIS_H_BTN_SM)};
}

static ccord_rect p_toggle(const ccord_ui *ui, ccord_rect c, int row)
{
  return uis_toggle_rect(p_row_rect(ui, c, row));
}

static ccord_rect p_toggle_hit(const ccord_ui *ui, ccord_rect c, int row)
{
  ccord_rect tr = p_toggle(ui, c, row);
  return (ui && ui->mobile_layout) ? uis_hit_padded(tr, 56.0f, UIS_MOB_TOUCH_H) : tr;
}

static ccord_rect p_chip(const ccord_ui *ui, ccord_rect c, int row, int opt, int count)
{
  if (count < 1) return (ccord_rect){0, 0, 0, 0};
  float gap = 8.0f;
  float cw = (c.w - gap * (float)(count - 1)) / (float)count;
  if (cw > 132.0f) cw = 132.0f;
  if (cw < 44.0f) cw = 44.0f;
  float ch = uis_chip_h_for(ui, UIS_H_CHIP);
  float yoff = (ui && ui->mobile_layout) ? 22.0f : 20.0f;
  return (ccord_rect){c.x + (float)opt * (cw + gap), p_row_y(ui, c, row) + yoff, cw, ch};
}

static bool p_slider_narrow(const ccord_ui *ui, ccord_rect c)
{
  return (ui && ui->mobile_layout) || c.w < 340.0f;
}

static ccord_rect p_minus(const ccord_ui *ui, ccord_rect c, int row)
{
  float sz = uis_btn_h_for(ui, UIS_H_BTN_SM);
  float yoff = (ui && ui->mobile_layout) ? 34.0f : (p_slider_narrow(ui, c) ? 30.0f : 18.0f);
  return (ccord_rect){c.x, p_row_y(ui, c, row) + yoff, sz, sz};
}

static ccord_rect p_track(const ccord_ui *ui, ccord_rect c, int row)
{
  if (p_slider_narrow(ui, c)) {
    float btn = uis_btn_h_for(ui, UIS_H_BTN_SM);
    float yoff = (ui && ui->mobile_layout) ? 34.0f : 30.0f;
    float by = p_row_y(ui, c, row) + yoff;
    float val_res = (row == 0) ? 94.0f : 52.0f;
    float tw = fmaxf(c.w - 2.0f * btn - 16.0f - val_res, 44.0f);
    return (ccord_rect){c.x + btn + 8.0f, by + (btn - UIS_SLIDER_H) * 0.5f, tw, UIS_SLIDER_H};
  }
  float tw = c.w < 430.0f ? fmaxf(c.w - 208.0f, 64.0f) : 200.0f;
  return (ccord_rect){c.x + 36.0f, p_row_y(ui, c, row) + 28.0f, tw, UIS_SLIDER_H};
}

static ccord_rect p_track_hit(const ccord_ui *ui, ccord_rect c, int row)
{
  ccord_rect tr = p_track(ui, c, row);
  return p_slider_narrow(ui, c) ? uis_hit_padded(tr, tr.w, UIS_MOB_TOUCH_H) : tr;
}

static ccord_rect p_plus(const ccord_ui *ui, ccord_rect c, int row)
{
  ccord_rect tr = p_track(ui, c, row);
  float sz = uis_btn_h_for(ui, UIS_H_BTN_SM);
  float yoff = (ui && ui->mobile_layout) ? 34.0f : (p_slider_narrow(ui, c) ? 30.0f : 18.0f);
  return (ccord_rect){tr.x + tr.w + 8.0f, p_row_y(ui, c, row) + yoff, sz, sz};
}

static ccord_rect p_device(const ccord_ui *ui, ccord_rect c, int row)
{
  if (ui && ui->mobile_layout) {
    return (ccord_rect){c.x, p_row_y(ui, c, row) + 20.0f, c.w, UIS_MOB_H_BTN};
  }
  float w = c.w * 0.7f;
  if (w > 300.0f) w = 300.0f;
  return (ccord_rect){c.x, p_row_y(ui, c, row) + 18.0f, w, UIS_H_BTN_SM};
}

static ccord_rect p_zoom_reset(const ccord_ui *ui, ccord_rect c)
{
  if (p_slider_narrow(ui, c)) {
    float rw = (ui && ui->mobile_layout) ? 80.0f : 72.0f;
    float rh = uis_btn_h_for(ui, 30.0f);
    float ry = p_row_y(ui, c, 2) + ((ui && ui->mobile_layout) ? -6.0f : -4.0f);
    return (ccord_rect){c.x + c.w - rw, ry, rw, rh};
  }
  if (c.w < 430.0f) {
    float rw = 70.0f;
    return (ccord_rect){c.x + c.w - rw, p_row_y(ui, c, 2) + 18.0f, rw, UIS_H_BTN_SM};
  }
  return (ccord_rect){c.x + 330.0f, p_row_y(ui, c, 2) + 18.0f, 90.0f, UIS_H_BTN_SM};
}

ccord_rect ui_settings_m6_zoom_reset_rect(const ccord_ui *ui, ccord_rect content)
{
  return p_zoom_reset(ui, content);
}

// ---- transient module state (one settings modal at a time; reset from ui_settings_init) ----
static struct {
  int drawn_page;             // module-local id of the last drawn page, -1 before the first draw
  ccord_rect content;         // content rect of the last draw/click (for the debug seam)
  bool notify_badge;          // local INI: unread tray badge (default on)
  bool notify_focus_suppress; // local INI: suppress desktop notifications while focused (default off)
  bool loaded_theme, loaded_zoom;   // the INI actually provided these keys (apply_loaded gate)
  char mask_last[64];         // last masked account string while streamer mode is on
  // Streamer Mode per-option toggles (local INI). Discord's defaults: all on; they only apply
  // while settings_streamer_mode is on.
  bool streamer_hide_personal, streamer_hide_invites, streamer_disable_sounds, streamer_disable_notifications;
  // Client-local C-Cord prefs (settings.ini [ui]): the "Jump to present" threshold (messages below the
  // viewport, 1..100, default 10) and game detection (default on).
  int jump_present_messages;
  bool game_detection;
  bool close_background;      // settings.ini key close_to_tray (default on): window close keeps C-Cord in the tray
  float sounds_scroll;        // vertical scroll offset for 1-col Customize Sounds list on narrow viewports
} s_prefs = {.drawn_page = -1, .notify_badge = true,
             .streamer_hide_personal = true, .streamer_hide_invites = true,
             .streamer_disable_sounds = true, .streamer_disable_notifications = true,
             .jump_present_messages = 10, .game_detection = true};

#define P_JUMP_PRESENT_MIN 1
#define P_JUMP_PRESENT_MAX 100
#define P_JUMP_PRESENT_DEFAULT 10

void ui_settings_m6_prefs_reset(void)
{
  s_prefs.drawn_page = -1;
  s_prefs.content = (ccord_rect){0, 0, 0, 0};
  s_prefs.notify_badge = true;
  s_prefs.notify_focus_suppress = false;
  s_prefs.loaded_theme = false;
  s_prefs.loaded_zoom = false;
  s_prefs.mask_last[0] = '\0';
  s_prefs.streamer_hide_personal = true;
  s_prefs.streamer_hide_invites = true;
  s_prefs.streamer_disable_sounds = true;
  s_prefs.streamer_disable_notifications = true;
  s_prefs.jump_present_messages = P_JUMP_PRESENT_DEFAULT;
  s_prefs.game_detection = true;
  s_prefs.close_background = true;
  s_prefs.sounds_scroll = 0.0f;
}

// ---- capabilities -----------------------------------------------------------------------------
#define P_CAP_ALL (CCORD_UI_CAP_AUDIO_PLAYBACK | CCORD_UI_CAP_NOTIFICATIONS | CCORD_UI_CAP_TRAY | CCORD_UI_CAP_GLOBAL_SHORTCUTS)

static uint32_t prefs_caps(const ccord_ui *ui)
{
  if (ui->d.host.desktop_capabilities) return ui->d.host.desktop_capabilities(ui->d.host.ud);
  return P_CAP_ALL;   // no host report (offline/demo fixtures): assume everything is available
}

// ---- governed account settings ----------------------------------------------------------------
// Discord defaults until the first full settings object arrives (the store is nullptr before that).
static ccord_user_settings prefs_current(const ccord_ui *ui)
{
  const ccord_user_settings *us = ui->d.store ? ccord_store_user_settings(ui->d.store) : nullptr;
  if (us) return *us;
  return (ccord_user_settings){.gif_auto_play = true, .animate_emoji = true, .animate_stickers = 0, .locale = "en-US"};
}

static void prefs_submit_patch(ccord_ui *ui, const ccord_user_settings_patch *patch)
{
  ui->dirty = true;
  if (!ui->d.session) {
    ui_notice(ui, "Sign in to change your Discord account settings.");
    return;
  }
  ccord_error err;
  if (ccord_session_user_settings_patch(ui->d.session, patch, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't update your settings: %s", err.message);
  }
}

// ---- Appearance theme / zoom ------------------------------------------------------------------
static const char *const k_theme_ids[4] = {"c-cord-dark", "c-cord-light", "c-cord-amoled", "c-cord-system"};
static const char *const k_theme_labels[4] = {"Graphite", "Stone", "Obsidian", "System"};

static const char *resolve_effective_theme_id(const char *id)
{
  if (id && strcmp(id, "c-cord-system") == 0) {
    return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_LIGHT ? "c-cord-light" : "c-cord-dark";
  }
  return (id && id[0]) ? id : "c-cord-dark";
}

static int theme_index(const char *id)
{
  for (int i = 0; i < 4; i++) {
    if (strcmp(id, k_theme_ids[i]) == 0) return i;
  }
  return 0;   // unknown id falls back to Dark (settings_load also normalizes to c-cord-dark)
}

static void prefs_set_theme(ccord_ui *ui, const char *id)
{
  if (strcmp(ui->settings_theme_id, id) == 0) return;
  snprintf(ui->settings_theme_id, sizeof ui->settings_theme_id, "%s", id);
  ui->last_system_theme = (int)SDL_GetSystemTheme();
  ui->dirty = true;
  if (ui->d.host.apply_theme) ui->d.host.apply_theme(ui->d.host.ud, resolve_effective_theme_id(id));
}

void ui_settings_m6_system_theme_tick(ccord_ui *ui)
{
  if (!ui || strcmp(ui->settings_theme_id, "c-cord-system") != 0) return;
  if (ui->now_ms < ui->system_theme_next_ms) return;
  ui->system_theme_next_ms = ui->now_ms + 1000u;
  int st = (int)SDL_GetSystemTheme();
  if (ui->last_system_theme != st) {
    ui->last_system_theme = st;
    if (ui->d.host.apply_theme) {
      ui->d.host.apply_theme(ui->d.host.ud, resolve_effective_theme_id("c-cord-system"));
    }
    ui->dirty = true;
  }
}

// Clamp to the renderer's zoom range and snap to the 0.05 slider step.
static float prefs_snap_zoom(float z)
{
  if (!isfinite(z)) return 1.0f;
  if (z < CCORD_ZOOM_MIN) z = CCORD_ZOOM_MIN;
  if (z > CCORD_ZOOM_MAX) z = CCORD_ZOOM_MAX;
  z = roundf(z / 0.05f) * 0.05f;
  if (z < CCORD_ZOOM_MIN) z = CCORD_ZOOM_MIN;
  if (z > CCORD_ZOOM_MAX) z = CCORD_ZOOM_MAX;
  return z;
}

static void prefs_set_zoom(ccord_ui *ui, float z)
{
  z = prefs_snap_zoom(z);
  if (fabsf(z - ui->settings_zoom) < 0.001f) return;
  ui->settings_zoom = z;
  ui->dirty = true;
  if (ui->d.host.set_zoom) ui->d.host.set_zoom(ui->d.host.ud, z);
}

// ---- local notification prefs + fixed option tables -------------------------------------------
static const char *const k_notify_timeout[4] = {"3s", "5s", "10s", "Never"};
static const int k_notify_timeout_ms[4] = {3000, 5000, 10000, 0};
static const char *const k_sticker_modes[3] = {"Always", "On hover", "Never"};
static const char *const k_display_mode[2] = {"Compact", "Cozy"};
static const char *const k_role_color_modes[3] = {"In name", "Next to name", "Don't show"};
static const char *const k_time_formats[3] = {"Auto", "12-hour", "24-hour"};

static int notify_timeout_index(int ms)
{
  if (ms == 3000) return 0;
  if (ms == 10000) return 2;
  if (ms == 0) return 3;
  return 1;
}

// Fixed Discord locale list (codes are what the governed PATCH sends; labels are English because
// C-Cord's interface is English only for now — the Language page says so honestly).
static const struct { const char *code; const char *label; } k_locales[] = {
  {"en-US", "English (US)"},   {"en-GB", "English (UK)"},        {"de", "German"},
  {"fr", "French"},            {"es-ES", "Spanish"},             {"it", "Italian"},
  {"ja", "Japanese"},          {"ko", "Korean"},                 {"nl", "Dutch"},
  {"pl", "Polish"},            {"pt-BR", "Portuguese (BR)"},     {"ru", "Russian"},
  {"sv-SE", "Swedish"},        {"tr", "Turkish"},                {"uk", "Ukrainian"},
  {"zh-CN", "Chinese (Simplified)"}, {"zh-TW", "Chinese (Traditional)"},
};
#define P_LOCALE_COUNT ((int)(sizeof k_locales / sizeof k_locales[0]))

static int locale_index(const char *code)
{
  for (int i = 0; i < P_LOCALE_COUNT; i++) {
    if (strcmp(code, k_locales[i].code) == 0) return i;
  }
  return -1;
}

// ---- drawing helpers --------------------------------------------------------------------------
static void p_row_label(ccord_ui *ui, ccord_renderer *r, ccord_rect c, int row, const char *label)
{
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_caption, COL(text_muted));
  ccord_text_draw(r, label, strlen(label), c.x, p_row_y(ui, c, row), &ls, 0.0f);
}

static void p_note(ccord_ui *ui, ccord_renderer *r, float x, float y, float w, const char *text)
{
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  ccord_text_draw_ellipsis(r, text, strlen(text), x, y, w, &ns);
}

static void p_toggle_row(ccord_ui *ui, ccord_renderer *r, ccord_rect c, int row, const char *label,
                         const char *note, bool on, bool dim, const char *ns, uint64_t id)
{
  p_row_label(ui, r, c, row, label);
  if (note && note[0]) {
    ccord_text_style ns2 = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
    ccord_text_draw_ellipsis(r, note, strlen(note), c.x,
                             p_row_y(ui, c, row) + (ui && ui->mobile_layout ? 20.0f : 18.0f),
                             c.w - 60.0f, &ns2);
  }
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, p_toggle(ui, c, row)), .disabled = dim};
  uis_toggle(ui, r, p_row_rect(ui, c, row), on, st, ns, id);
}

static void p_chip_row(ccord_ui *ui, ccord_renderer *r, ccord_rect c, int row, int count,
                       const char *const *labels, int selected, const char *ns)
{
  for (int i = 0; i < count; i++) {
    ccord_rect o = p_chip(ui, c, row, i, count);
    uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, o), .selected = i == selected};
    uint64_t cid = (uint64_t)row * 64u + (uint64_t)i;
    uis_chip(ui, r, o, labels[i], st, ns, cid);
  }
}

static void p_button(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, bool primary, bool dim)
{
  if (rc.w <= 0.0f) return;
  uis_btn_size size = uis_btn_size_for_h(ui, rc.h);
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, rc), .disabled = dim};
  uint64_t id = (uint64_t)(rc.y * 4096.0f + rc.x);
  uis_button(ui, r, rc, label, primary ? UIS_BTN_PRIMARY : UIS_BTN_SECONDARY, size, st, "pbtn", id);
}

static void p_slider_row(ccord_ui *ui, ccord_renderer *r, ccord_rect c, int row, const char *label, float fraction,
                         const char *val_text)
{
  p_row_label(ui, r, c, row, label);
  ccord_rect minus = p_minus(ui, c, row), plus = p_plus(ui, c, row), track = p_track(ui, c, row);
  uint64_t vid = (uint64_t)row;
  uis_state bst = {.hover = ui_in_rect(ui->mx, ui->my, minus)};
  uis_btn_size bsz = (ui && ui->mobile_layout) ? UIS_BTN_S_MD : UIS_BTN_S_SM;
  uis_button(ui, r, minus, "-", UIS_BTN_SECONDARY, bsz, bst, "pzm", vid);
  bst.hover = ui_in_rect(ui->mx, ui->my, plus);
  uis_button(ui, r, plus, "+", UIS_BTN_SECONDARY, bsz, bst, "pzp", vid);
  uis_slider(ui, r, track, fraction, (uis_state){0});
  ccord_text_style vsl = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
  float vx = plus.x + plus.w + (p_slider_narrow(ui, c) ? 8.0f : 12.0f);
  float vy = p_slider_narrow(ui, c)
               ? (plus.y + (plus.h - ui_line_h(ui, vsl.size)) * 0.5f)
               : (p_row_y(ui, c, row) + 26.0f);
  float vw = c.x + c.w - vx;
  if (vw > 12.0f) {
    ccord_text_draw_ellipsis(r, val_text, strlen(val_text), vx, vy, vw, &vsl);
  }
}

static void p_device_row(ccord_ui *ui, ccord_renderer *r, ccord_rect c, int row, const char *label, const char *value)
{
  p_row_label(ui, r, c, row, label);
  ccord_rect d = p_device(ui, c, row);
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, d)};
  uis_select(ui, r, d, value, st, "pdev", (uint64_t)row);
}

// ---- Appearance theme/zoom block (rows 1-3) -----------------------------------------------------
void ui_settings_m6_appearance_draw(ccord_ui *ui, ccord_renderer *r, ccord_rect content)
{
  s_prefs.content = content;
  p_row_label(ui, r, content, 1, "THEME");
  p_chip_row(ui, r, content, 1, 4, k_theme_labels, theme_index(ui->settings_theme_id), "pthm");
  float zoom_frac = (ui->settings_zoom - CCORD_ZOOM_MIN) / (CCORD_ZOOM_MAX - CCORD_ZOOM_MIN);
  int pct = (int)(ui->settings_zoom * 100.0f + 0.5f);
  char ztxt[16];
  snprintf(ztxt, sizeof ztxt, "%d%%", pct);
  p_slider_row(ui, r, content, 2, "UI ZOOM", zoom_frac, ztxt);
  p_button(ui, r, p_zoom_reset(ui, content), "Reset", false, false);

  // Compact theme summary + color swatches (row 3), replacing the old tall label_value block.
  const ccord_theme *t = ui->theme;
  float x = content.x, y = p_row_y(ui, content, 3) + 8.0f, w = content.w;
  char sum[512];
  snprintf(sum, sizeof sum, "%s (%s)  ·  fonts: %s, %s", t->name, t->id, t->font_regular, t->font_bold);
  p_note(ui, r, x, y, w, sum);
  y += ui_line_h(ui, uis_tokens_of(ui).fs_control) + 8.0f;
  struct { const char *name; ccord_color c; } sw[] = {
    {"primary", COL(bg_primary)}, {"secondary", COL(bg_secondary)}, {"tertiary", COL(bg_tertiary)}, {"input", COL(bg_input)},
    {"brand", COL(brand)}, {"online", COL(status_online)}, {"idle", COL(status_idle)}, {"dnd", COL(status_dnd)},
  };
  ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_overline, COL(text_muted));
  float cell = w < 400.0f ? 32.0f : 40.0f;
  float sgap = w < 400.0f ? 6.0f : 8.0f;
  uis_tokens tk = uis_tokens_of(ui);
  size_t per_row = (size_t)((w + sgap) / (cell + sgap));
  if (per_row == 0) per_row = 1;
  if (per_row > 8) per_row = 8;
  size_t nsw = sizeof sw / sizeof sw[0];
  size_t swatch_rows = (nsw + per_row - 1u) / per_row;
  float row_pitch = cell + 16.0f;
  for (size_t i = 0; i < nsw; i++) {
    float cx = x + (float)(i % per_row) * (cell + sgap);
    float cy = y + (float)(i / per_row) * row_pitch;
    ccord_draw_rrect(r, (ccord_rect){cx - 1.0f, cy - 1.0f, cell + 2.0f, 28.0f}, tk.r_control + 1.0f, COL(border_subtle));
    ccord_draw_rrect(r, (ccord_rect){cx, cy, cell, 26.0f}, tk.r_control, sw[i].c);
    ccord_text_draw_ellipsis(r, sw[i].name, strlen(sw[i].name), cx, cy + 28.0f, cell + sgap - 2.0f, &cs);
  }
  if (ui->d.contrib && ccord_contrib_theme_preset_count(ui->d.contrib) > 0) {
    float py = y + (swatch_rows <= 1u ? 54.0f : (float)swatch_rows * row_pitch);
    ccord_text_style ph = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
    ccord_text_draw(r, "PLUGIN THEMES", 13, x, py, &ph, 0.0f);
    py += ui_line_h(ui, ph.size) + UIS_SP_1;
    float px = x;
    float pb_h = uis_btn_h_for(ui, UIS_H_BTN_SM);
    size_t np = ccord_contrib_theme_preset_count(ui->d.contrib);
    for (size_t i = 0; i < np && i < 16u; i++) {
      const ccord_theme_preset *pr = ccord_contrib_theme_preset_at(ui->d.contrib, i);
      if (!pr || !pr->name || !pr->name[0]) continue;
      float bw = uis_label_width(ui, r, pr->name);
      if (px > x && px + bw > x + w) {
        px = x;
        py += pb_h + UIS_SP_1;
      }
      ccord_rect brc = {px, py, bw, pb_h};
      uis_state bst = {.hover = ui_in_rect(ui->mx, ui->my, brc)};
      uis_button(ui, r, brc, pr->name, UIS_BTN_SECONDARY, UIS_BTN_S_SM, bst, "ptheme", (uint64_t)i);
      px += bw + UIS_SP_2;
    }
  }
}

bool ui_settings_m6_appearance_click(ccord_ui *ui, ccord_rect content, float x, float y)
{
  s_prefs.content = content;
  for (int i = 0; i < 4; i++) {
    if (ui_in_rect(x, y, p_chip(ui, content, 1, i, 4))) {
      if (ui->d.host.select_theme_preset) ui->d.host.select_theme_preset(ui->d.host.ud, "", "");
      prefs_set_theme(ui, k_theme_ids[i]);
      return true;
    }
  }
  if (ui->d.contrib && ccord_contrib_theme_preset_count(ui->d.contrib) > 0 && ui->last_renderer) {
    uis_tokens tk = uis_tokens_of(ui);
    float cell = content.w < 400.0f ? 32.0f : 40.0f;
    float sgap = content.w < 400.0f ? 6.0f : 8.0f;
    size_t per_row = (size_t)((content.w + sgap) / (cell + sgap));
    if (per_row == 0) per_row = 1;
    if (per_row > 8) per_row = 8;
    size_t swatch_rows = (8u + per_row - 1u) / per_row;
    float sw_off = swatch_rows <= 1u ? 54.0f : (float)swatch_rows * (cell + 16.0f);
    float base_y = p_row_y(ui, content, 3) + 8.0f + ui_line_h(ui, tk.fs_control) + 8.0f + sw_off +
                   ui_line_h(ui, tk.fs_overline) + UIS_SP_1;
    float px = content.x;
    float py = base_y;
    float pb_h = uis_btn_h_for(ui, UIS_H_BTN_SM);
    size_t np = ccord_contrib_theme_preset_count(ui->d.contrib);
    for (size_t i = 0; i < np && i < 16u; i++) {
      const ccord_theme_preset *pr = ccord_contrib_theme_preset_at(ui->d.contrib, i);
      if (!pr || !pr->name || !pr->name[0]) continue;
      float bw = uis_label_width(ui, ui->last_renderer, pr->name);
      if (px > content.x && px + bw > content.x + content.w) {
        px = content.x;
        py += pb_h + UIS_SP_1;
      }
      ccord_rect brc = {px, py, bw, pb_h};
      if (ui_in_rect(x, y, brc)) {
        if (ui->d.host.select_theme_preset) {
          ui->d.host.select_theme_preset(ui->d.host.ud, pr->owner, pr->preset_id);
        }
        return true;
      }
      px += bw + UIS_SP_2;
    }
  }
  if (ui_in_rect(x, y, p_minus(ui, content, 2))) {
    prefs_set_zoom(ui, ui->settings_zoom - 0.05f);
    return true;
  }
  if (ui_in_rect(x, y, p_plus(ui, content, 2))) {
    prefs_set_zoom(ui, ui->settings_zoom + 0.05f);
    return true;
  }
  if (ui_in_rect(x, y, p_track_hit(ui, content, 2))) {
    ccord_rect tr = p_track(ui, content, 2);
    float t = (x - tr.x) / tr.w;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    prefs_set_zoom(ui, CCORD_ZOOM_MIN + t * (CCORD_ZOOM_MAX - CCORD_ZOOM_MIN));
    return true;
  }
  if (ui_in_rect(x, y, p_zoom_reset(ui, content))) {
    prefs_set_zoom(ui, 1.0f);
    return true;
  }
  return false;
}

// ---- INI persistence --------------------------------------------------------------------------
static bool p_ini_bool(const char *value, bool *out)
{
  if (!value) return false;
  if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0) {
    *out = true;
    return true;
  }
  if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0) {
    *out = false;
    return true;
  }
  return false;
}

bool ui_settings_m6_prefs_ini_visit(ccord_ui *ui, const char *key, const char *value)
{
  if (!value) return false;
  if (strcmp(key, "theme") == 0) {
    const char *migrated_value = value;
    if (strcmp(value, "discord-dark") == 0 || strcmp(value, "ccord-dark") == 0) {
      migrated_value = "c-cord-dark";
    } else if (strcmp(value, "discord-light") == 0 || strcmp(value, "ccord-light") == 0) {
      migrated_value = "c-cord-light";
    } else if (strcmp(value, "discord-amoled") == 0 || strcmp(value, "ccord-amoled") == 0) {
      migrated_value = "c-cord-amoled";
    }
    const char *id = "c-cord-dark";   // unknown / hand-edited ids fall back to Dark
    for (int i = 0; i < 4; i++) {
      if (strcmp(migrated_value, k_theme_ids[i]) == 0) {
        id = k_theme_ids[i];
        break;
      }
    }
    snprintf(ui->settings_theme_id, sizeof ui->settings_theme_id, "%s", id);
    s_prefs.loaded_theme = true;
    return true;
  }
  if (strcmp(key, "time_format") == 0) {
    char *end = nullptr;
    long n = strtol(value, &end, 10);
    if (end != value && end && *end == '\0' && n >= 0 && n <= 2) {
      ui->settings_time_format = (uint8_t)n;
      ccord_timefmt_set_mode((ccord_timefmt_mode)n);
    }
    return true;
  }
  if (strcmp(key, "role_color_mode") == 0) {
    char *end = nullptr;
    long n = strtol(value, &end, 10);
    if (end != value && end && *end == '\0' && n >= 0 && n <= 2) ui->settings_role_color_mode = (uint8_t)n;
    return true;
  }
  if (strcmp(key, "spoiler_mode") == 0) {
    char *end = nullptr;
    long n = strtol(value, &end, 10);
    if (end != value && end && *end == '\0' && n >= 0 && n <= 2) ui->settings_spoiler_mode = (uint8_t)n;
    return true;
  }
  if (strcmp(key, "underline_links") == 0) {
    bool ubv;
    if (p_ini_bool(value, &ubv)) ui->settings_underline_links = ubv;
    return true;
  }
  if (strcmp(key, "zoom") == 0) {
    char *end = nullptr;
    float z = strtof(value, &end);
    if (end != value && end && *end == '\0' && isfinite(z)) {
      ui->settings_zoom = prefs_snap_zoom(z);
      s_prefs.loaded_zoom = true;
    }
    return true;
  }
  if (strcmp(key, "jump_present_messages") == 0) {
    char *end = nullptr;
    long n = strtol(value, &end, 10);
    if (end != value && end && *end == '\0') {   // out-of-range numbers clamp; non-numbers keep the default
      if (n < P_JUMP_PRESENT_MIN) n = P_JUMP_PRESENT_MIN;
      if (n > P_JUMP_PRESENT_MAX) n = P_JUMP_PRESENT_MAX;
      s_prefs.jump_present_messages = (int)n;
    }
    return true;
  }
  bool bv;
  if (strcmp(key, "game_detection") == 0) {
    if (p_ini_bool(value, &bv)) s_prefs.game_detection = bv;
    return true;
  }
  if (strcmp(key, "streamer_mode") == 0) {
    if (p_ini_bool(value, &bv)) {
      ui->settings_streamer_mode = bv;
      if (ui->d.contrib) ccord_contrib_set_streamer_mode(ui->d.contrib, bv);
    }
    return true;
  }
  if (strcmp(key, "streamer_hide_personal") == 0) {
    if (p_ini_bool(value, &bv)) s_prefs.streamer_hide_personal = bv;
    return true;
  }
  if (strcmp(key, "streamer_hide_invites") == 0) {
    if (p_ini_bool(value, &bv)) s_prefs.streamer_hide_invites = bv;
    return true;
  }
  if (strcmp(key, "streamer_disable_sounds") == 0) {
    if (p_ini_bool(value, &bv)) s_prefs.streamer_disable_sounds = bv;
    return true;
  }
  if (strcmp(key, "streamer_disable_notifications") == 0) {
    if (p_ini_bool(value, &bv)) s_prefs.streamer_disable_notifications = bv;
    return true;
  }
  if (strcmp(key, "notify_badge") == 0) {
    if (p_ini_bool(value, &bv)) s_prefs.notify_badge = bv;
    return true;
  }
  if (strcmp(key, "notify_suppress_focused") == 0) {
    if (p_ini_bool(value, &bv)) s_prefs.notify_focus_suppress = bv;
    return true;
  }
  for (unsigned k = 0; k < UI_SOUND_KIND_COUNT; k++) {
    if (strcmp(key, ui_sound_kinds[k].ini) != 0) continue;
    if (p_ini_bool(value, &bv)) {
      if (bv) ui->settings_sounds_off &= ~(1u << k);
      else ui->settings_sounds_off |= 1u << k;
    }
    return true;
  }
  return false;
}

void ui_settings_m6_prefs_ini_save(const ccord_ui *ui, FILE *f)
{
  fprintf(f, "theme = %s\n", ui->settings_theme_id);
  fprintf(f, "zoom = %.2f\n", (double)ui->settings_zoom);
  fprintf(f, "time_format = %u\n", (unsigned)ui->settings_time_format);
  fprintf(f, "role_color_mode = %u\n", (unsigned)ui->settings_role_color_mode);
  fprintf(f, "spoiler_mode = %u\n", (unsigned)ui->settings_spoiler_mode);
  fprintf(f, "underline_links = %d\n", ui->settings_underline_links ? 1 : 0);
  fprintf(f, "streamer_mode = %d\n", ui->settings_streamer_mode ? 1 : 0);
  fprintf(f, "streamer_hide_personal = %d\n", s_prefs.streamer_hide_personal ? 1 : 0);
  fprintf(f, "streamer_hide_invites = %d\n", s_prefs.streamer_hide_invites ? 1 : 0);
  fprintf(f, "streamer_disable_sounds = %d\n", s_prefs.streamer_disable_sounds ? 1 : 0);
  fprintf(f, "streamer_disable_notifications = %d\n", s_prefs.streamer_disable_notifications ? 1 : 0);
  fprintf(f, "notify_badge = %d\n", s_prefs.notify_badge ? 1 : 0);
  fprintf(f, "notify_suppress_focused = %d\n", s_prefs.notify_focus_suppress ? 1 : 0);
  fprintf(f, "jump_present_messages = %d\n", s_prefs.jump_present_messages);
  fprintf(f, "game_detection = %d\n", s_prefs.game_detection ? 1 : 0);
  for (unsigned k = 0; k < UI_SOUND_KIND_COUNT; k++) {
    fprintf(f, "%s = %d\n", ui_sound_kinds[k].ini, ui_sound_kind_enabled(ui, k) ? 1 : 0);
  }
}

// ---- C-Cord client prefs (declared in settings_m6_prefs.h) -------------------------------------
int ccord_ui_pref_jump_present_messages(void) { return s_prefs.jump_present_messages; }

void ccord_ui_pref_set_jump_present_messages(int n)
{
  if (n < P_JUMP_PRESENT_MIN) n = P_JUMP_PRESENT_MIN;
  if (n > P_JUMP_PRESENT_MAX) n = P_JUMP_PRESENT_MAX;
  s_prefs.jump_present_messages = n;
}

bool ccord_ui_pref_close_to_background(void) { return s_prefs.close_background; }

void ccord_ui_pref_set_close_to_background(bool on) { s_prefs.close_background = on; }

bool ccord_ui_pref_game_detection(void) { return s_prefs.game_detection; }

void ccord_ui_pref_set_game_detection(bool on) { s_prefs.game_detection = on; }

void ui_settings_m6_prefs_apply_loaded(ccord_ui *ui)
{
  ccord_timefmt_set_mode((ccord_timefmt_mode)ui->settings_time_format);
  ui->last_system_theme = (int)SDL_GetSystemTheme();
  if (s_prefs.loaded_theme && ui->d.host.apply_theme) {
    ui->d.host.apply_theme(ui->d.host.ud, resolve_effective_theme_id(ui->settings_theme_id));
  }
  if (s_prefs.loaded_zoom && ui->d.host.set_zoom) ui->d.host.set_zoom(ui->d.host.ud, ui->settings_zoom);
}

// ---- Streamer-mode masking --------------------------------------------------------------------
const char *ui_settings_m6_mask(const ccord_ui *ui, const char *text, char *out, size_t cap)
{
  if (!text) text = "";
  bool mask = ui && ui->settings_streamer_mode && s_prefs.streamer_hide_personal && text[0];
  if (!mask) {
    s_prefs.mask_last[0] = '\0';
    return text;
  }
  if (cap == 0) {
    s_prefs.mask_last[0] = '\0';
    return "";
  }
  size_t n = strlen(text);
  if (n >= cap) n = cap - 1;
  for (size_t i = 0; i < n; i++) out[i] = '*';
  out[n] = '\0';
  snprintf(s_prefs.mask_last, sizeof s_prefs.mask_last, "%s", out);
  return out;
}

// ---- Streamer Mode per-option toggles + invite masking -----------------------------------------
ui_streamer_opts ui_streamer_get_opts(const ccord_ui *ui)
{
  (void)ui;
  return (ui_streamer_opts){.hide_personal = s_prefs.streamer_hide_personal,
                            .hide_invites = s_prefs.streamer_hide_invites,
                            .disable_sounds = s_prefs.streamer_disable_sounds,
                            .disable_notifications = s_prefs.streamer_disable_notifications};
}

void ui_streamer_toggle_rects(const ccord_ui *ui, ccord_rect out[4])
{
  for (int i = 0; i < 4; i++) {
    out[i] = s_prefs.content.w > 0.0f ? p_toggle(ui, s_prefs.content, 2 + i) : (ccord_rect){0, 0, 0, 0};
  }
}

// ---- Per-sound toggles (SOUNDS > Customize) ---------------------------------------------------
const ui_sound_kind ui_sound_kinds[UI_SOUND_KIND_COUNT] = {
  {"message", "Message", "sound_message"},
  {"mention", "Mention", "sound_mention"},
  {"voice-join", "Someone joins your channel", "sound_voice_join"},
  {"voice-leave", "Someone leaves your channel", "sound_voice_leave"},
  {"mute", "You mute", "sound_mute"},
  {"unmute", "You unmute", "sound_unmute"},
  {"deafen", "You deafen", "sound_deafen"},
  {"undeafen", "You undeafen", "sound_undeafen"},
  {"call-ring", "Incoming call ring", "sound_call_ring"},
  {"self-connect", "You connect", "sound_self_connect"},
  {"self-disconnect", "You disconnect", "sound_self_disconnect"},
  {"user-moved", "You are moved", "sound_user_moved"},
  {"outgoing-ring", "Outgoing call ring", "sound_outgoing_ring"},
  {"stream-start", "Stream starts", "sound_stream_start"},
  {"stream-stop", "Stream stops", "sound_stream_stop"},
  {"viewer-join", "Viewer joins your stream", "sound_viewer_join"},
  {"viewer-leave", "Viewer leaves your stream", "sound_viewer_leave"},
  {"ptt-on", "Push-to-talk on", "sound_ptt_on"},
  {"ptt-off", "Push-to-talk off", "sound_ptt_off"},
  {"invited-to-speak", "Invited to speak", "sound_invited_to_speak"},
};

#define P_SOUND_ROWS 10   // two columns of ten, filled column by column
#define P_SOUND_ROW_H 36.0f

static float p_sound_row_h(const ccord_ui *ui)
{
  return (ui && ui->mobile_layout) ? 46.0f : P_SOUND_ROW_H;
}

static bool p_sounds_single_col(ccord_rect c)
{
  return c.w < 440.0f;
}

static float p_sounds_max_scroll(const ccord_ui *ui, ccord_rect c)
{
  float list_y = c.y + P_TITLE_H + 24.0f;
  float view_h = fmaxf(c.y + c.h - list_y, 40.0f);
  float srh = p_sound_row_h(ui);
  float total_h = p_sounds_single_col(c) ? ((float)UI_SOUND_KIND_COUNT * srh + 8.0f)
                                         : ((float)P_SOUND_ROWS * srh + 8.0f);
  float m = total_h - view_h;
  return m > 0.0f ? m : 0.0f;
}

static ccord_rect p_sound_row(const ccord_ui *ui, ccord_rect c, int kind)
{
  float max_s = p_sounds_max_scroll(ui, c);
  float scroll = s_prefs.sounds_scroll;
  if (scroll < 0.0f) scroll = 0.0f;
  if (scroll > max_s) scroll = max_s;
  float srh = p_sound_row_h(ui);
  float bh = uis_btn_h_for(ui, UIS_H_BTN_SM);
  if (p_sounds_single_col(c)) {
    return (ccord_rect){c.x, c.y + P_TITLE_H + 24.0f + (float)kind * srh - scroll, c.w, bh};
  }
  float colw = (c.w - 24.0f) * 0.5f;
  int col = kind / P_SOUND_ROWS, row = kind % P_SOUND_ROWS;
  float x = c.x + (float)col * (colw + 24.0f);
  return (ccord_rect){x, c.y + P_TITLE_H + 24.0f + (float)row * srh - scroll, colw, bh};
}

static ccord_rect p_sound_toggle(const ccord_ui *ui, ccord_rect c, int kind)
{
  return uis_toggle_rect(p_sound_row(ui, c, kind));
}

static ccord_rect p_sound_toggle_hit(const ccord_ui *ui, ccord_rect c, int kind)
{
  ccord_rect tr = p_sound_toggle(ui, c, kind);
  return (ui && ui->mobile_layout) ? uis_hit_padded(tr, 56.0f, UIS_MOB_TOUCH_H) : tr;
}

static ccord_rect p_sounds_customize(const ccord_ui *ui, ccord_rect c)
{
  float bh = uis_btn_h_for(ui, UIS_H_BTN_SM);
  float gap = (ui && ui->mobile_layout) ? 14.0f : 12.0f;
  return (ccord_rect){c.x + c.w - UIS_TOGGLE_W - gap - 96.0f, p_row_y(ui, c, 2), 96.0f, bh};
}

static ccord_rect p_sounds_back(const ccord_ui *ui, ccord_rect c)
{
  float bw = (ui && ui->mobile_layout) ? 88.0f : 80.0f;
  float bh = uis_btn_h_for(ui, UIS_H_BTN_SM);
  return (ccord_rect){c.x + c.w - bw, c.y, bw, bh};
}

ccord_rect ui_settings_sounds_customize_rect(const ccord_ui *ui)
{
  if (!ui || ui->settings_sounds_open || s_prefs.content.w <= 0.0f || s_prefs.drawn_page != CCORD_M6_PREFS_NOTIFICATIONS)
    return (ccord_rect){0, 0, 0, 0};
  return p_sounds_customize(ui, s_prefs.content);
}

ccord_rect ui_settings_sounds_back_rect(const ccord_ui *ui)
{
  if (!ui || !ui->settings_sounds_open || s_prefs.content.w <= 0.0f || s_prefs.drawn_page != CCORD_M6_PREFS_NOTIFICATIONS)
    return (ccord_rect){0, 0, 0, 0};
  return p_sounds_back(ui, s_prefs.content);
}

ccord_rect ui_settings_sound_toggle_rect(const ccord_ui *ui, int kind)
{
  if (!ui || !ui->settings_sounds_open || kind < 0 || kind >= UI_SOUND_KIND_COUNT || s_prefs.content.w <= 0.0f ||
      s_prefs.drawn_page != CCORD_M6_PREFS_NOTIFICATIONS)
    return (ccord_rect){0, 0, 0, 0};
  return p_sound_toggle(ui, s_prefs.content, kind);
}

static void p_draw_sounds(ccord_ui *ui, ccord_renderer *r, ccord_rect c)
{
  bool master = ui->settings_notify_sounds;
  p_button(ui, r, p_sounds_back(ui, c), "Back", false, false);
  p_note(ui, r, c.x, c.y + P_TITLE_H, c.w,
         master ? "Choose which sounds play." : "Sounds are off: turn SOUNDS on to change these.");
  float max_s = p_sounds_max_scroll(ui, c);
  if (s_prefs.sounds_scroll < 0.0f) s_prefs.sounds_scroll = 0.0f;
  if (s_prefs.sounds_scroll > max_s) s_prefs.sounds_scroll = max_s;
  float list_y = c.y + P_TITLE_H + 22.0f;
  ccord_rect clip = {c.x - 4.0f, list_y, c.w + 8.0f, fmaxf(c.y + c.h - list_y, 20.0f)};
  bool single = p_sounds_single_col(c);
  float colw = single ? c.w : (c.w - 24.0f) * 0.5f;
  ccord_push_clip(r, clip);
  for (int k = 0; k < UI_SOUND_KIND_COUNT; k++) {
    ccord_rect row = p_sound_row(ui, c, k);
    if (row.y + row.h < clip.y || row.y > clip.y + clip.h) continue;
    ccord_rect t = uis_toggle_rect(row);
    ccord_text_style ls = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, master ? COL(text_header) : COL(text_muted));
    ccord_vec2 lm = ccord_text_measure(r, ui_sound_kinds[k].label, strlen(ui_sound_kinds[k].label), &ls, 0.0f);
    ccord_text_draw_ellipsis(r, ui_sound_kinds[k].label, strlen(ui_sound_kinds[k].label), row.x,
                             t.y + (t.h - lm.y) * 0.5f, colw - 44.0f - 10.0f, &ls);
    uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, t), .disabled = !master};
    uis_toggle(ui, r, row, ui_sound_kind_enabled(ui, (unsigned)k) && master, st, "m6ps", (uint64_t)k);
  }
  ccord_pop_clip(r);
}

static bool p_click_sounds(ccord_ui *ui, ccord_rect c, float x, float y)
{
  if (ui_in_rect(x, y, p_sounds_back(ui, c))) {
    ui->settings_sounds_open = false;
    s_prefs.sounds_scroll = 0.0f;
    ui->dirty = true;
    return true;
  }
  if (!ui->settings_notify_sounds) return false;
  float list_y = c.y + P_TITLE_H + 22.0f;
  ccord_rect clip = {c.x, list_y, c.w, fmaxf(c.y + c.h - list_y, 20.0f)};
  if (!ui_in_rect(x, y, clip)) return false;
  for (int k = 0; k < UI_SOUND_KIND_COUNT; k++) {
    if (ui_in_rect(x, y, p_sound_toggle_hit(ui, c, k))) {
      ui->settings_sounds_off ^= 1u << k;
      ui->dirty = true;
      return true;
    }
  }
  return false;
}

bool ui_settings_m6_prefs_wheel(ccord_ui *ui, int page, ccord_rect content, float wheel)
{
  if (!ui || page != CCORD_M6_PREFS_NOTIFICATIONS || !ui->settings_sounds_open) return false;
  float max_s = p_sounds_max_scroll(ui, content);
  if (max_s <= 0.0f) return false;
  float dy = ui->touch_scroll_direct_override != 0 ? ui->touch_scroll_px : (-wheel * 48.0f);
  ui_scroll_offset_by(ui, &s_prefs.sounds_scroll, dy, max_s);
  ui->dirty = true;
  return true;
}

// ---- Notifications page -----------------------------------------------------------------------
static void p_draw_notifications(ccord_ui *ui, ccord_renderer *r, ccord_rect c)
{
  uint32_t caps = prefs_caps(ui);
  bool audio = (caps & CCORD_UI_CAP_AUDIO_PLAYBACK) != 0;
  if (ui->settings_sounds_open) {
    p_draw_sounds(ui, r, c);
    return;
  }
  bool notif = (caps & CCORD_UI_CAP_NOTIFICATIONS) != 0;
  p_toggle_row(ui, r, c, 0, "MENTIONS", "Notify when someone mentions you.", ui->settings_notify_mentions, false, "m6pn", 0);
  p_toggle_row(ui, r, c, 1, "ALL MESSAGES", "Notify for every new message in this account.", ui->settings_notify_messages, false, "m6pn", 1);
  // SOUNDS row: the note is shortened to leave room for the Customize button (per-sound toggles).
  p_row_label(ui, r, c, 2, "SOUNDS");
  p_note(ui, r, c.x, p_row_y(ui, c, 2) + (ui && ui->mobile_layout ? 20.0f : 18.0f), c.w - 60.0f - 108.0f,
         audio ? "Play sounds for messages, voice and call activity." : "Unavailable: no audio playback device.");
  p_button(ui, r, p_sounds_customize(ui, c), "Customize", false, !audio);
  uis_state sst = {.hover = ui_in_rect(ui->mx, ui->my, p_toggle(ui, c, 2)), .disabled = !audio};
  uis_toggle(ui, r, p_row_rect(ui, c, 2), ui->settings_notify_sounds && audio, sst, "m6pn", 2);
  p_toggle_row(ui, r, c, 3, "DESKTOP NOTIFICATIONS",
               notif ? "Forward notifications to the desktop notification daemon."
                     : "Unavailable: desktop notifications are not supported here.",
               ui->settings_notify_desktop && notif, !notif, "m6pn", 3);
  p_row_label(ui, r, c, 4, "NOTIFICATION TIMEOUT");
  p_chip_row(ui, r, c, 4, 4, k_notify_timeout, notify_timeout_index(ui->settings_notify_timeout_ms), "m6pn");
  ccord_user_settings cur = prefs_current(ui);
  p_toggle_row(ui, r, c, 5, "ENABLE /TTS COMMAND", "Allow the /tts command to send text-to-speech messages.",
               cur.enable_tts_command, false, "m6pn", 5);
}

static bool p_click_notifications(ccord_ui *ui, ccord_rect c, float x, float y)
{
  uint32_t caps = prefs_caps(ui);
  if (ui->settings_sounds_open) return p_click_sounds(ui, c, x, y);
  if ((caps & CCORD_UI_CAP_AUDIO_PLAYBACK) && ui_in_rect(x, y, p_sounds_customize(ui, c))) {
    ui->settings_sounds_open = true;
    ui->dirty = true;
    return true;
  }
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 0))) {
    ui->settings_notify_mentions = !ui->settings_notify_mentions;
    ui->dirty = true;
    return true;
  }
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 1))) {
    ui->settings_notify_messages = !ui->settings_notify_messages;
    ui->dirty = true;
    return true;
  }
  if ((caps & CCORD_UI_CAP_AUDIO_PLAYBACK) && ui_in_rect(x, y, p_toggle_hit(ui, c, 2))) {
    ui->settings_notify_sounds = !ui->settings_notify_sounds;
    ui->dirty = true;
    return true;
  }
  if ((caps & CCORD_UI_CAP_NOTIFICATIONS) && ui_in_rect(x, y, p_toggle_hit(ui, c, 3))) {
    ui->settings_notify_desktop = !ui->settings_notify_desktop;
    ui->dirty = true;
    return true;
  }
  for (int i = 0; i < 4; i++) {
    if (ui_in_rect(x, y, p_chip(ui, c, 4, i, 4))) {
      ui->settings_notify_timeout_ms = k_notify_timeout_ms[i];
      ui->dirty = true;
      return true;
    }
  }
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 5))) {
    ccord_user_settings cur = prefs_current(ui);
    ccord_user_settings_patch p = CCORD_USER_SETTINGS_PATCH_INIT;
    p.enable_tts_command = cur.enable_tts_command ? 0 : 1;
    prefs_submit_patch(ui, &p);
    return true;
  }
  return false;
}

// ---- Accessibility page -----------------------------------------------------------------------
static void p_draw_accessibility(ccord_ui *ui, ccord_renderer *r, ccord_rect c)
{
  ccord_user_settings cur = prefs_current(ui);
  p_toggle_row(ui, r, c, 0, "AUTOMATICALLY PLAY GIFS", "Play GIFs in chat without hovering. When off, they play on hover.",
               cur.gif_auto_play, false, "m6pa", 0);
  p_toggle_row(ui, r, c, 1, "PLAY ANIMATED EMOJI", "Animate custom emoji. When off, they show the first frame.",
               cur.animate_emoji, false, "m6pa", 1);
  p_row_label(ui, r, c, 2, "PLAY ANIMATED STICKERS");
  p_chip_row(ui, r, c, 2, 3, k_sticker_modes, (int)cur.animate_stickers, "m6pa");
  p_toggle_row(ui, r, c, 3, "REDUCED MOTION", "Turn off fades, slides and other animations (local).",
               ui->settings_reduced_motion, false, "m6pa", 3);
  p_row_label(ui, r, c, 4, "MESSAGE DISPLAY");
  p_chip_row(ui, r, c, 4, 2, k_display_mode, ui->settings_display_mode, "m6pa");
  p_row_label(ui, r, c, 5, "ROLE COLORS");
  p_chip_row(ui, r, c, 5, 3, k_role_color_modes, (int)ui->settings_role_color_mode, "m6pa");
  p_toggle_row(ui, r, c, 6, "ALWAYS UNDERLINE LINKS", "Always draw an underline beneath links in chat (local).",
               ui->settings_underline_links, false, "m6pa", 6);
  p_note(ui, r, c.x, p_row_y(ui, c, 7) + 8.0f, c.w,
         "GIF, emoji and sticker playback sync to your Discord account; motion, display, role colors and links are local.");
}

static bool p_click_accessibility(ccord_ui *ui, ccord_rect c, float x, float y)
{
  ccord_user_settings cur = prefs_current(ui);
  ccord_user_settings_patch p = CCORD_USER_SETTINGS_PATCH_INIT;
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 0))) {
    p.gif_auto_play = cur.gif_auto_play ? 0 : 1;
    prefs_submit_patch(ui, &p);
    return true;
  }
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 1))) {
    p.animate_emoji = cur.animate_emoji ? 0 : 1;
    prefs_submit_patch(ui, &p);
    return true;
  }
  for (int i = 0; i < 3; i++) {
    if (ui_in_rect(x, y, p_chip(ui, c, 2, i, 3))) {
      if (cur.animate_stickers != (uint8_t)i) {
        p.animate_stickers = (int16_t)i;
        prefs_submit_patch(ui, &p);
      }
      return true;
    }
  }
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 3))) {
    ui->settings_reduced_motion = !ui->settings_reduced_motion;
    ui->anim.reduced_motion = ui->settings_reduced_motion;
    ui->dirty = true;
    return true;
  }
  for (int i = 0; i < 2; i++) {
    if (ui_in_rect(x, y, p_chip(ui, c, 4, i, 2))) {
      ui->settings_display_mode = i;
      ui->dirty = true;
      return true;
    }
  }
  for (int i = 0; i < 3; i++) {
    if (ui_in_rect(x, y, p_chip(ui, c, 5, i, 3))) {
      ui->settings_role_color_mode = (uint8_t)i;
      ui->dirty = true;
      return true;
    }
  }
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 6))) {
    ui->settings_underline_links = !ui->settings_underline_links;
    ui->dirty = true;
    return true;
  }
  return false;
}

// ---- Streamer Mode page -----------------------------------------------------------------------
static void p_draw_streamer(ccord_ui *ui, ccord_renderer *r, ccord_rect c)
{
  ccord_user_settings cur = prefs_current(ui);
  bool on = ui->settings_streamer_mode;
  p_toggle_row(ui, r, c, 0, "ENABLE STREAMER MODE", "Hide account details in Settings and suppress notifications while streaming.",
               on, false, "m6ps", 0);
  p_toggle_row(ui, r, c, 1, "ENABLE NOTIFICATIONS WHILE STREAMING", "Override the suppression and still notify while Streamer Mode is on.",
               cur.stream_notifications_enabled, false, "m6ps", 1);
  // The four per-option toggles (Discord's defaults: all on). They only apply while Streamer
  // Mode is on, so they dim until it is enabled.
  p_toggle_row(ui, r, c, 2, "HIDE PERSONAL INFORMATION", "Mask your name, ID and connected account usernames.",
               s_prefs.streamer_hide_personal, !on, "m6ps", 2);
  p_toggle_row(ui, r, c, 3, "HIDE INSTANT INVITE LINKS", "Mask invite codes in chat, embeds and the invite lists.",
               s_prefs.streamer_hide_invites, !on, "m6ps", 3);
  p_toggle_row(ui, r, c, 4, "DISABLE SOUNDS", "Don't play notification sounds while streaming.",
               s_prefs.streamer_disable_sounds, !on, "m6ps", 4);
  p_toggle_row(ui, r, c, 5, "DISABLE NOTIFICATIONS", "Don't show desktop notifications while streaming.",
               s_prefs.streamer_disable_notifications, !on, "m6ps", 5);
  p_note(ui, r, c.x, p_row_y(ui, c, 6) + 8.0f, c.w,
         "Streamer Mode is a local preference (like Discord's). The options above apply while it is on and persist to settings.ini.");
}

static bool p_click_streamer(ccord_ui *ui, ccord_rect c, float x, float y)
{
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 0))) {
    ui->settings_streamer_mode = !ui->settings_streamer_mode;
    if (ui->d.contrib) ccord_contrib_set_streamer_mode(ui->d.contrib, ui->settings_streamer_mode);
    ui->dirty = true;
    return true;
  }
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 1))) {
    ccord_user_settings cur = prefs_current(ui);
    ccord_user_settings_patch p = CCORD_USER_SETTINGS_PATCH_INIT;
    p.stream_notifications_enabled = cur.stream_notifications_enabled ? 0 : 1;
    prefs_submit_patch(ui, &p);
    return true;
  }
  if (!ui->settings_streamer_mode) return false;   // the option rows are dimmed until enabled
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 2))) {
    s_prefs.streamer_hide_personal = !s_prefs.streamer_hide_personal;
    ui->dirty = true;
    return true;
  }
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 3))) {
    s_prefs.streamer_hide_invites = !s_prefs.streamer_hide_invites;
    ui->dirty = true;
    return true;
  }
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 4))) {
    s_prefs.streamer_disable_sounds = !s_prefs.streamer_disable_sounds;
    ui->dirty = true;
    return true;
  }
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 5))) {
    s_prefs.streamer_disable_notifications = !s_prefs.streamer_disable_notifications;
    ui->dirty = true;
    return true;
  }
  return false;
}

// ---- Language page ----------------------------------------------------------------------------
static void p_draw_language(ccord_ui *ui, ccord_renderer *r, ccord_rect c)
{
  ccord_user_settings cur = prefs_current(ui);
  int idx = locale_index(cur.locale);
  char label[128];
  if (idx >= 0) snprintf(label, sizeof label, "%s — %s", k_locales[idx].label, k_locales[idx].code);
  else snprintf(label, sizeof label, "%s", cur.locale);
  p_device_row(ui, r, c, 0, "LANGUAGE", label);
  p_row_label(ui, r, c, 1, "TIME FORMAT");
  p_chip_row(ui, r, c, 1, 3, k_time_formats, (int)ui->settings_time_format, "m6pl");
  p_note(ui, r, c.x, p_row_y(ui, c, 2) + 8.0f, c.w,
         "Syncs your Discord account language. Time format applies locally to message timestamps.");
  p_note(ui, r, c.x, p_row_y(ui, c, 2) + 26.0f, c.w, "Click the language selector to cycle through available languages.");
}

static bool p_click_language(ccord_ui *ui, ccord_rect c, float x, float y)
{
  if (ui_in_rect(x, y, p_device(ui, c, 0))) {
    ccord_user_settings cur = prefs_current(ui);
    int idx = locale_index(cur.locale);
    int next = (idx + 1) % P_LOCALE_COUNT;   // unknown locale (-1) cycles to the first entry
    ccord_user_settings_patch p = CCORD_USER_SETTINGS_PATCH_INIT;
    snprintf(p.locale, sizeof p.locale, "%s", k_locales[next].code);
    prefs_submit_patch(ui, &p);
    return true;
  }
  for (int i = 0; i < 3; i++) {
    if (ui_in_rect(x, y, p_chip(ui, c, 1, i, 3))) {
      ui->settings_time_format = (uint8_t)i;
      ccord_timefmt_set_mode((ccord_timefmt_mode)i);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui->dirty = true;
      return true;
    }
  }
  return false;
}

// ---- Advanced page ----------------------------------------------------------------------------
static void p_draw_advanced(ccord_ui *ui, ccord_renderer *r, ccord_rect c)
{
  ccord_user_settings cur = prefs_current(ui);
  p_toggle_row(ui, r, c, 0, "DEVELOPER MODE", "Show Copy ID actions in context menus (syncs to your Discord account).",
               cur.developer_mode, false, "m6pad", 0);
  p_toggle_row(ui, r, c, 1, "HARDWARE ACCELERATION", "Use the GPU for rendering. Requires restart.",
               ui->settings_hardware_accel, false, "m6pad", 1);
#if defined(__APPLE__)
  p_toggle_row(ui, r, c, 2, "CLOSE TO TRAY", "Closing the window hides it to the menu bar; Menu Bar Quit or Cmd+Q still exits.",
               s_prefs.close_background, false, "m6pad", 2);
#else
  p_toggle_row(ui, r, c, 2, "CLOSE TO TRAY", "Closing the window hides it to the tray; Tray Quit or Ctrl+Q still exits.",
               s_prefs.close_background, false, "m6pad", 2);
#endif
  p_row_label(ui, r, c, 3, "DATA DIRECTORY");
  const char *dir = ccord_paths_data_dir();
  p_note(ui, r, c.x, p_row_y(ui, c, 3) + (ui && ui->mobile_layout ? 20.0f : 18.0f), c.w,
         dir && dir[0] ? dir : "(not available)");
}

static bool p_click_advanced(ccord_ui *ui, ccord_rect c, float x, float y)
{
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 0))) {
    ccord_user_settings cur = prefs_current(ui);
    ccord_user_settings_patch p = CCORD_USER_SETTINGS_PATCH_INIT;
    p.developer_mode = cur.developer_mode ? 0 : 1;
    prefs_submit_patch(ui, &p);
    return true;
  }
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 1))) {
    ui->settings_hardware_accel = !ui->settings_hardware_accel;
    ui->dirty = true;
    return true;
  }
  if (ui_in_rect(x, y, p_toggle_hit(ui, c, 2))) {
    s_prefs.close_background = !s_prefs.close_background;
    ui->dirty = true;
    if (ui->d.host.set_close_to_tray) ui->d.host.set_close_to_tray(ui->d.host.ud, s_prefs.close_background);
    ui_settings_save(ui);
    return true;
  }
  return false;   // the data directory is read-only text
}

// ---- C-Cord page ------------------------------------------------------------------------------
// Settings that have no counterpart in the official Discord client. Rows: 0 jump threshold stepper,
// 1 game detection, 2 unread tray badge, 3 suppress notifications while focused. Every change saves
// to settings.ini at once (no Save button).
static const ui_settings_control k_ccord_controls[] = {
  {"jump_present", "SHOW JUMP TO PRESENT AFTER N MESSAGES"},
  {"game_detection", "DETECT RUNNING GAMES"},
  {"notify_badge", "UNREAD TRAY BADGE"},
  {"notify_suppress_focused", "SUPPRESS WHEN FOCUSED"},
  {"close_background", "RUN IN BACKGROUND"},
};

size_t ui_settings_ccord_controls(const ui_settings_control **out)
{
  *out = k_ccord_controls;
  return sizeof k_ccord_controls / sizeof k_ccord_controls[0];
}

static ccord_rect p_update_primary_btn(const ccord_ui *ui, ccord_rect c)
{
  float bh = uis_btn_h_for(ui, 30.0f);
  return (ccord_rect){c.x, p_row_y(ui, c, 5) + 46.0f, 152.0f, bh};
}

static ccord_rect p_update_notes_btn(const ccord_ui *ui, ccord_rect c)
{
  float bh = uis_btn_h_for(ui, 30.0f);
  if (ui && ui->mobile_layout && c.w < 290.0f) {
    return (ccord_rect){c.x, p_row_y(ui, c, 5) + 46.0f + bh + 8.0f, 152.0f, bh};
  }
  return (ccord_rect){c.x + 162.0f, p_row_y(ui, c, 5) + 46.0f, 118.0f, bh};
}

static void p_draw_ccord(ccord_ui *ui, ccord_renderer *r, ccord_rect c)
{
  uint32_t caps = prefs_caps(ui);
  bool tray = (caps & CCORD_UI_CAP_TRAY) != 0;
  int n = s_prefs.jump_present_messages;
  char vs[24];
  snprintf(vs, sizeof vs, "%d messages", n);
  float jfrac = (float)(n - P_JUMP_PRESENT_MIN) / (float)(P_JUMP_PRESENT_MAX - P_JUMP_PRESENT_MIN);
  p_slider_row(ui, r, c, 0, k_ccord_controls[0].label, jfrac, vs);
  p_toggle_row(ui, r, c, 1, k_ccord_controls[1].label, "Detected games are shown as your activity.",
               s_prefs.game_detection, false, "m6pc", 1);
  p_toggle_row(ui, r, c, 2, k_ccord_controls[2].label,
               tray ? "Show the unread count badge on the tray icon." : "Unavailable: no system tray is available.",
               s_prefs.notify_badge && tray, !tray, "m6pc", 2);
  p_toggle_row(ui, r, c, 3, k_ccord_controls[3].label,
               "Don't show desktop notifications while this window is focused.",
               s_prefs.notify_focus_suppress, false, "m6pc", 3);
  p_toggle_row(ui, r, c, 4, k_ccord_controls[4].label,
               ui->force_mobile_mode ? "Keep connected in the background for live notifications (uses more battery)"
                                     : "Keep C-Cord running in the background when the window is closed",
               s_prefs.close_background, false, "m6pc", 4);
  char ver_line[160];
  if (ui->update_info.install_kind[0]) {
    snprintf(ver_line, sizeof ver_line, "C-Cord v%s (%s)", CCORD_VERSION_STRING, ui->update_info.install_kind);
  } else {
    snprintf(ver_line, sizeof ver_line, "C-Cord v%s", CCORD_VERSION_STRING);
  }
  p_note(ui, r, c.x, p_row_y(ui, c, 5) + 4.0f, c.w, ver_line);
  if (ui->update_info.self_updatable || ui->update_info.install_kind[0]) {
    const char *fallback_status = ui->update_info.self_updatable
                                    ? "Automatic updates enabled via c-cord.org"
                                    : (strcmp(ui->update_info.install_kind, "Android") == 0
                                         ? "Updates are managed via F-Droid or Releases."
                                         : "Updates are managed by your system package manager.");
    const char *status = ui->update_info.status_text[0] ? ui->update_info.status_text : fallback_status;
    p_note(ui, r, c.x, p_row_y(ui, c, 5) + 22.0f, c.w, status);
    ccord_rect btn = p_update_primary_btn(ui, c);
    bool can_inst = ui->update_info.self_updatable && ui->update_info.can_install;
    bool busy = ui->update_info.state == CCORD_UI_UPDATE_CHECKING || ui->update_info.state == CCORD_UI_UPDATE_DOWNLOADING;
    const char *lbl = "Check for Updates";
    uis_btn_kind kind = UIS_BTN_SECONDARY;
    if (ui->update_info.state == CCORD_UI_UPDATE_CHECKING) lbl = "Checking...";
    else if (ui->update_info.state == CCORD_UI_UPDATE_DOWNLOADING) lbl = "Downloading...";
    else if (can_inst && ui->update_info.state == CCORD_UI_UPDATE_AVAILABLE) {
      lbl = "Update Now";
      kind = UIS_BTN_PRIMARY;
    }
    uis_button(ui, r, btn, lbl, kind, UIS_BTN_S_SM,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, btn), .disabled = busy}, "m6up", 0);
    if (ui->update_info.release_url[0]) {
      ccord_rect nb = p_update_notes_btn(ui, c);
      uis_button(ui, r, nb, "Release Notes", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
                 (uis_state){.hover = ui_in_rect(ui->mx, ui->my, nb)}, "m6un", 0);
    }
  }
}

static bool p_click_ccord(ccord_ui *ui, ccord_rect c, float x, float y)
{
  int n = s_prefs.jump_present_messages;
  int want = n;
  if (ui_in_rect(x, y, p_minus(ui, c, 0))) want = n - 1;
  else if (ui_in_rect(x, y, p_plus(ui, c, 0))) want = n + 1;
  else if (ui_in_rect(x, y, p_track_hit(ui, c, 0))) {
    ccord_rect tr = p_track(ui, c, 0);
    float t = (x - tr.x) / tr.w;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    want = P_JUMP_PRESENT_MIN + (int)(t * (float)(P_JUMP_PRESENT_MAX - P_JUMP_PRESENT_MIN) + 0.5f);
  } else if (ui_in_rect(x, y, p_toggle_hit(ui, c, 1))) {
    s_prefs.game_detection = !s_prefs.game_detection;
    ui->dirty = true;
    ui_settings_save(ui);
    return true;
  } else if ((prefs_caps(ui) & CCORD_UI_CAP_TRAY) && ui_in_rect(x, y, p_toggle_hit(ui, c, 2))) {
    s_prefs.notify_badge = !s_prefs.notify_badge;
    ui->dirty = true;
    ui_settings_save(ui);
    return true;
  } else if (ui_in_rect(x, y, p_toggle_hit(ui, c, 3))) {
    s_prefs.notify_focus_suppress = !s_prefs.notify_focus_suppress;
    ui->dirty = true;
    ui_settings_save(ui);
    return true;
  } else if (ui_in_rect(x, y, p_toggle_hit(ui, c, 4))) {
    s_prefs.close_background = !s_prefs.close_background;
    ui->dirty = true;
    if (ui->d.host.set_close_to_tray) ui->d.host.set_close_to_tray(ui->d.host.ud, s_prefs.close_background);
    ui_settings_save(ui);
    return true;
  } else if ((ui->update_info.self_updatable || ui->update_info.install_kind[0]) &&
             ui_in_rect(x, y, p_update_primary_btn(ui, c))) {
    bool can_inst = ui->update_info.self_updatable && ui->update_info.can_install;
    if (can_inst && ui->update_info.state == CCORD_UI_UPDATE_AVAILABLE) {
      if (ui->d.host.update_install) ui->d.host.update_install(ui->d.host.ud);
    } else if (ui->update_info.state != CCORD_UI_UPDATE_CHECKING &&
               ui->update_info.state != CCORD_UI_UPDATE_DOWNLOADING) {
      if (ui->d.host.update_check) ui->d.host.update_check(ui->d.host.ud);
    }
    ui->dirty = true;
    return true;
  } else if ((ui->update_info.self_updatable || ui->update_info.install_kind[0]) &&
             ui->update_info.release_url[0] &&
             ui_in_rect(x, y, p_update_notes_btn(ui, c))) {
    if (ui->d.host.open_url) (void)ui->d.host.open_url(ui->d.host.ud, ui->update_info.release_url);
    return true;
  } else {
    return false;
  }
  ccord_ui_pref_set_jump_present_messages(want);
  ui->dirty = true;
  ui_settings_save(ui);
  return true;
}

// Close-to-tray toggle rect for the settings debug seam (zero before the first Advanced draw).
ccord_rect ui_settings_m6_close_tray_rect(const ccord_ui *ui)
{
  if (s_prefs.content.w <= 0.0f) return (ccord_rect){0, 0, 0, 0};
  return p_toggle(ui, s_prefs.content, 2);
}

// ---- dispatch ---------------------------------------------------------------------------------
void ui_settings_m6_prefs_draw(ccord_ui *ui, ccord_renderer *r, int page, ccord_rect content, ccord_rect panel)
{
  (void)panel;
  if (!ui || page < 0 || page >= CCORD_M6_PREFS_COUNT) return;
  s_prefs.content = content;
  s_prefs.drawn_page = page;
  if (page != CCORD_M6_PREFS_NOTIFICATIONS) ui->settings_sounds_open = false;   // the grid belongs to this page only
  if (page == CCORD_M6_PREFS_NOTIFICATIONS) p_draw_notifications(ui, r, content);
  else if (page == CCORD_M6_PREFS_ACCESSIBILITY) p_draw_accessibility(ui, r, content);
  else if (page == CCORD_M6_PREFS_STREAMER) p_draw_streamer(ui, r, content);
  else if (page == CCORD_M6_PREFS_LANGUAGE) p_draw_language(ui, r, content);
  else if (page == CCORD_M6_PREFS_CCORD) p_draw_ccord(ui, r, content);
  else p_draw_advanced(ui, r, content);
}

bool ui_settings_m6_prefs_click(ccord_ui *ui, int page, ccord_rect content, ccord_rect panel, float x, float y)
{
  (void)panel;
  if (!ui || page < 0 || page >= CCORD_M6_PREFS_COUNT) return false;
  s_prefs.content = content;
  s_prefs.drawn_page = page;
  if (page == CCORD_M6_PREFS_NOTIFICATIONS) return p_click_notifications(ui, content, x, y);
  if (page == CCORD_M6_PREFS_ACCESSIBILITY) return p_click_accessibility(ui, content, x, y);
  if (page == CCORD_M6_PREFS_STREAMER) return p_click_streamer(ui, content, x, y);
  if (page == CCORD_M6_PREFS_LANGUAGE) return p_click_language(ui, content, x, y);
  if (page == CCORD_M6_PREFS_CCORD) return p_click_ccord(ui, content, x, y);
  return p_click_advanced(ui, content, x, y);
}

// ---- debug seam -------------------------------------------------------------------------------
void ui_settings_m6_prefs_get_debug(const ccord_ui *ui, ccord_settings_m6_prefs_debug *out)
{
  *out = (ccord_settings_m6_prefs_debug){0};
  out->drawn_page = -1;
  if (!ui) return;
  out->caps = prefs_caps(ui);
  out->notify_badge_on = s_prefs.notify_badge;
  out->notify_focus_suppress = s_prefs.notify_focus_suppress;
  snprintf(out->mask_last, sizeof out->mask_last, "%s", s_prefs.mask_last);
  ccord_rect c = s_prefs.content;
  // Appearance theme/zoom geometry (deterministic from the content rect).
  for (int i = 0; i < 3; i++) out->theme_chips[i] = p_chip(ui, c, 1, i, 4);
  out->theme_chip_system = p_chip(ui, c, 1, 3, 4);
  out->zoom_minus = p_minus(ui, c, 2);
  out->zoom_track = p_track(ui, c, 2);
  out->zoom_plus = p_plus(ui, c, 2);
  out->zoom_reset = p_zoom_reset(ui, c);
  if (s_prefs.drawn_page < 0) return;
  int page = s_prefs.drawn_page;
  out->drawn_page = page;
  out->content = c;
  if (page == CCORD_M6_PREFS_NOTIFICATIONS) {
    out->notify_mentions = p_toggle(ui, c, 0);
    out->notify_messages = p_toggle(ui, c, 1);
    out->notify_sounds = p_toggle(ui, c, 2);
    out->notify_desktop = p_toggle(ui, c, 3);
    for (int i = 0; i < 4; i++) out->notify_timeout[i] = p_chip(ui, c, 4, i, 4);
    out->tts_toggle = p_toggle(ui, c, 5);
    return;
  }
  if (page == CCORD_M6_PREFS_CCORD) {
    out->jump_minus = p_minus(ui, c, 0);
    out->jump_track = p_track(ui, c, 0);
    out->jump_plus = p_plus(ui, c, 0);
    out->game_toggle = p_toggle(ui, c, 1);
    out->notify_badge = p_toggle(ui, c, 2);
    out->notify_focus = p_toggle(ui, c, 3);
    out->close_background = p_toggle(ui, c, 4);
    if (ui && (ui->update_info.self_updatable || ui->update_info.install_kind[0])) {
      out->update_primary = p_update_primary_btn(ui, c);
      if (ui->update_info.release_url[0]) out->update_notes = p_update_notes_btn(ui, c);
    }
    out->close_background_on = s_prefs.close_background;
    out->jump_value = s_prefs.jump_present_messages;
    out->game_detection_on = s_prefs.game_detection;
    return;
  }
  if (page == CCORD_M6_PREFS_ACCESSIBILITY) {
    out->gif_toggle = p_toggle(ui, c, 0);
    out->emoji_toggle = p_toggle(ui, c, 1);
    for (int i = 0; i < 3; i++) out->sticker_chips[i] = p_chip(ui, c, 2, i, 3);
    out->reduced_motion_toggle = p_toggle(ui, c, 3);
    for (int i = 0; i < 2; i++) out->display_chips[i] = p_chip(ui, c, 4, i, 2);
    for (int i = 0; i < 3; i++) out->role_color_chips[i] = p_chip(ui, c, 5, i, 3);
    out->underline_links_toggle = p_toggle(ui, c, 6);
    return;
  }
  if (page == CCORD_M6_PREFS_STREAMER) {
    out->streamer_toggle = p_toggle(ui, c, 0);
    out->stream_notify_toggle = p_toggle(ui, c, 1);
    return;
  }
  if (page == CCORD_M6_PREFS_LANGUAGE) {
    out->language_row = p_device(ui, c, 0);
    for (int i = 0; i < 3; i++) out->time_format_chips[i] = p_chip(ui, c, 1, i, 3);
    ccord_user_settings cur = prefs_current(ui);
    out->locale_index = locale_index(cur.locale);
    return;
  }
  out->developer_toggle = p_toggle(ui, c, 0);
  out->hardware_toggle = p_toggle(ui, c, 1);
}
