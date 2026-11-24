// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "plugin_panel_internal.h"

// ---------------------------------------------------------------- intrinsic measure & rendering

static float btn_default_h(pbtn_sz sz)
{
  switch (sz) {
    case PBTN_XS: return 18.0f;
    case PBTN_SM: return 24.0f;
    case PBTN_MD: return UIS_H_BTN_SM;
  }
  return 24.0f;
}

static bool child_visible(const pnode *c, bool parent_hovered)
{
  if (c->show_on_hover && !parent_hovered) return false;
  if (c->hide_on_hover && parent_hovered) return false;
  return true;
}

static ccord_vec2 measure_node(ccord_ui *ui, ccord_renderer *r, const pnode *nd, bool parent_is_col, bool parent_hovered)
{
  if (!nd) return (ccord_vec2){0.0f, 0.0f};
  float w = nd->w, h = nd->h;
  switch (nd->kind) {
    case PN_SPACER: break;
    case PN_DIVIDER: {
      bool vert = nd->orient_explicit ? nd->is_vertical : !parent_is_col;
      if (vert) { if (w <= 0.0f) w = nd->thickness; }
      else { if (h <= 0.0f) h = nd->thickness; }
      break;
    }
    case PN_ICON:
      if (w <= 0.0f) w = nd->icon_size;
      if (h <= 0.0f) h = nd->icon_size;
      break;
    case PN_TEXT:
    case PN_MARKDOWN: {
      float fs = pp_resolve_font_size(ui, nd->font_size);
      ccord_text_style st = {.font = nd->bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, .size = fs, .color = ui->theme->c.text_normal};
      float wrap = (nd->kind == PN_MARKDOWN && nd->max_w > 0.0f) ? nd->max_w : 0.0f;
      ccord_vec2 m = ccord_text_measure(r, nd->text, strlen(nd->text), &st, wrap);
      if (w <= 0.0f) w = m.x;
      if (h <= 0.0f) h = m.y;
      break;
    }
    case PN_BUTTON: {
      float bh = h > 0.0f ? h : btn_default_h(nd->btn_sz);
      h = bh;
      if (w <= 0.0f) {
        bool has_lbl = nd->text && nd->text[0];
        bool has_ic = nd->icon != PICON_NONE;
        if (!has_lbl && has_ic) w = bh;
        else {
          float fs = ui->theme->m.font_size_small - (nd->btn_sz == PBTN_XS ? 2.0f : 1.0f);
          ccord_text_style st = {.font = CCORD_FONT_BOLD, .size = fs, .color = ui->theme->c.text_header};
          ccord_vec2 m = has_lbl ? ccord_text_measure(r, nd->text, strlen(nd->text), &st, 0.0f) : (ccord_vec2){0.0f, 0.0f};
          float pad_x = nd->btn_sz == PBTN_XS ? 6.0f : 10.0f;
          w = m.x + pad_x * 2.0f + (has_ic ? nd->icon_size + 5.0f : 0.0f);
        }
      }
      break;
    }
    case PN_INPUT:
    case PN_SELECT:
      if (w <= 0.0f) w = 160.0f;
      if (h <= 0.0f) h = UIS_H_BTN;
      break;
    case PN_TOGGLE:
      if (w <= 0.0f) w = (nd->text && nd->text[0]) ? 160.0f : UIS_TOGGLE_W;
      if (h <= 0.0f) h = UIS_TOGGLE_H;
      break;
    case PN_CHECKBOX:
    case PN_RADIO: {
      float lw = 0.0f;
      if (nd->text && nd->text[0]) {
        ccord_text_style st = {.font = CCORD_FONT_REGULAR, .size = ui->theme->m.font_size_small, .color = ui->theme->c.text_normal};
        lw = ccord_text_measure(r, nd->text, strlen(nd->text), &st, 0.0f).x + 8.0f;
      }
      if (w <= 0.0f) w = UIS_RADIO_D + lw + 8.0f;
      if (h <= 0.0f) h = 24.0f;
      break;
    }
    case PN_SLIDER:
      if (w <= 0.0f) w = 140.0f;
      if (h <= 0.0f) h = 20.0f;
      break;
    case PN_TABS: {
      float sum = 0.0f;
      for (size_t i = 0; i < nd->nopts; i++) {
        if (i) sum += nd->gap;
        sum += uis_label_width(ui, r, nd->opts[i].label);
      }
      if (w <= 0.0f) w = pp_p_maxf(sum, 40.0f);
      if (h <= 0.0f) h = UIS_H_BTN_SM;
      break;
    }
    case PN_CHIP: {
      ccord_text_style st = {.font = CCORD_FONT_BOLD, .size = ui->theme->m.font_size_small - 2.0f, .color = ui->theme->c.text_header};
      ccord_vec2 tm = ccord_text_measure(r, nd->text, strlen(nd->text), &st, 0.0f);
      if (w <= 0.0f) w = tm.x + 14.0f;
      if (h <= 0.0f) h = UIS_H_CHIP;
      break;
    }
    case PN_PROGRESS:
      if (w <= 0.0f) w = 120.0f;
      if (h <= 0.0f) h = 8.0f;
      break;
    case PN_IMAGE:
      if (w <= 0.0f) w = 48.0f;
      if (h <= 0.0f) h = 48.0f;
      break;
    case PN_AVATAR:
      if (w <= 0.0f) w = nd->icon_size;
      if (h <= 0.0f) h = nd->icon_size;
      break;
    case PN_CANVAS:
      break;
    case PN_BOX:
    case PN_SCROLL: {
      float sum_main = 0.0f, max_cross = 0.0f;
      size_t vis = 0;
      for (size_t i = 0; i < nd->nchildren; i++) {
        const pnode *c = nd->children[i];
        if (!child_visible(c, parent_hovered) || c->pos_abs) continue;
        ccord_vec2 cm = measure_node(ui, r, c, nd->is_col, parent_hovered);
        float c_main = nd->is_col ? cm.y : cm.x;
        float c_cross = nd->is_col ? cm.x : cm.y;
        sum_main += c_main;
        if (c_cross > max_cross) max_cross = c_cross;
        vis++;
      }
      if (vis > 1u) sum_main += nd->gap * (float)(vis - 1u);
      float auto_w = (nd->is_col ? max_cross : sum_main) + nd->pad_l + nd->pad_r;
      float auto_h = (nd->is_col ? sum_main : max_cross) + nd->pad_t + nd->pad_b;
      if (w <= 0.0f) w = auto_w;
      if (h <= 0.0f) h = auto_h;
      break;
    }
  }
  if (nd->min_w > 0.0f && w < nd->min_w) w = nd->min_w;
  if (nd->max_w > 0.0f && w > nd->max_w) w = nd->max_w;
  if (nd->min_h > 0.0f && h < nd->min_h) h = nd->min_h;
  if (nd->max_h > 0.0f && h > nd->max_h) h = nd->max_h;
  return (ccord_vec2){w, h};
}

static bool panels_can_hover(const ccord_ui *ui)
{
  return !ui->touch_mode && ui->mouse_in && !ui->menu.open && !ui->settings_open && !ui->go_live_open &&
         !ui->quick_switcher_open && !ui->shortcuts_open && !ui_server_settings_is_open(ui) &&
         !ui_channel_settings_is_open(ui) && !ui_plugin_dialog_is_open(ui) && !ui_plugin_settings_is_open(ui);
}

typedef struct draw_ctx {
  const char *owner;
  const char *panel_id;
  bool is_decor;
  ccord_decor_target decor_target;
  ccord_id decor_target_id;
} draw_ctx;

static void record_hit_ex(ccord_ui *ui, ui_panel_mgr *m, const draw_ctx *dctx, const char *node_id, const char *tooltip,
                          ccord_cursor_kind cursor, ccord_rect rc, phit_widget_kind wk, bool checked,
                          const char *val_str, bool draggable, bool track_move, float nmin, float nmax, float nstep,
                          size_t opt_idx, size_t opt_cnt)
{
  if (!node_id || !node_id[0] || rc.w <= 0.0f || rc.h <= 0.0f) return;
  if (dctx->is_decor) {
    ui_decor_record_hit(ui, dctx->owner, dctx->panel_id, dctx->decor_target, dctx->decor_target_id, node_id, rc);
    return;
  }
  if (!m || m->nhits >= PANEL_MAX_HITS) return;
  panel_hit *h = &m->hits[m->nhits++];
  memset(h, 0, sizeof *h);
  snprintf(h->owner, sizeof h->owner, "%s", dctx->owner);
  snprintf(h->panel_id, sizeof h->panel_id, "%s", dctx->panel_id);
  snprintf(h->node_id, sizeof h->node_id, "%s", node_id);
  snprintf(h->tooltip, sizeof h->tooltip, "%s", tooltip ? tooltip : "");
  if (val_str) snprintf(h->value_str, sizeof h->value_str, "%s", val_str);
  h->cursor = cursor;
  h->rect = rc;
  h->wkind = wk;
  h->checked = checked;
  h->draggable = draggable;
  h->track_move = track_move;
  h->num_min = nmin;
  h->num_max = nmax;
  h->num_step = nstep;
  h->opt_idx = opt_idx;
  h->opt_count = opt_cnt;
}

