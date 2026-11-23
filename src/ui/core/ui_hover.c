// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ui_core_internal.h"

// ---------------------------------------------------------------- hover and cursor


hover_state uc_compute_hover(ccord_ui *ui)
{
  hover_state h = {.guild = -1, .rail_slot = -1, .crow = -1, .mrow = -1, .settings_page = -1, .menu = -1,
                   .link = -1, .ts_hit = -1, .msg = CCORD_ID_NONE, .user = CCORD_ID_NONE};
  if (!ui->mouse_in) return h;
  float x = ui->mx, y = ui->my;
  const ccord_layout *l = &ui->layout;
  if (ui->menu.open) {
    h.menu = ui->menu.hover;
    return h;
  }
  if (ui->settings_open || ui->go_live_open || ui_server_settings_is_open(ui) || ui_channel_settings_is_open(ui)) {
    if (ui->settings_open) {
      h.settings_page = ui->hover_settings_page;
      h.close = ui->hover_settings_close;
    }
    return h;
  }
  if (ui->stream_fullscreen || ui->drag == DRAG_STREAM_STAGE_H || ui->drag == DRAG_STAGE_SPLIT ||
      ui->drag == DRAG_STREAM_PIP_MOVE || ui->drag == DRAG_STREAM_PIP_RESIZE) {
    return h;
  }
  if (ui_stream_pip_active(ui) && ui->stream_pip_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_rect)) {
    return h;
  }
  bool left_vis = !ui->mobile_layout || l->left_drawer_visible;
  bool right_over = ui->mobile_layout && l->right_drawer_visible;
  if (left_vis && ui_in_rect(x, y, ui_rect(l->server_list))) {
    size_t nslots = 0;
    const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
    for (size_t si = 0; slots && si < nslots; si++) {
      if (ui_in_rect(x, y, slots[si].rect)) {
        h.rail_slot = (int)si;
        if (slots[si].kind == UI_RAIL_DM) h.guild = 0;
        else if (slots[si].kind == UI_RAIL_GUILD) h.guild = (int)slots[si].guild_index + 1;
        break;
      }
    }
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_rect(l->channel_scroll))) {
    float cy = y - l->channel_scroll.y + ui->chan_scroll;
    for (size_t i = 0; i < ui->ncrow; i++) {
      if (cy >= ui->crow[i].y && cy < ui->crow[i].y + ui->crow[i].h) {
        h.crow = (int)i;
        if (ui->crow[i].is_relationship && !ui->crow[i].is_category) {
          h.user = ui->crow[i].user_id;
        }
        break;
      }
    }
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_rect(l->guild_header))) {
    h.header = true;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_user_btn_rect(ui, 2))) {
    h.gear = true;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_user_btn_rect(ui, 1))) {
    h.deafen = true;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_user_btn_rect(ui, 0))) {
    h.mute = true;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_user_status_area_rect(ui))) {
    h.user_status = true;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_voice_btn_rect(ui, 2))) {
    h.disconnect = ui_voice_bar_rect(ui).w > 0.0f;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_voice_btn_rect(ui, 1))) {
    h.screen = ui_voice_bar_rect(ui).w > 0.0f;
    return h;
  } else if (left_vis && ui_in_rect(x, y, ui_voice_btn_rect(ui, 0))) {
    h.camera = ui_voice_bar_rect(ui).w > 0.0f;
    return h;
  } else if (ui->mobile_layout && l->left_drawer_visible) {
    return h;   // dimmed scrim covers chat while left drawer is open
  } else if (right_over && !ui_in_rect(x, y, ui_rect(l->member_list))) {
    return h;   // dimmed scrim covers chat while right drawer is open
  } else if (right_over && ui_in_rect(x, y, ui_rect(l->member_list))) {
    if (ccord_store_selected_guild(ui->d.store) == CCORD_DM_GUILD_ID) {
      if (ui->dm_panel_full_profile_rect.w > 0.0f && ui_in_rect(x, y, ui->dm_panel_full_profile_rect)) {
        h.gdm_ctrl = true;
      }
      for (size_t i = 0; i < ui->ngdm_member_hits; i++) {
        if (ui_in_rect(x, y, ui->gdm_member_hits[i].rect)) {
          h.user = ui->gdm_member_hits[i].user_id;
          h.gdm_ctrl = true;
          break;
        }
      }
    } else {
      float cy = y - l->member_list.y + ui->member_scroll;
      for (size_t i = 0; i < ui->nmrow; i++) {
        if (!ui->mrow[i].is_header && cy >= ui->mrow[i].y && cy < ui->mrow[i].y + ui->mrow[i].h) {
          h.mrow = (int)i;
          const ccord_member *m = ccord_store_member_at(ui->d.store, ccord_store_selected_guild(ui->d.store), ui->mrow[i].index);
          if (m) h.user = m->user_id;
          break;
        }
      }
    }
    return h;
  } else if (ui->discovery_open) {
    if (ui->discovery_search_rect.w > 0.0f && ui_in_rect(x, y, ui->discovery_search_rect)) {
      h.disc_text = true;
    } else {
      for (size_t i = 0; i < 6u; i++) {
        if (ui->discovery_cat_rects[i].w > 0.0f && ui_in_rect(x, y, ui->discovery_cat_rects[i])) {
          h.disc_btn = true;
          break;
        }
      }
      for (size_t i = 0; !h.disc_btn && i < ui->ndiscovery_hits; i++) {
        if (ui->discovery_hits[i].join_rect.w > 0.0f && ui_in_rect(x, y, ui->discovery_hits[i].join_rect)) {
          h.disc_btn = true;
        }
      }
    }
    return h;
  } else if (ui_in_rect(x, y, ui_search_rect(ui))) {
    h.search = true;
  } else if (ui_in_rect(x, y, ui_pin_rect(ui))) {
    h.pin = true;
  } else if (ui_in_rect(x, y, ui_inbox_rect(ui))) {
    h.inbox = true;
  } else if (ui_in_rect(x, y, ui_saved_messages_rect(ui))) {
    h.saved = true;
  } else if (ui->members_available && ui_in_rect(x, y, ui_members_btn_rect(ui))) {
    h.members = true;
  } else if ((ui->gdm_add_btn_rect.w > 0.0f && ui_in_rect(x, y, ui->gdm_add_btn_rect)) ||
             (ui->gdm_edit_btn_rect.w > 0.0f && ui_in_rect(x, y, ui->gdm_edit_btn_rect)) ||
             (ui_is_dm_requests_view(ui) &&
              (ui_in_rect(x, y, ui->dm_requests_tab_rects[0]) ||
               ui_in_rect(x, y, ui->dm_requests_tab_rects[1])))) {
    h.gdm_ctrl = true;
  } else if (ui->stage_channel != CCORD_ID_NONE && ui_in_rect(x, y, ui_stage_chat_btn(ui))) {
    h.voice_back = true;
  } else if (l->member_list_visible && ui_in_rect(x, y, ui_rect(l->member_list))) {
    if (ccord_store_selected_guild(ui->d.store) == CCORD_DM_GUILD_ID) {
      if (ui->dm_panel_full_profile_rect.w > 0.0f && ui_in_rect(x, y, ui->dm_panel_full_profile_rect)) {
        h.gdm_ctrl = true;
      }
      for (size_t i = 0; i < ui->ngdm_member_hits; i++) {
        if (ui_in_rect(x, y, ui->gdm_member_hits[i].rect)) {
          h.user = ui->gdm_member_hits[i].user_id;
          h.gdm_ctrl = true;
          break;
        }
      }
    } else {
      float cy = y - l->member_list.y + ui->member_scroll;
      for (size_t i = 0; i < ui->nmrow; i++) {
        if (!ui->mrow[i].is_header && cy >= ui->mrow[i].y && cy < ui->mrow[i].y + ui->mrow[i].h) {
          h.mrow = (int)i;
          const ccord_member *m = ccord_store_member_at(ui->d.store, ccord_store_selected_guild(ui->d.store), ui->mrow[i].index);
          if (m) h.user = m->user_id;
          break;
        }
      }
    }
  } else if (ui->msg_req_banner_visible &&
             (ui_in_rect(x, y, ui->msg_req_banner_accept_rect) ||
              ui_in_rect(x, y, ui->msg_req_banner_reject_rect))) {
    h.msg_req_ctrl = true;
  } else if (ui->mv.composer_btn.w > 0.0f && ui_in_rect(x, y, ui->mv.composer_btn)) {
    h.msg_req_ctrl = true;   // the "Complete the rules" button gets the same pointer cursor
  } else if ((ui->stage_channel == CCORD_ID_NONE || ui_stage_chat_active(ui)) && ui_is_dm_requests_view(ui) &&
             ui_in_rect(x, y, ui_rect(l->message_area))) {
    for (size_t i = 0; i < ui->ndm_request_hits; i++) {
      if (ui_in_rect(x, y, ui->dm_request_hits[i].card_rect)) {
        h.msg_req_ctrl = true;
        break;
      }
    }
    return h;
  } else if ((ui->stage_channel == CCORD_ID_NONE || ui_stage_chat_active(ui)) && ui_in_rect(x, y, ui_rect(l->message_area))) {
    if (ui->new_bar_rect.w > 0.0f && ui_in_rect(x, y, ui->new_bar_rect)) {
      h.new_bar = true;
      return h;
    }
    if (ui->thumb_h > 0.0f && x >= ui->thumb_x - 4.0f) h.sb = true;
    for (size_t i = 0; i < ui->nlinks; i++) {
      if (ui_in_rect(x, y, ui->links[i].rect)) {
        h.link = (int)i;
        break;
      }
    }
    ccord_id ch = ccord_store_selected_channel(ui->d.store);
    // The action bar hangs above its row (up to 20 px, into the previous row when grouped): the
    // pointer on any part of it keeps that row hovered so the bar stays and takes the click.
    bool on_bar = ui->hover_bar_msg_id != CCORD_ID_NONE && ui->hover_bar_rect.w > 0.0f &&
                  ui_in_rect(x, y, ui->hover_bar_rect);
    for (size_t i = 0; i < ui->nvis; i++) {
      if (y >= ui->vis[i].y && y < ui->vis[i].y + ui->vis[i].h) {
        const ccord_message *m = ccord_store_message_at(ui->d.store, ch, (size_t)ui_view_index(ui, ui->vis[i].index));
        if (m) {
          h.msg = m->id;
        }
        break;
      }
    }
    if (on_bar) {
      h.msg = ui->hover_bar_msg_id;
      h.link = -1;   // whatever the previous row has under the bar is covered by it
      return h;
    }
    if (!h.sb) {
      for (size_t i = 0; i < ui->nreply_hits; i++) {
        if (ui_in_rect(x, y, ui->reply_hits[i].rect)) {
          h.reply_card = true;
          break;
        }
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->nimg_hits; i++) {
        if (ui_in_rect(x, y, ui->img_hits[i].rect)) h.rich_ctrl = true;
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->nspoiler_hits; i++) {
        if (ui_in_rect(x, y, ui->spoiler_hits[i].rect)) h.rich_ctrl = true;
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->nmedia_hits; i++) {
        if (ui_in_rect(x, y, ui->media_hits[i].play_rect) ||
            ui_in_rect(x, y, ui->media_hits[i].scrub_rect) ||
            ui_in_rect(x, y, ui->media_hits[i].open_rect)) {
          h.rich_ctrl = true;
        }
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->npoll_hits; i++) {
        if (!ui->poll_hits[i].closed && ui_in_rect(x, y, ui->poll_hits[i].rect)) {
          h.rich_ctrl = true;
        }
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->ncomp_hits; i++) {
        if (!ui->comp_hits[i].disabled && ui_in_rect(x, y, ui->comp_hits[i].rect)) {
          h.rich_ctrl = true;
        }
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->nephem_hits; i++) {
        if (ui_in_rect(x, y, ui->ephem_hits[i].rect)) h.rich_ctrl = true;
      }
      for (size_t i = 0; !h.rich_ctrl && i < ui->nretry_hits; i++) {
        if (ui_in_rect(x, y, ui->retry_hits[i].rect)) h.rich_ctrl = true;
      }
      for (size_t i = 0; i < ui->nts_hits; i++) {
        if (ui_in_rect(x, y, ui->ts_hits[i].rect)) {
          h.ts_hit = (int)i;
          break;
        }
      }
      for (size_t i = 0; i < ui->nuser_hits; i++) {
        if (ui_in_rect(x, y, ui->user_hits[i].rect)) {
          h.user = ui->user_hits[i].user_id;
          break;
        }
      }
    }
  }
  return h;
}

