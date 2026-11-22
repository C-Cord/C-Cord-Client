// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "server_settings_internal.h"

// ---------------------------------------------------------------- hits and widgets

bool ss_visible(const ss_cx *c, ccord_rect rc)
{
  return rc.y + rc.h > c->clip.y && rc.y < c->clip.y + c->clip.h && rc.x + rc.w > c->clip.x &&
         rc.x < c->clip.x + c->clip.w;
}

// Registers a clickable region (clipped to what is visible); returns true when the pointer is over it.
bool ss_hit_add(ss_cx *c, ccord_rect rc, ss_act act, uint64_t arg, const char *why, const char *name)
{
  struct ui_server_settings *ss = c->ss;
  if (ss->nhits >= SS_HIT_MAX || !ss_visible(c, rc)) return false;
  float x0 = fmaxf(rc.x, c->clip.x), y0 = fmaxf(rc.y, c->clip.y);
  float x1 = fminf(rc.x + rc.w, c->clip.x + c->clip.w), y1 = fminf(rc.y + rc.h, c->clip.y + c->clip.h);
  ss_hit *h = &ss->hits[ss->nhits];
  h->r = (ccord_rect){x0, y0, x1 - x0, y1 - y0};
  h->act = act;
  h->arg = arg;
  h->why = why;
  (void)snprintf(h->name, sizeof h->name, "%s", name);
  bool hov = c->ui->mouse_in && ui_in_rect(c->ui->mx, c->ui->my, h->r);
  if (hov) ss->hover = (int)ss->nhits;
  ss->nhits++;
  return hov;
}

float ss_text(ss_cx *c, const char *s, float x, float y, float size, bool bold, ccord_color col, float max_w)
{
  ccord_ui *ui = c->ui;
  ccord_text_style st = ui_style(ui, bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, size, col);
  if (max_w > 0.0f) (void)ccord_text_draw_ellipsis(c->r, s, strlen(s), x, y, max_w, &st);
  else ccord_text_draw(c->r, s, strlen(s), x, y, &st, 0.0f);
  return ui_line_h(ui, size);
}

float ss_text_width(ss_cx *c, const char *s, float size, bool bold)
{
  ccord_text_style st = ui_style(c->ui, bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, size, (ccord_color){0, 0, 0, 255});
  return ccord_text_measure(c->r, s, strlen(s), &st, 0.0f).x;
}

// Draws `s` centred in `rc`.
void ss_text_centered(ss_cx *c, const char *s, ccord_rect rc, float size, bool bold, ccord_color col)
{
  ccord_ui *ui = c->ui;
  ccord_text_style st = ui_style(ui, bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, size, col);
  ccord_vec2 t = ccord_text_measure(c->r, s, strlen(s), &st, 0.0f);
  float w = rc.w - 8.0f;
  if (t.x > w) {
    (void)ccord_text_draw_ellipsis(c->r, s, strlen(s), rc.x + 4.0f, rc.y + (rc.h - t.y) * 0.5f, w, &st);
    return;
  }
  ccord_text_draw(c->r, s, strlen(s), rc.x + (rc.w - t.x) * 0.5f, rc.y + (rc.h - t.y) * 0.5f, &st, 0.0f);
}


// Buttons take one of the three token heights: a rect up to 28 px tall is small, 36 px and up is large, else medium.
static uis_btn_size ss_btn_size(float h)
{
  return h <= UIS_H_BTN_SM ? UIS_BTN_S_SM : (h >= UIS_H_INPUT ? UIS_BTN_S_LG : UIS_BTN_S_MD);
}

void ss_button(ss_cx *c, ccord_rect rc, const char *label, ss_btn_style style, const char *why, ss_act act,
                      uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  uis_btn_size size = ss_btn_size(rc.h);
  rc.h = uis_button_height(size);
  if (!ss_visible(c, rc)) return;
  bool hov = ss_hit_add(c, rc, act, arg, why, name) && why == nullptr;
  uis_btn_kind kind = UIS_BTN_SECONDARY;
  switch (style) {
    case BTN_PRIMARY: kind = UIS_BTN_PRIMARY; break;
    case BTN_SECONDARY: kind = UIS_BTN_SECONDARY; break;
    case BTN_DANGER: kind = UIS_BTN_DANGER; break;
    case BTN_LINK: kind = UIS_BTN_LINK; break;
  }
  uis_button(ui, c->r, rc, label, kind, size, (uis_state){.hover = hov, .disabled = why != nullptr}, "ssbtn",
             ui_name_id(name));
}

