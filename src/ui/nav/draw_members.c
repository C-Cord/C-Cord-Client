// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"
#include "ui_style.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

static void build_clan_badge_url(ccord_id guild_id, const char *badge, char *out, size_t cap)
{
  out[0] = '\0';
  if (guild_id == CCORD_ID_NONE || !badge || !badge[0]) return;
  if (strncmp(badge, "https://", 8) == 0 || strncmp(badge, "http://", 7) == 0) {
    snprintf(out, cap, "%s", badge);
  } else {
    snprintf(out, cap, "https://cdn.discordapp.com/guild-tag-badges/%llu/%.60s.png?size=64",
             (unsigned long long)guild_id, badge);
  }
}

float ui_clan_badge_width(ccord_ui *ui, ccord_renderer *r, const char *tag, bool has_badge)
{
  if (!tag || !tag[0]) return 0.0f;
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_header));
  ccord_vec2 bt = ccord_text_measure(r, tag, strlen(tag), &bs, 0.0f);
  return has_badge ? (5.0f + 12.0f + 4.0f + bt.x + 6.0f) : (bt.x + 12.0f);
}

float ui_draw_clan_badge_ex(ccord_ui *ui, ccord_renderer *r, ccord_id guild_id, const char *tag, const char *badge,
                            float x, float y, float line_h, bool clickable)
{
  if (!tag || !tag[0]) return 0.0f;
  char badge_url[256];
  build_clan_badge_url(guild_id, badge, badge_url, sizeof badge_url);
  bool has_badge = badge_url[0] != '\0';
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_header));
  ccord_vec2 bt = ccord_text_measure(r, tag, strlen(tag), &bs, 0.0f);
  float ph = 15.0f, isz = 12.0f;
  float pw = has_badge ? (5.0f + isz + 4.0f + bt.x + 6.0f) : (bt.x + 12.0f);
  ccord_rect pill = {x, y + (line_h - ph) * 0.5f, pw, ph};
  bool hov = clickable && ui->mouse_in && ui_in_rect(ui->mx, ui->my, pill);
  ccord_draw_rrect(r, (ccord_rect){pill.x - 0.5f, pill.y - 0.5f, pill.w + 1.0f, pill.h + 1.0f}, 4.0f,
                   ui_alpha(COL(brand), hov ? 0.75f : 0.45f));
  ccord_draw_rrect(r, pill, 3.5f, hov ? COL(bg_item_hover) : COL(bg_tertiary));
  float tx = pill.x + 6.0f;
  if (has_badge) {
    ccord_rect br = {pill.x + 5.0f, pill.y + (pill.h - isz) * 0.5f, isz, isz};
    if (ui_ensure_media(ui, r, badge_url)) {
      ccord_draw_texture_rounded(r, badge_url, br, 2.0f, 255);
    }
    tx = br.x + isz + 4.0f;
  }
  ccord_text_draw(r, tag, strlen(tag), tx, pill.y + (pill.h - bt.y) * 0.5f, &bs, 0.0f);
  if (clickable) {
    ui_clan_popover_record_hit(ui, pill, guild_id, tag, badge_url);
  }
  ui->dbg_clan_badges++;
  return pw;
}

float ui_draw_clan_badge(ccord_ui *ui, ccord_renderer *r, const char *tag, float x, float y, float line_h)
{
  return ui_draw_clan_badge_ex(ui, r, CCORD_ID_NONE, tag, nullptr, x, y, line_h, false);
}

float ui_draw_activity_card(ccord_ui *ui, ccord_renderer *r, const ccord_user *u, float x, float y, float w,
                            bool measure_only)
{
  if (!u || !u->has_activity || u->activity_name[0] == '\0') return 0.0f;
  const char *hdr = "PLAYING A GAME";
  switch (u->activity_type) {
    case CCORD_ACTIVITY_STREAMING: hdr = "LIVE ON STREAM"; break;
    case CCORD_ACTIVITY_LISTENING: hdr = "LISTENING TO"; break;
    case CCORD_ACTIVITY_WATCHING: hdr = "WATCHING"; break;
    case CCORD_ACTIVITY_COMPETING: hdr = "COMPETING IN"; break;
    default: hdr = "PLAYING A GAME"; break;
  }
  ccord_text_style hdr_s = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
  ccord_text_style nm_s = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_style dt_s = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_secondary));

  float pad = 8.0f;
  float h = pad + ui_line_h(ui, hdr_s.size) + 2.0f + ui_line_h(ui, nm_s.size);
  if (u->activity_details[0] != '\0') h += 2.0f + ui_line_h(ui, dt_s.size);
  if (u->activity_state[0] != '\0') h += 2.0f + ui_line_h(ui, dt_s.size);
  char el_buf[48] = "";
  if (u->activity_start_ms > 0 && ui->now_unix_ms > u->activity_start_ms) {
    int64_t sec = (ui->now_unix_ms - u->activity_start_ms) / 1000;
    int64_t hh = sec / 3600, mm = (sec % 3600) / 60, ss = sec % 60;
    if (hh > 0) {
      snprintf(el_buf, sizeof el_buf, "%02lld:%02lld:%02lld elapsed", (long long)hh, (long long)mm, (long long)ss);
    } else {
      snprintf(el_buf, sizeof el_buf, "%02lld:%02lld elapsed", (long long)mm, (long long)ss);
    }
    h += 2.0f + ui_line_h(ui, dt_s.size);
  }
  h += pad;

  if (!measure_only) {
    ui->dbg_activity_cards++;
    ccord_rect box = {x, y, w, h};
    ccord_draw_rrect(r, (ccord_rect){box.x - 0.5f, box.y - 0.5f, box.w + 1.0f, box.h + 1.0f}, 6.5f, COL(border_subtle));
    ccord_draw_rrect(r, box, 6.0f, COL(bg_primary));
    float ix = x + 10.0f, iw = w - 20.0f, iy = y + pad;
    if (iw < 20.0f) iw = 20.0f;
    float hdr_reserve = (u->activity_type == CCORD_ACTIVITY_STREAMING) ? 42.0f : 0.0f;
    ccord_text_draw_ellipsis(r, hdr, strlen(hdr), ix, iy, iw - hdr_reserve, &hdr_s);
    if (u->activity_type == CCORD_ACTIVITY_STREAMING) {
      ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, 9.5f, COL(text_on_brand));
      ccord_vec2 lt = ccord_text_measure(r, "LIVE", 4, &ls, 0.0f);
      ccord_rect lb = {x + w - lt.x - 18.0f, iy - 1.0f, lt.x + 10.0f, 14.0f};
      ccord_draw_rrect(r, lb, 3.0f, COL(danger));
      ccord_text_draw(r, "LIVE", 4, lb.x + 5.0f, lb.y + (lb.h - lt.y) * 0.5f, &ls, 0.0f);
    }
    iy += ui_line_h(ui, hdr_s.size) + 2.0f;
    ccord_text_draw_ellipsis(r, u->activity_name, strlen(u->activity_name), ix, iy, iw, &nm_s);
    iy += ui_line_h(ui, nm_s.size);
    if (u->activity_details[0] != '\0') {
      iy += 2.0f;
      ccord_text_draw_ellipsis(r, u->activity_details, strlen(u->activity_details), ix, iy, iw, &dt_s);
      iy += ui_line_h(ui, dt_s.size);
    }
    if (u->activity_state[0] != '\0') {
      iy += 2.0f;
      ccord_text_draw_ellipsis(r, u->activity_state, strlen(u->activity_state), ix, iy, iw, &dt_s);
      iy += ui_line_h(ui, dt_s.size);
    }
    if (el_buf[0] != '\0') {
      iy += 2.0f;
      ccord_text_style es = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(status_online));
      ccord_text_draw_ellipsis(r, el_buf, strlen(el_buf), ix, iy, iw, &es);
    }
  }
  return h;
}

// The '+' pill that ends the role list, wrapped like a role pill; returns the block's height (profile_roles.c owns the click).
static float ui_draw_member_role_plus(ccord_ui *ui, ccord_renderer *r, ccord_id guild_id, ccord_id user_id, float x, float y,
                                      float w, float cx, float cy, bool measure_only)
{
  float pw = ui_profile_roles_plus_width();
  if (cx > x && cx + pw > x + w) {
    cx = x;
    cy += 24.0f;
  }
  if (!measure_only) ui_profile_roles_plus_draw(ui, r, guild_id, user_id, (ccord_rect){cx, cy, pw, 20.0f});
  return (cy - y) + 24.0f;
}

