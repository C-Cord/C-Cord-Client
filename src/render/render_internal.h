// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <ccord/render/anim.h>
#include <ccord/render/av_stream.h>
#include <ccord/render/render.h>

#define CCORD_CLIP_DEPTH 16
#define CCORD_FX_DEPTH 8
#define CCORD_TEXT_CACHE_SIZE 1024
#define CCORD_FONT_CACHE_SIZE 24
#define CCORD_DISC_CACHE_SIZE 48
#define CCORD_TEX_CACHE_SIZE 1024
// Bundled subsets (2) plus fonts found at run time on the first miss of a code point no attached font covers.
#define CCORD_MAX_FALLBACKS 64
#define CCORD_FALLBACK_PATH_MAX 1100
#define CCORD_MISS_CACHE_SIZE 256

typedef struct font_entry {
  TTF_Font *font;
  TTF_Font *fallbacks[CCORD_MAX_FALLBACKS];
  int nfallbacks;
  ccord_font_style style;
  int px;
} font_entry;

typedef struct tex_entry {
  char *key;
  SDL_Texture *tex;
  int w, h;
  uint64_t tick;
  ccord_anim anim;
  bool has_anim;
  size_t anim_cur_frame;
  uint64_t anim_start_ms;
  // Animations load progressively: `dec` is open while frames are still being decoded into `anim` (a few ms per
  // frame of the app, see ccord_render_pump_media). One too big to keep decoded switches to `streaming`: no
  // frames are kept and `dec` stays open for good, handing out each frame as it falls due.
  ccord_av_stream *dec;
  bool streaming;
  bool dec_video;           // the source is a video clip (frame timing differs from GIF rules)
  size_t anim_cap;          // capacity of anim.frames / anim.delay_ms
  size_t anim_bytes;        // pixels held by anim.frames
  size_t anim_budget;       // above this the entry streams instead
  uint64_t stream_due_ms;   // streaming: when the next frame replaces the shown one
} tex_entry;

typedef struct text_entry {
  TTF_Text *text;
  TTF_Font *font;
  char *str;
  size_t len;
  // Control characters are cut out before shaping (they would draw as .notdef boxes). `shaped` is that cut text
  // (nullptr when nothing was cut: shaped == str); o2s/s2o map byte offsets between `str` and `shaped`.
  char *shaped;
  size_t slen;
  uint32_t *o2s;      // len + 1 entries, original offset -> shaped offset
  uint32_t *s2o;      // slen + 1 entries, shaped offset -> original offset
  int wrap_px;
  uint32_t hash;
  uint64_t tick;
  int next;           // next entry in the same hash bucket, -1 = end
  int w, h;           // measured size in physical px
} text_entry;

typedef struct disc_entry {
  SDL_Texture *tex;
  int diameter;
} disc_entry;

struct ccord_renderer {
  ccord_platform *platform;
  SDL_Window *window;
  SDL_Renderer *sdl;
  TTF_TextEngine *engine;
  char font_path[2][1024];
  float line_factor;
  float scale;          // physical px per logical px = display_scale * zoom
  float display_scale;  // platform display scale without UI zoom
  float zoom;           // user UI zoom, CCORD_ZOOM_MIN..CCORD_ZOOM_MAX
  float lw, lh;         // logical size of the frame (window px / display_scale / zoom)
  SDL_Rect clips[CCORD_CLIP_DEPTH];
  int nclips;
  // Whole-group fades and slides (ccord_push_alpha / ccord_push_offset): applied to everything drawn while pushed.
  float alpha;                       // product of the pushed alphas, 1 = untouched
  float alpha_stack[CCORD_FX_DEPTH];
  int nalpha;
  float ox, oy;                      // sum of the pushed offsets, logical px
  float off_stack[CCORD_FX_DEPTH][2];
  int noff;
  bool ttf_inited;

  TTF_Font *base[2];    // one open font per style; sized copies come from font_cache
  TTF_Font *fallback_base[CCORD_MAX_FALLBACKS];
  int nfallbacks;
  int nbundled;         // fallback_base[0..nbundled) are the bundled subsets; the rest were found at run time
  char fallback_path[CCORD_MAX_FALLBACKS][CCORD_FALLBACK_PATH_MAX];
  bool fallbacks_probed;
  bool glyph_fixes_off;                          // test hook: no run-time font lookup, no control-character cutting
  uint32_t miss_cp[CCORD_MISS_CACHE_SIZE];       // code points no installed font covers (not searched again)
  int nmiss, miss_next;
  font_entry fonts[CCORD_FONT_CACHE_SIZE];
  int nfonts;

  text_entry texts[CCORD_TEXT_CACHE_SIZE];
  int buckets[CCORD_TEXT_CACHE_SIZE];
  int ntexts;
  uint64_t tick;
  uint64_t cache_misses;

  disc_entry discs[CCORD_DISC_CACHE_SIZE];
  int ndiscs, disc_next;

  tex_entry texs[CCORD_TEX_CACHE_SIZE];
  int ntexs;
  int tex_cap;
  uint64_t tex_tick;
  uint64_t tex_evictions;
  uint64_t anim_now_ms;
  size_t anim_bytes_total;            // sum of every entry's anim_bytes
  uint64_t frame_tick, prev_frame_tick;   // tex_tick at the last two ccord_render_begin calls
  int dec_cursor;                     // round-robin start for progressive decoding
  bool anim_activity;                 // an animated texture was drawn (animating) since the last take
};

// text.c
void text_cache_init(ccord_renderer *r);
void text_cache_flush(ccord_renderer *r);   // destroys all TTF_Text and sized fonts (scale change, shutdown)
void fallback_fonts_init(ccord_renderer *r);   // opens the bundled emoji/CJK subsets only (no system font); idempotent

// Alpha-scaled colour component (render.c / text.c): what actually reaches the GPU while an alpha is pushed.
static inline uint8_t ccord_fx_alpha(const ccord_renderer *r, uint8_t a)
{
  return r->alpha >= 1.0f ? a : (uint8_t)((float)a * (r->alpha > 0.0f ? r->alpha : 0.0f) + 0.5f);
}
