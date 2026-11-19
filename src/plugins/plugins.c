// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include <ccord/plugins/plugins.h>
#include "catalog.h"
#include "zip.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/config.h>
#include <ccord/core/input.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>
#include <ccord/ipc/ipc.h>
#include <ccord/store/notify.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if CCORD_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#if !CCORD_OS_ANDROID
#include <spawn.h>
#include <sys/wait.h>
#endif
#include <unistd.h>
#endif

#define PBUF (2 * CCORD_PATH_MAX + 320)   // composed paths: data_dir + plugin id + file name
#define MAX_SEARCH 16
#define MAX_PENDING 8
#define MAX_CLIPBOARD_BYTES 65536u
#define INIT_DEADLINE_MS 5000u
#define CMD_DEADLINE_MS 2000u
#define PING_INTERVAL_MS 1000u
#define PING_TIMEOUT_MS 5000u
#define SAVE_INTERVAL_MS 1000u
#define EXIT_WAIT_MS 300u
#define EVB_MESSAGE          (1u << 0)
#define EVB_GUILD            (1u << 1)
#define EVB_CHANNEL          (1u << 2)
#define EVB_VIEW_SELECT      (1u << 3)
#define EVB_NAV_AUX          (1u << 4)
#define EVB_CHANNEL_UPSERT   (1u << 5)
#define EVB_CHANNEL_DELETE   (1u << 6)
#define EVB_GUILDS_LOADED    (1u << 7)
#define EVB_NOTIFY_UPDATE    (1u << 8)
#define EVB_MESSAGE_UPDATE   (1u << 9)
#define EVB_MESSAGE_DELETE   (1u << 10)
#define EVB_MESSAGE_PIN      (1u << 11)
#define EVB_REACTION_UPDATE  (1u << 12)
#define EVB_TYPING_START     (1u << 13)
#define EVB_PRESENCE_UPDATE  (1u << 14)
#define EVB_VOICE_STATE      (1u << 15)
#define EVB_APP_READY        (1u << 16)
#define EVB_CONNECTION_STATE (1u << 17)
#define EVB_APP_QUIT         (1u << 18)
#define EVB_APP_LIFECYCLE    (1u << 19)
#define EVB_PLAT_ORIENTATION (1u << 20)
#define EVB_PLAT_KEYBOARD    (1u << 21)
#define EVB_PLAT_SAFE_AREA   (1u << 22)
#define EVB_VOICE_SERVER     (1u << 23)
#define EVB_STREAM_STATE     (1u << 24)

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

typedef struct plugin {
  bool used;
  bool enabled;
  bool consent_granted;
  ccord_manifest m;                    // m.id is the owner key used everywhere
  char perm_str[512];
  char dir[CCORD_PATH_MAX];
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
  uint32_t subs;
  size_t menu_count;
  stored_value *items;
  size_t nitems, cap_items;
  size_t store_bytes;
  bool storage_enabled, dirty;
  uint64_t last_save_ms;
  char storage_dir[PBUF];
  ccord_perm_set consented_perms;
  bool is_bundled;
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
  char host_exe[CCORD_PATH_MAX];
  char data_dir[CCORD_PATH_MAX];
  bool has_data_dir;
  char search[MAX_SEARCH][CCORD_PATH_MAX];
  size_t nsearch;
  plugin *plugins;
  size_t nplugins, capplugins;
  ccord_catalog *catalog;
  ccord_sub_id sub;
  uint64_t now_ms;
};

static const ccord_perm k_all_perms[] = {
  CCORD_PERM_EVENTS_NAVIGATION, CCORD_PERM_UI_CONTEXTMENU, CCORD_PERM_CLIPBOARD_WRITE,
  CCORD_PERM_STORAGE, CCORD_PERM_COMMANDS,
  CCORD_PERM_EVENTS_MESSAGES, CCORD_PERM_EVENTS_PRESENCE, CCORD_PERM_EVENTS_VOICE,
  CCORD_PERM_EVENTS_CHANNELS, CCORD_PERM_STORE_READ, CCORD_PERM_MESSAGES_SEND,
  CCORD_PERM_MESSAGES_MANAGE, CCORD_PERM_MESSAGES_HOOK, CCORD_PERM_PRESENCE_WRITE,
  CCORD_PERM_NAVIGATION_WRITE, CCORD_PERM_VOICE_CONTROL, CCORD_PERM_UI_PANEL,
  CCORD_PERM_UI_COMPOSER, CCORD_PERM_CLIPBOARD_READ, CCORD_PERM_GATEWAY_RAW,
  CCORD_PERM_FS_READ, CCORD_PERM_FS_WRITE, CCORD_PERM_FS_WATCH, CCORD_PERM_NETWORK_HTTP,
  CCORD_PERM_SYSTEM_NOTIFY, CCORD_PERM_SYSTEM_OPEN, CCORD_PERM_SYSTEM_INFO, CCORD_PERM_SYSTEM_EXEC,
};

static const ccord_json_limits k_json_limits = {16, 8192, (size_t)1u << 20};

static void rm_dir_recursive(const char *path)
{
  ccord_error err = {0};
  (void)ccord_rmtree(path, &err);
}

static void set_text(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  const char *s = src ? src : "";
  size_t n = strlen(s);
  if (n >= cap) n = cap - 1u;
  memcpy(dst, s, n);
  dst[n] = '\0';
}

static bool has_perm(const plugin *p, ccord_perm perm)
{
  return (p->m.perms & (ccord_perm_set)perm) != 0;
}

static bool app_raw(ccord_buf *b, const char *s)
{
  return ccord_buf_append_str(b, s) == CCORD_OK;
}

static bool app_str(ccord_buf *b, const char *s, size_t n)
{
  ccord_jw w;
  ccord_jw_init(&w, b);
  ccord_jw_string(&w, s, n);
  return ccord_jw_ok(&w);
}

// ---------------------------------------------------------------- validation (main is authoritative)

static bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool valid_store_key(const char *s, size_t n)
{
  if (n == 0 || n > 128) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '_' || c == '.' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static bool valid_menu_id(const char *s, size_t n)
{
  if (n == 0 || n > 32 || !(is_lower(s[0]) || is_digit(s[0]))) return false;
  for (size_t i = 1; i < n; i++) {
    if (!(is_lower(s[i]) || is_digit(s[i]) || s[i] == '_' || s[i] == '-')) return false;
  }
  return true;
}

static bool valid_command_name(const char *s, size_t n)
{
  if (n == 0 || n > 32 || !is_lower(s[0])) return false;
  for (size_t i = 1; i < n; i++) {
    if (!(is_lower(s[i]) || is_digit(s[i]) || s[i] == '_' || s[i] == '-')) return false;
  }
  return true;
}

// Valid UTF-8 without control characters (label, description).
static bool valid_plain_text(const char *s, size_t n, size_t max)
{
  if (n == 0 || n > max || !ccord_utf8_valid(s, n)) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

// ---------------------------------------------------------------- state publication

static void publish_state(ccord_plugins *m, plugin *p)
{
  ccord_catalog_local_overlay ov = {0};
  bool has_ov = m->catalog && ccord_catalog_query_overlay(m->catalog, p->m.id, p->m.version, p->consented_perms, &ov);
  ccord_plugin_tier tier = p->is_bundled ? CCORD_TIER_OFFICIAL : (has_ov ? ov.tier : CCORD_TIER_UNKNOWN);
  ccord_plugin_info info = {
    .id = p->m.id, .name = p->m.name, .version = p->m.version,
    .description = p->m.description,
    .permissions = p->perm_str,
    .risk_class = (uint8_t)ccord_perm_max_class(p->m.perms),
    .tier = tier,
    .is_bundled = p->is_bundled,
    .auto_update = has_ov ? ov.auto_update : true,
    .enabled = p->enabled,
    .consent_granted = p->consent_granted,
    .update_needs_review = has_ov ? ov.update_needs_review : false,
    .update_version = has_ov ? ov.update_version : "",
    .warning_sev = has_ov ? ov.warning_sev : CCORD_WARN_NONE,
    .warning_title = has_ov ? ov.warning_title : "",
    .warning_message = has_ov ? ov.warning_message : "",
    .state = p->state, .error = p->error,
  };
  ccord_error err = {0};
  if (ccord_contrib_set_plugin(m->contrib, &info, &err) != CCORD_OK) {
    CCORD_LOG_W("plugin %s: contribution registry: %s", p->m.id, err.message);
  }
  if (m->bus) {
    ccord_event ev = {.type = CCORD_EVT_PLUGIN_STATE, .value = (int64_t)p->state};
    (void)ccord_bus_post(m->bus, ev);
  }
}

static void format_exit_code(int code, char *out, size_t cap)
{
  switch (code) {
    case 0: set_text(out, cap, "host exited"); break;
    case 64: set_text(out, cap, "host usage error"); break;
    case 70: set_text(out, cap, "host internal error"); break;
    case 71: set_text(out, cap, "host protocol violation"); break;
    case 72: set_text(out, cap, "host out of memory at startup"); break;
    case 73: set_text(out, cap, "instruction limit exceeded"); break;
    case 74: set_text(out, cap, "memory limit exceeded"); break;
    case 75: set_text(out, cap, "undeclared restricted API called"); break;
    default: snprintf(out, cap, "host exited with code %d", code); break;
  }
}

static bool host_is_running(const plugin *p)
{
  if (p->in_process) return p->worker != nullptr && !p->reaped;
#if CCORD_OS_WINDOWS
  return p->proc != nullptr && !p->reaped;
#elif CCORD_OS_ANDROID
  return false;
#else
  return p->pid > 0 && !p->reaped;
#endif
}

static void kill_host_process(plugin *p)
{
  if (!host_is_running(p)) return;
  if (p->in_process) {
    atomic_store_explicit(&p->cancel, true, memory_order_release);
    if (p->ipc) {
      ccord_ipc_close(p->ipc);
      p->ipc = nullptr;
    }
    ccord_thread_join(p->worker);
    p->worker = nullptr;
    p->reaped = true;
    return;
  }
#if CCORD_OS_WINDOWS
  (void)TerminateProcess(p->proc, 1);
  (void)WaitForSingleObject(p->proc, 2000);
  CloseHandle(p->proc);
  p->proc = nullptr;
  if (p->job) {
    CloseHandle(p->job);
    p->job = nullptr;
  }
#elif !CCORD_OS_ANDROID
  (void)kill(p->pid, SIGKILL);
  int st = 0;
  while (waitpid(p->pid, &st, 0) < 0 && errno == EINTR) {
  }
#endif
  p->reaped = true;
}

// Reaps the child if it has exited within wait_ms. Fills a human reason on exit.
static bool reap_wait(plugin *p, uint32_t wait_ms, char *out, size_t cap)
{
  if (!host_is_running(p)) return true;
  if (p->in_process) {
    uint32_t waited = 0;
    for (;;) {
      if (atomic_load_explicit(&p->thread_exited, memory_order_acquire)) {
        ccord_thread_join(p->worker);
        p->worker = nullptr;
        p->reaped = true;
        format_exit_code(atomic_load_explicit(&p->thread_exit_code, memory_order_relaxed), out, cap);
        return true;
      }
      if (waited >= wait_ms) return false;
      ccord_sleep_ms(5);
      waited += 5;
    }
  }
#if CCORD_OS_WINDOWS
  DWORD wr = WaitForSingleObject(p->proc, (DWORD)wait_ms);
  if (wr == WAIT_TIMEOUT) return false;
  DWORD code = 0;
  if (GetExitCodeProcess(p->proc, &code)) format_exit_code((int)code, out, cap);
  else set_text(out, cap, "host ended");
  CloseHandle(p->proc);
  p->proc = nullptr;
  if (p->job) {
    CloseHandle(p->job);
    p->job = nullptr;
  }
  p->reaped = true;
  return true;
#elif CCORD_OS_ANDROID
  p->reaped = true;
  set_text(out, cap, "host ended");
  return true;
#else
  uint32_t waited = 0;
  for (;;) {
    int st = 0;
    pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == p->pid) {
      p->reaped = true;
      if (WIFEXITED(st)) {
        format_exit_code(WEXITSTATUS(st), out, cap);
      } else if (WIFSIGNALED(st)) {
        snprintf(out, cap, "host killed by signal %d", WTERMSIG(st));
      } else {
        set_text(out, cap, "host ended");
      }
      return true;
    }
    if (r < 0 && errno != EINTR) {
      p->reaped = true;
      set_text(out, cap, "host process lost");
      return true;
    }
    if (waited >= wait_ms) return false;
    ccord_sleep_ms(5);
    waited += 5;
  }
#endif
}

// Moves a plugin to FAILED: kills the host, drops its contributions, announces the state.
static void fail_plugin(ccord_plugins *m, plugin *p, const char *reason)
{
  if (p->state == CCORD_PLUGIN_FAILED || p->state == CCORD_PLUGIN_STOPPED) return;
  kill_host_process(p);
  if (p->ipc) {
    ccord_ipc_close(p->ipc);
    p->ipc = nullptr;
  }
  p->state = CCORD_PLUGIN_FAILED;
  set_text(p->error, sizeof p->error, reason);
  memset(p->pend, 0, sizeof p->pend);
  ccord_contrib_remove_owner(m->contrib, p->m.id);
  CCORD_LOG_E("plugin %s FAILED: %s", p->m.id, p->error);
  publish_state(m, p);
  ccord_contrib_add_plugin_log(m->contrib, p->m.id, m->now_ms, (uint8_t)CCORD_LOG_ERROR, p->error);
}

// Sends a notification; any send failure fails the plugin.
static void send_ntf(ccord_plugins *m, plugin *p, const char *method, const char *params)
{
  if (!p->ipc) return;
  ccord_error err = {0};
  if (ccord_ipc_send_notify(p->ipc, method, params, &err) != CCORD_OK) {
    char why[256];
    snprintf(why, sizeof why, "could not send to host: %s", err.message);
    fail_plugin(m, p, why);
    return;
  }
  (void)ccord_ipc_flush(p->ipc, 0, &err);
}

static pending_req *pend_slot(plugin *p, pend_kind kind)
{
  for (size_t i = 0; i < MAX_PENDING; i++) {
    if (p->pend[i].kind == kind) return &p->pend[i];
  }
  return nullptr;
}

static pending_req *pend_free(plugin *p)
{
  for (size_t i = 0; i < MAX_PENDING; i++) {
    if (p->pend[i].kind == PEND_NONE) return &p->pend[i];
  }
  return nullptr;
}

static uint32_t next_request_id(plugin *p)
{
  if (p->next_id == 0) p->next_id = 1;
  return p->next_id++;
}

// ---------------------------------------------------------------- storage (main owns the values)

static stored_value *store_find(plugin *p, const char *key)
{
  for (size_t i = 0; i < p->nitems; i++) {
    if (strcmp(p->items[i].key, key) == 0) return &p->items[i];
  }
  return nullptr;
}

static void store_remove_at(plugin *p, size_t i)
{
  p->store_bytes -= strlen(p->items[i].key) + p->items[i].json_len;
  ccord_free(p->items[i].json);
  memmove(&p->items[i], &p->items[i + 1], (p->nitems - i - 1) * sizeof p->items[0]);
  p->nitems--;
}

static ccord_status store_put(plugin *p, const char *key, const char *json, size_t jlen)
{
  stored_value *old = store_find(p, key);
  if (old) {
    p->store_bytes -= strlen(old->key) + old->json_len;
    ccord_free(old->json);
    old->json = ccord_malloc(jlen + 1);
    if (!old->json) return CCORD_ERR_NOMEM;
    memcpy(old->json, json, jlen);
    old->json[jlen] = '\0';
    old->json_len = jlen;
    p->store_bytes += strlen(old->key) + jlen;
    p->dirty = true;
    return CCORD_OK;
  }
  if (p->nitems == p->cap_items) {
    size_t cap = p->cap_items ? p->cap_items * 2 : 8;
    stored_value *n = ccord_realloc(p->items, cap * sizeof *n);
    if (!n) return CCORD_ERR_NOMEM;
    p->items = n;
    p->cap_items = cap;
  }
  stored_value *v = &p->items[p->nitems];
  memset(v, 0, sizeof *v);
  snprintf(v->key, sizeof v->key, "%s", key);
  v->json = ccord_malloc(jlen + 1);
  if (!v->json) return CCORD_ERR_NOMEM;
  memcpy(v->json, json, jlen);
  v->json[jlen] = '\0';
  v->json_len = jlen;
  p->nitems++;
  p->store_bytes += strlen(key) + jlen;
  p->dirty = true;
  return CCORD_OK;
}

static void jw_copy_json(ccord_jw *w, const ccord_json *j);

static void storage_load(plugin *p)
{
  if (!p->storage_enabled) return;
  char path[PBUF + 32];
  int n = snprintf(path, sizeof path, "%s/storage.json", p->storage_dir);
  if (n < 0 || (size_t)n >= sizeof path) {
    CCORD_LOG_W("plugin %s: storage file path too long", p->m.id);
    return;
  }
  if (!ccord_path_exists(path)) return;
  uint64_t fsz = 0;
  if (!ccord_path_is_file(path) || !ccord_file_size(path, &fsz) || fsz > (uint64_t)(16u << 20)) {
    CCORD_LOG_W("plugin %s: storage file rejected (size or type)", p->m.id);
    return;
  }
  FILE *f = fopen(path, "rb");
  if (!f) {
    CCORD_LOG_W("plugin %s: storage not loaded: %s", p->m.id, strerror(errno));
    return;
  }
  size_t len = (size_t)fsz;
  char *buf = ccord_malloc(len + 1);
  if (!buf) {
    fclose(f);
    return;
  }
  size_t got = len ? fread(buf, 1, len, f) : 0;
  fclose(f);
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error err = {0};
  const ccord_json *root = got == len ? ccord_json_parse(&arena, buf, len, &k_json_limits, &err) : nullptr;
  bool ok = root && ccord_json_type_of(root) == CCORD_JSON_OBJECT;
  for (size_t i = 0; ok && i < ccord_json_object_len(root); i++) {
    const ccord_json *v = nullptr;
    const char *k = ccord_json_object_key(root, i, &v);
    ok = v != nullptr && valid_store_key(k, strlen(k));
    if (!ok) break;
    ccord_buf b;
    ccord_buf_init(&b, 0);
    ccord_jw w;
    ccord_jw_init(&w, &b);
    jw_copy_json(&w, v);
    if (ccord_jw_ok(&w) && store_put(p, k, ccord_buf_cstr(&b), b.len) == CCORD_OK) {
      p->dirty = false;
    } else {
      ok = false;
    }
    ccord_buf_free(&b);
  }
  if (!ok) CCORD_LOG_W("plugin %s: storage file is not valid, starting empty", p->m.id);
  if (!ok) {
    for (size_t i = p->nitems; i > 0; i--) store_remove_at(p, i - 1);
  }
  ccord_arena_free(&arena);
  ccord_free(buf);
}

static void mkdir_ok(const char *path)
{
  ccord_error err = {0};
  if (ccord_mkdir_p(path, 0700, &err) != CCORD_OK) CCORD_LOG_W("storage: mkdir %s: %s", path, err.message);
}

static void storage_save(ccord_plugins *m, plugin *p, uint64_t now)
{
  if (!p->storage_enabled || !p->dirty) return;
  ccord_buf b;
  ccord_buf_init(&b, 0);
  bool ok = app_raw(&b, "{");
  for (size_t i = 0; ok && i < p->nitems; i++) {
    if (i) ok = app_raw(&b, ",");
    ok = ok && app_str(&b, p->items[i].key, strlen(p->items[i].key)) && app_raw(&b, ":") &&
         ccord_buf_append(&b, p->items[i].json, p->items[i].json_len) == CCORD_OK;
  }
  ok = ok && app_raw(&b, "}");
  if (!ok) {
    CCORD_LOG_W("plugin %s: storage encoding failed", p->m.id);
    ccord_buf_free(&b);
    return;
  }
  mkdir_ok(m->data_dir);
  char plugins_dir[PBUF];
  int n1 = snprintf(plugins_dir, sizeof plugins_dir, "%s/plugins", m->data_dir);
  if (n1 < 0 || (size_t)n1 >= sizeof plugins_dir) {
    CCORD_LOG_W("plugin %s: plugins directory path too long", p->m.id);
    ccord_buf_free(&b);
    return;
  }
  mkdir_ok(plugins_dir);
  mkdir_ok(p->storage_dir);
  char final_path[PBUF + 32];
  int n3 = snprintf(final_path, sizeof final_path, "%s/storage.json", p->storage_dir);
  if (n3 < 0 || (size_t)n3 >= sizeof final_path) {
    CCORD_LOG_W("plugin %s: storage file path too long", p->m.id);
    ccord_buf_free(&b);
    return;
  }
  ccord_error werr = {0};
  if (ccord_file_write_atomic(final_path, b.data, b.len, 0600, true, &werr) != CCORD_OK) {
    CCORD_LOG_W("plugin %s: storage save failed: %s", p->m.id, werr.message);
  } else {
    p->dirty = false;
    p->last_save_ms = now;
  }
  ccord_buf_free(&b);
}

// ---------------------------------------------------------------- host -> main requests

static void reply_err(ccord_plugins *m, plugin *p, uint32_t id, const char *code, const char *msg)
{
  if (!p->ipc) return;
  ccord_error err = {0};
  if (ccord_ipc_send_error(p->ipc, id, code, msg, &err) != CCORD_OK) fail_plugin(m, p, "could not reply to host");
}

static void reply_ok(ccord_plugins *m, plugin *p, uint32_t id, const char *result)
{
  if (!p->ipc) return;
  ccord_error err = {0};
  if (ccord_ipc_send_ok(p->ipc, id, result, &err) != CCORD_OK) fail_plugin(m, p, "could not reply to host");
}

static void jw_copy_json(ccord_jw *w, const ccord_json *j)
{
  if (!j) { ccord_jw_null(w); return; }
  switch (ccord_json_type_of(j)) {
    case CCORD_JSON_NULL: ccord_jw_null(w); break;
    case CCORD_JSON_BOOL: ccord_jw_bool(w, ccord_json_bool(j, false)); break;
    case CCORD_JSON_NUMBER: ccord_jw_double(w, ccord_json_number(j, 0)); break;
    case CCORD_JSON_STRING: {
      size_t sl = 0;
      const char *s = ccord_json_string(j, &sl);
      ccord_jw_string(w, s ? s : "", sl);
      break;
    }
    case CCORD_JSON_ARRAY: {
      ccord_jw_begin_array(w);
      size_t n = ccord_json_array_len(j);
      for (size_t i = 0; i < n; i++) jw_copy_json(w, ccord_json_array_at(j, i));
      ccord_jw_end_array(w);
      break;
    }
    case CCORD_JSON_OBJECT: {
      ccord_jw_begin_object(w);
      size_t n = ccord_json_object_len(j);
      for (size_t i = 0; i < n; i++) {
        const ccord_json *v = nullptr;
        const char *k = ccord_json_object_key(j, i, &v);
        ccord_jw_key(w, k);
        jw_copy_json(w, v);
      }
      ccord_jw_end_object(w);
      break;
    }
  }
}

static bool event_bit_for(const char *name, uint32_t *bit, ccord_perm *perm)
{
  // Class 1 (Safe) events
  if (strcmp(name, "app.ready") == 0) { *bit = EVB_APP_READY; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "app.quit") == 0) { *bit = EVB_APP_QUIT; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "app.lifecycle") == 0) { *bit = EVB_APP_LIFECYCLE; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "connection.state") == 0) { *bit = EVB_CONNECTION_STATE; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "guild.select") == 0) { *bit = EVB_GUILD; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "channel.select") == 0) { *bit = EVB_CHANNEL; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "view.select") == 0) { *bit = EVB_VIEW_SELECT; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "navigation.aux_open") == 0) { *bit = EVB_NAV_AUX; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "platform.orientation") == 0) { *bit = EVB_PLAT_ORIENTATION; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "platform.keyboard") == 0) { *bit = EVB_PLAT_KEYBOARD; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "platform.safe_area") == 0) { *bit = EVB_PLAT_SAFE_AREA; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }

  // Class 2 (Unsafe) events
  if (strcmp(name, "message.create") == 0) { *bit = EVB_MESSAGE; *perm = CCORD_PERM_EVENTS_MESSAGES; return true; }
  if (strcmp(name, "message.update") == 0) { *bit = EVB_MESSAGE_UPDATE; *perm = CCORD_PERM_EVENTS_MESSAGES; return true; }
  if (strcmp(name, "message.delete") == 0) { *bit = EVB_MESSAGE_DELETE; *perm = CCORD_PERM_EVENTS_MESSAGES; return true; }
  if (strcmp(name, "message.pin") == 0) { *bit = EVB_MESSAGE_PIN; *perm = CCORD_PERM_EVENTS_MESSAGES; return true; }
  if (strcmp(name, "reaction.update") == 0) { *bit = EVB_REACTION_UPDATE; *perm = CCORD_PERM_EVENTS_MESSAGES; return true; }
  if (strcmp(name, "typing.start") == 0) { *bit = EVB_TYPING_START; *perm = CCORD_PERM_EVENTS_MESSAGES; return true; }
  if (strcmp(name, "presence.update") == 0) { *bit = EVB_PRESENCE_UPDATE; *perm = CCORD_PERM_EVENTS_PRESENCE; return true; }
  if (strcmp(name, "voice.state") == 0) { *bit = EVB_VOICE_STATE; *perm = CCORD_PERM_EVENTS_VOICE; return true; }
  if (strcmp(name, "voice.server") == 0) { *bit = EVB_VOICE_SERVER; *perm = CCORD_PERM_EVENTS_VOICE; return true; }
  if (strcmp(name, "stream.state") == 0) { *bit = EVB_STREAM_STATE; *perm = CCORD_PERM_EVENTS_VOICE; return true; }
  if (strcmp(name, "channel.upsert") == 0) { *bit = EVB_CHANNEL_UPSERT; *perm = CCORD_PERM_EVENTS_CHANNELS; return true; }
  if (strcmp(name, "channel.delete") == 0) { *bit = EVB_CHANNEL_DELETE; *perm = CCORD_PERM_EVENTS_CHANNELS; return true; }
  if (strcmp(name, "guilds.loaded") == 0) { *bit = EVB_GUILDS_LOADED; *perm = CCORD_PERM_EVENTS_CHANNELS; return true; }
  if (strcmp(name, "notify.update") == 0) { *bit = EVB_NOTIFY_UPDATE; *perm = CCORD_PERM_EVENTS_CHANNELS; return true; }
  return false;
}

