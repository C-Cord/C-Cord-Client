// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

// ---------------------------------------------------------------- voice / stream stage

static const char *encoder_label(int v)
{
  static const char *const names[] = {
    "NVENC", "VAAPI", "AMF", "Vulkan", "OpenH264", "x264", "Auto", "Quick Sync", "Media Found.", "VideoToolbox",
  };
  return v >= 0 && v < 10 ? names[v] : "Auto";
}

static const char *codec_label(int v)
{
  static const char *const names[] = {"H.264", "AV1"};
  return v >= 0 && v < 2 ? names[v] : "?";
}

[[maybe_unused]] static const char *framework_label(int v)
{
  static const char *const names[] = {
    "PipeWire", "DMA-BUF", "NVFBC", "Vulkan", "X11", "Auto", "Win. Capture", "DXGI Dup", "GDI", "ScreenCaptureKit", "CoreGraphics",
  };
  return v >= 0 && v < 11 ? names[v] : "Auto";
}

const char *dc_resolution_label(int v)
{
  static const char *const names[] = {"720p", "1080p", "1440p", "Source"};
  return v >= 0 && v < 4 ? names[v] : "?";
}

const char *dc_fps_label(int v)
{
  static const char *const names[] = {"15 FPS", "30 FPS", "60 FPS"};
  return v >= 0 && v < 3 ? names[v] : "?";
}

// ---- M5 stage-view controls (m5-ui-surfaces) ----------------------------------------------------
// Owner-tracked static state: struct ccord_ui is owned by ui.c and cannot grow fields. The rects are
// rebuilt every draw; clicks are matched in ui_voice_stage_handle. Nothing here writes Discord state
// locally — status/topic/speaker actions are session intents, volume/local-mute are client prefs.
typedef struct ui_stage_state {
  const ccord_ui *owner;
  ccord_rect status_edit, topic_edit, request_speak;
  ccord_rect stage_start, stage_end;   // lifecycle chips (zero when not offered)
  bool stage_active;                   // an active stage instance exists
  bool request_speak_cancel;
  size_t speaker_count, audience_count;
  size_t ctl_count;
  struct {
    ccord_id user_id;
    ccord_rect tile, slider, mute, mod;
    bool mod_is_invite;
  } ctl[UI_STAGE_CTL_MAX];
  size_t stream_cards;
  struct {
    ccord_id user_id;
    uint32_t viewers;
    ccord_rect rect, watch;
    bool watching;
  } card[UI_STREAM_CARD_MAX];
  char stream_badge[16];
  char stream_title[192];
  char stream_placeholder[64];
  bool stream_frame_present;
  int stream_frame_w, stream_frame_h;
  int cam_cols;
  size_t cam_tile_count;
  struct {
    ccord_id user_id;
    ccord_rect rect, video;
    char key[40];
    bool textured;
    int tex_w, tex_h;
    bool limit, connecting, self;
  } cam_tile[UI_CAM_TILE_MAX];
  ccord_rect stream_player, stage_view;
  ccord_rect stream_resize_handle, stream_fs_btn, stream_vport;
  ccord_rect mob_mute, mob_deaf, mob_cam, mob_stream, mob_leave;
} ui_stage_state;

static ui_stage_state g_stage;

// What stage_tile needs to draw a camera tile.
typedef struct ui_cam_tile_info {
  ccord_rect video;
  char key[40];
  bool textured, limit, self;
} ui_cam_tile_info;

static ui_stage_state *stage_state(const ccord_ui *ui)
{
  if (g_stage.owner != ui) {
    memset(&g_stage, 0, sizeof g_stage);
    g_stage.owner = ui;
  }
  return &g_stage;
}

void ui_stage_get_debug(const ccord_ui *ui, ui_stage_debug *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!ui) return;
  const ui_stage_state *st = stage_state(ui);
  out->status_edit = st->status_edit;
  out->topic_edit = st->topic_edit;
  out->request_speak = st->request_speak;
  out->request_speak_cancel = st->request_speak_cancel;
  out->speaker_count = st->speaker_count;
  out->audience_count = st->audience_count;
  out->ctl_count = st->ctl_count;
  for (size_t i = 0; i < st->ctl_count && i < UI_STAGE_CTL_MAX; i++) {
    out->ctl[i].user_id = st->ctl[i].user_id;
    out->ctl[i].slider = st->ctl[i].slider;
    out->ctl[i].mute = st->ctl[i].mute;
    out->ctl[i].mod = st->ctl[i].mod;
    out->ctl[i].mod_is_invite = st->ctl[i].mod_is_invite;
  }
  out->stream_cards = st->stream_cards;
  for (size_t i = 0; i < st->stream_cards && i < UI_STREAM_CARD_MAX; i++) {
    out->card[i].user_id = st->card[i].user_id;
    out->card[i].viewers = st->card[i].viewers;
    out->card[i].watch = st->card[i].watch;
    out->card[i].watching = st->card[i].watching;
  }
  memcpy(out->stream_badge, st->stream_badge, sizeof out->stream_badge);
  memcpy(out->stream_title, st->stream_title, sizeof out->stream_title);
  memcpy(out->stream_placeholder, st->stream_placeholder, sizeof out->stream_placeholder);
  out->stream_frame_present = st->stream_frame_present;
  out->stream_frame_w = st->stream_frame_w;
  out->stream_frame_h = st->stream_frame_h;
  out->cam_cols = st->cam_cols;
  out->cam_tile_count = st->cam_tile_count;
  for (size_t i = 0; i < st->cam_tile_count && i < UI_CAM_TILE_MAX; i++) {
    out->cam_tile[i].user_id = st->cam_tile[i].user_id;
    out->cam_tile[i].rect = st->cam_tile[i].rect;
    out->cam_tile[i].video = st->cam_tile[i].video;
    memcpy(out->cam_tile[i].key, st->cam_tile[i].key, sizeof out->cam_tile[i].key);
    out->cam_tile[i].textured = st->cam_tile[i].textured;
    out->cam_tile[i].tex_w = st->cam_tile[i].tex_w;
    out->cam_tile[i].tex_h = st->cam_tile[i].tex_h;
    out->cam_tile[i].limit = st->cam_tile[i].limit;
    out->cam_tile[i].connecting = st->cam_tile[i].connecting;
    out->cam_tile[i].self = st->cam_tile[i].self;
  }
  out->stream_player = st->stream_player;
  out->stage_view = st->stage_view;
  out->stream_resize_handle = st->stream_resize_handle;
  out->stream_fs_btn = st->stream_fs_btn;
  out->stream_vport = st->stream_vport;
  out->stage_split_handle = ui_stage_split_handle_rect(ui);
  out->stream_fullscreen = ui->stream_fullscreen;
  out->stream_fs_exit_btn = ui->stream_fs_exit_rect;
  out->stream_fs_video = ui->stream_fs_video_rect;
  out->stream_pip_visible = ui->stream_pip_rect.w > 0.0f && ui->stream_pip_rect.h > 0.0f;
  out->stream_pip_rect = ui->stream_pip_rect;
  out->stream_pip_fs_btn = ui->stream_pip_fs_rect;
  out->stream_pip_stage_btn = ui->stream_pip_stage_rect;
  out->stream_pip_close_btn = ui->stream_pip_close_rect;
}

void ui_stage_lifecycle_get_debug(const ccord_ui *ui, ui_stage_lifecycle_debug *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!ui) return;
  const ui_stage_state *st = stage_state(ui);
  out->start_stage = st->stage_start;
  out->end_stage = st->stage_end;
  out->stage_active = st->stage_active;
}

