// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
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

struct ui_expr_picker {
  int tab;                  // 0=GIFs, 1=Stickers, 2=Emoji, 3=Soundboard
  uint8_t skin_tone;        // 0..5
  bool skin_popup_open;
  ccord_rect skin_btn_rect;
  ccord_rect skin_option_rects[6];

  ccord_text_edit search_edit;
  bool search_focused;
  ccord_rect search_rect;
  ccord_rect search_clear_rect;
  ccord_rect back_btn_rect;

  ccord_rect tab_rects[4];
  ccord_rect composer_btn_rects[3]; // 0=GIF, 1=Sticker, 2=Emoji

  float scroll_y[4];
  ccord_scroller glide[4];      // wheel glide of each tab's grid (scroll.h)
  ccord_scroller rail_glide;    // wheel glide of the emoji guild rail
  float content_h[4];

  int gif_subview;          // 0=home (categories + trending), 1=favorites, 2=trending-only, 3=category/search
  char gif_category_label[64];
  uint64_t expr_retry_after_ms[CCORD_EXPR_KIND_COUNT];   // FAILED-blob retry cooldown per kind

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
  float rail_scroll_y;
  float rail_content_h;
  bool pending_unicode_jump;    // pinned button clicked: jump to the first Unicode category on next draw
};

static float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static float maxf(float a, float b)
{
  return a > b ? a : b;
}

// Clamp the Emoji rail scroll and shift the cached item rects with it, so hit-tests stay consistent
// between wheel events and the next draw (no off-screen ghost clicks).
static void expr_rail_set_scroll(struct ui_expr_picker *ep, float scroll)
{
  float max_s = maxf(ep->rail_content_h - ep->rail_view.h, 0.0f);
  scroll = clampf(scroll, 0.0f, max_s);
  float delta = ep->rail_scroll_y - scroll;
  if (delta == 0.0f) return;
  for (size_t i = 0; i < ep->nrail; i++) ep->rail[i].rect.y += delta;
  ep->rail_scroll_y = scroll;
}

static struct ui_expr_picker *ensure_expr(ccord_ui *ui)
{
  if (!ui) return nullptr;
  if (ui->expr) return ui->expr;
  struct ui_expr_picker *ep = ccord_calloc(1, sizeof *ep);
  if (!ep) return nullptr;
  ep->tab = EXPR_TAB_EMOJI;
  ep->hovered_hit = -1;
  ccord_text_edit_init(&ep->search_edit, 128);
  ui->expr = ep;
  return ep;
}

void ui_expr_picker_destroy(ccord_ui *ui)
{
  if (!ui || !ui->expr) return;
  ccord_text_edit_free(&ui->expr->search_edit);
  ccord_free(ui->expr);
  ui->expr = nullptr;
}

// Fetch one picker blob by state: NONE asks once, FAILED retries with force after the cooldown;
// PENDING/READY are left alone (the session also drops duplicates). No one-shot latch: reopening the
// picker or switching back to GIFs recovers blobs that failed earlier.
static void request_expr_blob(ccord_ui *ui, struct ui_expr_picker *ep, ccord_expr_kind kind)
{
  if (!ui->d.session || !ui->d.store) return;
  ccord_expr_blob_state st = ccord_store_expr_blob_state(ui->d.store, kind, nullptr);
  if (st != CCORD_EXPR_BLOB_NONE && st != CCORD_EXPR_BLOB_FAILED) return;
  if (st == CCORD_EXPR_BLOB_FAILED && ui->now_ms < ep->expr_retry_after_ms[kind]) return;
  ep->expr_retry_after_ms[kind] = ui->now_ms + EXPR_FETCH_RETRY_MS;
  ccord_error err;
  (void)ccord_session_request_expression_data(ui->d.session, kind, nullptr, st == CCORD_EXPR_BLOB_FAILED, &err);
}

static void request_expression_data(ccord_ui *ui, struct ui_expr_picker *ep)
{
  if (!ui || !ep) return;
  request_expr_blob(ui, ep, CCORD_EXPR_GIF_CATEGORIES);
  request_expr_blob(ui, ep, CCORD_EXPR_GIF_TRENDING);
  // Favorite GIFs + frequent emojis both live in the settings proto; local frecency must not gate it.
  request_expr_blob(ui, ep, CCORD_EXPR_SETTINGS_PROTO);
  request_expr_blob(ui, ep, CCORD_EXPR_STICKER_PACKS);
}

// One layout for every picker grid, derived from the popover width: Emoji cells, GIF tile columns and Sticker
// columns all come from here, so no recorded rect can leave its grid. At the default 428 px popover this is 8 emoji
// columns, 2 GIF columns (200 px tiles) and 3 sticker columns.
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

static int expr_fit_cols(float avail, float min_cell, float gap, int max_cols)
{
  int cols = (int)((avail + gap) / (min_cell + gap));
  return cols < 1 ? 1 : (cols > max_cols ? max_cols : cols);
}

// Discord's masonry column count (current.js): Math.min(Math.max(Math.floor(width/desiredItemWidth),1),maxColumns).
// The gutter is not part of it.
static int expr_masonry_cols(float width, float desired, int max_cols)
{
  int cols = (int)(width / desired);
  return cols < 1 ? 1 : (cols > max_cols ? max_cols : cols);
}

void ui_expr_grid_layout_for_width(float popover_w, ui_expr_grid_layout *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  float emoji_w = popover_w - EXPR_RAIL_W - EXPR_GRID_PAD * 2.0f;
  out->emoji_cell = EXPR_EMOJI_CELL;
  out->emoji_gap = EXPR_EMOJI_GAP;
  out->emoji_cols = expr_fit_cols(emoji_w, EXPR_EMOJI_CELL, EXPR_EMOJI_GAP, 64);

  float gif_w = popover_w - EXPR_GRID_PAD * 2.0f;
  out->gif_gap = EXPR_GIF_GAP;
  out->gif_cols = expr_masonry_cols(gif_w, EXPR_GIF_MIN_TILE_W, EXPR_GIF_MAX_COLS);
  out->gif_col_w = (gif_w - (float)(out->gif_cols - 1) * EXPR_GIF_GAP) / (float)out->gif_cols;
  out->gif_cat_w = (gif_w - EXPR_GIF_GAP) * 0.5f;   // Favorites/Trending/category tiles stay a pair of halves

  float stk_w = popover_w - EXPR_RAIL_W - EXPR_GRID_PAD * 2.0f;
  out->sticker_gap = EXPR_STICKER_GAP;
  out->sticker_cols = expr_fit_cols(stk_w, EXPR_STICKER_MIN_CELL, EXPR_STICKER_GAP, EXPR_STICKER_MAX_COLS);
  out->sticker_cell = (stk_w - (float)(out->sticker_cols - 1) * EXPR_STICKER_GAP) / (float)out->sticker_cols;
}

static void compute_picker_rect(ccord_ui *ui)
{
  float pw = EXPR_POPOVER_W;
  float ph = EXPR_POPOVER_H;
  if (pw > ui->win_w - 16.0f) pw = maxf(ui->win_w - 16.0f, 260.0f);
  if (ph > ui->win_h - 16.0f) ph = maxf(ui->win_h - 16.0f, 240.0f);

  ccord_rect cbox = ui_composer_box(ui);
  float px = cbox.x + cbox.w - pw;
  float py = cbox.y - ph - 8.0f;
  if (px < 8.0f) px = 8.0f;
  if (px + pw > ui->win_w - 8.0f) px = ui->win_w - pw - 8.0f;
  if (py < 8.0f) py = 8.0f;
  if (py + ph > ui->win_h - 8.0f) py = ui->win_h - ph - 8.0f;
  ui->emoji_picker_rect = (ccord_rect){px, py, pw, ph};
}

void ui_expr_picker_open(ccord_ui *ui, int tab, ccord_id channel_id, ccord_id reaction_msg_id)
{
  struct ui_expr_picker *ep = ensure_expr(ui);
  if (!ep) return;
  if (tab < 0 || tab > 3) tab = EXPR_TAB_EMOJI;
  if (reaction_msg_id != CCORD_ID_NONE) tab = EXPR_TAB_EMOJI;

  if (ui->emoji_picker_open && ui->emoji_picker_msg_id == reaction_msg_id && ep->tab == tab &&
      reaction_msg_id == CCORD_ID_NONE) {
    ui_expr_picker_close(ui);
    return;
  }

  ep->tab = tab;
  ep->skin_popup_open = false;
  ep->search_focused = true;
  ep->hovered_hit = -1;
  if (tab == EXPR_TAB_GIFS && ep->search_edit.len == 0) {
    ep->gif_subview = 0;
    ep->gif_category_label[0] = '\0';
  }
  ui->emoji_picker_open = true;
  ui->emoji_picker_channel = channel_id != CCORD_ID_NONE ? channel_id : ccord_store_selected_channel(ui->d.store);
  ui->emoji_picker_msg_id = reaction_msg_id;
  ui->emoji_picker_for_status = false;
  ui->autocomplete_open = false;
  request_expression_data(ui, ep);
  compute_picker_rect(ui);

  // No cell exists until the first draw records the real ones: a click before then lands on nothing.
  memset(ui->emoji_picker_cells, 0, sizeof ui->emoji_picker_cells);
  ui->dirty = true;
}

// Ticks the wheel glides of the picker's grids and rail by fdt_ms (scroll.h). Dropped when the picker is closed.
bool ui_expr_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  struct ui_expr_picker *ep = ui->expr;
  if (!ep) return false;
  bool moving = false;
  bool open = ui->emoji_picker_open;
  for (int t = 0; t < 4; t++) {
    if (ep->glide[t].pending == 0.0f) continue;
    if (!open) {
      ccord_scroller_cancel(&ep->glide[t]);
      continue;
    }
    moving = ccord_scroller_tick(&ep->glide[t], &ep->scroll_y[t], fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced) || moving;
    ui->dirty = true;
  }
  if (ep->rail_glide.pending != 0.0f) {
    if (!open) {
      ccord_scroller_cancel(&ep->rail_glide);
    } else {
      float off = ep->rail_scroll_y;   // the rail's rects follow its offset (expr_rail_set_scroll)
      moving = ccord_scroller_tick(&ep->rail_glide, &off, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced) || moving;
      expr_rail_set_scroll(ep, off);
      ui->dirty = true;
    }
  }
  return moving;
}

// Offset of the active tab's grid and how far it can travel (verify-slice seam; room is 0 when the picker is absent).
void ui_expr_scroll_debug(const ccord_ui *ui, float *scroll, float *room)
{
  const struct ui_expr_picker *ep = ui ? ui->expr : nullptr;
  int t = ep ? ep->tab : 0;
  if (scroll) *scroll = ep ? ep->scroll_y[t] : 0.0f;
  if (room) *room = ep ? maxf(ep->content_h[t] - (ui->emoji_picker_rect.h - 125.0f), 0.0f) : 0.0f;
}

void ui_expr_picker_close(ccord_ui *ui)
{
  if (!ui) return;
  ui->emoji_picker_open = false;
  ui->emoji_picker_msg_id = CCORD_ID_NONE;
  ui->emoji_picker_for_status = false;
  if (ui->expr) {
    ui->expr->skin_popup_open = false;
    ui->expr->hovered_hit = -1;
  }
  ui->dirty = true;
}

// ---------------------------------------------------------------- procedural icons & artwork

void ui_expr_draw_smiley(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float rad = size * 0.46f;
  ccord_draw_circle(r, cx, cy, rad, c);
  ccord_color hole = {0x2b, 0x2d, 0x31, 255};
  ccord_draw_circle(r, cx - rad * 0.34f, cy - rad * 0.18f, maxf(rad * 0.13f, 1.3f), hole);
  ccord_draw_circle(r, cx + rad * 0.34f, cy - rad * 0.18f, maxf(rad * 0.13f, 1.3f), hole);
  // Smile curve
  ccord_draw_line(r, (ccord_vec2){cx - rad * 0.38f, cy + rad * 0.18f},
                  (ccord_vec2){cx, cy + rad * 0.44f}, maxf(size * 0.09f, 1.4f), hole);
  ccord_draw_line(r, (ccord_vec2){cx, cy + rad * 0.44f},
                  (ccord_vec2){cx + rad * 0.38f, cy + rad * 0.18f}, maxf(size * 0.09f, 1.4f), hole);
}

static void draw_star_icon(ccord_renderer *r, float cx, float cy, float rad, ccord_color c)
{
  ccord_draw_triangle(r, (ccord_vec2){cx, cy - rad},
                      (ccord_vec2){cx - rad * 0.65f, cy + rad * 0.75f},
                      (ccord_vec2){cx + rad * 0.65f, cy + rad * 0.75f}, c);
  ccord_draw_triangle(r, (ccord_vec2){cx - rad * 0.95f, cy - rad * 0.25f},
                      (ccord_vec2){cx + rad * 0.95f, cy - rad * 0.25f},
                      (ccord_vec2){cx, cy + rad * 0.55f}, c);
}

static void draw_clock_icon(ccord_renderer *r, float cx, float cy, float rad, ccord_color c, ccord_color bg)
{
  ccord_draw_circle(r, cx, cy, rad, c);
  ccord_draw_circle(r, cx, cy, rad * 0.74f, bg);
  ccord_draw_line(r, (ccord_vec2){cx, cy}, (ccord_vec2){cx, cy - rad * 0.48f}, 1.6f, c);
  ccord_draw_line(r, (ccord_vec2){cx, cy}, (ccord_vec2){cx + rad * 0.42f, cy + rad * 0.15f}, 1.6f, c);
}

static void draw_sticker_icon(ccord_renderer *r, float cx, float cy, float size, ccord_color c, ccord_color bg)
{
  float hs = size * 0.44f;
  ccord_rect box = {cx - hs, cy - hs, hs * 2.0f, hs * 2.0f};
  ccord_draw_rrect(r, box, 4.0f, c);
  ccord_rect inner = {box.x + 1.8f, box.y + 1.8f, box.w - 3.6f, box.h - 3.6f};
  ccord_draw_rrect(r, inner, 2.5f, bg);
  // Folded corner at bottom-right
  ccord_draw_triangle(r, (ccord_vec2){box.x + box.w - 6.5f, box.y + box.h},
                      (ccord_vec2){box.x + box.w, box.y + box.h - 6.5f},
                      (ccord_vec2){box.x + box.w - 6.5f, box.y + box.h - 6.5f}, c);
  ccord_draw_circle(r, cx - 2.5f, cy - 1.5f, 1.4f, c);
  ccord_draw_circle(r, cx + 2.5f, cy - 1.5f, 1.4f, c);
}

static ccord_color palette_from_hash(uint64_t h)
{
  static const ccord_color k_pal[8] = {
    {0x58, 0x65, 0xf2, 255}, // Blurple
    {0x23, 0xa5, 0x5a, 255}, // Green
    {0xf0, 0xb2, 0x32, 255}, // Gold
    {0xeb, 0x45, 0x9e, 255}, // Fuchsia
    {0xf2, 0x3f, 0x43, 255}, // Crimson
    {0x00, 0xa8, 0xfc, 255}, // Cyan
    {0x9b, 0x59, 0xb6, 255}, // Purple
    {0xe6, 0x7e, 0x22, 255}, // Orange
  };
  return k_pal[h % 8u];
}

