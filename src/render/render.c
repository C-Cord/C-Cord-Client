// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "render_internal.h"

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/paths.h>
#include <ccord/render/anim.h>

#if CCORD_OS_ANDROID
#define RENDER_TEX_VRAM_BUDGET ((size_t)48u * 1024u * 1024u)
#define RENDER_ANIM_DEFAULT_DIM 512                                  // tighter mobile decode cap to avoid lmkd OOM
#define RENDER_ANIM_DEFAULT_BUDGET ((size_t)8u * 1024u * 1024u)
#define RENDER_ANIM_TOTAL_BUDGET ((size_t)32u * 1024u * 1024u)
#define RENDER_STREAM_IDLE_MS 10000u                                 // close off-screen streaming decoders after 10 s idle
#define RENDER_STREAM_MAX_OPEN 4
#else
#define RENDER_TEX_VRAM_BUDGET ((size_t)128u * 1024u * 1024u)
#define RENDER_ANIM_DEFAULT_DIM 800                                  // animated images are decoded no larger than this
#define RENDER_ANIM_DEFAULT_BUDGET ((size_t)16u * 1024u * 1024u)     // decoded bytes kept per animation
#define RENDER_ANIM_TOTAL_BUDGET ((size_t)96u * 1024u * 1024u)       // ... and across the whole cache
#define RENDER_STREAM_IDLE_MS 15000u                                 // close off-screen streaming decoders after 15 s idle
#define RENDER_STREAM_MAX_OPEN 12
#endif
#define RENDER_ANIM_INLINE_MS 6u                                     // decode time spent inside the upload call

static void tex_cache_init(ccord_renderer *r);
static void tex_free_slot(ccord_renderer *r, int slot);

static SDL_FColor fcolor(const ccord_renderer *r, ccord_color c, float alpha_mul)
{
  return (SDL_FColor){(float)c.r / 255.0f, (float)c.g / 255.0f, (float)c.b / 255.0f,
                      (float)c.a / 255.0f * alpha_mul * r->alpha};
}

static void set_draw_color(ccord_renderer *r, ccord_color c)
{
  SDL_SetRenderDrawColor(r->sdl, c.r, c.g, c.b, ccord_fx_alpha(r, c.a));
}

// Snaps a logical rect to whole physical pixels so neighbours sharing an edge never leave a seam.
static SDL_FRect snap(const ccord_renderer *r, ccord_rect rc)
{
  float x0 = roundf((rc.x + r->ox) * r->scale), y0 = roundf((rc.y + r->oy) * r->scale);
  float x1 = roundf((rc.x + r->ox + rc.w) * r->scale), y1 = roundf((rc.y + r->oy + rc.h) * r->scale);
  return (SDL_FRect){x0, y0, x1 - x0, y1 - y0};
}

ccord_renderer *ccord_render_create(ccord_platform *p, const ccord_theme *theme, ccord_error *err)
{
  ccord_error_clear(err);
  if (!p || !theme) {
    ccord_error_set(err, CCORD_ERR_INVALID, "render: platform and theme are required");
    return nullptr;
  }
  ccord_renderer *r = ccord_calloc(1, sizeof *r);
  if (!r) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "render: out of memory");
    return nullptr;
  }
  tex_cache_init(r);
  text_cache_init(r);
  r->platform = p;
  r->window = (SDL_Window *)ccord_platform_sdl_window(p);
  r->line_factor = theme->m.line_height_factor > 0.5f ? theme->m.line_height_factor : 1.375f;
  r->zoom = 1.0f;
  r->display_scale = 1.0f;
  r->scale = 1.0f;
  r->alpha = 1.0f;
  for (int i = 0; i < 2; i++) {
    const char *rel = i == 0 ? theme->font_regular : theme->font_bold;
    if (ccord_paths_join(r->font_path[i], sizeof r->font_path[i], ccord_paths_asset_dir(), rel, err) != CCORD_OK) {
      ccord_render_destroy(r);
      return nullptr;
    }
  }
  // Headless video drivers have no GPU to draw on: use the software renderer (deterministic, no driver code).
  const char *video = SDL_GetCurrentVideoDriver();
  const bool headless = video && (strcmp(video, "dummy") == 0 || strcmp(video, "offscreen") == 0);
  const char *driver = headless ? "software" : nullptr;
  r->sdl = SDL_CreateRenderer(r->window, driver);
  if (!r->sdl) {
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "render: cannot create renderer: %s", SDL_GetError());
    ccord_render_destroy(r);
    return nullptr;
  }
  if (!headless) SDL_SetRenderVSync(r->sdl, 1);   // headless dummy/offscreen skips simulated 60 Hz sleeps
  CCORD_LOG_I("render: SDL renderer '%s', video driver '%s'", SDL_GetRendererName(r->sdl), video ? video : "?");
  SDL_SetRenderDrawBlendMode(r->sdl, SDL_BLENDMODE_BLEND);
  if (!headless) {
    // Paint the theme backdrop immediately so Android/GLES surfaces never sit solid black while startup finishes.
    SDL_SetRenderDrawColor(r->sdl, theme->c.bg_tertiary.r, theme->c.bg_tertiary.g, theme->c.bg_tertiary.b, 255);
    SDL_RenderClear(r->sdl);
    SDL_RenderPresent(r->sdl);
  }
  if (!TTF_Init()) {
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "render: TTF_Init failed: %s", SDL_GetError());
    ccord_render_destroy(r);
    return nullptr;
  }
  r->ttf_inited = true;
  r->engine = TTF_CreateRendererTextEngine(r->sdl);
  if (!r->engine) {
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "render: cannot create text engine: %s", SDL_GetError());
    ccord_render_destroy(r);
    return nullptr;
  }
  for (int i = 0; i < 2; i++) {
    const char *rel = i == 0 ? theme->font_regular : theme->font_bold;
    r->base[i] = TTF_OpenFont(r->font_path[i], 16.0f);
    if (!r->base[i]) {
      size_t flen = 0;
      const unsigned char *fbytes = ccord_embedded_font(rel, &flen);
      if (fbytes && flen > 0) {
        SDL_IOStream *io = SDL_IOFromConstMem(fbytes, flen);
        if (io) r->base[i] = TTF_OpenFontIO(io, true, 16.0f);
      }
    }
    if (!r->base[i]) {
      ccord_error_set(err, CCORD_ERR_IO, "render: cannot open font %s: %s", rel,
                      SDL_GetError());
      ccord_render_destroy(r);
      return nullptr;
    }
  }
  // Bundled emoji/CJK fallback fonts (~16 MB) are loaded lazily on the first glyph NotoSans doesn't cover,
  // avoiding a cold-start stall before the first frame (especially on Android).
  r->tex_cap = CCORD_TEX_CACHE_SIZE;
  float w, h, s;
  ccord_platform_size(p, &w, &h, &s);
  r->display_scale = s;
  r->scale = s * r->zoom;
  r->lw = w / r->zoom;
  r->lh = h / r->zoom;
  return r;
}

void ccord_render_evict_caches(ccord_renderer *r)
{
  if (!r) return;
  text_cache_flush(r);
  while (r->tex_lru_head != -1) {
    tex_free_slot(r, r->tex_lru_head);
  }
  tex_cache_init(r);
  for (int i = 0; i < r->ndiscs; i++) SDL_DestroyTexture(r->discs[i].tex);
  r->ndiscs = 0;
  r->disc_next = 0;
}

void ccord_render_destroy(ccord_renderer *r)
{
  if (!r) return;
  text_cache_flush(r);
  while (r->tex_lru_head != -1) {
    tex_free_slot(r, r->tex_lru_head);
  }
  for (int i = 0; i < r->ndiscs; i++) SDL_DestroyTexture(r->discs[i].tex);
  for (int i = 0; i < 2; i++) {
    if (r->base[i]) {
      TTF_ClearFallbackFonts(r->base[i]);
      TTF_CloseFont(r->base[i]);
    }
  }
  for (int k = 0; k < CCORD_BUNDLED_COUNT; k++) {
    if (r->bundled_base[k]) TTF_CloseFont(r->bundled_base[k]);
  }
  for (int k = 0; k < r->nsys_fallbacks; k++) {
    if (r->fallback_base[k]) TTF_CloseFont(r->fallback_base[k]);
  }
  if (r->engine) TTF_DestroyRendererTextEngine(r->engine);
  if (r->ttf_inited) TTF_Quit();
  if (r->sdl) SDL_DestroyRenderer(r->sdl);
  ccord_free(r);
}

