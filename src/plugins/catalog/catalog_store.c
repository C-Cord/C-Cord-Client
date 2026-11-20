// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "catalog_internal.h"

void cat_mkdir_ok(const char *p)
{
  if (p && p[0]) {
    ccord_error err = {0};
    (void)ccord_mkdir_p(p, 0700, &err);
  }
}

char *cat_read_entire_file(const char *path, size_t max_bytes, size_t *out_len)
{
  FILE *fp = fopen(path, "rb");
  if (!fp) return nullptr;
  if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return nullptr; }
  long sz = ftell(fp);
  if (sz <= 0 || (size_t)sz > max_bytes || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return nullptr; }
  char *buf = ccord_malloc((size_t)sz + 1);
  if (!buf) { fclose(fp); return nullptr; }
  size_t rd = fread(buf, 1, (size_t)sz, fp);
  fclose(fp);
  buf[rd] = '\0';
  if (out_len) *out_len = rd;
  return buf;
}

void cat_json_str_copy(const ccord_json *obj, const char *key, char *dst, size_t cap)
{
  dst[0] = '\0';
  if (!obj) return;
  const ccord_json *v = ccord_json_get(obj, key);
  if (!v) return;
  size_t len = 0;
  const char *s = ccord_json_string(v, &len);
  if (!s) return;
  if (len >= cap) len = cap - 1;
  memcpy(dst, s, len);
  dst[len] = '\0';
}

// ---------------------------------------------------------------- persistence (tiny 0600 atomic files)

void cat_save_prefs(const ccord_catalog *cat)
{
  if (!cat || !cat->has_data_dir) return;
  char dir[CCORD_PATH_MAX + 32], path[CCORD_PATH_MAX + 64];
  snprintf(dir, sizeof dir, "%s/plugins", cat->data_dir);
  cat_mkdir_ok(cat->data_dir);
  cat_mkdir_ok(dir);
  snprintf(path, sizeof path, "%s/catalog-prefs.json", dir);
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "auto_update_enabled"); ccord_jw_bool(&w, cat->prefs.auto_update_enabled);
  ccord_jw_key(&w, "auto_update_min_tier"); ccord_jw_int(&w, cat->prefs.auto_update_min_tier);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) {
    ccord_error err = {0};
    (void)ccord_file_write_atomic(path, b.data, b.len, 0600, false, &err);
  }
  ccord_buf_free(&b);
}

void cat_load_prefs(ccord_catalog *cat)
{
  cat->prefs.auto_update_enabled = false;
  cat->prefs.auto_update_min_tier = 2;
  if (!cat->has_data_dir) return;
  char path[CCORD_PATH_MAX + 64];
  snprintf(path, sizeof path, "%s/plugins/catalog-prefs.json", cat->data_dir);
  size_t len = 0;
  char *raw = cat_read_entire_file(path, 65536, &len);
  if (!raw) return;
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error pe = {0};
  const ccord_json_limits lim = {8, 512, 65536};
  const ccord_json *root = ccord_json_parse(&arena, raw, len, &lim, &pe);
  if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    const ccord_json *ae = ccord_json_get(root, "auto_update_enabled");
    if (ae) cat->prefs.auto_update_enabled = ccord_json_bool(ae, false);
    const ccord_json *mt = ccord_json_get(root, "auto_update_min_tier");
    int64_t mv = 2;
    if (mt && ccord_json_int64(mt, &mv) && mv >= 1 && mv <= 3) {
      cat->prefs.auto_update_min_tier = (uint8_t)mv;
    }
  }
  ccord_arena_free(&arena);
  ccord_free(raw);
}

void cat_save_installed_meta(const ccord_catalog *cat)
{
  if (!cat || !cat->has_data_dir) return;
  char dir[CCORD_PATH_MAX + 32], path[CCORD_PATH_MAX + 64];
  snprintf(dir, sizeof dir, "%s/plugins", cat->data_dir);
  cat_mkdir_ok(cat->data_dir);
  cat_mkdir_ok(dir);
  snprintf(path, sizeof path, "%s/installed-meta.json", dir);
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  for (size_t i = 0; i < cat->nmeta; i++) {
    const installed_meta *m = &cat->meta[i];
    ccord_jw_key(&w, m->id);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "version"); ccord_jw_cstr(&w, m->version);
    ccord_jw_key(&w, "sha256"); ccord_jw_cstr(&w, m->sha256);
    ccord_jw_key(&w, "tier"); ccord_jw_int(&w, m->tier);
    ccord_jw_key(&w, "auto_update"); ccord_jw_bool(&w, m->auto_update);
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) {
    ccord_error err = {0};
    (void)ccord_file_write_atomic(path, b.data, b.len, 0600, false, &err);
  }
  ccord_buf_free(&b);
}

