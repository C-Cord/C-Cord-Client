// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L

#include "catalog.h"
#include <ccord/core/alloc.h>
#include <ccord/core/error.h>
#include <ccord/core/sha256.h>
#include <ccord/ipc/perm.h>
#include <ccord/model/contrib.h>
#include <ccord/plugins/manifest.h>

#include "ccord_test.h"

#if defined(CCORD_HAVE_SODIUM) && CCORD_HAVE_SODIUM
#include <sodium.h>
#endif

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
  char consented_hosts[256];
  bool had_consented_hosts;
  int import_calls;
  int import_approved_calls;
  bool last_preapproved;
  uint64_t last_expected_perms;
  int revoke_calls;
  char revoked_id[65];
  char revoked_ids[8][65];
  size_t nrevoked_ids;
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

static bool host_get_allowed_hosts(void *ud, const char *id, char *hosts_out, size_t hosts_cap,
                                   bool *had_consented_out)
{
  fake_host *h = ud;
  if (strcmp(id, "org.ccord.demo") != 0 || !h->installed_ver[0]) return false;
  if (hosts_out && hosts_cap) snprintf(hosts_out, hosts_cap, "%s", h->consented_hosts);
  if (had_consented_out) *had_consented_out = h->had_consented_hosts;
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
  (void)pkg_path; (void)expected_id; (void)err_buf; (void)err_cap;
  fake_host *h = ud;
  h->import_calls++;
  h->last_preapproved = false;
  h->last_expected_perms = expected_perms;
  snprintf(h->installed_ver, sizeof h->installed_ver, "%s",
           (expected_version && expected_version[0]) ? expected_version : "1.2.0");
  return true;
}

