// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "ui_style.h"

#include "ui_internal.h"
#include <ccord/ui/style_debug.h>

// Shared style tokens and draw helpers. See ui_style.h and

#define TH(name) (ui->theme->m.name)
#define CO(name) (ui->theme->c.name)

static uis_audit g_audit;

const uis_audit *uis_audit_get(void)
{
  return &g_audit;
}

void uis_audit_reset(void)
{
  memset(&g_audit, 0, sizeof g_audit);
}

const char *uis_kind_name(uis_kind k)
{
  static const char *const names[UIS_K_COUNT] = {"button", "icon-button", "menu-row", "list-row", "popover",
                                                 "modal",  "tooltip",     "input",    "toggle",   "tab",
                                                 "surface", "chip",       "select",   "swatch",   "radio",
                                                 "divider", "slider",     "card",    "scrollbar", "plate"};
  return (unsigned)k < (unsigned)UIS_K_COUNT ? names[k] : "?";
}

static void audit(uis_kind k, ccord_rect rc, float radius, float font_px, ccord_color fill)
{
  uis_audit_entry *e = &g_audit.kind[k];
  e->count++;
  e->rect = rc;
  e->radius = radius;
  e->font_px = font_px;
  e->fill = fill;
  if (g_audit.kept < UIS_AUDIT_MAX) {
    uis_audit_record *rec = &g_audit.rec[g_audit.kept++];
    rec->kind = k;
    rec->rect = rc;
    rec->radius = radius;
    rec->font_px = font_px;
    rec->fill = fill;
  }
  g_audit.total++;
}

uis_tokens uis_tokens_of(const ccord_ui *ui)
{
  uis_tokens t;
  t.r_control = TH(channel_item_radius);
  t.r_menu = TH(context_menu_radius);
  t.r_surface = TH(composer_radius);
  t.fs_title = TH(font_size_large);
  t.fs_body = TH(font_size_base);
  t.fs_control = TH(font_size_small);
  t.fs_caption = TH(font_size_small) - 2.0f;
  t.fs_overline = TH(font_size_small) - 3.0f;
  t.menu_row_h = TH(context_menu_item_height);
  t.menu_pad = TH(context_menu_pad);
  t.menu_min_w = TH(context_menu_min_width);
  t.list_row_h = TH(channel_item_height);
  return t;
}

static ccord_color darken(ccord_color c, float amount)
{
  return ui_mix(c, (ccord_color){0, 0, 0, c.a}, amount);
}

// ---------------------------------------------------------------- shadows and frames

void uis_shadow(ccord_renderer *r, ccord_rect box, float radius, uis_elevation e)
{
  float step = e == UIS_ELEV_MODAL ? 3.0f : 2.0f;
  float dy = e == UIS_ELEV_MODAL ? 3.0f : 2.0f;
  int base = e == UIS_ELEV_MODAL ? 20 : 18;
  int inc = e == UIS_ELEV_MODAL ? 10 : 8;
  for (int i = 3; i >= 1; i--) {
    float grow = (float)i * step;
    ccord_draw_rrect(r, (ccord_rect){box.x - grow, box.y - grow + dy, box.w + 2.0f * grow, box.h + 2.0f * grow},
                     radius + grow, (ccord_color){0, 0, 0, (uint8_t)(base + (3 - i) * inc)});
  }
}

void uis_frame(ccord_ui *ui, ccord_renderer *r, ccord_rect box, float radius, ccord_color fill, uis_elevation e)
{
  uis_shadow(r, box, radius, e);
  ccord_draw_rrect(r, (ccord_rect){box.x - 1.0f, box.y - 1.0f, box.w + 2.0f, box.h + 2.0f}, radius + 1.0f,
                   CO(border_subtle));
  ccord_draw_rrect(r, box, radius, fill);
}

void uis_popover_frame(ccord_ui *ui, ccord_renderer *r, ccord_rect box)
{
  float rad = TH(composer_radius);
  uis_frame(ui, r, box, rad, CO(bg_floating), UIS_ELEV_POPOVER);
  audit(UIS_K_POPOVER, box, rad, 0.0f, CO(bg_floating));
}

void uis_modal_frame(ccord_ui *ui, ccord_renderer *r, ccord_rect box)
{
  float rad = TH(composer_radius);
  ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h}, CO(overlay));
  uis_frame(ui, r, box, rad, CO(bg_secondary), UIS_ELEV_MODAL);
  audit(UIS_K_MODAL, box, rad, 0.0f, CO(bg_secondary));
}

