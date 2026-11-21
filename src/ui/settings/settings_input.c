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
      ccord_rect auto_enc = st_auto_pill_rect(g, 0);
      ccord_rect auto_fw = st_auto_pill_rect(g, 1);
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
        if (ui_in_rect(x, y, st_chips_rect(g, 0, i, STREAM_ENC_COUNT))) {
          ui->settings_encoder = st_k_encoder_vals[i];
          ui->gl_encoder = st_k_encoder_vals[i];   // Go Live starts from these defaults
          ui->dirty = true;
          return true;
        }
      }
      for (int i = 0; i < STREAM_FW_COUNT; i++) {
        if (ui_in_rect(x, y, st_chips_rect(g, 1, i, STREAM_FW_COUNT))) {
          ui->settings_framework = st_k_framework_vals[i];
          ui->gl_framework = st_k_framework_vals[i];
          ui->dirty = true;
          return true;
        }
      }
      for (int i = 0; i < 4; i++) {
        if (ui_in_rect(x, y, st_chips_rect(g, 2, i, 4))) {
          ui->settings_resolution = i;
          ui->gl_resolution = i;
          ui->dirty = true;
          return true;
        }
      }
      for (int i = 0; i < 3; i++) {
        if (ui_in_rect(x, y, st_chips_rect(g, 3, i, 3))) {
          ui->settings_fps = i;
          ui->gl_fps = i;
          ui->dirty = true;
          return true;
        }
      }
      for (int i = 0; i < 2; i++) {
        if (ui_in_rect(x, y, st_chips_rect(g, 4, i, 2))) {
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
    case SP_PLUGINS: {
      if (ui_in_rect(x, y, st_extensions_toggle_rect(g))) {
        st_set_extensions(ui, !ui->extensions_on);
        return true;
      }
      ccord_rect r_inst, r_disc, r_upd, r_sync;
      st_plugin_subtab_rects(g, &r_inst, &r_disc, &r_upd, &r_sync);
      if (ui_in_rect(x, y, r_inst)) {
        st_s_plugin_subtab = 0; st_s_plugin_search_focused = false;
        st_s_plugin_log_id[0] = '\0'; st_s_plugin_consent_id[0] = '\0'; st_s_plugin_consent_perms = 0; st_s_plugin_consent_box_h = 224.0f; st_s_plugin_detail_id[0] = '\0';
        return true;
      }
      if (ui_in_rect(x, y, r_disc)) {
        st_s_plugin_subtab = 1; st_s_plugin_search_focused = false;
        st_s_plugin_log_id[0] = '\0'; st_s_plugin_consent_id[0] = '\0'; st_s_plugin_consent_perms = 0; st_s_plugin_consent_box_h = 224.0f; st_s_plugin_detail_id[0] = '\0';
        const ccord_catalog_status *cst = ccord_contrib_catalog_status(ui->d.contrib);
        if (cst && !cst->has_loaded && !cst->syncing) st_trigger_plugin_discover_query(ui, false);
        return true;
      }
      if (ui_in_rect(x, y, r_upd)) {
        st_s_plugin_subtab = 2; st_s_plugin_search_focused = false;
        st_s_plugin_log_id[0] = '\0'; st_s_plugin_consent_id[0] = '\0'; st_s_plugin_consent_perms = 0; st_s_plugin_consent_box_h = 224.0f; st_s_plugin_detail_id[0] = '\0';
        ccord_contrib_catalog_check_updates(ui->d.contrib);
        return true;
      }
      if (ui_in_rect(x, y, r_sync)) {
        ccord_contrib_catalog_refresh(ui->d.contrib);
        return true;
      }

      float px = g->content.x, py = g->content.y + 120.0f, pw = g->content.w;
      if (st_s_plugin_log_id[0]) {
        if (ui_in_rect(x, y, (ccord_rect){px + pw - 246.0f, py - 4.0f, 74.0f, UIS_H_BTN_SM})) {
          ccord_contrib_plugin_reload(ui->d.contrib, st_s_plugin_log_id);
          return true;
        }
        if (ui_in_rect(x, y, (ccord_rect){px + pw - 164.0f, py - 4.0f, 74.0f, UIS_H_BTN_SM})) {
          ccord_contrib_clear_plugin_log(ui->d.contrib, st_s_plugin_log_id);
          return true;
        }
        if (ui_in_rect(x, y, (ccord_rect){px + pw - 82.0f, py - 4.0f, 82.0f, UIS_H_BTN_SM})) {
          st_s_plugin_log_id[0] = '\0';
          return true;
        }
        return false;
      }
      if (st_s_plugin_consent_id[0]) {
        ccord_rect box = {px, py, pw, st_s_plugin_consent_box_h};
        if (ui_in_rect(x, y, (ccord_rect){box.x + 16.0f, box.y + box.h - 46.0f, 184.0f, UIS_H_BTN})) {
          ccord_contrib_plugin_consent_accept_for(ui->d.contrib, st_s_plugin_consent_id, st_s_plugin_consent_perms);
          st_s_plugin_consent_id[0] = '\0';
          st_s_plugin_consent_perms = 0;
          st_s_plugin_consent_box_h = 224.0f;
          return true;
        }
        if (ui_in_rect(x, y, (ccord_rect){box.x + 212.0f, box.y + box.h - 46.0f, 96.0f, UIS_H_BTN})) {
          st_s_plugin_consent_id[0] = '\0';
          st_s_plugin_consent_perms = 0;
          st_s_plugin_consent_box_h = 224.0f;
          return true;
        }
        return false;
      }
      if (st_s_plugin_detail_id[0]) {
        const ccord_catalog_entry *e = ccord_contrib_catalog_detail(ui->d.contrib);
        if (!e || strcmp(e->id, st_s_plugin_detail_id) != 0) {
          const ccord_catalog_entry *fb = ccord_contrib_catalog_find(ui->d.contrib, st_s_plugin_detail_id);
          e = (fb && fb->has_detail) ? fb : nullptr;
        }
        if (ui_in_rect(x, y, (ccord_rect){px, py, 86.0f, UIS_H_BTN_SM})) {
          st_s_plugin_detail_id[0] = '\0';
          return true;
        }
        if (!e) {
          ccord_rect box = {px, py + 34.0f, pw, 128.0f};
          if (ui_in_rect(x, y, (ccord_rect){box.x + 16.0f, box.y + box.h - 44.0f, 120.0f, UIS_H_BTN})) {
            ccord_contrib_catalog_fetch_detail(ui->d.contrib, st_s_plugin_detail_id);
            return true;
          }
          return false;
        }
        ccord_rect box = {px, py + 34.0f, pw, 286.0f};
        if (!e->bundled && !e->installing && (!e->installed || e->update_available)) {
          if (ui_in_rect(x, y, (ccord_rect){box.x + 16.0f, box.y + box.h - 44.0f, 152.0f, UIS_H_BTN})) {
            ccord_contrib_catalog_install(ui->d.contrib, e->id);
            return true;
          }
        }
        if (e->installed && !e->bundled) {
          float ux = box.x + ((!e->installed || e->update_available) ? 180.0f : 16.0f);
          if (ui_in_rect(x, y, (ccord_rect){ux, box.y + box.h - 44.0f, 96.0f, UIS_H_BTN})) {
            char id_copy[65], name_copy[96], eb[128] = "";
            snprintf(id_copy, sizeof id_copy, "%s", e->id);
            snprintf(name_copy, sizeof name_copy, "%s", e->name);
            if (ccord_contrib_plugin_uninstall(ui->d.contrib, id_copy, eb, sizeof eb)) {
              snprintf(st_s_plugin_status_msg, sizeof st_s_plugin_status_msg, "Uninstalled %s", name_copy);
              st_s_plugin_detail_id[0] = '\0';
            } else {
              snprintf(st_s_plugin_status_msg, sizeof st_s_plugin_status_msg, "Uninstall failed: %.150s", eb);
            }
            return true;
          }
        }
        return false;
      }

      if (st_s_plugin_subtab == 1) {
        // Row 1: Search input + Search button + Sort toggle
        ccord_rect r_q = {px, py, pw - 196.0f, UIS_H_BTN_SM};
        ccord_rect r_go = {px + pw - 188.0f, py, 74.0f, UIS_H_BTN_SM};
        ccord_rect r_sort = {px + pw - 106.0f, py, 106.0f, UIS_H_BTN_SM};
        if (ui_in_rect(x, y, r_q)) {
          st_s_plugin_search_focused = true;
          return true;
        }
        st_s_plugin_search_focused = false;
        if (ui_in_rect(x, y, r_go)) {
          st_trigger_plugin_discover_query(ui, false);
          return true;
        }
        if (ui_in_rect(x, y, r_sort)) {
          st_s_plugin_sort_idx = (st_s_plugin_sort_idx + 1) % 5;
          st_trigger_plugin_discover_query(ui, false);
          return true;
        }
        py += 34.0f;

        // Row 2: Tier filters + API Class 1 toggle (server-side query)
        float fx = px;
        static const float k_fw[4] = {54.0f, 76.0f, 76.0f, 88.0f};
        for (int t = 0; t < 4; t++) {
          if (ui_in_rect(x, y, (ccord_rect){fx, py, k_fw[t], UIS_H_BTN_SM})) {
            st_s_plugin_tier_filter = t;
            st_trigger_plugin_discover_query(ui, false);
            return true;
          }
          fx += k_fw[t] + 6.0f;
        }
        if (ui_in_rect(x, y, (ccord_rect){fx + 4.0f, py, 126.0f, UIS_H_BTN_SM})) {
          st_s_plugin_class1_only = !st_s_plugin_class1_only;
          st_trigger_plugin_discover_query(ui, false);
          return true;
        }
        py += 36.0f;

        const ccord_catalog_status *cst = ccord_contrib_catalog_status(ui->d.contrib);
        float bottom_lim = g->panel.y + g->panel.h - (cst && cst->has_more ? 52.0f : 16.0f);
        size_t cn = ui->d.contrib ? ccord_contrib_catalog_count(ui->d.contrib) : 0;
        for (size_t i = 0; i < cn; i++) {
          const ccord_catalog_entry *e = ccord_contrib_catalog_at(ui->d.contrib, i);
          if (!e) continue;
          bool has_w = e->warning_sev != CCORD_WARN_NONE && e->warning_title && e->warning_title[0];
          float ch = 98.0f + (has_w ? 20.0f : 0.0f);
          if (py + ch > bottom_lim) break;
          ccord_rect btn_det = {px + pw - 200.0f, py + ch - 34.0f, 76.0f, UIS_H_BTN_SM};
          ccord_rect btn_act = {px + pw - 116.0f, py + ch - 34.0f, 102.0f, UIS_H_BTN_SM};
          if (ui_in_rect(x, y, btn_det)) {
            snprintf(st_s_plugin_detail_id, sizeof st_s_plugin_detail_id, "%s", e->id);
            ccord_contrib_catalog_fetch_detail(ui->d.contrib, e->id);
            return true;
          }
          if (ui_in_rect(x, y, btn_act) && !e->installing && !e->bundled) {
            if (!e->installed || e->update_available) {
              ccord_contrib_catalog_install(ui->d.contrib, e->id);
              return true;
            }
          }
          py += ch + 10.0f;
        }
        if (cst && cst->has_more && !cst->loading_more) {
          ccord_rect r_more = {px, g->panel.y + g->panel.h - 44.0f, 148.0f, UIS_H_BTN_SM};
          if (ui_in_rect(x, y, r_more)) {
            st_trigger_plugin_discover_query(ui, true);
            return true;
          }
        }
        return false;
      }

      if (st_s_plugin_subtab == 2) {
        // Updates & Settings tab clicks
        const ccord_catalog_prefs *pr_cur = ccord_contrib_catalog_prefs(ui->d.contrib);
        ccord_catalog_prefs pr = pr_cur ? *pr_cur : (ccord_catalog_prefs){.auto_update_enabled = false, .auto_update_min_tier = 2};
        ccord_rect box = {px, py, pw, 142.0f};
        ccord_rect sw_row = {box.x + 14.0f, box.y + 12.0f, box.w - 28.0f, 36.0f};
        ccord_rect sw_hit = {sw_row.x + sw_row.w - 48.0f, sw_row.y + 4.0f, 44.0f, 26.0f};
        if (ui_in_rect(x, y, sw_hit)) {
          pr.auto_update_enabled = !pr.auto_update_enabled;
          ccord_contrib_catalog_set_prefs(ui->d.contrib, &pr);
          return true;
        }
        float tx = box.x + 14.0f, ty = box.y + 68.0f;
        static const float k_tw[3] = {104.0f, 134.0f, 118.0f};
        for (int k = 0; k < 3; k++) {
          if (ui_in_rect(x, y, (ccord_rect){tx, ty, k_tw[k], UIS_H_BTN_SM})) {
            pr.auto_update_min_tier = (uint8_t)(k + 1);
            ccord_contrib_catalog_set_prefs(ui->d.contrib, &pr);
            return true;
          }
          tx += k_tw[k] + 8.0f;
        }
        if (ui_in_rect(x, y, (ccord_rect){box.x + box.w - 178.0f, box.y + box.h - 36.0f, 164.0f, UIS_H_BTN_SM})) {
          ccord_contrib_catalog_run_auto_updates(ui->d.contrib);
          return true;
        }
        py += box.h + 28.0f;
        size_t un = ui->d.contrib ? ccord_contrib_catalog_update_count(ui->d.contrib) : 0;
        for (size_t i = 0; i < un; i++) {
          const ccord_catalog_entry *e = ccord_contrib_catalog_update_at(ui->d.contrib, i);
          if (!e || !e->update_available) continue;
          float ch = 72.0f;
          if (py + ch > g->panel.y + g->panel.h - 16.0f) break;
          ccord_rect btn_u = {px + pw - 134.0f, py + 20.0f, 120.0f, UIS_H_BTN_SM};
          if (ui_in_rect(x, y, btn_u) && !e->installing) {
            if (e->update_needs_review) {
              snprintf(st_s_plugin_detail_id, sizeof st_s_plugin_detail_id, "%s", e->id);
              ccord_contrib_catalog_fetch_detail(ui->d.contrib, e->id);
            } else {
              ccord_contrib_catalog_install(ui->d.contrib, e->id);
            }
            return true;
          }
          py += ch + 8.0f;
        }
        return false;
      }

      // Tab 0: Installed
      py += 24.0f;
      size_t n = ui->d.contrib ? ccord_contrib_plugin_count(ui->d.contrib) : 0;
      for (size_t i = 0; i < n; i++) {
        const ccord_plugin_info *p = ccord_contrib_plugin_at(ui->d.contrib, i);
        if (!p) continue;
        plugin_card_rects rc;
        float h = st_plugin_card_layout(ui, g, i, p, py, &rc);
        if (py + h > g->panel.y + g->panel.h - 16.0f) break;
        if (ui_in_rect(x, y, rc.btn_toggle)) {
          if (p->state == CCORD_PLUGIN_NEEDS_CONSENT) {
            snprintf(st_s_plugin_consent_id, sizeof st_s_plugin_consent_id, "%s", p->id);
            st_s_plugin_consent_perms = p->perms_mask;
            st_s_plugin_consent_box_h = 224.0f;
          } else {
            bool on = !(p->state == CCORD_PLUGIN_READY || p->state == CCORD_PLUGIN_STARTING);
            ccord_contrib_plugin_set_enabled(ui->d.contrib, p->id, on);
          }
          return true;
        }
        if (ui_in_rect(x, y, rc.btn_log)) {
          snprintf(st_s_plugin_log_id, sizeof st_s_plugin_log_id, "%s", p->id);
          return true;
        }
        if (ui_in_rect(x, y, rc.btn_reload)) {
          ccord_contrib_plugin_reload(ui->d.contrib, p->id);
          return true;
        }
        if (rc.btn_configure.w > 0.0f && ui_in_rect(x, y, rc.btn_configure)) {
          ui_plugin_settings_open(ui, p->id);
          return true;
        }
        if (ui_in_rect(x, y, rc.btn_export)) {
          const char *home = getenv("HOME");
          char dest[512], ebuf[128] = "";
          snprintf(dest, sizeof dest, "%s/%s-%s.cce", home && home[0] ? home : ".", p->id, p->version);
          if (ccord_contrib_plugin_export_cce(ui->d.contrib, p->id, dest, ebuf, sizeof ebuf)) {
            snprintf(st_s_plugin_status_msg, sizeof st_s_plugin_status_msg, "Exported: %.175s", dest);
          } else {
            snprintf(st_s_plugin_status_msg, sizeof st_s_plugin_status_msg, "Export failed: %.170s", ebuf);
          }
          return true;
        }
        if (rc.btn_update.w > 0.0f && ui_in_rect(x, y, rc.btn_update)) {
          if (p->update_needs_review) {
            snprintf(st_s_plugin_detail_id, sizeof st_s_plugin_detail_id, "%s", p->id);
            ccord_contrib_catalog_fetch_detail(ui->d.contrib, p->id);
          } else {
            ccord_contrib_catalog_install(ui->d.contrib, p->id);
          }
          return true;
        }
        if (rc.btn_auto.w > 0.0f && ui_in_rect(x, y, rc.btn_auto)) {
          ccord_contrib_catalog_set_plugin_auto_update(ui->d.contrib, p->id, !p->auto_update);
          return true;
        }
        if (rc.btn_uninstall.w > 0.0f && ui_in_rect(x, y, rc.btn_uninstall)) {
          char id_copy[65], name_copy[96], ebuf[128] = "";
          snprintf(id_copy, sizeof id_copy, "%s", p->id);
          snprintf(name_copy, sizeof name_copy, "%s", p->name);
          if (ccord_contrib_plugin_uninstall(ui->d.contrib, id_copy, ebuf, sizeof ebuf)) {
            snprintf(st_s_plugin_status_msg, sizeof st_s_plugin_status_msg, "Uninstalled %s", name_copy);
          } else {
            snprintf(st_s_plugin_status_msg, sizeof st_s_plugin_status_msg, "Uninstall failed: %.150s", ebuf);
          }
          return true;
        }
        py += h + 10.0f;
      }
      break;
    }
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
      for (int i = 0; i < 5; i++) {
        if (ui_in_rect(x, y, st_account_web_btn_rect(g, i))) return 100 + i;
      }
      if (!ui->account_view.signed_in) {
        if (ui_in_rect(x, y, st_account_email_box_rect(g))) return 110;
        if (ui_in_rect(x, y, st_account_password_box_rect(g))) return 111;
        if (ui_in_rect(x, y, st_account_login_btn_rect(g))) return 112;
      }
      if (ui_in_rect(x, y, st_account_token_box_rect(g))) return 113;
      if (ui_in_rect(x, y, st_account_token_paste_rect(g))) return 114;
      if (ui_in_rect(x, y, st_account_token_save_rect(g))) return 115;
      if (ui_in_rect(x, y, st_account_token_clear_rect(g))) return 116;
      if (ui_in_rect(x, y, st_account_add_btn_rect(g))) return 117;
      for (size_t i = 0; i < ui->saved_account_count && i < SAVED_ACCOUNT_ROWS; i++) {
        if (ui_in_rect(x, y, st_saved_account_switch_rect(g, (int)i))) return 120 + (int64_t)i * 2;
        if (ui_in_rect(x, y, st_saved_account_remove_rect(g, (int)i))) return 120 + (int64_t)i * 2 + 1;
      }
      ccord_rect primary, secondary;
      st_account_buttons(ui, g, &primary, &secondary);
      if (ui_in_rect(x, y, primary)) return 140;
      if (ui_in_rect(x, y, secondary)) return 141;
      return 0;
    }
    case SP_APPEARANCE: {
      for (int i = 0; i < 2; i++) {
        if (ui_in_rect(x, y, st_chips_rect(g, 0, i, 2))) return 200 + i;
      }
      for (int i = 0; i < 3; i++) {
        if (ui_in_rect(x, y, st_chips_rect(g, 1, i, 3))) return 210 + i;
      }
      if (ui_in_rect(x, y, st_vol_minus_rect(g, 2))) return 220;
      if (ui_in_rect(x, y, st_vol_plus_rect(g, 2))) return 221;
      if (ui_in_rect(x, y, st_vol_track_rect(g, 2))) return 222;
      ccord_rect reset_r = {g->content.x + 324.0f, st_row_y(g, 2) + 18.0f, 64.0f, UIS_H_BTN_SM};
      if (ui_in_rect(x, y, reset_r)) return 223;
      if (ui_in_rect(x, y, st_toggle_rect(g, 5))) return 225;
      if (ui_in_rect(x, y, st_quickcss_box_rect(g))) return 226;
      if (ui_in_rect(x, y, st_toggle_rect(g, 7))) return 227;
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
    case SP_PLUGINS: {
      if (ui_in_rect(x, y, st_extensions_toggle_rect(g))) return 600;
      ccord_rect r_inst, r_disc, r_upd, r_sync;
      st_plugin_subtab_rects(g, &r_inst, &r_disc, &r_upd, &r_sync);
      if (ui_in_rect(x, y, r_inst)) return 601;
      if (ui_in_rect(x, y, r_disc)) return 602;
      if (ui_in_rect(x, y, r_upd)) return 603;
      if (ui_in_rect(x, y, r_sync)) return 604;
      if (ui_in_rect(x, y, g->content) && y >= g->content.y + 116.0f) {
        int row_cell = (int)((y - (g->content.y + 116.0f)) / 24.0f);
        int col_cell = (int)((x - g->content.x) / 64.0f);
        return 650 + (int64_t)row_cell * 32 + (int64_t)col_cell;
      }
      return 0;
    }
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
    if (ui_in_rect(ev->x, ev->y, g.close) || !ui_in_rect(ev->x, ev->y, g.panel)) {
      st_settings_save_flush(ui);
      ui_open_settings(ui, false);
      return true;
    }
    if (g.back.w > 0.0f && ui_in_rect(ev->x, ev->y, g.back)) {
      ui->mobile_settings_detail = false;
      ui->account_focus_field = 0;
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
      for (int i = 0; i < 5; i++) {
        if (ui_in_rect(ev->x, ev->y, st_account_web_btn_rect(&g, i))) {
          ui_open_url(ui, st_k_web_urls[i], st_k_web_labels[i]);
          ui->dirty = true;
          return true;
        }
      }
      if (!ui->account_view.signed_in) {
        ccord_rect em_box = st_account_email_box_rect(&g);
        ccord_rect pw_box = st_account_password_box_rect(&g);
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
        if (ui_in_rect(ev->x, ev->y, st_account_login_btn_rect(&g))) {
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
        ccord_rect tbox = st_account_token_box_rect(&g);
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
      if (ui_in_rect(ev->x, ev->y, st_account_token_paste_rect(&g))) {
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
      if (ui_in_rect(ev->x, ev->y, st_account_token_save_rect(&g))) {
        if (ui->d.host.account_action) {
          ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN);
        }
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, st_account_token_clear_rect(&g))) {
        ccord_text_edit_clear(&ui->account_token_edit);
        if (ui->d.host.account_action) {
          ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_TOKEN_SIGN_OUT);
        }
        ui->dirty = true;
        return true;
      }
      // Multi-account switcher: Add focuses the token box for a new sign-in; Switch/Remove go to
      // the app (keyring + session/store), which pushes the updated roster back.
      if (ui_in_rect(ev->x, ev->y, st_account_add_btn_rect(&g))) {
        ccord_text_edit_clear(&ui->account_token_edit);
        ui->account_focus_field = 1;
        ui->dirty = true;
        return true;
      }
      for (size_t i = 0; i < ui->saved_account_count && i < SAVED_ACCOUNT_ROWS; i++) {
        if (!ui->saved_accounts[i].active && ui_in_rect(ev->x, ev->y, st_saved_account_switch_rect(&g, (int)i))) {
          if (ui->d.host.account_switch) ui->d.host.account_switch(ui->d.host.ud, ui->saved_accounts[i].user_id);
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, st_saved_account_remove_rect(&g, (int)i))) {
          if (ui->d.host.account_remove) ui->d.host.account_remove(ui->d.host.ud, ui->saved_accounts[i].user_id);
          ui->dirty = true;
          return true;
        }
      }
      ccord_rect primary, secondary;
      st_account_buttons(ui, &g, &primary, &secondary);
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
  if (ui->settings_page == SP_PLUGINS && st_s_plugin_subtab == 1 && !st_s_plugin_detail_id[0] && st_s_plugin_search_focused) {
    if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
      size_t cur = strlen(st_s_plugin_search);
      size_t add = strlen(ev->text);
      if (cur + add < sizeof st_s_plugin_search) {
        memcpy(st_s_plugin_search + cur, ev->text, add + 1);
        ui->dirty = true;
      }
      return true;
    }
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_BACKSPACE) {
        size_t cur = strlen(st_s_plugin_search);
        while (cur > 0 && ((unsigned char)st_s_plugin_search[cur - 1] & 0xC0u) == 0x80u) cur--;
        if (cur > 0) st_s_plugin_search[cur - 1] = '\0';
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_RETURN) {
        st_trigger_plugin_discover_query(ui, false);
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_ESCAPE) {
        st_s_plugin_search_focused = false;
        ui->dirty = true;
        return true;
      }
    }
  }
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

