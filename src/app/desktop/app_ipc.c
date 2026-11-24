// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Single-instance control socket (Win32 Named Pipe with owner SID ACL / POSIX AF_UNIX with peer credential check).
#define _GNU_SOURCE
#include "app_desktop.h"

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/os.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CCORD_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif
#endif

// ---------------------------------------------------------------- single-instance control socket

#define IPC_CMD_MAX 512
#define IPC_ACK_MAX 544

static bool ipc_cmd_valid(const char *cmd)
{
  if (!cmd || !cmd[0]) return false;
  size_t n = strlen(cmd);
  if (n >= IPC_CMD_MAX) return false;
  if (strncmp(cmd, "open-url ", 9) == 0) {
    if (n <= 9) return false;
    for (size_t i = 9; i < n; i++) {
      unsigned char c = (unsigned char)cmd[i];
      if (c <= 0x20u || c >= 0x7Fu) return false;
    }
    return true;
  }
  for (size_t i = 0; i < n; i++) {
    char c = cmd[i];
    if (!((c >= 'a' && c <= 'z') || c == '-')) return false;
  }
  return true;
}

#if CCORD_OS_WINDOWS

struct ccord_app_ipc {
  HANDLE pipe;
  HANDLE ev;
  OVERLAPPED ov;
  bool pending;
  char path[CCORD_PATH_MAX];
};

static bool win_current_user_sid(char *out, size_t cap)
{
  if (!out || cap == 0) return false;
  out[0] = '\0';
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
  DWORD need = 0;
  (void)GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
  if (need == 0 || need > 4096) {
    CloseHandle(tok);
    return false;
  }
  unsigned char buf[4096];
  bool ok = false;
  if (GetTokenInformation(tok, TokenUser, buf, need, &need)) {
    TOKEN_USER *tu = (TOKEN_USER *)buf;
    LPSTR sid_str = nullptr;
    if (ConvertSidToStringSidA(tu->User.Sid, &sid_str) && sid_str) {
      int n = snprintf(out, cap, "%s", sid_str);
      ok = n > 0 && (size_t)n < cap;
      LocalFree(sid_str);
    }
  }
  CloseHandle(tok);
  return ok;
}

static bool win_make_owner_sa(SECURITY_ATTRIBUTES *sa, PSECURITY_DESCRIPTOR *out_sd)
{
  *out_sd = nullptr;
  char sid[128];
  if (!win_current_user_sid(sid, sizeof sid)) return false;
  char sddl[192];
  int n = snprintf(sddl, sizeof sddl, "D:P(A;;GA;;;%s)", sid);
  if (n <= 0 || (size_t)n >= sizeof sddl) return false;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, out_sd, nullptr)) {
    *out_sd = nullptr;
    return false;
  }
  sa->nLength = sizeof *sa;
  sa->lpSecurityDescriptor = *out_sd;
  sa->bInheritHandle = FALSE;
  return true;
}

static HANDLE win_create_ipc_pipe(const char *path, bool first)
{
  SECURITY_ATTRIBUTES sa = {0};
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!win_make_owner_sa(&sa, &sd)) return INVALID_HANDLE_VALUE;
  DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
  if (first) open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
  HANDLE h = CreateNamedPipeA(path, open_mode,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                              PIPE_UNLIMITED_INSTANCES, 1024, 1024, 0, &sa);
  LocalFree(sd);
  return h;
}

void ccord_app_ipc_resolve_path(char *out, size_t cap, const char *override_path)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (override_path && override_path[0]) {
    snprintf(out, cap, "%s", override_path);
    return;
  }
  char sid[128];
  if (win_current_user_sid(sid, sizeof sid)) {
    snprintf(out, cap, "\\\\.\\pipe\\ccord-control-%s", sid);
    return;
  }
  snprintf(out, cap, "\\\\.\\pipe\\ccord-control-user");
}

