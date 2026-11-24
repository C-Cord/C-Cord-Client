// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/render/render.h>
#include <ccord/render/anim.h>
#include <ccord/render/av_stream.h>
#include "lottie.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>

#include <ccord/core/alloc.h>

// ---- image decoding (libpng, libjpeg, libwebp) ----

#define MAX_IMAGE_DIM 4096   // the largest texture the cache holds, per side

// What a still picture may be before it is refused: a phone photo, a long screenshot or a wallpaper is far larger
// than MAX_IMAGE_DIM and is shrunk to it after decoding, so the expanded view of an embedded image still shows it.
// The pixel cap bounds the decoder's memory (a PNG or WebP is decoded whole; a JPEG is shrunk while decoding).
#define MAX_SOURCE_DIM 16384
#define MAX_SOURCE_PIXELS ((uint64_t)64 * 1000 * 1000)
#define MAX_JPEG_SOURCE_PIXELS ((uint64_t)400 * 1000 * 1000)

// Shrinks *rgba (w x h) with an area average so that neither side exceeds `max_dim`, keeping its shape. A picture
// that already fits is left as it is.
static ccord_status shrink_rgba_to_fit(uint8_t **rgba, int *w, int *h, int max_dim, ccord_error *err)
{
  if (max_dim <= 0 || max_dim > MAX_IMAGE_DIM) max_dim = MAX_IMAGE_DIM;
  int sw = *w, sh = *h;
  if (sw <= max_dim && sh <= max_dim) return CCORD_OK;
  int big = sw > sh ? sw : sh;
  int dw = (int)(((int64_t)sw * max_dim + big / 2) / big);
  int dh = (int)(((int64_t)sh * max_dim + big / 2) / big);
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;
  uint8_t *out = ccord_malloc((size_t)dw * (size_t)dh * 4u);
  if (!out) return ccord_error_set(err, CCORD_ERR_NOMEM, "image: out of memory shrinking");
  const uint8_t *src = *rgba;
  for (int y = 0; y < dh; y++) {
    int y0 = (int)((int64_t)y * sh / dh), y1 = (int)((int64_t)(y + 1) * sh / dh);
    if (y1 <= y0) y1 = y0 + 1;
    for (int x = 0; x < dw; x++) {
      int x0 = (int)((int64_t)x * sw / dw), x1 = (int)((int64_t)(x + 1) * sw / dw);
      if (x1 <= x0) x1 = x0 + 1;
      uint64_t acc[4] = {0, 0, 0, 0};
      for (int yy = y0; yy < y1; yy++) {
        const uint8_t *p = src + ((size_t)yy * (size_t)sw + (size_t)x0) * 4u;
        for (int xx = x0; xx < x1; xx++, p += 4) {
          acc[0] += p[0];
          acc[1] += p[1];
          acc[2] += p[2];
          acc[3] += p[3];
        }
      }
      uint64_t n = (uint64_t)(y1 - y0) * (uint64_t)(x1 - x0);
      uint8_t *d = out + ((size_t)y * (size_t)dw + (size_t)x) * 4u;
      for (int k = 0; k < 4; k++) d[k] = (uint8_t)((acc[k] + n / 2u) / n);
    }
  }
  ccord_free(*rgba);
  *rgba = out;
  *w = dw;
  *h = dh;
  return CCORD_OK;
}

static ccord_status decode_png_rgba(const uint8_t *data, size_t len, int max_dim, uint8_t **out_rgba, int *out_w,
                                    int *out_h, ccord_error *err)
{
  png_image img;
  memset(&img, 0, sizeof img);
  img.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&img, data, len) || PNG_IMAGE_FAILED(img)) {
    png_image_free(&img);
    return ccord_error_set(err, CCORD_ERR_PARSE, "png: invalid image header");
  }
  if (img.width == 0 || img.height == 0 || img.width > MAX_SOURCE_DIM || img.height > MAX_SOURCE_DIM ||
      (uint64_t)img.width * img.height > MAX_SOURCE_PIXELS) {
    png_image_free(&img);
    return ccord_error_set(err, CCORD_ERR_RANGE, "png: dimensions out of range");
  }
  img.format = PNG_FORMAT_RGBA;
  size_t sz = PNG_IMAGE_SIZE(img);
  uint8_t *rgba = ccord_malloc(sz);
  if (!rgba) {
    png_image_free(&img);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "png: out of memory");
  }
  if (!png_image_finish_read(&img, nullptr, rgba, 0, nullptr) || PNG_IMAGE_FAILED(img)) {
    ccord_free(rgba);
    png_image_free(&img);
    return ccord_error_set(err, CCORD_ERR_PARSE, "png: decode failed");
  }
  int pw = (int)img.width, ph = (int)img.height;
  png_image_free(&img);
  ccord_status sst = shrink_rgba_to_fit(&rgba, &pw, &ph, max_dim, err);
  if (sst != CCORD_OK) {
    ccord_free(rgba);
    return sst;
  }
  *out_w = pw;
  *out_h = ph;
  *out_rgba = rgba;
  return CCORD_OK;
}

