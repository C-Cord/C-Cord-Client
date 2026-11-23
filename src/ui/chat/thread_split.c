// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Desktop Split-View Threads: keeps the parent text channel visible on the left while the active thread renders on the right.
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_core_internal.h"
#include "ui_style.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

const ccord_channel *ui_thread_split_parent(const ccord_ui *ui)
{
  if (!ui || !ui->d.store) return nullptr;
  ccord_id sel = ccord_store_selected_channel(ui->d.store);
  const ccord_channel *ch = sel != CCORD_ID_NONE ? ccord_store_channel(ui->d.store, sel) : nullptr;
  if (!ch || ch->guild_id == CCORD_DM_GUILD_ID) return nullptr;
  if (!ccord_discord_type_is_thread(ch->discord_type) || ch->parent_id == CCORD_ID_NONE) return nullptr;
  const ccord_channel *parent = ccord_store_channel(ui->d.store, ch->parent_id);
  if (!parent || parent->kind != CCORD_CHANNEL_TEXT) return nullptr;
  if (ccord_store_channel_is_forum(ui->d.store, parent->id)) return nullptr;
  return parent;
}

bool ui_thread_split_active(const ccord_ui *ui)
{
  if (!ui || ui->mobile_layout || !ui->settings_threads_split_view) return false;
  if (ui->stage_channel != CCORD_ID_NONE || ui->discovery_open || ui_guild_view_active(ui)) return false;
  if (!ui->layout.stage_split) return false;
  return ui_thread_split_parent(ui) != nullptr;
}

ccord_rect ui_thread_split_handle_rect(const ccord_ui *ui)
{
  if (!ui_thread_split_active(ui)) return (ccord_rect){0, 0, 0, 0};
  const ccord_layout_rect *sp = &ui->layout.stage_pane;
  return (ccord_rect){sp->x + sp->w - 4.0f, sp->y, 8.0f, sp->h};
}

void ui_draw_thread_split_divider(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r || !ui_thread_split_active(ui)) return;
  ccord_rect handle = ui_thread_split_handle_rect(ui);
  if (handle.w <= 0.0f || handle.h <= 0.0f) return;
  float seam_x = ui->layout.stage_pane.x + ui->layout.stage_pane.w;
  bool dragging = ui->drag == DRAG_THREAD_SPLIT;
  bool hovered = !dragging && ui->mouse_in && ui_in_rect(ui->mx, ui->my, handle);
  if (dragging || hovered) {
    ccord_draw_rect(r, (ccord_rect){seam_x - 1.5f, handle.y, 3.0f, handle.h},
                    dragging ? COL(brand) : ui_mix(COL(border_subtle), COL(brand), 0.65f));
  } else {
    ccord_draw_rect(r, (ccord_rect){seam_x - 0.5f, handle.y, 1.0f, handle.h}, COL(border_subtle));
  }
}

