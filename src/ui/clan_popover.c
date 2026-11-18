// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Clickable Server Tag / Clan Info Popover (Issue 4), hooked through profile_roles.c at the top overlay layer.
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>

#include "ui_internal.h"
#include "ui_style.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define CP_MAX_HITS 64u
#define CP_CARD_W 280.0f
#define CP_BANNER_H 56.0f
#define CP_ICON_SZ 44.0f

typedef struct cp_hit {
  ccord_rect rect;
  ccord_id guild_id;
  char tag[16];
  char badge_url[256];
} cp_hit;

struct ui_clan_popover {
  cp_hit hits[CP_MAX_HITS];
  size_t nhits;

  bool open;
  bool swallow_up;
  ccord_id guild_id;
  ccord_rect anchor;
  char tag[16];
  char badge_url[256];

  ccord_rect box;
  ccord_rect go_btn;
};

static struct ui_clan_popover *cp_get(ccord_ui *ui, bool create)
{
  if (!ui) return nullptr;
  if (!ui->clan_pop && create) {
    ui->clan_pop = ccord_calloc(1, sizeof *ui->clan_pop);
  }
  return ui->clan_pop;
}

void ui_clan_popover_destroy(ccord_ui *ui)
{
  if (!ui || !ui->clan_pop) return;
  ccord_free(ui->clan_pop);
  ui->clan_pop = nullptr;
}

void ui_clan_popover_begin_frame(ccord_ui *ui)
{
  struct ui_clan_popover *cp = ui ? ui->clan_pop : nullptr;
  if (!cp) return;
  cp->nhits = 0;
}

void ui_clan_popover_record_hit(ccord_ui *ui, ccord_rect rect, ccord_id guild_id, const char *tag, const char *badge_url)
{
  if (!ui || !tag || !tag[0] || rect.w <= 0.0f || rect.h <= 0.0f) return;
  struct ui_clan_popover *cp = cp_get(ui, true);
  if (!cp || cp->nhits >= CP_MAX_HITS) return;
  cp_hit *h = &cp->hits[cp->nhits++];
  h->rect = rect;
  h->guild_id = guild_id;
  snprintf(h->tag, sizeof h->tag, "%s", tag);
  snprintf(h->badge_url, sizeof h->badge_url, "%s", badge_url ? badge_url : "");
}

bool ui_clan_popover_is_open(const ccord_ui *ui)
{
  return ui && ui->clan_pop && ui->clan_pop->open;
}

ccord_rect ui_clan_popover_box(const ccord_ui *ui)
{
  if (!ui_clan_popover_is_open(ui)) return (ccord_rect){0};
  return ui->clan_pop->box;
}

ccord_id ui_clan_popover_guild_id(const ccord_ui *ui)
{
  if (!ui_clan_popover_is_open(ui)) return CCORD_ID_NONE;
  return ui->clan_pop->guild_id;
}

bool ui_clan_popover_hit_rect(const ccord_ui *ui, const char *tag, ccord_rect *out)
{
  if (!ui || !ui->clan_pop || !tag) return false;
  for (size_t i = ui->clan_pop->nhits; i > 0; i--) {
    const cp_hit *h = &ui->clan_pop->hits[i - 1u];
    if (strcmp(h->tag, tag) == 0) {
      if (out) *out = h->rect;
      return true;
    }
  }
  return false;
}

static void cp_close(ccord_ui *ui)
{
  struct ui_clan_popover *cp = ui ? ui->clan_pop : nullptr;
  if (!cp || !cp->open) return;
  cp->open = false;
  cp->box = (ccord_rect){0};
  cp->go_btn = (ccord_rect){0};
  ui->dirty = true;
}

static void cp_open_hit(ccord_ui *ui, const cp_hit *h)
{
  struct ui_clan_popover *cp = cp_get(ui, true);
  if (!cp || !h) return;
  cp->open = true;
  cp->guild_id = h->guild_id;
  cp->anchor = h->rect;
  snprintf(cp->tag, sizeof cp->tag, "%s", h->tag);
  snprintf(cp->badge_url, sizeof cp->badge_url, "%s", h->badge_url);
  cp->box = (ccord_rect){0};
  cp->go_btn = (ccord_rect){0};
  if (ui->d.session && h->guild_id != CCORD_ID_NONE) {
    ccord_error err;
    (void)ccord_session_guild_profile_fetch(ui->d.session, h->guild_id, &err);
  }
  ui->dirty = true;
}