static bool win_ipc_arm(ccord_app_ipc *ipc)
{
  if (ipc->pipe == INVALID_HANDLE_VALUE) {
    ipc->pipe = win_create_ipc_pipe(ipc->path, false);
    if (ipc->pipe == INVALID_HANDLE_VALUE) return false;
  }
  ResetEvent(ipc->ev);
  memset(&ipc->ov, 0, sizeof ipc->ov);
  ipc->ov.hEvent = ipc->ev;
  BOOL ok = ConnectNamedPipe(ipc->pipe, &ipc->ov);
  if (ok || GetLastError() == ERROR_PIPE_CONNECTED) {
    SetEvent(ipc->ev);
    ipc->pending = true;
    return true;
  }
  if (GetLastError() == ERROR_IO_PENDING) {
    ipc->pending = true;
    return true;
  }
  CloseHandle(ipc->pipe);
  ipc->pipe = INVALID_HANDLE_VALUE;
  ipc->pending = false;
  return false;
}

ccord_app_ipc *ccord_app_ipc_listen(const char *path, ccord_error *err)
{
  ccord_error_clear(err);
  if (!path || !path[0]) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "ipc: pipe path missing");
    return nullptr;
  }
  HANDLE h = win_create_ipc_pipe(path, true);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY) {
      (void)ccord_error_set(err, CCORD_ERR_PROTOCOL, "another ccord instance is already running");
    } else {
      (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: CreateNamedPipe failed");
    }
    return nullptr;
  }
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ev) {
    CloseHandle(h);
    (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: CreateEvent failed");
    return nullptr;
  }
  ccord_app_ipc *ipc = ccord_calloc(1, sizeof *ipc);
  if (!ipc) {
    CloseHandle(ev);
    CloseHandle(h);
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "ipc: out of memory");
    return nullptr;
  }
  ipc->pipe = h;
  ipc->ev = ev;
  snprintf(ipc->path, sizeof ipc->path, "%s", path);
  if (!win_ipc_arm(ipc)) {
    ccord_app_ipc_destroy(ipc);
    (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: ConnectNamedPipe failed");
    return nullptr;
  }
  return ipc;
}

void ccord_app_ipc_destroy(ccord_app_ipc *ipc)
{
  if (!ipc) return;
  if (ipc->pipe != INVALID_HANDLE_VALUE) {
    if (ipc->pending) {
      (void)CancelIoEx(ipc->pipe, &ipc->ov);
      DWORD ignored = 0;
      (void)GetOverlappedResult(ipc->pipe, &ipc->ov, &ignored, TRUE);
      ipc->pending = false;
    }
    CloseHandle(ipc->pipe);
  }
  if (ipc->ev) CloseHandle(ipc->ev);
  ccord_free(ipc);
}

const char *ccord_app_ipc_path(const ccord_app_ipc *ipc)
{
  return ipc ? ipc->path : "";
}

ccord_socket_t ccord_app_ipc_wait_fd(const ccord_app_ipc *ipc)
{
  (void)ipc;
  return CCORD_INVALID_SOCKET;
}

bool ccord_app_ipc_poll(ccord_app_ipc *ipc, char *out, size_t cap)
{
  if (!ipc || !out || cap == 0) return false;
  for (int attempt = 0; attempt < 8; attempt++) {
    if (!ipc->pending && !win_ipc_arm(ipc)) return false;
    if (WaitForSingleObject(ipc->ev, 0) != WAIT_OBJECT_0) return false;
    HANDLE client = ipc->pipe;
    ipc->pipe = INVALID_HANDLE_VALUE;
    ipc->pending = false;
    (void)win_ipc_arm(ipc);

    bool ok = false;
    char buf[IPC_CMD_MAX + 2];
    OVERLAPPED rov = {0};
    rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (rov.hEvent) {
      DWORD n = 0;
      BOOL rd = ReadFile(client, buf, (DWORD)(sizeof buf - 1), &n, &rov);
      if (!rd && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(rov.hEvent, 50) == WAIT_OBJECT_0) {
          rd = GetOverlappedResult(client, &rov, &n, FALSE);
        } else {
          (void)CancelIoEx(client, &rov);
          DWORD ignored = 0;
          (void)GetOverlappedResult(client, &rov, &ignored, TRUE);
        }
      }
      CloseHandle(rov.hEvent);
      if (rd && n > 0) {
        buf[n] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';
        if (ipc_cmd_valid(buf)) {
          snprintf(out, cap, "%s", buf);
          char ack[IPC_ACK_MAX];
          int alen = snprintf(ack, sizeof ack, "ipc_ack=%s\n", buf);
          OVERLAPPED wov = {0};
          wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
          if (wov.hEvent) {
            DWORD wr = 0;
            if (!WriteFile(client, ack, (DWORD)alen, &wr, &wov) && GetLastError() == ERROR_IO_PENDING) {
              if (WaitForSingleObject(wov.hEvent, 50) != WAIT_OBJECT_0) {
                (void)CancelIoEx(client, &wov);
              }
              (void)GetOverlappedResult(client, &wov, &wr, TRUE);
            }
            CloseHandle(wov.hEvent);
          }
          ok = true;
        }
      }
    }
    (void)DisconnectNamedPipe(client);
    CloseHandle(client);
    if (ok) return true;
  }
  return false;
}

