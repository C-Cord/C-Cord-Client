// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ui_core_internal.h"

void ui_post_nav_aux(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id, ccord_id parent_id,
                            ccord_nav_view view)
{
  if (!ui || !ui->d.bus) return;
  (void)ccord_bus_post(ui->d.bus, (ccord_event){
                                    .type = CCORD_EVT_NAV_AUX,
                                    .guild_id = guild_id,
                                    .channel_id = channel_id,
                                    .message_id = parent_id,
                                    .value = (int64_t)view,
                                  });
}

void ui_post_view_select(ccord_ui *ui, ccord_id guild_id, ccord_nav_view view)
{
  if (!ui || !ui->d.bus) return;
  (void)ccord_bus_post(ui->d.bus, (ccord_event){
                                    .type = CCORD_EVT_VIEW_SELECT,
                                    .guild_id = guild_id,
                                    .value = (int64_t)view,
                                  });
}

void ui_materialize_thread_hit(ccord_ui *ui, const ui_thread_hit *th, ccord_id fallback_guild)
{
  if (!ui || !ui->d.store || !th || th->thread_id == CCORD_ID_NONE) return;
  if (ccord_store_channel(ui->d.store, th->thread_id)) return;
  ccord_id gid = th->guild_id != CCORD_ID_NONE ? th->guild_id : fallback_guild;
  if (gid == CCORD_ID_NONE) return;
  ccord_channel c = {
    .id = th->thread_id,
    .guild_id = gid,
    .parent_id = th->parent_id,
    .kind = CCORD_CHANNEL_TEXT,
    .name = th->name[0] ? th->name : "thread",
    .topic = "",
    .icon_url = "",
    .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD,
  };
  ccord_error err = {0};
  (void)ccord_store_upsert_channel(ui->d.store, &c, &err);
}

bool uc_aux_open_channel_row(ccord_ui *ui, size_t row)
{
  if (!ui || row >= ui->ncrow) return false;
  if (ui->crow[row].dm_nav != DM_NAV_NONE) {
    if (ui->crow[row].dm_nav == DM_NAV_FRIENDS) {
      ui_post_nav_aux(ui, CCORD_DM_GUILD_ID, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_NAV_VIEW_FRIENDS);
      return true;
    }
    if (ui->crow[row].dm_nav == DM_NAV_MESSAGE_REQUESTS) {
      ui_post_nav_aux(ui, CCORD_DM_GUILD_ID, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_NAV_VIEW_REQUESTS);
      return true;
    }
    return false;
  }
  if (ui->crow[row].is_relationship || ui->crow[row].is_voice_user || ui->crow[row].is_category) return false;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, ui->crow[row].index);
  if (!ch || ch->kind != CCORD_CHANNEL_TEXT) return false;
  bool is_th = ccord_discord_type_is_thread(ch->discord_type);
  ui_post_nav_aux(ui, g, ch->id, is_th ? ch->parent_id : CCORD_ID_NONE,
                  is_th ? CCORD_NAV_VIEW_THREAD : CCORD_NAV_VIEW_CHANNEL);
  return true;
}

