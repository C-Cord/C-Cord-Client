// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ui_core_internal.h"

static const ccord_ui *s_suppress_next_text_ui;

static bool handle_key(ccord_ui *ui, const ccord_input_event *ev)
{
  if (s_suppress_next_text_ui == ui) s_suppress_next_text_ui = nullptr;
  bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
  bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
  bool alt = (ev->mods & CCORD_MOD_ALT) != 0;
  if (ui->stream_fullscreen) {
    ui->stream_fs_hud_ms = ui->now_ms;
    if (ev->key == CCORD_KEY_ESCAPE || (!ctrl && !alt && !shift && ev->key == CCORD_KEY_F)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      ui_stream_set_fullscreen(ui, false);
      return true;
    }
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_MUTE, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      uc_toggle_voice_mute(ui);
      return true;
    }
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_DEAFEN, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      uc_toggle_voice_deafen(ui);
      return true;
    }
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_PTT, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      return true;
    }
    ui->dirty = true;
    return true;
  }
  if (ui->shortcuts_open) return ui_shortcuts_handle(ui, ev);
  if (ui->lightbox_open || ui->quick_switcher_open || ui->select_open || ui->modal_kind != UI_MODAL_NONE) {
    return ui_overlays_handle(ui, ev);
  }
  if (ui_server_settings_is_open(ui)) return ui_server_settings_handle(ui, ev);
  if (ui_channel_settings_is_open(ui)) return ui_channel_settings_handle(ui, ev);
  if (ui_panels_has_focused_input(ui) && ui_panels_handle_input(ui, ev)) return true;
  if (ui_plugin_keybinds_handle_key(ui, ev)) return true;
  if (ev->key == CCORD_KEY_ESCAPE) {
    if (ui->ime_preedit[0]) {
      ui_ime_preedit_clear(ui);
      return true;
    }
    if (ui->inbox_open) {
      ui->inbox_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->saved_popover_open) {
      ui->saved_popover_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->reactions_open) {
      ui->reactions_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->emoji_picker_open) {
      ui_expr_picker_close(ui);
      return true;
    }
    if (ui->autocomplete_open) {
      ui->autocomplete_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->member_popover_open) {
      ui->member_popover_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->go_live_open) {
      ui->go_live_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->search_open) {
      ui->search_open = false;
      ui->view_key = 0;
      ui_compute_layout(ui);
      ccord_vlist_invalidate_all(&ui->vlist);
      ui_sync_messages(ui);
      ui->dirty = true;
      return true;
    }
    if (ui->pins_open) {
      ui->pins_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->menu.open) {
      ui_menu_close(ui);
      return true;
    }
    if (ui->settings_open) {
      if (ui->account_mfa_required) {
        ui_settings_dismiss_mfa(ui);
      } else if (!ui_is_login_screen(ui)) {
        ui_open_settings(ui, false);
      }
      return true;
    }
    if (ui->discovery_open) {
      return ui_discovery_handle(ui, ev);
    }
    if (ui->editing_message != CCORD_ID_NONE) {
      ui_cancel_edit(ui);
      return true;
    }
    if (ui->reply_message != CCORD_ID_NONE) {
      ui_cancel_reply(ui);
      return true;
    }
    if (ui->staged_file_count > 0) {
      ccord_ui_clear_attachment(ui);
      return true;
    }
    if (ui->chat_sel_active) {
      ui->chat_sel_active = false;
      ui->chat_sel_len = 0;
      ui->chat_sel_buf[0] = '\0';
      ui->dirty = true;
      return true;
    }
    if (ui->role_preview_guild != CCORD_ID_NONE) {
      ui_stop_role_preview(ui);
      return true;
    }
    const ccord_channel *esc_ch = ui_selected_channel(ui);
    if (ui->new_marker_active || (esc_ch && (esc_ch->unread_count > 0 || esc_ch->mention_count > 0))) {
      ui_dismiss_new_marker(ui, true);
      return true;
    }
    return false;
  }
  if (!ui->settings_open && !ui->menu.open && !ui->go_live_open) {
    // Rebindable action keybinds (Settings > Keybinds) win over the fixed fallback chords below.
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_MUTE, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      uc_toggle_voice_mute(ui);
      return true;
    }
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_DEAFEN, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      uc_toggle_voice_deafen(ui);
      return true;
    }
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_QUICK_SWITCHER, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      ui->quick_switcher_open = !ui->quick_switcher_open;
      ui->inbox_open = false;
      ccord_text_edit_clear(&ui->quick_switcher_edit);
      ui_quick_switcher_reset_list(ui);
      ui->dirty = true;
      return true;
    }
    if (ui_keybind_match(ui, CCORD_UI_KEYBIND_PTT, ev)) {
      if (!ctrl && !alt) s_suppress_next_text_ui = ui;
      return true;
    }
    if (ctrl && !alt) {
      if (ev->key == CCORD_KEY_SLASH) {
        ui->shortcuts_open = !ui->shortcuts_open;
        ui->inbox_open = false;
        ui->pins_open = false;
        ui->search_open = false;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_K) {
        // Fixed Ctrl+K fallback only while Quick Switcher keeps its default bind; a rebind moves
        // the action to the new chord (matched above) and frees Ctrl+K.
        ccord_input_event def = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_K, .mods = CCORD_MOD_CTRL};
        if (ui_keybind_match(ui, CCORD_UI_KEYBIND_QUICK_SWITCHER, &def)) {
          ui->quick_switcher_open = !ui->quick_switcher_open;
          ui->inbox_open = false;
          ccord_text_edit_clear(&ui->quick_switcher_edit);
          ui_quick_switcher_reset_list(ui);
          ui->dirty = true;
          return true;
        }
      }
      if (ev->key == CCORD_KEY_F) {
        ui->search_open = !ui->search_open;
        ui->pins_open = false;
        ui->inbox_open = false;
        ccord_text_edit_clear(&ui->search_edit);
        uc_search_reset(ui);
        ui->view_key = 0;
        ui_sync_messages(ui);
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_P) {
        ui->pins_open = !ui->pins_open;
        ui->search_open = false;
        ui->inbox_open = false;
        if (ui->pins_open) uc_request_pins(ui);
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_I &&
          (shift || (ui->edit.len == 0 && ui->edit.caret == ui->edit.anchor && !ui->search_open && !ui_forum_is_active(ui)))) {
        ui->inbox_open = !ui->inbox_open;
        ui->inbox_scroll = 0.0f;
        ui->pins_open = false;
        ui->search_open = false;
        ui->dirty = true;
        return true;
      }
    }
    if (alt && !ctrl && !shift && (ev->key == CCORD_KEY_LEFT || ev->key == CCORD_KEY_RIGHT)) {
      ui->inbox_open = false;
      return ui_nav_step(ui, ev->key == CCORD_KEY_LEFT ? -1 : 1);
    }
    if (alt && (ev->key == CCORD_KEY_UP || ev->key == CCORD_KEY_DOWN)) {
      int dir = (ev->key == CCORD_KEY_DOWN) ? 1 : -1;
      ui->inbox_open = false;
      if (ctrl && shift) return ui_jump_unread_channel(ui, dir, true);
      if (ctrl) return ui_cycle_guild(ui, dir);
      if (shift) return ui_jump_unread_channel(ui, dir, false);
      return ui_cycle_channel(ui, dir);
    }
  }
  if (ui->go_live_open) return false;          // modal: only Esc
  if (ui->pins_open || ui->reactions_open || ui->inbox_open || ui->saved_popover_open) return false;   // popover: only Esc
  if (ui->emoji_picker_open) return ui_expr_handle_event(ui, ev);
  if (ui->search_open) {
    switch (ev->key) {
      case CCORD_KEY_BACKSPACE:
        if (ctrl) ccord_text_edit_backspace_word(&ui->search_edit);
        else ccord_text_edit_backspace(&ui->search_edit);
        break;
      case CCORD_KEY_DELETE:
        if (ctrl) ccord_text_edit_delete_word(&ui->search_edit);
        else ccord_text_edit_delete(&ui->search_edit);
        break;
      case CCORD_KEY_LEFT: ccord_text_edit_move(&ui->search_edit, ctrl ? -3 : -1, shift); break;
      case CCORD_KEY_RIGHT: ccord_text_edit_move(&ui->search_edit, ctrl ? 3 : 1, shift); break;
      case CCORD_KEY_HOME: ccord_text_edit_move(&ui->search_edit, -2, shift); break;
      case CCORD_KEY_END: ccord_text_edit_move(&ui->search_edit, 2, shift); break;
      case CCORD_KEY_A: if (!ctrl) return false; ccord_text_edit_select_all(&ui->search_edit); break;
      case CCORD_KEY_RETURN:
        uc_search_send(ui, true);
        return true;
      default: return false;
    }
    uc_search_schedule(ui);
    ui->view_key = 0;
    ui_compute_layout(ui);
    ui_sync_messages(ui);
    ui->dirty = true;
    return true;
  }
  if (ui->settings_open && (ui->account_focus_field != 0 || ui->account_mfa_required)) {
    return ui_settings_handle(ui, ev);
  }
  // Up/Down step through the settings sidebar.
  if (ui->settings_open && !ui->menu.open && (ev->key == CCORD_KEY_UP || ev->key == CCORD_KEY_DOWN)) {
    return ui_settings_handle(ui, ev);
  }
  if (ui->menu.open || ui->settings_open) return false;   // modal: only Esc
  if (ui->discovery_open) {
    if (ui_discovery_handle(ui, ev)) {
      uc_reset_caret_blink(ui);
      return true;
    }
    return false;
  }
  if (ui_forum_is_active(ui)) {
    if (ui_forum_handle_event(ui, ev)) {
      uc_reset_caret_blink(ui);
      return true;
    }
    return false;
  }
  if (!ctrl && !alt && !shift && ev->key == CCORD_KEY_F && ui->stage_channel != CCORD_ID_NONE &&
      !uc_composer_accepts_text(ui) && ui_stream_is_active(ui)) {
    s_suppress_next_text_ui = ui;
    ui_stream_set_fullscreen(ui, true);
    return true;
  }
  if (ui->autocomplete_open) {
    if (ev->key == CCORD_KEY_UP) {
      ui->autocomplete_sel = ui->autocomplete_sel > 0 ? ui->autocomplete_sel - 1 : ui->nautocomplete - 1;
      ui->dirty = true;
      return true;
    }
    if (ev->key == CCORD_KEY_DOWN) {
      ui->autocomplete_sel = (ui->autocomplete_sel + 1) % ui->nautocomplete;
      ui->dirty = true;
      return true;
    }
    if (ev->key == CCORD_KEY_TAB || (ev->key == CCORD_KEY_RETURN && !shift)) {
      if (ui_accept_autocomplete(ui, ui->autocomplete_sel)) {
        uc_reset_caret_blink(ui);
        return true;
      }
    }
  }
  switch (ev->key) {
    case CCORD_KEY_RETURN:
      if (shift) {
        ccord_error err;
        if (!uc_composer_accepts_text(ui) || ui_composer_lock_reason(ui)) break;
        if (ccord_text_edit_insert(&ui->edit, "\n", 1, &err) != CCORD_OK) ui_notice(ui, "%s", err.message);
        else ui_send_typing(ui);
        ui_update_autocomplete(ui);
      } else {
        ui_send_composer(ui);
      }
      break;
    case CCORD_KEY_BACKSPACE:
      if (ctrl) ccord_text_edit_backspace_word(&ui->edit);
      else ccord_text_edit_backspace(&ui->edit);
      ui_update_autocomplete(ui);
      ui_notify_composer_changed(ui);
      break;
    case CCORD_KEY_DELETE:
      if (ctrl) ccord_text_edit_delete_word(&ui->edit);
      else ccord_text_edit_delete(&ui->edit);
      ui_update_autocomplete(ui);
      ui_notify_composer_changed(ui);
      break;
    case CCORD_KEY_LEFT:
      ccord_text_edit_move(&ui->edit, ctrl ? -3 : -1, shift);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_RIGHT:
      ccord_text_edit_move(&ui->edit, ctrl ? 3 : 1, shift);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_HOME:
      ccord_text_edit_move(&ui->edit, ctrl ? -2 : -4, shift);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_END:
      ccord_text_edit_move(&ui->edit, ctrl ? 2 : 4, shift);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_UP:
      if (ui->edit.len == 0 && ui->editing_message == CCORD_ID_NONE && !ctrl && !shift) {
        ccord_id ch = ccord_store_selected_channel(ui->d.store);
        ccord_id self_id = ccord_store_self_id(ui->d.store);
        size_t mc = ccord_store_message_count(ui->d.store, ch);
        for (size_t i = mc; i > 0; i--) {
          const ccord_message *m = ccord_store_message_at(ui->d.store, ch, i - 1);
          if (m && m->author_id == self_id && !ccord_message_is_ephemeral(m) &&
              m->send_state == CCORD_MSG_SEND_CONFIRMED) {
            ui_begin_edit(ui, ch, m->id);
            return true;
          }
        }
        return false;
      }
      if (ui->edit.len > 0) {
        ui->pending_vert_move = -1;
        ui->pending_extend = shift;
        break;
      }
      return false;
    case CCORD_KEY_DOWN:
      if (ui->edit.len > 0) {
        ui->pending_vert_move = 1;
        ui->pending_extend = shift;
        break;
      }
      return false;
    case CCORD_KEY_PAGEUP:
    case CCORD_KEY_PAGEDOWN:
      if (ui->edit.len == 0) {
        ui->msg_scroll_vel = 0.0f;
        uc_scroll_messages(ui, ev->key == CCORD_KEY_PAGEUP ? -uc_page_step(ui) : uc_page_step(ui));
        return true;
      }
      return false;
    case CCORD_KEY_Z:
      if (!ctrl) return false;
      if (shift) (void)ccord_text_edit_redo(&ui->edit);
      else (void)ccord_text_edit_undo(&ui->edit);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_Y:
      if (!ctrl) return false;
      (void)ccord_text_edit_redo(&ui->edit);
      ui_update_autocomplete(ui);
      break;
    case CCORD_KEY_B: {
      if (!ctrl) return false;
      ccord_error err;
      (void)ccord_text_edit_wrap_selection(&ui->edit, "**", "**", &err);
      break;
    }
    case CCORD_KEY_I: {
      if (!ctrl) return false;
      ccord_error err;
      (void)ccord_text_edit_wrap_selection(&ui->edit, "*", "*", &err);
      break;
    }
    case CCORD_KEY_U: {
      if (!ctrl) return false;
      ccord_error err;
      (void)ccord_text_edit_wrap_selection(&ui->edit, "__", "__", &err);
      break;
    }
    case CCORD_KEY_S: {
      if (!ctrl || !shift) return false;
      ccord_error err;
      (void)ccord_text_edit_wrap_selection(&ui->edit, "~~", "~~", &err);
      break;
    }
    case CCORD_KEY_E: {
      if (!ctrl) return false;
      ccord_error err;
      (void)ccord_text_edit_wrap_selection(&ui->edit, "`", "`", &err);
      break;
    }
    case CCORD_KEY_A:
      if (!ctrl) return false;
      ccord_text_edit_select_all(&ui->edit);
      break;
    case CCORD_KEY_C:
    case CCORD_KEY_X: {
      if (!ctrl) return false;
      if (ui->edit.caret != ui->edit.anchor) {
        char *sel = ccord_text_edit_copy_selection(&ui->edit);
        if (sel && ui->d.host.clipboard_set) ui->d.host.clipboard_set(ui->d.host.ud, sel);
        ccord_free(sel);
        if (ev->key == CCORD_KEY_X) ccord_text_edit_delete(&ui->edit);
      } else if (ev->key == CCORD_KEY_C && ui->chat_sel_active && ui->chat_sel_len > 0) {
        if (ui->d.host.clipboard_set) ui->d.host.clipboard_set(ui->d.host.ud, ui->chat_sel_buf);
        ui_notice(ui, "Copied selected text");
      }
      break;
    }
    case CCORD_KEY_V:
      if (!ctrl) return false;
      uc_paste(ui);
      return true;
    default:
      return false;
  }
  uc_reset_caret_blink(ui);
  return true;
}

