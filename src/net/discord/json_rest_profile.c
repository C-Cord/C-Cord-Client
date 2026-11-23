// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "json_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool dj_parse_hex_color(const char *s, uint32_t *out)
{
  if (!s || s[0] != '#' || strlen(s) != 7) return false;
  uint32_t v = 0;
  for (size_t i = 1; i < 7; i++) {
    char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a') + 10u;
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A') + 10u;
    else return false;
    v = (v << 4) | d;
  }
  *out = v;
  return true;
}

// obj[key] is [primary, accent], each an integer 0..0xFFFFFF. Both must be valid, else nothing is written.
static bool theme_pair(const ccord_json *obj, const char *key, uint32_t *primary, uint32_t *accent)
{
  const ccord_json *arr = obj ? ccord_json_get(obj, key) : nullptr;
  if (!arr || ccord_json_array_len(arr) < 2) return false;
  int64_t p = -1, a = -1;
  const ccord_json *pj2 = ccord_json_array_at(arr, 0);
  const ccord_json *aj = ccord_json_array_at(arr, 1);
  if (!pj2 || !aj || !ccord_json_int64(pj2, &p) || !ccord_json_int64(aj, &a)) return false;
  if (p < 0 || a < 0 || p > (int64_t)0xFFFFFF || a > (int64_t)0xFFFFFF) return false;
  *primary = (uint32_t)p;
  *accent = (uint32_t)a;
  return true;
}

// Per-server avatar or banner: guilds/<guild>/users/<user>/<kind>/<hash>, animated hashes (a_...) as .gif.
static const char *guild_member_asset_url(ccord_arena *a, ccord_id gid, ccord_id uid, const char *kind, const char *hash)
{
  if (gid == CCORD_ID_NONE || uid == CCORD_ID_NONE || !dj_cdn_name_ok(hash)) return "";
  char path[96];
  (void)snprintf(path, sizeof path, "guilds/%llu/users/%llu/%s", (unsigned long long)gid,
                 (unsigned long long)uid, kind);
  bool anim = hash[0] == 'a' && hash[1] == '_';
  const char *sz = (kind && strcmp(kind, "banners") == 0) ? "512" : "128";
  char suffix[24];
  snprintf(suffix, sizeof suffix, "%s?size=%s", anim ? ".gif" : ".png", sz);
  return dj_cdn_asset_url(a, path, hash, suffix);
}

// An ISO timestamp key as unix ms; 0 when absent, null or not a timestamp.
static int64_t iso_key_ms(const ccord_json *obj, const char *key)
{
  const char *s = dj_jstr(obj, key);
  if (!s || s[0] == '\0') return 0;
  int64_t t = dj_iso_ms(s, strlen(s));
  return t > 0 ? t : 0;
}

static bool decor_url_ok(const char *s)
{
  if (!s || s[0] == '\0') return false;
  size_t n = strlen(s);
  if (n >= 256u) return false;
  return ccord_live_media_url_allowed(s);
}

static int32_t clamp_i32_field(const ccord_json *obj, const char *key, int32_t fallback, int32_t lo, int32_t hi)
{
  int64_t v = dj_json_i64(obj, key, (int64_t)fallback);
  if (v < (int64_t)lo) v = (int64_t)lo;
  if (v > (int64_t)hi) v = (int64_t)hi;
  return (int32_t)v;
}

static void copy_decor_url(char dst[256], const char *src)
{
  if (!dst) return;
  dst[0] = '\0';
  if (decor_url_ok(src)) dj_copy_trunc_utf8(dst, 256u, src);
}

static bool parse_profile_effect_item(const ccord_json *obj, ccord_id fallback_id, ccord_profile_effect_config *out)
{
  if (!obj || !out || ccord_json_type_of(obj) != CCORD_JSON_OBJECT) return false;
  *out = (ccord_profile_effect_config){0};
  const ccord_json *cfg = ccord_json_get(obj, "config");
  if (!cfg || ccord_json_type_of(cfg) != CCORD_JSON_OBJECT) cfg = obj;

  ccord_id id = CCORD_ID_NONE, sku_id = CCORD_ID_NONE;
  (void)dj_json_id_key(cfg, "id", &id);
  if (id == CCORD_ID_NONE) (void)dj_json_id_key(obj, "id", &id);
  (void)dj_json_id_key(cfg, "sku_id", &sku_id);
  if (sku_id == CCORD_ID_NONE) (void)dj_json_id_key(obj, "sku_id", &sku_id);
  if (id == CCORD_ID_NONE) id = sku_id != CCORD_ID_NONE ? sku_id : fallback_id;
  if (sku_id == CCORD_ID_NONE) sku_id = id;
  if (id == CCORD_ID_NONE) return false;
  out->id = id;
  out->sku_id = sku_id;

  const char *title = dj_jstr(cfg, "title");
  if (!title || title[0] == '\0') title = dj_jstr(obj, "title");
  if (!title || title[0] == '\0') title = dj_jstr(cfg, "name");
  if (!title || title[0] == '\0') title = dj_jstr(obj, "name");
  if (title && title[0] != '\0') dj_copy_trunc_utf8(out->title, sizeof out->title, title);

  const char *desc = dj_jstr(cfg, "description");
  if (!desc || desc[0] == '\0') desc = dj_jstr(obj, "description");
  if (!desc || desc[0] == '\0') desc = dj_jstr(cfg, "accessibilityLabel");
  if (desc && desc[0] != '\0') dj_copy_trunc_utf8(out->description, sizeof out->description, desc);

  const char *thumb = dj_jstr(cfg, "thumbnailPreviewSrc");
  if (!thumb) thumb = dj_jstr(cfg, "thumbnail_preview_src");
  copy_decor_url(out->thumbnail_url, thumb);

  const char *rm = dj_jstr(cfg, "reducedMotionSrc");
  if (!rm) rm = dj_jstr(cfg, "reduced_motion_src");
  copy_decor_url(out->reduced_motion_url, rm);

  const char *sf = dj_jstr(cfg, "staticFrameSrc");
  if (!sf) sf = dj_jstr(cfg, "static_frame_src");
  copy_decor_url(out->static_frame_url, sf);

  const ccord_json *effs = ccord_json_get(cfg, "effects");
  size_t ne = (effs && ccord_json_type_of(effs) == CCORD_JSON_ARRAY) ? ccord_json_array_len(effs) : 0u;
  for (size_t i = 0; i < ne && out->layer_count < CCORD_PROFILE_EFFECT_MAX_LAYERS; i++) {
    const ccord_json *ej = ccord_json_array_at(effs, i);
    if (!ej || ccord_json_type_of(ej) != CCORD_JSON_OBJECT) continue;
    const char *src = dj_jstr(ej, "src");
    if (!decor_url_ok(src)) {
      const ccord_json *rs = ccord_json_get(ej, "randomizedSources");
      if (rs && ccord_json_type_of(rs) == CCORD_JSON_ARRAY && ccord_json_array_len(rs) > 0u) {
        const ccord_json *r0 = ccord_json_array_at(rs, 0u);
        src = (r0 && ccord_json_type_of(r0) == CCORD_JSON_OBJECT) ? dj_jstr(r0, "src") : nullptr;
      }
    }
    if (!decor_url_ok(src)) continue;
    ccord_profile_effect_layer *L = &out->layers[out->layer_count];
    *L = (ccord_profile_effect_layer){0};
    dj_copy_trunc_utf8(L->src, sizeof L->src, src);
    L->loop = dj_json_bool_key(ej, "loop", true);
    L->width = clamp_i32_field(ej, "width", 450, 0, 4096);
    L->height = clamp_i32_field(ej, "height", 880, 0, 4096);
    L->start_ms = clamp_i32_field(ej, "start", 0, 0, 600000);
    L->duration_ms = clamp_i32_field(ej, "duration", 0, 0, 600000);
    L->loop_delay_ms = clamp_i32_field(ej, "loopDelay", 0, 0, 600000);
    if (L->loop_delay_ms == 0) L->loop_delay_ms = clamp_i32_field(ej, "loop_delay", 0, 0, 600000);
    const ccord_json *pos = ccord_json_get(ej, "position");
    if (pos && ccord_json_type_of(pos) == CCORD_JSON_OBJECT) {
      L->pos_x = clamp_i32_field(pos, "x", 0, -2048, 2048);
      L->pos_y = clamp_i32_field(pos, "y", 0, -2048, 2048);
    }
    L->z_index = clamp_i32_field(ej, "zIndex", (int32_t)out->layer_count, -1000, 10000);
    if (ccord_json_get(ej, "zIndex") == nullptr) {
      L->z_index = clamp_i32_field(ej, "z_index", (int32_t)out->layer_count, -1000, 10000);
    }
    out->layer_count++;
  }
  // Stable insertion sort by z_index ascending so lower layers paint first.
  for (size_t i = 1; i < out->layer_count; i++) {
    ccord_profile_effect_layer key = out->layers[i];
    size_t j = i;
    while (j > 0 && out->layers[j - 1].z_index > key.z_index) {
      out->layers[j] = out->layers[j - 1];
      j--;
    }
    out->layers[j] = key;
  }
  return out->layer_count > 0 || out->reduced_motion_url[0] != '\0' || out->static_frame_url[0] != '\0' ||
         out->thumbnail_url[0] != '\0';
}

