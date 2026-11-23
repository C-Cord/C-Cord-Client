// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "catalog_internal.h"

#if defined(CCORD_HAVE_SODIUM) && CCORD_HAVE_SODIUM
#include <sodium.h>
#endif

static int hex_nibble(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool cat_hex_decode(const char *hex, uint8_t *out, size_t out_bytes)
{
  if (!hex || !out) return false;
  if (strlen(hex) != out_bytes * 2u) return false;
  for (size_t i = 0; i < out_bytes; i++) {
    int hi = hex_nibble(hex[i * 2u]);
    int lo = hex_nibble(hex[i * 2u + 1u]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static void compute_pubkey_id(const uint8_t pk[32], char id_out[17])
{
  unsigned char digest[32];
  ccord_sha256(pk, 32, digest);
  static const char k_hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 8; i++) {
    id_out[i * 2u] = k_hex[(size_t)(digest[i] >> 4)];
    id_out[i * 2u + 1u] = k_hex[(size_t)(digest[i] & 0x0fu)];
  }
  id_out[16] = '\0';
}

static bool parse_pubkeys_csv(const char *hex_csv, uint8_t pks[CATALOG_MAX_PUBKEYS][32],
                              char ids[CATALOG_MAX_PUBKEYS][17], size_t *io_count)
{
  if (!hex_csv || !io_count) return false;
  const char *p = hex_csv;
  while (*p) {
    while (*p && (*p == ',' || isspace((unsigned char)*p))) p++;
    if (!*p) break;
    const char *s = p;
    while (*p && *p != ',') p++;
    const char *e = p;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    size_t tlen = (size_t)(e - s);
    if (tlen != 64) return false;
    char tmp[65];
    memcpy(tmp, s, 64);
    tmp[64] = '\0';
    uint8_t pk[32];
    if (!cat_hex_decode(tmp, pk, sizeof pk)) return false;
    bool dup = false;
    for (size_t k = 0; k < *io_count; k++) {
      if (memcmp(pks[k], pk, 32) == 0) { dup = true; break; }
    }
    if (!dup) {
      if (*io_count >= CATALOG_MAX_PUBKEYS) return false;
      memcpy(pks[*io_count], pk, 32);
      compute_pubkey_id(pk, ids[*io_count]);
      (*io_count)++;
    }
  }
  return true;
}

bool ccord_catalog_set_pubkeys(ccord_catalog *cat, const char *hex_csv)
{
  if (!cat) return false;
  if (!hex_csv || !hex_csv[0]) {
    cat->npubkeys = 0;
    cat->has_detail = false;
    cat->detail_etag[0] = '\0';
    cat->rev_etag[0] = '\0';
    return true;
  }
  uint8_t tmp_pks[CATALOG_MAX_PUBKEYS][32];
  char tmp_ids[CATALOG_MAX_PUBKEYS][17];
  size_t cnt = 0;
  if (!parse_pubkeys_csv(hex_csv, tmp_pks, tmp_ids, &cnt) || cnt == 0) return false;
  memcpy(cat->pubkeys, tmp_pks, sizeof tmp_pks);
  memcpy(cat->pubkey_ids, tmp_ids, sizeof tmp_ids);
  cat->npubkeys = cnt;
  cat->has_detail = false;
  cat->detail_etag[0] = '\0';
  cat->rev_etag[0] = '\0';
  return true;
}

#if defined(CCORD_HAVE_SODIUM) && CCORD_HAVE_SODIUM
static bool key_id_equal_ci(const char *a, const char *b)
{
  if (!a || !b) return false;
  if (strlen(a) != 16 || strlen(b) != 16) return false;
  for (size_t i = 0; i < 16; i++) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  }
  return true;
}
#endif

bool cat_verify_ed25519(const ccord_catalog *cat, const char *alg, const char *key_id,
                        const char *sig_hex, const uint8_t *msg, size_t msg_len)
{
  if (!cat || cat->npubkeys == 0) return true;
  if (!alg || strcmp(alg, "ed25519") != 0) return false;
  uint8_t sig_bytes[64];
  if (!sig_hex || !cat_hex_decode(sig_hex, sig_bytes, sizeof sig_bytes)) return false;
#if defined(CCORD_HAVE_SODIUM) && CCORD_HAVE_SODIUM
  if (sodium_init() < 0) return false;
  for (int pass = 0; pass < 2; pass++) {
    for (size_t k = 0; k < cat->npubkeys; k++) {
      bool id_match = key_id && key_id[0] && key_id_equal_ci(cat->pubkey_ids[k], key_id);
      if ((pass == 0 && !id_match) || (pass == 1 && id_match)) continue;
      if (crypto_sign_verify_detached(sig_bytes, msg, (unsigned long long)msg_len, cat->pubkeys[k]) == 0) {
        return true;
      }
    }
  }
  return false;
#else
  (void)key_id; (void)msg; (void)msg_len; (void)sig_bytes;
  CCORD_LOG_E("catalog: Ed25519 public key configured but C-Cord was built without libsodium; rejecting payload");
  return false;
#endif
}

void cat_copy_str(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  size_t n = strlen(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
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

bool cat_is_installing(const ccord_catalog *cat, const char *id)
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
    cat_copy_str(cat->installing_ids[cat->ninstalling++], sizeof cat->installing_ids[0], id);
  }
}

installed_meta *cat_find_meta(ccord_catalog *cat, const char *id)
{
  if (!cat || !id) return nullptr;
  for (size_t i = 0; i < cat->nmeta; i++) {
    if (strcmp(cat->meta[i].id, id) == 0) return &cat->meta[i];
  }
  return nullptr;
}

const installed_meta *cat_find_meta_const(const ccord_catalog *cat, const char *id)
{
  if (!cat || !id) return nullptr;
  for (size_t i = 0; i < cat->nmeta; i++) {
    if (strcmp(cat->meta[i].id, id) == 0) return &cat->meta[i];
  }
  return nullptr;
}

installed_meta *cat_upsert_meta(ccord_catalog *cat, const char *id)
{
  installed_meta *m = cat_find_meta(cat, id);
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
  cat_copy_str(m->id, sizeof m->id, id);
  m->auto_update = true;
  return m;
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
    res.preapprove_consent = job.preapprove_consent;
    res.expected_perms = job.expected_perms;
    res.approved_perms = job.approved_perms;
    cat_copy_str(res.expected_allowed_hosts, sizeof res.expected_allowed_hosts, job.expected_allowed_hosts);
    res.tier = job.tier;
    cat_copy_str(res.id, sizeof res.id, job.id);
    cat_copy_str(res.version, sizeof res.version, job.version);
    res.dest_path = job.dest_path;
    job.dest_path = nullptr;

    if (job.kind == JOB_DISCOVER_QUERY || job.kind == JOB_FETCH_DETAIL || job.kind == JOB_FETCH_REVOCATIONS) {
      if (!cat->http.get) {
        cat_copy_str(res.error, sizeof res.error, "network catalog fetcher unavailable");
      } else {
        res.ok = cat->http.get(cat->http.ud, job.url, job.etag[0] ? job.etag : nullptr,
                               CATALOG_MAX_API_BYTES, &res.body, &res.body_len,
                               res.etag, sizeof res.etag, &res.not_modified,
                               res.error, sizeof res.error);
      }
    } else if (job.kind == JOB_CHECK_UPDATES) {
      if (!cat->http.post_json) {
        cat_copy_str(res.error, sizeof res.error, "network catalog updater unavailable");
      } else {
        res.ok = cat->http.post_json(cat->http.ud, job.url,
                                     job.post_body ? job.post_body : "{}",
                                     job.post_body ? job.post_len : 2u,
                                     CATALOG_MAX_API_BYTES, &res.body, &res.body_len,
                                     res.error, sizeof res.error);
      }
    } else if (job.kind == JOB_INSTALL) {
      if (!cat->http.download_file || !res.dest_path) {
        cat_copy_str(res.error, sizeof res.error, "network downloader unavailable");
      } else if (cat->http.download_file(cat->http.ud, job.url, res.dest_path, CATALOG_MAX_CCE_BYTES,
                                         res.error, sizeof res.error)) {
        char actual_sha[65];
        if (!ccord_catalog_sha256_file_hex(res.dest_path, actual_sha)) {
          ccord_file_unlink(res.dest_path);
          cat_copy_str(res.error, sizeof res.error, "failed computing SHA-256 of downloaded package");
        } else if (!cat_hex_equal_ci(actual_sha, job.expected_sha256)) {
          ccord_file_unlink(res.dest_path);
          snprintf(res.error, sizeof res.error, "SHA-256 integrity mismatch for %s (download aborted)", job.id);
        } else {
          cat_copy_str(res.sha256, sizeof res.sha256, actual_sha);
          res.ok = true;
        }
      }
    } else if (job.kind == JOB_FETCH_ICON) {
      if (cat->http.download_file && res.dest_path) {
        char tmp_path[CCORD_PATH_MAX + 80];
        int tn = snprintf(tmp_path, sizeof tmp_path, "%s.tmp", res.dest_path);
        if (tn > 0 && (size_t)tn < sizeof tmp_path) {
          if (cat->http.download_file(cat->http.ud, job.url, tmp_path, CCORD_MANIFEST_MAX_ICON_BYTES,
                                      res.error, sizeof res.error)) {
            uint64_t fsz = 0;
            if (ccord_file_size(tmp_path, &fsz) && fsz > 0 && fsz <= CCORD_MANIFEST_MAX_ICON_BYTES &&
                rename(tmp_path, res.dest_path) == 0) {
              res.ok = true;
            } else {
              ccord_file_unlink(tmp_path);
            }
          } else {
            ccord_file_unlink(tmp_path);
          }
        }
      }
    }
    ccord_free(job.post_body);
    push_result(cat, &res);
  }
}

// ---------------------------------------------------------------- icon disk cache

static bool icon_is_inflight(const ccord_catalog *cat, const char *fname)
{
  for (size_t i = 0; i < cat->nicon_inflight; i++) {
    if (strcmp(cat->icon_inflight[i], fname) == 0) return true;
  }
  return false;
}

static void icon_set_inflight(ccord_catalog *cat, const char *fname, bool on)
{
  for (size_t i = 0; i < cat->nicon_inflight; i++) {
    if (strcmp(cat->icon_inflight[i], fname) == 0) {
      if (!on) {
        memmove(&cat->icon_inflight[i], &cat->icon_inflight[i + 1],
                (cat->nicon_inflight - i - 1) * sizeof cat->icon_inflight[0]);
        cat->nicon_inflight--;
      }
      return;
    }
  }
  if (on && cat->nicon_inflight < CATALOG_MAX_INFLIGHT_ICONS) {
    cat_copy_str(cat->icon_inflight[cat->nicon_inflight++], sizeof cat->icon_inflight[0], fname);
  }
}

void cat_maybe_queue_icon(ccord_catalog *cat, const char *icon_url)
{
  if (!cat || !cat->has_data_dir || !cat->http.download_file || !icon_url || !icon_url[0]) return;
  char fname[32];
  if (!cat_extract_icon_filename(icon_url, fname)) return;
  char existing[CCORD_PATH_MAX];
  if (cat_resolve_cached_icon_path(cat, icon_url, existing, sizeof existing)) return; // already cached on disk
  if (icon_is_inflight(cat, fname)) return;

  char pdir[CCORD_PATH_MAX + 32], cdir[CCORD_PATH_MAX + 48], dpath[CCORD_PATH_MAX + 96];
  snprintf(pdir, sizeof pdir, "%s/plugins", cat->data_dir);
  snprintf(cdir, sizeof cdir, "%s/plugins/icon_cache", cat->data_dir);
  cat_mkdir_ok(cat->data_dir);
  cat_mkdir_ok(pdir);
  cat_mkdir_ok(cdir);
  int dn = snprintf(dpath, sizeof dpath, "%s/%s", cdir, fname);
  if (dn < 0 || (size_t)dn >= sizeof dpath) return;

  worker_job job = {.kind = JOB_FETCH_ICON};
  cat_copy_str(job.id, sizeof job.id, fname);
  cat_copy_str(job.url, sizeof job.url, icon_url);
  job.dest_path = ccord_malloc((size_t)dn + 1u);
  if (!job.dest_path) return;
  memcpy(job.dest_path, dpath, (size_t)dn + 1u);
  if (enqueue_job(cat, &job)) {
    icon_set_inflight(cat, fname, true);
  }
}

// ---------------------------------------------------------------- public actions

static void enqueue_revocations_fetch(ccord_catalog *cat)
{
  if (!cat || !cat->http.get) return;
  worker_job job = {.kind = JOB_FETCH_REVOCATIONS};
  snprintf(job.url, sizeof job.url, "%s/api/v2/revocations?since=%" PRId64, cat->base_url, cat->rev_seq);
  cat_copy_str(job.etag, sizeof job.etag, cat->rev_etag);
  (void)enqueue_job(cat, &job);
}

static void queue_install_item_ex(ccord_catalog *cat, const cat_item *it, bool is_auto,
                                  bool preapprove_consent, uint64_t approved_perms)
{
  if (!cat || !it || !cat->has_data_dir || !cat_valid_catalog_id(it->id) || !cat_valid_catalog_version(it->version) ||
      !it->download_url[0] || strlen(it->sha256) != 64) return;
  char rev_reason[256] = "";
  if (ccord_catalog_is_hash_or_id_revoked(cat, it->id, it->sha256, rev_reason, sizeof rev_reason)) {
    cat_copy_str(cat->status.status_msg, sizeof cat->status.status_msg,
                 rev_reason[0] ? rev_reason : "Refused to install revoked plugin");
    if (cat->cbs.toast) cat->cbs.toast(cat->cbs.ud, cat->status.status_msg, 2);
    cat_publish_catalog(cat);
    return;
  }
  char tmp_dir[CCORD_PATH_MAX + 32], pdir[CCORD_PATH_MAX + 32];
  snprintf(pdir, sizeof pdir, "%s/plugins", cat->data_dir);
  snprintf(tmp_dir, sizeof tmp_dir, "%s/plugins/.download_tmp", cat->data_dir);
  cat_mkdir_ok(cat->data_dir);
  cat_mkdir_ok(pdir);
  cat_mkdir_ok(tmp_dir);

  bool allow_preapprove = preapprove_consent && ((it->perms_mask & ~approved_perms) == 0);
  worker_job job = {
    .kind = JOB_INSTALL,
    .tier = it->tier,
    .is_auto_update = is_auto,
    .preapprove_consent = allow_preapprove,
    .expected_perms = allow_preapprove ? approved_perms : it->perms_mask,
    .approved_perms = approved_perms,
  };
  cat_copy_str(job.id, sizeof job.id, it->id);
  cat_copy_str(job.url, sizeof job.url, it->download_url);
  cat_copy_str(job.expected_sha256, sizeof job.expected_sha256, it->sha256);
  cat_copy_str(job.version, sizeof job.version, it->version);
  cat_copy_str(job.expected_allowed_hosts, sizeof job.expected_allowed_hosts, it->allowed_hosts);
  char dpath[CCORD_PATH_MAX + 160];
  int dn = snprintf(dpath, sizeof dpath, "%s/%s-%s.cce", tmp_dir, it->id, it->version);
  if (dn < 0 || (size_t)dn >= sizeof dpath) return;
  job.dest_path = ccord_malloc((size_t)dn + 1u);
  if (!job.dest_path) return;
  memcpy(job.dest_path, dpath, (size_t)dn + 1u);
  if (enqueue_job(cat, &job)) {
    set_installing(cat, it->id, true);
    snprintf(cat->status.status_msg, sizeof cat->status.status_msg, "Downloading %s v%s...", it->name, it->version);
    cat_publish_catalog(cat);
  }
}

static void queue_install_item(ccord_catalog *cat, const cat_item *it, bool is_auto)
{
  queue_install_item_ex(cat, it, is_auto, false, 0);
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
        cat_copy_str(ids[nids++], sizeof ids[0], cat->meta[i].id);
      }
    }
  }
  if (nids == 0) {
    // Zero third-party plugins installed: skip background network polling entirely!
    ccord_free(cat->updates);
    cat->updates = nullptr;
    cat->nupdates = 0;
    cat_publish_catalog(cat);
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

void ccord_catalog_query_ex(ccord_catalog *cat, const char *q, int tier, int api_class, const char *platform,
                            const char *sort, bool append_next_page)
{
  if (!cat) return;
  if (!cat->http.get) {
    cat->status.offline = true;
    cat_copy_str(cat->status.status_msg, sizeof cat->status.status_msg, "Offline build: catalog browse unavailable");
    cat_publish_catalog(cat);
    return;
  }
  if (append_next_page && (!cat->status.has_more || !cat->next_cursor[0] || cat->status.loading_more)) {
    return;
  }
  if (!append_next_page) {
    cat_copy_str(cat->cur_query, sizeof cat->cur_query, q ? q : "");
    cat->cur_tier = (tier >= 1 && tier <= 3) ? tier : 0;
    cat->cur_api_class = (api_class >= 1 && api_class <= 3) ? api_class : 0;
    cat_copy_str(cat->cur_platform, sizeof cat->cur_platform, platform ? platform : "");
    cat_copy_str(cat->cur_sort, sizeof cat->cur_sort, (sort && sort[0]) ? sort : "popular");
    cat->next_cursor[0] = '\0';
    cat->status.has_more = false;
  }

  char enc_q[288], enc_cur[512], enc_plat[48];
  url_encode_component(cat->cur_query, enc_q, sizeof enc_q);
  url_encode_component(append_next_page ? cat->next_cursor : "", enc_cur, sizeof enc_cur);
  url_encode_component(cat->cur_platform, enc_plat, sizeof enc_plat);

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
  if (n > 0 && (size_t)n < sizeof job.url && enc_plat[0]) {
    n += snprintf(job.url + n, sizeof job.url - (size_t)n, "&platform=%s", enc_plat);
  }
  if (n > 0 && (size_t)n < sizeof job.url && append_next_page && enc_cur[0]) {
    (void)snprintf(job.url + n, sizeof job.url - (size_t)n, "&cursor=%s", enc_cur);
  }

  if (enqueue_job(cat, &job)) {
    if (append_next_page) {
      cat->status.loading_more = true;
    } else {
      cat->status.syncing = true;
      cat_copy_str(cat->status.status_msg, sizeof cat->status.status_msg, "Searching plugin catalog...");
    }
    cat_publish_catalog(cat);
  }
}

void ccord_catalog_query(ccord_catalog *cat, const char *q, int tier, int api_class, const char *sort,
                         bool append_next_page)
{
  ccord_catalog_query_ex(cat, q, tier, api_class, "", sort, append_next_page);
}

static void enqueue_detail_fetch(ccord_catalog *cat, const char *id, bool install_after,
                                 bool preapprove_consent, uint64_t approved_perms)
{
  if (!cat || !id || !id[0]) return;
  if (!cat->http.get) {
    cat->status.offline = true;
    cat_copy_str(cat->status.detail_error, sizeof cat->status.detail_error, "Offline mode: cannot fetch plugin details");
    cat_publish_catalog(cat);
    return;
  }
  char enc_id[192];
  url_encode_component(id, enc_id, sizeof enc_id);
  worker_job job = {
    .kind = JOB_FETCH_DETAIL,
    .seq = ++cat->detail_req_seq,
    .install_after_detail = install_after,
    .preapprove_consent = preapprove_consent,
    .approved_perms = approved_perms,
  };
  cat_copy_str(job.id, sizeof job.id, id);
  snprintf(job.url, sizeof job.url, "%s/api/v2/plugins/%s", cat->base_url, enc_id);
  if (cat->has_detail && cat->detail && strcmp(cat->detail->id, id) == 0 && cat->detail_etag[0]) {
    cat_copy_str(job.etag, sizeof job.etag, cat->detail_etag);
  }
  cat_copy_str(cat->detail_target_id, sizeof cat->detail_target_id, id);
  if (enqueue_job(cat, &job)) {
    cat->status.detail_loading = true;
    cat->status.detail_error[0] = '\0';
    if (install_after) {
      set_installing(cat, id, true);
      snprintf(cat->status.status_msg, sizeof cat->status.status_msg, "Fetching verified metadata for %s...", id);
    }
    cat_publish_catalog(cat);
  }
}

void ccord_catalog_fetch_detail(ccord_catalog *cat, const char *id)
{
  enqueue_detail_fetch(cat, id, false, false, 0);
}

void ccord_catalog_refresh(ccord_catalog *cat)
{
  if (!cat) return;
  ccord_catalog_check_updates(cat);
  ccord_catalog_query_ex(cat, cat->cur_query, cat->cur_tier, cat->cur_api_class, cat->cur_platform,
                         cat->cur_sort[0] ? cat->cur_sort : "popular", false);
}

void ccord_catalog_install(ccord_catalog *cat, const char *id)
{
  if (!cat || !id || !id[0] || cat_is_installing(cat, id)) return;
  // If we already have verified full metadata (SHA-256 + download_url), download immediately!
  const cat_item *best = cat_find_best_item(cat, id);
  if (best && best->has_detail && best->download_url[0] && strlen(best->sha256) == 64) {
    queue_install_item(cat, best, false);
    return;
  }
  // Otherwise (slim Discover card), fetch /api/v2/plugins/{id} first and auto-chain into install!
  enqueue_detail_fetch(cat, id, true, false, 0);
}

void ccord_catalog_install_approved(ccord_catalog *cat, const char *id, uint64_t approved_perms)
{
  if (!cat || !id || !id[0] || cat_is_installing(cat, id)) return;
  const cat_item *best = cat_find_best_item(cat, id);
  if (best && best->has_detail && best->download_url[0] && strlen(best->sha256) == 64) {
    queue_install_item_ex(cat, best, false, true, approved_perms);
    return;
  }
  enqueue_detail_fetch(cat, id, true, true, approved_perms);
}

void ccord_catalog_run_auto_updates(ccord_catalog *cat)
{
  if (!cat) return;
  size_t queued = 0;
  for (size_t i = 0; i < cat->nupdates; i++) {
    const cat_item *it = &cat->updates[i];
    if (cat_is_installing(cat, it->id)) continue;
    char local_ver[33] = "";
    uint64_t consented = 0;
    bool bundled = false;
    if (!cat->cbs.get_local_plugin ||
        !cat->cbs.get_local_plugin(cat->cbs.ud, it->id, local_ver, sizeof local_ver, &consented, &bundled)) {
      continue;
    }
    if (bundled) continue;
    const installed_meta *m = cat_find_meta_const(cat, it->id);
    if (m && !m->auto_update) continue;
    if ((uint8_t)it->tier > cat->prefs.auto_update_min_tier) continue;

    char consented_hosts[1024] = "";
    bool had_consented_hosts = false;
    if (cat->cbs.get_local_allowed_hosts) {
      (void)cat->cbs.get_local_allowed_hosts(cat->cbs.ud, it->id, consented_hosts,
                                             sizeof consented_hosts, &had_consented_hosts);
    }

    bool avail = false, review = false;
    if (cat_compute_update_info(it, local_ver, consented, consented_hosts, had_consented_hosts,
                                &avail, &review) && avail && !review) {
      queue_install_item(cat, it, true);
      queued++;
    }
  }
  if (queued == 0) {
    cat_copy_str(cat->status.status_msg, sizeof cat->status.status_msg,
             "No eligible auto-updates (updates with new API permissions require review).");
    cat_publish_catalog(cat);
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
  cat_copy_str(cat->cur_sort, sizeof cat->cur_sort, "popular");
  if (data_dir && data_dir[0]) {
    cat->data_dir = ccord_strdup(data_dir);
    cat->has_data_dir = cat->data_dir != nullptr;
  }

  const char *env_url = getenv("CCORD_CATALOG_URL");
  char url_buf[256];
  cat_copy_str(url_buf, sizeof url_buf,
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
  cat_copy_str(cat->status.catalog_url, sizeof cat->status.catalog_url, cat->base_url);

#ifdef CCORD_CATALOG_PUBKEY
  if (CCORD_CATALOG_PUBKEY[0] != '\0') {
    if (!parse_pubkeys_csv(CCORD_CATALOG_PUBKEY, cat->pubkeys, cat->pubkey_ids, &cat->npubkeys)) {
      CCORD_LOG_W("catalog: invalid compile-time CCORD_CATALOG_PUBKEY");
    }
  }
#endif
  const char *env_pk = getenv("CCORD_CATALOG_PUBKEY");
  if (env_pk && env_pk[0] != '\0') {
    if (!parse_pubkeys_csv(env_pk, cat->pubkeys, cat->pubkey_ids, &cat->npubkeys)) {
      CCORD_LOG_W("catalog: invalid CCORD_CATALOG_PUBKEY environment variable");
    }
  }

  cat_load_prefs(cat);
  cat_load_installed_meta(cat);
  cat_load_revocations_cache(cat);

  // Worker thread, mutex/cond, and job/result queues are allocated lazily on first job enqueue.
  atomic_init(&cat->ready_count, 0u);
  if (!cat->http.get && !cat->http.post_json) {
    cat->status.offline = true;
    cat_copy_str(cat->status.status_msg, sizeof cat->status.status_msg, "Offline mode");
  }
  cat_enforce_revocations(cat);
  cat_publish_catalog(cat);
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
  cat_save_prefs(cat);
  cat_publish_catalog(cat);
  if (cat->prefs.auto_update_enabled && cat->nupdates > 0) {
    ccord_catalog_run_auto_updates(cat);
  }
}

void ccord_catalog_set_plugin_auto_update(ccord_catalog *cat, const char *id, bool enabled)
{
  if (!cat || !id) return;
  installed_meta *m = cat_upsert_meta(cat, id);
  if (!m) return;
  m->auto_update = enabled;
  cat_save_installed_meta(cat);
  cat_publish_catalog(cat);
}

void ccord_catalog_on_uninstalled(ccord_catalog *cat, const char *id)
{
  if (!cat || !id) return;
  for (size_t i = 0; i < cat->nmeta; i++) {
    if (strcmp(cat->meta[i].id, id) == 0) {
      memmove(&cat->meta[i], &cat->meta[i + 1], (cat->nmeta - i - 1) * sizeof cat->meta[0]);
      cat->nmeta--;
      cat_save_installed_meta(cat);
      break;
    }
  }
  cat_publish_catalog(cat);
}

void ccord_catalog_record_local_install(ccord_catalog *cat, const char *id, const char *sha256_hex)
{
  if (!cat || !id || !id[0] || !sha256_hex || strlen(sha256_hex) != 64) return;
  for (size_t i = 0; i < 64; i++) {
    if (hex_nibble(sha256_hex[i]) < 0) return;
  }
  installed_meta *m = cat_upsert_meta(cat, id);
  if (!m) return;
  for (size_t i = 0; i < 64; i++) {
    m->sha256[i] = (char)tolower((unsigned char)sha256_hex[i]);
  }
  m->sha256[64] = '\0';
  if (cat->cbs.get_local_plugin) {
    char ver[33] = "";
    if (cat->cbs.get_local_plugin(cat->cbs.ud, id, ver, sizeof ver, nullptr, nullptr) && ver[0]) {
      cat_copy_str(m->version, sizeof m->version, ver);
    }
  }
  cat_save_installed_meta(cat);
  cat_enforce_revocations(cat);
  cat_publish_catalog(cat);
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
      cat_copy_str(cat->status.status_msg, sizeof cat->status.status_msg,
               res->error[0] ? res->error : "Catalog browse failed");
    } else if (res->body && cat_parse_discover_page_json(cat, res->body, res->body_len, res->append_page)) {
      cat->status.offline = false;
      cat->status.last_sync_unix = (int64_t)time(nullptr);
      snprintf(cat->status.status_msg, sizeof cat->status.status_msg,
               "Showing %zu of %zu catalog plugin%s",
               cat->ndiscover, cat->status.total_count, cat->status.total_count == 1 ? "" : "s");
    } else {
      cat_copy_str(cat->status.status_msg, sizeof cat->status.status_msg, "Invalid /api/v2/plugins response");
    }
    ccord_free(res->body);
    res->body = nullptr;
    cat_publish_catalog(cat);
    return;
  }

  if (res->kind == JOB_FETCH_DETAIL) {
    if (res->seq == cat->detail_req_seq) {
      cat->status.detail_loading = false;
    }
    if (!res->ok) {
      if (res->install_after_detail) set_installing(cat, res->id, false);
      cat_copy_str(cat->status.detail_error, sizeof cat->status.detail_error,
               res->error[0] ? res->error : "Failed loading plugin details");
      if (res->install_after_detail && cat->cbs.toast) {
        cat->cbs.toast(cat->cbs.ud, cat->status.detail_error, 2);
      }
    } else if (res->not_modified && cat->has_detail && cat->detail && strcmp(cat->detail->id, res->id) == 0) {
      cat->status.offline = false;
      cat->status.detail_error[0] = '\0';
      if (cat->detail->icon_url[0]) cat_maybe_queue_icon(cat, cat->detail->icon_url);
      if (res->install_after_detail) {
        queue_install_item_ex(cat, cat->detail, false, res->preapprove_consent, res->approved_perms);
      }
    } else if (res->body) {
      cat_item parsed = {0};
      bool sig_failed = false;
      if (cat_parse_detail_json(cat, res->body, res->body_len, &parsed, &sig_failed)) {
        if (!cat->detail) cat->detail = ccord_calloc(1, sizeof *cat->detail);
        if (cat->detail) {
          cat->status.offline = false;
          cat->status.detail_error[0] = '\0';
          *cat->detail = parsed;
          cat->has_detail = true;
          if (cat->detail->icon_url[0]) cat_maybe_queue_icon(cat, cat->detail->icon_url);
          if (res->etag[0]) cat_copy_str(cat->detail_etag, sizeof cat->detail_etag, res->etag);
          if (res->install_after_detail) {
            queue_install_item_ex(cat, cat->detail, false, res->preapprove_consent, res->approved_perms);
          }
        } else {
          if (res->install_after_detail) set_installing(cat, res->id, false);
          cat_copy_str(cat->status.detail_error, sizeof cat->status.detail_error, "Out of memory");
        }
      } else {
        if (res->install_after_detail) set_installing(cat, res->id, false);
        const char *emsg = sig_failed
                             ? "Catalog signature verification failed (missing or invalid Ed25519 signature)"
                             : "Invalid plugin detail payload";
        cat_copy_str(cat->status.detail_error, sizeof cat->status.detail_error, emsg);
        if (sig_failed) {
          cat_copy_str(cat->status.status_msg, sizeof cat->status.status_msg, emsg);
          if (cat->has_detail && cat->detail && strcmp(cat->detail->id, res->id) == 0) {
            cat->has_detail = false;
          }
        }
        if (res->install_after_detail && cat->cbs.toast) {
          cat->cbs.toast(cat->cbs.ud, cat->status.detail_error, 2);
        }
      }
    }
    ccord_free(res->body);
    res->body = nullptr;
    cat_publish_catalog(cat);
    return;
  }

  if (res->kind == JOB_CHECK_UPDATES) {
    if (res->ok && res->body) {
      bool rev_stale = false;
      int64_t rseq = 0;
      if (cat_parse_updates_check_json(cat, res->body, res->body_len, &rev_stale, &rseq)) {
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
    cat_publish_catalog(cat);
    return;
  }

  if (res->kind == JOB_FETCH_REVOCATIONS) {
    if (res->ok && !res->not_modified && res->body) {
      if (cat_merge_revocations_json(cat, res->body, res->body_len, false)) {
        if (res->etag[0]) cat_copy_str(cat->rev_etag, sizeof cat->rev_etag, res->etag);
        if (strstr(cat->status.status_msg, "Rejected revocations") != nullptr) {
          cat->status.status_msg[0] = '\0';
        }
        cat_save_revocations_cache(cat);
        cat_enforce_revocations(cat);
      } else if (cat->npubkeys > 0) {
        cat_copy_str(cat->status.status_msg, sizeof cat->status.status_msg,
                     "Rejected revocations update (invalid or missing Ed25519 signature)");
      }
    }
    ccord_free(res->body);
    res->body = nullptr;
    cat_publish_catalog(cat);
    return;
  }

  if (res->kind == JOB_INSTALL) {
    set_installing(cat, res->id, false);
    if (!res->ok) {
      cat_copy_str(cat->status.status_msg, sizeof cat->status.status_msg,
               res->error[0] ? res->error : "Install failed");
      if (cat->cbs.toast) cat->cbs.toast(cat->cbs.ud, cat->status.status_msg, 2);
      cat_publish_catalog(cat);
      return;
    }
    char err_buf[256] = "";
    bool imported = false;
    if (res->dest_path) {
      if (cat->cbs.import_verified_cce_ex) {
        imported = cat->cbs.import_verified_cce_ex(cat->cbs.ud, res->dest_path, res->id, res->version,
                                                   res->expected_perms, res->expected_allowed_hosts,
                                                   res->preapprove_consent, err_buf, sizeof err_buf);
      } else if (res->preapprove_consent && cat->cbs.import_verified_cce_approved) {
        imported = cat->cbs.import_verified_cce_approved(cat->cbs.ud, res->dest_path, res->id, res->version,
                                                         res->expected_perms, true, err_buf, sizeof err_buf);
      } else if (cat->cbs.import_verified_cce) {
        imported = cat->cbs.import_verified_cce(cat->cbs.ud, res->dest_path, res->id, res->version,
                                                res->expected_perms, err_buf, sizeof err_buf);
      }
    }
    if (res->dest_path) ccord_file_unlink(res->dest_path);
    if (!imported) {
      snprintf(cat->status.status_msg, sizeof cat->status.status_msg,
               "Install failed for %s: %.160s", res->id, err_buf[0] ? err_buf : "import error");
      if (cat->cbs.toast) cat->cbs.toast(cat->cbs.ud, cat->status.status_msg, 2);
    } else {
      installed_meta *m = cat_upsert_meta(cat, res->id);
      if (m) {
        cat_copy_str(m->version, sizeof m->version, res->version);
        cat_copy_str(m->sha256, sizeof m->sha256, res->sha256);
        m->tier = res->tier;
        cat_save_installed_meta(cat);
      }
      snprintf(cat->status.status_msg, sizeof cat->status.status_msg,
               "%s %s v%s (SHA-256 verified)",
               res->is_auto_update ? "Auto-updated" : "Installed", res->id, res->version);
      if (cat->cbs.toast) cat->cbs.toast(cat->cbs.ud, cat->status.status_msg, 0);
    }
    cat_publish_catalog(cat);
    return;
  }

  if (res->kind == JOB_FETCH_ICON) {
    icon_set_inflight(cat, res->id, false);
    if (res->ok) {
      cat_publish_catalog(cat);
    }
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
