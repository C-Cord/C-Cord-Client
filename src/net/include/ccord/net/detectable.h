// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>

#include <stddef.h>
#include <stdint.h>

// Detectable-applications list transport: GET /applications/detectable (public, no token) through the project's REST
// queue, cached on disk and refreshed at most once per CCORD_DETECTABLE_REFRESH_S. This module only moves bytes: the
// caller parses the body (ccord_game_list_load) and says whether it was usable. Main thread only, never blocks:
// the request is a non-blocking ccord_rest_queue job pumped from ccord_detectable_poll.
//
//   - A fresh cache (file younger than the refresh interval) means no request at all.
//   - A failed fetch (network, HTTP status, empty body, or a body the caller rejects) keeps the cache file untouched and
//     retries after an exponential backoff (CCORD_DETECTABLE_RETRY_MIN_MS doubling up to CCORD_DETECTABLE_RETRY_MAX_MS).
//   - Mock mode serves a small built-in fixture instead of touching the network.
#define CCORD_DETECTABLE_PATH "/applications/detectable"
#define CCORD_DETECTABLE_CACHE_FILE "detectable.json"
#define CCORD_DETECTABLE_REFRESH_S (24 * 60 * 60)
#define CCORD_DETECTABLE_RETRY_MIN_MS 30000u
#define CCORD_DETECTABLE_RETRY_MAX_MS (60u * 60u * 1000u)
#define CCORD_DETECTABLE_MAX_BYTES (24u * 1024u * 1024u)

typedef struct ccord_detectable ccord_detectable;

// Delivers a fetched body (valid only during the call; copy it). The receiver must answer with
// ccord_detectable_result() once it knows whether the body was usable.
typedef void (*ccord_detectable_body_cb)(void *ud, const char *body, size_t len);

typedef struct ccord_detectable_config {
  const char *cache_dir;       // directory of CCORD_DETECTABLE_CACHE_FILE; null = no on-disk cache (age tracked in memory)
  bool mock;                   // serve the built-in fixture, no network
  uint32_t retry_min_ms;       // 0 -> CCORD_DETECTABLE_RETRY_MIN_MS
  uint32_t retry_max_ms;       // 0 -> CCORD_DETECTABLE_RETRY_MAX_MS
  ccord_detectable_body_cb on_body;
  void *ud;
} ccord_detectable_config;

[[nodiscard]] ccord_detectable *ccord_detectable_create(const ccord_detectable_config *cfg, ccord_error *err);
void ccord_detectable_destroy(ccord_detectable *d);   // nullptr ok; abandons a request in flight

// Drives the refresh schedule. `now_ms` is the app's monotonic clock, `now_unix_s` the wall clock (cache age).
// With `enabled` false nothing is requested and nothing is pumped except a request already in flight.
void ccord_detectable_poll(ccord_detectable *d, uint64_t now_ms, int64_t now_unix_s, bool enabled);

// The receiver's verdict on the last body: usable (cache refreshed by the caller) or not (backoff, old cache kept).
void ccord_detectable_result(ccord_detectable *d, bool usable, uint64_t now_ms);

// Number of requests started so far (network or fixture). Test hook: a disabled feature must leave it unchanged.
size_t ccord_detectable_request_count(const ccord_detectable *d);

// True while a request or a delivered-but-unanswered body is outstanding.
bool ccord_detectable_busy(const ccord_detectable *d);

// Cache file helpers (the caller runs these off the UI thread; both are blocking file I/O).
// Reads the cache: returns a malloc'ed NUL-terminated body (free with ccord_free) or nullptr (not found / unreadable /
// over the size cap). `*mtime_s` gets the file's modification time when the call succeeds.
[[nodiscard]] char *ccord_detectable_cache_read(const char *dir, size_t *len, int64_t *mtime_s, ccord_error *err);
// Atomically replaces the cache (write tmp + rename): a crash or a full disk never leaves a half-written file.
[[nodiscard]] ccord_status ccord_detectable_cache_write(const char *dir, const char *body, size_t len, ccord_error *err);

// The mock fixture list: three applications. "Fixture Quest" (id 900000000000000001, linux "fixturequest",
// win32 "FixtureQuest.exe"), "Pixel Racer" (id 900000000000000002, win32 "pixelracer.exe") and a launcher entry.
const char *ccord_detectable_fixture(size_t *len);
