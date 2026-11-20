// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Profile view logic (profile_view.c): banner source order, the two-colour theme's text choice, the per-server profile in a
// guild versus a DM, badges, decoration, the Member Since lines, and the debug record the verify slice reads.
#include <stdio.h>
#include <string.h>

#include "../ui_internal.h"

#include "ccord_test.h"

#define GUILD 777u

static ccord_user base_user(void)
{
  ccord_user u = {0};
  u.id = 1001u;
  u.name = "global";
  u.display_name = "Global Name";
  u.handle = "handle";
  u.avatar_url = "";
  u.banner_url = "";
  u.bio = "user bio";
  u.pronouns = "they/them";
  return u;
}

static void test_banner_order(void)
{
  ccord_user_profile p = {0};
  ccord_user v = base_user();
  ui_profile_look l;

  // Before GET /users/{id}/profile arrives (prof == nullptr), view->accent_color must not flash a solid colour block.
  v.accent_color = 0x5700ffu;
  v.has_accent_color = true;
  ui_profile_resolve(nullptr, CCORD_ID_NONE, &v, &l);
  CT_CHECK(l.banner_src == UI_PBANNER_NONE && !l.has_fill && l.banner_url[0] == '\0');

  v = base_user();
  ui_profile_resolve(&p, CCORD_ID_NONE, &v, &l);
  CT_CHECK(l.banner_src == UI_PBANNER_NONE && !l.has_fill && l.banner_url[0] == '\0');

  v = base_user();
  v.accent_color = 0x112233u;
  ui_profile_resolve(&p, CCORD_ID_NONE, &v, &l);
  CT_CHECK(l.banner_src == UI_PBANNER_ACCENT && l.has_fill && l.banner_rgb == 0x112233u);

  p.has_banner_color = true;
  p.banner_color = 0xaabbccu;
  v = base_user();
  v.accent_color = 0x112233u;
  ui_profile_resolve(&p, CCORD_ID_NONE, &v, &l);
  CT_CHECK(l.banner_src == UI_PBANNER_COLOR && l.has_fill && l.banner_rgb == 0xaabbccu && l.banner_url[0] == '\0');

  v = base_user();
  v.banner_url = "https://cdn.example/user.png";
  ui_profile_resolve(&p, CCORD_ID_NONE, &v, &l);
  CT_CHECK(l.banner_src == UI_PBANNER_USER_IMAGE && strcmp(l.banner_url, "https://cdn.example/user.png") == 0);
  CT_CHECK(l.banner_rgb == 0xaabbccu);   // the colour stays as the fill under the image for non-themed users

  p.guild_id = GUILD;
  p.guild_banner_url = "https://cdn.example/guild.png";
  v = base_user();
  v.banner_url = "https://cdn.example/user.png";
  ui_profile_resolve(&p, GUILD, &v, &l);
  CT_CHECK(l.banner_src == UI_PBANNER_SERVER_IMAGE && strcmp(l.banner_url, "https://cdn.example/guild.png") == 0);

  // Nitro theme_colors active: official Discord ignores stale banner_color / accent_color completely.
  p = (ccord_user_profile){
    .has_banner_color = true,
    .banner_color = 0x5700ffu,
    .has_accent_color = true,
    .accent_color = 5701887u,
    .has_theme = true,
    .theme_primary = 0xff005bu,
    .theme_accent = 0x0b0a09u,
  };
  v = base_user();
  v.accent_color = 0x5700ffu;
  v.has_accent_color = true;
  ui_profile_resolve(&p, CCORD_ID_NONE, &v, &l);
  CT_CHECK(l.has_theme && !l.has_fill && l.banner_src == UI_PBANNER_NONE && l.banner_rgb == 0xff005bu && l.light_text);

  v = base_user();
  v.banner_url = "https://cdn.example/user.png";
  ui_profile_resolve(&p, CCORD_ID_NONE, &v, &l);
  CT_CHECK(l.has_theme && !l.has_fill && l.banner_src == UI_PBANNER_USER_IMAGE);

  p.guild_id = GUILD;
  p.guild_banner_url = "https://cdn.example/guild.png";
  v = base_user();
  ui_profile_resolve(&p, GUILD, &v, &l);
  CT_CHECK(l.has_theme && !l.has_fill && l.banner_src == UI_PBANNER_SERVER_IMAGE);
}

