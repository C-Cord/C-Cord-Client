// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/config.h>
uint64_t ccord_clock_ms(void);          // CLOCK_MONOTONIC
uint64_t ccord_clock_boottime_ms(void); // CLOCK_BOOTTIME (includes suspend time on Linux/Android; CLOCK_MONOTONIC elsewhere)
int64_t ccord_clock_unix_ms(void);      // CLOCK_REALTIME
