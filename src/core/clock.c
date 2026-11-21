// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include <ccord/core/clock.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <stdatomic.h>
#include <windows.h>

uint64_t ccord_clock_ms(void)
{
  static _Atomic int64_t s_freq = 0;
  int64_t freq = atomic_load_explicit(&s_freq, memory_order_relaxed);
  if (freq <= 0) {
    LARGE_INTEGER li = {0};
    if (QueryPerformanceFrequency(&li) && li.QuadPart > 0) {
      freq = li.QuadPart;
      atomic_store_explicit(&s_freq, freq, memory_order_relaxed);
    } else {
      freq = 1;
    }
  }
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  uint64_t ticks = now.QuadPart > 0 ? (uint64_t)now.QuadPart : 0u;
  uint64_t uf = (uint64_t)freq;
  return (ticks / uf) * 1000u + ((ticks % uf) * 1000u) / uf;
}

uint64_t ccord_clock_boottime_ms(void)
{
  return ccord_clock_ms();
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

uint64_t ccord_clock_boottime_ms(void)
{
#if defined(CLOCK_BOOTTIME)
  struct timespec ts;
  if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) return to_ms(&ts);
#endif
  return ccord_clock_ms();
}

int64_t ccord_clock_unix_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)to_ms(&ts);
}
#endif
