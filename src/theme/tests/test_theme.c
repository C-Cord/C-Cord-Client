// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>

#include <ccord/theme/theme.h>

#include "ccord_test.h"

#define ASSET CCORD_SOURCE_DIR "/assets/themes/c-cord-dark.theme"

static const char *valid_min =
  "[meta]\nid = t\nname = T\n[colors]\n"
#define C(n) #n " = #000000\n"
  CCORD_THEME_COLORS(C)
#undef C
  "[metrics]\n"
#define M(n) #n " = 1\n"
  CCORD_THEME_METRICS(M)
#undef M
  "[fonts]\nfont_regular = a.ttf\nfont_bold = b.otf\n";

static void test_color_parse(void)
{
  ccord_color c;
  CT_CHECK(ccord_color_parse("#5865f2", &c));
  CT_CHECK(c.r == 0x58 && c.g == 0x65 && c.b == 0xf2 && c.a == 255);
  CT_CHECK(ccord_color_parse("#00000099", &c) && c.a == 0x99);
  CT_CHECK(!ccord_color_parse("5865f2", &c));
  CT_CHECK(!ccord_color_parse("#5865f", &c));
  CT_CHECK(!ccord_color_parse("#5865gg", &c));
  CT_CHECK(!ccord_color_parse(nullptr, &c));
}

static void test_parse_asset(void)
{
  ccord_theme t;
  ccord_error err;
  ccord_error_clear(&err);
  CT_CHECK(ccord_theme_load(&t, ASSET, &err) == CCORD_OK);
  if (err.code != CCORD_OK) fprintf(stderr, "asset: %s\n", err.message);
  CT_CHECK(strcmp(t.id, "c-cord-dark") == 0);
  CT_CHECK(t.c.bg_primary.r == 0x24 && t.c.bg_primary.g == 0x24 && t.c.bg_primary.b == 0x2a);
  CT_CHECK(t.m.channel_list_width == 236.0f);
  CT_CHECK(t.m.line_height_factor == 1.375f);
  CT_CHECK(strcmp(t.font_regular, "fonts/NotoSans-Regular.ttf") == 0);
}

static void test_parse_valid_minimal(void)
{
  ccord_theme t;
  ccord_error err;
  CT_CHECK(ccord_theme_parse(&t, valid_min, strlen(valid_min), &err) == CCORD_OK);
  CT_CHECK(strcmp(t.font_bold, "b.otf") == 0);
}

static void expect_fail(const char *text, const char *label)
{
  ccord_theme t;
  ccord_error err;
  ccord_status st = ccord_theme_parse(&t, text, strlen(text), &err);
  if (st == CCORD_OK) fprintf(stderr, "expected failure: %s\n", label);
  CT_CHECK(st == CCORD_ERR_PARSE);
}

static void test_negative(void)
{
  expect_fail("[meta]\nid = t\nname = T\n", "missing colors");
  expect_fail("[meta]\nid = t\nname = T\n[colors]\nbogus = #000000\n", "unknown colour");
  expect_fail("[meta]\nid = t\nname = T\n[fonts]\nfont_regular = /abs.ttf\n", "absolute font");
  expect_fail("[meta]\nid = t\nname = T\n[fonts]\nfont_regular = ../x.ttf\n", "traversal font");
  expect_fail("[meta]\nid = t\nname = T\n[fonts]\nfont_regular = x.woff\n", "bad font ext");
  expect_fail("[meta]\nid = t\nname = T\n[metrics]\nheader_height = -1\n", "negative metric");
  expect_fail("[meta]\nid = t\nname = T\n[metrics]\nheader_height = 5000\n", "metric over 4096");
  expect_fail("[meta]\nid = t\nname = T\n[metrics]\nheader_height = 1e3\n", "exponent metric");
  expect_fail("[meta]\nid = t\nname = T\n[bogus]\nx = 1\n", "unknown section");
}

static void test_load_missing(void)
{
  ccord_theme t;
  ccord_error err;
  CT_CHECK(ccord_theme_load(&t, CCORD_SOURCE_DIR "/no-such-file.theme", &err) == CCORD_ERR_IO);
}

static const char *const builtin_ids[] = {"c-cord-dark", "c-cord-light", "c-cord-amoled"};

