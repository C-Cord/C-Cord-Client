// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice fav-gif-order: favourite GIFs in the picker follow the official client's sequence and masonry placement.
//
// Reference: Official client web client bundle analysis (describing the newest-first favorite GIF ordering logic):
//  - Read side, the favourites hook: `lodash(favoriteGifs.gifs).map(...).sortBy("order").reverse().value()`. sortBy is a
//    stable ascending sort over the map in wire order, so the list is order DESCENDING and entries with an equal or
//    missing (0) order come out in REVERSE wire order.
//  - Write side, GIF favourite action: `order = (max(Object.values(gifs).map(g => g.order)) ?? 0) + 1`, so a new
//    favourite is first and the first ever favourite gets order 1. The legacy-list migration assigns
//    `order = length - index + 1`, i.e. the legacy newest-first list keeps its order.
//  - The picker is MasonryList: `columnHeights.reduce((e, t, n) => t < e[0] ? [t, n] : e)` picks the shortest column
//    (leftmost on a tie); getItemHeight = columnWidth * (height / width) with no clamp; itemGutter is 12.
// The slice sends a settings proto whose map entries are shuffled (and tied) through the mock transport, exactly like
// the server's answer, and reads back what the real picker draws.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/core/input.h>
#include <ccord/net/mock_transport.h>
#include <ccord/store/expressions.h>

#include "ui_internal.h"
#include "verify_ui_parity.h"

#define FO_CHAN_TEXT 3121ULL   // the mock's text channel (verify_expressions.c EX_CHAN)
#define FO_N 13u

typedef struct {
  const char *url;
  uint32_t w, h, order;
} fo_gif;

// Wire order is the array order. Distinct orders are shuffled; the three order-0 entries and the two order-25 entries are
// tied and deliberately NOT in URL order, so only the wire sequence breaks the tie (reversed, as Discord does). The tall
// (320x480) and wide (320x60) entries sit near the top and fall outside any 64..240 px tile clamp.
static const fo_gif fo_wire[FO_N] = {
  {"https://tenor.com/view/fo-d-gif-1", 320, 200, 20},  //  0
  {"https://tenor.com/view/fo-zz-gif-9", 320, 300, 0},  //  1  tie group order 0, wire position 1
  {"https://tenor.com/view/fo-b-gif-2", 320, 160, 10},  //  2
  {"https://tenor.com/view/fo-c-gif-3", 320, 320, 60},  //  3
  {"https://tenor.com/view/fo-tb-gif-7", 320, 240, 25}, //  4  tie group order 25, wire position 4
  {"https://tenor.com/view/fo-aa-gif-8", 320, 180, 0},  //  5
  {"https://tenor.com/view/fo-e-gif-4", 320, 260, 50},  //  6
  {"https://tenor.com/view/fo-ta-gif-6", 320, 140, 25}, //  7
  {"https://tenor.com/view/fo-a-gif-5", 320, 220, 40},  //  8
  {"https://tenor.com/view/fo-mm-gif-10", 320, 120, 0}, //  9
  {"https://tenor.com/view/fo-f-gif-11", 320, 280, 30}, // 10
  {"https://tenor.com/view/fo-tall-gif-13", 320, 480, 55}, // 11
  {"https://tenor.com/view/fo-wide-gif-14", 320, 60, 45},  // 12
};

static const char fo_new_url[] = "https://tenor.com/view/fo-new-gif-12";

static const ccord_verify_ui_parity_ctx *fo_ctx;
static unsigned fo_checks;

CCORD_PRINTF(1, 2) static int fo_fail(const char *fmt, ...)
{
  char buf[400];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  printf("fav-gif-order: FAIL %s\n", buf);
  fflush(stdout);
  return 1;
}

static void fo_tick(int n) { fo_ctx->frames(fo_ctx->ud, n); }

static size_t fo_varint(uint8_t *p, uint64_t v)
{
  size_t n = 0;
  do {
    p[n] = (uint8_t)((v & 0x7fu) | (v > 0x7fu ? 0x80u : 0u));
    v >>= 7;
    n++;
  } while (v != 0);
  return n;
}

static size_t fo_bytes(uint8_t *p, unsigned field, const void *data, size_t len)
{
  size_t n = fo_varint(p, ((uint64_t)field << 3u) | 2u);
  n += fo_varint(p + n, len);
  memcpy(p + n, data, len);
  return n + len;
}

