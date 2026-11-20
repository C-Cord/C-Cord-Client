// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <ftw.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include <ccord/core/paths.h>
#include <ccord/model/rich.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "ccord_test.h"

static int ccord_test_unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
  (void)sb;
  (void)typeflag;
  (void)ftwbuf;
  return remove(fpath);
}

static char *ccord_test_data_dir = nullptr;

static void ccord_test_cleanup_dir(void)
{
  if (ccord_test_data_dir) {
    nftw(ccord_test_data_dir, ccord_test_unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
    free(ccord_test_data_dir);
    ccord_test_data_dir = nullptr;
  }
}

static const char *ccord_test_init_dir(const char *prefix)
{
  if (ccord_test_data_dir) return ccord_test_data_dir;
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !tmp[0]) tmp = "/tmp";
  size_t len = strlen(tmp) + strlen(prefix) + 16;
  ccord_test_data_dir = malloc(len);
  if (!ccord_test_data_dir) abort();
  snprintf(ccord_test_data_dir, len, "%s/%s_XXXXXX", tmp, prefix);
  if (!mkdtemp(ccord_test_data_dir)) {
    free(ccord_test_data_dir);
    ccord_test_data_dir = nullptr;
    abort();
  }
  atexit(ccord_test_cleanup_dir);
  return ccord_test_data_dir;
}

static size_t parse_jumbo(const char *s, ui_jumbo_item out[static UI_JUMBO_EMOJI_MAX])
{
  return ui_parse_jumbo_emojis(s, s ? strlen(s) : 0, out);
}

static void test_parse_jumbo_emojis(void)
{
  ui_jumbo_item items[UI_JUMBO_EMOJI_MAX];

  // Empty and whitespace-only messages are not jumbo.
  CT_CHECK(parse_jumbo(nullptr, items) == 0);
  CT_CHECK(parse_jumbo("", items) == 0);
  CT_CHECK(parse_jumbo("   \t\r\n  ", items) == 0);

  // 1..8 Unicode emojis qualify (including complex ZWJ, dark skin tone, keycaps, flags, subdivision flags).
  CT_CHECK(parse_jumbo("\xF0\x9F\x94\xA5", items) == 1 && !items[0].custom);
  CT_CHECK(parse_jumbo("  \xF0\x9F\x98\x80 \xF0\x9F\x9A\x80 \xF0\x9F\x8E\x89  ", items) == 3);
  const char *eight =
      "\xF0\x9F\x98\x80\xF0\x9F\x98\x82\xF0\x9F\x94\xA5\xF0\x9F\x92\x80"
      "\xF0\x9F\x98\xAD\xF0\x9F\x91\x8D\xF0\x9F\x99\x8F\xF0\x9F\x8E\x89";
  CT_CHECK(parse_jumbo(eight, items) == 8);
  // 9 emojis exceed UI_JUMBO_EMOJI_MAX and stay normal inline size.
  const char *nine =
      "\xF0\x9F\x98\x80\xF0\x9F\x98\x82\xF0\x9F\x94\xA5\xF0\x9F\x92\x80"
      "\xF0\x9F\x98\xAD\xF0\x9F\x91\x8D\xF0\x9F\x99\x8F\xF0\x9F\x8E\x89\xF0\x9F\x9A\x80";
  CT_CHECK(parse_jumbo(nine, items) == 0);

  // Multi-codepoint grapheme clusters count as 1 emoji each (not bytes or code points).
  const char *complex =
      "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6 "  // 👨‍👩‍👧‍👦
      "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBF "                                                                      // 👍🏿
      "1\xEF\xB8\x8F\xE2\x83\xA3 "                                                                             // 1️⃣
      "\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xA5\xF3\xA0\x81\xAE\xF3\xA0\x81\xA7\xF3\xA0\x81\xBF " // 🏴󠁧󠁢󠁥󠁮󠁧󠁿
      "\xC2\xA9\xEF\xB8\x8F";                                                                                  // ©️
  CT_CHECK(parse_jumbo(complex, items) == 5);
  CT_CHECK(!items[0].line_break_before && !items[1].line_break_before);

  // Newlines between emojis are preserved via line_break_before.
  CT_CHECK(parse_jumbo("\xF0\x9F\x94\xA5\n\xF0\x9F\x9A\x80", items) == 2);
  CT_CHECK(!items[0].line_break_before && items[1].line_break_before);

  // Custom Discord emojis (static and animated) and mixed Unicode + custom emojis qualify up to 8.
  CT_CHECK(parse_jumbo("<:pepe:123456789012345678> <a:dance:987654321098765432> \xF0\x9F\x94\xA5", items) == 3);
  CT_CHECK(items[0].custom && !items[0].animated && items[0].id == 123456789012345678ULL);
  CT_CHECK(items[1].custom && items[1].animated && items[1].id == 987654321098765432ULL);
  CT_CHECK(!items[2].custom);

  // Any non-emoji text, punctuation, markdown, code, mention, bare ©, VS15 ❤︎, or plain digits disqualify jumbo.
  const char *rejects[] = {
      "\xF0\x9F\x98\x80 hi",
      "hi \xF0\x9F\x98\x80",
      "\xF0\x9F\x98\x80!",
      "`\xF0\x9F\x98\x80`",
      "**\xF0\x9F\x98\x80**",
      "# \xF0\x9F\x98\x80",
      "> \xF0\x9F\x98\x80",
      "1 2 3",
      "\xC2\xA9",                    // bare © without U+FE0F
      "\xE2\x9D\xA4\xEF\xB8\x8E",    // ❤︎ with VS15 text selector
      "<@123456>",
      "<:broken>",
      "<:pepe:123> text",
  };
  for (size_t i = 0; i < sizeof rejects / sizeof rejects[0]; i++) {
    CT_CHECK(parse_jumbo(rejects[i], items) == 0);
  }
}