bool uc_update_hover(ccord_ui *ui)
{
  hover_state h = uc_compute_hover(ui);
  const ccord_member *dummy = nullptr;
  (void)dummy;
  bool changed = h.guild != ui->hover_guild_idx || h.rail_slot != ui->hover_rail_slot ||
                 h.crow != ui->hover_crow || h.mrow != ui->hover_mrow ||
                 h.link != ui->hover_link || h.ts_hit != ui->hover_ts_hit || h.gear != ui->hover_gear ||
                 h.header != ui->hover_guild_header || h.msg != ui->hover_message_id || h.user != ui->hover_user_id ||
                 h.sb != ui->hover_scrollbar || h.members != ui->hover_members_btn ||
                 h.mute != ui->hover_mute || h.deafen != ui->hover_deafen || h.camera != ui->hover_camera ||
                 h.screen != ui->hover_screen || h.disconnect != ui->hover_disconnect ||
                 h.search != ui->hover_search || h.pin != ui->hover_pin || h.inbox != ui->hover_inbox ||
                 h.saved != ui->hover_saved_msgs ||
                 h.voice_back != ui->hover_voice_back || h.user_status != ui->hover_user_status;
  ui->hover_guild_idx = h.guild;
  ui->hover_rail_slot = h.rail_slot;
  ui->hover_crow = h.crow;
  ui->hover_mrow = h.mrow;
  ui->hover_link = h.link;
  ui->hover_ts_hit = h.ts_hit;
  ui->hover_gear = h.gear;
  ui->hover_guild_header = h.header;
  ui->hover_message_id = h.msg;
  ui->hover_user_id = h.user;
  ui->hover_scrollbar = h.sb;
  ui->hover_members_btn = h.members;
  ui->hover_mute = h.mute;
  ui->hover_deafen = h.deafen;
  ui->hover_camera = h.camera;
  ui->hover_screen = h.screen;
  ui->hover_disconnect = h.disconnect;
  ui->hover_search = h.search;
  ui->hover_pin = h.pin;
  ui->hover_inbox = h.inbox;
  ui->hover_saved_msgs = h.saved;
  ui->hover_voice_back = h.voice_back;
  ui->hover_user_status = h.user_status;

  // cursor
  ccord_cursor_kind k = CCORD_CURSOR_DEFAULT;
  if (ui->drag == DRAG_STREAM_STAGE_H) {
    k = CCORD_CURSOR_NS_RESIZE;
  } else if (ui->drag == DRAG_STAGE_SPLIT) {
    k = CCORD_CURSOR_EW_RESIZE;
  } else if (ui->drag == DRAG_STREAM_PIP_MOVE || ui->drag == DRAG_STREAM_PIP_RESIZE) {
    k = ui_stream_pip_cursor(ui, ui->mx, ui->my);
  } else if (ui->stream_fullscreen) {
    if (ui->mouse_in && ((ui->stream_fs_exit_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_exit_rect)) ||
                         (ui->stream_fs_bot_exit_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_bot_exit_rect)) ||
                         (ui->stream_fs_pause_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_pause_rect)) ||
                         (ui->stream_fs_btn1_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_btn1_rect)) ||
                         (ui->stream_fs_btn2_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_btn2_rect)))) {
      k = CCORD_CURSOR_POINTER;
    }
  } else if (ui->shortcuts_open) {
    if (ui->mouse_in && ui->shortcuts_close_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->shortcuts_close_rect)) {
      k = CCORD_CURSOR_POINTER;
    }
  } else if (ui->lightbox_open) {
    if (ui->mouse_in && (ui_in_rect(ui->mx, ui->my, ui->lightbox_open_btn) ||
                         ui_in_rect(ui->mx, ui->my, ui->lightbox_prev_btn) ||
                         ui_in_rect(ui->mx, ui->my, ui->lightbox_next_btn) ||
                         ui_in_rect(ui->mx, ui->my, ui->lightbox_close_btn) ||
                         ui_in_rect(ui->mx, ui->my, ui->lightbox_zoom_btn) ||
                         ui_in_rect(ui->mx, ui->my, ui->lightbox_save_btn) ||
                         ui_in_rect(ui->mx, ui->my, ui->lightbox_share_btn) ||
                         ui_in_rect(ui->mx, ui->my, ui->lightbox_copy_btn) ||
                         ui_in_rect(ui->mx, ui->my, ui->lightbox_info_btn) ||
                         ui_in_rect(ui->mx, ui->my, ui->lightbox_retry_btn))) {
      k = CCORD_CURSOR_POINTER;
    }
  } else if (ui->select_open) {
    if (ui->mouse_in) {
      if (ui->select_confirm_btn.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->select_confirm_btn)) {
        k = CCORD_CURSOR_POINTER;
      }
      for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < ui->select_opt_count; i++) {
        if (ui_in_rect(ui->mx, ui->my, ui->select_opt_rects[i])) k = CCORD_CURSOR_POINTER;
      }
    }
  } else if (ui->modal_kind != UI_MODAL_NONE) {
    if (ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS) {
      k = ui_scheduled_events_cursor(ui);
    } else if (ui->modal_kind == UI_MODAL_FORWARD_PICKER) {
      k = ui_forward_picker_cursor(ui);
    } else if (ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION) {
      if (ui->mouse_in && (ui_in_rect(ui->mx, ui->my, ui->mv.cancel) ||
                           (ui->mv.submit_enabled && ui_in_rect(ui->mx, ui->my, ui->mv.submit)))) {
        k = CCORD_CURSOR_POINTER;
      }
      for (size_t i = 0; ui->mouse_in && k == CCORD_CURSOR_DEFAULT && i < CCORD_MEMBER_VERIFICATION_MAX_FIELDS; i++) {
        if (ui->mv.checks[i].w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->mv.checks[i]) &&
            ui_in_rect(ui->mx, ui->my, ui->mv.view)) {
          k = CCORD_CURSOR_POINTER;
        }
      }
    } else if (ui->modal_kind == UI_MODAL_ACTIVITY_SHELF) {
      k = ui_activity_shelf_cursor(ui);
    } else if (ui->modal_kind == UI_MODAL_INTERACTION && ui->mouse_in) {
      if (ui_in_rect(ui->mx, ui->my, ui->inter_modal_ok) || ui_in_rect(ui->mx, ui->my, ui->inter_modal_cancel)) {
        k = CCORD_CURSOR_POINTER;
      }
      size_t nfields = ui->inter_modal ? ui->inter_modal->field_count : 0u;
      for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < nfields; i++) {
        if (ui_in_rect(ui->mx, ui->my, ui->inter_modal_field_rects[i])) k = CCORD_CURSOR_TEXT;
      }
    } else if (ui->modal_kind == UI_MODAL_USER_PROFILE && ui->mouse_in) {
      if (ui_in_rect(ui->mx, ui->my, ui->profile_modal_close_rect) ||
          (ui->profile_modal_msg_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->profile_modal_msg_rect)) ||
          ui_in_rect(ui->mx, ui->my, ui->profile_modal_tab_rects[0]) ||
          ui_in_rect(ui->mx, ui->my, ui->profile_modal_tab_rects[1]) ||
          ui_in_rect(ui->mx, ui->my, ui->profile_modal_tab_rects[2]) ||
          (ui->profile_modal_note_save_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->profile_modal_note_save_rect))) {
        k = CCORD_CURSOR_POINTER;
      } else if (ui->profile_modal_note_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->profile_modal_note_rect)) {
        k = CCORD_CURSOR_TEXT;
      } else {
        for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < ui->nprofile_modal_mutual_hits; i++) {
          if (ui_in_rect(ui->mx, ui->my, ui->profile_modal_mutual_hits[i].rect)) k = CCORD_CURSOR_POINTER;
        }
        for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < ui->nprofile_modal_conn_hits; i++) {
          if (ui->profile_modal_conn_hits[i].url[0] && ui_in_rect(ui->mx, ui->my, ui->profile_modal_conn_hits[i].rect)) {
            k = CCORD_CURSOR_POINTER;
          }
        }
      }
    } else if ((ui->modal_kind == UI_MODAL_ADD_SERVER || ui->modal_kind == UI_MODAL_FOLDER_SETTINGS ||
                ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT ||
                ui->modal_kind == UI_MODAL_EDIT_GROUP_DM) &&
               ui->mouse_in) {
      if ((ui->modal_ok_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->modal_ok_rect)) ||
          (ui->modal_tab_rects[0].w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->modal_tab_rects[0])) ||
          (ui->modal_kind == UI_MODAL_ADD_SERVER &&
           ((ui->modal_preview_btn_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->modal_preview_btn_rect)) ||
            ui_in_rect(ui->mx, ui->my, ui->modal_tab_rects[1])))) {
        k = CCORD_CURSOR_POINTER;
      } else if ((ui->modal_input_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->modal_input_rect)) ||
                 (ui->modal_input2_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->modal_input2_rect))) {
        k = CCORD_CURSOR_TEXT;
      } else if (ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT) {
        for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < ui->nnew_dm_friend_hits; i++) {
          if (ui_in_rect(ui->mx, ui->my, ui->new_dm_friend_hits[i].rect)) k = CCORD_CURSOR_POINTER;
        }
      }
    } else {
      k = CCORD_CURSOR_DEFAULT;
    }
  } else if (ui_server_settings_is_open(ui)) {
    k = ui_server_settings_cursor(ui);
  } else if (ui_channel_settings_is_open(ui)) {
    k = ui_channel_settings_cursor(ui);
  } else if (ui->menu.open) {
    if (ui->menu.hover >= 0) k = CCORD_CURSOR_POINTER;
  } else if (ui->settings_open || ui->go_live_open) {
    if (ui->hover_settings_page >= 0 || ui->hover_settings_close) k = CCORD_CURSOR_POINTER;
  } else if (ui->inbox_open) {
    if (ui->mouse_in) {
      if (h.inbox || h.search || h.pin ||
          ui_in_rect(ui->mx, ui->my, ui->inbox_tab_rects[0]) ||
          ui_in_rect(ui->mx, ui->my, ui->inbox_tab_rects[1]) ||
          (ui->inbox_mark_all_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->inbox_mark_all_rect))) {
        k = CCORD_CURSOR_POINTER;
      }
      for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < ui->ninbox_hits; i++) {
        if (ui_in_rect(ui->mx, ui->my, ui->inbox_hits[i].card_rect)) k = CCORD_CURSOR_POINTER;
      }
    }
  } else if (ui->saved_popover_open) {
    if (ui->mouse_in) {
      if (h.saved || h.inbox || h.search || h.pin ||
          (ui->saved_close_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->saved_close_rect))) {
        k = CCORD_CURSOR_POINTER;
      }
      for (size_t i = 0; k == CCORD_CURSOR_DEFAULT && i < ui->nsaved_hits; i++) {
        if (ui_in_rect(ui->mx, ui->my, ui->saved_hits[i].row)) k = CCORD_CURSOR_POINTER;
      }
    }
  } else if (ui->member_popover_open && ui->mouse_in && ui_in_rect(ui->mx, ui->my, ui->member_popover_rect)) {
    if (ui->popover_full_profile_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->popover_full_profile_rect)) {
      k = CCORD_CURSOR_POINTER;
    }
  } else if (ui->mouse_in) {
    ccord_rect split_h = ui_stage_split_handle_rect(ui);
    if (ui_stream_pip_active(ui) && ui->stream_pip_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_pip_rect)) {
      k = ui_stream_pip_cursor(ui, ui->mx, ui->my);
    } else if (split_h.w > 0.0f && ui_in_rect(ui->mx, ui->my, split_h)) {
      k = CCORD_CURSOR_EW_RESIZE;
    } else if (ui->stage_channel != CCORD_ID_NONE && ui->stream_stage_resize_rect.w > 0.0f &&
               ui_in_rect(ui->mx, ui->my, ui->stream_stage_resize_rect)) {
      k = CCORD_CURSOR_NS_RESIZE;
    } else if (ui->stage_channel != CCORD_ID_NONE &&
               ((ui->stream_stage_fs_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_stage_fs_rect)) ||
                (ui->stream_stage_btn1_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_stage_btn1_rect)) ||
                (ui->stream_stage_btn2_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_stage_btn2_rect)) ||
                (ui->stream_stage_pause_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_stage_pause_rect)))) {
      k = CCORD_CURSOR_POINTER;
    } else if (!ui_panels_cursor(ui, ui->mx, ui->my, &k)) {
      ccord_rect box = ui_composer_box(ui);
      ccord_rect call_bar = ui_call_bar_rect(ui);
      bool comp_btn = !ui->discovery_open &&
                      ((ui->reply_ping_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->reply_ping_rect)) ||
                       (ui->composer_attach_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->composer_attach_rect)));
      for (size_t i = 0; !ui->discovery_open && !comp_btn && i < ui->staged_file_count; i++) {
        if (ui_in_rect(ui->mx, ui->my, ui->staged_spoiler_rects[i]) ||
            ui_in_rect(ui->mx, ui->my, ui->staged_remove_rects[i])) {
          comp_btn = true;
        }
      }
      for (size_t i = 0; !comp_btn && i < ui->nchan_gear_rects; i++) {
        if (ui_in_rect(ui->mx, ui->my, ui->chan_gear_rects[i].rect)) comp_btn = true;
      }
      if (h.rail_slot >= 0 || h.guild >= 0 || h.gear || h.sb || h.members || h.mute || h.deafen || h.camera || h.screen ||
          h.disconnect || h.search || h.pin || h.inbox || h.saved || h.voice_back || h.new_bar || h.reply_card || h.rich_ctrl ||
          h.disc_btn || h.user_status || h.msg_req_ctrl || h.gdm_ctrl ||
          h.link >= 0 || h.mrow >= 0 || h.user != CCORD_ID_NONE || comp_btn) {
        k = CCORD_CURSOR_POINTER;
      } else if (h.disc_text) {
        k = CCORD_CURSOR_TEXT;
      } else if (h.crow >= 0) {
        k = CCORD_CURSOR_POINTER;
      } else if (call_bar.w > 0.0f && ui_in_rect(ui->mx, ui->my, call_bar)) {
        k = CCORD_CURSOR_POINTER;   // the call bar is a button, never a text surface
      } else if (!ui->discovery_open && !ui->msg_req_banner_visible && !ui_is_dm_requests_view(ui) &&
                 ui_in_rect(ui->mx, ui->my, box)) {
        k = CCORD_CURSOR_TEXT;
      } else if (!ui->discovery_open && (ui->stage_channel == CCORD_ID_NONE || ui_stage_chat_active(ui)) &&
                 !ui_is_dm_friends_view(ui) && !ui_is_dm_requests_view(ui) &&
                 !(call_bar.w > 0.0f && ui_in_rect(ui->mx, ui->my, call_bar)) &&
                 ui_in_rect(ui->mx, ui->my, ui_rect(ui->layout.message_area))) {
        // The I-beam belongs over text that can be selected, not over the gaps, avatars and pictures around it.
        // It stays while a selection is being dragged, and (rather than vanish) when more lines were drawn than
        // the table holds.
        bool over_text = ui->drag == DRAG_CHAT_SELECT || ui->ntext_hits >= UI_TEXT_HIT_MAX;
        for (size_t i = 0; !over_text && i < ui->ntext_hits; i++) {
          over_text = ui_in_rect(ui->mx, ui->my, ui->text_hits[i]);
        }
        if (over_text) k = CCORD_CURSOR_TEXT;
      }
    }
  }
  if (k != ui->cursor) {
    ui->cursor = k;
    if (ui->d.host.set_cursor) ui->d.host.set_cursor(ui->d.host.ud, k);
  }
  return changed;
}

