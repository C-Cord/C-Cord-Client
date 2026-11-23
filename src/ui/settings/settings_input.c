// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "settings_internal.h"

static bool handle_page_controls(ccord_ui *ui, const sgeom *g, float x, float y)
{
  switch (ui->settings_page) {
    case SP_PROFILE_EDIT:
      return ui_settings_profile_click(ui, g->content, g->panel, x, y);
    case SP_APPEARANCE:
      if (ui->account_focus_field == 6 && !ui_in_rect(x, y, st_quickcss_box_rect(g))) {
        ui->account_focus_field = 0;   // clicking away commits the snippet editor
        st_settings_save_flush(ui);
        ui->dirty = true;
      }
      if (ui_settings_m6_appearance_click(ui, g->content, x, y)) return true;   // theme chips + zoom
      for (int i = 0; i < 2; i++) {
        if (ui_in_rect(x, y, st_chips_rect(g, 0, i, 2))) {
          ui->settings_display_mode = i;
          ui->dirty = true;
          return true;
        }
      }
      if (ui_in_rect(x, y, st_toggle_rect(g, 5))) {
        ui->settings_quickcss_enabled = !ui->settings_quickcss_enabled;
        ui_quickcss_changed(ui);   // live apply through the host + persist
        st_settings_save_flush(ui);
        return true;
      }
      if (ui_in_rect(x, y, st_quickcss_box_rect(g))) {
        ui->account_focus_field = 6;   // sentinel: routes key/text events to the snippet editor
        ccord_text_edit_set_caret(&ui->quickcss_edit, ui->quickcss_edit.len, false);
        if (ui->touch_mode) ui->touch_text_focused = true;
        ui->dirty = true;
        return false;   // focusing the box does not modify any persisted setting
      }
      if (ui_in_rect(x, y, st_toggle_rect(g, 7))) {
        ui->settings_reduced_motion = !ui->settings_reduced_motion;
        ui->anim.reduced_motion = ui->settings_reduced_motion;
        ui->dirty = true;
        return true;
      }
      break;
    case SP_VOICE: {
      // g arrives shifted by -st_s_voice_scroll. The fixed scrollbar lives in the right margin
      // (outside the content width), so answer it before clipping clicks to the content box.
      sgeom gu = *g;
      gu.content.y += st_s_voice_scroll;   // unshifted st_geometry for the fixed scrollbar
      if (st_voice_scroll_click(ui, &gu, x, y)) return false;   // scrolling does not change settings.ini
      if (!ui_in_rect(x, y, gu.content)) break;   // rows only answer inside the visible content box
      // Rows 0-8 keep the original Voice & Video layout; the M5 additions (camera, PTT key,
      // sensitivity, diagnostics) follow at rows 9-12 and are reached by scrolling.
      if (ui_in_rect(x, y, st_device_rect(g, 0))) {
        st_voice_devices_refresh(ui, true);
        st_voice_select_input(ui, (ui->settings_mic_device + 1) % st_voice_in_count());
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, st_device_rect(g, 1))) {
        st_voice_devices_refresh(ui, true);
        st_voice_select_output(ui, (ui->settings_speaker_device + 1) % st_voice_out_count());
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, st_vol_minus_rect(g, 2))) {
        ui->settings_input_volume = ui->settings_input_volume > 5 ? ui->settings_input_volume - 5 : 0;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, st_vol_plus_rect(g, 2))) {
        ui->settings_input_volume = ui->settings_input_volume < 95 ? ui->settings_input_volume + 5 : 100;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, st_vol_track_rect(g, 2))) {
        ccord_rect tr = st_vol_track_rect(g, 2);
        ui->settings_input_volume = st_clamp_volume(&tr, x);
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, st_vol_minus_rect(g, 3))) {
        ui->settings_output_volume = ui->settings_output_volume > 5 ? ui->settings_output_volume - 5 : 0;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, st_vol_plus_rect(g, 3))) {
        ui->settings_output_volume = ui->settings_output_volume < 95 ? ui->settings_output_volume + 5 : 100;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, st_vol_track_rect(g, 3))) {
        ccord_rect tr = st_vol_track_rect(g, 3);
        ui->settings_output_volume = st_clamp_volume(&tr, x);
        ui->dirty = true;
        return true;
      }
      for (int i = 0; i < 2; i++) {
        if (ui_in_rect(x, y, st_chips_rect(g, 4, i, 2))) {
          ui->settings_voice_mode = i;
          st_voice_config_apply(ui);
          return true;
        }
      }
      bool dsp = ccord_dsp_available();
      if (dsp && ui_in_rect(x, y, st_toggle_rect(g, 5))) {
        ui->settings_noise_suppression = !ui->settings_noise_suppression;
        st_voice_config_apply(ui);
        return true;
      }
      if (dsp && ui_in_rect(x, y, st_toggle_rect(g, 6))) {
        ui->settings_echo_cancel = !ui->settings_echo_cancel;
        st_voice_config_apply(ui);
        return true;
      }
      if (dsp && ui_in_rect(x, y, st_toggle_rect(g, 7))) {
        ui->settings_auto_gain = !ui->settings_auto_gain;
        st_voice_config_apply(ui);
        return true;
      }
      if (ui_in_rect(x, y, st_toggle_rect(g, 8))) {
        ui->settings_hardware_accel = !ui->settings_hardware_accel;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, st_device_rect(g, 9))) {
        st_voice_devices_refresh(ui, true);
        if (st_s_dev.n_cam > 0) {
          st_voice_select_camera(ui, (ui->settings_camera_device + 1) % st_voice_cam_count());
          ui->dirty = true;
          return true;
        }
        return false;
      }
      if (ui_in_rect(x, y, st_device_rect(g, 10))) {   // Push to Talk key: arm live capture
        st_s_ptt_armed = true;
        ui->account_focus_field = 5;   // sentinel: routes key events to this modal (ui.c handle_key)
        ui->dirty = true;
        return false;
      }
      if (ui->settings_voice_mode == 0) {   // sensitivity only applies to voice activity
        if (ui_in_rect(x, y, st_vol_minus_rect(g, 11))) {
          ui->settings_vad_sensitivity = ui->settings_vad_sensitivity > 5 ? ui->settings_vad_sensitivity - 5 : 0;
          st_voice_config_apply(ui);
          return true;
        }
        if (ui_in_rect(x, y, st_vol_plus_rect(g, 11))) {
          ui->settings_vad_sensitivity = ui->settings_vad_sensitivity < 95 ? ui->settings_vad_sensitivity + 5 : 100;
          st_voice_config_apply(ui);
          return true;
        }
        if (ui_in_rect(x, y, st_vol_track_rect(g, 11))) {
          ccord_rect tr = st_vol_track_rect(g, 11);
          ui->settings_vad_sensitivity = st_clamp_volume(&tr, x);
          st_voice_config_apply(ui);
          return true;
        }
      }
      break;
    }
    case SP_KEYBINDS: {
      // The Push to Talk row jumps to Voice & Video and arms the same live capture.
      ccord_rect b = st_keybind_rect(g, 0);
      if (ui_in_rect(x, y, b)) {
        ui->settings_page = SP_VOICE;
        ui->settings_category = st_k_pages[SP_VOICE].category;
        st_s_ptt_armed = true;
        ui->account_focus_field = 5;
        ui->dirty = true;
        return true;
      }
      // Mute / Deafen / Quick Switcher rows arm an in-place capture (rows 1..3).
      for (int row = 1; row <= 3; row++) {
        ccord_rect kb = st_keybind_rect(g, row);
        if (ui_in_rect(x, y, kb)) {
          st_s_kb_armed = row - 1;
          st_s_plugin_kb_armed = -1;
          ui->account_focus_field = 5;   // sentinel: routes key events to this modal (ui.c handle_key)
          ui->dirty = true;
          return true;
        }
      }
      if (ui->d.contrib) {
        size_t npk = ccord_contrib_keybind_count(ui->d.contrib);
        for (size_t i = 0; i < npk && i < 8u; i++) {
          ccord_rect kb = st_keybind_rect(g, 6 + (int)i);
          if (ui_in_rect(x, y, kb)) {
            st_s_plugin_kb_armed = (int)i;
            st_s_kb_armed = -1;
            ui->account_focus_field = 5;
            ui->dirty = true;
            return true;
          }
        }
      }
      break;
    }
    case SP_STREAMING: {
      ccord_rect auto_enc = st_stream_auto_pill_rect(g, 0);
      ccord_rect auto_fw = st_stream_auto_pill_rect(g, 1);
      if (ui_in_rect(x, y, auto_enc)) {
        ui->settings_encoder = (int)CCORD_STREAM_ENCODER_AUTO;
        ui->gl_encoder = (int)CCORD_STREAM_ENCODER_AUTO;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, auto_fw)) {
        ui->settings_framework = (int)CCORD_STREAM_FRAMEWORK_AUTO;
        ui->gl_framework = (int)CCORD_STREAM_FRAMEWORK_AUTO;
        ui->dirty = true;
        return true;
      }
      for (int i = 0; i < STREAM_ENC_COUNT; i++) {
        if (ui_in_rect(x, y, st_stream_chip_rect(g, 0, i, STREAM_ENC_COUNT))) {
          ui->settings_encoder = st_k_encoder_vals[i];
          ui->gl_encoder = st_k_encoder_vals[i];   // Go Live starts from these defaults
          ui->dirty = true;
          return true;
        }
      }
      for (int i = 0; i < STREAM_FW_COUNT; i++) {
        if (ui_in_rect(x, y, st_stream_chip_rect(g, 1, i, STREAM_FW_COUNT))) {
          ui->settings_framework = st_k_framework_vals[i];
          ui->gl_framework = st_k_framework_vals[i];
          ui->dirty = true;
          return true;
        }
      }
      for (int i = 0; i < 4; i++) {
        if (ui_in_rect(x, y, st_stream_chip_rect(g, 2, i, 4))) {
          ui->settings_resolution = i;
          ui->gl_resolution = i;
          ui->dirty = true;
          return true;
        }
      }
      for (int i = 0; i < 3; i++) {
        if (ui_in_rect(x, y, st_stream_chip_rect(g, 3, i, 3))) {
          ui->settings_fps = i;
          ui->gl_fps = i;
          ui->dirty = true;
          return true;
        }
      }
      for (int i = 0; i < 2; i++) {
        if (ui_in_rect(x, y, st_stream_chip_rect(g, 4, i, 2))) {
          ui->settings_codec = i;
          ui->gl_codec = i;
          ui->dirty = true;
          return true;
        }
      }
      break;
    }
    case SP_TEXT_IMAGES:
      if (ui_in_rect(x, y, st_toggle_rect(g, 0))) {
        ui->settings_embeds = !ui->settings_embeds;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, st_toggle_rect(g, 1))) {
        ui->settings_attachments = !ui->settings_attachments;
        ui->dirty = true;
        return true;
      }
      break;
    case SP_NOTIFICATIONS:
      // M6 rework: body, capability gating and the /tts PATCH live in settings_m6_prefs.c.
      return ui_settings_m6_prefs_click(ui, CCORD_M6_PREFS_NOTIFICATIONS, g->content, g->panel, x, y);
    case SP_ACCESSIBILITY:
      return ui_settings_m6_prefs_click(ui, CCORD_M6_PREFS_ACCESSIBILITY, g->content, g->panel, x, y);
    case SP_STREAMER:
      return ui_settings_m6_prefs_click(ui, CCORD_M6_PREFS_STREAMER, g->content, g->panel, x, y);
    case SP_LANGUAGE:
      return ui_settings_m6_prefs_click(ui, CCORD_M6_PREFS_LANGUAGE, g->content, g->panel, x, y);
    case SP_ADVANCED:
      return ui_settings_m6_prefs_click(ui, CCORD_M6_PREFS_ADVANCED, g->content, g->panel, x, y);
    case SP_CCORD:
      return ui_settings_m6_prefs_click(ui, CCORD_M6_PREFS_CCORD, g->content, g->panel, x, y);
    case SP_PRIVACY:
    case SP_DEVICES:
    case SP_CONNECTIONS:
      // M6 account pages: bodies, scrolling and confirmations live in settings_m6_devices.c.
      return ui_settings_m6_click(ui, ui->settings_page - SP_PRIVACY, g->content, g->panel, x, y);
    case SP_PLUGINS:
      return st_plugins_handle_click(ui, g, x, y);
    default:
      break;
  }
  return false;
}

