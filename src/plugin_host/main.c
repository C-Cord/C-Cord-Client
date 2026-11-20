// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "sandbox.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/config.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>
#include <ccord/ipc/ipc.h>

#include <lua.h>

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__linux__)
#include <signal.h>
#include <sys/prctl.h>
#endif
#if defined(__APPLE__) && !defined(CCORD_EMBEDDED_PLUGIN_HOST)
#include <sandbox.h>
#endif
#if !CCORD_OS_WINDOWS
#include <sys/resource.h>
#endif

// Exit codes are part of the contract: the main process maps them to the FAILED reason.
#define EXIT_USAGE 64
#define EXIT_INTERNAL 70
#define EXIT_PROTOCOL 71
#define EXIT_OOM 72
#define EXIT_INSTRUCTIONS 73
#define EXIT_MEMORY 74
#define EXIT_UNDECLARED 75

// Defined in sandbox.c: returns ms until earliest active Lua timer, or -1 if none.
extern int ccord_sandbox_next_timer_ms(const ccord_sandbox *sb);

#define MAX_DEFERRED 32

typedef struct deferred_msg {
  ccord_ipc_kind kind;
  uint32_t id;
  char method[64];
  char *params_json;
  size_t params_len;
} deferred_msg;

typedef struct host {
  ccord_ipc *ipc;
  ccord_sandbox *sb;
  uint32_t next_id;
  bool stop;
  int exit_code;
  const _Atomic bool *cancel;
  deferred_msg deferred[MAX_DEFERRED];
  size_t deferred_head;
  size_t deferred_count;
} host;

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

static void host_free_deferred(host *h)
{
  while (h->deferred_count > 0) {
    deferred_msg *d = &h->deferred[h->deferred_head];
    ccord_free(d->params_json);
    d->params_json = nullptr;
    d->params_len = 0;
    h->deferred_head = (h->deferred_head + 1u) % MAX_DEFERRED;
    h->deferred_count--;
  }
}

static void host_defer_msg(host *h, const ccord_ipc_msg *msg)
{
  if (!msg || !msg->method || h->deferred_count >= MAX_DEFERRED) return;
  size_t tail = (h->deferred_head + h->deferred_count) % MAX_DEFERRED;
  deferred_msg *d = &h->deferred[tail];
  memset(d, 0, sizeof *d);
  d->kind = msg->kind;
  d->id = msg->id;
  snprintf(d->method, sizeof d->method, "%s", msg->method);
  if (msg->params) {
    ccord_buf b;
    ccord_buf_init(&b, 0);
    ccord_jw w;
    ccord_jw_init(&w, &b);
    jw_copy_json(&w, msg->params);
    if (ccord_jw_ok(&w)) {
      d->params_len = b.len;
      d->params_json = ccord_strndup(ccord_buf_cstr(&b), b.len);
    }
    ccord_buf_free(&b);
  }
  h->deferred_count++;
}

static const char *safe_text(const char *s)
{
  return s && ccord_utf8_valid(s, strlen(s)) ? s : "message is not valid UTF-8";
}

static const char *str_of(const ccord_json *obj, const char *key, size_t *len)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  return j ? ccord_json_string(j, len) : nullptr;
}

static void fatal(host *h, int code)
{
  if (!h->stop) h->exit_code = code;
  h->stop = true;
}

static void send_ntf(host *h, const char *method, const char *params)
{
  ccord_error err = {0};
  if (ccord_ipc_send_notify(h->ipc, method, params, &err) != CCORD_OK) fatal(h, EXIT_PROTOCOL);
}

static void reply_ok(host *h, uint32_t id, const char *result)
{
  ccord_error err = {0};
  if (ccord_ipc_send_ok(h->ipc, id, result, &err) != CCORD_OK) fatal(h, EXIT_PROTOCOL);
}

static void reply_error(host *h, uint32_t id, const char *code, const char *msg)
{
  ccord_error err = {0};
  if (ccord_ipc_send_error(h->ipc, id, code, safe_text(msg), &err) != CCORD_OK) fatal(h, EXIT_PROTOCOL);
}

