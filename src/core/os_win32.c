// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>

#include <ccord/core/os.h>
#include <ccord/core/alloc.h>

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool utf8_to_wide(const char *s, wchar_t *out, int cap)
{
  if (!s || !out || cap <= 0) return false;
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
  return n > 0 && n <= cap;
}

static bool wide_to_utf8(const wchar_t *w, char *out, int cap)
{
  if (!w || !out || cap <= 0) return false;
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, nullptr, nullptr);
  return n > 0 && n <= cap;
}

const char *ccord_os_name(void)
{
  return "windows";
}

const char *ccord_os_arch(void)
{
#if defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#else
  return "x86_64";
#endif
}

bool ccord_os_is_mobile(void)
{
  return false;
}

int ccord_os_android_api_level(void)
{
  return 0;
}

uint64_t ccord_os_pid(void)
{
  return (uint64_t)GetCurrentProcessId();
}

bool ccord_os_hostname(char *out, size_t cap)
{
  if (!out || cap == 0) return false;
  wchar_t wbuf[256];
  DWORD n = 256;
  if (!GetComputerNameExW(ComputerNameDnsHostname, wbuf, &n)) {
    out[0] = '\0';
    return false;
  }
  return wide_to_utf8(wbuf, out, (int)cap);
}

bool ccord_os_getcwd(char *out, size_t cap)
{
  if (!out || cap == 0) return false;
  wchar_t wbuf[CCORD_PATH_MAX];
  DWORD n = GetCurrentDirectoryW(CCORD_PATH_MAX - 1, wbuf);
  return n > 0 && n < CCORD_PATH_MAX - 1 && wide_to_utf8(wbuf, out, (int)cap);
}

bool ccord_os_config_dir(char *out, size_t cap)
{
  if (!out || cap == 0) return false;
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0] != '\0') {
    size_t xn = strlen(xdg);
    while (xn > 1 && (xdg[xn - 1] == '/' || xdg[xn - 1] == '\\')) xn--;
    int n = snprintf(out, cap, "%.*s/ccord", (int)xn, xdg);
    return n > 0 && (size_t)n < cap;
  }
  PWSTR wdir = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, nullptr, &wdir)) && wdir) {
    char base[CCORD_PATH_MAX];
    bool ok = wide_to_utf8(wdir, base, (int)sizeof base);
    CoTaskMemFree(wdir);
    if (ok) {
      int n = snprintf(out, cap, "%s\\C-Cord", base);
      return n > 0 && (size_t)n < cap;
    }
  }
  const char *env = getenv("APPDATA");
  if (!env || !env[0]) return false;
  int n = snprintf(out, cap, "%s\\C-Cord", env);
  return n > 0 && (size_t)n < cap;
}

bool ccord_os_runtime_dir(char *out, size_t cap)
{
  if (!out || cap == 0) return false;
  wchar_t wtmp[MAX_PATH + 2];
  DWORD n = GetTempPathW(MAX_PATH + 1, wtmp);
  if (n == 0) return false;
  char base[CCORD_PATH_MAX];
  if (!wide_to_utf8(wtmp, base, (int)sizeof base)) return false;
  size_t bl = strlen(base);
  while (bl > 1 && (base[bl - 1] == '\\' || base[bl - 1] == '/')) base[--bl] = '\0';
  int r = snprintf(out, cap, "%s\\C-Cord", base);
  return r > 0 && (size_t)r < cap;
}

void ccord_sleep_ms(uint32_t ms)
{
  Sleep(ms);
}

void ccord_sleep_us(uint32_t us)
{
  Sleep((us + 999u) / 1000u);
}

bool ccord_gmtime(time_t t, struct tm *out)
{
  return out && gmtime_s(out, &t) == 0;
}

bool ccord_localtime(time_t t, struct tm *out)
{
  return out && localtime_s(out, &t) == 0;
}

time_t ccord_timegm(struct tm *tm)
{
  return tm ? _mkgmtime(tm) : (time_t)-1;
}

