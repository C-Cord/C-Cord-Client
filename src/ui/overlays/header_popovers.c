// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

// ---------------------------------------------------------------- search / pinned popovers

void ui_draw_search_popover(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui->search_open) return;
  ccord_rect box = ui_search_popover_rect(ui);
  uis_tokens tk = uis_tokens_of(ui);
  uis_popover_frame(ui, r, box);
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
  ccord_text_draw(r, "SEARCH MESSAGES", 15, box.x + 10.0f, box.y + 7.0f, &ls, 0.0f);
  ccord_rect input = {box.x + UIS_SP_2, box.y + 24.0f, box.w - 2.0f * UIS_SP_2, UIS_H_INPUT};
  uis_state ist = {.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, input), .focused = true};
  (void)uis_input_padded(ui, r, input, 30.0f, UIS_SP_2, &ui->search_edit, "Filter this channel", ist, false, "srchin", 0);
  ui_icon_search(r, input.x + 15.0f, input.y + input.h * 0.5f, 14.0f, COL(text_muted));
}

static void draw_pin_row(ccord_ui *ui, ccord_renderer *r, const ccord_message *m, ccord_rect ir)
{
  const ccord_user *u = ccord_store_user(ui->d.store, m->author_id);
  ui_draw_avatar(ui, r, u, ir.x + 14.0f, ir.y + ir.h * 0.5f, 26.0f, (ccord_color){0, 0, 0, 0}, false);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_secondary));
  ccord_text_draw_ellipsis(r, u ? u->name : "Unknown User", strlen(u ? u->name : "Unknown User"), ir.x + 32.0f, ir.y + 3.0f,
                           ir.w - 40.0f, &ns);
  char pline[256];
  size_t pl = ccord_message_preview(m, pline, sizeof pline);
  pl = ui_streamer_mask_inplace(ui, pline, pl, true);
  ccord_text_draw_ellipsis(r, pline, pl, ir.x + 32.0f, ir.y + 18.0f, ir.w - 40.0f, &ts);
  ui_icon_pin(r, ir.x + ir.w - 10.0f, ir.y + 12.0f, 12.0f, COL(text_muted));
}

void ui_draw_pins_popover(ccord_ui *ui, ccord_renderer *r)
{
  ui->npins_vis = 0;
  ui->pins_load_more_rect = (ccord_rect){0};
  ui->pins_thumb = (ccord_rect){0};
  ui->dbg_pins_total = 0;
  ui->dbg_pins_has_more = false;
  if (!ui->pins_open) return;

  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  const ccord_message *pins[256];
  size_t npins = 0;
  if (ccord_store_channel_pins_loaded(ui->d.store, ch)) {
    size_t bid_cnt = 0;
    const ccord_id *bids = ccord_store_channel_pin_ids(ui->d.store, ch, &bid_cnt);
    for (size_t i = 0; bids && i < bid_cnt && npins < 256u; i++) {
      const ccord_message *m = ccord_store_find_message(ui->d.store, ch, bids[i]);
      if (m) pins[npins++] = m;
    }
  } else {
    size_t total = ccord_store_message_count(ui->d.store, ch);
    for (size_t i = total; i > 0 && npins < 256u; i--) {
      const ccord_message *m = ccord_store_message_at(ui->d.store, ch, i - 1u);
      if (m && m->pinned) pins[npins++] = m;
    }
  }
  ui->dbg_pins_total = npins;
  ui->dbg_pins_has_more = ccord_store_channel_pins_has_more(ui->d.store, ch);

  ccord_rect box = ui_pins_popover_rect(ui);
  uis_popover_frame(ui, r, box);
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_draw(r, "Pinned Messages", 15, box.x + 10.0f, box.y + 9.0f, &ls, 0.0f);
  uis_divider(ui, r, box.x + UIS_SP_2, box.y + 30.0f, box.w - 2.0f * UIS_SP_2);

  if (npins == 0) {
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    ccord_text_draw(r, "No pinned messages in this channel.", 35, box.x + 10.0f, box.y + 40.0f, &es, box.w - 20.0f);
    return;
  }

  float footer_h = ui->dbg_pins_has_more ? 34.0f : 0.0f;
  float list_y = box.y + 34.0f;
  float list_h = box.h - 34.0f - 8.0f - footer_h;
  if (list_h < 40.0f) list_h = 40.0f;
  float content_h = (float)npins * 44.0f;
  float max_scroll = content_h - list_h;
  if (max_scroll < 0.0f) max_scroll = 0.0f;
  ui->pins_max = max_scroll;
  if (ui->pins_scroll > max_scroll) ui->pins_scroll = max_scroll;
  if (ui->pins_scroll < 0.0f) ui->pins_scroll = 0.0f;

  ccord_push_clip(r, (ccord_rect){box.x, list_y, box.w, list_h});
  for (size_t i = 0; i < npins; i++) {
    float ry = list_y + (float)i * 44.0f - ui->pins_scroll;
    if (ry + 40.0f < list_y || ry > list_y + list_h) continue;
    ccord_rect ir = {box.x + 8.0f, ry, box.w - 16.0f, 40.0f};
    draw_pin_row(ui, r, pins[i], ir);
    if (ui->npins_vis < 64u) {
      ui->pins_vis_ids[ui->npins_vis] = pins[i]->id;
      ui->pins_vis_rects[ui->npins_vis] = ir;
      ui->npins_vis++;
    }
  }
  ccord_pop_clip(r);
  ui->pins_thumb = ui_draw_list_scrollbar(ui, r, (ccord_rect){box.x, list_y, box.w, list_h}, content_h,
                                          ui->pins_scroll, "pinsscroll", 0);

  if (ui->dbg_pins_has_more) {
    bool loading = ui->d.session && ccord_session_pins_loading(ui->d.session, ch);
    ccord_rect lm = {box.x + UIS_SP_2, box.y + box.h - 34.0f, box.w - 2.0f * UIS_SP_2, UIS_H_BTN_SM};
    ui->pins_load_more_rect = lm;
    bool hv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, lm);
    uis_button(ui, r, lm, loading ? "Loading..." : "Load more", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = hv, .disabled = loading}, "pinmore", 0);
  }
}

