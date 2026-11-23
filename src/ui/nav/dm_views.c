// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"
#include <ccord/store/admin.h>
ccord_id cm_find_1on1_dm_channel(const ccord_ui *ui, ccord_id user_id);

void dc_draw_dm_friends_view(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma)
{
  ui->nfriend_row_hits = 0;
  size_t nrel = ccord_store_relationship_count(ui->d.store);
  size_t matched = 0;
  for (size_t i = 0; i < nrel; i++) {
    const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
    const ccord_user *u = rel ? ccord_store_user(ui->d.store, rel->user_id) : nullptr;
    if (dc_friend_matches_tab(rel, u, ui->dm_friends_tab)) matched++;
  }
  static const char *const k_tab_titles[4] = {"ONLINE", "ALL FRIENDS", "PENDING", "BLOCKED"};
  int tab = ui->dm_friends_tab >= 0 && ui->dm_friends_tab < 4 ? ui->dm_friends_tab : 0;
  bool compact = ui->mobile_layout || ma->w < 460.0f;
  float pad_x = compact ? 12.0f : 24.0f;
  float x = ma->x + pad_x;
  float w = maxf(ma->w - 2.0f * pad_x, 80.0f);
  float y = ma->y + (compact ? 10.0f : 20.0f);

  if (ui->mobile_layout) {
    static const char *const k_tabs[4] = {"Online", "All", "Pending", "Blocked"};
    float gap = 4.0f;
    float tw = floorf((w - 3.0f * gap) * 0.25f);
    float tx = x;
    for (int t = 0; t < 4; t++) {
      bool active = (tab == t);
      ccord_rect tab_r = {tx, y, tw, 38.0f};
      ui->dm_friends_tab_rects[t] = tab_r;
      ui_row_state tab_rs = ui_row_state_of(ui, "ftab", (uint64_t)t,
                                            ui->mouse_in && ui_in_rect(ui->mx, ui->my, tab_r), active, COL(bg_primary));
      ui_row_state_fill(ui, r, &tab_rs, tab_r, 6.0f);
      ccord_text_style ts = ui_style(ui, active ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small) - 1.0f,
                                     active ? COL(text_header) : COL(text_secondary));
      ccord_vec2 tt = ccord_text_measure(r, k_tabs[t], strlen(k_tabs[t]), &ts, 0.0f);
      ccord_text_draw_ellipsis(r, k_tabs[t], strlen(k_tabs[t]),
                               tab_r.x + maxf((tab_r.w - tt.x) * 0.5f, 4.0f),
                               tab_r.y + (tab_r.h - tt.y) * 0.5f, maxf(tab_r.w - 8.0f, 16.0f), &ts);
      tx += tw + gap;
    }
    y += 46.0f;
    ccord_draw_rect(r, (ccord_rect){x, y - 4.0f, w, 1.0f}, COL(border_subtle));
  }

  char hdr[96];
  snprintf(hdr, sizeof hdr, "%s \xE2\x80\x94 %zu", k_tab_titles[tab], matched);
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
  if (ui->mobile_layout) {
    ccord_text_style afs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_on_brand));
    ccord_vec2 aft = ccord_text_measure(r, "Add Friend", 10, &afs, 0.0f);
    float afw = aft.x + 24.0f;
    ccord_rect af_r = {x + w - afw, y, afw, 38.0f};
    ui->add_friend_btn_rect = af_r;
    ccord_draw_rrect(r, af_r, 6.0f, COL(status_online));
    ccord_text_draw(r, "Add Friend", 10, af_r.x + (af_r.w - aft.x) * 0.5f, af_r.y + (af_r.h - aft.y) * 0.5f, &afs, 0.0f);
    ccord_text_draw_ellipsis(r, hdr, strlen(hdr), x, y + (38.0f - ui_line_h(ui, hs.size)) * 0.5f,
                             maxf(af_r.x - x - 8.0f, 40.0f), &hs);
    y += 46.0f;
  } else {
    ccord_text_draw_ellipsis(r, hdr, strlen(hdr), x, y, w, &hs);
    y += 28.0f;
  }
  // The rows scroll under the fixed header; off-fold friends stay reachable via wheel + thumb.
  ccord_rect view = {ma->x, y, ma->w, ma->y + ma->h - y};
  if (view.h < 0.0f) view.h = 0.0f;
  float row_pitch = ui->mobile_layout ? 64.0f : 56.0f;
  float row_h = ui->mobile_layout ? 60.0f : 52.0f;
  ui->dm_friends_content_h = (float)matched * row_pitch;
  float max_s = maxf(ui->dm_friends_content_h - view.h, 0.0f);
  ui->dm_friends_scroll = clampf(ui->dm_friends_scroll, 0.0f, max_s);
  ccord_push_clip(r, view);
  float ry = view.y - ui->dm_friends_scroll;
  for (size_t i = 0; i < nrel; i++) {
    const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
    const ccord_user *u = rel ? ccord_store_user(ui->d.store, rel->user_id) : nullptr;
    if (!dc_friend_matches_tab(rel, u, tab)) continue;
    ccord_rect row_r = {x, ry + 2.0f, w, row_h};
    ccord_rect sep_r = {x, ry, w, 1.0f};
    ry += row_pitch;
    if (row_r.y + row_r.h < view.y || row_r.y > view.y + view.h) continue;
    bool fully_visible = row_r.y >= view.y && row_r.y + row_r.h <= view.y + view.h;
    ccord_draw_rect(r, sep_r, COL(border_subtle));
    ui_state_fill(ui, r, row_r, 6.0f, "frow", rel->user_id, ui->mouse_in && ui_in_rect(ui->mx, ui->my, row_r), false);
    ui_draw_avatar(ui, r, u, x + 24.0f, row_r.y + row_h * 0.5f, 36.0f, COL(bg_primary), false);
    const char *fnick = ui_friend_nickname(ui, rel->user_id);
    const char *dname = (fnick && fnick[0]) ? fnick
                        : ((u && u->display_name && u->display_name[0]) ? u->display_name
                        : ((u && u->name && u->name[0]) ? u->name : "Unknown User"));
    ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_secondary));
    ccord_rect msg_r = {0, 0, 0, 0}, acc_r = {0, 0, 0, 0}, rem_r = {0, 0, 0, 0};
    float btn_left = x + w - 8.0f;
    float bh = uis_btn_h_for(ui, 28.0f);
    float by = row_r.y + (row_r.h - bh) * 0.5f;
    float br = bh * 0.5f;
    if (rel->kind == CCORD_RELATIONSHIP_PENDING_INCOMING) {
      float aw = ui->mobile_layout ? 70.0f : 64.0f;
      acc_r = (ccord_rect){x + w - 2.0f * aw - 16.0f, by, aw, bh};
      rem_r = (ccord_rect){x + w - aw - 8.0f, by, aw, bh};
      btn_left = acc_r.x;
      ccord_draw_rrect(r, acc_r, br, COL(status_online));
      ccord_draw_rrect(r, rem_r, br, COL(bg_secondary));
      ccord_text_style abs_st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_on_brand));
      ccord_vec2 at = ccord_text_measure(r, "Accept", 6, &abs_st, 0.0f);
      ccord_vec2 rt = ccord_text_measure(r, "Ignore", 6, &bs, 0.0f);
      ccord_text_draw(r, "Accept", 6, acc_r.x + (acc_r.w - at.x) * 0.5f, acc_r.y + (acc_r.h - at.y) * 0.5f, &abs_st, 0.0f);
      ccord_text_draw(r, "Ignore", 6, rem_r.x + (rem_r.w - rt.x) * 0.5f, rem_r.y + (rem_r.h - rt.y) * 0.5f, &bs, 0.0f);
    } else if (rel->kind == CCORD_RELATIONSHIP_PENDING_OUTGOING || rel->kind == CCORD_RELATIONSHIP_BLOCKED) {
      float rw = ui->mobile_layout ? 76.0f : 68.0f;
      rem_r = (ccord_rect){x + w - rw - 8.0f, by, rw, bh};
      btn_left = rem_r.x;
      ccord_draw_rrect(r, rem_r, br, COL(bg_secondary));
      const char *lbl = (rel->kind == CCORD_RELATIONSHIP_BLOCKED) ? "Unblock" : "Cancel";
      ccord_vec2 bt = ccord_text_measure(r, lbl, strlen(lbl), &bs, 0.0f);
      ccord_text_draw(r, lbl, strlen(lbl), rem_r.x + (rem_r.w - bt.x) * 0.5f, rem_r.y + (rem_r.h - bt.y) * 0.5f, &bs, 0.0f);
    } else {
      float rsz = bh;
      float mw = ui->mobile_layout ? 74.0f : 68.0f;
      rem_r = (ccord_rect){x + w - rsz - 8.0f, by, rsz, rsz};
      msg_r = (ccord_rect){rem_r.x - 8.0f - mw, by, mw, bh};
      btn_left = msg_r.x;
      ccord_draw_rrect(r, msg_r, br, COL(bg_secondary));
      ccord_draw_rrect(r, rem_r, br, COL(bg_secondary));
      ccord_vec2 bt = ccord_text_measure(r, "Message", 7, &bs, 0.0f);
      ccord_text_draw(r, "Message", 7, msg_r.x + (msg_r.w - bt.x) * 0.5f, msg_r.y + (msg_r.h - bt.y) * 0.5f, &bs, 0.0f);
      ui_icon_close(r, rem_r.x + rem_r.w * 0.5f, rem_r.y + rem_r.h * 0.5f, ui->mobile_layout ? 13.0f : 11.0f, COL(text_muted));
    }
    float text_w = maxf(btn_left - (x + 52.0f) - 8.0f, 40.0f);
    float ty0 = row_r.y + (ui->mobile_layout ? 12.0f : 8.0f);
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    ccord_text_draw_ellipsis(r, dname, strlen(dname), x + 52.0f, ty0, text_w, &ns);
    const char *sub = (u && u->status_text && u->status_text[0]) ? u->status_text
                      : (rel->kind == CCORD_RELATIONSHIP_PENDING_INCOMING ? "Incoming Friend Request"
                      : (rel->kind == CCORD_RELATIONSHIP_PENDING_OUTGOING ? "Outgoing Friend Request"
                      : (rel->kind == CCORD_RELATIONSHIP_BLOCKED ? "Blocked"
                      : (u && u->presence == CCORD_PRESENCE_ONLINE ? "Online"
                      : (u && u->presence == CCORD_PRESENCE_IDLE ? "Idle"
                      : (u && u->presence == CCORD_PRESENCE_DND ? "Do Not Disturb" : "Offline"))))));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    ccord_text_draw_ellipsis(r, sub, strlen(sub), x + 52.0f, ty0 + 20.0f, text_w, &ss);
    // Only fully visible rows are clickable: a half-clipped row must not catch hits outside the fold.
    if (fully_visible && ui->nfriend_row_hits < 32) {
      ui->friend_row_hits[ui->nfriend_row_hits].row_rect = row_r;
      ui->friend_row_hits[ui->nfriend_row_hits].msg_rect = msg_r.w > 0.0f ? msg_r : row_r;
      ui->friend_row_hits[ui->nfriend_row_hits].accept_rect = acc_r;
      ui->friend_row_hits[ui->nfriend_row_hits].remove_rect = rem_r;
      ui->friend_row_hits[ui->nfriend_row_hits].user_id = rel->user_id;
      ui->friend_row_hits[ui->nfriend_row_hits].kind = rel->kind;
      ui->nfriend_row_hits++;
    }
    if (fully_visible) dc_record_user_hit(ui, (ccord_rect){x, row_r.y, 50.0f, row_r.h}, rel->user_id);
  }
  ccord_pop_clip(r);
  ui->dm_friends_thumb = ui_draw_list_scrollbar(ui, r, view, ui->dm_friends_content_h, ui->dm_friends_scroll,
                                                "friendscroll", 0);
}
void dc_draw_dm_requests_view(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma)
{
  ui->dm_requests_thumb = (ccord_rect){0, 0, 0, 0};
  bool want_spam = (ui->dm_requests_tab == 1);
  size_t dmn = ccord_store_channel_count(ui->d.store, CCORD_DM_GUILD_ID);
  size_t matched = 0;
  uint32_t req_cnt = 0, spam_cnt = 0;
  for (size_t i = 0; i < dmn; i++) {
    const ccord_channel *c = ccord_store_channel_at(ui->d.store, CCORD_DM_GUILD_ID, i);
    if (!c || !c->is_message_request) continue;
    if (c->is_spam) spam_cnt++;
    else req_cnt++;
    if (c->is_spam == want_spam) matched++;
  }
  bool compact = ui->mobile_layout || ma->w < 460.0f;
  float pad = compact ? 12.0f : 20.0f;
  float list_y = ma->y;
  if (ui->mobile_layout) {
    char tab0[32], tab1[32];
    snprintf(tab0, sizeof tab0, "Requests (%u)", (unsigned)req_cnt);
    snprintf(tab1, sizeof tab1, "Spam (%u)", (unsigned)spam_cnt);
    const char *rtabs[2] = {tab0, tab1};
    float bar_w = maxf(ma->w - 2.0f * pad, 120.0f);
    float tw = floorf((bar_w - 8.0f) * 0.5f);
    float tx = ma->x + pad;
    float ty = ma->y + 10.0f;
    for (int t = 0; t < 2; t++) {
      bool active = (ui->dm_requests_tab == t);
      ccord_rect tab_r = {tx, ty, tw, 38.0f};
      ui->dm_requests_tab_rects[t] = tab_r;
      ui_row_state tab_rs = ui_row_state_of(ui, "reqtab", (uint64_t)t,
                                            ui->mouse_in && ui_in_rect(ui->mx, ui->my, tab_r), active, COL(bg_primary));
      ui_row_state_fill(ui, r, &tab_rs, tab_r, 6.0f);
      ccord_text_style ts = ui_style(ui, active ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small),
                                     active ? COL(text_header) : COL(text_secondary));
      ccord_vec2 tt = ccord_text_measure(r, rtabs[t], strlen(rtabs[t]), &ts, 0.0f);
      ccord_text_draw_ellipsis(r, rtabs[t], strlen(rtabs[t]),
                               tab_r.x + maxf((tab_r.w - tt.x) * 0.5f, 4.0f),
                               tab_r.y + (tab_r.h - tt.y) * 0.5f, maxf(tab_r.w - 8.0f, 16.0f), &ts);
      tx += tw + 8.0f;
    }
    list_y = ty + 46.0f;
    ccord_draw_rect(r, (ccord_rect){ma->x + pad, list_y - 4.0f, bar_w, 1.0f}, COL(border_subtle));
  }
  ccord_rect view = {ma->x, list_y, ma->w, maxf(ma->y + ma->h - list_y, 0.0f)};
  float card_h = compact ? (ui->mobile_layout ? 118.0f : 108.0f) : 74.0f;
  float gap = 10.0f;
  ui->dm_requests_content_h = matched > 0 ? (pad * 2.0f + (float)matched * (card_h + gap)) : 0.0f;
  float max_s = maxf(ui->dm_requests_content_h - view.h, 0.0f);
  ui->dm_requests_scroll = clampf(ui->dm_requests_scroll, 0.0f, max_s);

  if (matched == 0) {
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base) + 1.0f, COL(text_header));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    const char *empty_t = want_spam ? "No spam message requests" : "No pending message requests";
    const char *empty_s = want_spam
      ? "Filtered message requests from unknown senders will appear here."
      : "Messages from people you don't share a friend connection with appear here before reaching your DMs.";
    float wrap_w = fminf(maxf(view.w - 32.0f, 120.0f), 440.0f);
    ccord_vec2 tt = ccord_text_measure(r, empty_t, strlen(empty_t), &ts, 0.0f);
    ccord_text_draw_ellipsis(r, empty_t, strlen(empty_t),
                             view.x + maxf((view.w - tt.x) * 0.5f, 16.0f), view.y + view.h * 0.38f, wrap_w, &ts);
    ccord_vec2 st = ccord_text_measure(r, empty_s, strlen(empty_s), &ss, wrap_w);
    ccord_text_draw(r, empty_s, strlen(empty_s), view.x + maxf((view.w - st.x) * 0.5f, 16.0f),
                    view.y + view.h * 0.38f + tt.y + 8.0f, &ss, wrap_w);
    return;
  }

  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
  ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_secondary));
  ccord_text_style abs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_on_brand));
  ccord_text_style rbs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));

  ccord_push_clip(r, view);
  float y = view.y + pad - ui->dm_requests_scroll;
  for (size_t i = 0; i < dmn; i++) {
    const ccord_channel *c = ccord_store_channel_at(ui->d.store, CCORD_DM_GUILD_ID, i);
    if (!c || !c->is_message_request || c->is_spam != want_spam) continue;
    ccord_rect card = {view.x + pad, y, maxf(view.w - pad * 2.0f, 160.0f), card_h};
    y += card_h + gap;
    if (card.y + card.h < view.y || card.y > view.y + view.h) continue;

    bool hover = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, card);
    ccord_draw_rrect(r, (ccord_rect){card.x - 1.0f, card.y - 1.0f, card.w + 2.0f, card.h + 2.0f}, 9.0f, COL(border_subtle));
    ccord_draw_rrect(r, card, 8.0f, hover ? COL(bg_item_hover) : COL(bg_secondary));

    const ccord_user *u = ui_dm_recipient(ui, c);
    const char *dname = ui_dm_display_name(ui, c);
    const char *uname = (u && u->handle && u->handle[0]) ? u->handle : ((u && u->name && u->name[0]) ? u->name : dname);
    ccord_user fallback = {
      .id = u ? u->id : (c->user_id != CCORD_ID_NONE ? c->user_id : c->id),
      .name = dname,
      .avatar_url = (u && u->avatar_url) ? u->avatar_url : "",
      .presence = u ? u->presence : CCORD_PRESENCE_OFFLINE,
      .avatar_palette = u ? u->avatar_palette : (uint8_t)(c->id % 6u),
    };
    float av_cy = compact ? (card.y + 34.0f) : (card.y + card.h * 0.5f);
    ui_draw_avatar(ui, r, &fallback, card.x + 32.0f, av_cy, 40.0f,
                   hover ? COL(bg_item_hover) : COL(bg_secondary), false);

    ccord_rect acc_r, rej_r;
    if (compact) {
      float bw = floorf((card.w - 32.0f) * 0.5f);
      float bh = uis_btn_h_for(ui, 30.0f);
      float by = card.y + card.h - (ui->mobile_layout ? 46.0f : 38.0f);
      acc_r = (ccord_rect){card.x + 12.0f, by, bw, bh};
      rej_r = (ccord_rect){acc_r.x + bw + 8.0f, by, bw, bh};
    } else {
      rej_r = (ccord_rect){card.x + card.w - 86.0f, card.y + (card.h - 30.0f) * 0.5f, 72.0f, 30.0f};
      acc_r = (ccord_rect){rej_r.x - 80.0f, rej_r.y, 72.0f, 30.0f};
    }
    bool ahv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, acc_r);
    bool rhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rej_r);
    ccord_draw_rrect(r, acc_r, 5.0f, ahv ? ui_mix(COL(brand), COL(text_on_brand), 0.12f) : COL(brand));
    ccord_draw_rrect(r, rej_r, 5.0f, rhv ? COL(danger) : COL(bg_tertiary));
    ccord_vec2 at = ccord_text_measure(r, "Accept", 6, &abs, 0.0f);
    ccord_vec2 rt = ccord_text_measure(r, "Reject", 6, &rbs, 0.0f);
    ccord_text_draw(r, "Accept", 6, acc_r.x + (acc_r.w - at.x) * 0.5f, acc_r.y + (acc_r.h - at.y) * 0.5f, &abs, 0.0f);
    ccord_text_draw(r, "Reject", 6, rej_r.x + (rej_r.w - rt.x) * 0.5f, rej_r.y + (rej_r.h - rt.y) * 0.5f,
                    rhv ? &abs : &rbs, 0.0f);

    float tx = card.x + 64.0f;
    float tw = compact ? maxf(card.w - 76.0f, 40.0f) : maxf(acc_r.x - tx - 14.0f, 40.0f);
    float nw = ccord_text_draw_ellipsis(r, dname, strlen(dname), tx, card.y + 12.0f, tw * 0.6f, &ns);
    char hbuf[96];
    snprintf(hbuf, sizeof hbuf, "@%s", uname ? uname : "user");
    float h_avail = tw - nw - 8.0f;
    if (h_avail > 20.0f) {
      ccord_text_draw_ellipsis(r, hbuf, strlen(hbuf), tx + nw + 8.0f, card.y + 14.0f, h_avail, &hs);
    }

    size_t mc = ccord_store_message_count(ui->d.store, c->id);
    char pbuf[192] = "Sent you a message request";
    if (mc > 0) {
      const ccord_message *lm = ccord_store_message_at(ui->d.store, c->id, mc - 1u);
      if (lm) (void)ccord_message_preview(lm, pbuf, sizeof pbuf);
    }
    size_t pbl = ui_streamer_mask_inplace(ui, pbuf, strlen(pbuf), true);
    ccord_text_draw_ellipsis(r, pbuf, pbl, tx, card.y + 38.0f, tw, &ps);

    // Only fully visible cards are clickable: clipped accept/reject buttons must not catch hits.
    if (card.y >= view.y && card.y + card.h <= view.y + view.h && ui->ndm_request_hits < 32u) {
      ui->dm_request_hits[ui->ndm_request_hits].card_rect = card;
      ui->dm_request_hits[ui->ndm_request_hits].accept_rect = acc_r;
      ui->dm_request_hits[ui->ndm_request_hits].reject_rect = rej_r;
      ui->dm_request_hits[ui->ndm_request_hits].channel_id = c->id;
      ui->ndm_request_hits++;
    }
  }
  ccord_pop_clip(r);
  ui->dm_requests_thumb = ui_draw_list_scrollbar(ui, r, view, ui->dm_requests_content_h, ui->dm_requests_scroll,
                                                 "reqscroll", 0);
}

