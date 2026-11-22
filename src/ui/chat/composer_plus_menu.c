// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Composer '+' menu: a popover above the button with Upload a File / Create Poll / Schedule Message.
#include <string.h>

#include "ui_internal.h"
#include "ui_style.h"

enum { PM_UPLOAD, PM_POLL, PM_SCHEDULE, PM_COUNT };
static const char *const pm_labels[PM_COUNT] = {"Upload a File", "Create Poll", "Schedule Message"};

static int pm_sel = -1;   // hovered / keyboard-selected row; reset on open

static int pm_total_count(const ccord_ui *ui)
{
  size_t extra = (ui && ui->d.contrib) ? ccord_contrib_action_btn_count(ui->d.contrib, CCORD_ACTBTN_COMPOSER_PLUS) : 0u;
  if (extra > 12u) extra = 12u;
  return PM_COUNT + (int)extra;
}

static float pm_row_h(const ccord_ui *ui)
{
  uis_tokens t = uis_tokens_of(ui);
  return ui->mobile_layout ? fmaxf(t.menu_row_h, 42.0f) : t.menu_row_h;
}

static ccord_rect pm_box(const ccord_ui *ui)
{
  uis_tokens t = uis_tokens_of(ui);
  int total = pm_total_count(ui);
  float rh = pm_row_h(ui);
  float sl = ui->mobile_layout ? ui->safe_insets.left : 0.0f;
  float sr = ui->mobile_layout ? ui->safe_insets.right : 0.0f;
  float st = ui->mobile_layout ? ui->safe_insets.top : 0.0f;
  float maxw = fmaxf(ui->win_w - sl - sr - 16.0f, 180.0f);
  float w = fminf(t.menu_min_w + 40.0f, maxw), h = (float)total * rh + 2.0f * t.menu_pad;
  ccord_rect a = ui->composer_attach_rect;
  float x = a.x < sl + 8.0f ? sl + 8.0f : a.x;
  if (x + w > ui->win_w - sr - 8.0f) x = ui->win_w - sr - 8.0f - w;
  float y = a.y - 8.0f - h;
  if (y < st + 8.0f) y = st + 8.0f;
  return (ccord_rect){x, y, w, h};
}

ccord_rect ui_plus_menu_row_rect(const ccord_ui *ui, size_t idx)
{
  uis_tokens t = uis_tokens_of(ui);
  ccord_rect b = pm_box(ui);
  float rh = pm_row_h(ui);
  return (ccord_rect){b.x + t.menu_pad, b.y + t.menu_pad + (float)idx * rh, b.w - 2.0f * t.menu_pad, rh};
}

static bool pm_disabled(const ccord_ui *ui, int i) { return i == PM_POLL && ui_composer_lock_reason(ui) != nullptr; }

void ui_plus_menu_open(ccord_ui *ui)
{
  if (!ui) return;
  ui->plus_menu_open = true;
  pm_sel = -1;
  ui->dirty = true;
}

void ui_plus_menu_close(ccord_ui *ui)
{
  if (!ui) return;
  ui->plus_menu_open = false;
  ui->dirty = true;
}

bool ui_plus_menu_is_open(const ccord_ui *ui) { return ui && ui->plus_menu_open; }

static void pm_icon(ccord_renderer *r, int i, ccord_rect row, ccord_color c)
{
  float s = 14.0f, x = row.x + row.w - s - 10.0f, y = row.y + (row.h - s) * 0.5f;
  if (i == PM_UPLOAD) {   // page with a folded look: tall rounded rect + a bar
    ccord_draw_rrect(r, (ccord_rect){x + 2.0f, y, s - 4.0f, s}, 2.0f, c);
  } else if (i == PM_POLL) {   // three bars
    ccord_draw_rrect(r, (ccord_rect){x, y + 1.0f, s, 3.0f}, 1.0f, c);
    ccord_draw_rrect(r, (ccord_rect){x, y + 5.5f, s * 0.7f, 3.0f}, 1.0f, c);
    ccord_draw_rrect(r, (ccord_rect){x, y + 10.0f, s * 0.45f, 3.0f}, 1.0f, c);
  } else {   // clock
    ccord_draw_circle(r, x + s * 0.5f, y + s * 0.5f, s * 0.5f, c);
  }
}

