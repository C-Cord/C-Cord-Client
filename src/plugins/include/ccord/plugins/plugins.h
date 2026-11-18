// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/event_bus.h>
#include <ccord/model/contrib.h>
#include <ccord/plugins/manifest.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
typedef struct ccord_plugins ccord_plugins;
typedef struct ccord_plugins_config {
  const char *host_exe;                          // path of ccord-plugin-host
  const char *const *search_dirs; size_t search_dir_count;   // each child dir with plugin.json is a plugin
  const char *data_dir;                          // storage root: <data_dir>/plugins/<id>/storage.json
  ccord_bus *bus; const ccord_store *store; ccord_session *session; ccord_contrib *contrib;
  void (*clipboard_write)(void *ud, const char *utf8);       // called on the main thread after checks
  void *clipboard_ud;
  void (*toast_show)(void *ud, const char *utf8, uint8_t level); // optional UI toast callback
  void *toast_ud;
} ccord_plugins_config;
ccord_plugins *ccord_plugins_create(const ccord_plugins_config *cfg, ccord_error *err);   // registers contrib handlers + bus subscriptions
void ccord_plugins_destroy(ccord_plugins *p);                        // = shutdown + free
size_t ccord_plugins_load_all(ccord_plugins *p);                      // discover, validate manifests, spawn hosts; returns started count
void ccord_plugins_pump(ccord_plugins *p, uint64_t now_ms);           // read IPC, enforce permissions, send events, pings, reap children, flush storage
void ccord_plugins_shutdown(ccord_plugins *p);                        // send shutdown, wait <=1 s, SIGKILL leftovers
int ccord_plugins_host_pid(const ccord_plugins *p, const char *owner);            // child pid of the running host, or -1 (diagnostics and tests)
bool ccord_plugins_import_package(ccord_plugins *p, const char *pkg_path, char *err_buf, size_t err_cap);
bool ccord_plugins_export_cce(ccord_plugins *p, const char *id, const char *dest_path, char *err_buf, size_t err_cap);
