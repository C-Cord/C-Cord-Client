// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>
#include <ccord/platform/platform.h>
#include <ccord/theme/theme.h>
// Immediate-mode 2D API. ALL coordinates are logical px (float); the renderer multiplies by display scale * UI zoom.
typedef struct ccord_rect { float x, y, w, h; } ccord_rect;
typedef struct ccord_vec2 { float x, y; } ccord_vec2;
typedef enum ccord_font_style : uint8_t { CCORD_FONT_REGULAR, CCORD_FONT_BOLD, CCORD_FONT_EMOJI } ccord_font_style;
typedef struct ccord_text_style { ccord_font_style font; float size; ccord_color color; } ccord_text_style;
typedef struct ccord_renderer ccord_renderer;
// Fonts are opened from the asset dir using theme->font_regular / font_bold. Bundled emoji (Twemoji) and CJK
// subsets in the asset dir are attached as fallbacks first, then host system fonts on first miss (deduplicated by family name).
ccord_renderer *ccord_render_create(ccord_platform *p, const ccord_theme *theme, ccord_error *err);
void ccord_render_destroy(ccord_renderer *r);
// Flushes GPU texture/disc/text caches (call on Android LOW_MEMORY or RENDER_TARGETS/DEVICE_RESET).
void ccord_render_evict_caches(ccord_renderer *r);
void ccord_render_begin(ccord_renderer *r, ccord_color clear);   // re-reads window size/scale; reopens fonts if scale changed
void ccord_render_end(ccord_renderer *r);                         // present
ccord_vec2 ccord_render_size(const ccord_renderer *r);            // logical px (window size / display scale / zoom)
// UI zoom on top of the display scale: scales the logical viewport, clips and glyph sizes. Default 1.0.
#define CCORD_ZOOM_MIN 0.75f
#define CCORD_ZOOM_MAX 2.0f
// Clamps to [CCORD_ZOOM_MIN, CCORD_ZOOM_MAX] (NaN -> min). No-op (no text-cache flush) when the value is unchanged.
void ccord_render_set_zoom(ccord_renderer *r, float zoom);
float ccord_render_get_zoom(const ccord_renderer *r);
void ccord_draw_rect(ccord_renderer *r, ccord_rect rc, ccord_color c);
void ccord_draw_rrect(ccord_renderer *r, ccord_rect rc, float radius, ccord_color c);       // antialiased via generated disc texture
void ccord_draw_circle(ccord_renderer *r, float cx, float cy, float radius, ccord_color c);   // antialiased
void ccord_draw_line(ccord_renderer *r, ccord_vec2 a, ccord_vec2 b, float thickness, ccord_color c);   // quad, for icons
void ccord_draw_triangle(ccord_renderer *r, ccord_vec2 a, ccord_vec2 b, ccord_vec2 c, ccord_color col);
void ccord_draw_rrect_stroke(ccord_renderer *r, ccord_rect rc, float radius, float thickness, ccord_color c);
void ccord_draw_circle_stroke(ccord_renderer *r, float cx, float cy, float radius, float thickness, ccord_color c);
// Draws an arc/ring stroke (filled == false) or pie wedge (filled == true). Angles in degrees (0 = right, clockwise).
void ccord_draw_arc(ccord_renderer *r, float cx, float cy, float radius, float start_deg, float end_deg,
                    float thickness, ccord_color c, bool filled);
// Draws a polyline (filled == false) or convex/fan filled polygon (filled == true) through `pts[0..count)`.
void ccord_draw_polygon(ccord_renderer *r, const ccord_vec2 *pts, size_t count, ccord_color c, bool filled,
                        float thickness);
