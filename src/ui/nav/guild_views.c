// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Member-facing Server Guide & Channels & Roles (Customize + Browse Channels) views (desktop + mobile).
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ccord/store/admin.h>

#include "ui_core_internal.h"
#include "ui_style.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define GV_HITS_MAX 192u

typedef enum gv_hit_kind : uint8_t {
  GV_HIT_NONE = 0,
  GV_HIT_TAB,
  GV_HIT_CHANNEL_JUMP,
  GV_HIT_CHANNEL_TOGGLE,
  GV_HIT_SHOW_ALL,
  GV_HIT_ONBOARD_OPT,
  GV_HIT_OPEN_CR,
  GV_HIT_COPY_VANITY,
} gv_hit_kind;

typedef struct gv_hit {
  ccord_rect rect;
  gv_hit_kind kind;
  int tab;
  ccord_id id;
  ccord_id prompt_id;
  bool single_select;
} gv_hit;

typedef struct gv_runtime {
  const ccord_ui *owner;
  gv_hit hits[GV_HITS_MAX];
  size_t nhits;
  ccord_rect view_rect;
} gv_runtime;

static gv_runtime g_gv;

static void gv_push_hit(ccord_rect r, gv_hit_kind kind, int tab, ccord_id id, ccord_id prompt_id, bool single_select)
{
  if (g_gv.nhits >= GV_HITS_MAX || r.w <= 0.0f || r.h <= 0.0f) return;
  g_gv.hits[g_gv.nhits++] = (gv_hit){r, kind, tab, id, prompt_id, single_select};
}

static bool gv_opt_selected(const ccord_ui *ui, ccord_id opt_id)
{
  if (!ui || opt_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < ui->nonboarding_opts; i++) {
    if (ui->onboarding_opts[i] == opt_id) return true;
  }
  return false;
}

static bool gv_opt_set(ccord_ui *ui, ccord_id opt_id, bool on)
{
  if (!ui || opt_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < ui->nonboarding_opts; i++) {
    if (ui->onboarding_opts[i] == opt_id) {
      if (!on) {
        ui->onboarding_opts[i] = ui->onboarding_opts[--ui->nonboarding_opts];
        return true;
      }
      return false;
    }
  }
  if (on && ui->nonboarding_opts < sizeof ui->onboarding_opts / sizeof ui->onboarding_opts[0]) {
    ui->onboarding_opts[ui->nonboarding_opts++] = opt_id;
    return true;
  }
  return false;
}

bool ui_channel_is_hidden(const ccord_ui *ui, ccord_id channel_id)
{
  if (!ui || channel_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < ui->nhidden_channels; i++) {
    if (ui->hidden_channels[i] == channel_id) return true;
  }
  return false;
}

static void gv_channel_set_hidden(ccord_ui *ui, ccord_id channel_id, bool hide)
{
  if (!ui || channel_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < ui->nhidden_channels; i++) {
    if (ui->hidden_channels[i] == channel_id) {
      if (!hide) {
        ui->hidden_channels[i] = ui->hidden_channels[--ui->nhidden_channels];
      }
      return;
    }
  }
  if (hide && ui->nhidden_channels < sizeof ui->hidden_channels / sizeof ui->hidden_channels[0]) {
    ui->hidden_channels[ui->nhidden_channels++] = channel_id;
  }
}

void ui_channel_toggle_hidden(ccord_ui *ui, ccord_id channel_id)
{
  if (!ui || channel_id == CCORD_ID_NONE) return;
  bool now_hidden = !ui_channel_is_hidden(ui, channel_id);
  gv_channel_set_hidden(ui, channel_id, now_hidden);
  ui->built_rev = UINT64_MAX;
  ui->built_lists_rev = UINT64_MAX;
  ui_settings_save(ui);
  ui->dirty = true;
}

bool ui_guild_show_all_channels(const ccord_ui *ui, ccord_id guild_id)
{
  if (!ui || guild_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < ui->nshow_all_guilds; i++) {
    if (ui->show_all_guilds[i] == guild_id) return true;
  }
  return false;
}

void ui_guild_toggle_show_all_channels(ccord_ui *ui, ccord_id guild_id)
{
  if (!ui || guild_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < ui->nshow_all_guilds; i++) {
    if (ui->show_all_guilds[i] == guild_id) {
      ui->show_all_guilds[i] = ui->show_all_guilds[--ui->nshow_all_guilds];
      ui->built_rev = UINT64_MAX;
      ui->built_lists_rev = UINT64_MAX;
      ui_settings_save(ui);
      ui->dirty = true;
      return;
    }
  }
  if (ui->nshow_all_guilds < sizeof ui->show_all_guilds / sizeof ui->show_all_guilds[0]) {
    ui->show_all_guilds[ui->nshow_all_guilds++] = guild_id;
  }
  ui->built_rev = UINT64_MAX;
  ui->built_lists_rev = UINT64_MAX;
  ui_settings_save(ui);
  ui->dirty = true;
}

bool ui_guild_view_active(const ccord_ui *ui)
{
  if (!ui || !ui->d.store || ui->guild_view == UI_GUILD_VIEW_NONE || ui->discovery_open) return false;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  if (g == CCORD_ID_NONE || g == CCORD_DM_GUILD_ID) return false;
  return ui->guild_view_guild_id == g;
}

