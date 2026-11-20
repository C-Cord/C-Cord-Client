// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

// ---- stream resize divider, fullscreen viewer & floating resizable PiP --------------------------

enum {
  PIP_EDGE_L = 1u,
  PIP_EDGE_R = 2u,
  PIP_EDGE_T = 4u,
  PIP_EDGE_B = 8u,
};

bool ui_stream_is_active(const ccord_ui *ui)
{
  if (!ui || !ui->d.store) return false;
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  if (st && st->active) return true;
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  return ws && ws->active;
}

bool ui_stream_pip_active(const ccord_ui *ui)
{
  if (!ui || !ui->d.store) return false;
  if (ui->stream_fullscreen || ui->stage_channel != CCORD_ID_NONE) return false;
  if (ui->settings_open || ui_is_login_screen(ui)) return false;
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  bool self_streaming = st && st->active;
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool watching_remote = !self_streaming && ws && ws->active;
  if (self_streaming) return !ui->stream_hide_preview;
  return watching_remote;
}

void ui_draw_stage_split_divider(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r || !ui_stage_split_active(ui)) return;
  ccord_rect handle = ui_stage_split_handle_rect(ui);
  if (handle.w <= 0.0f || handle.h <= 0.0f) return;
  float seam_x = ui->layout.stage_pane.x + ui->layout.stage_pane.w;
  bool dragging = ui->drag == DRAG_STAGE_SPLIT;
  bool hovered = !dragging && ui->mouse_in && ui_in_rect(ui->mx, ui->my, handle);
  if (dragging || hovered) {
    ccord_draw_rect(r, (ccord_rect){seam_x - 1.5f, handle.y, 3.0f, handle.h},
                    dragging ? COL(brand) : ui_mix(COL(border_subtle), COL(brand), 0.65f));
  } else {
    ccord_draw_rect(r, (ccord_rect){seam_x - 0.5f, handle.y, 1.0f, handle.h}, COL(border_subtle));
  }
}

static void stream_open_stage_for_active(ccord_ui *ui)
{
  if (!ui || !ui->d.store) return;
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  const ccord_voice_state *v = ui_self_voice(ui);
  ccord_id ch_id = CCORD_ID_NONE;
  if (st && st->active && st->channel_id != CCORD_ID_NONE) ch_id = st->channel_id;
  else if (ws && ws->active && ws->channel_id != CCORD_ID_NONE) ch_id = ws->channel_id;
  else if (v && v->channel_id != CCORD_ID_NONE) ch_id = v->channel_id;
  if (ch_id == CCORD_ID_NONE) return;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, ch_id);
  if (!ch) return;
  ccord_error err;
  if (ch->guild_id != ccord_store_selected_guild(ui->d.store)) {
    if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, ch->guild_id, &err);
    else (void)ccord_store_select_guild(ui->d.store, ch->guild_id, &err);
  }
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  ui->discovery_open = false;
  ui->stage_channel = ch->id;
  ui->stage_armed = (v && v->channel_id == ch->id);
  ui->stage_scroll = 0.0f;
  ui_compute_layout(ui);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui->dirty = true;
}

static float stream_fs_hud_alpha(const ccord_ui *ui, bool force_visible)
{
  if (force_visible || ui->now_ms == 0) return 1.0f;
  if (ui->mouse_in && ((ui->stream_fs_top_bar.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_top_bar)) ||
                       (ui->stream_fs_bot_bar.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_bot_bar)))) {
    return 1.0f;
  }
  uint64_t idle = ui->now_ms >= ui->stream_fs_hud_ms ? (ui->now_ms - ui->stream_fs_hud_ms) : 0;
  if (idle < 2500u) return 1.0f;
  if (ui->anim.reduced_motion || idle >= 2800u) return 0.0f;
  return 1.0f - (float)(idle - 2500u) / 300.0f;
}