bool ccord_app_ipc_send(const char *path, const char *cmd, ccord_error *err)
{
  ccord_error_clear(err);
  if (!path || !path[0] || !ipc_cmd_valid(cmd)) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "ipc: invalid pipe path or command");
    return false;
  }
  HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    (void)ccord_error_set(err, CCORD_ERR_NOT_FOUND, "no running ccord instance on the control pipe");
    return false;
  }
  char line[IPC_CMD_MAX + 2];
  int len = snprintf(line, sizeof line, "%s\n", cmd);
  DWORD written = 0;
  bool ok = WriteFile(h, line, (DWORD)len, &written, nullptr) && written == (DWORD)len;
  if (ok) {
    char ack[IPC_ACK_MAX], expect[IPC_ACK_MAX];
    snprintf(expect, sizeof expect, "ipc_ack=%s", cmd);
    DWORD rd = 0;
    if (ReadFile(h, ack, (DWORD)(sizeof ack - 1), &rd, nullptr) && rd > 0) {
      ack[rd] = '\0';
      ack[strcspn(ack, "\r\n")] = '\0';
      ok = strcmp(ack, expect) == 0;
    } else {
      ok = false;
    }
  }
  CloseHandle(h);
  if (!ok) (void)ccord_error_set(err, CCORD_ERR_PROTOCOL, "ipc: no valid ack from the running instance");
  return ok;
}

bool ccord_app_ipc_handoff(const char *path, ccord_error *err)
{
  return ccord_app_ipc_send(path, "show", err);
}

#else

struct ccord_app_ipc {
  int fd;                        // non-blocking listener; -1 after destroy
  char path[CCORD_PATH_MAX];
};

void ccord_app_ipc_resolve_path(char *out, size_t cap, const char *override_path)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (override_path && override_path[0]) {
    snprintf(out, cap, "%s", override_path);
    return;
  }
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (runtime && runtime[0]) {
    snprintf(out, cap, "%s/ccord/control.sock", runtime);
    return;
  }
  const char *tmp = getenv("TMPDIR");
  if (tmp && tmp[0]) {
    struct stat st;
    if (lstat(tmp, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 0077) == 0) {
      snprintf(out, cap, "%s/ccord-control-%ld.sock", tmp, (long)getuid());
    }
  }
  // No runtime dir and no TMPDIR: leave the path empty; the caller runs without single-instance IPC.
}

static bool ipc_sockaddr(const char *path, struct sockaddr_un *out)
{
  if (!path || !path[0] || strlen(path) >= sizeof out->sun_path) return false;
  memset(out, 0, sizeof *out);
  out->sun_family = AF_UNIX;
  snprintf(out->sun_path, sizeof out->sun_path, "%s", path);
  return true;
}

static bool ipc_peer_same_user(int fd, pid_t *out_pid)
{
  if (out_pid) *out_pid = 0;
#ifdef SO_PEERCRED
  struct ucred cred = {0};
  socklen_t clen = sizeof cred;
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0 || cred.uid != geteuid()) return false;
  if (out_pid && cred.pid > 1) *out_pid = cred.pid;
  return true;