void ui_guild_view_open(ccord_ui *ui, ui_guild_view view)
{
  if (!ui || !ui->d.store || view == UI_GUILD_VIEW_NONE) return;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  if (g == CCORD_ID_NONE || g == CCORD_DM_GUILD_ID) return;
  if (ui->discovery_open) ui->discovery_open = false;
  ui->stage_channel = CCORD_ID_NONE;
  ui->stage_chat_open = false;
  ui->guild_view = view;
  ui->guild_view_guild_id = g;
  ui->guild_view_scroll = 0.0f;
  if (view == UI_GUILD_VIEW_CHANNELS_ROLES) {
    const ccord_onboarding *ob = ccord_store_onboarding(ui->d.store, g);
    ui->cr_tab = (ob && ob->prompt_count > 0) ? 0 : 1;
  }
  if (ui->mobile_layout) ui->left_drawer_open = false;
  // Fetch welcome screen & onboarding once per guild if not already in store.
  if (ui->d.session && ui->guild_view_req_guild != g) {
    ui->guild_view_req_guild = g;
    ccord_admin_request req;
    ccord_error err;
    if (!ccord_store_welcome_screen_loaded(ui->d.store, g) &&
        ccord_admin_get_welcome_screen(&req, g, &err) == CCORD_OK) {
      (void)ccord_session_admin(ui->d.session, &req, &err);
    }
    if (!ccord_store_onboarding_loaded(ui->d.store, g) &&
        ccord_admin_get_onboarding(&req, g, &err) == CCORD_OK) {
      (void)ccord_session_admin(ui->d.session, &req, &err);
    }
  }
  ui_compute_layout(ui);
  ui->dirty = true;
}

void ui_guild_view_close(ccord_ui *ui)
{
  if (!ui) return;
  ui->guild_view = UI_GUILD_VIEW_NONE;
  ui->guild_view_guild_id = CCORD_ID_NONE;
  ui->guild_view_scroll = 0.0f;
  ui_compute_layout(ui);
  ui->dirty = true;
}

static ccord_rect gv_body_rect(const ccord_ui *ui)
{
  const ccord_layout *l = &ui->layout;
  float y = l->chat_header.y + l->chat_header.h;
  float bottom = ui->mobile_layout ? (ui->win_h - fmaxf(ui->safe_insets.bottom, ui->keyboard_inset))
                                   : ui->win_h;
  float h = fmaxf(bottom - y, 0.0f);
  return (ccord_rect){l->message_area.x, y, l->message_area.w, h};
}