// ---------------------------------------------------------------- actions

void ui_open_settings(ccord_ui *ui, bool open)
{
  if (open && ui->stream_fullscreen) ui_stream_set_fullscreen(ui, false);
  ui->settings_open = open;
  if (open) {
    ui_menu_close(ui);
    ui->settings_page = 0;
    ui->mobile_settings_detail = false;
    ui->left_drawer_open = false;
    ui->right_drawer_open = false;
  } else {
    ui->login_screen = false;
    ui->mobile_settings_detail = false;
    ui_settings_account_release(ui);
    ui_settings_profile_release(ui);   // unsaved Profile edits are dropped with the page
  }
  ui->hover_settings_page = -1;
  ui->hover_settings_close = false;
  ui->dirty = true;
}

static void stage_chat_restore(ccord_ui *ui);

// Closes the Text Chat pane and returns the selection to a text channel when it still sits on the voice channel.
void uc_stage_chat_leave(ccord_ui *ui)
{
  ui->stage_chat_open = false;
  stage_chat_restore(ui);
}

static void select_dm_guild(ccord_ui *ui)
{
  if (ui->discovery_open) {
    ui->discovery_open = false;
    ui_compute_layout(ui);
  }
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  uc_stage_chat_leave(ui);   // the split chat pane binds the voice channel; its selection goes back to text
  ccord_error err;
  ccord_status st = ui->d.session ? ccord_session_select_guild(ui->d.session, CCORD_DM_GUILD_ID, &err)
                                  : ccord_store_select_guild(ui->d.store, CCORD_DM_GUILD_ID, &err);
  if (st != CCORD_OK) ui_notice(ui, "%s", err.message);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_save_current_location(ui);
}

