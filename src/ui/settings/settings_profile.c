// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Settings > Profile: the own-profile editor (Main Profile + Per-Server Profiles, Markdown bio toolbar
// & live preview, 2-color Nitro Profile Themes, and 4 clean "Currently Equipped" cosmetic launchers
// opening specialized picker modals in settings_profile_pickers.c).
#include "settings_profile_internal.h"
#include "settings_internal.h"

#define PV_W 300.0f
#define PV_BANNER_H 96.0f
#define PV_AVATAR_D 80.0f
#define BIO_BOX_H 112.0f
#define BAR_H 56.0f
#define SAVED_MS 1800u
#define WHEEL_STEP 108.0f
#define ACCENT_PRESETS 8

static const uint32_t k_presets[ACCENT_PRESETS] = {0x1ABC9C, 0x2ECC71, 0x3498DB, 0x9B59B6,
                                                   0xE91E63, 0xF1C40F, 0xE67E22, 0xE74C3C};

static profile_state *S;
static uintptr_t s_gen = 1;
static unsigned s_img_seq;

// ---------------------------------------------------------------- shared helpers

const ccord_user *sp_self_user(const ccord_ui *ui)
{
  if (!ui || !ui->d.store) return nullptr;
  return ccord_store_user(ui->d.store, ccord_store_self_id(ui->d.store));
}

ccord_color sp_rgb(uint32_t v)
{
  return (ccord_color){(uint8_t)((v >> 16) & 0xffu), (uint8_t)((v >> 8) & 0xffu), (uint8_t)(v & 0xffu), 255};
}

static void copy_str(char *dst, size_t cap, const char *src)
{
  snprintf(dst, cap, "%s", src ? src : "");
}

static ccord_id first_joined_guild(const ccord_ui *ui)
{
  if (!ui || !ui->d.store) return CCORD_ID_NONE;
  size_t ng = ccord_store_guild_count(ui->d.store);
  for (size_t i = 0; i < ng; i++) {
    const ccord_guild *g = ccord_store_guild_at(ui->d.store, i);
    if (g && g->id != CCORD_ID_NONE && g->id != CCORD_DM_GUILD_ID) return g->id;
  }
  return CCORD_ID_NONE;
}

static ccord_id active_guild_id(const profile_state *s)
{
  return (s && s->subtab == 1) ? s->selected_guild : CCORD_ID_NONE;
}

static void set_theme_hex_texts(void);

static void request_live_profile_data(ccord_ui *ui, ccord_id guild_id)
{
  if (!ui || !ui->d.session || !ui->d.store) return;
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (self == CCORD_ID_NONE) return;
  ccord_error err = {0};
  if (guild_id != CCORD_ID_NONE) {
    if (S && S->last_fetched_guild != guild_id) {
      S->last_fetched_guild = guild_id;
      (void)ccord_session_request_profile_ex(ui->d.session, self, guild_id, ui->now_ms, false, &err);
    }
  } else if (S && !S->self_profile_requested) {
    S->self_profile_requested = true;
    (void)ccord_session_request_profile_ex(ui->d.session, self, CCORD_ID_NONE, ui->now_ms, false, &err);
  }
  if (S && !S->collectibles_requested) {
    S->collectibles_requested = true;
    (void)ccord_session_fetch_collectibles(ui->d.session, false, &err);
  }
}

static void read_snap(const ccord_ui *ui, ccord_id guild_id, prof_snap *out)
{
  *out = (prof_snap){.accent = -1};
  const ccord_user *u = sp_self_user(ui);
  if (!u) return;
  const ccord_user_profile *prof = ui->d.store ? ccord_store_user_profile(ui->d.store, u->id) : nullptr;
  bool match_guild = (guild_id != CCORD_ID_NONE && prof && prof->guild_id == guild_id);

  if (guild_id != CCORD_ID_NONE) {
    // Per-server overrides
    const ccord_member *mem = ui->d.store ? ccord_store_find_member(ui->d.store, guild_id, u->id) : nullptr;
    const char *nick = (match_guild && prof->guild_nick && prof->guild_nick[0]) ? prof->guild_nick
                       : ((mem && mem->nick[0]) ? mem->nick : "");
    copy_str(out->name, sizeof out->name, nick);
    copy_str(out->pronouns, sizeof out->pronouns, (match_guild && prof->guild_pronouns) ? prof->guild_pronouns : "");
    copy_str(out->bio, sizeof out->bio, (match_guild && prof->guild_bio) ? prof->guild_bio : "");
    copy_str(out->avatar_url, sizeof out->avatar_url, (match_guild && prof->guild_avatar_url) ? prof->guild_avatar_url : "");
    copy_str(out->banner_url, sizeof out->banner_url, (match_guild && prof->guild_banner_url) ? prof->guild_banner_url : "");
    if (match_guild && prof->guild_has_theme) {
      out->has_theme = true;
      out->theme_primary = prof->guild_theme_primary & 0xffffffu;
      out->theme_accent = prof->guild_theme_accent & 0xffffffu;
    }
    if (match_guild && prof->has_banner_color) {
      out->accent = (int32_t)(prof->banner_color & 0xffffffu);
    } else if (match_guild && (prof->has_accent_color || prof->accent_color != 0)) {
      out->accent = (int32_t)(prof->accent_color & 0xffffffu);
    }
    const char *gdurl = (match_guild && prof->guild_avatar_decoration_url && prof->guild_avatar_decoration_url[0])
                            ? prof->guild_avatar_decoration_url
                            : ((mem && mem->avatar_decoration_url[0]) ? mem->avatar_decoration_url : "");
    ccord_id gdsku = match_guild ? prof->guild_avatar_decoration_sku_id : CCORD_ID_NONE;
    if (gdurl[0] || gdsku != CCORD_ID_NONE) {
      out->deco_sku_id = gdsku;
      copy_str(out->deco_url, sizeof out->deco_url, gdurl);
      const ccord_owned_collectible *e = sp_find_owned_deco(ui, gdsku, gdurl);
      if (e) {
        out->deco_sku_id = e->sku_id;
        copy_str(out->deco_asset, sizeof out->deco_asset, e->asset);
        if (!out->deco_url[0]) copy_str(out->deco_url, sizeof out->deco_url, e->cdn_url);
        copy_str(out->deco_name, sizeof out->deco_name, e->name);
      } else {
        copy_str(out->deco_name, sizeof out->deco_name, "Equipped Decoration");
      }
    }
    const char *gnpa = (match_guild && prof->guild_nameplate_asset[0])
                           ? prof->guild_nameplate_asset
                           : ((mem && mem->nameplate_asset[0]) ? mem->nameplate_asset : "");
    const char *gnpp = (match_guild && prof->guild_nameplate_asset[0])
                           ? prof->guild_nameplate_palette
                           : ((mem && mem->nameplate_asset[0]) ? mem->nameplate_palette : "");
    ccord_id gnsku = match_guild ? prof->guild_nameplate_sku_id : CCORD_ID_NONE;
    if (gnpa[0] || gnsku != CCORD_ID_NONE) {
      out->nameplate_sku_id = gnsku;
      copy_str(out->nameplate_asset, sizeof out->nameplate_asset, gnpa);
      copy_str(out->nameplate_palette, sizeof out->nameplate_palette, gnpp);
      const ccord_owned_collectible *e = sp_find_owned_nameplate(ui, gnsku, gnpa);
      if (e) {
        out->nameplate_sku_id = e->sku_id;
        copy_str(out->nameplate_asset, sizeof out->nameplate_asset, e->asset);
        if (!out->nameplate_palette[0]) copy_str(out->nameplate_palette, sizeof out->nameplate_palette, e->palette);
        copy_str(out->nameplate_name, sizeof out->nameplate_name, e->name);
      } else {
        copy_str(out->nameplate_name, sizeof out->nameplate_name, "Equipped Nameplate");
      }
    }
    if (match_guild && prof->guild_profile_effect_id != CCORD_ID_NONE) {
      snprintf(out->effect_id, sizeof out->effect_id, "%llu", (unsigned long long)prof->guild_profile_effect_id);
      const ccord_owned_collectible *e = sp_find_owned_effect(ui, out->effect_id);
      if (e) {
        copy_str(out->effect_name, sizeof out->effect_name, e->name);
        copy_str(out->effect_thumb, sizeof out->effect_thumb, e->cdn_url);
        if (e->has_effect) { out->has_effect_cfg = true; out->effect_cfg = e->effect; }
      }
      if (!out->has_effect_cfg && prof->profile_effect.id != CCORD_ID_NONE) {
        out->has_effect_cfg = true;
        out->effect_cfg = prof->profile_effect;
        if (!out->effect_name[0] && prof->profile_effect.title[0]) {
          copy_str(out->effect_name, sizeof out->effect_name, prof->profile_effect.title);
        }
        if (!out->effect_thumb[0]) {
          copy_str(out->effect_thumb, sizeof out->effect_thumb,
                   prof->profile_effect.thumbnail_url[0] ? prof->profile_effect.thumbnail_url
                                                         : prof->profile_effect.static_frame_url);
        }
      }
      if (!out->effect_name[0]) copy_str(out->effect_name, sizeof out->effect_name, "Equipped Effect");
    }
    if (match_guild && prof->guild_profile_frame_sku_id != CCORD_ID_NONE) {
      out->frame_sku_id = prof->guild_profile_frame_sku_id;
      const ccord_owned_collectible *f = sp_find_owned_frame(ui, out->frame_sku_id);
      if (f) {
        copy_str(out->frame_name, sizeof out->frame_name, f->name);
        copy_str(out->frame_thumb, sizeof out->frame_thumb, f->cdn_url[0] ? f->cdn_url : f->static_url);
        if (f->has_frame) { out->has_frame_cfg = true; out->frame_cfg = f->frame; }
      }
      if (!out->has_frame_cfg && prof->profile_frame.sku_id == out->frame_sku_id) {
        out->has_frame_cfg = true;
        out->frame_cfg = prof->profile_frame;
        if (!out->frame_thumb[0]) {
          copy_str(out->frame_thumb, sizeof out->frame_thumb,
                   prof->profile_frame.animated_url[0] ? prof->profile_frame.animated_url
                                                       : (prof->profile_frame.static_url[0]
                                                            ? prof->profile_frame.static_url
                                                            : (prof->profile_frame.layer_count > 0
                                                                 ? prof->profile_frame.layers[0].url
                                                                 : "")));
        }
      }
      if (!out->frame_name[0]) copy_str(out->frame_name, sizeof out->frame_name, "Equipped Frame");
    }
    return;
  }

  // Main global profile
  copy_str(out->name, sizeof out->name, u->display_name);
  copy_str(out->pronouns, sizeof out->pronouns, u->pronouns);
  copy_str(out->bio, sizeof out->bio, u->bio);
  copy_str(out->avatar_url, sizeof out->avatar_url, u->avatar_url);
  const char *burl = (u->banner_url && u->banner_url[0]) ? u->banner_url
                     : ((prof && prof->banner_url && prof->banner_url[0]) ? prof->banner_url : "");
  copy_str(out->banner_url, sizeof out->banner_url, burl);
  if (prof && prof->has_theme) {
    out->has_theme = true;
    out->theme_primary = prof->theme_primary & 0xffffffu;
    out->theme_accent = prof->theme_accent & 0xffffffu;
  }
  // Prefer user_profile.banner_color / accent_color over stale user.accent_color.
  if (prof && prof->has_banner_color) {
    out->accent = (int32_t)(prof->banner_color & 0xffffffu);
  } else if (prof && (prof->has_accent_color || prof->accent_color != 0)) {
    out->accent = (int32_t)(prof->accent_color & 0xffffffu);
  } else if (u->has_accent_color || u->accent_color != 0) {
    out->accent = (int32_t)(u->accent_color & 0xffffffu);
  }
  const char *durl = (prof && prof->avatar_decoration_url && prof->avatar_decoration_url[0])
                         ? prof->avatar_decoration_url
                         : ((u->avatar_decoration_url && u->avatar_decoration_url[0]) ? u->avatar_decoration_url : "");
  ccord_id dsku = (prof && prof->avatar_decoration_sku_id != CCORD_ID_NONE)
                      ? prof->avatar_decoration_sku_id : u->avatar_decoration_sku_id;
  if (durl[0] || dsku != CCORD_ID_NONE) {
    out->deco_sku_id = dsku;
    copy_str(out->deco_url, sizeof out->deco_url, durl);
    const ccord_owned_collectible *e = sp_find_owned_deco(ui, dsku, durl);
    if (e) {
      out->deco_sku_id = e->sku_id;
      copy_str(out->deco_asset, sizeof out->deco_asset, e->asset);
      if (!out->deco_url[0]) copy_str(out->deco_url, sizeof out->deco_url, e->cdn_url);
      copy_str(out->deco_name, sizeof out->deco_name, e->name);
    } else {
      copy_str(out->deco_name, sizeof out->deco_name, "Equipped Decoration");
    }
  }
  if (prof && prof->profile_effect_id != CCORD_ID_NONE) {
    snprintf(out->effect_id, sizeof out->effect_id, "%llu", (unsigned long long)prof->profile_effect_id);
    const ccord_owned_collectible *e = sp_find_owned_effect(ui, out->effect_id);
    if (e) {
      copy_str(out->effect_name, sizeof out->effect_name, e->name);
      copy_str(out->effect_thumb, sizeof out->effect_thumb, e->cdn_url);
      if (e->has_effect) { out->has_effect_cfg = true; out->effect_cfg = e->effect; }
    }
    if (!out->has_effect_cfg && prof->profile_effect.id != CCORD_ID_NONE) {
      out->has_effect_cfg = true;
      out->effect_cfg = prof->profile_effect;
      if (!out->effect_name[0] && prof->profile_effect.title[0]) {
        copy_str(out->effect_name, sizeof out->effect_name, prof->profile_effect.title);
      }
      if (!out->effect_thumb[0]) {
        copy_str(out->effect_thumb, sizeof out->effect_thumb,
                 prof->profile_effect.thumbnail_url[0] ? prof->profile_effect.thumbnail_url
                                                       : prof->profile_effect.static_frame_url);
      }
    }
    if (!out->effect_name[0]) copy_str(out->effect_name, sizeof out->effect_name, "Equipped Effect");
  }
  if (prof && prof->profile_frame_sku_id != CCORD_ID_NONE) {
    out->frame_sku_id = prof->profile_frame_sku_id;
    const ccord_owned_collectible *f = sp_find_owned_frame(ui, out->frame_sku_id);
    if (f) {
      copy_str(out->frame_name, sizeof out->frame_name, f->name);
      copy_str(out->frame_thumb, sizeof out->frame_thumb, f->cdn_url[0] ? f->cdn_url : f->static_url);
      if (f->has_frame) { out->has_frame_cfg = true; out->frame_cfg = f->frame; }
    }
    if (!out->has_frame_cfg && prof->profile_frame.sku_id == out->frame_sku_id) {
      out->has_frame_cfg = true;
      out->frame_cfg = prof->profile_frame;
      if (!out->frame_thumb[0]) {
        copy_str(out->frame_thumb, sizeof out->frame_thumb,
                 prof->profile_frame.animated_url[0] ? prof->profile_frame.animated_url
                                                     : (prof->profile_frame.static_url[0]
                                                          ? prof->profile_frame.static_url
                                                          : (prof->profile_frame.layer_count > 0
                                                               ? prof->profile_frame.layers[0].url
                                                               : "")));
      }
    }
    if (!out->frame_name[0]) copy_str(out->frame_name, sizeof out->frame_name, "Equipped Frame");
  }
  const char *npa = (prof && prof->nameplate_asset[0]) ? prof->nameplate_asset : u->nameplate_asset;
  const char *npp = (prof && prof->nameplate_asset[0]) ? prof->nameplate_palette : u->nameplate_palette;
  ccord_id nsku = (prof && prof->nameplate_sku_id != CCORD_ID_NONE) ? prof->nameplate_sku_id : u->nameplate_sku_id;
  if ((npa && npa[0]) || nsku != CCORD_ID_NONE) {
    out->nameplate_sku_id = nsku;
    copy_str(out->nameplate_asset, sizeof out->nameplate_asset, npa ? npa : "");
    copy_str(out->nameplate_palette, sizeof out->nameplate_palette, npp ? npp : "");
    const ccord_owned_collectible *e = sp_find_owned_nameplate(ui, nsku, npa);
    if (e) {
      out->nameplate_sku_id = e->sku_id;
      copy_str(out->nameplate_asset, sizeof out->nameplate_asset, e->asset);
      if (!out->nameplate_palette[0]) copy_str(out->nameplate_palette, sizeof out->nameplate_palette, e->palette);
      copy_str(out->nameplate_name, sizeof out->nameplate_name, e->name);
    } else {
      copy_str(out->nameplate_name, sizeof out->nameplate_name, "Equipped Nameplate");
    }
  }
  if (u->clan_guild_id != CCORD_ID_NONE || u->clan_tag[0]) {
    out->clan_guild_id = u->clan_guild_id;
    copy_str(out->clan_tag, sizeof out->clan_tag, u->clan_tag);
    copy_str(out->clan_badge, sizeof out->clan_badge, u->clan_badge);
  }
}

