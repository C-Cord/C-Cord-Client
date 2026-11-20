// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Game detection app wiring: keeps the detectable-applications list (src/net/detectable.h: cached GET
// /applications/detectable), scans the process table every scan_interval_ms with the pure matcher
// (ccord/rpc/game_detect.h) and publishes the running game as one Playing (type 0) activity. app_rpc.c merges that
// activity with the RPC clients' activities (ccord_game_detect_app_merge), so it travels the same path into
// ccord_session_set_rich_presence.
//
// Blocking work (reading + parsing the list, scanning /proc, writing the cache) runs on one worker thread, so a frame
// never stalls; poll() only moves finished results into the main-thread state. Everything is gated by the client pref
// ccord_ui_pref_game_detection() (via cfg.enabled): while it is off nothing is scanned or fetched and the detected activity is gone
// at once; turning it on again resumes on the next poll.
#include <ccord/core/error.h>
#include <ccord/rpc/activity.h>

#include <stddef.h>
#include <stdint.h>

#define CCORD_GAME_DETECT_SCAN_MS 15000u

typedef struct ccord_game_detect_app_config {
  bool mock;                    // the detectable "request" is served from the built-in fixture (no network)
  const char *cache_dir;        // where detectable.json is cached; null = no cache (the list is refetched per run)
  uint32_t scan_interval_ms;    // 0 -> CCORD_GAME_DETECT_SCAN_MS
  uint32_t retry_min_ms;        // fetch backoff start; 0 -> detectable.h default
  // The user's on/off setting, read on every poll (main wires ccord_ui_pref_game_detection). null = always on.
  bool (*enabled)(void *ud);
  void *enabled_ud;
  // Internal test hooks (the verify slice and unit tests); never set from user input.
  const char *proc_root;        // process table root; null -> /proc
  bool synchronous;             // run worker jobs inline inside poll() instead of on the worker thread
} ccord_game_detect_app_config;

typedef struct ccord_game_detect_app ccord_game_detect_app;

[[nodiscard]] ccord_game_detect_app *ccord_game_detect_app_create(const ccord_game_detect_app_config *cfg, ccord_error *err);
void ccord_game_detect_app_destroy(ccord_game_detect_app *g);   // nullptr ok; joins the worker

// Drives the cache load, list refresh and scan schedule. `now_ms` is the app's monotonic clock. Never blocks (outside
// synchronous mode). Returns true when the detected activity appeared, changed or went away since the last call.
bool ccord_game_detect_app_poll(ccord_game_detect_app *g, uint64_t now_ms);

// The currently detected game as an activity (type 0, name, application_id, start_ms); false when none.
bool ccord_game_detect_app_current(const ccord_game_detect_app *g, ccord_rpc_activity *out);

// Appends the detected activity to acts[0..n) unless an IPC activity already covers that application (same
// application_id, or same name ignoring case) or `cap` is reached. Returns the new count. g == nullptr is a no-op.
size_t ccord_game_detect_app_merge(const ccord_game_detect_app *g, ccord_rpc_activity *acts, size_t n, size_t cap);

// Test hooks: processes scans run so far, and list requests started.
size_t ccord_game_detect_app_scan_count(const ccord_game_detect_app *g);
size_t ccord_game_detect_app_request_count(const ccord_game_detect_app *g);
// True once a usable list is loaded (cache or fetch).
bool ccord_game_detect_app_list_ready(const ccord_game_detect_app *g);
