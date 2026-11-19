// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Time-based transitions for an immediate-mode UI.
//
// The drawing code already knows, every frame, what each element should look like *now* (hovered, selected, open).
// An animation here is nothing but a number that eases toward that target instead of jumping to it: the caller says
// "for element K the target is 1.0" every frame and gets back where the value currently is. Elements are told apart by
// a key (ccord_anim_key); there is no registration, no cleanup and no callback.
//
//   float t = ccord_anim_to(&a, ccord_anim_key("chan", id), hovered ? 1.0f : 0.0f, 120);
//   draw_rect(..., ui_alpha(hover_colour, t));
//
// Rules that keep it from looking wrong:
//  - An element seen for the first time (or not drawn in the previous frame) starts AT its target. A row scrolling into
//    view does not fade in from nothing and a selected channel is not "selected" in slow motion after a reload;
//    ccord_anim_enter is the one call that deliberately starts from 0 (popups, dialogs, toasts).
//  - Changing the target mid-flight continues from wherever the value is, so quick hover in/out never snaps.
//  - Nothing moves while nothing is on screen to move: ccord_anim_active() tells the main loop whether another frame
//    is needed, so an idle client stays idle (no timers, no polling).
//  - "Reduced motion" turns every duration into zero.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CCORD_ANIM_SLOTS 512u

typedef struct ccord_anim_slot {
  uint32_t key;          // 0 = free
  float from, to, value;
  uint64_t start_ms;
  uint32_t dur_ms;
  uint64_t seen_frame;
} ccord_anim_slot;

typedef struct ccord_anim_set {
  ccord_anim_slot slots[CCORD_ANIM_SLOTS];
  uint64_t frame;        // draw counter
  uint64_t now_ms;
  bool pending;          // something is still moving after the last frame
  bool reduced_motion;
} ccord_anim_set;

void ccord_anim_init(ccord_anim_set *a);
// Start of a draw pass: advances the frame counter and the clock, forgets last frame's "still moving".
void ccord_anim_begin_frame(ccord_anim_set *a, uint64_t now_ms);
// True when the last frame left something moving: the caller must draw again soon.
[[nodiscard]] bool ccord_anim_active(const ccord_anim_set *a);

// Stable key for "thing `id` of kind `ns`" ("chan", channel id).
[[nodiscard]] uint32_t ccord_anim_key(const char *ns, uint64_t id);

// The element's value this frame, easing (ease-out cubic) toward `target` over `dur_ms`. First sight: snaps to target.
[[nodiscard]] float ccord_anim_to(ccord_anim_set *a, uint32_t key, float target, uint32_t dur_ms);
// 0 -> 1 over `dur_ms` from the moment the element first appears (or reappears after a frame without it).
[[nodiscard]] float ccord_anim_enter(ccord_anim_set *a, uint32_t key, uint32_t dur_ms);
// Like ccord_anim_enter with an overshoot at the end (a "pop"): 0 -> ~1.1 -> 1.
[[nodiscard]] float ccord_anim_pop(ccord_anim_set *a, uint32_t key, uint32_t dur_ms);
// Forgets an element so that its next appearance starts fresh.
void ccord_anim_reset(ccord_anim_set *a, uint32_t key);

// The curves, exposed for callers that animate on their own clock (spinners, waves) and for tests.
[[nodiscard]] float ccord_ease_out(float t);       // cubic, fast start, soft landing: the default for transitions
[[nodiscard]] float ccord_ease_in_out(float t);    // smoothstep-like
[[nodiscard]] float ccord_ease_out_back(float t);  // overshoots 1.0 by ~10 % near the end
