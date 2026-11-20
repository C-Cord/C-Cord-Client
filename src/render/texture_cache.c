// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "render_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
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

void tex_cache_init(ccord_renderer *r)
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

void tex_free_slot(ccord_renderer *r, int slot)
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
  SDL_FRect fr = render_snap(r, dst);
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
