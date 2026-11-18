// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/net/voice_crypto.h>

#include <string.h>

#include <openssl/evp.h>
#include <sodium.h>

const char *ccord_voice_mode_name(ccord_voice_mode m)
{
  switch (m) {
    case CCORD_VOICE_MODE_AES256_GCM_RTPSIZE: return "aead_aes256_gcm_rtpsize";
    case CCORD_VOICE_MODE_XCHACHA20_RTPSIZE: return "aead_xchacha20_poly1305_rtpsize";
    default: return "";
  }
}

ccord_voice_mode ccord_voice_mode_from_name(const char *name)
{
  if (!name) return CCORD_VOICE_MODE_NONE;
  if (strcmp(name, "aead_aes256_gcm_rtpsize") == 0) return CCORD_VOICE_MODE_AES256_GCM_RTPSIZE;
  if (strcmp(name, "aead_xchacha20_poly1305_rtpsize") == 0) return CCORD_VOICE_MODE_XCHACHA20_RTPSIZE;
  return CCORD_VOICE_MODE_NONE;
}

ccord_voice_mode ccord_voice_mode_choose(const char *const *names, size_t count)
{
  bool aes = false, xchacha = false;
  for (size_t i = 0; names && i < count; i++) {
    ccord_voice_mode m = ccord_voice_mode_from_name(names[i]);
    if (m == CCORD_VOICE_MODE_AES256_GCM_RTPSIZE) aes = true;
    if (m == CCORD_VOICE_MODE_XCHACHA20_RTPSIZE) xchacha = true;
  }
  if (aes) return CCORD_VOICE_MODE_AES256_GCM_RTPSIZE;
  if (xchacha) return CCORD_VOICE_MODE_XCHACHA20_RTPSIZE;
  return CCORD_VOICE_MODE_NONE;
}

bool ccord_voice_aes_hw_available(void)
{
  if (sodium_init() < 0) return false;
  return crypto_aead_aes256gcm_is_available() != 0;
}

ccord_status ccord_rtp_parse(const uint8_t *pkt, size_t len, ccord_rtp_view *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!pkt || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rtp: null argument");
  if (len < CCORD_RTP_FIXED_HEADER) return ccord_error_set(err, CCORD_ERR_PARSE, "rtp: short packet");
  memset(out, 0, sizeof *out);
  out->version = (uint8_t)(pkt[0] >> 6);
  if (out->version != 2) return ccord_error_set(err, CCORD_ERR_PARSE, "rtp: bad version");
  out->has_extension = (pkt[0] & 0x10u) != 0;
  out->csrc_count = (uint8_t)(pkt[0] & 0x0Fu);
  out->marker = (pkt[1] & 0x80u) != 0;
  out->payload_type = (uint8_t)(pkt[1] & 0x7Fu);
  out->seq = (uint16_t)(((uint16_t)pkt[2] << 8) | pkt[3]);
  out->timestamp = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) | ((uint32_t)pkt[6] << 8) | pkt[7];
  out->ssrc = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) | ((uint32_t)pkt[10] << 8) | pkt[11];
  size_t aad = CCORD_RTP_FIXED_HEADER + (size_t)out->csrc_count * 4u;
  if (out->has_extension) {
    if (len < aad + 4u) return ccord_error_set(err, CCORD_ERR_PARSE, "rtp: truncated extension header");
    size_t words = ((size_t)pkt[aad + 2u] << 8) | pkt[aad + 3u];
    out->ext_body_len = words * 4u;
    aad += 4u;
  }
  if (len < aad) return ccord_error_set(err, CCORD_ERR_PARSE, "rtp: truncated csrc list");
  out->aad_len = aad;
  return CCORD_OK;
}

size_t ccord_rtp_write_header(uint8_t *out, size_t cap, uint8_t payload_type, bool marker, uint16_t seq,
                              uint32_t timestamp, uint32_t ssrc)
{
  if (!out || cap < CCORD_RTP_FIXED_HEADER) return 0;
  out[0] = 0x80;
  out[1] = (uint8_t)((payload_type & 0x7Fu) | (marker ? 0x80u : 0u));
  out[2] = (uint8_t)(seq >> 8);
  out[3] = (uint8_t)seq;
  out[4] = (uint8_t)(timestamp >> 24);
  out[5] = (uint8_t)(timestamp >> 16);
  out[6] = (uint8_t)(timestamp >> 8);
  out[7] = (uint8_t)timestamp;
  out[8] = (uint8_t)(ssrc >> 24);
  out[9] = (uint8_t)(ssrc >> 16);
  out[10] = (uint8_t)(ssrc >> 8);
  out[11] = (uint8_t)ssrc;
  return CCORD_RTP_FIXED_HEADER;
}

