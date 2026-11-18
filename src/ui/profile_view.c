// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// What a user set on their profile, shared by the popout and the full profile: banner choice, the two-colour theme with
// readable text colours, the per-server profile, badges, avatar decoration and the Member Since dates.
// The draw code in draw_chat.c / draw_members.c only calls into here.
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"
#include "ui_style.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

static ui_profile_view_debug g_dbg[2];   // last draw of the popout [0] and the full profile [1]
static char g_tip_text[160];        // badge tooltip pending for ui_profile_flush_tooltip
static ccord_rect g_tip_rect;
static bool g_tip_on;

static bool has_text(const char *s) { return s && s[0]; }

static float channel01(uint32_t v) { return (float)v / 255.0f; }

static float lin(float c) { return c <= 0.03928f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

static float rgb_luma(uint32_t rgb)
{
  return 0.2126f * lin(channel01((rgb >> 16) & 0xffu)) + 0.7152f * lin(channel01((rgb >> 8) & 0xffu)) +
         0.0722f * lin(channel01(rgb & 0xffu));
}

static bool is_http(const char *s) { return strncmp(s, "https://", 8) == 0 || strncmp(s, "http://", 7) == 0; }

// An absolute URL is used as is; a bare asset id gets the CDN prefix and suffix.
static void cdn_url(char *out, size_t cap, const char *v, const char *prefix, const char *suffix)
{
  if (is_http(v)) snprintf(out, cap, "%s", v);
  else snprintf(out, cap, "%s%.120s%s", prefix, v, suffix);
}

static void upgrade_banner_size(char *url, size_t cap)
{
  char *p = strstr(url, "?size=64");
  if (!p) p = strstr(url, "&size=64");
  if (!p) return;
  if (p[8] != '\0' && p[8] != '&') return;
  size_t prefix_len = (size_t)(p + 6 - url);
  char tail[128];
  snprintf(tail, sizeof tail, "%s", p + 8);
  snprintf(url + prefix_len, cap - prefix_len, "512%s", tail);
}

void ui_profile_resolve(const ccord_user_profile *prof, ccord_id gid, ccord_user *view, ui_profile_look *look)
{
  *look = (ui_profile_look){0};
  look->user_id = view->id;
  look->guild_id = gid;
  look->name = view->display_name;
  look->bio = view->bio;
  look->pronouns = view->pronouns;
  bool in_guild = gid != CCORD_ID_NONE && gid != CCORD_DM_GUILD_ID;
  bool srv = in_guild && prof && (prof->guild_id == gid || prof->guild_id == CCORD_ID_NONE) &&
             (has_text(prof->guild_nick) || has_text(prof->guild_avatar_url) || has_text(prof->guild_banner_url) ||
              has_text(prof->guild_bio) || has_text(prof->guild_pronouns) || prof->guild_joined_at_ms > 0);
  look->global_name = view->display_name;
  char url[256] = "";
  if (srv && has_text(prof->guild_banner_url)) {
    if (is_http(prof->guild_banner_url)) {
      snprintf(url, sizeof url, "%s", prof->guild_banner_url);
    } else {
      snprintf(url, sizeof url, "https://cdn.discordapp.com/guilds/%llu/users/%llu/banners/%.96s.%s?size=512",
               (unsigned long long)gid, (unsigned long long)view->id, prof->guild_banner_url,
               strncmp(prof->guild_banner_url, "a_", 2) == 0 ? "gif" : "png");
    }
    look->banner_src = UI_PBANNER_SERVER_IMAGE;
  } else if (has_text(view->banner_url)) {
    (void)ui_user_banner_url(view, url, sizeof url);
    look->banner_src = UI_PBANNER_USER_IMAGE;
  } else if (prof && has_text(prof->banner_url)) {
    ccord_user tmp = *view;
    tmp.banner_url = prof->banner_url;
    (void)ui_user_banner_url(&tmp, url, sizeof url);
    look->banner_src = UI_PBANNER_USER_IMAGE;
  }
  upgrade_banner_size(url, sizeof url);
  snprintf(look->banner_url, sizeof look->banner_url, "%s", url);
  if (prof && prof->has_theme) {
    // Official Discord ignores banner_color and accent_color whenever Nitro theme_colors are active.
    look->has_theme = true;
    look->theme_top = prof->theme_primary & 0xffffffu;
    look->theme_bot = prof->theme_accent & 0xffffffu;
    float lum = (rgb_luma(look->theme_top) + rgb_luma(look->theme_bot)) * 0.5f;
    look->light_text = lum < 0.50f;
    look->has_fill = false;
    look->banner_rgb = look->theme_top;
  } else if (prof) {
    // Only read banner_color / accent_color once prof has arrived (avoiding a stale accent_color flash before GET /profile).
    if (prof->has_banner_color) {
      look->banner_rgb = prof->banner_color & 0xffffffu;
      look->has_fill = true;
      if (look->banner_src == UI_PBANNER_NONE) look->banner_src = UI_PBANNER_COLOR;
    } else if (prof->has_accent_color || prof->accent_color != 0) {
      look->banner_rgb = prof->accent_color & 0xffffffu;
      look->has_fill = true;
      if (look->banner_src == UI_PBANNER_NONE) look->banner_src = UI_PBANNER_ACCENT;
    } else if (view->has_accent_color || view->accent_color != 0) {
      look->banner_rgb = view->accent_color & 0xffffffu;
      look->has_fill = true;
      if (look->banner_src == UI_PBANNER_NONE) look->banner_src = UI_PBANNER_ACCENT;
    }
  }
  if (srv) {
    if (has_text(prof->guild_nick)) {
      look->name = prof->guild_nick;
      look->global_name = view->display_name;
      view->display_name = prof->guild_nick;
      view->name = prof->guild_nick;
    }
    if (has_text(prof->guild_avatar_url)) {
      if (is_http(prof->guild_avatar_url)) {
        look->avatar_override = prof->guild_avatar_url;
      } else {
        snprintf(look->avatar_url_buf, sizeof look->avatar_url_buf,
                 "https://cdn.discordapp.com/guilds/%llu/users/%llu/avatars/%.96s.%s?size=128", (unsigned long long)gid,
                 (unsigned long long)view->id, prof->guild_avatar_url,
                 strncmp(prof->guild_avatar_url, "a_", 2) == 0 ? "gif" : "png");
        look->avatar_override = look->avatar_url_buf;
      }
      view->avatar_url = look->avatar_override;
    }
    if (has_text(prof->guild_bio)) view->bio = prof->guild_bio;
    if (has_text(prof->guild_pronouns)) view->pronouns = prof->guild_pronouns;
    look->guild_joined_at_ms = prof->guild_joined_at_ms;
    look->server_profile = true;
  }
  look->name = view->display_name;
  look->bio = view->bio;
  look->pronouns = view->pronouns;
  if (prof) {
    look->premium_since_ms = prof->premium_since_ms;
    look->badge_count = prof->badge_count > CCORD_PROFILE_MAX_BADGES ? CCORD_PROFILE_MAX_BADGES : prof->badge_count;
    look->badges = prof->badges;
    if (has_text(prof->avatar_decoration_url)) {
      cdn_url(look->deco_url, sizeof look->deco_url, prof->avatar_decoration_url,
              "https://cdn.discordapp.com/avatar-decoration-presets/", ".png?size=128&passthrough=true");
    }
  }
}

ccord_color ui_profile_banner_fill(const ccord_ui *ui, const ui_profile_look *look, const ccord_user *u)
{
  (void)u;
  if (look->has_fill) return ui_rgb_color(look->banner_rgb);
  if (look->has_theme) {
    ccord_color tint = look->light_text ? (ccord_color){0x11, 0x12, 0x14, 255} : (ccord_color){0xff, 0xff, 0xff, 255};
    return ui_mix(ui_rgb_color(look->theme_top), tint, look->light_text ? (56.0f / 255.0f) : (64.0f / 255.0f));
  }
  return ui_mix(COL(bg_tertiary), COL(bg_secondary), 0.35f);
}

ccord_color ui_profile_card_bg(const ccord_ui *ui, const ui_profile_look *look, ccord_color fallback)
{
  (void)ui;
  if (!look || !look->has_theme) return fallback;
  ccord_color base = ui_rgb_color(look->theme_top);
  ccord_color scrim = look->light_text ? (ccord_color){0x11, 0x12, 0x14, 255} : (ccord_color){0xff, 0xff, 0xff, 255};
  return ui_mix(base, scrim, look->light_text ? (178.0f / 255.0f) : (188.0f / 255.0f));
}

void ui_profile_draw_banner(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, const ccord_user *u,
                            ccord_rect banner, float radius)
{
  if (banner.w <= 0.0f || banner.h <= 0.0f) return;
  bool have_img = false;
  if (look->banner_url[0] && ui_ensure_media(ui, r, look->banner_url)) {
    int tw = 0, th = 0;
    have_img = ccord_render_has_texture(r, look->banner_url, &tw, &th) && tw > 2 && th > 2;
  }
  if (look->has_theme) {
    if (!have_img) return;   // ui_profile_draw_theme_bg already painted the seamless themed banner + 4px border
    const float border = 4.0f;
    if (banner.w <= border * 2.0f || banner.h <= border) return;
    ccord_rect inner = {banner.x + border, banner.y + border, banner.w - border * 2.0f, banner.h - border};
    float ir = radius > border ? radius - border : 0.0f;
    ccord_push_clip(r, inner);
    ccord_draw_texture_cover(r, look->banner_url, (ccord_rect){inner.x, inner.y, inner.w, inner.h + ir * 2.0f}, ir, 255,
                             true);
    ccord_pop_clip(r);
    return;
  }
  ccord_push_clip(r, banner);
  ccord_draw_rrect(r, (ccord_rect){banner.x, banner.y, banner.w, banner.h + radius * 2.0f}, radius,
                   ui_profile_banner_fill(ui, look, u));
  if (have_img) {
    ccord_draw_texture_cover(r, look->banner_url, (ccord_rect){banner.x, banner.y, banner.w, banner.h + radius * 2.0f},
                             radius, 255, true);
  }
  ccord_pop_clip(r);
}

ui_profile_text ui_profile_text_colors(const ccord_ui *ui, const ui_profile_look *look)
{
  ui_profile_text t;
  if (look->has_theme) {
    if (look->light_text) {
      t = (ui_profile_text){{245, 245, 247, 255}, {222, 224, 228, 255}, {190, 193, 200, 255}};
    } else {
      t = (ui_profile_text){{18, 19, 22, 255}, {48, 51, 58, 255}, {78, 82, 92, 255}};
    }
  } else {
    t = (ui_profile_text){COL(text_header), COL(text_secondary), COL(text_muted)};
  }
  return t;
}

void ui_profile_draw_theme_bg(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, ccord_rect box, float banner_h,
                              float radius)
{
  (void)ui;
  if (!look->has_theme || box.w <= 0.0f || box.h <= 0.0f) return;
  enum { N = 28 };
  float bot = box.y + box.h;
  float h = box.h;
  ccord_color a = ui_rgb_color(look->theme_top), b = ui_rgb_color(look->theme_bot);
  for (int i = 0; i < N; i++) {
    float t = ((float)i + 0.5f) / (float)N;
    ccord_color c = ui_mix(a, b, t);
    float sy = roundf(box.y + h * (float)i / (float)N);
    float sy2 = (i == N - 1) ? bot : roundf(box.y + h * (float)(i + 1) / (float)N);
    if (sy2 <= sy) continue;
    ccord_rect strip = {box.x, sy, box.w, sy2 - sy};
    if (radius > 0.0f && (sy < box.y + radius || sy2 > bot - radius)) {
      ccord_push_clip(r, strip);
      ccord_draw_rrect(r, box, radius, c);
      ccord_pop_clip(r);
    } else {
      ccord_draw_rect(r, strip, c);
    }
  }
  const float border = 4.0f;
  if (box.w <= border * 2.0f || h <= border * 2.0f) return;
  float bx = box.x + border, bw = box.w - border * 2.0f;
  float ir = radius > border ? radius - border : 0.0f;
  float bh = banner_h;
  if (bh > h - border) bh = h - border;
  if (bh > border) {
    ccord_rect top_zone = {bx, box.y + border, bw, bh - border};
    ccord_color banner_tint = look->light_text ? (ccord_color){0x11, 0x12, 0x14, 56}
                                               : (ccord_color){0xff, 0xff, 0xff, 64};
    ccord_push_clip(r, top_zone);
    ccord_draw_rrect(r, (ccord_rect){top_zone.x, top_zone.y, top_zone.w, top_zone.h + ir * 2.0f}, ir, banner_tint);
    ccord_pop_clip(r);
  }
  float body_y = bh > border ? box.y + bh : box.y + border;
  float body_h = (bot - border) - body_y;
  if (body_h > 0.0f) {
    ccord_rect body_zone = {bx, body_y, bw, body_h};
    ccord_color scrim = look->light_text ? (ccord_color){0x11, 0x12, 0x14, 178}
                                         : (ccord_color){0xff, 0xff, 0xff, 188};
    if (bh > border) {
      ccord_push_clip(r, body_zone);
      ccord_draw_rrect(r, (ccord_rect){body_zone.x, body_zone.y - ir * 2.0f, body_zone.w, body_zone.h + ir * 2.0f},
                       ir, scrim);
      ccord_pop_clip(r);
    } else {
      ccord_draw_rrect(r, body_zone, ir, scrim);
    }
  }
}

static ccord_color mock_badge_accent(const char *id, size_t idx)
{
  if (id && (strstr(id, "early") || strstr(id, "gold") || strstr(id, "hypesquad")))
    return (ccord_color){0xf0, 0xb2, 0x32, 255};
  if (id && (strstr(id, "bug") || strstr(id, "active") || strstr(id, "dev")))
    return (ccord_color){0x43, 0xb5, 0x81, 255};
  if (id && (strstr(id, "nitro") || strstr(id, "premium") || strstr(id, "boost")))
    return (ccord_color){0xff, 0x73, 0xfa, 255};
  static const ccord_color k_acc[3] = {{0x58, 0x65, 0xf2, 255}, {0xf0, 0xb2, 0x32, 255}, {0x43, 0xb5, 0x81, 255}};
  return k_acc[idx % 3u];
}

float ui_profile_draw_badges(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, float x, float y, float w,
                             bool measure_only)
{
  if (look->badge_count == 0 || !look->badges) return 0.0f;
  const float sz = 20.0f, gap = 4.0f;
  float cx = x, cy = y;
  for (size_t i = 0; i < look->badge_count; i++) {
    const ccord_profile_badge *b = &look->badges[i];
    if (cx + sz > x + w && cx > x) { cx = x; cy += sz + gap; }
    ccord_rect ir = {cx, cy, sz, sz};
    if (!measure_only) {
      char url[256];
      cdn_url(url, sizeof url, b->icon_url, "https://cdn.discordapp.com/badge-icons/", ".png");
      bool drawn = b->icon_url[0] && ui_ensure_media(ui, r, url);
      int tw = 0, th = 0;
      if (drawn && ccord_render_has_texture(r, url, &tw, &th)) {
        if (tw > 2 && th > 2) {
          ccord_draw_texture(r, url, ir, 255);
        } else {
          // 1x1 mock PNG stub in --mock: draw a clean subtle circle + vector gem glyph instead of a solid square.
          float bcx = ir.x + sz * 0.5f, bcy = ir.y + sz * 0.5f;
          ccord_color plate = (look->has_theme && !look->light_text) ? (ccord_color){18, 19, 22, 38}
                                                                     : (ccord_color){0x11, 0x12, 0x14, 148};
          ccord_color acc = mock_badge_accent(b->id, i);
          ccord_draw_circle(r, bcx, bcy, 9.0f, plate);
          ccord_draw_circle(r, bcx, bcy, 6.2f, ui_alpha(acc, 0.22f));
          ccord_draw_line(r, (ccord_vec2){bcx, bcy - 4.5f}, (ccord_vec2){bcx + 4.5f, bcy}, 1.5f, acc);
          ccord_draw_line(r, (ccord_vec2){bcx + 4.5f, bcy}, (ccord_vec2){bcx, bcy + 4.5f}, 1.5f, acc);
          ccord_draw_line(r, (ccord_vec2){bcx, bcy + 4.5f}, (ccord_vec2){bcx - 4.5f, bcy}, 1.5f, acc);
          ccord_draw_line(r, (ccord_vec2){bcx - 4.5f, bcy}, (ccord_vec2){bcx, bcy - 4.5f}, 1.5f, acc);
          ccord_draw_circle(r, bcx, bcy, 2.0f, acc);
        }
      }
      // When !drawn (still downloading over HTTP in --live): draw nothing so layout stays stable without grey blocks.
      if (ui->mouse_in && ui_in_rect(ui->mx, ui->my, ir) && b->description[0]) {
        snprintf(g_tip_text, sizeof g_tip_text, "%s", b->description);
        g_tip_rect = ir;
        g_tip_on = true;
      }
    }
    cx += sz + gap;
  }
  return (cy - y) + sz + 6.0f;
}

void ui_profile_flush_tooltip(ccord_ui *ui, ccord_renderer *r)
{
  if (!g_tip_on) return;
  g_tip_on = false;
  (void)uis_tooltip(ui, r, g_tip_rect, g_tip_text, UIS_TIP_ABOVE, 7);
}

void ui_profile_draw_deco(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, float cx, float cy, float avatar_size)
{
  if (!look->deco_url[0]) return;
  float s = avatar_size * 1.2f;
  if (ui_ensure_media(ui, r, look->deco_url)) {
    int tw = 0, th = 0;
    if (ccord_render_has_texture(r, look->deco_url, &tw, &th) && tw <= 2 && th <= 2) return;
    ccord_draw_texture(r, look->deco_url, (ccord_rect){cx - s * 0.5f, cy - s * 0.5f, s, s}, 255);
  }
}

static void ms_date(int64_t ms, char *out, size_t cap)
{
  if (ms <= 1420070400000ll) { out[0] = '\0'; return; }
  ui_snowflake_date((ccord_id)((uint64_t)(ms - 1420070400000ll) << 22), out, cap);
}

size_t ui_profile_since_lines(const ui_profile_look *look, char lines[3][96])
{
  size_t n = 0;
  char d[48];
  ui_snowflake_date(look->user_id, d, sizeof d);
  snprintf(lines[n++], 96, "Discord: %s", d);
  if (look->server_profile) {
    ms_date(look->guild_joined_at_ms, d, sizeof d);
    if (d[0]) snprintf(lines[n++], 96, "This server: %s", d);
  }
  ms_date(look->premium_since_ms, d, sizeof d);
  if (d[0]) snprintf(lines[n++], 96, "Boosting since: %s", d);
  return n;
}

float ui_profile_draw_since(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, float x, float y, float w,
                            const ccord_text_style *st, bool measure_only)
{
  char lines[3][96];
  size_t n = ui_profile_since_lines(look, lines);
  float lh = ui_line_h(ui, st->size);
  if (!measure_only) {
    for (size_t i = 0; i < n; i++) {
      ccord_text_draw_ellipsis(r, lines[i], strlen(lines[i]), x, y + lh * (float)i, w, st);
    }
  }
  return lh * (float)n;
}

void ui_profile_view_debug_record(bool modal, const ui_profile_look *look, const ccord_user *u, bool dates_laid_out)
{
  ui_profile_view_debug *d = &g_dbg[modal ? 1 : 0];
  *d = (ui_profile_view_debug){0};
  d->valid = true;
  d->banner_src = look->banner_src;
  d->banner_rgb = look->banner_rgb;
  d->has_fill = look->has_fill;
  snprintf(d->banner_url, sizeof d->banner_url, "%s", look->banner_url);
  d->has_theme = look->has_theme;
  d->theme_top = look->theme_top;
  d->theme_bot = look->theme_bot;
  d->light_text = look->light_text;
  d->badge_count = look->badge_count;
  d->has_deco = look->deco_url[0] != '\0';
  d->server_profile = look->server_profile;
  d->dates_laid_out = dates_laid_out;
  snprintf(d->name, sizeof d->name, "%s", has_text(u->display_name) ? u->display_name : "");
  snprintf(d->bio, sizeof d->bio, "%s", has_text(u->bio) ? u->bio : "");
  snprintf(d->pronouns, sizeof d->pronouns, "%s", has_text(u->pronouns) ? u->pronouns : "");
}

void ui_profile_view_debug_get(const ccord_ui *ui, bool modal, ui_profile_view_debug *out)
{
  (void)ui;
  if (!out) return;
  *out = g_dbg[modal ? 1 : 0];
}