void ui_draw_thread_parent_pane(ccord_ui *ui, ccord_renderer *r)
{
  ui->thread_parent_close_rect = (ccord_rect){0};
  ui->thread_parent_focus_rect = (ccord_rect){0};
  if (!ui_thread_split_active(ui)) return;
  const ccord_channel *parent = ui_thread_split_parent(ui);
  if (!parent) return;

  ccord_id thread_id = ccord_store_selected_channel(ui->d.store);
  ccord_rect pane = ui_rect(ui->layout.stage_pane);
  ccord_draw_rect(r, pane, COL(bg_primary));

  uis_tokens tk = uis_tokens_of(ui);
  float hh = ui->layout.chat_header.h;
  ccord_rect hdr = {pane.x, pane.y, pane.w, hh};
  ccord_draw_rect(r, hdr, COL(bg_primary));
  ccord_draw_rect(r, (ccord_rect){hdr.x, hdr.y + hdr.h - 1.0f, hdr.w, 1.0f}, COL(border_subtle));

  float cy = hdr.y + hdr.h * 0.5f;
  ui_icon_hash(r, hdr.x + 22.0f, cy, 18.0f, COL(text_muted));

  // Right-side header buttons: "Full Thread" (exits split) and "Open #parent" (focuses parent channel)
  float bh = 24.0f;
  float right_x = hdr.x + hdr.w - 10.0f;
  ccord_rect full_r = {right_x - 76.0f, cy - bh * 0.5f, 76.0f, bh};
  bool fhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, full_r);
  uis_button(ui, r, full_r, "Full View", UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){.hover = fhv}, "thfull", 0);
  ui->thread_parent_close_rect = full_r;

  ccord_rect focus_r = {full_r.x - 66.0f, cy - bh * 0.5f, 60.0f, bh};
  bool ohv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, focus_r);
  uis_button(ui, r, focus_r, "Focus", UIS_BTN_LINK, UIS_BTN_S_SM, (uis_state){.hover = ohv}, "thfoc", 0);
  ui->thread_parent_focus_rect = focus_r;

  const char *pname = (parent->name && parent->name[0]) ? parent->name : "channel";
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
  float tx = hdr.x + 38.0f;
  float avail = fmaxf(focus_r.x - tx - 8.0f, 40.0f);
  ccord_text_draw_ellipsis(r, pname, strlen(pname), tx, cy - ui_line_h(ui, ns.size) * 0.5f, avail, &ns);

  // Scrollable message list of parent channel (anchored to newest messages by default)
  ccord_rect list = {pane.x, hdr.y + hdr.h, pane.w, fmaxf(pane.h - hdr.h, 0.0f)};
  size_t nm = ccord_store_message_count(ui->d.store, parent->id);
  if (ui->thread_parent_last_id != parent->id) {
    ui->thread_parent_last_id = parent->id;
    ui->thread_parent_scroll = 1e9f;   // snap to bottom on first view
  }

  float pad_x = 14.0f;
  float av_sz = 32.0f;
  float text_x = list.x + pad_x + av_sz + 10.0f;
  float text_w = fmaxf(list.w - (pad_x * 2.0f + av_sz + 10.0f), 80.0f);
  ccord_text_style body_s = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base) - 0.5f, COL(text_normal));
  ccord_text_style name_s = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base) - 0.5f, COL(text_header));
  ccord_text_style meta_s = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_muted));

  // First pass: measure total height (last 60 messages to stay O(1) even in huge channels)
  size_t first_idx = nm > 60u ? nm - 60u : 0u;
  float total_h = 12.0f;
  for (size_t i = first_idx; i < nm; i++) {
    const ccord_message *m = ccord_store_message_at(ui->d.store, parent->id, i);
    if (!m) continue;
    const char *txt = (m->content && m->content[0]) ? m->content
                    : ((m->rich && m->rich->attachment_count > 0) ? "[Attachment]" : "");
    ccord_vec2 sz = txt[0] ? ccord_text_measure(r, txt, strlen(txt), &body_s, text_w)
                           : (ccord_vec2){0.0f, ui_line_h(ui, body_s.size)};
    float rh = fmaxf(av_sz + 6.0f, ui_line_h(ui, name_s.size) + sz.y + 12.0f);
    total_h += rh + 6.0f;
  }
  total_h += 12.0f;
  ui->thread_parent_content_h = total_h;
  float max_s = fmaxf(total_h - list.h, 0.0f);
  if (ui->thread_parent_scroll > max_s) ui->thread_parent_scroll = max_s;
  if (ui->thread_parent_scroll < 0.0f) ui->thread_parent_scroll = 0.0f;

  ccord_push_clip(r, list);
  if (nm == 0) {
    const char *empty = "No messages loaded in parent channel.";
    ccord_text_draw(r, empty, strlen(empty), list.x + 16.0f, list.y + 20.0f, &meta_s, list.w - 32.0f);
  } else {
    float y = list.y + 12.0f - ui->thread_parent_scroll;
    for (size_t i = first_idx; i < nm; i++) {
      const ccord_message *m = ccord_store_message_at(ui->d.store, parent->id, i);
      if (!m) continue;
      const char *txt = (m->content && m->content[0]) ? m->content
                    : ((m->rich && m->rich->attachment_count > 0) ? "[Attachment]" : "");
      ccord_vec2 sz = txt[0] ? ccord_text_measure(r, txt, strlen(txt), &body_s, text_w)
                             : (ccord_vec2){0.0f, ui_line_h(ui, body_s.size)};
      float rh = fmaxf(av_sz + 6.0f, ui_line_h(ui, name_s.size) + sz.y + 12.0f);
      if (y + rh >= list.y && y <= list.y + list.h) {
        bool is_starter = (m->id == thread_id || (m->rich && m->rich->thread.id == thread_id));
        if (is_starter) {
          ccord_rect hi = {list.x + 6.0f, y - 2.0f, list.w - 12.0f, rh + 2.0f};
          ccord_draw_rrect(r, hi, 6.0f, ui_mix(COL(bg_primary), COL(brand), 0.14f));
          ccord_draw_rect(r, (ccord_rect){hi.x, hi.y + 2.0f, 3.0f, hi.h - 4.0f}, COL(brand));
        }
        const ccord_user *u = ccord_store_user(ui->d.store, m->author_id);
        ui_draw_avatar(ui, r, u, list.x + pad_x + av_sz * 0.5f, y + av_sz * 0.5f + 2.0f, av_sz, COL(bg_primary), false);
        const char *aname = (u && u->display_name && u->display_name[0]) ? u->display_name
                          : ((u && u->name && u->name[0]) ? u->name : "Member");
        ccord_text_draw_ellipsis(r, aname, strlen(aname), text_x, y + 2.0f, text_w, &name_s);
        if (txt[0]) {
          ccord_text_draw(r, txt, strlen(txt), text_x, y + ui_line_h(ui, name_s.size) + 4.0f, &body_s, text_w);
        }
      }
      y += rh + 6.0f;
    }
  }
  ccord_pop_clip(r);
}