static void draw_box_chrome(ccord_renderer *r, const ccord_theme_colors *tc, const pnode *nd, ccord_rect rc,
                            bool hovered, ccord_color *out_surface_bg)
{
  ccord_color fill = {0}, bord = {0};
  bool has_fill = (hovered && pp_resolve_pcolor(tc, nd->hover_bg, &fill)) || pp_resolve_pcolor(tc, nd->bg, &fill);
  bool has_bord = nd->border_w > 0.0f &&
                  ((hovered && pp_resolve_pcolor(tc, nd->hover_border_color, &bord)) ||
                   pp_resolve_pcolor(tc, nd->border_color, &bord));
  if (has_fill) *out_surface_bg = fill;
  float bw = nd->border_w, rad = nd->radius;

  if (nd->border_sides == PBORD_NO_BOTTOM) {
    if (has_bord) ccord_draw_rrect(r, (ccord_rect){rc.x - bw, rc.y - bw, rc.w + bw * 2.0f, rc.h + bw}, rad + bw, bord);
    if (has_fill) {
      if (rad > 0.0f) {
        ccord_draw_rrect(r, rc, rad, fill);
        float sq = pp_p_minf(rad, rc.h);
        ccord_draw_rect(r, (ccord_rect){rc.x, rc.y + rc.h - sq, rc.w, sq}, fill);
      } else ccord_draw_rect(r, rc, fill);
    }
    return;
  }
  if (nd->border_sides == PBORD_ALL && has_bord && rad > 0.0f) {
    ccord_draw_rrect(r, (ccord_rect){rc.x - bw, rc.y - bw, rc.w + bw * 2.0f, rc.h + bw * 2.0f}, rad + bw, bord);
  }
  if (has_fill) {
    if (rad > 0.0f) ccord_draw_rrect(r, rc, rad, fill);
    else ccord_draw_rect(r, rc, fill);
  }
  if (has_bord) {
    switch (nd->border_sides) {
      case PBORD_ALL:
        if (rad <= 0.0f) {
          ccord_draw_rect(r, (ccord_rect){rc.x, rc.y, rc.w, bw}, bord);
          ccord_draw_rect(r, (ccord_rect){rc.x, rc.y + rc.h - bw, rc.w, bw}, bord);
          ccord_draw_rect(r, (ccord_rect){rc.x, rc.y, bw, rc.h}, bord);
          ccord_draw_rect(r, (ccord_rect){rc.x + rc.w - bw, rc.y, bw, rc.h}, bord);
        }
        break;
      case PBORD_BOTTOM: ccord_draw_rect(r, (ccord_rect){rc.x, rc.y + rc.h - bw, rc.w, bw}, bord); break;
      case PBORD_TOP:    ccord_draw_rect(r, (ccord_rect){rc.x, rc.y, rc.w, bw}, bord); break;
      case PBORD_LEFT:   ccord_draw_rect(r, (ccord_rect){rc.x, rc.y, bw, rc.h}, bord); break;
      case PBORD_RIGHT:  ccord_draw_rect(r, (ccord_rect){rc.x + rc.w - bw, rc.y, bw, rc.h}, bord); break;
      case PBORD_NO_BOTTOM: break;
    }
  }
}

