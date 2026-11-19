// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "catalog.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/sha256.h>
#include <ccord/ipc/perm.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CATALOG_DEFAULT_URL "https://plugins.c-cord.org/catalog.v1.json"
#define CATALOG_MAX_JSON_BYTES (8u * 1024u * 1024u)
#define CATALOG_MAX_CCE_BYTES (25u * 1024u * 1024u)
#define CATALOG_REFRESH_INTERVAL_MS (6ULL * 60ULL * 60ULL * 1000ULL)
#define CATALOG_STARTUP_DELAY_MS 1500ULL
#define CATALOG_QUEUE_CAP 32

typedef struct cat_item {
  char id[65];
  char name[65];
  char description[201];
  char license[33];
  char author[65];
  char repo[140];
  char repo_url[256];
  char version[33];
  char download_url[512];
  char sha256[65];
  char permissions[512];
  char changelog[512];
  uint64_t size_bytes;
  uint64_t perms_mask;
  uint8_t api_class;
  ccord_plugin_tier tier;
  ccord_catalog_warn_sev warning_sev;
  char warning_title[128];
  char warning_message[512];
  bool installing;
} cat_item;

typedef struct revoked_item {
  char sha256[65];
  char plugin_id[65];
  char reason[200];
} revoked_item;

typedef struct installed_meta {
  char id[65];
  char version[33];
  char sha256[65];
  ccord_plugin_tier tier;
  bool auto_update;
} installed_meta;

typedef enum job_kind { JOB_REFRESH = 1, JOB_INSTALL = 2 } job_kind;

typedef struct worker_job {
  job_kind kind;
  char id[65];
  char url[512];
  char expected_sha256[65];
  char version[33];
  char etag[128];
  char dest_path[CCORD_PATH_MAX + 160];
  ccord_plugin_tier tier;
  bool is_auto_update;
} worker_job;

typedef struct worker_result {
  job_kind kind;
  bool ok;
  bool not_modified;
  bool is_auto_update;
  char id[65];
  char version[33];
  char sha256[65];
  char etag[128];
  char dest_path[CCORD_PATH_MAX + 160];
  ccord_plugin_tier tier;
  char *body;
  size_t body_len;
  char error[256];
} worker_result;

struct ccord_catalog {
  char data_dir[CCORD_PATH_MAX];
  bool has_data_dir;
  ccord_contrib *contrib;
  ccord_catalog_http http;
  ccord_catalog_host_cbs cbs;
  ccord_catalog_prefs prefs;
  ccord_catalog_status status;
  char etag[128];

  cat_item *items;
  size_t nitems;
  revoked_item *revoked;
  size_t nrevoked;
  installed_meta *meta;
  size_t nmeta, capmeta;

  bool initial_refresh_queued;
  uint64_t created_ms;
  uint64_t last_refresh_ms;

  ccord_thread *thread;
  bool thread_started;
  ccord_mutex *mu;
  ccord_cond *cv;
  bool stop;

  worker_job jobs[CATALOG_QUEUE_CAP];
  size_t job_head, job_count;
  worker_result results[CATALOG_QUEUE_CAP];
  size_t res_head, res_count;
};

static void copy_str(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  size_t n = strlen(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static void mkdir_ok(const char *p)
{
  if (p && p[0]) {
    ccord_error err = {0};
    (void)ccord_mkdir_p(p, 0700, &err);
  }
}

static bool parse_semver_triplet(const char *s, uint64_t out[3])
{
  if (!s || !s[0]) return false;
  out[0] = out[1] = out[2] = 0;
  int idx = 0;
  const char *p = s;
  while (*p && idx < 3) {
    if (!isdigit((unsigned char)*p)) return false;
    uint64_t v = 0;
    while (isdigit((unsigned char)*p)) {
      v = v * 10u + (uint64_t)(*p - '0');
      p++;
    }
    out[idx++] = v;
    if (*p == '.') {
      p++;
      if (!*p) return false;
    } else if (*p != '\0') {
      return false;
    }
  }
  return idx == 3 && *p == '\0';
}

int ccord_catalog_semver_cmp(const char *a, const char *b)
{
  uint64_t va[3], vb[3];
  if (!parse_semver_triplet(a, va) || !parse_semver_triplet(b, vb)) {
    return strcmp(a ? a : "", b ? b : "");
  }
  for (int i = 0; i < 3; i++) {
    if (va[i] < vb[i]) return -1;
    if (va[i] > vb[i]) return 1;
  }
  return 0;
}

bool ccord_catalog_sha256_file_hex(const char *path, char hex_out[65])
{
  if (!path || !hex_out) return false;
  FILE *fp = fopen(path, "rb");
  if (!fp) return false;
  ccord_sha256_ctx ctx;
  ccord_sha256_init(&ctx);
  unsigned char buf[16384];
  size_t rd = 0;
  while ((rd = fread(buf, 1, sizeof buf, fp)) > 0) {
    ccord_sha256_update(&ctx, buf, rd);
  }
  bool err = ferror(fp) != 0;
  fclose(fp);
  if (err) return false;
  unsigned char digest[32];
  ccord_sha256_final(&ctx, digest);
  static const char k_hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i++) {
    hex_out[i * 2] = k_hex[digest[i] >> 4];
    hex_out[i * 2 + 1] = k_hex[digest[i] & 0x0f];
  }
  hex_out[64] = '\0';
  return true;
}

static bool hex_equal_ci(const char *a, const char *b)
{
  if (!a || !b) return false;
  if (strlen(a) != 64 || strlen(b) != 64) return false;
  for (size_t i = 0; i < 64; i++) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  }
  return true;
}