static void extract_item_assets(const ccord_json *obj, char out_anim[256], char out_static[256])
{
  if (!obj || ccord_json_type_of(obj) != CCORD_JSON_OBJECT) return;
  const ccord_json *assets = ccord_json_get(obj, "assets");
  if (!assets || ccord_json_type_of(assets) != CCORD_JSON_OBJECT) assets = obj;
  if (out_anim && out_anim[0] == '\0') {
    const char *a = dj_jstr(assets, "animated_image_url");
    if (!a) a = dj_jstr(assets, "animatedImageUrl");
    if (!a) a = dj_jstr(assets, "src");
    if (!a) a = dj_jstr(assets, "url");
    copy_decor_url(out_anim, a);
  }
  if (out_static && out_static[0] == '\0') {
    const char *s = dj_jstr(assets, "static_image_url");
    if (!s) s = dj_jstr(assets, "staticImageUrl");
    copy_decor_url(out_static, s);
  }
}

static bool parse_profile_frame_item(const ccord_json *obj, ccord_id fallback_sku, ccord_profile_frame_config *out)
{
  if (!obj || !out || ccord_json_type_of(obj) != CCORD_JSON_OBJECT) return false;
  *out = (ccord_profile_frame_config){0};
  ccord_id sku = CCORD_ID_NONE;
  (void)dj_json_id_key(obj, "sku_id", &sku);
  if (sku == CCORD_ID_NONE) (void)dj_json_id_key(obj, "id", &sku);
  if (sku == CCORD_ID_NONE) sku = fallback_sku;
  if (sku == CCORD_ID_NONE) return false;
  out->sku_id = sku;
  out->inner_width = clamp_i32_field(obj, "inner_width", 0, 0, 2048);
  if (out->inner_width == 0) out->inner_width = clamp_i32_field(obj, "innerWidth", 300, 0, 2048);
  out->overflow_top = clamp_i32_field(obj, "overflow_top", 0, 0, 512);
  if (out->overflow_top == 0) out->overflow_top = clamp_i32_field(obj, "overflowTop", 0, 0, 512);
  out->overflow_bottom = clamp_i32_field(obj, "overflow_bottom", 0, 0, 512);
  if (out->overflow_bottom == 0) out->overflow_bottom = clamp_i32_field(obj, "overflowBottom", 0, 0, 512);
  out->overflow_horizontal = clamp_i32_field(obj, "overflow_horizontal", 0, 0, 512);
  if (out->overflow_horizontal == 0) out->overflow_horizontal = clamp_i32_field(obj, "overflowHorizontal", 0, 0, 512);
  extract_item_assets(obj, out->animated_url, out->static_url);

  const ccord_json *layers = ccord_json_get(obj, "layers");
  size_t nl = (layers && ccord_json_type_of(layers) == CCORD_JSON_ARRAY) ? ccord_json_array_len(layers) : 0u;
  for (size_t i = 0; i < nl && out->layer_count < CCORD_PROFILE_FRAME_MAX_LAYERS; i++) {
    const ccord_json *lj = ccord_json_array_at(layers, i);
    if (!lj || ccord_json_type_of(lj) != CCORD_JSON_OBJECT) continue;
    ccord_profile_frame_layer *L = &out->layers[out->layer_count];
    *L = (ccord_profile_frame_layer){0};
    (void)dj_json_id_key(lj, "id", &L->id);
    L->type = clamp_i32_field(lj, "type", 0, 0, 16);
    L->order = clamp_i32_field(lj, "order", (int32_t)out->layer_count, -100, 1000);
    L->anchor = clamp_i32_field(lj, "anchor", 0, 0, 8);
    L->responsive = dj_json_bool_key(lj, "responsive", false);
    extract_item_assets(lj, L->url, L->static_url);
    if (L->url[0] == '\0' && L->static_url[0] != '\0') {
      memcpy(L->url, L->static_url, sizeof L->url);
    } else if (L->url[0] == '\0' && L->static_url[0] == '\0' && L->id != CCORD_ID_NONE &&
               out->sku_id != CCORD_ID_NONE) {
      // Discord's Profile Frame Layer structure carries only the asset snowflake `id`; CDN URL is derived from sku_id/id.
      (void)snprintf(L->url, sizeof L->url,
                     "https://cdn.discordapp.com/media/v1/collectibles-shop/%llu/%llu/animated",
                     (unsigned long long)out->sku_id, (unsigned long long)L->id);
      (void)snprintf(L->static_url, sizeof L->static_url,
                     "https://cdn.discordapp.com/media/v1/collectibles-shop/%llu/%llu/static",
                     (unsigned long long)out->sku_id, (unsigned long long)L->id);
    }
    if (L->id == CCORD_ID_NONE && L->url[0] == '\0') continue;
    out->layer_count++;
  }
  for (size_t i = 1; i < out->layer_count; i++) {
    ccord_profile_frame_layer key = out->layers[i];
    size_t j = i;
    while (j > 0 && out->layers[j - 1].order > key.order) {
      out->layers[j] = out->layers[j - 1];
      j--;
    }
    out->layers[j] = key;
  }
  return out->layer_count > 0 || out->animated_url[0] != '\0' || out->static_url[0] != '\0';
}

static void extract_profile_effect_ref(const ccord_json *prof_obj, ccord_id *out_id,
                                       ccord_profile_effect_config *out_cfg)
{
  if (!prof_obj || ccord_json_type_of(prof_obj) != CCORD_JSON_OBJECT) return;
  const ccord_json *pe = ccord_json_get(prof_obj, "profile_effect");
  if (!pe || ccord_json_type_of(pe) != CCORD_JSON_OBJECT) {
    ccord_id direct_id = CCORD_ID_NONE;
    if (dj_json_id_key(prof_obj, "profile_effect_id", &direct_id) && direct_id != CCORD_ID_NONE && out_id) {
      *out_id = direct_id;
    }
    return;
  }
  ccord_id eid = CCORD_ID_NONE;
  if (!dj_json_id_key(pe, "id", &eid)) (void)dj_json_id_key(pe, "sku_id", &eid);
  if (eid != CCORD_ID_NONE && out_id) *out_id = eid;
  if (out_cfg) {
    ccord_profile_effect_config tmp;
    if (parse_profile_effect_item(pe, eid, &tmp)) *out_cfg = tmp;
  }
}