void ss_toggle(ss_cx *c, ccord_rect rc, bool on, const char *why, ss_act act, uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  rc = uis_toggle_rect(rc);
  if (!ss_visible(c, rc)) return;
  bool hov = ss_hit_add(c, rc, act, arg, why, name) && why == nullptr;
  uis_toggle(ui, c->r, rc, on, (uis_state){.hover = hov, .disabled = why != nullptr}, "ssw", ui_name_id(name));
}

void ss_chip(ss_cx *c, ccord_rect rc, const char *label, bool sel, ss_act act, uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  rc.h = UIS_H_BTN_SM;
  if (!ss_visible(c, rc)) return;
  bool hov = ss_hit_add(c, rc, act, arg, nullptr, name);
  uis_chip(ui, c->r, rc, label, (uis_state){.hover = hov, .selected = sel}, "sschip", ui_name_id(name));
}

// Snaps a content-sized height up to the spacing grid (UIS_SP_1), so every drawn box sits on a token step.
float ss_grid(float h)
{
  return ceilf(h / UIS_SP_1) * UIS_SP_1;
}

// A filter tab (uis_tab): the selected filter is bold on brand and the others are regular, as the audit log had them.
void ss_filter_tab(ss_cx *c, ccord_rect rc, const char *label, bool sel, ss_act act, uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  rc.h = UIS_H_BTN_SM;
  if (!ss_visible(c, rc)) return;
  bool hov = ss_hit_add(c, rc, act, arg, nullptr, name);
  uis_tab(ui, c->r, rc, label, (uis_state){.hover = hov, .selected = sel}, "ssfilter", ui_name_id(name));
}

// A dropdown trigger (uis_select): the rect is the hit target and is UIS_H_INPUT tall.
void ss_select(ss_cx *c, ccord_rect rc, const char *label, ss_act act, uint64_t arg, const char *why,
                      const char *name)
{
  ccord_ui *ui = c->ui;
  rc.h = UIS_H_INPUT;
  if (!ss_visible(c, rc)) return;
  bool hov = ss_hit_add(c, rc, act, arg, why, name) && why == nullptr;
  uis_select(ui, c->r, rc, label, (uis_state){.hover = hov, .disabled = why != nullptr}, "ssselect", ui_name_id(name));
}

// A text input (uis_input): the caret and selection follow the edit; a long text scrolls so the caret stays visible.
void ss_field_box(ss_cx *c, ccord_rect box, ccord_text_edit *ed, ss_field f, const char *placeholder, const char *why,
                         const char *name)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  box.h = UIS_H_INPUT;
  if (!ss_visible(c, box)) return;
  bool focused = ss->focus == f;
  (void)ss_hit_add(c, box, A_FIELD, (uint64_t)f, why, name);
  (void)uis_input(ui, c->r, box, ed, placeholder, (uis_state){.focused = focused, .disabled = why != nullptr}, false,
                  "ssfocus", (uint64_t)f);
}

void ss_label(ss_cx *c, const char *s, float x, float y)
{
  const ccord_ui *ui = c->ui;
  (void)ss_text(c, s, x, y, uis_tokens_of(ui).fs_overline, true, COL(text_muted), 0.0f);
}

// ---------------------------------------------------------------- geometry


static ss_geom ss_layout(const ccord_ui *ui)
{
  ss_geom g;
  if (ui->mobile_layout) {
    float sl = ui->safe_insets.left;
    float sr = ui->safe_insets.right;
    g.top_y = ui->safe_insets.top + UIS_SP_3 + UIS_ICON_BTN + UIS_SP_3;
    if (!ui->mobile_server_settings_detail) {
      g.sb_w = ui->win_w;
      g.x0 = ui->win_w;
      g.col_x = ui->win_w;
      g.col_w = 120.0f;
      return g;
    }
    g.sb_w = 0.0f;
    g.x0 = sl;
    g.col_x = sl + UIS_SP_4;
    g.col_w = fmaxf(ui->win_w - sl - sr - 2.0f * UIS_SP_4, 120.0f);
    return g;
  }
  g.sb_w = fminf(fmaxf(ui->win_w * 0.24f, 168.0f), 260.0f);
  g.x0 = g.sb_w;
  float avail = ui->win_w - g.x0;
  float pad = avail > 560.0f ? 40.0f : 20.0f;
  g.col_x = g.x0 + pad;
  g.col_w = fminf(avail - pad * 2.0f, 860.0f);
  if (g.col_w < 120.0f) g.col_w = 120.0f;
  g.top_y = 56.0f;
  return g;
}