void uc_click_channel_row(ccord_ui *ui, size_t row)
{
  if (row >= ui->ncrow) return;
  if (ui->crow[row].dm_nav != DM_NAV_NONE) {
    switch (ui->crow[row].dm_nav) {
      case DM_NAV_FRIENDS:
        if (ui->discovery_open) {
          ui->discovery_open = false;
          ui_compute_layout(ui);
        }
        if (ui->dm_requests_view) ui_invalidate_rows(ui);
        ui->dm_friends_view = true;
        ui->dm_requests_view = false;
        ui->dm_friends_scroll = 0.0f;
        ui->stage_channel = CCORD_ID_NONE;
        if (ui->mobile_layout) ui->left_drawer_open = false;
        ui_record_nav_location(ui);
        ui_post_view_select(ui, CCORD_DM_GUILD_ID, CCORD_NAV_VIEW_FRIENDS);
        ui->dirty = true;
        break;
      case DM_NAV_MESSAGE_REQUESTS:
        if (ui->discovery_open) {
          ui->discovery_open = false;
          ui_compute_layout(ui);
        }
        ui->dm_requests_view = true;
        ui->dm_friends_view = false;
        ui->dm_requests_scroll = 0.0f;
        ui->stage_channel = CCORD_ID_NONE;
        if (ui->mobile_layout) ui->left_drawer_open = false;
        ui_record_nav_location(ui);
        ui_post_view_select(ui, CCORD_DM_GUILD_ID, CCORD_NAV_VIEW_REQUESTS);
        ui->dirty = true;
        break;
      case DM_NAV_NITRO:
        (void)ui_open_url(ui, "https://discord.com/nitro", "Nitro");
        break;
      case DM_NAV_SHOP:
        (void)ui_open_url(ui, "https://discord.com/shop", "Shop");
        break;
      case DM_NAV_SERVER_GUIDE:
        ui_guild_view_open(ui, UI_GUILD_VIEW_SERVER_GUIDE);
        break;
      case DM_NAV_CHANNELS_ROLES:
        ui_guild_view_open(ui, UI_GUILD_VIEW_CHANNELS_ROLES);
        break;
      case DM_NAV_DM_HEADER:
      case DM_NAV_NONE:
        break;
    }
    return;
  }
  if (ui->crow[row].is_relationship) {
    if (ui->crow[row].is_category) return;
    const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, ui->crow[row].index);
    if (!rel) return;
    if (rel->kind == CCORD_RELATIONSHIP_PENDING_INCOMING) {
      ui_set_user_relationship(ui, rel->user_id, CCORD_RELATIONSHIP_FRIEND);
      return;
    }
    ui_open_dm_with_user(ui, rel->user_id);
    return;
  }
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, ui->crow[row].index);
  if (!ch) return;
  if (ui->crow[row].is_voice_user) {
    ccord_id uid = ui->crow[row].user_id;
    const ccord_voice_state *self = ui_self_voice(ui);
    bool already_in_stage = (self && self->channel_id == ch->id && ui->stage_channel == ch->id);
    if (!self || self->channel_id != ch->id) {
      if (ui_voice_lock_reason(ui, g, ch) != nullptr) {
        if (uid != CCORD_ID_NONE) ui_open_member_popover(ui, uid, ui->mx, ui->my, false);
        return;
      }
      uc_join_voice_channel(ui, g, ch);
    } else if (ui->stage_channel != ch->id) {
      ui->stage_channel = ch->id;
      ui->stage_armed = true;
      ui->dirty = true;
    }
    const ccord_voice_state *vs = ccord_store_voice_user(ui->d.store, g, uid);
    bool started_watch = false;
    if (vs && vs->self_stream && uid != ccord_store_self_id(ui->d.store) && ui->d.session) {
      ccord_error werr;
      if (ccord_session_watch_stream(ui->d.session, g, ch->id, uid, &werr) == CCORD_OK) {
        const ccord_user *u = ccord_store_user(ui->d.store, uid);
        ui_notice(ui, "Watching %s's stream", u ? u->name : "stream");
        started_watch = true;
      }
      ui->dirty = true;
    }
    if (already_in_stage && !started_watch && uid != CCORD_ID_NONE) {
      ui_open_member_popover(ui, uid, ui->mx, ui->my, false);
    }
    return;
  }
  if (ch->kind == CCORD_CHANNEL_CATEGORY) {
    uc_toggle_collapsed(ui, ch->id);
    ui_invalidate_rows(ui);
    ui_ensure_caches(ui);
    return;
  }
  if (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE) {
    const ccord_voice_state *self = ui_self_voice(ui);
    if (self && self->channel_id == ch->id && !ui_role_preview_active_for(ui, g)) {
      if (ui->discovery_open) {
        ui->discovery_open = false;
        ui_compute_layout(ui);
      }
      if (ui->mobile_layout) ui->left_drawer_open = false;
      ui->stage_channel = ch->id;   // clicking the joined channel reopens its stage
      ui->stage_chat_open = false;
      ui->stage_armed = true;
      ui->dirty = true;
      return;
    }
    const char *lock_reason = ui_voice_lock_reason(ui, g, ch);
    if (lock_reason) {
      ui_notice(ui, "%s", lock_reason);
      ui->dirty = true;
      return;
    }
    if (ui->discovery_open) {
      ui->discovery_open = false;
      ui_compute_layout(ui);
    }
    if (ui->mobile_layout) ui->left_drawer_open = false;
    uc_join_voice_channel(ui, g, ch);
    return;
  }
  if (ui->discovery_open) {
    ui->discovery_open = false;
    ui_compute_layout(ui);
  }
  if (ui->mobile_layout) ui->left_drawer_open = false;
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  if (ui->guild_view != UI_GUILD_VIEW_NONE) {
    ui->guild_view = UI_GUILD_VIEW_NONE;
    ui->guild_view_guild_id = CCORD_ID_NONE;
    ui_compute_layout(ui);
  }
  ui->stage_channel = CCORD_ID_NONE;
  ui->stage_chat_open = false;
  ui->stage_chat_return = CCORD_ID_NONE;
  ccord_error err;
  ccord_status st = ui->d.session ? ccord_session_select_channel(ui->d.session, ch->id, &err)
                                  : ccord_store_select_channel(ui->d.store, ch->id, &err);
  if (st != CCORD_OK) ui_notice(ui, "%s", err.message);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_save_current_location(ui);
}

float uc_clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

float uc_page_step(const ccord_ui *ui)
{
  return ui->layout.message_area.h * 0.9f;
}

void uc_scroll_messages(ccord_ui *ui, float dy)
{
  if (ui->touch_scroll_direct_override > 0 &&
      ((dy > 0.0f && ui->vlist.stick_bottom) ||
       (dy < 0.0f && ui->vlist.anchor_index == 0 && ui->vlist.anchor_offset >= 0.0f))) {
    ui->touch_scroll_hit_bound = true;
  }
  if (dy < 0.0f) ui->scroll_up_pending = true;   // reaching the top will page older history in
  ccord_vlist_scroll_by(&ui->vlist, dy);
  if (dy > 0.0f && ui->vlist.stick_bottom && ui->msg_scroll_vel > 0.0f) {
    ui->msg_scroll_vel = 0.0f;
  }
  ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
  ui->dirty = true;
}

// GET /channels/{id}/pins backs the Pinned Messages popover.
void uc_request_pins(ccord_ui *ui)
{
  ui->pins_scroll = 0.0f;
  if (!ui->d.session) return;
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (ch == CCORD_ID_NONE) return;
  ccord_error err;
  if (ccord_session_request_pins(ui->d.session, ch, &err) != CCORD_OK) ccord_error_clear(&err);
}

// Ticks one panel's wheel glide by fdt_ms (scroll.h). A glide whose panel is shut is dropped, so it does not carry on
// when the panel opens again. Returns true while the glide still moves.
bool uc_glide_tick(ccord_ui *ui, ccord_scroller *s, float *offset, bool open, float fdt_ms, bool reduced)
{
  if (s->pending == 0.0f) return false;
  if (!open) {
    ccord_scroller_cancel(s);
    return false;
  }
  ui->dirty = true;
  return ccord_scroller_tick(s, offset, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced);
}

