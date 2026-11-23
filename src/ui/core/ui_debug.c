// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ui_core_internal.h"

void ui_settings_m6_debug(const ccord_ui *ui, ccord_ui_settings_debug *out)
{
  if (!ui || !out) return;
  snprintf(out->theme_id, sizeof out->theme_id, "%.31s", ui->settings_theme_id);
  out->zoom = ui->settings_zoom;
  out->streamer_mode = ui->settings_streamer_mode;
  const ui_streamer_opts so = ui_streamer_get_opts(ui);
  out->streamer_hide_personal = so.hide_personal;
  out->streamer_hide_invites = so.hide_invites;
  out->streamer_disable_sounds = so.disable_sounds;
  out->streamer_disable_notifications = so.disable_notifications;
  out->developer_mode = ui->settings_developer_mode;
  out->notify_sounds = ui->settings_notify_sounds;
  out->hardware_accel = ui->settings_hardware_accel;
  snprintf(out->locale, sizeof out->locale, "%.15s", ui->settings_locale);
  out->explicit_content_filter = ui->settings_explicit_content_filter;
  out->animate_stickers = ui->settings_animate_stickers;
  out->gif_auto_play = ui->settings_gif_auto_play;
  out->animate_emoji = ui->settings_animate_emoji;
  out->enable_tts_command = ui->settings_enable_tts_command;
  out->stream_notifications_enabled = ui->settings_stream_notifications_enabled;
  out->default_guilds_restricted = ui->settings_default_guilds_restricted;
  out->allow_age_restricted = ui->settings_allow_age_restricted;
}

