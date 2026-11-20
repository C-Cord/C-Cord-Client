// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Headless renderer test: text metrics, caret/hit-test consistency, primitives and a screenshot with pixel asserts.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <jpeglib.h>
#include <png.h>
#include <webp/encode.h>

#include <ccord/core/alloc.h>
#include <ccord/core/paths.h>
#include <ccord/render/render.h>

#include "ccord_test.h"

static bool pixel_is(SDL_Surface *s, int x, int y, ccord_color want, int tol)
{
  uint8_t r, g, b, a;
  if (!SDL_ReadSurfacePixel(s, x, y, &r, &g, &b, &a)) return false;
  return abs(r - want.r) <= tol && abs(g - want.g) <= tol && abs(b - want.b) <= tol;
}

static void test_text(ccord_renderer *r)
{
  ccord_text_style st = {CCORD_FONT_REGULAR, 16.0f, {255, 255, 255, 255}};
  ccord_vec2 a = ccord_text_measure(r, "Hello", 5, &st, 0);
  ccord_vec2 b = ccord_text_measure(r, "Hello world", 11, &st, 0);
  CT_CHECK(a.x > 10.0f && b.x > a.x && a.y >= 16.0f && a.y < 40.0f);
  // Wrapping: narrow width makes the text taller, never wider than the width.
  const char *long_text = "alpha beta gamma delta epsilon zeta eta theta";
  ccord_vec2 wide = ccord_text_measure(r, long_text, strlen(long_text), &st, 0);
  ccord_vec2 wrapped = ccord_text_measure(r, long_text, strlen(long_text), &st, 120.0f);
  CT_CHECK(wrapped.y > wide.y * 1.5f);
  CT_CHECK(wrapped.x <= 121.0f);
  // A single word longer than the wrap width must not produce a wider layout than the width allows.
  ccord_vec2 word = ccord_text_measure(r, "Supercalifragilisticexpialidocious", 34, &st, 60.0f);
  CT_CHECK(word.x <= 61.0f);
  // Bold is wider than regular.
  ccord_text_style bold = st;
  bold.font = CCORD_FONT_BOLD;
  CT_CHECK(ccord_text_measure(r, "Hello world", 11, &bold, 0).x > b.x);
  // Empty text still has a line height.
  CT_CHECK(ccord_text_measure(r, "", 0, &st, 0).y > 10.0f);
  // Security regression (P14): BiDi override/isolate controls (RLO U+202E, LRI U+2066, PDI U+2069) are stripped.
  {
    const char *clean = "hello.txt";
    const char *bidi = "\xE2\x80\xAEhello\xE2\x81\xA6.txt\xE2\x81\xA9";
    ccord_vec2 wc = ccord_text_measure(r, clean, strlen(clean), &st, 0);
    ccord_vec2 wb = ccord_text_measure(r, bidi, strlen(bidi), &st, 0);
    CT_CHECK(fabsf(wc.x - wb.x) < 0.5f && fabsf(wc.y - wb.y) < 0.5f);
  }
  // Caret/hit-test round trip on every code point boundary of a multibyte string.
  const char *s = "ab\xC3\xA9" "cd";
  size_t len = strlen(s);
  size_t bounds[] = {0, 1, 2, 4, 5, 6};
  for (size_t i = 0; i < sizeof bounds / sizeof bounds[0]; i++) {
    ccord_rect c = ccord_text_caret_rect(r, s, len, &st, 0, bounds[i]);
    size_t hit = ccord_text_hit_test(r, s, len, &st, 0, c.x + 0.25f, c.y + c.h * 0.5f);
    CT_CHECK(hit == bounds[i]);
  }
  // Caret after a trailing newline moves to the next line.
  const char *nl = "one\n";
  ccord_rect top = ccord_text_caret_rect(r, nl, 4, &st, 0, 0);
  ccord_rect after = ccord_text_caret_rect(r, nl, 4, &st, 0, 4);
  CT_CHECK(after.y > top.y + 8.0f && after.x < 1.0f);
  // Trailing spaces advance the caret and round-trip through hit-testing (even before non-space text arrives).
  static const char *const k_space_cases[] = {"   ", "a   ", "a   \nb", "hello\n   "};
  for (size_t ci = 0; ci < sizeof k_space_cases / sizeof k_space_cases[0]; ci++) {
    const char *sp = k_space_cases[ci];
    size_t splen = strlen(sp);
    float prev_x = -1.0f, prev_y = -1.0f;
    for (size_t pos = 0; pos <= splen; pos++) {
      ccord_rect c = ccord_text_caret_rect(r, sp, splen, &st, 0, pos);
      if (pos > 0 && sp[pos - 1] != '\n') CT_CHECK(c.x > prev_x + 1.0f && fabsf(c.y - prev_y) < 0.5f);
      size_t hit = ccord_text_hit_test(r, sp, splen, &st, 0, c.x + 0.25f, c.y + c.h * 0.5f);
      CT_CHECK(hit == pos);
      prev_x = c.x;
      prev_y = c.y;
    }
  }
  // Ellipsis never exceeds the width.
  ccord_renderer *rr = r;
  float w = ccord_text_draw_ellipsis(rr, long_text, strlen(long_text), 0, 0, 100.0f, &st);
  CT_CHECK(w > 40.0f && w <= 100.5f);
  CT_CHECK(ccord_text_draw_ellipsis(rr, "hi", 2, 0, 0, 100.0f, &st) < 100.0f);
  // Measuring the same text twice hits the cache.
  uint64_t m0 = ccord_text_cache_misses(r);
  (void)ccord_text_measure(r, long_text, strlen(long_text), &st, 120.0f);
  CT_CHECK(ccord_text_cache_misses(r) == m0);
}

