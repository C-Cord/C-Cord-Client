// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// The shared eased scroller. Every scrollable surface that glides under the wheel uses it, so they all move the same
// way: a wheel notch becomes a `pending` distance that is paid out along an ease-out cubic over CCORD_SCROLL_EASE_MS.
//
// - Frame-rate independent: the curve is a function of elapsed time, so one notch covers the same distance at the same
//   moments whatever the frame step (only the last frame rounds up to the frame boundary).
// - Nothing jumps on the first frame: a notch queues the whole distance and the first frame pays out only its dt share.
// - A further notch in the same direction restarts the curve over the remaining distance plus the new one; a notch in
//   the opposite direction replaces what is left.
// - Precise deltas (trackpads, hi-res wheels), page jumps and reduced motion are `direct`: applied at once, no lag.
// - The pending distance is the scroller's target minus the current offset; the caller owns the offset itself, so one
//   scroller drives an offset float (lists) or a relative scroll call (the anchor-based timeline) alike.
//
// Offset lists (clamped to [0, max]):
//   ccord_scroller_scroll(&s, &offset, delta, max, direct);   // on a wheel event
//   ccord_scroller_tick(&s, &offset, dt_ms, max, reduced);    // once per frame; true while a glide is still running
// Relative surfaces (timeline):
//   float now = ccord_scroller_push(&s, delta, direct);       // apply `now` yourself
//   float step = ccord_scroller_advance(&s, dt_ms, reduced);  // once per frame; apply `step` yourself
#include <stdbool.h>

#define CCORD_SCROLL_EASE_MS 280.0f   // duration of one glide
// The first frame after a notch may follow a long idle wait (the app loop sleeps up to 100 ms between frames). A first
// frame longer than CCORD_SCROLL_WAKE_MS is such a wake-up, not an animating frame, so the glide starts one
// CCORD_SCROLL_FIRST_MS frame after the notch instead of jumping by the whole idle time.
#define CCORD_SCROLL_WAKE_MS 33.0f
#define CCORD_SCROLL_FIRST_MS 33.0f
#define CCORD_SCROLL_SNAP_PX 0.01f    // a glide with less than this left finishes in the same frame (float dust only: the
                                      // curve itself ends on the last frame of CCORD_SCROLL_EASE_MS, so settle time does
                                      // not depend on the distance)
// The bound to pass to ccord_scroller_tick when the surface's draw already clamps its offset to its content (the
// draw runs after the tick each frame). The wheel handler still bounds the distance it queues, so a glide never
// carries more than the content has room for at the time of the notch.
#define CCORD_SCROLL_UNBOUNDED 1.0e9f

typedef struct ccord_scroller {
  float pending;      // px still to travel, signed (positive = towards the end of the content). Keep first: ccord_ui
                      // aliases it as the legacy `*_scroll_vel` floats that other files clear to cancel a glide.
  float elapsed_ms;   // time into the current glide
  bool fresh;         // no frame has advanced this glide yet
} ccord_scroller;

[[nodiscard]] bool ccord_scroller_active(const ccord_scroller *s);

// Drops any glide in flight; the offset stays where it is.
void ccord_scroller_cancel(ccord_scroller *s);

// Queues `delta` px. Returns the distance to apply now: `delta` when `direct`, else 0. A direct delta against the
// direction of a glide in flight also cancels that glide.
[[nodiscard]] float ccord_scroller_push(ccord_scroller *s, float delta, bool direct);

// Advances the glide by dt_ms and returns the distance to apply this frame. `reduced` pays out everything at once.
[[nodiscard]] float ccord_scroller_advance(ccord_scroller *s, float dt_ms, bool reduced);

// push + apply for an offset clamped to [0, max]; keeps offset + pending inside the range.
void ccord_scroller_scroll(ccord_scroller *s, float *offset, float delta, float max, bool direct);

// advance + apply for an offset clamped to [0, max]. Returns true while the glide still has distance to travel.
bool ccord_scroller_tick(ccord_scroller *s, float *offset, float dt_ms, float max, bool reduced);