static const char *str_param(const ccord_json *obj, const char *key, size_t *len)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  return j ? ccord_json_string(j, len) : nullptr;
}

static void req_storage(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params)
{
  ccord_buf out;
  ccord_buf_init(&out, 0);
  size_t kl = 0;
  const char *key = str_param(params, "key", &kl);
  if (strcmp(method, "storage.keys") == 0) {
    // Sorted key list.
    size_t n = p->nitems;
    const char **names = ccord_calloc(n ? n : 1, sizeof *names);
    if (!names) {
      reply_err(m, p, id, "internal", "out of memory");
      ccord_buf_free(&out);
      return;
    }
    for (size_t i = 0; i < n; i++) names[i] = p->items[i].key;
    for (size_t i = 1; i < n; i++) {   // insertion sort: at most 128 keys
      const char *x = names[i];
      size_t j = i;
      while (j > 0 && strcmp(names[j - 1], x) > 0) {
        names[j] = names[j - 1];
        j--;
      }
      names[j] = x;
    }
    bool ok = app_raw(&out, "{\"keys\":[");
    for (size_t i = 0; ok && i < n; i++) {
      if (i) ok = app_raw(&out, ",");
      ok = ok && app_str(&out, names[i], strlen(names[i]));
    }
    ok = ok && app_raw(&out, "]}");
    if (ok) reply_ok(m, p, id, ccord_buf_cstr(&out));
    else reply_err(m, p, id, "internal", "could not encode keys");
    ccord_free(names);
    ccord_buf_free(&out);
    return;
  }
  if (strcmp(method, "storage.clear") == 0) {
    for (size_t i = 0; i < p->nitems; i++) ccord_free(p->items[i].json);
    p->nitems = 0;
    p->store_bytes = 0;
    p->dirty = true;
    reply_ok(m, p, id, nullptr);
    ccord_buf_free(&out);
    return;
  }
  if (strcmp(method, "storage.set") == 0) {
    const ccord_json *vj = params ? ccord_json_get(params, "value") : nullptr;
    if (!key || !valid_store_key(key, kl)) {
      reply_err(m, p, id, "invalid_argument", "storage key must match ^[A-Za-z0-9_.-]{1,128}$");
    } else if (!vj) {
      reply_err(m, p, id, "invalid_argument", "storage value is required");
    } else {
      ccord_jw w;
      ccord_jw_init(&w, &out);
      jw_copy_json(&w, vj);
      if (!ccord_jw_ok(&w)) {
        reply_err(m, p, id, "invalid_argument", "storage value could not be encoded");
      } else if (store_put(p, key, ccord_buf_cstr(&out), out.len) != CCORD_OK) {
        reply_err(m, p, id, "internal", "out of memory");
      } else {
        reply_ok(m, p, id, nullptr);
      }
    }
    ccord_buf_free(&out);
    return;
  }
  if (!key || !valid_store_key(key, kl)) {
    reply_err(m, p, id, "invalid_argument", "storage key must match ^[A-Za-z0-9_.-]{1,64}$");
    ccord_buf_free(&out);
    return;
  }
  stored_value *v = store_find(p, key);
  if (strcmp(method, "storage.get") == 0) {
    bool ok = v ? (app_raw(&out, "{\"found\":true,\"value\":") &&
                   ccord_buf_append(&out, v->json, v->json_len) == CCORD_OK && app_raw(&out, "}"))
                : app_raw(&out, "{\"found\":false}");
    if (ok) reply_ok(m, p, id, ccord_buf_cstr(&out));
    else reply_err(m, p, id, "internal", "could not encode value");
  } else {   // storage.delete
    bool existed = v != nullptr;
    if (v) {
      size_t i = (size_t)(v - p->items);
      store_remove_at(p, i);
      p->dirty = true;
    }
    reply_ok(m, p, id, existed ? "{\"existed\":true}" : "{\"existed\":false}");
  }
  ccord_buf_free(&out);
}

static void req_menu_add(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t il = 0, ll = 0, tl = 0;
  const char *item = str_param(params, "id", &il);
  const char *label = str_param(params, "label", &ll);
  const char *target = str_param(params, "target", &tl);
  if (!item || !valid_menu_id(item, il)) { reply_err(m, p, id, "invalid_argument", "menu id must match ^[a-z0-9][a-z0-9_-]{0,31}$"); return; }
  if (!label || !valid_plain_text(label, ll, 64)) { reply_err(m, p, id, "invalid_argument", "label must be 1-64 bytes of text"); return; }
  ccord_menu_target t;
  if (!target) { reply_err(m, p, id, "invalid_argument", "target is required"); return; }
  if (strcmp(target, "message") == 0) t = CCORD_MENU_MESSAGE;
  else if (strcmp(target, "channel") == 0) t = CCORD_MENU_CHANNEL;
  else if (strcmp(target, "guild") == 0) t = CCORD_MENU_GUILD;
  else if (strcmp(target, "member") == 0) t = CCORD_MENU_MEMBER;
  else if (strcmp(target, "nav") == 0) t = CCORD_MENU_NAV;
  else { reply_err(m, p, id, "invalid_argument", "target must be message, channel, guild, member or nav"); return; }
  char item_id[40];
  snprintf(item_id, sizeof item_id, "%.*s", (int)il, item);
  char label_s[80];
  snprintf(label_s, sizeof label_s, "%.*s", (int)ll, label);
  ccord_error err = {0};
  if (ccord_contrib_add_menu(m->contrib, p->m.id, item_id, label_s, t, &err) != CCORD_OK) {
    { reply_err(m, p, id, "invalid_argument", err.message); return; }
  }
  p->menu_count++;
  reply_ok(m, p, id, nullptr);
}

static void req_menu_remove(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t il = 0;
  const char *item = str_param(params, "id", &il);
  if (!item || !valid_menu_id(item, il)) {
    reply_err(m, p, id, "invalid_argument", "menu id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
    return;
  }
  char item_id[40];
  snprintf(item_id, sizeof item_id, "%.*s", (int)il, item);
  if (ccord_contrib_remove_menu(m->contrib, p->m.id, item_id) && p->menu_count > 0) p->menu_count--;
  reply_ok(m, p, id, nullptr);
}

static bool parse_panel_slot(const char *s, ccord_panel_slot *out)
{
  if (!s) return false;
  if (strcmp(s, "top") == 0) { *out = CCORD_PANEL_TOP; return true; }
  if (strcmp(s, "subheader") == 0) { *out = CCORD_PANEL_SUBHEADER; return true; }
  if (strcmp(s, "bottom") == 0) { *out = CCORD_PANEL_BOTTOM; return true; }
  return false;
}

static void req_panel_mount(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t il = 0, sl = 0;
  const char *pid = str_param(params, "id", &il);
  const char *slot_s = str_param(params, "slot", &sl);
  const ccord_json *hj = params ? ccord_json_get(params, "height") : nullptr;
  const ccord_json *root = params ? ccord_json_get(params, "root") : nullptr;
  if (!pid || !valid_menu_id(pid, il)) {
    reply_err(m, p, id, "invalid_argument", "panel id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
    return;
  }
  ccord_panel_slot slot = CCORD_PANEL_TOP;
  if (slot_s && !parse_panel_slot(slot_s, &slot)) {
    reply_err(m, p, id, "invalid_argument", "panel slot must be top, subheader or bottom");
    return;
  }
  double h = hj ? ccord_json_number(hj, 34.0) : 34.0;
  if (!(h >= 12.0 && h <= 240.0)) {
    reply_err(m, p, id, "invalid_argument", "panel height must be between 12 and 240");
    return;
  }
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    reply_err(m, p, id, "invalid_argument", "panel root must be an object");
    return;
  }
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  jw_copy_json(&w, root);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&b);
    reply_err(m, p, id, "invalid_argument", "panel root could not be encoded");
    return;
  }
  char panel_id[40];
  snprintf(panel_id, sizeof panel_id, "%.*s", (int)il, pid);
  ccord_error err = {0};
  ccord_status st = ccord_contrib_set_panel(m->contrib, p->m.id, panel_id, slot, (float)h, ccord_buf_cstr(&b), &err);
  ccord_buf_free(&b);
  if (st != CCORD_OK) {
    reply_err(m, p, id, "invalid_argument", err.message);
    return;
  }
  reply_ok(m, p, id, nullptr);
}