void ccord_render_begin(ccord_renderer *r, ccord_color clear)
{
  float w, h, s;
  ccord_platform_size(r->platform, &w, &h, &s);
  if (s != r->display_scale) {
    text_cache_flush(r);   // sized fonts depend on the scale
    r->display_scale = s;
    r->scale = s * r->zoom;
  }
  r->lw = w / r->zoom;
  r->lh = h / r->zoom;
  r->nclips = 0;
  r->alpha = 1.0f;
  r->nalpha = 0;
  r->ox = r->oy = 0.0f;
  r->noff = 0;
  r->prev_frame_tick = r->frame_tick;
  r->frame_tick = r->tex_tick;
  SDL_SetRenderClipRect(r->sdl, nullptr);
  SDL_SetRenderDrawBlendMode(r->sdl, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(r->sdl, clear.r, clear.g, clear.b, 255);
  SDL_RenderClear(r->sdl);
}

void ccord_render_end(ccord_renderer *r)
{
  SDL_SetRenderClipRect(r->sdl, nullptr);
  SDL_RenderPresent(r->sdl);
}

ccord_vec2 ccord_render_size(const ccord_renderer *r)
{
  return (ccord_vec2){r->lw, r->lh};
}

void ccord_render_set_zoom(ccord_renderer *r, float zoom)
{
  if (!r) return;
  if (!(zoom >= CCORD_ZOOM_MIN)) zoom = CCORD_ZOOM_MIN;   // also catches NaN
  if (zoom > CCORD_ZOOM_MAX) zoom = CCORD_ZOOM_MAX;
  if (zoom == r->zoom) return;
  r->zoom = zoom;
  float w, h, s;
  ccord_platform_size(r->platform, &w, &h, &s);
  r->display_scale = s;
  r->scale = s * zoom;
  r->lw = w / zoom;
  r->lh = h / zoom;
  text_cache_flush(r);   // sized fonts depend on the scale
}

float ccord_render_get_zoom(const ccord_renderer *r)
{
  return r ? r->zoom : 1.0f;
}

void ccord_draw_rect(ccord_renderer *r, ccord_rect rc, ccord_color c)
{
  if (c.a == 0 || rc.w <= 0.0f || rc.h <= 0.0f) return;
  SDL_FRect fr = snap(r, rc);
  if (fr.w <= 0.0f || fr.h <= 0.0f) return;
  set_draw_color(r, c);
  SDL_RenderFillRect(r->sdl, &fr);
}

// White disc with analytic edge coverage in the alpha channel; tinted with colour mod at draw time.
static SDL_Texture *disc_texture(ccord_renderer *r, int d)
{
  for (int i = 0; i < r->ndiscs; i++) {
    if (r->discs[i].diameter == d) return r->discs[i].tex;
  }
  uint32_t *px = ccord_alloc_array((size_t)d * (size_t)d, sizeof *px);
  if (!px) return nullptr;
  float rad = (float)d * 0.5f;
  for (int y = 0; y < d; y++) {
    for (int x = 0; x < d; x++) {
      float dx = (float)x + 0.5f - rad, dy = (float)y + 0.5f - rad;
      float cov = rad - sqrtf(dx * dx + dy * dy) + 0.5f;
      cov = cov < 0.0f ? 0.0f : (cov > 1.0f ? 1.0f : cov);
      uint32_t a = (uint32_t)lroundf(cov * 255.0f);
      uint8_t bytes[4] = {255, 255, 255, (uint8_t)a};   // RGBA32 = byte order R,G,B,A on any endianness
      memcpy(&px[(size_t)y * (size_t)d + (size_t)x], bytes, 4);
    }
  }
  SDL_Texture *tex = SDL_CreateTexture(r->sdl, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, d, d);
  if (tex) {
    SDL_UpdateTexture(tex, nullptr, px, d * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);   // always drawn 1:1
  }
  ccord_free(px);
  if (!tex) return nullptr;
  int slot;
  if (r->ndiscs < CCORD_DISC_CACHE_SIZE) {
    slot = r->ndiscs++;
  } else {
    slot = r->disc_next;
    r->disc_next = (r->disc_next + 1) % CCORD_DISC_CACHE_SIZE;
    SDL_DestroyTexture(r->discs[slot].tex);
  }
  r->discs[slot] = (disc_entry){tex, d};
  return tex;
}

static void tint(const ccord_renderer *r, SDL_Texture *t, ccord_color c)
{
  SDL_SetTextureColorMod(t, c.r, c.g, c.b);
  SDL_SetTextureAlphaMod(t, ccord_fx_alpha(r, c.a));
}

static void blit(ccord_renderer *r, SDL_Texture *t, float sx, float sy, float sw, float sh, float dx, float dy)
{
  SDL_FRect src = {sx, sy, sw, sh};
  SDL_FRect dst = {dx, dy, sw, sh};
  SDL_RenderTexture(r->sdl, t, &src, &dst);
}

void ccord_draw_rrect(ccord_renderer *r, ccord_rect rc, float radius, ccord_color c)
{
  if (c.a == 0 || rc.w <= 0.0f || rc.h <= 0.0f) return;
  SDL_FRect fr = snap(r, rc);
  if (fr.w <= 0.0f || fr.h <= 0.0f) return;
  float maxr = floorf((fr.w < fr.h ? fr.w : fr.h) * 0.5f);
  float rr = roundf(radius * r->scale);
  if (rr > maxr) rr = maxr;
  if (rr > 256.0f) rr = 256.0f;
  int R = (int)rr;
  if (R <= 0) {
    set_draw_color(r, c);
    SDL_RenderFillRect(r->sdl, &fr);
    return;
  }
  SDL_Texture *tex = disc_texture(r, 2 * R);
  if (!tex) {   // out of memory: degrade to a square rect rather than draw nothing
    set_draw_color(r, c);
    SDL_RenderFillRect(r->sdl, &fr);
    return;
  }
  tint(r, tex, c);
  float Rf = (float)R;
  float x = fr.x, y = fr.y, w = fr.w, h = fr.h;
  blit(r, tex, 0, 0, Rf, Rf, x, y);
  blit(r, tex, Rf, 0, Rf, Rf, x + w - Rf, y);
  blit(r, tex, 0, Rf, Rf, Rf, x, y + h - Rf);
  blit(r, tex, Rf, Rf, Rf, Rf, x + w - Rf, y + h - Rf);
  if (R >= 2) {
    // For R >= 2 (diameter >= 4), the 2x2 center of the disc texture is solid opaque white (255,255,255,255).
    // Drawing the interior bands from that 1x1 texel keeps all quads of the rounded rect in one GPU texture batch.
    SDL_FRect solid = {Rf - 1.0f, Rf - 1.0f, 1.0f, 1.0f};
    if (w > 2.0f * Rf) {
      SDL_FRect top = {x + Rf, y, w - 2.0f * Rf, Rf};
      SDL_FRect bot = {x + Rf, y + h - Rf, w - 2.0f * Rf, Rf};
      SDL_RenderTexture(r->sdl, tex, &solid, &top);
      SDL_RenderTexture(r->sdl, tex, &solid, &bot);
    }
    if (h > 2.0f * Rf) {
      SDL_FRect mid = {x, y + Rf, w, h - 2.0f * Rf};
      SDL_RenderTexture(r->sdl, tex, &solid, &mid);
    }
  } else {
    // A 2x2 disc (R == 1) has edge-antialiased alpha on all 4 pixels, so fill the interior bands untextured.
    set_draw_color(r, c);
    SDL_FRect mid[3] = {
      {x + Rf, y, w - 2.0f * Rf, Rf},            // top band between the corners
      {x + Rf, y + h - Rf, w - 2.0f * Rf, Rf},   // bottom band
      {x, y + Rf, w, h - 2.0f * Rf},             // full-width middle
    };
    SDL_RenderFillRects(r->sdl, mid, 3);
  }
}

void ccord_draw_circle(ccord_renderer *r, float cx, float cy, float radius, ccord_color c)
{
  if (c.a == 0 || radius <= 0.0f) return;
  int d = (int)lroundf(radius * 2.0f * r->scale);
  if (d <= 0) return;
  if (d > 512) d = 512;
  SDL_Texture *tex = disc_texture(r, d);
  if (!tex) return;
  tint(r, tex, c);
  SDL_FRect dst = {roundf((cx + r->ox) * r->scale - (float)d * 0.5f), roundf((cy + r->oy) * r->scale - (float)d * 0.5f), (float)d, (float)d};
  SDL_RenderTexture(r->sdl, tex, nullptr, &dst);
}

// Anti-aliased line: an opaque core strip plus two 1 px feather strips whose outer vertices are transparent.
void ccord_draw_line(ccord_renderer *r, ccord_vec2 a, ccord_vec2 b, float thickness, ccord_color c)
{
  if (c.a == 0 || thickness <= 0.0f) return;
  float ax = (a.x + r->ox) * r->scale, ay = (a.y + r->oy) * r->scale, bx = (b.x + r->ox) * r->scale, by = (b.y + r->oy) * r->scale;
  float dx = bx - ax, dy = by - ay;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.001f) return;
  float nx = -dy / len, ny = dx / len;
  float hw = thickness * r->scale * 0.5f;
  float inner = hw > 0.5f ? hw - 0.5f : 0.0f;
  float outer = hw + 0.5f;
  float peak = hw >= 0.5f ? 1.0f : hw * 2.0f;
  const float off[4] = {outer, inner, -inner, -outer};
  const float al[4] = {0.0f, peak, peak, 0.0f};
  SDL_Vertex v[8];
  for (int e = 0; e < 2; e++) {
    float px = e ? bx : ax, py = e ? by : ay;
    for (int k = 0; k < 4; k++) {
      v[e * 4 + k] = (SDL_Vertex){.position = {px + nx * off[k], py + ny * off[k]}, .color = fcolor(r, c, al[k]), .tex_coord = {0, 0}};
    }
  }
  int idx[18];
  int n = 0;
  for (int k = 0; k < 3; k++) {
    idx[n++] = k;
    idx[n++] = k + 1;
    idx[n++] = 4 + k + 1;
    idx[n++] = k;
    idx[n++] = 4 + k + 1;
    idx[n++] = 4 + k;
  }
  SDL_RenderGeometry(r->sdl, nullptr, v, 8, idx, n);
}