static void draw_node(ccord_ui *ui, ccord_renderer *r, ui_panel_mgr *m, const draw_ctx *dctx,
                      const pnode *nd, ccord_rect rc, bool parent_is_col, bool ancestor_hovered, ccord_color surface_bg)
{
  if (!nd || rc.w <= 0.5f || rc.h <= 0.5f) return;
  const ccord_theme_colors *tc = &ui->theme->c;
  bool can_hov = panels_can_hover(ui);
  bool self_hovered = can_hov && ui_in_rect(ui->mx, ui->my, rc);
  bool eff_hovered = (nd->id != nullptr) ? self_hovered : ancestor_hovered;

  if (nd->opacity < 0.999f) ccord_push_alpha(r, nd->opacity);

  // Generic hit recording (specialized widgets record their own rich hits below)
  if (nd->id && !nd->disabled &&
      nd->kind != PN_INPUT && nd->kind != PN_TOGGLE && nd->kind != PN_CHECKBOX &&
      nd->kind != PN_RADIO && nd->kind != PN_SLIDER && nd->kind != PN_SELECT &&
      nd->kind != PN_TABS && nd->kind != PN_SCROLL) {
    record_hit_ex(ui, m, dctx, nd->id, nd->tooltip, nd->cursor, rc, PHIT_GENERIC, false, nullptr,
                  nd->draggable, nd->track_move, 0.0f, 0.0f, 0.0f, 0, 0);
  }

  switch (nd->kind) {
    case PN_SPACER: break;

    case PN_DIVIDER: {
      ccord_color col = tc->divider;
      (void)pp_resolve_pcolor(tc, nd->fg, &col);
      bool vert = nd->orient_explicit ? nd->is_vertical : !parent_is_col;
      if (vert) {
        float dh = pp_p_maxf(rc.h - nd->inset * 2.0f, 0.0f);
        if (dh > 0.0f) ccord_draw_rect(r, (ccord_rect){rc.x + (rc.w - nd->thickness) * 0.5f, rc.y + nd->inset, nd->thickness, dh}, col);
      } else {
        float dw = pp_p_maxf(rc.w - nd->inset * 2.0f, 0.0f);
        if (dw > 0.0f) ccord_draw_rect(r, (ccord_rect){rc.x + nd->inset, rc.y + (rc.h - nd->thickness) * 0.5f, dw, nd->thickness}, col);
      }
      break;
    }

    case PN_ICON: {
      ccord_color col = tc->text_muted;
      if (!(eff_hovered && pp_resolve_pcolor(tc, nd->hover_fg, &col))) (void)pp_resolve_pcolor(tc, nd->fg, &col);
      pp_draw_picon(ui, r, nd->icon, nd->icon_name, rc.x + rc.w * 0.5f, rc.y + rc.h * 0.5f, nd->icon_size, col, surface_bg);
      break;
    }

    case PN_TEXT:
    case PN_MARKDOWN: {
      ccord_color col = tc->text_normal;
      if (!(eff_hovered && pp_resolve_pcolor(tc, nd->hover_fg, &col))) (void)pp_resolve_pcolor(tc, nd->fg, &col);
      float fs = pp_resolve_font_size(ui, nd->font_size);
      ccord_text_style st = {.font = nd->bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, .size = fs, .color = col};
      size_t len = strlen(nd->text);
      if (nd->kind == PN_MARKDOWN) {
        ccord_text_draw(r, nd->text, len, rc.x, rc.y, &st, rc.w);
      } else {
        ccord_vec2 tm = ccord_text_measure(r, nd->text, len, &st, 0.0f);
        float ty = rc.y + (rc.h - tm.y) * 0.5f;
        if (nd->ellipsis && tm.x > rc.w) {
          (void)ccord_text_draw_ellipsis(r, nd->text, len, rc.x, ty, rc.w, &st);
        } else {
          float tx = rc.x;
          if (nd->align == PALIGN_CENTER) tx = rc.x + (rc.w - tm.x) * 0.5f;
          else if (nd->align == PALIGN_END) tx = rc.x + rc.w - tm.x;
          ccord_text_draw(r, nd->text, len, tx, ty, &st, 0.0f);
        }
      }
      break;
    }

    case PN_BUTTON: {
      bool hov = self_hovered && !nd->disabled;
      ccord_color bg = {0}, fg = tc->text_normal;
      bool draw_bg = false;
      switch (nd->btn_variant) {
        case PBTN_PRIMARY:   bg = hov ? tc->brand_hover : tc->brand; fg = tc->text_on_brand; draw_bg = true; break;
        case PBTN_DANGER:    bg = tc->danger; fg = tc->text_on_brand; draw_bg = true; break;
        case PBTN_SECONDARY: bg = (hov || nd->selected) ? tc->bg_item_hover : tc->bg_secondary; fg = (hov || nd->selected) ? tc->text_header : tc->text_normal; draw_bg = true; break;
        case PBTN_GHOST:
          if (hov || nd->selected) { bg = nd->selected ? tc->bg_item_selected : tc->bg_item_hover; draw_bg = true; }
          fg = (hov || nd->selected) ? tc->text_header : tc->text_muted;
          break;
      }
      if (nd->disabled) ccord_push_alpha(r, 0.45f);
      if (draw_bg) ccord_draw_rrect(r, rc, 4.0f, bg);
      bool has_lbl = nd->text && nd->text[0];
      bool has_ic = nd->icon != PICON_NONE;
      float cy = rc.y + rc.h * 0.5f;
      if (has_ic && !has_lbl) {
        pp_draw_picon(ui, r, nd->icon, nd->icon_name, rc.x + rc.w * 0.5f, cy, nd->icon_size, fg, draw_bg ? bg : surface_bg);
      } else if (has_lbl) {
        float fs = ui->theme->m.font_size_small - (nd->btn_sz == PBTN_XS ? 2.0f : 1.0f);
        ccord_text_style st = {.font = CCORD_FONT_BOLD, .size = fs, .color = fg};
        size_t len = strlen(nd->text);
        ccord_vec2 tm = ccord_text_measure(r, nd->text, len, &st, 0.0f);
        float content_w = tm.x + (has_ic ? nd->icon_size + 5.0f : 0.0f);
        float start_x = rc.x + pp_p_maxf((rc.w - content_w) * 0.5f, 4.0f);
        if (has_ic) {
          pp_draw_picon(ui, r, nd->icon, nd->icon_name, start_x + nd->icon_size * 0.5f, cy, nd->icon_size, fg, draw_bg ? bg : surface_bg);
          start_x += nd->icon_size + 5.0f;
        }
        float avail_w = pp_p_maxf(rc.x + rc.w - start_x - 4.0f, 0.0f);
        (void)ccord_text_draw_ellipsis(r, nd->text, len, start_x, cy - tm.y * 0.5f, avail_w, &st);
      }
      if (nd->disabled) ccord_pop_alpha(r);
      break;
    }

    case PN_INPUT: {
      uint64_t k = pp_widget_key(dctx->owner, dctx->panel_id, nd->id ? nd->id : "input");
      panel_input_state *ist = m ? pp_get_input_state(m, k, nd->value_str) : nullptr;
      if (ist) {
        uis_state st = {.hover = self_hovered && !nd->disabled, .focused = (m->focused_input_key == k), .disabled = nd->disabled};
        (void)uis_input(ui, r, rc, &ist->ed, nd->placeholder ? nd->placeholder : "", st, nd->masked, "p_inp", k);
      }
      if (nd->id && !nd->disabled) {
        record_hit_ex(ui, m, dctx, nd->id, nd->tooltip, CCORD_CURSOR_TEXT, rc, PHIT_INPUT, false, nullptr,
                      false, false, 0, 0, 0, 0, 0);
      }
      break;
    }

    case PN_TOGGLE: {
      uint64_t k = pp_widget_key(dctx->owner, dctx->panel_id, nd->id ? nd->id : "toggle");
      if (nd->text && nd->text[0]) {
        ccord_text_style st = {.font = CCORD_FONT_REGULAR, .size = ui->theme->m.font_size_small, .color = tc->text_normal};
        ccord_vec2 tm = ccord_text_measure(r, nd->text, strlen(nd->text), &st, 0.0f);
        (void)ccord_text_draw_ellipsis(r, nd->text, strlen(nd->text), rc.x, rc.y + (rc.h - tm.y) * 0.5f,
                                       pp_p_maxf(rc.w - UIS_TOGGLE_W - 8.0f, 0.0f), &st);
      }
      uis_state st = {.hover = self_hovered && !nd->disabled, .disabled = nd->disabled};
      uis_toggle(ui, r, rc, nd->checked, st, "p_tog", k);
      if (nd->id && !nd->disabled) {
        record_hit_ex(ui, m, dctx, nd->id, nd->tooltip, CCORD_CURSOR_POINTER, rc, PHIT_TOGGLE, nd->checked,
                      nullptr, false, false, 0, 0, 0, 0, 0);
      }
      break;
    }

    case PN_CHECKBOX: {
      ccord_rect box = {rc.x, rc.y + (rc.h - 18.0f) * 0.5f, 18.0f, 18.0f};
      ccord_color fill = nd->checked ? tc->brand : tc->bg_input;
      ccord_draw_rrect(r, box, 4.0f, fill);
      if (!nd->checked) ccord_draw_rrect_stroke(r, box, 4.0f, 1.5f, self_hovered ? tc->brand : tc->border_subtle);
      else {
        ccord_draw_line(r, (ccord_vec2){box.x + 4.0f, box.y + 9.5f}, (ccord_vec2){box.x + 7.5f, box.y + 13.0f}, 2.0f, tc->text_on_brand);
        ccord_draw_line(r, (ccord_vec2){box.x + 7.5f, box.y + 13.0f}, (ccord_vec2){box.x + 14.0f, box.y + 5.5f}, 2.0f, tc->text_on_brand);
      }
      if (nd->text && nd->text[0]) {
        ccord_text_style st = {.font = CCORD_FONT_REGULAR, .size = ui->theme->m.font_size_small, .color = tc->text_normal};
        ccord_vec2 tm = ccord_text_measure(r, nd->text, strlen(nd->text), &st, 0.0f);
        (void)ccord_text_draw_ellipsis(r, nd->text, strlen(nd->text), box.x + 24.0f, rc.y + (rc.h - tm.y) * 0.5f,
                                       pp_p_maxf(rc.w - 24.0f, 0.0f), &st);
      }
      if (nd->id && !nd->disabled) {
        record_hit_ex(ui, m, dctx, nd->id, nd->tooltip, CCORD_CURSOR_POINTER, rc, PHIT_CHECKBOX, nd->checked,
                      nullptr, false, false, 0, 0, 0, 0, 0);
      }
      break;
    }

    case PN_RADIO: {
      uint64_t k = pp_widget_key(dctx->owner, dctx->panel_id, nd->id ? nd->id : "radio");
      uis_state st = {.hover = self_hovered && !nd->disabled, .disabled = nd->disabled};
      uis_radio(ui, r, rc, nd->text ? nd->text : "", nd->checked, st, "p_rad", k);
      if (nd->id && !nd->disabled) {
        record_hit_ex(ui, m, dctx, nd->id, nd->tooltip, CCORD_CURSOR_POINTER, rc, PHIT_RADIO, nd->checked,
                      nd->value_str, false, false, 0, 0, 0, 0, 0);
      }
      break;
    }

    case PN_SLIDER: {
      ccord_rect track = {rc.x + 6.0f, rc.y + (rc.h - UIS_SLIDER_H) * 0.5f, pp_p_maxf(rc.w - 12.0f, 8.0f), UIS_SLIDER_H};
      float frac = (nd->num_val - nd->num_min) / (nd->num_max - nd->num_min);
      uis_state st = {.hover = self_hovered && !nd->disabled, .disabled = nd->disabled};
      uis_slider(ui, r, track, pp_p_clampf(frac, 0.0f, 1.0f), st);
      if (nd->id && !nd->disabled) {
        record_hit_ex(ui, m, dctx, nd->id, nd->tooltip, CCORD_CURSOR_POINTER, rc, PHIT_SLIDER, false, nullptr,
                      true, false, nd->num_min, nd->num_max, nd->num_step, 0, 0);
      }
      break;
    }

    case PN_SELECT: {
      uint64_t k = pp_widget_key(dctx->owner, dctx->panel_id, nd->id ? nd->id : "select");
      size_t cur_idx = 0;
      const char *lbl = nd->value_str ? nd->value_str : "";
      for (size_t i = 0; i < nd->nopts; i++) {
        if (nd->value_str && strcmp(nd->opts[i].value, nd->value_str) == 0) {
          cur_idx = i;
          lbl = nd->opts[i].label;
          break;
        }
      }
      if (!lbl[0] && nd->nopts > 0) lbl = nd->opts[0].label;
      uis_state st = {.hover = self_hovered && !nd->disabled, .disabled = nd->disabled};
      uis_select(ui, r, rc, lbl, st, "p_sel", k);
      if (nd->id && !nd->disabled) {
        size_t next_idx = nd->nopts > 0 ? (cur_idx + 1u) % nd->nopts : 0;
        const char *next_val = nd->nopts > 0 ? nd->opts[next_idx].value : "";
        record_hit_ex(ui, m, dctx, nd->id, nd->tooltip, CCORD_CURSOR_POINTER, rc, PHIT_SELECT, false,
                      next_val, false, false, 0, 0, 0, cur_idx, nd->nopts);
      }
      break;
    }

    case PN_TABS: {
      float x = rc.x;
      for (size_t i = 0; i < nd->nopts; i++) {
        float tw = uis_label_width(ui, r, nd->opts[i].label);
        ccord_rect tr = {x, rc.y + (rc.h - UIS_H_BTN_SM) * 0.5f, tw, UIS_H_BTN_SM};
        bool sel = nd->value_str && strcmp(nd->opts[i].value, nd->value_str) == 0;
        bool hov = can_hov && ui_in_rect(ui->mx, ui->my, tr) && !nd->disabled;
        uint64_t tk = pp_widget_key(dctx->owner, dctx->panel_id, nd->opts[i].value);
        uis_tab(ui, r, tr, nd->opts[i].label, (uis_state){.hover = hov, .selected = sel, .disabled = nd->disabled}, "p_tab", tk);
        if (nd->id && !nd->disabled) {
          record_hit_ex(ui, m, dctx, nd->id, nd->tooltip, CCORD_CURSOR_POINTER, tr, PHIT_TAB, sel,
                        nd->opts[i].value, false, false, 0, 0, 0, i, nd->nopts);
        }
        x += tw + nd->gap;
      }
      break;
    }

    case PN_CHIP: {
      ccord_color custom_bg = {0};
      if (pp_resolve_pcolor(tc, nd->bg, &custom_bg)) {
        ccord_draw_rrect(r, rc, rc.h * 0.5f, custom_bg);
        ccord_color fg = tc->text_on_brand;
        (void)pp_resolve_pcolor(tc, nd->fg, &fg);
        ccord_text_style st = {.font = CCORD_FONT_BOLD, .size = ui->theme->m.font_size_small - 2.0f, .color = fg};
        ccord_vec2 tm = ccord_text_measure(r, nd->text, strlen(nd->text), &st, 0.0f);
        ccord_text_draw(r, nd->text, strlen(nd->text), rc.x + (rc.w - tm.x) * 0.5f, rc.y + (rc.h - tm.y) * 0.5f, &st, 0.0f);
      } else {
        uint64_t k = pp_widget_key(dctx->owner, dctx->panel_id, nd->id ? nd->id : nd->text);
        uis_chip(ui, r, rc, nd->text, (uis_state){.hover = self_hovered, .selected = nd->selected, .disabled = nd->disabled}, "p_chp", k);
      }
      break;
    }

    case PN_PROGRESS: {
      ccord_color track_c = tc->bg_tertiary, fill_c = tc->brand;
      (void)pp_resolve_pcolor(tc, nd->bg, &track_c);
      (void)pp_resolve_pcolor(tc, nd->fg, &fill_c);
      float rad = pp_p_minf(nd->radius, rc.h * 0.5f);
      ccord_draw_rrect(r, rc, rad, track_c);
      float fw = rc.w * pp_p_clampf(nd->num_val, 0.0f, 1.0f);
      if (fw > 1.0f) ccord_draw_rrect(r, (ccord_rect){rc.x, rc.y, fw, rc.h}, rad, fill_c);
      break;
    }

    case PN_IMAGE: {
      if (nd->texture_key && nd->texture_key[0] && ccord_render_has_texture(r, nd->texture_key, nullptr, nullptr)) {
        if (nd->cover) ccord_draw_texture_cover(r, nd->texture_key, rc, nd->radius, 255, true);
        else if (nd->radius > 0.0f) ccord_draw_texture_rounded(r, nd->texture_key, rc, nd->radius, 255);
        else ccord_draw_texture(r, nd->texture_key, rc, 255);
      }
      break;
    }

    case PN_AVATAR: {
      const ccord_user *u = (ui->d.store && nd->avatar_uid) ? ccord_store_user(ui->d.store, nd->avatar_uid) : nullptr;
      float sz = pp_p_minf(rc.w, rc.h);
      ui_draw_avatar(ui, r, u, rc.x + rc.w * 0.5f, rc.y + rc.h * 0.5f, sz, surface_bg, false);
      break;
    }

    case PN_CANVAS: {
      if (nd->clip) ccord_push_clip(r, rc);
      int clips = 0, alphas = 0, offsets = 0;
      for (size_t i = 0; i < nd->ncmds; i++) {
        const pcmd *c = &nd->cmds[i];
        ccord_color col = tc->text_normal, col2 = tc->brand;
        (void)pp_resolve_pcolor(tc, c->color, &col);
        (void)pp_resolve_pcolor(tc, c->color2, &col2);
        float cx = rc.x + pp_eval_dim(c->x, rc.w);
        float cy = rc.y + pp_eval_dim(c->y, rc.h);
        switch (c->kind) {
          case PCMD_RECT:
            ccord_draw_rect(r, (ccord_rect){cx, cy, pp_eval_dim(c->w, rc.w), pp_eval_dim(c->h, rc.h)}, col);
            break;
          case PCMD_RRECT:
            ccord_draw_rrect(r, (ccord_rect){cx, cy, pp_eval_dim(c->w, rc.w), pp_eval_dim(c->h, rc.h)}, c->r_or_thick, col);
            break;
          case PCMD_RRECT_STROKE:
            ccord_draw_rrect_stroke(r, (ccord_rect){cx, cy, pp_eval_dim(c->w, rc.w), pp_eval_dim(c->h, rc.h)}, c->r_or_thick, c->stroke_w, col);
            break;
          case PCMD_CIRCLE:
            ccord_draw_circle(r, cx, cy, c->r_or_thick, col);
            break;
          case PCMD_CIRCLE_STROKE:
            ccord_draw_circle_stroke(r, cx, cy, c->r_or_thick, c->stroke_w, col);
            break;
          case PCMD_ARC:
            ccord_draw_arc(r, cx, cy, c->r_or_thick, c->start_deg, c->end_deg, c->stroke_w, col, c->filled);
            break;
          case PCMD_LINE:
            ccord_draw_line(r, (ccord_vec2){cx, cy}, (ccord_vec2){rc.x + pp_eval_dim(c->w, rc.w), rc.y + pp_eval_dim(c->h, rc.h)}, c->r_or_thick, col);
            break;
          case PCMD_TRIANGLE:
            ccord_draw_triangle(r, (ccord_vec2){cx, cy}, (ccord_vec2){rc.x + c->x2, rc.y + c->y2},
                                (ccord_vec2){rc.x + c->x3, rc.y + c->y3}, col);
            break;
          case PCMD_POLYGON:
            if (c->pts && c->npts >= 2u) {
              ccord_vec2 abs_pts[PANEL_MAX_POLY];
              size_t pn = c->npts < PANEL_MAX_POLY ? c->npts : PANEL_MAX_POLY;
              for (size_t k = 0; k < pn; k++) abs_pts[k] = (ccord_vec2){rc.x + c->pts[k].x, rc.y + c->pts[k].y};
              ccord_draw_polygon(r, abs_pts, pn, col, c->filled, c->stroke_w);
            }
            break;
          case PCMD_GRADIENT:
            ccord_draw_gradient_rect(r, (ccord_rect){cx, cy, pp_eval_dim(c->w, rc.w), pp_eval_dim(c->h, rc.h)}, col, col2, c->vertical);
            break;
          case PCMD_IMAGE:
            if (c->text && c->text[0] && ccord_render_has_texture(r, c->text, nullptr, nullptr)) {
              ccord_rect dst = {cx, cy, pp_eval_dim(c->w, rc.w), pp_eval_dim(c->h, rc.h)};
              uint8_t a = (uint8_t)(pp_p_clampf(c->stroke_w, 0.0f, 1.0f) * 255.0f);
              if (c->cover) ccord_draw_texture_cover(r, c->text, dst, c->r_or_thick, a, true);
              else if (c->r_or_thick > 0.0f) ccord_draw_texture_rounded(r, c->text, dst, c->r_or_thick, a);
              else ccord_draw_texture(r, c->text, dst, a);
            }
            break;
          case PCMD_TEXT: {
            float fs = pp_resolve_font_size(ui, c->font_size);
            ccord_text_style st = {.font = c->bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, .size = fs, .color = col};
            float mw = pp_eval_dim(c->w, rc.w);
            if (c->ellipsis && mw > 0.0f) (void)ccord_text_draw_ellipsis(r, c->text, strlen(c->text), cx, cy, mw, &st);
            else ccord_text_draw(r, c->text, strlen(c->text), cx, cy, &st, 0.0f);
            break;
          }
          case PCMD_ICON:
            pp_draw_picon(ui, r, c->icon, c->text, cx, cy, c->r_or_thick, col, surface_bg);
            break;
          case PCMD_CLIP_PUSH:
            if (clips < 8) { ccord_push_clip(r, (ccord_rect){cx, cy, pp_eval_dim(c->w, rc.w), pp_eval_dim(c->h, rc.h)}); clips++; }
            break;
          case PCMD_CLIP_POP:
            if (clips > 0) { ccord_pop_clip(r); clips--; }
            break;
          case PCMD_ALPHA_PUSH:
            if (alphas < 6) { ccord_push_alpha(r, c->r_or_thick); alphas++; }
            break;
          case PCMD_ALPHA_POP:
            if (alphas > 0) { ccord_pop_alpha(r); alphas--; }
            break;
          case PCMD_OFFSET_PUSH:
            if (offsets < 6) { ccord_push_offset(r, pp_eval_dim(c->x, rc.w), pp_eval_dim(c->y, rc.h)); offsets++; }
            break;
          case PCMD_OFFSET_POP:
            if (offsets > 0) { ccord_pop_offset(r); offsets--; }
            break;
        }
      }
      while (offsets-- > 0) ccord_pop_offset(r);
      while (alphas-- > 0) ccord_pop_alpha(r);
      while (clips-- > 0) ccord_pop_clip(r);
      if (nd->clip) ccord_pop_clip(r);
      for (size_t i = 0; i < nd->nhits; i++) {
        const phit_spec *hs = &nd->hits[i];
        ccord_rect hr = {rc.x + pp_eval_dim(hs->x, rc.w), rc.y + pp_eval_dim(hs->y, rc.h),
                         pp_eval_dim(hs->w, rc.w), pp_eval_dim(hs->h, rc.h)};
        record_hit_ex(ui, m, dctx, hs->id, hs->tooltip, hs->cursor, hr, PHIT_GENERIC, false, nullptr,
                      hs->draggable, hs->track_move, 0, 0, 0, 0, 0);
      }
      break;
    }

    case PN_BOX:
    case PN_SCROLL: {
      ccord_color box_surface = surface_bg;
      draw_box_chrome(r, tc, nd, rc, eff_hovered, &box_surface);
      if (nd->nchildren == 0) break;

      panel_scroll_state *sst = nullptr;
      if (nd->kind == PN_SCROLL && m) {
        const char *sid = nd->id ? nd->id : "scroll";
        uint64_t sk = pp_widget_key(dctx->owner, dctx->panel_id, sid);
        sst = pp_get_scroll_state(m, sk);
        record_hit_ex(ui, m, dctx, sid, nullptr, CCORD_CURSOR_DEFAULT, rc, PHIT_SCROLL, false, nullptr,
                      false, false, 0, 0, 0, 0, 0);
      }

      if (nd->clip) ccord_push_clip(r, rc);
      ccord_rect inner = {
        rc.x + nd->pad_l, rc.y + nd->pad_t,
        pp_p_maxf(rc.w - nd->pad_l - nd->pad_r, 0.0f),
        pp_p_maxf(rc.h - nd->pad_t - nd->pad_b, 0.0f),
      };

      // Draw flow children (wrapping or flex) + absolute children.
      const pnode *vis[128];
      ccord_vec2 intrinsic[128];
      float main_sz[128];
      bool frozen[128];
      size_t nvis = 0;
      for (size_t i = 0; i < nd->nchildren && nvis < 128u; i++) {
        const pnode *c = nd->children[i];
        if (!child_visible(c, eff_hovered)) continue;
        if (c->pos_abs) {
          ccord_vec2 am = measure_node(ui, r, c, nd->is_col, eff_hovered);
          ccord_rect arc = {inner.x + c->left_x, inner.y + c->top_y, am.x > 0.0f ? am.x : inner.w, am.y > 0.0f ? am.y : inner.h};
          draw_node(ui, r, m, dctx, c, arc, nd->is_col, eff_hovered, box_surface);
          continue;
        }
        vis[nvis] = c;
        intrinsic[nvis] = measure_node(ui, r, c, nd->is_col, eff_hovered);
        main_sz[nvis] = nd->is_col ? intrinsic[nvis].y : intrinsic[nvis].x;
        frozen[nvis] = (c->flex <= 0.0f);
        nvis++;
      }

      if (nvis > 0 && nd->kind == PN_SCROLL) {
        float y = inner.y - (sst ? sst->scroll_y : 0.0f);
        float total_h = 0.0f;
        for (size_t i = 0; i < nvis; i++) {
          float ch = intrinsic[i].y > 0.0f ? intrinsic[i].y : 24.0f;
          ccord_rect crc = {inner.x, y, inner.w, ch};
          if (crc.y + crc.h >= rc.y && crc.y <= rc.y + rc.h) {
            draw_node(ui, r, m, dctx, vis[i], crc, true, eff_hovered, box_surface);
          }
          y += ch + nd->gap;
          total_h += ch + (i ? nd->gap : 0.0f);
        }
        if (sst) {
          sst->max_scroll = pp_p_maxf(total_h - inner.h, 0.0f);
          sst->scroll_y = pp_p_clampf(sst->scroll_y, 0.0f, sst->max_scroll);
        }
      } else if (nvis > 0 && nd->wrap && !nd->is_col) {
        float x = inner.x, y = inner.y, row_h = 0.0f;
        for (size_t i = 0; i < nvis; i++) {
          float cw = intrinsic[i].x, ch = intrinsic[i].y;
          if (x > inner.x && x + cw > inner.x + inner.w + 0.5f) {
            x = inner.x; y += row_h + nd->gap; row_h = 0.0f;
          }
          draw_node(ui, r, m, dctx, vis[i], (ccord_rect){x, y, cw, ch}, false, eff_hovered, box_surface);
          x += cw + nd->gap;
          if (ch > row_h) row_h = ch;
        }
      } else if (nvis > 0) {
        float avail_main = nd->is_col ? inner.h : inner.w;
        float avail_cross = nd->is_col ? inner.w : inner.h;
        float total_gaps = (nvis > 1u) ? nd->gap * (float)(nvis - 1u) : 0.0f;
        for (int pass = 0; pass < 3; pass++) {
          float fixed_sum = total_gaps, flex_sum = 0.0f;
          for (size_t i = 0; i < nvis; i++) {
            if (frozen[i]) fixed_sum += main_sz[i];
            else flex_sum += vis[i]->flex;
          }
          if (flex_sum <= 0.0f) break;
          float rem = pp_p_maxf(avail_main - fixed_sum, 0.0f);
          bool clamped_any = false;
          for (size_t i = 0; i < nvis; i++) {
            if (frozen[i]) continue;
            float target = rem * (vis[i]->flex / flex_sum);
            float min_m = nd->is_col ? vis[i]->min_h : vis[i]->min_w;
            float max_m = nd->is_col ? vis[i]->max_h : vis[i]->max_w;
            if (min_m > 0.0f && target < min_m) { main_sz[i] = min_m; frozen[i] = true; clamped_any = true; }
            else if (max_m > 0.0f && target > max_m) { main_sz[i] = max_m; frozen[i] = true; clamped_any = true; }
            else main_sz[i] = target;
          }
          if (!clamped_any) break;
        }
        float used_main = total_gaps;
        for (size_t i = 0; i < nvis; i++) used_main += main_sz[i];
        float extra = pp_p_maxf(avail_main - used_main, 0.0f);
        float cursor_main = 0.0f, step_gap = nd->gap;
        if (nd->justify == PJUST_CENTER) cursor_main = extra * 0.5f;
        else if (nd->justify == PJUST_END) cursor_main = extra;
        else if (nd->justify == PJUST_BETWEEN && nvis > 1u) step_gap += extra / (float)(nvis - 1u);

        for (size_t i = 0; i < nvis; i++) {
          const pnode *c = vis[i];
          float m_len = main_sz[i];
          float c_intr = nd->is_col ? intrinsic[i].x : intrinsic[i].y;
          float c_len = (nd->align == PALIGN_STRETCH || (c->kind == PN_DIVIDER && c_intr <= 0.0f)) ? avail_cross
                      : (c_intr > 0.0f ? pp_p_minf(c_intr, avail_cross) : avail_cross);
          float c_off = 0.0f;
          if (nd->align == PALIGN_CENTER) c_off = (avail_cross - c_len) * 0.5f;
          else if (nd->align == PALIGN_END) c_off = avail_cross - c_len;
          ccord_rect crc = nd->is_col
                             ? (ccord_rect){inner.x + c_off, inner.y + cursor_main, c_len, m_len}
                             : (ccord_rect){inner.x + cursor_main, inner.y + c_off, m_len, c_len};
          if (!nd->clip || cursor_main < avail_main + 1.0f) {
            draw_node(ui, r, m, dctx, c, crc, nd->is_col, eff_hovered, box_surface);
          }
          cursor_main += m_len + step_gap;
        }
      }
      if (nd->clip) ccord_pop_clip(r);
      break;
    }
  }

  if (nd->opacity < 0.999f) ccord_pop_alpha(r);
}

