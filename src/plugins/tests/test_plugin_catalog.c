// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L

#include "../catalog.h"
#include <ccord/core/alloc.h>
#include <ccord/core/error.h>
#include <ccord/ipc/perm.h>
#include <ccord/model/contrib.h>

#include "ccord_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct fake_http_state {
  const char *catalog_json;
  const char *download_bytes;
  size_t download_len;
  int get_calls;
  int dl_calls;
} fake_http_state;

static bool fake_get(void *ud, const char *url, const char *if_none_match, size_t max_bytes,
                     char **out_body, size_t *out_len, char *out_etag, size_t etag_cap,
                     bool *out_not_modified, char *err_buf, size_t err_cap)
{
  (void)url; (void)if_none_match; (void)max_bytes; (void)err_buf; (void)err_cap;
  fake_http_state *st = ud;
  st->get_calls++;
  if (out_not_modified) *out_not_modified = false;
  if (out_etag && etag_cap) snprintf(out_etag, etag_cap, "\"etag-v1\"");
  size_t n = strlen(st->catalog_json);
  char *copy = ccord_malloc(n + 1);
  memcpy(copy, st->catalog_json, n + 1);
  *out_body = copy;
  *out_len = n;
  return true;
}

static bool fake_dl(void *ud, const char *url, const char *dest_path, size_t max_bytes,
                    char *err_buf, size_t err_cap)
{
  (void)url; (void)max_bytes;
  fake_http_state *st = ud;
  st->dl_calls++;
  FILE *fp = fopen(dest_path, "wb");
  if (!fp) { snprintf(err_buf, err_cap, "fopen failed"); return false; }
  fwrite(st->download_bytes, 1, st->download_len, fp);
  fclose(fp);
  return true;
}

typedef struct fake_host {
  char installed_ver[33];
  uint64_t consented_perms;
  int import_calls;
  int revoke_calls;
  char revoked_id[65];
} fake_host;

static bool host_get_local(void *ud, const char *id, char *ver_out, size_t ver_cap,
                           uint64_t *consented_perms_out, bool *is_bundled_out)
{
  fake_host *h = ud;
  if (strcmp(id, "org.ccord.demo") != 0 || !h->installed_ver[0]) return false;
  snprintf(ver_out, ver_cap, "%s", h->installed_ver);
  if (consented_perms_out) *consented_perms_out = h->consented_perms;
  if (is_bundled_out) *is_bundled_out = false;
  return true;
}

static bool host_import(void *ud, const char *pkg_path, char *err_buf, size_t err_cap)
{
  (void)pkg_path; (void)err_buf; (void)err_cap;
  fake_host *h = ud;
  h->import_calls++;
  snprintf(h->installed_ver, sizeof h->installed_ver, "1.2.0");
  return true;
}

static void host_revoke(void *ud, const char *id, const char *reason)
{
  (void)reason;
  fake_host *h = ud;
  h->revoke_calls++;
  snprintf(h->revoked_id, sizeof h->revoked_id, "%s", id);
}

static void pump_until(ccord_catalog *cat, int ms_budget, bool (*pred)(void *), void *ud)
{
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
  uint64_t now = 5000;
  for (int waited = 0; waited < ms_budget; waited += 5) {
    ccord_catalog_pump(cat, now);
    if (pred(ud)) return;
    nanosleep(&ts, nullptr);
    now += 5;
  }
}

static bool pred_loaded(void *ud)
{
  ccord_contrib *c = ud;
  const ccord_catalog_status *st = ccord_contrib_catalog_status(c);
  return st && st->has_loaded && !st->syncing;
}

static bool pred_not_installing(void *ud)
{
  ccord_contrib *c = ud;
  const ccord_catalog_entry *e = ccord_contrib_catalog_find(c, "org.ccord.demo");
  return e && !e->installing;
}

static void test_semver(void)
{
  CT_CHECK(ccord_catalog_semver_cmp("1.2.0", "1.10.0") < 0);
  CT_CHECK(ccord_catalog_semver_cmp("2.0.0", "1.9.9") > 0);
  CT_CHECK(ccord_catalog_semver_cmp("1.0.0", "1.0.0") == 0);
}

