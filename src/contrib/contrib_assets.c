// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "contrib_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>

void contrib_free_bytes(const uint8_t *p)
{
  if (!p) return;
  void *raw = nullptr;
  memcpy(&raw, &p, sizeof raw);
  ccord_free(raw);
}

static void free_preset(ccord_theme_preset *p)
{
  contrib_free_str(p->owner);
  contrib_free_str(p->preset_id);
  contrib_free_str(p->name);
  contrib_free_str(p->base_id);
  contrib_free_str(p->css);
  contrib_free_str(p->colors_json);
  contrib_free_str(p->metrics_json);
  contrib_free_str(p->font_regular);
  contrib_free_str(p->font_bold);
}

static void free_layer(ccord_theme_layer *l)
{
  contrib_free_str(l->owner);
  contrib_free_str(l->layer_id);
  contrib_free_str(l->base_id);
  contrib_free_str(l->css);
  contrib_free_str(l->colors_json);
  contrib_free_str(l->metrics_json);
  contrib_free_str(l->font_regular);
  contrib_free_str(l->font_bold);
  contrib_free_str(l->bg_slot);
  contrib_free_str(l->bg_texture_key);
  contrib_free_str(l->bg_fit);
}

static void free_icon_ov(ccord_icon_override *o)
{
  contrib_free_str(o->owner);
  contrib_free_str(o->icon_name);
  contrib_free_str(o->texture_key);
}

static void free_tex(ccord_plugin_texture *t)
{
  contrib_free_str(t->owner);
  contrib_free_str(t->key);
  contrib_free_bytes(t->data);
}


static void free_earcon(ccord_earcon_override *e)
{
  contrib_free_str(e->owner);
  contrib_free_str(e->kind_name);
  contrib_free_bytes(e->audio_data);
}


void contrib_assets_destroy(ccord_contrib *c)
{
  for (size_t i = 0; i < c->npresets; i++) free_preset(&c->presets[i]);
  ccord_free(c->presets);
  for (size_t i = 0; i < c->ntheme_layers; i++) free_layer(&c->theme_layers[i]);
  ccord_free(c->theme_layers);
  for (size_t i = 0; i < c->nicon_overrides; i++) free_icon_ov(&c->icon_overrides[i]);
  ccord_free(c->icon_overrides);
  for (size_t i = 0; i < c->ntextures; i++) free_tex(&c->textures[i]);
  ccord_free(c->textures);
  for (size_t i = 0; i < c->nearcons; i++) free_earcon(&c->earcons[i]);
  ccord_free(c->earcons);
}

bool contrib_assets_remove_owner(ccord_contrib *c, const char *owner)
{
  bool changed = false;
  for (size_t i = c->npresets; i > 0; i--) {
    if (strcmp(c->presets[i - 1].owner, owner) == 0) {
      free_preset(&c->presets[i - 1]);
      memmove(&c->presets[i - 1], &c->presets[i], (c->npresets - i) * sizeof *c->presets);
      c->npresets--;
      c->theme_rev++;
      changed = true;
    }
  }
  for (size_t i = c->ntheme_layers; i > 0; i--) {
    if (strcmp(c->theme_layers[i - 1].owner, owner) == 0) {
      free_layer(&c->theme_layers[i - 1]);
      memmove(&c->theme_layers[i - 1], &c->theme_layers[i], (c->ntheme_layers - i) * sizeof *c->theme_layers);
      c->ntheme_layers--;
      c->theme_rev++;
      changed = true;
    }
  }
  for (size_t i = c->nicon_overrides; i > 0; i--) {
    if (strcmp(c->icon_overrides[i - 1].owner, owner) == 0) {
      free_icon_ov(&c->icon_overrides[i - 1]);
      memmove(&c->icon_overrides[i - 1], &c->icon_overrides[i], (c->nicon_overrides - i) * sizeof *c->icon_overrides);
      c->nicon_overrides--;
      c->theme_rev++;
      changed = true;
    }
  }
  for (size_t i = c->ntextures; i > 0; i--) {
    if (strcmp(c->textures[i - 1].owner, owner) == 0) {
      free_tex(&c->textures[i - 1]);
      memmove(&c->textures[i - 1], &c->textures[i], (c->ntextures - i) * sizeof *c->textures);
      c->ntextures--;
      c->textures_rev++;
      changed = true;
    }
  }
  for (size_t i = c->nearcons; i > 0; i--) {
    if (strcmp(c->earcons[i - 1].owner, owner) == 0) {
      free_earcon(&c->earcons[i - 1]);
      memmove(&c->earcons[i - 1], &c->earcons[i], (c->nearcons - i) * sizeof *c->earcons);
      c->nearcons--;
      c->earcons_rev++;
      changed = true;
    }
  }
  return changed;
}

