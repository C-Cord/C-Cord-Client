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

static void free_decor(ccord_decorator_contribution *d)
{
  contrib_free_str(d->owner);
  contrib_free_str(d->decor_id);
  contrib_free_str(d->widget_json);
  contrib_free_str(d->accent_color);
  contrib_free_str(d->tint_color);
  contrib_free_str(d->text_color);
  contrib_free_str(d->author_color);
  contrib_free_str(d->collapse_label);
}

static void free_settings(ccord_plugin_settings_schema *s)
{
  contrib_free_str(s->owner);
  contrib_free_str(s->schema_json);
  contrib_free_str(s->values_json);
}


static void free_kb(ccord_keybind_contribution *k)
{
  contrib_free_str(k->owner);
  contrib_free_str(k->bind_id);
  contrib_free_str(k->label);
  contrib_free_str(k->default_combo);
  contrib_free_str(k->current_combo);
}

static void free_actbtn(ccord_action_btn_contribution *b)
{
  contrib_free_str(b->owner);
  contrib_free_str(b->btn_id);
  contrib_free_str(b->label);
  contrib_free_str(b->icon);
  contrib_free_str(b->tooltip);
  contrib_free_str(b->badge);
}

static void free_local_msg(ccord_local_message *m)
{
  contrib_free_str(m->owner);
  contrib_free_str(m->client_id);
  contrib_free_str(m->author_name);
  contrib_free_str(m->author_color);
  contrib_free_str(m->author_badge);
  contrib_free_str(m->badge_color);
  contrib_free_str(m->content);
  contrib_free_str(m->text_color);
  contrib_free_str(m->accent_color);
  contrib_free_str(m->tint_color);
  contrib_free_str(m->embed_json);
  contrib_free_str(m->attachments_json);
}

static void free_ac(ccord_autocomplete_provider *a)
{
  contrib_free_str(a->owner);
  contrib_free_str(a->provider_id);
  contrib_free_str(a->trigger);
  contrib_free_str(a->description);
}


static void free_dialog(ccord_plugin_dialog *d)
{
  contrib_free_str(d->owner);
  contrib_free_str(d->plugin_name);
  contrib_free_str(d->title);
  contrib_free_str(d->message);
  contrib_free_str(d->confirm_label);
  contrib_free_str(d->cancel_label);
  contrib_free_str(d->default_value);
  contrib_free_str(d->fields_json);
  memset(d, 0, sizeof *d);
}

static void free_hitl(ccord_account_hitl_req *h)
{
  contrib_free_str(h->owner);
  contrib_free_str(h->plugin_name);
  contrib_free_str(h->target_value);
  contrib_free_str(h->summary);
  memset(h, 0, sizeof *h);
}


void contrib_ext_destroy(ccord_contrib *c)
{
  for (size_t i = 0; i < c->ndecors; i++) free_decor(&c->decors[i]);
  ccord_free(c->decors);
  for (size_t i = 0; i < c->nsettings; i++) free_settings(&c->settings[i]);
  ccord_free(c->settings);
  contrib_assets_destroy(c);
  for (size_t i = 0; i < c->nkeybinds; i++) free_kb(&c->keybinds[i]);
  ccord_free(c->keybinds);
  for (size_t i = 0; i < c->naction_btns; i++) free_actbtn(&c->action_btns[i]);
  ccord_free(c->action_btns);
  for (size_t i = 0; i < c->nlocal_msgs; i++) free_local_msg(&c->local_msgs[i]);
  ccord_free(c->local_msgs);
  for (size_t i = 0; i < c->nautocompletes; i++) free_ac(&c->autocompletes[i]);
  ccord_free(c->autocompletes);
  if (c->has_active_dialog) free_dialog(&c->active_dialog);
  if (c->has_active_hitl) free_hitl(&c->active_hitl);
}

bool contrib_ext_remove_owner(ccord_contrib *c, const char *owner)
{
  bool changed = contrib_assets_remove_owner(c, owner);
  for (size_t i = c->ndecors; i > 0; i--) {
    if (strcmp(c->decors[i - 1].owner, owner) == 0) {
      free_decor(&c->decors[i - 1]);
      memmove(&c->decors[i - 1], &c->decors[i], (c->ndecors - i) * sizeof *c->decors);
      c->ndecors--;
      c->decors_rev++;
      changed = true;
    }
  }
  for (size_t i = c->nsettings; i > 0; i--) {
    if (strcmp(c->settings[i - 1].owner, owner) == 0) {
      free_settings(&c->settings[i - 1]);
      memmove(&c->settings[i - 1], &c->settings[i], (c->nsettings - i) * sizeof *c->settings);
      c->nsettings--;
      changed = true;
    }
  }
  for (size_t i = c->nkeybinds; i > 0; i--) {
    if (strcmp(c->keybinds[i - 1].owner, owner) == 0) {
      free_kb(&c->keybinds[i - 1]);
      memmove(&c->keybinds[i - 1], &c->keybinds[i], (c->nkeybinds - i) * sizeof *c->keybinds);
      c->nkeybinds--;
      changed = true;
    }
  }
  for (size_t i = c->naction_btns; i > 0; i--) {
    if (strcmp(c->action_btns[i - 1].owner, owner) == 0) {
      free_actbtn(&c->action_btns[i - 1]);
      memmove(&c->action_btns[i - 1], &c->action_btns[i], (c->naction_btns - i) * sizeof *c->action_btns);
      c->naction_btns--;
      changed = true;
    }
  }
  for (size_t i = c->nlocal_msgs; i > 0; i--) {
    if (strcmp(c->local_msgs[i - 1].owner, owner) == 0) {
      free_local_msg(&c->local_msgs[i - 1]);
      memmove(&c->local_msgs[i - 1], &c->local_msgs[i], (c->nlocal_msgs - i) * sizeof *c->local_msgs);
      c->nlocal_msgs--;
      c->local_msgs_rev++;
      changed = true;
    }
  }
  for (size_t i = c->nautocompletes; i > 0; i--) {
    if (strcmp(c->autocompletes[i - 1].owner, owner) == 0) {
      free_ac(&c->autocompletes[i - 1]);
      memmove(&c->autocompletes[i - 1], &c->autocompletes[i], (c->nautocompletes - i) * sizeof *c->autocompletes);
      c->nautocompletes--;
      changed = true;
    }
  }
  if (c->has_active_dialog && strcmp(c->active_dialog.owner, owner) == 0) {
    free_dialog(&c->active_dialog);
    c->has_active_dialog = false;
    changed = true;
  }
  if (c->has_active_hitl && strcmp(c->active_hitl.owner, owner) == 0) {
    free_hitl(&c->active_hitl);
    c->has_active_hitl = false;
    changed = true;
  }
  return changed;
}

