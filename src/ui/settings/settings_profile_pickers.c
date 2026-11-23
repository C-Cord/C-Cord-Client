// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Real Specialized Cosmetic Picker Modals (Avatar Decoration, Profile Effect, Nameplate, Server Tag)
// and real PNG Image Crop/Zoom/Pan Modal for Settings > Profile.
// Backed exclusively by the user's real owned Discord collectibles (GET /users/@me/collectibles-purchases +
// GET /user-profile-effects) and real guild profiles (GET /guilds/{id}/profile) — zero fake placeholder catalogs.
#include "settings_profile_internal.h"

#define DISCORD_SHOP_URL "https://discord.com/shop"

// ---- HSV / RGB24 Color Helpers for Interactive Color Picker ----

static uint32_t hsv_to_rgb24(float h, float s, float v)
{
  while (h < 0.0f) h += 360.0f;
  while (h >= 360.0f) h -= 360.0f;
  if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
  if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
  float c = v * s;
  float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float m = v - c;
  float r = 0.0f, g = 0.0f, b = 0.0f;
  if (h < 60.0f)       { r = c; g = x; b = 0; }
  else if (h < 120.0f) { r = x; g = c; b = 0; }
  else if (h < 180.0f) { r = 0; g = c; b = x; }
  else if (h < 240.0f) { r = 0; g = x; b = c; }
  else if (h < 300.0f) { r = x; g = 0; b = c; }
  else                 { r = c; g = 0; b = x; }
  uint32_t R = (uint32_t)lroundf((r + m) * 255.0f);
  uint32_t G = (uint32_t)lroundf((g + m) * 255.0f);
  uint32_t B = (uint32_t)lroundf((b + m) * 255.0f);
  if (R > 255u) R = 255u;
  if (G > 255u) G = 255u;
  if (B > 255u) B = 255u;
  return (R << 16) | (G << 8) | B;
}

static void rgb24_to_hsv(uint32_t rgb, float *out_h, float *out_s, float *out_v)
{
  float r = (float)((rgb >> 16) & 0xffu) / 255.0f;
  float g = (float)((rgb >> 8) & 0xffu) / 255.0f;
  float b = (float)(rgb & 0xffu) / 255.0f;
  float mx = fmaxf(r, fmaxf(g, b));
  float mn = fminf(r, fminf(g, b));
  float d = mx - mn;
  float h = 0.0f;
  if (d > 1e-6f) {
    if (mx == r)      h = 60.0f * fmodf((g - b) / d + 6.0f, 6.0f);
    else if (mx == g) h = 60.0f * ((b - r) / d + 2.0f);
    else              h = 60.0f * ((r - g) / d + 4.0f);
  }
  *out_h = h;
  *out_s = mx > 1e-6f ? (d / mx) : 0.0f;
  *out_v = mx;
}

static bool parse_hex24(const char *s, uint32_t *out)
{
  if (!s) return false;
  if (*s == '#') s++;
  if (strlen(s) != 6u) return false;
  uint32_t v = 0;
  for (int i = 0; i < 6; i++) {
    char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
    else return false;
    v = (v << 4) | d;
  }
  *out = v;
  return true;
}

static void color_sync_hex(prof_picker_state *p)
{
  uint32_t rgb = hsv_to_rgb24(p->color_h, p->color_s, p->color_v);
  char hex[16];
  snprintf(hex, sizeof hex, "%06X", (unsigned)rgb);
  ccord_text_edit_clear(&p->search);
  ccord_error err = {0};
  (void)ccord_text_edit_insert(&p->search, hex, strlen(hex), &err);
}

// ---- Real Owned Collectibles Lookup ----

static bool url_contains_asset(const char *url, const char *asset)
{
  return url && url[0] && asset && asset[0] && strstr(url, asset) != nullptr;
}

const ccord_owned_collectible *sp_find_owned_deco(const ccord_ui *ui, ccord_id sku, const char *asset_or_url)
{
  if (!ui || !ui->d.session) return nullptr;
  size_t n = 0;
  const ccord_owned_collectible *list = ccord_session_collectibles(ui->d.session, &n);
  for (size_t i = 0; i < n; i++) {
    if (list[i].kind != CCORD_COLLECTIBLE_AVATAR_DECORATION) continue;
    if ((sku != CCORD_ID_NONE && list[i].sku_id == sku) ||
        url_contains_asset(asset_or_url, list[i].asset)) {
      return &list[i];
    }
  }
  return nullptr;
}

const ccord_owned_collectible *sp_find_owned_effect(const ccord_ui *ui, const char *effect_id_str)
{
  if (!ui || !ui->d.session || !effect_id_str || !effect_id_str[0]) return nullptr;
  ccord_id eid = CCORD_ID_NONE;
  (void)ccord_id_parse(effect_id_str, strlen(effect_id_str), &eid);
  if (eid == CCORD_ID_NONE) return nullptr;
  size_t n = 0;
  const ccord_owned_collectible *list = ccord_session_collectibles(ui->d.session, &n);
  for (size_t i = 0; i < n; i++) {
    if (list[i].kind != CCORD_COLLECTIBLE_PROFILE_EFFECT) continue;
    if (list[i].item_id == eid || list[i].sku_id == eid || list[i].effect.id == eid) {
      return &list[i];
    }
  }
  return nullptr;
}

const ccord_owned_collectible *sp_find_owned_frame(const ccord_ui *ui, ccord_id sku)
{
  if (!ui || !ui->d.session || sku == CCORD_ID_NONE) return nullptr;
  size_t n = 0;
  const ccord_owned_collectible *list = ccord_session_collectibles(ui->d.session, &n);
  for (size_t i = 0; i < n; i++) {
    if (list[i].kind != CCORD_COLLECTIBLE_PROFILE_FRAME) continue;
    if (list[i].sku_id == sku || list[i].item_id == sku || list[i].frame.sku_id == sku) {
      return &list[i];
    }
  }
  return nullptr;
}

const ccord_owned_collectible *sp_find_owned_nameplate(const ccord_ui *ui, ccord_id sku, const char *asset)
{
  if (!ui || !ui->d.session) return nullptr;
  size_t n = 0;
  const ccord_owned_collectible *list = ccord_session_collectibles(ui->d.session, &n);
  for (size_t i = 0; i < n; i++) {
    if (list[i].kind != CCORD_COLLECTIBLE_NAMEPLATE) continue;
    if ((sku != CCORD_ID_NONE && list[i].sku_id == sku) ||
        (asset && asset[0] && strcmp(list[i].asset, asset) == 0)) {
      return &list[i];
    }
  }
  return nullptr;
}

// ---- Real Discord CDN Asset Drawing Helpers ----

static void build_deco_cdn_url(const char *asset_or_url, bool animate, char out[256])
{
  out[0] = '\0';
  if (!asset_or_url || !asset_or_url[0]) return;
  if (strncmp(asset_or_url, "https://", 8) == 0) {
    snprintf(out, 256, "%s", asset_or_url);
    if (!animate) {
      char *p = strstr(out, "passthrough=true");
      if (p) {
        size_t tail = strlen(p + 16);
        if ((size_t)(p - out) + 17u + tail < 256u) {
          memmove(p + 17, p + 16, tail + 1u);
          memcpy(p, "passthrough=false", 17u);
        }
      }
    }
    return;
  }
  snprintf(out, 256, "https://cdn.discordapp.com/avatar-decoration-presets/%.127s.png?size=128&passthrough=%s",
           asset_or_url, animate ? "true" : "false");
}

void sp_draw_avatar_deco_cdn(ccord_ui *ui, ccord_renderer *r, const char *deco_url,
                             float cx, float cy, float avatar_d, bool animate)
{
  if (!ui || !r || !deco_url || !deco_url[0]) return;
  bool want_anim = animate && !ui->anim.reduced_motion;
  char url[256];
  build_deco_cdn_url(deco_url, want_anim, url);
  if (!url[0]) return;
  if (ui_ensure_media(ui, r, url)) {
    int tw = 0, th = 0;
    if (ccord_render_has_texture(r, url, &tw, &th) && tw > 2 && th > 2) {
      float dd = avatar_d * 1.20f;
      ccord_rect drc = {cx - dd * 0.5f, cy - dd * 0.5f, dd, dd};
      ccord_draw_texture_rounded_anim(r, url, drc, 0.0f, 255, want_anim);
    }
  }
}

void sp_draw_profile_effect_real(ccord_ui *ui, ccord_renderer *r, ccord_id uid,
                                 const ccord_profile_effect_config *cfg, const char *fallback_url,
                                 ccord_rect card, float radius, bool animate)
{
  if (!ui || !r || card.w <= 0.0f || card.h <= 0.0f) return;
  bool want_anim = animate && !ui->anim.reduced_motion;
  if (cfg && cfg->id != CCORD_ID_NONE && want_anim) {
    ui_profile_look look = {.user_id = uid, .effect = cfg};
    ui_profile_draw_effect(ui, r, &look, card, radius, true);
    return;
  }
  const char *src = nullptr;
  if (cfg && cfg->id != CCORD_ID_NONE) {
    src = cfg->reduced_motion_url[0] ? cfg->reduced_motion_url
                                     : (cfg->static_frame_url[0] ? cfg->static_frame_url : cfg->thumbnail_url);
  }
  if ((!src || !src[0]) && fallback_url && fallback_url[0]) src = fallback_url;
  if (src && src[0] && ui_ensure_media(ui, r, src)) {
    int tw = 0, th = 0;
    if (ccord_render_has_texture(r, src, &tw, &th) && tw > 2 && th > 2) {
      ccord_push_clip(r, card);
      float dh = card.w * ((float)th / (float)tw);
      ccord_rect dst = {card.x, card.y, card.w, dh > 0.0f ? dh : card.h};
      ccord_draw_texture_rounded_anim(r, src, dst, radius, 255, false);
      ccord_pop_clip(r);
    }
  }
}

void sp_draw_nameplate_strip(ccord_ui *ui, ccord_renderer *r, ccord_rect row, float radius,
                             const char *asset, const char *palette,
                             const ccord_user *u, const char *display_name, bool animate)
{
  uis_tokens tk = uis_tokens_of(ui);
  uis_plate(ui, r, row, radius, COL(bg_secondary));
  if (asset && asset[0]) {
    (void)ui_draw_nameplate(ui, r, asset, (palette && palette[0]) ? palette : "cobalt", row, radius,
                            (ui_nameplate_opts){.hover = true, .selected = true,
                                                .animate = animate && !ui->anim.reduced_motion,
                                                .anim_ns = "profnp", .anim_id = 1});
  }
  float av_d = row.h - 12.0f;
  if (av_d < 22.0f) av_d = 22.0f;
  float acx = row.x + 10.0f + av_d * 0.5f;
  float acy = row.y + row.h * 0.5f;
  if (u) {
    ui_draw_avatar(ui, r, u, acx, acy, av_d, COL(bg_secondary), true);
  } else {
    ccord_draw_circle(r, acx, acy, av_d * 0.5f, COL(brand));
  }
  const char *nm = (display_name && display_name[0]) ? display_name : "Your Name";
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
  float tx = row.x + 10.0f + av_d + 10.0f;
  ccord_text_draw_ellipsis(r, nm, strlen(nm), tx, row.y + (row.h - ui_line_h(ui, ns.size)) * 0.5f,
                           fmaxf(row.x + row.w - tx - 10.0f, 40.0f), &ns);
}