void ccord_ui_get_debug(const ccord_ui *ui, ccord_ui_debug *out)
{
  *out = (ccord_ui_debug){0};
  out->message_count = ui->vlist.count;
  out->first_visible = ui->dbg_first;
  out->last_visible = ui->dbg_last;
  out->rows_drawn = ui->dbg_drawn;
  out->rows_measured = ui->dbg_measured;
  out->typing_count = ccord_store_typing_count(ui->d.store, ccord_store_selected_channel(ui->d.store));
  out->stick_bottom = ui->vlist.stick_bottom;
  out->composer_editing = ui->editing_message != CCORD_ID_NONE;
  out->acked_message = ui->acked_message;
  out->list_rebuilds = ui->dbg_list_rebuilds;
  out->notice = ui->notice_until_ms > ui->now_ms ? ui->notice : "";
  out->context_menu_open = ui->menu.open;
  out->context_menu_items = 0;       // top-level items; the ones in a fly-out are counted apart
  out->context_menu_sub_items = 0;
  for (size_t i = 0; ui->menu.open && i < ui->menu.count; i++) {
    if (ui->menu.items[i].parent < 0) out->context_menu_items++;
    else if (ui->menu.items[i].parent == ui->menu.open_sub) out->context_menu_sub_items++;
  }
  out->settings_open = ui->settings_open;
  out->extensions_on = ui->extensions_on;
  out->demo_chip_visible = ui->account_view.demo_chip_visible;
  out->members_toggle = ui_members_btn_rect(ui);
  if (ui->settings_open) {
    out->settings_plugins_tab = ui_settings_page_rect(ui, 1);
    out->settings_account_tab = ui_settings_page_rect(ui, 2);
    out->extensions_toggle = ui_settings_rect(ui, false);
  }
  if (ui->hover_guild_idx >= 0) {
    size_t i = (size_t)ui->hover_guild_idx;
    size_t ng = ui_guild_real_rows(ui);
    if (i == 0) {
      out->hover_guild = CCORD_DM_GUILD_ID;
    } else if (i - 1 < ng) {
      const ccord_guild *g = ccord_store_guild_at(ui->d.store, i - 1);
      if (g) out->hover_guild = g->id;
    }
  }
  if (ui->hover_crow >= 0 && (size_t)ui->hover_crow < ui->ncrow &&
      !ui->crow[ui->hover_crow].is_relationship && !ui->crow[ui->hover_crow].is_voice_user &&
      ui->crow[ui->hover_crow].dm_nav == DM_NAV_NONE) {
    const ccord_channel *ch = ccord_store_channel_at(ui->d.store, ccord_store_selected_guild(ui->d.store), ui->crow[ui->hover_crow].index);
    if (ch) out->hover_channel = ch->id;
  }
  out->hover_message = ui->hover_message_id;
  out->hover_user = ui->hover_user_id;
  if (out->hover_user == CCORD_ID_NONE && ui->hover_crow >= 0 && (size_t)ui->hover_crow < ui->ncrow &&
      (ui->crow[ui->hover_crow].is_voice_user ||
       (ui->crow[ui->hover_crow].is_relationship && !ui->crow[ui->hover_crow].is_category))) {
    out->hover_user = ui->crow[ui->hover_crow].user_id;
  }
  out->message_area_h = ui->layout.message_area.h;

  const ccord_voice_state *v = ui_self_voice(ui);
  out->voice_connected = v && v->channel_id != CCORD_ID_NONE;
  out->voice_muted = v && v->self_mute;
  out->voice_deafened = v && v->self_deaf;
  out->camera_on = ui->camera_on;
  out->streaming = ccord_store_stream_active(ui->d.store);
  out->voice_channel = v ? v->channel_id : CCORD_ID_NONE;
  out->voice_stage_open = ui->stage_channel != CCORD_ID_NONE;
  out->stage_chat_open = ui_stage_split_active(ui);
  out->stage_pane = ui_stage_split_active(ui) ? ui_rect(ui->layout.stage_pane) : (ccord_rect){0, 0, 0, 0};
  out->stage_chat_channel = ui_stage_split_active(ui) ? ccord_store_selected_channel(ui->d.store) : CCORD_ID_NONE;
  out->go_live_open = ui->go_live_open;
  out->go_live_share_audio = ui->go_live_share_audio;
  out->go_live_audio_toggle = ui->go_live_open ? ui_go_live_audio_rect(ui) : (ccord_rect){0, 0, 0, 0};
  out->replying = ui->reply_message != CCORD_ID_NONE;
  out->search_open = ui->search_open;
  out->pins_open = ui->pins_open;
  out->search_matches = ui->search_open && ui->search_edit.len > 0 ? ui->nvmap : ui->view_count;
  out->reply_cards = ui->dbg_reply_cards;
  out->attachment_cards = ui->dbg_attachment_cards;
  out->embed_cards = ui->dbg_embed_cards;
  out->sticker_cards = ui->dbg_sticker_cards;
  out->forward_cards = ui->dbg_forward_cards;
  out->poll_cards = ui->dbg_poll_cards;
  out->voice_bar = ui_voice_bar_rect(ui);
  out->mute_btn = ui_user_btn_rect(ui, 0);
  out->deafen_btn = ui_user_btn_rect(ui, 1);
  out->camera_btn = ui_voice_btn_rect(ui, 0);
  out->go_live_btn = ui_voice_btn_rect(ui, 1);
  out->disconnect_btn = ui_voice_btn_rect(ui, 2);
  out->search_btn = ui_search_rect(ui);
  out->pin_btn = ui_pin_rect(ui);
  out->go_live_modal = ui_go_live_rect(ui);
  out->go_live_start = ui_go_live_start_rect(ui);
  out->go_live_close = ui_go_live_close_rect(ui);
  for (int cat = 0; cat < 5; cat++) {
    out->go_live_option_count[cat] = ui_go_live_option_count(cat);
    for (int i = 0; i < out->go_live_option_count[cat]; i++) {
      out->go_live_option[cat][i] = ui_go_live_option_rect(ui, cat, i);
    }
  }
  out->media_requests = ui->dbg_media_requests;
  out->media_textures = ui->dbg_media_textures;
  out->avatar_textures_drawn = ui->dbg_avatar_textures_drawn;
  out->guild_icon_textures_drawn = ui->dbg_guild_icon_textures_drawn;
  out->attachment_textures_drawn = ui->dbg_attachment_textures_drawn;
  out->relationship_rows = ui->dbg_relationship_rows;
  out->clickable_links = ui->nlinks;
  out->urls_opened = ui->dbg_urls_opened;
  out->notifications_prepared = ui->dbg_notifications_prepared;
  out->hover_link = ui->hover_link;
  snprintf(out->last_opened_url, sizeof out->last_opened_url, "%.255s", ui->last_opened_url);
  out->emoji_picker_open = ui->emoji_picker_open;
  out->autocomplete_open = ui->autocomplete_open;
  out->has_staged_attachment = ui->staged_file_count > 0;
  out->staged_attachment_count = ui->staged_file_count;
  for (size_t i = 0; i < ui->staged_file_count && i < CCORD_RICH_MAX_ATTACHMENTS; i++) {
    out->staged_spoiler_rects[i] = ui->staged_spoiler_rects[i];
    out->staged_remove_rects[i] = ui->staged_remove_rects[i];
    out->staged_spoilers[i] = ui->staged_files[i].spoiler;
  }
  out->reply_ping = ui->reply_ping;
  out->reply_ping_btn = ui->reply_ping_rect;
  out->slowmode_remaining_s = ui_slowmode_remaining_s(ui, ui_selected_channel(ui));
  out->ephemeral_dismiss_count = ui->nephem_hits < 8u ? ui->nephem_hits : 8u;
  for (size_t i = 0; i < out->ephemeral_dismiss_count; i++) {
    out->ephemeral_dismiss_rects[i] = ui->ephem_hits[i].rect;
  }
  out->pending_echoes_drawn = ui->dbg_pending_echoes;
  out->failed_echoes_drawn = ui->dbg_failed_echoes;
  out->retry_echo_count = ui->nretry_hits < 8u ? ui->nretry_hits : 8u;
  for (size_t i = 0; i < out->retry_echo_count; i++) {
    out->retry_echo_ids[i] = ui->retry_hits[i].message_id;
    out->retry_echo_rects[i] = ui->retry_hits[i].rect;
  }
  out->member_popover_open = ui->member_popover_open;
  out->login_screen_open = ui_is_login_screen(ui);
  out->loading_screen_open = ui_is_loading_screen(ui);
  out->stale_bar_open = ui_stale_bar_active(ui);
  out->loading_progress = ui->loading_progress;
  out->loading_bar_rect = ui->loading_bar_rect;
  out->autocomplete_count = ui->nautocomplete;
  out->reaction_pills_drawn = ui->nreaction_hits;
  out->composer_attach_btn = ui->composer_attach_rect;
  out->emoji_picker_rect = ui->emoji_picker_rect;
  out->autocomplete_rect = ui->autocomplete_rect;
  out->member_popover_rect = ui->member_popover_rect;
  if (ui->staged_file_count > 0) {
    snprintf(out->staged_filename, sizeof out->staged_filename, "%.127s", ui->staged_files[0].filename);
  } else {
    out->staged_filename[0] = '\0';
  }
  out->date_dividers = ui->dbg_date_dividers;
  out->new_dividers = ui->dbg_new_dividers;
  out->new_bar_visible = ui->new_bar_rect.w > 0.0f;
  out->new_bar_rect = ui->new_bar_rect;
  out->new_bar_ack_rect = ui->new_bar_ack_rect;
  out->jump_highlight_message =
    (ui->jump_highlight_msg_id != CCORD_ID_NONE && ui->now_ms < ui->jump_highlight_until_ms)
      ? ui->jump_highlight_msg_id
      : CCORD_ID_NONE;
  snprintf(out->tooltip_text, sizeof out->tooltip_text, "%.127s", ui->tooltip_text);
  out->lightbox_open = ui->lightbox_open;
  out->lightbox_index = ui->lightbox_index;
  out->lightbox_count = ui->lightbox_count;
  out->lightbox_zoom = ui->lightbox_zoom;
  snprintf(out->lightbox_url, sizeof out->lightbox_url, "%.255s", ui->lightbox_url);
  out->lightbox_img_rect = ui->lightbox_img_rect;
  out->lightbox_open_btn = ui->lightbox_open_btn;
  out->lightbox_prev_btn = ui->lightbox_prev_btn;
  out->lightbox_next_btn = ui->lightbox_next_btn;
  out->gallery_cards = ui->dbg_gallery_cards;
  out->media_player_cards = ui->dbg_media_player_cards;
  out->embed_fields_drawn = ui->dbg_embed_fields_drawn;
  out->component_rows_drawn = ui->dbg_component_rows;
  out->component_buttons_drawn = ui->dbg_component_buttons;
  out->component_selects_drawn = ui->dbg_component_selects;
  out->poll_option_count = ui->npoll_hits < 10u ? ui->npoll_hits : 10u;
  for (size_t i = 0; i < out->poll_option_count; i++) out->poll_option_rects[i] = ui->poll_hits[i].rect;
  out->component_rect_count = ui->ncomp_hits < 16u ? ui->ncomp_hits : 16u;
  for (size_t i = 0; i < out->component_rect_count; i++) out->component_rects[i] = ui->comp_hits[i].rect;
  out->select_dropdown_open = ui->select_open;
  out->select_dropdown_option_count = ui->select_open ? ui->select_opt_count : 0u;
  out->select_dropdown_rect = ui->select_dropdown_rect;
  out->select_confirm_btn = ui->select_confirm_btn;
  for (size_t i = 0; i < out->select_dropdown_option_count && i < CCORD_RICH_MAX_SELECT_OPTIONS; i++) {
    out->select_option_rects[i] = ui->select_opt_rects[i];
  }
  out->interaction_modal_open = (ui->modal_kind == UI_MODAL_INTERACTION);
  out->interaction_modal_field_count = (out->interaction_modal_open && ui->inter_modal) ? ui->inter_modal->field_count : 0u;
  out->interaction_modal_rect = ui->inter_modal_box;
  out->interaction_modal_submit_btn = ui->inter_modal_ok;
  out->interaction_modal_cancel_btn = ui->inter_modal_cancel;
  for (size_t i = 0; i < out->interaction_modal_field_count && i < CCORD_RICH_MAX_COMPONENT_ROWS; i++) {
    out->interaction_modal_fields[i] = ui->inter_modal_field_rects[i];
  }
  out->media_playing = ui->media_playing;
  out->media_progress = ui->media_duration_ms > 0
                        ? uc_clampf((float)ui->media_pos_ms / (float)ui->media_duration_ms, 0.0f, 1.0f)
                        : 0.0f;
  out->search_panel_open = ui_search_panel_active(ui);
  out->search_total_results = ui->d.session ? ccord_session_search_total_results(ui->d.session) : 0u;
  out->search_offset = ui->d.session ? ccord_session_search_offset(ui->d.session) : 0u;
  out->search_panel_cards = out->search_panel_open ? ui->nsearch_card_hits : 0u;
  out->search_prev_btn = out->search_panel_open ? ui->search_prev_rect : (ccord_rect){0};
  out->search_next_btn = out->search_panel_open ? ui->search_next_rect : (ccord_rect){0};
  for (size_t i = 0; i < out->search_panel_cards && i < 8u; i++) {
    out->search_card_rects[i] = ui->search_card_hits[i].rect;
  }
  out->pins_count = ui->pins_open ? ui->dbg_pins_total : 0u;
  out->pins_has_more = ui->pins_open && ui->dbg_pins_has_more;
  out->pins_load_more_btn = ui->pins_open ? ui->pins_load_more_rect : (ccord_rect){0};
  for (size_t i = 0; ui->pins_open && i < ui->npins_vis && i < 8u; i++) {
    out->pin_item_rects[i] = ui->pins_vis_rects[i];
  }
  out->thread_cards_drawn = ui->dbg_thread_cards;
  for (size_t i = 0; i < ui->nthread_hits && i < 8u; i++) {
    out->thread_card_rects[i] = ui->thread_hits[i].rect;
  }
  out->thread_join_btn = ui->thread_join_rect;
  out->thread_archive_btn = ui->thread_archive_rect;
  out->thread_breadcrumb_rect = ui->thread_breadcrumb_rect;
  out->burst_reaction_pills_drawn = ui->dbg_burst_reaction_pills;
  out->reactions_popover_open = ui->reactions_open;
  out->reactions_tab_count = ui->reactions_open ? ui->nreactions_tabs : 0u;
  out->reactions_user_count = ui->reactions_open ? ui->dbg_reactions_users : 0u;
  out->reactions_popover_rect = ui->reactions_open ? ui->reactions_popover_rect : (ccord_rect){0};
  for (size_t i = 0; ui->reactions_open && i < ui->nreactions_tabs && i < 8u; i++) {
    out->reactions_tab_rects[i] = ui->reactions_tab_rects[i];
  }
  out->inbox_open = ui->inbox_open;
  out->shortcuts_open = ui->shortcuts_open;
  out->quick_switcher_open = ui->quick_switcher_open;
  out->inbox_tab = ui->inbox_tab;
  out->inbox_item_count = ui->inbox_open ? ui->ninbox_hits : 0u;
  out->inbox_btn = ui_inbox_rect(ui);
  out->inbox_popover_rect = ui->inbox_open ? ui->inbox_popover_rect : (ccord_rect){0};
  out->inbox_tab_rects[0] = ui->inbox_open ? ui->inbox_tab_rects[0] : (ccord_rect){0};
  out->inbox_tab_rects[1] = ui->inbox_open ? ui->inbox_tab_rects[1] : (ccord_rect){0};
  out->inbox_mark_all_btn = ui->inbox_open ? ui->inbox_mark_all_rect : (ccord_rect){0};
  for (size_t i = 0; ui->inbox_open && i < ui->ninbox_hits && i < 8u; i++) {
    out->inbox_item_rects[i] = ui->inbox_hits[i].card_rect;
    out->inbox_item_ack_rects[i] = ui->inbox_hits[i].ack_rect;
  }
  out->shortcuts_modal_rect = ui->shortcuts_open ? ui->shortcuts_modal_rect : (ccord_rect){0};
  out->shortcuts_close_btn = ui->shortcuts_open ? ui->shortcuts_close_rect : (ccord_rect){0};
  out->nav_history_count = ui->nav_hist_len;
  out->nav_history_index = ui->nav_hist_pos;

  size_t nslots = 0;
  const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
  for (size_t si = 0; slots && si < nslots; si++) {
    const ui_rail_slot *sl = &slots[si];
    if (sl->kind == UI_RAIL_ADD_SERVER) {
      out->add_server_btn = sl->rect;
    } else if (sl->kind == UI_RAIL_DISCOVERY) {
      out->discovery_btn = sl->rect;
    } else if ((sl->kind == UI_RAIL_FOLDER_COLLAPSED || sl->kind == UI_RAIL_FOLDER_HEADER) &&
               out->rail_folder_count < 16u) {
      size_t fi = out->rail_folder_count++;
      out->rail_folder_ids[fi] = sl->folder_id;
      out->rail_folder_rects[fi] = sl->rect;
      out->rail_folder_expanded[fi] = (sl->kind == UI_RAIL_FOLDER_HEADER);
      uint32_t f_unread = 0u, f_ment = 0u;
      for (size_t k = 0; k < sl->folder_len; k++) {
        const ccord_guild *fg = ccord_store_guild_at(ui->d.store, sl->folder_start + k);
        if (!fg) continue;
        uint32_t gm = 0u;
        f_unread += ui_guild_unread(ui, fg->id, &gm);
        f_ment += gm;
      }
      out->rail_folder_mentions[fi] = f_ment;
      out->rail_folder_unread[fi] = (f_unread > 0u);
    }
  }
  out->discovery_open = ui->discovery_open;
  out->add_server_modal_open = (ui->modal_kind == UI_MODAL_ADD_SERVER);
  out->folder_settings_modal_open = (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS);
  out->discovery_category = (int)ui->discovery_category;
  if (ui->discovery_open) {
    out->discovery_card_count = ui->ndiscovery_hits;
    if (ui->d.session) {
      uint32_t dtotal = 0u;
      (void)ccord_session_discovery_results(ui->d.session, nullptr, &dtotal, nullptr, nullptr);
      out->discovery_total = dtotal;
    }
    out->discovery_search_rect = ui->discovery_search_rect;
    for (size_t i = 0; i < 6u; i++) out->discovery_cat_rects[i] = ui->discovery_cat_rects[i];
    for (size_t i = 0; i < ui->ndiscovery_hits && i < 16u; i++) {
      out->discovery_join_rects[i] = ui->discovery_hits[i].join_rect;
    }
  }
  if (out->add_server_modal_open || out->folder_settings_modal_open ||
      ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT ||
      ui->modal_kind == UI_MODAL_EDIT_GROUP_DM ||
      ui->modal_kind == UI_MODAL_CONFIRM) {
    out->modal_tab_rects[0] = ui->modal_tab_rects[0];
    out->modal_tab_rects[1] = ui->modal_tab_rects[1];
    out->modal_preview_btn = ui->modal_preview_btn_rect;
    out->modal_ok_btn = ui->modal_ok_rect;
    out->modal_input_rect = ui->modal_input_rect;
    out->modal_input2_rect = ui->modal_input2_rect;
    if (out->add_server_modal_open && ui->d.session) {
      const ccord_invite_preview *pv = ccord_session_invite_preview(ui->d.session, nullptr);
      if (pv) {
        out->modal_has_invite_preview = true;
        snprintf(out->modal_preview_guild_name, sizeof out->modal_preview_guild_name, "%s", pv->guild_name);
      }
    }
  }

  out->dm_requests_view = ui_is_dm_requests_view(ui);
  out->dm_requests_tab = ui->dm_requests_tab;
  out->dm_requests_badge_count = ui->dm_requests_badge_count;
  out->dm_requests_nav_rect = ui->dm_requests_nav_rect;
  if (out->dm_requests_view) {
    out->dm_requests_tab_rects[0] = ui->dm_requests_tab_rects[0];
    out->dm_requests_tab_rects[1] = ui->dm_requests_tab_rects[1];
    out->dm_request_card_count = ui->ndm_request_hits < 8u ? ui->ndm_request_hits : 8u;
    for (size_t i = 0; i < out->dm_request_card_count; i++) {
      out->dm_request_card_rects[i] = ui->dm_request_hits[i].card_rect;
      out->dm_request_accept_rects[i] = ui->dm_request_hits[i].accept_rect;
      out->dm_request_reject_rects[i] = ui->dm_request_hits[i].reject_rect;
    }
  }
  out->msg_req_banner_visible = ui->msg_req_banner_visible;
  out->msg_req_banner_accept_btn = ui->msg_req_banner_accept_rect;
  out->msg_req_banner_reject_btn = ui->msg_req_banner_reject_rect;

  if (ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT) {
    out->new_dm_selected_count = ui->new_dm_recipient_count;
    out->new_dm_friend_count = ui->nnew_dm_friend_hits < 8u ? ui->nnew_dm_friend_hits : 8u;
    for (size_t i = 0; i < out->new_dm_friend_count; i++) {
      out->new_dm_friend_rects[i] = ui->new_dm_friend_hits[i].rect;
    }
  }
  out->gdm_add_btn = ui->gdm_add_btn_rect;
  out->gdm_edit_btn = ui->gdm_edit_btn_rect;
  out->gdm_member_count = ui->ngdm_member_hits < 16u ? ui->ngdm_member_hits : 16u;
  for (size_t i = 0; i < out->gdm_member_count; i++) {
    out->gdm_member_rects[i] = ui->gdm_member_hits[i].rect;
  }

  out->user_status_rect = ui->user_status_rect;

  out->popover_full_profile_btn = ui->popover_full_profile_rect;
  out->dm_panel_full_profile_btn = ui->dm_panel_full_profile_rect;
  out->profile_modal_open = (ui->modal_kind == UI_MODAL_USER_PROFILE);
  out->profile_modal_tab = ui->profile_modal_tab;
  if (out->profile_modal_open) {
    out->profile_modal_rect = ui->profile_modal_box;
    out->profile_modal_close_btn = ui->profile_modal_close_rect;
    out->profile_modal_msg_btn = ui->profile_modal_msg_rect;
    for (int i = 0; i < 3; i++) out->profile_modal_tab_rects[i] = ui->profile_modal_tab_rects[i];
    out->profile_modal_note_rect = ui->profile_modal_note_rect;
    out->profile_modal_note_save_btn = ui->profile_modal_note_save_rect;
    out->profile_modal_mutual_count = ui->nprofile_modal_mutual_hits < 16u ? ui->nprofile_modal_mutual_hits : 16u;
    for (size_t i = 0; i < out->profile_modal_mutual_count; i++) {
      out->profile_modal_mutual_rects[i] = ui->profile_modal_mutual_hits[i].rect;
    }
    out->profile_modal_conn_count = ui->nprofile_modal_conn_hits < 12u ? ui->nprofile_modal_conn_hits : 12u;
    for (size_t i = 0; i < out->profile_modal_conn_count; i++) {
      out->profile_modal_conn_rects[i] = ui->profile_modal_conn_hits[i].rect;
    }
  }
  out->clan_badges_drawn = ui->dbg_clan_badges;
  out->activity_cards_drawn = ui->dbg_activity_cards;
  out->role_pills_drawn = ui->dbg_role_pills;
  out->invite_cards_drawn = ui->dbg_invite_cards;
  out->invite_btn_rect = ui->ninvite_hits > 0 ? ui->invite_hits[ui->ninvite_hits - 1u].btn_rect : (ccord_rect){0};
  out->role_preview_active = ui->d.store && ui_role_preview_active_for(ui, ccord_store_selected_guild(ui->d.store));
  out->role_preview_guild = ui->role_preview_guild;
  out->role_preview_role_id = ui->role_preview_role_id;
  out->role_preview_cycle_btn = ui->role_preview_cycle_rect;
  out->role_preview_exit_btn = ui->role_preview_exit_rect;
}