uint64_t ccord_contrib_theme_revision(const ccord_contrib *c) { return c ? c->theme_rev : 0; }
uint64_t ccord_contrib_textures_revision(const ccord_contrib *c) { return c ? c->textures_rev : 0; }
uint64_t ccord_contrib_earcons_revision(const ccord_contrib *c) { return c ? c->earcons_rev : 0; }

// ---- Theme Presets & Layers & Icons ----
ccord_status ccord_contrib_add_theme_preset(ccord_contrib *c, const ccord_theme_preset *preset, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !preset || !contrib_text_ok(preset->owner, CONTRIB_OWNER_MAX, false) ||
      !contrib_name_ok(preset->preset_id) || !contrib_text_ok(preset->name, 64u, false)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad theme preset");
  }
  ccord_theme_preset p = {
    .owner = ccord_strdup(preset->owner),
    .preset_id = ccord_strdup(preset->preset_id),
    .name = ccord_strdup(preset->name),
    .base_id = dup_opt(preset->base_id && preset->base_id[0] ? preset->base_id : "c-cord-dark"),
    .css = dup_opt(preset->css),
    .colors_json = dup_opt(preset->colors_json),
    .metrics_json = dup_opt(preset->metrics_json),
    .font_regular = dup_opt(preset->font_regular),
    .font_bold = dup_opt(preset->font_bold),
  };
  if (!p.owner || !p.preset_id || !p.name || !p.base_id || !p.css ||
      !p.colors_json || !p.metrics_json || !p.font_regular || !p.font_bold) {
    free_preset(&p);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
  }
  for (size_t i = 0; i < c->npresets; i++) {
    if (strcmp(c->presets[i].owner, p.owner) == 0 && strcmp(c->presets[i].preset_id, p.preset_id) == 0) {
      free_preset(&c->presets[i]);
      c->presets[i] = p;
      c->theme_rev++; c->revision++;
      return CCORD_OK;
    }
  }
  if (c->npresets == c->cappresets) {
    size_t nc = c->cappresets ? c->cappresets * 2u : 8u;
    ccord_theme_preset *g = ccord_realloc_array(c->presets, nc, sizeof *g);
    if (!g) { free_preset(&p); return ccord_error_set(err, CCORD_ERR_NOMEM, "oom"); }
    c->presets = g; c->cappresets = nc;
  }
  c->presets[c->npresets++] = p;
  c->theme_rev++; c->revision++;
  return CCORD_OK;
}

bool ccord_contrib_remove_theme_preset(ccord_contrib *c, const char *owner, const char *preset_id)
{
  if (!c || !owner || !preset_id) return false;
  for (size_t i = 0; i < c->npresets; i++) {
    if (strcmp(c->presets[i].owner, owner) == 0 && strcmp(c->presets[i].preset_id, preset_id) == 0) {
      free_preset(&c->presets[i]);
      memmove(&c->presets[i], &c->presets[i + 1], (c->npresets - i - 1u) * sizeof c->presets[0]);
      c->npresets--;
      c->theme_rev++; c->revision++;
      return true;
    }
  }
  return false;
}

size_t ccord_contrib_theme_preset_count(const ccord_contrib *c) { return c ? c->npresets : 0; }
const ccord_theme_preset *ccord_contrib_theme_preset_at(const ccord_contrib *c, size_t i)
{
  return (c && i < c->npresets) ? &c->presets[i] : nullptr;
}
const ccord_theme_preset *ccord_contrib_find_theme_preset(const ccord_contrib *c, const char *owner, const char *preset_id)
{
  if (!c || !preset_id) return nullptr;
  for (size_t i = 0; i < c->npresets; i++) {
    if ((!owner || strcmp(c->presets[i].owner, owner) == 0) && strcmp(c->presets[i].preset_id, preset_id) == 0) {
      return &c->presets[i];
    }
  }
  return nullptr;
}