struct jpeg_decode_ctx {
  struct jpeg_error_mgr pub;
  jmp_buf jb;
  uint8_t *rgba;
  uint8_t *row;
};

static void jpeg_error_exit_fn(j_common_ptr cinfo)
{
  struct jpeg_decode_ctx *ctx = (struct jpeg_decode_ctx *)(void *)cinfo->err;
  longjmp(ctx->jb, 1);
}

static void jpeg_output_message_fn(j_common_ptr cinfo)
{
  (void)cinfo;
}

static ccord_status decode_jpeg_rgba(const uint8_t *data, size_t len, int max_dim, uint8_t **out_rgba, int *out_w,
                                     int *out_h, ccord_error *err)
{
  if (max_dim <= 0 || max_dim > MAX_IMAGE_DIM) max_dim = MAX_IMAGE_DIM;
  struct jpeg_decompress_struct cinfo;
  struct jpeg_decode_ctx jctx;
  memset(&jctx, 0, sizeof jctx);
  cinfo.err = jpeg_std_error(&jctx.pub);
  jctx.pub.error_exit = jpeg_error_exit_fn;
  jctx.pub.output_message = jpeg_output_message_fn;

  if (setjmp(jctx.jb) != 0) {
    jpeg_destroy_decompress(&cinfo);
    ccord_free(jctx.row);
    ccord_free(jctx.rgba);
    return ccord_error_set(err, CCORD_ERR_PARSE, "jpeg: decode failed");
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, data, (unsigned long)len);
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return ccord_error_set(err, CCORD_ERR_PARSE, "jpeg: invalid header");
  }
  cinfo.out_color_space = JCS_RGB;
  if (cinfo.image_width == 0 || cinfo.image_height == 0 || cinfo.image_width > MAX_SOURCE_DIM ||
      cinfo.image_height > MAX_SOURCE_DIM ||
      (uint64_t)cinfo.image_width * cinfo.image_height > MAX_JPEG_SOURCE_PIXELS) {
    jpeg_destroy_decompress(&cinfo);
    return ccord_error_set(err, CCORD_ERR_RANGE, "jpeg: dimensions out of range");
  }
  // libjpeg shrinks by 1/2, 1/4 or 1/8 while decoding: a large photo never needs its full-size buffer.
  unsigned int target_dim = (unsigned int)max_dim;
  cinfo.scale_num = 1;
  cinfo.scale_denom = 1;
  while (cinfo.scale_denom < 8u &&
         (cinfo.image_width / (cinfo.scale_denom * 2u) >= target_dim ||
          cinfo.image_height / (cinfo.scale_denom * 2u) >= target_dim ||
          cinfo.image_width / cinfo.scale_denom > MAX_IMAGE_DIM ||
          cinfo.image_height / cinfo.scale_denom > MAX_IMAGE_DIM)) {
    cinfo.scale_denom *= 2u;
  }
  (void)jpeg_start_decompress(&cinfo);
  if (cinfo.output_width == 0 || cinfo.output_height == 0 || cinfo.output_width > MAX_SOURCE_DIM ||
      cinfo.output_height > MAX_SOURCE_DIM || cinfo.output_components != 3) {
    jpeg_destroy_decompress(&cinfo);
    return ccord_error_set(err, CCORD_ERR_RANGE, "jpeg: dimensions or components out of range");
  }
  int w = (int)cinfo.output_width;
  int h = (int)cinfo.output_height;
  jctx.rgba = ccord_malloc((size_t)w * (size_t)h * 4u);
  jctx.row = ccord_malloc((size_t)w * 3u);
  if (!jctx.rgba || !jctx.row) {
    jpeg_destroy_decompress(&cinfo);
    ccord_free(jctx.row);
    ccord_free(jctx.rgba);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "jpeg: out of memory");
  }
  while (cinfo.output_scanline < cinfo.output_height) {
    uint32_t y = cinfo.output_scanline;
    JSAMPROW row_ptr[1] = {jctx.row};
    if (jpeg_read_scanlines(&cinfo, row_ptr, 1) != 1) {
      jpeg_destroy_decompress(&cinfo);
      ccord_free(jctx.row);
      ccord_free(jctx.rgba);
      return ccord_error_set(err, CCORD_ERR_PARSE, "jpeg: truncated scanline");
    }
    uint8_t *dst = &jctx.rgba[(size_t)y * (size_t)w * 4u];
    for (int x = 0; x < w; x++) {
      dst[x * 4 + 0] = jctx.row[x * 3 + 0];
      dst[x * 4 + 1] = jctx.row[x * 3 + 1];
      dst[x * 4 + 2] = jctx.row[x * 3 + 2];
      dst[x * 4 + 3] = 255u;
    }
  }
  (void)jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  ccord_free(jctx.row);
  ccord_status sst = shrink_rgba_to_fit(&jctx.rgba, &w, &h, max_dim, err);
  if (sst != CCORD_OK) {
    ccord_free(jctx.rgba);
    return sst;
  }
  *out_w = w;
  *out_h = h;
  *out_rgba = jctx.rgba;
  return CCORD_OK;
}