// ---- Picker Modal Helpers ----

static bool ci_contains(const char *hay, const char *needle)
{
  if (!needle || !needle[0]) return true;
  if (!hay || !hay[0]) return false;
  size_t nlen = strlen(needle);
  for (const char *p = hay; *p; p++) {
    size_t i = 0;
    while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
    if (i == nlen) return true;
  }
  return false;
}

static void picker_add_hit(prof_picker_state *p, int kind, int arg, ccord_rect rc)
{
  if (p->nhits >= PROF_PICKER_HITS_MAX) return;
  p->hits[p->nhits++] = (prof_picker_hit){.kind = kind, .arg = arg, .rect = rc};
}

typedef struct clan_option {
  ccord_id guild_id;
  char guild_name[96];
  char tag[16];
  char badge[256];
} clan_option;

// Gathers ONLY real guilds that have an actual Discord Server Tag (from GET /guilds/{id}/profile or the user's
// currently equipped clan guild). Never invents fake tags.
static size_t gather_real_clan_options(const ccord_ui *ui, clan_option *out, size_t max_out, bool *any_loading)
{
  if (any_loading) *any_loading = false;
  if (!ui || !ui->d.store || !out || max_out == 0) return 0;
  size_t n = 0;
  const ccord_user *self = sp_self_user(ui);
  size_t ng = ccord_store_guild_count(ui->d.store);

  for (size_t i = 0; i < ng && n < max_out; i++) {
    const ccord_guild *g = ccord_store_guild_at(ui->d.store, i);
    if (!g || g->id == CCORD_DM_GUILD_ID) continue;
    const ccord_guild_profile *gp = ui->d.session ? ccord_session_guild_profile(ui->d.session, g->id) : nullptr;
    if (ui->d.session && ccord_session_guild_profile_loading(ui->d.session, g->id)) {
      if (any_loading) *any_loading = true;
    }
    bool is_equipped = (self && self->clan_guild_id == g->id && self->clan_tag[0] != '\0');
    const char *tag = (gp && gp->tag[0]) ? gp->tag : (is_equipped ? self->clan_tag : nullptr);
    if (!tag || !tag[0]) continue;
    clan_option *co = &out[n++];
    memset(co, 0, sizeof *co);
    co->guild_id = g->id;
    snprintf(co->guild_name, sizeof co->guild_name, "%s", (g->name && g->name[0]) ? g->name : "Server");
    snprintf(co->tag, sizeof co->tag, "%.15s", tag);
    if (gp && gp->badge_url[0]) snprintf(co->badge, sizeof co->badge, "%.255s", gp->badge_url);
    else if (is_equipped && self->clan_badge[0]) snprintf(co->badge, sizeof co->badge, "%.255s", self->clan_badge);
  }
  return n;
}

// Gathers owned collectibles of `want_kind` from session cache, plus the currently equipped item if not in cache.
static size_t gather_real_collectibles(const ccord_ui *ui, const profile_state *s, ccord_collectible_kind want_kind,
                                       ccord_owned_collectible *out, size_t max_out)
{
  if (!ui || !out || max_out == 0) return 0;
  size_t count = 0;
  if (ui->d.session) {
    size_t total = 0;
    const ccord_owned_collectible *list = ccord_session_collectibles(ui->d.session, &total);
    for (size_t i = 0; i < total && count < max_out; i++) {
      if (list[i].kind != want_kind) continue;
      out[count++] = list[i];
    }
  }
  // Ensure the user's currently equipped item is always selectable/visible even before inventory completes or
  // if acquired via a special entitlement not returned by collectibles-purchases.
  if (s && count < max_out) {
    if (want_kind == CCORD_COLLECTIBLE_AVATAR_DECORATION &&
        (s->base.deco_sku_id != CCORD_ID_NONE || s->base.deco_url[0])) {
      bool found = false;
      for (size_t i = 0; i < count; i++) {
        if ((s->base.deco_sku_id != CCORD_ID_NONE && out[i].sku_id == s->base.deco_sku_id) ||
            url_contains_asset(s->base.deco_url, out[i].asset)) {
          found = true;
          break;
        }
      }
      if (!found && s->base.deco_sku_id != CCORD_ID_NONE) {
        ccord_owned_collectible eq = {
          .kind = CCORD_COLLECTIBLE_AVATAR_DECORATION,
          .sku_id = s->base.deco_sku_id,
          .item_id = s->base.deco_sku_id,
        };
        snprintf(eq.name, sizeof eq.name, "%s", s->base.deco_name[0] ? s->base.deco_name : "Equipped Decoration");
        snprintf(eq.category, sizeof eq.category, "Equipped");
        snprintf(eq.asset, sizeof eq.asset, "%s", s->base.deco_asset);
        build_deco_cdn_url(s->base.deco_url[0] ? s->base.deco_url : s->base.deco_asset, true, eq.cdn_url);
        build_deco_cdn_url(s->base.deco_url[0] ? s->base.deco_url : s->base.deco_asset, false, eq.static_url);
        out[count++] = eq;
      }
    } else if (want_kind == CCORD_COLLECTIBLE_PROFILE_EFFECT && s->base.effect_id[0]) {
      ccord_id eid = CCORD_ID_NONE;
      (void)ccord_id_parse(s->base.effect_id, strlen(s->base.effect_id), &eid);
      bool found = false;
      for (size_t i = 0; i < count; i++) {
        if (eid != CCORD_ID_NONE && (out[i].item_id == eid || out[i].sku_id == eid)) { found = true; break; }
      }
      if (!found && eid != CCORD_ID_NONE) {
        ccord_owned_collectible eq = {
          .kind = CCORD_COLLECTIBLE_PROFILE_EFFECT,
          .sku_id = eid,
          .item_id = eid,
          .has_effect = s->base.has_effect_cfg,
          .effect = s->base.effect_cfg,
        };
        snprintf(eq.name, sizeof eq.name, "%s", s->base.effect_name[0] ? s->base.effect_name : "Equipped Effect");
        snprintf(eq.category, sizeof eq.category, "Equipped");
        snprintf(eq.cdn_url, sizeof eq.cdn_url, "%s", s->base.effect_thumb);
        snprintf(eq.static_url, sizeof eq.static_url, "%s", s->base.effect_thumb);
        out[count++] = eq;
      }
    } else if (want_kind == CCORD_COLLECTIBLE_NAMEPLATE &&
               (s->base.nameplate_sku_id != CCORD_ID_NONE || s->base.nameplate_asset[0])) {
      bool found = false;
      for (size_t i = 0; i < count; i++) {
        if ((s->base.nameplate_sku_id != CCORD_ID_NONE && out[i].sku_id == s->base.nameplate_sku_id) ||
            (s->base.nameplate_asset[0] && strcmp(out[i].asset, s->base.nameplate_asset) == 0)) {
          found = true;
          break;
        }
      }
      if (!found && s->base.nameplate_sku_id != CCORD_ID_NONE) {
        ccord_owned_collectible eq = {
          .kind = CCORD_COLLECTIBLE_NAMEPLATE,
          .sku_id = s->base.nameplate_sku_id,
          .item_id = s->base.nameplate_sku_id,
        };
        snprintf(eq.name, sizeof eq.name, "%s", s->base.nameplate_name[0] ? s->base.nameplate_name : "Equipped Nameplate");
        snprintf(eq.category, sizeof eq.category, "Equipped");
        snprintf(eq.asset, sizeof eq.asset, "%s", s->base.nameplate_asset);
        snprintf(eq.palette, sizeof eq.palette, "%s", s->base.nameplate_palette[0] ? s->base.nameplate_palette : "cobalt");
        out[count++] = eq;
      }
    } else if (want_kind == CCORD_COLLECTIBLE_PROFILE_FRAME && s->base.frame_sku_id != CCORD_ID_NONE) {
      bool found = false;
      for (size_t i = 0; i < count; i++) {
        if (out[i].sku_id == s->base.frame_sku_id || out[i].frame.sku_id == s->base.frame_sku_id) {
          found = true;
          break;
        }
      }
      if (!found) {
        ccord_owned_collectible eq = {
          .kind = CCORD_COLLECTIBLE_PROFILE_FRAME,
          .sku_id = s->base.frame_sku_id,
          .item_id = s->base.frame_sku_id,
          .has_frame = s->base.has_frame_cfg,
          .frame = s->base.frame_cfg,
        };
        snprintf(eq.name, sizeof eq.name, "%s", s->base.frame_name[0] ? s->base.frame_name : "Equipped Frame");
        snprintf(eq.category, sizeof eq.category, "Equipped");
        snprintf(eq.cdn_url, sizeof eq.cdn_url, "%s", s->base.frame_thumb);
        snprintf(eq.static_url, sizeof eq.static_url, "%s", s->base.frame_thumb);
        out[count++] = eq;
      }
    }
  }
  return count;
}

// Collect unique real categories/palettes from the user's actual owned items (filter[0] is always "All").
static size_t collect_real_filters(const ccord_owned_collectible *items, size_t nitems,
                                   char filters[6][48])
{
  snprintf(filters[0], 48, "All");
  size_t nf = 1;
  for (size_t i = 0; i < nitems && nf < 6u; i++) {
    const char *cat = items[i].category[0] ? items[i].category : items[i].palette;
    if (!cat || !cat[0]) continue;
    bool dup = false;
    for (size_t k = 1; k < nf; k++) {
      if (strcmp(filters[k], cat) == 0) { dup = true; break; }
    }
    if (!dup) snprintf(filters[nf++], 48, "%.47s", cat);
  }
  return nf;
}

