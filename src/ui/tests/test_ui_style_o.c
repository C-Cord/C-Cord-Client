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

// WP-O of the shared style pass: popovers, modals, the context menu, the quick switcher, the autocomplete, the member
// popover and scheduled events draw through ui_style.c helpers. Two parts:
//  1. Each overlay is opened through the real UI code, the style audit is reset, one frame is drawn, and every audited
//     record must be on a token radius, height and font, with the frame / input / row / button kinds the overlay is made of.
//  2. A source scan of the overlay draw functions: a scrim, a frame ring, a bg_floating / bg_input fill or a literal corner
//     radius drawn by hand fails unless it is in the exact allowance table (with its reason). The rules are proven to fire
//     on synthetic code, so the scan cannot pass by being blind.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "ui_style.h"

#include "ccord_test.h"

// Picker tab indices (expression_picker.c EXPR_TAB_*).
#define TAB_GIFS 0
#define TAB_STICKERS 1
#define TAB_EMOJI 2

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

static void host_clip_set(void *ud, const char *s) { (void)ud; (void)s; }
static void host_text_input(void *ud, bool a, float x, float y, float w, float h)
{
  (void)ud; (void)a; (void)x; (void)y; (void)w; (void)h;
}
static void host_set_ext(void *ud, bool on) { (void)ud; (void)on; }
static bool host_open_url(void *ud, const char *url) { (void)ud; (void)url; return true; }
static bool host_open_file(void *ud, char *out, size_t cap) { (void)ud; (void)out; (void)cap; return false; }

// Draws n frames, each `ms` of virtual time after the last (the hover fades need 16 ms steps to read part-way values).
static void frame_step(int n, uint64_t ms)
{
  for (int i = 0; i < n; i++) {
    F.now += ms;
    (void)ccord_session_pump(F.session);
    (void)ccord_bus_drain(F.bus);
    (void)ccord_ui_update(F.ui, F.now);
    ccord_render_begin(F.renderer, F.theme.c.bg_primary);
    ccord_ui_draw(F.ui, F.renderer);
    ccord_render_end(F.renderer);
  }
}

static void frame(int n) { frame_step(n, 50); }

static void send(ccord_input_event ev) { (void)ccord_ui_handle_input(F.ui, &ev); }

static void mouse(ccord_input_type t, uint8_t button, float x, float y)
{
  send((ccord_input_event){.type = t, .button = button, .clicks = 1, .x = x, .y = y});
}

static void click_rect(ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  frame(2);
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_style_o_data"));
remove("ui_style_o_data/settings.ini");
  remove("ui_style_o_data/drafts.json");
  remove("ui_style_o_data");
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK)
    return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 10, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session) return false;
  ccord_platform_config pc = {.title = "ui-style-o-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .host = {.clipboard_set = host_clip_set, .text_input = host_text_input,
                              .set_extensions_enabled = host_set_ext, .open_url = host_open_url,
                              .open_file_dialog = host_open_file},
                     .fixed_now_unix_ms = 1793534400000LL + 30 * 60 * 1000};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  CT_CHECK(ccord_session_start(F.session, &err) == CCORD_OK);
  frame(3);
  return true;
}

static void teardown(void)
{
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
  ccord_contrib_destroy(F.contrib);
}

static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }

static const uis_audit *audited_frame(void)
{
  frame(3);
  uis_audit_reset();
  frame(1);
  return uis_audit_get();
}

