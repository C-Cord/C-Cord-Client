// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Motion verification (--verify-slice motion); see verify_ui_parity.h. Runs the four area checks in order.
// Each area prints `motion <area>: ok` or `motion <area>: FAIL`; the PASS line appears only when all four pass.
#include "verify_ui_parity.h"

#include <stddef.h>
#include <stdio.h>

typedef int (*motion_area_fn)(const ccord_verify_ui_parity_ctx *ctx, int *checks);

typedef struct motion_area {
  const char *name;
  motion_area_fn fn;
} motion_area;

int ccord_verify_motion_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  static const motion_area areas[] = {
      {"overlays", ccord_verify_motion_overlays},
      {"nav", ccord_verify_motion_nav},
      {"chat", ccord_verify_motion_chat},
      {"controls", ccord_verify_motion_controls},
  };
  int checks = 0;
  int failed = 0;
  for (size_t i = 0; i < sizeof areas / sizeof areas[0]; i++) {
    if (areas[i].fn(ctx, &checks) == 0) {
      printf("motion %s: ok\n", areas[i].name);
    } else {
      printf("motion %s: FAIL\n", areas[i].name);
      failed = 1;
    }
  }
  fflush(stdout);
  if (failed) return 1;
  printf("motion: PASS checks=%d\n", checks);
  fflush(stdout);
  return 0;
}