void ccord_draw_triangle(ccord_renderer *r, ccord_vec2 a, ccord_vec2 b, ccord_vec2 c, ccord_color col)
{
  if (col.a == 0) return;
  SDL_FColor fc = fcolor(r, col, 1.0f);
  SDL_Vertex v[3] = {
    {.position = {(a.x + r->ox) * r->scale, (a.y + r->oy) * r->scale}, .color = fc},
    {.position = {(b.x + r->ox) * r->scale, (b.y + r->oy) * r->scale}, .color = fc},
    {.position = {(c.x + r->ox) * r->scale, (c.y + r->oy) * r->scale}, .color = fc},
  };
  SDL_RenderGeometry(r->sdl, nullptr, v, 3, nullptr, 0);
  // Cover the aliased edges with feathered outlines.
  ccord_draw_line(r, a, b, 1.0f / r->scale, col);
  ccord_draw_line(r, b, c, 1.0f / r->scale, col);
  ccord_draw_line(r, c, a, 1.0f / r->scale, col);
}

static void apply_clip(ccord_renderer *r)
{
  if (r->nclips == 0) {
    SDL_SetRenderClipRect(r->sdl, nullptr);
  } else {
    SDL_SetRenderClipRect(r->sdl, &r->clips[r->nclips - 1]);
  }
}

void ccord_push_alpha(ccord_renderer *r, float alpha)
{
  if (!r || r->nalpha >= CCORD_FX_DEPTH) return;
  r->alpha_stack[r->nalpha++] = r->alpha;
  r->alpha *= alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
}

void ccord_pop_alpha(ccord_renderer *r)
{
  if (!r || r->nalpha <= 0) return;
  r->alpha = r->alpha_stack[--r->nalpha];
}

void ccord_push_offset(ccord_renderer *r, float dx, float dy)
{
  if (!r || r->noff >= CCORD_FX_DEPTH) return;
  r->off_stack[r->noff][0] = r->ox;
  r->off_stack[r->noff][1] = r->oy;
  r->noff++;
  r->ox += dx;
  r->oy += dy;
}

void ccord_pop_offset(ccord_renderer *r)
{
  if (!r || r->noff <= 0) return;
  r->noff--;
  r->ox = r->off_stack[r->noff][0];
  r->oy = r->off_stack[r->noff][1];
}