static uint64_t str_hash(const char *s, size_t len)
{
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < len; i++) {
    h = (h ^ (uint8_t)s[i]) * 1099511628211ULL;
  }
  return h;
}

// Account playback preferences. Discord owns these; until the first settings object
// arrives the UI uses Discord's defaults: animated emoji on and stickers always playing.
static bool expr_animate_emoji(const ccord_ui *ui)
{
  const ccord_user_settings *settings =
    ui->d.store ? ccord_store_user_settings(ui->d.store) : nullptr;
  return settings ? settings->animate_emoji : true;
}

static uint8_t expr_animate_stickers_mode(const ccord_ui *ui)
{
  const ccord_user_settings *settings =
    ui->d.store ? ccord_store_user_settings(ui->d.store) : nullptr;
  return settings ? settings->animate_stickers : 0;
}

static bool expr_pointer_over(const ccord_ui *ui, ccord_rect rect)
{
  return ui->mouse_in && ui_in_rect(ui->mx, ui->my, rect);
}

void ui_expr_draw_custom_emoji(ccord_ui *ui, ccord_renderer *r, ccord_id id, const char *name, size_t nlen,
                               bool animated, ccord_rect rect)
{
  if (!ui || !r || rect.w <= 0.0f || rect.h <= 0.0f) return;
  char url[128];
  const char *ext = animated ? "gif" : "png";
  snprintf(url, sizeof url, "https://cdn.discordapp.com/emojis/%llu.%s?size=48", (unsigned long long)id, ext);
  bool ready = ui_ensure_media(ui, r, url);
  int tw = 0, th = 0;
  bool real_tex = ready && ccord_render_has_texture(r, url, &tw, &th) && tw > 1 && th > 1;
  if (ready) {
    bool animate = animated && (expr_animate_emoji(ui) || expr_pointer_over(ui, rect));
    ccord_draw_texture_rounded_anim(r, url, rect, 4.0f, 255, animate || !animated);
  }
  if (real_tex) return;

  // Stylized custom emoji tile for 1x1 mock textures or while loading
  uint64_t h = str_hash(name ? name : "", nlen) ^ (uint64_t)id;
  ccord_color bg = palette_from_hash(h);
  ccord_draw_rrect(r, rect, maxf(rect.w * 0.22f, 3.0f), bg);
  ccord_rect inner = {rect.x + 2.0f, rect.y + 2.0f, rect.w - 4.0f, rect.h - 4.0f};
  if (inner.w > 4.0f && inner.h > 4.0f) {
    ccord_draw_rrect(r, inner, maxf(inner.w * 0.18f, 2.0f), ui_mix(bg, (ccord_color){0x11, 0x12, 0x14, 255}, 0.28f));
  }
  char abbr[4] = {0};
  if (name && nlen > 0) {
    abbr[0] = (char)toupper((unsigned char)name[0]);
    if (nlen > 1) abbr[1] = (char)toupper((unsigned char)name[1]);
    if (nlen > 2 && rect.w >= 26.0f) abbr[2] = (char)toupper((unsigned char)name[2]);
  } else {
    abbr[0] = 'E';
  }
  float fsz = clampf(rect.h * 0.40f, 9.0f, 14.0f);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, fsz, (ccord_color){255, 255, 255, 255});
  ccord_vec2 m = ccord_text_measure(r, abbr, strlen(abbr), &ts, 0.0f);
  ccord_text_draw(r, abbr, strlen(abbr), rect.x + (rect.w - m.x) * 0.5f, rect.y + (rect.h - m.y) * 0.5f, &ts, 0.0f);
  if (animated && rect.w >= 24.0f) {
    ccord_draw_circle(r, rect.x + rect.w - 4.0f, rect.y + 4.0f, 3.0f, COL(status_online));
  }
}

void ui_expr_draw_sticker_art(ccord_ui *ui, ccord_renderer *r, ccord_id id, const char *name, size_t nlen,
                              uint8_t format_type, ccord_rect rect, bool inline_chat)
{
  if (!ui || !r || rect.w <= 0.0f || rect.h <= 0.0f) return;
  bool real_tex = false;
  bool animated_sticker = format_type == CCORD_STICKER_APNG || format_type == CCORD_STICKER_GIF;
  if (id != CCORD_ID_NONE && format_type != CCORD_STICKER_LOTTIE) {
    char url[160];
    const char *ext = format_type == CCORD_STICKER_GIF ? "gif" : "png";   // APNG bytes are served from the .png URL
    snprintf(url, sizeof url, "https://media.discordapp.net/stickers/%llu.%s?size=160", (unsigned long long)id, ext);
    bool ready = ui_ensure_media(ui, r, url);
    int tw = 0, th = 0;
    real_tex = ready && ccord_render_has_texture(r, url, &tw, &th) && tw > 1 && th > 1;
    if (ready) {
      uint8_t mode = expr_animate_stickers_mode(ui);
      bool animate = animated_sticker && (mode == 0 || (mode == 1 && expr_pointer_over(ui, rect)));
      ccord_draw_texture_rounded_anim(r, url, rect, 8.0f, 255, animate || !animated_sticker);
      if (inline_chat) ui->dbg_attachment_textures_drawn++;
    }
  }
  if (real_tex) return;

  uint64_t h = str_hash(name ? name : "Sticker", nlen) ^ (uint64_t)id;
  ccord_color accent = palette_from_hash(h);
  ccord_color card_bg = ui_mix(COL(bg_secondary), accent, 0.22f);
  ccord_draw_rrect(r, (ccord_rect){rect.x - 1.0f, rect.y - 1.0f, rect.w + 2.0f, rect.h + 2.0f}, 9.0f,
                   ui_alpha(accent, 0.55f));
  ccord_draw_rrect(r, rect, 8.0f, card_bg);

  // Decorative background circles + mascot badge
  float cx = rect.x + rect.w * 0.5f;
  float cy = rect.y + rect.h * 0.42f;
  float rad = (rect.w < rect.h ? rect.w : rect.h) * 0.26f;
  ccord_draw_circle(r, cx, cy, rad + 5.0f, ui_alpha(accent, 0.30f));
  ccord_draw_circle(r, cx, cy, rad, accent);

  // Mascot face inside sticker circle
  ccord_color white = {255, 255, 255, 255};
  ccord_color dark = {0x1e, 0x1f, 0x22, 255};
  ccord_draw_circle(r, cx - rad * 0.45f, cy - rad * 0.55f, rad * 0.28f, accent);
  ccord_draw_circle(r, cx + rad * 0.45f, cy - rad * 0.55f, rad * 0.28f, accent);
  ccord_draw_circle(r, cx - rad * 0.32f, cy - rad * 0.12f, maxf(rad * 0.13f, 2.0f), white);
  ccord_draw_circle(r, cx + rad * 0.32f, cy - rad * 0.12f, maxf(rad * 0.13f, 2.0f), white);
  ccord_draw_circle(r, cx - rad * 0.30f, cy - rad * 0.12f, maxf(rad * 0.07f, 1.2f), dark);
  ccord_draw_circle(r, cx + rad * 0.30f, cy - rad * 0.12f, maxf(rad * 0.07f, 1.2f), dark);
  ccord_draw_rrect(r, (ccord_rect){cx - rad * 0.24f, cy + rad * 0.08f, rad * 0.48f, rad * 0.28f}, rad * 0.12f,
                   ui_mix(accent, white, 0.45f));

  // Format pill in top-left corner
  const char *fmt_label = format_type == CCORD_STICKER_LOTTIE ? "LOTTIE"
                        : format_type == CCORD_STICKER_APNG   ? "APNG"
                        : format_type == CCORD_STICKER_GIF    ? "GIF"
                                                              : "STICKER";
  ccord_text_style fs = ui_style(ui, CCORD_FONT_BOLD, 9.5f, white);
  ccord_vec2 fm = ccord_text_measure(r, fmt_label, strlen(fmt_label), &fs, 0.0f);
  ccord_rect fpill = {rect.x + 6.0f, rect.y + 6.0f, fm.x + 10.0f, 16.0f};
  ccord_draw_rrect(r, fpill, 4.0f, ui_alpha(dark, 0.72f));
  ccord_text_draw(r, fmt_label, strlen(fmt_label), fpill.x + 5.0f, fpill.y + (fpill.h - fm.y) * 0.5f, &fs, 0.0f);

  // Sticker title banner at bottom
  if (name && nlen > 0) {
    ccord_rect lbar = {rect.x + 6.0f, rect.y + rect.h - 26.0f, rect.w - 12.0f, 20.0f};
    ccord_draw_rrect(r, lbar, 4.0f, ui_alpha(dark, 0.76f));
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, inline_chat ? 11.5f : 10.0f, white);
    ccord_vec2 nm = ccord_text_measure(r, name, nlen, &ns, 0.0f);
    float tx = lbar.x + maxf((lbar.w - nm.x) * 0.5f, 4.0f);
    ccord_text_draw_ellipsis(r, name, nlen, tx, lbar.y + (lbar.h - ui_line_h(ui, ns.size)) * 0.5f, lbar.w - 8.0f, &ns);
  }
}

// ---------------------------------------------------------------- composer right-side buttons

void ui_expr_draw_composer_buttons(ccord_ui *ui, ccord_renderer *r, ccord_rect box)
{
  struct ui_expr_picker *ep = ensure_expr(ui);
  if (!ep || box.w < 160.0f) return;

  float btn_h = 24.0f;
  float by = box.y + (box.h - btn_h) * 0.5f;
  // The voice message mic button takes the rightmost 26px slot (ui_vm_mic_rect), so the trio starts 30px further left.
  float right = box.x + box.w - 8.0f - 30.0f;

  // 1. Emoji button (rightmost of the trio, 26x24)
  ccord_rect emoji_r = {right - 26.0f, by, 26.0f, btn_h};
  right = emoji_r.x - 4.0f;

  // 2. Sticker button (middle, 26x24)
  ccord_rect sticker_r = {right - 26.0f, by, 26.0f, btn_h};
  right = sticker_r.x - 4.0f;

  // 3. GIF button (leftmost of the trio, 32x24)
  ccord_rect gif_r = {right - 32.0f, by, 32.0f, btn_h};

  ep->composer_btn_rects[EXPR_TAB_GIFS] = gif_r;
  ep->composer_btn_rects[EXPR_TAB_STICKERS] = sticker_r;
  ep->composer_btn_rects[EXPR_TAB_EMOJI] = emoji_r;

  // GIF badge button
  bool gif_active = ui->emoji_picker_open && ui->emoji_picker_msg_id == CCORD_ID_NONE && ep->tab == EXPR_TAB_GIFS;
  bool gif_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, gif_r);
  ccord_color gif_col =
      ui_state_mix(ui, "egifbtn", 0, gif_hov, gif_active, COL(text_secondary), COL(text_header), COL(brand));
  ccord_rect gif_badge = {gif_r.x + 2.0f, gif_r.y + 3.0f, gif_r.w - 4.0f, gif_r.h - 6.0f};
  ccord_draw_rrect(r, gif_badge, 3.5f, gif_col);
  ccord_text_style gs = ui_style(ui, CCORD_FONT_BOLD, 9.5f, COL(bg_input));
  ccord_vec2 gt = ccord_text_measure(r, "GIF", 3, &gs, 0.0f);
  ccord_text_draw(r, "GIF", 3, gif_badge.x + (gif_badge.w - gt.x) * 0.5f,
                  gif_badge.y + (gif_badge.h - gt.y) * 0.5f, &gs, 0.0f);

  // Sticker icon button
  bool stk_active = ui->emoji_picker_open && ui->emoji_picker_msg_id == CCORD_ID_NONE && ep->tab == EXPR_TAB_STICKERS;
  bool stk_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, sticker_r);
  ccord_color stk_col =
      ui_state_mix(ui, "estkbtn", 0, stk_hov, stk_active, COL(text_secondary), COL(text_header), COL(brand));
  draw_sticker_icon(r, sticker_r.x + sticker_r.w * 0.5f, sticker_r.y + sticker_r.h * 0.5f, 18.0f, stk_col, COL(bg_input));

  // Emoji smiley button
  bool emj_active = ui->emoji_picker_open && ui->emoji_picker_msg_id == CCORD_ID_NONE && ep->tab == EXPR_TAB_EMOJI;
  bool emj_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, emoji_r);
  ccord_color emj_col =
      ui_state_mix(ui, "eemjbtn", 0, emj_hov, emj_active, COL(text_secondary), COL(status_idle), COL(brand));
  ui_expr_draw_smiley(r, emoji_r.x + emoji_r.w * 0.5f, emoji_r.y + emoji_r.h * 0.5f, 18.0f, emj_col);
}

