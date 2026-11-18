// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>
// Line format: "[section]" | "key = value" | "# comment" (comment only at line start) | blank.
// Keys: [a-z0-9_.]+ ; value: rest of line, trimmed. Limits: line 512 B, 1024 entries. Duplicate key in a section -> error.
typedef bool (*ccord_ini_visit)(const char *section, const char *key, const char *value, int line, void *ud, ccord_error *err);
[[nodiscard]] ccord_status ccord_ini_parse(const char *text, size_t len, ccord_ini_visit visit, void *ud, ccord_error *err);