static void gv_draw_server_guide(ccord_ui *ui, ccord_renderer *r, const ccord_rect *view,
                                 const ccord_guild *gd, ccord_id g)
{
  uis_tokens tk = uis_tokens_of(ui);
  bool compact = ui->mobile_layout || view->w < 540.0f;
  float pad = compact ? 14.0f : 28.0f;
  float max_cw = fminf(view->w - 2.0f * pad, 760.0f);
  if (max_cw < 160.0f) max_cw = fmaxf(view->w - 16.0f, 120.0f);
  float cx = view->x + (view->w - max_cw) * 0.5f;
  float y0 = view->y + (compact ? 14.0f : 22.0f);
  float y = y0 - ui->guild_view_scroll;

  // Count online & total members known in store.
  size_t total_m = ccord_store_member_count(ui->d.store, g);
  size_t online_m = 0;
  for (size_t i = 0; i < total_m; i++) {
    const ccord_member *m = ccord_store_member_at(ui->d.store, g, i);
    const ccord_user *u = m ? ccord_store_user(ui->d.store, m->user_id) : nullptr;
    if (u && u->presence != CCORD_PRESENCE_OFFLINE) online_m++;
  }

  const ccord_welcome_screen *ws = ccord_store_welcome_screen(ui->d.store, g);
  const char *gname = (gd && gd->name && gd->name[0]) ? gd->name : "Server";
  const char *desc = (ws && ws->description && ws->description[0])
                       ? ws->description
                       : "Welcome! Explore key channels, server rules, and customize your channel list below.";

  // Hero welcome card
  float btn_h = uis_btn_h_for(ui, UIS_H_BTN);
  float hero_h = compact ? (116.0f + btn_h) : 132.0f;
  ccord_rect hero = {cx, y, max_cw, hero_h};
  ccord_draw_rrect(r, hero, tk.r_surface, COL(bg_secondary));
  ccord_draw_rrect_stroke(r, hero, tk.r_surface, 1.0f, COL(border_subtle));

  // Guild avatar / monogram
  float av_sz = compact ? 44.0f : 54.0f;
  ccord_rect av_r = {hero.x + 16.0f, hero.y + 16.0f, av_sz, av_sz};
  ccord_draw_rrect(r, av_r, tk.r_surface, COL(brand));
  char mono[4] = {gname[0] ? gname[0] : 'S', '\0', '\0', '\0'};
  ccord_text_style ms = ui_style(ui, CCORD_FONT_BOLD, compact ? 18.0f : 22.0f, COL(text_on_brand));
  ccord_vec2 mt = ccord_text_measure(r, mono, strlen(mono), &ms, 0.0f);
  ccord_text_draw(r, mono, strlen(mono), av_r.x + (av_r.w - mt.x) * 0.5f, av_r.y + (av_r.h - mt.y) * 0.5f, &ms, 0.0f);

  float tx = av_r.x + av_r.w + 14.0f;
  float right_res = compact ? 16.0f : 176.0f;
  float tw = fmaxf(hero.x + hero.w - right_res - tx, 80.0f);
  char title[160];
  snprintf(title, sizeof title, "Welcome to %s", gname);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, compact ? M(font_size_base) + 1.0f : M(font_size_large) + 2.0f,
                                 COL(text_header));
  ccord_text_draw_ellipsis(r, title, strlen(title), tx, hero.y + 16.0f, tw, &ts);

  char stats[96];
  snprintf(stats, sizeof stats, "%zu Online  \xC2\xB7  %zu Members", online_m, total_m);
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_muted));
  ccord_text_draw_ellipsis(r, stats, strlen(stats), tx, hero.y + 16.0f + ui_line_h(ui, ts.size) + 2.0f, tw, &ss);

  ccord_text_style ds = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_body, COL(text_secondary));
  ccord_text_draw(r, desc, strlen(desc), hero.x + 16.0f, av_r.y + av_r.h + 10.0f, &ds, hero.w - 32.0f);

  bool has_vanity = gd && gd->vanity_url_code[0] != '\0';
  const char *cta_lbl = has_vanity ? "Copy Invite" : "Channels & Roles";
  ccord_rect cta = compact
                     ? (ccord_rect){hero.x + 16.0f, hero.y + hero.h - btn_h - 12.0f, hero.w - 32.0f, btn_h}
                     : (ccord_rect){hero.x + hero.w - 164.0f, hero.y + 18.0f, 148.0f, btn_h};
  bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cta);
  uis_button(ui, r, cta, cta_lbl, UIS_BTN_PRIMARY, ui->mobile_layout ? UIS_BTN_S_MD : UIS_BTN_S_SM,
             (uis_state){.hover = chv}, "gvcta", 0);
  if (cta.y + cta.h >= view->y && cta.y <= view->y + view->h) {
    gv_push_hit(cta, has_vanity ? GV_HIT_COPY_VANITY : GV_HIT_OPEN_CR, 0, g, 0, false);
  }
  y += hero_h + 22.0f;

  // Resources section (welcome_screen channels + rules channel + fallback top channels)
  ccord_text_style ov = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
  ccord_text_draw(r, "RESOURCES & FEATURED CHANNELS", 29, cx, y, &ov, 0.0f);
  y += 22.0f;

  ccord_id shown[12];
  size_t nshown = 0;
  float card_h = ui->mobile_layout ? 64.0f : 58.0f;

  if (ws && ws->channel_count > 0) {
    for (size_t i = 0; i < ws->channel_count && nshown < 12; i++) {
      const ccord_welcome_channel *wc = &ws->channels[i];
      const ccord_channel *ch = ccord_store_channel(ui->d.store, wc->channel_id);
      if (!ch) continue;
      shown[nshown++] = ch->id;
      ccord_rect card = {cx, y, max_cw, card_h};
      bool hv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, card) && ui_in_rect(ui->mx, ui->my, *view);
      ccord_draw_rrect(r, card, tk.r_surface, hv ? COL(bg_item_hover) : COL(bg_secondary));
      ccord_draw_rrect_stroke(r, card, tk.r_surface, 1.0f, COL(border_subtle));
      ui_icon_hash(r, card.x + 22.0f, card.y + card.h * 0.5f, 18.0f, COL(text_muted));
      ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
      ccord_text_style sub = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_secondary));
      const char *cname = (ch->name && ch->name[0]) ? ch->name : "channel";
      const char *cdesc = (wc->description && wc->description[0]) ? wc->description : (ch->topic ? ch->topic : "");
      float text_x = card.x + 42.0f;
      float text_w = fmaxf(card.w - 76.0f, 60.0f);
      ccord_text_draw_ellipsis(r, cname, strlen(cname), text_x, card.y + 10.0f, text_w, &ns);
      if (cdesc[0]) ccord_text_draw_ellipsis(r, cdesc, strlen(cdesc), text_x, card.y + 32.0f, text_w, &sub);
      ui_icon_chevron(r, card.x + card.w - 20.0f, card.y + card.h * 0.5f, 10.0f, ICON_RIGHT, COL(text_muted));
      if (card.y + card.h >= view->y && card.y <= view->y + view->h) {
        gv_push_hit(card, GV_HIT_CHANNEL_JUMP, 0, ch->id, 0, false);
      }
      y += card_h + 8.0f;
    }
  }

  // Include rules channel if not already listed
  if (gd && gd->rules_channel_id != CCORD_ID_NONE && nshown < 12) {
    bool dup = false;
    for (size_t k = 0; k < nshown; k++) if (shown[k] == gd->rules_channel_id) dup = true;
    const ccord_channel *rch = !dup ? ccord_store_channel(ui->d.store, gd->rules_channel_id) : nullptr;
    if (rch) {
      shown[nshown++] = rch->id;
      ccord_rect card = {cx, y, max_cw, card_h};
      bool hv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, card) && ui_in_rect(ui->mx, ui->my, *view);
      ccord_draw_rrect(r, card, tk.r_surface, hv ? COL(bg_item_hover) : COL(bg_secondary));
      ccord_draw_rrect_stroke(r, card, tk.r_surface, 1.0f, COL(border_subtle));
      ui_icon_hash(r, card.x + 22.0f, card.y + card.h * 0.5f, 18.0f, COL(brand));
      ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
      ccord_text_style sub = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_secondary));
      float text_x = card.x + 42.0f;
      float text_w = fmaxf(card.w - 76.0f, 60.0f);
      ccord_text_draw_ellipsis(r, "Server Rules", 12, text_x, card.y + 10.0f, text_w, &ns);
      const char *rn = (rch->name && rch->name[0]) ? rch->name : "rules";
      char rbuf[96];
      snprintf(rbuf, sizeof rbuf, "Read the community guidelines in #%s", rn);
      ccord_text_draw_ellipsis(r, rbuf, strlen(rbuf), text_x, card.y + 32.0f, text_w, &sub);
      ui_icon_chevron(r, card.x + card.w - 20.0f, card.y + card.h * 0.5f, 10.0f, ICON_RIGHT, COL(text_muted));
      if (card.y + card.h >= view->y && card.y <= view->y + view->h) {
        gv_push_hit(card, GV_HIT_CHANNEL_JUMP, 0, rch->id, 0, false);
      }
      y += card_h + 8.0f;
    }
  }

  // Fallback: top visible text channels when server has no custom welcome channels yet
  if (nshown == 0) {
    size_t nch = ccord_store_channel_count(ui->d.store, g);
    for (size_t i = 0; i < nch && nshown < 5; i++) {
      const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, i);
      if (!ch || ch->kind != CCORD_CHANNEL_TEXT || ccord_discord_type_is_thread(ch->discord_type)) continue;
      shown[nshown++] = ch->id;
      ccord_rect card = {cx, y, max_cw, card_h};
      bool hv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, card) && ui_in_rect(ui->mx, ui->my, *view);
      ccord_draw_rrect(r, card, tk.r_surface, hv ? COL(bg_item_hover) : COL(bg_secondary));
      ccord_draw_rrect_stroke(r, card, tk.r_surface, 1.0f, COL(border_subtle));
      ui_icon_hash(r, card.x + 22.0f, card.y + card.h * 0.5f, 18.0f, COL(text_muted));
      ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
      ccord_text_style sub = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_secondary));
      float text_x = card.x + 42.0f;
      float text_w = fmaxf(card.w - 76.0f, 60.0f);
      const char *cname = (ch->name && ch->name[0]) ? ch->name : "channel";
      const char *cdesc = (ch->topic && ch->topic[0]) ? ch->topic : "Jump straight into the conversation";
      ccord_text_draw_ellipsis(r, cname, strlen(cname), text_x, card.y + 10.0f, text_w, &ns);
      ccord_text_draw_ellipsis(r, cdesc, strlen(cdesc), text_x, card.y + 32.0f, text_w, &sub);
      ui_icon_chevron(r, card.x + card.w - 20.0f, card.y + card.h * 0.5f, 10.0f, ICON_RIGHT, COL(text_muted));
      if (card.y + card.h >= view->y && card.y <= view->y + view->h) {
        gv_push_hit(card, GV_HIT_CHANNEL_JUMP, 0, ch->id, 0, false);
      }
      y += card_h + 8.0f;
    }
  }

  y += 12.0f;
  // Quick card to open Channels & Roles
  ccord_rect cr_card = {cx, y, max_cw, card_h};
  bool crhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cr_card) && ui_in_rect(ui->mx, ui->my, *view);
  ccord_draw_rrect(r, cr_card, tk.r_surface, crhv ? COL(bg_item_hover) : COL(bg_secondary));
  ccord_draw_rrect_stroke(r, cr_card, tk.r_surface, 1.0f, COL(border_subtle));
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
  ccord_text_style sub = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_secondary));
  ccord_text_draw_ellipsis(r, "Channels & Roles", 16, cr_card.x + 16.0f, cr_card.y + 10.0f, cr_card.w - 50.0f, &ns);
  ccord_text_draw_ellipsis(r, "Choose which channels appear in your sidebar and pick server roles.", 67,
                           cr_card.x + 16.0f, cr_card.y + 32.0f, cr_card.w - 50.0f, &sub);
  ui_icon_chevron(r, cr_card.x + cr_card.w - 20.0f, cr_card.y + cr_card.h * 0.5f, 10.0f, ICON_RIGHT, COL(text_muted));
  if (cr_card.y + cr_card.h >= view->y && cr_card.y <= view->y + view->h) {
    gv_push_hit(cr_card, GV_HIT_OPEN_CR, 0, g, 0, false);
  }
  y += card_h + 24.0f;

  ui->guild_view_content_h = (y + ui->guild_view_scroll) - view->y;
}

