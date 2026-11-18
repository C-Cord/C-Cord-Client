// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/config.h>
[[nodiscard]] void *ccord_malloc(size_t n);
[[nodiscard]] void *ccord_calloc(size_t count, size_t size);              // overflow-checked
[[nodiscard]] void *ccord_realloc(void *p, size_t n);
[[nodiscard]] void *ccord_alloc_array(size_t count, size_t size);        // nullptr on overflow or OOM
[[nodiscard]] void *ccord_realloc_array(void *p, size_t count, size_t size);
[[nodiscard]] char *ccord_strdup(const char *s);
[[nodiscard]] char *ccord_strndup(const char *s, size_t n);
void ccord_free(void *p);
// Test hook: the n-th next allocation (0 = the next one) returns nullptr; n < 0 disables.
void ccord_alloc_test_fail_after(long n);
