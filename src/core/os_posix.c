// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include <ccord/core/os.h>

#include <ccord/core/alloc.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#if CCORD_OS_ANDROID
#include <android/api-level.h>
#endif

const char *ccord_os_name(void)
{
#if CCORD_OS_MACOS
  return "macos";
#elif CCORD_OS_ANDROID
  return "android";
#else
  return "linux";
#endif
}

const char *ccord_os_arch(void)
{
#if defined(__aarch64__) || defined(__arm64__)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__arm__)
  return "arm";
#elif defined(__i386__)
  return "x86";
#else
  static _Atomic bool inited = false;
  static char buf[64];
  if (!atomic_load_explicit(&inited, memory_order_acquire)) {
    struct utsname u;
    if (uname(&u) == 0 && u.machine[0]) snprintf(buf, sizeof buf, "%s", u.machine);
    else snprintf(buf, sizeof buf, "unknown");
    atomic_store_explicit(&inited, true, memory_order_release);
  }
  return buf;
#endif
}

bool ccord_os_is_mobile(void)
{
#if CCORD_OS_MOBILE
  return true;
#else
  return false;
#endif
}

int ccord_os_android_api_level(void)
{
#if CCORD_OS_ANDROID
  int lvl = android_get_device_api_level();
  if (lvl > 0) return lvl;
  const char *env = getenv("CCORD_ANDROID_API_LEVEL");
  if (env && env[0]) {
    int parsed = atoi(env);
    if (parsed > 0) return parsed;
  }
  return __ANDROID_API__;
#else
  return 0;
#endif
}

uint64_t ccord_os_pid(void)
{
  return (uint64_t)getpid();
}

bool ccord_os_hostname(char *out, size_t cap)
{
  if (!out || cap == 0) return false;
  if (gethostname(out, cap - 1) != 0) {
    out[0] = '\0';
    return false;
  }
  out[cap - 1] = '\0';
  return true;
}

bool ccord_os_getcwd(char *out, size_t cap)
{
  return out && cap > 0 && getcwd(out, cap) != nullptr;
}

bool ccord_os_config_dir(char *out, size_t cap)
{
  if (!out || cap == 0) return false;
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0] != '\0') {
    size_t xn = strlen(xdg);
    while (xn > 1 && xdg[xn - 1] == '/') xn--;
    int n = snprintf(out, cap, "%.*s/ccord", (int)xn, xdg);
    return n > 0 && (size_t)n < cap;
  }
#if CCORD_OS_ANDROID
  const char *adata = getenv("CCORD_ANDROID_DATA_DIR");
  if (adata && adata[0] != '\0') {
    size_t an = strlen(adata);
    while (an > 1 && adata[an - 1] == '/') an--;
    int n = snprintf(out, cap, "%.*s/config", (int)an, adata);
    return n > 0 && (size_t)n < cap;
  }
#endif
  const char *home = getenv("HOME");
  if (!home || home[0] == '\0') return false;
  size_t hn = strlen(home);
  while (hn > 1 && home[hn - 1] == '/') hn--;
#if CCORD_OS_MACOS
  int n = snprintf(out, cap, "%.*s/Library/Application Support/C-Cord", (int)hn, home);
#elif CCORD_OS_ANDROID
  int n = snprintf(out, cap, "%.*s/config", (int)hn, home);
#else
  int n = snprintf(out, cap, "%.*s/.config/ccord", (int)hn, home);
#endif
  return n > 0 && (size_t)n < cap;
}

