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

static void free_plugin_info(ccord_plugin_info *info)
{
  contrib_free_str(info->id);
  contrib_free_str(info->name);
  contrib_free_str(info->version);
  contrib_free_str(info->description);
  contrib_free_str(info->icon_path);
  contrib_free_str(info->permissions);
  contrib_free_str(info->allowed_hosts);
  contrib_free_str(info->update_version);
  contrib_free_str(info->update_permissions);
  contrib_free_str(info->update_allowed_hosts);
  contrib_free_str(info->update_changelog);
  contrib_free_str(info->warning_title);
  contrib_free_str(info->warning_message);
  contrib_free_str(info->error);
}

void contrib_free_catalog_entry(ccord_catalog_entry *e)
{
  contrib_free_str(e->id);
  contrib_free_str(e->name);
  contrib_free_str(e->description);
  contrib_free_str(e->license);
  contrib_free_str(e->author);
  contrib_free_str(e->repo);
  contrib_free_str(e->repo_url);
  contrib_free_str(e->version);
  contrib_free_str(e->download_url);
  contrib_free_str(e->sha256);
  contrib_free_str(e->permissions);
  contrib_free_str(e->allowed_hosts);
  contrib_free_str(e->changelog);
  contrib_free_str(e->icon_url);
  contrib_free_str(e->icon_path);
  contrib_free_str(e->warning_title);
  contrib_free_str(e->warning_message);
  contrib_free_str(e->installed_version);
  contrib_free_str(e->claimed_dev);
}

static void free_plugin_logs(plugin_slot *p)
{
  if (p->logs && p->log_cap > 0) {
    for (size_t i = 0; i < p->log_count; i++) {
      size_t idx = (p->log_head + p->log_cap - p->log_count + i) % p->log_cap;
      contrib_free_str(p->logs[idx].message);
    }
    ccord_free(p->logs);
  }
  p->logs = nullptr;
  p->log_cap = 0;
  p->log_head = 0;
  p->log_count = 0;
}

void contrib_free_plugin(plugin_slot *p)
{
  free_plugin_info(&p->info);
  free_plugin_logs(p);
}


ccord_status ccord_contrib_set_plugin(ccord_contrib *c, const ccord_plugin_info *info, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !info || !contrib_text_ok(info->id, CONTRIB_OWNER_MAX, false) || !contrib_text_ok(info->name, CONTRIB_LABEL_MAX, false) ||
      !contrib_text_ok(info->version, CONTRIB_CMD_MAX, true) || !contrib_text_ok(info->description, CONTRIB_DESC_MAX, true) ||
      !contrib_text_ok(info->permissions, 1024, true) ||
      !contrib_text_ok(info->error, 192, true) || info->state > CCORD_PLUGIN_NEEDS_CONSENT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad plugin info");
  }
  ccord_plugin_info fresh = {
    .id = ccord_strdup(info->id), .name = ccord_strdup(info->name),
    .version = ccord_strdup(info->version ? info->version : ""),
    .description = ccord_strdup(info->description ? info->description : ""),
    .icon_path = ccord_strdup(info->icon_path ? info->icon_path : ""),
    .permissions = ccord_strdup(info->permissions ? info->permissions : ""),
    .allowed_hosts = ccord_strdup(info->allowed_hosts ? info->allowed_hosts : ""),
    .perms_mask = info->perms_mask,
    .consented_perms = info->consented_perms,
    .platforms = info->platforms,
    .risk_class = info->risk_class,
    .tier = info->tier,
    .is_bundled = info->is_bundled,
    .revoked = info->revoked,
    .auto_update = info->auto_update,
    .enabled = info->enabled,
    .consent_granted = info->consent_granted,
    .update_needs_review = info->update_needs_review,
    .update_version = ccord_strdup(info->update_version ? info->update_version : ""),
    .update_perms_mask = info->update_perms_mask,
    .update_permissions = ccord_strdup(info->update_permissions ? info->update_permissions : ""),
    .update_allowed_hosts = ccord_strdup(info->update_allowed_hosts ? info->update_allowed_hosts : ""),
    .update_changelog = ccord_strdup(info->update_changelog ? info->update_changelog : ""),
    .warning_sev = info->warning_sev,
    .warning_title = ccord_strdup(info->warning_title ? info->warning_title : ""),
    .warning_message = ccord_strdup(info->warning_message ? info->warning_message : ""),
    .state = info->state, .error = ccord_strdup(info->error ? info->error : ""),
  };
  if (!fresh.id || !fresh.name || !fresh.version || !fresh.description || !fresh.icon_path ||
      !fresh.permissions || !fresh.allowed_hosts || !fresh.update_version ||
      !fresh.update_permissions || !fresh.update_allowed_hosts || !fresh.update_changelog ||
      !fresh.warning_title || !fresh.warning_message || !fresh.error) {
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
      contrib_free_plugin(&c->plugins[i]);
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
    .allowed_hosts = ccord_strdup(s->allowed_hosts ? s->allowed_hosts : ""),
    .changelog = ccord_strdup(s->changelog ? s->changelog : ""),
    .icon_url = ccord_strdup(s->icon_url ? s->icon_url : ""),
    .icon_path = ccord_strdup(s->icon_path ? s->icon_path : ""),
    .perms_mask = s->perms_mask,
    .platforms = s->platforms,
    .size_bytes = s->size_bytes,
    .downloads = s->downloads,
    .rating_score_x10 = s->rating_score_x10,
    .rating_sentiment = s->rating_sentiment,
    .rating_pos = s->rating_pos,
    .rating_neu = s->rating_neu,
    .rating_neg = s->rating_neg,
    .rating_total = s->rating_total,
    .comment_count = s->comment_count,
    .claimed_dev = ccord_strdup(s->claimed_dev ? s->claimed_dev : ""),
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
      !d.allowed_hosts || !d.changelog || !d.icon_url || !d.icon_path ||
      !d.warning_title || !d.warning_message || !d.installed_version || !d.claimed_dev) {
    contrib_free_catalog_entry(&d);
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
    for (size_t i = 0; i < c->ncatalog; i++) contrib_free_catalog_entry(&c->catalog[i]);
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
    contrib_free_catalog_entry(&c->catalog_detail);
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
  for (size_t i = 0; i < c->ncatalog_updates; i++) contrib_free_catalog_entry(&c->catalog_updates[i]);
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
  if (p->log_count == p->log_cap && p->log_cap < CONTRIB_LOG_CAP) {
    size_t ncap = p->log_cap ? p->log_cap * 4u : CONTRIB_LOG_INIT_CAP;
    if (ncap > CONTRIB_LOG_CAP) ncap = CONTRIB_LOG_CAP;
    ccord_plugin_log_entry *g = ccord_realloc_array(p->logs, ncap, sizeof *g);
    if (!g) return;
    // Before reaching CONTRIB_LOG_CAP the ring has never overwritten entries, so indices [0..log_count-1]
    // are already contiguous in chronological order and log_head resumes at log_count.
    p->logs = g;
    p->log_head = p->log_count;
    p->log_cap = ncap;
  }
  char *msg_copy = ccord_strdup(message);
  if (!msg_copy) return;
  if (p->log_count == CONTRIB_LOG_CAP) {
    contrib_free_str(p->logs[p->log_head].message);
  } else {
    p->log_count++;
  }
  p->logs[p->log_head] = (ccord_plugin_log_entry){
    .timestamp_ms = timestamp_ms,
    .level = level,
    .message = msg_copy,
  };
  p->log_head = (p->log_head + 1u) % p->log_cap;
  c->revision++;
}