void uc_select_guild_at(ccord_ui *ui, size_t i)
{
  if (i == 0) {
    select_dm_guild(ui);
    return;
  }
  if (ui->discovery_open) {
    ui->discovery_open = false;
    ui_compute_layout(ui);
  }
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  uc_stage_chat_leave(ui);   // the split chat pane binds the voice channel; its selection goes back to text
  size_t ng = ui_guild_real_rows(ui);
  if (i - 1 >= ng) return;
  const ccord_guild *g = ccord_store_guild_at(ui->d.store, i - 1);
  if (!g) return;
  ccord_error err;
  ccord_status st = ui->d.session ? ccord_session_select_guild(ui->d.session, g->id, &err)
                                  : ccord_store_select_guild(ui->d.store, g->id, &err);
  if (st != CCORD_OK) ui_notice(ui, "%s", err.message);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_save_current_location(ui);
}

// The split chat pane binds the voice/stage channel's own chat: the channel the user is connected to.
const ccord_channel *uc_stage_chat_target(ccord_ui *ui)
{
  const ccord_channel *sc = ccord_store_channel(ui->d.store, ui->stage_channel);
  return ui_channel_has_chat(sc) ? sc : nullptr;
}

ccord_status uc_select_channel_any(ccord_ui *ui, ccord_id id, ccord_error *err)
{
  return ui->d.session ? ccord_session_select_channel(ui->d.session, id, err) : ccord_store_select_channel(ui->d.store, id, err);
}