static void test_sha_and_escalation_guard(void)
{
  char tmpl[] = "/tmp/ccord_cat_test_XXXXXX";
  char *dir = mkdtemp(tmpl);
  CT_CHECK(dir != nullptr);
  if (!dir) return;

  // Compute expected sha256 of our dummy .cce payload
  char sample_path[512];
  snprintf(sample_path, sizeof sample_path, "%s/sample.cce", dir);
  const char payload[] = "PK_FAKE_CCE_PAYLOAD_12345";
  FILE *fp = fopen(sample_path, "wb");
  CT_CHECK(fp != nullptr);
  if (!fp) return;
  fwrite(payload, 1, sizeof payload - 1, fp);
  fclose(fp);
  char good_sha[65];
  CT_CHECK(ccord_catalog_sha256_file_hex(sample_path, good_sha));
  unlink(sample_path);

  char cat_json[2048];
  snprintf(cat_json, sizeof cat_json,
           "{\"schema\":1,\"revoked_hashes\":[],\"plugins\":[{"
           "\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Demo plugin\",\"license\":\"MIT\","
           "\"api\":1,\"tier\":2,\"author\":{\"login\":\"alice\",\"repo\":\"alice/demo\",\"repo_url\":\"https://github.com/alice/demo\"},"
           "\"latest\":{\"version\":\"1.2.0\",\"download_url\":\"https://github.com/alice/demo/releases/download/v1.2.0/demo.cce\","
           "\"sha256\":\"%s\",\"size_bytes\":25,\"permissions\":[\"fs.read\"],\"api_class\":3,\"changelog\":\"Added fs.read\"},"
           "\"warnings\":[{\"severity\":\"caution\",\"title\":\"Note\",\"message\":\"Reads disk\"}]}]}",
           good_sha);

  ccord_error err = {0};
  ccord_contrib *contrib = ccord_contrib_create(&err);
  CT_CHECK(contrib != nullptr);

  fake_http_state http_st = {.catalog_json = cat_json, .download_bytes = "TAMPERED", .download_len = 8};
  ccord_catalog_http http = {.ud = &http_st, .get = fake_get, .download_file = fake_dl};
  fake_host host = {.installed_ver = "1.1.0", .consented_perms = 0};
  ccord_catalog_host_cbs cbs = {.ud = &host, .get_local_plugin = host_get_local, .import_verified_cce = host_import, .revoke_plugin = host_revoke};

  ccord_catalog *cat = ccord_catalog_create(dir, contrib, &http, &cbs);
  CT_CHECK(cat != nullptr);
  ccord_catalog_refresh(cat);
  pump_until(cat, 2000, pred_loaded, contrib);

  CT_CHECK(ccord_contrib_catalog_count(contrib) == 1);
  const ccord_catalog_entry *e = ccord_contrib_catalog_find(contrib, "org.ccord.demo");
  CT_CHECK(e != nullptr);
  if (e) {
    CT_CHECK(e->tier == CCORD_TIER_VERIFIED);
    CT_CHECK(e->api_class == 3);
    CT_CHECK(e->warning_sev == CCORD_WARN_CAUTION);
    CT_CHECK(e->update_available);
    // New fs.read perm beyond consented_perms == 0 -> must pause auto-update!
    CT_CHECK(e->update_needs_review);
  }

  // Enable auto-updates: must NOT auto-install because update_needs_review is true!
  ccord_catalog_prefs prefs = {.auto_update_enabled = true, .auto_update_min_tier = 2};
  ccord_catalog_set_prefs(cat, &prefs);
  ccord_catalog_run_auto_updates(cat);
  CT_CHECK(http_st.dl_calls == 0);

  // Manual install with tampered download bytes -> SHA-256 mismatch aborts before import!
  ccord_catalog_install(cat, "org.ccord.demo");
  pump_until(cat, 2000, pred_not_installing, contrib);
  CT_CHECK(http_st.dl_calls == 1);
  CT_CHECK(host.import_calls == 0);

  // Manual install with authentic bytes -> SHA-256 passes and imports!
  http_st.download_bytes = payload;
  http_st.download_len = sizeof payload - 1;
  ccord_catalog_install(cat, "org.ccord.demo");
  pump_until(cat, 2000, pred_not_installing, contrib);
  CT_CHECK(http_st.dl_calls == 2);
  CT_CHECK(host.import_calls == 1);

  // Revocation check: refresh with good_sha in revoked_hashes -> revokes plugin!
  char rev_json[512];
  snprintf(rev_json, sizeof rev_json,
           "{\"schema\":1,\"revoked_hashes\":[{\"sha256\":\"%s\",\"plugin_id\":\"org.ccord.demo\",\"reason\":\"malware\"}],\"plugins\":[]}",
           good_sha);
  http_st.catalog_json = rev_json;
  ccord_catalog_refresh(cat);
  pump_until(cat, 2000, pred_loaded, contrib);
  CT_CHECK(host.revoke_calls == 1);
  CT_CHECK(strcmp(host.revoked_id, "org.ccord.demo") == 0);

  ccord_catalog_destroy(cat);
  ccord_contrib_destroy(contrib);
}

int main(void)
{
  test_semver();
  test_sha_and_escalation_guard();
  return CT_RESULT("test_plugin_catalog");
}