// Logs go to main as notifications; main applies the per-plugin rate limit and prefixes the plugin id.
static void svc_log(void *ud, ccord_log_level level, const char *text)
{
  host *h = ud;
  const char *lv = level == CCORD_LOG_DEBUG  ? "debug"
                 : level == CCORD_LOG_WARN   ? "warn"
                 : level == CCORD_LOG_ERROR  ? "error" : "info";
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "level");
  ccord_jw_cstr(&w, lv);
  ccord_jw_key(&w, "msg");
  ccord_jw_string(&w, text, strlen(text));
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_clear(&b);
    ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "level");
    ccord_jw_cstr(&w, lv);
    ccord_jw_key(&w, "msg");
    ccord_jw_cstr(&w, "log line dropped: not valid UTF-8");
    ccord_jw_end_object(&w);
  }
  if (ccord_jw_ok(&w)) send_ntf(h, "log", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
}

static void svc_request(void *ud, const char *method, const char *params)
{
  host *h = ud;
  ccord_error err = {0};
  uint32_t id = h->next_id++;
  if (ccord_ipc_send_request(h->ipc, id, method, params, &err) != CCORD_OK) fatal(h, EXIT_PROTOCOL);
}

static void dispatch(host *h, const ccord_ipc_msg *msg);

static bool svc_request_sync(void *ud, const char *method, const char *params, ccord_arena *arena,
                             const ccord_json **out_result, char *err_buf, size_t err_cap)
{
  host *h = ud;
  *out_result = nullptr;
  ccord_error err = {0};
  uint32_t id = h->next_id++;
  if (ccord_ipc_send_request(h->ipc, id, method, params, &err) != CCORD_OK ||
      ccord_ipc_flush(h->ipc, 1000, &err) != CCORD_OK) {
    snprintf(err_buf, err_cap, "%s", err.message[0] ? err.message : "ipc send failed");
    return false;
  }
  for (;;) {
    ccord_arena_reset(arena);
    ccord_ipc_msg msg;
    if (ccord_ipc_wait(h->ipc, arena, &msg, 3000, &err) != CCORD_OK) {
      snprintf(err_buf, err_cap, "%s", err.message[0] ? err.message : "ipc timeout");
      return false;
    }
    if (msg.kind == CCORD_IPC_REQ && strcmp(msg.method, "ping") == 0) {
      reply_ok(h, msg.id, nullptr);
      continue;
    }
    if (msg.kind == CCORD_IPC_RES && msg.id == id) {
      if (!msg.ok) {
        snprintf(err_buf, err_cap, "%s", msg.err_message ? msg.err_message : "request failed");
        return false;
      }
      *out_result = msg.result;
      return true;
    }
    if (msg.kind == CCORD_IPC_NTF && strcmp(msg.method, "shutdown") == 0) {
      h->stop = true;
      snprintf(err_buf, err_cap, "shutting down");
      return false;
    }
    if (msg.kind == CCORD_IPC_REQ || msg.kind == CCORD_IPC_NTF) {
      host_defer_msg(h, &msg);
    }
  }
}

// A limit ends the process at once; main sees the exit code and marks the plugin FAILED.
static bool limit_stop(host *h)
{
  ccord_sandbox_limit lim = ccord_sandbox_limit_hit(h->sb);
  if (lim == CCORD_SB_LIMIT_NONE) return false;
  int code = lim == CCORD_SB_LIMIT_UNDECLARED ? EXIT_UNDECLARED
           : lim == CCORD_SB_LIMIT_MEMORY     ? EXIT_MEMORY
                                              : EXIT_INSTRUCTIONS;
  fatal(h, code);
  return true;
}

static float num_of(const ccord_json *obj, const char *key, float fallback)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  return j ? (float)ccord_json_number(j, (double)fallback) : fallback;
}

