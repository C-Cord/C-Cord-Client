// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <ftw.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int ccord_test_unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
  (void)sb; (void)typeflag; (void)ftwbuf;
  return remove(fpath);
}
static char *ccord_test_data_dir = NULL;
static void ccord_test_cleanup_dir(void) {
  if (ccord_test_data_dir) {
    nftw(ccord_test_data_dir, ccord_test_unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
    free(ccord_test_data_dir);
    ccord_test_data_dir = NULL;
  }
}
[[maybe_unused]] static const char *ccord_test_init_dir(const char *prefix) {
  if (ccord_test_data_dir) return ccord_test_data_dir;
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !tmp[0]) tmp = "/tmp";
  size_t len = strlen(tmp) + strlen(prefix) + 16;
  ccord_test_data_dir = malloc(len);
  if (!ccord_test_data_dir) abort();
  snprintf(ccord_test_data_dir, len, "%s/%s_XXXXXX", tmp, prefix);
  if (!mkdtemp(ccord_test_data_dir)) {
    free(ccord_test_data_dir);
    ccord_test_data_dir = NULL;
    abort();
  }
  atexit(ccord_test_cleanup_dir);
  return ccord_test_data_dir;
}

// Offscreen UI test: real store + mock transport + session + renderer, fake OS host, injected input events.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/alloc.h>
#include <ccord/core/paths.h>
#include <ccord/model/audit.h>
#include <ccord/model/permissions.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/store/admin.h>
#include <ccord/store/expressions.h>
#include <ccord/store/roles.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "../ui_voice_surfaces.h"

#include "ccord_test.h"

typedef struct fixture {
  ccord_theme theme;
  ccord_bus *bus;
  ccord_store *store;
  ccord_session *session;
  ccord_transport *transport;    // the mock behind `session` (request counters)
  ccord_contrib *contrib;
  ccord_platform *platform;
  ccord_renderer *renderer;
  ccord_ui *ui;
  uint64_t now;
  char clip[4096];
  int clip_calls;
  bool text_input_active;
  int menu_activations;
  ccord_menu_context last_ctx;
  char last_owner[64], last_item[64];
  int ext_calls;
  bool ext_last;
  int open_url_calls;
  char last_url[512];
  char pick_path[512];
} fixture;

static fixture F;

static uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

static void host_clip_set(void *ud, const char *s)
{
  (void)ud;
  snprintf(F.clip, sizeof F.clip, "%s", s);
  F.clip_calls++;
}

static void host_text_input(void *ud, bool active, float x, float y, float w, float h)
{
  (void)ud, (void)x, (void)y, (void)w, (void)h;
  F.text_input_active = active;
}

static void host_set_extensions(void *ud, bool on)
{
  (void)ud;
  F.ext_calls++;
  F.ext_last = on;
}

static bool host_open_url(void *ud, const char *url)
{
  (void)ud;
  F.open_url_calls++;
  snprintf(F.last_url, sizeof F.last_url, "%s", url ? url : "");
  return true;
}

static bool host_open_file(void *ud, char *out_path, size_t cap)
{
  (void)ud;
  if (!F.pick_path[0] || !out_path || cap == 0) return false;
  snprintf(out_path, cap, "%s", F.pick_path);
  return true;
}

static void menu_activate(void *ud, const ccord_menu_contribution *item, const ccord_menu_context *ctx)
{
  (void)ud;
  F.menu_activations++;
  F.last_ctx = *ctx;
  snprintf(F.last_owner, sizeof F.last_owner, "%s", item->owner);
  snprintf(F.last_item, sizeof F.last_item, "%s", item->item_id);
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

// One draw without pumping the session: lets a test inspect the local echo before the mock's reply lands.
static void draw_only(void)
{
  F.now += 50;
  (void)ccord_ui_update(F.ui, F.now);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_render_end(F.renderer);
}

static void send(ccord_input_event ev)
{
  (void)ccord_ui_handle_input(F.ui, &ev);
}

static void mouse(ccord_input_type t, uint8_t button, float x, float y)
{
  send((ccord_input_event){.type = t, .button = button, .clicks = 1, .x = x, .y = y});
}

static void click(float x, float y)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  frame(2);
}

// Account playback/developer settings (M6). The UI starts from Discord's defaults when the store has no settings;
// these tests seed a full object so the gates can be flipped deterministically.
static void seed_user_settings(bool developer_mode, bool animate_emoji, uint8_t animate_stickers)
{
  ccord_error err = {0};
  ccord_user_settings settings = {0};
  settings.developer_mode = developer_mode;
  settings.animate_emoji = animate_emoji;
  settings.animate_stickers = animate_stickers;
  settings.gif_auto_play = true;
  settings.friend_source_all = true;
  settings.friend_source_mutual_friends = true;
  settings.friend_source_mutual_guilds = true;
  snprintf(settings.locale, sizeof settings.locale, "en-US");
  CT_CHECK(ccord_store_set_user_settings(F.store, &settings, &err) == CCORD_OK);
}

static void key(ccord_key k, uint8_t mods)
{
  send((ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = k, .mods = mods});
}

// A text input event carries a few bytes, like SDL's; longer strings arrive as several events, never split mid-character.
static void type_text(const char *s)
{
  size_t left = strlen(s);
  while (left > 0) {
    ccord_input_event ev = {.type = CCORD_IN_TEXT};
    size_t take = left < sizeof ev.text - 1u ? left : sizeof ev.text - 1u;
    while (take < left && take > 1 && ((uint8_t)s[take] & 0xC0u) == 0x80u) take--;
    memcpy(ev.text, s, take);
    send(ev);
    s += take;
    left -= take;
  }
}

static ccord_ui_debug dbg(void)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(F.ui, &d);
  return d;
}

// Opt-in screenshots for looking at a change (CCORD_SHOT_DIR=<dir>); a no-op otherwise.
static void shot(const char *name)
{
  const char *dir = getenv("CCORD_SHOT_DIR");
  if (!dir || !*dir) return;
  char path[512];
  snprintf(path, sizeof path, "%s/%s.png", dir, name);
  F.now += 50;
  (void)ccord_ui_update(F.ui, F.now);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_error serr;
  (void)ccord_render_screenshot(F.renderer, path, &serr);
  ccord_render_end(F.renderer);
}

// Moves the pointer over (x, y_start..y_end) until the predicate hits; returns y or -1.
static float scan_hover_channel(float x, ccord_id want)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  for (float y = l->channel_scroll.y + 2; y < l->channel_scroll.y + l->channel_scroll.h; y += 3.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
    if (dbg().hover_channel == want) return y;
  }
  return -1.0f;
}

static float scan_hover_message(float x)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  for (float y = l->message_area.y + 60; y < l->message_area.y + l->message_area.h; y += 3.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
    if (dbg().hover_message != CCORD_ID_NONE) return y;
  }
  return -1.0f;
}

static float menu_item_center_y(float pointer_y, size_t count, int index, int seps_before)
{
  const float pad = F.theme.m.context_menu_pad;
  const float ih = F.theme.m.context_menu_item_height;
  const float sep = 9.0f;
  float h = 2.0f * pad + (float)count * ih + 2.0f * sep;
  float my = pointer_y;
  if (my + h > 800.0f - 4.0f) my = 800.0f - h - 4.0f;
  if (my < 4.0f) my = 4.0f;
  return my + pad + (float)index * ih + (float)seps_before * sep + ih * 0.5f;
}

// Moves to and clicks the menu item with this label (a top-level item, or one in the fly-out that is showing).
static bool menu_click_label(const char *label)
{
  for (size_t i = 0; i < F.ui->menu.count; i++) {
    const menu_item *it = &F.ui->menu.items[i];
    if (strcmp(it->label, label) != 0) continue;
    if (it->parent >= 0 && it->parent != F.ui->menu.open_sub) continue;
    ccord_rect r = ui_menu_item_rect(F.ui, i);
    mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    frame(1);
    mouse(CCORD_IN_MOUSE_DOWN, 1, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    frame(1);
    return true;
  }
  return false;
}

static float scan_hover_self_message(float x)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ccord_id self = ccord_store_self_id(F.store);
  for (float y = l->message_area.y + 60; y < l->message_area.y + l->message_area.h; y += 3.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
    ccord_id id = dbg().hover_message;
    if (id == CCORD_ID_NONE) continue;
    const ccord_message *m = ccord_store_find_message(F.store, ccord_store_selected_channel(F.store), id);
    // M0-3: pending/failed local echoes are not editable/deletable yet; the action tests target confirmed rows.
    if (m && m->author_id == self && m->send_state == CCORD_MSG_SEND_CONFIRMED) return y;
  }
  return -1.0f;
}

static float scan_hover_other_message(float x)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ccord_id self = ccord_store_self_id(F.store);
  for (float y = l->message_area.y + 60; y < l->message_area.y + l->message_area.h; y += 3.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
    ccord_id id = dbg().hover_message;
    if (id == CCORD_ID_NONE) continue;
    const ccord_message *m = ccord_store_find_message(F.store, ccord_store_selected_channel(F.store), id);
    if (m && m->author_id != self) return y;
  }
  return -1.0f;
}

static ccord_id find_channel(ccord_id guild, const char *name, ccord_channel_kind kind)
{
  for (size_t i = 0; i < ccord_store_channel_count(F.store, guild); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, guild, i);
    if (c->kind == kind && strcmp(c->name, name) == 0) return c->id;
  }
  return CCORD_ID_NONE;
}

static bool pixel_is(SDL_Surface *s, int x, int y, ccord_color want, int tol)
{
  uint8_t r, g, b, a;
  if (!SDL_ReadSurfacePixel(s, x, y, &r, &g, &b, &a)) return false;
  return abs(r - want.r) <= tol && abs(g - want.g) <= tol && abs(b - want.b) <= tol;
}

static const char *ui_test_file(const char *rel)
{
  static char bufs[4][1024];
  static unsigned idx = 0;
  char *b = bufs[idx++ & 3u];
  snprintf(b, 1024, "%s/%s", ccord_test_init_dir("ui_test_data"), rel);
  return b;
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
  // Keep the settings INI and drafts JSON hermetic across test runs.
  ccord_paths_set_data_dir(ccord_test_init_dir("ui_test_data"));
  remove(ui_test_file("settings.ini"));
  remove(ui_test_file("drafts.json"));
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK) return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 3000, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  F.transport = tr;
  F.session = ccord_session_create(F.bus, F.store, tr, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session) return false;
  CT_CHECK(ccord_contrib_add_menu(F.contrib, "test.plugin", "copy-id", "Copy Message ID", CCORD_MENU_MESSAGE, &err) == CCORD_OK);
  CT_CHECK(ccord_contrib_add_menu(F.contrib, "test.plugin", "chan-item", "Channel Thing", CCORD_MENU_CHANNEL, &err) == CCORD_OK);
  ccord_contrib_set_handlers(F.contrib, &(ccord_contrib_handlers){.menu_activate = menu_activate});
  ccord_platform_config pc = {.title = "ui-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .host = {.clipboard_set = host_clip_set, .text_input = host_text_input, .set_extensions_enabled = host_set_extensions, .open_url = host_open_url, .open_file_dialog = host_open_file},
                     .fixed_now_unix_ms = 1793534400000LL + 30 * 60 * 1000};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  CT_CHECK(ccord_session_start(F.session, &err) == CCORD_OK);
  return true;
}

static void test_markdown_tokens(void)
{
  const char *src = "a **bold** *ital* `code` @you [Nitro](https://discord.com/nitro) https://discord.com/shop";
  ui_md_iter it;
  ui_md_iter_init(&it, src, strlen(src));
  ui_md_token tok;
  bool bold = false, ital = false, code = false, mention = false, md_link = false, bare_link = false;
  while (ui_md_next(&it, &tok)) {
    CT_CHECK(memchr(tok.text, '*', tok.len) == nullptr && memchr(tok.text, '`', tok.len) == nullptr);
    if (tok.len == 4 && memcmp(tok.text, "bold", 4) == 0) {
      bold = true;
      CT_CHECK(tok.style == UI_MD_BOLD);
    } else if (tok.len == 4 && memcmp(tok.text, "ital", 4) == 0) {
      ital = true;
      CT_CHECK(tok.style == UI_MD_ITALIC);
    } else if (tok.len == 4 && memcmp(tok.text, "code", 4) == 0) {
      code = true;
      CT_CHECK(tok.style == UI_MD_CODE);
    } else if (tok.len == 4 && memcmp(tok.text, "@you", 4) == 0) {
      mention = true;
      CT_CHECK(tok.style == UI_MD_MENTION);
    } else if (tok.len == 5 && memcmp(tok.text, "Nitro", 5) == 0) {
      md_link = true;
      CT_CHECK(tok.style == UI_MD_LINK);
      CT_CHECK(tok.url_len == strlen("https://discord.com/nitro") &&
               memcmp(tok.url, "https://discord.com/nitro", tok.url_len) == 0);
    } else if (tok.style == UI_MD_LINK && tok.len == strlen("https://discord.com/shop") &&
               memcmp(tok.text, "https://discord.com/shop", tok.len) == 0) {
      bare_link = true;
      CT_CHECK(tok.url_len == tok.len && memcmp(tok.url, tok.text, tok.len) == 0);
    }
  }
  CT_CHECK(bold && ital && code && mention && md_link && bare_link);

  // Discord inline tokens: <@id>, <@!id>, <#id>, <@&id>, <:name:id>, <a:name:id>, <t:ts:R>, <url>, ***bi***, __u__, ~~s~~, ||sp||
  const char *dsrc =
    "<@1002> <@!1003> <#3111> <@&777> <:blob:12345> <a:party:67890> <t:1700000000:R> <https://example.org> ***bi*** __und__ ~~str~~ ||spoil||";
  ui_md_iter_init(&it, dsrc, strlen(dsrc));
  bool u1 = false, u2 = false, ch = false, role = false, ce1 = false, ce2 = false, ts = false, alink = false;
  bool bi = false, und = false, str = false, spoil = false;
  while (ui_md_next(&it, &tok)) {
    if (tok.style == UI_MD_USER_MENTION && tok.id == 1002) u1 = true;
    else if (tok.style == UI_MD_USER_MENTION && tok.id == 1003) u2 = true;
    else if (tok.style == UI_MD_CHANNEL_MENTION && tok.id == 3111) ch = true;
    else if (tok.style == UI_MD_ROLE_MENTION && tok.id == 777) role = true;
    else if (tok.style == UI_MD_CUSTOM_EMOJI && tok.id == 12345 && !tok.animated && tok.len == 4 && memcmp(tok.text, "blob", 4) == 0) ce1 = true;
    else if (tok.style == UI_MD_CUSTOM_EMOJI && tok.id == 67890 && tok.animated && tok.len == 5 && memcmp(tok.text, "party", 5) == 0) ce2 = true;
    else if (tok.style == UI_MD_TIMESTAMP && tok.timestamp_sec == 1700000000 && tok.timestamp_style == 'R') ts = true;
    else if (tok.style == UI_MD_LINK && tok.url_len == strlen("https://example.org")) alink = true;
    else if (tok.style == UI_MD_BOLD_ITALIC && tok.len == 2 && memcmp(tok.text, "bi", 2) == 0) bi = true;
    else if (tok.underline && tok.len == 3 && memcmp(tok.text, "und", 3) == 0) und = true;
    else if (tok.strikethrough && tok.len == 3 && memcmp(tok.text, "str", 3) == 0) str = true;
    else if (tok.spoiler && tok.len == 5 && memcmp(tok.text, "spoil", 5) == 0) spoil = true;
  }
  CT_CHECK(u1 && u2 && ch && role && ce1 && ce2 && ts && alink && bi && und && str && spoil);
}

// Collects the drawn text of one line (all tokens joined) and whether any token carries a style.
typedef struct md_probe {
  char text[256];
  size_t n;
  bool bold, italic, underline, strike, spoiler, code, link;
} md_probe;

static md_probe md_probe_line(const char *src)
{
  md_probe pr = {0};
  ui_md_iter it;
  ui_md_iter_init(&it, src, strlen(src));
  ui_md_token tok;
  while (ui_md_next(&it, &tok)) {
    if (tok.style == UI_MD_LINK) pr.link = true;
    if (tok.style == UI_MD_BOLD || tok.style == UI_MD_BOLD_ITALIC) pr.bold = true;
    if (tok.style == UI_MD_ITALIC || tok.style == UI_MD_BOLD_ITALIC) pr.italic = true;
    if (tok.style == UI_MD_CODE) pr.code = true;
    if (tok.underline) pr.underline = true;
    if (tok.strikethrough) pr.strike = true;
    if (tok.spoiler) pr.spoiler = true;
    if (pr.n + tok.len < sizeof pr.text) {
      memcpy(pr.text + pr.n, tok.text, tok.len);
      pr.n += tok.len;
      pr.text[pr.n] = '\0';
    }
  }
  return pr;
}

// Posted messages pair markers like the composer: no closer means literal text, a backslash escapes, _x_ is italic.
static void test_markdown_pairing(void)
{
  md_probe a = md_probe_line("**abc");
  CT_CHECK(strcmp(a.text, "**abc") == 0 && !a.bold && !a.italic);
  a = md_probe_line("*abc");
  CT_CHECK(strcmp(a.text, "*abc") == 0 && !a.italic);
  a = md_probe_line("x __abc ~~d ||e `f");
  CT_CHECK(strcmp(a.text, "x __abc ~~d ||e `f") == 0 && !a.underline && !a.strike && !a.spoiler && !a.code);
  a = md_probe_line("\\*abc\\*");
  CT_CHECK(strcmp(a.text, "*abc*") == 0 && !a.italic);
  a = md_probe_line("\\\\ \\*a*");
  CT_CHECK(strcmp(a.text, "\\ *a*") == 0);
  CT_CHECK(!a.italic);
  a = md_probe_line("_abc_");
  CT_CHECK(strcmp(a.text, "abc") == 0 && a.italic);
  a = md_probe_line("snake_case_name");
  CT_CHECK(strcmp(a.text, "snake_case_name") == 0 && !a.italic);
  a = md_probe_line("see https://x.com/_a_/b__c__d and <:a_b:1> ok");
  CT_CHECK(!a.italic && !a.underline && a.link);
  a = md_probe_line("`_a_` __init__");
  CT_CHECK(a.code && !a.italic && a.underline);
  a = md_probe_line("**a** *b* __c__ ~~d~~ `e` ||f||");
  CT_CHECK(a.bold && a.italic && a.underline && a.strike && a.code && a.spoiler);
  CT_CHECK(strcmp(a.text, "a b c d e f") == 0);
  a = md_probe_line("**bold** and *open");
  CT_CHECK(a.bold && !a.italic && strcmp(a.text, "bold and *open") == 0);
}

static void test_load_and_virtualization(void)
{
  frame(1);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.message_count == CCORD_SESSION_HISTORY_LIMIT); // history limit of the session, out of 3000 mock messages
  CT_CHECK(d.stick_bottom);
  CT_CHECK(d.last_visible == d.message_count - 1);
  size_t visible = d.last_visible - d.first_visible + 1;
  CT_CHECK(visible >= 5 && visible < 40);
  CT_CHECK(d.rows_drawn == visible);
  CT_CHECK(F.text_input_active);                         // composer is focused at start
  frame(2);
  d = dbg();
  CT_CHECK(d.rows_measured == 0);                        // steady state: height cache is warm
}

static void test_wheel_scroll(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ccord_ui_debug before = dbg();
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 5.0f, .x = l->message_area.x + 100, .y = l->message_area.y + 100});
  frame(1);
  ccord_ui_debug after = dbg();
  CT_CHECK(after.first_visible < before.first_visible);   // positive wheel = older messages
  CT_CHECK(!after.stick_bottom);
  CT_CHECK(after.rows_measured <= (after.last_visible - after.first_visible + 1) + 8 + 20);
  // wheel far up then back down re-sticks to the bottom
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100, .y = l->message_area.y + 100});
  frame(1);
  CT_CHECK(dbg().stick_bottom);
  // wheel outside any scrollable area does nothing
  ccord_ui_debug same = dbg();
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 3.0f, .x = l->chat_header.x + 10, .y = l->chat_header.y + 10});
  frame(1);
  CT_CHECK(dbg().first_visible == same.first_visible);
  // Scrolling to the top pages older history in through before=<id>.
  ccord_id ch = ccord_store_selected_channel(F.store);
  size_t before_count = ccord_store_message_count(F.store, ch);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 2000.0f, .x = l->message_area.x + 100, .y = l->message_area.y + 100});
  frame(4);
  CT_CHECK(ccord_store_message_count(F.store, ch) > before_count);
}

static void test_channel_click(void)
{
  ccord_id guild = ccord_store_selected_guild(F.store);
  ccord_id general = ccord_store_selected_channel(F.store);
  ccord_id ann = find_channel(guild, "announcements", CCORD_CHANNEL_TEXT);
  CT_CHECK(ann != CCORD_ID_NONE && ann != general);
  float y = scan_hover_channel(200.0f, ann);
  CT_CHECK(y > 0);
  click(200.0f, y);
  CT_CHECK(ccord_store_selected_channel(F.store) == ann);
  frame(2);   // history arrives
  ccord_ui_debug d = dbg();
  CT_CHECK(d.message_count == ccord_store_message_count(F.store, ann));
  CT_CHECK(d.message_count > 0 && d.message_count <= CCORD_SESSION_HISTORY_LIMIT);
  CT_CHECK(d.stick_bottom && d.last_visible == d.message_count - 1);

  // voice channel: clicking joins voice and opens the stage without selecting a text channel
  ccord_id voice = find_channel(guild, "General", CCORD_CHANNEL_VOICE);
  CT_CHECK(voice != CCORD_ID_NONE);
  y = scan_hover_channel(200.0f, voice);
  CT_CHECK(y > 0);
  click(200.0f, y);
  frame(3);
  ccord_ui_debug vd = dbg();
  CT_CHECK(vd.voice_connected && vd.voice_channel == voice);
  CT_CHECK(vd.voice_stage_open);
  CT_CHECK(ccord_store_voice_count(F.store, voice) >= 2);   // seeded participant plus self
  CT_CHECK(ccord_store_selected_channel(F.store) == ann);
  // switch back to text: the stage closes, voice stays connected
  y = scan_hover_channel(200.0f, general);
  click(200.0f, y);
  CT_CHECK(!dbg().voice_stage_open);
  CT_CHECK(dbg().voice_connected);
  CT_CHECK(ccord_store_selected_channel(F.store) == general);
  frame(1);
  CT_CHECK(dbg().message_count == ccord_store_message_count(F.store, general));
}

static void test_server_click(void)
{
  ccord_id g1 = ccord_store_selected_guild(F.store);
  const ccord_guild *second = ccord_store_guild_at(F.store, 1);
  CT_CHECK(second && second->id != g1);
  ccord_rect dm_r = ui_guild_icon_rect(F.ui, 0);
  mouse(CCORD_IN_MOUSE_MOVE, 0, dm_r.x + dm_r.w * 0.5f, dm_r.y + dm_r.h * 0.5f);
  CT_CHECK(dbg().hover_guild == CCORD_DM_GUILD_ID);

  ccord_rect r2 = ui_guild_icon_rect(F.ui, 2);
  float x = r2.x + r2.w * 0.5f;
  float y = r2.y + r2.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  CT_CHECK(dbg().hover_guild == second->id);
  click(x, y);
  CT_CHECK(ccord_store_selected_guild(F.store) == second->id);
  frame(2);
  CT_CHECK(ccord_store_selected_channel(F.store) != CCORD_ID_NONE);
  CT_CHECK(dbg().message_count == ccord_store_message_count(F.store, ccord_store_selected_channel(F.store)));
  // back to the first guild (slot 1)
  ccord_rect r1 = ui_guild_icon_rect(F.ui, 1);
  click(r1.x + r1.w * 0.5f, r1.y + r1.h * 0.5f);
  CT_CHECK(ccord_store_selected_guild(F.store) == g1);
  frame(1);
}

static void test_dm_pseudo_guild(void)
{
  ccord_error err;
  ccord_user ada = {.id = 424242u, .name = "Ada", .avatar_url = "fe3ffc01cc5d93689dbd3f09462374e7", .presence = CCORD_PRESENCE_ONLINE};
  CT_CHECK(ccord_store_upsert_user(F.store, &ada, &err) == CCORD_OK);
  ccord_channel dm = {.id = 987654321u, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ada", .topic = "", .user_id = ada.id};
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm, &err) == CCORD_OK);
  frame(1);
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ccord_rect dm_r = ui_guild_icon_rect(F.ui, 0);
  float x = dm_r.x + dm_r.w * 0.5f;
  float y = dm_r.y + dm_r.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  CT_CHECK(dbg().hover_guild == CCORD_DM_GUILD_ID);
  click(x, y);
  CT_CHECK(ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID);
  CT_CHECK(ccord_store_selected_channel(F.store) == dm.id);
  CT_CHECK(dbg().relationship_rows >= 5);

  // Hover and click a relationship row in the DM channel column to open a DM conversation.
  float rx = l->channel_scroll.x + 80.0f;
  float ry = -1.0f;
  ccord_id rel_uid = CCORD_ID_NONE;
  for (float sy = l->channel_scroll.y + 40.0f; sy < l->channel_scroll.y + l->channel_scroll.h; sy += 3.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, rx, sy);
    if (dbg().hover_user != CCORD_ID_NONE) {
      ry = sy;
      rel_uid = dbg().hover_user;
      break;
    }
  }
  CT_CHECK(ry > 0.0f && rel_uid != CCORD_ID_NONE);
  // Right-click opens the DM relationship context menu (Message + relationship action).
  mouse(CCORD_IN_MOUSE_DOWN, 3, rx, ry);
  frame(1);
  CT_CHECK(dbg().context_menu_open && dbg().context_menu_items >= 2);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  // Left-click opens/selects the DM channel for that friend.
  click(rx, ry);
  frame(3);
  ccord_id opened_dm = 900000u + rel_uid;
  CT_CHECK(ccord_store_channel(F.store, opened_dm) != nullptr);
  CT_CHECK(ccord_store_selected_channel(F.store) == opened_dm);

  ccord_id g1 = ccord_store_guild_at(F.store, 0)->id;
  CT_CHECK(ccord_store_select_guild(F.store, g1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, dm.id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, opened_dm, &err) == CCORD_OK);
  frame(1);
}

static void save_stream_screenshot(const char *filename)
{
  const char *dir = getenv("CCORD_STREAM_SCREENSHOT_DIR");
  if (!dir || !dir[0]) return;
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dir, filename);
  ccord_error err = {0};
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  (void)ccord_render_screenshot(F.renderer, path, &err);
  ccord_render_end(F.renderer);
}

static void test_voice_and_stream(void)
{
  ccord_id guild = ccord_store_selected_guild(F.store);
  ccord_id voice = find_channel(guild, "General", CCORD_CHANNEL_VOICE);
  CT_CHECK(voice != CCORD_ID_NONE);
  // Joining from the channel list happened in test_channel_click; rejoin if a reset happened.
  ccord_ui_debug d = dbg();
  if (!d.voice_connected) {
    float y = scan_hover_channel(200.0f, voice);
    CT_CHECK(y > 0);
    click(200.0f, y);
    frame(3);
    d = dbg();
  }
  CT_CHECK(d.voice_connected && d.voice_channel == voice);
  CT_CHECK(d.voice_bar.h > 0.0f);
  CT_CHECK(d.mute_btn.w > 0.0f && d.deafen_btn.w > 0.0f);
  CT_CHECK(d.camera_btn.w > 0.0f && d.go_live_btn.w > 0.0f && d.disconnect_btn.w > 0.0f);
  CT_CHECK(ccord_store_user_speaking(F.store, ccord_store_self_id(F.store)));
  CT_CHECK(ccord_store_user_speaking(F.store, 1002u));
  save_stream_screenshot("voice_stage_speaking.png");

  // Mute / deafen buttons next to the settings gear drive the session voice state.
  click(d.mute_btn.x + d.mute_btn.w * 0.5f, d.mute_btn.y + d.mute_btn.h * 0.5f);
  frame(2);
  const ccord_voice_state *self = ccord_store_voice_user(F.store, guild, ccord_store_self_id(F.store));
  CT_CHECK(self && self->self_mute);
  CT_CHECK(!ccord_store_user_speaking(F.store, ccord_store_self_id(F.store)));
  click(d.mute_btn.x + d.mute_btn.w * 0.5f, d.mute_btn.y + d.mute_btn.h * 0.5f);
  frame(2);
  self = ccord_store_voice_user(F.store, guild, ccord_store_self_id(F.store));
  CT_CHECK(self && !self->self_mute);
  CT_CHECK(ccord_store_user_speaking(F.store, ccord_store_self_id(F.store)));
  click(d.deafen_btn.x + d.deafen_btn.w * 0.5f, d.deafen_btn.y + d.deafen_btn.h * 0.5f);
  frame(2);
  self = ccord_store_voice_user(F.store, guild, ccord_store_self_id(F.store));
  CT_CHECK(self && self->self_deaf);
  click(d.deafen_btn.x + d.deafen_btn.w * 0.5f, d.deafen_btn.y + d.deafen_btn.h * 0.5f);
  frame(2);

  // Camera is a local stage flag; screen share opens the Go Live source & quality modal.
  click(d.camera_btn.x + d.camera_btn.w * 0.5f, d.camera_btn.y + d.camera_btn.h * 0.5f);
  CT_CHECK(dbg().camera_on);
  frame(2);
  self = ccord_store_voice_user(F.store, guild, ccord_store_self_id(F.store));
  CT_CHECK(self && self->self_video);   // camera toggles travel through the session/transport
  click(d.go_live_btn.x + d.go_live_btn.w * 0.5f, d.go_live_btn.y + d.go_live_btn.h * 0.5f);
  frame(2);
  d = dbg();
  CT_CHECK(d.go_live_open);
  CT_CHECK(F.ui->go_live_source_count > 0);
  save_stream_screenshot("go_live_modal.png");
  CT_CHECK(d.go_live_option_count[0] == 5 && d.go_live_option_count[1] == 3 &&
           d.go_live_option_count[2] == 5 && d.go_live_option_count[3] == 4 && d.go_live_option_count[4] == 3);
  // Pick AMF / AV1 / Vulkan / 1440p / 15 FPS.
  ccord_rect picks[5] = {d.go_live_option[0][2], d.go_live_option[1][2], d.go_live_option[2][3],
                         d.go_live_option[3][2], d.go_live_option[4][0]};
  for (int i = 0; i < 5; i++) {
    click(picks[i].x + picks[i].w * 0.5f, picks[i].y + picks[i].h * 0.5f);
  }
  d = dbg();
  click(d.go_live_start.x + d.go_live_start.w * 0.5f, d.go_live_start.y + d.go_live_start.h * 0.5f);
  frame(4);
  d = dbg();
  CT_CHECK(d.streaming && !d.go_live_open);
  ccord_capture_source active_src;
  CT_CHECK(ccord_ui_active_capture_source(F.ui, &active_src) && active_src.title[0] != '\0');
  {
    ccord_error verr = {0};
    uint8_t thumb[320 * 180 * 4];
    int tw = 0, th = 0;
    if (ccord_capture_thumbnail(&active_src, 320, 180, thumb, &tw, &th, &verr) && tw > 0 && th > 0) {
      (void)ccord_render_upload_rgba(F.renderer, "__video_stream__", thumb, tw, th, &verr);
    }
  }
  frame(2);
  save_stream_screenshot("stream_stage_local.png");
  const ccord_stream_state *st = ccord_store_stream(F.store);
  CT_CHECK(st && st->active && st->channel_id == voice);
  CT_CHECK(st->config.encoder == CCORD_STREAM_ENCODER_AMF);
  CT_CHECK(st->config.codec == CCORD_STREAM_CODEC_AV1);
  CT_CHECK(st->config.framework == CCORD_STREAM_FRAMEWORK_VULKAN);
  CT_CHECK(st->config.resolution == CCORD_STREAM_RES_1440P);
  CT_CHECK(st->config.fps == CCORD_STREAM_FPS_15);

  // Reopen and stop the stream.
  click(d.go_live_btn.x + d.go_live_btn.w * 0.5f, d.go_live_btn.y + d.go_live_btn.h * 0.5f);
  frame(1);
  d = dbg();
  CT_CHECK(d.go_live_open);
  click(d.go_live_start.x + d.go_live_start.w * 0.5f, d.go_live_start.y + d.go_live_start.h * 0.5f);
  frame(4);
  CT_CHECK(!dbg().streaming);

  // Watch Ada's remote stream from her stage participant tile.
  CT_CHECK(F.ui->stage_watch_count >= 1);
  if (F.ui->stage_watch_count >= 1) {
    ccord_rect wbtn = F.ui->stage_watch_rects[0];
    click(wbtn.x + wbtn.w * 0.5f, wbtn.y + wbtn.h * 0.5f);
    frame(4);
    const ccord_watched_stream *ws = ccord_session_watched_stream(F.session);
    CT_CHECK(ws && ws->active && ws->user_id == 1002u && !ws->paused);
    save_stream_screenshot("stream_stage_watching.png");
    // Stop watching via the stage Stop Watching button.
    ccord_rect stop_wbtn = F.ui->stream_stage_btn2_rect;
    CT_CHECK(stop_wbtn.w > 0.0f && stop_wbtn.h > 0.0f);
    click(stop_wbtn.x + stop_wbtn.w * 0.5f, stop_wbtn.y + stop_wbtn.h * 0.5f);
    frame(4);
    ws = ccord_session_watched_stream(F.session);
    CT_CHECK(!ws || !ws->active);
  }

  // Clicking the voice channel reopens its stage, then a text channel closes it.
  float y = scan_hover_channel(200.0f, voice);
  CT_CHECK(y > 0);
  click(200.0f, y);
  frame(2);
  CT_CHECK(dbg().voice_stage_open);
  self = ccord_store_voice_user(F.store, guild, ccord_store_self_id(F.store));
  if (self && self->self_mute) {
    d = dbg();
    click(d.mute_btn.x + d.mute_btn.w * 0.5f, d.mute_btn.y + d.mute_btn.h * 0.5f);
    frame(2);
  }
  CT_CHECK(ccord_store_user_speaking(F.store, ccord_store_self_id(F.store)));
  CT_CHECK(ccord_store_user_speaking(F.store, 1002u));
  save_stream_screenshot("voice_stage_speaking.png");
  ccord_id text = find_channel(guild, "general", CCORD_CHANNEL_TEXT);
  if (text == CCORD_ID_NONE) text = ccord_store_selected_channel(F.store);
  y = scan_hover_channel(200.0f, text);
  CT_CHECK(y > 0);
  click(200.0f, y);
  frame(2);
  CT_CHECK(!dbg().voice_stage_open);

  // Disconnect clears the bar and the stage.
  d = dbg();
  click(d.disconnect_btn.x + d.disconnect_btn.w * 0.5f, d.disconnect_btn.y + d.disconnect_btn.h * 0.5f);
  frame(4);
  d = dbg();
  CT_CHECK(!d.voice_connected && !d.voice_stage_open && !d.camera_on);
  CT_CHECK(ccord_store_voice_user(F.store, guild, ccord_store_self_id(F.store)) == nullptr);
}

static void test_send(void)
{
  ccord_id ch = ccord_store_selected_channel(F.store);
  size_t before = ccord_store_message_count(F.store, ch);
  type_text("hello ");
  type_text("w\xC3\xB6rld");
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "hello w\xC3\xB6rld") == 0);
  frame(1);
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  // M0-3: a pending local echo appears at once (nonce set, not confirmed); the mock's answer replaces it in place.
  CT_CHECK(ccord_store_message_count(F.store, ch) == before + 1);
  const ccord_message *pend = ccord_store_message_at(F.store, ch, before);
  CT_CHECK(pend && pend->send_state == CCORD_MSG_SEND_PENDING && pend->nonce[0] != '\0' &&
           strcmp(pend->content, "hello w\xC3\xB6rld") == 0);
  frame(3);
  CT_CHECK(ccord_store_message_count(F.store, ch) == before + 1);
  const ccord_message *m = ccord_store_message_at(F.store, ch, before);
  CT_CHECK(m && strcmp(m->content, "hello w\xC3\xB6rld") == 0 && m->author_id == ccord_store_self_id(F.store));
  CT_CHECK(m && m->send_state == CCORD_MSG_SEND_CONFIRMED);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.message_count == before + 1 && d.stick_bottom && d.last_visible == before);
  CT_CHECK(d.acked_message == m->id);                    // newest message acknowledged while pinned to the bottom

  // sending while scrolled up jumps to the new message
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 30.0f, .x = l->message_area.x + 100, .y = l->message_area.y + 100});
  frame(1);
  CT_CHECK(!dbg().stick_bottom);
  type_text("second");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  CT_CHECK(dbg().stick_bottom && dbg().last_visible == before + 1);

  // Enter on blank text sends nothing; shift+Enter inserts a newline
  size_t count = ccord_store_message_count(F.store, ch);
  type_text("   ");
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(ccord_store_message_count(F.store, ch) == count);
  key(CCORD_KEY_BACKSPACE, 0);
  key(CCORD_KEY_BACKSPACE, 0);
  key(CCORD_KEY_BACKSPACE, 0);
  type_text("a");
  key(CCORD_KEY_RETURN, CCORD_MOD_SHIFT);
  type_text("b");
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "a\nb") == 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_C, CCORD_MOD_CTRL);
  CT_CHECK(strcmp(F.clip, "a\nb") == 0);
  key(CCORD_KEY_BACKSPACE, 0);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  frame(1);
}

// Number of context-menu application-command rows of one type the store currently holds for a guild.
// The fixture runs as an administrator, so every indexed command of the type passes the permission gate.
static size_t count_app_cmds(ccord_id guild, uint8_t command_type)
{
  size_t n = 0, cnt = 0;
  const ccord_slash_command *cmds = ccord_store_slash_commands(F.store, guild, &cnt);
  for (size_t i = 0; i < cnt; i++) if (cmds[i].command_type == command_type) n++;
  return n;
}

static void test_context_menu(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  // Load the application-command index up front so every menu open below includes the same rows.
  ccord_id guild0 = ccord_store_selected_guild(F.store);
  (void)ccord_session_ensure_slash_commands(F.session, guild0, ccord_store_selected_channel(F.store), F.now);
  frame(6);
  size_t msg_app_cmds = count_app_cmds(guild0, 3);
  float x = l->message_area.x + 300;
  float y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  ccord_id msg = dbg().hover_message;
  const ccord_message *mm_pre = ccord_store_find_message(F.store, ccord_store_selected_channel(F.store), msg);
  size_t own_react_item = (mm_pre && mm_pre->reaction_kind_count > 0) ? 1u : 0u;
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(2);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.context_menu_open && d.context_menu_items == 12u + own_react_item + msg_app_cmds);   // Copy, React, Edit, Delete, Reply, Forward, Copy Link, Pin, Mark Unread, Save, Create Thread (+ View Reactions) (+ MESSAGE app commands) + plugin item
  // Esc closes
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!dbg().context_menu_open);
  // reopen and click the plugin item (last row, after the built-ins and one separator)
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(2);
  CT_CHECK(dbg().context_menu_open);
  CT_CHECK(menu_click_label("Copy Message ID"));
  frame(1);
  CT_CHECK(!dbg().context_menu_open);
  CT_CHECK(F.menu_activations == 1);
  CT_CHECK(strcmp(F.last_owner, "test.plugin") == 0 && strcmp(F.last_item, "copy-id") == 0);
  CT_CHECK(F.last_ctx.target == CCORD_MENU_MESSAGE && F.last_ctx.message_id == msg && F.last_ctx.channel_id == ccord_store_selected_channel(F.store));
  const ccord_message *mm = ccord_store_find_message(F.store, F.last_ctx.channel_id, msg);
  CT_CHECK(mm && F.last_ctx.user_id == mm->author_id);

  // built-in Copy Text puts the message text on the clipboard
  F.clip[0] = '\0';
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(menu_click_label("Copy Text"));
  frame(1);
  CT_CHECK(mm && strcmp(F.clip, mm->content) == 0);

  // clicking outside closes without activating
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  int acts = F.menu_activations;
  mouse(CCORD_IN_MOUSE_DOWN, 1, 5.0f, 5.0f);
  frame(1);
  CT_CHECK(!dbg().context_menu_open && F.menu_activations == acts);

  // Someone else's message has no Edit: Copy, React, Reply, Pin, the plugin item, Report Message, and Delete because you administer
  // this server (the mock gives you the Admins role here, so Manage Messages comes from the permission engine).
  float oy = scan_hover_other_message(x);
  if (oy > 0) {
    const ccord_message *om = ccord_store_find_message(F.store, ccord_store_selected_channel(F.store), dbg().hover_message);
    size_t oth_react_item = (om && om->reaction_kind_count > 0) ? 1u : 0u;
    mouse(CCORD_IN_MOUSE_DOWN, 3, x, oy);
    frame(1);
    CT_CHECK(dbg().context_menu_open && dbg().context_menu_items == 12u + oth_react_item + msg_app_cmds);
    key(CCORD_KEY_ESCAPE, 0);
  }

  // a channel menu: Mute and Notification Settings, the plugin item plus the four channel-management items an administrator gets; a member menu opens
  // too, because an administrator can moderate members (a plain member would get no menu: see the gating test)
  ccord_id guild = ccord_store_selected_guild(F.store);
  ccord_id ch = find_channel(guild, "dev", CCORD_CHANNEL_TEXT);
  float cy2 = scan_hover_channel(200.0f, ch);
  CT_CHECK(cy2 > 0);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, cy2);
  frame(1);
  CT_CHECK(dbg().context_menu_open && dbg().context_menu_items == 8);
  key(CCORD_KEY_ESCAPE, 0);
  if (l->member_list_visible) {
    mouse(CCORD_IN_MOUSE_DOWN, 3, l->member_list.x + 60, l->member_list.y + 60);
    frame(1);
    CT_CHECK(dbg().context_menu_open);
    key(CCORD_KEY_ESCAPE, 0);
    frame(1);
    CT_CHECK(!dbg().context_menu_open);
  }
}

// A menu taller than the window scrolls instead of losing its bottom rows, and clicks follow the scrolled rows.
static void test_context_menu_scroll(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float x = l->message_area.x + 300.0f;
  float y = scan_hover_self_message(x);
  CT_CHECK(y > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(F.ui->menu.open);
  ccord_menu_context ctx = F.ui->menu.ctx;
  ui_menu_close(F.ui);

  // Open at the bottom edge: the panel must flip up and stay inside [8, win_h - 8].
  ui_menu_open(F.ui, 300.0f, 780.0f, &ctx);
  CT_CHECK(F.ui->menu.open);
  // Fill it past the window height (each row is 32 px; the window is 800 px).
  while (F.ui->menu.count < 32 && F.ui->menu.count < UI_MENU_MAX_ITEMS) {
    menu_item *it = &F.ui->menu.items[F.ui->menu.count++];
    *it = (menu_item){.parent = -1};
    it->label = ccord_strdup("Scrollable row");
    it->owner = ccord_strdup("test.plugin");
    it->item_id = ccord_strdup("copy-id");
  }
  frame(1);
  ui_menu *m = &F.ui->menu;
  CT_CHECK(m->content_h > m->h);
  CT_CHECK(m->h <= 800.0f - 16.0f + 0.01f);
  CT_CHECK(m->y >= 8.0f - 0.01f && m->y + m->h <= 800.0f - 8.0f + 0.01f);
  CT_CHECK(m->scroll_y == 0.0f);
  CT_CHECK(m->scrollbar_rect.w == 4.0f && m->scrollbar_rect.h > 0.0f);

  // Wheel over the panel scrolls to the last row; the thumb tracks the visible window.
  float mx = m->x + m->w * 0.5f, my = m->y + m->h * 0.5f;
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -200.0f, .x = mx, .y = my});
  frame(1);
  CT_CHECK(m->scroll_y > 0.0f);
  CT_CHECK(fabsf(m->scroll_y - (m->content_h - m->h)) < 0.01f);
  CT_CHECK(m->scrollbar_rect.x >= m->x && m->scrollbar_rect.x + m->scrollbar_rect.w <= m->x + m->w + 0.01f);
  CT_CHECK(m->scrollbar_rect.y >= m->y && m->scrollbar_rect.y + m->scrollbar_rect.h <= m->y + m->h + 0.01f);

  // The scrolled bottom row owns its on-screen rect and dispatches its plugin action.
  size_t last = m->count - 1;
  ccord_rect r = ui_menu_item_rect(F.ui, last);
  CT_CHECK(r.y >= m->y - 0.01f && r.y + r.h <= m->y + m->h + 0.01f);
  int acts = F.menu_activations;
  mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  frame(1);
  mouse(CCORD_IN_MOUSE_DOWN, 1, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  frame(1);
  CT_CHECK(!m->open);
  CT_CHECK(F.menu_activations == acts + 1);
}

static int64_t self_typing_expiry(ccord_id ch)
{
  ccord_id self = ccord_store_self_id(F.store);
  size_t n = ccord_store_typing_count(F.store, ch);
  for (size_t i = 0; i < n; i++) {
    if (ccord_store_typing_user_at(F.store, ch, i) == self) return ccord_store_typing_expires_at(F.store, ch, i);
  }
  return -1;
}

static void test_typing_throttle(void)
{
  ccord_id ch = ccord_store_selected_channel(F.store);
  F.now += 8000;                                 // leave the window opened by test_send's keystrokes
  frame(1);
  type_text("q");
  frame(1);
  size_t n = ccord_store_typing_count(F.store, ch);
  int64_t e0 = self_typing_expiry(ch);
  CT_CHECK(n >= 1 && e0 > 0);                    // first keystroke sent a typing notice
  CT_CHECK(dbg().typing_count == n);
  type_text("t");
  frame(1);
  CT_CHECK(ccord_store_typing_count(F.store, ch) == n);
  CT_CHECK(self_typing_expiry(ch) == e0);        // second keystroke inside the throttle window is dropped
  F.now += 8000;
  frame(1);
  type_text("y");
  frame(1);
  CT_CHECK(self_typing_expiry(ch) > e0);         // a second notice goes out after the window
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
}

static void test_message_actions(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float x = l->message_area.x + 300.0f;
  float y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  ccord_id ch = ccord_store_selected_channel(F.store);
  ccord_id msg = dbg().hover_message;

  // Add Reaction opens the emoji picker for this message; Escape dismisses it and sends nothing
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(menu_click_label("Add Reaction"));
  frame(2);
  CT_CHECK(F.ui->emoji_picker_open && F.ui->emoji_picker_msg_id == msg);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!F.ui->emoji_picker_open);
  const ccord_message *m = ccord_store_find_message(F.store, ch, msg);
  CT_CHECK(m && m->reaction_count == 0 && !m->self_reacted);

  // Edit Message loads the body into the composer, Enter saves it
  y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  msg = dbg().hover_message;
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(menu_click_label("Edit Message"));
  frame(1);
  m = ccord_store_find_message(F.store, ch, msg);
  CT_CHECK(m && strcmp(ccord_ui_composer_text(F.ui), m->content) == 0);
  CT_CHECK(dbg().composer_editing);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("edited body");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  m = ccord_store_find_message(F.store, ch, msg);
  CT_CHECK(m && strcmp(m->content, "edited body") == 0 && m->edited);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  CT_CHECK(!dbg().composer_editing);

  // Delete Message removes it through the transport echo
  y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  msg = dbg().hover_message;
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(menu_click_label("Delete Message"));
  frame(3);
  CT_CHECK(ccord_store_find_message(F.store, ch, msg) == nullptr);
}

// Appends a message with structure at the end of the open channel, scrolls to it and draws it.
static ccord_id append_rich_message(ccord_id ch, const char *content, const ccord_message_rich *rich)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  size_t n = ccord_store_message_count(F.store, ch);
  const ccord_message *last = n ? ccord_store_message_at(F.store, ch, n - 1) : nullptr;
  ccord_message m = {.id = last ? last->id + 1 : 1, .channel_id = ch, .author_id = ccord_store_self_id(F.store),
                     .timestamp_ms = last ? last->timestamp_ms + 1 : (int64_t)F.now, .content = content, .rich = rich};
  ccord_error err;
  CT_CHECK(ccord_store_append_message(F.store, &m, &err) == CCORD_OK);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(3);
  return m.id;
}

static void remove_message(ccord_id ch, ccord_id id)
{
  ccord_error err;
  CT_CHECK(ccord_store_delete_message(F.store, ch, id, &err) == CCORD_OK);
  frame(2);
}

// What each kind of structured message draws, how a system message reads, and what the search filters see.
static void test_rich_message_rendering(void)
{
  ccord_id ch = ccord_store_selected_channel(F.store);
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ccord_error err;
  char text[256];

  // Reply cards already on screen (the reply sent earlier in this test) are the baseline for the reply checks.
  ccord_id plain = append_rich_message(ch, "plain", nullptr);
  size_t base_replies = dbg().reply_cards;
  remove_message(ch, plain);

  // A forward: a "Forwarded" card, then the forwarded files and embeds as ordinary cards.
  ccord_attachment fatt = {.id = 1, .filename = "orig.png", .content_type = "image/png", .url = "https://cdn.discordapp.com/attachments/1/2/orig.png"};
  ccord_embed femb = {.title = "Original embed"};
  ccord_message_rich fwd = {.present = CCORD_RICH_ALL, .reference = {.kind = CCORD_REF_FORWARD, .message_id = 5},
                            .forward = {.present = true, .content = "the original words", .attachments = &fatt, .attachment_count = 1,
                                        .embeds = &femb, .embed_count = 1}};
  ccord_id id = append_rich_message(ch, "", &fwd);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.forward_cards == 1 && d.attachment_cards == 1 && d.embed_cards == 1 && d.reply_cards == base_replies);
  remove_message(ch, id);

  // With attachment/embed previews off, only the forward header remains.
  F.ui->settings_attachments = false;
  F.ui->settings_embeds = false;
  id = append_rich_message(ch, "", &fwd);
  d = dbg();
  CT_CHECK(d.forward_cards == 1 && d.attachment_cards == 0 && d.embed_cards == 0 && d.reply_cards == base_replies);
  remove_message(ch, id);
  F.ui->settings_attachments = true;
  F.ui->settings_embeds = true;

  // A poll draws one card; a sticker draws one sticker.
  ccord_message_rich poll = {.present = CCORD_RICH_ALL, .poll = {.present = true, .question = "Pizza?", .answer_count = 2}};
  poll.poll.answers[0] = (ccord_poll_answer){.id = 1, .text = "Yes", .votes = 3, .me_voted = true};
  poll.poll.answers[1] = (ccord_poll_answer){.id = 2, .text = "No", .votes = 1};
  id = append_rich_message(ch, "", &poll);
  d = dbg();
  CT_CHECK(d.poll_cards == 1 && d.sticker_cards == 0);
  remove_message(ch, id);
  ccord_msg_sticker stk = {.id = 8001, .name = "WumpusHi", .format = 1};
  ccord_message_rich sticker = {.present = CCORD_RICH_ALL, .stickers = &stk, .sticker_count = 1};
  id = append_rich_message(ch, "", &sticker);
  d = dbg();
  CT_CHECK(d.sticker_cards == 1 && d.poll_cards == 0);
  remove_message(ch, id);

  // A reply whose original was deleted still draws its card.
  ccord_message_rich gone = {.present = CCORD_RICH_ALL, .type = 19, .reference = {.kind = CCORD_REF_REPLY, .message_id = 999999},
                             .ref_preview = {.deleted = true}};
  id = append_rich_message(ch, "answer", &gone);
  d = dbg();
  CT_CHECK(d.reply_cards == base_replies + 1);
  remove_message(ch, id);

  // An unknown reply target with no preview draws no card (nothing to show).
  ccord_message_rich unknown = {.present = CCORD_RICH_ALL, .type = 19, .reference = {.kind = CCORD_REF_REPLY, .message_id = 999999}};
  id = append_rich_message(ch, "answer", &unknown);
  CT_CHECK(dbg().reply_cards == base_replies);
  remove_message(ch, id);

  // System messages read "<name> <what happened>"; text-carrying ones add it; unknown ones say so.
  const ccord_user *self = ccord_store_user(F.store, ccord_store_self_id(F.store));
  CT_CHECK(self != nullptr);
  ccord_message_rich join = {.present = CCORD_RICH_ALL, .type = 7};
  id = append_rich_message(ch, "", &join);
  const ccord_message *jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0);
  char want[256];
  snprintf(want, sizeof want, "%s joined the server.", self ? self->name : "Someone");
  CT_CHECK(strcmp(text, want) == 0);
  remove_message(ch, id);
  ccord_message_rich thread = {.present = CCORD_RICH_ALL, .type = 18};
  id = append_rich_message(ch, "planning", &thread);
  jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0);
  snprintf(want, sizeof want, "%s started a thread: planning", self ? self->name : "Someone");
  CT_CHECK(strcmp(text, want) == 0);
  remove_message(ch, id);
  ccord_message_rich odd = {.present = CCORD_RICH_ALL, .type = 46};
  id = append_rich_message(ch, "", &odd);
  jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0);
  snprintf(want, sizeof want, "%s sent a system message.", self ? self->name : "Someone");
  CT_CHECK(strcmp(text, want) == 0);
  remove_message(ch, id);
  ccord_message_rich odd_text = {.present = CCORD_RICH_ALL, .type = 46};   // an unknown type that has text shows the text
  id = append_rich_message(ch, "poll results here", &odd_text);
  jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0 && strcmp(text, "poll results here") == 0);
  remove_message(ch, id);
  // Only the types that name something append it (a channel rename, a thread title); an empty name adds nothing at all,
  // and the rest never show their text next to the wording.
  ccord_message_rich renamed = {.present = CCORD_RICH_ALL, .type = 4};
  id = append_rich_message(ch, "ideas", &renamed);
  jm = ccord_store_find_message(F.store, ch, id);
  snprintf(want, sizeof want, "%s changed the channel name: ideas", self ? self->name : "Someone");
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0 && strcmp(text, want) == 0);
  remove_message(ch, id);
  id = append_rich_message(ch, "", &renamed);
  jm = ccord_store_find_message(F.store, ch, id);
  snprintf(want, sizeof want, "%s changed the channel name:", self ? self->name : "Someone");
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0 && strcmp(text, want) == 0);
  remove_message(ch, id);
  id = append_rich_message(ch, "", &thread);
  jm = ccord_store_find_message(F.store, ch, id);
  snprintf(want, sizeof want, "%s started a thread:", self ? self->name : "Someone");
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0 && strcmp(text, want) == 0);
  remove_message(ch, id);
  ccord_message_rich pinned = {.present = CCORD_RICH_ALL, .type = 6};
  id = append_rich_message(ch, "stray text", &pinned);
  jm = ccord_store_find_message(F.store, ch, id);
  snprintf(want, sizeof want, "%s pinned a message to this channel.", self ? self->name : "Someone");
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0 && strcmp(text, want) == 0);
  remove_message(ch, id);
  // A message with nothing to say in words (a sticker, a file) is not a system message: it has no text.
  ccord_message_rich wordless = {.present = CCORD_RICH_ALL, .type = 0};
  id = append_rich_message(ch, "", &wordless);
  jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) == 0 && text[0] == '\0');
  remove_message(ch, id);
  ccord_message_rich silent_reply = {.present = CCORD_RICH_ALL, .type = 19};
  id = append_rich_message(ch, "", &silent_reply);
  jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) == 0 && text[0] == '\0');
  remove_message(ch, id);
  // An ordinary message is its text and nothing else; the copied text never has a system line or an edited tag.
  ccord_message_rich reply = {.present = CCORD_RICH_ALL, .type = 19};
  id = append_rich_message(ch, "just text", &reply);
  jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) == 9 && strcmp(text, "just text") == 0);
  CT_CHECK(jm && ui_message_clean_body(F.ui, jm, text, sizeof text) == 9 && strcmp(text, "just text") == 0);
  char tiny[5];
  CT_CHECK(jm && ui_message_clean_body(F.ui, jm, tiny, sizeof tiny) == 4 && strcmp(tiny, "just") == 0);
  CT_CHECK(ui_message_display_body(F.ui, nullptr, text, sizeof text) == 0 && ui_message_clean_body(F.ui, nullptr, text, sizeof text) == 0);
  remove_message(ch, id);

  // Search filters read the structure: has:file, has:image, has:video, has:sound, has:sticker, has:embed, has:link.
  ccord_attachment a_img = {.id = 1, .filename = "p.png", .content_type = "image/png", .url = "https://cdn.discordapp.com/a/p.png"};
  ccord_attachment a_vid = {.id = 2, .filename = "v.mp4", .content_type = "video/mp4", .url = "https://cdn.discordapp.com/a/v.mp4"};
  ccord_attachment a_snd = {.id = 3, .filename = "s.ogg", .content_type = "audio/ogg", .url = "https://cdn.discordapp.com/a/s.ogg"};
  ccord_attachment a_doc = {.id = 4, .filename = "d.pdf", .content_type = "application/pdf", .url = "https://cdn.discordapp.com/a/d.pdf"};
  ccord_embed e_pic = {.title = "pic", .thumbnail = {.url = "https://e.example/t.png"}};
  ccord_embed e_link = {.title = "link", .url = "https://e.example/page"};
  ccord_embed e_vid = {.title = "vid", .video = {.url = "https://e.example/v.mp4"}};
  ccord_message_rich r_img = {.present = CCORD_RICH_ALL, .attachments = &a_img, .attachment_count = 1};
  ccord_message_rich r_vid = {.present = CCORD_RICH_ALL, .attachments = &a_vid, .attachment_count = 1};
  ccord_message_rich r_snd = {.present = CCORD_RICH_ALL, .attachments = &a_snd, .attachment_count = 1};
  ccord_message_rich r_doc = {.present = CCORD_RICH_ALL, .attachments = &a_doc, .attachment_count = 1};
  ccord_message_rich r_epic = {.present = CCORD_RICH_ALL, .embeds = &e_pic, .embed_count = 1};
  ccord_message_rich r_elink = {.present = CCORD_RICH_ALL, .embeds = &e_link, .embed_count = 1};
  ccord_message_rich r_evid = {.present = CCORD_RICH_ALL, .embeds = &e_vid, .embed_count = 1};
  ccord_id ids[9];
  ids[0] = append_rich_message(ch, "", &r_img);
  ids[1] = append_rich_message(ch, "", &r_vid);
  ids[2] = append_rich_message(ch, "", &r_snd);
  ids[3] = append_rich_message(ch, "", &r_doc);
  ids[4] = append_rich_message(ch, "", &r_epic);
  ids[5] = append_rich_message(ch, "", &r_elink);
  ids[6] = append_rich_message(ch, "", &r_evid);
  ids[7] = append_rich_message(ch, "", &sticker);
  ids[8] = append_rich_message(ch, "", nullptr);   // nothing at all
  d = dbg();
  click(d.search_btn.x + d.search_btn.w * 0.5f, d.search_btn.y + d.search_btn.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().search_open);
  struct { const char *query; size_t matches; } filters[] = {
    {"has:file", 4},      // the four attachments
    {"has:image", 2},     // an image file, and an embed with a picture
    {"has:video", 2},     // a video file, and an embed with a video
    {"has:sound", 1},
    {"has:sticker", 1},
    {"has:embed", 3},
    {"has:link", 1},      // only the embed with a link (the earlier channel text has plain links too: counted below)
  };
  size_t base_links = 0;
  for (size_t i = 0; i < ccord_store_message_count(F.store, ch); i++) {
    const ccord_message *cm = ccord_store_message_at(F.store, ch, i);
    bool has_http = cm && cm->content && (strstr(cm->content, "http://") || strstr(cm->content, "https://"));
    if (has_http) base_links++;
  }
  for (size_t i = 0; i < sizeof filters / sizeof filters[0]; i++) {
    key(CCORD_KEY_A, CCORD_MOD_CTRL);
    key(CCORD_KEY_BACKSPACE, 0);
    type_text(filters[i].query);
    frame(3);
    size_t want_n = filters[i].matches + (strcmp(filters[i].query, "has:link") == 0 ? base_links : 0u);
    if (dbg().search_matches != want_n) fprintf(stderr, "filter %s: got %zu want %zu\n", filters[i].query, dbg().search_matches, want_n);
    CT_CHECK(dbg().search_matches == want_n);
  }
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!dbg().search_open);
  for (size_t i = 0; i < 9; i++) CT_CHECK(ccord_store_delete_message(F.store, ch, ids[i], &err) == CCORD_OK);
  (void)l;
  frame(2);
}

static void test_reply_pin_search_cards(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float x = l->message_area.x + 300.0f;
  float y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  ccord_id ch = ccord_store_selected_channel(F.store);
  ccord_id target = dbg().hover_message;

  // Reply arms the composer banner.
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(menu_click_label("Reply"));
  frame(1);
  CT_CHECK(dbg().replying);
  type_text("reply body");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(!dbg().replying);
  size_t n = ccord_store_message_count(F.store, ch);
  const ccord_message *sent = ccord_store_message_at(F.store, ch, n - 1);
  CT_CHECK(sent && sent->reply_to_id == target && strcmp(sent->content, "reply body") == 0);

  // Pin toggles the message's pinned flag through the session.
  y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  target = dbg().hover_message;
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(menu_click_label("Pin Message"));
  frame(3);
  const ccord_message *pinned = ccord_store_find_message(F.store, ch, target);
  CT_CHECK(pinned && pinned->pinned);
  // The header Pinned Messages popover lists it and closes on Esc.
  ccord_ui_debug d = dbg();
  click(d.pin_btn.x + d.pin_btn.w * 0.5f, d.pin_btn.y + d.pin_btn.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().pins_open);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!dbg().pins_open);

  // Search keeps the full transcript in the timeline; matches feed the results panel instead.
  size_t total = ccord_store_message_count(F.store, ch);
  d = dbg();
  click(d.search_btn.x + d.search_btn.w * 0.5f, d.search_btn.y + d.search_btn.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().search_open);
  type_text("supercalifragilistic");
  frame(3);
  d = dbg();
  CT_CHECK(d.search_matches > 0 && d.search_matches < total);
  CT_CHECK(d.message_count == total);   // the timeline is not filtered by search
  CT_CHECK(F.ui->view_count == total);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(3);
  CT_CHECK(dbg().message_count == total);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!dbg().search_open);

  // Rich cards: reply bar, attachment card and accented embed card.
  const ccord_message *last = ccord_store_message_at(F.store, ch, ccord_store_message_count(F.store, ch) - 1);
  CT_CHECK(last != nullptr);
  ccord_attachment card_att = {.id = 1, .filename = "shot.png", .content_type = "image/png",
                               .url = "https://cdn.discordapp.com/attachments/1/2/shot.png", .size = 2048};
  ccord_embed card_emb = {.title = "Release", .description = "Notes for v1"};
  ccord_message_rich card_rich = {
    .present = CCORD_RICH_ALL, .type = 19,
    .reference = {.kind = CCORD_REF_REPLY, .message_id = 999999},   // not in the store: the card comes from the preview
    .ref_preview = {.present = true, .author_name = "Ada", .content = "original note"},
    .attachments = &card_att, .attachment_count = 1, .embeds = &card_emb, .embed_count = 1,
  };
  ccord_message card = {.id = last->id + 1, .channel_id = ch, .author_id = ccord_store_self_id(F.store),
                        .timestamp_ms = last->timestamp_ms + 1, .content = "Release ready", .rich = &card_rich};
  ccord_error err;
  CT_CHECK(ccord_store_append_message(F.store, &card, &err) == CCORD_OK);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(3);
  d = dbg();
  CT_CHECK(d.reply_cards >= 1 && d.attachment_cards >= 1 && d.embed_cards >= 1);
  CT_CHECK(ccord_store_delete_message(F.store, ch, card.id, &err) == CCORD_OK);
  frame(2);
  test_rich_message_rendering();
}

// Request budget: what the UI asks Discord for while the user types a search or reads a busy channel.
static void type_slowly(const char *s)
{
  for (const char *p = s; *p; p++) {
    char one[2] = {*p, '\0'};
    type_text(one);
    frame(1);   // 50 ms between keystrokes
  }
}

static void test_request_budget_search_and_ack(void)
{
  ccord_id ch = ccord_store_selected_channel(F.store);
  CT_CHECK(ch != CCORD_ID_NONE);

  // --- search: one request when the user pauses, not one per keystroke ---
  ccord_ui_debug d = dbg();
  frame(10);
  size_t base = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH);
  click(d.search_btn.x + d.search_btn.w * 0.5f, d.search_btn.y + d.search_btn.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().search_open);
  type_slowly("hello world");                       // 11 keystrokes, 50 ms apart
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base);   // still typing: nothing sent
  frame(12);                                        // the user pauses (600 ms)
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base + 1);
  key(CCORD_KEY_RETURN, 0);                         // Enter right after it went out does not repeat it
  frame(2);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base + 1);

  // A changed query typed fast and confirmed with Enter goes out at once, and the pending debounce is cancelled.
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  type_slowly("hello");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base + 2);
  frame(12);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base + 2);

  // A single character is too short to search for, and closing the box with a query pending sends nothing.
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  type_slowly("a");
  frame(12);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base + 2);
  type_slowly("b");                                 // "ab" is pending ...
  key(CCORD_KEY_ESCAPE, 0);                         // ... but the user closes the box
  CT_CHECK(!dbg().search_open);
  frame(12);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base + 2);

  // --- read ack: a burst of incoming messages is acknowledged once per gap, newest message last ---
  frame(60);                                        // 3 s: the channel is settled and its ack gap has passed
  size_t acks = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK);
  ccord_error ack_err;
  ccord_message_rich r_burst = {.present = CCORD_RICH_ALL};
  for (int i = 0; i < 6; i++) {
    ccord_id bmid = 0;
    CT_CHECK(ccord_mock_post_rich_message(F.transport, ch, 1002, "burst", &r_burst, &bmid, &ack_err) == CCORD_OK);
    frame(1);
  }
  size_t n = ccord_store_message_count(F.store, ch);
  const ccord_message *newest = ccord_store_message_at(F.store, ch, n - 1);
  CT_CHECK(newest != nullptr);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK) - acks <= 1);   // the burst is not acked 6 times
  frame(60);                                        // 3 s later the newest message is acknowledged
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK) - acks == 2);
  CT_CHECK(dbg().acked_message == newest->id);
  frame(60);                                        // and then nothing more, however long the user stays
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK) - acks == 2);
}

static void test_request_budget_typing_and_channel_switch(void)
{
  ccord_error err;
  ccord_id g = ccord_store_selected_guild(F.store);
  ccord_id home = ccord_store_selected_channel(F.store);

  // --- typing: one notice per throttle window, however many keys are pressed ---
  frame(200);                                       // 10 s: any earlier typing notice has expired
  size_t t0 = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_TYPING);
  type_slowly("a long message that takes a while");   // 34 keys over ~1.7 s
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_TYPING) - t0 == 1);
  frame(160);                                       // 8 s later the notice is refreshed, once
  type_slowly("more");
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_TYPING) - t0 == 2);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);

  // --- channel switching: each channel's history is requested at most once, however often the user flips ---
  size_t h0 = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_HISTORY);
  ccord_id text[8];
  size_t ntext = 0;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, g) && ntext < 8; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, g, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT) text[ntext++] = c->id;
  }
  CT_CHECK(ntext >= 3);
  for (int round = 0; round < 4; round++) {
    for (size_t i = 0; i < ntext; i++) {
      CT_CHECK(ccord_session_select_channel(F.session, text[i], &err) == CCORD_OK);
      frame(1);
    }
  }
  frame(10);
  size_t asked = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_HISTORY) - h0;
  CT_CHECK(asked <= ntext);                         // 4 rounds over ntext channels: no more than one request each
  CT_CHECK(ccord_session_select_channel(F.session, home, &err) == CCORD_OK);
  frame(2);
}

static void test_extensions_switch(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  click(l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
  CT_CHECK(dbg().settings_open);
  ccord_ui_set_extensions_enabled(F.ui, false);            // app-side sync: no host callback
  CT_CHECK(F.ext_calls == 0 && !dbg().extensions_on);
  click(dbg().settings_plugins_tab.x + 10.0f, dbg().settings_plugins_tab.y + 10.0f);
  ccord_rect t = dbg().extensions_toggle;
  click(t.x + t.w - 20.0f, t.y + t.h * 0.5f);             // switch on
  CT_CHECK(F.ext_calls == 1 && F.ext_last && dbg().extensions_on);
  click(t.x + t.w - 20.0f, t.y + t.h * 0.5f);             // switch off
  CT_CHECK(F.ext_calls == 2 && !F.ext_last && !dbg().extensions_on);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!dbg().settings_open);
}

static void test_settings_categories(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  click(l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
  ccord_ui_settings_debug s;
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.open && s.page_count == 9);
  // Slot 1 is the retired Profiles page: it keeps its index but has no tab.
  for (int i = 0; i < s.page_count; i++) {
    if (i == 1) CT_CHECK(s.tabs[i].w == 0.0f && s.tabs[i].h == 0.0f);
    else CT_CHECK(s.tabs[i].w > 0.0f && s.tabs[i].h > 0.0f);
  }

  // Appearance: compact/cozy selector.
  click(s.tabs[2].x + 10.0f, s.tabs[2].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 2);
  CT_CHECK(s.display_mode_value == 1);
  click(s.display_mode[0].x + 12.0f, s.display_mode[0].y + 12.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.display_mode_value == 0);
  click(s.display_mode[1].x + 12.0f, s.display_mode[1].y + 12.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.display_mode_value == 1);

  // Voice & Video: devices, volume sliders, mode and hardware acceleration.
  click(s.tabs[3].x + 10.0f, s.tabs[3].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 3);
  click(s.input_device.x + 20.0f, s.input_device.y + 12.0f);
  click(s.output_device.x + 20.0f, s.output_device.y + 12.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.mic_device_value == 1);
  CT_CHECK(s.speaker_device_value == 1);
  click(s.input_volume.x + s.input_volume.w * 0.25f, s.input_volume.y + 2.0f);
  click(s.output_volume.x + s.output_volume.w * 0.75f, s.output_volume.y + 2.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.input_volume_value >= 20 && s.input_volume_value <= 30);
  CT_CHECK(s.output_volume_value >= 70 && s.output_volume_value <= 80);
  click(s.voice_mode[1].x + 12.0f, s.voice_mode[1].y + 12.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.voice_mode_value == 1);
  bool hw = s.hardware_accel;
  click(s.hardware_toggle.x + s.hardware_toggle.w - 10.0f, s.hardware_toggle.y + s.hardware_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.hardware_accel == !hw);

  // Streaming & Encoders: default encoder, framework, resolution and FPS.
  click(s.tabs[4].x + 10.0f, s.tabs[4].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 4);
  save_stream_screenshot("settings_streaming.png");
  click(s.encoder[2].x + 10.0f, s.encoder[2].y + 12.0f);
  click(s.framework[3].x + 10.0f, s.framework[3].y + 12.0f);
  click(s.resolution[1].x + 10.0f, s.resolution[1].y + 12.0f);
  click(s.fps[2].x + 10.0f, s.fps[2].y + 12.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.encoder_value == 2 && s.framework_value == 3 && s.resolution_value == 1 && s.fps_value == 2);

  // Text & Images: inline embed and attachment cards.
  click(s.tabs[5].x + 10.0f, s.tabs[5].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 5);
  CT_CHECK(s.embeds && s.attachments);
  click(s.embed_toggle.x + s.embed_toggle.w - 10.0f, s.embed_toggle.y + s.embed_toggle.h * 0.5f);
  click(s.attachment_toggle.x + s.attachment_toggle.w - 10.0f, s.attachment_toggle.y + s.attachment_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(!s.embeds && !s.attachments);

  // The same choices are persisted to the INI file (display mode back to cozy, cards off).
  FILE *sf = fopen(ui_test_file("settings.ini"), "r");
  CT_CHECK(sf != nullptr);
  if (sf) {
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, sf);
    buf[n] = '\0';
    fclose(sf);
    CT_CHECK(strstr(buf, "[ui]") != nullptr);
    CT_CHECK(strstr(buf, "display_mode = 1") != nullptr);
    CT_CHECK(strstr(buf, "embeds = 0") != nullptr);
    CT_CHECK(strstr(buf, "attachments = 0") != nullptr);
  }

  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!dbg().settings_open);

  // With both Text & Images switches off, attachment/embed markers stay plain (no cards).
  ccord_id ch = ccord_store_selected_channel(F.store);
  size_t n = ccord_store_message_count(F.store, ch);
  const ccord_message *last = n ? ccord_store_message_at(F.store, ch, n - 1) : nullptr;
  ccord_attachment off_att = {.id = 1, .filename = "shot.png", .content_type = "image/png",
                              .url = "https://cdn.discordapp.com/attachments/1/2/shot.png", .size = 2048};
  ccord_embed off_emb = {.title = "Release", .description = "Notes for v1"};
  ccord_message_rich off_rich = {.present = CCORD_RICH_ALL, .attachments = &off_att, .attachment_count = 1,
                                 .embeds = &off_emb, .embed_count = 1};
  ccord_message card = {.id = last ? last->id + 1 : 1, .channel_id = ch, .author_id = ccord_store_self_id(F.store),
                        .timestamp_ms = last ? last->timestamp_ms + 1 : (int64_t)F.now,
                        .content = "", .rich = &off_rich};
  ccord_error err;
  CT_CHECK(ccord_store_append_message(F.store, &card, &err) == CCORD_OK);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(3);
  ccord_ui_debug cards = dbg();
  CT_CHECK(cards.attachment_cards == 0 && cards.embed_cards == 0);
  CT_CHECK(ccord_store_delete_message(F.store, ch, card.id, &err) == CCORD_OK);
  frame(2);
}

static void test_settings_and_resize(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float gx = l->user_panel.x + l->user_panel.w - 24.0f, gy = l->user_panel.y + l->user_panel.h * 0.5f;
  click(gx, gy);
  CT_CHECK(dbg().settings_open);
  CT_CHECK(!F.text_input_active);   // modal: the composer no longer gets text
  type_text("zzz");
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!dbg().settings_open && F.text_input_active);
  click(gx, gy);
  CT_CHECK(dbg().settings_open);
  // the close button (top-right of the full-window Settings page: 32 px square, 16 px inset) closes it
  click(1280.0f - 56.0f + 16.0f, 16.0f + 16.0f);
  CT_CHECK(!dbg().settings_open);

  // resize: layout stays valid, bottom anchor kept
  frame(1);
  CT_CHECK(dbg().stick_bottom);
  send((ccord_input_event){.type = CCORD_IN_RESIZE, .x = 700.0f, .y = 450.0f});
  frame(2);
  l = ccord_ui_get_layout(F.ui);
  CT_CHECK(!l->member_list_visible);
  CT_CHECK(l->message_area.w > 100 && l->message_area.h > 50);
  CT_CHECK(fabsf(l->composer.y + l->composer.h - 450.0f) < 0.01f);
  CT_CHECK(dbg().stick_bottom && dbg().last_visible == dbg().message_count - 1);
  send((ccord_input_event){.type = CCORD_IN_RESIZE, .x = 1280.0f, .y = 800.0f});
  frame(2);
  CT_CHECK(ccord_ui_get_layout(F.ui)->member_list_visible);
}

static void test_members_toggle(void)
{
  frame(2);
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  CT_CHECK(l->member_list_visible);
  ccord_rect b = dbg().members_toggle;
  CT_CHECK(b.w > 0.0f && b.h > 0.0f);
  CT_CHECK(b.y >= l->chat_header.y && b.y + b.h <= l->chat_header.y + l->chat_header.h);
  CT_CHECK(b.x + b.w <= l->chat_header.x + l->chat_header.w);
  float chat_w = l->message_area.w;
  click(b.x + b.w * 0.5f, b.y + b.h * 0.5f);   // hide
  frame(2);
  l = ccord_ui_get_layout(F.ui);
  CT_CHECK(!l->member_list_visible);
  CT_CHECK(fabsf(l->message_area.w - (chat_w + F.theme.m.member_list_width)) < 0.01f);
  CT_CHECK(dbg().members_toggle.w > 0.0f);   // the button stays so the list can come back
  CT_CHECK(dbg().stick_bottom);
  b = dbg().members_toggle;
  click(b.x + b.w * 0.5f, b.y + b.h * 0.5f);   // show again
  frame(2);
  l = ccord_ui_get_layout(F.ui);
  CT_CHECK(l->member_list_visible && fabsf(l->message_area.w - chat_w) < 0.01f);
  // narrow window: no member list, no button; hidden state does not resurrect the list
  click(b.x + b.w * 0.5f, b.y + b.h * 0.5f);   // hide
  send((ccord_input_event){.type = CCORD_IN_RESIZE, .x = 700.0f, .y = 450.0f});
  frame(2);
  CT_CHECK(!ccord_ui_get_layout(F.ui)->member_list_visible && dbg().members_toggle.w == 0.0f);
  send((ccord_input_event){.type = CCORD_IN_RESIZE, .x = 1280.0f, .y = 800.0f});
  frame(2);
  CT_CHECK(!ccord_ui_get_layout(F.ui)->member_list_visible && dbg().members_toggle.w > 0.0f);
  b = dbg().members_toggle;
  click(b.x + b.w * 0.5f, b.y + b.h * 0.5f);   // restore the default for the following tests
  frame(2);
  CT_CHECK(ccord_ui_get_layout(F.ui)->member_list_visible);
}

static void test_media_and_redirects(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  // Open Settings -> My Account: test Billing, Nitro, Shop, Server Boost browser redirects.
  click(l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
  ccord_ui_settings_debug s;
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.open);
  click(s.tabs[0].x + 10.0f, s.tabs[0].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 0);
  CT_CHECK(s.billing_btn.w > 0.0f && s.nitro_btn.w > 0.0f && s.shop_btn.w > 0.0f && s.boost_btn.w > 0.0f);

  int before_urls = F.open_url_calls;
  click(s.billing_btn.x + 20.0f, s.billing_btn.y + 12.0f);
  CT_CHECK(F.open_url_calls == before_urls + 1 && strcmp(F.last_url, "https://discord.com/settings/billing") == 0);
  click(s.nitro_btn.x + 20.0f, s.nitro_btn.y + 12.0f);
  CT_CHECK(F.open_url_calls == before_urls + 2 && strcmp(F.last_url, "https://discord.com/nitro") == 0);
  click(s.shop_btn.x + 20.0f, s.shop_btn.y + 12.0f);
  CT_CHECK(F.open_url_calls == before_urls + 3 && strcmp(F.last_url, "https://discord.com/shop") == 0);
  click(s.boost_btn.x + 20.0f, s.boost_btn.y + 12.0f);
  CT_CHECK(F.open_url_calls == before_urls + 4 && strcmp(F.last_url, "https://discord.com/nitro") == 0);

  // Appearance: Custom theme overrides toggle.
  click(s.tabs[2].x + 10.0f, s.tabs[2].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 2 && !s.quickcss_enabled);
  click(s.quickcss_toggle.x + s.quickcss_toggle.w - 10.0f, s.quickcss_toggle.y + s.quickcss_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.quickcss_enabled);
  click(s.quickcss_toggle.x + s.quickcss_toggle.w - 10.0f, s.quickcss_toggle.y + s.quickcss_toggle.h * 0.5f);

  // Text & Images: re-enable inline embeds and attachments for media card testing.
  click(s.tabs[5].x + 10.0f, s.tabs[5].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  if (!s.embeds) click(s.embed_toggle.x + s.embed_toggle.w - 10.0f, s.embed_toggle.y + s.embed_toggle.h * 0.5f);
  if (!s.attachments) click(s.attachment_toggle.x + s.attachment_toggle.w - 10.0f, s.attachment_toggle.y + s.attachment_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.embeds && s.attachments);

  // Notifications: Desktop notification toggle and timeout selector.
  click(s.tabs[6].x + 10.0f, s.tabs[6].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 6 && s.notify_desktop);
  click(s.notify_timeout[2].x + 12.0f, s.notify_timeout[2].y + 12.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.notify_timeout_ms == 10000);

  ccord_id ch = ccord_store_selected_channel(F.store);
  ccord_notification raw_notif = {.guild_id = ccord_store_selected_guild(F.store), .channel_id = ch,
                                  .message_id = 1, .author_id = ccord_store_self_id(F.store),
                                  .is_dm = false, .is_mention = true};
  char ntitle[128] = {0}, nbody[256] = {0};
  CT_CHECK(ccord_ui_prepare_notification(F.ui, &raw_notif, ntitle, sizeof ntitle, nbody, sizeof nbody));
  CT_CHECK(ntitle[0] != '\0' && nbody[0] != '\0');

  // Disabling desktop notifications suppresses ccord_ui_prepare_notification.
  click(s.notify_desktop_toggle.x + s.notify_desktop_toggle.w - 10.0f,
        s.notify_desktop_toggle.y + s.notify_desktop_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(!s.notify_desktop);
  CT_CHECK(!ccord_ui_prepare_notification(F.ui, &raw_notif, ntitle, sizeof ntitle, nbody, sizeof nbody));
  click(s.notify_desktop_toggle.x + s.notify_desktop_toggle.w - 10.0f,
        s.notify_desktop_toggle.y + s.notify_desktop_toggle.h * 0.5f);

  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!dbg().settings_open);

  int saved_w = 0, saved_h = 0;
  CT_CHECK(ccord_ui_get_saved_window_size(F.ui, &saved_w, &saved_h));
  CT_CHECK(saved_w == 1280 && saved_h == 800);

  // Guild right-click menu: Server Boost, Nitro, Shop, Billing browser redirects, plus Server Settings and Create
  // Channel because you administer this server.
  ccord_rect gr1 = ui_guild_icon_rect(F.ui, 1);
  float gx = gr1.x + gr1.w * 0.5f;
  float gy = gr1.y + gr1.h * 0.5f;
  mouse(CCORD_IN_MOUSE_DOWN, 3, gx, gy);
  frame(1);
  CT_CHECK(dbg().context_menu_open && dbg().context_menu_items == 10);   // + Mute Server, Notification Settings, Events, Leave Server
  CT_CHECK(menu_click_label("Discord Nitro (Browser)"));
  CT_CHECK(!dbg().context_menu_open);
  CT_CHECK(strcmp(F.last_url, "https://discord.com/nitro") == 0);

  // Media texture fetching/rendering (guild icon, user avatar, image attachment/embed) and clickable chat links.
  ccord_error err;
  const ccord_guild *g0 = ccord_store_guild_at(F.store, 0);
  CT_CHECK(g0 != nullptr);
  char g_name_buf[128];
  snprintf(g_name_buf, sizeof g_name_buf, "%s", g0->name ? g0->name : "");
  ccord_guild g_icon = {.id = g0->id, .name = g_name_buf, .icon_url = "a_1234567890abcdef"};
  CT_CHECK(ccord_store_upsert_guild(F.store, &g_icon, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);

  const ccord_user *self = ccord_store_user(F.store, ccord_store_self_id(F.store));
  CT_CHECK(self != nullptr);
  char u_name_buf[128], u_status_buf[128];
  snprintf(u_name_buf, sizeof u_name_buf, "%s", self->name ? self->name : "");
  snprintf(u_status_buf, sizeof u_status_buf, "%s", self->status_text ? self->status_text : "");
  ccord_user u_orig = {.id = self->id, .name = u_name_buf, .status_text = u_status_buf, .avatar_url = "",
                       .bio = "", .pronouns = "", .presence = self->presence, .is_bot = self->is_bot,
                       .avatar_palette = self->avatar_palette};
  ccord_user u_av = u_orig;
  u_av.avatar_url = "fe3ffc01cc5d93689dbd3f09462374e7";
  CT_CHECK(ccord_store_upsert_user(F.store, &u_av, &err) == CCORD_OK);

  size_t mcount = ccord_store_message_count(F.store, ch);
  const ccord_message *last = mcount ? ccord_store_message_at(F.store, ch, mcount - 1) : nullptr;
  ccord_attachment media_att = {.id = 1, .filename = "pic.png", .content_type = "image/png", .size = 512,
                                .url = "https://cdn.discordapp.com/attachments/1/2/pic.png"};
  ccord_embed media_emb = {.title = "Shot", .description = "Preview", .url = "https://media.discordapp.net/attachments/1/2/shot.png",
                           .thumbnail = {.url = "https://media.discordapp.net/attachments/1/2/shot.png"}};
  ccord_message_rich media_rich = {.present = CCORD_RICH_ALL, .attachments = &media_att, .attachment_count = 1,
                                   .embeds = &media_emb, .embed_count = 1};
  ccord_message media_msg = {
    .id = last ? last->id + 1 : 1,
    .channel_id = ch,
    .author_id = u_av.id,
    .timestamp_ms = last ? last->timestamp_ms + 1 : (int64_t)F.now,
    .content = "Visit [Shop](https://discord.com/store/skus/99) or https://discord.com/billing/payments",
    .rich = &media_rich,
  };
  CT_CHECK(ccord_store_append_message(F.store, &media_msg, &err) == CCORD_OK);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(6);
  ccord_ui_debug md = dbg();
  CT_CHECK(md.media_textures >= 3);
  CT_CHECK(md.guild_icon_textures_drawn >= 1);
  CT_CHECK(md.avatar_textures_drawn >= 1);
  CT_CHECK(md.attachment_textures_drawn >= 1);
  CT_CHECK(md.clickable_links >= 2);

  // Verify opening the emoji picker and evicting textures from the renderer cache
  // immediately restores profile pictures and guild icons from the session cache.
  click(l->composer.x + l->composer.w - 58.0f, l->composer.y + 22.0f);   // emoji button: 30px left of the mic
  frame(3);
  CT_CHECK(dbg().emoji_picker_open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!dbg().emoji_picker_open);
  uint8_t dummy_px[4] = {255, 0, 255, 255};
  for (int ti = 0; ti < 1040; ti++) {
    char dkey[64];
    snprintf(dkey, sizeof dkey, "https://cdn.discordapp.com/emojis/%d.png", 900000 + ti);
    CT_CHECK(ccord_render_upload_rgba(F.renderer, dkey, dummy_px, 1, 1, &err) == CCORD_OK);
  }
  frame(2);
  md = dbg();
  CT_CHECK(md.guild_icon_textures_drawn >= 1);
  CT_CHECK(md.avatar_textures_drawn >= 1);
  CT_CHECK(md.attachment_textures_drawn >= 1);

  // Scan and click a rendered link in the message area; payment/store routes normalize to discord.com browser URLs.
  bool clicked_link = false;
  int urls_before_click = F.open_url_calls;
  for (float ly = l->message_area.y + l->message_area.h - 260.0f;
       ly < l->message_area.y + l->message_area.h && !clicked_link; ly += 4.0f) {
    for (float lx = l->message_area.x + 60.0f; lx < l->message_area.x + 420.0f; lx += 12.0f) {
      mouse(CCORD_IN_MOUSE_MOVE, 0, lx, ly);
      if (dbg().hover_link >= 0) {
        click(lx, ly);
        clicked_link = true;
        break;
      }
    }
  }
  CT_CHECK(clicked_link && F.open_url_calls == urls_before_click + 1);
  CT_CHECK(strcmp(F.last_url, "https://discord.com/shop") == 0 ||
           strcmp(F.last_url, "https://discord.com/settings/billing") == 0);

  // Restore original message, user, and guild state before screenshot pixel checks.
  CT_CHECK(ccord_store_delete_message(F.store, ch, media_msg.id, &err) == CCORD_OK);
  frame(2);
}

static void test_screenshot_pixels(void)
{
  frame(3);
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float gx = l->message_area.x + 300;
  float y = scan_hover_message(gx);
  mouse(CCORD_IN_MOUSE_DOWN, 3, gx, y);
  frame(3);
  CT_CHECK(dbg().context_menu_open);
  F.now += 50;
  (void)ccord_ui_update(F.ui, F.now);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  const char *path = "ui_test_shot.png";
  ccord_error err;
  CT_CHECK(ccord_render_screenshot(F.renderer, path, &err) == CCORD_OK);
  ccord_render_end(F.renderer);
  SDL_Surface *s = SDL_LoadPNG(path);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(s->w == 1280 && s->h == 800);
  const ccord_theme_colors *c = &F.theme.c;
  CT_CHECK(pixel_is(s, 5, 700, c->bg_tertiary, 1));                    // server list
  CT_CHECK(pixel_is(s, 150, 600, c->bg_secondary, 1));                  // channel list
  CT_CHECK(pixel_is(s, 1100, 760, c->bg_secondary, 1));                 // member list
  CT_CHECK(pixel_is(s, 120, 780, c->bg_user_panel, 1));                 // user panel
  ccord_rect box = {l->composer.x + F.theme.m.composer_margin_x, l->composer.y, l->composer.w - 2 * F.theme.m.composer_margin_x, F.theme.m.composer_min_height};
  CT_CHECK(pixel_is(s, (int)(box.x + box.w - 40), (int)(box.y + 6), c->bg_input, 1));   // composer
  CT_CHECK(pixel_is(s, 275, 98, c->bg_item_selected, 1) || pixel_is(s, 275, 99, c->bg_item_selected, 1));   // selected channel row
  ccord_rect sel_g = ui_guild_icon_rect(F.ui, 1);
  CT_CHECK(pixel_is(s, 2, (int)(sel_g.y + sel_g.h * 0.5f), c->pill, 2));                              // selected server pill
  // The context menu body is drawn at the pointer position.
  CT_CHECK(pixel_is(s, (int)gx + 150, (int)y + 20, c->bg_floating, 2));
  // not blank: many distinct colours on a coarse grid
  uint32_t seen[64] = {0};
  size_t nseen = 0;
  for (int py = 0; py < 800; py += 5) {
    for (int px = 0; px < 1280; px += 5) {
      uint8_t r, g, b, a;
      SDL_ReadSurfacePixel(s, px, py, &r, &g, &b, &a);
      uint32_t v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
      bool found = false;
      for (size_t i = 0; i < nseen; i++) found = found || seen[i] == v;
      if (!found && nseen < 64) seen[nseen++] = v;
    }
  }
  CT_CHECK(nseen >= 8);
  SDL_DestroySurface(s);
  remove(path);
  key(CCORD_KEY_ESCAPE, 0);
}

static void test_reactions_autocomplete_attachments_and_login(void)
{
  frame(2);
  // 1. Composer autocomplete: @member, #channel, :emoji:
  type_text("@");
  frame(1);
  CT_CHECK(dbg().autocomplete_open && dbg().autocomplete_count > 0);
  key(CCORD_KEY_TAB, 0);
  frame(1);
  CT_CHECK(!dbg().autocomplete_open && ccord_ui_composer_text(F.ui)[0] == '@');
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);

  type_text(":fire");
  frame(1);
  CT_CHECK(dbg().autocomplete_open && dbg().autocomplete_count == 1);
  key(CCORD_KEY_RETURN, 0);
  frame(1);
  CT_CHECK(!dbg().autocomplete_open && strlen(ccord_ui_composer_text(F.ui)) > 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);

  // 2. Staged file attachment send via ccord_session_send_message_with_file
  static const uint8_t k_png[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  CT_CHECK(ccord_ui_attach_bytes(F.ui, "test_image.png", k_png, sizeof k_png));
  frame(1);
  CT_CHECK(dbg().has_staged_attachment && strcmp(dbg().staged_filename, "test_image.png") == 0);
  ccord_id ch = ccord_store_selected_channel(F.store);
  size_t before = ccord_store_message_count(F.store, ch);
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  CT_CHECK(!dbg().has_staged_attachment);
  CT_CHECK(ccord_store_message_count(F.store, ch) == before + 1);

  // 3. Settings > My Account: Email/Password/2FA + standalone token entry
  ccord_ui_open_account_settings(F.ui);
  frame(1);
  ccord_ui_settings_debug s;
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.open && s.page == 0);
  CT_CHECK(s.account_email_box.w > 0.0f && s.account_password_box.w > 0.0f && s.account_token_box.w > 0.0f);
  type_text("user@example.com");
  key(CCORD_KEY_TAB, 0);
  type_text("secret_pw");
  const char *em = nullptr, *pw = nullptr, *mfa = nullptr;
  ccord_ui_account_login_fields(F.ui, &em, &pw, &mfa);
  CT_CHECK(em && strcmp(em, "user@example.com") == 0);
  CT_CHECK(pw && strcmp(pw, "secret_pw") == 0);
  click(s.account_token_box.x + 12.0f, s.account_token_box.y + 12.0f);
  type_text("TEST_TOKEN_VALUE");
  CT_CHECK(strcmp(ccord_ui_account_token_text(F.ui), "TEST_TOKEN_VALUE") == 0);
  ccord_ui_clear_account_token_text(F.ui);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!dbg().settings_open);
}

typedef struct login_test_transport {
  ccord_transport base;
  int step;
  ccord_connection_state next_state;
  bool emit_state;
  bool emit_snap;
  ccord_snapshot snap;
  ccord_user self_user;
  ccord_guild guild;
  ccord_channel chan;
} login_test_transport;

static ccord_status ltt_connect(ccord_transport *t, ccord_error *err)
{
  (void)t;
  ccord_error_clear(err);
  return CCORD_OK;
}
static void ltt_disconnect(ccord_transport *t) { (void)t; }
static ccord_status ltt_req_hist(ccord_transport *t, ccord_id c, uint32_t l, ccord_error *err)
{
  (void)t, (void)c, (void)l;
  ccord_error_clear(err);
  return CCORD_OK;
}
static ccord_status ltt_send_msg(ccord_transport *t, ccord_id c, const char *s, size_t n, ccord_error *err)
{
  (void)t, (void)c, (void)s, (void)n;
  ccord_error_clear(err);
  return CCORD_OK;
}
static bool ltt_poll(ccord_transport *t, ccord_net_event *out)
{
  login_test_transport *lt = (login_test_transport *)t;
  *out = (ccord_net_event){0};
  if (lt->emit_state) {
    lt->emit_state = false;
    out->type = CCORD_NET_STATE;
    out->state = lt->next_state;
    return true;
  }
  if (lt->emit_snap) {
    lt->emit_snap = false;
    out->type = CCORD_NET_SNAPSHOT;
    out->snapshot = &lt->snap;
    return true;
  }
  return false;
}
static void ltt_destroy(ccord_transport *t) { free(t); }

static const ccord_transport_vtable k_ltt_vt = {
  .name = "login_test",
  .connect = ltt_connect,
  .disconnect = ltt_disconnect,
  .request_history = ltt_req_hist,
  .send_message = ltt_send_msg,
  .poll = ltt_poll,
  .destroy = ltt_destroy,
};

typedef struct login_host_state {
  char clip[128];
  int token_sign_ins;
  int cred_logins;
} login_host_state;

static char *login_clip_get(void *ud)
{
  login_host_state *hs = ud;
  size_t n = strlen(hs->clip);
  char *out = malloc(n + 1);
  if (out) memcpy(out, hs->clip, n + 1);
  return out;
}

static void login_account_action(void *ud, ccord_account_action action)
{
  login_host_state *hs = ud;
  if (action == CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN) hs->token_sign_ins++;
  if (action == CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN) hs->cred_logins++;
}

static bool surf_pixel_near(SDL_Surface *s, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
  uint8_t pr = 0, pg = 0, pb = 0, pa = 0;
  return SDL_ReadSurfacePixel(s, x, y, &pr, &pg, &pb, &pa) &&
         abs((int)pr - (int)r) <= 3 && abs((int)pg - (int)g) <= 3 && abs((int)pb - (int)b) <= 3;
}

static void test_dedicated_login_screen(void)
{
  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  login_test_transport *lt = calloc(1, sizeof *lt);
  CT_CHECK(bus && store && lt);
  lt->base.vt = &k_ltt_vt;
  ccord_session *sess = ccord_session_create(bus, store, &lt->base, &err);
  CT_CHECK(sess != nullptr);
  CT_CHECK(ccord_store_self_id(store) == CCORD_ID_NONE);
  CT_CHECK(ccord_store_guild_count(store) == 0);

  login_host_state hs = {.clip = "CLIPBOARD_DISCORD_TOKEN_999"};
  ccord_ui_deps deps = {
    .bus = bus,
    .store = store,
    .session = sess,
    .contrib = F.contrib,
    .theme = &F.theme,
    .host = {.ud = &hs, .clipboard_get = login_clip_get, .account_action = login_account_action},
    .fixed_now_unix_ms = 1793536200000LL,
  };
  ccord_ui *ui = ccord_ui_create(&deps, &err);
  CT_CHECK(ui != nullptr);
  ccord_ui_set_viewport(ui, 1280.0f, 800.0f);
  ccord_ui_open_account_settings(ui);

  // Pump and render the full-window Login Screen
  (void)ccord_ui_update(ui, 100);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(ui, F.renderer);
  const char *lshot = "ui_login_screen_test.png";
  remove(lshot);
  CT_CHECK(ccord_render_screenshot(F.renderer, lshot, &err) == CCORD_OK);
  ccord_render_end(F.renderer);

  ccord_ui_debug d = {0};
  ccord_ui_get_debug(ui, &d);
  ccord_ui_settings_debug sd = {0};
  ccord_ui_get_settings_debug(ui, &sd);
  CT_CHECK(d.settings_open && d.login_screen_open && sd.login_screen);
  CT_CHECK(sd.login_card.w == 480.0f && sd.login_card.h == 420.0f);
  CT_CHECK(sd.login_tab == 0 && sd.account_focus_field == 1);

  SDL_Surface *surf = SDL_LoadPNG(lshot);
  CT_CHECK(surf != nullptr);
  if (surf) {
    // Full-window bg_tertiary backdrop outside the 480x420 card
    CT_CHECK(surf_pixel_near(surf, 20, 20, F.theme.c.bg_tertiary.r, F.theme.c.bg_tertiary.g, F.theme.c.bg_tertiary.b));
    // bg_secondary card interior (the shared uis_card token)
    CT_CHECK(surf_pixel_near(surf, (int)(sd.login_card.x + 12.0f), (int)(sd.login_card.y + 200.0f),
                             F.theme.c.bg_secondary.r, F.theme.c.bg_secondary.g, F.theme.c.bg_secondary.b));
    // brand Connect to Discord primary CTA button
    CT_CHECK(surf_pixel_near(surf, (int)(sd.account_token_save_btn.x + 16.0f),
                             (int)(sd.account_token_save_btn.y + 18.0f),
                             F.theme.c.brand.r, F.theme.c.brand.g, F.theme.c.brand.b));
    SDL_DestroySurface(surf);
  }
  remove(lshot);

  // Esc and outside-card clicks must NOT dismiss the unauthenticated Login Screen
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_ESCAPE});
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = 20.0f, .y = 20.0f});
  ccord_ui_get_debug(ui, &d);
  CT_CHECK(d.settings_open && d.login_screen_open);

  // Tab 1 (Token Sign-In): Paste from Clipboard button + Connect to Discord button
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                                                       .x = sd.account_token_paste_btn.x + 12.0f,
                                                       .y = sd.account_token_paste_btn.y + 12.0f});
  CT_CHECK(strcmp(ccord_ui_account_token_text(ui), "CLIPBOARD_DISCORD_TOKEN_999") == 0);
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                                                       .x = sd.account_token_save_btn.x + 12.0f,
                                                       .y = sd.account_token_save_btn.y + 12.0f});
  CT_CHECK(hs.token_sign_ins == 1);

  // Tab 2 (Email & Password + 2FA auto-focus on MFA ticket)
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                                                       .x = sd.login_tab_credentials.x + 12.0f,
                                                       .y = sd.login_tab_credentials.y + 12.0f});
  ccord_ui_get_settings_debug(ui, &sd);
  CT_CHECK(sd.login_tab == 1 && sd.account_focus_field == 2);
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_TEXT, .text = "dev@discord.test"});
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_TAB});
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_TEXT, .text = "correct_horse"});
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                                                       .x = sd.account_login_btn.x + 12.0f,
                                                       .y = sd.account_login_btn.y + 12.0f});
  CT_CHECK(hs.cred_logins == 1);

  // Simulate MFA required response -> auto-focuses 6-digit 2FA box (field 4)
  ccord_ui_set_login_status(ui, "2FA required: enter 6-digit code", true);
  ccord_ui_get_settings_debug(ui, &sd);
  CT_CHECK(sd.account_mfa_required && sd.account_focus_field == 4);
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_TEXT, .text = "654321"});
  const char *em = nullptr, *pw = nullptr, *mfa = nullptr;
  ccord_ui_account_login_fields(ui, &em, &pw, &mfa);
  CT_CHECK(em && strcmp(em, "dev@discord.test") == 0);
  CT_CHECK(pw && strcmp(pw, "correct_horse") == 0);
  CT_CHECK(mfa && strcmp(mfa, "654321") == 0);

  // Connecting state -> Auth failure (4004/401) keeps Login Screen open with error banner
  ccord_ui_clear_account_token_text(ui);
  lt->emit_state = true;
  lt->next_state = CCORD_CONN_CONNECTING;
  (void)ccord_session_pump(sess);
  (void)ccord_bus_drain(bus);
  (void)ccord_ui_update(ui, 200);
  ccord_ui_get_debug(ui, &d);
  CT_CHECK(d.login_screen_open);

  lt->emit_state = true;
  lt->next_state = CCORD_CONN_FAILED;
  (void)ccord_session_pump(sess);
  (void)ccord_bus_drain(bus);
  (void)ccord_ui_update(ui, 250);
  ccord_ui_get_settings_debug(ui, &sd);
  CT_CHECK(sd.login_screen && sd.account_login_status[0] != '\0');

  // Gateway READY (SNAPSHOT + CCORD_CONN_CONNECTED) auto-closes the Login Screen into the live workspace
  lt->self_user = (ccord_user){.id = 42u, .name = "LiveUser", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  lt->guild = (ccord_guild){.id = 100u, .name = "Live Guild"};
  lt->chan = (ccord_channel){.id = 200u, .guild_id = 100u, .name = "general", .topic = "", .kind = CCORD_CHANNEL_TEXT};
  lt->snap = (ccord_snapshot){
    .self_id = 42u,
    .users = &lt->self_user,
    .user_count = 1,
    .guilds = &lt->guild,
    .guild_count = 1,
    .channels = &lt->chan,
    .channel_count = 1,
  };
  lt->emit_snap = true;
  (void)ccord_session_pump(sess);
  (void)ccord_bus_drain(bus);
  (void)ccord_ui_update(ui, 300);
  ccord_ui_get_debug(ui, &d);
  CT_CHECK(!d.settings_open && !d.login_screen_open);

  ccord_ui_destroy(ui);
  ccord_session_destroy(sess);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_category_grouping_and_media_throttling(void)
{
  ccord_error err = {0};
  ccord_id g_orig = ccord_store_selected_guild(F.store);
  ccord_guild g_cat = {.id = 777000u, .name = "Grouping Guild"};
  ccord_channel chans[] = {
    {.id = 777010u, .guild_id = 777000u, .parent_id = 0, .kind = CCORD_CHANNEL_CATEGORY, .name = "Cat A", .topic = ""},
    {.id = 777020u, .guild_id = 777000u, .parent_id = 0, .kind = CCORD_CHANNEL_CATEGORY, .name = "Cat B", .topic = ""},
    {.id = 777011u, .guild_id = 777000u, .parent_id = 777010u, .kind = CCORD_CHANNEL_TEXT, .name = "a-one", .topic = ""},
    {.id = 777001u, .guild_id = 777000u, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT, .name = "uncat-text", .topic = ""},
    {.id = 777021u, .guild_id = 777000u, .parent_id = 777020u, .kind = CCORD_CHANNEL_TEXT, .name = "b-one", .topic = ""},
    {.id = 777012u, .guild_id = 777000u, .parent_id = 777010u, .kind = CCORD_CHANNEL_TEXT, .name = "a-two", .topic = ""},
    {.id = 777002u, .guild_id = 777000u, .parent_id = 0, .kind = CCORD_CHANNEL_VOICE, .name = "uncat-voice", .topic = ""},
  };
  CT_CHECK(ccord_store_upsert_guild(F.store, &g_cat, chans, sizeof chans / sizeof chans[0], nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_guild(F.store, g_cat.id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, 777021u, &err) == CCORD_OK);
  frame(1);

  CT_CHECK(F.ui->ncrow == 7);
  const ccord_id want_order[7] = {777001u, 777002u, 777010u, 777011u, 777012u, 777020u, 777021u};
  for (size_t i = 0; i < 7; i++) {
    const ccord_channel *rc = ccord_store_channel_at(F.store, g_cat.id, F.ui->crow[i].index);
    CT_CHECK(rc && rc->id == want_order[i]);
  }

  // Collapse Cat A (row 2): its unselected children (a-one, a-two) hide, while uncategorized channels remain visible.
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float cat_a_y = l->channel_scroll.y + F.ui->crow[2].y + F.ui->crow[2].h * 0.5f;
  click(l->channel_scroll.x + 40.0f, cat_a_y);
  CT_CHECK(ui_is_collapsed(F.ui, 777010u));
  CT_CHECK(F.ui->ncrow == 5);
  const ccord_id want_collapsed[5] = {777001u, 777002u, 777010u, 777020u, 777021u};
  for (size_t i = 0; i < 5; i++) {
    const ccord_channel *rc = ccord_store_channel_at(F.store, g_cat.id, F.ui->crow[i].index);
    CT_CHECK(rc && rc->id == want_collapsed[i]);
  }

  // Media throttling: queue 6 fixed-size avatar URLs; one ui_pump_media call uploads at most 4 and does not bump vlist epoch.
  for (int i = 0; i < 6; i++) {
    char aurl[128];
    snprintf(aurl, sizeof aurl, "https://cdn.discordapp.com/avatars/88800%d/av%d.png", i, i);
    CT_CHECK(ccord_session_fetch_media(F.session, aurl, &err) == CCORD_OK);
  }
  (void)ccord_session_pump(F.session);
  size_t before_tex = F.ui->dbg_media_textures;
  uint32_t before_epoch = F.ui->vlist.epoch;
  ui_pump_media(F.ui, F.renderer);
  size_t after_first = F.ui->dbg_media_textures;
  CT_CHECK(after_first > before_tex && after_first - before_tex <= 4);
  CT_CHECK(F.ui->vlist.epoch == before_epoch);
  ui_pump_media(F.ui, F.renderer);
  CT_CHECK(F.ui->dbg_media_textures == before_tex + 6);
  CT_CHECK(F.ui->vlist.epoch == before_epoch);

  CT_CHECK(ccord_store_select_guild(F.store, g_orig, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_guild(F.store, g_cat.id, &err) == CCORD_OK);
  frame(1);
}

static void save_optional_screenshot(const char *filename)
{
  const char *dir = getenv("CCORD_EXPR_SCREENSHOT_DIR");
  if (!dir || !dir[0]) return;
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dir, filename);
  ccord_error err = {0};
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  (void)ccord_render_screenshot(F.renderer, path, &err);
  ccord_render_end(F.renderer);
}

static void test_dm_layout_ordering_and_pagination(void)
{
  ccord_error err = {0};
  const ccord_layout *l = ccord_ui_get_layout(F.ui);

  // 1. Chunked scroll pagination: scrolling to the top pages 50-message chunks without duplicate requests.
  ccord_id g1 = ccord_store_guild_at(F.store, 0)->id;
  ccord_id gen = find_channel(g1, "general", CCORD_CHANNEL_TEXT);
  CT_CHECK(gen != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, gen, &err) == CCORD_OK);
  frame(3);
  size_t gen_before = ccord_store_message_count(F.store, gen);
  CT_CHECK(ccord_session_history_has_more(F.session, gen));
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 2000.0f,
                           .x = l->message_area.x + 100.0f, .y = l->message_area.y + 100.0f});
  frame(4);
  CT_CHECK(ccord_store_message_count(F.store, gen) == gen_before + CCORD_SESSION_HISTORY_LIMIT);

  // 2. DM channel ordering (descending effective activity: last_message_id ? last_message_id : id),
  //    Friends view tabs, and right-hand User Profile panel.
  ccord_user u1 = {.id = 1007u, .name = "Fatima", .handle = "fatima", .bio = "", .pronouns = "", .presence = CCORD_PRESENCE_ONLINE};
  ccord_user u2 = {.id = 1008u, .name = "Gus", .handle = "gus", .bio = "", .pronouns = "", .presence = CCORD_PRESENCE_IDLE};
  ccord_user u3 = {.id = 1009u, .name = "Hana", .handle = "hana", .bio = "", .pronouns = "", .presence = CCORD_PRESENCE_ONLINE};
  CT_CHECK(ccord_store_upsert_user(F.store, &u1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_user(F.store, &u2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_user(F.store, &u3, &err) == CCORD_OK);
  ccord_channel dm_old = {.id = 950001u, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                          .name = "Fatima", .topic = "", .user_id = u1.id, .last_message_id = 1000u};
  ccord_channel dm_new = {.id = 950002u, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                          .name = "Gus", .topic = "", .user_id = u2.id, .last_message_id = 5000u};
  // Newly created DM channel with last_message_id == 0 and a newer channel snowflake (980000u > 5000u):
  // UI cmp_dm_sort must fall back to channel.id so dm_created sorts above dm_new and dm_old!
  ccord_channel dm_created = {.id = 980000u, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                              .name = "Hana", .topic = "", .user_id = u3.id, .last_message_id = 0u};
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm_old, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm_new, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm_created, &err) == CCORD_OK);

  ccord_rect dm_icon = ui_guild_icon_rect(F.ui, 0);
  click(dm_icon.x + dm_icon.w * 0.5f, dm_icon.y + dm_icon.h * 0.5f);
  frame(3);
  CT_CHECK(ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID);
  CT_CHECK(F.ui->ncrow >= 7);
  CT_CHECK(F.ui->crow[0].dm_nav == DM_NAV_FRIENDS);
  CT_CHECK(F.ui->crow[1].dm_nav == DM_NAV_NITRO);
  CT_CHECK(F.ui->crow[2].dm_nav == DM_NAV_SHOP);
  CT_CHECK(F.ui->crow[3].dm_nav == DM_NAV_DM_HEADER);
  const ccord_channel *first_dm = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[4].index);
  const ccord_channel *second_dm = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[5].index);
  const ccord_channel *third_dm = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[6].index);
  CT_CHECK(first_dm && second_dm && third_dm);
  CT_CHECK(first_dm && first_dm->id == dm_created.id);
  CT_CHECK(second_dm && second_dm->id == dm_new.id);
  CT_CHECK(third_dm && third_dm->id == dm_old.id);

  // Select dm_new and verify on-demand profile fetch populated u2's profile without reordering DM rows.
  float dm_new_y = l->channel_scroll.y + F.ui->crow[5].y + F.ui->crow[5].h * 0.5f;
  click(l->channel_scroll.x + 60.0f, dm_new_y);
  frame(3);
  const ccord_user *u2_after = ccord_store_user(F.store, u2.id);
  CT_CHECK(u2_after && u2_after->bio && u2_after->bio[0] != '\0' && u2_after->pronouns && u2_after->pronouns[0] != '\0');
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[4].index)->id == dm_created.id);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[5].index)->id == dm_new.id);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[6].index)->id == dm_old.id);

  // Click "Friends" in the DM sidebar -> opens Friends view with Online/All/Pending/Blocked tabs.
  float friends_y = l->channel_scroll.y + F.ui->crow[0].y + F.ui->crow[0].h * 0.5f;
  click(l->channel_scroll.x + 60.0f, friends_y);
  frame(2);
  CT_CHECK(ui_is_dm_friends_view(F.ui));
  for (int t = 0; t < 4; t++) {
    ccord_rect tr = F.ui->dm_friends_tab_rects[t];
    CT_CHECK(tr.w > 0.0f && tr.h > 0.0f);
    click(tr.x + tr.w * 0.5f, tr.y + tr.h * 0.5f);
    frame(1);
    CT_CHECK(F.ui->dm_friends_tab == t);
  }

  // Click dm_old row in DM sidebar -> exits Friends view and selects dm_old without reordering the DM list.
  float dm_old_y = l->channel_scroll.y + F.ui->crow[6].y + F.ui->crow[6].h * 0.5f;
  click(l->channel_scroll.x + 60.0f, dm_old_y);
  frame(3);
  CT_CHECK(!ui_is_dm_friends_view(F.ui));
  CT_CHECK(ccord_store_selected_channel(F.store) == dm_old.id);
  const ccord_user *u1_after = ccord_store_user(F.store, u1.id);
  CT_CHECK(u1_after && u1_after->bio && u1_after->bio[0] != '\0');
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[4].index)->id == dm_created.id);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[5].index)->id == dm_new.id);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[6].index)->id == dm_old.id);

  // Live message arriving in dm_old with id=990000 (> 980000) promotes dm_old to the top of the DM sidebar!
  ccord_message live_dm_msg = {.id = 990000u, .channel_id = dm_old.id, .author_id = u1.id,
                               .timestamp_ms = 1711901000000ULL, .content = "New DM activity"};
  CT_CHECK(ccord_store_append_message(F.store, &live_dm_msg, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[4].index)->id == dm_old.id);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[5].index)->id == dm_created.id);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[6].index)->id == dm_new.id);

  // Verify DM User Profile panel toggle (dm_profile_hidden) is completely independent from Server Member List toggle (members_hidden):
  CT_CHECK(!F.ui->dm_profile_hidden && !F.ui->members_hidden);
  ccord_rect mb = ui_members_btn_rect(F.ui);
  CT_CHECK(mb.w > 0.0f);
  click(mb.x + mb.w * 0.5f, mb.y + mb.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->dm_profile_hidden && !F.ui->members_hidden);
  CT_CHECK(!ccord_ui_get_layout(F.ui)->member_list_visible);
  // Restore DM User Profile panel
  click(mb.x + mb.w * 0.5f, mb.y + mb.h * 0.5f);
  frame(1);
  CT_CHECK(!F.ui->dm_profile_hidden && ccord_ui_get_layout(F.ui)->member_list_visible);
  save_optional_screenshot("dm_1to1_user_profile.png");

  // Verify Group DM rendering in DM sidebar, header, and right-hand participants panel:
  ccord_channel dm_group = {
    .id = 995000u,
    .guild_id = CCORD_DM_GUILD_ID,
    .kind = CCORD_CHANNEL_TEXT,
    .discord_type = CCORD_DISCORD_CHANNEL_GROUP_DM,
    .name = "Direct Message",
    .topic = "",
    .user_id = CCORD_ID_NONE,
    .last_message_id = 999000u,
    .recipient_ids = {u1.id, u2.id, u3.id},
    .recipient_count = 3,
  };
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm_group, &err) == CCORD_OK);
  const ccord_channel *stored_gdm = ccord_store_channel(F.store, dm_group.id);
  CT_CHECK(stored_gdm != nullptr && stored_gdm->user_id == CCORD_ID_NONE);
  CT_CHECK(stored_gdm != nullptr && strcmp(stored_gdm->name, "Fatima, Gus, Hana") == 0);
  CT_CHECK(ccord_store_select_channel(F.store, dm_group.id, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ui_channel_is_group_dm(ui_selected_channel(F.ui)));
  CT_CHECK(ui_dm_recipient(F.ui, ui_selected_channel(F.ui)) == nullptr);
  save_optional_screenshot("dm_group_members.png");

  CT_CHECK(ccord_store_select_guild(F.store, g1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, dm_group.id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, dm_old.id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, dm_new.id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, dm_created.id, &err) == CCORD_OK);
  frame(2);

  // 3. Clicking plain message body text does NOT open the user profile popover;
  //    clicking the author display name, author avatar, or inline @mention pill opens it.
  if (F.ui->nvis > 0) {
    const vis_row *hdr_row = nullptr;
    for (size_t vi = F.ui->nvis; vi > 0; vi--) {
      if (F.ui->vis[vi - 1].author_rect.w > 0.0f && F.ui->vis[vi - 1].avatar_rect.w > 0.0f) {
        hdr_row = &F.ui->vis[vi - 1];
        break;
      }
    }
    CT_CHECK(hdr_row != nullptr);
    if (hdr_row) {
      // Plain message body text click -> must NOT open popover.
      float body_x = hdr_row->author_rect.x + 12.0f;
      float body_y = hdr_row->author_rect.y + hdr_row->author_rect.h + 8.0f;
      click(body_x, body_y);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);

      // Blank space to the right of the author header -> must NOT open popover.
      float blank_x = l->message_area.x + l->message_area.w - 40.0f;
      float blank_y = hdr_row->author_rect.y + hdr_row->author_rect.h * 0.5f;
      click(blank_x, blank_y);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);

      // Direct click on author display name -> opens popover and fetches profile.
      click(hdr_row->author_rect.x + hdr_row->author_rect.w * 0.5f,
            hdr_row->author_rect.y + hdr_row->author_rect.h * 0.5f);
      frame(2);
      CT_CHECK(F.ui->member_popover_open);
      CT_CHECK(F.ui->member_popover_user_id != CCORD_ID_NONE);
      CT_CHECK(F.ui->member_popover_rect.h >= 260.0f);
      const ccord_user_profile *mprof = ccord_store_user_profile(F.store, F.ui->member_popover_user_id);
      CT_CHECK(mprof != nullptr);

      // Click outside popover to close it.
      click(l->server_list.x + 10.0f, l->server_list.y + l->server_list.h - 10.0f);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);

      // Direct click on author avatar -> opens popover.
      click(hdr_row->avatar_rect.x + hdr_row->avatar_rect.w * 0.5f,
            hdr_row->avatar_rect.y + hdr_row->avatar_rect.h * 0.5f);
      frame(2);
      CT_CHECK(F.ui->member_popover_open);
      CT_CHECK(F.ui->member_popover_user_id != CCORD_ID_NONE);

      // Click outside popover to close it.
      click(l->server_list.x + 10.0f, l->server_list.y + l->server_list.h - 10.0f);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);
    }

    // Append a message with an inline <@id> mention and verify clicking the mention pill opens the popover,
    // while clicking the surrounding plain message text does not.
    ccord_id sel_ch = ccord_store_selected_channel(F.store);
    char mention_txt[128];
    snprintf(mention_txt, sizeof mention_txt, "Hello there <@%llu> welcome to the channel",
             (unsigned long long)u1.id);
    ccord_message m_ment = {
      .id = 999888777666ULL,
      .channel_id = sel_ch,
      .author_id = u2.id,
      .timestamp_ms = 1711900999000LL,
      .content = mention_txt,
    };
    CT_CHECK(ccord_store_append_message(F.store, &m_ment, &err) == CCORD_OK);
    frame(2);
    ccord_rect ment_rect = {0};
    for (size_t hi = 0; hi < F.ui->nuser_hits; hi++) {
      if (F.ui->user_hits[hi].user_id == u1.id) {
        ment_rect = F.ui->user_hits[hi].rect;
      }
    }
    CT_CHECK(ment_rect.w > 0.0f && ment_rect.h > 0.0f);
    if (ment_rect.w > 0.0f) {
      // Plain text after the mention pill -> does NOT open popover.
      click(ment_rect.x + ment_rect.w + 25.0f, ment_rect.y + ment_rect.h * 0.5f);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);

      // Clicking the inline @mention pill -> opens popover for u1.
      click(ment_rect.x + ment_rect.w * 0.5f, ment_rect.y + ment_rect.h * 0.5f);
      frame(2);
      CT_CHECK(F.ui->member_popover_open);
      CT_CHECK(F.ui->member_popover_user_id == u1.id);

      click(l->server_list.x + 10.0f, l->server_list.y + l->server_list.h - 10.0f);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);
    }

    // Verify right-side guild member list population, role grouping, and click-to-popover:
    ccord_id sel_gid = ccord_store_selected_guild(F.store);
    CT_CHECK(sel_gid != CCORD_ID_NONE && sel_gid != CCORD_DM_GUILD_ID);
    ccord_user extra_users[2] = {
      {.id = 77001, .name = "Zelda Admin", .handle = "zelda", .status_text = "Ruling Hyrule", .presence = CCORD_PRESENCE_ONLINE},
      {.id = 77002, .name = "Link Hero", .handle = "link", .status_text = "", .presence = CCORD_PRESENCE_OFFLINE},
    };
    ccord_member extra_members[2] = {
      {.guild_id = sel_gid, .user_id = 77001, .role_name = "Triforce", .role_color = 0xF1C40Fu, .role_rank = 1},
      {.guild_id = sel_gid, .user_id = 77002, .role_name = "", .role_color = 0, .role_rank = 255},
    };
    CT_CHECK(ccord_store_upsert_guild_members(F.store, sel_gid, extra_members, 2, extra_users, 2, &err) == CCORD_OK);
    frame(2);
    l = ccord_ui_get_layout(F.ui);
    CT_CHECK(l->member_list_visible);
    CT_CHECK(F.ui->nmrow >= 4);
    bool saw_triforce_group = false;
    bool saw_offline_group = false;
    const member_row *zelda_row = nullptr;
    for (size_t mi = 0; mi < F.ui->nmrow; mi++) {
      const member_row *mr = &F.ui->mrow[mi];
      if (mr->is_header) {
        if (strstr(mr->label, "TRIFORCE") != nullptr) saw_triforce_group = true;
        if (strstr(mr->label, "OFFLINE") != nullptr) saw_offline_group = true;
      } else {
        const ccord_member *m = ccord_store_member_at(F.store, sel_gid, mr->index);
        if (m && m->user_id == 77001) zelda_row = mr;
      }
    }
    CT_CHECK(saw_triforce_group);
    CT_CHECK(saw_offline_group);
    CT_CHECK(zelda_row != nullptr);
    if (zelda_row) {
      float click_x = l->member_list.x + l->member_list.w * 0.5f;
      float click_y = l->member_list.y + zelda_row->y - F.ui->member_scroll + zelda_row->h * 0.5f;
      click(click_x, click_y);
      frame(2);
      CT_CHECK(F.ui->member_popover_open);
      CT_CHECK(F.ui->member_popover_user_id == 77001);
      // In a Server guild, member popover exposes both User Profile (0) and Server Profile (1) tabs:
      CT_CHECK(F.ui->member_popover_tab == 0);
      CT_CHECK(F.ui->member_popover_tab_rects[0].w > 0.0f && F.ui->member_popover_tab_rects[1].w > 0.0f);
      click(F.ui->member_popover_tab_rects[1].x + F.ui->member_popover_tab_rects[1].w * 0.5f,
            F.ui->member_popover_tab_rects[1].y + F.ui->member_popover_tab_rects[1].h * 0.5f);
      frame(2);
      CT_CHECK(F.ui->member_popover_open && F.ui->member_popover_tab == 1);
      save_optional_screenshot("server_member_popover_tabs.png");
      click(l->server_list.x + 10.0f, l->server_list.y + l->server_list.h - 10.0f);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);
    }
  }
}

static void test_expression_picker_and_stickers(void)
{
  ccord_error err = {0};
  ccord_id g1 = ccord_store_guild_at(F.store, 0)->id;
  ccord_id ch = find_channel(g1, "off-topic", CCORD_CHANNEL_TEXT);
  CT_CHECK(ch != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, ch, &err) == CCORD_OK);
  frame(3);

  // 1. Verify custom guild emojis and stickers seeded from mock_transport READY snapshot
  CT_CHECK(ccord_store_guild_emoji_count(F.store, g1) >= 4);
  CT_CHECK(ccord_store_guild_sticker_count(F.store, g1) >= 3);
  const ccord_emoji *ce = ccord_store_find_emoji_by_name(F.store, "ccord");
  CT_CHECK(ce != nullptr && ce->id == 5001u);
  const ccord_sticker *st_wave = ccord_store_find_sticker(F.store, 7001u);
  CT_CHECK(st_wave != nullptr && strcmp(st_wave->name, "WumpusWave") == 0);

  // 2. Send messages containing inline custom emojis and a 160x160 inline sticker
  const char *em_msg =
    "Testing custom emojis <:ccord:5001> <a:partyblob:5004> <:shipit:5002> and Unicode \xF0\x9F\x94\xA5 \xF0\x9F\x8E\x89!";
  CT_CHECK(ccord_session_send_message(F.session, ch, em_msg, strlen(em_msg), &err) == CCORD_OK);
  CT_CHECK(ccord_session_send_sticker(F.session, ch, 7001u, &err) == CCORD_OK);
  frame(4);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(4);
  ui_pump_media(F.ui, F.renderer);
  frame(3);
  save_optional_screenshot("chat_expressions.png");

  // 3. Open Expression Picker on the Emoji tab via composer right-side [Emoji] button
  ccord_rect cbox = ui_composer_box(F.ui);
  click(cbox.x + cbox.w - 52.0f, cbox.y + cbox.h * 0.5f);
  frame(5);
  ui_pump_media(F.ui, F.renderer);
  frame(2);
  CT_CHECK(F.ui->emoji_picker_open);
  CT_CHECK(F.ui->emoji_picker_rect.w >= 400.0f && F.ui->emoji_picker_rect.h >= 400.0f);
  CT_CHECK(ccord_store_gif_category_count(F.store) >= 4);
  CT_CHECK(ccord_store_gif_trending_count(F.store) >= 4);
  CT_CHECK(ccord_store_sticker_pack_count(F.store) >= 2);
  CT_CHECK(ccord_store_favorite_gif_count(F.store) >= 2);
  save_optional_screenshot("picker_emoji.png");

  // Click first emoji cell in composer mode -> inserts emoji into composer and closes picker
  ccord_rect ecell0 = F.ui->emoji_picker_cells[0];
  CT_CHECK(ecell0.w > 0.0f && ecell0.h > 0.0f);
  click(ecell0.x + ecell0.w * 0.5f, ecell0.y + ecell0.h * 0.5f);
  frame(2);
  CT_CHECK(!F.ui->emoji_picker_open);
  CT_CHECK(strlen(ccord_ui_composer_text(F.ui)) > 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);

  // 4. Open Expression Picker on the GIFs tab via composer right-side [GIF] button
  click(cbox.x + cbox.w - 122.0f, cbox.y + cbox.h * 0.5f);
  frame(4);
  ui_pump_media(F.ui, F.renderer);
  frame(2);
  CT_CHECK(F.ui->emoji_picker_open);
  save_optional_screenshot("picker_gifs.png");

  // Click a Trending GIF card in the lower half of the GIFs tab -> sends GIF URL as a message.
  // The home view lists every GIF category (8 in the mock), so the trending grid starts lower down.
  size_t before_gif = ccord_store_message_count(F.store, ch);
  ccord_rect pbox = F.ui->emoji_picker_rect;
  // The tile is found from the picker's drawn hit records (tile heights follow each GIF's own shape, as in Discord).
  ui_expr_inspect gif_ins;
  CT_CHECK(ui_expr_get_inspect(F.ui, &gif_ins));
  ccord_rect gif_tile = {0, 0, 0, 0};   // the visible part of the first GIF tile
  for (size_t gi = 0; gi < gif_ins.nhits && gif_tile.w == 0.0f; gi++) {
    ui_expr_hit_info ghi;
    if (!ui_expr_hit_at(F.ui, gi, &ghi) || ghi.kind != 2) continue;
    float x0 = ghi.rect.x > gif_ins.grid_view.x ? ghi.rect.x : gif_ins.grid_view.x;
    float y0 = ghi.rect.y > gif_ins.grid_view.y ? ghi.rect.y : gif_ins.grid_view.y;
    float x1 = ghi.rect.x + ghi.rect.w < gif_ins.grid_view.x + gif_ins.grid_view.w ? ghi.rect.x + ghi.rect.w
                                                                                    : gif_ins.grid_view.x + gif_ins.grid_view.w;
    float y1 = ghi.rect.y + ghi.rect.h < gif_ins.grid_view.y + gif_ins.grid_view.h ? ghi.rect.y + ghi.rect.h
                                                                                    : gif_ins.grid_view.y + gif_ins.grid_view.h;
    if (x1 - x0 > 20.0f && y1 - y0 > 20.0f) gif_tile = (ccord_rect){x0, y0, x1 - x0, y1 - y0};
  }
  CT_CHECK(gif_tile.w > 0.0f);
  click(gif_tile.x + gif_tile.w * 0.5f, gif_tile.y + gif_tile.h * 0.5f);
  frame(4);
  CT_CHECK(!F.ui->emoji_picker_open);
  CT_CHECK(ccord_store_message_count(F.store, ch) == before_gif + 1);
  const ccord_message *sent_gif = ccord_store_message_at(F.store, ch, ccord_store_message_count(F.store, ch) - 1);
  CT_CHECK(sent_gif && strstr(sent_gif->content, "https://tenor.com/view/") != nullptr);

  // 5. Open Expression Picker on the Stickers tab via composer right-side [Sticker] button
  click(cbox.x + cbox.w - 84.0f, cbox.y + cbox.h * 0.5f);
  frame(4);
  ui_pump_media(F.ui, F.renderer);
  frame(2);
  CT_CHECK(F.ui->emoji_picker_open);
  save_optional_screenshot("picker_stickers.png");

  // Click a sticker cell in the Stickers tab -> sends sticker message via ccord_session_send_sticker
  size_t before_stk = ccord_store_message_count(F.store, ch);
  click(pbox.x + 95.0f, pbox.y + 165.0f);
  frame(4);
  CT_CHECK(!F.ui->emoji_picker_open);
  CT_CHECK(ccord_store_message_count(F.store, ch) == before_stk + 1);
  const ccord_message *sent_stk = ccord_store_message_at(F.store, ch, ccord_store_message_count(F.store, ch) - 1);
  CT_CHECK(sent_stk && sent_stk->content[0] == '\0' && sent_stk->rich && sent_stk->rich->sticker_count == 1);

  // 6. Verify send-time :shortcode: expansion for both Unicode and custom guild emojis
  size_t before_sc = ccord_store_message_count(F.store, ch);
  type_text("Test :thumbsup: :ccord:!");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  CT_CHECK(ccord_store_message_count(F.store, ch) == before_sc + 1);
  const ccord_message *sent_sc = ccord_store_message_at(F.store, ch, ccord_store_message_count(F.store, ch) - 1);
  CT_CHECK(sent_sc != nullptr);
  if (sent_sc) {
    CT_CHECK(strstr(sent_sc->content, "\xF0\x9F\x91\x8D") != nullptr);
    CT_CHECK(strstr(sent_sc->content, "<:ccord:5001>") != nullptr);
  }
}

static void test_forum_channel_ui_flow(void)
{
  ccord_error err = {0};
  ccord_id g1 = ccord_store_guild_at(F.store, 0)->id;
  ccord_id forum_ch = find_channel(g1, "design-forum", CCORD_CHANNEL_TEXT);
  CT_CHECK(forum_ch != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, forum_ch, &err) == CCORD_OK);
  frame(3);

  // 1. Verify Forum UI view is active and displays all 5 seeded post cards (pinned post first)
  CT_CHECK(ui_forum_is_active(F.ui));
  CT_CHECK(ui_forum_visible_post_count(F.ui) == 5);
  ccord_id first_tid = CCORD_ID_NONE;
  ccord_rect card0 = ui_forum_post_card_rect(F.ui, 0, &first_tid);
  CT_CHECK(card0.w > 100.0f && card0.h > 40.0f);
  CT_CHECK(first_tid == 3151); // Pinned "Welcome & Forum Guidelines" post

  // 2. Click a filter tag pill ("Bug", index 1) -> filters visible posts down to 1
  ccord_rect bug_tag = ui_forum_filter_tag_rect(F.ui, 1);
  CT_CHECK(bug_tag.w > 20.0f && bug_tag.h > 14.0f);
  click(bug_tag.x + bug_tag.w * 0.5f, bug_tag.y + bug_tag.h * 0.5f);
  frame(2);
  CT_CHECK(ui_forum_visible_post_count(F.ui) == 1);
  // Click the same tag pill again to clear the tag filter
  click(bug_tag.x + bug_tag.w * 0.5f, bug_tag.y + bug_tag.h * 0.5f);
  frame(2);
  CT_CHECK(ui_forum_visible_post_count(F.ui) == 5);

  // 3. Click "+ New Post", enter title & body, and submit -> creates post and opens its thread channel
  ccord_rect new_btn = ui_forum_new_post_btn_rect(F.ui);
  CT_CHECK(new_btn.w > 40.0f && new_btn.h > 20.0f);
  click(new_btn.x + new_btn.w * 0.5f, new_btn.y + new_btn.h * 0.5f);
  frame(2);
  type_text("UI Test Forum Post");
  key(CCORD_KEY_TAB, 0);
  frame(1);
  type_text("Body of the newly created forum post.");
  frame(2);
  ccord_rect submit_btn = ui_forum_composer_submit_rect(F.ui);
  CT_CHECK(submit_btn.w > 30.0f && submit_btn.h > 16.0f);
  click(submit_btn.x + submit_btn.w * 0.5f, submit_btn.y + submit_btn.h * 0.5f);
  frame(4);

  // Submitting the new forum post opens and selects the newly created thread channel!
  CT_CHECK(!ui_forum_is_active(F.ui));
  ccord_id opened_tid = ccord_store_selected_channel(F.store);
  CT_CHECK(opened_tid != forum_ch && opened_tid != CCORD_ID_NONE);
  const ccord_channel *opened_ch = ccord_store_channel(F.store, opened_tid);
  CT_CHECK(opened_ch != nullptr && opened_ch->parent_id == forum_ch);

  // 4. Click the forum breadcrumb in the chat header ("<forum icon> design-forum") to return to the forum post list
  ccord_rect bc = ui_forum_breadcrumb_rect(F.ui);
  CT_CHECK(bc.w > 30.0f && bc.h > 12.0f);
  click(bc.x + bc.w * 0.5f, bc.y + bc.h * 0.5f);
  frame(3);
  CT_CHECK(ccord_store_selected_channel(F.store) == forum_ch);
  CT_CHECK(ui_forum_is_active(F.ui));
  CT_CHECK(ui_forum_visible_post_count(F.ui) >= 5);
}

// An open picker over a forum gets the click on its tile; with no overlay the same post card still opens.
static void test_picker_over_forum_clicks(void)
{
  ccord_error err = {0};
  ccord_id g1 = ccord_store_guild_at(F.store, 0)->id;
  ccord_id forum_ch = find_channel(g1, "design-forum", CCORD_CHANNEL_TEXT);
  CT_CHECK(forum_ch != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, forum_ch, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(ui_forum_is_active(F.ui));

  // 1. The emoji picker (tab 2 = EXPR_TAB_EMOJI) opens over the forum post list. A click on its first cell goes
  // to the picker: the emoji is inserted and the picker closes, and no forum post opens.
  ui_expr_picker_open(F.ui, 2, forum_ch, CCORD_ID_NONE);
  frame(3);
  CT_CHECK(F.ui->emoji_picker_open);
  ccord_rect cell = F.ui->emoji_picker_cells[0];
  CT_CHECK(cell.w > 0.0f && cell.h > 0.0f);
  ccord_text_edit_clear(&F.ui->edit);
  click(cell.x + cell.w * 0.5f, cell.y + cell.h * 0.5f);
  frame(2);
  CT_CHECK(!F.ui->emoji_picker_open);
  CT_CHECK(strlen(ccord_ui_composer_text(F.ui)) > 0);
  CT_CHECK(ccord_store_selected_channel(F.store) == forum_ch);
  CT_CHECK(ui_forum_is_active(F.ui));
  ccord_text_edit_clear(&F.ui->edit);

  // 2. No overlay is open: a click on a post card (index 1, not the pinned one) opens that post.
  ccord_id tid = CCORD_ID_NONE;
  ccord_rect card = ui_forum_post_card_rect(F.ui, 1, &tid);
  CT_CHECK(card.w > 100.0f && card.h > 40.0f && tid != CCORD_ID_NONE);
  click(card.x + card.w * 0.5f, card.y + card.h * 0.5f);
  frame(3);
  CT_CHECK(ccord_store_selected_channel(F.store) == tid);
  CT_CHECK(!ui_forum_is_active(F.ui));
}

static void test_vanilla_discord_gap_features(void)
{
  ccord_error err = {0};
  ccord_id g1 = ccord_store_guild_at(F.store, 0)->id;
  ccord_id gen = find_channel(g1, "general", CCORD_CHANNEL_TEXT);
  ccord_id dev = find_channel(g1, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(gen != CCORD_ID_NONE && dev != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, gen, &err) == CCORD_OK);
  frame(3);

  // 1. Rich Composer UX: Ctrl+B bold wrap, Ctrl+Z undo, Ctrl+Y redo
  ccord_text_edit_clear(&F.ui->edit);
  type_text("hello");
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_B, CCORD_MOD_CTRL);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&F.ui->edit), "**hello**") == 0);
  key(CCORD_KEY_Z, CCORD_MOD_CTRL);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&F.ui->edit), "hello") == 0);
  key(CCORD_KEY_Y, CCORD_MOD_CTRL);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&F.ui->edit), "**hello**") == 0);
  ccord_text_edit_clear(&F.ui->edit);

  // 2. Per-channel draft persistence across channel switches
  type_text("draft in general");
  float dev_y = scan_hover_channel(200.0f, dev);
  CT_CHECK(dev_y > 0.0f);
  click(200.0f, dev_y);
  frame(2);
  CT_CHECK(ccord_store_selected_channel(F.store) == dev);
  CT_CHECK(F.ui->edit.len == 0);
  float gen_y = scan_hover_channel(200.0f, gen);
  CT_CHECK(gen_y > 0.0f);
  click(200.0f, gen_y);
  frame(2);
  CT_CHECK(ccord_store_selected_channel(F.store) == gen);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&F.ui->edit), "draft in general") == 0);
  ccord_text_edit_clear(&F.ui->edit);

  // 3. Up arrow on empty composer starts editing last own message
  key(CCORD_KEY_UP, 0);
  frame(1);
  CT_CHECK(F.ui->editing_message != CCORD_ID_NONE);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(F.ui->editing_message == CCORD_ID_NONE);

  // 4. Quick Switcher (Ctrl+K) modal open, filter, and Escape close
  key(CCORD_KEY_K, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(F.ui->quick_switcher_open);
  type_text("#dev");
  frame(2);
  CT_CHECK(F.ui->nquick_switcher_items >= 1);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!F.ui->quick_switcher_open);

  // 5. Chat message text highlighting (drag-select) & Ctrl+C copy
  const ccord_layout *l = &F.ui->layout;
  float mx = l->message_area.x + 120.0f;
  float my = scan_hover_self_message(mx);
  if (my > 0.0f) {
    F.clip[0] = '\0';
    mouse(CCORD_IN_MOUSE_DOWN, 1, l->message_area.x + 60.0f, my - 10.0f);
    mouse(CCORD_IN_MOUSE_MOVE, 0, l->message_area.x + 420.0f, my + 24.0f);
    frame(2);
    mouse(CCORD_IN_MOUSE_UP, 1, l->message_area.x + 420.0f, my + 24.0f);
    frame(1);
    CT_CHECK(F.ui->chat_sel_active && F.ui->chat_sel_len > 0);
    shot("selection");
    key(CCORD_KEY_C, CCORD_MOD_CTRL);
    CT_CHECK(strlen(F.clip) > 0);
  }

  // 6. Permission-gated server management & moderation context menus when user is guild owner
  const ccord_guild *orig_g = ccord_store_guild(F.store, g1);
  CT_CHECK(orig_g != nullptr);
  ccord_guild admin_g = *orig_g;
  admin_g.owner_id = ccord_store_self_id(F.store);
  admin_g.self_permissions = UINT64_MAX;
  CT_CHECK(ccord_store_upsert_guild(F.store, &admin_g, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  frame(2);

  // Right-clicking channel `dev` now includes Mute, Notification Settings, Edit Channel, Clone Channel,
  // Create Channel, Create Thread, Delete Channel + plugin item (8 items)
  dev_y = scan_hover_channel(200.0f, dev);
  CT_CHECK(dev_y > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dev_y);
  frame(1);
  CT_CHECK(dbg().context_menu_open && dbg().context_menu_items == 8);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
}

static bool menu_has(const char *label)
{
  for (size_t i = 0; i < F.ui->menu.count; i++) {
    if (F.ui->menu.items[i].label && strcmp(F.ui->menu.items[i].label, label) == 0) return true;
  }
  return false;
}

static void open_menu(ccord_menu_target target, ccord_id guild, ccord_id channel, ccord_id user, ccord_id message)
{
  ui_menu_close(F.ui);
  ccord_menu_context ctx = {.target = target, .guild_id = guild, .channel_id = channel, .message_id = message, .user_id = user};
  ui_menu_open(F.ui, 300.0f, 300.0f, &ctx);
}

// What the context menus offer follows the permission engine, not a flag: the mock makes you an administrator of C-Cord
// HQ, a moderator of Lua Lounge (whose owner is Ada) and an ordinary member of Pixel Garden.
static void test_permission_gated_menus(void)
{
  const ccord_id self = 1001, ada = 1002, bjorn = 1003, chen = 1004;
  const ccord_id hq = 2001, lua = 2002, garden = 2003;
  CT_CHECK(ccord_store_self_id(F.store) == self);
  const ccord_id hq_chan = find_channel(hq, "dev", CCORD_CHANNEL_TEXT);
  const ccord_id lua_chan = find_channel(lua, "showcase", CCORD_CHANNEL_TEXT);
  const ccord_id garden_chan = find_channel(garden, "art", CCORD_CHANNEL_TEXT);
  const ccord_id garden_read_only = find_channel(garden, "critique", CCORD_CHANNEL_TEXT);   // overwrites: no threads
  CT_CHECK(hq_chan && lua_chan && garden_chan && garden_read_only);

  // The engine's own answers first, so a menu difference below can be traced to the right layer.
  CT_CHECK(ccord_store_can_act_on(F.store, CCORD_PERM_ACT_KICK, hq, self, ada) == CCORD_PERM_ALLOWED);
  CT_CHECK(ccord_store_can_act_on(F.store, CCORD_PERM_ACT_KICK, lua, self, chen) == CCORD_PERM_ALLOWED);
  CT_CHECK(ccord_store_can_act_on(F.store, CCORD_PERM_ACT_KICK, lua, self, bjorn) == CCORD_PERM_TARGET_HIGHER);
  CT_CHECK(ccord_store_can_act_on(F.store, CCORD_PERM_ACT_KICK, lua, self, ada) == CCORD_PERM_TARGET_OWNER);
  CT_CHECK(ccord_store_can_act_on(F.store, CCORD_PERM_ACT_KICK, garden, self, chen) == CCORD_PERM_MISSING);

  // ---- C-Cord HQ: administrator ----
  open_menu(CCORD_MENU_MEMBER, hq, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  CT_CHECK(F.ui->menu.open && menu_has("Kick Member") && menu_has("Ban Member") && menu_has("Timeout Member") &&
           menu_has("Change Nickname"));
  open_menu(CCORD_MENU_MEMBER, hq, CCORD_ID_NONE, self, CCORD_ID_NONE);   // yourself: a nickname, never a kick
  CT_CHECK(F.ui->menu.open && menu_has("Change Nickname") && !menu_has("Kick Member") && !menu_has("Ban Member") &&
           !menu_has("Timeout Member"));
  open_menu(CCORD_MENU_CHANNEL, hq, hq_chan, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_has("Edit Channel") && menu_has("Create Channel") && menu_has("Create Thread") && menu_has("Delete Channel"));
  open_menu(CCORD_MENU_GUILD, hq, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_has("Server Settings") && menu_has("Create Channel"));
  open_menu(CCORD_MENU_MESSAGE, hq, hq_chan, ada, 5);
  CT_CHECK(menu_has("Delete Message"));

  // ---- Lua Lounge: moderator under an owner ----
  open_menu(CCORD_MENU_MEMBER, lua, CCORD_ID_NONE, chen, CCORD_ID_NONE);
  CT_CHECK(F.ui->menu.open && menu_has("Kick Member") && menu_has("Ban Member") && menu_has("Timeout Member") &&
           menu_has("Change Nickname"));
  open_menu(CCORD_MENU_MEMBER, lua, CCORD_ID_NONE, bjorn, CCORD_ID_NONE);   // a peer: same highest role
  CT_CHECK(!menu_has("Kick Member") && !menu_has("Ban Member") && !menu_has("Timeout Member") && !menu_has("Change Nickname"));
  open_menu(CCORD_MENU_MEMBER, lua, CCORD_ID_NONE, ada, CCORD_ID_NONE);     // the owner: untouchable
  CT_CHECK(!menu_has("Kick Member") && !menu_has("Ban Member") && !menu_has("Timeout Member") && !menu_has("Change Nickname"));
  open_menu(CCORD_MENU_CHANNEL, lua, lua_chan, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_has("Create Thread") && !menu_has("Edit Channel") && !menu_has("Delete Channel") && !menu_has("Create Channel"));
  open_menu(CCORD_MENU_GUILD, lua, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_has("Server Settings") && !menu_has("Create Channel"));   // may ban, so the Bans page is theirs
  open_menu(CCORD_MENU_MESSAGE, lua, lua_chan, ada, 5);   // Manage Messages comes from the Moderators role
  CT_CHECK(menu_has("Delete Message"));

  // ---- Pixel Garden: ordinary member ----
  open_menu(CCORD_MENU_MEMBER, garden, CCORD_ID_NONE, chen, CCORD_ID_NONE);
  CT_CHECK(F.ui->menu.open && menu_has("Profile") && menu_has("Message (Open DM)") && menu_has("Report User") &&
           !menu_has("Kick Member") && !menu_has("Ban Member") && !menu_has("Timeout Member") &&
           !menu_has("Roles") && !menu_has("Open in Mod View"));
  open_menu(CCORD_MENU_MEMBER, garden, CCORD_ID_NONE, self, CCORD_ID_NONE);   // yourself: Change Nickname is everyone's right
  CT_CHECK(F.ui->menu.open && menu_has("Change Nickname") && !menu_has("Kick Member"));
  open_menu(CCORD_MENU_CHANNEL, garden, garden_chan, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_has("Create Thread") && !menu_has("Edit Channel") && !menu_has("Delete Channel") && !menu_has("Create Channel"));
  open_menu(CCORD_MENU_CHANNEL, garden, garden_read_only, CCORD_ID_NONE, CCORD_ID_NONE);   // the overwrite takes threads away
  CT_CHECK(!menu_has("Create Thread") && !menu_has("Edit Channel"));
  open_menu(CCORD_MENU_GUILD, garden, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(!menu_has("Server Settings") && !menu_has("Create Channel"));
  open_menu(CCORD_MENU_MESSAGE, garden, garden_chan, ada, 5);
  CT_CHECK(!menu_has("Delete Message") && menu_has("Copy Text"));
  ui_menu_close(F.ui);
  frame(1);
}

// Where the permission engine knows you cannot send, the composer shuts: it says why, takes no text and sends nothing.
// Where it only has a guess (no role data), it never locks you out.
static void test_composer_locks_where_sending_is_not_allowed(void)
{
  ccord_error err;
  ccord_id back_guild = ccord_store_selected_guild(F.store);
  ccord_id back_chan = ccord_store_selected_channel(F.store);
  const ccord_id self = 1001;
  const ccord_id hq = 2001, lua = 2002, garden = 2003;
  ui_cancel_reply(F.ui);   // earlier tests may have left a reply or an edit pending; it would answer Enter instead
  ui_cancel_edit(F.ui);
  ccord_id announcements = find_channel(hq, "announcements", CCORD_CHANNEL_TEXT);
  ccord_id showcase = find_channel(lua, "showcase", CCORD_CHANNEL_TEXT);
  ccord_id critique = find_channel(garden, "critique", CCORD_CHANNEL_TEXT);
  ccord_id art = find_channel(garden, "art", CCORD_CHANNEL_TEXT);
  CT_CHECK(announcements && showcase && critique && art);

  // Read-only for @everyone, but you are an administrator / hold the Moderators role the overwrite lets through.
  CT_CHECK(ccord_store_select_guild(F.store, hq, &err) == CCORD_OK && ccord_store_select_channel(F.store, announcements, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr);
  CT_CHECK(ccord_store_select_guild(F.store, lua, &err) == CCORD_OK && ccord_store_select_channel(F.store, showcase, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr);
  CT_CHECK(ccord_store_select_guild(F.store, garden, &err) == CCORD_OK && ccord_store_select_channel(F.store, art, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr);

  // An ordinary member of Pixel Garden meets the read-only channel.
  CT_CHECK(ccord_store_select_channel(F.store, critique, &err) == CCORD_OK);
  frame(2);
  const char *why = ui_composer_lock_reason(F.ui);
  CT_CHECK(why != nullptr && strstr(why, "permission") != nullptr);
  type_text("hello");
  frame(1);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');   // nothing was typed
  ccord_error ierr;
  CT_CHECK(ccord_text_edit_insert(&F.ui->edit, "stuck draft", 11, &ierr) == CCORD_OK);   // e.g. a draft restored earlier
  size_t before = ccord_store_message_count(F.store, critique);
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(ccord_store_message_count(F.store, critique) == before);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "stuck draft") == 0);   // not sent, not cleared
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);

  // A timeout shuts the box everywhere, with its own explanation; lifting it opens it again.
  CT_CHECK(ccord_store_select_channel(F.store, art, &err) == CCORD_OK);
  frame(1);
  ccord_member timed_out = {.guild_id = garden, .user_id = self, .role_name = "Members", .role_rank = 2,
                            .timeout_until_ms = F.ui->now_unix_ms + 3600000};
  CT_CHECK(ccord_store_upsert_guild_members(F.store, garden, &timed_out, 1, nullptr, 0, &err) == CCORD_OK);
  frame(1);
  why = ui_composer_lock_reason(F.ui);
  CT_CHECK(why != nullptr && strstr(why, "timed out") != nullptr);
  timed_out.timeout_until_ms = 0;
  CT_CHECK(ccord_store_upsert_guild_members(F.store, garden, &timed_out, 1, nullptr, 0, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr);

  // A thread is judged by "Send Messages in Threads", not "Send Messages": the parent may be read-only for talking while
  // its threads stay open.
  ccord_overwrite threads_only[1] = {{.id = garden, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_SEND_MESSAGES}};
  CT_CHECK(ccord_store_set_channel_overwrites(F.store, art, garden, threads_only, 1, &err) == CCORD_OK);
  ccord_channel thread = {.id = 9001, .guild_id = garden, .parent_id = art, .kind = CCORD_CHANNEL_TEXT,
                          .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD, .name = "side talk", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(F.store, &thread, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, art, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(ui_composer_lock_reason(F.ui) != nullptr);   // the channel itself: shut
  CT_CHECK(ccord_store_select_channel(F.store, 9001, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr);   // its thread: open
  CT_CHECK(ccord_store_set_channel_overwrites(F.store, art, garden, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, 9001, &err) == CCORD_OK);

  // A server whose roles we never received: no lock, because the answer would be a guess.
  ccord_guild unknown = {.id = 777, .name = "Unknown Roles"};
  ccord_channel unknown_chan = {.id = 778, .guild_id = 777, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = ""};
  CT_CHECK(ccord_store_upsert_guild(F.store, &unknown, &unknown_chan, 1, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_guild(F.store, 777, &err) == CCORD_OK && ccord_store_select_channel(F.store, 778, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(!ccord_store_permissions_exact(F.store, 777, self));
  CT_CHECK(ccord_store_permissions(F.store, 777, 778, self, 0) == 0);   // the guess is "nothing" ...
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr);                   // ... and it must not lock the box

  CT_CHECK(ccord_store_select_guild(F.store, back_guild, &err) == CCORD_OK);
  if (back_chan != CCORD_ID_NONE) CT_CHECK(ccord_store_select_channel(F.store, back_chan, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_guild(F.store, 777, &err) == CCORD_OK);
  frame(2);
}

static void test_smooth_scroll_hover_anim_loading_and_restore(void)
{
  ccord_error err = {0};

  // 1. Smooth scrolling (motion smoothing on wheel scrolls)
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  F.ui->msg_scroll_vel = 0.0f;
  frame(1);
  CT_CHECK(dbg().stick_bottom);

  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 6.0f, .x = l->message_area.x + 120.0f, .y = l->message_area.y + 120.0f});
  CT_CHECK(F.ui->msg_scroll_vel < 0.0f);
  float v0 = F.ui->msg_scroll_vel;
  frame(1);
  CT_CHECK(F.ui->msg_scroll_vel < 0.0f && F.ui->msg_scroll_vel > v0);   // velocity decays smoothly across frames
  frame(18);
  CT_CHECK(fabsf(F.ui->msg_scroll_vel) < 1.0f);
  F.ui->msg_scroll_vel = 0.0f;
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(1);

  // 2. Animated server icons (`a_` hash -> `.gif`) animate ONLY when hovered
  static const uint8_t k_rgb_gif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x08,
    0x05, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x15,
    0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1,
    0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08,
    0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08,
    0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00,
    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c,
    0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03,
    0x02, 0x00, 0x3b,
  };
  const ccord_guild *g1 = ccord_store_guild_at(F.store, 0);
  CT_CHECK(g1 != nullptr);
  ccord_guild anim_g = *g1;
  anim_g.icon_url = "a_serveranim99";
  CT_CHECK(ccord_store_upsert_guild(F.store, &anim_g, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);

  char icon_url[256] = {0};
  const ccord_guild *g1_after = ccord_store_guild(F.store, anim_g.id);
  CT_CHECK(ui_guild_icon_url(g1_after, icon_url, sizeof icon_url));
  CT_CHECK(strstr(icon_url, "/icons/") != nullptr && strstr(icon_url, "a_serveranim99.gif?size=64") != nullptr);

  CT_CHECK(ccord_render_upload_image(F.renderer, icon_url, k_rgb_gif, sizeof k_rgb_gif, nullptr, nullptr, &err) == CCORD_OK);
  size_t cur_frame = 99, frame_count = 0;
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, icon_url, &cur_frame, &frame_count));
  CT_CHECK(frame_count == 3);

  // Move mouse away from server list -> icon stays on frame 0 even across multiple frames
  mouse(CCORD_IN_MOUSE_MOVE, 0, 500.0f, 400.0f);
  frame(6);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, icon_url, &cur_frame, &frame_count));
  CT_CHECK(cur_frame == 0);

  // Hover over guild 0 (slot 1 in server list) -> icon animates past frame 0 as time advances
  ccord_rect g_ic = ui_guild_icon_rect(F.ui, 1);
  mouse(CCORD_IN_MOUSE_MOVE, 0, g_ic.x + g_ic.w * 0.5f, g_ic.y + g_ic.h * 0.5f);
  frame(6);   // 6 * 16ms = 96ms > 50ms delay of frame 0
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, icon_url, &cur_frame, &frame_count));
  CT_CHECK(cur_frame >= 1);

  // Unhover -> resets back to static frame 0
  mouse(CCORD_IN_MOUSE_MOVE, 0, 500.0f, 400.0f);
  frame(2);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, icon_url, &cur_frame, &frame_count));
  CT_CHECK(cur_frame == 0);

  // 3. Centered "C-Cord" loading screen with progress bar
  ccord_ui_set_loading_screen(F.ui, true);
  frame(3);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.loading_screen_open);
  CT_CHECK(d.loading_bar_rect.w > 100.0f && d.loading_bar_rect.h > 0.0f);
  float bar_center_x = d.loading_bar_rect.x + d.loading_bar_rect.w * 0.5f;
  CT_CHECK(fabsf(bar_center_x - F.ui->win_w * 0.5f) < 2.0f);
  CT_CHECK(d.loading_progress > 0.15f && d.loading_progress <= 1.0f);
  ccord_ui_set_loading_screen(F.ui, false);
  frame(1);
  CT_CHECK(!dbg().loading_screen_open);

  // 4. Persist & restore previous guild and channel across restarts
  const ccord_guild *g2 = ccord_store_guild_at(F.store, 1);
  CT_CHECK(g2 != nullptr);
  ccord_rect g2_ic = ui_guild_icon_rect(F.ui, 2);
  click(g2_ic.x + g2_ic.w * 0.5f, g2_ic.y + g2_ic.h * 0.5f);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == g2->id);

  // Find a second text channel in g2 to select so it's not just the default first channel
  ccord_id target_ch = ccord_store_selected_channel(F.store);
  size_t nch2 = ccord_store_channel_count(F.store, g2->id);
  for (size_t i = 0; i < nch2; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, g2->id, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->id != target_ch) {
      float cy = scan_hover_channel(200.0f, c->id);
      if (cy > 0.0f) {
        click(200.0f, cy);
        frame(2);
        target_ch = c->id;
        break;
      }
    }
  }
  CT_CHECK(ccord_store_selected_channel(F.store) == target_ch);
  CT_CHECK(F.ui->saved_guild_id == g2->id && F.ui->saved_channel_id == target_ch);

  // Simulate a fresh restart by selecting guild 0 first, then creating a new UI instance reading `ui_test_data/settings.ini`
  (void)ccord_session_select_guild(F.session, anim_g.id, &err);
  CT_CHECK(ccord_store_selected_guild(F.store) == anim_g.id);

  ccord_ui_deps deps2 = F.ui->d;
  ccord_ui *ui2 = ccord_ui_create(&deps2, &err);
  CT_CHECK(ui2 != nullptr);
  CT_CHECK(ccord_store_selected_guild(F.store) == g2->id);
  CT_CHECK(ccord_store_selected_channel(F.store) == target_ch);
  ccord_ui_destroy(ui2);
}

// Discord refuses writes: the call that started the action already returned OK, so the failure arrives later.
// The user must be told, and the text they wrote must come back instead of vanishing.
static void test_refused_writes_return_the_text(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 3000, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;  // the session now owns tr
  frame(8);
  ccord_id ch = ccord_store_selected_channel(F.store);
  CT_CHECK(ch != CCORD_ID_NONE && ccord_store_message_count(F.store, ch) > 0);
  // One accepted message of ours (the Edit menu is only offered on own messages), then Discord starts refusing.
  type_text("a message to edit later");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  size_t before = ccord_store_message_count(F.store, ch);
  ccord_mock_set_refuse_writes(tr, true);

  // 1. M0-3: a refused send fails its pending echo in place. The composer stays empty (the failed bubble keeps
  // the text for Retry), no draft-restore notice fires, and nothing was posted to Discord.
  type_text("keep me");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  frame(2);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  CT_CHECK(ccord_store_message_count(F.store, ch) == before + 1);   // the failed echo, not a posted message
  const ccord_message *failed = ccord_store_message_at(F.store, ch, before);
  CT_CHECK(failed && failed->send_state == CCORD_MSG_SEND_FAILED && strcmp(failed->content, "keep me") == 0 &&
           failed->nonce[0] != '\0');

  // 2. The user already started the next message: the refusal does not touch the composer, so only the new
  // draft survives there; the refused text lives on in its failed bubble.
  type_text("first");
  key(CCORD_KEY_RETURN, 0);
  type_text("second");
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "second") == 0);
  CT_CHECK(ccord_store_message_count(F.store, ch) == before + 2);
  const ccord_message *failed2 = ccord_store_message_at(F.store, ch, before + 1);
  CT_CHECK(failed2 && failed2->send_state == CCORD_MSG_SEND_FAILED && strcmp(failed2->content, "first") == 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);

  // 3. A refused edit puts the user back in edit mode on the same message with what they typed.
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float x = l->message_area.x + 300.0f;
  float y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  ccord_id msg = dbg().hover_message;
  const ccord_message *m0 = ccord_store_find_message(F.store, ch, msg);
  CT_CHECK(m0 != nullptr);
  char original[CCORD_MESSAGE_MAX_BYTES + 1];
  snprintf(original, sizeof original, "%s", m0 ? m0->content : "");
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  float ey = menu_item_center_y(y, dbg().context_menu_items, 2, 0);
  mouse(CCORD_IN_MOUSE_MOVE, 0, x + 20.0f, ey);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x + 20.0f, ey);
  frame(1);
  CT_CHECK(dbg().composer_editing);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("my edit");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(!dbg().composer_editing && ccord_ui_composer_text(F.ui)[0] == '\0');
  frame(2);
  CT_CHECK(dbg().composer_editing);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "my edit") == 0);
  CT_CHECK(strstr(dbg().notice, "Couldn't edit") != nullptr);
  const ccord_message *m1 = ccord_store_find_message(F.store, ch, msg);
  CT_CHECK(m1 && strcmp(m1->content, original) == 0 && !m1->edited);  // the message is unchanged

  // 4. Same, but the composer is busy by the time the refusal arrives: it is not overwritten, and the user is told.
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(!dbg().composer_editing);
  type_text("something else");
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "something else") == 0);
  CT_CHECK(strstr(dbg().notice, "could not be restored") != nullptr);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
  // The session owns `tr` now (the fixture's old transport is gone): later tests must neither inherit its refusals
  // nor poke the stale pointer.
  ccord_mock_set_refuse_writes(tr, false);
  F.transport = tr;
}

// M0-3: pending rows are dimmed with a clock/status line, failed rows are red and expose a Retry button,
// and Retry reuses the failed echo's nonce so the server's nonce-matched MESSAGE_CREATE confirms it in place.
static const ccord_message *find_by_nonce(ccord_id ch, const char *nonce)
{
  size_t n = ccord_store_message_count(F.store, ch);
  for (size_t i = 0; i < n; i++) {
    const ccord_message *m = ccord_store_message_at(F.store, ch, i);
    if (m && m->nonce[0] != '\0' && strcmp(m->nonce, nonce) == 0) return m;
  }
  return nullptr;
}

static bool has_reaction_hit_for(ccord_id message_id)
{
  for (size_t i = 0; i < F.ui->nreaction_hits; i++) {
    if (F.ui->reaction_hits[i].message_id == message_id) return true;
  }
  return false;
}

static void test_m0_pending_failed_retry_ui(void)
{
  draw_only();
  ccord_id ch = ccord_store_selected_channel(F.store);
  ccord_mock_set_refuse_writes(F.transport, false);
  // Earlier refusal tests leave their failed echoes in this channel; start from confirmed history only so the
  // drawn-status counts below belong to this test.
  for (size_t i = ccord_store_message_count(F.store, ch); i > 0; i--) {
    const ccord_message *m = ccord_store_message_at(F.store, ch, i - 1);
    if (m && m->send_state != CCORD_MSG_SEND_CONFIRMED) {
      ccord_error derr;
      (void)ccord_store_delete_message(F.store, ch, m->id, &derr);
    }
  }
  ccord_vlist_reset_bottom(&F.ui->vlist);
  draw_only();

  // Pending: draw without pumping, so the mock's MESSAGE_CREATE cannot confirm the echo yet.
  size_t before = ccord_store_message_count(F.store, ch);
  type_text("pending bubble");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  size_t after_send = ccord_store_message_count(F.store, ch);
  CT_CHECK(after_send == before + 1);
  const ccord_message *p = find_by_nonce(ch, ccord_store_message_at(F.store, ch, after_send - 1)->nonce);
  CT_CHECK(p && p->send_state == CCORD_MSG_SEND_PENDING && p->nonce[0] != '\0' &&
           strcmp(p->content, "pending bubble") == 0);
  char nonce[32];
  snprintf(nonce, sizeof nonce, "%s", p ? p->nonce : "");
  ccord_vlist_reset_bottom(&F.ui->vlist);
  draw_only();
  ccord_ui_debug d = dbg();
  CT_CHECK(d.pending_echoes_drawn == 1 && d.failed_echoes_drawn == 0 && d.retry_echo_count == 0);

  frame(3);
  p = find_by_nonce(ch, nonce);
  CT_CHECK(p && p->send_state == CCORD_MSG_SEND_CONFIRMED && strcmp(p->content, "pending bubble") == 0);
  CT_CHECK(ccord_store_message_count(F.store, ch) == after_send);   // replaced in place
  draw_only();
  d = dbg();
  CT_CHECK(d.pending_echoes_drawn == 0 && d.failed_echoes_drawn == 0 && d.retry_echo_count == 0);

  // Failed: Discord refuses, and the refusal marks exactly this echo.
  ccord_mock_set_refuse_writes(F.transport, true);
  before = ccord_store_message_count(F.store, ch);
  type_text("retry bubble");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  after_send = ccord_store_message_count(F.store, ch);
  p = ccord_store_message_at(F.store, ch, after_send - 1);
  CT_CHECK(after_send == before + 1 && p && p->send_state == CCORD_MSG_SEND_FAILED &&
           strcmp(p->content, "retry bubble") == 0);
  char failed_nonce[32];
  snprintf(failed_nonce, sizeof failed_nonce, "%s", p ? p->nonce : "");
  ccord_id failed_id = p ? p->id : CCORD_ID_NONE;
  ccord_vlist_reset_bottom(&F.ui->vlist);
  draw_only();
  d = dbg();
  CT_CHECK(d.failed_echoes_drawn == 1 && d.pending_echoes_drawn == 0);
  CT_CHECK(d.retry_echo_count == 1 && d.retry_echo_ids[0] == failed_id && d.retry_echo_rects[0].w > 0.0f);
  ccord_rect rb = d.retry_echo_rects[0];

  // An unconfirmed echo offers no server actions: hovering it draws no reply/pin/reaction bar, and a
  // right-click does not open the message menu.
  CT_CHECK(!dbg().context_menu_open);
  mouse(CCORD_IN_MOUSE_MOVE, 0, rb.x + rb.w * 0.5f, rb.y + rb.h * 0.5f);
  frame(2);   // the hover bar (if it were drawn) fades in over two frames
  CT_CHECK(dbg().hover_message == failed_id);
  CT_CHECK(F.ui->hover_bar_msg_id == CCORD_ID_NONE && !has_reaction_hit_for(failed_id));
  mouse(CCORD_IN_MOUSE_DOWN, 3, rb.x + 10.0f, rb.y + rb.h * 0.5f);
  CT_CHECK(!dbg().context_menu_open);
  mouse(CCORD_IN_MOUSE_UP, 3, rb.x + 10.0f, rb.y + rb.h * 0.5f);

  // Retry: the pill re-pends the bubble with its ORIGINAL nonce, then the server confirms it in place.
  ccord_mock_set_refuse_writes(F.transport, false);
  rb = dbg().retry_echo_rects[0];
  mouse(CCORD_IN_MOUSE_MOVE, 0, rb.x + rb.w * 0.5f, rb.y + rb.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, rb.x + rb.w * 0.5f, rb.y + rb.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 1, rb.x + rb.w * 0.5f, rb.y + rb.h * 0.5f);
  p = find_by_nonce(ch, failed_nonce);
  CT_CHECK(p && p->send_state == CCORD_MSG_SEND_PENDING && p->id == failed_id);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  draw_only();
  d = dbg();
  CT_CHECK(d.pending_echoes_drawn == 1 && d.failed_echoes_drawn == 0 && d.retry_echo_count == 0);
  frame(3);
  p = find_by_nonce(ch, failed_nonce);
  CT_CHECK(p && p->send_state == CCORD_MSG_SEND_CONFIRMED && strcmp(p->content, "retry bubble") == 0);
  CT_CHECK(ccord_store_message_count(F.store, ch) == after_send);
  draw_only();
  d = dbg();
  CT_CHECK(d.pending_echoes_drawn == 0 && d.failed_echoes_drawn == 0 && d.retry_echo_count == 0);
  ccord_mock_set_refuse_writes(F.transport, false);
}

static const ccord_member *member_of(ccord_id guild, ccord_id user)
{
  for (size_t i = 0; i < ccord_store_member_count(F.store, guild); i++) {
    const ccord_member *m = ccord_store_member_at(F.store, guild, i);
    if (m && m->user_id == user) return m;
  }
  return nullptr;
}

static ccord_id other_member(ccord_id guild, size_t skip)
{
  ccord_id self = ccord_store_self_id(F.store);
  for (size_t i = 0; i < ccord_store_member_count(F.store, guild); i++) {
    const ccord_member *m = ccord_store_member_at(F.store, guild, i);
    if (m && m->user_id != self && skip-- == 0) return m->user_id;
  }
  return CCORD_ID_NONE;
}

// Server administration from the UI. Asking is not doing: a dialog changes nothing until the user confirms; the
// confirmed call goes to Discord exactly as chosen; and nothing in the store moves until Discord has answered.
static void test_admin_dialogs_call_discord(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  ccord_guild admin_g = *ccord_store_guild(F.store, g);
  admin_g.owner_id = ccord_store_self_id(F.store);
  admin_g.self_permissions = UINT64_MAX;
  CT_CHECK(ccord_store_upsert_guild(F.store, &admin_g, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  frame(2);
  ccord_id v1 = other_member(g, 0), v2 = other_member(g, 1), v3 = other_member(g, 2), v4 = other_member(g, 3);
  CT_CHECK(v1 && v2 && v3 && v4);
  size_t members = ccord_store_member_count(F.store, g);
  CT_CHECK(ccord_mock_admin_count(tr) == 0);
  // These menus count on the built-in Copy User ID row, so Developer Mode is on for the member sections below.
  seed_user_settings(true, true, 0);

  // 1. Kick through the member menu: choosing the item only opens the dialog.
  ui_menu_open(F.ui, 400.0f, 300.0f, &(ccord_menu_context){CCORD_MENU_MEMBER, g, 0, 0, v1});
  CT_CHECK(dbg().context_menu_open && menu_has("Profile") && menu_has("Kick Member") &&
           menu_has("Copy User ID") && menu_has("Report User"));
  ccord_rect kick_rc = {0};
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Kick Member", &kick_rc));
  mouse(CCORD_IN_MOUSE_MOVE, 0, kick_rc.x + kick_rc.w * 0.5f, kick_rc.y + kick_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, kick_rc.x + kick_rc.w * 0.5f, kick_rc.y + kick_rc.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_KICK);
  CT_CHECK(ccord_mock_admin_count(tr) == 0);
  key(CCORD_KEY_ESCAPE, 0);   // cancel
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE && ccord_mock_admin_count(tr) == 0 && member_of(g, v1) != nullptr);

  ui_menu_open(F.ui, 400.0f, 300.0f, &(ccord_menu_context){CCORD_MENU_MEMBER, g, 0, 0, v1});
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Kick Member", &kick_rc));
  mouse(CCORD_IN_MOUSE_MOVE, 0, kick_rc.x + kick_rc.w * 0.5f, kick_rc.y + kick_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, kick_rc.x + kick_rc.w * 0.5f, kick_rc.y + kick_rc.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM);
  type_text("spam");   // the optional audit-log reason
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);
  CT_CHECK(ccord_mock_admin_count(tr) == 1);
  const ccord_admin_request *last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_KICK && last->guild_id == g && last->user_id == v1);
  CT_CHECK(last && strcmp(last->reason, "spam") == 0);
  CT_CHECK(member_of(g, v1) != nullptr);   // not removed on our say-so
  frame(2);
  CT_CHECK(member_of(g, v1) == nullptr && ccord_store_member_count(F.store, g) == members - 1);  // removed when the server confirmed

  // 2. Timeout: the default is 10 minutes, the member shows as timed out once Discord says so, and the menu then
  //    offers "Remove Timeout" instead of "Timeout Member".
  ui_confirm_open(F.ui, CCORD_ADMIN_TIMEOUT, g, v2, "x");
  key(CCORD_KEY_RETURN, 0);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 2 && last && last->op == CCORD_ADMIN_TIMEOUT);
  CT_CHECK(last && last->until_ms == F.ui->now_unix_ms + 10LL * 60 * 1000);
  CT_CHECK(member_of(g, v2)->timeout_until_ms == 0);
  frame(2);
  CT_CHECK(member_of(g, v2)->timeout_until_ms == F.ui->now_unix_ms + 10LL * 60 * 1000);
  ui_menu_open(F.ui, 400.0f, 300.0f, &(ccord_menu_context){CCORD_MENU_MEMBER, g, 0, 0, v2});
  CT_CHECK(dbg().context_menu_open && menu_has("Remove Timeout") && !menu_has("Timeout Member"));
  ccord_rect rt_rc = {0};
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Remove Timeout", &rt_rc));
  mouse(CCORD_IN_MOUSE_MOVE, 0, rt_rc.x + rt_rc.w * 0.5f, rt_rc.y + rt_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, rt_rc.x + rt_rc.w * 0.5f, rt_rc.y + rt_rc.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);   // lifting a timeout asks nothing
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 3 && last && last->op == CCORD_ADMIN_TIMEOUT && last->until_ms == 0);
  frame(2);
  CT_CHECK(member_of(g, v2)->timeout_until_ms == 0);

  // 3. The duration chips: Right/Left move through the choices and stop at the ends.
  ui_confirm_open(F.ui, CCORD_ADMIN_TIMEOUT, g, v3, "x");
  key(CCORD_KEY_RIGHT, 0);
  key(CCORD_KEY_RIGHT, 0);
  for (int i = 0; i < 4; i++) key(CCORD_KEY_RIGHT, 0);   // past the end: stays on "1 week"
  key(CCORD_KEY_RETURN, 0);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->until_ms == F.ui->now_unix_ms + 7LL * 24 * 60 * 60 * 1000);
  ui_confirm_open(F.ui, CCORD_ADMIN_TIMEOUT, g, v3, "x");
  for (int i = 0; i < 6; i++) key(CCORD_KEY_LEFT, 0);   // past the start: stays on "60 secs"
  key(CCORD_KEY_RETURN, 0);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->until_ms == F.ui->now_unix_ms + 60LL * 1000);
  frame(2);

  // 4. Ban with "delete 24 hours of messages".
  ui_confirm_open(F.ui, CCORD_ADMIN_BAN, g, v4, "x");
  key(CCORD_KEY_RIGHT, 0);
  key(CCORD_KEY_RIGHT, 0);
  key(CCORD_KEY_RETURN, 0);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_BAN && last->user_id == v4);
  CT_CHECK(last && strstr(last->body, "\"delete_message_seconds\":86400") != nullptr);
  frame(2);
  CT_CHECK(member_of(g, v4) == nullptr);

  // 5. Nobody gets kicked, banned or timed out by accident: yourself and the owner are refused before anything is sent.
  size_t sent = ccord_mock_admin_count(tr);
  ui_confirm_open(F.ui, CCORD_ADMIN_KICK, g, ccord_store_self_id(F.store), "me");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_mock_admin_count(tr) == sent);
  CT_CHECK(strstr(dbg().notice, "Couldn't kick") != nullptr && strstr(dbg().notice, "yourself") != nullptr);

  // 6. Delete channel: confirm dialog, then a real call; a refusal leaves the channel and says why.
  seed_user_settings(false, true, 0);   // channel menus below keep their normal rows
  ccord_id dev = find_channel(g, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(dev != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  frame(3);
  float dev_y = scan_hover_channel(200.0f, dev);
  CT_CHECK(dev_y > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dev_y);
  frame(1);
  CT_CHECK(dbg().context_menu_open && dbg().context_menu_items == 8);
  CT_CHECK(menu_click_label("Delete Channel"));
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_DELETE_CHANNEL);
  CT_CHECK(ccord_mock_admin_count(tr) == sent);
  ccord_mock_set_refuse_writes(tr, true);
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_mock_admin_count(tr) == sent + 1);
  frame(3);
  CT_CHECK(ccord_store_channel(F.store, dev) != nullptr);   // refused: still there
  CT_CHECK(strstr(dbg().notice, "Couldn't delete the channel") != nullptr);
  CT_CHECK(strstr(dbg().notice, "Missing Permissions") != nullptr);   // Discord's words, not ours
  ccord_mock_set_refuse_writes(tr, false);

  // 7. Closing a DM goes through Discord too: the conversation stays in the list until it confirms.
  ccord_id dm_guild = CCORD_DM_GUILD_ID;
  ccord_id friend_id = other_member(g, 0);
  CT_CHECK(ccord_session_open_dm(F.session, friend_id, &err) == CCORD_OK);
  frame(3);
  ccord_id dm_id = 900000u + friend_id;
  CT_CHECK(ccord_store_channel(F.store, dm_id) != nullptr);
  size_t dms = ccord_store_channel_count(F.store, dm_guild);
  size_t before_close = ccord_mock_admin_count(tr);
  ui_close_dm(F.ui, dm_id);
  CT_CHECK(ccord_mock_admin_count(tr) == before_close + 1);
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_CLOSE_DM && ccord_mock_last_admin(tr)->channel_id == dm_id);
  CT_CHECK(ccord_store_channel_count(F.store, dm_guild) == dms);
  frame(2);
  CT_CHECK(ccord_store_channel_count(F.store, dm_guild) == dms - 1 && ccord_store_channel(F.store, dm_id) == nullptr);
}

// ---------------------------------------------------------------- Server Settings overlay

// Opt-in (CCORD_SHOT_DIR=<dir>): saves what the overlay looks like at key moments, for eyeballing. Asserts nothing.
static ccord_ui_server_settings_debug ssd(void)
{
  ccord_ui_server_settings_debug d;
  ccord_ui_get_server_settings_debug(F.ui, &d);
  return d;
}

static bool ss_has(const char *name)
{
  return ccord_ui_server_settings_find(F.ui, name, nullptr);
}

// Clicks the centre of a named control of the overlay; false when it is not on screen.
static bool ss_click(const char *name)
{
  ccord_rect r;
  if (!ccord_ui_server_settings_find(F.ui, name, &r)) return false;
  click(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  return true;
}

// Like ss_click, but without letting any time pass afterwards (the mock answers within a frame or two).
static bool ss_press(const char *name)
{
  ccord_rect r;
  if (!ccord_ui_server_settings_find(F.ui, name, &r)) return false;
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  return true;
}

// Scrolls the editor until the named control is on screen; false when it never appears.
static bool ss_scroll_to(const char *name)
{
  const float x = F.ui->win_w - 220.0f, y = F.ui->win_h * 0.5f;   // inside the editor, whatever is scrolled off
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = x, .y = y, .wheel = 1000.0f});   // back to the top
  frame(1);
  for (int i = 0; i < 120 && !ss_has(name); i++) {
    send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = x, .y = y, .wheel = -3.0f});
    frame(1);
  }
  return ss_has(name);
}

static void clear_focused_text(void)
{
  for (int i = 0; i < 60; i++) key(CCORD_KEY_BACKSPACE, 0);
}

static const ccord_role *role_named(ccord_id guild, const char *name)
{
  size_t n = 0;
  const ccord_role *r = ccord_store_roles(F.store, guild, &n);
  for (size_t i = 0; r && i < n; i++) {
    if (r[i].name && strcmp(r[i].name, name) == 0) return &r[i];
  }
  return nullptr;
}

// Who may open Server Settings, and what the overlay offers, follows the permission engine; the title page of a
// moderator is Bans, not an empty Overview.
static void test_server_settings_gating_and_overview(ccord_transport *tr)
{
  const ccord_id hq = 2001, lua = 2002, garden = 2003;
  CT_CHECK(!ccord_ui_open_server_settings(F.ui, garden));   // an ordinary member has nothing to manage
  CT_CHECK(!ssd().open);
  CT_CHECK(!ccord_ui_open_server_settings(F.ui, CCORD_DM_GUILD_ID) && !ccord_ui_open_server_settings(F.ui, 99999));

  // Lua Lounge: a moderator may ban, nothing else here.
  size_t before = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, lua));
  frame(3);
  CT_CHECK(ssd().open && ssd().guild_id == lua && ssd().page == 3);
  shot("lua-bans");
  CT_CHECK(ss_has("page:bans") && !ss_has("page:overview") && !ss_has("page:roles") && !ss_has("page:invites") &&
           !ss_has("page:audit"));
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_BANS);
  frame(20);   // drawing and idling never ask again
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!ssd().open);

  // C-Cord HQ: administrator, every page.
  before = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(3);
  CT_CHECK(ssd().open && ssd().guild_id == hq && ssd().page == 0);
  CT_CHECK(ss_has("page:overview") && ss_has("page:roles") && ss_has("page:invites") && ss_has("page:bans") &&
           ss_has("page:audit") && ss_has("close"));
  CT_CHECK(ccord_mock_admin_count(tr) == before);   // Overview needs no list
  CT_CHECK(!ssd().unsaved && !ss_has("save"));

  // Renaming: typing alone changes nothing on Discord; Save sends exactly one request; the name changes when it answers.
  const char *old_name = ccord_store_guild(F.store, hq)->name;
  char saved_old[96];
  snprintf(saved_old, sizeof saved_old, "%s", old_name);
  CT_CHECK(ss_click("name") && ssd().focus == 1);
  type_text(" Reborn");
  CT_CHECK(ssd().unsaved);
  frame(1);
  shot("overview-unsaved");
  CT_CHECK(ss_has("save") && ss_has("reset"));
  CT_CHECK(ccord_mock_admin_count(tr) == before);
  CT_CHECK(strcmp(ccord_store_guild(F.store, hq)->name, saved_old) == 0);

  // Reset puts the field back without bothering Discord.
  CT_CHECK(ss_click("reset"));
  CT_CHECK(!ssd().unsaved && ccord_mock_admin_count(tr) == before);

  // While changes are pending, leaving is refused and says why; Escape leaves the field first.
  CT_CHECK(ss_click("name"));
  type_text("!");
  CT_CHECK(ssd().unsaved);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(ssd().open && ssd().focus == 0);
  CT_CHECK(ss_click("page:roles"));
  CT_CHECK(ssd().page == 0 && strstr(dbg().notice, "unsaved") != nullptr);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(ssd().open);
  CT_CHECK(ss_click("close"));
  CT_CHECK(ssd().open);

  // Save: one request however hard the button is pressed.
  ccord_rect save_rc;
  CT_CHECK(ccord_ui_server_settings_find(F.ui, "save", &save_rc));
  for (int i = 0; i < 3; i++) {   // three presses before any frame passes
    mouse(CCORD_IN_MOUSE_MOVE, 0, save_rc.x + 4.0f, save_rc.y + 4.0f);
    mouse(CCORD_IN_MOUSE_DOWN, 1, save_rc.x + 4.0f, save_rc.y + 4.0f);
    mouse(CCORD_IN_MOUSE_UP, 1, save_rc.x + 4.0f, save_rc.y + 4.0f);
  }
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1);
  const ccord_admin_request *last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_EDIT_GUILD && last->guild_id == hq);
  CT_CHECK(last && strstr(last->body, "\"name\":\"") != nullptr && strstr(last->body, "!\"") != nullptr);
  frame(3);
  CT_CHECK(!ssd().unsaved);
  char want[128];
  snprintf(want, sizeof want, "%s!", saved_old);
  CT_CHECK(strcmp(ccord_store_guild(F.store, hq)->name, want) == 0);   // Discord's answer, not ours

  // Put the name back (also checks the field follows the store when clean). A second save within the press guard
  // (0.8 s) of the first would be ignored, which no human edit-and-save can reach.
  frame(20);
  CT_CHECK(ss_click("name"));
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
  CT_CHECK(ssd().unsaved && ss_click("save"));
  frame(3);
  CT_CHECK(strcmp(ccord_store_guild(F.store, hq)->name, saved_old) == 0 && !ssd().unsaved);
}

static void test_server_settings_roles(ccord_transport *tr)
{
  const ccord_id hq = 2001, chen = 1004;
  CT_CHECK(ssd().open && ssd().guild_id == hq);
  size_t nperm = 0;
  const ccord_perm_info *ptbl = ccord_perm_table(&nperm);
  CT_CHECK(ptbl && nperm > 40);
  char last_name[32];
  snprintf(last_name, sizeof last_name, "perm:%llx", (unsigned long long)ptbl[nperm - 1].bit);
  CT_CHECK(ss_click("page:roles") && ssd().page == 1);
  CT_CHECK(ssd().role_id == hq);   // @everyone, which always exists
  CT_CHECK(ss_has("role:5003") && ss_has("role:5002") && ss_has("role:5001"));
  size_t before = ccord_mock_admin_count(tr);
  size_t roles_before = 0;
  (void)ccord_store_roles(F.store, hq, &roles_before);

  // @everyone has permissions only: no name, colour or delete.
  CT_CHECK(ss_click("role:2001") || true);
  CT_CHECK(!ss_has("role-delete"));

  // Create: asks Discord, then selects what Discord made.
  CT_CHECK(ss_click("role-create"));
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_CREATE_ROLE);
  frame(4);
  size_t roles_after = 0;
  (void)ccord_store_roles(F.store, hq, &roles_after);
  CT_CHECK(roles_after == roles_before + 1);
  ccord_id made = ssd().role_id;
  CT_CHECK(made >= 6000 && made != hq && ssd().role_tab == 0 && !ssd().unsaved);
  CT_CHECK(ss_has("role-name") && ss_has("role-delete") && ss_has("role-color"));
  shot("roles-display");

  // Rename + one colour + one permission: nothing sent until Save, and Save sends only what changed.
  CT_CHECK(ss_click("role-name") && ssd().focus == 2);
  clear_focused_text();
  type_text("Artists");
  CT_CHECK(ssd().unsaved);
  CT_CHECK(ss_click("color:E91E63") && ssd().work_color == 0xE91E63u);
  CT_CHECK(ss_click("tab:perms") && ssd().role_tab == 1);
  shot("roles-perms-unsaved");
  uint64_t perms0 = ssd().work_permissions;
  uint64_t first_bit = 0;   // the first switch on screen that is off (new roles start with @everyone's permissions)
  char first_name[32] = "";
  for (size_t i = 0; i < nperm && !first_bit; i++) {
    snprintf(first_name, sizeof first_name, "perm:%llx", (unsigned long long)ptbl[i].bit);
    if ((perms0 & ptbl[i].bit) == 0 && ss_has(first_name)) first_bit = ptbl[i].bit;
  }
  CT_CHECK(first_bit != 0);
  CT_CHECK(ss_click(first_name));
  CT_CHECK((ssd().work_permissions & first_bit) != 0);
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1);

  // Pending changes block leaving the page, and selecting another role.
  CT_CHECK(ss_click("role:5003"));
  CT_CHECK(ssd().role_id == made && strstr(dbg().notice, "unsaved") != nullptr);
  CT_CHECK(ss_click("page:bans") && ssd().page == 1);

  // Reset returns to Discord's copy...
  CT_CHECK(ss_click("reset"));
  CT_CHECK(!ssd().unsaved && ssd().work_permissions == perms0 && ssd().work_color == 0);
  // ...and Save sends the diff.
  CT_CHECK(ss_click(first_name) && ss_click("tab:display") && ss_click("role-name"));
  clear_focused_text();
  type_text("Artists");
  CT_CHECK(ss_click("color:E91E63"));
  CT_CHECK(ss_click("save"));
  CT_CHECK(ccord_mock_admin_count(tr) == before + 2);
  const ccord_admin_request *last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_EDIT_ROLE && last->role_id == made);
  CT_CHECK(last && strstr(last->body, "\"name\":\"Artists\"") != nullptr && strstr(last->body, "\"color\":") != nullptr &&
           strstr(last->body, "\"permissions\"") != nullptr && strstr(last->body, "hoist") == nullptr);
  frame(4);
  const ccord_role *art = ccord_store_role(F.store, hq, made);
  CT_CHECK(art && strcmp(art->name, "Artists") == 0 && art->color == 0xE91E63u && (art->permissions & first_bit));
  CT_CHECK(!ssd().unsaved);

  // The colour field takes #RRGGBB too.
  CT_CHECK(ss_click("role-color"));
  clear_focused_text();
  type_text("#3498db");
  frame(1);
  CT_CHECK(ssd().work_color == 0x3498DBu && ssd().unsaved);
  CT_CHECK(ss_click("reset") && ssd().work_color == 0xE91E63u);

  // Somebody else edits the role while you have changes pending: yours stay, the rest follows Discord, and Save sends
  // only yours (it must not put their colour back).
  CT_CHECK(ss_click("role-name"));
  clear_focused_text();
  type_text("Artists 2");
  frame(1);
  CT_CHECK(ssd().unsaved && ssd().work_color == 0xE91E63u);
  {
    ccord_role other = *ccord_store_role(F.store, hq, made);
    char other_name[64];
    snprintf(other_name, sizeof other_name, "%s", other.name);
    other.name = other_name;
    other.guild_id = hq;
    other.color = 0x2ECC71u;
    ccord_error uerr;
    CT_CHECK(ccord_store_upsert_role(F.store, &other, &uerr) == CCORD_OK);
  }
  frame(2);
  CT_CHECK(ssd().unsaved && ssd().work_color == 0x2ECC71u);   // their colour shows, our name edit is still ours
  CT_CHECK(ss_click("save"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_EDIT_ROLE && strstr(last->body, "\"name\":\"Artists 2\"") != nullptr &&
           strstr(last->body, "color") == nullptr && strstr(last->body, "permissions") == nullptr);
  frame(4);
  CT_CHECK(!ssd().unsaved && strcmp(ccord_store_role(F.store, hq, made)->name, "Artists 2") == 0);   // (the mock never knew their colour)

  // Their permission change and rename arrive while you have flipped a different switch: both are kept, and the name
  // you did not touch follows theirs.
  frame(20);
  CT_CHECK(ss_click("tab:perms"));
  CT_CHECK(ss_scroll_to(first_name));
  CT_CHECK(ss_click(first_name) && ssd().unsaved);
  {
    const ccord_role *cur = ccord_store_role(F.store, hq, made);
    ccord_role other = *cur;
    char other_name[64];
    snprintf(other_name, sizeof other_name, "Renamed By Them");
    other.name = other_name;
    other.guild_id = hq;
    other.permissions = cur->permissions ^ CCORD_PERM_MANAGE_MESSAGES;
    uint64_t want = other.permissions ^ first_bit;   // theirs, with our flip on top
    ccord_error uerr;
    CT_CHECK(ccord_store_upsert_role(F.store, &other, &uerr) == CCORD_OK);
    frame(2);
    CT_CHECK(ssd().unsaved && ssd().work_permissions == want);
    CT_CHECK(strcmp(ssd().role_name, "Renamed By Them") == 0);
    CT_CHECK(ss_click("reset") && !ssd().unsaved && ssd().work_permissions == other.permissions);
  }

  // Only what changed is sent: one permission flip sends the permissions and nothing else; the two display switches
  // are sent when, and only when, they were flipped.
  frame(20);   // past the press guard
  CT_CHECK(ss_click(first_name) && ssd().unsaved && ss_click("save"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_EDIT_ROLE && strstr(last->body, "permissions") != nullptr &&
           strstr(last->body, "name") == nullptr && strstr(last->body, "color") == nullptr &&
           strstr(last->body, "hoist") == nullptr && strstr(last->body, "mentionable") == nullptr);
  frame(4);
  CT_CHECK(!ssd().unsaved);
  frame(20);
  CT_CHECK(ss_click("tab:display") && ss_click("hoist") && ssd().unsaved && ss_click("save"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && strstr(last->body, "\"hoist\":true") != nullptr && strstr(last->body, "permissions") == nullptr &&
           strstr(last->body, "name") == nullptr && strstr(last->body, "mentionable") == nullptr);
  frame(4);
  CT_CHECK(ccord_store_role(F.store, hq, made)->hoist && !ssd().unsaved);
  frame(20);
  CT_CHECK(ss_click("mention") && ssd().unsaved && ss_click("save"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && strstr(last->body, "\"mentionable\":true") != nullptr && strstr(last->body, "hoist") == nullptr);
  frame(4);
  CT_CHECK(ccord_store_role(F.store, hq, made)->mentionable && !ssd().unsaved);

  // Reorder: up asks Discord to move it, and the list follows the answer.
  int32_t pos0 = ccord_store_role(F.store, hq, made)->position;
  CT_CHECK(ss_press("role-up"));
  CT_CHECK(ccord_mock_admin_count(tr) == before + 7 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_REORDER_ROLES);   // create, 5 saves, this
  CT_CHECK(ccord_store_role(F.store, hq, made)->position == pos0);   // not on our say-so
  frame(4);
  CT_CHECK(ccord_store_role(F.store, hq, made)->position > pos0);
  CT_CHECK(ss_click("role-down"));
  frame(4);
  CT_CHECK(ccord_store_role(F.store, hq, made)->position == pos0);

  // The long permission list scrolls with the wheel: the last switch is off screen until then.
  CT_CHECK(ss_click("tab:perms"));
  CT_CHECK(!ss_has(last_name));
  ccord_rect view = {0};
  CT_CHECK(ccord_ui_server_settings_find(F.ui, first_name, &view));
  for (int i = 0; i < 80 && !ss_has(last_name); i++) {
    send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = view.x - 20.0f, .y = view.y, .wheel = -3.0f});
    frame(1);
  }
  CT_CHECK(ss_has(last_name) && ssd().scroll > 0.0f);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = view.x - 20.0f, .y = view.y, .wheel = 500.0f});
  frame(2);
  CT_CHECK(ssd().scroll == 0.0f);   // never above the top

  // Members tab: giving the role to somebody is one request each, and shows once Discord answers.
  CT_CHECK(ss_click("tab:members") && ssd().role_tab == 2);
  size_t n_before = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("member:1004"));
  CT_CHECK(ccord_mock_admin_count(tr) == n_before + 1);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_ADD_MEMBER_ROLE && last->user_id == chen && last->role_id == made);
  frame(4);
  const ccord_id *ids = nullptr;
  size_t idn = 0;
  bool has = false;
  CT_CHECK(ccord_store_member_roles(F.store, hq, chen, &ids, &idn));
  for (size_t i = 0; i < idn; i++) has = has || ids[i] == made;
  CT_CHECK(has);
  CT_CHECK(ss_click("member:1004"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_REMOVE_MEMBER_ROLE && last->user_id == chen);
  frame(4);

  // Delete: a confirm dialog first (Escape backs out), then the real call; selection falls back to @everyone.
  size_t d_before = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("tab:display") && ss_click("role-delete"));
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_DELETE_ROLE);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE && ccord_mock_admin_count(tr) == d_before && ccord_store_role(F.store, hq, made));
  CT_CHECK(ssd().open);
  CT_CHECK(ss_click("role-delete"));
  type_text("cleanup");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_mock_admin_count(tr) == d_before + 1);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_DELETE_ROLE && last->role_id == made && strcmp(last->reason, "cleanup") == 0);
  frame(4);
  CT_CHECK(ccord_store_role(F.store, hq, made) == nullptr && ssd().role_id == hq);
  CT_CHECK(role_named(hq, "Artists") == nullptr);
}

// A frame that lets time pass without reading the transport: nothing can answer in it.
static void frame_deaf(uint64_t jump_ms)
{
  F.now += jump_ms;
  (void)ccord_ui_update(F.ui, F.now);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_render_end(F.renderer);
}

static void test_server_settings_lists(ccord_transport *tr)
{
  const ccord_id hq = 2001;
  CT_CHECK(ssd().open && ssd().guild_id == hq);
  size_t base = ccord_mock_admin_count(tr);
  const ccord_admin_request *last = nullptr;

  // ---- Invites: one request when the page opens, then the list is reused.
  CT_CHECK(ss_click("page:invites") && ssd().page == 2);
  CT_CHECK(ccord_mock_admin_count(tr) == base + 1 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_INVITES);
  frame(4);
  CT_CHECK(ccord_store_invites_loaded(F.store, hq) && ssd().rows == 2);
  CT_CHECK(ss_has("invite-copy:cCordHQ") && ss_has("invite-revoke:weekend") && ss_has("invite-create"));
  shot("invites");
  frame(30);   // idling and drawing never ask again
  CT_CHECK(ccord_mock_admin_count(tr) == base + 1);

  F.clip[0] = '\0';
  CT_CHECK(ss_click("invite-copy:cCordHQ"));
  CT_CHECK(strcmp(F.clip, "https://discord.gg/cCordHQ") == 0 && ccord_mock_admin_count(tr) == base + 1);

  // Refresh is no hammer: presses inside 2 s of the last request are ignored, later ones go through.
  CT_CHECK(ss_press("refresh") && ss_press("refresh") && ss_press("refresh"));
  CT_CHECK(ccord_mock_admin_count(tr) == base + 1);
  frame(50);
  CT_CHECK(ss_press("refresh") && ss_press("refresh"));
  CT_CHECK(ccord_mock_admin_count(tr) == base + 2 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_INVITES);
  frame(4);

  // Another page costs one request of its own; coming back inside 30 s costs none.
  CT_CHECK(ss_click("page:bans") && ccord_mock_admin_count(tr) == base + 3);
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_BANS);
  frame(4);
  CT_CHECK(ss_click("page:invites") && ccord_mock_admin_count(tr) == base + 3 && ssd().rows == 2);
  size_t c = ccord_mock_admin_count(tr);

  // Create: the chips are local, the button sends one request with what they say.
  CT_CHECK(ss_click("invite-age:604800") && ss_click("invite-uses:10"));
  CT_CHECK(ccord_mock_admin_count(tr) == c);
  CT_CHECK(ss_click("invite-create"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == c + 1 && last && last->op == CCORD_ADMIN_CREATE_INVITE);
  CT_CHECK(last && last->invite_max_age_s == 604800u && last->invite_max_uses == 10u && last->guild_id == hq);
  const ccord_channel *target = last ? ccord_store_channel(F.store, last->channel_id) : nullptr;
  CT_CHECK(target && target->guild_id == hq && target->kind == CCORD_CHANNEL_TEXT);
  frame(4);
  CT_CHECK(ssd().rows == 3);
  size_t ni = 0;
  const ccord_invite *invs = ccord_store_invites(F.store, hq, &ni);
  char code[40] = "";
  for (size_t i = 0; invs && i < ni; i++) {
    if (invs[i].max_uses == 10 && strcmp(invs[i].code, "weekend") != 0) snprintf(code, sizeof code, "%s", invs[i].code);
  }
  CT_CHECK(code[0] != '\0');
  char copy_name[64], revoke_name[64], link[96];
  snprintf(copy_name, sizeof copy_name, "invite-copy:%s", code);
  snprintf(revoke_name, sizeof revoke_name, "invite-revoke:%s", code);
  snprintf(link, sizeof link, "https://discord.gg/%s", code);
  CT_CHECK(ss_click(copy_name) && strcmp(F.clip, link) == 0);

  // Revoke: a confirm dialog first (Escape backs out), then the real call; the row goes when Discord says so.
  c = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click(revoke_name));
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_REVOKE_INVITE);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE && ccord_mock_admin_count(tr) == c && ssd().rows == 3 && ssd().open);
  CT_CHECK(ss_click(revoke_name));
  key(CCORD_KEY_RETURN, 0);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == c + 1 && last && last->op == CCORD_ADMIN_REVOKE_INVITE && strcmp(last->code, code) == 0);
  CT_CHECK(ssd().rows == 3);
  frame(4);
  CT_CHECK(ssd().rows == 2 && !ss_has(revoke_name));

  // ---- Bans: searching is local; Revoke Ban is one request and the row goes when Discord answers.
  CT_CHECK(ss_click("page:bans") && ssd().page == 3);
  frame(2);
  c = ccord_mock_admin_count(tr);
  CT_CHECK(ssd().rows == 2 && ss_has("unban:7001") && ss_has("unban:7002"));
  shot("bans");
  CT_CHECK(ss_click("ban-search") && ssd().focus == 4);
  type_text("troll");
  frame(2);
  CT_CHECK(ssd().rows == 1 && ss_has("unban:7002") && !ss_has("unban:7001"));
  clear_focused_text();
  type_text("7001");   // by id, too
  frame(2);
  CT_CHECK(ssd().rows == 1 && ss_has("unban:7001") && !ss_has("unban:7002"));
  clear_focused_text();
  type_text("nobody");
  frame(2);
  CT_CHECK(ssd().rows == 0);
  clear_focused_text();
  frame(2);
  CT_CHECK(ssd().rows == 2 && ccord_mock_admin_count(tr) == c);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(ssd().focus == 0 && ssd().open);
  CT_CHECK(ss_click("unban:7001"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == c + 1 && last && last->op == CCORD_ADMIN_UNBAN && last->user_id == 7001u);
  frame(4);
  CT_CHECK(ssd().rows == 1 && !ss_has("unban:7001") && ss_has("unban:7002"));

  // ---- Audit log: the filter chips are local and agree with the model.
  CT_CHECK(ss_click("page:audit") && ssd().page == 4);
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_AUDIT_LOG);
  frame(4);
  c = ccord_mock_admin_count(tr);
  size_t na = 0;
  const ccord_audit_entry *log = ccord_store_audit(F.store, hq, &na);
  CT_CHECK(log && na >= 6 && ssd().rows == na);   // the seeded three, and what this test just did
  shot("audit");
  bool narrowed = false;
  for (int cat = CCORD_AUDIT_CAT_ALL + 1; cat < CCORD_AUDIT_CAT_COUNT; cat++) {
    char fname[40];
    snprintf(fname, sizeof fname, "audit-filter:%d", cat);
    CT_CHECK(ss_click(fname));
    size_t want = 0;
    for (size_t i = 0; i < na; i++) want += ccord_audit_matches((ccord_audit_category)cat, log[i].action) ? 1u : 0u;
    CT_CHECK(ssd().rows == want);
    narrowed = narrowed || (want > 0 && want < na);
  }
  CT_CHECK(narrowed);
  char all_name[40];
  snprintf(all_name, sizeof all_name, "audit-filter:%d", (int)CCORD_AUDIT_CAT_ALL);
  CT_CHECK(ss_click(all_name) && ssd().rows == na);
  CT_CHECK(ccord_mock_admin_count(tr) == c);

  // ---- Leaving drops the lists (they are never kept around); a stale list is asked for again, once.
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!ssd().open);
  CT_CHECK(!ccord_store_audit_loaded(F.store, hq) && !ccord_store_bans_loaded(F.store, hq) &&
           !ccord_store_invites_loaded(F.store, hq));
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_click("page:bans"));
  frame(4);
  F.now += 31000;   // past the 30 s a list may be reused for
  frame(2);
  c = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("page:invites") && ss_click("page:bans"));
  CT_CHECK(ccord_mock_admin_count(tr) == c + 2);   // each page re-entered once
  frame(4);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!ssd().open);
}

// What the page says when Discord refuses or never answers, and that it recovers.
static void test_server_settings_failures(ccord_transport *tr)
{
  const ccord_id hq = 2001;
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  size_t c = ccord_mock_admin_count(tr);

  // Refused: nothing to show, so the page offers Try Again (and the notice carries Discord's words).
  ccord_mock_set_refuse_writes(tr, true);
  CT_CHECK(ss_click("page:invites"));
  frame(3);
  CT_CHECK(ccord_mock_admin_count(tr) == c + 1 && !ccord_store_invites_loaded(F.store, hq));
  CT_CHECK(ss_has("retry") && ssd().rows == 0);
  CT_CHECK(strstr(dbg().notice, "Missing Permissions") != nullptr);
  frame(60);   // stays put, and does not retry by itself
  CT_CHECK(ccord_mock_admin_count(tr) == c + 1);
  ccord_mock_set_refuse_writes(tr, false);
  CT_CHECK(ss_click("retry"));   // at least 2 s since the request: allowed
  CT_CHECK(ccord_mock_admin_count(tr) == c + 2);
  frame(4);
  CT_CHECK(ccord_store_invites_loaded(F.store, hq) && ssd().rows == 2 && !ss_has("retry"));

  // A refusal belongs to the page that asked.
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!ssd().open);
  for (int pass = 0; pass < 2; pass++) {
    CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
    frame(2);
    ccord_mock_set_refuse_writes(tr, true);
    CT_CHECK(ss_click(pass == 0 ? "page:bans" : "page:audit"));
    frame(3);
    CT_CHECK(ss_has("retry"));
    ccord_mock_set_refuse_writes(tr, false);
    key(CCORD_KEY_ESCAPE, 0);
    CT_CHECK(!ssd().open);
  }
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);

  // No answer at all: after 10 s the page stops saying "Loading" and offers Try Again; a late answer still lands.
  CT_CHECK(ss_click("page:audit"));   // sent; the mock has not been read yet when time jumps
  CT_CHECK(ssd().page == 4);
  c = ccord_mock_admin_count(tr);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!ssd().open);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_press("page:bans"));
  CT_CHECK(ccord_mock_admin_count(tr) == c + 1);
  frame_deaf(100);
  CT_CHECK(!ss_has("retry"));   // still loading
  frame_deaf(11000);
  CT_CHECK(ss_has("retry"));    // given up waiting
  frame(4);                       // ...but the answer was only slow
  CT_CHECK(ccord_store_bans_loaded(F.store, hq) && ssd().rows == 1 && !ss_has("retry"));
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!ssd().open);
}

static void set_hq_moderator_perms(uint64_t perms)
{
  const ccord_id hq = 2001, mods = 5002;
  const ccord_role *old = ccord_store_role(F.store, hq, mods);
  CT_CHECK(old != nullptr);
  if (!old) return;
  char name[64];
  snprintf(name, sizeof name, "%s", old->name);
  ccord_role r = *old;
  r.name = name;
  r.guild_id = hq;
  r.permissions = perms;
  ccord_error err;
  CT_CHECK(ccord_store_upsert_role(F.store, &r, &err) == CCORD_OK);
}

// Manage Roles is not Administrator: the editor locks what is above your highest role, shows why, and sends nothing;
// you cannot hand out a permission you do not hold; and when Discord takes pages away from under an open window, it
// follows (or closes).
static void test_server_settings_hierarchy(ccord_transport *tr)
{
  const ccord_id hq = 2001, self = 1001, admins = 5001, mods = 5002, members = 5003;
  const ccord_role *mrole = ccord_store_role(F.store, hq, mods);
  CT_CHECK(mrole != nullptr);
  if (!mrole) return;
  const uint64_t mods_perms_before = mrole->permissions;
  const ccord_id *ids = nullptr;
  size_t nids = 0;
  CT_CHECK(ccord_store_member_roles(F.store, hq, self, &ids, &nids) && nids > 0 && nids < 8);
  ccord_id self_roles[8];
  for (size_t i = 0; i < nids; i++) self_roles[i] = ids[i];
  const size_t self_nroles = nids;

  ccord_error err;
  set_hq_moderator_perms(CCORD_PERM_MANAGE_ROLES | CCORD_PERM_VIEW_CHANNEL);
  CT_CHECK(ccord_store_set_member_roles(F.store, hq, self, &mods, 1, &err) == CCORD_OK);
  uint64_t perms = ccord_store_permissions(F.store, hq, CCORD_ID_NONE, self, 0);
  CT_CHECK(ccord_store_permissions_exact(F.store, hq, self) && (perms & CCORD_PERM_MANAGE_ROLES) &&
           !(perms & CCORD_PERM_ADMINISTRATOR) && !(perms & CCORD_PERM_MANAGE_GUILD) && !(perms & CCORD_PERM_BAN_MEMBERS));
  CT_CHECK(ccord_store_guild(F.store, hq)->owner_id != self);
  frame(2);

  size_t before = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(3);
  CT_CHECK(ssd().page == 1);   // the first page this user has
  CT_CHECK(ss_has("page:roles") && !ss_has("page:overview") && !ss_has("page:invites") && !ss_has("page:bans") &&
           !ss_has("page:audit"));

  // A role above yours: viewable, not editable. Nothing is sent, and the reason is given.
  CT_CHECK(ss_click("role:5001") && ssd().role_id == admins);
  shot("roles-locked");
  CT_CHECK(ss_click("role-name") && ssd().focus == 0 && strstr(dbg().notice, "below your highest role") != nullptr);
  CT_CHECK(ss_click("color:E91E63") && !ssd().unsaved);
  CT_CHECK(ss_click("role-delete") && F.ui->modal_kind == UI_MODAL_NONE);
  CT_CHECK(ss_click("tab:perms"));
  CT_CHECK(ss_scroll_to("perm:4"));
  uint64_t admins_perms = ssd().work_permissions;
  CT_CHECK(ss_click("perm:4") && ssd().work_permissions == admins_perms && !ssd().unsaved);
  CT_CHECK(ccord_mock_admin_count(tr) == before);

  // A role below yours: editable, but only with permissions you hold yourself.
  CT_CHECK(ss_click("role:5003") && ssd().role_id == members);
  CT_CHECK(ss_click("tab:display") && ss_click("role-name") && ssd().focus == 2);
  type_text("x");
  frame(1);
  CT_CHECK(ssd().unsaved);
  CT_CHECK(ss_click("reset") && !ssd().unsaved);
  CT_CHECK(ss_click("tab:perms"));
  CT_CHECK((ssd().work_permissions & CCORD_PERM_BAN_MEMBERS) == 0 && (ssd().work_permissions & CCORD_PERM_MANAGE_ROLES) == 0);
  CT_CHECK(ss_scroll_to("perm:4"));
  CT_CHECK(ss_click("perm:4") && (ssd().work_permissions & CCORD_PERM_BAN_MEMBERS) == 0 && !ssd().unsaved);   // not yours to give
  CT_CHECK(strstr(dbg().notice, "give out permissions you have") != nullptr);
  CT_CHECK(ss_scroll_to("perm:10000000"));   // Manage Roles, which you do hold
  CT_CHECK(ss_click("perm:10000000") && (ssd().work_permissions & CCORD_PERM_MANAGE_ROLES) && ssd().unsaved);
  CT_CHECK(ss_click("perm:10000000") && !ssd().unsaved);
  CT_CHECK(ccord_mock_admin_count(tr) == before);

  // Discord takes Manage Roles away and leaves Ban Members: the window moves to Bans (asking for the list once).
  set_hq_moderator_perms(CCORD_PERM_BAN_MEMBERS | CCORD_PERM_VIEW_CHANNEL);
  frame(3);
  CT_CHECK(ssd().open && ssd().page == 3 && !ss_has("page:roles"));
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_BANS);
  frame(30);
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1);
  // ...and when nothing is left, it closes.
  set_hq_moderator_perms(CCORD_PERM_VIEW_CHANNEL);
  frame(3);
  CT_CHECK(!ssd().open);
  CT_CHECK(!ccord_ui_open_server_settings(F.ui, hq));

  // Put the world back.
  set_hq_moderator_perms(mods_perms_before);
  CT_CHECK(ccord_store_set_member_roles(F.store, hq, self, self_roles, self_nroles, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  ui_server_settings_close(F.ui);
}

static void test_server_settings_overlay(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  CT_CHECK(ccord_mock_admin_count(tr) == 0);
  test_server_settings_gating_and_overview(tr);
  test_server_settings_roles(tr);
  test_server_settings_lists(tr);
  test_server_settings_failures(tr);
  test_server_settings_hierarchy(tr);
  ui_server_settings_close(F.ui);
}

// The row of the member list whose header starts with `prefix` ("ARTISTS"); SIZE_MAX when there is none.
static size_t member_header(const char *prefix)
{
  for (size_t i = 0; i < F.ui->nmrow; i++) {
    if (F.ui->mrow[i].is_header && strncmp(F.ui->mrow[i].label, prefix, strlen(prefix)) == 0) return i;
  }
  return SIZE_MAX;
}

static size_t member_header_count(const char *prefix)
{
  size_t i = member_header(prefix);
  if (i == SIZE_MAX) return 0;
  const char *dash = strstr(F.ui->mrow[i].label, "\xE2\x80\x94");   // "ARTISTS — 3"
  return dash ? (size_t)strtoul(dash + 4, nullptr, 10) : 0;
}

// The member list and the colour of a name follow the role table, the way Discord does it: grouped under the highest
// hoisted role ("Online" when there is none), coloured by the highest role that has a colour, and editing a role in
// Server Settings regroups and recolours at once.
static void test_member_list_follows_roles(void)
{
  const ccord_id hq = 2001, chen = 1004;
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(6);
  size_t admins = member_header("ADMINS"), mods = member_header("MODERATORS"), online = member_header("ONLINE");
  CT_CHECK(admins != SIZE_MAX && mods != SIZE_MAX && online != SIZE_MAX && admins < mods && mods < online);
  CT_CHECK(member_header("MEMBERS") == SIZE_MAX && member_header("ARTISTS") == SIZE_MAX);   // Members is not hoisted
  size_t online_before = member_header_count("ONLINE");
  CT_CHECK(online_before >= 2);
  bool has = false;
  (void)ui_role_color(F.ui, hq, chen, &has);
  CT_CHECK(!has);   // a plain member has no colour

  // A new role, hoisted and pink, given to Chen through Server Settings.
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_click("page:roles") && ss_click("role-create"));
  frame(4);
  ccord_id made = ssd().role_id;
  CT_CHECK(made >= 6000);
  CT_CHECK(ss_click("role-name"));
  clear_focused_text();
  type_text("Artists");
  CT_CHECK(ss_click("color:E91E63") && ss_click("hoist") && ss_click("save"));
  frame(4);
  CT_CHECK(!ssd().unsaved);
  CT_CHECK(member_header("ARTISTS") == SIZE_MAX);   // nobody holds it yet
  CT_CHECK(ss_click("tab:members") && ss_click("member:1004"));
  frame(4);
  key(CCORD_KEY_ESCAPE, 0);
  frame(3);
  CT_CHECK(!ssd().open);
  size_t artists = member_header("ARTISTS");
  CT_CHECK(artists != SIZE_MAX && member_header_count("ARTISTS") == 1);
  mods = member_header("MODERATORS");
  online = member_header("ONLINE");
  CT_CHECK(mods < artists && artists < online);                      // by role position
  CT_CHECK(member_header_count("ONLINE") == online_before - 1);      // he left the plain group
  ccord_color pink = ui_role_color(F.ui, hq, chen, &has);
  CT_CHECK(has && pink.r == 0xE9 && pink.g == 0x1E && pink.b == 0x63);

  // Un-hoisting dissolves the group at once, but the colour stays (it is a different setting).
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_click("page:roles"));
  char rname[32];
  snprintf(rname, sizeof rname, "role:%llu", (unsigned long long)made);
  CT_CHECK(ss_click(rname));
  frame(20);
  CT_CHECK(ss_click("hoist") && ssd().unsaved && ss_click("save"));
  frame(4);
  key(CCORD_KEY_ESCAPE, 0);
  frame(3);
  CT_CHECK(member_header("ARTISTS") == SIZE_MAX && member_header_count("ONLINE") == online_before);
  (void)ui_role_color(F.ui, hq, chen, &has);
  CT_CHECK(has);

  // Deleting the role takes the colour with it.
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_click("page:roles") && ss_click(rname) && ss_click("role-delete"));
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  key(CCORD_KEY_ESCAPE, 0);
  frame(3);
  CT_CHECK(ccord_store_role(F.store, hq, made) == nullptr);
  (void)ui_role_color(F.ui, hq, chen, &has);
  CT_CHECK(!has && member_header_count("ONLINE") == online_before);
}

// Whether a channel (or category, or thread) of the selected guild has a row in the channel list.
static bool channel_listed(ccord_id channel)
{
  ccord_id g = ccord_store_selected_guild(F.store);
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    const chan_row *r = &F.ui->crow[i];
    if (r->is_voice_user || r->is_relationship || r->dm_nav != DM_NAV_NONE) continue;
    const ccord_channel *c = ccord_store_channel_at(F.store, g, r->index);
    if (c && c->id == channel) return true;
  }
  return false;
}

static size_t mock_requests(const ccord_transport *t)
{
  size_t n = ccord_mock_admin_count(t);
  for (int c = 0; c < CCORD_MOCK_CALL_COUNT; c++) n += ccord_mock_call_count(t, (ccord_mock_call)c);
  return n;
}

static size_t switcher_hits(const char *query)
{
  key(CCORD_KEY_K, CCORD_MOD_CTRL);
  frame(2);
  type_text(query);
  frame(2);
  size_t n = F.ui->nquick_switcher_items;
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  return n;
}

static size_t hash_complete(const char *typed)
{
  type_text(typed);
  frame(1);
  size_t n = dbg().autocomplete_open ? dbg().autocomplete_count : 0;
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
  return n;
}

// Discord does not list what you may not see: a channel whose overwrites take View Channel away is missing from the
// channel list, the "#" completion, the quick switcher and the server's unread badge. It is decided from the exact
// roles only (a guess hides nothing), the channel you are looking at stays, and a role change shows or hides it live.
static void test_hidden_channels(void)
{
  const ccord_id garden = 2003, private_ch = 3316, critique = 3313, general = 3311;
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  CT_CHECK(ccord_session_select_guild(F.session, garden, &err) == CCORD_OK);
  frame(6);
  ccord_id self = ccord_store_self_id(F.store);

  // The channel exists, the user is a plain member of the guild, and the roles are exact: so it is really hidden.
  bool known = false;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, garden); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, garden, i);
    if (c && c->id == private_ch) known = true;
  }
  CT_CHECK(known);
  CT_CHECK(ccord_store_permissions_exact(F.store, garden, self));
  CT_CHECK((ccord_store_permissions(F.store, garden, private_ch, self, 0) & CCORD_PERM_VIEW_CHANNEL) == 0);
  CT_CHECK(channel_listed(general) && channel_listed(critique));   // read-only is not hidden
  CT_CHECK(!channel_listed(private_ch));
  size_t listed_before = F.ui->ncrow;

  CT_CHECK(hash_complete("#mod") == 0);
  CT_CHECK(hash_complete("#cri") == 1);                  // a channel you can see still completes
  CT_CHECK(switcher_hits("#mod-lounge") == 0);
  CT_CHECK(switcher_hits("#critique") == 1);

  // Unread counts of a channel you cannot see are not yours to see either.
  CT_CHECK(ccord_store_set_channel_counts(F.store, private_ch, 7, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, critique, 2, 1, &err) == CCORD_OK);
  uint32_t mentions = 99;
  CT_CHECK(ui_guild_unread(F.ui, garden, &mentions) == 2 && mentions == 1);

  // Becoming a moderator shows it, at once and without a request of any kind.
  const ccord_role *mod_role = role_named(garden, "Moderators");
  CT_CHECK(mod_role != nullptr);
  ccord_id moderators = mod_role ? mod_role->id : CCORD_ID_NONE;
  size_t calls_before = mock_requests(tr);
  CT_CHECK(ccord_store_apply_member_role(F.store, garden, self, moderators, true, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(channel_listed(private_ch) && F.ui->ncrow == listed_before + 1);
  CT_CHECK(mock_requests(tr) == calls_before);
  CT_CHECK(hash_complete("#mod") == 1);
  CT_CHECK(switcher_hits("#mod-lounge") == 1);
  CT_CHECK(ui_guild_unread(F.ui, garden, &mentions) == 9 && mentions == 4);

  // Looking at it keeps it listed when the role goes (the view must not pull the floor out from under you); leaving
  // it for another channel lets it go.
  CT_CHECK(ccord_session_select_channel(F.session, private_ch, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_store_apply_member_role(F.store, garden, self, moderators, false, &err) == CCORD_OK);
  frame(4);
  CT_CHECK((ccord_store_permissions(F.store, garden, private_ch, self, 0) & CCORD_PERM_VIEW_CHANNEL) == 0);
  CT_CHECK(channel_listed(private_ch));
  CT_CHECK(switcher_hits("#mod-lounge") == 1);   // the quick switcher agrees with the list (the composer is locked here)
  CT_CHECK(ccord_session_select_channel(F.session, general, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(!channel_listed(private_ch) && F.ui->ncrow == listed_before);
  CT_CHECK(hash_complete("#mod") == 0);

  // A category you cannot view stays while something inside it can be viewed (Discord lists it then).
  const ccord_id text_category = 3310;
  const ccord_overwrite deny_view = {.id = garden, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_VIEW_CHANNEL};
  CT_CHECK(ccord_store_set_channel_overwrites(F.store, text_category, garden, &deny_view, 1, &err) == CCORD_OK);
  frame(4);
  CT_CHECK((ccord_store_permissions(F.store, garden, text_category, self, 0) & CCORD_PERM_VIEW_CHANNEL) == 0);
  CT_CHECK(channel_listed(text_category) && channel_listed(general));

  // A whole category closed to the user disappears with everything in it, and so does a loose channel (no category).
  const ccord_id voice_category = 3320, voice_a = 3321, voice_b = 3322, stage_ch = 3323, loose = 3399;
  CT_CHECK(channel_listed(voice_category) && channel_listed(voice_a) && channel_listed(voice_b));
  const ccord_channel loose_channel = {.id = loose, .guild_id = garden, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT,
                                       .name = "loose", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(F.store, &loose_channel, &err) == CCORD_OK);
  const ccord_id closed[5] = {voice_category, voice_a, voice_b, stage_ch, loose};
  for (size_t i = 0; i < 5; i++) {
    CT_CHECK(ccord_store_set_channel_overwrites(F.store, closed[i], garden, &deny_view, 1, &err) == CCORD_OK);
  }
  frame(4);
  CT_CHECK(!channel_listed(voice_category) && !channel_listed(voice_a) && !channel_listed(voice_b));
  CT_CHECK(!channel_listed(loose));
  CT_CHECK(channel_listed(text_category) && channel_listed(general));

  // Without exact roles nothing is hidden: Discord decides, and we do not guess someone out of a channel.
  ccord_store_forget_member_roles(F.store, garden, self);
  frame(4);
  CT_CHECK(!ccord_store_permissions_exact(F.store, garden, self));
  CT_CHECK(channel_listed(private_ch) && channel_listed(voice_category) && channel_listed(voice_a));
  CT_CHECK(channel_listed(loose));
  CT_CHECK(hash_complete("#mod") == 1);

  // Administrators see everything, owners too: the other guilds list all their channels.
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(6);
  CT_CHECK(channel_listed(3112) && channel_listed(3111));

  // A guild whose roles we have never heard of: every channel counts as viewable (Discord will tell us otherwise).
  const ccord_guild unknown_guild = {.id = 778000u, .name = "Unknown Roles"};
  const ccord_channel unknown_chan[1] = {
    {.id = 778001u, .guild_id = 778000u, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT, .name = "somewhere", .topic = ""},
  };
  CT_CHECK(ccord_store_upsert_guild(F.store, &unknown_guild, unknown_chan, 1, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_permissions_exact(F.store, 778000u, self));
  CT_CHECK(ui_channel_visible(F.ui, 778000u, &unknown_chan[0]));
}

// Selecting text highlights it the way a browser does (and so Discord): every line is one unbroken band - the blanks
// between words included, not word-sized islands - the line break at the end of a line the selection runs past is
// highlighted as a short stub, and copying gives a space across a soft wrap but a line break across a real one.
static void test_selection_is_one_band(void)
{
  const ccord_id g = 778200u, chn = 778201u, ada = 1002u;
  ccord_error err;
  const ccord_guild guild = {.id = g, .name = "Band"};
  const ccord_channel chans[1] = {
    {.id = chn, .guild_id = g, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT, .name = "band", .topic = ""},
  };
  CT_CHECK(ccord_store_upsert_guild(F.store, &guild, chans, 1, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  const char *para = "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen "
                     "seventeen eighteen nineteen twenty twentyone twentytwo twentythree twentyfour twentyfive twentysix";
  char first[512];
  snprintf(first, sizeof first, "%s\ntail line", para);
  const ccord_message msgs[2] = {
    {.id = 9000001u, .channel_id = chn, .author_id = ccord_store_self_id(F.store), .timestamp_ms = 1700000000000,
     .content = first},
    {.id = 9000002u, .channel_id = chn, .author_id = ada, .timestamp_ms = 1700000060000,
     .content = "second message here"},
  };
  CT_CHECK(ccord_store_set_history(F.store, chn, msgs, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_guild(F.store, g, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, chn, &err) == CCORD_OK);
  frame(4);

  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  F.clip[0] = '\0';
  mouse(CCORD_IN_MOUSE_DOWN, 1, l->message_area.x + 5.0f, l->message_area.y + 2.0f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, l->message_area.x + l->message_area.w - 20.0f, l->message_area.y + l->message_area.h - 2.0f);
  frame(2);
  mouse(CCORD_IN_MOUSE_UP, 1, l->message_area.x + l->message_area.w - 20.0f, l->message_area.y + l->message_area.h - 2.0f);
  frame(2);
  CT_CHECK(F.ui->chat_sel_active && F.ui->dbg_nsel > 10);

  // Per line (rectangles sharing a top), sorted left to right, each starts where the previous one ended.
  size_t lines = 0, widest = 0;
  bool used[128] = {false};
  for (size_t i = 0; i < F.ui->dbg_nsel; i++) {
    if (used[i]) continue;
    float y = F.ui->dbg_sel[i].y;
    ccord_rect row[128];
    size_t n = 0;
    for (size_t k = i; k < F.ui->dbg_nsel; k++) {
      if (fabsf(F.ui->dbg_sel[k].y - y) < 0.5f) { row[n++] = F.ui->dbg_sel[k]; used[k] = true; }
    }
    for (size_t a = 1; a < n; a++) {   // insertion sort by x
      ccord_rect t = row[a];
      size_t b = a;
      while (b > 0 && row[b - 1].x > t.x) { row[b] = row[b - 1]; b--; }
      row[b] = t;
    }
    for (size_t a = 1; a < n; a++) CT_CHECK(fabsf(row[a].x - (row[a - 1].x + row[a - 1].w)) < 0.6f);   // no holes
    CT_CHECK(row[n - 1].w < 14.0f);   // the selection runs on past every line, so each ends in the line-break stub
    if (n > widest) widest = n;
    lines++;
  }
  CT_CHECK(lines >= 4);      // two or more lines of the paragraph, "tail line", the second message
  CT_CHECK(widest >= 12);    // words and the blanks between them, as separate pieces of one band

  shot("selection-band");

  // What gets copied: soft wraps are spaces, real line ends (also between messages) are line breaks.
  key(CCORD_KEY_C, CCORD_MOD_CTRL);
  const char *from = strstr(F.clip, "one two three");
  const char *to = strstr(F.clip, "twentysix");
  CT_CHECK(from && to && to > from && memchr(from, '\n', (size_t)(to - from)) == nullptr);
  CT_CHECK(strncmp(F.clip, "You ", 4) == 0 && strstr(F.clip, "\none two three") != nullptr);   // the header is selected too
  CT_CHECK(strstr(F.clip, "twentysix\ntail line\nAda ") != nullptr && strstr(F.clip, "\nsecond message here") != nullptr);
  F.ui->chat_sel_active = false;
}

// ---- transitions: state changes ease over a few frames instead of switching ----

// Where the transition (ns, id) is right now; -1 when nothing is tracking it.
static float tval(const char *ns, uint64_t id)
{
  uint32_t k = ccord_anim_key(ns, id);
  for (size_t i = 0; i < CCORD_ANIM_SLOTS; i++) {
    if (F.ui->anim.slots[i].key == k) return F.ui->anim.slots[i].value;
  }
  return -1.0f;
}

static bool tmid(float v)
{
  return v > 0.05f && v < 0.99f;
}

// Draws one more frame (50 ms on) and reads one pixel of it.
static int grab_luma(int x, int y)
{
  F.now += 50;
  (void)ccord_ui_update(F.ui, F.now);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_error err;
  bool ok = ccord_render_screenshot(F.renderer, "ui_trans_shot.png", &err) == CCORD_OK;
  ccord_render_end(F.renderer);
  SDL_Surface *surf = ok ? SDL_LoadPNG("ui_trans_shot.png") : nullptr;
  uint8_t r = 0, g = 0, b = 0, a = 0;
  if (surf) SDL_ReadSurfacePixel(surf, x, y, &r, &g, &b, &a);
  CT_CHECK(surf != nullptr);
  if (surf) SDL_DestroySurface(surf);
  remove("ui_trans_shot.png");
  return (int)r + (int)g + (int)b;
}

static float l0_msg_x(void)
{
  return ccord_ui_get_layout(F.ui)->message_area.x + 300.0f;
}

static void test_transitions(void)
{
  // Make sure nothing is left open by an earlier test, and let everything settle.
  ui_server_settings_close(F.ui);
  frame(30);
  CT_CHECK(!F.ui->settings_open && !F.ui->menu.open && F.ui->modal_kind == UI_MODAL_NONE && !F.ui->quick_switcher_open);
  ccord_rect first_guild = ui_guild_icon_rect(F.ui, 1);   // an earlier test left a one-channel guild selected
  click(first_guild.x + first_guild.w * 0.5f, first_guild.y + first_guild.h * 0.5f);
  frame(30);

  // 1. Hovering a channel row fades its background in over ~110 ms and out again when the pointer leaves.
  ccord_id guild = ccord_store_selected_guild(F.store);
  ccord_id sel = ccord_store_selected_channel(F.store);
  ccord_id other = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, guild) && other == CCORD_ID_NONE; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, guild, i);
    if (c->kind == CCORD_CHANNEL_TEXT && c->id != sel && ui_channel_visible(F.ui, guild, c)) other = c->id;
  }
  CT_CHECK(other != CCORD_ID_NONE);
  float y = scan_hover_channel(200.0f, other);
  CT_CHECK(y > 0);
  frame(1);
  CT_CHECK(tval("chan", other * 2u) < 0.05f);                 // the first frame under the pointer has only begun
  frame(1);
  CT_CHECK(tmid(tval("chan", other * 2u)));                   // 50 ms in: on its way
  CT_CHECK(ccord_anim_active(&F.ui->anim));                   // and the client knows it needs more frames
  frame(6);
  CT_CHECK(tval("chan", other * 2u) == 1.0f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, 640.0f, 400.0f);
  frame(2);
  CT_CHECK(tmid(tval("chan", other * 2u)));                   // fading back out, not snapping
  frame(6);
  CT_CHECK(tval("chan", other * 2u) == 0.0f);
  frame(1);
  CT_CHECK(!ccord_anim_active(&F.ui->anim) && !F.ui->animating);   // idle again: no frames are being asked for

  // The same for a message row: its wash (and the action bar that rides on it) follows the pointer.
  float msg_y = scan_hover_message(l0_msg_x());
  CT_CHECK(msg_y > 0);
  ccord_id hov_msg = dbg().hover_message;
  frame(2);
  CT_CHECK(tmid(tval("msg", hov_msg)));
  frame(6);
  CT_CHECK(tval("msg", hov_msg) == 1.0f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, 640.0f, 20.0f);
  frame(8);
  CT_CHECK(tval("msg", hov_msg) == 0.0f);

  // 2. A context menu eases in, stays put while you use it, and starts over when opened again.
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float x = l->message_area.x + 300.0f;
  float my = scan_hover_self_message(x);
  CT_CHECK(my > 0);
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, my);
  frame(1);
  CT_CHECK(F.ui->menu.open);
  uint64_t seq1 = F.ui->menu.seq;
  CT_CHECK(tval("menu", seq1) < 0.05f);
  frame(1);
  CT_CHECK(tmid(tval("menu", seq1)));
  frame(6);
  CT_CHECK(tval("menu", seq1) == 1.0f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, x + 20.0f, my + 30.0f);       // using the menu does not replay the entrance
  frame(3);
  CT_CHECK(tval("menu", seq1) == 1.0f);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, my);
  frame(1);
  CT_CHECK(F.ui->menu.open && F.ui->menu.seq != seq1);
  CT_CHECK(tval("menu", F.ui->menu.seq) < 0.05f);             // a new menu, a new entrance
  key(CCORD_KEY_ESCAPE, 0);
  frame(8);

  // 3. The quick switcher (a full-window overlay) fades in as well.
  key(CCORD_KEY_K, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(F.ui->quick_switcher_open);
  CT_CHECK(tmid(tval("qswitch", 0)));
  frame(6);
  CT_CHECK(tval("qswitch", 0) == 1.0f);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  key(CCORD_KEY_K, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(tmid(tval("qswitch", 0)));                         // closing and reopening replays it
  key(CCORD_KEY_ESCAPE, 0);
  frame(8);

  // A popover plays its entrance each time it opens, not only the first time.
  F.ui->search_open = true;
  F.ui->dirty = true;
  frame(2);
  CT_CHECK(tmid(tval("search", 0)));
  frame(6);
  CT_CHECK(tval("search", 0) == 1.0f);
  F.ui->search_open = false;
  F.ui->dirty = true;
  frame(2);
  F.ui->search_open = true;
  F.ui->dirty = true;
  frame(2);
  CT_CHECK(tmid(tval("search", 0)));
  F.ui->search_open = false;
  F.ui->dirty = true;
  frame(4);

  // What the eye sees: the scrim over the channel list darkens a little more on every frame, from nothing to full.
  frame(8);
  int before = grab_luma(150, 600);
  key(CCORD_KEY_K, CCORD_MOD_CTRL);
  int f1 = grab_luma(150, 600), f2 = grab_luma(150, 600), f3 = grab_luma(150, 600);
  frame(8);
  int fin = grab_luma(150, 600);
  CT_CHECK(abs(f1 - before) <= 3);                            // the first frame has not started to show it
  CT_CHECK(f1 > f2 + 3 && f2 > f3 + 3 && f3 > fin + 3);      // then it darkens step by step
  CT_CHECK(fin < before - 30);
  key(CCORD_KEY_ESCAPE, 0);
  frame(8);

  // 4. Settings: the overlay fades in, a page eases in when chosen, switches slide instead of jumping.
  click(l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
  ccord_ui_settings_debug sd;
  ccord_ui_get_settings_debug(F.ui, &sd);
  CT_CHECK(sd.open);
  CT_CHECK(tmid(tval("settings", 0)));
  frame(8);
  CT_CHECK(tval("settings", 0) == 1.0f);
  click(sd.tabs[2].x + 10.0f, sd.tabs[2].y + 10.0f);           // Appearance
  ccord_ui_get_settings_debug(F.ui, &sd);
  CT_CHECK(sd.page == 2 && !sd.reduced_motion);
  CT_CHECK(tmid(tval("spagebody", 2)));                       // the page eases in
  frame(8);
  CT_CHECK(tval("spagebody", 2) == 1.0f);
  uint64_t knob = (2u * 64u + 7u) * 2u;                       // the Reduced motion switch of this page
  CT_CHECK(tval("swset", knob) == 0.0f);
  click(sd.reduced_motion_toggle.x + sd.reduced_motion_toggle.w - 10.0f,
        sd.reduced_motion_toggle.y + sd.reduced_motion_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &sd);
  CT_CHECK(sd.reduced_motion && F.ui->anim.reduced_motion);   // the setting reaches the animation clock
  CT_CHECK(tval("swset", knob) == 1.0f);                      // and with it on, even this switch moved at once
  click(sd.tabs[3].x + 10.0f, sd.tabs[3].y + 10.0f);           // Voice & Video: no easing under reduced motion
  CT_CHECK(tval("spagebody", 3) == 1.0f);
  click(sd.tabs[2].x + 10.0f, sd.tabs[2].y + 10.0f);
  click(sd.reduced_motion_toggle.x + sd.reduced_motion_toggle.w - 10.0f,
        sd.reduced_motion_toggle.y + sd.reduced_motion_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &sd);
  CT_CHECK(!sd.reduced_motion && !F.ui->anim.reduced_motion);
  CT_CHECK(tmid(tval("swset", knob)) || tval("swset", knob) == 0.0f);   // back to easing: it is travelling home
  key(CCORD_KEY_ESCAPE, 0);
  frame(12);
  CT_CHECK(!F.ui->settings_open);

  // 5. Dialogs and toasts.
  ui_confirm_open(F.ui, CCORD_ADMIN_DELETE_ROLE, guild, 1u, "Test");
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM);
  CT_CHECK(tmid(tval("modal", (uint64_t)UI_MODAL_CONFIRM)));
  frame(8);
  CT_CHECK(tval("modal", (uint64_t)UI_MODAL_CONFIRM) == 1.0f);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);
  ui_notice(F.ui, "Saved");
  float toast_a, toast_slide;
  ui_toast_look(F.ui, &toast_a, &toast_slide);
  CT_CHECK(toast_a < 0.05f && toast_slide > 9.0f);            // a toast starts transparent and 10 px low
  frame(2);
  CT_CHECK(tmid(tval("toast", F.ui->notice_seq)));
  frame(6);
  ui_toast_look(F.ui, &toast_a, &toast_slide);
  CT_CHECK(toast_a == 1.0f && toast_slide == 0.0f);           // then sits still
  while (F.ui->notice[0] && F.ui->notice_until_ms > F.now + 150) frame(1);
  ui_toast_look(F.ui, &toast_a, &toast_slide);
  CT_CHECK(tmid(toast_a) && toast_slide == 0.0f);             // and fades out over its last 250 ms
  uint64_t toast1 = F.ui->notice_seq;
  F.now += 5000;
  frame(4);
  ui_notice(F.ui, "Saved again");
  frame(1);
  CT_CHECK(F.ui->notice_seq != toast1 && tval("toast", F.ui->notice_seq) < 0.05f);   // a second toast slides in afresh
  F.now += 5000;
  frame(8);

  // 6. Someone typing: a pill with moving dots, which keeps the frames coming until they stop.
  CT_CHECK(ccord_store_set_typing(F.store, ccord_store_selected_channel(F.store), 1002u, (int64_t)F.now + 3000, &(ccord_error){0}) == CCORD_OK);
  frame(2);
  CT_CHECK(tmid(tval("typing", 0)));
  frame(8);
  CT_CHECK(tval("typing", 0) == 1.0f);
  CT_CHECK(ccord_anim_active(&F.ui->anim) && F.ui->animating);   // the dots are still going
  frame(70);                                                      // the notice expires
  CT_CHECK(!ccord_anim_active(&F.ui->anim) && !F.ui->animating);

  // 7. The scrollbar fades in with scrolling and fades out when it has been still for a while.
  mouse(CCORD_IN_MOUSE_MOVE, 0, l->chat_header.x + 10.0f, l->chat_header.y + 10.0f);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 3.0f, .x = l->chat_header.x + 10.0f, .y = l->chat_header.y + 10.0f});
  frame(1);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 3.0f, .x = l->message_area.x + 100, .y = l->message_area.y + 100});
  mouse(CCORD_IN_MOUSE_MOVE, 0, l->chat_header.x + 10.0f, l->chat_header.y + 10.0f);
  frame(3);
  CT_CHECK(tmid(tval("vscroll", 0)) || tval("vscroll", 0) == 1.0f);
  frame(4);
  CT_CHECK(tval("vscroll", 0) == 1.0f && F.ui->thumb_h > 0.0f);
  while (F.now < F.ui->last_scroll_ms + 1200) frame(1);          // the moment it is no longer wanted...
  CT_CHECK(tval("vscroll", 0) > 0.5f && F.ui->thumb_h == 0.0f);   // ...it is still fading out, but can no longer be grabbed
  frame(30);                                                      // 1.5 s of stillness
  CT_CHECK(tval("vscroll", 0) < 0.01f && F.ui->thumb_h == 0.0f);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100, .y = l->message_area.y + 100});
  frame(40);

  // 8. Server Settings: the overlay fades in, pages ease in, and the unsaved-changes bar rises when something is edited.
  ui_server_settings_open(F.ui, guild);
  frame(2);
  CT_CHECK(ui_server_settings_is_open(F.ui));
  CT_CHECK(tmid(tval("ssettings", 0)));
  frame(8);
  CT_CHECK(tval("ssettings", 0) == 1.0f);
  CT_CHECK(tval("sspagebody", 0) == 1.0f);                   // the Overview page
  CT_CHECK(ss_click("name") && ssd().focus == 1);
  type_text("x");
  frame(2);
  CT_CHECK(ssd().unsaved && tmid(tval("ssbar", 0)));              // the unsaved-changes bar rises into place
  frame(8);
  CT_CHECK(tval("ssbar", 0) == 1.0f);
  CT_CHECK(ss_click("reset"));
  frame(2);
  CT_CHECK(!ssd().unsaved);
  ui_server_settings_close(F.ui);
  frame(8);

  // 9. When nothing moves the client asks for no frames.
  frame(40);
  CT_CHECK(!ccord_anim_active(&F.ui->anim) && !F.ui->animating);
}

// ---- notification settings: Mute / Notification Settings in the menus, and what a mute does to the lists ----

static int menu_index(const char *prefix)
{
  for (size_t i = 0; i < F.ui->menu.count; i++) {
    if (strncmp(F.ui->menu.items[i].label, prefix, strlen(prefix)) == 0) return (int)i;
  }
  return -1;
}

static void menu_point_at(int idx)
{
  ccord_rect r = ui_menu_item_rect(F.ui, (size_t)idx);
  mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  frame(1);
}

static void menu_click_index(int idx)
{
  ccord_rect r = ui_menu_item_rect(F.ui, (size_t)idx);
  mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  frame(1);
  mouse(CCORD_IN_MOUSE_DOWN, 1, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  frame(1);
}

static void clear_counts(ccord_id guild)
{
  ccord_error err;
  size_t n = ccord_store_channel_count(F.store, guild);
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, guild, i);
    if (c) (void)ccord_store_set_channel_counts(F.store, c->id, 0, 0, &err);
  }
}

static void test_notification_settings_ui(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  frame(3);
  ccord_id dev = find_channel(g, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(dev != CCORD_ID_NONE);
  size_t nopts = 0;
  const ccord_mute_option *opts = ccord_notify_mute_options(&nopts);
  CT_CHECK(nopts == 6);
  CT_CHECK(ccord_mock_admin_count(tr) == 0);
  clear_counts(g);
  frame(2);

  // 1. Right-click a channel: Mute Channel is a parent; pointing at it opens a fly-out of the durations.
  float dev_y = scan_hover_channel(200.0f, dev);
  CT_CHECK(dev_y > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dev_y);
  frame(1);
  int mute = menu_index("Mute Channel");
  int notif = menu_index("Notification Settings");
  CT_CHECK(dbg().context_menu_open && mute == 0 && notif == 7);   // first: Mute Channel (+6 children), then Notification Settings
  CT_CHECK(F.ui->menu.items[mute].has_sub && F.ui->menu.open_sub == -1 && dbg().context_menu_sub_items == 0);
  menu_point_at(mute);
  CT_CHECK(F.ui->menu.open_sub == mute && dbg().context_menu_sub_items == 6);
  float t = tval("menusub", F.ui->menu.seq * 64u + (uint64_t)mute);
  CT_CHECK(t >= 0.0f && t < 0.99f);                      // the fly-out eases in
  frame(10);
  CT_CHECK(tval("menusub", F.ui->menu.seq * 64u + (uint64_t)mute) > 0.99f);
  for (size_t i = 0; i < nopts; i++) CT_CHECK(menu_index(opts[i].label) >= 0);

  // The pointer can cross to the fly-out; pointing at another top-level item closes it; clicking a parent keeps the menu.
  int child = menu_index(opts[1].label);
  menu_point_at(child);
  CT_CHECK(F.ui->menu.open_sub == mute && F.ui->menu.hover == child && dbg().context_menu_open);
  menu_point_at(notif);
  CT_CHECK(F.ui->menu.open_sub == notif && dbg().context_menu_sub_items == 4);   // server default, all, mentions, nothing
  menu_point_at(menu_index("Edit Channel"));
  CT_CHECK(F.ui->menu.open_sub == -1 && dbg().context_menu_sub_items == 0);
  ccord_rect pr = ui_menu_item_rect(F.ui, (size_t)mute);
  mouse(CCORD_IN_MOUSE_DOWN, 1, pr.x + 20.0f, pr.y + pr.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().context_menu_open && F.ui->menu.open_sub == mute);   // a click on a parent opens it, nothing is sent
  CT_CHECK(ccord_mock_admin_count(tr) == 0);

  // 2. Choosing a duration asks Discord; the store changes only when Discord answers.
  {
    ccord_rect cr = ui_menu_item_rect(F.ui, (size_t)menu_index(opts[1].label));
    mouse(CCORD_IN_MOUSE_MOVE, 0, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f);
    frame(1);
    mouse(CCORD_IN_MOUSE_DOWN, 1, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f);   // no frame yet: the request is out, nothing answered
  }
  CT_CHECK(!dbg().context_menu_open);
  CT_CHECK(ccord_mock_admin_count(tr) == 1);
  const ccord_admin_request *last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_SET_NOTIFY && last->guild_id == g && last->channel_id == dev && last->notify_muted == 1);
  CT_CHECK(last && last->until_ms == F.ui->now_unix_ms + opts[1].duration_ms && strstr(last->body, "\"muted\":true") != nullptr);
  CT_CHECK(!ccord_store_channel_muted(F.store, dev, F.ui->now_unix_ms));
  frame(2);
  CT_CHECK(ccord_store_channel_muted(F.store, dev, F.ui->now_unix_ms));

  // 3. A muted channel has no unread marker (its counts keep running), and a mention still counts on the server.
  CT_CHECK(ccord_store_set_channel_counts(F.store, dev, 4, 0, &err) == CCORD_OK);
  frame(8);
  uint32_t mentions = 99;
  CT_CHECK(ui_guild_unread(F.ui, g, &mentions) == 0 && mentions == 0);
  CT_CHECK(tval("unread", dev) < 0.01f);
  CT_CHECK(ccord_store_set_channel_counts(F.store, dev, 4, 2, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ui_guild_unread(F.ui, g, &mentions) == 0 && mentions == 2);
  CT_CHECK(ui_channel_muted(F.ui, ccord_store_channel(F.store, dev)));

  // 4. The menu now offers Unmute, with the time left; choosing it lifts the mute once Discord says so.
  dev_y = scan_hover_channel(200.0f, dev);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dev_y);
  frame(1);
  CT_CHECK(menu_index("Mute Channel") < 0);
  int unmute = menu_index("Unmute Channel (");
  CT_CHECK(unmute == 0 && strstr(F.ui->menu.items[unmute].label, "left") != nullptr);
  {
    ccord_rect cr = ui_menu_item_rect(F.ui, (size_t)unmute);
    mouse(CCORD_IN_MOUSE_MOVE, 0, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f);
    frame(1);
    mouse(CCORD_IN_MOUSE_DOWN, 1, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f);
  }
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 2 && last && last->notify_muted == 0 && strstr(last->body, "\"mute_config\":null") != nullptr);
  CT_CHECK(ccord_store_channel_muted(F.store, dev, F.ui->now_unix_ms));   // not yet: Discord has not answered
  frame(10);
  CT_CHECK(!ccord_store_channel_muted(F.store, dev, F.ui->now_unix_ms));
  CT_CHECK(ui_guild_unread(F.ui, g, &mentions) == 4 && mentions == 2);
  CT_CHECK(tval("unread", dev) > 0.9f);                                     // the marker grows back

  // 5. Notification Settings: the choices, the current one marked; picking one asks Discord.
  dev_y = scan_hover_channel(200.0f, dev);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dev_y);
  frame(1);
  notif = menu_index("Notification Settings");
  menu_point_at(notif);
  const ccord_channel *dev_ch = ccord_store_channel(F.store, dev);
  const ccord_channel *dev_parent = dev_ch && dev_ch->parent_id ? ccord_store_channel(F.store, dev_ch->parent_id) : nullptr;
  CT_CHECK(dev_parent && dev_parent->kind == CCORD_CHANNEL_CATEGORY);
  int use_default = menu_index("Use Category Default"), only = menu_index("Only @mentions");   // a channel in a category defers to it
  CT_CHECK(menu_index("Use Server Default") < 0);
  CT_CHECK(use_default >= 0 && F.ui->menu.items[use_default].checked && only >= 0 && !F.ui->menu.items[only].checked);
  CT_CHECK(menu_index("Suppress @everyone") < 0);   // those belong to the server menu
  menu_click_index(only);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 3 && last && last->notify_level == (int8_t)CCORD_NOTIFY_MENTIONS && last->channel_id == dev);
  frame(4);
  ccord_notify_scope row;
  CT_CHECK(ccord_store_notify_scope(F.store, g, dev, &row) && row.level == CCORD_NOTIFY_MENTIONS && !row.muted);
  dev_y = scan_hover_channel(200.0f, dev);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dev_y);
  frame(1);
  menu_point_at(menu_index("Notification Settings"));
  CT_CHECK(F.ui->menu.items[menu_index("Only @mentions")].checked && !F.ui->menu.items[menu_index("Use Category Default")].checked);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);

  // 6. The server menu: Mute Server mutes everything in it; its levels and the two @ switches live under Notification Settings.
  ccord_rect gr = ui_guild_icon_rect(F.ui, 1);
  float gx = gr.x + gr.w * 0.5f, gy = gr.y + gr.h * 0.5f;
  mouse(CCORD_IN_MOUSE_DOWN, 3, gx, gy);
  frame(1);
  CT_CHECK(dbg().context_menu_open && menu_index("Mute Server") == 0);
  menu_point_at(0);
  CT_CHECK(dbg().context_menu_sub_items == 6);
  menu_click_index(menu_index(opts[5].label));       // until I turn it back on
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 4 && last && last->op == CCORD_ADMIN_SET_NOTIFY && last->channel_id == 0 && last->until_ms == 0);
  CT_CHECK(strstr(last->body, "\"end_time\":null") != nullptr);
  frame(4);
  CT_CHECK(ccord_store_guild_muted(F.store, g, F.ui->now_unix_ms));
  CT_CHECK(ui_guild_unread(F.ui, g, &mentions) == 0 && mentions == 2);   // nothing unread in a muted server; the mention stays
  CT_CHECK(ccord_store_channel_muted(F.store, dev, F.ui->now_unix_ms));
  mouse(CCORD_IN_MOUSE_DOWN, 3, gx, gy);
  frame(1);
  CT_CHECK(menu_index("Mute Server") < 0 && menu_index("Unmute Server") == 0);
  CT_CHECK(strstr(F.ui->menu.items[0].label, "turn it back on") != nullptr);
  menu_point_at(menu_index("Notification Settings"));
  CT_CHECK(dbg().context_menu_sub_items == 5);   // all, mentions, nothing + the two switches (no "use default": nothing is above a server)
  int se = menu_index("Suppress @everyone");
  CT_CHECK(se >= 0 && !F.ui->menu.items[se].checked);
  menu_click_index(se);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 5 && last && strstr(last->body, "\"suppress_everyone\":true") != nullptr);
  frame(4);
  CT_CHECK(ccord_store_notify_scope(F.store, g, 0, &row) && row.suppress_everyone && row.muted);
  mouse(CCORD_IN_MOUSE_DOWN, 3, gx, gy);
  frame(1);
  menu_point_at(menu_index("Notification Settings"));
  se = menu_index("Suppress @everyone");
  CT_CHECK(F.ui->menu.items[se].checked && F.ui->menu.items[se].arg == 0);   // clicking it again would switch it off
  menu_click_index(menu_index("Unmute Server"));
  frame(4);
  CT_CHECK(!ccord_store_guild_muted(F.store, g, F.ui->now_unix_ms));
  CT_CHECK(ui_guild_unread(F.ui, g, &mentions) == 4);

  // 6b. A menu opened on a server that is not the one on screen acts on that server, not on the selected one.
  CT_CHECK(ccord_store_guild_count(F.store) >= 2);
  ccord_id g2 = ccord_store_guild_at(F.store, 1)->id;
  CT_CHECK(g2 != g && ccord_store_selected_guild(F.store) == g);
  ccord_rect gr2 = ui_guild_icon_rect(F.ui, 2);
  mouse(CCORD_IN_MOUSE_DOWN, 3, gr2.x + gr2.w * 0.5f, gr2.y + gr2.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().context_menu_open && menu_index("Mute Server") == 0);
  menu_point_at(0);
  menu_click_index(menu_index(opts[1].label));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 7 && last && last->guild_id == g2 && last->channel_id == 0);
  frame(4);
  CT_CHECK(ccord_store_guild_muted(F.store, g2, F.ui->now_unix_ms) && !ccord_store_guild_muted(F.store, g, F.ui->now_unix_ms));
  CT_CHECK(ccord_store_selected_guild(F.store) == g);

  // 7. A fly-out near the right edge opens on the left of the menu instead of off the window.
  ui_menu_open(F.ui, F.ui->win_w - 90.0f, 300.0f, &(ccord_menu_context){CCORD_MENU_GUILD, g, 0, 0, 0});
  frame(1);
  menu_point_at(menu_index("Mute Server"));
  ccord_rect first_child = ui_menu_item_rect(F.ui, (size_t)menu_index(opts[0].label));
  CT_CHECK(first_child.x + first_child.w <= F.ui->menu.x + 1.0f && first_child.x >= 0.0f);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);

  // 8. Direct messages can be muted but have no levels to pick; a failure from Discord is reported in plain words.
  ccord_id friend_id = other_member(g, 0);
  CT_CHECK(ccord_session_open_dm(F.session, friend_id, &err) == CCORD_OK);
  frame(4);
  const ccord_channel *dm = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, 0);
  CT_CHECK(dm != nullptr);
  if (dm) {
    ui_menu_open(F.ui, 400.0f, 300.0f, &(ccord_menu_context){CCORD_MENU_CHANNEL, CCORD_DM_GUILD_ID, dm->id, 0, 0});
    frame(1);
    int mute_idx = menu_index("Mute @");
    if (mute_idx < 0) mute_idx = menu_index("Mute Conversation");
    CT_CHECK(dbg().context_menu_open && mute_idx >= 0 && menu_index("Notification Settings") < 0);
    menu_click_index(mute_idx);   // clicking the parent only opens its fly-out
    CT_CHECK(dbg().context_menu_open && dbg().context_menu_sub_items == 6);
    menu_click_index(menu_index(opts[0].label));
    frame(4);
    CT_CHECK(ccord_store_channel_muted(F.store, dm->id, F.ui->now_unix_ms));
    CT_CHECK(ccord_store_notify_scope(F.store, CCORD_DM_GUILD_ID, dm->id, &row) && row.muted);
  }
  ccord_admin_notify_edit bad = CCORD_ADMIN_NOTIFY_EDIT_INIT;
  bad.channel_id = 987654321;
  bad.muted = 1;
  ccord_admin_request req;
  CT_CHECK(ccord_admin_set_notify(&req, g, &bad, F.ui->now_unix_ms, &err) == CCORD_OK);
  ui_admin_submit(F.ui, &req);
  CT_CHECK(strstr(dbg().notice, "Couldn't change the notification settings") != nullptr);

  // 9. Idle again: the fly-out's animations have settled, so the client asks for no more frames.
  frame(20);
  CT_CHECK(!ccord_anim_active(&F.ui->anim));
}

// "Mark As Read" leads the menu of a channel or a server that has something unread, sends one acknowledgement per
// channel, and clears the counts; with nothing unread it is not offered.
static void test_mark_as_read_ui(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  frame(3);
  ccord_id sel = ccord_store_selected_channel(F.store);
  ccord_id dev = find_channel(g, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(sel != CCORD_ID_NONE && dev != CCORD_ID_NONE && sel != dev);
  CT_CHECK(ccord_store_message_count(F.store, sel) > 0);   // history is loaded for the open channel: its newest message is known
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);   // and for this one, so it can be acknowledged too
  frame(4);
  CT_CHECK(ccord_session_select_channel(F.session, sel, &err) == CCORD_OK);
  frame(30);   // long enough for the automatic read-ack of the open channel to go out first
  CT_CHECK(ccord_store_message_count(F.store, dev) > 0 && ccord_store_selected_channel(F.store) == sel);
  clear_counts(g);
  frame(2);
  const ccord_menu_context chan_ctx = {CCORD_MENU_CHANNEL, g, sel, 0, 0};
  const ccord_menu_context guild_ctx = {CCORD_MENU_GUILD, g, 0, 0, 0};

  // Nothing unread: not offered, on the channel or on the server.
  ui_menu_open(F.ui, 300.0f, 200.0f, &chan_ctx);
  frame(1);
  CT_CHECK(dbg().context_menu_open && menu_index("Mark As Read") < 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  ui_menu_open(F.ui, 300.0f, 200.0f, &guild_ctx);
  frame(1);
  CT_CHECK(dbg().context_menu_open && menu_index("Mark As Read") < 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);

  // A channel with unread messages: first in its menu, followed by a separator; choosing it acknowledges that channel.
  size_t acks = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, sel, 3, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, dev, 6, 0, &err) == CCORD_OK);   // a neighbour: must stay unread
  frame(2);
  ui_menu_open(F.ui, 300.0f, 200.0f, &chan_ctx);
  frame(1);
  CT_CHECK(menu_index("Mark As Read") == 0 && F.ui->menu.items[1].sep_before);
  menu_click_index(0);
  CT_CHECK(!dbg().context_menu_open);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks + 1);
  const ccord_channel *sc = ccord_store_channel(F.store, sel);
  CT_CHECK(sc && sc->unread_count == 0 && sc->mention_count == 0);
  CT_CHECK(ccord_store_channel(F.store, dev)->unread_count == 6);
  ui_menu_open(F.ui, 300.0f, 200.0f, &chan_ctx);
  frame(1);
  CT_CHECK(menu_index("Mark As Read") < 0);   // read now: no longer offered
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);

  // The server: acknowledges every channel with something unread, and only those.
  clear_counts(g);
  CT_CHECK(ccord_store_set_channel_counts(F.store, sel, 2, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, dev, 0, 5, &err) == CCORD_OK);
  frame(2);
  acks = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  ui_menu_open(F.ui, 300.0f, 200.0f, &guild_ctx);
  frame(1);
  CT_CHECK(menu_index("Mark As Read") == 0 && F.ui->menu.items[1].sep_before);
  menu_click_index(0);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks + 2);
  CT_CHECK(ccord_store_channel(F.store, sel)->unread_count == 0 && ccord_store_channel(F.store, dev)->mention_count == 0);
  ui_menu_open(F.ui, 300.0f, 200.0f, &guild_ctx);
  frame(1);
  CT_CHECK(menu_index("Mark As Read") < 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks + 2);   // nothing more went out
  frame(20);
  CT_CHECK(!ccord_anim_active(&F.ui->anim));
}

// How many messages of a channel are newer than `id` (loading older history while scrolled up must not count).
static size_t messages_after(ccord_id ch, ccord_id id)
{
  size_t n = ccord_store_message_count(F.store, ch), k = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_message *m = ccord_store_message_at(F.store, ch, i);
    if (m && m->id > id) k++;
  }
  return k;
}

// What arrives in the open channel is read only while the user is looking at it: the window focused and the list at its
// newest message. Away or scrolled up it stays unread (and is not acknowledged); coming back acknowledges it.
static void test_reading_follows_focus_and_scroll(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .ambient_interval_ms = 1000, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  frame(3);
  ccord_id gen = ccord_store_selected_channel(F.store);   // #general: where the mock's bot talks
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  frame(80);                                              // settled; a few bot messages came in while it was being read
  const ccord_channel *c = ccord_store_channel(F.store, gen);
  CT_CHECK(c && dbg().stick_bottom && F.ui->window_focused);
  CT_CHECK(c->unread_count == 0 && c->mention_count == 0);
  const ccord_message *last0 = ccord_store_message_at(F.store, gen, ccord_store_message_count(F.store, gen) - 1);
  CT_CHECK(last0 != nullptr);
  ccord_id seen0 = last0 ? last0->id : 0;
  size_t acks = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);

  // Window unfocused: the bot's messages pile up unread, and nothing is acknowledged.
  send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = false});
  frame(1);
  CT_CHECK(!F.ui->window_focused);
  frame(70);
  c = ccord_store_channel(F.store, gen);
  size_t grew = messages_after(gen, seen0);
  CT_CHECK(grew >= 3 && c && c->unread_count == grew);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks);
  // Focus back with the list at the bottom: it is read, and acknowledged up to the newest message.
  send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  frame(3);
  CT_CHECK(F.ui->window_focused);
  c = ccord_store_channel(F.store, gen);
  CT_CHECK(c && c->unread_count == 0 && ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks + 1);
  const ccord_message *newest = ccord_store_message_at(F.store, gen, ccord_store_message_count(F.store, gen) - 1);
  CT_CHECK(newest && c->last_read_id <= newest->id && dbg().acked_message > 0);
  ccord_id seen1 = newest ? newest->id : 0;
  frame(70);                                              // and read from then on, as they arrive
  c = ccord_store_channel(F.store, gen);
  CT_CHECK(c && c->unread_count == 0 && messages_after(gen, seen1) >= 3);

  // Scrolled up: the user is reading history, not the newest messages.
  acks = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  newest = ccord_store_message_at(F.store, gen, ccord_store_message_count(F.store, gen) - 1);
  ccord_id seen2 = newest ? newest->id : 0;
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 600.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(2);
  CT_CHECK(!dbg().stick_bottom);
  frame(70);
  c = ccord_store_channel(F.store, gen);
  grew = messages_after(gen, seen2);
  CT_CHECK(grew >= 3 && c && c->unread_count == grew);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks);
  // Back at the bottom: read, acknowledged.
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -100000.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(3);
  CT_CHECK(dbg().stick_bottom);
  c = ccord_store_channel(F.store, gen);
  CT_CHECK(c && c->unread_count == 0 && ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks + 1);

  // Away *and* scrolled up: coming back to the window alone is not enough.
  send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = false});
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 600.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(60);
  acks = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  frame(60);
  c = ccord_store_channel(F.store, gen);
  CT_CHECK(!dbg().stick_bottom && c && c->unread_count >= 3);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -100000.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(3);
  c = ccord_store_channel(F.store, gen);
  CT_CHECK(dbg().stick_bottom && c && c->unread_count == 0);
  frame(20);
  CT_CHECK(!ccord_anim_active(&F.ui->anim));
}

// The size of the "OFFLINE" group in the member list, as its header says it ("OFFLINE — 3"); -1 when there is none.
static int offline_group_size(void)
{
  for (size_t i = 0; i < F.ui->nmrow; i++) {
    const member_row *r = &F.ui->mrow[i];
    if (r->is_header && strncmp(r->label, "OFFLINE", 7) == 0) {
      const char *dash = strstr(r->label, "\xE2\x80\x94 ");
      return dash ? atoi(dash + 4) : -1;
    }
  }
  return -1;
}

// Messages, reactions and typing redraw the chat but leave the lists of channels and members alone (no re-sorting of
// thousands of members per message); a change of who is where rebuilds them, and the rebuilt list is right.
static void test_lists_rebuild_only_when_they_change(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .ambient_interval_ms = 500, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  frame(40);
  ccord_id gen = ccord_store_selected_channel(F.store);
  CT_CHECK(F.ui->nmrow > 2 && F.ui->ncrow > 2);
  uint64_t base = dbg().list_rebuilds;
  CT_CHECK(base > 0);
  size_t seen = ccord_store_message_count(F.store, gen);

  // The mock's bot talks every half second, we react and type: lots of store changes, no new lists.
  uint64_t rev0 = ccord_store_revision(F.store);
  const ccord_message *last = ccord_store_message_at(F.store, gen, seen - 1);
  CT_CHECK(last != nullptr);
  CT_CHECK(ccord_store_set_reaction(F.store, gen, last->id, "+1", 2, false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_typing(F.store, gen, ccord_store_member_at(F.store, g, 1)->user_id, (int64_t)F.now + 5000, &err) == CCORD_OK);
  frame(60);
  CT_CHECK(ccord_store_message_count(F.store, gen) >= seen + 4);
  CT_CHECK(ccord_store_revision(F.store) >= rev0 + 6);
  CT_CHECK(dbg().list_rebuilds == base);

  // Someone goes offline: the member list is rebuilt once, and the group says so.
  int offline = offline_group_size();
  ccord_id who = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_member_count(F.store, g) && who == CCORD_ID_NONE; i++) {
    const ccord_member *m = ccord_store_member_at(F.store, g, i);
    const ccord_user *u = m ? ccord_store_user(F.store, m->user_id) : nullptr;
    if (u && u->presence != CCORD_PRESENCE_OFFLINE && m->user_id != ccord_store_self_id(F.store)) who = u->id;
  }
  CT_CHECK(who != CCORD_ID_NONE);
  CT_CHECK(ccord_store_update_presence(F.store, who, CCORD_PRESENCE_OFFLINE, "", &err) == CCORD_OK);
  frame(2);
  CT_CHECK(dbg().list_rebuilds == base + 1);
  CT_CHECK(offline_group_size() == (offline < 0 ? 1 : offline + 1));
  frame(20);
  CT_CHECK(dbg().list_rebuilds == base + 1);     // and the bot's messages since did not rebuild again

  // A new channel is a change of the lists.
  size_t nch = F.ui->ncrow;
  ccord_channel fresh = {.id = 987654321, .guild_id = g, .kind = CCORD_CHANNEL_TEXT, .name = "fresh", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(F.store, &fresh, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(dbg().list_rebuilds == base + 2 && F.ui->ncrow == nch + 1);

  // A choice made in the interface (collapsing a category) is not a store change but rebuilds the rows too.
  ccord_id cat = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, g) && cat == CCORD_ID_NONE; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, g, i);
    if (c && c->kind == CCORD_CHANNEL_CATEGORY) cat = c->id;
  }
  CT_CHECK(cat != CCORD_ID_NONE);
  float cy = scan_hover_channel(200.0f, cat);
  CT_CHECK(cy > 0.0f);
  uint64_t before = dbg().list_rebuilds;
  size_t rows = F.ui->ncrow;
  click(200.0f, cy);
  CT_CHECK(dbg().list_rebuilds > before && F.ui->ncrow < rows);   // its channels are gone from the list
  cy = scan_hover_channel(200.0f, cat);
  CT_CHECK(cy > 0.0f);
  click(200.0f, cy);
  CT_CHECK(F.ui->ncrow == rows);
  frame(120);   // the typing notice has expired by now
  CT_CHECK(!ccord_anim_active(&F.ui->anim));
}

// A big server's member list arrives a hundred positions at a time. The group headers print the server's own counts (not
// what happens to be loaded so far), and the end of what is loaded coming into view asks for the next piece: once, at a
// polite pace, and only while the list is on screen in a focused window.
static void scroll_members_to_end(void)
{
  float max = F.ui->member_content_h - F.ui->layout.member_list.h;
  F.ui->member_scroll = max > 0.0f ? max : 0.0f;
  F.ui->member_scroll_vel = 0.0f;
}

static void test_member_list_pages_on_scroll(void)
{
  const ccord_id hq = 2001;
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(6);
  CT_CHECK(F.ui->layout.member_list_visible && !F.ui->members_hidden);
  enum { N = 60 };
  ccord_user us[N];
  ccord_member ms[N];
  char names[N][16];
  for (size_t i = 0; i < N; i++) {
    snprintf(names[i], sizeof names[i], "Extra %02zu", i);
    us[i] = (ccord_user){.id = 7000 + i, .name = names[i], .status_text = "", .presence = i < 40 ? CCORD_PRESENCE_ONLINE : CCORD_PRESENCE_OFFLINE};
    ms[i] = (ccord_member){.guild_id = hq, .user_id = 7000 + i, .role_name = "Members", .role_rank = 255};
  }
  CT_CHECK(ccord_store_upsert_guild_members(F.store, hq, ms, N, us, N, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(F.ui->member_content_h > F.ui->layout.member_list.h + 4.0f * 48.0f);   // longer than the panel
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 0);

  // The server has not said how big the list is, so there is nothing to ask for, even at the end of it.
  scroll_members_to_end();
  frame(40);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 0);

  // It says (with the top of the list in view): the headers show the server's counts when larger than what is held, never smaller.
  F.ui->member_scroll = 0.0f;
  frame(1);
  size_t mods_held = member_header_count("MODERATORS");
  CT_CHECK(mods_held >= 1 && mods_held < 9);
  ccord_id mods_id = CCORD_ID_NONE;
  size_t nroles = 0;
  const ccord_role *roles = ccord_store_roles(F.store, hq, &nroles);
  for (size_t i = 0; roles && i < nroles; i++) {
    if (roles[i].name && strcmp(roles[i].name, "Moderators") == 0) mods_id = roles[i].id;
  }
  CT_CHECK(mods_id != CCORD_ID_NONE);
  const ccord_member_group groups[3] = {{.kind = CCORD_MEMBER_GROUP_ROLE, .role_id = mods_id, .count = 9},
                                        {.kind = CCORD_MEMBER_GROUP_ONLINE, .count = 250},
                                        {.kind = CCORD_MEMBER_GROUP_OFFLINE, .count = 300}};
  CT_CHECK(ccord_store_set_member_list_size(F.store, hq, 600, 250, groups, 3, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(member_header_count("MODERATORS") == 9 && member_header_count("ONLINE") == 250 && member_header_count("OFFLINE") == 300);
  // A server count lower than what is held does not hide members (it is stale, not the truth).
  const ccord_member_group low[1] = {{.kind = CCORD_MEMBER_GROUP_ONLINE, .count = 3}};
  CT_CHECK(ccord_store_set_member_list_size(F.store, hq, 600, 250, low, 1, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(member_header_count("ONLINE") >= 40);
  CT_CHECK(ccord_store_set_member_list_size(F.store, hq, 600, 250, groups, 3, &err) == CCORD_OK);
  frame(2);

  // With the top of the list in view, and the end far away, nothing is asked for.
  frame(40);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 0);
  // The end is looked ahead to by a few rows (so the next piece is on its way before the list runs out): six rows short of
  // it nothing is asked for, two rows short it is.
  const float row_h = F.ui->theme->m.member_item_height;
  scroll_members_to_end();
  const float end_scroll = F.ui->member_scroll;
  F.ui->member_scroll = end_scroll - 6.0f * row_h;
  frame(2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 0);
  F.ui->member_scroll = end_scroll - 2.0f * row_h;
  frame(1);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 1 && ccord_mock_last_member_chunks(tr) == 2);
  // The second hundred was asked for, once.
  scroll_members_to_end();
  frame(2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 1 && ccord_mock_last_member_chunks(tr) == 2);
  frame(100);   // five seconds, and the server has not answered: still the one request
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 1);
  frame(70);    // after eight seconds it is asked again, for one piece more
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 2 && ccord_mock_last_member_chunks(tr) == 3);

  // Nothing is asked for while the window is in the background...
  send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = false});
  frame(200);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 2);
  // ...and it is when the user comes back to a list that has its end in view.
  send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  frame(2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 3 && ccord_mock_last_member_chunks(tr) == 4);

  // Nor while the member list is switched off.
  F.ui->members_hidden = true;
  ui_compute_layout(F.ui);
  frame(200);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 3);
  F.ui->members_hidden = false;
  ui_compute_layout(F.ui);
  frame(2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 4);

  // Conversations have no member list to page.
  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  frame(200);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 4);

  // A list too short to fill the panel is asked for without any scrolling.
  ccord_id other = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_guild_count(F.store); i++) {
    const ccord_guild *gi = ccord_store_guild_at(F.store, i);
    if (gi && gi->id != hq) other = gi->id;
  }
  CT_CHECK(other != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_guild(F.session, other, &err) == CCORD_OK);
  frame(6);
  CT_CHECK(F.ui->member_content_h < F.ui->layout.member_list.h);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 4);   // its size is not known yet
  CT_CHECK(ccord_store_set_member_list_size(F.store, other, 500, 50, groups + 1, 2, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_MEMBERS) == 5 && ccord_mock_last_member_chunks(tr) == 2);
}

static void test_member_lookup_paths(void)
{
  ccord_error err;
  const ccord_id gid = 99001, cid = 99002, uid = 99003;
  ccord_guild ug = {.id = gid, .name = "Unroled Guild"};
  ccord_channel uc = {.id = cid, .guild_id = gid, .kind = CCORD_CHANNEL_TEXT,
                      .discord_type = CCORD_DISCORD_CHANNEL_GUILD_TEXT, .name = "lobby", .topic = ""};
  ccord_user uu = {.id = uid, .name = "plainuser", .handle = "plainuser",
                   .status_text = "On deck", .presence = CCORD_PRESENCE_ONLINE};
  ccord_member um = {.guild_id = gid, .user_id = uid, .role_name = "Captains", .role_color = 0xE67E22u, .role_rank = 10};
  snprintf(um.nick, sizeof um.nick, "CaptainZed");
  CT_CHECK(ccord_store_upsert_guild(F.store, &ug, &uc, 1, &um, 1, &uu, 1, &err) == CCORD_OK);
  ccord_user_profile prof = {.user_id = uid, .display_name = "Plain Global", .handle = "plainuser",
                             .bio = "First line\nSecond line\nThird line\nFourth line"};
  CT_CHECK(ccord_store_upsert_user_profile(F.store, &prof, &err) == CCORD_OK);

  // Role colour falls back to the stored member record when the guild's role table has not arrived yet.
  bool has_col = false;
  ccord_color col = ui_role_color(F.ui, gid, uid, &has_col);
  CT_CHECK(has_col && col.r == 0xE6 && col.g == 0x7E && col.b == 0x22);
  (void)ui_role_color(F.ui, gid, 999999, &has_col);
  CT_CHECK(!has_col);

  const ccord_message msgs[2] = {
    {.id = 991001, .channel_id = cid, .author_id = uid, .content = "ahoy there", .timestamp_ms = 1793534400000LL},
    {.id = 991002, .channel_id = cid, .author_id = ccord_store_self_id(F.store), .content = "hello back",
     .timestamp_ms = 1793535000000LL},
  };
  CT_CHECK(ccord_store_set_history(F.store, cid, msgs, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, cid, &err) == CCORD_OK);
  frame(2);

  // Chat row header uses the server nickname when non-empty, and falls back to the user's name when empty.
  ccord_rect ma = ui_rect(F.ui->layout.message_area);
  F.ui->chat_sel_active = true;
  F.ui->chat_sel_channel = cid;
  F.ui->chat_sel_ax = ma.x;
  F.ui->chat_sel_ay = ma.y;
  F.ui->chat_sel_cx = ma.x + ma.w;
  F.ui->chat_sel_cy = ma.y + ma.h;
  frame(1);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "CaptainZed") != nullptr);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "plainuser") == nullptr);

  ccord_member um_nonick = um;
  um_nonick.nick[0] = '\0';
  CT_CHECK(ccord_store_upsert_guild_members(F.store, gid, &um_nonick, 1, &uu, 1, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "plainuser") != nullptr);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "CaptainZed") == nullptr);
  CT_CHECK(ccord_store_upsert_guild_members(F.store, gid, &um, 1, &uu, 1, &err) == CCORD_OK);
  F.ui->chat_sel_active = false;
  frame(1);

  // Local `from:` filter matches server nicknames in a guild; the timeline itself stays intact.
  F.ui->search_open = true;
  ccord_text_edit_clear(&F.ui->search_edit);
  CT_CHECK(ccord_text_edit_insert(&F.ui->search_edit, "from:CaptainZed", 15, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(F.ui->nvmap == 1);
  CT_CHECK(F.ui->view_count == 2);
  ccord_text_edit_clear(&F.ui->search_edit);
  CT_CHECK(ccord_text_edit_insert(&F.ui->search_edit, "from:NobodyHere", 15, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(F.ui->nvmap == 0);
  CT_CHECK(F.ui->view_count == 2);
  ccord_text_edit_clear(&F.ui->search_edit);
  F.ui->search_open = false;
  frame(1);

  // Member profile popover resolves the server member on the Server Profile tab (role badge replaces multi-line bio),
  // and does not use server member state when viewing DMs.
  F.ui->member_popover_open = true;
  F.ui->member_popover_user_id = uid;
  F.ui->member_popover_rect = (ccord_rect){200.0f, 120.0f, 280.0f, 280.0f};
  F.ui->member_popover_tab = 0;
  frame(1);
  float h_user_tab = F.ui->member_popover_rect.h;
  F.ui->member_popover_tab = 1;
  frame(1);
  float h_server_tab = F.ui->member_popover_rect.h;
  CT_CHECK(h_user_tab > h_server_tab + 20.0f);

  CT_CHECK(ccord_store_select_guild(F.store, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  F.ui->member_popover_tab = 1;
  frame(1);
  float h_dm_popover = F.ui->member_popover_rect.h;
  CT_CHECK(h_dm_popover > h_server_tab);
  F.ui->member_popover_open = false;

  // DM messages never display another guild's nickname for the same user.
  const ccord_id dm_cid = 99004;
  ccord_channel dmc = {.id = dm_cid, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                       .discord_type = CCORD_DISCORD_CHANNEL_DM, .name = "plainuser", .topic = "", .user_id = uid};
  CT_CHECK(ccord_store_upsert_channel(F.store, &dmc, &err) == CCORD_OK);
  const ccord_message dm_msg = {.id = 991010, .channel_id = dm_cid, .author_id = uid, .content = "in dm",
                                .timestamp_ms = 1793536000000LL};
  CT_CHECK(ccord_store_set_history(F.store, dm_cid, &dm_msg, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, dm_cid, &err) == CCORD_OK);
  F.ui->dm_friends_view = false;
  frame(1);
  F.ui->chat_sel_active = true;
  F.ui->chat_sel_channel = dm_cid;
  ma = ui_rect(F.ui->layout.message_area);
  F.ui->chat_sel_ax = ma.x;
  F.ui->chat_sel_ay = ma.y;
  F.ui->chat_sel_cx = ma.x + ma.w;
  F.ui->chat_sel_cy = ma.y + ma.h;
  frame(1);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "plainuser") != nullptr);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "CaptainZed") == nullptr);
  F.ui->chat_sel_active = false;

  F.ui->search_open = true;
  ccord_text_edit_clear(&F.ui->search_edit);
  CT_CHECK(ccord_text_edit_insert(&F.ui->search_edit, "from:CaptainZed", 15, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(F.ui->nvmap == 0);          // nicknames do not apply in DMs
  CT_CHECK(F.ui->view_count == 1);     // the DM timeline is preserved
  ccord_text_edit_clear(&F.ui->search_edit);
  F.ui->search_open = false;

  CT_CHECK(ccord_store_delete_channel(F.store, dm_cid, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_guild(F.store, gid, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(2);
}

static void test_dividers_new_bar_reply_jump_and_timestamps(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(4);   // < UI_ACK_SETTLE_MS so initial default channel (gen) keeps last_read_id == NONE

  const ccord_id gid = 99200, cid = 99201, uid = 99203;
  ccord_guild g = {.id = gid, .name = "M2 Chat Guild"};
  ccord_channel ch1 = {.id = cid, .guild_id = gid, .kind = CCORD_CHANNEL_TEXT, .name = "chat-a", .topic = ""};
  ccord_user u = {.id = uid, .name = "Ada", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  ccord_member mem = {.guild_id = gid, .user_id = uid, .role_name = "", .role_rank = 255};
  CT_CHECK(ccord_store_upsert_guild(F.store, &g, &ch1, 1, &mem, 1, &u, 1, &err) == CCORD_OK);

  // 1. Date dividers across midnight UTC, compact break on day change, and timestamp tooltips.
  // 2026-11-01T23:58:00Z = 1793577480000LL; 2026-11-02T00:00:30Z = 1793577630000LL (90s apart, different UTC day).
  const ccord_message day_msgs[4] = {
    {.id = 992101, .channel_id = cid, .author_id = uid, .content = "late night one", .timestamp_ms = 1793577480000LL},
    {.id = 992102, .channel_id = cid, .author_id = uid, .content = "late night two", .timestamp_ms = 1793577540000LL},
    {.id = 992103, .channel_id = cid, .author_id = uid, .content = "past midnight <t:1793534400:R>", .timestamp_ms = 1793577630000LL,
     .edited = true, .edited_timestamp_ms = 1793581200000LL},
    {.id = 992104, .channel_id = cid, .author_id = uid, .content = "compact after midnight", .timestamp_ms = 1793577660000LL},
  };
  CT_CHECK(ccord_store_set_history(F.store, cid, day_msgs, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_guild(F.store, gid, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, cid, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(dbg().date_dividers == 2 && dbg().new_dividers == 0 && !dbg().new_bar_visible);
  CT_CHECK(!ui_is_compact(F.ui, cid, 0, ccord_store_message_at(F.store, cid, 0)));
  CT_CHECK(ui_is_compact(F.ui, cid, 1, ccord_store_message_at(F.store, cid, 1)));
  CT_CHECK(!ui_is_compact(F.ui, cid, 2, ccord_store_message_at(F.store, cid, 2)));   // midnight broke compact grouping
  CT_CHECK(ui_is_compact(F.ui, cid, 3, ccord_store_message_at(F.store, cid, 3)));

  // Drag selection across day dividers never copies divider date labels into selected text.
  ccord_rect ma = ui_rect(F.ui->layout.message_area);
  F.ui->chat_sel_active = true;
  F.ui->chat_sel_channel = cid;
  F.ui->chat_sel_ax = ma.x;
  F.ui->chat_sel_ay = ma.y;
  F.ui->chat_sel_cx = ma.x + ma.w;
  F.ui->chat_sel_cy = ma.y + ma.h;
  frame(1);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "late night one") != nullptr);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "past midnight") != nullptr);
  CT_CHECK(strstr(F.ui->chat_sel_buf, "November") == nullptr);
  F.ui->chat_sel_active = false;
  frame(1);

  // Timestamp hover tooltips: header timestamp, "(edited)" timestamp, inline <t:...:R>, and compact hover gutter clock.
  CT_CHECK(F.ui->nts_hits >= 4);
  int hdr_idx = -1, edit_idx = -1, inline_idx = -1;
  for (size_t i = 0; i < F.ui->nts_hits; i++) {
    if (F.ui->ts_hits[i].is_edited) edit_idx = (int)i;
    else if (F.ui->ts_hits[i].ts_ms == 1793534400000LL) inline_idx = (int)i;
    else if (F.ui->ts_hits[i].ts_ms == 1793577630000LL) hdr_idx = (int)i;
  }
  CT_CHECK(hdr_idx >= 0 && edit_idx >= 0 && inline_idx >= 0);
  if (hdr_idx >= 0) {
    ccord_rect r = F.ui->ts_hits[hdr_idx].rect;
    mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    frame(1);
    CT_CHECK(strcmp(dbg().tooltip_text, "Monday, November 2, 2026 12:00 AM") == 0);
  }
  if (edit_idx >= 0) {
    ccord_rect r = F.ui->ts_hits[edit_idx].rect;
    mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    frame(1);
    CT_CHECK(strcmp(dbg().tooltip_text, "Edited Monday, November 2, 2026 1:00 AM") == 0);
  }
  if (inline_idx >= 0) {
    ccord_rect r = F.ui->ts_hits[inline_idx].rect;
    mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    frame(1);
    CT_CHECK(strcmp(dbg().tooltip_text, "Sunday, November 1, 2026 12:00 PM") == 0);
  }
  // Compact row 3 reveals its gutter clock on hover and offers a full tooltip over it.
  if (F.ui->nvis >= 4) {
    float cy3 = F.ui->vis[3].y + F.ui->vis[3].h * 0.5f;
    mouse(CCORD_IN_MOUSE_MOVE, 0, ma.x + 200.0f, cy3);
    frame(1);
    int compact_idx = -1;
    for (size_t i = 0; i < F.ui->nts_hits; i++) {
      if (F.ui->ts_hits[i].ts_ms == 1793577660000LL) compact_idx = (int)i;
    }
    CT_CHECK(compact_idx >= 0);
    if (compact_idx >= 0) {
      ccord_rect r = F.ui->ts_hits[compact_idx].rect;
      mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
      frame(1);
      CT_CHECK(strcmp(dbg().tooltip_text, "Monday, November 2, 2026 12:01 AM") == 0);
    }
  }
  mouse(CCORD_IN_MOUSE_MOVE, 0, 10.0f, 10.0f);
  frame(1);
  CT_CHECK(dbg().tooltip_text[0] == '\0');
  CT_CHECK(ccord_store_delete_guild(F.store, gid, &err) == CCORD_OK);

  // 2. NEW divider, auto_ack persistence, "N new messages" bar (jump + Mark As Read), Escape, and self-send dismissal
  //    using real mock-transport channels so acks, sends, and older-history pagination go through session + transport.
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(3);   // 150 ms < UI_ACK_SETTLE_MS (400 ms): gen's history loads while last_read_id stays NONE
  const ccord_id gen = find_channel(2001, "general", CCORD_CHANNEL_TEXT);
  ccord_id c2 = CCORD_ID_NONE, c3 = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, 2001); i++) {
    const ccord_channel *ci = ccord_store_channel_at(F.store, 2001, i);
    if (ci && ci->kind == CCORD_CHANNEL_TEXT && ci->id != gen && !ccord_store_channel_is_forum(F.store, ci->id)) {
      if (c2 == CCORD_ID_NONE) c2 = ci->id;
      else if (c3 == CCORD_ID_NONE && ci->id != c2) { c3 = ci->id; break; }
    }
  }
  CT_CHECK(gen != CCORD_ID_NONE && c2 != CCORD_ID_NONE && c3 != CCORD_ID_NONE);

  ccord_id self_id = ccord_store_self_id(F.store);
  ccord_id u1 = CCORD_ID_NONE, u2 = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_member_count(F.store, 2001); i++) {
    const ccord_member *mi = ccord_store_member_at(F.store, 2001, i);
    if (!mi || mi->user_id == self_id) continue;
    if (u1 == CCORD_ID_NONE) u1 = mi->user_id;
    else if (u2 == CCORD_ID_NONE && mi->user_id != u1) { u2 = mi->user_id; break; }
  }
  CT_CHECK(u1 != CCORD_ID_NONE && u2 != CCORD_ID_NONE);

  size_t gen_n = ccord_store_message_count(F.store, gen);
  CT_CHECK(gen_n == 50 && ccord_store_channel(F.store, gen)->last_read_id == CCORD_ID_NONE);
  ccord_id real_ids[50];
  for (size_t i = 0; i < 50; i++) real_ids[i] = ccord_store_message_at(F.store, gen, i)->id;

  CT_CHECK(ccord_session_select_channel(F.session, c3, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(ccord_session_select_channel(F.session, c2, &err) == CCORD_OK);
  frame(3);

  // Normalize all 50 messages in gen while viewing c2:
  // rows 0..11 share u1 so row 10 breaks compact ONLY because of NEW while row 11 stays compact;
  // rows 12..49 alternate authors (~1750 px tall) so row 10 is well above the bottom viewport.
  ccord_message many[50];
  char bodies[50][32];
  for (size_t i = 0; i < 50; i++) {
    snprintf(bodies[i], sizeof bodies[i], "bulk line %02zu", i);
    many[i] = (ccord_message){
      .id = real_ids[i],
      .channel_id = gen,
      .author_id = (i <= 11) ? u1 : ((i % 2 == 0) ? u2 : u1),
      .content = bodies[i],
      .timestamp_ms = 1793585000000LL + (int64_t)i * 10000LL,
    };
  }
  CT_CHECK(ccord_store_set_history(F.store, gen, many, 50, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_read(F.store, gen, real_ids[9], 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel(F.store, gen)->unread_count == 40);

  // Switch into gen: viewport starts at bottom (first_visible > 10), so the NEW divider is above the viewport and the top bar shows.
  CT_CHECK(ccord_store_select_channel(F.store, gen, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(dbg().first_visible > 10 && dbg().new_bar_visible && dbg().new_dividers == 0);

  // Stay past UI_ACK_SETTLE_MS (400 ms): auto_ack clears unread_count, yet new_marker stays so the top bar still points to row 10!
  frame(12);
  CT_CHECK(ccord_store_channel(F.store, gen)->unread_count == 0);
  CT_CHECK(dbg().acked_message == real_ids[49]);
  CT_CHECK(dbg().new_bar_visible);

  // Click the left half of "40 new messages": jumps up so row 10 (with the red NEW divider) is on screen!
  ccord_rect nbar = dbg().new_bar_rect;
  click(nbar.x + 24.0f, nbar.y + nbar.h * 0.5f);
  CT_CHECK(!dbg().stick_bottom && dbg().first_visible <= 10 && dbg().last_visible >= 10);
  CT_CHECK(dbg().new_dividers == 1);
  CT_CHECK(!dbg().new_bar_visible);   // unread_count is 0 and the NEW divider is now visible in view!
  CT_CHECK(!ui_is_compact(F.ui, gen, 10, ccord_store_message_at(F.store, gen, 10)));
  CT_CHECK(ui_is_compact(F.ui, gen, 11, ccord_store_message_at(F.store, gen, 11)));

  // Escape dismisses the NEW divider when no popup/edit/selection is open.
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(dbg().new_dividers == 0 && !dbg().new_bar_visible);
  CT_CHECK(ui_is_compact(F.ui, gen, 10, ccord_store_message_at(F.store, gen, 10)));

  // While scrolled up (not reading) in c3, incoming messages trigger NEW + top bar; clicking "Mark As Read" acks & clears both.
  size_t c3_n = ccord_store_message_count(F.store, c3);
  CT_CHECK(c3_n >= 4);
  ccord_message c3_tail2[2] = {
    *ccord_store_message_at(F.store, c3, c3_n - 2),
    *ccord_store_message_at(F.store, c3, c3_n - 1),
  };
  c3_tail2[0].author_id = u1;
  c3_tail2[0].content = "while scrolled up 1";
  c3_tail2[0].rich = nullptr;
  c3_tail2[0].reactions = nullptr;
  c3_tail2[0].reaction_kind_count = 0;
  c3_tail2[1].author_id = u1;
  c3_tail2[1].content = "while scrolled up 2";
  c3_tail2[1].rich = nullptr;
  c3_tail2[1].reactions = nullptr;
  c3_tail2[1].reaction_kind_count = 0;
  CT_CHECK(ccord_store_delete_message(F.store, c3, c3_tail2[0].id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_message(F.store, c3, c3_tail2[1].id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, c3, &err) == CCORD_OK);
  frame(1);
  F.ui->vlist.stick_bottom = false;
  F.ui->vlist.anchor_index = 0;
  F.ui->vlist.anchor_offset = 0.0f;
  frame(1);
  size_t acks_before = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  CT_CHECK(ccord_store_append_message(F.store, &c3_tail2[0], &err) == CCORD_OK);
  CT_CHECK(ccord_store_append_message(F.store, &c3_tail2[1], &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, c3, 2, 0, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(dbg().new_bar_visible && dbg().new_bar_ack_rect.w > 20.0f);
  ccord_rect ack_btn = dbg().new_bar_ack_rect;
  click(ack_btn.x + ack_btn.w * 0.5f, ack_btn.y + ack_btn.h * 0.5f);
  frame(2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks_before + 1);
  CT_CHECK(ccord_store_channel(F.store, c3)->unread_count == 0);
  CT_CHECK(!dbg().new_bar_visible && dbg().new_dividers == 0);

  // Fallback when last_read_id == CCORD_ID_NONE (resolved from unread_count), and dismissal when self sends a message.
  size_t c2_n = ccord_store_message_count(F.store, c2);
  CT_CHECK(c2_n >= 4 && ccord_store_channel(F.store, c2)->last_read_id == CCORD_ID_NONE);
  ccord_message c2_msgs[4];
  for (size_t i = 0; i < 4; i++) {
    ccord_id mid = ccord_store_message_at(F.store, c2, c2_n - 4 + i)->id;
    c2_msgs[i] = (ccord_message){
      .id = mid, .channel_id = c2, .author_id = u1, .content = "c2 msg",
      .timestamp_ms = 1793586000000LL + (int64_t)i * 10000LL,
    };
  }
  CT_CHECK(ccord_store_set_history(F.store, c2, c2_msgs, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, c2, 2, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, c2, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(dbg().new_dividers == 1);
  CT_CHECK(!ui_is_compact(F.ui, c2, c2_n - 2, ccord_store_message_at(F.store, c2, c2_n - 2)));
  CT_CHECK(ui_is_compact(F.ui, c2, c2_n - 1, ccord_store_message_at(F.store, c2, c2_n - 1)));
  // User sends their own message -> NEW divider disappears as soon as the server echoes the message.
  ccord_rect box = ui_composer_box(F.ui);
  click(box.x + 80.0f, box.y + box.h * 0.5f);
  type_text("my own reply");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(dbg().new_dividers == 0);

  // 3. Reply jump & highlight flash (in-store + older-history fetch), deleted reply card, and missing target notice.
  CT_CHECK(ccord_store_select_channel(F.store, gen, &err) == CCORD_OK);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(2);
  CT_CHECK(dbg().stick_bottom && dbg().first_visible > 5);

  ccord_message rep_msg = {
    .id = real_ids[49] + 1000, .channel_id = gen, .author_id = u1, .reply_to_id = real_ids[2],
    .content = "jumping back to row 2", .timestamp_ms = 1793585600000LL,
  };
  CT_CHECK(ccord_store_append_message(F.store, &rep_msg, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(dbg().reply_cards >= 1 && F.ui->nreply_hits == 1);
  ccord_rect rep_rc = F.ui->reply_hits[0].rect;
  click(rep_rc.x + 40.0f, rep_rc.y + rep_rc.h * 0.5f);
  CT_CHECK(!dbg().stick_bottom && dbg().first_visible <= 2 && dbg().last_visible >= 2);
  CT_CHECK(dbg().jump_highlight_message == real_ids[2]);
  // Highlight flash expires cleanly and leaves no active animation behind.
  frame(35);
  CT_CHECK(dbg().jump_highlight_message == CCORD_ID_NONE);
  CT_CHECK(!ccord_anim_active(&F.ui->anim));

  // Replying to an older message not yet in store triggers before=<first->id> pagination and jumps once loaded!
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(2);
  ccord_id older_target = real_ids[0] - 5;
  ccord_message_rich older_rich = {
    .present = CCORD_RICH_HAS_REFERENCE,
    .reference = {.kind = CCORD_REF_REPLY, .message_id = older_target, .channel_id = gen, .guild_id = 2001},
    .ref_preview = {.present = true, .author_id = u1, .author_name = "Ada", .content = "from older page"},
  };
  ccord_message older_rep = {
    .id = real_ids[49] + 1001, .channel_id = gen, .author_id = u1, .reply_to_id = older_target,
    .content = "reply to older page", .timestamp_ms = 1793585605000LL, .rich = &older_rich,
  };
  CT_CHECK(ccord_store_append_message(F.store, &older_rep, &err) == CCORD_OK);
  frame(2);
  ccord_rect older_rc = {0};
  for (size_t i = 0; i < F.ui->nreply_hits; i++) {
    if (F.ui->reply_hits[i].target_msg_id == older_target) older_rc = F.ui->reply_hits[i].rect;
  }
  CT_CHECK(older_rc.w > 0.0f);
  click(older_rc.x + 40.0f, older_rc.y + older_rc.h * 0.5f);
  frame(6);
  CT_CHECK(ccord_store_find_message(F.store, gen, older_target) != nullptr);
  CT_CHECK(dbg().jump_highlight_message == older_target);
  frame(35);
  CT_CHECK(dbg().jump_highlight_message == CCORD_ID_NONE);

  // Deleted referenced message renders card without recording a clickable jump hit; missing reference shows a notice.
  ccord_vlist_reset_bottom(&F.ui->vlist);
  ccord_message_rich del_rich = {.present = CCORD_RICH_HAS_REFERENCE, .ref_preview = {.deleted = true}};
  ccord_message del_rep = {
    .id = real_ids[49] + 1002, .channel_id = gen, .author_id = u1, .content = "reply to deleted",
    .timestamp_ms = 1793585610000LL, .rich = &del_rich,
  };
  CT_CHECK(ccord_store_append_message(F.store, &del_rep, &err) == CCORD_OK);
  ccord_id miss_target = real_ids[49] + 9999;
  ccord_message_rich miss_rich = {
    .present = CCORD_RICH_HAS_REFERENCE,
    .reference = {.kind = CCORD_REF_REPLY, .message_id = miss_target, .channel_id = gen, .guild_id = 2001},
    .ref_preview = {.present = true, .author_id = u1, .author_name = "Ada", .content = "long gone"},
  };
  ccord_message miss_rep = {
    .id = real_ids[49] + 1003, .channel_id = gen, .author_id = u1, .reply_to_id = miss_target,
    .content = "reply to unloaded", .timestamp_ms = 1793585620000LL, .rich = &miss_rich,
  };
  CT_CHECK(ccord_store_append_message(F.store, &miss_rep, &err) == CCORD_OK);
  frame(2);
  bool found_deleted_hit = false;
  ccord_rect miss_rc = {0};
  for (size_t i = 0; i < F.ui->nreply_hits; i++) {
    if (F.ui->reply_hits[i].target_msg_id == 0) found_deleted_hit = true;
    if (F.ui->reply_hits[i].target_msg_id == miss_target) miss_rc = F.ui->reply_hits[i].rect;
  }
  CT_CHECK(!found_deleted_hit && miss_rc.w > 0.0f);
  click(miss_rc.x + 40.0f, miss_rc.y + miss_rc.h * 0.5f);
  CT_CHECK(strstr(dbg().notice, "not available") != nullptr);
  frame(2);
}

// M2 Slice 4b: Rich embed layout (provider, author, wrapped title link, markdown description, 3-col inline + full-width
// fields, thumbnail, multi-embed gallery, footer + timestamp tooltip), multi-image attachment galleries (2/3/4/6-tile),
// spoiler overlays, lightbox zoom/prev-next/Open-in-Browser/backdrop-close, and inline audio/video players.
static void test_embeds_galleries_spoilers_lightbox_and_media_players(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  const ccord_id empty_ch = 399901u;
  ccord_channel lab = {.id = empty_ch, .guild_id = 2001, .kind = CCORD_CHANNEL_TEXT, .name = "rich-lab", .topic = "", .icon_url = ""};
  CT_CHECK(ccord_store_upsert_channel(F.store, &lab, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, empty_ch, &err) == CCORD_OK);
  frame(3);

  // 1. Full rich embed: provider, author + icon + link, clickable wrapped title, markdown description,
  // 3 inline fields + 1 full-width field, right-side thumbnail -> lightbox, footer + timestamp hover tooltip.
  ccord_embed_field fields[4] = {
    {.name = "Arch", .value = "`x86_64`", .inline_ = true},
    {.name = "Compiler", .value = "GCC 14", .inline_ = true},
    {.name = "Status", .value = "**Green**", .inline_ = true},
    {.name = "Release Notes", .value = "Read [Docs](https://docs.kernel.org) for details.", .inline_ = false},
  };
  ccord_embed rich_emb = {
    .kind = CCORD_EMBED_RICH,
    .provider = "GitHub",
    .author_name = "Torvalds",
    .author_url = "https://github.com/torvalds",
    .author_icon = "https://cdn.discordapp.com/avatars/1/auth.png",
    .title = "Linux v6.12 Release Candidate",
    .url = "https://github.com/torvalds/linux/releases/tag/v6.12",
    .description = "Full **markdown** body with [Changelog](https://kernel.org/changelog) and `inline_code`.",
    .has_color = true,
    .color = 0x2ECC71u,
    .thumbnail = {.url = "https://cdn.discordapp.com/attachments/1/thumb.png", .width = 128, .height = 128},
    .fields = fields,
    .field_count = 4,
    .footer_text = "Linux CI",
    .footer_icon = "https://cdn.discordapp.com/avatars/1/foot.png",
    .timestamp_ms = 1793585600000LL,
  };
  ccord_message_rich r_emb = {.present = CCORD_RICH_ALL, .embeds = &rich_emb, .embed_count = 1};
  ccord_id mid = append_rich_message(empty_ch, "", &r_emb);
  frame(4);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.embed_cards == 1 && d.embed_fields_drawn == 4 && d.attachment_textures_drawn >= 1);

  // Verify author link, title link, description markdown link, and field markdown link are all clickable.
  const char *want_links[4] = {
    "https://github.com/torvalds",
    "https://github.com/torvalds/linux/releases/tag/v6.12",
    "https://kernel.org/changelog",
    "https://docs.kernel.org",
  };
  for (size_t k = 0; k < 4; k++) {
    ccord_rect lr = {0};
    for (size_t i = 0; i < F.ui->nlinks; i++) {
      if (strcmp(F.ui->links[i].url, want_links[k]) == 0) lr = F.ui->links[i].rect;
    }
    CT_CHECK(lr.w > 0.0f && lr.h > 0.0f);
    int before_u = F.open_url_calls;
    click(lr.x + 4.0f, lr.y + lr.h * 0.5f);
    CT_CHECK(F.open_url_calls == before_u + 1 && strcmp(F.last_url, want_links[k]) == 0);
  }

  // Hovering the embed footer timestamp shows the full timestamp tooltip.
  ccord_rect foot_ts = {0};
  for (size_t i = 0; i < F.ui->nts_hits; i++) {
    if (F.ui->ts_hits[i].ts_ms == 1793585600000LL && F.ui->ts_hits[i].rect.y > 140.0f) {
      foot_ts = F.ui->ts_hits[i].rect;
    }
  }
  CT_CHECK(foot_ts.w > 0.0f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, foot_ts.x + 2.0f, foot_ts.y + foot_ts.h * 0.5f);
  frame(2);
  CT_CHECK(dbg().tooltip_text[0] != '\0');
  mouse(CCORD_IN_MOUSE_MOVE, 0, 5.0f, 5.0f);
  frame(1);

  // Clicking the embed thumbnail opens the lightbox; Esc closes it.
  CT_CHECK(F.ui->nimg_hits == 1);
  ccord_rect thumb_rc = F.ui->img_hits[0].rect;
  click(thumb_rc.x + thumb_rc.w * 0.5f, thumb_rc.y + thumb_rc.h * 0.5f);
  d = dbg();
  CT_CHECK(d.lightbox_open && d.lightbox_count == 1 && d.lightbox_index == 0);
  CT_CHECK(strcmp(d.lightbox_url, "https://cdn.discordapp.com/attachments/1/thumb.png") == 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!dbg().lightbox_open);
  remove_message(empty_ch, mid);

  // Multi-image embed gallery (two embeds sharing the same URL).
  ccord_embed multi_emb[2] = {
    {.kind = CCORD_EMBED_RICH, .title = "Album", .url = "https://example.com/album",
     .image = {.url = "https://cdn.discordapp.com/attachments/1/e1.png", .width = 320, .height = 200}},
    {.kind = CCORD_EMBED_RICH, .url = "https://example.com/album",
     .image = {.url = "https://cdn.discordapp.com/attachments/1/e2.png", .width = 320, .height = 200}},
  };
  ccord_message_rich r_multi_emb = {.present = CCORD_RICH_ALL, .embeds = multi_emb, .embed_count = 2};
  mid = append_rich_message(empty_ch, "", &r_multi_emb);
  frame(3);
  CT_CHECK(dbg().embed_cards == 2 && F.ui->nimg_hits == 2);
  click(F.ui->img_hits[0].rect.x + 10.0f, F.ui->img_hits[0].rect.y + 10.0f);
  CT_CHECK(dbg().lightbox_open && dbg().lightbox_count == 2 && dbg().lightbox_index == 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  remove_message(empty_ch, mid);

  // 2. Multi-image attachment galleries (3-tile and 6-tile) + full Lightbox controls.
  ccord_attachment gal3[3] = {
    {.id = 101, .filename = "a.png", .content_type = "image/png", .url = "https://cdn.discordapp.com/attachments/1/a.png"},
    {.id = 102, .filename = "b.png", .content_type = "image/png", .url = "https://cdn.discordapp.com/attachments/1/b.png"},
    {.id = 103, .filename = "c.png", .content_type = "image/png", .url = "https://cdn.discordapp.com/attachments/1/c.png"},
  };
  ccord_message_rich r_gal3 = {.present = CCORD_RICH_ALL, .attachments = gal3, .attachment_count = 3};
  mid = append_rich_message(empty_ch, "", &r_gal3);
  frame(3);
  CT_CHECK(dbg().gallery_cards == 1 && dbg().attachment_cards == 3 && F.ui->nimg_hits == 3);
  // Tile 0 is on the left, tiles 1 & 2 are stacked on the right.
  CT_CHECK(F.ui->img_hits[1].rect.x > F.ui->img_hits[0].rect.x + 40.0f);
  CT_CHECK(F.ui->img_hits[2].rect.y > F.ui->img_hits[1].rect.y + 40.0f);
  remove_message(empty_ch, mid);

  ccord_attachment gal6[6];
  char gal_urls[6][64];
  for (size_t i = 0; i < 6; i++) {
    snprintf(gal_urls[i], sizeof gal_urls[i], "https://cdn.discordapp.com/attachments/1/g%zu.png", i + 1u);
    gal6[i] = (ccord_attachment){.id = 200u + i, .filename = "g.png", .content_type = "image/png", .url = gal_urls[i]};
  }
  ccord_message_rich r_gal6 = {.present = CCORD_RICH_ALL, .attachments = gal6, .attachment_count = 6};
  mid = append_rich_message(empty_ch, "", &r_gal6);
  frame(3);
  CT_CHECK(dbg().gallery_cards == 1 && dbg().attachment_cards == 6 && F.ui->nimg_hits == 5);

  // Click the 5th tile (index 4, which displays "+1"): opens lightbox at index 4 of 6.
  ccord_rect tile4 = F.ui->img_hits[4].rect;
  click(tile4.x + tile4.w * 0.5f, tile4.y + tile4.h * 0.5f);
  d = dbg();
  CT_CHECK(d.lightbox_open && d.lightbox_count == 6 && d.lightbox_index == 4);
  CT_CHECK(strcmp(d.lightbox_url, gal_urls[4]) == 0);

  // Right arrow steps to hidden 6th image (index 5); Next button wraps to index 0; Prev button wraps back to 5; Left arrow -> 4.
  key(CCORD_KEY_RIGHT, 0);
  frame(2);
  CT_CHECK(dbg().lightbox_index == 5 && strcmp(dbg().lightbox_url, gal_urls[5]) == 0);
  ccord_rect next_b = dbg().lightbox_next_btn;
  CT_CHECK(next_b.w > 0.0f);
  click(next_b.x + next_b.w * 0.5f, next_b.y + next_b.h * 0.5f);
  CT_CHECK(dbg().lightbox_index == 0 && strcmp(dbg().lightbox_url, gal_urls[0]) == 0);
  ccord_rect prev_b = dbg().lightbox_prev_btn;
  click(prev_b.x + prev_b.w * 0.5f, prev_b.y + prev_b.h * 0.5f);
  CT_CHECK(dbg().lightbox_index == 5);
  key(CCORD_KEY_LEFT, 0);
  frame(2);
  CT_CHECK(dbg().lightbox_index == 4);

  // Wheel zoom in, '-' zoom out, '0' reset, clicking image keeps lightbox open, "Open in Browser" opens URL, backdrop closes.
  float w_base = dbg().lightbox_img_rect.w;
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 1.0f, .x = 640.0f, .y = 400.0f});
  frame(2);
  CT_CHECK(dbg().lightbox_zoom > 1.2f && dbg().lightbox_img_rect.w > w_base * 1.15f);
  send((ccord_input_event){.type = CCORD_IN_TEXT, .text = "-"});
  frame(2);
  CT_CHECK(fabsf(dbg().lightbox_zoom - 1.0f) < 0.02f);
  send((ccord_input_event){.type = CCORD_IN_TEXT, .text = "+"});
  frame(2);
  CT_CHECK(dbg().lightbox_zoom > 1.2f);
  send((ccord_input_event){.type = CCORD_IN_TEXT, .text = "0"});
  frame(2);
  CT_CHECK(fabsf(dbg().lightbox_zoom - 1.0f) < 0.02f);

  ccord_rect img_r = dbg().lightbox_img_rect;
  click(img_r.x + img_r.w * 0.5f, img_r.y + img_r.h * 0.5f);
  CT_CHECK(dbg().lightbox_open);

  int before_open = F.open_url_calls;
  ccord_rect open_b = dbg().lightbox_open_btn;
  CT_CHECK(open_b.w > 0.0f);
  click(open_b.x + open_b.w * 0.5f, open_b.y + open_b.h * 0.5f);
  CT_CHECK(F.open_url_calls == before_open + 1 && strcmp(F.last_url, gal_urls[4]) == 0);
  CT_CHECK(dbg().lightbox_open);

  // Click backdrop at top-left (outside image and controls) closes lightbox.
  click(8.0f, 8.0f);
  CT_CHECK(!dbg().lightbox_open);
  remove_message(empty_ch, mid);

  // 3. Spoiler attachment overlay: hidden until clicked, excluded from lightbox gallery until revealed.
  ccord_attachment sp_atts[2] = {
    {.id = 301, .filename = "SPOILER_secret.png", .content_type = "image/png", .spoiler = true,
     .url = "https://cdn.discordapp.com/attachments/1/SPOILER_secret.png"},
    {.id = 302, .filename = "public.png", .content_type = "image/png", .spoiler = false,
     .url = "https://cdn.discordapp.com/attachments/1/public.png"},
  };
  ccord_message_rich r_sp = {.present = CCORD_RICH_ALL, .attachments = sp_atts, .attachment_count = 2};
  mid = append_rich_message(empty_ch, "", &r_sp);
  frame(3);
  CT_CHECK(F.ui->nspoiler_hits == 1 && F.ui->nimg_hits == 1);
  // Clicking public.png opens a 1-item lightbox (does not leak SPOILER_secret.png).
  click(F.ui->img_hits[0].rect.x + 10.0f, F.ui->img_hits[0].rect.y + 10.0f);
  CT_CHECK(dbg().lightbox_open && dbg().lightbox_count == 1);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  // First click on spoiler tile reveals it without opening lightbox; second click opens 2-item lightbox on secret.png.
  ccord_rect sp_rc = F.ui->spoiler_hits[0].rect;
  click(sp_rc.x + sp_rc.w * 0.5f, sp_rc.y + sp_rc.h * 0.5f);
  CT_CHECK(!dbg().lightbox_open && F.ui->nspoiler_hits == 0 && F.ui->nimg_hits == 2);
  click(F.ui->img_hits[0].rect.x + 10.0f, F.ui->img_hits[0].rect.y + 10.0f);
  CT_CHECK(dbg().lightbox_open && dbg().lightbox_count == 2 && dbg().lightbox_index == 0);
  CT_CHECK(strcmp(dbg().lightbox_url, "https://cdn.discordapp.com/attachments/1/SPOILER_secret.png") == 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  remove_message(empty_ch, mid);

  // 4. Inline audio (.wav) and video (.mp4) attachment players: Play -> real FFmpeg decode -> progress -> seek -> Pause -> Open.
  ccord_attachment wav_att = {
    .id = 401, .filename = "note.wav", .content_type = "audio/wav", .size = 48044, .duration_ms = 500,
    .url = "https://cdn.discordapp.com/attachments/1/note.wav",
  };
  ccord_message_rich r_wav = {.present = CCORD_RICH_ALL, .attachments = &wav_att, .attachment_count = 1};
  mid = append_rich_message(empty_ch, "", &r_wav);
  frame(2);
  CT_CHECK(dbg().media_player_cards == 1 && F.ui->nmedia_hits == 1 && !dbg().media_playing);
  ccord_rect play_r = F.ui->media_hits[0].play_rect;
  ccord_rect scrub_r = F.ui->media_hits[0].scrub_rect;
  ccord_rect open_r = F.ui->media_hits[0].open_rect;
  click(play_r.x + play_r.w * 0.5f, play_r.y + play_r.h * 0.5f);
  CT_CHECK(dbg().media_playing);
  frame(4);
  CT_CHECK(F.ui->media_loaded && !F.ui->media_failed && F.ui->media_stream != nullptr);
  CT_CHECK(ccord_av_stream_info(F.ui->media_stream)->has_audio && F.ui->media_duration_ms == 500u);
  CT_CHECK(dbg().media_playing && dbg().media_progress > 0.25f && dbg().media_progress < 0.85f);
  // Seek via scrub bar to ~75%, then click Play/Pause to pause and verify progress stays frozen.
  click(scrub_r.x + scrub_r.w * 0.75f, scrub_r.y + scrub_r.h * 0.5f);
  CT_CHECK(dbg().media_playing && dbg().media_progress > 0.65f && dbg().media_progress < 0.99f);
  click(play_r.x + play_r.w * 0.5f, play_r.y + play_r.h * 0.5f);
  CT_CHECK(!dbg().media_playing);
  float paused_prog = dbg().media_progress;
  frame(3);
  CT_CHECK(fabsf(dbg().media_progress - paused_prog) < 0.001f);
  int before_wav_open = F.open_url_calls;
  click(open_r.x + open_r.w * 0.5f, open_r.y + open_r.h * 0.5f);
  CT_CHECK(F.open_url_calls == before_wav_open + 1 &&
           strcmp(F.last_url, "https://cdn.discordapp.com/attachments/1/note.wav") == 0);
  remove_message(empty_ch, mid);

  ccord_attachment mp4_att = {
    .id = 402, .filename = "demo.mp4", .content_type = "video/mp4", .size = 2048, .width = 320, .height = 180,
    .url = "https://cdn.discordapp.com/attachments/1/demo.mp4",
  };
  ccord_message_rich r_mp4 = {.present = CCORD_RICH_ALL, .attachments = &mp4_att, .attachment_count = 1};
  mid = append_rich_message(empty_ch, "", &r_mp4);
  frame(2);
  CT_CHECK(dbg().media_player_cards == 1 && F.ui->nmedia_hits >= 1);
  size_t tex_before = dbg().attachment_textures_drawn;
  click(F.ui->media_hits[0].play_rect.x + 12.0f, F.ui->media_hits[0].play_rect.y + 12.0f);
  frame(5);
  CT_CHECK(F.ui->media_loaded && !F.ui->media_failed && F.ui->media_stream != nullptr);
  CT_CHECK(ccord_av_stream_info(F.ui->media_stream)->has_video && ccord_av_stream_info(F.ui->media_stream)->w == 16);
  CT_CHECK(dbg().attachment_textures_drawn >= tex_before + 1u);
  if (dbg().media_playing) {
    click(F.ui->media_hits[0].play_rect.x + 12.0f, F.ui->media_hits[0].play_rect.y + 12.0f);
  }
  remove_message(empty_ch, mid);
  CT_CHECK(ccord_store_delete_channel(F.store, empty_ch, &err) == CCORD_OK);
  frame(2);
}

typedef struct {
  const uint8_t *data;
  size_t len;
} stream_fixture;

static void stream_fixture_read(void *ud, int64_t off, uint8_t *dst, size_t n)
{
  const stream_fixture *f = ud;
  memset(dst, 0, n);
  if (off >= 0 && (size_t)off < f->len) memcpy(dst, f->data + off, n < f->len - (size_t)off ? n : f->len - (size_t)off);
}

// Presses the play control of the video at `url` without pumping the session (the click's own frames would let the
// first range answer arrive).
static bool press_play_of(const char *url)
{
  ccord_rect r = {0};
  for (size_t i = 0; i < F.ui->nmedia_hits; i++) {
    if (strcmp(F.ui->media_hits[i].url, url) == 0 && F.ui->media_hits[i].play_rect.w > 0.0f) {
      r = F.ui->media_hits[i].play_rect;
      break;
    }
  }
  if (r.w <= 0.0f) return false;
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  return true;
}

static ccord_av_state player_state(void)
{
  return ccord_av_stream_state(F.ui->media_stream, nullptr);
}

// Pumps frames, giving the demux thread real time, until the player has decoded its first frame.
static bool wait_first_frame(void)
{
  for (int i = 0; i < 1000; i++) {
    frame(1);
    uint32_t serial = 0;
    if (F.ui->media_failed) return false;
    if (F.ui->media_loaded && ccord_av_stream_frame(F.ui->media_stream, &serial) &&
        ccord_render_has_texture(F.renderer, "__ccord_inline_video__", nullptr, nullptr)) {
      return true;
    }
    SDL_Delay(2);
  }
  return false;
}

// A clicked video opens as a ranged stream: while it opens the player reads as buffering with the clock held, the first
// frame shows before the file is complete, selecting another video (or closing) mid-open returns without a hang, and a
// server that ignores Range falls back to the whole-file download.
static void test_clicked_video_streams_and_closes_mid_open(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  const ccord_id ch = 399903u;
  ccord_channel lab = {.id = ch, .guild_id = 2001, .kind = CCORD_CHANNEL_TEXT, .name = "stream-lab", .topic = "", .icon_url = ""};
  CT_CHECK(ccord_store_upsert_channel(F.store, &lab, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, ch, &err) == CCORD_OK);
  frame(3);
  const char *u_h = "https://cdn.discordapp.com/attachments/1/harvest.mp4";
  const char *u_a = "https://cdn.discordapp.com/attachments/1/stream_a.mp4";
  const char *u_b = "https://cdn.discordapp.com/attachments/1/stream_b.mp4";

  // 1. Nothing registered for harvest.mp4: the mock answers like a server that ignores Range, so the click falls
  // back to the whole-file download, which counts one media request. Its bytes become the ranged fixture below.
  ccord_attachment h_att = {.id = 411, .filename = "harvest.mp4", .content_type = "video/mp4", .size = 2048, .width = 320,
                            .height = 180, .url = u_h};
  ccord_message_rich r_h = {.present = CCORD_RICH_ALL, .attachments = &h_att, .attachment_count = 1};
  ccord_id mid = append_rich_message(ch, "", &r_h);
  frame(2);
  size_t req0 = dbg().media_requests;
  CT_CHECK(press_play_of(u_h));
  frame(8);
  CT_CHECK(F.ui->media_loaded && !F.ui->media_failed && F.ui->media_stream != nullptr && F.ui->media_net == nullptr);
  CT_CHECK(dbg().media_requests == req0 + 1u);
  const ccord_media_item *item = ccord_session_get_media(F.session, u_h);
  static uint8_t clip[65536];   // static: the mock keeps reading it while a player is still open at the end
  static stream_fixture fx;
  CT_CHECK(item && item->data && item->len > 0 && item->len <= sizeof clip);
  memcpy(clip, item->data, item->len);
  fx = (stream_fixture){.data = clip, .len = item->len};
  remove_message(ch, mid);

  // 2. Two ranged virtual files holding the same clip.
  CT_CHECK(ccord_mock_serve_range_file(F.transport, &(ccord_mock_range_file){.url_part = "stream_a.mp4",
               .total = (int64_t)fx.len, .read = stream_fixture_read, .ud = &fx}) == CCORD_OK);
  CT_CHECK(ccord_mock_serve_range_file(F.transport, &(ccord_mock_range_file){.url_part = "stream_b.mp4",
               .total = (int64_t)fx.len, .read = stream_fixture_read, .ud = &fx}) == CCORD_OK);
  ccord_attachment atts[2] = {
    {.id = 421, .filename = "stream_a.mp4", .content_type = "video/mp4", .size = 2048, .width = 320, .height = 180, .url = u_a},
    {.id = 422, .filename = "stream_b.mp4", .content_type = "video/mp4", .size = 2048, .width = 320, .height = 180, .url = u_b},
  };
  ccord_message_rich r_ab = {.present = CCORD_RICH_ALL, .attachments = atts, .attachment_count = 2};
  mid = append_rich_message(ch, "", &r_ab);
  frame(2);
  req0 = dbg().media_requests;
  ccord_mock_range_stats rs0;
  ccord_mock_range_stats_get(F.transport, &rs0);

  // 3. Click play on A and look before the session pumps: the stream is opening, the player shows buffering and the
  // playhead stays at 0 however long the UI runs.
  CT_CHECK(press_play_of(u_a));
  CT_CHECK(F.ui->media_net != nullptr && F.ui->media_stream != nullptr && F.ui->media_playing);
  CT_CHECK(player_state() == CCORD_AV_OPENING && !F.ui->media_loaded);
  for (int i = 0; i < 4; i++) frame_deaf(50);
  CT_CHECK(player_state() == CCORD_AV_OPENING && !F.ui->media_loaded && !F.ui->media_failed);
  CT_CHECK(F.ui->media_buffering && F.ui->media_pos_ms == 0u);
  ccord_mock_range_stats rs1;
  ccord_mock_range_stats_get(F.transport, &rs1);
  CT_CHECK(rs1.requests == rs0.requests && dbg().media_requests == req0);

  // 4. The first frame draws from ranged bytes; no whole-file download was made.
  CT_CHECK(wait_first_frame());
  ccord_media_stream_stats ms;
  ccord_media_stream_stats_get(F.ui->media_net, &ms);
  ccord_mock_range_stats_get(F.transport, &rs1);
  CT_CHECK(ms.ranged && !ms.no_range && ms.error == CCORD_OK && rs1.requests > rs0.requests);
  CT_CHECK(dbg().media_requests == req0 && ccord_av_stream_info(F.ui->media_stream)->w == 16);
  CT_CHECK(!F.ui->media_failed && player_state() == CCORD_AV_READY);

  // 5. A scrub click seeks through the stream and the player keeps going.
  if (F.ui->nmedia_hits > 0) {
    for (size_t i = 0; i < F.ui->nmedia_hits; i++) {
      const ui_media_hit *mh = &F.ui->media_hits[i];
      if (strcmp(mh->url, u_a) == 0 && mh->scrub_rect.w > 0.0f) {
        click(mh->scrub_rect.x + mh->scrub_rect.w * 0.5f, mh->scrub_rect.y + mh->scrub_rect.h * 0.5f);
        break;
      }
    }
  }
  frame(6);
  CT_CHECK(!F.ui->media_failed && player_state() != CCORD_AV_FAILED);

  // 6. Mid-open: select B, and before any bytes arrive select A again. Closing B (and A before it) must return.
  CT_CHECK(press_play_of(u_b));
  CT_CHECK(strcmp(F.ui->media_url, u_b) == 0 && F.ui->media_net != nullptr && player_state() == CCORD_AV_OPENING);
  CT_CHECK(press_play_of(u_a));
  CT_CHECK(strcmp(F.ui->media_url, u_a) == 0 && F.ui->media_net != nullptr && player_state() == CCORD_AV_OPENING);
  CT_CHECK(wait_first_frame());
  CT_CHECK(!F.ui->media_failed);
  // And once more with the player left opening when the message goes away and the player is retargeted.
  CT_CHECK(press_play_of(u_b));
  CT_CHECK(player_state() == CCORD_AV_OPENING);
  remove_message(ch, mid);
  frame(6);
  CT_CHECK(!F.ui->media_failed);
}

static bool media_url_requested(const char *url);   // defined with the other media helpers below

// Embed media comes from Discord's proxied copy at screen size, link videos play in place, GIF embeds play as the
// short videos they are, long signed URLs survive whole, and a download that fails is retried later, not every frame.
static void test_embed_media_sources_video_and_retry(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  const ccord_id ch = 399902u;
  ccord_channel lab = {.id = ch, .guild_id = 2001, .kind = CCORD_CHANNEL_TEXT, .name = "embed-lab", .topic = "", .icon_url = ""};
  CT_CHECK(ccord_store_upsert_channel(F.store, &lab, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, ch, &err) == CCORD_OK);
  frame(3);

  // 1. An article: the picture is loaded from the proxy, sized for the screen, never from the site itself, and
  // is shown as a lead image under the text (wider than a corner thumbnail).
  ccord_embed article = {
    .kind = CCORD_EMBED_ARTICLE, .provider = "Example News", .title = "A headline",
    .url = "https://news.example.test/a", .description = "Summary text.",
    .thumbnail = {.url = "https://news.example.test/lead.jpg",
                  .proxy_url = "https://images-ext-1.discordapp.net/external/abc/https/news.example.test/lead.jpg",
                  .width = 2400, .height = 1200},
  };
  ccord_message_rich r1 = {.present = CCORD_RICH_ALL, .embeds = &article, .embed_count = 1};
  ccord_id mid = append_rich_message(ch, "", &r1);
  frame(3);
  CT_CHECK(!media_url_requested("https://news.example.test/lead.jpg"));
  CT_CHECK(media_url_requested("https://images-ext-1.discordapp.net/external/abc/https/news.example.test/lead.jpg"
                               "?width=800&height=400&format=webp"));
  CT_CHECK(F.ui->nimg_hits == 1 && F.ui->img_hits[0].rect.w > 200.0f);
  CT_CHECK(fabsf(F.ui->img_hits[0].rect.w / F.ui->img_hits[0].rect.h - 2.0f) < 0.1f);   // its own shape, 2:1
  shot("embed_article");
  remove_message(ch, mid);

  // 2. A tweet fixer: a real video file behind the link. It gets a player in the embed, with the poster first and
  // the text kept; play, and the proxied file is decoded.
  ccord_embed tweet = {
    .kind = CCORD_EMBED_VIDEO, .provider = "vxTwitter", .author_name = "Someone (@someone)",
    .url = "https://vxtwitter.com/someone/status/1", .description = "the text of the post stays readable",
    .thumbnail = {.url = "https://pbs.example.test/poster.jpg",
                  .proxy_url = "https://images-ext-1.discordapp.net/external/p/https/pbs.example.test/poster.jpg",
                  .width = 1280, .height = 720},
    .video = {.url = "https://video.example.test/clip.mp4",
              .proxy_url = "https://images-ext-1.discordapp.net/external/v/https/video.example.test/clip.mp4",
              .width = 1280, .height = 720},
  };
  ccord_message_rich r2 = {.present = CCORD_RICH_ALL, .embeds = &tweet, .embed_count = 1};
  mid = append_rich_message(ch, "", &r2);
  frame(3);
  CT_CHECK(F.ui->nmedia_hits == 1);
  CT_CHECK(strcmp(F.ui->media_hits[0].url,
                  "https://images-ext-1.discordapp.net/external/v/https/video.example.test/clip.mp4") == 0);
  ccord_rect vbox = F.ui->media_hits[0].play_rect;
  CT_CHECK(vbox.w > 200.0f && fabsf(vbox.w / vbox.h - 16.0f / 9.0f) < 0.1f);
  CT_CHECK(media_url_requested("https://images-ext-1.discordapp.net/external/p/https/pbs.example.test/poster.jpg"
                               "?width=800&height=450&format=webp"));
  CT_CHECK(!media_url_requested("https://video.example.test/clip.mp4"));
  CT_CHECK(!dbg().media_playing);
  bool text_kept = false;
  for (size_t i = 0; i < F.ui->ntext_hits; i++) {
    if (F.ui->text_hits[i].y + F.ui->text_hits[i].h <= vbox.y + 1.0f && F.ui->text_hits[i].w > 100.0f) text_kept = true;
  }
  CT_CHECK(text_kept);   // the description is drawn above the video
  shot("embed_video_poster");
  click(vbox.x + vbox.w * 0.5f, vbox.y + vbox.h * 0.5f);
  frame(4);
  CT_CHECK(F.ui->media_loaded && !F.ui->media_failed && F.ui->media_stream != nullptr);
  CT_CHECK(ccord_av_stream_info(F.ui->media_stream)->has_video);
  CT_CHECK(ccord_render_has_texture(F.renderer, "__ccord_inline_video__", nullptr, nullptr));
  shot("embed_video_playing");
  // Once it is open the box grows a scrub bar; a click there seeks, it does not pause.
  CT_CHECK(F.ui->nmedia_hits == 1 && F.ui->media_hits[0].scrub_rect.w > 50.0f);
  if (dbg().media_playing) {
    click(F.ui->media_hits[0].play_rect.x + 20.0f, F.ui->media_hits[0].play_rect.y + 20.0f);
    CT_CHECK(!dbg().media_playing);
  }
  remove_message(ch, mid);

  // 3. A player page (no file we can fetch): the poster is a link to the page, there is nothing to play here,
  // and the long page description is left out, as Discord does.
  ccord_embed tube = {
    .kind = CCORD_EMBED_VIDEO, .provider = "YouTube", .title = "A video", .url = "https://www.youtube.com/watch?v=x",
    .description = "A very long description that Discord never shows under a player.",
    .thumbnail = {.url = "https://i.ytimg.example.test/hq.jpg",
                  .proxy_url = "https://images-ext-2.discordapp.net/external/y/https/i.ytimg.example.test/hq.jpg",
                  .width = 1280, .height = 720},
    .video = {.url = "https://www.youtube.com/embed/x", .width = 1280, .height = 720},
  };
  ccord_message_rich r3 = {.present = CCORD_RICH_ALL, .embeds = &tube, .embed_count = 1};
  mid = append_rich_message(ch, "", &r3);
  frame(3);
  CT_CHECK(F.ui->nmedia_hits == 0);
  bool page_link = false;
  for (size_t i = 0; i < F.ui->nlinks; i++) {
    if (strcmp(F.ui->links[i].url, "https://www.youtube.com/watch?v=x") == 0 && F.ui->links[i].rect.h > 100.0f) {
      page_link = true;
    }
  }
  CT_CHECK(page_link);
  remove_message(ch, mid);

  // 4. A GIF embed is a silent looping video: it plays in place from the proxied file, and the lightbox opens that.
  ccord_embed gifv = {
    .kind = CCORD_EMBED_GIFV, .url = "https://klipy.com/gifs/cat-1",
    .thumbnail = {.url = "https://static.klipy.com/cat.png",
                  .proxy_url = "https://images-ext-1.discordapp.net/external/g/https/static.klipy.com/cat.png",
                  .width = 498, .height = 280},
    .video = {.url = "https://static.klipy.com/cat.mp4",
              .proxy_url = "https://images-ext-1.discordapp.net/external/g/https/static.klipy.com/cat.mp4",
              .width = 498, .height = 280},
  };
  ccord_message_rich r4 = {.present = CCORD_RICH_ALL, .embeds = &gifv, .embed_count = 1};
  mid = append_rich_message(ch, "", &r4);
  mouse(CCORD_IN_MOUSE_MOVE, 0, 1270.0f, 40.0f);
  frame(4);
  const char *gif_vid = "https://images-ext-1.discordapp.net/external/g/https/static.klipy.com/cat.mp4";
  size_t cur = 0, count = 0;
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, gif_vid, &cur, &count));
  size_t top = cur;
  for (int i = 0; i < 6; i++) {
    frame(1);
    (void)ccord_render_texture_is_animated(F.renderer, gif_vid, &cur, &count);
    if (cur > top) top = cur;
  }
  CT_CHECK(top >= 1);   // it moves
  CT_CHECK(F.ui->nimg_hits == 1 && strcmp(F.ui->img_hits[0].urls[0], gif_vid) == 0);
  CT_CHECK(F.ui->nmedia_hits == 0);   // no player chrome on a GIF
  shot("embed_gifv");
  remove_message(ch, mid);

  // 5. A signed attachment URL far past 255 bytes is kept whole (it used to be cut, which broke the signature).
  char long_url[700];
  int ln = snprintf(long_url, sizeof long_url, "https://cdn.discordapp.com/attachments/1/2/long.png?ex=6a&is=6b&hm=");
  for (; ln < 600; ln++) long_url[ln] = (char)('a' + ln % 6);
  long_url[ln] = '\0';
  ccord_attachment long_att = {.id = 501, .filename = "long.png", .content_type = "image/png", .size = 900,
                               .url = long_url, .width = 300, .height = 200};
  ccord_message_rich r5 = {.present = CCORD_RICH_ALL, .attachments = &long_att, .attachment_count = 1};
  mid = append_rich_message(ch, "", &r5);
  frame(3);
  CT_CHECK(media_url_requested(long_url));
  CT_CHECK(F.ui->nimg_hits == 1 && strcmp(F.ui->img_hits[0].urls[0], long_url) == 0);
  remove_message(ch, mid);

  // 6. A picture whose sized copy fails falls back to the original; one that fails outright is asked for once,
  // left alone for a while, then tried again - and a later success shows it.
  ccord_mock_fail_media(F.transport, "width=");
  ccord_attachment big_att = {.id = 502, .filename = "big.png", .content_type = "image/png", .size = 9000,
                              .url = "https://cdn.discordapp.com/attachments/1/2/big.png",
                              .proxy_url = "https://media.discordapp.net/attachments/1/2/big.png",
                              .width = 4000, .height = 3000};
  ccord_message_rich r6 = {.present = CCORD_RICH_ALL, .attachments = &big_att, .attachment_count = 1};
  mid = append_rich_message(ch, "", &r6);
  frame(4);
  const char *big_sized = "https://media.discordapp.net/attachments/1/2/big.png?width=800&height=600&format=webp";
  CT_CHECK(media_url_requested(big_sized) && ui_media_failed(F.ui, big_sized));
  CT_CHECK(media_url_requested("https://cdn.discordapp.com/attachments/1/2/big.png"));
  CT_CHECK(ccord_render_has_texture(F.renderer, "https://cdn.discordapp.com/attachments/1/2/big.png", nullptr, nullptr));
  remove_message(ch, mid);

  ccord_mock_fail_media(F.transport, "gone.png");
  ccord_attachment gone_att = {.id = 503, .filename = "gone.png", .content_type = "image/png", .size = 900,
                               .url = "https://cdn.discordapp.com/attachments/1/2/gone.png", .width = 300, .height = 200};
  ccord_message_rich r7 = {.present = CCORD_RICH_ALL, .attachments = &gone_att, .attachment_count = 1};
  mid = append_rich_message(ch, "", &r7);
  frame(4);
  CT_CHECK(ui_media_failed(F.ui, gone_att.url));
  CT_CHECK(!ccord_render_has_texture(F.renderer, gone_att.url, nullptr, nullptr));
  size_t reqs = dbg().media_requests;
  frame(20);   // one second: still inside the back-off
  CT_CHECK(dbg().media_requests == reqs);
  ccord_mock_fail_media(F.transport, nullptr);
  F.now += 16000;   // past the first back-off step
  F.ui->dirty = true;
  frame(4);
  CT_CHECK(dbg().media_requests == reqs + 1u);
  CT_CHECK(!ui_media_failed(F.ui, gone_att.url));
  CT_CHECK(ccord_render_has_texture(F.renderer, gone_att.url, nullptr, nullptr));
  remove_message(ch, mid);

  // 7. A video that cannot be downloaded says so, and pressing play again asks again.
  ccord_mock_fail_media(F.transport, "lost.mp4");
  ccord_attachment lost = {.id = 504, .filename = "lost.mp4", .content_type = "video/mp4", .size = 2048,
                           .url = "https://cdn.discordapp.com/attachments/1/2/lost.mp4", .width = 320, .height = 180};
  ccord_message_rich r8 = {.present = CCORD_RICH_ALL, .attachments = &lost, .attachment_count = 1};
  mid = append_rich_message(ch, "", &r8);
  frame(2);
  CT_CHECK(F.ui->nmedia_hits >= 1);
  click(F.ui->media_hits[0].play_rect.x + 12.0f, F.ui->media_hits[0].play_rect.y + 12.0f);
  frame(4);
  CT_CHECK(F.ui->media_failed && !F.ui->media_loaded && !dbg().media_playing);
  ccord_mock_fail_media(F.transport, nullptr);
  click(F.ui->media_hits[0].play_rect.x + 12.0f, F.ui->media_hits[0].play_rect.y + 12.0f);
  frame(4);
  CT_CHECK(!F.ui->media_failed && F.ui->media_loaded);
  if (dbg().media_playing) click(F.ui->media_hits[0].play_rect.x + 12.0f, F.ui->media_hits[0].play_rect.y + 12.0f);
  remove_message(ch, mid);

  CT_CHECK(ccord_store_delete_channel(F.store, ch, &err) == CCORD_OK);
  frame(2);
}

static void test_polls_components_and_modals(void)
{
  ccord_error err = {0};
  // Create a dedicated empty channel in C-Cord HQ (2001) via session admin so both mock_transport and store know it.
  ccord_admin_request areq;
  CT_CHECK(ccord_admin_create_channel(&areq, 2001, "polls-lab", 0, 3110, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &areq, &err) == CCORD_OK);
  frame(3);
  ccord_id lab_ch = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, 2001); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, 2001, i);
    if (c && strcmp(c->name, "polls-lab") == 0) {
      lab_ch = c->id;
      break;
    }
  }
  CT_CHECK(lab_ch != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, lab_ch, &err) == CCORD_OK);
  frame(3);

  // 1. Single-select poll: vote option 1, switch to option 2, click option 2 again to unvote.
  ccord_message_rich r_poll = {
    .present = CCORD_RICH_ALL,
    .poll = {
      .present = true,
      .question = "Best rendering backend?",
      .answer_count = 2,
      .answers = {
        {.id = 1, .text = "Vulkan", .votes = 2},
        {.id = 2, .text = "OpenGL", .votes = 1},
      },
      .multiselect = false,
      .finalized = false,
    },
  };
  ccord_id poll_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, lab_ch, 1011, "", &r_poll, &poll_mid, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(dbg().poll_cards == 1 && dbg().poll_option_count == 2);

  ccord_rect opt0 = dbg().poll_option_rects[0];
  ccord_rect opt1 = dbg().poll_option_rects[1];
  click(opt0.x + 20.0f, opt0.y + opt0.h * 0.5f);
  const ccord_message *pm = ccord_store_find_message(F.store, lab_ch, poll_mid);
  CT_CHECK(pm && pm->rich && pm->rich->poll.answers[0].me_voted && pm->rich->poll.answers[0].votes == 3);

  click(opt1.x + 20.0f, opt1.y + opt1.h * 0.5f);
  pm = ccord_store_find_message(F.store, lab_ch, poll_mid);
  CT_CHECK(pm && pm->rich && !pm->rich->poll.answers[0].me_voted && pm->rich->poll.answers[0].votes == 2);
  CT_CHECK(pm && pm->rich && pm->rich->poll.answers[1].me_voted && pm->rich->poll.answers[1].votes == 2);

  click(opt1.x + 20.0f, opt1.y + opt1.h * 0.5f);
  pm = ccord_store_find_message(F.store, lab_ch, poll_mid);
  CT_CHECK(pm && pm->rich && !pm->rich->poll.answers[1].me_voted && pm->rich->poll.answers[1].votes == 1);
  CT_CHECK(ccord_session_delete_message(F.session, lab_ch, poll_mid, &err) == CCORD_OK);
  frame(2);

  // 2. Multiselect poll + finalized poll non-reactivity.
  r_poll.poll.multiselect = true;
  ccord_id mpoll_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, lab_ch, 1011, "", &r_poll, &mpoll_mid, &err) == CCORD_OK);
  frame(3);
  click(dbg().poll_option_rects[0].x + 20.0f, dbg().poll_option_rects[0].y + 14.0f);
  click(dbg().poll_option_rects[1].x + 20.0f, dbg().poll_option_rects[1].y + 14.0f);
  pm = ccord_store_find_message(F.store, lab_ch, mpoll_mid);
  CT_CHECK(pm && pm->rich->poll.answers[0].me_voted && pm->rich->poll.answers[1].me_voted);
  CT_CHECK(ccord_session_delete_message(F.session, lab_ch, mpoll_mid, &err) == CCORD_OK);
  frame(2);

  r_poll.poll.finalized = true;
  ccord_id cpoll_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, lab_ch, 1011, "", &r_poll, &cpoll_mid, &err) == CCORD_OK);
  frame(3);
  // Finalized poll marks poll_hits closed, so clicking option 0 changes nothing.
  CT_CHECK(dbg().poll_cards == 1 && dbg().poll_option_count == 2);
  click(dbg().poll_option_rects[0].x + 20.0f, dbg().poll_option_rects[0].y + 14.0f);
  pm = ccord_store_find_message(F.store, lab_ch, cpoll_mid);
  CT_CHECK(pm && pm->rich && !pm->rich->poll.answers[0].me_voted && pm->rich->poll.answers[0].votes == 2);
  CT_CHECK(ccord_session_delete_message(F.session, lab_ch, cpoll_mid, &err) == CCORD_OK);
  frame(2);

  // 3. Message components: Primary, Disabled, Link, Premium buttons; String Select (min=2, max=2); Role Select; Channel Select.
  ccord_component btn_items[4] = {
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = "Deploy", .custom_id = "btn:deploy"},
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_DANGER, .label = "Locked", .custom_id = "btn:locked", .disabled = true},
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_LINK, .label = "Docs", .url = "https://discord.com/developers/docs"},
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PREMIUM, .label = "Upgrade"},
  };
  ccord_select_option str_opts[3] = {
    {.label = "Linux", .value = "os_linux", .description = "ELF64"},
    {.label = "FreeBSD", .value = "os_bsd", .description = "kqueue"},
    {.label = "macOS", .value = "os_mac", .description = "Mach"},
  };
  ccord_component row_s1 = {
    .kind = CCORD_COMP_STRING_SELECT,
    .custom_id = "sel:os",
    .placeholder = "Pick 2 targets",
    .min_values = 2,
    .max_values = 2,
    .options = str_opts,
    .option_count = 3,
  };
  ccord_component row_s2 = {
    .kind = CCORD_COMP_ROLE_SELECT,
    .custom_id = "sel:role",
    .placeholder = "Pick a role",
    .min_values = 1,
    .max_values = 1,
  };
  ccord_component row_s3 = {
    .kind = CCORD_COMP_CHANNEL_SELECT,
    .custom_id = "sel:chan",
    .placeholder = "Pick a channel",
    .min_values = 1,
    .max_values = 1,
  };
  ccord_component btn_modal = {
    .kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_SECONDARY, .label = "Feedback", .custom_id = "modal:feedback",
  };
  ccord_component_row crows[5] = {
    {.components = btn_items, .component_count = 4},
    {.components = &row_s1, .component_count = 1},
    {.components = &row_s2, .component_count = 1},
    {.components = &row_s3, .component_count = 1},
    {.components = &btn_modal, .component_count = 1},
  };
  ccord_message_rich r_comp = {
    .present = CCORD_RICH_ALL,
    .application_id = 888001,
    .components = crows,
    .component_row_count = 5,
  };
  ccord_id comp_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, lab_ch, 1011, "Bot panel", &r_comp, &comp_mid, &err) == CCORD_OK);
  frame(3);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.component_rows_drawn == 5 && d.component_buttons_drawn == 5 && d.component_selects_drawn == 3);
  CT_CHECK(d.component_rect_count == 8);

  size_t ic0 = ccord_mock_interaction_count(F.transport);
  // Primary button -> sends MESSAGE_COMPONENT interaction.
  click(d.component_rects[0].x + 12.0f, d.component_rects[0].y + 14.0f);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 1 &&
           strcmp(ccord_mock_last_interaction_custom_id(F.transport), "btn:deploy") == 0);

  // Disabled button -> ignored.
  click(d.component_rects[1].x + 12.0f, d.component_rects[1].y + 14.0f);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 1);

  // Link button & Premium button -> open URL in browser without sending component interaction.
  int url0 = F.open_url_calls;
  click(d.component_rects[2].x + 12.0f, d.component_rects[2].y + 14.0f);
  CT_CHECK(F.open_url_calls == url0 + 1 && strcmp(F.last_url, "https://discord.com/developers/docs") == 0);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 1);

  click(d.component_rects[3].x + 12.0f, d.component_rects[3].y + 14.0f);
  CT_CHECK(F.open_url_calls == url0 + 2 && strcmp(F.last_url, "https://discord.com") == 0);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 1);

  // Multi-select String Select (min_values=2, max_values=2):
  click(d.component_rects[4].x + 20.0f, d.component_rects[4].y + 16.0f);
  d = dbg();
  CT_CHECK(d.select_dropdown_open && d.select_dropdown_option_count == 3 && d.select_confirm_btn.w > 0.0f);
  // Pick 1 option then click Apply -> min_values notice, dropdown stays open, no interaction sent.
  click(d.select_option_rects[0].x + 20.0f, d.select_option_rects[0].y + 12.0f);
  click(dbg().select_confirm_btn.x + 16.0f, dbg().select_confirm_btn.y + 10.0f);
  CT_CHECK(dbg().select_dropdown_open && strstr(F.ui->notice, "Select at least 2 options") != nullptr);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 1);
  // Pick 2nd option, try picking 3rd option -> max_values notice, then click Apply -> sends 2 values!
  click(dbg().select_option_rects[1].x + 20.0f, dbg().select_option_rects[1].y + 12.0f);
  click(dbg().select_option_rects[2].x + 20.0f, dbg().select_option_rects[2].y + 12.0f);
  CT_CHECK(strstr(F.ui->notice, "You can select at most 2 options") != nullptr);
  click(dbg().select_confirm_btn.x + 16.0f, dbg().select_confirm_btn.y + 10.0f);
  CT_CHECK(!dbg().select_dropdown_open && ccord_mock_interaction_count(F.transport) == ic0 + 2);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "sel:os") == 0 &&
           ccord_mock_last_interaction_value_count(F.transport) == 2 &&
           strcmp(ccord_mock_last_interaction_value(F.transport, 0), "os_linux") == 0 &&
           strcmp(ccord_mock_last_interaction_value(F.transport, 1), "os_bsd") == 0);

  // Single-select Role Select (populated from guild roles in store): clicking an option submits immediately.
  d = dbg();
  click(d.component_rects[5].x + 20.0f, d.component_rects[5].y + 16.0f);
  CT_CHECK(dbg().select_dropdown_open && dbg().select_dropdown_option_count >= 2);
  click(dbg().select_option_rects[0].x + 20.0f, dbg().select_option_rects[0].y + 12.0f);
  CT_CHECK(!dbg().select_dropdown_open && ccord_mock_interaction_count(F.transport) == ic0 + 3 &&
           ccord_mock_last_interaction_component_type(F.transport) == CCORD_COMP_ROLE_SELECT &&
           ccord_mock_last_interaction_value_count(F.transport) == 1);

  // Single-select Channel Select (populated from guild channels in store):
  d = dbg();
  click(d.component_rects[6].x + 20.0f, d.component_rects[6].y + 16.0f);
  CT_CHECK(dbg().select_dropdown_open && dbg().select_dropdown_option_count >= 2);
  click(dbg().select_option_rects[0].x + 20.0f, dbg().select_option_rects[0].y + 12.0f);
  CT_CHECK(!dbg().select_dropdown_open && ccord_mock_interaction_count(F.transport) == ic0 + 4 &&
           ccord_mock_last_interaction_component_type(F.transport) == CCORD_COMP_CHANNEL_SELECT);

  // 4. Interaction modal ("modal:feedback"): validation (required + min_length), Tab + multiline Enter, Submit.
  d = dbg();
  click(d.component_rects[7].x + 16.0f, d.component_rects[7].y + 14.0f);
  frame(2);
  d = dbg();
  CT_CHECK(d.interaction_modal_open && d.interaction_modal_field_count == 2);
  // Empty submit -> "Subject is required".
  click(d.interaction_modal_submit_btn.x + 20.0f, d.interaction_modal_submit_btn.y + 12.0f);
  CT_CHECK(dbg().interaction_modal_open && strstr(F.ui->notice, "Subject is required") != nullptr);
  // 1-char subject -> "Subject must be at least 2 characters".
  type_text("A");
  frame(1);
  click(d.interaction_modal_submit_btn.x + 20.0f, d.interaction_modal_submit_btn.y + 12.0f);
  CT_CHECK(dbg().interaction_modal_open && strstr(F.ui->notice, "Subject must be at least 2 characters") != nullptr);
  // Complete Subject ("API parity"), Tab to paragraph Details field, type 2 lines with plain Enter, then Submit.
  type_text("PI parity");
  key(CCORD_KEY_TAB, 0);
  type_text("Line 1");
  key(CCORD_KEY_RETURN, 0); // paragraph field inserts newline instead of submitting
  type_text("Line 2");
  frame(1);
  CT_CHECK(dbg().interaction_modal_open);
  click(dbg().interaction_modal_submit_btn.x + 20.0f, dbg().interaction_modal_submit_btn.y + 12.0f);
  frame(2);
  CT_CHECK(!dbg().interaction_modal_open);
  CT_CHECK(ccord_mock_last_interaction_type(F.transport) == CCORD_INTERACTION_MODAL_SUBMIT);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(F.transport, 0), "API parity") == 0);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(F.transport, 1), "Line 1\nLine 2") == 0);
  const ccord_message *last_m =
    ccord_store_message_at(F.store, lab_ch, ccord_store_message_count(F.store, lab_ch) - 1u);
  CT_CHECK(last_m && strcmp(last_m->content, "Modal submitted: API parity") == 0);

  // 5. Refused writes surface accurate failure toasts ("Couldn't use that control" / "Couldn't vote").
  ccord_mock_set_refuse_writes(F.transport, true);
  // Scroll up or click Deploy button if visible;comp_mid is right above last_m so still on screen:
  d = dbg();
  CT_CHECK(d.component_rect_count >= 1);
  click(d.component_rects[0].x + 12.0f, d.component_rects[0].y + 14.0f);
  frame(2);
  CT_CHECK(strstr(F.ui->notice, "Couldn't use that control") != nullptr);

  r_poll.poll.finalized = false;
  r_poll.poll.multiselect = false;
  ccord_id fail_poll = 0;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, lab_ch, 1011, "", &r_poll, &fail_poll, &err) == CCORD_OK);
  frame(3);
  click(dbg().poll_option_rects[0].x + 20.0f, dbg().poll_option_rects[0].y + 14.0f);
  frame(2);
  CT_CHECK(strstr(F.ui->notice, "Couldn't vote") != nullptr);
  ccord_mock_set_refuse_writes(F.transport, false);

  // Clean up test channel so the fixture leaves the server as it found it.
  CT_CHECK(ccord_admin_delete_channel(&areq, 2001, lab_ch, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &areq, &err) == CCORD_OK);
  frame(3);
}

static bool autocomplete_has(const char *substr)
{
  if (!F.ui || !F.ui->autocomplete_open) return false;
  for (size_t i = 0; i < F.ui->nautocomplete; i++) {
    if (strstr(F.ui->autocomplete_items[i].display, substr) != nullptr) return true;
  }
  return false;
}

static void clear_composer_input(void)
{
  if (F.ui && F.ui->autocomplete_open) key(CCORD_KEY_ESCAPE, 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
}

static void test_ephemeral_slash_mentions_uploads_reply_ping_slowmode_and_drafts(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);

  // 1. Mention serialization (@user, @role, #channel -> <@...>, <@&...>, <#...>), preserving code/URLs/raw/emails.
  size_t rc = 0;
  const ccord_role *roles = ccord_store_roles(F.store, 2001, &rc);
  ccord_id mod_role_id = CCORD_ID_NONE;
  for (size_t i = 0; i < rc; i++) {
    if (roles[i].name && strcmp(roles[i].name, "Moderators") == 0) {
      mod_role_id = roles[i].id;
      break;
    }
  }
  CT_CHECK(mod_role_id != CCORD_ID_NONE);

  const char *raw_mentions =
    "hi @Ada and @ada and @Moderators in #dev; keep `@Ada` ```@Ada``` https://example.com/@Ada <@1002> @everyone @here mail@ada.org";
  char ser_out[512];
  size_t ser_len = ccord_ui_serialize_mentions(F.store, 2001, raw_mentions, strlen(raw_mentions), ser_out, sizeof ser_out);
  char expected_ser[512];
  int exp_n = snprintf(
    expected_ser, sizeof expected_ser,
    "hi <@1002> and <@1002> and <@&%llu> in <#3113>; keep `@Ada` ```@Ada``` https://example.com/@Ada <@1002> @everyone @here mail@ada.org",
    (unsigned long long)mod_role_id);
  CT_CHECK(exp_n > 0 && ser_len == (size_t)exp_n && strcmp(ser_out, expected_ser) == 0);

  clear_composer_input();
  type_text("hello @Ada in #dev");
  if (F.ui->autocomplete_open) key(CCORD_KEY_ESCAPE, 0);
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  const ccord_message *sent_m =
    ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1u);
  CT_CHECK(sent_m && strcmp(sent_m->content, "hello <@1002> in <#3113>") == 0);

  // 2. @role & @everyone/@here autocomplete respects mentionable || MENTION_EVERYONE.
  clear_composer_input();
  type_text("@Adm");
  frame(1);
  CT_CHECK(autocomplete_has("@Admins (role)"));
  clear_composer_input();
  type_text("@Mod");
  frame(1);
  CT_CHECK(autocomplete_has("@Moderators (role)"));
  clear_composer_input();
  type_text("@Mem");
  frame(1);
  CT_CHECK(autocomplete_has("@Members (role)"));
  clear_composer_input();
  type_text("@every");
  frame(1);
  CT_CHECK(autocomplete_has("@everyone"));
  clear_composer_input();
  type_text("@her");
  frame(1);
  CT_CHECK(autocomplete_has("@here"));
  clear_composer_input();

  // Switch to guild 2003 channel 3315 (self is plain Member without MENTION_EVERYONE or MANAGE_MESSAGES).
  CT_CHECK(ccord_session_select_guild(F.session, 2003, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3315, &err) == CCORD_OK);
  frame(4);

  type_text("@Adm");
  frame(1);
  CT_CHECK(!autocomplete_has("@Admins (role)"));
  clear_composer_input();
  type_text("@Mod");
  frame(1);
  CT_CHECK(autocomplete_has("@Moderators (role)"));
  clear_composer_input();
  type_text("@Mem");
  frame(1);
  CT_CHECK(!autocomplete_has("@Members (role)"));
  clear_composer_input();
  type_text("@every");
  frame(1);
  CT_CHECK(!autocomplete_has("@everyone"));
  clear_composer_input();
  type_text("@her");
  frame(1);
  CT_CHECK(!autocomplete_has("@here"));
  clear_composer_input();

  // 3. Slash commands: governed index fetch + caching, permission filtering, autocomplete debounce, validation, ephemeral dismiss.
  size_t sidx0 = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SLASH_INDEX);
  type_text("/");
  frame(3);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SLASH_INDEX) == sidx0 + 1u);
  CT_CHECK(autocomplete_has("/ping") && !autocomplete_has("/purge"));
  type_text("p");
  frame(1);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SLASH_INDEX) == sidx0 + 1u);
  CT_CHECK(autocomplete_has("/ping") && !autocomplete_has("/purge"));
  clear_composer_input();

  // Switch back to guild 2001 channel 3111 (Admin -> /purge allowed).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);

  type_text("/");
  frame(3);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SLASH_INDEX) == sidx0 + 2u);
  CT_CHECK(autocomplete_has("/ping") && autocomplete_has("/purge"));

  // Option validation: /purge requires an integer `count`.
  clear_composer_input();
  size_t ic_before = ccord_mock_interaction_count(F.transport);
  type_text("/purge abc");
  key(CCORD_KEY_RETURN, 0);
  frame(1);
  CT_CHECK(strstr(F.ui->notice, "must be a valid integer") != nullptr);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic_before);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "/purge abc") == 0);

  // Debounced dynamic option autocomplete (`POST /interactions` type 4 after 250 ms).
  clear_composer_input();
  type_text("/topic vul");
  frame(1); // 50 ms < 250 ms debounce
  CT_CHECK(!autocomplete_has("Vulkan Render Pipelines"));
  frame(6); // +300 ms -> fires type 4 and receives APPLICATION_COMMAND_AUTOCOMPLETE_RESULT
  CT_CHECK(ccord_mock_last_interaction_type(F.transport) == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE);
  CT_CHECK(autocomplete_has("Vulkan Render Pipelines"));
  // Accept dynamic choice (note: item 0 is option-name suggestion `subject:*`, item 1 is the dynamic choice; select the choice):
  for (size_t i = 0; i < F.ui->nautocomplete; i++) {
    if (strstr(F.ui->autocomplete_items[i].display, "Vulkan Render Pipelines") != nullptr) {
      F.ui->autocomplete_sel = i;
      break;
    }
  }
  key(CCORD_KEY_TAB, 0);
  CT_CHECK(strstr(ccord_ui_composer_text(F.ui), "vulkan") != nullptr);
  if (F.ui->autocomplete_open) key(CCORD_KEY_ESCAPE, 0);
  key(CCORD_KEY_RETURN, 0);
  frame(45); // > 2000 ms (UI_ACK_MIN_GAP_MS) so read-ack settles on non-ephemeral message
  const ccord_message *topic_m =
    ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1u);
  CT_CHECK(topic_m && !ccord_message_is_ephemeral(topic_m) && strcmp(topic_m->content, "Command /topic: vulkan") == 0);
  ccord_id acked_before_ephem = dbg().acked_message;
  CT_CHECK(acked_before_ephem == topic_m->id);

  // Ephemeral `/ping`: never advances read ACK, renders "Dismiss message", removes locally without Discord DELETE.
  type_text("/ping");
  if (F.ui->autocomplete_open) key(CCORD_KEY_ESCAPE, 0);
  key(CCORD_KEY_RETURN, 0);
  frame(45); // > 2000 ms (UI_ACK_MIN_GAP_MS) to prove auto_ack skips ephemeral tail
  const ccord_message *ephem_m =
    ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1u);
  CT_CHECK(ephem_m && ccord_message_is_ephemeral(ephem_m) && strcmp(ephem_m->content, "Pong! 12 ms") == 0);
  ccord_id ephem_id = ephem_m ? ephem_m->id : CCORD_ID_NONE;
  ccord_ui_debug d = dbg();
  CT_CHECK(d.acked_message == acked_before_ephem);
  CT_CHECK(d.ephemeral_dismiss_count >= 1u);
  if (d.ephemeral_dismiss_count >= 1u) {
    ccord_rect dis_r = d.ephemeral_dismiss_rects[d.ephemeral_dismiss_count - 1u];
    CT_CHECK(dis_r.w > 0.0f && dis_r.h > 0.0f);
    F.ui->notice[0] = '\0';
    ccord_mock_set_refuse_writes(F.transport, true);
    click(dis_r.x + 6.0f, dis_r.y + 6.0f);
    ccord_mock_set_refuse_writes(F.transport, false);
    CT_CHECK(ccord_store_find_message(F.store, 3111, ephem_id) == nullptr);
    CT_CHECK(dbg().ephemeral_dismiss_count == 0u);
    CT_CHECK(F.ui->notice[0] == '\0');
  }

  // 4. Multi-file upload: 10-attachment cap, per-file spoiler toggle, remove staged attachment, send.
  static const uint8_t k_blob[4] = {'T', 'E', 'S', 'T'};
  ccord_ui_clear_attachment(F.ui);
  for (unsigned i = 0; i < 10u; i++) {
    char fn[32];
    snprintf(fn, sizeof fn, "f%u.txt", i);
    CT_CHECK(ccord_ui_attach_bytes(F.ui, fn, k_blob, sizeof k_blob));
  }
  CT_CHECK(!ccord_ui_attach_bytes(F.ui, "f10.txt", k_blob, sizeof k_blob));
  CT_CHECK(strstr(F.ui->notice, "Maximum of 10 attachments") != nullptr);
  ccord_ui_clear_attachment(F.ui);

  CT_CHECK(ccord_ui_attach_bytes(F.ui, "one.txt", k_blob, sizeof k_blob));
  CT_CHECK(ccord_ui_attach_bytes(F.ui, "two.png", k_blob, sizeof k_blob));
  CT_CHECK(ccord_ui_attach_bytes(F.ui, "three.log", k_blob, sizeof k_blob));
  frame(2);
  d = dbg();
  CT_CHECK(d.staged_attachment_count == 3u && !d.staged_spoilers[0] && !d.staged_spoilers[1]);
  click(d.staged_spoiler_rects[1].x + 4.0f, d.staged_spoiler_rects[1].y + 4.0f);
  d = dbg();
  CT_CHECK(!d.staged_spoilers[0] && d.staged_spoilers[1]);
  click(d.staged_remove_rects[2].x + 4.0f, d.staged_remove_rects[2].y + 4.0f);
  d = dbg();
  CT_CHECK(d.staged_attachment_count == 2u);
  clear_composer_input();
  type_text("multi upload");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  CT_CHECK(dbg().staged_attachment_count == 0u);
  const ccord_message *up_m =
    ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1u);
  CT_CHECK(up_m && up_m->rich && up_m->rich->attachment_count == 2u);
  CT_CHECK(!up_m->rich->attachments[0].spoiler && strcmp(up_m->rich->attachments[0].filename, "one.txt") == 0);
  CT_CHECK(up_m->rich->attachments[1].spoiler && strcmp(up_m->rich->attachments[1].filename, "SPOILER_two.png") == 0);

  // 5. Reply-ping `@ ON` / `@ OFF` toggle.
  ccord_id self_id = ccord_store_self_id(F.store);
  ccord_id other_msg_id = CCORD_ID_NONE;
  ccord_id other_author = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_message_count(F.store, 3111); i++) {
    const ccord_message *cand = ccord_store_message_at(F.store, 3111, i);
    if (cand && cand->author_id != self_id && cand->author_id != 1011 && !ccord_message_is_ephemeral(cand)) {
      other_msg_id = cand->id;
      other_author = cand->author_id;
      break;
    }
  }
  CT_CHECK(other_msg_id != CCORD_ID_NONE);
  ui_begin_reply(F.ui, 3111, other_msg_id);
  frame(2);
  d = dbg();
  CT_CHECK(d.replying && d.reply_ping && d.reply_ping_btn.w > 0.0f);
  click(d.reply_ping_btn.x + 8.0f, d.reply_ping_btn.y + 8.0f);
  CT_CHECK(!dbg().reply_ping);
  type_text("silent reply");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  const ccord_message *sil_m =
    ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1u);
  CT_CHECK(sil_m && sil_m->rich && sil_m->rich->reference.kind == CCORD_REF_REPLY &&
           sil_m->rich->mention_user_count == 0u);

  ui_begin_reply(F.ui, 3111, other_msg_id);
  frame(2);
  CT_CHECK(dbg().reply_ping);
  type_text("loud reply");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  const ccord_message *loud_m =
    ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1u);
  CT_CHECK(loud_m && loud_m->rich && loud_m->rich->mention_user_count == 1u &&
           loud_m->rich->mention_users[0] == other_author);

  // 6. Slowmode countdown & Admin bypass.
  CT_CHECK(ccord_session_select_channel(F.session, 3114, &err) == CCORD_OK);
  frame(3);
  clear_composer_input();
  type_text("admin fast 1");
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(dbg().slowmode_remaining_s == 0u);
  type_text("admin fast 2");
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  const ccord_message *adm_m =
    ccord_store_message_at(F.store, 3114, ccord_store_message_count(F.store, 3114) - 1u);
  CT_CHECK(adm_m && strcmp(adm_m->content, "admin fast 2") == 0);

  CT_CHECK(ccord_session_select_guild(F.session, 2003, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3315, &err) == CCORD_OK);
  frame(3);
  clear_composer_input();
  type_text("member slow 1");
  key(CCORD_KEY_RETURN, 0);
  frame(1);
  CT_CHECK(dbg().slowmode_remaining_s > 0u && dbg().slowmode_remaining_s <= 5u);
  type_text("member slow 2");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(strstr(F.ui->notice, "Slowmode active") != nullptr);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "member slow 2") == 0);
  frame(105); // > 5000 ms
  CT_CHECK(dbg().slowmode_remaining_s == 0u);
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "") == 0);
  const ccord_message *mem_m =
    ccord_store_message_at(F.store, 3315, ccord_store_message_count(F.store, 3315) - 1u);
  CT_CHECK(mem_m && strcmp(mem_m->content, "member slow 2") == 0);

  // 7. Persistent per-channel drafts across UI restarts (`drafts.json`).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(3);
  clear_composer_input();
  type_text("draft in general");
  frame(1);

  CT_CHECK(ccord_session_select_channel(F.session, 3113, &err) == CCORD_OK);
  ui_save_current_location(F.ui);
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "") == 0);
  type_text("draft in dev");
  frame(1);

  ccord_ui_destroy(F.ui);
  ccord_ui_deps deps = {
    .bus = F.bus,
    .store = F.store,
    .session = F.session,
    .contrib = F.contrib,
    .theme = &F.theme,
    .host = {.clipboard_set = host_clip_set, .text_input = host_text_input,
             .set_extensions_enabled = host_set_extensions, .open_url = host_open_url,
             .open_file_dialog = host_open_file},
    .fixed_now_unix_ms = 1793534400000LL + 30 * 60 * 1000,
  };
  F.ui = ccord_ui_create(&deps, &err);
  CT_CHECK(F.ui != nullptr);
  ccord_ui_set_viewport(F.ui, 1280, 800);
  frame(2);
  CT_CHECK(ccord_store_selected_channel(F.store) == 3113);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "draft in dev") == 0);

  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "draft in general") == 0);
  key(CCORD_KEY_RETURN, 0); // sending clears channel 3111's draft
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "") == 0);

  CT_CHECK(ccord_session_select_channel(F.session, 3113, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "draft in dev") == 0);
  clear_composer_input();
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "") == 0);
}

static void click_rect(ccord_rect r)
{
  click(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}

static void test_search_panel_paginated_pins_threads_and_burst_reactions_ui(void)
{
  ccord_error err;
  ccord_ui_set_viewport(F.ui, 1280.0f, 800.0f);
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);

  // Post a distinctive message in #dev (3113) so we can test cross-channel `in:dev` search jump!
  CT_CHECK(ccord_session_send_message(F.session, 3113, "cross-channel-target-msg", 24, &err) == CCORD_OK);
  frame(4);

  // 1. Search Results side panel + Next/Prev pagination across #big's messages in guild 2001.
  click_rect(dbg().search_btn);
  frame(2);
  CT_CHECK(dbg().search_open);
  type_text("in");
  key(CCORD_KEY_RETURN, 0);
  frame(6);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.search_panel_open && d.search_total_results > 25 && d.search_offset == 0 && d.search_panel_cards > 0);
  CT_CHECK(d.search_next_btn.w > 0.0f && d.search_prev_btn.w == 0.0f);

  click_rect(d.search_next_btn);
  frame(6);
  d = dbg();
  CT_CHECK(d.search_offset == 25 && d.search_prev_btn.w > 0.0f);

  click_rect(d.search_prev_btn);
  frame(6);
  CT_CHECK(dbg().search_offset == 0);

  // Cross-channel `in:dev` operator + clicking card jumps to #dev (3113) and highlights target message!
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  type_text("in:dev cross-channel-target-msg");
  key(CCORD_KEY_RETURN, 0);
  frame(6);
  d = dbg();
  CT_CHECK(d.search_panel_open && d.search_total_results == 1 && d.search_panel_cards == 1);
  click_rect(d.search_card_rects[0]);
  frame(6);
  CT_CHECK(!dbg().search_open && ccord_store_selected_channel(F.store) == 3113);
  CT_CHECK(dbg().jump_highlight_message != CCORD_ID_NONE);

  // Return to #general (3111).
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);

  // 2. Paginated Pins popover: Load more + click-to-jump.
  click_rect(dbg().pin_btn);
  frame(4);
  CT_CHECK(dbg().pins_open);
  const ccord_message *gm0 = ccord_store_message_at(F.store, 3111, 0);
  CT_CHECK(gm0 != nullptr);
  ccord_message fake_pin = {
    .id = gm0->id, .channel_id = 3111, .author_id = gm0->author_id,
    .timestamp_ms = gm0->timestamp_ms, .content = "pinned item", .pinned = true,
  };
  CT_CHECK(ccord_store_set_pins_page(F.store, 3111, &fake_pin, 1, false, true, "9999999999999999999", &err) == CCORD_OK);
  frame(2);
  d = dbg();
  CT_CHECK(d.pins_count >= 1 && d.pins_has_more && d.pins_load_more_btn.w > 0.0f);
  size_t pins_calls0 = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_PINS);
  click_rect(d.pins_load_more_btn);
  frame(4);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_PINS) == pins_calls0 + 1);
  d = dbg();
  CT_CHECK(d.pin_item_rects[0].w > 0.0f);
  click_rect(d.pin_item_rects[0]);
  frame(2);
  CT_CHECK(!dbg().pins_open && dbg().jump_highlight_message != CCORD_ID_NONE);

  // 3. Real Threads UI: Create Thread from message -> DECOR_THREAD card -> open -> Join/Archive -> breadcrumb -> hidden when archived!
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(2);
  CT_CHECK(ccord_session_send_message(F.session, 3111, "Thread starter message!", 23, &err) == CCORD_OK);
  frame(4);
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float hx = l->message_area.x + 260.0f;
  float hy = scan_hover_self_message(hx);
  CT_CHECK(hy > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, hx, hy);
  frame(2);
  CT_CHECK(dbg().context_menu_open);
  CT_CHECK(menu_click_label("Create Thread"));
  frame(2);
  type_text("Triage Thread");
  key(CCORD_KEY_RETURN, 0);
  frame(6);

  d = dbg();
  CT_CHECK(d.thread_cards_drawn >= 1 && d.thread_card_rects[0].w > 0.0f);
  click_rect(d.thread_card_rects[0]);
  frame(6);
  ccord_id tid = ccord_store_selected_channel(F.store);
  CT_CHECK(tid != 3111 && ccord_store_channel(F.store, tid) != nullptr);
  d = dbg();
  CT_CHECK(d.thread_breadcrumb_rect.w > 0.0f && d.thread_join_btn.w > 0.0f && d.thread_archive_btn.w > 0.0f);
  CT_CHECK(ccord_store_channel(F.store, tid)->thread_joined);

  // Toggle Joined -> Leave -> Join
  click_rect(d.thread_join_btn);
  frame(4);
  CT_CHECK(!ccord_store_channel(F.store, tid)->thread_joined);
  click_rect(dbg().thread_join_btn);
  frame(4);
  CT_CHECK(ccord_store_channel(F.store, tid)->thread_joined);

  // Archive thread, then click `#general` breadcrumb -> archived thread is hidden from channel sidebar!
  click_rect(dbg().thread_archive_btn);
  frame(4);
  CT_CHECK(ccord_store_channel(F.store, tid)->thread_archived);
  click_rect(dbg().thread_breadcrumb_rect);
  frame(4);
  CT_CHECK(ccord_store_selected_channel(F.store) == 3111);
  // Scan channel list for `tid`: should NOT be hovered anywhere in channel_scroll because archived & unselected!
  l = ccord_ui_get_layout(F.ui);
  bool saw_archived_in_sidebar = false;
  for (float sy = l->channel_scroll.y + 4.0f; sy < l->channel_scroll.y + l->channel_scroll.h; sy += 8.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, l->channel_scroll.x + 40.0f, sy);
    if (dbg().hover_channel == tid) { saw_archived_in_sidebar = true; break; }
  }
  CT_CHECK(!saw_archived_in_sidebar);

  // 4. Burst Reaction pills & Who-Reacted popover.
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(2);
  size_t mc_cnt = ccord_store_message_count(F.store, 3111);
  const ccord_message *last_gm = ccord_store_message_at(F.store, 3111, mc_cnt - 1u);
  CT_CHECK(last_gm != nullptr);
  ccord_id last_gm_id = last_gm->id;
  CT_CHECK(ccord_session_toggle_reaction(F.session, 3111, last_gm_id, "\xF0\x9F\x91\x8D", 4, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_store_set_reaction_ex(F.store, 3111, last_gm_id, "\xF0\x9F\x94\xA5", 3, false, 2, true, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(dbg().burst_reaction_pills_drawn >= 1);

  hy = -1.0f;
  for (float sy = l->message_area.y + l->message_area.h - 20.0f; sy > l->message_area.y + 40.0f; sy -= 4.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, hx, sy);
    if (dbg().hover_message == last_gm_id) { hy = sy; break; }
  }
  CT_CHECK(hy > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, hx, hy);
  frame(2);
  CT_CHECK(dbg().context_menu_open);
  CT_CHECK(menu_click_label("View Reactions"));
  frame(5);
  d = dbg();
  CT_CHECK(d.reactions_popover_open && d.reactions_tab_count >= 2 && d.reactions_user_count >= 1);
  size_t ru_calls0 = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_REACTION_USERS);
  click_rect(d.reactions_tab_rects[1]);
  frame(5);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_REACTION_USERS) == ru_calls0 + 1);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!dbg().reactions_popover_open);

  // Settle any pending auto-ACK before finishing.
  frame(45);
}

static void test_navigation_history_alt_shortcuts_and_inbox(void)
{
  ccord_error err = {0};

  // 1. SDL3 platform translation: SDLK_SLASH / SDLK_QUESTION -> CCORD_KEY_SLASH, SDL_BUTTON_X1 -> 4, SDL_BUTTON_X2 -> 5.
  // Drain each poll cycle completely so SDL3's SDL_EVENT_POLL_SENTINEL does not sit ahead of the next injected event.
  ccord_input_event pev = {0}, matched = {0};
  while (ccord_platform_next_event(F.platform, &pev, 0)) {}
  SDL_Event sev = {0};
  sev.type = SDL_EVENT_KEY_DOWN;
  sev.key.key = SDLK_SLASH;
  sev.key.down = true;
  sev.key.mod = SDL_KMOD_LCTRL;
  CT_CHECK(SDL_PushEvent(&sev));
  bool got_ev = false;
  while (ccord_platform_next_event(F.platform, &pev, 0)) {
    if (pev.type == CCORD_IN_KEY_DOWN) { got_ev = true; matched = pev; }
  }
  CT_CHECK(got_ev && matched.key == CCORD_KEY_SLASH && (matched.mods & CCORD_MOD_CTRL) != 0);

  sev = (SDL_Event){0};
  sev.type = SDL_EVENT_KEY_DOWN;
  sev.key.key = SDLK_QUESTION;
  sev.key.down = true;
  sev.key.mod = SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT;
  CT_CHECK(SDL_PushEvent(&sev));
  got_ev = false;
  while (ccord_platform_next_event(F.platform, &pev, 0)) {
    if (pev.type == CCORD_IN_KEY_DOWN) { got_ev = true; matched = pev; }
  }
  CT_CHECK(got_ev && matched.key == CCORD_KEY_SLASH);

  sev = (SDL_Event){0};
  sev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  sev.button.button = SDL_BUTTON_X1;
  sev.button.down = true;
  sev.button.clicks = 1;
  sev.button.x = 200.0f;
  sev.button.y = 200.0f;
  CT_CHECK(SDL_PushEvent(&sev));
  got_ev = false;
  while (ccord_platform_next_event(F.platform, &pev, 0)) {
    if (pev.type == CCORD_IN_MOUSE_DOWN) { got_ev = true; matched = pev; }
  }
  CT_CHECK(got_ev && matched.button == 4);

  sev = (SDL_Event){0};
  sev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  sev.button.button = SDL_BUTTON_X2;
  sev.button.down = true;
  sev.button.clicks = 1;
  sev.button.x = 200.0f;
  sev.button.y = 200.0f;
  CT_CHECK(SDL_PushEvent(&sev));
  got_ev = false;
  while (ccord_platform_next_event(F.platform, &pev, 0)) {
    if (pev.type == CCORD_IN_MOUSE_DOWN) { got_ev = true; matched = pev; }
  }
  CT_CHECK(got_ev && matched.button == 5);

  // Fresh mock transport so navigation history and unread state are clean.
  ccord_mock_config mc = {.big_channel_messages = 60, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  // Reset UI nav history baseline to 2001 / 3111.
  F.ui->nav_hist_len = 0;
  F.ui->nav_hist_pos = 0;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(dbg().nav_history_count == 1 && dbg().nav_history_index == 0);

  // 2. Back / Forward history: 2001/#general (3111) -> 2001/#dev (3113) -> 2002 (g2_ch0) -> DM Friends view.
  CT_CHECK(ccord_session_select_channel(F.session, 3113, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_session_select_guild(F.session, 2002, &err) == CCORD_OK);
  frame(3);
  ccord_id g2_ch0 = ccord_store_selected_channel(F.store);
  CT_CHECK(g2_ch0 != CCORD_ID_NONE);

  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  frame(3);
  // Click the DM "Friends" row so DM_NAV_FRIENDS records a distinct history entry.
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  bool clicked_friends = false;
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    if (F.ui->crow[i].dm_nav == DM_NAV_FRIENDS) {
      click(l->channel_scroll.x + 40.0f, l->channel_scroll.y + F.ui->crow[i].y - F.ui->chan_scroll + 12.0f);
      clicked_friends = true;
      break;
    }
  }
  CT_CHECK(clicked_friends && F.ui->dm_friends_view);
  size_t full_hist = dbg().nav_history_count;
  CT_CHECK(full_hist >= 4 && dbg().nav_history_index + 1u == full_hist);

  // Alt+Left -> steps back out of DM Friends view (to DM channel if any, or 2002). Step back until 2002 / g2_ch0:
  key(CCORD_KEY_LEFT, CCORD_MOD_ALT);
  frame(2);
  if (ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID) {
    CT_CHECK(!F.ui->dm_friends_view);
    key(CCORD_KEY_LEFT, CCORD_MOD_ALT);
    frame(2);
  }
  CT_CHECK(ccord_store_selected_guild(F.store) == 2002 && ccord_store_selected_channel(F.store) == g2_ch0);
  CT_CHECK(dbg().nav_history_count == full_hist);

  // Mouse button 4 (Back) -> 2001 / 3113 (#dev).
  mouse(CCORD_IN_MOUSE_DOWN, 4, 400.0f, 300.0f);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2001 && ccord_store_selected_channel(F.store) == 3113);

  // Alt+Right -> forward to 2002 / g2_ch0.
  key(CCORD_KEY_RIGHT, CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2002 && ccord_store_selected_channel(F.store) == g2_ch0);

  // Mouse button 5 (Forward) until back at DM Friends view.
  while (dbg().nav_history_index + 1u < dbg().nav_history_count) {
    mouse(CCORD_IN_MOUSE_DOWN, 5, 400.0f, 300.0f);
    frame(2);
  }
  CT_CHECK(ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID && F.ui->dm_friends_view);

  // Step back to 2001 / 3113, then open guild 2003 -> forward branch is truncated!
  while (!(ccord_store_selected_guild(F.store) == 2001 && ccord_store_selected_channel(F.store) == 3113)) {
    key(CCORD_KEY_LEFT, CCORD_MOD_ALT);
    frame(2);
  }
  CT_CHECK(ccord_session_select_guild(F.session, 2003, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(dbg().nav_history_index + 1u == dbg().nav_history_count);
  key(CCORD_KEY_RIGHT, CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2003);

  // 3. Alt+Up/Down channel cycling & Ctrl+Alt+Up/Down server cycling (with wraparound).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(3);

  key(CCORD_KEY_DOWN, CCORD_MOD_ALT);
  frame(2);
  ccord_id ch_second = ccord_store_selected_channel(F.store);
  CT_CHECK(ch_second != 3111 && ch_second != CCORD_ID_NONE);

  key(CCORD_KEY_UP, CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_channel(F.store) == 3111);

  // Wraparound from first visible channel (3111) Up -> last visible channel, then Down -> back to 3111.
  key(CCORD_KEY_UP, CCORD_MOD_ALT);
  frame(2);
  ccord_id ch_last = ccord_store_selected_channel(F.store);
  CT_CHECK(ch_last != 3111 && ch_last != ch_second);
  key(CCORD_KEY_DOWN, CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_channel(F.store) == 3111);

  // Ctrl+Alt+Up from 2001 (slot 1) -> DM Home (slot 0) -> 2003 (last slot) -> Ctrl+Alt+Down -> DM Home -> 2001.
  key(CCORD_KEY_UP, CCORD_MOD_CTRL | CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID);

  key(CCORD_KEY_UP, CCORD_MOD_CTRL | CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2003);

  key(CCORD_KEY_DOWN, CCORD_MOD_CTRL | CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID);

  key(CCORD_KEY_DOWN, CCORD_MOD_CTRL | CCORD_MOD_ALT);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2001);

  // 4. Alt+Shift+Up/Down (unread + mention jump, skipping muted unread-only) & Ctrl+Shift+Alt+Up/Down (mention-only).
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(25);
  clear_counts(CCORD_DM_GUILD_ID);
  clear_counts(2001);
  clear_counts(2002);
  clear_counts(2003);

  ccord_id g2_ch1 = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, 2002); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, 2002, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->id != g2_ch0) {
      g2_ch1 = c->id;
      break;
    }
  }
  ccord_id g3_ch0 = find_channel(2003, "general", CCORD_CHANNEL_TEXT);
  CT_CHECK(g2_ch1 != CCORD_ID_NONE && g3_ch0 != CCORD_ID_NONE);

  // Mute both g2_ch0 and g2_ch1; give g2_ch0 unread-only (skipped!) and g2_ch1 a @mention (visited!).
  ccord_notify_scope g2_ovs[2] = {
    {.guild_id = 2002, .channel_id = g2_ch0, .muted = true, .level = CCORD_NOTIFY_INHERIT},
    {.guild_id = 2002, .channel_id = g2_ch1, .muted = true, .level = CCORD_NOTIFY_INHERIT},
  };
  CT_CHECK(ccord_store_set_notify(F.store, 2002, g2_ovs, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, 3113, 4, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, g2_ch0, 5, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, g2_ch1, 2, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, g3_ch0, 1, 2, &err) == CCORD_OK);
  frame(2);

  // Alt+Shift+Down: 3111 -> 3113 -> (skips muted g2_ch0!) g2_ch1 -> g3_ch0; Alt+Shift+Up -> g2_ch1.
  key(CCORD_KEY_DOWN, CCORD_MOD_ALT | CCORD_MOD_SHIFT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2001 && ccord_store_selected_channel(F.store) == 3113);

  key(CCORD_KEY_DOWN, CCORD_MOD_ALT | CCORD_MOD_SHIFT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2002 && ccord_store_selected_channel(F.store) == g2_ch1);

  key(CCORD_KEY_DOWN, CCORD_MOD_ALT | CCORD_MOD_SHIFT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2003 && ccord_store_selected_channel(F.store) == g3_ch0);

  key(CCORD_KEY_UP, CCORD_MOD_ALT | CCORD_MOD_SHIFT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2002 && ccord_store_selected_channel(F.store) == g2_ch1);

  // Ctrl+Shift+Alt+Down (mention-only): from 2001/3111 skips unread-only 3113 and jumps straight to g2_ch1 -> g3_ch0 -> wraps to g2_ch1.
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(2);
  key(CCORD_KEY_DOWN, CCORD_MOD_CTRL | CCORD_MOD_SHIFT | CCORD_MOD_ALT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2002 && ccord_store_selected_channel(F.store) == g2_ch1);

  key(CCORD_KEY_DOWN, CCORD_MOD_CTRL | CCORD_MOD_SHIFT | CCORD_MOD_ALT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2003 && ccord_store_selected_channel(F.store) == g3_ch0);

  key(CCORD_KEY_DOWN, CCORD_MOD_CTRL | CCORD_MOD_SHIFT | CCORD_MOD_ALT);
  frame(1);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2002 && ccord_store_selected_channel(F.store) == g2_ch1);

  CT_CHECK(ccord_store_set_notify(F.store, 2002, nullptr, 0, &err) == CCORD_OK);

  // 5. Keyboard Shortcuts modal (Ctrl+/).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(25);
  clear_composer_input();
  frame(1);

  key(CCORD_KEY_SLASH, CCORD_MOD_CTRL);
  frame(2);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.shortcuts_open && d.shortcuts_modal_rect.w > 0.0f && d.shortcuts_close_btn.w > 0.0f);
  type_text("blocked");
  frame(1);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "") == 0);
  click_rect(d.shortcuts_close_btn);
  CT_CHECK(!dbg().shortcuts_open);

  key(CCORD_KEY_SLASH, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(dbg().shortcuts_open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!dbg().shortcuts_open);

  key(CCORD_KEY_SLASH, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(dbg().shortcuts_open);
  key(CCORD_KEY_SLASH, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(!dbg().shortcuts_open);

  // 6. Inbox: Ctrl+I disambiguation, zero history spam, Unreads & Mentions tabs, Mark Read, Mark All Read, click-to-jump.
  type_text("italic");
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_I, CCORD_MOD_CTRL);
  frame(1);
  CT_CHECK(!dbg().inbox_open && strcmp(ccord_ui_composer_text(F.ui), "*italic*") == 0);

  // Ctrl+Shift+I opens Inbox even while composer has text; Esc closes it.
  key(CCORD_KEY_I, CCORD_MOD_CTRL | CCORD_MOD_SHIFT);
  frame(2);
  CT_CHECK(dbg().inbox_open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!dbg().inbox_open);

  // Empty composer Ctrl+I toggles Inbox open and closed.
  clear_composer_input();
  frame(1);
  key(CCORD_KEY_I, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(dbg().inbox_open);
  key(CCORD_KEY_I, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(!dbg().inbox_open);

  // Prepare Inbox data:
  // - 3113 (#dev in 2001): already loaded in store; post 3 mention messages (direct <@self>, @everyone, role mention).
  // - unloaded_ch (in 2003, never selected so message_count == 0): unread=2, mention=1 -> summary card, zero history fetch!
  // - g2_ch0 (muted unread-only): excluded from Inbox Unreads!
  clear_counts(CCORD_DM_GUILD_ID);
  clear_counts(2001);
  clear_counts(2002);
  clear_counts(2003);

  ccord_id unloaded_ch = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, 2003); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, 2003, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && ccord_store_message_count(F.store, c->id) == 0 &&
        ui_channel_visible(F.ui, 2003, c)) {
      unloaded_ch = c->id;
      break;
    }
  }
  CT_CHECK(unloaded_ch != CCORD_ID_NONE);
  ccord_id mid_unloaded = 0;
  ccord_message_rich r_unloaded = {.present = CCORD_RICH_ALL};
  CT_CHECK(ccord_mock_post_rich_message(tr, unloaded_ch, 1002, "Unloaded seed", &r_unloaded, &mid_unloaded, &err) == CCORD_OK);
  frame(3);
  // Drop the cached message in store while preserving READY-style last_message_id so message_count == 0 AND mock_ack knows mid_unloaded.
  CT_CHECK(ccord_store_delete_message(F.store, unloaded_ch, mid_unloaded, &err) == CCORD_OK);
  {
    ccord_channel uc = *ccord_store_channel(F.store, unloaded_ch);
    uc.last_message_id = mid_unloaded;
    CT_CHECK(ccord_store_upsert_channel(F.store, &uc, &err) == CCORD_OK);
  }
  CT_CHECK(ccord_store_message_count(F.store, unloaded_ch) == 0 &&
           ccord_store_channel(F.store, unloaded_ch)->last_message_id == mid_unloaded);

  ccord_id self_id = ccord_store_self_id(F.store);
  const ccord_id *self_roles = nullptr;
  size_t self_rcnt = 0;
  CT_CHECK(ccord_store_member_roles(F.store, 2001, self_id, &self_roles, &self_rcnt) && self_rcnt >= 1);
  ccord_id my_role = self_roles[0];

  char direct_txt[64];
  snprintf(direct_txt, sizeof direct_txt, "Hey <@%llu> check this", (unsigned long long)self_id);
  ccord_id mid_direct = 0, mid_everyone = 0, mid_role = 0;
  ccord_message_rich r_direct = {.present = CCORD_RICH_ALL, .mention_users = &self_id, .mention_user_count = 1};
  ccord_message_rich r_every = {.present = CCORD_RICH_ALL, .mention_everyone = true};
  ccord_message_rich r_role = {.present = CCORD_RICH_ALL, .mention_roles = &my_role, .mention_role_count = 1};
  CT_CHECK(ccord_mock_post_rich_message(tr, 3113, 1002, direct_txt, &r_direct, &mid_direct, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_post_rich_message(tr, 3113, 1002, "Server-wide ping", &r_every, &mid_everyone, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_post_rich_message(tr, 3113, 1002, "Role ping", &r_role, &mid_role, &err) == CCORD_OK);
  frame(4);

  ccord_notify_scope mute_g2 = {.guild_id = 2002, .channel_id = g2_ch0, .muted = true, .level = CCORD_NOTIFY_INHERIT};
  CT_CHECK(ccord_store_set_notify(F.store, 2002, &mute_g2, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, 3113, 3, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, unloaded_ch, 2, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, g2_ch0, 4, 0, &err) == CCORD_OK);
  frame(2);

  size_t hist_before = ccord_mock_call_count(tr, CCORD_MOCK_CALL_HISTORY);
  click_rect(dbg().inbox_btn);
  d = dbg();
  // Unreads tab: 3113 and unloaded_ch (muted unread-only g2_ch0 excluded!), zero history fetches!
  CT_CHECK(d.inbox_open && d.inbox_tab == 0 && d.inbox_item_count == 2);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_HISTORY) == hist_before);

  // Switch to Mentions tab: 3 loaded mentions from 3113 + 1 unloaded channel summary card = 4 items, still zero history fetches!
  click_rect(d.inbox_tab_rects[1]);
  d = dbg();
  CT_CHECK(d.inbox_open && d.inbox_tab == 1 && d.inbox_item_count == 4);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_HISTORY) == hist_before);

  // Per-item "Mark Read" on the unloaded channel summary card (or whichever item we ack):
  // Let's switch to Unreads tab (2 items: 3113 and unloaded_ch) and click item 1's "Mark Read" button.
  click_rect(d.inbox_tab_rects[0]);
  d = dbg();
  CT_CHECK(d.inbox_tab == 0 && d.inbox_item_count == 2 && d.inbox_item_ack_rects[1].w > 0.0f);
  size_t acks_before = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  click_rect(d.inbox_item_ack_rects[1]);
  d = dbg();
  CT_CHECK(d.inbox_open && d.inbox_item_count == 1);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks_before + 1);
  CT_CHECK(ccord_store_channel(F.store, unloaded_ch)->unread_count == 0);

  // Re-add unread on unloaded_ch, then click "Mark All Read" -> clears both 3113 and unloaded_ch!
  CT_CHECK(ccord_store_set_channel_counts(F.store, unloaded_ch, 2, 1, &err) == CCORD_OK);
  frame(2);
  d = dbg();
  CT_CHECK(d.inbox_item_count == 2 && d.inbox_mark_all_btn.w > 0.0f);
  acks_before = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  click_rect(d.inbox_mark_all_btn);
  d = dbg();
  CT_CHECK(d.inbox_open && d.inbox_item_count == 0);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == acks_before + 2);
  CT_CHECK(ccord_store_channel(F.store, 3113)->unread_count == 0 &&
           ccord_store_channel(F.store, unloaded_ch)->unread_count == 0);

  // Click-to-jump from Mentions tab: re-mark 3113 as having mentions, switch to Mentions, click first card (newest = mid_role).
  CT_CHECK(ccord_store_set_channel_counts(F.store, 3113, 3, 3, &err) == CCORD_OK);
  frame(2);
  click_rect(dbg().inbox_tab_rects[1]);
  d = dbg();
  CT_CHECK(d.inbox_tab == 1 && d.inbox_item_count == 3 && d.inbox_item_rects[0].w > 0.0f);
  // Click left side of card (away from top-right Mark Read button) to jump to 3113 & highlight mid_role.
  click(d.inbox_item_rects[0].x + 24.0f, d.inbox_item_rects[0].y + d.inbox_item_rects[0].h - 14.0f);
  frame(2);
  CT_CHECK(!dbg().inbox_open);
  CT_CHECK(ccord_store_selected_guild(F.store) == 2001 && ccord_store_selected_channel(F.store) == 3113);
  CT_CHECK(dbg().jump_highlight_message == mid_role);

  CT_CHECK(ccord_store_set_notify(F.store, 2002, nullptr, 0, &err) == CCORD_OK);
  frame(30);
}

static void drag_mouse(float x0, float y0, float x1, float y1)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, x0, y0);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x0, y0);
  mouse(CCORD_IN_MOUSE_MOVE, 0, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, x1, y1);
  frame(1);
  mouse(CCORD_IN_MOUSE_UP, 1, x1, y1);
  frame(4);
}

static bool find_rail_guild_rect(ccord_id gid, ccord_rect *out)
{
  ui_rail_slot slots[UI_RAIL_SLOTS_MAX];
  size_t n = ui_rail_build_slots(F.ui, slots, UI_RAIL_SLOTS_MAX, nullptr);
  for (size_t i = 0; i < n; i++) {
    if (slots[i].kind == UI_RAIL_GUILD && slots[i].guild_id == gid) {
      *out = slots[i].rect;
      return true;
    }
  }
  return false;
}

static void test_m3b_folders_drag_add_join_and_discovery(void)
{
  ccord_error err;
  ccord_mock_config mc = {.seed = 59, .big_channel_messages = 15, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  F.ui->server_scroll = 0.0f;
  F.ui->server_scroll_vel = 0.0f;
  F.ui->nexpanded_folders = 0;
  F.ui->discovery_open = false;
  frame(12);

  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(6);
  clear_counts(CCORD_DM_GUILD_ID);
  clear_counts(2001);
  clear_counts(2002);
  clear_counts(2003);

  // 1. Server folders: collapsed badge aggregation, expand/collapse, Mark Folder As Read, Folder Settings, Ungroup.
  ccord_admin_request req;
  ccord_guild_folder_item fitems[3] = {
    {.guild_id = 2002, .folder_id = 500, .folder_name = "Dev", .folder_color = 0x5865F2},
    {.guild_id = 2003, .folder_id = 500, .folder_name = "Dev", .folder_color = 0x5865F2},
    {.guild_id = 2001, .folder_id = 0},
  };
  CT_CHECK(ccord_admin_set_guild_folders(&req, fitems, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &req, &err) == CCORD_OK);
  frame(4);

  ccord_ui_debug d = dbg();
  CT_CHECK(d.rail_folder_count == 1 && d.rail_folder_ids[0] == 500 && !d.rail_folder_expanded[0]);

  ccord_id g2_ch = find_channel(2002, "general", CCORD_CHANNEL_TEXT);
  ccord_id g3_ch = find_channel(2003, "general", CCORD_CHANNEL_TEXT);
  CT_CHECK(g2_ch != CCORD_ID_NONE && g3_ch != CCORD_ID_NONE);
  // Post real messages so last_message_id exists for Mark Folder As Read.
  ccord_id mid2 = 0, mid3 = 0;
  ccord_message_rich rplain = {.present = CCORD_RICH_ALL};
  CT_CHECK(ccord_mock_post_rich_message(tr, g2_ch, 1002, "Folder unread 1", &rplain, &mid2, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_post_rich_message(tr, g3_ch, 1002, "Folder mention", &rplain, &mid3, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_store_set_channel_counts(F.store, g2_ch, 2, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, g3_ch, 1, 3, &err) == CCORD_OK);
  frame(2);
  d = dbg();
  CT_CHECK(d.rail_folder_unread[0] && d.rail_folder_mentions[0] == 3);

  // Right-click collapsed folder -> Mark Folder As Read.
  size_t ack0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  ccord_rect fr = d.rail_folder_rects[0];
  mouse(CCORD_IN_MOUSE_MOVE, 0, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 3, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  frame(2);
  CT_CHECK(F.ui->menu.open && menu_click_label("Mark Folder As Read"));
  frame(4);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == ack0 + 2);
  d = dbg();
  CT_CHECK(!d.rail_folder_unread[0] && d.rail_folder_mentions[0] == 0);

  // Expand & collapse folder by clicking it.
  click_rect(d.rail_folder_rects[0]);
  d = dbg();
  CT_CHECK(d.rail_folder_expanded[0]);
  click_rect(d.rail_folder_rects[0]); // header slot when expanded
  d = dbg();
  CT_CHECK(!d.rail_folder_expanded[0]);

  // Right-click -> Folder Settings modal (edit name + #57F287 hex color).
  fr = d.rail_folder_rects[0];
  mouse(CCORD_IN_MOUSE_MOVE, 0, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 3, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  frame(2);
  CT_CHECK(F.ui->menu.open && menu_click_label("Folder Settings"));
  frame(2);
  d = dbg();
  CT_CHECK(d.folder_settings_modal_open && d.modal_input_rect.w > 0.0f && d.modal_input2_rect.w > 0.0f);
  click_rect(d.modal_input_rect);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Core Team");
  click_rect(d.modal_input2_rect);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("#57F287");
  click_rect(d.modal_ok_btn);
  frame(4);
  CT_CHECK(!dbg().folder_settings_modal_open);
  CT_CHECK(strcmp(ccord_store_guild(F.store, 2002)->folder_name, "Core Team") == 0 &&
           ccord_store_guild(F.store, 2002)->folder_color == 0x57F287u);

  // Right-click -> Ungroup Folder.
  fr = dbg().rail_folder_rects[0];
  mouse(CCORD_IN_MOUSE_MOVE, 0, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 3, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  frame(2);
  CT_CHECK(F.ui->menu.open && menu_click_label("Ungroup Folder"));
  frame(4);
  CT_CHECK(dbg().rail_folder_count == 0 && ccord_store_guild(F.store, 2002)->folder_id == 0);

  // 2. Server drag-and-drop: group 2002 into 2003 (center drop), then expand & drag 2002 out above top (ungroups both).
  ccord_rect r2002 = {0}, r2003 = {0};
  CT_CHECK(find_rail_guild_rect(2002, &r2002) && find_rail_guild_rect(2003, &r2003));
  drag_mouse(r2002.x + r2002.w * 0.5f, r2002.y + r2002.h * 0.5f,
             r2003.x + r2003.w * 0.5f, r2003.y + r2003.h * 0.5f);
  d = dbg();
  CT_CHECK(d.rail_folder_count == 1 && ccord_store_guild(F.store, 2002)->folder_id != 0 &&
           ccord_store_guild(F.store, 2002)->folder_id == ccord_store_guild(F.store, 2003)->folder_id);
  if (!d.rail_folder_expanded[0]) {
    click_rect(d.rail_folder_rects[0]);
    d = dbg();
  }
  CT_CHECK(d.rail_folder_expanded[0] && find_rail_guild_rect(2002, &r2002));
  // Drag 2002 onto top 15% of the folder header to place before the folder (leaving 1-guild nameless folder -> auto-ungroups!).
  fr = d.rail_folder_rects[0];
  drag_mouse(r2002.x + r2002.w * 0.5f, r2002.y + r2002.h * 0.5f,
             fr.x + fr.w * 0.5f, fr.y + 2.0f);
  CT_CHECK(dbg().rail_folder_count == 0 && ccord_store_guild(F.store, 2002)->folder_id == 0 &&
           ccord_store_guild(F.store, 2003)->folder_id == 0);

  // 3. Channel drag-reorder: allowed in 2001 (Manage Channels), blocked in 2003 (lacks Manage Channels).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(4);
  float y_gen = scan_hover_channel(150.0f, 3111);
  float y_ann = scan_hover_channel(150.0f, 3112);
  CT_CHECK(y_gen > 0.0f && y_ann > y_gen);
  size_t adm_before = ccord_mock_admin_count(tr);
  drag_mouse(150.0f, y_ann, 150.0f, y_gen - 10.0f);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_before + 1);
  float y_gen_after = scan_hover_channel(150.0f, 3111);
  float y_ann_after = scan_hover_channel(150.0f, 3112);
  CT_CHECK(y_ann_after > 0.0f && y_ann_after < y_gen_after);

  // Switch to 2003 (Pixel Garden, exact perms without Manage Channels): channel drag must not fire an admin request.
  CT_CHECK(ccord_session_select_guild(F.session, 2003, &err) == CCORD_OK);
  frame(4);
  ccord_id p_gen = find_channel(2003, "general", CCORD_CHANNEL_TEXT);
  ccord_id p_art = find_channel(2003, "art", CCORD_CHANNEL_TEXT);
  CT_CHECK(p_gen != CCORD_ID_NONE && p_art != CCORD_ID_NONE);
  float yp_gen = scan_hover_channel(150.0f, p_gen);
  float yp_art = scan_hover_channel(150.0f, p_art);
  CT_CHECK(yp_gen > 0.0f && yp_art > 0.0f);
  adm_before = ccord_mock_admin_count(tr);
  drag_mouse(150.0f, yp_art, 150.0f, yp_gen - 10.0f);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_before);

  // 4. Add / Join Server modal (+ button in rail): Join via invite preview + Create a new server.
  d = dbg();
  CT_CHECK(d.add_server_btn.w > 0.0f);
  click_rect(d.add_server_btn);
  d = dbg();
  CT_CHECK(d.add_server_modal_open && !d.modal_has_invite_preview && d.modal_preview_btn.w > 0.0f);
  type_text("https://discord.gg/vulkan-lab");
  click_rect(d.modal_preview_btn);
  frame(4);
  d = dbg();
  CT_CHECK(d.modal_has_invite_preview && strcmp(d.modal_preview_guild_name, "Vulkan & Graphics Lab") == 0);
  click_rect(d.modal_ok_btn);
  frame(6);
  CT_CHECK(!dbg().add_server_modal_open && ccord_store_selected_guild(F.store) == 9004);

  // Reopen Add Server modal -> switch to Create tab -> create "Systems Lab".
  F.ui->server_scroll = 0.0f;
  F.ui->server_scroll_vel = 0.0f;
  frame(2);
  click_rect(dbg().add_server_btn);
  d = dbg();
  CT_CHECK(d.add_server_modal_open && d.modal_tab_rects[1].w > 0.0f);
  click_rect(d.modal_tab_rects[1]);
  d = dbg();
  type_text("Systems Lab");
  click_rect(d.modal_ok_btn);
  frame(6);
  CT_CHECK(!dbg().add_server_modal_open);
  const ccord_guild *created_g = ccord_store_guild(F.store, ccord_store_selected_guild(F.store));
  CT_CHECK(created_g && strcmp(created_g->name, "Systems Lab") == 0);

  // 5. Server Discovery: compass button, 450 ms search debounce, category chips, 60 s TTL cache hit, Join action.
  F.ui->server_scroll = 0.0f;
  F.ui->server_scroll_vel = 0.0f;
  frame(2);
  size_t disc0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY);
  click_rect(dbg().discovery_btn);
  frame(4);
  d = dbg();
  CT_CHECK(d.discovery_open && !F.ui->layout.member_list_visible && d.discovery_card_count >= 5);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == disc0 + 1);

  // Type "synth" into search input -> 450 ms debounce.
  click_rect(d.discovery_search_rect);
  type_text("synth");
  frame(4); // 200 ms: still debouncing!
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == disc0 + 1);
  frame(8); // +400 ms = 600 ms total: fired once!
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == disc0 + 2);
  d = dbg();
  CT_CHECK(d.discovery_card_count == 1 && d.discovery_total == 1);

  // Clear search and click category chip 3 (Education -> Systems Programming Study, id 9003).
  click_rect(d.discovery_search_rect);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  click_rect(d.discovery_cat_rects[3]);
  frame(4);
  size_t disc_after_cat = ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY);
  d = dbg();
  CT_CHECK(d.discovery_category == 3 && d.discovery_card_count == 1);

  // Close Discovery by selecting server 2001, then reopen Discovery -> 60 s TTL cache hit (0 extra calls!).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  F.ui->discovery_open = false;
  ui_compute_layout(F.ui);
  F.ui->server_scroll = 0.0f;
  F.ui->server_scroll_vel = 0.0f;
  frame(2);
  click_rect(dbg().discovery_btn);
  frame(2);
  CT_CHECK(dbg().discovery_open && ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == disc_after_cat);

  // Click Join on Systems Programming Study (9003) -> joins server, closes Discovery, selects 9003!
  d = dbg();
  CT_CHECK(d.discovery_join_rects[0].w > 0.0f);
  click_rect(d.discovery_join_rects[0]);
  frame(6);
  CT_CHECK(!dbg().discovery_open && ccord_store_selected_guild(F.store) == 9003);
}


// The rect of a status-card control from the last draw (empty when it is not shown).
static ccord_rect status_card_item_rect(const ccord_ui *ui, ui_sc_kind kind, int arg)
{
  for (size_t i = 0; i < ui->sc.nitems; i++) {
    if (ui->sc.items[i].kind == kind && ui->sc.items[i].arg == arg) return ui->sc.items[i].rect;
  }
  return (ccord_rect){0};
}

static void test_m3c_group_dms_requests_profiles_and_custom_status(void)
{
  ccord_error err = {0};
  ccord_transport *tr = F.transport;
  CT_CHECK(tr != nullptr);
  if (!tr) return;

  // Ensure Ada (1002) and Bjorn (1003) are friends so the New Group DM friend checklist has >= 2 friends!
  CT_CHECK(ccord_session_set_relationship(F.session, 1002, CCORD_RELATIONSHIP_FRIEND, &err) == CCORD_OK);
  CT_CHECK(ccord_session_set_relationship(F.session, 1003, CCORD_RELATIONSHIP_FRIEND, &err) == CCORD_OK);
  CT_CHECK(ccord_session_set_relationship(F.session, 1004, CCORD_RELATIONSHIP_FRIEND, &err) == CCORD_OK);
  frame(4);

  // 1. Group DM: switch to DM Home, click `+` next to DIRECT MESSAGES, select 2 friends + custom group name -> Create Group DM.
  F.ui->server_scroll = 0.0f;
  F.ui->server_scroll_vel = 0.0f;
  frame(2);
  click_rect(ui_guild_icon_rect(F.ui, 0));
  frame(3);

  CT_CHECK(F.ui->dm_header_plus_rect.w > 0.0f);
  click_rect(F.ui->dm_header_plus_rect);
  frame(2);
  ccord_ui_debug d = dbg();
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NEW_DM && d.new_dm_friend_count >= 3);
  click_rect(d.new_dm_friend_rects[0]);
  click_rect(d.new_dm_friend_rects[1]);
  d = dbg();
  CT_CHECK(d.new_dm_selected_count == 2 && d.modal_input2_rect.w > 0.0f);
  click_rect(d.modal_input2_rect);
  type_text("C23 Core");
  click_rect(d.modal_ok_btn);
  frame(5);

  ccord_id gdm_id = ccord_store_selected_channel(F.store);
  const ccord_channel *gdm = ccord_store_channel(F.store, gdm_id);
  CT_CHECK(gdm && gdm->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM && strcmp(gdm->name, "C23 Core") == 0);
  // Make sure right-side member list is visible in Group DM:
  if (!F.ui->layout.member_list_visible) {
    F.ui->members_hidden = false;
    ui_compute_layout(F.ui);
    frame(2);
  }
  d = dbg();
  CT_CHECK(d.gdm_add_btn.w > 0.0f && d.gdm_edit_btn.w > 0.0f && d.gdm_member_count == 3);

  // Add 3rd friend via Add Member button:
  click_rect(d.gdm_add_btn);
  frame(2);
  d = dbg();
  CT_CHECK(F.ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT && d.new_dm_friend_count >= 1);
  click_rect(d.new_dm_friend_rects[0]);
  click_rect(d.modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_channel(F.store, gdm_id)->recipient_count == 3);

  // Edit Group DM name via Edit button:
  click_rect(dbg().gdm_edit_btn);
  frame(2);
  d = dbg();
  CT_CHECK(F.ui->modal_kind == UI_MODAL_EDIT_GROUP_DM);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("C23 Architects");
  click_rect(d.modal_ok_btn);
  frame(4);
  CT_CHECK(strcmp(ccord_store_channel(F.store, gdm_id)->name, "C23 Architects") == 0);

  // Right-click a non-self member in the Group DM member list -> Remove from Group:
  open_menu(CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, gdm_id, ccord_store_channel(F.store, gdm_id)->recipient_ids[2], CCORD_ID_NONE);
  CT_CHECK(F.ui->menu.open && menu_click_label("Remove from Group"));
  frame(4);
  CT_CHECK(ccord_store_channel(F.store, gdm_id)->recipient_count == 2);

  // 2. Message Requests: push 1 regular + 1 spam request -> "Message Requests" nav appears with badge 1!
  ccord_id req_cid = 0, spam_cid = 0;
  CT_CHECK(ccord_mock_push_message_request(tr, 1008, "Can we collaborate on SDL3?", false, &req_cid, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_push_message_request(tr, 1009, "Claim free gift link", true, &spam_cid, &err) == CCORD_OK);
  frame(4);
  d = dbg();
  CT_CHECK(d.dm_requests_nav_rect.w > 0.0f && d.dm_requests_badge_count == 1);

  // Click Message Requests nav row -> switch Requests / Spam tabs, reject spam, preview regular request (composer locked, 0 ACKs), accept!
  click_rect(d.dm_requests_nav_rect);
  frame(2);
  d = dbg();
  CT_CHECK(d.dm_requests_view && d.dm_requests_tab == 0 && d.dm_request_card_count == 1);

  click_rect(d.dm_requests_tab_rects[1]); // Spam tab
  frame(2);
  d = dbg();
  CT_CHECK(d.dm_requests_tab == 1 && d.dm_request_card_count == 1);
  click_rect(d.dm_request_reject_rects[0]);
  frame(4);
  CT_CHECK(ccord_store_channel(F.store, spam_cid) == nullptr);

  click_rect(dbg().dm_requests_tab_rects[0]); // Back to Requests tab
  frame(2);
  d = dbg();
  CT_CHECK(d.dm_request_card_count == 1);
  size_t ack_before = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  // Click card body (left side, away from accept/reject buttons) to preview conversation:
  click(d.dm_request_card_rects[0].x + 24.0f, d.dm_request_card_rects[0].y + d.dm_request_card_rects[0].h * 0.5f);
  frame(4);
  d = dbg();
  CT_CHECK(!d.dm_requests_view && ccord_store_selected_channel(F.store) == req_cid && d.msg_req_banner_visible);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == ack_before); // previewing sends 0 ACKs!

  // Click Accept on the preview banner -> moves into regular DMs and unlocks composer!
  click_rect(d.msg_req_banner_accept_btn);
  frame(4);
  CT_CHECK(!dbg().msg_req_banner_visible && !ccord_store_channel(F.store, req_cid)->is_message_request);

  // 3. Full Profile Popover & Modal + Rich Presence Activity Card + Clan Badge + Role Pills:
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  if (!F.ui->layout.member_list_visible) {
    F.ui->members_hidden = false;
    ui_compute_layout(F.ui);
  }
  // Ensure Ada (1002) has an active Rich Presence activity + clan tag so member list / popover / modal draw both!
  CT_CHECK(ccord_store_update_presence_detail(F.store, 1002, CCORD_PRESENCE_ONLINE, "Playing RenderDoc",
                                              "", "", 0, true, CCORD_ACTIVITY_PLAYING,
                                              "RenderDoc", "Frame #42", "Debugging shaders",
                                              (int64_t)F.now - 15000, 0, &err) == CCORD_OK);
  frame(4);
  d = dbg();
  CT_CHECK(d.clan_badges_drawn >= 1);

  // Scan member list for Ada (1002) and click to open popover:
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float mx = l->member_list.x + l->member_list.w * 0.5f;
  float ada_y = -1.0f;
  for (float y = l->member_list.y + 16.0f; y < l->member_list.y + l->member_list.h - 10.0f; y += 6.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, mx, y);
    if (dbg().hover_user == 1002) {
      ada_y = y;
      break;
    }
  }
  CT_CHECK(ada_y > 0.0f);
  size_t prof_calls0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE);
  click(mx, ada_y);
  frame(4);
  d = dbg();
  CT_CHECK(d.member_popover_open && d.popover_full_profile_btn.w > 0.0f && d.activity_cards_drawn >= 1);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == prof_calls0 + 1);

  // Click "View Full Profile" -> opens modal and hits 60 s TTL cache (0 extra profile REST calls!):
  click_rect(d.popover_full_profile_btn);
  frame(3);
  d = dbg();
  CT_CHECK(d.profile_modal_open && !d.member_popover_open);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == prof_calls0 + 1);
  CT_CHECK(d.activity_cards_drawn >= 1 && d.role_pills_drawn >= 1);

  // Scroll down inside the profile modal to reveal Note & Connections:
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = 640.0f, .y = 360.0f, .wheel = -6.0f});
  frame(2);
  d = dbg();
  CT_CHECK(d.profile_modal_conn_count >= 1 && d.profile_modal_note_save_btn.w > 0.0f);

  // Click verified Connection URL -> opens external URL via host hook!
  F.last_url[0] = '\0';
  click_rect(d.profile_modal_conn_rects[0]);
  frame(2);
  CT_CHECK(F.last_url[0] != '\0');

  // Pre-loaded initial note ("Core maintainer"): select all, replace with new Personal Note, and Save Note!
  CT_CHECK(strcmp(ccord_store_user_note(F.store, 1002), "Core maintainer") == 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("GPU debugger wizard");
  click_rect(dbg().profile_modal_note_save_btn);
  frame(4);
  CT_CHECK(strcmp(ccord_store_user_note(F.store, 1002), "GPU debugger wizard") == 0);

  // Switch to Mutual Servers (tab 1) and Mutual Friends (tab 2), then close modal:
  click_rect(dbg().profile_modal_tab_rects[1]);
  frame(2);
  CT_CHECK(dbg().profile_modal_tab == 1 && dbg().profile_modal_mutual_count >= 1);
  click_rect(dbg().profile_modal_tab_rects[2]);
  frame(2);
  CT_CHECK(dbg().profile_modal_tab == 2 && dbg().profile_modal_mutual_count >= 1);
  click_rect(dbg().profile_modal_close_btn);
  frame(2);
  CT_CHECK(!dbg().profile_modal_open);

  // 4. Account card (the user-panel popout): click the user status area -> Do Not Disturb applies at once -> emoji +
  // text + 30 min -> Set -> clear (x).
  d = dbg();
  CT_CHECK(d.user_status_rect.w > 0.0f);
  click_rect(d.user_status_rect);
  frame(2);
  CT_CHECK(ui_status_card_is_open(F.ui) && !dbg().custom_status_modal_open);
#define SC_ITEM_RECT(kind_, arg_) status_card_item_rect(F.ui, (kind_), (arg_))
  click_rect(SC_ITEM_RECT(UI_SC_PRESENCE, 2));   // Do Not Disturb
  frame(3);
  const ccord_user *self_u = ccord_store_user(F.store, 1001);
  CT_CHECK(self_u && self_u->presence == CCORD_PRESENCE_DND && ui_status_card_is_open(F.ui));
  click_rect(SC_ITEM_RECT(UI_SC_EMOJI_BUTTON, 0));
  frame(2);
  click_rect(SC_ITEM_RECT(UI_SC_EMOJI_PICK, 1));   // Fire emoji
  click_rect(SC_ITEM_RECT(UI_SC_DURATION, 0));     // 30 minutes
  click_rect(SC_ITEM_RECT(UI_SC_TEXT, 0));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Finishing Milestone M3");
  click_rect(SC_ITEM_RECT(UI_SC_SET, 0));
  frame(4);
  self_u = ccord_store_user(F.store, 1001);
  CT_CHECK(self_u && self_u->presence == CCORD_PRESENCE_DND &&
           strcmp(self_u->custom_status_text, "Finishing Milestone M3") == 0 &&
           self_u->custom_status_expires_ms > F.ui->now_unix_ms);
  frame(20);   // the confirmation checkmark ends

  // The clear (x) removes the custom status at once; Escape closes the card.
  click_rect(SC_ITEM_RECT(UI_SC_CLEAR, 0));
  frame(4);
  self_u = ccord_store_user(F.store, 1001);
  CT_CHECK(self_u && self_u->custom_status_text[0] == '\0');
#undef SC_ITEM_RECT
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!ui_status_card_is_open(F.ui));
}

static void test_m4a_server_settings_safety_vanity_audit_preview_and_danger(void)
{
  ccord_error err;
  ccord_mock_config mc = {.seed = 53, .big_channel_messages = 30, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  // Write a tiny valid PNG file into ui_test_data/icon.png for Server Icon & Banner upload testing.
  static const uint8_t k_tiny_png[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
    0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
    0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
    0x44, 0xAE, 0x42, 0x60, 0x82,
  };
  FILE *fp = fopen(ui_test_file("icon.png"), "wb");
  CT_CHECK(fp != nullptr);
  if (fp) {
    (void)fwrite(k_tiny_png, 1, sizeof k_tiny_png, fp);
    fclose(fp);
  }
  snprintf(F.pick_path, sizeof F.pick_path, "%s", ui_test_file("icon.png"));

  const ccord_id hq = 2001;
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(3);

  // 1. Overview completions: Server Icon & Banner upload/clear, System Messages channel & flags, AFK channel & timeout,
  // Default Notification Settings, Reset vs Save!
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(3);
  CT_CHECK(ssd().open && ssd().page == 0 && !ssd().unsaved);
  // Non-owner on 2001 (owner_id == 0) does NOT see Danger Zone page yet:
  CT_CHECK(ss_has("page:safety") && !ss_has("page:danger"));

  CT_CHECK(ss_scroll_to("icon-upload") && ss_click("icon-upload"));
  CT_CHECK(ssd().unsaved);
  CT_CHECK(ss_click("reset") && !ssd().unsaved);

  CT_CHECK(ss_scroll_to("icon-upload") && ss_click("icon-upload"));
  CT_CHECK(ss_scroll_to("banner-upload") && ss_click("banner-upload"));
  CT_CHECK(ss_scroll_to("sys-channel") && ss_click("sys-channel"));
  CT_CHECK(ss_scroll_to("sysflag:0") && ss_click("sysflag:0"));
  CT_CHECK(ss_scroll_to("afk-channel") && ss_click("afk-channel"));
  CT_CHECK(ss_scroll_to("afktimeout:900") && ss_click("afktimeout:900"));
  CT_CHECK(ss_scroll_to("defnotify:1") && ss_click("defnotify:1"));
  CT_CHECK(ssd().unsaved);
  frame(20);
  CT_CHECK(ss_click("save"));
  frame(4);
  CT_CHECK(!ssd().unsaved);
  const ccord_guild *g_hq = ccord_store_guild(F.store, hq);
  CT_CHECK(g_hq && g_hq->icon_url && strstr(g_hq->icon_url, "/icons/2001/") != nullptr &&
           strstr(g_hq->banner_url, "/banners/2001/") != nullptr &&
           (g_hq->system_channel_flags & 1u) != 0u &&
           g_hq->afk_channel_id != CCORD_ID_NONE && g_hq->afk_timeout == 900 &&
           g_hq->default_message_notifications == 1);

  // Remove Icon & Remove Banner -> Save clears both on Discord!
  frame(20);
  CT_CHECK(ss_scroll_to("icon-clear") && ss_click("icon-clear"));
  CT_CHECK(ss_scroll_to("banner-clear") && ss_click("banner-clear"));
  CT_CHECK(ssd().unsaved && ss_click("save"));
  frame(4);
  g_hq = ccord_store_guild(F.store, hq);
  CT_CHECK(g_hq && g_hq->icon_url[0] == '\0' && g_hq->banner_url[0] == '\0');

  // 2. Safety Setup: Raid Protection (Pause Invites & Pause DMs) + Verification Level + Explicit Media Content Filter!
  CT_CHECK(ss_click("page:safety") && ssd().page == 5);
  CT_CHECK(ss_scroll_to("pause-invites:3600") && ss_click("pause-invites:3600"));
  frame(4);
  g_hq = ccord_store_guild(F.store, hq);
  CT_CHECK(g_hq && g_hq->invites_disabled_until_ms > F.ui->now_unix_ms && g_hq->dms_disabled_until_ms == 0);

  CT_CHECK(ss_scroll_to("pause-dms:21600") && ss_click("pause-dms:21600"));
  frame(4);
  g_hq = ccord_store_guild(F.store, hq);
  CT_CHECK(g_hq && g_hq->invites_disabled_until_ms > F.ui->now_unix_ms &&
           g_hq->dms_disabled_until_ms > F.ui->now_unix_ms);

  // Unpausing invites preserves DMs pause!
  CT_CHECK(ss_scroll_to("pause-invites:0") && ss_click("pause-invites:0"));
  frame(4);
  g_hq = ccord_store_guild(F.store, hq);
  CT_CHECK(g_hq && g_hq->invites_disabled_until_ms == 0 && g_hq->dms_disabled_until_ms > F.ui->now_unix_ms);

  CT_CHECK(ss_scroll_to("verif:3") && ss_click("verif:3"));
  CT_CHECK(ss_scroll_to("ecf:2") && ss_click("ecf:2"));
  CT_CHECK(ssd().unsaved);
  frame(20);
  CT_CHECK(ss_click("save"));
  frame(4);
  g_hq = ccord_store_guild(F.store, hq);
  CT_CHECK(!ssd().unsaved && g_hq && g_hq->verification_level == 3 && g_hq->explicit_content_filter == 2);

  // 3. Invites completions: Pause Invites toggle + Vanity URL set & clear!
  CT_CHECK(ss_click("page:invites") && ssd().page == 2);
  frame(4);
  CT_CHECK(ss_scroll_to("invites-pause-toggle") && ss_click("invites-pause-toggle"));
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, hq)->invites_disabled_until_ms > F.ui->now_unix_ms);
  CT_CHECK(ss_scroll_to("invites-pause-toggle") && ss_click("invites-pause-toggle"));
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, hq)->invites_disabled_until_ms == 0);

  CT_CHECK(ss_scroll_to("vanity-code") && ss_click("vanity-code") && ssd().focus == 5);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("ccord-ui-hq");
  CT_CHECK(ss_click("vanity-save"));
  frame(4);
  CT_CHECK(strcmp(ccord_store_guild(F.store, hq)->vanity_url_code, "ccord-ui-hq") == 0);

  CT_CHECK(ss_scroll_to("vanity-clear") && ss_click("vanity-clear"));
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, hq)->vanity_url_code[0] == '\0');

  // 4. Audit Log completions: Filter by User, Filter by Action, and click-to-expand structured changes[]!
  CT_CHECK(ss_click("page:audit") && ssd().page == 4);
  frame(4);
  size_t na = 0;
  const ccord_audit_entry *alog = ccord_store_audit(F.store, hq, &na);
  CT_CHECK(alog && na >= 1);
  ccord_id expandable_id = CCORD_ID_NONE;
  for (size_t i = 0; i < na; i++) {
    if (alog[i].change_count > 0) { expandable_id = alog[i].id; break; }
  }
  CT_CHECK(expandable_id != CCORD_ID_NONE);
  char row_tag[64], chg_tag[64];
  snprintf(row_tag, sizeof row_tag, "audit-row:%llu", (unsigned long long)expandable_id);
  snprintf(chg_tag, sizeof chg_tag, "audit-change:%llu:0", (unsigned long long)expandable_id);
  CT_CHECK(ss_scroll_to(row_tag) && !ss_has(chg_tag));
  CT_CHECK(ss_click(row_tag) && ss_has(chg_tag));   // expanded!
  CT_CHECK(ss_click(row_tag) && !ss_has(chg_tag));  // collapsed!

  size_t adm_before = ccord_mock_admin_count(tr);
  CT_CHECK(ss_scroll_to("audit-user") && ss_click("audit-user"));
  frame(3);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_before + 1 &&
           ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_AUDIT_LOG &&
           ccord_mock_last_admin(tr)->user_id != CCORD_ID_NONE);

  CT_CHECK(ss_scroll_to("audit-action") && ss_click("audit-action"));
  frame(3);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_before + 2 &&
           ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_AUDIT_LOG &&
           ccord_mock_last_admin(tr)->audit_action != 0);

  // 5. "View Server As Role" preview mode!
  // Add a private channel (3195) denying VIEW_CHANNEL to @everyone (2001):
  ccord_overwrite ow_hide = {.id = hq, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_VIEW_CHANNEL};
  ccord_channel priv_ch = {
    .id = 3195, .guild_id = hq, .kind = CCORD_CHANNEL_TEXT, .name = "staff-vault", .topic = "",
  };
  CT_CHECK(ccord_store_upsert_channel(F.store, &priv_ch, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_overwrites(F.store, 3195, hq, &ow_hide, 1, &err) == CCORD_OK);

  CT_CHECK(ss_click("page:roles") && ssd().page == 1);
  CT_CHECK(ss_click("role:2001")); // @everyone
  CT_CHECK(ss_click("role-preview"));
  frame(3);
  CT_CHECK(!ssd().open && dbg().role_preview_active && dbg().role_preview_guild == hq &&
           dbg().role_preview_role_id == hq);
  // Private channel 3195 is visible to Admin normally, but hidden while previewing as @everyone!
  bool saw_priv = false;
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    if (!F.ui->crow[i].is_voice_user && !F.ui->crow[i].is_relationship) {
      const ccord_channel *rc = ccord_store_channel_at(F.store, hq, F.ui->crow[i].index);
      if (rc && rc->id == 3195) saw_priv = true;
    }
  }
  CT_CHECK(!saw_priv);

  // Switch Role button cycles to next role; Exit Preview restores normal view (3195 visible again)!
  click_rect(dbg().role_preview_cycle_btn);
  frame(2);
  CT_CHECK(dbg().role_preview_active && dbg().role_preview_role_id != hq);
  click_rect(dbg().role_preview_exit_btn);
  frame(2);
  CT_CHECK(!dbg().role_preview_active);
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    if (!F.ui->crow[i].is_voice_user && !F.ui->crow[i].is_relationship) {
      const ccord_channel *rc = ccord_store_channel_at(F.store, hq, F.ui->crow[i].index);
      if (rc && rc->id == 3195) saw_priv = true;
    }
  }
  CT_CHECK(saw_priv);

  // 6. Leave Server (guild context menu on non-owned guild 2003) & Owner Danger Zone (Transfer Ownership + Delete Server)!
  open_menu(CCORD_MENU_GUILD, 2003, 0, 0, 0);
  CT_CHECK(F.ui->menu.open && menu_click_label("Leave Server"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_LEAVE_GUILD);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, 2003) == nullptr);

  // Create a brand-new server owned by self (1001) to test Danger Zone!
  ccord_admin_request creq;
  CT_CHECK(ccord_admin_create_guild(&creq, "Throwaway HQ", &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id owned_gid = ccord_store_selected_guild(F.store);
  CT_CHECK(owned_gid != hq && ccord_store_guild(F.store, owned_gid)->owner_id == 1001);
  // Add member 1002 so Transfer Ownership has an eligible target, then test Delete Server name-match guard first!
  ccord_member m_ada = {.guild_id = owned_gid, .user_id = 1002, .role_name = "", .nick = ""};
  CT_CHECK(ccord_store_upsert_guild_members(F.store, owned_gid, &m_ada, 1, nullptr, 0, &err) == CCORD_OK);

  CT_CHECK(ccord_ui_open_server_settings(F.ui, owned_gid));
  frame(3);
  CT_CHECK(ss_has("page:danger") && ss_click("page:danger") && ssd().page == 6);

  // Delete Server requires typing the exact server name ("Throwaway HQ"); wrong name keeps modal open and shows notice!
  CT_CHECK(ss_scroll_to("delete-guild") && ss_click("delete-guild"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_DELETE_GUILD);
  type_text("wrong name");
  click_rect(dbg().modal_ok_btn);
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && ccord_store_guild(F.store, owned_gid) != nullptr &&
           strstr(dbg().notice, "did not match") != nullptr);

  // Type exact name -> deletes guild and closes Server Settings!
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Throwaway HQ");
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, owned_gid) == nullptr && !ssd().open);

  // Also test Transfer Ownership in Danger Zone on a second owned server:
  CT_CHECK(ccord_admin_create_guild(&creq, "Transfer Lab", &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id xfer_gid = ccord_store_selected_guild(F.store);
  CT_CHECK(ccord_store_guild(F.store, xfer_gid)->owner_id == 1001);
  CT_CHECK(ccord_mock_add_guild_member(tr, xfer_gid, 1002, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_add_guild_member(tr, xfer_gid, 1003, &err) == CCORD_OK);
  frame(3);

  CT_CHECK(ccord_ui_open_server_settings(F.ui, xfer_gid));
  frame(3);
  CT_CHECK(ss_click("page:danger") && ssd().page == 6);
  CT_CHECK(ss_scroll_to("transfer-member") && ss_click("transfer-member")); // cycles from 1002 -> 1003
  CT_CHECK(ss_scroll_to("transfer-owner") && ss_click("transfer-owner"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_TRANSFER_OWNERSHIP &&
           F.ui->modal_target_id == 1003);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, xfer_gid)->owner_id == 1003);
  // Once ownership transfers away from 1001, Danger Zone closes or switches away automatically!
  CT_CHECK(!ssd().open || ssd().page != 6);
  if (ssd().open) ui_server_settings_close(F.ui);

  remove(ui_test_file("icon.png"));
  F.pick_path[0] = '\0';
}

static void write_test_file(const char *path, const void *bytes, size_t len)
{
  FILE *fp = fopen(path, "wb");
  CT_CHECK(fp != nullptr);
  if (fp) {
    (void)fwrite(bytes, 1, len, fp);
    fclose(fp);
  }
}

static void test_m4b_expressions(void)
{
  ccord_error err;
  ccord_mock_config mc = {.seed = 59, .big_channel_messages = 20, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  const ccord_id hq = 2001, lua_g = 2002, pix_g = 2003, self_id = ccord_store_self_id(F.store);

  // 1. Sidebar permission gating:
  // - 2002 (Lua Lounge): self has Bans/Audit/Kick/ManageMessages, NOT Create/Manage Expressions -> hidden!
  CT_CHECK(ccord_ui_open_server_settings(F.ui, lua_g));
  frame(2);
  CT_CHECK(ssd().open && ssd().page == 3 && !ss_has("page:emoji") && !ss_has("page:stickers") &&
           !ss_has("page:soundboard"));
  ui_server_settings_close(F.ui);

  // - 2003 (Pixel Garden): ordinary member cannot open Server Settings until granted CREATE_EXPRESSIONS,
  //   which opens directly onto SS_EMOJI (page 7) and hides Overview/Roles/Bans/Audit!
  CT_CHECK(!ccord_ui_open_server_settings(F.ui, pix_g));
  const ccord_id *pix_roles = nullptr;
  size_t pix_nroles = 0;
  CT_CHECK(ccord_store_member_roles(F.store, pix_g, self_id, &pix_roles, &pix_nroles) && pix_nroles == 1);
  ccord_id pix_member_rid = pix_roles[0];
  const ccord_role *pix_r0 = ccord_store_role(F.store, pix_g, pix_member_rid);
  CT_CHECK(pix_r0 != nullptr);
  ccord_role pix_r_expr = *pix_r0;
  char pix_r_name[128];
  snprintf(pix_r_name, sizeof pix_r_name, "%s", pix_r_expr.name ? pix_r_expr.name : "");
  pix_r_expr.name = pix_r_name;
  pix_r_expr.permissions |= CCORD_PERM_CREATE_EXPRESSIONS;
  CT_CHECK(ccord_store_upsert_role(F.store, &pix_r_expr, &err) == CCORD_OK);
  size_t adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, pix_g));
  frame(3);
  CT_CHECK(ssd().open && ssd().page == 7 && ss_has("page:emoji") && ss_has("page:stickers") &&
           ss_has("page:soundboard") && !ss_has("page:overview") && !ss_has("page:roles") && !ss_has("page:bans"));
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 1 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_EMOJIS);
  // Revoking CREATE_EXPRESSIONS while open closes Server Settings cleanly:
  pix_r_expr.permissions &= ~CCORD_PERM_CREATE_EXPRESSIONS;
  CT_CHECK(ccord_store_upsert_role(F.store, &pix_r_expr, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(!ssd().open);

  // 2. Request budget, 30s freshness cache, and 2s Refresh cooldown on C-Cord HQ (2001):
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_has("page:emoji") && ss_has("page:stickers") && ss_has("page:soundboard"));

  adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("page:emoji") && ssd().page == 7);
  frame(4);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 1 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_EMOJIS);
  CT_CHECK(ccord_store_guild_emojis_admin_loaded(F.store, hq));
  size_t base_emojis = ccord_store_guild_emoji_count(F.store, hq);
  CT_CHECK(base_emojis >= 2 && ssd().rows == base_emojis);

  // Re-entering Emoji within 30s reuses cached store list (0 extra requests); immediate Refresh (< 2s) is rate-limited!
  CT_CHECK(ss_click("page:overview") && ssd().page == 0);
  CT_CHECK(ss_click("page:emoji") && ssd().page == 7);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 1);
  CT_CHECK(ss_scroll_to("refresh") && ss_click("refresh"));
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 1);
  frame(45);   // +2250 ms >= SS_REFRESH_GAP_MS (2000 ms)
  CT_CHECK(ss_click("refresh") && ccord_mock_admin_count(tr) == adm0 + 2 &&
           ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_EMOJIS);
  frame(4);

  // 3. Emoji upload (disabled without file, GIF animated sniffing + identifier stem prefill), search, inline edit, delete:
  CT_CHECK(ss_scroll_to("emoji-upload") && ss_click("emoji-upload"));
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 2 && strstr(dbg().notice, "Choose an image file first") != nullptr);

  static const uint8_t k_tiny_gif[] = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0, 0, 0, ';'};
  write_test_file(ui_test_file("party-parrot.gif"), k_tiny_gif, sizeof k_tiny_gif);
  snprintf(F.pick_path, sizeof F.pick_path, "%s", ui_test_file("party-parrot.gif"));
  CT_CHECK(ss_scroll_to("emoji-pick") && ss_click("emoji-pick"));
  CT_CHECK(ss_scroll_to("emoji-upload") && ss_click("emoji-upload"));
  frame(4);
  CT_CHECK(ccord_store_guild_emoji_count(F.store, hq) == base_emojis + 1);
  ccord_id new_em_id = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_guild_emoji_count(F.store, hq); i++) {
    const ccord_emoji *em = ccord_store_guild_emoji_at(F.store, hq, i);
    if (em && strcmp(em->name, "party_parrot") == 0) {
      CT_CHECK(em->animated && em->user_id == self_id);
      new_em_id = em->id;
    }
  }
  CT_CHECK(new_em_id != CCORD_ID_NONE);

  // Search filter by name + non-matching query + clear:
  CT_CHECK(ss_scroll_to("emoji-search") && ss_click("emoji-search"));
  type_text("party_parrot");
  frame(1);
  CT_CHECK(ssd().rows == 1);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("zzzz_missing");
  frame(1);
  CT_CHECK(ssd().rows == 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
  CT_CHECK(ssd().rows == base_emojis + 1);

  // Inline edit: Cancel discards, Enter saves rename to Discord!
  char em_edit_hit[64], em_del_hit[64];
  snprintf(em_edit_hit, sizeof em_edit_hit, "emoji-edit:%llu", (unsigned long long)new_em_id);
  snprintf(em_del_hit, sizeof em_del_hit, "emoji-delete:%llu", (unsigned long long)new_em_id);
  CT_CHECK(ss_scroll_to(em_edit_hit) && ss_click(em_edit_hit));
  CT_CHECK(ss_has("emoji-edit-name") && ss_has("emoji-edit-save") && ss_has("emoji-edit-cancel"));
  CT_CHECK(ss_click("emoji-edit-cancel") && !ss_has("emoji-edit-name"));

  CT_CHECK(ss_scroll_to(em_edit_hit) && ss_click(em_edit_hit));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("hyper_parrot");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(!ss_has("emoji-edit-name") &&
           strcmp(ccord_store_find_emoji(F.store, new_em_id)->name, "hyper_parrot") == 0);

  // Creator-ownership gating: when self has ONLY CREATE_EXPRESSIONS (not MANAGE_EXPRESSIONS),
  // modifying Ada's emoji (user_id != self_id) is blocked with an explanation, while own upload remains editable!
  ccord_id ada_em_id = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_guild_emoji_count(F.store, hq); i++) {
    const ccord_emoji *em = ccord_store_guild_emoji_at(F.store, hq, i);
    if (em && em->user_id != CCORD_ID_NONE && em->user_id != self_id) {
      ada_em_id = em->id;
      break;
    }
  }
  CT_CHECK(ada_em_id != CCORD_ID_NONE);

  const ccord_id *hq_roles = nullptr;
  size_t hq_nroles = 0;
  CT_CHECK(ccord_store_member_roles(F.store, hq, self_id, &hq_roles, &hq_nroles) && hq_nroles == 1);
  ccord_id hq_admin_rid = hq_roles[0];
  ccord_role orig_hq_admin = *ccord_store_role(F.store, hq, hq_admin_rid);
  char orig_hq_name[128];
  snprintf(orig_hq_name, sizeof orig_hq_name, "%s", orig_hq_admin.name ? orig_hq_admin.name : "");
  orig_hq_admin.name = orig_hq_name;
  ccord_role restricted_hq = orig_hq_admin;
  restricted_hq.permissions = CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_CREATE_EXPRESSIONS;
  CT_CHECK(ccord_store_upsert_role(F.store, &restricted_hq, &err) == CCORD_OK);
  frame(2);

  char ada_edit_hit[64], ada_del_hit[64];
  snprintf(ada_edit_hit, sizeof ada_edit_hit, "emoji-edit:%llu", (unsigned long long)ada_em_id);
  snprintf(ada_del_hit, sizeof ada_del_hit, "emoji-delete:%llu", (unsigned long long)ada_em_id);
  size_t adm_before_block = ccord_mock_admin_count(tr);
  CT_CHECK(ss_scroll_to(ada_edit_hit) && ss_click(ada_edit_hit));
  CT_CHECK(!ss_has("emoji-edit-name") && strstr(dbg().notice, "Manage Expressions") != nullptr);
  CT_CHECK(ss_scroll_to(ada_del_hit) && ss_click(ada_del_hit));
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE && ccord_mock_admin_count(tr) == adm_before_block);

  // Own emoji IS editable under CREATE_EXPRESSIONS:
  CT_CHECK(ss_scroll_to(em_edit_hit) && ss_click(em_edit_hit) && ss_has("emoji-edit-name"));
  CT_CHECK(ss_click("emoji-edit-cancel"));

  // Restore full admin permissions on HQ and delete hyper_parrot via confirmation modal with audit reason:
  CT_CHECK(ccord_store_upsert_role(F.store, &orig_hq_admin, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ss_scroll_to(em_del_hit) && ss_click(em_del_hit));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_DELETE_EMOJI);
  type_text("retired emoji");
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_find_emoji(F.store, new_em_id) == nullptr &&
           strcmp(ccord_mock_last_admin(tr)->reason, "retired emoji") == 0);

  // 4. Stickers page (SS_STICKERS = 8): APNG & Lottie sniffing, multipart upload, search, inline edit, delete:
  CT_CHECK(ss_click("page:stickers") && ssd().page == 8);
  frame(4);
  CT_CHECK(ccord_store_guild_stickers_admin_loaded(F.store, hq));
  size_t base_stickers = ccord_store_guild_sticker_count(F.store, hq);
  CT_CHECK(base_stickers >= 2 && ssd().rows == base_stickers);

  // APNG signature + acTL chunk before IDAT -> sniffed as image/apng!
  static const uint8_t k_tiny_apng[] = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
    0x00, 0x00, 0x00, 0x08, 'a', 'c', 'T', 'L', 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0,
    0x00, 0x00, 0x00, 0x00, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82,
  };
  write_test_file(ui_test_file("Wave Cat.png"), k_tiny_apng, sizeof k_tiny_apng);
  snprintf(F.pick_path, sizeof F.pick_path, "%s", ui_test_file("Wave Cat.png"));
  CT_CHECK(ss_scroll_to("sticker-pick") && ss_click("sticker-pick"));
  CT_CHECK(ss_scroll_to("sticker-new-tags") && ss_click("sticker-new-tags"));
  type_text("wave");
  CT_CHECK(ss_scroll_to("sticker-new-desc") && ss_click("sticker-new-desc"));
  type_text("Friendly cat waving");
  CT_CHECK(ss_scroll_to("sticker-upload") && ss_click("sticker-upload"));
  frame(4);
  CT_CHECK(ccord_store_guild_sticker_count(F.store, hq) == base_stickers + 1);
  CT_CHECK(strstr(ccord_mock_last_admin(tr)->content_type, "multipart/form-data; boundary=") ==
           ccord_mock_last_admin(tr)->content_type);

  ccord_id new_st_id = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_guild_sticker_count(F.store, hq); i++) {
    const ccord_sticker *st_it = ccord_store_guild_sticker_at(F.store, hq, i);
    if (st_it && strcmp(st_it->name, "Wave Cat") == 0) {
      CT_CHECK(st_it->format == CCORD_STICKER_APNG);
      new_st_id = st_it->id;
    }
  }
  CT_CHECK(new_st_id != CCORD_ID_NONE);

  // Search by description substring:
  CT_CHECK(ss_scroll_to("sticker-search") && ss_click("sticker-search"));
  type_text("Friendly cat");
  frame(1);
  CT_CHECK(ssd().rows == 1);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);

  // Inline edit sticker name, tags, description -> Save:
  char st_edit_hit[64], st_del_hit[64];
  snprintf(st_edit_hit, sizeof st_edit_hit, "sticker-edit:%llu", (unsigned long long)new_st_id);
  snprintf(st_del_hit, sizeof st_del_hit, "sticker-delete:%llu", (unsigned long long)new_st_id);
  CT_CHECK(ss_scroll_to(st_edit_hit) && ss_click(st_edit_hit));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Super Cat");
  CT_CHECK(ss_click("sticker-edit-tags"));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("super");
  CT_CHECK(ss_click("sticker-edit-desc"));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Updated cat");
  CT_CHECK(ss_click("sticker-edit-save"));
  frame(4);
  const ccord_sticker *edited_st = ccord_store_find_sticker(F.store, new_st_id);
  CT_CHECK(edited_st && strcmp(edited_st->name, "Super Cat") == 0 && strcmp(edited_st->tags, "super") == 0 &&
           strcmp(edited_st->description, "Updated cat") == 0);

  // Delete sticker via confirmation modal:
  CT_CHECK(ss_scroll_to(st_del_hit) && ss_click(st_del_hit));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_DELETE_STICKER);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_find_sticker(F.store, new_st_id) == nullptr);

  // 5. Soundboard page (SS_SOUNDBOARD = 9): OGG sniffing, volume chip (50%), upload, search, inline edit (75%), delete:
  CT_CHECK(ss_click("page:soundboard") && ssd().page == 9);
  frame(4);
  CT_CHECK(ccord_store_guild_soundboard_admin_loaded(F.store, hq));
  size_t base_sounds = ccord_store_guild_soundboard_count(F.store, hq);
  CT_CHECK(base_sounds >= 2 && ssd().rows == base_sounds);

  static const uint8_t k_tiny_ogg[] = {'O', 'g', 'g', 'S', 0, 2, 0, 0, 0, 0, 0, 0};
  write_test_file(ui_test_file("SynthBlast.ogg"), k_tiny_ogg, sizeof k_tiny_ogg);
  snprintf(F.pick_path, sizeof F.pick_path, "%s", ui_test_file("SynthBlast.ogg"));
  CT_CHECK(ss_scroll_to("sound-pick") && ss_click("sound-pick"));
  CT_CHECK(ss_scroll_to("sound-new-emoji") && ss_click("sound-new-emoji"));
  type_text("\xf0\x9f\x93\xa2");
  CT_CHECK(ss_scroll_to("sound-new-vol:50") && ss_click("sound-new-vol:50"));
  CT_CHECK(ss_scroll_to("sound-upload") && ss_click("sound-upload"));
  frame(4);
  CT_CHECK(ccord_store_guild_soundboard_count(F.store, hq) == base_sounds + 1);

  ccord_id new_sb_id = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_guild_soundboard_count(F.store, hq); i++) {
    const ccord_soundboard_sound *sb = ccord_store_guild_soundboard_at(F.store, hq, i);
    if (sb && strcmp(sb->name, "SynthBlast") == 0) {
      CT_CHECK(sb->volume_pct == 50 && strcmp(sb->emoji_name, "\xf0\x9f\x93\xa2") == 0);
      new_sb_id = sb->id;
    }
  }
  CT_CHECK(new_sb_id != CCORD_ID_NONE);

  // Search soundboard by emoji / name:
  CT_CHECK(ss_scroll_to("sound-search") && ss_click("sound-search"));
  type_text("SynthBlast");
  frame(1);
  CT_CHECK(ssd().rows == 1);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);

  // Inline edit sound name + 75% volume chip -> Save:
  char sb_edit_hit[64], sb_del_hit[64];
  snprintf(sb_edit_hit, sizeof sb_edit_hit, "sound-edit:%llu", (unsigned long long)new_sb_id);
  snprintf(sb_del_hit, sizeof sb_del_hit, "sound-delete:%llu", (unsigned long long)new_sb_id);
  CT_CHECK(ss_scroll_to(sb_edit_hit) && ss_click(sb_edit_hit));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Mega Horn");
  CT_CHECK(ss_click("sound-edit-vol:75"));
  CT_CHECK(ss_click("sound-edit-save"));
  frame(4);
  const ccord_soundboard_sound *edited_sb = ccord_store_find_soundboard(F.store, new_sb_id);
  CT_CHECK(edited_sb && strcmp(edited_sb->name, "Mega Horn") == 0 && edited_sb->volume_pct == 75);

  // Delete sound via confirmation modal:
  CT_CHECK(ss_scroll_to(sb_del_hit) && ss_click(sb_del_hit));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_DELETE_SOUNDBOARD);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_find_soundboard(F.store, new_sb_id) == nullptr);

  // Closing Server Settings forgets volatile admin-loaded flags while keeping expression data for pickers:
  ui_server_settings_close(F.ui);
  CT_CHECK(!ccord_store_guild_emojis_admin_loaded(F.store, hq) &&
           !ccord_store_guild_stickers_admin_loaded(F.store, hq) &&
           !ccord_store_guild_soundboard_admin_loaded(F.store, hq));
  CT_CHECK(ccord_store_guild_emoji_count(F.store, hq) == base_emojis &&
           ccord_store_guild_sticker_count(F.store, hq) == base_stickers &&
           ccord_store_guild_soundboard_count(F.store, hq) == base_sounds);

  remove(ui_test_file("party-parrot.gif"));
  remove(ui_test_file("Wave Cat.png"));
  remove(ui_test_file("SynthBlast.ogg"));
  F.pick_path[0] = '\0';
}

static void test_m4c_members_prune_modview_and_automod(void)
{
  ccord_error err;
  ccord_mock_config mc = {.seed = 73, .big_channel_messages = 20, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  const ccord_id hq = 2001, lua_g = 2002;

  // 1. Permission gating on Lua Lounge (2002): self has KICK/BAN/AUDIT, NOT MANAGE_GUILD or MANAGE_ROLES:
  //    SS_MEMBERS is visible (with Prune enabled), SS_AUTOMOD is hidden, inline role '+' and Transfer are hidden.
  CT_CHECK(ccord_ui_open_server_settings(F.ui, lua_g));
  frame(2);
  CT_CHECK(ssd().open && ss_has("page:members") && !ss_has("page:automod"));
  CT_CHECK(ss_click("page:members") && ssd().page == 10);
  CT_CHECK(ss_has("prune-toggle") && !ss_has("member-role-pick:1002") && !ss_has("member-transfer:1002"));
  ui_server_settings_close(F.ui);

  // 2. Full SS_MEMBERS workflow on C-Cord HQ (2001, self = 1001 Owner):
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_has("page:members") && ss_has("page:automod"));
  CT_CHECK(ss_click("page:members") && ssd().page == 10);
  size_t total_hq_members = ccord_store_member_count(F.store, hq);
  CT_CHECK(total_hq_members >= 6 && ssd().rows == total_hq_members);

  // Search filter by username ("Ada" = user 1002):
  CT_CHECK(ss_scroll_to("member-search") && ss_click("member-search"));
  type_text("Ada");
  frame(1);
  CT_CHECK(ssd().rows == 1);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
  CT_CHECK(ssd().rows == total_hq_members);

  // Role filter chip: filter by Ada's role, then reset via "All" (member-filter:0):
  const ccord_id *ada_roles = nullptr;
  size_t ada_nroles = 0;
  CT_CHECK(ccord_store_member_roles(F.store, hq, 1002, &ada_roles, &ada_nroles) && ada_nroles >= 1);
  ccord_id filter_rid = ada_roles[0];
  char filter_hit[64];
  snprintf(filter_hit, sizeof filter_hit, "member-filter:%llu", (unsigned long long)filter_rid);
  CT_CHECK(ss_scroll_to(filter_hit) && ss_click(filter_hit));
  CT_CHECK(ssd().rows >= 1 && ssd().rows < total_hq_members);
  CT_CHECK(ss_click("member-filter:0") && ssd().rows == total_hq_members);

  // Find a manageable non-everyone role strictly below self's highest role on HQ (self = 1001 holds Admins at index 0):
  ccord_id assign_rid = CCORD_ID_NONE;
  size_t hq_role_cnt = 0;
  const ccord_role *hq_role_arr = ccord_store_roles(F.store, hq, &hq_role_cnt);
  for (size_t i = 0; i < hq_role_cnt; i++) {
    const ccord_role *ro = &hq_role_arr[i];
    if (ro->id != hq && !ro->managed && i > 0) {
      assign_rid = ro->id;
      break;
    }
  }
  CT_CHECK(assign_rid != CCORD_ID_NONE);
  const ccord_id *u10_roles = nullptr;
  size_t u10_nroles = 0;
  CT_CHECK(ccord_store_member_roles(F.store, hq, 1010, &u10_roles, &u10_nroles) && u10_nroles == 0);

  char pick_hit[64], add_hit[64], del_hit[64];
  snprintf(pick_hit, sizeof pick_hit, "member-role-pick:%llu", 1010ULL);
  snprintf(add_hit, sizeof add_hit, "member-role-add:%llu:%llu", 1010ULL, (unsigned long long)assign_rid);
  snprintf(del_hit, sizeof del_hit, "member-role-del:%llu:%llu", 1010ULL, (unsigned long long)assign_rid);
  CT_CHECK(ss_scroll_to(pick_hit) && ss_click(pick_hit));
  CT_CHECK(ss_scroll_to(add_hit) && ss_click(add_hit));
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_ADD_MEMBER_ROLE);
  frame(4);
  CT_CHECK(ccord_store_member_roles(F.store, hq, 1010, &u10_roles, &u10_nroles) && u10_nroles == 1 &&
           u10_roles[0] == assign_rid);

  // Remove the role inline so 1010 returns to roleless:
  CT_CHECK(ss_scroll_to(del_hit) && ss_click(del_hit));
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_REMOVE_MEMBER_ROLE);
  frame(4);
  CT_CHECK(ccord_store_member_roles(F.store, hq, 1010, &u10_roles, &u10_nroles) && u10_nroles == 0);

  // Mod View side drawer: opens, fetches audit log once, keeps ESC close button unobscured, closes cleanly:
  CT_CHECK(ss_scroll_to("member-modview:1002") && ss_click("member-modview:1002"));
  frame(4);
  CT_CHECK(ss_has("modview-close") && ss_has("close") && ccord_store_audit_loaded(F.store, hq));
  CT_CHECK(ss_click("modview-close") && !ss_has("modview-close"));

  // Timeout + Clear Timeout + Kick/Ban/Transfer modal openers on member rows:
  CT_CHECK(ss_scroll_to("member-timeout:1010") && ss_click("member-timeout:1010"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_TIMEOUT);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_find_member(F.store, hq, 1010)->timeout_until_ms > 0);

  // Clicking Timeout again while timed out sends CCORD_ADMIN_TIMEOUT with until_ms == 0 directly:
  CT_CHECK(ss_scroll_to("member-timeout:1010") && ss_click("member-timeout:1010"));
  frame(4);
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_TIMEOUT &&
           ccord_mock_last_admin(tr)->until_ms == 0 &&
           ccord_store_find_member(F.store, hq, 1010)->timeout_until_ms == 0);

  CT_CHECK(ss_scroll_to("member-kick:1010") && ss_click("member-kick:1010"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_KICK);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);

  CT_CHECK(ss_scroll_to("member-ban:1010") && ss_click("member-ban:1010"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_BAN);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);

  // On HQ, owner_id is unset so member-transfer is hidden even for Admins:
  CT_CHECK(!ss_has("member-transfer:1002"));

  // 3. Prune panel: 7d auto-preview (2 members: 1009 + 1010), 30d preview (1 member: 1009), execute prune removes 1009!
  CT_CHECK(ss_scroll_to("prune-toggle") && ss_click("prune-toggle"));
  frame(4);
  ccord_prune_info pr = {0};
  CT_CHECK(ccord_store_prune_info(F.store, hq, &pr) && pr.has_result && !pr.executed && pr.days == 7 && pr.pruned == 2);

  CT_CHECK(ss_scroll_to("prune-days:30") && ss_click("prune-days:30"));
  CT_CHECK(ss_scroll_to("prune-preview") && ss_click("prune-preview"));
  frame(4);
  CT_CHECK(ccord_store_prune_info(F.store, hq, &pr) && !pr.executed && pr.days == 30 && pr.pruned == 1);

  CT_CHECK(ss_scroll_to("prune-reason") && ss_click("prune-reason"));
  type_text("30d inactive sweep");
  CT_CHECK(ss_scroll_to("prune-exec") && ss_click("prune-exec"));
  CT_CHECK(ss_has("prune-confirm") && ss_has("prune-cancel"));
  CT_CHECK(ss_click("prune-cancel") && !ss_has("prune-confirm"));
  CT_CHECK(ss_click("prune-exec") && ss_click("prune-confirm"));
  frame(4);
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_EXECUTE_PRUNE &&
           strcmp(ccord_mock_last_admin(tr)->reason, "30d inactive sweep") == 0);
  CT_CHECK(ccord_store_find_member(F.store, hq, 1009) == nullptr &&
           ccord_store_find_member(F.store, hq, 1010) != nullptr);
  CT_CHECK(ccord_store_prune_info(F.store, hq, &pr) && pr.executed && pr.pruned == 1);

  // 4. SS_AUTOMOD (page == 11): list + 30s cache, toggle enabled, create keyword rule, edit rule, delete rule:
  size_t adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("page:automod") && ssd().page == 11);
  frame(4);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 1 &&
           ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_AUTOMOD_RULES);
  CT_CHECK(ccord_store_automod_loaded(F.store, hq) && ssd().rows == 2);

  // Re-entering within 30s reuses cached AutoMod rules without spamming Discord:
  CT_CHECK(ss_click("page:overview") && ssd().page == 0);
  CT_CHECK(ss_click("page:automod") && ssd().page == 11);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 1);

  // Toggle rule 91001 off and back on:
  CT_CHECK(ss_scroll_to("automod-toggle:91001") && ss_click("automod-toggle:91001"));
  frame(4);
  size_t rcnt = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  CT_CHECK(rules && rcnt == 2 && !rules[0].enabled);
  CT_CHECK(ss_scroll_to("automod-toggle:91001") && ss_click("automod-toggle:91001"));
  frame(4);
  rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  CT_CHECK(rules && rules[0].enabled);

  // Create a new Keyword rule with block custom message + alert + 5m timeout + exempt role:
  CT_CHECK(ss_scroll_to("automod-new") && ss_click("automod-new"));
  CT_CHECK(ss_scroll_to("automod-name") && ss_click("automod-name"));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Scam Link Guard");
  CT_CHECK(ss_scroll_to("automod-keywords") && ss_click("automod-keywords"));
  type_text("phish.gg, *free-nitro*");
  CT_CHECK(ss_scroll_to("automod-allow") && ss_click("automod-allow"));
  type_text("discord.com");
  CT_CHECK(ss_scroll_to("automod-custom-msg") && ss_click("automod-custom-msg"));
  type_text("Scam links are not allowed");
  CT_CHECK(ss_scroll_to("automod-act-alert") && ss_click("automod-act-alert"));
  CT_CHECK(ss_scroll_to("automod-act-timeout") && ss_click("automod-act-timeout"));
  CT_CHECK(ss_scroll_to("automod-timeout-dur:300") && ss_click("automod-timeout-dur:300"));
  char ex_role_hit[64];
  snprintf(ex_role_hit, sizeof ex_role_hit, "automod-exempt-role:%llu", (unsigned long long)assign_rid);
  CT_CHECK(ss_scroll_to(ex_role_hit) && ss_click(ex_role_hit));
  CT_CHECK(ss_scroll_to("automod-save") && ss_click("automod-save"));
  frame(4);

  rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  CT_CHECK(rules && rcnt == 3);
  if (rules && rcnt == 3) {
    const ccord_automod_rule *created = &rules[2];
    ccord_id new_am_id = created->id;
    CT_CHECK(strcmp(created->name, "Scam Link Guard") == 0 && created->keyword_count == 2 &&
             created->allow_count == 1 && created->action_block && created->action_alert &&
             created->action_timeout && created->timeout_seconds == 300 &&
             created->exempt_role_count == 1 && created->exempt_role_ids[0] == assign_rid);

    // Edit the created rule:
    char am_edit_hit[64], am_del_hit[64], am_del_ok_hit[64], am_del_cancel_hit[64];
    snprintf(am_edit_hit, sizeof am_edit_hit, "automod-edit:%llu", (unsigned long long)new_am_id);
    snprintf(am_del_hit, sizeof am_del_hit, "automod-delete:%llu", (unsigned long long)new_am_id);
    snprintf(am_del_ok_hit, sizeof am_del_ok_hit, "automod-delete-confirm:%llu", (unsigned long long)new_am_id);
    snprintf(am_del_cancel_hit, sizeof am_del_cancel_hit, "automod-delete-cancel:%llu", (unsigned long long)new_am_id);

    CT_CHECK(ss_scroll_to(am_edit_hit) && ss_click(am_edit_hit));
    CT_CHECK(ss_scroll_to("automod-name") && ss_click("automod-name"));
    key(CCORD_KEY_A, CCORD_MOD_CTRL);
    type_text("Scam Link Guard v2");
    CT_CHECK(ss_scroll_to("automod-save") && ss_click("automod-save"));
    frame(4);
    rules = ccord_store_automod_rules(F.store, hq, &rcnt);
    CT_CHECK(rules && rcnt == 3 && strcmp(rules[2].name, "Scam Link Guard v2") == 0);

    // Delete rule (cancel first, then confirm):
    CT_CHECK(ss_scroll_to(am_del_hit) && ss_click(am_del_hit));
    CT_CHECK(ss_has(am_del_ok_hit) && ss_click(am_del_cancel_hit) && !ss_has(am_del_ok_hit));
    CT_CHECK(ss_click(am_del_hit) && ss_click(am_del_ok_hit));
    frame(4);
    rules = ccord_store_automod_rules(F.store, hq, &rcnt);
    CT_CHECK(rules && rcnt == 2);
  }

  ui_server_settings_close(F.ui);
  CT_CHECK(!ccord_store_automod_loaded(F.store, hq) && !ccord_store_prune_info(F.store, hq, &pr));

  // 5. Owner-only Transfer Ownership directly from SS_MEMBERS row button on an owned server:
  ccord_admin_request creq;
  CT_CHECK(ccord_admin_create_guild(&creq, "Members Owner Lab", &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id own_gid = ccord_store_selected_guild(F.store);
  CT_CHECK(ccord_store_guild(F.store, own_gid)->owner_id == 1001);
  CT_CHECK(ccord_mock_add_guild_member(tr, own_gid, 1002, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, own_gid));
  frame(2);
  CT_CHECK(ss_click("page:members") && ssd().page == 10);
  CT_CHECK(ss_scroll_to("member-transfer:1002") && ss_click("member-transfer:1002"));
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_TRANSFER_OWNERSHIP &&
           F.ui->modal_target_id == 1002);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, own_gid)->owner_id == 1002);
  if (ssd().open) ui_server_settings_close(F.ui);
}

// Regression: the AutoMod editor used to join rule lists into a 516-byte CSV and load them into 512-byte fields, so
// opening and saving a rule with more than ~17 keyword/allow items silently PATCHed a shortened list back to Discord.
static void test_automod_large_csv_round_trip(void)
{
  enum { AM_KEY_COUNT = 45, AM_ALLOW_COUNT = 45, AM_ITEM_MAX = 40, AM_REGEX_COUNT = 10, AM_REGEX_LEN = 260 };
  ccord_error err;
  ccord_mock_config mc = {.seed = 73, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  const ccord_id hq = 2001;
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_click("page:automod") && ssd().page == 11);
  frame(4);
  CT_CHECK(ccord_store_automod_loaded(F.store, hq));

  // 45 keywords (~27 bytes each) and 45 allow entries (~26 bytes each): both CSVs are > 1,200 bytes.
  // 10 regex patterns of 260 bytes each exercise the full regex cap (2,618-byte CSV).
  char kw_expect[AM_KEY_COUNT][AM_ITEM_MAX], al_expect[AM_ALLOW_COUNT][AM_ITEM_MAX];
  char rx_expect[AM_REGEX_COUNT][AM_REGEX_LEN + 1];
  char kw_csv[2048], al_csv[2048], rx_csv[2700];
  size_t kw_pos = 0, al_pos = 0, rx_pos = 0;
  kw_csv[0] = al_csv[0] = rx_csv[0] = '\0';
  for (size_t i = 0; i < AM_KEY_COUNT; i++) {
    (void)snprintf(kw_expect[i], sizeof kw_expect[i], "keyword-%02zu-0123456789abcdef", i);
    kw_pos += (size_t)snprintf(kw_csv + kw_pos, sizeof kw_csv - kw_pos, "%s%s", i ? ", " : "", kw_expect[i]);
  }
  for (size_t i = 0; i < AM_ALLOW_COUNT; i++) {
    (void)snprintf(al_expect[i], sizeof al_expect[i], "allow-%02zu-0123456789abcdefg", i);
    al_pos += (size_t)snprintf(al_csv + al_pos, sizeof al_csv - al_pos, "%s%s", i ? ", " : "", al_expect[i]);
  }
  for (size_t i = 0; i < AM_REGEX_COUNT; i++) {
    int pfx = snprintf(rx_expect[i], sizeof rx_expect[i], "regex-%02zu-", i);
    memset(rx_expect[i] + pfx, 'a' + (int)(i % 26u), AM_REGEX_LEN - (size_t)pfx);
    rx_expect[i][AM_REGEX_LEN] = '\0';
    rx_pos += (size_t)snprintf(rx_csv + rx_pos, sizeof rx_csv - rx_pos, "%s%s", i ? ", " : "", rx_expect[i]);
  }
  CT_CHECK(kw_pos > 1200u && al_pos > 1200u && rx_pos > 2500u);

  // Create a Keyword rule carrying all three large lists through the mock transport:
  CT_CHECK(ss_scroll_to("automod-new") && ss_click("automod-new"));
  CT_CHECK(ss_scroll_to("automod-keywords") && ss_click("automod-keywords"));
  type_text(kw_csv);
  CT_CHECK(ss_scroll_to("automod-regex") && ss_click("automod-regex"));
  type_text(rx_csv);
  CT_CHECK(ss_scroll_to("automod-allow") && ss_click("automod-allow"));
  type_text(al_csv);
  CT_CHECK(ss_scroll_to("automod-save") && ss_click("automod-save"));
  frame(4);

  size_t rcnt = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  ccord_id rule_id = CCORD_ID_NONE;
  for (size_t i = 0; rules && i < rcnt; i++) {
    if (strcmp(rules[i].name, "Custom Keywords") == 0) { rule_id = rules[i].id; break; }
  }
  CT_CHECK(rules && rcnt == 3 && rule_id != CCORD_ID_NONE);
  bool all_preserved = rule_id != CCORD_ID_NONE;
  if (all_preserved) {
    rules = ccord_store_automod_rules(F.store, hq, &rcnt);
    const ccord_automod_rule *ru = nullptr;
    for (size_t i = 0; i < rcnt; i++) if (rules[i].id == rule_id) { ru = &rules[i]; break; }
    all_preserved = ru && ru->keyword_count == AM_KEY_COUNT && ru->regex_count == AM_REGEX_COUNT &&
                    ru->allow_count == AM_ALLOW_COUNT;
    for (size_t i = 0; ru && i < AM_KEY_COUNT; i++) {
      all_preserved = all_preserved && ru->keywords[i] && strcmp(ru->keywords[i], kw_expect[i]) == 0;
    }
    for (size_t i = 0; ru && i < AM_REGEX_COUNT; i++) {
      all_preserved = all_preserved && ru->regex_patterns[i] && strcmp(ru->regex_patterns[i], rx_expect[i]) == 0;
    }
    for (size_t i = 0; ru && i < AM_ALLOW_COUNT; i++) {
      all_preserved = all_preserved && ru->allow_list[i] && strcmp(ru->allow_list[i], al_expect[i]) == 0;
    }
  }
  CT_CHECK(all_preserved);

  // Re-open the rule (the old 512-byte load lost most entries here), append one keyword, save, and check all 46 survive:
  char edit_hit[64];
  (void)snprintf(edit_hit, sizeof edit_hit, "automod-edit:%llu", (unsigned long long)rule_id);
  CT_CHECK(ss_scroll_to(edit_hit) && ss_click(edit_hit));
  CT_CHECK(ss_scroll_to("automod-keywords") && ss_click("automod-keywords"));
  key(CCORD_KEY_END, 0);
  type_text(", extra-keyword-9999");
  CT_CHECK(ss_scroll_to("automod-save") && ss_click("automod-save"));
  frame(4);

  rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  const ccord_automod_rule *edited = nullptr;
  for (size_t i = 0; rules && i < rcnt; i++) if (rules[i].id == rule_id) { edited = &rules[i]; break; }
  bool edit_kept_all = edited && edited->keyword_count == AM_KEY_COUNT + 1u && edited->allow_count == AM_ALLOW_COUNT &&
                       edited->regex_count == AM_REGEX_COUNT;
  for (size_t i = 0; edited && i < AM_KEY_COUNT; i++) {
    edit_kept_all = edit_kept_all && edited->keywords[i] && strcmp(edited->keywords[i], kw_expect[i]) == 0;
  }
  for (size_t i = 0; edited && i < AM_REGEX_COUNT; i++) {
    edit_kept_all = edit_kept_all && edited->regex_patterns[i] && strcmp(edited->regex_patterns[i], rx_expect[i]) == 0;
  }
  for (size_t i = 0; edited && i < AM_ALLOW_COUNT; i++) {
    edit_kept_all = edit_kept_all && edited->allow_list[i] && strcmp(edited->allow_list[i], al_expect[i]) == 0;
  }
  CT_CHECK(edit_kept_all && edited && strcmp(edited->keywords[AM_KEY_COUNT], "extra-keyword-9999") == 0);

  // Over-limit input (129 keywords) must fail inline with no request sent and no shortened list stored:
  CT_CHECK(ss_scroll_to(edit_hit) && ss_click(edit_hit));
  CT_CHECK(ss_scroll_to("automod-keywords") && ss_click("automod-keywords"));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  char over_csv[1024];
  size_t over_pos = 0;
  over_csv[0] = '\0';
  for (size_t i = 0; i <= CCORD_AUTOMOD_MAX_KEYWORDS; i++) {
    over_pos += (size_t)snprintf(over_csv + over_pos, sizeof over_csv - over_pos, "%sk%03zu", i ? ", " : "", i);
  }
  type_text(over_csv);
  size_t adm_before = ccord_mock_admin_count(tr);
  CT_CHECK(ss_scroll_to("automod-save") && ss_click("automod-save"));
  frame(2);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_before);   // rejected before the governed queue
  CT_CHECK(ss_scroll_to("automod-error") && ss_has("automod-error"));
  CT_CHECK(strstr(dbg().notice, "Too many keywords") != nullptr);
  rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  edited = nullptr;
  for (size_t i = 0; rules && i < rcnt; i++) if (rules[i].id == rule_id) { edited = &rules[i]; break; }
  CT_CHECK(edited && edited->keyword_count == AM_KEY_COUNT + 1u && edited->allow_count == AM_ALLOW_COUNT);

  // An over-long single item (61 bytes) is also rejected inline instead of being accepted and then silently cut:
  CT_CHECK(ss_scroll_to("automod-keywords") && ss_click("automod-keywords"));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  char long_kw[80];
  memset(long_kw, 'x', 61);
  long_kw[61] = '\0';
  type_text(long_kw);
  adm_before = ccord_mock_admin_count(tr);
  CT_CHECK(ss_scroll_to("automod-save") && ss_click("automod-save"));
  frame(2);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_before);
  CT_CHECK(strstr(dbg().notice, "60 characters") != nullptr);
  rules = ccord_store_automod_rules(F.store, hq, &rcnt);
  edited = nullptr;
  for (size_t i = 0; rules && i < rcnt; i++) if (rules[i].id == rule_id) { edited = &rules[i]; break; }
  CT_CHECK(edited && edited->keyword_count == AM_KEY_COUNT + 1u);

  CT_CHECK(ss_scroll_to("automod-cancel") && ss_click("automod-cancel"));
  if (ssd().open) ui_server_settings_close(F.ui);
}

static void test_m4c_integrations_and_community_ui(void)
{
  ccord_error err;
  ccord_transport *tr = F.transport;
  const ccord_id hq = 2001;

  // 1. Permission gating: Lua Lounge (2002, moderator without MANAGE_GUILD or MANAGE_WEBHOOKS) hides both pages
  //    and blocks session admin calls locally before touching the transport:
  size_t adm_pre = ccord_mock_admin_count(tr);
  ccord_admin_request sreq;
  CT_CHECK(ccord_admin_list_webhooks(&sreq, 2002, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_list_app_cmd_perms(&sreq, 2002, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_get_welcome_screen(&sreq, 2002, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_get_onboarding(&sreq, 2002, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_PERMISSION);
  // Session channel validation on HQ (2001): voice channel 3121 rejected for webhooks; non-announcement 3113 rejected for follow:
  CT_CHECK(ccord_admin_create_webhook(&sreq, hq, 3121, "Bad Chan", nullptr, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_INVALID);
  ccord_admin_webhook_edit bad_ch_we = {.channel_id = 3121};
  CT_CHECK(ccord_admin_edit_webhook(&sreq, hq, 95001, &bad_ch_we, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_follow_channel(&sreq, hq, 3113, 3111, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_follow_channel(&sreq, hq, 3112, 3121, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &sreq, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_admin_count(tr) == adm_pre);

  CT_CHECK(ccord_ui_open_server_settings(F.ui, 2002));
  frame(2);
  CT_CHECK(!ss_has("page:integrations") && !ss_has("page:community"));
  ui_server_settings_close(F.ui);

  // 2. SS_INTEGRATIONS on HQ (2001): fetches webhooks + app perms once, caches within 30s, refetches when stale:
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  size_t adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("page:integrations") && ssd().page == 12);
  frame(4);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 2);
  CT_CHECK(ccord_store_webhooks_loaded(F.store, hq) && ccord_store_app_cmd_perms_loaded(F.store, hq));

  // Re-entering within 30s does not spam; gateway WEBHOOKS_UPDATE note surfaces Refresh button without unsolicited REST:
  ccord_id stale_ch = CCORD_ID_NONE;
  CT_CHECK(ss_click("page:overview") && ss_click("page:integrations"));
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 + 2);
  ccord_store_note_webhooks_update(F.store, hq, 3111);
  frame(1);
  CT_CHECK(ccord_store_webhooks_stale(F.store, hq, &stale_ch) && ccord_mock_admin_count(tr) == adm0 + 2);
  CT_CHECK(ss_scroll_to("webhook-stale-refresh") && ss_click("webhook-stale-refresh"));
  frame(3);
  CT_CHECK(!ccord_store_webhooks_stale(F.store, hq, &stale_ch) && ccord_mock_admin_count(tr) == adm0 + 3);

  // Create webhook (verify no optimistic mutation before session_pump polls transport):
  size_t wh_before = 0;
  (void)ccord_store_webhooks(F.store, hq, &wh_before);
  CT_CHECK(ss_scroll_to("webhook-new-name") && ss_click("webhook-new-name"));
  type_text("Deploy Alert");
  CT_CHECK(ss_scroll_to("webhook-new-channel") && ss_click("webhook-new-channel"));
  ccord_rect wc_rc;
  CT_CHECK(ss_scroll_to("webhook-create") && ccord_ui_server_settings_find(F.ui, "webhook-create", &wc_rc));
  mouse(CCORD_IN_MOUSE_MOVE, 0, wc_rc.x + wc_rc.w * 0.5f, wc_rc.y + wc_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, wc_rc.x + wc_rc.w * 0.5f, wc_rc.y + wc_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 1, wc_rc.x + wc_rc.w * 0.5f, wc_rc.y + wc_rc.h * 0.5f);
  size_t wh_mid = 0;
  (void)ccord_store_webhooks(F.store, hq, &wh_mid);
  CT_CHECK(wh_mid == wh_before);   // zero optimistic mutation before Discord answers!
  frame(4);
  size_t wh_cnt = 0;
  const ccord_webhook *whs = ccord_store_webhooks(F.store, hq, &wh_cnt);
  CT_CHECK(whs && wh_cnt == wh_before + 1u);
  ccord_id wh_id = whs[wh_cnt - 1u].id;

  // Copy webhook URL, edit webhook name + channel, delete (cancel + confirm):
  char wh_copy[64], wh_edit[64], wh_del[64], wh_del_ok[64], wh_del_cancel[64];
  snprintf(wh_copy, sizeof wh_copy, "webhook-copy-url:%llu", (unsigned long long)wh_id);
  snprintf(wh_edit, sizeof wh_edit, "webhook-edit:%llu", (unsigned long long)wh_id);
  snprintf(wh_del, sizeof wh_del, "webhook-delete:%llu", (unsigned long long)wh_id);
  snprintf(wh_del_ok, sizeof wh_del_ok, "webhook-delete-confirm:%llu", (unsigned long long)wh_id);
  snprintf(wh_del_cancel, sizeof wh_del_cancel, "webhook-delete-cancel:%llu", (unsigned long long)wh_id);

  F.clip[0] = '\0';
  CT_CHECK(ss_scroll_to(wh_copy) && ss_click(wh_copy));
  CT_CHECK(strstr(F.clip, "https://discord.com/api/webhooks/") != nullptr);

  CT_CHECK(ss_scroll_to(wh_edit) && ss_click(wh_edit));
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Deploy Alert v2");
  CT_CHECK(ss_scroll_to("webhook-edit-save") && ss_click("webhook-edit-save"));
  frame(4);
  whs = ccord_store_webhooks(F.store, hq, &wh_cnt);
  CT_CHECK(whs && strcmp(whs[wh_cnt - 1u].name, "Deploy Alert v2") == 0);

  CT_CHECK(ss_scroll_to(wh_del) && ss_click(wh_del));
  CT_CHECK(ss_has(wh_del_ok) && ss_click(wh_del_cancel) && !ss_has(wh_del_ok));
  CT_CHECK(ss_click(wh_del) && ss_click(wh_del_ok));
  frame(4);
  whs = ccord_store_webhooks(F.store, hq, &wh_cnt);
  CT_CHECK(wh_cnt == wh_before);

  // Follow announcement channel -> records followed channel AND refreshes webhooks list:
  CT_CHECK(ss_scroll_to("follow-submit") && ss_click("follow-submit"));
  frame(5);
  size_t fc_cnt = 0;
  (void)ccord_store_followed_channels(F.store, hq, &fc_cnt);
  whs = ccord_store_webhooks(F.store, hq, &wh_cnt);
  CT_CHECK(fc_cnt >= 1u && wh_cnt == wh_before + 1u);
  ui_server_settings_close(F.ui);

  // 3. SS_COMMUNITY on a fresh non-community server: Enable Community wizard, Welcome Screen, and Onboarding:
  ccord_admin_request creq;
  CT_CHECK(ccord_admin_create_guild(&creq, "UI Community Lab", &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id cgid = ccord_store_selected_guild(F.store);
  CT_CHECK(ccord_admin_create_channel(&creq, cgid, "updates", 0, 0, "Public updates", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id first_ch = ccord_store_channel_at(F.store, cgid, 0)->id;

  CT_CHECK(ccord_ui_open_server_settings(F.ui, cgid));
  frame(2);
  adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("page:community") && ssd().page == 13);
  frame(2);
  // Before COMMUNITY is enabled, Welcome Screen and Onboarding are NOT fetched:
  CT_CHECK(ccord_mock_admin_count(tr) == adm0 && ss_has("community-enable-submit"));

  ccord_rect ce_rc;
  CT_CHECK(ccord_ui_server_settings_find(F.ui, "community-enable-submit", &ce_rc));
  mouse(CCORD_IN_MOUSE_MOVE, 0, ce_rc.x + ce_rc.w * 0.5f, ce_rc.y + ce_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, ce_rc.x + ce_rc.w * 0.5f, ce_rc.y + ce_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 1, ce_rc.x + ce_rc.w * 0.5f, ce_rc.y + ce_rc.h * 0.5f);
  CT_CHECK(!ccord_store_guild(F.store, cgid)->feature_community);   // no optimistic mutation!
  frame(6);
  const ccord_guild *cgd = ccord_store_guild(F.store, cgid);
  CT_CHECK(cgd && cgd->feature_community && cgd->rules_channel_id != CCORD_ID_NONE &&
           cgd->public_updates_channel_id != CCORD_ID_NONE);
  CT_CHECK(ccord_store_welcome_screen_loaded(F.store, cgid) && ccord_store_onboarding_loaded(F.store, cgid));

  // Welcome Screen edit + save:
  CT_CHECK(ss_scroll_to("welcome-enabled") && ss_click("welcome-enabled"));
  CT_CHECK(ss_scroll_to("welcome-desc") && ss_click("welcome-desc"));
  type_text("Welcome to UI Community Lab");
  CT_CHECK(ss_scroll_to("welcome-add-desc") && ss_click("welcome-add-desc"));
  type_text("Check out #general");
  CT_CHECK(ss_scroll_to("welcome-chan-add") && ss_click("welcome-chan-add"));
  CT_CHECK(ss_scroll_to("welcome-save") && ss_click("welcome-save"));
  frame(4);
  const ccord_welcome_screen *ws = ccord_store_welcome_screen(F.store, cgid);
  CT_CHECK(ws && ws->enabled && ws->channel_count == 1u &&
           strcmp(ws->description, "Welcome to UI Community Lab") == 0);

  // Server Onboarding edit + save:
  char def_hit[64], opt_ch_hit[64];
  snprintf(def_hit, sizeof def_hit, "onboarding-defchan:%llu", (unsigned long long)first_ch);
  snprintf(opt_ch_hit, sizeof opt_ch_hit, "onboarding-opt-chan:%llu", (unsigned long long)first_ch);
  CT_CHECK(ss_scroll_to(def_hit) && ss_click(def_hit));
  CT_CHECK(ss_scroll_to("onboarding-mode:1") && ss_click("onboarding-mode:1"));
  CT_CHECK(ss_scroll_to("onboarding-prompt-new") && ss_click("onboarding-prompt-new"));
  CT_CHECK(ss_scroll_to("onboarding-prompt-title") && ss_click("onboarding-prompt-title"));
  type_text("Choose your track");
  CT_CHECK(ss_scroll_to("onboarding-opt-title") && ss_click("onboarding-opt-title"));
  type_text("Core Engine");
  CT_CHECK(ss_scroll_to(opt_ch_hit) && ss_click(opt_ch_hit));
  CT_CHECK(ss_scroll_to("onboarding-opt-add") && ss_click("onboarding-opt-add"));
  CT_CHECK(ss_scroll_to("onboarding-prompt-save") && ss_click("onboarding-prompt-save"));
  CT_CHECK(ss_scroll_to("onboarding-enabled") && ss_click("onboarding-enabled"));
  CT_CHECK(ss_scroll_to("onboarding-save") && ss_click("onboarding-save"));
  frame(4);
  const ccord_onboarding *ob = ccord_store_onboarding(F.store, cgid);
  CT_CHECK(ob && ob->enabled && ob->mode == CCORD_ONBOARDING_ADVANCED && ob->prompt_count == 1u &&
           strcmp(ob->prompts[0].title, "Choose your track") == 0);

  ui_server_settings_close(F.ui);
}

static ccord_ui_channel_settings_debug csd(void)
{
  ccord_ui_channel_settings_debug d;
  ccord_ui_get_channel_settings_debug(F.ui, &d);
  return d;
}

static bool cs_has(const char *name)
{
  return ccord_ui_channel_settings_find(F.ui, name, nullptr);
}

static bool cs_click(const char *name)
{
  ccord_rect r;
  if (!ccord_ui_channel_settings_find(F.ui, name, &r)) return false;
  click(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  return true;
}

static bool cs_press(const char *name)
{
  ccord_rect r;
  if (!ccord_ui_channel_settings_find(F.ui, name, &r)) return false;
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  return true;
}

static bool cs_scroll_to(const char *name)
{
  const float x = F.ui->win_w - 220.0f, y = F.ui->win_h * 0.5f;
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = x, .y = y, .wheel = 1000.0f});
  frame(1);
  for (int i = 0; i < 120 && !cs_has(name); i++) {
    send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = x, .y = y, .wheel = -3.0f});
    frame(1);
  }
  return cs_has(name);
}

static void test_m4d_channel_settings_clone_and_voice_moderation(void)
{
  ccord_error err;
  ccord_mock_config mc = {.seed = 101, .big_channel_messages = 15, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  const ccord_id hq = 2001, lua_g = 2002, garden = 2003;
  const ccord_id dev = 3113, voice_lounge = 3121, ada = 1002;

  // 1. Permission gating: plain member on Pixel Garden (2003) sees no gear icon, no Edit/Clone Channel, and cannot open Channel Settings.
  CT_CHECK(ccord_session_select_guild(F.session, garden, &err) == CCORD_OK);
  frame(3);
  ccord_id g_art = find_channel(garden, "art", CCORD_CHANNEL_TEXT);
  CT_CHECK(g_art != CCORD_ID_NONE && !ccord_ui_open_channel_settings(F.ui, g_art));
  float art_y = scan_hover_channel(180.0f, g_art);
  frame(1);
  CT_CHECK(art_y > 0.0f && !ccord_ui_channel_gear_rect(F.ui, g_art, nullptr));
  open_menu(CCORD_MENU_CHANNEL, garden, g_art, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(!menu_has("Edit Channel") && !menu_has("Clone Channel"));
  ui_menu_close(F.ui);

  // 2. Switch to C-Cord HQ (2001): hover #dev -> click gear icon -> opens Channel Settings Overview!
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(3);
  float dev_y = scan_hover_channel(180.0f, dev);
  frame(1);
  ccord_rect gear_rc = {0};
  CT_CHECK(dev_y > 0.0f && ccord_ui_channel_gear_rect(F.ui, dev, &gear_rc) && gear_rc.w > 0.0f);
  click_rect(gear_rc);
  frame(2);
  CT_CHECK(csd().open && csd().channel_id == dev && csd().page == 0 && !csd().unsaved);

  // Overview edit: Reset restores base; Save sends PATCH /channels/{c} with zero optimistic mutation!
  CT_CHECK(cs_scroll_to("nsfw") && cs_click("nsfw") && csd().unsaved && csd().work_nsfw);
  CT_CHECK(cs_click("reset") && !csd().unsaved && !csd().work_nsfw);
  CT_CHECK(cs_click("field:topic") && csd().focus == 2);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("M4d dev channel");
  CT_CHECK(cs_scroll_to("nsfw") && cs_click("nsfw"));
  CT_CHECK(cs_scroll_to("slowmode:15") && cs_click("slowmode:15"));
  CT_CHECK(cs_scroll_to("archive:4320") && cs_click("archive:4320"));
  CT_CHECK(csd().unsaved && cs_press("save"));
  CT_CHECK(!ccord_store_channel(F.store, dev)->nsfw);   // zero optimistic mutation before transport answers!
  frame(4);
  const ccord_channel *dev_ch = ccord_store_channel(F.store, dev);
  CT_CHECK(dev_ch && dev_ch->nsfw && dev_ch->rate_limit_per_user == 15 &&
           dev_ch->default_auto_archive_duration == 4320 && strcmp(dev_ch->topic, "M4d dev channel") == 0);

  // 3. Permission Overwrites (page 1): add role overwrite (5003), tri-state Allow/Deny/Inherit, ESC block on dirty, Save & Remove Overwrite!
  CT_CHECK(cs_click("page:permissions") && csd().page == 1 && csd().overwrite_id == hq);
  CT_CHECK(cs_click("ow_add_toggle") && cs_click("ow_add_role:5003") && csd().overwrite_id == 5003);
  CT_CHECK(cs_scroll_to("ow_allow:400") && cs_click("ow_allow:400"));
  CT_CHECK(cs_scroll_to("ow_deny:800") && cs_click("ow_deny:800"));
  CT_CHECK(csd().unsaved && (csd().work_allow & 0x400u) != 0 && (csd().work_deny & 0x800u) != 0);
  // ESC while unsaved flashes warning bar instead of closing:
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(csd().open && csd().unsaved);

  size_t ow_before = 0;
  (void)ccord_store_channel_overwrites(F.store, dev, &ow_before);
  CT_CHECK(cs_press("save"));
  size_t ow_mid = 0;
  (void)ccord_store_channel_overwrites(F.store, dev, &ow_mid);
  CT_CHECK(ow_mid == ow_before);   // zero optimistic mutation!
  frame(4);
  size_t ow_after = 0;
  const ccord_overwrite *ows = ccord_store_channel_overwrites(F.store, dev, &ow_after);
  CT_CHECK(ows && ow_after == ow_before + 1u && !csd().unsaved);

  // Tri-state Inherit clears Deny bit, Save updates overwrite in place:
  CT_CHECK(cs_scroll_to("ow_inherit:800") && cs_click("ow_inherit:800"));
  CT_CHECK((csd().work_deny & 0x800u) == 0 && cs_click("save"));
  frame(4);
  ows = ccord_store_channel_overwrites(F.store, dev, &ow_after);
  bool saw_updated_ow = false;
  for (size_t i = 0; i < ow_after; i++) {
    if (ows[i].id == 5003 && ows[i].allow == 0x400u && ows[i].deny == 0u) saw_updated_ow = true;
  }
  CT_CHECK(saw_updated_ow);

  // Add Member overwrite (Ada 1002), Save, then Remove Overwrite (DELETE):
  CT_CHECK(cs_scroll_to("ow_add_toggle") && cs_click("ow_add_toggle") && cs_click("ow_add_member:1002"));
  CT_CHECK(cs_scroll_to("ow_deny:800") && cs_click("ow_deny:800") && cs_click("save"));
  frame(4);
  CT_CHECK(cs_scroll_to("ow_delete") && cs_click("ow_delete"));
  frame(4);
  ows = ccord_store_channel_overwrites(F.store, dev, &ow_after);
  for (size_t i = 0; i < ow_after; i++) CT_CHECK(ows[i].id != 1002);

  // 4. Channel Invites (page 2): TTL reuse, Refresh gap, Create, Copy, Revoke, and volatile cleanup on close:
  size_t adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(cs_click("page:invites") && csd().page == 2);
  frame(4);
  CT_CHECK(ccord_store_channel_invites_loaded(F.store, hq, dev) && ccord_mock_admin_count(tr) == adm0 + 1);
  // Switching away and back within 30s reuses loaded channel invites (0 extra requests):
  CT_CHECK(cs_click("page:overview") && cs_click("page:invites") && ccord_mock_admin_count(tr) == adm0 + 1);

  size_t cinv_before = csd().rows;
  CT_CHECK(cs_click("inv_age") && cs_click("inv_uses") && cs_click("inv_create"));
  frame(4);
  CT_CHECK(csd().rows == cinv_before + 1u);
  size_t ncinv = 0;
  const ccord_invite *cinvs = ccord_store_channel_invites(F.store, hq, dev, &ncinv);
  CT_CHECK(cinvs && ncinv >= 1u);
  if (cinvs && ncinv >= 1u) {
    char copy_nm[64], rev_nm[64];
    snprintf(copy_nm, sizeof copy_nm, "inv_copy:%s", cinvs[0].code);
    snprintf(rev_nm, sizeof rev_nm, "inv_revoke:%s", cinvs[0].code);
    F.clip[0] = '\0';
    CT_CHECK(cs_click(copy_nm) && strstr(F.clip, cinvs[0].code) != nullptr);
    CT_CHECK(cs_click(rev_nm));
    frame(4);
    CT_CHECK(csd().rows == cinv_before);
  }

  CT_CHECK(cs_click("close") && !csd().open && !ccord_store_channel_invites_loaded(F.store, hq, dev));

  // 5. Voice channel Overview (bitrate + user_limit + Unlimited 0 clear) & Forum channel available_tags:
  CT_CHECK(ccord_ui_open_channel_settings(F.ui, voice_lounge));
  frame(2);
  CT_CHECK(cs_scroll_to("bitrate:96000") && cs_click("bitrate:96000"));
  CT_CHECK(cs_scroll_to("user_limit:10") && cs_click("user_limit:10"));
  CT_CHECK(cs_scroll_to("user_limit:inc") && cs_click("user_limit:inc") && csd().work_user_limit == 11);
  CT_CHECK(cs_click("save"));
  frame(4);
  const ccord_channel *vc_ch = ccord_store_channel(F.store, voice_lounge);
  CT_CHECK(vc_ch && vc_ch->bitrate == 96000 && vc_ch->user_limit == 11);
  // Clear user_limit back to Unlimited (0):
  CT_CHECK(cs_scroll_to("user_limit:0") && cs_click("user_limit:0") && cs_click("save"));
  frame(4);
  vc_ch = ccord_store_channel(F.store, voice_lounge);
  CT_CHECK(vc_ch && vc_ch->user_limit == 0);
  CT_CHECK(cs_click("close"));

  // Create a Forum channel and edit available_tags in Channel Settings:
  ccord_admin_request creq;
  CT_CHECK(ccord_admin_create_channel(&creq, hq, "q-and-a", 15, 0, "Forum questions", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id forum_id = find_channel(hq, "q-and-a", CCORD_CHANNEL_TEXT);
  CT_CHECK(forum_id != CCORD_ID_NONE && ccord_ui_open_channel_settings(F.ui, forum_id));
  frame(2);
  CT_CHECK(cs_scroll_to("field:tag_name") && cs_click("field:tag_name"));
  type_text("solved");
  CT_CHECK(cs_scroll_to("field:tag_emoji") && cs_click("field:tag_emoji"));
  type_text("✅");
  CT_CHECK(cs_scroll_to("tag_new_mod") && cs_click("tag_new_mod"));
  CT_CHECK(cs_scroll_to("tag_add") && cs_click("tag_add") && csd().work_tag_count == 1);
  CT_CHECK(cs_click("save"));
  frame(4);
  const ccord_forum_meta *fm = ccord_store_forum_meta(F.store, forum_id);
  CT_CHECK(fm && fm->tag_count == 1 && strcmp(fm->tags[0].name, "solved") == 0 &&
           fm->tags[0].moderated);

  // Delete Channel directly from Channel Settings sidebar:
  CT_CHECK(cs_click("delete_channel") && F.ui->modal_kind == UI_MODAL_CONFIRM &&
           F.ui->confirm_op == CCORD_ADMIN_DELETE_CHANNEL);
  click_rect(dbg().modal_ok_btn);
  frame(4);
  CT_CHECK(ccord_store_channel(F.store, forum_id) == nullptr && !csd().open);

  // 6. Clone Channel via channel context menu (copies topic, NSFW, slowmode, archive, and overwrites):
  size_t nch_before = ccord_store_channel_count(F.store, hq);
  open_menu(CCORD_MENU_CHANNEL, hq, dev, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_has("Edit Channel") && menu_has("Clone Channel"));
  CT_CHECK(menu_click_label("Clone Channel"));
  frame(4);
  CT_CHECK(ccord_store_channel_count(F.store, hq) == nch_before + 1u);
  ccord_id cloned_id = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, hq); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, hq, i);
    if (c && c->id != dev && strcmp(c->name, "dev") == 0) cloned_id = c->id;
  }
  const ccord_channel *cloned_ch = cloned_id ? ccord_store_channel(F.store, cloned_id) : nullptr;
  size_t cloned_ows = 0;
  (void)ccord_store_channel_overwrites(F.store, cloned_id, &cloned_ows);
  CT_CHECK(cloned_ch && cloned_ch->nsfw && cloned_ch->rate_limit_per_user == 15 &&
           cloned_ch->default_auto_archive_duration == 4320 && cloned_ows == ow_after);

  // Category Channel Settings (3110 = TEXT CHANNELS) hides Invites tab while offering Overview & Permissions:
  CT_CHECK(ccord_ui_open_channel_settings(F.ui, 3110));
  frame(2);
  CT_CHECK(csd().open && cs_click("page:permissions") && csd().page == 1 && !cs_click("page:invites"));
  CT_CHECK(cs_click("close") && !csd().open);

  // 7. Session pre-checks (isolated from mock_transport via exact session error messages & store state):
  // 7a. Thread overwrite rejection:
  ccord_channel tmp_thread = {
    .id = 993119, .guild_id = hq, .parent_id = dev, .kind = CCORD_CHANNEL_TEXT,
    .discord_type = 11, .name = "temp-thread", .topic = "",
  };
  CT_CHECK(ccord_store_upsert_channel(F.store, &tmp_thread, &err) == CCORD_OK);
  ccord_overwrite th_ow = {.id = hq, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_SEND_MESSAGES};
  CT_CHECK(ccord_admin_put_channel_overwrite(&creq, hq, 993119, &th_ow, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_INVALID &&
           strstr(err.message, "Threads inherit permissions") != nullptr);
  (void)ccord_store_delete_channel(F.store, 993119, &err);

  // 7b. Missing MANAGE_ROLES on garden (2003), plus escalation check on Lua Lounge (2002):
  ccord_overwrite esc_ow = {.id = garden, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_VIEW_CHANNEL};
  CT_CHECK(ccord_admin_put_channel_overwrite(&creq, garden, 3311, &esc_ow, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION &&
           strstr(err.message, "Manage Permissions") != nullptr);

  const ccord_role *ev_ro_ptr = ccord_store_role(F.store, lua_g, lua_g);
  CT_CHECK(ev_ro_ptr != nullptr);
  if (ev_ro_ptr) {
    char rname[128];
    snprintf(rname, sizeof rname, "%s", ev_ro_ptr->name ? ev_ro_ptr->name : "@everyone");
    ccord_role orig_ro = *ev_ro_ptr;
    orig_ro.name = rname;
    ccord_role mod_ro = orig_ro;
    mod_ro.permissions = (orig_ro.permissions | CCORD_PERM_MANAGE_ROLES | CCORD_PERM_MANAGE_CHANNELS |
                          CCORD_PERM_MOVE_MEMBERS) & ~CCORD_PERM_ADMINISTRATOR;
    CT_CHECK(ccord_store_upsert_role(F.store, &mod_ro, &err) == CCORD_OK);

    // Overwrite escalation (has MANAGE_ROLES, lacks ADMINISTRATOR):
    ccord_overwrite esc_lua = {.id = lua_g, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_ADMINISTRATOR};
    CT_CHECK(ccord_admin_put_channel_overwrite(&creq, lua_g, 3211, &esc_lua, nullptr, &err) == CCORD_OK);
    CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION &&
             strstr(err.message, "permissions you don't hold") != nullptr);

    // Clone overwrite escalation (has MANAGE_CHANNELS, lacks ADMINISTRATOR):
    ccord_admin_channel_clone esc_cl = {
      .name = "esc-clone", .discord_type = 0, .overwrites = &esc_lua, .overwrite_count = 1,
    };
    CT_CHECK(ccord_admin_clone_channel(&creq, lua_g, &esc_cl, nullptr, &err) == CCORD_OK);
    CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION &&
             strstr(err.message, "clone permission overwrites you don't hold") != nullptr);

    // Voice Move destination missing CONNECT check:
    ccord_channel src_vc = {.id = 993201, .guild_id = lua_g, .kind = CCORD_CHANNEL_VOICE, .discord_type = 2, .name = "SrcVC", .topic = ""};
    ccord_channel dst_vc = {.id = 993202, .guild_id = lua_g, .kind = CCORD_CHANNEL_VOICE, .discord_type = 2, .name = "LockedVC", .topic = ""};
    CT_CHECK(ccord_store_upsert_channel(F.store, &src_vc, &err) == CCORD_OK);
    CT_CHECK(ccord_store_upsert_channel(F.store, &dst_vc, &err) == CCORD_OK);
    ccord_overwrite deny_conn = {.id = lua_g, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_CONNECT};
    CT_CHECK(ccord_store_upsert_channel_overwrite(F.store, 993202, lua_g, &deny_conn, &err) == CCORD_OK);
    ccord_voice_state lvs = {.guild_id = lua_g, .channel_id = 993201, .user_id = 1004};
    CT_CHECK(ccord_store_set_voice_state(F.store, &lvs, &err) == CCORD_OK);
    CT_CHECK(ccord_admin_move_voice_member(&creq, lua_g, 1004, 993202, nullptr, &err) == CCORD_OK);
    CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION &&
             strstr(err.message, "Connect permission in the destination") != nullptr);
    lvs.channel_id = CCORD_ID_NONE;
    (void)ccord_store_set_voice_state(F.store, &lvs, &err);
    (void)ccord_store_delete_channel(F.store, 993201, &err);
    (void)ccord_store_delete_channel(F.store, 993202, &err);

    CT_CHECK(ccord_store_upsert_role(F.store, &orig_ro, &err) == CCORD_OK);
  }

  // 7c. Channel invites category rejection & permission gate, and Clone permission gate:
  CT_CHECK(ccord_admin_list_channel_invites(&creq, hq, 3110, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_INVALID &&
           strstr(err.message, "Channel invites are not available") != nullptr);
  CT_CHECK(ccord_admin_list_channel_invites(&creq, garden, 3311, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION &&
           strstr(err.message, "Manage Channels permission to view channel invites") != nullptr);
  ccord_admin_channel_clone g_cl = {.name = "garden-copy", .discord_type = 0};
  CT_CHECK(ccord_admin_clone_channel(&creq, garden, &g_cl, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION &&
           strstr(err.message, "Manage Channels permission to clone channels") != nullptr);

  // 8. Voice Moderation: right-click Ada (1002) in voice channel 3121 -> Server Mute, Server Deafen, Move to, Disconnect!
  CT_CHECK(ccord_admin_create_channel(&creq, hq, "Stage Room", 2, 0, nullptr, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(F.session, &creq, &err) == CCORD_OK);
  frame(4);
  ccord_id vc2 = find_channel(hq, "Stage Room", CCORD_CHANNEL_VOICE);
  CT_CHECK(vc2 != CCORD_ID_NONE);

  // Member-list right-click (channel_id == 0) does NOT offer voice moderation items:
  open_menu(CCORD_MENU_MEMBER, hq, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  CT_CHECK(!menu_has("Server Mute") && !menu_has("Disconnect"));
  ui_menu_close(F.ui);

  // Voice-row right-click (channel_id == voice_lounge) offers Server Mute, Server Deafen, Move to, Disconnect:
  float ada_vy = 0.0f;
  for (float y = 60.0f; y < 860.0f; y += 6.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, 180.0f, y);
    frame(1);
    if (dbg().hover_user == ada) {
      ada_vy = y;
      break;
    }
  }
  CT_CHECK(ada_vy > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 180.0f, ada_vy);
  frame(1);
  CT_CHECK(menu_has("Server Mute") && menu_has("Server Deafen") && menu_has("Move to") && menu_has("Disconnect"));

  // Click Server Mute (verify zero optimistic mutation before frame):
  ccord_rect mute_rc = {0};
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Server Mute", &mute_rc));
  mouse(CCORD_IN_MOUSE_MOVE, 0, mute_rc.x + mute_rc.w * 0.5f, mute_rc.y + mute_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, mute_rc.x + mute_rc.w * 0.5f, mute_rc.y + mute_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 1, mute_rc.x + mute_rc.w * 0.5f, mute_rc.y + mute_rc.h * 0.5f);
  CT_CHECK(!ccord_store_voice_user(F.store, hq, ada)->mute);   // zero optimistic mutation!
  frame(4);
  CT_CHECK(ccord_store_voice_user(F.store, hq, ada)->mute && ccord_store_find_member(F.store, hq, ada)->mute);

  // Server Deafen:
  open_menu(CCORD_MENU_MEMBER, hq, voice_lounge, ada, CCORD_ID_NONE);
  CT_CHECK(menu_click_label("Server Deafen"));
  frame(4);
  CT_CHECK(ccord_store_voice_user(F.store, hq, ada)->deaf && ccord_store_find_member(F.store, hq, ada)->deaf);

  // Move to -> Stage Room:
  open_menu(CCORD_MENU_MEMBER, hq, voice_lounge, ada, CCORD_ID_NONE);
  menu_point_at(menu_index("Move to"));
  ccord_rect mv_rc = {0};
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Stage Room", &mv_rc));
  click_rect(mv_rc);
  frame(4);
  CT_CHECK(ccord_store_voice_user(F.store, hq, ada)->channel_id == vc2);

  // Disconnect:
  open_menu(CCORD_MENU_MEMBER, hq, vc2, ada, CCORD_ID_NONE);
  CT_CHECK(menu_click_label("Disconnect"));
  frame(4);
  CT_CHECK(ccord_store_voice_user(F.store, hq, ada) == nullptr);

  // Session pre-check: disconnecting an already-disconnected user fails with CCORD_ERR_INVALID, and voice moderation on Pixel Garden fails with CCORD_ERR_PERMISSION:
  CT_CHECK(ccord_admin_move_voice_member(&creq, hq, ada, CCORD_ID_NONE, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_INVALID &&
           strstr(err.message, "not connected to a voice channel") != nullptr);
  ccord_id g_vc = find_channel(garden, "Chill", CCORD_CHANNEL_VOICE);
  if (g_vc == CCORD_ID_NONE) {
    for (size_t i = 0; i < ccord_store_channel_count(F.store, garden); i++) {
      const ccord_channel *c = ccord_store_channel_at(F.store, garden, i);
      if (c && c->kind == CCORD_CHANNEL_VOICE) { g_vc = c->id; break; }
    }
  }
  CT_CHECK(g_vc != CCORD_ID_NONE);
  ccord_voice_state gvs = {.guild_id = garden, .channel_id = g_vc, .user_id = 1004};
  CT_CHECK(ccord_store_set_voice_state(F.store, &gvs, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_set_voice_mute(&creq, garden, 1004, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_set_voice_deaf(&creq, garden, 1004, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_move_voice_member(&creq, garden, 1004, CCORD_ID_NONE, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &creq, &err) == CCORD_ERR_PERMISSION);
}

static int menu_builtin_index(menu_builtin builtin)
{
  for (size_t i = 0; i < F.ui->menu.count; i++) {
    if (F.ui->menu.items[i].builtin == builtin) return (int)i;
  }
  return -1;
}

static bool media_url_requested(const char *url)
{
  return ui_media_requested(F.ui, url);
}

// M6: animated emoji/stickers follow the account playback settings, and the built-in Copy ID rows follow Developer
// Mode. Off/never pins frame 0; on/always plays; hover modes play the hovered item only.
static void test_m6_devmode_copy_ids_and_animation_settings(void)
{
  ccord_error err = {0};
  const ccord_id hq = 2001, ada = 1002;
  const ccord_id hq_chan = find_channel(hq, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(hq_chan != CCORD_ID_NONE);
  ui_menu_close(F.ui);

  // No settings object yet: developer mode is off, so the built-in Copy ID rows are absent (the test plugin's
  // "Copy Message ID" contribution is not a built-in and stays).
  open_menu(CCORD_MENU_MEMBER, hq, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);
  open_menu(CCORD_MENU_MESSAGE, hq, hq_chan, ada, 5);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);
  open_menu(CCORD_MENU_CHANNEL, hq, hq_chan, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);
  open_menu(CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);

  // An explicit Developer Mode off object still hides them: having account settings is not the gate.
  seed_user_settings(false, true, 0);
  open_menu(CCORD_MENU_MEMBER, hq, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);
  open_menu(CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);
  open_menu(CCORD_MENU_MESSAGE, hq, hq_chan, ada, 5);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);
  open_menu(CCORD_MENU_CHANNEL, hq, hq_chan, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_builtin_index(MENU_BUILTIN_COPY_ID) < 0);

  // Developer mode on: each built-in copies exactly its own snowflake.
  seed_user_settings(true, true, 0);
  char want[24] = {0};
  F.clip[0] = '\0';
  open_menu(CCORD_MENU_MEMBER, hq, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  int idx = menu_builtin_index(MENU_BUILTIN_COPY_ID);
  CT_CHECK(idx >= 0 && F.ui->menu.items[idx].label &&
           strcmp(F.ui->menu.items[idx].label, "Copy User ID") == 0);
  if (idx >= 0) menu_click_index(idx);
  frame(1);
  ccord_id_format(ada, want);
  CT_CHECK(strcmp(F.clip, want) == 0);

  // The DM member menu has its own Copy User ID row, behind the same gate.
  F.clip[0] = '\0';
  open_menu(CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  idx = menu_builtin_index(MENU_BUILTIN_COPY_ID);
  CT_CHECK(idx >= 0 && F.ui->menu.items[idx].label &&
           strcmp(F.ui->menu.items[idx].label, "Copy User ID") == 0);
  if (idx >= 0) menu_click_index(idx);
  frame(1);
  CT_CHECK(strcmp(F.clip, want) == 0);

  F.clip[0] = '\0';
  open_menu(CCORD_MENU_MESSAGE, hq, hq_chan, ada, 5);
  idx = menu_builtin_index(MENU_BUILTIN_COPY_ID);
  CT_CHECK(idx >= 0 && F.ui->menu.items[idx].label &&
           strcmp(F.ui->menu.items[idx].label, "Copy Message ID") == 0);
  if (idx >= 0) menu_click_index(idx);
  frame(1);
  ccord_id_format(5, want);
  CT_CHECK(strcmp(F.clip, want) == 0);

  F.clip[0] = '\0';
  open_menu(CCORD_MENU_CHANNEL, hq, hq_chan, CCORD_ID_NONE, CCORD_ID_NONE);
  idx = menu_builtin_index(MENU_BUILTIN_COPY_ID);
  CT_CHECK(idx >= 0 && F.ui->menu.items[idx].label &&
           strcmp(F.ui->menu.items[idx].label, "Copy Channel ID") == 0);
  if (idx >= 0) menu_click_index(idx);
  frame(1);
  ccord_id_format(hq_chan, want);
  CT_CHECK(strcmp(F.clip, want) == 0);

  // A minimally valid 3-frame GIF, the same bytes the renderer animation tests use.
  static const uint8_t k_m6_anim_gif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x08,
    0x05, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x15,
    0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1,
    0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08,
    0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08,
    0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00,
    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c,
    0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03,
    0x02, 0x00, 0x3b,
  };
  ccord_rect far = {1000.0f, 700.0f, 32.0f, 32.0f};
  F.ui->mouse_in = false;

  // Animated custom emoji use the .gif asset; static ones keep the .png one.
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 987654321u, "anim", 4, true, far);
  CT_CHECK(media_url_requested("https://cdn.discordapp.com/emojis/987654321.gif?size=48"));
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 987654322u, "still", 5, false, far);
  CT_CHECK(media_url_requested("https://cdn.discordapp.com/emojis/987654322.png?size=48"));

  char emoji_gif[128];
  snprintf(emoji_gif, sizeof emoji_gif, "https://cdn.discordapp.com/emojis/%llu.gif?size=48",
           (unsigned long long)5004u);
  CT_CHECK(ccord_render_upload_image(F.renderer, emoji_gif, k_m6_anim_gif, sizeof k_m6_anim_gif, nullptr, nullptr,
                                     &err) == CCORD_OK);
  size_t cur = 99, count = 0;

  // animate_emoji off and not hovered: pinned to frame 0.
  seed_user_settings(false, false, 0);
  ccord_render_set_anim_clock(F.renderer, 1000);
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 5004u, "partyblob", 9, true, far);
  ccord_render_set_anim_clock(F.renderer, 2000);
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 5004u, "partyblob", 9, true, far);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, emoji_gif, &cur, &count));
  CT_CHECK(count == 3 && cur == 0);

  // animate_emoji on: it plays without hover.
  seed_user_settings(false, true, 0);
  ccord_render_set_anim_clock(F.renderer, 3000);
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 5004u, "partyblob", 9, true, far);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, emoji_gif, &cur, &count));
  CT_CHECK(cur >= 1);

  // animate_emoji off but hovered: Discord still plays it for the hovered item.
  seed_user_settings(false, false, 0);
  F.ui->mouse_in = true;
  F.ui->mx = far.x + far.w * 0.5f;
  F.ui->my = far.y + far.h * 0.5f;
  ccord_render_set_anim_clock(F.renderer, 4000);
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 5004u, "partyblob", 9, true, far);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, emoji_gif, &cur, &count));
  CT_CHECK(cur >= 1);
  F.ui->mouse_in = false;
  ccord_render_set_anim_clock(F.renderer, 5000);
  ui_expr_draw_custom_emoji(F.ui, F.renderer, 5004u, "partyblob", 9, true, far);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, emoji_gif, &cur, &count));
  CT_CHECK(cur == 0);

  // Sticker assets: GIF uses .gif, APNG keeps the .png URL.
  const ccord_id gif_sticker = 987654323u;
  char sticker_gif[160], sticker_png[160], apng_png[160];
  snprintf(sticker_gif, sizeof sticker_gif, "https://media.discordapp.net/stickers/%llu.gif?size=160",
           (unsigned long long)gif_sticker);
  snprintf(sticker_png, sizeof sticker_png, "https://media.discordapp.net/stickers/%llu.png?size=160",
           (unsigned long long)gif_sticker);
  snprintf(apng_png, sizeof apng_png, "https://media.discordapp.net/stickers/%llu.png?size=160",
           (unsigned long long)987654324u);
  ui_expr_draw_sticker_art(F.ui, F.renderer, gif_sticker, "g", 1, CCORD_STICKER_GIF, far, false);
  ui_expr_draw_sticker_art(F.ui, F.renderer, 987654324u, "a", 1, CCORD_STICKER_APNG, far, false);
  CT_CHECK(media_url_requested(sticker_gif));
  CT_CHECK(media_url_requested(apng_png));
  CT_CHECK(!media_url_requested(sticker_png));
  CT_CHECK(ccord_render_upload_image(F.renderer, sticker_gif, k_m6_anim_gif, sizeof k_m6_anim_gif, nullptr, nullptr,
                                     &err) == CCORD_OK);

  // animate_stickers = 2 (never): pinned even while hovered.
  seed_user_settings(false, true, 2);
  F.ui->mouse_in = false;
  ccord_render_set_anim_clock(F.renderer, 6000);
  ui_expr_draw_sticker_art(F.ui, F.renderer, gif_sticker, "g", 1, CCORD_STICKER_GIF, far, false);
  F.ui->mouse_in = true;
  F.ui->mx = far.x + far.w * 0.5f;
  F.ui->my = far.y + far.h * 0.5f;
  ccord_render_set_anim_clock(F.renderer, 7000);
  ui_expr_draw_sticker_art(F.ui, F.renderer, gif_sticker, "g", 1, CCORD_STICKER_GIF, far, false);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, sticker_gif, &cur, &count));
  CT_CHECK(cur == 0);

  // animate_stickers = 1 (hover only): still off the item, playing over it.
  seed_user_settings(false, true, 1);
  F.ui->mouse_in = false;
  ccord_render_set_anim_clock(F.renderer, 8000);
  ui_expr_draw_sticker_art(F.ui, F.renderer, gif_sticker, "g", 1, CCORD_STICKER_GIF, far, false);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, sticker_gif, &cur, &count));
  CT_CHECK(cur == 0);
  F.ui->mouse_in = true;
  F.ui->mx = far.x + far.w * 0.5f;
  F.ui->my = far.y + far.h * 0.5f;
  ccord_render_set_anim_clock(F.renderer, 9000);
  ui_expr_draw_sticker_art(F.ui, F.renderer, gif_sticker, "g", 1, CCORD_STICKER_GIF, far, false);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, sticker_gif, &cur, &count));
  CT_CHECK(cur >= 1);

  // animate_stickers = 0 (always): plays without hover.
  seed_user_settings(false, true, 0);
  F.ui->mouse_in = false;
  ccord_render_set_anim_clock(F.renderer, 10000);
  ui_expr_draw_sticker_art(F.ui, F.renderer, gif_sticker, "g", 1, CCORD_STICKER_GIF, far, false);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, sticker_gif, &cur, &count));
  CT_CHECK(cur >= 1);

  // Leave the fixture on the defaults the later user-settings UI expects.
  seed_user_settings(false, true, 0);
}

// GIF picker fetch/retry + full category covers + 512-byte URLs, and the Emoji rail: server icons,
// independent scroll, pinned Default Unicode button. Runs last: seeds guilds up to the store cap.
static void test_expr_picker_fetch_and_emoji_rail(void)
{
  ccord_error err = {0};
  ui_menu_close(F.ui);
  const ccord_id g1 = 2001;
  ccord_id ch = find_channel(g1, "off-topic", CCORD_CHANNEL_TEXT);
  CT_CHECK(ch != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, ch, &err) == CCORD_OK);
  frame(2);
  ui_expr_picker_close(F.ui);

  // ---- 1. SETTINGS_PROTO is requested even when local frequent emojis exist (no one-shot latch) ----
  ccord_store_seed_emoji_frecency(F.store, "fire", 99);
  CT_CHECK(ccord_store_frequent_emoji_count(F.store) > 0);
  CT_CHECK(ccord_store_set_expr_blob(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr, nullptr, 0, false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_FAILED);
  ui_expr_picker_open(F.ui, 2, ch, CCORD_ID_NONE);   // 2 = Emoji tab
  draw_only();
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_PENDING);
  frame(3);
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_READY);

  // ---- 2. FAILED blobs retry after the >=2000ms cooldown, never per frame ----
  CT_CHECK(ccord_store_set_expr_blob(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr, nullptr, 0, false, &err) == CCORD_OK);
  draw_only();
  draw_only();
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_FAILED);
  F.now += 2100;
  draw_only();
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_PENDING);
  frame(3);
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_READY);
  // The GIF catalog blobs recover the same way.
  CT_CHECK(ccord_store_set_expr_blob(F.store, CCORD_EXPR_GIF_TRENDING, nullptr, nullptr, 0, false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_expr_blob(F.store, CCORD_EXPR_GIF_CATEGORIES, nullptr, nullptr, 0, false, &err) == CCORD_OK);
  F.now += 2100;
  draw_only();
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_GIF_TRENDING, nullptr) == CCORD_EXPR_BLOB_PENDING);
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_GIF_CATEGORIES, nullptr) == CCORD_EXPR_BLOB_PENDING);
  frame(3);
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_GIF_TRENDING, nullptr) == CCORD_EXPR_BLOB_READY);
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_GIF_CATEGORIES, nullptr) == CCORD_EXPR_BLOB_READY);

  // ---- 3. GIF hit cells hold full 512-byte ccord_gif_item URLs ----
  ui_expr_inspect ins;
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.hit_url_cap == CCORD_URL_MAX_BYTES + 1u && ins.hit_url_cap >= 513u);
  CT_CHECK(ins.hit_preview_url_cap == CCORD_URL_MAX_BYTES + 1u);
  ccord_gif_item lg = {0};
  snprintf(lg.url, sizeof lg.url, "https://tenor.com/view/long-url-gif-");
  size_t base = strlen(lg.url);
  memset(lg.url + base, 'x', 500 - base);
  lg.url[500] = '\0';
  snprintf(lg.src, sizeof lg.src, "%s", lg.url);
  lg.width = 320; lg.height = 200;
  CT_CHECK(ccord_store_toggle_favorite_gif(F.store, &lg));
  ui_expr_picker_close(F.ui);
  ui_expr_picker_open(F.ui, 0, ch, CCORD_ID_NONE);   // 0 = GIFs tab
  frame(2);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  ccord_rect fav_card = {0, 0, 0, 0};
  for (size_t i = 0; i < ins.nhits; i++) {
    ui_expr_hit_info hi;
    CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
    if (hi.kind == 4) { fav_card = hi.rect; break; }   // 4 = Favorites card
  }
  CT_CHECK(fav_card.w > 0.0f);
  click(fav_card.x + fav_card.w * 0.5f, fav_card.y + fav_card.h * 0.5f);
  frame(2);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.gif_subview == 1);
  bool long_url_intact = false;
  for (size_t i = 0; i < ins.nhits; i++) {
    ui_expr_hit_info hi;
    CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
    if (hi.kind == 2 && strlen(hi.url) == 500 && strcmp(hi.url, lg.url) == 0) long_url_intact = true;
  }
  CT_CHECK(long_url_intact);
  // Removing every favorite draws the Favorites empty state instead of tiles (no crash, no tile hits).
  while (ccord_store_favorite_gif_count(F.store) > 0) {
    const ccord_gif_item *f0 = ccord_store_favorite_gif_at(F.store, 0);
    CT_CHECK(f0 != nullptr);
    ccord_gif_item copy = *f0;
    (void)ccord_store_toggle_favorite_gif(F.store, &copy);   // returns false when removing
  }
  frame(2);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  size_t tile_hits = 0;
  for (size_t i = 0; i < ins.nhits; i++) {
    ui_expr_hit_info hi;
    CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
    if (hi.kind == 2) tile_hits++;
  }
  CT_CHECK(tile_hits == 0);

  // ---- 4. Every GIF category renders with its cover art (no 4-tile cap) ----
  ui_expr_picker_close(F.ui);
  ui_expr_picker_open(F.ui, 0, ch, CCORD_ID_NONE);
  frame(2);
  shot("picker_gifs_categories");
  size_t ncat = ccord_store_gif_category_count(F.store);
  CT_CHECK(ncat >= 8);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  size_t cat_hits = 0;
  for (size_t i = 0; i < ins.nhits; i++) {
    ui_expr_hit_info hi;
    CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
    if (hi.kind == 3) cat_hits++;   // 3 = GIF category tile
  }
  CT_CHECK(cat_hits == ncat);
  // Every category cover went through ui_ensure_media: after pumping, each has a texture. (The mock's
  // disk media cache survives runs, so a texture check is used instead of the request log.)
  ui_pump_media(F.ui, F.renderer);
  frame(2);
  ui_pump_media(F.ui, F.renderer);
  for (size_t i = 0; i < ncat; i++) {
    const ccord_gif_category *cat = ccord_store_gif_category_at(F.store, i);
    CT_CHECK(cat && cat->src[0]);
    CT_CHECK(ccord_render_has_texture(F.renderer, cat->src, nullptr, nullptr));
  }

  // ---- 5. Emoji rail: server icons, >=256 capacity, independent scroll, pinned Unicode button ----
  // Fill the store to its guild cap; each new guild gets one custom emoji so it earns a rail button.
  // The last two guilds are special: one has a unique icon URL the nav rail never drew (it culls
  // off-screen slots), one has no icon at all and must fall back to initials.
  size_t ng0 = ccord_store_guild_count(F.store);
  CT_CHECK(ng0 < CCORD_MAX_REAL_GUILDS);
  F.ui->server_scroll = 0.0f;      // nav rail at the top: the new guilds stay off-screen there
  F.ui->server_scroll_vel = 0.0f;
  ccord_id icon_gid = 0, plain_gid = 0;
  char namebuf[40];
  for (size_t i = ng0; i < CCORD_MAX_REAL_GUILDS; i++) {
    ccord_id gid = 900000 + (ccord_id)i;
    ccord_guild ng = {0};
    ng.id = gid;
    snprintf(namebuf, sizeof namebuf, "Rail Guild %zu", i);
    ng.name = namebuf;
    ng.icon_url = "";
    if (i == CCORD_MAX_REAL_GUILDS - 2) {
      icon_gid = gid;
      ng.icon_url = "https://cdn.discordapp.com/icons/999001/exprail.png";
    } else if (i == CCORD_MAX_REAL_GUILDS - 1) {
      plain_gid = gid;
    }
    CT_CHECK(ccord_store_upsert_guild(F.store, &ng, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
    ccord_emoji ne = {0};
    ne.id = 950000 + (ccord_id)i;
    ne.guild_id = gid;
    snprintf(namebuf, sizeof namebuf, "rail%zu", i);
    ne.name = namebuf;
    ne.available = true;
    CT_CHECK(ccord_store_set_guild_emojis(F.store, gid, &ne, 1, &err) == CCORD_OK);
  }
  CT_CHECK(icon_gid != 0 && plain_gid != 0);
  CT_CHECK(!ccord_render_has_texture(F.renderer, "https://cdn.discordapp.com/icons/999001/exprail.png", nullptr, nullptr));

  ui_expr_picker_close(F.ui);
  ui_expr_picker_open(F.ui, 2, ch, CCORD_ID_NONE);
  frame(2);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.tab == 2);
  CT_CHECK(ins.rail_cap >= 256);
  CT_CHECK(ins.nrail > 200);                          // the old 24 cap would clamp this
  CT_CHECK(ins.rail_view.h > 0.0f && ins.rail_view.h < ins.rail_rect.h);
  CT_CHECK(fabsf((ins.rail_rect.h - ins.rail_view.h) - 36.0f) < 0.01f);
  CT_CHECK(ins.rail_content_h > ins.rail_view.h);
  CT_CHECK(ins.rail_pinned_rect.w == 28.0f && ins.rail_pinned_rect.h == 28.0f);
  CT_CHECK(ins.rail_pinned_rect.y >= ins.rail_view.y + ins.rail_view.h);
  CT_CHECK(ins.rail_pinned_rect.y + 28.0f <= ins.rail_rect.y + ins.rail_rect.h + 0.01f);

  size_t unicode_idx = (size_t)-1, icon_idx = (size_t)-1, plain_idx = (size_t)-1;
  for (size_t i = 0; i < ins.nrail; i++) {
    ui_expr_rail_info ri;
    CT_CHECK(ui_expr_rail_at(F.ui, i, &ri));
    if (ri.section_id == 0 && unicode_idx == (size_t)-1) unicode_idx = i;
    if (ri.guild_id == icon_gid) icon_idx = i;
    if (ri.guild_id == plain_gid) plain_idx = i;
  }
  CT_CHECK(unicode_idx != (size_t)-1 && icon_idx != (size_t)-1 && plain_idx != (size_t)-1);

  // Wheel over the 40px rail scrolls ONLY the rail; the emoji grid stays put.
  ui_expr_rail_info icon_ri;
  CT_CHECK(ui_expr_rail_at(F.ui, icon_idx, &icon_ri));
  float grid_before = ins.grid_scroll_y;
  float need = icon_ri.rect.y - ins.rail_view.y - 4.0f;   // rail scroll that brings the icon button into view
  CT_CHECK(need > 0.0f);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -need / 105.0f,
                           .x = ins.rail_rect.x + 20.0f, .y = ins.rail_view.y + 40.0f});
  draw_only();
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(fabsf(ins.rail_scroll_y - need) < 1.0f);
  CT_CHECK(ins.grid_scroll_y == grid_before);
  CT_CHECK(ui_expr_rail_at(F.ui, icon_idx, &icon_ri));
  CT_CHECK(icon_ri.rect.y >= ins.rail_view.y && icon_ri.rect.y + 28.0f <= ins.rail_view.y + ins.rail_view.h);
  // The picker pulled the server icon through ui_ensure_media (initials fallback until bytes arrive)...
  frame(2);
  ui_pump_media(F.ui, F.renderer);
  frame(1);
  CT_CHECK(ccord_render_has_texture(F.renderer, "https://cdn.discordapp.com/icons/999001/exprail.png", nullptr, nullptr));
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ui_expr_rail_at(F.ui, icon_idx, &icon_ri));
  size_t icons_drawn_before = F.ui->dbg_expr_rail_icons_drawn;
  // ...and draws it once a real texture is uploaded.
  static const uint8_t k_rail_gif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x08,
    0x05, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x15,
    0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1,
    0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08,
    0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08,
    0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00,
    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c,
    0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03,
    0x02, 0x00, 0x3b,
  };
  CT_CHECK(ccord_render_upload_image(F.renderer, "https://cdn.discordapp.com/icons/999001/exprail.png",
                                     k_rail_gif, sizeof k_rail_gif, nullptr, nullptr, &err) == CCORD_OK);
  draw_only();
  CT_CHECK(F.ui->dbg_expr_rail_icons_drawn > icons_drawn_before);
  // The icon-less guild button is visible next to it (initials fallback, no texture request).
  ui_expr_rail_info plain_ri;
  CT_CHECK(ui_expr_rail_at(F.ui, plain_idx, &plain_ri));
  CT_CHECK(plain_ri.rect.y >= ins.rail_view.y - 28.0f && plain_ri.rect.y <= ins.rail_view.y + ins.rail_view.h);
  shot("picker_emoji_rail_icons");

  // No ghost clicks: item 0 scrolled far out of the view; clicking the visible icon button jumps the
  // grid to THAT section, not to item 0's target.
  ui_expr_rail_info first_ri;
  CT_CHECK(ui_expr_rail_at(F.ui, 0, &first_ri));
  CT_CHECK(first_ri.rect.y + first_ri.rect.h < ins.rail_view.y);
  click(icon_ri.rect.x + 14.0f, icon_ri.rect.y + 14.0f);
  frame(1);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.grid_scroll_y == icon_ri.target_scroll_y && ins.grid_scroll_y > 0.0f);

  // Wheel over the grid scrolls ONLY the grid; the rail stays put.
  float rail_before = ins.rail_scroll_y;
  ccord_rect pbox = F.ui->emoji_picker_rect;
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 3.0f,
                           .x = pbox.x + 150.0f, .y = pbox.y + 200.0f});
  draw_only();
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.grid_scroll_y < icon_ri.target_scroll_y);
  CT_CHECK(ins.rail_scroll_y == rail_before);

  // ---- 6. Pinned Default Unicode button: visible during search, clears it and jumps to Unicode ----
  ccord_rect pinned = ins.rail_pinned_rect;
  type_text("zz");
  frame(2);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.search_len == 2 && ins.nrail == 0);
  CT_CHECK(ins.rail_pinned_rect.x == pinned.x && ins.rail_pinned_rect.y == pinned.y &&
           ins.rail_pinned_rect.w == 28.0f && ins.rail_pinned_rect.h == 28.0f);
  // P1 regression: reduced motion snaps every fade, so no animation schedules a follow-up frame.
  // The jump resolves mid-draw (after the grid was drawn at the old scroll) and must mark the UI
  // dirty itself. Raw events + a hand-run app loop: click() would force frames and mask a stall.
  for (int i = 0; i < 128; i++) ui_pump_media(F.ui, F.renderer);   // drain uploads so media can't mask dirty
  bool rm_setting = F.ui->settings_reduced_motion, rm_anim = F.ui->anim.reduced_motion;
  F.ui->settings_reduced_motion = true;
  F.ui->anim.reduced_motion = true;
  mouse(CCORD_IN_MOUSE_MOVE, 0, pinned.x + 14.0f, pinned.y + 14.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, pinned.x + 14.0f, pinned.y + 14.0f);
  mouse(CCORD_IN_MOUSE_UP, 1, pinned.x + 14.0f, pinned.y + 14.0f);
  F.now += 50;
  CT_CHECK(ccord_ui_update(F.ui, F.now));   // the click asked for a redraw
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);          // this draw resolves the pending Unicode jump
  ccord_render_end(F.renderer);
  CT_CHECK(F.ui->dirty);                    // P1: the jump must schedule its own follow-up frame
  F.now += 50;
  CT_CHECK(ccord_ui_update(F.ui, F.now));   // ...and the app loop honors it
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_render_end(F.renderer);
  F.ui->settings_reduced_motion = rm_setting;
  F.ui->anim.reduced_motion = rm_anim;
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.search_len == 0);
  ui_expr_rail_info uni_ri;
  CT_CHECK(ui_expr_rail_at(F.ui, unicode_idx, &uni_ri));
  CT_CHECK(ins.grid_scroll_y == uni_ri.target_scroll_y && ins.grid_scroll_y > 0.0f);
  // The Unicode category button was brought into the rail view as well.
  CT_CHECK(uni_ri.rect.y >= ins.rail_view.y && uni_ri.rect.y + 28.0f <= ins.rail_view.y + ins.rail_view.h);
  shot("picker_emoji_pinned");

  // ---- 7. After the jump, visible Unicode emojis own the hit slots (not 160 off-screen cells) ----
  CT_CHECK(ins.grid_view.w > 0.0f && ins.grid_view.h > 0.0f);
  CT_CHECK(ins.nhits > 0);
  size_t uni_visible = 0;
  ccord_rect uni_cell = {0, 0, 0, 0};
  char uni_utf8[32] = {0};
  for (size_t i = 0; i < ins.nhits; i++) {
    ui_expr_hit_info hi;
    CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
    // Every recorded hit intersects the grid viewport: nothing off-screen earns a slot.
    CT_CHECK(hi.rect.y + hi.rect.h >= ins.grid_view.y && hi.rect.y <= ins.grid_view.y + ins.grid_view.h);
    CT_CHECK(hi.rect.x >= ins.grid_view.x &&
             hi.rect.x + hi.rect.w <= ins.grid_view.x + ins.grid_view.w + 0.01f);
    if (hi.kind == 0 && hi.rect.y >= ins.grid_view.y &&
        hi.rect.y + hi.rect.h <= ins.grid_view.y + ins.grid_view.h) {
      uni_visible++;
      if (uni_cell.w == 0.0f) {
        uni_cell = hi.rect;
        snprintf(uni_utf8, sizeof uni_utf8, "%s", hi.utf8);
      }
    }
  }
  CT_CHECK(uni_visible >= 8);
  CT_CHECK(uni_utf8[0]);
  // Clicking a visible Unicode emoji inserts exactly it and closes the picker.
  size_t draft_len_before = strlen(ccord_text_edit_cstr(&F.ui->edit));
  click(uni_cell.x + uni_cell.w * 0.5f, uni_cell.y + uni_cell.h * 0.5f);
  CT_CHECK(!F.ui->emoji_picker_open);
  const char *draft_after = ccord_text_edit_cstr(&F.ui->edit);
  CT_CHECK(strlen(draft_after) > draft_len_before);
  CT_CHECK(strstr(draft_after, uni_utf8) != nullptr);

  // ---- 8. No ghost clicks through the preview bar onto partially visible cells ----
  ccord_text_edit_clear(&F.ui->edit);
  ui_expr_picker_open(F.ui, 2, ch, CCORD_ID_NONE);
  frame(2);   // grid scroll persists across close/open: still at the Unicode section
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.grid_scroll_y == uni_ri.target_scroll_y);
  // Find a cell straddling the grid's bottom edge; nudge the scroll until one exists.
  ccord_rect straddle = {0, 0, 0, 0};
  for (int attempt = 0; attempt < 12 && straddle.w == 0.0f; attempt++) {
    if (attempt > 0) {
      send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -0.1f,
                               .x = ins.grid_view.x + 100.0f, .y = ins.grid_view.y + 100.0f});
      draw_only();
      CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
    }
    for (size_t i = 0; i < ins.nhits; i++) {
      ui_expr_hit_info hi;
      CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
      if (hi.kind == 0 && hi.rect.y < ins.grid_view.y + ins.grid_view.h &&
          hi.rect.y + hi.rect.h > ins.grid_view.y + ins.grid_view.h) {
        straddle = hi.rect;
        break;
      }
    }
  }
  CT_CHECK(straddle.w > 0.0f);
  float grid_bottom = ins.grid_view.y + ins.grid_view.h;
  float overhang = straddle.y + straddle.h - grid_bottom;
  float ghost_y = grid_bottom + fminf(6.0f, overhang * 0.5f);   // inside the preview bar AND the cell rect
  mouse(CCORD_IN_MOUSE_MOVE, 0, straddle.x + 21.0f, ghost_y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, straddle.x + 21.0f, ghost_y);
  mouse(CCORD_IN_MOUSE_UP, 1, straddle.x + 21.0f, ghost_y);
  CT_CHECK(F.ui->emoji_picker_open);                            // the clipped part is not clickable
  CT_CHECK(ccord_text_edit_cstr(&F.ui->edit)[0] == '\0');
  // The visible part of the same cell is still live.
  click(straddle.x + 21.0f, straddle.y + fminf(20.0f, (grid_bottom - straddle.y) * 0.5f));
  CT_CHECK(!F.ui->emoji_picker_open);
  CT_CHECK(ccord_text_edit_cstr(&F.ui->edit)[0] != '\0');
  frame(1);

  // ---- 9. Search caret/selection, grid scrollbar thumb, and width-adaptive Emoji columns ----
  ui_expr_picker_open(F.ui, 2, ch, CCORD_ID_NONE);
  frame(2);
  F.ui->caret_epoch_ms = F.now;   // caret visible phase
  type_text("fi");
  draw_only();
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.search_len == 2);
  CT_CHECK(ins.search_caret_rect.w > 0.0f && ins.search_caret_rect.h > 0.0f);
  CT_CHECK(ins.search_caret_rect.x >= ins.search_rect.x &&
           ins.search_caret_rect.x + ins.search_caret_rect.w <= ins.search_rect.x + ins.search_rect.w + 0.01f);
  CT_CHECK(ins.search_caret_rect.y >= ins.search_rect.y &&
           ins.search_caret_rect.y + ins.search_caret_rect.h <= ins.search_rect.y + ins.search_rect.h + 0.01f);
  key(CCORD_KEY_LEFT, CCORD_MOD_SHIFT);
  draw_only();
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.search_selection_rect.w > 0.0f);
  CT_CHECK(ins.search_selection_rect.x >= ins.search_rect.x &&
           ins.search_selection_rect.x + ins.search_selection_rect.w <= ins.search_rect.x + ins.search_rect.w + 0.01f);
  // Clear the query: the full Emoji grid overflows and shows its thumb at the right edge of grid_view.
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.search_len == 0);
  CT_CHECK(ins.grid_content_h > ins.grid_view.h);
  CT_CHECK(ins.grid_scrollbar_rect.w == 4.0f && ins.grid_scrollbar_rect.h > 0.0f);
  CT_CHECK(ins.grid_scrollbar_rect.x >= ins.grid_view.x &&
           ins.grid_scrollbar_rect.x + ins.grid_scrollbar_rect.w <= ins.grid_view.x + ins.grid_view.w + 0.01f);
  CT_CHECK(ins.grid_scrollbar_rect.y >= ins.grid_view.y &&
           ins.grid_scrollbar_rect.y + ins.grid_scrollbar_rect.h <= ins.grid_view.y + ins.grid_view.h + 0.01f);

  // A 280 px window narrows the picker: fewer than 8 columns, and every recorded cell stays inside grid_view.
  send((ccord_input_event){.type = CCORD_IN_RESIZE, .x = 280.0f, .y = 800.0f});
  frame(2);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.grid_view.w > 0.0f);
  float col_x[16];
  size_t ncol = 0, cells = 0;
  for (size_t i = 0; i < ins.nhits; i++) {
    ui_expr_hit_info hi;
    CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
    if (hi.kind != 0 && hi.kind != 1) continue;
    cells++;
    CT_CHECK(hi.rect.x >= ins.grid_view.x - 0.01f &&
             hi.rect.x + hi.rect.w <= ins.grid_view.x + ins.grid_view.w + 0.01f);
    bool seen = false;
    for (size_t c = 0; c < ncol; c++) {
      if (fabsf(col_x[c] - hi.rect.x) < 0.5f) seen = true;
    }
    if (!seen && ncol < 16) col_x[ncol++] = hi.rect.x;
  }
  CT_CHECK(cells > 0);
  CT_CHECK(ncol >= 4 && ncol < 8);
  send((ccord_input_event){.type = CCORD_IN_RESIZE, .x = 1280.0f, .y = 800.0f});
  frame(2);
  ui_expr_picker_close(F.ui);
  frame(1);
}

// ---- Regressions: search timeline, scrollable Friends/stage, DM call bar inset/cursor ----

static ui_voice_surfaces_debug vsdbg(void)
{
  ui_voice_surfaces_debug d;
  ui_voice_surfaces_get_debug(F.ui, &d);
  return d;
}

// An active search keeps every channel message in the timeline while the results panel populates.
static void test_search_preserves_timeline(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_session_send_message(F.session, 3111, "quokka-unique-search-token", 26, &err) == CCORD_OK);
  frame(4);
  size_t total = ccord_store_message_count(F.store, 3111);

  click_rect(dbg().search_btn);
  frame(2);
  CT_CHECK(dbg().search_open);
  type_text("quokka-unique-search-token");
  key(CCORD_KEY_RETURN, 0);
  frame(6);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.message_count == total);          // the timeline is not filtered away
  CT_CHECK(F.ui->view_count == total);
  CT_CHECK(d.search_panel_open && d.search_panel_cards > 0);
  CT_CHECK(d.search_matches >= 1 && d.search_matches < total);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!dbg().search_open);
  CT_CHECK(dbg().message_count == total);
}

// The DM Friends list scrolls under its fixed header with a fading thumb; off-fold rows stay reachable.
static void test_dm_friends_scroll_thumb(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  frame(2);
  const size_t NF = 40;
  for (size_t i = 0; i < NF; i++) {
    ccord_user u = {.id = 60000u + (ccord_id)i, .name = "ScrollFriend", .presence = CCORD_PRESENCE_ONLINE};
    CT_CHECK(ccord_store_upsert_user(F.store, &u, &err) == CCORD_OK);
    CT_CHECK(ccord_store_set_relationship(F.store, 60000u + (ccord_id)i, CCORD_RELATIONSHIP_FRIEND, &err) == CCORD_OK);
  }
  F.ui->dm_friends_view = true;
  F.ui->dm_friends_tab = 1;   // All Friends
  F.ui->dm_friends_scroll = 0.0f;
  F.ui->dirty = true;
  frame(2);
  CT_CHECK(ui_is_dm_friends_view(F.ui));
  ccord_rect ma = ui_rect(F.ui->layout.message_area);
  CT_CHECK(F.ui->dm_friends_content_h > ma.h);   // enough rows to overflow the fold
  CT_CHECK(F.ui->dm_friends_scroll == 0.0f);

  float wx = ma.x + ma.w * 0.5f, wy = ma.y + 100.0f;
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -5.0f, .x = wx, .y = wy});
  frame(3);   // let the 160ms thumb fade ramp in
  CT_CHECK(F.ui->dm_friends_scroll > 0.0f);
  CT_CHECK(F.ui->dm_friends_thumb.w > 0.0f);     // thumb appears right after scrolling
  CT_CHECK(F.ui->dm_friends_thumb.y >= ma.y &&
           F.ui->dm_friends_thumb.y + F.ui->dm_friends_thumb.h <= ma.y + ma.h + 1.0f);

  // Scroll to the bottom: the last friend becomes fully visible and clickable.
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = wx, .y = wy});
  frame(1);
  bool found_last = false;
  for (size_t i = 0; i < F.ui->nfriend_row_hits; i++) {
    if (F.ui->friend_row_hits[i].user_id == 60000u + NF - 1) found_last = true;
  }
  CT_CHECK(found_last);

  F.ui->dm_friends_view = false;
  F.ui->dirty = true;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(2);
}

// Voice/stage participant tiles wrap and scroll as one viewport; off-fold tiles become reachable.
static void test_stage_tiles_scroll_thumb(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(2);
  ccord_id voice = find_channel(2001, "General", CCORD_CHANNEL_VOICE);
  CT_CHECK(voice != CCORD_ID_NONE);
  CT_CHECK(ccord_session_join_voice(F.session, 2001, voice, &err) == CCORD_OK);
  frame(3);
  F.ui->stage_channel = voice;
  F.ui->stage_scroll = 0.0f;
  F.ui->dirty = true;
  frame(2);
  const size_t NU = 18;
  for (size_t i = 0; i < NU; i++) {
    ccord_id uid = 61000u + (ccord_id)i;
    ccord_user u = {.id = uid, .name = "StageUser", .presence = CCORD_PRESENCE_ONLINE};
    CT_CHECK(ccord_store_upsert_user(F.store, &u, &err) == CCORD_OK);
    ccord_voice_state vs = {.guild_id = 2001, .channel_id = voice, .user_id = uid};
    CT_CHECK(ccord_store_set_voice_state(F.store, &vs, &err) == CCORD_OK);
  }
  frame(2);
  CT_CHECK(F.ui->stage_content_h > F.ui->stage_view_h);   // the tile grid overflows the fold
  ccord_id last = 61000u + NU - 1;
  ui_voice_surfaces_debug d = vsdbg();
  bool last_before = false;
  for (size_t i = 0; i < d.stage.ctl_count; i++) if (d.stage.ctl[i].user_id == last) last_before = true;
  CT_CHECK(!last_before);   // the bottom tile starts off-fold

  ccord_rect ma = ui_rect(F.ui->layout.message_area);
  float wx = ma.x + ma.w * 0.5f, wy = ma.y + ma.h * 0.7f;
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -5.0f, .x = wx, .y = wy});
  frame(3);   // let the 160ms thumb fade ramp in
  CT_CHECK(F.ui->stage_scroll > 0.0f);
  CT_CHECK(F.ui->stage_thumb.w > 0.0f);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = wx, .y = wy});
  frame(1);
  d = vsdbg();
  bool last_after = false;
  for (size_t i = 0; i < d.stage.ctl_count; i++) if (d.stage.ctl[i].user_id == last) last_after = true;
  CT_CHECK(last_after);   // scrolled into view: its controls are now exposed

  CT_CHECK(ccord_session_leave_voice(F.session, 2001, &err) == CCORD_OK);
  frame(3);
  F.ui->stage_channel = CCORD_ID_NONE;
  F.ui->dirty = true;
  frame(2);
}

// The active DM call bar reserves a 36px strip above the message area: the timeline starts below it,
// the pointer over the bar is never a text cursor, and pressing the bar never starts a chat selection.
static void test_call_bar_inset_and_cursor(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  frame(2);
  ccord_id call_ch = 0;
  CT_CHECK(ccord_mock_push_incoming_call(F.transport, 1002u, &call_ch, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(ccord_session_select_channel(F.session, call_ch, &err) == CCORD_OK);
  frame(2);
  ccord_rect ma_before = ui_rect(F.ui->layout.message_area);
  CT_CHECK(!ui_call_bar_active(F.ui));

  ui_voice_surfaces_debug d = vsdbg();
  CT_CHECK(d.ring_visible && d.ring_accept.w > 0.0f);
  click(d.ring_accept.x + d.ring_accept.w * 0.5f, d.ring_accept.y + d.ring_accept.h * 0.5f);
  frame(3);
  CT_CHECK(ui_call_bar_active(F.ui));
  ccord_rect ma = ui_rect(F.ui->layout.message_area);
  CT_CHECK(fabsf(ma.y - (ma_before.y + UI_CALL_BAR_H)) < 0.5f);
  CT_CHECK(fabsf(ma.h - (ma_before.h - UI_CALL_BAR_H)) < 0.5f);
  ccord_rect bar = ui_call_bar_rect(F.ui);
  CT_CHECK(bar.h == UI_CALL_BAR_H && bar.y + bar.h <= ma.y + 0.5f);

  // Over the bar the cursor is a button pointer, never the text I-beam.
  float bx = bar.x + 60.0f, by = bar.y + bar.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, bx, by);
  frame(1);
  CT_CHECK(F.ui->cursor != CCORD_CURSOR_TEXT);
  CT_CHECK(F.ui->cursor == CCORD_CURSOR_POINTER);

  // In the timeline below, the I-beam shows over a line of message text and nowhere else: not in the empty
  // space above the messages, and not in the gap to the right of where a line ends.
  ccord_id cur_mid = append_rich_message(call_ch, "cursor probe", nullptr);
  ccord_rect line = {0, 0, 0, 0};
  for (size_t i = 0; i < F.ui->ntext_hits; i++) {
    ccord_rect t = F.ui->text_hits[i];
    if (t.w > 20.0f && t.y > ma.y && t.y + t.h < ma.y + ma.h && t.y > line.y) line = t;   // the lowest line
  }
  CT_CHECK(F.ui->ntext_hits > 0 && line.w > 20.0f && line.w < ma.w * 0.5f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, line.x + line.w * 0.5f, line.y + line.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->cursor == CCORD_CURSOR_TEXT);
  mouse(CCORD_IN_MOUSE_MOVE, 0, line.x + line.w + 60.0f, line.y + line.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->cursor != CCORD_CURSOR_TEXT);
  mouse(CCORD_IN_MOUSE_MOVE, 0, ma.x + ma.w * 0.5f, ma.y + 20.0f);
  frame(1);
  CT_CHECK(F.ui->cursor == CCORD_CURSOR_DEFAULT);
  // Dragging a selection keeps the I-beam even where the pointer leaves the text.
  mouse(CCORD_IN_MOUSE_DOWN, 1, line.x + 2.0f, line.y + line.h * 0.5f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, line.x + line.w + 60.0f, line.y + line.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->drag == DRAG_CHAT_SELECT && F.ui->cursor == CCORD_CURSOR_TEXT);
  mouse(CCORD_IN_MOUSE_UP, 1, line.x + line.w + 60.0f, line.y + line.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->cursor != CCORD_CURSOR_TEXT);
  key(CCORD_KEY_ESCAPE, 0);
  remove_message(call_ch, cur_mid);
  frame(1);

  // Pressing on the bar must not begin a chat text selection.
  mouse(CCORD_IN_MOUSE_DOWN, 1, bx, by);
  CT_CHECK(F.ui->drag != DRAG_CHAT_SELECT);
  mouse(CCORD_IN_MOUSE_UP, 1, bx, by);
  frame(1);

  // Leaving removes the inset entirely.
  d = vsdbg();
  CT_CHECK(d.call_bar_visible && d.call_bar_leave.w > 0.0f);
  click(d.call_bar_leave.x + d.call_bar_leave.w * 0.5f, d.call_bar_leave.y + d.call_bar_leave.h * 0.5f);
  frame(3);
  CT_CHECK(!ui_call_bar_active(F.ui));
  CT_CHECK(fabsf(ui_rect(F.ui->layout.message_area).y - ma_before.y) < 0.5f);
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(2);
}

// ---- Result 1: messaging parity (Forward / Crosspost / Copy Link / Mark Unread / Save), /tts, Saved Messages
//      popover + persistence, slash subcommand trees, and USER/MESSAGE context-menu application commands. ----

// Open a context menu for a known context and click one labelled row; returns false if the row is absent.
static bool open_menu_and_click(const ccord_menu_context *ctx, const char *label)
{
  ui_menu_open(F.ui, 420.0f, 300.0f, ctx);
  frame(1);
  if (!F.ui->menu.open) return false;
  return menu_click_label(label);
}

static ccord_id send_confirmed(ccord_id ch, const char *text)
{
  clear_composer_input();
  type_text(text);
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  size_t n = ccord_store_message_count(F.store, ch);
  const ccord_message *m = n ? ccord_store_message_at(F.store, ch, n - 1) : nullptr;
  CT_CHECK(m && m->send_state == CCORD_MSG_SEND_CONFIRMED && strcmp(m->content, text) == 0);
  return m ? m->id : CCORD_ID_NONE;
}

static void test_copy_link_mark_unread_save(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  ccord_id self = ccord_store_self_id(F.store);
  ccord_id msg = send_confirmed(3111, "parity target message");
  CT_CHECK(msg != CCORD_ID_NONE);
  ccord_menu_context ctx = {.target = CCORD_MENU_MESSAGE, .guild_id = 2001, .channel_id = 3111,
                            .message_id = msg, .user_id = self};

  // Copy Message Link -> discord.com/channels/<guild>/<channel>/<message>
  F.clip[0] = '\0';
  CT_CHECK(open_menu_and_click(&ctx, "Copy Message Link"));
  frame(1);
  char want_link[128];
  snprintf(want_link, sizeof want_link, "https://discord.com/channels/2001/3111/%llu", (unsigned long long)msg);
  CT_CHECK(strcmp(F.clip, want_link) == 0);

  // Mark Unread moves the read position back; the channel reports unread again.
  size_t acks0 = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK);
  CT_CHECK(open_menu_and_click(&ctx, "Mark Unread"));
  frame(1);
  const ccord_channel *c = ccord_store_channel(F.store, 3111);
  CT_CHECK(c && c->unread_count >= 1 && c->last_read_id < msg);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK) == acks0 + 1);

  // Save Message bookmarks it; the menu then offers Unsave.
  CT_CHECK(!ui_saved_messages_contains(F.ui, 3111, msg));
  CT_CHECK(open_menu_and_click(&ctx, "Save Message"));
  frame(1);
  CT_CHECK(ui_saved_messages_contains(F.ui, 3111, msg));
  CT_CHECK(ccord_store_message_saved(F.store, 3111, msg));
  CT_CHECK(F.ui->nsaved_msgs == 1);
  ui_menu_open(F.ui, 420.0f, 300.0f, &ctx);
  frame(1);
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Unsave Message", nullptr));
  CT_CHECK(!ccord_ui_context_menu_find(F.ui, "Save Message", nullptr));
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(open_menu_and_click(&ctx, "Unsave Message"));
  frame(1);
  CT_CHECK(!ui_saved_messages_contains(F.ui, 3111, msg));
  CT_CHECK(!ccord_store_message_saved(F.store, 3111, msg));
  CT_CHECK(F.ui->nsaved_msgs == 0);
}

static void test_saved_messages_popover_and_persistence(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  ccord_id msg = send_confirmed(3111, "bookmark me");
  CT_CHECK(msg != CCORD_ID_NONE);
  ui_saved_messages_save(F.ui, 3111, msg);
  frame(1);
  CT_CHECK(F.ui->nsaved_msgs == 1);

  // Move to another channel, then open the header popover and jump back.
  ccord_id general = find_channel(2001, "general", CCORD_CHANNEL_TEXT);
  CT_CHECK(general != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, general, &err) == CCORD_OK);
  frame(3);
  ui_saved_msgs_debug sd;
  ui_saved_messages_get_debug(F.ui, &sd);
  CT_CHECK(!sd.open && sd.header_btn.w > 0.0f);
  click(sd.header_btn.x + sd.header_btn.w * 0.5f, sd.header_btn.y + sd.header_btn.h * 0.5f);
  frame(1);
  ui_saved_messages_get_debug(F.ui, &sd);
  CT_CHECK(sd.open && sd.count == 1 && sd.hit_count >= 1);
  CT_CHECK(sd.hits[0].channel_id == 3111 && sd.hits[0].message_id == msg);
  CT_CHECK(sd.popover_rect.w > 0.0f && sd.hits[0].jump.w > 0.0f);
  click(sd.hits[0].jump.x + sd.hits[0].jump.w * 0.5f, sd.hits[0].jump.y + sd.hits[0].jump.h * 0.5f);
  frame(3);
  CT_CHECK(ccord_store_selected_channel(F.store) == 3111);
  ui_saved_messages_get_debug(F.ui, &sd);
  CT_CHECK(!sd.open);

  // Remove from the popover clears the bookmark.
  ui_saved_messages_get_debug(F.ui, &sd);
  click(sd.header_btn.x + sd.header_btn.w * 0.5f, sd.header_btn.y + sd.header_btn.h * 0.5f);
  frame(1);
  ui_saved_messages_get_debug(F.ui, &sd);
  CT_CHECK(sd.open && sd.hit_count >= 1 && sd.hits[0].remove.w > 0.0f);
  click(sd.hits[0].remove.x + sd.hits[0].remove.w * 0.5f, sd.hits[0].remove.y + sd.hits[0].remove.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->nsaved_msgs == 0 && !ui_saved_messages_contains(F.ui, 3111, msg));
  key(CCORD_KEY_ESCAPE, 0);

  // Persistence: save -> settings.ini on disk -> reload restores the list and the store.
  ui_saved_messages_save(F.ui, 3111, msg);
  frame(1);
  ui_settings_save(F.ui);
  FILE *ini = fopen(ui_test_file("settings.ini"), "rb");
  CT_CHECK(ini != nullptr);
  if (ini) {
    char buf[8192];
    size_t got = fread(buf, 1, sizeof buf - 1, ini);
    buf[got] = '\0';
    fclose(ini);
    CT_CHECK(strstr(buf, "[saved_messages]") != nullptr);
    char idstr[32];
    snprintf(idstr, sizeof idstr, "%llu", (unsigned long long)msg);
    CT_CHECK(strstr(buf, idstr) != nullptr);
  }
  F.ui->nsaved_msgs = 0;   // simulate a fresh process: drop the in-memory list, then reload from disk
  ui_settings_init(F.ui);
  frame(1);
  CT_CHECK(F.ui->nsaved_msgs == 1 && ui_saved_messages_contains(F.ui, 3111, msg));
  CT_CHECK(ccord_store_message_saved(F.store, 3111, msg));
  ui_saved_messages_unsave(F.ui, 3111, msg);   // clean up for later tests
  ui_settings_save(F.ui);
  frame(1);
}

static void test_forward_picker(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  ccord_id self = ccord_store_self_id(F.store);
  ccord_id msg = send_confirmed(3111, "forward this note");
  CT_CHECK(msg != CCORD_ID_NONE);
  ccord_id dest = find_channel(2001, "off-topic", CCORD_CHANNEL_TEXT);
  CT_CHECK(dest != CCORD_ID_NONE);
  size_t dest_before = ccord_store_message_count(F.store, dest);

  ccord_menu_context ctx = {.target = CCORD_MENU_MESSAGE, .guild_id = 2001, .channel_id = 3111,
                            .message_id = msg, .user_id = self};
  CT_CHECK(open_menu_and_click(&ctx, "Forward"));
  frame(1);
  CT_CHECK(ui_forward_picker_active(F.ui));
  ui_forward_picker_debug fd;
  ui_forward_picker_get_debug(F.ui, &fd);
  CT_CHECK(fd.open && fd.source_channel == 3111 && fd.source_message == msg && fd.item_count > 0);

  // Filter to the destination channel and confirm with Enter.
  type_text("off-topic");
  frame(1);
  ui_forward_picker_get_debug(F.ui, &fd);
  CT_CHECK(fd.item_count >= 1);
  bool found = false;
  for (size_t i = 0; i < fd.item_count; i++) if (fd.items[i].channel_id == dest) found = true;
  CT_CHECK(found);
  key(CCORD_KEY_RETURN, 0);
  frame(5);
  CT_CHECK(!ui_forward_picker_active(F.ui));
  CT_CHECK(ccord_store_selected_channel(F.store) == dest);   // forward navigates to the target
  size_t dest_after = ccord_store_message_count(F.store, dest);
  CT_CHECK(dest_after == dest_before + 1);
  const ccord_message *fm = ccord_store_message_at(F.store, dest, dest_after - 1);
  CT_CHECK(fm && fm->rich && fm->rich->reference.kind == CCORD_REF_FORWARD &&
           fm->rich->reference.message_id == msg && fm->rich->forward.present);
  CT_CHECK(strcmp(fm->rich->forward.content, "forward this note") == 0);
}

static void test_tts_command_and_badge(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);

  // /tts is offered in the slash picker alongside the application commands.
  clear_composer_input();
  type_text("/");
  frame(3);
  CT_CHECK(autocomplete_has("/tts"));
  clear_composer_input();

  type_text("/tts speak this aloud");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  size_t n = ccord_store_message_count(F.store, 3111);
  const ccord_message *m = ccord_store_message_at(F.store, 3111, n - 1);
  CT_CHECK(m && m->tts && strcmp(m->content, "speak this aloud") == 0);
  CT_CHECK(m->author_id == ccord_store_self_id(F.store));
  frame(1);
  CT_CHECK(F.ui->dbg_tts_badges >= 1);   // the TTS badge is drawn next to the author

  // Empty /tts is refused and keeps the text (Esc dismisses the slash picker so Enter sends).
  clear_composer_input();
  type_text("/tts");
  key(CCORD_KEY_ESCAPE, 0);
  key(CCORD_KEY_RETURN, 0);
  frame(1);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "/tts") == 0);
  CT_CHECK(strstr(F.ui->notice, "needs a message") != nullptr);
  clear_composer_input();
}

static void test_crosspost_announcement(void)
{
  ccord_error err;
  ccord_id ann = find_channel(2001, "announcements", CCORD_CHANNEL_TEXT);
  CT_CHECK(ann != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, ann, &err) == CCORD_OK);
  frame(4);
  ccord_id self = ccord_store_self_id(F.store);
  ccord_id msg = send_confirmed(ann, "publish to followers");
  CT_CHECK(msg != CCORD_ID_NONE);
  ccord_menu_context ctx = {.target = CCORD_MENU_MESSAGE, .guild_id = 2001, .channel_id = ann,
                            .message_id = msg, .user_id = self};

  // Own announcement message offers Publish; after crossposting the flag arrives and the row disappears.
  ui_menu_open(F.ui, 420.0f, 300.0f, &ctx);
  frame(1);
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Publish Message", nullptr));
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(open_menu_and_click(&ctx, "Publish Message"));
  frame(4);
  const ccord_message *m = ccord_store_find_message(F.store, ann, msg);
  CT_CHECK(m && (ccord_message_flags(m) & CCORD_MSGFLAG_CROSSPOSTED) != 0u);
  ui_menu_open(F.ui, 420.0f, 300.0f, &ctx);
  frame(1);
  CT_CHECK(!ccord_ui_context_menu_find(F.ui, "Publish Message", nullptr));
  key(CCORD_KEY_ESCAPE, 0);

  // A normal (non-announcement) channel never offers Publish.
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(3);
  ccord_id plain = send_confirmed(3111, "not an announcement");
  ccord_menu_context pctx = {.target = CCORD_MENU_MESSAGE, .guild_id = 2001, .channel_id = 3111,
                             .message_id = plain, .user_id = self};
  ui_menu_open(F.ui, 420.0f, 300.0f, &pctx);
  frame(1);
  CT_CHECK(!ccord_ui_context_menu_find(F.ui, "Publish Message", nullptr));
  key(CCORD_KEY_ESCAPE, 0);
}

static void test_slash_subcommand_tree(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  (void)ccord_session_ensure_slash_commands(F.session, 2001, 3111, F.now);
  frame(6);

  // Subcommand / group names are offered while picking; USER/MESSAGE commands are not slash entries.
  clear_composer_input();
  type_text("/");
  frame(3);
  CT_CHECK(autocomplete_has("/config") && !autocomplete_has("UserInfo") && !autocomplete_has("QuoteMessage"));
  type_text("config ");
  frame(2);
  CT_CHECK(autocomplete_has("show") && autocomplete_has("theme"));
  type_text("theme ");
  frame(2);
  CT_CHECK(autocomplete_has("set") && autocomplete_has("reset"));
  clear_composer_input();

  // /config show -> ephemeral answer.
  size_t ic = ccord_mock_interaction_count(F.transport);
  type_text("/config show");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic + 1);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "config show") == 0);
  const ccord_message *m = ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1);
  CT_CHECK(m && ccord_message_is_ephemeral(m) && strcmp(m->content, "Config: theme=dark, compact=false") == 0);

  // /config theme set preset:dark -> nested group + subcommand with a named option.
  clear_composer_input();
  type_text("/config theme set preset:dark");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "config theme set") == 0);
  m = ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1);
  CT_CHECK(m && strcmp(m->content, "Config theme set: dark") == 0);

  // /config theme reset -> group + subcommand, no options.
  clear_composer_input();
  type_text("/config theme reset");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "config theme reset") == 0);
  m = ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1);
  CT_CHECK(m && strcmp(m->content, "Config theme reset to default") == 0);

  // A bare tree command without a subcommand is refused client-side.
  clear_composer_input();
  ic = ccord_mock_interaction_count(F.transport);
  type_text("/config");
  key(CCORD_KEY_ESCAPE, 0);
  key(CCORD_KEY_RETURN, 0);
  frame(1);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic);
  CT_CHECK(strstr(F.ui->notice, "requires a subcommand") != nullptr);
  clear_composer_input();
}

static void test_context_menu_app_commands(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  (void)ccord_session_ensure_slash_commands(F.session, 2001, 3111, F.now);
  frame(6);
  ccord_id self = ccord_store_self_id(F.store);
  ccord_id msg = send_confirmed(3111, "quote me please");
  CT_CHECK(msg != CCORD_ID_NONE);

  // MESSAGE (type 3) command in the message menu runs against the message.
  ccord_menu_context mctx = {.target = CCORD_MENU_MESSAGE, .guild_id = 2001, .channel_id = 3111,
                             .message_id = msg, .user_id = self};
  ui_menu_open(F.ui, 420.0f, 300.0f, &mctx);
  frame(1);
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "QuoteMessage", nullptr));
  key(CCORD_KEY_ESCAPE, 0);
  size_t ic = ccord_mock_interaction_count(F.transport);
  CT_CHECK(open_menu_and_click(&mctx, "QuoteMessage"));
  frame(4);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic + 1);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "QuoteMessage") == 0);
  char tbuf[32];
  snprintf(tbuf, sizeof tbuf, "%llu", (unsigned long long)msg);
  CT_CHECK(ccord_mock_last_interaction_value_count(F.transport) == 1 &&
           strcmp(ccord_mock_last_interaction_value(F.transport, 0), tbuf) == 0);
  const ccord_message *qm = ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1);
  CT_CHECK(qm && strncmp(qm->content, "Quote (", 7) == 0);

  // USER (type 2) command in the member menu runs against the member.
  ccord_menu_context uctx = {.target = CCORD_MENU_MEMBER, .guild_id = 2001, .channel_id = 3111, .user_id = 1002};
  ui_menu_open(F.ui, 420.0f, 300.0f, &uctx);
  frame(1);
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "UserInfo", nullptr));
  key(CCORD_KEY_ESCAPE, 0);
  ic = ccord_mock_interaction_count(F.transport);
  CT_CHECK(open_menu_and_click(&uctx, "UserInfo"));
  frame(4);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic + 1);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "UserInfo") == 0);
  CT_CHECK(ccord_mock_last_interaction_value_count(F.transport) == 1 &&
           strcmp(ccord_mock_last_interaction_value(F.transport, 0), "1002") == 0);
}

// Membership screening. Joining a guild with member verification (the mock's invite "mv-test" -> "Rules Lounge") leaves
// the self member pending: the rules form opens on its own and the composer shuts. Submit stays dead until the rules
// are agreed to, then goes to Discord, and only Discord's confirmation (the pending flag clearing) opens the composer.
static ccord_id join_rules_lounge(void)
{
  ccord_error err;
  // An earlier test fills the store to its server cap; make room for the guild the invite adds.
  while (ccord_store_guild_count(F.store) >= CCORD_MAX_REAL_GUILDS) {
    const ccord_guild *filler = ccord_store_guild_at(F.store, ccord_store_guild_count(F.store) - 1u);
    CT_CHECK(filler != nullptr && filler->id != 2001 && filler->id != CCORD_DM_GUILD_ID);
    if (!filler || filler->id == 2001 || filler->id == CCORD_DM_GUILD_ID) break;
    CT_CHECK(ccord_store_delete_guild(F.store, filler->id, &err) == CCORD_OK);
  }
  ccord_admin_request req;
  CT_CHECK(ccord_admin_accept_invite(&req, "mv-test", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &req, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, 9101) != nullptr);
  ccord_id general = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, 9101); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, 9101, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT) {
      general = c->id;
      break;
    }
  }
  CT_CHECK(general != CCORD_ID_NONE);
  return general;
}

static void test_membership_screening_modal_and_composer_lock(void)
{
  ccord_error err;
  ccord_transport *tr = F.transport;
  ccord_ui_set_viewport(F.ui, 1280.0f, 800.0f);
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr && F.ui->mv.composer_btn.w == 0.0f);

  // 1. Joining a guild with screening: the form opens on its own, is fetched, and the composer is locked.
  ccord_id general = join_rules_lounge();
  CT_CHECK(ui_self_pending(F.ui, 9101));
  CT_CHECK(ccord_session_select_guild(F.session, 9101, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, general, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION && F.ui->mv.guild_id == 9101);
  const ccord_member_verification *form = ccord_session_member_verification(F.session, 9101);
  CT_CHECK(form != nullptr && form->field_count >= 1 && form->fields[0].field_type == CCORD_MEMBER_VERIFICATION_TERMS);
  const char *why = ui_composer_lock_reason(F.ui);
  CT_CHECK(why != nullptr && strstr(why, "rules") != nullptr);
  CT_CHECK(F.ui->mv.box.w > 0.0f && F.ui->mv.checks[0].w > 0.0f && F.ui->mv.submit.w > 0.0f);
  CT_CHECK(!F.ui->mv.submit_enabled && !F.ui->mv.agreed[0]);

  // 2. Submit is shut until the box is ticked: clicking it sends nothing and keeps the form open.
  size_t base = ccord_mock_admin_count(tr);
  click_rect(F.ui->mv.submit);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION && ccord_mock_admin_count(tr) == base);
  CT_CHECK(ui_self_pending(F.ui, 9101));

  // 3. Ticking the box enables Submit; Submit asks Discord (PUT /guilds/{g}/requests/@me) and closes the form, but
  //    the composer stays shut until Discord confirms.
  click_rect(F.ui->mv.checks[0]);
  CT_CHECK(F.ui->mv.agreed[0] && F.ui->mv.submit_enabled);
  ccord_rect submit = F.ui->mv.submit;
  mouse(CCORD_IN_MOUSE_DOWN, 1, submit.x + submit.w * 0.5f, submit.y + submit.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 1, submit.x + submit.w * 0.5f, submit.y + submit.h * 0.5f);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);
  CT_CHECK(ccord_mock_admin_count(tr) == base + 1);
  const ccord_admin_request *last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION && last->guild_id == 9101);
  CT_CHECK(ui_composer_lock_reason(F.ui) != nullptr);   // nothing local changed before Discord answered
  frame(4);
  CT_CHECK(!ui_self_pending(F.ui, 9101) && ui_composer_lock_reason(F.ui) == nullptr);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE && F.ui->mv.composer_btn.w == 0.0f);
}

static void test_issue3_auto_ack_and_input_guards(void)
{
  ccord_error err;
  // 1. ui_rgb_color(0) returns opaque black, and ui_user_banner_url uses ?size=512.
  ccord_color black = ui_rgb_color(0);
  CT_CHECK(black.r == 0 && black.g == 0 && black.b == 0 && black.a == 0xFFu);

  ccord_user u_hash = {.id = 42, .banner_url = "abc123"};
  char bbuf[256];
  CT_CHECK(ui_user_banner_url(&u_hash, bbuf, sizeof bbuf));
  CT_CHECK(strcmp(bbuf, "https://cdn.discordapp.com/banners/42/abc123.png?size=512") == 0);

  ccord_user u_cdn64 = {.id = 42, .banner_url = "https://cdn.discordapp.com/banners/42/abc123.png?size=64"};
  CT_CHECK(ui_user_banner_url(&u_cdn64, bbuf, sizeof bbuf));
  CT_CHECK(strcmp(bbuf, "https://cdn.discordapp.com/banners/42/abc123.png?size=512") == 0);

  // 2. auto_ack must not ack already-read or older messages (never roll last_read_id backwards).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(45);   // settle and ack up to current newest message
  const ccord_channel *ch3111 = ccord_store_channel(F.store, 3111);
  CT_CHECK(ch3111 != nullptr);
  ccord_id cur_last = ch3111->last_message_id;
  CT_CHECK(cur_last != CCORD_ID_NONE);
  // Simulate Discord's read_state already being ahead of the newest local message.
  ccord_id ahead_id = cur_last + 100u;
  CT_CHECK(ccord_store_apply_read(F.store, 3111, ahead_id, 0, &err) == CCORD_OK);
  F.ui->ack_channel = CCORD_ID_NONE;
  F.ui->acked_message = CCORD_ID_NONE;
  F.ui->ack_next_ms = 0;
  size_t acks_before = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK);
  frame(45);   // > UI_ACK_MIN_GAP_MS
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK) == acks_before);

  // 3. CCORD_IN_TEXT is guarded in Friends view and sync_text_input deactivates.
  ccord_text_edit_clear(&F.ui->edit);
  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  F.ui->dm_friends_view = true;
  frame(2);
  CT_CHECK(!F.text_input_active);
  bool handled = ccord_ui_handle_input(F.ui, &(ccord_input_event){.type = CCORD_IN_TEXT, .text = "ghost"});
  CT_CHECK(!handled);
  CT_CHECK(F.ui->edit.len == 0);
  F.ui->dm_friends_view = false;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(F.text_input_active);

  // 4. Bare-key keybind (Mute) and printable PTT suppress their accompanying CCORD_IN_TEXT.
  int prev_mute_sc = F.ui->settings_keybinds[CCORD_UI_KEYBIND_MUTE].scancode;
  uint8_t prev_mute_mods = F.ui->settings_keybinds[CCORD_UI_KEYBIND_MUTE].mods;
  int prev_voice_mode = F.ui->settings_voice_mode;
  int prev_ptt_sc = F.ui->settings_ptt_scancode;

  F.ui->settings_keybinds[CCORD_UI_KEYBIND_MUTE].scancode = SDL_SCANCODE_M;
  F.ui->settings_keybinds[CCORD_UI_KEYBIND_MUTE].mods = 0;
  key(CCORD_KEY_M, 0);
  type_text("m");
  CT_CHECK(F.ui->edit.len == 0);

  F.ui->settings_voice_mode = 1;
  F.ui->settings_ptt_scancode = SDL_SCANCODE_T;
  key(CCORD_KEY_T, 0);
  type_text("t");
  CT_CHECK(F.ui->edit.len == 0);

  // Synthetic CCORD_IN_TEXT without a preceding suppressed KEY_DOWN still inserts normally.
  type_text("ok");
  CT_CHECK(strcmp(ccord_text_edit_cstr(&F.ui->edit), "ok") == 0);
  ccord_text_edit_clear(&F.ui->edit);

  F.ui->settings_keybinds[CCORD_UI_KEYBIND_MUTE].scancode = prev_mute_sc;
  F.ui->settings_keybinds[CCORD_UI_KEYBIND_MUTE].mods = prev_mute_mods;
  F.ui->settings_voice_mode = prev_voice_mode;
  F.ui->settings_ptt_scancode = prev_ptt_sc;
}

static void test_dm_profile_right_click_menu(void)
{
  ccord_error err;
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  ccord_id target_uid = other_member(g, 0);
  CT_CHECK(target_uid != CCORD_ID_NONE);
  CT_CHECK(ccord_session_open_dm(F.session, target_uid, &err) == CCORD_OK);
  frame(4);

  const ccord_channel *dm = nullptr;
  size_t ndm = ccord_store_channel_count(F.store, CCORD_DM_GUILD_ID);
  for (size_t i = 0; i < ndm; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, i);
    const ccord_user *du = c ? ui_dm_recipient(F.ui, c) : nullptr;
    if (du && du->id == target_uid) {
      dm = c;
      break;
    }
  }
  CT_CHECK(dm != nullptr);
  if (!dm) return;

  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  F.ui->dm_friends_view = false;
  clear_counts(CCORD_DM_GUILD_ID);
  const ccord_user_settings *us = ccord_store_user_settings(F.store);
  bool prev_dev = us && us->developer_mode;
  bool prev_anim_e = us ? us->animate_emoji : true;
  uint8_t prev_anim_s = us ? us->animate_stickers : 0;
  seed_user_settings(false, prev_anim_e, prev_anim_s);
  frame(4);

  // Right-click the DM row directly in the DM sidebar.
  float dm_y = scan_hover_channel(200.0f, dm->id);
  CT_CHECK(dm_y > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);

  CT_CHECK(dbg().context_menu_open);
  int mark_idx = menu_index("Mark As Read");
  CT_CHECK(mark_idx == 0 && F.ui->menu.items[mark_idx].disabled);
  CT_CHECK(menu_index("Pin") > 0);
  CT_CHECK(menu_index("Add to Favorites") > 0);
  CT_CHECK(menu_index("Profile") > 0);
  CT_CHECK(menu_index("Start a Call") > 0 || menu_index("Leave Call") > 0);
  int note_idx = menu_index("Add Note");
  if (note_idx < 0) note_idx = menu_index("Edit Note");
  CT_CHECK(note_idx > 0 && F.ui->menu.items[note_idx].sublabel != nullptr &&
           strcmp(F.ui->menu.items[note_idx].sublabel, "Only visible to you") == 0);
  CT_CHECK(menu_index("Add Friend Nickname") > 0 || menu_index("Edit Friend Nickname") > 0);
  CT_CHECK(menu_index("Close DM") > 0);
  int apps_idx = menu_index("Apps");
  CT_CHECK(apps_idx > 0 && F.ui->menu.items[apps_idx].has_sub);
  CT_CHECK(menu_index("Invite to Server") > 0);
  CT_CHECK(menu_index("Ignore") > 0 || menu_index("Unignore") > 0);
  int blk_idx = menu_index("Block");
  CT_CHECK(blk_idx > 0 && F.ui->menu.items[blk_idx].danger);
  CT_CHECK(menu_index("Mute @") > 0 || menu_index("Unmute @") > 0);
  CT_CHECK(menu_index("Copy User ID") < 0 && menu_index("Copy Channel ID") < 0);

  // Pin toggles state and updates menu label to Unpin.
  menu_click_index(menu_index("Pin"));
  frame(2);
  CT_CHECK(ui_dm_is_pinned(F.ui, dm->id));
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  CT_CHECK(menu_index("Unpin") > 0 && menu_index("Pin") < 0);
  menu_click_index(menu_index("Unpin"));
  frame(2);
  CT_CHECK(!ui_dm_is_pinned(F.ui, dm->id));

  // Add to Favorites toggles state and label.
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  menu_click_index(menu_index("Add to Favorites"));
  frame(2);
  CT_CHECK(ui_dm_is_favorite(F.ui, dm->id));
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  CT_CHECK(menu_index("Remove from Favorites") > 0);
  menu_click_index(menu_index("Remove from Favorites"));
  frame(2);
  CT_CHECK(!ui_dm_is_favorite(F.ui, dm->id));

  // Add Friend Nickname modal updates ui_friend_nickname and DM display name, and Edit/Clear removes it.
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  menu_click_index(menu_index("Add Friend Nickname"));
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_FRIEND_NICK && F.ui->modal_target_id == target_uid);
  type_text("Bestie");
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);
  const char *fn = ui_friend_nickname(F.ui, target_uid);
  CT_CHECK(fn != nullptr && strcmp(fn, "Bestie") == 0);
  CT_CHECK(strcmp(ui_dm_display_name(F.ui, dm), "Bestie") == 0);

  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  CT_CHECK(menu_index("Edit Friend Nickname") > 0);
  menu_click_index(menu_index("Edit Friend Nickname"));
  frame(1);
  ccord_text_edit_clear(&F.ui->modal_edit1);
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(ui_friend_nickname(F.ui, target_uid) == nullptr);

  // Add/Edit Note modal opens UI_MODAL_USER_NOTE and submits CCORD_ADMIN_SET_USER_NOTE.
  size_t adm_before = ccord_mock_admin_count(F.transport);
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  note_idx = menu_index("Add Note");
  if (note_idx < 0) note_idx = menu_index("Edit Note");
  menu_click_index(note_idx);
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_USER_NOTE && F.ui->modal_target_id == target_uid);
  ccord_text_edit_clear(&F.ui->modal_edit1);
  type_text("met at conf");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(ccord_mock_admin_count(F.transport) > adm_before);

  // Ignore / Unignore toggles state.
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  menu_click_index(menu_index("Ignore"));
  frame(1);
  CT_CHECK(ui_user_is_ignored(F.ui, target_uid));
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  CT_CHECK(menu_index("Unignore") > 0);
  menu_click_index(menu_index("Unignore"));
  frame(1);
  CT_CHECK(!ui_user_is_ignored(F.ui, target_uid));

  // Unread DM enables Mark As Read; Developer Mode exposes Copy User ID & Copy Channel ID.
  CT_CHECK(ccord_store_set_channel_counts(F.store, dm->id, 3, 0, &err) == CCORD_OK);
  seed_user_settings(true, prev_anim_e, prev_anim_s);
  frame(2);
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  mark_idx = menu_index("Mark As Read");
  int copy_uid_idx = menu_index("Copy User ID");
  int copy_cid_idx = menu_index("Copy Channel ID");
  CT_CHECK(mark_idx == 0 && !F.ui->menu.items[mark_idx].disabled);
  CT_CHECK(copy_uid_idx > 0 && copy_cid_idx == copy_uid_idx + 1);
  F.clip[0] = '\0';
  menu_click_index(copy_uid_idx);
  frame(1);
  char uid_str[32];
  ccord_id_format(target_uid, uid_str);
  CT_CHECK(strcmp(F.clip, uid_str) == 0);

  // Right-clicking a row in the Friends center view also opens the full DM/profile menu.
  CT_CHECK(ccord_store_set_relationship(F.store, target_uid, CCORD_RELATIONSHIP_FRIEND, &err) == CCORD_OK);
  F.ui->dm_friends_view = true;
  frame(2);
  if (F.ui->nfriend_row_hits > 0) {
    ccord_rect fr = F.ui->friend_row_hits[0].row_rect;
    mouse(CCORD_IN_MOUSE_DOWN, 3, fr.x + 80.0f, fr.y + fr.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().context_menu_open && menu_index("Profile") >= 0 && menu_index("Block") >= 0);
    key(CCORD_KEY_ESCAPE, 0);
    frame(1);
  }

  seed_user_settings(prev_dev, prev_anim_e, prev_anim_s);
  F.ui->dm_friends_view = false;
  clear_counts(CCORD_DM_GUILD_ID);
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  frame(2);
}

static void test_invite_preview_cards_and_url_opener(void)
{
  ccord_error err;
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  const ccord_channel *ch = ccord_store_channel_at(F.store, g, 0);
  CT_CHECK(ch != nullptr);
  if (!ch) return;
  ccord_id ch_id = ch->id;

  // An earlier test fills the store to CCORD_MAX_REAL_GUILDS; free a slot so accepting "synthwave" can upsert guild 9002.
  while (ccord_store_guild_count(F.store) >= CCORD_MAX_REAL_GUILDS) {
    const ccord_guild *filler = ccord_store_guild_at(F.store, ccord_store_guild_count(F.store) - 1u);
    CT_CHECK(filler != nullptr && filler->id != g && filler->id != 2001 && filler->id != CCORD_DM_GUILD_ID);
    if (!filler || filler->id == g || filler->id == 2001 || filler->id == CCORD_DM_GUILD_ID) break;
    CT_CHECK(ccord_store_delete_guild(F.store, filler->id, &err) == CCORD_OK);
  }

  // 1. Sending a Discord invite link in chat renders an interactive Invite Preview card with a Join button.
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, ch_id, &err) == CCORD_OK);
  frame(2);
  type_text("Check this out: https://discord.gg/synthwave");
  key(CCORD_KEY_RETURN, 0);
  frame(6);
  CT_CHECK(dbg().invite_cards_drawn >= 1 && dbg().invite_btn_rect.w > 0.0f);

  // Clicking Join directly on the chat invite card accepts the invite inside C-Cord without leaving the app.
  size_t adm_before_join = ccord_mock_admin_count(F.transport);
  click_rect(dbg().invite_btn_rect);
  frame(4);
  const ccord_admin_request *join_req = ccord_mock_last_admin(F.transport);
  CT_CHECK(ccord_mock_admin_count(F.transport) > adm_before_join && join_req &&
           join_req->op == CCORD_ADMIN_ACCEPT_INVITE && strcmp(join_req->code, "synthwave") == 0);
  CT_CHECK(ccord_store_guild(F.store, 9002) != nullptr);

  // 2. Activating / opening a Discord invite link opens Join Server modal in-app instead of spawning a browser.
  int urls_before = F.open_url_calls;
  CT_CHECK(ccord_ui_open_url(F.ui, "https://discord.gg/vulkan-lab"));
  frame(4);
  CT_CHECK(F.open_url_calls == urls_before);
  CT_CHECK(dbg().add_server_modal_open && dbg().modal_has_invite_preview);
  CT_CHECK(strcmp(dbg().modal_preview_guild_name, "Vulkan & Graphics Lab") == 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);

  // 2b. Opening a ccord://plugins/install/<id> deep link opens Settings -> Plugins review prompt without spawning a browser.
  CT_CHECK(ccord_ui_open_url(F.ui, "ccord://plugins/install/org.ccord.example-plugin"));
  frame(4);
  CT_CHECK(F.open_url_calls == urls_before);
  CT_CHECK(dbg().settings_open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);

  // 3. Clicking a clan tag with an unknown/unavailable guild settles out of loading instead of staying stuck on "Loading server...".
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, ch_id, &err) == CCORD_OK);
  ccord_id self_id = ccord_store_self_id(F.store);
  const ccord_user *self_u = ccord_store_user(F.store, self_id);
  CT_CHECK(self_u != nullptr);
  if (self_u) {
    // Copy string fields owned by the store before upserting so restoring saved_self does not read freed pointers.
    ccord_user saved_self = *self_u;
    char s_name[128], s_handle[128], s_status[256], s_avatar[256], s_banner[256], s_bio[512], s_pronouns[128];
    snprintf(s_name, sizeof s_name, "%s", self_u->name ? self_u->name : "");
    snprintf(s_handle, sizeof s_handle, "%s", self_u->handle ? self_u->handle : "");
    snprintf(s_status, sizeof s_status, "%s", self_u->status_text ? self_u->status_text : "");
    snprintf(s_avatar, sizeof s_avatar, "%s", self_u->avatar_url ? self_u->avatar_url : "");
    snprintf(s_banner, sizeof s_banner, "%s", self_u->banner_url ? self_u->banner_url : "");
    snprintf(s_bio, sizeof s_bio, "%s", self_u->bio ? self_u->bio : "");
    snprintf(s_pronouns, sizeof s_pronouns, "%s", self_u->pronouns ? self_u->pronouns : "");
    saved_self.name = s_name;
    saved_self.handle = s_handle;
    saved_self.status_text = s_status;
    saved_self.avatar_url = s_avatar;
    saved_self.banner_url = s_banner;
    saved_self.bio = s_bio;
    saved_self.pronouns = s_pronouns;

    ccord_user unk_u = saved_self;
    snprintf(unk_u.clan_tag, sizeof unk_u.clan_tag, "UNKN");
    unk_u.clan_guild_id = 999999999;
    CT_CHECK(ccord_store_upsert_user(F.store, &unk_u, &err) == CCORD_OK);
    frame(2);
    ccord_rect tag_rect = {0};
    CT_CHECK(ui_clan_popover_hit_rect(F.ui, "UNKN", &tag_rect));
    if (tag_rect.w > 0.0f) {
      click_rect(tag_rect);
      frame(6);
      CT_CHECK(ui_clan_popover_is_open(F.ui));
      CT_CHECK(ui_clan_popover_box(F.ui).w > 0.0f);
      CT_CHECK(ui_clan_popover_guild_id(F.ui) == 999999999);
      CT_CHECK(!ccord_session_guild_profile_loading(F.session, 999999999));
      const ccord_guild_profile *unk_gp = ccord_session_guild_profile(F.session, 999999999);
      CT_CHECK(unk_gp != nullptr && unk_gp->unavailable);
      key(CCORD_KEY_ESCAPE, 0);
      frame(1);
      CT_CHECK(!ui_clan_popover_is_open(F.ui));
    }

    // Clicking a clan tag in chat/DM works even when prof_roles has not been allocated yet.
    ui_profile_roles_destroy(F.ui);
    CT_CHECK(F.ui->prof_roles == nullptr);
    ccord_user known_u = saved_self;
    snprintf(known_u.clan_tag, sizeof known_u.clan_tag, "KNO");
    known_u.clan_guild_id = g;
    CT_CHECK(ccord_store_upsert_user(F.store, &known_u, &err) == CCORD_OK);
    frame(2);
    ccord_rect known_rect = {0};
    CT_CHECK(ui_clan_popover_hit_rect(F.ui, "KNO", &known_rect));
    if (known_rect.w > 0.0f) {
      click_rect(known_rect);
      frame(6);
      CT_CHECK(ui_clan_popover_is_open(F.ui));
      CT_CHECK(ui_clan_popover_guild_id(F.ui) == g);
      CT_CHECK(ccord_session_guild_profile(F.session, g) != nullptr);
      key(CCORD_KEY_ESCAPE, 0);
      frame(1);
      CT_CHECK(!ui_clan_popover_is_open(F.ui));
    }

    // A transport error on an unavailable guild profile fetch also clears loading instead of sticking.
    ccord_user fail_u = saved_self;
    snprintf(fail_u.clan_tag, sizeof fail_u.clan_tag, "FAIL");
    fail_u.clan_guild_id = 999999998;
    CT_CHECK(ccord_store_upsert_user(F.store, &fail_u, &err) == CCORD_OK);
    frame(2);
    ccord_rect fail_rect = {0};
    CT_CHECK(ui_clan_popover_hit_rect(F.ui, "FAIL", &fail_rect));
    if (fail_rect.w > 0.0f) {
      ccord_mock_fail_next_admin(F.transport, true);
      click_rect(fail_rect);
      frame(6);
      CT_CHECK(ui_clan_popover_is_open(F.ui));
      CT_CHECK(ui_clan_popover_guild_id(F.ui) == 999999998);
      CT_CHECK(!ccord_session_guild_profile_loading(F.session, 999999998));
      key(CCORD_KEY_ESCAPE, 0);
      frame(1);
      CT_CHECK(!ui_clan_popover_is_open(F.ui));
    }

    CT_CHECK(!ccord_session_guild_profile_loading(F.session, CCORD_ID_NONE));
    CT_CHECK(ccord_store_upsert_user(F.store, &saved_self, &err) == CCORD_OK);
  }
}

typedef struct panel_ev_log {
  int count;
  ccord_panel_event_kind last_kind;
  char last_panel[40];
  char last_node[64];
  uint8_t last_button;
  uint8_t last_mods;
  float last_wheel;
} panel_ev_log;

static void record_panel_ev(void *ud, const ccord_panel_contribution *panel, const ccord_panel_event *ev)
{
  panel_ev_log *log = ud;
  log->count++;
  log->last_kind = ev->kind;
  snprintf(log->last_panel, sizeof log->last_panel, "%s", panel->panel_id ? panel->panel_id : "");
  snprintf(log->last_node, sizeof log->last_node, "%s", ev->node_id ? ev->node_id : "");
  log->last_button = ev->button;
  log->last_mods = ev->mods;
  log->last_wheel = ev->wheel;
}

static void test_plugin_ui_panels(void)
{
  ccord_error err = {0};
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  ccord_id dev = find_channel(g, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(dev != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);
  frame(2);

  // Baseline layout with zero mounted panels starts flush at y = 0.
  ccord_layout base = *ccord_ui_get_layout(F.ui);
  CT_CHECK(fabsf(base.server_list.y) < 0.01f && fabsf(base.channel_list.y) < 0.01f &&
           fabsf(base.chat_header.y) < 0.01f);

  panel_ev_log plog = {0};
  ccord_contrib_set_handlers(F.contrib, &(ccord_contrib_handlers){
                                          .ud = &plog, .menu_activate = menu_activate, .panel_event = record_panel_ev});

  // Mount Layer 1 flexbox panel in 'top', Layer 2 vector canvas panel in 'subheader', and status bar in 'bottom'.
  const char *top_spec =
    "{\"type\":\"row\",\"id\":\"strip\",\"bg\":\"bg_tertiary\",\"padding\":[4,8,4,8],\"gap\":6,"
    "\"children\":["
      "{\"type\":\"box\",\"id\":\"pill-1\",\"direction\":\"row\",\"align\":\"center\",\"gap\":6,"
       "\"padding\":[4,8,4,8],\"radius\":6,\"bg\":\"bg_primary\",\"hover_bg\":\"bg_hover\","
       "\"border_color\":\"border\",\"border_width\":1,\"border_sides\":\"no_bottom\","
       "\"cursor\":\"pointer\",\"tooltip\":\"First item\","
       "\"children\":["
         "{\"type\":\"icon\",\"icon\":\"hash\",\"size\":14,\"color\":\"text_muted\"},"
         "{\"type\":\"text\",\"text\":\"dev\",\"font\":\"bold\",\"color\":\"text_primary\"},"
         "{\"type\":\"icon\",\"id\":\"pill-1-close\",\"icon\":\"close\",\"size\":12,\"color\":\"text_muted\","
          "\"hover_color\":\"text_primary\",\"show_on_hover\":true,\"cursor\":\"pointer\"}"
       "]},"
      "{\"type\":\"button\",\"id\":\"add-btn\",\"icon\":\"plus\",\"label\":\"Add\",\"variant\":\"secondary\"},"
      "{\"type\":\"spacer\"},"
      "{\"type\":\"divider\",\"vertical\":true,\"color\":\"divider\"}"
    "]}";
  const char *sub_spec =
    "{\"type\":\"canvas\",\"id\":\"sub-canvas\",\"w\":\"100%\",\"h\":28,"
    "\"draw\":["
      "{\"cmd\":\"rect\",\"x\":0,\"y\":0,\"w\":\"100%\",\"h\":\"100%\",\"color\":\"bg_secondary\"},"
      "{\"cmd\":\"rrect\",\"x\":8,\"y\":4,\"w\":110,\"h\":20,\"radius\":5,\"color\":\"#5865F233\"},"
      "{\"cmd\":\"circle\",\"cx\":18,\"cy\":14,\"r\":4,\"color\":\"online\"},"
      "{\"cmd\":\"text\",\"x\":28,\"y\":7,\"text\":\"Canvas Subheader\",\"color\":\"text_primary\",\"size\":12},"
      "{\"cmd\":\"line\",\"x1\":0,\"y1\":27,\"x2\":\"100%\",\"y2\":27,\"width\":1,\"color\":\"divider\"}"
    "],"
    "\"hits\":["
      "{\"id\":\"canvas-pill\",\"x\":8,\"y\":4,\"w\":110,\"h\":20,\"cursor\":\"pointer\",\"tooltip\":\"Canvas pill\"}"
    "]}";
  const char *bot_spec =
    "{\"type\":\"row\",\"bg\":\"bg_tertiary\",\"padding\":[2,10,2,10],\"align\":\"center\","
    "\"children\":[{\"type\":\"text\",\"text\":\"Ready\",\"color\":\"text_muted\",\"size\":11}]}";

  CT_CHECK(ccord_contrib_set_panel(F.contrib, "dev.test.ui", "top-strip", CCORD_PANEL_TOP, 36.0f, top_spec, &err) == CCORD_OK);
  CT_CHECK(ccord_contrib_set_panel(F.contrib, "dev.test.ui", "sub-bar", CCORD_PANEL_SUBHEADER, 28.0f, sub_spec, &err) == CCORD_OK);
  CT_CHECK(ccord_contrib_set_panel(F.contrib, "dev.test.ui", "status", CCORD_PANEL_BOTTOM, 24.0f, bot_spec, &err) == CCORD_OK);
  frame(2);

  // Verify layout reflowed cleanly across all 3 slots (panels mount in the main workspace column; rail/channels stay flush).
  ccord_layout mounted = *ccord_ui_get_layout(F.ui);
  CT_CHECK(fabsf(mounted.server_list.y) < 0.01f);
  CT_CHECK(fabsf(mounted.channel_list.y) < 0.01f);
  CT_CHECK(fabsf(mounted.chat_header.y - 36.0f) < 0.01f);
  CT_CHECK(fabsf(mounted.message_area.y - (base.message_area.y + 36.0f + 28.0f)) < 0.01f);
  CT_CHECK(fabsf((mounted.composer.y + mounted.composer.h) - (base.composer.y + base.composer.h - 24.0f)) < 0.01f);

  // Verify hit rects and event dispatch (click, double_click, middle_click, right_click, wheel, show_on_hover).
  ccord_rect pill_r = {0}, close_r = {0}, canvas_r = {0};
  CT_CHECK(ui_panels_find_hit_rect(F.ui, "dev.test.ui", "top-strip", "pill-1", &pill_r) && pill_r.w > 20.0f);
  CT_CHECK(ui_panels_find_hit_rect(F.ui, "dev.test.ui", "sub-bar", "canvas-pill", &canvas_r) &&
           fabsf(canvas_r.w - 110.0f) < 0.5f);
  // Before hovering pill-1, show_on_hover child 'pill-1-close' is hidden (not registered as a hit).
  CT_CHECK(!ui_panels_find_hit_rect(F.ui, "dev.test.ui", "top-strip", "pill-1-close", &close_r));

  // Hover pill-1 -> cursor turns to POINTER and show_on_hover child becomes active!
  mouse(CCORD_IN_MOUSE_MOVE, 0, pill_r.x + 8.0f, pill_r.y + pill_r.h * 0.5f);
  frame(2);
  CT_CHECK(F.ui->cursor == CCORD_CURSOR_POINTER);
  CT_CHECK(ui_panels_find_hit_rect(F.ui, "dev.test.ui", "top-strip", "pill-1-close", &close_r) && close_r.w > 8.0f);

  // Left-click pill-1 -> CCORD_PANEL_EV_CLICK; double-click -> CCORD_PANEL_EV_DOUBLE_CLICK.
  plog.count = 0;
  send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                           .x = pill_r.x + 8.0f, .y = pill_r.y + pill_r.h * 0.5f});
  send((ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1,
                           .x = pill_r.x + 8.0f, .y = pill_r.y + pill_r.h * 0.5f});
  CT_CHECK(plog.count == 1 && plog.last_kind == CCORD_PANEL_EV_CLICK && strcmp(plog.last_node, "pill-1") == 0);

  send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 2,
                           .x = pill_r.x + 8.0f, .y = pill_r.y + pill_r.h * 0.5f});
  send((ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 2,
                           .x = pill_r.x + 8.0f, .y = pill_r.y + pill_r.h * 0.5f});
  CT_CHECK(plog.count == 2 && plog.last_kind == CCORD_PANEL_EV_DOUBLE_CLICK && strcmp(plog.last_node, "pill-1") == 0);

  // Click nested show_on_hover close icon -> deepest hit ('pill-1-close') wins over parent 'pill-1'.
  send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                           .x = close_r.x + close_r.w * 0.5f, .y = close_r.y + close_r.h * 0.5f});
  send((ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1,
                           .x = close_r.x + close_r.w * 0.5f, .y = close_r.y + close_r.h * 0.5f});
  CT_CHECK(plog.count == 3 && plog.last_kind == CCORD_PANEL_EV_CLICK && strcmp(plog.last_node, "pill-1-close") == 0);

  // Middle-click and right-click on canvas hit region + wheel over top strip.
  send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1,
                           .x = canvas_r.x + 20.0f, .y = canvas_r.y + 10.0f});
  CT_CHECK(plog.count == 4 && plog.last_kind == CCORD_PANEL_EV_MIDDLE_CLICK &&
           strcmp(plog.last_panel, "sub-bar") == 0 && strcmp(plog.last_node, "canvas-pill") == 0);

  send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 3, .clicks = 1,
                           .x = canvas_r.x + 20.0f, .y = canvas_r.y + 10.0f});
  CT_CHECK(plog.count == 5 && plog.last_kind == CCORD_PANEL_EV_RIGHT_CLICK &&
           strcmp(plog.last_node, "canvas-pill") == 0 && !dbg().context_menu_open);

  send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = pill_r.x + 8.0f, .y = pill_r.y + 8.0f, .wheel = -1.0f});
  CT_CHECK(plog.count == 6 && plog.last_kind == CCORD_PANEL_EV_WHEEL && plog.last_wheel < 0.0f);

  // Unmount all 3 panels and verify full workspace height is reclaimed.
  CT_CHECK(ccord_contrib_remove_panel(F.contrib, "dev.test.ui", "top-strip"));
  CT_CHECK(ccord_contrib_remove_panel(F.contrib, "dev.test.ui", "sub-bar"));
  CT_CHECK(ccord_contrib_remove_panel(F.contrib, "dev.test.ui", "status"));
  ccord_contrib_set_handlers(F.contrib, &(ccord_contrib_handlers){.menu_activate = menu_activate});
  frame(2);
  ccord_layout after = *ccord_ui_get_layout(F.ui);
  CT_CHECK(fabsf(after.server_list.y) < 0.01f && fabsf(after.message_area.y - base.message_area.y) < 0.01f &&
           fabsf(after.composer.y - base.composer.y) < 0.01f);
}

static void test_composer_upload_progress_indicator(void)
{
  ccord_error err = {0};
  ccord_mock_config mc = {.seed = 7, .latency_ms = 240, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  ccord_id dev = find_channel(g, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(dev != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);
  frame(8);

  uint8_t img_bytes[4096], vid_bytes[8192];
  memset(img_bytes, 0xAA, sizeof img_bytes);
  memset(vid_bytes, 0x55, sizeof vid_bytes);
  ccord_ui_clear_attachment(F.ui);
  CT_CHECK(ccord_ui_attach_bytes(F.ui, "render.png", img_bytes, sizeof img_bytes));
  CT_CHECK(ccord_ui_attach_bytes(F.ui, "demo.mp4", vid_bytes, sizeof vid_bytes));
  frame(2);

  ccord_text_edit_clear(&F.ui->edit);
  type_text("uploading media");
  key(CCORD_KEY_RETURN, 0);

  // Immediately after send (t = +50ms < 240ms latency): upload banner is active above composer!
  draw_only();
  ui_upload_debug up = {0};
  ui_upload_debug_get(F.ui, &up);
  CT_CHECK(up.count == 1u);
  CT_CHECK(up.banner_rect.w > 120.0f && up.banner_rect.h > 30.0f);
  CT_CHECK(up.bar_rects[0].w > 80.0f && up.bar_rects[0].h > 2.0f);
  CT_CHECK(strstr(up.label[0], "render.png") != nullptr && strstr(up.label[0], "(1/2)") != nullptr);
  CT_CHECK(ccord_ui_update(F.ui, F.now + 16u));   // keeps frame pacing active while uploading

  // Advance mid-upload (+100ms -> 166ms / 240ms): file 2/2 is actively uploading with mid-range percentage.
  F.now += 100u;
  draw_only();
  ui_upload_debug_get(F.ui, &up);
  CT_CHECK(up.count == 1u);
  CT_CHECK(up.pct[0] > 20 && up.pct[0] < 100);
  CT_CHECK(up.shown_fraction[0] > 0.0f);
  CT_CHECK(strstr(up.label[0], "demo.mp4") != nullptr && strstr(up.label[0], "(2/2)") != nullptr);

  // Pump past latency so MESSAGE_CREATE arrives -> upload banner disappears cleanly.
  frame(6);
  ui_upload_debug_get(F.ui, &up);
  CT_CHECK(up.count == 0u && up.banner_rect.h == 0.0f);

  // Restore standard 0-latency mock transport for subsequent tests.
  ccord_mock_config fast_mc = {.seed = 7, .now_ms = fake_clock};
  ccord_transport *fast_tr = ccord_mock_transport_create(&fast_mc, &err);
  CT_CHECK(fast_tr != nullptr);
  if (fast_tr) {
    CT_CHECK(ccord_session_set_transport(F.session, fast_tr, &err) == CCORD_OK);
    F.transport = fast_tr;
    frame(4);
  }
}

typedef struct nav_bus_log {
  int aux_count;
  ccord_event last_aux;
  int view_count;
  ccord_event last_view;
} nav_bus_log;

static void on_nav_bus_ev(const ccord_event *ev, void *ud)
{
  nav_bus_log *log = ud;
  if (ev->type == CCORD_EVT_NAV_AUX) {
    log->aux_count++;
    log->last_aux = *ev;
  } else if (ev->type == CCORD_EVT_VIEW_SELECT) {
    log->view_count++;
    log->last_view = *ev;
  }
}

static void test_plugin_nav_hooks_menus_and_aux_events(void)
{
  ccord_error err = {0};
  const ccord_id hq = 2001, lua = 2002;
  ccord_id dev = find_channel(hq, "dev", CCORD_CHANNEL_TEXT);
  ccord_id forum_ch = find_channel(hq, "design-forum", CCORD_CHANNEL_TEXT);
  ccord_id other_ch = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, hq); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, hq, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->id != dev && c->id != forum_ch &&
        !ccord_discord_type_is_forum(c->discord_type) && ui_channel_visible(F.ui, hq, c)) {
      other_ch = c->id;
      break;
    }
  }
  CT_CHECK(dev != CCORD_ID_NONE && other_ch != CCORD_ID_NONE && forum_ch != CCORD_ID_NONE);

  nav_bus_log nlog = {0};
  ccord_sub_id sub = ccord_bus_subscribe(F.bus, CCORD_EVT_ANY, on_nav_bus_ev, &nlog);
  CT_CHECK(sub != 0);

  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);
  F.ui->chan_scroll = 0.0f;
  frame(3);

  // 1. Ctrl+Click and Middle-Click on a channel row emit CCORD_EVT_NAV_AUX without leaving #dev.
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float cx = l->channel_list.x + l->channel_list.w * 0.5f;
  float oy = scan_hover_channel(cx, other_ch);
  CT_CHECK(oy > 0.0f);
  nlog.aux_count = 0;
  send((ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = cx, .y = oy});
  send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .mods = CCORD_MOD_CTRL, .x = cx, .y = oy});
  send((ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .mods = CCORD_MOD_CTRL, .x = cx, .y = oy});
  frame(2);
  CT_CHECK(nlog.aux_count == 1 && nlog.last_aux.guild_id == hq && nlog.last_aux.channel_id == other_ch &&
           nlog.last_aux.value == CCORD_NAV_VIEW_CHANNEL);
  CT_CHECK(ccord_store_selected_channel(F.store) == dev);

  send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1, .x = cx, .y = oy});
  send((ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 2, .clicks = 1, .x = cx, .y = oy});
  frame(2);
  CT_CHECK(nlog.aux_count == 2 && nlog.last_aux.channel_id == other_ch);

  // 2. Middle-Click on server rail icon and DM Home rail icon emit CCORD_EVT_NAV_AUX (CCORD_NAV_VIEW_GUILD).
  size_t lua_slot = 0;
  ui_rail_slot slots[UI_RAIL_SLOTS_MAX];
  size_t nslots = ui_rail_build_slots(F.ui, slots, UI_RAIL_SLOTS_MAX, nullptr);
  bool found_lua = false;
  for (size_t i = 0; i < nslots; i++) {
    if (slots[i].kind == UI_RAIL_GUILD && slots[i].guild_id == lua) {
      lua_slot = i;
      found_lua = true;
      break;
    }
  }
  CT_CHECK(found_lua);
  if (found_lua) {
    ccord_rect lr = ui_guild_icon_rect(F.ui, lua_slot);
    mouse(CCORD_IN_MOUSE_MOVE, 0, lr.x + lr.w * 0.5f, lr.y + lr.h * 0.5f);
    send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1,
                             .x = lr.x + lr.w * 0.5f, .y = lr.y + lr.h * 0.5f});
    frame(2);
    CT_CHECK(nlog.aux_count == 3 && nlog.last_aux.guild_id == lua && nlog.last_aux.value == CCORD_NAV_VIEW_GUILD);
  }
  ccord_rect dmr = ui_guild_icon_rect(F.ui, 0);
  mouse(CCORD_IN_MOUSE_MOVE, 0, dmr.x + dmr.w * 0.5f, dmr.y + dmr.h * 0.5f);
  send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1,
                           .x = dmr.x + dmr.w * 0.5f, .y = dmr.y + dmr.h * 0.5f});
  frame(2);
  CT_CHECK(nlog.aux_count == 4 && nlog.last_aux.guild_id == CCORD_DM_GUILD_ID &&
           nlog.last_aux.value == CCORD_NAV_VIEW_GUILD);

  // 3. CCORD_MENU_NAV plugin context menu on DM Home rail icon and DM Friends row + ccord_contrib_remove_menu.
  CT_CHECK(ccord_contrib_add_menu(F.contrib, "test.plugin", "nav-item", "Open in Aux", CCORD_MENU_NAV, &err) == CCORD_OK);
  F.menu_activations = 0;
  mouse(CCORD_IN_MOUSE_MOVE, 0, dmr.x + dmr.w * 0.5f, dmr.y + dmr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, dmr.x + dmr.w * 0.5f, dmr.y + dmr.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().context_menu_open && menu_has("Open in Aux"));
  CT_CHECK(menu_click_label("Open in Aux"));
  frame(1);
  CT_CHECK(F.menu_activations == 1 && F.last_ctx.target == CCORD_MENU_NAV &&
           F.last_ctx.message_id == CCORD_NAV_VIEW_GUILD);
  CT_CHECK(ccord_contrib_remove_menu(F.contrib, "test.plugin", "nav-item"));

  // 4. Middle-click Forum post card emits CCORD_EVT_NAV_AUX and materializes post channel in store.
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, forum_ch, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(ui_forum_is_active(F.ui));
  ccord_id post_tid = CCORD_ID_NONE;
  ccord_rect card0 = ui_forum_post_card_rect(F.ui, 0, &post_tid);
  CT_CHECK(card0.w > 50.0f && post_tid != CCORD_ID_NONE);
  nlog.aux_count = 0;
  mouse(CCORD_IN_MOUSE_MOVE, 0, card0.x + 40.0f, card0.y + 24.0f);
  send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1,
                           .x = card0.x + 40.0f, .y = card0.y + 24.0f});
  frame(2);
  CT_CHECK(nlog.aux_count == 1 && nlog.last_aux.channel_id == post_tid && ccord_store_channel(F.store, post_tid) != nullptr);

  // 5. Inline #channel mention and inline thread card emit CCORD_EVT_NAV_AUX on middle-click and pass parent_id on menu.
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);
  frame(3);
  size_t dev_mc = ccord_store_message_count(F.store, dev);
  const ccord_message *dev_tail = dev_mc > 0 ? ccord_store_message_at(F.store, dev, dev_mc - 1u) : nullptr;
  ccord_id next_mid = (dev_tail ? dev_tail->id : 1800000000000000000ULL) + 100ULL;
  char body[160];
  snprintf(body, sizeof body, "check out <#%llu> for details", (unsigned long long)other_ch);
  ccord_message_rich rich = {
    .present = CCORD_RICH_HAS_THREAD,
    .thread = {.id = 987654321ULL, .name = "Inline Thread Card", .message_count = 4},
  };
  ccord_message cm = {
    .id = next_mid,
    .channel_id = dev,
    .author_id = 1002,
    .timestamp_ms = (dev_tail ? dev_tail->timestamp_ms : (int64_t)F.ui->now_unix_ms) + 600000,
    .content = body,
    .rich = &rich,
  };
  CT_CHECK(ccord_store_append_message(F.store, &cm, &err) == CCORD_OK);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(3);
  CT_CHECK(F.ui->nchan_hits > 0 && F.ui->nthread_hits > 0);
  if (F.ui->nchan_hits > 0) {
    ccord_rect cr = F.ui->chan_hits[F.ui->nchan_hits - 1u].rect;
    nlog.aux_count = 0;
    send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1,
                             .x = cr.x + cr.w * 0.5f, .y = cr.y + cr.h * 0.5f});
    frame(2);
    CT_CHECK(nlog.aux_count == 1 && nlog.last_aux.channel_id == other_ch);
  }
  if (F.ui->nthread_hits > 0) {
    ccord_rect trc = F.ui->thread_hits[F.ui->nthread_hits - 1u].rect;
    nlog.aux_count = 0;
    send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1,
                             .x = trc.x + trc.w * 0.5f, .y = trc.y + trc.h * 0.5f});
    frame(2);
    CT_CHECK(nlog.aux_count == 1 && nlog.last_aux.channel_id == 987654321ULL &&
             nlog.last_aux.message_id == dev && nlog.last_aux.value == CCORD_NAV_VIEW_THREAD);
  }

  // 6. UI hooks registered on contrib drive client views & actions cleanly.
  nlog.view_count = 0;
  CT_CHECK(ccord_contrib_ui_navigate(F.contrib, 0, 0, 0, nullptr, "friends"));
  frame(2);
  CT_CHECK(strcmp(ccord_contrib_ui_view(F.contrib), "friends") == 0 && nlog.view_count >= 1);
  CT_CHECK(ccord_contrib_ui_action(F.contrib, "quick_switcher"));
  CT_CHECK(F.ui->quick_switcher_open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!F.ui->quick_switcher_open);

  ccord_bus_unsubscribe(F.bus, sub);
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);
  frame(2);
}

static void test_chat_message_vs_profile_right_click_and_admin_tools(void)
{
  ccord_error err = {0};
  const ccord_id hq = 2001, garden = 2003;
  const ccord_id ada = 1002, bjorn = 1003, chen = 1004;
  ccord_id dev = find_channel(hq, "dev", CCORD_CHANNEL_TEXT);
  ccord_id art = find_channel(garden, "art", CCORD_CHANNEL_TEXT);
  CT_CHECK(dev != CCORD_ID_NONE && art != CCORD_ID_NONE);

  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);
  frame(3);

  // Append a base message from Chen (1004) and a non-compact reply from Ada (1002) mentioning @Bjorn (<@1003>).
  size_t dev_mc = ccord_store_message_count(F.store, dev);
  const ccord_message *dev_tail = dev_mc > 0 ? ccord_store_message_at(F.store, dev, dev_mc - 1u) : nullptr;
  ccord_id base_id = (dev_tail ? dev_tail->id : 1800000000000000000ULL) + 100ULL;
  int64_t base_ts = (dev_tail ? dev_tail->timestamp_ms : (int64_t)F.ui->now_unix_ms) + 600000;
  ccord_message base_m = {
    .id = base_id,
    .channel_id = dev,
    .author_id = chen,
    .timestamp_ms = base_ts,
    .content = "Chen base message for reply target",
  };
  ccord_message reply_m = {
    .id = base_id + 1ULL,
    .channel_id = dev,
    .author_id = ada,
    .reply_to_id = base_m.id,
    .timestamp_ms = base_ts + 600000,
    .content = "Plain message text body mentioning <@1003> right here",
  };
  CT_CHECK(ccord_store_append_message(F.store, &base_m, &err) == CCORD_OK);
  CT_CHECK(ccord_store_append_message(F.store, &reply_m, &err) == CCORD_OK);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(3);

  // Locate Ada's rendered message row.
  const vis_row *ada_row = nullptr;
  for (size_t i = 0; i < F.ui->nvis; i++) {
    if (F.ui->vis[i].msg_id == reply_m.id) {
      ada_row = &F.ui->vis[i];
      break;
    }
  }
  CT_CHECK(ada_row != nullptr && ada_row->avatar_rect.w > 0.0f && ada_row->author_rect.w > 0.0f);
  if (!ada_row) return;

  // 1. Right-clicking the message body (away from avatar/name/@mention) opens CCORD_MENU_MESSAGE.
  float body_x = ada_row->author_rect.x + 20.0f;
  float body_y = ada_row->y + ada_row->h - 10.0f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, body_x, body_y);
  frame(1);
  CT_CHECK(dbg().hover_message == reply_m.id && dbg().hover_user == CCORD_ID_NONE);
  mouse(CCORD_IN_MOUSE_DOWN, 3, body_x, body_y);
  frame(1);
  CT_CHECK(dbg().context_menu_open && F.ui->menu.ctx.target == CCORD_MENU_MESSAGE &&
           F.ui->menu.ctx.message_id == reply_m.id);
  CT_CHECK(menu_has("Copy Text") && menu_has("Reply") && !menu_has("Profile") && !menu_has("Kick Member"));
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);

  // 2. Hovering & right-clicking Ada's avatar opens CCORD_MENU_MEMBER with profile + server admin tools!
  ccord_rect av = ada_row->avatar_rect;
  mouse(CCORD_IN_MOUSE_MOVE, 0, av.x + av.w * 0.5f, av.y + av.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().hover_user == ada);
  mouse(CCORD_IN_MOUSE_DOWN, 3, av.x + av.w * 0.5f, av.y + av.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().context_menu_open && F.ui->menu.ctx.target == CCORD_MENU_MEMBER && F.ui->menu.ctx.user_id == ada);
  CT_CHECK(menu_has("Profile") && menu_has("Mention") && menu_has("Message (Open DM)") &&
           menu_has("Open in Mod View") && menu_has("Roles") && menu_has("Change Nickname") &&
           menu_has("Timeout Member") && menu_has("Kick Member") && menu_has("Ban Member"));
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);

  // 3. Hovering & right-clicking Ada's display name opens CCORD_MENU_MEMBER; clicking "Mention" inserts @Ada!
  ccord_text_edit_clear(&F.ui->edit);
  ccord_rect au = ada_row->author_rect;
  mouse(CCORD_IN_MOUSE_MOVE, 0, au.x + au.w * 0.5f, au.y + au.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().hover_user == ada);
  mouse(CCORD_IN_MOUSE_DOWN, 3, au.x + au.w * 0.5f, au.y + au.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().context_menu_open && F.ui->menu.ctx.target == CCORD_MENU_MEMBER && F.ui->menu.ctx.user_id == ada);
  CT_CHECK(menu_click_label("Mention"));
  frame(1);
  CT_CHECK(strstr(ccord_ui_composer_text(F.ui), "@Ada ") != nullptr);
  ccord_text_edit_clear(&F.ui->edit);

  // 4. Hovering & right-clicking inline @Bjorn mention and replied-to author Chen open their respective profile menus!
  ccord_rect bjorn_hit = {0}, chen_reply_hit = {0};
  for (size_t i = 0; i < F.ui->nuser_hits; i++) {
    if (F.ui->user_hits[i].user_id == bjorn && F.ui->user_hits[i].rect.y >= ada_row->y) {
      bjorn_hit = F.ui->user_hits[i].rect;
    }
    if (F.ui->user_hits[i].user_id == chen && F.ui->user_hits[i].rect.y >= ada_row->y &&
        F.ui->user_hits[i].rect.y < au.y) {
      chen_reply_hit = F.ui->user_hits[i].rect;
    }
  }
  CT_CHECK(bjorn_hit.w > 0.0f && chen_reply_hit.w > 0.0f);
  if (bjorn_hit.w > 0.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, bjorn_hit.x + bjorn_hit.w * 0.5f, bjorn_hit.y + bjorn_hit.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().hover_user == bjorn);
    mouse(CCORD_IN_MOUSE_DOWN, 3, bjorn_hit.x + bjorn_hit.w * 0.5f, bjorn_hit.y + bjorn_hit.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().context_menu_open && F.ui->menu.ctx.target == CCORD_MENU_MEMBER && F.ui->menu.ctx.user_id == bjorn);
    key(CCORD_KEY_ESCAPE, 0);
    frame(1);
  }
  if (chen_reply_hit.w > 0.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, chen_reply_hit.x + chen_reply_hit.w * 0.5f, chen_reply_hit.y + chen_reply_hit.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().hover_user == chen);
    mouse(CCORD_IN_MOUSE_DOWN, 3, chen_reply_hit.x + chen_reply_hit.w * 0.5f, chen_reply_hit.y + chen_reply_hit.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().context_menu_open && F.ui->menu.ctx.target == CCORD_MENU_MEMBER && F.ui->menu.ctx.user_id == chen);
    key(CCORD_KEY_ESCAPE, 0);
    frame(1);
  }

  // 5. Server admin tools: "Roles" submenu toggles a role on Ada, and "Open in Mod View" opens Mod View!
  size_t adm_before = ccord_mock_admin_count(F.transport);
  open_menu(CCORD_MENU_MEMBER, hq, dev, ada, CCORD_ID_NONE);
  frame(1);
  int roles_idx = menu_index("Roles");
  CT_CHECK(roles_idx >= 0 && F.ui->menu.items[roles_idx].has_sub);
  menu_point_at(roles_idx);
  CT_CHECK(F.ui->menu.open_sub == roles_idx && dbg().context_menu_sub_items > 0);
  int first_role_child = -1;
  for (size_t i = 0; i < F.ui->menu.count; i++) {
    if (F.ui->menu.items[i].parent == roles_idx) {
      first_role_child = (int)i;
      break;
    }
  }
  CT_CHECK(first_role_child >= 0);
  if (first_role_child >= 0) {
    menu_click_index(first_role_child);
    frame(3);
    CT_CHECK(ccord_mock_admin_count(F.transport) == adm_before + 1u);
    const ccord_admin_request *last = ccord_mock_last_admin(F.transport);
    CT_CHECK(last && (last->op == CCORD_ADMIN_ADD_MEMBER_ROLE || last->op == CCORD_ADMIN_REMOVE_MEMBER_ROLE) &&
             last->user_id == ada);
  }

  open_menu(CCORD_MENU_MEMBER, hq, dev, ada, CCORD_ID_NONE);
  frame(1);
  CT_CHECK(menu_click_label("Open in Mod View"));
  frame(2);
  CT_CHECK(ui_server_settings_is_open(F.ui));
  ui_server_settings_close(F.ui);
  frame(2);
  CT_CHECK(!ui_server_settings_is_open(F.ui));

  // 6. Unprivileged server (Pixel Garden): right-clicking a user's profile shows profile items ONLY, zero admin tools!
  CT_CHECK(ccord_session_select_guild(F.session, garden, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, art, &err) == CCORD_OK);
  frame(3);
  size_t art_mc = ccord_store_message_count(F.store, art);
  const ccord_message *art_tail = art_mc > 0 ? ccord_store_message_at(F.store, art, art_mc - 1u) : nullptr;
  ccord_message g_m = {
    .id = (art_tail ? art_tail->id : 1800000000000000000ULL) + 100ULL,
    .channel_id = art,
    .author_id = chen,
    .timestamp_ms = (art_tail ? art_tail->timestamp_ms : (int64_t)F.ui->now_unix_ms) + 600000,
    .content = "Pixel Garden message from Chen",
  };
  CT_CHECK(ccord_store_append_message(F.store, &g_m, &err) == CCORD_OK);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(3);
  const vis_row *chen_row = nullptr;
  for (size_t i = 0; i < F.ui->nvis; i++) {
    if (F.ui->vis[i].msg_id == g_m.id) {
      chen_row = &F.ui->vis[i];
      break;
    }
  }
  CT_CHECK(chen_row != nullptr);
  if (chen_row) {
    ccord_rect cav = chen_row->avatar_rect;
    mouse(CCORD_IN_MOUSE_MOVE, 0, cav.x + cav.w * 0.5f, cav.y + cav.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().hover_user == chen);
    mouse(CCORD_IN_MOUSE_DOWN, 3, cav.x + cav.w * 0.5f, cav.y + cav.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().context_menu_open && F.ui->menu.ctx.target == CCORD_MENU_MEMBER && F.ui->menu.ctx.user_id == chen);
    CT_CHECK(menu_has("Profile") && menu_has("Mention") && menu_has("Message (Open DM)") && menu_has("Report User"));
    CT_CHECK(!menu_has("Open in Mod View") && !menu_has("Roles") && !menu_has("Change Nickname") &&
             !menu_has("Timeout Member") && !menu_has("Kick Member") && !menu_has("Ban Member"));
    key(CCORD_KEY_ESCAPE, 0);
    frame(1);
  }
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_interaction")) {
    fprintf(stderr, "ui interaction test: setup failed or no display driver; skipping\n");
    return 77;
  }
  test_markdown_tokens();
  test_markdown_pairing();
  test_load_and_virtualization();
  test_wheel_scroll();
  test_channel_click();
  test_server_click();
  test_dm_pseudo_guild();
  test_voice_and_stream();
  test_send();
  test_typing_throttle();
  test_context_menu();
  test_context_menu_scroll();
  test_message_actions();
  test_reply_pin_search_cards();
  test_request_budget_search_and_ack();
  test_request_budget_typing_and_channel_switch();
  test_settings_and_resize();
  test_extensions_switch();
  test_settings_categories();
  test_members_toggle();
  test_media_and_redirects();
  test_screenshot_pixels();
  test_reactions_autocomplete_attachments_and_login();
  test_dedicated_login_screen();
  test_category_grouping_and_media_throttling();
  test_dm_layout_ordering_and_pagination();
  test_expression_picker_and_stickers();
  test_forum_channel_ui_flow();
  test_picker_over_forum_clicks();
  test_vanilla_discord_gap_features();
  test_smooth_scroll_hover_anim_loading_and_restore();
  test_refused_writes_return_the_text();
  test_m0_pending_failed_retry_ui();
  test_permission_gated_menus();
  test_composer_locks_where_sending_is_not_allowed();
  test_admin_dialogs_call_discord();
  test_server_settings_overlay();
  test_member_list_follows_roles();
  test_hidden_channels();
  test_selection_is_one_band();
  test_transitions();
  test_notification_settings_ui();
  test_mark_as_read_ui();
  test_reading_follows_focus_and_scroll();
  test_lists_rebuild_only_when_they_change();
  test_member_list_pages_on_scroll();
  test_member_lookup_paths();
  test_dividers_new_bar_reply_jump_and_timestamps();
  test_embeds_galleries_spoilers_lightbox_and_media_players();
  test_embed_media_sources_video_and_retry();
  test_clicked_video_streams_and_closes_mid_open();
  test_polls_components_and_modals();
  test_ephemeral_slash_mentions_uploads_reply_ping_slowmode_and_drafts();
  test_search_panel_paginated_pins_threads_and_burst_reactions_ui();
  test_navigation_history_alt_shortcuts_and_inbox();
  test_m3b_folders_drag_add_join_and_discovery();
  test_m3c_group_dms_requests_profiles_and_custom_status();
  test_m4a_server_settings_safety_vanity_audit_preview_and_danger();
  test_m4b_expressions();
  test_m4c_members_prune_modview_and_automod();
  test_automod_large_csv_round_trip();
  test_m4c_integrations_and_community_ui();
  test_m4d_channel_settings_clone_and_voice_moderation();
  test_m6_devmode_copy_ids_and_animation_settings();
  test_expr_picker_fetch_and_emoji_rail();
  test_search_preserves_timeline();
  test_dm_friends_scroll_thumb();
  test_stage_tiles_scroll_thumb();
  test_call_bar_inset_and_cursor();
  test_copy_link_mark_unread_save();
  test_saved_messages_popover_and_persistence();
  test_forward_picker();
  test_tts_command_and_badge();
  test_crosspost_announcement();
  test_slash_subcommand_tree();
  test_context_menu_app_commands();
  test_membership_screening_modal_and_composer_lock();
  test_issue3_auto_ack_and_input_guards();
  test_dm_profile_right_click_menu();
  test_invite_preview_cards_and_url_opener();
  test_plugin_ui_panels();
  test_composer_upload_progress_indicator();
  test_plugin_nav_hooks_menus_and_aux_events();
  test_chat_message_vs_profile_right_click_and_admin_tools();
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_contrib_destroy(F.contrib);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
  remove(ui_test_file("settings.ini"));
  remove(ui_test_file("drafts.json"));
  return CT_RESULT("test_ui_interaction");
}