// Leaves a voice/stage channel's chat: reselect the text channel that was open before Text Chat (else the guild's first
// plain text channel). Selection only; callers refresh the caches. No-op while a text channel is selected.
static void stage_chat_restore(ccord_ui *ui)
{
  const ccord_channel *sel = ui_selected_channel(ui);
  ccord_id back = ui->stage_chat_return;
  ui->stage_chat_return = CCORD_ID_NONE;
  if (!sel || sel->kind == CCORD_CHANNEL_TEXT) return;
  ccord_id target = CCORD_ID_NONE;
  const ccord_channel *rc = ccord_store_channel(ui->d.store, back);
  if (rc && rc->kind == CCORD_CHANNEL_TEXT) target = rc->id;
  size_t n = ccord_store_channel_count(ui->d.store, sel->guild_id);
  for (size_t i = 0; target == CCORD_ID_NONE && i < n; i++) {
    const ccord_channel *c = ccord_store_channel_at(ui->d.store, sel->guild_id, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(c->discord_type) &&
        !ccord_store_channel_is_forum(ui->d.store, c->id)) {
      target = c->id;
    }
  }
  if (target == CCORD_ID_NONE) return;
  ccord_error err;
  if (uc_select_channel_any(ui, target, &err) != CCORD_OK) ui_notice(ui, "%s", err.message);
}

