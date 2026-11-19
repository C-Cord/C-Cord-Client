// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/model/contrib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>

#define CONTRIB_OWNER_MAX 64u
#define CONTRIB_ITEM_MAX 64u
#define CONTRIB_LABEL_MAX 100u
#define CONTRIB_CMD_MAX 32u
#define CONTRIB_DESC_MAX 200u
#define CONTRIB_PANEL_JSON_MAX (256u * 1024u)

#define CONTRIB_LOG_CAP 500u

typedef struct plugin_slot {
  ccord_plugin_info info; // every string is owned
  ccord_plugin_log_entry logs[CONTRIB_LOG_CAP];
  size_t log_head;        // next write index
  size_t log_count;       // <= CONTRIB_LOG_CAP
} plugin_slot;

struct ccord_contrib {
  ccord_menu_contribution *menus;
  size_t nmenus, capmenus;
  ccord_command_contribution *cmds;
  size_t ncmds, capcmds;
  ccord_panel_contribution *panels;
  size_t npanels, cappanels;
  uint64_t next_panel_rev;
  plugin_slot *plugins;
  size_t nplugins, capplugins;
  ccord_catalog_entry *catalog;
  size_t ncatalog;
  ccord_catalog_entry *catalog_updates;
  size_t ncatalog_updates;
  ccord_catalog_entry catalog_detail;
  bool has_catalog_detail;
  ccord_catalog_status catalog_status;
  ccord_catalog_prefs catalog_prefs;
  ccord_contrib_handlers handlers;
  ccord_contrib_ui_hooks ui_hooks;
  uint64_t revision;
};

// Public structs hold const char * fields we own; free them without a const-discarding cast.
static void free_str(const char *s)
{
  union { const char *c; void *v; } u = {.c = s};
  ccord_free(u.v);
}

static void free_menu(ccord_menu_contribution *m)
{
  free_str(m->owner);
  free_str(m->item_id);
  free_str(m->label);
}

static void free_cmd(ccord_command_contribution *c)
{
  free_str(c->owner);
  free_str(c->name);
  free_str(c->description);
}

static void free_panel(ccord_panel_contribution *p)
{
  free_str(p->owner);
  free_str(p->panel_id);
  free_str(p->root_json);
}

static void free_plugin_info(ccord_plugin_info *info)
{
  free_str(info->id);
  free_str(info->name);
  free_str(info->version);
  free_str(info->description);
  free_str(info->permissions);
  free_str(info->update_version);
  free_str(info->warning_title);
  free_str(info->warning_message);
  free_str(info->error);
}

static void free_catalog_entry(ccord_catalog_entry *e)
{
  free_str(e->id);
  free_str(e->name);
  free_str(e->description);
  free_str(e->license);
  free_str(e->author);
  free_str(e->repo);
  free_str(e->repo_url);
  free_str(e->version);
  free_str(e->download_url);
  free_str(e->sha256);
  free_str(e->permissions);
  free_str(e->changelog);
  free_str(e->warning_title);
  free_str(e->warning_message);
  free_str(e->installed_version);
}

static void free_plugin(plugin_slot *p)
{
  free_plugin_info(&p->info);
  for (size_t i = 0; i < p->log_count; i++) {
    size_t idx = (p->log_head + CONTRIB_LOG_CAP - p->log_count + i) % CONTRIB_LOG_CAP;
    free_str(p->logs[idx].message);
  }
  p->log_head = 0;
  p->log_count = 0;
}

static bool text_ok(const char *s, size_t max, bool allow_empty)
{
  if (!s) return allow_empty;
  size_t n = strlen(s);
  if (n == 0) return allow_empty;
  return n <= max && ccord_utf8_valid_text(s, n);
}

