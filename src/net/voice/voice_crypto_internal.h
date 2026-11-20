// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Internal AEAD cipher state shared between voice_crypto.c and voice_internal.h
// (keeps voice_crypto.c free of libcurl, Opus, and socket headers).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/net/voice_crypto.h>

struct evp_cipher_ctx_st;  // OpenSSL EVP_CIPHER_CTX (voice_crypto.c)

// Cached transport AEAD state (AES-256-GCM expanded key schedule / reusable OpenSSL EVP_CIPHER_CTX,
// or cached XChaCha20-Poly1305 key). Guarded by ccord_voice_gateway.mtx.
typedef struct ccord_voice_aead_ctx {
  ccord_voice_mode mode;
  bool ready;
  bool use_sodium_aes;
  uint8_t key[CCORD_VOICE_KEY_BYTES];
  unsigned char aes_storage[512u + 16u];  // 16-byte-aligned libsodium crypto_aead_aes256gcm_state
  struct evp_cipher_ctx_st *ossl_enc;
  struct evp_cipher_ctx_st *ossl_dec;
} ccord_voice_aead_ctx;

// Cached transport AEAD helpers (voice_crypto.c). Caller holds g->mtx.
[[nodiscard]] ccord_status ccord_voice_aead_init(ccord_voice_aead_ctx *ctx, ccord_voice_mode mode,
                                                 const uint8_t key[CCORD_VOICE_KEY_BYTES], ccord_error *err);
void ccord_voice_aead_clear(ccord_voice_aead_ctx *ctx);
[[nodiscard]] ccord_status ccord_voice_aead_seal(ccord_voice_aead_ctx *ctx, uint32_t counter,
                                                 const uint8_t *header, size_t header_len,
                                                 const uint8_t *plain, size_t plain_len,
                                                 uint8_t *out, size_t cap, size_t *out_len, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_aead_open(ccord_voice_aead_ctx *ctx, const uint8_t *pkt, size_t len,
                                                 uint8_t *plain, size_t cap, size_t *plain_len,
                                                 ccord_rtp_view *view, ccord_error *err);
bool ccord_voice_aead_open_srtcp(ccord_voice_aead_ctx *ctx, const uint8_t *pkt, size_t len,
                                 uint8_t *plain, size_t cap, size_t *out_len);