void ui_draw_stream_fullscreen(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r || !ui->stream_fullscreen) return;
  ui->stream_fs_exit_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_fs_bot_exit_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_fs_pause_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_fs_btn1_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_fs_btn2_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_fs_video_rect = (ccord_rect){0, 0, 0, 0};

  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool self_streaming = st && st->active;
  bool watching_remote = !self_streaming && ws && ws->active;
  if (!self_streaming && !watching_remote) {
    ui_stream_set_fullscreen(ui, false);
    return;
  }

  ccord_rect screen = {0.0f, 0.0f, ui->win_w, ui->win_h};
  ccord_draw_rect(r, screen, (ccord_color){0x0A, 0x0B, 0x0D, 255});

  int frame_w = 0, frame_h = 0;
  bool frame_present =
      ccord_render_has_texture(r, "__video_stream__", &frame_w, &frame_h) && frame_w > 0 && frame_h > 0;
  bool paused = (self_streaming && st->paused) || (watching_remote && ws->paused);
  bool hidden_preview = self_streaming && ui->stream_hide_preview;
  bool live = frame_present || hidden_preview;

  const char *badge = "LIVE";
  ccord_color badge_bg = COL(danger);
  ccord_color badge_fg = COL(text_on_brand);
  const char *placeholder = nullptr;
  if (paused) {
    badge = "PAUSED";
    badge_bg = COL(bg_tertiary);
    badge_fg = COL(text_header);
    placeholder = "Stream Paused";
  } else if (!live) {
    badge = self_streaming ? "STARTING" : "CONNECTING";
    badge_bg = COL(bg_tertiary);
    badge_fg = COL(text_header);
    placeholder = self_streaming ? "Starting capture & encoder..." : "Connecting to stream video...";
  }

  if (hidden_preview) {
    const char *hmsg = "Stream preview hidden — your screen share is still live";
    ccord_text_style hms = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
    ccord_vec2 hmt = ccord_text_measure(r, hmsg, strlen(hmsg), &hms, 0.0f);
    ccord_text_draw(r, hmsg, strlen(hmsg), (screen.w - hmt.x) * 0.5f, (screen.h - hmt.y) * 0.5f, &hms, 0.0f);
  } else if (frame_present) {
    ccord_rect vid = dc_stream_fit_aspect(screen, frame_w, frame_h);
    ui->stream_fs_video_rect = vid;
    uint8_t alpha = paused ? 110 : 255;
    ccord_draw_texture(r, "__video_stream__", vid, alpha);
    if (paused) {
      const char *pmsg = "Stream Paused";
      ccord_text_style pms = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
      ccord_vec2 pmt = ccord_text_measure(r, pmsg, strlen(pmsg), &pms, 0.0f);
      ccord_draw_rrect(r, (ccord_rect){(screen.w - pmt.x - 32.0f) * 0.5f, (screen.h - 40.0f) * 0.5f, pmt.x + 32.0f, 40.0f},
                       8.0f, COL(bg_floating));
      ccord_text_draw(r, pmsg, strlen(pmsg), (screen.w - pmt.x) * 0.5f, (screen.h - pmt.y) * 0.5f, &pms, 0.0f);
    }
  } else {
    ccord_text_style phs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
    ccord_vec2 pht = ccord_text_measure(r, placeholder, strlen(placeholder), &phs, 0.0f);
    ccord_text_draw(r, placeholder, strlen(placeholder), (screen.w - pht.x) * 0.5f, (screen.h - pht.y) * 0.5f, &phs, 0.0f);
  }

  ui->stream_fs_top_bar = (ccord_rect){0.0f, 0.0f, screen.w, 54.0f};
  float bot_h = 50.0f;
  float bot_w = fminf(screen.w - 32.0f, self_streaming ? 404.0f : 294.0f);
  if (bot_w < 240.0f) bot_w = fminf(screen.w, 240.0f);
  ui->stream_fs_bot_bar = (ccord_rect){(screen.w - bot_w) * 0.5f, screen.h - bot_h - 18.0f, bot_w, bot_h};

  float hud_a = stream_fs_hud_alpha(ui, paused || !frame_present || hidden_preview);
  if (hud_a <= 0.02f) return;

  ccord_push_alpha(r, hud_a);

  // Top translucent HUD bar
  ccord_draw_rect(r, ui->stream_fs_top_bar, (ccord_color){0x0E, 0x10, 0x14, 210});
  ccord_draw_rect(r, (ccord_rect){0.0f, ui->stream_fs_top_bar.h - 1.0f, screen.w, 1.0f},
                  ui_alpha(COL(border_subtle), 0.5f));

  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, badge_fg);
  ccord_vec2 lt = ccord_text_measure(r, badge, strlen(badge), &ls, 0.0f);
  ccord_rect lb = {16.0f, 17.0f, lt.x + 16.0f, 20.0f};
  ccord_draw_rrect(r, lb, 4.0f, badge_bg);
  ccord_text_draw(r, badge, strlen(badge), lb.x + (lb.w - lt.x) * 0.5f, lb.y + (lb.h - lt.y) * 0.5f, &ls, 0.0f);

  char title[192];
  if (self_streaming) {
    const char *src_name = (ui->has_active_capture_source && ui->active_capture_source.title[0])
                             ? ui->active_capture_source.title
                             : "Screen Share";
    snprintf(title, sizeof title, "Your Stream  ·  %s  (%s · %s)", src_name,
             dc_resolution_label(st->config.resolution), dc_fps_label(st->config.fps));
  } else {
    const ccord_user *wu = ccord_store_user(ui->d.store, ws->user_id);
    if (frame_present) {
      snprintf(title, sizeof title, "%s's Screen Share  ·  %dx%d", wu ? wu->name : "Remote User", frame_w, frame_h);
    } else {
      snprintf(title, sizeof title, "%s's Screen Share", wu ? wu->name : "Remote User");
    }
  }

  ccord_rect exit_btn = {screen.w - 16.0f - 132.0f, 12.0f, 132.0f, 30.0f};
  ui->stream_fs_exit_rect = exit_btn;
  bool exit_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, exit_btn);
  ccord_draw_rrect(r, exit_btn, 5.0f, exit_hov ? COL(bg_item_hover) : COL(bg_tertiary));
  ui_icon_fullscreen(r, exit_btn.x + 16.0f, exit_btn.y + exit_btn.h * 0.5f, 13.0f, COL(text_header), true);
  ccord_text_style exs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
  const char *ex_lbl = "Exit Fullscreen";
  ccord_vec2 ext = ccord_text_measure(r, ex_lbl, strlen(ex_lbl), &exs, 0.0f);
  ccord_text_draw(r, ex_lbl, strlen(ex_lbl), exit_btn.x + 28.0f, exit_btn.y + (exit_btn.h - ext.y) * 0.5f, &exs, 0.0f);

  const char *hint = "Esc to exit";
  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
  ccord_vec2 ht = ccord_text_measure(r, hint, strlen(hint), &hs, 0.0f);
  float title_right = exit_btn.x - 14.0f;
  if (screen.w >= 540.0f) {
    float hx = exit_btn.x - 14.0f - ht.x;
    ccord_text_draw(r, hint, strlen(hint), hx, (ui->stream_fs_top_bar.h - ht.y) * 0.5f, &hs, 0.0f);
    title_right = hx - 16.0f;
  }
  float title_x = lb.x + lb.w + 12.0f;
  float title_w = title_right - title_x;
  if (title_w > 32.0f) {
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    ccord_text_draw_ellipsis(r, title, strlen(title), title_x, (ui->stream_fs_top_bar.h - ui_line_h(ui, ts.size)) * 0.5f,
                             title_w, &ts);
  }

  // Bottom floating control pill
  ccord_rect bar = ui->stream_fs_bot_bar;
  ccord_draw_rrect(r, (ccord_rect){bar.x - 1.0f, bar.y - 1.0f, bar.w + 2.0f, bar.h + 2.0f}, 11.0f,
                   ui_alpha(COL(border_subtle), 0.7f));
  ccord_draw_rrect(r, bar, 10.0f, (ccord_color){0x14, 0x16, 0x1B, 232});

  ccord_text_style b1s = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
  ccord_text_style b2s = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_on_brand));
  const char *b1_lbl = self_streaming ? (ui->stream_hide_preview ? "Show Preview" : "Hide Preview")
                                      : (ws->paused ? "Resume" : "Pause");
  const char *b2_lbl = self_streaming ? "Stop Streaming" : "Stop Watching";
  float by = bar.y + (bar.h - 30.0f) * 0.5f;
  float bx = bar.x + 12.0f;
  float gap = 8.0f;

  if (self_streaming) {
    const char *plbl = st->paused ? "Resume" : "Pause";
    ccord_rect pb = {bx, by, 82.0f, 30.0f};
    ui->stream_fs_pause_rect = pb;
    bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, pb);
    ccord_draw_rrect(r, pb, 5.0f, hov ? COL(bg_item_hover) : COL(bg_tertiary));
    ccord_vec2 pt = ccord_text_measure(r, plbl, strlen(plbl), &b1s, 0.0f);
    ccord_text_draw(r, plbl, strlen(plbl), pb.x + (pb.w - pt.x) * 0.5f, pb.y + (pb.h - pt.y) * 0.5f, &b1s, 0.0f);
    bx += pb.w + gap;
  }

  float b1_w = self_streaming ? 116.0f : 96.0f;
  ccord_rect b1 = {bx, by, b1_w, 30.0f};
  ui->stream_fs_btn1_rect = b1;
  bool b1_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, b1);
  ccord_draw_rrect(r, b1, 5.0f, b1_hov ? COL(bg_item_hover) : COL(bg_tertiary));
  ccord_vec2 b1t = ccord_text_measure(r, b1_lbl, strlen(b1_lbl), &b1s, 0.0f);
  ccord_text_draw(r, b1_lbl, strlen(b1_lbl), b1.x + (b1.w - b1t.x) * 0.5f, b1.y + (b1.h - b1t.y) * 0.5f, &b1s, 0.0f);
  bx += b1.w + gap;

  float b2_w = 124.0f;
  ccord_rect b2 = {bx, by, b2_w, 30.0f};
  ui->stream_fs_btn2_rect = b2;
  bool b2_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, b2);
  ccord_draw_rrect(r, b2, 5.0f, b2_hov ? ui_mix(COL(danger), COL(bg_item_hover), 0.2f) : COL(danger));
  ccord_vec2 b2t = ccord_text_measure(r, b2_lbl, strlen(b2_lbl), &b2s, 0.0f);
  ccord_text_draw(r, b2_lbl, strlen(b2_lbl), b2.x + (b2.w - b2t.x) * 0.5f, b2.y + (b2.h - b2t.y) * 0.5f, &b2s, 0.0f);

  ccord_rect bex = {bar.x + bar.w - 12.0f - 34.0f, by, 34.0f, 30.0f};
  if (bex.x >= b2.x + b2.w + 4.0f) {
    ui->stream_fs_bot_exit_rect = bex;
    bool bex_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, bex);
    ccord_draw_rrect(r, bex, 5.0f, bex_hov ? COL(bg_item_hover) : COL(bg_tertiary));
    ui_icon_fullscreen(r, bex.x + bex.w * 0.5f, bex.y + bex.h * 0.5f, 14.0f, COL(text_header), true);
  }

  ccord_pop_alpha(r);
}