static bool name_ok(const char *s)
{
  if (!s || s[0] == '\0' || strlen(s) > CONTRIB_CMD_MAX) return false;
  for (const char *p = s; *p; p++) {
    bool ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
    if (!ok) return false;
  }
  return true;
}

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
  for (size_t i = 0; i < c->nplugins; i++) free_plugin(&c->plugins[i]);
  for (size_t i = 0; i < c->ncatalog; i++) free_catalog_entry(&c->catalog[i]);
  for (size_t i = 0; i < c->ncatalog_updates; i++) free_catalog_entry(&c->catalog_updates[i]);
  if (c->has_catalog_detail) free_catalog_entry(&c->catalog_detail);
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

ccord_status ccord_contrib_add_menu(ccord_contrib *c, const char *owner, const char *item_id, const char *label,
                                    ccord_menu_target t, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !text_ok(owner, CONTRIB_OWNER_MAX, false) || !text_ok(item_id, CONTRIB_ITEM_MAX, false) ||
      !text_ok(label, CONTRIB_LABEL_MAX, false) || t >= CCORD_MENU__COUNT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad menu contribution");
  }
  for (size_t i = 0; i < c->nmenus; i++) {
    if (strcmp(c->menus[i].owner, owner) == 0 && strcmp(c->menus[i].item_id, item_id) == 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: menu item already registered by owner '%s'", owner);
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
    .owner = ccord_strdup(owner), .item_id = ccord_strdup(item_id),
    .label = ccord_strdup(label), .target = t,
  };
  if (!m.owner || !m.item_id || !m.label) {
    free_menu(&m);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
  }
  c->menus[c->nmenus++] = m;
  c->revision++;
  return CCORD_OK;
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

ccord_status ccord_contrib_add_command(ccord_contrib *c, const char *owner, const char *name, const char *description,
                                       ccord_error *err)
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
    .owner = ccord_strdup(owner), .name = ccord_strdup(name), .description = ccord_strdup(description ? description : ""),
  };
  if (!cmd.owner || !cmd.name || !cmd.description) {
    free_cmd(&cmd);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
  }
  c->cmds[c->ncmds++] = cmd;
  c->revision++;
  return CCORD_OK;
}