static bool snap_eq(const prof_snap *a, const prof_snap *b)
{
  return a->accent == b->accent && a->has_theme == b->has_theme &&
         a->theme_primary == b->theme_primary && a->theme_accent == b->theme_accent &&
         a->deco_sku_id == b->deco_sku_id && a->frame_sku_id == b->frame_sku_id &&
         a->nameplate_sku_id == b->nameplate_sku_id && a->clan_guild_id == b->clan_guild_id &&
         a->has_effect_cfg == b->has_effect_cfg && a->has_frame_cfg == b->has_frame_cfg &&
         strcmp(a->name, b->name) == 0 && strcmp(a->pronouns, b->pronouns) == 0 &&
         strcmp(a->bio, b->bio) == 0 && strcmp(a->avatar_url, b->avatar_url) == 0 &&
         strcmp(a->banner_url, b->banner_url) == 0 && strcmp(a->deco_asset, b->deco_asset) == 0 &&
         strcmp(a->deco_url, b->deco_url) == 0 && strcmp(a->effect_id, b->effect_id) == 0 &&
         strcmp(a->effect_thumb, b->effect_thumb) == 0 && strcmp(a->frame_thumb, b->frame_thumb) == 0 &&
         strcmp(a->nameplate_asset, b->nameplate_asset) == 0 && strcmp(a->clan_tag, b->clan_tag) == 0;
}

static void ed_set(ccord_text_edit *t, const char *s)
{
  ccord_text_edit_clear(t);
  ccord_error e;
  if (s && s[0]) (void)ccord_text_edit_insert(t, s, strlen(s), &e);
}

static void set_hex_text(void)
{
  char hex[16] = "";
  if (S->accent >= 0) snprintf(hex, sizeof hex, "%06X", (unsigned)S->accent);
  ed_set(&S->ed[F_HEX], hex);
  S->hex_bad = false;
}

static void set_theme_hex_texts(void)
{
  char hex[16] = "";
  if (S->draft_has_theme) snprintf(hex, sizeof hex, "%06X", (unsigned)(S->draft_theme_primary & 0xffffffu));
  ed_set(&S->theme_ed[F_THEME_PRI], hex);
  hex[0] = '\0';
  if (S->draft_has_theme) snprintf(hex, sizeof hex, "%06X", (unsigned)(S->draft_theme_accent & 0xffffffu));
  ed_set(&S->theme_ed[F_THEME_ACC], hex);
}

void sp_sync_color_drafts(void)
{
  if (!S) return;
  set_hex_text();
  set_theme_hex_texts();
}

static void load_field(int f)
{
  switch (f) {
    case F_NAME: ed_set(&S->ed[F_NAME], S->base.name); break;
    case F_PRONOUNS: ed_set(&S->ed[F_PRONOUNS], S->base.pronouns); break;
    case F_BIO: ed_set(&S->ed[F_BIO], S->base.bio); break;
    case F_HEX:
      S->accent = S->base.accent;
      set_hex_text();
      break;
    default: break;
  }
}

static void img_free(prof_img_draft *d)
{
  ccord_free(d->orig_data);
  ccord_free(d->data);
  *d = (prof_img_draft){.zoom = 1.0f};
}

static void clear_errors(void)
{
  memset(S->err, 0, sizeof S->err);
  S->general[0] = '\0';
}

static void load_cosmetics_and_theme(void)
{
  S->draft_has_theme = S->base.has_theme;
  S->draft_theme_primary = S->base.theme_primary;
  S->draft_theme_accent = S->base.theme_accent;
  set_theme_hex_texts();
  S->draft_deco_sku = S->base.deco_sku_id;
  copy_str(S->draft_deco_asset, sizeof S->draft_deco_asset, S->base.deco_asset);
  copy_str(S->draft_deco_url, sizeof S->draft_deco_url, S->base.deco_url);
  copy_str(S->draft_deco_name, sizeof S->draft_deco_name, S->base.deco_name);
  copy_str(S->draft_effect_id, sizeof S->draft_effect_id, S->base.effect_id);
  copy_str(S->draft_effect_name, sizeof S->draft_effect_name, S->base.effect_name);
  copy_str(S->draft_effect_thumb, sizeof S->draft_effect_thumb, S->base.effect_thumb);
  S->draft_has_effect_cfg = S->base.has_effect_cfg;
  S->draft_effect_cfg = S->base.effect_cfg;
  S->draft_frame_sku = S->base.frame_sku_id;
  copy_str(S->draft_frame_name, sizeof S->draft_frame_name, S->base.frame_name);
  copy_str(S->draft_frame_thumb, sizeof S->draft_frame_thumb, S->base.frame_thumb);
  S->draft_has_frame_cfg = S->base.has_frame_cfg;
  S->draft_frame_cfg = S->base.frame_cfg;
  S->draft_np_sku = S->base.nameplate_sku_id;
  copy_str(S->draft_np_asset, sizeof S->draft_np_asset, S->base.nameplate_asset);
  copy_str(S->draft_np_palette, sizeof S->draft_np_palette, S->base.nameplate_palette);
  copy_str(S->draft_np_name, sizeof S->draft_np_name, S->base.nameplate_name);
  S->draft_clan_guild = S->base.clan_guild_id;
  copy_str(S->draft_clan_tag, sizeof S->draft_clan_tag, S->base.clan_tag);
  copy_str(S->draft_clan_badge, sizeof S->draft_clan_badge, S->base.clan_badge);
}

static void load_all(void)
{
  for (int f = 0; f < F_TEXT_COUNT; f++) load_field(f);
  img_free(&S->img[IMG_AVATAR]);
  img_free(&S->img[IMG_BANNER]);
  load_cosmetics_and_theme();
}

static uint32_t changed_mask(void)
{
  uint32_t m = 0;
  if (strcmp(ccord_text_edit_cstr(&S->ed[F_NAME]), S->base.name) != 0) m |= 1u << CCORD_PROFILE_FIELD_DISPLAY_NAME;
  if (strcmp(ccord_text_edit_cstr(&S->ed[F_PRONOUNS]), S->base.pronouns) != 0) m |= 1u << CCORD_PROFILE_FIELD_PRONOUNS;
  if (strcmp(ccord_text_edit_cstr(&S->ed[F_BIO]), S->base.bio) != 0) m |= 1u << CCORD_PROFILE_FIELD_BIO;
  if (S->accent != S->base.accent || S->hex_bad) m |= 1u << CCORD_PROFILE_FIELD_ACCENT;
  if (S->img[IMG_AVATAR].mode != IMG_KEEP) m |= 1u << CCORD_PROFILE_FIELD_AVATAR;
  if (S->img[IMG_BANNER].mode != IMG_KEEP) m |= 1u << CCORD_PROFILE_FIELD_BANNER;
  if (S->draft_has_theme != S->base.has_theme ||
      (S->draft_has_theme && (S->draft_theme_primary != S->base.theme_primary || S->draft_theme_accent != S->base.theme_accent))) {
    m |= 1u << CCORD_PROFILE_FIELD_THEME_COLORS;
  }
  if (S->draft_deco_sku != S->base.deco_sku_id ||
      (S->draft_deco_sku == CCORD_ID_NONE && S->base.deco_sku_id == CCORD_ID_NONE &&
       (strcmp(S->draft_deco_asset, S->base.deco_asset) != 0 || strcmp(S->draft_deco_url, S->base.deco_url) != 0))) {
    m |= 1u << CCORD_PROFILE_FIELD_AVATAR_DECORATION;
  }
  if (strcmp(S->draft_effect_id, S->base.effect_id) != 0) {
    m |= 1u << CCORD_PROFILE_FIELD_PROFILE_EFFECT;
  }
  if (S->draft_frame_sku != S->base.frame_sku_id) {
    m |= 1u << CCORD_PROFILE_FIELD_PROFILE_FRAME;
  }
  if (S->draft_np_sku != S->base.nameplate_sku_id ||
      (S->draft_np_sku == CCORD_ID_NONE && S->base.nameplate_sku_id == CCORD_ID_NONE &&
       strcmp(S->draft_np_asset, S->base.nameplate_asset) != 0)) {
    m |= 1u << CCORD_PROFILE_FIELD_NAMEPLATE;
  }
  if (S->subtab == 0 && (S->draft_clan_guild != S->base.clan_guild_id || strcmp(S->draft_clan_tag, S->base.clan_tag) != 0)) {
    m |= 1u << CCORD_PROFILE_FIELD_CLAN_GUILD;
  }
  return m;
}

static void sync_store(const ccord_ui *ui)
{
  prof_snap cur;
  read_snap(ui, active_guild_id(S), &cur);
  if (snap_eq(&cur, &S->base)) return;
  bool clean = changed_mask() == 0;
  S->base = cur;
  if (clean) load_all();
}

static bool ensure_live(ccord_ui *ui)
{
  if (S && S->owner != ui) ui_settings_profile_release(nullptr);
  if (S) return true;
  S = ccord_calloc(1, sizeof *S);
  if (!S) return false;
  S->owner = ui;
  S->awaiting = -1;
  S->theme_hex_focus = -1;
  S->accent = -1;
  S->pv_accent = -1;
  S->img[0].zoom = 1.0f;
  S->img[1].zoom = 1.0f;
  S->selected_guild = first_joined_guild(ui);
  ccord_text_edit_init(&S->ed[F_NAME], CCORD_PROFILE_DISPLAY_NAME_MAX * 4u);
  ccord_text_edit_init(&S->ed[F_PRONOUNS], CCORD_PROFILE_PRONOUNS_MAX * 4u);
  ccord_text_edit_init(&S->ed[F_BIO], CCORD_PROFILE_BIO_MAX * 4u);
  ccord_text_edit_init(&S->ed[F_HEX], 16u);
  ccord_text_edit_init(&S->theme_ed[F_THEME_PRI], 16u);
  ccord_text_edit_init(&S->theme_ed[F_THEME_ACC], 16u);
  request_live_profile_data(ui, CCORD_ID_NONE);
  read_snap(ui, CCORD_ID_NONE, &S->base);
  load_all();
  return true;
}

void ui_settings_profile_release(ccord_ui *ui)
{
  (void)ui;
  if (!S) return;
  for (int f = 0; f < F_TEXT_COUNT; f++) ccord_text_edit_free(&S->ed[f]);
  for (int f = 0; f < F_THEME_HEX_COUNT; f++) ccord_text_edit_free(&S->theme_ed[f]);
  img_free(&S->img[IMG_AVATAR]);
  img_free(&S->img[IMG_BANNER]);
  sp_picker_free(S);
  ccord_free(S);
  S = nullptr;
  s_gen++;
}

bool ui_settings_profile_wants_text(const ccord_ui *ui)
{
  if (!ui || !ui->settings_open || ui->settings_page != UI_SETTINGS_PAGE_PROFILE) return false;
  if (S && S->picker.kind != PROF_PICK_NONE) return S->picker.search_focused;
  if (S && S->theme_hex_focus >= 0) return true;
  return ui->account_focus_field >= UI_PROFILE_FOCUS_BASE && ui->account_focus_field < UI_PROFILE_FOCUS_BASE + F_TEXT_COUNT;
}

bool ui_settings_profile_has_overlay(const ccord_ui *ui)
{
  return ui && ui->settings_open && ui->settings_page == UI_SETTINGS_PAGE_PROFILE && S && S->picker.kind != PROF_PICK_NONE;
}

bool ui_settings_profile_dismiss_overlay(ccord_ui *ui)
{
  if (!ui || ui->settings_page != UI_SETTINGS_PAGE_PROFILE || !S) return false;
  if (S->picker.kind != PROF_PICK_NONE) {
    sp_picker_close(ui, S);
    return true;
  }
  if (S->guild_dropdown_open) {
    S->guild_dropdown_open = false;
    ui->dirty = true;
    return true;
  }
  return false;
}

void ui_settings_open_profile(ccord_ui *ui)
{
  ui_open_settings(ui, true);
  ui->settings_page = UI_SETTINGS_PAGE_PROFILE;
  ui->settings_category = 0;
  ui->account_focus_field = 0;
  ui->dirty = true;
}

// ---------------------------------------------------------------- text helpers

static size_t cp_count(const char *s, size_t len)
{
  size_t n = 0;
  for (size_t p = 0; p < len; p = ccord_utf8_next(s, len, p)) n++;
  return n;
}

static size_t field_max_cp(int f)
{
  switch (f) {
    case F_NAME: return CCORD_PROFILE_DISPLAY_NAME_MAX;
    case F_PRONOUNS: return CCORD_PROFILE_PRONOUNS_MAX;
    case F_BIO: return CCORD_PROFILE_BIO_MAX;
    default: return 7;
  }
}

static bool parse_hex(const char *s, int32_t *out)
{
  if (*s == '#') s++;
  if (strlen(s) != 6) return false;
  uint32_t v = 0;
  for (int i = 0; i < 6; i++) {
    char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
    else return false;
    v = v * 16u + d;
  }
  *out = (int32_t)v;
  return true;
}

static void edited(ccord_ui *ui, int f)
{
  static const int k_field[F_TEXT_COUNT] = {CCORD_PROFILE_FIELD_DISPLAY_NAME, CCORD_PROFILE_FIELD_PRONOUNS,
                                            CCORD_PROFILE_FIELD_BIO, CCORD_PROFILE_FIELD_ACCENT};
  S->err[k_field[f]][0] = '\0';
  if (f == F_HEX) {
    const char *t = ccord_text_edit_cstr(&S->ed[F_HEX]);
    int32_t v;
    if (t[0] == '\0') {
      S->accent = -1;
      S->hex_bad = false;
    } else if (parse_hex(t, &v)) {
      S->accent = v;
      S->hex_bad = false;
    } else {
      S->hex_bad = true;
    }
  }
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
  ui->dirty = true;
}

static void insert_capped(ccord_ui *ui, int f, const char *s, size_t len)
{
  ccord_text_edit *t = &S->ed[f];
  bool multi = f == F_BIO;
  char buf[1024];
  size_t o = 0;
  size_t sel = t->caret > t->anchor ? t->caret - t->anchor : t->anchor - t->caret;
  size_t lo = t->caret < t->anchor ? t->caret : t->anchor;
  size_t used = cp_count(t->data ? t->data : "", t->len) - cp_count(t->data ? t->data + lo : "", sel);
  size_t room = field_max_cp(f) > used ? field_max_cp(f) - used : 0;
  for (size_t p = 0; p < len && o < sizeof buf - 8u && room > 0;) {
    size_t n = ccord_utf8_next(s, len, p) - p;
    if (n == 1 && (s[p] == '\r' || (!multi && s[p] == '\n') || (f == F_HEX && !strchr("0123456789abcdefABCDEF#", s[p])))) {
      if (s[p] == '\n' || s[p] == '\r') {
        if (!multi && s[p] == '\n' && o > 0 && buf[o - 1] != ' ') {
          buf[o++] = ' ';
          room--;
        }
      }
      p += n;
      continue;
    }
    if (n == 1 && (unsigned char)s[p] < 0x20 && s[p] != '\n') {
      p += n;
      continue;
    }
    memcpy(buf + o, s + p, n);
    o += n;
    p += n;
    room--;
  }
  if (o == 0) return;
  ccord_error e;
  if (ccord_text_edit_insert(t, buf, o, &e) == CCORD_OK) edited(ui, f);
}

static void apply_bio_format(ccord_ui *ui, int fmt_kind)
{
  ccord_text_edit *t = &S->ed[F_BIO];
  if (fmt_kind == 4) {
    insert_capped(ui, F_BIO, "\xE2\x9C\xA8", 3);   // sparkle emoji ✨
    return;
  }
  const char *wrap = fmt_kind == 0 ? "**" : fmt_kind == 1 ? "*" : fmt_kind == 2 ? "`" : "||";
  size_t wlen = strlen(wrap);
  char *sel = ccord_text_edit_copy_selection(t);
  char buf[1024];
  if (sel && sel[0]) {
    snprintf(buf, sizeof buf, "%s%s%s", wrap, sel, wrap);
    ccord_free(sel);
    insert_capped(ui, F_BIO, buf, strlen(buf));
  } else {
    ccord_free(sel);
    snprintf(buf, sizeof buf, "%s%s", wrap, wrap);
    size_t before = t->caret;
    insert_capped(ui, F_BIO, buf, strlen(buf));
    if (t->caret >= before + wlen * 2u) ccord_text_edit_set_caret(t, t->caret - wlen, false);
  }
}

// ---------------------------------------------------------------- picking images

static const char *kind_noun(int slot)
{
  return slot == IMG_BANNER ? "banner" : "avatar";
}

static bool take_image(ccord_ui *ui, int slot, const char *name, const void *data, size_t size)
{
  (void)name;
  if (!data || size == 0) return false;
  if (size > CCORD_PROFILE_IMAGE_MAX_BYTES) {
    ui_notice(ui, "The %s must be 10 MB or smaller", kind_noun(slot));
    return true;
  }
  const char *mime = ccord_profile_image_mime(data, size);
  if (!mime) {
    ui_notice(ui, "Choose a PNG, JPEG, GIF or WebP image");
    return true;
  }
  uint8_t *orig = ccord_malloc(size);
  uint8_t *copy = ccord_malloc(size);
  if (!orig || !copy) {
    ccord_free(orig);
    ccord_free(copy);
    return true;
  }
  memcpy(orig, data, size);
  memcpy(copy, data, size);
  prof_img_draft *d = &S->img[slot];
  img_free(d);
  d->mode = IMG_NEW;
  d->orig_data = orig;
  d->orig_len = size;
  d->data = copy;
  d->len = size;
  d->is_gif = (strcmp(mime, "image/gif") == 0);
  d->seq = ++s_img_seq;
  d->zoom = 1.0f;
  d->pan_x = 0.0f;
  d->pan_y = 0.0f;
  snprintf(d->key, sizeof d->key, "profile-draft:%s:%u", slot == IMG_BANNER ? "banner" : "avatar", d->seq);
  S->err[slot == IMG_BANNER ? CCORD_PROFILE_FIELD_BANNER : CCORD_PROFILE_FIELD_AVATAR][0] = '\0';
  S->awaiting = -1;
  ui->dirty = true;
  return true;
}