bool ui_expr_handle_composer_click(ccord_ui *ui, float mx, float my)
{
  if (!ui) return false;
  // M5 voice surfaces hang off this every-left-down hook: ring banner, DM call bar, header call
  // button, DAVE badge / RTC button. They are independent of the expression picker state.
  if (ui_call_click(ui, mx, my)) return true;
  if (ui_dave_click(ui, mx, my)) return true;
  if (!ui->expr) return false;
  for (int t = 0; t < 3; t++) {
    if (ui_in_rect(mx, my, ui->expr->composer_btn_rects[t])) {
      ui_expr_picker_open(ui, t, ccord_store_selected_channel(ui->d.store), CCORD_ID_NONE);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------- Expression Picker drawing

static const char *const k_skin_tone_swatches[6] = {
  "\xF0\x9F\x91\x8F",                 // 👏 Default
  "\xF0\x9F\x91\x8F\xF0\x9F\x8F\xBB", // 👏🏻 Light
  "\xF0\x9F\x91\x8F\xF0\x9F\x8F\xBC", // 👏🏼 Medium-Light
  "\xF0\x9F\x91\x8F\xF0\x9F\x8F\xBD", // 👏🏽 Medium
  "\xF0\x9F\x91\x8F\xF0\x9F\x8F\xBE", // 👏🏾 Medium-Dark
  "\xF0\x9F\x91\x8F\xF0\x9F\x8F\xBF", // 👏🏿 Dark
};

static const ccord_color k_skin_tone_colors[6] = {
  {0xff, 0xc8, 0x3d, 255},
  {0xf7, 0xde, 0xce, 255},
  {0xf3, 0xd2, 0xa2, 255},
  {0xd5, 0xab, 0x88, 255},
  {0xaf, 0x7e, 0x57, 255},
  {0x7c, 0x53, 0x3e, 255},
};

static const char *const k_unicode_cat_icons[CCORD_EMOJI_CATEGORY_COUNT] = {
  "\xF0\x9F\x98\x80", // 😀 People
  "\xF0\x9F\x90\xBB", // 🐻 Nature
  "\xF0\x9F\x8D\x94", // 🍔 Food
  "\xE2\x9A\xBD",     // ⚽ Activities
  "\xF0\x9F\x9A\x97", // 🚗 Travel
  "\xF0\x9F\x92\xA1", // 💡 Objects
  "\xE2\x9D\xA4\xEF\xB8\x8F", // ❤️ Symbols
  "\xF0\x9F\x8F\xB3\xEF\xB8\x8F", // 🏳️ Flags
};

static const char *const k_quick_reaction_shortcodes[UI_EMOJI_PICKER_COUNT] = {
  "thumbsup", "heart", "joy", "open_mouth", "sob", "fire", "tada", "eyes",
};

static bool contains_ci_str(const char *hay, const char *needle)
{
  if (!needle || !needle[0]) return true;
  if (!hay) return false;
  size_t nlen = strlen(needle);
  for (const char *p = hay; *p; p++) {
    if (strncasecmp(p, needle, nlen) == 0) return true;
  }
  return false;
}

static void record_emoji_cell(ccord_ui *ui, struct ui_expr_picker *ep, ccord_rect cell,
                              bool is_custom, ccord_id custom_id, ccord_id guild_id, bool animated,
                              const char *name, const char *utf8, const char *sub, size_t *cell_seq,
                              ccord_rect clip_area)
{
  size_t idx = (*cell_seq)++;
  if (idx < UI_EMOJI_PICKER_COUNT) {
    ui->emoji_picker_cells[idx] = cell;
  }
  // Only cells inside the viewport earn hit slots: off-screen items must not exhaust EXPR_HIT_MAX
  // (which would leave the visible emojis unclickable) nor leave rects clickable outside the grid.
  if (cell.y + cell.h < clip_area.y || cell.y > clip_area.y + clip_area.h) return;
  if (ep->nhits >= EXPR_HIT_MAX) return;
  expr_hit_cell *h = &ep->hits[ep->nhits++];
  memset(h, 0, sizeof *h);
  h->rect = cell;
  h->kind = is_custom ? EXPR_ITEM_CUSTOM_EMOJI : EXPR_ITEM_UNICODE_EMOJI;
  h->id = custom_id;
  h->guild_id = guild_id;
  h->animated = animated;
  snprintf(h->name, sizeof h->name, "%.63s", name ? name : "");
  snprintf(h->utf8, sizeof h->utf8, "%.31s", utf8 ? utf8 : "");
  snprintf(h->sub, sizeof h->sub, "%.63s", sub ? sub : "");
}

static void draw_emoji_cell(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, ccord_rect cell,
                            bool is_custom, ccord_id custom_id, ccord_id guild_id, bool animated,
                            const char *name, const char *utf8, const char *sub, size_t *cell_seq,
                            ccord_rect clip_area)
{
  size_t nhits_before = ep->nhits;
  record_emoji_cell(ui, ep, cell, is_custom, custom_id, guild_id, animated, name, utf8, sub, cell_seq, clip_area);
  if (cell.y + cell.h < clip_area.y || cell.y > clip_area.y + clip_area.h) return;

  bool hov = ui->mouse_in && !ep->skin_popup_open && ui_in_rect(ui->mx, ui->my, clip_area) &&
             ui_in_rect(ui->mx, ui->my, cell);
  bool recorded = ep->nhits > nhits_before;
  if (hov && recorded) ep->hovered_hit = (int)(ep->nhits - 1);
  float ecell_h = ui_fade(ui, "ecell", is_custom ? (uint64_t)custom_id : ui_name_id(utf8), hov && recorded);
  if (ecell_h > 0.004f) ccord_draw_rrect(r, cell, 6.0f, ui_alpha(COL(bg_item_hover), ecell_h));
  if (is_custom) {
    ccord_rect img = {cell.x + 4.0f, cell.y + 4.0f, cell.w - 8.0f, cell.h - 8.0f};
    ui_expr_draw_custom_emoji(ui, r, custom_id, name, name ? strlen(name) : 0, animated, img);
  } else if (utf8 && utf8[0]) {
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, 22.0f, COL(text_normal));
    ccord_vec2 et = ccord_text_measure(r, utf8, strlen(utf8), &es, 0.0f);
    ccord_text_draw(r, utf8, strlen(utf8), cell.x + (cell.w - et.x) * 0.5f,
                    cell.y + (cell.h - et.y) * 0.5f, &es, 0.0f);
  }
}

static void draw_section_header(ccord_ui *ui, ccord_renderer *r, const char *title, float x, float y, float w,
                                ccord_rect clip_area)
{
  if (y + 22.0f < clip_area.y || y > clip_area.y + clip_area.h) return;
  char up[96];
  size_t i = 0;
  for (; title && title[i] && i < sizeof up - 1; i++) {
    up[i] = (char)toupper((unsigned char)title[i]);
  }
  up[i] = '\0';
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, 11.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, up, i, x + 2.0f, y + 4.0f, w - 4.0f, &hs);
}

// Discord server icon in a 28x28 rail button: CDN texture via ui_guild_icon_url + ui_ensure_media,
// falling back to the hashed circle + initials while the icon is absent or still loading.
static void draw_rail_guild_icon(ccord_ui *ui, ccord_renderer *r, const expr_rail_item *ri, float cx, float cy)
{
  const ccord_guild *g = ri->guild_id != CCORD_ID_NONE ? ccord_store_guild(ui->d.store, ri->guild_id) : nullptr;
  char icon_url[256];
  if (g && ui_guild_icon_url(g, icon_url, sizeof icon_url)) {
    bool ready = ui_ensure_media(ui, r, icon_url);
    int tw = 0, th = 0;
    if (ready && ccord_render_has_texture(r, icon_url, &tw, &th) && tw > 1 && th > 1) {
      ccord_draw_texture_rounded(r, icon_url, ri->rect, ri->rect.w * 0.5f, 255);
      ui->dbg_expr_rail_icons_drawn++;
      return;
    }
  }
  char init[4] = {0};
  ui_initials(ri->label, init);
  ccord_color gc = palette_from_hash(ri->guild_id);
  ccord_draw_circle(r, cx, cy, 11.0f, gc);
  ccord_text_style is = ui_style(ui, CCORD_FONT_BOLD, 9.5f, (ccord_color){255, 255, 255, 255});
  ccord_vec2 im = ccord_text_measure(r, init, strlen(init), &is, 0.0f);
  ccord_text_draw(r, init, strlen(init), cx - im.x * 0.5f, cy - im.y * 0.5f, &is, 0.0f);
}

static void draw_emoji_tab(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, ccord_rect body)
{
  float rail_w = EXPR_RAIL_W;
  float preview_h = 44.0f;
  ccord_rect rail_rect = {body.x, body.y, rail_w, body.h - preview_h};
  ccord_rect grid_area = {body.x + rail_w, body.y, body.w - rail_w, body.h - preview_h};
  ccord_rect prev_rect = {body.x, body.y + body.h - preview_h, body.w, preview_h};
  // The bottom strip of the rail holds the pinned Default Unicode button; the rest scrolls on its own.
  ccord_rect rail_view = {rail_rect.x, rail_rect.y, rail_rect.w, maxf(rail_rect.h - EXPR_RAIL_PINNED_H, 0.0f)};
  ep->rail_rect = rail_rect;
  ep->rail_view = rail_view;
  ep->hits_clip = grid_area;

  ccord_draw_rect(r, rail_rect, COL(bg_tertiary));
  ccord_draw_rect(r, (ccord_rect){grid_area.x, grid_area.y, 1.0f, grid_area.h}, COL(border_subtle));

  const char *query = ccord_text_edit_cstr(&ep->search_edit);
  size_t qlen = ep->search_edit.len;
  float scroll = ep->scroll_y[EXPR_TAB_EMOJI];

  ui_expr_grid_layout lay;
  ui_expr_grid_layout_for_width(body.w, &lay);
  const float cell_sz = lay.emoji_cell;
  const float gap = lay.emoji_gap;
  float gx0 = grid_area.x + EXPR_GRID_PAD;
  float gy = grid_area.y + 6.0f - scroll;
  float gw = grid_area.w - EXPR_GRID_PAD * 2.0f;
  // As many cells as the popover width allows (8 at the default width); never more than fit inside the grid.
  int cols = lay.emoji_cols;
  size_t cell_seq = 0;

  ep->nrail = 0;
  float ry = rail_view.y + 8.0f - ep->rail_scroll_y;

  ccord_push_clip(r, grid_area);

  if (qlen > 0) {
    draw_section_header(ui, r, "SEARCH RESULTS", gx0, gy, gw, grid_area);
    gy += 24.0f;
    int col = 0;
    // 1. Matching custom guild emojis
    size_t ng = ccord_store_guild_count(ui->d.store);
    for (size_t gi = 0; gi < ng; gi++) {
      const ccord_guild *g = ccord_store_guild_at(ui->d.store, gi);
      if (!g) continue;
      size_t ne = ccord_store_guild_emoji_count(ui->d.store, g->id);
      for (size_t ei = 0; ei < ne; ei++) {
        const ccord_emoji *ce = ccord_store_guild_emoji_at(ui->d.store, g->id, ei);
        if (!ce || !contains_ci_str(ce->name, query)) continue;
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
        draw_emoji_cell(ui, r, ep, cell, true, ce->id, g->id, ce->animated, ce->name, "", g->name, &cell_seq, grid_area);
        if (++col >= cols) { col = 0; gy += cell_sz + gap; }
      }
    }
    // 2. Matching Unicode emojis via ccord_emoji_search
    size_t match_idx[96];
    size_t nmatch = ccord_emoji_search(query, qlen, match_idx, 96);
    for (size_t mi = 0; mi < nmatch; mi++) {
      const ccord_emoji_entry *e = ccord_emoji_at(match_idx[mi]);
      if (!e) continue;
      char toned[32];
      const char *u = e->utf8;
      if (e->has_skin_tones && ep->skin_tone > 0 && ccord_emoji_with_tone(e, ep->skin_tone, toned, sizeof toned) > 0) {
        u = toned;
      }
      ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
      draw_emoji_cell(ui, r, ep, cell, false, CCORD_ID_NONE, CCORD_ID_NONE, false, e->name, u,
                      ccord_emoji_category_name(e->category), &cell_seq, grid_area);
      if (++col >= cols) { col = 0; gy += cell_sz + gap; }
    }
    if (col > 0) gy += cell_sz + gap;
  } else {
    // 1. FREQUENTLY USED
    if (ep->nrail < EXPR_RAIL_MAX) {
      ep->rail[ep->nrail++] = (expr_rail_item){
        .rect = {rail_rect.x + 6.0f, ry, 28.0f, 28.0f},
        .target_scroll_y = maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f),
        .section_id = -1,
        .label = "Frequently Used",
      };
      ry += 32.0f;
    }
    draw_section_header(ui, r, "FREQUENTLY USED", gx0, gy, gw, grid_area);
    gy += 24.0f;
    int col = 0;
    // Always include the 8 quick reaction emojis first so indices 0..7 match k_quick_reaction_shortcodes
    for (size_t qi = 0; qi < UI_EMOJI_PICKER_COUNT; qi++) {
      const char *sc = k_quick_reaction_shortcodes[qi];
      const ccord_emoji_entry *e = ccord_emoji_find_shortcode(sc, strlen(sc));
      if (!e) continue;
      char toned[32];
      const char *u = e->utf8;
      if (e->has_skin_tones && ep->skin_tone > 0 && ccord_emoji_with_tone(e, ep->skin_tone, toned, sizeof toned) > 0) {
        u = toned;
      }
      ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
      draw_emoji_cell(ui, r, ep, cell, false, CCORD_ID_NONE, CCORD_ID_NONE, false, e->name, u,
                      "Frequently Used", &cell_seq, grid_area);
      if (++col >= cols) { col = 0; gy += cell_sz + gap; }
    }
    size_t nfreq = ccord_store_frequent_emoji_count(ui->d.store);
    for (size_t fi = 0; fi < nfreq && fi < 8; fi++) {
      const char *key = ccord_store_frequent_emoji_at(ui->d.store, fi);
      if (!key || !key[0]) continue;
      const ccord_emoji_entry *e = ccord_emoji_find_shortcode(key, strlen(key));
      if (e) {
        bool dup = false;
        for (size_t qi = 0; qi < UI_EMOJI_PICKER_COUNT; qi++) {
          if (strcmp(e->name, k_quick_reaction_shortcodes[qi]) == 0) { dup = true; break; }
        }
        if (dup) continue;
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
        draw_emoji_cell(ui, r, ep, cell, false, CCORD_ID_NONE, CCORD_ID_NONE, false, e->name, e->utf8,
                        "Frequently Used", &cell_seq, grid_area);
        if (++col >= cols) { col = 0; gy += cell_sz + gap; }
      } else {
        const ccord_emoji *ce = ccord_store_find_emoji_by_name(ui->d.store, key);
        if (ce) {
          ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
          draw_emoji_cell(ui, r, ep, cell, true, ce->id, CCORD_ID_NONE, ce->animated, ce->name, "",
                          "Frequently Used", &cell_seq, grid_area);
          if (++col >= cols) { col = 0; gy += cell_sz + gap; }
        }
      }
    }
    if (col > 0) { col = 0; gy += cell_sz + gap; }
    gy += 4.0f;

    // 2. FAVORITES
    size_t nfav = ccord_store_favorite_emoji_count(ui->d.store);
    if (nfav > 0) {
      if (ep->nrail < EXPR_RAIL_MAX) {
        ep->rail[ep->nrail++] = (expr_rail_item){
          .rect = {rail_rect.x + 6.0f, ry, 28.0f, 28.0f},
          .target_scroll_y = maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f),
          .section_id = -2,
          .label = "Favorites",
        };
        ry += 32.0f;
      }
      draw_section_header(ui, r, "FAVORITES", gx0, gy, gw, grid_area);
      gy += 24.0f;
      for (size_t fi = 0; fi < nfav; fi++) {
        const char *fk = ccord_store_favorite_emoji_at(ui->d.store, fi);
        if (!fk) continue;
        const ccord_emoji *ce = ccord_store_find_emoji_by_name(ui->d.store, fk);
        if (ce) {
          ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
          draw_emoji_cell(ui, r, ep, cell, true, ce->id, CCORD_ID_NONE, ce->animated, ce->name, "",
                          "Favorites", &cell_seq, grid_area);
          if (++col >= cols) { col = 0; gy += cell_sz + gap; }
          continue;
        }
        const ccord_emoji_entry *e = ccord_emoji_find_shortcode(fk, strlen(fk));
        if (e) {
          ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
          draw_emoji_cell(ui, r, ep, cell, false, CCORD_ID_NONE, CCORD_ID_NONE, false, e->name, e->utf8,
                          "Favorites", &cell_seq, grid_area);
          if (++col >= cols) { col = 0; gy += cell_sz + gap; }
        }
      }
      if (col > 0) { col = 0; gy += cell_sz + gap; }
      gy += 4.0f;
    }

    // 3. GUILD CUSTOM EMOJIS
    size_t ng = ccord_store_guild_count(ui->d.store);
    for (size_t gi = 0; gi < ng; gi++) {
      const ccord_guild *g = ccord_store_guild_at(ui->d.store, gi);
      if (!g) continue;
      size_t ne = ccord_store_guild_emoji_count(ui->d.store, g->id);
      if (ne == 0) continue;
      if (ep->nrail < EXPR_RAIL_MAX) {
        expr_rail_item *ri = &ep->rail[ep->nrail++];
        ri->rect = (ccord_rect){rail_rect.x + 6.0f, ry, 28.0f, 28.0f};
        ri->target_scroll_y = maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f);
        ri->section_id = 100 + (int)gi;
        ri->guild_id = g->id;
        snprintf(ri->label, sizeof ri->label, "%.31s", g->name ? g->name : "Server");
        ry += 32.0f;
      }
      draw_section_header(ui, r, g->name ? g->name : "SERVER EMOJIS", gx0, gy, gw, grid_area);
      gy += 24.0f;
      for (size_t ei = 0; ei < ne; ei++) {
        const ccord_emoji *ce = ccord_store_guild_emoji_at(ui->d.store, g->id, ei);
        if (!ce) continue;
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
        draw_emoji_cell(ui, r, ep, cell, true, ce->id, g->id, ce->animated, ce->name, "",
                        g->name ? g->name : "Custom Emoji", &cell_seq, grid_area);
        if (++col >= cols) { col = 0; gy += cell_sz + gap; }
      }
      if (col > 0) { col = 0; gy += cell_sz + gap; }
      gy += 4.0f;
    }

    // 4. 8 UNICODE CATEGORIES
    for (int cat = 0; cat < CCORD_EMOJI_CATEGORY_COUNT; cat++) {
      size_t cfirst = 0, ccount = 0;
      ccord_emoji_category_range((ccord_emoji_category)cat, &cfirst, &ccount);
      if (ccount == 0) continue;
      const char *cname = ccord_emoji_category_name((ccord_emoji_category)cat);
      if (ep->nrail < EXPR_RAIL_MAX) {
        expr_rail_item *ri = &ep->rail[ep->nrail++];
        ri->rect = (ccord_rect){rail_rect.x + 6.0f, ry, 28.0f, 28.0f};
        ri->target_scroll_y = maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f);
        ri->section_id = cat;
        snprintf(ri->label, sizeof ri->label, "%.31s", cname);
        ry += 30.0f;
      }
      draw_section_header(ui, r, cname, gx0, gy, gw, grid_area);
      gy += 24.0f;
      size_t cap = ccount;
      for (size_t ei = 0; ei < cap; ei++) {
        const ccord_emoji_entry *e = ccord_emoji_at(cfirst + ei);
        if (!e) continue;
        char toned[32];
        const char *u = e->utf8;
        if (e->has_skin_tones && ep->skin_tone > 0 && ccord_emoji_with_tone(e, ep->skin_tone, toned, sizeof toned) > 0) {
          u = toned;
        }
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
        draw_emoji_cell(ui, r, ep, cell, false, CCORD_ID_NONE, CCORD_ID_NONE, false, e->name, u, cname,
                        &cell_seq, grid_area);
        if (++col >= cols) { col = 0; gy += cell_sz + gap; }
      }
      if (col > 0) { col = 0; gy += cell_sz + gap; }
      gy += 4.0f;
    }
  }

  ep->content_h[EXPR_TAB_EMOJI] = maxf((gy + scroll) - grid_area.y + 12.0f, grid_area.h);
  ccord_pop_clip(r);

  // Rail scroll bounds + pending "Default Unicode emoji" jump from the pinned button.
  ep->rail_content_h = maxf((ry + ep->rail_scroll_y) - rail_view.y + 8.0f, 0.0f);
  expr_rail_set_scroll(ep, ep->rail_scroll_y);
  if (ep->pending_unicode_jump) {
    ep->pending_unicode_jump = false;
    for (size_t i = 0; i < ep->nrail; i++) {
      if (ep->rail[i].section_id != 0) continue;
      ccord_scroller_cancel(&ep->glide[EXPR_TAB_EMOJI]);   // a category jump: no glide left over from a wheel notch
      ep->scroll_y[EXPR_TAB_EMOJI] = ep->rail[i].target_scroll_y;
      scroll = ep->scroll_y[EXPR_TAB_EMOJI];
      // Bring the first Unicode category button into the rail view as well.
      float unscrolled_y = ep->rail[i].rect.y + ep->rail_scroll_y;
      expr_rail_set_scroll(ep, unscrolled_y - rail_view.y - 4.0f);
      // The grid above was drawn at the old scroll; force a redraw so the Unicode section and its
      // hit cells actually appear even when no fade/animation schedules another frame (reduced motion).
      ui->dirty = true;
      break;
    }
  }

  // Draw Left Category Rail icons, clipped to the scrollable rail view
  bool unicode_active = false;
  ccord_push_clip(r, rail_view);
  for (size_t i = 0; i < ep->nrail; i++) {
    const expr_rail_item *ri = &ep->rail[i];
    if (ri->rect.y + ri->rect.h < rail_view.y || ri->rect.y > rail_view.y + rail_view.h) continue;
    bool active = (i + 1 == ep->nrail || (scroll >= ri->target_scroll_y - 16.0f &&
                   scroll < ep->rail[i + 1].target_scroll_y - 16.0f));
    if (active && ri->section_id >= 0 && ri->section_id < CCORD_EMOJI_CATEGORY_COUNT) unicode_active = true;
    bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rail_view) && ui_in_rect(ui->mx, ui->my, ri->rect);
    uis_list_row_fill(ui, r, ri->rect, (uis_state){.hover = hov, .selected = active}, "erail", (uint64_t)i);
    float cx = ri->rect.x + ri->rect.w * 0.5f;
    float cy = ri->rect.y + ri->rect.h * 0.5f;
    if (ri->section_id == -1) {
      draw_clock_icon(r, cx, cy, 8.5f, active ? COL(text_header) : COL(text_secondary),
                      active ? COL(bg_item_selected) : COL(bg_tertiary));
    } else if (ri->section_id == -2) {
      draw_star_icon(r, cx, cy, 8.5f, (ccord_color){0xf0, 0xb2, 0x32, 255});
    } else if (ri->section_id >= 100) {
      draw_rail_guild_icon(ui, r, ri, cx, cy);
    } else if (ri->section_id >= 0 && ri->section_id < CCORD_EMOJI_CATEGORY_COUNT) {
      const char *ico = k_unicode_cat_icons[ri->section_id];
      ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, 15.0f, COL(text_normal));
      ccord_vec2 em = ccord_text_measure(r, ico, strlen(ico), &es, 0.0f);
      ccord_text_draw(r, ico, strlen(ico), cx - em.x * 0.5f, cy - em.y * 0.5f, &es, 0.0f);
    }
  }
  ccord_pop_clip(r);

  // Pinned Default Unicode Emoji button: fixed bottom strip, always visible (even during search).
  ccord_rect pin_zone = {rail_rect.x, rail_rect.y + rail_rect.h - EXPR_RAIL_PINNED_H, rail_rect.w, EXPR_RAIL_PINNED_H};
  ccord_draw_rect(r, (ccord_rect){pin_zone.x, pin_zone.y, pin_zone.w, 1.0f}, COL(border_subtle));
  ep->rail_pinned_rect = (ccord_rect){rail_rect.x + 6.0f, pin_zone.y + 4.0f, 28.0f, 28.0f};
  bool pin_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ep->rail_pinned_rect);
  bool pin_active = unicode_active && qlen == 0;
  uis_list_row_fill(ui, r, ep->rail_pinned_rect, (uis_state){.hover = pin_hov, .selected = pin_active}, "erailpin", 0);
  ui_expr_draw_smiley(r, ep->rail_pinned_rect.x + 14.0f, ep->rail_pinned_rect.y + 14.0f, 18.0f,
                      ui_state_mix(ui, "erailpinico", 0, pin_hov, pin_active, COL(text_secondary), COL(text_header),
                                   COL(status_idle)));

  // Bottom Hover Preview Bar
  ccord_draw_rect(r, prev_rect, COL(bg_tertiary));
  ccord_draw_rect(r, (ccord_rect){prev_rect.x, prev_rect.y, prev_rect.w, 1.0f}, COL(border_subtle));
  const expr_hit_cell *preview = nullptr;
  if (ep->hovered_hit >= 0 && (size_t)ep->hovered_hit < ep->nhits) {
    preview = &ep->hits[ep->hovered_hit];
  } else if (ep->nhits > 0) {
    preview = &ep->hits[0];
  }
  if (preview) {
    ccord_rect picon = {prev_rect.x + 12.0f, prev_rect.y + 8.0f, 28.0f, 28.0f};
    if (preview->kind == EXPR_ITEM_CUSTOM_EMOJI) {
      ui_expr_draw_custom_emoji(ui, r, preview->id, preview->name, strlen(preview->name), preview->animated, picon);
    } else if (preview->utf8[0]) {
      ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, 22.0f, COL(text_normal));
      ccord_vec2 et = ccord_text_measure(r, preview->utf8, strlen(preview->utf8), &es, 0.0f);
      ccord_text_draw(r, preview->utf8, strlen(preview->utf8),
                      picon.x + (picon.w - et.x) * 0.5f, picon.y + (picon.h - et.y) * 0.5f, &es, 0.0f);
    }
    char sc_buf[80];
    snprintf(sc_buf, sizeof sc_buf, ":%s:", preview->name[0] ? preview->name : "emoji");
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    float nw = ccord_text_draw_ellipsis(r, sc_buf, strlen(sc_buf), picon.x + 36.0f,
                                        prev_rect.y + (prev_rect.h - ui_line_h(ui, ns.size)) * 0.5f,
                                        prev_rect.w - 160.0f, &ns);
    if (preview->sub[0]) {
      ccord_text_draw_ellipsis(r, preview->sub, strlen(preview->sub), picon.x + 46.0f + nw,
                               prev_rect.y + (prev_rect.h - ui_line_h(ui, ss.size)) * 0.5f,
                               prev_rect.w - (60.0f + nw), &ss);
    }
  }

  // Skin-tone selector popup if open
  if (ep->skin_popup_open) {
    float sw = 32.0f * 6.0f + 10.0f;
    float sh = 38.0f;
    ccord_rect pop = {ep->skin_btn_rect.x + ep->skin_btn_rect.w - sw, ep->skin_btn_rect.y + ep->skin_btn_rect.h + 4.0f, sw, sh};
    uis_popover_frame(ui, r, pop);
    for (int t = 0; t < 6; t++) {
      ccord_rect sr = {pop.x + 5.0f + (float)t * 32.0f, pop.y + 4.0f, 30.0f, 30.0f};
      ep->skin_option_rects[t] = sr;
      bool sel = (int)ep->skin_tone == t;
      bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, sr);
      ccord_draw_rrect(r, sr, 5.0f, ui_state_mix(ui, "eskin", (uint64_t)t, hov, sel, ui_alpha(COL(bg_item_hover), 0.0f),
                                                 COL(bg_item_hover), COL(brand)));
      ccord_draw_circle(r, sr.x + sr.w * 0.5f, sr.y + sr.h * 0.5f, 10.0f, k_skin_tone_colors[t]);
    }
  }
}