static void ui_vlist_shrink_tight(ccord_vlist *v)
{
  if (!v) return;
  if (v->count == 0) {
    ccord_free(v->heights);
    ccord_free(v->height_epoch);
    v->heights = nullptr;
    v->height_epoch = nullptr;
    v->heights_cap = 0;
    return;
  }
  if (v->heights_cap > v->count) {
    size_t ncap = v->count;
    float *h = ccord_realloc_array(v->heights, ncap, sizeof *h);
    if (h) {
      v->heights = h;
      v->heights_cap = ncap;
      uint32_t *e = ccord_realloc_array(v->height_epoch, ncap, sizeof *e);
      if (e) v->height_epoch = e;
    }
  }
}

static uint64_t ui_overlay_hover_sig(const ccord_ui *ui)
{
  if (!ui || !ui->mouse_in) return 0u;
  float x = ui->mx, y = ui->my;
  uint64_t sig = 14695981039346656037ull;
#define HBIT(cond, id) do { if (cond) sig = uc_sig_mix_u64(sig, (uint64_t)(id)); } while (0)
  if (ui->shortcuts_open) {
    HBIT(ui_in_rect(x, y, ui->shortcuts_close_rect), 1);
    HBIT(ui_in_rect(x, y, ui->shortcuts_modal_rect), 2);
  } else if (ui->inbox_open) {
    HBIT(ui_in_rect(x, y, ui->inbox_mark_all_rect), 10);
    HBIT(ui_in_rect(x, y, ui->inbox_tab_rects[0]), 11);
    HBIT(ui_in_rect(x, y, ui->inbox_tab_rects[1]), 12);
    for (size_t i = 0; i < ui->ninbox_hits; i++) {
      if (ui_in_rect(x, y, ui->inbox_hits[i].card_rect)) sig = uc_sig_mix_u64(sig, 100u + i);
      if (ui_in_rect(x, y, ui->inbox_hits[i].ack_rect)) sig = uc_sig_mix_u64(sig, 200u + i);
    }
  } else if (ui->saved_popover_open) {
    HBIT(ui_in_rect(x, y, ui->saved_close_rect), 20);
    for (size_t i = 0; i < ui->nsaved_hits; i++) {
      if (ui_in_rect(x, y, ui->saved_hits[i].row)) sig = uc_sig_mix_u64(sig, 300u + i);
      if (ui_in_rect(x, y, ui->saved_hits[i].jump)) sig = uc_sig_mix_u64(sig, 400u + i);
      if (ui_in_rect(x, y, ui->saved_hits[i].remove)) sig = uc_sig_mix_u64(sig, 500u + i);
    }
  } else if (ui->select_open) {
    HBIT(ui->select_confirm_btn.w > 0.0f && ui_in_rect(x, y, ui->select_confirm_btn), 30);
    for (size_t i = 0; i < ui->select_opt_count; i++) {
      if (ui_in_rect(x, y, ui->select_opt_rects[i])) sig = uc_sig_mix_u64(sig, 600u + i);
    }
  } else if (ui->modal_kind != UI_MODAL_NONE) {
    sig = uc_sig_mix_u64(sig, 700u + (uint64_t)ui->modal_kind);
    if (ui->modal_kind == UI_MODAL_USER_PROFILE) {
      HBIT(ui_in_rect(x, y, ui->profile_modal_close_rect), 710);
      HBIT(ui->profile_modal_msg_rect.w > 0.0f && ui_in_rect(x, y, ui->profile_modal_msg_rect), 711);
      HBIT(ui_in_rect(x, y, ui->profile_modal_tab_rects[0]), 712);
      HBIT(ui_in_rect(x, y, ui->profile_modal_tab_rects[1]), 713);
      HBIT(ui_in_rect(x, y, ui->profile_modal_tab_rects[2]), 714);
      HBIT(ui->profile_modal_note_rect.w > 0.0f && ui_in_rect(x, y, ui->profile_modal_note_rect), 715);
      HBIT(ui->profile_modal_note_save_rect.w > 0.0f && ui_in_rect(x, y, ui->profile_modal_note_save_rect), 716);
      for (size_t i = 0; i < ui->nprofile_modal_mutual_hits; i++) {
        if (ui_in_rect(x, y, ui->profile_modal_mutual_hits[i].rect)) sig = uc_sig_mix_u64(sig, 800u + i);
      }
      for (size_t i = 0; i < ui->nprofile_modal_conn_hits; i++) {
        if (ui_in_rect(x, y, ui->profile_modal_conn_hits[i].rect)) sig = uc_sig_mix_u64(sig, 900u + i);
      }
    } else if (ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION) {
      HBIT(ui_in_rect(x, y, ui->mv.cancel), 720);
      HBIT(ui_in_rect(x, y, ui->mv.submit), 721);
      for (size_t i = 0; i < CCORD_MEMBER_VERIFICATION_MAX_FIELDS; i++) {
        if (ui->mv.checks[i].w > 0.0f && ui_in_rect(x, y, ui->mv.checks[i])) sig = uc_sig_mix_u64(sig, 950u + i);
      }
    } else if (ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS || ui->modal_kind == UI_MODAL_ACTIVITY_SHELF) {
      ccord_cursor_kind ck = (ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS)
                               ? ui_scheduled_events_cursor(ui)
                               : ui_activity_shelf_cursor(ui);
      sig = uc_sig_mix_u64(sig, (uint64_t)ck + 1u);
      if (ck != CCORD_CURSOR_DEFAULT) {
        sig = uc_sig_mix_u64(sig, ((uint64_t)(unsigned)(int)(x / 16.0f) << 16) ^ (uint64_t)(unsigned)(int)(y / 16.0f));
      }
    } else {
      HBIT(ui->modal_ok_rect.w > 0.0f && ui_in_rect(x, y, ui->modal_ok_rect), 730);
      HBIT(ui->modal_tab_rects[0].w > 0.0f && ui_in_rect(x, y, ui->modal_tab_rects[0]), 731);
      HBIT(ui->modal_tab_rects[1].w > 0.0f && ui_in_rect(x, y, ui->modal_tab_rects[1]), 732);
      HBIT(ui->modal_preview_btn_rect.w > 0.0f && ui_in_rect(x, y, ui->modal_preview_btn_rect), 733);
      HBIT(ui->modal_input_rect.w > 0.0f && ui_in_rect(x, y, ui->modal_input_rect), 734);
      HBIT(ui->modal_input2_rect.w > 0.0f && ui_in_rect(x, y, ui->modal_input2_rect), 735);
      // Cancel button sits directly left of modal_ok_rect (width 90, gap 10).
      ccord_rect cancel_r = {ui->modal_ok_rect.x - 100.0f, ui->modal_ok_rect.y, 90.0f, ui->modal_ok_rect.h};
      HBIT(ui->modal_ok_rect.w > 0.0f && ui_in_rect(x, y, cancel_r), 736);
      if (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS) {
        // Color preset strip sits between input2 bottom and ok top.
        if (y >= ui->modal_input2_rect.y + ui->modal_input2_rect.h && y <= ui->modal_ok_rect.y) {
          sig = uc_sig_mix_u64(sig, 737u + (uint64_t)(unsigned)(int)(x / 32.0f));
        }
      }
      for (size_t i = 0; i < ui->nnew_dm_friend_hits; i++) {
        if (ui_in_rect(x, y, ui->new_dm_friend_hits[i].rect)) sig = uc_sig_mix_u64(sig, 1000u + i);
      }
    }
  } else if (ui->member_popover_open) {
    HBIT(ui_in_rect(x, y, ui->member_popover_msg_btn), 1100);
    HBIT(ui_in_rect(x, y, ui->member_popover_friend_btn), 1101);
    HBIT(ui_in_rect(x, y, ui->member_popover_tab_rects[0]), 1102);
    HBIT(ui_in_rect(x, y, ui->member_popover_tab_rects[1]), 1103);
    HBIT(ui->popover_full_profile_rect.w > 0.0f && ui_in_rect(x, y, ui->popover_full_profile_rect), 1104);
  } else if (ui_is_dm_requests_view(ui) || ui->msg_req_banner_visible) {
    HBIT(ui_in_rect(x, y, ui->dm_requests_tab_rects[0]), 1200);
    HBIT(ui_in_rect(x, y, ui->dm_requests_tab_rects[1]), 1201);
    HBIT(ui->msg_req_banner_visible && ui_in_rect(x, y, ui->msg_req_banner_accept_rect), 1202);
    HBIT(ui->msg_req_banner_visible && ui_in_rect(x, y, ui->msg_req_banner_reject_rect), 1203);
    for (size_t i = 0; i < ui->ndm_request_hits; i++) {
      if (ui_in_rect(x, y, ui->dm_request_hits[i].card_rect)) sig = uc_sig_mix_u64(sig, 1300u + i);
      if (ui_in_rect(x, y, ui->dm_request_hits[i].accept_rect)) sig = uc_sig_mix_u64(sig, 1400u + i);
      if (ui_in_rect(x, y, ui->dm_request_hits[i].reject_rect)) sig = uc_sig_mix_u64(sig, 1500u + i);
    }
  } else if (ui->discovery_open) {
    HBIT(ui_in_rect(x, y, ui->discovery_search_rect), 1600);
    for (size_t i = 0; i < 6u; i++) {
      if (ui_in_rect(x, y, ui->discovery_cat_rects[i])) sig = uc_sig_mix_u64(sig, 1610u + i);
    }
    for (size_t i = 0; i < ui->ndiscovery_hits; i++) {
      if (ui_in_rect(x, y, ui->discovery_hits[i].join_rect)) sig = uc_sig_mix_u64(sig, 1700u + i);
    }
  }
  if (ui_stream_is_active(ui)) {
    HBIT(ui_in_rect(x, y, ui->stream_stage_btn1_rect), 1800);
    HBIT(ui_in_rect(x, y, ui->stream_stage_btn2_rect), 1801);
    HBIT(ui_in_rect(x, y, ui->stream_stage_pause_rect), 1802);
    HBIT(ui_in_rect(x, y, ui->stream_stage_fs_rect), 1803);
    HBIT(ui_in_rect(x, y, ui->stream_stage_vport_rect), 1804);
    HBIT(ui_in_rect(x, y, ui->stream_stage_resize_rect), 1805);
    ccord_rect sh = ui_stage_split_handle_rect(ui);
    HBIT(sh.w > 0.0f && ui_in_rect(x, y, sh), 1806);
    if (ui_stream_pip_active(ui)) {
      HBIT(ui_in_rect(x, y, ui->stream_pip_rect), 1810);
      HBIT(ui_in_rect(x, y, ui->stream_pip_fs_rect), 1811);
      HBIT(ui_in_rect(x, y, ui->stream_pip_stage_rect), 1812);
      HBIT(ui_in_rect(x, y, ui->stream_pip_close_rect), 1813);
      sig = uc_sig_mix_u64(sig, 1820u + (uint64_t)ui_stream_pip_cursor(ui, x, y));
    }
  }
#undef HBIT
  return sig;
}