static void parse_platform_state(const ccord_json *obj, ccord_sandbox_platform_state *st)
{
  if (!obj || !st || ccord_json_type_of(obj) != CCORD_JSON_OBJECT) return;
  st->safe_top = num_of(obj, "safe_top", st->safe_top);
  st->safe_right = num_of(obj, "safe_right", st->safe_right);
  st->safe_bottom = num_of(obj, "safe_bottom", st->safe_bottom);
  st->safe_left = num_of(obj, "safe_left", st->safe_left);
  st->viewport_w = num_of(obj, "viewport_w", st->viewport_w);
  st->viewport_h = num_of(obj, "viewport_h", st->viewport_h);
  st->viewport_scale = num_of(obj, "viewport_scale", st->viewport_scale > 0.0f ? st->viewport_scale : 1.0f);
  st->keyboard_height = num_of(obj, "keyboard_height", st->keyboard_height);
  const ccord_json *kv = ccord_json_get(obj, "keyboard_visible");
  if (kv) st->keyboard_visible = ccord_json_bool(kv, st->keyboard_visible);
  const ccord_json *ls = ccord_json_get(obj, "landscape");
  if (ls) st->landscape = ccord_json_bool(ls, st->landscape);
}

static void handle_platform_state(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb || !msg->params) return;
  ccord_sandbox_platform_state st = {.viewport_scale = 1.0f};
  parse_platform_state(msg->params, &st);
  ccord_sandbox_set_platform_state(h->sb, &st);
}

