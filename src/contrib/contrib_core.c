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

void contrib_free_str(const char *s)
{
  union { const char *c; void *v; } u = {.c = s};
  ccord_free(u.v);
}

static void free_str(const char *s) { contrib_free_str(s); }

static void free_menu(ccord_menu_contribution *m)
{
  free_str(m->owner);
  free_str(m->item_id);
  free_str(m->label);
  free_str(m->icon);
  free_str(m->submenu);
  free_str(m->when_json);
}

static void free_cmd(ccord_command_contribution *c)
{
  free_str(c->owner);
  free_str(c->name);
  free_str(c->description);
  free_str(c->schema_json);
}

static void free_panel(ccord_panel_contribution *p)
{
  free_str(p->owner);
  free_str(p->panel_id);
  free_str(p->title);
  free_str(p->icon);
  free_str(p->root_json);
}


bool contrib_text_ok(const char *s, size_t max, bool allow_empty)
{
  if (!s) return allow_empty;
  size_t n = strlen(s);
  if (n == 0) return allow_empty;
  return n <= max && ccord_utf8_valid_text(s, n);
}

bool contrib_name_ok(const char *s)
{
  if (!s || s[0] == '\0' || strlen(s) > CONTRIB_CMD_MAX) return false;
  for (const char *p = s; *p; p++) {
    bool ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
    if (!ok) return false;
  }
  return true;
}

static bool text_ok(const char *s, size_t max, bool allow_empty) { return contrib_text_ok(s, max, allow_empty); }
static bool name_ok(const char *s) { return contrib_name_ok(s); }

ccord_contrib *ccord_contrib_create(ccord_error *err)
{
  ccord_error_clear(err);
  ccord_contrib *c = ccord_calloc(1, sizeof *c);
  if (!c) ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
  else {
    c->catalog_prefs.auto_update_enabled = false;
    c->catalog_prefs.auto_update_min_tier = 2;
  }
  return c;
}

void ccord_contrib_destroy(ccord_contrib *c)
{
  if (!c) return;
  for (size_t i = 0; i < c->nmenus; i++) free_menu(&c->menus[i]);
  for (size_t i = 0; i < c->ncmds; i++) free_cmd(&c->cmds[i]);
  for (size_t i = 0; i < c->npanels; i++) free_panel(&c->panels[i]);
  contrib_ext_destroy(c);
  for (size_t i = 0; i < c->nplugins; i++) contrib_free_plugin(&c->plugins[i]);
  for (size_t i = 0; i < c->ncatalog; i++) contrib_free_catalog_entry(&c->catalog[i]);
  for (size_t i = 0; i < c->ncatalog_updates; i++) contrib_free_catalog_entry(&c->catalog_updates[i]);
  if (c->has_catalog_detail) contrib_free_catalog_entry(&c->catalog_detail);
  ccord_free(c->menus);
  ccord_free(c->cmds);
  ccord_free(c->panels);
  ccord_free(c->plugins);
  ccord_free(c->catalog);
  ccord_free(c->catalog_updates);
  ccord_free(c);
}

uint64_t ccord_contrib_revision(const ccord_contrib *c)
{
  return c ? c->revision : 0;
}

