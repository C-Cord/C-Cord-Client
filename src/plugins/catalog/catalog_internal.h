// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
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
  uint64_t expected_perms;
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
  uint64_t expected_perms;
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


// Cross-TU internal helpers
void cat_copy_str(char *dst, size_t cap, const char *src);
void cat_mkdir_ok(const char *p);
char *cat_read_entire_file(const char *path, size_t max_bytes, size_t *out_len);
void cat_json_str_copy(const ccord_json *obj, const char *key, char *dst, size_t cap);
bool cat_hex_equal_ci(const char *a, const char *b);
bool cat_is_installing(const ccord_catalog *cat, const char *id);
installed_meta *cat_find_meta(ccord_catalog *cat, const char *id);
const installed_meta *cat_find_meta_const(const ccord_catalog *cat, const char *id);
installed_meta *cat_upsert_meta(ccord_catalog *cat, const char *id);

// Persistence (catalog_store.c)
void cat_save_prefs(const ccord_catalog *cat);
void cat_load_prefs(ccord_catalog *cat);
void cat_save_installed_meta(const ccord_catalog *cat);
void cat_load_installed_meta(ccord_catalog *cat);
void cat_save_revocations_cache(const ccord_catalog *cat);
bool cat_merge_revocations_json(ccord_catalog *cat, const char *json, size_t len, bool is_cache);
void cat_load_revocations_cache(ccord_catalog *cat);

// Parsers, overlays & publishing (catalog_parse.c)
bool cat_valid_catalog_id(const char *s);
bool cat_valid_catalog_version(const char *s);
bool cat_compute_update_info(const cat_item *it, const char *local_ver, uint64_t consented_perms,
                             bool *out_avail, bool *out_needs_review);
bool cat_parse_discover_page_json(ccord_catalog *cat, const char *json, size_t len, bool append);
bool cat_parse_detail_json(const char *json, size_t len, cat_item *out);
bool cat_parse_updates_check_json(ccord_catalog *cat, const char *json, size_t len,
                                  bool *out_rev_stale, int64_t *out_rev_seq);
const cat_item *cat_find_update_item(const ccord_catalog *cat, const char *id);
const cat_item *cat_find_best_item(const ccord_catalog *cat, const char *id);
void cat_publish_catalog(ccord_catalog *cat);
void cat_enforce_revocations(ccord_catalog *cat);
