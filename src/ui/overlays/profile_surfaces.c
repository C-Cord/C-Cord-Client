// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

void dc_draw_user_profile_modal(ccord_ui *ui, ccord_renderer *r)
{
  ccord_id uid = ui->modal_target_id;
  if (uid == CCORD_ID_NONE) return;
  ccord_id gid = (ui->modal_guild_id != CCORD_ID_NONE && ui->modal_guild_id != CCORD_DM_GUILD_ID)
                   ? ui->modal_guild_id
                   : CCORD_ID_NONE;
  if (ui->d.session) {
    ccord_error perr;
    (void)ccord_session_request_profile_ex(ui->d.session, uid, gid, ui->now_ms, false, &perr);
  }
  const ccord_user *store_u = ccord_store_user(ui->d.store, uid);
  const ccord_user_profile *prof = ccord_store_user_profile(ui->d.store, uid);
  ccord_user view = store_u ? *store_u : (ccord_user){0};
  view.id = uid;
  const char *p_name = (prof && prof->display_name && prof->display_name[0]) ? prof->display_name : nullptr;
  const char *u_dname = (store_u && store_u->display_name && store_u->display_name[0]) ? store_u->display_name : nullptr;
  const char *u_name = (store_u && store_u->name && store_u->name[0]) ? store_u->name : nullptr;
  const char *p_hand = (prof && prof->handle && prof->handle[0]) ? prof->handle : nullptr;
  const char *u_hand = (store_u && store_u->handle && store_u->handle[0]) ? store_u->handle : nullptr;
  view.name = p_name ? p_name : (u_dname ? u_dname : (u_name ? u_name : (p_hand ? p_hand : (u_hand ? u_hand : "Discord User"))));
  view.display_name = view.name;
  view.handle = p_hand ? p_hand : (u_hand ? u_hand : view.name);
  view.avatar_url = (prof && prof->avatar_url && prof->avatar_url[0]) ? prof->avatar_url
                    : ((store_u && store_u->avatar_url) ? store_u->avatar_url : "");
  view.banner_url = (prof && prof->banner_url && prof->banner_url[0]) ? prof->banner_url
                    : ((store_u && store_u->banner_url) ? store_u->banner_url : "");
  view.accent_color = (prof && prof->accent_color != 0) ? prof->accent_color : (store_u ? store_u->accent_color : 0);
  view.bio = (prof && prof->bio && prof->bio[0]) ? prof->bio : ((store_u && store_u->bio) ? store_u->bio : "");
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
    view.avatar_palette = (uint8_t)(uid % 6u);
  }
  if (gid != CCORD_ID_NONE && gid != CCORD_DM_GUILD_ID) {
    const ccord_member *gm_pre = ccord_store_find_member(ui->d.store, gid, uid);
    if (gm_pre && gm_pre->avatar_decoration_url[0] != '\0') {
      view.avatar_decoration_url = gm_pre->avatar_decoration_url;
    }
  }
  ui_profile_look look;
  ui_profile_resolve(prof, gid, &view, &look);
  ui_profile_text ptx = ui_profile_text_colors(ui, &look);
  const ccord_user *u = &view;

  ui_popup_begin(ui, r, "pmodal", uid, UI_MODAL_MS, 10.0f);
  float mw = clampf(ui->win_w - 48.0f, 360.0f, UIS_MODAL_W_LG);
  float mh = clampf(ui->win_h - 48.0f, 340.0f, 620.0f);
  ccord_rect m = uis_modal_box(ui, mw, mh);
  ui->profile_modal_box = m;
  uis_modal_frame(ui, r, m);
  uis_tokens ptk = uis_tokens_of(ui);
  ccord_push_clip(r, m);

  ui_profile_draw_theme_bg(ui, r, &look, m, 104.0f, ptk.r_surface);
  ccord_rect banner = {m.x, m.y, m.w, 104.0f};
  ui_profile_draw_banner(ui, r, &look, u, banner, ptk.r_surface);
  ui->profile_modal_close_rect = (ccord_rect){m.x + m.w - UIS_ICON_BTN - UIS_SP_2, m.y + UIS_SP_2, UIS_ICON_BTN, UIS_ICON_BTN};
  {
    ccord_rect cr = ui->profile_modal_close_rect;
    ccord_draw_rrect(r, cr, ptk.r_control, ui_alpha(COL(bg_tertiary), 0.8f));   // scrim chip so the glyph reads on a banner
    ccord_color cg = uis_icon_button(ui, r, cr, (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cr)},
                                     "pmclose", 0);
    ui_icon_close(r, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f, 12.0f, cg);
  }

  float av_cx = m.x + 24.0f + 38.0f, av_cy = m.y + 104.0f;
  ccord_color av_ring = ui_profile_card_bg(ui, &look, COL(bg_secondary));
  ccord_draw_circle(r, av_cx, av_cy, 43.0f, av_ring);
  ui_draw_avatar(ui, r, u, av_cx, av_cy, 76.0f, av_ring, false);

  ccord_id self_id = ccord_store_self_id(ui->d.store);
  if (uid != self_id) {
    ccord_rect mb = {m.x + m.w - 148.0f, m.y + 116.0f, 124.0f, UIS_H_BTN};
    ui->profile_modal_msg_rect = mb;
    uis_button(ui, r, mb, "Send Message", UIS_BTN_SUCCESS, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mb)}, "pmmsg", 0);
  }

  float hx = m.x + 24.0f, hy = m.y + 150.0f, hw = m.w - 48.0f;
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 1.0f, ptx.header);
  float nlh = ui_line_h(ui, ns.size);
  float nw = ccord_text_draw_ellipsis(r, u->display_name, strlen(u->display_name), hx, hy, hw - 110.0f, &ns);
  float bx = hx + nw + 8.0f;
  if (u->clan_tag[0]) {
    bx += ui_draw_clan_badge_ex(ui, r, u->clan_guild_id, u->clan_tag, u->clan_badge, bx, hy, nlh, true) + 6.0f;
  }
  if (u->is_bot) {
    ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_on_brand));
    ccord_vec2 bt = ccord_text_measure(r, "BOT", 3, &bs, 0.0f);
    ccord_rect btag = {bx, hy + (nlh - 16.0f) * 0.5f, bt.x + 10.0f, 16.0f};
    ccord_draw_rrect(r, btag, 3.5f, COL(brand));
    ccord_text_draw(r, "BOT", 3, btag.x + 5.0f, btag.y + (btag.h - bt.y) * 0.5f, &bs, 0.0f);
  }
  hy += nlh + 2.0f;

  char hline[256];
  bool show_global = look.server_profile && dc_has_global_name_differs(look.global_name, u->display_name);
  int hn = snprintf(hline, sizeof hline, "%s%s@%s", show_global ? look.global_name : "", show_global ? " \xE2\x80\xA2 " : "", u->handle);
  if (u->pronouns && u->pronouns[0] && hn > 0 && (size_t)hn < sizeof hline) {
    snprintf(hline + hn, sizeof hline - (size_t)hn, " \xE2\x80\xA2 %s", u->pronouns);
  }
  ccord_text_style sub = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), ptx.secondary);
  ccord_text_draw_ellipsis(r, hline, strlen(hline), hx, hy, hw, &sub);
  hy += ui_line_h(ui, sub.size) + 10.0f;
  {
    float bh = ui_profile_draw_badges(ui, r, &look, hx, hy - 4.0f, hw, false);
    if (bh > 0.0f) hy += bh;
  }

  size_t mg_cnt = prof ? prof->mutual_guild_count : 0u;
  uint32_t mf_cnt = prof ? (prof->mutual_friend_total >= prof->mutual_friend_count
                              ? prof->mutual_friend_total
                              : (uint32_t)prof->mutual_friend_count)
                         : 0u;
  char t1_lbl[48], t2_lbl[48];
  snprintf(t1_lbl, sizeof t1_lbl, "Mutual Servers (%zu)", mg_cnt);
  snprintf(t2_lbl, sizeof t2_lbl, "Mutual Friends (%u)", (unsigned)mf_cnt);
  const char *ptabs[3] = {"User Info", t1_lbl, t2_lbl};
  float tx = hx;
  for (int t = 0; t < 3; t++) {
    bool sel = (ui->profile_modal_tab == t);
    ccord_text_style ts_t = ui_style(ui, sel ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small),
                                     sel ? ptx.header : ptx.muted);
    ccord_vec2 tt = ccord_text_measure(r, ptabs[t], strlen(ptabs[t]), &ts_t, 0.0f);
    ccord_rect tr = {tx, hy, tt.x + 16.0f, 28.0f};
    ui->profile_modal_tab_rects[t] = tr;
    ccord_text_draw(r, ptabs[t], strlen(ptabs[t]), tr.x + 8.0f, tr.y + (tr.h - tt.y) * 0.5f, &ts_t, 0.0f);
    if (sel) {
      ccord_draw_rect(r, (ccord_rect){tr.x + 4.0f, tr.y + tr.h - 2.0f, tr.w - 8.0f, 2.0f}, COL(brand));
    }
    tx += tr.w + 10.0f;
  }
  hy += 30.0f;
  uis_divider(ui, r, hx, hy, hw);
  hy += 10.0f;

  ccord_rect body_area = {m.x + 16.0f, hy, m.w - 32.0f, maxf((m.y + m.h - 14.0f) - hy, 40.0f)};
  uis_plate(ui, r, body_area, ptk.r_surface,
            look.has_theme ? ui_alpha(look.light_text ? (ccord_color){0, 0, 0, 255} : (ccord_color){255, 255, 255, 255}, 0.22f)
                           : COL(bg_tertiary));
  ccord_push_clip(r, body_area);
  ui_profile_roles_set_clip(ui, body_area);   // the role + / x hit only where the body is visible
  float body_y0 = body_area.y, body_y1 = body_area.y + body_area.h;

  float ix = body_area.x + 14.0f, iw = body_area.w - 28.0f;
  float max_s = maxf(ui->profile_modal_content_h - body_area.h, 0.0f);
  ui->profile_modal_max = max_s;
  ui->profile_modal_scroll = clampf(ui->profile_modal_scroll, 0.0f, max_s);
  float cy = body_area.y + 12.0f - ui->profile_modal_scroll;

  ccord_text_style hdr_s = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, ptx.header);
  ccord_text_style bdy_s = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), ptx.secondary);
  bool dates_shown = false;

  if (ui->profile_modal_tab == 0) {
    ccord_text_draw(r, "ABOUT ME", 8, ix, cy, &hdr_s, 0.0f);
    cy += ui_line_h(ui, hdr_s.size) + 4.0f;
    const char *bio = (u->bio && u->bio[0]) ? u->bio : "No bio set.";
    ccord_vec2 bm = ccord_text_measure(r, bio, strlen(bio), &bdy_s, iw);
    ccord_text_draw(r, bio, strlen(bio), ix, cy, &bdy_s, iw);
    cy += maxf(bm.y, ui_line_h(ui, bdy_s.size)) + 12.0f;

    if (u->has_activity && u->activity_name[0] != '\0') {
      cy += ui_draw_activity_card(ui, r, u, ix, cy, iw, false) + 12.0f;
    }

    ccord_text_draw(r, "MEMBER SINCE", 12, ix, cy, &hdr_s, 0.0f);
    cy += ui_line_h(ui, hdr_s.size) + 4.0f;
    cy += ui_profile_draw_since(ui, r, &look, ix, cy, iw, &bdy_s, false) + 12.0f;
    dates_shown = true;

    if (gid != CCORD_ID_NONE) {
      float rh = ui_draw_member_role_pills(ui, r, gid, uid, ix, cy, iw, true);
      if (rh > 0.0f) {
        ccord_text_draw(r, "ROLES", 5, ix, cy, &hdr_s, 0.0f);
        cy += ui_line_h(ui, hdr_s.size) + 4.0f;
        cy += ui_draw_member_role_pills(ui, r, gid, uid, ix, cy, iw, false) + 8.0f;
      }
    }

    ccord_text_draw(r, "NOTE (ONLY VISIBLE TO YOU)", 26, ix, cy, &hdr_s, 0.0f);
    cy += ui_line_h(ui, hdr_s.size) + 4.0f;
    ccord_rect nr = {ix, cy, maxf(iw - 92.0f, 80.0f), UIS_H_INPUT};
    ccord_rect nsb = {nr.x + nr.w + 8.0f, cy + 2.0f, 84.0f, UIS_H_BTN};
    if (nr.y + nr.h > body_y0 && nr.y < body_y1) {
      float ny0 = maxf(nr.y, body_y0), ny1 = fminf(nr.y + nr.h, body_y1);
      ui->profile_modal_note_rect = (ccord_rect){nr.x, ny0, nr.w, ny1 - ny0};
      ui->profile_modal_note_save_rect = (ccord_rect){nsb.x, ny0, nsb.w, ny1 - ny0};
    }
    (void)uis_input(ui, r, nr, &ui->modal_edit1, "Click to add a note", (uis_state){.focused = true}, false, "pmnote", 0);
    uis_button(ui, r, nsb, "Save Note", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, nsb)}, "pmnotesave", 0);
    cy += 44.0f;

    if (prof && prof->connection_count > 0) {
      ccord_text_draw(r, "CONNECTIONS", 11, ix, cy, &hdr_s, 0.0f);
      cy += ui_line_h(ui, hdr_s.size) + 6.0f;
      float cw = (iw - 8.0f) * 0.5f;
      for (size_t ci = 0; ci < prof->connection_count; ci++) {
        const ccord_profile_connection *cn = &prof->connections[ci];
        float cx = ix + (float)(ci % 2u) * (cw + 8.0f);
        float cry = cy + (float)(ci / 2u) * 40.0f;
        ccord_rect cr = {cx, cry, cw, 34.0f};
        bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cr);
        uis_plate(ui, r, cr, ptk.r_control, COL(bg_secondary));
        uis_list_row_fill(ui, r, cr, (uis_state){.hover = chv}, "pmconn", (uint64_t)ci);
        char clbl[160];
        snprintf(clbl, sizeof clbl, "%s: %s", cn->type[0] ? cn->type : "Account", cn->name);
        float vres = cn->verified ? 58.0f : 0.0f;
        ccord_text_style cts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));
        ccord_text_draw_ellipsis(r, clbl, strlen(clbl), cr.x + 8.0f, cr.y + 9.0f, maxf(cr.w - 16.0f - vres, 20.0f), &cts);
        if (cn->verified) {
          ccord_text_style vts = ui_style(ui, CCORD_FONT_BOLD, 9.0f, COL(status_online));
          ccord_text_draw(r, "VERIFIED", 8, cr.x + cr.w - 54.0f, cr.y + 11.0f, &vts, 0.0f);
        }
        if (ui->nprofile_modal_conn_hits < CCORD_PROFILE_MAX_CONNECTIONS &&
            cr.y + cr.h > body_y0 && cr.y < body_y1) {
          float cy0 = maxf(cr.y, body_y0), cy1 = fminf(cr.y + cr.h, body_y1);
          char tmp_url[256] = "";
          if (cn->name[0]) {
            if (strcmp(cn->type, "github") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://github.com/%s", cn->name);
            } else if (strcmp(cn->type, "twitch") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://twitch.tv/%s", cn->name);
            } else if (strcmp(cn->type, "reddit") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://reddit.com/u/%s", cn->name);
            } else if (strcmp(cn->type, "twitter") == 0 || strcmp(cn->type, "x") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://x.com/%s", cn->name);
            } else if (strcmp(cn->type, "youtube") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://youtube.com/@%s", cn->name);
            } else if (strcmp(cn->type, "steam") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://steamcommunity.com/id/%s", cn->name);
            } else if (strcmp(cn->type, "bluesky") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://bsky.app/profile/%s", cn->name);
            } else if (strcmp(cn->type, "tiktok") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://tiktok.com/@%s", cn->name);
            }
          }
          size_t idx = ui->nprofile_modal_conn_hits++;
          ui->profile_modal_conn_hits[idx].rect = (ccord_rect){cr.x, cy0, cr.w, cy1 - cy0};
          ui->profile_modal_conn_hits[idx].url = ui_hit_cstr(ui, tmp_url);
        }
      }
      cy += (float)((prof->connection_count + 1u) / 2u) * 40.0f + 6.0f;
    }
  } else if (ui->profile_modal_tab == 1) {
    if (!prof || prof->mutual_guild_count == 0) {
      ccord_text_draw(r, "No mutual servers", 17, ix, cy + 8.0f, &bdy_s, 0.0f);
      cy += 36.0f;
    } else {
      ccord_text_style rts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
      for (size_t i = 0; i < prof->mutual_guild_count; i++) {
        ccord_id mgid = prof->mutual_guild_ids[i];
        const ccord_guild *gd = ccord_store_guild(ui->d.store, mgid);
        char gname_buf[64];
        const char *gname = (gd && gd->name && gd->name[0]) ? gd->name : nullptr;
        if (!gname) {
          snprintf(gname_buf, sizeof gname_buf, "Server %llu", (unsigned long long)mgid);
          gname = gname_buf;
        }
        ccord_rect rr = {ix, cy, iw, 40.0f};
        bool rhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rr);
        uis_plate(ui, r, rr, ptk.r_control, COL(bg_secondary));
        uis_list_row_fill(ui, r, rr, (uis_state){.hover = rhv}, "pmmg", (uint64_t)mgid);
        ccord_rect gic = {rr.x + 8.0f, rr.y + 6.0f, 28.0f, 28.0f};
        ccord_draw_rrect(r, gic, 8.0f, COL(brand));
        char init[8];
        ui_initials(gname, init);
        ccord_text_style ist = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_on_brand));
        ccord_vec2 it = ccord_text_measure(r, init, strlen(init), &ist, 0.0f);
        ccord_text_draw(r, init, strlen(init), gic.x + (gic.w - it.x) * 0.5f, gic.y + (gic.h - it.y) * 0.5f, &ist, 0.0f);
        ccord_text_draw_ellipsis(r, gname, strlen(gname), rr.x + 46.0f, rr.y + 10.0f, rr.w - 56.0f, &rts);
        if (ui->nprofile_modal_mutual_hits < 32u && rr.y + rr.h > body_y0 && rr.y < body_y1) {
          float ry0 = maxf(rr.y, body_y0), ry1 = fminf(rr.y + rr.h, body_y1);
          ui->profile_modal_mutual_hits[ui->nprofile_modal_mutual_hits].rect = (ccord_rect){rr.x, ry0, rr.w, ry1 - ry0};
          ui->profile_modal_mutual_hits[ui->nprofile_modal_mutual_hits].id = mgid;
          ui->nprofile_modal_mutual_hits++;
        }
        cy += 44.0f;
      }
    }
  } else {
    if (!prof || prof->mutual_friend_count == 0) {
      ccord_text_draw(r, "No mutual friends", 17, ix, cy + 8.0f, &bdy_s, 0.0f);
      cy += 36.0f;
    } else {
      ccord_text_style rts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
      for (size_t i = 0; i < prof->mutual_friend_count; i++) {
        const ccord_mutual_friend *mf = &prof->mutual_friends[i];
        const char *fnm = mf->name[0] ? mf->name : (mf->handle[0] ? mf->handle : "User");
        ccord_rect rr = {ix, cy, iw, 40.0f};
        bool rhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rr);
        uis_plate(ui, r, rr, ptk.r_control, COL(bg_secondary));
        uis_list_row_fill(ui, r, rr, (uis_state){.hover = rhv}, "pmmf", (uint64_t)mf->user_id);
        ccord_user fu = {
          .id = mf->user_id,
          .name = fnm,
          .avatar_url = mf->avatar_url,
          .presence = CCORD_PRESENCE_OFFLINE,
          .avatar_palette = (uint8_t)(mf->user_id % 6u),
        };
        ui_draw_avatar(ui, r, &fu, rr.x + 22.0f, rr.y + 20.0f, 28.0f, COL(bg_secondary), false);
        float fnw = ccord_text_draw_ellipsis(r, fnm, strlen(fnm), rr.x + 44.0f, rr.y + 10.0f, rr.w * 0.55f, &rts);
        if (mf->handle[0]) {
          char htag[112];
          snprintf(htag, sizeof htag, "@%s", mf->handle);
          ccord_text_draw_ellipsis(r, htag, strlen(htag), rr.x + 52.0f + fnw, rr.y + 12.0f,
                                   maxf(rr.w - (60.0f + fnw), 20.0f), &bdy_s);
        }
        if (ui->nprofile_modal_mutual_hits < 32u && rr.y + rr.h > body_y0 && rr.y < body_y1) {
          float ry0 = maxf(rr.y, body_y0), ry1 = fminf(rr.y + rr.h, body_y1);
          ui->profile_modal_mutual_hits[ui->nprofile_modal_mutual_hits].rect = (ccord_rect){rr.x, ry0, rr.w, ry1 - ry0};
          ui->profile_modal_mutual_hits[ui->nprofile_modal_mutual_hits].id = mf->user_id;
          ui->nprofile_modal_mutual_hits++;
        }
        cy += 44.0f;
      }
    }
  }
  ui->profile_modal_content_h = (cy + ui->profile_modal_scroll) - body_area.y + 8.0f;
  ccord_pop_clip(r);
  ui_profile_draw_effect(ui, r, &look, m, ptk.r_surface, true);
  ccord_pop_clip(r);
  ui_profile_draw_frame(ui, r, &look, m);
  ui_profile_view_debug_record(true, &look, u, dates_shown);
  ui_profile_flush_tooltip(ui, r);
  ui_popup_end(r);
}

