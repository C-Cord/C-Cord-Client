// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice tray-unread: the taskbar entry is never flashed and the tray icon carries the unread count.
//
// Checks (headless platform, mock store): notify and badge updates while unfocused request no window flash; the
// rendered tray icon differs between read, unread dot, 1, 9, 27 and 150, and 150 is the "99+" picture; the
// IconPixmap images (22/24/32/48 px) are ARGB32 in network byte order; NewIcon is decided only when the picture
// changes; the app-level badge sync turns mentions + unread DMs into the tray number.
// Set CCORD_TRAY_UNREAD_PNG=<path> to also write the enlarged preview sheet (docs/screenshots/tray-unread.png).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/error.h>
#include <ccord/platform/app_icon.h>
#include <ccord/platform/platform.h>
#include <ccord/platform/tray_icon.h>

#include "app_desktop.h"
#include "verify_ui_parity.h"

static int s_checks;

#define CHECK(cond, what)                                      \
  do {                                                         \
    if (!(cond)) {                                             \
      printf("tray-unread: FAIL %s\n", what);                  \
      return 1;                                                \
    }                                                          \
    s_checks++;                                                \
  } while (0)

static const int k_sizes[4] = {22, 24, 32, 48};

typedef struct state {
  const char *name;
  int count;
  bool unread;
} state;

// Read, unread dot, 1, 9, 27, 150 (the sheet order).
static const state k_states[6] = {{"read", 0, false}, {"dot", 0, true}, {"1", 1, true},
                                  {"9", 9, true},     {"27", 27, true}, {"150", 150, true}};

// ---- PNG writer (stored deflate blocks; enough for a preview sheet) ---------------------------------------------

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static bool png_chunk(FILE *f, const char type[4], const uint8_t *data, size_t n)
{
  uint8_t head[8];
  put32(head, (uint32_t)n);
  memcpy(head + 4, type, 4);
  uint32_t crc = crc32_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
  if (n) crc = crc32_update(crc, data, n);
  uint8_t tail[4];
  put32(tail, ~crc);
  return fwrite(head, 1, 8, f) == 8 && (n == 0 || fwrite(data, 1, n, f) == n) && fwrite(tail, 1, 4, f) == 4;
}