void ui_draw_reactions_popover(ccord_ui *ui, ccord_renderer *r)
{
  ui->nreactions_tabs = 0;
  ui->dbg_reactions_users = 0;
  ui->reactions_popover_rect = (ccord_rect){0};
  ui->reactions_close_rect = (ccord_rect){0};
  ui->reactions_thumb = (ccord_rect){0, 0, 0, 0};
  ui->reactions_tab_thumb = (ccord_rect){0, 0, 0, 0};
  if (!ui->reactions_open) return;

  const ccord_message *m = ccord_store_find_message(ui->d.store, ui->reactions_channel_id, ui->reactions_msg_id);
  if (!m || !dc_message_has_reactions(m)) {
    ui->reactions_open = false;
    return;
  }

  float w = 440.0f, h = 300.0f;
  if (w > ui->win_w - 32.0f && ui->win_w > 64.0f) w = ui->win_w - 32.0f;
  if (h > ui->win_h - 32.0f && ui->win_h > 64.0f) h = ui->win_h - 32.0f;
  ccord_rect box = {(ui->win_w - w) * 0.5f, (ui->win_h - h) * 0.5f, w, h};
  ui->reactions_popover_rect = box;

  ccord_draw_rect(r, (ccord_rect){0, 0, ui->win_w, ui->win_h}, (ccord_color){0, 0, 0, 120});
  ccord_draw_rrect(r, (ccord_rect){box.x - 1.0f, box.y - 1.0f, box.w + 2.0f, box.h + 2.0f}, 8.0f, COL(border_subtle));
  ccord_draw_rrect(r, box, 7.0f, COL(bg_floating));

  float side_w = 126.0f;
  ccord_draw_rect(r, (ccord_rect){box.x + side_w, box.y, 1.0f, box.h}, COL(divider));
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_draw(r, "Reactions", 9, box.x + 12.0f, box.y + 10.0f, &hs, 0.0f);

  ccord_rect close_r = {box.x + box.w - 34.0f, box.y + 8.0f, 26.0f, 24.0f};
  ui->reactions_close_rect = close_r;
  ccord_draw_rrect(r, close_r, 4.0f, COL(bg_tertiary));
  ccord_text_draw(r, "X", 1, close_r.x + 9.0f, close_r.y + 4.0f, &hs, 0.0f);

  // Emoji tab column: count kinds, clamp scroll, keep the active tab in view, then clip + cull.
  ccord_rect tab_view = {box.x, box.y + 38.0f, side_w, box.h - 46.0f};
  size_t ntabs = 0;
  for (size_t i = 0; i < m->reaction_kind_count && ntabs < 16u; i++) {
    const ccord_reaction *rx = &m->reactions[i];
    if (!rx->emoji || rx->count == 0) continue;
    ntabs++;
  }
  float tab_content_h = (float)ntabs * 32.0f;
  float tab_max_s = maxf(tab_content_h - tab_view.h, 0.0f);
  ui->reactions_tab_max = tab_max_s;
  ui->reactions_tab_scroll = clampf(ui->reactions_tab_scroll, 0.0f, tab_max_s);
  if (ui->reactions_tab < ntabs) {
    float sel_top = (float)ui->reactions_tab * 32.0f;
    float sel_bot = sel_top + 28.0f;
    if (sel_top < ui->reactions_tab_scroll) ui->reactions_tab_scroll = sel_top;
    else if (sel_bot > ui->reactions_tab_scroll + tab_view.h)
      ui->reactions_tab_scroll = clampf(sel_bot - tab_view.h, 0.0f, tab_max_s);
  }
  ccord_push_clip(r, tab_view);
  size_t tab_idx = 0;
  for (size_t i = 0; i < m->reaction_kind_count && tab_idx < 16u; i++) {
    const ccord_reaction *rx = &m->reactions[i];
    if (!rx->emoji || rx->count == 0) continue;
    float ty = tab_view.y + (float)tab_idx * 32.0f - ui->reactions_tab_scroll;
    ccord_rect tr = {box.x + 8.0f, ty, side_w - 16.0f, 28.0f};
    snprintf(ui->reactions_tab_emojis[tab_idx], sizeof ui->reactions_tab_emojis[tab_idx], "%.95s", rx->emoji);
    if (ty + 28.0f < tab_view.y || ty > tab_view.y + tab_view.h) {
      ui->reactions_tab_rects[tab_idx] = (ccord_rect){0, 0, 0, 0};
      tab_idx++;
      continue;
    }
    ui->reactions_tab_rects[tab_idx] = tr;
    bool active = (tab_idx == ui->reactions_tab);
    ccord_draw_rrect(r, tr, 5.0f, active ? COL(bg_item_selected) : COL(bg_secondary));
    char tlbl[120];
    if (rx->burst_count > 0) {
      snprintf(tlbl, sizeof tlbl, "%s  %u (%u*)", rx->emoji, (unsigned)rx->count, (unsigned)rx->burst_count);
    } else {
      snprintf(tlbl, sizeof tlbl, "%s  %u", rx->emoji, (unsigned)rx->count);
    }
    ccord_text_style ts = ui_style(ui, active ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small) - 1.0f,
                                   active ? COL(text_header) : COL(text_secondary));
    ccord_text_draw_ellipsis(r, tlbl, strlen(tlbl), tr.x + 8.0f, tr.y + 6.0f, tr.w - 14.0f, &ts);
    tab_idx++;
  }
  ccord_pop_clip(r);
  ui->nreactions_tabs = tab_idx;
  if (ui->reactions_tab >= ui->nreactions_tabs && ui->nreactions_tabs > 0) {
    ui->reactions_tab = 0;
  }
  ui->reactions_tab_thumb = ui_draw_list_scrollbar(ui, r, tab_view, tab_content_h, ui->reactions_tab_scroll,
                                                   "reactiontabs", 0);

  float rx_x = box.x + side_w + 12.0f;
  float rx_y = box.y + 38.0f;
  float rx_w = box.w - side_w - 24.0f;
  float rx_h = box.h - 48.0f;

  size_t nuc = 0;
  bool loading = false;
  const ccord_user *users = nullptr;
  if (ui->d.session && ui->nreactions_tabs > 0) {
    ccord_id rc_ch = CCORD_ID_NONE, rc_msg = CCORD_ID_NONE;
    const char *rc_emoji = nullptr;
    size_t raw_cnt = 0;
    bool raw_load = false;
    const ccord_user *raw_users = ccord_session_reaction_users(ui->d.session, &rc_ch, &rc_msg, &rc_emoji,
                                                               &raw_cnt, &raw_load);
    if (rc_ch == ui->reactions_channel_id && rc_msg == ui->reactions_msg_id &&
        rc_emoji && strcmp(rc_emoji, ui->reactions_tab_emojis[ui->reactions_tab]) == 0) {
      users = raw_users;
      nuc = raw_cnt;
      loading = raw_load;
    }
  }
  ui->dbg_reactions_users = nuc;

  ccord_push_clip(r, (ccord_rect){rx_x, rx_y, rx_w, rx_h});
  if (nuc == 0) {
    const char *msg = loading ? "Loading reactions..." : "No reaction users loaded.";
    ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    ccord_text_draw(r, msg, strlen(msg), rx_x, rx_y + 8.0f, &ms, rx_w);
  } else {
    float max_s = (float)nuc * 40.0f - rx_h;
    if (max_s < 0.0f) max_s = 0.0f;
    ui->reactions_max = max_s;
    if (ui->reactions_scroll > max_s) ui->reactions_scroll = max_s;
    if (ui->reactions_scroll < 0.0f) ui->reactions_scroll = 0.0f;
    ccord_text_style uns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style uhs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    for (size_t u = 0; u < nuc; u++) {
      float uy = rx_y + (float)u * 40.0f - ui->reactions_scroll;
      if (uy + 36.0f < rx_y || uy > rx_y + rx_h) continue;
      const ccord_user *usr = &users[u];
      ui_draw_avatar(ui, r, usr, rx_x + 14.0f, uy + 18.0f, 28.0f, COL(bg_floating), false);
      const char *dname = (usr->display_name && usr->display_name[0]) ? usr->display_name
                          : ((usr->name && usr->name[0]) ? usr->name : "User");
      ccord_text_draw_ellipsis(r, dname, strlen(dname), rx_x + 36.0f, uy + 3.0f, rx_w - 40.0f, &uns);
      if (usr->handle && usr->handle[0]) {
        char hbuf[96];
        snprintf(hbuf, sizeof hbuf, "@%s", usr->handle);
        ccord_text_draw_ellipsis(r, hbuf, strlen(hbuf), rx_x + 36.0f, uy + 19.0f, rx_w - 40.0f, &uhs);
      }
    }
  }
  ccord_pop_clip(r);
  ui->reactions_thumb = ui_draw_list_scrollbar(ui, r, (ccord_rect){rx_x, rx_y, rx_w, rx_h},
                                               (float)nuc * 40.0f, ui->reactions_scroll, "reactionusers", 0);
}