bool sp_apply_img_crop(ccord_ui *ui, prof_img_draft *d, int slot, float zoom, float pan_x, float pan_y)
{
  if (!ui || !d || d->mode != IMG_NEW || !d->orig_data || d->orig_len == 0) return false;
  if (zoom < 1.0f) zoom = 1.0f;
  if (zoom > 3.0f) zoom = 3.0f;
  if (pan_x < -1.0f) pan_x = -1.0f;
  if (pan_x > 1.0f) pan_x = 1.0f;
  if (pan_y < -1.0f) pan_y = -1.0f;
  if (pan_y > 1.0f) pan_y = 1.0f;
  if (d->is_gif) {
    if (zoom > 1.01f || fabsf(pan_x) > 0.01f || fabsf(pan_y) > 0.01f) {
      ui_notice(ui, "Animated GIFs upload uncropped to preserve animation");
    }
    d->zoom = 1.0f;
    d->pan_x = 0.0f;
    d->pan_y = 0.0f;
    ui->dirty = true;
    return true;
  }
  uint8_t *next_data = nullptr;
  size_t next_len = 0;
  if (zoom <= 1.01f && fabsf(pan_x) < 0.01f && fabsf(pan_y) < 0.01f) {
    next_data = ccord_malloc(d->orig_len);
    if (!next_data) return false;
    memcpy(next_data, d->orig_data, d->orig_len);
    next_len = d->orig_len;
    zoom = 1.0f;
    pan_x = 0.0f;
    pan_y = 0.0f;
  } else {
    int out_w = (slot == IMG_BANNER) ? 680 : 512;
    int out_h = (slot == IMG_BANNER) ? 240 : 512;
    ccord_error err = {0};
    if (ccord_image_crop_png(d->orig_data, d->orig_len, zoom, pan_x, pan_y, out_w, out_h,
                             &next_data, &next_len, &err) != CCORD_OK) {
      ui_notice(ui, "%s", err.message[0] ? err.message : "Couldn't crop image");
      return false;
    }
  }
  ccord_free(d->data);
  d->data = next_data;
  d->len = next_len;
  d->zoom = zoom;
  d->pan_x = pan_x;
  d->pan_y = pan_y;
  d->seq = ++s_img_seq;
  snprintf(d->key, sizeof d->key, "profile-draft:%s:%u", slot == IMG_BANNER ? "banner" : "avatar", d->seq);
  d->uploaded = false;
  ui->dirty = true;
  return true;
}

bool ui_settings_profile_take_image(ccord_ui *ui, const char *filename, const void *data, size_t size)
{
  if (!ui || !S || S->awaiting < 0 || !ui->settings_open || ui->settings_page != UI_SETTINGS_PAGE_PROFILE) return false;
  return take_image(ui, S->awaiting, filename, data, size);
}

bool ui_settings_profile_set_image_file(ccord_ui *ui, bool banner, const char *path)
{
  if (!ui || !path || !path[0]) return false;
  if (!ensure_live(ui)) return false;
  FILE *f = fopen(path, "rb");
  if (!f) {
    ui_notice(ui, "Cannot open file: %s", path);
    return false;
  }
  uint8_t *buf = nullptr;
  size_t got = 0;
  if (fseek(f, 0, SEEK_END) == 0) {
    long sz = ftell(f);
    if (sz > 0 && (unsigned long)sz <= CCORD_PROFILE_IMAGE_MAX_BYTES && fseek(f, 0, SEEK_SET) == 0) {
      buf = ccord_malloc((size_t)sz);
      if (buf) got = fread(buf, 1, (size_t)sz, f);
      if (got != (size_t)sz) got = 0;
    } else if (sz > 0) {
      fclose(f);
      ui_notice(ui, "The %s must be 10 MB or smaller", banner ? "banner" : "avatar");
      return false;
    }
  }
  fclose(f);
  bool ok = false;
  if (buf && got > 0 && ccord_profile_image_mime(buf, got)) {
    ok = take_image(ui, banner ? IMG_BANNER : IMG_AVATAR, path, buf, got);
  } else {
    ui_notice(ui, "Choose a PNG, JPEG, GIF or WebP image");
  }
  ccord_free(buf);
  return ok;
}

static void pick_image(ccord_ui *ui, int slot)
{
  S->awaiting = slot;
  if (!ui->d.host.open_file_dialog) {
    ui_notice(ui, "No file picker here: drop an image file onto the window");
    return;
  }
  char path[512] = "";
  if (ui->d.host.open_file_dialog(ui->d.host.ud, path, sizeof path) && path[0]) {
    (void)ui_settings_profile_set_image_file(ui, slot == IMG_BANNER, path);
  }
}

// ---------------------------------------------------------------- saving

static void on_done(void *ud, const ccord_profile_edit_result *res)
{
  if ((uintptr_t)ud != s_gen || !S) return;
  S->cb_res = *res;
  S->cb_done = true;
}

static void do_save(ccord_ui *ui)
{
  if (S->pending) return;
  clear_errors();
  ui->dirty = true;
  uint32_t m = changed_mask();
  if (!m) return;
  if (S->hex_bad) {
    copy_str(S->err[CCORD_PROFILE_FIELD_ACCENT], sizeof S->err[0], "Enter a 6-digit hex color, e.g. 3B82F6");
    return;
  }
  ccord_profile_edit e = CCORD_PROFILE_EDIT_INIT;
  e.guild_id = active_guild_id(S);
  if (m & (1u << CCORD_PROFILE_FIELD_DISPLAY_NAME)) {
    e.has_display_name = true;
    e.display_name = ccord_text_edit_cstr(&S->ed[F_NAME]);
  }
  if (m & (1u << CCORD_PROFILE_FIELD_PRONOUNS)) {
    e.has_pronouns = true;
    e.pronouns = ccord_text_edit_cstr(&S->ed[F_PRONOUNS]);
  }
  if (m & (1u << CCORD_PROFILE_FIELD_BIO)) {
    e.has_bio = true;
    e.bio = ccord_text_edit_cstr(&S->ed[F_BIO]);
  }
  if (m & (1u << CCORD_PROFILE_FIELD_ACCENT)) {
    e.has_accent = true;
    e.accent_color = S->accent;
  }
  if (m & (1u << CCORD_PROFILE_FIELD_AVATAR)) {
    e.has_avatar = true;
    e.avatar = S->img[IMG_AVATAR].mode == IMG_NEW ? S->img[IMG_AVATAR].data : nullptr;
    e.avatar_len = S->img[IMG_AVATAR].mode == IMG_NEW ? S->img[IMG_AVATAR].len : 0;
  }
  if (m & (1u << CCORD_PROFILE_FIELD_BANNER)) {
    e.has_banner = true;
    e.banner = S->img[IMG_BANNER].mode == IMG_NEW ? S->img[IMG_BANNER].data : nullptr;
    e.banner_len = S->img[IMG_BANNER].mode == IMG_NEW ? S->img[IMG_BANNER].len : 0;
  }
  if (m & (1u << CCORD_PROFILE_FIELD_THEME_COLORS)) {
    e.has_theme_colors = true;
    e.clear_theme_colors = !S->draft_has_theme;
    e.theme_primary = S->draft_theme_primary;
    e.theme_accent = S->draft_theme_accent;
  }
  if (m & (1u << CCORD_PROFILE_FIELD_AVATAR_DECORATION)) {
    e.has_avatar_decoration = true;
    e.avatar_decoration_sku_id = S->draft_deco_sku;
    e.avatar_decoration_asset = S->draft_deco_asset;
  }
  if (m & (1u << CCORD_PROFILE_FIELD_PROFILE_EFFECT)) {
    e.has_profile_effect = true;
    e.profile_effect_id = S->draft_effect_id;
  }
  if (m & (1u << CCORD_PROFILE_FIELD_PROFILE_FRAME)) {
    e.has_profile_frame = true;
    e.profile_frame_sku_id = S->draft_frame_sku;
  }
  if (m & (1u << CCORD_PROFILE_FIELD_NAMEPLATE)) {
    e.has_nameplate = true;
    e.nameplate_sku_id = S->draft_np_sku;
    e.nameplate_asset = S->draft_np_asset;
    e.nameplate_palette = S->draft_np_palette;
  }
  if (m & (1u << CCORD_PROFILE_FIELD_CLAN_GUILD)) {
    e.has_clan_guild = true;
    e.clan_guild_id = S->draft_clan_guild;
    e.clan_tag = S->draft_clan_tag;
    e.clan_badge = S->draft_clan_badge;
  }
  if (!ui->d.session) {
    copy_str(S->general, sizeof S->general, "Not connected to Discord");
    return;
  }
  ccord_error err;
  if (ccord_session_edit_profile(ui->d.session, &e, on_done, (void *)s_gen, &err) != CCORD_OK) {
    copy_str(S->general, sizeof S->general, err.message[0] ? err.message : "Couldn't save your profile");
    return;
  }
  S->pending = true;
  S->sent = m;
}

static void apply_result(ccord_ui *ui)
{
  const ccord_profile_edit_result *res = &S->cb_res;
  S->cb_done = false;
  S->pending = false;
  sync_store(ui);
  uint32_t ap = res->applied.fields;
  if (ap & (1u << CCORD_PROFILE_FIELD_AVATAR)) img_free(&S->img[IMG_AVATAR]);
  if (ap & (1u << CCORD_PROFILE_FIELD_BANNER)) img_free(&S->img[IMG_BANNER]);
  if (ap & (1u << CCORD_PROFILE_FIELD_DISPLAY_NAME)) load_field(F_NAME);
  if (ap & (1u << CCORD_PROFILE_FIELD_PRONOUNS)) load_field(F_PRONOUNS);
  if (ap & (1u << CCORD_PROFILE_FIELD_BIO)) load_field(F_BIO);
  if (ap & (1u << CCORD_PROFILE_FIELD_ACCENT)) load_field(F_HEX);
  if (ap & ((1u << CCORD_PROFILE_FIELD_THEME_COLORS) | (1u << CCORD_PROFILE_FIELD_AVATAR_DECORATION) |
            (1u << CCORD_PROFILE_FIELD_PROFILE_EFFECT) | (1u << CCORD_PROFILE_FIELD_PROFILE_FRAME) |
            (1u << CCORD_PROFILE_FIELD_NAMEPLATE) | (1u << CCORD_PROFILE_FIELD_CLAN_GUILD))) {
    // Re-sync applied cosmetics into base so Unsaved Changes clears cleanly even on mock/partial store updates
    if (ap & (1u << CCORD_PROFILE_FIELD_THEME_COLORS)) {
      S->base.has_theme = S->draft_has_theme;
      S->base.theme_primary = S->draft_theme_primary;
      S->base.theme_accent = S->draft_theme_accent;
      set_theme_hex_texts();
    }
    if (ap & (1u << CCORD_PROFILE_FIELD_AVATAR_DECORATION)) {
      S->base.deco_sku_id = S->draft_deco_sku;
      copy_str(S->base.deco_asset, sizeof S->base.deco_asset, S->draft_deco_asset);
      copy_str(S->base.deco_url, sizeof S->base.deco_url, S->draft_deco_url);
      copy_str(S->base.deco_name, sizeof S->base.deco_name, S->draft_deco_name);
    }
    if (ap & (1u << CCORD_PROFILE_FIELD_PROFILE_EFFECT)) {
      copy_str(S->base.effect_id, sizeof S->base.effect_id, S->draft_effect_id);
      copy_str(S->base.effect_name, sizeof S->base.effect_name, S->draft_effect_name);
      copy_str(S->base.effect_thumb, sizeof S->base.effect_thumb, S->draft_effect_thumb);
      S->base.has_effect_cfg = S->draft_has_effect_cfg;
      S->base.effect_cfg = S->draft_effect_cfg;
    }
    if (ap & (1u << CCORD_PROFILE_FIELD_PROFILE_FRAME)) {
      S->base.frame_sku_id = S->draft_frame_sku;
      copy_str(S->base.frame_name, sizeof S->base.frame_name, S->draft_frame_name);
      copy_str(S->base.frame_thumb, sizeof S->base.frame_thumb, S->draft_frame_thumb);
      S->base.has_frame_cfg = S->draft_has_frame_cfg;
      S->base.frame_cfg = S->draft_frame_cfg;
    }
    if (ap & (1u << CCORD_PROFILE_FIELD_NAMEPLATE)) {
      S->base.nameplate_sku_id = S->draft_np_sku;
      copy_str(S->base.nameplate_asset, sizeof S->base.nameplate_asset, S->draft_np_asset);
      copy_str(S->base.nameplate_palette, sizeof S->base.nameplate_palette, S->draft_np_palette);
      copy_str(S->base.nameplate_name, sizeof S->base.nameplate_name, S->draft_np_name);
    }
    if (ap & (1u << CCORD_PROFILE_FIELD_CLAN_GUILD)) {
      S->base.clan_guild_id = S->draft_clan_guild;
      copy_str(S->base.clan_tag, sizeof S->base.clan_tag, S->draft_clan_tag);
      copy_str(S->base.clan_badge, sizeof S->base.clan_badge, S->draft_clan_badge);
    }
  }
  clear_errors();
  if (res->status == CCORD_OK) {
    S->saved_until_ms = ui->now_ms + SAVED_MS;
    ui->dirty = true;
    return;
  }
  bool named = false;
  for (size_t i = 0; i < res->field_error_count; i++) {
    const ccord_profile_field_error *fe = &res->field_errors[i];
    if (fe->field < CCORD_PROFILE_FIELD_COUNT) {
      copy_str(S->err[fe->field], sizeof S->err[0], fe->message[0] ? fe->message : "Discord refused this change");
      named = true;
    } else if (!S->general[0]) {
      copy_str(S->general, sizeof S->general, fe->message);
    }
  }
  if (res->password_required || res->mfa_required) {
    copy_str(S->general, sizeof S->general, "Discord wants to verify your account: use My Account or discord.com");
  } else if (res->captcha_required) {
    copy_str(S->general, sizeof S->general, "Discord asked for a captcha: make this change on discord.com");
  } else if (res->rate_limited) {
    snprintf(S->general, sizeof S->general, "Slow down: try again in %d s", (int)(res->retry_after_s + 0.5));
  } else if (!named && !S->general[0]) {
    copy_str(S->general, sizeof S->general, res->message[0] ? res->message : "Couldn't save your profile");
  }
  ui->dirty = true;
}

bool ui_settings_profile_tick(ccord_ui *ui)
{
  if (!S) return false;
  if (!ui->settings_open) {
    ui_settings_profile_release(ui);
    return false;
  }
  if (ui->settings_page != UI_SETTINGS_PAGE_PROFILE) {
    S->awaiting = -1;
    S->theme_hex_focus = -1;
    sp_picker_close(ui, S);
    if (ui->account_focus_field >= UI_PROFILE_FOCUS_BASE && ui->account_focus_field < UI_PROFILE_FOCUS_BASE + F_TEXT_COUNT) {
      ui->account_focus_field = 0;
    }
    return false;
  }
  if (S->cb_done) apply_result(ui);
  if (S->saved_until_ms && ui->now_ms >= S->saved_until_ms) {
    S->saved_until_ms = 0;
    ui->dirty = true;
  }
  ccord_fetch_state cs = ui->d.session ? ccord_session_collectibles_state(ui->d.session) : CCORD_FETCH_IDLE;
  if (cs != S->last_col_state) {
    S->last_col_state = cs;
    ui->dirty = true;
  }
  return S->pending || S->saved_until_ms != 0 || cs == CCORD_FETCH_LOADING;
}

// ---------------------------------------------------------------- input

static void focus_field(ccord_ui *ui, int f)
{
  S->theme_hex_focus = -1;
  ui->account_focus_field = f < 0 ? 0 : UI_PROFILE_FOCUS_BASE + f;
  if (f >= 0) {
    ccord_text_edit_move(&S->ed[f], 2, false);
    ui->caret_on = true;
    ui->caret_epoch_ms = ui->now_ms;
    if (ui->touch_mode) ui->touch_text_focused = true;
  }
  ui->dirty = true;
}

static void focus_theme_hex(ccord_ui *ui, int tf)
{
  ui->account_focus_field = 0;
  S->theme_hex_focus = tf;
  if (tf >= 0 && tf < F_THEME_HEX_COUNT) {
    ccord_text_edit_move(&S->theme_ed[tf], 2, false);
    ui->caret_on = true;
    ui->caret_epoch_ms = ui->now_ms;
    if (ui->touch_mode) ui->touch_text_focused = true;
  }
  ui->dirty = true;
}

static void edited_theme_hex(ccord_ui *ui, int tf)
{
  if (tf < 0 || tf >= F_THEME_HEX_COUNT) return;
  const char *t = ccord_text_edit_cstr(&S->theme_ed[tf]);
  int32_t v = 0;
  if (parse_hex(t, &v)) {
    if (!S->draft_has_theme) {
      S->draft_has_theme = true;
      S->draft_theme_primary = (tf == F_THEME_PRI) ? (uint32_t)v : 0x1E3A8Au;
      S->draft_theme_accent = (tf == F_THEME_ACC) ? (uint32_t)v : 0x3B82F6u;
      int other = (tf == F_THEME_PRI) ? F_THEME_ACC : F_THEME_PRI;
      if (ccord_text_edit_cstr(&S->theme_ed[other])[0] == '\0') {
        char hex[16];
        snprintf(hex, sizeof hex, "%06X", (unsigned)(other == F_THEME_PRI ? S->draft_theme_primary : S->draft_theme_accent));
        ed_set(&S->theme_ed[other], hex);
      }
    } else if (tf == F_THEME_PRI) {
      S->draft_theme_primary = (uint32_t)v;
    } else {
      S->draft_theme_accent = (uint32_t)v;
    }
  }
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
  ui->dirty = true;
}