void ss_end_content(struct ui_server_settings *ss, float y_end)
{
  ss->content_h = y_end + ss->scroll + 110.0f;   // room under the last row for the unsaved-changes bar
}

void ss_title(ss_cx *c, const ss_geom *g, const char *title, float y)
{
  const ccord_ui *ui = c->ui;
  (void)ss_text(c, title, g->col_x, y, SS_TITLE(c), true, COL(text_header), 0.0f);
}

void ss_message(ss_cx *c, const char *s, float x, float y, float w)
{
  ccord_ui *ui = c->ui;
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
  ccord_text_draw(c->r, s, strlen(s), x, y, &st, w);
}

bool ss_contains_ci(const char *hay, const char *needle)
{
  if (!needle || needle[0] == '\0') return true;
  size_t n = strlen(needle);
  for (; *hay; hay++) {
    size_t i = 0;
    while (i < n && hay[i] && (hay[i] | 0x20) == (needle[i] | 0x20)) i++;
    if (i == n) return true;
  }
  return false;
}

// ---------------------------------------------------------------- chrome: sidebar, close, unsaved bar, toast

static void draw_sidebar(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  ccord_draw_rect(c->r, (ccord_rect){0.0f, 0.0f, g->sb_w, ui->win_h}, COL(bg_secondary));
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  float sl = ui->mobile_layout ? ui->safe_insets.left : 0.0f;
  float sr = ui->mobile_layout ? ui->safe_insets.right : 0.0f;
  float st = ui->mobile_layout ? ui->safe_insets.top : 0.0f;
  float sb_bot = ui->mobile_layout ? ui->safe_insets.bottom : 0.0f;
  float x = ui->mobile_layout ? (sl + UIS_SP_4) : 14.0f;
  float w = ui->mobile_layout ? fmaxf(g->sb_w - sl - sr - 2.0f * UIS_SP_4, 140.0f) : (g->sb_w - 28.0f);
  float hdr_y = ui->mobile_layout ? (st + UIS_SP_3 + 6.0f) : 36.0f;
  (void)ss_text(c, gd && gd->name ? gd->name : "Server", x + 6.0f, hdr_y,
                ui->mobile_layout ? SS_TITLE(c) : M(font_size_small), true, COL(text_header),
                fmaxf(w - (ui->mobile_layout ? (UIS_ICON_BTN + 16.0f) : 12.0f), 60.0f));
  float list_top = ui->mobile_layout ? (st + UIS_SP_3 + UIS_ICON_BTN + UIS_SP_2) : (hdr_y + 24.0f);
  float view_h = fmaxf(ui->win_h - sb_bot - list_top - (ui->mobile_layout ? UIS_SP_2 : 0.0f), 60.0f);
  ss->sb_view_h = view_h;
  float max_sb = fmaxf(0.0f, ss->sb_content_h - view_h);
  if (ss->sb_scroll < 0.0f) ss->sb_scroll = 0.0f;
  if (ss->sb_scroll > max_sb) ss->sb_scroll = max_sb;
  ss_cx sc = *c;
  if (ui->mobile_layout) {
    sc.clip = (ccord_rect){sl, list_top, fmaxf(g->sb_w - sl - sr, 120.0f), view_h};
    ccord_push_clip(c->r, sc.clip);
  }
  float y = list_top - (ui->mobile_layout ? ss->sb_scroll : 0.0f);
  float row_h = ui->mobile_layout ? 40.0f : 28.0f;
  float row_pitch = row_h + 2.0f;
  static const struct {
    const char *heading;
    ss_page pages[4];
    size_t count;
  } groups[] = {
    {nullptr, {SS_OVERVIEW, SS_ROLES, SS_INTEGRATIONS, SS_COMMUNITY}, 4},
    {"EXPRESSIONS", {SS_EMOJI, SS_STICKERS, SS_SOUNDBOARD}, 3},
    {"MODERATION", {SS_SAFETY, SS_AUTOMOD, SS_AUDIT}, 3},
    {"USER MANAGEMENT", {SS_INVITES, SS_BANS, SS_MEMBERS}, 3},
    {"DANGER ZONE", {SS_DANGER}, 1}
  };
  for (size_t gi = 0; gi < sizeof groups / sizeof groups[0]; gi++) {
    bool any = false;
    for (size_t k = 0; k < groups[gi].count; k++) any = any || ss_page_allowed(ui, ss->guild_id, groups[gi].pages[k]);
    if (!any) continue;
    if (groups[gi].heading) {
      y += 10.0f;
      if (!ui->mobile_layout || (y + 20.0f >= sc.clip.y && y <= sc.clip.y + sc.clip.h)) {
        ss_label(&sc, groups[gi].heading, x + 6.0f, y);
      }
      y += 20.0f;
    }
    for (size_t k = 0; k < groups[gi].count; k++) {
      ss_page p = groups[gi].pages[k];
      if (!ss_page_allowed(ui, ss->guild_id, p)) continue;
      ccord_rect item = {x, y, w, row_h};
      y += row_pitch;
      if (ui->mobile_layout && (item.y + item.h < sc.clip.y || item.y > sc.clip.y + sc.clip.h)) continue;
      char nm[SS_NAME_LEN];
      bool hov = ss_hit_add(&sc, item, A_PAGE, (uint64_t)p, nullptr, NAME(nm, "page:%s", ss_k_page_names[p]));
      bool sel = !ui->mobile_layout && ss->page == p;
      ui_row_state prs = ui_row_state_of(ui, "sspage", (uint64_t)p, hov, sel, COL(bg_secondary));
      uis_list_row_fill(ui, c->r, item, (uis_state){.hover = hov, .selected = sel}, "sspage", (uint64_t)p);
      ccord_color idle_col = p == SS_DANGER ? COL(danger) : (ui->mobile_layout ? COL(text_normal) : COL(text_muted));
      (void)ss_text(&sc, ss_k_page_labels[p], item.x + 10.0f, item.y + (item.h - ui_line_h(ui, M(font_size_base))) * 0.5f,
                    M(font_size_base), sel,
                    ui_row_state_text(&prs, idle_col, COL(text_interactive_hover), COL(text_interactive_active)),
                    item.w - (ui->mobile_layout ? 32.0f : 16.0f));
      if (ui->mobile_layout) {
        ui_icon_chevron(c->r, item.x + item.w - UIS_SP_4, item.y + item.h * 0.5f, UIS_SP_3, ICON_RIGHT, COL(text_muted));
      }
    }
  }
  ss->sb_content_h = (y + (ui->mobile_layout ? ss->sb_scroll : 0.0f)) - list_top + 16.0f;
  if (ui->mobile_layout) {
    ccord_pop_clip(c->r);
  } else {
    uis_vdivider(ui, c->r, g->sb_w - 1.0f, 0.0f, ui->win_h);
  }
}