void uc_handle_wheel(ccord_ui *ui, const ccord_input_event *ev)
{
  float notch = 3.0f * ui_line_h(ui, M(font_size_base));
  float dy = ui_wheel_delta_px(ui, ev, notch);   // wheel > 0 = towards older content = content moves down
  bool direct = ui_wheel_direct(ui, ev);
  if (ui->inbox_open && ui_in_rect(ev->x, ev->y, ui->inbox_popover_rect)) {
    ui_scroller_scroll_ui(ui, &ui->inbox_glide, &ui->inbox_scroll, dy, ui->inbox_max, direct);
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui->saved_popover_open && ui_in_rect(ev->x, ev->y, ui->saved_popover_rect)) {
    ui_scroller_scroll_ui(ui, &ui->saved_glide, &ui->saved_scroll, dy, ui->saved_max, direct);
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui->reactions_open && ui_in_rect(ev->x, ev->y, ui->reactions_popover_rect)) {
    if (ev->x < ui->reactions_popover_rect.x + UI_REACTIONS_SIDE_W) {
      // The emoji tab column scrolls independently of the user list.
      ui_scroller_scroll_ui(ui, &ui->reactions_tab_glide, &ui->reactions_tab_scroll, dy, ui->reactions_tab_max, direct);
    } else {
      ui_scroller_scroll_ui(ui, &ui->reactions_glide, &ui->reactions_scroll, dy, ui->reactions_max, direct);
    }
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui->pins_open && ui_in_rect(ev->x, ev->y, ui_pins_popover_rect(ui))) {
    ui_scroller_scroll_ui(ui, &ui->pins_glide, &ui->pins_scroll, dy, ui->pins_max, direct);
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  // Jumps (10+ notches), precise (trackpad / hi-res wheel) deltas and reduced motion apply at once; whole notches glide.
  const ccord_layout *l = &ui->layout;
  if (ui->mobile_layout) {
    // Open mobile drawers overlap message_area and forum views, and own scroll gestures inside their bounds.
    if (l->left_drawer_visible) {
      if (ui_in_rect(ev->x, ev->y, ui_rect(l->channel_scroll))) {
        float max = ui->chan_content_h - l->channel_scroll.h;
        ui_scroller_scroll_ui(ui, &ui->chan_scroller, &ui->chan_scroll, dy, max, direct);
        ui->animating = true;
        ui->dirty = true;
      } else if (ui_in_rect(ev->x, ev->y, ui_rect(l->server_list))) {
        float total = 0.0f;
        (void)ui_rail_slots_cached(ui, nullptr, &total);
        float max = total - l->server_list.h;
        ui_scroller_scroll_ui(ui, &ui->server_scroller, &ui->server_scroll, dy, max, direct);
        ui->animating = true;
        ui->dirty = true;
      }
      return;
    }
    if (l->right_drawer_visible) {
      if (ui_in_rect(ev->x, ev->y, ui_rect(l->member_list))) {
        if (ui_search_panel_active(ui)) {
          float max = ui->search_panel_max;
          ui_scroller_scroll_ui(ui, &ui->search_panel_glide, &ui->search_panel_scroll, dy, max, direct);
        } else {
          float max = ui->member_content_h - l->member_list.h;
          ui_scroller_scroll_ui(ui, &ui->member_scroller, &ui->member_scroll, dy, max, direct);
        }
        ui->animating = true;
        ui->dirty = true;
      }
      return;
    }
  }
  if (ui_guild_view_active(ui) && ui_guild_view_handle_wheel(ui, ev)) {
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    return;
  }
  if (ui_thread_split_active(ui) && ui_thread_parent_handle_wheel(ui, ev)) {
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    return;
  }
  if (ui_forum_is_active(ui) && ui_forum_handle_event(ui, ev)) {
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui_is_dm_requests_view(ui) && ui_in_rect(ev->x, ev->y, ui_rect(l->message_area))) {
    float max = ui->dm_requests_content_h - l->message_area.h;
    ui_scroller_scroll_ui(ui, &ui->dm_requests_glide, &ui->dm_requests_scroll, dy, max, direct);
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui_is_dm_friends_view(ui) && ui_in_rect(ev->x, ev->y, ui_rect(l->message_area))) {
    float max = ui->dm_friends_content_h - (l->message_area.h - 48.0f);
    ui_scroller_scroll_ui(ui, &ui->dm_friends_glide, &ui->dm_friends_scroll, dy, max, direct);
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui->stage_channel != CCORD_ID_NONE && !ui_stage_mobile_chat_active(ui) && ui_in_rect(ev->x, ev->y, ui_stage_area(ui))) {
    float max = ui->stage_content_h - ui->stage_view_h;
    ui_scroller_scroll_ui(ui, &ui->stage_glide, &ui->stage_scroll, dy, max, direct);
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return;
  }
  if (ui_in_rect(ev->x, ev->y, ui_rect(l->message_area))) {
    float now = ccord_scroller_push(&ui->msg_scroller, dy, direct);
    if (now != 0.0f) uc_scroll_messages(ui, now);
  } else if (ui_in_rect(ev->x, ev->y, ui_rect(l->channel_scroll))) {
    float max = ui->chan_content_h - l->channel_scroll.h;
    ui_scroller_scroll_ui(ui, &ui->chan_scroller, &ui->chan_scroll, dy, max, direct);
  } else if (l->member_list_visible && ui_in_rect(ev->x, ev->y, ui_rect(l->member_list))) {
    if (ui_search_panel_active(ui)) {
      float max = ui->search_panel_max;   // the same bound the results draw clamps to
      ui_scroller_scroll_ui(ui, &ui->search_panel_glide, &ui->search_panel_scroll, dy, max, direct);
    } else {
      float max = ui->member_content_h - l->member_list.h;
      ui_scroller_scroll_ui(ui, &ui->member_scroller, &ui->member_scroll, dy, max, direct);
    }
  } else if (ui_in_rect(ev->x, ev->y, ui_rect(l->server_list))) {
    float total = 0.0f;
    (void)ui_rail_slots_cached(ui, nullptr, &total);
    float max = total - l->server_list.h;
    ui_scroller_scroll_ui(ui, &ui->server_scroller, &ui->server_scroll, dy, max, direct);
  } else {
    return;
  }
  ui->animating = true;
  ui->dirty = true;
}

void uc_reset_caret_blink(ccord_ui *ui)
{
  ui->caret_epoch_ms = ui->now_ms;
  ui->caret_on = true;
  ui->dirty = true;
}

// ---------------------------------------------------------------- IME composition (m6-ui-ime)

void ui_ime_preedit_clear(ccord_ui *ui)
{
  if (!ui->ime_preedit[0] && ui->ime_cursor == 0 && ui->ime_sel_len == 0) return;
  ui->ime_preedit[0] = '\0';
  ui->ime_cursor = 0;
  ui->ime_sel_len = 0;
  ui->dirty = true;
}

// Stores a bounded, valid-UTF-8 composition string and clamps the codepoint
// cursor/selection into it. Empty text (or invalid bytes) clears the preedit.
void ui_ime_preedit_set(ccord_ui *ui, const char *text, int32_t start, int32_t length)
{
  size_t n = text ? strnlen(text, sizeof ui->ime_preedit) : 0;
  if (n >= sizeof ui->ime_preedit) {
    n = sizeof ui->ime_preedit - 1u;
    while (n > 0 && !ccord_utf8_valid(text, n)) n--;   // keep whole codepoints only
  }
  if (n == 0 || !ccord_utf8_valid(text, n)) {
    ui_ime_preedit_clear(ui);
    return;
  }
  size_t cps = 0;
  for (size_t pos = 0; pos < n; pos = ccord_utf8_next(text, n, pos)) cps++;
  int32_t cursor = start > 0 ? start : 0;
  if ((size_t)cursor > cps) cursor = (int32_t)cps;
  int32_t sel = length > 0 ? length : 0;
  size_t room = cps - (size_t)cursor;
  if ((size_t)sel > room) sel = (int32_t)room;
  memcpy(ui->ime_preedit, text, n);
  ui->ime_preedit[n] = '\0';
  ui->ime_cursor = cursor;
  ui->ime_sel_len = sel;
  ui->dirty = true;
  uc_reset_caret_blink(ui);
}

void uc_paste(ccord_ui *ui)
{
  if (ui->d.host.get_clipboard_image) {
    uint8_t *png = nullptr;
    size_t png_len = 0;
    if (ui->d.host.get_clipboard_image(ui->d.host.ud, &png, &png_len) && png && png_len > 0) {
      (void)ccord_ui_attach_bytes(ui, "clipboard.png", png, png_len);
      ccord_free(png);
      return;
    }
  }
  if (!ui->d.host.clipboard_get) return;
  char *txt = ui->d.host.clipboard_get(ui->d.host.ud);
  if (!txt) return;
  // Messages are single text blocks: drop control characters except newline, turn CR into nothing.
  size_t n = strlen(txt), w = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)txt[i];
    if (ch == '\r') continue;
    if (ch < 0x20 && ch != '\n') ch = ' ';
    txt[w++] = (char)ch;
  }
  if (!uc_composer_accepts_text(ui)) {
    ccord_free(txt);
    return;
  }
  ccord_error err;
  const char *lock = ui_composer_lock_reason(ui);
  bool inserted = false;
  if (lock) ui_notice(ui, "%s", lock);
  else if (ccord_text_edit_paste(&ui->edit, txt, w, &err) != CCORD_OK) ui_notice(ui, "%s", err.message);
  else inserted = w > 0;
  ccord_free(txt);
  ui_update_autocomplete(ui);
  if (inserted) ui_send_typing(ui);
  uc_reset_caret_blink(ui);
}

