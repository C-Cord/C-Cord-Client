// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Account card (the user-panel popout). See status_card.h.
//
// Layout is computed once per frame (compute_layout) relative to the card origin; draw adds the origin and records every
// control as a ui_sc_item, which the input handler and --verify-slice status-card hit-test. The card is anchored directly
// above the user panel, left-aligned with it. Presence rows apply at once through ccord_session_set_presence; the custom
// status is applied by Set (or Enter) and confirmed with a checkmark; there is no Save and no Cancel.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "settings_profile.h"
#include "status_card.h"
#include "ui_internal.h"
#include "ui_style.h"

#include <ccord/emoji/emoji.h>

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define SC_W 300.0f
#define SC_BANNER_H 60.0f
#define SC_AVATAR_D 56.0f
#define SC_GAP 8.0f          // between the card and the user panel
#define SC_PENDING_MS 10000u // how long a request can still be refused and restore the old marker

static const char *const k_pres_labels[4] = {"Online", "Idle", "Do Not Disturb", "Invisible"};
static const ccord_presence k_pres[4] = {CCORD_PRESENCE_ONLINE, CCORD_PRESENCE_IDLE, CCORD_PRESENCE_DND,
                                         CCORD_PRESENCE_OFFLINE};
static const char *const k_dur_labels[5] = {"Never", "30 min", "1 hour", "4 hours", "Today"};
static const char *const k_emojis[UI_SC_EMOJI_N] = {"\xF0\x9F\x98\x80", "\xF0\x9F\x94\xA5", "\xF0\x9F\x92\xBB",
                                                    "\xF0\x9F\x8E\xAE", "\xF0\x9F\x8E\xA7", "\xF0\x9F\x92\xA4"};

typedef struct sc_layout {
  float h;
  float banner_h;
  float avatar_cx, avatar_cy;
  float name_y, user_y;
  float div_y[3];
  ccord_rect edit;
  ccord_rect pres[4];
  ccord_rect emoji_btn, input;
  ccord_rect emoji_pick[UI_SC_EMOJI_N];
  bool has_pick;
  ccord_rect dur, clear, set;
  bool has_clear;
  float err_y;
  bool has_err;
} sc_layout;

static const ccord_user *self_user(const ccord_ui *ui)
{
  if (!ui->d.store) return nullptr;
  ccord_id id = ccord_store_self_id(ui->d.store);
  return id != CCORD_ID_NONE ? ccord_store_user(ui->d.store, id) : nullptr;
}

static bool custom_alive(const ccord_ui *ui, const ccord_user *u)
{
  if (!u || (u->custom_status_text[0] == '\0' && u->custom_status_emoji[0] == '\0')) return false;
  return u->custom_status_expires_ms == 0 || ui->now_unix_ms <= 0 || u->custom_status_expires_ms > ui->now_unix_ms;
}

static int presence_index(ccord_presence p)
{
  for (int i = 0; i < 4; i++) {
    if (k_pres[i] == p) return i;
  }
  return 0;
}

static ccord_color presence_col(const ccord_ui *ui, int idx)
{
  switch (idx) {
    case 0: return COL(status_online);
    case 1: return COL(status_idle);
    case 2: return COL(status_dnd);
    default: return COL(status_offline);
  }
}

static float line_of(const ccord_ui *ui, float size)
{
  return ui_line_h(ui, size);
}

