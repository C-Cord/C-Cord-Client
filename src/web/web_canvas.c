// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "web_internal.h"

#include <ccord/core/base64.h>
#include <zlib.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CANVAS_MAX_DIM   2048
#define CANVAS_STACK_MAX 16u
#define CANVAS_PATH_MAX  4096u

typedef struct canvas_state {
  float m[6]; // [a, b, c, d, e, f]: x' = a*x + c*y + e, y' = b*x + d*y + f
  ccord_color fill;
  ccord_color stroke;
  float line_width;
  float global_alpha;
  float font_px;
  uint8_t text_align;    // 0=left, 1=center, 2=right
  uint8_t text_baseline; // 0=alphabetic, 1=top, 2=middle, 3=bottom
  char fill_str[64];
  char stroke_str[64];
  char font_str[64];
} canvas_state;

typedef struct canvas_pt {
  float x, y;
  bool move;
} canvas_pt;

struct web_canvas {
  int w, h;
  uint8_t *rgba; // w * h * 4
  bool dirty;
  char tex_key[64];
  canvas_state st;
  canvas_state stack[CANVAS_STACK_MAX];
  size_t stack_len;
  canvas_pt *path;
  size_t path_len, path_cap;
  float sub_x, sub_y;
  float cur_x, cur_y;
  bool has_cur;
};

static uint32_t s_canvas_seq = 1u;

static void state_init(canvas_state *s)
{
  memset(s, 0, sizeof *s);
  s->m[0] = 1.0f; s->m[3] = 1.0f;
  s->fill = (ccord_color){0, 0, 0, 255};
  s->stroke = (ccord_color){0, 0, 0, 255};
  s->line_width = 1.0f;
  s->global_alpha = 1.0f;
  s->font_px = 12.0f;
  snprintf(s->fill_str, sizeof s->fill_str, "#000000");
  snprintf(s->stroke_str, sizeof s->stroke_str, "#000000");
  snprintf(s->font_str, sizeof s->font_str, "12px sans-serif");
}

web_canvas *web_canvas_create(int w, int h)
{
  if (w < 1) w = 300;
  if (h < 1) h = 150;
  if (w > CANVAS_MAX_DIM) w = CANVAS_MAX_DIM;
  if (h > CANVAS_MAX_DIM) h = CANVAS_MAX_DIM;
  web_canvas *c = ccord_calloc(1, sizeof *c);
  if (!c) return nullptr;
  c->w = w;
  c->h = h;
  c->rgba = ccord_calloc((size_t)w * (size_t)h, 4u);
  if (!c->rgba) { ccord_free(c); return nullptr; }
  c->dirty = true;
  snprintf(c->tex_key, sizeof c->tex_key, "web_canvas_%u", s_canvas_seq++);
  state_init(&c->st);
  return c;
}

void web_canvas_destroy(web_canvas *c)
{
  if (!c) return;
  ccord_free(c->rgba);
  ccord_free(c->path);
  ccord_free(c);
}

int web_canvas_width(const web_canvas *c) { return c ? c->w : 0; }
int web_canvas_height(const web_canvas *c) { return c ? c->h : 0; }

void web_canvas_resize(web_canvas *c, int w, int h)
{
  if (!c) return;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (w > CANVAS_MAX_DIM) w = CANVAS_MAX_DIM;
  if (h > CANVAS_MAX_DIM) h = CANVAS_MAX_DIM;
  uint8_t *nb = ccord_calloc((size_t)w * (size_t)h, 4u);
  if (!nb) return;
  ccord_free(c->rgba);
  c->rgba = nb;
  c->w = w;
  c->h = h;
  c->dirty = true;
  c->stack_len = 0;
  c->path_len = 0;
  state_init(&c->st);
}

// ---- Low-level pixel blending ----

static inline void blend_px(web_canvas *c, int x, int y, ccord_color col, float alpha_mul)
{
  if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
  float sa = ((float)col.a / 255.0f) * alpha_mul;
  if (sa <= 0.0f) return;
  if (sa > 1.0f) sa = 1.0f;
  uint8_t *dst = c->rgba + ((size_t)y * (size_t)c->w + (size_t)x) * 4u;
  if (sa >= 0.999f) {
    dst[0] = col.r; dst[1] = col.g; dst[2] = col.b; dst[3] = 255;
    return;
  }
  float da = (float)dst[3] / 255.0f;
  float oa = sa + da * (1.0f - sa);
  if (oa <= 0.0001f) {
    dst[0] = dst[1] = dst[2] = dst[3] = 0;
    return;
  }
  float inv = 1.0f / oa;
  dst[0] = (uint8_t)((col.r * sa + dst[0] * da * (1.0f - sa)) * inv + 0.5f);
  dst[1] = (uint8_t)((col.g * sa + dst[1] * da * (1.0f - sa)) * inv + 0.5f);
  dst[2] = (uint8_t)((col.b * sa + dst[2] * da * (1.0f - sa)) * inv + 0.5f);
  dst[3] = (uint8_t)(oa * 255.0f + 0.5f);
}

static inline void xform_pt(const float m[6], float x, float y, float *ox, float *oy)
{
  *ox = m[0] * x + m[2] * y + m[4];
  *oy = m[1] * x + m[3] * y + m[5];
}

static void path_push(web_canvas *c, float x, float y, bool move)
{
  if (c->path_len >= CANVAS_PATH_MAX) return;
  if (c->path_len == c->path_cap) {
    size_t nc = c->path_cap ? c->path_cap * 2u : 64u;
    if (nc > CANVAS_PATH_MAX) nc = CANVAS_PATH_MAX;
    canvas_pt *np = ccord_realloc(c->path, nc * sizeof *np);
    if (!np) return;
    c->path = np;
    c->path_cap = nc;
  }
  float tx, ty;
  xform_pt(c->st.m, x, y, &tx, &ty);
  c->path[c->path_len++] = (canvas_pt){tx, ty, move};
}

static void draw_thick_line(web_canvas *c, float x0, float y0, float x1, float y1, float lw, ccord_color col)
{
  float r = lw * 0.5f;
  if (r < 0.5f) r = 0.5f;
  int min_x = (int)floorf(fminf(x0, x1) - r - 1.0f);
  int max_x = (int)ceilf(fmaxf(x0, x1) + r + 1.0f);
  int min_y = (int)floorf(fminf(y0, y1) - r - 1.0f);
  int max_y = (int)ceilf(fmaxf(y0, y1) + r + 1.0f);
  if (min_x < 0) min_x = 0;
  if (min_y < 0) min_y = 0;
  if (max_x >= c->w) max_x = c->w - 1;
  if (max_y >= c->h) max_y = c->h - 1;

  float dx = x1 - x0, dy = y1 - y0;
  float len2 = dx * dx + dy * dy;
  for (int y = min_y; y <= max_y; y++) {
    for (int x = min_x; x <= max_x; x++) {
      float px = (float)x + 0.5f, py = (float)y + 0.5f;
      float t = len2 > 1e-6f ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0.0f;
      if (t < 0.0f) t = 0.0f;
      if (t > 1.0f) t = 1.0f;
      float cx = x0 + t * dx, cy = y0 + t * dy;
      float d = hypotf(px - cx, py - cy);
      if (d <= r + 0.5f) {
        float cov = d <= r - 0.5f ? 1.0f : (r + 0.5f - d);
        blend_px(c, x, y, col, c->st.global_alpha * cov);
      }
    }
  }
}