static size_t fo_uint(uint8_t *p, unsigned field, uint64_t v)
{
  size_t n = fo_varint(p, (uint64_t)field << 3u);
  return n + fo_varint(p + n, v);
}

// FrecencyUserSettings { favorite_gifs = 2 { gifs = 1 (map entries in array order) } }. An order of 0 is written (a
// proto3 client omits a zero, which the decoder reads as 0 as well).
static size_t fo_build_skip(uint8_t *out, unsigned skip_mask)
{
  uint8_t gifs[4096];
  size_t gn = 0;
  for (size_t i = 0; i < FO_N; i++) {
    if (skip_mask & (1u << i)) continue;   // omitted from the wire: a GIF unfavourited on another device
    uint8_t gif[256], ent[512];
    size_t n = 0;
    n += fo_uint(gif + n, 1, 1);
    n += fo_bytes(gif + n, 2, fo_wire[i].url, strlen(fo_wire[i].url));
    n += fo_uint(gif + n, 3, fo_wire[i].w);
    n += fo_uint(gif + n, 4, fo_wire[i].h);
    if (fo_wire[i].order != 0) n += fo_uint(gif + n, 5, fo_wire[i].order);
    size_t en = fo_bytes(ent, 1, fo_wire[i].url, strlen(fo_wire[i].url));
    en += fo_bytes(ent + en, 2, gif, n);
    gn += fo_bytes(gifs + gn, 1, ent, en);
  }
  return fo_bytes(out, 2, gifs, gn);
}

static size_t fo_build(uint8_t *out) { return fo_build_skip(out, 0); }