// OS-level containment for out-of-process plugin hosts (Landlock + seccomp-bpf on Linux, Seatbelt on macOS).
// All fs.* APIs are brokered over IPC through the main process, so the plugin host only needs read-only
// access to its own plugin directory (for Lua require()) and zero network/write/exec capabilities.
static void apply_os_sandbox(const char *plugin_dir)
{
#if defined(CCORD_EMBEDDED_PLUGIN_HOST)
  (void)plugin_dir;
#elif defined(__linux__) && !defined(__ANDROID__)
  (void)prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

  // 1. Landlock filesystem sandbox (ABI v1+): read-only access limited strictly to plugin_dir.
#ifdef SYS_landlock_create_ruleset
  {
    struct landlock_ruleset_attr attr = {0};
    attr.handled_access_fs =
      LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
      LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
      LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
      LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
      LANDLOCK_ACCESS_FS_MAKE_SYM;
    long rfd = syscall(SYS_landlock_create_ruleset, &attr, sizeof attr, (uint32_t)0);
    if (rfd >= 0) {
      if (plugin_dir && plugin_dir[0]) {
        int dfd = open(plugin_dir, O_PATH | O_CLOEXEC);
        if (dfd >= 0) {
          struct landlock_path_beneath_attr rule = {
            .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR,
            .parent_fd = dfd,
          };
          (void)syscall(SYS_landlock_add_rule, (int)rfd, LANDLOCK_RULE_PATH_BENEATH, &rule, (uint32_t)0);
          close(dfd);
        }
      }
#ifdef CCORD_SANITIZED
      // ASan/LSan reads /proc/self at process exit for leak reporting.
      {
        int pfd = open("/proc/self", O_PATH | O_CLOEXEC);
        if (pfd >= 0) {
          struct landlock_path_beneath_attr prule = {
            .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR,
            .parent_fd = pfd,
          };
          (void)syscall(SYS_landlock_add_rule, (int)rfd, LANDLOCK_RULE_PATH_BENEATH, &prule, (uint32_t)0);
          close(pfd);
        }
      }
#endif
      (void)syscall(SYS_landlock_restrict_self, (int)rfd, (uint32_t)0);
      close((int)rfd);
    }
  }
#endif

  // 2. Seccomp-BPF denylist: block network sockets, process exec/spawn, ptrace, and fs mutation syscalls.
#if defined(__x86_64__)
#define CCORD_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define CCORD_AUDIT_ARCH AUDIT_ARCH_AARCH64
#elif defined(__i386__)
#define CCORD_AUDIT_ARCH AUDIT_ARCH_I386
#elif defined(__arm__)
#define CCORD_AUDIT_ARCH AUDIT_ARCH_ARM
#elif defined(__riscv) && (__riscv_xlen == 64)
#define CCORD_AUDIT_ARCH AUDIT_ARCH_RISCV64
#endif
#ifdef CCORD_AUDIT_ARCH
  {
    static const uint32_t k_deny_nr[] = {
#ifdef __NR_socket
      __NR_socket,
#endif
#ifdef __NR_connect
      __NR_connect,
#endif
#ifdef __NR_bind
      __NR_bind,
#endif
#ifdef __NR_listen
      __NR_listen,
#endif
#ifdef __NR_accept
      __NR_accept,
#endif
#ifdef __NR_accept4
      __NR_accept4,
#endif
#ifdef __NR_execve
      __NR_execve,
#endif
#ifdef __NR_execveat
      __NR_execveat,
#endif
#if !defined(CCORD_SANITIZED)
#ifdef __NR_ptrace
      __NR_ptrace,
#endif
#endif
#ifdef __NR_process_vm_readv
      __NR_process_vm_readv,
#endif
#ifdef __NR_process_vm_writev
      __NR_process_vm_writev,
#endif
#ifdef __NR_fork
      __NR_fork,
#endif
#ifdef __NR_vfork
      __NR_vfork,
#endif
#ifdef __NR_mount
      __NR_mount,
#endif
#ifdef __NR_umount2
      __NR_umount2,
#endif
#ifdef __NR_chroot
      __NR_chroot,
#endif
#ifdef __NR_pivot_root
      __NR_pivot_root,
#endif
#ifdef __NR_unlink
      __NR_unlink,
#endif
#ifdef __NR_unlinkat
      __NR_unlinkat,
#endif
#ifdef __NR_rename
      __NR_rename,
#endif
#ifdef __NR_renameat
      __NR_renameat,
#endif
#ifdef __NR_renameat2
      __NR_renameat2,
#endif
#ifdef __NR_mkdir
      __NR_mkdir,
#endif
#ifdef __NR_mkdirat
      __NR_mkdirat,
#endif
#ifdef __NR_rmdir
      __NR_rmdir,
#endif
#ifdef __NR_chmod
      __NR_chmod,
#endif
#ifdef __NR_fchmod
      __NR_fchmod,
#endif
#ifdef __NR_fchmodat
      __NR_fchmodat,
#endif
#ifdef __NR_chown
      __NR_chown,
#endif
#ifdef __NR_fchown
      __NR_fchown,
#endif
#ifdef __NR_fchownat
      __NR_fchownat,
#endif
#ifdef __NR_symlink
      __NR_symlink,
#endif
#ifdef __NR_symlinkat
      __NR_symlinkat,
#endif
#ifdef __NR_link
      __NR_link,
#endif
#ifdef __NR_linkat
      __NR_linkat,
#endif
#ifdef __NR_creat
      __NR_creat,
#endif
#ifdef __NR_truncate
      __NR_truncate,
#endif
#ifdef __NR_ftruncate
      __NR_ftruncate,
#endif
    };
    const size_t ndeny = sizeof k_deny_nr / sizeof k_deny_nr[0];
    struct sock_filter insns[6 + (sizeof k_deny_nr / sizeof k_deny_nr[0])];
    size_t pc = 0;
    // Load arch and kill on mismatch.
    insns[pc++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)offsetof(struct seccomp_data, arch));
    insns[pc++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)CCORD_AUDIT_ARCH, 1, 0);
    insns[pc++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    // Load syscall number.
    insns[pc++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)offsetof(struct seccomp_data, nr));
    for (size_t i = 0; i < ndeny; i++) {
      uint8_t jt = (uint8_t)(ndeny - i);   // jump to RET_ERRNO (after RET_ALLOW)
      insns[pc++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, k_deny_nr[i], jt, 0);
    }
    insns[pc++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    insns[pc++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    struct sock_fprog prog = {.len = (unsigned short)pc, .filter = insns};
    (void)prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
  }
#endif
#elif defined(__APPLE__)
  {
    char real_dir[PATH_MAX];
    const char *dir = nullptr;
    if (plugin_dir && plugin_dir[0]) {
      dir = realpath(plugin_dir, real_dir) ? real_dir : plugin_dir;
    }
    ccord_buf prof;
    ccord_buf_init(&prof, 8192);
    (void)ccord_buf_append_str(
      &prof,
      "(version 1)\n"
      "(deny default)\n"
      "(allow process-info-pidinfo (target self))\n"
      "(allow sysctl-read)\n"
      "(allow file-read* (subpath \"/usr/lib\") (subpath \"/System/Library\") (subpath \"/dev/urandom\"))\n");
    if (dir && dir[0] == '/') {
      bool safe = true;
      for (const unsigned char *p = (const unsigned char *)dir; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) {
          safe = false;
          break;
        }
      }
      if (safe) {
        (void)ccord_buf_append_str(&prof, "(allow file-read* (subpath \"");
        for (const char *p = dir; *p; p++) {
          if (*p == '\\' || *p == '"') (void)ccord_buf_append(&prof, "\\", 1);
          (void)ccord_buf_append(&prof, p, 1);
        }
        (void)ccord_buf_append_str(&prof, "\"))\n");
      }
    }
    if (!prof.failed) {
      char *errbuf = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      (void)sandbox_init(ccord_buf_cstr(&prof), 0u, &errbuf);
      if (errbuf) sandbox_free_error(errbuf);
#pragma clang diagnostic pop
    }
    ccord_buf_free(&prof);
  }
