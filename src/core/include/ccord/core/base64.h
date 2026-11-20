// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/config.h>
#include <stddef.h>
#include <stdint.h>

// Required buffer size including trailing NUL for encoding `in_len` bytes into standard Base64.
static inline size_t ccord_base64_encoded_cap(size_t in_len)
{
  return 4u * ((in_len + 2u) / 3u) + 1u;
}

// Maximum decoded byte count for an input string of length `in_len`.
static inline size_t ccord_base64_decoded_max(size_t in_len)
{
  return (in_len / 4u) * 3u + 3u;
}

// Encodes `in_len` bytes into NUL-terminated standard Base64 (`+`, `/`, `=`).
// Returns encoded character count (excluding NUL), or 0 if `out_cap` is too small (or on invalid args when in_len > 0).
size_t ccord_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);

// Encodes `in_len` bytes into a newly allocated NUL-terminated Base64 string (caller frees with ccord_free).
[[nodiscard]] char *ccord_base64_encode_alloc(const uint8_t *in, size_t in_len, size_t *out_len);

// Decodes standard or URL-safe (`-`, `_`) Base64, ignoring ASCII whitespace (` `, `\t`, `\r`, `\n`),
// accepting both padded (`=`) and unpadded tails. Returns true on valid input and writes byte count to `*out_len`.
[[nodiscard]] bool ccord_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);

// Decodes Base64 into a newly allocated buffer (caller frees with ccord_free; +1 NUL byte always appended).
// Returns nullptr on invalid Base64 or OOM.
[[nodiscard]] uint8_t *ccord_base64_decode_alloc(const char *in, size_t in_len, size_t *out_len);
