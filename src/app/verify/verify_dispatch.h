// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include "app_internal.h"

int ccord_verify_run_slice(app *a, const options *o, const ccord_warm_load_result *warm_res);

// Internal verify slice helpers & entrypoints (verify_m0.c, verify_m4c.c, verify_m4d.c)
bool verify_click_ss(app *a, const char *name);
int run_verify_slice_m0(app *a);
int run_verify_slice_m4c(app *a);
int run_verify_slice_m4d(app *a);