bool ui_search_panel_handle_click(ccord_ui *ui, float mx, float my)
{
  if (!ui_search_panel_active(ui)) return false;
  if (!ui_in_rect(mx, my, ui_rect(ui->layout.member_list))) return false;
  for (int i = 0; i < 3; i++) {
    if (ui->search_sort_rects[i].w > 0.0f && ui_in_rect(mx, my, ui->search_sort_rects[i])) {
      if (ui->search_sort != (uint8_t)i) {
        ui->search_sort = (uint8_t)i;
        ui->search_sent_key = 0;
        ui->search_panel_scroll = 0.0f;
        uc_search_send(ui, true);
        ui->view_key = 0;
        ui_sync_messages(ui);
        ui->dirty = true;
      }
      return true;
    }
  }
  if (ui->search_prev_rect.w > 0.0f && ui_in_rect(mx, my, ui->search_prev_rect)) {
    ui_search_page_step(ui, -1);
    return true;
  }
  if (ui->search_next_rect.w > 0.0f && ui_in_rect(mx, my, ui->search_next_rect)) {
    ui_search_page_step(ui, 1);
    return true;
  }
  for (size_t i = 0; i < ui->nsearch_card_hits; i++) {
    const ui_search_card_hit *hit = &ui->search_card_hits[i];
    if (ui_in_rect(mx, my, hit->rect)) {
      ccord_id cid = hit->channel_id;
      ccord_id mid = hit->message_id;
      ui->search_open = false;
      if (ui->mobile_layout) ui->right_drawer_open = false;
      ui->view_key = 0;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      if (cid != CCORD_ID_NONE && cid != ccord_store_selected_channel(ui->d.store)) {
        const ccord_channel *tc = ccord_store_channel(ui->d.store, cid);
        ccord_error err;
        if (tc && tc->guild_id != ccord_store_selected_guild(ui->d.store)) {
          if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, tc->guild_id, &err);
          else (void)ccord_store_select_guild(ui->d.store, tc->guild_id, &err);
        }
        if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, cid, &err);
        else (void)ccord_store_select_channel(ui->d.store, cid, &err);
        ui_ensure_caches(ui);
        ui_sync_messages(ui);
        if (!ui_jump_to_message(ui, mid)) {
          ui->pending_jump_msg_id = mid;
          ui->pending_jump_channel = cid;
        }
      } else {
        ui_sync_messages(ui);
        (void)ui_jump_to_message(ui, mid);
      }
      ui->dirty = true;
      return true;
    }
  }
  return true;
}

