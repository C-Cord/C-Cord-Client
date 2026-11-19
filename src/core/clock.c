// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/core/clock.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint64_t ccord_clock_ms(void)
{
  static LARGE_INTEGER freq;
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return (uint64_t)((now.QuadPart * 1000LL) / (freq.QuadPart > 0 ? freq.QuadPart : 1LL));
}

int64_t ccord_clock_unix_ms(void)
{
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
  // Convert 100-ns ticks since 1601-01-01 to Unix epoch ms (1970-01-01)
  const uint64_t epoch_diff = 116444736000000000ULL;
  if (t < epoch_diff) return 0;
  return (int64_t)((t - epoch_diff) / 10000ULL);
}
#else
#include <time.h>

static uint64_t to_ms(const struct timespec *ts)
{
  return (uint64_t)ts->tv_sec * 1000u + (uint64_t)ts->tv_nsec / 1000000u;
}

uint64_t ccord_clock_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return to_ms(&ts);
}

int64_t ccord_clock_unix_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)to_ms(&ts);
}
#endif