// ---------------------------------------------------------------- Inline decorator widget helper

static pnode *get_cached_decor_node(ccord_ui *ui, const char *json)
{
  if (!ui || !json || !json[0]) return nullptr;
  ui_panel_mgr *m = pp_ensure_mgr(ui);
  if (!m) return nullptr;
  uint64_t h = pp_hash_str(json);
  if (h == 0) h = 1;
  for (size_t i = 0; i < 32u; i++) {
    if (m->decor_cache[i].hash == h) return m->decor_cache[i].root;
  }
  decor_cache_entry *e = &m->decor_cache[m->decor_cursor % 32u];
  m->decor_cursor++;
  if (e->hash) ccord_arena_reset(&e->arena);
  else ccord_arena_init(&e->arena, 1024u);
  e->hash = h;
  e->root = nullptr;
  static const ccord_json_limits k_lim = {16, 2048, 65536u};
  ccord_error err = {0};
  const ccord_json *j = ccord_json_parse(&e->arena, json, strlen(json), &k_lim, &err);
  uint32_t cnt = 0;
  if (j) e->root = pp_compile_node_into(&e->arena, &cnt, j, 0);
  return e->root;
}

ccord_vec2 ui_panels_measure_widget_json(ccord_ui *ui, ccord_renderer *r, const char *json, float max_w)
{
  pnode *root = get_cached_decor_node(ui, json);
  if (!root || !r) return (ccord_vec2){0.0f, 0.0f};
  ccord_vec2 sz = measure_node(ui, r, root, false, false);
  if (max_w > 0.0f && sz.x > max_w) sz.x = max_w;
  return sz;
}