#else
  (void)plugin_dir;
#endif
}

static void handle_init(host *h, const ccord_ipc_msg *msg)
{
  const ccord_json *p = msg->params;
  if (h->sb) { reply_error(h, msg->id, "invalid_argument", "init sent twice"); return; }
  size_t il = 0, nl = 0, vl = 0, sl = 0;
  const ccord_json *pl = p ? ccord_json_get(p, "plugin") : nullptr;
  const char *id = str_of(pl, "id", &il);
  const char *name = str_of(pl, "name", &nl);
  const char *version = str_of(pl, "version", &vl);
  const char *src = str_of(p, "source", &sl);
  if (!id || !name || !version || !src) { reply_error(h, msg->id, "invalid_argument", "init needs plugin id, name, version and source"); return; }

  ccord_perm_set perms = 0;
  const ccord_json *pa = p ? ccord_json_get(p, "permissions") : nullptr;
  if (!pa || ccord_json_type_of(pa) != CCORD_JSON_ARRAY) { reply_error(h, msg->id, "invalid_argument", "init needs a permissions array"); return; }
  for (size_t i = 0; i < ccord_json_array_len(pa); i++) {
    size_t n = 0;
    const char *pn = ccord_json_string(ccord_json_array_at(pa, i), &n);
    ccord_perm pm = CCORD_PERM_EVENTS_MESSAGES;
    if (!pn || !ccord_perm_from_name(pn, &pm)) { reply_error(h, msg->id, "invalid_argument", "unknown permission in init"); return; }
    perms |= (ccord_perm_set)pm;
  }

  size_t dl = 0;
  const char *dir = str_of(pl, "dir", &dl);
  (void)dl;
  if (!h->cancel) apply_os_sandbox(dir);
  ccord_sandbox_config cfg = {
    .plugin_id = id, .plugin_name = name, .plugin_version = version, .plugin_dir = dir,
    .perms = perms, .memory_bytes = h->cancel ? (32u << 20) : (64u << 20), .instructions = 0,
  };
  const ccord_json *plat = p ? ccord_json_get(p, "platform") : nullptr;
  if (plat && ccord_json_type_of(plat) == CCORD_JSON_OBJECT) {
    cfg.has_platform = true;
    size_t tl = 0;
    const char *os = str_of(plat, "os", &tl);
    const char *arch = str_of(plat, "arch", &tl);
    const char *ff = str_of(plat, "form_factor", &tl);
    if (os) snprintf(cfg.platform.os, sizeof cfg.platform.os, "%s", os);
    if (arch) snprintf(cfg.platform.arch, sizeof cfg.platform.arch, "%s", arch);
    if (ff) snprintf(cfg.platform.form_factor, sizeof cfg.platform.form_factor, "%s", ff);
    cfg.platform.is_mobile = ccord_json_bool(ccord_json_get(plat, "is_mobile"), false);
    cfg.platform.touch = ccord_json_bool(ccord_json_get(plat, "touch"), cfg.platform.is_mobile);
    int64_t api_lvl = 0;
    if (ccord_json_get(plat, "android_api_level") && ccord_json_int64(ccord_json_get(plat, "android_api_level"), &api_lvl)) {
      cfg.platform.android_api_level = (int)api_lvl;
    }
    int64_t caps = 0;
    if (ccord_json_get(plat, "capabilities_mask") && ccord_json_int64(ccord_json_get(plat, "capabilities_mask"), &caps)) {
      cfg.platform.capabilities = (uint64_t)caps;
    }
    cfg.platform_state.viewport_scale = 1.0f;
    parse_platform_state(ccord_json_get(plat, "state"), &cfg.platform_state);
  }
  ccord_sandbox_services svc = {.ud = h, .request = svc_request, .request_sync = svc_request_sync, .log = svc_log};
  ccord_error err = {0};
  h->sb = ccord_sandbox_create(&cfg, &svc, &err);
  if (!h->sb) { reply_error(h, msg->id, "invalid_argument", err.message); return; }
  if (h->cancel) ccord_sandbox_set_cancel_flag(h->sb, h->cancel);

  const ccord_json *storage = ccord_json_get(p, "storage");
  if (storage && ccord_sandbox_seed_storage(h->sb, storage, &err) != CCORD_OK) {
    { reply_error(h, msg->id, "invalid_argument", err.message); return; }
  }
  ccord_status st = ccord_sandbox_load(h->sb, src, sl, &err);
  if (limit_stop(h)) return;
  if (st != CCORD_OK) { reply_error(h, msg->id, "invalid_argument", err.message); return; }
  reply_ok(h, msg->id, nullptr);
}

