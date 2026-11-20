// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// The shared eased scroller (src/ui/scroll.h): curve, frame-rate independence, retarget, limits, reduced motion.
#include <math.h>

#include "scroll.h"
#include "ccord_test.h"

static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }

// Runs a one-notch glide at a fixed frame step; returns the first-frame step and the total, and checks monotonic travel.
static void glide(float notch, float step_ms, float *first, float *total, float *settle_ms)
{
  ccord_scroller s = {0};
  CT_CHECK(ccord_scroller_push(&s, notch, false) == 0.0f);
  float sum = 0.0f, prev = 0.0f, t = 0.0f;
  *first = 0.0f;
  *settle_ms = 0.0f;
  for (int i = 0; i < 2000 && ccord_scroller_active(&s); i++) {
    float d = ccord_scroller_advance(&s, step_ms, false);
    if (i == 0) *first = d;
    CT_CHECK(d >= 0.0f && sum + d <= notch + 0.001f);
    sum += d;
    t += step_ms;
    if (*settle_ms == 0.0f && sum >= notch - 0.05f) *settle_ms = t;   // on target, to a twentieth of a pixel
    CT_CHECK(sum >= prev);
    prev = sum;
  }
  *total = sum;
}

static void test_curve_and_frame_rate(void)
{
  static const float steps[] = {4.0f, 8.0f, 16.0f, 33.0f};
  float lo = 1e9f, hi = 0.0f;
  for (size_t i = 0; i < 4; i++) {
    float first, total, settle;
    glide(66.0f, steps[i], &first, &total, &settle);
    CT_CHECK(near(total, 66.0f));
    CT_CHECK(first > 0.0f && first <= 0.35f * 66.0f);
    if (settle < lo) lo = settle;
    if (settle > hi) hi = settle;
  }
  CT_CHECK(hi <= lo * 1.10f);   // settle time is the same whatever the frame step
}

static void test_idle_wakeup(void)
{
  ccord_scroller s = {0};
  (void)ccord_scroller_push(&s, 66.0f, false);
  float d = ccord_scroller_advance(&s, 100.0f, false);   // the loop slept 100 ms before this frame
  CT_CHECK(d > 0.0f && d <= 0.35f * 66.0f);
  float sum = d;
  for (int i = 0; i < 100 && ccord_scroller_active(&s); i++) sum += ccord_scroller_advance(&s, 16.0f, false);
  CT_CHECK(near(sum, 66.0f));
}

static void test_direct_and_reduced(void)
{
  ccord_scroller s = {0};
  CT_CHECK(near(ccord_scroller_push(&s, 12.0f, true), 12.0f) && !ccord_scroller_active(&s));   // trackpad: at once
  (void)ccord_scroller_push(&s, 66.0f, false);
  CT_CHECK(near(ccord_scroller_advance(&s, 16.0f, true), 66.0f) && !ccord_scroller_active(&s));   // reduced motion
}

static void test_retarget_and_reverse(void)
{
  ccord_scroller s = {0};
  (void)ccord_scroller_push(&s, 66.0f, false);
  (void)ccord_scroller_advance(&s, 16.0f, false);
  float left = s.pending;
  (void)ccord_scroller_push(&s, 66.0f, false);   // same way: remaining + new
  CT_CHECK(near(s.pending, left + 66.0f));
  (void)ccord_scroller_push(&s, -30.0f, false);  // reversed: replaces what is left
  CT_CHECK(near(s.pending, -30.0f));
  (void)ccord_scroller_push(&s, 66.0f, false);
  CT_CHECK(near(ccord_scroller_push(&s, -5.0f, true), -5.0f) && !ccord_scroller_active(&s));   // direct reversal cancels
}

static void test_offset_limits(void)
{
  float off = 0.0f;
  ccord_scroller s = {0};
  ccord_scroller_scroll(&s, &off, 500.0f, 100.0f, false);   // target clamps to the content end
  CT_CHECK(near(s.pending, 100.0f));
  for (int i = 0; i < 100 && ccord_scroller_tick(&s, &off, 16.0f, 100.0f, false); i++) {}
  CT_CHECK(near(off, 100.0f) && !ccord_scroller_active(&s));
  ccord_scroller_scroll(&s, &off, -500.0f, 100.0f, false);
  (void)ccord_scroller_tick(&s, &off, 16.0f, 100.0f, false);
  float mid = off;
  CT_CHECK(ccord_scroller_tick(&s, &off, 16.0f, 40.0f, false) || near(off, 40.0f));   // content shrank to 40 px
  CT_CHECK(off <= 40.0f + 0.01f && off <= mid);
  ccord_scroller_scroll(&s, &off, 5.0f, 0.0f, true);   // no overflow: stays at 0
  CT_CHECK(near(off, 0.0f));
}

int main(void)
{
  test_curve_and_frame_rate();
  test_idle_wakeup();
  test_direct_and_reduced();
  test_retarget_and_reverse();
  test_offset_limits();
  return CT_RESULT("test_ui_scroll");
}