static void compute_layout(const ccord_ui *ui, float card_w, sc_layout *L)
{
  memset(L, 0, sizeof *L);
  float nh = line_of(ui, M(font_size_base)), sh = line_of(ui, M(font_size_small) - 1.0f);
  float in = UIS_SP_3, iw = card_w - 2.0f * in;
  L->banner_h = SC_BANNER_H;
  L->avatar_cx = UIS_SP_4 + SC_AVATAR_D * 0.5f;
  L->avatar_cy = SC_BANNER_H;
  L->name_y = SC_BANNER_H + SC_AVATAR_D * 0.5f + UIS_SP_2;
  L->user_y = L->name_y + nh;
  float y = L->user_y + sh + UIS_SP_3;
  float rx = UIS_SP_1 + 2.0f, rw = card_w - 2.0f * rx;
  L->div_y[0] = y;
  y += 1.0f + UIS_SP_1;
  L->edit = (ccord_rect){rx, y, rw, UIS_H_BTN};
  y += UIS_H_BTN + UIS_SP_1;
  L->div_y[1] = y;
  y += 1.0f + UIS_SP_1;
  for (int i = 0; i < 4; i++) {
    L->pres[i] = (ccord_rect){rx, y, rw, UIS_H_BTN};
    y += UIS_H_BTN;
  }
  y += UIS_SP_1;
  L->div_y[2] = y;
  y += 1.0f + UIS_SP_2;
  L->emoji_btn = (ccord_rect){in, y, UIS_H_INPUT, UIS_H_INPUT};
  L->input = (ccord_rect){in + UIS_H_INPUT + UIS_SP_2, y, iw - UIS_H_INPUT - UIS_SP_2, UIS_H_INPUT};
  y += UIS_H_INPUT + UIS_SP_2;
  if (ui->sc.emoji_row) {
    L->has_pick = true;
    float ew = (iw - (float)(UI_SC_EMOJI_N - 1) * UIS_SP_1) / (float)UI_SC_EMOJI_N;
    for (int i = 0; i < UI_SC_EMOJI_N; i++) {
      L->emoji_pick[i] = (ccord_rect){in + (float)i * (ew + UIS_SP_1), y, ew, UIS_H_BTN_SM};
    }
    y += UIS_H_BTN_SM + UIS_SP_2;
  }
  L->has_clear = custom_alive(ui, self_user(ui));
  float set_w = 64.0f, x = in + iw;
  L->set = (ccord_rect){x - set_w, y, set_w, UIS_H_BTN_SM};
  x -= set_w + UIS_SP_2;
  if (L->has_clear) {
    L->clear = (ccord_rect){x - UIS_H_BTN_SM, y, UIS_H_BTN_SM, UIS_H_BTN_SM};
    x -= UIS_H_BTN_SM + UIS_SP_2;
  }
  L->dur = (ccord_rect){in, y, x - in, UIS_H_BTN_SM};
  y += UIS_H_BTN_SM + UIS_SP_2;
  if (ui->sc.error[0]) {
    L->has_err = true;
    L->err_y = y;
    y += line_of(ui, M(font_size_small) - 1.0f) + UIS_SP_2;
  }
  L->h = y;
}

static void add_item(ui_status_card *sc, ui_sc_kind kind, int arg, ccord_rect rect, const char *label)
{
  if (sc->nitems >= UI_SC_ITEMS_MAX) return;
  ui_sc_item *it = &sc->items[sc->nitems++];
  it->kind = kind;
  it->arg = arg;
  it->rect = rect;
  snprintf(it->label, sizeof it->label, "%s", label ? label : "");
}

static ccord_rect off(ccord_rect r, float x, float y)
{
  return (ccord_rect){r.x + x, r.y + y, r.w, r.h};
}

static bool hov(const ccord_ui *ui, ccord_rect r)
{
  return ui->mouse_in && ui_in_rect(ui->mx, ui->my, r);
}

static void draw_check(ccord_renderer *r, float cx, float cy, float s, ccord_color c)
{
  ccord_vec2 a = {cx - s * 0.42f, cy + s * 0.02f}, b = {cx - s * 0.1f, cy + s * 0.34f}, d = {cx + s * 0.46f, cy - s * 0.32f};
  ccord_draw_line(r, a, b, 2.0f, c);
  ccord_draw_line(r, b, d, 2.0f, c);
}

static void text_row(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *s, float x, float maxw,
                     ccord_font_style font, float size, ccord_color col)
{
  ccord_text_style st = ui_style(ui, font, size, col);
  float ty = rc.y + (rc.h - ui_line_h(ui, st.size)) * 0.5f;
  ccord_text_draw_ellipsis(r, s, strlen(s), x, ty, maxw, &st);
}

bool ui_status_card_is_open(const ccord_ui *ui)
{
  return ui && ui->sc.open;
}

bool ui_status_card_confirming(const ccord_ui *ui)
{
  return ui && ui->sc.open && ui->sc.confirm_until_ms != 0 && ui->now_ms < ui->sc.confirm_until_ms;
}