uint64_t ccord_contrib_decorators_revision(const ccord_contrib *c) { return c ? c->decors_rev : 0; }
uint64_t ccord_contrib_local_messages_revision(const ccord_contrib *c) { return c ? c->local_msgs_rev : 0; }

// ---- Decorators ----
ccord_status ccord_contrib_set_decorator(ccord_contrib *c, const ccord_decorator_contribution *spec, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !spec || !contrib_text_ok(spec->owner, CONTRIB_OWNER_MAX, false) ||
      !contrib_text_ok(spec->decor_id, CONTRIB_ITEM_MAX, false) || spec->target >= CCORD_DECOR__COUNT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad decorator");
  }
  const char *wj = spec->widget_json ? spec->widget_json : "";
  if (strlen(wj) > 65536u || !ccord_utf8_valid(wj, strlen(wj))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad decorator widget JSON");
  }
  float av_op = spec->has_avatar_opacity ? spec->avatar_opacity : 1.0f;
  if (av_op < 0.0f) av_op = 0.0f;
  if (av_op > 1.0f) av_op = 1.0f;
  float row_op = spec->has_opacity ? spec->opacity : 1.0f;
  if (row_op < 0.15f) row_op = 0.15f;
  if (row_op > 1.0f) row_op = 1.0f;
  ccord_decorator_contribution d = {
    .owner = ccord_strdup(spec->owner),
    .decor_id = ccord_strdup(spec->decor_id),
    .target = spec->target,
    .target_id = spec->target_id,
    .widget_json = ccord_strdup(wj),
    .accent_rgba = spec->accent_rgba,
    .tint_rgba = spec->tint_rgba,
    .text_rgba = spec->text_rgba,
    .author_rgba = spec->author_rgba,
    .accent_color = dup_opt(spec->accent_color),
    .tint_color = dup_opt(spec->tint_color),
    .text_color = dup_opt(spec->text_color),
    .author_color = dup_opt(spec->author_color),
    .avatar_grayscale = spec->avatar_grayscale,
    .has_avatar_opacity = spec->has_avatar_opacity,
    .avatar_opacity = av_op,
    .dim = spec->dim,
    .has_opacity = spec->has_opacity,
    .opacity = row_op,
    .collapsed = spec->collapsed,
    .collapse_label = dup_opt(spec->collapse_label),
    .priority = spec->priority,
    .revision = ++c->decors_rev,
  };
  if (!d.owner || !d.decor_id || !d.widget_json || !d.accent_color || !d.tint_color ||
      !d.text_color || !d.author_color || !d.collapse_label) {
    free_decor(&d);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
  }
  for (size_t i = 0; i < c->ndecors; i++) {
    if (strcmp(c->decors[i].owner, d.owner) == 0 && strcmp(c->decors[i].decor_id, d.decor_id) == 0) {
      free_decor(&c->decors[i]);
      c->decors[i] = d;
      c->revision++;
      return CCORD_OK;
    }
  }
  if (c->ndecors >= 256u) {
    free_decor(&d);
    return ccord_error_set(err, CCORD_ERR_RANGE, "contrib: decorator limit reached");
  }
  if (c->ndecors == c->capdecors) {
    size_t nc = c->capdecors ? c->capdecors * 2u : 16u;
    ccord_decorator_contribution *g = ccord_realloc_array(c->decors, nc, sizeof *g);
    if (!g) { free_decor(&d); return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory"); }
    c->decors = g; c->capdecors = nc;
  }
  c->decors[c->ndecors++] = d;
  c->revision++;
  return CCORD_OK;
}

bool ccord_contrib_remove_decorator(ccord_contrib *c, const char *owner, const char *decor_id)
{
  if (!c || !owner || !decor_id) return false;
  for (size_t i = 0; i < c->ndecors; i++) {
    if (strcmp(c->decors[i].owner, owner) == 0 && strcmp(c->decors[i].decor_id, decor_id) == 0) {
      free_decor(&c->decors[i]);
      memmove(&c->decors[i], &c->decors[i + 1], (c->ndecors - i - 1u) * sizeof c->decors[0]);
      c->ndecors--;
      c->decors_rev++;
      c->revision++;
      return true;
    }
  }
  return false;
}

void ccord_contrib_clear_decorators(ccord_contrib *c, const char *owner)
{
  if (!c || !owner) return;
  size_t w = 0;
  bool ch = false;
  for (size_t r = 0; r < c->ndecors; r++) {
    if (strcmp(c->decors[r].owner, owner) == 0) { free_decor(&c->decors[r]); ch = true; }
    else c->decors[w++] = c->decors[r];
  }
  c->ndecors = w;
  if (ch) { c->decors_rev++; c->revision++; }
}

size_t ccord_contrib_decorator_count(const ccord_contrib *c) { return c ? c->ndecors : 0; }
const ccord_decorator_contribution *ccord_contrib_decorator_at(const ccord_contrib *c, size_t i)
{
  return (c && i < c->ndecors) ? &c->decors[i] : nullptr;
}

// ---- Settings Schemas ----
ccord_status ccord_contrib_set_settings_schema(ccord_contrib *c, const char *owner, const char *schema_json,
                                               const char *values_json, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !contrib_text_ok(owner, CONTRIB_OWNER_MAX, false) || !schema_json || !schema_json[0]) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad settings schema");
  }
  const char *vj = values_json ? values_json : "{}";
  if (strlen(schema_json) > 131072u || strlen(vj) > 262144u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "contrib: settings schema too large");
  }
  for (size_t i = 0; i < c->nsettings; i++) {
    if (strcmp(c->settings[i].owner, owner) == 0) {
      char *sj = ccord_strdup(schema_json);
      char *vcopy = ccord_strdup(vj);
      if (!sj || !vcopy) { ccord_free(sj); ccord_free(vcopy); return ccord_error_set(err, CCORD_ERR_NOMEM, "oom"); }
      contrib_free_str(c->settings[i].schema_json);
      contrib_free_str(c->settings[i].values_json);
      c->settings[i].schema_json = sj;
      c->settings[i].values_json = vcopy;
      c->settings[i].revision++;
      c->revision++;
      return CCORD_OK;
    }
  }
  if (c->nsettings == c->capsettings) {
    size_t nc = c->capsettings ? c->capsettings * 2u : 8u;
    ccord_plugin_settings_schema *g = ccord_realloc_array(c->settings, nc, sizeof *g);
    if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
    c->settings = g; c->capsettings = nc;
  }
  ccord_plugin_settings_schema s = {
    .owner = ccord_strdup(owner),
    .schema_json = ccord_strdup(schema_json),
    .values_json = ccord_strdup(vj),
    .revision = 1,
  };
  if (!s.owner || !s.schema_json || !s.values_json) {
    free_settings(&s);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
  }
  c->settings[c->nsettings++] = s;
  c->revision++;
  return CCORD_OK;
}