void ui_plus_menu_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r || !ui->plus_menu_open) return;
  ui_popup_begin(ui, r, "plusmenu", 0, UI_POPUP_MS, 6.0f);
  uis_popover_frame(ui, r, pm_box(ui));
  for (int i = 0; i < PM_COUNT; i++) {
    ccord_rect row = ui_plus_menu_row_rect(ui, (size_t)i);
    bool dis = pm_disabled(ui, i);
    bool hv = !dis && (pm_sel == i);
    uis_menu_row(ui, r, row, pm_labels[i], (uis_state){.hover = hv, .disabled = dis}, false, false, false, "plusrow", (uint64_t)i);
    pm_icon(r, i, row, dis ? ui->theme->c.text_muted : ui->theme->c.text_normal);
  }
  int total = pm_total_count(ui);
  for (int i = PM_COUNT; i < total; i++) {
    const ccord_action_btn_contribution *b = ccord_contrib_action_btn_at(ui->d.contrib, CCORD_ACTBTN_COMPOSER_PLUS, (size_t)(i - PM_COUNT));
    if (!b) continue;
    ccord_rect row = ui_plus_menu_row_rect(ui, (size_t)i);
    const char *lbl = (b->label && b->label[0]) ? b->label : b->btn_id;
    uis_menu_row(ui, r, row, lbl, (uis_state){.hover = pm_sel == i}, false, false, false, "plusrow", (uint64_t)i);
  }
  ui_popup_end(r);
}

static void pm_activate(ccord_ui *ui, int i)
{
  if (i < PM_COUNT && pm_disabled(ui, i)) return;
  ui_plus_menu_close(ui);
  if (i >= PM_COUNT && ui->d.contrib) {
    const ccord_action_btn_contribution *b = ccord_contrib_action_btn_at(ui->d.contrib, CCORD_ACTBTN_COMPOSER_PLUS, (size_t)(i - PM_COUNT));
    if (b) {
      ccord_menu_context ctx = {
        .target = CCORD_MENU_COMPOSER,
        .guild_id = ui->d.store ? ccord_store_selected_guild(ui->d.store) : CCORD_ID_NONE,
        .channel_id = ui->d.store ? ccord_store_selected_channel(ui->d.store) : CCORD_ID_NONE,
      };
      (void)ccord_contrib_click_action_btn(ui->d.contrib, b->owner, b->btn_id, &ctx);
    }
    return;
  }
  if (i == PM_UPLOAD) {
    if (ui->d.host.open_file_dialog_async) {
      // Multi-select picker: the chosen paths come back as drop-file events and are attached one by one.
      ui->d.host.open_file_dialog_async(ui->d.host.ud);
    } else if (ui->d.host.open_file_dialog) {
      char fpath[1024] = {0};
      if (ui->d.host.open_file_dialog(ui->d.host.ud, fpath, sizeof fpath) && fpath[0]) (void)ccord_ui_attach_file(ui, fpath);
    }
  } else if (i == PM_POLL) {
    ui_poll_composer_open(ui);
  } else {
    ui_schedule_dialog_open(ui);
  }
}

static int pm_hit(const ccord_ui *ui, float x, float y)
{
  int total = pm_total_count(ui);
  for (int i = 0; i < total; i++)
    if (ui_in_rect(x, y, ui_plus_menu_row_rect(ui, (size_t)i))) return i;
  return -1;
}

bool ui_plus_menu_input(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || !ui->plus_menu_open) return false;
  switch (ev->type) {
    case CCORD_IN_MOUSE_MOVE: {
      int h = pm_hit(ui, ev->x, ev->y);
      if (h >= 0 && h != pm_sel) { pm_sel = h; ui->dirty = true; }
      return false;
    }
    case CCORD_IN_KEY_DOWN:
      if (ev->key == CCORD_KEY_ESCAPE) {
        ui_plus_menu_close(ui);
      } else if (ev->key == CCORD_KEY_DOWN || ev->key == CCORD_KEY_UP) {
        int d = ev->key == CCORD_KEY_DOWN ? 1 : -1;
        int total = pm_total_count(ui);
        int n = pm_sel;
        for (int k = 0; k < total; k++) {
          n = (n < 0) ? (d > 0 ? 0 : total - 1) : (n + d + total) % total;
          if (n >= PM_COUNT || !pm_disabled(ui, n)) { pm_sel = n; break; }
        }
        ui->dirty = true;
      } else if (ev->key == CCORD_KEY_RETURN) {
        if (pm_sel >= 0) pm_activate(ui, pm_sel);
      }
      return true;
    case CCORD_IN_MOUSE_DOWN: {
      int h = pm_hit(ui, ev->x, ev->y);
      if (h >= 0) pm_activate(ui, h);
      else if (!ui_in_rect(ev->x, ev->y, pm_box(ui))) ui_plus_menu_close(ui);
      return true;
    }
    case CCORD_IN_TEXT:
    case CCORD_IN_MOUSE_UP:
    case CCORD_IN_WHEEL:
      return true;
    default:
      return false;
  }
}