static void fill_polygon_path(web_canvas *c, ccord_color col)
{
  if (c->path_len < 3) return;
  float min_y = 1e9f, max_y = -1e9f;
  for (size_t i = 0; i < c->path_len; i++) {
    if (c->path[i].y < min_y) min_y = c->path[i].y;
    if (c->path[i].y > max_y) max_y = c->path[i].y;
  }
  int y0 = (int)floorf(min_y);
  int y1 = (int)ceilf(max_y);
  if (y0 < 0) y0 = 0;
  if (y1 >= c->h) y1 = c->h - 1;

  float xs[512];
  for (int y = y0; y <= y1; y++) {
    float fy = (float)y + 0.5f;
    size_t nx = 0;
    size_t sub_start = 0;
    for (size_t i = 0; i < c->path_len; i++) {
      if (i == 0 || c->path[i].move) { sub_start = i; continue; }
      float ax = c->path[i - 1u].x, ay = c->path[i - 1u].y;
      float bx = c->path[i].x, by = c->path[i].y;
      if ((ay <= fy && by > fy) || (by <= fy && ay > fy)) {
        if (nx < 512u) xs[nx++] = ax + (fy - ay) * (bx - ax) / (by - ay);
      }
      // Close implicit last edge of subpath
      bool end_sub = (i + 1u == c->path_len) || c->path[i + 1u].move;
      if (end_sub && i > sub_start) {
        ax = c->path[i].x; ay = c->path[i].y;
        bx = c->path[sub_start].x; by = c->path[sub_start].y;
        if ((ay <= fy && by > fy) || (by <= fy && ay > fy)) {
          if (nx < 512u) xs[nx++] = ax + (fy - ay) * (bx - ax) / (by - ay);
        }
      }
    }
    // Insertion sort intersections
    for (size_t i = 1; i < nx; i++) {
      float key = xs[i];
      size_t j = i;
      while (j > 0 && xs[j - 1u] > key) { xs[j] = xs[j - 1u]; j--; }
      xs[j] = key;
    }
    for (size_t i = 0; i + 1u < nx; i += 2u) {
      int x0 = (int)ceilf(xs[i] - 0.5f);
      int x1 = (int)floorf(xs[i + 1u] - 0.5f);
      if (x0 < 0) x0 = 0;
      if (x1 >= c->w) x1 = c->w - 1;
      for (int x = x0; x <= x1; x++) blend_px(c, x, y, col, c->st.global_alpha);
    }
  }
}

// Compact 5x7 ASCII glyph bitmap (printable 0x20..0x7E) for canvas fillText/strokeText.
static uint8_t glyph_row5x7(char ch, int row)
{
  unsigned char u = (unsigned char)ch;
  if (u >= 'a' && u <= 'z') u = (unsigned char)(u - 'a' + 'A');
  // 7 rows x 5 bits per glyph (compact clean geometric shapes for canvas text & labels)
  static const uint8_t k_digits[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
  };
  static const uint8_t k_alpha[26][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
  };
  if (row < 0 || row >= 7) return 0;
  if (u >= '0' && u <= '9') return k_digits[u - '0'][row];
  if (u >= 'A' && u <= 'Z') return k_alpha[u - 'A'][row];
  if (u == ' ') return 0;
  if (u == '.') return row == 6 ? 0x04 : 0;
  if (u == '-') return row == 3 ? 0x0E : 0;
  if (u == ':') return (row == 2 || row == 5) ? 0x04 : 0;
  if (u == '!') return (row <= 4 || row == 6) ? 0x04 : 0;
  if (u == '?') return (const uint8_t[7]){0x0E,0x11,0x01,0x06,0x04,0x00,0x04}[row];
  return (row == 0 || row == 6) ? 0x1F : 0x11; // fallback box
}

static void draw_canvas_text(web_canvas *c, const char *txt, float x, float y, ccord_color col)
{
  if (!txt || !txt[0]) return;
  float scale = c->st.font_px / 7.0f;
  if (scale < 1.0f) scale = 1.0f;
  size_t n = strlen(txt);
  float adv = 6.0f * scale;
  float total_w = (float)n * adv;
  if (c->st.text_align == 1) x -= total_w * 0.5f;
  else if (c->st.text_align == 2) x -= total_w;
  if (c->st.text_baseline == 0) y -= 7.0f * scale;
  else if (c->st.text_baseline == 2) y -= 3.5f * scale;
  else if (c->st.text_baseline == 3) y -= 7.0f * scale;

  float tx, ty;
  xform_pt(c->st.m, x, y, &tx, &ty);
  int iscale = (int)roundf(scale);
  if (iscale < 1) iscale = 1;
  for (size_t i = 0; i < n; i++) {
    int gx = (int)roundf(tx + (float)i * adv);
    int gy = (int)roundf(ty);
    for (int r = 0; r < 7; r++) {
      uint8_t bits = glyph_row5x7(txt[i], r);
      for (int col_i = 0; col_i < 5; col_i++) {
        if ((bits & (1u << (4 - col_i))) != 0) {
          for (int sy = 0; sy < iscale; sy++) {
            for (int sx = 0; sx < iscale; sx++) {
              blend_px(c, gx + col_i * iscale + sx, gy + r * iscale + sy, col, c->st.global_alpha);
            }
          }
        }
      }
    }
  }
}

// ---- PNG encoder via zlib for canvas.toDataURL("image/png") ----

static void png_write_u32be(ccord_buf *b, uint32_t v)
{
  uint8_t d[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
  (void)ccord_buf_append(b, d, 4);
}

static void png_write_chunk(ccord_buf *b, const char tag[4], const uint8_t *data, size_t len)
{
  png_write_u32be(b, (uint32_t)len);
  (void)ccord_buf_append(b, tag, 4);
  if (len > 0) (void)ccord_buf_append(b, data, len);
  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, (const Bytef *)tag, 4);
  if (len > 0) crc = crc32(crc, (const Bytef *)data, (uInt)len);
  png_write_u32be(b, (uint32_t)crc);
}

bool web_canvas_to_data_url(web_canvas *c, const char *mime, ccord_buf *out)
{
  (void)mime;
  if (!c || !c->rgba || !out) return false;
  size_t row_bytes = (size_t)c->w * 4u + 1u;
  size_t raw_len = row_bytes * (size_t)c->h;
  uint8_t *raw = ccord_malloc(raw_len);
  if (!raw) return false;
  for (int y = 0; y < c->h; y++) {
    raw[(size_t)y * row_bytes] = 0; // filter: None
    memcpy(raw + (size_t)y * row_bytes + 1u, c->rgba + (size_t)y * (size_t)c->w * 4u, (size_t)c->w * 4u);
  }
  uLongf comp_cap = compressBound((uLong)raw_len);
  uint8_t *comp = ccord_malloc(comp_cap);
  if (!comp) { ccord_free(raw); return false; }
  int zr = compress2(comp, &comp_cap, raw, (uLong)raw_len, 1);
  ccord_free(raw);
  if (zr != Z_OK) { ccord_free(comp); return false; }

  ccord_buf png;
  ccord_buf_init(&png, comp_cap + 128u);
  static const uint8_t k_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  (void)ccord_buf_append(&png, k_sig, 8);
  uint8_t ihdr[13] = {
    (uint8_t)(c->w >> 24), (uint8_t)(c->w >> 16), (uint8_t)(c->w >> 8), (uint8_t)c->w,
    (uint8_t)(c->h >> 24), (uint8_t)(c->h >> 16), (uint8_t)(c->h >> 8), (uint8_t)c->h,
    8, 6, 0, 0, 0
  };
  png_write_chunk(&png, "IHDR", ihdr, 13);
  png_write_chunk(&png, "IDAT", comp, (size_t)comp_cap);
  png_write_chunk(&png, "IEND", nullptr, 0);
  ccord_free(comp);

  size_t b64_len = 0;
  char *b64 = ccord_base64_encode_alloc((const uint8_t *)png.data, png.len, &b64_len);
  ccord_buf_free(&png);
  if (!b64) return false;
  (void)ccord_buf_append_str(out, "data:image/png;base64,");
  (void)ccord_buf_append(out, b64, b64_len);
  ccord_free(b64);
  return true;
}

void web_canvas_paint(web_canvas *c, ccord_renderer *r, ccord_rect dst)
{
  if (!c || !r || !c->rgba || dst.w <= 0.0f || dst.h <= 0.0f) return;
  if (c->dirty || !ccord_render_has_texture(r, c->tex_key, nullptr, nullptr)) {
    ccord_error err = {0};
    if (ccord_render_upload_rgba(r, c->tex_key, c->rgba, c->w, c->h, &err) == CCORD_OK) {
      c->dirty = false;
    }
  }
  ccord_draw_texture(r, c->tex_key, dst, 255);
}

// ---- JS CanvasRenderingContext2D bindings ----

static web_node *ctx2d_node(JSContext *ctx, JSValueConst this_val)
{
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || !f->view) return nullptr;
  return JS_GetOpaque(this_val, f->view->cls_canvas_ctx);
}

