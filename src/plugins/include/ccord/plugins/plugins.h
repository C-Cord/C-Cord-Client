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
  bool (*post_json)(void *ud, const char *url, const char *json_body, size_t body_len, size_t max_bytes,
                    char **out_body, size_t *out_len, char *err_buf, size_t err_cap);
  bool (*download_file)(void *ud, const char *url, const char *dest_path, size_t max_bytes,
                        char *err_buf, size_t err_cap);
} ccord_catalog_http;

typedef bool (*ccord_plugin_http_url_check_fn)(void *ud, const char *url, char *err_buf, size_t err_cap);

// Generalized plugin HTTP request/response (strict TLS 1.2/1.3 for external URLs; plain HTTP allowed only for localhost).
typedef struct ccord_plugin_http_req {
  const char *url;
  const char *method;             // "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"
  const char *headers_json;       // optional JSON object of request headers
  const uint8_t *body;
  size_t body_len;
  uint32_t timeout_ms;
  size_t max_bytes;
  bool allow_localhost_http;
  bool follow_redirects;
  uint8_t max_redirects;
  ccord_plugin_http_url_check_fn allow_url;
  void *allow_url_ud;
} ccord_plugin_http_req;

typedef struct ccord_plugin_http_resp {
  long status_code;
  uint8_t *body;                  // ccord_malloc'd, NUL-terminated
  size_t body_len;
  char *headers_json;             // ccord_malloc'd JSON object of response headers (optional)
  char final_url[2048];           // populated if redirected
  char error[256];
} ccord_plugin_http_resp;

typedef struct ccord_plugin_http_dl_req {
  const char *url;
  const char *dest_path;
  const char *headers_json;       // optional JSON object of request headers
  uint32_t timeout_ms;
  size_t max_bytes;
  bool allow_localhost_http;
  bool follow_redirects;
  uint8_t max_redirects;
  ccord_plugin_http_url_check_fn allow_url;
  void *allow_url_ud;
} ccord_plugin_http_dl_req;

typedef struct ccord_plugin_http_dl_resp {
  long status_code;
  size_t bytes_written;
  char content_type[128];
  char final_url[2048];
  char error[256];
} ccord_plugin_http_dl_resp;

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
// `ui.mobile.*`, `clipboard.read`, `system.open_url`, `system.notify`, plus API v3 extensions).
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
  // API v3 extended host bridges (all nullable)
  bool (*clipboard_read_image)(void *ud, uint8_t **out_png, size_t *out_len);
  bool (*clipboard_write_image)(void *ud, const uint8_t *png, size_t len);
  bool (*http_request)(void *ud, const ccord_plugin_http_req *req, ccord_plugin_http_resp *resp);
  bool (*http_download)(void *ud, const ccord_plugin_http_dl_req *req, ccord_plugin_http_dl_resp *resp);
  // External WSS transport (nullable: loopback ws:// works natively without this)
  void *(*ws_open)(void *ud, const char *url, const char *headers_json, char *err_buf, size_t err_cap);
  bool (*ws_send)(void *ud, void *handle, const uint8_t *data, size_t len, bool binary);
  int (*ws_recv)(void *ud, void *handle, uint8_t *buf, size_t cap, bool *out_binary, int timeout_ms); // >0 bytes, 0 timeout, -1 closed/err
  void (*ws_close)(void *ud, void *handle, uint16_t code, const char *reason);
  // Theme inspection & sync
  char *(*theme_current_json)(void *ud);             // ccord_malloc'd JSON { id, name, colors:{...}, metrics:{...} }
  char *(*theme_color_hex)(void *ud, const char *token);
  bool (*theme_metric_val)(void *ud, const char *metric, double *out_val);
  char *(*theme_list_json)(void *ud);
  // Scheduled messages bridge
  bool (*scheduled_add)(void *ud, ccord_id channel_id, const char *utf8, size_t len, int64_t due_unix_ms,
                        char *err, size_t err_cap);
  char *(*scheduled_list_json)(void *ud);
  bool (*scheduled_cancel)(void *ud, uint64_t id);
  // Voice levels & OS media/processes/shortcuts bridges
  char *(*voice_levels_json)(void *ud);
  bool (*voice_push_pcm)(void *ud, const int16_t *pcm, size_t samples);
  char *(*media_current_json)(void *ud);
  bool (*media_control)(void *ud, const char *action);
  char *(*processes_list_json)(void *ud);
  char *(*active_window_json)(void *ud);
  void (*global_shortcuts_sync)(void *ud);
  // Decode an encoded image buffer and box-downscale to RGBA32 (ccord_malloc'd *out_rgba).
  bool (*decode_image_rgba)(void *ud, const uint8_t *encoded, size_t len, int max_w, int max_h,
                            uint8_t **out_rgba, int *out_w, int *out_h, char *err_buf, size_t err_cap);
  // Optional re-encoder for hooks.on_before_upload (`format`: "png"|"jpeg"|"webp", `quality`: 1..100).
  bool (*reencode_image)(void *ud, const uint8_t *src, size_t src_len, const char *format, int quality,
                         int max_dim, uint8_t **out_bytes, size_t *out_len, char *err_buf, size_t err_cap);
} ccord_plugin_platform_ops;