void ccord_contrib_notify_setting_changed(ccord_contrib *c, const char *owner, const char *key, const char *value_json)
{
  if (!c || !owner || !key || !value_json) return;
  if (c->handlers.plugin_setting_changed) {
    c->handlers.plugin_setting_changed(c->handlers.ud, owner, key, value_json);
  }
}

const ccord_plugin_settings_schema *ccord_contrib_plugin_settings(const ccord_contrib *c, const char *owner)
{
  if (!c || !owner) return nullptr;
  for (size_t i = 0; i < c->nsettings; i++) {
    if (strcmp(c->settings[i].owner, owner) == 0) return &c->settings[i];
  }
  return nullptr;
}


// ---- Keybinds ----
ccord_status ccord_contrib_add_keybind(ccord_contrib *c, const ccord_keybind_contribution *kb, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !kb || !contrib_text_ok(kb->owner, CONTRIB_OWNER_MAX, false) ||
      !contrib_text_ok(kb->bind_id, CONTRIB_ITEM_MAX, false) || !contrib_text_ok(kb->label, CONTRIB_LABEL_MAX, false)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad keybind");
  }
  const char *def = kb->default_combo ? kb->default_combo : "";
  const char *cur = (kb->current_combo && kb->current_combo[0]) ? kb->current_combo : def;
  ccord_keybind_contribution item = {
    .owner = ccord_strdup(kb->owner),
    .bind_id = ccord_strdup(kb->bind_id),
    .label = ccord_strdup(kb->label),
    .default_combo = ccord_strdup(def),
    .current_combo = ccord_strdup(cur),
    .is_global = kb->is_global,
  };
  if (!item.owner || !item.bind_id || !item.label || !item.default_combo || !item.current_combo) {
    free_kb(&item);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
  }
  for (size_t i = 0; i < c->nkeybinds; i++) {
    if (strcmp(c->keybinds[i].owner, item.owner) == 0 && strcmp(c->keybinds[i].bind_id, item.bind_id) == 0) {
      free_kb(&c->keybinds[i]);
      c->keybinds[i] = item;
      c->revision++;
      return CCORD_OK;
    }
  }
  if (c->nkeybinds == c->capkeybinds) {
    size_t nc = c->capkeybinds ? c->capkeybinds * 2u : 8u;
    ccord_keybind_contribution *g = ccord_realloc_array(c->keybinds, nc, sizeof *g);
    if (!g) { free_kb(&item); return ccord_error_set(err, CCORD_ERR_NOMEM, "oom"); }
    c->keybinds = g; c->capkeybinds = nc;
  }
  c->keybinds[c->nkeybinds++] = item;
  c->revision++;
  return CCORD_OK;
}

bool ccord_contrib_remove_keybind(ccord_contrib *c, const char *owner, const char *bind_id)
{
  if (!c || !owner || !bind_id) return false;
  for (size_t i = 0; i < c->nkeybinds; i++) {
    if (strcmp(c->keybinds[i].owner, owner) == 0 && strcmp(c->keybinds[i].bind_id, bind_id) == 0) {
      free_kb(&c->keybinds[i]);
      memmove(&c->keybinds[i], &c->keybinds[i + 1], (c->nkeybinds - i - 1u) * sizeof c->keybinds[0]);
      c->nkeybinds--;
      c->revision++;
      return true;
    }
  }
  return false;
}

bool ccord_contrib_rebind_keybind(ccord_contrib *c, const char *owner, const char *bind_id, const char *combo)
{
  if (!c || !owner || !bind_id || !combo) return false;
  for (size_t i = 0; i < c->nkeybinds; i++) {
    if (strcmp(c->keybinds[i].owner, owner) == 0 && strcmp(c->keybinds[i].bind_id, bind_id) == 0) {
      char *cp = ccord_strdup(combo);
      if (!cp) return false;
      contrib_free_str(c->keybinds[i].current_combo);
      c->keybinds[i].current_combo = cp;
      c->revision++;
      return true;
    }
  }
  return false;
}

size_t ccord_contrib_keybind_count(const ccord_contrib *c) { return c ? c->nkeybinds : 0; }
const ccord_keybind_contribution *ccord_contrib_keybind_at(const ccord_contrib *c, size_t i)
{
  return (c && i < c->nkeybinds) ? &c->keybinds[i] : nullptr;
}

bool ccord_contrib_trigger_keybind(ccord_contrib *c, const char *owner, const char *bind_id)
{
  if (!c || !owner || !bind_id || !c->handlers.keybind_triggered) return false;
  c->handlers.keybind_triggered(c->handlers.ud, owner, bind_id);
  return true;
}

// ---- Action Buttons ----
ccord_status ccord_contrib_add_action_btn(ccord_contrib *c, const ccord_action_btn_contribution *btn, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !btn || !contrib_text_ok(btn->owner, CONTRIB_OWNER_MAX, false) ||
      !contrib_text_ok(btn->btn_id, CONTRIB_ITEM_MAX, false) || btn->slot >= CCORD_ACTBTN__COUNT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad action button");
  }
  ccord_action_btn_contribution b = {
    .owner = ccord_strdup(btn->owner),
    .btn_id = ccord_strdup(btn->btn_id),
    .slot = btn->slot,
    .label = dup_opt(btn->label),
    .icon = dup_opt(btn->icon),
    .tooltip = dup_opt(btn->tooltip),
    .badge = dup_opt(btn->badge),
  };
  if (!b.owner || !b.btn_id || !b.label || !b.icon || !b.tooltip || !b.badge) {
    free_actbtn(&b);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
  }
  for (size_t i = 0; i < c->naction_btns; i++) {
    if (strcmp(c->action_btns[i].owner, b.owner) == 0 && strcmp(c->action_btns[i].btn_id, b.btn_id) == 0) {
      free_actbtn(&c->action_btns[i]);
      c->action_btns[i] = b;
      c->revision++;
      return CCORD_OK;
    }
  }
  if (c->naction_btns == c->capaction_btns) {
    size_t nc = c->capaction_btns ? c->capaction_btns * 2u : 8u;
    ccord_action_btn_contribution *g = ccord_realloc_array(c->action_btns, nc, sizeof *g);
    if (!g) { free_actbtn(&b); return ccord_error_set(err, CCORD_ERR_NOMEM, "oom"); }
    c->action_btns = g; c->capaction_btns = nc;
  }
  c->action_btns[c->naction_btns++] = b;
  c->revision++;
  return CCORD_OK;
}

bool ccord_contrib_remove_action_btn(ccord_contrib *c, const char *owner, const char *btn_id)
{
  if (!c || !owner || !btn_id) return false;
  for (size_t i = 0; i < c->naction_btns; i++) {
    if (strcmp(c->action_btns[i].owner, owner) == 0 && strcmp(c->action_btns[i].btn_id, btn_id) == 0) {
      free_actbtn(&c->action_btns[i]);
      memmove(&c->action_btns[i], &c->action_btns[i + 1], (c->naction_btns - i - 1u) * sizeof c->action_btns[0]);
      c->naction_btns--;
      c->revision++;
      return true;
    }
  }
  return false;
}