static void stage_tile(ccord_ui *ui, ccord_renderer *r, const ccord_voice_state *vs, ccord_rect tile,
                       bool watching_remote, const ccord_watched_stream *ws, bool is_stage, bool can_mod,
                       ccord_rect clip, const ui_cam_tile_info *cam)
{
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  bool is_self = vs->user_id == self_id;
  bool record = tile.y >= clip.y && tile.y + tile.h <= clip.y + clip.h;
  bool speaking = ccord_store_user_speaking(ui->d.store, vs->user_id) && !vs->self_mute && !vs->suppress;
  // Camera tile: 6 px padding, a 16:9 video area, then the 82 px footer at dy = tile_h - 150.
  float dy = cam ? tile.h - 150.0f : 0.0f;
  float ty = tile.y + dy;
  if (speaking && !cam) {
    ccord_draw_rrect(r, (ccord_rect){tile.x - 2.0f, tile.y - 2.0f, tile.w + 4.0f, tile.h + 4.0f}, 8.0f,
                     COL(status_online));
  }
  ccord_color bg = is_self ? COL(bg_item_selected) : COL(bg_secondary);
  ccord_draw_rrect(r, tile, 6.0f, bg);
  const ccord_user *u = ccord_store_user(ui->d.store, vs->user_id);
  if (cam) {
    ccord_rect vid = cam->video;
    if (speaking) {
      ccord_draw_rrect(r, (ccord_rect){vid.x - 2.0f, vid.y - 2.0f, vid.w + 4.0f, vid.h + 4.0f}, 8.0f,
                       COL(status_online));
    }
    if (cam->textured) {
      ccord_draw_texture_cover(r, cam->key, vid, 6.0f, 255, false);
    } else {
      ccord_draw_rrect(r, vid, 6.0f, COL(bg_tertiary));
      float acy = vid.y + vid.h * 0.5f - 8.0f;
      ui_draw_avatar(ui, r, u, vid.x + vid.w * 0.5f, acy, 52.0f, (ccord_color){0, 0, 0, 0}, false);
      const char *msg = cam->self ? "Your camera is on" : (cam->limit ? "Video limit (4)" : "Connecting video\xE2\x80\xA6");
      ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
      ccord_vec2 cm = ccord_text_measure(r, msg, strlen(msg), &cs, 0.0f);
      ccord_text_draw(r, msg, strlen(msg), vid.x + (vid.w - cm.x) * 0.5f, acy + 32.0f, &cs, 0.0f);
    }
  } else if (vs->self_stream && ccord_render_has_texture(r, "__video_stream__", nullptr, nullptr) &&
      (is_self || (watching_remote && ws->user_id == vs->user_id))) {
    ccord_rect preview = {tile.x + 6.0f, tile.y + 6.0f, tile.w - 12.0f, 62.0f};
    ccord_draw_texture_rounded(r, "__video_stream__", preview, 4.0f, 255);
  } else {
    if (speaking) ccord_draw_circle(r, tile.x + tile.w * 0.5f, tile.y + 38.0f, 26.0f + 2.5f, COL(status_online));
    ui_draw_avatar(ui, r, u, tile.x + tile.w * 0.5f, tile.y + 38.0f, 52.0f, (ccord_color){0, 0, 0, 0}, false);
  }
  const char *name = u ? u->name : "Unknown User";
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_draw_ellipsis(r, name, strlen(name), tile.x + 10.0f, ty + 70.0f, tile.w - 20.0f, &ns);
  if (vs->self_stream) {
    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 5.0f, COL(text_on_brand));
    ccord_rect lb = {tile.x + tile.w - 46.0f, tile.y + 8.0f, 38.0f, 15.0f};
    ccord_draw_rrect(r, lb, 3.0f, COL(danger));
    ccord_text_draw(r, "LIVE", 4, lb.x + 6.0f, lb.y + 2.0f, &ls, 0.0f);
  }

  ui_stage_state *st = stage_state(ui);
  ccord_rect mod = {0, 0, 0, 0}, mute = {0, 0, 0, 0}, slider = {0, 0, 0, 0};
  bool mod_is_invite = false;
  // Row 3: stage speaker management, else the watch-stream button for a remote broadcaster.
  if (is_stage && can_mod && !is_self) {
    mod_is_invite = vs->suppress;
    const char *mlbl = mod_is_invite ? "Invite to Speak" : "Move to Audience";
    mod = (ccord_rect){tile.x + 10.0f, ty + 92.0f, tile.w - 20.0f, 22.0f};
    bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mod);
    ccord_draw_rrect(r, mod, 4.0f, hov ? ui_mix(COL(brand), COL(bg_item_hover), 0.25f) : COL(brand));
    ccord_text_style ms = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, COL(text_on_brand));
    ccord_vec2 mt = ccord_text_measure(r, mlbl, strlen(mlbl), &ms, 0.0f);
    ccord_text_draw(r, mlbl, strlen(mlbl), mod.x + (mod.w - mt.x) * 0.5f, mod.y + (mod.h - mt.y) * 0.5f, &ms, 0.0f);
  } else if (vs->self_stream && !is_self) {
    bool is_watched = watching_remote && ws->user_id == vs->user_id;
    mod = (ccord_rect){tile.x + 10.0f, ty + 92.0f, tile.w - 20.0f, 22.0f};
    ccord_draw_rrect(r, mod, 4.0f, is_watched ? COL(bg_tertiary) : COL(brand));
    const char *wlbl = is_watched ? "Watching Stream" : "Watch Stream";
    ccord_text_style wst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f,
                                    is_watched ? COL(text_secondary) : COL(text_on_brand));
    ccord_vec2 wt = ccord_text_measure(r, wlbl, strlen(wlbl), &wst, 0.0f);
    ccord_text_draw(r, wlbl, strlen(wlbl), mod.x + (mod.w - wt.x) * 0.5f, mod.y + (mod.h - wt.y) * 0.5f, &wst, 0.0f);
    if (record && ui->stage_watch_count < 16) {
      ui->stage_watch_rects[ui->stage_watch_count] = mod;
      ui->stage_watch_users[ui->stage_watch_count] = vs->user_id;
      ui->stage_watch_count++;
    }
  } else {
    ccord_text_style sts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 4.0f,
                                    speaking ? COL(status_online) : COL(text_muted));
    const char *state = vs->suppress ? "Audience"
                                     : (vs->self_mute ? "Muted" : (speaking ? "Speaking" : "Connected"));
    ccord_text_draw_ellipsis(r, state, strlen(state), tile.x + 10.0f, ty + 94.0f, tile.w - 20.0f, &sts);
  }
  // Row 4: local playback prefs for everyone but self (client-only, never Discord state).
  if (!is_self) {
    bool lm = ccord_store_user_local_mute(ui->d.store, vs->user_id);
    mute = (ccord_rect){tile.x + 10.0f, ty + 120.0f, 24.0f, 22.0f};
    bool mhov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mute);
    ccord_draw_rrect(r, mute, 4.0f, mhov ? COL(bg_item_hover) : COL(bg_tertiary));
    ui_icon_speaker(r, mute.x + mute.w * 0.5f, mute.y + mute.h * 0.5f, 14.0f,
                    lm ? COL(danger) : COL(text_secondary));
    if (lm) {
      ccord_draw_rect(r, (ccord_rect){mute.x + 4.0f, mute.y + mute.h * 0.5f - 1.0f, mute.w - 8.0f, 2.0f}, COL(danger));
    }
    slider = (ccord_rect){tile.x + 40.0f, ty + 126.0f, tile.w - 92.0f, 10.0f};
    uint8_t pct = ccord_store_user_volume(ui->d.store, vs->user_id);
    ccord_draw_rrect(r, slider, 5.0f, COL(bg_tertiary));
    float fill_w = slider.w * ((float)pct / 200.0f);
    if (fill_w > 0.0f) ccord_draw_rrect(r, (ccord_rect){slider.x, slider.y, fill_w, slider.h}, 5.0f, COL(brand));
    char pl[16];
    snprintf(pl, sizeof pl, "%u%%", (unsigned)pct);
    ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, COL(text_muted));
    ccord_text_draw(r, pl, strlen(pl), tile.x + tile.w - 46.0f, ty + 122.0f, &ps, 0.0f);
  }
  if (record && st->ctl_count < UI_STAGE_CTL_MAX) {
    st->ctl[st->ctl_count].user_id = vs->user_id;
    st->ctl[st->ctl_count].tile = tile;
    st->ctl[st->ctl_count].slider = slider;
    st->ctl[st->ctl_count].mute = mute;
    st->ctl[st->ctl_count].mod = mod;
    st->ctl[st->ctl_count].mod_is_invite = mod_is_invite;
    st->ctl_count++;
  }
}

static bool cam_in_list(const ccord_voice_state *const *list, size_t n, const ccord_voice_state *vs)
{
  for (size_t i = 0; i < n; i++) {
    if (list[i] == vs) return true;
  }
  return false;
}

