// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/event_bus.h>
#include <ccord/core/os.h>
#include <ccord/model/contrib.h>
#include <ccord/plugins/manifest.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>

#include <stdatomic.h>

typedef struct ccord_plugins ccord_plugins;
typedef struct ccord_catalog_http {
  void *ud;
  bool (*get)(void *ud, const char *url, const char *if_none_match, size_t max_bytes,
              char **out_body, size_t *out_len, char *out_etag, size_t etag_cap,
              bool *out_not_modified, char *err_buf, size_t err_cap);
  bool (*download_file)(void *ud, const char *url, const char *dest_path, size_t max_bytes,
                        char *err_buf, size_t err_cap);
} ccord_catalog_http;

// Static OS/platform descriptors handed to plugins at init for Class 1 `platform.*`.
typedef struct ccord_plugin_platform_info {
  const char *os;             // nullptr -> ccord_os_name()
  const char *arch;           // nullptr -> ccord_os_arch()
  const char *form_factor;    // "desktop", "phone", "tablet" (nullptr -> auto)
  bool is_mobile;
  bool touch;
  int android_api_level;
  uint64_t capabilities;      // CCORD_CAP_* bitmask
} ccord_plugin_platform_info;

// Live viewport/safe-area/keyboard/orientation metrics pushed to running plugins.
typedef struct ccord_plugin_platform_state {
  float safe_top, safe_right, safe_bottom, safe_left;
  float viewport_w, viewport_h, viewport_scale;
  float keyboard_height;
  bool keyboard_visible;
  bool landscape;
} ccord_plugin_platform_state;

// Optional OS/mobile action callbacks bridged from Lua plugins (`ui.haptics.*`, `ui.share.*`, `ui.keyboard.*`,
// `ui.mobile.*`, `clipboard.read`, `system.open_url`, `system.notify`). Any null callback returns false/no-op.
typedef struct ccord_plugin_platform_ops {
  void *ud;
  bool (*haptic_impact)(void *ud, int style);        // 0=light, 1=medium, 2=heavy, 3=selection
  bool (*haptic_notify)(void *ud, int kind);         // 0=success, 1=warning, 2=error
  bool (*share_text)(void *ud, const char *title, const char *text);
  void (*keyboard_dismiss)(void *ud);
  bool (*keep_screen_on)(void *ud, bool on);
  bool (*set_orientation)(void *ud, int mode);       // 0=auto, 1=portrait, 2=landscape
  bool (*open_url)(void *ud, const char *url);
  bool (*notify)(void *ud, const char *title, const char *body, const char *tag, bool silent);
  char *(*clipboard_read)(void *ud);                 // ccord_malloc'd UTF-8 string or nullptr
} ccord_plugin_platform_ops;

typedef struct ccord_plugins_config {
  const char *host_exe;                          // path of ccord-plugin-host (optional when in_process=true)
  bool in_process;                               // true: run sandboxed plugin host on worker threads over socketpair (used on Android)
  int (*in_process_host_fn)(ccord_socket_t fd, const _Atomic bool *cancel);   // required when in_process=true
  const char *const *search_dirs; size_t search_dir_count;   // each child dir with plugin.json is a plugin
  const char *data_dir;                          // storage root: <data_dir>/plugins/<id>/storage.json
  ccord_bus *bus; const ccord_store *store; ccord_session *session; ccord_contrib *contrib;
  void (*clipboard_write)(void *ud, const char *utf8);       // called on the main thread after checks
  void *clipboard_ud;
  void (*toast_show)(void *ud, const char *utf8, uint8_t level); // optional UI toast callback
  void *toast_ud;
  ccord_catalog_http catalog_http;               // optional HTTPS fetcher for decentralized plugin catalog
  bool has_platform_info;
  ccord_plugin_platform_info platform_info;
  ccord_plugin_platform_state platform_state;
  ccord_plugin_platform_ops platform_ops;
} ccord_plugins_config;

ccord_plugins *ccord_plugins_create(const ccord_plugins_config *cfg, ccord_error *err);   // registers contrib handlers + bus subscriptions
void ccord_plugins_destroy(ccord_plugins *p);                        // = shutdown + free
size_t ccord_plugins_load_all(ccord_plugins *p);                      // discover, validate manifests, spawn hosts; returns started count
void ccord_plugins_pump(ccord_plugins *p, uint64_t now_ms);           // read IPC, enforce permissions, send events, pings, reap children, flush storage
void ccord_plugins_shutdown(ccord_plugins *p);                        // send shutdown, wait <=1 s, SIGKILL leftovers
int ccord_plugins_host_pid(const ccord_plugins *p, const char *owner);            // child pid of the running host, or -1 (diagnostics and tests)
// Pushes updated safe-area/viewport/keyboard/orientation metrics to all active plugins and emits change events.
void ccord_plugins_update_platform_state(ccord_plugins *p, const ccord_plugin_platform_state *st);
// Dispatches Class 1 `app.lifecycle` ("foreground"|"background"|"low_memory") to subscribed plugins.
void ccord_plugins_dispatch_lifecycle(ccord_plugins *p, const char *state);
bool ccord_plugins_import_package(ccord_plugins *p, const char *pkg_path, char *err_buf, size_t err_cap);
bool ccord_plugins_export_cce(ccord_plugins *p, const char *id, const char *dest_path, char *err_buf, size_t err_cap);
bool ccord_plugins_uninstall(ccord_plugins *p, const char *id, char *err_buf, size_t err_cap);