static void test_theme(void)
{
  ccord_user_profile p = {0};
  ccord_user v = base_user();
  ui_profile_look l;
  ui_profile_resolve(&p, CCORD_ID_NONE, &v, &l);
  CT_CHECK(!l.has_theme);
  p.has_theme = true;
  p.theme_primary = 0x101020u;
  p.theme_accent = 0x202040u;
  ui_profile_resolve(&p, CCORD_ID_NONE, &v, &l);
  CT_CHECK(l.has_theme && l.theme_top == 0x101020u && l.theme_bot == 0x202040u && l.light_text);
  // Saturated mid-tone Nitro themes (gold/green, coral/rose) use Discord's dark scrim + light text.
  p.theme_primary = 0xfaa61au;
  p.theme_accent = 0x3ba55cu;
  ui_profile_resolve(&p, CCORD_ID_NONE, &v, &l);
  CT_CHECK(l.has_theme && l.light_text);
  p.theme_primary = 0xf5893au;
  p.theme_accent = 0xe47575u;
  ui_profile_resolve(&p, CCORD_ID_NONE, &v, &l);
  CT_CHECK(l.has_theme && l.light_text);
  // Bright pastel/white themes (luminance >= 0.50) use light scrim + dark text.
  p.theme_primary = 0xf0f0e0u;
  p.theme_accent = 0xffffffu;
  ui_profile_resolve(&p, CCORD_ID_NONE, &v, &l);
  CT_CHECK(l.has_theme && !l.light_text);
}

static void test_server_profile_and_dates(void)
{
  ccord_user_profile p = {0};
  p.guild_id = GUILD;
  p.guild_nick = "Server Nick";
  p.guild_bio = "server bio";
  p.guild_pronouns = "she/her";
  p.guild_joined_at_ms = 1600000000000ll;
  p.premium_since_ms = 1650000000000ll;
  p.badge_count = 2;
  strcpy(p.badges[0].id, "a");
  strcpy(p.badges[0].icon_url, "https://cdn.example/a.png");
  strcpy(p.badges[1].id, "b");
  p.avatar_decoration_url = "https://cdn.example/deco.png";
  ccord_user v = base_user();
  ui_profile_look l;
  char lines[3][96];

  ui_profile_resolve(&p, GUILD, &v, &l);
  CT_CHECK(l.server_profile && strcmp(v.display_name, "Server Nick") == 0 && strcmp(l.global_name, "Global Name") == 0);
  CT_CHECK(strcmp(v.bio, "server bio") == 0 && strcmp(v.pronouns, "she/her") == 0);
  CT_CHECK(l.badge_count == 2 && strcmp(l.deco_url, "https://cdn.example/deco.png") == 0);
  CT_CHECK(ui_profile_since_lines(&l, lines) == 3);
  ui_profile_view_debug_record(false, &l, &v, true);
  ui_profile_view_debug dbg;
  ui_profile_view_debug_get(nullptr, false, &dbg);
  CT_CHECK(dbg.valid && dbg.server_profile && dbg.badge_count == 2 && dbg.has_deco && dbg.dates_laid_out);
  CT_CHECK(strcmp(dbg.name, "Server Nick") == 0 && strcmp(dbg.bio, "server bio") == 0 && strcmp(dbg.pronouns, "she/her") == 0);

  // The same profile in a DM: user-level values, no server dates.
  v = base_user();
  ui_profile_resolve(&p, CCORD_ID_NONE, &v, &l);
  CT_CHECK(!l.server_profile && strcmp(v.display_name, "Global Name") == 0 && strcmp(v.bio, "user bio") == 0 &&
           strcmp(v.pronouns, "they/them") == 0);
  CT_CHECK(ui_profile_since_lines(&l, lines) == 2);   // account + boosting; no server line

  // Nothing extra set: no badges, no decoration, one date line.
  ccord_user_profile empty = {0};
  v = base_user();
  ui_profile_resolve(&empty, CCORD_ID_NONE, &v, &l);
  CT_CHECK(l.badge_count == 0 && l.deco_url[0] == '\0' && ui_profile_since_lines(&l, lines) == 1);
  CT_CHECK(l.effect == nullptr && l.frame == nullptr && l.nameplate_asset[0] == '\0');
}