ccord_status ccord_contrib_add_menu_ex(ccord_contrib *c, const ccord_menu_contribution *spec, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !spec || !text_ok(spec->owner, CONTRIB_OWNER_MAX, false) ||
      !text_ok(spec->item_id, CONTRIB_ITEM_MAX, false) ||
      !text_ok(spec->label, CONTRIB_LABEL_MAX, false) || spec->target >= CCORD_MENU__COUNT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad menu contribution");
  }
  for (size_t i = 0; i < c->nmenus; i++) {
    if (strcmp(c->menus[i].owner, spec->owner) == 0 && strcmp(c->menus[i].item_id, spec->item_id) == 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: menu item already registered by owner '%s'", spec->owner);
    }
  }
  if (c->nmenus == c->capmenus) {
    size_t ncap = c->capmenus ? c->capmenus * 2 : 8;
    ccord_menu_contribution *g = ccord_realloc_array(c->menus, ncap, sizeof *g);
    if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
    c->menus = g;
    c->capmenus = ncap;
  }
  ccord_menu_contribution m = {
    .owner = ccord_strdup(spec->owner),
    .item_id = ccord_strdup(spec->item_id),
    .label = ccord_strdup(spec->label),
    .target = spec->target,
    .icon = ccord_strdup(spec->icon ? spec->icon : ""),
    .submenu = ccord_strdup(spec->submenu ? spec->submenu : ""),
    .when_json = ccord_strdup(spec->when_json ? spec->when_json : ""),
    .danger = spec->danger,
    .checked = spec->checked,
    .divider_before = spec->divider_before,
  };
  if (!m.owner || !m.item_id || !m.label || !m.icon || !m.submenu || !m.when_json) {
    free_menu(&m);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
  }
  c->menus[c->nmenus++] = m;
  c->revision++;
  return CCORD_OK;
}

ccord_status ccord_contrib_add_menu(ccord_contrib *c, const char *owner, const char *item_id, const char *label,
                                    ccord_menu_target t, ccord_error *err)
{
  ccord_menu_contribution spec = {.owner = owner, .item_id = item_id, .label = label, .target = t};
  return ccord_contrib_add_menu_ex(c, &spec, err);
}

bool ccord_contrib_remove_menu(ccord_contrib *c, const char *owner, const char *item_id)
{
  if (!c || !owner || !item_id) return false;
  for (size_t i = 0; i < c->nmenus; i++) {
    if (strcmp(c->menus[i].owner, owner) == 0 && strcmp(c->menus[i].item_id, item_id) == 0) {
      free_menu(&c->menus[i]);
      memmove(&c->menus[i], &c->menus[i + 1], (c->nmenus - i - 1) * sizeof c->menus[0]);
      c->nmenus--;
      c->revision++;
      return true;
    }
  }
  return false;
}

ccord_status ccord_contrib_add_command_ex(ccord_contrib *c, const char *owner, const char *name,
                                          const char *description, const char *schema_json,
                                          bool has_autocomplete, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !text_ok(owner, CONTRIB_OWNER_MAX, false) || !name_ok(name) ||
      !text_ok(description, CONTRIB_DESC_MAX, true)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad command contribution");
  }
  for (size_t i = 0; i < c->ncmds; i++) {
    if (strcmp(c->cmds[i].name, name) == 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: command name already registered by owner '%s'",
                             c->cmds[i].owner);
    }
  }
  if (c->ncmds == c->capcmds) {
    size_t ncap = c->capcmds ? c->capcmds * 2 : 8;
    ccord_command_contribution *g = ccord_realloc_array(c->cmds, ncap, sizeof *g);
    if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
    c->cmds = g;
    c->capcmds = ncap;
  }
  ccord_command_contribution cmd = {
    .owner = ccord_strdup(owner),
    .name = ccord_strdup(name),
    .description = ccord_strdup(description ? description : ""),
    .schema_json = ccord_strdup(schema_json ? schema_json : ""),
    .has_autocomplete = has_autocomplete,
  };
  if (!cmd.owner || !cmd.name || !cmd.description || !cmd.schema_json) {
    free_cmd(&cmd);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
  }
  c->cmds[c->ncmds++] = cmd;
  c->revision++;
  return CCORD_OK;
}

ccord_status ccord_contrib_add_command(ccord_contrib *c, const char *owner, const char *name, const char *description,
                                       ccord_error *err)
{
  return ccord_contrib_add_command_ex(c, owner, name, description, "", false, err);
}

