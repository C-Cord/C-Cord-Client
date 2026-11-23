// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice app-icon: the embedded C-Cord logo is what the app, window and tray use.
// Checks: exact-size pixels (transparent + opaque) for every app size; the 256 px data holds the indigo #3B2FA0
// mark (plus teal and white, so not blank or single-colour); the tray variant at 22/24 (and 32/48) px keeps its
// corner free for the badge; the headless platform attached the embedded icon to its window; the tray serves
// non-empty IconPixmap data; the .desktop entry matches the app id.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/platform/app_icon.h>
#include <ccord/platform/platform.h>
#include <ccord/platform/tray_icon.h>

#include "verify_ui_parity.h"

static int s_checks;
#define CHECK(cond, what)                              \
  do {                                                 \
    s_checks++;                                        \
    if (!(cond)) {                                     \
      printf("app-icon: FAIL %s\n", what);             \
      return 1;                                        \
    }                                                  \
  } while (0)

static void count_px(const uint8_t *px, int size, size_t *clear, size_t *opaque, size_t *indigo, size_t *teal, size_t *white)
{
  *clear = *opaque = *indigo = *teal = *white = 0;
  for (size_t i = 0; i < (size_t)size * (size_t)size; i++) {
    const uint8_t *p = px + i * 4;
    if (p[3] == 0) (*clear)++;
    if (p[3] < 200) continue;
    (*opaque)++;
    if (abs(p[0] - 0x3B) < 12 && abs(p[1] - 0x2F) < 12 && abs(p[2] - 0xA0) < 12) (*indigo)++;
    if (abs(p[0] - 0x1F) < 12 && abs(p[1] - 0xD1) < 12 && abs(p[2] - 0xC0) < 12) (*teal)++;
    if (p[0] > 245 && p[1] > 245 && p[2] > 245) (*white)++;
  }
}

static int check_sizes(void)
{
  static const int sizes[] = {16, 22, 24, 32, 48, 64, 128, 256, 100, 512};
  const size_t buf_sz = 512u * 512u * 4u + 1u;
  uint8_t *buf = malloc(buf_sz);
  CHECK(buf != nullptr, "alloc icon buffer");
  for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
    int sz = sizes[i];
    size_t n = (size_t)sz * (size_t)sz * 4u;
    memset(buf, 0xAA, buf_sz);
    CHECK(ccord_app_icon_rgba(sz, buf, n), "app icon rgba");
    CHECK(buf[n] == 0xAA, "no write past the exact size");
    size_t clear, opaque, indigo, teal, white;
    count_px(buf, sz, &clear, &opaque, &indigo, &teal, &white);
    CHECK(clear > 0, "has fully transparent pixels");
    CHECK(opaque > 0, "has opaque pixels");
    if (sz == 256) {
      CHECK(indigo > 2000, "256 px has the indigo #3B2FA0 mark");
      CHECK(teal > 200 && white > 200, "256 px has teal plug and white outline");
      CHECK(indigo * 100 < (size_t)sz * (size_t)sz * 90u, "256 px is not a single colour");
    }
  }
  CHECK(!ccord_app_icon_rgba(256, buf, 256 * 256 * 4 - 1), "short buffer rejected");
  CHECK(!ccord_app_icon_rgba(4, buf, buf_sz), "tiny size rejected");
  free(buf);
  return 0;
}

static int check_tray(void)
{
  const size_t px_sz = 48u * 48u * 4u;
  uint8_t *base = malloc(px_sz);
  uint8_t *badge = malloc(px_sz);
  uint8_t *app = malloc(px_sz);
  CHECK(base && badge && app, "alloc tray icon buffers");
  static const int sizes[] = {22, 24, 32, 48};
  for (size_t i = 0; i < 4; i++) {
    int sz = sizes[i];
    size_t n = (size_t)sz * (size_t)sz * 4u;
    CHECK(ccord_tray_icon_rgba(sz, 0, false, base, n), "tray icon rgba");
    size_t clear, opaque, indigo, teal, white;
    count_px(base, sz, &clear, &opaque, &indigo, &teal, &white);
    CHECK(clear > 0 && opaque > 0, "tray icon has transparent and opaque pixels");
    CHECK(indigo > 0, "tray icon carries the indigo mark");
    CHECK(ccord_app_icon_rgba(sz, app, n) && memcmp(app, base, n) != 0, "tray variant differs from the app icon");
    // The bottom-right corner is free for the badge in the tray variant.
    bool corner_clear = true;
    for (int y = sz - 3; y < sz; y++)
      for (int x = sz - 3; x < sz; x++)
        if (base[((size_t)y * (size_t)sz + (size_t)x) * 4 + 3] > 40) corner_clear = false;
    CHECK(corner_clear, "tray corner is free for the badge");
    CHECK(ccord_tray_icon_rgba(sz, 5, true, badge, n) && memcmp(base, badge, n) != 0, "badge drawn over the tray icon");
  }
  free(base);
  free(badge);
  free(app);
  return 0;
}

static int check_platform(void)
{
  ccord_error err;
  ccord_platform_config cfg = {.title = "C-Cord", .width = 320, .height = 200, .headless = true};
  ccord_platform *p = ccord_platform_create(&cfg, &err);
  CHECK(p != nullptr, "headless platform");
  uint32_t icons = ccord_platform_window_icon_sizes(p);
  CHECK(icons >= 3, "window icon built from several embedded sizes and passed to SDL");
  // Acceptance depends on the video driver (the dummy driver refuses icons), so it is exposed but not required.
  (void)ccord_platform_window_icon_accepted(p);
  const char *id = ccord_platform_app_id();
  CHECK(id && strcmp(id, "C-Cord") == 0, "window app id is C-Cord");
  ccord_platform_set_badge_counts(p, 3, 1, 2);
  for (size_t i = 0; i < 4; i++) {
    int w = 0, h = 0;
    const uint8_t *argb = nullptr;
    CHECK(ccord_platform_tray_pixmap(p, i, &w, &h, &argb) && w > 0 && h == w && argb, "tray pixmap present");
    size_t nonzero = 0;
    for (size_t k = 0; k < (size_t)w * (size_t)h * 4u; k++) nonzero += argb[k] != 0;
    CHECK(nonzero > 0, "tray pixmap data is non-empty");
  }
  ccord_platform_destroy(p);
  return 0;
}

static int check_desktop_file(void)
{
  const char *root = getenv("CCORD_SOURCE_DIR");
  char path[512];
  snprintf(path, sizeof path, "%s/assets/icons/C-Cord.desktop", root && root[0] ? root : ".");
  FILE *f = fopen(path, "r");
  CHECK(f != nullptr, ".desktop file found (set CCORD_SOURCE_DIR outside the source tree)");
  char line[256];
  bool icon = false, wm = false;
  while (fgets(line, sizeof line, f)) {
    icon |= strncmp(line, "Icon=C-Cord", 11) == 0;
    wm |= strncmp(line, "StartupWMClass=C-Cord", 21) == 0;
  }
  fclose(f);
  CHECK(icon && wm, ".desktop Icon and StartupWMClass match the app id");
  return 0;
}

int ccord_verify_app_icon_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  (void)ctx;
  s_checks = 0;
  if (check_sizes() != 0 || check_tray() != 0 || check_platform() != 0 || check_desktop_file() != 0) return 1;
  printf("app-icon: PASS checks=%d\n", s_checks);
  return 0;
}