bool ui_stream_fullscreen_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ui->stream_fullscreen || !ev) return false;
  ui->stream_fs_hud_ms = ui->now_ms;
  if (ev->type != CCORD_IN_MOUSE_DOWN || ev->button != 1) return true;

  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool self_streaming = st && st->active;
  bool watching_remote = !self_streaming && ws && ws->active;

  if ((ui->stream_fs_exit_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_fs_exit_rect)) ||
      (ui->stream_fs_bot_exit_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_fs_bot_exit_rect))) {
    ui_stream_set_fullscreen(ui, false);
    return true;
  }
  if (self_streaming && ui->stream_fs_pause_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_fs_pause_rect)) {
    if (ui->d.session) {
      ccord_error err;
      (void)ccord_session_set_stream_paused(ui->d.session, !st->paused, &err);
    }
    ui->dirty = true;
    return true;
  }
  if ((self_streaming || watching_remote) && ui->stream_fs_btn1_rect.w > 0.0f &&
      ui_in_rect(ev->x, ev->y, ui->stream_fs_btn1_rect)) {
    if (self_streaming) {
      ui->stream_hide_preview = !ui->stream_hide_preview;
    } else if (ui->d.session) {
      ccord_error err;
      (void)ccord_session_set_stream_paused(ui->d.session, !ws->paused, &err);
    }
    ui->dirty = true;
    return true;
  }
  if ((self_streaming || watching_remote) && ui->stream_fs_btn2_rect.w > 0.0f &&
      ui_in_rect(ev->x, ev->y, ui->stream_fs_btn2_rect)) {
    ccord_error err;
    if (self_streaming && ui->d.session) {
      ccord_id gid = st->guild_id;
      if (gid == CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(ui->d.store, st->channel_id);
        if (ch) gid = ch->guild_id;
      }
      (void)ccord_session_stop_stream(ui->d.session, gid, &err);
    } else if (watching_remote && ui->d.session) {
      (void)ccord_session_stop_watching(ui->d.session, &err);
    }
    ui_stream_set_fullscreen(ui, false);
    return true;
  }
  if (ev->clicks >= 2) {
    ui_stream_set_fullscreen(ui, false);
    return true;
  }
  ui->dirty = true;
  return true;
}

static uint8_t pip_edge_at(const ccord_ui *ui, float x, float y)
{
  ccord_rect rc = ui->stream_pip_rect;
  if (rc.w <= 0.0f || rc.h <= 0.0f || !ui_in_rect(x, y, rc)) return 0;
  // Control buttons always win over edge/corner resize handles.
  if ((ui->stream_pip_close_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_close_rect)) ||
      (ui->stream_pip_fs_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_fs_rect)) ||
      (ui->stream_pip_stage_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_stage_rect))) {
    return 0;
  }
  const float edge = 8.0f;
  const float corner = 14.0f;
  bool nl = x <= rc.x + edge;
  bool nr = x >= rc.x + rc.w - edge;
  bool nt = y <= rc.y + edge;
  bool nb = y >= rc.y + rc.h - edge;
  if (nl || nr) {
    if (y <= rc.y + corner) nt = true;
    if (y >= rc.y + rc.h - corner) nb = true;
  }
  if (nt || nb) {
    if (x <= rc.x + corner) nl = true;
    if (x >= rc.x + rc.w - corner) nr = true;
  }
  uint8_t mask = 0;
  if (nl) mask |= PIP_EDGE_L;
  if (nr) mask |= PIP_EDGE_R;
  if (nt) mask |= PIP_EDGE_T;
  if (nb) mask |= PIP_EDGE_B;
  return mask;
}