static void gv_draw_channels_roles(ccord_ui *ui, ccord_renderer *r, const ccord_rect *view, ccord_id g)
{
  uis_tokens tk = uis_tokens_of(ui);
  bool compact = ui->mobile_layout || view->w < 540.0f;
  float pad = compact ? 14.0f : 28.0f;
  float max_cw = fminf(view->w - 2.0f * pad, 760.0f);
  if (max_cw < 160.0f) max_cw = fmaxf(view->w - 16.0f, 120.0f);
  float cx = view->x + (view->w - max_cw) * 0.5f;
  float y = view->y + (compact ? 12.0f : 18.0f) - ui->guild_view_scroll;

  // Two-tab switcher: Customize | Browse Channels
  float tab_h = ui->mobile_layout ? UIS_MOB_H_BTN_SM : 32.0f;
  float tab_w = compact ? (max_cw - 8.0f) * 0.5f : 160.0f;
  static const char *const k_tabs[2] = {"Customize", "Browse Channels"};
  for (int t = 0; t < 2; t++) {
    ccord_rect tr = {cx + (float)t * (tab_w + 8.0f), y, tab_w, tab_h};
    bool hv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, tr) && ui_in_rect(ui->mx, ui->my, *view);
    uis_chip(ui, r, tr, k_tabs[t], (uis_state){.hover = hv, .selected = ui->cr_tab == t}, "crtab", (uint64_t)t);
    if (tr.y + tr.h >= view->y && tr.y <= view->y + view->h) {
      gv_push_hit(tr, GV_HIT_TAB, t, 0, 0, false);
    }
  }
  y += tab_h + 14.0f;
  uis_divider(ui, r, cx, y, max_cw);
  y += 14.0f;

  if (ui->cr_tab == 0) {
    // Customize tab: onboarding prompts & option chips
    const ccord_onboarding *ob = ccord_store_onboarding(ui->d.store, g);
    if (!ob || ob->prompt_count == 0) {
      float card_h = 116.0f;
      ccord_rect empty = {cx, y, max_cw, card_h};
      ccord_draw_rrect(r, empty, tk.r_surface, COL(bg_secondary));
      ccord_draw_rrect_stroke(r, empty, tk.r_surface, 1.0f, COL(border_subtle));
      ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
      ccord_text_style bs = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_secondary));
      ccord_text_draw(r, "No onboarding questions configured", 34, empty.x + 16.0f, empty.y + 16.0f, &hs, empty.w - 32.0f);
      ccord_text_draw(r, "Switch to Browse Channels to choose which server channels show in your sidebar.", 79,
                      empty.x + 16.0f, empty.y + 40.0f, &bs, empty.w - 32.0f);
      float bh = uis_btn_h_for(ui, UIS_H_BTN_SM);
      ccord_rect go = {empty.x + 16.0f, empty.y + empty.h - bh - 14.0f, 160.0f, bh};
      bool ghv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, go) && ui_in_rect(ui->mx, ui->my, *view);
      uis_button(ui, r, go, "Browse Channels", UIS_BTN_PRIMARY, ui->mobile_layout ? UIS_BTN_S_MD : UIS_BTN_S_SM,
                 (uis_state){.hover = ghv}, "crbrowse", 0);
      if (go.y + go.h >= view->y && go.y <= view->y + view->h) gv_push_hit(go, GV_HIT_TAB, 1, 0, 0, false);
      y += card_h + 24.0f;
    } else {
      float chip_h = ui->mobile_layout ? UIS_MOB_H_BTN : 36.0f;
      for (size_t pi = 0; pi < ob->prompt_count; pi++) {
        const ccord_onboarding_prompt *pr = &ob->prompts[pi];
        ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
        const char *ptitle = (pr->title && pr->title[0]) ? pr->title : "Server Question";
        ccord_text_draw_ellipsis(r, ptitle, strlen(ptitle), cx, y, max_cw - 90.0f, &ps);
        const char *badge = pr->required ? "Required" : (pr->single_select ? "Pick one" : "Pick any");
        ccord_text_style bgs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
        ccord_vec2 bgt = ccord_text_measure(r, badge, strlen(badge), &bgs, 0.0f);
        ccord_text_draw(r, badge, strlen(badge), cx + max_cw - bgt.x, y + 3.0f, &bgs, 0.0f);
        y += ui_line_h(ui, ps.size) + 10.0f;

        float ox = cx;
        for (size_t oi = 0; oi < pr->option_count; oi++) {
          const ccord_onboarding_option *opt = &pr->options[oi];
          const char *ot = (opt->title && opt->title[0]) ? opt->title : "Option";
          char lbl[140];
          if (opt->emoji_name && opt->emoji_name[0] && opt->emoji_id == CCORD_ID_NONE) {
            snprintf(lbl, sizeof lbl, "%s %s", opt->emoji_name, ot);
          } else {
            snprintf(lbl, sizeof lbl, "%s", ot);
          }
          ccord_text_style os = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, COL(text_header));
          ccord_vec2 omt = ccord_text_measure(r, lbl, strlen(lbl), &os, 0.0f);
          float ow = fminf(omt.x + 28.0f, max_cw);
          if (ox > cx && ox + ow > cx + max_cw) {
            ox = cx;
            y += chip_h + 8.0f;
          }
          ccord_rect or_ = {ox, y, ow, chip_h};
          bool sel = gv_opt_selected(ui, opt->id);
          bool ohv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, or_) && ui_in_rect(ui->mx, ui->my, *view);
          uis_chip(ui, r, or_, lbl, (uis_state){.hover = ohv, .selected = sel}, "obopt", opt->id);
          if (or_.y + or_.h >= view->y && or_.y <= view->y + view->h) {
            gv_push_hit(or_, GV_HIT_ONBOARD_OPT, 0, opt->id, pr->id, pr->single_select);
          }
          ox += ow + 8.0f;
        }
        y += chip_h + 20.0f;
      }
    }
  } else {
    // Browse Channels tab
    size_t nch = ccord_store_channel_count(ui->d.store, g);
    size_t total_ch = 0, shown_ch = 0;
    bool show_all = ui_guild_show_all_channels(ui, g);
    for (size_t i = 0; i < nch; i++) {
      const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, i);
      if (!ch || ch->kind == CCORD_CHANNEL_CATEGORY || ccord_discord_type_is_thread(ch->discord_type)) continue;
      total_ch++;
      if (show_all || !ui_channel_is_hidden(ui, ch->id)) shown_ch++;
    }

    float bar_h = ui->mobile_layout ? UIS_MOB_TOUCH_H : 36.0f;
    ccord_rect bar = {cx, y, max_cw, bar_h};
    ccord_draw_rrect(r, bar, tk.r_surface, COL(bg_secondary));
    char cnt[96];
    snprintf(cnt, sizeof cnt, "Showing %zu of %zu channels", shown_ch, total_ch);
    ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, COL(text_secondary));
    ccord_text_draw_ellipsis(r, cnt, strlen(cnt), bar.x + 12.0f, bar.y + (bar.h - ui_line_h(ui, cs.size)) * 0.5f,
                             fmaxf(bar.w - 175.0f, 60.0f), &cs);

    float sw_w = 154.0f;
    float sw_h = ui->mobile_layout ? UIS_MOB_H_CHIP : 26.0f;
    ccord_rect sw_r = {bar.x + bar.w - sw_w - 8.0f, bar.y + (bar.h - sw_h) * 0.5f, sw_w, sw_h};
    bool shv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, sw_r) && ui_in_rect(ui->mx, ui->my, *view);
    uis_chip(ui, r, sw_r, show_all ? "Showing All" : "Show All Channels",
             (uis_state){.hover = shv, .selected = show_all}, "crall", g);
    if (sw_r.y + sw_r.h >= view->y && sw_r.y <= view->y + view->h) {
      gv_push_hit(sw_r, GV_HIT_SHOW_ALL, 0, g, 0, false);
    }
    y += bar_h + 14.0f;

    float row_h = ui->mobile_layout ? 56.0f : 48.0f;
    float chk_w = ui->mobile_layout ? 92.0f : 84.0f;
    float chk_h = ui->mobile_layout ? UIS_MOB_H_CHIP : 26.0f;

    // Pass 1: uncategorized channels, Pass 2: each category and its child channels
    for (int pass = 0; pass < 2; pass++) {
      for (size_t ci = 0; ci < (pass == 0 ? 1u : nch); ci++) {
        ccord_id cat_id = CCORD_ID_NONE;
        if (pass == 1) {
          const ccord_channel *cat = ccord_store_channel_at(ui->d.store, g, ci);
          if (!cat || cat->kind != CCORD_CHANNEL_CATEGORY) continue;
          cat_id = cat->id;
          // Check if category has any non-thread child channels
          bool any = false;
          for (size_t j = 0; j < nch; j++) {
            const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, j);
            if (ch && ch->kind != CCORD_CHANNEL_CATEGORY && !ccord_discord_type_is_thread(ch->discord_type) &&
                ch->parent_id == cat_id) { any = true; break; }
          }
          if (!any) continue;
          ccord_text_style ov = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
          const char *catn = (cat->name && cat->name[0]) ? cat->name : "CATEGORY";
          ccord_text_draw_ellipsis(r, catn, strlen(catn), cx + 4.0f, y + 6.0f, max_cw - 8.0f, &ov);
          y += 24.0f;
        }
        for (size_t j = 0; j < nch; j++) {
          const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, j);
          if (!ch || ch->kind == CCORD_CHANNEL_CATEGORY || ccord_discord_type_is_thread(ch->discord_type)) continue;
          if (pass == 0) {
            bool has_cat = ch->parent_id != CCORD_ID_NONE &&
                           ccord_store_channel(ui->d.store, ch->parent_id) != nullptr &&
                           ccord_store_channel(ui->d.store, ch->parent_id)->kind == CCORD_CHANNEL_CATEGORY;
            if (has_cat) continue;
          } else {
            if (ch->parent_id != cat_id) continue;
          }

          ccord_rect rr = {cx, y, max_cw, row_h};
          if (rr.y + rr.h >= view->y && rr.y <= view->y + view->h) {
            bool hidden = ui_channel_is_hidden(ui, ch->id);
            ui_voice_lock_kind vlock_kind = UI_VOICE_LOCK_NONE;
            const char *vlock = ui_voice_lock_reason_ex(ui, g, ch, &vlock_kind);
            bool voice_locked = (vlock != nullptr);
            bool rhv = !voice_locked && ui->mouse_in && ui_in_rect(ui->mx, ui->my, rr) && ui_in_rect(ui->mx, ui->my, *view);
            ccord_color row_bg = rhv ? COL(bg_item_hover) : COL(bg_secondary);
            ccord_draw_rrect(r, rr, tk.r_surface, row_bg);
            ccord_draw_rrect_stroke(r, rr, tk.r_surface, 1.0f, COL(border_subtle));
            float icx = rr.x + 20.0f, icy = rr.y + rr.h * 0.5f;
            ccord_color dim_c = ui_mix(COL(text_muted), COL(bg_secondary), 0.45f);
            if (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE) {
              if (vlock_kind == UI_VOICE_LOCK_PERMISSION) ui_icon_speaker_locked(r, icx, icy, 16.0f, dim_c, row_bg);
              else ui_icon_speaker(r, icx, icy, 16.0f, voice_locked ? dim_c : COL(text_muted));
            } else {
              ui_icon_hash(r, icx, icy, 16.0f, COL(text_muted));
            }
            ccord_rect tog_r = {rr.x + rr.w - chk_w - 10.0f, rr.y + (rr.h - chk_h) * 0.5f, chk_w, chk_h};
            float text_x = rr.x + 38.0f;
            float text_w = fmaxf(tog_r.x - text_x - 10.0f, 50.0f);
            ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body,
                                           voice_locked ? dim_c : (hidden ? COL(text_muted) : COL(text_header)));
            const char *cname = (ch->name && ch->name[0]) ? ch->name : "channel";
            const char *sub_txt = (ch->topic && ch->topic[0]) ? ch->topic : vlock;
            bool has_sub = sub_txt && sub_txt[0];
            ccord_text_draw_ellipsis(r, cname, strlen(cname), text_x,
                                     has_sub ? rr.y + 8.0f : rr.y + (rr.h - ui_line_h(ui, ns.size)) * 0.5f,
                                     text_w, &ns);
            if (has_sub) {
              ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, voice_locked ? dim_c : COL(text_muted));
              ccord_text_draw_ellipsis(r, sub_txt, strlen(sub_txt), text_x, rr.y + 28.0f, text_w, &ts);
            }
            bool thv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, tog_r) && ui_in_rect(ui->mx, ui->my, *view);
            uis_chip(ui, r, tog_r, hidden ? "Hidden" : "Shown",
                     (uis_state){.hover = thv, .selected = !hidden}, "crch", ch->id);
            gv_push_hit(tog_r, GV_HIT_CHANNEL_TOGGLE, 0, ch->id, 0, false);
            ccord_rect jump_r = {rr.x, rr.y, tog_r.x - rr.x - 4.0f, rr.h};
            gv_push_hit(jump_r, GV_HIT_CHANNEL_JUMP, 0, ch->id, 0, false);
          }
          y += row_h + 6.0f;
        }
      }
    }
    y += 18.0f;
  }

  ui->guild_view_content_h = (y + ui->guild_view_scroll) - view->y;
}