bool ui_thread_parent_handle_mouse(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui_thread_split_active(ui) || !ev) return false;
  ccord_rect pane = ui_rect(ui->layout.stage_pane);
  if (!ui_in_rect(ev->x, ev->y, pane)) return false;
  if (ui->thread_parent_close_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->thread_parent_close_rect)) {
    ui->settings_threads_split_view = false;
    ui_settings_save(ui);
    ui_compute_layout(ui);
    ccord_vlist_invalidate_all(&ui->vlist);
    ui->dirty = true;
    return true;
  }
  if (ui->thread_parent_focus_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->thread_parent_focus_rect)) {
    const ccord_channel *parent = ui_thread_split_parent(ui);
    if (parent) {
      ccord_error err;
      if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, parent->id, &err);
      else (void)ccord_store_select_channel(ui->d.store, parent->id, &err);
      ui_invalidate_rows(ui);
      ui_ensure_caches(ui);
      ui_sync_messages(ui);
      ui_save_current_location(ui);
      ui_compute_layout(ui);
      ui->dirty = true;
    }
    return true;
  }
  return true;
}

bool ui_thread_parent_handle_wheel(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui_thread_split_active(ui) || !ev) return false;
  ccord_rect pane = ui_rect(ui->layout.stage_pane);
  if (!ui_in_rect(ev->x, ev->y, pane)) return false;
  float view_h = fmaxf(pane.h - ui->layout.chat_header.h, 0.0f);
  float max_s = fmaxf(ui->thread_parent_content_h - view_h, 0.0f);
  ui->thread_parent_scroll -= ev->wheel * 44.0f;
  if (ui->thread_parent_scroll < 0.0f) ui->thread_parent_scroll = 0.0f;
  if (ui->thread_parent_scroll > max_s) ui->thread_parent_scroll = max_s;
  ui->dirty = true;
  return true;
}