void ui_clear_voice_stage(ccord_ui *ui)
{
  if (ui->stage_channel == CCORD_ID_NONE) return;
  stage_chat_restore(ui);   // leaving voice returns the selection to a text channel
  ui->stage_channel = CCORD_ID_NONE;
  ui->stage_armed = false;
  ui->stage_chat_open = false;
  ui->stage_scroll = 0.0f;
  ui->dirty = true;
}

void uc_join_voice_channel(ccord_ui *ui, ccord_id guild, const ccord_channel *ch)
{
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_error err;
  if (ccord_session_join_voice(ui->d.session, guild, ch->id, &err) != CCORD_OK) {
    ui_notice(ui, "Cannot join voice: %s", err.message);
    return;
  }
  ui->stage_channel = ch->id;
  ui->stage_armed = false;
  ui->stage_chat_open = false;
  ui->stage_scroll = 0.0f;
  ui->pins_open = false;
  ui->search_open = false;
  ui_compute_layout(ui);
  ui_notice(ui, "Joining voice: %s", ch->name);
  ui->dirty = true;
}

void uc_leave_voice(ccord_ui *ui)
{
  const ccord_voice_state *v = ui_self_voice(ui);
  if (!v) return;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, v->channel_id);
  ccord_id guild = ch ? ch->guild_id : ccord_store_selected_guild(ui->d.store);
  if (ui->d.session) {
    ccord_error err;
    if (ccord_session_leave_voice(ui->d.session, guild, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot disconnect: %s", err.message);
      return;
    }
  }
  ui->camera_on = false;
  ui->go_live_open = false;
  ui_clear_voice_stage(ui);
  ui_compute_layout(ui);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_notice(ui, "Disconnected from voice");
  ui->dirty = true;
}