void sp_picker_open(ccord_ui *ui, profile_state *s, prof_picker_kind kind, int crop_slot)
{
  if (!ui || !s) return;
  prof_picker_state *p = &s->picker;
  if (!p->search_inited) {
    ccord_text_edit_init(&p->search, 96);
    p->search_inited = true;
  } else {
    ccord_text_edit_clear(&p->search);
  }
  p->kind = kind;
  p->crop_slot = crop_slot;
  p->search_focused = false;
  p->filter_idx = 0;
  p->reduced_motion = ui->settings_reduced_motion;
  p->scroll = 0.0f;
  p->max_scroll = 0.0f;
  p->nhits = 0;
  p->staged_sku = CCORD_ID_NONE;
  p->staged_id[0] = p->staged_url[0] = p->staged_palette[0] = p->staged_name[0] = '\0';
  p->staged_tag[0] = p->staged_badge[0] = '\0';
  p->staged_has_effect = false;
  memset(&p->staged_effect, 0, sizeof p->staged_effect);
  p->staged_has_frame = false;
  memset(&p->staged_frame, 0, sizeof p->staged_frame);
  prof_img_draft *id = &s->img[crop_slot == IMG_BANNER ? IMG_BANNER : IMG_AVATAR];
  p->staged_zoom = id->zoom >= 1.0f ? id->zoom : 1.0f;
  p->staged_pan_x = id->pan_x;
  p->staged_pan_y = id->pan_y;

  if (kind == PROF_PICK_DECO) {
    p->staged_sku = s->draft_deco_sku;
    snprintf(p->staged_id, sizeof p->staged_id, "%s", s->draft_deco_asset);
    snprintf(p->staged_url, sizeof p->staged_url, "%s", s->draft_deco_url);
    snprintf(p->staged_name, sizeof p->staged_name, "%s", s->draft_deco_name);
    if (ui->d.session) {
      ccord_error err = {0};
      (void)ccord_session_fetch_collectibles(ui->d.session, false, &err);
    }
  } else if (kind == PROF_PICK_EFFECT) {
    snprintf(p->staged_id, sizeof p->staged_id, "%s", s->draft_effect_id);
    snprintf(p->staged_url, sizeof p->staged_url, "%s", s->draft_effect_thumb);
    snprintf(p->staged_name, sizeof p->staged_name, "%s", s->draft_effect_name);
    p->staged_has_effect = s->draft_has_effect_cfg;
    p->staged_effect = s->draft_effect_cfg;
    if (ui->d.session) {
      ccord_error err = {0};
      (void)ccord_session_fetch_collectibles(ui->d.session, false, &err);
    }
  } else if (kind == PROF_PICK_FRAME) {
    p->staged_sku = s->draft_frame_sku;
    snprintf(p->staged_url, sizeof p->staged_url, "%s", s->draft_frame_thumb);
    snprintf(p->staged_name, sizeof p->staged_name, "%s", s->draft_frame_name);
    p->staged_has_frame = s->draft_has_frame_cfg;
    p->staged_frame = s->draft_frame_cfg;
    if (ui->d.session) {
      ccord_error err = {0};
      (void)ccord_session_fetch_collectibles(ui->d.session, false, &err);
    }
  } else if (kind == PROF_PICK_NAMEPLATE) {
    p->staged_sku = s->draft_np_sku;
    snprintf(p->staged_id, sizeof p->staged_id, "%s", s->draft_np_asset);
    snprintf(p->staged_palette, sizeof p->staged_palette, "%s", s->draft_np_palette);
    snprintf(p->staged_name, sizeof p->staged_name, "%s", s->draft_np_name);
    if (ui->d.session) {
      ccord_error err = {0};
      (void)ccord_session_fetch_collectibles(ui->d.session, false, &err);
    }
  } else if (kind == PROF_PICK_CLAN) {
    p->staged_sku = s->draft_clan_guild;
    snprintf(p->staged_tag, sizeof p->staged_tag, "%s", s->draft_clan_tag);
    snprintf(p->staged_badge, sizeof p->staged_badge, "%s", s->draft_clan_badge);
    // Fetch real guild profiles for joined servers that have GUILD_TAGS / CLAN enabled (plus currently equipped guild).
    if (ui->d.session && ui->d.store) {
      const ccord_user *self = sp_self_user(ui);
      size_t ng = ccord_store_guild_count(ui->d.store);
      size_t fetched = 0;
      for (size_t i = 0; i < ng && fetched < 16u; i++) {
        const ccord_guild *g = ccord_store_guild_at(ui->d.store, i);
        if (!g || g->id == CCORD_DM_GUILD_ID) continue;
        bool want = g->feature_guild_tags || (self && self->clan_guild_id == g->id);
        if (!want) continue;
        if (!ccord_session_guild_profile(ui->d.session, g->id)) {
          ccord_error err = {0};
          (void)ccord_session_guild_profile_fetch(ui->d.session, g->id, &err);
          fetched++;
        }
      }
    }
  } else if (kind == PROF_PICK_COLOR) {
    uint32_t init_rgb = 0x3B82F6u;
    if (crop_slot == COLOR_TARGET_THEME_PRI) {
      init_rgb = s->draft_has_theme ? (s->draft_theme_primary & 0xffffffu) : 0xFF005Bu;
    } else if (crop_slot == COLOR_TARGET_THEME_ACC) {
      init_rgb = s->draft_has_theme ? (s->draft_theme_accent & 0xffffffu) : 0x0B0A09u;
    } else if (s->draft_has_theme) {
      init_rgb = s->draft_theme_primary & 0xffffffu;
    } else if (s->accent >= 0) {
      init_rgb = (uint32_t)s->accent & 0xffffffu;
    }
    rgb24_to_hsv(init_rgb, &p->color_h, &p->color_s, &p->color_v);
    p->color_tex_h = -1.0f;
    p->color_drag = 0;
    color_sync_hex(p);
  }

  ui->account_focus_field = 0;
  ui->dirty = true;
}

void sp_picker_close(ccord_ui *ui, profile_state *s)
{
  if (!s) return;
  s->picker.kind = PROF_PICK_NONE;
  s->picker.search_focused = false;
  s->picker.color_drag = 0;
  s->picker.nhits = 0;
  if (ui) {
    if (ui->touch_mode) ui->touch_text_focused = false;
    ui->dirty = true;
  }
}

void sp_picker_free(profile_state *s)
{
  if (!s) return;
  if (s->picker.search_inited) {
    ccord_text_edit_free(&s->picker.search);
    s->picker.search_inited = false;
  }
  s->picker.kind = PROF_PICK_NONE;
}

static void picker_apply(ccord_ui *ui, profile_state *s)
{
  prof_picker_state *p = &s->picker;
  if (p->kind == PROF_PICK_DECO) {
    s->draft_deco_sku = p->staged_sku;
    snprintf(s->draft_deco_asset, sizeof s->draft_deco_asset, "%s", p->staged_id);
    snprintf(s->draft_deco_url, sizeof s->draft_deco_url, "%s", p->staged_url);
    snprintf(s->draft_deco_name, sizeof s->draft_deco_name, "%s", p->staged_name);
  } else if (p->kind == PROF_PICK_EFFECT) {
    snprintf(s->draft_effect_id, sizeof s->draft_effect_id, "%.63s", p->staged_id);
    snprintf(s->draft_effect_name, sizeof s->draft_effect_name, "%s", p->staged_name);
    snprintf(s->draft_effect_thumb, sizeof s->draft_effect_thumb, "%s", p->staged_url);
    s->draft_has_effect_cfg = p->staged_has_effect;
    s->draft_effect_cfg = p->staged_effect;
  } else if (p->kind == PROF_PICK_FRAME) {
    s->draft_frame_sku = p->staged_sku;
    snprintf(s->draft_frame_name, sizeof s->draft_frame_name, "%s", p->staged_name);
    snprintf(s->draft_frame_thumb, sizeof s->draft_frame_thumb, "%s", p->staged_url);
    s->draft_has_frame_cfg = p->staged_has_frame;
    s->draft_frame_cfg = p->staged_frame;
  } else if (p->kind == PROF_PICK_NAMEPLATE) {
    s->draft_np_sku = p->staged_sku;
    snprintf(s->draft_np_asset, sizeof s->draft_np_asset, "%s", p->staged_id);
    snprintf(s->draft_np_palette, sizeof s->draft_np_palette, "%s", p->staged_palette);
    snprintf(s->draft_np_name, sizeof s->draft_np_name, "%s", p->staged_name);
  } else if (p->kind == PROF_PICK_CLAN) {
    s->draft_clan_guild = p->staged_sku;
    snprintf(s->draft_clan_tag, sizeof s->draft_clan_tag, "%s", p->staged_tag);
    snprintf(s->draft_clan_badge, sizeof s->draft_clan_badge, "%s", p->staged_badge);
  } else if (p->kind == PROF_PICK_CROP) {
    int slot = p->crop_slot == IMG_BANNER ? IMG_BANNER : IMG_AVATAR;
    (void)sp_apply_img_crop(ui, &s->img[slot], slot, p->staged_zoom, p->staged_pan_x, p->staged_pan_y);
  } else if (p->kind == PROF_PICK_COLOR) {
    uint32_t rgb = hsv_to_rgb24(p->color_h, p->color_s, p->color_v);
    if (p->crop_slot == COLOR_TARGET_THEME_PRI) {
      if (!s->draft_has_theme) {
        s->draft_has_theme = true;
        s->draft_theme_accent = 0x0B0A09u;
      }
      s->draft_theme_primary = rgb;
    } else if (p->crop_slot == COLOR_TARGET_THEME_ACC) {
      if (!s->draft_has_theme) {
        s->draft_has_theme = true;
        s->draft_theme_primary = 0x3B82F6u;
      }
      s->draft_theme_accent = rgb;
    } else {
      s->accent = (int32_t)rgb;
      s->hex_bad = false;
      s->err[CCORD_PROFILE_FIELD_ACCENT][0] = '\0';
    }
    sp_sync_color_drafts();
  }
  sp_picker_close(ui, s);
}

// ---- Interactive HSV Color Picker Modal ----

static void ensure_color_textures(ccord_renderer *r, prof_picker_state *p)
{
  ccord_error err = {0};
  if (!ccord_render_has_texture(r, "prof-cp-hue", nullptr, nullptr)) {
    uint8_t hue_px[256u * 4u];
    for (int x = 0; x < 256; x++) {
      uint32_t rgb = hsv_to_rgb24(((float)x / 255.0f) * 360.0f, 1.0f, 1.0f);
      hue_px[x * 4 + 0] = (uint8_t)((rgb >> 16) & 0xffu);
      hue_px[x * 4 + 1] = (uint8_t)((rgb >> 8) & 0xffu);
      hue_px[x * 4 + 2] = (uint8_t)(rgb & 0xffu);
      hue_px[x * 4 + 3] = 255u;
    }
    (void)ccord_render_upload_rgba(r, "prof-cp-hue", hue_px, 256, 1, &err);
  }
  if (!ccord_render_has_texture(r, "prof-cp-sv", nullptr, nullptr) || fabsf(p->color_h - p->color_tex_h) > 0.25f) {
    enum { N = 128 };
    uint8_t *sv_px = ccord_malloc((size_t)N * (size_t)N * 4u);
    if (sv_px) {
      for (int y = 0; y < N; y++) {
        float v = 1.0f - (float)y / (float)(N - 1);
        for (int x = 0; x < N; x++) {
          float s = (float)x / (float)(N - 1);
          uint32_t rgb = hsv_to_rgb24(p->color_h, s, v);
          size_t idx = ((size_t)y * (size_t)N + (size_t)x) * 4u;
          sv_px[idx + 0] = (uint8_t)((rgb >> 16) & 0xffu);
          sv_px[idx + 1] = (uint8_t)((rgb >> 8) & 0xffu);
          sv_px[idx + 2] = (uint8_t)(rgb & 0xffu);
          sv_px[idx + 3] = 255u;
        }
      }
      (void)ccord_render_upload_rgba(r, "prof-cp-sv", sv_px, N, N, &err);
      ccord_free(sv_px);
      p->color_tex_h = p->color_h;
    }
  }
}