bool ui_clan_popover_input(ccord_ui *ui, const ccord_input_event *ev)
{
  struct ui_clan_popover *cp = ui ? ui->clan_pop : nullptr;
  if (!cp || !ev) return false;

  if (ev->type == CCORD_IN_MOUSE_UP && ev->button == 1 && cp->swallow_up) {
    cp->swallow_up = false;
    return true;
  }

  if (cp->open) {
    if (ev->type == CCORD_IN_KEY_DOWN && ev->key == CCORD_KEY_ESCAPE) {
      cp_close(ui);
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
      cp->swallow_up = true;
      if (cp->go_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, cp->go_btn)) {
        ccord_id gid = cp->guild_id;
        cp_close(ui);
        ui->member_popover_open = false;
        if (ui->modal_kind == UI_MODAL_USER_PROFILE) {
          ui->modal_kind = UI_MODAL_NONE;
          ui->modal_target_id = CCORD_ID_NONE;
        }
        if (ui->d.session && gid != CCORD_ID_NONE) {
          ccord_error err;
          (void)ccord_session_select_guild(ui->d.session, gid, &err);
        }
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, cp->box)) {
        return true;
      }
      cp_close(ui);
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_MOVE && ui_in_rect(ev->x, ev->y, cp->box)) {
      return false;
    }
    return false;
  }

  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (ui->settings_open || ui_server_settings_is_open(ui) || ui_channel_settings_is_open(ui) ||
        ui->lightbox_open || ui->quick_switcher_open || ui->shortcuts_open || ui->go_live_open ||
        ui->menu.open || ui->inbox_open || ui->saved_popover_open || ui->pins_open ||
        ui->reactions_open || (ui->modal_kind != UI_MODAL_NONE && ui->modal_kind != UI_MODAL_USER_PROFILE)) {
      return false;
    }
    for (size_t i = cp->nhits; i > 0; i--) {
      const cp_hit *h = &cp->hits[i - 1u];
      if (ui_in_rect(ev->x, ev->y, h->rect)) {
        cp->swallow_up = true;
        cp_open_hit(ui, h);
        return true;
      }
    }
  }
  return false;
}

static void guild_initials(const char *name, char out[4])
{
  out[0] = '\0';
  if (!name || !name[0]) {
    snprintf(out, 4, "?");
    return;
  }
  size_t k = 0;
  bool word = true;
  for (size_t i = 0; name[i] && k < 2; i++) {
    unsigned char ch = (unsigned char)name[i];
    if (ch == ' ' || ch == '-' || ch == '_') {
      word = true;
      continue;
    }
    if (word && ch < 0x80) {
      out[k++] = (char)ch;
      word = false;
    }
  }
  if (k == 0) {
    out[k++] = name[0];
  }
  out[k] = '\0';
}