static void req_panel_update(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t il = 0, sl = 0;
  const char *pid = str_param(params, "id", &il);
  if (!pid || !valid_menu_id(pid, il)) {
    reply_err(m, p, id, "invalid_argument", "panel id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
    return;
  }
  char panel_id[40];
  snprintf(panel_id, sizeof panel_id, "%.*s", (int)il, pid);
  const ccord_panel_contribution *cur = ccord_contrib_find_panel(m->contrib, p->m.id, panel_id);
  if (!cur) {
    reply_err(m, p, id, "not_found", "panel is not mounted");
    return;
  }
  ccord_panel_slot slot = cur->slot;
  const char *slot_s = str_param(params, "slot", &sl);
  if (slot_s && !parse_panel_slot(slot_s, &slot)) {
    reply_err(m, p, id, "invalid_argument", "panel slot must be top, subheader or bottom");
    return;
  }
  float height = cur->height;
  const ccord_json *hj = params ? ccord_json_get(params, "height") : nullptr;
  if (hj && ccord_json_type_of(hj) == CCORD_JSON_NUMBER) {
    double h = ccord_json_number(hj, (double)height);
    if (!(h >= 12.0 && h <= 240.0)) {
      reply_err(m, p, id, "invalid_argument", "panel height must be between 12 and 240");
      return;
    }
    height = (float)h;
  }
  const ccord_json *root = params ? ccord_json_get(params, "root") : nullptr;
  ccord_buf b;
  ccord_buf_init(&b, 0);
  const char *root_str = cur->root_json;
  if (root) {
    if (ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
      ccord_buf_free(&b);
      reply_err(m, p, id, "invalid_argument", "panel root must be an object");
      return;
    }
    ccord_jw w;
    ccord_jw_init(&w, &b);
    jw_copy_json(&w, root);
    if (!ccord_jw_ok(&w)) {
      ccord_buf_free(&b);
      reply_err(m, p, id, "invalid_argument", "panel root could not be encoded");
      return;
    }
    root_str = ccord_buf_cstr(&b);
  }
  ccord_error err = {0};
  ccord_status st = ccord_contrib_set_panel(m->contrib, p->m.id, panel_id, slot, height, root_str, &err);
  ccord_buf_free(&b);
  if (st != CCORD_OK) {
    reply_err(m, p, id, "invalid_argument", err.message);
    return;
  }
  reply_ok(m, p, id, nullptr);
}

static void req_panel_unmount(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t il = 0;
  const char *pid = str_param(params, "id", &il);
  if (!pid || !valid_menu_id(pid, il)) {
    reply_err(m, p, id, "invalid_argument", "panel id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
    return;
  }
  char panel_id[40];
  snprintf(panel_id, sizeof panel_id, "%.*s", (int)il, pid);
  (void)ccord_contrib_remove_panel(m->contrib, p->m.id, panel_id);
  reply_ok(m, p, id, nullptr);
}

static void req_clipboard(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t len = 0;
  const char *text = str_param(params, "text", &len);
  if (!text || len > MAX_CLIPBOARD_BYTES || !ccord_utf8_valid_text(text, len)) {
    { reply_err(m, p, id, "invalid_argument", "clipboard text must be valid UTF-8 text up to 65536 bytes"); return; }
  }
  if (!m->clipboard_write) { reply_err(m, p, id, "unsupported", "clipboard is not available"); return; }
  char *copy = ccord_strndup(text, len);
  if (!copy) { reply_err(m, p, id, "internal", "out of memory"); return; }
  m->clipboard_write(m->clipboard_ud, copy);
  ccord_free(copy);
  reply_ok(m, p, id, nullptr);
}

static void req_commands_register(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t nl = 0, dl = 0;
  const char *name = str_param(params, "name", &nl);
  const char *desc = str_param(params, "description", &dl);
  if (!name || !valid_command_name(name, nl)) { reply_err(m, p, id, "invalid_argument", "command name must match ^[a-z][a-z0-9_-]{0,31}$"); return; }
  if (!desc || !valid_plain_text(desc, dl, 100)) { reply_err(m, p, id, "invalid_argument", "description must be 1-100 bytes of text"); return; }
  char name_s[40], desc_s[104];
  snprintf(name_s, sizeof name_s, "%.*s", (int)nl, name);
  snprintf(desc_s, sizeof desc_s, "%.*s", (int)dl, desc);
  ccord_error err = {0};
  if (ccord_contrib_add_command(m->contrib, p->m.id, name_s, desc_s, &err) != CCORD_OK) {
    { reply_err(m, p, id, "invalid_argument", err.message); return; }
  }
  reply_ok(m, p, id, nullptr);
}

static ccord_id parse_id_param(const ccord_json *params, const char *key)
{
  size_t n = 0;
  const char *s = str_param(params, key, &n);
  if (!s || !n) return 0;
  if (strcmp(s, "dm") == 0) return CCORD_DM_GUILD_ID;
  return (ccord_id)strtoull(s, nullptr, 10);
}

static const char *channel_kind_str(const ccord_store *st, const ccord_channel *c)
{
  if (!c) return "text";
  if (c->guild_id == CCORD_DM_GUILD_ID) {
    return (c->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM || c->recipient_count > 1u) ? "group_dm" : "dm";
  }
  if (st && ccord_store_channel_is_forum(st, c->id)) return "forum";
  if (ccord_discord_type_is_thread(c->discord_type)) return "thread";
  if (c->kind == CCORD_CHANNEL_CATEGORY) return "category";
  if (c->kind == CCORD_CHANNEL_STAGE || c->discord_type == CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE) return "stage";
  if (c->kind == CCORD_CHANNEL_VOICE) return "voice";
  return "text";
}

static void channel_display_name(const ccord_store *st, const ccord_channel *c, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!c) return;
  if (c->name && c->name[0]) {
    snprintf(out, cap, "%s", c->name);
    return;
  }
  if (c->guild_id == CCORD_DM_GUILD_ID && st) {
    if (c->recipient_count > 1u) {
      size_t pos = 0;
      for (uint16_t i = 0; i < c->recipient_count && i < CCORD_MAX_DM_RECIPIENTS; i++) {
        const ccord_user *u = ccord_store_user(st, c->recipient_ids[i]);
        const char *un = (u && u->name && u->name[0]) ? u->name : (u && u->handle && u->handle[0] ? u->handle : nullptr);
        if (!un) continue;
        int n = snprintf(out + pos, cap - pos, "%s%s", pos ? ", " : "", un);
        if (n < 0 || (size_t)n >= cap - pos) break;
        pos += (size_t)n;
      }
      if (out[0]) return;
    }
    ccord_id uid = c->user_id ? c->user_id : (c->recipient_count > 0 ? c->recipient_ids[0] : CCORD_ID_NONE);
    const ccord_user *u = uid ? ccord_store_user(st, uid) : nullptr;
    if (u && u->name && u->name[0]) { snprintf(out, cap, "%s", u->name); return; }
    if (u && u->handle && u->handle[0]) { snprintf(out, cap, "%s", u->handle); return; }
    snprintf(out, cap, "Direct Message");
    return;
  }
  snprintf(out, cap, "Channel");
}

static void write_channel_obj(ccord_jw *w, const ccord_store *st, const ccord_channel *c, int64_t now_unix_ms)
{
  char dname[192];
  channel_display_name(st, c, dname, sizeof dname);
  bool muted = st ? ccord_store_channel_is_muted(st, c, now_unix_ms) : false;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "id"); ccord_jw_id(w, c->id);
  ccord_jw_key(w, "guild_id"); ccord_jw_id(w, c->guild_id);
  if (c->parent_id != CCORD_ID_NONE) {
    ccord_jw_key(w, "parent_id"); ccord_jw_id(w, c->parent_id);
  }
  ccord_jw_key(w, "name"); ccord_jw_cstr(w, c->name ? c->name : "");
  ccord_jw_key(w, "display_name"); ccord_jw_cstr(w, dname);
  ccord_jw_key(w, "topic"); ccord_jw_cstr(w, c->topic ? c->topic : "");
  ccord_jw_key(w, "kind"); ccord_jw_cstr(w, channel_kind_str(st, c));
  ccord_jw_key(w, "is_dm"); ccord_jw_bool(w, c->guild_id == CCORD_DM_GUILD_ID);
  ccord_jw_key(w, "unread_count"); ccord_jw_int(w, (int64_t)c->unread_count);
  ccord_jw_key(w, "mention_count"); ccord_jw_int(w, (int64_t)c->mention_count);
  ccord_jw_key(w, "muted"); ccord_jw_bool(w, muted);
  ccord_jw_end_object(w);
}

static void req_store(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params)
{
  if (!m->store) { reply_err(m, p, id, "unsupported", "store unavailable"); return; }
  int64_t now_unix = ccord_clock_unix_ms();
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  if (strcmp(method, "store.self") == 0) {
    ccord_id sid = ccord_store_self_id(m->store);
    const ccord_user *u = sid ? ccord_store_user(m->store, sid) : nullptr;
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id"); ccord_jw_id(&w, sid);
    ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, u && u->name ? u->name : "");
    ccord_jw_end_object(&w);
  } else if (strcmp(method, "store.selected") == 0) {
    ccord_id sg = ccord_store_selected_guild(m->store);
    ccord_id sc = ccord_store_selected_channel(m->store);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, sg);
    ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, sc);
    ccord_jw_key(&w, "view"); ccord_jw_cstr(&w, ccord_contrib_ui_view(m->contrib));
    ccord_jw_key(&w, "is_dm"); ccord_jw_bool(&w, sg == CCORD_DM_GUILD_ID);
    ccord_jw_end_object(&w);
  } else if (strcmp(method, "store.guilds") == 0) {
    ccord_jw_begin_array(&w);
    size_t n = ccord_store_guild_count(m->store);
    for (size_t i = 0; i < n; i++) {
      const ccord_guild *g = ccord_store_guild_at(m->store, i);
      if (!g) continue;
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "id"); ccord_jw_id(&w, g->id);
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, g->name ? g->name : "");
      ccord_jw_key(&w, "muted"); ccord_jw_bool(&w, ccord_store_guild_muted(m->store, g->id, now_unix));
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_array(&w);
  } else if (strcmp(method, "store.guild") == 0) {
    ccord_id gid = parse_id_param(params, "guild_id");
    if (gid == CCORD_DM_GUILD_ID) {
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "id"); ccord_jw_id(&w, CCORD_DM_GUILD_ID);
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, CCORD_DM_GUILD_NAME);
      ccord_jw_key(&w, "is_dm"); ccord_jw_bool(&w, true);
      ccord_jw_key(&w, "muted"); ccord_jw_bool(&w, false);
      ccord_jw_end_object(&w);
    } else {
      const ccord_guild *g = ccord_store_guild(m->store, gid);
      if (!g) { ccord_buf_free(&b); reply_err(m, p, id, "not_found", "guild not found"); return; }
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "id"); ccord_jw_id(&w, g->id);
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, g->name ? g->name : "");
      ccord_jw_key(&w, "is_dm"); ccord_jw_bool(&w, false);
      ccord_jw_key(&w, "muted"); ccord_jw_bool(&w, ccord_store_guild_muted(m->store, g->id, now_unix));
      ccord_jw_end_object(&w);
    }
  } else if (strcmp(method, "store.channels") == 0) {
    ccord_id gid = parse_id_param(params, "guild_id");
    ccord_jw_begin_array(&w);
    size_t n = ccord_store_channel_count(m->store, gid);
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *c = ccord_store_channel_at(m->store, gid, i);
      if (!c) continue;
      write_channel_obj(&w, m->store, c, now_unix);
    }
    ccord_jw_end_array(&w);
  } else if (strcmp(method, "store.channel") == 0) {
    const ccord_channel *c = ccord_store_channel(m->store, parse_id_param(params, "channel_id"));
    if (!c) { ccord_buf_free(&b); reply_err(m, p, id, "not_found", "channel not found"); return; }
    write_channel_obj(&w, m->store, c, now_unix);
  } else if (strcmp(method, "store.user") == 0) {
    const ccord_user *u = ccord_store_user(m->store, parse_id_param(params, "user_id"));
    if (!u) { ccord_buf_free(&b); reply_err(m, p, id, "not_found", "user not found"); return; }
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id"); ccord_jw_id(&w, u->id);
    ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, u->name ? u->name : "");
    ccord_jw_end_object(&w);
  } else if (strcmp(method, "store.messages") == 0) {
    ccord_id cid = parse_id_param(params, "channel_id");
    int64_t lim = 50;
    const ccord_json *lj = params ? ccord_json_get(params, "limit") : nullptr;
    if (lj) (void)ccord_json_int64(lj, &lim);
    if (lim <= 0) lim = 50;
    size_t tot = ccord_store_message_count(m->store, cid);
    size_t start = tot > (size_t)lim ? tot - (size_t)lim : 0;
    ccord_jw_begin_array(&w);
    for (size_t i = start; i < tot; i++) {
      const ccord_message *msg = ccord_store_message_at(m->store, cid, i);
      if (!msg) continue;
      const ccord_user *u = ccord_store_user(m->store, msg->author_id);
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "id"); ccord_jw_id(&w, msg->id);
      ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, msg->channel_id);
      ccord_jw_key(&w, "author_id"); ccord_jw_id(&w, msg->author_id);
      ccord_jw_key(&w, "author_name"); ccord_jw_cstr(&w, u && u->name ? u->name : "");
      ccord_jw_key(&w, "content"); ccord_jw_cstr(&w, msg->content ? msg->content : "");
      ccord_jw_key(&w, "timestamp_ms"); ccord_jw_int(&w, msg->timestamp_ms);
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_array(&w);
  } else if (strcmp(method, "store.message") == 0) {
    const ccord_message *msg = ccord_store_find_message(m->store, parse_id_param(params, "channel_id"), parse_id_param(params, "message_id"));
    if (!msg) { ccord_buf_free(&b); reply_err(m, p, id, "not_found", "message not found"); return; }
    const ccord_user *u = ccord_store_user(m->store, msg->author_id);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id"); ccord_jw_id(&w, msg->id);
    ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, msg->channel_id);
    ccord_jw_key(&w, "author_id"); ccord_jw_id(&w, msg->author_id);
    ccord_jw_key(&w, "author_name"); ccord_jw_cstr(&w, u && u->name ? u->name : "");
    ccord_jw_key(&w, "content"); ccord_jw_cstr(&w, msg->content ? msg->content : "");
    ccord_jw_key(&w, "timestamp_ms"); ccord_jw_int(&w, msg->timestamp_ms);
    ccord_jw_end_object(&w);
  } else if (strcmp(method, "store.members") == 0) {
    ccord_id gid = parse_id_param(params, "guild_id");
    size_t n = ccord_store_member_count(m->store, gid);
    ccord_jw_begin_array(&w);
    for (size_t i = 0; i < n; i++) {
      const ccord_member *mb = ccord_store_member_at(m->store, gid, i);
      if (!mb) continue;
      const ccord_user *u = ccord_store_user(m->store, mb->user_id);
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "user_id"); ccord_jw_id(&w, mb->user_id);
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, u && u->name ? u->name : "");
      ccord_jw_key(&w, "nick"); ccord_jw_cstr(&w, mb->nick);
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_array(&w);
  } else {
    ccord_buf_free(&b);
    reply_err(m, p, id, "unsupported", "unknown store method");
    return;
  }
  if (ccord_jw_ok(&w)) reply_ok(m, p, id, ccord_buf_cstr(&b));
  else reply_err(m, p, id, "internal", "store encode failed");
  ccord_buf_free(&b);
}