static void insert_theme_hex(ccord_ui *ui, int tf, const char *s, size_t len)
{
  if (tf < 0 || tf >= F_THEME_HEX_COUNT) return;
  ccord_text_edit *t = &S->theme_ed[tf];
  char buf[32];
  size_t o = 0;
  size_t sel = t->caret > t->anchor ? t->caret - t->anchor : t->anchor - t->caret;
  size_t used = t->len > sel ? t->len - sel : 0;
  size_t room = 7u > used ? 7u - used : 0;
  for (size_t i = 0; i < len && o < sizeof buf - 1u && room > 0; i++) {
    if (strchr("0123456789abcdefABCDEF#", s[i])) {
      buf[o++] = s[i];
      room--;
    }
  }
  if (o == 0) return;
  ccord_error e;
  if (ccord_text_edit_insert(t, buf, o, &e) == CCORD_OK) edited_theme_hex(ui, tf);
}

static void switch_context(ccord_ui *ui, int new_subtab, ccord_id new_guild)
{
  if (S->pending) return;
  S->subtab = new_subtab;
  S->selected_guild = new_guild;
  S->guild_dropdown_open = false;
  S->scroll = 0.0f;
  clear_errors();
  request_live_profile_data(ui, active_guild_id(S));
  read_snap(ui, active_guild_id(S), &S->base);
  load_all();
  ui->dirty = true;
}

bool ui_settings_profile_click(ccord_ui *ui, ccord_rect content, ccord_rect panel, float x, float y)
{
  (void)content;
  if (!ensure_live(ui)) return false;
  if (S->picker.kind != PROF_PICK_NONE) {
    return sp_picker_click(ui, S, x, y);
  }
  if (!ui_in_rect(x, y, panel)) return false;
  bool in_bar = S->bar_visible && ui_in_rect(x, y, S->bar);
  bool in_view = ui_in_rect(x, y, S->view) && !in_bar;
  for (size_t i = S->nitems; i-- > 0;) {
    const ui_profile_item *it = &S->items[i];
    bool bar_item = it->kind == UI_PROF_SAVE || it->kind == UI_PROF_RESET;
    if (!ui_in_rect(x, y, it->rect) || !(bar_item ? in_bar : in_view)) continue;
    if (it->kind != UI_PROF_GUILD_TOGGLE && it->kind != UI_PROF_GUILD_PICK) S->guild_dropdown_open = false;
    switch (it->kind) {
      case UI_PROF_NAME:
      case UI_PROF_PRONOUNS:
      case UI_PROF_BIO:
      case UI_PROF_ACCENT_HEX:
        focus_field(ui, (int)it->kind);
        if (it->kind == UI_PROF_BIO) {
          S->click_pending = true;
          S->click_x = x;
          S->click_y = y;
        }
        return true;
      case UI_PROF_AVATAR_CHANGE: focus_field(ui, -1); pick_image(ui, IMG_AVATAR); return true;
      case UI_PROF_BANNER_CHANGE: focus_field(ui, -1); pick_image(ui, IMG_BANNER); return true;
      case UI_PROF_AVATAR_REMOVE:
      case UI_PROF_BANNER_REMOVE: {
        int slot = it->kind == UI_PROF_BANNER_REMOVE ? IMG_BANNER : IMG_AVATAR;
        const char *saved = slot == IMG_BANNER ? S->base.banner_url : S->base.avatar_url;
        img_free(&S->img[slot]);
        S->img[slot].mode = saved[0] ? IMG_REMOVE : IMG_KEEP;
        S->err[slot == IMG_BANNER ? CCORD_PROFILE_FIELD_BANNER : CCORD_PROFILE_FIELD_AVATAR][0] = '\0';
        focus_field(ui, -1);
        return true;
      }
      case UI_PROF_IMG_CROP:
        focus_field(ui, -1);
        sp_picker_open(ui, S, PROF_PICK_CROP, it->arg);
        return true;
      case UI_PROF_ACCENT_SWATCH:
        if (it->arg >= 0 && it->arg < ACCENT_PRESETS) S->accent = (int32_t)k_presets[it->arg];
        set_hex_text();
        S->err[CCORD_PROFILE_FIELD_ACCENT][0] = '\0';
        focus_field(ui, -1);
        return true;
      case UI_PROF_ACCENT_DEFAULT:
        S->accent = -1;
        set_hex_text();
        S->err[CCORD_PROFILE_FIELD_ACCENT][0] = '\0';
        focus_field(ui, -1);
        return true;
      case UI_PROF_SUBTAB:
        focus_field(ui, -1);
        if (it->arg != S->subtab) {
          ccord_id gid = (it->arg == 1) ? (S->selected_guild != CCORD_ID_NONE ? S->selected_guild : first_joined_guild(ui))
                                        : CCORD_ID_NONE;
          switch_context(ui, it->arg == 1 ? 1 : 0, gid != CCORD_ID_NONE ? gid : S->selected_guild);
        }
        return true;
      case UI_PROF_GUILD_TOGGLE:
        focus_field(ui, -1);
        S->guild_dropdown_open = !S->guild_dropdown_open;
        ui->dirty = true;
        return true;
      case UI_PROF_GUILD_PICK: {
        focus_field(ui, -1);
        if (ui->d.store) {
          size_t ng = ccord_store_guild_count(ui->d.store);
          int idx = 0;
          for (size_t gi = 0; gi < ng; gi++) {
            const ccord_guild *g = ccord_store_guild_at(ui->d.store, gi);
            if (!g || g->id == CCORD_ID_NONE || g->id == CCORD_DM_GUILD_ID) continue;
            if (idx == it->arg) {
              switch_context(ui, 1, g->id);
              break;
            }
            idx++;
          }
        }
        return true;
      }
      case UI_PROF_GUILD_RESET:
        focus_field(ui, -1);
        if (S->subtab == 1 && !S->pending) {
          ed_set(&S->ed[F_NAME], "");
          ed_set(&S->ed[F_PRONOUNS], "");
          ed_set(&S->ed[F_BIO], "");
          S->accent = -1;
          set_hex_text();
          S->draft_has_theme = false;
          S->draft_theme_primary = 0;
          S->draft_theme_accent = 0;
          set_theme_hex_texts();
          S->draft_deco_sku = CCORD_ID_NONE;
          S->draft_deco_asset[0] = S->draft_deco_url[0] = S->draft_deco_name[0] = '\0';
          S->draft_effect_id[0] = S->draft_effect_name[0] = S->draft_effect_thumb[0] = '\0';
          S->draft_has_effect_cfg = false;
          memset(&S->draft_effect_cfg, 0, sizeof S->draft_effect_cfg);
          S->draft_frame_sku = CCORD_ID_NONE;
          S->draft_frame_name[0] = S->draft_frame_thumb[0] = '\0';
          S->draft_has_frame_cfg = false;
          memset(&S->draft_frame_cfg, 0, sizeof S->draft_frame_cfg);
          S->draft_np_sku = CCORD_ID_NONE;
          S->draft_np_asset[0] = S->draft_np_palette[0] = S->draft_np_name[0] = '\0';
          if (S->base.avatar_url[0]) { img_free(&S->img[IMG_AVATAR]); S->img[IMG_AVATAR].mode = IMG_REMOVE; }
          if (S->base.banner_url[0]) { img_free(&S->img[IMG_BANNER]); S->img[IMG_BANNER].mode = IMG_REMOVE; }
          ui->dirty = true;
        }
        return true;
      case UI_PROF_BIO_FMT:
        focus_field(ui, F_BIO);
        apply_bio_format(ui, it->arg);
        return true;
      case UI_PROF_COLOR_PICK:
        focus_field(ui, -1);
        sp_picker_open(ui, S, PROF_PICK_COLOR, it->arg);
        return true;
      case UI_PROF_THEME_PRI_HEX:
        focus_theme_hex(ui, F_THEME_PRI);
        return true;
      case UI_PROF_THEME_ACC_HEX:
        focus_theme_hex(ui, F_THEME_ACC);
        return true;
      case UI_PROF_THEME_CLEAR:
        focus_field(ui, -1);
        S->draft_has_theme = false;
        S->draft_theme_primary = 0;
        S->draft_theme_accent = 0;
        set_theme_hex_texts();
        ui->dirty = true;
        return true;
      case UI_PROF_COSMETIC_OPEN:
        focus_field(ui, -1);
        if ((it->arg >= PROF_PICK_DECO && it->arg <= PROF_PICK_CLAN) || it->arg == PROF_PICK_FRAME) {
          sp_picker_open(ui, S, (prof_picker_kind)it->arg, 0);
        }
        return true;
      case UI_PROF_LINK_ACCOUNT:
        focus_field(ui, -1);
        ui->settings_page = SP_ACCOUNT;
        ui->settings_category = st_k_pages[SP_ACCOUNT].category;
        st_s_page_scroll = 0.0f;
        ui->dirty = true;
        return true;
      case UI_PROF_LINK_PRIVACY:
        focus_field(ui, -1);
        ui->settings_page = SP_PRIVACY;
        ui->settings_category = st_k_pages[SP_PRIVACY].category;
        st_s_page_scroll = 0.0f;
        ui->dirty = true;
        return true;
      case UI_PROF_SAVE:
        focus_field(ui, -1);
        do_save(ui);
        return true;
      case UI_PROF_RESET:
        if (!S->pending) {
          load_all();
          clear_errors();
        }
        focus_field(ui, -1);
        return true;
      case UI_PROF_WEB:
        (void)ui_open_url(ui, UI_PROFILE_WEB_URL, "Discord");
        ui->dirty = true;
        return true;
    }
  }
  S->guild_dropdown_open = false;
  if (in_view || in_bar) focus_field(ui, -1);
  return true;
}

bool ui_settings_profile_mouse_move(ccord_ui *ui, float x, float y)
{
  if (!S || S->picker.kind == PROF_PICK_NONE) return false;
  return sp_picker_mouse_move(ui, S, x, y);
}

void ui_settings_profile_mouse_up(ccord_ui *ui)
{
  if (!S || S->picker.kind == PROF_PICK_NONE) return;
  sp_picker_mouse_up(ui, S);
}

static int focused_field(const ccord_ui *ui)
{
  int f = ui->account_focus_field - UI_PROFILE_FOCUS_BASE;
  return f >= 0 && f < F_TEXT_COUNT ? f : -1;
}

bool ui_settings_profile_key(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!S || ui->settings_page != UI_SETTINGS_PAGE_PROFILE) return false;
  if (S->picker.kind != PROF_PICK_NONE) {
    return sp_picker_key(ui, S, ev);
  }
  if (S->theme_hex_focus >= 0 && S->theme_hex_focus < F_THEME_HEX_COUNT) {
    int tf = S->theme_hex_focus;
    ccord_text_edit *t = &S->theme_ed[tf];
    if (ev->type == CCORD_IN_TEXT) {
      if (ev->text[0]) insert_theme_hex(ui, tf, ev->text, strlen(ev->text));
      return true;
    }
    if (ev->type != CCORD_IN_KEY_DOWN) return true;
    bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0, shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
    switch (ev->key) {
      case CCORD_KEY_BACKSPACE:
        if (ctrl) ccord_text_edit_backspace_word(t);
        else ccord_text_edit_backspace(t);
        edited_theme_hex(ui, tf);
        return true;
      case CCORD_KEY_DELETE:
        if (ctrl) ccord_text_edit_delete_word(t);
        else ccord_text_edit_delete(t);
        edited_theme_hex(ui, tf);
        return true;
      case CCORD_KEY_LEFT: ccord_text_edit_move(t, ctrl ? -3 : -1, shift); break;
      case CCORD_KEY_RIGHT: ccord_text_edit_move(t, ctrl ? 3 : 1, shift); break;
      case CCORD_KEY_HOME: ccord_text_edit_move(t, -2, shift); break;
      case CCORD_KEY_END: ccord_text_edit_move(t, 2, shift); break;
      case CCORD_KEY_ESCAPE:
      case CCORD_KEY_RETURN:
        focus_field(ui, -1);
        return true;
      case CCORD_KEY_TAB:
        focus_theme_hex(ui, tf == F_THEME_PRI ? F_THEME_ACC : F_THEME_PRI);
        return true;
      case CCORD_KEY_A:
        if (ctrl) ccord_text_edit_select_all(t);
        break;
      case CCORD_KEY_Z:
        if (ctrl) {
          if (shift) (void)ccord_text_edit_redo(t);
          else (void)ccord_text_edit_undo(t);
          edited_theme_hex(ui, tf);
        }
        break;
      case CCORD_KEY_C:
      case CCORD_KEY_X:
        if (ctrl && ui->d.host.clipboard_set) {
          char *sel = ccord_text_edit_copy_selection(t);
          if (sel) {
            ui->d.host.clipboard_set(ui->d.host.ud, sel);
            ccord_free(sel);
            if (ev->key == CCORD_KEY_X) {
              ccord_text_edit_backspace(t);
              edited_theme_hex(ui, tf);
            }
          }
        }
        break;
      case CCORD_KEY_V:
        if (ctrl && ui->d.host.clipboard_get) {
          char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
          if (clip) {
            insert_theme_hex(ui, tf, clip, strlen(clip));
            ccord_free(clip);
          }
        }
        break;
      default: break;
    }
    ui->caret_on = true;
    ui->caret_epoch_ms = ui->now_ms;
    ui->dirty = true;
    return true;
  }
  int f = focused_field(ui);
  if (f < 0) return false;
  ccord_text_edit *t = &S->ed[f];
  if (ev->type == CCORD_IN_TEXT) {
    if (ev->text[0]) insert_capped(ui, f, ev->text, strlen(ev->text));
    return true;
  }
  if (ev->type != CCORD_IN_KEY_DOWN) return true;
  bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0, shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
  switch (ev->key) {
    case CCORD_KEY_BACKSPACE:
      if (ctrl) ccord_text_edit_backspace_word(t);
      else ccord_text_edit_backspace(t);
      edited(ui, f);
      return true;
    case CCORD_KEY_DELETE:
      if (ctrl) ccord_text_edit_delete_word(t);
      else ccord_text_edit_delete(t);
      edited(ui, f);
      return true;
    case CCORD_KEY_LEFT: ccord_text_edit_move(t, ctrl ? -3 : -1, shift); break;
    case CCORD_KEY_RIGHT: ccord_text_edit_move(t, ctrl ? 3 : 1, shift); break;
    case CCORD_KEY_HOME: ccord_text_edit_move(t, ctrl ? -2 : -4, shift); break;
    case CCORD_KEY_END: ccord_text_edit_move(t, ctrl ? 2 : 4, shift); break;
    case CCORD_KEY_RETURN:
      if (f == F_BIO) insert_capped(ui, f, "\n", 1);
      else focus_field(ui, f == F_HEX ? -1 : f + 1);
      return true;
    case CCORD_KEY_TAB: focus_field(ui, shift ? (f + F_TEXT_COUNT - 1) % F_TEXT_COUNT : (f + 1) % F_TEXT_COUNT); return true;
    case CCORD_KEY_A:
      if (ctrl) ccord_text_edit_select_all(t);
      break;
    case CCORD_KEY_Z:
      if (ctrl) {
        if (shift) (void)ccord_text_edit_redo(t);
        else (void)ccord_text_edit_undo(t);
        edited(ui, f);
      }
      break;
    case CCORD_KEY_Y:
      if (ctrl) {
        (void)ccord_text_edit_redo(t);
        edited(ui, f);
      }
      break;
    case CCORD_KEY_C:
    case CCORD_KEY_X:
      if (ctrl && ui->d.host.clipboard_set) {
        char *sel = ccord_text_edit_copy_selection(t);
        if (sel) {
          ui->d.host.clipboard_set(ui->d.host.ud, sel);
          ccord_free(sel);
          if (ev->key == CCORD_KEY_X) {
            ccord_text_edit_backspace(t);
            edited(ui, f);
          }
        }
      }
      break;
    case CCORD_KEY_V:
      if (ctrl && ui->d.host.clipboard_get) {
        char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
        if (clip) {
          insert_capped(ui, f, clip, strlen(clip));
          ccord_free(clip);
        }
      }
      break;
    default: break;
  }
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
  ui->dirty = true;
  return true;
}

bool ui_settings_profile_wheel(ccord_ui *ui, float wheel)
{
  if (!S) return false;
  if (S->picker.kind != PROF_PICK_NONE) {
    return sp_picker_wheel(ui, S, wheel);
  }
  float max = S->content_h - S->view_h;
  if (max <= 0.0f) return false;
  float old = S->scroll;
  S->scroll -= wheel * WHEEL_STEP;
  if (S->scroll < 0.0f) S->scroll = 0.0f;
  if (S->scroll > max) S->scroll = max;
  ui->dirty = true;
  return S->scroll != old;
}

// ---------------------------------------------------------------- drawing

typedef struct dctx {
  ccord_ui *ui;
  ccord_renderer *r;
  uis_tokens tk;
  float x, w;
  float top;
  float y;
} dctx;