bool ccord_ui_handle_input(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev) return false;
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  if (ev->type == CCORD_EV_IME_PREEDIT) {   // value beyond the ccord_input_type enum owned by platform.h
    ui_ime_preedit_set(ui, ev->text, ev->ime_start, ev->ime_length);
    return true;
  }
  if (ev->type == CCORD_IN_TOUCH_DOWN || ev->type == CCORD_IN_TOUCH_MOVE ||
      ev->type == CCORD_IN_TOUCH_UP || ev->type == CCORD_IN_TOUCH_CANCEL) {
    float prev_lt = ui->left_drawer_t, prev_rt = ui->right_drawer_t;
    bool handled = ui_touch_handle_input(ui, ev);
    if (ui->left_drawer_t != prev_lt || ui->right_drawer_t != prev_rt) {
      ui_compute_layout(ui);
    }
    return handled;
  }
  if (ev->type == CCORD_IN_SAFE_AREA) {
    ccord_ui_set_safe_insets(ui, ev->inset_top, ev->inset_right, ev->inset_bottom, ev->inset_left);
    return true;
  }
  if (ev->type == CCORD_IN_KEYBOARD_INSET) {
    ccord_ui_set_keyboard_inset(ui, ev->inset_bottom);
    return true;
  }
  if (ev->type == CCORD_IN_APP_BACKGROUND) {
    ui_menu_close(ui);
    ui->touch_text_focused = false;
    ui->ime_active = false;
    ui->window_focused = false;
    if (ui->media_playing) ui->media_playing = false;
    uc_media_player_close(ui);
    ui->media_url[0] = '\0';
    ui->media_att_id = CCORD_ID_NONE;
    if (ui->vm.recording) ui_vm_cancel(ui);
    if (ui->settings_save_dirty) {
      ui->settings_save_dirty = false;
      ui_settings_save(ui);
    }
    ui_sync_reading(ui);
    return false;
  }
  if (ev->type == CCORD_IN_APP_FOREGROUND) {
    ui->window_focused = true;
    ui_sync_reading(ui);
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_RENDER_RESET) {
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_APP_LOW_MEMORY) {
    if (!ui->lightbox_open) ui_close_lightbox(ui);
    if (!ui->select_open) ui_close_select_dropdown(ui);
    if (ui->modal_kind != UI_MODAL_INTERACTION) ui_free_inter_modal(ui);
    ccord_free(ui->media_reqs);
    ui->media_reqs = nullptr;
    ui->nmedia_reqs = 0;
    ui_track_free(&ui->trk_chan);
    ui_track_free(&ui->trk_member);
    ui_track_free(&ui->trk_rail);
    if (ui->rail_cache) {
      ccord_free(ui->rail_cache->slots);
      ui->rail_cache->slots = nullptr;
      ui->rail_cache->count = 0;
      ui->rail_cache->cap = 0;
      ui->rail_cache->valid = false;
    }
    ui_nav_caches_free(ui);
    ui_chat_caches_free(ui);
    ui_composer_format_free();
    ui_vlist_shrink_tight(&ui->vlist);
    ccord_arena_free(&ui->hit_arena);
    ccord_arena_init(&ui->hit_arena, 4096u);
    ui->nlinks = 0;
    ui->nimg_hits = 0;
    ui->nmedia_hits = 0;
    ui->ncomp_hits = 0;
    ui->ncode_copy_hits = 0;
    ui->nprofile_modal_conn_hits = 0;
    ui->dirty = true;
    return false;
  }
  if (ev->type == CCORD_IN_KEY_DOWN && ev->key == CCORD_KEY_BACK) {
    return ccord_ui_handle_back(ui);
  }
  if (ev->type != CCORD_IN_RESIZE && ev->type != CCORD_IN_FOCUS && ev->type != CCORD_IN_QUIT) {
    if (ev->type == CCORD_IN_MOUSE_MOVE || ev->type == CCORD_IN_MOUSE_DOWN || ev->type == CCORD_IN_MOUSE_UP || ev->type == CCORD_IN_WHEEL) {
      ui->mx = ev->x;
      ui->my = ev->y;
    }
    if (ui_plugin_dialog_is_open(ui) && ui_plugin_dialog_handle_input(ui, ev)) return true;
    if (ui_plugin_settings_is_open(ui) && ui_plugin_settings_handle_input(ui, ev)) return true;
  }
  if (ev->type != CCORD_IN_RESIZE && ev->type != CCORD_IN_FOCUS && ev->type != CCORD_IN_QUIT) {   // composer overlays sit above the views; topmost modal first
    if (ui->report_open && ui_report_flow_input(ui, ev)) return true;
    if (ui->schedule_open && ui_schedule_dialog_input(ui, ev)) return true;
    if (ui->poll_open && ui_poll_composer_input(ui, ev)) return true;
    if (ui->plus_menu_open && ui_plus_menu_input(ui, ev)) return true;
    if (ui->clan_pop && ui_clan_popover_input(ui, ev)) return true;
    if (ui->prof_roles && ui_profile_roles_input(ui, ev)) return true;
  }
  if (ui->sc.open && ev->type != CCORD_IN_RESIZE && ev->type != CCORD_IN_FOCUS && ev->type != CCORD_IN_QUIT &&
      ui_status_card_handle(ui, ev)) return true;   // account card
  switch (ev->type) {
    case CCORD_IN_RESIZE:
      ccord_ui_set_viewport(ui, ev->x, ev->y);
      if (!ui->mobile_layout && !ui->force_mobile_mode && !ui->stream_fullscreen && ev->x >= 320.0f && ev->y >= 240.0f) {
        ui->settings_win_w = (int)ev->x;
        ui->settings_win_h = (int)ev->y;
        ui->settings_win_saved = true;
        ui->settings_save_dirty = true;
        ui->settings_save_due_ms = ui->now_ms + 500u;
      }
      return true;
    case CCORD_IN_FOCUS:
      ui->window_focused = ev->focused;
      ui_sync_reading(ui);
      if (!ev->focused) {
        ui_ime_preedit_clear(ui);
        if (ui->touch_mode) {
          ui->ime_active = false;
          ui->touch_text_focused = false;
        }
        if (s_suppress_next_text_ui == ui) s_suppress_next_text_ui = nullptr;
      }
      uc_reset_caret_blink(ui);
      return true;
    case CCORD_IN_QUIT:
      if (ui->d.host.request_quit) ui->d.host.request_quit(ui->d.host.ud);
      return false;
    case CCORD_IN_MOUSE_LEAVE:
      ui->mouse_in = false;
      ui->drag = DRAG_NONE;
      ui->drag_active = false;
      ui->drag_drop_slot = -1;
      ui->drag_drop_zone = UI_DROP_NONE;
      ui->drag_chan_target_id = CCORD_ID_NONE;
      uc_update_hover(ui);
      ui->dirty = true;
      return true;
    case CCORD_IN_MOUSE_MOVE: {
      if (!ui->in_synthetic_mouse) ui->touch_mode = false;
      ui->mx = ev->x;
      ui->my = ev->y;
      ui->mouse_in = true;
      if (ui->stream_fullscreen) {
        ui->stream_fs_hud_ms = ui->now_ms;
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      bool changed = false;
      bool check_ovl_hover = false;
      if (ui->shortcuts_open || ui->inbox_open || ui->saved_popover_open || ui->select_open) {
        check_ovl_hover = true;
      } else if (ui->modal_kind != UI_MODAL_NONE) {
        if (ui->modal_kind == UI_MODAL_ADD_SERVER || ui->modal_kind == UI_MODAL_FOLDER_SETTINGS ||
            ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT ||
            ui->modal_kind == UI_MODAL_EDIT_GROUP_DM ||
            ui->modal_kind == UI_MODAL_USER_PROFILE || ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS ||
            ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION || ui->modal_kind == UI_MODAL_ACTIVITY_SHELF) {
          check_ovl_hover = true;
        }
      } else if (ui->member_popover_open || ui_is_dm_requests_view(ui) || ui->msg_req_banner_visible) {
        check_ovl_hover = true;
      } else if (ui_server_settings_is_open(ui)) {
        changed = ui_server_settings_handle(ui, ev);
      } else if (ui_channel_settings_is_open(ui)) {
        changed = ui_channel_settings_handle(ui, ev);
      } else if (ui->go_live_open) {
        changed = ui_go_live_handle(ui, ev);
      } else if (ui->menu.open) {
        int old = ui->menu.hover;
        ui_menu_handle_mouse(ui, ev);
        changed = old != ui->menu.hover;
      } else if (ui->settings_open) {
        changed = ui_settings_handle(ui, ev);
      } else if (ui->search_open) {
        changed = ui_search_handle(ui, ev);
      } else if (ui->pins_open) {
        changed = ui_pins_handle(ui, ev);
      } else if (ui->emoji_picker_open) {
        changed = ui_expr_handle_event(ui, ev);
      } else if (ui->mobile_layout && (ui->layout.left_drawer_visible || ui->layout.right_drawer_visible)) {
        // Mobile drawers sit above discovery and forum views; let drawer hover/panels run below.
        if (ui->drag == DRAG_NONE && !ui->menu.open && !ui->settings_open) {
          if (ui_panels_handle_input(ui, ev)) changed = true;
        }
      } else if (ui->discovery_open) {
        check_ovl_hover = true;
      } else if (ui_forum_is_active(ui)) {
        (void)ui_forum_handle_event(ui, ev);
      } else if (ui->drag == DRAG_NONE && !ui->menu.open && !ui->settings_open) {
        if (ui_panels_handle_input(ui, ev)) changed = true;
      }
      if (ui->drag == DRAG_SCROLLBAR) {
        uc_scrollbar_drag_to(ui, ev->y);
        changed = true;
      } else if (ui->drag == DRAG_COMPOSER) {
        ui->pending_click = true;
        ui->pending_extend = true;
        ui->pending_x = ev->x;
        ui->pending_y = ev->y;
        changed = true;
      } else if (ui->drag == DRAG_CHAT_SELECT) {
        ui->chat_sel_cx = ev->x;
        ui->chat_sel_cy = ev->y;
        ui->dirty = true;
        changed = true;
      } else if (ui->drag == DRAG_SERVER) {
        uc_update_server_drag(ui, ev->x, ev->y);
        changed = true;
      } else if (ui->drag == DRAG_CHANNEL) {
        uc_update_channel_drag(ui, ev->x, ev->y);
        changed = true;
      } else if (ui->drag == DRAG_STREAM_STAGE_H) {
        float avail = ui->stream_stage_avail_h;
        if (avail > 1.0f) {
          float min_h = fminf(160.0f, avail);
          float max_h = fmaxf(avail - 14.0f, min_h);
          float nh = uc_clampf(ev->y - ui->stream_stage_player_y, min_h, max_h);
          ui->stream_stage_height_frac = uc_clampf(nh / avail, 0.05f, 1.0f);
          ui->dirty = true;
          changed = true;
        }
      } else if (ui->drag == DRAG_STAGE_SPLIT) {
        float x0 = ui->layout.stage_pane.x;
        float total_w = ui->win_w - x0;
        if (total_w > 1.0f) {
          ui->stage_split_ratio = uc_clampf((ev->x - x0) / total_w, 0.15f, 0.85f);
          ui_compute_layout(ui);
          ccord_vlist_invalidate_all(&ui->vlist);
          ui->dirty = true;
          changed = true;
        }
      } else if (ui->drag == DRAG_STREAM_PIP_MOVE || ui->drag == DRAG_STREAM_PIP_RESIZE) {
        ui_stream_pip_drag_move(ui, ev->x, ev->y);
        changed = true;
      } else if (ui_stream_is_active(ui)) {
        check_ovl_hover = true;
      }
      if (check_ovl_hover) {
        uint64_t hsig = ui_overlay_hover_sig(ui);
        if (hsig != ui->last_ovl_hover_sig) {
          ui->last_ovl_hover_sig = hsig;
          ui->dirty = true;
          changed = true;
        }
      } else {
        ui->last_ovl_hover_sig = 0u;
      }
      bool hc = uc_update_hover(ui);
      if (hc) ui->dirty = true;
      return changed || hc;
    }
    case CCORD_IN_MOUSE_DOWN: {
      ui->mx = ev->x;
      ui->my = ev->y;
      ui->mouse_in = true;
      if (ui->stream_fullscreen) {
        ui_stream_fullscreen_handle(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      bool handled = false;
      if (ui->shortcuts_open) {
        ui_shortcuts_handle(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->lightbox_open || ui->quick_switcher_open || ui->select_open || ui->modal_kind != UI_MODAL_NONE) {
        ui_overlays_handle(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->inbox_open) {
        ui_inbox_handle(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->saved_popover_open) {
        ui_saved_messages_handle(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->reactions_open) {
        ui_reactions_handle(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui_server_settings_is_open(ui)) {
        (void)ui_server_settings_handle(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui_channel_settings_is_open(ui)) {
        (void)ui_channel_settings_handle(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->go_live_open) {
        ui_go_live_handle(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->search_open) {
        ui_search_handle(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->pins_open) {
        ui_pins_handle(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->menu.open) {
        ui_menu_handle_mouse(ui, ev);   // activates or closes; the click is consumed
        handled = true;
        if (ev->button != 3) {
          uc_update_hover(ui);
          ui->dirty = true;
          return true;
        }
      }
      if (ui->settings_open) {
        bool r = ui_settings_handle(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return r;
      }
      if (ui->member_popover_open && ui_member_popover_handle(ui, ev)) {
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      bool mobile_drawer_vis = ui->mobile_layout && (ui->layout.left_drawer_visible || ui->layout.right_drawer_visible);
      if (!mobile_drawer_vis && ui->discovery_open && ev->button != 4 && ev->button != 5 && ui_discovery_handle(ui, ev)) {
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      // An open picker owns right-clicks inside its popover (right-click stars a GIF, sticker or emoji tile); a
      // right-click outside it still opens the chat menu behind it.
      if (ev->button == 3 && ui->emoji_picker_open && ui_in_rect(ev->x, ev->y, ui->emoji_picker_rect) &&
          ui_expr_handle_event(ui, ev)) {
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ev->button == 4 || ev->button == 5) {
        bool r = ui_nav_step(ui, ev->button == 4 ? -1 : 1);
        uc_update_hover(ui);
        ui->dirty = true;
        return r;
      }
      if ((ev->button == 1 || ev->button == 2 || ev->button == 3) && ui_panels_handle_input(ui, ev)) {
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if ((!mobile_drawer_vis && ui_local_messages_handle_click(ui, ev)) || ui_actbtns_handle_click(ui, ev) ||
          ui_decor_handle_click(ui, ev) || (ev->button == 1 && ui_plugin_full_views_handle_rail_click(ui, ev->x, ev->y))) {
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ev->button == 3) {
        bool r = uc_handle_right_click(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return r || handled;
      }
      if (ev->button == 2) {
        hover_state hm = uc_compute_hover(ui);
        if (hm.rail_slot >= 0) {
          size_t nslots = 0;
          const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
          if (slots && (size_t)hm.rail_slot < nslots) {
            const ui_rail_slot *sl = &slots[hm.rail_slot];
            if (sl->kind == UI_RAIL_DM) {
              ui_post_nav_aux(ui, CCORD_DM_GUILD_ID, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_NAV_VIEW_GUILD);
              uc_update_hover(ui);
              ui->dirty = true;
              return true;
            }
            if (sl->kind == UI_RAIL_GUILD && sl->guild_id != CCORD_ID_NONE) {
              ui_post_nav_aux(ui, sl->guild_id, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_NAV_VIEW_GUILD);
              uc_update_hover(ui);
              ui->dirty = true;
              return true;
            }
          }
        }
        if (hm.crow >= 0 && uc_aux_open_channel_row(ui, (size_t)hm.crow)) {
          uc_update_hover(ui);
          ui->dirty = true;
          return true;
        }
        if (!mobile_drawer_vis && !ui->discovery_open && ui_is_dm_requests_view(ui)) {
          for (size_t i = 0; i < ui->ndm_request_hits; i++) {
            if (ui_in_rect(ev->x, ev->y, ui->dm_request_hits[i].card_rect)) {
              ui_post_nav_aux(ui, CCORD_DM_GUILD_ID, ui->dm_request_hits[i].channel_id, CCORD_ID_NONE,
                              CCORD_NAV_VIEW_CHANNEL);
              uc_update_hover(ui);
              ui->dirty = true;
              return true;
            }
          }
        }
        if (!mobile_drawer_vis && !ui->discovery_open && ui_forum_is_active(ui) && ui_forum_handle_event(ui, ev)) {
          uc_update_hover(ui);
          ui->dirty = true;
          return true;
        }
        if (!mobile_drawer_vis && !ui->discovery_open && ui_in_rect(ev->x, ev->y, ui_rect(ui->layout.message_area))) {
          for (size_t i = 0; i < ui->nchan_hits; i++) {
            if (ui_in_rect(ev->x, ev->y, ui->chan_hits[i].rect)) {
              const ccord_channel *mc = ccord_store_channel(ui->d.store, ui->chan_hits[i].channel_id);
              if (mc && mc->kind == CCORD_CHANNEL_TEXT) {
                bool is_th = ccord_discord_type_is_thread(mc->discord_type);
                ui_post_nav_aux(ui, mc->guild_id, mc->id, is_th ? mc->parent_id : CCORD_ID_NONE,
                                is_th ? CCORD_NAV_VIEW_THREAD : CCORD_NAV_VIEW_CHANNEL);
                uc_update_hover(ui);
                ui->dirty = true;
                return true;
              }
            }
          }
          for (size_t i = 0; i < ui->nthread_hits; i++) {
            if (ui_in_rect(ev->x, ev->y, ui->thread_hits[i].rect)) {
              const ui_thread_hit *th = &ui->thread_hits[i];
              ccord_id tgid = th->guild_id != CCORD_ID_NONE ? th->guild_id : ccord_store_selected_guild(ui->d.store);
              ui_materialize_thread_hit(ui, th, tgid);
              ui_post_nav_aux(ui, tgid, th->thread_id, th->parent_id, CCORD_NAV_VIEW_THREAD);
              uc_update_hover(ui);
              ui->dirty = true;
              return true;
            }
          }
        }
      }
      if (ev->button == 1) {
        bool r = uc_handle_left_down(ui, ev);
        uc_update_hover(ui);
        ui->dirty = true;
        return r;
      }
      return handled;
    }
    case CCORD_IN_MOUSE_UP:
      if (ui->drag == DRAG_STREAM_STAGE_H || ui->drag == DRAG_STAGE_SPLIT) {
        ui->drag = DRAG_NONE;
        ui_settings_save(ui);
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->drag == DRAG_STREAM_PIP_MOVE || ui->drag == DRAG_STREAM_PIP_RESIZE) {
        ui_stream_pip_drag_end(ui);
        uc_update_hover(ui);
        return true;
      }
      if (ui->drag == DRAG_SERVER) {
        if (ui->drag_active) uc_finish_server_drag(ui);
        ui->drag = DRAG_NONE;
        ui->drag_active = false;
        ui->drag_drop_slot = -1;
        ui->drag_drop_zone = UI_DROP_NONE;
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->drag == DRAG_CHANNEL) {
        if (ui->drag_active) uc_finish_channel_drag(ui);
        ui->drag = DRAG_NONE;
        ui->drag_active = false;
        ui->drag_chan_target_id = CCORD_ID_NONE;
        ui->drag_chan_parent_id = CCORD_ID_NONE;
        uc_update_hover(ui);
        ui->dirty = true;
        return true;
      }
      if (ui->drag == DRAG_CHAT_SELECT) {
        if (fabsf(ui->chat_sel_ax - ui->chat_sel_cx) < 2.0f && fabsf(ui->chat_sel_ay - ui->chat_sel_cy) < 2.0f) {
          ui->chat_sel_active = false;
          ui->chat_sel_len = 0;
          ui->chat_sel_buf[0] = '\0';
          ui->dirty = true;
        }
        ui->drag = DRAG_NONE;
        return true;
      }
      if (ui->drag != DRAG_NONE) {
        ui->drag = DRAG_NONE;
        return true;
      }
      if (ui_panels_handle_input(ui, ev)) {
        ui->dirty = true;
        return true;
      }
      return false;
    case CCORD_IN_WHEEL:
      ui->mx = ev->x;
      ui->my = ev->y;
      if (ui->stream_fullscreen) return true;
      if (ui->lightbox_open || ui->quick_switcher_open || ui->modal_kind == UI_MODAL_USER_PROFILE ||
          ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS || ui->modal_kind == UI_MODAL_FORWARD_PICKER ||
          ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION || ui->modal_kind == UI_MODAL_ACTIVITY_SHELF) {
        bool r = ui_overlays_handle(ui, ev);
        uc_update_hover(ui);
        return r;
      }
      if (ui->modal_kind != UI_MODAL_NONE || ui->select_open || ui->shortcuts_open) return false;
      if (ui_server_settings_is_open(ui)) return ui_server_settings_handle(ui, ev);
      if (ui_channel_settings_is_open(ui)) return ui_channel_settings_handle(ui, ev);
      if (ui->menu.open) {
        bool r = ui_menu_handle_mouse(ui, ev);   // an overflowing panel scrolls under the wheel
        uc_update_hover(ui);
        return r;
      }
      if (ui->settings_open) {
        bool r = ui_settings_handle_wheel(ui, ev);   // Voice & Video and the list pages of settings_m6_devices.c
        uc_update_hover(ui);
        return r;
      }
      if (ui->emoji_picker_open && ui_expr_handle_event(ui, ev)) {
        uc_update_hover(ui);
        return true;
      }
      bool mobile_drawer_vis_w = ui->mobile_layout && (ui->layout.left_drawer_visible || ui->layout.right_drawer_visible);
      if (!mobile_drawer_vis_w && ui->discovery_open && ui_discovery_handle(ui, ev)) {
        uc_update_hover(ui);
        return true;
      }
      if (ui_stream_pip_active(ui) && ui->stream_pip_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_pip_rect)) {
        return true;   // wheel over the floating PiP does not scroll the chat underneath
      }
      if (ui_panels_handle_input(ui, ev)) {
        uc_update_hover(ui);
        return true;
      }
      uc_handle_wheel(ui, ev);
      uc_update_hover(ui);
      return true;
    case CCORD_IN_KEY_DOWN:
      return handle_key(ui, ev);
    case CCORD_IN_TEXT: {
      if (ui->stream_fullscreen) return true;
      if (s_suppress_next_text_ui == ui) {
        s_suppress_next_text_ui = nullptr;
        return true;
      }
      ui_ime_preedit_clear(ui);   // the composition was committed (or abandoned) by this text event
      if (ui->lightbox_open || ui->quick_switcher_open || ui->select_open || ui->modal_kind != UI_MODAL_NONE) {
        return ui_overlays_handle(ui, ev);
      }
      if (ui_server_settings_is_open(ui)) return ui_server_settings_handle(ui, ev);
      if (ui_channel_settings_is_open(ui)) return ui_channel_settings_handle(ui, ev);
      if (ui->settings_open && (ui->account_focus_field != 0 || ui->account_mfa_required)) {
        return ui_settings_handle(ui, ev);
      }
      if (ui->menu.open || ui->settings_open || ui->go_live_open || ui->pins_open || ui->reactions_open ||
          ui->inbox_open || ui->shortcuts_open) {
        return false;
      }
      if (ui->emoji_picker_open) {
        return ui_expr_handle_event(ui, ev);
      }
      if (ui_panels_has_focused_input(ui) && ui_panels_handle_input(ui, ev)) {
        return true;
      }
      if (ui->discovery_open) {
        if (ui_discovery_handle(ui, ev)) {
          uc_reset_caret_blink(ui);
          return true;
        }
        return false;
      }
      size_t n = strnlen(ev->text, sizeof ev->text);
      ccord_error err;
      if (ui->search_open) {
        if (ccord_text_edit_insert(&ui->search_edit, ev->text, n, &err) != CCORD_OK) ui_notice(ui, "%s", err.message);
        uc_search_schedule(ui);
        ui->view_key = 0;
        ui_compute_layout(ui);
        ui_sync_messages(ui);
        ui->dirty = true;
        return true;
      }
      if (ui_forum_is_active(ui)) {
        if (ui_forum_handle_event(ui, ev)) {
          uc_reset_caret_blink(ui);
          return true;
        }
        return false;
      }
      if (!uc_composer_accepts_text(ui)) return false;
      if (ui_composer_lock_reason(ui)) return true;   // the box is shut: typing does nothing (and sends no typing notice)
      if (ccord_text_edit_insert(&ui->edit, ev->text, n, &err) != CCORD_OK) {
        ui_notice(ui, "%s", err.message);
      } else if (n > 0) {
        ui_send_typing(ui);
        ui_notify_composer_changed(ui);
      }
      ui_update_autocomplete(ui);
      uc_reset_caret_blink(ui);
      return true;
    }
    case CCORD_IN_NONE:
    case CCORD_IN_TOUCH_DOWN:
    case CCORD_IN_TOUCH_MOVE:
    case CCORD_IN_TOUCH_UP:
    case CCORD_IN_TOUCH_CANCEL:
    case CCORD_IN_APP_BACKGROUND:
    case CCORD_IN_APP_FOREGROUND:
    case CCORD_IN_APP_LOW_MEMORY:
    case CCORD_IN_RENDER_RESET:
    case CCORD_IN_SAFE_AREA:
    case CCORD_IN_KEYBOARD_INSET:
      return false;
  }
  return false;
}

