// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "expr_internal.h"


static float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

float ep_maxf(float a, float b)
{
  return a > b ? a : b;
}

// Clamp the Emoji rail scroll and shift the cached item rects with it, so hit-tests stay consistent
// between wheel events and the next draw (no off-screen ghost clicks).
void ep_expr_rail_set_scroll(struct ui_expr_picker *ep, float scroll)
{
  if (!ep || !ep->body) return;
  ui_expr_body *b = ep->body;
  float *cur = (ep->tab == EXPR_TAB_STICKERS) ? &ep->sticker_rail_scroll_y : &ep->rail_scroll_y;
  float max_s = ep_maxf(b->rail_content_h - b->rail_view.h, 0.0f);
  scroll = clampf(scroll, 0.0f, max_s);
  float delta = *cur - scroll;
  if (delta == 0.0f) return;
  for (size_t i = 0; i < b->nrail; i++) b->rail[i].rect.y += delta;
  *cur = scroll;
}

static struct ui_expr_picker *ensure_expr(ccord_ui *ui)
{
  if (!ui) return nullptr;
  if (ui->expr) return ui->expr;
  struct ui_expr_picker *ep = ccord_calloc(1, sizeof *ep);
  if (!ep) return nullptr;
  ep->tab = EXPR_TAB_EMOJI;
  ep->btn_hover = -1;
  ui->expr = ep;
  return ep;
}

static ui_expr_body *ensure_expr_body(ccord_ui *ui)
{
  struct ui_expr_picker *ep = ensure_expr(ui);
  if (!ep) return nullptr;
  if (ep->body) return ep->body;
  ui_expr_body *b = ccord_calloc(1, sizeof *b);
  if (!b) return nullptr;
  b->hovered_hit = -1;
  b->last_hover_key = -1;
  ccord_text_edit_init(&b->search_edit, 128);
  ep->body = b;
  return b;
}

static void expr_body_free(struct ui_expr_picker *ep)
{
  if (!ep || !ep->body) return;
  ui_expr_body *b = ep->body;
  ccord_text_edit_free(&b->search_edit);
  ccord_free(b->emoji_custom_hits);
  ccord_free(b->sticker_hits);
  ccord_free(b->sb_hits);
  ccord_free(b);
  ep->body = nullptr;
}

void ui_expr_picker_destroy(ccord_ui *ui)
{
  if (!ui || !ui->expr) return;
  expr_body_free(ui->expr);
  ccord_free(ui->expr);
  ui->expr = nullptr;
}

// Fetch one picker blob by state: NONE asks once, FAILED retries with force after the cooldown;
// PENDING/READY are left alone (the session also drops duplicates). No one-shot latch: reopening the
// picker or switching back to GIFs recovers blobs that failed earlier.
static void request_expr_blob(ccord_ui *ui, ui_expr_body *b, ccord_expr_kind kind)
{
  if (!ui->d.session || !ui->d.store || !b) return;
  ccord_expr_blob_state st = ccord_store_expr_blob_state(ui->d.store, kind, nullptr);
  if (st != CCORD_EXPR_BLOB_NONE && st != CCORD_EXPR_BLOB_FAILED) return;
  if (st == CCORD_EXPR_BLOB_FAILED && ui->now_ms < b->expr_retry_after_ms[kind]) return;
  b->expr_retry_after_ms[kind] = ui->now_ms + EXPR_FETCH_RETRY_MS;
  ccord_error err;
  (void)ccord_session_request_expression_data(ui->d.session, kind, nullptr, st == CCORD_EXPR_BLOB_FAILED, &err);
}

static void request_expression_data(ccord_ui *ui, ui_expr_body *b)
{
  if (!ui || !b) return;
  request_expr_blob(ui, b, CCORD_EXPR_GIF_CATEGORIES);
  request_expr_blob(ui, b, CCORD_EXPR_GIF_TRENDING);
  // Favorite GIFs + frequent emojis both live in the settings proto; local frecency must not gate it.
  request_expr_blob(ui, b, CCORD_EXPR_SETTINGS_PROTO);
  request_expr_blob(ui, b, CCORD_EXPR_STICKER_PACKS);
}

// One layout for every picker grid, derived from the popover width: Emoji cells, GIF tile columns and Sticker
// columns all come from here, so no recorded rect can leave its grid. At the default 428 px popover this is 8 emoji
// columns, 2 GIF columns (200 px tiles) and 3 sticker columns.

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
  if (ui->mobile_layout) {
    float sl = ui->safe_insets.left;
    float sr = ui->safe_insets.right;
    float sb = ep_maxf(ui->safe_insets.bottom, ui->keyboard_inset);
    float st = ui->safe_insets.top;
    float pw = ep_maxf(ui->win_w - sl - sr - 2.0f * UIS_SP_2, 240.0f);
    float max_h = ep_maxf((ui->win_h - st - sb) * 0.58f, 220.0f);
    float ph = fminf(EXPR_POPOVER_H, max_h);
    float px = sl + UIS_SP_2;
    float py = ep_maxf(st + UIS_SP_2, ui->win_h - sb - UIS_SP_2 - ph);
    ui->emoji_picker_rect = (ccord_rect){px, py, pw, ph};
    return;
  }
  float pw = EXPR_POPOVER_W;
  float ph = EXPR_POPOVER_H;
  if (pw > ui->win_w - 16.0f) pw = ep_maxf(ui->win_w - 16.0f, 260.0f);
  if (ph > ui->win_h - 16.0f) ph = ep_maxf(ui->win_h - 16.0f, 240.0f);

  ccord_rect cbox = ui_composer_box(ui);
  float px = cbox.x + cbox.w - pw;
  float py = cbox.y - ph - 8.0f;
  if (px < 8.0f) px = 8.0f;
  if (px + pw > ui->win_w - 8.0f) px = ui->win_w - pw - 8.0f;
  if (py < 8.0f) py = 8.0f;
  if (py + ph > ui->win_h - 8.0f) py = ui->win_h - ph - 8.0f;
  ui->emoji_picker_rect = (ccord_rect){px, py, pw, ph};
}

