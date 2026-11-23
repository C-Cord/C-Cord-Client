// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Quick Audio Device, Input Profile & Volume popover anchored to the Mute / Deafen buttons (desktop + mobile).
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ccord/platform/platform.h>

#include "ui_core_internal.h"
#include "ui_style.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define AP_DEV_MAX 12u

typedef struct audio_pop_state {
  const ccord_ui *owner;
  int mode;   // 0 = closed, 1 = input (mic), 2 = output (speaker)
  ccord_audio_device_info devs[AP_DEV_MAX];
  size_t ndevs;
  bool dragging_vol;
  ccord_rect box;
  ccord_rect dev_rects[AP_DEV_MAX + 1u];
  size_t ndev_rects;
  ccord_rect prof_rects[3];   // Input profiles: Voice Isolation, Studio, Custom
  ccord_rect vol_track;
  ccord_rect settings_btn;
} audio_pop_state;

static audio_pop_state g_ap;

static bool ap_active(const ccord_ui *ui)
{
  return ui && g_ap.owner == ui && g_ap.mode != 0;
}

ccord_rect ui_audio_caret_rect(const ccord_ui *ui, int slot)
{
  if (!ui || (slot != 0 && slot != 1)) return (ccord_rect){0};
  ccord_rect b = ui_user_btn_rect(ui, slot);
  if (b.w <= 0.0f || b.h <= 0.0f) return (ccord_rect){0};
  float sz = ui->mobile_layout ? 16.0f : 12.0f;
  // Sits at the top-right corner of the button so center clicks/taps always keep toggling Mute / Deafen.
  return (ccord_rect){b.x + b.w - sz + 1.0f, b.y - 1.0f, sz, sz};
}

void ui_audio_popover_open(ccord_ui *ui, int mode)
{
  if (!ui || (mode != 1 && mode != 2)) return;
  if (ap_active(ui) && g_ap.mode == mode) {
    ui_audio_popover_close(ui);
    return;
  }
  ui_status_card_close(ui);
  ui_menu_close(ui);
  memset(&g_ap, 0, sizeof g_ap);
  g_ap.owner = ui;
  g_ap.mode = mode;
  g_ap.ndevs = ccord_audio_devices_list(mode == 1, g_ap.devs, AP_DEV_MAX);
  ui->dirty = true;
}

void ui_audio_popover_close(ccord_ui *ui)
{
  if (ui && g_ap.owner != ui) return;
  g_ap.mode = 0;
  g_ap.dragging_vol = false;
  if (ui) ui->dirty = true;
}

bool ui_audio_popover_is_open(const ccord_ui *ui)
{
  return ap_active(ui);
}

static int current_input_profile(const ccord_ui *ui)
{
  if (ui->settings_noise_suppression && ui->settings_echo_cancel && ui->settings_auto_gain) return 0;   // Voice Isolation
  if (!ui->settings_noise_suppression && !ui->settings_echo_cancel && !ui->settings_auto_gain) return 1; // Studio
  return 2;                                                                                              // Custom
}

static void apply_vol_from_x(ccord_ui *ui, float mx)
{
  if (g_ap.vol_track.w <= 1.0f) return;
  float t = (mx - g_ap.vol_track.x) / g_ap.vol_track.w;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  int pct = (int)lroundf(t * 200.0f);
  if (g_ap.mode == 1) ui->settings_input_volume = pct;
  else ui->settings_output_volume = pct;
  ui_settings_save(ui);
  ui->dirty = true;
}

