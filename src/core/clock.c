// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/core/clock.h>

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