const ccord_layout *ccord_ui_get_layout(const ccord_ui *ui)
{
  return &ui->layout;
}

const char *ccord_ui_composer_text(const ccord_ui *ui)
{
  return ccord_text_edit_cstr(&ui->edit);
}

static size_t utf8_fit_bytes(const char *s, size_t max_bytes)
{
  if (!s) return 0;
  size_t n = strlen(s);
  if (n > max_bytes) n = max_bytes;
  while (n > 0 && !ccord_utf8_valid(s, n)) n--;
  return n;
}

static void utf8_trim_inplace(char *s)
{
  size_t n = strlen(s);
  while (n > 0 && !ccord_utf8_valid(s, n)) n--;
  s[n] = '\0';
}

bool ccord_ui_prepare_notification(ccord_ui *ui, const ccord_notification *notif,
                                   char *out_title, size_t title_cap, char *out_body, size_t body_cap)
{
  if (!ui || !notif) return false;
  if (!ui->settings_notify_desktop) return false;
  if (!ui->settings_notify_messages && !ui->settings_notify_mentions) return false;
  if (!ui->settings_notify_messages && !(notif->is_mention || notif->is_dm)) return false;
  if (out_title && title_cap > 0) {
    const ccord_user *u = ccord_store_user(ui->d.store, notif->author_id);
    const ccord_channel *ch = ccord_store_channel(ui->d.store, notif->channel_id);
    if (notif->is_dm) {
      const char *uname = u ? u->name : "Direct Message";
      size_t un = utf8_fit_bytes(uname, 60u);
      snprintf(out_title, title_cap, "%.*s (Direct Message)", (int)un, uname);
    } else {
      const char *uname = u ? u->name : "New Message";
      const char *cname = ch ? ch->name : "channel";
      size_t un = utf8_fit_bytes(uname, 50u);
      size_t cn = utf8_fit_bytes(cname, 50u);
      snprintf(out_title, title_cap, "%.*s in #%.*s", (int)un, uname, (int)cn, cname);
    }
    utf8_trim_inplace(out_title);
  }
  if (out_body && body_cap > 0) {
    const ccord_message *m = ccord_store_find_message(ui->d.store, notif->channel_id, notif->message_id);
    char preview[256];
    size_t pl = m ? ccord_message_preview(m, preview, sizeof preview) : 0u;
    pl = ui_streamer_mask_inplace(ui, preview, pl, false);   // Streamer Mode: no invite codes in toasts
    const char *src = pl > 0 ? preview : "Sent a message";
    size_t bn = utf8_fit_bytes(src, 240u);
    snprintf(out_body, body_cap, "%.*s", (int)bn, src);
    utf8_trim_inplace(out_body);
  }
  ui->dbg_notifications_prepared++;
  return true;
}