static void extract_profile_frame_ref(const ccord_json *prof_obj, const ccord_json *holder_obj,
                                      ccord_id *out_sku, ccord_profile_frame_config *out_cfg)
{
  const ccord_json *pf = prof_obj ? ccord_json_get(prof_obj, "profile_frame") : nullptr;
  if (!pf || ccord_json_type_of(pf) != CCORD_JSON_OBJECT) {
    const ccord_json *col = holder_obj ? ccord_json_get(holder_obj, "collectibles") : nullptr;
    pf = (col && ccord_json_type_of(col) == CCORD_JSON_OBJECT) ? ccord_json_get(col, "profile_frame") : nullptr;
  }
  if (!pf || ccord_json_type_of(pf) != CCORD_JSON_OBJECT) {
    ccord_id direct_sku = CCORD_ID_NONE;
    if (prof_obj && dj_json_id_key(prof_obj, "profile_frame_sku_id", &direct_sku) &&
        direct_sku != CCORD_ID_NONE && out_sku) {
      *out_sku = direct_sku;
    }
    return;
  }
  ccord_id fsku = CCORD_ID_NONE;
  if (!dj_json_id_key(pf, "sku_id", &fsku)) (void)dj_json_id_key(pf, "id", &fsku);
  if (fsku != CCORD_ID_NONE && out_sku) *out_sku = fsku;
  if (out_cfg) {
    ccord_profile_frame_config tmp;
    if (parse_profile_frame_item(pf, fsku, &tmp)) *out_cfg = tmp;
  }
}

static void parse_profile_extras(ccord_arena *a, const ccord_json *root, const ccord_json *uj, const ccord_json *pj,
                                 ccord_id uid, ccord_user_profile *prof)
{
  // user_profile.banner_color takes priority over user.banner_color, then user_profile.theme_colors.
  if ((pj && dj_parse_hex_color(dj_jstr(pj, "banner_color"), &prof->banner_color)) ||
      dj_parse_hex_color(dj_jstr(uj, "banner_color"), &prof->banner_color)) {
    prof->has_banner_color = true;
  }
  if (theme_pair(pj, "theme_colors", &prof->theme_primary, &prof->theme_accent)) prof->has_theme = true;

  const ccord_json *gmp = ccord_json_get(root, "guild_member_profile");
  if (gmp && ccord_json_type_of(gmp) != CCORD_JSON_OBJECT) gmp = nullptr;
  const ccord_json *gm = ccord_json_get(root, "guild_member");
  if (gm && ccord_json_type_of(gm) != CCORD_JSON_OBJECT) gm = nullptr;

  // Per-server profile: store guild theme separately AND also apply to active theme for profile_view.c.
  ccord_id gid = CCORD_ID_NONE;
  if (dj_json_id_key(gmp, "guild_id", &gid) && gid != CCORD_DM_GUILD_ID) prof->guild_id = gid;
  else gid = CCORD_ID_NONE;
  if (theme_pair(gmp, "theme_colors", &prof->guild_theme_primary, &prof->guild_theme_accent)) {
    prof->guild_has_theme = true;
    prof->has_theme = true;
    prof->theme_primary = prof->guild_theme_primary;
    prof->theme_accent = prof->guild_theme_accent;
  }
  if (gmp) {
    if (dj_parse_hex_color(dj_jstr(gmp, "banner_color"), &prof->banner_color)) prof->has_banner_color = true;
    const ccord_json *gac = ccord_json_get(gmp, "accent_color");
    if (gac && ccord_json_type_of(gac) == CCORD_JSON_NUMBER) {
      int64_t ga = dj_json_i64(gmp, "accent_color", 0);
      if (ga >= 0 && ga <= (int64_t)0xFFFFFF) {
        prof->accent_color = (uint32_t)ga;
        prof->has_accent_color = true;
      }
    }
  }
  prof->guild_nick = dj_opt_text_dup(a, dj_jstr(gm, "nick"), CCORD_NAME_MAX_BYTES);
  prof->guild_avatar_url = guild_member_asset_url(a, gid, uid, "avatars", dj_jstr(gm, "avatar"));
  prof->guild_banner_url = guild_member_asset_url(a, gid, uid, "banners", dj_jstr(gmp, "banner"));
  prof->guild_bio = dj_opt_text_dup(a, dj_jstr(gmp, "bio"), CCORD_USER_BIO_MAX_BYTES);
  prof->guild_pronouns = dj_opt_text_dup(a, dj_jstr(gmp, "pronouns"), CCORD_USER_PRONOUNS_MAX_BYTES);
  prof->guild_joined_at_ms = iso_key_ms(gm, "joined_at");
  prof->premium_since_ms = iso_key_ms(gm, "premium_since");

  // Top-level badges: [{id, description, icon}], capped at CCORD_PROFILE_MAX_BADGES.
  const ccord_json *badges_arr = root ? ccord_json_get(root, "badges") : nullptr;
  size_t nb = badges_arr ? ccord_json_array_len(badges_arr) : 0;
  for (size_t i = 0; i < nb && prof->badge_count < CCORD_PROFILE_MAX_BADGES; i++) {
    const ccord_json *bj = ccord_json_array_at(badges_arr, i);
    if (!bj || ccord_json_type_of(bj) != CCORD_JSON_OBJECT) continue;
    ccord_profile_badge *dst = &prof->badges[prof->badge_count];
    dj_copy_trunc_utf8(dst->id, sizeof dst->id, dj_jstr(bj, "id"));
    if (dst->id[0] == '\0') continue;
    dj_copy_trunc_utf8(dst->description, sizeof dst->description, dj_jstr(bj, "description"));
    dj_copy_trunc_utf8(dst->icon_url, sizeof dst->icon_url,
                    dj_cdn_asset_url(a, "badge-icons", dj_jstr(bj, "icon"), ".png"));
    prof->badge_count++;
  }

  // Avatar decorations (user + per-server guild_member override).
  prof->avatar_decoration_url = dj_extract_avatar_deco_url(a, uj);
  prof->avatar_decoration_sku_id = dj_extract_avatar_deco_sku(uj);
  prof->guild_avatar_decoration_url = dj_extract_avatar_deco_url(a, gm);
  prof->guild_avatar_decoration_sku_id = dj_extract_avatar_deco_sku(gm);

  // Nameplates (user + per-server guild_member override).
  dj_extract_nameplate_info(uj, prof->nameplate_asset, prof->nameplate_palette);
  prof->nameplate_sku_id = dj_extract_nameplate_sku(uj);
  dj_extract_nameplate_info(gm, prof->guild_nameplate_asset, prof->guild_nameplate_palette);
  prof->guild_nameplate_sku_id = dj_extract_nameplate_sku(gm);

  // Profile effects & profile frames (user_profile + per-server guild_member_profile overrides).
  extract_profile_effect_ref(pj, &prof->profile_effect_id, &prof->profile_effect);
  extract_profile_effect_ref(gmp, &prof->guild_profile_effect_id, &prof->profile_effect);
  extract_profile_frame_ref(pj, uj, &prof->profile_frame_sku_id, &prof->profile_frame);
  extract_profile_frame_ref(gmp, gm, &prof->guild_profile_frame_sku_id, &prof->profile_frame);
}

