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
  size_t old_len = 8u;
  char *p = strstr(url, "?size=64");
  if (!p) p = strstr(url, "&size=64");
  if (!p) {
    old_len = 9u;
    p = strstr(url, "?size=128");
    if (!p) p = strstr(url, "&size=128");
  }
  if (!p) return;
  if (p[old_len] != '\0' && p[old_len] != '&') return;
  size_t prefix_len = (size_t)(p + 6 - url);
  char tail[128];
  snprintf(tail, sizeof tail, "%s", p + old_len);
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
  }
  bool match_guild = in_guild && prof && (prof->guild_id == gid || prof->guild_id == CCORD_ID_NONE);
  const char *deco_src = nullptr;
  if (match_guild && has_text(prof->guild_avatar_decoration_url)) {
    deco_src = prof->guild_avatar_decoration_url;
  } else if (prof && has_text(prof->avatar_decoration_url)) {
    deco_src = prof->avatar_decoration_url;
  } else if (has_text(view->avatar_decoration_url)) {
    deco_src = view->avatar_decoration_url;
  }
  if (deco_src) {
    cdn_url(look->deco_url, sizeof look->deco_url, deco_src,
            "https://cdn.discordapp.com/avatar-decoration-presets/", ".png?size=128&passthrough=true");
    view->avatar_decoration_url = look->deco_url;
  }
  const char *np_asset = nullptr, *np_pal = nullptr;
  if (match_guild && has_text(prof->guild_nameplate_asset)) {
    np_asset = prof->guild_nameplate_asset;
    np_pal = prof->guild_nameplate_palette;
  } else if (prof && has_text(prof->nameplate_asset)) {
    np_asset = prof->nameplate_asset;
    np_pal = prof->nameplate_palette;
  } else if (has_text(view->nameplate_asset)) {
    np_asset = view->nameplate_asset;
    np_pal = view->nameplate_palette;
  }
  if (np_asset) {
    snprintf(look->nameplate_asset, sizeof look->nameplate_asset, "%s", np_asset);
    snprintf(look->nameplate_palette, sizeof look->nameplate_palette, "%s", np_pal ? np_pal : "");
  }
  if (prof && prof->profile_effect.id != CCORD_ID_NONE) {
    ccord_id want_eff = match_guild && prof->guild_profile_effect_id != CCORD_ID_NONE
                          ? prof->guild_profile_effect_id
                          : prof->profile_effect_id;
    if (want_eff == CCORD_ID_NONE || prof->profile_effect.id == want_eff || prof->profile_effect.sku_id == want_eff) {
      look->effect = &prof->profile_effect;
    }
  }
  if (prof && prof->profile_frame.sku_id != CCORD_ID_NONE) {
    ccord_id want_frm = match_guild && prof->guild_profile_frame_sku_id != CCORD_ID_NONE
                          ? prof->guild_profile_frame_sku_id
                          : prof->profile_frame_sku_id;
    if (want_frm == CCORD_ID_NONE || prof->profile_frame.sku_id == want_frm) {
      look->frame = &prof->profile_frame;
    }
  }
}

ccord_color ui_profile_banner_fill(const ccord_ui *ui, const ui_profile_look *look, const ccord_user *u)
{
  (void)u;
  if (look->has_fill || look->has_theme) return ui_rgb_color(look->banner_rgb);
  return ui_mix(COL(bg_tertiary), COL(bg_secondary), 0.35f);
}

ccord_color ui_profile_card_bg(const ccord_ui *ui, const ui_profile_look *look, ccord_color fallback)
{
  (void)ui;
  if (!look || !look->has_theme) return fallback;
  // Match the body color directly under the banner (~16% down the Primary -> Accent gradient) under #00000099 / white scrim.
  ccord_color base = ui_mix(ui_rgb_color(look->theme_top), ui_rgb_color(look->theme_bot), 0.16f);
  ccord_color scrim = look->light_text ? (ccord_color){0, 0, 0, 255} : (ccord_color){255, 255, 255, 255};
  return ui_mix(base, scrim, look->light_text ? (153.0f / 255.0f) : (166.0f / 255.0f));
}