static void add_item(ui_profile_kind kind, int arg, ccord_rect rc, const char *label)
{
  if (S->nitems >= UI_PROFILE_ITEMS_MAX) return;
  ui_profile_item *it = &S->items[S->nitems++];
  it->kind = kind;
  it->arg = arg;
  it->rect = rc;
  copy_str(it->label, sizeof it->label, label);
}

static bool hovered(const ccord_ui *ui, ccord_rect rc, bool in_bar)
{
  if (S && S->picker.kind != PROF_PICK_NONE) return false;
  if (!ui_in_rect(ui->mx, ui->my, rc)) return false;
  if (in_bar) return true;
  return ui_in_rect(ui->mx, ui->my, S->view) && !(S->bar_visible && ui_in_rect(ui->mx, ui->my, S->bar));
}

static void section_label(dctx *c, const char *label, const char *right, ccord_color right_col)
{
  ccord_ui *ui = c->ui;
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, c->tk.fs_overline, COL(text_muted));
  ccord_text_draw(c->r, label, strlen(label), c->x, c->top + c->y, &ls, 0.0f);
  if (right) {
    ccord_text_style rs = ui_style(ui, CCORD_FONT_BOLD, c->tk.fs_overline, right_col);
    ccord_vec2 m = ccord_text_measure(c->r, right, strlen(right), &rs, 0.0f);
    ccord_text_draw(c->r, right, strlen(right), c->x + c->w - m.x, c->top + c->y, &rs, 0.0f);
  }
  c->y += ui_line_h(ui, ls.size) + UIS_SP_2;
}

static void field_error(dctx *c, int field)
{
  if (!S->err[field][0]) return;
  ccord_text_style es = ui_style(c->ui, CCORD_FONT_REGULAR, c->tk.fs_caption, c->ui->theme->c.danger);
  float h = ccord_text_measure(c->r, S->err[field], strlen(S->err[field]), &es, c->w).y;
  ccord_text_draw(c->r, S->err[field], strlen(S->err[field]), c->x, c->top + c->y + UIS_SP_1, &es, c->w);
  S->err_rect[field] = (ccord_rect){c->x, c->top + c->y + UIS_SP_1, c->w, h};
  c->y += h + UIS_SP_1;
}

static void caption(dctx *c, const char *text)
{
  ccord_text_style cs = ui_style(c->ui, CCORD_FONT_REGULAR, c->tk.fs_caption, c->ui->theme->c.text_muted);
  float h = ccord_text_measure(c->r, text, strlen(text), &cs, c->w).y;
  ccord_text_draw(c->r, text, strlen(text), c->x, c->top + c->y + UIS_SP_1, &cs, c->w);
  c->y += h + UIS_SP_1;
}

static void text_input(dctx *c, int f, ui_profile_kind kind, const char *placeholder, float w, float pad_l)
{
  ccord_ui *ui = c->ui;
  ccord_rect box = {c->x, c->top + c->y, w, UIS_H_INPUT};
  uis_state st = {.hover = hovered(ui, box, false), .focused = focused_field(ui) == f};
  (void)uis_input_padded(ui, c->r, box, pad_l, UIS_SP_3, &S->ed[f], placeholder, st, false, "pfld", (uint64_t)f);
  add_item(kind, f, box, placeholder);
  c->y += UIS_H_INPUT;
}

static void bio_toolbar(dctx *c)
{
  ccord_ui *ui = c->ui;
  static const char *const k_glyphs[5] = {"B", "I", "</>", "||", "Emoji"};
  static const char *const k_labels[5] = {"Bold", "Italic", "Monospace", "Spoiler", "Emoji"};
  float bx = c->x;
  float by = c->top + c->y;
  float bh = uis_btn_h_for(ui, UIS_H_BTN_SM);
  uis_btn_size bsz = ui->mobile_layout ? UIS_BTN_S_MD : UIS_BTN_S_SM;
  for (int i = 0; i < 5; i++) {
    float bw = ceilf(uis_label_width(ui, c->r, k_glyphs[i])) + 4.0f;
    if (bw < 38.0f) bw = 38.0f;
    if (bx > c->x && bx + bw > c->x + c->w) {
      bx = c->x;
      by += bh + UIS_SP_1;
      c->y += bh + UIS_SP_1;
    }
    ccord_rect rc = {bx, by, bw, bh};
    uis_button(ui, c->r, rc, k_glyphs[i], UIS_BTN_SECONDARY, bsz,
               (uis_state){.hover = hovered(ui, rc, false)}, "pbfmt", (uint64_t)i);
    add_item(UI_PROF_BIO_FMT, i, rc, k_labels[i]);
    bx += bw + UIS_SP_1;
  }
  c->y += bh + UIS_SP_2;
}

static void bio_box(dctx *c)
{
  ccord_ui *ui = c->ui;
  ccord_renderer *r = c->r;
  ccord_text_edit *ed = &S->ed[F_BIO];
  ccord_rect box = {c->x, c->top + c->y, c->w, BIO_BOX_H};
  bool focused = focused_field(ui) == F_BIO;
  float ring = ui_fade(ui, "pfld", F_BIO, focused);
  if (ring > 0.004f) {
    ccord_draw_rrect(r, (ccord_rect){box.x - 1.0f, box.y - 1.0f, box.w + 2.0f, box.h + 2.0f}, c->tk.r_control + 1.0f,
                     ui_alpha(COL(brand), ring));
  }
  uis_plate(ui, r, box, c->tk.r_control, COL(bg_input));
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, c->tk.fs_body, COL(text_normal));
  float lh = ui_line_h(ui, ts.size), inner_w = box.w - 2.0f * UIS_SP_3;
  const char *t = ed->data ? ed->data : "";
  ccord_rect cr = ccord_text_caret_rect(r, t, ed->len, &ts, inner_w, ed->caret);
  float view_h = box.h - 2.0f * UIS_SP_2, shift = 0.0f;
  if (focused && cr.y + lh > view_h) shift = cr.y + lh - view_h;
  float tx = box.x + UIS_SP_3, ty = box.y + UIS_SP_2 - shift;
  if (S->click_pending && ui_in_rect(S->click_x, S->click_y, box)) {
    size_t pos = ccord_text_hit_test(r, t, ed->len, &ts, inner_w, S->click_x - tx, S->click_y - ty);
    ccord_text_edit_set_caret(ed, pos, false);
    ui->caret_on = true;
    ui->caret_epoch_ms = ui->now_ms;
    S->click_pending = false;
    cr = ccord_text_caret_rect(r, t, ed->len, &ts, inner_w, ed->caret);
  }
  ccord_push_clip(r, (ccord_rect){box.x, box.y + 1.0f, box.w, box.h - 2.0f});
  if (ed->len == 0) {
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, c->tk.fs_body, COL(text_placeholder));
    ccord_text_draw(r, "Tell people about yourself", 26, tx, ty, &ps, inner_w);
  } else {
    if (focused && ed->anchor != ed->caret) {
      size_t lo = ed->anchor < ed->caret ? ed->anchor : ed->caret, hi = ed->anchor < ed->caret ? ed->caret : ed->anchor;
      ccord_rect a = ccord_text_caret_rect(r, t, ed->len, &ts, inner_w, lo);
      ccord_rect b = ccord_text_caret_rect(r, t, ed->len, &ts, inner_w, hi);
      if (b.y - a.y < 1.0f) {
        ccord_draw_rect(r, (ccord_rect){tx + a.x, ty + a.y, b.x - a.x, lh}, COL(selection));
      } else {
        ccord_draw_rect(r, (ccord_rect){tx + a.x, ty + a.y, inner_w - a.x, lh}, COL(selection));
        for (float yy = a.y + lh; yy < b.y - 1.0f; yy += lh) ccord_draw_rect(r, (ccord_rect){tx, ty + yy, inner_w, lh}, COL(selection));
        ccord_draw_rect(r, (ccord_rect){tx, ty + b.y, b.x, lh}, COL(selection));
      }
    }
    ccord_text_draw(r, t, ed->len, tx, ty, &ts, inner_w);
  }
  if (focused && ui->caret_on) ccord_draw_rect(r, (ccord_rect){tx + cr.x, ty + cr.y, 1.5f, lh}, COL(caret));
  ccord_pop_clip(r);
  if (focused) ui->text_area = box;
  add_item(UI_PROF_BIO, F_BIO, box, "About Me");
  c->y += BIO_BOX_H;
}

bool sp_ensure_img(ccord_ui *ui, ccord_renderer *r, prof_img_draft *d)
{
  if (d->mode != IMG_NEW || !d->data) return false;
  if (d->uploaded && ccord_render_has_texture(r, d->key, nullptr, nullptr)) return true;
  ccord_error e;
  int w, h;
  if (ccord_render_upload_image(r, d->key, d->data, d->len, &w, &h, &e) == CCORD_OK) {
    d->uploaded = true;
    return true;
  }
  ui_notice(ui, "Couldn't read that image");
  img_free(d);
  return false;
}

static void image_row(dctx *c, int slot, const char *heading, const char *change, int field)
{
  ccord_ui *ui = c->ui;
  section_label(c, heading, nullptr, COL(text_muted));
  float bh = uis_button_height(UIS_BTN_S_MD);
  float cw = uis_label_width(ui, c->r, change);
  ccord_rect crc = {c->x, c->top + c->y, cw, bh};
  uis_state cst = {.hover = hovered(ui, crc, false), .selected = S->awaiting == slot};
  uis_button(ui, c->r, crc, change, UIS_BTN_PRIMARY, UIS_BTN_S_MD, cst, "pbtn", (uint64_t)slot * 4u);
  add_item(slot == IMG_BANNER ? UI_PROF_BANNER_CHANGE : UI_PROF_AVATAR_CHANGE, 0, crc, change);
  const char *saved = slot == IMG_BANNER ? S->base.banner_url : S->base.avatar_url;
  prof_img_draft *d = &S->img[slot];
  float nx = crc.x + cw + UIS_SP_2;
  if (d->mode == IMG_NEW || (d->mode == IMG_KEEP && saved[0])) {
    const char *rm = slot == IMG_BANNER ? "Remove Banner" : "Remove Avatar";
    float rw = uis_label_width(ui, c->r, rm);
    ccord_rect rrc = {nx, crc.y, rw, bh};
    uis_state rst = {.hover = hovered(ui, rrc, false)};
    uis_button(ui, c->r, rrc, rm, UIS_BTN_SECONDARY, UIS_BTN_S_MD, rst, "pbtn", (uint64_t)slot * 4u + 1u);
    add_item(slot == IMG_BANNER ? UI_PROF_BANNER_REMOVE : UI_PROF_AVATAR_REMOVE, 0, rrc, rm);
    nx = rrc.x + rw + UIS_SP_2;
  }
  if (d->mode == IMG_NEW && nx + 98.0f <= c->x + c->w) {
    ccord_rect prc = {nx, crc.y, 98.0f, bh};
    uis_button(ui, c->r, prc, "Adjust Crop", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
               (uis_state){.hover = hovered(ui, prc, false)}, "pbtn", (uint64_t)slot * 4u + 2u);
    add_item(UI_PROF_IMG_CROP, slot, prc, "Adjust Crop");
  }
  c->y += bh;
  if (d->mode == IMG_NEW) caption(c, "New image chosen. Save Changes to apply it.");
  else if (d->mode == IMG_REMOVE) caption(c, "It will be removed when you save.");
  else if (S->awaiting == slot) caption(c, "Choose an image file, or drop one onto the window.");
  field_error(c, field);
}

static void draw_vgradient_rrect(ccord_renderer *r, ccord_rect box, float rad, ccord_color top_col, ccord_color bot_col)
{
  enum { STEPS = 24 };
  float bot = box.y + box.h;
  for (int i = 0; i < STEPS; i++) {
    float t = ((float)i + 0.5f) / (float)STEPS;
    ccord_color col = ui_mix(top_col, bot_col, t);
    float sy = roundf(box.y + box.h * (float)i / (float)STEPS);
    float sy2 = (i == STEPS - 1) ? bot : roundf(box.y + box.h * (float)(i + 1) / (float)STEPS);
    if (sy2 <= sy) continue;
    ccord_rect strip = {box.x, sy, box.w, sy2 - sy};
    if (rad > 0.0f && (sy < box.y + rad || sy2 > bot - rad)) {
      ccord_push_clip(r, strip);
      ccord_draw_rrect(r, box, rad, col);
      ccord_pop_clip(r);
    } else {
      ccord_draw_rect(r, strip, col);
    }
  }
}