ccord_rect uis_modal_box(const ccord_ui *ui, float w, float h)
{
  float sl = ui->mobile_layout ? ui->safe_insets.left : 0.0f;
  float sr = ui->mobile_layout ? ui->safe_insets.right : 0.0f;
  float st = ui->mobile_layout ? ui->safe_insets.top : 0.0f;
  float sb = ui->mobile_layout ? fmaxf(ui->safe_insets.bottom, ui->keyboard_inset) : 0.0f;
  float pad = ui->mobile_layout ? UIS_SP_3 : UIS_SP_8;
  float uw = ui->win_w - sl - sr;
  float uh = ui->win_h - st - sb;
  float maxw = uw - 2.0f * pad;
  float maxh = uh - 2.0f * pad;
  if (maxw < 240.0f) maxw = uw > 0.0f ? uw : ui->win_w;
  if (maxh < 160.0f) maxh = uh > 0.0f ? uh : ui->win_h;
  if (w > maxw) w = maxw;
  if (h > maxh) h = maxh;
  float y = st + (uh - h) * UIS_MODAL_Y_BIAS;
  if (y < st + UIS_SP_3) y = st + UIS_SP_3;
  return (ccord_rect){sl + (uw - w) * 0.5f, y, w, h};
}

float uis_modal_title(ccord_ui *ui, ccord_renderer *r, ccord_rect box, const char *title)
{
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, TH(font_size_large), CO(text_header));
  ccord_text_draw_ellipsis(r, title, strlen(title), box.x + UIS_SP_5, box.y + UIS_SP_5, box.w - 2.0f * UIS_SP_5, &ts);
  return box.y + UIS_SP_5 + ui_line_h(ui, ts.size) + UIS_SP_3;
}

ccord_rect uis_modal_footer_button(ccord_rect box, int index_from_right, float w)
{
  // All slots in the row are `w` wide; a row of mixed widths positions itself from the box edge and UIS_SP_5 / UIS_SP_2.
  float x = box.x + box.w - UIS_SP_5 - (float)(index_from_right + 1) * w - (float)index_from_right * UIS_SP_2;
  return (ccord_rect){x, box.y + box.h - UIS_SP_5 - UIS_H_BTN, w, UIS_H_BTN};
}

void uis_divider(ccord_ui *ui, ccord_renderer *r, float x, float y, float w)
{
  ccord_draw_rect(r, (ccord_rect){x, y, w, 1.0f}, CO(divider));
}

void uis_vdivider(ccord_ui *ui, ccord_renderer *r, float x, float y, float h)
{
  (void)ui;
  if (h <= 0.0f) return;
  ccord_rect rc = {x, y, 1.0f, h};
  ccord_draw_rect(r, rc, CO(divider));
  audit(UIS_K_VDIVIDER, rc, 0.0f, 0.0f, CO(divider));
}

// ---------------------------------------------------------------- buttons

#define UIS_SLIDER_EASE_MS 150u   // a slider fill moves to a new value over this long

float uis_button_height(uis_btn_size size)
{
  return size == UIS_BTN_S_SM ? UIS_H_BTN_SM : (size == UIS_BTN_S_LG ? UIS_H_BTN_LG : UIS_H_BTN);
}

static void centered_label(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, float size, ccord_color fg)
{
  ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, size, fg);
  float pad = UIS_SP_3;
  float maxw = rc.w - 2.0f * pad;
  if (maxw < 8.0f) maxw = rc.w;
  ccord_vec2 m = ccord_text_measure(r, label, strlen(label), &st, 0.0f);
  float x = m.x < maxw ? rc.x + (rc.w - m.x) * 0.5f : rc.x + (rc.w - maxw) * 0.5f;
  ccord_text_draw_ellipsis(r, label, strlen(label), x, rc.y + (rc.h - m.y) * 0.5f, maxw, &st);
}