// Shared keyboard editing for single-line login/account fields (supports Copy/Cut/Paste/Select All/arrows).
static int64_t settings_hover_key(const ccord_ui *ui, const sgeom *g, float x, float y)
{
  if (g->back.w > 0.0f && ui_in_rect(x, y, g->back)) return 1;
  if (g->sb_up.w > 0.0f) {
    if (ui_in_rect(x, y, g->sb_up)) return 2;
    if (ui_in_rect(x, y, g->sb_down)) return 3;
  }
  if (ui->mobile_layout && !ui->mobile_settings_detail) return 0;

  switch (ui->settings_page) {
    case SP_ACCOUNT: {
      sgeom ga = *g;
      if (st_page_max_scroll(ui, g) > 0.0f) ga.content.y -= st_s_page_scroll;
      for (int i = 0; i < 5; i++) {
        if (ui_in_rect(x, y, st_account_web_btn_rect(ui, &ga, i))) return 100 + i;
      }
      if (!ui->account_view.signed_in) {
        if (ui_in_rect(x, y, st_account_email_box_rect(&ga))) return 110;
        if (ui_in_rect(x, y, st_account_password_box_rect(&ga))) return 111;
        if (ui_in_rect(x, y, st_account_login_btn_rect(&ga))) return 112;
      }
      if (ui_in_rect(x, y, st_account_token_box_rect(&ga))) return 113;
      if (ui_in_rect(x, y, st_account_token_paste_rect(&ga))) return 114;
      if (ui_in_rect(x, y, st_account_token_save_rect(&ga))) return 115;
      if (ui_in_rect(x, y, st_account_token_clear_rect(&ga))) return 116;
      if (ui_in_rect(x, y, st_account_add_btn_rect(&ga))) return 117;
      for (size_t i = 0; i < ui->saved_account_count && i < SAVED_ACCOUNT_ROWS; i++) {
        if (ui_in_rect(x, y, st_saved_account_switch_rect(&ga, (int)i))) return 120 + (int64_t)i * 2;
        if (ui_in_rect(x, y, st_saved_account_remove_rect(&ga, (int)i))) return 120 + (int64_t)i * 2 + 1;
      }
      ccord_rect primary, secondary;
      st_account_buttons(ui, &ga, &primary, &secondary);
      if (ui_in_rect(x, y, primary)) return 140;
      if (ui_in_rect(x, y, secondary)) return 141;
      return 0;
    }
    case SP_APPEARANCE: {
      sgeom ga = *g;
      if (st_page_max_scroll(ui, g) > 0.0f) ga.content.y -= st_s_page_scroll;
      for (int i = 0; i < 2; i++) {
        if (ui_in_rect(x, y, st_chips_rect(&ga, 0, i, 2))) return 200 + i;
      }
      for (int i = 0; i < 3; i++) {
        if (ui_in_rect(x, y, st_chips_rect(&ga, 1, i, 3))) return 210 + i;
      }
      if (ui_in_rect(x, y, st_vol_minus_rect(&ga, 2))) return 220;
      if (ui_in_rect(x, y, st_vol_plus_rect(&ga, 2))) return 221;
      if (ui_in_rect(x, y, st_vol_track_rect(&ga, 2))) return 222;
      if (ui_in_rect(x, y, ui_settings_m6_zoom_reset_rect(ga.content))) return 223;
      if (ui_in_rect(x, y, st_toggle_rect(&ga, 5))) return 225;
      if (ui_in_rect(x, y, st_quickcss_box_rect(&ga))) return 226;
      if (ui_in_rect(x, y, st_toggle_rect(&ga, 7))) return 227;
      return 0;
    }
    case SP_VOICE: {
      if (st_voice_max_scroll(g) > 0.0f) {
        if (ui_in_rect(x, y, st_voice_scroll_up_rect(g))) return 300;
        if (ui_in_rect(x, y, st_voice_scroll_down_rect(g))) return 301;
        if (ui_in_rect(x, y, st_voice_scroll_track(g))) return 302;
      }
      if (!ui_in_rect(x, y, g->content)) return 0;
      sgeom gv = *g;
      gv.content.y -= st_s_voice_scroll;
      if (ui_in_rect(x, y, st_device_rect(&gv, 0))) return 310;
      if (ui_in_rect(x, y, st_device_rect(&gv, 1))) return 311;
      if (ui_in_rect(x, y, st_vol_minus_rect(&gv, 2))) return 312;
      if (ui_in_rect(x, y, st_vol_plus_rect(&gv, 2))) return 313;
      if (ui_in_rect(x, y, st_vol_track_rect(&gv, 2))) return 314;
      if (ui_in_rect(x, y, st_vol_minus_rect(&gv, 3))) return 315;
      if (ui_in_rect(x, y, st_vol_plus_rect(&gv, 3))) return 316;
      if (ui_in_rect(x, y, st_vol_track_rect(&gv, 3))) return 317;
      for (int i = 0; i < 2; i++) {
        if (ui_in_rect(x, y, st_chips_rect(&gv, 4, i, 2))) return 320 + i;
      }
      for (int r = 5; r <= 8; r++) {
        if (ui_in_rect(x, y, st_toggle_rect(&gv, r))) return 320 + r;
      }
      if (ui_in_rect(x, y, st_device_rect(&gv, 9))) return 329;
      if (ui_in_rect(x, y, st_device_rect(&gv, 10))) return 330;
      if (ui_in_rect(x, y, st_vol_minus_rect(&gv, 11))) return 331;
      if (ui_in_rect(x, y, st_vol_plus_rect(&gv, 11))) return 332;
      if (ui_in_rect(x, y, st_vol_track_rect(&gv, 11))) return 333;
      return 0;
    }
    case SP_STREAMING: {
      if (ui_in_rect(x, y, st_auto_pill_rect(g, 0))) return 400;
      if (ui_in_rect(x, y, st_auto_pill_rect(g, 1))) return 401;
      for (int i = 0; i < STREAM_ENC_COUNT; i++) {
        if (ui_in_rect(x, y, st_chips_rect(g, 0, i, STREAM_ENC_COUNT))) return 410 + i;
      }
      for (int i = 0; i < STREAM_FW_COUNT; i++) {
        if (ui_in_rect(x, y, st_chips_rect(g, 1, i, STREAM_FW_COUNT))) return 420 + i;
      }
      for (int i = 0; i < 4; i++) {
        if (ui_in_rect(x, y, st_chips_rect(g, 2, i, 4))) return 430 + i;
      }
      for (int i = 0; i < 3; i++) {
        if (ui_in_rect(x, y, st_chips_rect(g, 3, i, 3))) return 440 + i;
      }
      for (int i = 0; i < 2; i++) {
        if (ui_in_rect(x, y, st_chips_rect(g, 4, i, 2))) return 450 + i;
      }
      return 0;
    }
    case SP_TEXT_IMAGES: {
      if (ui_in_rect(x, y, st_toggle_rect(g, 0))) return 500;
      if (ui_in_rect(x, y, st_toggle_rect(g, 1))) return 501;
      return 0;
    }
    case SP_KEYBINDS: {
      for (int r = 0; r < 4; r++) {
        if (ui_in_rect(x, y, st_keybind_rect(g, r))) return 550 + r;
      }
      return 0;
    }
    case SP_PLUGINS:
      return st_plugins_hover_key(ui, g, x, y);
    default: {
      // External M6 preference / device / profile pages: quantize hover cells inside content so
      // 1px pointer jitter in the same control or whitespace does not trigger redraws.
      if (!ui_in_rect(x, y, g->content)) return 0;
      int row_cell = (int)((y - g->content.y) / 26.0f);
      int col_cell = (int)((x - g->content.x) / 44.0f);
      return 1000 + (int64_t)ui->settings_page * 4096 + (int64_t)row_cell * 64 + (int64_t)col_cell;
    }
  }
}