// ---- DM pins, favorites, friend nicknames, ignored users & INI persistence ----


static bool id_list_has(const ccord_id *arr, size_t n, ccord_id id)
{
  for (size_t i = 0; i < n; i++) {
    if (arr[i] == id) return true;
  }
  return false;
}

static bool id_list_toggle(ccord_id *arr, size_t *n, size_t cap, ccord_id id)
{
  for (size_t i = 0; i < *n; i++) {
    if (arr[i] == id) {
      memmove(&arr[i], &arr[i + 1], (*n - i - 1) * sizeof arr[0]);
      (*n)--;
      return false;
    }
  }
  if (*n < cap) arr[(*n)++] = id;
  return true;
}

bool ui_dm_is_pinned(const ccord_ui *ui, ccord_id channel_id)
{
  return ui && channel_id != CCORD_ID_NONE && id_list_has(ui->pinned_dms, ui->npinned_dms, channel_id);
}

void ui_dm_toggle_pin(ccord_ui *ui, ccord_id channel_id)
{
  if (!ui || channel_id == CCORD_ID_NONE) return;
  bool on = id_list_toggle(ui->pinned_dms, &ui->npinned_dms, sizeof ui->pinned_dms / sizeof ui->pinned_dms[0], channel_id);
  ui->built_rev = UINT64_MAX;
  ui->built_lists_rev = UINT64_MAX;   // trigger channel-row rebuild so pinned order updates at once
  ui_settings_save(ui);
  ui_notice(ui, on ? "Pinned conversation" : "Unpinned conversation");
  ui->dirty = true;
}

