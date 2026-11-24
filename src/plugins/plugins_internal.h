// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/plugins/plugins.h>
#include "catalog/catalog.h"
#include "workers/plugin_workers.h"

#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>
#include <ccord/ipc/ipc.h>

#if CCORD_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif !CCORD_OS_ANDROID
#include <sys/types.h>
#endif

#define PBUF (2 * CCORD_PATH_MAX + 320)   // composed paths: data_dir + plugin id + file name
#define MAX_SEARCH 16
#define MAX_PENDING 8
#define MAX_CLIPBOARD_BYTES 65536u
#define MAX_STORE_KEYS 128u
#define MAX_STORE_BYTES (256u * 1024u)
#define MAX_FS_READ_BYTES (16u * 1024u * 1024u)
#define MAX_FS_CHUNK_BYTES (384u * 1024u)
#define INIT_DEADLINE_MS 5000u
#define CMD_DEADLINE_MS 2000u
#define PING_INTERVAL_MS 5000u
#define PING_TIMEOUT_MS 5000u
#define SAVE_INTERVAL_MS 5000u
#define EXIT_WAIT_MS 300u

#define EVB_MESSAGE          (1ULL << 0)
#define EVB_GUILD            (1ULL << 1)
#define EVB_CHANNEL          (1ULL << 2)
#define EVB_VIEW_SELECT      (1ULL << 3)
#define EVB_NAV_AUX          (1ULL << 4)
#define EVB_CHANNEL_UPSERT   (1ULL << 5)
#define EVB_CHANNEL_DELETE   (1ULL << 6)
#define EVB_GUILDS_LOADED    (1ULL << 7)
#define EVB_NOTIFY_UPDATE    (1ULL << 8)
#define EVB_MESSAGE_UPDATE   (1ULL << 9)
#define EVB_MESSAGE_DELETE   (1ULL << 10)
#define EVB_MESSAGE_PIN      (1ULL << 11)
#define EVB_REACTION_UPDATE  (1ULL << 12)
#define EVB_TYPING_START     (1ULL << 13)
#define EVB_PRESENCE_UPDATE  (1ULL << 14)
#define EVB_VOICE_STATE      (1ULL << 15)
#define EVB_APP_READY        (1ULL << 16)
#define EVB_CONNECTION_STATE (1ULL << 17)
#define EVB_APP_QUIT         (1ULL << 18)
#define EVB_APP_LIFECYCLE    (1ULL << 19)
#define EVB_PLAT_ORIENTATION (1ULL << 20)
#define EVB_PLAT_KEYBOARD    (1ULL << 21)
#define EVB_PLAT_SAFE_AREA   (1ULL << 22)
#define EVB_VOICE_SERVER     (1ULL << 23)
#define EVB_STREAM_STATE     (1ULL << 24)
// API v3 events
#define EVB_THEME_CHANGE     (1ULL << 25)
#define EVB_WINDOW_FOCUS     (1ULL << 26)
#define EVB_WINDOW_RESIZE    (1ULL << 27)
#define EVB_STREAMER_MODE    (1ULL << 28)
#define EVB_COMPOSER_CHANGE  (1ULL << 29)
#define EVB_VOICE_LEVELS     (1ULL << 30)
#define EVB_SETTINGS_CHANGE  (1ULL << 31)
#define EVB_ACCOUNT_UPDATE   (1ULL << 32)

#define HOOK_BEFORE_SEND     (1u << 0)
#define HOOK_BEFORE_EDIT     (1u << 1)
#define HOOK_NOTIFICATION    (1u << 2)
#define HOOK_LINK_OPEN       (1u << 3)

typedef enum pend_kind { PEND_NONE = 0, PEND_INIT, PEND_PING, PEND_CMD } pend_kind;

typedef struct pending_req {
  pend_kind kind;
  uint32_t id;
  uint64_t deadline_ms;
  ccord_id channel_id;
} pending_req;

typedef struct stored_value {
  char key[129];
  char *json;
  size_t json_len;
} stored_value;