typedef struct role_pill_item {
  ccord_id role_id;
  size_t role_idx;
  float dx, dy, pw, text_h;
} role_pill_item;

static struct {
  bool valid;
  const ccord_renderer *r;
  const ccord_store *store;
  uint64_t lists_rev;
  ccord_id guild_id;
  ccord_id user_id;
  float w, font_size, zoom;
  bool can_add;
  size_t count;
  float end_dx, end_dy;
  float total_h;
  role_pill_item items[32];
} g_role_pill_cache;

float ui_draw_member_role_pills(ccord_ui *ui, ccord_renderer *r, ccord_id guild_id, ccord_id user_id,
                                float x, float y, float w, bool measure_only)
{
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID || user_id == CCORD_ID_NONE) return 0.0f;
  const ccord_member *gm = ccord_store_find_member(ui->d.store, guild_id, user_id);
  const ccord_id *role_ids = nullptr;
  size_t role_count = 0;
  bool known_roles = ccord_store_member_roles(ui->d.store, guild_id, user_id, &role_ids, &role_count);
  if (!gm && (!known_roles || role_count == 0)) return 0.0f;
  size_t nroles = 0;
  const ccord_role *roles = ccord_store_roles(ui->d.store, guild_id, &nroles);
  float fsize = M(font_size_small) - 2.0f;
  ccord_text_style rts = ui_style(ui, CCORD_FONT_BOLD, fsize, COL(text_header));
  bool can_add = known_roles && ui_profile_roles_can_add(ui, guild_id, user_id);
  uint64_t lists_rev = ccord_store_lists_revision(ui->d.store);
  float zoom = ccord_render_get_zoom(r);

  if (known_roles && role_count > 0 && g_role_pill_cache.valid && g_role_pill_cache.r == r &&
      g_role_pill_cache.store == ui->d.store && g_role_pill_cache.lists_rev == lists_rev &&
      g_role_pill_cache.guild_id == guild_id && g_role_pill_cache.user_id == user_id &&
      g_role_pill_cache.w == w && g_role_pill_cache.font_size == fsize &&
      g_role_pill_cache.zoom == zoom && g_role_pill_cache.can_add == can_add) {
    if (!measure_only) {
      for (size_t i = 0; i < g_role_pill_cache.count; i++) {
        const role_pill_item *it = &g_role_pill_cache.items[i];
        if (it->role_idx >= nroles) continue;
        const ccord_role *rl = &roles[it->role_idx];
        ui->dbg_role_pills++;
        ccord_color rc = rl->color != 0 ? ui_rgb_color(rl->color) : COL(text_secondary);
        ccord_rect pill = {x + it->dx, y + it->dy, it->pw, 20.0f};
        ccord_draw_rrect(r, pill, 4.0f, COL(bg_tertiary));
        ccord_draw_circle(r, pill.x + 9.0f, pill.y + 10.0f, 4.5f, rc);
        ccord_text_draw_ellipsis(r, rl->name, strlen(rl->name), pill.x + 18.0f,
                                 pill.y + (pill.h - it->text_h) * 0.5f, pill.w - 22.0f, &rts);
        ui_profile_roles_pill_overlay(ui, r, guild_id, user_id, rl->id, pill);
      }
      if (can_add) {
        return ui_draw_member_role_plus(ui, r, guild_id, user_id, x, y, w,
                                        x + g_role_pill_cache.end_dx, y + g_role_pill_cache.end_dy, false);
      }
    }
    return g_role_pill_cache.total_h;
  }

  float cx = x, cy = y;
  size_t drawn = 0;
  role_pill_item items[32];
  for (size_t k = 0; known_roles && role_count > 0 && k < nroles; k++) {
    const ccord_role *rl = &roles[k];
    if (rl->id == CCORD_ID_NONE || rl->id == guild_id || !rl->name || !rl->name[0]) continue;
    bool held = false;
    for (size_t ri = 0; ri < role_count; ri++) {
      if (role_ids[ri] == rl->id) {
        held = true;
        break;
      }
    }
    if (!held) continue;
    ccord_vec2 rt = ccord_text_measure(r, rl->name, strlen(rl->name), &rts, 0.0f);
    float pw = rt.x + 24.0f;
    if (pw > w) pw = w;
    if (cx > x && cx + pw > x + w) {
      cx = x;
      cy += 24.0f;
    }
    if (drawn < 32u) {
      items[drawn] = (role_pill_item){.role_id = rl->id, .role_idx = k, .dx = cx - x, .dy = cy - y,
                                      .pw = pw, .text_h = rt.y};
    }
    if (!measure_only) {
      ui->dbg_role_pills++;
      ccord_color rc = rl->color != 0 ? ui_rgb_color(rl->color) : COL(text_secondary);
      ccord_rect pill = {cx, cy, pw, 20.0f};
      ccord_draw_rrect(r, pill, 4.0f, COL(bg_tertiary));
      ccord_draw_circle(r, pill.x + 9.0f, pill.y + 10.0f, 4.5f, rc);
      ccord_text_draw_ellipsis(r, rl->name, strlen(rl->name), pill.x + 18.0f, pill.y + (pill.h - rt.y) * 0.5f,
                               pill.w - 22.0f, &rts);
      ui_profile_roles_pill_overlay(ui, r, guild_id, user_id, rl->id, pill);   // x on hover when I may remove it
    }
    cx += pw + 6.0f;
    drawn++;
  }
  if (drawn == 0) {
    ui_member_look glook = {0};
    if (gm) ui_member_look_of(ui, guild_id, gm, &glook);
    if (!gm || !glook.badge || !glook.badge[0]) {
      if (!can_add) return 0.0f;
      return ui_draw_member_role_plus(ui, r, guild_id, user_id, x, y, w, x, y, measure_only);
    }
    ccord_vec2 bt = ccord_text_measure(r, glook.badge, strlen(glook.badge), &rts, 0.0f);
    float bw = bt.x + 24.0f;
    if (bw > w) bw = w;
    if (!measure_only) {
      ui->dbg_role_pills++;
      ccord_color rc = glook.badge_color != 0 ? ui_rgb_color(glook.badge_color) : COL(text_secondary);
      ccord_rect pill = {x, y, bw, 20.0f};
      ccord_draw_rrect(r, pill, 4.0f, COL(bg_tertiary));
      ccord_draw_circle(r, pill.x + 9.0f, pill.y + 10.0f, 4.5f, rc);
      ccord_text_draw_ellipsis(r, glook.badge, strlen(glook.badge), pill.x + 18.0f, pill.y + (pill.h - bt.y) * 0.5f,
                               pill.w - 22.0f, &rts);
    }
    if (!can_add) return 24.0f;
    cx = x + bw + 6.0f;
  }
  float res_h = can_add ? ui_draw_member_role_plus(ui, r, guild_id, user_id, x, y, w, cx, cy, measure_only)
                        : ((cy - y) + 24.0f);
  if (known_roles && drawn > 0 && drawn <= 32u) {
    g_role_pill_cache.valid = true;
    g_role_pill_cache.r = r;
    g_role_pill_cache.store = ui->d.store;
    g_role_pill_cache.lists_rev = lists_rev;
    g_role_pill_cache.guild_id = guild_id;
    g_role_pill_cache.user_id = user_id;
    g_role_pill_cache.w = w;
    g_role_pill_cache.font_size = fsize;
    g_role_pill_cache.zoom = zoom;
    g_role_pill_cache.can_add = can_add;
    g_role_pill_cache.count = drawn;
    g_role_pill_cache.end_dx = cx - x;
    g_role_pill_cache.end_dy = cy - y;
    g_role_pill_cache.total_h = res_h;
    memcpy(g_role_pill_cache.items, items, drawn * sizeof items[0]);
  }
  return res_h;
}