size_t ccord_contrib_action_btn_count(const ccord_contrib *c, ccord_action_btn_slot slot)
{
  size_t n = 0;
  for (size_t i = 0; c && i < c->naction_btns; i++) {
    if (c->action_btns[i].slot == slot) n++;
  }
  return n;
}

const ccord_action_btn_contribution *ccord_contrib_action_btn_at(const ccord_contrib *c, ccord_action_btn_slot slot, size_t i)
{
  for (size_t k = 0; c && k < c->naction_btns; k++) {
    if (c->action_btns[k].slot != slot) continue;
    if (i == 0) return &c->action_btns[k];
    i--;
  }
  return nullptr;
}

bool ccord_contrib_click_action_btn(ccord_contrib *c, const char *owner, const char *btn_id, const ccord_menu_context *ctx)
{
  if (!c || !owner || !btn_id || !c->handlers.action_btn_click) return false;
  for (size_t i = 0; i < c->naction_btns; i++) {
    if (strcmp(c->action_btns[i].owner, owner) == 0 && strcmp(c->action_btns[i].btn_id, btn_id) == 0) {
      ccord_menu_context empty = {0};
      c->handlers.action_btn_click(c->handlers.ud, &c->action_btns[i], ctx ? ctx : &empty);
      return true;
    }
  }
  return false;
}

// ---- Local Ephemeral & Positioned Timeline Messages ----

static bool local_client_id_ok(const char *s)
{
  if (!s || !s[0]) return true;
  size_t n = strlen(s);
  if (n > 64u) return false;
  for (size_t i = 0; i < n; i++) {
    char ch = s[i];
    bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '.' || ch == ':' || ch == '-';
    if (!ok) return false;
  }
  return true;
}

static bool local_color_ok(const char *s)
{
  return !s || !s[0] || (strlen(s) <= 40u && ccord_utf8_valid(s, strlen(s)));
}

static float clamp_av_opacity(float v)
{
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

static bool local_msg_build(const ccord_local_message *src, uint64_t lid, uint64_t rev,
                            ccord_local_message *out, ccord_error *err)
{
  const char *cid = src->client_id ? src->client_id : "";
  const char *ej = src->embed_json ? src->embed_json : "";
  const char *aj = src->attachments_json ? src->attachments_json : "";
  const char *cnt = src->content ? src->content : "";
  if (!local_client_id_ok(cid) ||
      !local_color_ok(src->author_color) || !local_color_ok(src->badge_color) ||
      !local_color_ok(src->text_color) || !local_color_ok(src->accent_color) || !local_color_ok(src->tint_color)) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "contrib: invalid local message id or color");
    return false;
  }
  if (strlen(cnt) > 16384u || strlen(ej) > 65536u || strlen(aj) > 16384u ||
      !ccord_utf8_valid(cnt, strlen(cnt)) || !ccord_utf8_valid(ej, strlen(ej)) || !ccord_utf8_valid(aj, strlen(aj))) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "contrib: local message payload invalid or too large");
    return false;
  }
  const char *def_badge = (src->position == CCORD_LOCAL_POS_TIMELINE) ? "" : "PLUGIN";
  const char *badge = src->author_badge != nullptr ? src->author_badge : def_badge;
  const char *aname = (src->author_name && src->author_name[0]) ? src->author_name : src->owner;
  ccord_local_message m = {
    .owner = ccord_strdup(src->owner),
    .local_id = lid,
    .client_id = ccord_strdup(cid),
    .channel_id = src->channel_id,
    .position = src->position,
    .sort_id = src->sort_id,
    .after_message_id = src->after_message_id,
    .before_message_id = src->before_message_id,
    .author_id = src->author_id,
    .author_name = dup_opt(aname),
    .author_color = dup_opt(src->author_color),
    .author_badge = dup_opt(badge),
    .badge_color = dup_opt(src->badge_color),
    .content = ccord_strdup(cnt),
    .text_color = dup_opt(src->text_color),
    .accent_color = dup_opt(src->accent_color),
    .tint_color = dup_opt(src->tint_color),
    .embed_json = ccord_strdup(ej),
    .attachments_json = ccord_strdup(aj),
    .timestamp_ms = src->timestamp_ms,
    .dismissible = src->dismissible,
    .avatar_grayscale = src->avatar_grayscale,
    .has_avatar_opacity = src->has_avatar_opacity,
    .avatar_opacity = src->has_avatar_opacity ? clamp_av_opacity(src->avatar_opacity) : 1.0f,
    .compact_mode = src->compact_mode,
    .break_group = src->break_group,
    .clip_to_history = src->clip_to_history,
    .has_on_event = src->has_on_event,
    .revision = rev,
  };
  if (!m.owner || !m.client_id || !m.author_name || !m.author_color || !m.author_badge ||
      !m.badge_color || !m.content || !m.text_color || !m.accent_color || !m.tint_color ||
      !m.embed_json || !m.attachments_json) {
    free_local_msg(&m);
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
    return false;
  }
  *out = m;
  return true;
}

static bool evict_oldest_bottom(ccord_contrib *c, const char *owner_filter, ccord_id ch_filter)
{
  for (size_t i = 0; i < c->nlocal_msgs; i++) {
    if (c->local_msgs[i].position != CCORD_LOCAL_POS_BOTTOM) continue;
    if (owner_filter && strcmp(c->local_msgs[i].owner, owner_filter) != 0) continue;
    if (ch_filter != CCORD_ID_NONE && c->local_msgs[i].channel_id != ch_filter) continue;
    free_local_msg(&c->local_msgs[i]);
    memmove(&c->local_msgs[i], &c->local_msgs[i + 1], (c->nlocal_msgs - i - 1u) * sizeof c->local_msgs[0]);
    c->nlocal_msgs--;
    return true;
  }
  return false;
}