void ui_panels_draw_widget_json(ccord_ui *ui, ccord_renderer *r, const char *owner, const char *decor_id,
                                ccord_decor_target target, ccord_id target_id, const char *json, ccord_rect rc,
                                ccord_color surface_bg)
{
  pnode *root = get_cached_decor_node(ui, json);
  if (!root || !r) return;
  draw_ctx dctx = {.owner = owner ? owner : "", .panel_id = decor_id ? decor_id : "",
                   .is_decor = true, .decor_target = target, .decor_target_id = target_id};
  draw_node(ui, r, ui->panel_mgr, &dctx, root, rc, false, false, surface_bg);
}

// ---------------------------------------------------------------- Panel drawing (docked + full_view + overlays)

static void draw_one_panel(ccord_ui *ui, ccord_renderer *r, ui_panel_mgr *m, const ccord_panel_contribution *pc,
                           ccord_rect prc, bool is_overlay, bool draw_default_bg)
{
  if (m->nboxes < PANEL_MAX_BOXES) {
    panel_box *bx = &m->boxes[m->nboxes++];
    snprintf(bx->owner, sizeof bx->owner, "%s", pc->owner);
    snprintf(bx->panel_id, sizeof bx->panel_id, "%s", pc->panel_id);
    bx->slot = pc->slot;
    bx->rect = prc;
    bx->is_overlay = is_overlay;
  }
  if (draw_default_bg) ccord_draw_rect(r, prc, ui->theme->c.bg_tertiary);
  ccord_push_clip(r, prc);
  compiled_panel *cp = pp_sync_compiled(m, pc);
  if (cp && cp->root) {
    draw_ctx dctx = {.owner = pc->owner, .panel_id = pc->panel_id, .is_decor = false};
    draw_node(ui, r, m, &dctx, cp->root, prc, false, false, ui->theme->c.bg_tertiary);
  }
  ccord_pop_clip(r);
}

