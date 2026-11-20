// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "plugins_internal.h"
#include "package/zip.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/base64.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/config.h>
#include <ccord/core/input.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/paths.h>
#include <ccord/core/utf8.h>
#include <ccord/ipc/ipc.h>
#include <ccord/store/notify.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if !CCORD_OS_WINDOWS
#include <fcntl.h>
#include <signal.h>
#if !CCORD_OS_ANDROID
#include <spawn.h>
#include <sys/wait.h>
#endif
#include <unistd.h>
#endif


static plugin *new_record(ccord_plugins *m)
{
  if (m->nplugins == m->capplugins) {
    size_t ncap = m->capplugins ? m->capplugins * 2 : 4;
    plugin **np = ccord_realloc_array(m->plugins, ncap, sizeof *np);
    if (!np) return nullptr;
    m->plugins = np;
    m->capplugins = ncap;
  }
  plugin *p = ccord_calloc(1, sizeof *p);
  if (!p) return nullptr;
  m->plugins[m->nplugins++] = p;
  p->used = true;
  p->enabled = true;
#if !CCORD_OS_WINDOWS && !CCORD_OS_ANDROID
  p->pid = -1;
#endif
  p->next_id = 1;
  p->state = CCORD_PLUGIN_STARTING;
  return p;
}

static void record_failed(ccord_plugins *m, const char *dir_name, const char *reason)
{
  plugin *p = new_record(m);
  if (!p) {
    CCORD_LOG_W("extensions: too many plugins, skipping %s", dir_name);
    return;
  }
  pl_set_text(p->m.id, sizeof p->m.id, dir_name);
  pl_set_text(p->m.name, sizeof p->m.name, dir_name);
  pl_set_text(p->m.version, sizeof p->m.version, "0.0.0");
  p->state = CCORD_PLUGIN_FAILED;
  pl_set_text(p->error, sizeof p->error, reason);
  CCORD_LOG_E("plugin %s rejected: %s", dir_name, reason);
  pl_publish_state(m, p);
}

bool pl_read_file(const char *path, size_t max, char **out, size_t *len, char *why, size_t why_cap)
{
  uint64_t fsz = 0;
  if (!ccord_path_is_file(path) || !ccord_file_size(path, &fsz) || fsz > (uint64_t)max) {
    pl_set_text(why, why_cap, "file is not a regular file of acceptable size");
    return false;
  }
  FILE *f = fopen(path, "rb");
  if (!f) {
    snprintf(why, why_cap, "cannot open %s: %s", path, strerror(errno));
    return false;
  }
  size_t n = (size_t)fsz;
  char *buf = ccord_malloc(n + 1);
  if (!buf) {
    pl_set_text(why, why_cap, "out of memory");
    fclose(f);
    return false;
  }
  size_t got = n ? fread(buf, 1, n, f) : 0;
  fclose(f);
  if (got != n) {
    ccord_free(buf);
    pl_set_text(why, why_cap, "short read");
    return false;
  }
  buf[n] = '\0';
  *out = buf;
  *len = n;
  return true;
}

bool pl_resolve_real_path(const char *in, char *out, size_t cap)
{
#if CCORD_OS_WINDOWS
  if (!in || !out || cap == 0) return false;
  int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, nullptr, 0);
  if (wlen <= 0 || wlen > 32768) return false;
  wchar_t *wpath = ccord_malloc((size_t)wlen * sizeof(wchar_t));
  if (!wpath) return false;
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, wpath, wlen);
  HANDLE h = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  ccord_free(wpath);
  if (h == INVALID_HANDLE_VALUE) return false;
  wchar_t wout[CCORD_PATH_MAX];
  DWORD got = GetFinalPathNameByHandleW(h, wout, (DWORD)(sizeof wout / sizeof wout[0]), FILE_NAME_NORMALIZED);
  CloseHandle(h);
  if (got == 0 || got >= (DWORD)(sizeof wout / sizeof wout[0])) return false;
  const wchar_t *src = wout;
  if (wcsncmp(src, L"\\\\?\\", 4) == 0) src += 4;
  int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, out, (int)cap, nullptr, nullptr);
  return n > 0 && (size_t)n <= cap;