bool ui_search_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->search_open) return false;
  if (ev->type == CCORD_IN_MOUSE_MOVE) return false;
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 3 && ui_in_rect(ev->x, ev->y, ui_search_popover_rect(ui))) {
    ui_menu_open_text_field(ui, ev->x, ev->y, &ui->search_edit, false, false, false, true);
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (ui_in_rect(ev->x, ev->y, ui_search_rect(ui))) {
      ui->search_open = false;
      if (ui->mobile_layout) ui->right_drawer_open = false;
      ui->view_key = 0;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui_sync_messages(ui);
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui_pin_rect(ui))) {
      ui->search_open = false;
      if (ui->mobile_layout) ui->right_drawer_open = false;
      ui->pins_open = true;
      uc_request_pins(ui);
      ui->view_key = 0;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui_sync_messages(ui);
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui_inbox_rect(ui))) {
      ui->search_open = false;
      if (ui->mobile_layout) ui->right_drawer_open = false;
      ui->inbox_open = true;
      ui->inbox_scroll = 0.0f;
      ui->view_key = 0;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui_sync_messages(ui);
      ui->dirty = true;
      return true;
    }
    if (ui_search_panel_handle_click(ui, ev->x, ev->y)) {
      return true;
    }
    if (ui->search_clear_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->search_clear_rect)) {
      ccord_text_edit_clear(&ui->search_edit);
      uc_search_reset(ui);
      if (ui->mobile_layout) ui->right_drawer_open = false;
      ui->view_key = 0;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui_sync_messages(ui);
      ui->dirty = true;
      return true;
    }
    for (size_t ci = 0; ci < ui->nsearch_chips; ci++) {
      if (ui_in_rect(ev->x, ev->y, ui->search_chip_rects[ci]) && ui->search_chip_tokens[ci]) {
        const char *tok = ui->search_chip_tokens[ci];
        ccord_error serr;
        // Add a leading space if the query doesn't already end with whitespace.
        const char *cur = ccord_text_edit_cstr(&ui->search_edit);
        if (ui->search_edit.len > 0 && cur[ui->search_edit.len - 1u] != ' ') {
          (void)ccord_text_edit_insert(&ui->search_edit, " ", 1, &serr);
        }
        (void)ccord_text_edit_insert(&ui->search_edit, tok, strlen(tok), &serr);
        if (strchr(tok, ':') != nullptr && tok[strlen(tok) - 1u] != ':') {
          (void)ccord_text_edit_insert(&ui->search_edit, " ", 1, &serr);
        }
        ui_sync_messages(ui);
        uc_search_send(ui, true);
        if (ui->mobile_layout && ui->search_edit.len >= 2) ui->right_drawer_open = true;
        ui->dirty = true;
        return true;
      }
    }
    if (!ui_in_rect(ev->x, ev->y, ui_search_popover_rect(ui))) {
      ui->search_open = false;
      if (ui->mobile_layout) ui->right_drawer_open = false;
      ui->view_key = 0;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui_sync_messages(ui);
      ui->dirty = true;
    }
  }
  return true;
}

bool ui_pins_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->pins_open) return false;
  if (ev->type == CCORD_IN_MOUSE_MOVE) return false;
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (ui_in_rect(ev->x, ev->y, ui_pin_rect(ui))) {
      ui->pins_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui_inbox_rect(ui))) {
      ui->pins_open = false;
      ui->inbox_open = true;
      ui->inbox_scroll = 0.0f;
      ui->dirty = true;
      return true;
    }
    ccord_rect box = ui_pins_popover_rect(ui);
    if (!ui_in_rect(ev->x, ev->y, box)) {
      ui->pins_open = false;
      ui->dirty = true;
      return true;
    }
    ccord_id ch = ccord_store_selected_channel(ui->d.store);
    if (ui->pins_load_more_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->pins_load_more_rect)) {
      if (ui->d.session) {
        ccord_error perr;
        (void)ccord_session_request_more_pins(ui->d.session, ch, &perr);
      }
      ui->dirty = true;
      return true;
    }
    for (size_t i = 0; i < ui->npins_vis; i++) {
      if (ui_in_rect(ev->x, ev->y, ui->pins_vis_rects[i])) {
        ui->pins_open = false;
        (void)ui_jump_to_message(ui, ui->pins_vis_ids[i]);
        return true;
      }
    }
    if (ui->npins_vis == 0) {
      size_t total = ccord_store_message_count(ui->d.store, ch);
      size_t seen = 0;
      for (size_t i = 0; i < total; i++) {
        const ccord_message *m = ccord_store_message_at(ui->d.store, ch, i);
        if (!m || !m->pinned) continue;
        if (seen >= UI_PIN_POPOVER_MAX) break;
        if (ui_in_rect(ev->x, ev->y, ui_pins_item_rect(ui, seen))) {
          ui->pins_open = false;
          (void)ui_jump_to_message(ui, m->id);
          return true;
        }
        seen++;
      }
    }
  }
  return true;
}

bool ui_reactions_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->reactions_open) return false;
  if (ev->type == CCORD_IN_MOUSE_MOVE) return false;
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if ((ui->reactions_close_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->reactions_close_rect)) ||
        (ui->reactions_popover_rect.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->reactions_popover_rect))) {
      ui->reactions_open = false;
      ui->dirty = true;
      return true;
    }
    for (size_t t = 0; t < ui->nreactions_tabs; t++) {
      if (ui_in_rect(ev->x, ev->y, ui->reactions_tab_rects[t])) {
        ui->reactions_tab = t;
        ui->reactions_scroll = 0.0f;
        if (ui->d.session && ui->reactions_tab_emojis[t][0]) {
          ccord_error rerr;
          (void)ccord_session_request_reaction_users(ui->d.session, ui->reactions_channel_id, ui->reactions_msg_id,
                                                     ui->reactions_tab_emojis[t], 50u,
                                                     &rerr);
        }
        ui->dirty = true;
        return true;
      }
    }
  }
  return true;
}

