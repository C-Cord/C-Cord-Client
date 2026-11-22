// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

// ---- ui_scheduled_events.c: guild Scheduled Events modal + stage start/end prompts -------------
// Borrows the generic modal routing (ui->modal_kind = UI_MODAL_SCHEDULED_EVENTS) and intercepts
// ui_draw_overlays / ui_overlays_handle before the legacy modal code, like the M5 voice surfaces.
// Every Discord change is a session intent / governed admin request; nothing is written locally.
#define UI_EVENTS_ROW_MAX 32u
typedef enum ui_events_view : uint8_t {
  UI_EVENTS_LIST = 0,        // the guild's event list (RSVP / edit / delete)
  UI_EVENTS_FORM,            // create or edit one event
  UI_EVENTS_STAGE_START,     // topic prompt -> ccord_session_stage_start
  UI_EVENTS_STAGE_END,       // confirm -> ccord_session_stage_end
} ui_events_view;
typedef struct ui_events_debug {
  bool open;
  ui_events_view view;
  ccord_id guild_id;
  ccord_id edit_event_id;    // FORM: the event being edited (0 = creating)
  // LIST geometry as last drawn (zero size when not offered).
  ccord_rect box, create_btn, close_btn, list_view;
  float list_scroll, list_content_h;
  ccord_rect list_thumb;
  size_t row_count;
  struct {
    ccord_id event_id;
    ccord_rect row, rsvp, edit, del;   // zero size when absent
    bool rsvp_on;                      // the signed-in user is interested
    bool delete_armed;                 // first Delete click: waiting for the confirm click
  } rows[UI_EVENTS_ROW_MAX];
  // FORM geometry & state.
  ccord_rect name_input, location_input, start_input, end_input, desc_input;
  ccord_rect type_chip, channel_chip, save_btn, cancel_btn;
  int entity_type;                     // ccord_scheduled_event_entity_type (1..3)
  ccord_id channel_id;                 // selected stage / voice channel (0 for EXTERNAL)
  int focus_field;                     // 0 name, 1 location, 2 start, 3 end, 4 description
  char form_error[192];                // last validation refusal shown in the form
  // Stage prompts.
  ccord_rect stage_box, stage_input, stage_ok, stage_cancel;
  ccord_id stage_channel;
  char stage_topic[512];
} ui_events_debug;
void ui_scheduled_events_open(ccord_ui *ui, ccord_id guild_id);
bool ui_scheduled_events_active(const ccord_ui *ui);
bool ui_scheduled_events_wants_text(const ccord_ui *ui);
bool ui_scheduled_events_handle_back(ccord_ui *ui);
void ui_scheduled_events_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_scheduled_events_handle(ccord_ui *ui, const ccord_input_event *ev);   // consumes every event while active
ccord_cursor_kind ui_scheduled_events_cursor(const ccord_ui *ui);
void ui_scheduled_events_get_debug(const ccord_ui *ui, ui_events_debug *out);
// Stage lifecycle prompts, opened from the stage control row (draw_chat.c).
void ui_stage_start_prompt_open(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id);
void ui_stage_end_confirm_open(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id);
// Start/End chips in the stage control row (zero size when not offered).
typedef struct ui_stage_lifecycle_debug {
  ccord_rect start_stage, end_stage;
  bool stage_active;                   // an active stage instance exists
} ui_stage_lifecycle_debug;
void ui_stage_lifecycle_get_debug(const ccord_ui *ui, ui_stage_lifecycle_debug *out);

// ---- draw_chat.c: Embedded Activities (shelf modal + voice-stage instance cards) ---------------
// The shelf modal borrows ui->modal_kind = UI_MODAL_ACTIVITY_SHELF and intercepts ui_draw_overlays /
// ui_overlays_handle before the legacy modal code, like the scheduled-events modal. Launch / join /
// leave are session intents; running instances only appear once the transport answers. Activity
// content runs in the browser: the stage view offers "Open in browser" and never embeds web content.
#define UI_ACTIVITY_SHELF_MAX 16u
#define UI_ACTIVITY_INST_MAX 8u
typedef struct ui_activity_debug {
  // Voice-stage control row chip + instance cards (zero size when not offered).
  ccord_rect chip;
  bool banner_visible;                 // the "runs in your browser" note above the instance cards
  size_t instance_count;
  struct {
    ccord_id instance_id, application_id;
    ccord_rect card, join, leave, open_browser;   // zero size when absent or scrolled off; clipped to the viewport
    bool self_in;                      // the signed-in user is a participant
    size_t participant_count;
  } instances[UI_ACTIVITY_INST_MAX];
  // Shelf modal as last drawn.
  bool open;
  ccord_id guild_id, channel_id;
  ccord_rect box, close_btn, list_view, list_thumb;
  float list_scroll, list_content_h;
  size_t row_count;
  size_t sel;                          // keyboard selection (Up/Down move it, Enter launches it)
  float gutter;                        // right-hand strip reserved for the scrollbar (rows and clicks stop short of it)
  char load_error[128];                // the shelf request was refused when it opened ("" otherwise)
  struct {
    ccord_id application_id;
    ccord_rect row;                    // zero size when scrolled off
    char name[CCORD_NAME_MAX_BYTES + 1];
    char description[CCORD_ACTIVITY_DESC_MAX_BYTES + 1];
  } rows[UI_ACTIVITY_SHELF_MAX];
} ui_activity_debug;
void ui_activity_shelf_open(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id);
bool ui_activity_shelf_active(const ccord_ui *ui);
void ui_activity_shelf_draw(ccord_ui *ui, ccord_renderer *r);
bool ui_activity_shelf_handle(ccord_ui *ui, const ccord_input_event *ev);   // consumes every event while active
ccord_cursor_kind ui_activity_shelf_cursor(const ccord_ui *ui);
void ui_activity_get_debug(const ccord_ui *ui, ui_activity_debug *out);

