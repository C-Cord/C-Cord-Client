// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "contrib_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
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
  for (size_t i = 0; i < c->nmenu_states; i++) {
    ccord_free(c->menu_states[i].owner);
    ccord_free(c->menu_states[i].item_id);
  }
  for (size_t i = 0; i < c->ncmds; i++) free_cmd(&c->cmds[i]);
  for (size_t i = 0; i < c->npanels; i++) free_panel(&c->panels[i]);
  contrib_ext_destroy(c);
  for (size_t i = 0; i < c->nplugins; i++) contrib_free_plugin(&c->plugins[i]);
  for (size_t i = 0; i < c->ncatalog; i++) contrib_free_catalog_entry(&c->catalog[i]);
  for (size_t i = 0; i < c->ncatalog_updates; i++) contrib_free_catalog_entry(&c->catalog_updates[i]);
  if (c->has_catalog_detail) contrib_free_catalog_entry(&c->catalog_detail);
  ccord_free(c->menus);
  ccord_free(c->menu_states);
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
    .has_resolver = spec->has_resolver,
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
      (void)ccord_contrib_clear_menu_state(c, owner, item_id, CCORD_ID_NONE, true);
      c->revision++;
      return true;
    }
  }
  return false;
}

static int menu_state_cmp_key(const contrib_menu_state_entry *e, const char *owner, const char *item_id, ccord_id tid)
{
  int r = strcmp(e->owner, owner);
  if (r != 0) return r;
  r = strcmp(e->item_id, item_id);
  if (r != 0) return r;
  if (e->target_id < tid) return -1;
  if (e->target_id > tid) return 1;
  return 0;
}

static size_t menu_state_lower_bound(const ccord_contrib *c, const char *owner, const char *item_id, ccord_id tid)
{
  size_t lo = 0, hi = c->nmenu_states;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (menu_state_cmp_key(&c->menu_states[mid], owner, item_id, tid) < 0) lo = mid + 1u;
    else hi = mid;
  }
  return lo;
}

ccord_status ccord_contrib_set_menu_state(ccord_contrib *c, const char *owner, const char *item_id,
                                          ccord_id target_id, const ccord_menu_item_state *state,
                                          ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !owner || !item_id || !state) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: null menu state argument");
  }
  bool owned = false;
  for (size_t i = 0; i < c->nmenus; i++) {
    if (strcmp(c->menus[i].owner, owner) == 0 && strcmp(c->menus[i].item_id, item_id) == 0) {
      owned = true;
      break;
    }
  }
  if (!owned) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "contrib: menu item '%s' not registered by owner", item_id);
  }
  if (state->has_label && !text_ok(state->label, 64u, false)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: menu state label must be 1..64 bytes UTF-8 text");
  }

  size_t pos = menu_state_lower_bound(c, owner, item_id, target_id);
  if (pos < c->nmenu_states && menu_state_cmp_key(&c->menu_states[pos], owner, item_id, target_id) == 0) {
    c->menu_states[pos].state = *state;
    c->revision++;
    return CCORD_OK;
  }

  size_t owner_cnt = 0;
  for (size_t i = 0; i < c->nmenu_states; i++) {
    if (strcmp(c->menu_states[i].owner, owner) == 0) owner_cnt++;
  }
  if (owner_cnt >= CONTRIB_MENU_STATE_MAX_OWNER) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "contrib: menu state override limit (2048) reached");
  }
  if (c->nmenu_states == c->capmenu_states) {
    size_t ncap = c->capmenu_states ? c->capmenu_states * 2u : 16u;
    contrib_menu_state_entry *g = ccord_realloc_array(c->menu_states, ncap, sizeof *g);
    if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
    c->menu_states = g;
    c->capmenu_states = ncap;
  }
  char *odup = ccord_strdup(owner);
  char *idup = ccord_strdup(item_id);
  if (!odup || !idup) {
    ccord_free(odup);
    ccord_free(idup);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
  }
  if (pos < c->nmenu_states) {
    memmove(&c->menu_states[pos + 1u], &c->menu_states[pos],
            (c->nmenu_states - pos) * sizeof c->menu_states[0]);
  }
  c->menu_states[pos] = (contrib_menu_state_entry){
    .owner = odup,
    .item_id = idup,
    .target_id = target_id,
    .state = *state,
  };
  c->nmenu_states++;
  c->revision++;
  return CCORD_OK;
}