void ccord_push_clip(ccord_renderer *r, ccord_rect rc)
{
  if (r->nclips >= CCORD_CLIP_DEPTH) {
    CCORD_LOG_W("render: clip stack overflow, push ignored");
    return;
  }
  SDL_FRect f = snap(r, rc);
  int x0 = (int)f.x, y0 = (int)f.y, x1 = (int)(f.x + f.w), y1 = (int)(f.y + f.h);
  if (r->nclips > 0) {
    const SDL_Rect *p = &r->clips[r->nclips - 1];
    if (x0 < p->x) x0 = p->x;
    if (y0 < p->y) y0 = p->y;
    if (x1 > p->x + p->w) x1 = p->x + p->w;
    if (y1 > p->y + p->h) y1 = p->y + p->h;
  }
  SDL_Rect cr = {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
  if (cr.w == 0 || cr.h == 0) cr = (SDL_Rect){x0, y0, 0, 0};   // empty clip still clips everything: SDL treats w/h 0 as empty
  r->clips[r->nclips++] = cr;
  apply_clip(r);
}

void ccord_pop_clip(ccord_renderer *r)
{
  if (r->nclips <= 0) return;
  r->nclips--;
  apply_clip(r);
}

ccord_status ccord_render_screenshot(ccord_renderer *r, const char *path, ccord_error *err)
{
  if (!r || !path) return ccord_error_set(err, CCORD_ERR_INVALID, "screenshot: null argument");
  SDL_SetRenderClipRect(r->sdl, nullptr);
  SDL_Surface *surf = SDL_RenderReadPixels(r->sdl, nullptr);
  if (!surf) return ccord_error_set(err, CCORD_ERR_IO, "screenshot: cannot read pixels: %s", SDL_GetError());
  bool ok = SDL_SavePNG(surf, path);
  SDL_DestroySurface(surf);
  apply_clip(r);
  if (!ok) return ccord_error_set(err, CCORD_ERR_IO, "screenshot: cannot write %s: %s", path, SDL_GetError());
  return CCORD_OK;
}

// ---- image decoding (libpng, libjpeg, libwebp) ----

#define MAX_IMAGE_DIM 4096   // the largest texture the cache holds, per side

// What a still picture may be before it is refused: a phone photo, a long screenshot or a wallpaper is far larger
// than MAX_IMAGE_DIM and is shrunk to it after decoding, so the expanded view of an embedded image still shows it.
// The pixel cap bounds the decoder's memory (a PNG or WebP is decoded whole; a JPEG is shrunk while decoding).
#define MAX_SOURCE_DIM 16384
#define MAX_SOURCE_PIXELS ((uint64_t)64 * 1000 * 1000)
#define MAX_JPEG_SOURCE_PIXELS ((uint64_t)400 * 1000 * 1000)

// Shrinks *rgba (w x h) with an area average so that neither side exceeds MAX_IMAGE_DIM, keeping its shape. A picture
// that already fits is left as it is.
static ccord_status shrink_rgba_to_fit(uint8_t **rgba, int *w, int *h, ccord_error *err)
{
  int sw = *w, sh = *h;
  if (sw <= MAX_IMAGE_DIM && sh <= MAX_IMAGE_DIM) return CCORD_OK;
  int big = sw > sh ? sw : sh;
  int dw = (int)(((int64_t)sw * MAX_IMAGE_DIM + big / 2) / big);
  int dh = (int)(((int64_t)sh * MAX_IMAGE_DIM + big / 2) / big);
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

static ccord_status decode_png_rgba(const uint8_t *data, size_t len, uint8_t **out_rgba, int *out_w, int *out_h,
                                    ccord_error *err)
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
  ccord_status sst = shrink_rgba_to_fit(&rgba, &pw, &ph, err);
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

static ccord_status decode_jpeg_rgba(const uint8_t *data, size_t len, uint8_t **out_rgba, int *out_w, int *out_h,
                                     ccord_error *err)
{
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
  cinfo.scale_num = 1;
  cinfo.scale_denom = 1;
  while (cinfo.scale_denom < 8u &&
         (cinfo.image_width / cinfo.scale_denom > MAX_IMAGE_DIM || cinfo.image_height / cinfo.scale_denom > MAX_IMAGE_DIM)) {
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
  ccord_status sst = shrink_rgba_to_fit(&jctx.rgba, &w, &h, err);
  if (sst != CCORD_OK) {
    ccord_free(jctx.rgba);
    return sst;
  }
  *out_w = w;
  *out_h = h;
  *out_rgba = jctx.rgba;
  return CCORD_OK;
}

static ccord_status decode_webp_rgba(const uint8_t *data, size_t len, uint8_t **out_rgba, int *out_w, int *out_h,
                                     ccord_error *err)
{
  int w = 0, h = 0;
  if (!WebPGetInfo(data, len, &w, &h) || w <= 0 || h <= 0 || w > MAX_SOURCE_DIM || h > MAX_SOURCE_DIM ||
      (uint64_t)w * (uint64_t)h > MAX_SOURCE_PIXELS) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "webp: invalid header or dimensions");
  }
  size_t sz = (size_t)w * (size_t)h * 4u;
  uint8_t *rgba = ccord_malloc(sz);
  if (!rgba) return ccord_error_set(err, CCORD_ERR_NOMEM, "webp: out of memory");
  if (!WebPDecodeRGBAInto(data, len, rgba, sz, w * 4)) {
    ccord_free(rgba);
    return ccord_error_set(err, CCORD_ERR_PARSE, "webp: decode failed");
  }
  ccord_status sst = shrink_rgba_to_fit(&rgba, &w, &h, err);
  if (sst != CCORD_OK) {
    ccord_free(rgba);
    return sst;
  }
  *out_w = w;
  *out_h = h;
  *out_rgba = rgba;
  return CCORD_OK;
}

// First frame of anything the ffmpeg path understands (GIF, APNG, animated WebP, BMP, TIFF, ...).
static ccord_status decode_ffmpeg_first_frame(const uint8_t *data, size_t len, uint8_t **out_rgba, int *out_w,
                                              int *out_h, ccord_error *err)
{
  ccord_anim a;
  ccord_status st = ccord_anim_decode(data, len, MAX_IMAGE_DIM, 1, &a, err);
  if (st != CCORD_OK) return st;
  *out_rgba = a.frames[0];
  *out_w = a.w;
  *out_h = a.h;
  a.frames[0] = nullptr;   // ownership moved to the caller
  ccord_anim_free(&a);
  return CCORD_OK;
}

ccord_status ccord_image_decode_rgba(const uint8_t *data, size_t len, uint8_t **out_rgba, int *out_w, int *out_h,
                                     ccord_error *err)
{
  ccord_error_clear(err);
  if (out_rgba) *out_rgba = nullptr;
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (!data || len < 4 || !out_rgba || !out_w || !out_h) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "image decode: invalid argument");
  }
  if (len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
    return decode_png_rgba(data, len, out_rgba, out_w, out_h, err);
  }
  if (len >= 3 && data[0] == 0xffu && data[1] == 0xd8u && data[2] == 0xffu) {
    return decode_jpeg_rgba(data, len, out_rgba, out_w, out_h, err);
  }
  if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
    // libwebp's simple decoder rejects animated WebP; the ffmpeg path yields its first frame.
    if (decode_webp_rgba(data, len, out_rgba, out_w, out_h, err) == CCORD_OK) return CCORD_OK;
    return decode_ffmpeg_first_frame(data, len, out_rgba, out_w, out_h, err);
  }
  return decode_ffmpeg_first_frame(data, len, out_rgba, out_w, out_h, err);
}

// ---- keyed LRU SDL_Texture cache ----

static uint32_t tex_hash_key(const char *key)
{
  uint32_t h = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
    h ^= *p;
    h *= 16777619u;
  }
  return h;
}

static void tex_cache_init(ccord_renderer *r)
{
  memset(r->tex_hash, 0, sizeof r->tex_hash);
  r->tex_lru_head = -1;
  r->tex_lru_tail = -1;
  r->tex_free_head = -1;
  r->tex_hwm = 0;
  r->ntexs = 0;
  r->tex_bytes_total = 0;
  r->anim_bytes_total = 0;
  r->anim_dec_count = 0;
  r->stream_dec_count = 0;
  r->dec_cursor = 0;
}

static void tex_hash_insert(ccord_renderer *r, int slot, uint32_t h)
{
  const uint32_t mask = CCORD_TEX_HASH_MASK;
  uint32_t pos = h & mask;
  while (r->tex_hash[pos] != 0) pos = (pos + 1u) & mask;
  r->tex_hash[pos] = (uint16_t)(slot + 1);
}

static void tex_hash_remove(ccord_renderer *r, int slot, uint32_t h)
{
  const uint32_t mask = CCORD_TEX_HASH_MASK;
  const uint16_t target = (uint16_t)(slot + 1);
  uint32_t pos = h & mask;
  while (r->tex_hash[pos] != 0) {
    if (r->tex_hash[pos] == target) break;
    pos = (pos + 1u) & mask;
  }
  if (r->tex_hash[pos] == 0) return;
  // Backward-shift deletion: keeps probe chains compact with zero tombstones.
  uint32_t hole = pos;
  uint32_t cur = (hole + 1u) & mask;
  while (r->tex_hash[cur] != 0) {
    int cslot = (int)r->tex_hash[cur] - 1;
    uint32_t ideal = r->texs[cslot].key_hash & mask;
    if (((hole - ideal) & mask) < ((cur - ideal) & mask)) {
      r->tex_hash[hole] = r->tex_hash[cur];
      hole = cur;
    }
    cur = (cur + 1u) & mask;
  }
  r->tex_hash[hole] = 0;
}

static int tex_lookup_slot(const ccord_renderer *r, const char *key, uint32_t *out_hash)
{
  if (!r || !key || !key[0]) return -1;
  uint32_t h = tex_hash_key(key);
  if (out_hash) *out_hash = h;
  const uint32_t mask = CCORD_TEX_HASH_MASK;
  uint32_t pos = h & mask;
  for (;;) {
    uint16_t v = r->tex_hash[pos];
    if (v == 0) return -1;
    int slot = (int)v - 1;
    const tex_entry *e = &r->texs[slot];
    if (e->key_hash == h && e->key && strcmp(e->key, key) == 0) return slot;
    pos = (pos + 1u) & mask;
  }
}

static void tex_lru_unlink(ccord_renderer *r, int slot)
{
  tex_entry *e = &r->texs[slot];
  if (e->lru_prev != -1) r->texs[e->lru_prev].lru_next = e->lru_next;
  else r->tex_lru_head = e->lru_next;
  if (e->lru_next != -1) r->texs[e->lru_next].lru_prev = e->lru_prev;
  else r->tex_lru_tail = e->lru_prev;
  e->lru_prev = -1;
  e->lru_next = -1;
}

static void tex_lru_push_front(ccord_renderer *r, int slot)
{
  tex_entry *e = &r->texs[slot];
  e->lru_prev = -1;
  e->lru_next = r->tex_lru_head;
  if (r->tex_lru_head != -1) r->texs[r->tex_lru_head].lru_prev = (int16_t)slot;
  else r->tex_lru_tail = (int16_t)slot;
  r->tex_lru_head = (int16_t)slot;
}

static void tex_lru_touch(ccord_renderer *r, int slot)
{
  if (r->tex_lru_head == (int16_t)slot) return;
  tex_lru_unlink(r, slot);
  tex_lru_push_front(r, slot);
}

static tex_entry *tex_find(ccord_renderer *r, const char *key)
{
  int slot = tex_lookup_slot(r, key, nullptr);
  if (slot < 0) return nullptr;
  r->texs[slot].tick = ++r->tex_tick;
  tex_lru_touch(r, slot);
  return &r->texs[slot];
}

static void tex_set_dec(ccord_renderer *r, tex_entry *e, ccord_av_stream *dec, bool streaming)
{
  e->dec = dec;
  e->streaming = streaming;
  if (dec) {
    if (streaming) r->stream_dec_count++;
    else r->anim_dec_count++;
  }
}