static void test_screenshot(ccord_renderer *r)
{
  ccord_color bg = {0x31, 0x33, 0x38, 255}, red = {255, 0, 0, 255}, blue = {0, 0, 255, 255}, green = {0, 200, 0, 255};
  ccord_render_begin(r, bg);
  ccord_draw_rect(r, (ccord_rect){10, 10, 50, 50}, red);
  ccord_draw_rrect(r, (ccord_rect){100, 10, 80, 40}, 12, blue);
  ccord_draw_circle(r, 240, 30, 20, green);
  ccord_draw_line(r, (ccord_vec2){10, 100}, (ccord_vec2){200, 100}, 4, red);
  ccord_push_clip(r, (ccord_rect){300, 0, 20, 100});
  ccord_draw_rect(r, (ccord_rect){290, 0, 100, 100}, red);   // only x in [300,320) may show
  ccord_pop_clip(r);
  ccord_text_style st = {CCORD_FONT_REGULAR, 20.0f, {255, 255, 255, 255}};
  ccord_text_draw(r, "Hello", 5, 10, 140, &st, 0);
  const char *path = "test_render_shot.png";
  ccord_error err;
  CT_CHECK(ccord_render_screenshot(r, path, &err) == CCORD_OK);
  ccord_render_end(r);
  SDL_Surface *s = SDL_LoadPNG(path);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(s->w == 800 && s->h == 600);
  CT_CHECK(pixel_is(s, 5, 5, bg, 1));
  CT_CHECK(pixel_is(s, 30, 30, red, 1));
  CT_CHECK(pixel_is(s, 140, 30, blue, 1));
  CT_CHECK(pixel_is(s, 102, 12, bg, 2));    // outside the rounded corner stays background
  CT_CHECK(pixel_is(s, 240, 30, green, 1));
  CT_CHECK(pixel_is(s, 100, 100, red, 1));
  CT_CHECK(pixel_is(s, 310, 50, red, 1));
  CT_CHECK(pixel_is(s, 295, 50, bg, 1));
  CT_CHECK(pixel_is(s, 325, 50, bg, 1));
  int lit = 0;   // text pixels exist
  for (int y = 140; y < 165; y++) {
    for (int x = 10; x < 60; x++) {
      uint8_t pr, pg, pb, pa;
      SDL_ReadSurfacePixel(s, x, y, &pr, &pg, &pb, &pa);
      if (pr > 200 && pg > 200 && pb > 200) lit++;
    }
  }
  CT_CHECK(lit > 20);
  SDL_DestroySurface(s);
  remove(path);
}

static uint8_t *make_solid_png(int w, int h, ccord_color c, size_t *out_len)
{
  *out_len = 0;
  size_t px_bytes = (size_t)w * (size_t)h * 4u;
  uint8_t *rgba = ccord_malloc(px_bytes);
  if (!rgba) return nullptr;
  for (int i = 0; i < w * h; i++) {
    rgba[i * 4 + 0] = c.r;
    rgba[i * 4 + 1] = c.g;
    rgba[i * 4 + 2] = c.b;
    rgba[i * 4 + 3] = c.a;
  }
  png_image img;
  memset(&img, 0, sizeof img);
  img.version = PNG_IMAGE_VERSION;
  img.width = (png_uint_32)w;
  img.height = (png_uint_32)h;
  img.format = PNG_FORMAT_RGBA;
  png_alloc_size_t mem_bytes = 0;
  (void)png_image_write_to_memory(&img, nullptr, &mem_bytes, 0, rgba, 0, nullptr);
  uint8_t *out = mem_bytes > 0 ? ccord_malloc(mem_bytes) : nullptr;
  if (out && png_image_write_to_memory(&img, out, &mem_bytes, 0, rgba, 0, nullptr)) {
    *out_len = (size_t)mem_bytes;
  } else {
    ccord_free(out);
    out = nullptr;
  }
  png_image_free(&img);
  ccord_free(rgba);
  return out;
}

static uint8_t *make_solid_jpeg(int w, int h, ccord_color c, size_t *out_len)
{
  *out_len = 0;
  uint8_t *rgb = ccord_malloc((size_t)w * 3u);
  if (!rgb) return nullptr;
  for (int x = 0; x < w; x++) {
    rgb[x * 3 + 0] = c.r;
    rgb[x * 3 + 1] = c.g;
    rgb[x * 3 + 2] = c.b;
  }
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  unsigned char *mem = nullptr;
  unsigned long mem_sz = 0;
  jpeg_mem_dest(&cinfo, &mem, &mem_sz);
  cinfo.image_width = (JDIMENSION)w;
  cinfo.image_height = (JDIMENSION)h;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 100, TRUE);
  jpeg_start_compress(&cinfo, TRUE);
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row[1] = {rgb};
    (void)jpeg_write_scanlines(&cinfo, row, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  ccord_free(rgb);
  uint8_t *copy = nullptr;
  if (mem && mem_sz > 0) {
    copy = ccord_malloc((size_t)mem_sz);
    if (copy) {
      memcpy(copy, mem, (size_t)mem_sz);
      *out_len = (size_t)mem_sz;
    }
  }
  free(mem);
  return copy;
}

static uint8_t *make_solid_webp(int w, int h, ccord_color c, size_t *out_len)
{
  *out_len = 0;
  uint8_t *rgba = ccord_malloc((size_t)w * (size_t)h * 4u);
  if (!rgba) return nullptr;
  for (int i = 0; i < w * h; i++) {
    rgba[i * 4 + 0] = c.r;
    rgba[i * 4 + 1] = c.g;
    rgba[i * 4 + 2] = c.b;
    rgba[i * 4 + 3] = c.a;
  }
  uint8_t *webp = nullptr;
  size_t sz = WebPEncodeLosslessRGBA(rgba, w, h, w * 4, &webp);
  ccord_free(rgba);
  uint8_t *copy = nullptr;
  if (webp && sz > 0) {
    copy = ccord_malloc(sz);
    if (copy) {
      memcpy(copy, webp, sz);
      *out_len = sz;
    }
  }
  WebPFree(webp);
  return copy;
}