static void test_builtin_matches_disk(void)
{
  for (size_t i = 0; i < sizeof builtin_ids / sizeof builtin_ids[0]; i++) {
    char path[512];
    (void)snprintf(path, sizeof path, CCORD_SOURCE_DIR "/assets/themes/%s.theme", builtin_ids[i]);
    ccord_theme disk, emb;
    ccord_error err;
    CT_CHECK(ccord_theme_load(&disk, path, &err) == CCORD_OK);
    CT_CHECK(ccord_theme_load_builtin(&emb, builtin_ids[i], &err) == CCORD_OK);
    CT_CHECK(strcmp(emb.id, builtin_ids[i]) == 0);
    CT_CHECK(memcmp(&disk, &emb, sizeof disk) == 0);   // embedded copy must be byte-identical to the asset file
  }
}

static void test_builtin_switch_in_place(void)
{
  ccord_theme t, fresh;
  ccord_error err;
  CT_CHECK(ccord_theme_load_builtin(&t, "c-cord-amoled", &err) == CCORD_OK);
  CT_CHECK(t.c.bg_primary.r == 0 && t.c.bg_primary.g == 0 && t.c.bg_primary.b == 0);
  CT_CHECK(ccord_theme_load_builtin(&t, "c-cord-dark", &err) == CCORD_OK);
  CT_CHECK(ccord_theme_load_builtin(&fresh, "c-cord-dark", &err) == CCORD_OK);
  CT_CHECK(memcmp(&t, &fresh, sizeof t) == 0);   // no stale id/name/colour bytes from the previous theme
  CT_CHECK(t.c.bg_primary.r == 0x24);
  CT_CHECK(ccord_theme_load_builtin(&t, "c-cord-light", &err) == CCORD_OK);
  CT_CHECK(t.c.bg_primary.r == 0xfa && t.c.text_normal.r == 0x27);
  CT_CHECK(ccord_theme_load_builtin(&t, "c-cord-amoled", &err) == CCORD_OK);
  CT_CHECK(ccord_theme_load_builtin(&fresh, "c-cord-amoled", &err) == CCORD_OK);
  CT_CHECK(memcmp(&t, &fresh, sizeof t) == 0);
}

