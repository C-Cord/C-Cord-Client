// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/core/timefmt.h>
#include <ccord/core/os.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

static ccord_timefmt_mode g_timefmt_mode = CCORD_TIMEFMT_AUTO;

void ccord_timefmt_set_mode(ccord_timefmt_mode mode)
{
  if (mode <= CCORD_TIMEFMT_24H) g_timefmt_mode = mode;
}

ccord_timefmt_mode ccord_timefmt_get_mode(void)
{
  return g_timefmt_mode;
}

static int64_t floor_div_1000(int64_t ts_ms)
{
  return ts_ms >= 0 ? ts_ms / 1000 : -((-ts_ms + 999) / 1000);
}

static bool to_tm(int64_t ts_ms, bool use_utc, struct tm *out)
{
  time_t t = (time_t)floor_div_1000(ts_ms);
  return use_utc ? ccord_gmtime(t, out) : ccord_localtime(t, out);
}

int32_t ccord_timefmt_tz_offset_min(int64_t ts_ms)
{
  time_t t = (time_t)floor_div_1000(ts_ms);
  struct tm lt = {0};
  if (!ccord_localtime(t, &lt)) return 0;
  struct tm utc_like = lt;
  utc_like.tm_isdst = 0;
  time_t as_utc = ccord_timegm(&utc_like);
  if (as_utc == (time_t)-1) return 0;
  int64_t diff_s = (int64_t)as_utc - (int64_t)t;
  return (int32_t)(diff_s / 60);
}

size_t ccord_timefmt_clock(char *out, size_t cap, int64_t ts_ms, bool use_utc)
{
  if (cap == 0) return 0;
  struct tm tm;
  if (!to_tm(ts_ms, use_utc, &tm)) {
    out[0] = '\0';
    return 0;
  }
  int n;
  if (g_timefmt_mode == CCORD_TIMEFMT_24H) {
    n = snprintf(out, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
  } else {
    int h12 = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
    n = snprintf(out, cap, "%d:%02d %s", h12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
  }
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t ccord_timefmt_clock_secs(char *out, size_t cap, int64_t ts_ms, bool use_utc)
{
  if (cap == 0) return 0;
  struct tm tm;
  if (!to_tm(ts_ms, use_utc, &tm)) {
    out[0] = '\0';
    return 0;
  }
  int n;
  if (g_timefmt_mode == CCORD_TIMEFMT_24H) {
    n = snprintf(out, cap, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
  } else {
    int h12 = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
    n = snprintf(out, cap, "%d:%02d:%02d %s", h12, tm.tm_min, tm.tm_sec, tm.tm_hour < 12 ? "AM" : "PM");
  }
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

bool ccord_timefmt_discord(char *out, size_t cap, int64_t ts_ms, int64_t now_ms,
                           const char *style, bool use_utc, size_t *out_len)
{
  if (out_len) *out_len = 0;
  if (!out || cap == 0) return false;
  out[0] = '\0';
  size_t len = 0;
  if (!style || style[0] == '\0' || strcmp(style, "message") == 0) {
    len = ccord_timefmt_message(out, cap, ts_ms, now_ms, use_utc);
  } else if (strcmp(style, "clock") == 0 || strcmp(style, "t") == 0) {
    len = ccord_timefmt_clock(out, cap, ts_ms, use_utc);
  } else if (strcmp(style, "T") == 0) {
    len = ccord_timefmt_clock_secs(out, cap, ts_ms, use_utc);
  } else if (strcmp(style, "d") == 0) {
    struct tm tm;
    if (to_tm(ts_ms, use_utc, &tm)) {
      int n = snprintf(out, cap, "%02d/%02d/%04d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
      len = (n > 0) ? ((size_t)n < cap ? (size_t)n : cap - 1) : 0;
    }
  } else if (strcmp(style, "D") == 0) {
    len = ccord_timefmt_date(out, cap, ts_ms, use_utc);
  } else if (strcmp(style, "f") == 0) {
    char dpart[48], tpart[32];
    size_t dl = ccord_timefmt_date(dpart, sizeof dpart, ts_ms, use_utc);
    size_t tl = ccord_timefmt_clock(tpart, sizeof tpart, ts_ms, use_utc);
    int n = (dl > 0 && tl > 0) ? snprintf(out, cap, "%s %s", dpart, tpart) : 0;
    len = (n > 0) ? ((size_t)n < cap ? (size_t)n : cap - 1) : 0;
  } else if (strcmp(style, "F") == 0) {
    len = ccord_timefmt_full(out, cap, ts_ms, use_utc);
  } else if (strcmp(style, "R") == 0) {
    int64_t diff = floor_div_1000(now_ms) - floor_div_1000(ts_ms);
    int64_t abs_d = diff < 0 ? -diff : diff;
    int n = 0;
    if (abs_d < 45) {
      n = snprintf(out, cap, "%s", diff >= 0 ? "just now" : "in a few seconds");
    } else if (abs_d < 3600) {
      long m = (long)((abs_d + 30) / 60);
      if (m < 1) m = 1;
      n = diff >= 0 ? snprintf(out, cap, "%ld minute%s ago", m, m == 1 ? "" : "s")
                    : snprintf(out, cap, "in %ld minute%s", m, m == 1 ? "" : "s");
    } else if (abs_d < 86400) {
      long h = (long)((abs_d + 1800) / 3600);
      if (h < 1) h = 1;
      n = diff >= 0 ? snprintf(out, cap, "%ld hour%s ago", h, h == 1 ? "" : "s")
                    : snprintf(out, cap, "in %ld hour%s", h, h == 1 ? "" : "s");
    } else if (abs_d < 30 * 86400) {
      long d = (long)((abs_d + 43200) / 86400);
      if (d < 1) d = 1;
      n = diff >= 0 ? snprintf(out, cap, "%ld day%s ago", d, d == 1 ? "" : "s")
                    : snprintf(out, cap, "in %ld day%s", d, d == 1 ? "" : "s");
    } else if (abs_d < 365 * 86400) {
      long mo = (long)((abs_d + 15 * 86400) / (30 * 86400));
      if (mo < 1) mo = 1;
      n = diff >= 0 ? snprintf(out, cap, "%ld month%s ago", mo, mo == 1 ? "" : "s")
                    : snprintf(out, cap, "in %ld month%s", mo, mo == 1 ? "" : "s");
    } else {
      long yr = (long)((abs_d + 182 * 86400) / (365 * 86400));
      if (yr < 1) yr = 1;
      n = diff >= 0 ? snprintf(out, cap, "%ld year%s ago", yr, yr == 1 ? "" : "s")
                    : snprintf(out, cap, "in %ld year%s", yr, yr == 1 ? "" : "s");
    }
    len = (n > 0) ? ((size_t)n < cap ? (size_t)n : cap - 1) : 0;
  } else {
    return false;
  }
  if (out_len) *out_len = len;
  return true;
}