static bool expr_flush_gif_search(ccord_ui *ui, struct ui_expr_picker *ep)
{
  if (!ui || !ep || !ep->body) return false;
  ui_expr_body *b = ep->body;
  if (!b->gif_search_pending) return false;
  if (!ui->emoji_picker_open || ep->tab != EXPR_TAB_GIFS || b->search_edit.len < 2 || !ui->d.session) {
    b->gif_search_pending = false;
    return false;
  }
  if (ui->now_ms < b->gif_search_due_ms) return true;
  b->gif_search_pending = false;
  ccord_error err;
  (void)ccord_session_request_expression_data(ui->d.session, CCORD_EXPR_GIF_SEARCH,
                                              ccord_text_edit_cstr(&b->search_edit), true, &err);
  ui->dirty = true;
  return false;
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

  ui_expr_body *b = ensure_expr_body(ui);
  if (!b) return;

  ep->tab = tab;
  b->skin_popup_open = false;
  b->search_focused = true;
  b->hovered_hit = -1;
  b->last_hover_key = -1;
  b->gif_search_pending = false;
  if (tab == EXPR_TAB_GIFS && b->search_edit.len == 0) {
    b->gif_subview = 0;
    b->gif_category_label[0] = '\0';
  }
  ui->emoji_picker_open = true;
  ui->emoji_picker_channel = channel_id != CCORD_ID_NONE ? channel_id : ccord_store_selected_channel(ui->d.store);
  ui->emoji_picker_msg_id = reaction_msg_id;
  ui->emoji_picker_for_status = false;
  ui->autocomplete_open = false;
  request_expression_data(ui, b);
  compute_picker_rect(ui);

  // No cell exists until the first draw records the real ones: a click before then lands on nothing.
  memset(ui->emoji_picker_cells, 0, sizeof ui->emoji_picker_cells);
  ui->dirty = true;
}

// Ticks the wheel glides of the picker's grids and rail by fdt_ms (scroll.h). Dropped when the picker is closed.
bool ui_expr_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  struct ui_expr_picker *ep = ui->expr;
  if (!ep || !ep->body) return false;
  ui_expr_body *b = ep->body;
  bool moving = false;
  bool open = ui->emoji_picker_open;
  for (int t = 0; t < 4; t++) {
    if (b->glide[t].pending == 0.0f) continue;
    if (!open) {
      ccord_scroller_cancel(&b->glide[t]);
      continue;
    }
    moving = ccord_scroller_tick(&b->glide[t], &ep->scroll_y[t], fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced) || moving;
    ui->dirty = true;
  }
  if (b->rail_glide.pending != 0.0f) {
    if (!open) {
      ccord_scroller_cancel(&b->rail_glide);
    } else {
      float off = (ep->tab == EXPR_TAB_STICKERS) ? ep->sticker_rail_scroll_y : ep->rail_scroll_y;
      moving = ccord_scroller_tick(&b->rail_glide, &off, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced) || moving;
      ep_expr_rail_set_scroll(ep, off);
      ui->dirty = true;
    }
  }
  if (expr_flush_gif_search(ui, ep)) moving = true;
  return moving;
}

// Offset of the active tab's grid and how far it can travel (verify-slice seam; room is 0 when the picker is absent).
void ui_expr_scroll_debug(const ccord_ui *ui, float *scroll, float *room)
{
  const struct ui_expr_picker *ep = ui ? ui->expr : nullptr;
  const ui_expr_body *b = ep ? ep->body : nullptr;
  int t = ep ? ep->tab : 0;
  if (scroll) *scroll = ep ? ep->scroll_y[t] : 0.0f;
  if (room) *room = b ? ep_maxf(b->content_h[t] - (ui->emoji_picker_rect.h - 125.0f), 0.0f) : 0.0f;
}

bool ui_expr_wants_text(const ccord_ui *ui)
{
  if (!ui || !ui->emoji_picker_open || !ui->expr || !ui->expr->body) return false;
  return ui->expr->body->search_focused;
}

