// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// "N new messages since <time>" bar at the top of the timeline, with Mark as Read and jump to the oldest unread.
// The bar shows while the channel has unread messages and the NEW divider is out of view (above, below or not loaded).
// A click on the bar scrolls the divider into view, paging older history until it is loaded; Mark as Read acks the channel.
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#define NMB_COL(name) (ui->theme->c.name)
#define NMB_PLUS 50u   // an unloaded divider means the real count may be higher: read '50+'

// Unread messages by authors the user blocked: they are collapsed in the timeline and do not count.
static uint32_t nmb_blocked_unread(const ccord_ui *ui, ccord_id ch, uint32_t unread_n)
{
  size_t total = ccord_store_message_count(ui->d.store, ch);
  uint32_t seen = 0, blocked = 0;
  for (size_t k = total; k > 0 && seen < unread_n; k--) {
    const ccord_message *m = ccord_store_message_at(ui->d.store, ch, k - 1);
    if (!m || ccord_message_is_ephemeral(m)) continue;
    if (ui->new_marker_id != CCORD_ID_NONE && m->id <= ui->new_marker_id) break;
    seen++;
    if (m->author_id != CCORD_ID_NONE && ccord_store_relationship(ui->d.store, m->author_id) == CCORD_REL_BLOCKED) blocked++;
  }
  return blocked;
}

