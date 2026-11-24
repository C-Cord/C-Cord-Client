// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "plugins_internal.h"
#include "package/zip.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/base64.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/config.h>
#include <ccord/core/input.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/paths.h>
#include <ccord/core/utf8.h>
#include <ccord/ipc/ipc.h>
#include <ccord/store/notify.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if !CCORD_OS_WINDOWS
#include <fcntl.h>
#include <signal.h>
#if !CCORD_OS_ANDROID
#include <spawn.h>
#include <sys/wait.h>
#endif
#include <unistd.h>
#endif


static void format_exit_code(int code, char *out, size_t cap)
{
  switch (code) {
    case 0: pl_set_text(out, cap, "host exited"); break;
    case 64: pl_set_text(out, cap, "host usage error"); break;
    case 70: pl_set_text(out, cap, "host internal error"); break;
    case 71: pl_set_text(out, cap, "host protocol violation"); break;
    case 72: pl_set_text(out, cap, "host out of memory at startup"); break;
    case 73: pl_set_text(out, cap, "instruction limit exceeded"); break;
    case 74: pl_set_text(out, cap, "memory limit exceeded"); break;
    case 75: pl_set_text(out, cap, "undeclared restricted API called"); break;
    default: snprintf(out, cap, "host exited with code %d", code); break;
  }
}

bool pl_host_is_running(const plugin *p)
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

void pl_kill_host_process(plugin *p)
{
  if (!pl_host_is_running(p)) return;
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
bool pl_reap_wait(plugin *p, uint32_t wait_ms, char *out, size_t cap)
{
  if (!pl_host_is_running(p)) return true;
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
  else pl_set_text(out, cap, "host ended");
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
  pl_set_text(out, cap, "host ended");
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
        pl_set_text(out, cap, "host ended");
      }
      return true;
    }
    if (r < 0 && errno != EINTR) {
      p->reaped = true;
      pl_set_text(out, cap, "host process lost");
      return true;
    }
    if (waited >= wait_ms) return false;
    ccord_sleep_ms(5);
    waited += 5;
  }
#endif
}

// Moves a plugin to FAILED: kills the host, drops its contributions, announces the state.
// ---------------------------------------------------------------- pump and timers

static void read_host(ccord_plugins *m, plugin *p)
{
  // Lazily initialized: no chunk is allocated and no arena free runs on EAGAIN.
  ccord_arena arena = {.chunk_size = 4096u};
  for (int k = 0; k < 64 && p->ipc; k++) {
    ccord_ipc_msg msg;
    ccord_error err = {0};
    ccord_ipc_read r = ccord_ipc_try_read(p->ipc, &arena, &msg, &err);
    if (r == CCORD_IPC_READ_MSG) {
      pl_handle_frame(m, p, &msg);
      ccord_arena_reset(&arena);
      continue;
    }
    if (r == CCORD_IPC_READ_EMPTY) break;
    char reason[320];
    char exit_text[160] = "";
    if (r == CCORD_IPC_READ_EOF) {
      if (pl_reap_wait(p, EXIT_WAIT_MS, exit_text, sizeof exit_text) && exit_text[0]) {
        snprintf(reason, sizeof reason, "%s", exit_text);
      } else {
        pl_set_text(reason, sizeof reason, "host closed the connection");
      }
    } else {
      snprintf(reason, sizeof reason, "protocol violation: %s", err.message);
    }
    pl_fail_plugin(m, p, reason);
    break;
  }
  if (arena.head) ccord_arena_free(&arena);
}

