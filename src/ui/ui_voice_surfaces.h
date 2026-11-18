// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// M5 voice surfaces shared between draw_chat.c, ui_call.c and ui_dave.c (m5-ui-surfaces,
//
//
// Input routing: ui.c is owned by another slice, so these surfaces hang off the existing hooks:
//  - draw: ui_draw_overlays / ui_draw_chat_header / ui_draw_voice_stage (draw_chat.c)
//  - clicks while no modal is open: ui_expr_handle_composer_click (expression_picker.c)
//  - the text prompt (voice status / stage topic) and the DAVE/RTC modals borrow the generic
//    modal routing (ui->modal_kind = UI_MODAL_EDIT_CHANNEL) and intercept ui_draw_overlays /
//    ui_overlays_handle before the legacy modal code; nothing else reacts to that kind.
// All Discord state changes go through the session (intents / ui_admin_submit) and only land in
// the store when the transport answers; the surfaces below never write Discord state locally.
#include "ui_internal.h"

// ---- introspection for tests --------------------------------------------------------------------
#define UI_STAGE_CTL_MAX 32u
#define UI_STREAM_CARD_MAX 8u
#define UI_CAM_TILE_MAX 16u
typedef struct ui_stage_debug {
  ccord_rect status_edit, topic_edit, request_speak;   // zero size when not offered
  bool request_speak_cancel;                           // the button cancels a pending ask
  size_t speaker_count, audience_count;                // stage sections (0 for plain voice)
  size_t ctl_count;
  struct {
    ccord_id user_id;
    ccord_rect slider, mute, mod;                      // zero size when absent
    bool mod_is_invite;                                // true = invite to speak, false = suppress
  } ctl[UI_STAGE_CTL_MAX];
  size_t stream_cards;
  struct {
    ccord_id user_id;
    uint32_t viewers;
    ccord_rect watch;                                  // zero size for the self card
    bool watching;
  } card[UI_STREAM_CARD_MAX];
  // Go Live stage header as last drawn (empty when no stream player is shown).
  char stream_badge[16];                               // LIVE / STARTING / CONNECTING / PAUSED
  char stream_title[192];
  char stream_placeholder[64];                         // status text inside the empty viewport
  bool stream_frame_present;                           // a real preview/decoded frame texture exists
  int stream_frame_w, stream_frame_h;                  // that texture's real dimensions
  // Camera tile grid; cam_tile_count is 0 on a stage channel.
  int cam_cols;
  size_t cam_tile_count;
  struct {
    ccord_id user_id;
    ccord_rect rect, video;                            // whole tile / its 16:9 video area
    char key[40];                                      // texture key "__cam__:<uid>"
    bool textured;                                     // the camera texture exists and is drawn
    int tex_w, tex_h;
    bool limit;                                        // waiting: "Video limit (4)"
    bool connecting;                                   // "Connecting video..."
    bool self;
  } cam_tile[UI_CAM_TILE_MAX];
  ccord_rect stream_player;                            // Go Live player (zero size when none)
  ccord_rect stage_view;                               // the scroll viewport of the stage
} ui_stage_debug;

// ---- camera tile grid layout (pure; unit-tested) ------------------------------------------------
// cols = min(3, ceil(sqrt(n))), reduced while a tile would be < 220 wide; tile_w in 220..520 (or the
// whole area when it is narrower than 220, so a tile never leaves the area horizontally). The video
// area is (tile_w-12) x floor((tile_w-12)*9/16); tile_h = 6 + video_h + 82 (name/status footer).
// Smallest rect holding 4 tiles of 220 px (2 cols x 2 rows): 452 x 422 (see ui_cam_layout_height).
#define UI_CAM_GAP 12.0f
#define UI_CAM_MIN_W 220.0f
#define UI_CAM_MAX_W 520.0f
typedef struct ui_cam_layout {
  int cols;
  float tile_w, video_h, tile_h, gap, block_w;
} ui_cam_layout;

static inline void ui_cam_layout_compute(float area_w, size_t n, ui_cam_layout *out)
{
  int cols = 1;
  while (cols < 3 && (size_t)(cols * cols) < n) cols++;   // ceil(sqrt(n)) capped at 3
  if (area_w < 0.0f) area_w = 0.0f;
  while (cols > 1 && (area_w - UI_CAM_GAP * (float)(cols - 1)) / (float)cols < UI_CAM_MIN_W) cols--;
  float w = (area_w - UI_CAM_GAP * (float)(cols - 1)) / (float)cols;
  if (w > UI_CAM_MAX_W) w = UI_CAM_MAX_W;
  if (w < UI_CAM_MIN_W) w = area_w;   // narrower than a minimal tile: use the whole area (cols == 1)
  out->cols = cols;
  out->tile_w = w;
  out->gap = UI_CAM_GAP;
  float vw = w - 12.0f;
  if (vw < 0.0f) vw = 0.0f;
  out->video_h = (float)(int)(vw * 9.0f / 16.0f);
  out->tile_h = 6.0f + out->video_h + 82.0f;
  out->block_w = (float)cols * w + (float)(cols - 1) * UI_CAM_GAP;
}