static const uint32_t k_color_suggestions[5] = {0xFF005Bu, 0x3B82F6u, 0x10B981u, 0xA855F7u, 0xF59E0Bu};

static void draw_color_modal(ccord_ui *ui, ccord_renderer *r, profile_state *s)
{
  prof_picker_state *p = &s->picker;
  uis_tokens tk = uis_tokens_of(ui);

  float mw = fminf((float)ui->win_w - 32.0f, 344.0f);
  float mh = 456.0f;
  ccord_rect box = {floorf(((float)ui->win_w - mw) * 0.5f), floorf(((float)ui->win_h - mh) * 0.5f), mw, mh};
  p->box = box;

  uis_modal_frame(ui, r, box);
  ensure_color_textures(r, p);

  const char *title = (p->crop_slot == COLOR_TARGET_THEME_PRI) ? "Primary Theme Color"
                    : (p->crop_slot == COLOR_TARGET_THEME_ACC) ? "Accent Theme Color"
                                                               : "Banner Color";
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, tk.fs_title, COL(text_header));
  ccord_text_draw(r, title, strlen(title), box.x + 20.0f, box.y + 18.0f, &ts, 0.0f);
  ccord_rect close_rc = {box.x + box.w - 46.0f, box.y + 12.0f, 32.0f, UIS_H_BTN_SM};
  uis_button(ui, r, close_rc, "X", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, close_rc)}, "pcolx", 1);
  picker_add_hit(p, 1, 0, close_rc);

  // 2D Saturation x Value gradient plane
  ccord_rect sv_rc = {box.x + 20.0f, box.y + 56.0f, box.w - 40.0f, 180.0f};
  p->color_sv_rc = sv_rc;
  ccord_draw_texture_rounded(r, "prof-cp-sv", sv_rc, 6.0f, 255);
  uint32_t cur_rgb = hsv_to_rgb24(p->color_h, p->color_s, p->color_v);
  float rcx = sv_rc.x + p->color_s * sv_rc.w;
  float rcy = sv_rc.y + (1.0f - p->color_v) * sv_rc.h;
  ccord_draw_circle(r, rcx, rcy, 8.5f, (ccord_color){0, 0, 0, 140});
  ccord_draw_circle(r, rcx, rcy, 7.0f, (ccord_color){255, 255, 255, 255});
  ccord_draw_circle(r, rcx, rcy, 5.0f, sp_rgb(cur_rgb));
  picker_add_hit(p, 17, 0, sv_rc);

  // Rainbow Hue bar
  ccord_rect hue_rc = {box.x + 20.0f, sv_rc.y + sv_rc.h + 16.0f, box.w - 40.0f, 16.0f};
  p->color_hue_rc = hue_rc;
  ccord_draw_texture_rounded(r, "prof-cp-hue", hue_rc, 8.0f, 255);
  float hcx = hue_rc.x + (p->color_h / 360.0f) * hue_rc.w;
  float hcy = hue_rc.y + hue_rc.h * 0.5f;
  ccord_draw_circle(r, hcx, hcy, 10.0f, (ccord_color){0, 0, 0, 140});
  ccord_draw_circle(r, hcx, hcy, 8.5f, (ccord_color){255, 255, 255, 255});
  ccord_draw_circle(r, hcx, hcy, 6.0f, sp_rgb(hsv_to_rgb24(p->color_h, 1.0f, 1.0f)));
  picker_add_hit(p, 18, 0, (ccord_rect){hue_rc.x - 4.0f, hue_rc.y - 6.0f, hue_rc.w + 8.0f, hue_rc.h + 12.0f});

  // Live swatch + editable #HEX box
  float row_y = hue_rc.y + hue_rc.h + 16.0f;
  ccord_rect sw = {box.x + 20.0f, row_y, UIS_H_INPUT, UIS_H_INPUT};
  ccord_draw_rrect(r, (ccord_rect){sw.x - 1.0f, sw.y - 1.0f, sw.w + 2.0f, sw.h + 2.0f}, tk.r_control + 1.0f, COL(border_subtle));
  ccord_draw_rrect(r, sw, tk.r_control, sp_rgb(cur_rgb));

  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_body, COL(text_normal));
  float hash_w = ccord_text_measure(r, "#", 1, &hs, 0.0f).x;
  ccord_rect hbox = {sw.x + sw.w + UIS_SP_3, row_y, box.x + box.w - 20.0f - (sw.x + sw.w + UIS_SP_3), UIS_H_INPUT};
  (void)uis_input_padded(ui, r, hbox, UIS_SP_3 + hash_w + UIS_SP_1, UIS_SP_2, &p->search, "FF005B",
                         (uis_state){.hover = ui_in_rect(ui->mx, ui->my, hbox), .focused = p->search_focused},
                         false, "pcolh", 1);
  ccord_text_draw(r, "#", 1, hbox.x + UIS_SP_3, row_y + (UIS_H_INPUT - ui_line_h(ui, hs.size)) * 0.5f, &hs, 0.0f);
  picker_add_hit(p, 4, 0, hbox);

  // 5 Suggested Quick Swatches (matching Discord's Pick a Color popover)
  float sg_y = row_y + UIS_H_INPUT + 14.0f;
  ccord_text_style sls = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
  ccord_text_draw(r, "SUGGESTED", 9, box.x + 20.0f, sg_y, &sls, 0.0f);
  float sw_y = sg_y + ui_line_h(ui, sls.size) + 6.0f;
  float sw_d = 28.0f;
  float sw_gap = floorf((box.w - 40.0f - 5.0f * sw_d) / 4.0f);
  if (sw_gap > 16.0f) sw_gap = 16.0f;
  for (int i = 0; i < 5; i++) {
    ccord_rect sr = {box.x + 20.0f + (float)i * (sw_d + sw_gap), sw_y, sw_d, sw_d};
    uis_swatch(ui, r, sr, sp_rgb(k_color_suggestions[i]),
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, sr), .selected = (cur_rgb == k_color_suggestions[i])},
               "pcolsg", (uint64_t)(i + 1));
    picker_add_hit(p, 19, i, sr);
  }

  // Footer
  float fy = box.y + box.h - UIS_H_BTN - 16.0f;
  uis_divider(ui, r, box.x + 16.0f, fy - 10.0f, box.w - 32.0f);
  ccord_rect app = {box.x + box.w - 112.0f, fy, 92.0f, UIS_H_BTN};
  ccord_rect can = {app.x - 94.0f, fy, 84.0f, UIS_H_BTN};
  uis_button(ui, r, can, "Cancel", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, can)}, "pcolf", 1);
  uis_button(ui, r, app, "Apply", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, app)}, "pcolf", 2);
  picker_add_hit(p, 1, 0, can);
  picker_add_hit(p, 2, 0, app);
}

// ---- Crop / Zoom / Pan Modal ----

static void draw_crop_modal(ccord_ui *ui, ccord_renderer *r, profile_state *s)
{
  prof_picker_state *p = &s->picker;
  uis_tokens tk = uis_tokens_of(ui);
  bool is_banner = (p->crop_slot == IMG_BANNER);
  prof_img_draft *img = &s->img[is_banner ? IMG_BANNER : IMG_AVATAR];

  float mw = fminf((float)ui->win_w - 40.0f, 520.0f);
  float mh = is_banner ? 390.0f : 445.0f;
  ccord_rect box = {floorf(((float)ui->win_w - mw) * 0.5f), floorf(((float)ui->win_h - mh) * 0.5f), mw, mh};
  p->box = box;

  uis_modal_frame(ui, r, box);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, tk.fs_title, COL(text_header));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_muted));
  const char *title = is_banner ? "Crop & Position Banner" : "Crop & Position Avatar";
  ccord_text_draw(r, title, strlen(title), box.x + 20.0f, box.y + 18.0f, &ts, 0.0f);
  ccord_rect close_rc = {box.x + box.w - 44.0f, box.y + 12.0f, 28.0f, 28.0f};
  uis_button(ui, r, close_rc, "X", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, close_rc)}, "pcrx", 1);
  picker_add_hit(p, 1, 0, close_rc);

  float prev_w = is_banner ? (box.w - 48.0f) : 210.0f;
  float prev_h = is_banner ? floorf(prev_w * 0.353f) : 210.0f;  // 680:240 banner or 1:1 avatar
  ccord_rect frame = {floorf(box.x + (box.w - prev_w) * 0.5f), box.y + 56.0f, prev_w, prev_h};
  uis_plate(ui, r, frame, is_banner ? tk.r_surface : prev_h * 0.5f, COL(bg_tertiary));
  if (img->mode == IMG_NEW && sp_ensure_img(ui, r, img)) {
    float z = p->staged_zoom;
    float dw = frame.w * z;
    float dh = frame.h * z;
    float ox = (dw - frame.w) * 0.5f;
    float oy = (dh - frame.h) * 0.5f;
    ccord_rect dst = {frame.x - ox - p->staged_pan_x * ox, frame.y - oy - p->staged_pan_y * oy, dw, dh};
    ccord_push_clip(r, frame);
    ccord_draw_texture_cover(r, img->key, dst, is_banner ? tk.r_surface : prev_h * 0.5f, 255, !p->reduced_motion);
    ccord_pop_clip(r);
  }
  const char *hint = img->is_gif
                       ? "Animated GIFs are uploaded with their original frames preserved."
                       : (is_banner ? "Output: 680 x 240 PNG cropped to the frame above."
                                    : "Output: 512 x 512 PNG cropped to the circular frame above.");
  ccord_text_draw_ellipsis(r, hint, strlen(hint), box.x + 20.0f, frame.y + frame.h + 10.0f, box.w - 40.0f, &ss);

  // Zoom & Pan controls (real crop parameters passed to ccord_image_crop_png!)
  float cy = frame.y + frame.h + 36.0f;
  ccord_rect zm = {box.x + 20.0f, cy, 36.0f, UIS_H_BTN_SM};
  ccord_rect zp = {zm.x + 118.0f, cy, 36.0f, UIS_H_BTN_SM};
  uis_button(ui, r, zm, "-", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, zm), .disabled = img->is_gif}, "pcrz", 1);
  uis_button(ui, r, zp, "+", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, zp), .disabled = img->is_gif}, "pcrz", 2);
  char zlbl[32];
  snprintf(zlbl, sizeof zlbl, "Zoom %.0f%%", (double)(p->staged_zoom * 100.0f));
  ccord_text_style zs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, COL(text_header));
  ccord_text_draw(r, zlbl, strlen(zlbl), zm.x + 42.0f, cy + 5.0f, &zs, 0.0f);
  if (!img->is_gif) {
    picker_add_hit(p, 8, 0, zm);
    picker_add_hit(p, 9, 0, zp);
  }

  float px = zp.x + zp.w + 12.0f;
  const char *pdirs[4] = {"<", ">", "^", "v"};
  for (int i = 0; i < 4; i++) {
    ccord_rect pb = {px + (float)i * 38.0f, cy, 34.0f, UIS_H_BTN_SM};
    uis_button(ui, r, pb, pdirs[i], UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, pb), .disabled = img->is_gif || p->staged_zoom <= 1.01f},
               "pcrp", (uint64_t)(i + 1));
    if (!img->is_gif && p->staged_zoom > 1.01f) picker_add_hit(p, 12 + i, 0, pb);
  }
  ccord_rect rst = {box.x + box.w - 86.0f, cy, 66.0f, UIS_H_BTN_SM};
  uis_button(ui, r, rst, "Reset", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, rst), .disabled = img->is_gif}, "pcrr", 1);
  if (!img->is_gif) picker_add_hit(p, 16, 0, rst);

  // Footer
  float fy = box.y + box.h - UIS_H_BTN - 16.0f;
  ccord_rect apply = {box.x + box.w - 134.0f, fy, 114.0f, UIS_H_BTN};
  ccord_rect cancel = {apply.x - 94.0f, fy, 84.0f, UIS_H_BTN};
  uis_button(ui, r, cancel, "Cancel", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, cancel)}, "pcrf", 1);
  uis_button(ui, r, apply, "Apply Crop", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, apply)}, "pcrf", 2);
  picker_add_hit(p, 1, 0, cancel);
  picker_add_hit(p, 2, 0, apply);
}

