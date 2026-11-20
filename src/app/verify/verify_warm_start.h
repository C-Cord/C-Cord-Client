// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// --verify-slice warm-start.
//
// Without --warm-phase the slice is the parent: it spawns /proc/self/exe once per phase against one scratch data dir
// (a true restart through the real main() startup), sums the children's `warm-start[<phase>]: PASS checks=N` lines and
// prints `warm-start: PASS checks=<sum>`. With a hidden --warm-phase (cold, warm, corrupt, other-account, off) the
// slice is a child: main() has already run ccord_warm_load before the UI existed, and passes the result here.
#include <stdint.h>

#include "warm_cache.h"

typedef struct ccord_verify_warm_env {
  const char *phase;                 // nullptr = parent
  ccord_warm *warm;                  // the handle main() opened (disabled under --no-warm-cache)
  ccord_warm_load_result load;       // what ccord_warm_load returned before ccord_ui_create
  const char *data_dir;              // ccord_paths_data_dir()
  uint64_t (*now_ms)(void *ud);      // the app's (virtual) millisecond clock
  void *ud;
  void (*sign_out)(void *ud);                      // main.c's real sign-out handler
  void (*remove_account)(void *ud, uint64_t user_id);   // main.c's real account-removal handler
} ccord_verify_warm_env;

void ccord_verify_warm_start_configure(const ccord_verify_warm_env *env);
