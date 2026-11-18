// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Voice-message encoder tests: 1 s of 440 Hz sine becomes a valid Ogg/Opus stream that the existing decoder
// reads back as ~1 s; argument validation, the 10 minute cap and misuse after finish are checked too.
// Encode asserts are skipped (not failed) when this build has no libopus encoder / Ogg muxer.
#include <ccord/platform/platform.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/error.h>

#include "ccord_test.h"

#define RATE 48000u
#define PI_D 3.14159265358979323846
// Decoder output is always 48 kHz stereo: 2 interleaved samples per frame.
#define DEC_CAP (RATE * 4u * 2u)

static void sine(int16_t *out, size_t frames, size_t channels)
{
  for (size_t i = 0; i < frames; i++) {
    int16_t s = (int16_t)(sin(2.0 * PI_D * 440.0 * (double)i / (double)RATE) * 12000.0);
    for (size_t c = 0; c < channels; c++) out[i * channels + c] = s;
  }
}

static int peak_abs(const int16_t *pcm, size_t samples)
{
  int peak = 0;
  for (size_t i = 0; i < samples; i++) {
    int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
    if (v > peak) peak = v;
  }
  return peak;
}

static const uint8_t *find_bytes(const uint8_t *hay, size_t hay_len, const char *needle)
{
  size_t n = strlen(needle);
  for (size_t i = 0; i + n <= hay_len; i++) {
    if (memcmp(hay + i, needle, n) == 0) return hay + i;
  }
  return nullptr;
}

