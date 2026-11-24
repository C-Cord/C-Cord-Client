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

#include <ctype.h>
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


// ---------------------------------------------------------------- storage (main owns the values)

stored_value *pl_store_find(plugin *p, const char *key)
{
  for (size_t i = 0; i < p->nitems; i++) {
    if (strcmp(p->items[i].key, key) == 0) return &p->items[i];
  }
  return nullptr;
}

void pl_store_remove_at(plugin *p, size_t i)
{
  p->store_bytes -= strlen(p->items[i].key) + p->items[i].json_len;
  ccord_free(p->items[i].json);
  memmove(&p->items[i], &p->items[i + 1], (p->nitems - i - 1) * sizeof p->items[0]);
  p->nitems--;
}

ccord_status pl_store_put(plugin *p, const char *key, const char *json, size_t jlen)
{
  size_t klen = strlen(key);
  stored_value *old = pl_store_find(p, key);
  if (old) {
    size_t old_entry = strlen(old->key) + old->json_len;
    size_t base = p->store_bytes >= old_entry ? p->store_bytes - old_entry : 0;
    if (jlen > MAX_STORE_BYTES || klen > MAX_STORE_BYTES - jlen || base > MAX_STORE_BYTES - (klen + jlen)) {
      return CCORD_ERR_RANGE;
    }
    char *fresh = ccord_malloc(jlen + 1);
    if (!fresh) return CCORD_ERR_NOMEM;
    memcpy(fresh, json, jlen);
    fresh[jlen] = '\0';
    ccord_free(old->json);
    old->json = fresh;
    old->json_len = jlen;
    p->store_bytes = base + klen + jlen;
    p->dirty = true;
    return CCORD_OK;
  }
  if (p->nitems >= MAX_STORE_KEYS) return CCORD_ERR_RANGE;
  if (jlen > MAX_STORE_BYTES || klen > MAX_STORE_BYTES - jlen || p->store_bytes > MAX_STORE_BYTES - (klen + jlen)) {
    return CCORD_ERR_RANGE;
  }
  if (p->nitems == p->cap_items) {
    size_t cap = p->cap_items ? p->cap_items * 2 : 8;
    stored_value *n = ccord_realloc(p->items, cap * sizeof *n);
    if (!n) return CCORD_ERR_NOMEM;
    p->items = n;
    p->cap_items = cap;
  }
  char *fresh = ccord_malloc(jlen + 1);
  if (!fresh) return CCORD_ERR_NOMEM;
  memcpy(fresh, json, jlen);
  fresh[jlen] = '\0';
  stored_value *v = &p->items[p->nitems];
  memset(v, 0, sizeof *v);
  snprintf(v->key, sizeof v->key, "%s", key);
  v->json = fresh;
  v->json_len = jlen;
  p->nitems++;
  p->store_bytes += klen + jlen;
  p->dirty = true;
  return CCORD_OK;
}


