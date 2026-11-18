// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/ui/anim.h>

#include <math.h>

#include "ccord_test.h"

static ccord_anim_set A;

static void frame_at(uint64_t ms)
{
  ccord_anim_begin_frame(&A, ms);
}

static bool near(float a, float b)
{
  return fabsf(a - b) < 0.002f;
}

static void test_curves(void)
{
  CT_CHECK(ccord_ease_out(0.0f) == 0.0f && ccord_ease_out(1.0f) == 1.0f);
  CT_CHECK(ccord_ease_out(-3.0f) == 0.0f && ccord_ease_out(9.0f) == 1.0f);
  CT_CHECK(ccord_ease_out(0.5f) > 0.5f);                       // fast start
  float prev = 0.0f;
  for (int i = 1; i <= 20; i++) {                               // monotone
    float v = ccord_ease_out((float)i / 20.0f);
    CT_CHECK(v >= prev);
    prev = v;
  }
  CT_CHECK(near(ccord_ease_in_out(0.5f), 0.5f) && ccord_ease_in_out(0.25f) < 0.25f);
  float peak = 0.0f;
  for (int i = 0; i <= 100; i++) {
    float v = ccord_ease_out_back((float)i / 100.0f);
    if (v > peak) peak = v;
  }
  CT_CHECK(peak > 1.05f && peak < 1.2f && ccord_ease_out_back(1.0f) == 1.0f && ccord_ease_out_back(0.0f) == 0.0f);
}

static void test_keys(void)
{
  CT_CHECK(ccord_anim_key("chan", 7) == ccord_anim_key("chan", 7));
  CT_CHECK(ccord_anim_key("chan", 7) != ccord_anim_key("chan", 8));
  CT_CHECK(ccord_anim_key("chan", 7) != ccord_anim_key("user", 7));
  CT_CHECK(ccord_anim_key(nullptr, 0) != 0 && ccord_anim_key("", 0) != 0);
}

// A value follows its target over the time given, starts where it first appears, and a changed mind is not a jump.
static void test_to(void)
{
  ccord_anim_init(&A);
  uint32_t k = ccord_anim_key("row", 1);
  frame_at(1000);
  CT_CHECK(ccord_anim_to(&A, k, 1.0f, 100) == 1.0f);          // first sight: already there, nothing to animate
  CT_CHECK(!ccord_anim_active(&A));
  frame_at(1016);
  CT_CHECK(ccord_anim_to(&A, k, 0.0f, 100) == 1.0f);          // target changed this very frame: no time has passed yet
  CT_CHECK(ccord_anim_active(&A));
  frame_at(1066);
  float mid = ccord_anim_to(&A, k, 0.0f, 100);
  CT_CHECK(mid > 0.0f && mid < 1.0f && ccord_anim_active(&A));
  frame_at(1090);
  float later = ccord_anim_to(&A, k, 0.0f, 100);
  CT_CHECK(later < mid);                                        // still falling
  frame_at(1200);
  CT_CHECK(ccord_anim_to(&A, k, 0.0f, 100) == 0.0f && !ccord_anim_active(&A));   // landed exactly, and it says so

  // Turning around mid-flight continues from the current value.
  frame_at(1300);
  CT_CHECK(ccord_anim_to(&A, k, 1.0f, 100) == 0.0f);
  frame_at(1350);
  float up = ccord_anim_to(&A, k, 1.0f, 100);
  CT_CHECK(up > 0.0f && up < 1.0f);
  frame_at(1360);
  float back = ccord_anim_to(&A, k, 0.0f, 100);
  CT_CHECK(back > 0.0f && back < 1.0f && near(back, ccord_anim_to(&A, k, 0.0f, 100)) );
  frame_at(1361);
  float after = ccord_anim_to(&A, k, 0.0f, 100);
  CT_CHECK(after <= back);
}

static void test_not_drawn_means_fresh(void)
{
  ccord_anim_init(&A);
  uint32_t k = ccord_anim_key("row", 2);
  frame_at(0);
  CT_CHECK(ccord_anim_to(&A, k, 1.0f, 100) == 1.0f);
  frame_at(10);
  CT_CHECK(ccord_anim_to(&A, k, 0.0f, 100) == 1.0f);          // moving
  frame_at(20);
  frame_at(30);                                                 // two frames in which the element is not drawn
  CT_CHECK(ccord_anim_to(&A, k, 1.0f, 100) == 1.0f);          // it is back: no stale half-way value, straight to target
  CT_CHECK(!ccord_anim_active(&A));
}

