// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/ui/virtual_list.h>

#include <ccord/core/alloc.h>

#define DEFAULT_AVG 56.0f
#define AVG_ALPHA 0.1f

void ccord_vlist_init(ccord_vlist *v)
{
  *v = (ccord_vlist){.epoch = 1, .avg_height = DEFAULT_AVG, .stick_bottom = true};
}

void ccord_vlist_free(ccord_vlist *v)
{
  ccord_free(v->heights);
  ccord_free(v->height_epoch);
  ccord_vlist_init(v);
}

ccord_status ccord_vlist_set_count(ccord_vlist *v, size_t count, ccord_error *err)
{
  if (count > v->heights_cap) {
    size_t ncap = v->heights_cap ? v->heights_cap : 256;
    while (ncap < count) ncap *= 2;
    float *h = ccord_realloc_array(v->heights, ncap, sizeof *h);
    if (!h) return ccord_error_set(err, CCORD_ERR_NOMEM, "vlist: out of memory");
    v->heights = h;
    uint32_t *e = ccord_realloc_array(v->height_epoch, ncap, sizeof *e);
    if (!e) return ccord_error_set(err, CCORD_ERR_NOMEM, "vlist: out of memory");
    v->height_epoch = e;
    for (size_t i = v->heights_cap; i < ncap; i++) {
      v->heights[i] = 0.0f;
      v->height_epoch[i] = 0;   // epoch 0 is never valid
    }
    v->heights_cap = ncap;
  }
  if (count < v->count) {
    // Rows past the new end are gone; make sure a later growth does not resurrect stale heights.
    for (size_t i = count; i < v->count; i++) v->height_epoch[i] = 0;
  }
  // The last row may measure differently from the others (the UI pads it at the bottom), so a count change
  // re-measures the old and the new last row.
  if (count != v->count) {
    if (v->count > 0) v->height_epoch[v->count - 1] = 0;
    if (count > 0) v->height_epoch[count - 1] = 0;
  }
  size_t old_count = v->count;
  v->count = count;
  if (count == 0) {
    v->anchor_index = 0;
    v->anchor_offset = 0.0f;
    v->stick_bottom = true;
  } else if (old_count == 0 || v->stick_bottom || v->anchor_index >= count) {
    v->anchor_index = count - 1;
    v->anchor_offset = 0.0f;
    if (old_count == 0) v->stick_bottom = true;
  }
  return CCORD_OK;
}

void ccord_vlist_reset_bottom(ccord_vlist *v)
{
  v->anchor_index = v->count ? v->count - 1 : 0;
  v->anchor_offset = 0.0f;
  v->stick_bottom = true;
}

void ccord_vlist_invalidate_all(ccord_vlist *v)
{
  v->epoch++;
  if (v->epoch == 0) {   // wrapped: epoch 0 means "never measured"
    v->epoch = 1;
    for (size_t i = 0; i < v->heights_cap; i++) v->height_epoch[i] = 0;
  }
}

void ccord_vlist_scroll_by(ccord_vlist *v, float dy)
{
  if (v->count == 0 || dy == 0.0f) return;
  if (dy < 0.0f) v->stick_bottom = false;
  if (dy > 0.0f && v->stick_bottom) return;   // already following the newest row
  v->anchor_offset -= dy;
}

size_t ccord_vlist_rows_below(const ccord_vlist *v, const ccord_vlist_view *view)
{
  if (v->count == 0 || view->first > view->last || view->last >= v->count) return 0;
  return v->count - 1 - view->last;
}

float ccord_vlist_row_height(const ccord_vlist *v, size_t index)
{
  if (index < v->count && index < v->heights_cap && v->height_epoch[index] == v->epoch) return v->heights[index];
  return v->avg_height;
}

typedef struct measure_ctx {
  ccord_vlist *v;
  float width;
  ccord_vlist_measure_fn fn;
  void *ud;
  size_t measured;
} measure_ctx;