// Every record of the last audited frame: theme radius (or a pill), token font for text kinds, token heights.
static void check_tokens(const char *tag)
{
  const uis_audit *a = uis_audit_get();
  uis_tokens t = uis_tokens_of(F.ui);
  CT_CHECK(a->kept == a->total);
  uint32_t bad = 0;
  for (uint32_t i = 0; i < a->kept; i++) {
    const uis_audit_record *e = &a->rec[i];
    bool pill = near(e->radius, e->rect.h * 0.5f) && (e->kind == UIS_K_CHIP || e->kind == UIS_K_TOGGLE || e->kind == UIS_K_SWATCH);
    bool rad_ok = near(e->radius, t.r_control) || near(e->radius, t.r_menu) || near(e->radius, t.r_surface) || pill ||
                  (e->kind == UIS_K_VDIVIDER && near(e->radius, 0.0f));
    bool text_kind = e->kind == UIS_K_BUTTON || e->kind == UIS_K_MENU_ROW || e->kind == UIS_K_TOOLTIP ||
                     e->kind == UIS_K_INPUT || e->kind == UIS_K_TAB || e->kind == UIS_K_CHIP || e->kind == UIS_K_SELECT ||
                     e->kind == UIS_K_RADIO;
    bool font_ok = text_kind ? (near(e->font_px, t.fs_body) || near(e->font_px, t.fs_control) || near(e->font_px, t.fs_caption))
                             : near(e->font_px, 0.0f);
    bool h_ok = true;
    switch (e->kind) {
      case UIS_K_BUTTON:
        h_ok = near(e->rect.h, UIS_H_BTN_SM) || near(e->rect.h, UIS_H_BTN) || near(e->rect.h, UIS_H_BTN_LG);
        break;
      case UIS_K_INPUT: h_ok = near(e->rect.h, UIS_H_INPUT); break;
      case UIS_K_ICON_BUTTON: h_ok = near(e->rect.w, UIS_ICON_BTN) && near(e->rect.h, UIS_ICON_BTN); break;
      case UIS_K_TAB: h_ok = near(e->rect.h, UIS_H_BTN_SM); break;
      case UIS_K_MENU_ROW: h_ok = near(e->rect.h, t.menu_row_h); break;
      default: break;
    }
    if (!rad_ok || !font_ok || !h_ok) {
      fprintf(stderr, "%s: %s at %.0f,%.0f %.0fx%.0f radius %.2f font %.2f is off the tokens\n", tag, uis_kind_name(e->kind),
              (double)e->rect.x, (double)e->rect.y, (double)e->rect.w, (double)e->rect.h, (double)e->radius, (double)e->font_px);
      bad++;
    }
  }
  CT_CHECK(bad == 0);
}

static void close_all(void)
{
  send((ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_ESCAPE});
  F.ui->search_open = F.ui->pins_open = F.ui->inbox_open = F.ui->shortcuts_open = F.ui->quick_switcher_open = false;
  F.ui->member_popover_open = false;
  F.ui->modal_kind = UI_MODAL_NONE;
  ui_menu_close(F.ui);
  frame(12);
}

// What an overlay must draw through the helpers: a minimum per kind (0 = unchecked).
typedef struct need {
  uint32_t popover, modal, input, button, tab, menu_row, list_row, icon_button;
} need;

static void check_need(const char *name, const uis_audit *a, need n)
{
  const struct {
    uis_kind k;
    uint32_t want;
  } rows[] = {{UIS_K_POPOVER, n.popover}, {UIS_K_MODAL, n.modal}, {UIS_K_INPUT, n.input}, {UIS_K_BUTTON, n.button},
              {UIS_K_TAB, n.tab}, {UIS_K_MENU_ROW, n.menu_row}, {UIS_K_LIST_ROW, n.list_row}, {UIS_K_ICON_BUTTON, n.icon_button}};
  for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
    if (a->kind[rows[i].k].count < rows[i].want) {
      fprintf(stderr, "%s: %s drawn %u times through the helpers, expected at least %u\n", name, uis_kind_name(rows[i].k),
              (unsigned)a->kind[rows[i].k].count, (unsigned)rows[i].want);
      CT_CHECK(0);
    }
  }
  // Frames follow the theme: popover bg_floating, modal bg_secondary, both at the surface radius.
  uis_tokens t = uis_tokens_of(F.ui);
  if (n.popover) CT_CHECK(near(a->kind[UIS_K_POPOVER].radius, t.r_surface));
  if (n.modal) {
    CT_CHECK(near(a->kind[UIS_K_MODAL].radius, t.r_surface));
    ccord_rect m = a->kind[UIS_K_MODAL].rect;
    CT_CHECK(m.x >= UIS_SP_8 - 0.5f && m.y >= UIS_SP_8 - 0.5f && m.x + m.w <= F.ui->win_w - UIS_SP_8 + 0.5f &&
             m.y + m.h <= F.ui->win_h - UIS_SP_8 + 0.5f);
  }
  check_tokens(name);
}