static bool host_import_approved(void *ud, const char *pkg_path, const char *expected_id,
                                 const char *expected_version, uint64_t expected_perms,
                                 bool preapprove_consent, char *err_buf, size_t err_cap)
{
  (void)pkg_path; (void)expected_id; (void)err_buf; (void)err_cap;
  fake_host *h = ud;
  h->import_approved_calls++;
  h->last_preapproved = preapprove_consent;
  h->last_expected_perms = expected_perms;
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
  if (h->nrevoked_ids < 8) {
    snprintf(h->revoked_ids[h->nrevoked_ids++], sizeof h->revoked_ids[0], "%s", id);
  }
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

static bool pred_icon_cached(void *ud)
{
  const ccord_contrib *c = ud;
  const ccord_catalog_entry *e = ccord_contrib_catalog_at(c, 0);
  return e && e->icon_path && e->icon_path[0] != '\0';
}

static void test_manifest_icon_and_catalog_v2_fields(void)
{
  // 1. Manifest "icon" validation: valid PNG/JPEG/WebP paths accepted; traversal & unsupported extensions rejected.
  ccord_manifest m = {0};
  ccord_error err = {0};
  const char *good_png = "{\"schema\":1,\"id\":\"io.test.icon\",\"name\":\"Icon\",\"version\":\"1.0.0\",\"api\":1,\"icon\":\"assets/icon.png\"}";
  CT_CHECK(ccord_manifest_parse(good_png, strlen(good_png), &m, &err) == CCORD_OK);
  CT_CHECK(strcmp(m.icon, "assets/icon.png") == 0);

  const char *good_webp = "{\"schema\":1,\"id\":\"io.test.icon\",\"name\":\"Icon\",\"version\":\"1.0.0\",\"api\":1,\"icon\":\"logo.WEBP\"}";
  CT_CHECK(ccord_manifest_parse(good_webp, strlen(good_webp), &m, &err) == CCORD_OK);
  CT_CHECK(strcmp(m.icon, "logo.WEBP") == 0);

  const char *bad_icons[] = {
    "{\"schema\":1,\"id\":\"io.test.icon\",\"name\":\"Icon\",\"version\":\"1.0.0\",\"api\":1,\"icon\":\"../escape.png\"}",
    "{\"schema\":1,\"id\":\"io.test.icon\",\"name\":\"Icon\",\"version\":\"1.0.0\",\"api\":1,\"icon\":\"/abs.png\"}",
    "{\"schema\":1,\"id\":\"io.test.icon\",\"name\":\"Icon\",\"version\":\"1.0.0\",\"api\":1,\"icon\":\"icon.svg\"}",
    "{\"schema\":1,\"id\":\"io.test.icon\",\"name\":\"Icon\",\"version\":\"1.0.0\",\"api\":1,\"icon\":\"a//b.png\"}",
  };
  for (size_t i = 0; i < sizeof bad_icons / sizeof bad_icons[0]; i++) {
    CT_CHECK(ccord_manifest_parse(bad_icons[i], strlen(bad_icons[i]), &m, &err) != CCORD_OK);
  }

  // 2. Catalog API v2 icon_url + platforms + allowed_hosts + disk icon cache + preapproved install!
  char tmpl[] = "/tmp/ccord_cat_icon_XXXXXX";
  char *dir = mkdtemp(tmpl);
  CT_CHECK(dir != nullptr);
  if (!dir) return;
  const char payload[] = "AUTHENTIC-CCE-BYTES-V2";
  char tmp_cce[512];
  snprintf(tmp_cce, sizeof tmp_cce, "%s/ref.cce", dir);
  FILE *fp = fopen(tmp_cce, "wb");
  CT_CHECK(fp != nullptr);
  if (fp) { fwrite(payload, 1, sizeof payload - 1, fp); fclose(fp); }
  char good_sha[65] = "";
  CT_CHECK(ccord_catalog_sha256_file_hex(tmp_cce, good_sha));

  const char *disc =
      "{\"schema\":2,\"total\":1,\"has_more\":false,\"next_cursor\":\"\",\"plugins\":[{"
      "\"id\":\"org.ccord.demo\",\"name\":\"Demo Icon\",\"description\":\"Cross-platform plugin\","
      "\"version\":\"2.0.0\",\"tier\":1,\"api_class\":3,\"size_bytes\":22,\"downloads\":99,"
      "\"author_login\":\"C-Cord\",\"icon_url\":\"https://plugins.c-cord.org/icons/0123456789abcdef.png\","
      "\"platforms\":[\"desktop\",\"android\"]}]}";
  char det_json[1024];
  snprintf(det_json, sizeof det_json,
           "{\"schema\":1,\"id\":\"org.ccord.demo\",\"name\":\"Demo Icon\",\"description\":\"Cross-platform plugin\","
           "\"license\":\"MIT\",\"tier\":1,\"downloads\":99,"
           "\"icon_url\":\"https://plugins.c-cord.org/icons/0123456789abcdef.png\","
           "\"author\":{\"login\":\"C-Cord\",\"repo\":\"C-Cord/demo\",\"repo_url\":\"https://github.com/C-Cord/demo\"},"
           "\"latest\":{\"version\":\"2.0.0\",\"download_url\":\"https://github.com/C-Cord/demo/releases/download/v2.0.0/d.cce\","
           "\"sha256\":\"%s\",\"size_bytes\":22,\"permissions\":[\"network.http\"],\"allowed_hosts\":[\"api.example.com\"],"
           "\"platforms\":[\"desktop\",\"android\"],\"api_class\":3,\"changelog\":\"Icon support\"}}",
           good_sha);

  ccord_contrib *contrib = ccord_contrib_create(&err);
  CT_CHECK(contrib != nullptr);
  fake_http_state http_st = {
    .discover_p1_json = disc,
    .detail_json = det_json,
    .download_bytes = payload,
    .download_len = sizeof payload - 1,
  };
  ccord_catalog_http http = {.ud = &http_st, .get = fake_get, .post_json = fake_post, .download_file = fake_dl};
  fake_host host = {0};
  ccord_catalog_host_cbs cbs = {
    .ud = &host, .get_local_plugin = host_get_local, .list_local_plugins = host_list_local,
    .import_verified_cce = host_import, .import_verified_cce_approved = host_import_approved,
    .revoke_plugin = host_revoke,
  };
  ccord_catalog *cat = ccord_catalog_create(dir, contrib, &http, &cbs);
  CT_CHECK(cat != nullptr);

  // Query with platform=android -> verifies query param AND triggers 1-time icon cache download
  ccord_catalog_query_ex(cat, "", 0, 0, "android", "popular", false);
  pump_until(cat, 2000, pred_icon_cached, contrib);
  CT_CHECK(strstr(http_st.last_get_url, "platform=android") != nullptr);
  CT_CHECK(http_st.dl_calls == 1);
  const ccord_catalog_entry *e0 = ccord_contrib_catalog_at(contrib, 0);
  CT_CHECK(e0 != nullptr);
  if (e0) {
    CT_CHECK(e0->platforms == CCORD_CONTRIB_PLAT_ALL);
    CT_CHECK(strstr(e0->icon_path, "0123456789abcdef.png") != nullptr);
  }

  // Second query MUST NOT re-download the cached icon (0 extra dl_calls)!
  int dls_after_first = http_st.dl_calls;
  ccord_catalog_query_ex(cat, "", 0, 0, "desktop", "popular", false);
  pump_until(cat, 2000, pred_discover_loaded, contrib);
  CT_CHECK(http_st.dl_calls == dls_after_first);

  // Fetch detail -> verify allowed_hosts and perms_mask
  ccord_catalog_fetch_detail(cat, "org.ccord.demo");
  pump_until(cat, 2000, pred_detail_ready, contrib);
  const ccord_catalog_entry *det = ccord_contrib_catalog_detail(contrib);
  CT_CHECK(det != nullptr && det->has_detail);
  if (det) {
    CT_CHECK(strcmp(det->allowed_hosts, "api.example.com") == 0);
    CT_CHECK(det->perms_mask == (uint64_t)CCORD_PERM_NETWORK_HTTP);
  }

  // Atomic pre-approved install -> invokes import_verified_cce_approved with preapprove_consent == true
  ccord_catalog_install_approved(cat, "org.ccord.demo", (uint64_t)CCORD_PERM_NETWORK_HTTP);
  pump_until(cat, 2000, pred_not_installing, contrib);
  CT_CHECK(host.import_approved_calls == 1 && host.last_preapproved);
  CT_CHECK(host.last_expected_perms == (uint64_t)CCORD_PERM_NETWORK_HTTP);

  ccord_catalog_destroy(cat);
  ccord_contrib_destroy(contrib);
}

typedef struct upd_ver_wait {
  ccord_contrib *contrib;
  const char *ver;
} upd_ver_wait;

static bool pred_update_version(void *ud)
{
  const upd_ver_wait *w = ud;
  if (ccord_contrib_catalog_update_count(w->contrib) == 0) return false;
  const ccord_catalog_entry *u = ccord_contrib_catalog_update_at(w->contrib, 0);
  return u && u->version && strcmp(u->version, w->ver) == 0;
}

static bool pred_imported(void *ud)
{
  const fake_host *h = ud;
  return h->import_calls > 0;
}

static bool pred_revoked_two(void *ud)
{
  const fake_host *h = ud;
  return h->revoke_calls >= 2;
}

static void test_allowed_hosts_expansion_and_tier_review_block_auto_update(void)
{
  // 1. Direct unit checks of ccord_hosts_expanded()
  uint64_t net_http = (uint64_t)CCORD_PERM_NETWORK_HTTP;
  uint64_t net_ws = (uint64_t)CCORD_PERM_NETWORK_WS;
  CT_CHECK(!ccord_hosts_expanded("api.example.com", true, "API.EXAMPLE.COM", net_http));
  CT_CHECK(!ccord_hosts_expanded("api.example.com, cdn.example.com", true, "api.example.com", net_http));
  CT_CHECK(ccord_hosts_expanded("api.example.com", true, "api.example.com, evil.example", net_http));
  CT_CHECK(ccord_hosts_expanded("api.example.com", true, "", net_http));
  CT_CHECK(ccord_hosts_expanded("api.example.com", true, "", net_ws));
  CT_CHECK(!ccord_hosts_expanded("", true, "api.example.com", net_http));
  CT_CHECK(!ccord_hosts_expanded("api.example.com", false, "evil.example", net_http));
  CT_CHECK(!ccord_hosts_expanded("api.example.com", true, "evil.example", (uint64_t)CCORD_PERM_FS_READ));

  // 2. End-to-end auto-update gating on allowed_hosts expansion & needs_tier_review
  char tmpl[] = "/tmp/ccord_cat_hosts_XXXXXX";
  char *dir = mkdtemp(tmpl);
  CT_CHECK(dir != nullptr);
  if (!dir) return;

  const char payload[] = "AUTO-UPDATE-HOST-TEST-BYTES";
  char tmp_cce[512];
  snprintf(tmp_cce, sizeof tmp_cce, "%s/ref.cce", dir);
  FILE *fp = fopen(tmp_cce, "wb");
  CT_CHECK(fp != nullptr);
  if (fp) { fwrite(payload, 1, sizeof payload - 1, fp); fclose(fp); }
  char good_sha[65] = "";
  CT_CHECK(ccord_catalog_sha256_file_hex(tmp_cce, good_sha));
  unlink(tmp_cce);

  char upd_added_host[2048], upd_unrestricted[2048], upd_tier_review[2048], upd_safe[2048];
  snprintf(upd_added_host, sizeof upd_added_host,
           "{\"schema\":2,\"rev_seq\":0,\"revocations_stale\":false,\"plugins\":[{"
           "\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Demo\",\"license\":\"MIT\","
           "\"tier\":2,\"author_login\":\"alice\",\"version\":\"1.1.0\","
           "\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.demo\","
           "\"sha256\":\"%s\",\"size_bytes\":27,\"permissions\":[\"network.http\"],"
           "\"allowed_hosts\":[\"api.example.com\",\"evil.example\"],\"api_class\":3}]}",
           good_sha);
  snprintf(upd_unrestricted, sizeof upd_unrestricted,
           "{\"schema\":2,\"rev_seq\":0,\"revocations_stale\":false,\"plugins\":[{"
           "\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Demo\",\"license\":\"MIT\","
           "\"tier\":2,\"author_login\":\"alice\",\"version\":\"1.2.0\","
           "\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.demo\","
           "\"sha256\":\"%s\",\"size_bytes\":27,\"permissions\":[\"network.http\"],"
           "\"allowed_hosts\":[],\"api_class\":3}]}",
           good_sha);
  snprintf(upd_tier_review, sizeof upd_tier_review,
           "{\"schema\":2,\"rev_seq\":0,\"revocations_stale\":false,\"plugins\":[{"
           "\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Demo\",\"license\":\"MIT\","
           "\"tier\":2,\"author_login\":\"alice\",\"version\":\"1.3.0\","
           "\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.demo\","
           "\"sha256\":\"%s\",\"size_bytes\":27,\"permissions\":[\"network.http\"],"
           "\"allowed_hosts\":[\"api.example.com\"],\"needs_tier_review\":true,\"api_class\":3}]}",
           good_sha);
  snprintf(upd_safe, sizeof upd_safe,
           "{\"schema\":2,\"rev_seq\":0,\"revocations_stale\":false,\"plugins\":[{"
           "\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Demo\",\"license\":\"MIT\","
           "\"tier\":2,\"author_login\":\"alice\",\"version\":\"1.4.0\","
           "\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.demo\","
           "\"sha256\":\"%s\",\"size_bytes\":27,\"permissions\":[\"network.http\"],"
           "\"allowed_hosts\":[\"api.example.com\"],\"needs_tier_review\":false,\"api_class\":3}]}",
           good_sha);

  ccord_error err = {0};
  ccord_contrib *contrib = ccord_contrib_create(&err);
  CT_CHECK(contrib != nullptr);
  fake_http_state http_st = {
    .updates_json = upd_added_host,
    .download_bytes = payload,
    .download_len = sizeof payload - 1,
  };
  ccord_catalog_http http = {.ud = &http_st, .get = fake_get, .post_json = fake_post, .download_file = fake_dl};
  fake_host host = {
    .installed_ver = "1.0.0",
    .consented_perms = net_http,
    .consented_hosts = "api.example.com",
    .had_consented_hosts = true,
  };
  ccord_catalog_host_cbs cbs = {
    .ud = &host, .get_local_plugin = host_get_local, .get_local_allowed_hosts = host_get_allowed_hosts,
    .list_local_plugins = host_list_local, .import_verified_cce = host_import, .revoke_plugin = host_revoke,
  };

  ccord_catalog *cat = ccord_catalog_create(dir, contrib, &http, &cbs);
  CT_CHECK(cat != nullptr);
  ccord_catalog_prefs prefs = {.auto_update_enabled = true, .auto_update_min_tier = 2};
  ccord_catalog_set_prefs(cat, &prefs);

  // 2a. Added host -> update_needs_review == true and auto-update blocked
  ccord_catalog_check_updates(cat);
  upd_ver_wait w1 = {.contrib = contrib, .ver = "1.1.0"};
  pump_until(cat, 2000, pred_update_version, &w1);
  const ccord_catalog_entry *u = ccord_contrib_catalog_update_at(contrib, 0);
  CT_CHECK(u != nullptr && u->update_available && u->update_needs_review);
  ccord_catalog_run_auto_updates(cat);
  CT_CHECK(http_st.dl_calls == 0);

  // 2b. Removing allowed_hosts (unrestricted) -> update_needs_review == true and auto-update blocked
  http_st.updates_json = upd_unrestricted;
  ccord_catalog_check_updates(cat);
  upd_ver_wait w2 = {.contrib = contrib, .ver = "1.2.0"};
  pump_until(cat, 2000, pred_update_version, &w2);
  u = ccord_contrib_catalog_update_at(contrib, 0);
  CT_CHECK(u != nullptr && u->update_available && u->update_needs_review);
  ccord_catalog_run_auto_updates(cat);
  CT_CHECK(http_st.dl_calls == 0);

  // 2c. needs_tier_review == true -> update_needs_review == true and auto-update blocked
  http_st.updates_json = upd_tier_review;
  ccord_catalog_check_updates(cat);
  upd_ver_wait w3 = {.contrib = contrib, .ver = "1.3.0"};
  pump_until(cat, 2000, pred_update_version, &w3);
  u = ccord_contrib_catalog_update_at(contrib, 0);
  CT_CHECK(u != nullptr && u->update_available && u->update_needs_review);
  ccord_catalog_run_auto_updates(cat);
  CT_CHECK(http_st.dl_calls == 0);

  // 2d. Unchanged allowed_hosts & needs_tier_review == false -> auto-update proceeds!
  http_st.updates_json = upd_safe;
  ccord_catalog_check_updates(cat);
  pump_until(cat, 2000, pred_imported, &host);
  CT_CHECK(http_st.dl_calls == 1);
  CT_CHECK(host.import_calls == 1);

  ccord_catalog_destroy(cat);
  ccord_contrib_destroy(contrib);
}

static void test_id_level_revocation_and_sideload_hash_tracking(void)
{
  char tmpl[] = "/tmp/ccord_cat_revid_XXXXXX";
  char *dir = mkdtemp(tmpl);
  CT_CHECK(dir != nullptr);
  if (!dir) return;

  ccord_error err = {0};
  ccord_contrib *contrib = ccord_contrib_create(&err);
  CT_CHECK(contrib != nullptr);

  // 1. Two distinct ID-only revocations (empty sha256) must BOTH be retained and trigger revoke_plugin
  const char *rev_ids_json =
    "{\"schema\":2,\"seq\":5,\"since\":0,\"full\":true,\"revoked\":["
    "{\"seq\":4,\"sha256\":\"\",\"plugin_id\":\"org.ccord.bad\",\"reason\":\"impersonation\"},"
    "{\"seq\":5,\"sha256\":\"\",\"plugin_id\":\"org.ccord.bad2\",\"reason\":\"malware author\"}]}";

  fake_http_state http_st = {
    .updates_json = "{\"schema\":2,\"rev_seq\":5,\"revocations_stale\":true,\"plugins\":[]}",
    .revocations_json = rev_ids_json,
  };
  ccord_catalog_http http = {.ud = &http_st, .get = fake_get, .post_json = fake_post, .download_file = fake_dl};
  fake_host host = {.installed_ver = "1.0.0", .consented_perms = 0};
  ccord_catalog_host_cbs cbs = {
    .ud = &host, .get_local_plugin = host_get_local, .list_local_plugins = host_list_local,
    .import_verified_cce = host_import, .revoke_plugin = host_revoke,
  };

  ccord_catalog *cat = ccord_catalog_create(dir, contrib, &http, &cbs);
  CT_CHECK(cat != nullptr);
  ccord_catalog_check_updates(cat);
  pump_until(cat, 2000, pred_revoked_two, &host);
  CT_CHECK(host.revoke_calls >= 2);
  CT_CHECK(ccord_catalog_is_revoked(cat, "org.ccord.bad", nullptr, 0));
  CT_CHECK(ccord_catalog_is_revoked(cat, "org.ccord.bad2", nullptr, 0));
  CT_CHECK(!ccord_catalog_is_revoked(cat, "org.ccord.demo", nullptr, 0));

  // 2. Sideloaded .cce hash recorded via ccord_catalog_record_local_install persists in installed-meta.json
  const char *sideload_sha = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  ccord_catalog_record_local_install(cat, "org.ccord.sideload", sideload_sha);
  CT_CHECK(!ccord_catalog_is_revoked(cat, "org.ccord.sideload", nullptr, 0));

  // Survives restart: ID-only revocations and sideload hash remain persisted
  ccord_catalog_destroy(cat);
  host.revoke_calls = 0;
  host.nrevoked_ids = 0;
  cat = ccord_catalog_create(dir, contrib, &http, &cbs);
  CT_CHECK(cat != nullptr);
  CT_CHECK(ccord_catalog_is_revoked(cat, "org.ccord.bad", nullptr, 0));
  CT_CHECK(ccord_catalog_is_revoked(cat, "org.ccord.bad2", nullptr, 0));
  CT_CHECK(host.revoke_calls == 2);

  // 3. Revoking the sideloaded SHA-256 enforces revocation on org.ccord.sideload!
  char rev_sideload[512];
  snprintf(rev_sideload, sizeof rev_sideload,
           "{\"schema\":2,\"seq\":6,\"since\":5,\"full\":false,\"revoked\":["
           "{\"seq\":6,\"sha256\":\"%s\",\"plugin_id\":\"\",\"reason\":\"trojanized cce\"}]}",
           sideload_sha);
  host.revoke_calls = 0;
  host.revoked_id[0] = '\0';
  host.nrevoked_ids = 0;
  http_st.updates_json = "{\"schema\":2,\"rev_seq\":6,\"revocations_stale\":true,\"plugins\":[]}";
  http_st.revocations_json = rev_sideload;
  ccord_catalog_check_updates(cat);
  pump_until(cat, 2000, pred_revoked, &host);
  CT_CHECK(ccord_catalog_is_revoked(cat, "org.ccord.sideload", nullptr, 0));
  bool saw_sideload_revoke = false;
  for (size_t i = 0; i < host.nrevoked_ids; i++) {
    if (strcmp(host.revoked_ids[i], "org.ccord.sideload") == 0) saw_sideload_revoke = true;
  }
  CT_CHECK(saw_sideload_revoke);

  ccord_catalog_destroy(cat);
  ccord_contrib_destroy(contrib);
}

#if defined(CCORD_HAVE_SODIUM) && CCORD_HAVE_SODIUM
static bool pred_detail_error(void *ud)
{
  const ccord_contrib *c = ud;
  const ccord_catalog_status *st = ccord_contrib_catalog_status(c);
  return st && !st->detail_loading && st->detail_error[0] != '\0';
}

static bool pred_revocations_sig_rejected(void *ud)
{
  const ccord_contrib *c = ud;
  const ccord_catalog_status *st = ccord_contrib_catalog_status(c);
  return st && strstr(st->status_msg, "Rejected revocations") != nullptr;
}

static void bytes_to_hex_lower(const unsigned char *in, size_t len, char *out)
{
  static const char k_hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2u] = k_hex[(size_t)(in[i] >> 4)];
    out[i * 2u + 1u] = k_hex[(size_t)(in[i] & 0x0fu)];
  }
  out[len * 2u] = '\0';
}