ccord_status ccord_contrib_add_local_message(ccord_contrib *c, const ccord_local_message *msg,
                                             uint64_t *out_local_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !msg || !contrib_text_ok(msg->owner, CONTRIB_OWNER_MAX, false) || msg->channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad local message");
  }
  // Upsert when (owner, client_id) already exists.
  if (msg->client_id && msg->client_id[0]) {
    for (size_t i = 0; i < c->nlocal_msgs; i++) {
      if (strcmp(c->local_msgs[i].owner, msg->owner) == 0 &&
          c->local_msgs[i].client_id && strcmp(c->local_msgs[i].client_id, msg->client_id) == 0) {
        uint64_t lid = c->local_msgs[i].local_id;
        ccord_local_message repl = {0};
        if (!local_msg_build(msg, lid, c->local_msgs_rev + 1u, &repl, err)) return err ? err->code : CCORD_ERR_INVALID;
        free_local_msg(&c->local_msgs[i]);
        c->local_msgs[i] = repl;
        c->local_msgs_rev++;
        c->revision++;
        if (out_local_id) *out_local_id = lid;
        return CCORD_OK;
      }
    }
  }
  // Enforce capacity: 1024 global, 256 per owner, 256 per channel.
  enum { MAX_LOCAL_GLOBAL = 1024u, MAX_LOCAL_OWNER = 256u, MAX_LOCAL_CHANNEL = 256u };
  size_t owner_cnt = 0, ch_cnt = 0;
  for (size_t i = 0; i < c->nlocal_msgs; i++) {
    if (strcmp(c->local_msgs[i].owner, msg->owner) == 0) owner_cnt++;
    if (c->local_msgs[i].channel_id == msg->channel_id) ch_cnt++;
  }
  if (owner_cnt >= MAX_LOCAL_OWNER) {
    if (msg->position != CCORD_LOCAL_POS_BOTTOM || !evict_oldest_bottom(c, msg->owner, CCORD_ID_NONE)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "contrib: per-plugin local message limit reached (256)");
    }
  }
  if (ch_cnt >= MAX_LOCAL_CHANNEL) {
    if (msg->position != CCORD_LOCAL_POS_BOTTOM || !evict_oldest_bottom(c, nullptr, msg->channel_id)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "contrib: per-channel local message limit reached (256)");
    }
  }
  if (c->nlocal_msgs >= MAX_LOCAL_GLOBAL) {
    if (msg->position != CCORD_LOCAL_POS_BOTTOM || !evict_oldest_bottom(c, nullptr, CCORD_ID_NONE)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "contrib: global local message limit reached (1024)");
    }
  }
  if (c->nlocal_msgs == c->caplocal_msgs) {
    size_t nc = c->caplocal_msgs ? c->caplocal_msgs * 2u : 8u;
    ccord_local_message *g = ccord_realloc_array(c->local_msgs, nc, sizeof *g);
    if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
    c->local_msgs = g; c->caplocal_msgs = nc;
  }
  uint64_t lid = ++c->next_local_id;
  ccord_local_message m = {0};
  if (!local_msg_build(msg, lid, c->local_msgs_rev + 1u, &m, err)) return err ? err->code : CCORD_ERR_INVALID;
  c->local_msgs[c->nlocal_msgs++] = m;
  c->local_msgs_rev++;
  c->revision++;
  if (out_local_id) *out_local_id = lid;
  return CCORD_OK;
}

static size_t find_local_idx_owned(const ccord_contrib *c, const char *owner, uint64_t local_id, const char *client_id)
{
  if (!c || !owner) return SIZE_MAX;
  for (size_t i = 0; i < c->nlocal_msgs; i++) {
    if (strcmp(c->local_msgs[i].owner, owner) != 0) continue;
    if (local_id != 0 && c->local_msgs[i].local_id == local_id) return i;
    if (client_id && client_id[0] && c->local_msgs[i].client_id &&
        strcmp(c->local_msgs[i].client_id, client_id) == 0) return i;
  }
  return SIZE_MAX;
}

ccord_status ccord_contrib_update_local_message(ccord_contrib *c, const char *owner,
                                                uint64_t local_id, const char *client_id,
                                                const ccord_local_message *patch, uint32_t patch_mask,
                                                uint64_t *out_local_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !owner || !patch) return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad update_local args");
  size_t idx = find_local_idx_owned(c, owner, local_id, client_id);
  if (idx == SIZE_MAX) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "contrib: local message not found");
  ccord_local_message merged = c->local_msgs[idx];
  if (patch_mask & CCORD_LPATCH_CHANNEL) merged.channel_id = patch->channel_id;
  if (patch_mask & CCORD_LPATCH_POSITION) merged.position = patch->position;
  if (patch_mask & CCORD_LPATCH_SORT_ID) merged.sort_id = patch->sort_id;
  if (patch_mask & CCORD_LPATCH_AFTER_ID) merged.after_message_id = patch->after_message_id;
  if (patch_mask & CCORD_LPATCH_BEFORE_ID) merged.before_message_id = patch->before_message_id;
  if (patch_mask & CCORD_LPATCH_AUTHOR_ID) merged.author_id = patch->author_id;
  if (patch_mask & CCORD_LPATCH_AUTHOR_NAME) merged.author_name = patch->author_name;
  if (patch_mask & CCORD_LPATCH_AUTHOR_COLOR) merged.author_color = patch->author_color;
  if (patch_mask & CCORD_LPATCH_BADGE) merged.author_badge = patch->author_badge ? patch->author_badge : "";
  if (patch_mask & CCORD_LPATCH_BADGE_COLOR) merged.badge_color = patch->badge_color;
  if (patch_mask & CCORD_LPATCH_CONTENT) merged.content = patch->content;
  if (patch_mask & CCORD_LPATCH_TEXT_COLOR) merged.text_color = patch->text_color;
  if (patch_mask & CCORD_LPATCH_ACCENT_COLOR) merged.accent_color = patch->accent_color;
  if (patch_mask & CCORD_LPATCH_TINT_COLOR) merged.tint_color = patch->tint_color;
  if (patch_mask & CCORD_LPATCH_EMBED_JSON) merged.embed_json = patch->embed_json;
  if (patch_mask & CCORD_LPATCH_ATTACHMENTS_JSON) merged.attachments_json = patch->attachments_json;
  if (patch_mask & CCORD_LPATCH_TIMESTAMP) merged.timestamp_ms = patch->timestamp_ms;
  if (patch_mask & CCORD_LPATCH_DISMISSIBLE) merged.dismissible = patch->dismissible;
  if (patch_mask & CCORD_LPATCH_AVATAR_GRAYSCALE) merged.avatar_grayscale = patch->avatar_grayscale;
  if (patch_mask & CCORD_LPATCH_AVATAR_OPACITY) {
    merged.has_avatar_opacity = patch->has_avatar_opacity;
    merged.avatar_opacity = patch->avatar_opacity;
  }
  if (patch_mask & CCORD_LPATCH_COMPACT) merged.compact_mode = patch->compact_mode;
  if (patch_mask & CCORD_LPATCH_BREAK_GROUP) merged.break_group = patch->break_group;
  if (patch_mask & CCORD_LPATCH_CLIP_TO_HISTORY) merged.clip_to_history = patch->clip_to_history;
  if (patch_mask & CCORD_LPATCH_ON_EVENT) merged.has_on_event = patch->has_on_event;
  if (merged.channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad channel_id");
  ccord_local_message repl = {0};
  if (!local_msg_build(&merged, c->local_msgs[idx].local_id, c->local_msgs_rev + 1u, &repl, err)) {
    return err ? err->code : CCORD_ERR_INVALID;
  }
  free_local_msg(&c->local_msgs[idx]);
  c->local_msgs[idx] = repl;
  c->local_msgs_rev++;
  c->revision++;
  if (out_local_id) *out_local_id = repl.local_id;
  return CCORD_OK;
}