bool ccord_contrib_remove_command(ccord_contrib *c, const char *owner, const char *name)
{
  if (!c || !owner || !name) return false;
  for (size_t i = 0; i < c->ncmds; i++) {
    if (strcmp(c->cmds[i].owner, owner) == 0 && strcmp(c->cmds[i].name, name) == 0) {
      free_cmd(&c->cmds[i]);
      memmove(&c->cmds[i], &c->cmds[i + 1], (c->ncmds - i - 1u) * sizeof c->cmds[0]);
      c->ncmds--;
      c->revision++;
      return true;
    }
  }
  return false;
}

ccord_status ccord_contrib_set_panel_ex(ccord_contrib *c, const char *owner, const char *panel_id,
                                        const ccord_panel_spec *spec, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !spec || !text_ok(owner, CONTRIB_OWNER_MAX, false) || !name_ok(panel_id) ||
      spec->slot >= CCORD_PANEL__COUNT || !spec->root_json) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad panel contribution");
  }
  float h = spec->height > 0.0f ? spec->height : 34.0f;
  if (!(h >= 12.0f && h <= 2048.0f)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad panel height");
  }
  size_t jlen = strlen(spec->root_json);
  if (jlen == 0 || jlen > CONTRIB_PANEL_JSON_MAX || !ccord_utf8_valid(spec->root_json, jlen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad panel JSON");
  }
  size_t owner_panels = 0;
  for (size_t i = 0; i < c->npanels; i++) {
    if (strcmp(c->panels[i].owner, owner) == 0) {
      if (strcmp(c->panels[i].panel_id, panel_id) == 0) {
        char *rj = ccord_strdup(spec->root_json);
        char *ti = ccord_strdup(spec->title ? spec->title : "");
        char *ic = ccord_strdup(spec->icon ? spec->icon : "");
        if (!rj || !ti || !ic) { ccord_free(rj); ccord_free(ti); ccord_free(ic); return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory"); }
        free_str(c->panels[i].root_json);
        free_str(c->panels[i].title);
        free_str(c->panels[i].icon);
        c->panels[i].slot = spec->slot;
        c->panels[i].height = h;
        c->panels[i].width = spec->width;
        c->panels[i].x = spec->x;
        c->panels[i].y = spec->y;
        c->panels[i].title = ti;
        c->panels[i].icon = ic;
        c->panels[i].visible = spec->visible;
        c->panels[i].root_json = rj;
        c->panels[i].revision = ++c->next_panel_rev;
        c->revision++;
        return CCORD_OK;
      }
      owner_panels++;
    }
  }
  if (owner_panels >= 16u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "contrib: plugin panel limit (16) reached");
  }
  if (c->npanels >= 64u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "contrib: global panel limit (64) reached");
  }
  if (c->npanels == c->cappanels) {
    size_t ncap = c->cappanels ? c->cappanels * 2 : 4;
    ccord_panel_contribution *g = ccord_realloc_array(c->panels, ncap, sizeof *g);
    if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
    c->panels = g;
    c->cappanels = ncap;
  }
  ccord_panel_contribution p = {
    .owner = ccord_strdup(owner),
    .panel_id = ccord_strdup(panel_id),
    .slot = spec->slot,
    .height = h,
    .width = spec->width,
    .x = spec->x,
    .y = spec->y,
    .title = ccord_strdup(spec->title ? spec->title : ""),
    .icon = ccord_strdup(spec->icon ? spec->icon : ""),
    .visible = spec->visible,
    .root_json = ccord_strdup(spec->root_json),
    .revision = ++c->next_panel_rev,
  };
  if (!p.owner || !p.panel_id || !p.title || !p.icon || !p.root_json) {
    free_panel(&p);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
  }
  c->panels[c->npanels++] = p;
  c->revision++;
  return CCORD_OK;
}

ccord_status ccord_contrib_set_panel(ccord_contrib *c, const char *owner, const char *panel_id, ccord_panel_slot slot,
                                     float height, const char *root_json, ccord_error *err)
{
  ccord_panel_spec spec = {.slot = slot, .height = height, .visible = true, .root_json = root_json};
  return ccord_contrib_set_panel_ex(c, owner, panel_id, &spec, err);
}