typedef struct ccord_plugins_config {
  const char *host_exe;                          // path of ccord-plugin-host (optional when in_process=true)
  bool in_process;                               // true: run sandboxed plugin host on worker threads over socketpair (used on Android)
  int (*in_process_host_fn)(ccord_socket_t fd, const _Atomic bool *cancel);   // required when in_process=true
  const char *const *search_dirs; size_t search_dir_count;   // each child dir with plugin.json is a plugin
  const char *data_dir;                          // storage root: <data_dir>/plugins/<id>/storage.json
  ccord_bus *bus; ccord_store *store; ccord_session *session; ccord_contrib *contrib;
  void (*clipboard_write)(void *ud, const char *utf8);       // called on the main thread after checks
  void *clipboard_ud;
  void (*toast_show)(void *ud, const char *utf8, uint8_t level); // optional UI toast callback
  void *toast_ud;
  ccord_catalog_http catalog_http;               // optional HTTPS fetcher for decentralized plugin catalog
  bool has_platform_info;
  ccord_plugin_platform_info platform_info;
  ccord_plugin_platform_state platform_state;
  ccord_plugin_platform_ops platform_ops;
  bool dev_auto_reload;                          // --plugin-dev: auto-reload plugins when files change
} ccord_plugins_config;

ccord_plugins *ccord_plugins_create(const ccord_plugins_config *cfg, ccord_error *err);   // registers contrib handlers + bus subscriptions
void ccord_plugins_destroy(ccord_plugins *p);                        // = shutdown + free
size_t ccord_plugins_load_all(ccord_plugins *p);                      // discover, validate manifests, spawn hosts; returns started count
void ccord_plugins_pump(ccord_plugins *p, uint64_t now_ms);           // read IPC, enforce permissions, send events, pings, reap children, flush storage
// Pauses/resumes periodic keep-alive pings while the app is backgrounded so idle plugins sleep with zero wakeups.
void ccord_plugins_set_background(ccord_plugins *p, bool background);
// Collects active plugin IPC sockets for kernel poll() and reports the next pending deadline/flush timeout (UINT32_MAX = none).
size_t ccord_plugins_wait_info(const ccord_plugins *p, ccord_socket_t *out_fds, size_t cap, uint32_t *out_timeout_ms);
void ccord_plugins_shutdown(ccord_plugins *p);                        // send shutdown, wait <=1 s, SIGKILL leftovers
int ccord_plugins_host_pid(const ccord_plugins *p, const char *owner);            // child pid of the running host, or -1 (diagnostics and tests)
// Pushes updated safe-area/viewport/keyboard/orientation metrics to all active plugins and emits change events.
void ccord_plugins_update_platform_state(ccord_plugins *p, const ccord_plugin_platform_state *st);
// Dispatches Class 1 `app.lifecycle` ("foreground"|"background"|"low_memory") to subscribed plugins.
void ccord_plugins_dispatch_lifecycle(ccord_plugins *p, const char *state);
// API v3 client -> plugin dispatchers
bool ccord_plugins_has_raw_gateway_subscribers(const ccord_plugins *p);
void ccord_plugins_dispatch_raw_gateway(ccord_plugins *p, const char *event_name, const char *payload_json);
void ccord_plugins_dispatch_theme_change(ccord_plugins *p, const char *theme_id);
void ccord_plugins_dispatch_window_focus(ccord_plugins *p, bool focused);
void ccord_plugins_dispatch_window_resize(ccord_plugins *p, float width, float height);
void ccord_plugins_dispatch_streamer_mode(ccord_plugins *p, bool enabled);
void ccord_plugins_dispatch_composer_change(ccord_plugins *p, ccord_id channel_id, const char *text);
void ccord_plugins_dispatch_voice_pcm(ccord_plugins *p, const int16_t *pcm, size_t samples);
void ccord_plugins_dispatch_voice_levels(ccord_plugins *p, const char *levels_json);
bool ccord_plugins_get_consented_hosts(const ccord_plugins *p, const char *id,
                                       char *hosts_out, size_t hosts_cap, bool *had_consented_out);
bool ccord_plugins_import_package(ccord_plugins *p, const char *pkg_path, char *err_buf, size_t err_cap);
// Returns the plugin id from the most recent successful package import, or nullptr if none/failed.
const char *ccord_plugins_last_imported_id(const ccord_plugins *p);
bool ccord_plugins_import_package_from_catalog(ccord_plugins *p, const char *pkg_path,
                                               const char *expected_id, const char *expected_version,
                                               uint64_t expected_perms, const char *expected_allowed_hosts,
                                               bool preapprove_consent, char *err_buf, size_t err_cap);
bool ccord_plugins_export_cce(ccord_plugins *p, const char *id, const char *dest_path, char *err_buf, size_t err_cap);
bool ccord_plugins_uninstall(ccord_plugins *p, const char *id, char *err_buf, size_t err_cap);