void ui_status_card_init(ccord_ui *ui)
{
  ccord_text_edit_init(&ui->sc.text, 128);
}

void ui_status_card_free(ccord_ui *ui)
{
  ccord_text_edit_free(&ui->sc.text);
}

void ui_status_card_close(ccord_ui *ui)
{
  if (!ui || !ui->sc.open) return;
  ui->sc.open = false;
  ui->sc.card = (ccord_rect){0};
  ui->sc.nitems = 0;
  ui->sc.scroll = 0.0f;
  ui->sc.max_scroll = 0.0f;
  if (ui->touch_text_focused) {
    ui->touch_text_focused = false;
    if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
  }
  ui->dirty = true;
}

void ui_status_card_open(ccord_ui *ui)
{
  if (!ui) return;
  ui_menu_close(ui);
  ui_status_card *sc = &ui->sc;
  sc->open = true;
  sc->duration = 0;
  sc->emoji_row = false;
  sc->confirm_until_ms = 0;
  sc->error[0] = '\0';
  sc->emoji[0] = '\0';
  ui->sc_emoji_id = CCORD_ID_NONE;
  sc->last_caret_on = ui->caret_on;
  sc->card = (ccord_rect){0};
  sc->nitems = 0;
  sc->scroll = 0.0f;
  sc->max_scroll = 0.0f;
  ui->touch_text_focused = false;
  ccord_text_edit_clear(&sc->text);
  const ccord_user *u = self_user(ui);
  if (custom_alive(ui, u)) {
    ccord_error e;
    if (u->custom_status_text[0]) (void)ccord_text_edit_insert(&sc->text, u->custom_status_text, strlen(u->custom_status_text), &e);
    snprintf(sc->emoji, sizeof sc->emoji, "%s", u->custom_status_emoji);
  }
  ui->dirty = true;
}

void ui_status_card_tick(ccord_ui *ui)
{
  ui_status_card *sc = &ui->sc;
  if (sc->presence_until_ms && ui->now_ms >= sc->presence_until_ms) sc->presence_until_ms = 0;
  if (sc->custom_until_ms && ui->now_ms >= sc->custom_until_ms) sc->custom_until_ms = 0;
  if (!sc->open) return;
  if (sc->confirm_until_ms) {
    ui->dirty = true;   // the checkmark leaves (and the Set button returns) on a redraw
    if (ui->now_ms >= sc->confirm_until_ms) sc->confirm_until_ms = 0;
  }
  if (ui->caret_on != sc->last_caret_on) {
    sc->last_caret_on = ui->caret_on;
    ui->dirty = true;
  }
}

// ---------------------------------------------------------------- drawing

