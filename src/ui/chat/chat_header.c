// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

// ---------------------------------------------------------------- chat header

// A bookmark ribbon: rectangle body with a notched (swallowtail) bottom, drawn from primitives.
static void ui_icon_bookmark(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float w = size * 0.62f, h = size * 0.86f;
  float x = cx - w * 0.5f, y = cy - h * 0.5f;
  float notch = h * 0.28f;
  ccord_draw_rect(r, (ccord_rect){x, y, w, h - notch}, c);
  ccord_draw_triangle(r, (ccord_vec2){x, y + h - notch}, (ccord_vec2){x + w * 0.5f, y + h - notch},
                      (ccord_vec2){x, y + h}, c);
  ccord_draw_triangle(r, (ccord_vec2){x + w, y + h - notch}, (ccord_vec2){x + w * 0.5f, y + h - notch},
                      (ccord_vec2){x + w, y + h}, c);
}

void ui_draw_chat_header(ccord_ui *ui, ccord_renderer *r)
{
  const ccord_layout_rect *h = &ui->layout.chat_header;
  ccord_draw_rect(r, ui_rect(*h), COL(bg_primary));
  ccord_draw_rect(r, (ccord_rect){h->x, h->y + h->h - 1.0f, h->w, 1.0f}, COL(border_subtle));
  bool is_1to1_dm_panel = ui_panel_is_profile(ui);
  uint32_t tot_mentions = 0;
  uint32_t tot_unreads = ui_guild_unread(ui, CCORD_DM_GUILD_ID, &tot_mentions);
  {
    size_t ng_hdr = ccord_store_guild_count(ui->d.store);
    for (size_t gi = 0; gi < ng_hdr; gi++) {
      const ccord_guild *gg = ccord_store_guild_at(ui->d.store, gi);
      if (!gg) continue;
      uint32_t gm = 0;
      tot_unreads += ui_guild_unread(ui, gg->id, &gm);
      tot_mentions += gm;
    }
  }
  ccord_rect db = ui_drawer_btn_rect(ui);
  if (db.w > 0.0f) {
    ccord_color dic = uis_icon_button(ui, r, db, (uis_state){.selected = ui->left_drawer_open}, "hdrib", 5);
    float dcx = db.x + db.w * 0.5f, dcy = db.y + db.h * 0.5f;
    for (int bi = -1; bi <= 1; bi++) {
      ccord_draw_rrect(r, (ccord_rect){dcx - 8.0f, dcy + (float)bi * 5.0f - 1.0f, 16.0f, 2.0f}, 1.0f, dic);
    }
    if (tot_mentions > 0) {
      char blbl[16];
      if (tot_mentions > 99u) snprintf(blbl, sizeof blbl, "99+");
      else snprintf(blbl, sizeof blbl, "%u", (unsigned)tot_mentions);
      ccord_text_style bst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 5.0f, COL(text_on_brand));
      ccord_vec2 bt = ccord_text_measure(r, blbl, strlen(blbl), &bst, 0.0f);
      float bh = 14.0f, bw = maxf(bt.x + 8.0f, 14.0f);
      float bx = db.x + db.w - bw + 2.0f, by = db.y + 1.0f;
      ccord_draw_rrect(r, (ccord_rect){bx, by, bw, bh}, bh * 0.5f, COL(danger));
      ccord_text_draw(r, blbl, strlen(blbl), bx + (bw - bt.x) * 0.5f, by + (bh - bt.y) * 0.5f, &bst, 0.0f);
    } else if (tot_unreads > 0) {
      ccord_draw_circle(r, db.x + db.w - 6.0f, db.y + 6.0f, 3.5f, COL(brand));
    }
  }
  ccord_rect mb = ui_members_btn_rect(ui);
  if (mb.w > 0.0f) {
    bool hover = ui->hover_members_btn && !ui->menu.open && !ui->settings_open;
    bool hidden = ui->mobile_layout ? !ui->right_drawer_open : ui_panel_hidden(ui);
    // Shown panel: the button reads as active (selected fill); hidden: a plain secondary glyph.
    ccord_color ic = uis_icon_button(ui, r, mb, (uis_state){.hover = hover, .selected = !hidden}, "hdrib", 0);
    if (!hidden) ic = COL(text_header);
    if (is_1to1_dm_panel) {
      ui_icon_user_profile(r, mb.x + mb.w * 0.5f, mb.y + mb.h * 0.5f, 22.0f, ic, COL(bg_primary));
    } else {
      ui_icon_members(r, mb.x + mb.w * 0.5f, mb.y + mb.h * 0.5f, 24.0f, ic, COL(bg_primary));
    }
  }
  ccord_rect sr = ui_search_rect(ui);
  ccord_color sic = uis_icon_button(ui, r, sr, (uis_state){.hover = ui->hover_search && !ui->go_live_open, .selected = ui->search_open},
                                    "hdrib", 1);
  ui_icon_search(r, sr.x + sr.w * 0.5f, sr.y + sr.h * 0.5f, 18.0f, sic);
  ccord_rect pr = ui_pin_rect(ui);
  ccord_color pic = uis_icon_button(ui, r, pr, (uis_state){.hover = ui->hover_pin && !ui->go_live_open, .selected = ui->pins_open},
                                    "hdrib", 2);
  ui_icon_pin(r, pr.x + pr.w * 0.5f, pr.y + pr.h * 0.5f, 18.0f, pic);
  ccord_rect ir = ui_inbox_rect(ui);
  if (ir.w > 0.0f) {
    ccord_color iic = uis_icon_button(ui, r, ir, (uis_state){.hover = ui->hover_inbox && !ui->go_live_open, .selected = ui->inbox_open},
                                      "hdrib", 3);
    ui_icon_inbox(r, ir.x + ir.w * 0.5f, ir.y + ir.h * 0.5f, 18.0f, iic, COL(bg_primary));
    if (tot_mentions > 0) {
      char blbl[16];
      if (tot_mentions > 99u) snprintf(blbl, sizeof blbl, "99+");
      else snprintf(blbl, sizeof blbl, "%u", (unsigned)tot_mentions);
      ccord_text_style bst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 5.0f, COL(text_on_brand));
      ccord_vec2 bt = ccord_text_measure(r, blbl, strlen(blbl), &bst, 0.0f);
      float bh = 14.0f, bw = maxf(bt.x + 8.0f, 14.0f);
      float bx = ir.x + ir.w - bw + 2.0f, by = ir.y + 1.0f;
      ccord_draw_rrect(r, (ccord_rect){bx, by, bw, bh}, bh * 0.5f, COL(danger));
      ccord_text_draw(r, blbl, strlen(blbl), bx + (bw - bt.x) * 0.5f, by + (bh - bt.y) * 0.5f, &bst, 0.0f);
    } else if (tot_unreads > 0) {
      ccord_draw_circle(r, ir.x + ir.w - 6.0f, ir.y + 6.0f, 3.5f, COL(brand));
    }
  }
  ccord_rect bmr = ui_saved_messages_rect(ui);
  if (bmr.w > 0.0f) {
    ccord_color bic = uis_icon_button(
        ui, r, bmr, (uis_state){.hover = ui->hover_saved_msgs && !ui->go_live_open, .selected = ui->saved_popover_open}, "hdrib", 4);
    ui_icon_bookmark(r, bmr.x + bmr.w * 0.5f, bmr.y + bmr.h * 0.5f, 18.0f, bic);
  }
  float hdr_leftmost = bmr.w > 0.0f ? bmr.x : (ir.w > 0.0f ? ir.x : pr.x);
  if (ui->account_view.demo_chip_visible && !(ui->mobile_layout && ui->win_w < 500.0f)) {
    ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_secondary));
    const char *label = "Demo data";
    ccord_vec2 ct = ccord_text_measure(r, label, strlen(label), &cs, 0.0f);
    float right = hdr_leftmost - 8.0f;
    ccord_rect chip = {right - (ct.x + 18.0f), h->y + (h->h - 22.0f) * 0.5f, ct.x + 18.0f, 22.0f};
    ccord_draw_rrect(r, chip, 5.0f, COL(bg_tertiary));
    ccord_text_draw(r, label, strlen(label), chip.x + 9.0f, chip.y + (chip.h - ct.y) * 0.5f, &cs, 0.0f);
    hdr_leftmost = chip.x;
  }
  float left_x = db.w > 0.0f ? (db.x + db.w + 4.0f) : (h->x + 16.0f);
  ui->gdm_add_btn_rect = (ccord_rect){0, 0, 0, 0};
  ui->gdm_edit_btn_rect = (ccord_rect){0, 0, 0, 0};
  ui->dm_requests_tab_rects[0] = (ccord_rect){0, 0, 0, 0};
  ui->dm_requests_tab_rects[1] = (ccord_rect){0, 0, 0, 0};
  float cy = h->y + h->h * 0.5f;
  if (ui_is_dm_requests_view(ui)) {
    ui->add_friend_btn_rect = (ccord_rect){0, 0, 0, 0};
    ui_icon_inbox(r, left_x + 10.0f, cy, 20.0f, COL(text_muted), COL(bg_primary));
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    float ty = cy - ui_line_h(ui, ns.size) * 0.5f;
    float nw = ccord_text_draw_ellipsis(r, "Message Requests", 16, left_x + 30.0f, ty, maxf(hdr_leftmost - (left_x + 36.0f), 60.0f), &ns);
    float dx = left_x + 30.0f + nw + 16.0f;
    ccord_draw_rect(r, (ccord_rect){dx, cy - 12.0f, 1.0f, 24.0f}, COL(divider));
    uint32_t req_cnt = 0, spam_cnt = 0;
    size_t dmn = ccord_store_channel_count(ui->d.store, CCORD_DM_GUILD_ID);
    for (size_t di = 0; di < dmn; di++) {
      const ccord_channel *mc = ccord_store_channel_at(ui->d.store, CCORD_DM_GUILD_ID, di);
      if (!mc || !mc->is_message_request) continue;
      if (mc->is_spam) spam_cnt++;
      else req_cnt++;
    }
    char tab0[32], tab1[32];
    snprintf(tab0, sizeof tab0, "Requests (%u)", (unsigned)req_cnt);
    snprintf(tab1, sizeof tab1, "Spam (%u)", (unsigned)spam_cnt);
    const char *rtabs[2] = {tab0, tab1};
    float tx = dx + 16.0f;
    for (int t = 0; t < 2; t++) {
      bool active = (ui->dm_requests_tab == t);
      ccord_text_style ts = ui_style(ui, active ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small),
                                     active ? COL(text_header) : COL(text_secondary));
      ccord_vec2 tt = ccord_text_measure(r, rtabs[t], strlen(rtabs[t]), &ts, 0.0f);
      ccord_rect tab_r = {tx, cy - 13.0f, tt.x + 20.0f, 26.0f};
      ui->dm_requests_tab_rects[t] = tab_r;
      ui_row_state tab_rs = ui_row_state_of(ui, "reqtab", (uint64_t)t, ui->mouse_in && ui_in_rect(ui->mx, ui->my, tab_r),
                                            active, COL(bg_primary));
      ui_row_state_fill(ui, r, &tab_rs, tab_r, 4.0f);
      ccord_text_draw(r, rtabs[t], strlen(rtabs[t]), tab_r.x + 10.0f, tab_r.y + (tab_r.h - tt.y) * 0.5f, &ts, 0.0f);
      tx += tab_r.w + 8.0f;
    }
    return;
  }
  if (ui_is_dm_friends_view(ui)) {
    ui_icon_members(r, left_x + 10.0f, cy, 20.0f, COL(text_muted), COL(bg_primary));
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    float ty = cy - ui_line_h(ui, ns.size) * 0.5f;
    float nw = ccord_text_draw_ellipsis(r, "Friends", 7, left_x + 30.0f, ty, 120.0f, &ns);
    float dx = left_x + 30.0f + nw + 16.0f;
    ccord_draw_rect(r, (ccord_rect){dx, cy - 12.0f, 1.0f, 24.0f}, COL(divider));
    static const char *const k_tabs[4] = {"Online", "All", "Pending", "Blocked"};
    float tx = dx + 16.0f;
    for (int t = 0; t < 4; t++) {
      bool active = (ui->dm_friends_tab == t);
      ccord_text_style ts = ui_style(ui, active ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small),
                                     active ? COL(text_header) : COL(text_secondary));
      ccord_vec2 tt = ccord_text_measure(r, k_tabs[t], strlen(k_tabs[t]), &ts, 0.0f);
      ccord_rect tab_r = {tx, cy - 13.0f, tt.x + 20.0f, 26.0f};
      ui->dm_friends_tab_rects[t] = tab_r;
      ui_row_state tab_rs = ui_row_state_of(ui, "ftab", (uint64_t)t, ui->mouse_in && ui_in_rect(ui->mx, ui->my, tab_r), active,
                                            COL(bg_primary));
      ui_row_state_fill(ui, r, &tab_rs, tab_r, 4.0f);
      ccord_text_draw(r, k_tabs[t], strlen(k_tabs[t]), tab_r.x + 10.0f, tab_r.y + (tab_r.h - tt.y) * 0.5f, &ts, 0.0f);
      tx += tab_r.w + 8.0f;
    }
    ccord_text_style afs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_on_brand));
    ccord_vec2 aft = ccord_text_measure(r, "Add Friend", 10, &afs, 0.0f);
    ccord_rect af_r = {tx + 4.0f, cy - 13.0f, aft.x + 20.0f, 26.0f};
    ui->add_friend_btn_rect = (ccord_rect){0, 0, 0, 0};
    if (af_r.x + af_r.w < ir.x - 12.0f) {
      ui->add_friend_btn_rect = af_r;
      ccord_draw_rrect(r, af_r, 4.0f, COL(status_online));
      ccord_text_draw(r, "Add Friend", 10, af_r.x + 10.0f, af_r.y + (af_r.h - aft.y) * 0.5f, &afs, 0.0f);
    }
    return;
  }
  ui->add_friend_btn_rect = (ccord_rect){0, 0, 0, 0};
  ui->thread_join_rect = (ccord_rect){0, 0, 0, 0};
  ui->thread_archive_rect = (ccord_rect){0, 0, 0, 0};
  ui->thread_breadcrumb_rect = (ccord_rect){0, 0, 0, 0};
  ui_forum_set_breadcrumb_rect(ui, (ccord_rect){0}, CCORD_ID_NONE);
  const ccord_channel *ch = ui_selected_channel(ui);
  bool voice_stage = false;
  if (ui->stage_channel != CCORD_ID_NONE && !ui_stage_chat_active(ui)) {
    const ccord_channel *sc = ccord_store_channel(ui->d.store, ui->stage_channel);
    if (sc) {
      ch = sc;
      voice_stage = true;
    }
  }
  if (!ch) return;
  bool is_dm = ch->guild_id == CCORD_DM_GUILD_ID && !voice_stage;
  bool group_dm = is_dm && ui_channel_is_group_dm(ch);
  const ccord_user *dm_u = is_dm ? ui_dm_recipient(ui, ch) : nullptr;
  const char *ch_name = is_dm ? ui_dm_display_name(ui, ch) : ch->name;
  bool is_thread_ch = !voice_stage && !is_dm && ccord_discord_type_is_thread(ch->discord_type);
  const ccord_channel *parent_ch = (!voice_stage && !is_dm && ch->parent_id != CCORD_ID_NONE &&
                                    (is_thread_ch || ccord_store_channel_is_forum(ui->d.store, ch->parent_id)))
                                     ? ccord_store_channel(ui->d.store, ch->parent_id)
                                     : nullptr;
  bool is_forum_parent = parent_ch && ccord_store_channel_is_forum(ui->d.store, parent_ch->id);
  if (voice_stage) {
    ui_icon_speaker(r, left_x + 10.0f, cy, 20.0f, COL(text_muted));
  } else if (group_dm) {
    ui_icon_members(r, left_x + 10.0f, cy, 20.0f, COL(text_muted), COL(bg_primary));
  } else if (is_dm) {
    ccord_text_style ats = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 2.0f, COL(text_muted));
    ccord_vec2 att = ccord_text_measure(r, "@", 1, &ats, 0.0f);
    ccord_text_draw(r, "@", 1, left_x + 10.0f - att.x * 0.5f, cy - att.y * 0.5f, &ats, 0.0f);
  } else if (is_forum_parent || ccord_store_channel_is_forum(ui->d.store, ch->id)) {
    ui_icon_forum(r, left_x + 10.0f, cy, 20.0f, COL(text_muted));
  } else {
    ui_icon_hash(r, left_x + 10.0f, cy, 20.0f, COL(text_muted));
  }
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
  float tx = left_x + 30.0f;
  float ty = cy - ui_line_h(ui, ns.size) * 0.5f;
  float nw = 0.0f;
  float hdr_btn_left = hdr_leftmost - 8.0f;
  float title_max_w = ui->mobile_layout ? maxf(hdr_btn_left - tx - 8.0f, 40.0f) : (h->w * 0.5f);
  if (parent_ch && parent_ch->name) {
    ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_secondary));
    float pnw = ccord_text_draw_ellipsis(r, parent_ch->name, strlen(parent_ch->name), tx, ty, h->w * 0.25f, &ps);
    ccord_rect bc_r = {left_x - 4.0f, h->y + 6.0f, (tx - (left_x - 4.0f)) + pnw + 6.0f, h->h - 12.0f};
    ui->thread_breadcrumb_rect = bc_r;
    ui_forum_set_breadcrumb_rect(ui, bc_r, parent_ch->id);
    ui_icon_chevron(r, tx + pnw + 10.0f, cy, 10.0f, ICON_RIGHT, COL(text_muted));
    float post_x = tx + pnw + 20.0f;
    float post_w = ccord_text_draw_ellipsis(r, ch_name, strlen(ch_name), post_x, ty, maxf(hdr_btn_left - post_x - 8.0f, 30.0f), &ns);
    nw = (post_x - tx) + post_w;
    if (is_thread_ch && ch->thread_archived) {
      ccord_text_style abs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
      ccord_vec2 abt = ccord_text_measure(r, "Archived", 8, &abs, 0.0f);
      ccord_rect arch_tag = {tx + nw + 8.0f, cy - 10.0f, abt.x + 12.0f, 20.0f};
      ccord_draw_rrect(r, arch_tag, 4.0f, COL(bg_tertiary));
      ccord_text_draw(r, "Archived", 8, arch_tag.x + 6.0f, arch_tag.y + (arch_tag.h - abt.y) * 0.5f, &abs, 0.0f);
      nw += arch_tag.w + 8.0f;
    }
  } else {
    nw = ccord_text_draw_ellipsis(r, ch_name, strlen(ch_name), tx, ty, title_max_w, &ns);
  }
  if (is_thread_ch) {
    ccord_id self_id = ccord_store_self_id(ui->d.store);
    ccord_id perm_ch = ch->parent_id != CCORD_ID_NONE ? ch->parent_id : ch->id;
    bool can_manage_threads = ccord_store_has_permission(ui->d.store, ch->guild_id, perm_ch, self_id,
                                                         CCORD_PERM_MANAGE_THREADS);
    bool can_toggle_arch = can_manage_threads || (ch->thread_archived && !ch->thread_locked);
    if (can_toggle_arch) {
      const char *albl = ch->thread_archived ? "Unarchive" : "Archive";
      ccord_text_style abs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_secondary));
      ccord_vec2 abt = ccord_text_measure(r, albl, strlen(albl), &abs, 0.0f);
      float bw = abt.x + 18.0f;
      ccord_rect ar = {hdr_btn_left - bw, cy - 12.0f, bw, 24.0f};
      if (ar.x > tx + nw + 12.0f) {
        ui->thread_archive_rect = ar;
        bool ahv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ar);
        ccord_draw_rrect(r, ar, 4.0f, ahv ? COL(bg_item_hover) : COL(bg_tertiary));
        ccord_text_draw(r, albl, strlen(albl), ar.x + 9.0f, ar.y + (ar.h - abt.y) * 0.5f, &abs, 0.0f);
        hdr_btn_left = ar.x - 6.0f;
      }
    }
    if (!ch->thread_archived) {
      const char *jlbl = ch->thread_joined ? "Joined" : "Join";
      ccord_text_style jbs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f,
                                      ch->thread_joined ? COL(text_secondary) : COL(text_on_brand));
      ccord_vec2 jbt = ccord_text_measure(r, jlbl, strlen(jlbl), &jbs, 0.0f);
      float bw = jbt.x + 18.0f;
      ccord_rect jr = {hdr_btn_left - bw, cy - 12.0f, bw, 24.0f};
      if (jr.x > tx + nw + 12.0f) {
        ui->thread_join_rect = jr;
        bool jhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, jr);
        ccord_color jbg = ch->thread_joined ? (jhv ? COL(bg_item_hover) : COL(bg_tertiary)) : COL(brand);
        ccord_draw_rrect(r, jr, 4.0f, jbg);
        ccord_text_draw(r, jlbl, strlen(jlbl), jr.x + 9.0f, jr.y + (jr.h - jbt.y) * 0.5f, &jbs, 0.0f);
        hdr_btn_left = jr.x - 6.0f;
      }
    }
  } else if (group_dm) {
    const char *elbl = "Edit Group";
    ccord_text_style ebs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_secondary));
    ccord_vec2 ebt = ccord_text_measure(r, elbl, strlen(elbl), &ebs, 0.0f);
    float ebw = ebt.x + 16.0f;
    ccord_rect er = {hdr_btn_left - ebw, cy - 12.0f, ebw, 24.0f};
    if (er.x > tx + nw + 12.0f) {
      ui->gdm_edit_btn_rect = er;
      bool ehv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, er);
      ccord_draw_rrect(r, er, 4.0f, ehv ? COL(bg_item_hover) : COL(bg_tertiary));
      ccord_text_draw(r, elbl, strlen(elbl), er.x + 8.0f, er.y + (er.h - ebt.y) * 0.5f, &ebs, 0.0f);
      hdr_btn_left = er.x - 6.0f;
    }
    const char *albl = "Add Member";
    ccord_text_style abs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_on_brand));
    ccord_vec2 abt = ccord_text_measure(r, albl, strlen(albl), &abs, 0.0f);
    float abw = abt.x + 16.0f;
    ccord_rect ar = {hdr_btn_left - abw, cy - 12.0f, abw, 24.0f};
    if (ar.x > tx + nw + 12.0f) {
      ui->gdm_add_btn_rect = ar;
      ccord_draw_rrect(r, ar, 4.0f, COL(brand));
      ccord_text_draw(r, albl, strlen(albl), ar.x + 8.0f, ar.y + (ar.h - abt.y) * 0.5f, &abs, 0.0f);
      hdr_btn_left = ar.x - 6.0f;
    }
  }
  if (ui_stage_mobile_chat_active(ui)) {
    ccord_rect vbtn = ui_stage_chat_btn(ui);
    if (vbtn.w > 0.0f && vbtn.x > tx + nw + 4.0f) {
      bool vhv = ui->hover_voice_back;
      ccord_draw_rrect(r, vbtn, 4.0f, vhv ? COL(bg_item_hover) : COL(brand));
      ccord_text_style vbs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_on_brand));
      const char *vlbl = "< Voice";
      ccord_vec2 vbt = ccord_text_measure(r, vlbl, strlen(vlbl), &vbs, 0.0f);
      ccord_text_draw(r, vlbl, strlen(vlbl), vbtn.x + (vbtn.w - vbt.x) * 0.5f,
                      vbtn.y + (vbtn.h - vbt.y) * 0.5f, &vbs, 0.0f);
      hdr_btn_left = vbtn.x - 6.0f;
    }
  }
  if (is_dm && dm_u) {
    ccord_color dot = COL(status_offline);
    switch (dm_u->presence) {
      case CCORD_PRESENCE_ONLINE: dot = COL(status_online); break;
      case CCORD_PRESENCE_IDLE: dot = COL(status_idle); break;
      case CCORD_PRESENCE_DND: dot = COL(status_dnd); break;
      case CCORD_PRESENCE_OFFLINE: dot = COL(status_offline); break;
    }
    ccord_draw_circle(r, tx + nw + 10.0f, cy, 4.5f, dot);
    nw += 18.0f;
  }
  if (is_dm) ui_call_draw_header_button(ui, r, &hdr_btn_left, cy, tx + nw + 12.0f);
  if (ui->d.contrib && ccord_contrib_action_btn_count(ui->d.contrib, CCORD_ACTBTN_HEADER) > 0) {
    ccord_menu_context hctx = {.target = CCORD_MENU_CHANNEL, .guild_id = ch->guild_id, .channel_id = ch->id};
    float avail_w = hdr_btn_left - (tx + nw + 12.0f);
    if (avail_w > 28.0f) {
      ccord_rect ab = {tx + nw + 12.0f, h->y + (h->h - UIS_ICON_BTN) * 0.5f, avail_w, UIS_ICON_BTN};
      hdr_btn_left -= ui_actbtns_draw_slot(ui, r, CCORD_ACTBTN_HEADER, ab, true, &hctx);
    }
  }
  char gdm_topic[48];
  const char *topic = voice_stage ? "Voice / Stream Stage" : ch->topic;
  if (group_dm && (!topic || !topic[0])) {
    snprintf(gdm_topic, sizeof gdm_topic, "%zu Members", ui_dm_member_count(ch));
    topic = gdm_topic;
  }
  if (topic && topic[0]) {
    float dx = tx + nw + 12.0f;
    float right = hdr_btn_left < (mb.w > 0.0f ? mb.x - 8.0f : h->x + h->w - 16.0f)
                    ? hdr_btn_left
                    : (mb.w > 0.0f ? mb.x - 8.0f : h->x + h->w - 16.0f);
    if (right > dx + 24.0f) {
      ccord_draw_rect(r, (ccord_rect){dx, cy - 12.0f, 1.0f, 24.0f}, COL(divider));
      ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_secondary));
      float tty = cy - ui_line_h(ui, ts.size) * 0.5f;
      ccord_text_draw_ellipsis(r, topic, strlen(topic), dx + 13.0f, tty, right - (dx + 13.0f), &ts);
    }
  }
}

