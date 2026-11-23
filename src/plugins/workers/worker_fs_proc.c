// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "workers_internal.h"

// ---------------------------------------------------------------- Filesystem watcher (fs.watch)

static void stat_snapshot(const char *path, bool *existed, int64_t *mtime_ns, int64_t *size)
{
  struct stat st;
  if (stat(path, &st) == 0) {
    *existed = true;
    *size = (int64_t)st.st_size;
#if defined(__linux__)
    *mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
#else
    *mtime_ns = (int64_t)st.st_mtime * 1000000000LL;
#endif
  } else {
    *existed = false;
    *size = 0;
    *mtime_ns = 0;
  }
}

bool pw_fs_watch_add(plugin_workers *w, const char *owner, uint64_t watch_id, const char *path)
{
  if (!w || !owner || !path || !path[0]) return false;
  for (size_t i = 0; i < PW_MAX_WATCHES; i++) {
    if (!w->watches[i] || !w->watches[i]->active) {
      pw_watch *slot = w->watches[i];
      if (!slot) {
        slot = ccord_calloc(1, sizeof *slot);
        if (!slot) return false;
        w->watches[i] = slot;
      } else {
        memset(slot, 0, sizeof *slot);
      }
      slot->active = true;
      slot->watch_id = watch_id;
      snprintf(slot->owner, sizeof slot->owner, "%s", owner);
      snprintf(slot->path, sizeof slot->path, "%s", path);
      stat_snapshot(path, &slot->existed, &slot->last_mtime_ns, &slot->last_size);
#if defined(__linux__) && !CCORD_OS_WINDOWS
      if (w->inotify_fd < 0) w->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
      slot->wd = w->inotify_fd >= 0
                   ? inotify_add_watch(w->inotify_fd, path,
                                       IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF)
                   : -1;
#endif
      return true;
    }
  }
  return false;
}

void pw_fs_watch_remove(plugin_workers *w, const char *owner, uint64_t watch_id)
{
  if (!w || !owner) return;
  for (size_t i = 0; i < PW_MAX_WATCHES; i++) {
    pw_watch *wt = w->watches[i];
    if (wt && wt->active && wt->watch_id == watch_id && strcmp(wt->owner, owner) == 0) {
#if defined(__linux__) && !CCORD_OS_WINDOWS
      pw_inotify_rm_watch(w->inotify_fd, wt->wd);
#endif
      ccord_free(wt);
      w->watches[i] = nullptr;
      return;
    }
  }
}

void pw_poll_watches(plugin_workers *w)
{
  bool any_watch = false;
  for (size_t i = 0; i < PW_MAX_WATCHES; i++) {
    if (w->watches[i] && w->watches[i]->active) {
      any_watch = true;
      break;
    }
  }
  if (!any_watch) return;
#if defined(__linux__) && !CCORD_OS_WINDOWS
  if (w->inotify_fd >= 0) {
    alignas(struct inotify_event) char buf[4096];
    for (;;) {
      ssize_t n = read(w->inotify_fd, buf, sizeof buf);
      if (n <= 0) break;
      ssize_t off = 0;
      while (off + (ssize_t)sizeof(struct inotify_event) <= n) {
        struct inotify_event ie;
        memcpy(&ie, buf + off, sizeof ie);
        const char *ename = (ie.len > 0 && off + (ssize_t)(sizeof ie + ie.len) <= n) ? (buf + off + sizeof ie) : nullptr;
        for (size_t i = 0; i < PW_MAX_WATCHES; i++) {
          pw_watch *wt = w->watches[i];
          if (!wt || !wt->active || wt->wd != ie.wd) continue;
          const char *kind = (ie.mask & (IN_CREATE | IN_MOVED_TO)) ? "create"
                           : (ie.mask & (IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM)) ? "delete"
                           : "modify";
          ccord_buf jb; ccord_buf_init(&jb, 0); ccord_jw jw; ccord_jw_init(&jw, &jb);
          ccord_jw_begin_object(&jw);
          ccord_jw_key(&jw, "kind"); ccord_jw_cstr(&jw, kind);
          ccord_jw_key(&jw, "path"); ccord_jw_cstr(&jw, wt->path);
          if (ename && ename[0]) { ccord_jw_key(&jw, "name"); ccord_jw_cstr(&jw, ename); }
          ccord_jw_end_object(&jw);
          if (ccord_jw_ok(&jw)) {
            pw_event ev = {.kind = PW_EV_FS_WATCH, .id = wt->watch_id, .ok = true,
                           .json = ccord_strndup(ccord_buf_cstr(&jb), jb.len)};
            snprintf(ev.owner, sizeof ev.owner, "%s", wt->owner);
            pw_push_ev(w, &ev);
          }
          ccord_buf_free(&jb);
          stat_snapshot(wt->path, &wt->existed, &wt->last_mtime_ns, &wt->last_size);
        }
        off += (ssize_t)(sizeof(struct inotify_event) + ie.len);
      }
    }
  }
#endif
  uint64_t now = ccord_clock_ms();
  if (now - w->last_watch_poll_ms < 250u) return;
  w->last_watch_poll_ms = now;
  for (size_t i = 0; i < PW_MAX_WATCHES; i++) {
    pw_watch *wt = w->watches[i];
    if (!wt || !wt->active) continue;
#if defined(__linux__) && !CCORD_OS_WINDOWS
    if (wt->wd >= 0) continue;   // handled by inotify
#endif
    bool ex = false; int64_t mt = 0, sz = 0;
    stat_snapshot(wt->path, &ex, &mt, &sz);
    if (ex != wt->existed || mt != wt->last_mtime_ns || sz != wt->last_size) {
      const char *kind = (!wt->existed && ex) ? "create" : (wt->existed && !ex) ? "delete" : "modify";
      wt->existed = ex; wt->last_mtime_ns = mt; wt->last_size = sz;
      ccord_buf jb; ccord_buf_init(&jb, 0); ccord_jw jw; ccord_jw_init(&jw, &jb);
      ccord_jw_begin_object(&jw);
      ccord_jw_key(&jw, "kind"); ccord_jw_cstr(&jw, kind);
      ccord_jw_key(&jw, "path"); ccord_jw_cstr(&jw, wt->path);
      ccord_jw_end_object(&jw);
      if (ccord_jw_ok(&jw)) {
        pw_event ev = {.kind = PW_EV_FS_WATCH, .id = wt->watch_id, .ok = true,
                       .json = ccord_strndup(ccord_buf_cstr(&jb), jb.len)};
        snprintf(ev.owner, sizeof ev.owner, "%s", wt->owner);
        pw_push_ev(w, &ev);
      }
      ccord_buf_free(&jb);
    }
  }
}