static float hsl_hue2rgb(float p, float q, float t)
{
  if (t < 0.0f) t += 1.0f;
  if (t > 1.0f) t -= 1.0f;
  if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
  if (t < 0.5f) return q;
  if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
  return p;
}

ccord_color ui_profile_button_color(const ccord_ui *ui, const ui_profile_look *look)
{
  if (!look || !look->has_theme) return COL(brand);
  float r = channel01((look->theme_top >> 16) & 0xffu);
  float g = channel01((look->theme_top >> 8) & 0xffu);
  float b = channel01(look->theme_top & 0xffu);
  float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b));
  float h = 0.0f, s = 0.0f, l = (mx + mn) * 0.5f;
  float d = mx - mn;
  if (d > 1e-5f) {
    s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    h /= 6.0f;
  }
  // Discord --profile-gradient-button-color: clamp lightness so white label text stays crisp & saturated.
  float lo = look->light_text ? 0.25f : 0.30f;
  float hi = look->light_text ? 0.34f : 0.42f;
  if (l < lo) l = lo;
  if (l > hi) l = hi;
  float rr, gg, bb;
  if (s <= 1e-5f) {
    rr = gg = bb = l;
  } else {
    float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    float p = 2.0f * l - q;
    rr = hsl_hue2rgb(p, q, h + 1.0f / 3.0f);
    gg = hsl_hue2rgb(p, q, h);
    bb = hsl_hue2rgb(p, q, h - 1.0f / 3.0f);
  }
  return (ccord_color){
    (uint8_t)roundf(fminf(fmaxf(rr, 0.0f), 1.0f) * 255.0f),
    (uint8_t)roundf(fminf(fmaxf(gg, 0.0f), 1.0f) * 255.0f),
    (uint8_t)roundf(fminf(fmaxf(bb, 0.0f), 1.0f) * 255.0f),
    255,
  };
}

#define UI_PROFILE_BANNER_FADE_MS 180u
#define UI_NAMEPLATE_IN_MS 200u
#define UI_NAMEPLATE_OUT_MS 150u
#define UI_NAMEPLATE_HOVER_MS 160u
#define UI_NAMEPLATE_TEX_MS 160u

static uint8_t clamp_u8_alpha(float v)
{
  if (v <= 0.0f) return 0u;
  if (v >= 255.0f) return 255u;
  return (uint8_t)roundf(v);
}