// Unified Discord-inspired "THEME & BANNER" section:
// - Side-by-side 96x96 Gradient tile (Primary/Accent stop squares -> HSV picker) + 96x96 live Banner tile (image or
//   active solid banner color: theme_primary when themed, accent_color otherwise) + compact Primary / Accent #HEX inputs.
// - Banner image controls (Change Banner / Remove Banner / Adjust Crop / Reset Theme) + Banner field error directly
//   underneath (preserving error_rect[BANNER] between Change Banner and the Banner Color swatches when unthemed).
// - Solid Banner Color swatches + #HEX + Default + Pick Color are only shown when no custom Profile Theme is active
//   (when a Profile Theme is active, Primary IS the solid banner color on Discord, avoiding a redundant/stale block).
static void theme_and_banner_section(dctx *c)
{
  ccord_ui *ui = c->ui;
  section_label(c, "THEME & BANNER", S->draft_has_theme ? "Custom Theme" : "Default",
                S->draft_has_theme ? COL(brand) : COL(text_muted));

  float tile_w = 96.0f, tile_h = 96.0f;
  float tile_y = c->top + c->y;

  // 1. Profile Theme 2-stop vertical gradient tile
  ccord_rect gtile = {c->x, tile_y, tile_w, tile_h};
  ccord_color top_col = S->draft_has_theme ? sp_rgb(S->draft_theme_primary) : COL(bg_tertiary);
  ccord_color bot_col = S->draft_has_theme ? sp_rgb(S->draft_theme_accent) : COL(bg_secondary);
  draw_vgradient_rrect(c->r, gtile, 10.0f, top_col, bot_col);

  ccord_rect pri_sq = {gtile.x + (gtile.w - 30.0f) * 0.5f, gtile.y + 12.0f, 30.0f, 30.0f};
  ccord_rect acc_sq = {gtile.x + (gtile.w - 30.0f) * 0.5f, gtile.y + 54.0f, 30.0f, 30.0f};
  bool pri_hov = hovered(ui, pri_sq, false);
  bool acc_hov = hovered(ui, acc_sq, false);
  ccord_color ring = {255, 255, 255, 235};
  ccord_draw_rrect(c->r, (ccord_rect){pri_sq.x - 2.0f, pri_sq.y - 2.0f, pri_sq.w + 4.0f, pri_sq.h + 4.0f}, 8.0f,
                   pri_hov ? COL(brand) : ring);
  ccord_draw_rrect(c->r, pri_sq, 6.0f, top_col);
  ccord_draw_rrect(c->r, (ccord_rect){acc_sq.x - 2.0f, acc_sq.y - 2.0f, acc_sq.w + 4.0f, acc_sq.h + 4.0f}, 8.0f,
                   acc_hov ? COL(brand) : ring);
  ccord_draw_rrect(c->r, acc_sq, 6.0f, bot_col);
  add_item(UI_PROF_COLOR_PICK, COLOR_TARGET_THEME_PRI, pri_sq, "Primary Color");
  add_item(UI_PROF_COLOR_PICK, COLOR_TARGET_THEME_ACC, acc_sq, "Accent Color");

  // 2. Live Banner tile right next to the gradient tile (shows banner image OR actual active banner color)
  ccord_rect btile = {gtile.x + tile_w + UIS_SP_3, tile_y, tile_w, tile_h};
  bool btile_hov = hovered(ui, btile, false);
  // Official Discord uses theme_primary as the solid banner fill whenever a Profile Theme is active (ignoring stale accent_color).
  ccord_color ban_col = S->draft_has_theme
                          ? sp_rgb(S->draft_theme_primary)
                          : (S->accent >= 0 ? sp_rgb((uint32_t)S->accent) : COL(bg_tertiary));
  if (btile_hov) {
    ccord_draw_rrect(c->r, (ccord_rect){btile.x - 2.0f, btile.y - 2.0f, btile.w + 4.0f, btile.h + 4.0f}, 12.0f,
                     COL(brand));
  }
  ccord_draw_rrect(c->r, btile, 10.0f, ban_col);
  prof_img_draft *bd = &S->img[IMG_BANNER];
  if (sp_ensure_img(ui, c->r, bd)) {
    ccord_draw_texture_cover(c->r, bd->key, btile, 10.0f, 255, false);
  } else if (bd->mode == IMG_KEEP && S->base.banner_url[0]) {
    char burl[256] = "";
    const ccord_user *su = sp_self_user(ui);
    ccord_user bu = su ? *su : (ccord_user){0};
    bu.banner_url = S->base.banner_url;
    if (ui_user_banner_url(&bu, burl, sizeof burl) && ui_ensure_media(ui, c->r, burl)) {
      int tw = 0, th = 0;
      if (ccord_render_has_texture(c->r, burl, &tw, &th) && tw > 2 && th > 2) {
        ccord_draw_texture_cover(c->r, burl, btile, 10.0f, 255, false);
      }
    }
  }
  if (S->draft_has_theme) {
    add_item(UI_PROF_BANNER_CHANGE, 0, btile, "Change Banner");
  } else {
    add_item(UI_PROF_COLOR_PICK, COLOR_TARGET_ACCENT, btile, "Banner Color");
  }

  // 3. Compact Primary / Accent #HEX inputs beside the tiles (wraps below on narrow widths)
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, c->tk.fs_caption, COL(text_secondary));
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, c->tk.fs_body, COL(text_normal));
  float hash_w = ccord_text_measure(c->r, "#", 1, &ts, 0.0f).x;
  float pad_l = UIS_SP_3 + hash_w + UIS_SP_1;
  float lbl_w = 54.0f;
  float hex_w = 104.0f;
  bool beside = (btile.x + btile.w + UIS_SP_4 + lbl_w + hex_w <= c->x + c->w + 2.0f);
  float rx = beside ? (btile.x + btile.w + UIS_SP_4) : c->x;
  float ry = beside ? (tile_y + (tile_h - (2.0f * UIS_H_INPUT + UIS_SP_2)) * 0.5f) : (tile_y + tile_h + UIS_SP_3);

  ccord_text_draw(c->r, "Primary", 7, rx, ry + (UIS_H_INPUT - ui_line_h(ui, ls.size)) * 0.5f, &ls, 0.0f);
  ccord_rect pbox = {rx + lbl_w, ry, hex_w, UIS_H_INPUT};
  (void)uis_input_padded(ui, c->r, pbox, pad_l, UIS_SP_2, &S->theme_ed[F_THEME_PRI], "FF005B",
                         (uis_state){.hover = hovered(ui, pbox, false), .focused = S->theme_hex_focus == F_THEME_PRI},
                         false, "pthx", 0u);
  ccord_text_draw(c->r, "#", 1, pbox.x + UIS_SP_3, ry + (UIS_H_INPUT - ui_line_h(ui, ts.size)) * 0.5f, &ts, 0.0f);
  add_item(UI_PROF_THEME_PRI_HEX, 0, pbox, "Primary Hex");

  float ry2 = ry + UIS_H_INPUT + UIS_SP_2;
  ccord_text_draw(c->r, "Accent", 6, rx, ry2 + (UIS_H_INPUT - ui_line_h(ui, ls.size)) * 0.5f, &ls, 0.0f);
  ccord_rect abox = {rx + lbl_w, ry2, hex_w, UIS_H_INPUT};
  (void)uis_input_padded(ui, c->r, abox, pad_l, UIS_SP_2, &S->theme_ed[F_THEME_ACC], "0B0A09",
                         (uis_state){.hover = hovered(ui, abox, false), .focused = S->theme_hex_focus == F_THEME_ACC},
                         false, "pthx", 1u);
  ccord_text_draw(c->r, "#", 1, abox.x + UIS_SP_3, ry2 + (UIS_H_INPUT - ui_line_h(ui, ts.size)) * 0.5f, &ts, 0.0f);
  add_item(UI_PROF_THEME_ACC_HEX, 0, abox, "Accent Hex");

  c->y += beside ? (tile_h + UIS_SP_3) : (tile_h + UIS_SP_3 + 2.0f * UIS_H_INPUT + UIS_SP_2 + UIS_SP_3);

  // 4. Banner Image action row + Banner status / error (stays directly above Banner Color swatches when unthemed)
  float bh = uis_button_height(UIS_BTN_S_MD);
  float by = c->top + c->y;
  float cw = uis_label_width(ui, c->r, "Change Banner");
  ccord_rect crc = {c->x, by, cw, bh};
  uis_state cst = {.hover = hovered(ui, crc, false), .selected = S->awaiting == IMG_BANNER};
  uis_button(ui, c->r, crc, "Change Banner", UIS_BTN_PRIMARY, UIS_BTN_S_MD, cst, "pbtn", (uint64_t)IMG_BANNER * 4u);
  add_item(UI_PROF_BANNER_CHANGE, 0, crc, "Change Banner");
  float nx = crc.x + cw + UIS_SP_2;
  if (bd->mode == IMG_NEW || (bd->mode == IMG_KEEP && S->base.banner_url[0])) {
    float rw = uis_label_width(ui, c->r, "Remove Banner");
    if (nx + rw > c->x + c->w + 2.0f) {
      c->y += bh + UIS_SP_2;
      by = c->top + c->y;
      nx = c->x;
    }
    ccord_rect rrc = {nx, by, rw, bh};
    uis_button(ui, c->r, rrc, "Remove Banner", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
               (uis_state){.hover = hovered(ui, rrc, false)}, "pbtn", (uint64_t)IMG_BANNER * 4u + 1u);
    add_item(UI_PROF_BANNER_REMOVE, 0, rrc, "Remove Banner");
    nx = rrc.x + rw + UIS_SP_2;
  }
  if (bd->mode == IMG_NEW) {
    float crw = fmaxf(ceilf(uis_label_width(ui, c->r, "Adjust Crop")), 96.0f);
    if (nx + crw > c->x + c->w + 2.0f) {
      c->y += bh + UIS_SP_2;
      by = c->top + c->y;
      nx = c->x;
    }
    ccord_rect prc = {nx, by, crw, bh};
    uis_button(ui, c->r, prc, "Adjust Crop", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
               (uis_state){.hover = hovered(ui, prc, false)}, "pbtn", (uint64_t)IMG_BANNER * 4u + 2u);
    add_item(UI_PROF_IMG_CROP, IMG_BANNER, prc, "Adjust Crop");
    nx = prc.x + crw + UIS_SP_2;
  }
  if (S->draft_has_theme) {
    float rtw = ceilf(uis_label_width(ui, c->r, "Reset Theme")) + 2.0f;
    if (nx + rtw > c->x + c->w + 2.0f) {
      c->y += bh + UIS_SP_2;
      by = c->top + c->y;
      nx = c->x;
    }
    ccord_rect cl = {nx, by, rtw, bh};
    uis_button(ui, c->r, cl, "Reset Theme", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
               (uis_state){.hover = hovered(ui, cl, false)}, "ptclr", 1);
    add_item(UI_PROF_THEME_CLEAR, 0, cl, "Reset Theme");
  }
  c->y += bh;
  if (bd->mode == IMG_NEW) caption(c, "New banner chosen. Save Changes to apply it.");
  else if (bd->mode == IMG_REMOVE) caption(c, "Your banner image will be removed when you save.");
  else if (S->awaiting == IMG_BANNER) caption(c, "Choose an image file, or drop one onto the window.");
  field_error(c, CCORD_PROFILE_FIELD_BANNER);

  // 5. Solid Banner Color controls: only shown when no custom 2-stop Profile Theme is active (Discord ignores
  //    accent_color whenever theme_colors is set, using Primary as the solid banner fill).
  if (!S->draft_has_theme) {
    c->y += UIS_SP_4;
    section_label(c, "BANNER COLOR", nullptr, COL(text_muted));
    for (int i = 0; i < ACCENT_PRESETS; i++) {
      ccord_rect rc = {c->x + (float)i * (UIS_SWATCH_D + UIS_SP_2), c->top + c->y, UIS_SWATCH_D, UIS_SWATCH_D};
      uis_state st = {.hover = hovered(ui, rc, false), .selected = S->accent == (int32_t)k_presets[i]};
      uis_swatch(ui, c->r, rc, sp_rgb(k_presets[i]), st, "pswatch", (uint64_t)i);
      add_item(UI_PROF_ACCENT_SWATCH, i, rc, "Banner color");
    }
    c->y += UIS_SWATCH_D + UIS_SP_2;
    float row_y = c->top + c->y;
    text_input(c, F_HEX, UI_PROF_ACCENT_HEX, "3B82F6", 120.0f, UIS_SP_3 + hash_w + UIS_SP_1);
    ccord_text_draw(c->r, "#", 1, c->x + UIS_SP_3, row_y + (UIS_H_INPUT - ui_line_h(ui, ts.size)) * 0.5f, &ts, 0.0f);
    float dw = uis_label_width(ui, c->r, "Default");
    ccord_rect drc = {c->x + 120.0f + UIS_SP_2, row_y + (UIS_H_INPUT - bh) * 0.5f, dw, bh};
    uis_button(ui, c->r, drc, "Default", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
               (uis_state){.hover = hovered(ui, drc, false)}, "pbtn", 4u);
    add_item(UI_PROF_ACCENT_DEFAULT, 0, drc, "Default");
    float pcw = ceilf(uis_label_width(ui, c->r, "Pick Color")) + 2.0f;
    if (drc.x + dw + UIS_SP_2 + pcw <= c->x + c->w + 2.0f) {
      ccord_rect pcrc = {drc.x + dw + UIS_SP_2, drc.y, pcw, bh};
      uis_button(ui, c->r, pcrc, "Pick Color", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
                 (uis_state){.hover = hovered(ui, pcrc, false)}, "pbtn", 8u);
      add_item(UI_PROF_COLOR_PICK, COLOR_TARGET_ACCENT, pcrc, "Pick Color");
    }
    field_error(c, CCORD_PROFILE_FIELD_ACCENT);
  }
}

static void cosmetic_launcher_card(dctx *c, int pick_kind, const char *title, const char *equipped, bool has_equipped)
{
  ccord_ui *ui = c->ui;
  float card_h = ui->mobile_layout ? 64.0f : 56.0f;
  ccord_rect card = {c->x, c->top + c->y, c->w, card_h};
  uis_card(ui, c->r, card);

  // Real miniature Discord CDN preview thumbnail on left (visual-only, zero text overlay)
  ccord_rect thumb = {card.x + 12.0f, card.y + (card_h - 36.0f) * 0.5f, 36.0f, 36.0f};
  uis_plate(ui, c->r, thumb, c->tk.r_control, COL(bg_tertiary));
  if (has_equipped) {
    if (pick_kind == PROF_PICK_DECO) {
      float cx = thumb.x + 18.0f, cy = thumb.y + 18.0f;
      const char *dref = S->draft_deco_url[0] ? S->draft_deco_url : S->draft_deco_asset;
      sp_draw_avatar_deco_cdn(ui, c->r, dref, cx, cy, 26.0f, !ui->anim.reduced_motion);
    } else if (pick_kind == PROF_PICK_EFFECT) {
      const char *et = S->draft_effect_thumb[0]
                           ? S->draft_effect_thumb
                           : (S->draft_has_effect_cfg && S->draft_effect_cfg.thumbnail_url[0]
                                  ? S->draft_effect_cfg.thumbnail_url
                                  : (S->draft_has_effect_cfg ? S->draft_effect_cfg.static_frame_url : ""));
      if (et && et[0] && ui_ensure_media(ui, c->r, et)) {
        int tw = 0, th = 0;
        if (ccord_render_has_texture(c->r, et, &tw, &th) && tw > 2 && th > 2) {
          ccord_draw_texture_cover(c->r, et, thumb, c->tk.r_control, 255, !ui->anim.reduced_motion);
        }
      }
    } else if (pick_kind == PROF_PICK_FRAME) {
      const char *ft = S->draft_frame_thumb[0]
                           ? S->draft_frame_thumb
                           : (S->draft_has_frame_cfg && S->draft_frame_cfg.animated_url[0]
                                  ? S->draft_frame_cfg.animated_url
                                  : (S->draft_has_frame_cfg && S->draft_frame_cfg.static_url[0]
                                         ? S->draft_frame_cfg.static_url
                                         : (S->draft_has_frame_cfg && S->draft_frame_cfg.layer_count > 0
                                                ? S->draft_frame_cfg.layers[0].url
                                                : "")));
      if (ft && ft[0] && ui_ensure_media(ui, c->r, ft)) {
        int tw = 0, th = 0;
        if (ccord_render_has_texture(c->r, ft, &tw, &th) && tw > 2 && th > 2) {
          ccord_draw_texture_cover(c->r, ft, thumb, c->tk.r_control, 255, !ui->anim.reduced_motion);
        }
      }
    } else if (pick_kind == PROF_PICK_NAMEPLATE) {
      ccord_rect npr = {thumb.x + 2.0f, thumb.y + 8.0f, thumb.w - 4.0f, 20.0f};
      ui_draw_nameplate(ui, c->r, S->draft_np_asset, S->draft_np_palette, npr, 4.0f,
                        (ui_nameplate_opts){.animate = !ui->anim.reduced_motion});
    } else {
      ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, c->tk.fs_overline, COL(brand));
      const char *t = S->draft_clan_tag[0] ? S->draft_clan_tag : "TAG";
      ccord_vec2 m = ccord_text_measure(c->r, t, strlen(t), &ts, 0.0f);
      ccord_text_draw_ellipsis(c->r, t, strlen(t), thumb.x + fmaxf((thumb.w - m.x) * 0.5f, 2.0f),
                               thumb.y + (thumb.h - ui_line_h(ui, ts.size)) * 0.5f, thumb.w - 4.0f, &ts);
    }
  } else {
    ccord_draw_circle(c->r, thumb.x + 18.0f, thumb.y + 18.0f, 7.0f, COL(border_subtle));
  }

  float bw = 90.0f;
  float bh = uis_btn_h_for(ui, UIS_H_BTN_SM);
  ccord_rect btn = {card.x + card.w - bw - 12.0f, card.y + (card.h - bh) * 0.5f, bw, bh};
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, c->tk.fs_control, COL(text_header));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, c->tk.fs_caption, has_equipped ? COL(brand) : COL(text_muted));
  float tx = thumb.x + thumb.w + 12.0f;
  float tw = fmaxf(btn.x - tx - 10.0f, 60.0f);
  ccord_text_draw_ellipsis(c->r, title, strlen(title), tx, card.y + (ui->mobile_layout ? 12.0f : 10.0f), tw, &ts);
  const char *eq = (has_equipped && equipped && equipped[0]) ? equipped : "None equipped";
  ccord_text_draw_ellipsis(c->r, eq, strlen(eq), tx, card.y + (ui->mobile_layout ? 34.0f : 30.0f), tw, &ss);

  uis_button(ui, c->r, btn, "Choose >", UIS_BTN_SECONDARY, ui->mobile_layout ? UIS_BTN_S_MD : UIS_BTN_S_SM,
             (uis_state){.hover = hovered(ui, btn, false)}, "pcos", (uint64_t)pick_kind);
  add_item(UI_PROF_COSMETIC_OPEN, pick_kind, btn, title);
  c->y += card_h + UIS_SP_2;
}

// Live Markdown renderer for the Preview Card's About Me bio
static void draw_markdown_bio(ccord_ui *ui, ccord_renderer *r, const char *bio, float x, float y, float w, float max_h,
                              ccord_color base_col, ccord_color muted_col)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style reg = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, base_col);
  ccord_text_style bld = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, base_col);
  float lh = ui_line_h(ui, reg.size);
  float cy = y;
  const char *p = bio;
  while (*p && cy < y + max_h) {
    const char *nl = strchr(p, '\n');
    size_t llen = nl ? (size_t)(nl - p) : strlen(p);
    if (llen == 0) {
      cy += lh;
      p = nl ? nl + 1 : p + llen;
      continue;
    }
    ui_md_iter it;
    ui_md_iter_init(&it, p, llen);
    ui_md_token tok;
    float cx = x;
    while (ui_md_next(&it, &tok)) {
      if (tok.len == 0) continue;
      ccord_text_style st = (tok.style == UI_MD_BOLD || tok.style == UI_MD_BOLD_ITALIC) ? bld : reg;
      if (tok.style == UI_MD_LINK) st.color = COL(text_link);
      else if (tok.style == UI_MD_ITALIC) st.color = ui_mix(base_col, muted_col, 0.2f);
      ccord_vec2 m = ccord_text_measure(r, tok.text, tok.len, &st, 0.0f);
      if (cx > x && cx + m.x > x + w) {
        cx = x;
        cy += lh;
        if (cy >= y + max_h) break;
      }
      if (tok.style == UI_MD_CODE) {
        ccord_draw_rrect(r, (ccord_rect){cx - 2.0f, cy, fminf(m.x + 4.0f, x + w - cx), lh}, 3.0f,
                         ui_alpha(COL(bg_tertiary), 0.75f));
      } else if (tok.style == UI_MD_SPOILER || tok.spoiler) {
        ccord_draw_rrect(r, (ccord_rect){cx, cy, fminf(m.x + 2.0f, x + w - cx), lh}, 3.0f,
                         ui_alpha(COL(text_muted), 0.32f));
      }
      float drawn = ccord_text_draw_ellipsis(r, tok.text, tok.len, cx, cy, fmaxf(x + w - cx, 12.0f), &st);
      if (tok.underline || tok.style == UI_MD_UNDERLINE) {
        ccord_draw_rect(r, (ccord_rect){cx, cy + lh - 2.0f, drawn, 1.0f}, st.color);
      }
      if (tok.strikethrough || tok.style == UI_MD_STRIKETHROUGH) {
        ccord_draw_rect(r, (ccord_rect){cx, cy + lh * 0.5f, drawn, 1.0f}, st.color);
      }
      cx += drawn;
    }
    cy += lh;
    p = nl ? nl + 1 : p + llen;
  }
}