#else
  if (!in || !out || cap < PATH_MAX) return false;
  return realpath(in, out) != nullptr;
#endif
}

bool pl_path_strictly_under(const char *real_path, const char *real_dir)
{
  size_t dl = strlen(real_dir);
  if (dl == 0) return false;
  while (dl > 1 && (real_dir[dl - 1] == '/' || real_dir[dl - 1] == '\\')) dl--;
#if CCORD_OS_WINDOWS
  if (_strnicmp(real_path, real_dir, dl) != 0) return false;
#else
  if (strncmp(real_path, real_dir, dl) != 0) return false;
#endif
  return real_path[dl] == '/' || real_path[dl] == '\\';
}

static void consider_dir(ccord_plugins *m, const char *dir_path, const char *dir_name,
                         const ccord_json *consent_root, size_t *started)
{
  char manifest_path[PBUF + 64];
  int mp_len = snprintf(manifest_path, sizeof manifest_path, "%s/plugin.json", dir_path);
  if (mp_len < 0 || (size_t)mp_len >= sizeof manifest_path) return;
  char *mbuf = nullptr;
  size_t mlen = 0;
  char why[256];
  if (!pl_read_file(manifest_path, 16u << 20, &mbuf, &mlen, why, sizeof why)) return;   // not a plugin directory
  ccord_manifest man;
  ccord_error err = {0};
  ccord_status st = ccord_manifest_parse(mbuf, mlen, &man, &err);
  ccord_free(mbuf);
  if (st != CCORD_OK) {
    record_failed(m, dir_name, err.message);
    ccord_contrib_add_plugin_log(m->contrib, dir_name, m->now_ms, (uint8_t)CCORD_LOG_ERROR, err.message);
    return;
  }
  char compat_why[160] = "";
  if (!ccord_manifest_supports(&man, m->plat_bit, m->plat_capabilities, compat_why, sizeof compat_why)) {
    CCORD_LOG_I("extensions: skipping %s (%s)", man.id, compat_why);
    record_failed(m, man.id, compat_why);
    ccord_contrib_add_plugin_log(m->contrib, man.id, m->now_ms, (uint8_t)CCORD_LOG_INFO, compat_why);
    return;
  }
  for (size_t i = 0; i < m->nplugins; i++) {
    if (m->plugins[i]->used && strcmp(m->plugins[i]->m.id, man.id) == 0) {
      CCORD_LOG_W("extensions: duplicate plugin id %s in %s skipped", man.id, dir_path);
      return;
    }
  }
  // main.lua must resolve inside the plugin directory (no symlink escape).
  char real_dir[CCORD_PATH_MAX], real_main[CCORD_PATH_MAX], main_path[PBUF + 128];
  if (!pl_resolve_real_path(dir_path, real_dir, sizeof real_dir)) {
    record_failed(m, man.id, "plugin directory cannot be resolved");
    return;
  }
  int main_len = snprintf(main_path, sizeof main_path, "%s/%s", dir_path, man.main);
  if (main_len < 0 || (size_t)main_len >= sizeof main_path) {
    record_failed(m, man.id, "main file path too long");
    return;
  }
  if (!pl_resolve_real_path(main_path, real_main, sizeof real_main)) {
    record_failed(m, man.id, "main file is missing");
    return;
  }
  size_t dl = strlen(real_dir);
  bool under_dir = pl_path_strictly_under(real_main, real_dir);
  if (!under_dir) {
    record_failed(m, man.id, "main file resolves outside the plugin directory");
    return;
  }
  if (dl >= CCORD_PATH_MAX) {
    record_failed(m, man.id, "plugin directory path too long");
    return;
  }
  plugin *p = new_record(m);
  if (!p) {
    CCORD_LOG_W("extensions: too many plugins, skipping %s", man.id);
    return;
  }
  p->m = man;
  p->is_bundled = (strncmp(man.id, "dev.ccord.", 10) == 0);
  char pbuf[1024] = "";
  ccord_perm_list(man.perms, pbuf, sizeof pbuf);
  p->perm_str = ccord_strdup(pbuf);
  char hbuf[1024] = "";
  plugins_ext_format_allowed_hosts(&man, hbuf, sizeof hbuf);
  p->allowed_hosts_str = ccord_strdup(hbuf);
  p->dir = ccord_strdup(real_dir);
  if (!p->dir || !p->perm_str) {
    p->state = CCORD_PLUGIN_FAILED;
    pl_set_text(p->error, sizeof p->error, "out of memory");
    pl_publish_state(m, p);
    return;
  }
  char rev_reason[256] = "";
  if (m->catalog && ccord_catalog_is_revoked(m->catalog, man.id, rev_reason, sizeof rev_reason)) {
    p->revoked = true;
    p->enabled = false;
    p->consent_granted = false;
    p->consented_perms = 0;
    p->state = CCORD_PLUGIN_FAILED;
    pl_set_text(p->error, sizeof p->error, rev_reason[0] ? rev_reason : "Revoked by C-Cord moderators");
    pl_publish_state(m, p);
    return;
  }
  if (!pl_read_file(real_main, 16u << 20, &p->source, &p->source_len, why, sizeof why)) {
    p->state = CCORD_PLUGIN_FAILED;
    pl_set_text(p->error, sizeof p->error, why);
    CCORD_LOG_E("plugin %s rejected: %s", man.id, why);
    pl_publish_state(m, p);
    ccord_contrib_add_plugin_log(m->contrib, man.id, m->now_ms, (uint8_t)CCORD_LOG_ERROR, why);
    return;
  }
  pl_storage_setup(m, p);
  p->consented_perms = ccord_perm_max_class(man.perms) == CCORD_PERM_CLASS_SAFE ? man.perms : 0;
  pl_consent_apply(consent_root, man.id, man.perms, &p->enabled, &p->consent_granted, &p->consented_perms);
  if (ccord_perm_max_class(man.perms) == CCORD_PERM_CLASS_SAFE) {
    p->consent_granted = true;
    p->consented_perms = man.perms;
  }
  if (!p->consent_granted) {
    ccord_free(p->source);
    p->source = nullptr;
    p->source_len = 0;
    p->enabled = false;
    p->state = CCORD_PLUGIN_NEEDS_CONSENT;
    pl_publish_state(m, p);
    return;
  }
  if (!p->enabled) {
    ccord_free(p->source);
    p->source = nullptr;
    p->source_len = 0;
    p->state = CCORD_PLUGIN_STOPPED;
    pl_publish_state(m, p);
    return;
  }
  pl_publish_state(m, p);
  if (pl_spawn_host(m, p, &err) != CCORD_OK) {
    char reason[256];
    snprintf(reason, sizeof reason, "could not start host: %s", err.message);
    pl_fail_plugin(m, p, reason);
    return;
  }
  (*started)++;
}

