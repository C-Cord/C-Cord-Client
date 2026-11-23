// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Emoji/GIF/Sticker picker layout verification; see verify_ui_parity.h.
//
// Drives the real picker over the mock app at several window sizes: the popover must fit its window, every
// recorded hit rect (emoji cells, GIF cards + stars, sticker cells) must lie inside the grid clip rect
// horizontally, the column counts must be the shared helper's (ui_expr_grid_layout_for_width) for that popover
// width, and a click at the centre of a row's last cell must select exactly that cell.
#include "verify_ui_parity.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_internal.h"

#define PL_EPS 0.51f
#define PL_MAX_HITS 1024
#define PL_TAB_GIFS 0       // the picker tab ids (expression_picker.c EXPR_TAB_*)
#define PL_TAB_STICKERS 1
#define PL_TAB_EMOJI 2

typedef struct pl_size { float w, h; } pl_size;

static const ccord_verify_ui_parity_ctx *pl_ctx;
static int pl_checks;

[[gnu::format(printf, 1, 2)]] static int pl_fail(const char *fmt, ...)
{
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  printf("picker-layout: FAIL %s\n", buf);
  fflush(stdout);
  return 1;
}

static void pl_tick(int n) { pl_ctx->frames(pl_ctx->ud, n); }

static void pl_click(float x, float y)
{
  ccord_input_event ev = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  pl_ctx->input(pl_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  pl_ctx->input(pl_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y};
  pl_ctx->input(pl_ctx->ud, &ev);
}

static bool pl_inside_x(ccord_rect r, ccord_rect clip)
{
  return r.x >= clip.x - PL_EPS && r.x + r.w <= clip.x + clip.w + PL_EPS;
}

static const char *pl_tab_name(int tab) { return tab == PL_TAB_GIFS ? "gif" : (tab == PL_TAB_STICKERS ? "stickers" : "emoji"); }

// Snapshot of the hits the last frame recorded.
typedef struct pl_hits {
  ui_expr_hit_info h[PL_MAX_HITS];
  size_t n;
  ui_expr_inspect ins;
} pl_hits;

static pl_hits *pl_snap;

static bool pl_snapshot(const ccord_ui *ui)
{
  if (!pl_snap || !ui_expr_get_inspect(ui, &pl_snap->ins)) return false;
  pl_snap->n = pl_snap->ins.nhits < PL_MAX_HITS ? pl_snap->ins.nhits : PL_MAX_HITS;
  for (size_t i = 0; i < pl_snap->n; i++) {
    if (!ui_expr_hit_at(ui, i, &pl_snap->h[i])) return false;
  }
  return true;
}

// Open the picker on `tab` and settle the frames so the data blobs land and the tab is drawn.
static void pl_open(ccord_ui *ui, int tab, ccord_id chan)
{
  ui_expr_picker_close(ui);
  ui_expr_picker_open(ui, tab, chan, CCORD_ID_NONE);
  pl_tick(8);
}

static int pl_geometry(ccord_ui *ui, const char *tag, int tab, float win_w, float win_h)
{
  const ccord_rect box = ui->emoji_picker_rect;
  // 1. the popover is inside the window with its 8 px margins (it shrinks with a small window)
  if (box.x < 8.0f - PL_EPS || box.y < 8.0f - PL_EPS || box.x + box.w > win_w - 8.0f + PL_EPS ||
      box.y + box.h > win_h - 8.0f + PL_EPS) {
    return pl_fail("%s: popover rect %.1f,%.1f %.1fx%.1f leaves the %.0fx%.0f window margins", tag, (double)box.x, (double)box.y,
                   (double)box.w, (double)box.h, (double)win_w, (double)win_h);
  }
  pl_checks++;
  if (!pl_snapshot(ui)) return pl_fail("%s: picker inspection unavailable", tag);
  if (pl_snap->n == 0) return pl_fail("%s: the tab recorded no hit cells", tag);
  const ccord_rect clip = pl_snap->ins.grid_view;
  if (!pl_inside_x(clip, box)) return pl_fail("%s: grid clip rect leaves the popover", tag);
  // 2. every hit rect (and star rect) lies horizontally inside the grid clip rect
  for (size_t i = 0; i < pl_snap->n; i++) {
    const ui_expr_hit_info *h = &pl_snap->h[i];
    if (!pl_inside_x(h->rect, clip)) {
      return pl_fail("%s: hit %zu (kind %d) x=%.1f w=%.1f extends past the grid clip rect %.1f..%.1f", tag, i, h->kind,
                     (double)h->rect.x, (double)h->rect.w, (double)clip.x, (double)(clip.x + clip.w));
    }
    // Readable tiles: a column that squeezes a GIF below 180 px or a sticker below 100 px is one column too many.
    if ((h->kind == 2 && h->rect.w < 180.0f - PL_EPS) || (h->kind == 6 && h->rect.w < 100.0f - PL_EPS)) {
      return pl_fail("%s: hit %zu (kind %d) is only %.1f px wide: too many columns for a %.0f px popover", tag, i, h->kind,
                     (double)h->rect.w, (double)box.w);
    }
    if (h->star_rect.w > 0.0f && !pl_inside_x(h->star_rect, h->rect)) {
      return pl_fail("%s: hit %zu favorite star leaves its card", tag, i);
    }
  }
  pl_checks++;

  ui_expr_grid_layout lay;
  ui_expr_grid_layout_for_width(box.w, &lay);
  // 3. the column count of what was drawn is the helper's value for this popover width
  int kind = tab == PL_TAB_GIFS ? 2 : (tab == PL_TAB_STICKERS ? 6 : 0);
  int want = tab == PL_TAB_GIFS ? lay.gif_cols : (tab == PL_TAB_STICKERS ? lay.sticker_cols : lay.emoji_cols);
  float xs[PL_MAX_HITS];
  size_t nx = 0, ntiles = 0;
  int best_row = 0;
  for (size_t i = 0; i < pl_snap->n; i++) {
    const ui_expr_hit_info *h = &pl_snap->h[i];
    if (h->kind != kind && !(kind == 0 && h->kind == 1)) continue;
    ntiles++;
    bool seen = false;
    for (size_t k = 0; k < nx; k++) seen = seen || fabsf(xs[k] - h->rect.x) < 0.5f;
    if (!seen && nx < PL_MAX_HITS) xs[nx++] = h->rect.x;
    int row = 0;   // cells sharing this cell's top edge form one grid row
    for (size_t k = 0; k < pl_snap->n; k++) {
      const ui_expr_hit_info *o = &pl_snap->h[k];
      if ((o->kind == kind || (kind == 0 && o->kind == 1)) && fabsf(o->rect.y - h->rect.y) < 0.5f) row++;
    }
    if (row > best_row) best_row = row;
  }
  if (ntiles < 1) return pl_fail("%s: only %zu %s cells drawn", tag, ntiles, pl_tab_name(tab));
  // Distinct x origins is the column count for the masonry GIF grid; the widest row is it for the cell grids.
  int got = tab == PL_TAB_GIFS ? (int)nx : best_row;
  int expect = (tab == PL_TAB_GIFS && (size_t)want > ntiles) ? (int)ntiles : want;
  if (got != expect) {
    return pl_fail("%s: %s columns drawn=%d but the width-derived layout says %d (popover %.0f px)", tag, pl_tab_name(tab),
                   got, expect, (double)box.w);
  }
  pl_checks++;
  return 0;
}

// Click the centre of the last cell of the widest row and require that exact cell to be the one selected.
static int pl_click_last(ccord_ui *ui, ccord_store *store, const char *tag, int tab, ccord_id chan)
{
  int kind = tab == PL_TAB_GIFS ? 2 : (tab == PL_TAB_STICKERS ? 6 : 0);
  const ui_expr_hit_info *last = nullptr;
  for (size_t i = 0; i < pl_snap->n; i++) {
    const ui_expr_hit_info *h = &pl_snap->h[i];
    if (h->kind != kind && !(kind == 0 && h->kind == 1)) continue;
    // Only a cell whose centre is in the grid view can be clicked (the rest is scrolled or clipped away).
    const ccord_rect clip = pl_snap->ins.grid_view;
    float mx = h->rect.x + h->rect.w * 0.5f, my = h->rect.y + h->rect.h * 0.5f;
    if (mx < clip.x || mx > clip.x + clip.w || my < clip.y || my > clip.y + clip.h) continue;
    if (!last || h->rect.x > last->rect.x) last = h;
  }
  if (!last) return pl_fail("%s: no cell to click", tag);
  ui_expr_hit_info target = *last;
  float cx = target.rect.x + target.rect.w * 0.5f, cy = target.rect.y + target.rect.h * 0.5f;
  ccord_text_edit_clear(&ui->edit);
  size_t nb = ccord_store_message_count(store, chan);
  const ccord_message *mb = nb ? ccord_store_message_at(store, chan, nb - 1) : nullptr;
  const ccord_id last_before = mb ? mb->id : CCORD_ID_NONE;
  pl_click(cx, cy);
  pl_tick(4);
  if (ui->emoji_picker_open) return pl_fail("%s: clicking the last cell did not select it (picker still open)", tag);
  if (tab == PL_TAB_EMOJI) {
    const char *txt = ccord_ui_composer_text(ui);
    if (target.utf8[0] ? strstr(txt, target.utf8) == nullptr : strstr(txt, target.name) == nullptr) {
      return pl_fail("%s: last emoji cell click inserted '%s', not the clicked cell %s", tag, txt, target.name);
    }
  } else {
    // The store keeps a window of recent messages, so the count can stand still: the newest message must be new.
    size_t n = ccord_store_message_count(store, chan);
    const ccord_message *m = n ? ccord_store_message_at(store, chan, n - 1) : nullptr;
    if (!m || m->id == last_before) return pl_fail("%s: last cell click sent no message", tag);
    if (tab == PL_TAB_GIFS && strstr(m->content, target.url) == nullptr) {
      return pl_fail("%s: GIF click sent '%s', not the clicked tile %s", tag, m->content, target.url);
    }
    if (tab == PL_TAB_STICKERS &&
        !(m->rich && m->rich->sticker_count == 1 && m->rich->stickers[0].id == target.id)) {
      return pl_fail("%s: sticker click sent a different sticker than the clicked cell", tag);
    }
  }
  pl_checks++;
  return 0;
}

// Forum view: an open picker gets a click on its tile before the post list under it, and no post opens; with no
// picker open the same click on a post card still opens that post.
static int pl_forum(ccord_ui *ui, ccord_store *store, ccord_session *session)
{
  ccord_id guild = ccord_store_selected_guild(store);
  ccord_id forum = CCORD_ID_NONE;
  const size_t nch = ccord_store_channel_count(store, guild);
  for (size_t i = 0; i < nch && forum == CCORD_ID_NONE; i++) {
    const ccord_channel *ch = ccord_store_channel_at(store, guild, i);
    if (ch && ccord_store_channel_is_forum(store, ch->id)) forum = ch->id;
  }
  if (forum == CCORD_ID_NONE) return pl_fail("forum: the selected guild has no forum channel");
  ccord_error err = {0};
  if (ccord_session_select_channel(session, forum, &err) != CCORD_OK) return pl_fail("forum: selecting it failed: %s", err.message);
  pl_tick(3);
  if (ccord_store_selected_channel(store) != forum || !ui_forum_is_active(ui)) {
    return pl_fail("forum: selecting the forum channel did not show its post list");
  }
  pl_checks++;

  ui_expr_picker_open(ui, PL_TAB_EMOJI, forum, CCORD_ID_NONE);
  pl_tick(8);
  if (!ui->emoji_picker_open) return pl_fail("forum: the picker did not open over the forum");
  if (!pl_snapshot(ui)) return pl_fail("forum: picker inspection unavailable");
  const ccord_rect clip = pl_snap->ins.grid_view;
  const ui_expr_hit_info *tile = nullptr;
  for (size_t i = 0; i < pl_snap->n && !tile; i++) {
    const ui_expr_hit_info *h = &pl_snap->h[i];
    if (h->kind != 0 && h->kind != 1) continue;
    float mx = h->rect.x + h->rect.w * 0.5f, my = h->rect.y + h->rect.h * 0.5f;
    if (mx >= clip.x && mx <= clip.x + clip.w && my >= clip.y && my <= clip.y + clip.h) tile = h;
  }
  if (!tile) return pl_fail("forum: the picker drew no clickable emoji tile");
  ui_expr_hit_info target = *tile;
  ccord_text_edit_clear(&ui->edit);
  pl_click(target.rect.x + target.rect.w * 0.5f, target.rect.y + target.rect.h * 0.5f);
  pl_tick(4);
  if (ui->emoji_picker_open) {
    return pl_fail("forum: a click on an open picker's tile did not reach the picker (the post list took it)");
  }
  const char *txt = ccord_ui_composer_text(ui);
  if (target.utf8[0] ? strstr(txt, target.utf8) == nullptr : strstr(txt, target.name) == nullptr) {
    return pl_fail("forum: the picker's tile click inserted '%s', not %s", txt, target.name);
  }
  if (ccord_store_selected_channel(store) != forum) return pl_fail("forum: a click on the picker's tile opened a forum post");
  pl_checks++;
  ccord_text_edit_clear(&ui->edit);

  // No overlay now: a click on a post card (index 1, not the pinned one) still opens that post.
  ccord_id tid = CCORD_ID_NONE;
  ccord_rect card = ui_forum_post_card_rect(ui, 1, &tid);
  if (card.w <= 0.0f || tid == CCORD_ID_NONE) return pl_fail("forum: no post card is drawn to click");
  pl_click(card.x + card.w * 0.5f, card.y + card.h * 0.5f);
  pl_tick(3);
  if (ccord_store_selected_channel(store) != tid) return pl_fail("forum: a click on a post card with no overlay did not open the post");
  pl_checks++;
  return 0;
}

static int pl_run_body(const ccord_verify_ui_parity_ctx *ctx)
{
  pl_ctx = ctx;
  pl_checks = 0;
  ccord_ui *ui = ctx->ui;
  ccord_store *store = ctx->store;

  ccord_id chan = CCORD_ID_NONE;
  for (int i = 0; i < 400 && chan == CCORD_ID_NONE; i++) {
    pl_tick(1);
    if (ccord_session_state(ctx->session) == CCORD_CONN_CONNECTED) chan = ccord_store_selected_channel(store);
  }
  if (chan == CCORD_ID_NONE) return pl_fail("setup: mock did not connect and select a channel");

  // The helper itself: 2 GIF / 3 sticker columns at the default width, fewer when narrow, more when wide.
  ui_expr_grid_layout lay;
  ui_expr_grid_layout_for_width(428.0f, &lay);
  if (lay.gif_cols != 2 || lay.sticker_cols != 3 || lay.emoji_cols != 8) {
    return pl_fail("helper: the default 428 px popover must be 8 emoji / 2 GIF / 3 sticker columns, got %d/%d/%d",
                   lay.emoji_cols, lay.gif_cols, lay.sticker_cols);
  }
  pl_checks++;
  ui_expr_grid_layout narrow, wide;
  ui_expr_grid_layout_for_width(344.0f, &narrow);
  ui_expr_grid_layout_for_width(620.0f, &wide);
  if (narrow.gif_cols != 1 || narrow.sticker_cols != 2 || wide.gif_cols < 3 || wide.sticker_cols < 4 || wide.emoji_cols <= lay.emoji_cols) {
    return pl_fail("helper: a 344 px popover needs 1 GIF and 2 sticker columns and a 620 px one more of each (%d/%d | %d/%d/%d)",
                   narrow.gif_cols, narrow.sticker_cols, wide.emoji_cols, wide.gif_cols, wide.sticker_cols);
  }
  pl_checks++;

  static const pl_size sizes[] = {{1280.0f, 720.0f}, {800.0f, 480.0f}, {480.0f, 360.0f}, {360.0f, 320.0f}};
  static const int tabs[] = {PL_TAB_EMOJI, PL_TAB_GIFS, PL_TAB_STICKERS};
  const float save_w = ui->win_w, save_h = ui->win_h;
  int rc = 0;
  for (size_t si = 0; si < sizeof sizes / sizeof sizes[0] && rc == 0; si++) {
    ccord_ui_set_viewport(ui, sizes[si].w, sizes[si].h);
    pl_tick(3);
    for (size_t ti = 0; ti < sizeof tabs / sizeof tabs[0] && rc == 0; ti++) {
      char tag[48];
      (void)snprintf(tag, sizeof tag, "%.0fx%.0f %s", (double)sizes[si].w, (double)sizes[si].h,
                     pl_tab_name(tabs[ti]));
      pl_open(ui, tabs[ti], chan);
      if (!ui->emoji_picker_open) { rc = pl_fail("%s: picker did not open", tag); break; }
      if (tabs[ti] == PL_TAB_GIFS) {
        // The home view's tiles sit below the category rows: scroll until some are drawn.
        ui_expr_grid_layout wl;
        ui_expr_grid_layout_for_width(ui->emoji_picker_rect.w, &wl);
        const size_t want_tiles = wl.gif_cols >= 2 ? 2u : 1u;
        for (int k = 0; k < 40; k++) {
          if (pl_snapshot(ui)) {
            size_t tiles = 0;
            const ccord_rect clip = pl_snap->ins.grid_view;
            for (size_t i = 0; i < pl_snap->n; i++) {
              const ui_expr_hit_info *h = &pl_snap->h[i];
              float my = h->rect.y + h->rect.h * 0.5f;
              tiles += h->kind == 2 && my >= clip.y && my <= clip.y + clip.h;
            }
            if (tiles >= want_tiles) break;   // 2 columns need 2 tiles in view
          }
          ccord_input_event ev = {.type = CCORD_IN_WHEEL, .x = ui->emoji_picker_rect.x + 80.0f,
                                  .y = ui->emoji_picker_rect.y + ui->emoji_picker_rect.h * 0.6f, .wheel = -1.0f};
          ctx->input(ctx->ud, &ev);
          pl_tick(2);
        }
      }
      rc = pl_geometry(ui, tag, tabs[ti], sizes[si].w, sizes[si].h);
      if (rc == 0) rc = pl_click_last(ui, store, tag, tabs[ti], chan);
      if (ui->emoji_picker_open) ui_expr_picker_close(ui);
    }
  }
  ccord_ui_set_viewport(ui, save_w, save_h);
  pl_tick(2);
  if (rc != 0) return rc;
  rc = pl_forum(ui, store, ctx->session);
  if (rc != 0) return rc;

  printf("picker-layout: PASS checks=%d\n", pl_checks);
  fflush(stdout);
  return 0;
}

int ccord_verify_picker_layout_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->frames || !ctx->input) {
    return pl_fail("invalid context");
  }
  pl_snap = calloc(1, sizeof *pl_snap);
  if (!pl_snap) return pl_fail("out of memory");
  int rc = pl_run_body(ctx);
  free(pl_snap);
  pl_snap = nullptr;
  return rc;
}