// ---------------------------------------------------------------- system.exec & system.spawn

typedef struct exec_job {
  plugin_workers *w;
  char owner[65];
  uint64_t req_id;
  char **argv;
  size_t argc;
  char cwd[CCORD_PATH_MAX];
  uint8_t *stdin_data;
  size_t stdin_len;
  uint32_t timeout_ms;
} exec_job;

static void *exec_worker_fn(void *arg)
{
  exec_job *j = arg;
  plugin_workers *w = j->w;
  bool ok = false;
  int exit_code = -1;
  ccord_buf out_b, err_b;
  ccord_buf_init(&out_b, 0);
  ccord_buf_init(&err_b, 0);
  char ebuf[128] = "exec unsupported";

#if !CCORD_OS_WINDOWS
  int in_p[2] = {-1, -1}, out_p[2] = {-1, -1}, err_p[2] = {-1, -1};
  if (pipe(in_p) == 0 && pipe(out_p) == 0 && pipe(err_p) == 0) {
    pid_t pid = fork();
    if (pid == 0) {
      (void)dup2(in_p[0], STDIN_FILENO);
      (void)dup2(out_p[1], STDOUT_FILENO);
      (void)dup2(err_p[1], STDERR_FILENO);
      close(in_p[0]); close(in_p[1]);
      close(out_p[0]); close(out_p[1]);
      close(err_p[0]); close(err_p[1]);
      if (j->cwd[0] && chdir(j->cwd) != 0) _exit(127);
      execvp(j->argv[0], j->argv);
      _exit(127);
    } else if (pid > 0) {
      close(in_p[0]); close(out_p[1]); close(err_p[1]);
      if (j->stdin_data && j->stdin_len > 0) {
        size_t off = 0;
        while (off < j->stdin_len) {
          ssize_t n = write(in_p[1], j->stdin_data + off, j->stdin_len - off);
          if (n <= 0) break;
          off += (size_t)n;
        }
      }
      close(in_p[1]);
      int fl1 = fcntl(out_p[0], F_GETFL, 0); if (fl1 >= 0) (void)fcntl(out_p[0], F_SETFL, fl1 | O_NONBLOCK);
      int fl2 = fcntl(err_p[0], F_GETFL, 0); if (fl2 >= 0) (void)fcntl(err_p[0], F_SETFL, fl2 | O_NONBLOCK);
      uint64_t deadline = ccord_clock_ms() + (j->timeout_ms ? j->timeout_ms : 15000u);
      bool open_out = true, open_err = true;
      bool timed_out = false;
      while ((open_out || open_err) && !atomic_load(&w->shutting_down)) {
        if (ccord_clock_ms() >= deadline) { timed_out = true; kill(pid, SIGKILL); break; }
        struct pollfd pfs[2]; int np = 0;
        if (open_out) pfs[np++] = (struct pollfd){.fd = out_p[0], .events = POLLIN};
        if (open_err) pfs[np++] = (struct pollfd){.fd = err_p[0], .events = POLLIN};
        int pr = poll(pfs, (nfds_t)np, 50);
        if (pr <= 0) continue;
        char tmp[4096];
        for (int k = 0; k < np; k++) {
          if (!(pfs[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
          ssize_t r = read(pfs[k].fd, tmp, sizeof tmp);
          if (r > 0) {
            ccord_buf *dst = pfs[k].fd == out_p[0] ? &out_b : &err_b;
            if (dst->len + (size_t)r <= 512u * 1024u) (void)ccord_buf_append(dst, tmp, (size_t)r);
          } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
            if (pfs[k].fd == out_p[0]) open_out = false;
            else open_err = false;
          }
        }
      }
      close(out_p[0]); close(err_p[0]);
      int status = 0;
      (void)waitpid(pid, &status, 0);
      if (timed_out) {
        snprintf(ebuf, sizeof ebuf, "process timed out");
      } else {
        ok = true;
        exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? -WTERMSIG(status) : -1);
      }
    } else {
      close(in_p[0]); close(in_p[1]); close(out_p[0]); close(out_p[1]); close(err_p[0]); close(err_p[1]);
      snprintf(ebuf, sizeof ebuf, "fork failed");
    }
  } else {
    if (in_p[0] >= 0) { close(in_p[0]); close(in_p[1]); }
    if (out_p[0] >= 0) { close(out_p[0]); close(out_p[1]); }
    if (err_p[0] >= 0) { close(err_p[0]); close(err_p[1]); }
    snprintf(ebuf, sizeof ebuf, "pipe failed");
  }
#endif

  if (!atomic_load(&w->shutting_down)) {
    pw_event ev = {.kind = PW_EV_ASYNC_REPLY, .id = j->req_id, .ok = ok};
    snprintf(ev.owner, sizeof ev.owner, "%s", j->owner);
    if (ok) {
      ccord_buf jb; ccord_buf_init(&jb, 0); ccord_jw jw; ccord_jw_init(&jw, &jb);
      ccord_jw_begin_object(&jw);
      ccord_jw_key(&jw, "code"); ccord_jw_int(&jw, exit_code);
      ccord_jw_key(&jw, "stdout"); ccord_jw_string(&jw, out_b.data ? out_b.data : "", out_b.len);
      ccord_jw_key(&jw, "stderr"); ccord_jw_string(&jw, err_b.data ? err_b.data : "", err_b.len);
      ccord_jw_end_object(&jw);
      if (ccord_jw_ok(&jw)) ev.json = ccord_strndup(ccord_buf_cstr(&jb), jb.len);
      ccord_buf_free(&jb);
    } else {
      snprintf(ev.error, sizeof ev.error, "%s", ebuf);
    }
    pw_push_ev(w, &ev);
  }
  ccord_buf_free(&out_b);
  ccord_buf_free(&err_b);
  for (size_t i = 0; i < j->argc; i++) ccord_free(j->argv[i]);
  ccord_free(j->argv);
  ccord_free(j->stdin_data);
  ccord_free(j);
  atomic_fetch_sub(&w->active_jobs, 1);
  return nullptr;
}

bool pw_start_exec(plugin_workers *w, const char *owner, uint64_t req_id, const char *cmd,
                   const char *const *args, size_t nargs, const char *cwd, const uint8_t *stdin_data,
                   size_t stdin_len, uint32_t timeout_ms)
{
  if (!w || !owner || !cmd || !cmd[0]) return false;
  exec_job *j = ccord_calloc(1, sizeof *j);
  if (!j) return false;
  j->w = w; snprintf(j->owner, sizeof j->owner, "%s", owner); j->req_id = req_id;
  if (cwd && cwd[0]) snprintf(j->cwd, sizeof j->cwd, "%s", cwd);
  j->timeout_ms = timeout_ms;
  j->argc = 1u + nargs;
  j->argv = ccord_calloc(j->argc + 1u, sizeof(char *));
  if (!j->argv) { ccord_free(j); return false; }
  j->argv[0] = ccord_strdup(cmd);
  for (size_t i = 0; i < nargs; i++) j->argv[1u + i] = ccord_strdup(args[i] ? args[i] : "");
  if (stdin_data && stdin_len > 0) {
    j->stdin_data = ccord_malloc(stdin_len);
    if (j->stdin_data) { memcpy(j->stdin_data, stdin_data, stdin_len); j->stdin_len = stdin_len; }
  }
  atomic_fetch_add(&w->active_jobs, 1);
  pthread_t t;
  if (pthread_create(&t, nullptr, exec_worker_fn, j) != 0) {
    atomic_fetch_sub(&w->active_jobs, 1);
    for (size_t i = 0; i < j->argc; i++) ccord_free(j->argv[i]);
    ccord_free(j->argv); ccord_free(j->stdin_data); ccord_free(j);
    return false;
  }
  pthread_detach(t);
  return true;
}

#if !CCORD_OS_WINDOWS
static void proc_emit(plugin_workers *w, const char *owner, uint64_t proc_id, const char *sub_kind, const char *json)
{
  if (atomic_load(&w->shutting_down)) return;
  pw_event ev = {.kind = PW_EV_PROC, .id = proc_id, .ok = true};
  snprintf(ev.owner, sizeof ev.owner, "%s", owner);
  snprintf(ev.sub_kind, sizeof ev.sub_kind, "%s", sub_kind);
  ev.json = ccord_strdup(json ? json : "{}");
  pw_push_ev(w, &ev);
}

static void *spawn_worker_fn(void *arg)
{
  pw_proc_slot *s = arg;
  plugin_workers *w = s->parent;
  bool open_out = s->stdout_fd >= 0, open_err = s->stderr_fd >= 0;
  while ((open_out || open_err) && !atomic_load(&s->stop) && !atomic_load(&w->shutting_down)) {
    struct pollfd pfs[2]; int np = 0;
    if (open_out) pfs[np++] = (struct pollfd){.fd = s->stdout_fd, .events = POLLIN};
    if (open_err) pfs[np++] = (struct pollfd){.fd = s->stderr_fd, .events = POLLIN};
    int pr = poll(pfs, (nfds_t)np, 50);
    if (pr <= 0) continue;
    char tmp[4096];
    for (int k = 0; k < np; k++) {
      if (!(pfs[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      ssize_t r = read(pfs[k].fd, tmp, sizeof tmp);
      if (r > 0) {
        ccord_buf jb; ccord_buf_init(&jb, 0); ccord_jw jw; ccord_jw_init(&jw, &jb);
        ccord_jw_begin_object(&jw); ccord_jw_key(&jw, "data"); ccord_jw_string(&jw, tmp, (size_t)r); ccord_jw_end_object(&jw);
        if (ccord_jw_ok(&jw)) proc_emit(w, s->owner, s->proc_id, pfs[k].fd == s->stdout_fd ? "stdout" : "stderr", ccord_buf_cstr(&jb));
        ccord_buf_free(&jb);
      } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
        if (pfs[k].fd == s->stdout_fd) open_out = false;
        else open_err = false;
      }
    }
  }
  pthread_mutex_lock(&s->io_mu);
  if (s->stdin_fd >= 0) { close(s->stdin_fd); s->stdin_fd = -1; }
  pthread_mutex_unlock(&s->io_mu);
  if (s->stdout_fd >= 0) { close(s->stdout_fd); s->stdout_fd = -1; }
  if (s->stderr_fd >= 0) { close(s->stderr_fd); s->stderr_fd = -1; }
  int status = 0;
  if (s->pid > 0) {
    if (atomic_load(&s->stop)) kill(s->pid, SIGKILL);
    (void)waitpid(s->pid, &status, 0);
    s->pid = -1;
  }
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? -WTERMSIG(status) : -1);
  char eb[64]; snprintf(eb, sizeof eb, "{\"code\":%d}", code);
  proc_emit(w, s->owner, s->proc_id, "exit", eb);
  return nullptr;
}
#endif

bool pw_start_spawn(plugin_workers *w, const char *owner, uint64_t proc_id, const char *cmd,
                    const char *const *args, size_t nargs, const char *cwd)
{
#if CCORD_OS_WINDOWS
  (void)w; (void)owner; (void)proc_id; (void)cmd; (void)args; (void)nargs; (void)cwd;
  return false;
#else
  if (!w || !owner || !cmd || !cmd[0]) return false;
  int in_p[2], out_p[2], err_p[2];
  if (pipe(in_p) != 0) return false;
  if (pipe(out_p) != 0) { close(in_p[0]); close(in_p[1]); return false; }
  if (pipe(err_p) != 0) { close(in_p[0]); close(in_p[1]); close(out_p[0]); close(out_p[1]); return false; }

  char **argv = ccord_calloc(nargs + 2u, sizeof(char *));
  if (!argv) { close(in_p[0]); close(in_p[1]); close(out_p[0]); close(out_p[1]); close(err_p[0]); close(err_p[1]); return false; }
  argv[0] = ccord_strdup(cmd);
  for (size_t i = 0; i < nargs; i++) argv[1u + i] = ccord_strdup(args[i] ? args[i] : "");

  pid_t pid = fork();
  if (pid == 0) {
    (void)dup2(in_p[0], STDIN_FILENO);
    (void)dup2(out_p[1], STDOUT_FILENO);
    (void)dup2(err_p[1], STDERR_FILENO);
    close(in_p[0]); close(in_p[1]); close(out_p[0]); close(out_p[1]); close(err_p[0]); close(err_p[1]);
    if (cwd && cwd[0] && chdir(cwd) != 0) _exit(127);
    execvp(argv[0], argv);
    _exit(127);
  }
  for (size_t i = 0; i < nargs + 1u; i++) ccord_free(argv[i]);
  ccord_free(argv);
  close(in_p[0]); close(out_p[1]); close(err_p[1]);
  if (pid < 0) { close(in_p[1]); close(out_p[0]); close(err_p[0]); return false; }
  int fl1 = fcntl(out_p[0], F_GETFL, 0); if (fl1 >= 0) (void)fcntl(out_p[0], F_SETFL, fl1 | O_NONBLOCK);
  int fl2 = fcntl(err_p[0], F_GETFL, 0); if (fl2 >= 0) (void)fcntl(err_p[0], F_SETFL, fl2 | O_NONBLOCK);

  pw_proc_slot *s = ccord_calloc(1, sizeof *s);
  if (!s) { kill(pid, SIGKILL); close(in_p[1]); close(out_p[0]); close(err_p[0]); return false; }
  s->active = true; s->proc_id = proc_id; s->parent = w; s->pid = pid;
  s->stdin_fd = in_p[1]; s->stdout_fd = out_p[0]; s->stderr_fd = err_p[0];
  snprintf(s->owner, sizeof s->owner, "%s", owner);
  pthread_mutex_init(&s->io_mu, nullptr);

  pthread_mutex_lock(&w->slot_mu);
  size_t slot = PW_MAX_PROCS;
  for (size_t i = 0; i < PW_MAX_PROCS; i++) {
    if (!w->procs[i]) { slot = i; w->procs[i] = s; break; }
  }
  pthread_mutex_unlock(&w->slot_mu);
  if (slot == PW_MAX_PROCS || pthread_create(&s->thr, nullptr, spawn_worker_fn, s) != 0) {
    if (slot < PW_MAX_PROCS) { pthread_mutex_lock(&w->slot_mu); w->procs[slot] = nullptr; pthread_mutex_unlock(&w->slot_mu); }
    kill(pid, SIGKILL); close(in_p[1]); close(out_p[0]); close(err_p[0]);
    pthread_mutex_destroy(&s->io_mu); ccord_free(s);
    return false;
  }
  return true;
#endif
}

bool pw_proc_write(plugin_workers *w, const char *owner, uint64_t proc_id, const uint8_t *data, size_t len)
{
#if CCORD_OS_WINDOWS
  (void)w; (void)owner; (void)proc_id; (void)data; (void)len;
  return false;
#else
  if (!w || !owner || !data || !len) return false;
  bool ok = false;
  pthread_mutex_lock(&w->slot_mu);
  for (size_t i = 0; i < PW_MAX_PROCS; i++) {
    pw_proc_slot *s = w->procs[i];
    if (s && s->proc_id == proc_id && strcmp(s->owner, owner) == 0) {
      pthread_mutex_lock(&s->io_mu);
      if (s->stdin_fd >= 0) ok = write(s->stdin_fd, data, len) == (ssize_t)len;
      pthread_mutex_unlock(&s->io_mu);
      break;
    }
  }
  pthread_mutex_unlock(&w->slot_mu);
  return ok;
#endif
}

void pw_proc_kill(plugin_workers *w, const char *owner, uint64_t proc_id)
{
  if (!w || !owner) return;
  pthread_mutex_lock(&w->slot_mu);
  for (size_t i = 0; i < PW_MAX_PROCS; i++) {
    pw_proc_slot *s = w->procs[i];
    if (s && s->proc_id == proc_id && strcmp(s->owner, owner) == 0) {
      atomic_store(&s->stop, true);
#if !CCORD_OS_WINDOWS
      if (s->pid > 0) kill(s->pid, SIGKILL);
#endif
      break;
    }
  }
  pthread_mutex_unlock(&w->slot_mu);
}