static char *read_entire_file(const char *path, size_t max_bytes, size_t *out_len)
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

static installed_meta *find_meta(ccord_catalog *cat, const char *id)
{
  if (!cat || !id) return nullptr;
  for (size_t i = 0; i < cat->nmeta; i++) {
    if (strcmp(cat->meta[i].id, id) == 0) return &cat->meta[i];
  }
  return nullptr;
}

static const installed_meta *find_meta_const(const ccord_catalog *cat, const char *id)
{
  if (!cat || !id) return nullptr;
  for (size_t i = 0; i < cat->nmeta; i++) {
    if (strcmp(cat->meta[i].id, id) == 0) return &cat->meta[i];
  }
  return nullptr;
}

static installed_meta *upsert_meta(ccord_catalog *cat, const char *id)
{
  installed_meta *m = find_meta(cat, id);
  if (m) return m;
  if (cat->nmeta == cat->capmeta) {
    size_t ncap = cat->capmeta ? cat->capmeta * 2 : 8;
    installed_meta *g = ccord_realloc_array(cat->meta, ncap, sizeof *g);
    if (!g) return nullptr;
    cat->meta = g;
    cat->capmeta = ncap;
  }
  m = &cat->meta[cat->nmeta++];
  memset(m, 0, sizeof *m);
  copy_str(m->id, sizeof m->id, id);
  m->auto_update = true;
  return m;
}

// ---------------------------------------------------------------- persistence (0600 atomic files)