static void sign_msg_hex(const char *msg, const unsigned char sk[crypto_sign_SECRETKEYBYTES], char sig_hex[129])
{
  unsigned char sig[crypto_sign_BYTES];
  crypto_sign_detached(sig, nullptr, (const unsigned char *)msg, (unsigned long long)strlen(msg), sk);
  bytes_to_hex_lower(sig, sizeof sig, sig_hex);
}

static void test_ed25519_signature_verification(void)
{
  CT_CHECK(sodium_init() >= 0);

  unsigned char pk[crypto_sign_PUBLICKEYBYTES];
  unsigned char sk[crypto_sign_SECRETKEYBYTES];
  crypto_sign_keypair(pk, sk);

  char pk_hex[65];
  bytes_to_hex_lower(pk, sizeof pk, pk_hex);

  unsigned char pk_hash[32];
  ccord_sha256(pk, sizeof pk, pk_hash);
  char key_id[17];
  bytes_to_hex_lower(pk_hash, 8, key_id);

  char tmpl[] = "/tmp/ccord_cat_ed25519_XXXXXX";
  char *dir = mkdtemp(tmpl);
  CT_CHECK(dir != nullptr);
  if (!dir) return;

  const char *good_sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  const char *bad_sha  = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

  // Canonical plugin message (note unsorted/duplicate wire JSON arrays -> sorted unique canonical CSV)
  char canon_plugin[1024];
  snprintf(canon_plugin, sizeof canon_plugin,
           "ccord-plugin-v2\n"
           "org.ccord.demo\n"
           "2\n"
           "1.2.0\n"
           "%s\n"
           "https://plugins.c-cord.org/d/org.ccord.demo\n"
           "fs.read,network.http\n"
           "api.example.com,cdn.example.com\n"
           "2\n"
           "0\n"
           "12345\n"
           "67890\n",
           good_sha);
  char plugin_sig[129];
  sign_msg_hex(canon_plugin, sk, plugin_sig);

  char valid_detail[2048];
  snprintf(valid_detail, sizeof valid_detail,
           "{\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Signed demo\",\"license\":\"MIT\","
           "\"tier\":2,\"downloads\":42,\"needs_tier_review\":false,"
           "\"author\":{\"login\":\"alice\",\"repo\":\"alice/demo\",\"repo_url\":\"https://github.com/alice/demo\","
           "\"github_user_id\":12345,\"repo_id\":67890},"
           "\"latest\":{\"version\":\"1.2.0\",\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.demo\","
           "\"sha256\":\"%s\",\"size_bytes\":25,\"permissions\":[\"network.http\",\"fs.read\"],"
           "\"allowed_hosts\":[\"cdn.example.com\",\"api.example.com\",\"api.example.com\"],\"api_class\":3},"
           "\"warnings\":[{\"severity\":\"caution\",\"title\":\"Note\",\"message\":\"Reads disk\"}],"
           "\"signature\":{\"alg\":\"ed25519\",\"key_id\":\"%s\",\"sig\":\"%s\"}}",
           good_sha, key_id, plugin_sig);

  char unsigned_detail[2048];
  snprintf(unsigned_detail, sizeof unsigned_detail,
           "{\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Unsigned\",\"license\":\"MIT\","
           "\"tier\":2,\"downloads\":42,\"needs_tier_review\":false,"
           "\"author\":{\"login\":\"alice\",\"github_user_id\":12345,\"repo_id\":67890},"
           "\"latest\":{\"version\":\"1.2.0\",\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.demo\","
           "\"sha256\":\"%s\",\"size_bytes\":25,\"permissions\":[\"network.http\",\"fs.read\"],"
           "\"allowed_hosts\":[\"api.example.com\",\"cdn.example.com\"],\"api_class\":3},"
           "\"warnings\":[{\"severity\":\"caution\",\"title\":\"Note\",\"message\":\"Reads disk\"}]}",
           good_sha);

  char tampered_sha_detail[2048];
  snprintf(tampered_sha_detail, sizeof tampered_sha_detail,
           "{\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Tampered sha\",\"license\":\"MIT\","
           "\"tier\":2,\"downloads\":42,\"needs_tier_review\":false,"
           "\"author\":{\"login\":\"alice\",\"github_user_id\":12345,\"repo_id\":67890},"
           "\"latest\":{\"version\":\"1.2.0\",\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.demo\","
           "\"sha256\":\"%s\",\"size_bytes\":25,\"permissions\":[\"network.http\",\"fs.read\"],"
           "\"allowed_hosts\":[\"api.example.com\",\"cdn.example.com\"],\"api_class\":3},"
           "\"warnings\":[{\"severity\":\"caution\",\"title\":\"Note\",\"message\":\"Reads disk\"}],"
           "\"signature\":{\"alg\":\"ed25519\",\"key_id\":\"%s\",\"sig\":\"%s\"}}",
           bad_sha, key_id, plugin_sig);

  char tampered_hosts_detail[2048];
  snprintf(tampered_hosts_detail, sizeof tampered_hosts_detail,
           "{\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Tampered hosts\",\"license\":\"MIT\","
           "\"tier\":2,\"downloads\":42,\"needs_tier_review\":false,"
           "\"author\":{\"login\":\"alice\",\"github_user_id\":12345,\"repo_id\":67890},"
           "\"latest\":{\"version\":\"1.2.0\",\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.demo\","
           "\"sha256\":\"%s\",\"size_bytes\":25,\"permissions\":[\"network.http\",\"fs.read\"],"
           "\"allowed_hosts\":[\"api.example.com\",\"evil.example\"],\"api_class\":3},"
           "\"warnings\":[{\"severity\":\"caution\",\"title\":\"Note\",\"message\":\"Reads disk\"}],"
           "\"signature\":{\"alg\":\"ed25519\",\"key_id\":\"%s\",\"sig\":\"%s\"}}",
           good_sha, key_id, plugin_sig);

  char tampered_tier_detail[2048];
  snprintf(tampered_tier_detail, sizeof tampered_tier_detail,
           "{\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Tampered tier\",\"license\":\"MIT\","
           "\"tier\":1,\"downloads\":42,\"needs_tier_review\":false,"
           "\"author\":{\"login\":\"alice\",\"github_user_id\":12345,\"repo_id\":67890},"
           "\"latest\":{\"version\":\"1.2.0\",\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.demo\","
           "\"sha256\":\"%s\",\"size_bytes\":25,\"permissions\":[\"network.http\",\"fs.read\"],"
           "\"allowed_hosts\":[\"api.example.com\",\"cdn.example.com\"],\"api_class\":3},"
           "\"warnings\":[{\"severity\":\"caution\",\"title\":\"Note\",\"message\":\"Reads disk\"}],"
           "\"signature\":{\"alg\":\"ed25519\",\"key_id\":\"%s\",\"sig\":\"%s\"}}",
           good_sha, key_id, plugin_sig);

  ccord_error err = {0};
  ccord_contrib *contrib = ccord_contrib_create(&err);
  CT_CHECK(contrib != nullptr);

  fake_http_state http_st = {.detail_json = valid_detail};
  ccord_catalog_http http = {.ud = &http_st, .get = fake_get, .post_json = fake_post, .download_file = fake_dl};
  fake_host host = {.installed_ver = "1.0.0", .consented_perms = 0};
  ccord_catalog_host_cbs cbs = {
    .ud = &host, .get_local_plugin = host_get_local, .list_local_plugins = host_list_local,
    .import_verified_cce = host_import, .revoke_plugin = host_revoke,
  };

  ccord_catalog *cat = ccord_catalog_create(dir, contrib, &http, &cbs);
  CT_CHECK(cat != nullptr);
  CT_CHECK(!ccord_catalog_set_pubkeys(cat, "not-valid-hex"));
  CT_CHECK(ccord_catalog_set_pubkeys(cat, pk_hex));

  // 1. Valid signed detail passes!
  ccord_catalog_fetch_detail(cat, "org.ccord.demo");
  pump_until(cat, 2000, pred_detail_ready, contrib);
  const ccord_catalog_entry *det = ccord_contrib_catalog_detail(contrib);
  CT_CHECK(det != nullptr && det->has_detail && strcmp(det->sha256, good_sha) == 0);
  CT_CHECK(ccord_contrib_catalog_status(contrib)->detail_error[0] == '\0');

  // 2. Missing signature detail is REJECTED (and does NOT download when install requested)!
  http_st.detail_json = unsigned_detail;
  CT_CHECK(ccord_catalog_set_pubkeys(cat, pk_hex)); // clears cached detail
  ccord_catalog_install(cat, "org.ccord.demo");
  pump_until(cat, 2000, pred_detail_error, contrib);
  CT_CHECK(ccord_contrib_catalog_detail(contrib) == nullptr);
  CT_CHECK(strstr(ccord_contrib_catalog_status(contrib)->detail_error, "signature") != nullptr);
  CT_CHECK(http_st.dl_calls == 0);

  // 3. Tampered SHA-256 detail is REJECTED!
  http_st.detail_json = tampered_sha_detail;
  ccord_catalog_fetch_detail(cat, "org.ccord.demo");
  pump_until(cat, 2000, pred_detail_error, contrib);
  CT_CHECK(ccord_contrib_catalog_detail(contrib) == nullptr);
  CT_CHECK(strstr(ccord_contrib_catalog_status(contrib)->detail_error, "signature") != nullptr);

  // 4. Tampered allowed_hosts detail is REJECTED!
  http_st.detail_json = tampered_hosts_detail;
  ccord_catalog_fetch_detail(cat, "org.ccord.demo");
  pump_until(cat, 2000, pred_detail_error, contrib);
  CT_CHECK(ccord_contrib_catalog_detail(contrib) == nullptr);
  CT_CHECK(strstr(ccord_contrib_catalog_status(contrib)->detail_error, "signature") != nullptr);

  // 5. Tampered tier detail is REJECTED!
  http_st.detail_json = tampered_tier_detail;
  ccord_catalog_fetch_detail(cat, "org.ccord.demo");
  pump_until(cat, 2000, pred_detail_error, contrib);
  CT_CHECK(ccord_contrib_catalog_detail(contrib) == nullptr);
  CT_CHECK(strstr(ccord_contrib_catalog_status(contrib)->detail_error, "signature") != nullptr);

  // 6. Updates check: valid signed item accepted while tampered item is skipped!
  char updates_mixed[4096];
  snprintf(updates_mixed, sizeof updates_mixed,
           "{\"schema\":2,\"rev_seq\":0,\"revocations_stale\":false,\"plugins\":["
           "{\"id\":\"org.ccord.bad\",\"name\":\"Tampered\",\"description\":\"Bad\",\"license\":\"MIT\","
           "\"tier\":1,\"author_login\":\"mallory\",\"author_github_id\":12345,\"repo_id\":67890,"
           "\"version\":\"1.2.0\",\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.demo\","
           "\"sha256\":\"%s\",\"size_bytes\":25,\"permissions\":[\"fs.read\",\"network.http\"],"
           "\"allowed_hosts\":[\"api.example.com\",\"cdn.example.com\"],\"needs_tier_review\":false,"
           "\"warnings\":[{\"severity\":\"caution\",\"title\":\"Note\",\"message\":\"Reads disk\"}],"
           "\"signature\":{\"alg\":\"ed25519\",\"key_id\":\"%s\",\"sig\":\"%s\"}},"
           "{\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"description\":\"Good\",\"license\":\"MIT\","
           "\"tier\":2,\"author_login\":\"alice\",\"author_github_id\":12345,\"repo_id\":67890,"
           "\"version\":\"1.2.0\",\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.demo\","
           "\"sha256\":\"%s\",\"size_bytes\":25,\"permissions\":[\"network.http\",\"fs.read\"],"
           "\"allowed_hosts\":[\"cdn.example.com\",\"api.example.com\"],\"needs_tier_review\":false,"
           "\"warnings\":[{\"severity\":\"caution\",\"title\":\"Note\",\"message\":\"Reads disk\"}],"
           "\"signature\":{\"alg\":\"ed25519\",\"key_id\":\"%s\",\"sig\":\"%s\"}}]}",
           good_sha, key_id, plugin_sig, good_sha, key_id, plugin_sig);
  http_st.updates_json = updates_mixed;
  ccord_catalog_check_updates(cat);
  pump_until(cat, 2000, pred_updates_ready, contrib);
  CT_CHECK(ccord_contrib_catalog_update_count(contrib) == 1);
  const ccord_catalog_entry *u0 = ccord_contrib_catalog_update_at(contrib, 0);
  CT_CHECK(u0 != nullptr && strcmp(u0->id, "org.ccord.demo") == 0);

  // 7. Unsigned revocations response is REJECTED!
  ccord_catalog_record_local_install(cat, "org.ccord.demo", good_sha);
  http_st.updates_json = "{\"schema\":2,\"rev_seq\":1,\"revocations_stale\":true,\"plugins\":[]}";
  http_st.revocations_json =
    "{\"schema\":2,\"seq\":1,\"full\":true,\"revoked\":[{\"seq\":1,\"sha256\":\"\",\"plugin_id\":\"org.ccord.demo\",\"reason\":\"forged\"}]}";
  ccord_catalog_check_updates(cat);
  pump_until(cat, 2000, pred_revocations_sig_rejected, contrib);
  CT_CHECK(strstr(ccord_contrib_catalog_status(contrib)->status_msg, "Rejected revocations") != nullptr);
  CT_CHECK(host.revoke_calls == 0);
  CT_CHECK(!ccord_catalog_is_revoked(cat, "org.ccord.demo", nullptr, 0));

  // 8. Valid signed revocations response (with | and \n in reason) is ACCEPTED!
  char canon_rev[512];
  snprintf(canon_rev, sizeof canon_rev,
           "ccord-revocations-v2\n1\n1\n1|%s|org.ccord.demo|bad payload line2\n",
           good_sha);
  char rev_sig[129];
  sign_msg_hex(canon_rev, sk, rev_sig);
  char signed_rev[1024];
  snprintf(signed_rev, sizeof signed_rev,
           "{\"schema\":2,\"seq\":1,\"full\":true,\"revoked\":["
           "{\"seq\":1,\"sha256\":\"%s\",\"plugin_id\":\"org.ccord.demo\",\"reason\":\"bad|payload\\nline2\"}],"
           "\"signature\":{\"alg\":\"ed25519\",\"key_id\":\"%s\",\"sig\":\"%s\"}}",
           good_sha, key_id, rev_sig);
  http_st.revocations_json = signed_rev;
  ccord_catalog_check_updates(cat);
  pump_until(cat, 2000, pred_revoked, &host);
  CT_CHECK(host.revoke_calls == 1);
  CT_CHECK(ccord_catalog_is_revoked(cat, "org.ccord.demo", nullptr, 0));

  // 9. Forged empty full=true revocations cannot wipe verified revocations cache!
  http_st.updates_json = "{\"schema\":2,\"rev_seq\":2,\"revocations_stale\":true,\"plugins\":[]}";
  http_st.revocations_json = "{\"schema\":2,\"seq\":2,\"full\":true,\"revoked\":[]}";
  ccord_catalog_check_updates(cat);
  pump_until(cat, 2000, pred_revocations_sig_rejected, contrib);
  CT_CHECK(strstr(ccord_contrib_catalog_status(contrib)->status_msg, "Rejected revocations") != nullptr);
  CT_CHECK(ccord_catalog_is_revoked(cat, "org.ccord.demo", nullptr, 0));

  ccord_catalog_destroy(cat);
  ccord_contrib_destroy(contrib);
}