static web_canvas *ctx2d_canvas(JSContext *ctx, JSValueConst this_val)
{
  web_node *n = ctx2d_node(ctx, this_val);
  return n ? n->canvas : nullptr;
}

static void ctx2d_mark_dirty(JSContext *ctx, web_node *n)
{
  (void)ctx;
  if (n && n->canvas) n->canvas->dirty = true;
  if (n && n->frame && n->frame->view) web_mark_dirty(n->frame->view);
}

static JSValue c2d_clear_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = ctx2d_node(ctx, this_val);
  web_canvas *c = n ? n->canvas : nullptr;
  if (!c || argc < 4) return JS_UNDEFINED;
  double x = 0, y = 0, w = 0, h = 0;
  (void)JS_ToFloat64(ctx, &x, argv[0]); (void)JS_ToFloat64(ctx, &y, argv[1]);
  (void)JS_ToFloat64(ctx, &w, argv[2]); (void)JS_ToFloat64(ctx, &h, argv[3]);
  float x0, y0, x1, y1;
  xform_pt(c->st.m, (float)x, (float)y, &x0, &y0);
  xform_pt(c->st.m, (float)(x + w), (float)(y + h), &x1, &y1);
  int ix0 = (int)floorf(fminf(x0, x1)), ix1 = (int)ceilf(fmaxf(x0, x1));
  int iy0 = (int)floorf(fminf(y0, y1)), iy1 = (int)ceilf(fmaxf(y0, y1));
  if (ix0 < 0) ix0 = 0;
  if (iy0 < 0) iy0 = 0;
  if (ix1 > c->w) ix1 = c->w;
  if (iy1 > c->h) iy1 = c->h;
  for (int py = iy0; py < iy1; py++) {
    memset(c->rgba + ((size_t)py * (size_t)c->w + (size_t)ix0) * 4u, 0, (size_t)(ix1 - ix0) * 4u);
  }
  ctx2d_mark_dirty(ctx, n);
  return JS_UNDEFINED;
}

static JSValue c2d_fill_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = ctx2d_node(ctx, this_val);
  web_canvas *c = n ? n->canvas : nullptr;
  if (!c || argc < 4) return JS_UNDEFINED;
  double x = 0, y = 0, w = 0, h = 0;
  (void)JS_ToFloat64(ctx, &x, argv[0]); (void)JS_ToFloat64(ctx, &y, argv[1]);
  (void)JS_ToFloat64(ctx, &w, argv[2]); (void)JS_ToFloat64(ctx, &h, argv[3]);
  float x0, y0, x1, y1;
  xform_pt(c->st.m, (float)x, (float)y, &x0, &y0);
  xform_pt(c->st.m, (float)(x + w), (float)(y + h), &x1, &y1);
  int ix0 = (int)floorf(fminf(x0, x1)), ix1 = (int)ceilf(fmaxf(x0, x1));
  int iy0 = (int)floorf(fminf(y0, y1)), iy1 = (int)ceilf(fmaxf(y0, y1));
  if (ix0 < 0) ix0 = 0;
  if (iy0 < 0) iy0 = 0;
  if (ix1 > c->w) ix1 = c->w;
  if (iy1 > c->h) iy1 = c->h;
  for (int py = iy0; py < iy1; py++) {
    for (int px = ix0; px < ix1; px++) {
      blend_px(c, px, py, c->st.fill, c->st.global_alpha);
    }
  }
  ctx2d_mark_dirty(ctx, n);
  return JS_UNDEFINED;
}

static JSValue c2d_stroke_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = ctx2d_node(ctx, this_val);
  web_canvas *c = n ? n->canvas : nullptr;
  if (!c || argc < 4) return JS_UNDEFINED;
  double x = 0, y = 0, w = 0, h = 0;
  (void)JS_ToFloat64(ctx, &x, argv[0]); (void)JS_ToFloat64(ctx, &y, argv[1]);
  (void)JS_ToFloat64(ctx, &w, argv[2]); (void)JS_ToFloat64(ctx, &h, argv[3]);
  float p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y;
  xform_pt(c->st.m, (float)x, (float)y, &p0x, &p0y);
  xform_pt(c->st.m, (float)(x + w), (float)y, &p1x, &p1y);
  xform_pt(c->st.m, (float)(x + w), (float)(y + h), &p2x, &p2y);
  xform_pt(c->st.m, (float)x, (float)(y + h), &p3x, &p3y);
  draw_thick_line(c, p0x, p0y, p1x, p1y, c->st.line_width, c->st.stroke);
  draw_thick_line(c, p1x, p1y, p2x, p2y, c->st.line_width, c->st.stroke);
  draw_thick_line(c, p2x, p2y, p3x, p3y, c->st.line_width, c->st.stroke);
  draw_thick_line(c, p3x, p3y, p0x, p0y, c->st.line_width, c->st.stroke);
  ctx2d_mark_dirty(ctx, n);
  return JS_UNDEFINED;
}

static JSValue c2d_begin_path(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (c) { c->path_len = 0; c->has_cur = false; }
  return JS_UNDEFINED;
}

static JSValue c2d_close_path(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (c && c->path_len > 0) {
    path_push(c, c->sub_x, c->sub_y, false);
    c->cur_x = c->sub_x; c->cur_y = c->sub_y;
  }
  return JS_UNDEFINED;
}

static JSValue c2d_move_to(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 2) return JS_UNDEFINED;
  double x = 0, y = 0;
  (void)JS_ToFloat64(ctx, &x, argv[0]); (void)JS_ToFloat64(ctx, &y, argv[1]);
  c->sub_x = c->cur_x = (float)x;
  c->sub_y = c->cur_y = (float)y;
  c->has_cur = true;
  path_push(c, (float)x, (float)y, true);
  return JS_UNDEFINED;
}

static JSValue c2d_line_to(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 2) return JS_UNDEFINED;
  double x = 0, y = 0;
  (void)JS_ToFloat64(ctx, &x, argv[0]); (void)JS_ToFloat64(ctx, &y, argv[1]);
  bool first = (c->path_len == 0);
  if (first) { c->sub_x = (float)x; c->sub_y = (float)y; }
  c->cur_x = (float)x; c->cur_y = (float)y;
  c->has_cur = true;
  path_push(c, (float)x, (float)y, first);
  return JS_UNDEFINED;
}

static JSValue c2d_bezier_to(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 6) return JS_UNDEFINED;
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0, x = 0, y = 0;
  (void)JS_ToFloat64(ctx, &x1, argv[0]); (void)JS_ToFloat64(ctx, &y1, argv[1]);
  (void)JS_ToFloat64(ctx, &x2, argv[2]); (void)JS_ToFloat64(ctx, &y2, argv[3]);
  (void)JS_ToFloat64(ctx, &x, argv[4]);  (void)JS_ToFloat64(ctx, &y, argv[5]);
  double x0 = c->has_cur ? (double)c->cur_x : x1;
  double y0 = c->has_cur ? (double)c->cur_y : y1;
  if (!c->has_cur) { c->sub_x = (float)x0; c->sub_y = (float)y0; path_push(c, (float)x0, (float)y0, true); }
  const int steps = 16;
  for (int i = 1; i <= steps; i++) {
    double t = (double)i / (double)steps;
    double u = 1.0 - t;
    double px = u*u*u*x0 + 3.0*u*u*t*x1 + 3.0*u*t*t*x2 + t*t*t*x;
    double py = u*u*u*y0 + 3.0*u*u*t*y1 + 3.0*u*t*t*y2 + t*t*t*y;
    path_push(c, (float)px, (float)py, false);
  }
  c->cur_x = (float)x; c->cur_y = (float)y; c->has_cur = true;
  return JS_UNDEFINED;
}

