// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Unit test for animated image decoding (ccord_anim_*) and the ffmpeg fallback of ccord_image_decode_rgba.
// Needs no SDL video: only the memory decoding paths are exercised. The fixtures below were produced with
// Pillow (16x8 solid red/green/blue frames, delays 50/100/200 ms) and verified with ffprobe.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/render/anim.h>
#include <ccord/render/av_stream.h>
#include <ccord/render/render.h>

#include "ccord_test.h"

static const uint8_t rgb_gif[] = {
  0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
  0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x08,
  0x05, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x15,
  0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1,
  0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00,
  0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08,
  0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08,
  0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00,
  0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c,
  0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03,
  0x02, 0x00, 0x3b,
};

static const uint8_t patch_gif[] = {
  0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0xff, 0xff, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
  0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x04,
  0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x15,
  0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1,
  0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x05, 0x0a, 0x00, 0x02, 0x00, 0x2c, 0x02,
  0x00, 0x02, 0x00, 0x04, 0x00, 0x04, 0x00, 0x81, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x08, 0x09, 0x00, 0x03, 0x08, 0x1c, 0x48, 0xb0, 0x60, 0x80, 0x80, 0x00,
  0x3b,
};

static const uint8_t alpha_gif[] = {
  0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
  0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x09,
  0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x24,
  0x00, 0x01, 0x08, 0x1c, 0x28, 0x30, 0x80, 0xc1, 0x83, 0x06, 0x09, 0x12, 0x44, 0x88, 0x50, 0xe1,
  0x40, 0x86, 0x07, 0x1d, 0x16, 0x84, 0x18, 0x40, 0x22, 0x00, 0x8a, 0x15, 0x25, 0x62, 0xb4, 0xb8,
  0x51, 0x23, 0xc5, 0x80, 0x00, 0x21, 0xf9, 0x04, 0x09, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00,
  0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x08, 0x24, 0x00, 0x01, 0x08, 0x1c, 0x28, 0x30, 0x80, 0xc1, 0x83, 0x06, 0x09,
  0x12, 0x44, 0x88, 0x50, 0xe1, 0x40, 0x86, 0x07, 0x1d, 0x16, 0x84, 0x18, 0x40, 0x22, 0x00, 0x8a,
  0x15, 0x25, 0x62, 0xb4, 0xb8, 0x51, 0x23, 0xc5, 0x80, 0x00, 0x3b,
};

