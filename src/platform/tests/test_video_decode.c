// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// H.264 Annex-B decoder tests: keyframe -> RGBA dimensions/pixels, resolution changes,
// and clean rejection of empty/corrupt/truncated access units.
#include <ccord/platform/platform.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/error.h>

#include "ccord_test.h"

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

static void check_pixel(const uint8_t *rgba, int w, int x, int y, int r, int g, int b, int tol)
{
  size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 4u;
  int pr = rgba[idx + 0];
  int pg = rgba[idx + 1];
  int pb = rgba[idx + 2];
  CT_CHECK(abs(pr - r) <= tol);
  CT_CHECK(abs(pg - g) <= tol);
  CT_CHECK(abs(pb - b) <= tol);
}

// Encodes one solid-colour keyframe of w x h with the platform encoder into `pkt`; returns its length (0 on failure).
static size_t encode_clip(int w, int h, uint8_t *pkt, size_t cap)
{
  ccord_error err;
  ccord_video_config cfg = {.width = w, .height = h, .fps = 30, .bitrate = 800000, .codec = CCORD_VIDEO_CODEC_H264};
  ccord_video_stream *vs = ccord_video_stream_create(&cfg, &err);
  if (!vs) return 0;
  size_t n = (size_t)w * (size_t)h * 4u;
  uint8_t *rgba = malloc(n);
  size_t len = 0;
  if (rgba) {
    for (size_t i = 0; i < n; i += 4) {
      rgba[i + 0] = 200;
      rgba[i + 1] = 60;
      rgba[i + 2] = 40;
      rgba[i + 3] = 255;
    }
    if (ccord_video_stream_encode_rgba(vs, rgba, w, h, w * 4, pkt, cap, &len, &err) != CCORD_OK) len = 0;
  }
  free(rgba);
  ccord_video_stream_destroy(vs);
  return len;
}

static void test_decoder_max_dim(void)
{
  ccord_error err;
  static uint8_t pkt[1 << 18];
  size_t len = encode_clip(320, 180, pkt, sizeof pkt);
  CT_CHECK(len > 0);
  if (len == 0) return;
  int w = 0;
  int h = 0;

  // No clamp: native size. A clamp of 0 stays native too.
  ccord_video_decoder *d = ccord_video_decoder_create(&err);
  CT_CHECK(d != nullptr);
  if (!d) return;
  ccord_video_decoder_set_max_dim(nullptr, 160, 90);  // nullptr-safe
  CT_CHECK(ccord_video_decoder_feed(d, pkt, len, &err) == CCORD_OK);
  const uint8_t *rgba = ccord_video_decoder_last_rgba(d, &w, &h);
  CT_CHECK(rgba && w == 320 && h == 180);
  ccord_video_decoder_set_max_dim(d, 0, 0);
  CT_CHECK(ccord_video_decoder_feed(d, pkt, len, &err) == CCORD_OK);
  rgba = ccord_video_decoder_last_rgba(d, &w, &h);
  CT_CHECK(rgba && w == 320 && h == 180);

  // Clamp 160x90: takes effect on the next frame, aspect kept.
  ccord_video_decoder_set_max_dim(d, 160, 90);
  CT_CHECK(ccord_video_decoder_feed(d, pkt, len, &err) == CCORD_OK);
  rgba = ccord_video_decoder_last_rgba(d, &w, &h);
  CT_CHECK(rgba && w == 160 && h == 90);
  if (rgba) check_pixel(rgba, w, 80, 45, 200, 60, 40, 30);

  // A box with a different aspect: width-bound, then height-bound; never larger, aspect within 1 px.
  static const int boxes[][2] = {{100, 100}, {400, 50}, {160, 0}, {0, 60}, {1, 1}};
  for (size_t i = 0; i < sizeof boxes / sizeof boxes[0]; i++) {
    ccord_video_decoder_set_max_dim(d, boxes[i][0], boxes[i][1]);
    CT_CHECK(ccord_video_decoder_feed(d, pkt, len, &err) == CCORD_OK);
    rgba = ccord_video_decoder_last_rgba(d, &w, &h);
    CT_CHECK(rgba != nullptr);
    CT_CHECK(w >= 2 && h >= 2 && w <= 320 && h <= 180);
    if (boxes[i][0] > 0) CT_CHECK(w <= boxes[i][0] || w == 2);
    if (boxes[i][1] > 0) CT_CHECK(h <= boxes[i][1] || h == 2);
    if (w > 2 && h > 2) CT_CHECK(abs(h - w * 180 / 320) <= 1);
  }

  // A clip smaller than the clamp is not enlarged.
  ccord_video_decoder_set_max_dim(d, 640, 360);
  CT_CHECK(ccord_video_decoder_feed(d, pkt, len, &err) == CCORD_OK);
  rgba = ccord_video_decoder_last_rgba(d, &w, &h);
  CT_CHECK(rgba && w == 320 && h == 180);

  // Clearing the clamp restores native output.
  ccord_video_decoder_set_max_dim(d, 160, 90);
  CT_CHECK(ccord_video_decoder_feed(d, pkt, len, &err) == CCORD_OK);
  ccord_video_decoder_set_max_dim(d, -1, -1);
  CT_CHECK(ccord_video_decoder_feed(d, pkt, len, &err) == CCORD_OK);
  rgba = ccord_video_decoder_last_rgba(d, &w, &h);
  CT_CHECK(rgba && w == 320 && h == 180);
  ccord_video_decoder_destroy(d);
}