typedef struct plugin_exposed_fn {
  char name[65];
  bool allow_lower_class;
} plugin_exposed_fn;

typedef struct plugin {
  bool used;
  bool enabled;
  bool consent_granted;
  ccord_manifest m;                    // m.id is the owner key used everywhere
  char *perm_str;
  char *allowed_hosts_str;
  char *icon_path;
  char *dir;
  char *source;
  size_t source_len;
  ccord_plugin_state state;
  char error[256];
#if CCORD_OS_WINDOWS
  HANDLE proc;
  HANDLE job;
#elif !CCORD_OS_ANDROID
  pid_t pid;
#endif
  bool in_process;
  ccord_thread *worker;
  _Atomic bool cancel;
  _Atomic bool thread_exited;
  _Atomic int thread_exit_code;
  ccord_socket_t thread_fd;
  int (*thread_host_fn)(ccord_socket_t fd, const _Atomic bool *cancel);
  bool reaped;
  ccord_ipc *ipc;
  bool hello_seen;
  uint32_t next_id;
  pending_req pend[MAX_PENDING];
  uint64_t last_ping_sent_ms;
  uint64_t subs;
  size_t menu_count;
  stored_value *items;
  size_t nitems, cap_items;
  size_t store_bytes;
  bool storage_enabled, dirty;
  uint64_t last_save_ms;
  char *storage_dir;
  ccord_perm_set consented_perms;
  bool has_consented_hosts;
  char consented_hosts_csv[1024];
  bool is_bundled;
  bool revoked;
  // API v3 state
  uint8_t hook_mask;
  bool gw_raw_any;
  char **gw_raw_events;
  size_t ngw_raw_events;
  bool voice_pcm_sub;
  plugin_exposed_fn *exposed;
  size_t nexposed;
  int64_t dev_mtime_sec;
  uint64_t last_profile_write_ms;   // Class 3 account.profile rate limit (10s per plugin)
} plugin;

struct ccord_plugins {
  ccord_bus *bus;
  const ccord_store *store;
  ccord_session *session;
  ccord_contrib *contrib;
  void (*clipboard_write)(void *ud, const char *utf8);
  void *clipboard_ud;
  void (*toast_show)(void *ud, const char *utf8, uint8_t level);
  void *toast_ud;
  bool in_process;
  int (*in_process_host_fn)(ccord_socket_t fd, const _Atomic bool *cancel);
  char plat_os[24];
  char plat_arch[24];
  char plat_form_factor[16];
  bool plat_is_mobile;
  bool plat_touch;
  int plat_android_api_level;
  uint64_t plat_capabilities;
  uint32_t plat_bit;
  ccord_plugin_platform_state plat_state;
  ccord_plugin_platform_ops plat_ops;
  char *host_exe;
  char *data_dir;
  bool has_data_dir;
  char **search;
  size_t nsearch;
  plugin **plugins;
  size_t nplugins, capplugins;
  ccord_catalog *catalog;
  ccord_sub_id sub;
  uint64_t now_ms;
  bool in_background;
  // API v3
  plugin_workers *workers;
  bool dev_auto_reload;
  uint64_t last_dev_scan_ms;
  uint64_t last_account_rev;
  uint64_t import_seq;
  char last_imported_id[65];
};