// Group effects: an alpha makes everything drawn under it (shapes, text, nested pushes) that much fainter and an offset
// slides it; both come back on pop, an unbalanced pop does nothing, and a new frame always starts clean.
static void test_group_effects(ccord_renderer *r)
{
  ccord_color bg = {0, 0, 0, 255}, white = {255, 255, 255, 255};
  ccord_render_begin(r, bg);
  ccord_push_alpha(r, 0.5f);
  ccord_draw_rect(r, (ccord_rect){10, 10, 20, 20}, white);              // half
  ccord_push_alpha(r, 0.5f);
  ccord_draw_rect(r, (ccord_rect){40, 10, 20, 20}, white);              // a quarter
  ccord_draw_circle(r, 90, 20, 8, white);
  ccord_pop_alpha(r);
  ccord_draw_rrect(r, (ccord_rect){110, 10, 20, 20}, 4, white);         // half again after the inner pop
  ccord_pop_alpha(r);
  ccord_draw_rect(r, (ccord_rect){140, 10, 20, 20}, white);             // untouched
  ccord_pop_alpha(r);                                                   // nothing to pop: harmless
  ccord_draw_rect(r, (ccord_rect){170, 10, 20, 20}, white);
  ccord_push_offset(r, 30.0f, 50.0f);
  ccord_draw_rect(r, (ccord_rect){10, 10, 20, 20}, white);              // lands at (40, 60)
  ccord_push_offset(r, 0.0f, 40.0f);
  ccord_draw_rect(r, (ccord_rect){10, 10, 20, 20}, white);              // (40, 100)
  ccord_pop_offset(r);
  ccord_draw_line(r, (ccord_vec2){100, 70}, (ccord_vec2){160, 70}, 4, white);   // shifted too: y 120
  ccord_pop_offset(r);
  ccord_draw_rect(r, (ccord_rect){10, 200, 20, 20}, white);
  ccord_push_offset(r, 0.0f, 150.0f);                                   // clips move with the offset
  ccord_push_clip(r, (ccord_rect){300, 0, 20, 100});
  ccord_draw_rect(r, (ccord_rect){290, 0, 100, 100}, white);            // shows at y 150..250, x 300..320
  ccord_pop_clip(r);
  ccord_pop_offset(r);
  ccord_push_alpha(r, 0.0f);
  ccord_draw_rect(r, (ccord_rect){400, 10, 20, 20}, white);             // fully faded: not there
  ccord_pop_alpha(r);
  ccord_text_style st = {CCORD_FONT_REGULAR, 20.0f, white};
  ccord_push_alpha(r, 0.25f);
  ccord_text_draw(r, "Faded", 5, 10, 300, &st, 0);
  ccord_pop_alpha(r);
  ccord_text_draw(r, "Solid", 5, 10, 340, &st, 0);
  ccord_push_alpha(r, 0.5f);                                            // deliberately left pushed
  ccord_push_offset(r, 99.0f, 99.0f);
  const char *path = "test_render_fx.png";
  ccord_error err;
  CT_CHECK(ccord_render_screenshot(r, path, &err) == CCORD_OK);
  ccord_render_end(r);
  SDL_Surface *s = SDL_LoadPNG(path);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(pixel_is(s, 20, 20, (ccord_color){128, 128, 128, 255}, 2));
  CT_CHECK(pixel_is(s, 50, 20, (ccord_color){64, 64, 64, 255}, 2));
  CT_CHECK(pixel_is(s, 90, 20, (ccord_color){64, 64, 64, 255}, 2));
  CT_CHECK(pixel_is(s, 120, 20, (ccord_color){128, 128, 128, 255}, 2));
  CT_CHECK(pixel_is(s, 150, 20, white, 1) && pixel_is(s, 180, 20, white, 1));
  CT_CHECK(pixel_is(s, 50, 70, white, 1) && pixel_is(s, 20, 20, (ccord_color){128, 128, 128, 255}, 2));
  CT_CHECK(pixel_is(s, 50, 110, white, 1) && pixel_is(s, 50, 130, bg, 1));
  CT_CHECK(pixel_is(s, 130, 120, white, 1) && pixel_is(s, 130, 70, bg, 1));
  CT_CHECK(pixel_is(s, 20, 210, white, 1));                             // back to normal after the pops
  CT_CHECK(pixel_is(s, 310, 200, white, 1) && pixel_is(s, 310, 50, bg, 1) && pixel_is(s, 295, 200, bg, 1) &&
           pixel_is(s, 325, 200, bg, 1) && pixel_is(s, 310, 260, bg, 1));
  CT_CHECK(pixel_is(s, 410, 20, bg, 1));
  int faded = 0, solid = 0, bright = 0;
  for (int y = 300; y < 330; y++) {
    for (int x = 10; x < 90; x++) {
      uint8_t pr, pg, pb, pa;
      SDL_ReadSurfacePixel(s, x, y, &pr, &pg, &pb, &pa);
      if (pr > 20) faded++;
      if (pr > 90) bright++;
    }
  }
  for (int y = 340; y < 370; y++) {
    for (int x = 10; x < 90; x++) {
      uint8_t pr, pg, pb, pa;
      SDL_ReadSurfacePixel(s, x, y, &pr, &pg, &pb, &pa);
      if (pr > 200) solid++;
    }
  }
  CT_CHECK(faded > 20 && bright == 0 && solid > 20);                    // the faded text exists but never gets bright
  SDL_DestroySurface(s);
  remove(path);

  // The next frame starts clean even though the last one ended with a push left open.
  ccord_render_begin(r, bg);
  ccord_draw_rect(r, (ccord_rect){10, 10, 20, 20}, white);
  CT_CHECK(ccord_render_screenshot(r, path, &err) == CCORD_OK);
  ccord_render_end(r);
  s = SDL_LoadPNG(path);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(pixel_is(s, 20, 20, white, 1));
  SDL_DestroySurface(s);
  remove(path);
}