// Total height of the grid of n tiles (rows stacked with the gap, none after the last).
static inline float ui_cam_layout_height(const ui_cam_layout *l, size_t n)
{
  if (n == 0 || l->cols < 1) return 0.0f;
  size_t rows = (n + (size_t)l->cols - 1u) / (size_t)l->cols;
  return (float)rows * l->tile_h + (float)(rows - 1u) * l->gap;
}

// Rect of tile `index` of `n`; the block is centred in [area_x, area_x+area_w] and so is a partial last row.
static inline ccord_rect ui_cam_layout_tile(const ui_cam_layout *l, float area_x, float area_w, float top_y,
                                            size_t n, size_t index)
{
  size_t cols = (size_t)l->cols;
  size_t row = index / cols, col = index % cols;
  size_t in_row = cols;
  if (n - row * cols < cols) in_row = n - row * cols;
  float row_w = (float)in_row * l->tile_w + (float)(in_row - 1u) * l->gap;
  float x0 = area_x + (area_w - row_w) * 0.5f;
  return (ccord_rect){x0 + (float)col * (l->tile_w + l->gap), top_y + (float)row * (l->tile_h + l->gap),
                      l->tile_w, l->tile_h};
}

typedef enum ui_voice_prompt_kind : uint8_t {
  UI_VOICE_PROMPT_STATUS = 0,   // PUT /channels/{c}/voice-status
  UI_VOICE_PROMPT_TOPIC,        // PATCH /stage-instances/{c}
} ui_voice_prompt_kind;

typedef struct ui_voice_surfaces_debug {
  bool ring_visible;
  ccord_rect ring_accept, ring_decline;
  bool call_bar_visible;
  ccord_id call_bar_channel;
  ccord_rect call_bar_leave;
  bool header_call_visible;
  ccord_rect header_call;
  bool prompt_open;
  ui_voice_prompt_kind prompt_kind;
  ccord_id prompt_channel;
  ccord_rect prompt_input, prompt_ok, prompt_cancel;
  char prompt_text[512];
  bool dave_badge_visible;
  ccord_rect dave_badge, rtc_btn;
  bool dave_modal_open, rtc_open;
  ccord_rect dave_close, rtc_close;
  bool dave_active, dave_verified;
  uint32_t dave_epoch;
  char dave_privacy_code[64];
  ui_stage_debug stage;
} ui_voice_surfaces_debug;
void ui_voice_surfaces_get_debug(const ccord_ui *ui, ui_voice_surfaces_debug *out);

// ---- ui_call.c: incoming-call ring banner, active DM call bar, header call button --------------
// Ring banner + call bar, drawn on top of everything from ui_draw_overlays.
void ui_call_draw(ccord_ui *ui, ccord_renderer *r);
// Clicks on the ring banner, the call bar and the header call button (no modal open).
bool ui_call_click(ccord_ui *ui, float x, float y);
// "Start Call" / "Join Call" / "Leave Call" button for the selected DM channel; advances
// *hdr_btn_left like the other header buttons. min_x is the leftmost allowed position.
void ui_call_draw_header_button(ccord_ui *ui, ccord_renderer *r, float *hdr_btn_left, float cy, float min_x);

// ---- ui_call.c: shared voice text prompt (voice status / stage topic) ---------------------------
// Opens the prompt prefilled with the store's current value. No-op while another modal is open.
void ui_voice_prompt_open(ccord_ui *ui, ui_voice_prompt_kind kind, ccord_id channel_id);
bool ui_call_prompt_active(const ccord_ui *ui);
void ui_call_prompt_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_call_prompt_handle(ccord_ui *ui, const ccord_input_event *ev);   // consumes every event while active

// ---- ui_dave.c: E2EE badge, verification modal, RTC debug popover -------------------------------
void ui_dave_draw_badge(ccord_ui *ui, ccord_renderer *r);   // badge + RTC button by the voice bar
bool ui_dave_click(ccord_ui *ui, float x, float y);
bool ui_dave_modal_active(const ccord_ui *ui);
void ui_dave_modal_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_dave_modal_handle(ccord_ui *ui, const ccord_input_event *ev);   // consumes every event while active
void ui_dave_get_debug(const ccord_ui *ui, ui_voice_surfaces_debug *out);   // fills the dave_* fields

// ---- draw_chat.c: stage-view controls (status/topic/request-to-speak, tiles, stream cards) ------
void ui_stage_get_debug(const ccord_ui *ui, ui_stage_debug *out);

// ---- expression_picker.c: soundboard tab introspection for tests -------------------------------
// Rect of the listed hit cell for `sound_id` on the Soundboard tab (zero size when not listed).
ccord_rect ui_expr_soundboard_hit(const ccord_ui *ui, ccord_id sound_id);