static ccord_status decode_webp_rgba(const uint8_t *data, size_t len, int max_dim, uint8_t **out_rgba, int *out_w,
                                     int *out_h, ccord_error *err)
{
  int w = 0, h = 0;
  if (!WebPGetInfo(data, len, &w, &h) || w <= 0 || h <= 0 || w > MAX_SOURCE_DIM || h > MAX_SOURCE_DIM ||
      (uint64_t)w * (uint64_t)h > MAX_SOURCE_PIXELS) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "webp: invalid header or dimensions");
  }
  if (max_dim <= 0 || max_dim > MAX_IMAGE_DIM) max_dim = MAX_IMAGE_DIM;
  if (w > max_dim || h > max_dim) {
    int big = w > h ? w : h;
    int dw = (int)(((int64_t)w * max_dim + big / 2) / big);
    int dh = (int)(((int64_t)h * max_dim + big / 2) / big);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    WebPDecoderConfig cfg;
    if (!WebPInitDecoderConfig(&cfg)) return ccord_error_set(err, CCORD_ERR_INTERNAL, "webp: config init failed");
    size_t sz = (size_t)dw * (size_t)dh * 4u;
    uint8_t *rgba = ccord_malloc(sz);
    if (!rgba) return ccord_error_set(err, CCORD_ERR_NOMEM, "webp: out of memory");
    cfg.options.use_scaling = 1;
    cfg.options.scaled_width = dw;
    cfg.options.scaled_height = dh;
    cfg.output.colorspace = MODE_RGBA;
    cfg.output.is_external_memory = 1;
    cfg.output.u.RGBA.rgba = rgba;
    cfg.output.u.RGBA.stride = dw * 4;
    cfg.output.u.RGBA.size = sz;
    VP8StatusCode st = WebPDecode(data, len, &cfg);
    WebPFreeDecBuffer(&cfg.output);
    if (st != VP8_STATUS_OK) {
      ccord_free(rgba);
      return ccord_error_set(err, CCORD_ERR_PARSE, "webp: decode failed");
    }
    *out_w = dw;
    *out_h = dh;
    *out_rgba = rgba;
    return CCORD_OK;
  }
  size_t sz = (size_t)w * (size_t)h * 4u;
  uint8_t *rgba = ccord_malloc(sz);
  if (!rgba) return ccord_error_set(err, CCORD_ERR_NOMEM, "webp: out of memory");
  if (!WebPDecodeRGBAInto(data, len, rgba, sz, w * 4)) {
    ccord_free(rgba);
    return ccord_error_set(err, CCORD_ERR_PARSE, "webp: decode failed");
  }
  *out_w = w;
  *out_h = h;
  *out_rgba = rgba;
  return CCORD_OK;
}