bool ui_dm_is_favorite(const ccord_ui *ui, ccord_id channel_id)
{
  return ui && channel_id != CCORD_ID_NONE && id_list_has(ui->favorite_dms, ui->nfavorite_dms, channel_id);
}

void ui_dm_toggle_favorite(ccord_ui *ui, ccord_id channel_id)
{
  if (!ui || channel_id == CCORD_ID_NONE) return;
  bool on = id_list_toggle(ui->favorite_dms, &ui->nfavorite_dms,
                           sizeof ui->favorite_dms / sizeof ui->favorite_dms[0], channel_id);
  ui->built_rev = UINT64_MAX;
  ui->built_lists_rev = UINT64_MAX;
  ui_settings_save(ui);
  ui_notice(ui, on ? "Added to Favorites" : "Removed from Favorites");
  ui->dirty = true;
}

const char *ui_friend_nickname(const ccord_ui *ui, ccord_id user_id)
{
  if (!ui || user_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < ui->nfriend_nicks; i++) {
    if (ui->friend_nicks[i].user_id == user_id && ui->friend_nicks[i].nick[0] != '\0') {
      return ui->friend_nicks[i].nick;
    }
  }
  return nullptr;
}

void ui_set_friend_nickname(ccord_ui *ui, ccord_id user_id, const char *nick)
{
  if (!ui || user_id == CCORD_ID_NONE) return;
  while (nick && (*nick == ' ' || *nick == '\t')) nick++;
  size_t len = nick ? strlen(nick) : 0;
  while (len > 0 && (nick[len - 1] == ' ' || nick[len - 1] == '\t')) len--;
  for (size_t i = 0; i < ui->nfriend_nicks; i++) {
    if (ui->friend_nicks[i].user_id != user_id) continue;
    if (len == 0) {
      memmove(&ui->friend_nicks[i], &ui->friend_nicks[i + 1],
              (ui->nfriend_nicks - i - 1) * sizeof ui->friend_nicks[0]);
      ui->nfriend_nicks--;
      ui_settings_save(ui);
      ui_notice(ui, "Removed friend nickname");
      ui->dirty = true;
      return;
    }
    if (len > CCORD_NAME_MAX_BYTES) len = CCORD_NAME_MAX_BYTES;
    memcpy(ui->friend_nicks[i].nick, nick, len);
    ui->friend_nicks[i].nick[len] = '\0';
    ui_settings_save(ui);
    ui_notice(ui, "Updated friend nickname");
    ui->dirty = true;
    return;
  }
  if (len == 0) return;
  if (ui->nfriend_nicks < sizeof ui->friend_nicks / sizeof ui->friend_nicks[0]) {
    if (len > CCORD_NAME_MAX_BYTES) len = CCORD_NAME_MAX_BYTES;
    ui->friend_nicks[ui->nfriend_nicks].user_id = user_id;
    memcpy(ui->friend_nicks[ui->nfriend_nicks].nick, nick, len);
    ui->friend_nicks[ui->nfriend_nicks].nick[len] = '\0';
    ui->nfriend_nicks++;
    ui_settings_save(ui);
    ui_notice(ui, "Added friend nickname");
    ui->dirty = true;
  }
}