void uc_toggle_voice_mute(ccord_ui *ui)
{
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  const ccord_voice_state *v = ui_self_voice(ui);
  bool cur_mute = v ? v->self_mute : ccord_session_voice_muted(ui->d.session);
  ccord_error err;
  if (ccord_session_set_voice_mute(ui->d.session, !cur_mute, &err) != CCORD_OK) {
    ui_notice(ui, "Cannot change mute: %s", err.message);
  }
  ui->dirty = true;
}

void uc_toggle_voice_deafen(ccord_ui *ui)
{
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  const ccord_voice_state *v = ui_self_voice(ui);
  bool cur_deaf = v ? v->self_deaf : ccord_session_voice_deafened(ui->d.session);
  ccord_error err;
  if (ccord_session_set_voice_deafen(ui->d.session, !cur_deaf, &err) != CCORD_OK) {
    ui_notice(ui, "Cannot change deafen: %s", err.message);
  }
  ui->dirty = true;
}

void uc_open_go_live(ccord_ui *ui)
{
  if (!ui_self_voice(ui)) {
    ui_notice(ui, "Join a voice channel first");
    return;
  }
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  if (st && st->active) {
    ui->gl_encoder = st->config.encoder;
    ui->gl_codec = st->config.codec;
    ui->gl_framework = st->config.framework;
    ui->gl_resolution = st->config.resolution;
    ui->gl_fps = st->config.fps;
  }
  ui->go_live_sources_dirty = true;
  ui->go_live_open = true;
  ui->pins_open = false;
  ui->search_open = false;
  ui->inbox_open = false;
  ui->dirty = true;
}