static void draw_dm_profile_panel(ccord_ui *ui, ccord_renderer *r, const ccord_layout_rect *l)
{
  ui->dm_panel_full_profile_rect = (ccord_rect){0};
  if (ui_is_dm_friends_view(ui) || ui_is_dm_requests_view(ui)) {
    ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base) + 2.0f, COL(text_header));
    ccord_text_draw_ellipsis(r, "Active Now", 10, l->x + 16.0f, l->y + 18.0f, l->w - 32.0f, &hs);
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    float ay = l->y + 52.0f;
    size_t drawn_act = 0;
    size_t nrel = ccord_store_relationship_count(ui->d.store);
    for (size_t i = 0; i < nrel && ay + 64.0f <= l->y + l->h; i++) {
      const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
      if (!rel || rel->kind != CCORD_RELATIONSHIP_FRIEND) continue;
      const ccord_user *u = ccord_store_user(ui->d.store, rel->user_id);
      if (!u || u->presence == CCORD_PRESENCE_OFFLINE) continue;
      char act_buf[160];
      if (!ui_format_user_activity_line(ui, u, act_buf, sizeof act_buf)) continue;
      ccord_rect card = {l->x + 12.0f, ay, l->w - 24.0f, 56.0f};
      ccord_draw_rrect(r, card, 6.0f, COL(bg_primary));
      ui_draw_avatar(ui, r, u, card.x + 22.0f, card.y + 28.0f, 32.0f, COL(bg_primary), false);
      const char *nm = (u->display_name && u->display_name[0]) ? u->display_name : u->name;
      ccord_text_draw_ellipsis(r, nm, strlen(nm), card.x + 46.0f, card.y + 10.0f, card.w - 54.0f, &ts);
      ccord_text_draw_ellipsis(r, act_buf, strlen(act_buf), card.x + 46.0f, card.y + 29.0f, card.w - 54.0f, &ss);
      ay += 64.0f;
      drawn_act++;
    }
    if (drawn_act == 0) {
      const char *empty_t = "It's quiet for now...";
      const char *empty_s = "When a friend starts an activity, we'll show it here!";
      ccord_text_draw_ellipsis(r, empty_t, strlen(empty_t), l->x + 16.0f, l->y + 56.0f, l->w - 32.0f, &ts);
      ccord_text_draw(r, empty_s, strlen(empty_s), l->x + 16.0f, l->y + 78.0f, &ss, l->w - 32.0f);
    }
    return;
  }
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch || ch->guild_id != CCORD_DM_GUILD_ID) return;
  if (ui_channel_is_group_dm(ch)) {
    size_t mcount = ui_dm_member_count(ch);
    char hdr[64];
    snprintf(hdr, sizeof hdr, "MEMBERS \xE2\x80\x94 %zu", mcount);
    ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
    float y = l->y + 18.0f;
    ccord_text_draw_ellipsis(r, hdr, strlen(hdr), l->x + 16.0f, y, l->w - 24.0f, &hs);
    y += ui_line_h(ui, hs.size) + 8.0f;
    size_t nrec = ch->recipient_count <= CCORD_MAX_DM_RECIPIENTS ? ch->recipient_count : CCORD_MAX_DM_RECIPIENTS;
    ccord_id self_id = ccord_store_self_id(ui->d.store);
    for (size_t idx = 0; idx < nrec + 1u; idx++) {
      ccord_id uid = (idx == 0) ? self_id : ch->recipient_ids[idx - 1u];
      if (uid == CCORD_ID_NONE) continue;
      const ccord_user *mu = ccord_store_user(ui->d.store, uid);
      ccord_user fallback = {
        .id = uid,
        .name = "User",
        .avatar_url = "",
        .presence = CCORD_PRESENCE_OFFLINE,
        .avatar_palette = (uint8_t)(uid % 6u),
      };
      if (!mu) mu = &fallback;
      float rh = M(member_item_height);
      if (y + rh > l->y + l->h) break;
      ccord_rect item = {l->x + 8.0f, y, l->w - 16.0f, rh};
      bool hover = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, item);
      if (hover) ccord_draw_rrect(r, item, M(channel_item_radius), COL(bg_item_hover));
      bool profile_open = (ui->member_popover_open && ui->member_popover_user_id == uid) ||
                          (ui->modal_kind == UI_MODAL_USER_PROFILE && ui->modal_target_id == uid);
      if (mu->nameplate_asset[0] != '\0') {
        (void)ui_draw_nameplate(ui, r, mu->nameplate_asset, mu->nameplate_palette,
                                item, M(channel_item_radius),
                                (ui_nameplate_opts){
                                  .hover = hover,
                                  .selected = profile_open,
                                  .animate = profile_open,
                                  .anim_ns = "gdmnp",
                                  .anim_id = uid,
                                });
      }
      if (ui->ngdm_member_hits < 16u) {
        ui->gdm_member_hits[ui->ngdm_member_hits].rect = item;
        ui->gdm_member_hits[ui->ngdm_member_hits].user_id = uid;
        ui->ngdm_member_hits++;
      }
      float cy = y + rh * 0.5f;
      ui_draw_avatar(ui, r, mu, item.x + 8.0f + 16.0f, cy, 32.0f, hover ? COL(bg_item_hover) : COL(bg_secondary), false);
      float tx = item.x + 8.0f + 32.0f + 12.0f;
      const char *mname = (mu->display_name && mu->display_name[0]) ? mu->display_name
                          : ((mu->name && mu->name[0]) ? mu->name : "User");
      char act_line[160];
      bool has_sub = ui_format_user_activity_line(ui, mu, act_line, sizeof act_line);
      ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
      ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
      float nh = ui_line_h(ui, ns.size), sh = ui_line_h(ui, ss.size);
      float ny = has_sub ? cy - (nh + sh) * 0.5f + 1.0f : cy - nh * 0.5f;
      bool is_owner = (ch->owner_id != CCORD_ID_NONE && uid == ch->owner_id);
      float badge_reserve = (is_owner ? 48.0f : 0.0f) + (mu->clan_tag[0] ? 44.0f : 0.0f);
      float maxw = item.x + item.w - tx - 8.0f;
      float name_w = maxw - badge_reserve;
      if (name_w < 30.0f) name_w = maxw;
      float nw = ccord_text_draw_ellipsis(r, mname, strlen(mname), tx, ny, name_w, &ns);
      float bx = tx + nw + 6.0f;
      if (mu->clan_tag[0] && bx + 32.0f <= item.x + item.w - 4.0f) {
        bx += ui_draw_clan_badge_ex(ui, r, mu->clan_guild_id, mu->clan_tag, mu->clan_badge, bx, ny, nh, false) + 4.0f;
      }
      if (is_owner && bx + 38.0f <= item.x + item.w - 4.0f) {
        ccord_text_style os = ui_style(ui, CCORD_FONT_BOLD, 9.5f, COL(status_idle));
        ccord_vec2 ot = ccord_text_measure(r, "OWNER", 5, &os, 0.0f);
        ccord_rect op = {bx, ny + (nh - 15.0f) * 0.5f, ot.x + 10.0f, 15.0f};
        ccord_draw_rrect(r, op, 3.0f, ui_alpha(COL(status_idle), 0.18f));
        ccord_text_draw(r, "OWNER", 5, op.x + 5.0f, op.y + (op.h - ot.y) * 0.5f, &os, 0.0f);
      }
      if (has_sub) {
        ccord_text_draw_ellipsis(r, act_line, strlen(act_line), tx, ny + nh - 1.0f, maxw, &ss);
      }
      y += rh;
    }
    return;
  }
  const ccord_user *u = ui_dm_recipient(ui, ch);
  ccord_id target_uid = u ? u->id : ch->user_id;
  if (ui->d.session && target_uid != CCORD_ID_NONE) {
    ccord_error perr;
    if (ccord_session_request_profile_ex(ui->d.session, target_uid, CCORD_ID_NONE, ui->now_ms, false, &perr) == CCORD_OK) {
      ui->dm_profile_requested_user = target_uid;
    }
  }
  const ccord_user_profile *prof = target_uid != CCORD_ID_NONE ? ccord_store_user_profile(ui->d.store, target_uid) : nullptr;

  const char *dname = (prof && prof->display_name && prof->display_name[0]) ? prof->display_name
                      : ((u && u->display_name && u->display_name[0]) ? u->display_name : ch->name);
  const char *uname = (prof && prof->handle && prof->handle[0]) ? prof->handle
                      : ((u && u->handle && u->handle[0]) ? u->handle
                         : ((u && u->name && u->name[0]) ? u->name : ch->name));
  const char *clan = (prof && prof->clan_tag[0]) ? prof->clan_tag : (u ? u->clan_tag : "");
  ccord_id clan_gid = (prof && prof->clan_guild_id != CCORD_ID_NONE) ? prof->clan_guild_id
                      : (u ? u->clan_guild_id : CCORD_ID_NONE);
  const char *clan_badge = (prof && prof->clan_badge[0]) ? prof->clan_badge : (u ? u->clan_badge : "");

  ccord_user dm_u = u ? *u : (ccord_user){0};
  dm_u.id = target_uid != CCORD_ID_NONE ? target_uid : ch->id;
  dm_u.name = dname;
  dm_u.display_name = dname;
  dm_u.handle = uname;
  dm_u.avatar_url = (prof && prof->avatar_url && prof->avatar_url[0]) ? prof->avatar_url
                    : ((u && u->avatar_url && u->avatar_url[0]) ? u->avatar_url : (ch->icon_url ? ch->icon_url : ""));
  dm_u.banner_url = (prof && prof->banner_url && prof->banner_url[0]) ? prof->banner_url
                    : ((u && u->banner_url) ? u->banner_url : "");
  if (prof && (prof->has_accent_color || prof->accent_color != 0)) {
    dm_u.accent_color = prof->accent_color;
    dm_u.has_accent_color = prof->has_accent_color || prof->accent_color != 0;
  }
  dm_u.bio = (prof && prof->bio && prof->bio[0]) ? prof->bio : ((u && u->bio && u->bio[0]) ? u->bio : "");
  dm_u.pronouns = (prof && prof->pronouns && prof->pronouns[0]) ? prof->pronouns
                  : ((u && u->pronouns && u->pronouns[0]) ? u->pronouns : "");
  if (!u) {
    dm_u.presence = CCORD_PRESENCE_OFFLINE;
    dm_u.avatar_palette = (uint8_t)(ch->id % 6);
  }
  snprintf(dm_u.clan_tag, sizeof dm_u.clan_tag, "%s", clan);
  dm_u.clan_guild_id = clan_gid;
  snprintf(dm_u.clan_badge, sizeof dm_u.clan_badge, "%s", clan_badge);

  ui_profile_look look;
  ui_profile_resolve(prof, CCORD_ID_NONE, &dm_u, &look);
  ui_profile_text ptx = ui_profile_text_colors(ui, &look);

  ccord_rect panel = {l->x, l->y, l->w, l->h};
  ccord_push_clip(r, panel);
  ui_profile_draw_theme_bg(ui, r, &look, panel, 84.0f, 0.0f);

  // Banner (84px tall)
  ccord_rect banner = {l->x, l->y, l->w, 84.0f};
  ui_profile_draw_banner(ui, r, &look, &dm_u, banner, 0.0f);

  // Avatar overlapping banner bottom-left
  float av_cx = l->x + 16.0f + 36.0f;
  float av_cy = l->y + 84.0f;
  ccord_color av_ring = ui_profile_card_bg(ui, &look, COL(bg_secondary));
  ccord_draw_circle(r, av_cx, av_cy, 41.0f, av_ring);
  ui_draw_avatar(ui, r, &dm_u, av_cx, av_cy, 72.0f, av_ring, false);

  // Inner profile card
  ccord_rect card = {l->x + 12.0f, l->y + 132.0f, l->w - 24.0f, l->h - 144.0f};
  if (card.h < 120.0f) card.h = 120.0f;
  ccord_color card_bg = look.has_theme ? ui_alpha(ui_profile_card_bg(ui, &look, COL(bg_tertiary)), 0.85f)
                                       : COL(bg_tertiary);
  ccord_draw_rrect(r, card, 8.0f, card_bg);
  ccord_push_clip(r, card);

  float x = card.x + 12.0f;
  float w = card.w - 24.0f;
  float y = card.y + 12.0f;

  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base) + 2.0f, ptx.header);
  float nlh = ui_line_h(ui, ns.size);
  float clan_res = (dm_u.clan_tag[0]) ? 64.0f : 0.0f;
  float nw = ccord_text_draw_ellipsis(r, dname, strlen(dname), x, y, w - clan_res, &ns);
  if (dm_u.clan_tag[0]) {
    (void)ui_draw_clan_badge_ex(ui, r, dm_u.clan_guild_id, dm_u.clan_tag, dm_u.clan_badge, x + nw + 6.0f, y, nlh, true);
  }
  y += nlh + 2.0f;

  const char *pronouns = dm_u.pronouns[0] ? dm_u.pronouns : nullptr;
  char handle_line[192];
  if (pronouns) {
    snprintf(handle_line, sizeof handle_line, "@%s \xE2\x80\xA2 %s", uname, pronouns);
  } else {
    snprintf(handle_line, sizeof handle_line, "@%s", uname);
  }
  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, ptx.secondary);
  ccord_text_draw_ellipsis(r, handle_line, strlen(handle_line), x, y, w, &hs);
  y += ui_line_h(ui, hs.size) + 8.0f;

  if (u && u->has_activity && u->activity_name[0] != '\0') {
    y += ui_draw_activity_card(ui, r, u, x, y, w, false) + 8.0f;
  }

  ccord_draw_rect(r, (ccord_rect){x, y, w, 1.0f}, COL(border_subtle));
  y += 10.0f;

  ccord_text_style lbl = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, ptx.header);
  ccord_text_style body = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, ptx.secondary);

  const char *about_hdr = "ABOUT ME";
  ccord_text_draw_ellipsis(r, about_hdr, strlen(about_hdr), x, y, w, &lbl);
  y += ui_line_h(ui, lbl.size) + 4.0f;

  const char *bio = dm_u.bio[0] ? dm_u.bio : "No bio set.";
  ccord_vec2 bt = ccord_text_measure(r, bio, strlen(bio), &body, w);
  ccord_text_draw(r, bio, strlen(bio), x, y, &body, w);
  y += (bt.y > 0.0f ? bt.y : ui_line_h(ui, body.size)) + 12.0f;

  ccord_draw_rect(r, (ccord_rect){x, y, w, 1.0f}, COL(border_subtle));
  y += 10.0f;

  const char *since_hdr = "DISCORD MEMBER SINCE";
  ccord_text_draw_ellipsis(r, since_hdr, strlen(since_hdr), x, y, w, &lbl);
  y += ui_line_h(ui, lbl.size) + 4.0f;

  char since_date[48];
  ui_snowflake_date(dm_u.id, since_date, sizeof since_date);
  ccord_text_draw_ellipsis(r, since_date, strlen(since_date), x, y, w, &body);

  if (target_uid != CCORD_ID_NONE && card.y + card.h - 38.0f > card.y + 40.0f) {
    ccord_rect fbtn = {x, card.y + card.h - 38.0f, w, UIS_H_BTN_SM};
    ui->dm_panel_full_profile_rect = fbtn;
    bool fhv = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, fbtn);
    uis_button(ui, r, fbtn, "View Full Profile", UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){.hover = fhv}, "dmfull", 0);
  }
  ccord_pop_clip(r);
  ui_profile_draw_effect(ui, r, &look, panel, 0.0f, false);
  ccord_pop_clip(r);
}