static void test_fallback_fonts_and_textures(ccord_renderer *r)
{
  // Fallback fonts (emoji + CJK) are attached via TTF_AddFallbackFont.
  CT_CHECK(ccord_render_fallback_font_count(r) >= 2);
  // Bundled subsets come first: index 1 must be the SC face of the bundled single-face CJK subset
  // (the host NotoSansCJK TTC would open face 0 = JP, so "SC" proves the bundled file was used).
  const char *fam0 = ccord_render_fallback_font_family(r, 0);
  const char *fam1 = ccord_render_fallback_font_family(r, 1);
  CT_CHECK(fam0 && strcmp(fam0, "Twemoji") == 0);
  CT_CHECK(fam1 && strcmp(fam1, "Noto Sans CJK SC") == 0);
  CT_CHECK(ccord_render_fallback_font_family(r, -1) == nullptr);
  CT_CHECK(ccord_render_fallback_font_family(r, ccord_render_fallback_font_count(r)) == nullptr);
  CT_CHECK(ccord_render_fallback_font_family(nullptr, 0) == nullptr);
  ccord_text_style st = {CCORD_FONT_REGULAR, 20.0f, {255, 255, 255, 255}};
  const char *emoji_cjk = "\xF0\x9F\x98\x80 \xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x91\x8D";
  ccord_vec2 m = ccord_text_measure(r, emoji_cjk, strlen(emoji_cjk), &st, 0.0f);
  CT_CHECK(m.x > 30.0f && m.y >= 18.0f && m.y < 50.0f);
  ccord_vec2 single_em = ccord_text_measure(r, "\xF0\x9F\x91\x8D", 4, &st, 0.0f);
  CT_CHECK(single_em.x > 10.0f && single_em.x < 40.0f && single_em.y >= 18.0f && single_em.y < 40.0f);
  // Direct Twemoji font style (CCORD_FONT_EMOJI): ZWJ, dark skin tone (U+1F3FF), keycaps and jumbo sizes shape as 1 em.
  ccord_text_style em_st = {CCORD_FONT_EMOJI, 20.0f, {255, 255, 255, 255}};
  ccord_vec2 zwj_m = ccord_text_measure(r, "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x92\xBB", 11, &em_st, 0.0f);
  CT_CHECK(zwj_m.x >= 18.0f && zwj_m.x <= 24.0f && zwj_m.y >= 18.0f && zwj_m.y <= 24.0f);
  ccord_vec2 dark_m = ccord_text_measure(r, "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBF", 8, &em_st, 0.0f);
  CT_CHECK(dark_m.x >= 18.0f && dark_m.x <= 24.0f && dark_m.y >= 18.0f && dark_m.y <= 24.0f);
  ccord_vec2 keycap_m = ccord_text_measure(r, "1\xEF\xB8\x8F\xE2\x83\xA3", 7, &em_st, 0.0f);
  CT_CHECK(keycap_m.x >= 18.0f && keycap_m.x <= 24.0f && keycap_m.y >= 18.0f && keycap_m.y <= 24.0f);
  ccord_text_style jumbo_st = {CCORD_FONT_EMOJI, 48.0f, {255, 255, 255, 255}};
  ccord_vec2 jumbo_m = ccord_text_measure(r, "\xF0\x9F\x98\x80", 4, &jumbo_st, 0.0f);
  CT_CHECK(jumbo_m.x >= 44.0f && jumbo_m.x <= 52.0f && jumbo_m.y >= 44.0f && jumbo_m.y <= 52.0f);

  // Startup opens no system font: text the bundled fonts cover (Latin, Cyrillic, emoji, GB2312) attaches none.
  CT_CHECK(ccord_render_system_font_count(r) == 0);
  CT_CHECK(ccord_text_glyph_kind(r, &st, 0x41) == CCORD_GLYPH_REAL);
  CT_CHECK(ccord_text_glyph_kind(r, &st, 0x416) == CCORD_GLYPH_REAL);
  CT_CHECK(ccord_text_glyph_kind(r, &st, 0x1F600) == CCORD_GLYPH_REAL);
  CT_CHECK(ccord_text_glyph_kind(r, &st, 0x4E2D) == CCORD_GLYPH_REAL);
  CT_CHECK(ccord_render_system_font_count(r) == 0);
  // Controls and invisible formatting characters between letters take no width and draw no box.
  const float plain_w = ccord_text_measure(r, "abc", 3, &st, 0.0f).x;
  static const char *const k_invisible[] = {"ab\x1b" "c", "ab\xE2\x80\x8B" "c", "ab\xE2\x80\x8D" "c", "ab\xEF\xB8\x8F" "c", "ab\xC2\xAD" "c", "ab\x7f" "c", "ab\xC2\x85" "c"};
  for (size_t i = 0; i < sizeof k_invisible / sizeof k_invisible[0]; i++) {
    CT_CHECK(fabsf(ccord_text_measure(r, k_invisible[i], strlen(k_invisible[i]), &st, 0.0f).x - plain_w) < 0.01f);
  }
  CT_CHECK(ccord_text_glyph_kind(r, &st, 0x1B) == CCORD_GLYPH_NONE);
  CT_CHECK(ccord_text_glyph_kind(r, &st, 0x200B) == CCORD_GLYPH_NONE);
  // Offsets stay on the original text when a control is cut: the caret sits at the same x before and after it,
  // and a click in the right half of 'c' lands after the 'c' in the original bytes.
  const char *cut = "ab\x1b" "cd";
  CT_CHECK(fabsf(ccord_text_caret_rect(r, cut, 5, &st, 0.0f, 2).x - ccord_text_caret_rect(r, cut, 5, &st, 0.0f, 3).x) < 0.01f);
  CT_CHECK(ccord_text_hit_test(r, cut, 5, &st, 0.0f, plain_w - 1.0f, 8.0f) == 4);
  // A Hangul Jamo letter is outside the bundled subsets: where an installed font has it, the first use attaches that font.
  if (ccord_text_glyph_kind(r, &st, 0x1100) == CCORD_GLYPH_REAL) CT_CHECK(ccord_render_system_font_count(r) >= 1);
  ccord_render_set_glyph_fixes(r, false);
  CT_CHECK(ccord_text_glyph_kind(r, &st, 0x1B) == CCORD_GLYPH_NOTDEF);
  ccord_render_set_glyph_fixes(r, true);
  CT_CHECK(ccord_text_glyph_kind(r, &st, 0x1B) == CCORD_GLYPH_NONE);

  // Decode PNG, JPEG, WebP and upload into LRU texture cache.
  ccord_color red = {240, 30, 40, 255}, green = {30, 220, 50, 255}, blue = {40, 70, 240, 255};
  size_t png_len = 0, jpg_len = 0, webp_len = 0;
  uint8_t *png_bytes = make_solid_png(32, 32, red, &png_len);
  uint8_t *jpg_bytes = make_solid_jpeg(32, 32, green, &jpg_len);
  uint8_t *webp_bytes = make_solid_webp(32, 32, blue, &webp_len);
  CT_CHECK(png_bytes && png_len > 0);
  CT_CHECK(jpg_bytes && jpg_len > 0);
  CT_CHECK(webp_bytes && webp_len > 0);

  ccord_error err;
  uint8_t *dec = nullptr;
  int dw = 0, dh = 0;
  CT_CHECK(ccord_image_decode_rgba(png_bytes, 3, &dec, &dw, &dh, &err) != CCORD_OK);
  CT_CHECK(ccord_image_decode_rgba(png_bytes, png_len, &dec, &dw, &dh, &err) == CCORD_OK);
  CT_CHECK(dec && dw == 32 && dh == 32 && dec[0] == red.r && dec[1] == red.g && dec[2] == red.b && dec[3] == 255);
  ccord_free(dec);

  CT_CHECK(ccord_image_decode_rgba(jpg_bytes, jpg_len, &dec, &dw, &dh, &err) == CCORD_OK);
  CT_CHECK(dec && dw == 32 && dh == 32 && abs((int)dec[0] - (int)green.r) <= 4 && abs((int)dec[1] - (int)green.g) <= 4);
  ccord_free(dec);

  CT_CHECK(ccord_image_decode_rgba(webp_bytes, webp_len, &dec, &dw, &dh, &err) == CCORD_OK);
  CT_CHECK(dec && dw == 32 && dh == 32 && dec[0] == blue.r && dec[1] == blue.g && dec[2] == blue.b && dec[3] == 255);
  ccord_free(dec);

  // Test LRU eviction in the texture cache.
  ccord_render_set_texture_cache_cap(r, 3);
  int tw = 0, th = 0;
  CT_CHECK(ccord_render_upload_image(r, "img:png", png_bytes, png_len, &tw, &th, &err) == CCORD_OK && tw == 32 && th == 32);
  CT_CHECK(ccord_render_upload_image(r, "img:jpg", jpg_bytes, jpg_len, &tw, &th, &err) == CCORD_OK && tw == 32 && th == 32);
  CT_CHECK(ccord_render_upload_image(r, "img:webp", webp_bytes, webp_len, &tw, &th, &err) == CCORD_OK && tw == 32 && th == 32);
  CT_CHECK(ccord_render_texture_cache_count(r) == 3);
  // Touch "img:png" so "img:jpg" becomes the least-recently-used entry.
  CT_CHECK(ccord_render_has_texture(r, "img:png", &tw, &th));
  uint8_t raw_yellow[16 * 16 * 4];
  for (int i = 0; i < 16 * 16; i++) {
    raw_yellow[i * 4 + 0] = 250;
    raw_yellow[i * 4 + 1] = 210;
    raw_yellow[i * 4 + 2] = 20;
    raw_yellow[i * 4 + 3] = 255;
  }
  CT_CHECK(ccord_render_upload_rgba(r, "img:rgba", raw_yellow, 16, 16, &err) == CCORD_OK);
  CT_CHECK(ccord_render_texture_cache_count(r) == 3);
  CT_CHECK(ccord_render_texture_evictions(r) >= 1);
  CT_CHECK(!ccord_render_has_texture(r, "img:jpg", nullptr, nullptr));
  CT_CHECK(ccord_render_has_texture(r, "img:png", nullptr, nullptr));
  CT_CHECK(ccord_render_has_texture(r, "img:webp", nullptr, nullptr));
  CT_CHECK(ccord_render_has_texture(r, "img:rgba", nullptr, nullptr));
  ccord_render_set_texture_cache_cap(r, 64);
  CT_CHECK(ccord_render_upload_image(r, "img:jpg", jpg_bytes, jpg_len, nullptr, nullptr, &err) == CCORD_OK);

  // Draw rectangular and rounded textures plus emoji/CJK text, then verify pixels.
  ccord_color bg = {0x31, 0x33, 0x38, 255};
  ccord_render_begin(r, bg);
  ccord_draw_texture(r, "img:png", (ccord_rect){20, 20, 60, 60}, 255);
  ccord_draw_texture_rounded(r, "img:webp", (ccord_rect){120, 20, 60, 60}, 30.0f, 255);
  ccord_draw_texture(r, "img:jpg", (ccord_rect){220, 20, 60, 60}, 255);
  ccord_text_draw(r, emoji_cjk, strlen(emoji_cjk), 20, 120, &st, 0.0f);
  ccord_text_draw(r, "\xF0\x9F\x98\x80", 4, 20, 170, &jumbo_st, 0.0f);
  const char *shot = "test_render_tex_shot.png";
  CT_CHECK(ccord_render_screenshot(r, shot, &err) == CCORD_OK);
  ccord_render_end(r);

  SDL_Surface *s = SDL_LoadPNG(shot);
  CT_CHECK(s != nullptr);
  if (s) {
    CT_CHECK(pixel_is(s, 50, 50, red, 2));
    CT_CHECK(pixel_is(s, 150, 50, blue, 2));
    CT_CHECK(pixel_is(s, 121, 21, bg, 2));   // outside the rounded avatar circle stays background
    CT_CHECK(pixel_is(s, 250, 50, green, 6));
    int glyph_lit = 0;
    for (int y = 120; y < 150; y++) {
      for (int x = 20; x < 140; x++) {
        if (!pixel_is(s, x, y, bg, 10)) glyph_lit++;
      }
    }
    CT_CHECK(glyph_lit > 25);
    int emoji_color = 0;   // Twemoji COLRv0 renders full-colour pixels (yellow face, not monochrome)
    for (int y = 170; y < 225; y++) {
      for (int x = 20; x < 75; x++) {
        uint8_t pr, pg, pb, pa;
        SDL_ReadSurfacePixel(s, x, y, &pr, &pg, &pb, &pa);
        if (pr > 180 && pg > 140 && pb < 120) emoji_color++;
      }
    }
    CT_CHECK(emoji_color > 200);
    SDL_DestroySurface(s);
  }
  remove(shot);
  ccord_free(png_bytes);
  ccord_free(jpg_bytes);
  ccord_free(webp_bytes);
}