void ui_draw_chat_header_tooltip(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui->hover_members_btn || ui->menu.open || ui->settings_open) return;
  ccord_rect mb = ui_members_btn_rect(ui);
  if (mb.w <= 0.0f) return;
  bool is_1to1_dm = ui_panel_is_profile(ui);
  bool hidden = ui_panel_hidden(ui);
  const char *label = is_1to1_dm ? (hidden ? "Show User Profile" : "Hide User Profile")
                                 : (hidden ? "Show Member List" : "Hide Member List");
  (void)uis_tooltip(ui, r, mb, label, UIS_TIP_BELOW, 1);
}

// The hover bar's Edit button is offered for the messages the context menu's "Edit Message" entry is: your own.
bool ui_message_editable_by_self(const ccord_ui *ui, const ccord_message *m)
{
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!m || self == CCORD_ID_NONE || m->author_id != self) return false;
  if (ccord_message_type_is_system(ccord_message_type(m))) return false;
  if ((ccord_message_flags(m) & (CCORD_MSGFLAG_VOICE_MESSAGE | CCORD_MSGFLAG_IS_COMPONENTS_V2)) != 0) return false;
  if (m->rich && (m->rich->present & (CCORD_RICH_HAS_POLL | CCORD_RICH_HAS_FORWARD)) != 0) return false;
  return true;
}