static void test_builtin_errors(void)
{
  ccord_theme t;
  ccord_error err;
  CT_CHECK(ccord_theme_load_builtin(&t, "no-such-theme", &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_theme_load_builtin(nullptr, "c-cord-dark", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_theme_load_builtin(&t, nullptr, &err) == CCORD_ERR_INVALID);
  size_t len = 0;
  CT_CHECK(ccord_embedded_theme_by_id("c-cord-light", &len) != nullptr && len > 0);
  CT_CHECK(ccord_embedded_theme_by_id("no-such-theme", &len) == nullptr);
  CT_CHECK(ccord_embedded_theme_by_id(nullptr, &len) == nullptr);
  CT_CHECK(ccord_embedded_theme_by_id("c-cord-dark", nullptr) != nullptr);
}

// Full schema with one overridable font path and one overridable metric value.
static ccord_status parse_variant(const char *font_rel, const char *metric, const char *value, ccord_theme *out)
{
  char text[4096];
  size_t off = 0;
  off += (size_t)snprintf(text + off, sizeof text - off, "[meta]\nid = t\nname = T\n[colors]\n");
#define C(n) off += (size_t)snprintf(text + off, sizeof text - off, "%s = #000000\n", #n);
  CCORD_THEME_COLORS(C)
#undef C
  off += (size_t)snprintf(text + off, sizeof text - off, "[metrics]\n");
#define M(n) off += (size_t)snprintf(text + off, sizeof text - off, "%s = %s\n", #n, strcmp(#n, metric) == 0 ? value : "1");
  CCORD_THEME_METRICS(M)
#undef M
  off += (size_t)snprintf(text + off, sizeof text - off, "[fonts]\nfont_regular = %s\nfont_bold = b.otf\n", font_rel);
  ccord_error err;
  return ccord_theme_parse(out, text, off, &err);
}

static void test_font_paths(void)
{
  ccord_theme t;
  CT_CHECK(parse_variant("x.ttc", "header_height", "1", &t) == CCORD_OK);
  CT_CHECK(strcmp(t.font_regular, "x.ttc") == 0);
  CT_CHECK(parse_variant("x.woff", "header_height", "1", &t) == CCORD_ERR_PARSE);
  char rel[160];
  memset(rel, 'a', 123);
  memcpy(rel + 123, ".ttf", 5);   // 127 bytes: the longest allowed path
  CT_CHECK(parse_variant(rel, "header_height", "1", &t) == CCORD_OK);
  memset(rel, 'a', 124);
  memcpy(rel + 124, ".ttf", 5);   // 128 bytes: one too many
  CT_CHECK(parse_variant(rel, "header_height", "1", &t) == CCORD_ERR_PARSE);
}

static void test_metric_boundary(void)
{
  ccord_theme t;
  CT_CHECK(parse_variant("a.ttf", "header_height", "4096", &t) == CCORD_OK);
  CT_CHECK(t.m.header_height == 4096.0f);
  CT_CHECK(parse_variant("a.ttf", "header_height", "4096.01", &t) == CCORD_ERR_PARSE);
  CT_CHECK(parse_variant("a.ttf", "message_merge_ms", "3600000", &t) == CCORD_OK);
  CT_CHECK(t.m.message_merge_ms == 3600000.0f);
  CT_CHECK(parse_variant("a.ttf", "message_merge_ms", "3600001", &t) == CCORD_ERR_PARSE);
}

static bool color_is(ccord_color c, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
  return c.r == r && c.g == g && c.b == b && c.a == a;
}

// Loads a fresh built-in so each QuickCSS assertion starts from known theme bytes.
static bool fresh_dark(ccord_theme *t)
{
  ccord_error err;
  return ccord_theme_load_builtin(t, "c-cord-dark", &err) == CCORD_OK;
}

static void test_quick_css_basic(void)
{
  ccord_theme t;
  ccord_error err;
  size_t applied = 999;
  CT_CHECK(fresh_dark(&t));
  ccord_color old_secondary = t.c.bg_secondary;
  const char *css =
    "/* Custom Theme Overrides */\n"
    ":root {\n"
    "  --background-primary: #112233;\n"
    "  --text-normal: rgb(240, 241, 242);\n"
    "  --brand-experiment: #5865f2 !important;\n"
    "}\n";
  CT_CHECK(ccord_theme_apply_quick_css(&t, css, strlen(css), &applied, &err) == CCORD_OK);
  CT_CHECK(applied == 3);
  CT_CHECK(color_is(t.c.bg_primary, 0x11, 0x22, 0x33, 255));
  CT_CHECK(color_is(t.c.text_normal, 240, 241, 242, 255));
  CT_CHECK(color_is(t.c.brand, 0x58, 0x65, 0xf2, 255));
  CT_CHECK(color_is(t.c.bg_secondary, old_secondary.r, old_secondary.g, old_secondary.b, old_secondary.a));
  CT_CHECK(strcmp(t.id, "c-cord-dark") == 0 && strcmp(t.name, "Matte Graphite") == 0);
}

static void test_quick_css_aliases(void)
{
  ccord_theme t;
  ccord_error err;
  size_t applied = 0;
  CT_CHECK(fresh_dark(&t));
  float old_width = t.m.channel_list_width;
  const char *css =
    "--ccord-bg-secondary: #445566;\n"
    "--ccord-bg-tertiary: #778899;\n"
    "--channel_list_width: 300px;\n"
    "--ccord-text-link: rgba(10, 20, 30, 0.5);\n"
    "--bg-primary: #010203;\n";
  CT_CHECK(ccord_theme_apply_quick_css(&t, css, strlen(css), &applied, &err) == CCORD_OK);
  CT_CHECK(applied == 5);
  CT_CHECK(color_is(t.c.bg_secondary, 0x44, 0x55, 0x66, 255));
  CT_CHECK(color_is(t.c.bg_tertiary, 0x77, 0x88, 0x99, 255));
  CT_CHECK(color_is(t.c.text_link, 10, 20, 30, 128));
  CT_CHECK(color_is(t.c.bg_primary, 1, 2, 3, 255));
  CT_CHECK(t.m.channel_list_width == 300.0f && old_width != 300.0f);
}

// Valid rules before and after malformed declarations, comments and unsupported selectors must apply.
static void test_quick_css_skips_bad(void)
{
  ccord_theme t;
  ccord_error err;
  size_t applied = 0;
  CT_CHECK(fresh_dark(&t));
  ccord_color old_primary = t.c.bg_primary;
  ccord_color old_muted = t.c.text_muted;
  ccord_color old_secondary = t.c.bg_secondary;
  ccord_color old_brand = t.c.brand;
  float old_scrollbar = t.m.scrollbar_width;
  const char *css =
    ":root {\n"
    "  --background-primary: #5865f;\n"
    "  --text-muted: ;\n"
    "  --text-normal: #abc;\n"
    "}\n"
    ".sidebar { --background-secondary: #000000; }\n"
    "--brand-experiment #123456;\n"
    ":root, body { --background-tertiary: #123456; }\n"
    "garbage;\n"
    "* { --header-primary: rgb(1, 2, 3); --header-secondary: #12345; }\n"
    ":root { --background-floating: #abcdef!important; --scrollbar-width: 12em; }\n"
    "/* a comment */ body { --text-link: /* inline */ #0a0b0c; }\n"
    "/* unterminated comment swallows the rest --danger: #ffffff;";
  CT_CHECK(ccord_theme_apply_quick_css(&t, css, strlen(css), &applied, &err) == CCORD_OK);
  CT_CHECK(applied == 5);
  CT_CHECK(color_is(t.c.bg_primary, old_primary.r, old_primary.g, old_primary.b, old_primary.a));
  CT_CHECK(color_is(t.c.text_muted, old_muted.r, old_muted.g, old_muted.b, old_muted.a));
  CT_CHECK(color_is(t.c.bg_secondary, old_secondary.r, old_secondary.g, old_secondary.b, old_secondary.a));
  CT_CHECK(color_is(t.c.brand, old_brand.r, old_brand.g, old_brand.b, old_brand.a));
  CT_CHECK(color_is(t.c.text_normal, 0xaa, 0xbb, 0xcc, 255));
  CT_CHECK(color_is(t.c.bg_tertiary, 0x12, 0x34, 0x56, 255));
  CT_CHECK(color_is(t.c.text_header, 1, 2, 3, 255));
  CT_CHECK(color_is(t.c.bg_floating, 0xab, 0xcd, 0xef, 255));
  CT_CHECK(color_is(t.c.text_link, 0x0a, 0x0b, 0x0c, 255));
  CT_CHECK(t.m.scrollbar_width == old_scrollbar);
}

static void test_quick_css_color_forms(void)
{
  ccord_theme t;
  ccord_error err;
  size_t applied = 0;
  CT_CHECK(fresh_dark(&t));
  const char *css =
    ":root { --danger: #f00; --status-online: #0f08; --status-idle: #00ff00; }\n"
    ".theme-light { --background-primary: #00000080; }\n"
    "body { --text-secondary: rgba(255, 128, 0, 1); }\n";
  CT_CHECK(ccord_theme_apply_quick_css(&t, css, strlen(css), &applied, &err) == CCORD_OK);
  CT_CHECK(applied == 5);
  CT_CHECK(color_is(t.c.danger, 255, 0, 0, 255));
  CT_CHECK(color_is(t.c.status_online, 0, 255, 0, 0x88));
  CT_CHECK(color_is(t.c.status_idle, 0, 255, 0, 255));
  CT_CHECK(color_is(t.c.bg_primary, 0, 0, 0, 0x80));
  CT_CHECK(color_is(t.c.text_secondary, 255, 128, 0, 255));
}

static void test_quick_css_idempotent_and_args(void)
{
  ccord_theme t, once;
  ccord_error err;
  size_t applied = 0;
  const char *css = ":root { --background-primary: #112233; --scrollbar-width: 8.5px; }";
  CT_CHECK(fresh_dark(&t));
  CT_CHECK(ccord_theme_apply_quick_css(&t, css, strlen(css), &applied, &err) == CCORD_OK);
  CT_CHECK(applied == 2);
  once = t;
  CT_CHECK(ccord_theme_apply_quick_css(&t, css, strlen(css), &applied, &err) == CCORD_OK);
  CT_CHECK(applied == 2 && memcmp(&t, &once, sizeof t) == 0);
  CT_CHECK(t.m.scrollbar_width == 8.5f);

  applied = 99;
  CT_CHECK(ccord_theme_apply_quick_css(&t, "/* only a comment */", strlen("/* only a comment */"), &applied, &err) == CCORD_OK);
  CT_CHECK(applied == 0);
  CT_CHECK(ccord_theme_apply_quick_css(nullptr, css, strlen(css), &applied, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_theme_apply_quick_css(&t, nullptr, 1, &applied, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_theme_apply_quick_css(&t, nullptr, 0, &applied, &err) == CCORD_OK && applied == 0);
}

static void test_plugin_css_json_and_reflection(void)
{
  ccord_theme t;
  ccord_error err;
  size_t applied = 0;
  CT_CHECK(fresh_dark(&t));

  // Surface-scoped CSS rules + custom properties + !important + comma-separated selectors.
  const char *pcss =
    ":root { --brand-experiment: #ff3366 !important; }\n"
    ".chat { background-color: #101218; color: rgb(230, 235, 240); }\n"
    ".sidebar { background: #161820; width: 268px; }\n"
    ".composer { background-color: #202430; border-radius: 14px; min-height: 48; }\n"
    ".header { height: 54px; border-color: #303440; }\n"
    ".modal { background: #0e1015; border-color: #404450; }\n"
    ".user-panel { background: #12141a; height: 60px; --status-online: #22cc66; }\n"
    ".unknown-surface { background: #ffffff; }\n";
  CT_CHECK(ccord_theme_apply_plugin_css(&t, pcss, strlen(pcss), &applied, &err) == CCORD_OK);
  CT_CHECK(applied == 15);
  CT_CHECK(color_is(t.c.brand, 0xff, 0x33, 0x66, 255));
  CT_CHECK(color_is(t.c.bg_primary, 0x10, 0x12, 0x18, 255));
  CT_CHECK(color_is(t.c.text_normal, 230, 235, 240, 255));
  CT_CHECK(color_is(t.c.bg_secondary, 0x16, 0x18, 0x20, 255));
  CT_CHECK(t.m.channel_list_width == 268.0f);
  CT_CHECK(color_is(t.c.bg_input, 0x20, 0x24, 0x30, 255));
  CT_CHECK(t.m.composer_radius == 14.0f && t.m.composer_min_height == 48.0f);
  CT_CHECK(t.m.header_height == 54.0f && color_is(t.c.divider, 0x30, 0x34, 0x40, 255));
  CT_CHECK(color_is(t.c.bg_floating, 0x0e, 0x10, 0x15, 255));
  CT_CHECK(color_is(t.c.border_subtle, 0x40, 0x44, 0x50, 255));
  CT_CHECK(color_is(t.c.bg_user_panel, 0x12, 0x14, 0x1a, 255));
  CT_CHECK(t.m.user_panel_height == 60.0f);
  CT_CHECK(color_is(t.c.status_online, 0x22, 0xcc, 0x66, 255));

  // JSON color + metric patching (unknown/invalid values safely skipped).
  CT_CHECK(fresh_dark(&t));
  const char *cjson = "{\"bg_primary\":\"#1a1b26\",\"text-link\":\"rgba(122,162,247,0.5)\",\"bogus\":\"#fff\",\"brand\":123}";
  const char *mjson = "{\"channel_list_width\":275,\"header-height\":52,\"bogus\":10,\"server_list_width\":-5}";
  CT_CHECK(ccord_theme_apply_json(&t, cjson, mjson, &applied, &err) == CCORD_OK);
  CT_CHECK(applied == 4);
  CT_CHECK(color_is(t.c.bg_primary, 0x1a, 0x1b, 0x26, 255));
  CT_CHECK(color_is(t.c.text_link, 122, 162, 247, 128));
  CT_CHECK(t.m.channel_list_width == 275.0f && t.m.header_height == 52.0f);
  CT_CHECK(ccord_theme_apply_json(nullptr, cjson, mjson, &applied, &err) == CCORD_ERR_INVALID);

  // Color & metric token reflection (underscore + hyphen normalization + range validation).
  CT_CHECK(ccord_theme_color_count() > 20 && ccord_theme_color_name(0) != nullptr);
  CT_CHECK(ccord_theme_color_name(ccord_theme_color_count()) == nullptr);
  CT_CHECK(ccord_theme_metric_count() > 20 && ccord_theme_metric_name(0) != nullptr);
  CT_CHECK(ccord_theme_metric_name(ccord_theme_metric_count()) == nullptr);
  ccord_color got = {0};
  CT_CHECK(ccord_theme_color_set(&t, "bg-primary", (ccord_color){10, 20, 30, 255}));
  CT_CHECK(ccord_theme_color_get(&t, "bg_primary", &got) && color_is(got, 10, 20, 30, 255));
  CT_CHECK(!ccord_theme_color_get(&t, "no_such_token", &got));
  CT_CHECK(!ccord_theme_color_set(&t, "no_such_token", got));
  float mv = 0.0f;
  CT_CHECK(ccord_theme_metric_set(&t, "scrollbar-width", 9.5f));
  CT_CHECK(ccord_theme_metric_get(&t, "scrollbar_width", &mv) && mv == 9.5f);
  CT_CHECK(!ccord_theme_metric_set(&t, "scrollbar_width", -1.0f));
  CT_CHECK(!ccord_theme_metric_set(&t, "scrollbar_width", 9999.0f));
  CT_CHECK(!ccord_theme_metric_get(&t, "no_such_metric", &mv));
}

int main(void)
{
  test_color_parse();
  test_parse_asset();
  test_parse_valid_minimal();
  test_negative();
  test_load_missing();
  test_builtin_matches_disk();
  test_builtin_switch_in_place();
  test_builtin_errors();
  test_font_paths();
  test_metric_boundary();
  test_quick_css_basic();
  test_quick_css_aliases();
  test_quick_css_skips_bad();
  test_quick_css_color_forms();
  test_quick_css_idempotent_and_args();
  test_plugin_css_json_and_reflection();
  return CT_RESULT("test_theme");
}