void ui_nmb_draw(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma, ccord_id ch, size_t view_first, size_t view_last)
{
  bool nb_on = false;
  if (!(ui->search_open && ui->search_edit.len > 0)) {
    size_t first_new_row = SIZE_MAX;
    if (ui->new_marker_active && ui->new_marker_channel == ch) {
      size_t total_m = ccord_store_message_count(ui->d.store, ch);
      for (size_t k = total_m; k > 0; k--) {
        const ccord_message *mk = ccord_store_message_at(ui->d.store, ch, k - 1);
        if (!mk) continue;
        if (ui->new_marker_id != CCORD_ID_NONE && mk->id <= ui->new_marker_id) break;
        if (ui_row_has_new_divider(ui, ch, k - 1, mk)) {
          first_new_row = k - 1;
          break;
        }
      }
    }
    // Seeking: a click asked for the divider and it is not loaded yet, so keep paging older history.
    if (ui->nmb_seek_channel == ch && ui->nmb_seek_channel != CCORD_ID_NONE) {
      if (first_new_row != SIZE_MAX || !ui->new_marker_active || !ui->d.session ||
          !ccord_session_history_has_more(ui->d.session, ch)) {
        ui->nmb_seek_channel = CCORD_ID_NONE;
        if (first_new_row != SIZE_MAX && first_new_row < ui->vlist.count) {
          ui->vlist.stick_bottom = false;
          ui->vlist.anchor_index = first_new_row;
          ui->vlist.anchor_offset = first_new_row > 0 ? (ui->mobile_layout ? 46.0f : 34.0f) : 0.0f;
        }
      } else {
        ui->vlist.stick_bottom = false;
        ui->vlist.anchor_index = 0;
        ui->vlist.anchor_offset = 0.0f;
        ui->scroll_up_pending = true;
      }
    }
    const ccord_channel *ch_obj = ccord_store_channel(ui->d.store, ch);
    uint32_t unread_n = ch_obj ? ch_obj->unread_count : 0;
    uint32_t blocked_n = unread_n > 0 ? nmb_blocked_unread(ui, ch, unread_n) : 0;
    uint32_t shown_n = unread_n > 0 ? (unread_n > blocked_n ? unread_n - blocked_n : 0) : ui->new_marker_count;
    bool in_view = first_new_row != SIZE_MAX && first_new_row >= view_first && first_new_row <= view_last;
    bool have_unread = unread_n > 0 ? shown_n > 0 : (first_new_row != SIZE_MAX && first_new_row < view_first);
    nb_on = have_unread && !in_view && ma->w > 120.0f;
    if (nb_on) {
      char *btxt = ui->new_bar_txt;   // kept while the bar fades out
      char num[16];
      if (first_new_row == SIZE_MAX && shown_n >= NMB_PLUS) snprintf(num, sizeof num, "%u+", (unsigned)NMB_PLUS);
      else snprintf(num, sizeof num, "%u", (unsigned)shown_n);
      if (shown_n > 0) {
        const ccord_message *fnm = first_new_row != SIZE_MAX ? ccord_store_message_at(ui->d.store, ch, first_new_row) : nullptr;
        if (fnm && fnm->timestamp_ms > 0) {
          char since[32];
          ccord_timefmt_clock(since, sizeof since, fnm->timestamp_ms, ui->use_utc);
          snprintf(btxt, sizeof ui->new_bar_txt, "%s new message%s since %s", num, shown_n == 1u ? "" : "s", since);
        } else {
          snprintf(btxt, sizeof ui->new_bar_txt, "%s new message%s", num, shown_n == 1u ? "" : "s");
        }
      } else {
        snprintf(btxt, sizeof ui->new_bar_txt, "New messages above");
      }
      ui->new_bar_target_row = first_new_row;
    }
  }
  float nb_t = ui_fade_ms(ui, "newbar", 1u, nb_on, UI_BAR_MS);
  ui->dbg_new_bar_t = nb_t;
  // The click rects follow the bar's state, not its fade: a bar that is on is clickable on its first frame.
  if (nb_t > 0.004f || nb_on) {
    ccord_text_style bst = ui_style(ui, CCORD_FONT_BOLD, ui->theme->m.font_size_small - 2.0f, NMB_COL(text_on_brand));
    const char *ack_lbl = "Mark as Read";
    ccord_vec2 at = ccord_text_measure(r, ack_lbl, strlen(ack_lbl), &bst, 0.0f);
    float ack_w = fminf(at.x + 20.0f, ma->w * 0.45f);
    float bh = ui->mobile_layout ? 38.0f : 26.0f;
    ccord_rect bar = {ma->x + 12.0f, ma->y + 4.0f, ma->w - 24.0f, bh};
    ccord_rect ack_r = {bar.x + bar.w - ack_w, bar.y, ack_w, bar.h};
    if (nb_on) {
      ui->new_bar_rect = bar;
      ui->new_bar_ack_rect = ack_r;
    }
    if (nb_t > 0.004f) {
      ccord_push_alpha(r, nb_t);
      ccord_push_offset(r, 0.0f, (nb_t - 1.0f) * UI_BAR_SLIDE);   // it comes down from the top edge
      ccord_draw_rrect(r, bar, 6.0f, NMB_COL(brand));
      ccord_draw_rect(r, (ccord_rect){ack_r.x, bar.y + 4.0f, 1.0f, bar.h - 8.0f}, ui_alpha(NMB_COL(text_on_brand), 0.35f));
      float text_y = bar.y + (bar.h - at.y) * 0.5f;
      ccord_text_draw_ellipsis(r, ui->new_bar_txt, strlen(ui->new_bar_txt), bar.x + 12.0f, text_y,
                               fmaxf(ack_r.x - (bar.x + 20.0f), 10.0f), &bst);
      ccord_text_draw_ellipsis(r, ack_lbl, strlen(ack_lbl), ack_r.x + 10.0f, text_y, fmaxf(ack_r.w - 16.0f, 10.0f), &bst);
      ccord_pop_offset(r);
      ccord_pop_alpha(r);
    }
  }
}

bool ui_nmb_click(ccord_ui *ui, float x, float y)
{
  if (!(ui->new_bar_rect.w > 0.0f && ui_in_rect(x, y, ui->new_bar_rect))) return false;
  if (ui_in_rect(x, y, ui->new_bar_ack_rect)) {
    ui->nmb_seek_channel = CCORD_ID_NONE;
    ui_dismiss_new_marker(ui, true);
    return true;
  }
  ui->msg_scroll_vel = 0.0f;
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (ui->new_bar_target_row != SIZE_MAX && ui->new_bar_target_row < ui->vlist.count) {
    ui->vlist.stick_bottom = false;
    ui->vlist.anchor_index = ui->new_bar_target_row;
    ui->vlist.anchor_offset = ui->new_bar_target_row > 0 ? (ui->mobile_layout ? 46.0f : 34.0f) : 0.0f;
  } else if (ui->vlist.count > 0) {
    ui->vlist.stick_bottom = false;
    ui->vlist.anchor_index = 0;
    ui->vlist.anchor_offset = 0.0f;
    ui->scroll_up_pending = true;
    ui->nmb_seek_channel = ch;   // keep paging older history until the divider is loaded
  }
  ui->dirty = true;
  return true;
}