static void draw_search_results_panel(ccord_ui *ui, ccord_renderer *r, const ccord_layout_rect *l)
{
  ui->nsearch_card_hits = 0;
  ui->search_prev_rect = (ccord_rect){0};
  ui->search_next_rect = (ccord_rect){0};

  size_t sc = 0;
  const ccord_message *sr = ui->d.session ? ccord_session_search_results(ui->d.session, &sc) : nullptr;
  bool loading = ui->d.session && ccord_session_search_loading(ui->d.session);
  uint32_t sess_total = ui->d.session ? ccord_session_search_total_results(ui->d.session) : 0u;
  uint32_t offset = ui->d.session ? ccord_session_search_offset(ui->d.session) : 0u;
  bool use_session = ui->d.session && (sc > 0 || sess_total > 0 || offset > 0);
  size_t item_count = use_session ? sc : (!loading ? ui->nvmap : 0u);
  uint32_t total = use_session ? (sess_total >= (uint32_t)sc ? sess_total : (uint32_t)sc) : (uint32_t)item_count;

  char hdr[64];
  if (loading && item_count == 0) {
    snprintf(hdr, sizeof hdr, "Searching...");
  } else {
    snprintf(hdr, sizeof hdr, "%u Result%s", (unsigned)total, total == 1u ? "" : "s");
  }
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_draw_ellipsis(r, hdr, strlen(hdr), l->x + 12.0f, l->y + 11.0f, l->w - 24.0f, &hs);
  ccord_draw_rect(r, (ccord_rect){l->x, l->y + 36.0f, l->w, 1.0f}, COL(border_subtle));

  bool show_pager = use_session && (total > CCORD_SEARCH_PAGE_SIZE || offset > 0u);
  float footer_h = show_pager ? 38.0f : 0.0f;
  float body_y = l->y + 37.0f;
  float body_h = l->h - 37.0f - footer_h;
  if (body_h < 0.0f) body_h = 0.0f;

  float card_h = 62.0f, gap = 6.0f;
  ui->search_panel_content_h = item_count > 0 ? (8.0f + (float)item_count * (card_h + gap) + 4.0f) : 0.0f;
  float max_scroll = ui->search_panel_content_h - body_h;
  if (max_scroll < 0.0f) max_scroll = 0.0f;
  ui->search_panel_max = max_scroll;
  if (ui->search_panel_scroll > max_scroll) ui->search_panel_scroll = max_scroll;
  if (ui->search_panel_scroll < 0.0f) ui->search_panel_scroll = 0.0f;

  ccord_push_clip(r, (ccord_rect){l->x, body_y, l->w, body_h});
  ccord_id sel_ch = ccord_store_selected_channel(ui->d.store);
  ccord_text_style chs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
  ccord_text_style aus = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));
  ccord_text_style pvs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_normal));

  for (size_t i = 0; i < item_count; i++) {
    const ccord_message *m = nullptr;
    if (use_session) {
      m = &sr[i];
    } else {
      size_t raw = ui->vmap[i];
      if ((raw & (1ULL << 62)) != 0) continue;
      m = ccord_store_message_at(ui->d.store, sel_ch, raw);
    }
    if (!m) continue;
    float cy = body_y + 8.0f + (float)i * (card_h + gap) - ui->search_panel_scroll;
    if (cy + card_h < body_y || cy > body_y + body_h) continue;

    ccord_rect card = {l->x + 8.0f, cy, l->w - 16.0f, card_h};
    bool hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, card);
    ccord_draw_rrect(r, card, 6.0f, hover ? COL(bg_item_hover) : COL(bg_primary));

    ccord_id cid = m->channel_id != CCORD_ID_NONE ? m->channel_id : sel_ch;
    const ccord_channel *ch = ccord_store_channel(ui->d.store, cid);
    const ccord_user *u = ccord_store_user(ui->d.store, m->author_id);
    const char *aname = (u && u->display_name && u->display_name[0]) ? u->display_name
                        : ((u && u->name && u->name[0]) ? u->name : "Unknown");
    char meta[160];
    if (ch && ch->guild_id != CCORD_DM_GUILD_ID && ch->name && ch->name[0]) {
      snprintf(meta, sizeof meta, "#%s", ch->name);
      ccord_text_draw_ellipsis(r, meta, strlen(meta), card.x + 8.0f, card.y + 6.0f, card.w - 16.0f, &chs);
      ccord_text_draw_ellipsis(r, aname, strlen(aname), card.x + 8.0f, card.y + 21.0f, card.w - 16.0f, &aus);
    } else {
      ccord_text_draw_ellipsis(r, aname, strlen(aname), card.x + 8.0f, card.y + 10.0f, card.w - 16.0f, &aus);
    }
    char prev[160];
    size_t pl = ccord_message_preview(m, prev, sizeof prev);
    pl = ui_streamer_mask_inplace(ui, prev, pl, true);   // Streamer Mode: hide invite codes in search hits
    ccord_text_draw_ellipsis(r, prev, pl, card.x + 8.0f, card.y + 39.0f, card.w - 16.0f, &pvs);

    if (ui->nsearch_card_hits < UI_SEARCH_CARD_HIT_MAX) {
      ui->search_card_hits[ui->nsearch_card_hits++] = (ui_search_card_hit){card, cid, m->id};
    }
  }
  if (item_count == 0 && !loading) {
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    ccord_text_draw(r, "No results found.", 17, l->x + 12.0f, body_y + 16.0f, &es, l->w - 24.0f);
  }
  ccord_pop_clip(r);

  if (show_pager) {
    float fy = l->y + l->h - footer_h;
    ccord_draw_rect(r, (ccord_rect){l->x, fy, l->w, 1.0f}, COL(border_subtle));
    bool can_prev = offset > 0u;
    bool can_next = offset + (uint32_t)sc < total && sc > 0u;
    ccord_rect pb = {l->x + 8.0f, fy + 6.0f, 54.0f, 26.0f};
    ccord_rect nb = {l->x + l->w - 62.0f, fy + 6.0f, 54.0f, 26.0f};
    ui->search_prev_rect = can_prev ? pb : (ccord_rect){0};
    ui->search_next_rect = can_next ? nb : (ccord_rect){0};

    ccord_draw_rrect(r, pb, 4.0f, can_prev ? COL(bg_primary) : COL(bg_tertiary));
    ccord_draw_rrect(r, nb, 4.0f, can_next ? COL(bg_primary) : COL(bg_tertiary));
    ccord_text_style bps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f,
                                    can_prev ? COL(text_header) : COL(text_muted));
    ccord_text_style bns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f,
                                    can_next ? COL(text_header) : COL(text_muted));
    ccord_vec2 pt = ccord_text_measure(r, "Prev", 4, &bps, 0.0f);
    ccord_vec2 nt = ccord_text_measure(r, "Next", 4, &bns, 0.0f);
    ccord_text_draw(r, "Prev", 4, pb.x + (pb.w - pt.x) * 0.5f, pb.y + (pb.h - pt.y) * 0.5f, &bps, 0.0f);
    ccord_text_draw(r, "Next", 4, nb.x + (nb.w - nt.x) * 0.5f, nb.y + (nb.h - nt.y) * 0.5f, &bns, 0.0f);

    uint32_t cur_p = (offset / CCORD_SEARCH_PAGE_SIZE) + 1u;
    uint32_t max_p = (total + CCORD_SEARCH_PAGE_SIZE - 1u) / CCORD_SEARCH_PAGE_SIZE;
    if (max_p == 0u) max_p = 1u;
    char pg[32];
    snprintf(pg, sizeof pg, "%u / %u", (unsigned)cur_p, (unsigned)max_p);
    ccord_text_style pgs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    ccord_vec2 pgt = ccord_text_measure(r, pg, strlen(pg), &pgs, 0.0f);
    ccord_text_draw(r, pg, strlen(pg), l->x + (l->w - pgt.x) * 0.5f, fy + (footer_h - pgt.y) * 0.5f, &pgs, 0.0f);
  }
}