bool ccord_contrib_clear_menu_state(ccord_contrib *c, const char *owner, const char *item_id,
                                    ccord_id target_id, bool clear_all_targets)
{
  if (!c || !owner || !item_id || c->nmenu_states == 0) return false;
  if (!clear_all_targets) {
    size_t pos = menu_state_lower_bound(c, owner, item_id, target_id);
    if (pos < c->nmenu_states && menu_state_cmp_key(&c->menu_states[pos], owner, item_id, target_id) == 0) {
      ccord_free(c->menu_states[pos].owner);
      ccord_free(c->menu_states[pos].item_id);
      memmove(&c->menu_states[pos], &c->menu_states[pos + 1u],
              (c->nmenu_states - pos - 1u) * sizeof c->menu_states[0]);
      c->nmenu_states--;
      c->revision++;
      return true;
    }
    return false;
  }
  size_t w = 0;
  bool removed = false;
  for (size_t r = 0; r < c->nmenu_states; r++) {
    if (strcmp(c->menu_states[r].owner, owner) == 0 && strcmp(c->menu_states[r].item_id, item_id) == 0) {
      ccord_free(c->menu_states[r].owner);
      ccord_free(c->menu_states[r].item_id);
      removed = true;
    } else {
      c->menu_states[w++] = c->menu_states[r];
    }
  }
  c->nmenu_states = w;
  if (removed) c->revision++;
  return removed;
}

static void merge_menu_state(ccord_menu_item_state *dst, const ccord_menu_item_state *src)
{
  if (src->has_label) {
    dst->has_label = true;
    snprintf(dst->label, sizeof dst->label, "%s", src->label);
  }
  if (src->has_checked) { dst->has_checked = true; dst->checked = src->checked; }
  if (src->has_hidden) { dst->has_hidden = true; dst->hidden = src->hidden; }
  if (src->has_disabled) { dst->has_disabled = true; dst->disabled = src->disabled; }
  if (src->has_danger) { dst->has_danger = true; dst->danger = src->danger; }
}

bool ccord_contrib_find_menu_state(const ccord_contrib *c, const char *owner, const char *item_id,
                                   ccord_id target_id, ccord_menu_item_state *out)
{
  if (out) memset(out, 0, sizeof *out);
  if (!c || !owner || !item_id || !out || c->nmenu_states == 0) return false;
  bool found = false;
  size_t p0 = menu_state_lower_bound(c, owner, item_id, CCORD_ID_NONE);
  if (p0 < c->nmenu_states && menu_state_cmp_key(&c->menu_states[p0], owner, item_id, CCORD_ID_NONE) == 0) {
    merge_menu_state(out, &c->menu_states[p0].state);
    found = true;
  }
  if (target_id != CCORD_ID_NONE) {
    size_t pt = menu_state_lower_bound(c, owner, item_id, target_id);
    if (pt < c->nmenu_states && menu_state_cmp_key(&c->menu_states[pt], owner, item_id, target_id) == 0) {
      merge_menu_state(out, &c->menu_states[pt].state);
      found = true;
    }
  }
  return found;
}

size_t ccord_contrib_resolve_menu_items(const ccord_contrib *c, ccord_menu_target target,
                                        const ccord_menu_context *ctx,
                                        ccord_menu_resolved_entry *out, size_t max_out)
{
  if (!c || !ctx || !out || max_out == 0 || !c->handlers.menu_resolve) return 0;
  // Collect distinct owners that have >=1 resolver item for `target`.
  const char *owners[32];
  size_t nowners = 0;
  for (size_t i = 0; i < c->nmenus; i++) {
    const ccord_menu_contribution *m = &c->menus[i];
    if (m->target != target || !m->has_resolver) continue;
    bool seen = false;
    for (size_t k = 0; k < nowners; k++) {
      if (strcmp(owners[k], m->owner) == 0) { seen = true; break; }
    }
    if (!seen && nowners < 32u) owners[nowners++] = m->owner;
  }
  if (nowners == 0) return 0;

  uint64_t total_deadline = ccord_clock_ms() + 100u;
  size_t total = 0;
  for (size_t oi = 0; oi < nowners && total < max_out; oi++) {
    uint64_t now = ccord_clock_ms();
    if (now >= total_deadline) break;
    uint64_t rem = total_deadline - now;
    uint32_t per_plugin_ms = rem < 50u ? (uint32_t)rem : 50u;

    const char *ids[64];
    size_t nid = 0;
    for (size_t i = 0; i < c->nmenus && nid < 64u; i++) {
      if (c->menus[i].target == target && c->menus[i].has_resolver &&
          strcmp(c->menus[i].owner, owners[oi]) == 0) {
        ids[nid++] = c->menus[i].item_id;
      }
    }
    if (nid == 0) continue;
    size_t got = c->handlers.menu_resolve(c->handlers.ud, owners[oi], ids, nid, ctx, per_plugin_ms,
                                          out + total, max_out - total);
    if (got > max_out - total) got = max_out - total;
    total += got;
  }
  return total;
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
  for (size_t r = 0; r < c->nmenu_states; r++) {
    if (strcmp(c->menu_states[r].owner, owner) == 0) {
      ccord_free(c->menu_states[r].owner);
      ccord_free(c->menu_states[r].item_id);
      changed = true;
    } else {
      c->menu_states[w++] = c->menu_states[r];
    }
  }
  c->nmenu_states = w;
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

bool ccord_contrib_ui_jump_to_message(ccord_contrib *c, ccord_id channel_id, ccord_id message_id)
{
  if (!c || !c->ui_hooks.jump_to_message || channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) return false;
  return c->ui_hooks.jump_to_message(c->ui_hooks.ud, channel_id, message_id);
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