bool ccord_os_runtime_dir(char *out, size_t cap)
{
  if (!out || cap == 0) return false;
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  if (xdg && xdg[0] != '\0') {
    size_t xn = strlen(xdg);
    while (xn > 1 && xdg[xn - 1] == '/') xn--;
    int n = snprintf(out, cap, "%.*s/ccord", (int)xn, xdg);
    return n > 0 && (size_t)n < cap;
  }
  const char *tmp = getenv("TMPDIR");
  if (tmp && tmp[0] != '\0') {
    size_t tl = strlen(tmp);
    while (tl > 1 && tmp[tl - 1] == '/') tl--;
    int n = snprintf(out, cap, "%.*s/ccord-%lu", (int)tl, tmp, (unsigned long)getuid());
    return n > 0 && (size_t)n < cap;
  }
#if CCORD_OS_ANDROID
  const char *adata = getenv("CCORD_ANDROID_DATA_DIR");
  if (adata && adata[0] != '\0') {
    size_t an = strlen(adata);
    while (an > 1 && adata[an - 1] == '/') an--;
    int n = snprintf(out, cap, "%.*s/run", (int)an, adata);
    return n > 0 && (size_t)n < cap;
  }
  return false;
#else
  int n = snprintf(out, cap, "/tmp/ccord-%lu", (unsigned long)getuid());
  return n > 0 && (size_t)n < cap;
#endif
}