static void build_preview_look(ccord_ui *ui, const ccord_user *u, ccord_user *tmp, ccord_user_profile *draft_prof,
                               ui_profile_look *look)
{
  *draft_prof = (ccord_user_profile){0};
  const ccord_user_profile *prof = (ui->d.store && u) ? ccord_store_user_profile(ui->d.store, u->id) : nullptr;
  if (prof) *draft_prof = *prof;
  if (S->draft_has_theme) {
    draft_prof->has_theme = true;
    draft_prof->theme_primary = S->draft_theme_primary;
    draft_prof->theme_accent = S->draft_theme_accent;
  } else {
    draft_prof->has_theme = false;
  }
  if (S->accent >= 0) {
    draft_prof->has_banner_color = true;
    draft_prof->banner_color = (uint32_t)S->accent & 0xffffffu;
  } else {
    draft_prof->has_banner_color = false;
    draft_prof->has_accent_color = false;
    draft_prof->accent_color = 0;
  }
  ccord_user lview = *tmp;
  if (S->img[IMG_BANNER].mode == IMG_KEEP && (!lview.banner_url || !lview.banner_url[0]) && draft_prof->banner_url &&
      draft_prof->banner_url[0]) {
    lview.banner_url = draft_prof->banner_url;
  }
  ui_profile_resolve(draft_prof, active_guild_id(S), &lview, look);
}

static float preview_height(dctx *c, const ui_profile_look *look, const char *bio, float pw)
{
  float tw = pw - 2.0f * UIS_SP_4;
  ccord_text_style bs = ui_style(c->ui, CCORD_FONT_REGULAR, c->tk.fs_control, c->ui->theme->c.text_muted);
  float lh = ui_line_h(c->ui, bs.size);
  float bio_h = lh;
  if (bio[0]) {
    bio_h = ccord_text_measure(c->r, bio, strlen(bio), &bs, tw).y;
    if (bio_h > 6.0f * lh) bio_h = 6.0f * lh;
  }
  float badge_h = look ? ui_profile_draw_badges(c->ui, c->r, look, 0.0f, 0.0f, tw, true) : 0.0f;
  ccord_text_style ss = ui_style(c->ui, CCORD_FONT_REGULAR, c->tk.fs_caption, c->ui->theme->c.text_muted);
  float since_h = look ? ui_profile_draw_since(c->ui, c->r, look, 0.0f, 0.0f, tw, &ss, true) : 0.0f;
  return PV_BANNER_H + PV_AVATAR_D * 0.5f + UIS_SP_2 + ui_line_h(c->ui, c->tk.fs_title) +
         ui_line_h(c->ui, c->tk.fs_control) + (badge_h > 0.0f ? (UIS_SP_1 + badge_h) : 0.0f) +
         UIS_SP_2 + 28.0f + UIS_SP_3 + UIS_SP_3 + ui_line_h(c->ui, c->tk.fs_overline) + UIS_SP_1 + bio_h +
         (since_h > 0.0f ? (UIS_SP_3 + ui_line_h(c->ui, c->tk.fs_overline) + UIS_SP_1 + since_h) : 0.0f) + UIS_SP_4;
}

static float draw_preview(dctx *c, float px, float py, float pw)
{
  ccord_ui *ui = c->ui;
  ccord_renderer *r = c->r;
  const uis_tokens *tk = &c->tk;
  const ccord_user *u = sp_self_user(ui);
  ccord_user tmp = {0};
  if (u) tmp = *u;
  const char *username = tmp.handle ? tmp.handle : "";
  const char *typed = ccord_text_edit_cstr(&S->ed[F_NAME]);
  const char *shown = typed[0] ? typed : (u && u->display_name && u->display_name[0] ? u->display_name
                                                                                     : (username[0] ? username : "Your name"));
  const char *pron = ccord_text_edit_cstr(&S->ed[F_PRONOUNS]);
  const char *bio = ccord_text_edit_cstr(&S->ed[F_BIO]);
  copy_str(S->pv_name, sizeof S->pv_name, shown);
  copy_str(S->pv_pronouns, sizeof S->pv_pronouns, pron);
  copy_str(S->pv_bio, sizeof S->pv_bio, bio);
  S->pv_accent = S->accent;
  tmp.name = shown;
  tmp.display_name = shown;
  if (S->img[IMG_AVATAR].mode == IMG_REMOVE) tmp.avatar_url = "";
  if (S->img[IMG_BANNER].mode == IMG_REMOVE) tmp.banner_url = "";

  ccord_user_profile draft_prof;
  ui_profile_look look = {0};
  build_preview_look(ui, u, &tmp, &draft_prof, &look);
  bool use_theme = look.has_theme;

  float ph = preview_height(c, &look, bio, pw);
  ccord_rect card = {px, py, pw, ph};
  uis_card(ui, r, card);
  ccord_push_clip(r, card);
  ccord_rect banner = {px, py, pw, PV_BANNER_H};
  if (use_theme) {
    ui_profile_draw_theme_bg(ui, r, &look, card, PV_BANNER_H, tk->r_surface);
    if (sp_ensure_img(ui, r, &S->img[IMG_BANNER])) {
      ccord_push_clip(r, banner);
      ccord_draw_texture_cover(r, S->img[IMG_BANNER].key,
                               (ccord_rect){banner.x, banner.y, banner.w, banner.h + tk->r_surface * 2.0f},
                               tk->r_surface, 255, false);
      ccord_pop_clip(r);
    } else if (S->img[IMG_BANNER].mode == IMG_KEEP) {
      ui_profile_draw_banner(ui, r, &look, &tmp, banner, tk->r_surface);
    }
  } else {
    ccord_color bcol = S->accent >= 0 ? sp_rgb((uint32_t)S->accent) : ui_mix(COL(bg_tertiary), COL(bg_secondary), 0.35f);
    ccord_push_clip(r, banner);
    ccord_draw_rrect(r, (ccord_rect){banner.x, banner.y, banner.w, banner.h + tk->r_surface * 2.0f}, tk->r_surface, bcol);
    char burl[256];
    if (sp_ensure_img(ui, r, &S->img[IMG_BANNER])) {
      ccord_draw_texture_cover(r, S->img[IMG_BANNER].key,
                               (ccord_rect){banner.x, banner.y, banner.w, banner.h + tk->r_surface * 2.0f},
                               tk->r_surface, 255, false);
    } else if (S->img[IMG_BANNER].mode == IMG_KEEP && ui_user_banner_url(&tmp, burl, sizeof burl) &&
               ui_ensure_media(ui, r, burl)) {
      int btw = 0, bth = 0;
      if (ccord_render_has_texture(r, burl, &btw, &bth) && btw > 2 && bth > 2) {
        ccord_draw_texture_cover(r, burl,
                                 (ccord_rect){banner.x, banner.y, banner.w, banner.h + tk->r_surface * 2.0f},
                                 tk->r_surface, 255, false);
      }
    }
    ccord_pop_clip(r);
  }

  ui_profile_text ptx = ui_profile_text_colors(ui, &look);
  ccord_color av_ring = use_theme ? ui_profile_card_bg(ui, &look, COL(bg_secondary)) : COL(bg_secondary);
  float acx = px + UIS_SP_4 + PV_AVATAR_D * 0.5f, acy = py + PV_BANNER_H;
  ccord_draw_circle(r, acx, acy, PV_AVATAR_D * 0.5f + UIS_SP_1 + 1.0f, av_ring);
  if (sp_ensure_img(ui, r, &S->img[IMG_AVATAR])) {
    ccord_draw_texture_rounded(r, S->img[IMG_AVATAR].key,
                               (ccord_rect){acx - PV_AVATAR_D * 0.5f, acy - PV_AVATAR_D * 0.5f, PV_AVATAR_D, PV_AVATAR_D},
                               PV_AVATAR_D * 0.5f, 255);
  } else {
    tmp.avatar_decoration_url = "";
    ui_draw_avatar(ui, r, &tmp, acx, acy, PV_AVATAR_D, av_ring, true);
  }
  const char *deco_ref = S->draft_deco_url[0] ? S->draft_deco_url : S->draft_deco_asset;
  if (deco_ref[0]) {
    sp_draw_avatar_deco_cdn(ui, r, deco_ref, acx, acy, PV_AVATAR_D, !ui->anim.reduced_motion);
  }

  float ty = py + PV_BANNER_H + PV_AVATAR_D * 0.5f + UIS_SP_2, tw = pw - 2.0f * UIS_SP_4, tx = px + UIS_SP_4;
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk->fs_title, ptx.header);
  float clan_w = S->draft_clan_tag[0] ? (ui_clan_badge_width(ui, r, S->draft_clan_tag, S->draft_clan_badge[0] != '\0') + 6.0f) : 0.0f;
  float nw = ccord_text_draw_ellipsis(r, shown, strlen(shown), tx, ty, fmaxf(tw - clan_w, 40.0f), &ns);
  if (S->draft_clan_tag[0] && tx + nw + clan_w <= tx + tw + 4.0f) {
    (void)ui_draw_clan_badge_ex(ui, r, S->draft_clan_guild, S->draft_clan_tag, S->draft_clan_badge,
                                tx + nw + 6.0f, ty + 2.0f, ui_line_h(ui, ns.size), UI_CLAN_LAYER_NONE);
  }
  ty += ui_line_h(ui, ns.size);
  char sub[256];
  snprintf(sub, sizeof sub, "%s%s%s", username, username[0] && pron[0] ? " \xC2\xB7 " : "", pron);
  ccord_text_style us = ui_style(ui, CCORD_FONT_REGULAR, tk->fs_control, ptx.secondary);
  ccord_text_draw_ellipsis(r, sub, strlen(sub), tx, ty, tw, &us);
  ty += ui_line_h(ui, us.size);

  // Badges row (matching Discord's live profile preview)
  float badge_h = ui_profile_draw_badges(ui, r, &look, tx, ty + UIS_SP_1, tw, false);
  if (badge_h > 0.0f) ty += UIS_SP_1 + badge_h;

  // Theme-tinted sample "Message" action button showing how Primary color tints buttons
  ty += UIS_SP_2;
  float msg_h = ui->mobile_layout ? UIS_MOB_H_BTN_SM : 28.0f;
  ccord_rect msg_btn = {tx, ty, ui->mobile_layout ? 104.0f : 96.0f, msg_h};
  ccord_color btn_bg = use_theme ? ui_profile_button_color(ui, &look) : COL(brand);
  ccord_draw_rrect(r, msg_btn, tk->r_control, btn_bg);
  ccord_text_style mbs = ui_style(ui, CCORD_FONT_BOLD, tk->fs_caption, (ccord_color){255, 255, 255, 255});
  ccord_vec2 mbt = ccord_text_measure(r, "Message", 7, &mbs, 0.0f);
  ccord_text_draw(r, "Message", 7, msg_btn.x + (msg_btn.w - mbt.x) * 0.5f,
                  msg_btn.y + (msg_btn.h - mbt.y) * 0.5f, &mbs, 0.0f);
  ty += msg_h + UIS_SP_3;

  uis_divider(ui, r, tx, ty, tw);
  ty += UIS_SP_3;
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, tk->fs_overline, ptx.muted);
  ccord_text_draw(r, "ABOUT ME", 8, tx, ty, &hs, 0.0f);
  ty += ui_line_h(ui, hs.size) + UIS_SP_1;
  ccord_color bio_col = use_theme ? (bio[0] ? ptx.header : ptx.muted) : (bio[0] ? COL(text_normal) : COL(text_muted));
  ccord_text_style bs = ui_style(ui, CCORD_FONT_REGULAR, tk->fs_control, bio_col);
  float blh = ui_line_h(ui, bs.size);
  float bio_h = blh;
  if (bio[0]) {
    bio_h = ccord_text_measure(r, bio, strlen(bio), &bs, tw).y;
    if (bio_h > 6.0f * blh) bio_h = 6.0f * blh;
  }
  ccord_push_clip(r, (ccord_rect){tx, ty, tw, bio_h + 2.0f});
  if (bio[0]) {
    draw_markdown_bio(ui, r, bio, tx, ty, tw, bio_h + 2.0f, bio_col, ptx.muted);
  } else {
    ccord_text_draw(r, "Nothing here yet.", 17, tx, ty, &bs, tw);
  }
  ccord_pop_clip(r);
  ty += bio_h;

  // Member Since dates
  ccord_text_style ms_st = ui_style(ui, CCORD_FONT_REGULAR, tk->fs_caption, ptx.secondary);
  float since_h = ui_profile_draw_since(ui, r, &look, tx, ty, tw, &ms_st, true);
  if (since_h > 0.0f) {
    ty += UIS_SP_3;
    ccord_text_draw(r, "MEMBER SINCE", 12, tx, ty, &hs, 0.0f);
    ty += ui_line_h(ui, hs.size) + UIS_SP_1;
    (void)ui_profile_draw_since(ui, r, &look, tx, ty, tw, &ms_st, false);
  }

  // Real Discord Profile Effect overlay (multi-layer animation or reduced-motion frame)
  if (S->draft_effect_id[0]) {
    const ccord_profile_effect_config *ecfg = S->draft_has_effect_cfg ? &S->draft_effect_cfg : nullptr;
    if (!ecfg && ui->d.session) {
      ccord_id eid = CCORD_ID_NONE;
      if (ccord_id_parse(S->draft_effect_id, strlen(S->draft_effect_id), &eid) == CCORD_OK) {
        ecfg = ccord_session_find_owned_effect(ui->d.session, eid);
      }
    }
    sp_draw_profile_effect_real(ui, r, u ? u->id : CCORD_ID_NONE, ecfg, S->draft_effect_thumb,
                                card, tk->r_surface, !ui->anim.reduced_motion);
  }
  ccord_pop_clip(r);

  // Real Discord Profile Frame overlay (drawn after popping card clip so overflow extends cleanly)
  if (S->draft_frame_sku != CCORD_ID_NONE) {
    const ccord_profile_frame_config *fcfg = S->draft_has_frame_cfg ? &S->draft_frame_cfg : nullptr;
    if (!fcfg && ui->d.session) fcfg = ccord_session_find_owned_frame(ui->d.session, S->draft_frame_sku);
    if (fcfg) {
      ui_profile_look flook = look;
      flook.frame = fcfg;
      ui_profile_draw_frame(ui, r, &flook, card);
    } else if (S->draft_frame_thumb[0] && ui_ensure_media(ui, r, S->draft_frame_thumb)) {
      int ftw = 0, fth = 0;
      if (ccord_render_has_texture(r, S->draft_frame_thumb, &ftw, &fth) && ftw > 2 && fth > 2) {
        ccord_draw_texture_rounded_anim(r, S->draft_frame_thumb, card, tk->r_surface, 255, !ui->anim.reduced_motion);
      }
    }
  }
  ui_profile_flush_tooltip(ui, r);

  // Member List Nameplate Preview strip directly underneath the sticky Preview Card
  float npy = py + ph + UIS_SP_3;
  ccord_text_style nls = ui_style(ui, CCORD_FONT_BOLD, tk->fs_overline, COL(text_muted));
  ccord_text_draw(r, "NAMEPLATE PREVIEW", 17, px, npy, &nls, 0.0f);
  npy += ui_line_h(ui, nls.size) + UIS_SP_1;
  ccord_rect np_row = {px, npy, pw, 44.0f};
  sp_draw_nameplate_strip(ui, r, np_row, tk->r_control, S->draft_np_asset, S->draft_np_palette,
                          &tmp, shown, !ui->anim.reduced_motion);
  return ph;
}

static void draw_bar(dctx *c, uint32_t changed)
{
  ccord_ui *ui = c->ui;
  ccord_renderer *r = c->r;
  ccord_rect bar = S->bar;
  uis_frame(ui, r, bar, c->tk.r_surface, COL(bg_floating), UIS_ELEV_POPOVER);
  float bh = uis_button_height(UIS_BTN_S_MD), by = bar.y + (bar.h - bh) * 0.5f;
  float right = bar.x + bar.w - UIS_SP_4;
  if (changed) {
    float sw = uis_label_width(ui, r, "Save Changes");
    ccord_rect src = {right - sw, by, sw, bh};
    uis_state sst = {.hover = hovered(ui, src, true), .disabled = S->pending};
    uis_button(ui, r, src, S->pending ? "Saving..." : "Save Changes", UIS_BTN_SUCCESS, UIS_BTN_S_MD, sst, "pbar", 1u);
    float rw = uis_label_width(ui, r, "Reset");
    ccord_rect rrc = {src.x - UIS_SP_2 - rw, by, rw, bh};
    uis_state rst = {.hover = hovered(ui, rrc, true), .disabled = S->pending};
    uis_button(ui, r, rrc, "Reset", UIS_BTN_LINK, UIS_BTN_S_MD, rst, "pbar", 0u);
    add_item(UI_PROF_RESET, 0, rrc, "Reset");
    add_item(UI_PROF_SAVE, 0, src, "Save Changes");
    right = rrc.x - UIS_SP_3;
  }
  const char *msg = "You have unsaved changes";
  ccord_color mc = COL(text_header);
  float tx = bar.x + UIS_SP_4;
  if (S->pending) {
    msg = "Saving your profile...";
  } else if (S->general[0]) {
    msg = S->general;
    mc = COL(danger);
  } else if (!changed) {
    msg = "Changes saved";
    float cy = bar.y + bar.h * 0.5f;
    ccord_draw_circle(r, tx + UIS_SP_3, cy, UIS_SP_3, COL(status_online));
    ccord_color wc = COL(text_on_brand);
    ccord_draw_line(r, (ccord_vec2){tx + UIS_SP_3 - 5.0f, cy}, (ccord_vec2){tx + UIS_SP_3 - 1.5f, cy + 3.5f}, 2.0f, wc);
    ccord_draw_line(r, (ccord_vec2){tx + UIS_SP_3 - 1.5f, cy + 3.5f}, (ccord_vec2){tx + UIS_SP_3 + 5.0f, cy - 3.5f}, 2.0f, wc);
    tx += UIS_SP_6 + UIS_SP_2;
  }
  ccord_text_style ms = ui_style(ui, CCORD_FONT_BOLD, c->tk.fs_control, mc);
  float max_w = right - tx;
  if (max_w > 0.0f) ccord_text_draw_ellipsis(r, msg, strlen(msg), tx, bar.y + (bar.h - ui_line_h(ui, ms.size)) * 0.5f, max_w, &ms);
}

