// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/assets/embedded_assets.h>
#include <ccord/core/error.h>
typedef struct ccord_color { uint8_t r, g, b, a; } ccord_color;
// X-macro lists = the theme file schema. Key in the file == identifier. Section: [colors] / [metrics] / [fonts].
#define CCORD_THEME_COLORS(X) \
  X(bg_primary) X(bg_secondary) X(bg_tertiary) X(bg_floating) X(bg_input) X(bg_user_panel) \
  X(bg_message_hover) X(bg_item_hover) X(bg_item_selected) X(bg_guild_idle) \
  X(divider) X(border_subtle) X(scrollbar_thumb) X(scrollbar_track) \
  X(text_normal) X(text_muted) X(text_header) X(text_secondary) X(text_link) X(text_on_brand) \
  X(text_placeholder) X(text_interactive_hover) X(text_interactive_active) \
  X(brand) X(brand_hover) X(status_online) X(status_idle) X(status_dnd) X(status_offline) \
  X(danger) X(mention_bg) X(selection) X(caret) X(pill) X(overlay) \
  X(avatar_0) X(avatar_1) X(avatar_2) X(avatar_3) X(avatar_4) X(avatar_5)
#define CCORD_THEME_METRICS(X) \
  X(server_list_width) X(channel_list_width) X(member_list_width) X(header_height) X(user_panel_height) \
  X(guild_icon_size) X(guild_icon_gap) X(guild_icon_radius) X(guild_icon_radius_active) X(pill_width) \
  X(pill_height_hover) X(pill_height_active) X(channel_item_height) X(channel_item_margin_x) X(channel_item_pad_x) \
  X(channel_item_radius) X(category_height) X(avatar_size) X(avatar_size_small) X(member_item_height) X(member_group_height) \
  X(status_dot_size) X(message_pad_x) X(message_text_x) X(message_group_gap) X(message_line_gap) X(message_merge_ms) \
  X(line_height_factor) X(composer_margin_x) X(composer_margin_bottom) X(composer_min_height) X(composer_max_height) \
  X(composer_radius) X(composer_pad_x) X(composer_pad_y) X(scrollbar_width) X(scrollbar_min_thumb) \
  X(context_menu_min_width) X(context_menu_item_height) X(context_menu_radius) X(context_menu_pad) \
  X(settings_sidebar_width) X(hover_anim_ms) X(min_window_width) X(min_window_height) X(hide_members_below_width) \
  X(font_size_base) X(font_size_small) X(font_size_large)
typedef struct ccord_theme_colors {
#define X(n) ccord_color n;
  CCORD_THEME_COLORS(X)
#undef X
} ccord_theme_colors;
typedef struct ccord_theme_metrics {
#define X(n) float n;
  CCORD_THEME_METRICS(X)
#undef X
} ccord_theme_metrics;
typedef struct ccord_theme {
  char id[48], name[64];
  char font_regular[128], font_bold[128];        // relative to the asset dir
  ccord_theme_colors c;
  ccord_theme_metrics m;
} ccord_theme;
// Strict: every key must be present exactly once, unknown key/section -> CCORD_ERR_PARSE naming the line.
// Colors "#rrggbb" or "#rrggbbaa". Metrics: decimal float, 0 <= v <= 4096 (message_merge_ms <= 3.6e6).
// Fonts: relative path, no "..", no leading "/", <= 127 bytes, must end in .ttf, .otf or .ttc.
[[nodiscard]] ccord_status ccord_theme_parse(ccord_theme *out, const char *text, size_t len, ccord_error *err);
[[nodiscard]] ccord_status ccord_theme_load(ccord_theme *out, const char *path, ccord_error *err);  // file <= 64 KiB
// Built-in themes compiled into the binary: "c-cord-dark", "c-cord-light", "c-cord-amoled".
// `out` is fully replaced (safe to switch in place, no stale keys). Unknown id -> CCORD_ERR_NOT_FOUND.
[[nodiscard]] ccord_status ccord_theme_load_builtin(ccord_theme *out, const char *id, ccord_error *err);
[[nodiscard]] bool ccord_color_parse(const char *s, ccord_color *out);
// Applies user QuickCSS custom-property overrides onto an initialized theme, in place.
// Supported input: /* ... */ comments; rule blocks whose selectors are :root, body, *, .theme-dark
// or .theme-light (comma-separated lists allowed); or bare `--prop: value;` declarations.
// Values: #rgb / #rgba / #rrggbb / #rrggbbaa, rgb(...) / rgba(...) for colours and px or plain
// decimal numbers for metrics; a trailing !important is accepted and ignored.
// Properties: Discord custom properties (--background-primary, --background-secondary-alt,
// --channeltextarea-background, --text-normal, --header-primary, --interactive-*,
// --brand-experiment, --brand-experiment-560, --status-*, --scrollbar-thin-thumb, ...), plus
// --ccord-<ini key> and --<ini key> aliases (hyphens or underscores) mapping onto ccord_theme.
// Unknown properties, unsupported selectors, malformed declarations and values of the wrong kind
// (e.g. a number for a colour) are skipped without changing the theme and without failing.
// *applied (optional) receives the number of overrides written. Returns CCORD_OK unless the
// arguments are invalid, the text exceeds 64 KiB or allocation fails.
[[nodiscard]] ccord_status ccord_theme_apply_quick_css(ccord_theme *theme, const char *css, size_t len,
                                                       size_t *applied, ccord_error *err);
// Extended plugin CSS: supports all QuickCSS custom properties plus surface-scoped rules
// (.chat, .sidebar, .composer, .member-list, .guild-rail, .message, .header, .modal, .user-panel)
// mapping background/background-color/color/border-color/border-radius/width/height/font-size onto theme tokens.
[[nodiscard]] ccord_status ccord_theme_apply_plugin_css(ccord_theme *theme, const char *css, size_t len,
                                                        size_t *applied, ccord_error *err);
// Applies JSON object patches ({ "bg_primary": "#112233", ... } and { "channel_list_width": 260, ... }) onto `theme`.
// Either JSON string may be nullptr/empty. Unknown or malformed entries are skipped safely.
[[nodiscard]] ccord_status ccord_theme_apply_json(ccord_theme *theme, const char *colors_json, const char *metrics_json,
                                                  size_t *applied, ccord_error *err);
// Token reflection & lookup by canonical key (accepts hyphens or underscores).
size_t ccord_theme_color_count(void);
const char *ccord_theme_color_name(size_t index);
bool ccord_theme_color_get(const ccord_theme *theme, const char *name, ccord_color *out);
bool ccord_theme_color_set(ccord_theme *theme, const char *name, ccord_color color);
size_t ccord_theme_metric_count(void);
const char *ccord_theme_metric_name(size_t index);
bool ccord_theme_metric_get(const ccord_theme *theme, const char *name, float *out);
bool ccord_theme_metric_set(ccord_theme *theme, const char *name, float value);