int ccord_strcasecmp(const char *a, const char *b)
{
  if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
  while (*a && *b) {
    int ca = tolower((unsigned char)*a++);
    int cb = tolower((unsigned char)*b++);
    if (ca != cb) return ca - cb;
  }
  return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int ccord_strncasecmp(const char *a, const char *b, size_t n)
{
  if (n == 0) return 0;
  if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
  for (size_t i = 0; i < n; i++) {
    int ca = tolower((unsigned char)a[i]);
    int cb = tolower((unsigned char)b[i]);
    if (ca != cb || ca == 0) return ca - cb;
  }
  return 0;
}

// Threads, SRWLOCK mutexes, Condition Variables ---------------------------------------------------
struct ccord_mutex {
  SRWLOCK lock;
};

struct ccord_cond {
  CONDITION_VARIABLE cv;
};

struct ccord_thread {
  HANDLE handle;
  ccord_thread_fn fn;
  void *arg;
};

ccord_mutex *ccord_mutex_create(void)
{
  ccord_mutex *m = ccord_calloc(1, sizeof *m);
  if (!m) return nullptr;
  InitializeSRWLock(&m->lock);
  return m;
}

void ccord_mutex_destroy(ccord_mutex *m)
{
  ccord_free(m);
}

void ccord_mutex_lock(ccord_mutex *m)
{
  if (m) AcquireSRWLockExclusive(&m->lock);
}

void ccord_mutex_unlock(ccord_mutex *m)
{
  if (m) ReleaseSRWLockExclusive(&m->lock);
}

ccord_cond *ccord_cond_create(void)
{
  ccord_cond *c = ccord_calloc(1, sizeof *c);
  if (!c) return nullptr;
  InitializeConditionVariable(&c->cv);
  return c;
}

void ccord_cond_destroy(ccord_cond *c)
{
  ccord_free(c);
}

void ccord_cond_signal(ccord_cond *c)
{
  if (c) WakeConditionVariable(&c->cv);
}

void ccord_cond_broadcast(ccord_cond *c)
{
  if (c) WakeAllConditionVariable(&c->cv);
}

void ccord_cond_wait(ccord_cond *c, ccord_mutex *m)
{
  if (c && m) SleepConditionVariableSRW(&c->cv, &m->lock, INFINITE, 0);
}

bool ccord_cond_wait_timeout(ccord_cond *c, ccord_mutex *m, uint32_t timeout_ms)
{
  if (!c || !m) return false;
  if (SleepConditionVariableSRW(&c->cv, &m->lock, timeout_ms, 0)) return true;
  return GetLastError() != ERROR_TIMEOUT;
}

static DWORD WINAPI win_thread_trampoline(LPVOID p)
{
  ccord_thread *t = p;
  t->fn(t->arg);
  return 0;
}

ccord_thread *ccord_thread_create(ccord_thread_fn fn, void *arg)
{
  if (!fn) return nullptr;
  ccord_thread *t = ccord_calloc(1, sizeof *t);
  if (!t) return nullptr;
  t->fn = fn;
  t->arg = arg;
  t->handle = CreateThread(nullptr, 0, win_thread_trampoline, t, 0, nullptr);
  if (!t->handle) {
    ccord_free(t);
    return nullptr;
  }
  return t;
}

void ccord_thread_join(ccord_thread *t)
{
  if (!t) return;
  WaitForSingleObject(t->handle, INFINITE);
  CloseHandle(t->handle);
  ccord_free(t);
}

void ccord_spin_lock_acquire(ccord_spin_lock *l)
{
  if (!l) return;
  while (atomic_exchange_explicit(&l->state, 1u, memory_order_acquire) != 0u) {
    SwitchToThread();
  }
}

void ccord_spin_lock_release(ccord_spin_lock *l)
{
  if (l) atomic_store_explicit(&l->state, 0u, memory_order_release);
}

// Directory & filesystem helpers ------------------------------------------------------------------
struct ccord_dir {
  HANDLE h;
  WIN32_FIND_DATAW fd;
  bool first;
};

ccord_dir *ccord_dir_open(const char *path)
{
  if (!path || !path[0]) return nullptr;
  char pat[CCORD_PATH_MAX];
  int n = snprintf(pat, sizeof pat, "%s\\*", path);
  if (n <= 0 || (size_t)n >= sizeof pat) return nullptr;
  wchar_t wpat[CCORD_PATH_MAX];
  if (!utf8_to_wide(pat, wpat, CCORD_PATH_MAX)) return nullptr;
  ccord_dir *d = ccord_calloc(1, sizeof *d);
  if (!d) return nullptr;
  d->h = FindFirstFileW(wpat, &d->fd);
  if (d->h == INVALID_HANDLE_VALUE) {
    ccord_free(d);
    return nullptr;
  }
  d->first = true;
  return d;
}

bool ccord_dir_next(ccord_dir *d, char *out, size_t cap, bool *is_dir)
{
  if (!d || d->h == INVALID_HANDLE_VALUE || !out || cap == 0) return false;
  for (;;) {
    if (d->first) {
      d->first = false;
    } else if (!FindNextFileW(d->h, &d->fd)) {
      return false;
    }
    if (wcscmp(d->fd.cFileName, L".") == 0 || wcscmp(d->fd.cFileName, L"..") == 0) continue;
    if (!wide_to_utf8(d->fd.cFileName, out, (int)cap)) continue;
    if (is_dir) *is_dir = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
  }
}

void ccord_dir_close(ccord_dir *d)
{
  if (!d) return;
  if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
  ccord_free(d);
}

bool ccord_path_exists(const char *path)
{
  wchar_t w[CCORD_PATH_MAX];
  return path && utf8_to_wide(path, w, CCORD_PATH_MAX) && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

bool ccord_path_is_dir(const char *path)
{
  wchar_t w[CCORD_PATH_MAX];
  if (!path || !utf8_to_wide(path, w, CCORD_PATH_MAX)) return false;
  DWORD a = GetFileAttributesW(w);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ccord_path_is_file(const char *path)
{
  wchar_t w[CCORD_PATH_MAX];
  if (!path || !utf8_to_wide(path, w, CCORD_PATH_MAX)) return false;
  DWORD a = GetFileAttributesW(w);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ccord_path_is_symlink(const char *path)
{
  wchar_t w[CCORD_PATH_MAX];
  if (!path || !utf8_to_wide(path, w, CCORD_PATH_MAX)) return false;
  DWORD a = GetFileAttributesW(w);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool ccord_file_size(const char *path, uint64_t *out_size)
{
  wchar_t w[CCORD_PATH_MAX];
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!path || !utf8_to_wide(path, w, CCORD_PATH_MAX)) return false;
  if (!GetFileAttributesExW(w, GetFileExInfoStandard, &fad)) return false;
  if (out_size) *out_size = ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;
  return true;
}

bool ccord_file_mtime_ns(const char *path, int64_t *out_ns)
{
  wchar_t w[CCORD_PATH_MAX];
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!path || !utf8_to_wide(path, w, CCORD_PATH_MAX)) return false;
  if (!GetFileAttributesExW(w, GetFileExInfoStandard, &fad)) return false;
  uint64_t t = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | (uint64_t)fad.ftLastWriteTime.dwLowDateTime;
  const uint64_t epoch_diff = 116444736000000000ULL;
  int64_t ns = (t >= epoch_diff) ? (int64_t)((t - epoch_diff) * 100ULL) : 0;
  if (out_ns) *out_ns = ns;
  return true;
}

bool ccord_file_unlink(const char *path)
{
  wchar_t w[CCORD_PATH_MAX];
  return path && utf8_to_wide(path, w, CCORD_PATH_MAX) && DeleteFileW(w) != 0;
}

bool ccord_file_rename_replace(const char *src, const char *dst)
{
  wchar_t ws[CCORD_PATH_MAX], wd[CCORD_PATH_MAX];
  if (!src || !dst || !utf8_to_wide(src, ws, CCORD_PATH_MAX) || !utf8_to_wide(dst, wd, CCORD_PATH_MAX)) {
    return false;
  }
  return MoveFileExW(ws, wd, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// Winsock2 helpers --------------------------------------------------------------------------------
void ccord_net_os_init(void)
{
  static bool started = false;
  if (!started) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) started = true;
  }
}

void ccord_sock_close(ccord_socket_t s)
{
  if (s != CCORD_INVALID_SOCKET) closesocket((SOCKET)s);
}

bool ccord_sock_set_nonblock(ccord_socket_t s)
{
  if (s == CCORD_INVALID_SOCKET) return false;
  u_long mode = 1;
  return ioctlsocket((SOCKET)s, (long)FIONBIO, &mode) == 0;
}

bool ccord_sock_set_cloexec(ccord_socket_t s)
{
  if (s == CCORD_INVALID_SOCKET) return false;
  return SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0) != 0;
}

void ccord_sock_set_nosigpipe(ccord_socket_t s)
{
  (void)s;
}

ptrdiff_t ccord_sock_send(ccord_socket_t s, const void *buf, size_t len, bool *would_block)
{
  if (would_block) *would_block = false;
  int n = send((SOCKET)s, (const char *)buf, (int)(len > 0x7FFFFFFFu ? 0x7FFFFFFFu : len), 0);
  if (n == SOCKET_ERROR) {
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK || e == WSAEINTR) {
      if (would_block) *would_block = true;
    }
    return -1;
  }
  return (ptrdiff_t)n;
}

ptrdiff_t ccord_sock_recv(ccord_socket_t s, void *buf, size_t len, bool *would_block)
{
  if (would_block) *would_block = false;
  int n = recv((SOCKET)s, (char *)buf, (int)(len > 0x7FFFFFFFu ? 0x7FFFFFFFu : len), 0);
  if (n == SOCKET_ERROR) {
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK || e == WSAEINTR) {
      if (would_block) *would_block = true;
    }
    return -1;
  }
  return (ptrdiff_t)n;
}

