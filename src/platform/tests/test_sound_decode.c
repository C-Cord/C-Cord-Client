// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Soundboard decoder tests: MP3 and OGG-Vorbis probe-decode to 48 kHz stereo S16,
// in-memory WAV support, capacity truncation, and ccord_error on empty/corrupt input.
#include <ccord/platform/platform.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/error.h>

#include "ccord_test.h"

#define SOUND_CAP (120000u)
#define PI_D 3.14159265358979323846

static uint8_t *read_fixture(const char *rel, size_t *out_len)
{
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", CCORD_SOURCE_DIR, rel);
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open fixture %s\n", path);
    return nullptr;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return nullptr;
  }
  long n = ftell(f);
  if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return nullptr;
  }
  size_t len = (size_t)n;
  uint8_t *buf = malloc(len);
  if (!buf || fread(buf, 1, len, f) != len) {
    free(buf);
    fclose(f);
    return nullptr;
  }
  fclose(f);
  *out_len = len;
  return buf;
}

static void put_u16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
  p[2] = (uint8_t)((v >> 16) & 0xffu);
  p[3] = (uint8_t)((v >> 24) & 0xffu);
}

// Minimal 48 kHz stereo S16 PCM WAV container with a 440 Hz tone.
static size_t build_wav(uint8_t *buf, size_t frames)
{
  uint32_t data_bytes = (uint32_t)(frames * 4u);
  memcpy(buf, "RIFF", 4);
  put_u32(buf + 4, 36u + data_bytes);
  memcpy(buf + 8, "WAVE", 4);
  memcpy(buf + 12, "fmt ", 4);
  put_u32(buf + 16, 16u);
  put_u16(buf + 20, 1u);       // PCM
  put_u16(buf + 22, 2u);       // stereo
  put_u32(buf + 24, 48000u);   // sample rate
  put_u32(buf + 28, 192000u);  // byte rate
  put_u16(buf + 32, 4u);       // block align
  put_u16(buf + 34, 16u);      // bits per sample
  memcpy(buf + 36, "data", 4);
  put_u32(buf + 40, data_bytes);
  for (size_t i = 0; i < frames; i++) {
    double phase = 2.0 * PI_D * 440.0 * (double)i / 48000.0;
    int16_t s = (int16_t)(sin(phase) * 12000.0);
    memcpy(buf + 44u + i * 4u, &s, sizeof s);
    memcpy(buf + 46u + i * 4u, &s, sizeof s);
  }
  return 44u + (size_t)data_bytes;
}

static int peak_abs(const int16_t *pcm, size_t samples)
{
  int peak = 0;
  for (size_t i = 0; i < samples; i++) {
    int v = pcm[i];
    if (v < 0) v = -v;
    if (v > peak) peak = v;
  }
  return peak;
}

int main(void)
{
  size_t mp3_len = 0;
  size_t ogg_len = 0;
  uint8_t *mp3 = read_fixture("src/platform/tests/fixtures/sound.mp3", &mp3_len);
  uint8_t *ogg = read_fixture("src/platform/tests/fixtures/sound.ogg", &ogg_len);
  if (!mp3 || !ogg) {
    free(mp3);
    free(ogg);
    return 77;
  }
  int16_t *pcm = malloc(SOUND_CAP * sizeof *pcm);
  uint8_t *wav = malloc(44u + 4800u * 4u);
  if (!pcm || !wav) {
    free(mp3);
    free(ogg);
    free(pcm);
    free(wav);
    return 77;
  }
  ccord_error err;

  // MP3 (44.1 kHz source) must be resampled to 48 kHz stereo.
  size_t n = 0;
  ccord_status st = ccord_sound_decode(mp3, mp3_len, pcm, SOUND_CAP, &n, &err);
  CT_CHECK(st == CCORD_OK);
  CT_CHECK(n >= 90000u && n <= 100000u);  // 1.0 s at 48 kHz interleaved stereo
  CT_CHECK(n >= 90000u ? peak_abs(pcm, n) > 1000 : false);

  // OGG-Vorbis takes the same path through the probe/demux/decoder.
  n = 0;
  st = ccord_sound_decode(ogg, ogg_len, pcm, SOUND_CAP, &n, &err);
  CT_CHECK(st == CCORD_OK);
  CT_CHECK(n >= 90000u && n <= 100000u);
  CT_CHECK(n >= 90000u ? peak_abs(pcm, n) > 1000 : false);

  // WAV (already 48 kHz stereo) decodes without resampling loss.
  size_t wav_len = build_wav(wav, 4800u);
  n = 0;
  st = ccord_sound_decode(wav, wav_len, pcm, SOUND_CAP, &n, &err);
  CT_CHECK(st == CCORD_OK);
  CT_CHECK(n >= 9500u && n <= 9700u);
  CT_CHECK(peak_abs(pcm, n < SOUND_CAP ? n : SOUND_CAP) > 1000);

  // A container with no audio frames is a parse error, not an empty success.
  size_t empty_wav_len = build_wav(wav, 0u);
  n = 12345;
  st = ccord_sound_decode(wav, empty_wav_len, pcm, SOUND_CAP, &n, &err);
  CT_CHECK(st != CCORD_OK);
  CT_CHECK(n == 0);
  CT_CHECK(err.message[0] != '\0');

  // A capacity too small for the clip reports a range error with the written prefix.
  int16_t tiny[100];
  n = 12345;
  st = ccord_sound_decode(mp3, mp3_len, tiny, 100u, &n, &err);
  CT_CHECK(st == CCORD_ERR_RANGE);
  CT_CHECK(n == 100u);
  CT_CHECK(err.message[0] != '\0');

  // Empty input is an argument error and clears the sample count.
  n = 12345;
  st = ccord_sound_decode(mp3, 0, pcm, SOUND_CAP, &n, &err);
  CT_CHECK(st == CCORD_ERR_INVALID);
  CT_CHECK(n == 0);
  CT_CHECK(err.message[0] != '\0');

  // Random bytes populate ccord_error and decode nothing.
  static const uint8_t junk[64] = {
      0x9e, 0x37, 0x79, 0xb9, 0x7f, 0x4a, 0x7c, 0x15, 0x8f, 0x2c, 0x31, 0x6d, 0x52, 0x0b, 0xa3, 0xe4,
      0xde, 0xad, 0xbe, 0xef, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc,
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
      0xf0, 0xe0, 0xd0, 0xc0, 0xb0, 0xa0, 0x90, 0x80, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10, 0x00};
  n = 12345;
  st = ccord_sound_decode(junk, sizeof junk, pcm, SOUND_CAP, &n, &err);
  CT_CHECK(st != CCORD_OK);
  CT_CHECK(n == 0);
  CT_CHECK(err.code != CCORD_OK);
  CT_CHECK(err.message[0] != '\0');

  free(mp3);
  free(ogg);
  free(pcm);
  free(wav);
  return CT_RESULT("test_sound_decode");
}
