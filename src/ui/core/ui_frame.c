// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ui_core_internal.h"

// ---------------------------------------------------------------- draw entry and introspection

static void ui_draw_loading_screen(ccord_ui *ui, ccord_renderer *r)
{
  const ccord_theme_colors *c = &ui->theme->c;
  ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h}, c->bg_tertiary);
  float cx = ui->win_w * 0.5f;
  float cy = ui->win_h * 0.5f;
  ccord_text_style title_st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 14.0f, c->text_header);
  const char *title = "C-Cord";
  ccord_vec2 ts = ccord_text_measure(r, title, strlen(title), &title_st, 0.0f);
  ccord_text_draw(r, title, strlen(title), cx - ts.x * 0.5f, cy - ts.y - 18.0f, &title_st, 0.0f);

  float bar_w = 280.0f;
  float bar_h = 8.0f;
  if (bar_w > ui->win_w - 48.0f && ui->win_w > 64.0f) bar_w = ui->win_w - 48.0f;
  ccord_rect track = {cx - bar_w * 0.5f, cy, bar_w, bar_h};
  ui->loading_bar_rect = track;
  ccord_draw_rrect(r, track, bar_h * 0.5f, c->bg_primary);
  float prog = uc_clampf(ui->loading_progress > 0.0f ? ui->loading_progress : 0.15f, 0.05f, 1.0f);
  ccord_rect fill = {track.x, track.y, bar_w * prog, bar_h};
  ccord_draw_rrect(r, fill, bar_h * 0.5f, c->brand);

  const char *sub = (ui->d.session && ccord_session_state(ui->d.session) == CCORD_CONN_CONNECTED)
                      ? "Loading servers & channels..."
                      : "Connecting to Discord...";
  ccord_text_style sub_st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), c->text_muted);
  ccord_vec2 ss = ccord_text_measure(r, sub, strlen(sub), &sub_st, 0.0f);
  ccord_text_draw(r, sub, strlen(sub), cx - ss.x * 0.5f, cy + bar_h + 14.0f, &sub_st, 0.0f);
}

// Draws one popup with its entrance: it fades in (and slides `slide_px` up into place) from the frame it opens. `open`
// must be the same condition the popup uses to draw anything, so a closed popup leaves no trace and the next opening
// starts from nothing again.
static void draw_popup(ccord_ui *ui, ccord_renderer *r, bool open, const char *ns, uint32_t ms, float slide_px,
                       void (*draw)(ccord_ui *, ccord_renderer *))
{
  if (!open) {
    draw(ui, r);
    return;
  }
  ui_popup_begin(ui, r, ns, 0, ms, slide_px);
  draw(ui, r);
  ui_popup_end(r);
}

// The same with a close transition. The popup's presence eases up while `open` and down after, and it is drawn
// while either. A closing popup is drawn with `*flag` (the state `draw` tests to draw anything) put back for that
// call only, so the rest of the UI keeps seeing it closed and sends it no input; one more call with the flag cleared
// then lets `draw` reset its hit rects as it always does when closed. Reopening mid-fade continues from the value.
static void draw_overlay_popup(ccord_ui *ui, ccord_renderer *r, ui_overlay o, const char *ns, bool open, bool *flag,
                               uint32_t open_ms, uint32_t close_ms, float slide_px,
                               void (*draw)(ccord_ui *, ccord_renderer *))
{
  float t = ui_overlay_presence(ui, o, open, open_ms, close_ms);
  if (open) {
    // The popup's own entrance (it starts over after any frame it was not drawn open) and the presence agree when it
    // opens from nothing; reopened mid-exit the presence is the one that continues from where it is.
    float enter = ui_enter(ui, ns, 0, open_ms);
    ui_overlay_begin(ui, r, o, enter > t ? enter : t, slide_px);
    draw(ui, r);
    ui_overlay_end(ui, r);
    return;
  }
  if (flag && t > UI_PRESENCE_EPS) {
    *flag = true;
    ui_overlay_begin(ui, r, o, t, slide_px);
    draw(ui, r);
    ui_overlay_end(ui, r);
    *flag = false;
  }
  draw(ui, r);
}