static float row_h(measure_ctx *c, size_t i)
{
  ccord_vlist *v = c->v;
  if (v->height_epoch[i] == v->epoch) return v->heights[i];
  float h = c->fn(i, c->width, c->ud);
  if (!(h >= 1.0f)) h = 1.0f;   // also catches NaN
  v->heights[i] = h;
  v->height_epoch[i] = v->epoch;
  v->avg_height += (h - v->avg_height) * AVG_ALPHA;
  c->measured++;
  return h;
}

static void layout_stick_bottom(measure_ctx *c, size_t last_row, float viewport_h, ccord_vlist_view *out)
{
  ccord_vlist *v = c->v;
  size_t a = last_row;
  float off = viewport_h - row_h(c, last_row);
  while (off > 0.0f && a > 0) {
    a--;
    off -= row_h(c, a);
  }
  float total_vis = 0.0f;
  for (size_t i = a; i <= last_row; i++) total_vis += row_h(c, i);
  off = viewport_h - total_vis;
  while (off > 0.0f && a > 0) {
    a--;
    total_vis += row_h(c, a);
    off = viewport_h - total_vis;
  }
  v->anchor_index = a;
  v->anchor_offset = off;
  v->stick_bottom = true;
  out->first = a;
  out->first_top = off;
  out->last = last_row;
  out->measured = c->measured;
}

ccord_status ccord_vlist_layout(ccord_vlist *v, float viewport_h, float width, ccord_vlist_measure_fn measure, void *ud, ccord_vlist_view *out, ccord_error *err)
{
  if (v->count > v->heights_cap) return ccord_error_set(err, CCORD_ERR_INVALID, "vlist: call set_count first");
  *out = (ccord_vlist_view){0};
  if (v->count == 0 || viewport_h <= 0.0f) {
    out->first = 1;   // first > last: nothing visible
    out->last = 0;
    return CCORD_OK;
  }
  measure_ctx c = {v, width, measure, ud, 0};
  size_t last_row = v->count - 1;
  if (v->stick_bottom) {
    layout_stick_bottom(&c, last_row, viewport_h, out);
    return CCORD_OK;
  }
  if (v->anchor_index > last_row) {
    v->anchor_index = last_row;
    v->anchor_offset = 0.0f;
  }
  size_t a = v->anchor_index;
  float off = v->anchor_offset;

  // Phase A: make the anchor the row that straddles the viewport top (or row 0 if we overscrolled upwards).
  while (a < last_row && off + row_h(&c, a) <= 0.0f) {
    off += row_h(&c, a);
    a++;
  }
  while (off > 0.0f && a > 0) {
    a--;
    off -= row_h(&c, a);
  }
  if (a == 0 && off > 0.0f) off = 0.0f;   // never scroll above the first row (content taller than viewport)

  // Phase B: walk down to find the bottom of the content that is visible.
  size_t i = a;
  float y = off;
  while (i <= last_row) {
    y += row_h(&c, i);
    if (y >= viewport_h || i == last_row) break;
    i++;
  }
  bool at_end = (i == last_row) && y <= viewport_h;
  if (at_end) {
    layout_stick_bottom(&c, last_row, viewport_h, out);
    return CCORD_OK;
  }
  v->stick_bottom = false;
  v->anchor_index = a;
  v->anchor_offset = off;
  out->first = a;
  out->first_top = off;
  // Re-walk to find `last` (rows past the viewport bottom are not part of the view).
  size_t li = a;
  float yy = off;
  while (li < last_row) {
    float h = row_h(&c, li);
    if (yy + h >= viewport_h) break;
    yy += h;
    li++;
  }
  out->last = li;
  out->measured = c.measured;
  return CCORD_OK;
}

void ccord_vlist_thumb(const ccord_vlist *v, float viewport_h, float *top01, float *size01)
{
  *top01 = 0.0f;
  *size01 = 1.0f;
  if (v->count == 0 || viewport_h <= 0.0f) return;
  float total = (float)v->count * v->avg_height;
  if (total <= viewport_h) return;
  float size = viewport_h / total;
  float pos = ((float)v->anchor_index * v->avg_height - v->anchor_offset) / total;
  float max_top = 1.0f - size;
  if (pos < 0.0f) pos = 0.0f;
  if (pos > max_top) pos = max_top;
  if (v->stick_bottom) pos = max_top;
  *top01 = pos;
  *size01 = size;
}