static void handle_event(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb) return;
  size_t n = 0;
  const char *name = str_of(msg->params, "name", &n);
  const ccord_json *data = msg->params ? ccord_json_get(msg->params, "data") : nullptr;
  if (!name) return;
  ccord_error err = {0};
  (void)ccord_sandbox_emit(h->sb, name, data, &err);
  (void)limit_stop(h);
}

static void handle_menu(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb) return;
  size_t n = 0;
  const char *item = str_of(msg->params, "item_id", &n);
  const ccord_json *ctx = msg->params ? ccord_json_get(msg->params, "ctx") : nullptr;
  if (!item) return;
  ccord_error err = {0};
  if (ccord_sandbox_menu_invoke(h->sb, item, ctx, &err) != CCORD_OK && !limit_stop(h)) {
    svc_log(h, CCORD_LOG_WARN, err.message);
  }
  (void)limit_stop(h);
}

static void handle_panel_event(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb) return;
  size_t n = 0;
  const char *panel_id = str_of(msg->params, "panel_id", &n);
  const ccord_json *ev = msg->params ? ccord_json_get(msg->params, "event") : nullptr;
  if (!panel_id) return;
  ccord_error err = {0};
  if (ccord_sandbox_panel_event(h->sb, panel_id, ev, &err) != CCORD_OK && !limit_stop(h)) {
    svc_log(h, CCORD_LOG_WARN, err.message);
  }
  (void)limit_stop(h);
}