void uis_button(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, uis_btn_kind kind,
                uis_btn_size size, uis_state st, const char *ns, uint64_t id)
{
  if (rc.w <= 0.0f || rc.h <= 0.0f) return;
  float hv = ui_fade(ui, ns, id, st.hover && !st.disabled);
  float pv = ui_fade(ui, ns, id ^ UIS_SALT_PRESS, st.pressed && !st.disabled);
  float fv = ui_fade(ui, ns, id ^ UIS_SALT_FOCUS, st.focused && !st.disabled);
  ccord_color rest = CO(bg_tertiary), over = CO(bg_item_hover), fg = CO(text_header);
  switch (kind) {
    case UIS_BTN_PRIMARY: rest = CO(brand); over = CO(brand_hover); fg = CO(text_on_brand); break;
    case UIS_BTN_SECONDARY:
      if (st.selected) rest = over = CO(bg_item_selected);
      break;
    case UIS_BTN_DANGER: rest = CO(danger); over = ui_mix(CO(danger), CO(text_header), 0.18f); fg = CO(text_on_brand); break;
    case UIS_BTN_SUCCESS:
      rest = CO(status_online);
      over = ui_mix(CO(status_online), CO(text_header), 0.18f);
      fg = CO(text_on_brand);
      break;
    case UIS_BTN_LINK: rest = ui_alpha(CO(bg_item_hover), 0.0f); fg = CO(text_link); break;
    case UIS_BTN_DANGER_SOFT:
      over = ui_mix(CO(danger), CO(text_header), 0.18f);
      fg = ui_mix(CO(text_header), CO(text_on_brand), hv);
      break;
  }
  ccord_color bg = ui_mix(rest, over, hv);
  if (pv > 0.004f) bg = ui_mix(bg, darken(kind == UIS_BTN_LINK ? CO(bg_item_hover) : over, UIS_PRESSED_DARKEN), pv);
  if (st.disabled) {
    bg = ui_alpha(bg, UIS_DISABLED_BG);
    fg = ui_alpha(fg, UIS_DISABLED_FG);
  }
  float rad = TH(channel_item_radius);
  float fs = size == UIS_BTN_S_LG ? TH(font_size_base) : TH(font_size_small);
  if (fv > 0.004f) {
    // Keyboard focus: the same 1 px outset brand ring an input gets, eased in and out.
    ccord_draw_rrect(r, (ccord_rect){rc.x - 1.0f, rc.y - 1.0f, rc.w + 2.0f, rc.h + 2.0f}, rad + 1.0f,
                     ui_alpha(CO(brand), fv));
  }
  ccord_draw_rrect(r, rc, rad, bg);
  centered_label(ui, r, rc, label, fs, fg);
  audit(UIS_K_BUTTON, rc, rad, fs, bg);
}

ccord_color uis_icon_button(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, uis_state st, const char *ns, uint64_t id)
{
  float h = ui_fade(ui, ns, id * 2u, st.hover && !st.selected && !st.disabled);
  float s = ui_fade(ui, ns, id * 2u + 1u, st.selected);
  float rad = TH(channel_item_radius);
  ccord_color bg = ui_mix(ui_mix(CO(bg_primary), CO(bg_item_hover), h), CO(bg_item_selected), s);
  if (st.pressed && !st.disabled) bg = darken(CO(bg_item_selected), UIS_PRESSED_DARKEN);
  if (h > 0.004f || s > 0.004f || st.pressed) ccord_draw_rrect(r, rc, rad, bg);
  ccord_color ic = ui_mix(ui_mix(CO(text_secondary), CO(text_interactive_hover), h), CO(text_interactive_active), s);
  if (st.disabled) ic = ui_alpha(ic, UIS_DISABLED_FG);
  audit(UIS_K_ICON_BUTTON, rc, rad, 0.0f, bg);
  return ic;
}

// ---------------------------------------------------------------- rows

void uis_menu_row(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, uis_state st, bool has_sub,
                  bool checked, bool danger, const char *ns, uint64_t id)
{
  float hv = ui_fade(ui, ns, id, (st.hover || st.selected) && !st.disabled);
  ccord_color fill = danger ? CO(danger) : CO(brand);
  float rad = TH(channel_item_radius);
  if (hv > 0.004f) ccord_draw_rrect(r, rc, rad, ui_alpha(fill, hv));
  ccord_color rest = danger ? CO(danger) : CO(text_normal);
  ccord_color fg = ui_mix(rest, CO(text_on_brand), hv);
  if (st.disabled) fg = ui_alpha(fg, UIS_DISABLED_FG);
  float fs = TH(font_size_small);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, fs, fg);
  float reserve = (has_sub || checked) ? 18.0f : 0.0f;
  ccord_text_draw_ellipsis(r, label, strlen(label), rc.x + UIS_SP_2, rc.y + (rc.h - ui_line_h(ui, fs)) * 0.5f,
                           rc.w - 2.0f * UIS_SP_2 - reserve, &ts);
  float cy = rc.y + rc.h * 0.5f;
  if (has_sub) ui_icon_chevron(r, rc.x + rc.w - 12.0f, cy, 12.0f, ICON_RIGHT, fg);
  if (checked) ccord_draw_circle(r, rc.x + rc.w - 12.0f, cy, 3.5f, ui_mix(CO(brand), CO(text_on_brand), hv));
  audit(UIS_K_MENU_ROW, rc, rad, fs, ui_alpha(fill, hv));
}

void uis_list_row_fill(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, uis_state st, const char *ns, uint64_t id)
{
  float rad = TH(channel_item_radius);
  ui_state_fill(ui, r, rc, rad, ns, id, st.hover && !st.disabled, st.selected);
  audit(UIS_K_LIST_ROW, rc, rad, 0.0f, st.selected ? CO(bg_item_selected) : CO(bg_item_hover));
}