static ccord_cursor_kind pip_cursor_for_edge(uint8_t edge)
{
  bool h = (edge & (PIP_EDGE_L | PIP_EDGE_R)) != 0;
  bool v = (edge & (PIP_EDGE_T | PIP_EDGE_B)) != 0;
  if (h && v) {
    bool nwse = ((edge & PIP_EDGE_L) && (edge & PIP_EDGE_T)) || ((edge & PIP_EDGE_R) && (edge & PIP_EDGE_B));
    return nwse ? CCORD_CURSOR_NWSE_RESIZE : CCORD_CURSOR_NESW_RESIZE;
  }
  if (h) return CCORD_CURSOR_EW_RESIZE;
  if (v) return CCORD_CURSOR_NS_RESIZE;
  return CCORD_CURSOR_MOVE;
}

ccord_cursor_kind ui_stream_pip_cursor(const ccord_ui *ui, float x, float y)
{
  if (!ui) return CCORD_CURSOR_DEFAULT;
  if (ui->drag == DRAG_STREAM_PIP_RESIZE) return pip_cursor_for_edge(ui->stream_pip_resize_edge);
  if (ui->drag == DRAG_STREAM_PIP_MOVE) return CCORD_CURSOR_MOVE;
  if (ui->stream_pip_rect.w <= 0.0f || !ui_in_rect(x, y, ui->stream_pip_rect)) return CCORD_CURSOR_DEFAULT;
  if ((ui->stream_pip_close_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_close_rect)) ||
      (ui->stream_pip_fs_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_fs_rect)) ||
      (ui->stream_pip_stage_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_stage_rect))) {
    return CCORD_CURSOR_POINTER;
  }
  uint8_t edge = pip_edge_at(ui, x, y);
  if (edge != 0) return pip_cursor_for_edge(edge);
  return CCORD_CURSOR_POINTER;
}

static void pip_bounds(const ccord_ui *ui, float aspect, float *out_min_w, float *out_max_w)
{
  float max_w = clampf(ui->win_w * 0.72f, 200.0f, 960.0f);
  float max_h = clampf(ui->win_h * 0.72f, 120.0f, 640.0f);
  if (max_w / aspect > max_h) max_w = maxf(max_h * aspect, 200.0f);
  if (max_w > ui->win_w - 16.0f) max_w = maxf(ui->win_w - 16.0f, 160.0f);
  float min_w = fminf(200.0f, max_w);
  *out_min_w = min_w;
  *out_max_w = max_w;
}

