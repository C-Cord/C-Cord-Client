// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>
// Anchor-based virtualization for variable-height rows. Only rows intersecting the viewport are measured/drawn;
// the rest of the list costs O(1). Scroll position = (anchor_index, anchor_offset).
typedef float (*ccord_vlist_measure_fn)(size_t index, float width, void *ud);   // row height in px, > 0
typedef struct ccord_vlist {
  size_t count;
  size_t anchor_index;       // row used as reference
  float anchor_offset;       // y of anchor row's top relative to viewport top (<= 0 means partly above)
  bool stick_bottom;         // follow new rows while at bottom
  float *heights; size_t heights_cap; uint32_t *height_epoch; uint32_t epoch;   // lazy measure cache
  float avg_height;          // running average of measured rows (for scrollbar estimate)
} ccord_vlist;
typedef struct ccord_vlist_view {
  size_t first, last;        // inclusive visible range, valid if count > 0 and first <= last
  float first_top;           // y of row `first` relative to viewport top
  size_t measured;           // rows measured during this layout (test hook: must stay O(visible))
} ccord_vlist_view;
void ccord_vlist_init(ccord_vlist *v);
void ccord_vlist_free(ccord_vlist *v);
[[nodiscard]] ccord_status ccord_vlist_set_count(ccord_vlist *v, size_t count, ccord_error *err);   // grows cache; keeps position; if stick_bottom stays at bottom; re-measures the old and new last row
void ccord_vlist_reset_bottom(ccord_vlist *v);          // anchor = last row at the bottom, stick_bottom = true (channel switch)
void ccord_vlist_invalidate_all(ccord_vlist *v);        // width/theme change: bumps epoch
void ccord_vlist_scroll_by(ccord_vlist *v, float dy);   // dy > 0 moves content up (towards newer rows); clears stick_bottom when moving up
// Computes the visible window, normalises the anchor against content ends (never scrolls past first/last row), sets stick_bottom when last row bottom <= viewport bottom.
[[nodiscard]] ccord_status ccord_vlist_layout(ccord_vlist *v, float viewport_h, float width, ccord_vlist_measure_fn measure, void *ud, ccord_vlist_view *out, ccord_error *err);
// Rows that lie entirely below the viewport in a view from ccord_vlist_layout (0 when nothing is visible or the list is
// pinned to the bottom). The row the viewport bottom cuts through is visible, so it is not counted.
size_t ccord_vlist_rows_below(const ccord_vlist *v, const ccord_vlist_view *view);
float ccord_vlist_row_height(const ccord_vlist *v, size_t index);   // last measured or avg
// Scrollbar thumb: top/height as fraction of the track, estimated from measured + avg heights.
void ccord_vlist_thumb(const ccord_vlist *v, float viewport_h, float *top01, float *size01);