// Shared helpers defined in plugins.c
bool pl_has_perm(const plugin *p, ccord_perm perm);
void pl_set_text(char *dst, size_t cap, const char *src);
void pl_reply_ok(ccord_plugins *m, plugin *p, uint32_t id, const char *result);
void pl_reply_err(ccord_plugins *m, plugin *p, uint32_t id, const char *code, const char *msg);
void pl_send_ntf(ccord_plugins *m, plugin *p, const char *method, const char *params);
void pl_fail_plugin(ccord_plugins *m, plugin *p, const char *reason);
plugin *pl_find_owner(ccord_plugins *m, const char *owner);
uint32_t pl_next_request_id(plugin *p);
const char *pl_str_param(const ccord_json *obj, const char *key, size_t *len);
ccord_id pl_parse_id_param(const ccord_json *params, const char *key);
void pl_jw_copy_json(ccord_jw *w, const ccord_json *j);
bool pl_valid_store_key(const char *s, size_t n);
bool pl_valid_menu_id(const char *s, size_t n);
bool pl_valid_plain_text(const char *s, size_t n, size_t max);
ccord_status pl_store_put(plugin *p, const char *key, const char *json, size_t jlen);
bool pl_fs_path_is_protected(const ccord_plugins *m, const char *raw_path);
void pl_fs_b64_encode(const unsigned char *in, size_t n, ccord_buf *out);
bool pl_fs_b64_decode(const unsigned char *in, size_t n, uint8_t *dst, size_t *out_len);
bool pl_resolve_real_path(const char *in, char *out, size_t cap);
bool pl_path_strictly_under(const char *real_path, const char *real_dir);
void pl_broadcast_event_json(ccord_plugins *m, uint64_t bit, const char *name, const char *data_json);
void pl_handle_frame(ccord_plugins *m, plugin *p, const ccord_ipc_msg *msg);
void pl_reload_plugin(ccord_plugins *m, const char *id);
const char *pl_menu_target_str(ccord_menu_target t);
bool pl_parse_menu_target(const char *s, ccord_menu_target *out);
bool pl_parse_panel_slot(const char *s, ccord_panel_slot *out);
void pl_write_menu_ctx_obj(ccord_jw *w, const ccord_menu_context *ctx);

// API v3 handlers & bridges defined in plugin_api_ext.c
bool plugins_ext_handle_request(ccord_plugins *m, plugin *p, const ccord_ipc_msg *msg);
bool plugins_ext_event_bit_for(const char *name, uint64_t *bit, ccord_perm *perm);
void plugins_ext_on_worker_event(void *ud, const pw_event *ev);
void plugins_ext_reset_plugin(plugin *p);
void plugins_ext_dev_poll(ccord_plugins *m, uint64_t now_ms);
void plugins_ext_write_message_obj(ccord_jw *w, const ccord_store *st, const ccord_message *msg);
void plugins_ext_format_allowed_hosts(const ccord_manifest *man, char *out, size_t cap);
void plugins_ext_handle_command_result(ccord_plugins *m, plugin *p, ccord_id channel, const ccord_json *result);

// API v3 ccord_contrib_handlers callbacks
size_t plugins_ext_on_command_complete(void *ud, const ccord_command_contribution *cmd, const char *args,
                                       ccord_id guild_id, ccord_id channel_id, ccord_autocomplete_item *out,
                                       size_t max_items);
void plugins_ext_on_decor_event(void *ud, const ccord_decorator_contribution *decor, ccord_id target_id,
                                const ccord_panel_event *ev);
void plugins_ext_on_local_msg_event(void *ud, const ccord_local_message *lm, const char *ev_type, float x, float y);
size_t plugins_ext_on_menu_resolve(void *ud, const char *owner, const char *const *item_ids, size_t item_count,
                                   const ccord_menu_context *ctx, uint32_t timeout_ms,
                                   ccord_menu_resolved_entry *out, size_t max_out);
void plugins_ext_on_setting_changed(void *ud, const char *owner, const char *key, const char *value_json);
void plugins_ext_on_keybind_triggered(void *ud, const char *owner, const char *bind_id);
void plugins_ext_on_action_btn_click(void *ud, const ccord_action_btn_contribution *btn, const ccord_menu_context *ctx);
void plugins_ext_on_dialog_result(void *ud, const char *owner, uint64_t req_id, bool accepted, const char *result_json);
void plugins_ext_on_account_hitl_result(void *ud, const char *owner, uint64_t req_id, bool approved, const char *err_msg);
void plugins_ext_poll_account_update(ccord_plugins *m);
size_t plugins_ext_on_autocomplete_query(void *ud, const char *owner, const char *provider_id, const char *query,
                                         ccord_id guild_id, ccord_id channel_id, ccord_autocomplete_item *out,
                                         size_t max_items);