void ccord_ui_open_message(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id, ccord_id message_id)
{
  if (!ui || !ui->d.store || channel_id == CCORD_ID_NONE) return;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, channel_id);
  ccord_id gid = ch ? ch->guild_id : guild_id;
  if (gid == CCORD_ID_NONE) return;
  bool was_here = channel_id == ccord_store_selected_channel(ui->d.store);
  ui_select_guild_and_channel(ui, gid, channel_id);
  if (channel_id != ccord_store_selected_channel(ui->d.store)) return;   // the channel could not be opened
  if (message_id != CCORD_ID_NONE && ccord_store_find_message(ui->d.store, channel_id, message_id)) {
    const ccord_message *latest = uc_latest_non_ephemeral_message(ui->d.store, channel_id);
    if (latest && latest->id == message_id &&
        (latest->author_id == CCORD_ID_NONE ||
         ccord_store_relationship(ui->d.store, latest->author_id) != CCORD_REL_BLOCKED)) {
      // Target is already the newest message: stay pinned to the bottom so reading/auto-ack stays active
      // even for tall media/embed messages, while still flashing the jump highlight on it.
      ui->msg_scroll_vel = 0.0f;
      ccord_vlist_reset_bottom(&ui->vlist);
      ui->jump_highlight_msg_id = message_id;
      ui->jump_highlight_until_ms = ui->now_ms + UI_JUMP_FLASH_MS;
      ui->dirty = true;
      return;
    }
    (void)ui_jump_to_message(ui, message_id);
    return;
  }
  // Not loaded (yet): jump when its page arrives. Already loaded and absent means it was deleted: show the present.
  if (message_id != CCORD_ID_NONE) {
    ui->pending_jump_msg_id = message_id;
    ui->pending_jump_channel = channel_id;
  }
  if (was_here) {
    ui->msg_scroll_vel = 0.0f;
    ccord_vlist_reset_bottom(&ui->vlist);
  }
  ui->dirty = true;
}