void ui_audio_popover_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ap_active(ui)) return;
  bool mob = ui->mobile_layout;
  uis_tokens tk = uis_tokens_of(ui);
  float row_h = mob ? UIS_MOB_TOUCH_H : tk.menu_row_h;
  float btn_h = uis_btn_h_for(ui, UIS_H_BTN_SM);
  float chip_h = mob ? UIS_MOB_H_CHIP : 24.0f;

  size_t total_devs = 1u + g_ap.ndevs;
  size_t show_devs = total_devs > 7u ? 7u : total_devs;
  float prof_h = (g_ap.mode == 1) ? (18.0f + chip_h + 12.0f) : 0.0f;
  float vol_h = 46.0f;
  float h = 28.0f + (float)show_devs * row_h + 12.0f + prof_h + vol_h + btn_h + 16.0f;

  float sl = 8.0f + (mob ? ui->safe_insets.left : 0.0f);
  float sr = 8.0f + (mob ? ui->safe_insets.right : 0.0f);
  float st = 8.0f + (mob ? ui->safe_insets.top : 0.0f);
  float sb = 8.0f + (mob ? fmaxf(ui->safe_insets.bottom, ui->keyboard_inset) : 0.0f);
  float max_w = fmaxf(ui->win_w - sl - sr, 200.0f);
  float w = mob ? fminf(340.0f, max_w) : fminf(268.0f, max_w);
  float max_h = fmaxf(ui->win_h - st - sb, 140.0f);
  if (h > max_h) h = max_h;

  float bx = sl;
  float by = ui->win_h - sb - h;
  if (!mob) {
    ccord_rect anchor = ui_user_btn_rect(ui, g_ap.mode == 1 ? 0 : 1);
    bx = anchor.x - w * 0.5f + anchor.w * 0.5f;
    by = ui->layout.user_panel.y - h - 8.0f;
  } else {
    bx = sl + (max_w - w) * 0.5f;
  }
  if (bx < sl) bx = sl;
  if (bx + w > ui->win_w - sr) bx = ui->win_w - sr - w;
  if (by < st) by = st;

  ccord_rect box = {bx, by, w, h};
  g_ap.box = box;
  g_ap.ndev_rects = 0;
  for (int i = 0; i < 3; i++) g_ap.prof_rects[i] = (ccord_rect){0};

  ui_popup_begin(ui, r, "audiopop", (uint64_t)g_ap.mode, UI_POPUP_MS, UIS_SLIDE_POPOVER);
  uis_popover_frame(ui, r, box);

  ccord_text_style ov = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
  const char *hdr = (g_ap.mode == 1) ? "INPUT DEVICE" : "OUTPUT DEVICE";
  float y = box.y + 10.0f;
  ccord_text_draw(r, hdr, strlen(hdr), box.x + 12.0f, y, &ov, 0.0f);
  y += 18.0f;

  int active_idx = (g_ap.mode == 1) ? ui->settings_mic_device : ui->settings_speaker_device;
  for (size_t i = 0; i < show_devs; i++) {
    const char *name = (i == 0) ? "Default" : g_ap.devs[i - 1u].name;
    ccord_rect rr = {box.x + 6.0f, y, box.w - 12.0f, row_h};
    g_ap.dev_rects[g_ap.ndev_rects++] = rr;
    bool hv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rr);
    uis_menu_row(ui, r, rr, name, (uis_state){.hover = hv}, false, active_idx == (int)i, false, "apdev", i);
    y += row_h;
  }

  y += 4.0f;
  uis_divider(ui, r, box.x + 10.0f, y, box.w - 20.0f);
  y += 8.0f;

  if (g_ap.mode == 1) {
    ccord_text_draw(r, "INPUT PROFILE", 13, box.x + 12.0f, y, &ov, 0.0f);
    y += 16.0f;
    static const char *const k_prof[3] = {"Isolation", "Studio", "Custom"};
    int cur_p = current_input_profile(ui);
    float gap = 6.0f;
    float cw = (box.w - 24.0f - 2.0f * gap) / 3.0f;
    for (int i = 0; i < 3; i++) {
      ccord_rect pr = {box.x + 12.0f + (float)i * (cw + gap), y, cw, chip_h};
      g_ap.prof_rects[i] = pr;
      bool phv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, pr);
      uis_chip(ui, r, pr, k_prof[i], (uis_state){.hover = phv, .selected = cur_p == i}, "approf", (uint64_t)i);
    }
    y += chip_h + 10.0f;
  }

  int vol = (g_ap.mode == 1) ? ui->settings_input_volume : ui->settings_output_volume;
  if (vol < 0) vol = 0;
  if (vol > 200) vol = 200;
  char vbuf[48];
  snprintf(vbuf, sizeof vbuf, "%s VOLUME  \xC2\xB7  %d%%", g_ap.mode == 1 ? "INPUT" : "OUTPUT", vol);
  ccord_text_draw(r, vbuf, strlen(vbuf), box.x + 12.0f, y, &ov, 0.0f);
  y += 20.0f;

  ccord_rect track = {box.x + 14.0f, y, box.w - 28.0f, UIS_SLIDER_H};
  g_ap.vol_track = (ccord_rect){track.x - 4.0f, track.y - 10.0f, track.w + 8.0f, track.h + 20.0f};
  ccord_draw_rrect(r, track, track.h * 0.5f, COL(bg_tertiary));
  float frac = (float)vol / 200.0f;
  float fill_w = track.w * frac;
  if (fill_w > 1.0f) {
    ccord_draw_rrect(r, (ccord_rect){track.x, track.y, fill_w, track.h}, track.h * 0.5f, COL(brand));
  }
  ccord_draw_circle(r, track.x + fill_w, track.y + track.h * 0.5f, mob ? 8.5f : 6.5f, COL(text_header));
  y += 22.0f;

  ccord_rect sbtn = {box.x + 12.0f, box.y + box.h - btn_h - 10.0f, box.w - 24.0f, btn_h};
  g_ap.settings_btn = sbtn;
  bool shv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, sbtn);
  uis_button(ui, r, sbtn, "Voice & Video Settings", UIS_BTN_SECONDARY,
             mob ? UIS_BTN_S_MD : UIS_BTN_S_SM, (uis_state){.hover = shv}, "apset", 0);
  ui_popup_end(r);
}