bool plugins_ext_on_before_send(void *ud, ccord_id channel_id, ccord_id reply_to_id, const char *text,
                                char *out_text, size_t out_cap, char *cancel_reason, size_t reason_cap);
bool plugins_ext_on_before_edit(void *ud, ccord_id channel_id, ccord_id message_id, const char *text,
                                char *out_text, size_t out_cap, char *cancel_reason, size_t reason_cap);
bool plugins_ext_on_notification_hook(void *ud, ccord_id guild_id, ccord_id channel_id, ccord_id message_id,
                                      ccord_id author_id, const char *content, bool is_mention, bool is_dm,
                                      char *out_title, size_t title_cap, char *out_body, size_t body_cap,
                                      bool *out_silent);
bool plugins_ext_on_link_open_hook(void *ud, const char *url, char *out_url, size_t out_cap);

// Additional cross-TU declarations across core/ & package/
extern const ccord_json_limits pl_json_limits;
void pl_rm_dir_recursive(const char *path);
bool pl_app_raw(ccord_buf *b, const char *s);
bool pl_app_str(ccord_buf *b, const char *s, size_t n);
bool pl_valid_command_name(const char *s, size_t n);
void pl_publish_state(ccord_plugins *m, plugin *p);
bool pl_host_is_running(const plugin *p);
void pl_kill_host_process(plugin *p);
bool pl_reap_wait(plugin *p, uint32_t wait_ms, char *out, size_t cap);
void pl_set_plugin_failed(ccord_plugins *m, plugin *p, const char *reason, bool allow_from_stopping);
pending_req *pl_pend_slot(plugin *p, pend_kind kind);
pending_req *pl_pend_free(plugin *p);
stored_value *pl_store_find(plugin *p, const char *key);
void pl_store_remove_at(plugin *p, size_t i);
void pl_storage_load(plugin *p);
void pl_mkdir_ok(const char *path);
void pl_storage_save(ccord_plugins *m, plugin *p, uint64_t now, bool sync);
void pl_storage_setup(ccord_plugins *m, plugin *p);
void pl_format_hosts_csv(const ccord_manifest *man, char *out, size_t cap);
const ccord_json *pl_consent_parse_file(const ccord_plugins *m, ccord_arena *ar);
bool pl_consent_apply(const ccord_json *root, const char *id, const ccord_manifest *man,
                      bool *out_enabled, bool *out_consent, ccord_perm_set *out_consented_perms,
                      bool *out_has_consented_hosts, char *out_consented_hosts, size_t hosts_cap,
                      bool *out_migrated);
void pl_consent_save(const ccord_plugins *m);
ccord_status pl_spawn_host(ccord_plugins *m, plugin *p, ccord_error *err);
void pl_stop_host(ccord_plugins *m, plugin *p);
void pl_start_plugin_host(ccord_plugins *m, plugin *p);
bool pl_reload_plugin_source(plugin *p, char *why, size_t why_cap);
void pl_on_plugin_reload(void *ud, const char *id);
void pl_on_plugin_consent_accept_for(void *ud, const char *id, uint64_t expected_perms);
char *pl_resolve_icon_path(const char *real_dir, const char *rel_icon);
bool pl_import_package_internal(ccord_plugins *m, const char *pkg_path,
                                const char *expected_id, const char *expected_version,
                                uint64_t expected_perms, const char *expected_allowed_hosts,
                                bool from_catalog, bool preapprove_consent,
                                char *err_buf, size_t err_cap);
const char *pl_channel_kind_str(const ccord_store *st, const ccord_channel *c);
void pl_channel_display_name(const ccord_store *st, const ccord_channel *c, char *out, size_t cap);
bool pl_read_file(const char *path, size_t max, char **out, size_t *len, char *why, size_t why_cap);