ccord_status ccord_contrib_set_theme_layer(ccord_contrib *c, const ccord_theme_layer *layer, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !layer || !contrib_text_ok(layer->owner, CONTRIB_OWNER_MAX, false) ||
      !contrib_text_ok(layer->layer_id, CONTRIB_ITEM_MAX, false)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad theme layer");
  }
  ccord_theme_layer l = {
    .owner = ccord_strdup(layer->owner),
    .layer_id = ccord_strdup(layer->layer_id),
    .base_id = dup_opt(layer->base_id),
    .css = dup_opt(layer->css),
    .colors_json = dup_opt(layer->colors_json),
    .metrics_json = dup_opt(layer->metrics_json),
    .font_regular = dup_opt(layer->font_regular),
    .font_bold = dup_opt(layer->font_bold),
    .bg_slot = dup_opt(layer->bg_slot),
    .bg_texture_key = dup_opt(layer->bg_texture_key),
    .bg_opacity = layer->bg_opacity,
    .bg_fit = dup_opt(layer->bg_fit && layer->bg_fit[0] ? layer->bg_fit : "cover"),
    .priority = layer->priority,
    .revision = ++c->theme_rev,
  };
  if (!l.owner || !l.layer_id || !l.base_id || !l.css || !l.colors_json || !l.metrics_json ||
      !l.font_regular || !l.font_bold || !l.bg_slot || !l.bg_texture_key || !l.bg_fit) {
    free_layer(&l);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
  }
  for (size_t i = 0; i < c->ntheme_layers; i++) {
    if (strcmp(c->theme_layers[i].owner, l.owner) == 0 && strcmp(c->theme_layers[i].layer_id, l.layer_id) == 0) {
      free_layer(&c->theme_layers[i]);
      c->theme_layers[i] = l;
      c->revision++;
      return CCORD_OK;
    }
  }
  if (c->ntheme_layers == c->captheme_layers) {
    size_t nc = c->captheme_layers ? c->captheme_layers * 2u : 8u;
    ccord_theme_layer *g = ccord_realloc_array(c->theme_layers, nc, sizeof *g);
    if (!g) { free_layer(&l); return ccord_error_set(err, CCORD_ERR_NOMEM, "oom"); }
    c->theme_layers = g; c->captheme_layers = nc;
  }
  c->theme_layers[c->ntheme_layers++] = l;
  c->revision++;
  return CCORD_OK;
}

bool ccord_contrib_remove_theme_layer(ccord_contrib *c, const char *owner, const char *layer_id)
{
  if (!c || !owner || !layer_id) return false;
  for (size_t i = 0; i < c->ntheme_layers; i++) {
    if (strcmp(c->theme_layers[i].owner, owner) == 0 && strcmp(c->theme_layers[i].layer_id, layer_id) == 0) {
      free_layer(&c->theme_layers[i]);
      memmove(&c->theme_layers[i], &c->theme_layers[i + 1], (c->ntheme_layers - i - 1u) * sizeof c->theme_layers[0]);
      c->ntheme_layers--;
      c->theme_rev++; c->revision++;
      return true;
    }
  }
  return false;
}

void ccord_contrib_clear_theme_layers(ccord_contrib *c, const char *owner)
{
  if (!c || !owner) return;
  size_t w = 0;
  bool ch = false;
  for (size_t r = 0; r < c->ntheme_layers; r++) {
    if (strcmp(c->theme_layers[r].owner, owner) == 0) { free_layer(&c->theme_layers[r]); ch = true; }
    else c->theme_layers[w++] = c->theme_layers[r];
  }
  c->ntheme_layers = w;
  if (ch) { c->theme_rev++; c->revision++; }
}

size_t ccord_contrib_theme_layer_count(const ccord_contrib *c) { return c ? c->ntheme_layers : 0; }
const ccord_theme_layer *ccord_contrib_theme_layer_at(const ccord_contrib *c, size_t i)
{
  return (c && i < c->ntheme_layers) ? &c->theme_layers[i] : nullptr;
}

ccord_status ccord_contrib_set_icon_override(ccord_contrib *c, const char *owner, const char *icon_name,
                                             const char *texture_key, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !contrib_text_ok(owner, CONTRIB_OWNER_MAX, false) ||
      !contrib_text_ok(icon_name, 48u, false) || !contrib_text_ok(texture_key, 128u, false)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad icon override");
  }
  for (size_t i = 0; i < c->nicon_overrides; i++) {
    if (strcmp(c->icon_overrides[i].owner, owner) == 0 && strcmp(c->icon_overrides[i].icon_name, icon_name) == 0) {
      char *tk = ccord_strdup(texture_key);
      if (!tk) return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
      contrib_free_str(c->icon_overrides[i].texture_key);
      c->icon_overrides[i].texture_key = tk;
      c->theme_rev++; c->revision++;
      return CCORD_OK;
    }
  }
  if (c->nicon_overrides == c->capicon_overrides) {
    size_t nc = c->capicon_overrides ? c->capicon_overrides * 2u : 8u;
    ccord_icon_override *g = ccord_realloc_array(c->icon_overrides, nc, sizeof *g);
    if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
    c->icon_overrides = g; c->capicon_overrides = nc;
  }
  ccord_icon_override ov = {
    .owner = ccord_strdup(owner),
    .icon_name = ccord_strdup(icon_name),
    .texture_key = ccord_strdup(texture_key),
  };
  if (!ov.owner || !ov.icon_name || !ov.texture_key) { free_icon_ov(&ov); return ccord_error_set(err, CCORD_ERR_NOMEM, "oom"); }
  c->icon_overrides[c->nicon_overrides++] = ov;
  c->theme_rev++; c->revision++;
  return CCORD_OK;
}