ccord_rect ss_mobile_back_rect(const ccord_ui *ui)
{
  if (!ui->mobile_layout || !ui->mobile_server_settings_detail) return (ccord_rect){0};
  return (ccord_rect){ui->safe_insets.left + UIS_SP_3, ui->safe_insets.top + UIS_SP_3, 76.0f, UIS_ICON_BTN};
}

static void draw_close(ss_cx *c)
{
  ccord_ui *ui = c->ui;
  ccord_rect br = ss_mobile_back_rect(ui);
  if (br.w > 0.0f) {
    uis_button(ui, c->r, br, "< Back", UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){0}, "ssback", 0);
  }
  ccord_rect cr = ui->mobile_layout
                    ? (ccord_rect){ui->win_w - ui->safe_insets.right - UIS_ICON_BTN - UIS_SP_3,
                                   ui->safe_insets.top + UIS_SP_3, UIS_ICON_BTN, UIS_ICON_BTN}
                    : (ccord_rect){ui->win_w - 64.0f, 32.0f, UIS_ICON_BTN, UIS_ICON_BTN};
  bool hov = ss_hit_add(c, cr, A_CLOSE, 0, nullptr, "close");
  ccord_color ic = uis_icon_button(ui, c->r, cr, (uis_state){.hover = hov}, "ssclose", 0);
  ui_icon_close(c->r, cr.x + UIS_ICON_BTN * 0.5f, cr.y + UIS_ICON_BTN * 0.5f, 14.0f, ic);
  if (!ui->mobile_layout) {
    ss_text_centered(c, "ESC", (ccord_rect){cr.x - 4.0f, cr.y + UIS_ICON_BTN + 4.0f, cr.w + 8.0f, 16.0f}, SS_CAP(c), true,
                     COL(text_muted));
  }
}

