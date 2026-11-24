// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>

#include <ccord/net/voice_crypto.h>

#include "ccord_test.h"

static void test_mode_negotiation(void)
{
  CT_CHECK(ccord_voice_mode_from_name(nullptr) == CCORD_VOICE_MODE_NONE);
  CT_CHECK(ccord_voice_mode_from_name("xsalsa20_poly1305") == CCORD_VOICE_MODE_NONE);
  CT_CHECK(ccord_voice_mode_from_name("aead_aes256_gcm_rtpsize") == CCORD_VOICE_MODE_AES256_GCM_RTPSIZE);
  CT_CHECK(ccord_voice_mode_from_name("aead_xchacha20_poly1305_rtpsize") == CCORD_VOICE_MODE_XCHACHA20_RTPSIZE);
  CT_CHECK(strcmp(ccord_voice_mode_name(CCORD_VOICE_MODE_AES256_GCM_RTPSIZE), "aead_aes256_gcm_rtpsize") == 0);
  CT_CHECK(strcmp(ccord_voice_mode_name(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE), "aead_xchacha20_poly1305_rtpsize") == 0);

  const char *both[] = {"aead_xchacha20_poly1305_rtpsize", "aead_aes256_gcm_rtpsize"};
  ccord_voice_mode expect_both = ccord_voice_aes_hw_available() ? CCORD_VOICE_MODE_AES256_GCM_RTPSIZE
                                                                : CCORD_VOICE_MODE_XCHACHA20_RTPSIZE;
  CT_CHECK(ccord_voice_mode_choose(both, 2) == expect_both);

  const char *aes_only[] = {"xsalsa20_poly1305", "aead_aes256_gcm_rtpsize"};
  CT_CHECK(ccord_voice_mode_choose(aes_only, 2) == CCORD_VOICE_MODE_AES256_GCM_RTPSIZE);

  const char *xchacha_only[] = {"xsalsa20_poly1305", "aead_xchacha20_poly1305_rtpsize"};
  CT_CHECK(ccord_voice_mode_choose(xchacha_only, 2) == CCORD_VOICE_MODE_XCHACHA20_RTPSIZE);

  const char *legacy_only[] = {"xsalsa20_poly1305"};
  CT_CHECK(ccord_voice_mode_choose(legacy_only, 1) == CCORD_VOICE_MODE_NONE);
}

static void test_rtp_header_and_extensions(void)
{
  ccord_error err;
  uint8_t hdr[32];
  size_t n = ccord_rtp_write_header(hdr, sizeof hdr, 120u, true, 0x1234u, 0x56789ABCu, 0xCAFEBABEu);
  CT_CHECK(n == 12u);

  ccord_rtp_view view;
  CT_CHECK(ccord_rtp_parse(hdr, n, &view, &err) == CCORD_OK);
  CT_CHECK(view.version == 2u);
  CT_CHECK(view.payload_type == 120u);
  CT_CHECK(view.marker);
  CT_CHECK(!view.has_extension);
  CT_CHECK(view.csrc_count == 0u);
  CT_CHECK(view.seq == 0x1234u);
  CT_CHECK(view.timestamp == 0x56789ABCu);
  CT_CHECK(view.ssrc == 0xCAFEBABEu);
  CT_CHECK(view.aad_len == 12u);
  CT_CHECK(view.ext_body_len == 0u);

  // Add 1 CSRC + RFC 5285 0xBEDE extension preamble (2 words = 8 bytes extension body)
  uint8_t ext_pkt[24] = {0};
  memcpy(ext_pkt, hdr, 12);
  ext_pkt[0] = 0x91u; // V=2, X=1, CC=1
  // 4-byte CSRC at [12..15], 4-byte extension preamble at [16..19]: 0xBE 0xDE 0x00 0x02
  ext_pkt[16] = 0xBEu;
  ext_pkt[17] = 0xDEu;
  ext_pkt[18] = 0x00u;
  ext_pkt[19] = 0x02u;
  CT_CHECK(ccord_rtp_parse(ext_pkt, 20, &view, &err) == CCORD_OK);
  CT_CHECK(view.has_extension);
  CT_CHECK(view.csrc_count == 1u);
  CT_CHECK(view.aad_len == 20u);
  CT_CHECK(view.ext_body_len == 8u);
}