static void test_runtime(void)
{
  ccord_error err = {0};
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  ccord_id general = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, g); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, g, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && strcmp(c->name, "general") == 0) general = c->id;
  }
  CT_CHECK(general != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, general, &err) == CCORD_OK);
  frame(4);

  click_rect(ui_search_rect(F.ui));
  CT_CHECK(F.ui->search_open);
  check_need("search", audited_frame(), (need){.popover = 1, .input = 1});
  close_all();

  click_rect(ui_pin_rect(F.ui));
  CT_CHECK(F.ui->pins_open);
  check_need("pins", audited_frame(), (need){.popover = 1});
  close_all();

  click_rect(ui_inbox_rect(F.ui));
  CT_CHECK(F.ui->inbox_open);
  check_need("inbox", audited_frame(), (need){.popover = 1, .tab = 2, .button = 1});
  close_all();

  F.ui->modal_kind = UI_MODAL_ADD_FRIEND;
  F.ui->dirty = true;
  check_need("add-friend", audited_frame(), (need){.modal = 1, .input = 1, .button = 2});
  close_all();

  F.ui->modal_kind = UI_MODAL_ADD_SERVER;
  F.ui->modal_type_sel = 0;
  F.ui->dirty = true;
  check_need("add-server", audited_frame(), (need){.modal = 1, .input = 1, .button = 3, .tab = 2});
  close_all();

  F.ui->modal_kind = UI_MODAL_FOLDER_SETTINGS;
  F.ui->dirty = true;
  check_need("folder", audited_frame(), (need){.modal = 1, .input = 2, .button = 2});
  close_all();

  F.ui->shortcuts_open = true;
  F.ui->dirty = true;
  check_need("shortcuts", audited_frame(), (need){.modal = 1, .icon_button = 1});
  close_all();

  F.ui->quick_switcher_open = true;
  ccord_text_edit_clear(&F.ui->quick_switcher_edit);
  ui_quick_switcher_reset_list(F.ui);
  F.ui->dirty = true;
  check_need("quick-switcher", audited_frame(), (need){.modal = 1, .input = 1, .list_row = 1});
  close_all();

  F.ui->member_popover_open = true;
  F.ui->member_popover_user_id = ccord_store_self_id(F.store);
  F.ui->member_popover_tab = 0;
  F.ui->member_popover_rect = (ccord_rect){700.0f, 120.0f, 300.0f, 320.0f};   // as the member list click places it
  F.ui->dirty = true;
  check_need("member-popover", audited_frame(), (need){.popover = 1, .button = 3});
  close_all();

  ccord_menu_context ctx = {.target = CCORD_MENU_GUILD, .guild_id = g};
  ui_menu_open(F.ui, 420.0f, 260.0f, &ctx);
  CT_CHECK(F.ui->menu.open);
  check_need("context-menu", audited_frame(), (need){.popover = 1, .menu_row = 3});
  close_all();
}

// ---- part 2: the source scan ------------------------------------------------------------------------------------------

enum rule { R_SCRIM, R_RING, R_FILL, R_RADIUS, R_COUNT };
static const char *const k_rule_name[R_COUNT] = {"hand-drawn scrim (COL(overlay))", "hand-drawn frame ring (COL(border_subtle))",
                                                 "hand-drawn bg_floating / bg_input fill", "literal corner radius"};

static void strip_comments(char *s)
{
  for (char *p = s; *p; p++) {
    if (*p == '"' || *p == '\'') {
      char q = *p++;
      while (*p && *p != q) {
        if (*p == '\\' && p[1]) p++;
        p++;
      }
      if (!*p) break;
    } else if (p[0] == '/' && p[1] == '/') {
      while (*p && *p != '\n') *p++ = ' ';
      if (!*p) break;
    } else if (p[0] == '/' && p[1] == '*') {
      *p++ = ' ';
      *p++ = ' ';
      while (*p && !(p[0] == '*' && p[1] == '/')) {
        if (*p != '\n') *p = ' ';
        p++;
      }
      if (!*p) break;
      p[0] = ' ';
      p[1] = ' ';
      p++;
    }
  }
}

static char *read_file(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)n + 1u);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
    free(b);
    fclose(f);
    return nullptr;
  }
  b[n] = '\0';
  fclose(f);
  return b;
}

static bool has(const char *s, size_t n, const char *needle)
{
  size_t m = strlen(needle);
  for (size_t i = 0; i + m <= n; i++) {
    if (memcmp(s + i, needle, m) == 0) return true;
  }
  return false;
}