static void gif_title_from_url(const char *url, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  const char *slash = url ? strrchr(url, '/') : nullptr;
  const char *slug = (slash && slash[1]) ? slash + 1 : "Tenor GIF";
  size_t i = 0;
  for (; slug[i] && i + 1 < cap; i++) {
    if (strncmp(&slug[i], "-gif-", 5) == 0 || slug[i] == '.') break;
    out[i] = (slug[i] == '-') ? ' ' : slug[i];
  }
  out[i] = '\0';
  if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
  if (!out[0]) snprintf(out, cap, "Tenor GIF");
}

// Previews are decoded no larger than this: a tile is about 200 px wide.
#define GIF_PREVIEW_DIM 256

static bool gif_auto_play(const ccord_ui *ui)
{
  const ccord_user_settings *us = ui->d.store ? ccord_store_user_settings(ui->d.store) : nullptr;
  return us ? us->gif_auto_play : true;
}

static void draw_gif_card(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, const ccord_gif_item *g,
                          ccord_rect card, ccord_rect clip_area)
{
  if (!g) return;
  char title_buf[64];
  gif_title_from_url(g->url, title_buf, sizeof title_buf);
  const char *purl = g->src[0] ? g->src : g->url;
  // Cull before recording: off-screen tiles must not consume hit slots or stay clickable.
  if (card.y + card.h < clip_area.y || card.y > clip_area.y + clip_area.h) return;
  expr_hit_cell *hit = nullptr;
  if (ep->nhits < EXPR_HIT_MAX) {
    hit = &ep->hits[ep->nhits++];
    memset(hit, 0, sizeof *hit);
    hit->rect = card;
    hit->star_rect = (ccord_rect){card.x + card.w - 28.0f, card.y + 6.0f, 22.0f, 22.0f};
    hit->kind = EXPR_ITEM_GIF_TILE;
    snprintf(hit->name, sizeof hit->name, "%.63s", title_buf);
    snprintf(hit->url, sizeof hit->url, "%s", g->url);
    snprintf(hit->preview_url, sizeof hit->preview_url, "%s", purl);
    hit->gif_w = g->width;
    hit->gif_h = g->height;
    hit->gif_format = g->format;
  }

  bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, clip_area) && ui_in_rect(ui->mx, ui->my, card);
  // The preview is the GIF itself, playing: a small decode of the animation (older favourites point at a
  // short video instead, which plays the same way). A favourite saved as a VIDEO (settings-proto format 2) is a
  // video whatever its src looks like: Tenor/Klipy media URLs do not always end in .mp4, and without allow_video the
  // bytes go to the still-image decoder, which refuses them ("Couldn't load").
  // A scheme-less src ("//media.tenor.com/x/y.mp4") is loaded as https://; the stored favorite (and what a star
  // click writes back) keeps its src as it came. The host allow-list still applies to the https:// form.
  char load_buf[CCORD_URL_MAX_BYTES + 8];
  const char *lurl = purl;
  if (purl[0] == '/' && purl[1] == '/') {
    (void)snprintf(load_buf, sizeof load_buf, "https:%s", purl);
    lurl = load_buf;
  }
  bool video = g->format == 2u || ui_url_path_ends(lurl, ".mp4") || ui_url_path_ends(lurl, ".webm");
  // The ring, caption and star share one hover fade (ui_fade), so none of them snaps on the pointer.
  float gh = ui_fade(ui, "gifcell", ui_name_id(g->url), hov);
  float gh_cap = hit ? gh : 0.0f;
  if (gh > 0.004f) {   // a ring around the tile under the pointer
    ccord_draw_rrect(r, (ccord_rect){card.x - 2.0f, card.y - 2.0f, card.w + 4.0f, card.h + 4.0f}, 8.0f,
                     ui_alpha(COL(brand), gh));
  }
  if (ui_ensure_media_ex(ui, r, lurl, GIF_PREVIEW_DIM, video)) {
    ccord_draw_texture_cover(r, lurl, card, 6.0f, 255, gif_auto_play(ui) || hov);
  } else {
    // Still on its way (or lost): a quiet tile that says which.
    ccord_draw_rrect(r, card, 6.0f, COL(bg_tertiary));
    const char *note = ui_media_failed(ui, lurl) ? "Couldn't load" : "GIF";
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, 11.0f, COL(text_muted));
    ccord_vec2 nm = ccord_text_measure(r, note, strlen(note), &ns, 0.0f);
    ccord_text_draw(r, note, strlen(note), card.x + (card.w - nm.x) * 0.5f, card.y + (card.h - nm.y) * 0.5f, &ns, 0.0f);
  }

  if (hov && hit) ep->hovered_hit = (int)(ep->nhits - 1);
  if (gh_cap > 0.004f) {
    // The name only where the pointer is, so the pictures are not half covered by labels.
    ccord_rect cap_r = {card.x, card.y + card.h - 22.0f, card.w, 22.0f};
    ccord_draw_rrect(r, cap_r, 4.0f, ui_alpha((ccord_color){0x11, 0x12, 0x14, 195}, gh_cap));
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, 10.5f, ui_alpha((ccord_color){255, 255, 255, 255}, gh_cap));
    ccord_text_draw_ellipsis(r, title_buf, strlen(title_buf), cap_r.x + 7.0f,
                             cap_r.y + (cap_r.h - ui_line_h(ui, ts.size)) * 0.5f, cap_r.w - 14.0f, &ts);
  }
  // Favorite star button in top-right corner: always shown on a favorite, eased in on hover otherwise.
  bool is_fav = ccord_store_is_favorite_gif(ui->d.store, g->url);
  float star_a = is_fav ? 1.0f : gh_cap;
  if (star_a > 0.004f && hit) {
    ccord_draw_rrect(r, hit->star_rect, 4.0f, ui_alpha((ccord_color){0x11, 0x12, 0x14, 210}, star_a));
    ccord_color scol = is_fav ? (ccord_color){0xf0, 0xb2, 0x32, 255} : (ccord_color){220, 222, 228, 255};
    scol = ui_alpha(scol, star_a);
    draw_star_icon(r, hit->star_rect.x + hit->star_rect.w * 0.5f,
                   hit->star_rect.y + hit->star_rect.h * 0.5f, 6.5f, scol);
  }
}