static void tex_clear_dec(ccord_renderer *r, tex_entry *e)
{
  if (e->dec) {
    if (e->streaming) {
      if (r->stream_dec_count > 0) r->stream_dec_count--;
    } else {
      if (r->anim_dec_count > 0) r->anim_dec_count--;
    }
    ccord_av_stream_close(e->dec);
    e->dec = nullptr;
  }
  e->streaming = false;
}

// Releases everything animated about an entry (decoded frames, the open decoder); the texture stays.
static void tex_drop_anim(ccord_renderer *r, tex_entry *e)
{
  tex_clear_dec(r, e);
  if (e->has_anim || e->anim.frames || e->anim.delay_ms) ccord_anim_free(&e->anim);
  r->anim_bytes_total -= e->anim_bytes <= r->anim_bytes_total ? e->anim_bytes : r->anim_bytes_total;
  e->anim = (ccord_anim){0};
  e->anim_bytes = 0;
  e->anim_cap = 0;
  e->has_anim = false;
  e->anim_cur_frame = 0;
}

static int tex_alloc_slot(ccord_renderer *r)
{
  if (r->tex_free_head != -1) {
    int slot = r->tex_free_head;
    r->tex_free_head = r->texs[slot].lru_next;
    r->texs[slot] = (tex_entry){.lru_prev = -1, .lru_next = -1};
    return slot;
  }
  if (r->tex_hwm < CCORD_TEX_CACHE_SIZE) {
    int slot = r->tex_hwm++;
    r->texs[slot] = (tex_entry){.lru_prev = -1, .lru_next = -1};
    return slot;
  }
  return -1;
}

static void tex_free_slot(ccord_renderer *r, int slot)
{
  tex_entry *e = &r->texs[slot];
  if (!e->key) return;
  tex_hash_remove(r, slot, e->key_hash);
  tex_lru_unlink(r, slot);
  tex_drop_anim(r, e);
  if (e->tex) SDL_DestroyTexture(e->tex);
  r->tex_bytes_total -= e->tex_bytes <= r->tex_bytes_total ? e->tex_bytes : r->tex_bytes_total;
  ccord_free(e->key);
  memset(e, 0, sizeof *e);
  e->lru_prev = -1;
  e->lru_next = r->tex_free_head;
  r->tex_free_head = (int16_t)slot;
  r->ntexs--;
}

static void tex_enforce_vram_budget(ccord_renderer *r, int protect_slot)
{
  // Evict oldest LRU textures first (naturally prefers entries not drawn on the current frame).
  while (r->tex_bytes_total > RENDER_TEX_VRAM_BUDGET && r->ntexs > 1) {
    int victim = r->tex_lru_tail;
    if (victim == protect_slot) victim = r->texs[victim].lru_prev;
    if (victim == -1) break;
    tex_free_slot(r, victim);
    r->tex_evictions++;
  }
}

static bool tex_update_pixels(SDL_Texture *tex, bool streaming, const uint8_t *rgba, int width, int height)
{
  const int row_bytes = width * 4;
  if (streaming) {
    void *pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(tex, nullptr, &pixels, &pitch)) {
      if (pitch == row_bytes) {
        memcpy(pixels, rgba, (size_t)row_bytes * (size_t)height);
      } else {
        uint8_t *dst = (uint8_t *)pixels;
        for (int y = 0; y < height; y++) {
          memcpy(dst + (size_t)y * (size_t)pitch, rgba + (size_t)y * (size_t)row_bytes, (size_t)row_bytes);
        }
      }
      SDL_UnlockTexture(tex);
      return true;
    }
  }
  return SDL_UpdateTexture(tex, nullptr, rgba, row_bytes);
}

static ccord_status tex_upload_rgba(ccord_renderer *r, const char *key, const uint8_t *rgba, int width, int height,
                                    bool streaming, ccord_error *err)
{
  ccord_error_clear(err);
  if (!r || !key || !key[0] || !rgba || width <= 0 || height <= 0 || width > MAX_IMAGE_DIM || height > MAX_IMAGE_DIM) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "upload rgba: invalid argument");
  }
  uint32_t kh = 0;
  int existing_slot = tex_lookup_slot(r, key, &kh);
  tex_entry *existing = nullptr;
  if (existing_slot >= 0) {
    existing = &r->texs[existing_slot];
    existing->tick = ++r->tex_tick;
    tex_lru_touch(r, existing_slot);
  }
  if (existing && existing->w == width && existing->h == height && existing->tex) {
    tex_drop_anim(r, existing);
    // Repeated same-key uploads (video frames, animated media) upgrade once to STREAMING for lock/copy/unlock.
    if (!existing->tex_streaming) {
      SDL_Texture *stex = SDL_CreateTexture(r->sdl, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
      if (stex) {
        SDL_SetTextureBlendMode(stex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(stex, SDL_SCALEMODE_LINEAR);
        SDL_DestroyTexture(existing->tex);
        existing->tex = stex;
        existing->tex_streaming = true;
      }
    }
    if (!tex_update_pixels(existing->tex, existing->tex_streaming, rgba, width, height)) {
      return ccord_error_set(err, CCORD_ERR_IO, "upload rgba: texture update failed: %s", SDL_GetError());
    }
    return CCORD_OK;
  }
  const bool want_streaming = streaming || existing != nullptr;
  const SDL_TextureAccess access = want_streaming ? SDL_TEXTUREACCESS_STREAMING : SDL_TEXTUREACCESS_STATIC;
  SDL_Texture *tex = SDL_CreateTexture(r->sdl, SDL_PIXELFORMAT_RGBA32, access, width, height);
  if (!tex) {
    return ccord_error_set(err, CCORD_ERR_IO, "upload rgba: SDL_CreateTexture failed: %s", SDL_GetError());
  }
  if (!tex_update_pixels(tex, want_streaming, rgba, width, height)) {
    SDL_DestroyTexture(tex);
    return ccord_error_set(err, CCORD_ERR_IO, "upload rgba: texture update failed: %s", SDL_GetError());
  }
  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
  const size_t new_bytes = (size_t)width * (size_t)height * 4u;

  if (existing) {
    tex_drop_anim(r, existing);
    if (existing->tex) SDL_DestroyTexture(existing->tex);
    r->tex_bytes_total -= existing->tex_bytes <= r->tex_bytes_total ? existing->tex_bytes : r->tex_bytes_total;
    existing->tex = tex;
    existing->tex_streaming = want_streaming;
    existing->w = width;
    existing->h = height;
    existing->tex_bytes = new_bytes;
    r->tex_bytes_total += new_bytes;
    tex_enforce_vram_budget(r, existing_slot);
    return CCORD_OK;
  }

  char *kcopy = ccord_strndup(key, strlen(key));
  if (!kcopy) {
    SDL_DestroyTexture(tex);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "upload rgba: out of memory");
  }
  int cap = r->tex_cap > 0 && r->tex_cap <= CCORD_TEX_CACHE_SIZE ? r->tex_cap : CCORD_TEX_CACHE_SIZE;
  while (r->ntexs >= cap && r->tex_lru_tail != -1) {
    tex_free_slot(r, r->tex_lru_tail);
    r->tex_evictions++;
  }
  int slot = tex_alloc_slot(r);
  if (slot < 0) {
    SDL_DestroyTexture(tex);
    ccord_free(kcopy);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "upload rgba: texture cache full");
  }
  r->texs[slot] = (tex_entry){
    .key = kcopy,
    .key_hash = kh,
    .tex = tex,
    .w = width,
    .h = height,
    .tex_bytes = new_bytes,
    .tick = ++r->tex_tick,
    .lru_prev = -1,
    .lru_next = -1,
    .tex_streaming = want_streaming,
  };
  tex_hash_insert(r, slot, kh);
  tex_lru_push_front(r, slot);
  r->ntexs++;
  r->tex_bytes_total += new_bytes;
  tex_enforce_vram_budget(r, slot);
  return CCORD_OK;
}

ccord_status ccord_render_upload_rgba(ccord_renderer *r, const char *key, const uint8_t *rgba, int width, int height,
                                      ccord_error *err)
{
  return tex_upload_rgba(r, key, rgba, width, height, false, err);
}

// ---- progressive animations ----

