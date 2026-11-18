// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/ui/timefmt.h>

#include <stdio.h>
#include <time.h>

static bool to_tm(int64_t ts_ms, bool use_utc, struct tm *out)
{
  int64_t secs = ts_ms >= 0 ? ts_ms / 1000 : -((-ts_ms + 999) / 1000);
  time_t t = (time_t)secs;
  return (use_utc ? gmtime_r(&t, out) : localtime_r(&t, out)) != nullptr;
}

size_t ccord_timefmt_clock(char *out, size_t cap, int64_t ts_ms, bool use_utc)
{
  if (cap == 0) return 0;
  struct tm tm;
  if (!to_tm(ts_ms, use_utc, &tm)) {
    out[0] = '\0';
    return 0;
  }
  int h12 = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
  int n = snprintf(out, cap, "%d:%02d %s", h12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

// Day number since a fixed origin, used to compare calendar days.
static long day_number(const struct tm *tm)
{
  long y = tm->tm_year + 1900L, m = tm->tm_mon + 1L, d = tm->tm_mday;
  // days from civil (Howard Hinnant)
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe;
}

size_t ccord_timefmt_message(char *out, size_t cap, int64_t ts_ms, int64_t now_ms, bool use_utc)
{
  if (cap == 0) return 0;
  struct tm tm, now;
  if (!to_tm(ts_ms, use_utc, &tm) || !to_tm(now_ms, use_utc, &now)) {
    out[0] = '\0';
    return 0;
  }
  long diff = day_number(&now) - day_number(&tm);
  char clock[24];
  int n;
  if (diff == 0 || diff == 1) {
    ccord_timefmt_clock(clock, sizeof clock, ts_ms, use_utc);
    n = snprintf(out, cap, "%s at %s", diff == 0 ? "Today" : "Yesterday", clock);
  } else {
    n = snprintf(out, cap, "%02d/%02d/%04d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
  }
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

static const char *const k_months[12] = {
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December",
};

static const char *const k_weekdays[7] = {
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

bool ccord_timefmt_same_day(int64_t a_ms, int64_t b_ms, bool use_utc)
{
  struct tm ta, tb;
  if (!to_tm(a_ms, use_utc, &ta) || !to_tm(b_ms, use_utc, &tb)) return false;
  return day_number(&ta) == day_number(&tb);
}

size_t ccord_timefmt_date(char *out, size_t cap, int64_t ts_ms, bool use_utc)
{
  if (cap == 0) return 0;
  struct tm tm;
  if (!to_tm(ts_ms, use_utc, &tm) || tm.tm_mon < 0 || tm.tm_mon >= 12) {
    out[0] = '\0';
    return 0;
  }
  int n = snprintf(out, cap, "%s %d, %04d", k_months[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t ccord_timefmt_full(char *out, size_t cap, int64_t ts_ms, bool use_utc)
{
  if (cap == 0) return 0;
  struct tm tm;
  if (!to_tm(ts_ms, use_utc, &tm) || tm.tm_mon < 0 || tm.tm_mon >= 12 || tm.tm_wday < 0 || tm.tm_wday >= 7) {
    out[0] = '\0';
    return 0;
  }
  char clock[24];
  ccord_timefmt_clock(clock, sizeof clock, ts_ms, use_utc);
  int n = snprintf(out, cap, "%s, %s %d, %04d %s", k_weekdays[tm.tm_wday], k_months[tm.tm_mon],
                   tm.tm_mday, tm.tm_year + 1900, clock);
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}