bool ccord_contrib_remove_panel(ccord_contrib *c, const char *owner, const char *panel_id)
{
  if (!c || !owner || !panel_id) return false;
  for (size_t i = 0; i < c->npanels; i++) {
    if (strcmp(c->panels[i].owner, owner) == 0 && strcmp(c->panels[i].panel_id, panel_id) == 0) {
      free_panel(&c->panels[i]);
      memmove(&c->panels[i], &c->panels[i + 1], (c->npanels - i - 1) * sizeof c->panels[0]);
      c->npanels--;
      c->revision++;
      return true;
    }
  }
  return false;
}


void ccord_contrib_remove_owner(ccord_contrib *c, const char *owner)
{
  if (!c || !owner) return;
  bool changed = false;
  size_t w = 0;
  for (size_t r = 0; r < c->nmenus; r++) {
    if (strcmp(c->menus[r].owner, owner) == 0) {
      free_menu(&c->menus[r]);
      changed = true;
    } else {
      c->menus[w++] = c->menus[r];
    }
  }
  c->nmenus = w;
  w = 0;
  for (size_t r = 0; r < c->ncmds; r++) {
    if (strcmp(c->cmds[r].owner, owner) == 0) {
      free_cmd(&c->cmds[r]);
      changed = true;
    } else {
      c->cmds[w++] = c->cmds[r];
    }
  }
  c->ncmds = w;
  w = 0;
  for (size_t r = 0; r < c->npanels; r++) {
    if (strcmp(c->panels[r].owner, owner) == 0) {
      free_panel(&c->panels[r]);
      changed = true;
    } else {
      c->panels[w++] = c->panels[r];
    }
  }
  c->npanels = w;
  if (contrib_ext_remove_owner(c, owner)) changed = true;
  if (changed) c->revision++;
}

void ccord_contrib_set_handlers(ccord_contrib *c, const ccord_contrib_handlers *h)
{
  if (!c) return;
  if (h) {
    c->handlers = *h;
  } else {
    memset(&c->handlers, 0, sizeof c->handlers);
  }
}

void ccord_contrib_set_ui_hooks(ccord_contrib *c, const ccord_contrib_ui_hooks *h)
{
  if (!c) return;
  if (h) {
    c->ui_hooks = *h;
  } else {
    memset(&c->ui_hooks, 0, sizeof c->ui_hooks);
  }
}

size_t ccord_contrib_menu_count(const ccord_contrib *c, ccord_menu_target t)
{
  size_t n = 0;
  for (size_t i = 0; c && i < c->nmenus; i++) {
    if (c->menus[i].target == t) n++;
  }
  return n;
}

const ccord_menu_contribution *ccord_contrib_menu_at(const ccord_contrib *c, ccord_menu_target t, size_t i)
{
  for (size_t k = 0; c && k < c->nmenus; k++) {
    if (c->menus[k].target != t) continue;
    if (i == 0) return &c->menus[k];
    i--;
  }
  return nullptr;
}

size_t ccord_contrib_command_count(const ccord_contrib *c)
{
  return c ? c->ncmds : 0;
}

const ccord_command_contribution *ccord_contrib_command_at(const ccord_contrib *c, size_t i)
{
  return (c && i < c->ncmds) ? &c->cmds[i] : nullptr;
}

const ccord_command_contribution *ccord_contrib_find_command(const ccord_contrib *c, const char *name)
{
  if (!c || !name) return nullptr;
  for (size_t i = 0; i < c->ncmds; i++) {
    if (strcmp(c->cmds[i].name, name) == 0) return &c->cmds[i];
  }
  return nullptr;
}

size_t ccord_contrib_panel_count(const ccord_contrib *c)
{
  return c ? c->npanels : 0;
}