static JSValue c2d_quad_to(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 4) return JS_UNDEFINED;
  double cx = 0, cy = 0, x = 0, y = 0;
  (void)JS_ToFloat64(ctx, &cx, argv[0]); (void)JS_ToFloat64(ctx, &cy, argv[1]);
  (void)JS_ToFloat64(ctx, &x, argv[2]);  (void)JS_ToFloat64(ctx, &y, argv[3]);
  double x0 = c->has_cur ? (double)c->cur_x : cx;
  double y0 = c->has_cur ? (double)c->cur_y : cy;
  if (!c->has_cur) { c->sub_x = (float)x0; c->sub_y = (float)y0; path_push(c, (float)x0, (float)y0, true); }
  const int steps = 12;
  for (int i = 1; i <= steps; i++) {
    double t = (double)i / (double)steps;
    double u = 1.0 - t;
    double px = u*u*x0 + 2.0*u*t*cx + t*t*x;
    double py = u*u*y0 + 2.0*u*t*cy + t*t*y;
    path_push(c, (float)px, (float)py, false);
  }
  c->cur_x = (float)x; c->cur_y = (float)y; c->has_cur = true;
  return JS_UNDEFINED;
}

static JSValue c2d_arc_to(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 5) return JS_UNDEFINED;
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0, r = 0;
  (void)JS_ToFloat64(ctx, &x1, argv[0]); (void)JS_ToFloat64(ctx, &y1, argv[1]);
  (void)JS_ToFloat64(ctx, &x2, argv[2]); (void)JS_ToFloat64(ctx, &y2, argv[3]);
  (void)JS_ToFloat64(ctx, &r, argv[4]);
  double x0 = c->has_cur ? (double)c->cur_x : x1;
  double y0 = c->has_cur ? (double)c->cur_y : y1;
  // Approximate smooth corner with quadratic Bezier tangent at distance r along both legs
  double dx0 = x0 - x1, dy0 = y0 - y1, l0 = hypot(dx0, dy0);
  double dx2 = x2 - x1, dy2 = y2 - y1, l2 = hypot(dx2, dy2);
  if (r <= 0.0 || l0 < 1e-5 || l2 < 1e-5) {
    path_push(c, (float)x1, (float)y1, !c->has_cur);
    c->cur_x = (float)x1; c->cur_y = (float)y1; c->has_cur = true;
    return JS_UNDEFINED;
  }
  double d0 = fmin(r, l0 * 0.5), d2 = fmin(r, l2 * 0.5);
  double ax = x1 + (dx0 / l0) * d0, ay = y1 + (dy0 / l0) * d0;
  double bx = x1 + (dx2 / l2) * d2, by = y1 + (dy2 / l2) * d2;
  path_push(c, (float)ax, (float)ay, !c->has_cur);
  for (int i = 1; i <= 8; i++) {
    double t = (double)i / 8.0, u = 1.0 - t;
    path_push(c, (float)(u*u*ax + 2.0*u*t*x1 + t*t*bx), (float)(u*u*ay + 2.0*u*t*y1 + t*t*by), false);
  }
  c->cur_x = (float)bx; c->cur_y = (float)by; c->has_cur = true;
  return JS_UNDEFINED;
}

static JSValue c2d_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 4) return JS_UNDEFINED;
  double x = 0, y = 0, w = 0, h = 0;
  (void)JS_ToFloat64(ctx, &x, argv[0]); (void)JS_ToFloat64(ctx, &y, argv[1]);
  (void)JS_ToFloat64(ctx, &w, argv[2]); (void)JS_ToFloat64(ctx, &h, argv[3]);
  c->sub_x = c->cur_x = (float)x;
  c->sub_y = c->cur_y = (float)y;
  c->has_cur = true;
  path_push(c, (float)x, (float)y, true);
  path_push(c, (float)(x + w), (float)y, false);
  path_push(c, (float)(x + w), (float)(y + h), false);
  path_push(c, (float)x, (float)(y + h), false);
  path_push(c, (float)x, (float)y, false);
  return JS_UNDEFINED;
}

static JSValue c2d_round_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 4) return JS_UNDEFINED;
  double x = 0, y = 0, w = 0, h = 0, rad = 0;
  (void)JS_ToFloat64(ctx, &x, argv[0]); (void)JS_ToFloat64(ctx, &y, argv[1]);
  (void)JS_ToFloat64(ctx, &w, argv[2]); (void)JS_ToFloat64(ctx, &h, argv[3]);
  if (argc >= 5) {
    if (JS_IsArray(argv[4])) {
      JSValue r0 = JS_GetPropertyUint32(ctx, argv[4], 0);
      (void)JS_ToFloat64(ctx, &rad, r0);
      JS_FreeValue(ctx, r0);
    } else if (!JS_IsUndefined(argv[4])) {
      (void)JS_ToFloat64(ctx, &rad, argv[4]);
    }
  }
  double maxr = fmin(fabs(w), fabs(h)) * 0.5;
  if (rad < 0.0) rad = 0.0;
  if (rad > maxr) rad = maxr;
  if (rad < 0.5) return c2d_rect(ctx, this_val, 4, argv);
  const double pi = 3.141592653589793;
  double cxs[4] = {x + w - rad, x + w - rad, x + rad, x + rad};
  double cys[4] = {y + rad, y + h - rad, y + h - rad, y + rad};
  double a0s[4] = {-pi * 0.5, 0.0, pi * 0.5, pi};
  c->sub_x = (float)(x + rad); c->sub_y = (float)y;
  bool first = true;
  for (int k = 0; k < 4; k++) {
    for (int i = 0; i <= 4; i++) {
      double a = a0s[k] + (pi * 0.5) * ((double)i / 4.0);
      float px = (float)(cxs[k] + cos(a) * rad);
      float py = (float)(cys[k] + sin(a) * rad);
      path_push(c, px, py, first);
      first = false;
    }
  }
  path_push(c, c->sub_x, c->sub_y, false);
  c->cur_x = c->sub_x; c->cur_y = c->sub_y; c->has_cur = true;
  return JS_UNDEFINED;
}

static JSValue c2d_ellipse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 7) return JS_UNDEFINED;
  double cx = 0, cy = 0, rx = 0, ry = 0, rot = 0, sa = 0, ea = 0;
  (void)JS_ToFloat64(ctx, &cx, argv[0]); (void)JS_ToFloat64(ctx, &cy, argv[1]);
  (void)JS_ToFloat64(ctx, &rx, argv[2]); (void)JS_ToFloat64(ctx, &ry, argv[3]);
  (void)JS_ToFloat64(ctx, &rot, argv[4]);
  (void)JS_ToFloat64(ctx, &sa, argv[5]); (void)JS_ToFloat64(ctx, &ea, argv[6]);
  bool ccw = argc > 7 && JS_ToBool(ctx, argv[7]) > 0;
  double span = ea - sa;
  const double two_pi = 6.283185307179586;
  if (!ccw && span < 0) span = fmod(span, two_pi) + two_pi;
  if (ccw && span > 0) span = fmod(span, two_pi) - two_pi;
  int segs = (int)fmax(12.0, fabs(span) * fmax(rx, ry) * 0.5);
  if (segs > 128) segs = 128;
  double cr = cos(rot), sr = sin(rot);
  for (int i = 0; i <= segs; i++) {
    double a = sa + span * ((double)i / (double)segs);
    double lx = cos(a) * rx, ly = sin(a) * ry;
    float px = (float)(cx + lx * cr - ly * sr);
    float py = (float)(cy + lx * sr + ly * cr);
    path_push(c, px, py, i == 0 && c->path_len == 0);
    c->cur_x = px; c->cur_y = py; c->has_cur = true;
  }
  return JS_UNDEFINED;
}

static JSValue c2d_arc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 5) return JS_UNDEFINED;
  double cx = 0, cy = 0, r = 0, sa = 0, ea = 0;
  (void)JS_ToFloat64(ctx, &cx, argv[0]); (void)JS_ToFloat64(ctx, &cy, argv[1]);
  (void)JS_ToFloat64(ctx, &r, argv[2]); (void)JS_ToFloat64(ctx, &sa, argv[3]);
  (void)JS_ToFloat64(ctx, &ea, argv[4]);
  bool ccw = argc > 5 && JS_ToBool(ctx, argv[5]) > 0;
  double span = ea - sa;
  const double two_pi = 6.283185307179586;
  if (!ccw && span < 0) span = fmod(span, two_pi) + two_pi;
  if (ccw && span > 0) span = fmod(span, two_pi) - two_pi;
  int segs = (int)fmax(12.0, fabs(span) * r * 0.5);
  if (segs > 128) segs = 128;
  for (int i = 0; i <= segs; i++) {
    double a = sa + span * ((double)i / (double)segs);
    float px = (float)(cx + cos(a) * r);
    float py = (float)(cy + sin(a) * r);
    path_push(c, px, py, i == 0 && c->path_len == 0);
    c->cur_x = px; c->cur_y = py; c->has_cur = true;
  }
  return JS_UNDEFINED;
}

