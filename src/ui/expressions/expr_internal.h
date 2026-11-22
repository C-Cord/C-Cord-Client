// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#endif

#include "ui_internal.h"
#include "ui_voice_surfaces.h"
#include "ui_style.h"

#include <ccord/core/alloc.h>
#include <ccord/emoji/emoji.h>
#include <ccord/store/expressions.h>

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define EXPR_TAB_GIFS       0
#define EXPR_TAB_STICKERS   1
#define EXPR_TAB_EMOJI      2
#define EXPR_TAB_SOUNDBOARD 3

#define EXPR_POPOVER_W 428.0f
#define EXPR_POPOVER_H 448.0f
// Search field text inset: the leading glyph on the left, the clear button on the right (UIS_SP_6 + UIS_SP_2).
#define EXPR_SEARCH_PAD (UIS_SP_6 + UIS_SP_2)

#define EXPR_HIT_MAX 160u
#define EXPR_RAIL_MAX 256u

// Failed expression blobs (GIF catalog, settings-proto, sticker packs) retry after this cooldown so a
// flaky REST endpoint recovers without a per-frame request storm.
#define EXPR_FETCH_RETRY_MS 2000u
// Bottom strip of the Emoji rail reserved for the pinned Default Unicode emoji button.
#define EXPR_RAIL_PINNED_H 36.0f

typedef enum expr_item_kind : uint8_t {
  EXPR_ITEM_UNICODE_EMOJI = 0,
  EXPR_ITEM_CUSTOM_EMOJI  = 1,
  EXPR_ITEM_GIF_TILE      = 2,
  EXPR_ITEM_GIF_CATEGORY  = 3,
  EXPR_ITEM_GIF_FAV_CAT   = 4,
  EXPR_ITEM_GIF_TREND_CAT = 5,
  EXPR_ITEM_STICKER       = 6,
  EXPR_ITEM_SOUNDBOARD    = 7,
} expr_item_kind;

typedef struct expr_hit_cell {
  ccord_rect rect;
  ccord_rect star_rect;
  expr_item_kind kind;
  size_t index;
  ccord_id id;
  ccord_id guild_id;
  uint8_t format_type;
  bool animated;
  char name[64];
  char utf8[32];
  char sub[64];
  char url[CCORD_URL_MAX_BYTES + 1];          // full ccord_gif_item URL, no truncation
  char preview_url[CCORD_URL_MAX_BYTES + 1];
  uint32_t gif_w, gif_h;                      // GIF tile: the item's own size and wire format, so starring it
  uint32_t gif_format;                        // saves the same favorite Discord holds, not a guessed 320x200
} expr_hit_cell;

typedef struct expr_rail_item {
  ccord_rect rect;
  float target_scroll_y;
  int section_id;
  ccord_id guild_id;
  char label[32];
} expr_rail_item;

typedef struct expr_custom_emoji_hit {
  size_t guild_idx;
  size_t emoji_idx;
} expr_custom_emoji_hit;

typedef struct expr_sticker_hit {
  bool is_pack;
  size_t container_idx;
  size_t sticker_idx;
} expr_sticker_hit;

#define EXPR_GIF_SEARCH_DEBOUNCE_MS 250u

