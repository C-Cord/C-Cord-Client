// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "scroll.h"

#include <math.h>

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool ccord_scroller_active(const ccord_scroller *s) { return s && s->pending != 0.0f; }

void ccord_scroller_cancel(ccord_scroller *s)
{
  if (!s) return;
  s->pending = 0.0f;
  s->elapsed_ms = 0.0f;
  s->fresh = false;
}

float ccord_scroller_push(ccord_scroller *s, float delta, bool direct)
{
  if (!s || delta == 0.0f || !isfinite(delta)) return 0.0f;
  if (direct) {
    if (s->pending * delta < 0.0f) ccord_scroller_cancel(s);   // the user reversed: stop gliding the old way
    return delta;
  }
  if (s->pending * delta <= 0.0f) s->pending = delta;
  else s->pending += delta;
  s->elapsed_ms = 0.0f;   // restart the curve over the new distance
  s->fresh = true;
  return 0.0f;
}

float ccord_scroller_advance(ccord_scroller *s, float dt_ms, bool reduced)
{
  if (!s) return 0.0f;
  if (s->pending == 0.0f) {
    s->elapsed_ms = 0.0f;
    return 0.0f;
  }
  if (dt_ms < 0.0f) dt_ms = 0.0f;
  if (s->fresh) {
    if (dt_ms > CCORD_SCROLL_WAKE_MS) dt_ms = CCORD_SCROLL_FIRST_MS;
    s->fresh = false;
  }
  float t1 = clampf(s->elapsed_ms / CCORD_SCROLL_EASE_MS, 0.0f, 1.0f);
  float t2 = (s->elapsed_ms + dt_ms) / CCORD_SCROLL_EASE_MS;
  float left = 0.0f;
  if (!reduced && t2 < 1.0f && t1 < 1.0f) {
    // remaining(t) = total * (1 - t)^3, so the ratio of two moments needs no memory of the total
    float k = (1.0f - t2) / (1.0f - t1);
    left = s->pending * k * k * k;
  }
  if (fabsf(left) < CCORD_SCROLL_SNAP_PX) left = 0.0f;
  float step = s->pending - left;
  s->pending = left;
  s->elapsed_ms = left == 0.0f ? 0.0f : s->elapsed_ms + dt_ms;
  return step;
}

// Keeps offset + pending inside [0, hi]; pending only shrinks, so the glide stays monotonic.
static void limit(ccord_scroller *s, float offset, float hi)
{
  float target = offset + s->pending;
  if (target > hi) s->pending = hi - offset;
  else if (target < 0.0f) s->pending = -offset;
  if (s->pending == 0.0f) s->elapsed_ms = 0.0f;
}

void ccord_scroller_scroll(ccord_scroller *s, float *offset, float delta, float max, bool direct)
{
  float hi = max > 0.0f ? max : 0.0f;
  *offset = clampf(*offset + ccord_scroller_push(s, delta, direct), 0.0f, hi);
  limit(s, *offset, hi);
}

bool ccord_scroller_tick(ccord_scroller *s, float *offset, float dt_ms, float max, bool reduced)
{
  if (s->pending == 0.0f) return false;
  float hi = max > 0.0f ? max : 0.0f;
  float step = ccord_scroller_advance(s, dt_ms, reduced);
  *offset = clampf(*offset + step, 0.0f, hi);
  limit(s, *offset, hi);   // content may have shrunk since the notch
  return s->pending != 0.0f;
}