bool ui_bar_edit_hovered(const ccord_ui *ui)
{
  return ui->mouse_in && !ui->menu.open && !ui->settings_open && ui->hover_bar_edit_rect.w > 0.0f &&
         ui_in_rect(ui->mx, ui->my, ui->hover_bar_edit_rect);
}

// Drawn in the overlay pass (ui.c) so the message area's clip does not cut it off.
void ui_draw_bar_edit_tooltip(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui_bar_edit_hovered(ui)) return;
  (void)uis_tooltip(ui, r, ui->hover_bar_edit_rect, "Edit", UIS_TIP_ABOVE, 1);
}

void ui_draw_timestamp_tooltip(ccord_ui *ui, ccord_renderer *r)
{
  ui->tooltip_text[0] = '\0';
  if (ui->hover_ts_hit < 0 || (size_t)ui->hover_ts_hit >= ui->nts_hits ||
      ui->menu.open || ui->settings_open || ui->pins_open || ui->search_open) {
    return;
  }
  const ui_ts_hit *hit = &ui->ts_hits[ui->hover_ts_hit];
  char full[96];
  if (ccord_timefmt_full(full, sizeof full, hit->ts_ms, ui->use_utc) == 0) return;
  if (hit->is_edited) snprintf(ui->tooltip_text, sizeof ui->tooltip_text, "Edited %s", full);
  else snprintf(ui->tooltip_text, sizeof ui->tooltip_text, "%s", full);

  (void)uis_tooltip(ui, r, hit->rect, ui->tooltip_text, UIS_TIP_ABOVE, 2);
}

// ---------------------------------------------------------------- messages