// Columns of GIFs (2 at the default popover width), each tile in its picture's own shape and placed in the shorter column, so nothing is
// squashed into a fixed box. Returns the y below the taller column.
static float draw_gif_grid(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep,
                           const ccord_gif_item *(*at)(const ccord_store *, size_t), size_t n, float x, float y,
                           float col_w, int cols, float gap, ccord_rect body)
{
  float col_y[EXPR_GIF_MAX_COLS];
  if (cols < 1) cols = 1;
  if (cols > EXPR_GIF_MAX_COLS) cols = EXPR_GIF_MAX_COLS;
  for (int c = 0; c < cols; c++) col_y[c] = y;
  for (size_t i = 0; i < n; i++) {
    const ccord_gif_item *g = at(ui->d.store, i);
    if (!g) continue;
    // Discord's GIF masonry sizes a tile as columnWidth * (height / width) with no clamp, so the shortest-column choice
    // below matches the official client's; only an unknown shape falls back to 4:3.
    float th = col_w * 0.75f;
    if (g->width > 0 && g->height > 0) th = col_w * (float)g->height / (float)g->width;
    int col = 0;   // the shortest column takes the next tile
    for (int c = 1; c < cols; c++) {
      if (col_y[c] + 0.5f < col_y[col]) col = c;
    }
    ccord_rect card = {x + (float)col * (col_w + gap), col_y[col], col_w, th};
    draw_gif_card(ui, r, ep, g, card, body);
    col_y[col] += th + gap;
  }
  float bottom = col_y[0];
  for (int c = 1; c < cols; c++) bottom = maxf(bottom, col_y[c]);
  return bottom;
}

// Loading / empty hint for GIF lists, drawn inside the clipped tab body.
static float draw_gif_hint(ccord_ui *ui, ccord_renderer *r, const char *msg, float x, float y, float w)
{
  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  ccord_text_draw(r, msg, strlen(msg), x, y, &hs, w);
  return y + 24.0f;
}

static void draw_gifs_tab(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, ccord_rect body)
{
  ccord_push_clip(r, body);
  ep->hits_clip = body;
  float scroll = ep->scroll_y[EXPR_TAB_GIFS];
  ui_expr_grid_layout lay;
  ui_expr_grid_layout_for_width(body.w, &lay);
  float pad = EXPR_GRID_PAD;
  float gap = lay.gif_gap;
  float col_w = lay.gif_col_w;     // one GIF tile column
  int gif_cols = lay.gif_cols;
  float cat_w = lay.gif_cat_w;     // half-width Favorites / Trending / category tiles
  float y = body.y + 8.0f - scroll;
  const char *query = ccord_text_edit_cstr(&ep->search_edit);

  ep->back_btn_rect = (ccord_rect){0, 0, 0, 0};

  if (query[0] != '\0') {
    draw_section_header(ui, r, "SEARCH RESULTS", body.x + pad, y, body.w - pad * 2.0f, body);
    y += 24.0f;
    size_t n = ccord_store_gif_search_count(ui->d.store);
    y = draw_gif_grid(ui, r, ep, ccord_store_gif_search_at, n, body.x + pad, y, col_w, gif_cols, gap, body);
    if (n == 0) {
      ccord_expr_blob_state st = ccord_store_expr_blob_state(ui->d.store, CCORD_EXPR_GIF_SEARCH, query);
      const char *hint = (st == CCORD_EXPR_BLOB_PENDING || st == CCORD_EXPR_BLOB_NONE)
                           ? "Searching..."
                           : "No GIFs found. Try a different search.";
      y = draw_gif_hint(ui, r, hint, body.x + pad, y, body.w - pad * 2.0f);
    }
  } else if (ep->gif_subview == 1) {
    // Favorites subview
    ccord_rect back = {body.x + pad, y, 82.0f, 22.0f};
    ep->back_btn_rect = back;
    ccord_draw_rrect(r, back, 4.0f, COL(bg_tertiary));
    ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 11.0f, COL(text_header));
    ccord_text_draw(r, "< Back", 6, back.x + 10.0f, back.y + 4.0f, &bs, 0.0f);
    draw_section_header(ui, r, "FAVORITE GIFS", body.x + pad + 90.0f, y, body.w - 110.0f, body);
    y += 28.0f;
    size_t nfav = ccord_store_favorite_gif_count(ui->d.store);
    y = draw_gif_grid(ui, r, ep, ccord_store_favorite_gif_at, nfav, body.x + pad, y, col_w, gif_cols, gap, body);
    if (nfav == 0) {
      ccord_expr_blob_state st = ccord_store_expr_blob_state(ui->d.store, CCORD_EXPR_SETTINGS_PROTO, nullptr);
      const char *hint = (st == CCORD_EXPR_BLOB_PENDING || st == CCORD_EXPR_BLOB_NONE)
                           ? "Loading Favorite GIFs..."
                           : "No Favorite GIFs yet. Star a GIF to keep it here.";
      y = draw_gif_hint(ui, r, hint, body.x + pad, y, body.w - pad * 2.0f);
    }
  } else {
    // Home view: Favorites & Trending cards + Category tiles + Trending GIFs 2-column grid
    float cat_h = 58.0f;
    ccord_rect fav_tile = {body.x + pad, y, cat_w, cat_h};
    ccord_rect trd_tile = {body.x + pad + cat_w + gap, y, cat_w, cat_h};
    bool cards_visible = (y + cat_h >= body.y && y <= body.y + body.h);

    if (cards_visible && ep->nhits < EXPR_HIT_MAX) {
      expr_hit_cell *hf = &ep->hits[ep->nhits++];
      memset(hf, 0, sizeof *hf);
      hf->rect = fav_tile;
      hf->kind = EXPR_ITEM_GIF_FAV_CAT;
      snprintf(hf->name, sizeof hf->name, "Favorites");
    }
    if (cards_visible && ep->nhits < EXPR_HIT_MAX) {
      expr_hit_cell *ht = &ep->hits[ep->nhits++];
      memset(ht, 0, sizeof *ht);
      ht->rect = trd_tile;
      ht->kind = EXPR_ITEM_GIF_TREND_CAT;
      snprintf(ht->name, sizeof ht->name, "Trending GIFs");
    }

    if (cards_visible) {
      // Favorites card with star badge
      ccord_draw_rrect(r, fav_tile, 6.0f, (ccord_color){0x58, 0x65, 0xf2, 255});
      draw_star_icon(r, fav_tile.x + 22.0f, fav_tile.y + cat_h * 0.5f, 9.0f, (ccord_color){0xf0, 0xb2, 0x32, 255});
      char flbl[48];
      snprintf(flbl, sizeof flbl, "Favorites (%zu)", ccord_store_favorite_gif_count(ui->d.store));
      ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, 12.5f, (ccord_color){255, 255, 255, 255});
      ccord_text_draw_ellipsis(r, flbl, strlen(flbl), fav_tile.x + 38.0f,
                               fav_tile.y + (cat_h - ui_line_h(ui, cs.size)) * 0.5f, fav_tile.w - 44.0f, &cs);

      // Trending GIFs card
      ccord_draw_rrect(r, trd_tile, 6.0f, (ccord_color){0x9b, 0x59, 0xb6, 255});
      ccord_text_draw_ellipsis(r, "Trending GIFs", 13, trd_tile.x + 16.0f,
                               trd_tile.y + (cat_h - ui_line_h(ui, cs.size)) * 0.5f, trd_tile.w - 24.0f, &cs);
    }
    y += cat_h + gap;

    // Category tiles: every loaded category, with its cover art when available
    size_t ncat = ccord_store_gif_category_count(ui->d.store);
    float sm_cat_h = 46.0f;
    for (size_t i = 0; i < ncat; i++) {
      const ccord_gif_category *cat = ccord_store_gif_category_at(ui->d.store, i);
      if (!cat) continue;
      int col = (int)(i % 2u);
      ccord_rect cr = {body.x + pad + (float)col * (cat_w + gap), y, cat_w, sm_cat_h};
      bool cat_visible = (cr.y + cr.h >= body.y && cr.y <= body.y + body.h);
      if (cat_visible && ep->nhits < EXPR_HIT_MAX) {
        expr_hit_cell *hc = &ep->hits[ep->nhits++];
        memset(hc, 0, sizeof *hc);
        hc->rect = cr;
        hc->kind = EXPR_ITEM_GIF_CATEGORY;
        snprintf(hc->name, sizeof hc->name, "%.63s", cat->name);
        snprintf(hc->sub, sizeof hc->sub, "%.63s", cat->name);
      }
      if (cat_visible) {
        bool cover = false;
        if (cat->src[0]) {
          bool ready = ui_ensure_media_ex(ui, r, cat->src, GIF_PREVIEW_DIM, false);
          int tw = 0, th = 0;
          cover = ready && ccord_render_has_texture(r, cat->src, &tw, &th) && tw > 1 && th > 1;
          if (ready) ccord_draw_texture_cover(r, cat->src, cr, 6.0f, 255, gif_auto_play(ui));
        }
        if (cover) {
          // Dark scrim so the white label stays readable over the cover art
          ccord_draw_rrect(r, cr, 6.0f, (ccord_color){0x11, 0x12, 0x14, 110});
        } else {
          ccord_color ccol = palette_from_hash(i + 2u);
          ccord_draw_rrect(r, cr, 6.0f, ui_mix(ccol, COL(bg_tertiary), 0.35f));
        }
        ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, 12.0f, (ccord_color){255, 255, 255, 255});
        ccord_vec2 cm = ccord_text_measure(r, cat->name, strlen(cat->name), &cs, 0.0f);
        ccord_text_draw(r, cat->name, strlen(cat->name), cr.x + (cr.w - cm.x) * 0.5f,
                        cr.y + (cr.h - cm.y) * 0.5f, &cs, 0.0f);
      }
      if (col == 1 || i + 1 == ncat) y += sm_cat_h + gap;
    }

    // Trending GIFs 2-column grid directly visible below categories!
    draw_section_header(ui, r, "TRENDING GIFS", body.x + pad, y, body.w - pad * 2.0f, body);
    y += 22.0f;
    size_t ntr = ccord_store_gif_trending_count(ui->d.store);
    y = draw_gif_grid(ui, r, ep, ccord_store_gif_trending_at, ntr, body.x + pad, y, col_w, gif_cols, gap, body);
    if (ntr == 0) {
      ccord_expr_blob_state st = ccord_store_expr_blob_state(ui->d.store, CCORD_EXPR_GIF_TRENDING, nullptr);
      const char *hint = (st == CCORD_EXPR_BLOB_PENDING || st == CCORD_EXPR_BLOB_NONE)
                           ? "Loading Trending GIFs..."
                           : "No Trending GIFs right now.";
      y = draw_gif_hint(ui, r, hint, body.x + pad, y, body.w - pad * 2.0f);
    }
  }

  ep->content_h[EXPR_TAB_GIFS] = maxf((y + scroll) - body.y + 12.0f, body.h);
  ccord_pop_clip(r);
}

static void draw_sticker_cell(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, const ccord_sticker *st,
                              const char *source_name, ccord_rect cell, ccord_rect clip_area)
{
  if (!st) return;
  // Cull before recording: off-screen stickers must not consume hit slots or stay clickable.
  if (cell.y + cell.h < clip_area.y || cell.y > clip_area.y + clip_area.h) return;
  expr_hit_cell *hit = nullptr;
  if (ep->nhits < EXPR_HIT_MAX) {
    hit = &ep->hits[ep->nhits++];
    memset(hit, 0, sizeof *hit);
    hit->rect = cell;
    hit->star_rect = (ccord_rect){cell.x + cell.w - 24.0f, cell.y + 4.0f, 20.0f, 20.0f};
    hit->kind = EXPR_ITEM_STICKER;
    hit->id = st->id;
    hit->guild_id = st->guild_id;
    hit->format_type = (uint8_t)st->format;
    snprintf(hit->name, sizeof hit->name, "%.63s", st->name);
    snprintf(hit->sub, sizeof hit->sub, "%.63s", source_name ? source_name : "Sticker");
  }

  bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, clip_area) && ui_in_rect(ui->mx, ui->my, cell);
  if (hov && hit) ep->hovered_hit = (int)(ep->nhits - 1);
  // One sticker can sit in several sections (Frequently Used, a pack, Favorites): key each instance by its section too,
  // so hovering one copy does not drive the fade of another.
  uint64_t inst = ui_name_id(source_name ? source_name : "") + (uint64_t)st->id * 0x9E3779B97F4A7C15ull;
  float scell_h = ui_fade(ui, "scell", inst, hov && hit);
  if (scell_h > 0.004f) {
    ccord_draw_rrect(r, (ccord_rect){cell.x - 2.0f, cell.y - 2.0f, cell.w + 4.0f, cell.h + 4.0f}, 9.0f,
                     ui_alpha(COL(bg_item_hover), scell_h));
  }
  ui_expr_draw_sticker_art(ui, r, st->id, st->name, strlen(st->name), (uint8_t)st->format, cell, false);

  // The star eases in and out on the pointer (its own fade, so the plate and the star can be told apart).
  bool is_fav = ccord_store_is_favorite_sticker(ui->d.store, st->id);
  float star_h = ui_fade(ui, "sstar", inst, hov && hit);
  float star_a = is_fav ? 1.0f : star_h;
  if (star_a > 0.004f && hit) {
    ccord_draw_rrect(r, hit->star_rect, 4.0f, ui_alpha((ccord_color){0x11, 0x12, 0x14, 210}, star_a));
    ccord_color scol = is_fav ? (ccord_color){0xf0, 0xb2, 0x32, 255} : (ccord_color){220, 222, 228, 255};
    scol = ui_alpha(scol, star_a);
    draw_star_icon(r, hit->star_rect.x + hit->star_rect.w * 0.5f,
                   hit->star_rect.y + hit->star_rect.h * 0.5f, 6.0f, scol);
  }
}