static void maybe_apply_path2d(JSContext *ctx, JSValueConst this_val, JSValueConst maybe_path)
{
  if (!JS_IsObject(maybe_path)) return;
  JSValue replay = JS_GetPropertyStr(ctx, maybe_path, "__replayTo");
  if (JS_IsFunction(ctx, replay)) {
    JSValue arg = this_val;
    JSValue rv = JS_Call(ctx, replay, maybe_path, 1, &arg);
    if (JS_IsException(rv)) { JSValue ex = JS_GetException(ctx); JS_FreeValue(ctx, ex); }
    JS_FreeValue(ctx, rv);
  }
  JS_FreeValue(ctx, replay);
}

static JSValue c2d_fill(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = ctx2d_node(ctx, this_val);
  web_canvas *c = n ? n->canvas : nullptr;
  if (!c) return JS_UNDEFINED;
  if (argc > 0 && JS_IsObject(argv[0])) maybe_apply_path2d(ctx, this_val, argv[0]);
  fill_polygon_path(c, c->st.fill);
  ctx2d_mark_dirty(ctx, n);
  return JS_UNDEFINED;
}

static JSValue c2d_stroke(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = ctx2d_node(ctx, this_val);
  web_canvas *c = n ? n->canvas : nullptr;
  if (!c) return JS_UNDEFINED;
  if (argc > 0 && JS_IsObject(argv[0])) maybe_apply_path2d(ctx, this_val, argv[0]);
  for (size_t i = 1; i < c->path_len; i++) {
    if (!c->path[i].move) {
      draw_thick_line(c, c->path[i - 1u].x, c->path[i - 1u].y,
                      c->path[i].x, c->path[i].y, c->st.line_width, c->st.stroke);
    }
  }
  ctx2d_mark_dirty(ctx, n);
  return JS_UNDEFINED;
}

static JSValue c2d_is_point_in_path(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 2) return JS_FALSE;
  int off = 0;
  if (JS_IsObject(argv[0]) && argc >= 3) {
    maybe_apply_path2d(ctx, this_val, argv[0]);
    off = 1;
  }
  double x = 0, y = 0;
  (void)JS_ToFloat64(ctx, &x, argv[off]);
  (void)JS_ToFloat64(ctx, &y, argv[off + 1]);
  float px = (float)x, py = (float)y;
  bool inside = false;
  size_t sub_start = 0;
  for (size_t i = 0; i < c->path_len; i++) {
    if (i == 0 || c->path[i].move) { sub_start = i; continue; }
    float ax = c->path[i - 1u].x, ay = c->path[i - 1u].y;
    float bx = c->path[i].x, by = c->path[i].y;
    if (((ay > py) != (by > py)) && (px < (bx - ax) * (py - ay) / (by - ay) + ax)) inside = !inside;
    bool end_sub = (i + 1u == c->path_len) || c->path[i + 1u].move;
    if (end_sub && i > sub_start) {
      ax = c->path[i].x; ay = c->path[i].y;
      bx = c->path[sub_start].x; by = c->path[sub_start].y;
      if (((ay > py) != (by > py)) && (px < (bx - ax) * (py - ay) / (by - ay) + ax)) inside = !inside;
    }
  }
  return JS_NewBool(ctx, inside);
}

static JSValue c2d_is_point_in_stroke(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 2) return JS_FALSE;
  int off = 0;
  if (JS_IsObject(argv[0]) && argc >= 3) {
    maybe_apply_path2d(ctx, this_val, argv[0]);
    off = 1;
  }
  double x = 0, y = 0;
  (void)JS_ToFloat64(ctx, &x, argv[off]);
  (void)JS_ToFloat64(ctx, &y, argv[off + 1]);
  float px = (float)x, py = (float)y;
  float hr = fmaxf(1.0f, c->st.line_width * 0.5f);
  for (size_t i = 1; i < c->path_len; i++) {
    if (c->path[i].move) continue;
    float ax = c->path[i - 1u].x, ay = c->path[i - 1u].y;
    float dx = c->path[i].x - ax, dy = c->path[i].y - ay;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 1e-6f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    if (hypotf(px - (ax + t * dx), py - (ay + t * dy)) <= hr) return JS_TRUE;
  }
  return JS_FALSE;
}

static JSValue c2d_fill_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = ctx2d_node(ctx, this_val);
  web_canvas *c = n ? n->canvas : nullptr;
  if (!c || argc < 3) return JS_UNDEFINED;
  const char *s = JS_ToCString(ctx, argv[0]);
  if (!s) return JS_EXCEPTION;
  double x = 0, y = 0;
  (void)JS_ToFloat64(ctx, &x, argv[1]); (void)JS_ToFloat64(ctx, &y, argv[2]);
  draw_canvas_text(c, s, (float)x, (float)y, c->st.fill);
  JS_FreeCString(ctx, s);
  ctx2d_mark_dirty(ctx, n);
  return JS_UNDEFINED;
}

static JSValue c2d_measure_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  double w = 0.0;
  if (c && argc > 0) {
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    w = (double)strlen(s) * (double)(c->st.font_px * 0.6f);
    JS_FreeCString(ctx, s);
  }
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, w));
  return o;
}

static JSValue c2d_draw_image(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = ctx2d_node(ctx, this_val);
  web_canvas *c = n ? n->canvas : nullptr;
  if (!c || argc < 3) return JS_UNDEFINED;
  web_node *src_node = web_node_unwrap(ctx, argv[0]);
  if (!src_node && JS_IsObject(argv[0])) {
    JSValue inner = JS_GetPropertyStr(ctx, argv[0], "_canvas");
    if (JS_IsUndefined(inner) || JS_IsNull(inner)) {
      JS_FreeValue(ctx, inner);
      inner = JS_GetPropertyStr(ctx, argv[0], "_img");
    }
    src_node = web_node_unwrap(ctx, inner);
    JS_FreeValue(ctx, inner);
  }
  const uint8_t *src_px = nullptr;
  int sw_tot = 0, sh_tot = 0;
  if (src_node && src_node->img && src_node->img->loaded && src_node->img->rgba) {
    src_px = src_node->img->rgba;
    sw_tot = src_node->img->w;
    sh_tot = src_node->img->h;
  } else if (src_node && src_node->canvas && src_node->canvas->rgba) {
    src_px = src_node->canvas->rgba;
    sw_tot = src_node->canvas->w;
    sh_tot = src_node->canvas->h;
  }
  if (!src_px || sw_tot <= 0 || sh_tot <= 0) return JS_UNDEFINED;

  double sx = 0, sy = 0, sw = sw_tot, sh = sh_tot;
  double dx = 0, dy = 0, dw = sw_tot, dh = sh_tot;
  if (argc >= 9) {
    (void)JS_ToFloat64(ctx, &sx, argv[1]); (void)JS_ToFloat64(ctx, &sy, argv[2]);
    (void)JS_ToFloat64(ctx, &sw, argv[3]); (void)JS_ToFloat64(ctx, &sh, argv[4]);
    (void)JS_ToFloat64(ctx, &dx, argv[5]); (void)JS_ToFloat64(ctx, &dy, argv[6]);
    (void)JS_ToFloat64(ctx, &dw, argv[7]); (void)JS_ToFloat64(ctx, &dh, argv[8]);
  } else if (argc >= 5) {
    (void)JS_ToFloat64(ctx, &dx, argv[1]); (void)JS_ToFloat64(ctx, &dy, argv[2]);
    (void)JS_ToFloat64(ctx, &dw, argv[3]); (void)JS_ToFloat64(ctx, &dh, argv[4]);
  } else {
    (void)JS_ToFloat64(ctx, &dx, argv[1]); (void)JS_ToFloat64(ctx, &dy, argv[2]);
  }
  if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return JS_UNDEFINED;

  float x0, y0, x1, y1;
  xform_pt(c->st.m, (float)dx, (float)dy, &x0, &y0);
  xform_pt(c->st.m, (float)(dx + dw), (float)(dy + dh), &x1, &y1);
  int ix0 = (int)floorf(fminf(x0, x1)), ix1 = (int)ceilf(fmaxf(x0, x1));
  int iy0 = (int)floorf(fminf(y0, y1)), iy1 = (int)ceilf(fmaxf(y0, y1));
  int rw = ix1 - ix0, rh = iy1 - iy0;
  if (rw <= 0 || rh <= 0) return JS_UNDEFINED;
  for (int py = iy0; py < iy1; py++) {
    if (py < 0 || py >= c->h) continue;
    int src_y = (int)(sy + ((double)(py - iy0) / (double)rh) * sh);
    if (src_y < 0 || src_y >= sh_tot) continue;
    for (int px = ix0; px < ix1; px++) {
      if (px < 0 || px >= c->w) continue;
      int src_x = (int)(sx + ((double)(px - ix0) / (double)rw) * sw);
      if (src_x < 0 || src_x >= sw_tot) continue;
      const uint8_t *sp = src_px + ((size_t)src_y * (size_t)sw_tot + (size_t)src_x) * 4u;
      ccord_color sc = {sp[0], sp[1], sp[2], sp[3]};
      blend_px(c, px, py, sc, c->st.global_alpha);
    }
  }
  ctx2d_mark_dirty(ctx, n);
  return JS_UNDEFINED;
}