// ---- Specialized Cosmetic Picker Modal Drawing ----

void sp_picker_draw(ccord_ui *ui, ccord_renderer *r, profile_state *s)
{
  if (!ui || !r || !s || s->picker.kind == PROF_PICK_NONE) return;
  prof_picker_state *p = &s->picker;
  p->nhits = 0;

  if (p->kind == PROF_PICK_CROP) {
    draw_crop_modal(ui, r, s);
    return;
  }
  if (p->kind == PROF_PICK_COLOR) {
    draw_color_modal(ui, r, s);
    return;
  }

  uis_tokens tk = uis_tokens_of(ui);
  bool compact = (ui->win_w < 780);
  float mw = fminf((float)ui->win_w - 32.0f, 820.0f);
  float mh = fminf((float)ui->win_h - 40.0f, 580.0f);
  ccord_rect box = {floorf(((float)ui->win_w - mw) * 0.5f), floorf(((float)ui->win_h - mh) * 0.5f), mw, mh};
  p->box = box;

  uis_modal_frame(ui, r, box);

  const char *title = "Customize";
  const char *subtitle = "";
  ccord_collectible_kind ckind = CCORD_COLLECTIBLE_AVATAR_DECORATION;
  if (p->kind == PROF_PICK_DECO) {
    title = "Avatar Decorations";
    subtitle = "Your owned Discord avatar decorations — previewed live on your avatar.";
    ckind = CCORD_COLLECTIBLE_AVATAR_DECORATION;
  } else if (p->kind == PROF_PICK_EFFECT) {
    title = "Profile Effects";
    subtitle = "Your owned Discord profile effects — rendered live on your profile card.";
    ckind = CCORD_COLLECTIBLE_PROFILE_EFFECT;
  } else if (p->kind == PROF_PICK_FRAME) {
    title = "Profile Frames";
    subtitle = "Your owned Discord profile frames — rendered live around your profile card.";
    ckind = CCORD_COLLECTIBLE_PROFILE_FRAME;
  } else if (p->kind == PROF_PICK_NAMEPLATE) {
    title = "Nameplates";
    subtitle = "Your owned Discord nameplates — previewed live in a member list row.";
    ckind = CCORD_COLLECTIBLE_NAMEPLATE;
  } else if (p->kind == PROF_PICK_CLAN) {
    title = "Server Tags";
    subtitle = "Choose a Server Tag from any of your joined servers that have one enabled.";
  }

  // Header
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, tk.fs_title, COL(text_header));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_muted));
  ccord_text_draw(r, title, strlen(title), box.x + 20.0f, box.y + 16.0f, &ts, 0.0f);
  ccord_text_draw_ellipsis(r, subtitle, strlen(subtitle), box.x + 20.0f, box.y + 40.0f, box.w - 150.0f, &ss);

  ccord_rect close_rc = {box.x + box.w - 44.0f, box.y + 14.0f, 28.0f, 28.0f};
  uis_button(ui, r, close_rc, "X", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, close_rc)}, "ppkx", 1);
  picker_add_hit(p, 1, 0, close_rc);

  if (p->kind != PROF_PICK_CLAN) {
    ccord_rect ref_rc = {close_rc.x - 86.0f, box.y + 14.0f, 78.0f, 28.0f};
    bool loading = ui->d.session && ccord_session_collectibles_state(ui->d.session) == CCORD_FETCH_LOADING;
    uis_button(ui, r, ref_rc, loading ? "..." : "Refresh", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, ref_rc), .disabled = loading}, "ppkr", 1);
    if (!loading) picker_add_hit(p, 10, 0, ref_rc);
  }

  // Gather real owned items (or real guild server tags)
  ccord_owned_collectible owned[CCORD_COLLECTIBLES_MAX];
  size_t nowned = 0;
  clan_option clans[64];
  size_t nclans = 0;
  bool clans_loading = false;
  if (p->kind == PROF_PICK_CLAN) {
    nclans = gather_real_clan_options(ui, clans, 64, &clans_loading);
  } else {
    nowned = gather_real_collectibles(ui, s, ckind, owned, CCORD_COLLECTIBLES_MAX);
  }

  // Search box + dynamic real filter chips
  float top_y = box.y + 66.0f;
  ccord_rect sbox = {box.x + 20.0f, top_y, compact ? (box.w - 40.0f) : 240.0f, UIS_H_INPUT};
  uis_input(ui, r, sbox, &p->search, "Search your collection...",
            (uis_state){.hover = ui_in_rect(ui->mx, ui->my, sbox), .focused = p->search_focused}, false, "ppks", 1);
  picker_add_hit(p, 4, 0, sbox);

  char filters[6][48];
  size_t nfilt = 0;
  if (p->kind != PROF_PICK_CLAN && nowned > 0) {
    nfilt = collect_real_filters(owned, nowned, filters);
  }
  if (p->filter_idx >= (int)nfilt) p->filter_idx = 0;
  float fx = compact ? (box.x + 20.0f) : (sbox.x + sbox.w + 12.0f);
  float fy = compact ? (sbox.y + sbox.h + 8.0f) : (top_y + 4.0f);
  for (size_t i = 0; i < nfilt && nfilt > 1u; i++) {
    float cw = fmaxf(uis_label_width(ui, r, filters[i]) - 6.0f, 44.0f);
    if (fx + cw > box.x + box.w - 20.0f) break;
    ccord_rect crc = {fx, fy, cw, UIS_H_CHIP};
    uis_chip(ui, r, crc, filters[i],
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, crc), .selected = (p->filter_idx == (int)i)},
             "ppkc", (uint64_t)(i + 1));
    picker_add_hit(p, 5, (int)i, crc);
    fx += cw + 6.0f;
  }

  float body_top = (compact && nfilt > 1u) ? (fy + UIS_H_CHIP + 12.0f) : (sbox.y + sbox.h + 12.0f);
  float footer_h = UIS_H_BTN + 28.0f;
  float body_h = fmaxf(box.y + box.h - footer_h - body_top, 140.0f);

  float preview_w = compact ? 0.0f : 276.0f;
  float list_w = compact ? (box.w - 40.0f) : (box.w - 40.0f - preview_w - 16.0f);
  ccord_rect list_rc = {box.x + 20.0f, body_top, list_w, body_h};
  p->list_rect = list_rc;

  const char *query = ccord_text_edit_cstr(&p->search);
  const char *active_cat = (nfilt > 1u && p->filter_idx > 0 && (size_t)p->filter_idx < nfilt)
                             ? filters[p->filter_idx] : nullptr;
  const ccord_user *self = sp_self_user(ui);
  const char *disp_name = ccord_text_edit_cstr(&s->ed[F_NAME]);
  if (!disp_name || !disp_name[0]) disp_name = (self && self->name && self->name[0]) ? self->name : "You";

  ccord_fetch_state cstate = ui->d.session ? ccord_session_collectibles_state(ui->d.session) : CCORD_FETCH_IDLE;

  // ---- Left Panel: Scrollable Real Collection Grid (visual-only tiles for Deco / Effect / Nameplate) ----
  ccord_push_clip(r, list_rc);
  float gx = list_rc.x;
  float gy = list_rc.y - p->scroll;
  int cols = 1;
  if (p->kind == PROF_PICK_DECO || p->kind == PROF_PICK_EFFECT || p->kind == PROF_PICK_FRAME) {
    cols = (list_rc.w >= 300.0f) ? 3 : 2;
  } else if (p->kind == PROF_PICK_NAMEPLATE) {
    cols = (list_rc.w >= 340.0f) ? 2 : 1;
  }
  float gap = UIS_SP_2;
  float cell_w = floorf((list_rc.w - gap * (float)(cols - 1)) / (float)cols);
  float cell_h = 56.0f;
  if (p->kind == PROF_PICK_DECO) cell_h = cell_w;
  else if (p->kind == PROF_PICK_EFFECT) cell_h = floorf(cell_w * 1.25f);
  else if (p->kind == PROF_PICK_FRAME) cell_h = floorf(cell_w * 1.15f);
  else if (p->kind == PROF_PICK_NAMEPLATE) cell_h = 54.0f;

  int vis_idx = 0;
  // Item 0: "None" clear option
  if (ci_contains("None", query)) {
    ccord_rect cr = {gx + (float)(vis_idx % cols) * (cell_w + gap),
                     gy + (float)(vis_idx / cols) * (cell_h + gap), cell_w, cell_h};
    bool sel = (p->kind == PROF_PICK_EFFECT) ? (p->staged_id[0] == '\0') : (p->staged_sku == CCORD_ID_NONE);
    if (sel) {
      ccord_draw_rrect(r, cr, tk.r_surface, COL(brand));
      ccord_draw_rrect(r, (ccord_rect){cr.x + 2.0f, cr.y + 2.0f, cr.w - 4.0f, cr.h - 4.0f},
                       fmaxf(tk.r_surface - 2.0f, 2.0f), COL(bg_secondary));
    } else {
      uis_card(ui, r, cr);
    }
    ccord_text_style nt = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, COL(text_header));
    if (p->kind == PROF_PICK_CLAN) {
      ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_muted));
      ccord_text_draw_ellipsis(r, "None", 4, cr.x + 12.0f, cr.y + 10.0f, cr.w - 24.0f, &nt);
      ccord_text_draw_ellipsis(r, "Hide server tag", 15, cr.x + 12.0f, cr.y + 30.0f, cr.w - 24.0f, &ns);
    } else {
      // Minimal crossed-circle icon + "None" label
      float icx = (p->kind == PROF_PICK_NAMEPLATE) ? (cr.x + 26.0f) : (cr.x + cr.w * 0.5f);
      float icy = (p->kind == PROF_PICK_NAMEPLATE) ? (cr.y + cr.h * 0.5f) : (cr.y + cr.h * 0.42f);
      ccord_draw_circle(r, icx, icy, 12.0f, COL(border_subtle));
      ccord_draw_circle(r, icx, icy, 9.5f, COL(bg_secondary));
      ccord_draw_line(r, (ccord_vec2){icx - 7.5f, icy - 7.5f}, (ccord_vec2){icx + 7.5f, icy + 7.5f}, 2.0f, COL(text_muted));
      ccord_vec2 nm = ccord_text_measure(r, "None", 4, &nt, 0.0f);
      if (p->kind == PROF_PICK_NAMEPLATE) {
        ccord_text_draw(r, "None", 4, icx + 20.0f, cr.y + (cr.h - nm.y) * 0.5f, &nt, 0.0f);
      } else {
        ccord_text_draw(r, "None", 4, cr.x + (cr.w - nm.x) * 0.5f, icy + 18.0f, &nt, 0.0f);
      }
    }
    picker_add_hit(p, 6, -1, cr);
    vis_idx++;
  }

  if (p->kind == PROF_PICK_CLAN) {
    for (size_t i = 0; i < nclans; i++) {
      if (!ci_contains(clans[i].guild_name, query) && !ci_contains(clans[i].tag, query)) continue;
      ccord_rect cr = {gx, gy + (float)vis_idx * (cell_h + gap), cell_w, cell_h};
      bool sel = (p->staged_sku == clans[i].guild_id);
      if (sel) {
        ccord_draw_rrect(r, cr, tk.r_surface, COL(brand));
        ccord_draw_rrect(r, (ccord_rect){cr.x + 2.0f, cr.y + 2.0f, cr.w - 4.0f, cr.h - 4.0f},
                         fmaxf(tk.r_surface - 2.0f, 2.0f), COL(bg_secondary));
      } else {
        uis_card(ui, r, cr);
      }
      ccord_text_style nt = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, COL(text_header));
      float bw = ui_draw_clan_badge_ex(ui, r, clans[i].guild_id, clans[i].tag, clans[i].badge,
                                       cr.x + 12.0f, cr.y + (cr.h - 20.0f) * 0.5f, 20.0f, UI_CLAN_LAYER_NONE);
      ccord_text_draw_ellipsis(r, clans[i].guild_name, strlen(clans[i].guild_name),
                               cr.x + 20.0f + bw, cr.y + (cr.h - ui_line_h(ui, nt.size)) * 0.5f,
                               fmaxf(cr.w - 30.0f - bw, 60.0f), &nt);
      picker_add_hit(p, 6, (int)i, cr);
      vis_idx++;
    }
    if (nclans == 0) {
      float ey = gy + (float)vis_idx * (cell_h + gap) + 12.0f;
      const char *msg = clans_loading
                          ? "Checking your joined servers for Server Tags..."
                          : "None of your joined servers have a Server Tag enabled.";
      ccord_text_draw(r, msg, strlen(msg), gx + 4.0f, ey, &ss, list_rc.w - 8.0f);
    }
  } else {
    for (size_t i = 0; i < nowned; i++) {
      const ccord_owned_collectible *e = &owned[i];
      const char *ecat = e->category[0] ? e->category : e->palette;
      if (active_cat && (!ecat || strcmp(ecat, active_cat) != 0)) continue;
      if (!ci_contains(e->name, query) && !ci_contains(ecat, query)) continue;
      ccord_rect cr = {gx + (float)(vis_idx % cols) * (cell_w + gap),
                       gy + (float)(vis_idx / cols) * (cell_h + gap), cell_w, cell_h};
      char eff_id_buf[32] = "";
      if (p->kind == PROF_PICK_EFFECT) (void)ccord_id_format(e->item_id, eff_id_buf);
      bool sel = (p->kind == PROF_PICK_EFFECT) ? (strcmp(p->staged_id, eff_id_buf) == 0)
                                               : (p->staged_sku == e->sku_id);
      bool hov = ui_in_rect(ui->mx, ui->my, cr) && ui_in_rect(ui->mx, ui->my, list_rc);
      if (sel || hov) {
        ccord_draw_rrect(r, cr, tk.r_surface, sel ? COL(brand) : COL(border_subtle));
        ccord_draw_rrect(r, (ccord_rect){cr.x + 2.0f, cr.y + 2.0f, cr.w - 4.0f, cr.h - 4.0f},
                         fmaxf(tk.r_surface - 2.0f, 2.0f), COL(bg_secondary));
      } else {
        uis_card(ui, r, cr);
      }

      if (p->kind == PROF_PICK_DECO) {
        // Pure visual tile: ONLY the real Discord CDN avatar decoration artwork (zero text overlay)
        float dcx = cr.x + cr.w * 0.5f, dcy = cr.y + cr.h * 0.5f;
        float av_d = fminf(cr.w, cr.h) * 0.62f;
        sp_draw_avatar_deco_cdn(ui, r, e->cdn_url[0] ? e->cdn_url : e->asset,
                                dcx, dcy, av_d, !p->reduced_motion && (hov || sel));
      } else if (p->kind == PROF_PICK_EFFECT || p->kind == PROF_PICK_FRAME) {
        // Pure visual tile: ONLY the real Discord CDN artwork filling the tile (zero text overlay)
        ccord_rect inner = {cr.x + 4.0f, cr.y + 4.0f, cr.w - 8.0f, cr.h - 8.0f};
        float ir = fmaxf(tk.r_surface - 3.0f, 3.0f);
        uis_plate(ui, r, inner, ir, COL(bg_tertiary));
        const char *thumb = p->reduced_motion
                              ? (e->static_url[0] ? e->static_url : e->cdn_url)
                              : (e->cdn_url[0] ? e->cdn_url : e->static_url);
        if (thumb && thumb[0] && ui_ensure_media(ui, r, thumb)) {
          int tw = 0, th = 0;
          if (ccord_render_has_texture(r, thumb, &tw, &th) && tw > 2 && th > 2) {
            ccord_draw_texture_cover(r, thumb, inner, ir, 255, !p->reduced_motion && (hov || sel));
          }
        }
      } else {
        // Pure visual tile: ONLY the real Discord CDN nameplate artwork (zero avatar or text drawn on top)
        ccord_rect inner = {cr.x + 4.0f, cr.y + 4.0f, cr.w - 8.0f, cr.h - 8.0f};
        float ir = fmaxf(tk.r_surface - 3.0f, 3.0f);
        uis_plate(ui, r, inner, ir, COL(bg_tertiary));
        (void)ui_draw_nameplate(ui, r, e->asset, e->palette[0] ? e->palette : "cobalt", inner, ir,
                                (ui_nameplate_opts){.hover = hov || sel, .selected = sel,
                                                    .animate = !p->reduced_motion && (hov || sel),
                                                    .anim_ns = "ppknp", .anim_id = (uint64_t)(i + 1)});
      }
      picker_add_hit(p, 6, (int)i, cr);
      vis_idx++;
    }

    // Real loading / error / empty states underneath "None" when no items are owned
    if (nowned == 0) {
      int rows_used = (vis_idx + cols - 1) / cols;
      float sy = gy + (float)rows_used * (cell_h + gap) + 12.0f;
      if (cstate == CCORD_FETCH_LOADING || cstate == CCORD_FETCH_IDLE) {
        const char *lmsg = "Loading your owned Discord items...";
        ccord_text_draw(r, lmsg, strlen(lmsg), gx + 4.0f, sy, &ss, list_rc.w - 8.0f);
      } else if (cstate == CCORD_FETCH_FAILED) {
        const char *emsg = ui->d.session ? ccord_session_collectibles_error(ui->d.session) : "";
        if (!emsg || !emsg[0]) emsg = "Couldn't load your Discord collection.";
        ccord_text_draw(r, emsg, strlen(emsg), gx + 4.0f, sy, &ss, list_rc.w - 8.0f);
        ccord_rect rbtn = {gx + 4.0f, sy + 28.0f, 96.0f, UIS_H_BTN_SM};
        uis_button(ui, r, rbtn, "Retry", UIS_BTN_PRIMARY, UIS_BTN_S_SM,
                   (uis_state){.hover = ui_in_rect(ui->mx, ui->my, rbtn)}, "ppkrt", 1);
        picker_add_hit(p, 10, 0, rbtn);
      } else {
        const char *kind_s = (p->kind == PROF_PICK_DECO) ? "avatar decorations"
                           : (p->kind == PROF_PICK_EFFECT) ? "profile effects"
                           : (p->kind == PROF_PICK_FRAME) ? "profile frames" : "nameplates";
        char emsg[160];
        snprintf(emsg, sizeof emsg, "You don't own any %s on this Discord account yet.", kind_s);
        ccord_text_draw(r, emsg, strlen(emsg), gx + 4.0f, sy, &ss, list_rc.w - 8.0f);
        ccord_rect sbtn = {gx + 4.0f, sy + 32.0f, 152.0f, UIS_H_BTN_SM};
        uis_button(ui, r, sbtn, "Open Discord Shop", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
                   (uis_state){.hover = ui_in_rect(ui->mx, ui->my, sbtn)}, "ppksh", 1);
        picker_add_hit(p, 11, 0, sbtn);
      }
    }
  }
  ccord_pop_clip(r);

  int total_rows = (vis_idx + cols - 1) / cols;
  float total_h = (float)total_rows * (cell_h + gap) + (nowned == 0 ? 90.0f : 8.0f);
  p->max_scroll = total_h > list_rc.h ? (total_h - list_rc.h) : 0.0f;
  if (p->scroll > p->max_scroll) p->scroll = p->max_scroll;

  // ---- Right Panel: Live Contextual Preview ----
  if (!compact) {
    ccord_rect prc = {list_rc.x + list_rc.w + 16.0f, body_top, preview_w, body_h};
    uis_card(ui, r, prc);
    ccord_text_style pl = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
    ccord_text_draw(r, "LIVE PREVIEW", 12, prc.x + 14.0f, prc.y + 12.0f, &pl, 0.0f);

    if (p->kind == PROF_PICK_DECO) {
      float acx = prc.x + prc.w * 0.5f;
      float acy = prc.y + 112.0f;
      if (self) {
        ccord_user av = *self;
        av.avatar_decoration_url = "";
        ui_draw_avatar(ui, r, &av, acx, acy, 92.0f, COL(bg_secondary), false);
      } else {
        ccord_draw_circle(r, acx, acy, 46.0f, COL(brand));
      }
      if (p->staged_sku != CCORD_ID_NONE || p->staged_url[0] || p->staged_id[0]) {
        sp_draw_avatar_deco_cdn(ui, r, p->staged_url[0] ? p->staged_url : p->staged_id,
                                acx, acy, 92.0f, !p->reduced_motion);
      }
      const char *nm = p->staged_name[0] ? p->staged_name : "No Avatar Decoration";
      ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
      float nw = ccord_text_measure(r, nm, strlen(nm), &ns, 0.0f).x;
      ccord_text_draw_ellipsis(r, nm, strlen(nm), prc.x + fmaxf((prc.w - nw) * 0.5f, 12.0f), prc.y + 178.0f,
                               prc.w - 24.0f, &ns);
    } else if (p->kind == PROF_PICK_EFFECT) {
      ccord_rect card = {prc.x + 14.0f, prc.y + 34.0f, prc.w - 28.0f, 206.0f};
      uis_plate(ui, r, card, tk.r_surface, COL(bg_primary));
      ccord_color bcol = s->draft_has_theme ? sp_rgb(s->draft_theme_primary)
                                            : (s->accent >= 0 ? sp_rgb((uint32_t)s->accent) : COL(brand));
      ccord_draw_rrect(r, (ccord_rect){card.x, card.y, card.w, 64.0f}, tk.r_surface, bcol);
      if (self) ui_draw_avatar(ui, r, self, card.x + 42.0f, card.y + 64.0f, 52.0f, COL(bg_primary), true);
      else ccord_draw_circle(r, card.x + 42.0f, card.y + 64.0f, 26.0f, COL(brand));
      ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
      ccord_text_draw_ellipsis(r, disp_name, strlen(disp_name), card.x + 14.0f, card.y + 98.0f, card.w - 28.0f, &ns);
      if (p->staged_id[0]) {
        sp_draw_profile_effect_real(ui, r, self ? self->id : CCORD_ID_NONE,
                                    p->staged_has_effect ? &p->staged_effect : nullptr,
                                    p->staged_url, card, tk.r_surface, !p->reduced_motion);
      }
      const char *enm = p->staged_name[0] ? p->staged_name : "No Profile Effect";
      float nw = ccord_text_measure(r, enm, strlen(enm), &ns, 0.0f).x;
      ccord_text_draw_ellipsis(r, enm, strlen(enm), prc.x + fmaxf((prc.w - nw) * 0.5f, 12.0f),
                               card.y + card.h + 12.0f, prc.w - 24.0f, &ns);
    } else if (p->kind == PROF_PICK_FRAME) {
      ccord_rect card = {prc.x + 22.0f, prc.y + 42.0f, prc.w - 44.0f, 196.0f};
      uis_plate(ui, r, card, tk.r_surface, COL(bg_primary));
      ccord_color bcol = s->draft_has_theme ? sp_rgb(s->draft_theme_primary)
                                            : (s->accent >= 0 ? sp_rgb((uint32_t)s->accent) : COL(brand));
      ccord_push_clip(r, (ccord_rect){card.x, card.y, card.w, 60.0f});
      ccord_draw_rrect(r, (ccord_rect){card.x, card.y, card.w, 60.0f + tk.r_surface * 2.0f}, tk.r_surface, bcol);
      ccord_pop_clip(r);
      if (self) ui_draw_avatar(ui, r, self, card.x + 40.0f, card.y + 60.0f, 48.0f, COL(bg_primary), true);
      else ccord_draw_circle(r, card.x + 40.0f, card.y + 60.0f, 24.0f, COL(brand));
      ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
      ccord_text_draw_ellipsis(r, disp_name, strlen(disp_name), card.x + 14.0f, card.y + 94.0f, card.w - 28.0f, &ns);
      if (p->staged_sku != CCORD_ID_NONE) {
        const ccord_profile_frame_config *fcfg = p->staged_has_frame ? &p->staged_frame : nullptr;
        if (!fcfg && ui->d.session) fcfg = ccord_session_find_owned_frame(ui->d.session, p->staged_sku);
        if (fcfg) {
          ui_profile_look flook = {.user_id = self ? self->id : CCORD_ID_NONE, .frame = fcfg};
          ui_profile_draw_frame(ui, r, &flook, card);
        } else if (p->staged_url[0] && ui_ensure_media(ui, r, p->staged_url)) {
          int tw = 0, th = 0;
          if (ccord_render_has_texture(r, p->staged_url, &tw, &th) && tw > 2 && th > 2) {
            ccord_draw_texture_rounded_anim(r, p->staged_url, card, tk.r_surface, 255, !p->reduced_motion);
          }
        }
      }
      const char *fnm = p->staged_name[0] ? p->staged_name : "No Profile Frame";
      float nw = ccord_text_measure(r, fnm, strlen(fnm), &ns, 0.0f).x;
      ccord_text_draw_ellipsis(r, fnm, strlen(fnm), prc.x + fmaxf((prc.w - nw) * 0.5f, 12.0f),
                               card.y + card.h + 14.0f, prc.w - 24.0f, &ns);
    } else if (p->kind == PROF_PICK_NAMEPLATE) {
      float ry = prc.y + 40.0f;
      ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
      ccord_text_draw(r, "ONLINE — 3", 10, prc.x + 14.0f, ry, &hs, 0.0f);
      ry += 20.0f;
      uis_plate(ui, r, (ccord_rect){prc.x + 12.0f, ry, prc.w - 24.0f, 40.0f}, tk.r_control, COL(bg_primary));
      ccord_draw_circle(r, prc.x + 32.0f, ry + 20.0f, 13.0f, COL(bg_tertiary));
      ccord_text_draw(r, "Ada", 3, prc.x + 54.0f, ry + 11.0f, &ss, 0.0f);
      ry += 46.0f;
      ccord_rect row = {prc.x + 12.0f, ry, prc.w - 24.0f, 46.0f};
      sp_draw_nameplate_strip(ui, r, row, tk.r_control, p->staged_id, p->staged_palette,
                              self, disp_name, !p->reduced_motion);
      ry += 52.0f;
      uis_plate(ui, r, (ccord_rect){prc.x + 12.0f, ry, prc.w - 24.0f, 40.0f}, tk.r_control, COL(bg_primary));
      ccord_draw_circle(r, prc.x + 32.0f, ry + 20.0f, 13.0f, COL(bg_tertiary));
      ccord_text_draw(r, "Grace", 5, prc.x + 54.0f, ry + 11.0f, &ss, 0.0f);
      const char *nm = p->staged_name[0] ? p->staged_name : "No Nameplate";
      ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
      float nw = ccord_text_measure(r, nm, strlen(nm), &ns, 0.0f).x;
      ccord_text_draw_ellipsis(r, nm, strlen(nm), prc.x + fmaxf((prc.w - nw) * 0.5f, 12.0f),
                               ry + 54.0f, prc.w - 24.0f, &ns);
    } else if (p->kind == PROF_PICK_CLAN) {
      ccord_rect chat = {prc.x + 12.0f, prc.y + 42.0f, prc.w - 24.0f, 94.0f};
      uis_plate(ui, r, chat, tk.r_control, COL(bg_primary));
      if (self) ui_draw_avatar(ui, r, self, chat.x + 28.0f, chat.y + 32.0f, 34.0f, COL(bg_primary), true);
      else ccord_draw_circle(r, chat.x + 28.0f, chat.y + 32.0f, 17.0f, COL(brand));
      ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, COL(text_header));
      float nw = ccord_text_draw_ellipsis(r, disp_name, strlen(disp_name), chat.x + 54.0f, chat.y + 14.0f, 110.0f, &ns);
      if (p->staged_sku != CCORD_ID_NONE && p->staged_tag[0]) {
        (void)ui_draw_clan_badge_ex(ui, r, p->staged_sku, p->staged_tag, p->staged_badge,
                                    chat.x + 60.0f + nw, chat.y + 13.0f, 18.0f, UI_CLAN_LAYER_NONE);
      }
      ccord_text_draw_ellipsis(r, "Looking good in chat!", 21, chat.x + 54.0f, chat.y + 42.0f, chat.w - 64.0f, &ss);
    }

    // Reduced Motion toggle inside preview panel for animated cosmetics
    if (p->kind == PROF_PICK_DECO || p->kind == PROF_PICK_EFFECT || p->kind == PROF_PICK_FRAME ||
        p->kind == PROF_PICK_NAMEPLATE) {
      ccord_rect mrow = {prc.x + 14.0f, prc.y + prc.h - 40.0f, prc.w - 28.0f, 28.0f};
      ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_secondary));
      ccord_text_draw(r, "Reduced Motion", 14, mrow.x, mrow.y + 5.0f, &ms, 0.0f);
      uis_toggle(ui, r, mrow, p->reduced_motion, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, mrow)}, "ppkm", 1);
      picker_add_hit(p, 7, 0, mrow);
    }
  }

  // ---- Footer Actions ----
  float foot_y = box.y + box.h - UIS_H_BTN - 14.0f;
  uis_divider(ui, r, box.x + 16.0f, foot_y - 10.0f, box.w - 32.0f);
  ccord_rect clr = {box.x + 20.0f, foot_y, 92.0f, UIS_H_BTN};
  ccord_rect app = {box.x + box.w - 112.0f, foot_y, 92.0f, UIS_H_BTN};
  ccord_rect can = {app.x - 94.0f, foot_y, 84.0f, UIS_H_BTN};
  uis_button(ui, r, clr, "Clear", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, clr)}, "ppkf", 1);
  uis_button(ui, r, can, "Cancel", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, can)}, "ppkf", 2);
  uis_button(ui, r, app, "Apply", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, app)}, "ppkf", 3);
  picker_add_hit(p, 3, 0, clr);
  picker_add_hit(p, 1, 0, can);
  picker_add_hit(p, 2, 0, app);
}