typedef struct fixture {
  ccord_theme theme;
  ccord_bus *bus;
  ccord_store *store;
  ccord_session *session;
  ccord_transport *transport;
  ccord_contrib *contrib;
  ccord_platform *platform;
  ccord_renderer *renderer;
  ccord_ui *ui;
  uint64_t now;
} fixture;

static fixture F;

static uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

static void frame(int n)
{
  for (int i = 0; i < n; i++) {
    F.now += 50;
    (void)ccord_session_pump(F.session);
    (void)ccord_bus_drain(F.bus);
    (void)ccord_ui_update(F.ui, F.now);
    ccord_render_begin(F.renderer, F.theme.c.bg_primary);
    ccord_ui_draw(F.ui, F.renderer);
    ccord_render_end(F.renderer);
  }
}

static const vis_row *find_vis_row(ccord_id msg_id)
{
  for (size_t i = 0; i < F.ui->nvis; i++) {
    if (F.ui->vis[i].msg_id == msg_id) return &F.ui->vis[i];
  }
  return nullptr;
}

static void test_jumbo_ui_render(const char *argv0)
{
  ccord_error err;
  if (ccord_paths_init(argv0, &err) != CCORD_OK) {
    CT_CHECK(0);
    return;
  }
  ccord_paths_set_data_dir(ccord_test_init_dir("jumbo_emoji_data"));
  char tpath[1024];
  CT_CHECK(ccord_paths_join(tpath, sizeof tpath, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) == CCORD_OK);
  CT_CHECK(ccord_theme_load(&F.theme, tpath, &err) == CCORD_OK);
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 10, .latency_ms = 0, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  ccord_platform_config pc = {.title = "jumbo-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) return;
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  ccord_ui_deps deps = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib,
                        .theme = &F.theme, .fixed_now_unix_ms = 1793534400000LL};
  F.ui = ccord_ui_create(&deps, &err);
  ccord_ui_set_viewport(F.ui, 1280, 800);
  CT_CHECK(ccord_session_start(F.session, &err) == CCORD_OK);
  frame(4);

  ccord_id ch = ccord_store_selected_channel(F.store);
  CT_CHECK(ch != CCORD_ID_NONE);

  // Alternate authors (starting with 1002 so id_inline never merges with the last seed message by 1001).
  ccord_message_rich empty_r = {.present = CCORD_RICH_ALL};
  ccord_id id_inline = 0, id_one = 0, id_three = 0, id_eight = 0, id_nine = 0, id_mixed_custom = 0, id_grouped = 0;

  CT_CHECK(ccord_mock_post_rich_message(
               F.transport, ch, 1002,
               "Inline Twemoji: \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x92\xBB \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBF "
               "1\xEF\xB8\x8F\xE2\x83\xA3 \xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88 "
               "\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x94\xA5",
               &empty_r, &id_inline, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_post_rich_message(F.transport, ch, 1003, "\xF0\x9F\x94\xA5", &empty_r, &id_one, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_post_rich_message(F.transport, ch, 1001,
                                        "\xF0\x9F\x98\x80 \xF0\x9F\x9A\x80 \xF0\x9F\x8E\x89", &empty_r,
                                        &id_three, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_post_rich_message(
               F.transport, ch, 1002,
               "\xF0\x9F\x98\x80\xF0\x9F\x98\x82\xF0\x9F\x94\xA5\xF0\x9F\x92\x80"
               "\xF0\x9F\x98\xAD\xF0\x9F\x91\x8D\xF0\x9F\x99\x8F\xF0\x9F\x8E\x89",
               &empty_r, &id_eight, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_post_rich_message(
               F.transport, ch, 1003,
               "\xF0\x9F\x98\x80\xF0\x9F\x98\x82\xF0\x9F\x94\xA5\xF0\x9F\x92\x80"
               "\xF0\x9F\x98\xAD\xF0\x9F\x91\x8D\xF0\x9F\x99\x8F\xF0\x9F\x8E\x89\xF0\x9F\x9A\x80",
               &empty_r, &id_nine, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_post_rich_message(F.transport, ch, 1001,
                                        "\xF0\x9F\x94\xA5 <:party:987654322>", &empty_r,
                                        &id_mixed_custom, &err) == CCORD_OK);
  // Same author (1001) -> grouped/compact follow-up row with a single dark-skin-tone jumbo emoji.
  CT_CHECK(ccord_mock_post_rich_message(F.transport, ch, 1001,
                                        "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBF", &empty_r,
                                        &id_grouped, &err) == CCORD_OK);
  frame(6);

  const vis_row *r_inline = find_vis_row(id_inline);
  const vis_row *r_one = find_vis_row(id_one);
  const vis_row *r_three = find_vis_row(id_three);
  const vis_row *r_eight = find_vis_row(id_eight);
  const vis_row *r_nine = find_vis_row(id_nine);
  const vis_row *r_mixed = find_vis_row(id_mixed_custom);
  const vis_row *r_grouped = find_vis_row(id_grouped);

  CT_CHECK(r_inline && r_one && r_three && r_eight && r_nine && r_mixed && r_grouped);
  if (r_inline && r_one && r_three && r_eight && r_nine && r_mixed && r_grouped) {
    // 1, 3, 8, and mixed custom+unicode jumbo rows are all enlarged (+34 px taller than inline 1-line messages).
    CT_CHECK(r_one->h > r_inline->h + 28.0f);
    CT_CHECK(fabsf(r_one->h - r_three->h) < 0.5f);
    CT_CHECK(fabsf(r_one->h - r_eight->h) < 0.5f);
    CT_CHECK(fabsf(r_one->h - r_mixed->h) < 0.5f);
    // 9 emojis drop back to regular inline size, matching the normal inline text row height.
    CT_CHECK(fabsf(r_nine->h - r_inline->h) < 0.5f);
    // Grouped follow-up jumbo row has no header, yet is still enlarged (>= 54 px tall).
    CT_CHECK(r_grouped->h >= 54.0f && r_grouped->h < r_one->h);
  }

  // Render one frame and verify full-colour Twemoji pixels land in the enlarged jumbo row.
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  const char *env_shot = getenv("CCORD_JUMBO_SHOT");
  const char *shot_path = (env_shot && env_shot[0]) ? env_shot : "test_ui_jumbo_emoji_shot.png";
  CT_CHECK(ccord_render_screenshot(F.renderer, shot_path, &err) == CCORD_OK);
  ccord_render_end(F.renderer);

  SDL_Surface *surf = SDL_LoadPNG(shot_path);
  CT_CHECK(surf != nullptr);
  if (surf && r_one) {
    const ccord_layout *lay = ccord_ui_get_layout(F.ui);
    int x0 = (int)(lay->message_area.x + F.theme.m.message_text_x);
    int x1 = x0 + 60;
    int y0 = (int)r_one->y;
    int y1 = (int)(r_one->y + r_one->h);
    if (y0 < 0) y0 = 0;
    if (y1 > surf->h) y1 = surf->h;
    int warm_px = 0;   // orange/yellow Twemoji fire pixels in the 48x48 enlarged glyph
    for (int y = y0; y < y1; y++) {
      for (int x = x0; x < x1 && x < surf->w; x++) {
        uint8_t pr, pg, pb, pa;
        SDL_ReadSurfacePixel(surf, x, y, &pr, &pg, &pb, &pa);
        if (pr > 180 && pg > 80 && pb < 100) warm_px++;
      }
    }
    CT_CHECK(warm_px > 400);
    SDL_DestroySurface(surf);
  }
  if (!env_shot || !env_shot[0]) remove(shot_path);

  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_contrib_destroy(F.contrib);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
}

int main(int argc, char **argv)
{
  test_parse_jumbo_emojis();
  test_jumbo_ui_render(argc > 0 ? argv[0] : "test_ui_jumbo_emoji");
  return CT_RESULT("test_ui_jumbo_emoji");
}