void ui_panels_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r) return;
  ui_panels_sync_textures(ui, r);
  size_t np = ui->d.contrib ? ccord_contrib_panel_count(ui->d.contrib) : 0;
  if (np == 0) {
    if (ui->panel_mgr) ui_panels_destroy(ui);
    return;
  }
  if (ui->discovery_open) {
    if (ui->panel_mgr) {
      ui->panel_mgr->nhits = 0;
      ui->panel_mgr->nboxes = 0;
      pp_prune_stale_compiled(ui->panel_mgr, ui->d.contrib);
    }
    return;
  }
  ui_panel_mgr *m = pp_ensure_mgr(ui);
  if (!m) return;
  if (!ui_panels_has_active_full_view(ui)) m->full_view_back_rect = (ccord_rect){0};
  m->nhits = 0;
  m->nboxes = 0;
  pp_prune_stale_compiled(m, ui->d.contrib);

  float col_x = ui->layout.chat_header.x;
  float col_w = ui->layout.chat_header.w;
  if (col_w <= 0.0f) return;

  float safe_t = ui->safe_insets.top;
  float safe_r = ui->safe_insets.right;
  float bot_in = pp_p_maxf(ui->safe_insets.bottom, ui->keyboard_inset);
  float top_y = safe_t;
  float sub_y = ui->layout.chat_header.y + ui->layout.chat_header.h;
  float bot_total = ui_panels_slot_height(ui, CCORD_PANEL_BOTTOM);
  float bot_y = ui->win_h - bot_in - bot_total;
  float comp_top_y = ui->layout.message_area.y + ui->layout.message_area.h;
  float left_x = ui->layout.channel_list.x + ui->layout.channel_list.w;
  float right_total = ui_panels_slot_width(ui, CCORD_PANEL_RIGHT_SIDEBAR);
  if (ui->mobile_layout) right_total = pp_p_minf(right_total, col_w * 0.65f);
  float right_x = ui->win_w - safe_r - right_total;
  float chan_top_y = ui->layout.channel_list.y;
  float chan_bot_y = ui->layout.channel_list.y + ui->layout.channel_list.h - ui_panels_slot_height(ui, CCORD_PANEL_CHANNEL_BOTTOM);
  float rail_top_y = ui->layout.server_list.y;
  float rail_bot_y = ui->layout.server_list.y + ui->layout.server_list.h - ui_panels_slot_height(ui, CCORD_PANEL_RAIL_BOTTOM);
  float hdr_right_x = ui->layout.chat_header.x + ui->layout.chat_header.w - (ui->mobile_layout ? 110.0f : 180.0f);

  for (size_t i = 0; i < np; i++) {
    const ccord_panel_contribution *pc = ccord_contrib_panel_at(ui->d.contrib, i);
    if (!pc || !pc->visible) continue;
    float ph = pp_p_clampf(pc->height, 12.0f, 360.0f);
    ccord_rect prc = {0};
    bool def_bg = true;
    switch (pc->slot) {
      case CCORD_PANEL_TOP:
        prc = (ccord_rect){col_x, top_y, col_w, ph}; top_y += ph; break;
      case CCORD_PANEL_SUBHEADER:
        prc = (ccord_rect){col_x, sub_y, col_w, ph}; sub_y += ph; break;
      case CCORD_PANEL_BOTTOM:
        prc = (ccord_rect){col_x, bot_y, col_w, ph}; bot_y += ph; break;
      case CCORD_PANEL_COMPOSER_TOP:
        prc = (ccord_rect){ui->layout.composer.x, comp_top_y, ui->layout.composer.w, ph}; comp_top_y += ph; break;
      case CCORD_PANEL_COMPOSER_LEFT: {
        float pw = pc->width > 0.0f ? pc->width : 36.0f;
        prc = (ccord_rect){ui->layout.composer.x + 8.0f, ui->layout.composer.y + 8.0f, pw, ui->layout.composer.h - 16.0f};
        def_bg = false; break;
      }
      case CCORD_PANEL_COMPOSER_RIGHT: {
        float pw = pc->width > 0.0f ? pc->width : 36.0f;
        prc = (ccord_rect){ui->layout.composer.x + ui->layout.composer.w - pw - 8.0f, ui->layout.composer.y + 8.0f, pw, ui->layout.composer.h - 16.0f};
        def_bg = false; break;
      }
      case CCORD_PANEL_LEFT_SIDEBAR: {
        if (ui->mobile_layout && !ui->layout.left_drawer_visible) continue;
        float pw = pp_p_clampf(pc->width > 0.0f ? pc->width : 200.0f, 48.0f, 480.0f);
        if (left_x + pw <= 0.0f) continue;
        prc = (ccord_rect){left_x, safe_t, pw, pp_p_maxf(ui->win_h - safe_t - bot_in, 0.0f)}; left_x += pw; break;
      }
      case CCORD_PANEL_RIGHT_SIDEBAR: {
        float pw = pp_p_clampf(pc->width > 0.0f ? pc->width : 240.0f, 48.0f, 480.0f);
        if (ui->mobile_layout) pw = pp_p_minf(pw, col_w * 0.65f);
        float ry = ui->layout.chat_header.y + ui->layout.chat_header.h;
        prc = (ccord_rect){right_x, ry, pw, pp_p_maxf(ui->win_h - bot_in - ry, 0.0f)};
        right_x += pw; break;
      }
      case CCORD_PANEL_CHANNEL_TOP:
        if (ui->mobile_layout && !ui->layout.left_drawer_visible) continue;
        prc = (ccord_rect){ui->layout.channel_list.x, chan_top_y, ui->layout.channel_list.w, ph}; chan_top_y += ph; break;
      case CCORD_PANEL_CHANNEL_BOTTOM:
        if (ui->mobile_layout && !ui->layout.left_drawer_visible) continue;
        prc = (ccord_rect){ui->layout.channel_list.x, chan_bot_y, ui->layout.channel_list.w, ph}; chan_bot_y += ph; break;
      case CCORD_PANEL_RAIL_TOP:
        if (ui->mobile_layout && !ui->layout.left_drawer_visible) continue;
        prc = (ccord_rect){ui->layout.server_list.x, rail_top_y, ui->layout.server_list.w, ph}; rail_top_y += ph; break;
      case CCORD_PANEL_RAIL_BOTTOM:
        if (ui->mobile_layout && !ui->layout.left_drawer_visible) continue;
        prc = (ccord_rect){ui->layout.server_list.x, rail_bot_y, ui->layout.server_list.w, ph}; rail_bot_y += ph; break;
      case CCORD_PANEL_HEADER_RIGHT: {
        float pw = pc->width > 0.0f ? pc->width : 96.0f;
        hdr_right_x -= pw + 4.0f;
        prc = (ccord_rect){hdr_right_x, ui->layout.chat_header.y + 6.0f, pw, ui->layout.chat_header.h - 12.0f};
        def_bg = false; break;
      }
      case CCORD_PANEL_VOICE_BAR: {
        if (ui->mobile_layout && !ui->layout.left_drawer_visible) continue;
        ccord_rect vb = ui_voice_bar_rect(ui);
        if (vb.h <= 0.0f) continue;
        prc = (ccord_rect){vb.x, vb.y - ph, vb.w, ph}; break;
      }
      case CCORD_PANEL_STAGE_OVERLAY: {
        ccord_rect sa = ui_stage_area(ui);
        if (sa.w <= 0.0f || sa.h <= 0.0f) continue;
        float pw = pc->width > 0.0f ? pp_p_minf(pc->width, sa.w - 16.0f) : sa.w - 16.0f;
        prc = (ccord_rect){sa.x + 8.0f + pc->x, sa.y + 8.0f + pc->y, pw, ph};
        def_bg = false; break;
      }
      default:
        continue;
    }
    if (prc.w > 1.0f && prc.h > 1.0f) draw_one_panel(ui, r, m, pc, prc, false, def_bg);
  }
}

void ui_panels_draw_full_view(ccord_ui *ui, ccord_renderer *r, ccord_rect area)
{
  if (!ui || !r || !ui_panels_has_active_full_view(ui)) return;
  ui_panel_mgr *m = ui->panel_mgr;
  const ccord_panel_contribution *pc = ccord_contrib_find_panel(ui->d.contrib, m->full_view_owner, m->full_view_id);
  if (!pc) return;
  m->full_view_back_rect = (ccord_rect){0};
  if (ui->mobile_layout && area.h > 44.0f) {
    float bar_h = 44.0f;
    ccord_rect bar = {area.x, area.y, area.w, bar_h};
    ccord_draw_rect(r, bar, ui->theme->c.bg_secondary);
    uis_divider(ui, r, bar.x, bar.y + bar_h - 1.0f, bar.w);
    ccord_rect back = {bar.x + 8.0f, bar.y + 6.0f, 76.0f, 32.0f};
    m->full_view_back_rect = back;
    uis_button(ui, r, back, "< Back", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = panels_can_hover(ui) && ui_in_rect(ui->mx, ui->my, back)}, "pfv_back", 1);
    const char *title = (pc->title && pc->title[0]) ? pc->title : pc->panel_id;
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, ui->theme->m.font_size_base, ui->theme->c.text_header);
    float tx = back.x + back.w + 8.0f;
    float tw = pp_p_maxf(bar.x + bar.w - tx - 12.0f, 24.0f);
    ccord_vec2 tm = ccord_text_measure(r, title, strlen(title), &ts, 0.0f);
    ccord_text_draw_ellipsis(r, title, strlen(title), tx, bar.y + (bar_h - tm.y) * 0.5f, tw, &ts);
    area.y += bar_h;
    area.h -= bar_h;
  }
  draw_one_panel(ui, r, m, pc, area, false, true);
}

void ui_panels_draw_overlays(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r || !ui->d.contrib || ui->discovery_open) return;
  // Security invariant: never draw plugin overlays over protected security screens (consent / login / MFA).
  if (ui_plugin_security_surface_active(ui)) return;
  ui_panel_mgr *m = pp_ensure_mgr(ui);
  if (!m) return;
  float safe_l = ui->safe_insets.left;
  float safe_r = ui->safe_insets.right;
  float safe_t = ui->safe_insets.top;
  float bot_in = pp_p_maxf(ui->safe_insets.bottom, ui->keyboard_inset);
  float avail_w = pp_p_maxf(ui->win_w - safe_l - safe_r - 16.0f, 120.0f);
  float avail_h = pp_p_maxf(ui->win_h - safe_t - bot_in - 16.0f, 60.0f);
  size_t np = ccord_contrib_panel_count(ui->d.contrib);
  for (size_t i = 0; i < np; i++) {
    const ccord_panel_contribution *pc = ccord_contrib_panel_at(ui->d.contrib, i);
    if (!pc || !pc->visible) continue;
    if (pc->slot == CCORD_PANEL_MODAL) {
      float mw = pc->width > 0.0f ? pc->width : UIS_MODAL_W_MD;
      float mh = pp_p_clampf(pc->height, 80.0f, pp_p_maxf(avail_h - 32.0f, 80.0f));
      ccord_rect box = uis_modal_box(ui, mw, mh);
      uis_modal_frame(ui, r, box);
      draw_one_panel(ui, r, m, pc, box, true, false);
    } else if (pc->slot == CCORD_PANEL_POPOVER) {
      float pw = pp_p_minf(pc->width > 0.0f ? pc->width : 280.0f, avail_w);
      float ph = pp_p_clampf(pc->height, 40.0f, avail_h);
      float min_x = safe_l + 8.0f, max_x = pp_p_maxf(min_x, ui->win_w - safe_r - pw - 8.0f);
      float min_y = safe_t + 8.0f, max_y = pp_p_maxf(min_y, ui->win_h - bot_in - ph - 8.0f);
      float px = pp_p_clampf(pc->x > 0.0f ? pc->x : (safe_l + (ui->win_w - safe_l - safe_r - pw) * 0.5f), min_x, max_x);
      float py = pp_p_clampf(pc->y > 0.0f ? pc->y : safe_t + 56.0f, min_y, max_y);
      ccord_rect box = {px, py, pw, ph};
      uis_popover_frame(ui, r, box);
      draw_one_panel(ui, r, m, pc, box, true, false);
    } else if (pc->slot == CCORD_PANEL_FLOATING) {
      float fw = pp_p_minf(pc->width > 0.0f ? pc->width : 300.0f, avail_w);
      float fh = pp_p_clampf(pc->height, 40.0f, avail_h);
      float min_x = safe_l + 8.0f, max_x = pp_p_maxf(min_x, ui->win_w - safe_r - fw - 8.0f);
      float min_y = safe_t + 8.0f, max_y = pp_p_maxf(min_y, ui->win_h - bot_in - fh - 8.0f);
      float fx = pp_p_clampf(pc->x > 0.0f ? pc->x : ui->win_w - safe_r - fw - 24.0f, min_x, max_x);
      float fy = pp_p_clampf(pc->y > 0.0f ? pc->y : safe_t + 64.0f, min_y, max_y);
      ccord_rect box = {fx, fy, fw, fh};
      uis_tokens tk = uis_tokens_of(ui);
      uis_frame(ui, r, box, tk.r_surface, ui->theme->c.bg_floating, UIS_ELEV_POPOVER);
      draw_one_panel(ui, r, m, pc, box, true, false);
    }
  }
  // Tooltip for topmost hovered interactive node across docked + overlay panels.
  if (panels_can_hover(ui)) {
    for (size_t k = m->nhits; k > 0; k--) {
      const panel_hit *h = &m->hits[k - 1u];
      if (ui_in_rect(ui->mx, ui->my, h->rect)) {
        if (h->tooltip[0] != '\0') {
          uis_tip_side side = (h->rect.y < 60.0f) ? UIS_TIP_BELOW : UIS_TIP_ABOVE;
          (void)uis_tooltip(ui, r, h->rect, h->tooltip, side, pp_hash_str(h->owner) ^ pp_hash_str(h->node_id));
        }
        break;
      }
    }
  }
}

