// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Opus codec and RTP/xsalsa20_poly1305 framing. Split out of discord_gateway.c (m5-split).
#include <ccord/net/live_transport.h>

#include <stdint.h>
#include <string.h>

#if defined(__has_include) && __has_include(<opus/opus.h>)
#include <opus/opus.h>
#else
#include <opus.h>
#endif
#include <sodium.h>

#include <ccord/core/alloc.h>

// ---- Opus codec and RTP/xsalsa20_poly1305 framing ----

struct ccord_voice_codec {
  OpusEncoder *enc;
  OpusDecoder *dec;
};

ccord_voice_codec *ccord_voice_codec_create(ccord_error *err)
{
  ccord_error_clear(err);
  ccord_voice_codec *c = ccord_calloc(1, sizeof *c);
  if (!c) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "voice: codec out of memory");
    return nullptr;
  }
  int opus_err = OPUS_OK;
  c->enc = opus_encoder_create((opus_int32)CCORD_VOICE_SAMPLE_RATE, (int)CCORD_VOICE_CHANNELS,
                               OPUS_APPLICATION_VOIP, &opus_err);
  if (opus_err != OPUS_OK || !c->enc) {
    ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: opus encoder init failed");
    ccord_free(c);
    return nullptr;
  }
  // Decoder is created lazily on the first ccord_voice_decode() call (voice_gateway uses per-SSRC decoders).
  c->dec = nullptr;
  (void)opus_encoder_ctl(c->enc, OPUS_SET_BITRATE(64000));
  (void)opus_encoder_ctl(c->enc, OPUS_SET_COMPLEXITY(5));
  (void)opus_encoder_ctl(c->enc, OPUS_SET_INBAND_FEC(1));
  (void)opus_encoder_ctl(c->enc, OPUS_SET_PACKET_LOSS_PERC(5));
  (void)opus_encoder_ctl(c->enc, OPUS_SET_DTX(1));
  return c;
}

void ccord_voice_codec_destroy(ccord_voice_codec *c)
{
  if (!c) return;
  if (c->enc) opus_encoder_destroy(c->enc);
  if (c->dec) opus_decoder_destroy(c->dec);
  ccord_free(c);
}

ccord_status ccord_voice_encode(ccord_voice_codec *c, const int16_t *pcm, size_t samples,
                                uint8_t *out, size_t cap, size_t *out_len, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !c->enc || !pcm || !out || !out_len) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: encode fields");
  if (samples < 120 || samples > 5760 || cap > (size_t)INT32_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "voice: opus frame size out of range");
  }
  int n = opus_encode(c->enc, pcm, (int)samples, out, (opus_int32)cap);
  if (n < 0) return ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: opus encode failed");
  *out_len = (size_t)n;
  return CCORD_OK;
}

ccord_status ccord_voice_decode(ccord_voice_codec *c, const uint8_t *packet, size_t len,
                                int16_t *pcm, size_t samples, size_t *out_samples, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !packet || !pcm || !out_samples) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: decode fields");
  }
  if (samples < 120 || samples > 5760 || len > (size_t)INT32_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "voice: opus frame size out of range");
  }
  if (!c->dec) {
    int opus_err = OPUS_OK;
    c->dec = opus_decoder_create((opus_int32)CCORD_VOICE_SAMPLE_RATE, (int)CCORD_VOICE_CHANNELS, &opus_err);
    if (opus_err != OPUS_OK || !c->dec) {
      c->dec = nullptr;
      return ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: opus decoder init failed");
    }
  }
  int n = opus_decode(c->dec, packet, (opus_int32)len, pcm, (int)samples, 0);
  if (n < 0) return ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: opus decode failed");
  *out_samples = (size_t)n;
  return CCORD_OK;
}

size_t ccord_voice_rtp_header(uint8_t *out, size_t cap, uint16_t seq, uint32_t timestamp, uint32_t ssrc)
{
  if (!out || cap < CCORD_VOICE_RTP_HEADER_SIZE) return 0;
  out[0] = 0x80;                                  // V=2, no padding/extension/CSRC
  out[1] = (uint8_t)(CCORD_VOICE_RTP_PAYLOAD_TYPE & 0x7Fu);
  out[2] = (uint8_t)(seq >> 8);
  out[3] = (uint8_t)(seq & 0xFFu);
  out[4] = (uint8_t)(timestamp >> 24);
  out[5] = (uint8_t)(timestamp >> 16);
  out[6] = (uint8_t)(timestamp >> 8);
  out[7] = (uint8_t)(timestamp & 0xFFu);
  out[8] = (uint8_t)(ssrc >> 24);
  out[9] = (uint8_t)(ssrc >> 16);
  out[10] = (uint8_t)(ssrc >> 8);
  out[11] = (uint8_t)(ssrc & 0xFFu);
  return CCORD_VOICE_RTP_HEADER_SIZE;
}

ccord_status ccord_voice_encrypt(const uint8_t key[CCORD_VOICE_MAX_SECRET], const uint8_t *header,
                                 size_t header_len, const uint8_t *frame, size_t frame_len,
                                 uint8_t *out, size_t cap, size_t *out_len, ccord_error *err)
{
  ccord_error_clear(err);
  if (!key || !header || !frame || !out || !out_len || header_len != CCORD_VOICE_RTP_HEADER_SIZE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: encrypt fields");
  }
  if (frame_len > SIZE_MAX - header_len - crypto_secretbox_MACBYTES || cap < header_len + frame_len + crypto_secretbox_MACBYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "voice: encrypted packet too large");
  }
  if (sodium_init() < 0) return ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: sodium init failed");
  uint8_t nonce[crypto_secretbox_NONCEBYTES];
  memset(nonce, 0, sizeof nonce);
  memcpy(nonce, header, header_len);              // Discord legacy xsalsa20 nonce = RTP header padded
  memcpy(out, header, header_len);
  if (crypto_secretbox_easy(out + header_len, frame, frame_len, nonce, key) != 0) {
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: encryption failed");
  }
  *out_len = header_len + frame_len + crypto_secretbox_MACBYTES;
  return CCORD_OK;
}

ccord_status ccord_voice_decrypt(const uint8_t key[CCORD_VOICE_MAX_SECRET], const uint8_t *packet,
                                 size_t len, size_t header_len, uint8_t *out, size_t cap,
                                 size_t *out_len, ccord_error *err)
{
  ccord_error_clear(err);
  if (!key || !packet || !out || !out_len ||
      (header_len != 8u && header_len != CCORD_VOICE_RTP_HEADER_SIZE)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: decrypt fields");
  }
  if (len < header_len + crypto_secretbox_MACBYTES) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "voice: short encrypted packet");
  }
  size_t body = len - header_len - crypto_secretbox_MACBYTES;
  if (cap < body) return ccord_error_set(err, CCORD_ERR_RANGE, "voice: decrypt buffer too small");
  if (sodium_init() < 0) return ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: sodium init failed");
  uint8_t nonce[crypto_secretbox_NONCEBYTES];
  memset(nonce, 0, sizeof nonce);
  memcpy(nonce, packet, header_len);
  if (crypto_secretbox_open_easy(out, packet + header_len, len - header_len, nonce, key) != 0) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "voice: authentication failed");
  }
  *out_len = body;
  return CCORD_OK;
}