void ui_draw_guild_view(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui_guild_view_active(ui)) return;
  ccord_rect view = gv_body_rect(ui);
  g_gv.owner = ui;
  g_gv.nhits = 0;
  g_gv.view_rect = view;

  ccord_draw_rect(r, view, COL(bg_primary));
  float max_s = fmaxf(ui->guild_view_content_h - view.h, 0.0f);
  if (ui->guild_view_scroll < 0.0f) ui->guild_view_scroll = 0.0f;
  if (ui->guild_view_scroll > max_s) ui->guild_view_scroll = max_s;

  ccord_id g = ccord_store_selected_guild(ui->d.store);
  const ccord_guild *gd = ccord_store_guild(ui->d.store, g);
  ccord_push_clip(r, view);
  if (ui->guild_view == UI_GUILD_VIEW_SERVER_GUIDE) {
    gv_draw_server_guide(ui, r, &view, gd, g);
  } else {
    gv_draw_channels_roles(ui, r, &view, g);
  }
  ccord_pop_clip(r);
}

static void gv_apply_onboarding_option(ccord_ui *ui, ccord_id g, ccord_id prompt_id, ccord_id opt_id, bool single_select)
{
  const ccord_onboarding *ob = ccord_store_onboarding(ui->d.store, g);
  const ccord_onboarding_prompt *pr = nullptr;
  const ccord_onboarding_option *opt = nullptr;
  if (ob) {
    for (size_t i = 0; i < ob->prompt_count; i++) {
      if (ob->prompts[i].id == prompt_id) {
        pr = &ob->prompts[i];
        for (size_t j = 0; j < pr->option_count; j++) {
          if (pr->options[j].id == opt_id) { opt = &pr->options[j]; break; }
        }
        break;
      }
    }
  }
  bool next = !gv_opt_selected(ui, opt_id);
  if (next && single_select && pr) {
    for (size_t j = 0; j < pr->option_count; j++) {
      if (pr->options[j].id != opt_id) (void)gv_opt_set(ui, pr->options[j].id, false);
    }
  }
  (void)gv_opt_set(ui, opt_id, next);

  if (opt) {
    for (size_t k = 0; k < opt->channel_count; k++) {
      gv_channel_set_hidden(ui, opt->channel_ids[k], !next);
    }
    if (ui->d.session && opt->role_count > 0) {
      ccord_id self_id = ccord_store_self_id(ui->d.store);
      if (ccord_store_has_permission(ui->d.store, g, CCORD_ID_NONE, self_id, CCORD_PERM_MANAGE_ROLES)) {
        for (size_t k = 0; k < opt->role_count; k++) {
          ccord_admin_request req;
          ccord_error err;
          ccord_status st = next ? ccord_admin_add_member_role(&req, g, self_id, opt->role_ids[k], nullptr, &err)
                                 : ccord_admin_remove_member_role(&req, g, self_id, opt->role_ids[k], nullptr, &err);
          if (st == CCORD_OK) (void)ccord_session_admin(ui->d.session, &req, &err);
        }
      }
    }
    ui_notice(ui, "%s: %s", next ? "Selected" : "Deselected",
              (opt->title && opt->title[0]) ? opt->title : "option");
  }
  ui->built_rev = UINT64_MAX;
  ui->built_lists_rev = UINT64_MAX;
  ui_settings_save(ui);
  ui->dirty = true;
}