static void handle_request(ccord_plugins *m, plugin *p, const ccord_ipc_msg *msg)
{
  uint32_t id = msg->id;
  const char *method = msg->method;
  const ccord_json *params = msg->params;

  // Class 1 (Safe) APIs — no manifest permission required
  if (strcmp(method, "ui.contextMenu.add") == 0) { req_menu_add(m, p, id, params); return; }
  if (strcmp(method, "ui.contextMenu.remove") == 0) { req_menu_remove(m, p, id, params); return; }
  if (strcmp(method, "clipboard.write") == 0) { req_clipboard(m, p, id, params); return; }
  if (strcmp(method, "commands.register") == 0) { req_commands_register(m, p, id, params); return; }
  if (strcmp(method, "ui.toast.show") == 0) {
    size_t tl = 0, ll = 0;
    const char *txt = str_param(params, "text", &tl);
    const char *lvl = str_param(params, "level", &ll);
    uint8_t lv = (lvl && strcmp(lvl, "error") == 0) ? 2 : (lvl && strcmp(lvl, "warn") == 0) ? 1 : 0;
    if (txt && m->toast_show) m->toast_show(m->toast_ud, txt, lv);
    reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "storage.get") == 0 || strcmp(method, "storage.set") == 0 ||
      strcmp(method, "storage.delete") == 0 || strcmp(method, "storage.keys") == 0 ||
      strcmp(method, "storage.clear") == 0) {
    req_storage(m, p, id, method, params);
    return;
  }
  if (strcmp(method, "events.subscribe") == 0 || strcmp(method, "events.unsubscribe") == 0) {
    size_t nl = 0;
    const char *name = str_param(params, "name", &nl);
    uint32_t bit = 0;
    ccord_perm perm = CCORD_PERM_EVENTS_NAVIGATION;
    char name_s[32];
    if (!name || nl >= sizeof name_s) { reply_err(m, p, id, "invalid_argument", "unknown event name"); return; }
    snprintf(name_s, sizeof name_s, "%.*s", (int)nl, name);
    if (!event_bit_for(name_s, &bit, &perm)) { reply_err(m, p, id, "invalid_argument", "unknown event name"); return; }
    if (strcmp(method, "events.subscribe") == 0) {
      if (ccord_perm_class_of(perm) != CCORD_PERM_CLASS_SAFE && !has_perm(p, perm)) {
        fail_plugin(m, p, "undeclared restricted API called");
        return;
      }
      p->subs |= bit;
    } else {
      p->subs &= ~bit;
    }
    reply_ok(m, p, id, nullptr);
    return;
  }

  // Class 2 (Unsafe) APIs
  if (strncmp(method, "ui.panel.", 9) == 0) {
    if (!has_perm(p, CCORD_PERM_UI_PANEL)) { fail_plugin(m, p, "undeclared restricted API called: ui.panel"); return; }
    if (strcmp(method, "ui.panel.mount") == 0) { req_panel_mount(m, p, id, params); return; }
    if (strcmp(method, "ui.panel.update") == 0) { req_panel_update(m, p, id, params); return; }
    if (strcmp(method, "ui.panel.unmount") == 0) { req_panel_unmount(m, p, id, params); return; }
    reply_err(m, p, id, "unsupported", "unknown panel method");
    return;
  }
  if (strncmp(method, "store.", 6) == 0) {
    if (!has_perm(p, CCORD_PERM_STORE_READ)) { fail_plugin(m, p, "undeclared restricted API called: store.read"); return; }
    req_store(m, p, id, method, params);
    return;
  }
  if (strcmp(method, "messages.send") == 0) {
    if (!has_perm(p, CCORD_PERM_MESSAGES_SEND)) { fail_plugin(m, p, "undeclared restricted API called: messages.send"); return; }
    ccord_id cid = parse_id_param(params, "channel_id");
    size_t cl = 0; const char *cnt = str_param(params, "content", &cl);
    ccord_error err = {0};
    if (!m->session || !cid || !cnt || ccord_session_send_message(m->session, cid, cnt, cl, &err) != CCORD_OK) {
      reply_err(m, p, id, "internal", err.message[0] ? err.message : "send failed");
    } else reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "messages.edit") == 0) {
    if (!has_perm(p, CCORD_PERM_MESSAGES_MANAGE)) { fail_plugin(m, p, "undeclared restricted API called: messages.manage"); return; }
    ccord_id cid = parse_id_param(params, "channel_id"), mid = parse_id_param(params, "message_id");
    size_t cl = 0; const char *cnt = str_param(params, "content", &cl);
    ccord_error err = {0};
    if (!m->session || !cid || !mid || !cnt || ccord_session_edit_message(m->session, cid, mid, cnt, cl, &err) != CCORD_OK) {
      reply_err(m, p, id, "internal", err.message[0] ? err.message : "edit failed");
    } else reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "messages.delete") == 0) {
    if (!has_perm(p, CCORD_PERM_MESSAGES_MANAGE)) { fail_plugin(m, p, "undeclared restricted API called: messages.manage"); return; }
    ccord_id cid = parse_id_param(params, "channel_id"), mid = parse_id_param(params, "message_id");
    ccord_error err = {0};
    if (!m->session || !cid || !mid || ccord_session_delete_message(m->session, cid, mid, &err) != CCORD_OK) {
      reply_err(m, p, id, "internal", err.message[0] ? err.message : "delete failed");
    } else reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "messages.typing") == 0) {
    if (!has_perm(p, CCORD_PERM_MESSAGES_MANAGE)) { fail_plugin(m, p, "undeclared restricted API called: messages.manage"); return; }
    ccord_id cid = parse_id_param(params, "channel_id");
    ccord_error err = {0};
    if (m->session && cid) (void)ccord_session_send_typing(m->session, cid, &err);
    reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "navigation.select_guild") == 0 || strcmp(method, "navigation.select_channel") == 0 ||
      strcmp(method, "navigation.select_view") == 0 || strcmp(method, "navigation.action") == 0) {
    if (!has_perm(p, CCORD_PERM_NAVIGATION_WRITE)) { fail_plugin(m, p, "undeclared restricted API called: navigation.write"); return; }
    ccord_error err = {0};
    if (strcmp(method, "navigation.action") == 0) {
      size_t al = 0;
      const char *act = str_param(params, "action", &al);
      if (act) (void)ccord_contrib_ui_action(m->contrib, act);
    } else if (strcmp(method, "navigation.select_view") == 0) {
      size_t vl = 0;
      const char *view = str_param(params, "view", &vl);
      if (view && !ccord_contrib_ui_navigate(m->contrib, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, "", view)) {
        if (m->session && (strcmp(view, "friends") == 0 || strcmp(view, "requests") == 0)) {
          (void)ccord_session_select_guild(m->session, CCORD_DM_GUILD_ID, &err);
        }
      }
    } else if (strcmp(method, "navigation.select_guild") == 0) {
      ccord_id gid = parse_id_param(params, "guild_id");
      if (!ccord_contrib_ui_navigate(m->contrib, gid, CCORD_ID_NONE, CCORD_ID_NONE, "", "guild") && m->session) {
        (void)ccord_session_select_guild(m->session, gid, &err);
      }
    } else {
      ccord_id cid = parse_id_param(params, "channel_id");
      ccord_id gid = parse_id_param(params, "guild_id");
      ccord_id pid = parse_id_param(params, "parent_id");
      size_t nl = 0;
      const char *tname = str_param(params, "name", &nl);
      if (!ccord_contrib_ui_navigate(m->contrib, gid, cid, pid, tname ? tname : "", "channel") && m->session) {
        if (pid != CCORD_ID_NONE && gid != CCORD_ID_NONE) {
          (void)ccord_session_open_thread(m->session, gid, pid, cid, tname ? tname : "", &err);
        } else {
          (void)ccord_session_select_channel(m->session, cid, &err);
        }
      }
    }
    reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "voice.join") == 0 || strcmp(method, "voice.leave") == 0 ||
      strcmp(method, "voice.set_mute") == 0 || strcmp(method, "voice.set_deafen") == 0) {
    if (!has_perm(p, CCORD_PERM_VOICE_CONTROL)) { fail_plugin(m, p, "undeclared restricted API called: voice.control"); return; }
    ccord_error err = {0};
    if (m->session) {
      if (strcmp(method, "voice.join") == 0) (void)ccord_session_join_voice(m->session, parse_id_param(params, "guild_id"), parse_id_param(params, "channel_id"), &err);
      else if (strcmp(method, "voice.leave") == 0) (void)ccord_session_leave_voice(m->session, parse_id_param(params, "guild_id"), &err);
      else if (strcmp(method, "voice.set_mute") == 0) (void)ccord_session_set_voice_mute(m->session, ccord_json_bool(ccord_json_get(params, "muted"), false), &err);
      else (void)ccord_session_set_voice_deafen(m->session, ccord_json_bool(ccord_json_get(params, "deafened"), false), &err);
    }
    reply_ok(m, p, id, nullptr);
    return;
  }

  // Class 1 (Safe) mobile UX APIs: gracefully report performed=false when unsupported on desktop.
  if (strcmp(method, "ui.haptics.impact") == 0) {
    size_t sl = 0;
    const char *style = str_param(params, "style", &sl);
    int code = 0;
    if (style && strcmp(style, "medium") == 0) code = 1;
    else if (style && strcmp(style, "heavy") == 0) code = 2;
    else if (style && strcmp(style, "selection") == 0) code = 3;
    bool ok = m->plat_ops.haptic_impact && m->plat_ops.haptic_impact(m->plat_ops.ud, code);
    reply_ok(m, p, id, ok ? "{\"performed\":true}" : "{\"performed\":false}");
    return;
  }
  if (strcmp(method, "ui.haptics.notify") == 0) {
    size_t kl = 0;
    const char *kind = str_param(params, "kind", &kl);
    int code = 0;
    if (kind && strcmp(kind, "warning") == 0) code = 1;
    else if (kind && strcmp(kind, "error") == 0) code = 2;
    bool ok = m->plat_ops.haptic_notify && m->plat_ops.haptic_notify(m->plat_ops.ud, code);
    reply_ok(m, p, id, ok ? "{\"performed\":true}" : "{\"performed\":false}");
    return;
  }
  if (strcmp(method, "ui.share.text") == 0) {
    size_t tl = 0, xl = 0;
    const char *title = str_param(params, "title", &tl);
    const char *text = str_param(params, "text", &xl);
    if (!text || xl == 0 || xl > MAX_CLIPBOARD_BYTES) {
      reply_err(m, p, id, "invalid_argument", "share text invalid");
      return;
    }
    bool ok = m->plat_ops.share_text && m->plat_ops.share_text(m->plat_ops.ud, title ? title : "", text);
    reply_ok(m, p, id, ok ? "{\"performed\":true}" : "{\"performed\":false}");
    return;
  }
  if (strcmp(method, "ui.keyboard.dismiss") == 0) {
    if (m->plat_ops.keyboard_dismiss) m->plat_ops.keyboard_dismiss(m->plat_ops.ud);
    reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "ui.mobile.keep_screen_on") == 0) {
    bool enabled = ccord_json_bool(ccord_json_get(params, "enabled"), false);
    bool ok = m->plat_ops.keep_screen_on && m->plat_ops.keep_screen_on(m->plat_ops.ud, enabled);
    reply_ok(m, p, id, ok ? "{\"performed\":true}" : "{\"performed\":false}");
    return;
  }
  if (strcmp(method, "ui.mobile.set_orientation") == 0) {
    size_t ml = 0;
    const char *mode = str_param(params, "mode", &ml);
    int code = 0;
    if (mode && strcmp(mode, "portrait") == 0) code = 1;
    else if (mode && strcmp(mode, "landscape") == 0) code = 2;
    bool ok = m->plat_ops.set_orientation && m->plat_ops.set_orientation(m->plat_ops.ud, code);
    reply_ok(m, p, id, ok ? "{\"performed\":true}" : "{\"performed\":false}");
    return;
  }

  // Class 2 / Class 3 system & clipboard bridges.
  if (strcmp(method, "clipboard.read") == 0) {
    if (!has_perm(p, CCORD_PERM_CLIPBOARD_READ)) { fail_plugin(m, p, "undeclared restricted API called: clipboard.read"); return; }
    char *txt = m->plat_ops.clipboard_read ? m->plat_ops.clipboard_read(m->plat_ops.ud) : nullptr;
    if (!txt) {
      reply_ok(m, p, id, "{\"text\":null}");
      return;
    }
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "text"); ccord_jw_string(&w, txt, strlen(txt));
    ccord_jw_end_object(&w);
    ccord_free(txt);
    if (ccord_jw_ok(&w)) reply_ok(m, p, id, ccord_buf_cstr(&b));
    else reply_err(m, p, id, "internal", "clipboard text encode failed");
    ccord_buf_free(&b);
    return;
  }
  if (strcmp(method, "system.open_url") == 0) {
    if (!has_perm(p, CCORD_PERM_SYSTEM_OPEN)) { fail_plugin(m, p, "undeclared restricted API called: system.open"); return; }
    size_t ul = 0;
    const char *url = str_param(params, "url", &ul);
    if (!url || (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0)) {
      reply_err(m, p, id, "invalid_argument", "only http:// and https:// URLs allowed");
      return;
    }
    if (m->plat_ops.open_url) (void)m->plat_ops.open_url(m->plat_ops.ud, url);
    reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "system.notify") == 0) {
    if (!has_perm(p, CCORD_PERM_SYSTEM_NOTIFY)) { fail_plugin(m, p, "undeclared restricted API called: system.notify"); return; }
    size_t tl = 0, bl = 0, tagl = 0;
    const char *title = str_param(params, "title", &tl);
    const char *body = str_param(params, "body", &bl);
    const char *tag = str_param(params, "tag", &tagl);
    bool silent = ccord_json_bool(ccord_json_get(params, "silent"), false);
    if (m->plat_ops.notify) (void)m->plat_ops.notify(m->plat_ops.ud, title ? title : "", body ? body : "", tag, silent);
    reply_ok(m, p, id, nullptr);
    return;
  }

  reply_err(m, p, id, "unsupported", "unknown method");
}

// ---------------------------------------------------------------- host -> main notifications and responses

static void handle_hello(ccord_plugins *m, plugin *p, const ccord_ipc_msg *msg)
{
  const ccord_json *proto = msg->params ? ccord_json_get(msg->params, "proto") : nullptr;
  size_t ll = 0;
  const char *lua = str_param(msg->params, "lua", &ll);
  int64_t pv = 0;
  if (!proto || !ccord_json_int64(proto, &pv) || pv != CCORD_IPC_PROTO_VERSION) {
    { fail_plugin(m, p, "hello: protocol version mismatch"); return; }
  }
  if (!lua || strcmp(lua, CCORD_LUA_EXPECTED_RELEASE) != 0) {
    { fail_plugin(m, p, "hello: host runs an unexpected Lua release"); return; }
  }
  if (p->hello_seen) { fail_plugin(m, p, "hello sent twice"); return; }
  p->hello_seen = true;

  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "plugin");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, p->m.id);
  ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, p->m.name);
  ccord_jw_key(&w, "version"); ccord_jw_cstr(&w, p->m.version);
  ccord_jw_key(&w, "dir"); ccord_jw_cstr(&w, p->dir);
  ccord_jw_end_object(&w);
  ccord_jw_key(&w, "permissions");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < sizeof k_all_perms / sizeof k_all_perms[0]; i++) {
    if (has_perm(p, k_all_perms[i])) ccord_jw_cstr(&w, ccord_perm_name(k_all_perms[i]));
  }
  ccord_jw_end_array(&w);
  ccord_jw_key(&w, "source"); ccord_jw_string(&w, p->source, p->source_len);
  ccord_jw_key(&w, "api"); ccord_jw_int(&w, CCORD_PLUGIN_API_VERSION);
  ccord_jw_key(&w, "platform");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "os"); ccord_jw_cstr(&w, m->plat_os);
  ccord_jw_key(&w, "arch"); ccord_jw_cstr(&w, m->plat_arch);
  ccord_jw_key(&w, "form_factor"); ccord_jw_cstr(&w, m->plat_form_factor);
  ccord_jw_key(&w, "is_mobile"); ccord_jw_bool(&w, m->plat_is_mobile);
  ccord_jw_key(&w, "touch"); ccord_jw_bool(&w, m->plat_touch);
  ccord_jw_key(&w, "android_api_level"); ccord_jw_int(&w, m->plat_android_api_level);
  ccord_jw_key(&w, "capabilities_mask"); ccord_jw_int(&w, (int64_t)m->plat_capabilities);
  ccord_jw_key(&w, "state");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "safe_top"); ccord_jw_int(&w, (int64_t)m->plat_state.safe_top);
  ccord_jw_key(&w, "safe_right"); ccord_jw_int(&w, (int64_t)m->plat_state.safe_right);
  ccord_jw_key(&w, "safe_bottom"); ccord_jw_int(&w, (int64_t)m->plat_state.safe_bottom);
  ccord_jw_key(&w, "safe_left"); ccord_jw_int(&w, (int64_t)m->plat_state.safe_left);
  ccord_jw_key(&w, "viewport_w"); ccord_jw_int(&w, (int64_t)m->plat_state.viewport_w);
  ccord_jw_key(&w, "viewport_h"); ccord_jw_int(&w, (int64_t)m->plat_state.viewport_h);
  ccord_jw_key(&w, "viewport_scale"); ccord_jw_int(&w, (int64_t)(m->plat_state.viewport_scale > 0.0f ? m->plat_state.viewport_scale : 1.0f));
  ccord_jw_key(&w, "keyboard_height"); ccord_jw_int(&w, (int64_t)m->plat_state.keyboard_height);
  ccord_jw_key(&w, "keyboard_visible"); ccord_jw_bool(&w, m->plat_state.keyboard_visible);
  ccord_jw_key(&w, "landscape"); ccord_jw_bool(&w, m->plat_state.landscape);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (p->nitems > 0) {
    ccord_arena ar;
    ccord_arena_init(&ar, 0);
    ccord_jw_key(&w, "storage");
    ccord_jw_begin_object(&w);
    for (size_t i = 0; i < p->nitems; i++) {
      ccord_arena_reset(&ar);
      ccord_error pe = {0};
      const ccord_json *v = ccord_json_parse(&ar, p->items[i].json, p->items[i].json_len, &k_json_limits, &pe);
      ccord_jw_key(&w, p->items[i].key);
      jw_copy_json(&w, v);
    }
    ccord_jw_end_object(&w);
    ccord_arena_free(&ar);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&b);
    { fail_plugin(m, p, "main.lua is not valid UTF-8 text"); return; }
  }
  pending_req *slot = pend_free(p);
  if (!slot) {
    ccord_buf_free(&b);
    { fail_plugin(m, p, "internal: no free request slot"); return; }
  }
  uint32_t id = next_request_id(p);
  ccord_error err = {0};
  if (ccord_ipc_send_request(p->ipc, id, "init", ccord_buf_cstr(&b), &err) != CCORD_OK) {
    ccord_buf_free(&b);
    char why[256];
    snprintf(why, sizeof why, "could not send init: %s", err.message);
    { fail_plugin(m, p, why); return; }
  }
  ccord_buf_free(&b);
  slot->kind = PEND_INIT;
  slot->id = id;
  slot->deadline_ms = m->now_ms + INIT_DEADLINE_MS;
  (void)ccord_ipc_flush(p->ipc, 0, &err);
}

