// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <stdbool.h>
#include <stddef.h>

// Standard PKZIP (.zip / .cce) extractor and packer using zlib.
bool ccord_zip_extract(const char *zip_path, const char *dest_dir, char *err_buf, size_t err_cap);
bool ccord_zip_pack_dir(const char *src_dir, const char *zip_path, char *err_buf, size_t err_cap);