static bool write_png_rgba(const char *path, const uint8_t *rgba, int w, int h)
{
  size_t row = (size_t)w * 4u + 1u, raw_len = row * (size_t)h;
  uint8_t *raw = ccord_malloc(raw_len);
  if (!raw) return false;
  for (int y = 0; y < h; y++) {
    raw[(size_t)y * row] = 0;
    memcpy(raw + (size_t)y * row + 1, rgba + (size_t)y * (size_t)w * 4u, (size_t)w * 4u);
  }
  size_t blocks = (raw_len + 65534u) / 65535u;
  size_t z_len = 2 + raw_len + blocks * 5 + 4;
  uint8_t *z = ccord_malloc(z_len);
  if (!z) {
    ccord_free(raw);
    return false;
  }
  size_t o = 0;
  z[o++] = 0x78;
  z[o++] = 0x01;
  for (size_t off = 0; off < raw_len; off += 65535u) {
    size_t n = raw_len - off < 65535u ? raw_len - off : 65535u;
    z[o++] = off + n >= raw_len ? 1 : 0;
    z[o++] = (uint8_t)(n & 0xFF);
    z[o++] = (uint8_t)(n >> 8);
    z[o++] = (uint8_t)(~n & 0xFF);
    z[o++] = (uint8_t)((~n >> 8) & 0xFF);
    memcpy(z + o, raw + off, n);
    o += n;
  }
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw_len; i++) {
    a = (a + raw[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  put32(z + o, (b << 16) | a);
  o += 4;
  FILE *f = fopen(path, "wb");
  bool ok = f != nullptr;
  if (ok) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    ok = fwrite(sig, 1, 8, f) == 8 && png_chunk(f, "IHDR", ihdr, 13) && png_chunk(f, "IDAT", z, o) &&
         png_chunk(f, "IEND", nullptr, 0);
    ok = (fclose(f) == 0) && ok;
  }
  ccord_free(z);
  ccord_free(raw);
  return ok;
}

// Sheet: columns = the six states, row 1 = 22 px (8x), row 2 = 48 px (4x), nearest-neighbour over a dark panel.
static bool write_sheet(const char *path)
{
  enum { CELL = 200, COLS = 6, ROWS = 2, W = CELL * COLS, H = CELL * ROWS };
  uint8_t *img = ccord_malloc((size_t)W * H * 4u);
  if (!img) return false;
  for (size_t i = 0; i < (size_t)W * H; i++) {
    img[i * 4 + 0] = 0x2B;
    img[i * 4 + 1] = 0x2D;
    img[i * 4 + 2] = 0x31;
    img[i * 4 + 3] = 0xFF;
  }
  for (int r = 0; r < ROWS; r++) {
    int sz = r == 0 ? 22 : 48, mag = r == 0 ? 8 : 4;
    for (int c = 0; c < COLS; c++) {
      uint8_t px[48 * 48 * 4];
      if (!ccord_tray_icon_rgba(sz, k_states[c].count, k_states[c].unread, px, sizeof px)) {
        ccord_free(img);
        return false;
      }
      int ox = c * CELL + (CELL - sz * mag) / 2, oy = r * CELL + (CELL - sz * mag) / 2;
      for (int y = 0; y < sz * mag; y++) {
        for (int x = 0; x < sz * mag; x++) {
          const uint8_t *s = px + ((size_t)(y / mag) * (size_t)sz + (size_t)(x / mag)) * 4u;
          uint8_t *d = img + ((size_t)(oy + y) * W + (size_t)(ox + x)) * 4u;
          for (int k = 0; k < 3; k++) d[k] = (uint8_t)((s[k] * s[3] + d[k] * (255 - s[3])) / 255);
        }
      }
    }
  }
  bool ok = write_png_rgba(path, img, W, H);
  ccord_free(img);
  return ok;
}

// ---- checks -------------------------------------------------------------------------------------------------------

static bool render(int size, int count, bool unread, uint8_t *out)
{
  return ccord_tray_icon_rgba(size, count, unread, out, (size_t)size * (size_t)size * 4u);
}

static int check_render_distinct(void)
{
  for (int si = 0; si < 4; si++) {
    int sz = k_sizes[si];
    size_t n = (size_t)sz * (size_t)sz * 4u;
    uint8_t px[6][48 * 48 * 4];
    for (int i = 0; i < 6; i++) CHECK(render(sz, k_states[i].count, k_states[i].unread, px[i]), "render state");
    for (int i = 0; i < 6; i++) {
      for (int j = i + 1; j < 6; j++) {
        CHECK(memcmp(px[i], px[j], n) != 0, "states render differently");
      }
    }
    uint8_t other[48 * 48 * 4];
    uint8_t plain[48 * 48 * 4];
    CHECK(render(sz, 100, true, other) && memcmp(other, px[5], n) == 0, "100 renders as the 150 picture (99+)");
    CHECK(render(sz, 100000, true, other) && memcmp(other, px[5], n) == 0, "100000 renders as 99+");
    CHECK(render(sz, 99, true, other) && memcmp(other, px[5], n) != 0, "99 differs from 99+");
    CHECK(ccord_tray_base_rgba(sz, plain, n) && memcmp(plain, px[0], n) == 0, "read state is the plain tray base icon");
    CHECK(render(sz, 0, true, other) && memcmp(other, px[1], n) == 0, "dot independent of count 0");
  }
  uint8_t tiny[4];
  CHECK(!ccord_tray_icon_rgba(22, 1, true, tiny, sizeof tiny), "short buffer refused");
  return 0;
}

static int check_no_flash(ccord_platform *p)
{
  CHECK(ccord_platform_flash_requests(p) == 0, "no flash at start");
  ccord_platform_set_window_focused(p, false);
  CHECK(ccord_platform_notify(p, "New mention", "hello @you"), "notify unfocused accepted");
  CHECK(ccord_platform_notify_count(p) == 1, "notify counted");
  CHECK(ccord_platform_flash_requests(p) == 0, "notify requests no flash");
  ccord_platform_set_unread_badge(p, 5, 3);
  CHECK(ccord_platform_flash_requests(p) == 0, "mention badge while unfocused requests no flash");
  ccord_platform_set_badge_counts(p, 9, 4, 7);
  CHECK(ccord_platform_flash_requests(p) == 0, "badge counts while unfocused request no flash");
  ccord_platform_set_window_focused(p, true);
  CHECK(ccord_platform_flash_requests(p) == 0, "focus gain only cancels");
  return 0;
}

static int check_pixmaps(ccord_platform *p, int count, bool unread)
{
  for (size_t i = 0; i < 4; i++) {
    int w = 0, h = 0;
    const uint8_t *argb = nullptr;
    CHECK(ccord_platform_tray_pixmap(p, i, &w, &h, &argb), "pixmap present");
    CHECK(w == k_sizes[i] && h == k_sizes[i] && argb, "pixmap size");
    uint8_t rgba[48 * 48 * 4];
    CHECK(render(w, count, unread, rgba), "reference render");
    for (size_t k = 0; k < (size_t)w * (size_t)h; k++) {
      if (argb[k * 4 + 0] != rgba[k * 4 + 3] || argb[k * 4 + 1] != rgba[k * 4 + 0] ||
          argb[k * 4 + 2] != rgba[k * 4 + 1] || argb[k * 4 + 3] != rgba[k * 4 + 2]) {
        printf("tray-unread: FAIL pixmap %d px is not ARGB32 network byte order\n", w);
        return 1;
      }
    }
  }
  int w = 0, h = 0;
  const uint8_t *argb = nullptr;
  CHECK(!ccord_platform_tray_pixmap(p, 4, &w, &h, &argb), "pixmap index out of range");
  return 0;
}

static int check_new_icon(ccord_platform *p)
{
  ccord_platform_set_badge_counts(p, 0, 0, 0);
  uint32_t n = ccord_platform_tray_new_icon_count(p);
  CHECK(check_pixmaps(p, 0, false) == 0, "read pixmaps");
  ccord_platform_set_badge_counts(p, 0, 0, 0);
  CHECK(ccord_platform_tray_new_icon_count(p) == n, "unchanged totals emit no NewIcon");
  ccord_platform_set_badge_counts(p, 3, 1, 1);
  CHECK(ccord_platform_tray_new_icon_count(p) == n + 1, "changed total emits NewIcon");
  CHECK(check_pixmaps(p, 1, true) == 0, "count 1 pixmaps");
  ccord_platform_set_badge_counts(p, 3, 1, 1);
  CHECK(ccord_platform_tray_new_icon_count(p) == n + 1, "repeated sync emits no NewIcon");
  ccord_platform_set_badge_counts(p, 8, 1, 1);
  CHECK(ccord_platform_tray_new_icon_count(p) == n + 1, "unread change under the same picture emits no NewIcon");
  ccord_platform_set_badge_counts(p, 8, 2, 27);
  CHECK(ccord_platform_tray_new_icon_count(p) == n + 2, "27 emits NewIcon");
  CHECK(check_pixmaps(p, 27, true) == 0, "count 27 pixmaps");
  ccord_platform_set_badge_counts(p, 8, 2, 150);
  CHECK(ccord_platform_tray_new_icon_count(p) == n + 3, "150 emits NewIcon");
  ccord_platform_set_badge_counts(p, 8, 2, 400);
  CHECK(ccord_platform_tray_new_icon_count(p) == n + 3, "400 is the same 99+ picture");
  ccord_platform_set_badge_counts(p, 2, 0, 0);
  CHECK(ccord_platform_tray_new_icon_count(p) == n + 4, "unread dot emits NewIcon");
  CHECK(check_pixmaps(p, 0, true) == 0, "dot pixmaps");
  ccord_platform_set_badge_counts(p, 0, 0, 0);
  CHECK(ccord_platform_tray_new_icon_count(p) == n + 5, "all read emits NewIcon");
  return 0;
}

// App level: the tray number = every mention + unread DM messages not already counted as DM mentions.
static int check_app_sync(const ccord_verify_ui_parity_ctx *ctx, ccord_platform *p)
{
  ccord_theme theme;
  memset(&theme, 0, sizeof theme);
  ccord_error err;
  ccord_app_desktop *d = ccord_app_desktop_create(
      &(ccord_app_desktop_deps){.store = ctx->store, .platform = p, .theme = &theme}, &err);
  CHECK(d != nullptr, "app desktop created");
  if (ctx->frames) ctx->frames(ctx->ud, 5);   // let the mock session populate the store first
  ccord_channel dm = {.id = 900001, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ada",
                      .topic = ""};
  CHECK(ccord_store_upsert_channel(ctx->store, &dm, &err) == CCORD_OK, "dm channel added");
  ccord_id sel = CCORD_ID_NONE;   // the first text channel of the first guild
  const ccord_guild *g0 = ccord_store_guild_at(ctx->store, 0);
  for (size_t i = 0; g0 && i < ccord_store_channel_count(ctx->store, g0->id) && sel == CCORD_ID_NONE; i++) {
    const ccord_channel *c = ccord_store_channel_at(ctx->store, g0->id, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT) sel = c->id;
  }
  CHECK(sel != CCORD_ID_NONE, "a text channel exists");
  CHECK(ccord_store_set_channel_counts(ctx->store, sel, 6, 2, &err) == CCORD_OK, "guild counts");
  CHECK(ccord_store_set_channel_counts(ctx->store, dm.id, 4, 1, &err) == CCORD_OK, "dm counts");
  ccord_app_desktop_sync_badge(d);
  // mentions 2 + 1 = 3; DM unread beyond its mention = 3; the 6 plain guild unread are not counted.
  CHECK(check_pixmaps(p, 6, true) == 0, "tray number is mentions + unread DMs");
  CHECK(ccord_store_set_channel_counts(ctx->store, sel, 6, 0, &err) == CCORD_OK, "guild mentions cleared");
  CHECK(ccord_store_set_channel_counts(ctx->store, dm.id, 0, 0, &err) == CCORD_OK, "dm cleared");
  ccord_app_desktop_sync_badge(d);
  CHECK(check_pixmaps(p, 0, true) == 0, "plain unread shows only the dot");
  CHECK(ccord_store_set_channel_counts(ctx->store, sel, 0, 0, &err) == CCORD_OK, "all read");
  ccord_app_desktop_sync_badge(d);
  CHECK(check_pixmaps(p, 0, false) == 0, "all read shows no badge");
  ccord_app_desktop_destroy(d);
  return 0;
}

int ccord_verify_tray_unread_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  s_checks = 0;
  if (check_render_distinct() != 0) return 1;
  ccord_error err;
  ccord_platform_config cfg = {.title = "C-Cord", .width = 320, .height = 200, .headless = true};
  ccord_platform *p = ccord_platform_create(&cfg, &err);
  CHECK(p != nullptr, "headless platform");
  int rc = check_no_flash(p);
  if (rc == 0) rc = check_new_icon(p);
  if (rc == 0 && ctx && ctx->store) rc = check_app_sync(ctx, p);
  ccord_platform_destroy(p);
  if (rc != 0) return rc;
  const char *png = getenv("CCORD_TRAY_UNREAD_PNG");
  if (png && png[0]) CHECK(write_sheet(png), "preview sheet written");
  printf("tray-unread: PASS checks=%d\n", s_checks);
  return 0;
}