ccord_status ccord_contrib_set_panel(ccord_contrib *c, const char *owner, const char *panel_id, ccord_panel_slot slot,
                                     float height, const char *root_json, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !text_ok(owner, CONTRIB_OWNER_MAX, false) || !name_ok(panel_id) || slot >= CCORD_PANEL__COUNT ||
      !(height >= 12.0f && height <= 240.0f) || !root_json) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad panel contribution");
  }
  size_t jlen = strlen(root_json);
  if (jlen == 0 || jlen > CONTRIB_PANEL_JSON_MAX || !ccord_utf8_valid(root_json, jlen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad panel JSON");
  }
  for (size_t i = 0; i < c->npanels; i++) {
    if (strcmp(c->panels[i].owner, owner) == 0 && strcmp(c->panels[i].panel_id, panel_id) == 0) {
      char *rj = ccord_strdup(root_json);
      if (!rj) return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
      free_str(c->panels[i].root_json);
      c->panels[i].slot = slot;
      c->panels[i].height = height;
      c->panels[i].root_json = rj;
      c->panels[i].revision = ++c->next_panel_rev;
      c->revision++;
      return CCORD_OK;
    }
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
    .slot = slot,
    .height = height,
    .root_json = ccord_strdup(root_json),
    .revision = ++c->next_panel_rev,
  };
  if (!p.owner || !p.panel_id || !p.root_json) {
    free_panel(&p);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
  }
  c->panels[c->npanels++] = p;
  c->revision++;
  return CCORD_OK;
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

ccord_status ccord_contrib_set_plugin(ccord_contrib *c, const ccord_plugin_info *info, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !info || !text_ok(info->id, CONTRIB_OWNER_MAX, false) || !text_ok(info->name, CONTRIB_LABEL_MAX, false) ||
      !text_ok(info->version, CONTRIB_CMD_MAX, true) || !text_ok(info->description, CONTRIB_DESC_MAX, true) ||
      !text_ok(info->permissions, 1024, true) ||
      !text_ok(info->error, 192, true) || info->state > CCORD_PLUGIN_NEEDS_CONSENT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad plugin info");
  }
  ccord_plugin_info fresh = {
    .id = ccord_strdup(info->id), .name = ccord_strdup(info->name),
    .version = ccord_strdup(info->version ? info->version : ""),
    .description = ccord_strdup(info->description ? info->description : ""),
    .permissions = ccord_strdup(info->permissions ? info->permissions : ""),
    .risk_class = info->risk_class,
    .tier = info->tier,
    .is_bundled = info->is_bundled,
    .auto_update = info->auto_update,
    .enabled = info->enabled,
    .consent_granted = info->consent_granted,
    .update_needs_review = info->update_needs_review,
    .update_version = ccord_strdup(info->update_version ? info->update_version : ""),
    .warning_sev = info->warning_sev,
    .warning_title = ccord_strdup(info->warning_title ? info->warning_title : ""),
    .warning_message = ccord_strdup(info->warning_message ? info->warning_message : ""),
    .state = info->state, .error = ccord_strdup(info->error ? info->error : ""),
  };
  if (!fresh.id || !fresh.name || !fresh.version || !fresh.description || !fresh.permissions ||
      !fresh.update_version || !fresh.warning_title || !fresh.warning_message || !fresh.error) {
    free_plugin_info(&fresh);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
  }
  for (size_t i = 0; i < c->nplugins; i++) {
    if (strcmp(c->plugins[i].info.id, info->id) == 0) {
      free_plugin_info(&c->plugins[i].info);
      c->plugins[i].info = fresh;
      c->revision++;
      return CCORD_OK;
    }
  }
  if (c->nplugins == c->capplugins) {
    size_t ncap = c->capplugins ? c->capplugins * 2 : 8;
    plugin_slot *g = ccord_realloc_array(c->plugins, ncap, sizeof *g);
    if (!g) {
      free_plugin_info(&fresh);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
    }
    c->plugins = g;
    c->capplugins = ncap;
  }
  c->plugins[c->nplugins] = (plugin_slot){.info = fresh};
  c->nplugins++;
  c->revision++;
  return CCORD_OK;
}

bool ccord_contrib_remove_plugin(ccord_contrib *c, const char *id)
{
  if (!c || !id) return false;
  for (size_t i = 0; i < c->nplugins; i++) {
    if (strcmp(c->plugins[i].info.id, id) == 0) {
      free_plugin(&c->plugins[i]);
      memmove(&c->plugins[i], &c->plugins[i + 1], (c->nplugins - i - 1) * sizeof c->plugins[0]);
      c->nplugins--;
      c->revision++;
      return true;
    }
  }
  return false;
}

static bool clone_catalog_entry(const ccord_catalog_entry *s, ccord_catalog_entry *out)
{
  ccord_catalog_entry d = {
    .id = ccord_strdup(s->id ? s->id : ""),
    .name = ccord_strdup(s->name ? s->name : ""),
    .description = ccord_strdup(s->description ? s->description : ""),
    .license = ccord_strdup(s->license ? s->license : ""),
    .author = ccord_strdup(s->author ? s->author : ""),
    .repo = ccord_strdup(s->repo ? s->repo : ""),
    .repo_url = ccord_strdup(s->repo_url ? s->repo_url : ""),
    .version = ccord_strdup(s->version ? s->version : ""),
    .download_url = ccord_strdup(s->download_url ? s->download_url : ""),
    .sha256 = ccord_strdup(s->sha256 ? s->sha256 : ""),
    .permissions = ccord_strdup(s->permissions ? s->permissions : ""),
    .changelog = ccord_strdup(s->changelog ? s->changelog : ""),
    .size_bytes = s->size_bytes,
    .downloads = s->downloads,
    .api_class = s->api_class ? s->api_class : 1,
    .tier = s->tier,
    .warning_sev = s->warning_sev,
    .warning_title = ccord_strdup(s->warning_title ? s->warning_title : ""),
    .warning_message = ccord_strdup(s->warning_message ? s->warning_message : ""),
    .has_detail = s->has_detail,
    .installed = s->installed,
    .bundled = s->bundled,
    .installing = s->installing,
    .update_available = s->update_available,
    .update_needs_review = s->update_needs_review,
    .auto_update = s->auto_update,
    .installed_version = ccord_strdup(s->installed_version ? s->installed_version : ""),
  };
  if (!d.id || !d.name || !d.description || !d.license || !d.author || !d.repo ||
      !d.repo_url || !d.version || !d.download_url || !d.sha256 || !d.permissions ||
      !d.changelog || !d.warning_title || !d.warning_message || !d.installed_version) {
    free_catalog_entry(&d);
    return false;
  }
  *out = d;
  return true;
}