// Identity of a member-list row across frames, for its row motion: a member keeps its key when it changes group.
static uint64_t member_row_key(const ccord_ui *ui, ccord_id g, const member_row *row)
{
  if (row->is_header) return ((uint64_t)1 << 56) ^ ui_name_id(row->label);
  const ccord_member *m = ccord_store_member_at(ui->d.store, g, row->index);
  return m ? ((uint64_t)2 << 56) ^ m->user_id : ((uint64_t)3 << 56) ^ (uint64_t)row->index;
}

void ui_draw_member_list(ccord_ui *ui, ccord_renderer *r)
{
  const ccord_layout_rect *l = &ui->layout.member_list;
  ui->ngdm_member_hits = 0;
  ui->dm_panel_full_profile_rect = (ccord_rect){0};
  ccord_rect mbg = ui_rect(*l);
  if (ui->mobile_layout) {
    mbg = (ccord_rect){l->x, 0.0f, l->w + ui->safe_insets.right, ui->win_h};
  }
  ccord_draw_rect(r, mbg, COL(bg_secondary));
  ccord_draw_rect(r, (ccord_rect){l->x, mbg.y, 1.0f, mbg.h}, COL(border_subtle));
  ccord_push_clip(r, (ccord_rect){l->x + 1.0f, l->y, fmaxf(l->w - 1.0f, 0.0f), l->h});
  if (ui_search_panel_active(ui)) {
    draw_search_results_panel(ui, r, l);
    ccord_pop_clip(r);
    return;
  }
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  if (g == CCORD_DM_GUILD_ID) {
    draw_dm_profile_panel(ui, r, l);
    ccord_pop_clip(r);
    return;
  }
  float max_scroll = ui->member_content_h - l->h;
  if (max_scroll < 0.0f) max_scroll = 0.0f;
  if (ui->member_scroll > max_scroll) ui->member_scroll = max_scroll;
  // Row motion: a member that moved group (online <-> offline) or a header that moved slides to its new place.
  ui_track_begin(ui, &ui->trk_member, "trkm", g);
  for (size_t i = 0; i < ui->nmrow; i++) {
    const member_row *row = &ui->mrow[i];
    float alpha;
    float row_cy = ui_track_row(ui, &ui->trk_member, member_row_key(ui, g, row), row->y, 0.0f, false, &alpha);
    float y = l->y + row_cy - ui->member_scroll;
    if (y + row->h < l->y || y > l->y + l->h) continue;
    if (row->is_header) {
      ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
      float hy = y + row->h - ui_line_h(ui, hs.size) - 4.0f;
      ccord_text_draw_ellipsis(r, row->label, strlen(row->label), l->x + 16.0f, hy, l->w - 24.0f, &hs);
      continue;
    }
    const ccord_member *m = ccord_store_member_at(ui->d.store, g, row->index);
    if (!m) continue;
    const ccord_user *u = ccord_store_user(ui->d.store, m->user_id);
    if (!u) continue;
    bool offline = u->presence == CCORD_PRESENCE_OFFLINE;
    bool hover = (int)i == ui->hover_mrow && !ui->menu.open && !ui->settings_open;
    ccord_rect item = {l->x + 8.0f, y, l->w - 16.0f, row->h};
    ui_row_state mrs = ui_row_state_of(ui, "member", m->user_id, hover, false, COL(bg_secondary));
    ccord_color item_bg = mrs.bg;
    ui_row_state_fill(ui, r, &mrs, item, M(channel_item_radius));
    bool profile_open = (ui->member_popover_open && ui->member_popover_user_id == m->user_id) ||
                        (ui->modal_kind == UI_MODAL_USER_PROFILE && ui->modal_target_id == m->user_id);
    const char *np_asset = m->nameplate_asset[0] ? m->nameplate_asset : u->nameplate_asset;
    const char *np_pal = m->nameplate_asset[0] ? m->nameplate_palette : u->nameplate_palette;
    if (np_asset[0] != '\0') {
      (void)ui_draw_nameplate(ui, r, np_asset, np_pal, item, M(channel_item_radius),
                              (ui_nameplate_opts){
                                .hover = hover,
                                .selected = profile_open,
                                .animate = profile_open,
                                .offline = offline,
                                .anim_ns = "memnp",
                                .anim_id = m->user_id,
                              });
    }
    float cy = y + row->h * 0.5f;
    ccord_user draw_u = *u;
    if (m->avatar_decoration_url[0] != '\0') draw_u.avatar_decoration_url = m->avatar_decoration_url;
    ui_draw_avatar(ui, r, &draw_u, item.x + 8.0f + 16.0f, cy, 32.0f, item_bg, offline);
    float tx = item.x + 8.0f + 32.0f + 12.0f;
    char act_line[160];
    bool has_status = ui_format_user_activity_line(ui, u, act_line, sizeof act_line);
    ccord_color nc = row->name_color != 0 ? ui_rgb_color(row->name_color) : COL(text_normal);
    if (offline) nc = ui_mix(nc, item_bg, 0.55f);
    ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), nc);
    float nh = ui_line_h(ui, ns.size);
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, ui_mix(COL(text_muted), item_bg, offline ? 0.4f : 0.0f));
    float sh = ui_line_h(ui, ss.size);
    float ny = has_status ? cy - (nh + sh) * 0.5f + 1.0f : cy - nh * 0.5f;
    float maxw = item.x + item.w - tx - 8.0f;
    float tag_w = (u->is_bot ? 34.0f : 0.0f) + (u->clan_tag[0] ? 44.0f : 0.0f);
    const char *mname = (m->nick[0] != '\0') ? m->nick : ((u->display_name && u->display_name[0]) ? u->display_name : u->name);
    float name_avail = maxw - tag_w;
    if (name_avail < 32.0f) name_avail = maxw;
    float nw = ccord_text_draw_ellipsis(r, mname, strlen(mname), tx, ny, name_avail, &ns);
    float bx = tx + nw + 6.0f;
    if (u->clan_tag[0] && bx + 32.0f <= item.x + item.w - 4.0f) {
      bx += ui_draw_clan_badge_ex(ui, r, u->clan_guild_id, u->clan_tag, u->clan_badge, bx, ny, nh, false) + 4.0f;
    }
    if (u->is_bot && bx + 26.0f <= item.x + item.w - 4.0f) {
      ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_on_brand));
      ccord_vec2 bt = ccord_text_measure(r, "BOT", 3, &bs, 0.0f);
      ccord_rect tag = {bx, ny + (nh - 15.0f) * 0.5f + 0.5f, bt.x + 9.0f, 15.0f};
      ccord_draw_rrect(r, tag, 3.0f, offline ? ui_mix(COL(brand), item_bg, 0.5f) : COL(brand));
      ccord_text_draw(r, "BOT", 3, tag.x + 4.5f, tag.y + (tag.h - bt.y) * 0.5f, &bs, 0.0f);
      bx += tag.w + 4.0f;
    }
    if (bx + 12.0f <= item.x + item.w - 4.0f) {
      bx += ui_decor_draw_inline_badges(ui, r, CCORD_DECOR_MEMBER_BADGE, m->user_id, bx, ny, nh, item_bg);
    }
    if (has_status) ccord_text_draw_ellipsis(r, act_line, strlen(act_line), tx, ny + nh - 1.0f, maxw, &ss);
  }
  ui_track_end(&ui->trk_member);
  ccord_pop_clip(r);
}