// ---- Picker Modal Input ----

static void color_update_sv(prof_picker_state *p, float x, float y)
{
  if (p->color_sv_rc.w <= 0.0f || p->color_sv_rc.h <= 0.0f) return;
  float s = (x - p->color_sv_rc.x) / p->color_sv_rc.w;
  float v = 1.0f - (y - p->color_sv_rc.y) / p->color_sv_rc.h;
  if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
  if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
  p->color_s = s;
  p->color_v = v;
  color_sync_hex(p);
}

static void color_update_hue(prof_picker_state *p, float x)
{
  if (p->color_hue_rc.w <= 0.0f) return;
  float t = (x - p->color_hue_rc.x) / p->color_hue_rc.w;
  if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
  p->color_h = t * 359.5f;
  color_sync_hex(p);
}

static void color_apply_hex_text(prof_picker_state *p)
{
  uint32_t rgb = 0;
  if (parse_hex24(ccord_text_edit_cstr(&p->search), &rgb)) {
    rgb24_to_hsv(rgb, &p->color_h, &p->color_s, &p->color_v);
  }
}

bool sp_picker_click(ccord_ui *ui, profile_state *s, float x, float y)
{
  if (!ui || !s || s->picker.kind == PROF_PICK_NONE) return false;
  prof_picker_state *p = &s->picker;
  p->color_drag = 0;
  if (!ui_in_rect(x, y, p->box)) {
    sp_picker_close(ui, s);
    return true;
  }
  for (size_t i = p->nhits; i > 0; i--) {
    const prof_picker_hit *h = &p->hits[i - 1u];
    if (!ui_in_rect(x, y, h->rect)) continue;
    if (h->kind == 1) { sp_picker_close(ui, s); return true; }
    if (h->kind == 2) { picker_apply(ui, s); return true; }
    if (h->kind == 17) {
      p->search_focused = false;
      p->color_drag = 1;
      color_update_sv(p, x, y);
      ui->dirty = true;
      return true;
    }
    if (h->kind == 18) {
      p->search_focused = false;
      p->color_drag = 2;
      color_update_hue(p, x);
      ui->dirty = true;
      return true;
    }
    if (h->kind == 19) {
      p->search_focused = false;
      if (h->arg >= 0 && h->arg < 5) {
        rgb24_to_hsv(k_color_suggestions[h->arg], &p->color_h, &p->color_s, &p->color_v);
        color_sync_hex(p);
      }
      ui->dirty = true;
      return true;
    }
    if (h->kind == 3) {
      p->staged_sku = CCORD_ID_NONE;
      p->staged_id[0] = p->staged_url[0] = p->staged_palette[0] = p->staged_name[0] = '\0';
      p->staged_tag[0] = p->staged_badge[0] = '\0';
      p->staged_has_effect = false;
      memset(&p->staged_effect, 0, sizeof p->staged_effect);
      p->staged_has_frame = false;
      memset(&p->staged_frame, 0, sizeof p->staged_frame);
      ui->dirty = true;
      return true;
    }
    if (h->kind == 4) {
      p->search_focused = true;
      if (ui->touch_mode) ui->touch_text_focused = true;
      ui->dirty = true;
      return true;
    }
    if (h->kind == 5) {
      p->filter_idx = h->arg;
      p->scroll = 0.0f;
      p->search_focused = false;
      ui->dirty = true;
      return true;
    }
    if (h->kind == 7) {
      p->reduced_motion = !p->reduced_motion;
      ui->dirty = true;
      return true;
    }
    if (h->kind == 8) {
      p->staged_zoom = fmaxf(1.0f, p->staged_zoom - 0.15f);
      if (p->staged_zoom <= 1.01f) { p->staged_pan_x = 0.0f; p->staged_pan_y = 0.0f; }
      ui->dirty = true;
      return true;
    }
    if (h->kind == 9) {
      p->staged_zoom = fminf(3.0f, p->staged_zoom + 0.15f);
      ui->dirty = true;
      return true;
    }
    if (h->kind == 10) {
      if (ui->d.session) {
        ccord_error err = {0};
        (void)ccord_session_fetch_collectibles(ui->d.session, true, &err);
      }
      ui->dirty = true;
      return true;
    }
    if (h->kind == 11) {
      if (ui->d.host.open_url) (void)ui->d.host.open_url(ui->d.host.ud, DISCORD_SHOP_URL);
      return true;
    }
    if (h->kind == 12) { p->staged_pan_x = fmaxf(-1.0f, p->staged_pan_x - 0.20f); ui->dirty = true; return true; }
    if (h->kind == 13) { p->staged_pan_x = fminf(1.0f, p->staged_pan_x + 0.20f); ui->dirty = true; return true; }
    if (h->kind == 14) { p->staged_pan_y = fmaxf(-1.0f, p->staged_pan_y - 0.20f); ui->dirty = true; return true; }
    if (h->kind == 15) { p->staged_pan_y = fminf(1.0f, p->staged_pan_y + 0.20f); ui->dirty = true; return true; }
    if (h->kind == 16) { p->staged_zoom = 1.0f; p->staged_pan_x = 0.0f; p->staged_pan_y = 0.0f; ui->dirty = true; return true; }
    if (h->kind == 6) {
      if (!ui_in_rect(x, y, p->list_rect)) continue;
      p->search_focused = false;
      if (h->arg < 0) {
        p->staged_sku = CCORD_ID_NONE;
        p->staged_id[0] = p->staged_url[0] = p->staged_palette[0] = p->staged_name[0] = '\0';
        p->staged_tag[0] = p->staged_badge[0] = '\0';
        p->staged_has_effect = false;
        memset(&p->staged_effect, 0, sizeof p->staged_effect);
        p->staged_has_frame = false;
        memset(&p->staged_frame, 0, sizeof p->staged_frame);
      } else if (p->kind == PROF_PICK_CLAN) {
        clan_option clans[64];
        size_t nc = gather_real_clan_options(ui, clans, 64, nullptr);
        if ((size_t)h->arg < nc) {
          p->staged_sku = clans[h->arg].guild_id;
          snprintf(p->staged_tag, sizeof p->staged_tag, "%s", clans[h->arg].tag);
          snprintf(p->staged_badge, sizeof p->staged_badge, "%.63s", clans[h->arg].badge);
          snprintf(p->staged_name, sizeof p->staged_name, "%s", clans[h->arg].guild_name);
        }
      } else {
        ccord_collectible_kind ckind = (p->kind == PROF_PICK_DECO) ? CCORD_COLLECTIBLE_AVATAR_DECORATION
                                     : (p->kind == PROF_PICK_EFFECT) ? CCORD_COLLECTIBLE_PROFILE_EFFECT
                                     : (p->kind == PROF_PICK_FRAME) ? CCORD_COLLECTIBLE_PROFILE_FRAME
                                                                    : CCORD_COLLECTIBLE_NAMEPLATE;
        ccord_owned_collectible owned[CCORD_COLLECTIBLES_MAX];
        size_t nowned = gather_real_collectibles(ui, s, ckind, owned, CCORD_COLLECTIBLES_MAX);
        if ((size_t)h->arg < nowned) {
          const ccord_owned_collectible *e = &owned[h->arg];
          p->staged_sku = e->sku_id;
          snprintf(p->staged_name, sizeof p->staged_name, "%s", e->name);
          if (p->kind == PROF_PICK_EFFECT) {
            (void)ccord_id_format(e->item_id, p->staged_id);
            snprintf(p->staged_url, sizeof p->staged_url, "%s", e->cdn_url);
            p->staged_has_effect = e->has_effect;
            p->staged_effect = e->effect;
          } else if (p->kind == PROF_PICK_FRAME) {
            snprintf(p->staged_url, sizeof p->staged_url, "%s", e->cdn_url[0] ? e->cdn_url : e->static_url);
            p->staged_has_frame = e->has_frame;
            p->staged_frame = e->frame;
          } else {
            snprintf(p->staged_id, sizeof p->staged_id, "%s", e->asset);
            snprintf(p->staged_url, sizeof p->staged_url, "%s", e->cdn_url);
            snprintf(p->staged_palette, sizeof p->staged_palette, "%s", e->palette);
          }
        }
      }
      ui->dirty = true;
      return true;
    }
  }
  p->search_focused = false;
  if (ui->touch_mode) ui->touch_text_focused = false;
  ui->dirty = true;
  return true;
}