// Draws an axis-aligned linear gradient rectangle (vertical == true: top->bottom, false: left->right).
void ccord_draw_gradient_rect(ccord_renderer *r, ccord_rect rc, ccord_color c0, ccord_color c1, bool vertical);
// Swaps the regular/bold base fonts to custom absolute paths (nullptr or "" restores the default theme font).
// Flushes the text cache if changed. Returns false if a non-empty path could not be opened as a font.
bool ccord_render_set_custom_fonts(ccord_renderer *r, const char *regular_path, const char *bold_path);
// Group effects: everything drawn between a push and its pop is multiplied by `alpha` (0..1, nests by multiplying) or
// shifted by (dx, dy) logical px (nests by adding). This is how a menu fades in or a toast slides up without every
// widget knowing about it. Depth 8; extra pushes are ignored (and their pops too), and both stacks are reset by
// ccord_render_begin so an early return can never leave the next frame faded.
void ccord_push_alpha(ccord_renderer *r, float alpha);
void ccord_pop_alpha(ccord_renderer *r);
void ccord_push_offset(ccord_renderer *r, float dx, float dy);
void ccord_pop_offset(ccord_renderer *r);
void ccord_push_clip(ccord_renderer *r, ccord_rect rc);   // intersects with current; depth <= 16, overflow logs and is ignored
void ccord_pop_clip(ccord_renderer *r);
// Text: len = byte length (use strlen for C strings). wrap_width 0 = single line, no wrapping. Shaping by HarfBuzz via SDL3_ttf.
// A cache of TTF_Text objects (LRU, 1024 entries, key = hash(font,size_px,wrap,bytes)) serves both measure and draw.
ccord_vec2 ccord_text_measure(ccord_renderer *r, const char *utf8, size_t len, const ccord_text_style *st, float wrap_width);
void ccord_text_draw(ccord_renderer *r, const char *utf8, size_t len, float x, float y, const ccord_text_style *st, float wrap_width);
// Draws on one line, cutting with "…" to fit max_w. Returns drawn width.
float ccord_text_draw_ellipsis(ccord_renderer *r, const char *utf8, size_t len, float x, float y, float max_w, const ccord_text_style *st);
// Byte offset of the caret position nearest to (px,py) relative to the text origin (wrapped text supported).
size_t ccord_text_hit_test(ccord_renderer *r, const char *utf8, size_t len, const ccord_text_style *st, float wrap_width, float px, float py);
// Caret rectangle for byte offset `pos` (relative to text origin).
ccord_rect ccord_text_caret_rect(ccord_renderer *r, const char *utf8, size_t len, const ccord_text_style *st, float wrap_width, size_t pos);
// Reads back the current frame and writes a PNG (SDL_RenderReadPixels + SDL_SavePNG).
// Call after drawing and BEFORE ccord_render_end: reading after present is undefined on GPU renderers.
[[nodiscard]] ccord_status ccord_render_screenshot(ccord_renderer *r, const char *path, ccord_error *err);
// Number of text-cache misses so far (a miss creates a TTF_Text). Test/perf hook.
uint64_t ccord_text_cache_misses(const ccord_renderer *r);
// Number of fallback fonts (bundled emoji/CJK, then system symbols) attached via TTF_AddFallbackFont.
int ccord_render_fallback_font_count(const ccord_renderer *r);
// Family name of attached fallback font `index` (0 = first attached); nullptr when out of range. Test hook.
const char *ccord_render_fallback_font_family(const ccord_renderer *r, int index);
// Fonts attached after startup because a code point was not covered by the bundled ones (found on the host at run
// time, on the first miss only). 0 until some text needs one.
int ccord_render_system_font_count(const ccord_renderer *r);
// Glyph probe, test hook: how the text path draws one code point (between two 'a's) in the given style.
typedef enum ccord_glyph_kind { CCORD_GLYPH_NOTDEF, CCORD_GLYPH_REAL, CCORD_GLYPH_NONE } ccord_glyph_kind;   // box / glyph / nothing
ccord_glyph_kind ccord_text_glyph_kind(ccord_renderer *r, const ccord_text_style *st, uint32_t cp);
// Turns the run-time font lookup and the control-character cutting off (false) or on again; flushes the text cache.
void ccord_render_set_glyph_fixes(ccord_renderer *r, bool enabled);

