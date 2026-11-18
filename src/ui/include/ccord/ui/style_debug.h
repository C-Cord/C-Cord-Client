// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Read-only view of the UI style audit for the `--verify-slice ui-style` check.
//
// Every control the shared style helpers (ui_style.h, private to the ui module) draw leaves one record: its kind, rect,
// corner radius, label size and fill. ccord_ui_get_style_debug copies that table and the theme-derived tokens it is judged
// against, so the host can check a surface without including the private header. The table is process-global and filled by
// every draw: reset it, draw the surface, read it.
#include <stdint.h>

#include <ccord/render/render.h>
#include <ccord/ui/ui.h>

// The kinds of control that are audited, in the order of `uis_kind` in ui_style.h (ui_style.c asserts the two agree).
typedef enum ccord_ui_style_kind : uint8_t {
  CCORD_UI_STYLE_BUTTON, CCORD_UI_STYLE_ICON_BUTTON, CCORD_UI_STYLE_MENU_ROW, CCORD_UI_STYLE_LIST_ROW,
  CCORD_UI_STYLE_POPOVER, CCORD_UI_STYLE_MODAL, CCORD_UI_STYLE_TOOLTIP, CCORD_UI_STYLE_INPUT, CCORD_UI_STYLE_TOGGLE,
  CCORD_UI_STYLE_TAB, CCORD_UI_STYLE_SURFACE, CCORD_UI_STYLE_CHIP, CCORD_UI_STYLE_SELECT, CCORD_UI_STYLE_SWATCH,
  CCORD_UI_STYLE_RADIO, CCORD_UI_STYLE_VDIVIDER, CCORD_UI_STYLE_SLIDER, CCORD_UI_STYLE_CARD, CCORD_UI_STYLE_SCROLLBAR,
  CCORD_UI_STYLE_PLATE,
  CCORD_UI_STYLE_KIND_COUNT
} ccord_ui_style_kind;

#define CCORD_UI_STYLE_RECORDS 256   // draws kept in order; the counts below keep rising past it

typedef struct ccord_ui_style_record {
  ccord_ui_style_kind kind;
  ccord_rect rect;
  float radius;     // corner radius drawn with (0 for a divider)
  float font_px;    // label size drawn with (0 when the control draws no text)
  ccord_color fill;
} ccord_ui_style_record;

typedef struct ccord_ui_style_debug {
  uint32_t drawn[CCORD_UI_STYLE_KIND_COUNT];   // draws of each kind since the last reset
  ccord_rect last_rect[CCORD_UI_STYLE_KIND_COUNT];
  float last_radius[CCORD_UI_STYLE_KIND_COUNT];
  float last_font_px[CCORD_UI_STYLE_KIND_COUNT];
  ccord_color last_fill[CCORD_UI_STYLE_KIND_COUNT];
  uint32_t total;                              // every draw since the reset
  uint32_t kept;                               // records[0 .. kept) are the first draws in order: min(total, RECORDS)
  ccord_ui_style_record records[CCORD_UI_STYLE_RECORDS];
  // The tokens a record is judged against, resolved from the UI's theme now.
  float r_control, r_menu, r_surface;
  float fs_title, fs_body, fs_control, fs_caption, fs_overline;
  float menu_row_h, list_row_h;
} ccord_ui_style_debug;

void ccord_ui_get_style_debug(const ccord_ui *ui, ccord_ui_style_debug *out);
const char *ccord_ui_style_kind_name(ccord_ui_style_kind kind);
// Empties the audit table (the draws counted so far are forgotten).
void ccord_ui_reset_style_audit(ccord_ui *ui);
