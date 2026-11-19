// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/platform/app_icon.h>
#include <ccord/platform/tray_icon.h>

#include <math.h>
#include <string.h>

// The tray icon is the app icon plus an unread badge in the bottom-right corner:
//   count >= 1            red pill with the number (1..99), "99+" from 100 up
//   count == 0, unread    small red dot
//   count == 0, all read  no badge
// The badge is laid out on a 22 px design grid and scaled to the requested size with 4x4 supersampling, so the
// built-in 3x5 bitmap digits stay crisp at 22 px and smooth at 24/32/48 px.
#define GRID 22.0f
#define SUB 4

static const char *const k_glyph[11][5] = {
    {"###", "#.#", "#.#", "#.#", "###"},  // 0
    {".#.", "##.", ".#.", ".#.", "###"},  // 1
    {"###", "..#", "###", "#..", "###"},  // 2
    {"###", "..#", "###", "..#", "###"},  // 3
    {"#.#", "#.#", "###", "..#", "..#"},  // 4
    {"###", "#..", "###", "..#", "###"},  // 5
    {"###", "#..", "###", "#.#", "###"},  // 6
    {"###", "..#", ".#.", ".#.", ".#."},  // 7
    {"###", "#.#", "###", "#.#", "###"},  // 8
    {"###", "#.#", "###", "..#", "###"},  // 9
    {"...", ".#.", "###", ".#.", "..."},  // +
};

typedef struct badge_layout {
  int glyphs[3];
  int n;        // glyph count, 0 = dot
  float x0, y0, w, h;
  float tx, ty;  // text origin on the grid (integers)
} badge_layout;

static badge_layout layout_for(int count, bool has_unread)
{
  badge_layout b = {0};
  if (count <= 0) {
    b.w = b.h = 8.0f;
    b.x0 = GRID - b.w;
    b.y0 = GRID - b.h;
    (void)has_unread;
    return b;
  }
  if (count >= 100) {
    b.glyphs[0] = 9;
    b.glyphs[1] = 9;
    b.glyphs[2] = 10;
    b.n = 3;
  } else if (count >= 10) {
    b.glyphs[0] = count / 10;
    b.glyphs[1] = count % 10;
    b.n = 2;
  } else {
    b.glyphs[0] = count;
    b.n = 1;
  }
  float text_w = (float)(b.n * 4 - 1);
  float pad = b.n == 1 ? 3.0f : b.n == 2 ? 2.0f : 1.0f;
  b.h = 9.0f;
  b.w = text_w + 2.0f * pad;
  if (b.w < b.h) b.w = b.h;
  b.x0 = GRID - b.w;
  b.y0 = GRID - b.h;
  b.tx = b.x0 + pad;
  b.ty = b.y0 + 2.0f;
  return b;
}

static bool pill_hit(const badge_layout *b, float u, float v)
{
  float r = b->h * 0.5f;
  float cx = u < b->x0 + r ? b->x0 + r : (u > b->x0 + b->w - r ? b->x0 + b->w - r : u);
  float dx = u - cx, dy = v - (b->y0 + r);
  return u >= b->x0 && u <= b->x0 + b->w && dx * dx + dy * dy <= r * r;
}

static bool text_hit(const badge_layout *b, float u, float v)
{
  float lx = u - b->tx, ly = v - b->ty;
  if (b->n == 0 || lx < 0.0f || ly < 0.0f || ly >= 5.0f) return false;
  int cell = (int)(lx / 4.0f);
  if (cell >= b->n) return false;
  int col = (int)(lx - (float)(cell * 4));
  if (col >= 3) return false;  // inter-glyph gap
  return k_glyph[b->glyphs[cell]][(int)ly][col] == '#';
}

bool ccord_tray_icon_rgba(int size, int count, bool has_unread, uint8_t *out, size_t out_len)
{
  if (!ccord_tray_base_rgba(size, out, out_len)) return false;
  if (count <= 0 && !has_unread) return true;
  badge_layout b = layout_for(count, has_unread);
  const float scale = (float)size / GRID;
  // Restrict supersampling to the badge's bounding box (skipping ~65% of pixels guaranteed outside pill_hit).
  int y_start = (int)floorf(b.y0 * scale) - 1;
  int x_start = (int)floorf(b.x0 * scale) - 1;
  if (y_start < 0) y_start = 0;
  if (x_start < 0) x_start = 0;
  for (int y = y_start; y < size; y++) {
    for (int x = x_start; x < size; x++) {
      int pill = 0, text = 0;
      for (int sy = 0; sy < SUB; sy++) {
        for (int sx = 0; sx < SUB; sx++) {
          float u = ((float)x + ((float)sx + 0.5f) / SUB) / scale;
          float v = ((float)y + ((float)sy + 0.5f) / SUB) / scale;
          if (!pill_hit(&b, u, v)) continue;
          pill++;
          if (text_hit(&b, u, v)) text++;
        }
      }
      if (pill == 0) continue;
      uint8_t *p = out + ((size_t)y * (size_t)size + (size_t)x) * 4u;
      float pa = (float)pill / (SUB * SUB);
      float ta = (float)text / (float)pill;  // share of the covered area that is a digit
      // Red badge colour with white digits, as one colour for the covered part.
      float red[3] = {0xE5, 0x39, 0x35}, white[3] = {255, 255, 255};
      float col[3];
      for (int k = 0; k < 3; k++) col[k] = red[k] * (1.0f - ta) + white[k] * ta;
      float ba = (float)p[3] / 255.0f;
      float oa = pa + ba * (1.0f - pa);
      for (int k = 0; k < 3; k++) {
        float v = (col[k] * pa + (float)p[k] * ba * (1.0f - pa)) / oa;
        p[k] = (uint8_t)lroundf(v);
      }
      p[3] = (uint8_t)lroundf(oa * 255.0f);
    }
  }
  return true;
}