// ---------------------------------------------------------------- tooltip

ccord_rect uis_tooltip_begin(ccord_ui *ui, ccord_renderer *r, ccord_rect anchor, float content_w, float content_h,
                             uis_tip_side side, uint64_t id)
{
  float fs = TH(font_size_small);
  ccord_rect tip = {0.0f, 0.0f, content_w + 2.0f * UIS_SP_3, content_h + 2.0f * UIS_SP_2};
  const float gap = 6.0f, margin = 8.0f;
  switch (side) {
    case UIS_TIP_BELOW:
      tip.x = anchor.x + (anchor.w - tip.w) * 0.5f;
      tip.y = anchor.y + anchor.h + gap;
      break;
    case UIS_TIP_ABOVE:
      tip.x = anchor.x + (anchor.w - tip.w) * 0.5f;
      tip.y = anchor.y - tip.h - gap;
      if (tip.y < margin) tip.y = anchor.y + anchor.h + gap;   // no room above: flip
      break;
    case UIS_TIP_RIGHT:
      tip.x = anchor.x + anchor.w + gap;
      tip.y = anchor.y + (anchor.h - tip.h) * 0.5f;
      break;
  }
  if (tip.x + tip.w > ui->win_w - margin) tip.x = ui->win_w - margin - tip.w;
  if (tip.x < margin) tip.x = margin;
  if (tip.y + tip.h > ui->win_h - margin) tip.y = ui->win_h - margin - tip.h;
  if (tip.y < margin) tip.y = margin;
  float rad = TH(context_menu_radius);
  ui_popup_begin(ui, r, "uistip", id, UIS_MS_TIP, 0.0f);
  uis_frame(ui, r, tip, rad, CO(bg_floating), UIS_ELEV_POPOVER);
  audit(UIS_K_TOOLTIP, tip, rad, fs, CO(bg_floating));
  return tip;
}

void uis_tooltip_end(ccord_renderer *r)
{
  ui_popup_end(r);
}

ccord_rect uis_tooltip(ccord_ui *ui, ccord_renderer *r, ccord_rect anchor, const char *text, uis_tip_side side,
                       uint64_t id)
{
  float fs = TH(font_size_small);
  ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, fs, CO(text_header));
  ccord_vec2 t = ccord_text_measure(r, text, strlen(text), &st, 0.0f);
  ccord_rect tip = uis_tooltip_begin(ui, r, anchor, t.x, t.y, side, id);
  ccord_text_draw(r, text, strlen(text), tip.x + UIS_SP_3, tip.y + UIS_SP_2, &st, 0.0f);
  uis_tooltip_end(r);
  return tip;
}

// ---------------------------------------------------------------- input

float uis_input(ccord_ui *ui, ccord_renderer *r, ccord_rect box, const ccord_text_edit *ed, const char *placeholder,
                uis_state st, bool masked, const char *ns, uint64_t id)
{
  return uis_input_padded(ui, r, box, UIS_SP_3, UIS_SP_3, ed, placeholder, st, masked, ns, id);
}