static void save_prefs(const ccord_catalog *cat)
{
  if (!cat || !cat->has_data_dir) return;
  char dir[CCORD_PATH_MAX + 32], path[CCORD_PATH_MAX + 64];
  snprintf(dir, sizeof dir, "%s/plugins", cat->data_dir);
  mkdir_ok(cat->data_dir);
  mkdir_ok(dir);
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

static void load_prefs(ccord_catalog *cat)
{
  cat->prefs.auto_update_enabled = false;   // opt-in by default
  cat->prefs.auto_update_min_tier = 2;      // Official & Verified by default
  if (!cat->has_data_dir) return;
  char path[CCORD_PATH_MAX + 64];
  snprintf(path, sizeof path, "%s/plugins/catalog-prefs.json", cat->data_dir);
  size_t len = 0;
  char *raw = read_entire_file(path, 65536, &len);
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

static void save_installed_meta(const ccord_catalog *cat)
{
  if (!cat || !cat->has_data_dir) return;
  char dir[CCORD_PATH_MAX + 32], path[CCORD_PATH_MAX + 64];
  snprintf(dir, sizeof dir, "%s/plugins", cat->data_dir);
  mkdir_ok(cat->data_dir);
  mkdir_ok(dir);
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

static void load_installed_meta(ccord_catalog *cat)
{
  if (!cat->has_data_dir) return;
  char path[CCORD_PATH_MAX + 64];
  snprintf(path, sizeof path, "%s/plugins/installed-meta.json", cat->data_dir);
  size_t len = 0;
  char *raw = read_entire_file(path, 512u * 1024u, &len);
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
      installed_meta *m = upsert_meta(cat, id);
      if (!m) break;
      size_t sl = 0;
      const char *s = ccord_json_string(ccord_json_get(val, "version"), &sl);
      if (s) copy_str(m->version, sizeof m->version, s);
      s = ccord_json_string(ccord_json_get(val, "sha256"), &sl);
      if (s) copy_str(m->sha256, sizeof m->sha256, s);
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

static void save_catalog_cache(const ccord_catalog *cat, const char *json_body, size_t body_len)
{
  if (!cat || !cat->has_data_dir || !json_body || body_len == 0) return;
  char dir[CCORD_PATH_MAX + 32], path[CCORD_PATH_MAX + 64], meta_path[CCORD_PATH_MAX + 64];
  snprintf(dir, sizeof dir, "%s/plugins", cat->data_dir);
  mkdir_ok(cat->data_dir);
  mkdir_ok(dir);
  snprintf(path, sizeof path, "%s/catalog-cache.json", dir);
  ccord_error err = {0};
  (void)ccord_file_write_atomic(path, json_body, body_len, 0600, false, &err);

  snprintf(meta_path, sizeof meta_path, "%s/catalog-cache-meta.json", dir);
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "etag"); ccord_jw_cstr(&w, cat->etag);
  ccord_jw_key(&w, "last_sync_unix"); ccord_jw_int(&w, cat->status.last_sync_unix);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) {
    (void)ccord_file_write_atomic(meta_path, b.data, b.len, 0600, false, &err);
  }
  ccord_buf_free(&b);
}

// ---------------------------------------------------------------- catalog.v1.json parser

static void json_str_copy(const ccord_json *obj, const char *key, char *dst, size_t cap)
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

static bool parse_catalog_json(ccord_catalog *cat, const char *json, size_t len)
{
  if (!cat || !json || len == 0) return false;
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error pe = {0};
  const ccord_json_limits lim = {16, 65536, CATALOG_MAX_JSON_BYTES};
  const ccord_json *root = ccord_json_parse(&arena, json, len, &lim, &pe);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return false;
  }
  int64_t schema = 0;
  if (!ccord_json_int64(ccord_json_get(root, "schema"), &schema) || schema != 1) {
    ccord_arena_free(&arena);
    return false;
  }

  // Parse revoked_hashes
  const ccord_json *rev_arr = ccord_json_get(root, "revoked_hashes");
  revoked_item *new_rev = nullptr;
  size_t new_nrev = 0;
  if (rev_arr && ccord_json_type_of(rev_arr) == CCORD_JSON_ARRAY) {
    size_t rn = ccord_json_array_len(rev_arr);
    if (rn > 0 && rn < 4096) {
      new_rev = ccord_calloc(rn, sizeof *new_rev);
      if (new_rev) {
        for (size_t i = 0; i < rn; i++) {
          const ccord_json *o = ccord_json_array_at(rev_arr, i);
          if (!o || ccord_json_type_of(o) != CCORD_JSON_OBJECT) continue;
          json_str_copy(o, "sha256", new_rev[new_nrev].sha256, sizeof new_rev[new_nrev].sha256);
          json_str_copy(o, "plugin_id", new_rev[new_nrev].plugin_id, sizeof new_rev[new_nrev].plugin_id);
          json_str_copy(o, "reason", new_rev[new_nrev].reason, sizeof new_rev[new_nrev].reason);
          if (strlen(new_rev[new_nrev].sha256) == 64) new_nrev++;
        }
      }
    }
  }

  // Parse plugins[]
  const ccord_json *arr = ccord_json_get(root, "plugins");
  cat_item *new_items = nullptr;
  size_t new_n = 0;
  if (arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY) {
    size_t pn = ccord_json_array_len(arr);
    if (pn > 0 && pn < 4096) {
      new_items = ccord_calloc(pn, sizeof *new_items);
      if (new_items) {
        for (size_t i = 0; i < pn; i++) {
          const ccord_json *o = ccord_json_array_at(arr, i);
          if (!o || ccord_json_type_of(o) != CCORD_JSON_OBJECT) continue;
          const ccord_json *latest = ccord_json_get(o, "latest");
          const ccord_json *author = ccord_json_get(o, "author");
          if (!latest || ccord_json_type_of(latest) != CCORD_JSON_OBJECT) continue;

          cat_item *it = &new_items[new_n];
          json_str_copy(o, "id", it->id, sizeof it->id);
          json_str_copy(o, "name", it->name, sizeof it->name);
          json_str_copy(o, "description", it->description, sizeof it->description);
          json_str_copy(o, "license", it->license, sizeof it->license);
          if (author && ccord_json_type_of(author) == CCORD_JSON_OBJECT) {
            json_str_copy(author, "login", it->author, sizeof it->author);
            json_str_copy(author, "repo", it->repo, sizeof it->repo);
            json_str_copy(author, "repo_url", it->repo_url, sizeof it->repo_url);
          }
          json_str_copy(latest, "version", it->version, sizeof it->version);
          json_str_copy(latest, "download_url", it->download_url, sizeof it->download_url);
          json_str_copy(latest, "sha256", it->sha256, sizeof it->sha256);
          json_str_copy(latest, "changelog", it->changelog, sizeof it->changelog);
          if (!it->id[0] || !it->name[0] || !it->version[0] || !it->download_url[0] || strlen(it->sha256) != 64) {
            continue;
          }

          int64_t tv = 3;
          if (ccord_json_int64(ccord_json_get(o, "tier"), &tv) && tv >= 1 && tv <= 3) {
            it->tier = (ccord_plugin_tier)tv;
          } else {
            it->tier = CCORD_TIER_UNVERIFIED;
          }

          int64_t sz = 0;
          if (ccord_json_int64(ccord_json_get(latest, "size_bytes"), &sz) && sz > 0) {
            it->size_bytes = (uint64_t)sz;
          }

          // Permissions array -> comma string + bitmask + API class
          const ccord_json *perms = ccord_json_get(latest, "permissions");
          it->perms_mask = 0;
          it->permissions[0] = '\0';
          if (perms && ccord_json_type_of(perms) == CCORD_JSON_ARRAY) {
            size_t pc = ccord_json_array_len(perms);
            size_t off = 0;
            for (size_t k = 0; k < pc; k++) {
              size_t sl = 0;
              const char *ps = ccord_json_string(ccord_json_array_at(perms, k), &sl);
              if (!ps || sl == 0) continue;
              ccord_perm perm_bit = 0;
              if (ccord_perm_from_name(ps, &perm_bit)) {
                it->perms_mask |= (uint64_t)perm_bit;
              }
              if (off > 0 && off + 2 < sizeof it->permissions) {
                it->permissions[off++] = ',';
                it->permissions[off++] = ' ';
              }
              if (off + sl < sizeof it->permissions) {
                memcpy(it->permissions + off, ps, sl);
                off += sl;
                it->permissions[off] = '\0';
              }
            }
          }
          int64_t ac = 0;
          if (ccord_json_int64(ccord_json_get(latest, "api_class"), &ac) && ac >= 1 && ac <= 3) {
            it->api_class = (uint8_t)ac;
          } else {
            it->api_class = (uint8_t)(ccord_perm_max_class(it->perms_mask) + 1);
          }

          // Highest-severity moderator warning
          const ccord_json *warns = ccord_json_get(o, "warnings");
          if (warns && ccord_json_type_of(warns) == CCORD_JSON_ARRAY) {
            size_t wc = ccord_json_array_len(warns);
            for (size_t k = 0; k < wc; k++) {
              const ccord_json *wo = ccord_json_array_at(warns, k);
              if (!wo || ccord_json_type_of(wo) != CCORD_JSON_OBJECT) continue;
              char sev_s[32];
              json_str_copy(wo, "severity", sev_s, sizeof sev_s);
              ccord_catalog_warn_sev sev = CCORD_WARN_CAUTION;
              if (strcmp(sev_s, "danger") == 0) sev = CCORD_WARN_DANGER;
              else if (strcmp(sev_s, "info") == 0) sev = CCORD_WARN_INFO;
              if (sev >= it->warning_sev) {
                it->warning_sev = sev;
                json_str_copy(wo, "title", it->warning_title, sizeof it->warning_title);
                json_str_copy(wo, "message", it->warning_message, sizeof it->warning_message);
              }
            }
          }

          // Preserve active installing flag if this plugin is mid-download
          for (size_t old_i = 0; old_i < cat->nitems; old_i++) {
            if (strcmp(cat->items[old_i].id, it->id) == 0) {
              it->installing = cat->items[old_i].installing;
              break;
            }
          }
          new_n++;
        }
      }
    }
  }

  ccord_arena_free(&arena);

  ccord_free(cat->items);
  cat->items = new_items;
  cat->nitems = new_n;
  ccord_free(cat->revoked);
  cat->revoked = new_rev;
  cat->nrevoked = new_nrev;
  cat->status.has_loaded = true;
  return true;
}

static void load_catalog_cache(ccord_catalog *cat)
{
  if (!cat->has_data_dir) return;
  char path[CCORD_PATH_MAX + 64], meta_path[CCORD_PATH_MAX + 64];
  snprintf(path, sizeof path, "%s/plugins/catalog-cache.json", cat->data_dir);
  size_t len = 0;
  char *raw = read_entire_file(path, CATALOG_MAX_JSON_BYTES, &len);
  if (raw) {
    (void)parse_catalog_json(cat, raw, len);
    ccord_free(raw);
  }
  snprintf(meta_path, sizeof meta_path, "%s/plugins/catalog-cache-meta.json", cat->data_dir);
  size_t mlen = 0;
  char *mraw = read_entire_file(meta_path, 4096, &mlen);
  if (mraw) {
    ccord_arena arena;
    ccord_arena_init(&arena, 0);
    ccord_error pe = {0};
    const ccord_json_limits lim = {8, 64, 4096};
    const ccord_json *root = ccord_json_parse(&arena, mraw, mlen, &lim, &pe);
    if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
      json_str_copy(root, "etag", cat->etag, sizeof cat->etag);
      int64_t ts = 0;
      if (ccord_json_int64(ccord_json_get(root, "last_sync_unix"), &ts)) {
        cat->status.last_sync_unix = ts;
      }
    }
    ccord_arena_free(&arena);
    ccord_free(mraw);
  }
}