void ccord_sleep_ms(uint32_t ms)
{
  struct timespec ts = {.tv_sec = (time_t)(ms / 1000u), .tv_nsec = (long)(ms % 1000u) * 1000000L};
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

void ccord_sleep_us(uint32_t us)
{
  struct timespec ts = {.tv_sec = (time_t)(us / 1000000u), .tv_nsec = (long)(us % 1000000u) * 1000L};
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

bool ccord_gmtime(time_t t, struct tm *out)
{
  return out && gmtime_r(&t, out) != nullptr;
}

bool ccord_localtime(time_t t, struct tm *out)
{
  return out && localtime_r(&t, out) != nullptr;
}

time_t ccord_timegm(struct tm *tm)
{
  return tm ? timegm(tm) : (time_t)-1;
}

void ccord_tz_init(void)
{
  tzset();
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

// Threads, mutexes, monotonic condvars ------------------------------------------------------------
struct ccord_mutex {
  pthread_mutex_t m;
};

struct ccord_cond {
  pthread_cond_t c;
};

struct ccord_thread {
  pthread_t t;
  ccord_thread_fn fn;
  void *arg;
};

ccord_mutex *ccord_mutex_create(void)
{
  ccord_mutex *m = ccord_calloc(1, sizeof *m);
  if (!m) return nullptr;
  if (pthread_mutex_init(&m->m, nullptr) != 0) {
    ccord_free(m);
    return nullptr;
  }
  return m;
}

void ccord_mutex_destroy(ccord_mutex *m)
{
  if (!m) return;
  pthread_mutex_destroy(&m->m);
  ccord_free(m);
}

void ccord_mutex_lock(ccord_mutex *m)
{
  if (m) pthread_mutex_lock(&m->m);
}

void ccord_mutex_unlock(ccord_mutex *m)
{
  if (m) pthread_mutex_unlock(&m->m);
}

ccord_cond *ccord_cond_create(void)
{
  ccord_cond *c = ccord_calloc(1, sizeof *c);
  if (!c) return nullptr;
#if !CCORD_OS_MACOS
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
  int rc = pthread_cond_init(&c->c, &ca);
  pthread_condattr_destroy(&ca);
#else
  int rc = pthread_cond_init(&c->c, nullptr);
#endif
  if (rc != 0) {
    ccord_free(c);
    return nullptr;
  }
  return c;
}

void ccord_cond_destroy(ccord_cond *c)
{
  if (!c) return;
  pthread_cond_destroy(&c->c);
  ccord_free(c);
}

void ccord_cond_signal(ccord_cond *c)
{
  if (c) pthread_cond_signal(&c->c);
}

void ccord_cond_broadcast(ccord_cond *c)
{
  if (c) pthread_cond_broadcast(&c->c);
}

void ccord_cond_wait(ccord_cond *c, ccord_mutex *m)
{
  if (c && m) pthread_cond_wait(&c->c, &m->m);
}

bool ccord_cond_wait_timeout(ccord_cond *c, ccord_mutex *m, uint32_t timeout_ms)
{
  if (!c || !m) return false;
#if CCORD_OS_MACOS
  struct timespec rel = {
    .tv_sec = (time_t)(timeout_ms / 1000u),
    .tv_nsec = (long)(timeout_ms % 1000u) * 1000000L,
  };
  return pthread_cond_timedwait_relative_np(&c->c, &m->m, &rel) != ETIMEDOUT;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  ts.tv_sec += (time_t)(timeout_ms / 1000u);
  ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }
  return pthread_cond_timedwait(&c->c, &m->m, &ts) != ETIMEDOUT;
#endif
}

static void *thread_trampoline(void *p)
{
  ccord_thread *t = p;
  t->fn(t->arg);
  return nullptr;
}

ccord_thread *ccord_thread_create(ccord_thread_fn fn, void *arg)
{
  if (!fn) return nullptr;
  ccord_thread *t = ccord_calloc(1, sizeof *t);
  if (!t) return nullptr;
  t->fn = fn;
  t->arg = arg;
  if (pthread_create(&t->t, nullptr, thread_trampoline, t) != 0) {
    ccord_free(t);
    return nullptr;
  }
  return t;
}

void ccord_thread_join(ccord_thread *t)
{
  if (!t) return;
  pthread_join(t->t, nullptr);
  ccord_free(t);
}

void ccord_os_trim_memory(void)
{
#if defined(__GLIBC__)
  (void)malloc_trim(0);
#endif
}

void ccord_spin_lock_acquire(ccord_spin_lock *l)
{
  if (!l) return;
  while (atomic_exchange_explicit(&l->state, 1u, memory_order_acquire) != 0u) {
    sched_yield();
  }
}

void ccord_spin_lock_release(ccord_spin_lock *l)
{
  if (l) atomic_store_explicit(&l->state, 0u, memory_order_release);
}

// Directory & filesystem helpers ------------------------------------------------------------------
struct ccord_dir {
  DIR *d;
  char base[CCORD_PATH_MAX];
};

ccord_dir *ccord_dir_open(const char *path)
{
  if (!path || !path[0]) return nullptr;
  DIR *d = opendir(path);
  if (!d) return nullptr;
  ccord_dir *cd = ccord_calloc(1, sizeof *cd);
  if (!cd) {
    closedir(d);
    return nullptr;
  }
  cd->d = d;
  snprintf(cd->base, sizeof cd->base, "%s", path);
  return cd;
}

bool ccord_dir_next(ccord_dir *d, char *out, size_t cap, bool *is_dir)
{
  if (!d || !d->d || !out || cap == 0) return false;
  struct dirent *e;
  while ((e = readdir(d->d)) != nullptr) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    snprintf(out, cap, "%s", e->d_name);
    if (is_dir) {
      char full[CCORD_PATH_MAX];
      struct stat st;
      int n = snprintf(full, sizeof full, "%s/%s", d->base, e->d_name);
      *is_dir = (n > 0 && (size_t)n < sizeof full && stat(full, &st) == 0 && S_ISDIR(st.st_mode));
    }
    return true;
  }
  return false;
}

void ccord_dir_close(ccord_dir *d)
{
  if (!d) return;
  if (d->d) closedir(d->d);
  ccord_free(d);
}

bool ccord_path_exists(const char *path)
{
  struct stat st;
  return path && stat(path, &st) == 0;
}

bool ccord_path_is_dir(const char *path)
{
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool ccord_path_is_file(const char *path)
{
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool ccord_path_is_symlink(const char *path)
{
  struct stat st;
  return path && lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

bool ccord_file_size(const char *path, uint64_t *out_size)
{
  struct stat st;
  if (!path || stat(path, &st) != 0 || st.st_size < 0) return false;
  if (out_size) *out_size = (uint64_t)st.st_size;
  return true;
}

bool ccord_file_mtime_ns(const char *path, int64_t *out_ns)
{
  struct stat st;
  if (!path || stat(path, &st) != 0) return false;
#if CCORD_OS_MACOS
  int64_t ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + (int64_t)st.st_mtimespec.tv_nsec;
#else
  int64_t ns = (int64_t)st.st_mtim.tv_sec * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
#endif
  if (out_ns) *out_ns = ns;
  return true;
}

bool ccord_file_unlink(const char *path)
{
  return path && unlink(path) == 0;
}

bool ccord_file_rename_replace(const char *src, const char *dst)
{
  return src && dst && rename(src, dst) == 0;
}

// Sockets & poll ----------------------------------------------------------------------------------
void ccord_net_os_init(void) {}

void ccord_sock_close(ccord_socket_t s)
{
  if (s >= 0) close(s);
}

bool ccord_sock_set_nonblock(ccord_socket_t s)
{
  if (s < 0) return false;
  int fl = fcntl(s, F_GETFL, 0);
  return fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
}

bool ccord_sock_set_cloexec(ccord_socket_t s)
{
  if (s < 0) return false;
  int fl = fcntl(s, F_GETFD, 0);
  return fl >= 0 && fcntl(s, F_SETFD, fl | FD_CLOEXEC) == 0;
}

void ccord_sock_set_nosigpipe(ccord_socket_t s)
{
#ifdef SO_NOSIGPIPE
  int one = 1;
  (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)s;
#endif
}

ptrdiff_t ccord_sock_send(ccord_socket_t s, const void *buf, size_t len, bool *would_block)
{
  if (would_block) *would_block = false;
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
#ifdef MSG_DONTWAIT
  flags |= MSG_DONTWAIT;
#endif
  ssize_t n = send(s, buf, len, flags);
  if (n < 0) {
    if (errno == EAGAIN || errno == EINTR
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        || errno == EWOULDBLOCK
#endif
    ) {
      if (would_block) *would_block = true;
    }
    return -1;
  }
  return (ptrdiff_t)n;
}

ptrdiff_t ccord_sock_recv(ccord_socket_t s, void *buf, size_t len, bool *would_block)
{
  if (would_block) *would_block = false;
  ssize_t n = recv(s, buf, len, 0);
  if (n < 0) {
    if (errno == EAGAIN || errno == EINTR
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        || errno == EWOULDBLOCK
#endif
    ) {
      if (would_block) *would_block = true;
    }
    return -1;
  }
  return (ptrdiff_t)n;
}

bool ccord_sock_pair(ccord_socket_t out[2])
{
  if (!out) return false;
  int sv[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return false;
  (void)ccord_sock_set_cloexec(sv[0]);
  (void)ccord_sock_set_cloexec(sv[1]);
  ccord_sock_set_nosigpipe(sv[0]);
  ccord_sock_set_nosigpipe(sv[1]);
  out[0] = sv[0];
  out[1] = sv[1];
  return true;
}

int ccord_sock_poll1(ccord_socket_t s, unsigned events, int timeout_ms)
{
  if (s < 0) return -1;
  struct pollfd pfd = {.fd = s, .events = 0};
  if (events & CCORD_POLL_IN) pfd.events |= POLLIN;
  if (events & CCORD_POLL_OUT) pfd.events |= POLLOUT;
  int rc = poll(&pfd, 1, timeout_ms);
  if (rc < 0) return (errno == EINTR) ? 0 : -1;
  if (rc == 0) return 0;
  int out = 0;
  if (pfd.revents & POLLIN) out |= CCORD_POLL_IN;
  if (pfd.revents & POLLOUT) out |= CCORD_POLL_OUT;
  if (pfd.revents & (POLLERR | POLLNVAL)) out |= CCORD_POLL_ERR;
  if (pfd.revents & POLLHUP) out |= CCORD_POLL_HUP;
  return out;
}