ccord_rect dc_stream_fit_aspect(ccord_rect bounds, int frame_w, int frame_h)
{
  if (bounds.w <= 0.0f || bounds.h <= 0.0f) return (ccord_rect){0, 0, 0, 0};
  float aspect = (frame_w > 0 && frame_h > 0) ? ((float)frame_w / (float)frame_h) : (16.0f / 9.0f);
  aspect = clampf(aspect, 0.25f, 4.0f);
  float target_w = bounds.h * aspect;
  if (target_w > bounds.w) target_w = bounds.w;
  float target_h = target_w / aspect;
  if (target_h > bounds.h) {
    target_h = bounds.h;
    target_w = target_h * aspect;
  }
  return (ccord_rect){bounds.x + (bounds.w - target_w) * 0.5f,
                      bounds.y + (bounds.h - target_h) * 0.5f, target_w, target_h};
}

void ui_draw_voice_stage(ccord_ui *ui, ccord_renderer *r)
{
  ccord_rect ma = ui_stage_area(ui);
  if (ma.w <= 0.0f || ma.h <= 0.0f) return;
  // The stage owns the message area (or the split stage pane): drop any stale transcript hits so
  // they cannot intercept stage clicks. In split mode ui_draw_messages repopulates them afterwards.
  dc_reset_message_hits(ui);
  const ccord_voice_state *v = ui_self_voice(ui);
  const ccord_channel *ch = v ? ccord_store_channel(ui->d.store, v->channel_id) : nullptr;
  if (!ch && ui->stage_channel != CCORD_ID_NONE) ch = ccord_store_channel(ui->d.store, ui->stage_channel);
  if (!ch) return;
  ccord_draw_rect(r, ma, COL(bg_primary));
  ccord_draw_rect(r, (ccord_rect){ma.x, ma.y + 56.0f, ma.w, 1.0f}, COL(border_subtle));
  ui_icon_speaker(r, ma.x + 24.0f, ma.y + 28.0f, 22.0f, COL(status_online));
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  ccord_rect back = ui_stage_chat_btn(ui);
  float hdr_text_w = maxf(back.x - (ma.x + 44.0f) - 10.0f, 60.0f);
  ccord_text_draw_ellipsis(r, ch->name, strlen(ch->name), ma.x + 44.0f, ma.y + 12.0f, hdr_text_w, &ts);
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool self_streaming = st && st->active;
  bool watching_remote = !self_streaming && ws && ws->active;
  char sub[192];
  if (self_streaming) {
    snprintf(sub, sizeof sub, "Voice / Stream Stage  ·  LIVE · %s · %s · %s · %s", encoder_label(st->config.encoder),
             codec_label(st->config.codec), dc_resolution_label(st->config.resolution), dc_fps_label(st->config.fps));
  } else if (watching_remote) {
    const ccord_user *wu = ccord_store_user(ui->d.store, ws->user_id);
    snprintf(sub, sizeof sub, "Voice / Stream Stage  ·  Watching %s's Stream", wu ? wu->name : "Live");
  } else {
    snprintf(sub, sizeof sub, "Voice / Stream Stage  ·  %zu connected", ccord_store_voice_count(ui->d.store, ch->id));
  }
  ccord_text_draw_ellipsis(r, sub, strlen(sub), ma.x + 44.0f, ma.y + 31.0f, hdr_text_w, &ss);
  bool bh = ui->hover_voice_back;
  float bhf = ui_fade(ui, "stageback", 0, bh);
  ccord_draw_rrect(r, back, 4.0f, ui_mix(COL(bg_tertiary), COL(bg_item_hover), bhf));
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f,
                                 ui_mix(COL(text_secondary), COL(text_interactive_hover), bhf));
  const char *chat_lbl = ui_stage_split_active(ui) ? "Close Chat" : "Text Chat";
  ccord_vec2 bt = ccord_text_measure(r, chat_lbl, strlen(chat_lbl), &bs, 0.0f);
  ccord_text_draw(r, chat_lbl, strlen(chat_lbl), back.x + (back.w - bt.x) * 0.5f, back.y + (back.h - bt.y) * 0.5f, &bs, 0.0f);

  ui->stream_stage_btn1_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_stage_btn2_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_stage_pause_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_stage_fs_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_stage_vport_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_stage_resize_rect = (ccord_rect){0, 0, 0, 0};
  ui->stage_watch_count = 0;
  ui_stage_state *sst = stage_state(ui);
  memset(sst, 0, sizeof *sst);
  sst->owner = ui;
  ui_activity_state *ast = dc_activity_state(ui);
  ast->chip = (ccord_rect){0, 0, 0, 0};
  ast->banner_visible = false;
  ast->instance_count = 0;

  float area_x = ma.x + (ui->mobile_layout ? 12.0f : 20.0f), area_y = ma.y + 70.0f;
  float area_w = maxf(ma.w - (ui->mobile_layout ? 24.0f : 40.0f), 80.0f);

  // M5 control row: voice status, stage topic, request-to-speak. Edits open the shared text prompt;
  // the buttons only appear when the session would accept the action (permission / stage membership).
  bool is_stage = ch->kind == CCORD_CHANNEL_STAGE;
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  bool self_here = v && v->channel_id == ch->id;
  float ctl_cy = ma.y + 66.0f, cx = area_x;
  float row_right = area_x + area_w;
  ccord_text_style chip_ms = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_secondary));
  bool can_status = ccord_store_has_permission(ui->d.store, ch->guild_id, ch->id, self_id,
                                               CCORD_PERM_SET_VOICE_CHANNEL_STATUS);
  const char *status = ccord_store_voice_status(ui->d.store, ch->id);
  if (can_status) {
    char sl[160];
    snprintf(sl, sizeof sl, "Status: %s", (status && status[0]) ? status : "set voice status");
    float max_sw = fminf(260.0f, area_w);
    sst->status_edit = dc_stage_chip(ui, r, cx, ctl_cy, sl, COL(bg_tertiary), COL(text_secondary), max_sw);
    cx = sst->status_edit.x + sst->status_edit.w + 8.0f;
  } else if (status && status[0]) {
    ccord_text_style sts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
    float stw = fminf(260.0f, area_w);
    ccord_text_draw_ellipsis(r, status, strlen(status), cx, ctl_cy - 7.0f, stw, &sts);
    cx += stw + 8.0f;
  }
  bool can_mod = is_stage && ccord_store_has_permission(ui->d.store, ch->guild_id, ch->id, self_id,
                                                        CCORD_PERM_MUTE_MEMBERS);
  if (is_stage && can_mod) {
    const ccord_stage_instance *si = ccord_store_stage_instance(ui->d.store, ch->id);
    sst->stage_active = si != nullptr;
    if (si) {
      // An instance is live: its topic can be edited and the stage can be ended.
      char tl[160];
      snprintf(tl, sizeof tl, "Topic: %s", si->topic[0] ? si->topic : "set topic");
      float max_tw = fminf(260.0f, area_w);
      float est_tw = fminf(ccord_text_measure(r, tl, strlen(tl), &chip_ms, max_tw - 18.0f).x + 18.0f, max_tw);
      if (cx > area_x && cx + est_tw > row_right) { cx = area_x; ctl_cy += 30.0f; }
      sst->topic_edit = dc_stage_chip(ui, r, cx, ctl_cy, tl, COL(bg_tertiary), COL(text_secondary), max_tw);
      cx = sst->topic_edit.x + sst->topic_edit.w + 8.0f;
      float est_ew = fminf(ccord_text_measure(r, "End Stage", 9, &chip_ms, 102.0f).x + 18.0f, 120.0f);
      if (cx > area_x && cx + est_ew > row_right) { cx = area_x; ctl_cy += 30.0f; }
      sst->stage_end = dc_stage_chip(ui, r, cx, ctl_cy, "End Stage", COL(danger), COL(text_on_brand), 120.0f);
      cx = sst->stage_end.x + sst->stage_end.w + 8.0f;
    } else {
      // No instance yet: Start Stage opens the topic prompt and creates it through the session.
      float est_sw = fminf(ccord_text_measure(r, "Start Stage", 11, &chip_ms, 112.0f).x + 18.0f, 130.0f);
      if (cx > area_x && cx + est_sw > row_right) { cx = area_x; ctl_cy += 30.0f; }
      sst->stage_start = dc_stage_chip(ui, r, cx, ctl_cy, "Start Stage", COL(brand), COL(text_on_brand), 130.0f);
      cx = sst->stage_start.x + sst->stage_start.w + 8.0f;
    }
  }
  // Embedded Activities: the chip opens the shelf. Only for a voice channel the user is connected to
  // (launching also joins) and only with the Use Activities permission.
  if (ch->kind == CCORD_CHANNEL_VOICE && self_here && ui->d.session && ch->guild_id != CCORD_DM_GUILD_ID &&
      ccord_store_has_permission(ui->d.store, ch->guild_id, ch->id, self_id, CCORD_PERM_USE_EMBEDDED_ACTIVITIES)) {
    float est_aw = fminf(ccord_text_measure(r, "Activities", 10, &chip_ms, 102.0f).x + 18.0f, 120.0f);
    if (cx > area_x && cx + est_aw > row_right) { cx = area_x; ctl_cy += 30.0f; }
    ast->chip = dc_stage_chip(ui, r, cx, ctl_cy, "Activities", COL(brand), COL(text_on_brand), 120.0f);
    cx = ast->chip.x + ast->chip.w + 8.0f;
  }
  if (is_stage && self_here && v->suppress) {
    bool pending = v->request_to_speak_timestamp > 0;
    bool can_ask = pending || ccord_store_has_permission(ui->d.store, ch->guild_id, ch->id, self_id,
                                                         CCORD_PERM_REQUEST_TO_SPEAK);
    if (can_ask) {
      const char *rl = pending ? "Cancel Request" : "Request to Speak";
      float rw = fminf(150.0f, area_w);
      float rx = row_right - rw;
      if (rx < cx && cx > area_x) {
        ctl_cy += 30.0f;
        rx = area_x;
      } else if (rx < area_x) {
        rx = area_x;
      }
      sst->request_speak = dc_stage_chip(ui, r, rx, ctl_cy, rl, pending ? COL(bg_tertiary) : COL(brand),
                                    pending ? COL(text_secondary) : COL(text_on_brand), rw);
      sst->request_speak_cancel = pending;
    }
  }
  area_y = (ctl_cy > ma.y + 66.0f) ? (ctl_cy + 26.0f) : (ma.y + 96.0f);
  float mob_bar_h = (ui->mobile_layout && self_here) ? 68.0f : 0.0f;
  float stage_bottom = ma.y + ma.h - mob_bar_h;

  if (self_streaming || watching_remote) {
    float avail_h = maxf(stage_bottom - area_y, 0.0f);
    float min_h = fminf(160.0f, avail_h);
    float max_h = maxf(avail_h - 14.0f, min_h);
    float stage_h;
    if (ui->stream_stage_height_frac > 0.0f) {
      stage_h = clampf(avail_h * ui->stream_stage_height_frac, min_h, max_h);
    } else {
      float def_hi = maxf(avail_h - 280.0f, min_h);
      stage_h = clampf(avail_h * 0.58f, min_h, def_hi);
    }
    ui->stream_stage_player_y = area_y;
    ui->stream_stage_avail_h = avail_h;

    ccord_rect player = {area_x, area_y, area_w, stage_h};
    sst->stream_player = player;
    ccord_rect rhandle = {player.x, player.y + player.h - 2.0f, player.w, 16.0f};
    ui->stream_stage_resize_rect = rhandle;
    sst->stream_resize_handle = rhandle;
    bool rh_drag = ui->drag == DRAG_STREAM_STAGE_H;
    bool rh_hov = !rh_drag && ui->mouse_in && ui_in_rect(ui->mx, ui->my, rhandle);
    ccord_draw_rrect(r, (ccord_rect){player.x - 1.0f, player.y - 1.0f, player.w + 2.0f, player.h + 2.0f}, 9.0f,
                     rh_drag ? COL(brand) : COL(border_subtle));
    ccord_draw_rrect(r, player, 8.0f, (ccord_color){0x14, 0x15, 0x18, 255});

    // Status is honest: the store only learns that a stream exists (STREAM_CREATE); a LIVE badge
    // needs an actual preview/decoded frame, except when the user hid their own preview on purpose.
    int frame_w = 0, frame_h = 0;
    bool frame_present =
        ccord_render_has_texture(r, "__video_stream__", &frame_w, &frame_h) && frame_w > 0 && frame_h > 0;
    bool paused = (self_streaming && st->paused) || (watching_remote && ws->paused);
    bool live = frame_present || (self_streaming && ui->stream_hide_preview);
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

    // Header bar inside the stream player
    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, badge_fg);
    ccord_vec2 lt = ccord_text_measure(r, badge, strlen(badge), &ls, 0.0f);
    ccord_rect lb = {player.x + 12.0f, player.y + 10.0f, lt.x + 16.0f, 18.0f};
    ccord_draw_rrect(r, lb, 4.0f, badge_bg);
    ccord_text_draw(r, badge, strlen(badge), lb.x + (lb.w - lt.x) * 0.5f, lb.y + (lb.h - lt.y) * 0.5f, &ls, 0.0f);

    char st_title[192];
    char stream_sub[48];
    if (frame_present) {
      snprintf(stream_sub, sizeof stream_sub, "%dx%d", frame_w, frame_h);
    } else {
      snprintf(stream_sub, sizeof stream_sub, "Screen Share");
    }
    if (self_streaming) {
      const char *src_name = (ui->has_active_capture_source && ui->active_capture_source.title[0])
                               ? ui->active_capture_source.title
                               : "Screen Share";
      snprintf(st_title, sizeof st_title, "Your Stream  ·  %s  (%s · %s)", src_name,
               dc_resolution_label(st->config.resolution), dc_fps_label(st->config.fps));
    } else {
      const ccord_user *wu = ccord_store_user(ui->d.store, ws->user_id);
      snprintf(st_title, sizeof st_title, "%s's Screen Share  ·  %s", wu ? wu->name : "Remote User", stream_sub);
    }
    snprintf(sst->stream_badge, sizeof sst->stream_badge, "%s", badge);
    snprintf(sst->stream_title, sizeof sst->stream_title, "%s", st_title);
    snprintf(sst->stream_placeholder, sizeof sst->stream_placeholder, "%s", placeholder ? placeholder : "");
    sst->stream_frame_present = frame_present;
    sst->stream_frame_w = frame_present ? frame_w : 0;
    sst->stream_frame_h = frame_present ? frame_h : 0;

    // Right-aligned stream controls: [Pause (self)] [Hide Preview / Pause] [Stop] [Fullscreen]
    bool compact_hdr = player.w < 440.0f;
    ccord_text_style b1s = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_header));
    ccord_text_style b2s = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_on_brand));
    const char *b1_lbl = self_streaming
                           ? (compact_hdr ? (ui->stream_hide_preview ? "Show" : "Hide")
                                          : (ui->stream_hide_preview ? "Show Preview" : "Hide Preview"))
                           : (ws->paused ? "Resume" : "Pause");
    const char *b2_lbl = compact_hdr ? "Stop" : (self_streaming ? "Stop Streaming" : "Stop Watching");
    ccord_vec2 b1t = ccord_text_measure(r, b1_lbl, strlen(b1_lbl), &b1s, 0.0f);
    ccord_vec2 b2t = ccord_text_measure(r, b2_lbl, strlen(b2_lbl), &b2s, 0.0f);
    float b2_w = compact_hdr ? maxf(b2t.x + 14.0f, 48.0f) : 114.0f;
    float b1_w = compact_hdr ? maxf(b1t.x + 14.0f, 48.0f) : 104.0f;
    const char *plbl = (self_streaming && st->paused) ? "Resume" : "Pause";
    ccord_vec2 pt = ccord_text_measure(r, plbl, strlen(plbl), &b1s, 0.0f);
    float pb_w = compact_hdr ? maxf(pt.x + 14.0f, 48.0f) : 78.0f;
    float btns_total_w = 28.0f + 6.0f + b2_w + 6.0f + b1_w + (self_streaming ? (6.0f + pb_w) : 0.0f);
    bool two_row_hdr = (player.x + player.w - 12.0f - btns_total_w < lb.x + lb.w + 8.0f);
    float btn_y = two_row_hdr ? (player.y + 34.0f) : (player.y + 7.0f);

    ccord_rect fsb = {player.x + player.w - 12.0f - 28.0f, btn_y, 28.0f, 24.0f};
    ccord_rect b2 = {fsb.x - 6.0f - b2_w, btn_y, b2_w, 24.0f};
    ccord_rect b1 = {b2.x - 6.0f - b1_w, btn_y, b1_w, 24.0f};
    ui->stream_stage_fs_rect = fsb;
    sst->stream_fs_btn = fsb;
    ui->stream_stage_btn1_rect = b1;
    ui->stream_stage_btn2_rect = b2;
    bool fs_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, fsb);
    bool b1_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, b1);
    bool b2_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, b2);
    ccord_draw_rrect(r, fsb, 4.0f, fs_hov ? COL(bg_item_hover) : COL(bg_tertiary));
    ui_icon_fullscreen(r, fsb.x + fsb.w * 0.5f, fsb.y + fsb.h * 0.5f, 13.0f,
                       fs_hov ? COL(text_interactive_hover) : COL(text_header), false);
    ccord_draw_rrect(r, b1, 4.0f, b1_hov ? COL(bg_item_hover) : COL(bg_tertiary));
    ccord_draw_rrect(r, b2, 4.0f, b2_hov ? ui_mix(COL(danger), COL(bg_item_hover), 0.2f) : COL(danger));
    ccord_text_draw(r, b1_lbl, strlen(b1_lbl), b1.x + (b1.w - b1t.x) * 0.5f, b1.y + (b1.h - b1t.y) * 0.5f, &b1s, 0.0f);
    ccord_text_draw(r, b2_lbl, strlen(b2_lbl), b2.x + (b2.w - b2t.x) * 0.5f, b2.y + (b2.h - b2t.y) * 0.5f, &b2s, 0.0f);
    float left_btn_x = b1.x;
    if (self_streaming) {
      ccord_rect pb = {b1.x - 6.0f - pb_w, btn_y, pb_w, 24.0f};
      ui->stream_stage_pause_rect = pb;
      bool pb_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, pb);
      ccord_draw_rrect(r, pb, 4.0f, pb_hov ? COL(bg_item_hover) : COL(bg_tertiary));
      ccord_text_draw(r, plbl, strlen(plbl), pb.x + (pb.w - pt.x) * 0.5f, pb.y + (pb.h - pt.y) * 0.5f, &b1s, 0.0f);
      left_btn_x = pb.x;
    }

    ccord_text_style pts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    float title_x = lb.x + lb.w + 10.0f;
    float title_max_w = (two_row_hdr ? (player.x + player.w - 12.0f) : left_btn_x) - title_x - 10.0f;
    if (title_max_w > 24.0f) {
      ccord_text_draw_ellipsis(r, st_title, strlen(st_title), title_x, player.y + 11.0f, title_max_w, &pts);
    }

    // Video frame viewport inside the stream player
    float vport_top = two_row_hdr ? (player.y + 64.0f) : (player.y + 36.0f);
    ccord_rect vport = {player.x + 12.0f, vport_top, maxf(player.w - 24.0f, 0.0f),
                        maxf(player.y + player.h - vport_top - 10.0f, 0.0f)};
    ui->stream_stage_vport_rect = vport;
    sst->stream_vport = vport;
    if (self_streaming && ui->stream_hide_preview) {
      ccord_draw_rrect(r, vport, 6.0f, COL(bg_secondary));
      const char *hmsg = "Stream preview hidden — your screen share is still live";
      ccord_text_style hms = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
      ccord_vec2 hmt = ccord_text_measure(r, hmsg, strlen(hmsg), &hms, 0.0f);
      ccord_text_draw(r, hmsg, strlen(hmsg), vport.x + (vport.w - hmt.x) * 0.5f, vport.y + (vport.h - hmt.y) * 0.5f, &hms, 0.0f);
    } else if (frame_present) {
      ccord_rect vid = dc_stream_fit_aspect(vport, frame_w, frame_h);
      uint8_t alpha = paused ? 110 : 255;
      ccord_draw_texture_rounded(r, "__video_stream__", vid, 6.0f, alpha);
      if (paused) {
        const char *pmsg = "Stream Paused";
        ccord_text_style pms = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
        ccord_vec2 pmt = ccord_text_measure(r, pmsg, strlen(pmsg), &pms, 0.0f);
        ccord_draw_rrect(r, (ccord_rect){vid.x + (vid.w - pmt.x - 24.0f) * 0.5f, vid.y + (vid.h - 32.0f) * 0.5f, pmt.x + 24.0f, 32.0f},
                         6.0f, COL(bg_floating));
        ccord_text_draw(r, pmsg, strlen(pmsg), vid.x + (vid.w - pmt.x) * 0.5f, vid.y + (vid.h - pmt.y) * 0.5f, &pms, 0.0f);
      }
    } else {
      ccord_draw_rrect(r, vport, 6.0f, COL(bg_secondary));
      ccord_text_style phs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
      ccord_vec2 pht = ccord_text_measure(r, placeholder, strlen(placeholder), &phs, 0.0f);
      ccord_text_draw(r, placeholder, strlen(placeholder), vport.x + (vport.w - pht.x) * 0.5f,
                      vport.y + (vport.h - pht.y) * 0.5f, &phs, 0.0f);
    }

    if (fs_hov && !ui->menu.open && !ui->settings_open && !ui->go_live_open) {
      const char *tip = "Fullscreen (F)";
      ccord_text_style tips = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_header));
      ccord_vec2 tipt = ccord_text_measure(r, tip, strlen(tip), &tips, 0.0f);
      float tw = tipt.x + 14.0f, th = 22.0f;
      ccord_rect tipr = {fsb.x + fsb.w - tw, fsb.y + fsb.h + 4.0f, tw, th};
      ccord_draw_rrect(r, (ccord_rect){tipr.x - 1.0f, tipr.y - 1.0f, tipr.w + 2.0f, tipr.h + 2.0f}, 5.0f, COL(border_subtle));
      ccord_draw_rrect(r, tipr, 4.0f, COL(bg_floating));
      ccord_text_draw(r, tip, strlen(tip), tipr.x + 7.0f, tipr.y + (tipr.h - tipt.y) * 0.5f, &tips, 0.0f);
    }

    // Bottom vertical resize grip bar (drag to resize stream height; double-click to reset).
    if (rh_drag || rh_hov) {
      ccord_draw_rect(r, (ccord_rect){player.x + 8.0f, player.y + player.h - 1.0f, maxf(player.w - 16.0f, 0.0f), 2.0f},
                      rh_drag ? COL(brand) : ui_mix(COL(border_subtle), COL(brand), 0.65f));
    }
    float grip_w = fminf(48.0f, maxf(player.w - 32.0f, 16.0f));
    ccord_rect grip = {player.x + (player.w - grip_w) * 0.5f, player.y + player.h + 5.0f, grip_w, 4.0f};
    ccord_color grip_col = rh_drag ? COL(brand) : (rh_hov ? COL(text_header) : ui_alpha(COL(text_muted), 0.55f));
    ccord_draw_rrect(r, grip, 2.0f, grip_col);

    area_y += stage_h + 14.0f;
  }

  // Participant tiles (Speakers / Audience sections on a stage channel) and the Go Live cards share
  // one scrollable viewport below the header / controls / stream player, so off-fold tiles wrap and
  // stay reachable via the wheel instead of being dropped at the bottom edge.
  size_t n = ccord_store_voice_count(ui->d.store, ch->id);
  float tile_w = 176.0f, tile_h = 150.0f, gap = 12.0f;
  int cols;
  if (area_w >= 260.0f && area_w < 380.0f) {
    cols = 2;
    gap = 10.0f;
    tile_w = floorf((area_w - gap) * 0.5f);
  } else {
    cols = (int)((area_w + gap) / (tile_w + gap));
    if (cols < 1) cols = 1;
    if (tile_w > area_w) tile_w = area_w;
  }
  int passes = is_stage ? 2 : 1;
  size_t pass_match[2] = {0, 0};
  // Camera participants (self_video; self first) get the 16:9 tile grid on a plain voice channel.
  // The sender list is read once per draw; everyone else stays a legacy tile below the grid.
  const ccord_voice_state *camlist[UI_CAM_TILE_MAX];
  size_t ncam = 0;
  ccord_camera_rx cam_rx[CCORD_CAM_RX_MAX];
  size_t ncam_rx = 0, cam_decoding = 0;
  // Only the call self is in has cameras to show: from a channel self has left (or never joined) no sender is
  // delivered, so its camera users stay audio-only tiles instead of waiting forever on "Connecting video".
  const ccord_voice_state *self_vs = ui_self_voice(ui);
  if (!is_stage && self_vs && self_vs->channel_id == ch->id) {
    ccord_id sid = ccord_store_self_id(ui->d.store);
    for (int selfpass = 1; selfpass >= 0; selfpass--) {
      for (size_t i = 0; i < n && ncam < UI_CAM_TILE_MAX; i++) {
        const ccord_voice_state *vs = ccord_store_voice_at(ui->d.store, ch->id, i);
        if (vs && vs->self_video && (vs->user_id == sid) == (selfpass == 1)) camlist[ncam++] = vs;
      }
    }
    if (ncam > 0 && ui->d.session) ncam_rx = ccord_session_camera_senders(ui->d.session, cam_rx, CCORD_CAM_RX_MAX);
    for (size_t i = 0; i < ncam_rx; i++) {
      if (cam_rx[i].decoding) cam_decoding++;
    }
  }
  ui_cam_layout cam_l = {0};
  float cam_h = 0.0f;
  if (ncam > 0) {
    ui_cam_layout_compute(area_w, ncam, &cam_l);
    cam_h = ui_cam_layout_height(&cam_l, ncam) + gap;
  }
  for (int pass = 0; pass < passes; pass++) {
    for (size_t i = 0; i < n; i++) {
      const ccord_voice_state *vs = ccord_store_voice_at(ui->d.store, ch->id, i);
      if (!vs) continue;
      if (cam_in_list(camlist, ncam, vs)) continue;
      if (is_stage && ((pass == 0) == vs->suppress)) continue;
      pass_match[pass]++;
    }
  }
  size_t nsc = ccord_store_stream_list_count(ui->d.store);
  size_t ncards = 0;
  for (size_t i = 0; i < nsc && ncards < UI_STREAM_CARD_MAX; i++) {
    const ccord_stream_state *bcs = ccord_store_stream_list_at(ui->d.store, i);
    if (bcs && bcs->active && bcs->channel_id == ch->id) ncards++;
  }
  int card_cols = (int)((area_w - 220.0f) / 232.0f) + 1;
  if (card_cols < 1) card_cols = 1;
  // Measure the full content first so the scroll offset clamps before anything is placed.
  float content_h = cam_h;
  for (int pass = 0; pass < passes; pass++) {
    size_t rows = (pass_match[pass] + (size_t)cols - 1u) / (size_t)cols;
    content_h += (is_stage ? 22.0f : 0.0f) + (float)rows * (tile_h + gap) + (is_stage ? 10.0f : 0.0f);
  }
  if (n == 0) content_h += 30.0f;
  sst->cam_cols = cam_l.cols;
  const ccord_activity_instance *act_inst = nullptr;
  size_t act_n = dc_activity_instance_total(ui, ch, &act_inst);
  float act_blk_h = dc_activity_block_h_w(act_n, area_w);
  content_h += act_blk_h;
  if (ncards > 0) content_h += 18.0f + (float)((ncards + (size_t)card_cols - 1u) / (size_t)card_cols) *
                                         (ui->mobile_layout ? 70.0f : 62.0f);

  ccord_rect sview = {ma.x, area_y, ma.w, stage_bottom - area_y};
  if (sview.h < 0.0f) sview.h = 0.0f;
  ui->stage_view_h = sview.h;
  ui->stage_content_h = content_h;
  ui->stage_scroll = clampf(ui->stage_scroll, 0.0f, maxf(content_h - sview.h, 0.0f));
  sst->stage_view = sview;
  if (sview.h > 1.0f) {
    ccord_push_clip(r, sview);
    float section_y = area_y - ui->stage_scroll;
    dc_activity_draw_block(ui, r, ch, act_inst, act_n, area_x, section_y, area_w, sview);
    section_y += act_blk_h;
  for (size_t i = 0; i < ncam; i++) {
    const ccord_voice_state *vs = camlist[i];
    ccord_rect tile = ui_cam_layout_tile(&cam_l, area_x, area_w, section_y, ncam, i);
    ui_cam_tile_info ci = {0};
    ci.video = (ccord_rect){tile.x + 6.0f, tile.y + 6.0f, tile.w - 12.0f, cam_l.video_h};
    ccord_session_camera_texture_key(vs->user_id, ci.key, sizeof ci.key);
    int tw = 0, th = 0;
    ci.textured = ccord_render_has_texture(r, ci.key, &tw, &th) && tw > 0 && th > 0;
    ci.self = vs->user_id == ccord_store_self_id(ui->d.store);
    bool listed = false, decoding = false;
    for (size_t k = 0; k < ncam_rx; k++) {
      if (cam_rx[k].user_id == vs->user_id) {
        listed = true;
        decoding = cam_rx[k].decoding;
      }
    }
    ci.limit = !ci.self && listed && !decoding && cam_decoding >= CCORD_CAM_DECODE_MAX;
    if (sst->cam_tile_count < UI_CAM_TILE_MAX) {
      size_t ti = sst->cam_tile_count++;
      sst->cam_tile[ti].user_id = vs->user_id;
      sst->cam_tile[ti].rect = tile;
      sst->cam_tile[ti].video = ci.video;
      memcpy(sst->cam_tile[ti].key, ci.key, sizeof sst->cam_tile[ti].key);
      sst->cam_tile[ti].textured = ci.textured;
      sst->cam_tile[ti].tex_w = ci.textured ? tw : 0;
      sst->cam_tile[ti].tex_h = ci.textured ? th : 0;
      sst->cam_tile[ti].limit = ci.limit;
      sst->cam_tile[ti].connecting = !ci.textured && !ci.self && !ci.limit;
      sst->cam_tile[ti].self = ci.self;
    }
    if (tile.y + tile.h < sview.y || tile.y > sview.y + sview.h) continue;
    stage_tile(ui, r, vs, tile, watching_remote, ws, is_stage, can_mod, sview, &ci);
  }
  section_y += cam_h;
  for (int pass = 0; pass < passes; pass++) {
    if (is_stage) {
      if (pass == 0) sst->speaker_count = pass_match[0];
      else sst->audience_count = pass_match[1];
      char hdr[64];
      snprintf(hdr, sizeof hdr, "%s  ·  %zu", pass == 0 ? "SPEAKERS" : "AUDIENCE", pass_match[pass]);
      ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
      ccord_text_draw(r, hdr, strlen(hdr), area_x, section_y, &hs, 0.0f);
      section_y += 22.0f;
    }
    size_t placed = 0;
    for (size_t i = 0; i < n; i++) {
      const ccord_voice_state *vs = ccord_store_voice_at(ui->d.store, ch->id, i);
      if (!vs) continue;
      if (is_stage && ((pass == 0) == vs->suppress)) continue;
      if (cam_in_list(camlist, ncam, vs)) continue;
      int col = (int)(placed % (size_t)cols), row = (int)(placed / (size_t)cols);
      ccord_rect tile = {area_x + (float)col * (tile_w + gap), section_y + (float)row * (tile_h + gap),
                         tile_w, tile_h};
      placed++;
      if (tile.y + tile.h < sview.y || tile.y > sview.y + sview.h) continue;
      stage_tile(ui, r, vs, tile, watching_remote, ws, is_stage, can_mod, sview, nullptr);
    }
    size_t rows = (pass_match[pass] + (size_t)cols - 1u) / (size_t)cols;
    section_y += (float)rows * (tile_h + gap) + (is_stage ? 10.0f : 0.0f);
  }
  if (n == 0) {
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
    float empty_y = area_y - ui->stage_scroll + act_blk_h;
    ccord_text_draw(r, "No one is connected yet.", 25, area_x, empty_y, &es, 0.0f);
    section_y = empty_y + 30.0f;
  }

  // Go Live broadcast cards: the server-reported broadcasts in this channel, with viewer counts.
  float card_w = fminf(220.0f, area_w);
  float card_x = area_x, card_y = section_y + 2.0f;
  bool card_hdr = false;
  for (size_t i = 0; i < nsc && sst->stream_cards < UI_STREAM_CARD_MAX; i++) {
    const ccord_stream_state *bcs = ccord_store_stream_list_at(ui->d.store, i);
    if (!bcs || !bcs->active || bcs->channel_id != ch->id) continue;
    if (!card_hdr) {
      ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
      ccord_text_draw(r, "LIVE NOW", 8, area_x, card_y, &hs, 0.0f);
      card_y += 18.0f;
      card_hdr = true;
    }
    float card_h = ui->mobile_layout ? 58.0f : 50.0f;
    float card_stride = ui->mobile_layout ? 70.0f : 62.0f;
    ccord_rect card = {card_x, card_y, card_w, card_h};
    card_x += card_w + 12.0f;
    if (card_x + card_w > area_x + area_w + 1.0f) {
      card_x = area_x;
      card_y += card_stride;
    }
    if (card.y + card.h < sview.y || card.y > sview.y + sview.h) continue;
    ccord_draw_rrect(r, card, 6.0f, COL(bg_secondary));
    const ccord_user *bu = ccord_store_user(ui->d.store, bcs->user_id);
    char cl[160];
    snprintf(cl, sizeof cl, "%s", bu ? bu->name : "Live");
    ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_draw_ellipsis(r, cl, strlen(cl), card.x + 10.0f, card.y + (ui->mobile_layout ? 9.0f : 6.0f),
                             card.w - (ui->mobile_layout ? 112.0f : 100.0f), &cs);
    uint32_t viewers = ccord_store_stream_viewer_count(ui->d.store, ch->id, bcs->user_id);
    char vl[48];
    snprintf(vl, sizeof vl, "%u watching", (unsigned)viewers);
    ccord_text_style vs2 = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
    ccord_text_draw(r, vl, strlen(vl), card.x + 10.0f, card.y + (ui->mobile_layout ? 30.0f : 26.0f), &vs2, 0.0f);
    bool is_watched = watching_remote && ws && ws->user_id == bcs->user_id;
    bool is_self_card = bcs->user_id == self_id;
    float wb_w = ui->mobile_layout ? 84.0f : 72.0f;
    float wb_h = uis_btn_h_for(ui, 22.0f);
    ccord_rect wb = {card.x + card.w - wb_w - 10.0f, card.y + (card.h - wb_h) * 0.5f, wb_w, wb_h};
    if (!is_self_card) {
      ccord_draw_rrect(r, wb, 4.0f, is_watched ? COL(bg_tertiary) : COL(brand));
      const char *wl = is_watched ? "Watching" : "Watch";
      ccord_text_style wst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f,
                                      is_watched ? COL(text_secondary) : COL(text_on_brand));
      ccord_vec2 wt = ccord_text_measure(r, wl, strlen(wl), &wst, 0.0f);
      ccord_text_draw(r, wl, strlen(wl), wb.x + (wb.w - wt.x) * 0.5f, wb.y + (wb.h - wt.y) * 0.5f, &wst, 0.0f);
    } else {
      wb = (ccord_rect){0, 0, 0, 0};
    }
    // Only fully visible cards expose their Watch button.
    if (card.y >= sview.y && card.y + card.h <= sview.y + sview.h) {
      sst->card[sst->stream_cards].user_id = bcs->user_id;
      sst->card[sst->stream_cards].viewers = viewers;
      sst->card[sst->stream_cards].rect = card;
      sst->card[sst->stream_cards].watch = wb;
      sst->card[sst->stream_cards].watching = is_watched;
      sst->stream_cards++;
    }
  }
    ccord_pop_clip(r);
    ui->stage_thumb = ui_draw_list_scrollbar(ui, r, sview, content_h, ui->stage_scroll, "stagescroll", 0);
  } else {
    ui->stage_thumb = (ccord_rect){0, 0, 0, 0};
  }

  // Inline mobile voice control bar (Mute, Deafen, Camera, Screen, Disconnect) so users do not need
  // to open the left drawer to reach voice controls on phones.
  if (mob_bar_h > 0.0f) {
    ccord_rect bar = {ma.x, stage_bottom, ma.w, mob_bar_h};
    ccord_draw_rect(r, bar, COL(bg_secondary));
    ccord_draw_rect(r, (ccord_rect){bar.x, bar.y, bar.w, 1.0f}, COL(border_subtle));
    float bpad = 10.0f, bgap = 8.0f;
    float bw = floorf((bar.w - 2.0f * bpad - 4.0f * bgap) / 5.0f);
    if (bw < 36.0f) bw = 36.0f;
    float btn_h = UIS_MOB_H_BTN_LG, by = bar.y + (bar.h - btn_h) * 0.5f;
    float bx = bar.x + (bar.w - (5.0f * bw + 4.0f * bgap)) * 0.5f;
    sst->mob_mute = (ccord_rect){bx, by, bw, btn_h}; bx += bw + bgap;
    sst->mob_deaf = (ccord_rect){bx, by, bw, btn_h}; bx += bw + bgap;
    sst->mob_cam = (ccord_rect){bx, by, bw, btn_h}; bx += bw + bgap;
    sst->mob_stream = (ccord_rect){bx, by, bw, btn_h}; bx += bw + bgap;
    sst->mob_leave = (ccord_rect){bx, by, bw, btn_h};

    bool deaf = v ? v->self_deaf : (ui->d.session && ccord_session_voice_deafened(ui->d.session));
    bool muted = (v ? v->self_mute : (ui->d.session && ccord_session_voice_muted(ui->d.session))) || deaf;
    bool cam = ui->camera_on;
    bool streaming = self_streaming;
    const ccord_rect btns[5] = {sst->mob_mute, sst->mob_deaf, sst->mob_cam, sst->mob_stream, sst->mob_leave};
    const char *lbls[5] = {muted ? "Unmute" : "Mute", deaf ? "Undeaf" : "Deafen",
                           cam ? "Cam On" : "Cam", streaming ? "Live" : "Share", "Leave"};
    const ccord_color bgs[5] = {
      muted ? COL(danger) : COL(bg_tertiary),
      deaf ? COL(danger) : COL(bg_tertiary),
      cam ? COL(status_online) : COL(bg_tertiary),
      streaming ? COL(brand) : COL(bg_tertiary),
      COL(danger),
    };
    const ccord_color fgs[5] = {
      muted ? COL(text_on_brand) : COL(text_header),
      deaf ? COL(text_on_brand) : COL(text_header),
      cam ? COL(text_on_brand) : COL(text_header),
      streaming ? COL(text_on_brand) : COL(text_header),
      COL(text_on_brand),
    };
    for (int bi = 0; bi < 5; bi++) {
      bool bhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, btns[bi]);
      ccord_draw_rrect(r, btns[bi], 8.0f, bhv ? ui_mix(bgs[bi], COL(bg_item_hover), 0.25f) : bgs[bi]);
      ccord_text_style mbs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, fgs[bi]);
      ccord_vec2 mbt = ccord_text_measure(r, lbls[bi], strlen(lbls[bi]), &mbs, 0.0f);
      ccord_text_draw_ellipsis(r, lbls[bi], strlen(lbls[bi]),
                               btns[bi].x + maxf((btns[bi].w - mbt.x) * 0.5f, 3.0f),
                               btns[bi].y + (btns[bi].h - mbt.y) * 0.5f, maxf(btns[bi].w - 6.0f, 16.0f), &mbs);
    }
  }
}