// Cross-language interoperability check: exact deterministic Ed25519 signatures produced by
// plugin-catalog's Go crypto/ed25519 Signer (ccord-plugin-v2 and ccord-revocations-v2).
static void test_go_interop_ed25519_vector(void)
{
  const char *go_pk_hex = "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8";
  const char *go_key_id = "56475aa75463474c";
  const char *go_sha    = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  const char *go_plugin_sig =
    "e9ceea4c34568f4a6b6878f644e638ae322dffeeba64f7be4f1254312d95b775"
    "4acd92e6209b9f72724ea93c4d96b4df569f78d35afd5963a8190944f08d7c07";
  const char *go_rev_sig =
    "2f623764117058c91fc37950cfabfa6563fc059f800b7d7def2ce8696c83aaf6"
    "628f886ff4a3dfbe4444379148bbad0073785cb44d0d61b19336f839a33f1109";

  char tmpl[] = "/tmp/ccord_cat_go_interop_XXXXXX";
  char *dir = mkdtemp(tmpl);
  CT_CHECK(dir != nullptr);
  if (!dir) return;

  char detail_json[2048];
  snprintf(detail_json, sizeof detail_json,
           "{\"id\":\"org.ccord.example\",\"name\":\"Example\",\"description\":\"Go signed\",\"license\":\"MIT\","
           "\"tier\":2,\"downloads\":10,\"needs_tier_review\":true,"
           "\"author\":{\"login\":\"alice\",\"repo\":\"alice/example\",\"repo_url\":\"https://github.com/alice/example\","
           "\"github_user_id\":12345,\"repo_id\":67890},"
           "\"latest\":{\"version\":\"1.2.3\",\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.example\","
           "\"sha256\":\"%s\",\"size_bytes\":123,\"permissions\":[\"ui.panel\",\"network.http\"],"
           "\"allowed_hosts\":[\"b.example.com\",\"a.example.com\"],\"api_class\":3},"
           "\"warnings\":[{\"severity\":\"caution\",\"title\":\"Check\",\"message\":\"Review\"}],"
           "\"signature\":{\"alg\":\"ed25519\",\"key_id\":\"%s\",\"sig\":\"%s\"}}",
           go_sha, go_key_id, go_plugin_sig);

  char updates_json[2048];
  snprintf(updates_json, sizeof updates_json,
           "{\"schema\":2,\"rev_seq\":2,\"revocations_stale\":true,\"plugins\":[{"
           "\"id\":\"org.ccord.example\",\"name\":\"Example\",\"description\":\"Go signed\",\"license\":\"MIT\","
           "\"tier\":2,\"needs_tier_review\":true,\"author_login\":\"alice\","
           "\"author_github_id\":12345,\"repo_id\":67890,"
           "\"version\":\"1.2.3\",\"download_url\":\"https://plugins.c-cord.org/d/org.ccord.example\","
           "\"sha256\":\"%s\",\"size_bytes\":123,\"permissions\":[\"ui.panel\",\"network.http\"],"
           "\"allowed_hosts\":[\"b.example.com\",\"a.example.com\"],"
           "\"warnings\":[{\"severity\":\"caution\",\"title\":\"Check\",\"message\":\"Review\"}],"
           "\"signature\":{\"alg\":\"ed25519\",\"key_id\":\"%s\",\"sig\":\"%s\"}}]}",
           go_sha, go_key_id, go_plugin_sig);

  char rev_json[1024];
  snprintf(rev_json, sizeof rev_json,
           "{\"schema\":2,\"seq\":2,\"full\":true,\"revoked\":["
           "{\"seq\":1,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
           "\"plugin_id\":\"io.bad.one\",\"reason\":\"Malware|line1\\rline2\\nline3\"},"
           "{\"seq\":2,\"sha256\":\"\",\"plugin_id\":\"io.bad.one\",\"reason\":\"Plugin ID kill-switch\"}],"
           "\"signature\":{\"alg\":\"ed25519\",\"key_id\":\"%s\",\"sig\":\"%s\"}}",
           go_key_id, go_rev_sig);

  ccord_error err = {0};
  ccord_contrib *contrib = ccord_contrib_create(&err);
  CT_CHECK(contrib != nullptr);

  fake_http_state http_st = {
    .detail_json = detail_json,
    .updates_json = updates_json,
    .revocations_json = rev_json,
  };
  ccord_catalog_http http = {.ud = &http_st, .get = fake_get, .post_json = fake_post, .download_file = fake_dl};
  fake_host host = {.installed_ver = "1.0.0", .consented_perms = 0};
  ccord_catalog_host_cbs cbs = {
    .ud = &host, .get_local_plugin = host_get_local, .list_local_plugins = host_list_local,
    .import_verified_cce = host_import, .revoke_plugin = host_revoke,
  };

  ccord_catalog *cat = ccord_catalog_create(dir, contrib, &http, &cbs);
  CT_CHECK(cat != nullptr);
  CT_CHECK(ccord_catalog_set_pubkeys(cat, go_pk_hex));

  // Verify Go-signed detail passes byte-for-byte
  ccord_catalog_fetch_detail(cat, "org.ccord.example");
  pump_until(cat, 2000, pred_detail_ready, contrib);
  const ccord_catalog_entry *det = ccord_contrib_catalog_detail(contrib);
  CT_CHECK(det != nullptr && det->has_detail && strcmp(det->id, "org.ccord.example") == 0);

  // Verify Go-signed updates item + revocations (with |, \r, \n sanitization and ID-only entry) pass byte-for-byte
  ccord_catalog_check_updates(cat);
  pump_until(cat, 2000, pred_revoked, &host);
  CT_CHECK(ccord_catalog_is_revoked(cat, "io.bad.one", nullptr, 0));
  CT_CHECK(ccord_catalog_is_hash_or_id_revoked(
    cat, "io.other", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", nullptr, 0));
  CT_CHECK(ccord_catalog_is_hash_or_id_revoked(cat, "io.bad.one", "", nullptr, 0));
  CT_CHECK(!ccord_catalog_is_hash_or_id_revoked(cat, "io.clean", "", nullptr, 0));
  // Verify the Go-signed update item itself was also accepted and flags needs_tier_review!
  ccord_catalog_local_overlay ov_go = {0};
  CT_CHECK(ccord_catalog_query_overlay(cat, "org.ccord.example", "1.0.0", 0, &ov_go));
  CT_CHECK(strcmp(ov_go.update_version, "1.2.3") == 0 && ov_go.update_needs_review);

  ccord_catalog_destroy(cat);
  ccord_contrib_destroy(contrib);
}
#endif

int main(void)
{
  test_semver();
  test_zero_installed_makes_zero_startup_calls();
  test_v2_discover_updates_sha_and_revocations();
  test_manifest_icon_and_catalog_v2_fields();
  test_allowed_hosts_expansion_and_tier_review_block_auto_update();
  test_id_level_revocation_and_sideload_hash_tracking();
#if defined(CCORD_HAVE_SODIUM) && CCORD_HAVE_SODIUM
  test_ed25519_signature_verification();
  test_go_interop_ed25519_vector();
#endif
  return CT_RESULT("test_plugin_catalog");
}