void ui_draw_stream_pip(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r) return;
  ui->stream_pip_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_pip_fs_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_pip_stage_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_pip_close_rect = (ccord_rect){0, 0, 0, 0};
  if (!ui_stream_pip_active(ui)) return;

  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool self_streaming = st && st->active;
  bool watching_remote = !self_streaming && ws && ws->active;
  if (!self_streaming && !watching_remote) return;

  int frame_w = 0, frame_h = 0;
  bool frame_present =
      ccord_render_has_texture(r, "__video_stream__", &frame_w, &frame_h) && frame_w > 0 && frame_h > 0;
  float aspect = (frame_w > 0 && frame_h > 0) ? clampf((float)frame_w / (float)frame_h, 0.5f, 2.5f) : (16.0f / 9.0f);
  ui->stream_pip_aspect = aspect;

  float min_w = 200.0f, max_w = 640.0f;
  pip_bounds(ui, aspect, &min_w, &max_w);
  float w = clampf(ui->stream_pip_w > 1.0f ? ui->stream_pip_w : 320.0f, min_w, max_w);
  float h = w / aspect;
  float px, py;
  if (!ui->stream_pip_positioned) {
    ccord_rect ma = ui_rect(ui->layout.message_area);
    px = (ma.w >= w + 24.0f) ? (ma.x + ma.w - w - 16.0f) : (ui->win_w - w - 16.0f);
    py = (ma.h >= h + 24.0f) ? (ma.y + ma.h - h - 16.0f) : (ui->win_h - h - 16.0f);
  } else {
    px = ui->stream_pip_x;
    py = ui->stream_pip_y;
  }
  px = clampf(px, 8.0f, maxf(8.0f, ui->win_w - w - 8.0f));
  py = clampf(py, 8.0f, maxf(8.0f, ui->win_h - h - 8.0f));

  ccord_rect pip = {px, py, w, h};
  ui->stream_pip_rect = pip;
  bool resizing = ui->drag == DRAG_STREAM_PIP_RESIZE;
  bool moving = ui->drag == DRAG_STREAM_PIP_MOVE;
  bool hovered = ui->mouse_in && ui_in_rect(ui->mx, ui->my, pip);

  // Subtle drop shadow + border ring + dark media plate
  ccord_draw_rrect(r, (ccord_rect){pip.x - 2.0f, pip.y + 2.0f, pip.w + 4.0f, pip.h + 4.0f}, 10.0f,
                   (ccord_color){0, 0, 0, 100});
  ccord_draw_rrect(r, (ccord_rect){pip.x - 1.5f, pip.y - 1.5f, pip.w + 3.0f, pip.h + 3.0f}, 9.5f,
                   (resizing || moving) ? COL(brand) : COL(border_subtle));
  ccord_draw_rrect(r, pip, 8.0f, (ccord_color){0x14, 0x15, 0x18, 255});

  bool paused = (self_streaming && st->paused) || (watching_remote && ws->paused);
  ccord_rect vid_bounds = {pip.x + 2.0f, pip.y + 2.0f, maxf(pip.w - 4.0f, 0.0f), maxf(pip.h - 4.0f, 0.0f)};
  if (frame_present) {
    ccord_rect vid = dc_stream_fit_aspect(vid_bounds, frame_w, frame_h);
    ccord_draw_texture_rounded(r, "__video_stream__", vid, 6.0f, paused ? 110 : 255);
    if (paused) {
      const char *pmsg = "Paused";
      ccord_text_style pms = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
      ccord_vec2 pmt = ccord_text_measure(r, pmsg, strlen(pmsg), &pms, 0.0f);
      ccord_draw_rrect(r, (ccord_rect){vid.x + (vid.w - pmt.x - 18.0f) * 0.5f, vid.y + (vid.h - 24.0f) * 0.5f,
                                       pmt.x + 18.0f, 24.0f},
                       5.0f, COL(bg_floating));
      ccord_text_draw(r, pmsg, strlen(pmsg), vid.x + (vid.w - pmt.x) * 0.5f, vid.y + (vid.h - pmt.y) * 0.5f, &pms, 0.0f);
    }
  } else {
    const char *pmsg = paused ? "Stream Paused" : (self_streaming ? "Starting stream..." : "Connecting...");
    ccord_text_style phs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
    ccord_vec2 pht = ccord_text_measure(r, pmsg, strlen(pmsg), &phs, 0.0f);
    ccord_text_draw(r, pmsg, strlen(pmsg), pip.x + (pip.w - pht.x) * 0.5f, pip.y + (pip.h - pht.y) * 0.5f, &phs, 0.0f);
  }

  // Top translucent overlay bar with LIVE badge, streamer name, Return-to-Stage, Fullscreen, and Close buttons.
  ccord_rect top = {pip.x + 2.0f, pip.y + 2.0f, maxf(pip.w - 4.0f, 0.0f), 30.0f};
  ccord_draw_rrect(r, top, 6.0f, (ccord_color){0x0E, 0x10, 0x14, (uint8_t)(hovered ? 210 : 145)});

  const char *badge = paused ? "PAUSED" : (frame_present ? "LIVE" : "...");
  ccord_color badge_bg = (!paused && frame_present) ? COL(danger) : COL(bg_tertiary);
  ccord_color badge_fg = (!paused && frame_present) ? COL(text_on_brand) : COL(text_header);
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 5.0f, badge_fg);
  ccord_vec2 lt = ccord_text_measure(r, badge, strlen(badge), &ls, 0.0f);
  ccord_rect lb = {top.x + 6.0f, top.y + 6.0f, lt.x + 10.0f, 16.0f};
  ccord_draw_rrect(r, lb, 3.0f, badge_bg);
  ccord_text_draw(r, badge, strlen(badge), lb.x + (lb.w - lt.x) * 0.5f, lb.y + (lb.h - lt.y) * 0.5f, &ls, 0.0f);

  ccord_rect close_b = {top.x + top.w - 6.0f - 22.0f, top.y + 4.0f, 22.0f, 22.0f};
  ccord_rect fs_b = {close_b.x - 4.0f - 22.0f, top.y + 4.0f, 22.0f, 22.0f};
  ccord_rect stage_b = {fs_b.x - 4.0f - 22.0f, top.y + 4.0f, 22.0f, 22.0f};
  ui->stream_pip_close_rect = close_b;
  ui->stream_pip_fs_rect = fs_b;
  ui->stream_pip_stage_rect = stage_b;

  bool chov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, close_b);
  bool fhov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, fs_b);
  bool shov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, stage_b);
  ccord_draw_rrect(r, stage_b, 4.0f, shov ? COL(bg_item_hover) : (ccord_color){0x26, 0x29, 0x30, 200});
  ui_icon_stage_return(r, stage_b.x + stage_b.w * 0.5f, stage_b.y + stage_b.h * 0.5f, 11.0f, COL(text_header));
  ccord_draw_rrect(r, fs_b, 4.0f, fhov ? COL(bg_item_hover) : (ccord_color){0x26, 0x29, 0x30, 200});
  ui_icon_fullscreen(r, fs_b.x + fs_b.w * 0.5f, fs_b.y + fs_b.h * 0.5f, 11.0f, COL(text_header), false);
  ccord_draw_rrect(r, close_b, 4.0f, chov ? COL(danger) : (ccord_color){0x26, 0x29, 0x30, 200});
  ui_icon_close(r, close_b.x + close_b.w * 0.5f, close_b.y + close_b.h * 0.5f, 11.0f,
                chov ? COL(text_on_brand) : COL(text_header));

  const char *who = "Your Stream";
  if (watching_remote) {
    const ccord_user *wu = ccord_store_user(ui->d.store, ws->user_id);
    who = wu ? wu->name : "Live Stream";
  }
  float name_x = lb.x + lb.w + 6.0f;
  float name_w = stage_b.x - name_x - 6.0f;
  if (name_w > 20.0f) {
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
    ccord_text_draw_ellipsis(r, who, strlen(who), name_x, top.y + (top.h - ui_line_h(ui, ns.size)) * 0.5f, name_w, &ns);
  }

  // Subtle corner resize grip ticks on hover or during resize.
  if (hovered || resizing) {
    ccord_color gc = resizing ? COL(brand) : ui_alpha(COL(text_header), 0.75f);
    float rx = pip.x + pip.w - 4.0f, ry = pip.y + pip.h - 4.0f;
    ccord_draw_line(r, (ccord_vec2){rx - 8.0f, ry}, (ccord_vec2){rx, ry - 8.0f}, 1.5f, gc);
    ccord_draw_line(r, (ccord_vec2){rx - 4.0f, ry}, (ccord_vec2){rx, ry - 4.0f}, 1.5f, gc);
  }
}