static void handle_command(host *h, const ccord_ipc_msg *msg)
{
  if (!h->sb) { reply_error(h, msg->id, "internal", "plugin not initialised"); return; }
  size_t nl = 0, al = 0;
  const char *name = str_of(msg->params, "name", &nl);
  const char *args = str_of(msg->params, "args", &al);
  const ccord_json *ctx = msg->params ? ccord_json_get(msg->params, "ctx") : nullptr;
  if (!name) { reply_error(h, msg->id, "invalid_argument", "command.run needs a name"); return; }
  char *text = nullptr;
  ccord_error err = {0};
  ccord_status st = ccord_sandbox_command_run(h->sb, name, args ? args : "", ctx, &text, &err);
  if (limit_stop(h)) {
    ccord_free(text);
    return;
  }
  if (st != CCORD_OK) {
    { reply_error(h, msg->id, st == CCORD_ERR_NOT_FOUND ? "not_found" : "internal", err.message); return; }
  }
  if (!text) { reply_ok(h, msg->id, nullptr); return; }
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "text");
  ccord_jw_string(&w, text, strlen(text));
  ccord_jw_end_object(&w);
  ccord_free(text);
  if (ccord_jw_ok(&w)) reply_ok(h, msg->id, ccord_buf_cstr(&b));
  else reply_error(h, msg->id, "invalid_argument", "command result is not valid UTF-8");
  ccord_buf_free(&b);
}

static void dispatch(host *h, const ccord_ipc_msg *msg)
{
  switch (msg->kind) {
    case CCORD_IPC_REQ:
      if (strcmp(msg->method, "init") == 0) handle_init(h, msg);
      else if (strcmp(msg->method, "command.run") == 0) handle_command(h, msg);
      else if (strcmp(msg->method, "ping") == 0) reply_ok(h, msg->id, nullptr);
      else reply_error(h, msg->id, "unsupported", "unknown method");
      return;
    case CCORD_IPC_NTF:
      if (strcmp(msg->method, "event") == 0) handle_event(h, msg);
      else if (strcmp(msg->method, "menu.invoke") == 0) handle_menu(h, msg);
      else if (strcmp(msg->method, "panel.event") == 0) handle_panel_event(h, msg);
      else if (strcmp(msg->method, "platform.state") == 0) handle_platform_state(h, msg);
      else if (strcmp(msg->method, "shutdown") == 0) h->stop = true;
      return;
    case CCORD_IPC_RES:
      if (!msg->ok) svc_log(h, CCORD_LOG_WARN, msg->err_message ? msg->err_message : "main rejected a request");
      return;
  }
}

static void drain_deferred(host *h, ccord_arena *arena)
{
  static const ccord_json_limits k_lim = {16, 8192, (size_t)1u << 20};
  while (h->deferred_count > 0 && !h->stop) {
    deferred_msg d = h->deferred[h->deferred_head];
    h->deferred[h->deferred_head] = (deferred_msg){0};
    h->deferred_head = (h->deferred_head + 1u) % MAX_DEFERRED;
    h->deferred_count--;
    ccord_arena_reset(arena);
    ccord_ipc_msg msg = {.kind = d.kind, .id = d.id, .method = d.method};
    if (d.params_json) {
      ccord_error perr = {0};
      msg.params = ccord_json_parse(arena, d.params_json, d.params_len, &k_lim, &perr);
    }
    dispatch(h, &msg);
    ccord_free(d.params_json);
    ccord_arena_reset(arena);
  }
}

static int run_loop(host *h)
{
  ccord_arena arena;
  ccord_arena_init(&arena, 4096u);
  while (!h->stop) {
    drain_deferred(h, &arena);
    if (h->stop) break;
    if (h->cancel && atomic_load_explicit(h->cancel, memory_order_relaxed)) {
      h->stop = true;
      break;
    }
    unsigned events = CCORD_POLL_IN;
    if (ccord_ipc_has_pending_output(h->ipc)) events |= CCORD_POLL_OUT;
    int poll_ms = h->sb ? ccord_sandbox_next_timer_ms(h->sb) : -1;
    if (h->cancel && (poll_ms < 0 || poll_ms > 1000)) poll_ms = 1000;
    if (h->deferred_count > 0) poll_ms = 0;
    if (ccord_sock_poll1(ccord_ipc_fd(h->ipc), events, poll_ms) < 0) fatal(h, EXIT_INTERNAL);
    for (;;) {
      ccord_ipc_msg msg;
      ccord_error err = {0};
      ccord_ipc_read r = ccord_ipc_try_read(h->ipc, &arena, &msg, &err);
      if (r == CCORD_IPC_READ_EMPTY) break;
      if (r == CCORD_IPC_READ_EOF) {   // main closed the socket: nothing left to serve
        h->stop = true;
        break;
      }
      if (r == CCORD_IPC_READ_ERROR) {
        fatal(h, EXIT_PROTOCOL);
        break;
      }
      dispatch(h, &msg);
      ccord_arena_reset(&arena);
      drain_deferred(h, &arena);
      if (h->stop) break;
    }
    if (!h->stop) {
      if (h->sb && ccord_sandbox_next_timer_ms(h->sb) == 0) {
        ccord_error terr = {0};
        (void)ccord_sandbox_tick(h->sb, ccord_clock_ms(), &terr);
        (void)limit_stop(h);
      }
      ccord_error err = {0};
      if (ccord_ipc_flush(h->ipc, h->stop ? 100 : 0, &err) != CCORD_OK && !h->stop) fatal(h, EXIT_PROTOCOL);
    }
  }
  host_free_deferred(h);
  if (h->ipc) {
    ccord_error ferr = {0};
    (void)ccord_ipc_flush(h->ipc, 100, &ferr);
  }
  ccord_arena_free(&arena);
  return h->exit_code;
}