bool ui_user_is_ignored(const ccord_ui *ui, ccord_id user_id)
{
  return ui && user_id != CCORD_ID_NONE && id_list_has(ui->ignored_users, ui->nignored_users, user_id);
}

void ui_user_toggle_ignore(ccord_ui *ui, ccord_id user_id)
{
  if (!ui || user_id == CCORD_ID_NONE) return;
  bool on = id_list_toggle(ui->ignored_users, &ui->nignored_users,
                           sizeof ui->ignored_users / sizeof ui->ignored_users[0], user_id);
  ui_settings_save(ui);
  ui_notice(ui, on ? "User ignored" : "User unignored");
  ui->dirty = true;
}

void ui_dm_prefs_load_ini(ccord_ui *ui, const char *section, const char *key, const char *value)
{
  if (!ui || !section || !key || !value) return;
  if (strcmp(section, "pinned_dms") == 0) {
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(value, strlen(value), &id) && id != CCORD_ID_NONE &&
        !id_list_has(ui->pinned_dms, ui->npinned_dms, id) &&
        ui->npinned_dms < sizeof ui->pinned_dms / sizeof ui->pinned_dms[0]) {
      ui->pinned_dms[ui->npinned_dms++] = id;
    }
  } else if (strcmp(section, "favorite_dms") == 0) {
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(value, strlen(value), &id) && id != CCORD_ID_NONE &&
        !id_list_has(ui->favorite_dms, ui->nfavorite_dms, id) &&
        ui->nfavorite_dms < sizeof ui->favorite_dms / sizeof ui->favorite_dms[0]) {
      ui->favorite_dms[ui->nfavorite_dms++] = id;
    }
  } else if (strcmp(section, "ignored_users") == 0) {
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(value, strlen(value), &id) && id != CCORD_ID_NONE &&
        !id_list_has(ui->ignored_users, ui->nignored_users, id) &&
        ui->nignored_users < sizeof ui->ignored_users / sizeof ui->ignored_users[0]) {
      ui->ignored_users[ui->nignored_users++] = id;
    }
  } else if (strcmp(section, "friend_nicknames") == 0) {
    ccord_id uid = CCORD_ID_NONE;
    if (ccord_id_parse(key, strlen(key), &uid) && uid != CCORD_ID_NONE && value[0] != '\0' &&
        ui->nfriend_nicks < sizeof ui->friend_nicks / sizeof ui->friend_nicks[0]) {
      for (size_t i = 0; i < ui->nfriend_nicks; i++) {
        if (ui->friend_nicks[i].user_id == uid) return;
      }
      ui->friend_nicks[ui->nfriend_nicks].user_id = uid;
      snprintf(ui->friend_nicks[ui->nfriend_nicks].nick, sizeof ui->friend_nicks[0].nick, "%s", value);
      ui->nfriend_nicks++;
    }
  } else if (strcmp(section, "hide_muted_guilds") == 0) {
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(value, strlen(value), &id) && id != CCORD_ID_NONE &&
        !id_list_has(ui->hide_muted_guilds, ui->nhide_muted_guilds, id) &&
        ui->nhide_muted_guilds < sizeof ui->hide_muted_guilds / sizeof ui->hide_muted_guilds[0]) {
      ui->hide_muted_guilds[ui->nhide_muted_guilds++] = id;
    }
  } else if (strcmp(section, "pinned_channels") == 0) {
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(value, strlen(value), &id) && id != CCORD_ID_NONE &&
        !id_list_has(ui->pinned_channels, ui->npinned_channels, id) &&
        ui->npinned_channels < sizeof ui->pinned_channels / sizeof ui->pinned_channels[0]) {
      ui->pinned_channels[ui->npinned_channels++] = id;
    }
  } else if (strcmp(section, "hidden_voice_names") == 0) {
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(value, strlen(value), &id) && id != CCORD_ID_NONE &&
        !id_list_has(ui->hidden_voice_names, ui->nhidden_voice_names, id) &&
        ui->nhidden_voice_names < sizeof ui->hidden_voice_names / sizeof ui->hidden_voice_names[0]) {
      ui->hidden_voice_names[ui->nhidden_voice_names++] = id;
    }
  }
}

