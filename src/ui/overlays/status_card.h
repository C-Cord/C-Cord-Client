// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Account card: Discord's user-panel popout. Opens above the user panel; holds the profile header, an Edit Profile
// row, the four presence rows (applied at once) and the custom status (Set / clear). No Save, no Cancel.
// All state lives in ui_status_card (ccord_ui::sc); the code is in status_card.c.
#include <stdbool.h>
#include <stdint.h>

#include <ccord/core/input.h>
#include <ccord/net/admin.h>
#include <ccord/render/render.h>
#include <ccord/ui/text_edit.h>
#include <ccord/ui/ui.h>

#define UI_SC_ITEMS_MAX 24u
#define UI_SC_CONFIRM_MS 1500u   // how long the checkmark replaces the Set button
#define UI_SC_EMOJI_N 6

typedef enum ui_sc_kind : uint8_t {
  UI_SC_EDIT_PROFILE,
  UI_SC_PRESENCE,        // arg: 0 Online, 1 Idle, 2 Do Not Disturb, 3 Invisible
  UI_SC_EMOJI_BUTTON,
  UI_SC_EMOJI_PICK,      // arg: index into the preset emoji
  UI_SC_TEXT,
  UI_SC_DURATION,        // cycles Don't clear / 30m / 1h / 4h / Today
  UI_SC_SET,
  UI_SC_CLEAR,
} ui_sc_kind;

// One hit-testable control, recorded by the last draw.
typedef struct ui_sc_item {
  ui_sc_kind kind;
  int arg;
  ccord_rect rect;
  char label[48];
} ui_sc_item;

typedef struct ui_status_card {
  bool open;
  ccord_text_edit text;            // custom status text (init/free through ui_status_card_init/_free)
  char emoji[72];                  // chosen custom status emoji ("" none)
  int duration;                    // 0 don't clear, 1 30m, 2 1h, 3 4h, 4 today
  bool emoji_row;                  // preset emoji row shown
  uint64_t confirm_until_ms;       // checkmark replaces Set until then (0 = not confirming)
  uint64_t presence_until_ms;      // a presence request is awaiting Discord's answer until then; a refusal restores prev_presence
  int prev_presence;               // ccord_presence before that request
  uint64_t custom_until_ms;        // the same for the custom status request
  char error[256];                 // inline error, "" none
  bool last_caret_on;
  ccord_rect card;                 // whole card, from the last draw (w == 0 while closed)
  ccord_rect panel;                // the user panel it hangs above, from the last draw
  float scroll;                    // vertical scroll when clamped (e.g., mobile keyboard open)
  float max_scroll;
  ui_sc_item items[UI_SC_ITEMS_MAX];
  size_t nitems;
} ui_status_card;

void ui_status_card_init(ccord_ui *ui);
void ui_status_card_free(ccord_ui *ui);
void ui_status_card_open(ccord_ui *ui);      // from a click on the user panel; loads the current custom status
void ui_status_card_close(ccord_ui *ui);
bool ui_status_card_is_open(const ccord_ui *ui);
void ui_status_card_tick(ccord_ui *ui);      // from ccord_ui_update: caret blink and the confirmation timeout
void ui_status_card_draw(ccord_ui *ui, ccord_renderer *r);
// True while the card consumed `ev` (the whole event stream is routed here while it is open).
bool ui_status_card_handle(ccord_ui *ui, const ccord_input_event *ev);
// A user-settings / custom-status request was refused: show the error and restore the previous marker.
void ui_status_card_on_failure(ccord_ui *ui, ccord_admin_op op);
// True while the confirmation checkmark is shown in place of Set.
bool ui_status_card_confirming(const ccord_ui *ui);