static uint64_t tex_clock_ms(const ccord_renderer *r)
{
  return (r && r->anim_now_ms) ? r->anim_now_ms : SDL_GetTicks();
}

// MP4/MOV ("ftyp" box) or WebM/Matroska (EBML magic).
static bool sniff_video(const uint8_t *d, size_t len)
{
  if (len >= 12 && memcmp(d + 4, "ftyp", 4) == 0) return true;
  return len >= 4 && d[0] == 0x1Au && d[1] == 0x45u && d[2] == 0xDFu && d[3] == 0xA3u;
}

static uint32_t tex_frame_delay(const tex_entry *e, uint32_t dur_ms)
{
  if (e->dec_video) return dur_ms == 0 ? 33u : (dur_ms < 10u ? 10u : dur_ms);
  if (dur_ms <= 10u) return 100u;   // browsers play a GIF frame of <= 10 ms as 100 ms
  return dur_ms < 20u ? 20u : dur_ms;
}

static void tex_evict_offscreen_anims(ccord_renderer *r, size_t max_bytes, int protect_slot)
{
  // Walk from LRU tail (oldest first); stop as soon as entries drawn in the last two frames are reached.
  for (int slot = r->tex_lru_tail; slot != -1 && r->anim_bytes_total > max_bytes;) {
    int prev = r->texs[slot].lru_prev;
    const tex_entry *e = &r->texs[slot];
    if (e->tick > r->prev_frame_tick) break;
    if (slot != protect_slot && e->anim_bytes > 0) {
      tex_free_slot(r, slot);
      r->tex_evictions++;
    }
    slot = prev;
  }
}

static bool tex_anim_append(ccord_renderer *r, tex_entry *e, const uint8_t *rgba, uint32_t delay_ms)
{
  size_t fbytes = (size_t)e->w * (size_t)e->h * 4u;
  if (e->anim.frame_count == e->anim_cap) {
    size_t ncap = e->anim_cap ? e->anim_cap * 2u : 8u;
    uint8_t **nf = ccord_realloc_array(e->anim.frames, ncap, sizeof *nf);
    if (!nf) return false;
    e->anim.frames = nf;
    uint32_t *nd = ccord_realloc_array(e->anim.delay_ms, ncap, sizeof *nd);
    if (!nd) return false;
    e->anim.delay_ms = nd;
    e->anim_cap = ncap;
  }
  uint8_t *px = ccord_malloc(fbytes);
  if (!px) return false;
  memcpy(px, rgba, fbytes);
  e->anim.frames[e->anim.frame_count] = px;
  e->anim.delay_ms[e->anim.frame_count] = delay_ms;
  e->anim.frame_count++;
  uint64_t total = (uint64_t)e->anim.total_ms + delay_ms;
  e->anim.total_ms = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
  e->anim_bytes += fbytes;
  r->anim_bytes_total += fbytes;
  return true;
}

// Decodes more frames of a loading animation for up to budget_ms. Ends the load at the end of the stream, or
// turns the entry into a streaming one when the decoded frames would outgrow its budget.
static void tex_decode_more(ccord_renderer *r, tex_entry *e, uint64_t budget_ms)
{
  uint64_t t0 = SDL_GetTicks();
  size_t fbytes = (size_t)e->w * (size_t)e->h * 4u;
  const int self_slot = (int)(e - r->texs);
  while (e->dec && !e->streaming) {
    const uint8_t *px = nullptr;
    uint32_t pts = 0, dur = 0;
    if (!ccord_av_stream_next_frame(e->dec, &px, &pts, &dur)) {
      tex_clear_dec(r, e);
      if (e->anim.frame_count <= 1) {
        tex_drop_anim(r, e);   // a single frame is just an image
      } else {
        uint64_t now = tex_clock_ms(r);
        if (now - e->anim_start_ms >= e->anim.total_ms) e->anim_start_ms = now;   // was holding the last frame
      }
      return;
    }
    uint32_t delay = tex_frame_delay(e, dur);
    bool over_budget = e->anim_bytes + fbytes > e->anim_budget;
    if (!over_budget && r->anim_bytes_total + fbytes > RENDER_ANIM_TOTAL_BUDGET) {
      size_t limit = RENDER_ANIM_TOTAL_BUDGET >= fbytes ? RENDER_ANIM_TOTAL_BUDGET - fbytes : 0;
      tex_evict_offscreen_anims(r, limit, self_slot);
      over_budget = r->anim_bytes_total + fbytes > RENDER_ANIM_TOTAL_BUDGET;
    }
    if (over_budget || !tex_anim_append(r, e, px, delay)) {
      // Too long (or global budget saturated) to keep decoded: show frames straight from the decoder from here on.
      ccord_av_stream *dec = e->dec;
      bool was_video = e->dec_video;
      e->dec = nullptr;
      if (r->anim_dec_count > 0) r->anim_dec_count--;
      tex_drop_anim(r, e);
      e->dec_video = was_video;
      tex_set_dec(r, e, dec, true);
      tex_update_pixels(e->tex, e->tex_streaming, px, e->w, e->h);
      e->stream_due_ms = tex_clock_ms(r) + delay;
      return;
    }
    if (SDL_GetTicks() - t0 >= budget_ms) return;
  }
}

ccord_status ccord_render_upload_media(ccord_renderer *r, const char *key, const uint8_t *data, size_t len,
                                       const ccord_media_opts *opts, int *out_w, int *out_h, ccord_error *err)
{
  ccord_error_clear(err);
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (!r || !key || !key[0] || !data || len == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "upload image: invalid argument");
  }
  int max_dim = opts && opts->max_dim > 0 ? opts->max_dim : RENDER_ANIM_DEFAULT_DIM;
  if (max_dim > MAX_IMAGE_DIM) max_dim = MAX_IMAGE_DIM;
  size_t budget = opts && opts->anim_budget > 0 ? opts->anim_budget : RENDER_ANIM_DEFAULT_BUDGET;
  bool video = opts && opts->allow_video && sniff_video(data, len);
  if (video || ccord_anim_sniff(data, len)) {
    unsigned flags = video ? (unsigned)(CCORD_AV_MEDIA | CCORD_AV_NO_AUDIO) : (unsigned)CCORD_AV_IMAGES;
    ccord_av_stream *dec = ccord_av_stream_open(data, len, max_dim, flags, err);
    const uint8_t *px = nullptr;
    uint32_t pts = 0, dur = 0;
    if (dec && ccord_av_stream_next_frame(dec, &px, &pts, &dur)) {
      const ccord_av_info *info = ccord_av_stream_info(dec);
      ccord_status st = tex_upload_rgba(r, key, px, info->w, info->h, true, err);
      tex_entry *e = st == CCORD_OK ? tex_find(r, key) : nullptr;
      if (!e) {
        ccord_av_stream_close(dec);
        return st != CCORD_OK ? st : ccord_error_set(err, CCORD_ERR_INTERNAL, "upload image: texture lost");
      }
      if (out_w) *out_w = e->w;
      if (out_h) *out_h = e->h;
      tex_set_dec(r, e, dec, false);
      e->dec_video = video;
      e->anim_budget = budget;
      e->anim = (ccord_anim){.w = e->w, .h = e->h};
      e->anim_cur_frame = 0;
      e->anim_start_ms = tex_clock_ms(r);
      if (!tex_anim_append(r, e, px, tex_frame_delay(e, dur))) {
        tex_drop_anim(r, e);   // out of memory: keep the first frame as a still
        return CCORD_OK;
      }
      e->has_anim = true;
      tex_decode_more(r, e, RENDER_ANIM_INLINE_MS);   // small animations are complete before this returns
      return CCORD_OK;
    }
    if (dec) ccord_av_stream_close(dec);
    if (video) {
      if (err && err->code == CCORD_OK) (void)ccord_error_set(err, CCORD_ERR_PARSE, "upload image: no video frames");
      return err ? err->code : CCORD_ERR_PARSE;
    }
    ccord_error_clear(err);   // not really animated, or damaged: try it as a still image
  }
  uint8_t *rgba = nullptr;
  int w = 0, h = 0;
  ccord_status st = ccord_image_decode_rgba(data, len, &rgba, &w, &h, err);
  if (st != CCORD_OK) return st;
  st = tex_upload_rgba(r, key, rgba, w, h, false, err);
  ccord_free(rgba);
  if (st == CCORD_OK) {
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
  }
  return st;
}