bool ui_audio_popover_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ap_active(ui) || !ev) return false;
  if (ui->settings_open || ui->modal_kind != UI_MODAL_NONE || ui->lightbox_open || ui->quick_switcher_open) {
    ui_audio_popover_close(ui);
    return false;
  }
  bool inside = g_ap.box.w > 0.0f && ui_in_rect(ev->x, ev->y, g_ap.box);
  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_ESCAPE) {
      ui_audio_popover_close(ui);
      return true;
    }
    return false;
  }
  if (ev->type == CCORD_IN_MOUSE_UP) {
    if (ev->button == 1 && g_ap.dragging_vol) {
      g_ap.dragging_vol = false;
      return true;
    }
    return inside;
  }
  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    ui->mx = ev->x;
    ui->my = ev->y;
    ui->mouse_in = true;
    ui->dirty = true;
    if (g_ap.dragging_vol) {
      apply_vol_from_x(ui, ev->x);
      return true;
    }
    return inside;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN) {
    ui->mx = ev->x;
    ui->my = ev->y;
    ui->mouse_in = true;
    ui->dirty = true;
    if (g_ap.box.w <= 0.0f) return true;
    if (!inside) {
      ui_audio_popover_close(ui);
      return true;
    }
    if (ev->button != 1) return true;
    for (size_t i = 0; i < g_ap.ndev_rects; i++) {
      if (ui_in_rect(ev->x, ev->y, g_ap.dev_rects[i])) {
        if (g_ap.mode == 1) {
          ui->settings_mic_device = (int)i;
          if (i == 0) ui->settings_mic_device_id[0] = '\0';
          else snprintf(ui->settings_mic_device_id, sizeof ui->settings_mic_device_id, "%s", g_ap.devs[i - 1u].id);
        } else {
          ui->settings_speaker_device = (int)i;
          if (i == 0) ui->settings_speaker_device_id[0] = '\0';
          else snprintf(ui->settings_speaker_device_id, sizeof ui->settings_speaker_device_id, "%s", g_ap.devs[i - 1u].id);
        }
        ui_settings_save(ui);
        ui_notice(ui, "%s device: %s", g_ap.mode == 1 ? "Input" : "Output", i == 0 ? "Default" : g_ap.devs[i - 1u].name);
        ui->dirty = true;
        return true;
      }
    }
    if (g_ap.mode == 1) {
      for (int p = 0; p < 3; p++) {
        if (g_ap.prof_rects[p].w > 0.0f && ui_in_rect(ev->x, ev->y, g_ap.prof_rects[p])) {
          if (p == 0) {
            ui->settings_noise_suppression = true;
            ui->settings_echo_cancel = true;
            ui->settings_auto_gain = true;
            ui_notice(ui, "Input profile: Voice Isolation");
          } else if (p == 1) {
            ui->settings_noise_suppression = false;
            ui->settings_echo_cancel = false;
            ui->settings_auto_gain = false;
            ui_notice(ui, "Input profile: Studio");
          } else {
            ui->settings_noise_suppression = true;
            ui->settings_echo_cancel = false;
            ui->settings_auto_gain = true;
            ui_notice(ui, "Input profile: Custom");
          }
          ui_settings_save(ui);
          ui->dirty = true;
          return true;
        }
      }
    }
    if (g_ap.vol_track.w > 0.0f && ui_in_rect(ev->x, ev->y, g_ap.vol_track)) {
      g_ap.dragging_vol = true;
      apply_vol_from_x(ui, ev->x);
      return true;
    }
    if (g_ap.settings_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, g_ap.settings_btn)) {
      ui_audio_popover_close(ui);
      ui->settings_page = 3;   // SP_VOICE ("Voice & Video")
      ui_open_settings(ui, false);
      return true;
    }
    return true;
  }
  return false;
}