// First frame of anything the ffmpeg path understands (GIF, APNG, animated WebP, BMP, TIFF, ...).
static ccord_status decode_ffmpeg_first_frame(const uint8_t *data, size_t len, int max_dim, uint8_t **out_rgba,
                                              int *out_w, int *out_h, ccord_error *err)
{
  if (max_dim <= 0 || max_dim > MAX_IMAGE_DIM) max_dim = MAX_IMAGE_DIM;
  ccord_anim a;
  ccord_status st = ccord_anim_decode(data, len, max_dim, 1, &a, err);
  if (st != CCORD_OK) return st;
  *out_rgba = a.frames[0];
  *out_w = a.w;
  *out_h = a.h;
  a.frames[0] = nullptr;   // ownership moved to the caller
  ccord_anim_free(&a);
  return CCORD_OK;
}

ccord_status ccord_image_decode_rgba_max(const uint8_t *data, size_t len, int max_dim, uint8_t **out_rgba, int *out_w,
                                         int *out_h, ccord_error *err)
{
  ccord_error_clear(err);
  if (out_rgba) *out_rgba = nullptr;
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (!data || len < 4 || !out_rgba || !out_w || !out_h) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "image decode: invalid argument");
  }
  if (max_dim <= 0 || max_dim > MAX_IMAGE_DIM) max_dim = MAX_IMAGE_DIM;
  if (len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
    return decode_png_rgba(data, len, max_dim, out_rgba, out_w, out_h, err);
  }
  if (len >= 3 && data[0] == 0xffu && data[1] == 0xd8u && data[2] == 0xffu) {
    return decode_jpeg_rgba(data, len, max_dim, out_rgba, out_w, out_h, err);
  }
  if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
    // libwebp's simple decoder rejects animated WebP; the ffmpeg path yields its first frame.
    if (decode_webp_rgba(data, len, max_dim, out_rgba, out_w, out_h, err) == CCORD_OK) return CCORD_OK;
    return decode_ffmpeg_first_frame(data, len, max_dim, out_rgba, out_w, out_h, err);
  }
  if (ccord_lottie_sniff(data, len)) {
    return ccord_lottie_render_first_frame(data, len, max_dim, out_rgba, out_w, out_h, err);
  }
  return decode_ffmpeg_first_frame(data, len, max_dim, out_rgba, out_w, out_h, err);
}

void ccord_rgba_to_grayscale_inplace(uint8_t *rgba, size_t npixels)
{
  if (!rgba) return;
  for (size_t i = 0; i < npixels; i++) {
    uint8_t *p = rgba + i * 4u;
    uint8_t y = (uint8_t)((54u * (uint32_t)p[0] + 183u * (uint32_t)p[1] + 19u * (uint32_t)p[2] + 128u) >> 8);
    p[0] = y;
    p[1] = y;
    p[2] = y;
  }
}