ccord_status ccord_render_upload_image(ccord_renderer *r, const char *key, const uint8_t *data, size_t len, int *out_w,
                                       int *out_h, ccord_error *err)
{
  return ccord_render_upload_media(r, key, data, len, nullptr, out_w, out_h, err);
}

bool ccord_render_pump_media(ccord_renderer *r, uint32_t budget_ms)
{
  if (!r) return false;
  bool pending = false;
  if (r->anim_dec_count > 0 && r->tex_hwm > 0) {
    uint64_t t0 = SDL_GetTicks();
    int n = r->tex_hwm;
    int start = r->dec_cursor % n;
    for (int k = 0; k < n; k++) {
      int i = (start + k) % n;
      tex_entry *e = &r->texs[i];
      if (!e->key || !e->dec || e->streaming) continue;
      uint64_t used = SDL_GetTicks() - t0;
      if (used >= budget_ms) {
        pending = true;
        r->dec_cursor = i;   // whoever did not get a turn goes first next time
        break;
      }
      tex_decode_more(r, e, budget_ms - used);
      if (e->dec && !e->streaming) pending = true;
    }
  }
  // Keep decoded animations within the global budget: drop the least recently drawn ones that were not on
  // screen in the last two frames. They decode again from the cached bytes if they scroll back.
  if (r->anim_bytes_total > RENDER_ANIM_TOTAL_BUDGET) {
    tex_evict_offscreen_anims(r, RENDER_ANIM_TOTAL_BUDGET, -1);
  }
  // Close stale/excess off-screen streaming FFmpeg decoders so scrolled-away large GIFs/videos don't pin RAM.
  if (r->stream_dec_count > 0) {
    uint64_t now_ms = ccord_clock_ms();
    size_t stream_bytes = 0;
    for (int slot = r->tex_lru_head; slot != -1; slot = r->texs[slot].lru_next) {
      const tex_entry *e = &r->texs[slot];
      if (e->streaming && e->dec) stream_bytes += av_stream_mem_bytes(e->dec);
    }
    const size_t stream_budget = RENDER_ANIM_TOTAL_BUDGET / 2u;
    for (int slot = r->tex_lru_tail; slot != -1 && r->stream_dec_count > 0;) {
      int prev = r->texs[slot].lru_prev;
      const tex_entry *e = &r->texs[slot];
      if (e->tick > r->prev_frame_tick) break;
      if (e->streaming && e->dec) {
        uint64_t last_ms = av_stream_last_used_ms(e->dec);
        size_t mb = av_stream_mem_bytes(e->dec);
        bool stale = now_ms >= last_ms && (now_ms - last_ms) >= RENDER_STREAM_IDLE_MS;
        if (stale || r->stream_dec_count > RENDER_STREAM_MAX_OPEN || stream_bytes > stream_budget) {
          stream_bytes -= mb <= stream_bytes ? mb : stream_bytes;
          tex_free_slot(r, slot);
          r->tex_evictions++;
        }
      }
      slot = prev;
    }
  }
  return pending;
}

bool ccord_render_take_anim_activity(ccord_renderer *r)
{
  if (!r) return false;
  bool a = r->anim_activity;
  r->anim_activity = false;
  return a;
}

bool ccord_render_has_texture(ccord_renderer *r, const char *key, int *out_w, int *out_h)
{
  tex_entry *e = tex_find(r, key);
  if (!e) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    return false;
  }
  if (out_w) *out_w = e->w;
  if (out_h) *out_h = e->h;
  return true;
}

void ccord_render_evict_texture(ccord_renderer *r, const char *key)
{
  int slot = tex_lookup_slot(r, key, nullptr);
  if (slot >= 0) tex_free_slot(r, slot);
}

void ccord_render_set_texture_cache_cap(ccord_renderer *r, int cap)
{
  if (!r) return;
  if (cap < 1) cap = 1;
  if (cap > CCORD_TEX_CACHE_SIZE) cap = CCORD_TEX_CACHE_SIZE;
  r->tex_cap = cap;
  while (r->ntexs > r->tex_cap && r->tex_lru_tail != -1) {
    tex_free_slot(r, r->tex_lru_tail);
    r->tex_evictions++;
  }
}

int ccord_render_texture_cache_count(const ccord_renderer *r)
{
  return r ? r->ntexs : 0;
}

uint64_t ccord_render_texture_evictions(const ccord_renderer *r)
{
  return r ? r->tex_evictions : 0;
}

void ccord_render_set_anim_clock(ccord_renderer *r, uint64_t now_ms)
{
  if (r) r->anim_now_ms = now_ms;
}

bool ccord_render_texture_is_animated(ccord_renderer *r, const char *key, size_t *out_cur_frame, size_t *out_frame_count)
{
  if (out_cur_frame) *out_cur_frame = 0;
  if (out_frame_count) *out_frame_count = 0;
  tex_entry *e = tex_find(r, key);
  if (e && e->streaming) return true;   // frames are not kept: the counts stay 0
  if (!e || !e->has_anim || e->anim.frame_count <= 1) return false;
  if (out_cur_frame) *out_cur_frame = e->anim_cur_frame;
  if (out_frame_count) *out_frame_count = e->anim.frame_count;
  return true;
}

static void tex_advance_anim(ccord_renderer *r, tex_entry *e, bool animate)
{
  if (!e || !e->tex) return;
  uint64_t now = tex_clock_ms(r);
  if (e->streaming) {
    if (e->dec) av_stream_touch(e->dec, ccord_clock_ms());
    if (!animate || !e->dec) return;
    r->anim_activity = true;
    if (now < e->stream_due_ms) return;
    const uint8_t *px = nullptr;
    uint32_t pts = 0, dur = 0;
    if (!ccord_av_stream_next_frame(e->dec, &px, &pts, &dur)) {
      ccord_av_stream_seek(e->dec, 0);   // loop
      if (!ccord_av_stream_next_frame(e->dec, &px, &pts, &dur)) {
        tex_drop_anim(r, e);   // cannot restart: the last frame stays as a still
        return;
      }
    }
    tex_update_pixels(e->tex, e->tex_streaming, px, e->w, e->h);
    // Pace from the schedule, not from "now", so a late frame does not slow the clip; resync after a long gap.
    uint64_t base = now - e->stream_due_ms > 250u ? now : e->stream_due_ms;
    e->stream_due_ms = base + tex_frame_delay(e, dur);
    return;
  }
  if (!e->has_anim) return;
  if (e->anim.frame_count <= 1) {
    if (e->dec && animate) r->anim_activity = true;   // more frames are on their way
    return;
  }
  if (!animate) {
    e->anim_start_ms = now;
    if (e->anim_cur_frame != 0 && e->anim.frames[0]) {
      tex_update_pixels(e->tex, e->tex_streaming, e->anim.frames[0], e->w, e->h);
      e->anim_cur_frame = 0;
    }
    return;
  }
  r->anim_activity = true;
  uint64_t elapsed = now >= e->anim_start_ms ? (now - e->anim_start_ms) : 0;
  // While the tail is still decoding, hold the newest frame instead of looping over a partial clip.
  size_t idx = (e->dec && elapsed >= e->anim.total_ms) ? e->anim.frame_count - 1u
                                                       : ccord_anim_frame_at(&e->anim, elapsed);
  if (idx < e->anim.frame_count && idx != e->anim_cur_frame && e->anim.frames[idx]) {
    tex_update_pixels(e->tex, e->tex_streaming, e->anim.frames[idx], e->w, e->h);
    e->anim_cur_frame = idx;
  }
}