void ui_draw_member_popover(ccord_ui *ui, ccord_renderer *r)
{
  ui->popover_full_profile_rect = (ccord_rect){0};
  if (!ui->member_popover_open || ui->member_popover_user_id == CCORD_ID_NONE) return;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  bool in_server = (g != CCORD_ID_NONE && g != CCORD_DM_GUILD_ID);
  if (ui->d.session) {
    ccord_error perr;
    (void)ccord_session_request_profile_ex(ui->d.session, ui->member_popover_user_id,
                                           in_server ? g : CCORD_ID_NONE, ui->now_ms, false, &perr);
  }
  const ccord_user *store_u = ccord_store_user(ui->d.store, ui->member_popover_user_id);
  const ccord_user_profile *prof = ccord_store_user_profile(ui->d.store, ui->member_popover_user_id);
  ccord_user view = store_u ? *store_u : (ccord_user){0};
  view.id = ui->member_popover_user_id;
  const char *u_name = (store_u && store_u->name && store_u->name[0]) ? store_u->name : nullptr;
  const char *u_dname = (store_u && store_u->display_name && store_u->display_name[0]) ? store_u->display_name : nullptr;
  const char *p_name = (prof && prof->display_name && prof->display_name[0]) ? prof->display_name : nullptr;
  const char *p_handle = (prof && prof->handle && prof->handle[0]) ? prof->handle : nullptr;
  const char *u_handle = (store_u && store_u->handle && store_u->handle[0]) ? store_u->handle : nullptr;
  view.name = p_name ? p_name : (u_dname ? u_dname : (u_name ? u_name : (p_handle ? p_handle : (u_handle ? u_handle : "Discord User"))));
  view.display_name = view.name;
  view.handle = p_handle ? p_handle : (u_handle ? u_handle : view.name);
  view.status_text = (store_u && store_u->status_text) ? store_u->status_text : "";
  view.avatar_url = (prof && prof->avatar_url && prof->avatar_url[0]) ? prof->avatar_url
                    : ((store_u && store_u->avatar_url) ? store_u->avatar_url : "");
  view.banner_url = (prof && prof->banner_url && prof->banner_url[0]) ? prof->banner_url
                    : ((store_u && store_u->banner_url) ? store_u->banner_url : "");
  view.accent_color = (prof && prof->accent_color != 0) ? prof->accent_color
                      : (store_u ? store_u->accent_color : 0);
  view.bio = (prof && prof->bio && prof->bio[0]) ? prof->bio
             : ((store_u && store_u->bio) ? store_u->bio : "");
  view.pronouns = (prof && prof->pronouns && prof->pronouns[0]) ? prof->pronouns
                  : ((store_u && store_u->pronouns) ? store_u->pronouns : "");
  if (prof && prof->clan_tag[0] != '\0') {
    snprintf(view.clan_tag, sizeof view.clan_tag, "%s", prof->clan_tag);
  }
  if (prof && prof->clan_guild_id != CCORD_ID_NONE) {
    view.clan_guild_id = prof->clan_guild_id;
  }
  if (prof && prof->clan_badge[0] != '\0') {
    snprintf(view.clan_badge, sizeof view.clan_badge, "%s", prof->clan_badge);
  }
  if (!store_u) {
    view.presence = CCORD_PRESENCE_OFFLINE;
    view.avatar_palette = (uint8_t)(ui->member_popover_user_id % 6u);
  }
  const ccord_member *gm_pre = in_server ? ccord_store_find_member(ui->d.store, g, view.id) : nullptr;
  if (gm_pre && gm_pre->avatar_decoration_url[0] != '\0') {
    view.avatar_decoration_url = gm_pre->avatar_decoration_url;
  }
  ui_profile_look look;
  ui_profile_resolve(prof, in_server ? g : CCORD_ID_NONE, &view, &look);
  ui_profile_text ptx = ui_profile_text_colors(ui, &look);
  const ccord_user *u = &view;

  ccord_rect card = ui->member_popover_rect;
  if (card.w <= 0.0f || card.h <= 0.0f) {
    // Opened without a click position (a programmatic open): place it beside the member list, as a row click does.
    float cw = fminf(280.0f, (float)ui->win_w - 16.0f);
    float ch = fminf(280.0f, (float)ui->win_h - 16.0f);
    if (cw <= 0.0f || ch <= 0.0f) return;
    float cx = fmaxf(8.0f, fminf(ui->layout.member_list.x - cw - 8.0f, (float)ui->win_w - cw - 8.0f));
    card = (ccord_rect){cx, fmaxf(8.0f, ((float)ui->win_h - ch) * 0.5f), cw, ch};
  }

  ui_profile_roles_set_clip(ui, card);   // the role + / x hit only inside the card
  float w = card.w - 28.0f;
  if (w < 60.0f) w = 60.0f;

  const ccord_member *gm = in_server ? ccord_store_find_member(ui->d.store, g, u->id) : nullptr;
  int active_tab = in_server ? ui->member_popover_tab : 0;

  // A per-server profile nick (look.server_profile) already sits in u->display_name; the global name goes below it.
  const char *global_dname = (u->display_name && u->display_name[0]) ? u->display_name : "Discord User";
  const char *server_dname = (gm && gm->nick[0] != '\0' && !look.server_profile) ? gm->nick : global_dname;
  const char *dname = (active_tab == 1 || look.server_profile) ? server_dname : global_dname;
  bool show_global = look.server_profile && look.global_name && look.global_name[0] && strcmp(look.global_name, dname) != 0;
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base) + 1.0f, ptx.header);
  ccord_text_style sub = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, ptx.muted);
  ccord_text_style bs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, ptx.secondary);
  float badges_h = ui_profile_draw_badges(ui, r, &look, 0.0f, 0.0f, w, true);
  float since_h = ui_profile_draw_since(ui, r, &look, 0.0f, 0.0f, w, &sub, true);

  const char *bio = (u->bio && u->bio[0]) ? u->bio : "Discord Member";
  float bio_lh = ui_line_h(ui, bs.size);
  ccord_vec2 bio_m = ccord_text_measure(r, bio, strlen(bio), &bs, w);
  float bio_h = bio_m.y > bio_lh ? bio_m.y : bio_lh;
  float max_bio_h = bio_lh * 4.0f;
  if (bio_h > max_bio_h) bio_h = max_bio_h;

  char status_line[256] = "";
  bool cs_valid = (u->custom_status_emoji[0] != '\0' || u->custom_status_text[0] != '\0') &&
                  (u->custom_status_expires_ms == 0 || ui->now_unix_ms <= 0 ||
                   u->custom_status_expires_ms > ui->now_unix_ms);
  if (cs_valid) {
    if (u->custom_status_emoji[0] && u->custom_status_text[0]) {
      (void)snprintf(status_line, sizeof status_line, "%.64s %.160s", u->custom_status_emoji, u->custom_status_text);
    } else if (u->custom_status_emoji[0]) {
      (void)snprintf(status_line, sizeof status_line, "%.64s", u->custom_status_emoji);
    } else {
      (void)snprintf(status_line, sizeof status_line, "%.160s", u->custom_status_text);
    }
  } else if (!u->has_activity && u->status_text && u->status_text[0] && u->custom_status_expires_ms == 0) {
    (void)snprintf(status_line, sizeof status_line, "%.200s", u->status_text);
  }

  float act_card_h = ui_draw_activity_card(ui, r, u, 0.0f, 0.0f, w, true);
  float roles_h = (active_tab == 1 && gm) ? ui_draw_member_role_pills(ui, r, g, u->id, 0.0f, 0.0f, w, true) : 0.0f;
  float plugin_sec_h = 0.0f;
  if (ui->d.contrib) {
    size_t nd = ccord_contrib_decorator_count(ui->d.contrib);
    for (size_t di = 0; di < nd; di++) {
      const ccord_decorator_contribution *dc = ccord_contrib_decorator_at(ui->d.contrib, di);
      if (!dc || dc->target != CCORD_DECOR_PROFILE_SECTION) continue;
      if (dc->target_id != 0 && dc->target_id != u->id) continue;
      if (!dc->widget_json || !dc->widget_json[0]) continue;
      ccord_vec2 dsz = ui_panels_measure_widget_json(ui, r, dc->widget_json, w - UIS_SP_2 * 2.0f);
      if (dsz.y > 0.0f) plugin_sec_h += dsz.y + UIS_SP_2 * 3.0f;
    }
  }

  float needed_h = 80.0f;
  needed_h += ui_line_h(ui, ns.size) + 2.0f;
  needed_h += ui_line_h(ui, sub.size) + 4.0f;
  if (status_line[0] != '\0') {
    needed_h += ui_line_h(ui, sub.size) + 4.0f;
  }
  if (in_server) {
    needed_h += 28.0f;   // User Profile | Server Profile tab strip
  }
  needed_h += 10.0f;
  if (act_card_h > 0.0f) {
    needed_h += act_card_h + 8.0f;
  }
  if (active_tab == 1 && gm && roles_h > 0.0f) {
    needed_h += roles_h + 4.0f;
  } else {
    needed_h += bio_h + 6.0f;
  }
  needed_h += since_h + 10.0f;
  needed_h += plugin_sec_h;
  needed_h += badges_h;
  needed_h += UIS_H_BTN_SM + 6.0f;   // View Full Profile button
  needed_h += UIS_H_BTN_SM + 12.0f;
  if (needed_h < 260.0f) needed_h = 260.0f;
  needed_h = ceilf(needed_h / UIS_SP_1) * UIS_SP_1;   // popover heights sit on the spacing grid
  if (ui->win_h > 32.0f && needed_h > (float)ui->win_h - 16.0f) {
    needed_h = (float)ui->win_h - 16.0f;
  }
  card.h = needed_h;
  float pad_t = 8.0f, pad_b = 8.0f, pad_h = 8.0f;
  if (look.frame && look.frame->sku_id != CCORD_ID_NONE) {
    float fscale = card.w / (look.frame->inner_width > 0 ? (float)look.frame->inner_width : 300.0f);
    float ft = (float)look.frame->overflow_top * fscale;
    float fb = (float)look.frame->overflow_bottom * fscale;
    float fh = (float)look.frame->overflow_horizontal * fscale;
    if ((float)ui->win_h > card.h + ft + fb + 16.0f) {
      pad_t += ft;
      pad_b += fb;
    }
    if ((float)ui->win_w > card.w + fh * 2.0f + 16.0f) {
      pad_h += fh;
    }
  }
  if (ui->win_w > 16.0f) {
    if (card.x + card.w > (float)ui->win_w - pad_h) card.x = (float)ui->win_w - card.w - pad_h;
    if (card.x < pad_h) card.x = pad_h;
  }
  if (ui->win_h > 16.0f) {
    if (card.y + card.h > (float)ui->win_h - pad_b) card.y = (float)ui->win_h - card.h - pad_b;
    if (card.y < pad_t) card.y = pad_t;
  }
  ui->member_popover_rect = card;

  uis_popover_frame(ui, r, card);
  float pop_rad = uis_tokens_of(ui).r_surface;
  ccord_push_clip(r, card);
  ui_profile_draw_theme_bg(ui, r, &look, card, 50.0f, pop_rad);
  ccord_rect banner = {card.x, card.y, card.w, 50.0f};
  ui_profile_draw_banner(ui, r, &look, u, banner, pop_rad);
  ccord_color av_ring = ui_profile_card_bg(ui, &look, COL(bg_floating));
  if (look.deco_url[0] != '\0') {
    ccord_draw_circle(r, card.x + 38.0f, card.y + 50.0f, 27.5f, av_ring);
  } else {
    ccord_draw_rrect(r, (ccord_rect){card.x + 38.0f - 27.5f, card.y + 50.0f - 27.5f, 55.0f, 55.0f},
                     roundf(48.0f * 0.26f) + 3.0f, av_ring);
  }
  ui_draw_avatar(ui, r, u, card.x + 38.0f, card.y + 50.0f, 48.0f, av_ring, false);

  float x = card.x + 14.0f;
  float y = card.y + 80.0f;
  float nlh = ui_line_h(ui, ns.size);
  float clan_res = u->clan_tag[0] ? 64.0f : 0.0f;
  float nw = ccord_text_draw_ellipsis(r, dname, strlen(dname), x, y, w - clan_res, &ns);
  if (u->clan_tag[0]) {
    (void)ui_draw_clan_badge_ex(ui, r, u->clan_guild_id, u->clan_tag, u->clan_badge, x + nw + 6.0f, y, nlh, true);
  }
  y += nlh + 2.0f;

  const char *uname = (u->handle && u->handle[0]) ? u->handle : global_dname;
  char handle_line[256];
  int hn = snprintf(handle_line, sizeof handle_line, "%s%s@%s", show_global ? look.global_name : "",
                    show_global ? " \xE2\x80\xA2 " : "", uname);
  if (u->pronouns && u->pronouns[0] && hn > 0 && (size_t)hn < sizeof handle_line) {
    snprintf(handle_line + hn, sizeof handle_line - (size_t)hn, " \xE2\x80\xA2 %s", u->pronouns);
  }
  ccord_text_draw_ellipsis(r, handle_line, strlen(handle_line), x, y, w, &sub);
  y += ui_line_h(ui, sub.size) + 4.0f;
  if (badges_h > 0.0f) {
    y += ui_profile_draw_badges(ui, r, &look, x, y, w, false);
  }
  if (status_line[0] != '\0') {
    ccord_text_draw_ellipsis(r, status_line, strlen(status_line), x, y, w, &sub);
    y += ui_line_h(ui, sub.size) + 4.0f;
  }

  ui->member_popover_tab_rects[0] = (ccord_rect){0};
  ui->member_popover_tab_rects[1] = (ccord_rect){0};
  if (in_server) {
    static const char *const k_ptabs[2] = {"User Profile", "Server Profile"};
    float tx = x;
    for (int t = 0; t < 2; t++) {
      bool sel = (active_tab == t);
      ccord_text_style ts = ui_style(ui, sel ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small) - 2.0f,
                                     sel ? ptx.header : ptx.muted);
      ccord_vec2 tt = ccord_text_measure(r, k_ptabs[t], strlen(k_ptabs[t]), &ts, 0.0f);
      ccord_rect tr = {tx, y + 2.0f, tt.x + 12.0f, 22.0f};
      ui->member_popover_tab_rects[t] = tr;
      ccord_text_draw(r, k_ptabs[t], strlen(k_ptabs[t]), tr.x + 6.0f, tr.y + (tr.h - tt.y) * 0.5f, &ts, 0.0f);
      if (sel) {
        ccord_draw_rect(r, (ccord_rect){tr.x + 4.0f, tr.y + tr.h - 2.0f, tr.w - 8.0f, 2.0f}, COL(brand));
      }
      tx += tr.w + 8.0f;
    }
    y += 28.0f;
  }
  ccord_draw_rect(r, (ccord_rect){x, y + 2.0f, w, 1.0f}, COL(divider));
  y += 10.0f;

  if (act_card_h > 0.0f) {
    y += ui_draw_activity_card(ui, r, u, x, y, w, false) + 8.0f;
  }

  if (active_tab == 1 && gm && roles_h > 0.0f) {
    y += ui_draw_member_role_pills(ui, r, g, u->id, x, y, w, false) + 4.0f;
  } else {
    ccord_push_clip(r, (ccord_rect){x, y, w, bio_h + 2.0f});
    ccord_text_draw(r, bio, strlen(bio), x, y, &bs, w);
    ccord_pop_clip(r);
    y += bio_h + 6.0f;
  }

  y += ui_profile_draw_since(ui, r, &look, x, y, w, &sub, false);
  if (plugin_sec_h > 0.0f) {
    y += ui_decor_draw_profile_sections(ui, r, u->id, x, y + 4.0f, w, av_ring);
  }

  float by = card.y + card.h - 12.0f - UIS_H_BTN_SM;
  ui->popover_full_profile_rect = (ccord_rect){x, by - UIS_H_BTN_SM - 6.0f, w, UIS_H_BTN_SM};
  uis_button(ui, r, ui->popover_full_profile_rect, "View Full Profile", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
             (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ui->popover_full_profile_rect)}, "mpfull", 0);

  float bw = (w - 8.0f) * 0.5f;
  ui->member_popover_msg_btn = (ccord_rect){x, by, bw, UIS_H_BTN_SM};
  ui->member_popover_friend_btn = (ccord_rect){x + bw + 8.0f, by, bw, UIS_H_BTN_SM};
  uis_button(ui, r, ui->member_popover_msg_btn, "Message", UIS_BTN_PRIMARY, UIS_BTN_S_SM,
             (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ui->member_popover_msg_btn)}, "mpmsg", 0);

  ccord_relationship_kind rkind = ccord_store_relationship(ui->d.store, u->id);
  const char *flabel = (rkind == CCORD_REL_FRIEND) ? "Remove Friend" : "Add Friend";
  uis_button(ui, r, ui->member_popover_friend_btn, flabel, UIS_BTN_SECONDARY, UIS_BTN_S_SM,
             (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ui->member_popover_friend_btn)}, "mpfriend", 0);
  ui_profile_draw_effect(ui, r, &look, card, pop_rad, false);
  ccord_pop_clip(r);
  ui_profile_draw_frame(ui, r, &look, card);
  ui_profile_view_debug_record(false, &look, u, since_h > 0.0f);
  ui_profile_flush_tooltip(ui, r);
}