static ccord_status shrink_rgba_to_box(uint8_t **rgba, int *w, int *h, int max_w, int max_h, ccord_error *err)
{
  if (max_w <= 0 || max_w > MAX_IMAGE_DIM) max_w = MAX_IMAGE_DIM;
  if (max_h <= 0 || max_h > MAX_IMAGE_DIM) max_h = MAX_IMAGE_DIM;
  int sw = *w, sh = *h;
  if (sw <= max_w && sh <= max_h) return CCORD_OK;
  double sx = (double)max_w / (double)sw;
  double sy = (double)max_h / (double)sh;
  double sc = sx < sy ? sx : sy;
  int dw = (int)((double)sw * sc + 0.5);
  int dh = (int)((double)sh * sc + 0.5);
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;
  if (dw > max_w) dw = max_w;
  if (dh > max_h) dh = max_h;
  uint8_t *out = ccord_malloc((size_t)dw * (size_t)dh * 4u);
  if (!out) return ccord_error_set(err, CCORD_ERR_NOMEM, "image: out of memory shrinking");
  const uint8_t *src = *rgba;
  for (int y = 0; y < dh; y++) {
    int y0 = (int)((int64_t)y * sh / dh), y1 = (int)((int64_t)(y + 1) * sh / dh);
    if (y1 <= y0) y1 = y0 + 1;
    for (int x = 0; x < dw; x++) {
      int x0 = (int)((int64_t)x * sw / dw), x1 = (int)((int64_t)(x + 1) * sw / dw);
      if (x1 <= x0) x1 = x0 + 1;
      uint64_t acc[4] = {0, 0, 0, 0};
      for (int yy = y0; yy < y1; yy++) {
        const uint8_t *p = src + ((size_t)yy * (size_t)sw + (size_t)x0) * 4u;
        for (int xx = x0; xx < x1; xx++, p += 4) {
          acc[0] += p[0];
          acc[1] += p[1];
          acc[2] += p[2];
          acc[3] += p[3];
        }
      }
      uint64_t n = (uint64_t)(y1 - y0) * (uint64_t)(x1 - x0);
      uint8_t *d = out + ((size_t)y * (size_t)dw + (size_t)x) * 4u;
      for (int k = 0; k < 4; k++) d[k] = (uint8_t)((acc[k] + n / 2u) / n);
    }
  }
  ccord_free(*rgba);
  *rgba = out;
  *w = dw;
  *h = dh;
  return CCORD_OK;
}

ccord_status ccord_image_decode_rgba_box(const uint8_t *data, size_t len, int max_w, int max_h, bool grayscale,
                                         uint8_t **out_rgba, int *out_w, int *out_h, ccord_error *err)
{
  if (max_w <= 0 || max_w > MAX_IMAGE_DIM) max_w = MAX_IMAGE_DIM;
  if (max_h <= 0 || max_h > MAX_IMAGE_DIM) max_h = MAX_IMAGE_DIM;
  int max_dim = max_w > max_h ? max_w : max_h;
  uint8_t *rgba = nullptr;
  int w = 0, h = 0;
  ccord_status st = ccord_image_decode_rgba_max(data, len, max_dim, &rgba, &w, &h, err);
  if (st != CCORD_OK) return st;
  if (w > max_w || h > max_h) {
    st = shrink_rgba_to_box(&rgba, &w, &h, max_w, max_h, err);
    if (st != CCORD_OK) {
      ccord_free(rgba);
      return st;
    }
  }
  if (grayscale) ccord_rgba_to_grayscale_inplace(rgba, (size_t)w * (size_t)h);
  *out_rgba = rgba;
  *out_w = w;
  *out_h = h;
  return CCORD_OK;
}

ccord_status ccord_image_decode_rgba(const uint8_t *data, size_t len, uint8_t **out_rgba, int *out_w, int *out_h,
                                     ccord_error *err)
{
  return ccord_image_decode_rgba_max(data, len, MAX_IMAGE_DIM, out_rgba, out_w, out_h, err);
}