static const uint8_t rgb_apng[] = {
  0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
  0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0xf0, 0x76, 0x7f,
  0x97, 0x00, 0x00, 0x00, 0x08, 0x61, 0x63, 0x54, 0x4c, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
  0x00, 0xce, 0xed, 0xba, 0xc0, 0x00, 0x00, 0x00, 0x1a, 0x66, 0x63, 0x54, 0x4c, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x01, 0x00, 0x14, 0x00, 0x00, 0xfe, 0x15, 0x12, 0x97, 0x00, 0x00, 0x00, 0x1a, 0x49,
  0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xfc, 0xcf, 0xc0, 0xf0, 0x9f, 0x81, 0x02, 0xc0, 0x44, 0x89,
  0xe6, 0x51, 0x03, 0x20, 0x60, 0xe0, 0x03, 0x11, 0x00, 0x19, 0x79, 0x02, 0x0e, 0x56, 0x54, 0x11,
  0x3a, 0x00, 0x00, 0x00, 0x1a, 0x66, 0x63, 0x54, 0x4c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x10, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x0a, 0x00, 0x00, 0x73, 0xde, 0x76, 0x39, 0x00, 0x00, 0x00, 0x1e, 0x66, 0x64, 0x41, 0x54, 0x00,
  0x00, 0x00, 0x02, 0x78, 0x9c, 0x63, 0x64, 0xf8, 0xcf, 0xf0, 0x9f, 0x81, 0x02, 0xc0, 0x44, 0x89,
  0xe6, 0x51, 0x03, 0x20, 0x60, 0xe0, 0x03, 0x11, 0x00, 0x18, 0x7a, 0x02, 0x0e, 0x84, 0x70, 0xef,
  0xa6, 0x00, 0x00, 0x00, 0x1a, 0x66, 0x63, 0x54, 0x4c, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
  0x10, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x05, 0x00, 0x00, 0x95, 0x14, 0xe2, 0xed, 0x00, 0x00, 0x00, 0x1e, 0x66, 0x64, 0x41, 0x54, 0x00,
  0x00, 0x00, 0x04, 0x78, 0x9c, 0x63, 0x64, 0x60, 0xf8, 0xff, 0x9f, 0x81, 0x02, 0xc0, 0x44, 0x89,
  0xe6, 0x51, 0x03, 0x20, 0x60, 0xe0, 0x03, 0x11, 0x00, 0x17, 0x7b, 0x02, 0x0e, 0xf7, 0x85, 0x24,
  0xc3, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

static const uint8_t rgb_webp[] = {
  0x52, 0x49, 0x46, 0x46, 0xb4, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x58,
  0x0a, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x07, 0x00, 0x00, 0x41, 0x4e,
  0x49, 0x4d, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x4e, 0x4d, 0x46,
  0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x07, 0x00, 0x00,
  0x32, 0x00, 0x00, 0x02, 0x56, 0x50, 0x38, 0x4c, 0x0f, 0x00, 0x00, 0x00, 0x2f, 0x0f, 0xc0, 0x01,
  0x00, 0x07, 0x10, 0xfd, 0x8f, 0xfe, 0x07, 0x22, 0xa2, 0xff, 0x01, 0x00, 0x41, 0x4e, 0x4d, 0x46,
  0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x07, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x56, 0x50, 0x38, 0x4c, 0x0f, 0x00, 0x00, 0x00, 0x2f, 0x0f, 0xc0, 0x01,
  0x00, 0x07, 0xd0, 0xff, 0x88, 0xfe, 0x07, 0x22, 0xa2, 0xff, 0x01, 0x00, 0x41, 0x4e, 0x4d, 0x46,
  0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x07, 0x00, 0x00,
  0xc8, 0x00, 0x00, 0x00, 0x56, 0x50, 0x38, 0x4c, 0x0f, 0x00, 0x00, 0x00, 0x2f, 0x0f, 0xc0, 0x01,
  0x00, 0x07, 0x10, 0xd1, 0xff, 0xfe, 0x07, 0x22, 0xa2, 0xff, 0x01, 0x00,
};

static bool near_px(const uint8_t *frame, int w, int x, int y, int r, int g, int b, int a)
{
  const uint8_t *p = frame + ((size_t)y * (size_t)w + (size_t)x) * 4u;
  return abs(p[0] - r) <= 3 && abs(p[1] - g) <= 3 && abs(p[2] - b) <= 3 && abs(p[3] - a) <= 3;
}

static bool frame_is(const ccord_anim *a, size_t i, int r, int g, int b)
{
  if (i >= a->frame_count) return false;
  for (int y = 0; y < a->h; y++) {
    for (int x = 0; x < a->w; x++) {
      if (!near_px(a->frames[i], a->w, x, y, r, g, b, 255)) return false;
    }
  }
  return true;
}

static void check_rgb(const ccord_anim *a, const char *what)
{
  if (a->frame_count != 3) {
    fprintf(stderr, "%s: frame_count=%zu\n", what, a->frame_count);
    ct_failures++;
    return;
  }
  CT_CHECK(a->w == 16 && a->h == 8);
  CT_CHECK(frame_is(a, 0, 255, 0, 0));
  CT_CHECK(frame_is(a, 1, 0, 255, 0));
  CT_CHECK(frame_is(a, 2, 0, 0, 255));
  CT_CHECK(a->delay_ms[0] == 50 && a->delay_ms[1] == 100 && a->delay_ms[2] == 200);
  CT_CHECK(a->total_ms == 350);
}

static void test_sniff(void)
{
  CT_CHECK(ccord_anim_sniff(rgb_gif, sizeof rgb_gif));
  CT_CHECK(ccord_anim_sniff(rgb_apng, sizeof rgb_apng));
  CT_CHECK(ccord_anim_sniff(rgb_webp, sizeof rgb_webp));
  CT_CHECK(!ccord_anim_sniff(nullptr, 0));
  CT_CHECK(!ccord_anim_sniff(rgb_gif, 3));
  CT_CHECK(!ccord_anim_sniff((const uint8_t *)"garbage garbage", 15));
  // Plain PNG: signature, IHDR, then IDAT without acTL.
  static const uint8_t plain_png[] = {0x89, 'P',  'N',  'G',  0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 'I', 'H', 'D',
                                      'R',  0,    0,    0,    1,    0,    0,    0,    1,    8, 6, 0,  0,   0,   0,
                                      0,    0,    0,    0,    0,    0,    0,    4,    'I',  'D', 'A', 'T', 0,  0,
                                      0,    0,    0,    0,    0,    0};
  CT_CHECK(!ccord_anim_sniff(plain_png, sizeof plain_png));
  static const uint8_t jpg[] = {0xff, 0xd8, 0xff, 0xe0, 0, 0x10, 'J', 'F', 'I', 'F', 0};
  CT_CHECK(!ccord_anim_sniff(jpg, sizeof jpg));
  // Truncated APNG before acTL is reached must not read out of bounds.
  for (size_t n = 0; n <= sizeof rgb_apng; n++) (void)ccord_anim_sniff(rgb_apng, n);
  for (size_t n = 0; n <= sizeof rgb_webp; n++) (void)ccord_anim_sniff(rgb_webp, n);
}

static void test_gif(void)
{
  ccord_error err;
  ccord_anim a;
  CT_CHECK(ccord_anim_decode(rgb_gif, sizeof rgb_gif, 0, 0, &a, &err) == CCORD_OK);
  check_rgb(&a, "gif");
  // frame_at looping
  CT_CHECK(ccord_anim_frame_at(&a, 0) == 0 && ccord_anim_frame_at(&a, 49) == 0);
  CT_CHECK(ccord_anim_frame_at(&a, 50) == 1 && ccord_anim_frame_at(&a, 149) == 1);
  CT_CHECK(ccord_anim_frame_at(&a, 150) == 2 && ccord_anim_frame_at(&a, 349) == 2);
  CT_CHECK(ccord_anim_frame_at(&a, 350) == 0 && ccord_anim_frame_at(&a, 350u * 1000u + 60u) == 1);
  CT_CHECK(ccord_anim_frame_at(&a, UINT64_MAX) < 3);
  CT_CHECK(ccord_anim_frame_at(nullptr, 5) == 0);
  ccord_anim_free(&a);
  CT_CHECK(a.frames == nullptr && a.frame_count == 0);
  ccord_anim_free(&a);   // double free is harmless

  // Downscale keeps the aspect ratio.
  CT_CHECK(ccord_anim_decode(rgb_gif, sizeof rgb_gif, 8, 0, &a, &err) == CCORD_OK);
  CT_CHECK(a.w == 8 && a.h == 4 && a.frame_count == 3);
  CT_CHECK(frame_is(&a, 0, 255, 0, 0) && frame_is(&a, 2, 0, 0, 255));
  ccord_anim_free(&a);
  CT_CHECK(ccord_anim_decode(rgb_gif, sizeof rgb_gif, 5, 0, &a, &err) == CCORD_OK);
  CT_CHECK(a.w == 5 && a.h == 3);
  ccord_anim_free(&a);
  // max_dim larger than the image never upscales.
  CT_CHECK(ccord_anim_decode(rgb_gif, sizeof rgb_gif, 100, 0, &a, &err) == CCORD_OK);
  CT_CHECK(a.w == 16 && a.h == 8);
  ccord_anim_free(&a);

  // Frame cap.
  CT_CHECK(ccord_anim_decode(rgb_gif, sizeof rgb_gif, 0, 2, &a, &err) == CCORD_OK);
  CT_CHECK(a.frame_count == 2 && a.total_ms == 150);
  ccord_anim_free(&a);
  CT_CHECK(ccord_anim_decode(rgb_gif, sizeof rgb_gif, 0, 1, &a, &err) == CCORD_OK);
  CT_CHECK(a.frame_count == 1 && ccord_anim_frame_at(&a, 123456) == 0);
  ccord_anim_free(&a);

  // Sub-frame composition with "keep" disposal: the second frame is the first one plus a blue 4x4 patch.
  CT_CHECK(ccord_anim_decode(patch_gif, sizeof patch_gif, 0, 0, &a, &err) == CCORD_OK);
  CT_CHECK(a.frame_count == 2 && a.w == 16 && a.h == 8);
  if (a.frame_count == 2) {
    CT_CHECK(near_px(a.frames[0], a.w, 3, 3, 255, 255, 0, 255));
    CT_CHECK(near_px(a.frames[1], a.w, 3, 3, 0, 0, 255, 255));
    CT_CHECK(near_px(a.frames[1], a.w, 10, 6, 255, 255, 0, 255));
    CT_CHECK(near_px(a.frames[1], a.w, 0, 0, 255, 255, 0, 255));
  }
  ccord_anim_free(&a);

  // Transparency is preserved as alpha 0 and disposal restores it between frames.
  CT_CHECK(ccord_anim_decode(alpha_gif, sizeof alpha_gif, 0, 0, &a, &err) == CCORD_OK);
  CT_CHECK(a.frame_count == 2);
  if (a.frame_count == 2) {
    CT_CHECK(a.frames[0][((size_t)4 * 16 + 2) * 4 + 3] == 0);
    CT_CHECK(near_px(a.frames[0], 16, 12, 4, 255, 0, 0, 255));
    CT_CHECK(a.frames[1][((size_t)4 * 16 + 2) * 4 + 3] == 0);
    CT_CHECK(near_px(a.frames[1], 16, 12, 4, 0, 255, 0, 255));
  }
  ccord_anim_free(&a);

  // Delays <= 10 ms become 100 ms; 11..19 ms become 20 ms. Patch each Graphic Control Extension (21 F9 04 ..).
  uint8_t *copy = ccord_malloc(sizeof rgb_gif);
  CT_CHECK(copy != nullptr);
  if (copy) {
    memcpy(copy, rgb_gif, sizeof rgb_gif);
    int k = 0;
    const uint16_t want_cs[3] = {0, 1, 2};   // centiseconds written into the file
    for (size_t i = 0; i + 8 < sizeof rgb_gif && k < 3; i++) {
      if (copy[i] == 0x21 && copy[i + 1] == 0xf9 && copy[i + 2] == 0x04 && copy[i + 7] == 0x00) {
        copy[i + 4] = (uint8_t)(want_cs[k] & 0xffu);
        copy[i + 5] = (uint8_t)(want_cs[k] >> 8);
        k++;
      }
    }
    CT_CHECK(k == 3);
    CT_CHECK(ccord_anim_decode(copy, sizeof rgb_gif, 0, 0, &a, &err) == CCORD_OK);
    if (a.frame_count == 3) {
      CT_CHECK(a.delay_ms[0] == 100 && a.delay_ms[1] == 100 && a.delay_ms[2] == 20);
    } else {
      ct_failures++;
    }
    ccord_anim_free(&a);
    ccord_free(copy);
  }
}

static void test_apng_webp(void)
{
  ccord_error err;
  ccord_anim a;
  CT_CHECK(ccord_anim_decode(rgb_apng, sizeof rgb_apng, 0, 0, &a, &err) == CCORD_OK);
  check_rgb(&a, "apng");
  ccord_anim_free(&a);
  if (ccord_anim_decode(rgb_webp, sizeof rgb_webp, 0, 0, &a, &err) == CCORD_OK) {
    check_rgb(&a, "webp");
    ccord_anim_free(&a);
  } else {
    fprintf(stderr, "note: this ffmpeg build cannot decode animated WebP (%s)\n", err.message);
    CT_CHECK(false);
  }
}

static void test_robustness(void)
{
  ccord_error err;
  ccord_anim a;
  CT_CHECK(ccord_anim_decode(nullptr, 0, 0, 0, &a, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_anim_decode(rgb_gif, 2, 0, 0, &a, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_anim_decode(rgb_gif, sizeof rgb_gif, 0, 0, nullptr, &err) == CCORD_ERR_INVALID);
  static const uint8_t junk[] = "this is definitely not an image, just some text padding it out to a decent length";
  CT_CHECK(ccord_anim_decode(junk, sizeof junk, 0, 0, &a, &err) != CCORD_OK);
  CT_CHECK(a.frames == nullptr && a.frame_count == 0);
  // Oversized input is rejected before it is touched.
  CT_CHECK(ccord_anim_decode(rgb_gif, (size_t)33u * 1024u * 1024u, 0, 0, &a, &err) == CCORD_ERR_RANGE);

  struct {
    const uint8_t *d;
    size_t n;
  } inputs[] = {{rgb_gif, sizeof rgb_gif}, {patch_gif, sizeof patch_gif}, {alpha_gif, sizeof alpha_gif},
                {rgb_apng, sizeof rgb_apng}, {rgb_webp, sizeof rgb_webp}};
  // Truncation at every offset: allocate an exact-size copy so ASan flags any read past the end.
  for (size_t k = 0; k < sizeof inputs / sizeof inputs[0]; k++) {
    for (size_t n = 0; n <= inputs[k].n; n++) {
      uint8_t *exact = ccord_malloc(n ? n : 1);
      if (!exact) continue;
      if (n) memcpy(exact, inputs[k].d, n);
      if (ccord_anim_decode(exact, n, 0, 0, &a, &err) == CCORD_OK) {
        CT_CHECK(a.frame_count >= 1 && a.w > 0 && a.h > 0);
        ccord_anim_free(&a);
      }
      ccord_free(exact);
    }
  }
  // Deterministic byte corruption.
  uint32_t rng = 0x12345678u;
  for (size_t k = 0; k < sizeof inputs / sizeof inputs[0]; k++) {
    for (int it = 0; it < 300; it++) {
      uint8_t *buf = ccord_malloc(inputs[k].n);
      if (!buf) continue;
      memcpy(buf, inputs[k].d, inputs[k].n);
      for (int f = 0; f < 3; f++) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        buf[rng % inputs[k].n] = (uint8_t)(rng >> 24);
      }
      if (ccord_anim_decode(buf, inputs[k].n, 0, 0, &a, &err) == CCORD_OK) {
        CT_CHECK(a.frame_count >= 1);
        ccord_anim_free(&a);
      }
      ccord_free(buf);
    }
  }
  // A GIF header claiming a huge canvas must be refused, not allocated (logical screen 0xFFFF x 0xFFFF).
  uint8_t *huge = ccord_malloc(sizeof rgb_gif);
  if (huge) {
    memcpy(huge, rgb_gif, sizeof rgb_gif);
    huge[6] = huge[7] = huge[8] = huge[9] = 0xff;
    CT_CHECK(ccord_anim_decode(huge, sizeof rgb_gif, 0, 0, &a, &err) != CCORD_OK);
    ccord_free(huge);
  }
}

static void test_decode_rgba(void)
{
  ccord_error err;
  uint8_t *rgba = nullptr;
  int w = 0, h = 0;
  CT_CHECK(ccord_image_decode_rgba(rgb_gif, sizeof rgb_gif, &rgba, &w, &h, &err) == CCORD_OK);
  CT_CHECK(w == 16 && h == 8 && rgba != nullptr);
  if (rgba) {
    CT_CHECK(near_px(rgba, w, 0, 0, 255, 0, 0, 255) && near_px(rgba, w, 15, 7, 255, 0, 0, 255));
    ccord_free(rgba);
  }
  rgba = nullptr;
  CT_CHECK(ccord_image_decode_rgba(rgb_apng, sizeof rgb_apng, &rgba, &w, &h, &err) == CCORD_OK);
  CT_CHECK(w == 16 && h == 8 && rgba && near_px(rgba, w, 4, 4, 255, 0, 0, 255));
  ccord_free(rgba);
  rgba = nullptr;
  // libwebp refuses animated files; the ffmpeg fallback returns the first frame.
  CT_CHECK(ccord_image_decode_rgba(rgb_webp, sizeof rgb_webp, &rgba, &w, &h, &err) == CCORD_OK);
  CT_CHECK(w == 16 && h == 8 && rgba && near_px(rgba, w, 4, 4, 255, 0, 0, 255));
  ccord_free(rgba);
  rgba = nullptr;
  static const uint8_t junk[] = "GIF89a but then nothing useful follows this header at all";
  CT_CHECK(ccord_image_decode_rgba(junk, sizeof junk, &rgba, &w, &h, &err) != CCORD_OK);
  CT_CHECK(rgba == nullptr);
}

static void test_av_stream(void)
{
  ccord_error err;
  CT_CHECK(ccord_av_stream_open(nullptr, 0, 0, CCORD_AV_IMAGES, &err) == nullptr && err.code == CCORD_ERR_INVALID);
  CT_CHECK(ccord_av_stream_open(rgb_gif, 4, 0, CCORD_AV_IMAGES, &err) == nullptr && err.code == CCORD_ERR_INVALID);
  CT_CHECK(ccord_av_stream_open(rgb_gif, sizeof rgb_gif, 0, 0, &err) == nullptr && err.code == CCORD_ERR_INVALID);
  CT_CHECK(ccord_av_stream_open(rgb_gif, CCORD_AV_MAX_INPUT + 1u, 0, CCORD_AV_IMAGES, &err) == nullptr &&
           err.code == CCORD_ERR_RANGE);
  static const uint8_t bad_media[] = "not a media stream at all, purely text padding bytes";
  CT_CHECK(ccord_av_stream_open(bad_media, sizeof bad_media, 0, CCORD_AV_IMAGES | CCORD_AV_MEDIA, &err) == nullptr);
  CT_CHECK(ccord_av_stream_info(nullptr) == nullptr);
  ccord_av_stream_close(nullptr);

  // Sequential mode: every frame in order, downscaled, then the end.
  ccord_av_stream *s = ccord_av_stream_open(rgb_gif, sizeof rgb_gif, 8, CCORD_AV_IMAGES, &err);
  CT_CHECK(s != nullptr);
  if (s) {
    const ccord_av_info *inf = ccord_av_stream_info(s);
    CT_CHECK(inf->has_video && !inf->has_audio && inf->w == 0);   // the size comes with the first frame
    static const int want[3][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    uint32_t last_pts = 0;
    for (int i = 0; i < 3; i++) {
      const uint8_t *px = nullptr;
      uint32_t pts = 0, dur = 0;
      CT_CHECK(ccord_av_stream_next_frame(s, &px, &pts, &dur));
      CT_CHECK(px && near_px(px, inf->w, 0, 0, want[i][0], want[i][1], want[i][2], 255));
      CT_CHECK(i == 0 ? pts == 0 : pts > last_pts);
      last_pts = pts;
    }
    CT_CHECK(last_pts >= 150u && inf->w == 8 && inf->h == 4);
    const uint8_t *px = nullptr;
    uint32_t pts = 0, dur = 0;
    CT_CHECK(!ccord_av_stream_next_frame(s, &px, &pts, &dur));
    // Rewinding plays it again from the top.
    ccord_av_stream_seek(s, 0);
    CT_CHECK(ccord_av_stream_next_frame(s, &px, &pts, &dur) && px && near_px(px, inf->w, 0, 0, 255, 0, 0, 255));
    ccord_av_stream_close(s);
  }

  // Player mode: the frame due at a position, a new serial whenever the pixels change, seeking, the end.
  s = ccord_av_stream_open(rgb_gif, sizeof rgb_gif, 0, CCORD_AV_MEDIA, &err);
  CT_CHECK(s != nullptr);
  if (s) {
    const ccord_av_info *inf = ccord_av_stream_info(s);
    uint32_t serial0 = 0, serial1 = 0, serial2 = 0;
    CT_CHECK(ccord_av_stream_frame(s, &serial0) == nullptr);
    CT_CHECK(ccord_av_stream_sync(s, 0, 0));
    CT_CHECK(inf->w == 16 && inf->h == 8);
    const uint8_t *px = ccord_av_stream_frame(s, &serial0);
    CT_CHECK(px && serial0 != 0 && near_px(px, inf->w, 0, 0, 255, 0, 0, 255));
    CT_CHECK(!ccord_av_stream_ended(s));
    CT_CHECK(ccord_av_stream_sync(s, 100, 0));
    px = ccord_av_stream_frame(s, &serial1);
    CT_CHECK(px && serial1 != serial0 && near_px(px, inf->w, 0, 0, 0, 255, 0, 255));
    CT_CHECK(ccord_av_stream_sync(s, 100, 0));
    (void)ccord_av_stream_frame(s, &serial2);
    CT_CHECK(serial2 == serial1);   // same position, same pixels
    CT_CHECK(ccord_av_stream_sync(s, 5000, 0));
    px = ccord_av_stream_frame(s, &serial2);
    CT_CHECK(px && near_px(px, inf->w, 0, 0, 0, 0, 255, 255) && ccord_av_stream_ended(s));
    ccord_av_stream_seek(s, 0);
    CT_CHECK(ccord_av_stream_sync(s, 0, 0) && !ccord_av_stream_ended(s));
    px = ccord_av_stream_frame(s, &serial2);
    CT_CHECK(px && near_px(px, inf->w, 0, 0, 255, 0, 0, 255));
    ccord_av_stream_close(s);
  }

  // Audio WAV decode (24 kHz mono -> 48 kHz stereo S16).
  uint8_t wav[44 + 2400 * 2];
  uint32_t rate = 24000u, samples = 2400u, dbytes = samples * 2u, rsz = 36u + dbytes, fsz = 16u, br = rate * 2u;
  uint16_t afmt = 1u, ch = 1u, ba = 2u, bits = 16u;
  memcpy(wav + 0, "RIFF", 4);
  memcpy(wav + 4, &rsz, 4);
  memcpy(wav + 8, "WAVEfmt ", 8);
  memcpy(wav + 16, &fsz, 4);
  memcpy(wav + 20, &afmt, 2);
  memcpy(wav + 22, &ch, 2);
  memcpy(wav + 24, &rate, 4);
  memcpy(wav + 28, &br, 4);
  memcpy(wav + 32, &ba, 2);
  memcpy(wav + 34, &bits, 2);
  memcpy(wav + 36, "data", 4);
  memcpy(wav + 40, &dbytes, 4);
  for (uint32_t i = 0; i < samples; i++) {
    int16_t v = (i < 1200u) ? (int16_t)12000 : (int16_t)-12000;
    memcpy(wav + 44u + i * 2u, &v, 2);
  }
  CT_CHECK(ccord_av_stream_open(wav, sizeof wav, 0, CCORD_AV_IMAGES, &err) == nullptr);   // not an image
  CT_CHECK(ccord_av_stream_open(wav, sizeof wav, 0, CCORD_AV_MEDIA | CCORD_AV_NO_AUDIO, &err) == nullptr);
  s = ccord_av_stream_open(wav, sizeof wav, 0, CCORD_AV_MEDIA, &err);
  CT_CHECK(s != nullptr);
  if (s) {
    const ccord_av_info *inf = ccord_av_stream_info(s);
    CT_CHECK(inf->has_audio && !inf->has_video && inf->duration_ms == 100u);
    static int16_t pcm[8192 * 2];
    size_t total = 0;
    for (uint32_t pos = 0; pos <= 400u && total < 8192u; pos += 20u) {
      (void)ccord_av_stream_sync(s, pos, 0);
      total += ccord_av_stream_read_pcm(s, pcm + total * 2u, 8192u - total);
    }
    CT_CHECK(total == 4800u && ccord_av_stream_ended(s));
    CT_CHECK(pcm[0] > 10000 && pcm[1] > 10000);
    CT_CHECK(pcm[(total - 1u) * 2u] < -10000 && pcm[(total - 1u) * 2u + 1u] < -10000);
    // Seeking to the middle resumes there: what is left is about half the clip, and it is the negative half.
    ccord_av_stream_seek(s, 50);
    size_t rest = 0;
    for (uint32_t pos = 50; pos <= 400u && rest < 8192u; pos += 20u) {
      (void)ccord_av_stream_sync(s, pos, 0);
      rest += ccord_av_stream_read_pcm(s, pcm + rest * 2u, 8192u - rest);
    }
    CT_CHECK(rest >= 2000u && rest <= 2800u && pcm[(rest - 1u) * 2u] < -10000);
    ccord_av_stream_close(s);
  }
}

int main(void)
{
  test_sniff();
  test_gif();
  test_apng_webp();
  test_robustness();
  test_decode_rgba();
  test_av_stream();
  return CT_RESULT("test_anim");
}