// Image decoding and keyed LRU SDL_Texture cache ------------------------------
// Decodes PNG (libpng), JPEG (libjpeg), or WebP (libwebp) bytes into a ccord_malloc'd RGBA32 buffer (4 bytes/px).
[[nodiscard]] ccord_status ccord_image_decode_rgba(const uint8_t *data, size_t len, uint8_t **out_rgba,
                                                   int *out_w, int *out_h, ccord_error *err);
// Decodes an image and uploads it into the renderer's LRU SDL_Texture cache under `key`.
[[nodiscard]] ccord_status ccord_render_upload_image(ccord_renderer *r, const char *key, const uint8_t *data,
                                                     size_t len, int *out_w, int *out_h, ccord_error *err);
// Like ccord_render_upload_image with hints. Animated content (GIF, APNG, animated WebP and, with allow_video,
// a silent MP4/WebM clip) is decoded no larger than max_dim and loads progressively: the first frame is uploaded
// at once and the rest arrive from ccord_render_pump_media. Up to anim_budget bytes of decoded frames are kept;
// a longer animation is streamed from its compressed bytes instead, so it still plays whole in bounded memory.
typedef struct ccord_media_opts {
  int max_dim;          // 0 = default (800)
  size_t anim_budget;   // 0 = default (16 MiB)
  bool allow_video;
} ccord_media_opts;
[[nodiscard]] ccord_status ccord_render_upload_media(ccord_renderer *r, const char *key, const uint8_t *data,
                                                     size_t len, const ccord_media_opts *opts, int *out_w,
                                                     int *out_h, ccord_error *err);
// Spends up to budget_ms decoding animations that are still loading. Returns true while any remain: keep
// drawing frames until it returns false.
bool ccord_render_pump_media(ccord_renderer *r, uint32_t budget_ms);
// True when an animated texture was drawn in its animating state since the last call (then cleared): the caller
// must schedule another frame for the animation to advance.
bool ccord_render_take_anim_activity(ccord_renderer *r);
// Uploads raw RGBA32 pixels (width * height * 4 bytes) into the LRU SDL_Texture cache under `key`.
[[nodiscard]] ccord_status ccord_render_upload_rgba(ccord_renderer *r, const char *key, const uint8_t *rgba,
                                                    int width, int height, ccord_error *err);
bool ccord_render_has_texture(ccord_renderer *r, const char *key, int *out_w, int *out_h);
void ccord_render_evict_texture(ccord_renderer *r, const char *key);
void ccord_render_set_texture_cache_cap(ccord_renderer *r, int cap);
int ccord_render_texture_cache_count(const ccord_renderer *r);
uint64_t ccord_render_texture_evictions(const ccord_renderer *r);
// Draws a cached texture into logical rect `dst` (alpha 255 = opaque). No-op if `key` is not in cache.
void ccord_draw_texture(ccord_renderer *r, const char *key, ccord_rect dst, uint8_t alpha);
// Draws a cached texture with rounded corners of `radius` logical px (radius >= min(w,h)/2 draws a circle).
void ccord_draw_texture_rounded(ccord_renderer *r, const char *key, ccord_rect dst, float radius, uint8_t alpha);
// Draws a cached texture with rounded corners; when `animate` is false, multi-frame textures stay on frame 0.
void ccord_draw_texture_rounded_anim(ccord_renderer *r, const char *key, ccord_rect dst, float radius, uint8_t alpha, bool animate);
// Like ccord_draw_texture_rounded_anim, but keeps the image's aspect ratio: it fills `dst` and crops the overflow
// evenly (CSS object-fit: cover) instead of stretching.
void ccord_draw_texture_cover(ccord_renderer *r, const char *key, ccord_rect dst, float radius, uint8_t alpha, bool animate);
void ccord_render_set_anim_clock(ccord_renderer *r, uint64_t now_ms);
bool ccord_render_texture_is_animated(ccord_renderer *r, const char *key, size_t *out_cur_frame, size_t *out_frame_count);