bool ui_stream_pip_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || ev->type != CCORD_IN_MOUSE_DOWN || ev->button != 1) return false;
  if (!ui_stream_pip_active(ui) || ui->stream_pip_rect.w <= 0.0f) return false;
  if (!ui_in_rect(ev->x, ev->y, ui->stream_pip_rect)) return false;

  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool self_streaming = st && st->active;
  bool watching_remote = !self_streaming && ws && ws->active;

  if (ui->stream_pip_close_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_pip_close_rect)) {
    if (self_streaming) {
      ui->stream_hide_preview = true;
    } else if (watching_remote && ui->d.session) {
      ccord_error err;
      (void)ccord_session_stop_watching(ui->d.session, &err);
    }
    ui->dirty = true;
    return true;
  }
  if (ui->stream_pip_fs_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_pip_fs_rect)) {
    ui_stream_set_fullscreen(ui, true);
    return true;
  }
  if (ui->stream_pip_stage_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_pip_stage_rect)) {
    stream_open_stage_for_active(ui);
    return true;
  }

  uint8_t edge = pip_edge_at(ui, ev->x, ev->y);
  if (edge != 0) {
    if (ev->clicks >= 2) {
      ui->stream_pip_w = 320.0f;
      ui->stream_pip_positioned = false;
      ui_settings_save(ui);
      ui->dirty = true;
      return true;
    }
    ui->stream_pip_resize_edge = edge;
    ui->stream_pip_anchor = ui->stream_pip_rect;
    ui->stream_pip_drag_ox = ev->x;
    ui->stream_pip_drag_oy = ev->y;
    ui->stream_pip_positioned = true;
    ui->stream_pip_x = ui->stream_pip_rect.x;
    ui->stream_pip_y = ui->stream_pip_rect.y;
    ui->drag = DRAG_STREAM_PIP_RESIZE;
    ui->dirty = true;
    return true;
  }

  if (ev->clicks >= 2) {
    ui_stream_set_fullscreen(ui, true);
    return true;
  }
  ui->stream_pip_anchor = ui->stream_pip_rect;
  ui->stream_pip_drag_ox = ev->x;
  ui->stream_pip_drag_oy = ev->y;
  ui->stream_pip_moved = false;
  ui->drag = DRAG_STREAM_PIP_MOVE;
  ui->dirty = true;
  return true;
}

void ui_stream_pip_drag_move(ccord_ui *ui, float x, float y)
{
  if (!ui) return;
  if (ui->drag == DRAG_STREAM_PIP_MOVE) {
    float dx = x - ui->stream_pip_drag_ox;
    float dy = y - ui->stream_pip_drag_oy;
    if (!ui->stream_pip_moved && (fabsf(dx) >= 4.0f || fabsf(dy) >= 4.0f)) ui->stream_pip_moved = true;
    if (ui->stream_pip_moved) {
      ui->stream_pip_positioned = true;
      ui->stream_pip_x = clampf(ui->stream_pip_anchor.x + dx, 8.0f, maxf(8.0f, ui->win_w - ui->stream_pip_anchor.w - 8.0f));
      ui->stream_pip_y = clampf(ui->stream_pip_anchor.y + dy, 8.0f, maxf(8.0f, ui->win_h - ui->stream_pip_anchor.h - 8.0f));
      ui->dirty = true;
    }
    return;
  }
  if (ui->drag == DRAG_STREAM_PIP_RESIZE) {
    float aspect = ui->stream_pip_aspect > 0.1f ? ui->stream_pip_aspect : (16.0f / 9.0f);
    float min_w = 200.0f, max_w = 640.0f;
    pip_bounds(ui, aspect, &min_w, &max_w);
    uint8_t mask = ui->stream_pip_resize_edge;
    ccord_rect a = ui->stream_pip_anchor;
    float dx = x - ui->stream_pip_drag_ox;
    float dy = y - ui->stream_pip_drag_oy;
    bool has_x = (mask & (PIP_EDGE_L | PIP_EDGE_R)) != 0;
    bool has_y = (mask & (PIP_EDGE_T | PIP_EDGE_B)) != 0;
    float wx = a.w + ((mask & PIP_EDGE_R) ? dx : -dx);
    float hy = a.h + ((mask & PIP_EDGE_B) ? dy : -dy);
    float wy = hy * aspect;
    float new_w = a.w;
    if (has_x && has_y) {
      new_w = fabsf(wx - a.w) >= fabsf(wy - a.w) ? wx : wy;
    } else if (has_x) {
      new_w = wx;
    } else if (has_y) {
      new_w = wy;
    }
    new_w = clampf(new_w, min_w, max_w);
    float new_h = new_w / aspect;
    float new_x = (mask & PIP_EDGE_L) ? (a.x + a.w - new_w) : a.x;
    float new_y = (mask & PIP_EDGE_T) ? (a.y + a.h - new_h) : a.y;
    new_x = clampf(new_x, 8.0f, maxf(8.0f, ui->win_w - new_w - 8.0f));
    new_y = clampf(new_y, 8.0f, maxf(8.0f, ui->win_h - new_h - 8.0f));
    ui->stream_pip_w = new_w;
    ui->stream_pip_x = new_x;
    ui->stream_pip_y = new_y;
    ui->stream_pip_positioned = true;
    ui->dirty = true;
  }
}

void ui_stream_pip_drag_end(ccord_ui *ui)
{
  if (!ui) return;
  if (ui->drag == DRAG_STREAM_PIP_MOVE) {
    bool was_click = !ui->stream_pip_moved;
    ui->drag = DRAG_NONE;
    if (was_click) stream_open_stage_for_active(ui);
    ui->dirty = true;
  } else if (ui->drag == DRAG_STREAM_PIP_RESIZE) {
    ui->drag = DRAG_NONE;
    ui_settings_save(ui);
    ui->dirty = true;
  }
}

// ---------------------------------------------------------------- Go Live modal

static ccord_rect go_live_tab_rect(const ccord_rect *m, int tab)
{
  return (ccord_rect){m->x + 24.0f + (float)tab * 136.0f, m->y + 62.0f, 128.0f, 28.0f};
}

static ccord_rect go_live_source_card_rect(const ccord_rect *m, size_t idx)
{
  float grid_x = m->x + 24.0f, grid_y = m->y + 98.0f;
  float grid_w = m->w - 48.0f;
  float gap = 12.0f;
  float card_w = (grid_w - gap * 2.0f) / 3.0f;
  float card_h = 102.0f;
  size_t col = idx % 3u, row = idx / 3u;
  return (ccord_rect){grid_x + (float)col * (card_w + gap), grid_y + (float)row * (card_h + gap), card_w, card_h};
}