static void draw_stickers_tab(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, ccord_rect body)
{
  float rail_w = EXPR_RAIL_W;
  float preview_h = 40.0f;
  ccord_rect rail_rect = {body.x, body.y, rail_w, body.h - preview_h};
  ccord_rect grid_area = {body.x + rail_w, body.y, body.w - rail_w, body.h - preview_h};
  ccord_rect prev_rect = {body.x, body.y + body.h - preview_h, body.w, preview_h};
  ep->hits_clip = grid_area;

  ccord_draw_rect(r, rail_rect, COL(bg_tertiary));
  ccord_draw_rect(r, (ccord_rect){grid_area.x, grid_area.y, 1.0f, grid_area.h}, COL(border_subtle));

  const char *query = ccord_text_edit_cstr(&ep->search_edit);
  float scroll = ep->scroll_y[EXPR_TAB_STICKERS];

  ui_expr_grid_layout lay;
  ui_expr_grid_layout_for_width(body.w, &lay);
  const int cols = lay.sticker_cols;   // 3 at the default popover width
  const float gap = lay.sticker_gap;
  const float pad = EXPR_GRID_PAD;
  const float cell_sz = lay.sticker_cell;
  float gx0 = grid_area.x + pad;
  float gy = grid_area.y + 6.0f - scroll;
  float gw = grid_area.w - pad * 2.0f;

  ep->nrail = 0;
  float ry = rail_rect.y + 8.0f;

  ccord_push_clip(r, grid_area);

  if (query[0] != '\0') {
    draw_section_header(ui, r, "STICKER SEARCH RESULTS", gx0, gy, gw, grid_area);
    gy += 24.0f;
    int col = 0;
    size_t ng = ccord_store_guild_count(ui->d.store);
    for (size_t gi = 0; gi < ng; gi++) {
      const ccord_guild *g = ccord_store_guild_at(ui->d.store, gi);
      if (!g) continue;
      size_t ns = ccord_store_guild_sticker_count(ui->d.store, g->id);
      for (size_t si = 0; si < ns; si++) {
        const ccord_sticker *st = ccord_store_guild_sticker_at(ui->d.store, g->id, si);
        if (!st || (!contains_ci_str(st->name, query) && !contains_ci_str(st->tags, query))) continue;
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
        draw_sticker_cell(ui, r, ep, st, g->name, cell, grid_area);
        if (++col >= cols) { col = 0; gy += cell_sz + gap; }
      }
    }
    size_t np = ccord_store_sticker_pack_count(ui->d.store);
    for (size_t pi = 0; pi < np; pi++) {
      const ccord_sticker_pack *pk = ccord_store_sticker_pack_at(ui->d.store, pi);
      if (!pk) continue;
      for (size_t si = 0; si < pk->sticker_count; si++) {
        const ccord_sticker *st = &pk->stickers[si];
        if (!contains_ci_str(st->name, query) && !contains_ci_str(st->tags, query)) continue;
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
        draw_sticker_cell(ui, r, ep, st, pk->name, cell, grid_area);
        if (++col >= cols) { col = 0; gy += cell_sz + gap; }
      }
    }
    if (col > 0) gy += cell_sz + gap;
  } else {
    int col = 0;
    // 1. FREQUENTLY USED
    size_t nfreq = ccord_store_frequent_sticker_count(ui->d.store);
    if (nfreq > 0) {
      if (ep->nrail < EXPR_RAIL_MAX) {
        ep->rail[ep->nrail++] = (expr_rail_item){
          .rect = {rail_rect.x + 6.0f, ry, 28.0f, 28.0f},
          .target_scroll_y = maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f),
          .section_id = -1,
          .label = "Frequently Used",
        };
        ry += 32.0f;
      }
      draw_section_header(ui, r, "FREQUENTLY USED", gx0, gy, gw, grid_area);
      gy += 24.0f;
      for (size_t fi = 0; fi < nfreq && fi < 6; fi++) {
        ccord_id sid = ccord_store_frequent_sticker_at(ui->d.store, fi);
        const ccord_sticker *st = ccord_store_find_sticker(ui->d.store, sid);
        if (!st) continue;
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
        draw_sticker_cell(ui, r, ep, st, "Frequently Used", cell, grid_area);
        if (++col >= cols) { col = 0; gy += cell_sz + gap; }
      }
      if (col > 0) { col = 0; gy += cell_sz + gap; }
      gy += 6.0f;
    }

    // 2. FAVORITES
    size_t nfav = ccord_store_favorite_sticker_count(ui->d.store);
    if (nfav > 0) {
      if (ep->nrail < EXPR_RAIL_MAX) {
        ep->rail[ep->nrail++] = (expr_rail_item){
          .rect = {rail_rect.x + 6.0f, ry, 28.0f, 28.0f},
          .target_scroll_y = maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f),
          .section_id = -2,
          .label = "Favorites",
        };
        ry += 32.0f;
      }
      draw_section_header(ui, r, "FAVORITE STICKERS", gx0, gy, gw, grid_area);
      gy += 24.0f;
      for (size_t fi = 0; fi < nfav; fi++) {
        ccord_id sid = ccord_store_favorite_sticker_at(ui->d.store, fi);
        const ccord_sticker *st = ccord_store_find_sticker(ui->d.store, sid);
        if (!st) continue;
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
        draw_sticker_cell(ui, r, ep, st, "Favorites", cell, grid_area);
        if (++col >= cols) { col = 0; gy += cell_sz + gap; }
      }
      if (col > 0) { col = 0; gy += cell_sz + gap; }
      gy += 6.0f;
    }

    // 3. GUILD STICKERS
    size_t ng = ccord_store_guild_count(ui->d.store);
    for (size_t gi = 0; gi < ng; gi++) {
      const ccord_guild *g = ccord_store_guild_at(ui->d.store, gi);
      if (!g) continue;
      size_t ns = ccord_store_guild_sticker_count(ui->d.store, g->id);
      if (ns == 0) continue;
      if (ep->nrail < EXPR_RAIL_MAX) {
        expr_rail_item *ri = &ep->rail[ep->nrail++];
        ri->rect = (ccord_rect){rail_rect.x + 6.0f, ry, 28.0f, 28.0f};
        ri->target_scroll_y = maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f);
        ri->section_id = 100 + (int)gi;
        ri->guild_id = g->id;
        snprintf(ri->label, sizeof ri->label, "%.31s", g->name ? g->name : "Server");
        ry += 32.0f;
      }
      draw_section_header(ui, r, g->name ? g->name : "SERVER STICKERS", gx0, gy, gw, grid_area);
      gy += 24.0f;
      for (size_t si = 0; si < ns; si++) {
        const ccord_sticker *st = ccord_store_guild_sticker_at(ui->d.store, g->id, si);
        if (!st) continue;
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
        draw_sticker_cell(ui, r, ep, st, g->name, cell, grid_area);
        if (++col >= cols) { col = 0; gy += cell_sz + gap; }
      }
      if (col > 0) { col = 0; gy += cell_sz + gap; }
      gy += 6.0f;
    }

    // 4. STANDARD STICKER PACKS
    size_t np = ccord_store_sticker_pack_count(ui->d.store);
    for (size_t pi = 0; pi < np; pi++) {
      const ccord_sticker_pack *pk = ccord_store_sticker_pack_at(ui->d.store, pi);
      if (!pk || pk->sticker_count == 0) continue;
      if (ep->nrail < EXPR_RAIL_MAX) {
        expr_rail_item *ri = &ep->rail[ep->nrail++];
        ri->rect = (ccord_rect){rail_rect.x + 6.0f, ry, 28.0f, 28.0f};
        ri->target_scroll_y = maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f);
        ri->section_id = 200 + (int)pi;
        ri->guild_id = pk->id;
        snprintf(ri->label, sizeof ri->label, "%.31s", pk->name);
        ry += 32.0f;
      }
      draw_section_header(ui, r, pk->name, gx0, gy, gw, grid_area);
      gy += 24.0f;
      for (size_t si = 0; si < pk->sticker_count; si++) {
        const ccord_sticker *st = &pk->stickers[si];
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
        draw_sticker_cell(ui, r, ep, st, pk->name, cell, grid_area);
        if (++col >= cols) { col = 0; gy += cell_sz + gap; }
      }
      if (col > 0) { col = 0; gy += cell_sz + gap; }
      gy += 6.0f;
    }
  }

  ep->content_h[EXPR_TAB_STICKERS] = maxf((gy + scroll) - grid_area.y + 12.0f, grid_area.h);
  ccord_pop_clip(r);

  // Left rail icons for Stickers tab (clipped: the shared rail cap allows more items than fit)
  ccord_push_clip(r, rail_rect);
  for (size_t i = 0; i < ep->nrail; i++) {
    const expr_rail_item *ri = &ep->rail[i];
    if (ri->rect.y + ri->rect.h < rail_rect.y || ri->rect.y > rail_rect.y + rail_rect.h) continue;
    bool active = (i + 1 == ep->nrail || (scroll >= ri->target_scroll_y - 16.0f &&
                   scroll < ep->rail[i + 1].target_scroll_y - 16.0f));
    bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rail_rect) && ui_in_rect(ui->mx, ui->my, ri->rect);
    uis_list_row_fill(ui, r, ri->rect, (uis_state){.hover = hov, .selected = active}, "erail", (uint64_t)i);
    float cx = ri->rect.x + ri->rect.w * 0.5f;
    float cy = ri->rect.y + ri->rect.h * 0.5f;
    if (ri->section_id == -1) {
      draw_clock_icon(r, cx, cy, 8.5f, active ? COL(text_header) : COL(text_secondary),
                      active ? COL(bg_item_selected) : COL(bg_tertiary));
    } else if (ri->section_id == -2) {
      draw_star_icon(r, cx, cy, 8.5f, (ccord_color){0xf0, 0xb2, 0x32, 255});
    } else {
      draw_rail_guild_icon(ui, r, ri, cx, cy);
    }
  }
  ccord_pop_clip(r);

  // Bottom hover preview bar for Stickers
  ccord_draw_rect(r, prev_rect, COL(bg_tertiary));
  ccord_draw_rect(r, (ccord_rect){prev_rect.x, prev_rect.y, prev_rect.w, 1.0f}, COL(border_subtle));
  const expr_hit_cell *preview = nullptr;
  if (ep->hovered_hit >= 0 && (size_t)ep->hovered_hit < ep->nhits) {
    preview = &ep->hits[ep->hovered_hit];
  } else if (ep->nhits > 0) {
    preview = &ep->hits[0];
  }
  if (preview) {
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    float nw = ccord_text_draw_ellipsis(r, preview->name, strlen(preview->name), prev_rect.x + 14.0f,
                                        prev_rect.y + (prev_rect.h - ui_line_h(ui, ns.size)) * 0.5f,
                                        prev_rect.w * 0.55f, &ns);
    if (preview->sub[0]) {
      ccord_text_draw_ellipsis(r, preview->sub, strlen(preview->sub), prev_rect.x + 24.0f + nw,
                               prev_rect.y + (prev_rect.h - ui_line_h(ui, ss.size)) * 0.5f,
                               prev_rect.w - (36.0f + nw), &ss);
    }
  }
}

// M5 soundboard: the current server's sounds, playable only while connected to its voice. Playback is
// a session intent (the server answers availability / USE_SOUNDBOARD); nothing is mutated locally.
static void draw_soundboard_tab(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, ccord_rect body)
{
  const char *query = ccord_text_edit_cstr(&ep->search_edit);
  float scroll = ep->scroll_y[EXPR_TAB_SOUNDBOARD];
  ep->nrail = 0;
  ccord_id guild = ccord_store_selected_guild(ui->d.store);
  ccord_id avc = ccord_store_active_voice_channel(ui->d.store);
  const ccord_channel *avch = avc != CCORD_ID_NONE ? ccord_store_channel(ui->d.store, avc) : nullptr;
  if (avch && avch->guild_id != CCORD_DM_GUILD_ID) guild = avch->guild_id;
  ccord_id self = ccord_store_self_id(ui->d.store);
  const ccord_voice_state *sv = guild != CCORD_ID_NONE ? ccord_store_voice_user(ui->d.store, guild, self) : nullptr;
  bool in_voice = sv && sv->channel_id != CCORD_ID_NONE;
  bool can_play = in_voice && ccord_store_has_permission(ui->d.store, guild, avch ? avch->id : CCORD_ID_NONE,
                                                         self, CCORD_PERM_USE_SOUNDBOARD);

  ccord_push_clip(r, body);
  ep->hits_clip = body;
  float gx = body.x + 12.0f, gw = body.w - 24.0f;
  float gy = body.y + 8.0f - scroll;
  if (!in_voice || !can_play) {
    const char *hint = !in_voice ? "Join a voice channel to play sounds."
                                 : "You need Use Soundboard permission in this channel.";
    ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
    ccord_text_draw(r, hint, strlen(hint), gx, gy, &hs, gw);
    gy += 24.0f;
  }
  size_t ns = guild != CCORD_ID_NONE ? ccord_store_guild_soundboard_count(ui->d.store, guild) : 0;
  int shown = 0;
  for (size_t i = 0; i < ns; i++) {
    const ccord_soundboard_sound *snd = ccord_store_guild_soundboard_at(ui->d.store, guild, i);
    if (!snd || !snd->available) continue;
    if (query[0] != '\0' && !contains_ci_str(snd->name, query)) continue;
    ccord_rect row = {gx, gy, gw, 40.0f};
    bool row_visible = (row.y + row.h >= body.y && row.y <= body.y + body.h);
    if (row_visible && ep->nhits < EXPR_HIT_MAX) {
      expr_hit_cell *h = &ep->hits[ep->nhits++];
      memset(h, 0, sizeof *h);
      h->rect = row;
      h->kind = EXPR_ITEM_SOUNDBOARD;
      h->id = snd->id;
      h->guild_id = guild;
      snprintf(h->name, sizeof h->name, "%s", snd->name ? snd->name : "Sound");
    }
    bool hov = ui->mouse_in && row_visible && ui_in_rect(ui->mx, ui->my, row);
    ccord_draw_rrect(r, row, uis_tokens_of(ui).r_control, COL(bg_tertiary));
    uis_list_row_fill(ui, r, row, (uis_state){.hover = hov}, "esnd", snd->id);
    ui_icon_speaker(r, row.x + 20.0f, row.y + row.h * 0.5f, 16.0f,
                    can_play ? COL(status_online) : COL(text_muted));
    ccord_text_style ns2 = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_draw_ellipsis(r, snd->name ? snd->name : "Sound", strlen(snd->name ? snd->name : "Sound"),
                             row.x + 38.0f, row.y + (row.h - ui_line_h(ui, ns2.size)) * 0.5f, gw - 110.0f, &ns2);
    // The Play pill is the shared primary button at the small height, dimmed while this user cannot play.
    float play_w = uis_label_width(ui, r, "Play");
    ccord_rect play = {row.x + row.w - UIS_SP_2 - play_w, row.y + (row.h - UIS_H_BTN_SM) * 0.5f, play_w, UIS_H_BTN_SM};
    bool play_hov = ui->mouse_in && row_visible && ui_in_rect(ui->mx, ui->my, play);
    uis_button(ui, r, play, "Play", UIS_BTN_PRIMARY, UIS_BTN_S_SM,
               (uis_state){.hover = play_hov, .disabled = !can_play}, "esndplay", snd->id);
    gy += 46.0f;
    shown++;
  }
  if (shown == 0) {
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    const char *msg = query[0] ? "No sounds match your search." : "No soundboard sounds in this server.";
    ccord_text_draw(r, msg, strlen(msg), gx, gy, &es, gw);
    gy += 24.0f;
  }
  ep->content_h[EXPR_TAB_SOUNDBOARD] = maxf((gy + scroll) - body.y + 12.0f, body.h);
  ccord_pop_clip(r);
}