void ui_settings_profile_draw(ccord_ui *ui, ccord_renderer *r, ccord_rect content, ccord_rect panel)
{
  if (!ensure_live(ui)) return;
  request_live_profile_data(ui, active_guild_id(S));
  sync_store(ui);
  uis_tokens tk = uis_tokens_of(ui);
  S->nitems = 0;
  memset(S->err_rect, 0, sizeof S->err_rect);
  uint32_t changed = changed_mask();
  float top = content.y + 48.0f;
  float bottom = content.y + content.h;
  if (bottom > panel.y + panel.h - UIS_SP_4) bottom = panel.y + panel.h - UIS_SP_4;
  S->bar_visible = changed != 0 || S->pending || S->saved_until_ms != 0 || S->general[0] != '\0';
  S->bar = (ccord_rect){content.x, bottom - BAR_H, content.w, BAR_H};
  S->view = (ccord_rect){content.x - UIS_SP_2, top, content.w + 2.0f * UIS_SP_2, fmaxf(bottom - top, 40.0f)};
  S->view_h = S->view.h;
  float max_scroll = S->content_h - S->view_h;
  if (max_scroll < 0.0f) max_scroll = 0.0f;
  if (S->scroll > max_scroll) S->scroll = max_scroll;
  bool two_col = content.w >= 700.0f;
  float fw = content.w;
  if (two_col) fw = content.w - PV_W - UIS_SP_8;
  if (fw > 480.0f) fw = 480.0f;
  dctx c = {.ui = ui, .r = r, .tk = tk, .x = content.x, .w = fw, .top = top - S->scroll, .y = 0.0f};

  // Fixed visible scrollbar when scrolling
  if (max_scroll > 0.0f) {
    float right_limit = ui->mobile_layout ? (panel.x + panel.w - ui->safe_insets.right) : (panel.x + panel.w);
    float tx = content.x + content.w + (ui->mobile_layout ? 5.0f : 10.0f);
    float max_tx = right_limit - 10.0f;
    if (tx > max_tx) tx = max_tx;
    ccord_rect tr = {tx, top, 6.0f, fmaxf(S->view_h - 8.0f, 32.0f)};
    float th = fmaxf(24.0f, tr.h * (S->view_h / (S->view_h + max_scroll)));
    float ty = tr.y + (tr.h - th) * (S->scroll / max_scroll);
    uis_scrollbar(ui, r, tr, (ccord_rect){tr.x, ty, tr.w, th});
  }

  ccord_push_clip(r, S->view);
  if (two_col) {
    (void)draw_preview(&c, content.x + fw + UIS_SP_8, top, PV_W);
  } else {
    float pw = content.w < PV_W ? content.w : PV_W;
    float ph = draw_preview(&c, content.x, c.top + c.y, pw);
    c.y += ph + 74.0f + UIS_SP_5;
  }

  // ---- Main Profile | Per-Server Profiles Subtabs ----
  float st1w = fmaxf(uis_label_width(ui, r, "Main Profile"), 112.0f);
  float st2w = fmaxf(uis_label_width(ui, r, "Per-Server Profiles"), 152.0f);
  float sub_h = ui->mobile_layout ? UIS_MOB_H_CHIP : (UIS_H_CHIP + 4.0f);
  float st_x2 = c.x + st1w + UIS_SP_2;
  float st_y2 = c.top + c.y;
  ccord_rect t1 = {c.x, st_y2, st1w, sub_h};
  if (st_x2 + st2w > c.x + c.w) {
    st_x2 = c.x;
    st_y2 += sub_h + UIS_SP_2;
    c.y += sub_h + UIS_SP_2;
  }
  ccord_rect t2 = {st_x2, st_y2, st2w, sub_h};
  uis_chip(ui, r, t1, "Main Profile", (uis_state){.hover = hovered(ui, t1, false), .selected = S->subtab == 0}, "psub", 0);
  uis_chip(ui, r, t2, "Per-Server Profiles", (uis_state){.hover = hovered(ui, t2, false), .selected = S->subtab == 1}, "psub", 1);
  add_item(UI_PROF_SUBTAB, 0, t1, "Main Profile");
  add_item(UI_PROF_SUBTAB, 1, t2, "Per-Server Profiles");
  c.y += sub_h + 12.0f;

  // Per-server selector bar
  if (S->subtab == 1) {
    section_label(&c, "CHOOSE A SERVER", nullptr, COL(text_muted));
    const char *gname = "No joined servers";
    if (ui->d.store && S->selected_guild != CCORD_ID_NONE) {
      const ccord_guild *g = ccord_store_guild(ui->d.store, S->selected_guild);
      if (g && g->name && g->name[0]) gname = g->name;
    }
    float rst_w = 128.0f;
    float sel_w = fmaxf(c.w - rst_w - UIS_SP_2, 120.0f);
    float bh = uis_btn_h_for(ui, UIS_H_BTN_SM);
    ccord_rect sbox = {c.x, c.top + c.y, sel_w, bh};
    ccord_rect rbtn = {sbox.x + sbox.w + UIS_SP_2, c.top + c.y, rst_w, bh};
    uis_select(ui, r, sbox, gname, (uis_state){.hover = hovered(ui, sbox, false), .focused = S->guild_dropdown_open}, "pgsel", 1);
    uis_button(ui, r, rbtn, "Reset Overrides", UIS_BTN_SECONDARY, ui->mobile_layout ? UIS_BTN_S_MD : UIS_BTN_S_SM,
               (uis_state){.hover = hovered(ui, rbtn, false)}, "pgrst", 1);
    add_item(UI_PROF_GUILD_TOGGLE, 0, sbox, "Choose Server");
    add_item(UI_PROF_GUILD_RESET, 0, rbtn, "Reset Overrides");
    c.y += bh + UIS_SP_2;

    if (S->guild_dropdown_open && ui->d.store) {
      size_t ng = ccord_store_guild_count(ui->d.store);
      int idx = 0;
      float grow_h = ui->mobile_layout ? 40.0f : 30.0f;
      float gstep = ui->mobile_layout ? 44.0f : 32.0f;
      for (size_t gi = 0; gi < ng && idx < 12; gi++) {
        const ccord_guild *g = ccord_store_guild_at(ui->d.store, gi);
        if (!g || g->id == CCORD_ID_NONE || g->id == CCORD_DM_GUILD_ID) continue;
        ccord_rect grow = {c.x, c.top + c.y, sel_w, grow_h};
        uis_card(ui, r, grow);
        if (g->id == S->selected_guild) uis_list_row_fill(ui, r, grow, (uis_state){.selected = true}, "pgopt", (uint64_t)idx);
        ccord_text_style gs = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_header));
        const char *gn = (g->name && g->name[0]) ? g->name : "Server";
        ccord_text_draw_ellipsis(r, gn, strlen(gn), grow.x + 10.0f,
                                 grow.y + (grow.h - ui_line_h(ui, gs.size)) * 0.5f, grow.w - 20.0f, &gs);
        add_item(UI_PROF_GUILD_PICK, idx, grow, gn);
        c.y += gstep;
        idx++;
      }
      c.y += UIS_SP_2;
    }
    c.y += UIS_SP_3;
  }

  // ---- Basic Profile Fields ----
  section_label(&c, S->subtab == 1 ? "SERVER NICKNAME" : "DISPLAY NAME", nullptr, COL(text_muted));
  text_input(&c, F_NAME, UI_PROF_NAME, S->subtab == 1 ? "Server nickname" : "Display name", fw, UIS_SP_3);
  field_error(&c, CCORD_PROFILE_FIELD_DISPLAY_NAME);
  c.y += UIS_SP_5;

  section_label(&c, "PRONOUNS", nullptr, COL(text_muted));
  text_input(&c, F_PRONOUNS, UI_PROF_PRONOUNS, "Add your pronouns", fw, UIS_SP_3);
  field_error(&c, CCORD_PROFILE_FIELD_PRONOUNS);
  c.y += UIS_SP_5;

  char left[32];
  long remaining = (long)CCORD_PROFILE_BIO_MAX - (long)cp_count(ccord_text_edit_cstr(&S->ed[F_BIO]), S->ed[F_BIO].len);
  snprintf(left, sizeof left, "%ld", remaining);
  section_label(&c, "ABOUT ME", left, remaining <= 10 ? COL(danger) : COL(text_muted));
  bio_toolbar(&c);
  bio_box(&c);
  field_error(&c, CCORD_PROFILE_FIELD_BIO);
  c.y += UIS_SP_5;

  // ---- Avatar + Unified Theme & Banner ----
  image_row(&c, IMG_AVATAR, "AVATAR", "Change Avatar", CCORD_PROFILE_FIELD_AVATAR);
  c.y += UIS_SP_5;
  theme_and_banner_section(&c);
  c.y += UIS_SP_5;

  // ---- Clean "Currently Equipped" Cosmetic Launcher Buttons (Specialized Menus!) ----
  uis_divider(ui, r, c.x, c.top + c.y, c.w);
  c.y += UIS_SP_4;
  ccord_fetch_state cstate = ui->d.session ? ccord_session_collectibles_state(ui->d.session) : CCORD_FETCH_IDLE;
  const char *cright = cstate == CCORD_FETCH_LOADING ? "Syncing..." : "Owned Collection";
  section_label(&c, "PROFILE COSMETICS", cright, cstate == CCORD_FETCH_LOADING ? COL(text_muted) : COL(brand));
  {
    const ccord_owned_collectible *de = sp_find_owned_deco(ui, S->draft_deco_sku,
                                                           S->draft_deco_url[0] ? S->draft_deco_url : S->draft_deco_asset);
    const ccord_owned_collectible *ef = sp_find_owned_effect(ui, S->draft_effect_id);
    const ccord_owned_collectible *fr = sp_find_owned_frame(ui, S->draft_frame_sku);
    const ccord_owned_collectible *np = sp_find_owned_nameplate(ui, S->draft_np_sku, S->draft_np_asset);
    cosmetic_launcher_card(&c, PROF_PICK_DECO, "Avatar Decoration",
                           de ? de->name : S->draft_deco_name,
                           S->draft_deco_sku != CCORD_ID_NONE || S->draft_deco_url[0] != '\0' || S->draft_deco_asset[0] != '\0');
    cosmetic_launcher_card(&c, PROF_PICK_EFFECT, "Profile Effect",
                           ef ? ef->name : S->draft_effect_name,
                           S->draft_effect_id[0] != '\0');
    cosmetic_launcher_card(&c, PROF_PICK_FRAME, "Profile Frame",
                           fr ? fr->name : S->draft_frame_name,
                           S->draft_frame_sku != CCORD_ID_NONE);
    cosmetic_launcher_card(&c, PROF_PICK_NAMEPLATE, "Nameplate",
                           np ? np->name : S->draft_np_name,
                           S->draft_np_sku != CCORD_ID_NONE || S->draft_np_asset[0] != '\0');
    if (S->subtab == 0) {
      char cdisp[64] = "";
      if (S->draft_clan_tag[0]) snprintf(cdisp, sizeof cdisp, "[%s] Equipped", S->draft_clan_tag);
      cosmetic_launcher_card(&c, PROF_PICK_CLAN, "Server Tag", cdisp,
                             S->draft_clan_guild != CCORD_ID_NONE || S->draft_clan_tag[0] != '\0');
    }
  }
  c.y += UIS_SP_4;

  // ---- Cross-links to My Account, Privacy & Safety, and Discord Web ----
  uis_divider(ui, r, c.x, c.top + c.y, c.w);
  c.y += UIS_SP_4;
  section_label(&c, "ACCOUNT & PRIVACY LINKS", nullptr, COL(text_muted));
  const char *sentence = "Manage login details in My Account, profile visibility in Privacy & Safety, or open Discord on the web.";
  ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_secondary));
  ccord_text_draw(r, sentence, strlen(sentence), c.x, c.top + c.y, &ps, c.w);
  c.y += ccord_text_measure(r, sentence, strlen(sentence), &ps, c.w).y + UIS_SP_3;

  float bh = uis_button_height(UIS_BTN_S_MD);
  float aw = uis_label_width(ui, r, "My Account ->");
  float pw = uis_label_width(ui, r, "Profile Privacy ->");
  float ww = uis_label_width(ui, r, "Open discord.com");
  ccord_rect arc = {c.x, c.top + c.y, aw, bh};
  ccord_rect prc = {arc.x + aw + UIS_SP_2, c.top + c.y, pw, bh};
  bool wrap_web = (prc.x + pw + UIS_SP_2 + ww > c.x + c.w);
  ccord_rect wrc = wrap_web ? (ccord_rect){c.x, c.top + c.y + bh + UIS_SP_2, ww, bh}
                            : (ccord_rect){prc.x + pw + UIS_SP_2, c.top + c.y, ww, bh};

  uis_button(ui, r, arc, "My Account ->", UIS_BTN_SECONDARY, UIS_BTN_S_MD, (uis_state){.hover = hovered(ui, arc, false)}, "pbtn", 5u);
  uis_button(ui, r, prc, "Profile Privacy ->", UIS_BTN_SECONDARY, UIS_BTN_S_MD, (uis_state){.hover = hovered(ui, prc, false)}, "pbtn", 6u);
  uis_button(ui, r, wrc, "Open discord.com", UIS_BTN_SECONDARY, UIS_BTN_S_MD, (uis_state){.hover = hovered(ui, wrc, false)}, "pbtn", 7u);
  add_item(UI_PROF_LINK_ACCOUNT, 0, arc, "My Account");
  add_item(UI_PROF_LINK_PRIVACY, 0, prc, "Profile Privacy");
  add_item(UI_PROF_WEB, 0, wrc, "Open discord.com");
  c.y += wrap_web ? (2.0f * bh + UIS_SP_2) : bh;

  S->content_h = 52.0f + c.y + UIS_SP_6 + BAR_H + 2.0f * UIS_SP_4;
  ccord_pop_clip(r);

  if (S->bar_visible) draw_bar(&c, changed);
}

void ui_settings_profile_draw_overlays(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r || !S || S->picker.kind == PROF_PICK_NONE) return;
  sp_picker_draw(ui, r, S);
}

// ---------------------------------------------------------------- test seams

size_t ui_settings_profile_items(const ccord_ui *ui, const ui_profile_item **out)
{
  (void)ui;
  if (out) *out = S ? S->items : nullptr;
  return S ? S->nitems : 0;
}

void ui_settings_profile_debug(const ccord_ui *ui, ui_profile_debug *out)
{
  (void)ui;
  *out = (ui_profile_debug){0};
  out->preview_accent = -1;
  if (!S) return;
  out->live = true;
  out->changed = changed_mask();
  out->pending = S->pending;
  out->saved = S->saved_until_ms != 0;
  out->bar_visible = S->bar_visible;
  for (int i = 0; i < 6; i++) {
    out->has_error[i] = S->err[i][0] != '\0';
    copy_str(out->error[i], sizeof out->error[i], S->err[i]);
  }
  copy_str(out->general, sizeof out->general, S->general);
  for (int i = 0; i < 6; i++) out->error_rect[i] = S->err_rect[i];
  copy_str(out->preview_name, sizeof out->preview_name, S->pv_name);
  copy_str(out->preview_pronouns, sizeof out->preview_pronouns, S->pv_pronouns);
  copy_str(out->preview_bio, sizeof out->preview_bio, S->pv_bio);
  out->preview_accent = S->pv_accent;
  out->avatar_mode = S->img[IMG_AVATAR].mode;
  out->banner_mode = S->img[IMG_BANNER].mode;
  out->bar = S->bar;
  out->view = S->view;
  out->scroll = S->scroll;
  out->max_scroll = S->content_h > S->view_h ? S->content_h - S->view_h : 0.0f;
  for (int f = 0; f < F_TEXT_COUNT; f++) out->text[f] = ccord_text_edit_cstr(&S->ed[f]);
  out->subtab = S->subtab;
  out->selected_guild = S->selected_guild;
  out->open_picker = (int)S->picker.kind;
  out->has_theme = S->draft_has_theme;
  out->theme_primary = S->draft_theme_primary;
  out->theme_accent = S->draft_theme_accent;
  copy_str(out->deco_name, sizeof out->deco_name, S->draft_deco_name);
  copy_str(out->effect_name, sizeof out->effect_name, S->draft_effect_name);
  copy_str(out->frame_name, sizeof out->frame_name, S->draft_frame_name);
  copy_str(out->nameplate_name, sizeof out->nameplate_name, S->draft_np_name);
  copy_str(out->clan_tag, sizeof out->clan_tag, S->draft_clan_tag);
  out->avatar_draft_len = S->img[IMG_AVATAR].len;
  out->banner_draft_len = S->img[IMG_BANNER].len;
  out->avatar_zoom = S->img[IMG_AVATAR].zoom;
  out->banner_zoom = S->img[IMG_BANNER].zoom;
}

bool ui_settings_profile_picker_hit(const ccord_ui *ui, int kind, int arg, ccord_rect *out)
{
  (void)ui;
  if (!S || S->picker.kind == PROF_PICK_NONE || !out) return false;
  for (size_t i = 0; i < S->picker.nhits; i++) {
    if (S->picker.hits[i].kind == kind && S->picker.hits[i].arg == arg) {
      *out = S->picker.hits[i].rect;
      return true;
    }
  }
  return false;
}
