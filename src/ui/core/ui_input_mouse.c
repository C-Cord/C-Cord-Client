// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ui_core_internal.h"

bool uc_handle_left_down(ccord_ui *ui, const ccord_input_event *ev)
{
  // An open picker or autocomplete owns the clicks inside its box, before any view, rail, thread or composer
  // handler below (forum post list, stage, voice, thread buttons). A click outside it falls through as before.
  if (ui->emoji_picker_open && ui_in_rect(ev->x, ev->y, ui->emoji_picker_rect)) {
    return ui_expr_handle_event(ui, ev);
  }
  if (ui->autocomplete_open && ui_in_rect(ev->x, ev->y, ui->autocomplete_rect)) {
    float row_h = ui->mobile_layout ? 38.0f : 26.0f;
    float row = (ev->y - (ui->autocomplete_rect.y + 6.0f)) / row_h;
    if (row >= 0.0f && row < (float)ui->nautocomplete && ui_accept_autocomplete(ui, (size_t)row)) {
      uc_reset_caret_blink(ui);
    }
    return true;
  }
  if (ui_stream_pip_handle(ui, ev)) return true;
  const ccord_layout *l = &ui->layout;
  if (ui->mobile_layout && (l->left_drawer_visible || l->right_drawer_visible)) {
    bool in_left = l->left_drawer_visible && ev->x <= l->channel_list.x + l->channel_list.w;
    bool in_right = l->right_drawer_visible && ev->x >= l->member_list.x;
    if (!in_left && !in_right) {
      // Tapping the dimmed backdrop outside an open drawer dismisses it.
      ui->left_drawer_open = false;
      ui->right_drawer_open = false;
      ui->dirty = true;
      return true;
    }
    hover_state hd = uc_compute_hover(ui);
    if (in_left) {
      if (hd.rail_slot >= 0) {
        size_t nslots = 0;
        const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
        if (slots && (size_t)hd.rail_slot < nslots) {
          const ui_rail_slot *sl = &slots[hd.rail_slot];
          switch (sl->kind) {
            case UI_RAIL_FOLDER_COLLAPSED:
            case UI_RAIL_FOLDER_HEADER:
              ui_folder_toggle_expanded(ui, sl->folder_id);
              return true;
            case UI_RAIL_ADD_SERVER:
              ui->left_drawer_open = false;
              ui->modal_kind = UI_MODAL_ADD_SERVER;
              ui->modal_type_sel = 0;
              ccord_text_edit_clear(&ui->modal_edit1);
              if (ui->d.session) ccord_session_clear_invite_preview(ui->d.session);
              uc_reset_caret_blink(ui);
              ui->dirty = true;
              return true;
            case UI_RAIL_DISCOVERY:
              ui->left_drawer_open = false;
              ui->discovery_open = !ui->discovery_open;
              if (ui->discovery_open) {
                ui->inbox_open = false;
                ui->pins_open = false;
                ui->search_open = false;
                ui->view_key = 0;
                ui_compute_layout(ui);
                ui_discovery_request(ui, false);
                uc_reset_caret_blink(ui);
                ui_post_view_select(ui, ccord_store_selected_guild(ui->d.store), CCORD_NAV_VIEW_DISCOVERY);
              } else {
                ui_compute_layout(ui);
              }
              ui->dirty = true;
              return true;
            case UI_RAIL_DM:
              uc_select_guild_at(ui, 0);
              return true;
            case UI_RAIL_GUILD:
              uc_select_guild_at(ui, sl->guild_index + 1u);
              return true;
          }
        }
      }
      if (ui->dm_header_plus_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->dm_header_plus_rect)) {
        ui->left_drawer_open = false;
        ui->modal_kind = UI_MODAL_NEW_DM;
        ui->modal_focus_field = 0;
        ui->new_dm_recipient_count = 0;
        ccord_text_edit_clear(&ui->modal_edit1);
        ccord_text_edit_clear(&ui->modal_edit2);
        uc_reset_caret_blink(ui);
        ui->dirty = true;
        return true;
      }
      for (size_t i = 0; i < ui->ndm_close_rects; i++) {
        if (ui_in_rect(ev->x, ev->y, ui->dm_close_rects[i].rect)) {
          ui_close_dm(ui, ui->dm_close_rects[i].channel_id);
          ui->dirty = true;
          return true;
        }
      }
      for (size_t i = 0; i < ui->nchan_gear_rects; i++) {
        if (ui_in_rect(ev->x, ev->y, ui->chan_gear_rects[i].rect)) {
          ui->left_drawer_open = false;
          (void)ui_channel_settings_open(ui, ui->chan_gear_rects[i].channel_id);
          ui->dirty = true;
          return true;
        }
      }
      if (hd.user_status) { ui_status_card_open(ui); uc_reset_caret_blink(ui); return true; }
      if (hd.guild >= 0) { uc_select_guild_at(ui, (size_t)hd.guild); return true; }
      if (hd.crow >= 0) { uc_click_channel_row(ui, (size_t)hd.crow); return true; }
      if (hd.mute) { uc_toggle_voice_mute(ui); return true; }
      if (hd.deafen) { uc_toggle_voice_deafen(ui); return true; }
      if (hd.gear) { ui_open_settings(ui, true); return true; }
      if (hd.camera) {
        bool next = !ui->camera_on;
        if (ui->d.session) {
          ccord_error err;
          if (ccord_session_set_voice_camera(ui->d.session, next, &err) != CCORD_OK) {
            ui_notice(ui, "Cannot toggle camera: %s", err.message);
            return true;
          }
        }
        ui->camera_on = next;
        ui_notice(ui, ui->camera_on ? "Camera on" : "Camera off");
        ui->dirty = true;
        return true;
      }
      if (hd.screen) { ui->left_drawer_open = false; uc_open_go_live(ui); return true; }
      if (hd.disconnect) { uc_leave_voice(ui); return true; }
      if (hd.header) {
        ui->menu_folder_id = CCORD_ID_NONE;
        ccord_id g = ccord_store_selected_guild(ui->d.store);
        if (g == CCORD_DM_GUILD_ID) {
          ui->left_drawer_open = false;
          ui->quick_switcher_open = true;
          ccord_text_edit_clear(&ui->quick_switcher_edit);
          ui_quick_switcher_reset_list(ui);
          ui->dirty = true;
          return true;
        }
        uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_GUILD, g, 0, 0, 0, nullptr, nullptr});
        return true;
      }
      return true;
    }
    // Right drawer (member list / DM profile / group DM members)
    if (ui->dm_panel_full_profile_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->dm_panel_full_profile_rect)) {
      const ccord_channel *dch = ui_selected_channel(ui);
      const ccord_user *du = dch ? ui_dm_recipient(ui, dch) : nullptr;
      ccord_id target_uid = du ? du->id : (dch ? dch->user_id : CCORD_ID_NONE);
      if (target_uid != CCORD_ID_NONE) {
        ui->right_drawer_open = false;
        ui_open_user_profile_modal(ui, target_uid, CCORD_ID_NONE);
        uc_reset_caret_blink(ui);
      }
      return true;
    }
    if (hd.user != CCORD_ID_NONE) {
      ui_open_member_popover(ui, hd.user, ev->x, ev->y, true);
      return true;
    }
    return true;
  }
  ccord_rect split_h = ui_stage_split_handle_rect(ui);
  if (split_h.w > 0.0f && ui_in_rect(ev->x, ev->y, split_h)) {
    if (ev->clicks >= 2) {
      ui->stage_split_ratio = 0.55f;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui_settings_save(ui);
    } else {
      ui->drag = DRAG_STAGE_SPLIT;
    }
    ui->dirty = true;
    return true;
  }
  if (ui->mobile_layout && ui_in_rect(ev->x, ev->y, ui_drawer_btn_rect(ui))) {
    ui->left_drawer_open = !ui->left_drawer_open;
    ui->right_drawer_open = false;
    if (ui->left_drawer_open && ui->touch_text_focused) {
      ui->touch_text_focused = false;
      if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
    }
    if (ui->d.host.haptic_impact) (void)ui->d.host.haptic_impact(ui->d.host.ud, 0);
    ui->dirty = true;
    return true;
  }
  if (ui->mobile_layout && ui->mobile_send_btn_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->mobile_send_btn_rect)) {
    ui_send_composer(ui);
    if (ui->d.host.haptic_impact) (void)ui->d.host.haptic_impact(ui->d.host.ud, 0);
    ui->dirty = true;
    return true;
  }
  hover_state h_early = uc_compute_hover(ui);
  if (h_early.rail_slot >= 0) {
    size_t nslots = 0;
    const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
    if (slots && (size_t)h_early.rail_slot < nslots) {
      const ui_rail_slot *sl = &slots[h_early.rail_slot];
      switch (sl->kind) {
        case UI_RAIL_FOLDER_COLLAPSED:
        case UI_RAIL_FOLDER_HEADER:
          ui_folder_toggle_expanded(ui, sl->folder_id);
          return true;
        case UI_RAIL_ADD_SERVER:
          ui->modal_kind = UI_MODAL_ADD_SERVER;
          ui->modal_type_sel = 0;
          ccord_text_edit_clear(&ui->modal_edit1);
          if (ui->d.session) ccord_session_clear_invite_preview(ui->d.session);
          uc_reset_caret_blink(ui);
          ui->dirty = true;
          return true;
        case UI_RAIL_DISCOVERY:
          ui->discovery_open = !ui->discovery_open;
          if (ui->discovery_open) {
            ui->inbox_open = false;
            ui->pins_open = false;
            ui->search_open = false;
            ui->view_key = 0;
            ui_compute_layout(ui);
            ui_discovery_request(ui, false);
            uc_reset_caret_blink(ui);
            ui_post_view_select(ui, ccord_store_selected_guild(ui->d.store), CCORD_NAV_VIEW_DISCOVERY);
          } else {
            ui_compute_layout(ui);
          }
          ui->dirty = true;
          return true;
        case UI_RAIL_DM:
          if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0) {
            ui_post_nav_aux(ui, CCORD_DM_GUILD_ID, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_NAV_VIEW_GUILD);
            return true;
          }
          uc_select_guild_at(ui, 0);
          return true;
        case UI_RAIL_GUILD:
          if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0) {
            ui_post_nav_aux(ui, sl->guild_id, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_NAV_VIEW_GUILD);
            return true;
          }
          uc_select_guild_at(ui, sl->guild_index + 1u);
          if (!ui->touch_mode) {
            ui->drag = DRAG_SERVER;
            ui->drag_active = false;
            ui->drag_guild_id = sl->guild_id;
            ui->drag_start_x = ev->x;
            ui->drag_start_y = ev->y;
            ui->drag_drop_slot = -1;
            ui->drag_drop_zone = UI_DROP_NONE;
          }
          return true;
      }
    }
  }
  if (ui->update_banner_dismiss_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->update_banner_dismiss_rect)) {
    ui->update_banner_dismissed = true;
    ui->dirty = true;
    return true;
  }
  if (ui->update_banner_install_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->update_banner_install_rect)) {
    if (ui->update_info.self_updatable && ui->update_info.can_install &&
        ui->update_info.state == CCORD_UI_UPDATE_AVAILABLE && ui->d.host.update_install) {
      ui->d.host.update_install(ui->d.host.ud);
    }
    ui->dirty = true;
    return true;
  }
  if (ui->update_banner_notes_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->update_banner_notes_rect)) {
    if (ui->update_info.release_url[0] && ui->d.host.open_url) {
      (void)ui->d.host.open_url(ui->d.host.ud, ui->update_info.release_url);
    }
    return true;
  }
  if (!ui->discovery_open && ui_role_preview_active_for(ui, ccord_store_selected_guild(ui->d.store))) {
    if (ui->role_preview_exit_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->role_preview_exit_rect)) {
      ui_stop_role_preview(ui);
      return true;
    }
    if (ui->role_preview_cycle_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->role_preview_cycle_rect)) {
      ui_cycle_role_preview(ui);
      return true;
    }
  }
  if (!ui->discovery_open && ui->thread_join_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->thread_join_rect)) {
    const ccord_channel *ch = ui_selected_channel(ui);
    if (ch && ui->d.session) {
      ccord_admin_request req;
      ccord_error err;
      ccord_status st = ch->thread_joined
                          ? ccord_admin_leave_thread(&req, ch->guild_id, ch->id, &err)
                          : ccord_admin_join_thread(&req, ch->guild_id, ch->id, &err);
      if (st == CCORD_OK && ccord_session_admin(ui->d.session, &req, &err) != CCORD_OK) {
        ui_notice(ui, "Thread action failed: %s", err.message);
      }
    }
    ui->dirty = true;
    return true;
  }
  if (ui->thread_archive_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->thread_archive_rect)) {
    const ccord_channel *ch = ui_selected_channel(ui);
    if (ch && ui->d.session) {
      ccord_admin_request req;
      ccord_error err;
      if (ccord_admin_archive_thread(&req, ch->guild_id, ch->id, !ch->thread_archived, nullptr, &err) == CCORD_OK) {
        if (ccord_session_admin(ui->d.session, &req, &err) != CCORD_OK) {
          ui_notice(ui, "Couldn't update thread: %s", err.message);
        }
      }
    }
    ui->dirty = true;
    return true;
  }
  if (ui->thread_breadcrumb_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->thread_breadcrumb_rect)) {
    const ccord_channel *ch = ui_selected_channel(ui);
    if (ch && ch->parent_id != CCORD_ID_NONE) {
      ccord_error err;
      if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, ch->parent_id, &err);
      else (void)ccord_store_select_channel(ui->d.store, ch->parent_id, &err);
      ui_invalidate_rows(ui);
      ui_ensure_caches(ui);
      ui_sync_messages(ui);
      ui_save_current_location(ui);
      ui->dirty = true;
      return true;
    }
  }
  if (ui_forum_handle_header_click(ui, ev->x, ev->y)) {
    return true;
  }
  for (size_t i = 0; i < ui->ninvite_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->invite_hits[i].btn_rect)) {
      ccord_error err;
      if (ui->invite_hits[i].joined && ui->invite_hits[i].guild_id != CCORD_ID_NONE) {
        if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, ui->invite_hits[i].guild_id, &err);
        else (void)ccord_store_select_guild(ui->d.store, ui->invite_hits[i].guild_id, &err);
        ui_invalidate_rows(ui);
        ui_ensure_caches(ui);
        ui_sync_messages(ui);
        ui_save_current_location(ui);
      } else {
        ccord_admin_request req;
        if (ccord_admin_accept_invite(&req, ui->invite_hits[i].code, &err) != CCORD_OK) {
          ui_notice(ui, "Couldn't join server: %s", err.message);
        } else {
          ui_admin_submit(ui, &req);
        }
      }
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->nthread_hits; i++) {
    const ui_thread_hit *th = &ui->thread_hits[i];
    if (ui_in_rect(ev->x, ev->y, th->rect)) {
      if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0) {
        ccord_id tgid = th->guild_id != CCORD_ID_NONE ? th->guild_id : ccord_store_selected_guild(ui->d.store);
        ui_materialize_thread_hit(ui, th, tgid);
        ui_post_nav_aux(ui, tgid, th->thread_id, th->parent_id, CCORD_NAV_VIEW_THREAD);
        return true;
      }
      ccord_error err;
      if (ui->d.session) {
        if (ccord_session_open_thread(ui->d.session, th->guild_id, th->parent_id, th->thread_id, th->name, &err) != CCORD_OK) {
          ui_notice(ui, "Couldn't open thread: %s", err.message);
          return true;
        }
      } else if (ccord_store_channel(ui->d.store, th->thread_id)) {
        (void)ccord_store_select_channel(ui->d.store, th->thread_id, &err);
      }
      ui_invalidate_rows(ui);
      ui_ensure_caches(ui);
      ui_sync_messages(ui);
      ui_save_current_location(ui);
      ui->dirty = true;
      return true;
    }
  }
  if (ui_forum_is_active(ui) && ui_forum_handle_event(ui, ev)) {
    return true;
  }
  if (ui->stage_channel != CCORD_ID_NONE && !ui_stage_mobile_chat_active(ui) && ui_in_rect(ev->x, ev->y, ui_stage_area(ui))) {
    if (ui_voice_stage_handle(ui, ev)) return true;
  }
  if (ui_vm_handle_click(ui, ev->x, ev->y)) {   // mic button, and the recording / review bar
    return true;
  }
  if (ui_overlays_handle_surface_click(ui, ev->x, ev->y)) {
    return true;
  }
  if (ui_expr_handle_composer_click(ui, ev->x, ev->y)) {
    return true;
  }
  if (ui->emoji_picker_open) {
    return ui_expr_handle_event(ui, ev);
  }
  for (size_t i = 0; i < ui->nretry_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->retry_hits[i].rect)) {
      // M0-3: retry resends the failed echo with its original nonce; Discord dedupes it server-side.
      if (ui->d.session) {
        ccord_error rerr;
        (void)ccord_session_retry_message(ui->d.session, ui->retry_hits[i].channel_id,
                                          ui->retry_hits[i].message_id, &rerr);
      }
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->nreaction_hits; i++) {
    const ui_reaction_hit *hit = &ui->reaction_hits[i];
    if (ui_in_rect(ev->x, ev->y, hit->rect)) {
      if (hit->is_add_btn) {
        ui_expr_picker_open(ui, 2, hit->channel_id, hit->message_id);
      } else if (ui->d.session && hit->emoji[0]) {
        ccord_error rerr;
        (void)ccord_session_toggle_reaction(ui->d.session, hit->channel_id, hit->message_id,
                                            hit->emoji, strlen(hit->emoji), &rerr);
      }
      ui->dirty = true;
      return true;
    }
  }
  if (ui->hover_bar_msg_id != CCORD_ID_NONE) {
    ccord_id ch = ccord_store_selected_channel(ui->d.store);
    if (ui_in_rect(ev->x, ev->y, ui->hover_bar_reply_rect)) {
      ui_begin_reply(ui, ch, ui->hover_bar_msg_id);
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui->hover_bar_edit_rect)) {
      // The same path as the context menu's "Edit Message": own messages only.
      const ccord_message *m = ccord_store_find_message(ui->d.store, ch, ui->hover_bar_msg_id);
      if (ui_message_editable_by_self(ui, m)) ui_begin_edit(ui, ch, ui->hover_bar_msg_id);
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui->hover_bar_pin_rect)) {
      if (ui->d.session) {
        ccord_error perr;
        (void)ccord_session_toggle_pin(ui->d.session, ch, ui->hover_bar_msg_id, &perr);
      }
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui->hover_bar_more_rect)) {
      ccord_id g = ccord_store_selected_guild(ui->d.store);
      const ccord_message *m = ccord_store_find_message(ui->d.store, ch, ui->hover_bar_msg_id);
      uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_MESSAGE, g, ch, ui->hover_bar_msg_id, m ? m->author_id : 0, nullptr, nullptr});
      return true;
    }
  }
  for (size_t i = 0; i < ui->nephem_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->ephem_hits[i].rect)) {
      ui_dismiss_ephemeral(ui, ui->ephem_hits[i].channel_id, ui->ephem_hits[i].message_id);
      return true;
    }
  }
  for (size_t i = 0; i < ui->staged_file_count; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->staged_spoiler_rects[i])) {
      (void)ccord_ui_toggle_attachment_spoiler(ui, i);
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui->staged_remove_rects[i])) {
      (void)ccord_ui_remove_staged_attachment(ui, i);
      return true;
    }
  }
  if (ui_in_rect(ev->x, ev->y, ui->composer_attach_rect)) {
    ui_plus_menu_open(ui);
    return true;
  }
  hover_state h = uc_compute_hover(ui);
  if (h.user_status) {
    ui_status_card_open(ui);
    uc_reset_caret_blink(ui);
    return true;
  }
  if (!ui->discovery_open && ui->dm_panel_full_profile_rect.w > 0.0f &&
      ui_in_rect(ev->x, ev->y, ui->dm_panel_full_profile_rect)) {
    const ccord_channel *dch = ui_selected_channel(ui);
    const ccord_user *du = dch ? ui_dm_recipient(ui, dch) : nullptr;
    ccord_id target_uid = du ? du->id : (dch ? dch->user_id : CCORD_ID_NONE);
    if (target_uid != CCORD_ID_NONE) {
      ui_open_user_profile_modal(ui, target_uid, CCORD_ID_NONE);
      uc_reset_caret_blink(ui);
      return true;
    }
  }
  if (!ui->discovery_open && ui->gdm_add_btn_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->gdm_add_btn_rect)) {
    const ccord_channel *gch = ui_selected_channel(ui);
    if (gch) {
      ui->modal_kind = UI_MODAL_ADD_DM_RECIPIENT;
      ui->modal_target_id = gch->id;
      ui->modal_focus_field = 0;
      ui->new_dm_recipient_count = 0;
      ccord_text_edit_clear(&ui->modal_edit1);
      uc_reset_caret_blink(ui);
      ui->dirty = true;
      return true;
    }
  }
  if (!ui->discovery_open && ui->gdm_edit_btn_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->gdm_edit_btn_rect)) {
    const ccord_channel *gch = ui_selected_channel(ui);
    if (gch) {
      ui->modal_kind = UI_MODAL_EDIT_GROUP_DM;
      ui->modal_target_id = gch->id;
      ui->modal_focus_field = 0;
      ccord_text_edit_clear(&ui->modal_edit1);
      ccord_text_edit_clear(&ui->modal_edit2);
      ccord_error eerr;
      if (gch->name && gch->name[0]) {
        (void)ccord_text_edit_insert(&ui->modal_edit1, gch->name, strlen(gch->name), &eerr);
      }
      if (gch->icon_url && gch->icon_url[0]) {
        (void)ccord_text_edit_insert(&ui->modal_edit2, gch->icon_url, strlen(gch->icon_url), &eerr);
      }
      uc_reset_caret_blink(ui);
      ui->dirty = true;
      return true;
    }
  }
  if (!ui->discovery_open && l->member_list_visible &&
      ccord_store_selected_guild(ui->d.store) == CCORD_DM_GUILD_ID &&
      ui_in_rect(ev->x, ev->y, ui_rect(l->member_list))) {
    for (size_t i = 0; i < ui->ngdm_member_hits; i++) {
      if (ui_in_rect(ev->x, ev->y, ui->gdm_member_hits[i].rect)) {
        ccord_id muid = ui->gdm_member_hits[i].user_id;
        ui->member_popover_open = true;
        ui->member_popover_user_id = muid;
        ui->member_popover_tab = 0;
        if (ui->d.session) {
          ccord_error perr;
          (void)ccord_session_request_profile_ex(ui->d.session, muid, CCORD_ID_NONE, ui->now_ms, false, &perr);
        }
        float cw = 280.0f, ch_h = 280.0f;
        if (cw > (float)ui->win_w - 16.0f && ui->win_w > 32) cw = (float)ui->win_w - 16.0f;
        if (ch_h > (float)ui->win_h - 16.0f && ui->win_h > 32) ch_h = (float)ui->win_h - 16.0f;
        float cx = uc_clampf(l->member_list.x - cw - 8.0f, 8.0f, fmaxf((float)ui->win_w - cw - 8.0f, 8.0f));
        float cy = uc_clampf(ev->y - 40.0f, 8.0f, fmaxf((float)ui->win_h - ch_h - 8.0f, 8.0f));
        ui->member_popover_rect = (ccord_rect){cx, cy, cw, ch_h};
        ui->dirty = true;
        return true;
      }
    }
  }
  if (ui_is_dm_requests_view(ui)) {
    for (int t = 0; t < 2; t++) {
      if (ui->dm_requests_tab_rects[t].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->dm_requests_tab_rects[t])) {
        ui->dm_requests_tab = t;
        ui->dm_requests_scroll = 0.0f;
        ui->dirty = true;
        return true;
      }
    }
    for (size_t i = 0; i < ui->ndm_request_hits; i++) {
      auto hit = &ui->dm_request_hits[i];
      if (hit->accept_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, hit->accept_rect)) {
        ccord_admin_request req;
        ccord_error rerr;
        if (ccord_admin_accept_message_request(&req, hit->channel_id, &rerr) == CCORD_OK) {
          ui_admin_submit(ui, &req);
        }
        ui->dirty = true;
        return true;
      }
      if (hit->reject_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, hit->reject_rect)) {
        ccord_admin_request req;
        ccord_error rerr;
        if (ccord_admin_reject_message_request(&req, hit->channel_id, &rerr) == CCORD_OK) {
          ui_admin_submit(ui, &req);
        }
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, hit->card_rect)) {
        ccord_id cid = hit->channel_id;
        if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0) {
          ui_post_nav_aux(ui, CCORD_DM_GUILD_ID, cid, CCORD_ID_NONE, CCORD_NAV_VIEW_CHANNEL);
          return true;
        }
        ui->dm_requests_view = false;
        ui->dm_friends_view = false;
        ui->stage_channel = CCORD_ID_NONE;
        ccord_error serr;
        if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, cid, &serr);
        else (void)ccord_store_select_channel(ui->d.store, cid, &serr);
        ui_invalidate_rows(ui);
        ui_ensure_caches(ui);
        ui_sync_messages(ui);
        ui->dirty = true;
        return true;
      }
    }
  }
  if (ui->mv.composer_btn.w > 0.0f) {   // the composer is a membership-screening banner: its button reopens the rules
    if (ui_in_rect(ev->x, ev->y, ui->mv.composer_btn)) {
      ui_member_verification_open(ui, ccord_store_selected_guild(ui->d.store));
      return true;
    }
  }
  if (ui->msg_req_banner_visible) {
    const ccord_channel *mch = ui_selected_channel(ui);
    if (mch && ui_in_rect(ev->x, ev->y, ui->msg_req_banner_accept_rect)) {
      ccord_admin_request req;
      ccord_error rerr;
      if (ccord_admin_accept_message_request(&req, mch->id, &rerr) == CCORD_OK) {
        ui_admin_submit(ui, &req);
      }
      ui->dirty = true;
      return true;
    }
    if (mch && ui_in_rect(ev->x, ev->y, ui->msg_req_banner_reject_rect)) {
      ccord_admin_request req;
      ccord_error rerr;
      if (ccord_admin_reject_message_request(&req, mch->id, &rerr) == CCORD_OK) {
        ui_admin_submit(ui, &req);
      }
      ui->dm_requests_view = true;
      ui->dm_friends_view = false;
      ui->dirty = true;
      return true;
    }
  }
  if (ui_is_dm_friends_view(ui)) {
    if (ui_in_rect(ev->x, ev->y, ui->add_friend_btn_rect)) {
      ui->modal_kind = UI_MODAL_ADD_FRIEND;
      ccord_text_edit_clear(&ui->modal_edit1);
      ui->dirty = true;
      return true;
    }
    for (int t = 0; t < 4; t++) {
      if (ui_in_rect(ev->x, ev->y, ui->dm_friends_tab_rects[t])) {
        ui->dm_friends_tab = t;
        ui->dm_friends_scroll = 0.0f;
        ui->dirty = true;
        return true;
      }
    }
    for (size_t i = 0; i < ui->nfriend_row_hits; i++) {
      auto hit = &ui->friend_row_hits[i];
      if (hit->accept_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, hit->accept_rect)) {
        ui_set_user_relationship(ui, hit->user_id, CCORD_RELATIONSHIP_FRIEND);
        return true;
      }
      if (hit->remove_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, hit->remove_rect)) {
        ui_set_user_relationship(ui, hit->user_id, CCORD_RELATIONSHIP_NONE);
        return true;
      }
      if (hit->kind == CCORD_RELATIONSHIP_FRIEND && ui_in_rect(ev->x, ev->y, hit->msg_rect)) {
        ui_open_dm_with_user(ui, hit->user_id);
        return true;
      }
    }
  }
  if (ui_nmb_click(ui, ev->x, ev->y)) return true;
  if (ui->jump_present_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->jump_present_rect)) {
    ui->msg_scroll_vel = 0.0f;
    ccord_vlist_reset_bottom(&ui->vlist);
    ui->dirty = true;
    return true;
  }
  if (ui_blocked_toggle_at(ui, ev->x, ev->y)) return true;
  for (size_t i = 0; i < ui->nreply_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->reply_hits[i].rect)) {
      (void)ui_jump_to_message(ui, ui->reply_hits[i].target_msg_id);
      return true;
    }
  }
  for (size_t i = 0; i < ui->ncode_copy_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->code_copy_hits[i].rect)) {
      if (ui->d.host.clipboard_set) {
        const char *raw = ui->code_copy_hits[i].code ? ui->code_copy_hits[i].code : "";
        size_t rlen = strlen(raw);
        char stack_strip[1024];
        char *strip_heap = rlen >= sizeof stack_strip ? ccord_malloc(rlen + 1u) : nullptr;
        char *strip_buf = strip_heap ? strip_heap : stack_strip;
        size_t strip_cap = strip_heap ? (rlen + 1u) : sizeof stack_strip;
        size_t w = 0;
        for (size_t p = 0; p < rlen && w + 1u < strip_cap;) {
          if (raw[p] == '\x1b' && p + 1u < rlen && raw[p + 1u] == '[') {
            size_t q = p + 2u;
            while (q < rlen && (unsigned char)raw[q] >= 0x30u && (unsigned char)raw[q] <= 0x3Fu) q++;
            while (q < rlen && (unsigned char)raw[q] >= 0x20u && (unsigned char)raw[q] <= 0x2Fu) q++;
            if (q < rlen && (unsigned char)raw[q] >= 0x40u && (unsigned char)raw[q] <= 0x7Eu) {
              p = q + 1u;
              continue;
            }
          }
          strip_buf[w++] = raw[p++];
        }
        strip_buf[w] = '\0';
        const char *to_copy = strip_buf;
        char stack_mask[1024];
        char *mask_heap = nullptr;
        if (ui_streamer_invites_active(ui)) {
          size_t mcap = w + 1u;
          char *mbuf = mcap <= sizeof stack_mask ? stack_mask : (mask_heap = ccord_malloc(mcap));
          if (mbuf && ui_streamer_mask_invites(ui, strip_buf, w, mbuf, mcap, false) > 0) {
            to_copy = mbuf;
          }
        }
        ui->d.host.clipboard_set(ui->d.host.ud, to_copy);
        ccord_free(mask_heap);
        ccord_free(strip_heap);
      }
      ui_notice(ui, "Copied code block");
      return true;
    }
  }
  for (size_t i = 0; i < ui->nspoiler_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->spoiler_hits[i].rect)) {
      ccord_id mid = ui->spoiler_hits[i].msg_id;
      bool already = false;
      for (size_t k = 0; k < ui->nrevealed_spoilers; k++) {
        if (ui->revealed_spoilers[k] == mid) { already = true; break; }
      }
      if (!already && ui->nrevealed_spoilers < 64) {
        ui->revealed_spoilers[ui->nrevealed_spoilers++] = mid;
        ui_chat_caches_clear(ui);
        ccord_vlist_invalidate_all(&ui->vlist);
      }
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->npoll_hits; i++) {
    const ui_poll_hit *ph = &ui->poll_hits[i];
    if (ui_in_rect(ev->x, ev->y, ph->rect)) {
      if (ph->closed) return true;
      const ccord_message *pm = ccord_store_find_message(ui->d.store, ph->channel_id, ph->message_id);
      if (!pm || !pm->rich || !pm->rich->poll.present) return true;
      const ccord_poll *poll = &pm->rich->poll;
      uint32_t next_ids[10];
      size_t nnext = 0;
      bool clicked_voted = false;
      for (size_t k = 0; k < poll->answer_count; k++) {
        if (poll->answers[k].id == ph->answer_id) clicked_voted = poll->answers[k].me_voted;
      }
      if (poll->multiselect) {
        for (size_t k = 0; k < poll->answer_count && nnext < 10; k++) {
          bool sel = poll->answers[k].me_voted;
          if (poll->answers[k].id == ph->answer_id) sel = !sel;
          if (sel) next_ids[nnext++] = poll->answers[k].id;
        }
      } else if (!clicked_voted) {
        next_ids[nnext++] = ph->answer_id;
      }
      if (!ui->d.session) {
        ui_notice(ui, "Not connected");
      } else {
        ccord_error verr;
        if (ccord_session_vote_poll(ui->d.session, ph->channel_id, ph->message_id, next_ids, nnext, &verr) != CCORD_OK) {
          ui_notice(ui, "Couldn't vote: %s", verr.message);
        }
      }
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->ncomp_hits; i++) {
    const ui_comp_hit *ch = &ui->comp_hits[i];
    if (ui_in_rect(ev->x, ev->y, ch->rect)) {
      if (ch->disabled) return true;
      if (ch->kind == CCORD_COMP_BUTTON) {
        if (ch->style == CCORD_BTN_LINK) {
          if (ch->url[0]) (void)ui_activate_link(ui, ch->url);
          return true;
        }
        if (ch->style == CCORD_BTN_PREMIUM) {
          (void)ui_activate_link(ui, "https://discord.com");
          return true;
        }
        if (!ui->d.session) {
          ui_notice(ui, "Not connected");
          return true;
        }
        ccord_interaction_request req = {
          .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
          .application_id = ch->application_id,
          .channel_id = ch->channel_id,
          .message_id = ch->message_id,
          .message_flags = ch->message_flags,
          .component_type = (uint8_t)CCORD_COMP_BUTTON,
          .custom_id = ch->custom_id,
        };
        ccord_error ierr;
        if (ccord_session_send_interaction(ui->d.session, &req, &ierr) != CCORD_OK) {
          ui_notice(ui, "Couldn't use button: %s", ierr.message);
        }
        ui->dirty = true;
        return true;
      }
      ui_open_select_dropdown(ui, ch);
      return true;
    }
  }
  for (size_t i = 0; i < ui->nmedia_hits; i++) {
    const ui_media_hit *mh = &ui->media_hits[i];
    if (mh->open_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, mh->open_rect)) {
      ui_activate_link(ui, mh->url);
      return true;
    }
    if (mh->play_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, mh->play_rect)) {
      if (strcmp(ui->media_url, mh->url) != 0 || ui->media_att_id != mh->att_id) {
        uc_media_player_select(ui, mh);
        ui->media_playing = !ui->media_failed;
      } else {
        if (ui->media_failed) {
          uc_media_player_select(ui, mh);   // try the download again
          ui->media_playing = !ui->media_failed;
        } else {
          if (!ui->media_playing && ui->media_pos_ms >= ui->media_duration_ms) uc_media_player_seek(ui, 0);
          ui->media_playing = !ui->media_playing;
          ui->media_audio_resync = true;
        }
      }
      ui->dirty = true;
      return true;
    }
    if (mh->scrub_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, mh->scrub_rect)) {
      if (strcmp(ui->media_url, mh->url) != 0 || ui->media_att_id != mh->att_id) {
        uc_media_player_select(ui, mh);
      }
      float frac = uc_clampf((ev->x - mh->scrub_rect.x) / mh->scrub_rect.w, 0.0f, 1.0f);
      uc_media_player_seek(ui, (uint32_t)lroundf(frac * (float)ui->media_duration_ms));
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->nimg_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->img_hits[i].rect)) {
      ui_open_lightbox(ui, ui->img_hits[i].urls, ui->img_hits[i].count, ui->img_hits[i].index);
      return true;
    }
  }
  for (size_t i = 0; i < ui->nchan_hits; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->chan_hits[i].rect)) {
      ccord_id cid = ui->chan_hits[i].channel_id;
      const ccord_channel *c = ccord_store_channel(ui->d.store, cid);
      if (c) {
        if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0 && c->kind == CCORD_CHANNEL_TEXT) {
          bool is_th = ccord_discord_type_is_thread(c->discord_type);
          ui_post_nav_aux(ui, c->guild_id, cid, is_th ? c->parent_id : CCORD_ID_NONE,
                          is_th ? CCORD_NAV_VIEW_THREAD : CCORD_NAV_VIEW_CHANNEL);
          return true;
        }
        ccord_error err;
        if (c->guild_id != ccord_store_selected_guild(ui->d.store)) {
          if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, c->guild_id, &err);
          else (void)ccord_store_select_guild(ui->d.store, c->guild_id, &err);
        }
        ui->dm_friends_view = false;
        ui->dm_requests_view = false;
        if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, cid, &err);
        else (void)ccord_store_select_channel(ui->d.store, cid, &err);
        ui_ensure_caches(ui);
        ui_sync_messages(ui);
        ui_save_current_location(ui);
        ui->dirty = true;
      }
      return true;
    }
  }
  if (ui->dm_header_plus_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->dm_header_plus_rect)) {
    ui->modal_kind = UI_MODAL_NEW_DM;
    ui->modal_focus_field = 0;
    ui->new_dm_recipient_count = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    ccord_text_edit_clear(&ui->modal_edit2);
    uc_reset_caret_blink(ui);
    ui->dirty = true;
    return true;
  }
  for (size_t i = 0; i < ui->ndm_close_rects; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->dm_close_rects[i].rect)) {
      ui_close_dm(ui, ui->dm_close_rects[i].channel_id);   // the DM leaves the list when Discord confirms
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->nchan_gear_rects; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->chan_gear_rects[i].rect)) {
      (void)ui_channel_settings_open(ui, ui->chan_gear_rects[i].channel_id);
      ui->dirty = true;
      return true;
    }
  }
  if (h.link >= 0 && (size_t)h.link < ui->nlinks) {
    ui_activate_link(ui, ui->links[h.link].url);
    return true;
  }
  if ((h.mrow >= 0 || (!h.sb && ui_in_rect(ev->x, ev->y, ui_rect(l->message_area)))) && h.user != CCORD_ID_NONE) {
    ui_open_member_popover(ui, h.user, ev->x, ev->y, h.mrow >= 0);
    return true;
  }
  if (h.guild >= 0) {
    uc_select_guild_at(ui, (size_t)h.guild);
    return true;
  }
  if (h.crow >= 0) {
    if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0 &&
        uc_aux_open_channel_row(ui, (size_t)h.crow)) {
      return true;
    }
    ccord_id g = ccord_store_selected_guild(ui->d.store);
    ccord_id drag_cid = CCORD_ID_NONE;
    if ((size_t)h.crow < ui->ncrow && !ui->crow[h.crow].is_category && !ui->crow[h.crow].is_voice_user &&
        !ui->crow[h.crow].is_relationship && !ui->crow[h.crow].is_thread && ui->crow[h.crow].dm_nav == DM_NAV_NONE &&
        uc_can_reorder_guild_channels(ui, g)) {
      const ccord_channel *dc = ccord_store_channel_at(ui->d.store, g, ui->crow[h.crow].index);
      if (dc && dc->kind != CCORD_CHANNEL_CATEGORY && !ccord_discord_type_is_thread(dc->discord_type)) {
        drag_cid = dc->id;
      }
    }
    uc_click_channel_row(ui, (size_t)h.crow);
    if (drag_cid != CCORD_ID_NONE && !ui->touch_mode) {
      ui->drag = DRAG_CHANNEL;
      ui->drag_active = false;
      ui->drag_chan_id = drag_cid;
      ui->drag_start_x = ev->x;
      ui->drag_start_y = ev->y;
      ui->drag_chan_target_id = CCORD_ID_NONE;
      ui->drag_chan_parent_id = CCORD_ID_NONE;
    }
    return true;
  }
  if (h.mute) {
    uc_toggle_voice_mute(ui);
    return true;
  }
  if (h.deafen) {
    uc_toggle_voice_deafen(ui);
    return true;
  }
  if (h.gear) {
    ui_open_settings(ui, true);
    return true;
  }
  if (h.camera) {
    bool next = !ui->camera_on;
    if (ui->d.session) {
      ccord_error err;
      if (ccord_session_set_voice_camera(ui->d.session, next, &err) != CCORD_OK) {
        ui_notice(ui, "Cannot toggle camera: %s", err.message);
        return true;
      }
    }
    ui->camera_on = next;
    ui_notice(ui, ui->camera_on ? "Camera on" : "Camera off");
    ui->dirty = true;
    return true;
  }
  if (h.screen) {
    uc_open_go_live(ui);
    return true;
  }
  if (h.disconnect) {
    uc_leave_voice(ui);
    return true;
  }
  if (h.search) {
    ui->search_open = !ui->search_open;
    ui->pins_open = false;
    ui->inbox_open = false;
    ui->saved_popover_open = false;
    ui->go_live_open = false;
    if (ui->mobile_layout) {
      ui->right_drawer_open = ui->search_open;
      if (ui->search_open) ui->left_drawer_open = false;
      ui_compute_layout(ui);
    }
    ccord_text_edit_clear(&ui->search_edit);
    uc_search_reset(ui);
    ui->view_key = 0;
    ui_sync_messages(ui);
    ui->dirty = true;
    return true;
  }
  if (h.pin) {
    ui->pins_open = !ui->pins_open;
    ui->search_open = false;
    ui->inbox_open = false;
    ui->saved_popover_open = false;
    ui->go_live_open = false;
    if (ui->pins_open) uc_request_pins(ui);
    ui->dirty = true;
    return true;
  }
  if (h.inbox) {
    ui->inbox_open = !ui->inbox_open;
    ui->inbox_scroll = 0.0f;
    ui->pins_open = false;
    ui->search_open = false;
    ui->saved_popover_open = false;
    ui->go_live_open = false;
    ui->dirty = true;
    return true;
  }
  if (h.saved) {
    ui_saved_messages_toggle_popover(ui);
    ui->go_live_open = false;
    return true;
  }
  if (h.voice_back) {
    // Text Chat: toggle the side-by-side pane showing the connected voice channel's own chat; the stage and the voice
    // connection stay up either way.
    if (ui->stage_chat_open) {
      uc_stage_chat_leave(ui);
      ui_ensure_caches(ui);
      ui_sync_messages(ui);
    } else {
      const ccord_channel *t = uc_stage_chat_target(ui);
      if (!t) {
        ui_notice(ui, "No chat available for this channel");
        return true;
      }
      ccord_id prev = ccord_store_selected_channel(ui->d.store);
      if (t->id != prev) {
        ccord_error err;
        ccord_status st = uc_select_channel_any(ui, t->id, &err);
        if (st != CCORD_OK) {
          ui_notice(ui, "%s", err.message);
          return true;
        }
        const ccord_channel *pc = ccord_store_channel(ui->d.store, prev);
        ui->stage_chat_return = pc && pc->kind == CCORD_CHANNEL_TEXT ? prev : CCORD_ID_NONE;
      }
      ui->stage_chat_open = true;
      ui_ensure_caches(ui);
      ui_sync_messages(ui);
    }
    ui_compute_layout(ui);
    ccord_vlist_invalidate_all(&ui->vlist);   // the chat pane changed width
    ui->dirty = true;
    return true;
  }
  if (h.members) {
    if (ui->mobile_layout) {
      ui->right_drawer_open = !ui->right_drawer_open;
      ui->left_drawer_open = false;
      if (ui->right_drawer_open && ui->touch_text_focused) {
        ui->touch_text_focused = false;
        if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
      }
      if (ui->d.host.haptic_impact) (void)ui->d.host.haptic_impact(ui->d.host.ud, 0);
      ui->hover_members_btn = false;
      ui->dirty = true;
      return true;
    }
    if (ui_panel_is_profile(ui)) {
      ui->dm_profile_hidden = !ui->dm_profile_hidden;
    } else {
      ui->members_hidden = !ui->members_hidden;
    }
    ui_settings_save(ui);   // both states persist across restarts
    ui_compute_layout(ui);
    ccord_vlist_invalidate_all(&ui->vlist);   // the chat column changed width
    ui->hover_members_btn = false;
    ui->dirty = true;
    return true;
  }
  if (h.header) {
    ui->menu_folder_id = CCORD_ID_NONE;
    ccord_id g = ccord_store_selected_guild(ui->d.store);
    if (g == CCORD_DM_GUILD_ID) {
      ui->quick_switcher_open = true;
      ccord_text_edit_clear(&ui->quick_switcher_edit);
      ui_quick_switcher_reset_list(ui);
      ui->dirty = true;
      return true;
    }
    uc_open_menu_at(ui, ev, (ccord_menu_context){CCORD_MENU_GUILD, g, 0, 0, 0, nullptr, nullptr});
    return true;
  }
  if (h.sb && ui->thumb_h > 0.0f) {
    if (ev->y >= ui->thumb_top && ev->y < ui->thumb_top + ui->thumb_h) {
      ui->drag = DRAG_SCROLLBAR;
      ui->drag_grab = ev->y - ui->thumb_top;
    } else {
      uc_scroll_messages(ui, ev->y < ui->thumb_top ? -uc_page_step(ui) : uc_page_step(ui));
    }
    return true;
  }
  ccord_rect box = ui_composer_box(ui);
  if (ui->reply_message != CCORD_ID_NONE) {
    ccord_rect banner = {box.x, box.y, box.w, 28.0f};
    if (ui->reply_ping_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->reply_ping_rect)) {
      ui->reply_ping = !ui->reply_ping;
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, banner)) {
      ui_cancel_reply(ui);
      return true;
    }
    box.y += 28.0f;
    box.h -= 28.0f;
  }
  if (ui->staged_file_count > 0) {
    size_t cols = ui->staged_file_count <= 2u ? ui->staged_file_count : (ui->staged_file_count <= 6u ? 3u : 5u);
    size_t frows = (ui->staged_file_count + cols - 1u) / cols;
    float fh = 28.0f * (float)frows;
    box.y += fh;
    box.h -= fh;
  }
  if (!ui->msg_req_banner_visible && ui->mv.composer_btn.w <= 0.0f && !ui_is_dm_requests_view(ui) &&
      ui_in_rect(ev->x, ev->y, box)) {
    ui->chat_sel_active = false;
    ui->touch_text_focused = true;
    ui->pending_click = true;
    ui->pending_extend = (ev->mods & CCORD_MOD_SHIFT) != 0;
    ui->pending_select_mode = ev->clicks >= 3 ? 2 : (ev->clicks == 2 ? 1 : 0);
    ui->pending_x = ev->x;
    ui->pending_y = ev->y;
    if (!ui->touch_mode) ui->drag = DRAG_COMPOSER;
    uc_reset_caret_blink(ui);
    return true;
  }
  if ((ui->stage_channel == CCORD_ID_NONE || ui_stage_chat_active(ui)) && !ui_is_dm_friends_view(ui) &&
      !ui_is_dm_requests_view(ui) && ui_in_rect(ev->x, ev->y, ui_rect(l->message_area))) {
    if (ui->touch_mode) {
      if (ui->touch_text_focused) {
        ui->touch_text_focused = false;
        if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
        ui->dirty = true;
      }
      return true;
    }
    ui->chat_sel_active = true;
    ui->chat_sel_channel = ccord_store_selected_channel(ui->d.store);
    ui->chat_sel_ax = ui->chat_sel_cx = ev->x;
    ui->chat_sel_ay = ui->chat_sel_cy = ev->y;
    ui->chat_sel_len = 0;
    ui->chat_sel_buf[0] = '\0';
    ui->drag = DRAG_CHAT_SELECT;
    ui->dirty = true;
    return true;
  }
  (void)l;
  return false;
}