static JSValue c2d_get_image_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 4) return JS_UNDEFINED;
  int32_t sx = 0, sy = 0, sw = 0, sh = 0;
  (void)JS_ToInt32(ctx, &sx, argv[0]); (void)JS_ToInt32(ctx, &sy, argv[1]);
  (void)JS_ToInt32(ctx, &sw, argv[2]); (void)JS_ToInt32(ctx, &sh, argv[3]);
  if (sw < 1) sw = 1;
  if (sh < 1) sh = 1;
  if (sw > CANVAS_MAX_DIM) sw = CANVAS_MAX_DIM;
  if (sh > CANVAS_MAX_DIM) sh = CANVAS_MAX_DIM;
  size_t nbytes = (size_t)sw * (size_t)sh * 4u;
  uint8_t *tmp = ccord_calloc(1, nbytes);
  if (!tmp) return JS_ThrowOutOfMemory(ctx);
  for (int y = 0; y < sh; y++) {
    int cy = sy + y;
    if (cy < 0 || cy >= c->h) continue;
    for (int x = 0; x < sw; x++) {
      int cx = sx + x;
      if (cx < 0 || cx >= c->w) continue;
      memcpy(tmp + ((size_t)y * (size_t)sw + (size_t)x) * 4u,
             c->rgba + ((size_t)cy * (size_t)c->w + (size_t)cx) * 4u, 4u);
    }
  }
  JSValue ab = JS_NewArrayBufferCopy(ctx, tmp, nbytes);
  ccord_free(tmp);
  JSValue args[1] = {ab};
  JSValue u8c = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_UINT8C);
  JS_FreeValue(ctx, ab);
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, sw));
  JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, sh));
  JS_SetPropertyStr(ctx, obj, "data", u8c);
  return obj;
}

static JSValue c2d_put_image_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = ctx2d_node(ctx, this_val);
  web_canvas *c = n ? n->canvas : nullptr;
  if (!c || argc < 3 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
  int32_t dx = 0, dy = 0, w = 0, h = 0;
  (void)JS_ToInt32(ctx, &dx, argv[1]); (void)JS_ToInt32(ctx, &dy, argv[2]);
  JSValue jw = JS_GetPropertyStr(ctx, argv[0], "width");
  JSValue jh = JS_GetPropertyStr(ctx, argv[0], "height");
  JSValue jd = JS_GetPropertyStr(ctx, argv[0], "data");
  (void)JS_ToInt32(ctx, &w, jw); (void)JS_ToInt32(ctx, &h, jh);
  size_t off = 0, blen = 0, bpe = 0, ab_sz = 0;
  JSValue ab = JS_GetTypedArrayBuffer(ctx, jd, &off, &blen, &bpe);
  if (!JS_IsException(ab)) {
    uint8_t *p = JS_GetArrayBuffer(ctx, &ab_sz, ab);
    if (p && w > 0 && h > 0 && off + (size_t)w * (size_t)h * 4u <= ab_sz) {
      const uint8_t *src = p + off;
      for (int y = 0; y < h; y++) {
        int cy = dy + y;
        if (cy < 0 || cy >= c->h) continue;
        for (int x = 0; x < w; x++) {
          int cx = dx + x;
          if (cx < 0 || cx >= c->w) continue;
          memcpy(c->rgba + ((size_t)cy * (size_t)c->w + (size_t)cx) * 4u,
                 src + ((size_t)y * (size_t)w + (size_t)x) * 4u, 4u);
        }
      }
      ctx2d_mark_dirty(ctx, n);
    }
    JS_FreeValue(ctx, ab);
  } else {
    JSValue ex = JS_GetException(ctx);
    JS_FreeValue(ctx, ex);
  }
  JS_FreeValue(ctx, jw); JS_FreeValue(ctx, jh); JS_FreeValue(ctx, jd);
  return JS_UNDEFINED;
}

static JSValue c2d_save(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (c && c->stack_len < CANVAS_STACK_MAX) c->stack[c->stack_len++] = c->st;
  return JS_UNDEFINED;
}

static JSValue c2d_restore(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (c && c->stack_len > 0) c->st = c->stack[--c->stack_len];
  return JS_UNDEFINED;
}

static void mat_mul(float dst[6], const float a[6], const float b[6])
{
  float r[6] = {
    a[0] * b[0] + a[2] * b[1],
    a[1] * b[0] + a[3] * b[1],
    a[0] * b[2] + a[2] * b[3],
    a[1] * b[2] + a[3] * b[3],
    a[0] * b[4] + a[2] * b[5] + a[4],
    a[1] * b[4] + a[3] * b[5] + a[5],
  };
  memcpy(dst, r, sizeof r);
}

static JSValue c2d_translate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 2) return JS_UNDEFINED;
  double tx = 0, ty = 0;
  (void)JS_ToFloat64(ctx, &tx, argv[0]); (void)JS_ToFloat64(ctx, &ty, argv[1]);
  float t[6] = {1, 0, 0, 1, (float)tx, (float)ty};
  mat_mul(c->st.m, c->st.m, t);
  return JS_UNDEFINED;
}

static JSValue c2d_scale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 2) return JS_UNDEFINED;
  double sx = 1, sy = 1;
  (void)JS_ToFloat64(ctx, &sx, argv[0]); (void)JS_ToFloat64(ctx, &sy, argv[1]);
  float t[6] = {(float)sx, 0, 0, (float)sy, 0, 0};
  mat_mul(c->st.m, c->st.m, t);
  return JS_UNDEFINED;
}

static JSValue c2d_rotate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 1) return JS_UNDEFINED;
  double ang = 0;
  (void)JS_ToFloat64(ctx, &ang, argv[0]);
  float cs = (float)cos(ang), sn = (float)sin(ang);
  float t[6] = {cs, sn, -sn, cs, 0, 0};
  mat_mul(c->st.m, c->st.m, t);
  return JS_UNDEFINED;
}

static JSValue c2d_create_image_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  int32_t sw = 1, sh = 1;
  if (argc == 1 && JS_IsObject(argv[0])) {
    JSValue jw = JS_GetPropertyStr(ctx, argv[0], "width");
    JSValue jh = JS_GetPropertyStr(ctx, argv[0], "height");
    (void)JS_ToInt32(ctx, &sw, jw); (void)JS_ToInt32(ctx, &sh, jh);
    JS_FreeValue(ctx, jw); JS_FreeValue(ctx, jh);
  } else if (argc >= 2) {
    (void)JS_ToInt32(ctx, &sw, argv[0]);
    (void)JS_ToInt32(ctx, &sh, argv[1]);
  }
  if (sw < 1) sw = 1;
  if (sh < 1) sh = 1;
  if (sw > CANVAS_MAX_DIM) sw = CANVAS_MAX_DIM;
  if (sh > CANVAS_MAX_DIM) sh = CANVAS_MAX_DIM;
  size_t nbytes = (size_t)sw * (size_t)sh * 4u;
  uint8_t *tmp = ccord_calloc(1, nbytes);
  if (!tmp) return JS_ThrowOutOfMemory(ctx);
  JSValue ab = JS_NewArrayBufferCopy(ctx, tmp, nbytes);
  ccord_free(tmp);
  JSValue args[1] = {ab};
  JSValue u8c = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_UINT8C);
  JS_FreeValue(ctx, ab);
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, sw));
  JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, sh));
  JS_SetPropertyStr(ctx, obj, "data", u8c);
  return obj;
}

