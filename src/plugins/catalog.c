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
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CATALOG_DEFAULT_BASE_URL "https://plugins.c-cord.org"
#define CATALOG_MAX_API_BYTES (2u * 1024u * 1024u)
#define CATALOG_MAX_CCE_BYTES (25u * 1024u * 1024u)
#define CATALOG_REFRESH_INTERVAL_MS (6ULL * 60ULL * 60ULL * 1000ULL)
#define CATALOG_STARTUP_DELAY_MS 1500ULL
#define CATALOG_QUEUE_CAP 32
#define CATALOG_PAGE_LIMIT 24
#define CATALOG_MAX_BATCH_IDS 64
#define CATALOG_MAX_INFLIGHT_INSTALLS 8

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
  uint64_t downloads;
  uint64_t perms_mask;
  uint8_t api_class;
  ccord_plugin_tier tier;
  ccord_catalog_warn_sev warning_sev;
  char warning_title[128];
  char warning_message[512];
  bool has_detail;
} cat_item;

typedef struct revoked_item {
  int64_t seq;
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

typedef enum job_kind : uint8_t {
  JOB_DISCOVER_QUERY = 1,
  JOB_FETCH_DETAIL = 2,
  JOB_CHECK_UPDATES = 3,
  JOB_FETCH_REVOCATIONS = 4,
  JOB_INSTALL = 5,
} job_kind;

typedef struct worker_job {
  job_kind kind;
  uint64_t seq;
  bool append_page;
  bool install_after_detail;
  bool is_auto_update;
  char id[65];
  char url[1024];
  char expected_sha256[65];
  char version[33];
  char etag[128];
  char *dest_path; // owned by job (JOB_INSTALL only), transferred to worker_result
  char *post_body; // owned by job until worker frees it
  size_t post_len;
  ccord_plugin_tier tier;
} worker_job;

typedef struct worker_result {
  job_kind kind;
  uint64_t seq;
  bool ok;
  bool not_modified;
  bool append_page;
  bool install_after_detail;
  bool is_auto_update;
  char id[65];
  char version[33];
  char sha256[65];
  char etag[128];
  char *dest_path; // owned until pump frees after handle_result
  ccord_plugin_tier tier;
  char *body;
  size_t body_len;
  char error[256];
} worker_result;

struct ccord_catalog {
  char *data_dir;
  bool has_data_dir;
  char *base_url;
  ccord_contrib *contrib;
  ccord_catalog_http http;
  ccord_catalog_host_cbs cbs;
  ccord_catalog_prefs prefs;
  ccord_catalog_status status;

  // Bounded O(1) discover page state (only currently browsed page(s))
  cat_item *discover;
  size_t ndiscover, capdiscover;
  char next_cursor[256];
  char cur_query[96];
  int cur_tier;
  int cur_api_class;
  char cur_sort[16];
  uint64_t discover_req_seq;

  // On-demand single plugin detail (lazily heap-allocated on first detail fetch)
  cat_item *detail;
  bool has_detail;
  char detail_target_id[65];
  char detail_etag[128];
  uint64_t detail_req_seq;

  // Targeted update metadata for ONLY locally installed plugins
  cat_item *updates;
  size_t nupdates;

  // Incremental malware revocation kill-switch list
  revoked_item *revoked;
  size_t nrevoked;
  int64_t rev_seq;
  char rev_etag[128];

  // Locally installed plugin tracking metadata
  installed_meta *meta;
  size_t nmeta, capmeta;

  // Active download/install IDs (lazily allocated on first install)
  char (*installing_ids)[65];
  size_t ninstalling;

  bool initial_check_queued;
  uint64_t created_ms;
  uint64_t last_check_ms;

  // Worker thread & queues (lazily allocated & spawned on first job enqueue)
  ccord_thread *thread;
  bool thread_started;
  ccord_mutex *mu;
  ccord_cond *cv;
  bool stop;
  _Atomic uint32_t ready_count;

  worker_job *jobs;
  size_t job_head, job_count;
  worker_result *results;
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

static void url_encode_component(const char *src, char *dst, size_t cap)
{
  if (!dst || cap == 0) return;
  dst[0] = '\0';
  if (!src) return;
  static const char k_hex[] = "0123456789ABCDEF";
  size_t out = 0;
  for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
    unsigned char c = *p;
    bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      if (out + 1 >= cap) break;
      dst[out++] = (char)c;
    } else {
      if (out + 3 >= cap) break;
      dst[out++] = '%';
      dst[out++] = k_hex[c >> 4];
      dst[out++] = k_hex[c & 0x0f];
    }
  }
  dst[out] = '\0';
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

static bool is_installing(const ccord_catalog *cat, const char *id)
{
  if (!cat || !cat->installing_ids || !id || !id[0]) return false;
  for (size_t i = 0; i < cat->ninstalling; i++) {
    if (strcmp(cat->installing_ids[i], id) == 0) return true;
  }
  return false;
}

static void set_installing(ccord_catalog *cat, const char *id, bool on)
{
  if (!cat || !id || !id[0]) return;
  if (cat->installing_ids) {
    for (size_t i = 0; i < cat->ninstalling; i++) {
      if (strcmp(cat->installing_ids[i], id) == 0) {
        if (!on) {
          memmove(&cat->installing_ids[i], &cat->installing_ids[i + 1],
                  (cat->ninstalling - i - 1) * sizeof cat->installing_ids[0]);
          cat->ninstalling--;
        }
        return;
      }
    }
  }
  if (on && cat->ninstalling < CATALOG_MAX_INFLIGHT_INSTALLS) {
    if (!cat->installing_ids) {
      cat->installing_ids = ccord_calloc(CATALOG_MAX_INFLIGHT_INSTALLS, sizeof *cat->installing_ids);
      if (!cat->installing_ids) return;
    }
    copy_str(cat->installing_ids[cat->ninstalling++], sizeof cat->installing_ids[0], id);
  }
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

// ---------------------------------------------------------------- persistence (tiny 0600 atomic files)

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
  cat->prefs.auto_update_enabled = false;
  cat->prefs.auto_update_min_tier = 2;
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

static void save_revocations_cache(const ccord_catalog *cat)
{
  if (!cat || !cat->has_data_dir) return;
  char dir[CCORD_PATH_MAX + 32], path[CCORD_PATH_MAX + 64];
  snprintf(dir, sizeof dir, "%s/plugins", cat->data_dir);
  mkdir_ok(cat->data_dir);
  mkdir_ok(dir);
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

static bool merge_revocations_json(ccord_catalog *cat, const char *json, size_t len, bool is_cache_file)
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
    json_str_copy(root, "etag", cat->rev_etag, sizeof cat->rev_etag);
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
          json_str_copy(o, "sha256", item.sha256, sizeof item.sha256);
          json_str_copy(o, "plugin_id", item.plugin_id, sizeof item.plugin_id);
          json_str_copy(o, "reason", item.reason, sizeof item.reason);
          if (strlen(item.sha256) != 64) continue;
          bool dup = false;
          for (size_t k = 0; k < cat->nrevoked; k++) {
            if (hex_equal_ci(cat->revoked[k].sha256, item.sha256)) { dup = true; break; }
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

static void load_revocations_cache(ccord_catalog *cat)
{
  if (!cat->has_data_dir) return;
  char path[CCORD_PATH_MAX + 64];
  snprintf(path, sizeof path, "%s/plugins/revocations-cache.json", cat->data_dir);
  size_t len = 0;
  char *raw = read_entire_file(path, CATALOG_MAX_API_BYTES, &len);
  if (raw) {
    (void)merge_revocations_json(cat, raw, len, true);
    ccord_free(raw);
  }
}

// ---------------------------------------------------------------- API v2 JSON parsers

static void parse_permissions_array(const ccord_json *perms, cat_item *it)
{
  it->perms_mask = 0;
  it->permissions[0] = '\0';
  if (!perms || ccord_json_type_of(perms) != CCORD_JSON_ARRAY) return;
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

static void parse_warnings_array(const ccord_json *warns, cat_item *it)
{
  it->warning_sev = CCORD_WARN_NONE;
  it->warning_title[0] = '\0';
  it->warning_message[0] = '\0';
  if (!warns || ccord_json_type_of(warns) != CCORD_JSON_ARRAY) return;
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

// Parse GET /api/v2/plugins slim card response
static bool parse_discover_page_json(ccord_catalog *cat, const char *json, size_t len, bool append)
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

  int64_t total = 0;
  (void)ccord_json_int64(ccord_json_get(root, "total"), &total);
  const ccord_json *hm = ccord_json_get(root, "has_more");
  bool has_more = hm ? ccord_json_bool(hm, false) : false;
  char next_cur[256];
  json_str_copy(root, "next_cursor", next_cur, sizeof next_cur);

  const ccord_json *arr = ccord_json_get(root, "plugins");
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) {
    ccord_arena_free(&arena);
    return false;
  }
  size_t pn = ccord_json_array_len(arr);
  if (!append) {
    cat->ndiscover = 0;
  }
  if (pn > 0 && cat->ndiscover + pn > cat->capdiscover) {
    size_t ncap = cat->ndiscover + pn;
    cat_item *g = ccord_realloc_array(cat->discover, ncap, sizeof *g);
    if (!g) {
      ccord_arena_free(&arena);
      return false;
    }
    cat->discover = g;
    cat->capdiscover = ncap;
  }

  for (size_t i = 0; i < pn; i++) {
    const ccord_json *o = ccord_json_array_at(arr, i);
    if (!o || ccord_json_type_of(o) != CCORD_JSON_OBJECT) continue;
    cat_item it = {0};
    json_str_copy(o, "id", it.id, sizeof it.id);
    json_str_copy(o, "name", it.name, sizeof it.name);
    json_str_copy(o, "description", it.description, sizeof it.description);
    json_str_copy(o, "license", it.license, sizeof it.license);
    json_str_copy(o, "author_login", it.author, sizeof it.author);
    json_str_copy(o, "author_repo", it.repo, sizeof it.repo);
    json_str_copy(o, "author_repo_url", it.repo_url, sizeof it.repo_url);
    json_str_copy(o, "version", it.version, sizeof it.version);
    json_str_copy(o, "warning_title", it.warning_title, sizeof it.warning_title);
    if (!it.id[0] || !it.name[0] || !it.version[0]) continue;

    int64_t tv = 3;
    if (ccord_json_int64(ccord_json_get(o, "tier"), &tv) && tv >= 1 && tv <= 3) it.tier = (ccord_plugin_tier)tv;
    else it.tier = CCORD_TIER_UNVERIFIED;

    int64_t ac = 1;
    if (ccord_json_int64(ccord_json_get(o, "api_class"), &ac) && ac >= 1 && ac <= 3) it.api_class = (uint8_t)ac;
    else it.api_class = 1;

    int64_t sz = 0;
    if (ccord_json_int64(ccord_json_get(o, "size_bytes"), &sz) && sz > 0) it.size_bytes = (uint64_t)sz;
    int64_t dls = 0;
    if (ccord_json_int64(ccord_json_get(o, "downloads"), &dls) && dls > 0) it.downloads = (uint64_t)dls;
    int64_t ws = 0;
    if (ccord_json_int64(ccord_json_get(o, "warning_sev"), &ws) && ws >= 0 && ws <= 3) {
      it.warning_sev = (ccord_catalog_warn_sev)ws;
    }
    it.has_detail = false;

    // Avoid duplicates when appending cursor pages
    bool dup = false;
    for (size_t k = 0; k < cat->ndiscover; k++) {
      if (strcmp(cat->discover[k].id, it.id) == 0) {
        cat->discover[k] = it;
        dup = true;
        break;
      }
    }
    if (!dup) cat->discover[cat->ndiscover++] = it;
  }

  cat->status.total_count = total > 0 ? (size_t)total : cat->ndiscover;
  cat->status.has_more = has_more && next_cur[0] != '\0';
  copy_str(cat->next_cursor, sizeof cat->next_cursor, next_cur);
  cat->status.has_loaded = true;
  ccord_arena_free(&arena);
  return true;
}

// Parse GET /api/v2/plugins/{id} full detail response
static bool parse_detail_json(const char *json, size_t len, cat_item *out)
{
  if (!json || len == 0 || !out) return false;
  memset(out, 0, sizeof *out);
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error pe = {0};
  const ccord_json_limits lim = {12, 8192, CATALOG_MAX_API_BYTES};
  const ccord_json *o = ccord_json_parse(&arena, json, len, &lim, &pe);
  if (!o || ccord_json_type_of(o) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return false;
  }
  const ccord_json *latest = ccord_json_get(o, "latest");
  const ccord_json *author = ccord_json_get(o, "author");
  if (!latest || ccord_json_type_of(latest) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return false;
  }

  json_str_copy(o, "id", out->id, sizeof out->id);
  json_str_copy(o, "name", out->name, sizeof out->name);
  json_str_copy(o, "description", out->description, sizeof out->description);
  json_str_copy(o, "license", out->license, sizeof out->license);
  if (author && ccord_json_type_of(author) == CCORD_JSON_OBJECT) {
    json_str_copy(author, "login", out->author, sizeof out->author);
    json_str_copy(author, "repo", out->repo, sizeof out->repo);
    json_str_copy(author, "repo_url", out->repo_url, sizeof out->repo_url);
  }
  json_str_copy(latest, "version", out->version, sizeof out->version);
  json_str_copy(latest, "download_url", out->download_url, sizeof out->download_url);
  json_str_copy(latest, "sha256", out->sha256, sizeof out->sha256);
  json_str_copy(latest, "changelog", out->changelog, sizeof out->changelog);
  if (!out->id[0] || !out->name[0] || !out->version[0] || !out->download_url[0] || strlen(out->sha256) != 64) {
    ccord_arena_free(&arena);
    return false;
  }
  int64_t tv = 3;
  if (ccord_json_int64(ccord_json_get(o, "tier"), &tv) && tv >= 1 && tv <= 3) out->tier = (ccord_plugin_tier)tv;
  else out->tier = CCORD_TIER_UNVERIFIED;
  int64_t sz = 0;
  if (ccord_json_int64(ccord_json_get(latest, "size_bytes"), &sz) && sz > 0) out->size_bytes = (uint64_t)sz;
  int64_t dls = 0;
  if (ccord_json_int64(ccord_json_get(o, "downloads"), &dls) && dls > 0) out->downloads = (uint64_t)dls;

  parse_permissions_array(ccord_json_get(latest, "permissions"), out);
  int64_t ac = 0;
  if (ccord_json_int64(ccord_json_get(latest, "api_class"), &ac) && ac >= 1 && ac <= 3) out->api_class = (uint8_t)ac;
  else out->api_class = (uint8_t)(ccord_perm_max_class(out->perms_mask) + 1);
  parse_warnings_array(ccord_json_get(o, "warnings"), out);
  out->has_detail = true;

  ccord_arena_free(&arena);
  return true;
}

// Parse POST /api/v2/updates/check response
static bool parse_updates_check_json(ccord_catalog *cat, const char *json, size_t len,
                                     bool *out_rev_stale, int64_t *out_rev_seq)
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
  int64_t schema = 0;
  if (!ccord_json_int64(ccord_json_get(root, "schema"), &schema) || schema != 2) {
    ccord_arena_free(&arena);
    return false;
  }
  int64_t rseq = 0;
  (void)ccord_json_int64(ccord_json_get(root, "rev_seq"), &rseq);
  const ccord_json *rs = ccord_json_get(root, "revocations_stale");
  if (out_rev_stale) *out_rev_stale = (rs && ccord_json_bool(rs, false)) || rseq > cat->rev_seq;
  if (out_rev_seq) *out_rev_seq = rseq;

  const ccord_json *arr = ccord_json_get(root, "plugins");
  cat_item *new_upd = nullptr;
  size_t new_n = 0;
  if (arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY) {
    size_t pn = ccord_json_array_len(arr);
    if (pn > 0 && pn <= CATALOG_MAX_BATCH_IDS) {
      new_upd = ccord_calloc(pn, sizeof *new_upd);
      if (new_upd) {
        for (size_t i = 0; i < pn; i++) {
          const ccord_json *o = ccord_json_array_at(arr, i);
          if (!o || ccord_json_type_of(o) != CCORD_JSON_OBJECT) continue;
          cat_item *it = &new_upd[new_n];
          json_str_copy(o, "id", it->id, sizeof it->id);
          json_str_copy(o, "name", it->name, sizeof it->name);
          json_str_copy(o, "description", it->description, sizeof it->description);
          json_str_copy(o, "license", it->license, sizeof it->license);
          json_str_copy(o, "author_login", it->author, sizeof it->author);
          json_str_copy(o, "author_repo", it->repo, sizeof it->repo);
          json_str_copy(o, "author_repo_url", it->repo_url, sizeof it->repo_url);
          json_str_copy(o, "version", it->version, sizeof it->version);
          json_str_copy(o, "download_url", it->download_url, sizeof it->download_url);
          json_str_copy(o, "sha256", it->sha256, sizeof it->sha256);
          json_str_copy(o, "changelog", it->changelog, sizeof it->changelog);
          if (!it->id[0] || !it->name[0] || !it->version[0] || !it->download_url[0] || strlen(it->sha256) != 64) {
            continue;
          }
          int64_t tv = 3;
          if (ccord_json_int64(ccord_json_get(o, "tier"), &tv) && tv >= 1 && tv <= 3) it->tier = (ccord_plugin_tier)tv;
          else it->tier = CCORD_TIER_UNVERIFIED;
          int64_t sz = 0;
          if (ccord_json_int64(ccord_json_get(o, "size_bytes"), &sz) && sz > 0) it->size_bytes = (uint64_t)sz;
          parse_permissions_array(ccord_json_get(o, "permissions"), it);
          int64_t ac = 0;
          if (ccord_json_int64(ccord_json_get(o, "api_class"), &ac) && ac >= 1 && ac <= 3) it->api_class = (uint8_t)ac;
          else it->api_class = (uint8_t)(ccord_perm_max_class(it->perms_mask) + 1);
          parse_warnings_array(ccord_json_get(o, "warnings"), it);
          it->has_detail = true;
          new_n++;
        }
      }
    }
  }
  ccord_arena_free(&arena);

  ccord_free(cat->updates);
  cat->updates = new_upd;
  cat->nupdates = new_n;
  return true;
}

// ---------------------------------------------------------------- lookup & publish to ccord_contrib

static const cat_item *find_update_item(const ccord_catalog *cat, const char *id)
{
  if (!cat || !id) return nullptr;
  for (size_t i = 0; i < cat->nupdates; i++) {
    if (strcmp(cat->updates[i].id, id) == 0) return &cat->updates[i];
  }
  return nullptr;
}

static const cat_item *find_best_item(const ccord_catalog *cat, const char *id)
{
  if (!cat || !id) return nullptr;
  if (cat->has_detail && cat->detail && strcmp(cat->detail->id, id) == 0) return cat->detail;
  const cat_item *u = find_update_item(cat, id);
  if (u) return u;
  for (size_t i = 0; i < cat->ndiscover; i++) {
    if (strcmp(cat->discover[i].id, id) == 0) return &cat->discover[i];
  }
  return nullptr;
}

static bool compute_update_info(const cat_item *it, const char *local_ver, uint64_t consented_perms,
                                bool *out_avail, bool *out_needs_review)
{
  *out_avail = false;
  *out_needs_review = false;
  if (!it || !local_ver || !local_ver[0]) return false;
  if (ccord_catalog_semver_cmp(local_ver, it->version) < 0) {
    *out_avail = true;
    bool new_restricted_perms = false;
    uint64_t added = it->perms_mask & ~consented_perms;
    if (added != 0 && ccord_perm_max_class(added) > CCORD_PERM_CLASS_SAFE) {
      new_restricted_perms = true;
    }
    if (new_restricted_perms || it->warning_sev >= CCORD_WARN_DANGER) {
      *out_needs_review = true;
    }
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

  const cat_item *it = find_best_item(cat, id);
  if (!it) return m != nullptr;

  out->tier = it->tier;
  out->warning_sev = it->warning_sev;
  copy_str(out->warning_title, sizeof out->warning_title, it->warning_title);
  copy_str(out->warning_message, sizeof out->warning_message, it->warning_message);
  bool avail = false, review = false;
  if (compute_update_info(it, installed_ver, consented_perms, &avail, &review) && avail) {
    copy_str(out->update_version, sizeof out->update_version, it->version);
    out->update_needs_review = review;
  }
  return true;
}

static void build_contrib_entry(const ccord_catalog *cat, const cat_item *it,
                                char local_ver_buf[33], ccord_catalog_entry *out,
                                bool *out_pending_upd)
{
  bool installed = false, bundled = false;
  uint64_t consented = 0;
  local_ver_buf[0] = '\0';
  if (cat->cbs.get_local_plugin) {
    installed = cat->cbs.get_local_plugin(cat->cbs.ud, it->id, local_ver_buf, 33, &consented, &bundled);
  }
  const installed_meta *m = find_meta_const(cat, it->id);
  bool avail = false, review = false;
  if (installed && !bundled) {
    const cat_item *upd_src = it->has_detail ? it : find_update_item(cat, it->id);
    compute_update_info(upd_src ? upd_src : it, local_ver_buf, consented, &avail, &review);
  }
  if (out_pending_upd) *out_pending_upd = avail;

  *out = (ccord_catalog_entry){
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
    .downloads = it->downloads,
    .api_class = it->api_class,
    .tier = it->tier,
    .warning_sev = it->warning_sev,
    .warning_title = it->warning_title,
    .warning_message = it->warning_message,
    .has_detail = it->has_detail,
    .installed = installed,
    .bundled = bundled,
    .installing = is_installing(cat, it->id),
    .update_available = avail,
    .update_needs_review = review,
    .auto_update = m ? m->auto_update : true,
    .installed_version = installed ? local_ver_buf : "",
  };
}

static void publish_catalog(ccord_catalog *cat)
{
  if (!cat || !cat->contrib) return;

  // 1. Publish installed updates list (independent of current Discover search page!)
  size_t pending = 0;
  ccord_catalog_entry *upd_entries = cat->nupdates ? ccord_calloc(cat->nupdates, sizeof *upd_entries) : nullptr;
  char (*upd_vers)[33] = cat->nupdates ? ccord_calloc(cat->nupdates, sizeof *upd_vers) : nullptr;
  size_t upd_out_n = 0;
  for (size_t i = 0; i < cat->nupdates && upd_entries && upd_vers; i++) {
    bool is_upd = false;
    build_contrib_entry(cat, &cat->updates[i], upd_vers[upd_out_n], &upd_entries[upd_out_n], &is_upd);
    if (is_upd) {
      pending++;
      upd_out_n++;
    }
  }
  cat->status.pending_updates = pending;
  ccord_contrib_set_catalog_updates(cat->contrib, upd_entries, upd_out_n);
  ccord_free(upd_entries);
  ccord_free(upd_vers);

  // 2. Publish on-demand detail entry
  if (cat->has_detail && cat->detail) {
    char det_ver[33];
    ccord_catalog_entry det_e = {0};
    build_contrib_entry(cat, cat->detail, det_ver, &det_e, nullptr);
    ccord_contrib_set_catalog_detail(cat->contrib, &det_e);
  } else {
    ccord_contrib_set_catalog_detail(cat->contrib, nullptr);
  }

  // 3. Publish current Discover page + status + prefs
  ccord_catalog_entry *disc_entries = cat->ndiscover ? ccord_calloc(cat->ndiscover, sizeof *disc_entries) : nullptr;
  char (*disc_vers)[33] = cat->ndiscover ? ccord_calloc(cat->ndiscover, sizeof *disc_vers) : nullptr;
  for (size_t i = 0; i < cat->ndiscover && disc_entries && disc_vers; i++) {
    build_contrib_entry(cat, &cat->discover[i], disc_vers[i], &disc_entries[i], nullptr);
  }
  ccord_contrib_set_catalog(cat->contrib, disc_entries, disc_entries ? cat->ndiscover : 0, &cat->status, &cat->prefs);
  ccord_free(disc_entries);
  ccord_free(disc_vers);

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

static void worker_main(void *arg);

static bool ensure_worker_started(ccord_catalog *cat)
{
  if (cat->thread_started) return true;
  if (!cat->jobs) {
    cat->jobs = ccord_calloc(CATALOG_QUEUE_CAP, sizeof *cat->jobs);
    if (!cat->jobs) return false;
  }
  if (!cat->results) {
    cat->results = ccord_calloc(CATALOG_QUEUE_CAP, sizeof *cat->results);
    if (!cat->results) return false;
  }
  if (!cat->mu) {
    cat->mu = ccord_mutex_create();
    if (!cat->mu) return false;
  }
  if (!cat->cv) {
    cat->cv = ccord_cond_create();
    if (!cat->cv) return false;
  }
  cat->thread = ccord_thread_create(worker_main, cat);
  if (!cat->thread) return false;
  cat->thread_started = true;
  return true;
}

static bool enqueue_job(ccord_catalog *cat, worker_job *job)
{
  if (!cat || !ensure_worker_started(cat)) {
    ccord_free(job->post_body);
    job->post_body = nullptr;
    ccord_free(job->dest_path);
    job->dest_path = nullptr;
    return false;
  }
  ccord_mutex_lock(cat->mu);
  if (cat->job_count >= CATALOG_QUEUE_CAP) {
    ccord_mutex_unlock(cat->mu);
    ccord_free(job->post_body);
    job->post_body = nullptr;
    ccord_free(job->dest_path);
    job->dest_path = nullptr;
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
    atomic_store_explicit(&cat->ready_count, (uint32_t)cat->res_count, memory_order_release);
  } else {
    ccord_free(res->body);
    ccord_free(res->dest_path);
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
    res.seq = job.seq;
    res.append_page = job.append_page;
    res.install_after_detail = job.install_after_detail;
    res.is_auto_update = job.is_auto_update;
    res.tier = job.tier;
    copy_str(res.id, sizeof res.id, job.id);
    copy_str(res.version, sizeof res.version, job.version);
    res.dest_path = job.dest_path;
    job.dest_path = nullptr;

    if (job.kind == JOB_DISCOVER_QUERY || job.kind == JOB_FETCH_DETAIL || job.kind == JOB_FETCH_REVOCATIONS) {
      if (!cat->http.get) {
        copy_str(res.error, sizeof res.error, "network catalog fetcher unavailable");
      } else {
        res.ok = cat->http.get(cat->http.ud, job.url, job.etag[0] ? job.etag : nullptr,
                               CATALOG_MAX_API_BYTES, &res.body, &res.body_len,
                               res.etag, sizeof res.etag, &res.not_modified,
                               res.error, sizeof res.error);
      }
    } else if (job.kind == JOB_CHECK_UPDATES) {
      if (!cat->http.post_json) {
        copy_str(res.error, sizeof res.error, "network catalog updater unavailable");
      } else {
        res.ok = cat->http.post_json(cat->http.ud, job.url,
                                     job.post_body ? job.post_body : "{}",
                                     job.post_body ? job.post_len : 2u,
                                     CATALOG_MAX_API_BYTES, &res.body, &res.body_len,
                                     res.error, sizeof res.error);
      }
    } else if (job.kind == JOB_INSTALL) {
      if (!cat->http.download_file || !res.dest_path) {
        copy_str(res.error, sizeof res.error, "network downloader unavailable");
      } else if (cat->http.download_file(cat->http.ud, job.url, res.dest_path, CATALOG_MAX_CCE_BYTES,
                                         res.error, sizeof res.error)) {
        char actual_sha[65];
        if (!ccord_catalog_sha256_file_hex(res.dest_path, actual_sha)) {
          ccord_file_unlink(res.dest_path);
          copy_str(res.error, sizeof res.error, "failed computing SHA-256 of downloaded package");
        } else if (!hex_equal_ci(actual_sha, job.expected_sha256)) {
          ccord_file_unlink(res.dest_path);
          snprintf(res.error, sizeof res.error, "SHA-256 integrity mismatch for %s (download aborted)", job.id);
        } else {
          copy_str(res.sha256, sizeof res.sha256, actual_sha);
          res.ok = true;
        }
      }
    }
    ccord_free(job.post_body);
    push_result(cat, &res);
  }
}

// ---------------------------------------------------------------- public actions

static void enqueue_revocations_fetch(ccord_catalog *cat)
{
  if (!cat || !cat->http.get) return;
  worker_job job = {.kind = JOB_FETCH_REVOCATIONS};
  snprintf(job.url, sizeof job.url, "%s/api/v2/revocations?since=%" PRId64, cat->base_url, cat->rev_seq);
  copy_str(job.etag, sizeof job.etag, cat->rev_etag);
  (void)enqueue_job(cat, &job);
}

static void queue_install_item(ccord_catalog *cat, const cat_item *it, bool is_auto)
{
  if (!cat || !it || !cat->has_data_dir || !it->download_url[0] || strlen(it->sha256) != 64) return;
  char tmp_dir[CCORD_PATH_MAX + 32], pdir[CCORD_PATH_MAX + 32];
  snprintf(pdir, sizeof pdir, "%s/plugins", cat->data_dir);
  snprintf(tmp_dir, sizeof tmp_dir, "%s/plugins/.download_tmp", cat->data_dir);
  mkdir_ok(cat->data_dir);
  mkdir_ok(pdir);
  mkdir_ok(tmp_dir);

  worker_job job = {.kind = JOB_INSTALL, .tier = it->tier, .is_auto_update = is_auto};
  copy_str(job.id, sizeof job.id, it->id);
  copy_str(job.url, sizeof job.url, it->download_url);
  copy_str(job.expected_sha256, sizeof job.expected_sha256, it->sha256);
  copy_str(job.version, sizeof job.version, it->version);
  char dpath[CCORD_PATH_MAX + 160];
  int dn = snprintf(dpath, sizeof dpath, "%s/%s-%s.cce", tmp_dir, it->id, it->version);
  if (dn < 0 || (size_t)dn >= sizeof dpath) return;
  job.dest_path = ccord_malloc((size_t)dn + 1u);
  if (!job.dest_path) return;
  memcpy(job.dest_path, dpath, (size_t)dn + 1u);
  if (enqueue_job(cat, &job)) {
    set_installing(cat, it->id, true);
    snprintf(cat->status.status_msg, sizeof cat->status.status_msg, "Downloading %s v%s...", it->name, it->version);
    publish_catalog(cat);
  }
}

void ccord_catalog_check_updates(ccord_catalog *cat)
{
  if (!cat) return;
  if (!cat->http.post_json && !cat->http.get) {
    cat->status.offline = true;
    return;
  }

  // Gather unique non-bundled installed plugin IDs (0 installed -> 0 network requests!)
  char ids[CATALOG_MAX_BATCH_IDS][65];
  size_t nids = 0;
  if (cat->cbs.list_local_plugins) {
    nids = cat->cbs.list_local_plugins(cat->cbs.ud, ids, CATALOG_MAX_BATCH_IDS);
  }
  for (size_t i = 0; i < cat->nmeta && nids < CATALOG_MAX_BATCH_IDS; i++) {
    bool dup = false;
    for (size_t k = 0; k < nids; k++) {
      if (strcmp(ids[k], cat->meta[i].id) == 0) { dup = true; break; }
    }
    if (!dup) {
      bool bundled = false;
      char ver[33] = "";
      if (!cat->cbs.get_local_plugin ||
          (cat->cbs.get_local_plugin(cat->cbs.ud, cat->meta[i].id, ver, sizeof ver, nullptr, &bundled) && !bundled)) {
        copy_str(ids[nids++], sizeof ids[0], cat->meta[i].id);
      }
    }
  }
  if (nids == 0) {
    // Zero third-party plugins installed: skip background network polling entirely!
    ccord_free(cat->updates);
    cat->updates = nullptr;
    cat->nupdates = 0;
    publish_catalog(cat);
    return;
  }
  if (!cat->http.post_json) return;

  ccord_buf b;
  ccord_buf_init(&b, 256);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "ids");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < nids; i++) ccord_jw_cstr(&w, ids[i]);
  ccord_jw_end_array(&w);
  ccord_jw_key(&w, "rev_seq"); ccord_jw_int(&w, cat->rev_seq);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&b);
    return;
  }
  char *payload = ccord_malloc(b.len + 1);
  if (!payload) {
    ccord_buf_free(&b);
    return;
  }
  memcpy(payload, b.data, b.len);
  payload[b.len] = '\0';
  size_t plen = b.len;
  ccord_buf_free(&b);

  worker_job job = {.kind = JOB_CHECK_UPDATES, .post_body = payload, .post_len = plen};
  snprintf(job.url, sizeof job.url, "%s/api/v2/updates/check", cat->base_url);
  (void)enqueue_job(cat, &job);
}

void ccord_catalog_query(ccord_catalog *cat, const char *q, int tier, int api_class, const char *sort,
                         bool append_next_page)
{
  if (!cat) return;
  if (!cat->http.get) {
    cat->status.offline = true;
    copy_str(cat->status.status_msg, sizeof cat->status.status_msg, "Offline build: catalog browse unavailable");
    publish_catalog(cat);
    return;
  }
  if (append_next_page && (!cat->status.has_more || !cat->next_cursor[0] || cat->status.loading_more)) {
    return;
  }
  if (!append_next_page) {
    copy_str(cat->cur_query, sizeof cat->cur_query, q ? q : "");
    cat->cur_tier = (tier >= 1 && tier <= 3) ? tier : 0;
    cat->cur_api_class = (api_class >= 1 && api_class <= 3) ? api_class : 0;
    copy_str(cat->cur_sort, sizeof cat->cur_sort, (sort && sort[0]) ? sort : "popular");
    cat->next_cursor[0] = '\0';
    cat->status.has_more = false;
  }

  char enc_q[288], enc_cur[512];
  url_encode_component(cat->cur_query, enc_q, sizeof enc_q);
  url_encode_component(append_next_page ? cat->next_cursor : "", enc_cur, sizeof enc_cur);

  worker_job job = {
    .kind = JOB_DISCOVER_QUERY,
    .seq = ++cat->discover_req_seq,
    .append_page = append_next_page,
  };
  int n = snprintf(job.url, sizeof job.url, "%s/api/v2/plugins?sort=%s&limit=%d",
                   cat->base_url, cat->cur_sort[0] ? cat->cur_sort : "popular", CATALOG_PAGE_LIMIT);
  if (n > 0 && (size_t)n < sizeof job.url && enc_q[0]) {
    n += snprintf(job.url + n, sizeof job.url - (size_t)n, "&q=%s", enc_q);
  }
  if (n > 0 && (size_t)n < sizeof job.url && cat->cur_tier >= 1 && cat->cur_tier <= 3) {
    n += snprintf(job.url + n, sizeof job.url - (size_t)n, "&tier=%d", cat->cur_tier);
  }
  if (n > 0 && (size_t)n < sizeof job.url && cat->cur_api_class >= 1 && cat->cur_api_class <= 3) {
    n += snprintf(job.url + n, sizeof job.url - (size_t)n, "&api_class=%d", cat->cur_api_class);
  }
  if (n > 0 && (size_t)n < sizeof job.url && append_next_page && enc_cur[0]) {
    (void)snprintf(job.url + n, sizeof job.url - (size_t)n, "&cursor=%s", enc_cur);
  }

  if (enqueue_job(cat, &job)) {
    if (append_next_page) {
      cat->status.loading_more = true;
    } else {
      cat->status.syncing = true;
      copy_str(cat->status.status_msg, sizeof cat->status.status_msg, "Searching plugin catalog...");
    }
    publish_catalog(cat);
  }
}

static void enqueue_detail_fetch(ccord_catalog *cat, const char *id, bool install_after)
{
  if (!cat || !id || !id[0]) return;
  if (!cat->http.get) {
    cat->status.offline = true;
    copy_str(cat->status.detail_error, sizeof cat->status.detail_error, "Offline mode: cannot fetch plugin details");
    publish_catalog(cat);
    return;
  }
  char enc_id[192];
  url_encode_component(id, enc_id, sizeof enc_id);
  worker_job job = {
    .kind = JOB_FETCH_DETAIL,
    .seq = ++cat->detail_req_seq,
    .install_after_detail = install_after,
  };
  copy_str(job.id, sizeof job.id, id);
  snprintf(job.url, sizeof job.url, "%s/api/v2/plugins/%s", cat->base_url, enc_id);
  if (cat->has_detail && cat->detail && strcmp(cat->detail->id, id) == 0 && cat->detail_etag[0]) {
    copy_str(job.etag, sizeof job.etag, cat->detail_etag);
  }
  copy_str(cat->detail_target_id, sizeof cat->detail_target_id, id);
  if (enqueue_job(cat, &job)) {
    cat->status.detail_loading = true;
    cat->status.detail_error[0] = '\0';
    if (install_after) {
      set_installing(cat, id, true);
      snprintf(cat->status.status_msg, sizeof cat->status.status_msg, "Fetching verified metadata for %s...", id);
    }
    publish_catalog(cat);
  }
}

void ccord_catalog_fetch_detail(ccord_catalog *cat, const char *id)
{
  enqueue_detail_fetch(cat, id, false);
}

void ccord_catalog_refresh(ccord_catalog *cat)
{
  if (!cat) return;
  ccord_catalog_check_updates(cat);
  ccord_catalog_query(cat, cat->cur_query, cat->cur_tier, cat->cur_api_class,
                      cat->cur_sort[0] ? cat->cur_sort : "popular", false);
}

void ccord_catalog_install(ccord_catalog *cat, const char *id)
{
  if (!cat || !id || !id[0] || is_installing(cat, id)) return;
  // If we already have verified full metadata (SHA-256 + download_url), download immediately!
  const cat_item *best = find_best_item(cat, id);
  if (best && best->has_detail && best->download_url[0] && strlen(best->sha256) == 64) {
    queue_install_item(cat, best, false);
    return;
  }
  // Otherwise (slim Discover card), fetch /api/v2/plugins/{id} first and auto-chain into install!
  enqueue_detail_fetch(cat, id, true);
}

void ccord_catalog_run_auto_updates(ccord_catalog *cat)
{
  if (!cat) return;
  size_t queued = 0;
  for (size_t i = 0; i < cat->nupdates; i++) {
    const cat_item *it = &cat->updates[i];
    if (is_installing(cat, it->id)) continue;
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
    if (compute_update_info(it, local_ver, consented, &avail, &review) && avail && !review) {
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
  copy_str(cat->cur_sort, sizeof cat->cur_sort, "popular");
  if (data_dir && data_dir[0]) {
    cat->data_dir = ccord_strdup(data_dir);
    cat->has_data_dir = cat->data_dir != nullptr;
  }

  const char *env_url = getenv("CCORD_CATALOG_URL");
  char url_buf[256];
  copy_str(url_buf, sizeof url_buf,
           (env_url && env_url[0]) ? env_url : CATALOG_DEFAULT_BASE_URL);
  size_t blen = strlen(url_buf);
  while (blen > 0 && url_buf[blen - 1] == '/') {
    url_buf[--blen] = '\0';
  }
  cat->base_url = ccord_strdup(url_buf);
  if (!cat->base_url) {
    ccord_free(cat->data_dir);
    ccord_free(cat);
    return nullptr;
  }
  copy_str(cat->status.catalog_url, sizeof cat->status.catalog_url, cat->base_url);

  load_prefs(cat);
  load_installed_meta(cat);
  load_revocations_cache(cat);

  // Worker thread, mutex/cond, and job/result queues are allocated lazily on first job enqueue.
  atomic_init(&cat->ready_count, 0u);
  if (!cat->http.get && !cat->http.post_json) {
    cat->status.offline = true;
    copy_str(cat->status.status_msg, sizeof cat->status.status_msg, "Offline mode");
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
    cat->thread_started = false;
  }
  if (cat->jobs) {
    while (cat->job_count > 0) {
      ccord_free(cat->jobs[cat->job_head].post_body);
      ccord_free(cat->jobs[cat->job_head].dest_path);
      cat->job_head = (cat->job_head + 1) % CATALOG_QUEUE_CAP;
      cat->job_count--;
    }
    ccord_free(cat->jobs);
  }
  if (cat->results) {
    while (cat->res_count > 0) {
      ccord_free(cat->results[cat->res_head].body);
      ccord_free(cat->results[cat->res_head].dest_path);
      cat->res_head = (cat->res_head + 1) % CATALOG_QUEUE_CAP;
      cat->res_count--;
    }
    ccord_free(cat->results);
  }
  if (cat->cv) ccord_cond_destroy(cat->cv);
  if (cat->mu) ccord_mutex_destroy(cat->mu);
  ccord_free(cat->installing_ids);
  ccord_free(cat->detail);
  ccord_free(cat->discover);
  ccord_free(cat->updates);
  ccord_free(cat->revoked);
  ccord_free(cat->meta);
  ccord_free(cat->data_dir);
  ccord_free(cat->base_url);
  ccord_free(cat);
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
  if (cat->prefs.auto_update_enabled && cat->nupdates > 0) {
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
  if (res->kind == JOB_DISCOVER_QUERY) {
    if (res->seq != cat->discover_req_seq) {
      ccord_free(res->body);
      res->body = nullptr;
      return;
    }
    cat->status.syncing = false;
    cat->status.loading_more = false;
    if (!res->ok) {
      cat->status.offline = true;
      copy_str(cat->status.status_msg, sizeof cat->status.status_msg,
               res->error[0] ? res->error : "Catalog browse failed");
    } else if (res->body && parse_discover_page_json(cat, res->body, res->body_len, res->append_page)) {
      cat->status.offline = false;
      cat->status.last_sync_unix = (int64_t)time(nullptr);
      snprintf(cat->status.status_msg, sizeof cat->status.status_msg,
               "Showing %zu of %zu catalog plugin%s",
               cat->ndiscover, cat->status.total_count, cat->status.total_count == 1 ? "" : "s");
    } else {
      copy_str(cat->status.status_msg, sizeof cat->status.status_msg, "Invalid /api/v2/plugins response");
    }
    ccord_free(res->body);
    res->body = nullptr;
    publish_catalog(cat);
    return;
  }

  if (res->kind == JOB_FETCH_DETAIL) {
    if (res->seq == cat->detail_req_seq) {
      cat->status.detail_loading = false;
    }
    if (!res->ok) {
      if (res->install_after_detail) set_installing(cat, res->id, false);
      copy_str(cat->status.detail_error, sizeof cat->status.detail_error,
               res->error[0] ? res->error : "Failed loading plugin details");
      if (res->install_after_detail && cat->cbs.toast) {
        cat->cbs.toast(cat->cbs.ud, cat->status.detail_error, 2);
      }
    } else if (res->not_modified && cat->has_detail && cat->detail && strcmp(cat->detail->id, res->id) == 0) {
      cat->status.offline = false;
      cat->status.detail_error[0] = '\0';
      if (res->install_after_detail) {
        queue_install_item(cat, cat->detail, false);
      }
    } else if (res->body) {
      cat_item parsed = {0};
      if (parse_detail_json(res->body, res->body_len, &parsed)) {
        if (!cat->detail) cat->detail = ccord_calloc(1, sizeof *cat->detail);
        if (cat->detail) {
          cat->status.offline = false;
          cat->status.detail_error[0] = '\0';
          *cat->detail = parsed;
          cat->has_detail = true;
          if (res->etag[0]) copy_str(cat->detail_etag, sizeof cat->detail_etag, res->etag);
          if (res->install_after_detail) {
            queue_install_item(cat, cat->detail, false);
          }
        } else {
          if (res->install_after_detail) set_installing(cat, res->id, false);
          copy_str(cat->status.detail_error, sizeof cat->status.detail_error, "Out of memory");
        }
      } else {
        if (res->install_after_detail) set_installing(cat, res->id, false);
        copy_str(cat->status.detail_error, sizeof cat->status.detail_error, "Invalid plugin detail payload");
      }
    }
    ccord_free(res->body);
    res->body = nullptr;
    publish_catalog(cat);
    return;
  }

  if (res->kind == JOB_CHECK_UPDATES) {
    if (res->ok && res->body) {
      bool rev_stale = false;
      int64_t rseq = 0;
      if (parse_updates_check_json(cat, res->body, res->body_len, &rev_stale, &rseq)) {
        cat->status.offline = false;
        cat->status.last_sync_unix = (int64_t)time(nullptr);
        if (rev_stale || (cat->nrevoked == 0 && rseq > 0)) {
          enqueue_revocations_fetch(cat);
        }
        if (cat->prefs.auto_update_enabled) {
          ccord_catalog_run_auto_updates(cat);
        }
      }
    }
    ccord_free(res->body);
    res->body = nullptr;
    publish_catalog(cat);
    return;
  }

  if (res->kind == JOB_FETCH_REVOCATIONS) {
    if (res->ok && !res->not_modified && res->body) {
      if (merge_revocations_json(cat, res->body, res->body_len, false)) {
        if (res->etag[0]) copy_str(cat->rev_etag, sizeof cat->rev_etag, res->etag);
        save_revocations_cache(cat);
        enforce_revocations(cat);
      }
    }
    ccord_free(res->body);
    res->body = nullptr;
    publish_catalog(cat);
    return;
  }

  if (res->kind == JOB_INSTALL) {
    set_installing(cat, res->id, false);
    if (!res->ok) {
      copy_str(cat->status.status_msg, sizeof cat->status.status_msg,
               res->error[0] ? res->error : "Install failed");
      if (cat->cbs.toast) cat->cbs.toast(cat->cbs.ud, cat->status.status_msg, 2);
      publish_catalog(cat);
      return;
    }
    char err_buf[256] = "";
    bool imported = res->dest_path && cat->cbs.import_verified_cce &&
                    cat->cbs.import_verified_cce(cat->cbs.ud, res->dest_path, err_buf, sizeof err_buf);
    if (res->dest_path) ccord_file_unlink(res->dest_path);
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

  // Background check ONLY verifies installed plugins + revocations (0 requests if 0 plugins installed!).
  if ((cat->http.post_json || cat->http.get) && !cat->initial_check_queued &&
      now_ms - cat->created_ms >= CATALOG_STARTUP_DELAY_MS) {
    cat->initial_check_queued = true;
    cat->last_check_ms = now_ms;
    ccord_catalog_check_updates(cat);
  } else if ((cat->http.post_json || cat->http.get) && cat->initial_check_queued &&
             now_ms - cat->last_check_ms >= CATALOG_REFRESH_INTERVAL_MS) {
    cat->last_check_ms = now_ms;
    ccord_catalog_check_updates(cat);
  }

  // Lock-free atomic fast-path when idle (avoids mutex lock at 60-144 Hz).
  if (!cat->thread_started || atomic_load_explicit(&cat->ready_count, memory_order_acquire) == 0) {
    return;
  }

  for (;;) {
    worker_result res = {0};
    bool has = false;
    ccord_mutex_lock(cat->mu);
    if (cat->res_count > 0) {
      res = cat->results[cat->res_head];
      cat->res_head = (cat->res_head + 1) % CATALOG_QUEUE_CAP;
      cat->res_count--;
      atomic_store_explicit(&cat->ready_count, (uint32_t)cat->res_count, memory_order_release);
      has = true;
    }
    ccord_mutex_unlock(cat->mu);
    if (!has) break;
    handle_result(cat, &res);
    ccord_free(res.dest_path);
  }
}