size_t ccord_plugins_load_all(ccord_plugins *m)
{
  if (!m) return 0;
  // Parse consent.json once for the entire discovery pass instead of once per plugin.
  ccord_arena consent_ar = {.chunk_size = 4096u};
  const ccord_json *consent_root = m->nsearch > 0 ? pl_consent_parse_file(m, &consent_ar) : nullptr;
  size_t started = 0;
  for (size_t i = 0; i < m->nsearch; i++) {
    ccord_dir *d = ccord_dir_open(m->search[i]);
    if (!d) {
      CCORD_LOG_D("extensions: %s not readable", m->search[i]);
      continue;
    }
    char ename[256];
    bool is_dir = false;
    while (ccord_dir_next(d, ename, sizeof ename, &is_dir)) {
      if (ename[0] == '.') continue;
      char path[PBUF + 32];
      int n = snprintf(path, sizeof path, "%s/%s", m->search[i], ename);
      if (n < 0 || (size_t)n >= sizeof path) continue;
      if (!is_dir && !ccord_path_is_dir(path)) continue;
      consider_dir(m, path, ename, consent_root, &started);
    }
    ccord_dir_close(d);
  }
  if (consent_ar.head) ccord_arena_free(&consent_ar);
  if (m->catalog) ccord_catalog_enforce_revocations(m->catalog);
  return started;
}

