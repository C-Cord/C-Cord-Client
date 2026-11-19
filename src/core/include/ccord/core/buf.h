// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>
typedef struct ccord_buf { char *data; size_t len, cap, max; bool failed; } ccord_buf;
void ccord_buf_init(ccord_buf *b, size_t max_cap);   // max_cap 0 = 64 MiB
void ccord_buf_free(ccord_buf *b);
void ccord_buf_clear(ccord_buf *b);
void ccord_buf_shrink(ccord_buf *b, size_t max_keep); // clears; frees backing buffer if cap > max_keep
[[nodiscard]] ccord_status ccord_buf_append(ccord_buf *b, const void *p, size_t n);  // CCORD_ERR_RANGE past max, sets failed
[[nodiscard]] ccord_status ccord_buf_append_str(ccord_buf *b, const char *s);
CCORD_PRINTF(2, 3) ccord_status ccord_buf_appendf(ccord_buf *b, const char *fmt, ...);
const char *ccord_buf_cstr(ccord_buf *b);            // always NUL-terminated ("" if empty/failed)
char *ccord_buf_take(ccord_buf *b, size_t *len);     // caller frees with ccord_free; buf reset
