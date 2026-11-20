// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L

#include "catalog.h"
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
  const char *discover_p1_json;
  const char *discover_p2_json;
  const char *detail_json;
  const char *updates_json;
  const char *revocations_json;
  const char *download_bytes;
  size_t download_len;
  int get_calls;
  int post_calls;
  int dl_calls;
  char last_get_url[512];
  char last_post_url[512];
  char last_post_body[512];
} fake_http_state;

static bool fake_get(void *ud, const char *url, const char *if_none_match, size_t max_bytes,
                     char **out_body, size_t *out_len, char *out_etag, size_t etag_cap,
                     bool *out_not_modified, char *err_buf, size_t err_cap)
{
  (void)if_none_match; (void)max_bytes; (void)err_buf; (void)err_cap;
  fake_http_state *st = ud;
  st->get_calls++;
  snprintf(st->last_get_url, sizeof st->last_get_url, "%s", url);
  if (out_not_modified) *out_not_modified = false;
  if (out_etag && etag_cap) snprintf(out_etag, etag_cap, "\"etag-v2\"");

  const char *resp = "{}";
  if (strstr(url, "/api/v2/revocations")) {
    resp = st->revocations_json ? st->revocations_json : "{\"schema\":2,\"seq\":0,\"full\":true,\"revoked\":[]}";
  } else if (strstr(url, "/api/v2/plugins/")) {
    resp = st->detail_json ? st->detail_json : "{}";
  } else if (strstr(url, "/api/v2/plugins")) {
    resp = (strstr(url, "cursor=cur-p2") && st->discover_p2_json) ? st->discover_p2_json : st->discover_p1_json;
  }

  size_t n = strlen(resp);
  char *copy = ccord_malloc(n + 1);
  memcpy(copy, resp, n + 1);
  *out_body = copy;
  *out_len = n;
  return true;
}