int main(void)
{
  size_t big_len = 0;
  size_t small_len = 0;
  uint8_t *big = read_fixture("src/platform/tests/fixtures/h264_keyframe.h264", &big_len);
  uint8_t *small = read_fixture("src/platform/tests/fixtures/h264_keyframe_small.h264", &small_len);
  if (!big || !small) {
    free(big);
    free(small);
    return 77;
  }

  test_decoder_max_dim();

  ccord_error err;
  ccord_video_decoder *d = ccord_video_decoder_create(&err);
  CT_CHECK(d != nullptr);
  if (!d) {
    free(big);
    free(small);
    return CT_RESULT("test_video_decode");
  }
  CT_CHECK(ccord_video_decoder_last_rgba(d, nullptr, nullptr) == nullptr);

  int w = 0;
  int h = 0;
  CT_CHECK(ccord_video_decoder_feed(d, big, big_len, &err) == CCORD_OK);
  const uint8_t *rgba = ccord_video_decoder_last_rgba(d, &w, &h);
  CT_CHECK(rgba != nullptr);
  CT_CHECK(w == 64);
  CT_CHECK(h == 48);
  if (rgba) check_pixel(rgba, w, 32, 24, 255, 0, 0, 80);

  // Feeding a second keyframe replaces the retained frame.
  CT_CHECK(ccord_video_decoder_feed(d, big, big_len, &err) == CCORD_OK);

  // Resolution change: reallocate and rescale for the smaller stream.
  CT_CHECK(ccord_video_decoder_feed(d, small, small_len, &err) == CCORD_OK);
  rgba = ccord_video_decoder_last_rgba(d, &w, &h);
  CT_CHECK(rgba != nullptr);
  CT_CHECK(w == 32);
  CT_CHECK(h == 24);
  if (rgba) check_pixel(rgba, w, 16, 12, 0, 0, 255, 80);

  // And back up again.
  CT_CHECK(ccord_video_decoder_feed(d, big, big_len, &err) == CCORD_OK);
  rgba = ccord_video_decoder_last_rgba(d, &w, &h);
  CT_CHECK(w == 64);
  CT_CHECK(h == 48);

  // Empty input is an argument error.
  err.code = CCORD_OK;
  err.message[0] = '\0';
  CT_CHECK(ccord_video_decoder_feed(d, big, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(err.code == CCORD_ERR_INVALID);
  CT_CHECK(err.message[0] != '\0');

  // Bytes without an Annex-B start code are rejected before decoding.
  static const uint8_t no_start[] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
  CT_CHECK(ccord_video_decoder_feed(d, no_start, sizeof no_start, &err) == CCORD_ERR_PARSE);
  CT_CHECK(err.message[0] != '\0');

  // A start code followed by an unknown/invalid NAL produces no frame.
  static const uint8_t bad_nal[] = {0x00, 0x00, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff};
  CT_CHECK(ccord_video_decoder_feed(d, bad_nal, sizeof bad_nal, &err) != CCORD_OK);
  CT_CHECK(err.code != CCORD_OK);

  // A start code with only an access-unit delimiter never yields a picture.
  static const uint8_t aud_only[] = {0x00, 0x00, 0x00, 0x01, 0x09, 0xf0};
  CT_CHECK(ccord_video_decoder_feed(d, aud_only, sizeof aud_only, &err) != CCORD_OK);

  // Truncated parameter-set / slice data must not crash or fabricate a frame.
  size_t cut = 8u;
  CT_CHECK(ccord_video_decoder_feed(d, big, cut, &err) != CCORD_OK);

  // Pre-IDR non-IDR P-slice (even with SPS/PPS) on a fresh decoder is gated out instead of fabricating
  // a solid grey YUV(128,128,128) error-concealment frame; a subsequent IDR decodes cleanly.
  {
    ccord_video_decoder *fresh = ccord_video_decoder_create(&err);
    CT_CHECK(fresh != nullptr);
    if (fresh) {
      static const uint8_t pre_idr_p[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,
        0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x24, 0x6C,
      };
      CT_CHECK(ccord_video_decoder_feed(fresh, pre_idr_p, sizeof pre_idr_p, &err) != CCORD_OK);
      CT_CHECK(ccord_video_decoder_last_rgba(fresh, nullptr, nullptr) == nullptr);
      CT_CHECK(ccord_video_decoder_feed(fresh, big, big_len, &err) == CCORD_OK);
      CT_CHECK(ccord_video_decoder_last_rgba(fresh, &w, &h) != nullptr && w == 64 && h == 48);
      ccord_video_decoder_destroy(fresh);
    }
  }

  ccord_video_decoder_destroy(d);
  // Destroying without any decoded frame is safe too.
  ccord_video_decoder *empty = ccord_video_decoder_create(&err);
  CT_CHECK(empty != nullptr);
  ccord_video_decoder_destroy(empty);

  free(big);
  free(small);
  return CT_RESULT("test_video_decode");
}