void ui_select_guild_and_channel(ccord_ui *ui, ccord_id gid, ccord_id cid)
{
  if (!ui || gid == CCORD_ID_NONE) return;
  if (ui->discovery_open) {
    ui->discovery_open = false;
    ui_compute_layout(ui);
  }
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  ui->stage_channel = CCORD_ID_NONE;
  ccord_error err;
  if (gid != ccord_store_selected_guild(ui->d.store)) {
    if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, gid, &err);
    else (void)ccord_store_select_guild(ui->d.store, gid, &err);
  }
  if (cid != CCORD_ID_NONE) {
    if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, cid, &err);
    else (void)ccord_store_select_channel(ui->d.store, cid, &err);
  }
  ui_invalidate_rows(ui);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_save_current_location(ui);
  ui->dirty = true;
}

void ui_record_nav_location(ccord_ui *ui)
{
  if (!ui || !ui->d.store || ui->nav_hist_suppress) return;
  ui_nav_loc cur = {
    .guild_id = ccord_store_selected_guild(ui->d.store),
    .channel_id = ccord_store_selected_channel(ui->d.store),
    .dm_friends = ui_is_dm_friends_view(ui),
    .dm_requests = ui_is_dm_requests_view(ui),
  };
  if (cur.guild_id == CCORD_ID_NONE) return;
  if (cur.guild_id != CCORD_DM_GUILD_ID && cur.channel_id == CCORD_ID_NONE) return;
  if (cur.dm_friends || cur.dm_requests) cur.channel_id = CCORD_ID_NONE;

  if (ui->nav_hist_len == 0) {
    ui->nav_hist[0] = cur;
    ui->nav_hist_len = 1u;
    ui->nav_hist_pos = 0u;
    return;
  }
  const ui_nav_loc *top = &ui->nav_hist[ui->nav_hist_pos];
  if (top->guild_id == cur.guild_id && top->channel_id == cur.channel_id &&
      top->dm_friends == cur.dm_friends && top->dm_requests == cur.dm_requests) {
    return;
  }
  ui->nav_hist_len = ui->nav_hist_pos + 1u;
  if (ui->nav_hist_len >= UI_NAV_HIST_MAX) {
    memmove(&ui->nav_hist[0], &ui->nav_hist[1], (UI_NAV_HIST_MAX - 1u) * sizeof ui->nav_hist[0]);
    ui->nav_hist_len = UI_NAV_HIST_MAX - 1u;
  }
  ui->nav_hist[ui->nav_hist_len] = cur;
  ui->nav_hist_pos = ui->nav_hist_len;
  ui->nav_hist_len++;
}

static bool nav_loc_valid(const ccord_ui *ui, ui_nav_loc t)
{
  if (t.guild_id == CCORD_ID_NONE) return false;
  if (t.guild_id == CCORD_DM_GUILD_ID) {
    return t.dm_friends || t.dm_requests || t.channel_id == CCORD_ID_NONE ||
           ccord_store_channel(ui->d.store, t.channel_id) != nullptr;
  }
  return ccord_store_guild(ui->d.store, t.guild_id) != nullptr &&
         ccord_store_channel(ui->d.store, t.channel_id) != nullptr;
}

bool ui_nav_step(ccord_ui *ui, int dir)
{
  if (!ui || !ui->d.store || dir == 0) return false;
  ui_record_nav_location(ui);
  if (ui->nav_hist_len == 0) return false;
  size_t pos = ui->nav_hist_pos;
  while (true) {
    if (dir < 0) {
      if (pos == 0u) return false;
      pos--;
    } else {
      if (pos + 1u >= ui->nav_hist_len) return false;
      pos++;
    }
    ui_nav_loc t = ui->nav_hist[pos];
    if (!nav_loc_valid(ui, t)) continue;
    ui->nav_hist_pos = pos;
    ui->nav_hist_suppress = true;
    ui->inbox_open = false;
    ui->pins_open = false;
    if (ui->discovery_open) {
      ui->discovery_open = false;
      ui_compute_layout(ui);
    }
    if (ui->search_open) {
      ui->search_open = false;
      ui->view_key = 0;
    }
    ui->stage_channel = CCORD_ID_NONE;
    ccord_error err;
    if (t.guild_id != ccord_store_selected_guild(ui->d.store)) {
      if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, t.guild_id, &err);
      else (void)ccord_store_select_guild(ui->d.store, t.guild_id, &err);
    }
    if (t.guild_id == CCORD_DM_GUILD_ID && t.dm_requests) {
      ui->dm_requests_view = true;
      ui->dm_friends_view = false;
    } else if (t.guild_id == CCORD_DM_GUILD_ID && t.dm_friends) {
      ui->dm_friends_view = true;
      ui->dm_requests_view = false;
    } else {
      ui->dm_friends_view = false;
      ui->dm_requests_view = false;
      if (t.channel_id != CCORD_ID_NONE) {
        if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, t.channel_id, &err);
        else (void)ccord_store_select_channel(ui->d.store, t.channel_id, &err);
      }
    }
    ui_invalidate_rows(ui);
    ui_ensure_caches(ui);
    ui_sync_messages(ui);
    ui_save_current_location(ui);
    ui->nav_hist_suppress = false;
    ui->dirty = true;
    return true;
  }
}

