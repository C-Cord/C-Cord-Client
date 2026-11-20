// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/platform/app_icon.h>

#include <math.h>
#include <string.h>

#include <ccord/core/alloc.h>

#include "app_icon_data.h"

// Minimal lossless QOI (Quite OK Image, v1.0) decoder for embedded square RGBA icons.
static bool qoi_decode_rgba(const uint8_t *src, size_t src_len, int expect_size, uint8_t *out, size_t out_len)
{
  if (!src || src_len < 22u || !out || expect_size <= 0) return false;
  if (src[0] != 'q' || src[1] != 'o' || src[2] != 'i' || src[3] != 'f') return false;
  uint32_t w = ((uint32_t)src[4] << 24) | ((uint32_t)src[5] << 16) | ((uint32_t)src[6] << 8) | (uint32_t)src[7];
  uint32_t h = ((uint32_t)src[8] << 24) | ((uint32_t)src[9] << 16) | ((uint32_t)src[10] << 8) | (uint32_t)src[11];
  if (w != (uint32_t)expect_size || h != (uint32_t)expect_size || src[12] != 4u) return false;
  size_t px_bytes = (size_t)w * (size_t)h * 4u;
  if (out_len < px_bytes) return false;

  uint8_t index[64][4];
  memset(index, 0, sizeof index);
  uint8_t r = 0, g = 0, b = 0, a = 255u;
  size_t p = 14u;
  size_t end = src_len - 8u;
  unsigned run = 0;

  for (size_t dst = 0; dst < px_bytes; dst += 4u) {
    if (run > 0) {
      run--;
    } else if (p < end) {
      uint8_t b1 = src[p++];
      bool update_index = true;
      if (b1 == 0xFEu) {
        if (end - p < 3u) return false;
        r = src[p++];
        g = src[p++];
        b = src[p++];
      } else if (b1 == 0xFFu) {
        if (end - p < 4u) return false;
        r = src[p++];
        g = src[p++];
        b = src[p++];
        a = src[p++];
      } else {
        uint8_t tag = b1 & 0xC0u;
        if (tag == 0x00u) {
          r = index[b1][0];
          g = index[b1][1];
          b = index[b1][2];
          a = index[b1][3];
          update_index = false;
        } else if (tag == 0x40u) {
          r = (uint8_t)((unsigned)r + (unsigned)((b1 >> 4) & 3u) - 2u);
          g = (uint8_t)((unsigned)g + (unsigned)((b1 >> 2) & 3u) - 2u);
          b = (uint8_t)((unsigned)b + (unsigned)(b1 & 3u) - 2u);
        } else if (tag == 0x80u) {
          if (p >= end) return false;
          uint8_t b2 = src[p++];
          unsigned vg = (unsigned)(b1 & 0x3Fu) - 32u;
          r = (uint8_t)((unsigned)r + vg - 8u + (unsigned)((b2 >> 4) & 0x0Fu));
          g = (uint8_t)((unsigned)g + vg);
          b = (uint8_t)((unsigned)b + vg - 8u + (unsigned)(b2 & 0x0Fu));
        } else {
          run = (unsigned)(b1 & 0x3Fu);
          update_index = false;
        }
      }
      if (update_index) {
        unsigned idx = ((unsigned)r * 3u + (unsigned)g * 5u + (unsigned)b * 7u + (unsigned)a * 11u) & 63u;
        index[idx][0] = r;
        index[idx][1] = g;
        index[idx][2] = b;
        index[idx][3] = a;
      }
    } else {
      return false;
    }
    out[dst + 0u] = r;
    out[dst + 1u] = g;
    out[dst + 2u] = b;
    out[dst + 3u] = a;
  }
  return run == 0 && p == end;
}

// Area-weighted (premultiplied) resample of a straight-alpha RGBA square; used for sizes that are not embedded.
static void resample(const uint8_t *src, int ssz, uint8_t *out, int size)
{
  const float r = (float)ssz / (float)size;
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      float x0 = (float)x * r, x1 = x0 + r, y0 = (float)y * r, y1 = y0 + r;
      if (r < 1.0f) {  // upscale: sample the source pixel containing the centre
        x0 = fminf(x0 + r * 0.5f, (float)ssz - 0.001f);
        y0 = fminf(y0 + r * 0.5f, (float)ssz - 0.001f);
        x1 = x0 + 0.001f;
        y1 = y0 + 0.001f;
      }
      float acc[4] = {0, 0, 0, 0}, wsum = 0;
      for (int sy = (int)y0; (float)sy < y1 && sy < ssz; sy++) {
        float wy = fminf(y1, (float)sy + 1) - fmaxf(y0, (float)sy);
        for (int sx = (int)x0; (float)sx < x1 && sx < ssz; sx++) {
          float w = wy * (fminf(x1, (float)sx + 1) - fmaxf(x0, (float)sx));
          const uint8_t *p = src + ((size_t)sy * (size_t)ssz + (size_t)sx) * 4u;
          float a = (float)p[3] / 255.0f;
          acc[0] += w * a * p[0];
          acc[1] += w * a * p[1];
          acc[2] += w * a * p[2];
          acc[3] += w * a;
          wsum += w;
        }
      }
      uint8_t *d = out + ((size_t)y * (size_t)size + (size_t)x) * 4u;
      if (wsum <= 0 || acc[3] <= 0) {
        d[0] = d[1] = d[2] = d[3] = 0;
        continue;
      }
      for (int k = 0; k < 3; k++) d[k] = (uint8_t)lroundf(acc[k] / acc[3]);
      d[3] = (uint8_t)lroundf(acc[3] / wsum * 255.0f);
    }
  }
}

static bool fill(const ccord_icon_image *set, size_t n, int size, uint8_t *out, size_t out_len)
{
  if (!out || n == 0 || size < 8 || size > 1024 || out_len < (size_t)size * (size_t)size * 4u) return false;
  const ccord_icon_image *best = &set[n - 1];
  for (size_t i = 0; i < n; i++) {
    if (set[i].size == size) {
      return qoi_decode_rgba(set[i].qoi, set[i].qoi_len, size, out, out_len);
    }
    if (set[i].size > size) {  // smallest embedded image that is larger: downscale from it
      best = &set[i];
      break;
    }
  }
  size_t src_bytes = (size_t)best->size * (size_t)best->size * 4u;
  uint8_t *src = ccord_malloc(src_bytes);
  if (!src) return false;
  bool ok = qoi_decode_rgba(best->qoi, best->qoi_len, best->size, src, src_bytes);
  if (ok) resample(src, best->size, out, size);
  ccord_free(src);
  return ok;
}

bool ccord_app_icon_rgba(int size, uint8_t *out, size_t out_len)
{
  return fill(ccord_app_icon_images, ccord_app_icon_image_count, size, out, out_len);
}

bool ccord_tray_base_rgba(int size, uint8_t *out, size_t out_len)
{
  return fill(ccord_tray_icon_images, ccord_tray_icon_image_count, size, out, out_len);
}