void ui_expr_picker_close(ccord_ui *ui)
{
  if (!ui) return;
  ui->emoji_picker_open = false;
  ui->emoji_picker_msg_id = CCORD_ID_NONE;
  ui->emoji_picker_for_status = false;
  if (ui->touch_mode) ui->touch_text_focused = false;
  // Defer freeing ep->body until the close fade-out finishes so draw_overlay_popup doesn't re-allocate it
  // every animation frame; free immediately under reduced motion or when no overlay presence is active.
  if (ui->expr && (ui->anim.reduced_motion || ui->dbg_ovl_t[UI_OVL_PICKER] <= UI_PRESENCE_EPS)) {
    expr_body_free(ui->expr);
  }
  ui->dirty = true;
}

// ---------------------------------------------------------------- procedural icons & artwork

void ui_expr_draw_smiley(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float rad = size * 0.46f;
  ccord_draw_circle(r, cx, cy, rad, c);
  ccord_color hole = {0x2b, 0x2d, 0x31, 255};
  ccord_draw_circle(r, cx - rad * 0.34f, cy - rad * 0.18f, ep_maxf(rad * 0.13f, 1.3f), hole);
  ccord_draw_circle(r, cx + rad * 0.34f, cy - rad * 0.18f, ep_maxf(rad * 0.13f, 1.3f), hole);
  // Smile curve
  ccord_draw_line(r, (ccord_vec2){cx - rad * 0.38f, cy + rad * 0.18f},
                  (ccord_vec2){cx, cy + rad * 0.44f}, ep_maxf(size * 0.09f, 1.4f), hole);
  ccord_draw_line(r, (ccord_vec2){cx, cy + rad * 0.44f},
                  (ccord_vec2){cx + rad * 0.38f, cy + rad * 0.18f}, ep_maxf(size * 0.09f, 1.4f), hole);
}

void ep_draw_star_icon(ccord_renderer *r, float cx, float cy, float rad, ccord_color c)
{
  ccord_draw_triangle(r, (ccord_vec2){cx, cy - rad},
                      (ccord_vec2){cx - rad * 0.65f, cy + rad * 0.75f},
                      (ccord_vec2){cx + rad * 0.65f, cy + rad * 0.75f}, c);
  ccord_draw_triangle(r, (ccord_vec2){cx - rad * 0.95f, cy - rad * 0.25f},
                      (ccord_vec2){cx + rad * 0.95f, cy - rad * 0.25f},
                      (ccord_vec2){cx, cy + rad * 0.55f}, c);
}

void ep_draw_clock_icon(ccord_renderer *r, float cx, float cy, float rad, ccord_color c, ccord_color bg)
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

ccord_color ep_palette_from_hash(uint64_t h)
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
  snprintf(url, sizeof url, "https://cdn.discordapp.com/emojis/%llu.%s?size=128", (unsigned long long)id, ext);
  bool ready = ui_ensure_media(ui, r, url);
  int tw = 0, th = 0;
  bool real_tex = ready && ccord_render_has_texture(r, url, &tw, &th) && tw > 1 && th > 1;
  if (ready) {
    bool animate = animated && (expr_animate_emoji(ui) || expr_pointer_over(ui, rect));
    ccord_rect draw_r = rect;
    if (tw > 1 && th > 1 && tw != th) {
      if (tw > th) {
        float dh = rect.w * ((float)th / (float)tw);
        draw_r.y += (rect.h - dh) * 0.5f;
        draw_r.h = dh;
      } else {
        float dw = rect.h * ((float)tw / (float)th);
        draw_r.x += (rect.w - dw) * 0.5f;
        draw_r.w = dw;
      }
    }
    ccord_draw_texture_rounded_anim(r, url, draw_r, 0.0f, 255, animate || !animated);
  }
  if (real_tex) return;

  // Stylized custom emoji tile for 1x1 mock textures or while loading
  uint64_t h = str_hash(name ? name : "", nlen) ^ (uint64_t)id;
  ccord_color bg = ep_palette_from_hash(h);
  ccord_draw_rrect(r, rect, ep_maxf(rect.w * 0.22f, 3.0f), bg);
  ccord_rect inner = {rect.x + 2.0f, rect.y + 2.0f, rect.w - 4.0f, rect.h - 4.0f};
  if (inner.w > 4.0f && inner.h > 4.0f) {
    ccord_draw_rrect(r, inner, ep_maxf(inner.w * 0.18f, 2.0f), ui_mix(bg, (ccord_color){0x11, 0x12, 0x14, 255}, 0.28f));
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
  ccord_color accent = ep_palette_from_hash(h);
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
  ccord_draw_circle(r, cx - rad * 0.32f, cy - rad * 0.12f, ep_maxf(rad * 0.13f, 2.0f), white);
  ccord_draw_circle(r, cx + rad * 0.32f, cy - rad * 0.12f, ep_maxf(rad * 0.13f, 2.0f), white);
  ccord_draw_circle(r, cx - rad * 0.30f, cy - rad * 0.12f, ep_maxf(rad * 0.07f, 1.2f), dark);
  ccord_draw_circle(r, cx + rad * 0.30f, cy - rad * 0.12f, ep_maxf(rad * 0.07f, 1.2f), dark);
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
    float tx = lbar.x + ep_maxf((lbar.w - nm.x) * 0.5f, 4.0f);
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
  if (ui->mobile_layout) {
    // Compact mobile composer: single Emoji button (opens tabbed Emoji/GIFs/Stickers sheet) left of Send/Mic.
    float right = box.x + box.w - 48.0f;
    float sz = 38.0f;
    ccord_rect emoji_r = {right - sz, box.y + (box.h - sz) * 0.5f, sz, sz};
    ep->composer_btn_rects[EXPR_TAB_GIFS] = (ccord_rect){0};
    ep->composer_btn_rects[EXPR_TAB_STICKERS] = (ccord_rect){0};
    ep->composer_btn_rects[EXPR_TAB_EMOJI] = emoji_r;
    bool emj_active = ui->emoji_picker_open && ui->emoji_picker_msg_id == CCORD_ID_NONE;
    ccord_color emj_col =
        ui_state_mix(ui, "eemjbtn", 0, false, emj_active, COL(text_secondary), COL(status_idle), COL(brand));
    ui_expr_draw_smiley(r, emoji_r.x + emoji_r.w * 0.5f, emoji_r.y + emoji_r.h * 0.5f, 22.0f, emj_col);
    return;
  }
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
  if (!ui || !ui->expr) return false;
  for (int t = 0; t < 3; t++) {
    if (ui_in_rect(mx, my, ui->expr->composer_btn_rects[t])) {
      ui_expr_picker_open(ui, t, ccord_store_selected_channel(ui->d.store), CCORD_ID_NONE);
      return true;
    }
  }
  return false;
}