// Activity chip / instance-card clicks inside the stage view. Join launches (and joins) through the
// session, Leave leaves the user's instance, Open in browser hands the channel URL to the host opener.
ccord_id ui_voice_stage_user_at(const ccord_ui *ui, float x, float y)
{
  if (!ui || ui->stage_channel == CCORD_ID_NONE) return CCORD_ID_NONE;
  const ui_stage_state *sst = stage_state(ui);
  if (sst->stage_view.w <= 0.0f || !ui_in_rect(x, y, sst->stage_view)) return CCORD_ID_NONE;
  for (size_t i = 0; i < sst->stream_cards; i++) {
    if (sst->card[i].rect.w > 0.0f && ui_in_rect(x, y, sst->card[i].rect)) return sst->card[i].user_id;
  }
  for (size_t i = 0; i < sst->cam_tile_count; i++) {
    if (sst->cam_tile[i].rect.w > 0.0f && ui_in_rect(x, y, sst->cam_tile[i].rect)) return sst->cam_tile[i].user_id;
  }
  for (size_t i = 0; i < sst->ctl_count; i++) {
    if (sst->ctl[i].tile.w > 0.0f && ui_in_rect(x, y, sst->ctl[i].tile)) return sst->ctl[i].user_id;
  }
  return CCORD_ID_NONE;
}