void ui_clan_popover_draw(ccord_ui *ui, ccord_renderer *r)
{
  struct ui_clan_popover *cp = ui ? ui->clan_pop : nullptr;
  if (!cp || !cp->open || !r) return;

  const ccord_guild_profile *gp =
    (ui->d.session && cp->guild_id != CCORD_ID_NONE) ? ccord_session_guild_profile(ui->d.session, cp->guild_id) : nullptr;
  const ccord_guild *lg =
    (ui->d.store && cp->guild_id != CCORD_ID_NONE) ? ccord_store_guild(ui->d.store, cp->guild_id) : nullptr;
  bool loading = ui->d.session && cp->guild_id != CCORD_ID_NONE &&
                 ccord_session_guild_profile_loading(ui->d.session, cp->guild_id);

  const char *name = (gp && gp->name[0]) ? gp->name
                     : ((lg && lg->name && lg->name[0]) ? lg->name
                        : (loading ? "Loading server..."
                           : ((gp && !gp->unavailable && cp->tag[0]) ? cp->tag : "Private Server")));
  const char *icon_url = (gp && gp->icon_url[0]) ? gp->icon_url
                         : ((lg && lg->icon_url && lg->icon_url[0]) ? lg->icon_url : "");
  const char *banner_url = (gp && gp->banner_url[0]) ? gp->banner_url : "";
  const char *tag = (gp && gp->tag[0]) ? gp->tag : cp->tag;
  const char *badge_url = (gp && gp->badge_url[0]) ? gp->badge_url : cp->badge_url;
  const char *desc = (gp && gp->description[0]) ? gp->description : "";
  bool has_counts = gp && !gp->unavailable && (gp->member_count > 0 || gp->online_count > 0);
  bool in_guild = (lg != nullptr);

  char est_date[48] = "";
  if (cp->guild_id != CCORD_ID_NONE) {
    ui_snowflake_date(cp->guild_id, est_date, sizeof est_date);
  }

  float w = CP_CARD_W;
  float pad = UIS_SP_3;
  float inner_w = w - pad * 2.0f;

  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base) + 1.0f, COL(text_header));
  ccord_text_style sub = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
  ccord_text_style ds = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_secondary));
  ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_secondary));

  float name_lh = ui_line_h(ui, ns.size);
  float sub_lh = ui_line_h(ui, sub.size);
  float desc_lh = ui_line_h(ui, ds.size);
  float desc_h = 0.0f;
  if (desc[0]) {
    ccord_vec2 dm = ccord_text_measure(r, desc, strlen(desc), &ds, inner_w);
    float max_dh = desc_lh * 4.0f;
    desc_h = dm.y > max_dh ? max_dh : dm.y;
    if (desc_h < desc_lh) desc_h = desc_lh;
  }

  float h = CP_BANNER_H + (CP_ICON_SZ * 0.5f) + 6.0f;   // banner + overlapping icon
  h += name_lh + 4.0f;                                   // server name + tag pill
  h += sub_lh + 8.0f;                                    // "Server Tag" subtitle
  h += 1.0f + 8.0f;                                      // divider
  if (has_counts) h += ui_line_h(ui, cs.size) + 4.0f;
  if (est_date[0]) h += ui_line_h(ui, sub.size) + 6.0f;
  if (desc_h > 0.0f) h += desc_h + 8.0f;
  if (in_guild) h += UIS_H_BTN_SM + 4.0f;
  h += pad;

  float ww = (float)ui->win_w, wh = (float)ui->win_h;
  float x = cp->anchor.x;
  float y = cp->anchor.y + cp->anchor.h + 6.0f;
  if (ww > 16.0f && x + w > ww - 8.0f) x = ww - w - 8.0f;
  if (x < 8.0f) x = 8.0f;
  if (wh > 16.0f && y + h > wh - 8.0f) y = cp->anchor.y - h - 6.0f;
  if (y < 8.0f) y = 8.0f;

  ccord_rect box = {x, y, w, h};
  cp->box = box;
  cp->go_btn = (ccord_rect){0};

  ui_popup_begin(ui, r, "clanpop", cp->guild_id, UI_POPUP_MS, 6.0f);
  uis_popover_frame(ui, r, box);
  ccord_push_clip(r, box);

  ccord_rect banner = {box.x, box.y, box.w, CP_BANNER_H};
  ccord_color brand_col = (gp && gp->has_brand_color) ? ui_rgb_color(gp->brand_color) : COL(bg_tertiary);
  uis_plate(ui, r, (ccord_rect){banner.x, banner.y, banner.w, banner.h + 8.0f}, 8.0f, brand_col);
  if (banner_url[0] && ui_ensure_media(ui, r, banner_url)) {
    ccord_push_clip(r, banner);
    ccord_draw_texture_cover(r, banner_url, banner, 0.0f, 255, false);
    ccord_pop_clip(r);
  }

  float icx = box.x + pad + CP_ICON_SZ * 0.5f;
  float icy = box.y + CP_BANNER_H;
  ccord_rect icon_ring = {icx - CP_ICON_SZ * 0.5f - 3.0f, icy - CP_ICON_SZ * 0.5f - 3.0f, CP_ICON_SZ + 6.0f, CP_ICON_SZ + 6.0f};
  uis_plate(ui, r, icon_ring, 12.0f, COL(bg_floating));
  ccord_rect icon_r = {icx - CP_ICON_SZ * 0.5f, icy - CP_ICON_SZ * 0.5f, CP_ICON_SZ, CP_ICON_SZ};
  if (icon_url[0] && ui_ensure_media(ui, r, icon_url)) {
    ccord_draw_texture_rounded(r, icon_url, icon_r, 10.0f, 255);
  } else {
    uis_plate(ui, r, icon_r, 10.0f, COL(bg_secondary));
    char init[4];
    guild_initials(name, init);
    ccord_text_style is = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    ccord_vec2 it = ccord_text_measure(r, init, strlen(init), &is, 0.0f);
    ccord_text_draw(r, init, strlen(init), icon_r.x + (icon_r.w - it.x) * 0.5f, icon_r.y + (icon_r.h - it.y) * 0.5f, &is, 0.0f);
  }

  float cx = box.x + pad;
  float cy = box.y + CP_BANNER_H + CP_ICON_SZ * 0.5f + 6.0f;

  // Name + [badge] TAG pill
  float tag_w = tag[0] ? ui_clan_badge_width(ui, r, tag, badge_url[0] != '\0') : 0.0f;
  float max_nw = inner_w - (tag_w > 0.0f ? tag_w + 6.0f : 0.0f);
  if (max_nw < 60.0f) max_nw = 60.0f;
  float nw = ccord_text_draw_ellipsis(r, name, strlen(name), cx, cy, max_nw, &ns);
  if (tag[0]) {
    ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_header));
    ccord_vec2 bt = ccord_text_measure(r, tag, strlen(tag), &bs, 0.0f);
    bool has_b = badge_url[0] != '\0';
    float ph = 15.0f, isz = 12.0f;
    float pw = has_b ? (5.0f + isz + 4.0f + bt.x + 6.0f) : (bt.x + 12.0f);
    ccord_rect pill = {cx + nw + 6.0f, cy + (name_lh - ph) * 0.5f, pw, ph};
    uis_plate(ui, r, pill, 3.5f, COL(bg_tertiary));
    float tx = pill.x + 6.0f;
    if (has_b) {
      ccord_rect br = {pill.x + 5.0f, pill.y + (pill.h - isz) * 0.5f, isz, isz};
      if (ui_ensure_media(ui, r, badge_url)) {
        ccord_draw_texture_rounded(r, badge_url, br, 2.0f, 255);
      }
      tx = br.x + isz + 4.0f;
    }
    ccord_text_draw(r, tag, strlen(tag), tx, pill.y + (pill.h - bt.y) * 0.5f, &bs, 0.0f);
  }
  cy += name_lh + 2.0f;

  const char *subtitle = "Server Tag";
  ccord_text_draw_ellipsis(r, subtitle, strlen(subtitle), cx, cy, inner_w, &sub);
  cy += sub_lh + 6.0f;

  uis_divider(ui, r, cx, cy, inner_w);
  cy += 8.0f;

  if (has_counts) {
    char c1[48], c2[48];
    snprintf(c1, sizeof c1, "%u Online", (unsigned)gp->online_count);
    snprintf(c2, sizeof c2, "%u Members", (unsigned)gp->member_count);
    float clh = ui_line_h(ui, cs.size);
    float dot_y = cy + clh * 0.5f;
    uis_plate(ui, r, (ccord_rect){cx, dot_y - 3.5f, 7.0f, 7.0f}, 3.5f, COL(status_online));
    ccord_vec2 c1m = ccord_text_measure(r, c1, strlen(c1), &cs, 0.0f);
    ccord_text_draw(r, c1, strlen(c1), cx + 11.0f, cy, &cs, 0.0f);
    float x2 = cx + 11.0f + c1m.x + 12.0f;
    uis_plate(ui, r, (ccord_rect){x2, dot_y - 3.5f, 7.0f, 7.0f}, 3.5f, COL(text_muted));
    ccord_text_draw(r, c2, strlen(c2), x2 + 11.0f, cy, &cs, 0.0f);
    cy += clh + 4.0f;
  }

  if (est_date[0]) {
    char est_line[64];
    snprintf(est_line, sizeof est_line, "Est. %s", est_date);
    ccord_text_draw_ellipsis(r, est_line, strlen(est_line), cx, cy, inner_w, &sub);
    cy += sub_lh + 6.0f;
  }

  if (desc_h > 0.0f) {
    ccord_push_clip(r, (ccord_rect){cx, cy, inner_w, desc_h});
    ccord_text_draw(r, desc, strlen(desc), cx, cy, &ds, inner_w);
    ccord_pop_clip(r);
    cy += desc_h + 8.0f;
  }

  if (in_guild) {
    cp->go_btn = (ccord_rect){cx, cy, inner_w, UIS_H_BTN_SM};
    bool hv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cp->go_btn);
    uis_button(ui, r, cp->go_btn, "Go to Server", UIS_BTN_PRIMARY, UIS_BTN_S_SM, (uis_state){.hover = hv}, "clangoto", 0);
  }

  ccord_pop_clip(r);
  ui_popup_end(r);
}