bool ccord_contrib_dismiss_local_message(ccord_contrib *c, uint64_t local_id)
{
  if (!c || !local_id) return false;
  for (size_t i = 0; i < c->nlocal_msgs; i++) {
    if (c->local_msgs[i].local_id == local_id) {
      free_local_msg(&c->local_msgs[i]);
      memmove(&c->local_msgs[i], &c->local_msgs[i + 1], (c->nlocal_msgs - i - 1u) * sizeof c->local_msgs[0]);
      c->nlocal_msgs--;
      c->local_msgs_rev++;
      c->revision++;
      return true;
    }
  }
  return false;
}

bool ccord_contrib_dismiss_local_message_owned(ccord_contrib *c, const char *owner,
                                               uint64_t local_id, const char *client_id, uint64_t *out_local_id)
{
  size_t idx = find_local_idx_owned(c, owner, local_id, client_id);
  if (idx == SIZE_MAX) return false;
  uint64_t lid = c->local_msgs[idx].local_id;
  free_local_msg(&c->local_msgs[idx]);
  memmove(&c->local_msgs[idx], &c->local_msgs[idx + 1], (c->nlocal_msgs - idx - 1u) * sizeof c->local_msgs[0]);
  c->nlocal_msgs--;
  c->local_msgs_rev++;
  c->revision++;
  if (out_local_id) *out_local_id = lid;
  return true;
}

size_t ccord_contrib_clear_local_messages(ccord_contrib *c, const char *owner, ccord_id channel_id)
{
  if (!c || !owner) return 0;
  size_t removed = 0;
  for (size_t i = c->nlocal_msgs; i > 0; i--) {
    ccord_local_message *m = &c->local_msgs[i - 1u];
    if (strcmp(m->owner, owner) != 0) continue;
    if (channel_id != CCORD_ID_NONE && m->channel_id != channel_id) continue;
    free_local_msg(m);
    memmove(&c->local_msgs[i - 1u], &c->local_msgs[i], (c->nlocal_msgs - i) * sizeof c->local_msgs[0]);
    c->nlocal_msgs--;
    removed++;
  }
  if (removed > 0) {
    c->local_msgs_rev++;
    c->revision++;
  }
  return removed;
}

const ccord_local_message *ccord_contrib_find_local_message(const ccord_contrib *c, uint64_t local_id)
{
  if (!c || !local_id) return nullptr;
  for (size_t i = 0; i < c->nlocal_msgs; i++) {
    if (c->local_msgs[i].local_id == local_id) return &c->local_msgs[i];
  }
  return nullptr;
}

const ccord_local_message *ccord_contrib_find_local_message_owned(const ccord_contrib *c, const char *owner,
                                                                  uint64_t local_id, const char *client_id)
{
  size_t idx = find_local_idx_owned(c, owner, local_id, client_id);
  return idx != SIZE_MAX ? &c->local_msgs[idx] : nullptr;
}

bool ccord_contrib_dispatch_local_msg_event(ccord_contrib *c, uint64_t local_id,
                                            const char *ev_type, float x, float y)
{
  if (!c || !local_id || !ev_type || !c->handlers.local_msg_event) return false;
  const ccord_local_message *m = ccord_contrib_find_local_message(c, local_id);
  if (!m || !m->has_on_event) return false;
  c->handlers.local_msg_event(c->handlers.ud, m, ev_type, x, y);
  return true;
}

size_t ccord_contrib_local_message_count(const ccord_contrib *c, ccord_id channel_id)
{
  size_t n = 0;
  for (size_t i = 0; c && i < c->nlocal_msgs; i++) {
    if (channel_id == CCORD_ID_NONE || c->local_msgs[i].channel_id == channel_id) n++;
  }
  return n;
}

const ccord_local_message *ccord_contrib_local_message_at(const ccord_contrib *c, ccord_id channel_id, size_t i)
{
  for (size_t k = 0; c && k < c->nlocal_msgs; k++) {
    if (channel_id != CCORD_ID_NONE && c->local_msgs[k].channel_id != channel_id) continue;
    if (i == 0) return &c->local_msgs[k];
    i--;
  }
  return nullptr;
}

// ---- Autocomplete Providers ----
ccord_status ccord_contrib_add_autocomplete(ccord_contrib *c, const ccord_autocomplete_provider *prov, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !prov || !contrib_text_ok(prov->owner, CONTRIB_OWNER_MAX, false) ||
      !contrib_text_ok(prov->provider_id, CONTRIB_ITEM_MAX, false) || !contrib_text_ok(prov->trigger, 16u, false)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad autocomplete provider");
  }
  ccord_autocomplete_provider a = {
    .owner = ccord_strdup(prov->owner),
    .provider_id = ccord_strdup(prov->provider_id),
    .trigger = ccord_strdup(prov->trigger),
    .description = dup_opt(prov->description),
  };
  if (!a.owner || !a.provider_id || !a.trigger || !a.description) {
    free_ac(&a);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
  }
  for (size_t i = 0; i < c->nautocompletes; i++) {
    if (strcmp(c->autocompletes[i].owner, a.owner) == 0 && strcmp(c->autocompletes[i].provider_id, a.provider_id) == 0) {
      free_ac(&c->autocompletes[i]);
      c->autocompletes[i] = a;
      c->revision++;
      return CCORD_OK;
    }
  }
  if (c->nautocompletes == c->capautocompletes) {
    size_t nc = c->capautocompletes ? c->capautocompletes * 2u : 8u;
    ccord_autocomplete_provider *g = ccord_realloc_array(c->autocompletes, nc, sizeof *g);
    if (!g) { free_ac(&a); return ccord_error_set(err, CCORD_ERR_NOMEM, "oom"); }
    c->autocompletes = g; c->capautocompletes = nc;
  }
  c->autocompletes[c->nautocompletes++] = a;
  c->revision++;
  return CCORD_OK;
}

bool ccord_contrib_remove_autocomplete(ccord_contrib *c, const char *owner, const char *provider_id)
{
  if (!c || !owner || !provider_id) return false;
  for (size_t i = 0; i < c->nautocompletes; i++) {
    if (strcmp(c->autocompletes[i].owner, owner) == 0 && strcmp(c->autocompletes[i].provider_id, provider_id) == 0) {
      free_ac(&c->autocompletes[i]);
      memmove(&c->autocompletes[i], &c->autocompletes[i + 1], (c->nautocompletes - i - 1u) * sizeof c->autocompletes[0]);
      c->nautocompletes--;
      c->revision++;
      return true;
    }
  }
  return false;
}

