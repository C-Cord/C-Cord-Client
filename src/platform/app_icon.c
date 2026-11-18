// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/platform/app_icon.h>

#include <math.h>
#include <string.h>

#include "app_icon_data.h"

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
  if (!out || size < 8 || size > 1024 || out_len < (size_t)size * (size_t)size * 4u) return false;
  const ccord_icon_image *best = &set[n - 1];
  for (size_t i = 0; i < n; i++) {
    if (set[i].size == size) {
      memcpy(out, set[i].rgba, (size_t)size * (size_t)size * 4u);
      return true;
    }
    if (set[i].size > size) {  // smallest embedded image that is larger: downscale from it
      best = &set[i];
      break;
    }
  }
  resample(best->rgba, best->size, out, size);
  return true;
}

bool ccord_app_icon_rgba(int size, uint8_t *out, size_t out_len)
{
  return fill(ccord_app_icon_images, ccord_app_icon_image_count, size, out, out_len);
}

bool ccord_tray_base_rgba(int size, uint8_t *out, size_t out_len)
{
  return fill(ccord_tray_icon_images, ccord_tray_icon_image_count, size, out, out_len);
}