bool ui_guild_hide_muted(const ccord_ui *ui, ccord_id guild_id)
{
  return ui && id_list_has(ui->hide_muted_guilds, ui->nhide_muted_guilds, guild_id);
}

void ui_guild_toggle_hide_muted(ccord_ui *ui, ccord_id guild_id)
{
  if (!ui || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return;
  bool on = id_list_toggle(ui->hide_muted_guilds, &ui->nhide_muted_guilds,
                           sizeof ui->hide_muted_guilds / sizeof ui->hide_muted_guilds[0], guild_id);
  ui->built_rev = UINT64_MAX;
  ui->built_lists_rev = UINT64_MAX;
  ui_settings_save(ui);
  ui_notice(ui, on ? "Hiding muted channels" : "Showing all channels");
  ui->dirty = true;
}

bool ui_channel_is_pinned(const ccord_ui *ui, ccord_id ch_id)
{
  return ui && id_list_has(ui->pinned_channels, ui->npinned_channels, ch_id);
}

void ui_channel_toggle_pin(ccord_ui *ui, ccord_id ch_id)
{
  if (!ui || ch_id == CCORD_ID_NONE) return;
  bool on = id_list_toggle(ui->pinned_channels, &ui->npinned_channels,
                           sizeof ui->pinned_channels / sizeof ui->pinned_channels[0], ch_id);
  ui->built_rev = UINT64_MAX;
  ui->built_lists_rev = UINT64_MAX;
  ui_settings_save(ui);
  ui_notice(ui, on ? "Pinned channel to top" : "Unpinned channel");
  ui->dirty = true;
}

bool ui_voice_names_hidden(const ccord_ui *ui, ccord_id ch_id)
{
  return ui && id_list_has(ui->hidden_voice_names, ui->nhidden_voice_names, ch_id);
}

void ui_voice_toggle_names_hidden(ccord_ui *ui, ccord_id ch_id)
{
  if (!ui || ch_id == CCORD_ID_NONE) return;
  bool on = id_list_toggle(ui->hidden_voice_names, &ui->nhidden_voice_names,
                           sizeof ui->hidden_voice_names / sizeof ui->hidden_voice_names[0], ch_id);
  ui->built_rev = UINT64_MAX;
  ui->built_lists_rev = UINT64_MAX;
  ui_settings_save(ui);
  ui_notice(ui, on ? "Hidden voice participant names" : "Showing voice participant names");
  ui->dirty = true;
}

void ui_dm_prefs_save_ini(const ccord_ui *ui, FILE *f)
{
  if (!ui || !f) return;
  if (ui->npinned_dms > 0) {
    fprintf(f, "[pinned_dms]\n");
    for (size_t i = 0; i < ui->npinned_dms; i++) {
      fprintf(f, "dm_%zu = %llu\n", i, (unsigned long long)ui->pinned_dms[i]);
    }
  }
  if (ui->nfavorite_dms > 0) {
    fprintf(f, "[favorite_dms]\n");
    for (size_t i = 0; i < ui->nfavorite_dms; i++) {
      fprintf(f, "dm_%zu = %llu\n", i, (unsigned long long)ui->favorite_dms[i]);
    }
  }
  if (ui->nignored_users > 0) {
    fprintf(f, "[ignored_users]\n");
    for (size_t i = 0; i < ui->nignored_users; i++) {
      fprintf(f, "u_%zu = %llu\n", i, (unsigned long long)ui->ignored_users[i]);
    }
  }
  if (ui->nfriend_nicks > 0) {
    fprintf(f, "[friend_nicknames]\n");
    for (size_t i = 0; i < ui->nfriend_nicks; i++) {
      if (ui->friend_nicks[i].nick[0] == '\0') continue;
      fprintf(f, "%llu = %s\n", (unsigned long long)ui->friend_nicks[i].user_id, ui->friend_nicks[i].nick);
    }
  }
  if (ui->nhide_muted_guilds > 0) {
    fprintf(f, "[hide_muted_guilds]\n");
    for (size_t i = 0; i < ui->nhide_muted_guilds; i++) {
      fprintf(f, "g_%zu = %llu\n", i, (unsigned long long)ui->hide_muted_guilds[i]);
    }
  }
  if (ui->npinned_channels > 0) {
    fprintf(f, "[pinned_channels]\n");
    for (size_t i = 0; i < ui->npinned_channels; i++) {
      fprintf(f, "ch_%zu = %llu\n", i, (unsigned long long)ui->pinned_channels[i]);
    }
  }
  if (ui->nhidden_voice_names > 0) {
    fprintf(f, "[hidden_voice_names]\n");
    for (size_t i = 0; i < ui->nhidden_voice_names; i++) {
      fprintf(f, "vc_%zu = %llu\n", i, (unsigned long long)ui->hidden_voice_names[i]);
    }
  }
}

void ui_dm_invite_tick(ccord_ui *ui)
{
  if (!ui || ui->pending_dm_invite_guild == CCORD_ID_NONE) return;
  if (ui->now_ms > ui->pending_dm_invite_until_ms) {
    ui->pending_dm_invite_guild = CCORD_ID_NONE;
    return;
  }
  char code[36] = {0};
  uint64_t seq = 0;
  if (!ccord_store_last_invite(ui->d.store, ui->pending_dm_invite_guild, code, &seq) ||
      seq == (uint64_t)ui->pending_dm_invite_prev_count) {
    return;
  }
  ccord_id target_ch = ui->pending_dm_invite_channel;
  if (target_ch == CCORD_ID_NONE && ui->pending_dm_invite_user != CCORD_ID_NONE) {
    target_ch = cm_find_1on1_dm_channel(ui, ui->pending_dm_invite_user);
    if (target_ch == CCORD_ID_NONE) return;
  }
  if (target_ch != CCORD_ID_NONE && ui->d.session) {
    char msg[96];
    snprintf(msg, sizeof msg, "https://discord.gg/%s", code);
    ccord_error err;
    if (ccord_session_send_message(ui->d.session, target_ch, msg, strlen(msg), &err) == CCORD_OK) {
      ui_notice(ui, "Sent server invite");
    } else {
      ui_notice(ui, "Couldn't send invite: %s", err.message);
    }
  }
  ui->pending_dm_invite_guild = CCORD_ID_NONE;
  ui->pending_dm_invite_channel = CCORD_ID_NONE;
  ui->pending_dm_invite_user = CCORD_ID_NONE;
}