// ---------------------------------------------------------------- Input & rich events

static const panel_hit *find_top_hit(const ui_panel_mgr *m, float x, float y)
{
  if (!m) return nullptr;
  for (size_t k = m->nhits; k > 0; k--) {
    const panel_hit *h = &m->hits[k - 1u];
    if (ui_in_rect(x, y, h->rect)) return h;
  }
  return nullptr;
}

static bool point_in_any_panel(const ui_panel_mgr *m, float x, float y)
{
  if (!m) return false;
  for (size_t i = 0; i < m->nboxes; i++) {
    if (ui_in_rect(x, y, m->boxes[i].rect)) return true;
  }
  return false;
}

static void dispatch_ev(ccord_ui *ui, const char *owner, const char *panel_id, const ccord_panel_event *pev)
{
  if (!ui || !ui->d.contrib || !owner || !panel_id) return;
  ccord_error err = {0};
  (void)ccord_contrib_dispatch_panel_event(ui->d.contrib, owner, panel_id, pev, &err);
  ui->dirty = true;
}

static double slider_val_at(const panel_hit *h, float mx)
{
  float frac = h->rect.w > 12.0f ? (mx - (h->rect.x + 6.0f)) / (h->rect.w - 12.0f) : 0.0f;
  frac = pp_p_clampf(frac, 0.0f, 1.0f);
  double v = (double)h->num_min + (double)frac * (double)(h->num_max - h->num_min);
  if (h->num_step > 0.0f) v = round((v - (double)h->num_min) / (double)h->num_step) * (double)h->num_step + (double)h->num_min;
  return (double)pp_p_clampf((float)v, h->num_min, h->num_max);
}

bool ui_panels_has_focused_input(const ccord_ui *ui)
{
  return ui && ui->panel_mgr && ui->panel_mgr->focused_input_key != 0;
}

bool ui_panels_handle_back(ccord_ui *ui)
{
  if (!ui || !ui->panel_mgr || !ui->d.contrib) return false;
  ui_panel_mgr *m = ui->panel_mgr;
  for (size_t i = m->nboxes; i > 0; i--) {
    ccord_panel_slot sl = m->boxes[i - 1u].slot;
    if (sl == CCORD_PANEL_MODAL || sl == CCORD_PANEL_POPOVER) {
      ccord_panel_event pev = {.kind = CCORD_PANEL_EV_CLOSE, .node_id = ""};
      dispatch_ev(ui, m->boxes[i - 1u].owner, m->boxes[i - 1u].panel_id, &pev);
      ui->dirty = true;
      return true;
    }
  }
  if (ui_panels_has_active_full_view(ui)) {
    ui_panels_set_active_full_view(ui, nullptr, nullptr);
    ui->dirty = true;
    return true;
  }
  return false;
}