bool ui_settings_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->settings_open) return false;
  st_settings_init_defaults(ui);
  st_settings_fix_page(ui);
  // A capture armed on the Voice page must not leak into other pages (e.g. after Esc closed the
  // modal and it reopened on My Account): disarm as soon as the Voice page is not current.
  if (st_s_ptt_armed && ui->settings_page != SP_VOICE) {
    st_s_ptt_armed = false;
    if (ui->account_focus_field == 5) ui->account_focus_field = 0;
    ui->dirty = true;
  }
  if (st_s_kb_armed >= 0 && ui->settings_page != SP_KEYBINDS) {
    st_s_kb_armed = -1;
    if (ui->account_focus_field == 5) ui->account_focus_field = 0;
    ui->dirty = true;
  }
  if (ui->account_mfa_required && (ui_is_login_screen(ui) || ui->settings_page == SP_ACCOUNT)) {
    return st_handle_mfa_popup(ui, ev);
  }
  if (ui_is_login_screen(ui)) {
    ccord_rect card = st_login_card_rect(ui);
    if (ev->type == CCORD_IN_MOUSE_MOVE) {
      int64_t hk = 800;
      if (ui_in_rect(ev->x, ev->y, st_login_tab_rect(&card, 0))) hk = 801;
      else if (ui_in_rect(ev->x, ev->y, st_login_tab_rect(&card, 1))) hk = 802;
      else if (ui->login_tab == 0) {
        if (ui_in_rect(ev->x, ev->y, st_login_token_box_rect(&card))) hk = 803;
        else if (ui_in_rect(ev->x, ev->y, st_login_token_paste_rect(&card))) hk = 804;
        else if (ui_in_rect(ev->x, ev->y, st_login_token_save_rect(&card))) hk = 805;
      } else {
        if (ui_in_rect(ev->x, ev->y, st_login_email_box_rect(&card))) hk = 806;
        else if (ui_in_rect(ev->x, ev->y, st_login_password_box_rect(&card))) hk = 807;
        else if (ui_in_rect(ev->x, ev->y, st_login_credentials_btn_rect(&card))) hk = 808;
      }
      bool changed = ui->hover_settings_page != -1 || ui->hover_settings_close || hk != st_s_hover_key;
      ui->hover_settings_page = -1;
      ui->hover_settings_close = false;
      st_s_hover_key = hk;
      if (changed) ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 3) {
      if (ui->login_tab == 0) {
        ccord_rect tbox = st_login_token_box_rect(&card);
        if (ui_in_rect(ev->x, ev->y, tbox)) {
          ui->account_focus_field = 1;
          ui->text_area = tbox;
          ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_token_edit, true, false, false, false);
          return true;
        }
      } else {
        ccord_rect em_box = st_login_email_box_rect(&card);
        ccord_rect pw_box = st_login_password_box_rect(&card);
        if (ui_in_rect(ev->x, ev->y, em_box)) {
          ui->account_focus_field = 2;
          ui->text_area = em_box;
          ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_email_edit, false, false, false, false);
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, pw_box)) {
          ui->account_focus_field = 3;
          ui->text_area = pw_box;
          ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_password_edit, true, false, false, false);
          return true;
        }
      }
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
      if (!ui_in_rect(ev->x, ev->y, card)) {
        // Unauthenticated Login Screen is non-dismissable by outside clicks.
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, st_login_tab_rect(&card, 0))) {
        ui->login_tab = 0;
        ui->account_focus_field = 1;
        ui->touch_text_focused = false;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, st_login_tab_rect(&card, 1))) {
        ui->login_tab = 1;
        ui->account_focus_field = 2;
        ui->touch_text_focused = false;
        ui->dirty = true;
        return true;
      }
      if (ui->login_tab == 0) {
        ccord_rect tbox = st_login_token_box_rect(&card);
        if (ui_in_rect(ev->x, ev->y, tbox)) {
          ui->account_focus_field = 1;
          ui->touch_text_focused = true;
          ui->text_area = tbox;
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, st_login_token_paste_rect(&card))) {
          ui->account_focus_field = 1;
          ui->touch_text_focused = false;
          if (ui->d.host.clipboard_get) {
            char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
            if (clip) {
              ccord_error ierr;
              ccord_text_edit_clear(&ui->account_token_edit);
              (void)ccord_text_edit_insert(&ui->account_token_edit, clip, strlen(clip), &ierr);
              ccord_free(clip);
            }
          }
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, st_login_token_save_rect(&card))) {
          ui->touch_text_focused = false;
          if (ui->d.host.account_action) {
            ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN);
          }
          ui->dirty = true;
          return true;
        }
      } else {
        ccord_rect em_box = st_login_email_box_rect(&card);
        ccord_rect pw_box = st_login_password_box_rect(&card);
        if (ui_in_rect(ev->x, ev->y, em_box)) {
          ui->account_focus_field = 2;
          ui->touch_text_focused = true;
          ui->text_area = em_box;
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, pw_box)) {
          ui->account_focus_field = 3;
          ui->touch_text_focused = true;
          ui->text_area = pw_box;
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, st_login_credentials_btn_rect(&card))) {
          ui->touch_text_focused = false;
          if (ui->d.host.account_action) {
            ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN);
          }
          ui->dirty = true;
          return true;
        }
      }
      ui->touch_text_focused = false;
      return true;
    }
    if (ui->account_focus_field == 0) {
      ui->account_focus_field = (ui->login_tab == 0) ? 1 : 2;
    }
    ccord_text_edit *active = nullptr;
    bool masked = false;
    if (ui->account_focus_field == 1) { active = &ui->account_token_edit; masked = true; }
    else if (ui->account_focus_field == 2) { active = &ui->account_email_edit; }
    else if (ui->account_focus_field == 3) { active = &ui->account_password_edit; masked = true; }
    if (active) {
      if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
        ccord_error ierr;
        (void)ccord_text_edit_insert(active, ev->text, strlen(ev->text), &ierr);
        ui->dirty = true;
        return true;
      }
      if (ev->type == CCORD_IN_KEY_DOWN) {
        if (ev->key == CCORD_KEY_TAB) {
          if (ui->login_tab == 0) {
            ui->account_focus_field = 1;
          } else {
            ui->account_focus_field = (ui->account_focus_field == 2) ? 3 : 2;
          }
          ui->dirty = true;
          return true;
        }
        if (ev->key == CCORD_KEY_RETURN) {
          if (ui->d.host.account_action) {
            ccord_account_action act = (ui->login_tab == 0 || ui->account_focus_field == 1)
                                         ? CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN
                                         : CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN;
            ui->d.host.account_action(ui->d.host.ud, act);
          }
          ui->dirty = true;
          return true;
        }
        if (st_handle_account_field_edit_key(ui, active, masked, ev)) return true;
      }
    }
    return true;
  }
  sgeom g = st_geometry(ui);
  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    if (ui->settings_page == SP_PROFILE_EDIT && ui_settings_profile_has_overlay(ui)) {
      if (ui_settings_profile_mouse_move(ui, ev->x, ev->y)) return true;
    }
    int page = -1;
    for (int i = 0; i < SP_COUNT; i++) {
      if (ui_in_rect(ev->x, ev->y, g.page[i]) && ui_in_rect(ev->x, ev->y, g.list)) page = i;
    }
    bool close = ui_in_rect(ev->x, ev->y, g.close);
    int64_t hk = ui_in_rect(ev->x, ev->y, g.panel) ? settings_hover_key(ui, &g, ev->x, ev->y) : -1;
    bool changed = page != ui->hover_settings_page || close != ui->hover_settings_close || hk != st_s_hover_key;
    ui->hover_settings_page = page;
    ui->hover_settings_close = close;
    st_s_hover_key = hk;
    if (changed) ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_UP) {
    if (ui->settings_page == SP_PROFILE_EDIT && ui_settings_profile_has_overlay(ui)) {
      ui_settings_profile_mouse_up(ui);
      return true;
    }
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 3) {
    if (ui->settings_page == SP_ACCOUNT && (!ui->mobile_layout || ui->mobile_settings_detail)) {
      if (!ui->account_view.signed_in) {
        ccord_rect em_box = st_account_email_box_rect(&g);
        ccord_rect pw_box = st_account_password_box_rect(&g);
        if (ui_in_rect(ev->x, ev->y, em_box)) {
          ui->account_focus_field = 2;
          ui->text_area = em_box;
          ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_email_edit, false, false, false, false);
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, pw_box)) {
          ui->account_focus_field = 3;
          ui->text_area = pw_box;
          ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_password_edit, true, false, false, false);
          return true;
        }
      }
      ccord_rect tbox = st_account_token_box_rect(&g);
      if (ui_in_rect(ev->x, ev->y, tbox)) {
        ui->account_focus_field = 1;
        ui->text_area = tbox;
        ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_token_edit, true, false, false, false);
        return true;
      }
    } else if (ui->settings_page == SP_APPEARANCE && (!ui->mobile_layout || ui->mobile_settings_detail)) {
      ccord_rect qbox = st_quickcss_box_rect(&g);
      if (ui_in_rect(ev->x, ev->y, qbox)) {
        ui->account_focus_field = 6;
        ui->text_area = qbox;
        ui_menu_open_text_field(ui, ev->x, ev->y, &ui->quickcss_edit, false, false, true, false);
        return true;
      }
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    st_s_press_x = ev->x;
    st_s_press_y = ev->y;
    st_s_press_until_ms = ui->now_ms + SETTINGS_PRESS_MS;
    ui->dirty = true;
    if (ui->settings_page == SP_PLUGINS && (!ui->mobile_layout || ui->mobile_settings_detail) &&
        st_plugins_has_overlay()) {
      (void)st_plugins_handle_click(ui, &g, ev->x, ev->y);
      st_s_hover_key = -1;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, g.close) || !ui_in_rect(ev->x, ev->y, g.panel)) {
      st_settings_save_flush(ui);
      ui_open_settings(ui, false);
      return true;
    }
    if (g.back.w > 0.0f && ui_in_rect(ev->x, ev->y, g.back)) {
      if (ui->settings_page == SP_NOTIFICATIONS && ui->settings_sounds_open) {
        ui->settings_sounds_open = false;
        ui->dirty = true;
        return true;
      }
      if (ui->settings_page == SP_PLUGINS && st_plugins_handle_back(ui)) {
        return true;
      }
      ui->mobile_settings_detail = false;
      ui->account_focus_field = 0;
      if (ui->touch_mode) ui->touch_text_focused = false;
      st_s_page_scroll = 0.0f;
      ui->dirty = true;
      return true;
    }
    if (st_s_ptt_armed || st_s_kb_armed >= 0) {
      // Any click cancels an armed keybind capture; clicking the row again re-arms it.
      st_s_ptt_armed = false;
      st_s_kb_armed = -1;
      ui->account_focus_field = 0;
      ui->dirty = true;
      return true;
    }
    if (g.sb_up.w > 0.0f) {
      if (ui_in_rect(ev->x, ev->y, g.sb_up)) {
        st_s_sidebar_scroll -= g.step;   // st_geometry() clamps the offset to the list
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, g.sb_down)) {
        st_s_sidebar_scroll += g.step;
        ui->dirty = true;
        return true;
      }
    }
    for (int i = 0; i < SP_COUNT; i++) {
      if (ui_in_rect(ev->x, ev->y, g.page[i]) && ui_in_rect(ev->x, ev->y, g.list)) {
        if (i == SP_VOICE && ui->settings_page != SP_VOICE) st_s_dev.valid = false;
        ui->settings_page = i;
        ui->settings_category = st_k_pages[i].category;
        st_s_hover_key = -1;
        if (ui->mobile_layout) ui->mobile_settings_detail = true;
        ui->dirty = true;
        return true;
      }
    }
    if (ui->mobile_layout && !ui->mobile_settings_detail) {
      return true;   // Level 1 categories screen: page content is not visible
    }
    if (ui->settings_page == SP_ACCOUNT) {
      sgeom ga = g;
      float max_a = st_page_max_scroll(ui, &g);
      if (max_a > 0.0f) {
        if (st_s_page_scroll > max_a) st_s_page_scroll = max_a;
        ga.content.y -= st_s_page_scroll;
      }
      for (int i = 0; i < 5; i++) {
        if (ui_in_rect(ev->x, ev->y, st_account_web_btn_rect(ui, &ga, i))) {
          ui_open_url(ui, st_k_web_urls[i], st_k_web_labels[i]);
          ui->dirty = true;
          return true;
        }
      }
      if (!ui->account_view.signed_in) {
        ccord_rect em_box = st_account_email_box_rect(&ga);
        ccord_rect pw_box = st_account_password_box_rect(&ga);
        if (ui_in_rect(ev->x, ev->y, em_box)) {
          ui->account_focus_field = 2;
          ui->touch_text_focused = true;
          ui->text_area = em_box;
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, pw_box)) {
          ui->account_focus_field = 3;
          ui->touch_text_focused = true;
          ui->text_area = pw_box;
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, st_account_login_btn_rect(&ga))) {
          ui->account_focus_field = 0;
          ui->touch_text_focused = false;
          if (ui->d.host.account_action) {
            ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN);
          }
          ui->dirty = true;
          return true;
        }
      }
      {
        ccord_rect tbox = st_account_token_box_rect(&ga);
        if (ui_in_rect(ev->x, ev->y, tbox)) {
          ui->account_focus_field = 1;
          ui->touch_text_focused = true;
          ui->text_area = tbox;
          ui->dirty = true;
          return true;
        }
      }
      ui->account_focus_field = 0;
      ui->touch_text_focused = false;
      if (ui_in_rect(ev->x, ev->y, st_account_token_paste_rect(&ga))) {
        if (ui->d.host.clipboard_get) {
          char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
          if (clip) {
            ccord_error ierr;
            ccord_text_edit_clear(&ui->account_token_edit);
            (void)ccord_text_edit_insert(&ui->account_token_edit, clip, strlen(clip), &ierr);
            ccord_free(clip);
          }
        }
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, st_account_token_save_rect(&ga))) {
        if (ui->d.host.account_action) {
          ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN);
        }
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, st_account_token_clear_rect(&ga))) {
        ccord_text_edit_clear(&ui->account_token_edit);
        if (ui->d.host.account_action) {
          ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_TOKEN_SIGN_OUT);
        }
        ui->dirty = true;
        return true;
      }
      // Multi-account switcher: Add focuses the token box for a new sign-in; Switch/Remove go to
      // the app (keyring + session/store), which pushes the updated roster back.
      if (ui_in_rect(ev->x, ev->y, st_account_add_btn_rect(&ga))) {
        ccord_text_edit_clear(&ui->account_token_edit);
        ui->account_focus_field = 1;
        if (ui->touch_mode) ui->touch_text_focused = true;
        ui->dirty = true;
        return true;
      }
      for (size_t i = 0; i < ui->saved_account_count && i < SAVED_ACCOUNT_ROWS; i++) {
        if (!ui->saved_accounts[i].active && ui_in_rect(ev->x, ev->y, st_saved_account_switch_rect(&ga, (int)i))) {
          if (ui->d.host.account_switch) ui->d.host.account_switch(ui->d.host.ud, ui->saved_accounts[i].user_id);
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, st_saved_account_remove_rect(&ga, (int)i))) {
          if (ui->d.host.account_remove) ui->d.host.account_remove(ui->d.host.ud, ui->saved_accounts[i].user_id);
          ui->dirty = true;
          return true;
        }
      }
      ccord_rect primary, secondary;
      st_account_buttons(ui, &ga, &primary, &secondary);
      const ccord_account_view *v = &ui->account_view;
      ccord_account_action action;
      bool have = false;
      if (v->signed_in && ui_in_rect(ev->x, ev->y, primary)) {
        action = CCORD_ACCOUNT_ACTION_REFRESH;
        have = true;
      } else if (v->signed_in && ui_in_rect(ev->x, ev->y, secondary)) {
        action = CCORD_ACCOUNT_ACTION_SIGN_OUT;
        have = true;
      } else if (v->can_sign_in && ui_in_rect(ev->x, ev->y, primary)) {
        action = CCORD_ACCOUNT_ACTION_SIGN_IN;
        have = true;
      }
      if (have) {
        if (ui->d.host.account_action) ui->d.host.account_action(ui->d.host.ud, action);
        ui->dirty = true;
        return true;
      }
      return true;
    }
    bool handled = false;
    if (ui->settings_page == SP_VOICE) {
      sgeom gv = g;
      gv.content.y -= st_s_voice_scroll;   // rows scroll; hit-testing matches the drawn positions
      handled = handle_page_controls(ui, &gv, ev->x, ev->y);
    } else if (ui->settings_page == SP_APPEARANCE || ui->settings_page == SP_STREAMING ||
               ui->settings_page == SP_KEYBINDS) {
      sgeom gs = g;
      float max_p = st_page_max_scroll(ui, &g);
      if (max_p > 0.0f) {
        if (st_s_page_scroll > max_p) st_s_page_scroll = max_p;
        gs.content.y -= st_s_page_scroll;
      }
      handled = handle_page_controls(ui, &gs, ev->x, ev->y);
    } else {
      handled = handle_page_controls(ui, &g, ev->x, ev->y);
    }
    st_s_hover_key = -1;
    if (handled &&
        (ui->settings_page == SP_APPEARANCE || ui->settings_page == SP_VOICE ||
         ui->settings_page == SP_STREAMING || ui->settings_page == SP_TEXT_IMAGES ||
         ui->settings_page == SP_NOTIFICATIONS || ui->settings_page == SP_ACCESSIBILITY ||
         ui->settings_page == SP_STREAMER || ui->settings_page == SP_LANGUAGE ||
         ui->settings_page == SP_ADVANCED || ui->settings_page == SP_CCORD)) {
      st_settings_save(ui);
    }
    return true;
  }
  if (ui->settings_page == SP_PROFILE_EDIT && ui_settings_profile_key(ui, ev)) return true;
  if (ui->settings_page == SP_PLUGINS && st_plugins_handle_key(ui, ev)) return true;
  if (ui->settings_page == SP_APPEARANCE && ui->account_focus_field == 6) {
    // QuickCSS snippet editor: every edit applies live through the host and persists.
    ccord_text_edit *active = &ui->quickcss_edit;
    if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
      ccord_error ierr;
      if (ccord_text_edit_insert(active, ev->text, strlen(ev->text), &ierr) == CCORD_OK) ui_quickcss_changed(ui);
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_RETURN || ev->key == CCORD_KEY_TAB) {
        ui->account_focus_field = 0;   // commit and leave the editor
        st_settings_save_flush(ui);
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_BACKSPACE) {
        if (ev->mods & CCORD_MOD_CTRL) ccord_text_edit_backspace_word(active);
        else ccord_text_edit_backspace(active);
        ui_quickcss_changed(ui);
        return true;
      }
      if (ev->key == CCORD_KEY_DELETE) {
        if (ev->mods & CCORD_MOD_CTRL) ccord_text_edit_delete_word(active);
        else ccord_text_edit_delete(active);
        ui_quickcss_changed(ui);
        return true;
      }
      if (ev->key == CCORD_KEY_LEFT) { ccord_text_edit_move(active, -1, (ev->mods & CCORD_MOD_SHIFT) != 0); ui->dirty = true; return true; }
      if (ev->key == CCORD_KEY_RIGHT) { ccord_text_edit_move(active, 1, (ev->mods & CCORD_MOD_SHIFT) != 0); ui->dirty = true; return true; }
      if (ev->key == CCORD_KEY_HOME) { ccord_text_edit_move(active, -4, false); ui->dirty = true; return true; }
      if (ev->key == CCORD_KEY_END) { ccord_text_edit_move(active, 4, false); ui->dirty = true; return true; }
      if ((ev->mods & CCORD_MOD_CTRL) && ev->key == CCORD_KEY_A) {
        ccord_text_edit_select_all(active);
        ui->dirty = true;
        return true;
      }
      if ((ev->mods & CCORD_MOD_CTRL) && ev->key == CCORD_KEY_V && ui->d.host.clipboard_get) {
        char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
        if (clip) {
          ccord_error ierr;
          if (ccord_text_edit_insert(active, clip, strlen(clip), &ierr) == CCORD_OK) ui_quickcss_changed(ui);
          ccord_free(clip);
        }
        ui->dirty = true;
        return true;
      }
    }
    return true;   // modal editor: swallow everything else
  }
  if (ui->settings_page == SP_ACCOUNT && ui->account_focus_field != 0) {
    ccord_text_edit *active = nullptr;
    bool masked = false;
    if (ui->account_focus_field == 1) { active = &ui->account_token_edit; masked = true; }
    else if (ui->account_focus_field == 2) { active = &ui->account_email_edit; }
    else if (ui->account_focus_field == 3) { active = &ui->account_password_edit; masked = true; }
    if (active) {
      if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
        ccord_error ierr;
        (void)ccord_text_edit_insert(active, ev->text, strlen(ev->text), &ierr);
        ui->dirty = true;
        return true;
      }
      if (ev->type == CCORD_IN_KEY_DOWN) {
        if (ev->key == CCORD_KEY_TAB && !ui->account_view.signed_in) {
          if (ui->account_focus_field == 2) ui->account_focus_field = 3;
          else if (ui->account_focus_field == 3) ui->account_focus_field = 1;
          else ui->account_focus_field = 2;
          ui->dirty = true;
          return true;
        }
        if (ev->key == CCORD_KEY_RETURN) {
          if (ui->d.host.account_action) {
            ccord_account_action act = (ui->account_focus_field == 1)
                                         ? CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN
                                         : CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN;
            ui->d.host.account_action(ui->d.host.ud, act);
          }
          ui->dirty = true;
          return true;
        }
        if (st_handle_account_field_edit_key(ui, active, masked, ev)) return true;
      }
    }
  }
  if (st_s_ptt_armed && ui->settings_page == SP_VOICE) {
    // Live keybind capture: the next key press binds Push to Talk. Escape never arrives here
    // (ui.c closes the modal with it); the stale-armed cleanup above handles that path.
    if (ev->type == CCORD_IN_KEY_DOWN) {
      SDL_Scancode sc = st_key_to_scancode(ev->key);
      if (sc != SDL_SCANCODE_UNKNOWN) {
        ui->settings_ptt_scancode = (int)sc;
        st_s_ptt_armed = false;
        ui->account_focus_field = 0;
        st_settings_save(ui);
      }
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_TEXT) {
      ui->dirty = true;
      return true;   // swallow text while waiting for a key
    }
  }
  if (st_s_kb_armed >= 0 && ui->settings_page == SP_KEYBINDS) {
    // Live capture for Mute / Deafen / Quick Switcher: the next key press binds scancode + mods.
    if (ev->type == CCORD_IN_KEY_DOWN) {
      SDL_Scancode sc = st_key_to_scancode(ev->key);
      if (sc != SDL_SCANCODE_UNKNOWN) {
        ui->settings_keybinds[st_s_kb_armed].scancode = (int)sc;
        ui->settings_keybinds[st_s_kb_armed].mods =
            ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SHIFT | CCORD_MOD_ALT | CCORD_MOD_SUPER);
        st_s_kb_armed = -1;
        ui->account_focus_field = 0;
        st_settings_save(ui);
        if (ui->d.host.keybinds_changed) ui->d.host.keybinds_changed(ui->d.host.ud);
      }
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_TEXT) {
      ui->dirty = true;
      return true;   // swallow text while waiting for a key
    }
  }
  if (st_s_plugin_kb_armed >= 0 && ui->settings_page == SP_KEYBINDS && ui->d.contrib) {
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_ESCAPE) {
        st_s_plugin_kb_armed = -1;
        ui->account_focus_field = 0;
        ui->dirty = true;
        return true;
      }
      SDL_Scancode sc = st_key_to_scancode(ev->key);
      if (sc != SDL_SCANCODE_UNKNOWN) {
        ccord_ui_keybind tmp = {
          .scancode = (int)sc,
          .mods = (uint8_t)(ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SHIFT | CCORD_MOD_ALT | CCORD_MOD_SUPER)),
        };
        char trig[64] = {0};
        ccord_ui_keybind_trigger(&tmp, trig, sizeof trig);
        const ccord_keybind_contribution *kb = ccord_contrib_keybind_at(ui->d.contrib, (size_t)st_s_plugin_kb_armed);
        if (kb && trig[0]) {
          (void)ccord_contrib_rebind_keybind(ui->d.contrib, kb->owner, kb->bind_id, trig);
          if (ui->d.host.keybinds_changed) ui->d.host.keybinds_changed(ui->d.host.ud);
        }
        st_s_plugin_kb_armed = -1;
        ui->account_focus_field = 0;
      }
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_TEXT) {
      ui->dirty = true;
      return true;
    }
  }
  if (ev->type == CCORD_IN_KEY_DOWN && !(ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SHIFT | CCORD_MOD_ALT | CCORD_MOD_SUPER)) &&
      ui->account_focus_field == 0 && (ev->key == CCORD_KEY_UP || ev->key == CCORD_KEY_DOWN)) {
    return st_sidebar_key_step(ui, ev->key == CCORD_KEY_UP ? -1 : 1);
  }
  return true;   // modal: swallow everything else
}