static void draw_unsaved_bar(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  if (!ss_dirty(ss)) return;
  ui_popup_begin(ui, c->r, "ssbar", 0, 160, 16.0f);   // rises into place the moment something is edited
  float bot_in = ui->mobile_layout ? fmaxf(ui->safe_insets.bottom, ui->keyboard_inset) : 0.0f;
  float sr = ui->mobile_layout ? ui->safe_insets.right : 0.0f;
  float bw = fminf(g->col_w, 640.0f), bh = 52.0f;
  ccord_rect bar = {g->x0 + (ui->win_w - sr - g->x0 - bw) * 0.5f, ui->win_h - bot_in - bh - 16.0f, bw, bh};
  bool flash = ui->now_ms < ss->flash_until_ms;
  uis_popover_frame(ui, c->r, bar);
  if (flash) uis_surface(ui, c->r, bar, ui_alpha(COL(danger), 0.18f));
  bool compact_bar = bar.w < 400.0f;
  float reset_w = compact_bar ? 62.0f : 80.0f;
  float save_w = compact_bar ? 76.0f : 112.0f;
  float btns_w = reset_w + save_w + 22.0f;
  (void)ss_text(c, compact_bar ? "Unsaved changes" : "Careful - you have unsaved changes!", bar.x + 12.0f,
                bar.y + (bar.h - ui_line_h(ui, M(font_size_base))) * 0.5f,
                M(font_size_base), true, flash ? COL(danger) : COL(text_header), fmaxf(bar.w - btns_w - 16.0f, 56.0f));
  const char *why = nullptr;
  if (ss->page == SS_ROLES) {
    ccord_perm_verdict v = ss_role_verdict(ui, ss->guild_id, ss->role_sel, false);
    if (v != CCORD_PERM_ALLOWED) why = ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES);
  }
  ss_button(c, (ccord_rect){bar.x + bar.w - btns_w, bar.y + 10.0f, reset_w, 32.0f}, "Reset", BTN_LINK, nullptr, A_RESET, 0, "reset");
  ss_button(c, (ccord_rect){bar.x + bar.w - save_w - 12.0f, bar.y + 10.0f, save_w, 32.0f},
            compact_bar ? "Save" : "Save Changes", BTN_PRIMARY, why, A_SAVE, 0, "save");
  ui_popup_end(c->r);
}

static void draw_toast(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  if (!ui->notice[0] || ui->now_ms >= ui->notice_until_ms) return;
  float bot_in = ui->mobile_layout ? fmaxf(ui->safe_insets.bottom, ui->keyboard_inset) : 0.0f;
  float sr = ui->mobile_layout ? ui->safe_insets.right : 0.0f;
  float tw = fminf(g->col_w, 560.0f);
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_normal));
  ccord_vec2 t = ccord_text_measure(c->r, ui->notice, strlen(ui->notice), &st, tw - 28.0f);
  ccord_rect box = {g->x0 + (ui->win_w - sr - g->x0 - tw) * 0.5f,
                    ui->win_h - bot_in - 52.0f - 22.0f - t.y - 34.0f, tw, t.y + 20.0f};
  float ta, tslide;
  ui_toast_look(ui, &ta, &tslide);
  ccord_push_alpha(c->r, ta);
  ccord_push_offset(c->r, 0.0f, tslide);
  uis_popover_frame(ui, c->r, box);
  ccord_text_draw(c->r, ui->notice, strlen(ui->notice), box.x + 14.0f, box.y + 10.0f, &st, tw - 28.0f);
  ccord_pop_offset(c->r);
  ccord_pop_alpha(c->r);
}

// ---------------------------------------------------------------- draw