static void check_timers(ccord_plugins *m, plugin *p)
{
  if (!p->ipc) return;
  uint64_t now = m->now_ms;
  for (size_t i = 0; i < MAX_PENDING && p->ipc; i++) {
    pending_req *s = &p->pend[i];
    if (s->kind == PEND_NONE || now < s->deadline_ms) continue;
    if (s->kind == PEND_INIT) { pl_fail_plugin(m, p, "init timed out after 5 s"); return; }
    if (s->kind == PEND_PING) { pl_fail_plugin(m, p, "ping timeout: host did not answer in 5 s"); return; }
    CCORD_LOG_W("plugin %s: command.run timed out after 2 s", p->m.id);
    s->kind = PEND_NONE;
  }
  if (!p->ipc || p->state != CCORD_PLUGIN_READY) return;
  if (!m->in_background && !pl_pend_slot(p, PEND_PING) && now - p->last_ping_sent_ms >= PING_INTERVAL_MS) {
    pending_req *slot = pl_pend_free(p);
    if (!slot) return;
    uint32_t id = pl_next_request_id(p);
    ccord_error err = {0};
    if (ccord_ipc_send_request(p->ipc, id, "ping", "{}", &err) != CCORD_OK) {
      char why[256];
      snprintf(why, sizeof why, "could not send ping: %s", err.message);
      { pl_fail_plugin(m, p, why); return; }
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
    plugin *p = m->plugins[i];
    if (!p->ipc) continue;
    read_host(m, p);
    if (p->ipc) {
      ccord_error err = {0};
      if (ccord_ipc_flush(p->ipc, 0, &err) != CCORD_OK) pl_fail_plugin(m, p, "host connection lost while writing");
    }
    if (p->ipc) check_timers(m, p);
    if (p->ipc == nullptr && pl_host_is_running(p)) {
      char reason[160];
      (void)pl_reap_wait(p, 0, reason, sizeof reason);
    }
    if (p->storage_enabled && p->dirty && now_ms - p->last_save_ms >= SAVE_INTERVAL_MS) pl_storage_save(m, p, now_ms);
  }
  if (m->workers) pw_pump(m->workers, plugins_ext_on_worker_event, m);
  plugins_ext_poll_account_update(m);
  if (m->dev_auto_reload) plugins_ext_dev_poll(m, now_ms);
  if (m->catalog) ccord_catalog_pump(m->catalog, now_ms);
}

void ccord_plugins_set_background(ccord_plugins *m, bool background)
{
  if (!m || m->in_background == background) return;
  m->in_background = background;
  uint64_t now = !background ? (m->now_ms ? m->now_ms : ccord_clock_ms()) : 0;
  const char *params = background ? "{\"background\":true}" : "{\"background\":false}";
  for (size_t i = 0; i < m->nplugins; i++) {
    plugin *p = m->plugins[i];
    if (!p) continue;
    if (!background) p->last_ping_sent_ms = now;
    if (p->ipc && p->state == CCORD_PLUGIN_READY) {
      pl_send_ntf(m, p, "host.background", params);
    }
  }
}

size_t ccord_plugins_wait_info(const ccord_plugins *m, ccord_socket_t *out_fds, size_t cap, uint32_t *out_timeout_ms)
{
  if (out_timeout_ms) *out_timeout_ms = UINT32_MAX;
  if (!m) return 0;
  uint64_t now = m->now_ms ? m->now_ms : ccord_clock_ms();
  uint32_t timeout = UINT32_MAX;
  size_t n = 0;
  for (size_t i = 0; i < m->nplugins; i++) {
    const plugin *p = m->plugins[i];
    if (!p || !p->ipc) continue;
    ccord_socket_t fd = ccord_ipc_fd(p->ipc);
    if (fd != CCORD_INVALID_SOCKET && out_fds && n < cap) out_fds[n++] = fd;
    if (ccord_ipc_has_pending_output(p->ipc)) timeout = 0;
    for (size_t k = 0; k < MAX_PENDING; k++) {
      const pending_req *s = &p->pend[k];
      if (s->kind == PEND_NONE) continue;
      uint64_t rem = s->deadline_ms > now ? (s->deadline_ms - now) : 0u;
      if (rem < (uint64_t)timeout) timeout = (uint32_t)rem;
    }
    if (p->storage_enabled && p->dirty) {
      uint64_t due = p->last_save_ms + SAVE_INTERVAL_MS;
      uint64_t rem = due > now ? (due - now) : 0u;
      if (rem < (uint64_t)timeout) timeout = (uint32_t)rem;
    }
    if (!m->in_background && p->state == CCORD_PLUGIN_READY) {
      bool has_ping = false;
      for (size_t k = 0; k < MAX_PENDING; k++) {
        if (p->pend[k].kind == PEND_PING) {
          has_ping = true;
          break;
        }
      }
      if (!has_ping) {
        uint64_t due = p->last_ping_sent_ms + PING_INTERVAL_MS;
        uint64_t rem = due > now ? (due - now) : 0u;
        if (rem < (uint64_t)timeout) timeout = (uint32_t)rem;
      }
    }
  }
  if (out_timeout_ms) *out_timeout_ms = timeout;
  return n;
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

ccord_status pl_spawn_host(ccord_plugins *m, plugin *p, ccord_error *err)
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
      pl_kill_host_process(p);
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
  STARTUPINFOEXW siex = {0};
  siex.StartupInfo.cb = sizeof siex;
  HANDLE inherit_list[1] = {(HANDLE)sv[1]};
  SIZE_T attr_sz = 0;
  (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_sz);
  LPPROC_THREAD_ATTRIBUTE_LIST attr_list = attr_sz ? (LPPROC_THREAD_ATTRIBUTE_LIST)ccord_malloc(attr_sz) : nullptr;
  bool attr_ok = attr_list &&
                 InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_sz) &&
                 UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                           inherit_list, sizeof inherit_list, nullptr, nullptr);
  if (!attr_ok) {
    if (attr_list) { DeleteProcThreadAttributeList(attr_list); ccord_free(attr_list); }
    ccord_sock_close(sv[0]);
    ccord_sock_close(sv[1]);
    ccord_error_set(err, CCORD_ERR_IO, "PROC_THREAD_ATTRIBUTE_HANDLE_LIST setup failed");
    return CCORD_ERR_IO;
  }
  siex.lpAttributeList = attr_list;
  PROCESS_INFORMATION pi = {0};
  wchar_t empty_env[2] = {0, 0};
  BOOL ok = CreateProcessW(nullptr, wcmd, nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                           empty_env, nullptr, &siex.StartupInfo, &pi);
  DeleteProcThreadAttributeList(attr_list);
  ccord_free(attr_list);
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
    pl_kill_host_process(p);
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
    pl_kill_host_process(p);
    return err->code ? err->code : CCORD_ERR_INTERNAL;
  }
  return CCORD_OK;
