// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <stddef.h>

// Embedded fallback assets compiled into the binary for single-binary deployment.
const char *ccord_embedded_theme(size_t *out_len);
const char *ccord_embedded_theme_by_id(const char *id, size_t *out_len);
const unsigned char *ccord_embedded_font(const char *rel_path, size_t *out_len);
bool ccord_embedded_materialize_extensions(const char *dest_dir);