static uint64_t le(const uint8_t *p, int bytes)
{
  uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

// Granule position of the last Ogg page minus the OpusHead pre-skip = the exact decoded length in frames.
static int64_t ogg_opus_frames(const uint8_t *ogg, size_t len)
{
  const uint8_t *head = find_bytes(ogg, len, "OpusHead");
  if (!head || head + 12 > ogg + len) return -1;
  uint64_t pre_skip = le(head + 10, 2);
  const uint8_t *last = nullptr;
  for (size_t i = 0; i + 27 <= len; i++) {
    if (memcmp(ogg + i, "OggS", 4) == 0) last = ogg + i;
  }
  if (!last) return -1;
  return (int64_t)le(last + 6, 8) - (int64_t)pre_skip;
}

// Feeds `total` frames in `chunk`-sized pieces (not a multiple of the 960-frame codec frame on purpose).
static ccord_status feed_all(ccord_sound_encoder *e, const int16_t *pcm, size_t total, size_t chunk, size_t channels,
                             ccord_error *err)
{
  for (size_t done = 0; done < total;) {
    size_t n = total - done < chunk ? total - done : chunk;
    ccord_status st = ccord_sound_encoder_feed(e, pcm + done * channels, n, err);
    if (st != CCORD_OK) return st;
    done += n;
  }
  return CCORD_OK;
}

static void check_roundtrip(uint32_t channels, size_t chunk, int16_t *pcm, int16_t *dec)
{
  ccord_error err;
  ccord_sound_encoder *e = ccord_sound_encoder_create(RATE, channels, &err);
  CT_CHECK(e != nullptr);
  if (!e) return;
  CT_CHECK(feed_all(e, pcm, RATE, chunk, channels, &err) == CCORD_OK);

  uint8_t *ogg = nullptr;
  size_t ogg_len = 0;
  CT_CHECK(ccord_sound_encoder_finish(e, &ogg, &ogg_len, &err) == CCORD_OK);
  if (ogg) {
    CT_CHECK(ogg_len > 200u && ogg_len < 40000u);  // ~4 KB at 32 kbps for 1 s; never a raw-PCM sized blob
    CT_CHECK(memcmp(ogg, "OggS", 4) == 0);
    CT_CHECK(find_bytes(ogg, ogg_len, "OpusHead") != nullptr);
    CT_CHECK(find_bytes(ogg, ogg_len, "OpusTags") != nullptr);
    // Last granule - pre-skip says exactly how many frames a player will produce.
    CT_CHECK(ogg_opus_frames(ogg, ogg_len) == (int64_t)RATE);

    size_t n = 0;
    ccord_status st = ccord_sound_decode(ogg, ogg_len, dec, DEC_CAP, &n, &err);
    CT_CHECK(st == CCORD_OK);
    CT_CHECK(n >= 94000u && n <= 98000u);  // 1.0 s of interleaved stereo (96000) within codec-delay slack
    CT_CHECK(peak_abs(dec, n) > 4000);
  }
  ccord_free(ogg);
  ccord_sound_encoder_destroy(e);
}

int main(void)
{
  ccord_error err;

  // Argument validation runs before any codec lookup, so it holds even in a build without libopus.
  CT_CHECK(ccord_sound_encoder_create(44100u, 1u, &err) == nullptr);
  CT_CHECK(err.code == CCORD_ERR_INVALID);
  CT_CHECK(ccord_sound_encoder_create(RATE, 0u, &err) == nullptr);
  CT_CHECK(err.code == CCORD_ERR_INVALID);
  CT_CHECK(ccord_sound_encoder_create(RATE, 3u, &err) == nullptr);
  CT_CHECK(err.code == CCORD_ERR_INVALID);
  ccord_sound_encoder_destroy(nullptr);  // no-op

  ccord_sound_encoder *e = ccord_sound_encoder_create(RATE, 1u, &err);
  if (!e) {
    CT_CHECK(err.code == CCORD_ERR_UNSUPPORTED);
    CT_CHECK(err.message[0] != '\0');
    fprintf(stdout, "test_sound_encode: Opus encoder/Ogg muxer unavailable, encode asserts skipped\n");
    return CT_RESULT("test_sound_encode");
  }

  int16_t *pcm = malloc((size_t)RATE * 2u * sizeof *pcm);
  int16_t *dec = malloc(DEC_CAP * sizeof *dec);
  if (!pcm || !dec) {
    ccord_sound_encoder_destroy(e);
    free(pcm);
    free(dec);
    return 77;
  }

  // Misuse before any audio: no empty Ogg files; nothing is handed out.
  uint8_t *ogg = (uint8_t *)pcm;
  size_t ogg_len = 99;
  CT_CHECK(ccord_sound_encoder_finish(e, &ogg, &ogg_len, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ogg == nullptr && ogg_len == 0);
  CT_CHECK(ccord_sound_encoder_feed(e, nullptr, 10, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_sound_encoder_feed(e, nullptr, 0, &err) == CCORD_OK);  // empty feed is a harmless no-op

  // The 10 minute cap rejects a feed whole, before reading it, and leaves the encoder usable.
  CT_CHECK(ccord_sound_encoder_feed(e, pcm, (size_t)RATE * 600u + 1u, &err) == CCORD_ERR_RANGE);
  CT_CHECK(err.message[0] != '\0');
  CT_CHECK(ccord_sound_encoder_feed(e, pcm, SIZE_MAX, &err) == CCORD_ERR_RANGE);
  sine(pcm, RATE, 1u);
  CT_CHECK(feed_all(e, pcm, 5000u, 1000u, 1u, &err) == CCORD_OK);
  CT_CHECK(ccord_sound_encoder_finish(e, &ogg, &ogg_len, &err) == CCORD_OK);
  CT_CHECK(ogg != nullptr && ogg_len > 0);
  CT_CHECK(ogg_opus_frames(ogg, ogg_len) == 5000);
  ccord_free(ogg);
  // Finished encoders refuse more work.
  CT_CHECK(ccord_sound_encoder_feed(e, pcm, 960u, &err) == CCORD_ERR_INVALID);
  ogg = nullptr;
  CT_CHECK(ccord_sound_encoder_finish(e, &ogg, &ogg_len, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ogg == nullptr);
  ccord_sound_encoder_destroy(e);

  // Destroying mid-recording releases everything (leak-checked under valgrind/ASan runs).
  e = ccord_sound_encoder_create(RATE, 1u, &err);
  CT_CHECK(e != nullptr);
  if (e) {
    CT_CHECK(ccord_sound_encoder_feed(e, pcm, 1500u, &err) == CCORD_OK);
    ccord_sound_encoder_destroy(e);
  }

  // The acceptance case: 1 s of 440 Hz, mono, fed in odd 1000-frame pieces.
  check_roundtrip(1u, 1000u, pcm, dec);
  // One big feed, and stereo (interleaved) through the same path.
  check_roundtrip(1u, RATE, pcm, dec);
  int16_t *st_pcm = malloc((size_t)RATE * 2u * sizeof *st_pcm);
  if (st_pcm) {
    sine(st_pcm, RATE, 2u);
    check_roundtrip(2u, 441u, st_pcm, dec);
    free(st_pcm);
  }

  free(pcm);
  free(dec);
  return CT_RESULT("test_sound_encode");
}