bool ui_cycle_channel(ccord_ui *ui, int dir)
{
  if (!ui || !ui->d.store || dir == 0) return false;
  ui_ensure_caches(ui);
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  ccord_id ids[CCORD_MAX_CHANNELS_PER_GUILD];
  size_t n = 0;
  for (size_t i = 0; i < ui->ncrow && n < CCORD_MAX_CHANNELS_PER_GUILD; i++) {
    const chan_row *r = &ui->crow[i];
    if (r->is_category || r->is_voice_user || r->is_relationship || r->dm_nav != DM_NAV_NONE) continue;
    const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, r->index);
    if (!ch || ch->kind != CCORD_CHANNEL_TEXT) continue;
    ids[n++] = ch->id;
  }
  if (n == 0) return false;
  ccord_id sel = (ui_is_dm_friends_view(ui) || ui_is_dm_requests_view(ui)) ? CCORD_ID_NONE
                                                                          : ccord_store_selected_channel(ui->d.store);
  size_t cur = SIZE_MAX;
  for (size_t i = 0; i < n; i++) {
    if (ids[i] == sel) {
      cur = i;
      break;
    }
  }
  size_t next = 0;
  if (cur == SIZE_MAX) {
    next = dir > 0 ? 0u : (n - 1u);
  } else {
    next = dir > 0 ? ((cur + 1u) % n) : ((cur + n - 1u) % n);
  }
  ui_select_guild_and_channel(ui, g, ids[next]);
  return true;
}

bool ui_cycle_guild(ccord_ui *ui, int dir)
{
  if (!ui || !ui->d.store || dir == 0) return false;
  size_t rows = ui_guild_rows(ui);
  if (rows <= 1u) return false;
  ccord_id sel = ccord_store_selected_guild(ui->d.store);
  size_t cur = 0u;
  if (sel != CCORD_DM_GUILD_ID) {
    size_t ng = ui_guild_real_rows(ui);
    for (size_t i = 0; i < ng; i++) {
      const ccord_guild *g = ccord_store_guild_at(ui->d.store, i);
      if (g && g->id == sel) {
        cur = i + 1u;
        break;
      }
    }
  }
  size_t next = dir > 0 ? ((cur + 1u) % rows) : ((cur + rows - 1u) % rows);
  uc_select_guild_at(ui, next);
  ui->dirty = true;
  return true;
}

bool ui_jump_unread_channel(ccord_ui *ui, int dir, bool mentions_only)
{
  if (!ui || !ui->d.store || dir == 0) return false;
  typedef struct chan_slot {
    ccord_id gid;
    ccord_id cid;
    bool match;
  } chan_slot;
  size_t ng = ccord_store_guild_count(ui->d.store);
  size_t cap = 64u;
  for (size_t pass = 0; pass <= ng; pass++) {
    ccord_id gid = (pass == 0) ? CCORD_DM_GUILD_ID : ccord_store_guild_at(ui->d.store, pass - 1)->id;
    cap += ccord_store_channel_count(ui->d.store, gid);
  }
  chan_slot *slots = ccord_alloc_array(cap, sizeof *slots);
  if (!slots) return false;

  ccord_id sel_g = ccord_store_selected_guild(ui->d.store);
  ccord_id sel_ch = (ui_is_dm_friends_view(ui) || ui_is_dm_requests_view(ui)) ? CCORD_ID_NONE
                                                                              : ccord_store_selected_channel(ui->d.store);
  size_t total = 0;
  size_t cur_idx = SIZE_MAX;
  for (size_t pass = 0; pass <= ng; pass++) {
    ccord_id gid = (pass == 0) ? CCORD_DM_GUILD_ID : ccord_store_guild_at(ui->d.store, pass - 1)->id;
    size_t nc = ccord_store_channel_count(ui->d.store, gid);
    for (size_t ci = 0; ci < nc && total < cap; ci++) {
      const ccord_channel *c = ccord_store_channel_at(ui->d.store, gid, ci);
      if (!c || c->is_message_request || c->kind != CCORD_CHANNEL_TEXT || !ui_channel_visible(ui, gid, c)) continue;
      bool match = mentions_only ? (c->mention_count > 0)
                                 : (c->mention_count > 0 || (c->unread_count > 0 && !ui_channel_muted(ui, c)));
      if (gid == sel_g && c->id == sel_ch) cur_idx = total;
      slots[total++] = (chan_slot){gid, c->id, match};
    }
  }
  if (total == 0) {
    ccord_free(slots);
    return false;
  }
  size_t chosen = SIZE_MAX;
  for (size_t step = 1u; step <= total; step++) {
    size_t idx;
    if (cur_idx == SIZE_MAX) {
      idx = dir > 0 ? ((step - 1u) % total) : ((total - (step % total)) % total);
    } else {
      idx = dir > 0 ? ((cur_idx + step) % total) : ((cur_idx + total - (step % total)) % total);
    }
    if (slots[idx].match) {
      chosen = idx;
      break;
    }
  }
  if (chosen == SIZE_MAX) {
    ccord_free(slots);
    return false;
  }
  ccord_id target_g = slots[chosen].gid;
  ccord_id target_c = slots[chosen].cid;
  ccord_free(slots);
  ui_select_guild_and_channel(ui, target_g, target_c);
  return true;
}