ccord_status ccord_live_json_user_profile(const char *json, size_t len, ccord_id user_id,
                                          ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null user profile");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad user profile object");
    goto done;
  }
  const ccord_json *uj = ccord_json_get(root, "user");
  if (!uj || ccord_json_type_of(uj) != CCORD_JSON_OBJECT) uj = root;
  const ccord_json *pj = ccord_json_get(root, "user_profile");
  if (pj && ccord_json_type_of(pj) != CCORD_JSON_OBJECT) pj = nullptr;

  ccord_id uid = user_id;
  (void)dj_json_id_key(uj, "id", &uid);
  if (uid == CCORD_ID_NONE) (void)dj_json_id_key(root, "id", &uid);
  if (uid == CCORD_ID_NONE) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: user profile missing id");
    goto done;
  }

  const char *display_name = dj_jstr(uj, "global_name");
  if (!display_name || display_name[0] == '\0') display_name = dj_jstr(uj, "username");
  display_name = dj_opt_text_dup(&out->arena, display_name, CCORD_NAME_MAX_BYTES);

  const char *handle = dj_opt_text_dup(&out->arena, dj_jstr(uj, "username"), CCORD_NAME_MAX_BYTES);

  const char *av_hash = dj_jstr(uj, "avatar");
  const char *avatar_url = dj_build_cdn_url(&out->arena, "avatars", uid, av_hash);

  const char *ban_hash = pj ? dj_jstr(pj, "banner") : nullptr;
  if (!ban_hash || ban_hash[0] == '\0') ban_hash = dj_jstr(uj, "banner");
  const char *banner_url = dj_build_cdn_url(&out->arena, "banners", uid, ban_hash);

  const ccord_json *pac = pj ? ccord_json_get(pj, "accent_color") : nullptr;
  const ccord_json *uac = ccord_json_get(uj, "accent_color");
  bool has_accent = (pac && ccord_json_type_of(pac) == CCORD_JSON_NUMBER) ||
                    (uac && ccord_json_type_of(uac) == CCORD_JSON_NUMBER);
  int64_t accent = (pac && ccord_json_type_of(pac) == CCORD_JSON_NUMBER) ? dj_json_i64(pj, "accent_color", 0)
                                                                          : dj_json_i64(uj, "accent_color", 0);
  if (accent < 0 || accent > (int64_t)0xFFFFFF) {
    accent = 0;
    has_accent = false;
  }

  const char *bio = pj ? dj_jstr(pj, "bio") : nullptr;
  if (!bio || bio[0] == '\0') bio = dj_jstr(uj, "bio");
  bio = dj_opt_text_dup(&out->arena, bio, CCORD_USER_BIO_MAX_BYTES);

  const char *pronouns = pj ? dj_jstr(pj, "pronouns") : nullptr;
  if (!pronouns || pronouns[0] == '\0') pronouns = dj_jstr(uj, "pronouns");
  pronouns = dj_opt_text_dup(&out->arena, pronouns, CCORD_USER_PRONOUNS_MAX_BYTES);

  ccord_user_profile *prof = ccord_arena_alloc(&out->arena, sizeof *prof, _Alignof(ccord_user_profile));
  ccord_user *u = ccord_arena_alloc(&out->arena, sizeof *u, _Alignof(ccord_user));
  if (!prof || !u) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }

  *prof = (ccord_user_profile){
    .user_id = uid,
    .display_name = display_name,
    .handle = handle,
    .avatar_url = avatar_url,
    .banner_url = banner_url,
    .accent_color = (uint32_t)accent,
    .has_accent_color = has_accent,
    .bio = bio,
    .pronouns = pronouns,
  };
  dj_extract_clan_info(uj, prof->clan_tag, &prof->clan_guild_id, prof->clan_badge);
  parse_profile_extras(&out->arena, root, uj, pj, uid, prof);
  {
    const ccord_json *conn_arr = ccord_json_get(root, "connected_accounts");
    size_t nconn = conn_arr ? ccord_json_array_len(conn_arr) : 0;
    for (size_t i = 0; i < nconn && prof->connection_count < CCORD_PROFILE_MAX_CONNECTIONS; i++) {
      const ccord_json *cj = ccord_json_array_at(conn_arr, i);
      if (!cj || ccord_json_type_of(cj) != CCORD_JSON_OBJECT) continue;
      const char *ctype = dj_jstr(cj, "type");
      const char *cname = dj_jstr(cj, "name");
      if (!ctype || ctype[0] == '\0' || !cname || cname[0] == '\0') continue;
      ccord_profile_connection *dst = &prof->connections[prof->connection_count];
      dj_copy_trunc_utf8(dst->type, sizeof dst->type, ctype);
      dj_copy_trunc_utf8(dst->name, sizeof dst->name, cname);
      if (dst->type[0] == '\0' || dst->name[0] == '\0') continue;
      dst->verified = dj_json_bool_key(cj, "verified", false);
      prof->connection_count++;
    }
    const ccord_json *mg_arr = ccord_json_get(root, "mutual_guilds");
    size_t nmg = mg_arr ? ccord_json_array_len(mg_arr) : 0;
    for (size_t i = 0; i < nmg && prof->mutual_guild_count < CCORD_PROFILE_MAX_MUTUAL_GUILDS; i++) {
      const ccord_json *mgj = ccord_json_array_at(mg_arr, i);
      ccord_id mgid = CCORD_ID_NONE;
      if (!dj_json_id_key(mgj, "id", &mgid) && !dj_json_id(mgj, &mgid)) continue;
      if (mgid == CCORD_ID_NONE || mgid == CCORD_DM_GUILD_ID) continue;
      bool dup = false;
      for (size_t k = 0; k < prof->mutual_guild_count; k++) {
        if (prof->mutual_guild_ids[k] == mgid) { dup = true; break; }
      }
      if (!dup) prof->mutual_guild_ids[prof->mutual_guild_count++] = mgid;
    }
    const ccord_json *mf_arr = ccord_json_get(root, "mutual_friends");
    size_t nmf = mf_arr ? ccord_json_array_len(mf_arr) : 0;
    for (size_t i = 0; i < nmf && prof->mutual_friend_count < CCORD_PROFILE_MAX_MUTUAL_FRIENDS; i++) {
      const ccord_json *mfj = ccord_json_array_at(mf_arr, i);
      ccord_id fuid = CCORD_ID_NONE;
      if (!mfj || !dj_json_id_key(mfj, "id", &fuid) || fuid == CCORD_ID_NONE) continue;
      bool dup = false;
      for (size_t k = 0; k < prof->mutual_friend_count; k++) {
        if (prof->mutual_friends[k].user_id == fuid) { dup = true; break; }
      }
      if (dup) continue;
      const char *fname = dj_jstr(mfj, "global_name");
      if (!fname || fname[0] == '\0') fname = dj_jstr(mfj, "username");
      if (!fname || fname[0] == '\0') fname = "User";
      ccord_mutual_friend *mf = &prof->mutual_friends[prof->mutual_friend_count];
      mf->user_id = fuid;
      dj_copy_trunc_utf8(mf->name, sizeof mf->name, fname);
      if (mf->name[0] == '\0') dj_copy_trunc_utf8(mf->name, sizeof mf->name, "User");
      dj_copy_trunc_utf8(mf->handle, sizeof mf->handle, dj_jstr(mfj, "username"));
      const char *fav = dj_build_cdn_url(&scratch, "avatars", fuid, dj_jstr(mfj, "avatar"));
      dj_copy_trunc_utf8(mf->avatar_url, sizeof mf->avatar_url, fav);
      prof->mutual_friend_count++;
    }
    int64_t mfc = dj_json_i64(root, "mutual_friends_count", -1);
    if (mfc < 0) mfc = dj_json_i64(root, "mutual_friend_count", -1);
    if (mfc < 0) mfc = (int64_t)prof->mutual_friend_count;
    if (mfc > (int64_t)UINT32_MAX) mfc = (int64_t)UINT32_MAX;
    prof->mutual_friend_total = (uint32_t)mfc;
    if (prof->mutual_friend_total < prof->mutual_friend_count) {
      prof->mutual_friend_total = (uint32_t)prof->mutual_friend_count;
    }
  }

  const ccord_json *botj = ccord_json_get(uj, "bot");
  *u = (ccord_user){
    .id = uid,
    .name = (display_name && display_name[0] != '\0') ? display_name : "User",
    .handle = handle,
    .status_text = "",
    .avatar_url = avatar_url,
    .avatar_decoration_url = prof->avatar_decoration_url,
    .avatar_decoration_sku_id = prof->avatar_decoration_sku_id,
    .banner_url = banner_url,
    .accent_color = (uint32_t)accent,
    .bio = bio,
    .pronouns = pronouns,
    .presence = CCORD_PRESENCE_OFFLINE,
    .is_bot = botj && ccord_json_bool(botj, false),
    .avatar_palette = (uint8_t)(uid % 6u),
    .clan_guild_id = prof->clan_guild_id,
    .nameplate_sku_id = prof->nameplate_sku_id,
  };
  memcpy(u->clan_tag, prof->clan_tag, sizeof u->clan_tag);
  memcpy(u->clan_badge, prof->clan_badge, sizeof u->clan_badge);
  memcpy(u->nameplate_asset, prof->nameplate_asset, sizeof u->nameplate_asset);
  memcpy(u->nameplate_palette, prof->nameplate_palette, sizeof u->nameplate_palette);

  out->users = u;
  out->user_count = 1;
  out->user_profile = prof;
  out->ev.type = CCORD_NET_USER_PROFILE;
  out->ev.user_id = uid;
  out->ev.user_profile = prof;
  out->ev.users = u;
  out->ev.user_count = 1;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_profile_effects(const char *json, size_t len,
                                             ccord_profile_effect_config **out_items,
                                             size_t *out_count, ccord_error *err)
{
  ccord_error_clear(err);
  if (out_items) *out_items = nullptr;
  if (out_count) *out_count = 0;
  if (!json || !out_items || !out_count) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null profile effects args");
  }
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad profile effects json");
    goto done;
  }
  const ccord_json *arr = root;
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    arr = ccord_json_get(root, "profile_effect_configs");
  }
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: profile effects missing array");
    goto done;
  }
  size_t n = ccord_json_array_len(arr);
  if (n > 2048u) n = 2048u;
  if (n == 0u) goto done;
  ccord_profile_effect_config *items = ccord_calloc(n, sizeof *items);
  if (!items) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *ej = ccord_json_array_at(arr, i);
    if (parse_profile_effect_item(ej, CCORD_ID_NONE, &items[kept])) kept++;
  }
  if (kept == 0u) {
    ccord_free(items);
  } else {
    *out_items = items;
    *out_count = kept;
  }
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_collectible_product(const char *json, size_t len, ccord_id sku_id,
                                                 ccord_profile_effect_config *out_effect, bool *has_effect,
                                                 ccord_profile_frame_config *out_frame, bool *has_frame,
                                                 ccord_error *err)
{
  ccord_error_clear(err);
  if (has_effect) *has_effect = false;
  if (has_frame) *has_frame = false;
  if (out_effect) *out_effect = (ccord_profile_effect_config){0};
  if (out_frame) *out_frame = (ccord_profile_frame_config){0};
  if (!json) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null collectible product");

  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad collectible product object");
    goto done;
  }
  ccord_id root_sku = sku_id;
  if (root_sku == CCORD_ID_NONE) (void)dj_json_id_key(root, "sku_id", &root_sku);
  char top_anim[256] = "", top_static[256] = "";
  extract_item_assets(root, top_anim, top_static);

  const ccord_json *items = ccord_json_get(root, "items");
  if (items && ccord_json_type_of(items) == CCORD_JSON_ARRAY) {
    size_t ni = ccord_json_array_len(items);
    for (size_t i = 0; i < ni; i++) {
      const ccord_json *it = ccord_json_array_at(items, i);
      if (!it || ccord_json_type_of(it) != CCORD_JSON_OBJECT) continue;
      int64_t itype = dj_json_i64(it, "type", -1);
      if ((itype == 1 || ccord_json_get(it, "effects") != nullptr) && out_effect && has_effect && !*has_effect) {
        if (parse_profile_effect_item(it, root_sku, out_effect)) *has_effect = true;
      } else if ((itype == 3 || ccord_json_get(it, "layers") != nullptr) && out_frame && has_frame && !*has_frame) {
        if (parse_profile_frame_item(it, root_sku, out_frame)) {
          if (out_frame->animated_url[0] == '\0' && top_anim[0] != '\0') {
            memcpy(out_frame->animated_url, top_anim, sizeof out_frame->animated_url);
          }
          if (out_frame->static_url[0] == '\0' && top_static[0] != '\0') {
            memcpy(out_frame->static_url, top_static, sizeof out_frame->static_url);
          }
          *has_frame = true;
        }
      }
    }
  } else {
    int64_t rtype = dj_json_i64(root, "type", -1);
    if ((rtype == 1 || ccord_json_get(root, "effects") != nullptr) && out_effect && has_effect) {
      if (parse_profile_effect_item(root, root_sku, out_effect)) *has_effect = true;
    } else if ((rtype == 3 || ccord_json_get(root, "layers") != nullptr) && out_frame && has_frame) {
      if (parse_profile_frame_item(root, root_sku, out_frame)) *has_frame = true;
    }
  }