static void test_decor_nameplate_effect_frame(void)
{
  ccord_user_profile p = {0};
  p.guild_id = GUILD;
  p.avatar_decoration_url = "https://cdn.example/user_deco.png";
  p.guild_avatar_decoration_url = "https://cdn.example/guild_deco.png";
  snprintf(p.nameplate_asset, sizeof p.nameplate_asset, "nameplates/nameplates/american_flag/");
  snprintf(p.nameplate_palette, sizeof p.nameplate_palette, "cobalt");
  snprintf(p.guild_nameplate_asset, sizeof p.guild_nameplate_asset, "nameplates/nameplates/twilight/");
  snprintf(p.guild_nameplate_palette, sizeof p.guild_nameplate_palette, "berry");
  p.profile_effect_id = 555u;
  p.profile_effect.id = 555u;
  p.profile_effect.layer_count = 1;
  snprintf(p.profile_effect.layers[0].src, sizeof p.profile_effect.layers[0].src,
           "https://cdn.discordapp.com/assets/profile_effects/starry.png");
  p.profile_frame_sku_id = 777u;
  p.profile_frame.sku_id = 777u;
  p.profile_frame.inner_width = 300;
  p.profile_frame.overflow_top = 32;
  p.profile_frame.layer_count = 1;
  snprintf(p.profile_frame.layers[0].url, sizeof p.profile_frame.layers[0].url,
           "https://cdn.discordapp.com/assets/collectibles/cat/top.png");

  // In a guild: per-server avatar decoration & nameplate win; effect and frame resolve.
  ccord_user v = base_user();
  ui_profile_look l;
  ui_profile_resolve(&p, GUILD, &v, &l);
  CT_CHECK(strcmp(l.deco_url, "https://cdn.example/guild_deco.png") == 0);
  CT_CHECK(strcmp(v.avatar_decoration_url, "https://cdn.example/guild_deco.png") == 0);
  CT_CHECK(strcmp(l.nameplate_asset, "nameplates/nameplates/twilight/") == 0 && strcmp(l.nameplate_palette, "berry") == 0);
  CT_CHECK(l.effect == &p.profile_effect && l.frame == &p.profile_frame);
  ui_profile_view_debug_record(true, &l, &v, true);
  ui_profile_view_debug dbg;
  ui_profile_view_debug_get(nullptr, true, &dbg);
  CT_CHECK(dbg.valid && dbg.has_deco && dbg.has_effect && dbg.has_frame);

  // In a DM: user-level avatar decoration & nameplate win.
  v = base_user();
  ui_profile_resolve(&p, CCORD_ID_NONE, &v, &l);
  CT_CHECK(strcmp(l.deco_url, "https://cdn.example/user_deco.png") == 0);
  CT_CHECK(strcmp(l.nameplate_asset, "nameplates/nameplates/american_flag/") == 0 &&
           strcmp(l.nameplate_palette, "cobalt") == 0);
  CT_CHECK(l.effect == &p.profile_effect && l.frame == &p.profile_frame);

  // Before GET /users/{id}/profile arrives (prof == nullptr): user's own avatar_decoration_url & nameplate carry through.
  v = base_user();
  v.avatar_decoration_url = "https://cdn.example/live_deco.png";
  snprintf(v.nameplate_asset, sizeof v.nameplate_asset, "nameplates/nameplates/american_flag/");
  snprintf(v.nameplate_palette, sizeof v.nameplate_palette, "cobalt");
  ui_profile_resolve(nullptr, GUILD, &v, &l);
  CT_CHECK(strcmp(l.deco_url, "https://cdn.example/live_deco.png") == 0);
  CT_CHECK(strcmp(l.nameplate_asset, "nameplates/nameplates/american_flag/") == 0);
}

int main(void)
{
  test_banner_order();
  test_theme();
  test_server_profile_and_dates();
  test_decor_nameplate_effect_frame();
  return CT_RESULT("test_ui_profile_view");
}
