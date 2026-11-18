// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Voice transport encryption ("rtpsize" AEAD modes) and RTP header helpers. Pure functions, no I/O.
//
// Discord voice gateway v8 only offers these transport modes (xsalsa20_* was discontinued 2024-11-18):
//   aead_aes256_gcm_rtpsize          preferred (libsodium crypto_aead_aes256gcm when the CPU supports it,
//                                    OpenSSL EVP otherwise)
//   aead_xchacha20_poly1305_rtpsize  mandatory (libsodium)
// Packet layout (both modes):
//   RTP fixed header (12) | CSRC list (4*CC) | [extension preamble 0xBEDE + length words (4), if X=1]   <- AAD
//   AEAD(ciphertext of: [extension body (4*length words)] | payload) | 16-byte tag
//   32-bit big-endian nonce counter                                                                      <- appended
// The nonce is the 4 counter bytes (as on the wire) zero-padded to the AEAD nonce size (12 for AES-GCM, 24 for
// XChaCha20). The extension *body* is encrypted but the preamble is not, so after decryption the receiver strips
// the extension body from the plaintext.
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>

#define CCORD_VOICE_KEY_BYTES 32u
#define CCORD_VOICE_TAG_BYTES 16u
#define CCORD_VOICE_NONCE_SUFFIX_BYTES 4u
#define CCORD_RTP_FIXED_HEADER 12u

typedef enum ccord_voice_mode : uint8_t {
  CCORD_VOICE_MODE_NONE = 0,
  CCORD_VOICE_MODE_AES256_GCM_RTPSIZE,
  CCORD_VOICE_MODE_XCHACHA20_RTPSIZE,
} ccord_voice_mode;

const char *ccord_voice_mode_name(ccord_voice_mode m);                 // "aead_aes256_gcm_rtpsize", ...; "" for NONE
ccord_voice_mode ccord_voice_mode_from_name(const char *name);         // NONE when unknown/removed (e.g. xsalsa20_*)
// Picks the best supported mode from the server's list: aes256_gcm first (always usable: OpenSSL fallback when
// libsodium lacks hardware AES), then xchacha20. NONE if neither is offered.
ccord_voice_mode ccord_voice_mode_choose(const char *const *names, size_t count);
bool ccord_voice_aes_hw_available(void);                               // crypto_aead_aes256gcm_is_available()

// Parsed unencrypted RTP header geometry.
typedef struct ccord_rtp_view {
  uint8_t version, payload_type, csrc_count;
  bool marker, has_extension;
  uint16_t seq;
  uint32_t timestamp, ssrc;
  size_t aad_len;       // fixed header + CSRCs + (extension preamble when X=1): the AEAD associated data
  size_t ext_body_len;  // bytes of extension body at the start of the decrypted plaintext (0 without X)
} ccord_rtp_view;

[[nodiscard]] ccord_status ccord_rtp_parse(const uint8_t *pkt, size_t len, ccord_rtp_view *out, ccord_error *err);
// Writes a 12-byte RTP v2 header (no CSRC/extension) in network byte order; returns bytes written (0 if cap < 12).
size_t ccord_rtp_write_header(uint8_t *out, size_t cap, uint8_t payload_type, bool marker, uint16_t seq,
                              uint32_t timestamp, uint32_t ssrc);
// Builds the 12/24-byte AEAD nonce for a counter. Returns the nonce length. Exposed for tests.
size_t ccord_voice_nonce(ccord_voice_mode mode, uint32_t counter, uint8_t out[24]);

// Encrypts `plain` with `header` (header_len bytes, the AAD, usually the 12-byte RTP header) and writes
// header | ciphertext | tag | counter_be32 to `out`. Returns CCORD_ERR_RANGE if cap is too small.
[[nodiscard]] ccord_status ccord_voice_seal(ccord_voice_mode mode, const uint8_t key[CCORD_VOICE_KEY_BYTES],
                                            uint32_t counter, const uint8_t *header, size_t header_len,
                                            const uint8_t *plain, size_t plain_len, uint8_t *out, size_t cap,
                                            size_t *out_len, ccord_error *err);
// Verifies and decrypts a whole received RTP packet. The plaintext (extension body already stripped) is written
// to `plain`; `view` (optional) receives the parsed header.
[[nodiscard]] ccord_status ccord_voice_open(ccord_voice_mode mode, const uint8_t key[CCORD_VOICE_KEY_BYTES],
                                            const uint8_t *pkt, size_t len, uint8_t *plain, size_t cap,
                                            size_t *plain_len, ccord_rtp_view *view, ccord_error *err);