static bool inbox_message_mentions_self(const ccord_ui *ui, ccord_id guild_id, const ccord_message *m)
{
  if (!ui || !m || ccord_message_is_ephemeral(m)) return false;
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  if (self_id == CCORD_ID_NONE || m->author_id == self_id) return false;
  if (ccord_message_mentions_user(m, self_id)) return true;
  if (m->rich) {
    if (m->rich->mention_everyone) return true;
    if (m->rich->mention_role_count > 0 && guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID) {
      const ccord_id *rids = nullptr;
      size_t rcnt = 0;
      if (ccord_store_member_roles(ui->d.store, guild_id, self_id, &rids, &rcnt)) {
        for (size_t k = 0; k < rcnt; k++) {
          if (ccord_rich_mentions_role(m->rich, rids[k])) return true;
        }
      }
    }
  }
  return false;
}

typedef struct inbox_entry {
  ccord_id guild_id;
  ccord_id channel_id;
  ccord_id message_id;
  uint32_t unread_count;
  uint32_t mention_count;
  char title[140];
  char subtitle[200];
} inbox_entry;

static size_t collect_inbox_entries(const ccord_ui *ui, int tab, inbox_entry *out, size_t cap)
{
  if (!ui || !out || cap == 0) return 0;
  size_t count = 0;
  size_t ng = ccord_store_guild_count(ui->d.store);
  for (size_t pass = 0; pass <= ng && count < cap; pass++) {
    ccord_id gid = (pass == 0) ? CCORD_DM_GUILD_ID : ccord_store_guild_at(ui->d.store, pass - 1)->id;
    const ccord_guild *g = (pass == 0) ? nullptr : ccord_store_guild_at(ui->d.store, pass - 1);
    if (pass > 0 && !g) continue;
    size_t nc = ccord_store_channel_count(ui->d.store, gid);
    for (size_t ci = 0; ci < nc && count < cap; ci++) {
      const ccord_channel *c = ccord_store_channel_at(ui->d.store, gid, ci);
      if (!c || c->is_message_request || c->kind != CCORD_CHANNEL_TEXT || !ui_channel_visible(ui, gid, c)) continue;
      bool is_dm = (gid == CCORD_DM_GUILD_ID);
      const char *ch_lbl = is_dm ? ui_dm_display_name(ui, c) : (c->name ? c->name : "channel");
      if (tab == 0) {
        bool muted = ui_channel_muted(ui, c);
        if ((muted || c->unread_count == 0) && c->mention_count == 0) continue;
        inbox_entry *e = &out[count++];
        *e = (inbox_entry){
          .guild_id = gid,
          .channel_id = c->id,
          .message_id = CCORD_ID_NONE,
          .unread_count = c->unread_count,
          .mention_count = c->mention_count,
        };
        if (is_dm) {
          snprintf(e->title, sizeof e->title, "@%.120s", ch_lbl);
        } else {
          snprintf(e->title, sizeof e->title, "%.56s  #%.64s", g && g->name ? g->name : "Server", ch_lbl);
        }
        size_t mc = ccord_store_message_count(ui->d.store, c->id);
        const ccord_message *preview_m = nullptr;
        ccord_id jump_id = CCORD_ID_NONE;
        for (size_t mi = 0; mi < mc; mi++) {
          const ccord_message *m = ccord_store_message_at(ui->d.store, c->id, mi);
          if (!m || ccord_message_is_ephemeral(m)) continue;
          preview_m = m;
          if (jump_id == CCORD_ID_NONE && (c->last_read_id == CCORD_ID_NONE || m->id > c->last_read_id)) {
            jump_id = m->id;
          }
        }
        e->message_id = jump_id != CCORD_ID_NONE ? jump_id : (preview_m ? preview_m->id : CCORD_ID_NONE);
        if (preview_m) {
          const char *aname = is_dm ? ui_dm_display_name(ui, c) : ui_member_display_name(ui, gid, preview_m->author_id);
          const ccord_user *au = ccord_store_user(ui->d.store, preview_m->author_id);
          if (is_dm && au && au->name && au->name[0]) aname = au->name;
          char body[160];
          size_t bl = ccord_message_preview(preview_m, body, sizeof body);
          bl = ui_streamer_mask_text(ui, body, bl);
          snprintf(e->subtitle, sizeof e->subtitle, "%.40s: %.140s", aname ? aname : "User", bl > 0 ? body : "Message");
        } else if (c->mention_count > 0) {
          snprintf(e->subtitle, sizeof e->subtitle, "%u unread mention%s",
                   (unsigned)c->mention_count, c->mention_count == 1u ? "" : "s");
        } else {
          snprintf(e->subtitle, sizeof e->subtitle, "%u unread message%s",
                   (unsigned)c->unread_count, c->unread_count == 1u ? "" : "s");
        }
      } else {
        size_t mc = ccord_store_message_count(ui->d.store, c->id);
        size_t matched_in_ch = 0;
        for (size_t mi = mc; mi > 0 && count < cap; mi--) {
          const ccord_message *m = ccord_store_message_at(ui->d.store, c->id, mi - 1u);
          if (!inbox_message_mentions_self(ui, gid, m)) continue;
          matched_in_ch++;
          inbox_entry *e = &out[count++];
          *e = (inbox_entry){
            .guild_id = gid,
            .channel_id = c->id,
            .message_id = m->id,
            .unread_count = c->unread_count,
            .mention_count = c->mention_count,
          };
          if (is_dm) {
            snprintf(e->title, sizeof e->title, "@%.120s", ch_lbl);
          } else {
            snprintf(e->title, sizeof e->title, "%.56s  #%.64s", g && g->name ? g->name : "Server", ch_lbl);
          }
          const char *aname = is_dm ? ui_dm_display_name(ui, c) : ui_member_display_name(ui, gid, m->author_id);
          const ccord_user *au = ccord_store_user(ui->d.store, m->author_id);
          if (is_dm && au && au->name && au->name[0]) aname = au->name;
          char body[160];
          size_t bl = ccord_message_preview(m, body, sizeof body);
          bl = ui_streamer_mask_text(ui, body, bl);
          snprintf(e->subtitle, sizeof e->subtitle, "%.40s: %.140s", aname ? aname : "User", bl > 0 ? body : "Mentioned you");
        }
        if (matched_in_ch == 0 && c->mention_count > 0 && count < cap) {
          inbox_entry *e = &out[count++];
          *e = (inbox_entry){
            .guild_id = gid,
            .channel_id = c->id,
            .message_id = c->last_message_id,
            .unread_count = c->unread_count,
            .mention_count = c->mention_count,
          };
          if (is_dm) {
            snprintf(e->title, sizeof e->title, "@%.120s", ch_lbl);
          } else {
            snprintf(e->title, sizeof e->title, "%.56s  #%.64s", g && g->name ? g->name : "Server", ch_lbl);
          }
          snprintf(e->subtitle, sizeof e->subtitle, "%u unread mention%s — click to jump",
                   (unsigned)c->mention_count, c->mention_count == 1u ? "" : "s");
        }
      }
    }
  }
  return count;
}