void ui_status_card_draw(ccord_ui *ui, ccord_renderer *r)
{
  ui_status_card *sc = &ui->sc;
  if (!sc->open) return;
  const ccord_layout_rect *p = &ui->layout.user_panel;
  if (!ui->mobile_layout && (p->w <= 0.0f || p->h <= 0.0f)) {
    ui_status_card_close(ui);
    return;
  }
  float sl = SC_GAP + ui->safe_insets.left, sr_in = SC_GAP + ui->safe_insets.right;
  float st = SC_GAP + ui->safe_insets.top;
  float sb = SC_GAP + fmaxf(ui->safe_insets.bottom, ui->keyboard_inset);
  float avail_w = fmaxf(ui->win_w - sl - sr_in, 160.0f);
  float avail_h = fmaxf(ui->win_h - st - sb, 140.0f);
  float cw = fminf(SC_W, avail_w);
  sc_layout L;
  compute_layout(ui, cw, &L);
  float ch = fminf(L.h, avail_h);
  sc->max_scroll = L.h > ch ? (L.h - ch) : 0.0f;
  sc->scroll = fminf(fmaxf(sc->scroll, 0.0f), sc->max_scroll);
  float cx = ui->mobile_layout ? (sl + (avail_w - cw) * 0.5f) : (p->x + SC_GAP);
  if (cx + cw > ui->win_w - sr_in) cx = ui->win_w - sr_in - cw;
  if (cx < sl) cx = sl;
  float cy = ui->mobile_layout ? (st + (avail_h - ch) * 0.5f) : (p->y - SC_GAP - ch);
  if (cy + ch > ui->win_h - sb) cy = ui->win_h - sb - ch;
  if (cy < st) cy = st;
  ccord_rect card = {cx, cy, cw, ch};
  sc->card = card;
  sc->panel = (ccord_rect){p->x, p->y, p->w, p->h};
  sc->nitems = 0;

  const ccord_user *u = self_user(ui);
  ccord_user fallback = {0};
  if (!u) {
    fallback.name = "";
    fallback.presence = CCORD_PRESENCE_ONLINE;
    u = &fallback;
  }
  const char *dname = (u->display_name && u->display_name[0]) ? u->display_name : (u->name ? u->name : "");
  const char *uname = (u->handle && u->handle[0]) ? u->handle : (u->name ? u->name : "");
  int cur = presence_index(u->presence);

  ui_popup_begin(ui, r, "stcard", 0, UI_POPUP_MS, 6.0f);
  uis_popover_frame(ui, r, card);
  float rad = ui->theme->m.composer_radius;
  ccord_push_clip(r, card);
  float oy = card.y - sc->scroll;

  // Banner (accent colour strip, or the banner image) with the avatar and its presence dot.
  if (ui->d.session && u->id != CCORD_ID_NONE) {
    ccord_error perr;
    (void)ccord_session_request_profile_ex(ui->d.session, u->id, CCORD_ID_NONE, ui->now_ms, false, &perr);
  }
  const ccord_user_profile *prof = ui->d.store ? ccord_store_user_profile(ui->d.store, u->id) : nullptr;
  ccord_user pview = *u;
  if (prof) {
    if (prof->banner_url && prof->banner_url[0]) pview.banner_url = prof->banner_url;
    if (prof->has_accent_color || prof->accent_color != 0) {
      pview.accent_color = prof->accent_color;
      pview.has_accent_color = true;
    }
  }
  ui_profile_look look;
  ui_profile_resolve(prof, CCORD_ID_NONE, &pview, &look);
  ui_profile_text ptx = ui_profile_text_colors(ui, &look);
  ui_profile_draw_theme_bg(ui, r, &look, card, L.banner_h, rad);
  ccord_rect banner = {card.x, oy, card.w, L.banner_h};
  ui_profile_draw_banner(ui, r, &look, &pview, banner, rad);
  float acx = card.x + L.avatar_cx, acy = oy + L.avatar_cy;
  ccord_color av_ring = ui_profile_card_bg(ui, &look, COL(bg_floating));
  ccord_draw_circle(r, acx, acy, SC_AVATAR_D * 0.5f + 4.0f, av_ring);
  ui_draw_avatar(ui, r, &pview, acx, acy, SC_AVATAR_D, av_ring, false);
  float tx = card.x + UIS_SP_4, tw = card.w - 2.0f * UIS_SP_4;
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), ptx.header);
  ccord_text_style us = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, ptx.muted);
  ccord_text_draw_ellipsis(r, dname, strlen(dname), tx, oy + L.name_y, tw, &ns);
  ccord_text_draw_ellipsis(r, uname, strlen(uname), tx, oy + L.user_y, tw, &us);

  for (int i = 0; i < 3; i++) uis_divider(ui, r, card.x + UIS_SP_3, oy + L.div_y[i], card.w - 2.0f * UIS_SP_3);

  float fs = M(font_size_small);
  ccord_color row_fg = look.has_theme ? ptx.secondary : COL(text_normal);
  // Edit Profile
  ccord_rect er = off(L.edit, card.x, oy);
  add_item(sc, UI_SC_EDIT_PROFILE, 0, er, "Edit Profile");
  uis_list_row_fill(ui, r, er, (uis_state){.hover = hov(ui, er)}, "scrow", 100u);
  ui_icon_user_profile(r, er.x + 18.0f, er.y + er.h * 0.5f, 16.0f, row_fg, av_ring);
  text_row(ui, r, er, "Edit Profile", er.x + 36.0f, er.w - 48.0f, CCORD_FONT_REGULAR, fs, row_fg);
  ui_icon_chevron(r, er.x + er.w - 14.0f, er.y + er.h * 0.5f, 10.0f, ICON_RIGHT, ptx.muted);

  // Presence rows: the marked one is the current status.
  for (int i = 0; i < 4; i++) {
    ccord_rect pr = off(L.pres[i], card.x, oy);
    add_item(sc, UI_SC_PRESENCE, i, pr, k_pres_labels[i]);
    bool on = i == cur;
    uis_list_row_fill(ui, r, pr, (uis_state){.hover = hov(ui, pr), .selected = on}, "scrow", (uint64_t)i);
    ccord_draw_circle(r, pr.x + 18.0f, pr.y + pr.h * 0.5f, 5.0f, presence_col(ui, i));
    if (i == 1) ccord_draw_circle(r, pr.x + 16.0f, pr.y + pr.h * 0.5f - 1.5f, 3.0f, av_ring);   // idle: a crescent
    if (i == 2) ccord_draw_rrect(r, (ccord_rect){pr.x + 14.5f, pr.y + pr.h * 0.5f - 1.0f, 7.0f, 2.0f}, 1.0f, av_ring);
    if (i == 3) ccord_draw_circle(r, pr.x + 18.0f, pr.y + pr.h * 0.5f, 2.5f, av_ring);
    text_row(ui, r, pr, k_pres_labels[i], pr.x + 36.0f, pr.w - 72.0f, on ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, fs,
             on ? ptx.header : row_fg);
    if (on) draw_check(r, pr.x + pr.w - 18.0f, pr.y + pr.h * 0.5f, 12.0f, COL(brand));
  }

  // Custom status: emoji button + field, optional preset emoji, clear-after, clear (x), Set / confirmation.
  ccord_rect eb = off(L.emoji_btn, card.x, oy);
  add_item(sc, UI_SC_EMOJI_BUTTON, 0, eb, "Emoji");
  uis_state ebs = {.hover = hov(ui, eb), .selected = sc->emoji_row};
  ccord_color eic = uis_icon_button(ui, r, eb, ebs, "scemoji", 0);
  if (sc->emoji[0]) {
    size_t elen = strlen(sc->emoji);
    bool is_uemoji = ccord_emoji_match_prefix(sc->emoji, elen) == elen;
    ccord_text_style es = ui_style(ui, is_uemoji ? CCORD_FONT_EMOJI : CCORD_FONT_REGULAR, M(font_size_large), COL(text_normal));
    ccord_vec2 m = ccord_text_measure(r, sc->emoji, elen, &es, 0.0f);
    ccord_text_draw(r, sc->emoji, elen, eb.x + (eb.w - m.x) * 0.5f, eb.y + (eb.h - m.y) * 0.5f, &es, 0.0f);
  } else {   // an empty smile
    float ex = eb.x + eb.w * 0.5f, ey = eb.y + eb.h * 0.5f;
    ccord_draw_circle(r, ex, ey, 9.0f, eic);
    ccord_draw_circle(r, ex, ey, 7.0f, ebs.hover || ebs.selected ? COL(bg_item_hover) : COL(bg_floating));
    ccord_draw_circle(r, ex - 3.0f, ey - 2.0f, 1.3f, eic);
    ccord_draw_circle(r, ex + 3.0f, ey - 2.0f, 1.3f, eic);
    ccord_draw_line(r, (ccord_vec2){ex - 3.5f, ey + 2.5f}, (ccord_vec2){ex + 3.5f, ey + 2.5f}, 1.5f, eic);
  }
  ccord_rect ir = off(L.input, card.x, oy);
  add_item(sc, UI_SC_TEXT, 0, ir, "Custom status");
  (void)uis_input(ui, r, ir, &sc->text, "Set a custom status",
                  (uis_state){.focused = !ui->touch_mode || ui->touch_text_focused}, false, "scinput", 0);
  if (L.has_pick) {
    for (int i = 0; i < UI_SC_EMOJI_N; i++) {
      ccord_rect pr = off(L.emoji_pick[i], card.x, oy);
      add_item(sc, UI_SC_EMOJI_PICK, i, pr, "Emoji choice");
      uis_tab(ui, r, pr, k_emojis[i], (uis_state){.hover = hov(ui, pr), .selected = strcmp(sc->emoji, k_emojis[i]) == 0},
              "scpick", (uint64_t)i);
    }
  }
  ccord_rect dr = off(L.dur, card.x, oy);
  char dlabel[48];
  snprintf(dlabel, sizeof dlabel, "Clear after: %s", k_dur_labels[sc->duration]);
  add_item(sc, UI_SC_DURATION, 0, dr, dlabel);
  uis_button(ui, r, dr, dlabel, UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){.hover = hov(ui, dr)}, "scdur", 0);
  if (L.has_clear) {
    ccord_rect cr = off(L.clear, card.x, oy);
    add_item(sc, UI_SC_CLEAR, 0, cr, "Clear status");
    ccord_color cic = uis_icon_button(ui, r, cr, (uis_state){.hover = hov(ui, cr)}, "scclear", 0);
    ui_icon_close(r, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f, 11.0f, cic);
    if (hov(ui, cr)) (void)uis_tooltip(ui, r, cr, "Clear status", UIS_TIP_ABOVE, 200u);
  }
  ccord_rect sr = off(L.set, card.x, oy);
  if (ui_status_card_confirming(ui)) {
    ccord_draw_rrect(r, sr, uis_tokens_of(ui).r_control, COL(status_online));
    draw_check(r, sr.x + sr.w * 0.5f, sr.y + sr.h * 0.5f, 14.0f, COL(text_on_brand));
  } else {
    add_item(sc, UI_SC_SET, 0, sr, "Set");
    uis_button(ui, r, sr, "Set", UIS_BTN_PRIMARY, UIS_BTN_S_SM, (uis_state){.hover = hov(ui, sr)}, "scset", 0);
  }
  if (L.has_err) {
    ccord_rect rr = {card.x + UIS_SP_3, oy + L.err_y, card.w - 2.0f * UIS_SP_3, line_of(ui, fs - 1.0f)};
    text_row(ui, r, rr, sc->error, rr.x, rr.w, CCORD_FONT_REGULAR, fs - 1.0f, COL(danger));
  }
  if (sc->max_scroll > 0.0f) {
    (void)ui_draw_list_scrollbar(ui, r, card, L.h, sc->scroll, "scscroll", 0);
  }
  ui_profile_draw_effect(ui, r, &look, card, rad, false);
  ccord_pop_clip(r);
  ui_profile_draw_frame(ui, r, &look, card);
  ui_popup_end(r);
}