static void refresh_go_live_sources(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !ui->go_live_sources_dirty) return;
  ui->go_live_sources_dirty = false;
  ccord_capture_kind kind = (ui->go_live_tab == 1) ? CCORD_CAPTURE_SCREEN : CCORD_CAPTURE_WINDOW;
  ccord_error err;
  size_t count = ccord_capture_list(CCORD_CAPBE_AUTO, kind, ui->go_live_sources, 6, &err);
  if (count == 0) {
    count = ccord_capture_list(CCORD_CAPBE_FAKE, kind, ui->go_live_sources, 6, &err);
  }
  ui->go_live_source_count = count;
  if (ui->go_live_selected_source >= count) ui->go_live_selected_source = 0;

  if (!r || count == 0) return;
  enum { TH_W = 192, TH_H = 108 };
  uint8_t *rgba = ccord_malloc((size_t)TH_W * (size_t)TH_H * 4u);
  if (!rgba) return;
  ccord_capture_source fake_fallbacks[6];
  size_t nfake = ccord_capture_list(CCORD_CAPBE_FAKE, kind, fake_fallbacks, 6, &err);
  for (size_t i = 0; i < count; i++) {
    int tw = 0, th = 0;
    bool ok = ccord_capture_thumbnail(&ui->go_live_sources[i], TH_W, TH_H, rgba, &tw, &th, &err);
    if (!ok && nfake > 0) {
      ok = ccord_capture_thumbnail(&fake_fallbacks[i % nfake], TH_W, TH_H, rgba, &tw, &th, &err);
    }
    if (ok && tw > 0 && th > 0) {
      char key[48];
      snprintf(key, sizeof key, "__cap_thumb_%zu__", i);
      (void)ccord_render_upload_rgba(r, key, rgba, tw, th, &err);
    }
  }
  ccord_free(rgba);
}

void ui_draw_go_live(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui->go_live_open) return;
  refresh_go_live_sources(ui, r);
  ccord_rect m = ui_go_live_rect(ui);
  uis_modal_frame(ui, r, m);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_draw(r, "Screen Share", 12, m.x + 24.0f, m.y + 16.0f, &ts, 0.0f);
  const ccord_voice_state *v = ui_self_voice(ui);
  const ccord_channel *ch = v ? ccord_store_channel(ui->d.store, v->channel_id) : nullptr;
  char sub[160];
  snprintf(sub, sizeof sub, "Select an application window or screen to share to %s", ch ? ch->name : "your voice channel");
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  ccord_text_draw_ellipsis(r, sub, strlen(sub), m.x + 24.0f, m.y + 39.0f, m.w - 72.0f, &ss);
  {
    ccord_rect cr = ui_go_live_close_rect(ui);
    ccord_color cg = uis_icon_button(ui, r, cr, (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cr)}, "glclose", 0);
    ui_icon_close(r, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f, 14.0f, cg);
  }
  uis_tokens gtk = uis_tokens_of(ui);

  // Source Picker Tabs: Applications | Screens
  static const char *const k_tabs[2] = {"Applications", "Screens"};
  for (int t = 0; t < 2; t++) {
    ccord_rect tr = go_live_tab_rect(&m, t);
    bool active = (ui->go_live_tab == (uint8_t)t);
    uis_tab(ui, r, tr, k_tabs[t], (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, tr), .selected = active},
            "gltab", (uint64_t)t);
  }

  // Grid of live source cards with thumbnails
  for (size_t i = 0; i < ui->go_live_source_count && i < 6; i++) {
    ccord_rect card = go_live_source_card_rect(&m, i);
    bool sel = (i == ui->go_live_selected_source);
    if (sel) {
      ccord_draw_rrect(r, (ccord_rect){card.x - 2.0f, card.y - 2.0f, card.w + 4.0f, card.h + 4.0f}, 8.0f, COL(brand));
    }
    uis_plate(ui, r, card, gtk.r_control, COL(bg_tertiary));
    uis_list_row_fill(ui, r, card, (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, card), .selected = sel},
                      "glsrc", (uint64_t)i);
    ccord_rect thumb = {card.x + 6.0f, card.y + 6.0f, card.w - 12.0f, 68.0f};
    char key[48];
    snprintf(key, sizeof key, "__cap_thumb_%zu__", i);
    if (ccord_render_has_texture(r, key, nullptr, nullptr)) {
      ccord_draw_texture_rounded(r, key, thumb, 4.0f, 255);
    } else {
      ccord_draw_rrect(r, thumb, 4.0f, COL(bg_primary));
    }
    const char *title = ui->go_live_sources[i].title[0] ? ui->go_live_sources[i].title : "Window";
    ccord_text_style cts = ui_style(ui, sel ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small) - 2.0f,
                                    sel ? COL(text_header) : COL(text_secondary));
    ccord_text_draw_ellipsis(r, title, strlen(title), card.x + 8.0f, card.y + 78.0f, card.w - 16.0f, &cts);
  }

  // Stream Quality Box (Resolution & Frame Rate ONLY — encoder/codec/framework are in Settings)
  ccord_rect qbox = {m.x + 24.0f, m.y + 320.0f, m.w - 48.0f, 86.0f};
  uis_plate(ui, r, qbox, gtk.r_control, COL(bg_primary));
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
  ccord_text_draw(r, "RESOLUTION", 10, m.x + 36.0f, m.y + 338.0f, &ls, 0.0f);
  ccord_text_draw(r, "FRAME RATE", 10, m.x + 36.0f, m.y + 376.0f, &ls, 0.0f);

  for (int cat = 3; cat <= 4; cat++) {
    int count = ui_go_live_option_count(cat);
    int cur = (cat == 3) ? ui->gl_resolution : ui->gl_fps;
    for (int opt = 0; opt < count; opt++) {
      ccord_rect o = ui_go_live_option_rect(ui, cat, opt);
      bool sel = (cur == opt);
      uis_tab(ui, r, o, (cat == 3) ? dc_resolution_label(opt) : dc_fps_label(opt),
              (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, o), .selected = sel}, "glopt",
              (uint64_t)(cat * 16 + opt));
    }
  }

  // Share System Audio: the broadcast carries the desktop/monitor loopback mix instead of the
  // mic downmix (app_media reads the toggle through ccord_ui_go_live_share_audio).
  ccord_rect ar = ui_go_live_audio_rect(ui);
  ccord_rect cb = {ar.x, ar.y + (ar.h - 18.0f) * 0.5f, 18.0f, 18.0f};
  bool ahov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ar);
  ccord_draw_rrect(r, cb, 4.0f, ui->go_live_share_audio ? COL(brand) : COL(bg_tertiary));
  if (ui->go_live_share_audio) {
    ccord_draw_line(r, (ccord_vec2){cb.x + 4.0f, cb.y + 9.5f}, (ccord_vec2){cb.x + 7.5f, cb.y + 13.0f}, 2.0f,
                    COL(text_on_brand));
    ccord_draw_line(r, (ccord_vec2){cb.x + 7.5f, cb.y + 13.0f}, (ccord_vec2){cb.x + 14.0f, cb.y + 5.5f}, 2.0f,
                    COL(text_on_brand));
  }
  ccord_text_style als = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small),
                                  ahov ? COL(text_header) : COL(text_secondary));
  ccord_vec2 alt = ccord_text_measure(r, "Share System Audio", 18, &als, 0.0f);
  ccord_text_draw(r, "Share System Audio", 18, cb.x + cb.w + 10.0f,
                  ar.y + (ar.h - alt.y) * 0.5f, &als, 0.0f);
  ccord_text_style ahs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
  float hint_x = cb.x + cb.w + 10.0f + alt.x + 10.0f;
  const char *hint = "desktop / monitor loopback";
  ccord_text_draw_ellipsis(r, hint, strlen(hint), hint_x,
                           ar.y + (ar.h - ui_line_h(ui, ahs.size)) * 0.5f, ar.x + ar.w - hint_x, &ahs);

  // Footer: selected source summary + Go Live / Stop Stream button
  ccord_rect start = ui_go_live_start_rect(ui);
  if (ui->go_live_selected_source < ui->go_live_source_count) {
    char sel_lbl[192];
    snprintf(sel_lbl, sizeof sel_lbl, "Selected: %s", ui->go_live_sources[ui->go_live_selected_source].title);
    ccord_text_draw_ellipsis(r, sel_lbl, strlen(sel_lbl), m.x + 24.0f, start.y + 7.0f, start.x - (m.x + 36.0f), &ss);
  }
  bool live = ccord_store_stream_active(ui->d.store);
  uis_button(ui, r, start, live ? "Stop Stream" : "Go Live", live ? UIS_BTN_DANGER : UIS_BTN_PRIMARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, start)}, "glstart", 0);
}