void ui_draw_inbox_popover(ccord_ui *ui, ccord_renderer *r)
{
  ui->ninbox_hits = 0;
  ui->inbox_popover_rect = (ccord_rect){0};
  ui->inbox_mark_all_rect = (ccord_rect){0};
  ui->inbox_tab_rects[0] = (ccord_rect){0};
  ui->inbox_tab_rects[1] = (ccord_rect){0};
  ui->inbox_content_h = 0.0f;
  ui->inbox_thumb = (ccord_rect){0, 0, 0, 0};
  if (!ui->inbox_open) return;

  ccord_rect ir = ui_inbox_rect(ui);
  const ccord_layout_rect *h = &ui->layout.chat_header;
  float w = 420.0f, hgt = 420.0f;
  if (w > ui->win_w - 16.0f && ui->win_w > 32.0f) w = ui->win_w - 16.0f;
  if (hgt > ui->win_h - (h->y + h->h + 14.0f) && ui->win_h > 120.0f) hgt = ui->win_h - (h->y + h->h + 14.0f);
  if (hgt < 160.0f) hgt = 160.0f;
  float x = ir.x + ir.w - w;
  if (x < h->x + 8.0f) x = h->x + 8.0f;
  if (x + w > ui->win_w - 8.0f) x = ui->win_w - w - 8.0f;
  ccord_rect box = {x, h->y + h->h + 6.0f, w, hgt};
  ui->inbox_popover_rect = box;

  uis_popover_frame(ui, r, box);

  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
  ccord_text_draw(r, "Inbox", 5, box.x + 14.0f, box.y + 12.0f, &hs, 0.0f);

  static const char *const k_tabs[2] = {"Unreads", "Mentions"};
  float tx = box.x + 76.0f;
  for (int t = 0; t < 2; t++) {
    ccord_rect tr = {tx, box.y + 8.0f, uis_label_width(ui, r, k_tabs[t]), UIS_H_BTN_SM};
    ui->inbox_tab_rects[t] = tr;
    bool thv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, tr);
    uis_tab(ui, r, tr, k_tabs[t], (uis_state){.hover = thv, .selected = ui->inbox_tab == t}, "inboxtab", (uint64_t)t);
    tx += tr.w + UIS_SP_1;
  }

  inbox_entry entries[UI_INBOX_HIT_MAX];
  size_t nentries = collect_inbox_entries(ui, ui->inbox_tab, entries, UI_INBOX_HIT_MAX);

  bool has_ackable = false;
  for (size_t i = 0; i < nentries; i++) {
    if (entries[i].unread_count > 0 || entries[i].mention_count > 0) {
      has_ackable = true;
      break;
    }
  }
  const char *malbl = "Mark All Read";
  float maw = uis_label_width(ui, r, malbl);
  ccord_rect mar = {box.x + box.w - maw - 12.0f, box.y + 8.0f, maw, UIS_H_BTN_SM};
  ui->inbox_mark_all_rect = mar;
  bool mahv = has_ackable && ui->mouse_in && ui_in_rect(ui->mx, ui->my, mar);
  uis_button(ui, r, mar, malbl, UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){.hover = mahv, .disabled = !has_ackable},
             "inboxall", 0);

  uis_divider(ui, r, box.x, box.y + 44.0f, box.w);

  float list_x = box.x + 10.0f;
  float list_y = box.y + 52.0f;
  float list_w = box.w - 20.0f;
  float list_h = box.h - 60.0f;
  float card_h = 62.0f, gap = 6.0f;
  ui->inbox_content_h = nentries > 0 ? (float)nentries * (card_h + gap) - gap : 0.0f;
  float max_s = ui->inbox_content_h > list_h ? ui->inbox_content_h - list_h : 0.0f;
  ui->inbox_max = max_s;
  if (ui->inbox_scroll > max_s) ui->inbox_scroll = max_s;
  if (ui->inbox_scroll < 0.0f) ui->inbox_scroll = 0.0f;

  ccord_push_clip(r, (ccord_rect){list_x, list_y, list_w, list_h});
  if (nentries == 0) {
    const char *empty = (ui->inbox_tab == 0) ? "You're all caught up! No unread channels."
                                             : "No recent mentions.";
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    ccord_vec2 et = ccord_text_measure(r, empty, strlen(empty), &es, 0.0f);
    ccord_text_draw(r, empty, strlen(empty), list_x + (list_w - et.x) * 0.5f, list_y + (list_h - et.y) * 0.45f, &es, 0.0f);
  } else {
    ccord_text_style cts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style css = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_secondary));
    for (size_t i = 0; i < nentries; i++) {
      float cy = list_y + (float)i * (card_h + gap) - ui->inbox_scroll;
      if (cy + card_h < list_y || cy > list_y + list_h) continue;
      ccord_rect card = {list_x, cy, list_w, card_h};
      ccord_rect ack_r = {0};
      bool can_ack = entries[i].unread_count > 0 || entries[i].mention_count > 0;
      if (can_ack) {
        ack_r = (ccord_rect){card.x + card.w - 84.0f, card.y + 4.0f, 76.0f, UIS_H_BTN_SM};
      }
      // Only fully visible cards are clickable; clipped ones must be scrolled into view first.
      if (cy >= list_y && cy + card_h <= list_y + list_h && ui->ninbox_hits < UI_INBOX_HIT_MAX) {
        ui->inbox_hits[ui->ninbox_hits++] = (ui_inbox_hit){
          .card_rect = card,
          .ack_rect = ack_r,
          .guild_id = entries[i].guild_id,
          .channel_id = entries[i].channel_id,
          .message_id = entries[i].message_id,
        };
      }
      bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, card);
      bool ahv = can_ack && ui->mouse_in && ui_in_rect(ui->mx, ui->my, ack_r);
      uis_card(ui, r, card);
      uis_list_row_fill(ui, r, card, (uis_state){.hover = chv && !ahv}, "inboxcard", (uint64_t)i);

      float right_stop = can_ack ? (ack_r.x - 8.0f) : (card.x + card.w - 10.0f);
      if (entries[i].mention_count > 0) {
        char pbuf[24];
        snprintf(pbuf, sizeof pbuf, "@%u", (unsigned)entries[i].mention_count);
        ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, COL(text_on_brand));
        ccord_vec2 pt = ccord_text_measure(r, pbuf, strlen(pbuf), &ps, 0.0f);
        ccord_rect pr_pill = {right_stop - (pt.x + 12.0f), card.y + 8.0f, pt.x + 12.0f, 18.0f};
        ccord_draw_rrect(r, pr_pill, 9.0f, COL(danger));
        ccord_text_draw(r, pbuf, strlen(pbuf), pr_pill.x + 6.0f, pr_pill.y + (pr_pill.h - pt.y) * 0.5f, &ps, 0.0f);
        right_stop = pr_pill.x - 6.0f;
      } else if (entries[i].unread_count > 0) {
        char pbuf[24];
        snprintf(pbuf, sizeof pbuf, "%u", (unsigned)entries[i].unread_count);
        ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, COL(text_on_brand));
        ccord_vec2 pt = ccord_text_measure(r, pbuf, strlen(pbuf), &ps, 0.0f);
        ccord_rect pr_pill = {right_stop - maxf(pt.x + 10.0f, 18.0f), card.y + 8.0f, maxf(pt.x + 10.0f, 18.0f), 18.0f};
        ccord_draw_rrect(r, pr_pill, 9.0f, COL(brand));
        ccord_text_draw(r, pbuf, strlen(pbuf), pr_pill.x + (pr_pill.w - pt.x) * 0.5f,
                        pr_pill.y + (pr_pill.h - pt.y) * 0.5f, &ps, 0.0f);
        right_stop = pr_pill.x - 6.0f;
      }

      float avail_t = right_stop - (card.x + 10.0f);
      if (avail_t > 24.0f) {
        ccord_text_draw_ellipsis(r, entries[i].title, strlen(entries[i].title), card.x + 10.0f, card.y + 8.0f, avail_t, &cts);
      }
      ccord_text_draw_ellipsis(r, entries[i].subtitle, strlen(entries[i].subtitle),
                               card.x + 10.0f, card.y + 34.0f, card.w - 20.0f, &css);

      if (can_ack) {
        uis_button(ui, r, ack_r, "Mark Read", UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){.hover = ahv}, "inboxack", (uint64_t)i);
      }
    }
  }
  ccord_pop_clip(r);
  ui->inbox_thumb = ui_draw_list_scrollbar(ui, r, (ccord_rect){list_x, list_y, list_w, list_h},
                                           ui->inbox_content_h, ui->inbox_scroll, "inboxscroll", 0);
}