// Draws entry `e` into dst showing the normalised source rectangle uv = {u0, v0, u1, v1}.
static void tex_draw(ccord_renderer *r, tex_entry *e, ccord_rect dst, float radius, uint8_t alpha, const float uv[4])
{
  SDL_FRect fr = snap(r, dst);
  if (fr.w <= 0.0f || fr.h <= 0.0f) return;
  float du = uv[2] - uv[0], dv = uv[3] - uv[1];
  float maxr = floorf((fr.w < fr.h ? fr.w : fr.h) * 0.5f);
  float rr = roundf(radius * r->scale);
  if (rr > maxr) rr = maxr;
  if (rr <= 0.5f) {
    SDL_FRect src = {uv[0] * (float)e->w, uv[1] * (float)e->h, du * (float)e->w, dv * (float)e->h};
    SDL_SetTextureColorMod(e->tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(e->tex, ccord_fx_alpha(r, alpha));
    SDL_RenderTexture(r->sdl, e->tex, &src, &fr);
    return;
  }

  enum { CORNER_SEGS = 12, PERIM_VERTS = 4 * (CORNER_SEGS + 1), TOTAL_VERTS = 1 + PERIM_VERTS };
  // Precomputed unit-circle (cos, sin) for 4 corners x (CORNER_SEGS + 1 = 13) angles = 52 entries (7.5 deg step).
  static const float k_corner_lut[PERIM_VERTS][2] = {
    // corner 0: -90..0 deg
    {0.00000000f, -1.00000000f}, {0.13052619f, -0.99144486f}, {0.25881905f, -0.96592583f}, {0.38268343f, -0.92387953f},
    {0.50000000f, -0.86602540f}, {0.60876143f, -0.79335334f}, {0.70710678f, -0.70710678f}, {0.79335334f, -0.60876143f},
    {0.86602540f, -0.50000000f}, {0.92387953f, -0.38268343f}, {0.96592583f, -0.25881905f}, {0.99144486f, -0.13052619f},
    {1.00000000f, 0.00000000f},
    // corner 1: 0..90 deg
    {1.00000000f, 0.00000000f}, {0.99144486f, 0.13052619f}, {0.96592583f, 0.25881905f}, {0.92387953f, 0.38268343f},
    {0.86602540f, 0.50000000f}, {0.79335334f, 0.60876143f}, {0.70710678f, 0.70710678f}, {0.60876143f, 0.79335334f},
    {0.50000000f, 0.86602540f}, {0.38268343f, 0.92387953f}, {0.25881905f, 0.96592583f}, {0.13052619f, 0.99144486f},
    {0.00000000f, 1.00000000f},
    // corner 2: 90..180 deg
    {0.00000000f, 1.00000000f}, {-0.13052619f, 0.99144486f}, {-0.25881905f, 0.96592583f}, {-0.38268343f, 0.92387953f},
    {-0.50000000f, 0.86602540f}, {-0.60876143f, 0.79335334f}, {-0.70710678f, 0.70710678f}, {-0.79335334f, 0.60876143f},
    {-0.86602540f, 0.50000000f}, {-0.92387953f, 0.38268343f}, {-0.96592583f, 0.25881905f}, {-0.99144486f, 0.13052619f},
    {-1.00000000f, 0.00000000f},
    // corner 3: 180..270 deg
    {-1.00000000f, 0.00000000f}, {-0.99144486f, -0.13052619f}, {-0.96592583f, -0.25881905f}, {-0.92387953f, -0.38268343f},
    {-0.86602540f, -0.50000000f}, {-0.79335334f, -0.60876143f}, {-0.70710678f, -0.70710678f}, {-0.60876143f, -0.79335334f},
    {-0.50000000f, -0.86602540f}, {-0.38268343f, -0.92387953f}, {-0.25881905f, -0.96592583f}, {-0.13052619f, -0.99144486f},
    {0.00000000f, -1.00000000f},
  };
  static const int k_rrect_indices[PERIM_VERTS * 3] = {
    0,  1,  2,  0,  2,  3,  0,  3,  4,  0,  4,  5,  0,  5,  6,  0,  6,  7,  0,  7,  8,  0,  8,  9,  0,  9, 10,
    0, 10, 11,  0, 11, 12,  0, 12, 13,  0, 13, 14,  0, 14, 15,  0, 15, 16,  0, 16, 17,  0, 17, 18,  0, 18, 19,
    0, 19, 20,  0, 20, 21,  0, 21, 22,  0, 22, 23,  0, 23, 24,  0, 24, 25,  0, 25, 26,  0, 26, 27,  0, 27, 28,
    0, 28, 29,  0, 29, 30,  0, 30, 31,  0, 31, 32,  0, 32, 33,  0, 33, 34,  0, 34, 35,  0, 35, 36,  0, 36, 37,
    0, 37, 38,  0, 38, 39,  0, 39, 40,  0, 40, 41,  0, 41, 42,  0, 42, 43,  0, 43, 44,  0, 44, 45,  0, 45, 46,
    0, 46, 47,  0, 47, 48,  0, 48, 49,  0, 49, 50,  0, 50, 51,  0, 51, 52,  0, 52,  1,
  };

  SDL_Vertex verts[TOTAL_VERTS];
  SDL_FColor col = {1.0f, 1.0f, 1.0f, (float)alpha / 255.0f * r->alpha};

  float cx = fr.x + fr.w * 0.5f;
  float cy = fr.y + fr.h * 0.5f;
  verts[0] = (SDL_Vertex){.position = {cx, cy}, .color = col, .tex_coord = {uv[0] + du * 0.5f, uv[1] + dv * 0.5f}};

  const float corner_cx[4] = {fr.x + fr.w - rr, fr.x + fr.w - rr, fr.x + rr, fr.x + rr};
  const float corner_cy[4] = {fr.y + rr, fr.y + fr.h - rr, fr.y + fr.h - rr, fr.y + rr};
  const float inv_w = 1.0f / fr.w;
  const float inv_h = 1.0f / fr.h;

  int li = 0;
  for (int c = 0; c < 4; c++) {
    const float ccx = corner_cx[c];
    const float ccy = corner_cy[c];
    for (int sgi = 0; sgi <= CORNER_SEGS; sgi++, li++) {
      float px = ccx + k_corner_lut[li][0] * rr;
      float py = ccy + k_corner_lut[li][1] * rr;
      float u = (px - fr.x) * inv_w;
      float v = (py - fr.y) * inv_h;
      if (u < 0.0f) u = 0.0f;
      if (u > 1.0f) u = 1.0f;
      if (v < 0.0f) v = 0.0f;
      if (v > 1.0f) v = 1.0f;
      verts[1 + li] = (SDL_Vertex){.position = {px, py}, .color = col, .tex_coord = {uv[0] + u * du, uv[1] + v * dv}};
    }
  }

  SDL_SetTextureColorMod(e->tex, 255, 255, 255);
  SDL_SetTextureAlphaMod(e->tex, 255);
  SDL_RenderGeometry(r->sdl, e->tex, verts, TOTAL_VERTS, k_rrect_indices, PERIM_VERTS * 3);
}

void ccord_draw_texture(ccord_renderer *r, const char *key, ccord_rect dst, uint8_t alpha)
{
  ccord_draw_texture_rounded_anim(r, key, dst, 0.0f, alpha, true);
}

void ccord_draw_texture_rounded_anim(ccord_renderer *r, const char *key, ccord_rect dst, float radius, uint8_t alpha, bool animate)
{
  if (!r || alpha == 0 || dst.w <= 0.0f || dst.h <= 0.0f) return;
  tex_entry *e = tex_find(r, key);
  if (!e || !e->tex) return;
  tex_advance_anim(r, e, animate);
  if (!e->tex) return;
  tex_draw(r, e, dst, radius, alpha, (const float[4]){0.0f, 0.0f, 1.0f, 1.0f});
}

void ccord_draw_texture_cover(ccord_renderer *r, const char *key, ccord_rect dst, float radius, uint8_t alpha, bool animate)
{
  if (!r || alpha == 0 || dst.w <= 0.0f || dst.h <= 0.0f) return;
  tex_entry *e = tex_find(r, key);
  if (!e || !e->tex || e->w <= 0 || e->h <= 0) return;
  tex_advance_anim(r, e, animate);
  float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  float ta = (float)e->w / (float)e->h, da = dst.w / dst.h;
  if (ta > da) {            // image is wider than the box: crop left and right
    float keep = da / ta;
    uv[0] = (1.0f - keep) * 0.5f;
    uv[2] = uv[0] + keep;
  } else if (ta < da) {     // taller: crop top and bottom
    float keep = ta / da;
    uv[1] = (1.0f - keep) * 0.5f;
    uv[3] = uv[1] + keep;
  }
  tex_draw(r, e, dst, radius, alpha, uv);
}

void ccord_draw_texture_rounded(ccord_renderer *r, const char *key, ccord_rect dst, float radius, uint8_t alpha)
{
  ccord_draw_texture_rounded_anim(r, key, dst, radius, alpha, true);
}