// ---------------------------------------------------------------- actions

static void set_error(ui_status_card *sc, const char *what, const char *why)
{
  snprintf(sc->error, sizeof sc->error, "%s: %.180s", what, why && why[0] ? why : "unknown error");
}

static void apply_presence(ccord_ui *ui, int idx)
{
  ui_status_card *sc = &ui->sc;
  const ccord_user *u = self_user(ui);
  ccord_presence want = k_pres[idx], was = u ? u->presence : CCORD_PRESENCE_ONLINE;
  sc->error[0] = '\0';
  if (want == was) return;
  if (!ui->d.session) {   // no session (offline preview): the store is all there is
    ccord_error e;
    if (ui->d.store) (void)ccord_store_update_presence(ui->d.store, ccord_store_self_id(ui->d.store), want, nullptr, &e);
    return;
  }
  ccord_error err;
  if (ccord_session_set_presence(ui->d.session, want, nullptr, &err) != CCORD_OK) {
    set_error(sc, "Couldn't change status", err.message);
    return;
  }
  sc->prev_presence = (int)was;
  sc->presence_until_ms = ui->now_ms + SC_PENDING_MS;
}

static int64_t duration_expiry(const ccord_ui *ui, int duration)
{
  int64_t base = ui->now_unix_ms > 0 ? ui->now_unix_ms : 1700000000000LL;
  switch (duration) {
    case 1: return base + 30LL * 60 * 1000;
    case 2: return base + 60LL * 60 * 1000;
    case 3: return base + 4LL * 60 * 60 * 1000;
    case 4: return ((base / 86400000LL) + 1LL) * 86400000LL;
    default: return 0;
  }
}