done:
  ccord_arena_free(&scratch);
  return st;
}

typedef struct cp_list {
  ccord_owned_collectible *items;
  size_t count;
  size_t cap;
} cp_list;

static void cp_push(cp_list *L, const ccord_owned_collectible *cand)
{
  if (!L || !cand || L->count >= CCORD_COLLECTIBLES_MAX) return;
  if (cand->sku_id == CCORD_ID_NONE && cand->item_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < L->count; i++) {
    ccord_owned_collectible *e = &L->items[i];
    if (e->kind != cand->kind) continue;
    bool same = (cand->sku_id != CCORD_ID_NONE && e->sku_id == cand->sku_id) ||
                (cand->kind == CCORD_COLLECTIBLE_PROFILE_EFFECT && cand->item_id != CCORD_ID_NONE && e->item_id == cand->item_id);
    if (!same) continue;
    if (e->name[0] == '\0' && cand->name[0] != '\0') memcpy(e->name, cand->name, sizeof e->name);
    if (e->category[0] == '\0' && cand->category[0] != '\0') memcpy(e->category, cand->category, sizeof e->category);
    if (e->asset[0] == '\0' && cand->asset[0] != '\0') memcpy(e->asset, cand->asset, sizeof e->asset);
    if (e->palette[0] == '\0' && cand->palette[0] != '\0') memcpy(e->palette, cand->palette, sizeof e->palette);
    if (e->cdn_url[0] == '\0' && cand->cdn_url[0] != '\0') memcpy(e->cdn_url, cand->cdn_url, sizeof e->cdn_url);
    if (e->static_url[0] == '\0' && cand->static_url[0] != '\0') memcpy(e->static_url, cand->static_url, sizeof e->static_url);
    if (!e->has_effect && cand->has_effect) {
      e->has_effect = true;
      e->effect = cand->effect;
    }
    if (!e->has_frame && cand->has_frame) {
      e->has_frame = true;
      e->frame = cand->frame;
    }
    return;
  }
  if (L->count == L->cap) {
    size_t ncap = L->cap ? L->cap * 2u : 16u;
    if (ncap > CCORD_COLLECTIBLES_MAX) ncap = CCORD_COLLECTIBLES_MAX;
    ccord_owned_collectible *na = ccord_realloc_array(L->items, ncap, sizeof *na);
    if (!na) return;
    L->items = na;
    L->cap = ncap;
  }
  L->items[L->count++] = *cand;
}

static void cp_titlecase_copy(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  dst[0] = '\0';
  if (!src || !src[0]) return;
  dj_copy_trunc_utf8(dst, cap, src);
  for (size_t i = 0; dst[i] != '\0'; i++) {
    if (dst[i] == '_') dst[i] = ' ';
    if ((i == 0 || dst[i - 1] == ' ') && dst[i] >= 'a' && dst[i] <= 'z') {
      dst[i] = (char)(dst[i] - 'a' + 'A');
    }
  }
}