// Counts the hand-drawn pieces in code[0..n): every ccord_draw_rect / ccord_draw_rrect call, its balanced arguments.
static void scan_code(const char *code, size_t n, int count[R_COUNT])
{
  for (size_t i = 0; i + 16 < n; i++) {
    bool rrect = strncmp(code + i, "ccord_draw_rrect(", 17) == 0;
    bool rect = !rrect && strncmp(code + i, "ccord_draw_rect(", 16) == 0;
    if ((!rrect && !rect) || (i > 0 && (isalnum((unsigned char)code[i - 1]) || code[i - 1] == '_'))) continue;
    size_t p = i + (rrect ? 17u : 16u);
    size_t start = p;
    int depth = 1, arg = 0;
    size_t arg_start[8] = {start};
    while (p < n && depth > 0) {
      char c = code[p];
      if (c == '(' || c == '{' || c == '[') depth++;
      else if (c == ')' || c == '}' || c == ']') depth--;
      else if (c == ',' && depth == 1 && arg < 7) arg_start[++arg] = p + 1;
      p++;
    }
    size_t len = p - start;
    const char *call = code + start;
    if (has(call, len, "COL(overlay)")) count[R_SCRIM]++;
    if (has(call, len, "COL(border_subtle)") && rrect) count[R_RING]++;
    if (rrect && (has(call, len, "COL(bg_floating)") || has(call, len, "COL(bg_input)"))) count[R_FILL]++;
    if (rrect && arg >= 2) {
      const char *a = code + arg_start[2];
      while (*a == ' ' || *a == '\n') a++;
      // A radius argument that starts with a number is a literal (a bare 0 is a square corner).
      if (isdigit((unsigned char)*a) && strtod(a, nullptr) != 0.0) count[R_RADIUS]++;
    }
  }
}

// The body of the top-level function `name` (from its signature line to the closing brace at column 0), or false.
static bool function_body(const char *src, const char *name, const char **begin, size_t *len)
{
  char needle[128];
  snprintf(needle, sizeof needle, " %s(", name);
  const char *p = src;
  while ((p = strstr(p, needle)) != nullptr) {
    const char *ls = p;
    while (ls > src && ls[-1] != '\n') ls--;
    if (ls == p + 1 - 1 || (*ls != ' ' && *ls != '\t' && *ls != '/')) {
      const char *end = strstr(p, "\n}\n");
      const char *open = strchr(p, '{');
      const char *semi = strchr(p, ';');
      if (end && open && (!semi || open < semi)) {
        *begin = ls;
        *len = (size_t)(end - ls) + 2u;
        return true;
      }
    }
    p++;
  }
  return false;
}

typedef struct allow {
  const char *file;
  const char *func;
  int count[R_COUNT];
  const char *why;
} allow;

// Exact allowances. A function that draws one more (or one fewer) raw piece than listed fails, so a new raw frame, scrim,
// fill or literal radius cannot slip in, and a removed one must lower its line.
static const allow k_funcs[] = {
  {"overlays/header_popovers.c", "ui_draw_search_popover", {0}, ""},
  {"overlays/header_popovers.c", "ui_draw_pins_popover", {0}, ""},
  {"overlays/header_popovers.c", "ui_draw_inbox_popover", {0, 0, 0, 2}, "mention and unread count pills (radius 9, round badges)"},
  {"overlays/header_popovers.c", "ui_draw_shortcuts_modal", {0}, ""},
  {"chat/composer_autocomplete.c", "ui_draw_autocomplete", {0}, ""},
  {"voice/stream_viewer.c", "ui_draw_go_live", {0, 0, 0, 3}, "source thumbnail placeholder, audio checkbox, thumb tile"},
  {"overlays/modals_draw.c", "draw_group_dm_modal", {0, 0, 0, 4}, "recipient check box shapes"},
  {"overlays/modals_draw.c", "draw_edit_group_dm_modal", {0}, ""},
  {"overlays/profile_surfaces.c", "dc_draw_user_profile_modal", {0, 0, 0, 2}, "bot tag and server icon shapes inside the profile card"},
  {"overlays/modals_draw.c", "ui_draw_overlays", {0, 0, 1, 6},
   "multi-line interaction-modal field (no shared helper for it yet); select-dropdown checkbox and dot glyphs; invite preview icon"},
  {"overlays/context_menu_actions.c", "draw_panel", {0}, ""},
  {"voice/ui_scheduled_events.c", "ev_modal_begin", {0}, ""},
  {"voice/ui_scheduled_events.c", "ev_input", {0}, ""},
  {"voice/ui_scheduled_events.c", "ev_button", {0}, ""},
};