// ---------------------------------------------------------------- publish to ccord_contrib

static const cat_item *find_item(const ccord_catalog *cat, const char *id)
{
  if (!cat || !id) return nullptr;
  for (size_t i = 0; i < cat->nitems; i++) {
    if (strcmp(cat->items[i].id, id) == 0) return &cat->items[i];
  }
  return nullptr;
}

static bool compute_update_info(const ccord_catalog *cat, const cat_item *it,
                                const char *local_ver, uint64_t consented_perms,
                                bool *out_avail, bool *out_needs_review)
{
  *out_avail = false;
  *out_needs_review = false;
  if (!it || !local_ver || !local_ver[0]) return false;
  if (ccord_catalog_semver_cmp(local_ver, it->version) < 0) {
    *out_avail = true;
    // Escalation guard: new Class 2/3 perms beyond consented_perms OR Danger warning pause auto-update.
    bool new_restricted_perms = false;
    uint64_t added = it->perms_mask & ~consented_perms;
    if (added != 0 && ccord_perm_max_class(added) > CCORD_PERM_CLASS_SAFE) {
      new_restricted_perms = true;
    }
    if (new_restricted_perms || it->warning_sev >= CCORD_WARN_DANGER) {
      *out_needs_review = true;
    }
    (void)cat;
    return true;
  }
  return false;
}