static void test_enter_and_pop(void)
{
  ccord_anim_init(&A);
  uint32_t k = ccord_anim_key("dialog", 1), p = ccord_anim_key("badge", 1);
  frame_at(0);
  CT_CHECK(ccord_anim_enter(&A, k, 200) == 0.0f && ccord_anim_pop(&A, p, 200) == 0.0f);
  CT_CHECK(ccord_anim_active(&A));
  frame_at(100);
  float e = ccord_anim_enter(&A, k, 200);
  CT_CHECK(e > 0.5f && e < 1.0f);
  float peak = 0.0f;
  for (uint64_t t = 110; t <= 210; t += 10) {
    frame_at(t);
    float v = ccord_anim_pop(&A, p, 200);
    if (v > peak) peak = v;
    (void)ccord_anim_enter(&A, k, 200);
  }
  CT_CHECK(peak > 1.0f);                                        // overshoot
  frame_at(400);
  CT_CHECK(ccord_anim_enter(&A, k, 200) == 1.0f && ccord_anim_pop(&A, p, 200) == 1.0f && !ccord_anim_active(&A));
  // Gone for a frame, then back: it plays again (a menu opened a second time).
  frame_at(410);
  frame_at(420);
  CT_CHECK(ccord_anim_enter(&A, k, 200) == 0.0f);
  // An explicit reset does the same.
  frame_at(430);
  CT_CHECK(ccord_anim_enter(&A, k, 200) < 1.0f);
  ccord_anim_reset(&A, k);
  frame_at(440);
  CT_CHECK(ccord_anim_enter(&A, k, 200) == 0.0f);
}

static void test_reduced_motion_and_zero_duration(void)
{
  ccord_anim_init(&A);
  uint32_t k = ccord_anim_key("row", 3);
  frame_at(0);
  CT_CHECK(ccord_anim_to(&A, k, 1.0f, 100) == 1.0f);
  frame_at(5);
  CT_CHECK(ccord_anim_to(&A, k, 0.0f, 0) == 0.0f && !ccord_anim_active(&A));   // zero duration: instant
  A.reduced_motion = true;
  frame_at(10);
  CT_CHECK(ccord_anim_to(&A, k, 1.0f, 500) == 1.0f && !ccord_anim_active(&A));
  CT_CHECK(ccord_anim_enter(&A, ccord_anim_key("dialog", 9), 500) == 1.0f && !ccord_anim_active(&A));
}

// Idle means idle: with nothing moving the main loop is told so, and a time that does not advance does not fake motion.
static void test_idle_and_clock(void)
{
  ccord_anim_init(&A);
  uint32_t k = ccord_anim_key("row", 4);
  frame_at(100);
  CT_CHECK(!ccord_anim_active(&A) && ccord_anim_to(&A, k, 0.0f, 100) == 0.0f && !ccord_anim_active(&A));
  frame_at(110);
  CT_CHECK(ccord_anim_to(&A, k, 1.0f, 100) == 0.0f && ccord_anim_active(&A));
  frame_at(110);                                                // the clock did not move
  CT_CHECK(ccord_anim_to(&A, k, 1.0f, 100) == 0.0f);
  frame_at(50);                                                 // ... or went backwards
  CT_CHECK(ccord_anim_to(&A, k, 1.0f, 100) == 0.0f);
  ccord_anim_begin_frame(nullptr, 1);
  CT_CHECK(!ccord_anim_active(nullptr) && ccord_anim_to(nullptr, k, 0.5f, 10) == 0.5f && ccord_anim_enter(nullptr, k, 10) == 1.0f);
  CT_CHECK(ccord_anim_to(&A, 0, 0.25f, 10) == 0.25f);           // no key: no state, the target
}

// Far more elements than slots: nothing crashes, the ones drawn right now keep working.
static void test_table_pressure(void)
{
  ccord_anim_init(&A);
  for (uint64_t f = 0; f < 6; f++) {
    frame_at(f * 16);
    for (uint64_t i = 0; i < 2000; i++) {
      float v = ccord_anim_to(&A, ccord_anim_key("m", i), (f + i) % 2 ? 1.0f : 0.0f, 100);
      CT_CHECK(v >= 0.0f && v <= 1.0f);
    }
  }
  uint32_t mine = ccord_anim_key("mine", 1);
  frame_at(1000);
  CT_CHECK(ccord_anim_to(&A, mine, 1.0f, 100) == 1.0f);
  frame_at(1016);
  CT_CHECK(ccord_anim_to(&A, mine, 0.0f, 100) == 1.0f);
  frame_at(1066);
  float v = ccord_anim_to(&A, mine, 0.0f, 100);
  CT_CHECK(v > 0.0f && v < 1.0f);
}

int main(void)
{
  test_curves();
  test_keys();
  test_to();
  test_not_drawn_means_fresh();
  test_enter_and_pop();
  test_reduced_motion_and_zero_duration();
  test_idle_and_clock();
  test_table_pressure();
  return CT_RESULT("test_ui_anim");
}