ccord_rect ui_expr_soundboard_hit(const ccord_ui *ui, ccord_id sound_id)
{
  if (!ui || !ui->expr || !ui->expr->body) return (ccord_rect){0, 0, 0, 0};
  const ui_expr_body *b = ui->expr->body;
  for (size_t i = 0; i < b->nhits; i++) {
    if (b->hits[i].kind == EXPR_ITEM_SOUNDBOARD && b->hits[i].id == sound_id) return b->hits[i].rect;
  }
  return (ccord_rect){0, 0, 0, 0};
}

// A subtle 4 px thumb on the grid's right edge while the tab's content is taller than its viewport.
static void draw_grid_scrollbar(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep)
{
  ui_expr_body *b = ep->body;
  b->grid_scrollbar_rect = (ccord_rect){0};
  float view_h = b->hits_clip.h;
  float content_h = b->content_h[ep->tab];
  if (view_h <= 0.0f || content_h <= view_h + 0.5f) return;
  float max_s = content_h - view_h;
  float frac = clampf(ep->scroll_y[ep->tab] / max_s, 0.0f, 1.0f);
  float w = 4.0f;
  float thumb_h = view_h * view_h / content_h;
  if (thumb_h < 20.0f) thumb_h = 20.0f;
  if (thumb_h > view_h - 8.0f) thumb_h = view_h - 8.0f;
  float top = b->hits_clip.y + 4.0f + frac * (view_h - 8.0f - thumb_h);
  ccord_rect thumb = {b->hits_clip.x + b->hits_clip.w - w - 2.0f, top, w, thumb_h};
  b->grid_scrollbar_rect = thumb;
  ccord_draw_rrect(r, thumb, w * 0.5f, ui_alpha(COL(scrollbar_thumb), 0.9f));
}