static void test_zoom(ccord_renderer *r)
{
  CT_CHECK(ccord_render_get_zoom(r) == 1.0f);
  CT_CHECK(ccord_render_get_zoom(nullptr) == 1.0f);
  ccord_vec2 base = ccord_render_size(r);
  CT_CHECK(base.x == 800.0f && base.y == 600.0f);

  // Clamping, including NaN and infinities.
  ccord_render_set_zoom(r, 5.0f);
  CT_CHECK(ccord_render_get_zoom(r) == CCORD_ZOOM_MAX);
  ccord_render_set_zoom(r, CCORD_ZOOM_MAX + 0.1f);   // just above the cap must clamp too
  CT_CHECK(ccord_render_get_zoom(r) == CCORD_ZOOM_MAX);
  ccord_render_set_zoom(r, 0.1f);
  CT_CHECK(ccord_render_get_zoom(r) == CCORD_ZOOM_MIN);
  ccord_render_set_zoom(r, NAN);
  CT_CHECK(ccord_render_get_zoom(r) == CCORD_ZOOM_MIN);
  ccord_render_set_zoom(r, INFINITY);
  CT_CHECK(ccord_render_get_zoom(r) == CCORD_ZOOM_MAX);
  ccord_render_set_zoom(r, -INFINITY);
  CT_CHECK(ccord_render_get_zoom(r) == CCORD_ZOOM_MIN);
  ccord_render_set_zoom(nullptr, 1.5f);   // must be a safe no-op

  // The logical viewport scales inversely to the zoom.
  ccord_render_set_zoom(r, 2.0f);
  ccord_vec2 z2 = ccord_render_size(r);
  CT_CHECK(z2.x == 400.0f && z2.y == 300.0f);
  ccord_render_set_zoom(r, 0.75f);
  ccord_vec2 z075 = ccord_render_size(r);
  CT_CHECK(fabsf(z075.x - 1066.6667f) < 0.01f && fabsf(z075.y - 800.0f) < 0.01f);

  // Logical text metrics stay ~constant: glyphs scale physically with the zoom.
  ccord_text_style st = {CCORD_FONT_REGULAR, 16.0f, {255, 255, 255, 255}};
  ccord_render_set_zoom(r, 1.0f);
  ccord_vec2 m1 = ccord_text_measure(r, "zoom probe", 10, &st, 0);
  ccord_render_set_zoom(r, 2.0f);
  ccord_vec2 m2 = ccord_text_measure(r, "zoom probe", 10, &st, 0);
  CT_CHECK(m1.x > 10.0f);
  CT_CHECK(m2.x > m1.x * 0.9f && m2.x < m1.x * 1.1f);
  CT_CHECK(m2.y > m1.y * 0.9f && m2.y < m1.y * 1.1f);

  // Same zoom = no-op (cached text survives); a changed zoom flushes the sized fonts.
  (void)ccord_text_measure(r, "zoom probe", 10, &st, 0);
  uint64_t misses = ccord_text_cache_misses(r);
  ccord_render_set_zoom(r, 2.0f);
  (void)ccord_text_measure(r, "zoom probe", 10, &st, 0);
  CT_CHECK(ccord_text_cache_misses(r) == misses);
  ccord_render_set_zoom(r, 1.5f);
  (void)ccord_text_measure(r, "zoom probe", 10, &st, 0);
  CT_CHECK(ccord_text_cache_misses(r) == misses + 1);
  // Back to a previously used zoom: the flush must have dropped its cached text and sized font.
  ccord_render_set_zoom(r, 2.0f);
  (void)ccord_text_measure(r, "zoom probe", 10, &st, 0);
  CT_CHECK(ccord_text_cache_misses(r) == misses + 2);

  // Pixel geometry at 2x: logical (10,10,50,50) covers physical (20,20)-(120,120).
  ccord_render_set_zoom(r, 2.0f);
  ccord_color bg = {0x31, 0x33, 0x38, 255}, red = {255, 0, 0, 255};
  ccord_render_begin(r, bg);
  CT_CHECK(ccord_render_size(r).x == 400.0f && ccord_render_size(r).y == 300.0f);
  ccord_draw_rect(r, (ccord_rect){10, 10, 50, 50}, red);
  ccord_push_clip(r, (ccord_rect){0, 0, 30, 200});
  ccord_draw_rect(r, (ccord_rect){0, 100, 100, 20}, red);   // only logical x < 30 may show
  ccord_pop_clip(r);
  ccord_text_draw(r, "Hello", 5, 10, 140, &st, 0);
  const char *path = "test_render_zoom_shot.png";
  ccord_error err;
  CT_CHECK(ccord_render_screenshot(r, path, &err) == CCORD_OK);
  ccord_render_end(r);
  SDL_Surface *s = SDL_LoadPNG(path);
  CT_CHECK(s != nullptr);
  if (s) {
    CT_CHECK(s->w == 800 && s->h == 600);
    CT_CHECK(pixel_is(s, 60, 60, red, 1));
    CT_CHECK(pixel_is(s, 15, 15, bg, 1));
    CT_CHECK(pixel_is(s, 125, 60, bg, 1));
    CT_CHECK(pixel_is(s, 50, 210, red, 1));   // inside the clip: logical (25,105) -> physical (50,210)
    CT_CHECK(pixel_is(s, 70, 210, bg, 1));    // clipped away: logical x = 35 >= 30
    int lit = 0;   // 2x text sits at physical y ~280..330 (logical y 140), not y 140..165
    for (int y = 280; y < 330; y++) {
      for (int x = 20; x < 140; x++) {
        uint8_t pr, pg, pb, pa;
        SDL_ReadSurfacePixel(s, x, y, &pr, &pg, &pb, &pa);
        if (pr > 200 && pg > 200 && pb > 200) lit++;
      }
    }
    CT_CHECK(lit > 40);
    SDL_DestroySurface(s);
  }
  remove(path);

  // Caret/hit-test round trip at 2x, then restore the default zoom.
  const char *cs = "ab\xC3\xA9" "cd";
  size_t clen = strlen(cs);
  size_t bounds[] = {0, 1, 2, 4, 5, 6};
  for (size_t i = 0; i < sizeof bounds / sizeof bounds[0]; i++) {
    ccord_rect c = ccord_text_caret_rect(r, cs, clen, &st, 0, bounds[i]);
    size_t hit = ccord_text_hit_test(r, cs, clen, &st, 0, c.x + 0.25f, c.y + c.h * 0.5f);
    CT_CHECK(hit == bounds[i]);
  }
  ccord_render_set_zoom(r, 1.0f);
  CT_CHECK(ccord_render_get_zoom(r) == 1.0f);
  ccord_vec2 restored = ccord_render_size(r);
  CT_CHECK(restored.x == 800.0f && restored.y == 600.0f);
}