float uis_input_padded(ccord_ui *ui, ccord_renderer *r, ccord_rect box, float pad_l, float pad_r,
                       const ccord_text_edit *ed, const char *placeholder, uis_state st, bool masked, const char *ns,
                       uint64_t id)
{
  float rad = TH(channel_item_radius);
  float ring = ui_fade(ui, ns, id, st.focused && !st.disabled);
  if (ring > 0.004f) {
    ccord_draw_rrect(r, (ccord_rect){box.x - 1.0f, box.y - 1.0f, box.w + 2.0f, box.h + 2.0f}, rad + 1.0f,
                     ui_alpha(CO(brand), ring));
  }
  ccord_color bg = st.disabled ? ui_alpha(CO(bg_input), UIS_DISABLED_BG + 0.2f) : CO(bg_input);
  ccord_draw_rrect(r, box, rad, bg);
  float fs = TH(font_size_base);
  float lh = ui_line_h(ui, fs);
  float ty = box.y + (box.h - lh) * 0.5f;
  float tx = box.x + pad_l;
  float inner = box.w - pad_l - pad_r;
  if (ed->len == 0) {
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, fs, CO(text_placeholder));
    ccord_text_draw_ellipsis(r, placeholder, strlen(placeholder), tx, ty, inner, &ps);
  } else {
    char mask[128];
    const char *text = ed->data;
    size_t len = ed->len, caret = ed->caret, anchor = ed->anchor;
    if (masked) {
      // One bullet per byte keeps caret offsets 1:1 with the edit (the original ASCII mask did the same).
      len = ed->len < sizeof mask - 1u ? ed->len : sizeof mask - 1u;
      memset(mask, '*', len);
      mask[len] = '\0';
      text = mask;
      if (caret > len) caret = len;
      if (anchor > len) anchor = len;
    }
    ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, fs, st.disabled ? ui_alpha(CO(text_normal), UIS_DISABLED_FG)
                                                                            : CO(text_normal));
    float caret_x = ccord_text_measure(r, text, caret, &ts, 0.0f).x;
    float shift = caret_x > inner ? caret_x - inner : 0.0f;   // a long text scrolls so the caret stays visible
    ccord_push_clip(r, (ccord_rect){box.x + UIS_SP_1, box.y, box.w - 2.0f * UIS_SP_1, box.h});
    if (st.focused && anchor != caret) {
      size_t lo = anchor < caret ? anchor : caret, hi = anchor < caret ? caret : anchor;
      float x0 = ccord_text_measure(r, text, lo, &ts, 0.0f).x, x1 = ccord_text_measure(r, text, hi, &ts, 0.0f).x;
      ccord_draw_rect(r, (ccord_rect){tx - shift + x0, ty, x1 - x0, lh}, CO(selection));
    }
    ccord_text_draw(r, text, len, tx - shift, ty, &ts, 0.0f);
    if (st.focused && ui->caret_on) ccord_draw_rect(r, (ccord_rect){tx - shift + caret_x, ty, 1.5f, lh}, CO(caret));
    ccord_pop_clip(r);
  }
  if (st.focused && ed->len == 0 && ui->caret_on) ccord_draw_rect(r, (ccord_rect){tx, ty, 1.5f, lh}, CO(caret));
  if (st.focused) ui->text_area = box;   // where the OS puts its input-method window
  audit(UIS_K_INPUT, box, rad, fs, bg);
  return tx;
}

// ---------------------------------------------------------------- toggle

ccord_rect uis_toggle_rect(ccord_rect row)
{
  return (ccord_rect){row.x + row.w - UIS_TOGGLE_W, row.y + (row.h - UIS_TOGGLE_H) * 0.5f, UIS_TOGGLE_W, UIS_TOGGLE_H};
}

void uis_toggle(ccord_ui *ui, ccord_renderer *r, ccord_rect row, bool on, uis_state st, const char *ns, uint64_t id)
{
  ccord_rect t = uis_toggle_rect(row);
  ui_draw_switch(ui, r, t, on, st.hover, st.disabled, ns, id);
  audit(UIS_K_TOGGLE, t, t.h * 0.5f, 0.0f, on ? CO(brand) : CO(bg_tertiary));
}

// ---------------------------------------------------------------- label width and tab

float uis_label_width(ccord_ui *ui, ccord_renderer *r, const char *label)
{
  ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, TH(font_size_small), CO(text_header));
  return ccord_text_measure(r, label, strlen(label), &st, 0.0f).x + 2.0f * UIS_SP_3;
}

void uis_tab(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, uis_state st, const char *ns, uint64_t id)
{
  if (rc.w <= 0.0f || rc.h <= 0.0f) return;
  float hv = ui_fade(ui, ns, id, st.hover && !st.selected && !st.disabled);
  float sv = ui_fade(ui, ns, id ^ UIS_SALT_SELECT, st.selected);
  float rad = TH(channel_item_radius);
  float fs = TH(font_size_small);
  ccord_color fill = ui_mix(ui_alpha(CO(bg_item_hover), hv), CO(brand), sv);
  if (sv > 0.004f || hv > 0.004f) ccord_draw_rrect(r, rc, rad, fill);
  ccord_color fg = ui_mix(ui_mix(CO(text_secondary), CO(text_header), hv), CO(text_on_brand), sv);
  if (st.disabled) fg = ui_alpha(fg, UIS_DISABLED_FG);
  ccord_text_style ts = ui_style(ui, st.selected ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, fs, fg);
  ccord_vec2 m = ccord_text_measure(r, label, strlen(label), &ts, 0.0f);
  ccord_text_draw_ellipsis(r, label, strlen(label), rc.x + (rc.w - m.x) * 0.5f, rc.y + (rc.h - m.y) * 0.5f, rc.w, &ts);
  audit(UIS_K_TAB, rc, rad, fs, fill);
}

void uis_surface(ccord_ui *ui, ccord_renderer *r, ccord_rect box, ccord_color fill)
{
  (void)ui;
  if (box.w <= 0.0f || box.h <= 0.0f) return;
  float rad = TH(composer_radius);
  ccord_draw_rrect(r, box, rad, fill);
  audit(UIS_K_SURFACE, box, rad, 0.0f, fill);
}

// ---------------------------------------------------------------- chip