static void test_aead_roundtrip(ccord_voice_mode mode)
{
  ccord_error err;
  uint8_t key[CCORD_VOICE_KEY_BYTES];
  for (size_t i = 0; i < sizeof key; i++) key[i] = (uint8_t)(0x30u + i);

  uint8_t rtp[12];
  CT_CHECK(ccord_rtp_write_header(rtp, sizeof rtp, 120u, false, 42u, 960u, 7777u) == 12u);

  const uint8_t opus_payload[11] = {0xF8u, 0xFFu, 0xFEu, 1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t wire[128];
  size_t wire_len = 0;
  CT_CHECK(ccord_voice_seal(mode, key, 0x01020304u, rtp, sizeof rtp, opus_payload, sizeof opus_payload, wire,
                            sizeof wire, &wire_len, &err) == CCORD_OK);
  CT_CHECK(wire_len == 12u + sizeof opus_payload + CCORD_VOICE_TAG_BYTES + CCORD_VOICE_NONCE_SUFFIX_BYTES);
  // Trailing 4 bytes must be big-endian counter 0x01020304
  CT_CHECK(wire[wire_len - 4] == 0x01u && wire[wire_len - 3] == 0x02u &&
           wire[wire_len - 2] == 0x03u && wire[wire_len - 1] == 0x04u);

  uint8_t opened[128];
  size_t opened_len = 0;
  ccord_rtp_view view;
  CT_CHECK(ccord_voice_open(mode, key, wire, wire_len, opened, sizeof opened, &opened_len, &view, &err) == CCORD_OK);
  CT_CHECK(opened_len == sizeof opus_payload);
  CT_CHECK(memcmp(opened, opus_payload, sizeof opus_payload) == 0);
  CT_CHECK(view.seq == 42u && view.ssrc == 7777u);

  // Tampering with AAD (RTP header) or ciphertext must fail authentication
  wire[3] ^= 0x01u;
  CT_CHECK(ccord_voice_open(mode, key, wire, wire_len, opened, sizeof opened, &opened_len, &view, &err) != CCORD_OK);
  wire[3] ^= 0x01u;
  wire[14] ^= 0x80u;
  CT_CHECK(ccord_voice_open(mode, key, wire, wire_len, opened, sizeof opened, &opened_len, &view, &err) != CCORD_OK);
}

static void test_aead_with_encrypted_extension_body(void)
{
  ccord_error err;
  uint8_t key[CCORD_VOICE_KEY_BYTES];
  for (size_t i = 0; i < sizeof key; i++) key[i] = (uint8_t)(0xA0u + i);

  // RTP header with X=1 and 1-word (4-byte) extension preamble in AAD
  uint8_t aad[16] = {
    0x90u, 0x78u, 0x00u, 0x09u,
    0x00u, 0x00u, 0x03u, 0xC0u,
    0x00u, 0x00u, 0x10u, 0x01u,
    0xBEu, 0xDEu, 0x00u, 0x01u,
  };
  // Plaintext inside AEAD starts with 4-byte extension body followed by 3-byte Opus payload
  const uint8_t inner_plain[7] = {0x10u, 0xAAu, 0x00u, 0x00u, 0xF8u, 0xFFu, 0xFEu};
  uint8_t wire[128];
  size_t wire_len = 0;
  CT_CHECK(ccord_voice_seal(CCORD_VOICE_MODE_AES256_GCM_RTPSIZE, key, 99u, aad, sizeof aad, inner_plain,
                            sizeof inner_plain, wire, sizeof wire, &wire_len, &err) == CCORD_OK);

  uint8_t payload[64];
  size_t payload_len = 0;
  ccord_rtp_view view;
  CT_CHECK(ccord_voice_open(CCORD_VOICE_MODE_AES256_GCM_RTPSIZE, key, wire, wire_len, payload, sizeof payload,
                            &payload_len, &view, &err) == CCORD_OK);
  // Extension body (4 bytes) is automatically stripped by ccord_voice_open!
  CT_CHECK(payload_len == 3u);
  CT_CHECK(payload[0] == 0xF8u && payload[1] == 0xFFu && payload[2] == 0xFEu);
}

int main(void)
{
  test_mode_negotiation();
  test_rtp_header_and_extensions();
  test_aead_roundtrip(CCORD_VOICE_MODE_AES256_GCM_RTPSIZE);
  test_aead_roundtrip(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE);
  test_aead_with_encrypted_extension_body();
  return CT_RESULT("test_voice_crypto");
}