bool ccord_contrib_remove_icon_override(ccord_contrib *c, const char *owner, const char *icon_name)
{
  if (!c || !owner || !icon_name) return false;
  for (size_t i = 0; i < c->nicon_overrides; i++) {
    if (strcmp(c->icon_overrides[i].owner, owner) == 0 && strcmp(c->icon_overrides[i].icon_name, icon_name) == 0) {
      free_icon_ov(&c->icon_overrides[i]);
      memmove(&c->icon_overrides[i], &c->icon_overrides[i + 1], (c->nicon_overrides - i - 1u) * sizeof c->icon_overrides[0]);
      c->nicon_overrides--;
      c->theme_rev++; c->revision++;
      return true;
    }
  }
  return false;
}

const char *ccord_contrib_find_icon_override(const ccord_contrib *c, const char *icon_name)
{
  if (!c || !icon_name) return nullptr;
  for (size_t i = c->nicon_overrides; i > 0; i--) {
    if (strcmp(c->icon_overrides[i - 1u].icon_name, icon_name) == 0) {
      return c->icon_overrides[i - 1u].texture_key;
    }
  }
  return nullptr;
}

// ---- Plugin Textures ----
ccord_status ccord_contrib_set_texture(ccord_contrib *c, const char *owner, const char *key,
                                       const uint8_t *data, size_t len, int width, int height,
                                       bool is_rgba, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !contrib_text_ok(owner, CONTRIB_OWNER_MAX, false) || !contrib_text_ok(key, 128u, false) ||
      !data || len == 0 || len > (8u * 1024u * 1024u)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad texture");
  }
  if (is_rgba && (width <= 0 || height <= 0 || (size_t)width * (size_t)height * 4u != len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: RGBA texture dimensions mismatch");
  }
  uint8_t *copy = ccord_malloc(len);
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
  memcpy(copy, data, len);
  for (size_t i = 0; i < c->ntextures; i++) {
    if (strcmp(c->textures[i].owner, owner) == 0 && strcmp(c->textures[i].key, key) == 0) {
      contrib_free_bytes(c->textures[i].data);
      c->textures[i].data = copy;
      c->textures[i].len = len;
      c->textures[i].width = width;
      c->textures[i].height = height;
      c->textures[i].is_rgba = is_rgba;
      c->textures[i].revision = ++c->textures_rev;
      c->revision++;
      return CCORD_OK;
    }
  }
  if (c->ntextures >= 4096u) {
    ccord_free(copy);
    return ccord_error_set(err, CCORD_ERR_RANGE, "contrib: texture limit reached");
  }
  size_t total_tex_bytes = len;
  for (size_t i = 0; i < c->ntextures; i++) total_tex_bytes += c->textures[i].len;
  if (total_tex_bytes > (128u * 1024u * 1024u)) {
    ccord_free(copy);
    return ccord_error_set(err, CCORD_ERR_RANGE, "contrib: texture memory budget exceeded (128 MiB)");
  }
  if (c->ntextures == c->captextures) {
    size_t nc = c->captextures ? c->captextures * 2u : 8u;
    ccord_plugin_texture *g = ccord_realloc_array(c->textures, nc, sizeof *g);
    if (!g) { ccord_free(copy); return ccord_error_set(err, CCORD_ERR_NOMEM, "oom"); }
    c->textures = g; c->captextures = nc;
  }
  ccord_plugin_texture t = {
    .owner = ccord_strdup(owner),
    .key = ccord_strdup(key),
    .data = copy,
    .len = len,
    .width = width,
    .height = height,
    .is_rgba = is_rgba,
    .revision = ++c->textures_rev,
  };
  if (!t.owner || !t.key) { free_tex(&t); return ccord_error_set(err, CCORD_ERR_NOMEM, "oom"); }
  c->textures[c->ntextures++] = t;
  c->revision++;
  return CCORD_OK;
}