void uis_chip(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, uis_state st, const char *ns,
              uint64_t id)
{
  if (rc.w <= 0.0f || rc.h <= 0.0f) return;
  uis_tokens t = uis_tokens_of(ui);
  // An unselected chip eases to bg_item_hover under the pointer; a selected chip stays brand whatever the pointer does.
  float hv = ui_fade(ui, ns, id * 2u, st.hover && !st.selected && !st.disabled);
  float sel = ui_fade(ui, ns, id * 2u + 1u, st.selected && !st.disabled);
  ccord_color fill = ui_mix(ui_mix(CO(bg_tertiary), CO(bg_item_hover), hv), CO(brand), sel);
  ccord_color fg = ui_mix(CO(text_header), CO(text_on_brand), sel);
  if (st.disabled) {
    fill = ui_alpha(fill, UIS_DISABLED_BG);
    fg = ui_alpha(fg, UIS_DISABLED_FG);
  }
  ccord_rect pill = {rc.x, rc.y + (rc.h - UIS_H_CHIP) * 0.5f, rc.w, UIS_H_CHIP};
  float rad = UIS_H_CHIP * 0.5f;
  ccord_draw_rrect(r, pill, rad, fill);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, t.fs_caption, fg);
  ccord_vec2 m = ccord_text_measure(r, label, strlen(label), &ts, 0.0f);
  float x = m.x < pill.w ? pill.x + (pill.w - m.x) * 0.5f : pill.x + UIS_SP_2;
  ccord_text_draw_ellipsis(r, label, strlen(label), x, pill.y + (pill.h - m.y) * 0.5f, pill.w - 2.0f * UIS_SP_2, &ts);
  audit(UIS_K_CHIP, pill, rad, t.fs_caption, fill);
}

// ---------------------------------------------------------------- select

void uis_select(ccord_ui *ui, ccord_renderer *r, ccord_rect box, const char *value, uis_state st, const char *ns,
                uint64_t id)
{
  if (box.w <= 0.0f || box.h <= 0.0f) return;
  uis_tokens t = uis_tokens_of(ui);
  float hv = ui_fade(ui, ns, id * 2u, st.hover && !st.disabled);
  float ring = ui_fade(ui, ns, id * 2u + 1u, (st.focused || st.selected) && !st.disabled);
  if (ring > 0.004f) {
    ccord_draw_rrect(r, (ccord_rect){box.x - 1.0f, box.y - 1.0f, box.w + 2.0f, box.h + 2.0f}, t.r_control + 1.0f,
                     ui_alpha(CO(brand), ring));
  }
  ccord_color fill = ui_mix(CO(bg_input), CO(bg_item_hover), hv);
  ccord_color fg = CO(text_normal), ic = CO(text_secondary);
  if (st.disabled) {
    fill = ui_alpha(fill, UIS_DISABLED_BG);
    fg = ui_alpha(fg, UIS_DISABLED_FG);
    ic = ui_alpha(ic, UIS_DISABLED_FG);
  }
  ccord_draw_rrect(r, box, t.r_control, fill);
  float reserve = UIS_SP_4 + UIS_SP_3 + UIS_SP_2;   // the chevron and its gap, on the right
  float tw = box.w - UIS_SP_3 - reserve;
  if (value && value[0] && tw > 0.0f) {
    ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, t.fs_body, fg);
    ccord_text_draw_ellipsis(r, value, strlen(value), box.x + UIS_SP_3,
                             box.y + (box.h - ui_line_h(ui, t.fs_body)) * 0.5f, tw, &ts);
  }
  ui_icon_chevron(r, box.x + box.w - UIS_SP_4, box.y + box.h * 0.5f, UIS_SP_3, ICON_DOWN, ic);
  audit(UIS_K_SELECT, box, t.r_control, t.fs_body, fill);
}

// ---------------------------------------------------------------- swatch

void uis_swatch(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, ccord_color colour, uis_state st, const char *ns,
                uint64_t id)
{
  if (rc.w <= 0.0f || rc.h <= 0.0f) return;
  float hv = ui_fade(ui, ns, id * 2u, st.hover && !st.disabled);
  float sel = ui_fade(ui, ns, id * 2u + 1u, st.selected);
  ccord_color ring = ui_mix(ui_mix(CO(border_subtle), CO(text_secondary), hv), CO(brand), sel);
  if (st.disabled) colour = ui_alpha(colour, UIS_DISABLED_FG);
  float rad = (rc.w < rc.h ? rc.w : rc.h) * 0.5f;
  float cx = rc.x + rc.w * 0.5f, cy = rc.y + rc.h * 0.5f;
  ccord_draw_circle(r, cx, cy, rad, ring);
  ccord_draw_circle(r, cx, cy, rad - 2.0f, colour);
  audit(UIS_K_SWATCH, rc, rad, 0.0f, colour);
}

// ---------------------------------------------------------------- radio