ccord_status ccord_image_crop_png(const uint8_t *src, size_t src_len, float zoom, float pan_x, float pan_y,
                                  int out_w, int out_h, uint8_t **out_png, size_t *out_len, ccord_error *err)
{
  ccord_error_clear(err);
  if (out_png) *out_png = nullptr;
  if (out_len) *out_len = 0;
  if (!src || src_len < 4 || !out_png || !out_len || out_w <= 0 || out_h <= 0 || out_w > 1024 || out_h > 1024) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "image crop: invalid argument");
  }
  if (zoom < 1.0f) zoom = 1.0f;
  if (zoom > 4.0f) zoom = 4.0f;
  if (pan_x < -1.0f) pan_x = -1.0f;
  if (pan_x > 1.0f) pan_x = 1.0f;
  if (pan_y < -1.0f) pan_y = -1.0f;
  if (pan_y > 1.0f) pan_y = 1.0f;

  uint8_t *rgba = nullptr;
  int sw = 0, sh = 0;
  ccord_status st = ccord_image_decode_rgba(src, src_len, &rgba, &sw, &sh, err);
  if (st != CCORD_OK) return st;

  // Compute aspect-fit cover crop window inside (sw x sh), zoomed by `zoom` and panned by (pan_x, pan_y).
  double target_ar = (double)out_w / (double)out_h;
  double src_ar = (double)sw / (double)sh;
  double base_w = (double)sw, base_h = (double)sh;
  if (src_ar > target_ar) base_w = (double)sh * target_ar;
  else base_h = (double)sw / target_ar;
  double crop_w = base_w / (double)zoom;
  double crop_h = base_h / (double)zoom;
  if (crop_w < 1.0) crop_w = 1.0;
  if (crop_h < 1.0) crop_h = 1.0;
  double max_ox = ((double)sw - crop_w) * 0.5;
  double max_oy = ((double)sh - crop_h) * 0.5;
  double x0 = max_ox + (double)pan_x * max_ox;
  double y0 = max_oy + (double)pan_y * max_oy;

  uint8_t *dst = ccord_malloc((size_t)out_w * (size_t)out_h * 4u);
  if (!dst) {
    ccord_free(rgba);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "image crop: out of memory");
  }
  for (int y = 0; y < out_h; y++) {
    double sy = y0 + (((double)y + 0.5) / (double)out_h) * crop_h - 0.5;
    if (sy < 0.0) sy = 0.0;
    if (sy > (double)(sh - 1)) sy = (double)(sh - 1);
    int iy0 = (int)sy;
    int iy1 = iy0 + 1 < sh ? iy0 + 1 : iy0;
    double fy = sy - (double)iy0;
    for (int x = 0; x < out_w; x++) {
      double sx = x0 + (((double)x + 0.5) / (double)out_w) * crop_w - 0.5;
      if (sx < 0.0) sx = 0.0;
      if (sx > (double)(sw - 1)) sx = (double)(sw - 1);
      int ix0 = (int)sx;
      int ix1 = ix0 + 1 < sw ? ix0 + 1 : ix0;
      double fx = sx - (double)ix0;

      const uint8_t *p00 = rgba + ((size_t)iy0 * (size_t)sw + (size_t)ix0) * 4u;
      const uint8_t *p10 = rgba + ((size_t)iy0 * (size_t)sw + (size_t)ix1) * 4u;
      const uint8_t *p01 = rgba + ((size_t)iy1 * (size_t)sw + (size_t)ix0) * 4u;
      const uint8_t *p11 = rgba + ((size_t)iy1 * (size_t)sw + (size_t)ix1) * 4u;
      uint8_t *op = dst + ((size_t)y * (size_t)out_w + (size_t)x) * 4u;
      for (int c = 0; c < 4; c++) {
        double top = (double)p00[c] * (1.0 - fx) + (double)p10[c] * fx;
        double bot = (double)p01[c] * (1.0 - fx) + (double)p11[c] * fx;
        int val = (int)(top * (1.0 - fy) + bot * fy + 0.5);
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        op[c] = (uint8_t)val;
      }
    }
  }
  ccord_free(rgba);

  png_image pimg;
  memset(&pimg, 0, sizeof pimg);
  pimg.version = PNG_IMAGE_VERSION;
  pimg.width = (png_uint_32)out_w;
  pimg.height = (png_uint_32)out_h;
  pimg.format = PNG_FORMAT_RGBA;

  png_alloc_size_t png_sz = 0;
  if (!png_image_write_to_memory(&pimg, nullptr, &png_sz, 0, dst, 0, nullptr) || png_sz == 0) {
    ccord_free(dst);
    png_image_free(&pimg);
    return ccord_error_set(err, CCORD_ERR_IO, "image crop: png size query failed");
  }
  uint8_t *buf = ccord_malloc((size_t)png_sz);
  if (!buf) {
    ccord_free(dst);
    png_image_free(&pimg);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "image crop: out of memory");
  }
  if (!png_image_write_to_memory(&pimg, buf, &png_sz, 0, dst, 0, nullptr) || PNG_IMAGE_FAILED(pimg)) {
    ccord_free(buf);
    ccord_free(dst);
    png_image_free(&pimg);
    return ccord_error_set(err, CCORD_ERR_IO, "image crop: png encode failed");
  }
  ccord_free(dst);
  png_image_free(&pimg);
  *out_png = buf;
  *out_len = (size_t)png_sz;
  return CCORD_OK;
}