void ui_draw_shortcuts_modal(ccord_ui *ui, ccord_renderer *r)
{
  ui->shortcuts_modal_rect = (ccord_rect){0};
  ui->shortcuts_close_rect = (ccord_rect){0};
  if (!ui->shortcuts_open) return;

  uis_tokens tk = uis_tokens_of(ui);
  ccord_rect box = uis_modal_box(ui, UIS_MODAL_W_LG + 20.0f, 420.0f);
  ui->shortcuts_modal_rect = box;
  uis_modal_frame(ui, r, box);

  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_draw(r, "Keyboard Shortcuts", 18, box.x + 22.0f, box.y + 16.0f, &ts, 0.0f);
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
  ccord_text_draw(r, "Navigate servers, channels, unreads, and chat without leaving the keyboard", 74,
                  box.x + 22.0f, box.y + 40.0f, &ss, 0.0f);

  ccord_rect close_r = {box.x + box.w - UIS_SP_1 - UIS_ICON_BTN - UIS_SP_2, box.y + 12.0f, UIS_ICON_BTN, UIS_ICON_BTN};
  ui->shortcuts_close_rect = close_r;
  bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, close_r);
  ccord_color cg = uis_icon_button(ui, r, close_r, (uis_state){.hover = chv}, "scclose", 0);
  ui_icon_close(r, close_r.x + close_r.w * 0.5f, close_r.y + close_r.h * 0.5f, 13.0f, cg);

  uis_divider(ui, r, box.x, box.y + 64.0f, box.w);

  static const struct {
    const char *keys;
    const char *desc;
  } k_shortcuts[] = {
    {"Ctrl + K", "Quick Switcher"},
    {"Ctrl + /", "Keyboard Shortcuts"},
    {"Ctrl + I", "Toggle Inbox"},
    {"Ctrl + F", "Search Messages"},
    {"Ctrl + P", "Pinned Messages"},
    {"Alt + Left / Right", "Navigate Back / Forward"},
    {"Alt + Up / Down", "Previous / Next Channel"},
    {"Ctrl + Alt + Up / Down", "Previous / Next Server"},
    {"Alt + Shift + Up / Down", "Previous / Next Unread"},
    {"Ctrl + Shift + Alt + Up / Down", "Previous / Next Mention"},
    {"Up (empty composer)", "Edit Last Sent Message"},
    {"Esc", "Close Popover / Mark Read"},
    {"Ctrl + B / I / U", "Bold / Italic / Underline"},
    {"Ctrl + Shift + S / Ctrl + E", "Strikethrough / Inline Code"},
  };
  size_t total = sizeof k_shortcuts / sizeof k_shortcuts[0];
  size_t half = (total + 1u) / 2u;
  float col_w = (box.w - 52.0f) * 0.5f;
  float y0 = box.y + 78.0f;
  float row_h = 44.0f;

  ccord_text_style ks = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
  ccord_text_style ds = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_secondary));
  for (size_t i = 0; i < total; i++) {
    size_t col = i / half;
    size_t row = i % half;
    float rx = box.x + 20.0f + (float)col * (col_w + 12.0f);
    float ry = y0 + (float)row * row_h;
    if (ry + row_h > box.y + box.h - 8.0f) continue;
    ccord_text_draw_ellipsis(r, k_shortcuts[i].desc, strlen(k_shortcuts[i].desc), rx, ry + 2.0f, col_w, &ds);
    ccord_vec2 kt = ccord_text_measure(r, k_shortcuts[i].keys, strlen(k_shortcuts[i].keys), &ks, 0.0f);
    float kw = (kt.x + 14.0f < col_w) ? (kt.x + 14.0f) : col_w;
    ccord_rect cap_r = {rx, ry + 20.0f, kw, 20.0f};
    uis_plate(ui, r, cap_r, tk.r_control, COL(bg_tertiary));
    ccord_text_draw_ellipsis(r, k_shortcuts[i].keys, strlen(k_shortcuts[i].keys),
                             cap_r.x + 7.0f, cap_r.y + (cap_r.h - kt.y) * 0.5f, kw - 12.0f, &ks);
  }
}