static void cp_parse_item(cp_list *L, const ccord_json *it, int64_t type_hint, ccord_id parent_sku,
                          const char *parent_name, const char *parent_cat, int64_t expires_ms)
{
  if (!it || ccord_json_type_of(it) != CCORD_JSON_OBJECT) return;
  int64_t itype = dj_json_i64(it, "type", type_hint);
  if (itype < 0) {
    if (ccord_json_get(it, "layers") != nullptr || ccord_json_get(it, "inner_width") != nullptr ||
        ccord_json_get(it, "innerWidth") != nullptr) itype = 3;
    else if (ccord_json_get(it, "palette") != nullptr) itype = 2;
    else if (ccord_json_get(it, "effects") != nullptr || ccord_json_get(it, "thumbnailPreviewSrc") != nullptr ||
             ccord_json_get(it, "config") != nullptr) itype = 1;
    else if (ccord_json_get(it, "asset") != nullptr) itype = 0;
  }
  ccord_id isku = CCORD_ID_NONE;
  (void)dj_json_id_key(it, "sku_id", &isku);
  if (isku == CCORD_ID_NONE) isku = parent_sku;

  int64_t exp = dj_json_ts_ms(it, "expires_at");
  if (exp == 0) exp = iso_key_ms(it, "expires_at");
  if (exp == 0) exp = expires_ms;

  const char *iname = dj_jstr(it, "title");
  if (!iname || !iname[0]) iname = dj_jstr(it, "name");
  if (!iname || !iname[0]) iname = dj_jstr(it, "label");
  if (!iname || !iname[0]) iname = parent_name;

  if (itype == 0) {
    const char *asset = dj_jstr(it, "asset");
    if (!dj_cdn_name_ok(asset) || isku == CCORD_ID_NONE) return;
    ccord_owned_collectible c = {
      .kind = CCORD_COLLECTIBLE_AVATAR_DECORATION,
      .sku_id = isku,
      .item_id = isku,
      .expires_at_ms = exp,
    };
    dj_copy_trunc_utf8(c.name, sizeof c.name, (iname && iname[0]) ? iname : "Avatar Decoration");
    if (parent_cat && parent_cat[0]) dj_copy_trunc_utf8(c.category, sizeof c.category, parent_cat);
    dj_copy_trunc_utf8(c.asset, sizeof c.asset, asset);
    (void)snprintf(c.cdn_url, sizeof c.cdn_url,
                   "https://cdn.discordapp.com/avatar-decoration-presets/%s.png?size=128&passthrough=true", asset);
    (void)snprintf(c.static_url, sizeof c.static_url,
                   "https://cdn.discordapp.com/avatar-decoration-presets/%s.png?size=128&passthrough=false", asset);
    cp_push(L, &c);
  } else if (itype == 1) {
    ccord_id eff_id = CCORD_ID_NONE;
    (void)dj_json_id_key(it, "id", &eff_id);
    const ccord_json *cfg = ccord_json_get(it, "config");
    if (eff_id == CCORD_ID_NONE && cfg && ccord_json_type_of(cfg) == CCORD_JSON_OBJECT) {
      (void)dj_json_id_key(cfg, "id", &eff_id);
    }
    if (eff_id == CCORD_ID_NONE) eff_id = isku;
    if (isku == CCORD_ID_NONE) isku = eff_id;
    if (eff_id == CCORD_ID_NONE) return;
    ccord_owned_collectible c = {
      .kind = CCORD_COLLECTIBLE_PROFILE_EFFECT,
      .sku_id = isku,
      .item_id = eff_id,
      .expires_at_ms = exp,
    };
    ccord_profile_effect_config pe = {0};
    if (parse_profile_effect_item(it, eff_id, &pe)) {
      c.has_effect = true;
      c.effect = pe;
      if (pe.id != CCORD_ID_NONE) c.item_id = pe.id;
      if (pe.title[0] != '\0') iname = pe.title;
      const char *thumb = pe.thumbnail_url[0] ? pe.thumbnail_url
                                              : (pe.static_frame_url[0] ? pe.static_frame_url : pe.reduced_motion_url);
      const char *sturl = pe.reduced_motion_url[0] ? pe.reduced_motion_url
                                                   : (pe.static_frame_url[0] ? pe.static_frame_url : pe.thumbnail_url);
      dj_copy_trunc_utf8(c.cdn_url, sizeof c.cdn_url, thumb);
      dj_copy_trunc_utf8(c.static_url, sizeof c.static_url, sturl);
    }
    dj_copy_trunc_utf8(c.name, sizeof c.name, (iname && iname[0]) ? iname : "Profile Effect");
    if (parent_cat && parent_cat[0]) dj_copy_trunc_utf8(c.category, sizeof c.category, parent_cat);
    cp_push(L, &c);
  } else if (itype == 2) {
    const char *asset = dj_jstr(it, "asset");
    if (!dj_nameplate_asset_ok(asset) || isku == CCORD_ID_NONE) return;
    const char *pal = dj_jstr(it, "palette");
    if (!dj_cdn_name_ok(pal)) pal = "cobalt";
    ccord_owned_collectible c = {
      .kind = CCORD_COLLECTIBLE_NAMEPLATE,
      .sku_id = isku,
      .item_id = isku,
      .expires_at_ms = exp,
    };
    dj_copy_trunc_utf8(c.name, sizeof c.name, (iname && iname[0]) ? iname : "Nameplate");
    dj_copy_trunc_utf8(c.asset, sizeof c.asset, asset);
    dj_copy_trunc_utf8(c.palette, sizeof c.palette, pal);
    if (parent_cat && parent_cat[0]) dj_copy_trunc_utf8(c.category, sizeof c.category, parent_cat);
    else cp_titlecase_copy(c.category, sizeof c.category, pal);
    cp_push(L, &c);
  } else if (itype == 3) {
    ccord_profile_frame_config pf = {0};
    bool parsed = parse_profile_frame_item(it, isku, &pf);
    ccord_id fsku = parsed ? pf.sku_id : isku;
    if (fsku == CCORD_ID_NONE) (void)dj_json_id_key(it, "id", &fsku);
    if (fsku == CCORD_ID_NONE) return;
    ccord_owned_collectible c = {
      .kind = CCORD_COLLECTIBLE_PROFILE_FRAME,
      .sku_id = fsku,
      .item_id = fsku,
      .expires_at_ms = exp,
      .has_frame = parsed,
    };
    if (parsed) {
      c.frame = pf;
      const char *anim = pf.animated_url[0]
                           ? pf.animated_url
                           : (pf.layer_count > 0 && pf.layers[0].url[0] ? pf.layers[0].url : pf.static_url);
      const char *stat = pf.static_url[0]
                           ? pf.static_url
                           : (pf.layer_count > 0 && pf.layers[0].static_url[0] ? pf.layers[0].static_url : anim);
      dj_copy_trunc_utf8(c.cdn_url, sizeof c.cdn_url, anim);
      dj_copy_trunc_utf8(c.static_url, sizeof c.static_url, stat);
    } else {
      extract_item_assets(it, c.cdn_url, c.static_url);
    }
    dj_copy_trunc_utf8(c.name, sizeof c.name, (iname && iname[0]) ? iname : "Profile Frame");
    if (parent_cat && parent_cat[0]) dj_copy_trunc_utf8(c.category, sizeof c.category, parent_cat);
    cp_push(L, &c);
  }
}