void cat_load_installed_meta(ccord_catalog *cat)
{
  if (!cat->has_data_dir) return;
  char path[CCORD_PATH_MAX + 64];
  snprintf(path, sizeof path, "%s/plugins/installed-meta.json", cat->data_dir);
  size_t len = 0;
  char *raw = cat_read_entire_file(path, 512u * 1024u, &len);
  if (!raw) return;
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error pe = {0};
  const ccord_json_limits lim = {8, 4096, 512u * 1024u};
  const ccord_json *root = ccord_json_parse(&arena, raw, len, &lim, &pe);
  if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    size_t n = ccord_json_object_len(root);
    for (size_t i = 0; i < n; i++) {
      const ccord_json *val = nullptr;
      const char *id = ccord_json_object_key(root, i, &val);
      if (!id || !val || ccord_json_type_of(val) != CCORD_JSON_OBJECT) continue;
      installed_meta *m = cat_upsert_meta(cat, id);
      if (!m) break;
      size_t sl = 0;
      const char *s = ccord_json_string(ccord_json_get(val, "version"), &sl);
      if (s) cat_copy_str(m->version, sizeof m->version, s);
      s = ccord_json_string(ccord_json_get(val, "sha256"), &sl);
      if (s) cat_copy_str(m->sha256, sizeof m->sha256, s);
      int64_t tv = 0;
      if (ccord_json_int64(ccord_json_get(val, "tier"), &tv) && tv >= 0 && tv <= 3) {
        m->tier = (ccord_plugin_tier)tv;
      }
      const ccord_json *au = ccord_json_get(val, "auto_update");
      m->auto_update = au ? ccord_json_bool(au, true) : true;
    }
  }
  ccord_arena_free(&arena);
  ccord_free(raw);
}

void cat_save_revocations_cache(const ccord_catalog *cat)
{
  if (!cat || !cat->has_data_dir) return;
  char dir[CCORD_PATH_MAX + 32], path[CCORD_PATH_MAX + 64];
  snprintf(dir, sizeof dir, "%s/plugins", cat->data_dir);
  cat_mkdir_ok(cat->data_dir);
  cat_mkdir_ok(dir);
  snprintf(path, sizeof path, "%s/revocations-cache.json", dir);
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "schema"); ccord_jw_int(&w, 2);
  ccord_jw_key(&w, "seq"); ccord_jw_int(&w, cat->rev_seq);
  ccord_jw_key(&w, "etag"); ccord_jw_cstr(&w, cat->rev_etag);
  ccord_jw_key(&w, "revoked");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < cat->nrevoked; i++) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "seq"); ccord_jw_int(&w, cat->revoked[i].seq);
    ccord_jw_key(&w, "sha256"); ccord_jw_cstr(&w, cat->revoked[i].sha256);
    ccord_jw_key(&w, "plugin_id"); ccord_jw_cstr(&w, cat->revoked[i].plugin_id);
    ccord_jw_key(&w, "reason"); ccord_jw_cstr(&w, cat->revoked[i].reason);
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) {
    ccord_error err = {0};
    (void)ccord_file_write_atomic(path, b.data, b.len, 0600, false, &err);
  }
  ccord_buf_free(&b);
}

bool cat_merge_revocations_json(ccord_catalog *cat, const char *json, size_t len, bool is_cache_file)
{
  if (!cat || !json || len == 0) return false;
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error pe = {0};
  const ccord_json_limits lim = {12, 16384, CATALOG_MAX_API_BYTES};
  const ccord_json *root = ccord_json_parse(&arena, json, len, &lim, &pe);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return false;
  }
  int64_t seq = 0;
  (void)ccord_json_int64(ccord_json_get(root, "seq"), &seq);
  bool full = is_cache_file;
  const ccord_json *full_j = ccord_json_get(root, "full");
  if (full_j) full = ccord_json_bool(full_j, false);
  if (is_cache_file) {
    cat_json_str_copy(root, "etag", cat->rev_etag, sizeof cat->rev_etag);
  }

  const ccord_json *arr = ccord_json_get(root, "revoked");
  if (arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY) {
    size_t rn = ccord_json_array_len(arr);
    if (full) {
      ccord_free(cat->revoked);
      cat->revoked = nullptr;
      cat->nrevoked = 0;
    }
    if (rn > 0 && rn < 16384) {
      revoked_item *grown = ccord_realloc_array(cat->revoked, cat->nrevoked + rn, sizeof *grown);
      if (grown) {
        cat->revoked = grown;
        for (size_t i = 0; i < rn; i++) {
          const ccord_json *o = ccord_json_array_at(arr, i);
          if (!o || ccord_json_type_of(o) != CCORD_JSON_OBJECT) continue;
          revoked_item item = {0};
          (void)ccord_json_int64(ccord_json_get(o, "seq"), &item.seq);
          cat_json_str_copy(o, "sha256", item.sha256, sizeof item.sha256);
          cat_json_str_copy(o, "plugin_id", item.plugin_id, sizeof item.plugin_id);
          cat_json_str_copy(o, "reason", item.reason, sizeof item.reason);
          if (strlen(item.sha256) != 64) continue;
          bool dup = false;
          for (size_t k = 0; k < cat->nrevoked; k++) {
            if (cat_hex_equal_ci(cat->revoked[k].sha256, item.sha256)) { dup = true; break; }
          }
          if (!dup) cat->revoked[cat->nrevoked++] = item;
        }
      }
    }
  }
  if (seq > cat->rev_seq || full) cat->rev_seq = seq;
  ccord_arena_free(&arena);
  return true;
}

void cat_load_revocations_cache(ccord_catalog *cat)
{
  if (!cat->has_data_dir) return;
  char path[CCORD_PATH_MAX + 64];
  snprintf(path, sizeof path, "%s/plugins/revocations-cache.json", cat->data_dir);
  size_t len = 0;
  char *raw = cat_read_entire_file(path, CATALOG_MAX_API_BYTES, &len);
  if (raw) {
    (void)cat_merge_revocations_json(cat, raw, len, true);
    ccord_free(raw);
  }
}