// ---- Stale-while-revalidate (warm start) ----
// The store was seeded from the on-disk cache and the session has not had its READY yet: say so, so that nobody
// mistakes saved data for live data or wonders why sending is refused. One UI per process, like the settings.
static int64_t g_stale_saved_ms;   // when the cache was written (unix ms); 0 = unknown

void ccord_ui_set_stale_saved_ms(ccord_ui *ui, int64_t saved_ms)
{
  (void)ui;
  g_stale_saved_ms = saved_ms > 0 ? saved_ms : 0;
}

bool ui_stale_bar_active(const ccord_ui *ui)
{
  return ui && ui->d.session && ccord_session_is_stale(ui->d.session) && !ui_is_loading_screen(ui) &&
         !ui_is_login_screen(ui);
}

static void ui_stale_bar_text(const ccord_ui *ui, char *out, size_t cap)
{
  ccord_connection_state st = ccord_session_state(ui->d.session);
  if (st != CCORD_CONN_FAILED && st != CCORD_CONN_DISCONNECTED) {
    snprintf(out, cap, "Connecting\xe2\x80\xa6 showing saved data");
    return;
  }
  int64_t age_s = (g_stale_saved_ms > 0 && ui->now_unix_ms > g_stale_saved_ms) ? (ui->now_unix_ms - g_stale_saved_ms) / 1000 : 0;
  if (g_stale_saved_ms <= 0) snprintf(out, cap, "Offline \xe2\x80\x94 showing saved data");
  else if (age_s < 60) snprintf(out, cap, "Offline \xe2\x80\x94 showing saved data from just now");
  else if (age_s < 3600) snprintf(out, cap, "Offline \xe2\x80\x94 showing saved data from %lld min ago", (long long)(age_s / 60));
  else if (age_s < 86400) snprintf(out, cap, "Offline \xe2\x80\x94 showing saved data from %lld h ago", (long long)(age_s / 3600));
  else snprintf(out, cap, "Offline \xe2\x80\x94 showing saved data from %lld d ago", (long long)(age_s / 86400));
}

// A 22 px strip at the top of the chat pane (below the channel header) while the data on screen is the saved copy.
static void ui_draw_stale_bar(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui_stale_bar_active(ui)) return;
  const ccord_theme_colors *c = &ui->theme->c;
  const ccord_layout_rect *m = &ui->layout.message_area;
  if (m->w < 40.0f || m->h < 60.0f) return;
  ccord_rect bar = {m->x, m->y, m->w, 22.0f};
  ccord_draw_rect(r, bar, c->bg_secondary);
  char text[160];
  ui_stale_bar_text(ui, text, sizeof text);
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), c->text_muted);
  float lh = ui_line_h(ui, st.size);
  ccord_text_draw_ellipsis(r, text, strlen(text), bar.x + 12.0f, bar.y + (bar.h - lh) * 0.5f, bar.w - 24.0f, &st);
}