static void cp_parse_product(cp_list *L, const ccord_json *prod, int depth)
{
  if (!prod || ccord_json_type_of(prod) != CCORD_JSON_OBJECT || depth > 4) return;
  ccord_id psku = CCORD_ID_NONE;
  (void)dj_json_id_key(prod, "sku_id", &psku);
  if (psku == CCORD_ID_NONE) (void)dj_json_id_key(prod, "id", &psku);
  int64_t ptype = dj_json_i64(prod, "type", -1);
  const char *pname = dj_jstr(prod, "name");
  if (!pname || !pname[0]) pname = dj_jstr(prod, "title");
  const char *pcat = dj_jstr(prod, "category");
  if (!pcat || !pcat[0]) pcat = dj_jstr(prod, "category_title");
  int64_t exp = dj_json_ts_ms(prod, "expires_at");
  if (exp == 0) exp = iso_key_ms(prod, "expires_at");

  bool parsed_children = false;
  const ccord_json *items = ccord_json_get(prod, "items");
  if (items && ccord_json_type_of(items) == CCORD_JSON_ARRAY && ccord_json_array_len(items) > 0u) {
    size_t ni = ccord_json_array_len(items);
    for (size_t i = 0; i < ni; i++) {
      cp_parse_item(L, ccord_json_array_at(items, i), (ptype >= 0 && ptype <= 3) ? ptype : -1,
                    psku, pname, pcat, exp);
    }
    parsed_children = true;
  }
  const ccord_json *bundled = ccord_json_get(prod, "bundled_products");
  if (bundled && ccord_json_type_of(bundled) == CCORD_JSON_ARRAY) {
    size_t nb = ccord_json_array_len(bundled);
    for (size_t i = 0; i < nb; i++) cp_parse_product(L, ccord_json_array_at(bundled, i), depth + 1);
    parsed_children = true;
  }
  const ccord_json *variants = ccord_json_get(prod, "variants");
  if (variants && ccord_json_type_of(variants) == CCORD_JSON_ARRAY) {
    size_t nv = ccord_json_array_len(variants);
    for (size_t i = 0; i < nv; i++) cp_parse_product(L, ccord_json_array_at(variants, i), depth + 1);
    parsed_children = true;
  }
  const ccord_json *ad = ccord_json_get(prod, "avatar_decoration");
  if (ad && ccord_json_type_of(ad) == CCORD_JSON_OBJECT) {
    cp_parse_item(L, ad, 0, psku, pname, pcat, exp);
    parsed_children = true;
  }
  const ccord_json *pe = ccord_json_get(prod, "profile_effect");
  if (pe && ccord_json_type_of(pe) == CCORD_JSON_OBJECT) {
    cp_parse_item(L, pe, 1, psku, pname, pcat, exp);
    parsed_children = true;
  }
  const ccord_json *np = ccord_json_get(prod, "nameplate");
  if (np && ccord_json_type_of(np) == CCORD_JSON_OBJECT) {
    cp_parse_item(L, np, 2, psku, pname, pcat, exp);
    parsed_children = true;
  }
  const ccord_json *pf = ccord_json_get(prod, "profile_frame");
  if (pf && ccord_json_type_of(pf) == CCORD_JSON_OBJECT) {
    cp_parse_item(L, pf, 3, psku, pname, pcat, exp);
    parsed_children = true;
  }
  if (!parsed_children) {
    cp_parse_item(L, prod, ptype, psku, pname, pcat, exp);
  }
  // Inherit parent product-level frame assets when a child frame item omitted them.
  char top_anim[256] = "", top_static[256] = "";
  extract_item_assets(prod, top_anim, top_static);
  if (top_anim[0] != '\0' || top_static[0] != '\0') {
    for (size_t i = 0; i < L->count; i++) {
      ccord_owned_collectible *e = &L->items[i];
      if (e->kind != CCORD_COLLECTIBLE_PROFILE_FRAME) continue;
      if (psku != CCORD_ID_NONE && e->sku_id != psku) continue;
      if (e->cdn_url[0] == '\0' && top_anim[0] != '\0') dj_copy_trunc_utf8(e->cdn_url, sizeof e->cdn_url, top_anim);
      if (e->static_url[0] == '\0' && top_static[0] != '\0') {
        dj_copy_trunc_utf8(e->static_url, sizeof e->static_url, top_static);
      }
      if (e->has_frame) {
        if (e->frame.animated_url[0] == '\0' && top_anim[0] != '\0') {
          dj_copy_trunc_utf8(e->frame.animated_url, sizeof e->frame.animated_url, top_anim);
        }
        if (e->frame.static_url[0] == '\0' && top_static[0] != '\0') {
          dj_copy_trunc_utf8(e->frame.static_url, sizeof e->frame.static_url, top_static);
        }
      }
    }
  }
}

ccord_status ccord_live_json_collectibles_purchases(const char *json, size_t len,
                                                    ccord_owned_collectible **out_items,
                                                    size_t *out_count, ccord_error *err)
{
  ccord_error_clear(err);
  if (out_items) *out_items = nullptr;
  if (out_count) *out_count = 0;
  if (!json || !out_items || !out_count) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null collectibles purchases args");
  }
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad collectibles purchases json");
    goto done;
  }
  cp_list L = {0};
  if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(root);
    for (size_t i = 0; i < n; i++) cp_parse_product(&L, ccord_json_array_at(root, i), 0);
  } else if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    const ccord_json *arr = ccord_json_get(root, "purchases");
    if (!arr) arr = ccord_json_get(root, "collectibles-purchases");
    if (!arr) arr = ccord_json_get(root, "products");
    if (!arr) arr = ccord_json_get(root, "items");
    if (arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY) {
      size_t n = ccord_json_array_len(arr);
      for (size_t i = 0; i < n; i++) cp_parse_product(&L, ccord_json_array_at(arr, i), 0);
    }
    const ccord_json *cats = ccord_json_get(root, "categories");
    if (cats && ccord_json_type_of(cats) == CCORD_JSON_ARRAY) {
      size_t nc = ccord_json_array_len(cats);
      for (size_t ci = 0; ci < nc; ci++) {
        const ccord_json *cat = ccord_json_array_at(cats, ci);
        if (!cat || ccord_json_type_of(cat) != CCORD_JSON_OBJECT) continue;
        const ccord_json *prods = ccord_json_get(cat, "products");
        if (!prods || ccord_json_type_of(prods) != CCORD_JSON_ARRAY) continue;
        size_t np = ccord_json_array_len(prods);
        for (size_t pi = 0; pi < np; pi++) cp_parse_product(&L, ccord_json_array_at(prods, pi), 0);
      }
    }
  } else {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: unexpected collectibles purchases root");
    goto done;
  }
  *out_items = L.items;
  *out_count = L.count;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_upload_slots(const char *json, size_t len, size_t expected_count,
                                          char out_upload_urls[][2048],
                                          char out_upload_filenames[][256], ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out_upload_urls || !out_upload_filenames || expected_count == 0 ||
      expected_count > CCORD_RICH_MAX_ATTACHMENTS) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad upload slot args");
  }
  for (size_t i = 0; i < expected_count; i++) {
    out_upload_urls[i][0] = '\0';
    out_upload_filenames[i][0] = '\0';
  }
  ccord_arena arena;
  ccord_arena_init(&arena, dj_scratch_chunk_size(len));
  const ccord_json *root = dj_parse_json(&arena, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return ccord_error_set(err, CCORD_ERR_PARSE, "rest: upload slot not an object");
  }
  const ccord_json *atts = ccord_json_get(root, "attachments");
  if (!atts || !dj_is_json_array(atts) || ccord_json_array_len(atts) < expected_count) {
    ccord_arena_free(&arena);
    return ccord_error_set(err, CCORD_ERR_PARSE, "rest: upload slot missing attachments");
  }
  size_t n = ccord_json_array_len(atts);
  for (size_t i = 0; i < n; i++) {
    const ccord_json *slot = ccord_json_array_at(atts, i);
    if (!dj_is_json_object(slot)) continue;
    size_t idx = i;
    const ccord_json *idj = ccord_json_get(slot, "id");
    if (idj) {
      int64_t iv = -1;
      if (ccord_json_type_of(idj) == CCORD_JSON_NUMBER && ccord_json_int64(idj, &iv) && iv >= 0) {
        idx = (size_t)iv;
      } else if (ccord_json_type_of(idj) == CCORD_JSON_STRING) {
        size_t sl = 0;
        const char *s = ccord_json_string(idj, &sl);
        ccord_id parsed = 0;
        if (s && sl == 1 && s[0] == '0') idx = 0;
        else if (s && ccord_id_parse(s, sl, &parsed)) idx = (size_t)parsed;
      }
    }
    if (idx >= expected_count) continue;
    const char *uurl = dj_jstr(slot, "upload_url");
    const char *ufname = dj_jstr(slot, "upload_filename");
    if (!uurl || uurl[0] == '\0' || !ufname || ufname[0] == '\0') {
      ccord_arena_free(&arena);
      return ccord_error_set(err, CCORD_ERR_PARSE, "rest: upload slot missing url or filename");
    }
    size_t ulen = strlen(uurl), flen = strlen(ufname);
    if (ulen >= 2048u || flen >= 256u) {
      ccord_arena_free(&arena);
      return ccord_error_set(err, CCORD_ERR_RANGE, "rest: upload slot string too long");
    }
    memcpy(out_upload_urls[idx], uurl, ulen + 1);
    memcpy(out_upload_filenames[idx], ufname, flen + 1);
  }
  for (size_t i = 0; i < expected_count; i++) {
    if (out_upload_urls[i][0] == '\0' || out_upload_filenames[i][0] == '\0') {
      ccord_arena_free(&arena);
      return ccord_error_set(err, CCORD_ERR_PARSE, "rest: incomplete upload slots");
    }
  }
  ccord_arena_free(&arena);
  return CCORD_OK;
}