bool ui_guild_view_handle_mouse(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui_guild_view_active(ui) || !ev || g_gv.owner != ui) return false;
  if (!ui_in_rect(ev->x, ev->y, g_gv.view_rect)) return false;
  for (size_t i = 0; i < g_gv.nhits; i++) {
    const gv_hit *h = &g_gv.hits[i];
    if (!ui_in_rect(ev->x, ev->y, h->rect)) continue;
    ccord_id g = ccord_store_selected_guild(ui->d.store);
    switch (h->kind) {
      case GV_HIT_TAB:
        ui->cr_tab = h->tab;
        ui->guild_view_scroll = 0.0f;
        ui->dirty = true;
        return true;
      case GV_HIT_OPEN_CR:
        ui_guild_view_open(ui, UI_GUILD_VIEW_CHANNELS_ROLES);
        return true;
      case GV_HIT_COPY_VANITY: {
        const ccord_guild *gd = ccord_store_guild(ui->d.store, g);
        if (gd && gd->vanity_url_code[0]) {
          char url[128];
          snprintf(url, sizeof url, "https://discord.gg/%s", gd->vanity_url_code);
          if (ui->d.host.clipboard_set) ui->d.host.clipboard_set(ui->d.host.ud, url);
          ui_notice(ui, "Copied invite link");
        }
        return true;
      }
      case GV_HIT_SHOW_ALL:
        ui_guild_toggle_show_all_channels(ui, g);
        return true;
      case GV_HIT_CHANNEL_TOGGLE:
        ui_channel_toggle_hidden(ui, h->id);
        return true;
      case GV_HIT_CHANNEL_JUMP: {
        const ccord_channel *jch = ccord_store_channel(ui->d.store, h->id);
        if (jch && (jch->kind == CCORD_CHANNEL_VOICE || jch->kind == CCORD_CHANNEL_STAGE)) {
          const char *vlock = ui_voice_lock_reason(ui, g, jch);
          if (vlock) {
            ui_notice(ui, "%s", vlock);
            ui->dirty = true;
            return true;
          }
          ui_guild_view_close(ui);
          const ccord_voice_state *self = ui_self_voice(ui);
          if (self && self->channel_id == jch->id && !ui_role_preview_active_for(ui, g)) {
            ui->stage_channel = jch->id;
            ui->stage_chat_open = false;
            ui->stage_armed = true;
          } else {
            uc_join_voice_channel(ui, g, jch);
          }
          ui->dirty = true;
          return true;
        }
        ccord_error err;
        ui_guild_view_close(ui);
        if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, h->id, &err);
        else (void)ccord_store_select_channel(ui->d.store, h->id, &err);
        ui_invalidate_rows(ui);
        ui_ensure_caches(ui);
        ui_sync_messages(ui);
        ui_save_current_location(ui);
        ui->dirty = true;
        return true;
      }
      case GV_HIT_ONBOARD_OPT:
        gv_apply_onboarding_option(ui, g, h->prompt_id, h->id, h->single_select);
        return true;
      case GV_HIT_NONE:
        break;
    }
  }
  return true;
}

bool ui_guild_view_handle_wheel(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui_guild_view_active(ui) || !ev || g_gv.owner != ui) return false;
  if (!ui_in_rect(ev->x, ev->y, g_gv.view_rect)) return false;
  float max_s = fmaxf(ui->guild_view_content_h - g_gv.view_rect.h, 0.0f);
  ui_scroll_offset_by(ui, &ui->guild_view_scroll, ui_wheel_delta_px(ui, ev, 44.0f), max_s);
  ui->dirty = true;
  return true;
}