void ui_server_settings_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui_server_settings_is_open(ui) || !r) return;
  ss_sync(ui);
  if (!ui_server_settings_is_open(ui)) return;
  struct ui_server_settings *ss = ui->ss;
  const ccord_guild *cur_gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (ss->page == SS_COMMUNITY && cur_gd && cur_gd->feature_community && !ss->requested[SS_COMMUNITY]) {
    ss_request(ui, SS_COMMUNITY, false);
  }
  // Permissions can change under an open window (a role taken away): move to a page that is still allowed, or leave.
  if (!ss_page_allowed(ui, ss->guild_id, ss->page)) {
    bool found = false;
    for (size_t i = 0; i < SS_PAGE_COUNT && !found; i++) {
      ss_page cand = ss_k_sidebar_order[i];
      if (ss_page_allowed(ui, ss->guild_id, cand)) {
        ss->page = cand;
        ss->scroll = 0.0f;
        ccord_scroller_cancel(&ss->scroll_glide);
        ss->focus = SSF_NONE;
        ss_request(ui, ss->page, false);
        found = true;
      }
    }
    if (!found) {
      ui_server_settings_close(ui);
      return;
    }
  }
  ss->nhits = 0;
  ss->rows = 0;
  ss->hover = -1;
  ss_geom g = ss_layout(ui);
  float top_clip = ui->mobile_layout ? (ui->safe_insets.top + UIS_SP_3 + UIS_ICON_BTN + UIS_SP_2) : 0.0f;
  float bot_in = ui->mobile_layout ? fmaxf(ui->safe_insets.bottom, ui->keyboard_inset) : 0.0f;
  float sr_in = ui->mobile_layout ? ui->safe_insets.right : 0.0f;
  ss_cx c = {ui, r, ss, (ccord_rect){g.x0, top_clip, fmaxf(ui->win_w - g.x0 - sr_in, 80.0f), fmaxf(ui->win_h - bot_in - top_clip, 80.0f)}};
  ss->view = c.clip;
  ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h}, COL(bg_primary));

  if (!(ui->mobile_layout && !ui->mobile_server_settings_detail)) {
    ccord_push_clip(r, c.clip);
    ccord_push_alpha(r, ui_enter(ui, "sspagebody", (uint64_t)ss->page, 160));   // a page eases in when chosen
    switch (ss->page) {
      case SS_OVERVIEW: ss_draw_overview(&c, &g); break;
      case SS_ROLES: ss_draw_roles(&c, &g); break;
      case SS_INTEGRATIONS: ss_draw_integrations(&c, &g); break;
      case SS_COMMUNITY: ss_draw_community(&c, &g); break;
      case SS_EMOJI: ss_draw_emoji(&c, &g); break;
      case SS_STICKERS: ss_draw_stickers(&c, &g); break;
      case SS_SOUNDBOARD: ss_draw_soundboard(&c, &g); break;
      case SS_SAFETY: ss_draw_safety(&c, &g); break;
      case SS_AUTOMOD: ss_draw_automod(&c, &g); break;
      case SS_INVITES: ss_draw_invites(&c, &g); break;
      case SS_BANS: ss_draw_bans(&c, &g); break;
      case SS_MEMBERS: ss_draw_members(&c, &g); break;
      case SS_AUDIT: ss_draw_audit(&c, &g); break;
      case SS_DANGER: ss_draw_danger(&c, &g); break;
      case SS_PAGE_COUNT: break;
    }
    ccord_pop_alpha(r);
    ccord_pop_clip(r);
  }

  // Keep the scroll positions inside what was measured.
  float view_h = (ss->page == SS_ROLES && ss->editor_view.h > 0.0f) ? ss->editor_view.h : c.clip.h;
  float max_scroll = fmaxf(0.0f, ss->content_h - view_h);
  if (ss->scroll > max_scroll) ss->scroll = max_scroll;
  if (ss->scroll < 0.0f) ss->scroll = 0.0f;
  if (ss->page == SS_ROLES) {
    float max_list = fmaxf(0.0f, ss->list_content_h - ss->list_view.h);
    if (ss->list_scroll > max_list) ss->list_scroll = max_list;
  }
  if (ss->page == SS_MEMBERS && ss->modview_uid != CCORD_ID_NONE && ss->modview_rect.h > 0.0f) {
    float max_mv = fmaxf(0.0f, ss->modview_content_h - ss->modview_rect.h);
    if (ss->modview_scroll > max_mv) ss->modview_scroll = max_mv;
    if (ss->modview_scroll < 0.0f) ss->modview_scroll = 0.0f;
  }
  ss->view_h = view_h;

  ss_cx top = c;   // the chrome lives outside the scrolling content, so it is not clipped to it
  top.clip = (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h};
  if (g.sb_w > 0.0f) draw_sidebar(&top, &g);
  draw_close(&top);
  draw_unsaved_bar(&top, &g);
  draw_toast(&top, &g);
}