bool ccord_ui_get_saved_window_size(const ccord_ui *ui, int *out_w, int *out_h)
{
  if (!ui || !ui->settings_win_saved || ui->settings_win_w <= 0 || ui->settings_win_h <= 0) return false;
  if (out_w) *out_w = ui->settings_win_w;
  if (out_h) *out_h = ui->settings_win_h;
  return true;
}

static const char *attach_basename(const char *path)
{
  if (!path) return "";
  const char *base = strrchr(path, '/');
#if CCORD_OS_WINDOWS
  const char *bs = strrchr(path, '\\');
  if (bs && (!base || bs > base)) base = bs;
#endif
  return base ? base + 1 : path;
}

// Takes ownership of `owned_data` on every path (frees it on rejection or if Settings > Profile consumes it).
static bool ui_attach_owned_bytes(ccord_ui *ui, const char *filename, uint8_t *owned_data, size_t size)
{
  if (!ui || !filename || !filename[0] || !owned_data || size == 0) {
    ccord_free(owned_data);
    return false;
  }
  if (ui_settings_profile_take_image(ui, filename, owned_data, size)) {
    ccord_free(owned_data);
    return true;   // Settings > Profile asked for an image
  }
  if (ui->staged_file_count >= CCORD_RICH_MAX_ATTACHMENTS) {
    ccord_free(owned_data);
    ui_notice(ui, "Maximum of %u attachments per message", (unsigned)CCORD_RICH_MAX_ATTACHMENTS);
    return false;
  }
  const char *base = attach_basename(filename);
  if (!base[0]) {
    ccord_free(owned_data);
    return false;
  }
  bool sp = false;
  if (strncmp(base, "SPOILER_", 8) == 0 && base[8] != '\0') {
    sp = true;
    base += 8;
  }
  size_t idx = ui->staged_file_count++;
  ui->staged_files[idx].data = owned_data;
  ui->staged_files[idx].size = size;
  ui->staged_files[idx].spoiler = sp;
  snprintf(ui->staged_files[idx].filename, sizeof ui->staged_files[idx].filename, "%.127s", base);
  ui->dirty = true;
  return true;
}