bool ccord_catalog_query_overlay(const ccord_catalog *cat, const char *id,
                                 const char *installed_ver, uint64_t consented_perms,
                                 ccord_catalog_local_overlay *out)
{
  if (!cat || !id || !out) return false;
  memset(out, 0, sizeof *out);
  const installed_meta *m = find_meta_const(cat, id);
  out->auto_update = m ? m->auto_update : true;
  if (m) out->tier = m->tier;

  const cat_item *it = find_item(cat, id);
  if (!it) return m != nullptr;

  out->tier = it->tier;
  out->warning_sev = it->warning_sev;
  copy_str(out->warning_title, sizeof out->warning_title, it->warning_title);
  copy_str(out->warning_message, sizeof out->warning_message, it->warning_message);
  bool avail = false, review = false;
  if (compute_update_info(cat, it, installed_ver, consented_perms, &avail, &review) && avail) {
    copy_str(out->update_version, sizeof out->update_version, it->version);
    out->update_needs_review = review;
  }
  return true;
}

static void publish_catalog(ccord_catalog *cat)
{
  if (!cat || !cat->contrib) return;
  ccord_catalog_entry *entries = cat->nitems ? ccord_calloc(cat->nitems, sizeof *entries) : nullptr;
  char (*local_vers)[33] = cat->nitems ? ccord_calloc(cat->nitems, sizeof *local_vers) : nullptr;
  size_t pending = 0;

  for (size_t i = 0; i < cat->nitems && entries && local_vers; i++) {
    const cat_item *it = &cat->items[i];
    bool installed = false, bundled = false;
    uint64_t consented = 0;
    if (cat->cbs.get_local_plugin) {
      installed = cat->cbs.get_local_plugin(cat->cbs.ud, it->id, local_vers[i], sizeof local_vers[i], &consented, &bundled);
    }
    const installed_meta *m = find_meta_const(cat, it->id);
    bool avail = false, review = false;
    if (installed && !bundled) {
      compute_update_info(cat, it, local_vers[i], consented, &avail, &review);
      if (avail) pending++;
    }
    entries[i] = (ccord_catalog_entry){
      .id = it->id,
      .name = it->name,
      .description = it->description,
      .license = it->license,
      .author = it->author,
      .repo = it->repo,
      .repo_url = it->repo_url,
      .version = it->version,
      .download_url = it->download_url,
      .sha256 = it->sha256,
      .permissions = it->permissions,
      .changelog = it->changelog,
      .size_bytes = it->size_bytes,
      .api_class = it->api_class,
      .tier = it->tier,
      .warning_sev = it->warning_sev,
      .warning_title = it->warning_title,
      .warning_message = it->warning_message,
      .installed = installed,
      .bundled = bundled,
      .installing = it->installing,
      .update_available = avail,
      .update_needs_review = review,
      .auto_update = m ? m->auto_update : true,
      .installed_version = installed ? local_vers[i] : "",
    };
  }
  cat->status.pending_updates = pending;
  ccord_contrib_set_catalog(cat->contrib, entries, entries ? cat->nitems : 0, &cat->status, &cat->prefs);
  ccord_free(entries);
  ccord_free(local_vers);
  if (cat->cbs.sync_installed_info) cat->cbs.sync_installed_info(cat->cbs.ud);
}

static void enforce_revocations(ccord_catalog *cat)
{
  if (!cat || cat->nrevoked == 0 || !cat->cbs.revoke_plugin) return;
  for (size_t i = 0; i < cat->nmeta; i++) {
    const installed_meta *m = &cat->meta[i];
    if (strlen(m->sha256) != 64) continue;
    for (size_t r = 0; r < cat->nrevoked; r++) {
      if (hex_equal_ci(m->sha256, cat->revoked[r].sha256)) {
        char msg[256];
        snprintf(msg, sizeof msg, "Revoked by C-Cord moderators: %s",
                 cat->revoked[r].reason[0] ? cat->revoked[r].reason : "security policy");
        cat->cbs.revoke_plugin(cat->cbs.ud, m->id, msg);
        break;
      }
    }
  }
}

// ---------------------------------------------------------------- worker thread

static bool enqueue_job(ccord_catalog *cat, const worker_job *job)
{
  if (!cat || !cat->thread_started) return false;
  ccord_mutex_lock(cat->mu);
  if (cat->job_count >= CATALOG_QUEUE_CAP) {
    ccord_mutex_unlock(cat->mu);
    return false;
  }
  size_t tail = (cat->job_head + cat->job_count) % CATALOG_QUEUE_CAP;
  cat->jobs[tail] = *job;
  cat->job_count++;
  ccord_cond_signal(cat->cv);
  ccord_mutex_unlock(cat->mu);
  return true;
}