size_t ccord_contrib_autocomplete_count(const ccord_contrib *c) { return c ? c->nautocompletes : 0; }
const ccord_autocomplete_provider *ccord_contrib_autocomplete_at(const ccord_contrib *c, size_t i)
{
  return (c && i < c->nautocompletes) ? &c->autocompletes[i] : nullptr;
}

size_t ccord_contrib_query_autocomplete(ccord_contrib *c, const char *owner, const char *provider_id, const char *query,
                                        ccord_id guild_id, ccord_id channel_id, ccord_autocomplete_item *out, size_t max_items)
{
  if (!c || !owner || !provider_id || !out || max_items == 0 || !c->handlers.autocomplete_query) return 0;
  return c->handlers.autocomplete_query(c->handlers.ud, owner, provider_id, query ? query : "", guild_id, channel_id,
                                        out, max_items);
}


// ---- User-Initiated Dialogs ----
ccord_status ccord_contrib_open_dialog(ccord_contrib *c, const ccord_plugin_dialog *dlg, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !dlg || !contrib_text_ok(dlg->owner, CONTRIB_OWNER_MAX, false) || !dlg->req_id ||
      !contrib_text_ok(dlg->title, 128u, false)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad dialog");
  }
  if (c->has_active_dialog) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "contrib: another plugin dialog is already open");
  }
  ccord_plugin_dialog d = {
    .owner = ccord_strdup(dlg->owner),
    .plugin_name = dup_opt(dlg->plugin_name && dlg->plugin_name[0] ? dlg->plugin_name : dlg->owner),
    .req_id = dlg->req_id,
    .kind = dlg->kind,
    .title = ccord_strdup(dlg->title),
    .message = dup_opt(dlg->message),
    .confirm_label = dup_opt(dlg->confirm_label && dlg->confirm_label[0] ? dlg->confirm_label : "OK"),
    .cancel_label = dup_opt(dlg->cancel_label && dlg->cancel_label[0] ? dlg->cancel_label : "Cancel"),
    .default_value = dup_opt(dlg->default_value),
    .fields_json = dup_opt(dlg->fields_json),
  };
  if (!d.owner || !d.plugin_name || !d.title || !d.message || !d.confirm_label ||
      !d.cancel_label || !d.default_value || !d.fields_json) {
    free_dialog(&d);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
  }
  c->active_dialog = d;
  c->has_active_dialog = true;
  c->revision++;
  return CCORD_OK;
}

const ccord_plugin_dialog *ccord_contrib_active_dialog(const ccord_contrib *c)
{
  return (c && c->has_active_dialog) ? &c->active_dialog : nullptr;
}

void ccord_contrib_resolve_dialog(ccord_contrib *c, uint64_t req_id, bool accepted, const char *result_json)
{
  if (!c || !c->has_active_dialog || c->active_dialog.req_id != req_id) return;
  char owner[65];
  snprintf(owner, sizeof owner, "%s", c->active_dialog.owner ? c->active_dialog.owner : "");
  free_dialog(&c->active_dialog);
  c->has_active_dialog = false;
  c->revision++;
  if (c->handlers.dialog_result) {
    c->handlers.dialog_result(c->handlers.ud, owner, req_id, accepted, result_json ? result_json : "null");
  }
}

// ---- Class 3 HITL Account Change Requests & Streamer Mode ----
ccord_status ccord_contrib_open_account_hitl(ccord_contrib *c, const ccord_account_hitl_req *req, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !req || !contrib_text_ok(req->owner, CONTRIB_OWNER_MAX, false) || !req->req_id ||
      !contrib_text_ok(req->summary, 256u, false)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad account confirmation request");
  }
  if (c->has_active_hitl) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "contrib: another account confirmation modal is already open");
  }
  ccord_account_hitl_req h = {
    .owner = ccord_strdup(req->owner),
    .plugin_name = dup_opt(req->plugin_name && req->plugin_name[0] ? req->plugin_name : req->owner),
    .req_id = req->req_id,
    .kind = req->kind,
    .target_value = dup_opt(req->target_value),
    .summary = ccord_strdup(req->summary),
  };
  if (!h.owner || !h.plugin_name || !h.target_value || !h.summary) {
    free_hitl(&h);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
  }
  c->active_hitl = h;
  c->has_active_hitl = true;
  c->revision++;
  return CCORD_OK;
}

const ccord_account_hitl_req *ccord_contrib_active_account_hitl(const ccord_contrib *c)
{
  return (c && c->has_active_hitl) ? &c->active_hitl : nullptr;
}

void ccord_contrib_resolve_account_hitl(ccord_contrib *c, uint64_t req_id, bool approved, const char *err_msg)
{
  if (!c || !c->has_active_hitl || c->active_hitl.req_id != req_id) return;
  char owner[65];
  snprintf(owner, sizeof owner, "%s", c->active_hitl.owner ? c->active_hitl.owner : "");
  free_hitl(&c->active_hitl);
  c->has_active_hitl = false;
  c->revision++;
  if (c->handlers.account_hitl_result) {
    c->handlers.account_hitl_result(c->handlers.ud, owner, req_id, approved, err_msg);
  }
}

void ccord_contrib_set_streamer_mode(ccord_contrib *c, bool enabled)
{
  if (!c) return;
  c->streamer_mode = enabled;
}

bool ccord_contrib_streamer_mode(const ccord_contrib *c)
{
  return c && c->streamer_mode;
}

// ---- Dispatchers & UI hook forwarders ----
size_t ccord_contrib_complete_command(ccord_contrib *c, const char *name, const char *args, ccord_id guild_id,
                                      ccord_id channel_id, ccord_autocomplete_item *out, size_t max_items)
{
  if (!c || !name || !out || max_items == 0 || !c->handlers.command_complete) return 0;
  const ccord_command_contribution *cmd = ccord_contrib_find_command(c, name);
  if (!cmd || !cmd->has_autocomplete) return 0;
  return c->handlers.command_complete(c->handlers.ud, cmd, args ? args : "", guild_id, channel_id, out, max_items);
}

ccord_status ccord_contrib_dispatch_decor_event(ccord_contrib *c, const char *owner, const char *decor_id,
                                                ccord_id target_id, const ccord_panel_event *ev, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !owner || !decor_id || !ev) return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: null argument");
  for (size_t i = 0; i < c->ndecors; i++) {
    if (strcmp(c->decors[i].owner, owner) == 0 && strcmp(c->decors[i].decor_id, decor_id) == 0) {
      if (!c->handlers.decor_event) return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "no decor handler");
      c->handlers.decor_event(c->handlers.ud, &c->decors[i], target_id, ev);
      return CCORD_OK;
    }
  }
  return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "contrib: no such decorator");
}