typedef struct ui_expr_body {
  bool skin_popup_open;
  ccord_rect skin_btn_rect;
  ccord_rect skin_option_rects[6];

  ccord_text_edit search_edit;
  bool search_focused;
  ccord_rect search_rect;
  ccord_rect search_clear_rect;
  ccord_rect back_btn_rect;

  ccord_rect tab_rects[4];

  ccord_scroller glide[4];      // wheel glide of each tab's grid (scroll.h)
  ccord_scroller rail_glide;    // wheel glide of the emoji guild rail
  float content_h[4];

  int gif_subview;          // 0=home (categories + trending), 1=favorites, 2=trending-only, 3=category/search
  char gif_category_label[64];
  uint64_t expr_retry_after_ms[CCORD_EXPR_KIND_COUNT];   // FAILED-blob retry cooldown per kind

  uint64_t gif_search_due_ms;
  bool gif_search_pending;
  int64_t last_hover_key;

  expr_hit_cell hits[EXPR_HIT_MAX];
  size_t nhits;
  int hovered_hit;
  ccord_rect hits_clip;       // viewport the grid hit cells are live in (no clicks through the preview bar)
  ccord_rect grid_scrollbar_rect;   // drawn grid thumb (zero when the tab does not overflow)
  ccord_rect search_caret_rect;     // drawn caret in the search field (zero when unfocused or blinking off)
  ccord_rect search_selection_rect; // drawn search selection highlight (zero when the caret has no selection)

  expr_rail_item rail[EXPR_RAIL_MAX];
  size_t nrail;
  int active_rail;

  // Emoji guild rail: independent scroll + pinned Default Unicode button (rects kept for input hit-tests)
  ccord_rect rail_rect;         // full 40px rail column
  ccord_rect rail_view;         // scrollable upper region (rail_rect minus the pinned strip)
  ccord_rect rail_pinned_rect;  // 28x28 Default Unicode emoji button
  float rail_content_h;
  bool pending_unicode_jump;    // pinned button clicked: jump to the first Unicode category on next draw

  // Cached filtered search lists (invalidated on query, store/expr revision, or soundboard guild change).
  bool emoji_cache_valid;
  uint64_t emoji_cache_srev;
  uint64_t emoji_cache_erev;
  char emoji_cache_query[128];
  expr_custom_emoji_hit *emoji_custom_hits;
  size_t emoji_custom_count;
  size_t emoji_custom_cap;
  size_t emoji_unicode_hits[96];
  size_t emoji_unicode_count;

  bool sticker_cache_valid;
  uint64_t sticker_cache_srev;
  uint64_t sticker_cache_erev;
  char sticker_cache_query[128];
  expr_sticker_hit *sticker_hits;
  size_t sticker_count;
  size_t sticker_cap;

  bool sb_cache_valid;
  uint64_t sb_cache_srev;
  uint64_t sb_cache_erev;
  ccord_id sb_cache_guild;
  char sb_cache_query[128];
  size_t *sb_hits;
  size_t sb_count;
  size_t sb_cap;
} ui_expr_body;

struct ui_expr_picker {
  int tab;                          // 0=GIFs, 1=Stickers, 2=Emoji, 3=Soundboard
  uint8_t skin_tone;                // 0..5
  int8_t btn_hover;                 // last hovered composer button (-1..2)
  ccord_rect composer_btn_rects[3]; // 0=GIF, 1=Sticker, 2=Emoji
  float scroll_y[4];                // grid scroll per tab (persists across close/open)
  float rail_scroll_y;              // emoji rail scroll (persists across close/open)
  float sticker_rail_scroll_y;      // sticker rail scroll (persists across close/open)
  ui_expr_body *body;               // heavy picker body (~215 KiB), allocated only while open
};

#define EXPR_RAIL_W 40.0f
#define EXPR_EMOJI_CELL 42.0f
#define EXPR_EMOJI_GAP 3.0f
#define EXPR_GRID_PAD 10.0f
#define EXPR_GIF_GAP 12.0f   // Discord's masonry itemGutter
#define EXPR_GIF_MIN_TILE_W 200.0f      // Discord masonry desiredItemWidth
#define EXPR_GIF_MAX_COLS 8             // Discord masonry maxColumns
#define EXPR_STICKER_GAP 8.0f
#define EXPR_STICKER_MIN_CELL 100.0f
#define EXPR_STICKER_MAX_COLS 6

// Cross-TU internal declarations
void ep_draw_clock_icon(ccord_renderer *r, float cx, float cy, float rad, ccord_color c, ccord_color bg);
void ep_draw_star_icon(ccord_renderer *r, float cx, float cy, float rad, ccord_color c);
void ep_expr_rail_set_scroll(struct ui_expr_picker *ep, float scroll);
float ep_maxf(float a, float b);
ccord_color ep_palette_from_hash(uint64_t h);
void ep_draw_emoji_tab(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, ccord_rect body);
void ep_draw_gifs_tab(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, ccord_rect body);
void ep_draw_soundboard_tab(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, ccord_rect body);
void ep_draw_stickers_tab(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, ccord_rect body);
extern const char *const ep_k_skin_tone_swatches[6];