void ui_open_dm_with_user(ccord_ui *ui, ccord_id user_id)
{
  if (!ui || user_id == CCORD_ID_NONE) return;
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  ccord_error err;
  if (ui->d.session) {
    (void)ccord_session_request_profile_ex(ui->d.session, user_id, CCORD_ID_NONE, ui->now_ms, false, &err);
    if (ccord_session_open_dm(ui->d.session, user_id, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot open DM: %s", err.message);
      return;
    }
  }
  const ccord_channel *dm = nullptr;
  size_t ndm = ccord_store_channel_count(ui->d.store, CCORD_DM_GUILD_ID);
  for (size_t i = 0; i < ndm; i++) {
    const ccord_channel *c = ccord_store_channel_at(ui->d.store, CCORD_DM_GUILD_ID, i);
    if (!c) continue;
    if (c->user_id == user_id || c->id == 900000u + user_id) {
      dm = c;
      break;
    }
  }
  if (!dm) dm = ccord_store_channel(ui->d.store, 900000u + user_id);
  if (dm) {
    ui->pending_dm_user = CCORD_ID_NONE;
    if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, dm->id, &err);
    else (void)ccord_store_select_channel(ui->d.store, dm->id, &err);
  } else {
    ui->pending_dm_user = user_id;
  }
  ui_notice(ui, "Opening Direct Message");
  ui_invalidate_rows(ui);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_save_current_location(ui);
  ui->dirty = true;
}

void ui_set_user_relationship(ccord_ui *ui, ccord_id user_id, ccord_relationship_kind kind)
{
  if (!ui || user_id == CCORD_ID_NONE) return;
  ccord_error err;
  ccord_status st = ui->d.session ? ccord_session_set_relationship(ui->d.session, user_id, kind, &err)
                                  : ccord_store_set_relationship(ui->d.store, user_id, kind, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Cannot update relationship: %s", err.message);
    return;
  }
  ui_notice(ui, kind == CCORD_RELATIONSHIP_FRIEND ? "Friend added"
                : kind == CCORD_RELATIONSHIP_BLOCKED ? "User blocked"
                : "Relationship removed");
  ui_invalidate_rows(ui);
  ui_ensure_caches(ui);
  ui->dirty = true;
}