ccord_rect ui_expr_soundboard_hit(const ccord_ui *ui, ccord_id sound_id)
{
  if (!ui || !ui->expr) return (ccord_rect){0, 0, 0, 0};
  const struct ui_expr_picker *ep = ui->expr;
  for (size_t i = 0; i < ep->nhits; i++) {
    if (ep->hits[i].kind == EXPR_ITEM_SOUNDBOARD && ep->hits[i].id == sound_id) return ep->hits[i].rect;
  }
  return (ccord_rect){0, 0, 0, 0};
}

// A subtle 4 px thumb on the grid's right edge while the tab's content is taller than its viewport.
static void draw_grid_scrollbar(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep)
{
  ep->grid_scrollbar_rect = (ccord_rect){0};
  float view_h = ep->hits_clip.h;
  float content_h = ep->content_h[ep->tab];
  if (view_h <= 0.0f || content_h <= view_h + 0.5f) return;
  float max_s = content_h - view_h;
  float frac = clampf(ep->scroll_y[ep->tab] / max_s, 0.0f, 1.0f);
  float w = 4.0f;
  float thumb_h = view_h * view_h / content_h;
  if (thumb_h < 20.0f) thumb_h = 20.0f;
  if (thumb_h > view_h - 8.0f) thumb_h = view_h - 8.0f;
  float top = ep->hits_clip.y + 4.0f + frac * (view_h - 8.0f - thumb_h);
  ccord_rect thumb = {ep->hits_clip.x + ep->hits_clip.w - w - 2.0f, top, w, thumb_h};
  ep->grid_scrollbar_rect = thumb;
  ccord_draw_rrect(r, thumb, w * 0.5f, ui_alpha(COL(scrollbar_thumb), 0.9f));
}

void ui_expr_picker_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !ui->emoji_picker_open) return;
  struct ui_expr_picker *ep = ensure_expr(ui);
  if (!ep) return;

  request_expression_data(ui, ep);
  compute_picker_rect(ui);
  ccord_rect box = ui->emoji_picker_rect;

  uis_popover_frame(ui, r, box);

  // Header tabs: GIFs | Stickers | Emoji | Sounds (Sounds is hidden in reaction mode)
  static const char *const k_tab_names[4] = {"GIFs", "Stickers", "Emoji", "Sounds"};
  float tab_y = box.y + UIS_SP_2;
  float tx = box.x + UIS_SP_3;
  bool reaction_mode = (ui->emoji_picker_msg_id != CCORD_ID_NONE);
  int ntabs = reaction_mode ? 3 : 4;
  for (int t = 0; t < 4; t++) {
    ep->tab_rects[t] = (ccord_rect){0, 0, 0, 0};
    if (t >= ntabs) continue;
    ccord_rect tr = {tx, tab_y, uis_label_width(ui, r, k_tab_names[t]), UIS_H_BTN_SM};
    ep->tab_rects[t] = tr;
    bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, tr);
    uis_tab(ui, r, tr, k_tab_names[t], (uis_state){.hover = hov && !reaction_mode, .selected = ep->tab == t}, "etab",
            (uint64_t)t);
    tx += tr.w + UIS_SP_2;
  }
  if (reaction_mode) {
    const char *rm_lbl = "Add Reaction";
    ccord_rect badge = {box.x + box.w - UIS_SP_3 - uis_label_width(ui, r, rm_lbl), tab_y, uis_label_width(ui, r, rm_lbl),
                        UIS_H_BTN_SM};
    uis_button(ui, r, badge, rm_lbl, UIS_BTN_PRIMARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, badge)}, "eaddrx", 0);
  }

  // Search bar + skin tone button (Emoji tab). The search is a shared input; its leading glyph and trailing clear button sit
  // in the EXPR_SEARCH_PAD insets.
  float search_y = tab_y + UIS_H_BTN_SM + UIS_SP_2;
  bool emoji_tab = (ep->tab == EXPR_TAB_EMOJI);
  float search_w = emoji_tab ? box.w - 2.0f * UIS_SP_3 - UIS_SP_2 - UIS_ICON_BTN : box.w - 2.0f * UIS_SP_3;
  ep->search_rect = (ccord_rect){box.x + UIS_SP_3, search_y, search_w, UIS_H_INPUT};

  const char *q = ccord_text_edit_cstr(&ep->search_edit);
  const char *placeholder = (ep->tab == EXPR_TAB_GIFS)     ? "Search GIFs"
                          : (ep->tab == EXPR_TAB_STICKERS) ? "Search Stickers"
                          : (ep->tab == EXPR_TAB_SOUNDBOARD) ? "Search Sounds"
                                                           : "Search Emoji";
  ui_icon_search(r, ep->search_rect.x + UIS_SP_4, ep->search_rect.y + ep->search_rect.h * 0.5f, 14.0f, COL(text_muted));
  uis_input_padded(ui, r, ep->search_rect, EXPR_SEARCH_PAD, EXPR_SEARCH_PAD, &ep->search_edit, placeholder,
                   (uis_state){.focused = ep->search_focused}, false, "esearch", 0);

  // The caret and selection rects are recorded for the inspect snapshot (the IME window); uis_input draws them.
  const float qx = ep->search_rect.x + EXPR_SEARCH_PAD;
  const float qw = ep->search_rect.w - 2.0f * EXPR_SEARCH_PAD;
  ccord_text_style qs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
  const float qlh = ui_line_h(ui, qs.size);
  const float qy = ep->search_rect.y + (ep->search_rect.h - qlh) * 0.5f;
  ep->search_caret_rect = (ccord_rect){0};
  ep->search_selection_rect = (ccord_rect){0};
  if (ep->search_focused && q[0] && ep->search_edit.caret != ep->search_edit.anchor) {
    size_t from = ep->search_edit.caret < ep->search_edit.anchor ? ep->search_edit.caret : ep->search_edit.anchor;
    size_t to = ep->search_edit.caret < ep->search_edit.anchor ? ep->search_edit.anchor : ep->search_edit.caret;
    float sx0 = qx + ccord_text_measure(r, q, from, &qs, 0.0f).x;
    float sx1 = qx + ccord_text_measure(r, q, to, &qs, 0.0f).x;
    if (sx0 < qx) sx0 = qx;
    if (sx1 > qx + qw) sx1 = qx + qw;
    if (sx1 > sx0) ep->search_selection_rect = (ccord_rect){sx0, qy, sx1 - sx0, qlh};
  }
  if (ep->search_focused && ui->caret_on) {
    size_t caret = ep->search_edit.caret <= ep->search_edit.len ? ep->search_edit.caret : ep->search_edit.len;
    float cx = qx + ccord_text_measure(r, q, caret, &qs, 0.0f).x;
    if (cx < qx) cx = qx;
    if (cx > qx + qw - 1.5f) cx = qx + qw - 1.5f;
    ep->search_caret_rect = (ccord_rect){cx, qy + 1.0f, 1.5f, qlh - 2.0f};
  }

  if (q[0]) {
    ep->search_clear_rect = (ccord_rect){ep->search_rect.x + ep->search_rect.w - UIS_ICON_BTN - 2.0f,
                                         ep->search_rect.y + (ep->search_rect.h - UIS_ICON_BTN) * 0.5f, UIS_ICON_BTN,
                                         UIS_ICON_BTN};
    bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ep->search_clear_rect);
    ccord_color cc = uis_icon_button(ui, r, ep->search_clear_rect, (uis_state){.hover = chv}, "esearchclr", 0);
    ui_icon_close(r, ep->search_clear_rect.x + ep->search_clear_rect.w * 0.5f,
                  ep->search_clear_rect.y + ep->search_clear_rect.h * 0.5f, UIS_ICON_GLYPH, cc);
  } else {
    ep->search_clear_rect = (ccord_rect){0, 0, 0, 0};
  }

  if (emoji_tab) {
    ep->skin_btn_rect = (ccord_rect){ep->search_rect.x + ep->search_rect.w + UIS_SP_2,
                                     search_y + (UIS_H_INPUT - UIS_ICON_BTN) * 0.5f, UIS_ICON_BTN, UIS_ICON_BTN};
    bool shov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ep->skin_btn_rect);
    uis_icon_button(ui, r, ep->skin_btn_rect, (uis_state){.hover = shov, .selected = ep->skin_popup_open}, "eskinbtn", 0);
    const char *sw = k_skin_tone_swatches[ep->skin_tone % 6u];
    ccord_text_style sws = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
    ccord_vec2 swm = ccord_text_measure(r, sw, strlen(sw), &sws, 0.0f);
    ccord_text_draw(r, sw, strlen(sw), ep->skin_btn_rect.x + (ep->skin_btn_rect.w - swm.x) * 0.5f,
                    ep->skin_btn_rect.y + (ep->skin_btn_rect.h - swm.y) * 0.5f, &sws, 0.0f);
  } else {
    ep->skin_btn_rect = (ccord_rect){0, 0, 0, 0};
  }

  float body_y = search_y + UIS_H_INPUT + UIS_SP_2 + 1.0f;
  ccord_draw_rect(r, (ccord_rect){box.x, body_y - 1.0f, box.w, 1.0f}, COL(border_subtle));

  // Tab Body
  ccord_rect body = {box.x, body_y, box.w, box.y + box.h - body_y};
  ep->nhits = 0;
  ep->hovered_hit = -1;
  memset(ui->emoji_picker_cells, 0, sizeof ui->emoji_picker_cells);   // re-recorded by the Emoji tab only

  if (ep->tab == EXPR_TAB_EMOJI) {
    draw_emoji_tab(ui, r, ep, body);
  } else if (ep->tab == EXPR_TAB_GIFS) {
    draw_gifs_tab(ui, r, ep, body);
  } else if (ep->tab == EXPR_TAB_SOUNDBOARD) {
    draw_soundboard_tab(ui, r, ep, body);
  } else {
    draw_stickers_tab(ui, r, ep, body);
  }
  draw_grid_scrollbar(ui, r, ep);
}

// ---------------------------------------------------------------- event handling

static void activate_emoji_hit(ccord_ui *ui, const expr_hit_cell *h, bool keep_open)
{
  if (!ui || !h) return;
  ccord_error err;
  if (h->name[0]) {
    ccord_store_record_emoji_use(ui->d.store, h->name);
  }
  if (ui->emoji_picker_for_status) {
    // Status card mode: the pick becomes the custom status emoji; the card stays open.
    if (h->kind == EXPR_ITEM_CUSTOM_EMOJI) {
      snprintf(ui->sc.emoji, sizeof ui->sc.emoji, "%s", h->name);
      ui->sc_emoji_id = h->id;
    } else if (h->utf8[0]) {
      snprintf(ui->sc.emoji, sizeof ui->sc.emoji, "%s", h->utf8);
      ui->sc_emoji_id = CCORD_ID_NONE;
    }
    ui_expr_picker_close(ui);
    return;
  }
  if (ui->emoji_picker_msg_id != CCORD_ID_NONE) {
    // Reaction mode
    if (ui->d.session) {
      if (h->kind == EXPR_ITEM_CUSTOM_EMOJI) {
        char react_key[96];
        int n = snprintf(react_key, sizeof react_key, "%s:%llu", h->name, (unsigned long long)h->id);
        if (n > 0) {
          (void)ccord_session_toggle_reaction(ui->d.session, ui->emoji_picker_channel, ui->emoji_picker_msg_id,
                                              react_key, (size_t)n, &err);
        }
      } else if (h->utf8[0]) {
        (void)ccord_session_toggle_reaction(ui->d.session, ui->emoji_picker_channel, ui->emoji_picker_msg_id,
                                            h->utf8, strlen(h->utf8), &err);
      }
    }
    ui_expr_picker_close(ui);
    return;
  }

  // Composer insertion mode
  char ins[128];
  int ilen = 0;
  if (h->kind == EXPR_ITEM_CUSTOM_EMOJI) {
    ilen = snprintf(ins, sizeof ins, "<%s:%s:%llu> ", h->animated ? "a" : "", h->name, (unsigned long long)h->id);
  } else if (h->utf8[0]) {
    ilen = snprintf(ins, sizeof ins, "%s ", h->utf8);
  }
  if (ilen > 0 && (size_t)ilen < sizeof ins) {
    (void)ccord_text_edit_insert(&ui->edit, ins, (size_t)ilen, &err);
    ui->caret_epoch_ms = ui->now_ms;
    ui->caret_on = true;
  }
  if (!keep_open) {
    ui_expr_picker_close(ui);
  }
  ui->dirty = true;
}

