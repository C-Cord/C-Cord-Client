// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <uchar.h>
#include <ccord/core/config.h>
bool ccord_utf8_valid(const char *s, size_t len);       // rejects overlong, surrogates, > U+10FFFF
bool ccord_utf8_valid_text(const char *s, size_t len);  // valid + no C0/C1 controls except \n and \t, no U+0000
int ccord_utf8_decode(const char *s, size_t len, char32_t *cp);  // bytes consumed, or -1
size_t ccord_utf8_encode(char32_t cp, char out[static 4]);       // bytes written, 0 if invalid
size_t ccord_utf8_next(const char *s, size_t len, size_t pos);   // next boundary > pos (or len)
size_t ccord_utf8_prev(const char *s, size_t pos);               // previous boundary < pos (or 0)
