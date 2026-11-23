// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "render_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ccord/assets/embedded_assets.h>
#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/paths.h>

static SDL_FColor fcolor(const ccord_renderer *r, ccord_color c, float alpha_mul)
{
  return (SDL_FColor){(float)c.r / 255.0f, (float)c.g / 255.0f, (float)c.b / 255.0f,
                      (float)c.a / 255.0f * alpha_mul * r->alpha};
}

static void set_draw_color(ccord_renderer *r, ccord_color c)
{
  SDL_SetRenderDrawColor(r->sdl, c.r, c.g, c.b, ccord_fx_alpha(r, c.a));
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
    snprintf(r->default_font_rel[i], sizeof r->default_font_rel[i], "%s", rel ? rel : "");
    if (ccord_paths_join(r->font_path[i], sizeof r->font_path[i], ccord_paths_asset_dir(), rel, err) != CCORD_OK) {
      ccord_render_destroy(r);
      return nullptr;
    }
    snprintf(r->default_font_path[i], sizeof r->default_font_path[i], "%s", r->font_path[i]);
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
  const char *rname = SDL_GetRendererName(r->sdl);
  r->software = rname && strcmp(rname, "software") == 0;
  if (!headless) SDL_SetRenderVSync(r->sdl, 1);   // headless dummy/offscreen skips simulated 60 Hz sleeps
  CCORD_LOG_I("render: SDL renderer '%s', video driver '%s'", rname ? rname : "?", video ? video : "?");
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
  // Bundled Twemoji/CJK fallback fonts (~7 MB) are loaded lazily on the first glyph NotoSans doesn't cover,
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
  if (r->engine) {
    TTF_TextEngine *fresh = TTF_CreateRendererTextEngine(r->sdl);
    if (fresh) {
      TTF_DestroyRendererTextEngine(r->engine);
      r->engine = fresh;
    }
  }
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
  if (r->mono_base) {
    TTF_ClearFallbackFonts(r->mono_base);
    TTF_CloseFont(r->mono_base);
  }
  for (int k = 0; k < CCORD_BUNDLED_COUNT; k++) {
    if (r->bundled_base[k]) TTF_CloseFont(r->bundled_base[k]);
  }
  for (int k = 0; k < r->nsys_fallbacks; k++) {
    if (r->fallback_base[k]) TTF_CloseFont(r->fallback_base[k]);
    ccord_free(r->fallback_path[k]);
    r->fallback_path[k] = nullptr;
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
  uint64_t now_ms = ccord_clock_ms();
  if (now_ms >= r->last_idle_sweep_ms && (now_ms - r->last_idle_sweep_ms) >= 1000u) {
    r->last_idle_sweep_ms = now_ms;
    tex_cache_evict_idle(r);
  }
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
  SDL_FRect fr = render_snap(r, rc);
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
  SDL_FRect fr = render_snap(r, rc);
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

void ccord_draw_arc(ccord_renderer *r, float cx, float cy, float radius, float start_deg, float end_deg,
                    float thickness, ccord_color c, bool filled)
{
  if (!r || c.a == 0 || radius <= 0.0f) return;
  float span = end_deg - start_deg;
  if (span == 0.0f) return;
  if (span > 360.0f) span = 360.0f;
  if (span < -360.0f) span = -360.0f;
  float abs_span = fabsf(span);
  int segs = (int)ceilf((abs_span / 360.0f) * fmaxf(16.0f, radius * 0.9f));
  if (segs < 4) segs = 4;
  if (segs > 96) segs = 96;
  const float k_rad = 3.14159265358979323846f / 180.0f;
  if (filled) {
    SDL_Vertex verts[98];
    int idx[96 * 3];
    SDL_FColor fc = fcolor(r, c, 1.0f);
    verts[0] = (SDL_Vertex){.position = {(cx + r->ox) * r->scale, (cy + r->oy) * r->scale}, .color = fc};
    for (int i = 0; i <= segs; i++) {
      float a = (start_deg + span * ((float)i / (float)segs)) * k_rad;
      float px = (cx + cosf(a) * radius + r->ox) * r->scale;
      float py = (cy + sinf(a) * radius + r->oy) * r->scale;
      verts[i + 1] = (SDL_Vertex){.position = {px, py}, .color = fc};
    }
    int ni = 0;
    for (int i = 0; i < segs; i++) {
      idx[ni++] = 0;
      idx[ni++] = i + 1;
      idx[ni++] = i + 2;
    }
    SDL_RenderGeometry(r->sdl, nullptr, verts, segs + 2, idx, ni);
    return;
  }
  float th = thickness > 0.0f ? thickness : 1.0f;
  ccord_vec2 prev = {cx + cosf(start_deg * k_rad) * radius, cy + sinf(start_deg * k_rad) * radius};
  for (int i = 1; i <= segs; i++) {
    float a = (start_deg + span * ((float)i / (float)segs)) * k_rad;
    ccord_vec2 cur = {cx + cosf(a) * radius, cy + sinf(a) * radius};
    ccord_draw_line(r, prev, cur, th, c);
    prev = cur;
  }
}

void ccord_draw_circle_stroke(ccord_renderer *r, float cx, float cy, float radius, float thickness, ccord_color c)
{
  ccord_draw_arc(r, cx, cy, radius, 0.0f, 360.0f, thickness, c, false);
}

void ccord_draw_rrect_stroke(ccord_renderer *r, ccord_rect rc, float radius, float thickness, ccord_color c)
{
  if (!r || c.a == 0 || rc.w <= 0.0f || rc.h <= 0.0f) return;
  float th = thickness > 0.0f ? thickness : 1.0f;
  float half = th * 0.5f;
  ccord_rect in = {rc.x + half, rc.y + half, rc.w - th, rc.h - th};
  if (in.w <= 0.0f || in.h <= 0.0f) {
    ccord_draw_rrect(r, rc, radius, c);
    return;
  }
  float maxr = fminf(in.w, in.h) * 0.5f;
  float rad = radius > maxr ? maxr : (radius < 0.0f ? 0.0f : radius);
  if (rad < 0.5f) {
    ccord_draw_rect(r, (ccord_rect){rc.x, rc.y, rc.w, th}, c);
    ccord_draw_rect(r, (ccord_rect){rc.x, rc.y + rc.h - th, rc.w, th}, c);
    ccord_draw_rect(r, (ccord_rect){rc.x, rc.y + th, th, fmaxf(rc.h - 2.0f * th, 0.0f)}, c);
    ccord_draw_rect(r, (ccord_rect){rc.x + rc.w - th, rc.y + th, th, fmaxf(rc.h - 2.0f * th, 0.0f)}, c);
    return;
  }
  // Straight segments
  if (in.w > 2.0f * rad) {
    ccord_draw_line(r, (ccord_vec2){in.x + rad, in.y}, (ccord_vec2){in.x + in.w - rad, in.y}, th, c);
    ccord_draw_line(r, (ccord_vec2){in.x + rad, in.y + in.h}, (ccord_vec2){in.x + in.w - rad, in.y + in.h}, th, c);
  }
  if (in.h > 2.0f * rad) {
    ccord_draw_line(r, (ccord_vec2){in.x, in.y + rad}, (ccord_vec2){in.x, in.y + in.h - rad}, th, c);
    ccord_draw_line(r, (ccord_vec2){in.x + in.w, in.y + rad}, (ccord_vec2){in.x + in.w, in.y + in.h - rad}, th, c);
  }
  // Four corner quarter-arcs
  ccord_draw_arc(r, in.x + in.w - rad, in.y + rad, rad, 270.0f, 360.0f, th, c, false);
  ccord_draw_arc(r, in.x + in.w - rad, in.y + in.h - rad, rad, 0.0f, 90.0f, th, c, false);
  ccord_draw_arc(r, in.x + rad, in.y + in.h - rad, rad, 90.0f, 180.0f, th, c, false);
  ccord_draw_arc(r, in.x + rad, in.y + rad, rad, 180.0f, 270.0f, th, c, false);
}

void ccord_draw_polygon(ccord_renderer *r, const ccord_vec2 *pts, size_t count, ccord_color c, bool filled,
                        float thickness)
{
  if (!r || !pts || count < 2u || c.a == 0) return;
  if (count > 128u) count = 128u;
  if (!filled) {
    float th = thickness > 0.0f ? thickness : 1.0f;
    for (size_t i = 0; i + 1u < count; i++) {
      ccord_draw_line(r, pts[i], pts[i + 1u], th, c);
    }
    return;
  }
  if (count < 3u) return;
  SDL_Vertex verts[128];
  int idx[126 * 3];
  SDL_FColor fc = fcolor(r, c, 1.0f);
  for (size_t i = 0; i < count; i++) {
    verts[i] = (SDL_Vertex){.position = {(pts[i].x + r->ox) * r->scale, (pts[i].y + r->oy) * r->scale}, .color = fc};
  }
  int ni = 0;
  for (size_t i = 1u; i + 1u < count; i++) {
    idx[ni++] = 0;
    idx[ni++] = (int)i;
    idx[ni++] = (int)(i + 1u);
  }
  SDL_RenderGeometry(r->sdl, nullptr, verts, (int)count, idx, ni);
}

void ccord_draw_gradient_rect(ccord_renderer *r, ccord_rect rc, ccord_color c0, ccord_color c1, bool vertical)
{
  if (!r || rc.w <= 0.0f || rc.h <= 0.0f || (c0.a == 0 && c1.a == 0)) return;
  SDL_FRect fr = render_snap(r, rc);
  if (fr.w <= 0.0f || fr.h <= 0.0f) return;
  SDL_FColor fc0 = fcolor(r, c0, 1.0f);
  SDL_FColor fc1 = fcolor(r, c1, 1.0f);
  SDL_FColor tl = fc0;
  SDL_FColor tr = vertical ? fc0 : fc1;
  SDL_FColor bl = vertical ? fc1 : fc0;
  SDL_FColor br = fc1;
  SDL_Vertex v[4] = {
    {.position = {fr.x, fr.y}, .color = tl},
    {.position = {fr.x + fr.w, fr.y}, .color = tr},
    {.position = {fr.x + fr.w, fr.y + fr.h}, .color = br},
    {.position = {fr.x, fr.y + fr.h}, .color = bl},
  };
  static const int idx[6] = {0, 1, 2, 0, 2, 3};
  SDL_RenderGeometry(r->sdl, nullptr, v, 4, idx, 6);
}

bool ccord_render_set_custom_fonts(ccord_renderer *r, const char *regular_path, const char *bold_path)
{
  if (!r) return false;
  const char *want[2] = {
    (regular_path && regular_path[0]) ? regular_path : r->default_font_path[0],
    (bold_path && bold_path[0]) ? bold_path : r->default_font_path[1],
  };
  if (strcmp(r->font_path[0], want[0]) == 0 && strcmp(r->font_path[1], want[1]) == 0) {
    return true;
  }
  TTF_Font *opened[2] = {nullptr, nullptr};
  for (int i = 0; i < 2; i++) {
    opened[i] = TTF_OpenFont(want[i], 16.0f);
    if (!opened[i] && strcmp(want[i], r->default_font_path[i]) == 0 && r->default_font_rel[i][0]) {
      size_t flen = 0;
      const unsigned char *fbytes = ccord_embedded_font(r->default_font_rel[i], &flen);
      if (fbytes && flen > 0) {
        SDL_IOStream *io = SDL_IOFromConstMem(fbytes, flen);
        if (io) opened[i] = TTF_OpenFontIO(io, true, 16.0f);
      }
    }
    if (!opened[i]) {
      if (opened[0]) TTF_CloseFont(opened[0]);
      return false;
    }
  }
  text_cache_flush(r);
  for (int i = 0; i < 2; i++) {
    if (r->base[i]) {
      TTF_ClearFallbackFonts(r->base[i]);
      TTF_CloseFont(r->base[i]);
    }
    r->base[i] = opened[i];
    snprintf(r->font_path[i], sizeof r->font_path[i], "%s", want[i]);
  }
  return true;
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
  SDL_FRect f = render_snap(r, rc);
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

