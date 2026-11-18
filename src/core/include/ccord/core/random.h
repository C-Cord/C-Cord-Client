// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>

// Fills buf with len bytes from the OS CSPRNG (getrandom(2)); retries on EINTR. Never uses rand().
// Returns CCORD_ERR_IO when the operating system cannot supply random bytes.
[[nodiscard]] ccord_status ccord_random_bytes(void *buf, size_t len);
