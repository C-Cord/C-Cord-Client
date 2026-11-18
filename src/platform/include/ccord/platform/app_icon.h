// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// C-Cord application icon pixels (window/app icon).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Fills out with size*size straight-alpha RGBA bytes (row-major, 4 bytes/pixel). out_len must be >= size*size*4.
// Returns false (writing nothing) on size < 8 / size > 1024 or a short buffer.
[[nodiscard]] bool ccord_app_icon_rgba(int size, uint8_t *out, size_t out_len);
// The simplified tray variant (mark confined to the top-left so a badge fits bottom-right), same rules.
[[nodiscard]] bool ccord_tray_base_rgba(int size, uint8_t *out, size_t out_len);