static void handle_log(ccord_plugins *m, plugin *p, const ccord_ipc_msg *msg, uint64_t now)
{
  size_t ml = 0, ll = 0;
  const char *level = str_param(msg->params, "level", &ll);
  const char *text = str_param(msg->params, "msg", &ml);
  ccord_log_level lvl = CCORD_LOG_INFO;
  if (level && strcmp(level, "debug") == 0) lvl = CCORD_LOG_DEBUG;
  else if (level && strcmp(level, "warn") == 0) lvl = CCORD_LOG_WARN;
  else if (level && strcmp(level, "error") == 0) lvl = CCORD_LOG_ERROR;
  char clipped[1024];
  snprintf(clipped, sizeof clipped, "%.*s", (int)(ml < 1000 ? ml : 1000), text ? text : "");
  ccord_logf(lvl, __FILE__, __LINE__, "[plugin %s] %s", p->m.id, clipped);
  ccord_contrib_add_plugin_log(m->contrib, p->m.id, now, (uint8_t)lvl, clipped);
}

static void handle_response(ccord_plugins *m, plugin *p, const ccord_ipc_msg *msg)
{
  pending_req *slot = nullptr;
  for (size_t i = 0; i < MAX_PENDING; i++) {
    if (p->pend[i].kind != PEND_NONE && p->pend[i].id == msg->id) slot = &p->pend[i];
  }
  if (!slot) { fail_plugin(m, p, "protocol violation: response for unknown request"); return; }
  pend_kind kind = slot->kind;
  ccord_id channel = slot->channel_id;
  slot->kind = PEND_NONE;
  if (kind == PEND_INIT) {
    if (!msg->ok) {
      char why[256];
      snprintf(why, sizeof why, "init failed: %s", msg->err_message ? msg->err_message : "unknown error");
      { fail_plugin(m, p, why); return; }
    }
    p->state = CCORD_PLUGIN_READY;
    p->last_ping_sent_ms = m->now_ms;
    CCORD_LOG_I("plugin %s ready", p->m.id);
    publish_state(m, p);
  } else if (kind == PEND_CMD) {
    if (!msg->ok) {
      CCORD_LOG_W("plugin %s: command failed: %s", p->m.id, msg->err_message ? msg->err_message : "");
      return;
    }
    size_t tl = 0;
    const char *text = str_param(msg->result, "text", &tl);
    if (!text) return;
    if (!m->session) return;
    ccord_error err = {0};
    if (ccord_session_send_message(m->session, channel, text, tl, &err) != CCORD_OK) {
      CCORD_LOG_W("plugin %s: command result not sent: %s", p->m.id, err.message);
    }
  }
  // PEND_PING: the answer itself is the liveness signal.
}

static void handle_frame(ccord_plugins *m, plugin *p, const ccord_ipc_msg *msg)
{
  if (msg->kind == CCORD_IPC_NTF) {
    if (strcmp(msg->method, "hello") == 0) { handle_hello(m, p, msg); return; }
    if (strcmp(msg->method, "log") == 0) { handle_log(m, p, msg, m->now_ms); return; }
    CCORD_LOG_D("plugin %s: ignored notification %s", p->m.id, msg->method);
    return;
  }
  if (msg->kind == CCORD_IPC_REQ) { handle_request(m, p, msg); return; }
  { handle_response(m, p, msg); return; }
}

// ---------------------------------------------------------------- main -> host events and commands

static void bus_send_event(ccord_plugins *m, plugin *p, const char *name, const ccord_buf *data)
{
  ccord_buf params;
  ccord_buf_init(&params, 0);
  bool ok = app_raw(&params, "{\"name\":") && app_str(&params, name, strlen(name)) && app_raw(&params, ",\"data\":") &&
            ccord_buf_append(&params, data->data, data->len) == CCORD_OK && app_raw(&params, "}");
  if (ok) send_ntf(m, p, "event", ccord_buf_cstr(&params));
  ccord_buf_free(&params);
}

static const char *nav_view_str(ccord_nav_view v)
{
  switch (v) {
    case CCORD_NAV_VIEW_FRIENDS: return "friends";
    case CCORD_NAV_VIEW_REQUESTS: return "requests";
    case CCORD_NAV_VIEW_DISCOVERY: return "discovery";
    case CCORD_NAV_VIEW_GUILD: return "guild";
    case CCORD_NAV_VIEW_THREAD: return "thread";
    default: return "channel";
  }
}

static void on_bus_event(const ccord_event *ev, void *ud)
{
  ccord_plugins *m = ud;
  uint32_t bit = 0;
  const char *name = nullptr;
  switch (ev->type) {
    case CCORD_EVT_APP_READY:        bit = EVB_APP_READY;        name = "app.ready"; break;
    case CCORD_EVT_CONNECTION_STATE: bit = EVB_CONNECTION_STATE; name = "connection.state"; break;
    case CCORD_EVT_GUILD_SELECT:     bit = EVB_GUILD;            name = "guild.select"; break;
    case CCORD_EVT_CHANNEL_SELECT:   bit = EVB_CHANNEL;          name = "channel.select"; break;
    case CCORD_EVT_VIEW_SELECT:      bit = EVB_VIEW_SELECT;      name = "view.select"; break;
    case CCORD_EVT_NAV_AUX:          bit = EVB_NAV_AUX;          name = "navigation.aux_open"; break;
    case CCORD_EVT_CHANNEL_UPSERT:   bit = EVB_CHANNEL_UPSERT;   name = "channel.upsert"; break;
    case CCORD_EVT_CHANNEL_DELETE:   bit = EVB_CHANNEL_DELETE;   name = "channel.delete"; break;
    case CCORD_EVT_GUILDS_LOADED:    bit = EVB_GUILDS_LOADED;    name = "guilds.loaded"; break;
    case CCORD_EVT_NOTIFY_UPDATE:    bit = EVB_NOTIFY_UPDATE;    name = "notify.update"; break;
    case CCORD_EVT_MESSAGE_CREATE:   bit = EVB_MESSAGE;          name = "message.create"; break;
    case CCORD_EVT_MESSAGE_UPDATE:   bit = EVB_MESSAGE_UPDATE;   name = "message.update"; break;
    case CCORD_EVT_MESSAGE_DELETE:   bit = EVB_MESSAGE_DELETE;   name = "message.delete"; break;
    case CCORD_EVT_MESSAGE_PIN:      bit = EVB_MESSAGE_PIN;      name = "message.pin"; break;
    case CCORD_EVT_REACTION_UPDATE:  bit = EVB_REACTION_UPDATE;  name = "reaction.update"; break;
    case CCORD_EVT_TYPING_START:     bit = EVB_TYPING_START;     name = "typing.start"; break;
    case CCORD_EVT_PRESENCE_UPDATE:  bit = EVB_PRESENCE_UPDATE;  name = "presence.update"; break;
    case CCORD_EVT_VOICE_STATE:      bit = EVB_VOICE_STATE;      name = "voice.state"; break;
    default: return;
  }

  bool any = false;
  for (size_t i = 0; i < m->nplugins; i++) {
    plugin *p = &m->plugins[i];
    if (p->state == CCORD_PLUGIN_READY && (p->subs & bit)) any = true;
  }
  if (!any) return;

  ccord_buf data;
  ccord_buf_init(&data, 0);
  ccord_jw w;
  ccord_jw_init(&w, &data);
  if (bit == EVB_MESSAGE || bit == EVB_MESSAGE_UPDATE) {
    const ccord_message *msg = m->store ? ccord_store_find_message(m->store, ev->channel_id, ev->message_id) : nullptr;
    const ccord_user *u = msg && m->store ? ccord_store_user(m->store, msg->author_id) : nullptr;
    if (!msg) { ccord_buf_free(&data); return; }
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id"); ccord_jw_id(&w, msg->id);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, ev->guild_id);
    ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, msg->channel_id);
    ccord_jw_key(&w, "author_id"); ccord_jw_id(&w, msg->author_id);
    ccord_jw_key(&w, "author_name"); ccord_jw_cstr(&w, u ? u->name : "");
    ccord_jw_key(&w, "content"); ccord_jw_cstr(&w, msg->content ? msg->content : "");
    ccord_jw_key(&w, "timestamp_ms"); ccord_jw_int(&w, msg->timestamp_ms);
    ccord_jw_end_object(&w);
  } else if (bit == EVB_CHANNEL || bit == EVB_CHANNEL_UPSERT) {
    const ccord_channel *c = m->store ? ccord_store_channel(m->store, ev->channel_id) : nullptr;
    if (!c) { ccord_buf_free(&data); return; }
    char dname[192];
    channel_display_name(m->store, c, dname, sizeof dname);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, c->guild_id);
    ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, c->id);
    if (c->parent_id != CCORD_ID_NONE) { ccord_jw_key(&w, "parent_id"); ccord_jw_id(&w, c->parent_id); }
    ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, c->name ? c->name : "");
    ccord_jw_key(&w, "display_name"); ccord_jw_cstr(&w, dname);
    ccord_jw_key(&w, "kind"); ccord_jw_cstr(&w, channel_kind_str(m->store, c));
    ccord_jw_key(&w, "unread_count"); ccord_jw_int(&w, (int64_t)c->unread_count);
    ccord_jw_key(&w, "mention_count"); ccord_jw_int(&w, (int64_t)c->mention_count);
    ccord_jw_end_object(&w);
  } else if (bit == EVB_GUILD) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, ev->guild_id);
    if (ev->guild_id == CCORD_DM_GUILD_ID) {
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, CCORD_DM_GUILD_NAME);
      ccord_jw_key(&w, "is_dm"); ccord_jw_bool(&w, true);
    } else {
      const ccord_guild *g = m->store ? ccord_store_guild(m->store, ev->guild_id) : nullptr;
      if (!g) { ccord_buf_free(&data); return; }
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, g->name ? g->name : "");
      ccord_jw_key(&w, "is_dm"); ccord_jw_bool(&w, false);
    }
    ccord_jw_end_object(&w);
  } else if (bit == EVB_VIEW_SELECT) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, ev->guild_id);
    ccord_jw_key(&w, "view"); ccord_jw_cstr(&w, nav_view_str((ccord_nav_view)ev->value));
    ccord_jw_end_object(&w);
  } else if (bit == EVB_NAV_AUX) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, ev->guild_id);
    if (ev->channel_id) { ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, ev->channel_id); }
    if (ev->message_id) { ccord_jw_key(&w, "parent_id"); ccord_jw_id(&w, ev->message_id); }
    ccord_jw_key(&w, "view"); ccord_jw_cstr(&w, nav_view_str((ccord_nav_view)ev->value));
    const ccord_channel *c = (m->store && ev->channel_id) ? ccord_store_channel(m->store, ev->channel_id) : nullptr;
    if (c) {
      char dname[192];
      channel_display_name(m->store, c, dname, sizeof dname);
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, dname);
    }
    ccord_jw_end_object(&w);
  } else {
    ccord_jw_begin_object(&w);
    if (ev->guild_id) { ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, ev->guild_id); }
    if (ev->channel_id) { ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, ev->channel_id); }
    if (ev->message_id) { ccord_jw_key(&w, "message_id"); ccord_jw_id(&w, ev->message_id); }
    if (ev->user_id) { ccord_jw_key(&w, "user_id"); ccord_jw_id(&w, ev->user_id); }
    ccord_jw_key(&w, "value"); ccord_jw_int(&w, ev->value);
    ccord_jw_end_object(&w);
  }
  if (ccord_jw_ok(&w)) {
    for (size_t i = 0; i < m->nplugins; i++) {
      plugin *p = &m->plugins[i];
      if (p->state == CCORD_PLUGIN_READY && (p->subs & bit)) bus_send_event(m, p, name, &data);
    }
  }
  ccord_buf_free(&data);
}

static plugin *find_owner(ccord_plugins *m, const char *owner)
{
  if (!owner) return nullptr;
  for (size_t i = 0; i < m->nplugins; i++) {
    if (m->plugins[i].used && strcmp(m->plugins[i].m.id, owner) == 0) return &m->plugins[i];
  }
  return nullptr;
}

static void on_menu_activate(void *ud, const ccord_menu_contribution *item, const ccord_menu_context *ctx)
{
  ccord_plugins *m = ud;
  plugin *p = find_owner(m, item->owner);
  if (!p || p->state != CCORD_PLUGIN_READY) return;
  char g[24] = "", c[24] = "", mid[24] = "", u[24] = "";
  if (ctx->guild_id) ccord_id_format(ctx->guild_id, g);
  if (ctx->channel_id) ccord_id_format(ctx->channel_id, c);
  if (ctx->message_id) ccord_id_format(ctx->message_id, mid);
  if (ctx->user_id) ccord_id_format(ctx->user_id, u);
  const char *kind = ctx->target == CCORD_MENU_MESSAGE ? "message"
                   : ctx->target == CCORD_MENU_CHANNEL ? "channel"
                   : ctx->target == CCORD_MENU_GUILD   ? "guild"
                   : ctx->target == CCORD_MENU_NAV     ? "nav" : "member";
  ccord_buf params;
  ccord_buf_init(&params, 0);
  ccord_jw w;
  ccord_jw_init(&w, &params);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "item_id"); ccord_jw_cstr(&w, item->item_id);
  ccord_jw_key(&w, "ctx");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "kind"); ccord_jw_cstr(&w, kind);
  if (ctx->guild_id) { ccord_jw_key(&w, "guild_id"); ccord_jw_cstr(&w, g); }
  if (ctx->channel_id) { ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, c); }
  if (ctx->target == CCORD_MENU_NAV) {
    const char *view = ctx->message_id == CCORD_NAV_VIEW_FRIENDS  ? "friends"
                     : ctx->message_id == CCORD_NAV_VIEW_REQUESTS ? "requests"
                     : ctx->message_id == CCORD_NAV_VIEW_GUILD    ? "dm_home" : "nav";
    ccord_jw_key(&w, "view"); ccord_jw_cstr(&w, view);
  } else if (ctx->target == CCORD_MENU_CHANNEL && ctx->message_id) {
    ccord_jw_key(&w, "parent_id"); ccord_jw_cstr(&w, mid);
  } else if (ctx->message_id) {
    ccord_jw_key(&w, "message_id"); ccord_jw_cstr(&w, mid);
  }
  if (ctx->user_id) { ccord_jw_key(&w, "user_id"); ccord_jw_cstr(&w, u); }
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) send_ntf(m, p, "menu.invoke", ccord_buf_cstr(&params));
  else CCORD_LOG_W("plugin %s: menu context not encoded", p->m.id);
  ccord_buf_free(&params);
}