bool sp_picker_mouse_move(ccord_ui *ui, profile_state *s, float x, float y)
{
  if (!ui || !s || s->picker.kind == PROF_PICK_NONE) return false;
  prof_picker_state *p = &s->picker;
  if (p->kind == PROF_PICK_COLOR && p->color_drag != 0) {
    if (p->color_drag == 1) color_update_sv(p, x, y);
    else if (p->color_drag == 2) color_update_hue(p, x);
    ui->dirty = true;
    return true;
  }
  if (ui_in_rect(x, y, p->box)) {
    ui->dirty = true;
    return true;
  }
  return false;
}

void sp_picker_mouse_up(ccord_ui *ui, profile_state *s)
{
  (void)ui;
  if (!s) return;
  s->picker.color_drag = 0;
}

bool sp_picker_wheel(ccord_ui *ui, profile_state *s, float wheel)
{
  if (!ui || !s || s->picker.kind == PROF_PICK_NONE) return false;
  prof_picker_state *p = &s->picker;
  if (p->max_scroll <= 0.0f) return true;
  p->scroll -= wheel * 48.0f;
  if (p->scroll < 0.0f) p->scroll = 0.0f;
  if (p->scroll > p->max_scroll) p->scroll = p->max_scroll;
  ui->dirty = true;
  return true;
}