static void ui_draw_update_bar(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || ui->update_banner_dismissed) return;
  if (ui_is_loading_screen(ui) || ui_is_login_screen(ui)) return;
  ccord_ui_update_state st = ui->update_info.state;
  if (st != CCORD_UI_UPDATE_AVAILABLE && st != CCORD_UI_UPDATE_DOWNLOADING && st != CCORD_UI_UPDATE_READY) return;
  const ccord_theme_colors *c = &ui->theme->c;
  const ccord_layout_rect *m = &ui->layout.message_area;
  if (m->w < 260.0f || m->h < 80.0f) return;
  float y = m->y + (ui_stale_bar_active(ui) ? 22.0f : 0.0f);
  ccord_rect bar = {m->x, y, m->w, 34.0f};
  ccord_draw_rect(r, bar, c->brand);
  ccord_rect dismiss_r = {bar.x + bar.w - 32.0f, bar.y + 4.0f, 26.0f, 26.0f};
  ui->update_banner_dismiss_rect = dismiss_r;
  float right_x = dismiss_r.x - 6.0f;
  if (ui->update_info.release_url[0]) {
    ccord_rect notes_r = {right_x - 66.0f, bar.y + 4.0f, 66.0f, 26.0f};
    ui->update_banner_notes_rect = notes_r;
    uis_button(ui, r, notes_r, "Notes", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, notes_r)}, "updn", 0);
    right_x = notes_r.x - 6.0f;
  }
  if (ui->update_info.self_updatable && ui->update_info.can_install &&
      (st == CCORD_UI_UPDATE_AVAILABLE || st == CCORD_UI_UPDATE_DOWNLOADING)) {
    const char *blbl = (st == CCORD_UI_UPDATE_DOWNLOADING) ? "Downloading..." : "Update";
    float bw = (st == CCORD_UI_UPDATE_DOWNLOADING) ? 106.0f : 72.0f;
    ccord_rect inst_r = {right_x - bw, bar.y + 4.0f, bw, 26.0f};
    ui->update_banner_install_rect = inst_r;
    uis_button(ui, r, inst_r, blbl, UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, inst_r),
                           .disabled = st == CCORD_UI_UPDATE_DOWNLOADING},
               "updi", 0);
    right_x = inst_r.x - 10.0f;
  } else {
    right_x -= 4.0f;
  }
  uis_button(ui, r, dismiss_r, "x", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, dismiss_r)}, "updx", 0);
  char msg[220];
  if (ui->update_info.status_text[0]) {
    snprintf(msg, sizeof msg, "%s", ui->update_info.status_text);
  } else {
    snprintf(msg, sizeof msg, "C-Cord %s is available.", ui->update_info.latest_version);
  }
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), c->text_on_brand);
  float lh = ui_line_h(ui, ts.size);
  float tw = right_x - (bar.x + 12.0f);
  if (tw > 20.0f) ccord_text_draw_ellipsis(r, msg, strlen(msg), bar.x + 12.0f, bar.y + (bar.h - lh) * 0.5f, tw, &ts);
}

static void ui_draw_frame(ccord_ui *ui, ccord_renderer *r);

void ccord_ui_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r) return;
  ui_draw_frame(ui, r);
  // Animated images only advance when drawn, and long ones decode a little per frame: record both activity
  // and the earliest next frame deadline so the main loop can sleep between GIF/avatar frame transitions.
  bool decoding = ccord_render_pump_media(r, 5);
  bool anim_act = ccord_render_take_anim_activity(r);
  uint64_t anim_due = 0;
  bool has_due = ccord_render_take_anim_next_ms(r, &anim_due);
  ui->media_animating = anim_act || decoding;
  if (decoding) {
    ui->has_media_anim_next = true;
    ui->media_anim_next_ms = ui->now_ms;
  } else if (anim_act && has_due && ui->now_ms != 0) {
    ui->has_media_anim_next = true;
    ui->media_anim_next_ms = anim_due;
  } else {
    ui->has_media_anim_next = false;
    ui->media_anim_next_ms = 0;
  }
}

static bool overlay_enter_opaque(const ccord_ui *ui, const char *ns)
{
  if (ui->anim.reduced_motion) return true;
  uint32_t key = ccord_anim_key(ns, 0);
  size_t base = (size_t)key % CCORD_ANIM_SLOTS;
  for (size_t i = 0; i < 8u; i++) {
    const ccord_anim_slot *s = &ui->anim.slots[(base + i) % CCORD_ANIM_SLOTS];
    if (s->key == key) {
      if (s->seen_frame + 1u == ui->anim.frame &&
          (s->value >= 0.999f || (s->to >= 0.999f && ui->anim.now_ms >= s->start_ms + (uint64_t)s->dur_ms))) {
        return true;
      }
      return false;
    }
  }
  return false;
}