static bool ui_ack_channel_now(ccord_ui *ui, ccord_id gid, const ccord_channel *c)
{
  if (!ui || !c || (c->unread_count == 0 && c->mention_count == 0)) return false;
  ccord_error err;
  bool ok = false;
  if (ui->d.session) {
    size_t one = 0;
    if (ccord_session_mark_read(ui->d.session, gid, c->id, &one, &err) == CCORD_OK && one > 0) {
      ok = true;
    } else {
      const ccord_message *last = uc_latest_non_ephemeral_message(ui->d.store, c->id);
      ccord_id upto = last ? last->id : c->last_message_id;
      if (upto != CCORD_ID_NONE && ccord_session_ack_message(ui->d.session, c->id, upto, &err) == CCORD_OK) {
        ok = true;
      }
    }
  } else {
    const ccord_message *last = uc_latest_non_ephemeral_message(ui->d.store, c->id);
    ccord_id upto = last ? last->id : (c->last_message_id != CCORD_ID_NONE ? c->last_message_id : 1u);
    if (ccord_store_apply_read(ui->d.store, c->id, upto, 0u, &err) == CCORD_OK) {
      ok = true;
    }
  }
  if (ok && c->id == ccord_store_selected_channel(ui->d.store) && ui->new_marker_active) {
    ui->new_marker_active = false;
    ui->new_marker_channel = CCORD_ID_NONE;
    ui->new_marker_id = CCORD_ID_NONE;
    ui->new_marker_count = 0;
    ccord_vlist_invalidate_all(&ui->vlist);
  }
  return ok;
}

bool ui_inbox_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->inbox_open) return false;
  if (ev->type == CCORD_IN_MOUSE_MOVE) return false;
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (ui_in_rect(ev->x, ev->y, ui_inbox_rect(ui))) {
      ui->inbox_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui_pin_rect(ui))) {
      ui->inbox_open = false;
      ui->pins_open = true;
      uc_request_pins(ui);
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui_search_rect(ui))) {
      ui->inbox_open = false;
      ui->search_open = true;
      if (ui->mobile_layout) {
        ui->right_drawer_open = true;
        ui->left_drawer_open = false;
        ui_compute_layout(ui);
      }
      ccord_text_edit_clear(&ui->search_edit);
      uc_search_reset(ui);
      ui->view_key = 0;
      ui_sync_messages(ui);
      ui->dirty = true;
      return true;
    }
    if (ui->inbox_popover_rect.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->inbox_popover_rect)) {
      ui->inbox_open = false;
      ui->dirty = true;
      return true;
    }
    for (int t = 0; t < UI_INBOX_TAB_COUNT; t++) {
      if (ui->inbox_tab_rects[t].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inbox_tab_rects[t])) {
        ui->inbox_tab = t;
        ui->inbox_scroll = 0.0f;
        ui->dirty = true;
        return true;
      }
    }
    if (ui->inbox_mark_all_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inbox_mark_all_rect)) {
      if (ui->inbox_tab == 4) {
        ui->inbox_open = false;
        ui_schedule_dialog_open(ui);
        ui->dirty = true;
        return true;
      }
      size_t acked = 0;
      size_t ng = ccord_store_guild_count(ui->d.store);
      for (size_t pass = 0; pass <= ng && acked < CCORD_MARK_READ_MAX; pass++) {
        ccord_id gid = (pass == 0) ? CCORD_DM_GUILD_ID : ccord_store_guild_at(ui->d.store, pass - 1)->id;
        size_t nc = ccord_store_channel_count(ui->d.store, gid);
        for (size_t ci = 0; ci < nc && acked < CCORD_MARK_READ_MAX; ci++) {
          const ccord_channel *c = ccord_store_channel_at(ui->d.store, gid, ci);
          if (!c || c->is_message_request || c->kind != CCORD_CHANNEL_TEXT || !ui_channel_visible(ui, gid, c)) continue;
          bool want = (ui->inbox_tab == 1)
                        ? (c->mention_count > 0)
                        : (c->mention_count > 0 || (c->unread_count > 0 && !ui_channel_muted(ui, c)));
          if (!want) continue;
          if (ui_ack_channel_now(ui, gid, c)) acked++;
        }
      }
      ui->dirty = true;
      return true;
    }
    for (size_t i = 0; i < ui->ninbox_hits; i++) {
      const ui_inbox_hit *hit = &ui->inbox_hits[i];
      if (hit->ack_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, hit->ack_rect)) {
        if (hit->kind == 1u) {
          ui_saved_messages_unsave(ui, hit->channel_id, hit->message_id);
        } else if (hit->kind == 2u) {
          ui_reminder_remove(ui, (size_t)hit->aux_id);
        } else if (hit->kind == 3u) {
          if (ui->d.host.scheduled_cancel && ui->d.host.scheduled_cancel(ui->d.host.ud, hit->aux_id)) {
            ui_notice(ui, "Scheduled message cancelled");
          }
        } else {
          const ccord_channel *c = ccord_store_channel(ui->d.store, hit->channel_id);
          if (c) (void)ui_ack_channel_now(ui, hit->guild_id, c);
        }
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, hit->card_rect)) {
        ccord_id gid = hit->guild_id;
        ccord_id cid = hit->channel_id;
        ccord_id mid = hit->message_id;
        ui->inbox_open = false;
        ui_select_guild_and_channel(ui, gid, cid);
        if (mid != CCORD_ID_NONE) {
          if (!ui_jump_to_message(ui, mid)) {
            ui->pending_jump_msg_id = mid;
            ui->pending_jump_channel = cid;
          }
        }
        ui->dirty = true;
        return true;
      }
    }
  }
  return true;
}

bool ui_shortcuts_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->shortcuts_open) return false;
  if (ev->type == CCORD_IN_KEY_DOWN) {
    bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
    if (ev->key == CCORD_KEY_ESCAPE || (ctrl && ev->key == CCORD_KEY_SLASH)) {
      ui->shortcuts_open = false;
      ui->dirty = true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_WHEEL) {
    ui_scroll_offset_by(ui, &ui->shortcuts_scroll, ui_wheel_delta_px(ui, ev, 36.0f), ui->shortcuts_max);
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if ((ui->shortcuts_close_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->shortcuts_close_rect)) ||
        (ui->shortcuts_modal_rect.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->shortcuts_modal_rect))) {
      ui->shortcuts_open = false;
      ui->dirty = true;
    }
    return true;
  }
  return true;
}