void ccord_contrib_clear_plugin_log(ccord_contrib *c, const char *owner)
{
  plugin_slot *p = find_plugin_slot(c, owner);
  if (!p) return;
  free_plugin_logs(p);
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
  if (!p || !p->logs || p->log_cap == 0 || i >= p->log_count) return nullptr;
  size_t idx = (p->log_head + p->log_cap - p->log_count + i) % p->log_cap;
  return &p->logs[idx];
}

size_t ccord_contrib_plugin_error_count(const ccord_contrib *c, const char *owner)
{
  plugin_slot *p = find_plugin_slot(c, owner);
  if (!p || !p->logs || p->log_cap == 0) return 0;
  size_t n = 0;
  for (size_t i = 0; i < p->log_count; i++) {
    size_t idx = (p->log_head + p->log_cap - p->log_count + i) % p->log_cap;
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

void ccord_contrib_plugin_consent_accept_for(ccord_contrib *c, const char *id, uint64_t expected_perms)
{
  if (!c || !id) return;
  if (c->handlers.plugin_consent_accept_for) {
    c->handlers.plugin_consent_accept_for(c->handlers.ud, id, expected_perms);
  } else if (c->handlers.plugin_consent_accept) {
    c->handlers.plugin_consent_accept(c->handlers.ud, id);
  }
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

void ccord_contrib_catalog_query_ex(ccord_contrib *c, const char *q, int tier, int api_class, const char *platform,
                                    const char *sort, bool append_next_page)
{
  if (!c) return;
  if (c->handlers.catalog_query_ex) {
    c->handlers.catalog_query_ex(c->handlers.ud, q ? q : "", tier, api_class,
                                 platform ? platform : "", sort ? sort : "popular", append_next_page);
  } else if (c->handlers.catalog_query) {
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

void ccord_contrib_catalog_install_approved(ccord_contrib *c, const char *id, uint64_t approved_perms)
{
  if (!c || !id) return;
  if (c->handlers.catalog_install_approved) {
    c->handlers.catalog_install_approved(c->handlers.ud, id, approved_perms);
  } else if (c->handlers.catalog_install) {
    c->handlers.catalog_install(c->handlers.ud, id);
  }
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

