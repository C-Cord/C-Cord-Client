// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/config.h>

// SHA-256 (FIPS 180-4) with no TLS or third-party dependency. Used for the PKCE S256 challenge.
typedef struct ccord_sha256_ctx {
  uint32_t state[8];
  uint64_t bitlen;
  unsigned char block[64];
  size_t block_len;
} ccord_sha256_ctx;

void ccord_sha256_init(ccord_sha256_ctx *ctx);
void ccord_sha256_update(ccord_sha256_ctx *ctx, const void *data, size_t len);
// Writes the digest and wipes the context.
void ccord_sha256_final(ccord_sha256_ctx *ctx, unsigned char out[32]);
// One-shot equivalent of init/update/final.
void ccord_sha256(const void *data, size_t len, unsigned char out[32]);
