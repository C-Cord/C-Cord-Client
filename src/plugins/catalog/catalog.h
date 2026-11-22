// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

#include <ccord/model/contrib.h>
#include <ccord/plugins/plugins.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ccord_catalog ccord_catalog;

typedef struct ccord_catalog_host_cbs {
  void *ud;
  // Query locally installed plugin state (main thread only).
  bool (*get_local_plugin)(void *ud, const char *id, char *ver_out, size_t ver_cap,
                           uint64_t *consented_perms_out, bool *is_bundled_out);
  // Enumerate locally installed non-bundled plugin IDs for targeted update/revocation checks.
  size_t (*list_local_plugins)(void *ud, char (*ids_out)[65], size_t cap);
  // Install a SHA-256-verified .cce file on the main thread, enforcing expected catalog id/version/perms.
  bool (*import_verified_cce)(void *ud, const char *pkg_path, const char *expected_id,
                              const char *expected_version, uint64_t expected_perms,
                              char *err_buf, size_t err_cap);
  // Optional: same as import_verified_cce, additionally granting consent atomically when man.perms matches approved_perms.
  bool (*import_verified_cce_approved)(void *ud, const char *pkg_path, const char *expected_id,
                                       const char *expected_version, uint64_t expected_perms,
                                       bool preapprove_consent,
                                       char *err_buf, size_t err_cap);
  // Stop & disable a plugin whose installed SHA-256 was revoked for malware.
  void (*revoke_plugin)(void *ud, const char *id, const char *reason);
  // Re-publish installed plugin metadata (tier, warnings, update_version, auto_update) into contrib.
  void (*sync_installed_info)(void *ud);
  // Optional toast callback.
  void (*toast)(void *ud, const char *msg, uint8_t level);
} ccord_catalog_host_cbs;

ccord_catalog *ccord_catalog_create(const char *data_dir, ccord_contrib *contrib,
                                    const ccord_catalog_http *http,
                                    const ccord_catalog_host_cbs *cbs);
void ccord_catalog_destroy(ccord_catalog *cat);

// Call from ccord_plugins_pump() on the main thread.
void ccord_catalog_pump(ccord_catalog *cat, uint64_t now_ms);

// Actions (main thread only).
void ccord_catalog_refresh(ccord_catalog *cat);
void ccord_catalog_query(ccord_catalog *cat, const char *q, int tier, int api_class, const char *sort,
                         bool append_next_page);
void ccord_catalog_query_ex(ccord_catalog *cat, const char *q, int tier, int api_class, const char *platform,
                            const char *sort, bool append_next_page);
void ccord_catalog_fetch_detail(ccord_catalog *cat, const char *id);
void ccord_catalog_check_updates(ccord_catalog *cat);
void ccord_catalog_install(ccord_catalog *cat, const char *id);
void ccord_catalog_install_approved(ccord_catalog *cat, const char *id, uint64_t approved_perms);
void ccord_catalog_set_prefs(ccord_catalog *cat, const ccord_catalog_prefs *prefs);
void ccord_catalog_set_plugin_auto_update(ccord_catalog *cat, const char *id, bool enabled);
void ccord_catalog_run_auto_updates(ccord_catalog *cat);
void ccord_catalog_on_uninstalled(ccord_catalog *cat, const char *id);
// Re-run cached revocation checks against currently loaded plugins (must be called after load_all).
void ccord_catalog_enforce_revocations(ccord_catalog *cat);
// True if the installed SHA-256 recorded for `id` matches a moderator revocation entry.
bool ccord_catalog_is_revoked(const ccord_catalog *cat, const char *id, char *reason_out, size_t reason_cap);

// Metadata enrichment for installed plugin cards in ccord_plugins.
typedef struct ccord_catalog_local_overlay {
  ccord_plugin_tier tier;
  bool auto_update;
  bool update_needs_review;
  char update_version[33];
  uint64_t update_perms_mask;
  char update_permissions[512];
  char update_allowed_hosts[512];
  char update_changelog[512];
  char icon_path[CCORD_PATH_MAX];
  ccord_catalog_warn_sev warning_sev;
  char warning_title[128];
  char warning_message[512];
} ccord_catalog_local_overlay;

bool ccord_catalog_query_overlay(const ccord_catalog *cat, const char *id,
                                 const char *installed_ver, uint64_t consented_perms,
                                 ccord_catalog_local_overlay *out);

// Exposed for unit tests.
int ccord_catalog_semver_cmp(const char *a, const char *b);
bool ccord_catalog_sha256_file_hex(const char *path, char hex_out[65]);