bool ui_expr_handle_event(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ui->emoji_picker_open || !ev) return false;
  struct ui_expr_picker *ep = ensure_expr(ui);
  if (!ep) return false;

  compute_picker_rect(ui);
  ccord_rect box = ui->emoji_picker_rect;

  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    if (ui_in_rect(ev->x, ev->y, box)) {
      ui->dirty = true;
      return true;
    }
    return false;
  }

  if (ev->type == CCORD_IN_WHEEL) {
    if (!ui_in_rect(ev->x, ev->y, box)) return false;
    float step = 42.0f * 2.5f;
    float dy = -ev->wheel * step;
    bool direct = ui_wheel_direct(ui, ev);
    if (ep->tab == EXPR_TAB_EMOJI && ep->rail_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ep->rail_rect)) {
      // Wheel over the 40px rail scrolls only the rail, never the emoji grid. The rail's rects move with its offset.
      float off = ep->rail_scroll_y;
      ccord_scroller_scroll(&ep->rail_glide, &off, dy, maxf(ep->rail_content_h - ep->rail_view.h, 0.0f), direct);
      expr_rail_set_scroll(ep, off);
    } else {
      int t = ep->tab;
      float max_s = maxf(ep->content_h[t] - (box.h - 125.0f), 0.0f);
      ccord_scroller_scroll(&ep->glide[t], &ep->scroll_y[t], dy, max_s, direct);
    }
    ui->dirty = true;
    return true;
  }

  if (ev->type == CCORD_IN_MOUSE_DOWN) {
    // 1. Check skin-tone popup first if open
    if (ep->skin_popup_open && ev->button == 1) {
      for (int t = 0; t < 6; t++) {
        if (ui_in_rect(ev->x, ev->y, ep->skin_option_rects[t])) {
          ep->skin_tone = (uint8_t)t;
          ep->skin_popup_open = false;
          ui->dirty = true;
          return true;
        }
      }
      ep->skin_popup_open = false;
      ui->dirty = true;
      if (ui_in_rect(ev->x, ev->y, ep->skin_btn_rect)) return true;
    }

    // 3. Click outside popover -> check composer buttons or close
    if (!ui_in_rect(ev->x, ev->y, box)) {
      if (ev->button == 1 && ui_expr_handle_composer_click(ui, ev->x, ev->y)) {
        return true;
      }
      ui_expr_picker_close(ui);
      return true;
    }

    // 4. Header tab switching
    if (ev->button == 1) {
      for (int t = 0; t < 4; t++) {
        if (ep->tab_rects[t].w > 0.0f && ui_in_rect(ev->x, ev->y, ep->tab_rects[t])) {
          ep->tab = t;
          ep->skin_popup_open = false;
          ui->dirty = true;
          return true;
        }
      }
      if (ep->search_clear_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ep->search_clear_rect)) {
        ccord_text_edit_clear(&ep->search_edit);
        ep->gif_subview = 0;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, ep->search_rect)) {
        ep->search_focused = true;
        ui->dirty = true;
        return true;
      }
      if (ep->tab == EXPR_TAB_EMOJI && ui_in_rect(ev->x, ev->y, ep->skin_btn_rect)) {
        ep->skin_popup_open = !ep->skin_popup_open;
        ui->dirty = true;
        return true;
      }
      if (ep->tab == EXPR_TAB_GIFS && ep->back_btn_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ep->back_btn_rect)) {
        ep->gif_subview = 0;
        ep->scroll_y[EXPR_TAB_GIFS] = 0.0f;
        ui->dirty = true;
        return true;
      }
      // Pinned Default Unicode Emoji button: clear any search and jump to the Unicode categories.
      if (ep->tab == EXPR_TAB_EMOJI && ep->rail_pinned_rect.w > 0.0f &&
          ui_in_rect(ev->x, ev->y, ep->rail_pinned_rect)) {
        if (ep->search_edit.len > 0) ccord_text_edit_clear(&ep->search_edit);
        ep->pending_unicode_jump = true;
        ui->dirty = true;
        return true;
      }
      // Left category rail clicks (Emoji rail items are only live inside the scrollable rail view)
      if (ep->tab != EXPR_TAB_EMOJI || ui_in_rect(ev->x, ev->y, ep->rail_view)) {
        for (size_t ri = 0; ri < ep->nrail; ri++) {
          if (ui_in_rect(ev->x, ev->y, ep->rail[ri].rect)) {
            ep->scroll_y[ep->tab] = ep->rail[ri].target_scroll_y;
            ui->dirty = true;
            return true;
          }
        }
      }
    }

    // 5. Grid item hits (only inside the tab's viewport: partially visible cells must not be
    // clickable through the preview bar or outside the clipped grid)
    for (size_t i = 0; i < ep->nhits; i++) {
      const expr_hit_cell *h = &ep->hits[i];
      if (!ui_in_rect(ev->x, ev->y, ep->hits_clip) || !ui_in_rect(ev->x, ev->y, h->rect)) continue;
      ccord_error err;
      if (h->kind == EXPR_ITEM_UNICODE_EMOJI || h->kind == EXPR_ITEM_CUSTOM_EMOJI) {
        if (ev->button == 3) {
          if (h->name[0]) (void)ccord_store_toggle_favorite_emoji(ui->d.store, h->name);
          ui->dirty = true;
          return true;
        }
        bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
        activate_emoji_hit(ui, h, shift);
        return true;
      }
      if (h->kind == EXPR_ITEM_GIF_FAV_CAT) {
        ep->gif_subview = 1;
        ep->scroll_y[EXPR_TAB_GIFS] = 0.0f;
        ui->dirty = true;
        return true;
      }
      if (h->kind == EXPR_ITEM_GIF_TREND_CAT) {
        ep->gif_subview = 0;
        ep->scroll_y[EXPR_TAB_GIFS] = 160.0f;
        ui->dirty = true;
        return true;
      }
      if (h->kind == EXPR_ITEM_GIF_CATEGORY) {
        ccord_text_edit_clear(&ep->search_edit);
        const char *term = h->sub[0] ? h->sub : h->name;
        (void)ccord_text_edit_insert(&ep->search_edit, term, strlen(term), &err);
        if (ui->d.session) {
          (void)ccord_session_request_expression_data(ui->d.session, CCORD_EXPR_GIF_SEARCH, term, true, &err);
        }
        ep->scroll_y[EXPR_TAB_GIFS] = 0.0f;
        ui->dirty = true;
        return true;
      }
      if (h->kind == EXPR_ITEM_GIF_TILE) {
        if (ev->button == 3 || ui_in_rect(ev->x, ev->y, h->star_rect)) {
          ccord_gif_item gitem = {0};
          snprintf(gitem.url, sizeof gitem.url, "%s", h->url);
          snprintf(gitem.src, sizeof gitem.src, "%s", h->preview_url);
          gitem.width = h->gif_w;
          gitem.height = h->gif_h;
          gitem.format = h->gif_format;
          // Starring a GIF is a Discord setting: the session toggles the store and PATCHes the settings proto, so
          // the favorite survives a restart and shows in the official client (a failed PATCH rolls the store back).
          // Only a picker with no session (tests) falls back to the local-only toggle.
          if (ui->d.session) {
            if (ccord_session_toggle_favorite_gif(ui->d.session, &gitem, &err) != CCORD_OK) {
              ui_notice(ui, "%s", err.message);
            }
          } else {
            (void)ccord_store_toggle_favorite_gif(ui->d.store, &gitem);
          }
          ui->dirty = true;
          return true;
        }
        ccord_id ch_id = ccord_store_selected_channel(ui->d.store);
        if (ui->d.session && ch_id != CCORD_ID_NONE && h->url[0]) {
          (void)ccord_session_send_message(ui->d.session, ch_id, h->url, strlen(h->url), &err);
          ccord_vlist_reset_bottom(&ui->vlist);
        }
        ui_expr_picker_close(ui);
        return true;
      }
      if (h->kind == EXPR_ITEM_STICKER) {
        if (ev->button == 3 || ui_in_rect(ev->x, ev->y, h->star_rect)) {
          (void)ccord_store_toggle_favorite_sticker(ui->d.store, h->id);
          ui->dirty = true;
          return true;
        }
        ccord_store_record_sticker_use(ui->d.store, h->id);
        ccord_id ch_id = ccord_store_selected_channel(ui->d.store);
        if (ui->d.session && ch_id != CCORD_ID_NONE && h->id != CCORD_ID_NONE) {
          (void)ccord_session_send_sticker(ui->d.session, ch_id, h->id, &err);
          ccord_vlist_reset_bottom(&ui->vlist);
        }
        ui_expr_picker_close(ui);
        return true;
      }
      if (h->kind == EXPR_ITEM_SOUNDBOARD) {
        // The session gates on active voice; the server answers availability / USE_SOUNDBOARD.
        if (ui->d.session && h->id != CCORD_ID_NONE) {
          if (ccord_session_play_soundboard(ui->d.session, h->guild_id, h->id, &err) != CCORD_OK) {
            ui_notice(ui, "%s", err.message);
          }
        }
        ui->dirty = true;
        return true;
      }
    }
    return true;
  }

  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_ESCAPE) {
      ui_expr_picker_close(ui);
      return true;
    }
    bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
    bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
    switch (ev->key) {
      case CCORD_KEY_BACKSPACE: ccord_text_edit_backspace(&ep->search_edit); break;
      case CCORD_KEY_DELETE:    ccord_text_edit_delete(&ep->search_edit); break;
      case CCORD_KEY_LEFT:      ccord_text_edit_move(&ep->search_edit, -1, shift); break;
      case CCORD_KEY_RIGHT:     ccord_text_edit_move(&ep->search_edit, 1, shift); break;
      case CCORD_KEY_HOME:      ccord_text_edit_move(&ep->search_edit, -2, shift); break;
      case CCORD_KEY_END:       ccord_text_edit_move(&ep->search_edit, 2, shift); break;
      case CCORD_KEY_A:
        if (ctrl) ccord_text_edit_select_all(&ep->search_edit);
        break;
      case CCORD_KEY_RETURN:
        if (ep->tab == EXPR_TAB_GIFS && ep->search_edit.len > 0 && ui->d.session) {
          ccord_error err;
          (void)ccord_session_request_expression_data(ui->d.session, CCORD_EXPR_GIF_SEARCH,
                                                      ccord_text_edit_cstr(&ep->search_edit), true, &err);
        } else if (ep->nhits > 0) {
          const expr_hit_cell *h = &ep->hits[0];
          if (h->kind == EXPR_ITEM_UNICODE_EMOJI || h->kind == EXPR_ITEM_CUSTOM_EMOJI) {
            activate_emoji_hit(ui, h, shift);
          }
        }
        ui->dirty = true;
        return true;
      default:
        return false;
    }
    if (ep->tab == EXPR_TAB_GIFS && ep->search_edit.len >= 2 && ui->d.session) {
      ccord_error err;
      (void)ccord_session_request_expression_data(ui->d.session, CCORD_EXPR_GIF_SEARCH,
                                                  ccord_text_edit_cstr(&ep->search_edit), true, &err);
    }
    ep->scroll_y[ep->tab] = 0.0f;
    ui->dirty = true;
    return true;
  }

  if (ev->type == CCORD_IN_TEXT) {
    size_t n = strnlen(ev->text, sizeof ev->text);
    ccord_error err;
    (void)ccord_text_edit_insert(&ep->search_edit, ev->text, n, &err);
    if (ep->tab == EXPR_TAB_GIFS && ep->search_edit.len >= 2 && ui->d.session) {
      (void)ccord_session_request_expression_data(ui->d.session, CCORD_EXPR_GIF_SEARCH,
                                                  ccord_text_edit_cstr(&ep->search_edit), true, &err);
    }
    ep->scroll_y[ep->tab] = 0.0f;
    ui->dirty = true;
    return true;
  }

  return false;
}

// ---------------------------------------------------------------- read-only inspection (tests/slices)

bool ui_expr_get_inspect(const ccord_ui *ui, ui_expr_inspect *out)
{
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!ui || !ui->expr) return false;
  const struct ui_expr_picker *ep = ui->expr;
  out->tab = ep->tab;
  out->gif_subview = ep->gif_subview;
  out->nhits = ep->nhits;
  out->nrail = ep->nrail;
  out->rail_cap = EXPR_RAIL_MAX;
  out->search_len = ep->search_edit.len;
  out->grid_scroll_y = ep->scroll_y[ep->tab];
  out->grid_content_h = ep->content_h[ep->tab];
  out->rail_scroll_y = ep->rail_scroll_y;
  out->rail_content_h = ep->rail_content_h;
  out->rail_rect = ep->rail_rect;
  out->rail_view = ep->rail_view;
  out->rail_pinned_rect = ep->rail_pinned_rect;
  out->grid_view = ep->hits_clip;
  out->search_rect = ep->search_rect;
  out->grid_scrollbar_rect = ep->grid_scrollbar_rect;
  out->search_caret_rect = ep->search_caret_rect;
  out->search_selection_rect = ep->search_selection_rect;
  out->hit_url_cap = sizeof ep->hits[0].url;
  out->hit_preview_url_cap = sizeof ep->hits[0].preview_url;
  return true;
}

bool ui_expr_rail_at(const ccord_ui *ui, size_t index, ui_expr_rail_info *out)
{
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!ui || !ui->expr || index >= ui->expr->nrail) return false;
  const expr_rail_item *ri = &ui->expr->rail[index];
  out->rect = ri->rect;
  out->section_id = ri->section_id;
  out->guild_id = ri->guild_id;
  out->target_scroll_y = ri->target_scroll_y;
  snprintf(out->label, sizeof out->label, "%s", ri->label);
  return true;
}

bool ui_expr_hit_at(const ccord_ui *ui, size_t index, ui_expr_hit_info *out)
{
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!ui || !ui->expr || index >= ui->expr->nhits) return false;
  const expr_hit_cell *h = &ui->expr->hits[index];
  out->rect = h->rect;
  out->star_rect = h->star_rect;
  out->kind = (int)h->kind;
  out->id = h->id;
  out->guild_id = h->guild_id;
  snprintf(out->name, sizeof out->name, "%s", h->name);
  snprintf(out->sub, sizeof out->sub, "%s", h->sub);
  snprintf(out->utf8, sizeof out->utf8, "%s", h->utf8);
  snprintf(out->url, sizeof out->url, "%s", h->url);
  snprintf(out->preview_url, sizeof out->preview_url, "%s", h->preview_url);
  return true;
}

// ---------------------------------------------------------------- :shortcode: replacement

static bool is_sc_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-';
}

size_t ui_expr_replace_shortcodes(const ccord_store *store, ccord_id preferred_guild_id,
                                  const char *in, size_t in_len, char *out, size_t out_cap)
{
  (void)preferred_guild_id;
  if (!out || out_cap == 0) return 0;
  if (!in || in_len == 0) {
    out[0] = '\0';
    return 0;
  }

  // Pass 1: expand guild custom emoji :shortcode: into <:name:id> or <a:name:id>
  char mid[CCORD_SEND_MAX_BYTES + 1];
  size_t mpos = 0;
  size_t i = 0;
  bool in_code = false;
  while (i < in_len && mpos + 1 < sizeof mid) {
    if (in[i] == '`') {
      in_code = !in_code;
      mid[mpos++] = in[i++];
      continue;
    }
    if (!in_code && in[i] == '<') {
      // Copy angle-bracket tokens (<:name:id>, <@123>, <https://...>) verbatim
      size_t j = i + 1;
      while (j < in_len && in[j] != '>' && in[j] != '\n' && in[j] != ' ') j++;
      if (j < in_len && in[j] == '>') {
        size_t span = j + 1 - i;
        if (mpos + span < sizeof mid) {
          memcpy(mid + mpos, in + i, span);
          mpos += span;
          i = j + 1;
          continue;
        }
      }
    }
    if (!in_code && in[i] == ':' &&
        !(i >= 4 && strncasecmp(in + i - 4, "http", 4) == 0) &&
        !(i >= 5 && strncasecmp(in + i - 5, "https", 5) == 0)) {
      size_t j = i + 1;
      while (j < in_len && (j - (i + 1)) < 48u && is_sc_char(in[j])) j++;
      if (j > i + 1 && j < in_len && in[j] == ':') {
        char sc_name[52];
        size_t nlen = j - (i + 1);
        memcpy(sc_name, in + i + 1, nlen);
        sc_name[nlen] = '\0';
        const ccord_emoji *ce = store ? ccord_store_find_emoji_by_name(store, sc_name) : nullptr;
        if (ce) {
          char tok[96];
          int tlen = snprintf(tok, sizeof tok, "<%s:%s:%llu>",
                              ce->animated ? "a" : "", ce->name, (unsigned long long)ce->id);
          if (tlen > 0 && mpos + (size_t)tlen < sizeof mid) {
            memcpy(mid + mpos, tok, (size_t)tlen);
            mpos += (size_t)tlen;
            i = j + 1;
            continue;
          }
        }
      }
    }
    mid[mpos++] = in[i++];
  }
  mid[mpos] = '\0';

  // Pass 2: expand standard Unicode :shortcode: tokens via ccord_emoji_replace_shortcodes
  return ccord_emoji_replace_shortcodes(mid, mpos, out, out_cap);
}