static void push_result(ccord_catalog *cat, const worker_result *res)
{
  ccord_mutex_lock(cat->mu);
  if (cat->res_count < CATALOG_QUEUE_CAP) {
    size_t tail = (cat->res_head + cat->res_count) % CATALOG_QUEUE_CAP;
    cat->results[tail] = *res;
    cat->res_count++;
  } else {
    ccord_free(res->body);
  }
  ccord_mutex_unlock(cat->mu);
}

static void worker_main(void *arg)
{
  ccord_catalog *cat = arg;
  for (;;) {
    ccord_mutex_lock(cat->mu);
    while (!cat->stop && cat->job_count == 0) {
      ccord_cond_wait(cat->cv, cat->mu);
    }
    if (cat->stop) {
      ccord_mutex_unlock(cat->mu);
      break;
    }
    worker_job job = cat->jobs[cat->job_head];
    cat->job_head = (cat->job_head + 1) % CATALOG_QUEUE_CAP;
    cat->job_count--;
    ccord_mutex_unlock(cat->mu);

    worker_result res = {0};
    res.kind = job.kind;
    res.is_auto_update = job.is_auto_update;
    res.tier = job.tier;
    copy_str(res.id, sizeof res.id, job.id);
    copy_str(res.version, sizeof res.version, job.version);
    copy_str(res.dest_path, sizeof res.dest_path, job.dest_path);

    if (job.kind == JOB_REFRESH) {
      if (!cat->http.get) {
        copy_str(res.error, sizeof res.error, "network catalog fetcher unavailable");
      } else {
        res.ok = cat->http.get(cat->http.ud, job.url, job.etag[0] ? job.etag : nullptr,
                               CATALOG_MAX_JSON_BYTES, &res.body, &res.body_len,
                               res.etag, sizeof res.etag, &res.not_modified,
                               res.error, sizeof res.error);
      }
    } else if (job.kind == JOB_INSTALL) {
      if (!cat->http.download_file) {
        copy_str(res.error, sizeof res.error, "network downloader unavailable");
      } else if (cat->http.download_file(cat->http.ud, job.url, job.dest_path, CATALOG_MAX_CCE_BYTES,
                                         res.error, sizeof res.error)) {
        char actual_sha[65];
        if (!ccord_catalog_sha256_file_hex(job.dest_path, actual_sha)) {
          ccord_file_unlink(job.dest_path);
          copy_str(res.error, sizeof res.error, "failed computing SHA-256 of downloaded package");
        } else if (!hex_equal_ci(actual_sha, job.expected_sha256)) {
          ccord_file_unlink(job.dest_path);
          snprintf(res.error, sizeof res.error, "SHA-256 integrity mismatch for %s (download aborted)", job.id);
        } else {
          copy_str(res.sha256, sizeof res.sha256, actual_sha);
          res.ok = true;
        }
      }
    }
    push_result(cat, &res);
  }
}

// ---------------------------------------------------------------- public API

static void queue_install_item(ccord_catalog *cat, cat_item *it, bool is_auto)
{
  if (!cat || !it || it->installing || !cat->has_data_dir) return;
  char tmp_dir[CCORD_PATH_MAX + 32];
  snprintf(tmp_dir, sizeof tmp_dir, "%s/plugins/.download_tmp", cat->data_dir);
  mkdir_ok(cat->data_dir);
  char pdir[CCORD_PATH_MAX + 32];
  snprintf(pdir, sizeof pdir, "%s/plugins", cat->data_dir);
  mkdir_ok(pdir);
  mkdir_ok(tmp_dir);

  worker_job job = {.kind = JOB_INSTALL, .tier = it->tier, .is_auto_update = is_auto};
  copy_str(job.id, sizeof job.id, it->id);
  copy_str(job.url, sizeof job.url, it->download_url);
  copy_str(job.expected_sha256, sizeof job.expected_sha256, it->sha256);
  copy_str(job.version, sizeof job.version, it->version);
  int dn = snprintf(job.dest_path, sizeof job.dest_path, "%s/%s-%s.cce", tmp_dir, it->id, it->version);
  if (dn < 0 || (size_t)dn >= sizeof job.dest_path) return;
  if (enqueue_job(cat, &job)) {
    it->installing = true;
    snprintf(cat->status.status_msg, sizeof cat->status.status_msg, "Downloading %s v%s...", it->name, it->version);
    publish_catalog(cat);
  }
}

