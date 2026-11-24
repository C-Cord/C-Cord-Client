// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/config.h>

typedef enum ccord_timefmt_mode : uint8_t {
  CCORD_TIMEFMT_AUTO = 0,
  CCORD_TIMEFMT_12H = 1,
  CCORD_TIMEFMT_24H = 2,
} ccord_timefmt_mode;

// Process-wide clock display preference (Auto = 12-hour default, 12-hour, or 24-hour).
void ccord_timefmt_set_mode(ccord_timefmt_mode mode);
ccord_timefmt_mode ccord_timefmt_get_mode(void);

// Local timezone offset from UTC in minutes at `ts_ms` (DST-aware; e.g. -360 for UTC-6).
int32_t ccord_timefmt_tz_offset_min(int64_t ts_ms);

// Timestamp text for message headers. No SDL.
//   same calendar day as `now`      -> "Today at 2:34 PM"
//   the day before                  -> "Yesterday at 2:34 PM"
//   otherwise                       -> "MM/DD/YYYY"
// use_utc: format in UTC (deterministic tests/screenshots); otherwise local time zone.
// Returns the length written (NUL-terminated, truncated to cap-1).
size_t ccord_timefmt_message(char *out, size_t cap, int64_t ts_ms, int64_t now_ms, bool use_utc);

// "2:34 PM" or "14:34" (compact rows reveal this on hover).
size_t ccord_timefmt_clock(char *out, size_t cap, int64_t ts_ms, bool use_utc);

// "2:34:05 PM" or "14:34:05" (Discord style "T").
size_t ccord_timefmt_clock_secs(char *out, size_t cap, int64_t ts_ms, bool use_utc);

// True when `a_ms` and `b_ms` fall on the same calendar day in the chosen zone.
bool ccord_timefmt_same_day(int64_t a_ms, int64_t b_ms, bool use_utc);

// "November 10, 2026" (for day dividers in the message transcript).
size_t ccord_timefmt_date(char *out, size_t cap, int64_t ts_ms, bool use_utc);

// "Tuesday, November 10, 2026 2:34 PM" (for timestamp hover tooltips).
size_t ccord_timefmt_full(char *out, size_t cap, int64_t ts_ms, bool use_utc);

// Native Discord timestamp formatter shared between UI and plugin host.
// `style`: nullptr / "" / "message", "clock", "t", "T", "d", "D", "f", "F", or "R".
// Returns false if `style` is unrecognized.
bool ccord_timefmt_discord(char *out, size_t cap, int64_t ts_ms, int64_t now_ms,
                           const char *style, bool use_utc, size_t *out_len);