bool ui_panels_handle_input(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || !ui->panel_mgr || !ui->d.contrib || ui->discovery_open) return false;
  ui_panel_mgr *m = ui->panel_mgr;
  if (m->nboxes == 0 && m->focused_input_key == 0 && m->full_view_back_rect.w <= 0.0f) return false;

  // Check if a modal plugin panel is open (blocks clicks outside and closes on Escape/scrim).
  const panel_box *top_modal = nullptr;
  for (size_t i = m->nboxes; i > 0; i--) {
    if (m->boxes[i - 1u].slot == CCORD_PANEL_MODAL) { top_modal = &m->boxes[i - 1u]; break; }
  }

  // Focused input text / key handling
  if (m->focused_input_key != 0 && (ev->type == CCORD_IN_TEXT || ev->type == CCORD_IN_KEY_DOWN)) {
    panel_input_state *ist = nullptr;
    for (size_t i = 0; i < m->ninputs; i++) {
      if (m->inputs[i].key == m->focused_input_key) { ist = &m->inputs[i]; break; }
    }
    if (ist) {
      if (ev->type == CCORD_IN_TEXT) {
        ccord_error err = {0};
        if (ccord_text_edit_insert(&ist->ed, ev->text, strlen(ev->text), &err) == CCORD_OK) {
          ccord_panel_event pev = {.kind = CCORD_PANEL_EV_CHANGE, .node_id = m->focused_node,
                                   .text_value = ccord_text_edit_cstr(&ist->ed)};
          dispatch_ev(ui, m->focused_owner, m->focused_panel, &pev);
        }
        return true;
      }
      if (ev->key == CCORD_KEY_ESCAPE) {
        ccord_panel_event pev = {.kind = CCORD_PANEL_EV_BLUR, .node_id = m->focused_node};
        dispatch_ev(ui, m->focused_owner, m->focused_panel, &pev);
        m->focused_input_key = 0;
        return true;
      }
      if (ev->key == CCORD_KEY_RETURN) {
        ccord_panel_event pev = {.kind = CCORD_PANEL_EV_SUBMIT, .node_id = m->focused_node,
                                 .text_value = ccord_text_edit_cstr(&ist->ed)};
        dispatch_ev(ui, m->focused_owner, m->focused_panel, &pev);
        return true;
      }
      bool ctrl = (ev->mods & CCORD_MOD_CTRL) != 0;
      bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
      size_t prev_len = ist->ed.len;
      if (ev->key == CCORD_KEY_BACKSPACE) ccord_text_edit_backspace(&ist->ed);
      else if (ev->key == CCORD_KEY_DELETE) ccord_text_edit_delete(&ist->ed);
      else if (ev->key == CCORD_KEY_LEFT) ccord_text_edit_move(&ist->ed, -1, shift);
      else if (ev->key == CCORD_KEY_RIGHT) ccord_text_edit_move(&ist->ed, 1, shift);
      else if (ev->key == CCORD_KEY_HOME) ccord_text_edit_move(&ist->ed, -2, shift);
      else if (ev->key == CCORD_KEY_END) ccord_text_edit_move(&ist->ed, 2, shift);
      else if (ctrl && ev->key == CCORD_KEY_A) ccord_text_edit_select_all(&ist->ed);
      if (ist->ed.len != prev_len) {
        ccord_panel_event pev = {.kind = CCORD_PANEL_EV_CHANGE, .node_id = m->focused_node,
                                 .text_value = ccord_text_edit_cstr(&ist->ed)};
        dispatch_ev(ui, m->focused_owner, m->focused_panel, &pev);
      }
      ui->dirty = true;
      return true;
    }
  }

  if (ev->type == CCORD_IN_KEY_DOWN && top_modal && ev->key == CCORD_KEY_ESCAPE) {
    ccord_panel_event pev = {.kind = CCORD_PANEL_EV_CLOSE, .node_id = ""};
    dispatch_ev(ui, top_modal->owner, top_modal->panel_id, &pev);
    return true;
  }

  // Hover enter/leave + drag move + track_move
  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    if (m->dragging) {
      if (m->drag_hit.wkind == PHIT_SLIDER) {
        double v = slider_val_at(&m->drag_hit, ev->x);
        ccord_panel_event pev = {.kind = CCORD_PANEL_EV_CHANGE, .node_id = m->drag_hit.node_id,
                                 .x = ev->x - m->drag_hit.rect.x, .y = ev->y - m->drag_hit.rect.y,
                                 .num_value = v, .has_num = true};
        dispatch_ev(ui, m->drag_hit.owner, m->drag_hit.panel_id, &pev);
      } else {
        ccord_panel_event pev = {.kind = CCORD_PANEL_EV_DRAG_MOVE, .node_id = m->drag_hit.node_id, .mods = ev->mods,
                                 .x = ev->x - m->drag_hit.rect.x, .y = ev->y - m->drag_hit.rect.y};
        dispatch_ev(ui, m->drag_hit.owner, m->drag_hit.panel_id, &pev);
      }
      return true;
    }
    const panel_hit *h = point_in_any_panel(m, ev->x, ev->y) ? find_top_hit(m, ev->x, ev->y) : nullptr;
    bool same = h && strcmp(h->owner, m->hover_owner) == 0 && strcmp(h->panel_id, m->hover_panel) == 0 &&
                strcmp(h->node_id, m->hover_node) == 0;
    if (!same) {
      if (m->hover_node[0]) {
        ccord_panel_event lev = {.kind = CCORD_PANEL_EV_HOVER_LEAVE, .node_id = m->hover_node};
        dispatch_ev(ui, m->hover_owner, m->hover_panel, &lev);
        m->hover_owner[0] = m->hover_panel[0] = m->hover_node[0] = '\0';
      }
      if (h) {
        snprintf(m->hover_owner, sizeof m->hover_owner, "%s", h->owner);
        snprintf(m->hover_panel, sizeof m->hover_panel, "%s", h->panel_id);
        snprintf(m->hover_node, sizeof m->hover_node, "%s", h->node_id);
        ccord_panel_event eev = {.kind = CCORD_PANEL_EV_HOVER_ENTER, .node_id = h->node_id,
                                 .x = ev->x - h->rect.x, .y = ev->y - h->rect.y};
        dispatch_ev(ui, h->owner, h->panel_id, &eev);
      }
    }
    if (h && h->track_move) {
      ccord_panel_event mev = {.kind = CCORD_PANEL_EV_MOUSE_MOVE, .node_id = h->node_id, .mods = ev->mods,
                               .x = ev->x - h->rect.x, .y = ev->y - h->rect.y};
      dispatch_ev(ui, h->owner, h->panel_id, &mev);
    }
    return false;
  }

  if (ev->type == CCORD_IN_MOUSE_UP && m->dragging) {
    if (m->drag_hit.wkind != PHIT_SLIDER) {
      ccord_panel_event pev = {.kind = CCORD_PANEL_EV_DRAG_END, .node_id = m->drag_hit.node_id, .mods = ev->mods,
                               .x = ev->x - m->drag_hit.rect.x, .y = ev->y - m->drag_hit.rect.y};
      dispatch_ev(ui, m->drag_hit.owner, m->drag_hit.panel_id, &pev);
    }
    m->dragging = false;
    return true;
  }

  if (ev->type == CCORD_IN_WHEEL) {
    if (!point_in_any_panel(m, ev->x, ev->y)) return top_modal != nullptr;
    // Scroll container handling first
    for (size_t k = m->nhits; k > 0; k--) {
      const panel_hit *h = &m->hits[k - 1u];
      if (h->wkind == PHIT_SCROLL && ui_in_rect(ev->x, ev->y, h->rect)) {
        uint64_t sk = pp_widget_key(h->owner, h->panel_id, h->node_id);
        panel_scroll_state *sst = pp_get_scroll_state(m, sk);
        if (sst && sst->max_scroll > 0.0f) {
          ui_scroll_offset_by(ui, &sst->scroll_y, ui_wheel_delta_px(ui, ev, 36.0f), sst->max_scroll);
          ui->dirty = true;
          return true;
        }
      }
    }
    const panel_hit *h = find_top_hit(m, ev->x, ev->y);
    if (h) {
      ccord_panel_event pev = {.kind = CCORD_PANEL_EV_WHEEL, .node_id = h->node_id, .mods = ev->mods,
                               .x = ev->x - h->rect.x, .y = ev->y - h->rect.y, .wheel = ev->wheel};
      dispatch_ev(ui, h->owner, h->panel_id, &pev);
    }
    return true;
  }

  if (ev->type == CCORD_IN_MOUSE_DOWN) {
    if (ev->button == 1 && m->full_view_back_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, m->full_view_back_rect)) {
      ui_panels_set_active_full_view(ui, nullptr, nullptr);
      return true;
    }
    if (!point_in_any_panel(m, ev->x, ev->y)) {
      if (m->focused_input_key != 0) {
        ccord_panel_event pev = {.kind = CCORD_PANEL_EV_BLUR, .node_id = m->focused_node};
        dispatch_ev(ui, m->focused_owner, m->focused_panel, &pev);
        m->focused_input_key = 0;
        if (ui->touch_mode) ui->touch_text_focused = false;
      }
      if (top_modal) {
        ccord_panel_event pev = {.kind = CCORD_PANEL_EV_CLOSE, .node_id = ""};
        dispatch_ev(ui, top_modal->owner, top_modal->panel_id, &pev);
        return true;
      }
      // Close any popover panels when clicking outside
      for (size_t i = 0; i < m->nboxes; i++) {
        if (m->boxes[i].slot == CCORD_PANEL_POPOVER) {
          ccord_panel_event pev = {.kind = CCORD_PANEL_EV_CLOSE, .node_id = ""};
          dispatch_ev(ui, m->boxes[i].owner, m->boxes[i].panel_id, &pev);
        }
      }
      return false;
    }
    const panel_hit *h = find_top_hit(m, ev->x, ev->y);
    if (m->focused_input_key != 0 && (!h || h->wkind != PHIT_INPUT)) {
      ccord_panel_event bev = {.kind = CCORD_PANEL_EV_BLUR, .node_id = m->focused_node};
      dispatch_ev(ui, m->focused_owner, m->focused_panel, &bev);
      m->focused_input_key = 0;
      if (ui->touch_mode) ui->touch_text_focused = false;
    }
    if (h && (ev->button >= 1 && ev->button <= 3)) {
      if (ev->button == 1 && h->wkind == PHIT_INPUT) {
        uint64_t k = pp_widget_key(h->owner, h->panel_id, h->node_id);
        m->focused_input_key = k;
        snprintf(m->focused_owner, sizeof m->focused_owner, "%s", h->owner);
        snprintf(m->focused_panel, sizeof m->focused_panel, "%s", h->panel_id);
        snprintf(m->focused_node, sizeof m->focused_node, "%s", h->node_id);
        if (ui->touch_mode) ui->touch_text_focused = true;
        ccord_panel_event fev = {.kind = CCORD_PANEL_EV_FOCUS, .node_id = h->node_id};
        dispatch_ev(ui, h->owner, h->panel_id, &fev);
        return true;
      }
      if (ev->button == 1 && (h->wkind == PHIT_TOGGLE || h->wkind == PHIT_CHECKBOX)) {
        ccord_panel_event cev = {.kind = CCORD_PANEL_EV_CHANGE, .node_id = h->node_id,
                                 .bool_value = !h->checked, .has_bool = true};
        dispatch_ev(ui, h->owner, h->panel_id, &cev);
        return true;
      }
      if (ev->button == 1 && (h->wkind == PHIT_RADIO || h->wkind == PHIT_SELECT || h->wkind == PHIT_TAB)) {
        ccord_panel_event cev = {.kind = CCORD_PANEL_EV_CHANGE, .node_id = h->node_id,
                                 .text_value = h->value_str[0] ? h->value_str : nullptr,
                                 .bool_value = true, .has_bool = (h->value_str[0] == '\0')};
        dispatch_ev(ui, h->owner, h->panel_id, &cev);
        return true;
      }
      if (ev->button == 1 && h->wkind == PHIT_SLIDER) {
        m->dragging = true;
        m->drag_hit = *h;
        double v = slider_val_at(h, ev->x);
        ccord_panel_event cev = {.kind = CCORD_PANEL_EV_CHANGE, .node_id = h->node_id,
                                 .x = ev->x - h->rect.x, .y = ev->y - h->rect.y,
                                 .num_value = v, .has_num = true};
        dispatch_ev(ui, h->owner, h->panel_id, &cev);
        return true;
      }
      if (ev->button == 1 && h->draggable) {
        m->dragging = true;
        m->drag_hit = *h;
        ccord_panel_event dev = {.kind = CCORD_PANEL_EV_DRAG_START, .node_id = h->node_id, .button = ev->button,
                                 .mods = ev->mods, .x = ev->x - h->rect.x, .y = ev->y - h->rect.y};
        dispatch_ev(ui, h->owner, h->panel_id, &dev);
      }
      ccord_panel_event_kind kind = CCORD_PANEL_EV_CLICK;
      if (ev->button == 1 && ev->clicks >= 2) kind = CCORD_PANEL_EV_DOUBLE_CLICK;
      else if (ev->button == 2) kind = CCORD_PANEL_EV_MIDDLE_CLICK;
      else if (ev->button == 3) kind = CCORD_PANEL_EV_RIGHT_CLICK;
      ccord_panel_event pev = {.kind = kind, .node_id = h->node_id, .button = ev->button, .mods = ev->mods,
                               .x = ev->x - h->rect.x, .y = ev->y - h->rect.y};
      dispatch_ev(ui, h->owner, h->panel_id, &pev);
    }
    return true;
  }

  return top_modal != nullptr;
}

bool ui_panels_cursor(const ccord_ui *ui, float x, float y, ccord_cursor_kind *out_cursor)
{
  if (!ui || !ui->panel_mgr || ui->discovery_open) return false;
  if (!point_in_any_panel(ui->panel_mgr, x, y)) return false;
  const panel_hit *h = find_top_hit(ui->panel_mgr, x, y);
  if (out_cursor) *out_cursor = h ? h->cursor : CCORD_CURSOR_DEFAULT;
  return true;
}

bool ui_panels_find_hit_rect(const ccord_ui *ui, const char *owner, const char *panel_id, const char *node_id,
                             ccord_rect *out)
{
  if (!ui || !ui->panel_mgr || !node_id) return false;
  const ui_panel_mgr *m = ui->panel_mgr;
  for (size_t k = m->nhits; k > 0; k--) {
    const panel_hit *h = &m->hits[k - 1u];
    if (owner && strcmp(h->owner, owner) != 0) continue;
    if (panel_id && strcmp(h->panel_id, panel_id) != 0) continue;
    if (strcmp(h->node_id, node_id) == 0) {
      if (out) *out = h->rect;
      return true;
    }
  }
  return false;
}