void ccord_catalog_run_auto_updates(ccord_catalog *cat)
{
  if (!cat) return;
  size_t queued = 0;
  for (size_t i = 0; i < cat->nitems; i++) {
    cat_item *it = &cat->items[i];
    if (it->installing) continue;
    char local_ver[33] = "";
    uint64_t consented = 0;
    bool bundled = false;
    if (!cat->cbs.get_local_plugin ||
        !cat->cbs.get_local_plugin(cat->cbs.ud, it->id, local_ver, sizeof local_ver, &consented, &bundled)) {
      continue;
    }
    if (bundled) continue;
    const installed_meta *m = find_meta_const(cat, it->id);
    if (m && !m->auto_update) continue;
    if ((uint8_t)it->tier > cat->prefs.auto_update_min_tier) continue;

    bool avail = false, review = false;
    if (compute_update_info(cat, it, local_ver, consented, &avail, &review) && avail && !review) {
      queue_install_item(cat, it, true);
      queued++;
    }
  }
  if (queued == 0) {
    copy_str(cat->status.status_msg, sizeof cat->status.status_msg,
             "No eligible auto-updates (updates with new API permissions require review).");
    publish_catalog(cat);
  }
}

ccord_catalog *ccord_catalog_create(const char *data_dir, ccord_contrib *contrib,
                                    const ccord_catalog_http *http,
                                    const ccord_catalog_host_cbs *cbs)
{
  ccord_catalog *cat = ccord_calloc(1, sizeof *cat);
  if (!cat) return nullptr;
  cat->contrib = contrib;
  if (http) cat->http = *http;
  if (cbs) cat->cbs = *cbs;
  if (data_dir && data_dir[0]) {
    copy_str(cat->data_dir, sizeof cat->data_dir, data_dir);
    cat->has_data_dir = true;
  }
  const char *env_url = getenv("CCORD_CATALOG_URL");
  copy_str(cat->status.catalog_url, sizeof cat->status.catalog_url,
           (env_url && env_url[0]) ? env_url : CATALOG_DEFAULT_URL);

  load_prefs(cat);
  load_installed_meta(cat);
  load_catalog_cache(cat);

  cat->mu = ccord_mutex_create();
  cat->cv = ccord_cond_create();
  if (cat->mu && cat->cv) {
    cat->thread = ccord_thread_create(worker_main, cat);
    if (cat->thread) cat->thread_started = true;
  }
  if (!cat->http.get) {
    cat->status.offline = true;
    copy_str(cat->status.status_msg, sizeof cat->status.status_msg,
             cat->status.has_loaded ? "Offline mode (showing cached catalog)" : "Offline mode");
  }
  enforce_revocations(cat);
  publish_catalog(cat);
  return cat;
}

void ccord_catalog_destroy(ccord_catalog *cat)
{
  if (!cat) return;
  if (cat->thread_started) {
    ccord_mutex_lock(cat->mu);
    cat->stop = true;
    ccord_cond_broadcast(cat->cv);
    ccord_mutex_unlock(cat->mu);
    ccord_thread_join(cat->thread);
    cat->thread = nullptr;
  }
  while (cat->res_count > 0) {
    ccord_free(cat->results[cat->res_head].body);
    cat->res_head = (cat->res_head + 1) % CATALOG_QUEUE_CAP;
    cat->res_count--;
  }
  ccord_cond_destroy(cat->cv);
  ccord_mutex_destroy(cat->mu);
  ccord_free(cat->items);
  ccord_free(cat->revoked);
  ccord_free(cat->meta);
  ccord_free(cat);
}

void ccord_catalog_refresh(ccord_catalog *cat)
{
  if (!cat || cat->status.syncing) return;
  if (!cat->http.get) {
    cat->status.offline = true;
    copy_str(cat->status.status_msg, sizeof cat->status.status_msg, "Offline build: catalog refresh unavailable");
    publish_catalog(cat);
    return;
  }
  worker_job job = {.kind = JOB_REFRESH};
  copy_str(job.url, sizeof job.url, cat->status.catalog_url);
  copy_str(job.etag, sizeof job.etag, cat->etag);
  if (enqueue_job(cat, &job)) {
    cat->status.syncing = true;
    copy_str(cat->status.status_msg, sizeof cat->status.status_msg, "Syncing plugin catalog...");
    publish_catalog(cat);
  }
}

void ccord_catalog_install(ccord_catalog *cat, const char *id)
{
  if (!cat || !id) return;
  for (size_t i = 0; i < cat->nitems; i++) {
    if (strcmp(cat->items[i].id, id) == 0) {
      queue_install_item(cat, &cat->items[i], false);
      return;
    }
  }
}

void ccord_catalog_set_prefs(ccord_catalog *cat, const ccord_catalog_prefs *prefs)
{
  if (!cat || !prefs) return;
  cat->prefs.auto_update_enabled = prefs->auto_update_enabled;
  uint8_t mt = prefs->auto_update_min_tier;
  if (mt < 1 || mt > 3) mt = 2;
  cat->prefs.auto_update_min_tier = mt;
  save_prefs(cat);
  publish_catalog(cat);
  if (cat->prefs.auto_update_enabled && cat->status.has_loaded) {
    ccord_catalog_run_auto_updates(cat);
  }
}

void ccord_catalog_set_plugin_auto_update(ccord_catalog *cat, const char *id, bool enabled)
{
  if (!cat || !id) return;
  installed_meta *m = upsert_meta(cat, id);
  if (!m) return;
  m->auto_update = enabled;
  save_installed_meta(cat);
  publish_catalog(cat);
}

