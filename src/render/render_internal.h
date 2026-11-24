// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <math.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <ccord/render/anim.h>
#include <ccord/render/av_stream.h>
#include <ccord/render/render.h>

#define CCORD_CLIP_DEPTH 16
#define CCORD_FX_DEPTH 8
#define CCORD_TEXT_CACHE_SIZE 1024
#define CCORD_ELLIPSIS_SETS 256
#define CCORD_ELLIPSIS_WAYS 2
#define CCORD_FONT_CACHE_SIZE 64
#define CCORD_COLOR_GLYPH_CACHE_SIZE 256
#define CCORD_DISC_CACHE_SIZE 48
#define CCORD_TEX_CACHE_SIZE 1024
#define CCORD_TEX_MAX_MIPS 4
#define MAX_IMAGE_DIM 4096
#define CCORD_TEX_HASH_SIZE 2048
#define CCORD_TEX_HASH_MASK (CCORD_TEX_HASH_SIZE - 1u)
// Bundled subsets (emoji + CJK, loaded lazily & independently) plus fonts found at run time on the first miss.
#define CCORD_BUNDLED_EMOJI 0
#define CCORD_BUNDLED_CJK 1
#define CCORD_BUNDLED_COUNT 2
#define CCORD_MAX_FALLBACKS 64
#define CCORD_FALLBACK_PATH_MAX 1100
#define CCORD_MISS_CACHE_SIZE 256

typedef struct font_entry {
  TTF_Font *font;
  TTF_Font *bundled_fb[CCORD_BUNDLED_COUNT];   // lazily attached per sized font
  TTF_Font *fallbacks[CCORD_MAX_FALLBACKS];    // indexed by system fallback slot, lazily attached
  ccord_font_style style;
  int px;
} font_entry;

typedef struct tex_entry {
  char *key;
  uint32_t key_hash;
  SDL_Texture *tex;
  int w, h;
  SDL_Texture *mips[CCORD_TEX_MAX_MIPS];
  int mip_w[CCORD_TEX_MAX_MIPS];
  int mip_h[CCORD_TEX_MAX_MIPS];
  int nmips;
  size_t tex_bytes;
  uint64_t tick;
  int16_t lru_prev;         // intrusive LRU doubly-linked list (-1 = head / unlinked)
  int16_t lru_next;         // intrusive LRU doubly-linked list (-1 = tail), or free-list next when key == nullptr
  bool tex_streaming;       // created with SDL_TEXTUREACCESS_STREAMING
  bool premul;              // pixels premultiplied + SDL_BLENDMODE_BLEND_PREMULTIPLIED
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
  uint64_t last_used_ms;    // monotonic wall clock (ccord_clock_ms) of last touch/upload
  uint64_t anim_drawn_tick; // tex_tick when last drawn with animate == true
} tex_entry;

typedef struct text_color_op {
  TTF_Font *font;
  uint32_t glyph_index;
  SDL_Rect src;
  SDL_Rect dst;
} text_color_op;

typedef struct color_glyph_entry {
  TTF_Font *font;
  uint32_t glyph_index;
  SDL_Rect src;
  int w, h;                 // target physical pixel size
  SDL_Texture *tex;         // premultiplied RGBA32, SDL_BLENDMODE_BLEND_PREMULTIPLIED
  uint64_t tick;
} color_glyph_entry;

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
  text_color_op *color_ops;
  int ncolor_ops;
  bool has_non_color_ops;
  int wrap_px;
  uint32_t hash;
  int next;           // next entry in the same hash bucket, -1 = end
  int16_t lru_prev;   // intrusive LRU doubly-linked list, -1 = head
  int16_t lru_next;   // intrusive LRU doubly-linked list, -1 = tail
  int w, h;           // measured size in physical px
} text_entry;