bool pl_reload_plugin_source(plugin *p, char *why, size_t why_cap)
{
  if (!p->dir || !p->dir[0]) {
    pl_set_text(why, why_cap, "plugin directory unavailable");
    return false;
  }
  char real_dir[CCORD_PATH_MAX], real_main[CCORD_PATH_MAX], main_path[PBUF + 128];
  if (!pl_resolve_real_path(p->dir, real_dir, sizeof real_dir)) {
    pl_set_text(why, why_cap, "plugin directory cannot be resolved");
    return false;
  }
  int main_len = snprintf(main_path, sizeof main_path, "%s/%s", p->dir, p->m.main);
  if (main_len < 0 || (size_t)main_len >= sizeof main_path) {
    pl_set_text(why, why_cap, "main file path too long");
    return false;
  }
  if (!pl_resolve_real_path(main_path, real_main, sizeof real_main)) {
    pl_set_text(why, why_cap, "main file is missing");
    return false;
  }
  if (!pl_path_strictly_under(real_main, real_dir)) {
    pl_set_text(why, why_cap, "main file resolves outside the plugin directory");
    return false;
  }
  return pl_read_file(real_main, 16u << 20, &p->source, &p->source_len, why, why_cap);
}

bool ccord_plugins_export_cce(ccord_plugins *m, const char *id, const char *dest_path, char *err_buf, size_t err_cap)
{
  if (!m || !id || !dest_path) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "invalid arguments");
    return false;
  }
  plugin *p = pl_find_owner(m, id);
  if (!p || !p->dir || !p->dir[0]) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "plugin not found or has no directory");
    return false;
  }
  return ccord_zip_pack_dir(p->dir, dest_path, err_buf, err_cap);
}

bool pl_import_package_internal(ccord_plugins *m, const char *pkg_path,
                                    const char *expected_id, const char *expected_version,
                                    uint64_t expected_perms, bool from_catalog,
                                    char *err_buf, size_t err_cap)
{
  if (!m || !pkg_path || !m->has_data_dir) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "plugin data directory unavailable");
    return false;
  }
  char base[PBUF], tmp_dir[PBUF + 32], inst_root[PBUF + 32];
  snprintf(base, sizeof base, "%s/plugins", m->data_dir);
  pl_mkdir_ok(m->data_dir);
  pl_mkdir_ok(base);
  snprintf(inst_root, sizeof inst_root, "%s/installed", base);
  pl_mkdir_ok(inst_root);
  snprintf(tmp_dir, sizeof tmp_dir, "%s/.import_tmp_%llu", base, (unsigned long long)m->now_ms);
  if (!ccord_zip_extract(pkg_path, tmp_dir, err_buf, err_cap)) {
    pl_rm_dir_recursive(tmp_dir);
    return false;
  }

  char man_path[PBUF + 64], why[256];
  snprintf(man_path, sizeof man_path, "%s/plugin.json", tmp_dir);
  char *mbuf = nullptr; size_t mlen = 0;
  if (!pl_read_file(man_path, 16u << 20, &mbuf, &mlen, why, sizeof why)) {
    pl_rm_dir_recursive(tmp_dir);
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "package is missing plugin.json");
    return false;
  }
  ccord_manifest man; ccord_error err = {0};
  ccord_status st = ccord_manifest_parse(mbuf, mlen, &man, &err);
  ccord_free(mbuf);
  if (st != CCORD_OK) {
    pl_rm_dir_recursive(tmp_dir);
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", err.message);
    return false;
  }

  if (strncmp(man.id, "dev.ccord.", 10) == 0) {
    pl_rm_dir_recursive(tmp_dir);
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "cannot overwrite bundled extension");
    return false;
  }
  if (from_catalog) {
    if (!expected_id || strcmp(man.id, expected_id) != 0) {
      pl_rm_dir_recursive(tmp_dir);
      if (err_buf && err_cap) snprintf(err_buf, err_cap, "package id '%s' does not match catalog id '%s'",
                                       man.id, expected_id ? expected_id : "");
      return false;
    }
    if (expected_version && expected_version[0] && strcmp(man.version, expected_version) != 0) {
      pl_rm_dir_recursive(tmp_dir);
      if (err_buf && err_cap) snprintf(err_buf, err_cap, "package version '%s' does not match catalog version '%s'",
                                       man.version, expected_version);
      return false;
    }
    // Reject packages declaring restricted permissions absent from the verified catalog listing.
    if ((man.perms & ~expected_perms) != 0) {
      pl_rm_dir_recursive(tmp_dir);
      if (err_buf && err_cap) snprintf(err_buf, err_cap, "package declares permissions absent from catalog entry");
      return false;
    }
  }

  plugin *existing = pl_find_owner(m, man.id);
  char final_dir[PBUF + 128];
  snprintf(final_dir, sizeof final_dir, "%s/%s", inst_root, man.id);
  if (ccord_path_exists(final_dir)) {
    pl_rm_dir_recursive(final_dir);
  }
  if (rename(tmp_dir, final_dir) != 0) {
    pl_rm_dir_recursive(tmp_dir);
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "rename failed: %s", strerror(errno));
    return false;
  }

  if (existing) {
    char *nd = ccord_strdup(final_dir);
    if (!nd) {
      if (err_buf && err_cap) snprintf(err_buf, err_cap, "out of memory");
      return false;
    }
    ccord_free(existing->dir);
    existing->dir = nd;
    if (!from_catalog) {
      // Manual .cce overwrite must never silently inherit prior Class 2/3 consent.
      existing->consent_granted = false;
      existing->consented_perms = 0;
    }
    pl_on_plugin_reload(m, man.id);
    return true;
  }
  size_t started = 0;
  ccord_arena consent_ar = {.chunk_size = 4096u};
  const ccord_json *consent_root = from_catalog ? pl_consent_parse_file(m, &consent_ar) : nullptr;
  consider_dir(m, final_dir, man.id, consent_root, &started);
  if (consent_ar.head) ccord_arena_free(&consent_ar);
  return true;
}

