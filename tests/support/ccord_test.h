// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Minimal test helpers shared by all C-Cord unit tests. Exit code 0 = pass, 77 = skip.
#pragma once
#include <stdio.h>

[[maybe_unused]] static int ct_failures = 0;

#define CT_CHECK(cond)                                                        \
  do {                                                                        \
    if (!(cond)) {                                                            \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ct_failures++;                                                          \
    }                                                                         \
  } while (0)

#define CT_RESULT(name)                                                 \
  (ct_failures ? (fprintf(stderr, "%s: %d failure(s)\n", name, ct_failures), 1) \
               : (fprintf(stdout, "%s: ok\n", name), 0))