void pl_storage_load(plugin *p)
{
  if (!p->storage_enabled || !p->storage_dir) return;
  char path[PBUF + 32];
  int n = snprintf(path, sizeof path, "%s/storage.json", p->storage_dir);
  if (n < 0 || (size_t)n >= sizeof path) {
    CCORD_LOG_W("plugin %s: storage file path too long", p->m.id);
    return;
  }
  if (!ccord_path_exists(path)) return;
  uint64_t fsz = 0;
  if (!ccord_path_is_file(path) || !ccord_file_size(path, &fsz) || fsz > (uint64_t)(16u << 20)) {
    CCORD_LOG_W("plugin %s: storage file rejected (size or type)", p->m.id);
    return;
  }
  FILE *f = fopen(path, "rb");
  if (!f) {
    CCORD_LOG_W("plugin %s: storage not loaded: %s", p->m.id, strerror(errno));
    return;
  }
  size_t len = (size_t)fsz;
  char *buf = ccord_malloc(len + 1);
  if (!buf) {
    fclose(f);
    return;
  }
  size_t got = len ? fread(buf, 1, len, f) : 0;
  fclose(f);
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error err = {0};
  const ccord_json *root = got == len ? ccord_json_parse(&arena, buf, len, &pl_json_limits, &err) : nullptr;
  bool ok = root && ccord_json_type_of(root) == CCORD_JSON_OBJECT;
  for (size_t i = 0; ok && i < ccord_json_object_len(root); i++) {
    const ccord_json *v = nullptr;
    const char *k = ccord_json_object_key(root, i, &v);
    ok = v != nullptr && pl_valid_store_key(k, strlen(k));
    if (!ok) break;
    ccord_buf b;
    ccord_buf_init(&b, 0);
    ccord_jw w;
    ccord_jw_init(&w, &b);
    pl_jw_copy_json(&w, v);
    if (ccord_jw_ok(&w) && pl_store_put(p, k, ccord_buf_cstr(&b), b.len) == CCORD_OK) {
      p->dirty = false;
    } else {
      ok = false;
    }
    ccord_buf_free(&b);
  }
  if (!ok) CCORD_LOG_W("plugin %s: storage file is not valid, starting empty", p->m.id);
  if (!ok) {
    for (size_t i = p->nitems; i > 0; i--) pl_store_remove_at(p, i - 1);
  }
  ccord_arena_free(&arena);
  ccord_free(buf);
}

void pl_mkdir_ok(const char *path)
{
  ccord_error err = {0};
  if (ccord_mkdir_p(path, 0700, &err) != CCORD_OK) CCORD_LOG_W("storage: mkdir %s: %s", path, err.message);
}

void pl_storage_save(ccord_plugins *m, plugin *p, uint64_t now, bool sync)
{
  if (!p->storage_enabled || !p->dirty || !p->storage_dir || !m->data_dir) return;
  ccord_buf b;
  ccord_buf_init(&b, 0);
  bool ok = pl_app_raw(&b, "{");
  for (size_t i = 0; ok && i < p->nitems; i++) {
    if (i) ok = pl_app_raw(&b, ",");
    ok = ok && pl_app_str(&b, p->items[i].key, strlen(p->items[i].key)) && pl_app_raw(&b, ":") &&
         ccord_buf_append(&b, p->items[i].json, p->items[i].json_len) == CCORD_OK;
  }
  ok = ok && pl_app_raw(&b, "}");
  if (!ok) {
    CCORD_LOG_W("plugin %s: storage encoding failed", p->m.id);
    ccord_buf_free(&b);
    return;
  }
  pl_mkdir_ok(m->data_dir);
  char plugins_dir[PBUF];
  int n1 = snprintf(plugins_dir, sizeof plugins_dir, "%s/plugins", m->data_dir);
  if (n1 < 0 || (size_t)n1 >= sizeof plugins_dir) {
    CCORD_LOG_W("plugin %s: plugins directory path too long", p->m.id);
    ccord_buf_free(&b);
    return;
  }
  pl_mkdir_ok(plugins_dir);
  pl_mkdir_ok(p->storage_dir);
  char final_path[PBUF + 32];
  int n3 = snprintf(final_path, sizeof final_path, "%s/storage.json", p->storage_dir);
  if (n3 < 0 || (size_t)n3 >= sizeof final_path) {
    CCORD_LOG_W("plugin %s: storage file path too long", p->m.id);
    ccord_buf_free(&b);
    return;
  }
  ccord_error werr = {0};
  if (ccord_file_write_atomic(final_path, b.data, b.len, 0600, sync, &werr) != CCORD_OK) {
    CCORD_LOG_W("plugin %s: storage save failed: %s", p->m.id, werr.message);
  } else {
    p->dirty = false;
    p->last_save_ms = now;
  }
  ccord_buf_free(&b);
}