static void fo_click(float x, float y)
{
  ccord_input_event ev = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  fo_ctx->input(fo_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  fo_ctx->input(fo_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y};
  fo_ctx->input(fo_ctx->ud, &ev);
}

// True when the store's favourites are exactly `urls` in that order (the store may hold more after them).
static bool fo_store_is(const char *const *urls, size_t n)
{
  if (ccord_store_favorite_gif_count(fo_ctx->store) < n) return false;
  for (size_t i = 0; i < n; i++) {
    const ccord_gif_item *it = ccord_store_favorite_gif_at(fo_ctx->store, i);
    if (!it || strcmp(it->url, urls[i]) != 0) return false;
  }
  return true;
}

static void fo_store_dump(char *buf, size_t cap)
{
  size_t off = 0;
  buf[0] = '\0';
  for (size_t i = 0, n = ccord_store_favorite_gif_count(fo_ctx->store); i < n && off + 24 < cap; i++) {
    const ccord_gif_item *it = ccord_store_favorite_gif_at(fo_ctx->store, i);
    const char *tail = strrchr(it->url, '/');
    off += (size_t)snprintf(buf + off, cap - off, "%s(%u) ", tail ? tail + 1 : it->url, (unsigned)it->order);
  }
}

// The reference sequence over everything the store holds: any favourite whose URL is not in fo_wire (none once the
// first answer, which is authoritative, has replaced the mock's two seeded favourites) plus the proto's entries in wire
// order, stable-sorted by order descending.
#define FO_MAX 64u
static const char *fo_exp[FO_MAX];
static char (*fo_extra_url)[CCORD_URL_MAX_BYTES + 1];   // copies: the store's array shifts as favourites move
static uint32_t fo_exp_order[FO_MAX];
static size_t fo_nexp;
static uint32_t fo_max_order;

static void fo_build_expected(void)
{
  size_t n = 0;
  for (size_t i = 0, c = ccord_store_favorite_gif_count(fo_ctx->store); i < c && n < FO_MAX; i++) {
    const ccord_gif_item *it = ccord_store_favorite_gif_at(fo_ctx->store, i);
    bool ours = false;
    for (size_t k = 0; k < FO_N; k++) ours = ours || strcmp(it->url, fo_wire[k].url) == 0;
    if (ours) continue;
    (void)snprintf(fo_extra_url[n], CCORD_URL_MAX_BYTES + 1u, "%s", it->url);
    fo_exp[n] = fo_extra_url[n];
    fo_exp_order[n++] = it->order;
  }
  // sortBy("order").reverse(): stable ascending in wire order, then reversed. Equivalent: walk the wire backwards and
  // stable-sort descending.
  for (size_t i = FO_N; i > 0 && n < FO_MAX; i--) {
    fo_exp[n] = fo_wire[i - 1].url;
    fo_exp_order[n++] = fo_wire[i - 1].order;
  }
  for (size_t i = 1; i < n; i++) {   // stable insertion sort, order descending
    const char *u = fo_exp[i];
    uint32_t o = fo_exp_order[i];
    size_t j = i;
    while (j > 0 && fo_exp_order[j - 1] < o) {
      fo_exp[j] = fo_exp[j - 1];
      fo_exp_order[j] = fo_exp_order[j - 1];
      j--;
    }
    fo_exp[j] = u;
    fo_exp_order[j] = o;
  }
  fo_nexp = n;
  fo_max_order = n ? fo_exp_order[0] : 0;
}

// The GIF tiles drawn in the last frame, in the picker's placement order.
static size_t fo_read_tiles(ui_expr_hit_info *out, size_t cap)
{
  ui_expr_inspect ins;
  size_t n = 0;
  if (!ui_expr_get_inspect(fo_ctx->ui, &ins)) return 0;
  for (size_t h = 0; h < ins.nhits && n < cap; h++) {
    ui_expr_hit_info hi;
    if (ui_expr_hit_at(fo_ctx->ui, h, &hi) && hi.kind == 2) out[n++] = hi;
  }
  return n;
}

// Checks the drawn tiles against `urls` (the reference sequence): same sequence, and each tile in the shortest column.
static int fo_check_picker(const char *const *urls, size_t nurls, const char *what)
{
  ui_expr_inspect ins;
  if (!ui_expr_get_inspect(fo_ctx->ui, &ins)) return fo_fail("%s: no picker", what);
  ui_expr_hit_info tiles[32];
  size_t nt = fo_read_tiles(tiles, 32);
  if (nt < 4) return fo_fail("%s: only %zu favourite tiles drawn", what, nt);
  if (nt > nurls) nt = nurls;
  for (size_t i = 0; i < nt; i++) {
    if (strcmp(tiles[i].url, urls[i]) != 0) {
      return fo_fail("%s: tile %zu is %s, the reference has %s", what, i, tiles[i].url, urls[i]);
    }
  }
  fo_checks++;
  // Tile shape: columnWidth * height / width, unclamped (checked for the entries whose shape the slice knows).
  for (size_t i = 0; i < nt; i++) {
    for (size_t k = 0; k < FO_N; k++) {
      if (strcmp(tiles[i].url, fo_wire[k].url) != 0) continue;
      float want = tiles[i].rect.w * (float)fo_wire[k].h / (float)fo_wire[k].w;
      if (fabsf(tiles[i].rect.h - want) > 1.0f) {
        return fo_fail("%s: tile %s is %.1f px tall, columnWidth*h/w is %.1f", what, tiles[i].url,
                       (double)tiles[i].rect.h, (double)want);
      }
    }
  }
  fo_checks++;
  // Masonry: columns by distinct x; the first tile of each column starts at the grid top. The next tile goes to the
  // column whose bottom is highest (leftmost on a tie), using the heights the picker itself drew.
  float col_x[8], col_y[8];
  size_t ncols = 0;
  float top = tiles[0].rect.y;
  for (size_t i = 0; i < nt && ncols < 8; i++) {
    if (fabsf(tiles[i].rect.y - top) > 0.5f) continue;
    col_x[ncols] = tiles[i].rect.x;
    col_y[ncols] = top;
    ncols++;
  }
  {
    // Discord's rule: min(max(floor(width/200),1),8); the grid width is the drawn view.
    int want = (int)(ins.grid_view.w / 200.0f);
    want = want < 1 ? 1 : (want > 8 ? 8 : want);
    if (nt >= (size_t)want && (int)ncols != want) {
      return fo_fail("%s: %zu columns drawn, Discord's rule gives %d for %.0f px", what, ncols, want, (double)ins.grid_view.w);
    }
    fo_checks++;
  }
  if (ncols < 2) return fo_fail("%s: expected a multi-column grid, found %zu column(s)", what, ncols);
  // The gap between stacked cards, read from the first column's first two cards.
  float gap = -1.0f;
  for (size_t i = 1; i < nt && gap < 0.0f; i++) {
    if (fabsf(tiles[i].rect.x - col_x[0]) <= 0.5f) gap = tiles[i].rect.y - (tiles[0].rect.y + tiles[0].rect.h);
  }
  if (gap < 0.0f || gap > 40.0f) return fo_fail("%s: cannot read the card gap (%.1f)", what, (double)gap);
  for (size_t c = 1; c < ncols; c++) {
    if (col_x[c] <= col_x[c - 1]) return fo_fail("%s: columns are not left to right", what);
  }
  for (size_t i = 0; i < nt; i++) {
    size_t col = 0;
    for (size_t c = 1; c < ncols; c++) {
      if (col_y[c] + 0.5f < col_y[col]) col = c;
    }
    if (fabsf(tiles[i].rect.x - col_x[col]) > 0.5f || fabsf(tiles[i].rect.y - col_y[col]) > 0.5f) {
      return fo_fail("%s: tile %zu (%s) sits at (%.0f,%.0f), the shortest column is %zu at (%.0f,%.0f)", what, i,
                     tiles[i].url, (double)tiles[i].rect.x, (double)tiles[i].rect.y, col, (double)col_x[col],
                     (double)col_y[col]);
    }
    col_y[col] += tiles[i].rect.h + gap;
  }
  fo_checks++;
  return 0;
}

static int fo_run_body(const ccord_verify_ui_parity_ctx *ctx)
{
  fo_ctx = ctx;
  fo_checks = 0;
  ccord_error err = {0};

  ccord_id chan = CCORD_ID_NONE;
  for (int i = 0; i < 400 && chan == CCORD_ID_NONE; i++) {
    fo_tick(1);
    if (ccord_session_state(ctx->session) == CCORD_CONN_CONNECTED) chan = ccord_store_selected_channel(ctx->store);
  }
  if (chan == CCORD_ID_NONE) return fo_fail("setup: mock did not connect");
  if (ccord_session_select_channel(ctx->session, FO_CHAN_TEXT, &err) != CCORD_OK) {
    return fo_fail("setup: select the text channel: %s", err.message);
  }
  for (int i = 0; i < 100 && ccord_store_selected_channel(ctx->store) != FO_CHAN_TEXT; i++) fo_tick(1);
  ui_expr_picker_close(ctx->ui);
  ui_expr_picker_open(ctx->ui, 0, FO_CHAN_TEXT, CCORD_ID_NONE);
  for (int i = 0; i < 200; i++) {
    fo_tick(1);
    if (ccord_store_expr_blob_state(ctx->store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_READY) break;
  }
  if (ccord_store_expr_blob_state(ctx->store, CCORD_EXPR_SETTINGS_PROTO, nullptr) != CCORD_EXPR_BLOB_READY) {
    return fo_fail("setup: settings-proto did not load");
  }

  // 1. The server's answer, entries shuffled/tied: the store holds the reference sequence.
  uint8_t raw[8192];
  size_t rn = fo_build(raw);
  if (ccord_transport_patch_settings_proto(ctx->transport, raw, rn, &err) != CCORD_OK) {
    return fo_fail("setup: settings-proto answer refused: %s", err.message);
  }
  for (int i = 0; i < 300 && ccord_store_favorite_gif_count(ctx->store) < FO_N; i++) fo_tick(1);
  fo_tick(30);
  fo_build_expected();
  if (fo_nexp < FO_N) return fo_fail("setup: expected sequence is short");
  const char *const *expect = fo_exp;
  const size_t nexp = fo_nexp;
  char dump[1024];
  if (!fo_store_is(expect, nexp) || ccord_store_favorite_gif_count(ctx->store) != nexp) {
    fo_store_dump(dump, sizeof dump);
    char want[512] = "";
    for (size_t i = 0, off = 0; i < nexp && off + 24 < sizeof want; i++) {
      const char *tail = strrchr(expect[i], '/');
      off += (size_t)snprintf(want + off, sizeof want - off, "%s ", tail ? tail + 1 : expect[i]);
    }
    return fo_fail("store sequence is not sortBy(order).reverse(): %s| want %s", dump, want);
  }
  fo_checks++;

  // 2. The same sequence and masonry placement in the real Favorites view.
  ui_expr_inspect ins;
  ccord_rect card = {0, 0, 0, 0};
  for (int i = 0; i < 100 && card.w == 0.0f; i++) {
    fo_tick(1);
    if (!ui_expr_get_inspect(ctx->ui, &ins)) continue;
    for (size_t h = 0; h < ins.nhits; h++) {
      ui_expr_hit_info hi;
      if (ui_expr_hit_at(ctx->ui, h, &hi) && hi.kind == 4) card = hi.rect;
    }
  }
  if (card.w == 0.0f) return fo_fail("picker: Favorites card missing");
  fo_click(card.x + card.w * 0.5f, card.y + card.h * 0.5f);
  fo_tick(2);
  if (!ui_expr_get_inspect(ctx->ui, &ins) || ins.gif_subview != 1) return fo_fail("picker: Favorites view did not open");
  int rc = fo_check_picker(expect, nexp, "shuffled settings");
  if (rc) return rc;

  // 3. A second settings update carrying the same data leaves the sequence unchanged (store and picker).
  if (ccord_transport_patch_settings_proto(ctx->transport, raw, rn, &err) != CCORD_OK) {
    return fo_fail("second update refused: %s", err.message);
  }
  fo_tick(30);
  if (!fo_store_is(expect, nexp) || ccord_store_favorite_gif_count(ctx->store) != nexp) {
    fo_store_dump(dump, sizeof dump);
    return fo_fail("a second identical settings update changed the sequence: %s", dump);
  }
  fo_checks++;
  rc = fo_check_picker(expect, nexp, "after the second update");
  if (rc) return rc;

  // 4. Favouriting a new GIF locally: Discord gives it max(order)+1, so it is first; the answer that follows the
  // PATCH (the mock echoes what was sent) leaves it there.
  ccord_gif_item gi = {0};
  (void)snprintf(gi.url, sizeof gi.url, "%s", fo_new_url);
  (void)snprintf(gi.src, sizeof gi.src, "https://media.tenor.com/fo-new.mp4");
  gi.width = 320;
  gi.height = 200;
  gi.format = 2;
  if (ccord_session_toggle_favorite_gif(ctx->session, &gi, &err) != CCORD_OK) {
    return fo_fail("favouriting a new gif failed: %s", err.message);
  }
  fo_tick(40);
  const ccord_gif_item *first = ccord_store_favorite_gif_at(ctx->store, 0);
  if (!first || strcmp(first->url, fo_new_url) != 0 || first->order != fo_max_order + 1u) {
    return fo_fail("a new favourite is not first with order max+1 (first=%s order=%u)", first ? first->url : "none",
                   first ? (unsigned)first->order : 0u);
  }
  const char *after[FO_MAX + 1];
  after[0] = fo_new_url;
  for (size_t i = 0; i < nexp; i++) after[i + 1] = expect[i];
  if (!fo_store_is(after, nexp + 1) || ccord_store_favorite_gif_count(ctx->store) != nexp + 1) {
    fo_store_dump(dump, sizeof dump);
    return fo_fail("local favourite disturbed the rest of the sequence: %s", dump);
  }
  fo_checks++;
  rc = fo_check_picker(after, nexp + 1, "after a local favourite");
  if (rc) return rc;

  // 5. A settings answer with no favorite_gifs submessage says nothing about favourites: every one stays.
  const uint8_t no_gifs[] = {0x3a, 0x02, 'a', 'b'};   // an unrelated length-delimited root field (7)
  if (ccord_transport_patch_settings_proto(ctx->transport, no_gifs, sizeof no_gifs, &err) != CCORD_OK) {
    return fo_fail("answer without favorite_gifs refused: %s", err.message);
  }
  fo_tick(30);
  if (!fo_store_is(after, nexp + 1) || ccord_store_favorite_gif_count(ctx->store) != nexp + 1) {
    fo_store_dump(dump, sizeof dump);
    return fo_fail("an answer without favorite_gifs changed the favourites: %s", dump);
  }
  fo_checks++;

  // 6. An answer that fails to decode (the submessage claims 5 bytes, 1 follows) leaves every favourite as well.
  const uint8_t bad_proto[] = {0x12, 0x05, 0x0a};
  if (ccord_transport_patch_settings_proto(ctx->transport, bad_proto, sizeof bad_proto, &err) != CCORD_OK) {
    return fo_fail("malformed answer refused by the mock: %s", err.message);
  }
  fo_tick(30);
  if (!fo_store_is(after, nexp + 1) || ccord_store_favorite_gif_count(ctx->store) != nexp + 1) {
    fo_store_dump(dump, sizeof dump);
    return fo_fail("an answer that fails to decode changed the favourites: %s", dump);
  }
  fo_checks++;

  // 7. A second settings load whose favorite_gifs omits one earlier favourite (fo_wire[2], unstarred on another
  // device) is authoritative: that GIF is gone, and so is the local favourite the list does not contain; the
  // others keep their sequence, in the store and in the picker.
  const unsigned omit = 1u << 2;
  uint8_t raw2[8192];
  size_t rn2 = fo_build_skip(raw2, omit);
  if (ccord_transport_patch_settings_proto(ctx->transport, raw2, rn2, &err) != CCORD_OK) {
    return fo_fail("settings answer without a favourite refused: %s", err.message);
  }
  fo_tick(30);
  const char *kept[FO_MAX];
  size_t nkept = 0;
  for (size_t i = 0; i < nexp; i++) {
    if (strcmp(expect[i], fo_wire[2].url) != 0) kept[nkept++] = expect[i];
  }
  if (ccord_store_is_favorite_gif(ctx->store, fo_wire[2].url) || !fo_store_is(kept, nkept) ||
      ccord_store_favorite_gif_count(ctx->store) != nkept) {
    fo_store_dump(dump, sizeof dump);
    return fo_fail("a favourite missing from the settings list was not removed (or the rest moved): %s", dump);
  }
  fo_checks++;
  rc = fo_check_picker(kept, nkept, "after an unfavourite elsewhere");
  if (rc) return rc;

  // 8. Local changes whose PATCH was refused (so never acknowledged) survive an older answer: the starred GIF stays,
  // the unstarred one is not re-added, and the rest keep their sequence. Without this a later PATCH built from the
  // store would delete the user's favourites on Discord.
  ccord_mock_set_refuse_writes(ctx->transport, true);
  if (ccord_session_toggle_favorite_gif(ctx->session, &gi, &err) != CCORD_OK) {
    return fo_fail("starring again failed: %s", err.message);
  }
  const ccord_gif_item *top = ccord_store_favorite_gif_at(ctx->store, 1);   // [0] is the new star, [1] the old top
  ccord_gif_item unstar = {0};
  if (!top) return fo_fail("no favourite to unstar");
  unstar = *top;
  if (ccord_session_toggle_favorite_gif(ctx->session, &unstar, &err) != CCORD_OK) {
    return fo_fail("unstarring failed: %s", err.message);
  }
  ccord_mock_set_refuse_writes(ctx->transport, false);
  if (ccord_transport_patch_settings_proto(ctx->transport, raw2, rn2, &err) != CCORD_OK) {
    return fo_fail("older settings answer refused: %s", err.message);
  }
  fo_tick(40);
  const char *pend[FO_MAX + 1];
  size_t npend = 0;
  pend[npend++] = fo_new_url;
  for (size_t i = 0; i < nkept; i++) {
    if (strcmp(kept[i], unstar.url) != 0) pend[npend++] = kept[i];
  }
  if (ccord_store_is_favorite_gif(ctx->store, unstar.url)) {
    return fo_fail("a locally unstarred favourite came back from an older answer");
  }
  if (!ccord_store_is_favorite_gif(ctx->store, fo_new_url)) {
    return fo_fail("a locally starred favourite was dropped by an older answer");
  }
  if (!fo_store_is(pend, npend) || ccord_store_favorite_gif_count(ctx->store) != npend) {
    fo_store_dump(dump, sizeof dump);
    return fo_fail("pending local changes disturbed the sequence: %s", dump);
  }
  fo_checks++;

  // 9. Another client's settings update arrives over the gateway (type 2 USER_SETTINGS_PROTO_UPDATE), with no settings
  // reload. Only a whole (non-partial) proto that carries favorite_gifs is authoritative; anything else removes nothing.
  // `pend` is the store now. A delta that lists some favourites:
  uint8_t raw_one[512];
  size_t rn_one = fo_build_skip(raw_one, ~1u);   // fo_wire[0] only
  if (ccord_mock_inject_settings_proto_update(ctx->transport, raw_one, rn_one, true, &err) != CCORD_OK) {
    return fo_fail("partial update not queued: %s", err.message);
  }
  fo_tick(30);
  if (!fo_store_is(pend, npend) || ccord_store_favorite_gif_count(ctx->store) != npend) {
    fo_store_dump(dump, sizeof dump);
    return fo_fail("a partial gateway update removed or moved favourites: %s", dump);
  }
  fo_checks++;
  // A whole update without the favorite_gifs submessage:
  if (ccord_mock_inject_settings_proto_update(ctx->transport, no_gifs, sizeof no_gifs, false, &err) != CCORD_OK) {
    return fo_fail("update without favorite_gifs not queued: %s", err.message);
  }
  fo_tick(30);
  if (!fo_store_is(pend, npend) || ccord_store_favorite_gif_count(ctx->store) != npend) {
    fo_store_dump(dump, sizeof dump);
    return fo_fail("a gateway update without favorite_gifs removed favourites: %s", dump);
  }
  fo_checks++;
  // A whole update that does not decode:
  if (ccord_mock_inject_settings_proto_update(ctx->transport, bad_proto, sizeof bad_proto, false, &err) != CCORD_OK) {
    return fo_fail("undecodable update not queued: %s", err.message);
  }
  fo_tick(30);
  if (!fo_store_is(pend, npend) || ccord_store_favorite_gif_count(ctx->store) != npend) {
    fo_store_dump(dump, sizeof dump);
    return fo_fail("an undecodable gateway update removed favourites: %s", dump);
  }
  fo_checks++;
  // A whole update that omits fo_wire[2] and fo_wire[6] (unstarred on another device) and does not list the GIF this
  // client starred a moment ago (its PATCH never went through, so it is still pending): fo_wire[6] goes, the pending star
  // stays, the rest keep their sequence.
  const unsigned omit2 = (1u << 2) | (1u << 6);
  uint8_t raw3[8192];
  size_t rn3 = fo_build_skip(raw3, omit2);
  if (ccord_mock_inject_settings_proto_update(ctx->transport, raw3, rn3, false, &err) != CCORD_OK) {
    return fo_fail("whole update not queued: %s", err.message);
  }
  fo_tick(30);
  const char *live[FO_MAX + 1];
  size_t nlive = 0;
  for (size_t i = 0; i < npend; i++) {
    if (strcmp(pend[i], fo_wire[6].url) != 0) live[nlive++] = pend[i];
  }
  if (nlive + 1 != npend) return fo_fail("setup: fo_wire[6] is not among the favourites");
  if (ccord_store_is_favorite_gif(ctx->store, fo_wire[6].url)) {
    return fo_fail("a favourite missing from a whole gateway update was not removed");
  }
  fo_checks++;
  if (!ccord_store_is_favorite_gif(ctx->store, fo_new_url)) {
    return fo_fail("a favourite starred here and still pending was removed by a gateway update that lacks it");
  }
  if (!fo_store_is(live, nlive) || ccord_store_favorite_gif_count(ctx->store) != nlive) {
    fo_store_dump(dump, sizeof dump);
    return fo_fail("the gateway update disturbed the remaining sequence: %s", dump);
  }
  fo_checks++;
  rc = fo_check_picker(live, nlive, "after a gateway unfavourite");
  if (rc) return rc;

  {
    static const float widths[] = {344.0f, 428.0f, 620.0f, 820.0f, 1700.0f, 2300.0f};
    static const int want[] = {1, 2, 3, 4, 8, 8};   // floor((w-20)/200) clamped to 1..8
    for (size_t i = 0; i < sizeof widths / sizeof *widths; i++) {
      ui_expr_grid_layout lay;
      ui_expr_grid_layout_for_width(widths[i], &lay);
      if (lay.gif_cols != want[i]) {
        return fo_fail("%.0f px popover: %d GIF columns, Discord's rule gives %d", (double)widths[i], lay.gif_cols, want[i]);
      }
      fo_checks++;
    }
  }

  printf("fav-gif-order: PASS checks=%u\n", fo_checks);
  return 0;
}

int ccord_verify_fav_gif_order_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->renderer || !ctx->transport || !ctx->frames ||
      !ctx->input) {
    return fo_fail("invalid context");
  }
  fo_extra_url = calloc(FO_MAX, sizeof *fo_extra_url);
  if (!fo_extra_url) return fo_fail("out of memory");
  int rc = fo_run_body(ctx);
  free(fo_extra_url);
  fo_extra_url = nullptr;
  return rc;
}