static JSValue c2d_transform(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c || argc < 6) return JS_UNDEFINED;
  float t[6];
  for (int i = 0; i < 6; i++) {
    double v = 0;
    (void)JS_ToFloat64(ctx, &v, argv[i]);
    t[i] = (float)v;
  }
  mat_mul(c->st.m, c->st.m, t);
  return JS_UNDEFINED;
}

static JSValue c2d_set_transform(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c) return JS_UNDEFINED;
  if (argc >= 6) {
    for (int i = 0; i < 6; i++) {
      double v = 0;
      (void)JS_ToFloat64(ctx, &v, argv[i]);
      c->st.m[i] = (float)v;
    }
  } else if (argc == 1 && JS_IsObject(argv[0])) {
    static const char *const k_f[6] = {"a", "b", "c", "d", "e", "f"};
    for (int i = 0; i < 6; i++) {
      JSValue p = JS_GetPropertyStr(ctx, argv[0], k_f[i]);
      double v = (i == 0 || i == 3) ? 1.0 : 0.0;
      if (!JS_IsUndefined(p)) (void)JS_ToFloat64(ctx, &v, p);
      JS_FreeValue(ctx, p);
      c->st.m[i] = (float)v;
    }
  } else {
    c->st.m[0] = 1; c->st.m[1] = 0; c->st.m[2] = 0; c->st.m[3] = 1; c->st.m[4] = 0; c->st.m[5] = 0;
  }
  return JS_UNDEFINED;
}

static JSValue c2d_reset_transform(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (c) { c->st.m[0] = 1; c->st.m[1] = 0; c->st.m[2] = 0; c->st.m[3] = 1; c->st.m[4] = 0; c->st.m[5] = 0; }
  return JS_UNDEFINED;
}

static JSValue c2d_get_transform(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  float m[6] = {1, 0, 0, 1, 0, 0};
  if (c) memcpy(m, c->st.m, sizeof m);
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "a", JS_NewFloat64(ctx, (double)m[0]));
  JS_SetPropertyStr(ctx, o, "b", JS_NewFloat64(ctx, (double)m[1]));
  JS_SetPropertyStr(ctx, o, "c", JS_NewFloat64(ctx, (double)m[2]));
  JS_SetPropertyStr(ctx, o, "d", JS_NewFloat64(ctx, (double)m[3]));
  JS_SetPropertyStr(ctx, o, "e", JS_NewFloat64(ctx, (double)m[4]));
  JS_SetPropertyStr(ctx, o, "f", JS_NewFloat64(ctx, (double)m[5]));
  JS_SetPropertyStr(ctx, o, "m11", JS_NewFloat64(ctx, (double)m[0]));
  JS_SetPropertyStr(ctx, o, "m12", JS_NewFloat64(ctx, (double)m[1]));
  JS_SetPropertyStr(ctx, o, "m21", JS_NewFloat64(ctx, (double)m[2]));
  JS_SetPropertyStr(ctx, o, "m22", JS_NewFloat64(ctx, (double)m[3]));
  JS_SetPropertyStr(ctx, o, "m41", JS_NewFloat64(ctx, (double)m[4]));
  JS_SetPropertyStr(ctx, o, "m42", JS_NewFloat64(ctx, (double)m[5]));
  return o;
}

static JSValue c2d_reset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_node *n = ctx2d_node(ctx, this_val);
  web_canvas *c = n ? n->canvas : nullptr;
  if (c && c->rgba) {
    memset(c->rgba, 0, (size_t)c->w * (size_t)c->h * 4u);
    c->stack_len = 0;
    c->path_len = 0;
    c->has_cur = false;
    state_init(&c->st);
    ctx2d_mark_dirty(ctx, n);
  }
  return JS_UNDEFINED;
}

static JSValue c2d_grad_add_stop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  if (argc >= 2) (void)JS_SetPropertyStr(ctx, this_val, "_color", JS_DupValue(ctx, argv[1]));
  return JS_UNDEFINED;
}

static JSValue c2d_create_gradient(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val; (void)argc; (void)argv;
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "addColorStop", JS_NewCFunction(ctx, c2d_grad_add_stop, "addColorStop", 2));
  return o;
}

static JSValue c2d_noop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)ctx; (void)this_val; (void)argc; (void)argv;
  return JS_UNDEFINED;
}

static JSValue c2d_create_pattern(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val; (void)argc; (void)argv;
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "setTransform", JS_NewCFunction(ctx, c2d_noop, "setTransform", 1));
  return o;
}

static JSValue c2d_set_line_dash(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  if (argc > 0 && JS_IsArray(argv[0])) (void)JS_SetPropertyStr(ctx, this_val, "_dash", JS_DupValue(ctx, argv[0]));
  return JS_UNDEFINED;
}

static JSValue c2d_get_line_dash(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  JSValue d = JS_GetPropertyStr(ctx, this_val, "_dash");
  if (JS_IsArray(d)) return d;
  JS_FreeValue(ctx, d);
  return JS_NewArray(ctx);
}

static JSValue c2d_get_prop(JSContext *ctx, JSValueConst this_val, int magic)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c) return JS_UNDEFINED;
  switch (magic) {
    case 0: return JS_NewString(ctx, c->st.fill_str);
    case 1: return JS_NewString(ctx, c->st.stroke_str);
    case 2: return JS_NewFloat64(ctx, (double)c->st.line_width);
    case 3: return JS_NewFloat64(ctx, (double)c->st.global_alpha);
    case 4: return JS_NewString(ctx, c->st.font_str);
    case 5: return JS_NewString(ctx, c->st.text_align == 1 ? "center" : (c->st.text_align == 2 ? "right" : "left"));
    case 6: return JS_NewString(ctx, c->st.text_baseline == 1 ? "top" : (c->st.text_baseline == 2 ? "middle" : (c->st.text_baseline == 3 ? "bottom" : "alphabetic")));
    default: return JS_UNDEFINED;
  }
}