void uis_radio(ccord_ui *ui, ccord_renderer *r, ccord_rect row, const char *label, bool on, uis_state st, const char *ns,
               uint64_t id)
{
  if (row.w <= 0.0f || row.h <= 0.0f) return;
  uis_tokens t = uis_tokens_of(ui);
  float hv = ui_fade(ui, ns, id * 2u, st.hover && !st.disabled);
  ccord_color fill = ui_alpha(CO(bg_item_hover), hv);
  if (hv > 0.004f) ccord_draw_rrect(r, row, t.r_control, fill);
  float s = ui_fade(ui, ns, id * 2u + 1u, on);
  ccord_color ring = ui_mix(CO(text_secondary), CO(brand), s), dot = CO(brand);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, t.fs_control, CO(text_normal));
  if (st.disabled) {
    ring = ui_alpha(ring, UIS_DISABLED_FG);
    dot = ui_alpha(dot, UIS_DISABLED_FG);
    ts.color = ui_alpha(CO(text_normal), UIS_DISABLED_FG);
  }
  float rad = UIS_RADIO_D * 0.5f;
  float cx = row.x + UIS_SP_2 + rad, cy = row.y + row.h * 0.5f;
  ccord_draw_circle(r, cx, cy, rad, ring);            // the 2 px ring
  ccord_draw_circle(r, cx, cy, rad - 2.0f, CO(bg_primary));
  if (s > 0.004f) ccord_draw_circle(r, cx, cy, (rad - 5.0f) * s, dot);
  float tx = row.x + UIS_SP_2 + UIS_RADIO_D + UIS_SP_3;
  float tw = row.x + row.w - UIS_SP_2 - tx;
  if (label && label[0] && tw > 0.0f) {
    ccord_text_draw_ellipsis(r, label, strlen(label), tx, row.y + (row.h - ui_line_h(ui, t.fs_control)) * 0.5f, tw, &ts);
  }
  audit(UIS_K_RADIO, row, t.r_control, t.fs_control, fill);
}

// ---------------------------------------------------------------- slider, card
// A slider is keyed by its track position (the caller passes no id); two tracks never share a spot.
static uint64_t slider_id(ccord_rect track)
{
  return (uint64_t)(int64_t)(track.x * 16.0f) * 65599u + (uint64_t)(int64_t)(track.y * 16.0f);
}

void uis_slider(ccord_ui *ui, ccord_renderer *r, ccord_rect track, float fraction, uis_state st)
{
  if (track.w <= 0.0f || track.h <= 0.0f) return;
  float f = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
  // The fill eases to a new value (a click-to-set or a +/- step); the first sight of a track snaps to it.
  f = ccord_anim_to(&ui->anim, ccord_anim_key("uisl", slider_id(track)), f, UIS_SLIDER_EASE_MS);
  float rad = track.h * 0.5f;
  ccord_draw_rrect(r, track, rad, CO(bg_tertiary));
  float fill = track.w * f;
  ccord_color fc = st.disabled ? ui_alpha(CO(brand), UIS_DISABLED_BG) : CO(brand);
  if (fill > 0.0f) ccord_draw_rrect(r, (ccord_rect){track.x, track.y, fill, track.h}, rad, fc);
  ccord_draw_circle(r, track.x + fill, track.y + track.h * 0.5f, 6.0f,
                    st.disabled ? ui_alpha(CO(text_header), 0.5f) : CO(text_header));
  audit(UIS_K_SLIDER, track, rad, 0.0f, fc);
}

float uis_anim_value(const ccord_ui *ui, const char *ns, uint64_t id)
{
  uint32_t key = ccord_anim_key(ns, id);
  size_t base = key % CCORD_ANIM_SLOTS;
  for (size_t i = 0; i < 8u; i++) {
    const ccord_anim_slot *s = &ui->anim.slots[(base + i) % CCORD_ANIM_SLOTS];
    if (s->key == key) return s->value;
  }
  return -1.0f;
}

float uis_slider_shown(const ccord_ui *ui, ccord_rect track)
{
  return uis_anim_value(ui, "uisl", slider_id(track));
}

void uis_card(ccord_ui *ui, ccord_renderer *r, ccord_rect box)
{
  if (box.w <= 0.0f || box.h <= 0.0f) return;
  float rad = TH(composer_radius);
  ccord_draw_rrect(r, box, rad, CO(bg_secondary));
  audit(UIS_K_CARD, box, rad, 0.0f, CO(bg_secondary));
}

// ---------------------------------------------------------------- scrollbar