static bool fake_post(void *ud, const char *url, const char *json_body, size_t body_len, size_t max_bytes,
                      char **out_body, size_t *out_len, char *err_buf, size_t err_cap)
{
  (void)max_bytes; (void)err_buf; (void)err_cap;
  fake_http_state *st = ud;
  st->post_calls++;
  snprintf(st->last_post_url, sizeof st->last_post_url, "%s", url);
  size_t clen = body_len < sizeof st->last_post_body - 1 ? body_len : sizeof st->last_post_body - 1;
  memcpy(st->last_post_body, json_body, clen);
  st->last_post_body[clen] = '\0';

  const char *resp = st->updates_json ? st->updates_json : "{\"schema\":2,\"rev_seq\":0,\"revocations_stale\":false,\"plugins\":[]}";
  size_t n = strlen(resp);
  char *copy = ccord_malloc(n + 1);
  memcpy(copy, resp, n + 1);
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

static size_t host_list_local(void *ud, char (*ids_out)[65], size_t cap)
{
  fake_host *h = ud;
  if (!h->installed_ver[0] || cap == 0) return 0;
  snprintf(ids_out[0], 65, "org.ccord.demo");
  return 1;
}

static bool host_import(void *ud, const char *pkg_path, const char *expected_id,
                        const char *expected_version, uint64_t expected_perms,
                        char *err_buf, size_t err_cap)
{
  (void)pkg_path; (void)expected_id; (void)expected_perms; (void)err_buf; (void)err_cap;
  fake_host *h = ud;
  h->import_calls++;
  snprintf(h->installed_ver, sizeof h->installed_ver, "%s",
           (expected_version && expected_version[0]) ? expected_version : "1.2.0");
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

static bool pred_discover_loaded(void *ud)
{
  ccord_contrib *c = ud;
  const ccord_catalog_status *st = ccord_contrib_catalog_status(c);
  return st && st->has_loaded && !st->syncing && !st->loading_more;
}

static bool pred_discover_two(void *ud)
{
  ccord_contrib *c = ud;
  const ccord_catalog_status *st = ccord_contrib_catalog_status(c);
  return st && !st->loading_more && ccord_contrib_catalog_count(c) == 2;
}

static bool pred_updates_ready(void *ud)
{
  ccord_contrib *c = ud;
  return ccord_contrib_catalog_update_count(c) == 1;
}

static bool pred_detail_ready(void *ud)
{
  ccord_contrib *c = ud;
  const ccord_catalog_status *st = ccord_contrib_catalog_status(c);
  const ccord_catalog_entry *d = ccord_contrib_catalog_detail(c);
  return st && !st->detail_loading && d != nullptr;
}

static bool pred_not_installing(void *ud)
{
  ccord_contrib *c = ud;
  const ccord_catalog_entry *e = ccord_contrib_catalog_find(c, "org.ccord.demo");
  return e && !e->installing;
}

static bool pred_revoked(void *ud)
{
  fake_host *h = ud;
  return h->revoke_calls > 0;
}

static void test_semver(void)
{
  CT_CHECK(ccord_catalog_semver_cmp("1.2.0", "1.10.0") < 0);
  CT_CHECK(ccord_catalog_semver_cmp("2.0.0", "1.9.9") > 0);
  CT_CHECK(ccord_catalog_semver_cmp("1.0.0", "1.0.0") == 0);
}

static void test_zero_installed_makes_zero_startup_calls(void)
{
  char tmpl[] = "/tmp/ccord_cat_zero_XXXXXX";
  char *dir = mkdtemp(tmpl);
  CT_CHECK(dir != nullptr);
  if (!dir) return;

  ccord_error err = {0};
  ccord_contrib *contrib = ccord_contrib_create(&err);
  fake_http_state http_st = {0};
  ccord_catalog_http http = {.ud = &http_st, .get = fake_get, .post_json = fake_post, .download_file = fake_dl};
  fake_host host = {0}; // 0 plugins installed
  ccord_catalog_host_cbs cbs = {
    .ud = &host, .get_local_plugin = host_get_local, .list_local_plugins = host_list_local,
    .import_verified_cce = host_import, .revoke_plugin = host_revoke,
  };

  ccord_catalog *cat = ccord_catalog_create(dir, contrib, &http, &cbs);
  CT_CHECK(cat != nullptr);
  // Pump well past startup delay -> 0 HTTP requests!
  ccord_catalog_pump(cat, 100);
  ccord_catalog_pump(cat, 5000);
  ccord_catalog_pump(cat, 6000);
  CT_CHECK(http_st.get_calls == 0);
  CT_CHECK(http_st.post_calls == 0);

  ccord_catalog_destroy(cat);
  ccord_contrib_destroy(contrib);
}

static void test_v2_discover_updates_sha_and_revocations(void)
{
  char tmpl[] = "/tmp/ccord_cat_v2_XXXXXX";
  char *dir = mkdtemp(tmpl);
  CT_CHECK(dir != nullptr);
  if (!dir) return;

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

  const char *disc_p1 =
    "{\"total\":2,\"limit\":1,\"has_more\":true,\"next_cursor\":\"cur-p2\",\"plugins\":[{"
    "\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Demo plugin\",\"license\":\"MIT\","
    "\"tier\":2,\"author_login\":\"alice\",\"author_repo\":\"alice/demo\",\"author_repo_url\":\"https://github.com/alice/demo\","
    "\"version\":\"1.2.0\",\"api_class\":3,\"size_bytes\":25,\"downloads\":42,\"warning_sev\":2,\"warning_title\":\"Note\"}]}";
  const char *disc_p2 =
    "{\"total\":2,\"limit\":1,\"has_more\":false,\"next_cursor\":\"\",\"plugins\":[{"
    "\"id\":\"org.ccord.two\",\"name\":\"Second\",\"description\":\"Second plugin\",\"license\":\"MIT\","
    "\"tier\":1,\"author_login\":\"C-Cord\",\"author_repo\":\"C-Cord/two\",\"author_repo_url\":\"https://github.com/C-Cord/two\","
    "\"version\":\"1.0.0\",\"api_class\":1,\"size_bytes\":10,\"downloads\":7,\"warning_sev\":0}]}";

  char detail_json[2048];
  snprintf(detail_json, sizeof detail_json,
           "{\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Demo plugin\",\"license\":\"MIT\","
           "\"api\":1,\"tier\":2,\"downloads\":42,\"metadata_rev\":3,"
           "\"author\":{\"login\":\"alice\",\"repo\":\"alice/demo\",\"repo_url\":\"https://github.com/alice/demo\"},"
           "\"latest\":{\"version\":\"1.2.0\",\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.demo\","
           "\"sha256\":\"%s\",\"size_bytes\":25,\"permissions\":[\"fs.read\"],\"api_class\":3,\"changelog\":\"Added fs.read\"},"
           "\"warnings\":[{\"severity\":\"caution\",\"title\":\"Note\",\"message\":\"Reads disk\"}]}",
           good_sha);

  char updates_json[2048];
  snprintf(updates_json, sizeof updates_json,
           "{\"schema\":2,\"rev_seq\":0,\"revocations_stale\":false,\"plugins\":[{"
           "\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Demo plugin\",\"license\":\"MIT\","
           "\"tier\":2,\"author_login\":\"alice\",\"author_repo\":\"alice/demo\",\"author_repo_url\":\"https://github.com/alice/demo\","
           "\"version\":\"1.2.0\",\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.demo\","
           "\"sha256\":\"%s\",\"size_bytes\":25,\"permissions\":[\"fs.read\"],\"api_class\":3,\"changelog\":\"Added fs.read\","
           "\"warnings\":[{\"severity\":\"caution\",\"title\":\"Note\",\"message\":\"Reads disk\"}]}]}",
           good_sha);

  ccord_error err = {0};
  ccord_contrib *contrib = ccord_contrib_create(&err);
  CT_CHECK(contrib != nullptr);

  fake_http_state http_st = {
    .discover_p1_json = disc_p1,
    .discover_p2_json = disc_p2,
    .detail_json = detail_json,
    .updates_json = updates_json,
    .download_bytes = "TAMPERED",
    .download_len = 8,
  };
  ccord_catalog_http http = {.ud = &http_st, .get = fake_get, .post_json = fake_post, .download_file = fake_dl};
  fake_host host = {.installed_ver = "1.1.0", .consented_perms = 0};
  ccord_catalog_host_cbs cbs = {
    .ud = &host, .get_local_plugin = host_get_local, .list_local_plugins = host_list_local,
    .import_verified_cce = host_import, .revoke_plugin = host_revoke,
  };

  ccord_catalog *cat = ccord_catalog_create(dir, contrib, &http, &cbs);
  CT_CHECK(cat != nullptr);

  // 1. Targeted update check POSTs ONLY installed IDs
  ccord_catalog_check_updates(cat);
  pump_until(cat, 2000, pred_updates_ready, contrib);
  CT_CHECK(http_st.post_calls == 1);
  CT_CHECK(strstr(http_st.last_post_url, "/api/v2/updates/check") != nullptr);
  CT_CHECK(strstr(http_st.last_post_body, "org.ccord.demo") != nullptr);
  CT_CHECK(ccord_contrib_catalog_update_count(contrib) == 1);

  const ccord_catalog_entry *u = ccord_contrib_catalog_update_at(contrib, 0);
  CT_CHECK(u != nullptr);
  if (u) {
    CT_CHECK(u->tier == CCORD_TIER_VERIFIED);
    CT_CHECK(u->api_class == 3);
    CT_CHECK(u->warning_sev == CCORD_WARN_CAUTION);
    CT_CHECK(u->update_available);
    CT_CHECK(u->update_needs_review); // new fs.read perm beyond consented_perms == 0 -> pauses auto-update!
  }

  // Auto-update must NOT install because update_needs_review is true
  ccord_catalog_prefs prefs = {.auto_update_enabled = true, .auto_update_min_tier = 2};
  ccord_catalog_set_prefs(cat, &prefs);
  ccord_catalog_run_auto_updates(cat);
  CT_CHECK(http_st.dl_calls == 0);

  // 2. Paginated Discover query + Load More cursor pagination
  ccord_catalog_query(cat, "demo", 0, 0, "popular", false);
  pump_until(cat, 2000, pred_discover_loaded, contrib);
  CT_CHECK(ccord_contrib_catalog_count(contrib) == 1);
  CT_CHECK(ccord_contrib_catalog_status(contrib)->has_more);
  CT_CHECK(ccord_contrib_catalog_status(contrib)->total_count == 2);

  ccord_catalog_query(cat, "demo", 0, 0, "popular", true);
  pump_until(cat, 2000, pred_discover_two, contrib);
  CT_CHECK(ccord_contrib_catalog_count(contrib) == 2);
  CT_CHECK(!ccord_contrib_catalog_status(contrib)->has_more);

  // 3. On-demand detail lookup (GET /api/v2/plugins/{id})
  ccord_catalog_fetch_detail(cat, "org.ccord.demo");
  pump_until(cat, 2000, pred_detail_ready, contrib);
  const ccord_catalog_entry *det = ccord_contrib_catalog_detail(contrib);
  CT_CHECK(det != nullptr && det->has_detail && strcmp(det->sha256, good_sha) == 0);

  // 4. Manual install with tampered bytes -> SHA-256 mismatch aborts before import!
  ccord_catalog_install(cat, "org.ccord.demo");
  pump_until(cat, 2000, pred_not_installing, contrib);
  CT_CHECK(http_st.dl_calls == 1);
  CT_CHECK(host.import_calls == 0);

  // 5. Manual install with authentic bytes -> SHA-256 passes and imports!
  http_st.download_bytes = payload;
  http_st.download_len = sizeof payload - 1;
  ccord_catalog_install(cat, "org.ccord.demo");
  pump_until(cat, 2000, pred_not_installing, contrib);
  CT_CHECK(http_st.dl_calls == 2);
  CT_CHECK(host.import_calls == 1);

  // 6. Revocation delta trigger: updates/check reports rev_seq=1 stale -> fetches /api/v2/revocations -> revokes plugin!
  char rev_json[512];
  snprintf(rev_json, sizeof rev_json,
           "{\"schema\":2,\"seq\":1,\"since\":0,\"full\":true,\"revoked\":[{\"seq\":1,\"sha256\":\"%s\",\"plugin_id\":\"org.ccord.demo\",\"reason\":\"malware\"}]}",
           good_sha);
  http_st.updates_json = "{\"schema\":2,\"rev_seq\":1,\"revocations_stale\":true,\"plugins\":[]}";
  http_st.revocations_json = rev_json;
  ccord_catalog_check_updates(cat);
  pump_until(cat, 2000, pred_revoked, &host);
  CT_CHECK(host.revoke_calls == 1);
  CT_CHECK(strcmp(host.revoked_id, "org.ccord.demo") == 0);
  // 7. Persisted revocations survive restart and enforce automatically on startup (P11).
  CT_CHECK(ccord_catalog_is_revoked(cat, "org.ccord.demo", nullptr, 0));
  ccord_catalog_destroy(cat);
  host.revoke_calls = 0;
  host.revoked_id[0] = '\0';
  cat = ccord_catalog_create(dir, contrib, &http, &cbs);
  CT_CHECK(cat != nullptr);
  CT_CHECK(ccord_catalog_is_revoked(cat, "org.ccord.demo", nullptr, 0));
  CT_CHECK(host.revoke_calls == 1 && strcmp(host.revoked_id, "org.ccord.demo") == 0);
  ccord_catalog_enforce_revocations(cat);
  CT_CHECK(host.revoke_calls == 2 && strcmp(host.revoked_id, "org.ccord.demo") == 0);

  ccord_catalog_destroy(cat);
  ccord_contrib_destroy(contrib);
}

int main(void)
{
  test_semver();
  test_zero_installed_makes_zero_startup_calls();
  test_v2_discover_updates_sha_and_revocations();
  return CT_RESULT("test_plugin_catalog");
}