static void on_panel_event(void *ud, const ccord_panel_contribution *panel, const ccord_panel_event *ev)
{
  ccord_plugins *m = ud;
  plugin *p = find_owner(m, panel->owner);
  if (!p || p->state != CCORD_PLUGIN_READY) return;
  const char *etype = ev->kind == CCORD_PANEL_EV_DOUBLE_CLICK ? "double_click"
                    : ev->kind == CCORD_PANEL_EV_MIDDLE_CLICK ? "middle_click"
                    : ev->kind == CCORD_PANEL_EV_RIGHT_CLICK  ? "right_click"
                    : ev->kind == CCORD_PANEL_EV_WHEEL        ? "wheel" : "click";
  ccord_buf params;
  ccord_buf_init(&params, 0);
  ccord_jw w;
  ccord_jw_init(&w, &params);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "panel_id"); ccord_jw_cstr(&w, panel->panel_id);
  ccord_jw_key(&w, "event");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "panel_id"); ccord_jw_cstr(&w, panel->panel_id);
  ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, ev->node_id ? ev->node_id : "");
  ccord_jw_key(&w, "type"); ccord_jw_cstr(&w, etype);
  ccord_jw_key(&w, "button"); ccord_jw_int(&w, (int64_t)ev->button);
  ccord_jw_key(&w, "x"); ccord_jw_double(&w, (double)ev->x);
  ccord_jw_key(&w, "y"); ccord_jw_double(&w, (double)ev->y);
  if (ev->kind == CCORD_PANEL_EV_WHEEL) {
    ccord_jw_key(&w, "wheel"); ccord_jw_double(&w, (double)ev->wheel);
  }
  ccord_jw_key(&w, "mods");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "shift"); ccord_jw_bool(&w, (ev->mods & CCORD_MOD_SHIFT) != 0);
  ccord_jw_key(&w, "ctrl"); ccord_jw_bool(&w, (ev->mods & CCORD_MOD_CTRL) != 0);
  ccord_jw_key(&w, "alt"); ccord_jw_bool(&w, (ev->mods & CCORD_MOD_ALT) != 0);
  ccord_jw_key(&w, "super"); ccord_jw_bool(&w, (ev->mods & CCORD_MOD_SUPER) != 0);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) send_ntf(m, p, "panel.event", ccord_buf_cstr(&params));
  ccord_buf_free(&params);
}

static void on_command_run(void *ud, const ccord_command_contribution *cmd, const char *args, ccord_id guild_id, ccord_id channel_id)
{
  ccord_plugins *m = ud;
  plugin *p = find_owner(m, cmd->owner);
  if (!p || p->state != CCORD_PLUGIN_READY || !p->ipc) {
    CCORD_LOG_W("command /%s: plugin is not running", cmd->name);
    return;
  }
  pending_req *slot = pend_free(p);
  if (!slot) {
    CCORD_LOG_W("command /%s: too many pending commands", cmd->name);
    return;
  }
  char g[24] = "", c[24] = "";
  if (guild_id) ccord_id_format(guild_id, g);
  if (channel_id) ccord_id_format(channel_id, c);
  ccord_buf params;
  ccord_buf_init(&params, 0);
  ccord_jw w;
  ccord_jw_init(&w, &params);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, cmd->name);
  ccord_jw_key(&w, "args"); ccord_jw_cstr(&w, args ? args : "");
  ccord_jw_key(&w, "ctx");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id"); ccord_jw_cstr(&w, g);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, c);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) {
    uint32_t id = next_request_id(p);
    ccord_error err = {0};
    if (ccord_ipc_send_request(p->ipc, id, "command.run", ccord_buf_cstr(&params), &err) == CCORD_OK) {
      slot->kind = PEND_CMD;
      slot->id = id;
      slot->deadline_ms = m->now_ms + CMD_DEADLINE_MS;
      slot->channel_id = channel_id;
      (void)ccord_ipc_flush(p->ipc, 0, &err);
    } else {
      CCORD_LOG_W("command /%s: not sent: %s", cmd->name, err.message);
    }
  }
  ccord_buf_free(&params);
}

// ---------------------------------------------------------------- pump and timers

static void read_host(ccord_plugins *m, plugin *p)
{
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  for (int k = 0; k < 64 && p->ipc; k++) {
    ccord_ipc_msg msg;
    ccord_error err = {0};
    ccord_ipc_read r = ccord_ipc_try_read(p->ipc, &arena, &msg, &err);
    if (r == CCORD_IPC_READ_MSG) {
      handle_frame(m, p, &msg);
      ccord_arena_reset(&arena);
      continue;
    }
    if (r == CCORD_IPC_READ_EMPTY) break;
    char reason[320];
    char exit_text[160] = "";
    if (r == CCORD_IPC_READ_EOF) {
      if (reap_wait(p, EXIT_WAIT_MS, exit_text, sizeof exit_text) && exit_text[0]) {
        snprintf(reason, sizeof reason, "%s", exit_text);
      } else {
        set_text(reason, sizeof reason, "host closed the connection");
      }
    } else {
      snprintf(reason, sizeof reason, "protocol violation: %s", err.message);
    }
    fail_plugin(m, p, reason);
    break;
  }
  ccord_arena_free(&arena);
}

static void check_timers(ccord_plugins *m, plugin *p)
{
  if (!p->ipc) return;
  uint64_t now = m->now_ms;
  for (size_t i = 0; i < MAX_PENDING && p->ipc; i++) {
    pending_req *s = &p->pend[i];
    if (s->kind == PEND_NONE || now < s->deadline_ms) continue;
    if (s->kind == PEND_INIT) { fail_plugin(m, p, "init timed out after 5 s"); return; }
    if (s->kind == PEND_PING) { fail_plugin(m, p, "ping timeout: host did not answer in 5 s"); return; }
    CCORD_LOG_W("plugin %s: command.run timed out after 2 s", p->m.id);
    s->kind = PEND_NONE;
  }
  if (!p->ipc || p->state != CCORD_PLUGIN_READY) return;
  if (!pend_slot(p, PEND_PING) && now - p->last_ping_sent_ms >= PING_INTERVAL_MS) {
    pending_req *slot = pend_free(p);
    if (!slot) return;
    uint32_t id = next_request_id(p);
    ccord_error err = {0};
    if (ccord_ipc_send_request(p->ipc, id, "ping", "{}", &err) != CCORD_OK) {
      char why[256];
      snprintf(why, sizeof why, "could not send ping: %s", err.message);
      { fail_plugin(m, p, why); return; }
    }
    slot->kind = PEND_PING;
    slot->id = id;
    slot->deadline_ms = now + PING_TIMEOUT_MS;
    p->last_ping_sent_ms = now;
  }
}

void ccord_plugins_pump(ccord_plugins *m, uint64_t now_ms)
{
  if (!m) return;
  m->now_ms = now_ms;
  for (size_t i = 0; i < m->nplugins; i++) {
    plugin *p = &m->plugins[i];
    if (!p->ipc) continue;
    read_host(m, p);
    if (p->ipc) {
      ccord_error err = {0};
      if (ccord_ipc_flush(p->ipc, 0, &err) != CCORD_OK) fail_plugin(m, p, "host connection lost while writing");
    }
    if (p->ipc) check_timers(m, p);
    if (p->ipc == nullptr && host_is_running(p)) {
      char reason[160];
      (void)reap_wait(p, 0, reason, sizeof reason);
    }
    if (p->storage_enabled && p->dirty && now_ms - p->last_save_ms >= SAVE_INTERVAL_MS) storage_save(m, p, now_ms);
  }
  if (m->catalog) ccord_catalog_pump(m->catalog, now_ms);
}

// ---------------------------------------------------------------- spawn and discovery

static void plugin_worker_entry(void *arg)
{
  plugin *p = (plugin *)arg;
  int code = 70;
  if (p && p->thread_host_fn) {
    code = p->thread_host_fn(p->thread_fd, &p->cancel);
  } else if (p && p->thread_fd != CCORD_INVALID_SOCKET) {
    ccord_sock_close(p->thread_fd);
  }
  if (p) {
    atomic_store_explicit(&p->thread_exit_code, code, memory_order_relaxed);
    atomic_store_explicit(&p->thread_exited, true, memory_order_release);
  }
}

static ccord_status spawn_host(ccord_plugins *m, plugin *p, ccord_error *err)
{
  ccord_socket_t sv[2] = {CCORD_INVALID_SOCKET, CCORD_INVALID_SOCKET};
  if (!ccord_sock_pair(sv)) {
    ccord_error_set(err, CCORD_ERR_IO, "socket pair failed");
    return CCORD_ERR_IO;
  }
  if (m->in_process) {
    if (!m->in_process_host_fn) {
      ccord_sock_close(sv[0]);
      ccord_sock_close(sv[1]);
      ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "in-process plugin host callback missing");
      return CCORD_ERR_UNSUPPORTED;
    }
    p->in_process = true;
    p->thread_fd = sv[1];
    p->thread_host_fn = m->in_process_host_fn;
    atomic_store_explicit(&p->cancel, false, memory_order_relaxed);
    atomic_store_explicit(&p->thread_exited, false, memory_order_relaxed);
    atomic_store_explicit(&p->thread_exit_code, 0, memory_order_relaxed);
    p->reaped = false;
    p->worker = ccord_thread_create(plugin_worker_entry, p);
    if (!p->worker) {
      ccord_sock_close(sv[0]);
      ccord_sock_close(sv[1]);
      p->reaped = true;
      ccord_error_set(err, CCORD_ERR_IO, "failed to create plugin worker thread");
      return CCORD_ERR_IO;
    }
    p->ipc = ccord_ipc_open(sv[0], err);
    if (!p->ipc) {
      kill_host_process(p);
      return err->code ? err->code : CCORD_ERR_INTERNAL;
    }
    return CCORD_OK;
  }
  p->in_process = false;
#if CCORD_OS_ANDROID
  ccord_sock_close(sv[0]);
  ccord_sock_close(sv[1]);
  ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "Android requires in-process plugin worker mode");
  return CCORD_ERR_UNSUPPORTED;
#elif CCORD_OS_WINDOWS
  if (!SetHandleInformation((HANDLE)sv[1], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
    ccord_sock_close(sv[0]);
    ccord_sock_close(sv[1]);
    ccord_error_set(err, CCORD_ERR_IO, "SetHandleInformation failed");
    return CCORD_ERR_IO;
  }
  char cmd_utf8[CCORD_PATH_MAX + 64];
  snprintf(cmd_utf8, sizeof cmd_utf8, "\"%s\" --ipc-fd %llu", m->host_exe, (unsigned long long)sv[1]);
  wchar_t wcmd[CCORD_PATH_MAX + 64];
  if (MultiByteToWideChar(CP_UTF8, 0, cmd_utf8, -1, wcmd, (int)(sizeof wcmd / sizeof wcmd[0])) <= 0) {
    ccord_sock_close(sv[0]);
    ccord_sock_close(sv[1]);
    ccord_error_set(err, CCORD_ERR_IO, "host path encoding failed");
    return CCORD_ERR_IO;
  }
  STARTUPINFOW si = {.cb = sizeof si};
  PROCESS_INFORMATION pi = {0};
  wchar_t empty_env[2] = {0, 0};
  BOOL ok = CreateProcessW(nullptr, wcmd, nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                           empty_env, nullptr, &si, &pi);
  ccord_sock_close(sv[1]);
  if (!ok) {
    ccord_sock_close(sv[0]);
    ccord_error_set(err, CCORD_ERR_IO, "spawn %s failed (win32=%lu)", m->host_exe, (unsigned long)GetLastError());
    return CCORD_ERR_IO;
  }
  CloseHandle(pi.hThread);
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {0};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info)) {
      (void)AssignProcessToJobObject(job, pi.hProcess);
    }
  }
  p->proc = pi.hProcess;
  p->job = job;
  p->reaped = false;
  p->ipc = ccord_ipc_open(sv[0], err);
  if (!p->ipc) {
    kill_host_process(p);
    return err->code ? err->code : CCORD_ERR_INTERNAL;
  }
  return CCORD_OK;
#else
  int child = sv[1];
  if (child == 3) {   // dup2 onto fd 3 would keep CLOEXEC on the same fd; move it first
    int moved = fcntl(3, F_DUPFD_CLOEXEC, 4);
    close(3);
    if (moved < 0) {
      close(sv[0]);
      ccord_error_set(err, CCORD_ERR_IO, "fcntl failed");
      return CCORD_ERR_IO;
    }
    child = moved;
  }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, child, 3);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
  posix_spawn_file_actions_addclosefrom_np(&fa, 4);
#endif
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  (void)posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
  char arg_fd[] = "3";
  char arg_opt[] = "--ipc-fd";
  char *argv[] = {m->host_exe, arg_opt, arg_fd, nullptr};
  char env_buf[3][512];
  char *envp[4] = {nullptr, nullptr, nullptr, nullptr};
  size_t ne = 0;
#ifdef CCORD_SANITIZED
  static const char *const k_san_env[] = {"ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"};
  for (size_t i = 0; i < 3; i++) {
    const char *v = getenv(k_san_env[i]);
    if (v) {
      int er = snprintf(env_buf[ne], sizeof env_buf[ne], "%s=%s", k_san_env[i], v);
      if (er >= 0 && (size_t)er < sizeof env_buf[ne]) {
        envp[ne] = env_buf[ne];
        ne++;
      }
    }
  }
#else
  (void)env_buf;
  (void)ne;
#endif
  pid_t pid = 0;
  int rc = posix_spawn(&pid, m->host_exe, &fa, &attr, argv, envp);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&fa);
  close(child);
  if (rc != 0) {
    close(sv[0]);
    ccord_error_set(err, CCORD_ERR_IO, "spawn %s failed: %s", m->host_exe, strerror(rc));
    return CCORD_ERR_IO;
  }
  p->pid = pid;
  p->reaped = false;
  p->ipc = ccord_ipc_open(sv[0], err);
  if (!p->ipc) {
    kill_host_process(p);
    return err->code ? err->code : CCORD_ERR_INTERNAL;
  }
  return CCORD_OK;
#endif
}

static void storage_setup(ccord_plugins *m, plugin *p)
{
  p->storage_enabled = m->has_data_dir;
  if (!p->storage_enabled) return;
  char dir[sizeof p->storage_dir];
  int n = snprintf(dir, sizeof dir, "%s/plugins/%s", m->data_dir, p->m.id);
  if (n < 0 || (size_t)n >= sizeof dir) {
    p->storage_enabled = false;
    CCORD_LOG_W("plugin %s: storage directory path truncated", p->m.id);
    return;
  }
  memcpy(p->storage_dir, dir, (size_t)n + 1);
  storage_load(p);
}

static plugin *new_record(ccord_plugins *m)
{
  if (m->nplugins == m->capplugins) {
    size_t ncap = m->capplugins ? m->capplugins * 2 : 16;
    plugin *np = ccord_realloc_array(m->plugins, ncap, sizeof *np);
    if (!np) return nullptr;
    m->plugins = np;
    m->capplugins = ncap;
  }
  plugin *p = &m->plugins[m->nplugins++];
  memset(p, 0, sizeof *p);
  p->used = true;
  p->enabled = true;
#if !CCORD_OS_WINDOWS && !CCORD_OS_ANDROID
  p->pid = -1;
#endif
  p->next_id = 1;
  p->state = CCORD_PLUGIN_STARTING;
  return p;
}

static bool consent_load(const ccord_plugins *m, const char *id, uint64_t perms, bool *out_enabled, bool *out_consent, ccord_perm_set *out_consented_perms)
{
  *out_enabled = true;
  *out_consent = ccord_perm_max_class(perms) == CCORD_PERM_CLASS_SAFE;
  *out_consented_perms = ccord_perm_max_class(perms) == CCORD_PERM_CLASS_SAFE ? perms : 0;
  if (!m->has_data_dir) return false;
  char path[PBUF + 32];
  snprintf(path, sizeof path, "%s/plugins/consent.json", m->data_dir);
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > (1 << 20)) { fclose(f); return false; }
  char *buf = ccord_malloc((size_t)sz + 1);
  if (!buf) { fclose(f); return false; }
  size_t rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[rd] = '\0';
  ccord_arena ar; ccord_arena_init(&ar, 0); ccord_error err = {0};
  const ccord_json *root = ccord_json_parse(&ar, buf, rd, &k_json_limits, &err);
  ccord_free(buf);
  const ccord_json *entry = root ? ccord_json_get(root, id) : nullptr;
  if (entry) {
    *out_enabled = ccord_json_bool(ccord_json_get(entry, "enabled"), true);
    int64_t ap = 0;
    if (ccord_json_int64(ccord_json_get(entry, "perms"), &ap)) {
      *out_consented_perms = (uint64_t)ap;
      if (((uint64_t)ap & perms) == perms) *out_consent = true;
    } else {
      const ccord_json *consent_bool = ccord_json_get(entry, "consent");
      if (consent_bool && ccord_json_bool(consent_bool, false)) {
        *out_consent = true;
        *out_consented_perms = perms;
      }
    }
  }
  ccord_arena_free(&ar);
  return entry != nullptr;
}