static void send_custom(ccord_ui *ui, const char *emoji, const char *text, int64_t expires_ms, bool confirm)
{
  ui_status_card *sc = &ui->sc;
  sc->error[0] = '\0';
  if (!ui->d.session) {
    ccord_error e;
    bool clr = emoji[0] == '\0' && text[0] == '\0';
    if (ui->d.store) (void)ccord_store_set_self_custom_status(ui->d.store, emoji, text, expires_ms, clr, &e);
  } else {
    ccord_error err;
    if (ccord_session_set_custom_status(ui->d.session, emoji, emoji[0] ? ui->sc_emoji_id : CCORD_ID_NONE, text, expires_ms, &err) != CCORD_OK) {
      set_error(sc, "Couldn't set status", err.message);
      return;
    }
    sc->custom_until_ms = ui->now_ms + SC_PENDING_MS;
  }
  if (confirm) sc->confirm_until_ms = ui->now_ms + UI_SC_CONFIRM_MS;
}

static void apply_set(ccord_ui *ui)
{
  ui_status_card *sc = &ui->sc;
  const char *text = ccord_text_edit_cstr(&sc->text);
  if (text[0] == '\0' && sc->emoji[0] == '\0') return;   // nothing to set; the clear (x) removes a status
  send_custom(ui, sc->emoji, text, duration_expiry(ui, sc->duration), true);
}