const ccord_panel_contribution *ccord_contrib_panel_at(const ccord_contrib *c, size_t i)
{
  if (!c || i >= c->npanels) return nullptr;
  return &c->panels[i];
}

const ccord_panel_contribution *ccord_contrib_find_panel(const ccord_contrib *c, const char *owner, const char *panel_id)
{
  if (!c || !owner || !panel_id) return nullptr;
  for (size_t i = 0; i < c->npanels; i++) {
    if (strcmp(c->panels[i].owner, owner) == 0 && strcmp(c->panels[i].panel_id, panel_id) == 0) {
      return &c->panels[i];
    }
  }
  return nullptr;
}


static const ccord_menu_contribution *find_menu(const ccord_contrib *c, const char *owner, const char *item_id)
{
  for (size_t i = 0; i < c->nmenus; i++) {
    if (strcmp(c->menus[i].owner, owner) == 0 && strcmp(c->menus[i].item_id, item_id) == 0) {
      return &c->menus[i];
    }
  }
  return nullptr;
}

ccord_status ccord_contrib_activate_menu(ccord_contrib *c, const char *owner, const char *item_id,
                                         const ccord_menu_context *ctx, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !owner || !item_id || !ctx) return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: null argument");
  const ccord_menu_contribution *m = find_menu(c, owner, item_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "contrib: no such menu item");
  if (m->target != ctx->target) return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: menu target mismatch");
  if (!c->handlers.menu_activate) {
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "contrib: no menu handler installed");
  }
  c->handlers.menu_activate(c->handlers.ud, m, ctx);
  return CCORD_OK;
}

ccord_status ccord_contrib_run_command(ccord_contrib *c, const char *name, const char *args, ccord_id guild_id,
                                       ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !name) return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: null argument");
  const ccord_command_contribution *cmd = ccord_contrib_find_command(c, name);
  if (!cmd) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "contrib: no such command");
  if (!c->handlers.command_run) {
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "contrib: no command handler installed");
  }
  c->handlers.command_run(c->handlers.ud, cmd, args ? args : "", guild_id, channel_id);
  return CCORD_OK;
}

ccord_status ccord_contrib_dispatch_panel_event(ccord_contrib *c, const char *owner, const char *panel_id,
                                                const ccord_panel_event *ev, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !owner || !panel_id || !ev || !ev->node_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: null argument");
  }
  const ccord_panel_contribution *p = ccord_contrib_find_panel(c, owner, panel_id);
  if (!p) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "contrib: no such panel");
  if (!c->handlers.panel_event) {
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "contrib: no panel handler installed");
  }
  c->handlers.panel_event(c->handlers.ud, p, ev);
  return CCORD_OK;
}



bool ccord_contrib_ui_navigate(ccord_contrib *c, ccord_id guild_id, ccord_id channel_id, ccord_id parent_id,
                               const char *name, const char *view)
{
  if (!c || !c->ui_hooks.navigate) return false;
  return c->ui_hooks.navigate(c->ui_hooks.ud, guild_id, channel_id, parent_id, name ? name : "", view ? view : "");
}

bool ccord_contrib_ui_action(ccord_contrib *c, const char *action)
{
  if (!c || !action || !c->ui_hooks.ui_action) return false;
  return c->ui_hooks.ui_action(c->ui_hooks.ud, action);
}

const char *ccord_contrib_ui_view(const ccord_contrib *c)
{
  if (!c || !c->ui_hooks.get_view) return "channel";
  const char *v = c->ui_hooks.get_view(c->ui_hooks.ud);
  return (v && v[0]) ? v : "channel";
}

bool ccord_contrib_ui_insert_draft(ccord_contrib *c, ccord_id channel_id, const char *text)
{
  if (!c || !text || !c->ui_hooks.insert_draft) return false;
  return c->ui_hooks.insert_draft(c->ui_hooks.ud, channel_id, text);
}