static void consent_save(const ccord_plugins *m)
{
  if (!m->has_data_dir) return;
  char dir[PBUF];
  snprintf(dir, sizeof dir, "%s/plugins", m->data_dir);
  mkdir_ok(m->data_dir);
  mkdir_ok(dir);
  char path[PBUF + 32];
  snprintf(path, sizeof path, "%s/consent.json", dir);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  for (size_t i = 0; i < m->nplugins; i++) {
    const plugin *p = &m->plugins[i];
    if (!p->used) continue;
    ccord_jw_key(&w, p->m.id);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "enabled"); ccord_jw_bool(&w, p->enabled);
    ccord_jw_key(&w, "perms"); ccord_jw_int(&w, (int64_t)p->consented_perms);
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) {
    ccord_error err = {0};
    (void)ccord_file_write_atomic(path, b.data, b.len, 0600, false, &err);
  }
  ccord_buf_free(&b);
}

// Registers a plugin that could not start: it shows in Settings with its reason.
static void record_failed(ccord_plugins *m, const char *dir_name, const char *reason)
{
  plugin *p = new_record(m);
  if (!p) {
    CCORD_LOG_W("extensions: too many plugins, skipping %s", dir_name);
    return;
  }
  set_text(p->m.id, sizeof p->m.id, dir_name);
  set_text(p->m.name, sizeof p->m.name, dir_name);
  set_text(p->m.version, sizeof p->m.version, "0.0.0");
  p->state = CCORD_PLUGIN_FAILED;
  set_text(p->error, sizeof p->error, reason);
  CCORD_LOG_E("plugin %s rejected: %s", dir_name, reason);
  publish_state(m, p);
}

static bool read_file(const char *path, size_t max, char **out, size_t *len, char *why, size_t why_cap)
{
  uint64_t fsz = 0;
  if (!ccord_path_is_file(path) || !ccord_file_size(path, &fsz) || fsz > (uint64_t)max) {
    set_text(why, why_cap, "file is not a regular file of acceptable size");
    return false;
  }
  FILE *f = fopen(path, "rb");
  if (!f) {
    snprintf(why, why_cap, "cannot open %s: %s", path, strerror(errno));
    return false;
  }
  size_t n = (size_t)fsz;
  char *buf = ccord_malloc(n + 1);
  if (!buf) {
    set_text(why, why_cap, "out of memory");
    fclose(f);
    return false;
  }
  size_t got = n ? fread(buf, 1, n, f) : 0;
  fclose(f);
  if (got != n) {
    ccord_free(buf);
    set_text(why, why_cap, "short read");
    return false;
  }
  buf[n] = '\0';
  *out = buf;
  *len = n;
  return true;
}

static bool resolve_real_path(const char *in, char *out, size_t cap)
{
#if CCORD_OS_WINDOWS
  if (!in || !out || cap == 0) return false;
  return _fullpath(out, in, cap) != nullptr && ccord_path_exists(out);
#else
  if (!in || !out || cap < PATH_MAX) return false;
  return realpath(in, out) != nullptr;
#endif
}

static void consider_dir(ccord_plugins *m, const char *dir_path, const char *dir_name, size_t *started)
{
  char manifest_path[PBUF + 64];
  int mp_len = snprintf(manifest_path, sizeof manifest_path, "%s/plugin.json", dir_path);
  if (mp_len < 0 || (size_t)mp_len >= sizeof manifest_path) return;
  char *mbuf = nullptr;
  size_t mlen = 0;
  char why[256];
  if (!read_file(manifest_path, 16u << 20, &mbuf, &mlen, why, sizeof why)) return;   // not a plugin directory
  ccord_manifest man;
  ccord_error err = {0};
  ccord_status st = ccord_manifest_parse(mbuf, mlen, &man, &err);
  ccord_free(mbuf);
  if (st != CCORD_OK) {
    record_failed(m, dir_name, err.message);
    ccord_contrib_add_plugin_log(m->contrib, dir_name, m->now_ms, (uint8_t)CCORD_LOG_ERROR, err.message);
    return;
  }
  char compat_why[160] = "";
  if (!ccord_manifest_supports(&man, m->plat_bit, m->plat_capabilities, compat_why, sizeof compat_why)) {
    CCORD_LOG_I("extensions: skipping %s (%s)", man.id, compat_why);
    record_failed(m, man.id, compat_why);
    ccord_contrib_add_plugin_log(m->contrib, man.id, m->now_ms, (uint8_t)CCORD_LOG_INFO, compat_why);
    return;
  }
  for (size_t i = 0; i < m->nplugins; i++) {
    if (strcmp(m->plugins[i].m.id, man.id) == 0) {
      CCORD_LOG_W("extensions: duplicate plugin id %s in %s skipped", man.id, dir_path);
      return;
    }
  }
  // main.lua must resolve inside the plugin directory (no symlink escape).
  char real_dir[CCORD_PATH_MAX], real_main[CCORD_PATH_MAX], main_path[PBUF + 128];
  if (!resolve_real_path(dir_path, real_dir, sizeof real_dir)) {
    record_failed(m, man.id, "plugin directory cannot be resolved");
    return;
  }
  int main_len = snprintf(main_path, sizeof main_path, "%s/%s", dir_path, man.main);
  if (main_len < 0 || (size_t)main_len >= sizeof main_path) {
    record_failed(m, man.id, "main file path too long");
    return;
  }
  if (!resolve_real_path(main_path, real_main, sizeof real_main)) {
    record_failed(m, man.id, "main file is missing");
    return;
  }
  size_t dl = strlen(real_dir);
  bool under_dir = strncmp(real_main, real_dir, dl) == 0 && (real_main[dl] == '/' || real_main[dl] == '\\');
  if (!under_dir) {
    record_failed(m, man.id, "main file resolves outside the plugin directory");
    return;
  }
  plugin *p = new_record(m);
  if (!p) {
    CCORD_LOG_W("extensions: too many plugins, skipping %s", man.id);
    return;
  }
  p->m = man;
  p->perm_str[0] = '\0';
  ccord_perm_list(man.perms, p->perm_str, sizeof p->perm_str);
  int r = snprintf(p->dir, sizeof p->dir, "%s", real_dir);
  if (r < 0 || (size_t)r >= sizeof p->dir) {
    record_failed(m, man.id, "plugin directory path too long");
    return;
  }
  if (!read_file(real_main, 16u << 20, &p->source, &p->source_len, why, sizeof why)) {
    p->state = CCORD_PLUGIN_FAILED;
    set_text(p->error, sizeof p->error, why);
    CCORD_LOG_E("plugin %s rejected: %s", man.id, why);
    publish_state(m, p);
    ccord_contrib_add_plugin_log(m->contrib, man.id, m->now_ms, (uint8_t)CCORD_LOG_ERROR, why);
    return;
  }
  storage_setup(m, p);
  p->consented_perms = ccord_perm_max_class(man.perms) == CCORD_PERM_CLASS_SAFE ? man.perms : 0;
  consent_load(m, man.id, man.perms, &p->enabled, &p->consent_granted, &p->consented_perms);
  if (ccord_perm_max_class(man.perms) == CCORD_PERM_CLASS_SAFE) {
    p->consent_granted = true;
    p->consented_perms = man.perms;
  }
  if (!p->consent_granted) {
    p->enabled = false;
    p->state = CCORD_PLUGIN_NEEDS_CONSENT;
    publish_state(m, p);
    return;
  }
  if (!p->enabled) {
    p->state = CCORD_PLUGIN_STOPPED;
    publish_state(m, p);
    return;
  }
  publish_state(m, p);
  if (spawn_host(m, p, &err) != CCORD_OK) {
    char reason[256];
    snprintf(reason, sizeof reason, "could not start host: %s", err.message);
    fail_plugin(m, p, reason);
    return;
  }
  (*started)++;
}

size_t ccord_plugins_load_all(ccord_plugins *m)
{
  if (!m) return 0;
  size_t started = 0;
  for (size_t i = 0; i < m->nsearch; i++) {
    ccord_dir *d = ccord_dir_open(m->search[i]);
    if (!d) {
      CCORD_LOG_D("extensions: %s not readable", m->search[i]);
      continue;
    }
    char ename[256];
    bool is_dir = false;
    while (ccord_dir_next(d, ename, sizeof ename, &is_dir)) {
      if (ename[0] == '.') continue;
      char path[PBUF + 32];
      int n = snprintf(path, sizeof path, "%s/%s", m->search[i], ename);
      if (n < 0 || (size_t)n >= sizeof path) continue;
      if (!is_dir && !ccord_path_is_dir(path)) continue;
      consider_dir(m, path, ename, &started);
    }
    ccord_dir_close(d);
  }
  return started;
}

// ---------------------------------------------------------------- lifecycle

static void stop_host(ccord_plugins *m, plugin *p)
{
  if (p->state == CCORD_PLUGIN_STARTING || p->state == CCORD_PLUGIN_READY) {
    if (p->ipc) {
      ccord_error err = {0};
      (void)ccord_ipc_send_notify(p->ipc, "shutdown", "{}", &err);
      (void)ccord_ipc_flush(p->ipc, 100, &err);
    }
    storage_save(m, p, m->now_ms);
    char reason[160];
    if (host_is_running(p) && !reap_wait(p, 1000, reason, sizeof reason)) {
      kill_host_process(p);
    }
    if (p->ipc) {
      ccord_ipc_close(p->ipc);
      p->ipc = nullptr;
    }
    p->state = CCORD_PLUGIN_STOPPED;
    memset(p->pend, 0, sizeof p->pend);
    ccord_contrib_remove_owner(m->contrib, p->m.id);
    publish_state(m, p);
  } else if (p->ipc) {
    ccord_ipc_close(p->ipc);
    p->ipc = nullptr;
  }
}

void ccord_plugins_shutdown(ccord_plugins *m)
{
  if (!m) return;
  for (size_t i = 0; i < m->nplugins; i++) stop_host(m, &m->plugins[i]);
}

void ccord_plugins_destroy(ccord_plugins *m)
{
  if (!m) return;
  ccord_catalog_destroy(m->catalog);
  m->catalog = nullptr;
  ccord_plugins_shutdown(m);
  for (size_t i = 0; i < m->nplugins; i++) {
    plugin *p = &m->plugins[i];
    for (size_t k = 0; k < p->nitems; k++) ccord_free(p->items[k].json);
    ccord_free(p->items);
    ccord_free(p->source);
  }
  ccord_free(m->plugins);
  if (m->bus && m->sub) {
    ccord_bus_unsubscribe(m->bus, m->sub);
  }
  ccord_free(m);
}

static void start_plugin_host(ccord_plugins *m, plugin *p)
{
  if (!p || p->state == CCORD_PLUGIN_STARTING || p->state == CCORD_PLUGIN_READY) return;
  p->error[0] = '\0';
  p->hello_seen = false;
  p->subs = 0;
  p->menu_count = 0;
  memset(p->pend, 0, sizeof p->pend);
  p->state = CCORD_PLUGIN_STARTING;
  publish_state(m, p);
  ccord_error err = {0};
  if (spawn_host(m, p, &err) != CCORD_OK) {
    char reason[256];
    snprintf(reason, sizeof reason, "could not start host: %s", err.message);
    fail_plugin(m, p, reason);
  }
}

static void on_plugin_set_enabled(void *ud, const char *id, bool enabled)
{
  ccord_plugins *m = ud;
  plugin *p = find_owner(m, id);
  if (!p) return;
  if (!enabled) {
    p->enabled = false;
    stop_host(m, p);
    p->state = p->consent_granted ? CCORD_PLUGIN_STOPPED : CCORD_PLUGIN_NEEDS_CONSENT;
    publish_state(m, p);
    consent_save(m);
    return;
  }
  if (!p->consent_granted) {
    p->state = CCORD_PLUGIN_NEEDS_CONSENT;
    publish_state(m, p);
    return;
  }
  p->enabled = true;
  consent_save(m);
  start_plugin_host(m, p);
}

static void on_plugin_consent_accept(void *ud, const char *id)
{
  ccord_plugins *m = ud;
  plugin *p = find_owner(m, id);
  if (!p) return;
  p->consent_granted = true;
  p->consented_perms = p->m.perms;
  p->enabled = true;
  consent_save(m);
  start_plugin_host(m, p);
}

static void on_plugin_reload(void *ud, const char *id)
{
  ccord_plugins *m = ud;
  plugin *p = find_owner(m, id);
  if (!p || !p->dir[0]) return;
  bool was_enabled = p->enabled;
  stop_host(m, p);
  char manifest_path[PBUF + 64], main_path[PBUF + 128], why[256];
  snprintf(manifest_path, sizeof manifest_path, "%s/plugin.json", p->dir);
  char *mbuf = nullptr; size_t mlen = 0;
  if (!read_file(manifest_path, 16u << 20, &mbuf, &mlen, why, sizeof why)) {
    fail_plugin(m, p, why);
    return;
  }
  ccord_manifest man; ccord_error err = {0};
  ccord_status st = ccord_manifest_parse(mbuf, mlen, &man, &err);
  ccord_free(mbuf);
  if (st != CCORD_OK) { fail_plugin(m, p, err.message); return; }
  snprintf(main_path, sizeof main_path, "%s/%s", p->dir, man.main);
  char *src = nullptr; size_t slen = 0;
  if (!read_file(main_path, 16u << 20, &src, &slen, why, sizeof why)) { fail_plugin(m, p, why); return; }
  ccord_free(p->source);
  p->source = src;
  p->source_len = slen;
  p->m = man;
  p->perm_str[0] = '\0';
  ccord_perm_list(man.perms, p->perm_str, sizeof p->perm_str);

  bool safe_class = ccord_perm_max_class(man.perms) == CCORD_PERM_CLASS_SAFE;
  if (safe_class) {
    p->consent_granted = true;
    p->consented_perms = man.perms;
  } else if (!p->consent_granted || (man.perms & ~p->consented_perms) != 0) {
    p->consent_granted = false;
    p->enabled = false;
    p->state = CCORD_PLUGIN_NEEDS_CONSENT;
    publish_state(m, p);
    consent_save(m);
    return;
  }

  p->enabled = was_enabled;
  consent_save(m);
  if (p->enabled) {
    start_plugin_host(m, p);
  } else {
    p->state = CCORD_PLUGIN_STOPPED;
    publish_state(m, p);
  }
}

bool ccord_plugins_export_cce(ccord_plugins *m, const char *id, const char *dest_path, char *err_buf, size_t err_cap)
{
  if (!m || !id || !dest_path) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "invalid arguments");
    return false;
  }
  plugin *p = find_owner(m, id);
  if (!p || !p->dir[0]) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "plugin not found or has no directory");
    return false;
  }
  return ccord_zip_pack_dir(p->dir, dest_path, err_buf, err_cap);
}

bool ccord_plugins_import_package(ccord_plugins *m, const char *pkg_path, char *err_buf, size_t err_cap)
{
  if (!m || !pkg_path || !m->has_data_dir) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "plugin data directory unavailable");
    return false;
  }
  char base[PBUF], tmp_dir[PBUF + 32], inst_root[PBUF + 32];
  snprintf(base, sizeof base, "%s/plugins", m->data_dir);
  mkdir_ok(m->data_dir);
  mkdir_ok(base);
  snprintf(inst_root, sizeof inst_root, "%s/installed", base);
  mkdir_ok(inst_root);
  snprintf(tmp_dir, sizeof tmp_dir, "%s/.import_tmp_%llu", base, (unsigned long long)m->now_ms);
  if (!ccord_zip_extract(pkg_path, tmp_dir, err_buf, err_cap)) {
    rm_dir_recursive(tmp_dir);
    return false;
  }

  char man_path[PBUF + 64], why[256];
  snprintf(man_path, sizeof man_path, "%s/plugin.json", tmp_dir);
  char *mbuf = nullptr; size_t mlen = 0;
  if (!read_file(man_path, 16u << 20, &mbuf, &mlen, why, sizeof why)) {
    rm_dir_recursive(tmp_dir);
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "package is missing plugin.json");
    return false;
  }
  ccord_manifest man; ccord_error err = {0};
  ccord_status st = ccord_manifest_parse(mbuf, mlen, &man, &err);
  ccord_free(mbuf);
  if (st != CCORD_OK) {
    rm_dir_recursive(tmp_dir);
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", err.message);
    return false;
  }

  plugin *existing = find_owner(m, man.id);
  if (strncmp(man.id, "dev.ccord.", 10) == 0) {
    rm_dir_recursive(tmp_dir);
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "cannot overwrite bundled extension");
    return false;
  }

  char final_dir[PBUF + 128];
  snprintf(final_dir, sizeof final_dir, "%s/%s", inst_root, man.id);
  if (ccord_path_exists(final_dir)) {
    rm_dir_recursive(final_dir);
  }
  if (rename(tmp_dir, final_dir) != 0) {
    rm_dir_recursive(tmp_dir);
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "rename failed: %s", strerror(errno));
    return false;
  }

  if (existing) {
    set_text(existing->dir, sizeof existing->dir, final_dir);
    on_plugin_reload(m, man.id);
    return true;
  }
  size_t started = 0;
  consider_dir(m, final_dir, man.id, &started);
  return true;
}