bool ui_member_popover_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->member_popover_open) return false;
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    for (int t = 0; t < 2; t++) {
      if (ui->member_popover_tab_rects[t].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->member_popover_tab_rects[t])) {
        ui->member_popover_tab = t;
        ui->dirty = true;
        return true;
      }
    }
    ccord_rect header_hit = {ui->member_popover_rect.x, ui->member_popover_rect.y, ui->member_popover_rect.w, 80.0f};
    if ((ui->popover_full_profile_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->popover_full_profile_rect)) ||
        ui_in_rect(ev->x, ev->y, header_hit)) {
      ccord_id uid = ui->member_popover_user_id;
      ccord_id g = ccord_store_selected_guild(ui->d.store);
      bool in_server = (g != CCORD_ID_NONE && g != CCORD_DM_GUILD_ID);
      ui->member_popover_open = false;
      ui_open_user_profile_modal(ui, uid, in_server ? g : CCORD_ID_NONE);
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui->member_popover_msg_btn)) {
      ccord_id uid = ui->member_popover_user_id;
      ui->member_popover_open = false;
      ui_open_dm_with_user(ui, uid);
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui->member_popover_friend_btn)) {
      ccord_id uid = ui->member_popover_user_id;
      ccord_relationship_kind rkind = ccord_store_relationship(ui->d.store, uid);
      ui_set_user_relationship(ui, uid, (rkind == CCORD_REL_FRIEND) ? CCORD_REL_NONE : CCORD_REL_OUTGOING_REQUEST);
      ui->member_popover_open = false;
      ui->dirty = true;
      return true;
    }
    if (!ui_in_rect(ev->x, ev->y, ui->member_popover_rect)) {
      ui->member_popover_open = false;
      ui->dirty = true;
      return false;
    }
    return true;
  }
  return false;
}
