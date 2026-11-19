// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>
// asset dir resolution: $CCORD_ASSET_DIR, then <exe_dir>/../share/ccord, then compile-time CCORD_DEFAULT_ASSET_DIR (source tree assets/).
[[nodiscard]] ccord_status ccord_paths_init(const char *argv0, ccord_error *err);
const char *ccord_paths_exe_path(void);
const char *ccord_paths_exe_dir(void);
const char *ccord_paths_asset_dir(void);
const char *ccord_paths_data_dir(void);     // $XDG_DATA_HOME/ccord or ~/.local/share/ccord; override via ccord_paths_set_data_dir
void ccord_paths_set_data_dir(const char *dir);
// Joins base + "/" + rel into out. Rejects absolute rel, any ".." segment, NUL, overflow (CCORD_ERR_INVALID/RANGE).
[[nodiscard]] ccord_status ccord_paths_join(char *out, size_t cap, const char *base, const char *rel, ccord_error *err);

// Embedded fallback assets compiled into the binary for single-binary deployment.
const char *ccord_embedded_theme(size_t *out_len);
const unsigned char *ccord_embedded_font(const char *rel_path, size_t *out_len);
bool ccord_embedded_materialize_extensions(const char *dest_dir);