typedef struct ellipsis_entry {
  const TTF_Font *font;
  int max_w_px;
  uint32_t hash;
  uint32_t len;
  char *str;
  uint16_t fit_bytes;
  bool fits_full;
  bool valid;
} ellipsis_entry;

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
  char default_font_path[2][1024];
  char default_font_rel[2][128];
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
  bool software;        // true when SDL renderer is the CPU "software" backend
  bool vsync_enabled;   // true when SDL_SetRenderVSync(1) succeeded

  TTF_Font *base[2];    // one open font per style; sized copies come from font_cache
  TTF_Font *mono_base;  // lazily probed monospace base face for CCORD_FONT_MONO / CCORD_FONT_MONO_BOLD
  bool mono_probed;
  TTF_Font *bundled_base[CCORD_BUNDLED_COUNT];   // 0 = Emoji, 1 = CJK; opened lazily & independently
  bool bundled_probed[CCORD_BUNDLED_COUNT];
  TTF_Font *fallback_base[CCORD_MAX_FALLBACKS];  // system fallback fonts found at run time
  int nsys_fallbacks;
  char *fallback_path[CCORD_MAX_FALLBACKS];
  bool glyph_fixes_off;                          // test hook: no run-time font lookup, no control-character cutting
  bool android_font_deferred;                    // true if a glyph lookup was deferred while Android font index built
  uint32_t miss_cp[CCORD_MISS_CACHE_SIZE];       // code points no installed font covers (not searched again)
  int nmiss, miss_next;
  font_entry fonts[CCORD_FONT_CACHE_SIZE];
  int nfonts;
  color_glyph_entry color_glyphs[CCORD_COLOR_GLYPH_CACHE_SIZE];
  int ncolor_glyphs;
  uint64_t color_glyph_tick;

  text_entry texts[CCORD_TEXT_CACHE_SIZE];
  int buckets[CCORD_TEXT_CACHE_SIZE];
  int ntexts;
  int16_t text_lru_head, text_lru_tail;
  uint64_t cache_misses;
  ellipsis_entry ellipsis[CCORD_ELLIPSIS_SETS][CCORD_ELLIPSIS_WAYS];
  uint8_t ellipsis_lru[CCORD_ELLIPSIS_SETS];

  disc_entry discs[CCORD_DISC_CACHE_SIZE];
  int ndiscs, disc_next;

  tex_entry texs[CCORD_TEX_CACHE_SIZE];
  uint16_t tex_hash[CCORD_TEX_HASH_SIZE];        // open-addressed hash table: 0 = empty, slot + 1 = occupied
  int16_t tex_lru_head, tex_lru_tail, tex_free_head;
  int tex_hwm;                                   // high-water mark of allocated slots in texs[]
  int ntexs;
  int tex_cap;
  size_t tex_bytes_total;                        // sum of every active entry's tex_bytes
  uint64_t tex_tick;
  uint64_t tex_evictions;
  uint64_t anim_now_ms;
  size_t anim_bytes_total;                       // sum of every entry's anim_bytes
  int anim_dec_count;                            // active progressive-decode entries (dec && !streaming)
  int stream_dec_count;                          // active streaming-decode entries (dec && streaming)
  uint64_t frame_tick, prev_frame_tick;          // tex_tick at the last two ccord_render_begin calls
  int dec_cursor;                                // round-robin start for progressive decoding
  bool anim_activity;                            // an animated texture was drawn (animating) since the last take
  bool has_anim_next;                            // earliest anim clock deadline recorded since the last take
  uint64_t anim_next_ms;
  uint64_t last_idle_sweep_ms;
};

// text.c
void text_cache_init(ccord_renderer *r);
void text_cache_flush(ccord_renderer *r);   // destroys all TTF_Text and sized fonts (scale change, shutdown)
void fallback_fonts_init(ccord_renderer *r);   // opens the bundled emoji/CJK subsets only (no system font); idempotent

// av_stream.c
uint64_t av_stream_last_used_ms(const ccord_av_stream *s);
void av_stream_touch(ccord_av_stream *s, uint64_t now_ms);
size_t av_stream_mem_bytes(const ccord_av_stream *s);
bool av_stream_next_frame_ex(ccord_av_stream *s, bool want_premul, const uint8_t **rgba, uint32_t *pts_ms,
                             uint32_t *dur_ms, bool *out_already_premul);

// Alpha-scaled colour component (render.c / text.c): what actually reaches the GPU while an alpha is pushed.
static inline uint8_t ccord_fx_alpha(const ccord_renderer *r, uint8_t a)
{
  return r->alpha >= 1.0f ? a : (uint8_t)((float)a * (r->alpha > 0.0f ? r->alpha : 0.0f) + 0.5f);
}

// texture_cache.c
void tex_cache_init(ccord_renderer *r);
void tex_free_slot(ccord_renderer *r, int slot);
void tex_cache_evict_idle(ccord_renderer *r);
void tex_cache_evict_idle_now(ccord_renderer *r);

// Snaps a logical rect to whole physical pixels so neighbours sharing an edge never leave a seam.
static inline SDL_FRect render_snap(const ccord_renderer *r, ccord_rect rc)
{
  float x0 = roundf((rc.x + r->ox) * r->scale), y0 = roundf((rc.y + r->oy) * r->scale);
  float x1 = roundf((rc.x + r->ox + rc.w) * r->scale), y1 = roundf((rc.y + r->oy + rc.h) * r->scale);
  return (SDL_FRect){x0, y0, x1 - x0, y1 - y0};
}
