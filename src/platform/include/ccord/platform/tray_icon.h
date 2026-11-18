// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Tray icon pixels: the app icon, plus an unread badge once implemented.
#include <stddef.h>
#include <stdint.h>

// Renders size*size straight-alpha RGBA bytes for the tray. count = unread count (0 = none), has_unread = unread
// without a known count (e.g. unread channels, no mentions). Same buffer rules as ccord_app_icon_rgba.
[[nodiscard]] bool ccord_tray_icon_rgba(int size, int count, bool has_unread, uint8_t *out, size_t out_len);