void pl_storage_setup(ccord_plugins *m, plugin *p)
{
  p->storage_enabled = m->has_data_dir && m->data_dir != nullptr;
  if (!p->storage_enabled) return;
  char dir[PBUF];
  int n = snprintf(dir, sizeof dir, "%s/plugins/%s", m->data_dir, p->m.id);
  if (n < 0 || (size_t)n >= sizeof dir) {
    p->storage_enabled = false;
    CCORD_LOG_W("plugin %s: storage directory path truncated", p->m.id);
    return;
  }
  ccord_free(p->storage_dir);
  p->storage_dir = ccord_malloc((size_t)n + 1u);
  if (!p->storage_dir) {
    p->storage_enabled = false;
    return;
  }
  memcpy(p->storage_dir, dir, (size_t)n + 1u);
  pl_storage_load(p);
}

const ccord_json *pl_consent_parse_file(const ccord_plugins *m, ccord_arena *ar)
{
  if (!m || !m->has_data_dir || !m->data_dir) return nullptr;
  char path[PBUF + 32];
  snprintf(path, sizeof path, "%s/plugins/consent.json", m->data_dir);
  FILE *f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > (1 << 20)) { fclose(f); return nullptr; }
  char *buf = ccord_malloc((size_t)sz + 1);
  if (!buf) { fclose(f); return nullptr; }
  size_t rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[rd] = '\0';
  ccord_error err = {0};
  const ccord_json *root = ccord_json_parse(ar, buf, rd, &pl_json_limits, &err);
  ccord_free(buf);
  return root;
}

void pl_format_hosts_csv(const ccord_manifest *man, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!man || man->allowed_host_count == 0) return;
  size_t off = 0;
  for (size_t i = 0; i < man->allowed_host_count; i++) {
    const char *h = man->allowed_hosts[i];
    size_t hl = strlen(h);
    if (hl == 0) continue;
    if (off > 0 && off + 1 < cap) out[off++] = ',';
    if (off + hl >= cap) break;
    for (size_t k = 0; k < hl; k++) {
      out[off++] = (char)tolower((unsigned char)h[k]);
    }
    out[off] = '\0';
  }
}