bool sp_picker_key(ccord_ui *ui, profile_state *s, const ccord_input_event *ev)
{
  if (!ui || !s || s->picker.kind == PROF_PICK_NONE || !ev) return false;
  prof_picker_state *p = &s->picker;
  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_ESCAPE) { sp_picker_close(ui, s); return true; }
    if (ev->key == CCORD_KEY_RETURN && (!p->search_focused || p->kind == PROF_PICK_COLOR)) {
      picker_apply(ui, s);
      return true;
    }
    if (p->search_focused) {
      bool mod = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
      if (ev->key == CCORD_KEY_BACKSPACE) {
        if (mod) ccord_text_edit_backspace_word(&p->search);
        else ccord_text_edit_backspace(&p->search);
        if (p->kind == PROF_PICK_COLOR) color_apply_hex_text(p);
        p->scroll = 0.0f;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_DELETE) {
        ccord_text_edit_delete(&p->search);
        if (p->kind == PROF_PICK_COLOR) color_apply_hex_text(p);
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_LEFT) { ccord_text_edit_move(&p->search, -1, false); ui->dirty = true; return true; }
      if (ev->key == CCORD_KEY_RIGHT) { ccord_text_edit_move(&p->search, 1, false); ui->dirty = true; return true; }
      if (mod && ev->key == CCORD_KEY_A) { ccord_text_edit_select_all(&p->search); ui->dirty = true; return true; }
      if (mod && ev->key == CCORD_KEY_V && ui->d.host.clipboard_get) {
        char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
        if (clip) {
          ccord_error err = {0};
          if (p->kind == PROF_PICK_COLOR) {
            char buf[16];
            size_t o = 0;
            for (size_t i = 0; clip[i] && o < 7u; i++) {
              if (strchr("0123456789abcdefABCDEF#", clip[i])) buf[o++] = clip[i];
            }
            if (o > 0) (void)ccord_text_edit_insert(&p->search, buf, o, &err);
            color_apply_hex_text(p);
          } else {
            (void)ccord_text_edit_insert(&p->search, clip, strlen(clip), &err);
          }
          ccord_free(clip);
          p->scroll = 0.0f;
          ui->dirty = true;
        }
        return true;
      }
    }
    return true;
  }
  if (ev->type == CCORD_IN_TEXT && p->search_focused && ev->text[0]) {
    ccord_error err = {0};
    if (p->kind == PROF_PICK_COLOR) {
      char buf[16];
      size_t o = 0;
      size_t sel = p->search.caret > p->search.anchor ? p->search.caret - p->search.anchor
                                                      : p->search.anchor - p->search.caret;
      size_t used = p->search.len > sel ? p->search.len - sel : 0;
      size_t room = 7u > used ? 7u - used : 0;
      for (size_t i = 0; ev->text[i] && o < sizeof buf - 1u && room > 0; i++) {
        if (strchr("0123456789abcdefABCDEF#", ev->text[i])) {
          buf[o++] = ev->text[i];
          room--;
        }
      }
      if (o > 0) {
        (void)ccord_text_edit_insert(&p->search, buf, o, &err);
        color_apply_hex_text(p);
      }
    } else {
      (void)ccord_text_edit_insert(&p->search, ev->text, strlen(ev->text), &err);
    }
    p->scroll = 0.0f;
    ui->dirty = true;
    return true;
  }
  return true;
}