void uis_scrollbar(ccord_ui *ui, ccord_renderer *r, ccord_rect track, ccord_rect thumb)
{
  (void)ui;
  if (track.w <= 0.0f || track.h <= 0.0f) return;
  float rad = track.w * 0.5f;
  ccord_draw_rrect(r, track, rad, CO(bg_tertiary));
  if (thumb.h > 0.0f) ccord_draw_rrect(r, thumb, rad, CO(text_muted));
  audit(UIS_K_SCROLLBAR, track, rad, 0.0f, CO(bg_tertiary));
}

void uis_scroll_arrow(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, bool up, uis_state st, const char *ns, uint64_t id)
{
  if (rc.w <= 0.0f || rc.h <= 0.0f) return;
  ccord_color ic = uis_icon_button(ui, r, rc, st, ns, id);
  float cx = rc.x + rc.w * 0.5f, cy = rc.y + rc.h * 0.5f;
  if (up) {
    // The shared icon helper only draws down and right chevrons, so the up one is two strokes.
    float s = 3.5f;
    ccord_draw_line(r, (ccord_vec2){cx - s * 0.7f, cy + s * 0.32f}, (ccord_vec2){cx, cy - s * 0.38f}, 1.0f, ic);
    ccord_draw_line(r, (ccord_vec2){cx, cy - s * 0.38f}, (ccord_vec2){cx + s * 0.7f, cy + s * 0.32f}, 1.0f, ic);
  } else {
    ui_icon_chevron(r, cx, cy, 7.0f, ICON_DOWN, ic);
  }
}

// ---------------------------------------------------------------- plate

void uis_plate(ccord_ui *ui, ccord_renderer *r, ccord_rect box, float radius, ccord_color fill)
{
  (void)ui;
  if (box.w <= 0.0f || box.h <= 0.0f) return;
  ccord_draw_rrect(r, box, radius, fill);
  audit(UIS_K_PLATE, box, radius, 0.0f, fill);
}

// ---------------------------------------------------------------- audit read-out for the ui-style slice (style_debug.h)

_Static_assert((int)CCORD_UI_STYLE_KIND_COUNT == (int)UIS_K_COUNT, "style_debug.h kinds must match uis_kind");
_Static_assert(CCORD_UI_STYLE_RECORDS == UIS_AUDIT_MAX, "style_debug.h record table must match the audit table");
_Static_assert((int)CCORD_UI_STYLE_BUTTON == (int)UIS_K_BUTTON, "style_debug.h button kind order");
_Static_assert((int)CCORD_UI_STYLE_TOOLTIP == (int)UIS_K_TOOLTIP, "style_debug.h tooltip kind order");
_Static_assert((int)CCORD_UI_STYLE_SCROLLBAR == (int)UIS_K_SCROLLBAR, "style_debug.h scrollbar kind order");
_Static_assert((int)CCORD_UI_STYLE_CARD == (int)UIS_K_CARD, "style_debug.h card kind order");
_Static_assert((int)CCORD_UI_STYLE_SURFACE == (int)UIS_K_SURFACE, "style_debug.h surface kind order");
_Static_assert((int)CCORD_UI_STYLE_CHIP == (int)UIS_K_CHIP, "style_debug.h chip kind order");

void ccord_ui_get_style_debug(const ccord_ui *ui, ccord_ui_style_debug *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  const uis_audit *a = uis_audit_get();
  for (int k = 0; k < (int)UIS_K_COUNT; k++) {
    out->drawn[k] = a->kind[k].count;
    out->last_rect[k] = a->kind[k].rect;
    out->last_radius[k] = a->kind[k].radius;
    out->last_font_px[k] = a->kind[k].font_px;
    out->last_fill[k] = a->kind[k].fill;
  }
  out->total = a->total;
  out->kept = a->kept;
  for (uint32_t i = 0; i < a->kept; i++) {
    out->records[i] = (ccord_ui_style_record){.kind = (ccord_ui_style_kind)a->rec[i].kind, .rect = a->rec[i].rect,
                                              .radius = a->rec[i].radius, .font_px = a->rec[i].font_px,
                                              .fill = a->rec[i].fill};
  }
  if (!ui) return;
  uis_tokens t = uis_tokens_of(ui);
  out->r_control = t.r_control;
  out->r_menu = t.r_menu;
  out->r_surface = t.r_surface;
  out->fs_title = t.fs_title;
  out->fs_body = t.fs_body;
  out->fs_control = t.fs_control;
  out->fs_caption = t.fs_caption;
  out->fs_overline = t.fs_overline;
  out->menu_row_h = t.menu_row_h;
  out->list_row_h = t.list_row_h;
}

const char *ccord_ui_style_kind_name(ccord_ui_style_kind kind)
{
  return uis_kind_name((uis_kind)kind);
}

void ccord_ui_reset_style_audit(ccord_ui *ui)
{
  (void)ui;
  uis_audit_reset();
}