void ccord_contrib_set_catalog(ccord_contrib *c, const ccord_catalog_entry *entries, size_t count,
                               const ccord_catalog_status *status, const ccord_catalog_prefs *prefs)
{
  if (!c) return;
  if (entries || count == 0) {
    for (size_t i = 0; i < c->ncatalog; i++) free_catalog_entry(&c->catalog[i]);
    ccord_free(c->catalog);
    c->catalog = nullptr;
    c->ncatalog = 0;
    if (count > 0) {
      ccord_catalog_entry *arr = ccord_calloc(count, sizeof *arr);
      if (arr) {
        size_t ok = 0;
        for (size_t i = 0; i < count; i++) {
          if (!clone_catalog_entry(&entries[i], &arr[ok])) break;
          ok++;
        }
        c->catalog = arr;
        c->ncatalog = ok;
      }
    }
  }
  if (status) c->catalog_status = *status;
  if (prefs) c->catalog_prefs = *prefs;
  c->revision++;
}

void ccord_contrib_set_catalog_detail(ccord_contrib *c, const ccord_catalog_entry *detail)
{
  if (!c) return;
  if (c->has_catalog_detail) {
    free_catalog_entry(&c->catalog_detail);
    memset(&c->catalog_detail, 0, sizeof c->catalog_detail);
    c->has_catalog_detail = false;
  }
  if (detail && clone_catalog_entry(detail, &c->catalog_detail)) {
    c->has_catalog_detail = true;
  }
  c->revision++;
}