void ccord_catalog_on_uninstalled(ccord_catalog *cat, const char *id)
{
  if (!cat || !id) return;
  for (size_t i = 0; i < cat->nmeta; i++) {
    if (strcmp(cat->meta[i].id, id) == 0) {
      memmove(&cat->meta[i], &cat->meta[i + 1], (cat->nmeta - i - 1) * sizeof cat->meta[0]);
      cat->nmeta--;
      save_installed_meta(cat);
      break;
    }
  }
  publish_catalog(cat);
}

static void handle_result(ccord_catalog *cat, worker_result *res)
{
  if (res->kind == JOB_REFRESH) {
    cat->status.syncing = false;
    if (!res->ok) {
      cat->status.offline = true;
      copy_str(cat->status.status_msg, sizeof cat->status.status_msg,
               res->error[0] ? res->error : "Catalog refresh failed");
    } else if (res->not_modified) {
      cat->status.offline = false;
      cat->status.last_sync_unix = (int64_t)time(nullptr);
      copy_str(cat->status.status_msg, sizeof cat->status.status_msg, "Catalog is up to date");
    } else if (res->body && parse_catalog_json(cat, res->body, res->body_len)) {
      cat->status.offline = false;
      cat->status.last_sync_unix = (int64_t)time(nullptr);
      if (res->etag[0]) copy_str(cat->etag, sizeof cat->etag, res->etag);
      save_catalog_cache(cat, res->body, res->body_len);
      snprintf(cat->status.status_msg, sizeof cat->status.status_msg,
               "Synced %zu catalog plugin%s", cat->nitems, cat->nitems == 1 ? "" : "s");
      enforce_revocations(cat);
      if (cat->prefs.auto_update_enabled) {
        ccord_catalog_run_auto_updates(cat);
      }
    } else {
      copy_str(cat->status.status_msg, sizeof cat->status.status_msg, "Invalid catalog.v1.json payload");
    }
    ccord_free(res->body);
    res->body = nullptr;
    publish_catalog(cat);
    return;
  }

  if (res->kind == JOB_INSTALL) {
    for (size_t i = 0; i < cat->nitems; i++) {
      if (strcmp(cat->items[i].id, res->id) == 0) {
        cat->items[i].installing = false;
        break;
      }
    }
    if (!res->ok) {
      copy_str(cat->status.status_msg, sizeof cat->status.status_msg,
               res->error[0] ? res->error : "Install failed");
      if (cat->cbs.toast) cat->cbs.toast(cat->cbs.ud, cat->status.status_msg, 2);
      publish_catalog(cat);
      return;
    }
    char err_buf[256] = "";
    bool imported = cat->cbs.import_verified_cce &&
                    cat->cbs.import_verified_cce(cat->cbs.ud, res->dest_path, err_buf, sizeof err_buf);
    ccord_file_unlink(res->dest_path);
    if (!imported) {
      snprintf(cat->status.status_msg, sizeof cat->status.status_msg,
               "Install failed for %s: %.160s", res->id, err_buf[0] ? err_buf : "import error");
      if (cat->cbs.toast) cat->cbs.toast(cat->cbs.ud, cat->status.status_msg, 2);
    } else {
      installed_meta *m = upsert_meta(cat, res->id);
      if (m) {
        copy_str(m->version, sizeof m->version, res->version);
        copy_str(m->sha256, sizeof m->sha256, res->sha256);
        m->tier = res->tier;
        save_installed_meta(cat);
      }
      snprintf(cat->status.status_msg, sizeof cat->status.status_msg,
               "%s %s v%s (SHA-256 verified)",
               res->is_auto_update ? "Auto-updated" : "Installed", res->id, res->version);
      if (cat->cbs.toast) cat->cbs.toast(cat->cbs.ud, cat->status.status_msg, 0);
    }
    publish_catalog(cat);
  }
}

void ccord_catalog_pump(ccord_catalog *cat, uint64_t now_ms)
{
  if (!cat) return;
  if (cat->created_ms == 0) cat->created_ms = now_ms;

  if (cat->http.get && !cat->initial_refresh_queued && now_ms - cat->created_ms >= CATALOG_STARTUP_DELAY_MS) {
    cat->initial_refresh_queued = true;
    cat->last_refresh_ms = now_ms;
    ccord_catalog_refresh(cat);
  } else if (cat->http.get && cat->initial_refresh_queued && now_ms - cat->last_refresh_ms >= CATALOG_REFRESH_INTERVAL_MS) {
    cat->last_refresh_ms = now_ms;
    ccord_catalog_refresh(cat);
  }

  for (;;) {
    worker_result res = {0};
    bool has = false;
    ccord_mutex_lock(cat->mu);
    if (cat->res_count > 0) {
      res = cat->results[cat->res_head];
      cat->res_head = (cat->res_head + 1) % CATALOG_QUEUE_CAP;
      cat->res_count--;
      has = true;
    }
    ccord_mutex_unlock(cat->mu);
    if (!has) break;
    handle_result(cat, &res);
  }
}