void ui_expr_picker_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui) return;
  if (!ui->emoji_picker_open) {
    if (ui->expr && ui->expr->body && ui->dbg_ovl_frame[UI_OVL_PICKER] != ui->anim.frame) {
      expr_body_free(ui->expr);
    }
    return;
  }
  struct ui_expr_picker *ep = ensure_expr(ui);
  ui_expr_body *b = ensure_expr_body(ui);
  if (!ep || !b) return;

  (void)expr_flush_gif_search(ui, ep);
  request_expression_data(ui, b);
  compute_picker_rect(ui);
  ccord_rect box = ui->emoji_picker_rect;

  uis_popover_frame(ui, r, box);

  // Header tabs: GIFs | Stickers | Emoji | Sounds (Sounds is hidden in reaction mode)
  static const char *const k_tab_names[4] = {"GIFs", "Stickers", "Emoji", "Sounds"};
  float tab_y = box.y + UIS_SP_2;
  float tx = box.x + UIS_SP_3;
  float tab_h = uis_btn_h_for(ui, UIS_H_BTN_SM);
  float icon_sz = uis_icon_btn_for(ui);
  float inp_h = ui->mobile_layout ? 42.0f : UIS_H_INPUT;
  bool reaction_mode = (ui->emoji_picker_msg_id != CCORD_ID_NONE);
  int ntabs = reaction_mode ? 3 : 4;
  for (int t = 0; t < 4; t++) {
    b->tab_rects[t] = (ccord_rect){0, 0, 0, 0};
    if (t >= ntabs) continue;
    ccord_rect tr = {tx, tab_y, uis_label_width(ui, r, k_tab_names[t]), tab_h};
    b->tab_rects[t] = tr;
    bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, tr);
    uis_tab(ui, r, tr, k_tab_names[t], (uis_state){.hover = hov && !reaction_mode, .selected = ep->tab == t}, "etab",
            (uint64_t)t);
    tx += tr.w + UIS_SP_2;
  }
  if (reaction_mode) {
    const char *rm_lbl = "Add Reaction";
    ccord_rect badge = {box.x + box.w - UIS_SP_3 - uis_label_width(ui, r, rm_lbl), tab_y, uis_label_width(ui, r, rm_lbl),
                        tab_h};
    uis_button(ui, r, badge, rm_lbl, UIS_BTN_PRIMARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, badge)}, "eaddrx", 0);
  }

  // Search bar + skin tone button (Emoji tab). The search is a shared input; its leading glyph and trailing clear button sit
  // in the EXPR_SEARCH_PAD insets.
  float search_y = tab_y + tab_h + UIS_SP_2;
  bool emoji_tab = (ep->tab == EXPR_TAB_EMOJI);
  float search_w = emoji_tab ? box.w - 2.0f * UIS_SP_3 - UIS_SP_2 - icon_sz : box.w - 2.0f * UIS_SP_3;
  b->search_rect = (ccord_rect){box.x + UIS_SP_3, search_y, search_w, inp_h};

  const char *q = ccord_text_edit_cstr(&b->search_edit);
  const char *placeholder = (ep->tab == EXPR_TAB_GIFS)     ? "Search GIFs"
                          : (ep->tab == EXPR_TAB_STICKERS) ? "Search Stickers"
                          : (ep->tab == EXPR_TAB_SOUNDBOARD) ? "Search Sounds"
                                                           : "Search Emoji";
  ui_icon_search(r, b->search_rect.x + UIS_SP_4, b->search_rect.y + b->search_rect.h * 0.5f, 14.0f, COL(text_muted));
  uis_input_padded(ui, r, b->search_rect, EXPR_SEARCH_PAD, EXPR_SEARCH_PAD, &b->search_edit, placeholder,
                   (uis_state){.focused = b->search_focused}, false, "esearch", 0);

  // The caret and selection rects are recorded for the inspect snapshot (the IME window); uis_input draws them.
  const float qx = b->search_rect.x + EXPR_SEARCH_PAD;
  const float qw = b->search_rect.w - 2.0f * EXPR_SEARCH_PAD;
  ccord_text_style qs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
  const float qlh = ui_line_h(ui, qs.size);
  const float qy = b->search_rect.y + (b->search_rect.h - qlh) * 0.5f;
  b->search_caret_rect = (ccord_rect){0};
  b->search_selection_rect = (ccord_rect){0};
  if (b->search_focused && q[0] && b->search_edit.caret != b->search_edit.anchor) {
    size_t from = b->search_edit.caret < b->search_edit.anchor ? b->search_edit.caret : b->search_edit.anchor;
    size_t to = b->search_edit.caret < b->search_edit.anchor ? b->search_edit.anchor : b->search_edit.caret;
    float sx0 = qx + ccord_text_measure(r, q, from, &qs, 0.0f).x;
    float sx1 = qx + ccord_text_measure(r, q, to, &qs, 0.0f).x;
    if (sx0 < qx) sx0 = qx;
    if (sx1 > qx + qw) sx1 = qx + qw;
    if (sx1 > sx0) b->search_selection_rect = (ccord_rect){sx0, qy, sx1 - sx0, qlh};
  }
  if (b->search_focused && ui->caret_on) {
    size_t caret = b->search_edit.caret <= b->search_edit.len ? b->search_edit.caret : b->search_edit.len;
    float cx = qx + ccord_text_measure(r, q, caret, &qs, 0.0f).x;
    if (cx < qx) cx = qx;
    if (cx > qx + qw - 1.5f) cx = qx + qw - 1.5f;
    b->search_caret_rect = (ccord_rect){cx, qy + 1.0f, 1.5f, qlh - 2.0f};
  }

  if (q[0]) {
    b->search_clear_rect = (ccord_rect){b->search_rect.x + b->search_rect.w - icon_sz - 2.0f,
                                        b->search_rect.y + (b->search_rect.h - icon_sz) * 0.5f, icon_sz,
                                        icon_sz};
    bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, b->search_clear_rect);
    ccord_color cc = uis_icon_button(ui, r, b->search_clear_rect, (uis_state){.hover = chv}, "esearchclr", 0);
    ui_icon_close(r, b->search_clear_rect.x + b->search_clear_rect.w * 0.5f,
                  b->search_clear_rect.y + b->search_clear_rect.h * 0.5f, UIS_ICON_GLYPH, cc);
  } else {
    b->search_clear_rect = (ccord_rect){0, 0, 0, 0};
  }

  if (emoji_tab) {
    b->skin_btn_rect = (ccord_rect){b->search_rect.x + b->search_rect.w + UIS_SP_2,
                                    search_y + (inp_h - icon_sz) * 0.5f, icon_sz, icon_sz};
    bool shov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, b->skin_btn_rect);
    uis_icon_button(ui, r, b->skin_btn_rect, (uis_state){.hover = shov, .selected = b->skin_popup_open}, "eskinbtn", 0);
    const char *sw = ep_k_skin_tone_swatches[ep->skin_tone % 6u];
    ccord_text_style sws = ui_style(ui, CCORD_FONT_EMOJI, M(font_size_base), COL(text_normal));
    ccord_vec2 swm = ccord_text_measure(r, sw, strlen(sw), &sws, 0.0f);
    ccord_text_draw(r, sw, strlen(sw), b->skin_btn_rect.x + (b->skin_btn_rect.w - swm.x) * 0.5f,
                    b->skin_btn_rect.y + (b->skin_btn_rect.h - swm.y) * 0.5f, &sws, 0.0f);
  } else {
    b->skin_btn_rect = (ccord_rect){0, 0, 0, 0};
  }

  float body_y = search_y + inp_h + UIS_SP_2 + 1.0f;
  ccord_draw_rect(r, (ccord_rect){box.x, body_y - 1.0f, box.w, 1.0f}, COL(border_subtle));

  // Tab Body
  ccord_rect body = {box.x, body_y, box.w, box.y + box.h - body_y};
  b->nhits = 0;
  b->hovered_hit = -1;
  memset(ui->emoji_picker_cells, 0, sizeof ui->emoji_picker_cells);   // re-recorded by the Emoji tab only

  if (ep->tab == EXPR_TAB_EMOJI) {
    ep_draw_emoji_tab(ui, r, ep, body);
  } else if (ep->tab == EXPR_TAB_GIFS) {
    ep_draw_gifs_tab(ui, r, ep, body);
  } else if (ep->tab == EXPR_TAB_SOUNDBOARD) {
    ep_draw_soundboard_tab(ui, r, ep, body);
  } else {
    ep_draw_stickers_tab(ui, r, ep, body);
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

static int64_t expr_hover_key(const ccord_ui *ui, const struct ui_expr_picker *ep, const ui_expr_body *b,
                              ccord_rect box, float mx, float my)
{
  if (b->skin_popup_open) {
    for (int t = 0; t < 6; t++) {
      if (b->skin_option_rects[t].w > 0.0f && ui_in_rect(mx, my, b->skin_option_rects[t])) {
        return 100 + t;
      }
    }
    return 199;
  }
  for (int t = 0; t < 4; t++) {
    if (b->tab_rects[t].w > 0.0f && ui_in_rect(mx, my, b->tab_rects[t])) return 200 + t;
  }
  if (b->search_clear_rect.w > 0.0f && ui_in_rect(mx, my, b->search_clear_rect)) return 300;
  if (ep->tab == EXPR_TAB_EMOJI && b->skin_btn_rect.w > 0.0f && ui_in_rect(mx, my, b->skin_btn_rect)) return 301;
  if (ep->tab == EXPR_TAB_GIFS && b->back_btn_rect.w > 0.0f && ui_in_rect(mx, my, b->back_btn_rect)) return 302;
  if (ep->tab == EXPR_TAB_EMOJI && b->rail_pinned_rect.w > 0.0f && ui_in_rect(mx, my, b->rail_pinned_rect)) {
    return 303;
  }
  if (ui->emoji_picker_msg_id != CCORD_ID_NONE && my < b->search_rect.y && mx >= box.x + box.w - 140.0f) {
    return 304;
  }
  if (b->nrail > 0 && (ep->tab != EXPR_TAB_EMOJI || ui_in_rect(mx, my, b->rail_view))) {
    for (size_t i = 0; i < b->nrail; i++) {
      if (ui_in_rect(mx, my, b->rail[i].rect)) return 1000 + (int64_t)i;
    }
  }
  if (ui_in_rect(mx, my, b->hits_clip)) {
    for (size_t i = 0; i < b->nhits; i++) {
      const expr_hit_cell *h = &b->hits[i];
      if (!ui_in_rect(mx, my, h->rect)) continue;
      int64_t sub = 0;
      if ((h->kind == EXPR_ITEM_GIF_TILE || h->kind == EXPR_ITEM_STICKER) && h->star_rect.w > 0.0f &&
          ui_in_rect(mx, my, h->star_rect)) {
        sub = 1;
      } else if (h->kind == EXPR_ITEM_SOUNDBOARD && mx >= h->rect.x + h->rect.w - 80.0f) {
        sub = 1;
      }
      return 2000 + (int64_t)i * 2 + sub;
    }
  }
  return 0;
}

bool ui_expr_handle_event(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ui->emoji_picker_open || !ev) return false;
  struct ui_expr_picker *ep = ensure_expr(ui);
  ui_expr_body *b = ensure_expr_body(ui);
  if (!ep || !b) return false;

  compute_picker_rect(ui);
  ccord_rect box = ui->emoji_picker_rect;

  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    if (ui_in_rect(ev->x, ev->y, box)) {
      int64_t hk = expr_hover_key(ui, ep, b, box, ev->x, ev->y);
      if (hk != b->last_hover_key) {
        b->last_hover_key = hk;
        ui->dirty = true;
      }
      return true;
    }
    if (b->last_hover_key != -1) {
      b->last_hover_key = -1;
      ui->dirty = true;
    }
    return false;
  }

  if (ev->type == CCORD_IN_WHEEL) {
    if (!ui_in_rect(ev->x, ev->y, box)) return false;
    float step = 42.0f * 2.5f;
    float dy = -ev->wheel * step;
    bool direct = ui_wheel_direct(ui, ev);
    if ((ep->tab == EXPR_TAB_EMOJI || ep->tab == EXPR_TAB_STICKERS) &&
        b->rail_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, b->rail_rect)) {
      // Wheel over the 40px rail scrolls only the rail, never the grid. The rail's rects move with its offset.
      float off = (ep->tab == EXPR_TAB_STICKERS) ? ep->sticker_rail_scroll_y : ep->rail_scroll_y;
      ccord_scroller_scroll(&b->rail_glide, &off, dy, ep_maxf(b->rail_content_h - b->rail_view.h, 0.0f), direct);
      ep_expr_rail_set_scroll(ep, off);
    } else {
      int t = ep->tab;
      float max_s = ep_maxf(b->content_h[t] - (box.h - 125.0f), 0.0f);
      ccord_scroller_scroll(&b->glide[t], &ep->scroll_y[t], dy, max_s, direct);
    }
    b->last_hover_key = -1;
    ui->dirty = true;
    return true;
  }

  if (ev->type == CCORD_IN_MOUSE_DOWN) {
    // 1. Check skin-tone popup first if open
    if (b->skin_popup_open && ev->button == 1) {
      for (int t = 0; t < 6; t++) {
        if (ui_in_rect(ev->x, ev->y, b->skin_option_rects[t])) {
          ep->skin_tone = (uint8_t)t;
          b->skin_popup_open = false;
          b->last_hover_key = -1;
          ui->dirty = true;
          return true;
        }
      }
      b->skin_popup_open = false;
      b->last_hover_key = -1;
      ui->dirty = true;
      if (ui_in_rect(ev->x, ev->y, b->skin_btn_rect)) return true;
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
        if (b->tab_rects[t].w > 0.0f && ui_in_rect(ev->x, ev->y, b->tab_rects[t])) {
          ep->tab = t;
          b->skin_popup_open = false;
          if (t == EXPR_TAB_GIFS && b->search_edit.len >= 2) {
            b->gif_search_pending = true;
            b->gif_search_due_ms = 0;
          } else {
            b->gif_search_pending = false;
          }
          b->last_hover_key = -1;
          ui->dirty = true;
          return true;
        }
      }
      if (b->search_clear_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, b->search_clear_rect)) {
        ccord_text_edit_clear(&b->search_edit);
        b->gif_subview = 0;
        b->gif_search_pending = false;
        b->last_hover_key = -1;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, b->search_rect)) {
        b->search_focused = true;
        if (ui->touch_mode) ui->touch_text_focused = true;
        ui->dirty = true;
        return true;
      }
      if (ep->tab == EXPR_TAB_EMOJI && ui_in_rect(ev->x, ev->y, b->skin_btn_rect)) {
        b->skin_popup_open = !b->skin_popup_open;
        b->last_hover_key = -1;
        ui->dirty = true;
        return true;
      }
      if (ep->tab == EXPR_TAB_GIFS && b->back_btn_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, b->back_btn_rect)) {
        b->gif_subview = 0;
        ep->scroll_y[EXPR_TAB_GIFS] = 0.0f;
        b->last_hover_key = -1;
        ui->dirty = true;
        return true;
      }
      // Pinned Default Unicode Emoji button: clear any search and jump to the Unicode categories.
      if (ep->tab == EXPR_TAB_EMOJI && b->rail_pinned_rect.w > 0.0f &&
          ui_in_rect(ev->x, ev->y, b->rail_pinned_rect)) {
        if (b->search_edit.len > 0) ccord_text_edit_clear(&b->search_edit);
        b->pending_unicode_jump = true;
        b->last_hover_key = -1;
        ui->dirty = true;
        return true;
      }
      // Left category rail clicks (Emoji rail items are only live inside the scrollable rail view)
      if (ep->tab != EXPR_TAB_EMOJI || ui_in_rect(ev->x, ev->y, b->rail_view)) {
        for (size_t ri = 0; ri < b->nrail; ri++) {
          if (ui_in_rect(ev->x, ev->y, b->rail[ri].rect)) {
            ep->scroll_y[ep->tab] = b->rail[ri].target_scroll_y;
            b->last_hover_key = -1;
            ui->dirty = true;
            return true;
          }
        }
      }
    }

    // 5. Grid item hits (only inside the tab's viewport: partially visible cells must not be
    // clickable through the preview bar or outside the clipped grid)
    for (size_t i = 0; i < b->nhits; i++) {
      const expr_hit_cell *h = &b->hits[i];
      if (!ui_in_rect(ev->x, ev->y, b->hits_clip) || !ui_in_rect(ev->x, ev->y, h->rect)) continue;
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
        b->gif_subview = 1;
        ep->scroll_y[EXPR_TAB_GIFS] = 0.0f;
        b->last_hover_key = -1;
        ui->dirty = true;
        return true;
      }
      if (h->kind == EXPR_ITEM_GIF_TREND_CAT) {
        b->gif_subview = 0;
        ep->scroll_y[EXPR_TAB_GIFS] = 160.0f;
        b->last_hover_key = -1;
        ui->dirty = true;
        return true;
      }
      if (h->kind == EXPR_ITEM_GIF_CATEGORY) {
        ccord_text_edit_clear(&b->search_edit);
        const char *term = h->sub[0] ? h->sub : h->name;
        (void)ccord_text_edit_insert(&b->search_edit, term, strlen(term), &err);
        b->gif_search_pending = false;
        if (ui->d.session) {
          (void)ccord_session_request_expression_data(ui->d.session, CCORD_EXPR_GIF_SEARCH, term, true, &err);
        }
        ep->scroll_y[EXPR_TAB_GIFS] = 0.0f;
        b->last_hover_key = -1;
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
    bool text_edited = false;
    switch (ev->key) {
      case CCORD_KEY_BACKSPACE: ccord_text_edit_backspace(&b->search_edit); text_edited = true; break;
      case CCORD_KEY_DELETE:    ccord_text_edit_delete(&b->search_edit); text_edited = true; break;
      case CCORD_KEY_LEFT:      ccord_text_edit_move(&b->search_edit, -1, shift); break;
      case CCORD_KEY_RIGHT:     ccord_text_edit_move(&b->search_edit, 1, shift); break;
      case CCORD_KEY_HOME:      ccord_text_edit_move(&b->search_edit, -2, shift); break;
      case CCORD_KEY_END:       ccord_text_edit_move(&b->search_edit, 2, shift); break;
      case CCORD_KEY_A:
        if (ctrl) ccord_text_edit_select_all(&b->search_edit);
        break;
      case CCORD_KEY_RETURN:
        b->gif_search_pending = false;
        if (ep->tab == EXPR_TAB_GIFS && b->search_edit.len > 0 && ui->d.session) {
          ccord_error err;
          (void)ccord_session_request_expression_data(ui->d.session, CCORD_EXPR_GIF_SEARCH,
                                                      ccord_text_edit_cstr(&b->search_edit), true, &err);
        } else if (b->nhits > 0) {
          const expr_hit_cell *h = &b->hits[0];
          if (h->kind == EXPR_ITEM_UNICODE_EMOJI || h->kind == EXPR_ITEM_CUSTOM_EMOJI) {
            activate_emoji_hit(ui, h, shift);
          }
        }
        ui->dirty = true;
        return true;
      default:
        return false;
    }
    if (text_edited && ep->tab == EXPR_TAB_GIFS && ui->d.session) {
      if (b->search_edit.len >= 2) {
        b->gif_search_pending = true;
        b->gif_search_due_ms = ui->now_ms + EXPR_GIF_SEARCH_DEBOUNCE_MS;
      } else {
        b->gif_search_pending = false;
      }
    }
    if (text_edited) {
      ep->scroll_y[ep->tab] = 0.0f;
      b->last_hover_key = -1;
    }
    ui->dirty = true;
    return true;
  }

  if (ev->type == CCORD_IN_TEXT) {
    size_t n = strnlen(ev->text, sizeof ev->text);
    ccord_error err;
    (void)ccord_text_edit_insert(&b->search_edit, ev->text, n, &err);
    if (ep->tab == EXPR_TAB_GIFS && ui->d.session) {
      if (b->search_edit.len >= 2) {
        b->gif_search_pending = true;
        b->gif_search_due_ms = ui->now_ms + EXPR_GIF_SEARCH_DEBOUNCE_MS;
      } else {
        b->gif_search_pending = false;
      }
    }
    ep->scroll_y[ep->tab] = 0.0f;
    b->last_hover_key = -1;
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
  if (!ui || !ui->expr || !ui->expr->body) return false;
  const struct ui_expr_picker *ep = ui->expr;
  const ui_expr_body *b = ep->body;
  out->tab = ep->tab;
  out->gif_subview = b->gif_subview;
  out->nhits = b->nhits;
  out->nrail = b->nrail;
  out->rail_cap = EXPR_RAIL_MAX;
  out->search_len = b->search_edit.len;
  out->grid_scroll_y = ep->scroll_y[ep->tab];
  out->grid_content_h = b->content_h[ep->tab];
  out->rail_scroll_y = ep->rail_scroll_y;
  out->rail_content_h = b->rail_content_h;
  out->rail_rect = b->rail_rect;
  out->rail_view = b->rail_view;
  out->rail_pinned_rect = b->rail_pinned_rect;
  out->grid_view = b->hits_clip;
  out->search_rect = b->search_rect;
  out->grid_scrollbar_rect = b->grid_scrollbar_rect;
  out->search_caret_rect = b->search_caret_rect;
  out->search_selection_rect = b->search_selection_rect;
  out->hit_url_cap = sizeof b->hits[0].url;
  out->hit_preview_url_cap = sizeof b->hits[0].preview_url;
  return true;
}

bool ui_expr_rail_at(const ccord_ui *ui, size_t index, ui_expr_rail_info *out)
{
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!ui || !ui->expr || !ui->expr->body || index >= ui->expr->body->nrail) return false;
  const expr_rail_item *ri = &ui->expr->body->rail[index];
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
  if (!ui || !ui->expr || !ui->expr->body || index >= ui->expr->body->nhits) return false;
  const expr_hit_cell *h = &ui->expr->body->hits[index];
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