static void scan_function(const char *file, const char *src, const allow *al, bool verbose)
{
  const char *b;
  size_t len;
  if (!function_body(src, al->func, &b, &len)) {
    fprintf(stderr, "%s: function %s not found: update k_funcs\n", file, al->func);
    CT_CHECK(0);
    return;
  }
  int c[R_COUNT] = {0};
  scan_code(b, len, c);
  for (int r = 0; r < R_COUNT; r++) {
    if (verbose || c[r] != al->count[r]) {
      if (c[r] != al->count[r])
        fprintf(stderr, "%s %s: %d %s, %d allowed%s\n", file, al->func, c[r], k_rule_name[r], al->count[r],
                c[r] > al->count[r] ? ": draw it through a ui_style.c helper" : ": lower the allowance in k_funcs");
      else
        fprintf(stderr, "%s %s: %d %s\n", file, al->func, c[r], k_rule_name[r]);
    }
    if (c[r] != al->count[r]) CT_CHECK(0);
  }
}

static void test_source_scan(void)
{
  const bool verbose = getenv("CCORD_UI_STYLE_VERBOSE") != nullptr;
  const char *files[] = {
    "overlays/header_popovers.c",
    "chat/composer_autocomplete.c",
    "voice/stream_viewer.c",
    "overlays/modals_draw.c",
    "overlays/profile_surfaces.c",
    "overlays/context_menu_actions.c",
    "voice/ui_scheduled_events.c",
  };
  for (size_t f = 0; f < sizeof files / sizeof files[0]; f++) {
    char path[1024];
    snprintf(path, sizeof path, "%s/src/ui/%s", CCORD_SOURCE_DIR, files[f]);
    char *src = read_file(path);
    if (!src) {
      fprintf(stderr, "cannot read %s\n", path);
      CT_CHECK(0);
      continue;
    }
    strip_comments(src);
    for (size_t i = 0; i < sizeof k_funcs / sizeof k_funcs[0]; i++) {
      if (strcmp(k_funcs[i].file, files[f]) == 0) scan_function(files[f], src, &k_funcs[i], verbose);
    }
    free(src);
  }
}

// The rules must fire on raw overlay code and stay quiet on helper calls.
static void test_rules_fire(void)
{
  const struct {
    const char *code;
    int want[R_COUNT];
  } cases[] = {
    {"ccord_draw_rect(r, (ccord_rect){0, 0, w, h}, COL(overlay));", {1, 0, 0, 0}},
    {"ccord_draw_rrect(r, (ccord_rect){m.x - 1.0f, m.y - 1.0f, m.w + 2.0f, m.h + 2.0f}, 9.0f, COL(border_subtle));", {0, 1, 0, 1}},
    {"ccord_draw_rrect(r, box, M(context_menu_radius), COL(bg_floating));", {0, 0, 1, 0}},
    {"ccord_draw_rrect(r, inp, tk.r_control, COL(bg_input));", {0, 0, 1, 0}},
    {"ccord_draw_rrect(r, ok_r, 4.0f, COL(brand));", {0, 0, 0, 1}},
    {"ccord_draw_rrect(r, ok_r,\n 4, COL(brand));", {0, 0, 0, 1}},
    {"ccord_draw_rrect(r, ok_r, tk.r_control, COL(brand));", {0, 0, 0, 0}},
    {"ccord_draw_rrect(r, ok_r, 0.0f, COL(brand));", {0, 0, 0, 0}},
    {"uis_popover_frame(ui, r, box); uis_button(ui, r, rc, \"OK\", UIS_BTN_PRIMARY, UIS_BTN_S_MD, st, \"k\", 0);", {0, 0, 0, 0}},
    {"// ccord_draw_rrect(r, ok_r, 4.0f, COL(brand));", {0, 0, 0, 0}},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    char *code = strdup(cases[i].code);
    strip_comments(code);
    int c[R_COUNT] = {0};
    scan_code(code, strlen(code), c);
    for (int r = 0; r < R_COUNT; r++) {
      if (c[r] != cases[i].want[r]) fprintf(stderr, "case %zu: rule %s fired %d, want %d\n", i, k_rule_name[r], c[r], cases[i].want[r]);
      CT_CHECK(c[r] == cases[i].want[r]);
    }
    free(code);
  }
}

int main(int argc, char **argv)
{
  test_rules_fire();
  test_source_scan();
  if (!setup(argc > 0 ? argv[0] : "test_ui_style_o")) return 77;
  test_runtime();
  teardown();
  return CT_RESULT("test_ui_style_o");
}