bool ccord_ui_attach_bytes(ccord_ui *ui, const char *filename, const void *data, size_t size)
{
  if (!ui || !filename || !filename[0] || !data) return false;
  ccord_upload_limit_source src = CCORD_UPLOAD_SRC_FREE;
  uint64_t limit = ui_current_upload_limit(ui, &src);
  if (size == 0 || (uint64_t)size > limit) {
    char lim_s[32];
    ccord_format_upload_limit(limit, lim_s, sizeof lim_s);
    ui_notice(ui, "File must be between 1 B and %s", lim_s);
    ui_upload_error_open(ui, filename, (uint64_t)size, limit, src);
    return false;
  }
  if (ui_settings_profile_take_image(ui, filename, data, size)) return true;   // Settings > Profile asked for an image
  if (ui->staged_file_count >= CCORD_RICH_MAX_ATTACHMENTS) {
    ui_notice(ui, "Maximum of %u attachments per message", (unsigned)CCORD_RICH_MAX_ATTACHMENTS);
    return false;
  }
  uint8_t *copy = ccord_malloc(size);
  if (!copy) {
    ui_notice(ui, "Not enough memory to attach %s", attach_basename(filename));
    return false;
  }
  memcpy(copy, data, size);
  return ui_attach_owned_bytes(ui, filename, copy, size);
}

size_t ccord_ui_staged_attachment_count(const ccord_ui *ui)
{
  return ui ? ui->staged_file_count : 0u;
}

bool ccord_ui_toggle_attachment_spoiler(ccord_ui *ui, size_t index)
{
  if (!ui || index >= ui->staged_file_count) return false;
  ui->staged_files[index].spoiler = !ui->staged_files[index].spoiler;
  ui->dirty = true;
  return true;
}

bool ccord_ui_remove_staged_attachment(ccord_ui *ui, size_t index)
{
  if (!ui || index >= ui->staged_file_count) return false;
  bool large = ui->staged_files[index].size >= (1024u * 1024u);
  ccord_free(ui->staged_files[index].data);
  for (size_t i = index; i + 1u < ui->staged_file_count; i++) {
    ui->staged_files[i] = ui->staged_files[i + 1u];
  }
  ui->staged_file_count--;
  memset(&ui->staged_files[ui->staged_file_count], 0, sizeof ui->staged_files[0]);
  if (large) ccord_os_trim_memory();
  ui->dirty = true;
  return true;
}

bool ccord_ui_attach_file(ccord_ui *ui, const char *path)
{
  if (!ui || !path || !path[0]) return false;
  uint64_t sz = 0;
  if (!ccord_file_size(path, &sz)) {
    ui_notice(ui, "Cannot open file: %s", path);
    return false;
  }
  ccord_upload_limit_source src = CCORD_UPLOAD_SRC_FREE;
  uint64_t limit = ui_current_upload_limit(ui, &src);
  if (sz == 0 || sz > limit) {
    char lim_s[32];
    ccord_format_upload_limit(limit, lim_s, sizeof lim_s);
    ui_notice(ui, "File must be between 1 B and %s", lim_s);
    ui_upload_error_open(ui, path, sz, limit, src);
    return false;
  }
  FILE *f = fopen(path, "rb");
  if (!f) {
    ui_notice(ui, "Cannot open file: %s", path);
    return false;
  }
  // Single-buffer read: transfers ownership directly to staged_files without a second malloc/memcpy peak.
  uint8_t *buf = ccord_malloc((size_t)sz);
  if (!buf) {
    fclose(f);
    ui_notice(ui, "Not enough memory to attach %s", attach_basename(path));
    return false;
  }
  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (got != (size_t)sz) {
    ccord_free(buf);
    ui_notice(ui, "Cannot read file: %s", attach_basename(path));
    return false;
  }
  size_t before = ui->staged_file_count;
  bool ok = ui_attach_owned_bytes(ui, path, buf, got);
  if (ok && ui->staged_file_count == before + 1u) {
    char *dst = ui->staged_files[before].path;
    size_t cap = sizeof ui->staged_files[before].path;
    if (path[0] == '/') {
      if (strlen(path) < cap) snprintf(dst, cap, "%s", path);
    } else {
      char cwd[512];
      if (ccord_os_getcwd(cwd, sizeof cwd) && strlen(cwd) + strlen(path) + 2u < cap) (void)snprintf(dst, cap, "%s/%.400s", cwd, path);
    }
  }
  return ok;
}

void ccord_ui_clear_attachment(ccord_ui *ui)
{
  if (!ui) return;
  bool had_large = false;
  for (size_t i = 0; i < ui->staged_file_count; i++) {
    if (ui->staged_files[i].size >= (1024u * 1024u)) had_large = true;
    ccord_free(ui->staged_files[i].data);
  }
  memset(ui->staged_files, 0, sizeof ui->staged_files);
  ui->staged_file_count = 0;
  if (had_large) ccord_os_trim_memory();
  ui->dirty = true;
}

bool ccord_ui_active_capture_source(const ccord_ui *ui, struct ccord_capture_source *out_src)
{
  if (!ui || !ui->has_active_capture_source) return false;
  if (out_src) *out_src = ui->active_capture_source;
  return true;
}

void ccord_ui_audio_volumes(const ccord_ui *ui, int *out_input_vol, int *out_output_vol)
{
  if (out_input_vol) *out_input_vol = ui ? ui->settings_input_volume : 100;
  if (out_output_vol) *out_output_vol = ui ? ui->settings_output_volume : 100;
}

// ---- M5 getter seams (controls + INI persistence: m5-ui-settings) ----------------------------------------------
void ccord_ui_voice_settings(const ccord_ui *ui, ccord_ui_voice_prefs *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  out->input_volume = 100;
  out->output_volume = 100;
  if (!ui) return;
  out->ptt = ui->settings_voice_mode == 1;
  out->ptt_scancode = ui->settings_ptt_scancode;
  out->vad_sensitivity = ui->settings_vad_sensitivity;
  out->noise_suppression = ui->settings_noise_suppression;
  out->echo_cancel = ui->settings_echo_cancel;
  out->auto_gain = ui->settings_auto_gain;
  memcpy(out->input_device_id, ui->settings_mic_device_id, sizeof out->input_device_id);
  memcpy(out->output_device_id, ui->settings_speaker_device_id, sizeof out->output_device_id);
  memcpy(out->camera_device_id, ui->settings_camera_device_id, sizeof out->camera_device_id);
  out->input_volume = ui->settings_input_volume;
  out->output_volume = ui->settings_output_volume;
}

uint8_t ccord_ui_user_volume(const ccord_ui *ui, ccord_id user_id)
{
  if (ui) {
    for (size_t i = 0; i < ui->user_volume_count; i++) {
      if (ui->user_volume[i].user_id == user_id) return ui->user_volume[i].pct;
    }
  }
  return 100;
}