// Process-level hardening. Empty environment and null stdin come from the spawning side.
static void harden(void)
{
#if defined(__linux__)
  (void)prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
  (void)prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
#endif
#if !CCORD_OS_WINDOWS
  struct rlimit core = {0, 0};
  (void)setrlimit(RLIMIT_CORE, &core);
#endif
}

static void send_hello(host *h)
{
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "proto");
  ccord_jw_int(&w, CCORD_IPC_PROTO_VERSION);
  ccord_jw_key(&w, "lua");
  ccord_jw_cstr(&w, LUA_RELEASE);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) send_ntf(h, "hello", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
}

#ifdef CCORD_EMBEDDED_PLUGIN_HOST
int ccord_plugin_host_run_fd(ccord_socket_t fd, const _Atomic bool *cancel);
int ccord_plugin_host_run_fd(ccord_socket_t fd, const _Atomic bool *cancel)
{
  host h = {.ipc = nullptr, .sb = nullptr, .next_id = 1, .stop = false, .exit_code = 0, .cancel = cancel};
  ccord_error err = {0};
  h.ipc = ccord_ipc_open(fd, &err);
  if (!h.ipc) return EXIT_INTERNAL;
  send_hello(&h);
  int code = run_loop(&h);
  ccord_sandbox_destroy(h.sb);
  ccord_ipc_close(h.ipc);
  return code;
}

int ccord_plugin_host_main(int argc, char **argv);
int ccord_plugin_host_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
  if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
    printf("ccord-plugin-host %s\n", CCORD_VERSION_STRING);
    return 0;
  }
  if (argc != 3 || strcmp(argv[1], "--ipc-fd") != 0) {
    fprintf(stderr, "usage: ccord-plugin-host --ipc-fd N\n");
    return EXIT_USAGE;
  }
  char *end = nullptr;
  unsigned long long raw_fd = strtoull(argv[2], &end, 10);
  if (!end || *end != '\0' || argv[2][0] == '-' ||
#if !CCORD_OS_WINDOWS
      raw_fd > (unsigned long long)INT_MAX
#else
      raw_fd == (unsigned long long)CCORD_INVALID_SOCKET
#endif
  ) {
    fprintf(stderr, "ccord-plugin-host: bad --ipc-fd\n");
    return EXIT_USAGE;
  }
  ccord_net_os_init();
  harden();
  host h = {.ipc = nullptr, .sb = nullptr, .next_id = 1, .stop = false, .exit_code = 0};
  ccord_error err = {0};
  h.ipc = ccord_ipc_open((ccord_socket_t)raw_fd, &err);   // takes ownership of socket
  if (!h.ipc) {
    fprintf(stderr, "ccord-plugin-host: %s\n", err.message);
    return EXIT_INTERNAL;
  }
  send_hello(&h);
  int code = run_loop(&h);
  ccord_sandbox_destroy(h.sb);
  ccord_ipc_close(h.ipc);
  return code;
}