#elif CCORD_OS_MACOS
  uid_t euid = 0;
  gid_t egid = 0;
  if (getpeereid(fd, &euid, &egid) != 0 || euid != geteuid()) return false;
  if (out_pid) {
    pid_t pid = 0;
    socklen_t plen = sizeof pid;
    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &plen) == 0 && pid > 1) *out_pid = pid;
  }
  return true;
#else
  (void)fd;
  return false;
#endif
}

// A live listener answers connect(); a stale socket file refuses it.
static bool ipc_probe_alive(const char *path)
{
  struct sockaddr_un addr;
  if (!ipc_sockaddr(path, &addr)) return false;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  (void)ccord_sock_set_cloexec(fd);
  bool alive = connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
  close(fd);
  return alive;
}

ccord_app_ipc *ccord_app_ipc_listen(const char *path, ccord_error *err)
{
  ccord_error_clear(err);
  struct sockaddr_un addr;
  if (!ipc_sockaddr(path, &addr)) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "ipc: socket path missing or too long");
    return nullptr;
  }
  // Create the parent directory (e.g. $XDG_RUNTIME_DIR/ccord) private to the user.
  char parent[CCORD_PATH_MAX];
  snprintf(parent, sizeof parent, "%s", path);
  char *slash = strrchr(parent, '/');
  if (slash && slash != parent) {
    *slash = '\0';
    if (mkdir(parent, 0700) != 0 && errno != EEXIST) {
      (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: cannot create socket directory");
      return nullptr;
    }
  }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: socket() failed");
    return nullptr;
  }
  (void)ccord_sock_set_cloexec(fd);
  (void)fchmod(fd, 0600);
  mode_t old_umask = umask(0077);
  int rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
  int bind_errno = errno;
  umask(old_umask);
  if (rc != 0) {
    if (bind_errno == EADDRINUSE) {
      if (ipc_probe_alive(path)) {
        close(fd);
        (void)ccord_error_set(err, CCORD_ERR_PROTOCOL, "another ccord instance is already running");
        return nullptr;
      }
      struct stat st;
      if (lstat(path, &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == geteuid()) {
        (void)unlink(path);   // stale socket from a crashed instance: recover and re-bind
      }
      old_umask = umask(0077);
      rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
      umask(old_umask);
    }
    if (rc != 0) {
      close(fd);
      (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: bind() failed");
      return nullptr;
    }
  }
  (void)chmod(path, 0600);
  if (listen(fd, 4) != 0 || !ccord_sock_set_nonblock(fd)) {
    close(fd);
    (void)unlink(path);
    (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: listen() failed");
    return nullptr;
  }
  ccord_app_ipc *ipc = ccord_calloc(1, sizeof *ipc);
  if (!ipc) {
    close(fd);
    (void)unlink(path);
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "ipc: out of memory");
    return nullptr;
  }
  ipc->fd = fd;
  snprintf(ipc->path, sizeof ipc->path, "%s", path);
  return ipc;
}

void ccord_app_ipc_destroy(ccord_app_ipc *ipc)
{
  if (!ipc) return;
  if (ipc->fd >= 0) close(ipc->fd);
  if (ipc->path[0]) (void)unlink(ipc->path);
  ccord_free(ipc);
}

const char *ccord_app_ipc_path(const ccord_app_ipc *ipc)
{
  return ipc ? ipc->path : "";
}

ccord_socket_t ccord_app_ipc_wait_fd(const ccord_app_ipc *ipc)
{
  return (ipc && ipc->fd >= 0) ? (ccord_socket_t)ipc->fd : CCORD_INVALID_SOCKET;
}

bool ccord_app_ipc_poll(ccord_app_ipc *ipc, char *out, size_t cap)
{
  if (!ipc || ipc->fd < 0 || !out || cap == 0) return false;
  // Drain up to 8 pending connections so an empty liveness probe (ipc_probe_alive) never delays
  // a real CLI command ("show", "quit", ...) sitting right behind it in the accept backlog.
  for (int attempt = 0; attempt < 8; attempt++) {
    int cfd = accept(ipc->fd, nullptr, nullptr);   // listener is non-blocking
    if (cfd < 0) return false;
    if (!ipc_peer_same_user(cfd, nullptr)) {
      close(cfd);
      continue;
    }
    ccord_sock_set_nosigpipe(cfd);
    bool ok = false;
    char buf[IPC_CMD_MAX + 2];
    struct pollfd pfd = {.fd = cfd, .events = POLLIN};
    if (poll(&pfd, 1, 15) > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
      ssize_t n = read(cfd, buf, sizeof buf - 1);
      if (n > 0) {
        buf[n] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';
        if (ipc_cmd_valid(buf)) {
          snprintf(out, cap, "%s", buf);
          char ack[IPC_ACK_MAX];
          int alen = snprintf(ack, sizeof ack, "ipc_ack=%s\n", buf);
          (void)ccord_sock_set_nonblock(cfd);
          ssize_t w = write(cfd, ack, (size_t)alen);   // best effort: the client waits for this ack
          (void)w;
          ok = true;
        }
      }
    }
    close(cfd);
    if (ok) return true;
  }
  return false;
}

static bool ipc_peer_is_ccord(pid_t pid)
{
  if (pid <= 1) return false;
#if CCORD_OS_MACOS
  char path[PROC_PIDPATHINFO_MAXSIZE];
  int n = proc_pidpath(pid, path, sizeof path);
  if (n <= 0) return false;
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  return strcmp(base, "ccord") == 0 || strcmp(base, "c-cord") == 0;
#else
  char path[64], comm[64];
  snprintf(path, sizeof path, "/proc/%ld/comm", (long)pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  ssize_t n = read(fd, comm, sizeof comm - 1);
  close(fd);
  if (n <= 0) return false;
  while (n > 0 && (comm[n - 1] == '\n' || comm[n - 1] == '\r')) n--;
  comm[n] = '\0';
  return strcmp(comm, "ccord") == 0 || strcmp(comm, "c-cord") == 0;
#endif
}

static bool ipc_peer_is_stale_exe(pid_t peer_pid)
{
  if (peer_pid <= 1 || peer_pid == getpid()) return false;
#if CCORD_OS_MACOS
  char peer_exe[PROC_PIDPATHINFO_MAXSIZE], self_exe[PROC_PIDPATHINFO_MAXSIZE];
  if (proc_pidpath(peer_pid, peer_exe, sizeof peer_exe) <= 0 ||
      proc_pidpath(getpid(), self_exe, sizeof self_exe) <= 0) {
    return false;
  }
  struct stat peer_st, self_st;
  if (stat(peer_exe, &peer_st) == 0 && stat(self_exe, &self_st) == 0) {
    if (peer_st.st_size != self_st.st_size || peer_st.st_mtime < self_st.st_mtime) return true;
  }
  return false;
#else
  char link_path[64], peer_exe[CCORD_PATH_MAX], self_exe[CCORD_PATH_MAX];
  snprintf(link_path, sizeof link_path, "/proc/%ld/exe", (long)peer_pid);
  ssize_t pn = readlink(link_path, peer_exe, sizeof peer_exe - 1);
  if (pn <= 0) return false;
  peer_exe[pn] = '\0';
  static const char k_del[] = " (deleted)";
  if ((size_t)pn >= sizeof k_del - 1 &&
      memcmp(peer_exe + pn - (sizeof k_del - 1), k_del, sizeof k_del - 1) == 0) {
    return true;
  }
  ssize_t sn = readlink("/proc/self/exe", self_exe, sizeof self_exe - 1);
  if (sn <= 0) return false;
  self_exe[sn] = '\0';
  struct stat peer_st, self_st;
  if (stat(peer_exe, &peer_st) == 0 && stat(self_exe, &self_st) == 0) {
    if (peer_st.st_size != self_st.st_size || peer_st.st_mtime < self_st.st_mtime) return true;
  }
  return false;
#endif
}

bool ccord_app_ipc_handoff(const char *path, ccord_error *err)
{
  ccord_error_clear(err);
  struct sockaddr_un addr;
  if (!ipc_sockaddr(path, &addr)) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "ipc: invalid socket path");
    return false;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: socket() failed");
    return false;
  }
  (void)ccord_sock_set_cloexec(fd);
  ccord_sock_set_nosigpipe(fd);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    if (errno == ECONNREFUSED) {
      struct stat st;
      if (lstat(path, &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == geteuid()) {
        (void)unlink(path);
      }
    }
    close(fd);
    (void)ccord_error_set(err, CCORD_ERR_NOT_FOUND, "no running ccord instance on the control socket");
    return false;
  }

  pid_t peer_pid = 0;
  if (!ipc_peer_same_user(fd, &peer_pid)) {
    close(fd);
    (void)ccord_error_set(err, CCORD_ERR_PROTOCOL, "ipc: control socket owned by another user");
    return false;
  }

  bool stale = ipc_peer_is_stale_exe(peer_pid);
  const char *cmd = stale ? "quit" : "show";
  char line[IPC_CMD_MAX + 2];
  int len = snprintf(line, sizeof line, "%s\n", cmd);
  bool ok = false;
  if (write(fd, line, (size_t)len) == len) {
    char ack[IPC_ACK_MAX];
    char expect[IPC_ACK_MAX];
    snprintf(expect, sizeof expect, "ipc_ack=%s", cmd);
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, 2000) > 0 && (pfd.revents & POLLIN)) {
      ssize_t n = read(fd, ack, sizeof ack - 1);
      if (n > 0) {
        ack[n] = '\0';
        ack[strcspn(ack, "\r\n")] = '\0';
        ok = strcmp(ack, expect) == 0;
      }
    }
  }
  close(fd);

  if (!stale && ok) return true;   // healthy current primary surfaced its window

  if (peer_pid > 1) {
    for (int i = 0; i < 40 && kill(peer_pid, 0) == 0; i++) ccord_sleep_ms(20);
    if (kill(peer_pid, 0) == 0 && ipc_peer_is_ccord(peer_pid)) {
      (void)kill(peer_pid, SIGTERM);
      for (int i = 0; i < 20 && kill(peer_pid, 0) == 0; i++) ccord_sleep_ms(20);
    }
  }
  struct stat st;
  if (lstat(path, &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == geteuid()) {
    (void)unlink(path);
  }
  (void)ccord_error_set(err, CCORD_ERR_PROTOCOL,
                        stale ? "replaced stale background ccord instance" : "evicted unresponsive ccord instance");
  return false;
}