bool ccord_plugins_import_package(ccord_plugins *m, const char *pkg_path, char *err_buf, size_t err_cap)
{
  return pl_import_package_internal(m, pkg_path, nullptr, nullptr, 0, false, err_buf, err_cap);
}

bool ccord_plugins_uninstall(ccord_plugins *m, const char *id, char *err_buf, size_t err_cap)
{
  if (!m || !id || !id[0]) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "invalid plugin id");
    return false;
  }
  char id_copy[65];
  pl_set_text(id_copy, sizeof id_copy, id);
  plugin *p = pl_find_owner(m, id_copy);
  if (!p) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "plugin not found");
    return false;
  }
  if (p->is_bundled || strncmp(p->m.id, "dev.ccord.", 10) == 0) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "cannot uninstall bundled extension");
    return false;
  }
  pl_stop_host(m, p);
  ccord_contrib_remove_owner(m->contrib, id_copy);
  (void)ccord_contrib_remove_plugin(m->contrib, id_copy);

  if (m->has_data_dir) {
    char inst_dir[PBUF + 128];
    snprintf(inst_dir, sizeof inst_dir, "%s/plugins/installed/%s", m->data_dir, id_copy);
    if (ccord_path_exists(inst_dir)) pl_rm_dir_recursive(inst_dir);
  }
  for (size_t k = 0; k < p->nitems; k++) ccord_free(p->items[k].json);
  ccord_free(p->items);
  ccord_free(p->source);
  ccord_free(p->dir);
  ccord_free(p->storage_dir);
  ccord_free(p->perm_str);
  ccord_free(p->allowed_hosts_str);
  plugins_ext_reset_plugin(p);
  p->used = false;
  p->m.id[0] = '\0';
  p->revoked = false;
  p->items = nullptr;
  p->nitems = 0;
  p->cap_items = 0;
  p->store_bytes = 0;
  p->source = nullptr;
  p->source_len = 0;
  p->dir = nullptr;
  p->storage_dir = nullptr;
  p->perm_str = nullptr;
  p->allowed_hosts_str = nullptr;
  pl_consent_save(m);
  if (m->catalog) ccord_catalog_on_uninstalled(m->catalog, id_copy);
  return true;
}

