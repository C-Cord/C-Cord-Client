// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>

// Opaque bounded buffer for credentials. Never copy a ccord_secret by value.
typedef struct ccord_secret ccord_secret;

// cap must be nonzero. Returns nullptr with CCORD_ERR_INVALID or CCORD_ERR_NOMEM.
[[nodiscard]] ccord_secret *ccord_secret_create(size_t cap, ccord_error *e);
// Replaces the value. A zero-length set clears the secret. Over-cap returns CCORD_ERR_RANGE and leaves the
// previous value unchanged.
[[nodiscard]] ccord_status ccord_secret_set(ccord_secret *s, const void *data, size_t len, ccord_error *e);
// Pointer to the internal bytes (NUL-terminated for convenience, never assumed by callers); valid until the next
// set/wipe/destroy. Returns nullptr only when s is nullptr; *len receives the value length.
const char *ccord_secret_view(const ccord_secret *s, size_t *len);
bool ccord_secret_present(const ccord_secret *s);
// Zeroes every capacity byte and sets the length to 0.
void ccord_secret_wipe(ccord_secret *s);
// Zeroes and frees.
void ccord_secret_destroy(ccord_secret *s);