bool ccord_contrib_run_before_send(ccord_contrib *c, ccord_id channel_id, ccord_id reply_to_id, const char *text,
                                   char *out_text, size_t out_cap, char *cancel_reason, size_t reason_cap)
{
  if (out_text && out_cap) snprintf(out_text, out_cap, "%s", text ? text : "");
  if (cancel_reason && reason_cap) cancel_reason[0] = '\0';
  if (!c || !c->handlers.before_send_hook) return true;
  return c->handlers.before_send_hook(c->handlers.ud, channel_id, reply_to_id, text ? text : "",
                                      out_text, out_cap, cancel_reason, reason_cap);
}

bool ccord_contrib_run_before_edit(ccord_contrib *c, ccord_id channel_id, ccord_id message_id, const char *text,
                                   char *out_text, size_t out_cap, char *cancel_reason, size_t reason_cap)
{
  if (out_text && out_cap) snprintf(out_text, out_cap, "%s", text ? text : "");
  if (cancel_reason && reason_cap) cancel_reason[0] = '\0';
  if (!c || !c->handlers.before_edit_hook) return true;
  return c->handlers.before_edit_hook(c->handlers.ud, channel_id, message_id, text ? text : "",
                                      out_text, out_cap, cancel_reason, reason_cap);
}

bool ccord_contrib_run_notification_hook(ccord_contrib *c, ccord_id guild_id, ccord_id channel_id, ccord_id message_id,
                                         ccord_id author_id, const char *content, bool is_mention, bool is_dm,
                                         char *out_title, size_t title_cap, char *out_body, size_t body_cap, bool *out_silent)
{
  if (!c || !c->handlers.notification_hook) return true;
  return c->handlers.notification_hook(c->handlers.ud, guild_id, channel_id, message_id, author_id,
                                       content ? content : "", is_mention, is_dm,
                                       out_title, title_cap, out_body, body_cap, out_silent);
}

bool ccord_contrib_run_link_open_hook(ccord_contrib *c, const char *url, char *out_url, size_t out_cap)
{
  if (out_url && out_cap) snprintf(out_url, out_cap, "%s", url ? url : "");
  if (!c || !url || !c->handlers.link_open_hook) return true;
  return c->handlers.link_open_hook(c->handlers.ud, url, out_url, out_cap);
}

bool ccord_contrib_ui_composer_get(ccord_contrib *c, char *text_out, size_t text_cap, size_t *sel_start, size_t *sel_end,
                                   ccord_id *reply_id, bool *reply_ping)
{
  if (!c || !c->ui_hooks.composer_get) return false;
  return c->ui_hooks.composer_get(c->ui_hooks.ud, text_out, text_cap, sel_start, sel_end, reply_id, reply_ping);
}

bool ccord_contrib_ui_composer_set(ccord_contrib *c, const char *text, bool insert_mode, int pos)
{
  if (!c || !c->ui_hooks.composer_set) return false;
  return c->ui_hooks.composer_set(c->ui_hooks.ud, text ? text : "", insert_mode, pos);
}

bool ccord_contrib_ui_composer_set_selection(ccord_contrib *c, size_t start, size_t end)
{
  if (!c || !c->ui_hooks.composer_set_selection) return false;
  return c->ui_hooks.composer_set_selection(c->ui_hooks.ud, start, end);
}

bool ccord_contrib_ui_composer_set_reply(ccord_contrib *c, ccord_id msg_id, bool ping)
{
  if (!c || !c->ui_hooks.composer_set_reply) return false;
  return c->ui_hooks.composer_set_reply(c->ui_hooks.ud, msg_id, ping);
}

bool ccord_contrib_ui_composer_attach_bytes(ccord_contrib *c, const char *filename, const uint8_t *data, size_t len, bool spoiler)
{
  if (!c || !filename || !data || !len || !c->ui_hooks.composer_attach_bytes) return false;
  return c->ui_hooks.composer_attach_bytes(c->ui_hooks.ud, filename, data, len, spoiler);
}

bool ccord_contrib_ui_measure_text(ccord_contrib *c, const char *utf8, float size_px, bool bold, float wrap_w, float *out_w, float *out_h)
{
  if (!c || !utf8 || !c->ui_hooks.measure_text) return false;
  return c->ui_hooks.measure_text(c->ui_hooks.ud, utf8, size_px, bold, wrap_w, out_w, out_h);
}

bool ccord_contrib_ui_window_set_title(ccord_contrib *c, const char *title)
{
  if (!c || !c->ui_hooks.window_set_title) return false;
  return c->ui_hooks.window_set_title(c->ui_hooks.ud, title ? title : "");
}

bool ccord_contrib_ui_window_set_badge(ccord_contrib *c, int count)
{
  if (!c || !c->ui_hooks.window_set_badge) return false;
  return c->ui_hooks.window_set_badge(c->ui_hooks.ud, count);
}

bool ccord_contrib_ui_window_flash(ccord_contrib *c)
{
  if (!c || !c->ui_hooks.window_flash) return false;
  return c->ui_hooks.window_flash(c->ui_hooks.ud);
}

bool ccord_contrib_ui_window_is_focused(const ccord_contrib *c)
{
  if (!c || !c->ui_hooks.window_is_focused) return false;
  return c->ui_hooks.window_is_focused(c->ui_hooks.ud);
}

bool ccord_contrib_ui_play_earcon(ccord_contrib *c, const char *kind_name)
{
  if (!c || !kind_name || !c->ui_hooks.play_earcon) return false;
  return c->ui_hooks.play_earcon(c->ui_hooks.ud, kind_name);
}

bool ccord_contrib_ui_play_audio_bytes(ccord_contrib *c, const uint8_t *data, size_t len)
{
  if (!c || !data || !len || !c->ui_hooks.play_audio_bytes) return false;
  return c->ui_hooks.play_audio_bytes(c->ui_hooks.ud, data, len);
}

bool ccord_contrib_ui_file_pick(ccord_contrib *c, const char *title, const char *filters_json, bool multiple,
                                char *out_paths_json, size_t cap)
{
  if (!c || !c->ui_hooks.file_pick) return false;
  return c->ui_hooks.file_pick(c->ui_hooks.ud, title ? title : "", filters_json ? filters_json : "", multiple, out_paths_json, cap);
}

bool ccord_contrib_ui_file_save(ccord_contrib *c, const char *suggested_name, const uint8_t *data, size_t len,
                                char *out_path, size_t cap)
{
  if (!c || !c->ui_hooks.file_save) return false;
  return c->ui_hooks.file_save(c->ui_hooks.ud, suggested_name ? suggested_name : "", data, len, out_path, cap);
}

bool ccord_contrib_ui_select_theme_preset(ccord_contrib *c, const char *owner, const char *preset_id)
{
  if (!c || !c->ui_hooks.select_theme_preset) return false;
  return c->ui_hooks.select_theme_preset(c->ui_hooks.ud, owner ? owner : "", preset_id ? preset_id : "");
}