#endif
}

// ---------------------------------------------------------------- lifecycle

void pl_stop_host(ccord_plugins *m, plugin *p)
{
  if (p->state == CCORD_PLUGIN_STARTING || p->state == CCORD_PLUGIN_READY) {
    if (p->ipc) {
      ccord_error err = {0};
      (void)ccord_ipc_send_notify(p->ipc, "shutdown", "{}", &err);
      (void)ccord_ipc_flush(p->ipc, 100, &err);
    }
    pl_storage_save(m, p, m->now_ms);
    char reason[160];
    if (pl_host_is_running(p) && !pl_reap_wait(p, 1000, reason, sizeof reason)) {
      pl_kill_host_process(p);
    }
    if (p->ipc) {
      ccord_ipc_close(p->ipc);
      p->ipc = nullptr;
    }
    p->state = CCORD_PLUGIN_STOPPED;
    memset(p->pend, 0, sizeof p->pend);
    if (m->workers) pw_remove_owner(m->workers, p->m.id);
    plugins_ext_reset_plugin(p);
    ccord_contrib_remove_owner(m->contrib, p->m.id);
    pl_publish_state(m, p);
  } else if (p->ipc) {
    ccord_ipc_close(p->ipc);
    p->ipc = nullptr;
  }
}

void pl_start_plugin_host(ccord_plugins *m, plugin *p)
{
  if (!p || p->state == CCORD_PLUGIN_STARTING || p->state == CCORD_PLUGIN_READY) return;
  char rev_reason[256] = "";
  if (p->revoked || (m->catalog && ccord_catalog_is_revoked(m->catalog, p->m.id, rev_reason, sizeof rev_reason))) {
    p->revoked = true;
    p->enabled = false;
    p->consent_granted = false;
    p->consented_perms = 0;
    pl_set_plugin_failed(m, p, rev_reason[0] ? rev_reason : "Revoked by C-Cord moderators", true);
    return;
  }
  if (!p->source) {
    char why[256] = "";
    if (!pl_reload_plugin_source(p, why, sizeof why)) {
      p->state = CCORD_PLUGIN_FAILED;
      pl_set_text(p->error, sizeof p->error, why);
      CCORD_LOG_E("plugin %s FAILED: %s", p->m.id, p->error);
      pl_publish_state(m, p);
      ccord_contrib_add_plugin_log(m->contrib, p->m.id, m->now_ms, (uint8_t)CCORD_LOG_ERROR, p->error);
      return;
    }
  }
  p->error[0] = '\0';
  p->hello_seen = false;
  p->subs = 0;
  p->menu_count = 0;
  memset(p->pend, 0, sizeof p->pend);
  p->state = CCORD_PLUGIN_STARTING;
  pl_publish_state(m, p);
  ccord_error err = {0};
  if (pl_spawn_host(m, p, &err) != CCORD_OK) {
    char reason[256];
    snprintf(reason, sizeof reason, "could not start host: %s", err.message);
    pl_fail_plugin(m, p, reason);
  }
}