void ccord_contrib_set_catalog_updates(ccord_contrib *c, const ccord_catalog_entry *entries, size_t count)
{
  if (!c) return;
  for (size_t i = 0; i < c->ncatalog_updates; i++) free_catalog_entry(&c->catalog_updates[i]);
  ccord_free(c->catalog_updates);
  c->catalog_updates = nullptr;
  c->ncatalog_updates = 0;
  if (entries && count > 0) {
    ccord_catalog_entry *arr = ccord_calloc(count, sizeof *arr);
    if (arr) {
      size_t ok = 0;
      for (size_t i = 0; i < count; i++) {
        if (!clone_catalog_entry(&entries[i], &arr[ok])) break;
        ok++;
      }
      c->catalog_updates = arr;
      c->ncatalog_updates = ok;
    }
  }
  c->revision++;
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

size_t ccord_contrib_plugin_count(const ccord_contrib *c)
{
  return c ? c->nplugins : 0;
}

const ccord_plugin_info *ccord_contrib_plugin_at(const ccord_contrib *c, size_t i)
{
  if (!c || i >= c->nplugins) return nullptr;
  return &c->plugins[i].info;
}

size_t ccord_contrib_catalog_count(const ccord_contrib *c)
{
  return c ? c->ncatalog : 0;
}

const ccord_catalog_entry *ccord_contrib_catalog_at(const ccord_contrib *c, size_t i)
{
  if (!c || i >= c->ncatalog) return nullptr;
  return &c->catalog[i];
}

const ccord_catalog_entry *ccord_contrib_catalog_find(const ccord_contrib *c, const char *id)
{
  if (!c || !id) return nullptr;
  if (c->has_catalog_detail && strcmp(c->catalog_detail.id, id) == 0) {
    return &c->catalog_detail;
  }
  for (size_t i = 0; i < c->ncatalog_updates; i++) {
    if (strcmp(c->catalog_updates[i].id, id) == 0) return &c->catalog_updates[i];
  }
  for (size_t i = 0; i < c->ncatalog; i++) {
    if (strcmp(c->catalog[i].id, id) == 0) return &c->catalog[i];
  }
  return nullptr;
}

const ccord_catalog_entry *ccord_contrib_catalog_detail(const ccord_contrib *c)
{
  if (!c || !c->has_catalog_detail) return nullptr;
  return &c->catalog_detail;
}

size_t ccord_contrib_catalog_update_count(const ccord_contrib *c)
{
  return c ? c->ncatalog_updates : 0;
}

const ccord_catalog_entry *ccord_contrib_catalog_update_at(const ccord_contrib *c, size_t i)
{
  if (!c || i >= c->ncatalog_updates) return nullptr;
  return &c->catalog_updates[i];
}

const ccord_catalog_status *ccord_contrib_catalog_status(const ccord_contrib *c)
{
  return c ? &c->catalog_status : nullptr;
}

const ccord_catalog_prefs *ccord_contrib_catalog_prefs(const ccord_contrib *c)
{
  return c ? &c->catalog_prefs : nullptr;
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

static plugin_slot *find_plugin_slot(const ccord_contrib *c, const char *owner)
{
  if (!c || !owner) return nullptr;
  for (size_t i = 0; i < c->nplugins; i++) {
    if (strcmp(c->plugins[i].info.id, owner) == 0) return &c->plugins[i];
  }
  return nullptr;
}

void ccord_contrib_add_plugin_log(ccord_contrib *c, const char *owner, uint64_t timestamp_ms, uint8_t level, const char *message)
{
  plugin_slot *p = find_plugin_slot(c, owner);
  if (!p || !message) return;
  if (p->log_count == CONTRIB_LOG_CAP) {
    free_str(p->logs[p->log_head].message);
  } else {
    p->log_count++;
  }
  p->logs[p->log_head] = (ccord_plugin_log_entry){
    .timestamp_ms = timestamp_ms,
    .level = level,
    .message = ccord_strdup(message),
  };
  p->log_head = (p->log_head + 1) % CONTRIB_LOG_CAP;
  c->revision++;
}

void ccord_contrib_clear_plugin_log(ccord_contrib *c, const char *owner)
{
  plugin_slot *p = find_plugin_slot(c, owner);
  if (!p) return;
  for (size_t i = 0; i < p->log_count; i++) {
    size_t idx = (p->log_head + CONTRIB_LOG_CAP - p->log_count + i) % CONTRIB_LOG_CAP;
    free_str(p->logs[idx].message);
  }
  p->log_head = 0;
  p->log_count = 0;
  c->revision++;
}

size_t ccord_contrib_plugin_log_count(const ccord_contrib *c, const char *owner)
{
  plugin_slot *p = find_plugin_slot(c, owner);
  return p ? p->log_count : 0;
}

const ccord_plugin_log_entry *ccord_contrib_plugin_log_at(const ccord_contrib *c, const char *owner, size_t i)
{
  plugin_slot *p = find_plugin_slot(c, owner);
  if (!p || i >= p->log_count) return nullptr;
  size_t idx = (p->log_head + CONTRIB_LOG_CAP - p->log_count + i) % CONTRIB_LOG_CAP;
  return &p->logs[idx];
}

size_t ccord_contrib_plugin_error_count(const ccord_contrib *c, const char *owner)
{
  plugin_slot *p = find_plugin_slot(c, owner);
  if (!p) return 0;
  size_t n = 0;
  for (size_t i = 0; i < p->log_count; i++) {
    size_t idx = (p->log_head + CONTRIB_LOG_CAP - p->log_count + i) % CONTRIB_LOG_CAP;
    if (p->logs[idx].level >= 2) n++;
  }
  return n;
}

void ccord_contrib_plugin_set_enabled(ccord_contrib *c, const char *id, bool enabled)
{
  if (c && id && c->handlers.plugin_set_enabled) c->handlers.plugin_set_enabled(c->handlers.ud, id, enabled);
}

void ccord_contrib_plugin_consent_accept(ccord_contrib *c, const char *id)
{
  if (c && id && c->handlers.plugin_consent_accept) c->handlers.plugin_consent_accept(c->handlers.ud, id);
}

void ccord_contrib_plugin_reload(ccord_contrib *c, const char *id)
{
  if (c && id && c->handlers.plugin_reload) c->handlers.plugin_reload(c->handlers.ud, id);
}

bool ccord_contrib_plugin_export_cce(ccord_contrib *c, const char *id, const char *dest_path, char *err_buf, size_t err_cap)
{
  if (!c || !id || !dest_path || !c->handlers.plugin_export_cce) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "export not supported");
    return false;
  }
  return c->handlers.plugin_export_cce(c->handlers.ud, id, dest_path, err_buf, err_cap);
}