static void ui_draw_frame(ccord_ui *ui, ccord_renderer *r)
{
  ui->last_renderer = r;
  ui_plugin_v3_begin_frame(ui);
  ccord_anim_begin_frame(&ui->anim, ui->now_ms);
  ccord_render_set_anim_clock(r, ui->now_ms);
  ccord_arena_reset(&ui->hit_arena);
  ui->nlinks = 0;
  ui->nimg_hits = 0;
  ui->nmedia_hits = 0;
  ui->ncomp_hits = 0;
  ui->ncode_copy_hits = 0;
  ui->nprofile_modal_conn_hits = 0;
  ui->typing_next_ms = 0;
  ui->typing_anim_only = false;
  ui_pump_media(ui, r);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  if (ui_is_loading_screen(ui)) {
    ui_draw_loading_screen(ui, r);
    return;
  }
  if (ui_is_login_screen(ui)) {
    // Full-window opaque login screen: skip drawing the invisible workspace behind it so frame 1 is instant,
    // and draw context menus / notices on top so mobile Copy/Paste sheets appear above login fields.
    ui_settings_draw(ui, r);
    ui_menu_draw(ui, r);
    ui_draw_notice(ui, r);
    uc_update_hover(ui);
    return;
  }
  if (ui->stream_fullscreen && !ui_stream_is_active(ui)) {
    ui_stream_set_fullscreen(ui, false);
  }
  if (ui->stream_fullscreen) {
    ui->stream_pip_rect = (ccord_rect){0, 0, 0, 0};
    ui_draw_stream_fullscreen(ui, r);
    ui_draw_notice(ui, r);
    uc_update_hover(ui);
    return;
  }
  ui->loading_bar_rect = (ccord_rect){0.0f, 0.0f, 0.0f, 0.0f};
  ui->dbg_clan_badges = 0;
  ui->dbg_activity_cards = 0;
  ui->dbg_role_pills = 0;
  ui->dbg_rail_tip_voice_shown = 0;
  ui->dbg_rail_tip_voice_total = 0;
  ui_profile_roles_begin_frame(ui);
  ui->dbg_tts_badges = 0;
  ui->ngdm_member_hits = 0;
  ui->dm_panel_full_profile_rect = (ccord_rect){0};
  ui->dm_header_plus_rect = (ccord_rect){0};
  ui->dm_requests_nav_rect = (ccord_rect){0};
  ui->ndm_close_rects = 0;
  ui->nchan_gear_rects = 0;
  ui->gdm_add_btn_rect = (ccord_rect){0};
  ui->gdm_edit_btn_rect = (ccord_rect){0};
  ui->dm_requests_tab_rects[0] = (ccord_rect){0};
  ui->dm_requests_tab_rects[1] = (ccord_rect){0};
  ui->ndm_request_hits = 0;
  ui->msg_req_banner_visible = false;
  ui->msg_req_banner_accept_rect = (ccord_rect){0};
  ui->msg_req_banner_reject_rect = (ccord_rect){0};
  ui->mv.composer_btn = (ccord_rect){0};   // ui_draw_composer sets it again while the box is a screening banner
  ui->popover_full_profile_rect = (ccord_rect){0};
  ui->role_preview_cycle_rect = (ccord_rect){0};
  ui->role_preview_exit_rect = (ccord_rect){0};
  ui->update_banner_install_rect = (ccord_rect){0};
  ui->update_banner_notes_rect = (ccord_rect){0};
  ui->update_banner_dismiss_rect = (ccord_rect){0};
  ui->mobile_send_btn_rect = (ccord_rect){0};

  bool full_settings = ui->settings_open && !ui_is_login_screen(ui);
  bool full_ss = ui_server_settings_is_open(ui);
  bool full_cs = ui_channel_settings_is_open(ui);
  bool opaque_full_ovl = !ui->go_live_open &&
                         ((full_settings && overlay_enter_opaque(ui, "settings")) ||
                          (full_ss && overlay_enter_opaque(ui, "ssettings")) ||
                          (full_cs && overlay_enter_opaque(ui, "csettings")));

  const ccord_theme_colors *c = &ui->theme->c;
  if (!opaque_full_ovl) {
    float box_h = ui_composer_measure(ui, r);
    if (box_h != ui->composer_box_h) {
      ui->composer_box_h = box_h;
      ui_compute_layout(ui);
    }
    ccord_draw_rect(r, (ccord_rect){0, 0, ui->win_w, ui->win_h}, c->bg_primary);
    ui_theme_bg_draw(ui, r, "window", (ccord_rect){0, 0, ui->win_w, ui->win_h});
    if (ui->discovery_open) {
      ui_draw_discovery(ui, r);
    } else if (ui_panels_has_active_full_view(ui)) {
      float fv_x = ui->layout.chat_header.x;
      float fv_y = ui_panels_slot_height(ui, CCORD_PANEL_TOP);
      float fv_h = ui->win_h - fv_y - ui_panels_slot_height(ui, CCORD_PANEL_BOTTOM);
      if (fv_h < 0.0f) fv_h = 0.0f;
      ui_panels_draw_full_view(ui, r, (ccord_rect){fv_x, fv_y, ui->win_w - fv_x, fv_h});
    } else if (ui->stage_channel != CCORD_ID_NONE) {
      if (ui_stage_mobile_chat_active(ui)) {
        ui_draw_messages(ui, r);
        ui_draw_chat_header(ui, r);
        ui_draw_composer(ui, r);
      } else {
        ui_draw_voice_stage(ui, r);
        if (ui_stage_split_active(ui)) ui_draw_messages(ui, r);   // side-by-side Text Chat pane
        ui_draw_chat_header(ui, r);
        if (!ui->mobile_layout || ui_stage_split_active(ui)) ui_draw_composer(ui, r);
        if (ui_stage_split_active(ui)) ui_draw_stage_split_divider(ui, r);
      }
    } else if (ui_forum_is_active(ui)) {
      ui_draw_forum(ui, r);
      ui_draw_chat_header(ui, r);
    } else {
      ui_draw_messages(ui, r);
      ui_draw_chat_header(ui, r);
      ui_draw_composer(ui, r);
    }
    ui_anchor_remember(ui);
    ui_draw_stale_bar(ui, r);
    ui_draw_update_bar(ui, r);
    if (!ui->mobile_layout && !ui->discovery_open && ui->layout.member_list_visible) ui_draw_member_list(ui, r);
    if (ui->d.store && !ui->discovery_open &&
        ui_role_preview_active_for(ui, ccord_store_selected_guild(ui->d.store))) {
      ccord_id pg = ui->role_preview_guild;
      const ccord_role *pr = ccord_store_role(ui->d.store, pg, ui->role_preview_role_id);
      const char *rname = (pr && pr->name && pr->name[0]) ? pr->name : "@everyone";
      ccord_rect bar = {ui->layout.chat_header.x, ui_panels_slot_height(ui, CCORD_PANEL_TOP),
                        ui->win_w - ui->layout.chat_header.x, 36.0f};
      ccord_draw_rect(r, bar, c->brand);
      char lbl[160];
      if (rname[0] == '@') snprintf(lbl, sizeof lbl, "Viewing server as role: %s", rname);
      else snprintf(lbl, sizeof lbl, "Viewing server as role: @%s", rname);
      ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), c->text_on_brand);
      float blh = ui_line_h(ui, bs.size);
      ccord_rect exit_r = {bar.x + bar.w - 104.0f, bar.y + 5.0f, 92.0f, 26.0f};
      ccord_rect cyc_r = {exit_r.x - 104.0f, bar.y + 5.0f, 96.0f, 26.0f};
      ui->role_preview_exit_rect = exit_r;
      ui->role_preview_cycle_rect = cyc_r;
      float text_max = cyc_r.x - (bar.x + 16.0f) - 10.0f;
      if (text_max > 24.0f) {
        ccord_text_draw_ellipsis(r, lbl, strlen(lbl), bar.x + 14.0f, bar.y + (bar.h - blh) * 0.5f, text_max, &bs);
      }
      bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cyc_r);
      bool ehv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, exit_r);
      ccord_draw_rrect(r, cyc_r, 4.0f, ui_alpha(c->bg_tertiary, chv ? 0.55f : 0.35f));
      ccord_draw_rrect(r, exit_r, 4.0f, ui_alpha(c->bg_tertiary, ehv ? 0.70f : 0.45f));
      ccord_text_style btns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, c->text_on_brand);
      ccord_vec2 cm = ccord_text_measure(r, "Switch Role", 11, &btns, 0.0f);
      ccord_text_draw(r, "Switch Role", 11, cyc_r.x + (cyc_r.w - cm.x) * 0.5f, cyc_r.y + (cyc_r.h - cm.y) * 0.5f, &btns, 0.0f);
      ccord_vec2 em = ccord_text_measure(r, "Exit Preview", 12, &btns, 0.0f);
      ccord_text_draw(r, "Exit Preview", 12, exit_r.x + (exit_r.w - em.x) * 0.5f, exit_r.y + (exit_r.h - em.y) * 0.5f, &btns, 0.0f);
    }
    draw_overlay_popup(ui, r, UI_OVL_TIP, "tip",
                       !ui->touch_mode && !ui->discovery_open && ui->hover_members_btn && !ui->menu.open && !ui->settings_open,
                       &ui->hover_members_btn, 90, UI_TIP_CLOSE_MS, 0.0f, ui_draw_chat_header_tooltip);
    {
      bool ts_open = !ui->touch_mode && !ui->discovery_open && ui->hover_ts_hit >= 0 && (size_t)ui->hover_ts_hit < ui->nts_hits &&
                     !ui->menu.open && !ui->settings_open && !ui->pins_open && !ui->search_open && !ui->inbox_open &&
                     !ui->shortcuts_open;
      if (ts_open) ui->tstip_last_hit = ui->hover_ts_hit;
      float ts_t = ui_overlay_presence(ui, UI_OVL_TSTIP, ts_open, 90, UI_TIP_CLOSE_MS);
      if (ts_open) {
        float ts_enter = ui_enter(ui, "tstip", 0, 90);
        ui_overlay_begin(ui, r, UI_OVL_TSTIP, ts_enter > ts_t ? ts_enter : ts_t, 0.0f);
        ui_draw_timestamp_tooltip(ui, r);
        ui_overlay_end(ui, r);
      } else {
        // Fading out: the hit the pointer just left (it is -1 now) is put back for that call only.
        int held = ui->hover_ts_hit;
        if (ts_t > UI_PRESENCE_EPS && ui->tstip_last_hit >= 0 && (size_t)ui->tstip_last_hit < ui->nts_hits &&
            !ui->menu.open && !ui->settings_open) {
          ui->hover_ts_hit = ui->tstip_last_hit;
          ui_overlay_begin(ui, r, UI_OVL_TSTIP, ts_t, 0.0f);
          ui_draw_timestamp_tooltip(ui, r);
          ui_overlay_end(ui, r);
          ui->hover_ts_hit = held;
        }
        ui_draw_timestamp_tooltip(ui, r);   // closed: clears the tooltip text
      }
    }
    draw_overlay_popup(ui, r, UI_OVL_BARTIP, "bartip", !ui->touch_mode && ui_bar_edit_hovered(ui), nullptr, 90, UI_TIP_CLOSE_MS, 0.0f,
                       ui_draw_bar_edit_tooltip);
    if (ui->mobile_layout) {
      float drawer_t = fmaxf(ui->left_drawer_t, ui->right_drawer_t);
      if (drawer_t > 0.001f) {
        ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h},
                        ui_alpha(c->bg_tertiary, 0.56f * uc_clampf(drawer_t, 0.0f, 1.0f)));
      }
      if (ui->layout.right_drawer_visible && !ui->discovery_open) {
        ui_draw_member_list(ui, r);
      }
      if (ui->layout.left_drawer_visible) {
        ui_draw_channel_list(ui, r);
        ui_draw_server_list(ui, r);
        ui_draw_voice_bar(ui, r);
        ui_draw_user_panel(ui, r);
        ui_draw_server_rail_tooltip(ui, r);
      }
      ui_panels_draw(ui, r);
      ui_draw_stream_pip(ui, r);
    } else {
      ui_draw_channel_list(ui, r);
      ui_draw_server_list(ui, r);
      ui_panels_draw(ui, r);
      ui_draw_voice_bar(ui, r);
      ui_draw_user_panel(ui, r);
      ui_draw_server_rail_tooltip(ui, r);
      ui_draw_stream_pip(ui, r);
    }
    ui_draw_notice(ui, r);
    draw_overlay_popup(ui, r, UI_OVL_SEARCH, "search", ui->search_open, &ui->search_open, UI_POPUP_MS, UI_POPUP_CLOSE_MS, 6.0f,
                       ui_draw_search_popover);
    draw_overlay_popup(ui, r, UI_OVL_PINS, "pins", ui->pins_open, &ui->pins_open, UI_POPUP_MS, UI_POPUP_CLOSE_MS, 6.0f,
                       ui_draw_pins_popover);
    draw_overlay_popup(ui, r, UI_OVL_INBOX, "inbox", ui->inbox_open, &ui->inbox_open, UI_POPUP_MS, UI_POPUP_CLOSE_MS, 6.0f,
                       ui_draw_inbox_popover);
    ui_draw_saved_messages_popover(ui, r);   // manages its own popup entrance; resets hit rects when closed
    draw_overlay_popup(ui, r, UI_OVL_REACTIONS, "reactpop", ui->reactions_open, &ui->reactions_open, UI_MODAL_MS,
                       UI_MODAL_CLOSE_MS, 6.0f, ui_draw_reactions_popover);
    draw_overlay_popup(ui, r, UI_OVL_PICKER, "emoji", ui->emoji_picker_open, &ui->emoji_picker_open, UI_POPUP_MS,
                       UI_POPUP_CLOSE_MS, 8.0f, ui_draw_emoji_picker);
    bool autoc_show = ui->autocomplete_open && ui->nautocomplete > 0 && !ui->settings_open && !ui->menu.open &&
                      !ui->discovery_open && !ui_forum_is_active(ui) && uc_composer_accepts_text(ui);
    draw_popup(ui, r, autoc_show, "autoc", 90, 4.0f, ui_draw_autocomplete);
    draw_overlay_popup(ui, r, UI_OVL_MEMBER_POPOVER, "mpop", ui->member_popover_open, &ui->member_popover_open, UI_POPUP_MS,
                       UI_POPUP_CLOSE_MS, 6.0f, ui_draw_member_popover);
    ui_status_card_draw(ui, r);
    ui_plus_menu_draw(ui, r);
  }
  draw_popup(ui, r, full_settings, "settings", UI_MODAL_MS, 0.0f, ui_settings_draw);
  draw_popup(ui, r, ui->go_live_open, "golive", UI_MODAL_MS, 10.0f, ui_draw_go_live);
  draw_popup(ui, r, full_ss, "ssettings", UI_MODAL_MS, 0.0f, ui_server_settings_draw);
  draw_popup(ui, r, full_cs, "csettings", UI_MODAL_MS, 0.0f, ui_channel_settings_draw);
  if (opaque_full_ovl) {
    ui_draw_notice(ui, r);
    if (!ui->settings_open && !ui_server_settings_is_open(ui) && !ui_channel_settings_is_open(ui)) {
      ui->dirty = true;   // overlay self-closed during draw (e.g. guild/channel deleted): paint workspace next frame
    }
  }
  draw_overlay_popup(ui, r, UI_OVL_SHORTCUTS, "shortcuts", ui->shortcuts_open, &ui->shortcuts_open, UI_MODAL_MS, UI_MODAL_CLOSE_MS,
                     8.0f, ui_draw_shortcuts_modal);
  ui_poll_composer_draw(ui, r);
  ui_schedule_dialog_draw(ui, r);
  ui_report_flow_draw(ui, r);
  ui_panels_draw_overlays(ui, r);
  ui_draw_overlays(ui, r);
  ui_plugin_settings_draw(ui, r);
  ui_plugin_dialog_draw(ui, r);
  ui_profile_roles_draw(ui, r);   // above the popout / profile that anchors it
  ui_menu_draw(ui, r);            // topmost: context menus & mobile Copy/Paste bottom sheets cover settings/modals
  uc_update_hover(ui);   // rows moved under a stationary pointer
  if (ui->typing_next_ms != 0) {
    ui->typing_anim_only = !ui->anim.pending;
    ui->anim.pending = true;
  }
}

