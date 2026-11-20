// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// PipeWire frame capture for the ScreenCast portal (m5-portal part 2).
//
// After ccord_portal_dbus_start() hands back a portal session with a PipeWire remote fd and
// stream node ids, this module connects to that remote, negotiates a raw video format and
// converts incoming frames to tightly packed BGRA8 for ccord_capture_grab().
//
// Two layers, mirroring portal_dbus.h:
//  1. A transport-agnostic reader over an injectable ccord_portal_pw_ops seam, so tests can
//     script raw frames (formats, strides, resizes, timeouts, teardown) deterministically.
//  2. A real libpipewire-0.3 backend (ccord_portal_pw_open_remote). When no PipeWire session
//     is reachable (headless CI) the connect step fails cleanly with an error; nothing blocks
//     forever and no fd leaks.
//
// Threading: a reader (and its seam) must be used from ONE thread at a time, matching the
// ccord_capture handle contract. All waits take an explicit timeout_ms.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/platform/capture.h>

#include "portal_dbus.h"

// ---- raw frame model -------------------------------------------------------------------------

// Wire pixel layouts the portal may negotiate. BGRx is BGRA with an undefined padding byte.
typedef enum ccord_portal_pw_format : uint8_t {
  CCORD_PORTAL_PW_BGRA = 0,
  CCORD_PORTAL_PW_BGRX,
  CCORD_PORTAL_PW_RGBA,
} ccord_portal_pw_format;

// A borrowed raw frame from the producer. `data` stays valid until the next read_frame or
// teardown. `stride` is bytes per row and may exceed width*4 (row padding).
typedef struct ccord_portal_pw_raw {
  const uint8_t *data;
  int width, height;
  int stride;
  ccord_portal_pw_format format;
} ccord_portal_pw_raw;

// Converts a raw frame to tightly packed BGRA8 (row stride dst_w*4) in `dst`, which must hold
// at least dst_w*dst_h*4 bytes. When the destination size differs from the source the frame is
// resized with nearest-neighbour sampling. Returns false on bad arguments (null pointers,
// non-positive sizes, stride < width*4, unknown format); never reads out of bounds.
bool ccord_portal_pw_convert(uint8_t *dst, int dst_w, int dst_h, const ccord_portal_pw_raw *src);

// ---- owned frame staging -----------------------------------------------------------------------
// PipeWire lends the process callback an mmap'd buffer that is ours only until the callback
// returns it with pw_stream_queue_buffer(); the producer may then rewrite it at any time. The
// real backend copies the validated pixels here BEFORE queueing, so a frame handed to grab()
// never aliases memory the producer has taken back. Grow-only: the allocation is reused.
typedef struct ccord_portal_pw_staging {
  uint8_t *data;
  size_t cap;
} ccord_portal_pw_staging;

// Copies `len` bytes from `src` into `st`, growing it as needed. `src` is read only for the
// duration of the call; no pointer into it is retained. false + `st` untouched on bad args/OOM.
[[nodiscard]] bool ccord_portal_pw_stage(ccord_portal_pw_staging *st, const uint8_t *src, size_t len);
// Frees the staging storage and zeroes the struct. NULL-safe.
void ccord_portal_pw_staging_free(ccord_portal_pw_staging *st);

// ---- injectable producer seam ------------------------------------------------------------------

typedef struct ccord_portal_pw_ops {
  void *ud;
  // Connects to the PipeWire remote on `fd` and starts capturing node `node_id`, waiting up to
  // timeout_ms for the stream to negotiate. ALWAYS consumes fd: on failure it is closed, on
  // success the seam owns it until destroy. Returns false with err set on failure.
  bool (*connect)(void *ud, int fd, uint32_t node_id, int timeout_ms, ccord_error *err);
  // Produces the next raw frame, waiting up to timeout_ms. false + CCORD_ERR_TIMEOUT means no
  // frame arrived in time (not fatal); any other error means the stream is dead.
  bool (*read_frame)(void *ud, int timeout_ms, ccord_portal_pw_raw *out, ccord_error *err);
  // Full teardown: releases the remote, closes any owned fd and frees ud. May be NULL. Called
  // once by ccord_portal_pw_destroy(), whether or not connect succeeded.
  void (*destroy)(void *ud);
} ccord_portal_pw_ops;

typedef struct ccord_portal_pw ccord_portal_pw;

// Creates a reader over an injected seam (copied; ud stays owned by the caller until destroy).
// On failure the seam is NOT destroyed. NULL + err on bad ops or OOM.
[[nodiscard]] ccord_portal_pw *ccord_portal_pw_create(const ccord_portal_pw_ops *ops, ccord_error *err);
// Real libpipewire-0.3 backend. The reader is created lazily; the PipeWire remote is only
// contacted by ccord_portal_pw_start(), which fails cleanly when no session is reachable.
[[nodiscard]] ccord_portal_pw *ccord_portal_pw_open_remote(ccord_error *err);
// Starts capture. Consumes `fd` in ALL cases (the seam closes it on failure), so the caller
// must not close it afterwards. Non-positive timeout_ms or a second start is INVALID.
[[nodiscard]] ccord_status ccord_portal_pw_start(ccord_portal_pw *pw, int fd, uint32_t node_id, int timeout_ms,
                                                 ccord_error *err);
// Grabs the newest frame as tightly packed BGRA8, waiting up to timeout_ms. out->data stays
// valid until the next grab or destroy on this reader. false + CCORD_ERR_TIMEOUT = no frame
// yet (retry); false + CCORD_ERR_NOMEM = transient allocation failure; any other error marks
// the reader failed (ccord_portal_pw_failed).
bool ccord_portal_pw_grab(ccord_portal_pw *pw, int timeout_ms, ccord_capture_frame *out, ccord_error *err);
bool ccord_portal_pw_failed(const ccord_portal_pw *pw);
void ccord_portal_pw_destroy(ccord_portal_pw *pw);   // NULL-safe; tears the seam down

// ---- capture_portal wiring seam (tests only) ----------------------------------------------------

// When both seams are set, capture_portal.c negotiates over dbus_ops and reads frames over
// pw_ops instead of the real session bus / PipeWire remote, and reports itself available.
// Pass NULL/NULL to restore the real backends. Not thread-safe: set before opening captures.
void ccord_capture_portal_set_test_seams(const ccord_portal_dbus_ops *dbus_ops, const ccord_portal_pw_ops *pw_ops);