bool ui_go_live_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->go_live_open) return false;
  if (ev->type != CCORD_IN_MOUSE_DOWN || ev->button != 1) return true;
  ccord_rect m = ui_go_live_rect(ui);
  if (ui_in_rect(ev->x, ev->y, ui_go_live_close_rect(ui)) || !ui_in_rect(ev->x, ev->y, m)) {
    ui->go_live_open = false;
    ui->dirty = true;
    return true;
  }
  for (int t = 0; t < 2; t++) {
    if (ui_in_rect(ev->x, ev->y, go_live_tab_rect(&m, t))) {
      if (ui->go_live_tab != (uint8_t)t) {
        ui->go_live_tab = (uint8_t)t;
        ui->go_live_selected_source = 0;
        ui->go_live_sources_dirty = true;
        ui->dirty = true;
      }
      return true;
    }
  }
  for (size_t i = 0; i < ui->go_live_source_count && i < 6; i++) {
    if (ui_in_rect(ev->x, ev->y, go_live_source_card_rect(&m, i))) {
      ui->go_live_selected_source = i;
      ui->dirty = true;
      return true;
    }
  }
  for (int cat = 0; cat < 5; cat++) {
    int count = ui_go_live_option_count(cat);
    for (int opt = 0; opt < count; opt++) {
      if (!ui_in_rect(ev->x, ev->y, ui_go_live_option_rect(ui, cat, opt))) continue;
      int *slot = cat == 0 ? &ui->gl_encoder : cat == 1 ? &ui->gl_codec : cat == 2 ? &ui->gl_framework
                 : cat == 3 ? &ui->gl_resolution : &ui->gl_fps;
      *slot = opt;
      ui->dirty = true;
      return true;
    }
  }
  if (ui_in_rect(ev->x, ev->y, ui_go_live_audio_rect(ui))) {
    ui->go_live_share_audio = !ui->go_live_share_audio;
    ui->dirty = true;
    return true;
  }
  if (ui_in_rect(ev->x, ev->y, ui_go_live_start_rect(ui))) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return true;
    }
    const ccord_voice_state *v = ui_self_voice(ui);
    const ccord_channel *ch = v ? ccord_store_channel(ui->d.store, v->channel_id) : nullptr;
    if (!ch) {
      ui_notice(ui, "Join a voice channel first");
      return true;
    }
    ccord_error err;
    if (ccord_store_stream_active(ui->d.store)) {
      ui->has_active_capture_source = false;
      if (ccord_session_stop_stream(ui->d.session, ch->guild_id, &err) != CCORD_OK) {
        ui_notice(ui, "Cannot stop stream: %s", err.message);
        return true;
      }
    } else {
      if (ui->go_live_selected_source < ui->go_live_source_count) {
        ui->active_capture_source = ui->go_live_sources[ui->go_live_selected_source];
        ui->has_active_capture_source = true;
      }
      ui->stream_hide_preview = false;
      ccord_stream_config cfg = {
        .encoder = (ccord_stream_encoder)ui->gl_encoder,
        .codec = (ccord_stream_codec)ui->gl_codec,
        .framework = (ccord_stream_framework)ui->gl_framework,
        .resolution = (ccord_stream_resolution)ui->gl_resolution,
        .fps = (ccord_stream_fps)ui->gl_fps,
      };
      if (ccord_session_start_stream(ui->d.session, ch->guild_id, ch->id, &cfg, &err) != CCORD_OK) {
        ui_notice(ui, "Cannot go live: %s", err.message);
        return true;
      }
      ui->stage_channel = ch->id;
      ui->stage_armed = true;
    }
    ui->go_live_open = false;
    ui->dirty = true;
  }
  return true;
}