bool ccord_contrib_plugin_import_package(ccord_contrib *c, const char *pkg_path, char *err_buf, size_t err_cap)
{
  if (!c || !pkg_path || !c->handlers.plugin_import_package) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "import not supported");
    return false;
  }
  return c->handlers.plugin_import_package(c->handlers.ud, pkg_path, err_buf, err_cap);
}

bool ccord_contrib_plugin_uninstall(ccord_contrib *c, const char *id, char *err_buf, size_t err_cap)
{
  if (!c || !id || !c->handlers.plugin_uninstall) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "uninstall not supported");
    return false;
  }
  return c->handlers.plugin_uninstall(c->handlers.ud, id, err_buf, err_cap);
}

void ccord_contrib_catalog_refresh(ccord_contrib *c)
{
  if (c && c->handlers.catalog_refresh) c->handlers.catalog_refresh(c->handlers.ud);
}

void ccord_contrib_catalog_query(ccord_contrib *c, const char *q, int tier, int api_class, const char *sort,
                                 bool append_next_page)
{
  if (c && c->handlers.catalog_query) {
    c->handlers.catalog_query(c->handlers.ud, q ? q : "", tier, api_class, sort ? sort : "popular", append_next_page);
  }
}

void ccord_contrib_catalog_fetch_detail(ccord_contrib *c, const char *id)
{
  if (c && id && c->handlers.catalog_fetch_detail) c->handlers.catalog_fetch_detail(c->handlers.ud, id);
}

void ccord_contrib_catalog_check_updates(ccord_contrib *c)
{
  if (c && c->handlers.catalog_check_updates) c->handlers.catalog_check_updates(c->handlers.ud);
}

void ccord_contrib_catalog_install(ccord_contrib *c, const char *id)
{
  if (c && id && c->handlers.catalog_install) c->handlers.catalog_install(c->handlers.ud, id);
}

void ccord_contrib_catalog_set_prefs(ccord_contrib *c, const ccord_catalog_prefs *prefs)
{
  if (c && prefs && c->handlers.catalog_set_prefs) c->handlers.catalog_set_prefs(c->handlers.ud, prefs);
}

void ccord_contrib_catalog_set_plugin_auto_update(ccord_contrib *c, const char *id, bool enabled)
{
  if (c && id && c->handlers.catalog_set_plugin_auto_update) {
    c->handlers.catalog_set_plugin_auto_update(c->handlers.ud, id, enabled);
  }
}

void ccord_contrib_catalog_run_auto_updates(ccord_contrib *c)
{
  if (c && c->handlers.catalog_run_auto_updates) c->handlers.catalog_run_auto_updates(c->handlers.ud);
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