bool ccord_app_ipc_send(const char *path, const char *cmd, ccord_error *err)
{
  ccord_error_clear(err);
  struct sockaddr_un addr;
  if (!ipc_sockaddr(path, &addr) || !ipc_cmd_valid(cmd)) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "ipc: invalid socket path or command");
    return false;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: socket() failed");
    return false;
  }
  (void)ccord_sock_set_cloexec(fd);
  ccord_sock_set_nosigpipe(fd);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    (void)ccord_error_set(err, CCORD_ERR_NOT_FOUND, "no running ccord instance on the control socket");
    return false;
  }
  if (!ipc_peer_same_user(fd, nullptr)) {
    close(fd);
    (void)ccord_error_set(err, CCORD_ERR_PROTOCOL, "ipc: control socket owned by another user");
    return false;
  }
  char line[IPC_CMD_MAX + 2];
  int len = snprintf(line, sizeof line, "%s\n", cmd);
  ssize_t w = write(fd, line, (size_t)len);
  if (w != len) {
    close(fd);
    (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: command write failed");
    return false;
  }
  char ack[IPC_ACK_MAX];
  char expect[IPC_ACK_MAX];
  snprintf(expect, sizeof expect, "ipc_ack=%s", cmd);
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  bool ok = false;
  if (poll(&pfd, 1, 2000) > 0 && (pfd.revents & POLLIN)) {
    ssize_t n = read(fd, ack, sizeof ack - 1);
    if (n > 0) {
      ack[n] = '\0';
      ack[strcspn(ack, "\r\n")] = '\0';
      ok = strcmp(ack, expect) == 0;
    }
  }
  close(fd);
  if (!ok) (void)ccord_error_set(err, CCORD_ERR_PROTOCOL, "ipc: no valid ack from the running instance");
  return ok;
}
#endif