bool pl_consent_apply(const ccord_json *root, const char *id, const ccord_manifest *man,
                      bool *out_enabled, bool *out_consent, ccord_perm_set *out_consented_perms,
                      bool *out_has_consented_hosts, char *out_consented_hosts, size_t hosts_cap,
                      bool *out_migrated)
{
  uint64_t perms = man ? man->perms : 0;
  *out_enabled = strcmp(id, "dev.ccord.starter-example") != 0;
  *out_consent = ccord_perm_max_class(perms) == CCORD_PERM_CLASS_SAFE;
  *out_consented_perms = ccord_perm_max_class(perms) == CCORD_PERM_CLASS_SAFE ? perms : 0;
  if (out_has_consented_hosts) *out_has_consented_hosts = false;
  if (out_consented_hosts && hosts_cap > 0) out_consented_hosts[0] = '\0';
  if (out_migrated) *out_migrated = false;

  const ccord_json *entry = root ? ccord_json_get(root, id) : nullptr;
  if (entry) {
    *out_enabled = ccord_json_bool(ccord_json_get(entry, "enabled"), true);
    int64_t ap = 0;
    if (ccord_json_int64(ccord_json_get(entry, "perms"), &ap)) {
      *out_consented_perms = (uint64_t)ap;
      if (((uint64_t)ap & perms) == perms) *out_consent = true;
    } else {
      const ccord_json *consent_bool = ccord_json_get(entry, "consent");
      if (consent_bool && ccord_json_bool(consent_bool, false)) {
        *out_consent = true;
        *out_consented_perms = perms;
      }
    }

    const ccord_json *hosts_j = ccord_json_get(entry, "allowed_hosts");
    const ccord_json *hc_j = ccord_json_get(entry, "allowed_hosts_consented");
    bool has_hosts_field = (hosts_j && ccord_json_type_of(hosts_j) == CCORD_JSON_ARRAY) ||
                           (hc_j && ccord_json_bool(hc_j, false));
    bool had_consented_hosts = false;
    if (has_hosts_field) {
      had_consented_hosts = true;
      if (hosts_j && ccord_json_type_of(hosts_j) == CCORD_JSON_ARRAY &&
          out_consented_hosts && hosts_cap > 0) {
        size_t hn = ccord_json_array_len(hosts_j);
        size_t off = 0;
        for (size_t i = 0; i < hn; i++) {
          size_t sl = 0;
          const char *hs = ccord_json_string(ccord_json_array_at(hosts_j, i), &sl);
          if (!hs || sl == 0) continue;
          if (off > 0 && off + 1 < hosts_cap) out_consented_hosts[off++] = ',';
          if (off + sl >= hosts_cap) break;
          for (size_t k = 0; k < sl; k++) {
            out_consented_hosts[off++] = (char)tolower((unsigned char)hs[k]);
          }
          out_consented_hosts[off] = '\0';
        }
      }
    } else if ((*out_consented_perms & ((uint64_t)CCORD_PERM_NETWORK_HTTP | (uint64_t)CCORD_PERM_NETWORK_WS)) != 0) {
      // Backwards compatibility: existing consent.json predates allowed_hosts persistence.
      // Snapshot the currently installed manifest's allowed_hosts so unchanged plugins work seamlessly
      // while future updates that expand allowed_hosts require explicit user consent.
      had_consented_hosts = true;
      if (man && out_consented_hosts && hosts_cap > 0) {
        pl_format_hosts_csv(man, out_consented_hosts, hosts_cap);
      }
      if (out_migrated) *out_migrated = true;
    }

    if (out_has_consented_hosts) *out_has_consented_hosts = had_consented_hosts;
    if (*out_consent && man) {
      char cur_hosts[1024] = "";
      pl_format_hosts_csv(man, cur_hosts, sizeof cur_hosts);
      if (ccord_hosts_expanded(out_consented_hosts ? out_consented_hosts : "",
                               had_consented_hosts, cur_hosts, man->perms)) {
        *out_consent = false;
      }
    }
  }
  return entry != nullptr;
}

void pl_consent_save(const ccord_plugins *m)
{
  if (!m->has_data_dir) return;
  char dir[PBUF];
  snprintf(dir, sizeof dir, "%s/plugins", m->data_dir);
  pl_mkdir_ok(m->data_dir);
  pl_mkdir_ok(dir);
  char path[PBUF + 32];
  snprintf(path, sizeof path, "%s/consent.json", dir);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  for (size_t i = 0; i < m->nplugins; i++) {
    const plugin *p = m->plugins[i];
    if (!p->used) continue;
    ccord_jw_key(&w, p->m.id);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "enabled"); ccord_jw_bool(&w, p->enabled);
    ccord_jw_key(&w, "perms"); ccord_jw_int(&w, (int64_t)p->consented_perms);
    if (p->has_consented_hosts) {
      ccord_jw_key(&w, "allowed_hosts_consented"); ccord_jw_bool(&w, true);
      ccord_jw_key(&w, "allowed_hosts");
      ccord_jw_begin_array(&w);
      const char *hp = p->consented_hosts_csv;
      while (*hp) {
        while (*hp && (*hp == ',' || isspace((unsigned char)*hp))) hp++;
        const char *hs = hp;
        while (*hp && *hp != ',') hp++;
        const char *he = hp;
        while (he > hs && isspace((unsigned char)he[-1])) he--;
        if (he > hs) ccord_jw_string(&w, hs, (size_t)(he - hs));
      }
      ccord_jw_end_array(&w);
    }
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) {
    ccord_error err = {0};
    (void)ccord_file_write_atomic(path, b.data, b.len, 0600, false, &err);
  }
  ccord_buf_free(&b);
}

// Registers a plugin that could not start: it shows in Settings with its reason.