static JSValue c2d_set_prop(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
  web_canvas *c = ctx2d_canvas(ctx, this_val);
  if (!c) return JS_UNDEFINED;
  if (magic == 0 || magic == 1 || magic == 4 || magic == 5 || magic == 6) {
    JSValue actual = JS_DupValue(ctx, val);
    if ((magic == 0 || magic == 1) && JS_IsObject(val)) {
      JSValue gc = JS_GetPropertyStr(ctx, val, "_color");
      JS_FreeValue(ctx, actual);
      if (!JS_IsString(gc)) { JS_FreeValue(ctx, gc); return JS_UNDEFINED; }
      actual = gc;
    }
    const char *s = JS_ToCString(ctx, actual);
    JS_FreeValue(ctx, actual);
    if (!s) return JS_EXCEPTION;
    if (magic == 0) {
      snprintf(c->st.fill_str, sizeof c->st.fill_str, "%s", s);
      (void)web_css_parse_color(s, &c->st.fill);
    } else if (magic == 1) {
      snprintf(c->st.stroke_str, sizeof c->st.stroke_str, "%s", s);
      (void)web_css_parse_color(s, &c->st.stroke);
    } else if (magic == 4) {
      snprintf(c->st.font_str, sizeof c->st.font_str, "%s", s);
      for (const char *p = s; *p; p++) {
        if (isdigit((unsigned char)*p)) {
          float px = (float)strtod(p, nullptr);
          if (px >= 4.0f && px <= 200.0f) c->st.font_px = px;
          break;
        }
      }
    } else if (magic == 5) {
      if (strcmp(s, "center") == 0) c->st.text_align = 1;
      else if (strcmp(s, "right") == 0 || strcmp(s, "end") == 0) c->st.text_align = 2;
      else c->st.text_align = 0;
    } else if (magic == 6) {
      if (strcmp(s, "top") == 0 || strcmp(s, "hanging") == 0) c->st.text_baseline = 1;
      else if (strcmp(s, "middle") == 0) c->st.text_baseline = 2;
      else if (strcmp(s, "bottom") == 0 || strcmp(s, "ideographic") == 0) c->st.text_baseline = 3;
      else c->st.text_baseline = 0;
    }
    JS_FreeCString(ctx, s);
  } else if (magic == 2) {
    double v = 1.0;
    if (JS_ToFloat64(ctx, &v, val) == 0 && v > 0.0) c->st.line_width = (float)v;
  } else if (magic == 3) {
    double v = 1.0;
    if (JS_ToFloat64(ctx, &v, val) == 0 && v >= 0.0 && v <= 1.0) c->st.global_alpha = (float)v;
  }
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry k_c2d_proto[] = {
  JS_CFUNC_DEF("clearRect", 4, c2d_clear_rect),
  JS_CFUNC_DEF("fillRect", 4, c2d_fill_rect),
  JS_CFUNC_DEF("strokeRect", 4, c2d_stroke_rect),
  JS_CFUNC_DEF("beginPath", 0, c2d_begin_path),
  JS_CFUNC_DEF("closePath", 0, c2d_close_path),
  JS_CFUNC_DEF("moveTo", 2, c2d_move_to),
  JS_CFUNC_DEF("lineTo", 2, c2d_line_to),
  JS_CFUNC_DEF("bezierCurveTo", 6, c2d_bezier_to),
  JS_CFUNC_DEF("quadraticCurveTo", 4, c2d_quad_to),
  JS_CFUNC_DEF("arcTo", 5, c2d_arc_to),
  JS_CFUNC_DEF("rect", 4, c2d_rect),
  JS_CFUNC_DEF("roundRect", 5, c2d_round_rect),
  JS_CFUNC_DEF("arc", 6, c2d_arc),
  JS_CFUNC_DEF("ellipse", 8, c2d_ellipse),
  JS_CFUNC_DEF("fill", 1, c2d_fill),
  JS_CFUNC_DEF("stroke", 1, c2d_stroke),
  JS_CFUNC_DEF("isPointInPath", 3, c2d_is_point_in_path),
  JS_CFUNC_DEF("isPointInStroke", 3, c2d_is_point_in_stroke),
  JS_CFUNC_DEF("fillText", 3, c2d_fill_text),
  JS_CFUNC_DEF("strokeText", 3, c2d_fill_text),
  JS_CFUNC_DEF("measureText", 1, c2d_measure_text),
  JS_CFUNC_DEF("drawImage", 9, c2d_draw_image),
  JS_CFUNC_DEF("createImageData", 2, c2d_create_image_data),
  JS_CFUNC_DEF("getImageData", 4, c2d_get_image_data),
  JS_CFUNC_DEF("putImageData", 3, c2d_put_image_data),
  JS_CFUNC_DEF("createLinearGradient", 4, c2d_create_gradient),
  JS_CFUNC_DEF("createRadialGradient", 6, c2d_create_gradient),
  JS_CFUNC_DEF("createConicGradient", 3, c2d_create_gradient),
  JS_CFUNC_DEF("createPattern", 2, c2d_create_pattern),
  JS_CFUNC_DEF("setLineDash", 1, c2d_set_line_dash),
  JS_CFUNC_DEF("getLineDash", 0, c2d_get_line_dash),
  JS_CFUNC_DEF("save", 0, c2d_save),
  JS_CFUNC_DEF("restore", 0, c2d_restore),
  JS_CFUNC_DEF("translate", 2, c2d_translate),
  JS_CFUNC_DEF("scale", 2, c2d_scale),
  JS_CFUNC_DEF("rotate", 1, c2d_rotate),
  JS_CFUNC_DEF("transform", 6, c2d_transform),
  JS_CFUNC_DEF("setTransform", 6, c2d_set_transform),
  JS_CFUNC_DEF("resetTransform", 0, c2d_reset_transform),
  JS_CFUNC_DEF("getTransform", 0, c2d_get_transform),
  JS_CFUNC_DEF("reset", 0, c2d_reset),
  JS_CFUNC_DEF("clip", 1, c2d_noop),
  JS_CFUNC_DEF("drawFocusIfNeeded", 1, c2d_noop),
  JS_CGETSET_MAGIC_DEF("fillStyle", c2d_get_prop, c2d_set_prop, 0),
  JS_CGETSET_MAGIC_DEF("strokeStyle", c2d_get_prop, c2d_set_prop, 1),
  JS_CGETSET_MAGIC_DEF("lineWidth", c2d_get_prop, c2d_set_prop, 2),
  JS_CGETSET_MAGIC_DEF("globalAlpha", c2d_get_prop, c2d_set_prop, 3),
  JS_CGETSET_MAGIC_DEF("font", c2d_get_prop, c2d_set_prop, 4),
  JS_CGETSET_MAGIC_DEF("textAlign", c2d_get_prop, c2d_set_prop, 5),
  JS_CGETSET_MAGIC_DEF("textBaseline", c2d_get_prop, c2d_set_prop, 6),
};

void web_canvas_register_classes(ccord_web_view *v)
{
  JS_NewClassID(v->rt, &v->cls_canvas_ctx);
  JSClassDef cd = {.class_name = "CanvasRenderingContext2D"};
  JS_NewClass(v->rt, v->cls_canvas_ctx, &cd);
}

JSValue web_canvas_get_context_2d(web_frame *f, web_node *node)
{
  if (!f || !node) return JS_NULL;
  if (!node->canvas) {
    int w = 300, h = 150;
    const char *ws = web_node_get_attr(node, "width");
    const char *hs = web_node_get_attr(node, "height");
    if (ws && atoi(ws) > 0) w = atoi(ws);
    if (hs && atoi(hs) > 0) h = atoi(hs);
    node->canvas = web_canvas_create(w, h);
    if (!node->canvas) return JS_NULL;
  }
  JSValue obj = JS_NewObjectClass(f->ctx, f->view->cls_canvas_ctx);
  JS_SetOpaque(obj, node);
  JS_SetPropertyFunctionList(f->ctx, obj, k_c2d_proto, (int)(sizeof k_c2d_proto / sizeof k_c2d_proto[0]));
  JS_SetPropertyStr(f->ctx, obj, "canvas", web_node_wrap(f, node));
  JS_SetPropertyStr(f->ctx, obj, "globalCompositeOperation", JS_NewString(f->ctx, "source-over"));
  JS_SetPropertyStr(f->ctx, obj, "lineCap", JS_NewString(f->ctx, "butt"));
  JS_SetPropertyStr(f->ctx, obj, "lineJoin", JS_NewString(f->ctx, "miter"));
  JS_SetPropertyStr(f->ctx, obj, "miterLimit", JS_NewFloat64(f->ctx, 10.0));
  JS_SetPropertyStr(f->ctx, obj, "lineDashOffset", JS_NewFloat64(f->ctx, 0.0));
  JS_SetPropertyStr(f->ctx, obj, "shadowBlur", JS_NewFloat64(f->ctx, 0.0));
  JS_SetPropertyStr(f->ctx, obj, "shadowColor", JS_NewString(f->ctx, "rgba(0, 0, 0, 0)"));
  JS_SetPropertyStr(f->ctx, obj, "shadowOffsetX", JS_NewFloat64(f->ctx, 0.0));
  JS_SetPropertyStr(f->ctx, obj, "shadowOffsetY", JS_NewFloat64(f->ctx, 0.0));
  JS_SetPropertyStr(f->ctx, obj, "imageSmoothingEnabled", JS_TRUE);
  JS_SetPropertyStr(f->ctx, obj, "imageSmoothingQuality", JS_NewString(f->ctx, "low"));
  JS_SetPropertyStr(f->ctx, obj, "filter", JS_NewString(f->ctx, "none"));
  JS_SetPropertyStr(f->ctx, obj, "direction", JS_NewString(f->ctx, "ltr"));
  return obj;
}