void ccord_ui_set_user_volume(ccord_ui *ui, ccord_id user_id, uint8_t pct)
{
  if (!ui || !user_id) return;
  if (pct > 200) pct = 200;
  for (size_t i = 0; i < ui->user_volume_count; i++) {
    if (ui->user_volume[i].user_id == user_id) {
      ui->user_volume[i].pct = pct;
      ui->dirty = true;
      return;
    }
  }
  if (ui->user_volume_count < CCORD_UI_USER_VOLUME_MAX) {
    ui->user_volume[ui->user_volume_count].user_id = user_id;
    ui->user_volume[ui->user_volume_count].pct = pct;
    ui->user_volume_count++;
    ui->dirty = true;
  }
}

// ---- Responsive mobile UI, touch scroll bridge & Android Back navigation ---------------------------------------

bool ui_scroll_at_px(ccord_ui *ui, float x, float y, float dy_px, bool direct)
{
  if (!ui || dy_px == 0.0f) return false;
  float notch = 3.0f * ui_line_h(ui, M(font_size_base));
  if (notch < 1.0f) notch = 48.0f;
  ccord_input_event wev = {
    .type = CCORD_IN_WHEEL,
    .x = x,
    .y = y,
    .wheel = -(dy_px / notch),
  };
  int8_t prev_override = ui->touch_scroll_direct_override;
  ui->touch_scroll_direct_override = direct ? 1 : -1;
  bool r = ccord_ui_handle_input(ui, &wev);
  ui->touch_scroll_direct_override = prev_override;
  return r;
}

bool ui_dispatch_synthetic_mouse(ccord_ui *ui, ccord_input_type type, uint8_t button, float x, float y)
{
  if (!ui) return false;
  ccord_input_event ev = {
    .type = type,
    .x = x,
    .y = y,
    .button = button,
    .clicks = 1,
  };
  bool prev = ui->in_synthetic_mouse;
  ui->in_synthetic_mouse = true;
  bool r = ccord_ui_handle_input(ui, &ev);
  ui->in_synthetic_mouse = prev;
  return r;
}