bool ui_voice_stage_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || ui->stage_channel == CCORD_ID_NONE || ev->type != CCORD_IN_MOUSE_DOWN || ev->button != 1) return false;
  const ccord_voice_state *v = ui_self_voice(ui);
  const ccord_channel *ch = v ? ccord_store_channel(ui->d.store, v->channel_id)
                              : ccord_store_channel(ui->d.store, ui->stage_channel);
  if (!ch) return false;
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool self_streaming = st && st->active;
  bool watching_remote = !self_streaming && ws && ws->active;

  if ((self_streaming || watching_remote) && ui_in_rect(ev->x, ev->y, ui->stream_stage_fs_rect)) {
    ui_stream_set_fullscreen(ui, true);
    return true;
  }
  if (self_streaming && ui_in_rect(ev->x, ev->y, ui->stream_stage_pause_rect)) {
    if (ui->d.session) {
      ccord_error err;
      (void)ccord_session_set_stream_paused(ui->d.session, !st->paused, &err);
    }
    ui->dirty = true;
    return true;
  }
  if ((self_streaming || watching_remote) && ui_in_rect(ev->x, ev->y, ui->stream_stage_btn1_rect)) {
    if (self_streaming) {
      ui->stream_hide_preview = !ui->stream_hide_preview;
    } else if (ui->d.session) {
      ccord_error err;
      (void)ccord_session_set_stream_paused(ui->d.session, !ws->paused, &err);
    }
    ui->dirty = true;
    return true;
  }
  if ((self_streaming || watching_remote) && ui_in_rect(ev->x, ev->y, ui->stream_stage_btn2_rect)) {
    ccord_error err;
    if (self_streaming && ui->d.session) {
      (void)ccord_session_stop_stream(ui->d.session, ch->guild_id, &err);
    } else if (watching_remote && ui->d.session) {
      (void)ccord_session_stop_watching(ui->d.session, &err);
    }
    ui->dirty = true;
    return true;
  }
  if ((self_streaming || watching_remote) && ui->stream_stage_resize_rect.w > 0.0f &&
      ui_in_rect(ev->x, ev->y, ui->stream_stage_resize_rect)) {
    if (ev->clicks >= 2) {
      ui->stream_stage_height_frac = 0.0f;
      ui_settings_save(ui);
    } else {
      ui->drag = DRAG_STREAM_STAGE_H;
    }
    ui->dirty = true;
    return true;
  }
  if ((self_streaming || watching_remote) && ev->clicks >= 2 && ui->stream_stage_vport_rect.w > 0.0f &&
      ui_in_rect(ev->x, ev->y, ui->stream_stage_vport_rect)) {
    ui_stream_set_fullscreen(ui, true);
    return true;
  }

  // M5 control row: status / topic open the shared prompt; request-to-speak is a governed PATCH.
  ui_stage_state *sst = stage_state(ui);
  if (sst->mob_mute.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->mob_mute)) {
    uc_toggle_voice_mute(ui);
    return true;
  }
  if (sst->mob_deaf.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->mob_deaf)) {
    uc_toggle_voice_deafen(ui);
    return true;
  }
  if (sst->mob_cam.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->mob_cam)) {
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
  if (sst->mob_stream.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->mob_stream)) {
    uc_open_go_live(ui);
    return true;
  }
  if (sst->mob_leave.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->mob_leave)) {
    uc_leave_voice(ui);
    return true;
  }
  bool is_stage = ch->kind == CCORD_CHANNEL_STAGE;
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  bool can_mod = is_stage && ccord_store_has_permission(ui->d.store, ch->guild_id, ch->id, self_id,
                                                        CCORD_PERM_MUTE_MEMBERS);
  if (sst->status_edit.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->status_edit)) {
    ui_voice_prompt_open(ui, UI_VOICE_PROMPT_STATUS, ch->id);
    return true;
  }
  if (sst->topic_edit.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->topic_edit)) {
    ui_voice_prompt_open(ui, UI_VOICE_PROMPT_TOPIC, ch->id);
    return true;
  }
  if (sst->stage_start.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->stage_start)) {
    ui_stage_start_prompt_open(ui, ch->guild_id, ch->id);
    return true;
  }
  if (sst->stage_end.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->stage_end)) {
    ui_stage_end_confirm_open(ui, ch->guild_id, ch->id);
    return true;
  }
  if (sst->request_speak.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->request_speak)) {
    ccord_admin_request req;
    ccord_error err;
    int64_t ask = sst->request_speak_cancel ? 0 : ui->now_unix_ms;
    if (ccord_admin_stage_request_speak(&req, ch->guild_id, ch->id, ask, &err) == CCORD_OK) {
      ui_admin_submit(ui, &req);
    } else {
      ui_notice(ui, "%s", err.message);
    }
    ui->dirty = true;
    return true;
  }
  if (dc_activity_stage_click(ui, ch, ev->x, ev->y)) return true;
  // Per-user controls: stage speaker management, local mute and the click-to-set volume slider.
  for (size_t i = 0; i < sst->ctl_count; i++) {
    if (sst->ctl[i].mute.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->ctl[i].mute)) {
      bool lm = ccord_store_user_local_mute(ui->d.store, sst->ctl[i].user_id);
      ccord_error err;
      (void)ccord_store_set_user_local_mute(ui->d.store, sst->ctl[i].user_id, !lm, &err);
      ui->dirty = true;
      return true;
    }
    if (sst->ctl[i].slider.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->ctl[i].slider)) {
      float frac = (ev->x - sst->ctl[i].slider.x) / sst->ctl[i].slider.w;
      if (frac < 0.0f) frac = 0.0f;
      if (frac > 1.0f) frac = 1.0f;
      int pct = (int)(frac * 200.0f + 0.5f);
      if (pct > 200) pct = 200;
      ccord_error err;
      if (ui->d.session &&
          ccord_session_set_user_volume(ui->d.session, sst->ctl[i].user_id, (uint8_t)pct, &err) == CCORD_OK) {
        ccord_ui_set_user_volume(ui, sst->ctl[i].user_id, (uint8_t)pct);
      } else if (!ui->d.session) {
        ui_notice(ui, "Not connected");
      }
      ui->dirty = true;
      return true;
    }
    if (is_stage && can_mod && sst->ctl[i].mod.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->ctl[i].mod) &&
        sst->ctl[i].user_id != self_id) {
      ccord_admin_request req;
      ccord_error err;
      if (ccord_admin_stage_set_speaker(&req, ch->guild_id, ch->id, sst->ctl[i].user_id,
                                       sst->ctl[i].mod_is_invite, nullptr, &err) == CCORD_OK) {
        ui_admin_submit(ui, &req);
      } else {
        ui_notice(ui, "%s", err.message);
      }
      ui->dirty = true;
      return true;
    }
  }
  // Go Live broadcast cards.
  for (size_t i = 0; i < sst->stream_cards; i++) {
    if (sst->card[i].watch.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->card[i].watch) && ui->d.session) {
      ccord_error err;
      if (sst->card[i].watching) {
        (void)ccord_session_stop_watching(ui->d.session, &err);
      } else {
        (void)ccord_session_watch_stream(ui->d.session, ch->guild_id, ch->id, sst->card[i].user_id, &err);
      }
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->stage_watch_count; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->stage_watch_rects[i]) && ui->d.session) {
      ccord_id uid = ui->stage_watch_users[i];
      ccord_error err;
      if (watching_remote && ws->user_id == uid) {
        (void)ccord_session_stop_watching(ui->d.session, &err);
      } else {
        (void)ccord_session_watch_stream(ui->d.session, ch->guild_id, ch->id, uid, &err);
      }
      ui->dirty = true;
      return true;
    }
  }
  ccord_id tile_uid = ui_voice_stage_user_at(ui, ev->x, ev->y);
  if (tile_uid != CCORD_ID_NONE) {
    ui_open_member_popover(ui, tile_uid, ev->x, ev->y, false);
    return true;
  }
  return false;
}