static bool cb_export_cce(void *ud, const char *id, const char *dest, char *eb, size_t ec)
{
  return ccord_plugins_export_cce(ud, id, dest, eb, ec);
}

static bool cb_import_pkg(void *ud, const char *pkg, char *eb, size_t ec)
{
  return ccord_plugins_import_package(ud, pkg, eb, ec);
}

bool ccord_plugins_uninstall(ccord_plugins *m, const char *id, char *err_buf, size_t err_cap)
{
  if (!m || !id || !id[0]) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "invalid plugin id");
    return false;
  }
  plugin *p = find_owner(m, id);
  if (!p) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "plugin not found");
    return false;
  }
  if (p->is_bundled || strncmp(p->m.id, "dev.ccord.", 10) == 0) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "cannot uninstall bundled extension");
    return false;
  }
  stop_host(m, p);
  ccord_contrib_remove_owner(m->contrib, p->m.id);
  (void)ccord_contrib_remove_plugin(m->contrib, p->m.id);

  if (m->has_data_dir) {
    char inst_dir[PBUF + 128];
    snprintf(inst_dir, sizeof inst_dir, "%s/plugins/installed/%s", m->data_dir, p->m.id);
    if (ccord_path_exists(inst_dir)) rm_dir_recursive(inst_dir);
  }
  for (size_t k = 0; k < p->nitems; k++) ccord_free(p->items[k].json);
  ccord_free(p->items);
  ccord_free(p->source);
  p->used = false;
  p->items = nullptr;
  p->nitems = 0;
  p->source = nullptr;
  p->source_len = 0;
  consent_save(m);
  if (m->catalog) ccord_catalog_on_uninstalled(m->catalog, id);
  return true;
}

static bool cb_uninstall(void *ud, const char *id, char *eb, size_t ec)
{
  return ccord_plugins_uninstall(ud, id, eb, ec);
}

static void cb_cat_refresh(void *ud) { ccord_plugins *m = ud; if (m->catalog) ccord_catalog_refresh(m->catalog); }
static void cb_cat_query(void *ud, const char *q, int tier, int api_class, const char *sort, bool append_next_page)
{
  ccord_plugins *m = ud;
  if (m->catalog) ccord_catalog_query(m->catalog, q, tier, api_class, sort, append_next_page);
}
static void cb_cat_fetch_detail(void *ud, const char *id)
{
  ccord_plugins *m = ud;
  if (m->catalog) ccord_catalog_fetch_detail(m->catalog, id);
}
static void cb_cat_check_updates(void *ud) { ccord_plugins *m = ud; if (m->catalog) ccord_catalog_check_updates(m->catalog); }
static void cb_cat_install(void *ud, const char *id) { ccord_plugins *m = ud; if (m->catalog) ccord_catalog_install(m->catalog, id); }
static void cb_cat_set_prefs(void *ud, const ccord_catalog_prefs *pr) { ccord_plugins *m = ud; if (m->catalog) ccord_catalog_set_prefs(m->catalog, pr); }
static void cb_cat_set_auto(void *ud, const char *id, bool on) { ccord_plugins *m = ud; if (m->catalog) ccord_catalog_set_plugin_auto_update(m->catalog, id, on); }
static void cb_cat_run_auto(void *ud) { ccord_plugins *m = ud; if (m->catalog) ccord_catalog_run_auto_updates(m->catalog); }

static bool cat_get_local_plugin(void *ud, const char *id, char *ver_out, size_t ver_cap,
                                 uint64_t *consented_perms_out, bool *is_bundled_out)
{
  ccord_plugins *m = ud;
  plugin *p = find_owner(m, id);
  if (!p) return false;
  set_text(ver_out, ver_cap, p->m.version);
  if (consented_perms_out) *consented_perms_out = p->consented_perms;
  if (is_bundled_out) *is_bundled_out = p->is_bundled;
  return true;
}

static size_t cat_list_local_plugins(void *ud, char (*ids_out)[65], size_t cap)
{
  ccord_plugins *m = ud;
  size_t n = 0;
  for (size_t i = 0; i < m->nplugins && n < cap; i++) {
    const plugin *p = &m->plugins[i];
    if (p->used && !p->is_bundled && p->m.id[0]) {
      set_text(ids_out[n++], 65, p->m.id);
    }
  }
  return n;
}

static void cat_revoke_plugin(void *ud, const char *id, const char *reason)
{
  ccord_plugins *m = ud;
  plugin *p = find_owner(m, id);
  if (!p) return;
  stop_host(m, p);
  p->enabled = false;
  p->state = CCORD_PLUGIN_FAILED;
  set_text(p->error, sizeof p->error, reason ? reason : "Revoked by C-Cord moderators");
  consent_save(m);
  publish_state(m, p);
}

static void cat_sync_installed_info(void *ud)
{
  ccord_plugins *m = ud;
  for (size_t i = 0; i < m->nplugins; i++) {
    if (m->plugins[i].used) publish_state(m, &m->plugins[i]);
  }
}

static void cat_toast(void *ud, const char *msg, uint8_t level)
{
  ccord_plugins *m = ud;
  if (m->toast_show) m->toast_show(m->toast_ud, msg, level);
}

ccord_plugins *ccord_plugins_create(const ccord_plugins_config *cfg, ccord_error *err)
{
  if (!cfg || !cfg->bus || !cfg->store || !cfg->contrib ||
      (!cfg->in_process && !cfg->host_exe) ||
      (cfg->in_process && !cfg->in_process_host_fn)) {
    ccord_error_set(err, CCORD_ERR_INVALID, "plugins: bus, store, contrib and host_exe (or in_process_host_fn) are required");
    return nullptr;
  }
  if (cfg->search_dir_count > MAX_SEARCH) {
    ccord_error_set(err, CCORD_ERR_RANGE, "plugins: too many search directories");
    return nullptr;
  }
  ccord_plugins *m = ccord_calloc(1, sizeof *m);
  if (!m) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "plugins: out of memory");
    return nullptr;
  }
  m->bus = cfg->bus;
  m->store = cfg->store;
  m->session = cfg->session;
  m->contrib = cfg->contrib;
  m->clipboard_write = cfg->clipboard_write;
  m->clipboard_ud = cfg->clipboard_ud;
  m->toast_show = cfg->toast_show;
  m->toast_ud = cfg->toast_ud;
  m->in_process = cfg->in_process;
  m->in_process_host_fn = cfg->in_process_host_fn;
  m->plat_ops = cfg->platform_ops;
  m->plat_bit = ccord_platform_current_bit();
  if (cfg->has_platform_info) {
    snprintf(m->plat_os, sizeof m->plat_os, "%s",
             (cfg->platform_info.os && cfg->platform_info.os[0]) ? cfg->platform_info.os : ccord_os_name());
    snprintf(m->plat_arch, sizeof m->plat_arch, "%s",
             (cfg->platform_info.arch && cfg->platform_info.arch[0]) ? cfg->platform_info.arch : ccord_os_arch());
    snprintf(m->plat_form_factor, sizeof m->plat_form_factor, "%s",
             (cfg->platform_info.form_factor && cfg->platform_info.form_factor[0])
               ? cfg->platform_info.form_factor
               : (cfg->platform_info.is_mobile ? "phone" : "desktop"));
    m->plat_is_mobile = cfg->platform_info.is_mobile;
    m->plat_touch = cfg->platform_info.touch;
    m->plat_android_api_level = cfg->platform_info.android_api_level;
    m->plat_capabilities = cfg->platform_info.capabilities;
    m->plat_state = cfg->platform_state;
    if (strcmp(m->plat_os, "android") == 0) m->plat_bit = CCORD_PLAT_ANDROID;
    else if (strcmp(m->plat_os, "macos") == 0) m->plat_bit = CCORD_PLAT_MACOS;
    else if (strcmp(m->plat_os, "windows") == 0) m->plat_bit = CCORD_PLAT_WINDOWS;
    else if (strcmp(m->plat_os, "linux") == 0) m->plat_bit = CCORD_PLAT_LINUX;
  } else {
    snprintf(m->plat_os, sizeof m->plat_os, "%s", ccord_os_name());
    snprintf(m->plat_arch, sizeof m->plat_arch, "%s", ccord_os_arch());
    m->plat_is_mobile = ccord_os_is_mobile();
    snprintf(m->plat_form_factor, sizeof m->plat_form_factor, "%s", m->plat_is_mobile ? "phone" : "desktop");
    m->plat_touch = m->plat_is_mobile;
    m->plat_android_api_level = ccord_os_android_api_level();
    m->plat_capabilities = CCORD_CAP_PANELS | (m->plat_is_mobile ? (CCORD_CAP_TOUCH | CCORD_CAP_HAPTICS | CCORD_CAP_SHARE | CCORD_CAP_SAFE_AREA | CCORD_CAP_KEYBOARD_CONTROL | CCORD_CAP_ORIENTATION_CONTROL | CCORD_CAP_KEEP_SCREEN_ON | CCORD_CAP_NOTIFICATIONS)
                                                                 : (CCORD_CAP_HOVER | CCORD_CAP_NOTIFICATIONS | CCORD_CAP_TRAY | CCORD_CAP_GLOBAL_SHORTCUTS | CCORD_CAP_SCREEN_CAPTURE | CCORD_CAP_RPC | CCORD_CAP_KEEP_SCREEN_ON));
    m->plat_state.viewport_scale = 1.0f;
    m->plat_state.landscape = !m->plat_is_mobile;
  }
  if (cfg->host_exe) {
    if (strlen(cfg->host_exe) >= sizeof m->host_exe) {
      ccord_error_set(err, CCORD_ERR_RANGE, "plugins: host path too long");
      ccord_free(m);
      return nullptr;
    }
    snprintf(m->host_exe, sizeof m->host_exe, "%s", cfg->host_exe);
  }
  if (cfg->data_dir) {
    int dr = snprintf(m->data_dir, sizeof m->data_dir, "%s", cfg->data_dir);
    m->has_data_dir = dr >= 0 && (size_t)dr < sizeof m->data_dir;
  }
  for (size_t i = 0; i < cfg->search_dir_count; i++) {
    if (!cfg->search_dirs[i] || snprintf(m->search[i], sizeof m->search[i], "%s", cfg->search_dirs[i]) >= (int)sizeof m->search[i]) {
      ccord_error_set(err, CCORD_ERR_RANGE, "plugins: search directory too long");
      ccord_free(m);
      return nullptr;
    }
    m->nsearch++;
  }
  ccord_contrib_handlers h = {
    .ud = m,
    .menu_activate = on_menu_activate,
    .command_run = on_command_run,
    .panel_event = on_panel_event,
    .plugin_set_enabled = on_plugin_set_enabled,
    .plugin_consent_accept = on_plugin_consent_accept,
    .plugin_reload = on_plugin_reload,
    .plugin_export_cce = cb_export_cce,
    .plugin_import_package = cb_import_pkg,
    .plugin_uninstall = cb_uninstall,
    .catalog_refresh = cb_cat_refresh,
    .catalog_query = cb_cat_query,
    .catalog_fetch_detail = cb_cat_fetch_detail,
    .catalog_check_updates = cb_cat_check_updates,
    .catalog_install = cb_cat_install,
    .catalog_set_prefs = cb_cat_set_prefs,
    .catalog_set_plugin_auto_update = cb_cat_set_auto,
    .catalog_run_auto_updates = cb_cat_run_auto,
  };
  ccord_contrib_set_handlers(m->contrib, &h);
  ccord_catalog_host_cbs ccbs = {
    .ud = m,
    .get_local_plugin = cat_get_local_plugin,
    .list_local_plugins = cat_list_local_plugins,
    .import_verified_cce = cb_import_pkg,
    .revoke_plugin = cat_revoke_plugin,
    .sync_installed_info = cat_sync_installed_info,
    .toast = cat_toast,
  };
  m->catalog = ccord_catalog_create(m->has_data_dir ? m->data_dir : nullptr, m->contrib, &cfg->catalog_http, &ccbs);
  m->sub = ccord_bus_subscribe(m->bus, CCORD_EVT_ANY, on_bus_event, m);
  if (m->sub == 0) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "plugins: bus subscription failed");
    ccord_plugins_destroy(m);
    return nullptr;
  }
  return m;
}

int ccord_plugins_host_pid(const ccord_plugins *m, const char *owner)
{
  if (!m || !owner) return -1;
  for (size_t i = 0; i < m->nplugins; i++) {
    const plugin *p = &m->plugins[i];
    if (p->used && strcmp(p->m.id, owner) == 0) {
      if (!p->ipc || !host_is_running(p)) return -1;
      if (p->in_process) return (int)ccord_os_pid();
#if CCORD_OS_WINDOWS
      return (int)GetProcessId(p->proc);
#elif CCORD_OS_ANDROID
      return (int)ccord_os_pid();
#else
      return (int)p->pid;
#endif
    }
  }
  return -1;
}

static void broadcast_event_json(ccord_plugins *m, uint32_t bit, const char *name, const char *data_json)
{
  if (!m || !name || !data_json) return;
  char payload[512];
  int n = snprintf(payload, sizeof payload, "{\"name\":\"%s\",\"data\":%s}", name, data_json);
  if (n <= 0 || (size_t)n >= sizeof payload) return;
  for (size_t i = 0; i < m->nplugins; i++) {
    plugin *p = &m->plugins[i];
    if (!p->used || p->state != CCORD_PLUGIN_READY || !p->ipc) continue;
    if ((p->subs & bit) == 0) continue;
    ccord_error err = {0};
    (void)ccord_ipc_send_notify(p->ipc, "event", payload, &err);
  }
}

void ccord_plugins_update_platform_state(ccord_plugins *m, const ccord_plugin_platform_state *st)
{
  if (!m || !st) return;
  ccord_plugin_platform_state prev = m->plat_state;
  m->plat_state = *st;

  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "safe_top"); ccord_jw_int(&w, (int64_t)st->safe_top);
  ccord_jw_key(&w, "safe_right"); ccord_jw_int(&w, (int64_t)st->safe_right);
  ccord_jw_key(&w, "safe_bottom"); ccord_jw_int(&w, (int64_t)st->safe_bottom);
  ccord_jw_key(&w, "safe_left"); ccord_jw_int(&w, (int64_t)st->safe_left);
  ccord_jw_key(&w, "viewport_w"); ccord_jw_int(&w, (int64_t)st->viewport_w);
  ccord_jw_key(&w, "viewport_h"); ccord_jw_int(&w, (int64_t)st->viewport_h);
  ccord_jw_key(&w, "viewport_scale"); ccord_jw_int(&w, (int64_t)(st->viewport_scale > 0.0f ? st->viewport_scale : 1.0f));
  ccord_jw_key(&w, "keyboard_height"); ccord_jw_int(&w, (int64_t)st->keyboard_height);
  ccord_jw_key(&w, "keyboard_visible"); ccord_jw_bool(&w, st->keyboard_visible);
  ccord_jw_key(&w, "landscape"); ccord_jw_bool(&w, st->landscape);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) {
    for (size_t i = 0; i < m->nplugins; i++) {
      plugin *p = &m->plugins[i];
      if (!p->used || p->state != CCORD_PLUGIN_READY || !p->ipc) continue;
      ccord_error err = {0};
      (void)ccord_ipc_send_notify(p->ipc, "platform.state", ccord_buf_cstr(&b), &err);
    }
  }
  ccord_buf_free(&b);

  if (prev.landscape != st->landscape) {
    char dj[160];
    snprintf(dj, sizeof dj, "{\"orientation\":\"%s\",\"width\":%d,\"height\":%d}",
             st->landscape ? "landscape" : "portrait", (int)st->viewport_w, (int)st->viewport_h);
    broadcast_event_json(m, EVB_PLAT_ORIENTATION, "platform.orientation", dj);
  }
  if (prev.keyboard_visible != st->keyboard_visible || prev.keyboard_height != st->keyboard_height) {
    char dj[128];
    snprintf(dj, sizeof dj, "{\"visible\":%s,\"height\":%d}",
             st->keyboard_visible ? "true" : "false", (int)st->keyboard_height);
    broadcast_event_json(m, EVB_PLAT_KEYBOARD, "platform.keyboard", dj);
  }
  if (prev.safe_top != st->safe_top || prev.safe_right != st->safe_right ||
      prev.safe_bottom != st->safe_bottom || prev.safe_left != st->safe_left) {
    char dj[160];
    snprintf(dj, sizeof dj, "{\"top\":%d,\"right\":%d,\"bottom\":%d,\"left\":%d}",
             (int)st->safe_top, (int)st->safe_right, (int)st->safe_bottom, (int)st->safe_left);
    broadcast_event_json(m, EVB_PLAT_SAFE_AREA, "platform.safe_area", dj);
  }
}

void ccord_plugins_dispatch_lifecycle(ccord_plugins *m, const char *state)
{
  if (!m || !state || !state[0]) return;
  char dj[96];
  snprintf(dj, sizeof dj, "{\"state\":\"%.32s\"}", state);
  broadcast_event_json(m, EVB_APP_LIFECYCLE, "app.lifecycle", dj);
}