bool ccord_contrib_remove_texture(ccord_contrib *c, const char *owner, const char *key)
{
  if (!c || !owner || !key) return false;
  for (size_t i = 0; i < c->ntextures; i++) {
    if (strcmp(c->textures[i].owner, owner) == 0 && strcmp(c->textures[i].key, key) == 0) {
      free_tex(&c->textures[i]);
      memmove(&c->textures[i], &c->textures[i + 1], (c->ntextures - i - 1u) * sizeof c->textures[0]);
      c->ntextures--;
      c->textures_rev++; c->revision++;
      return true;
    }
  }
  return false;
}

size_t ccord_contrib_texture_count(const ccord_contrib *c) { return c ? c->ntextures : 0; }
const ccord_plugin_texture *ccord_contrib_texture_at(const ccord_contrib *c, size_t i)
{
  return (c && i < c->ntextures) ? &c->textures[i] : nullptr;
}
const ccord_plugin_texture *ccord_contrib_find_texture(const ccord_contrib *c, const char *key)
{
  if (!c || !key) return nullptr;
  for (size_t i = 0; i < c->ntextures; i++) {
    if (strcmp(c->textures[i].key, key) == 0) return &c->textures[i];
  }
  return nullptr;
}


// ---- Earcon Overrides ----
ccord_status ccord_contrib_set_earcon_override(ccord_contrib *c, const char *owner, const char *kind_name,
                                               const uint8_t *audio_data, size_t audio_len, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !contrib_text_ok(owner, CONTRIB_OWNER_MAX, false) || !contrib_text_ok(kind_name, 32u, false) ||
      !audio_data || audio_len == 0 || audio_len > (2u * 1024u * 1024u)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad earcon override");
  }
  uint8_t *copy = ccord_malloc(audio_len);
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
  memcpy(copy, audio_data, audio_len);
  for (size_t i = 0; i < c->nearcons; i++) {
    if (strcmp(c->earcons[i].owner, owner) == 0 && strcmp(c->earcons[i].kind_name, kind_name) == 0) {
      contrib_free_bytes(c->earcons[i].audio_data);
      c->earcons[i].audio_data = copy;
      c->earcons[i].audio_len = audio_len;
      c->earcons[i].revision = ++c->earcons_rev;
      c->revision++;
      return CCORD_OK;
    }
  }
  if (c->nearcons == c->capearcons) {
    size_t nc = c->capearcons ? c->capearcons * 2u : 8u;
    ccord_earcon_override *g = ccord_realloc_array(c->earcons, nc, sizeof *g);
    if (!g) { ccord_free(copy); return ccord_error_set(err, CCORD_ERR_NOMEM, "oom"); }
    c->earcons = g; c->capearcons = nc;
  }
  ccord_earcon_override ov = {
    .owner = ccord_strdup(owner),
    .kind_name = ccord_strdup(kind_name),
    .audio_data = copy,
    .audio_len = audio_len,
    .revision = ++c->earcons_rev,
  };
  if (!ov.owner || !ov.kind_name) { free_earcon(&ov); return ccord_error_set(err, CCORD_ERR_NOMEM, "oom"); }
  c->earcons[c->nearcons++] = ov;
  c->revision++;
  return CCORD_OK;
}

bool ccord_contrib_remove_earcon_override(ccord_contrib *c, const char *owner, const char *kind_name)
{
  if (!c || !owner || !kind_name) return false;
  for (size_t i = 0; i < c->nearcons; i++) {
    if (strcmp(c->earcons[i].owner, owner) == 0 && strcmp(c->earcons[i].kind_name, kind_name) == 0) {
      free_earcon(&c->earcons[i]);
      memmove(&c->earcons[i], &c->earcons[i + 1], (c->nearcons - i - 1u) * sizeof c->earcons[0]);
      c->nearcons--;
      c->earcons_rev++; c->revision++;
      return true;
    }
  }
  return false;
}

const ccord_earcon_override *ccord_contrib_find_earcon_override(const ccord_contrib *c, const char *kind_name)
{
  if (!c || !kind_name) return nullptr;
  for (size_t i = c->nearcons; i > 0; i--) {
    if (strcmp(c->earcons[i - 1u].kind_name, kind_name) == 0) return &c->earcons[i - 1u];
  }
  return nullptr;
}