bool ccord_sock_pair(ccord_socket_t out[2])
{
  if (!out) return false;
  ccord_net_os_init();
  SOCKET lst = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
  if (lst == INVALID_SOCKET) return false;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  int alen = (int)sizeof addr;
  if (bind(lst, (struct sockaddr *)&addr, alen) != 0 ||
      getsockname(lst, (struct sockaddr *)&addr, &alen) != 0 ||
      listen(lst, 1) != 0) {
    closesocket(lst);
    return false;
  }
  SOCKET c = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
  if (c == INVALID_SOCKET) {
    closesocket(lst);
    return false;
  }
  if (connect(c, (struct sockaddr *)&addr, alen) != 0) {
    closesocket(c);
    closesocket(lst);
    return false;
  }
  SOCKET s = accept(lst, nullptr, nullptr);
  closesocket(lst);
  if (s == INVALID_SOCKET) {
    closesocket(c);
    return false;
  }
  (void)ccord_sock_set_cloexec((ccord_socket_t)s);
  (void)ccord_sock_set_cloexec((ccord_socket_t)c);
  out[0] = (ccord_socket_t)s;
  out[1] = (ccord_socket_t)c;
  return true;
}

int ccord_sock_poll1(ccord_socket_t s, unsigned events, int timeout_ms)
{
  if (s == CCORD_INVALID_SOCKET) return -1;
  WSAPOLLFD pfd = {.fd = (SOCKET)s, .events = 0};
  if (events & CCORD_POLL_IN) pfd.events |= POLLRDNORM;
  if (events & CCORD_POLL_OUT) pfd.events |= POLLWRNORM;
  int rc = WSAPoll(&pfd, 1, timeout_ms);
  if (rc < 0) return -1;
  if (rc == 0) return 0;
  int out = 0;
  if (pfd.revents & POLLRDNORM) out |= CCORD_POLL_IN;
  if (pfd.revents & POLLWRNORM) out |= CCORD_POLL_OUT;
  if (pfd.revents & (POLLERR | POLLNVAL)) out |= CCORD_POLL_ERR;
  if (pfd.revents & POLLHUP) out |= CCORD_POLL_HUP;
  return out;
}