static void apply_clear(ccord_ui *ui)
{
  ui_status_card *sc = &ui->sc;
  ccord_text_edit_clear(&sc->text);
  sc->emoji[0] = '\0';
  ui->sc_emoji_id = CCORD_ID_NONE;
  sc->duration = 0;
  sc->confirm_until_ms = 0;
  send_custom(ui, "", "", 0, false);
}

void ui_status_card_on_failure(ccord_ui *ui, ccord_admin_op op)
{
  if (!ui) return;
  ui_status_card *sc = &ui->sc;
  if (op == CCORD_ADMIN_USER_SETTINGS_PATCH && sc->presence_until_ms) {
    sc->presence_until_ms = 0;
    set_error(sc, "Couldn't change status", "Discord refused it");
    ccord_error e;
    if (ui->d.session) (void)ccord_session_set_presence(ui->d.session, (ccord_presence)sc->prev_presence, nullptr, &e);
    else if (ui->d.store) (void)ccord_store_update_presence(ui->d.store, ccord_store_self_id(ui->d.store), (ccord_presence)sc->prev_presence, nullptr, &e);
    ui->dirty = true;
  } else if (op == CCORD_ADMIN_SET_CUSTOM_STATUS && sc->custom_until_ms) {
    sc->custom_until_ms = 0;
    sc->confirm_until_ms = 0;
    set_error(sc, "Couldn't set status", "Discord refused it");
    ui->dirty = true;
  }
}

// ---------------------------------------------------------------- input

static bool edit_key(ccord_text_edit *t, const ccord_input_event *ev)
{
  bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
  bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
  switch (ev->key) {
    case CCORD_KEY_BACKSPACE:
      if (ctrl) ccord_text_edit_backspace_word(t);
      else ccord_text_edit_backspace(t);
      return true;
    case CCORD_KEY_DELETE:
      if (ctrl) ccord_text_edit_delete_word(t);
      else ccord_text_edit_delete(t);
      return true;
    case CCORD_KEY_LEFT: ccord_text_edit_move(t, ctrl ? -3 : -1, shift); return true;
    case CCORD_KEY_RIGHT: ccord_text_edit_move(t, ctrl ? 3 : 1, shift); return true;
    case CCORD_KEY_HOME: ccord_text_edit_move(t, -2, shift); return true;
    case CCORD_KEY_END: ccord_text_edit_move(t, 2, shift); return true;
    case CCORD_KEY_A:
      if (ctrl) ccord_text_edit_select_all(t);
      return ctrl;
    case CCORD_KEY_Z:
      if (ctrl) {
        if (shift) (void)ccord_text_edit_redo(t);
        else (void)ccord_text_edit_undo(t);
      }
      return ctrl;
    case CCORD_KEY_Y:
      if (ctrl) (void)ccord_text_edit_redo(t);
      return ctrl;
    default: return false;
  }
}