static bool prof_reduced_motion(const ccord_ui *ui)
{
  return ui && (ui->anim.reduced_motion || ui->settings_reduced_motion);
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
  uint64_t bkey = look->user_id ^ (look->guild_id * 0x9E3779B1ull);
  float img_vis = ui_fade_ms(ui, "pban", bkey, have_img, UI_PROFILE_BANNER_FADE_MS);
  uint8_t img_a = clamp_u8_alpha(255.0f * img_vis);
  bool anim = !prof_reduced_motion(ui);
  if (look->has_theme) {
    if (!have_img || img_a == 0) return;   // ui_profile_draw_theme_bg already painted the flush vivid themed banner
    ccord_push_clip(r, banner);
    ccord_draw_texture_cover(r, look->banner_url, (ccord_rect){banner.x, banner.y, banner.w, banner.h + radius * 2.0f},
                             radius, img_a, anim);
    ccord_pop_clip(r);
    return;
  }
  ccord_push_clip(r, banner);
  ccord_draw_rrect(r, (ccord_rect){banner.x, banner.y, banner.w, banner.h + radius * 2.0f}, radius,
                   ui_profile_banner_fill(ui, look, u));
  if (have_img && img_a > 0) {
    ccord_draw_texture_cover(r, look->banner_url, (ccord_rect){banner.x, banner.y, banner.w, banner.h + radius * 2.0f},
                             radius, img_a, anim);
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
  enum { N = 32 };
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
  float bh = banner_h;
  if (bh > h) bh = h;
  if (bh > 0.0f) {
    // Vivid, flush top banner (pure banner_rgb / theme_top, zero dark scrim wash — matches Discord .fill_vNEdYY).
    ccord_rect top_zone = {box.x, box.y, box.w, bh};
    ccord_push_clip(r, top_zone);
    ccord_draw_rrect(r, (ccord_rect){box.x, box.y, box.w, bh + radius * 2.0f}, radius,
                     ui_rgb_color(look->banner_rgb));
    ccord_pop_clip(r);
  }
  float body_y = bh > 0.0f ? box.y + bh : box.y;
  float body_h = bot - body_y;
  if (body_h > 0.0f) {
    // Discord --profile-gradient-overlay-color: #00000099 (60% black) on dark themes, 65% white on light themes.
    ccord_rect body_zone = {box.x, body_y, box.w, body_h};
    ccord_color scrim = look->light_text ? (ccord_color){0, 0, 0, 153}
                                         : (ccord_color){255, 255, 255, 166};
    if (bh > 0.0f) {
      ccord_push_clip(r, body_zone);
      ccord_draw_rrect(r, (ccord_rect){box.x, body_y - radius * 2.0f, box.w, body_h + radius * 2.0f},
                       radius, scrim);
      ccord_pop_clip(r);
    } else {
      ccord_draw_rrect(r, body_zone, radius, scrim);
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

void ui_draw_avatar_deco_url_ex(ccord_ui *ui, ccord_renderer *r, const char *deco_url, float cx, float cy,
                                float avatar_size, uint8_t alpha, bool grayscale)
{
  if (!ui || !r || !has_text(deco_url) || alpha == 0 || avatar_size <= 0.0f) return;
  char url[256];
  cdn_url(url, sizeof url, deco_url, "https://cdn.discordapp.com/avatar-decoration-presets/",
          ".png?size=128&passthrough=true");
  float s = avatar_size * 1.2f;
  if (ui_ensure_media_ex(ui, r, url, 256, false)) {
    int tw = 0, th = 0;
    if (ccord_render_has_texture(r, url, &tw, &th) && tw <= 2 && th <= 2) return;
    const char *draw_key = url;
    char gkey[272];
    if (grayscale) {
      snprintf(gkey, sizeof gkey, "__gray__:%s", url);
      const ccord_media_item *cm = ui->d.session ? ccord_session_get_media(ui->d.session, url) : nullptr;
      if (ccord_render_ensure_grayscale_texture(r, url, gkey, cm ? cm->data : nullptr, cm ? cm->len : 0, false, 0, 0)) {
        draw_key = gkey;
      }
    }
    ccord_draw_texture_rounded_anim(r, draw_key, (ccord_rect){cx - s * 0.5f, cy - s * 0.5f, s, s}, 0.0f, alpha,
                                    !grayscale && !prof_reduced_motion(ui));
  }
}

void ui_draw_avatar_deco_url(ccord_ui *ui, ccord_renderer *r, const char *deco_url, float cx, float cy,
                             float avatar_size, uint8_t alpha)
{
  ui_draw_avatar_deco_url_ex(ui, r, deco_url, cx, cy, avatar_size, alpha, false);
}

void ui_profile_draw_deco(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, float cx, float cy, float avatar_size)
{
  if (!look || !look->deco_url[0]) return;
  ui_draw_avatar_deco_url(ui, r, look->deco_url, cx, cy, avatar_size, 255);
}

static uint32_t nameplate_palette_rgb(const char *palette, bool light_ui)
{
  if (has_text(palette)) {
    if (strcmp(palette, "crimson") == 0) return light_ui ? 0xE7040Fu : 0x900007u;
    if (strcmp(palette, "cobalt") == 0) return light_ui ? 0x4278FFu : 0x0131C2u;
    if (strcmp(palette, "berry") == 0) return light_ui ? 0xE55BE8u : 0x893A99u;
    if (strcmp(palette, "bubble_gum") == 0) return light_ui ? 0xFF75B5u : 0xE54B8Eu;
    if (strcmp(palette, "clover") == 0) return light_ui ? 0x4CAF50u : 0x2E7D32u;
    if (strcmp(palette, "forest") == 0) return light_ui ? 0x2E7D32u : 0x1B5E20u;
    if (strcmp(palette, "lemon") == 0) return light_ui ? 0xFFD54Fu : 0xC79100u;
    if (strcmp(palette, "sky") == 0) return light_ui ? 0x29B6F6u : 0x0277BDu;
    if (strcmp(palette, "teal") == 0) return light_ui ? 0x26A69Au : 0x00695Cu;
    if (strcmp(palette, "violet") == 0) return light_ui ? 0x9575CDu : 0x5E35B1u;
    if (strcmp(palette, "indigo") == 0) return light_ui ? 0x5C6BC0u : 0x3949ABu;
    if (strcmp(palette, "white") == 0) return light_ui ? 0xFFFFFFu : 0x9E9E9Eu;
  }
  return 0x5865F2u;
}

static bool url_is_video_asset(const char *url)
{
  if (!url) return false;
  const char *q = strchr(url, '?');
  size_t len = q ? (size_t)(q - url) : strlen(url);
  if (len >= 5 && (memcmp(url + len - 5, ".webm", 5) == 0 || memcmp(url + len - 5, ".WEBM", 5) == 0)) return true;
  if (len >= 4 && (memcmp(url + len - 4, ".mp4", 4) == 0 || memcmp(url + len - 4, ".MP4", 4) == 0)) return true;
  return false;
}

bool ui_draw_nameplate(ccord_ui *ui, ccord_renderer *r, const char *asset, const char *palette, ccord_rect row,
                       float radius, ui_nameplate_opts opt)
{
  if (!ui || !r || row.w <= 0.0f || row.h <= 0.0f) return false;
  bool has_asset = has_text(asset);
  bool active = opt.hover || opt.selected;
  const char *ns = opt.anim_ns ? opt.anim_ns : "np";
  uint64_t id = opt.anim_id;
  float vis = 1.0f;
  float boost = 1.0f;
  if (opt.reveal_on_active) {
    bool want = has_asset && active;
    vis = ui_fade_ms(ui, ns, id * 2u, want, want ? UI_NAMEPLATE_IN_MS : UI_NAMEPLATE_OUT_MS);
    if (!has_asset || (vis <= UI_PRESENCE_EPS && !want)) return false;
    // Reveal-only rows (DM sidebar) fade from full active intensity down to 0 without a mid-fade step drop.
    boost = 1.0f;
  } else {
    if (!has_asset) return false;
    boost = ui_fade_ms(ui, ns, id * 2u, active, UI_NAMEPLATE_HOVER_MS);
  }
  bool allow_fetch = !opt.reveal_on_active || active;
  bool play_anim = opt.animate && !prof_reduced_motion(ui);
  char static_url[256] = {0};
  char anim_url[256] = {0};
  bool direct_http = is_http(asset);
  if (direct_http) {
    snprintf(static_url, sizeof static_url, "%s", asset);
  } else {
    size_t alen = strlen(asset);
    bool slash = alen > 0 && asset[alen - 1] == '/';
    snprintf(static_url, sizeof static_url, "https://cdn.discordapp.com/assets/collectibles/%.127s%sstatic.png",
             asset, slash ? "" : "/");
    snprintf(anim_url, sizeof anim_url, "https://cdn.discordapp.com/assets/collectibles/%.127s%sasset.webm",
             asset, slash ? "" : "/");
  }

  const char *draw_url = nullptr;
  bool draw_anim = false;
  int tw = 0, th = 0;
  if (direct_http) {
    bool is_vid = url_is_video_asset(static_url);
    bool ready = allow_fetch
                     ? (is_vid ? ui_ensure_media_ex(ui, r, static_url, 512, true)
                               : ui_ensure_media_ex(ui, r, static_url, 512, false))
                     : true;
    if (ready && ccord_render_has_texture(r, static_url, &tw, &th) && tw > 2 && th > 2) {
      draw_url = static_url;
      draw_anim = play_anim;
    }
  } else {
    if (play_anim) {
      if (allow_fetch && ui_ensure_media_ex(ui, r, anim_url, 512, true) &&
          ccord_render_has_texture(r, anim_url, &tw, &th) && tw > 2 && th > 2) {
        draw_url = anim_url;
        draw_anim = true;
      }
    }
    if (!draw_url) {
      if (!allow_fetch) {
        // Fading out on the DM list: keep any already-cached texture frozen instead of initiating new fetches.
        if (ccord_render_has_texture(r, anim_url, &tw, &th) && tw > 2 && th > 2) {
          draw_url = anim_url;
          draw_anim = false;
        } else if (ccord_render_has_texture(r, static_url, &tw, &th) && tw > 2 && th > 2) {
          draw_url = static_url;
          draw_anim = false;
        }
      } else if (ui_ensure_media_ex(ui, r, static_url, 512, false) &&
                 ccord_render_has_texture(r, static_url, &tw, &th) && tw > 2 && th > 2) {
        draw_url = static_url;
        draw_anim = false;
      } else if (ccord_render_has_texture(r, anim_url, &tw, &th) && tw > 2 && th > 2) {
        draw_url = anim_url;
        draw_anim = false;
      }
    }
  }
  float tex_vis = ui_fade_ms(ui, ns, id * 2u + 1u, draw_url != nullptr, UI_NAMEPLATE_TEX_MS);
  if (vis <= UI_PRESENCE_EPS) return false;

  ccord_color bg = COL(bg_secondary);
  uint32_t bg_rgb = ((uint32_t)bg.r << 16) | ((uint32_t)bg.g << 8) | (uint32_t)bg.b;
  bool light_ui = rgb_luma(bg_rgb) > 0.50f;
  ccord_color tint = ui_rgb_color(nameplate_palette_rgb(palette, light_ui));

  // Subtle left-to-right palette gradient behind the row (matching Discord's nameplate backdrop).
  float a0 = (opt.offline ? 0.05f : (0.11f + (0.20f - 0.11f) * boost)) * vis;
  float a1 = (opt.offline ? 0.13f : (0.26f + (0.38f - 0.26f) * boost)) * vis;
  enum { NP_STEPS = 12 };
  float x1 = row.x + row.w;
  for (int i = 0; i < NP_STEPS; i++) {
    float t = ((float)i + 0.5f) / (float)NP_STEPS;
    float sx0 = roundf(row.x + row.w * (float)i / (float)NP_STEPS);
    float sx1 = (i == NP_STEPS - 1) ? x1 : roundf(row.x + row.w * (float)(i + 1) / (float)NP_STEPS);
    if (sx1 <= sx0) continue;
    ccord_color sc = ui_alpha(tint, a0 + (a1 - a0) * t);
    ccord_rect strip = {sx0, row.y, sx1 - sx0, row.h};
    if (radius > 0.0f && (sx0 < row.x + radius || sx1 > x1 - radius)) {
      ccord_push_clip(r, strip);
      ccord_draw_rrect(r, row, radius, sc);
      ccord_pop_clip(r);
    } else {
      ccord_draw_rect(r, strip, sc);
    }
  }

  if (draw_url) {
    float img_w = row.h * ((float)tw / (float)th);
    ccord_rect dst = {row.x + row.w - img_w, row.y, img_w, row.h};
    float base_a = opt.offline ? 115.0f : (210.0f + (245.0f - 210.0f) * boost);
    uint8_t img_a = clamp_u8_alpha(base_a * vis * tex_vis);
    if (img_a == 0 && draw_anim) img_a = 1;
    if (img_a > 0) {
      ccord_push_clip(r, row);
      ccord_draw_texture_rounded_anim(r, draw_url, dst, radius, img_a, draw_anim);
      ccord_pop_clip(r);
    }
  }
  return true;
}

typedef struct effect_open_slot {
  ccord_id user_id;
  ccord_id effect_id;
  uint64_t opened_ms;
} effect_open_slot;
static effect_open_slot g_effect_open[2];

void ui_profile_draw_effect(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, ccord_rect box,
                            float radius, bool is_modal)
{
  if (!ui || !r || !look || !look->effect || look->effect->id == CCORD_ID_NONE) return;
  if (box.w <= 0.0f || box.h <= 0.0f) return;
  const ccord_profile_effect_config *cfg = look->effect;
  effect_open_slot *slot = &g_effect_open[is_modal ? 1 : 0];
  if (slot->user_id != look->user_id || slot->effect_id != cfg->id) {
    slot->user_id = look->user_id;
    slot->effect_id = cfg->id;
    slot->opened_ms = ui->now_ms;
  }
  int64_t elapsed = (ui->now_ms >= slot->opened_ms) ? (int64_t)(ui->now_ms - slot->opened_ms) : 0;

  ccord_push_clip(r, box);
  bool reduced = prof_reduced_motion(ui);
  bool any_ready = false;
  if (!reduced) {
    for (size_t i = 0; i < cfg->layer_count && i < CCORD_PROFILE_EFFECT_MAX_LAYERS; i++) {
      const ccord_profile_effect_layer *L = &cfg->layers[i];
      if (!has_text(L->src)) continue;
      // Kick off download even before start_ms so intro->loop transitions seamlessly.
      bool loaded = ui_ensure_media_ex(ui, r, L->src, 512, false);
      if (L->start_ms > 0 && elapsed < (int64_t)L->start_ms) continue;
      if (!L->loop && L->duration_ms > 0 && elapsed >= (int64_t)L->start_ms + (int64_t)L->duration_ms) continue;
      if (L->loop && L->loop_delay_ms > 0 && L->duration_ms > 0) {
        int64_t t = elapsed - (int64_t)L->start_ms;
        int64_t period = (int64_t)L->duration_ms + (int64_t)L->loop_delay_ms;
        if (t >= 0 && period > 0 && (t % period) >= (int64_t)L->duration_ms) continue;
      }
      if (!loaded) continue;
      int tw = 0, th = 0;
      if (!ccord_render_has_texture(r, L->src, &tw, &th) || tw <= 2 || th <= 2) continue;
      float ref_w = L->width > 0 ? (float)L->width : 450.0f;
      float ref_h = L->height > 0 ? (float)L->height : ((float)th * ref_w / (float)tw);
      float scale = box.w / ref_w;
      ccord_rect dst = {box.x + (float)L->pos_x * scale, box.y + (float)L->pos_y * scale, ref_w * scale, ref_h * scale};
      ccord_draw_texture_rounded_anim(r, L->src, dst, radius, 255, true);
      any_ready = true;
    }
  }
  if (!any_ready) {
    const char *fb = has_text(cfg->reduced_motion_url) ? cfg->reduced_motion_url
                     : (has_text(cfg->static_frame_url) ? cfg->static_frame_url
                        : (has_text(cfg->thumbnail_url) ? cfg->thumbnail_url : nullptr));
    if (fb && ui_ensure_media_ex(ui, r, fb, 512, false)) {
      int tw = 0, th = 0;
      if (ccord_render_has_texture(r, fb, &tw, &th) && tw > 2 && th > 2) {
        float dh = box.w * ((float)th / (float)tw);
        ccord_draw_texture_rounded_anim(r, fb, (ccord_rect){box.x, box.y, box.w, dh}, radius, 255, false);
      }
    }
  }
  ccord_pop_clip(r);
}

void ui_profile_draw_frame(ccord_ui *ui, ccord_renderer *r, const ui_profile_look *look, ccord_rect box)
{
  if (!ui || !r || !look || !look->frame || look->frame->sku_id == CCORD_ID_NONE) return;
  if (box.w <= 0.0f || box.h <= 0.0f) return;
  const ccord_profile_frame_config *frm = look->frame;
  float ref_w = frm->inner_width > 0 ? (float)frm->inner_width : 300.0f;
  float scale = box.w / ref_w;
  float ov_t = (float)frm->overflow_top * scale;
  float ov_b = (float)frm->overflow_bottom * scale;
  float ov_h = (float)frm->overflow_horizontal * scale;
  float fw = box.w + ov_h * 2.0f;
  float full_h = box.h + ov_t + ov_b;

  // Support both 0-based (0=top, 1=bottom) and 1-based (1=top, 2=bottom) anchor enums seamlessly.
  bool has_a0 = false, has_a2 = false;
  for (size_t i = 0; i < frm->layer_count && i < CCORD_PROFILE_FRAME_MAX_LAYERS; i++) {
    if (frm->layers[i].anchor == 0) has_a0 = true;
    else if (frm->layers[i].anchor == 2) has_a2 = true;
  }
  int32_t bot_anchor = (!has_a0 && has_a2) ? 2 : 1;
  int32_t top_anchor = (!has_a0 && has_a2) ? 1 : 0;

  bool reduced = prof_reduced_motion(ui);
  bool drew_layer = false;
  for (size_t i = 0; i < frm->layer_count && i < CCORD_PROFILE_FRAME_MAX_LAYERS; i++) {
    const ccord_profile_frame_layer *L = &frm->layers[i];
    const char *url = nullptr;
    if (reduced) {
      if (has_text(L->static_url) && !ui_media_failed(ui, L->static_url)) url = L->static_url;
      else if (has_text(L->url) && !ui_media_failed(ui, L->url)) url = L->url;
    } else {
      if (has_text(L->url) && !ui_media_failed(ui, L->url)) url = L->url;
      else if (has_text(L->static_url) && !ui_media_failed(ui, L->static_url)) url = L->static_url;
    }
    if (!url || !ui_ensure_media_ex(ui, r, url, 512, false)) continue;
    int tw = 0, th = 0;
    if (!ccord_render_has_texture(r, url, &tw, &th) || tw <= 2 || th <= 2) continue;
    float fh = fw * ((float)th / (float)tw);
    ccord_rect dst;
    if (L->anchor == bot_anchor) {
      dst = (ccord_rect){box.x - ov_h, box.y + box.h + ov_b - fh, fw, fh};
    } else if (L->anchor == top_anchor && (!L->responsive || fh <= full_h)) {
      dst = (ccord_rect){box.x - ov_h, box.y - ov_t, fw, fh};
    } else {
      dst = (ccord_rect){box.x - ov_h, box.y - ov_t, fw, full_h};
    }
    ccord_draw_texture_rounded_anim(r, url, dst, 0.0f, 255, !reduced);
    drew_layer = true;
  }
  if (!drew_layer && frm->layer_count == 0) {
    const char *fb = nullptr;
    if (reduced) {
      if (has_text(frm->static_url) && !ui_media_failed(ui, frm->static_url)) fb = frm->static_url;
      else if (has_text(frm->animated_url) && !ui_media_failed(ui, frm->animated_url)) fb = frm->animated_url;
    } else {
      if (has_text(frm->animated_url) && !ui_media_failed(ui, frm->animated_url)) fb = frm->animated_url;
      else if (has_text(frm->static_url) && !ui_media_failed(ui, frm->static_url)) fb = frm->static_url;
    }
    if (fb && ui_ensure_media_ex(ui, r, fb, 512, false)) {
      int tw = 0, th = 0;
      if (ccord_render_has_texture(r, fb, &tw, &th) && tw > 2 && th > 2) {
        ccord_rect dst = {box.x - ov_h, box.y - ov_t, fw, full_h};
        ccord_draw_texture_rounded_anim(r, fb, dst, 0.0f, 255, !reduced);
      }
    }
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
  d->has_effect = look->effect != nullptr && look->effect->id != CCORD_ID_NONE;
  d->has_frame = look->frame != nullptr && look->frame->sku_id != CCORD_ID_NONE;
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