static void test_platform_apis(ccord_platform *p, ccord_renderer *r)
{
  ccord_error err;
  // URL validation and headless open tracking.
  CT_CHECK(!ccord_platform_open_url(nullptr));
  CT_CHECK(!ccord_platform_open_url(""));
  CT_CHECK(!ccord_platform_open_url("javascript:alert(1)"));
  CT_CHECK(!ccord_platform_open_url("file:///etc/passwd"));
  CT_CHECK(ccord_platform_open_url("https://discord.com/channels/@me"));
  CT_CHECK(ccord_platform_last_opened_url() &&
           strcmp(ccord_platform_last_opened_url(), "https://discord.com/channels/@me") == 0);

  // Clipboard image paste injection and retrieval.
  const uint8_t fake_png[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  CT_CHECK(ccord_platform_set_clipboard_image(p, fake_png, sizeof fake_png, &err) == CCORD_OK);
  size_t clip_len = 0;
  uint8_t *clip = ccord_platform_get_clipboard_image(p, &clip_len);
  CT_CHECK(clip && clip_len == sizeof fake_png && memcmp(clip, fake_png, sizeof fake_png) == 0);
  ccord_free(clip);
  CT_CHECK(ccord_platform_set_clipboard_image(p, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_platform_get_clipboard_image(p, &clip_len) == nullptr);

  // File drop injection, CCORD_EV_DROP_FILE event translation, and file dialog headless guard.
  ccord_platform_open_file_dialog(p);
  CT_CHECK(ccord_platform_inject_drop_file(p, "/tmp/ccord_attachment_test.png", &err) == CCORD_OK);
  ccord_input_event ev;
  CT_CHECK(ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(ev.type == CCORD_EV_DROP_FILE);
  CT_CHECK(ccord_platform_last_drop_file(p) &&
           strcmp(ccord_platform_last_drop_file(p), "/tmp/ccord_attachment_test.png") == 0);
  char popped[256];
  CT_CHECK(ccord_platform_pop_drop_file(p, popped, sizeof popped) &&
           strcmp(popped, "/tmp/ccord_attachment_test.png") == 0);
  CT_CHECK(!ccord_platform_pop_drop_file(p, popped, sizeof popped));

  // Multi-file drop queue verification: separate read cursors for next_event and pop_drop_file (F14)
  CT_CHECK(ccord_platform_inject_drop_file(p, "/tmp/drop1.png", &err) == CCORD_OK);
  CT_CHECK(ccord_platform_inject_drop_file(p, "/tmp/drop2.png", &err) == CCORD_OK);
  CT_CHECK(ccord_platform_inject_drop_file(p, "/tmp/drop3.png", &err) == CCORD_OK);

  // Read all via next_event first:
  ccord_input_event dev1, dev2, dev3;
  CT_CHECK(ccord_platform_next_event(p, &dev1, 0) && dev1.type == CCORD_EV_DROP_FILE);
  CT_CHECK(ccord_platform_next_event(p, &dev2, 0) && dev2.type == CCORD_EV_DROP_FILE);
  CT_CHECK(ccord_platform_next_event(p, &dev3, 0) && dev3.type == CCORD_EV_DROP_FILE);
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0) || ev.type != CCORD_EV_DROP_FILE);

  // pop_drop_file must still have all 3 files without loss:
  char pbuf[256];
  CT_CHECK(ccord_platform_pop_drop_file(p, pbuf, sizeof pbuf) && strcmp(pbuf, "/tmp/drop1.png") == 0);
  CT_CHECK(ccord_platform_pop_drop_file(p, pbuf, sizeof pbuf) && strcmp(pbuf, "/tmp/drop2.png") == 0);
  CT_CHECK(ccord_platform_pop_drop_file(p, pbuf, sizeof pbuf) && strcmp(pbuf, "/tmp/drop3.png") == 0);
  CT_CHECK(!ccord_platform_pop_drop_file(p, pbuf, sizeof pbuf));

  // Interleaved consumer order: pop_drop_file first, then next_event
  CT_CHECK(ccord_platform_inject_drop_file(p, "/tmp/interleaved1.png", &err) == CCORD_OK);
  CT_CHECK(ccord_platform_inject_drop_file(p, "/tmp/interleaved2.png", &err) == CCORD_OK);
  CT_CHECK(ccord_platform_pop_drop_file(p, pbuf, sizeof pbuf) && strcmp(pbuf, "/tmp/interleaved1.png") == 0);
  CT_CHECK(ccord_platform_next_event(p, &dev1, 0) && dev1.type == CCORD_EV_DROP_FILE);
  CT_CHECK(ccord_platform_next_event(p, &dev2, 0) && dev2.type == CCORD_EV_DROP_FILE);
  CT_CHECK(ccord_platform_pop_drop_file(p, pbuf, sizeof pbuf) && strcmp(pbuf, "/tmp/interleaved2.png") == 0);
  CT_CHECK(!ccord_platform_pop_drop_file(p, pbuf, sizeof pbuf));

  // Long path (> 31 chars) preview vs full path (F13):
  const char *long_path = "/tmp/a_very_long_path_that_exceeds_thirty_one_bytes_length.png";
  CT_CHECK(ccord_platform_inject_drop_file(p, long_path, &err) == CCORD_OK);
  CT_CHECK(ccord_platform_next_event(p, &dev1, 0) && dev1.type == CCORD_EV_DROP_FILE);
  CT_CHECK(strlen(dev1.text) <= 31);
  CT_CHECK(strncmp(long_path, dev1.text, strlen(dev1.text)) == 0);
  CT_CHECK(ccord_platform_last_drop_file(p) && strcmp(ccord_platform_last_drop_file(p), long_path) == 0);
  CT_CHECK(ccord_platform_pop_drop_file(p, pbuf, sizeof pbuf) && strcmp(pbuf, long_path) == 0);
  CT_CHECK(!ccord_platform_pop_drop_file(p, pbuf, sizeof pbuf));

  // Window focus and unfocused urgency notifications (SDL_FlashWindow).
  CT_CHECK(ccord_platform_window_focused(p));
  CT_CHECK(!ccord_platform_notify(p, "New mention", "Hello while focused"));
  CT_CHECK(ccord_platform_notify_count(p) == 0);
  ccord_platform_set_window_focused(p, false);
  CT_CHECK(!ccord_platform_window_focused(p));
  CT_CHECK(ccord_platform_notify(p, "New DM", "Ping while unfocused"));
  CT_CHECK(ccord_platform_notify_count(p) == 1);
  ccord_platform_set_window_focused(p, true);

  // Audio capture & playback stream lifecycle with headless fallback.
  ccord_audio_config bad_ac = {.sample_rate = 100};
  CT_CHECK(ccord_audio_create(&bad_ac, &err) == nullptr);
  ccord_audio_config cap_cfg = {.sample_rate = 48000, .channels = 2, .frame_samples = 960, .capture = true};
  ccord_audio *cap = ccord_audio_create(&cap_cfg, &err);
  CT_CHECK(cap != nullptr);
  int16_t pcm[960 * 2] = {0};
  (void)ccord_audio_read(cap, pcm, 960);
  ccord_audio_destroy(cap);

  ccord_audio_config play_cfg = {.sample_rate = 48000, .channels = 2, .frame_samples = 960, .capture = false};
  ccord_audio *play = ccord_audio_create(&play_cfg, &err);
  CT_CHECK(play != nullptr);
  (void)ccord_audio_write(play, pcm, 960);
  ccord_audio_destroy(play);

  // Video stream codec pipeline (libavcodec + libswscale RGBA encode/decode) and PipeWire probe (F15).
  ccord_video_config vcfg_no_pw = {.width = 64, .height = 64, .fps = 30, .bitrate = 400000, .codec = CCORD_VIDEO_CODEC_H264, .use_pipewire = false};
  ccord_video_stream *vs_no_pw = ccord_video_stream_create(&vcfg_no_pw, &err);
  CT_CHECK(vs_no_pw != nullptr);
  if (vs_no_pw) {
    CT_CHECK(!ccord_video_stream_pipewire_available(vs_no_pw));
    ccord_video_stream_destroy(vs_no_pw);
  }

  ccord_video_config vcfg = {.width = 64, .height = 64, .fps = 30, .bitrate = 400000, .codec = CCORD_VIDEO_CODEC_H264, .use_pipewire = true};
  ccord_video_stream *vs = ccord_video_stream_create(&vcfg, &err);
  CT_CHECK(vs != nullptr);
  if (vs) {
    (void)ccord_video_stream_pipewire_available(vs);
    CT_CHECK(ccord_video_stream_codec_name(vs)[0] != '\0');
    uint8_t src_rgba[64 * 64 * 4];
    for (int i = 0; i < 64 * 64; i++) {
      src_rgba[i * 4 + 0] = 220;
      src_rgba[i * 4 + 1] = 40;
      src_rgba[i * 4 + 2] = 60;
      src_rgba[i * 4 + 3] = 255;
    }
    uint8_t pkt[32768];
    size_t pkt_len = 0;
    CT_CHECK(ccord_video_stream_encode_rgba(vs, src_rgba, 64, 64, 64 * 4, pkt, sizeof pkt, &pkt_len, &err) == CCORD_OK);
    CT_CHECK(pkt_len > 0);
    uint8_t *dec_rgba = nullptr;
    int vw = 0, vh = 0;
    CT_CHECK(ccord_video_stream_decode_rgba(vs, pkt, pkt_len, &dec_rgba, &vw, &vh, &err) == CCORD_OK);
    CT_CHECK(dec_rgba && vw == 64 && vh == 64);
    if (dec_rgba) {
      CT_CHECK(abs((int)dec_rgba[0] - 220) <= 15 && abs((int)dec_rgba[1] - 40) <= 15 && abs((int)dec_rgba[2] - 60) <= 15);
      CT_CHECK(ccord_render_upload_rgba(r, "video:stage", dec_rgba, vw, vh, &err) == CCORD_OK);
      ccord_free(dec_rgba);
    }
    CT_CHECK(ccord_video_stream_step_test_frame(vs, &err) == CCORD_OK);
    CT_CHECK(ccord_video_stream_last_rgba(vs, &vw, &vh) != nullptr && vw == 64 && vh == 64);

    // Arbitrary BGRA input dimension scaling + keyframe tracking + resolve_dimensions.
    int rw = 0, rh = 0;
    ccord_video_resolve_dimensions(1920, 1080, 720, &rw, &rh);
    CT_CHECK(rw == 1280 && rh == 720);
    ccord_video_resolve_dimensions(1921, 1081, 0, &rw, &rh);
    CT_CHECK((rw & 1) == 0 && (rh & 1) == 0);
    uint8_t src_bgra[128 * 96 * 4];
    for (int i = 0; i < 128 * 96; i++) {
      src_bgra[i * 4 + 0] = 60;
      src_bgra[i * 4 + 1] = 40;
      src_bgra[i * 4 + 2] = 220;
      src_bgra[i * 4 + 3] = 255;
    }
    ccord_video_stream_force_keyframe(vs);
    CT_CHECK(ccord_video_stream_encode_bgra(vs, src_bgra, 128, 96, 128 * 4, pkt, sizeof pkt, &pkt_len, &err) == CCORD_OK);
    CT_CHECK(pkt_len > 0 && ccord_video_stream_last_keyframe(vs));
    CT_CHECK(ccord_video_stream_last_rgba(vs, &vw, &vh) != nullptr && vw == 64 && vh == 64);
    ccord_video_stream_destroy(vs);
  }
}

int main(int argc, char **argv)
{
  ccord_error err;
  if (ccord_paths_init(argc > 0 ? argv[0] : "test_render", &err) != CCORD_OK) {
    fprintf(stderr, "paths: %s\n", err.message);
    return 1;
  }
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK) return 1;
  ccord_theme theme;
  if (ccord_theme_load(&theme, path, &err) != CCORD_OK) {
    fprintf(stderr, "theme: %s\n", err.message);
    return 1;
  }
  ccord_platform_config pc = {.title = "test", .width = 800, .height = 600, .headless = true};
  ccord_platform *p = ccord_platform_create(&pc, &err);
  if (!p) {
    fprintf(stderr, "skip: %s\n", err.message);
    return 77;
  }
  ccord_renderer *r = ccord_render_create(p, &theme, &err);
  if (!r) {
    fprintf(stderr, "renderer: %s\n", err.message);
    ccord_platform_destroy(p);
    return 1;
  }
  ccord_render_begin(r, theme.c.bg_primary);
  test_text(r);
  ccord_render_end(r);
  test_screenshot(r);
  test_group_effects(r);
  test_fallback_fonts_and_textures(r);
  test_zoom(r);
  test_platform_apis(p, r);
  ccord_render_destroy(r);
  ccord_platform_destroy(p);
  return CT_RESULT("test_render");
}