static size_t nonce_size(ccord_voice_mode mode)
{
  return mode == CCORD_VOICE_MODE_AES256_GCM_RTPSIZE ? 12u : 24u;
}

static void counter_bytes(uint32_t counter, uint8_t out[4])
{
  out[0] = (uint8_t)(counter >> 24);
  out[1] = (uint8_t)(counter >> 16);
  out[2] = (uint8_t)(counter >> 8);
  out[3] = (uint8_t)counter;
}

size_t ccord_voice_nonce(ccord_voice_mode mode, uint32_t counter, uint8_t out[24])
{
  size_t n = nonce_size(mode);
  memset(out, 0, 24);
  counter_bytes(counter, out);
  return n;
}

// AES-256-GCM through OpenSSL; output is ciphertext || 16-byte tag.
static bool ossl_gcm_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                          const uint8_t *in, size_t in_len, uint8_t *out)
{
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  bool ok = false;
  int n = 0;
  if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) goto done;
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1) goto done;
  if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1) goto done;
  if (aad_len > 0 && EVP_EncryptUpdate(ctx, nullptr, &n, aad, (int)aad_len) != 1) goto done;
  if (in_len > 0 && EVP_EncryptUpdate(ctx, out, &n, in, (int)in_len) != 1) goto done;
  if (EVP_EncryptFinal_ex(ctx, out + in_len, &n) != 1) goto done;
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out + in_len) != 1) goto done;
  ok = true;
done:
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

static bool ossl_gcm_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                          const uint8_t *in, size_t in_len, uint8_t *out)
{
  if (in_len < 16) return false;
  size_t ct_len = in_len - 16;
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  bool ok = false;
  int n = 0;
  uint8_t tag[16];
  memcpy(tag, in + ct_len, 16);
  if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) goto done;
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1) goto done;
  if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1) goto done;
  if (aad_len > 0 && EVP_DecryptUpdate(ctx, nullptr, &n, aad, (int)aad_len) != 1) goto done;
  if (ct_len > 0 && EVP_DecryptUpdate(ctx, out, &n, in, (int)ct_len) != 1) goto done;
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag) != 1) goto done;
  ok = EVP_DecryptFinal_ex(ctx, out + ct_len, &n) == 1;
done:
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

ccord_status ccord_voice_seal(ccord_voice_mode mode, const uint8_t key[CCORD_VOICE_KEY_BYTES], uint32_t counter,
                              const uint8_t *header, size_t header_len, const uint8_t *plain, size_t plain_len,
                              uint8_t *out, size_t cap, size_t *out_len, ccord_error *err)
{
  ccord_error_clear(err);
  if (!key || !header || (!plain && plain_len > 0) || !out || !out_len ||
      (mode != CCORD_VOICE_MODE_AES256_GCM_RTPSIZE && mode != CCORD_VOICE_MODE_XCHACHA20_RTPSIZE)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: seal arguments");
  }
  if (plain_len > 65535u || header_len > 255u) return ccord_error_set(err, CCORD_ERR_RANGE, "voice: packet too large");
  size_t total = header_len + plain_len + CCORD_VOICE_TAG_BYTES + CCORD_VOICE_NONCE_SUFFIX_BYTES;
  if (cap < total) return ccord_error_set(err, CCORD_ERR_RANGE, "voice: seal output too small");
  if (sodium_init() < 0) return ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: sodium init failed");
  uint8_t nonce[24];
  (void)ccord_voice_nonce(mode, counter, nonce);
  uint8_t *ct = out + header_len;
  memmove(out, header, header_len);  // header may alias nothing in practice; memmove tolerates overlap
  if (mode == CCORD_VOICE_MODE_AES256_GCM_RTPSIZE) {
    if (crypto_aead_aes256gcm_is_available()) {
      unsigned long long clen = 0;
      if (crypto_aead_aes256gcm_encrypt(ct, &clen, plain, plain_len, out, header_len, nullptr, nonce, key) != 0) {
        return ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: aes-gcm seal failed");
      }
    } else if (!ossl_gcm_seal(key, nonce, out, header_len, plain, plain_len, ct)) {
      return ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: aes-gcm (openssl) seal failed");
    }
  } else {
    unsigned long long clen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(ct, &clen, plain, plain_len, out, header_len, nullptr, nonce,
                                                   key) != 0) {
      return ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: xchacha20 seal failed");
    }
  }
  counter_bytes(counter, out + header_len + plain_len + CCORD_VOICE_TAG_BYTES);
  *out_len = total;
  return CCORD_OK;
}