void ccord_ui_set_mobile_mode(ccord_ui *ui, bool mobile)
{
  if (!ui) return;
  if (ui->force_mobile_mode == mobile) return;
  ui->force_mobile_mode = mobile;
  if (!mobile) {
    ui->left_drawer_open = false;
    ui->right_drawer_open = false;
    ui->left_drawer_t = 0.0f;
    ui->right_drawer_t = 0.0f;
  }
  ui_compute_layout(ui);
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

bool ccord_ui_is_mobile_layout(const ccord_ui *ui)
{
  return ui && ui->mobile_layout;
}

void ccord_ui_set_safe_insets(ccord_ui *ui, float top, float right, float bottom, float left)
{
  if (!ui) return;
  if (top < 0.0f) top = 0.0f;
  if (right < 0.0f) right = 0.0f;
  if (bottom < 0.0f) bottom = 0.0f;
  if (left < 0.0f) left = 0.0f;
  if (ui->safe_insets.top == top && ui->safe_insets.right == right &&
      ui->safe_insets.bottom == bottom && ui->safe_insets.left == left) {
    return;
  }
  ui->safe_insets = (ccord_layout_insets){top, right, bottom, left};
  ui_compute_layout(ui);
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

void ccord_ui_set_keyboard_inset(ccord_ui *ui, float bottom_px)
{
  if (!ui) return;
  if (bottom_px < 0.0f) bottom_px = 0.0f;
  // If the OS soft keyboard was dismissed externally (e.g. system back/chevron), clear touch focus
  // so tapping a text field again re-requests SDL_StartTextInput cleanly.
  if (ui->touch_mode && ui->keyboard_inset > 1.0f && bottom_px <= 1.0f) {
    ui->touch_text_focused = false;
    ui->ime_active = false;
  }
  if (ui->keyboard_inset == bottom_px) return;
  ui->keyboard_inset = bottom_px;
  ui_compute_layout(ui);
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

void ccord_ui_set_touch_mode(ccord_ui *ui, bool touch)
{
  if (!ui) return;
  ui->touch_mode = touch;
  if (touch) ui->mouse_in = false;
  ui->dirty = true;
}

void ccord_ui_set_left_drawer(ccord_ui *ui, bool open)
{
  if (!ui) return;
  ui->left_drawer_open = open;
  if (open) {
    ui->right_drawer_open = false;
    ui->right_drawer_t = 0.0f;
    if (ui->touch_text_focused) {
      ui->touch_text_focused = false;
      if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
    }
  }
  if (ui->anim.reduced_motion || ui->last_anim_ms == 0) {
    ui->left_drawer_t = open ? 1.0f : 0.0f;
  }
  ui_compute_layout(ui);
  ui->dirty = true;
}

void ccord_ui_set_right_drawer(ccord_ui *ui, bool open)
{
  if (!ui) return;
  ui->right_drawer_open = open;
  if (open) {
    ui->left_drawer_open = false;
    ui->left_drawer_t = 0.0f;
    if (ui->touch_text_focused) {
      ui->touch_text_focused = false;
      if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
    }
  }
  if (ui->anim.reduced_motion || ui->last_anim_ms == 0) {
    ui->right_drawer_t = open ? 1.0f : 0.0f;
  }
  ui_compute_layout(ui);
  ui->dirty = true;
}

bool ccord_ui_left_drawer_open(const ccord_ui *ui)
{
  return ui && ui->left_drawer_open;
}

bool ccord_ui_right_drawer_open(const ccord_ui *ui)
{
  return ui && ui->right_drawer_open;
}

bool ccord_ui_handle_back(ccord_ui *ui)
{
  if (!ui) return false;
  ccord_input_event esc = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_ESCAPE};

  // 1. Soft keyboard / IME composition first.
  if (ui->ime_preedit[0]) {
    ui_ime_preedit_clear(ui);
    return true;
  }
  if (ui->keyboard_inset > 1.0f || ui->touch_text_focused) {
    ui->touch_text_focused = false;
    if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
    ui->dirty = true;
    return true;
  }

  // 2. Topmost plugin dialogs/settings, composer & profile popovers/modals.
  if (ui_upload_error_is_open(ui)) { ui_upload_error_close(ui); return true; }
  if (ui_plugin_dialog_is_open(ui)) { (void)ui_plugin_dialog_handle_input(ui, &esc); ui->dirty = true; return true; }
  if (ui_plugin_settings_is_open(ui)) { (void)ui_plugin_settings_handle_input(ui, &esc); ui->dirty = true; return true; }
  if (ui->report_open) { ui_report_flow_close(ui); ui->dirty = true; return true; }
  if (ui->schedule_open) { ui_schedule_dialog_close(ui); ui->dirty = true; return true; }
  if (ui->poll_open) { ui_poll_composer_close(ui); ui->dirty = true; return true; }
  if (ui->plus_menu_open) { ui_plus_menu_close(ui); ui->dirty = true; return true; }
  if (ui->clan_pop && ui_clan_popover_is_open(ui)) { (void)ui_clan_popover_input(ui, &esc); ui->dirty = true; return true; }
  if (ui_profile_roles_picker_open(ui)) { ui_profile_roles_close_picker(ui); return true; }
  if (ui->sc.open) { ui_status_card_close(ui); ui->dirty = true; return true; }
  if (ui_audio_popover_is_open(ui)) { ui_audio_popover_close(ui); ui->dirty = true; return true; }

  // 3. Stream fullscreen, lightbox, quick switcher, dropdowns, modals, shortcuts.
  if (ui->stream_fullscreen) {
    ui_stream_set_fullscreen(ui, false);
    ui->dirty = true;
    return true;
  }
  if (ui->shortcuts_open) { ui->shortcuts_open = false; ui->dirty = true; return true; }
  if (ui_scheduled_events_active(ui)) {
    return ui_scheduled_events_handle_back(ui);
  }
  if (ui->lightbox_open || ui->quick_switcher_open || ui->select_open || ui->modal_kind != UI_MODAL_NONE) {
    (void)ui_overlays_handle(ui, &esc);
    ui->dirty = true;
    return true;
  }

  // 4. Context menu (sub-level unwinds back to top-level on mobile first).
  if (ui->menu.open) {
    if (ui->mobile_layout && ui->mobile_menu_in_sub && ui->menu.open_sub >= 0) {
      ui->mobile_menu_in_sub = false;
      ui->menu.open_sub = -1;
      ui->menu.sub_scroll_y = 0.0f;
      ui->dirty = true;
      return true;
    }
    ui_menu_close(ui);
    return true;
  }

  // 5. Header popovers, emoji picker, member popout, Go Live, autocomplete.
  if (ui->emoji_picker_open) { ui_expr_picker_close(ui); return true; }
  if (ui->autocomplete_open) { ui->autocomplete_open = false; ui->dirty = true; return true; }
  if (ui->member_popover_open) { ui->member_popover_open = false; ui->dirty = true; return true; }
  if (ui->inbox_open) { ui->inbox_open = false; ui->dirty = true; return true; }
  if (ui->saved_popover_open) { ui->saved_popover_open = false; ui->dirty = true; return true; }
  if (ui->reactions_open) { ui->reactions_open = false; ui->dirty = true; return true; }
  if (ui->pins_open) { ui->pins_open = false; ui->dirty = true; return true; }
  if (ui->search_open) {
    ui->search_open = false;
    if (ui->mobile_layout) ui->right_drawer_open = false;
    ui->view_key = 0;
    ui_compute_layout(ui);
    ccord_vlist_invalidate_all(&ui->vlist);
    ui_sync_messages(ui);
    ui->dirty = true;
    return true;
  }
  if (ui->go_live_open) { ui->go_live_open = false; ui->dirty = true; return true; }
  if (ui_panels_handle_back(ui)) return true;

  // 6. Full-screen settings (mobile 2-level stack unwinds detail -> categories -> close).
  if (ui_channel_settings_is_open(ui)) {
    if (ui->mobile_layout && ui->mobile_channel_settings_detail) {
      ui->mobile_channel_settings_detail = false;
      ui->dirty = true;
      return true;
    }
    ui_channel_settings_close(ui);
    return true;
  }
  if (ui_server_settings_is_open(ui)) {
    return ui_server_settings_handle_back(ui);
  }
  if (ui->settings_open) {
    if (ui->account_mfa_required) {
      ui_settings_dismiss_mfa(ui);
      return true;
    }
    if (ui_is_login_screen(ui)) return false;
    if (ui->settings_sounds_open) {
      ui->settings_sounds_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->mobile_layout && ui->mobile_settings_detail) {
      ui->mobile_settings_detail = false;
      ui->dirty = true;
      return true;
    }
    ui_open_settings(ui, false);
    return true;
  }

  // 7. Discovery, voice message recorder/review, forum composer, voice stage, edit/reply states, and open drawers.
  if (ui->discovery_open) {
    ui->discovery_open = false;
    ui_compute_layout(ui);
    ui->dirty = true;
    return true;
  }
  if (ui_guild_view_active(ui)) {
    ui_guild_view_close(ui);
    return true;
  }
  if (ui->vm.recording || ui->vm.reviewing) {
    ui_vm_cancel(ui);
    ui->dirty = true;
    return true;
  }
  if (ui_forum_handle_back(ui)) return true;
  if (ui->stage_channel != CCORD_ID_NONE) {
    if (ui->stage_chat_open) {
      ui->stage_chat_open = false;
      ui_compute_layout(ui);
      ui->dirty = true;
      return true;
    }
    ui_clear_voice_stage(ui);
    ui->dirty = true;
    return true;
  }
  if (ui->editing_message != CCORD_ID_NONE) { ui_cancel_edit(ui); return true; }
  if (ui->reply_message != CCORD_ID_NONE) { ui_cancel_reply(ui); return true; }
  // Hierarchical mobile Back: Right Member Drawer -> Chat -> Left Server/Channel Drawer -> minimize app.
  // Opening the Left Drawer from root chat ensures accidental Android edge-Back swipes never kick the user home.
  if (ui->right_drawer_open) {
    ui->right_drawer_open = false;
    ui->dirty = true;
    return true;
  }
  if (ui->mobile_layout) {
    if (!ui->left_drawer_open) {
      const ccord_channel *cur_ch = ui->d.store ? ui_selected_channel(ui) : nullptr;
      if (cur_ch && ccord_discord_type_is_thread(cur_ch->discord_type) && cur_ch->parent_id != CCORD_ID_NONE) {
        ccord_error err;
        if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, cur_ch->parent_id, &err);
        else (void)ccord_store_select_channel(ui->d.store, cur_ch->parent_id, &err);
        ui_invalidate_rows(ui);
        ui_ensure_caches(ui);
        ui_sync_messages(ui);
        ui_save_current_location(ui);
        ui->dirty = true;
        return true;
      }
      ui->left_drawer_open = true;
      ui->dirty = true;
      return true;
    }
    return false;   // Back from Left Drawer: let Android background the Activity
  }
  if (ui->left_drawer_open) {
    ui->left_drawer_open = false;
    ui->dirty = true;
    return true;
  }
  return false;
}