static void activate(ccord_ui *ui, const ui_sc_item *it)
{
  ui_status_card *sc = &ui->sc;
  switch (it->kind) {
    case UI_SC_EDIT_PROFILE:
      ui_status_card_close(ui);
      ui_settings_open_profile(ui);   // Settings > Profile
      break;
    case UI_SC_PRESENCE: apply_presence(ui, it->arg); break;
    case UI_SC_EMOJI_BUTTON:
      ui_expr_picker_open(ui, 2, ccord_store_selected_channel(ui->d.store), CCORD_ID_NONE);   // the full emoji tab
      ui->emoji_picker_for_status = ui->emoji_picker_open;
      break;
    case UI_SC_EMOJI_PICK:
      if (it->arg >= 0 && it->arg < UI_SC_EMOJI_N) {
        if (strcmp(sc->emoji, k_emojis[it->arg]) == 0) sc->emoji[0] = '\0';
        else snprintf(sc->emoji, sizeof sc->emoji, "%s", k_emojis[it->arg]);
      }
      break;
    case UI_SC_TEXT:
      ccord_text_edit_move(&sc->text, 2, false);
      if (ui->touch_mode) {
        ui->touch_text_focused = true;
        if (sc->max_scroll > 0.0f) sc->scroll = sc->max_scroll;
      }
      break;
    case UI_SC_DURATION: sc->duration = (sc->duration + 1) % 5; break;
    case UI_SC_SET:
      ui->touch_text_focused = false;
      apply_set(ui);
      break;
    case UI_SC_CLEAR:
      ui->touch_text_focused = false;
      apply_clear(ui);
      break;
  }
  ui->dirty = true;
}

bool ui_status_card_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  ui_status_card *sc = &ui->sc;
  if (!sc->open) return false;
  // The emoji picker opened from the card owns input until it closes; the card stays open behind it.
  if (ui->emoji_picker_open && ui->emoji_picker_for_status) return false;
  // Anything else taking the screen (settings, a modal, the lightbox) ends the card; the event goes on to it.
  if (ui->settings_open || ui->modal_kind != UI_MODAL_NONE || ui->lightbox_open || ui->quick_switcher_open) {
    ui_status_card_close(ui);
    return false;
  }
  bool inside = sc->card.w > 0.0f && ui_in_rect(ev->x, ev->y, sc->card);
  switch (ev->type) {
    case CCORD_IN_MOUSE_MOVE:
      ui->mx = ev->x;
      ui->my = ev->y;
      ui->mouse_in = true;
      ui->dirty = true;
      return inside;
    case CCORD_IN_MOUSE_DOWN:
      ui->mx = ev->x;
      ui->my = ev->y;
      ui->mouse_in = true;
      ui->dirty = true;
      if (sc->card.w <= 0.0f) return true;   // not drawn yet
      if (!inside) {
        ui_status_card_close(ui);   // a click outside closes it and is not passed on
        return true;
      }
      if (ev->button == 1) {
        for (size_t i = 0; i < sc->nitems; i++) {
          if (ui_in_rect(ev->x, ev->y, sc->items[i].rect)) {
            ui_sc_item it = sc->items[i];   // activate may close the card and reset the list
            activate(ui, &it);
            break;
          }
        }
      }
      return true;
    case CCORD_IN_MOUSE_UP: return inside;
    case CCORD_IN_WHEEL:
      if (inside && sc->max_scroll > 0.0f) {
        sc->scroll = fminf(fmaxf(sc->scroll - ev->wheel * 36.0f, 0.0f), sc->max_scroll);
        ui->dirty = true;
      }
      return inside;
    case CCORD_IN_TEXT: {
      size_t n = strnlen(ev->text, sizeof ev->text);
      ccord_error err;
      if (ccord_text_edit_insert(&sc->text, ev->text, n, &err) == CCORD_OK) sc->error[0] = '\0';
      ui->caret_epoch_ms = ui->now_ms;
      ui->caret_on = true;
      ui->dirty = true;
      return true;
    }
    case CCORD_IN_KEY_DOWN:
      ui->dirty = true;
      if (ev->key == CCORD_KEY_ESCAPE) {
        ui_status_card_close(ui);
        return true;
      }
      if (ev->key == CCORD_KEY_RETURN) {
        apply_set(ui);
        return true;
      }
      if (edit_key(&sc->text, ev)) {
        ui->caret_epoch_ms = ui->now_ms;
        ui->caret_on = true;
      }
      return true;   // the card owns the keyboard while it is open
    default: return false;
  }
}