ccord_status ccord_voice_open(ccord_voice_mode mode, const uint8_t key[CCORD_VOICE_KEY_BYTES], const uint8_t *pkt,
                              size_t len, uint8_t *plain, size_t cap, size_t *plain_len, ccord_rtp_view *view,
                              ccord_error *err)
{
  ccord_error_clear(err);
  if (!key || !pkt || !plain || !plain_len ||
      (mode != CCORD_VOICE_MODE_AES256_GCM_RTPSIZE && mode != CCORD_VOICE_MODE_XCHACHA20_RTPSIZE)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: open arguments");
  }
  ccord_rtp_view v;
  ccord_status st = ccord_rtp_parse(pkt, len, &v, err);
  if (st != CCORD_OK) return st;
  if (len < v.aad_len + CCORD_VOICE_TAG_BYTES + CCORD_VOICE_NONCE_SUFFIX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "voice: short encrypted packet");
  }
  size_t sealed_len = len - v.aad_len - CCORD_VOICE_NONCE_SUFFIX_BYTES;  // ciphertext + tag
  size_t body = sealed_len - CCORD_VOICE_TAG_BYTES;
  if (cap < body) return ccord_error_set(err, CCORD_ERR_RANGE, "voice: open buffer too small");
  if (v.ext_body_len > body) return ccord_error_set(err, CCORD_ERR_PARSE, "voice: extension longer than payload");
  const uint8_t *suffix = pkt + len - CCORD_VOICE_NONCE_SUFFIX_BYTES;
  uint32_t counter = ((uint32_t)suffix[0] << 24) | ((uint32_t)suffix[1] << 16) | ((uint32_t)suffix[2] << 8) | suffix[3];
  if (sodium_init() < 0) return ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: sodium init failed");
  uint8_t nonce[24];
  (void)ccord_voice_nonce(mode, counter, nonce);
  // Decrypt into a scratch the size of the full plaintext, then drop the extension body.
  const uint8_t *sealed = pkt + v.aad_len;
  bool ok;
  if (mode == CCORD_VOICE_MODE_AES256_GCM_RTPSIZE) {
    if (crypto_aead_aes256gcm_is_available()) {
      unsigned long long mlen = 0;
      ok = crypto_aead_aes256gcm_decrypt(plain, &mlen, nullptr, sealed, sealed_len, pkt, v.aad_len, nonce, key) == 0;
    } else {
      ok = ossl_gcm_open(key, nonce, pkt, v.aad_len, sealed, sealed_len, plain);
    }
  } else {
    unsigned long long mlen = 0;
    ok = crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &mlen, nullptr, sealed, sealed_len, pkt, v.aad_len, nonce,
                                                    key) == 0;
  }
  if (!ok) return ccord_error_set(err, CCORD_ERR_PARSE, "voice: authentication failed");
  if (v.ext_body_len > 0) {
    memmove(plain, plain + v.ext_body_len, body - v.ext_body_len);
    body -= v.ext_body_len;
  }
  if ((pkt[0] & 0x20u) != 0) {
    if (body == 0) return ccord_error_set(err, CCORD_ERR_PARSE, "voice: empty padded RTP payload");
    size_t pad_len = plain[body - 1u];
    if (pad_len == 0 || pad_len > body) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "voice: invalid RTP padding length");
    }
    body -= pad_len;
  }
  *plain_len = body;
  if (view) *view = v;
  return CCORD_OK;
}