ccord_status ccord_live_json_upload_slot(const char *json, size_t len, char *out_upload_url,
                                         size_t url_cap, char *out_upload_filename,
                                         size_t filename_cap, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out_upload_url || url_cap == 0 || !out_upload_filename || filename_cap == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null upload slot args");
  }
  char urls[1][2048];
  char names[1][256];
  ccord_status st = ccord_live_json_upload_slots(json, len, 1, urls, names, err);
  if (st != CCORD_OK) return st;
  size_t ulen = strlen(urls[0]), flen = strlen(names[0]);
  if (ulen >= url_cap || flen >= filename_cap) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "rest: upload slot string too long");
  }
  memcpy(out_upload_url, urls[0], ulen + 1);
  memcpy(out_upload_filename, names[0], flen + 1);
  return CCORD_OK;
}

// Discord nests options at most SUB_COMMAND_GROUP -> SUB_COMMAND -> leaf. Flatten the tree depth-first into
// cmd->options; parent_plus_one is the 1-based index of the owning container option (0 for root options).
// Malformed nesting (a group inside a group, a leaf inside a group, ...) is skipped, which also bounds the
// recursion depth regardless of the payload.
static void slash_flatten_options(ccord_slash_command *cmd, const ccord_json *opts, uint8_t parent_plus_one)
{
  size_t no = dj_is_json_array(opts) ? ccord_json_array_len(opts) : 0;
  for (size_t k = 0; k < no && cmd->option_count < CCORD_SLASH_MAX_OPTIONS; k++) {
    const ccord_json *oj = ccord_json_array_at(opts, k);
    if (!dj_is_json_object(oj)) continue;
    int64_t okind = dj_json_i64(oj, "type", 0);
    if (okind < CCORD_SLASH_OPT_SUB_COMMAND || okind > CCORD_SLASH_OPT_NUMBER) continue;
    ccord_slash_option opt = {0};
    opt.kind = (ccord_slash_opt_kind)okind;
    dj_copy_json_str(opt.name, sizeof opt.name, oj, "name");
    dj_copy_json_str(opt.description, sizeof opt.description, oj, "description");
    if (opt.name[0] == '\0') continue;
    const ccord_slash_option *parent = parent_plus_one ? &cmd->options[parent_plus_one - 1u] : nullptr;
    if (opt.kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP) {
      if (parent) continue;   // groups only live at the root
    } else if (opt.kind == CCORD_SLASH_OPT_SUB_COMMAND) {
      if (parent && parent->kind != CCORD_SLASH_OPT_SUB_COMMAND_GROUP) continue;
    } else {
      if (parent && parent->kind != CCORD_SLASH_OPT_SUB_COMMAND) continue;   // leaves live at the root or in a subcommand
      opt.required = dj_json_bool_key(oj, "required", false);
      opt.autocomplete = dj_json_bool_key(oj, "autocomplete", false) &&
                         (opt.kind == CCORD_SLASH_OPT_STRING || opt.kind == CCORD_SLASH_OPT_INTEGER ||
                          opt.kind == CCORD_SLASH_OPT_NUMBER);
    }
    opt.parent_plus_one = parent_plus_one;
    cmd->options[cmd->option_count++] = opt;
    if (opt.kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP || opt.kind == CCORD_SLASH_OPT_SUB_COMMAND) {
      slash_flatten_options(cmd, ccord_json_get(oj, "options"), (uint8_t)cmd->option_count);
    }
  }
}

ccord_status ccord_live_json_slash_commands(const char *json, size_t len, ccord_id guild_id,
                                            ccord_id channel_id, ccord_live_event *out,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null slash commands");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    st = CCORD_ERR_PARSE;
    goto done;
  }
  const ccord_json *arr = nullptr;
  if (dj_is_json_array(root)) {
    arr = root;
  } else if (dj_is_json_object(root)) {
    arr = ccord_json_get(root, "application_commands");
    if (!arr) arr = ccord_json_get(root, "commands");
  }
  if (!dj_is_json_array(arr)) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: slash commands not an array");
    goto done;
  }
  size_t raw_n = ccord_json_array_len(arr);
  size_t cap = raw_n < CCORD_SLASH_MAX_COMMANDS ? raw_n : CCORD_SLASH_MAX_COMMANDS;
  ccord_slash_command *cmds = nullptr;
  if (cap > 0) {
    cmds = ccord_arena_alloc(&out->arena, cap * sizeof *cmds, _Alignof(ccord_slash_command));
    if (!cmds) {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    memset(cmds, 0, cap * sizeof *cmds);
  }
  size_t out_n = 0;
  for (size_t i = 0; i < raw_n && out_n < CCORD_SLASH_MAX_COMMANDS; i++) {
    const ccord_json *cj = ccord_json_array_at(arr, i);
    if (!dj_is_json_object(cj)) continue;
    int64_t ctype = dj_json_i64(cj, "type", 1);
    if (ctype < 1 || ctype > 3) continue; // CHAT_INPUT slash + USER / MESSAGE context-menu commands
    ccord_slash_command cmd = {0};
    if (!dj_json_id_key(cj, "id", &cmd.id) || !dj_json_id_key(cj, "application_id", &cmd.application_id)) continue;
    cmd.command_type = (uint8_t)ctype;
    (void)dj_json_id_key(cj, "guild_id", &cmd.guild_id);
    if (!dj_json_id_key(cj, "version", &cmd.version) || cmd.version == CCORD_ID_NONE) cmd.version = cmd.id;
    dj_copy_json_str(cmd.name, sizeof cmd.name, cj, "name");
    dj_copy_json_str(cmd.description, sizeof cmd.description, cj, "description");
    cmd.default_member_permissions = UINT64_MAX;
    const ccord_json *dpj = ccord_json_get(cj, "default_member_permissions");
    if (dpj && ccord_json_type_of(dpj) == CCORD_JSON_STRING) {
      size_t plen = 0;
      const char *ps = ccord_json_string(dpj, &plen);
      if (ps && plen > 0 && plen <= 20) {
        uint64_t mask = 0;
        bool ok = true;
        for (size_t k = 0; k < plen; k++) {
          if (ps[k] < '0' || ps[k] > '9') { ok = false; break; }
          uint64_t next = mask * 10u + (uint64_t)(ps[k] - '0');
          if (next < mask) { ok = false; break; }
          mask = next;
        }
        if (ok) cmd.default_member_permissions = mask;
      }
    }
    const ccord_json *dmj = ccord_json_get(cj, "dm_permission");
    cmd.dm_permission = dmj && ccord_json_type_of(dmj) == CCORD_JSON_BOOL ? ccord_json_bool(dmj, true) : true;
    // Context-menu commands carry no options; CHAT_INPUT options arrive as a group/subcommand/leaf tree.
    if (cmd.command_type == 1) slash_flatten_options(&cmd, ccord_json_get(cj, "options"), 0);
    ccord_error verr;
    if (ccord_slash_command_validate(&cmd, &verr) != CCORD_OK) continue;
    cmds[out_n++] = cmd;
  }
  out->ev.type = CCORD_NET_SLASH_COMMANDS;
  out->ev.guild_id = guild_id;
  out->ev.channel_id = channel_id;
  out->ev.slash_commands = cmds;
  out->ev.slash_command_count = out_n;
done:
  ccord_arena_free(&scratch);
  return st;
}


