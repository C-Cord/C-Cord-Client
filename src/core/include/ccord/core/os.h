// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <ccord/core/error.h>

// Platform identification -------------------------------------------------------------------------
#if defined(_WIN32)
#define CCORD_OS_WINDOWS 1
#define CCORD_OS_MACOS   0
#define CCORD_OS_LINUX   0
#elif defined(__APPLE__)
#define CCORD_OS_WINDOWS 0
#define CCORD_OS_MACOS   1
#define CCORD_OS_LINUX   0
#else
#define CCORD_OS_WINDOWS 0
#define CCORD_OS_MACOS   0
#define CCORD_OS_LINUX   1
#endif

#ifndef CCORD_PATH_MAX
#define CCORD_PATH_MAX 4096
#endif

const char *ccord_os_name(void);   // "linux", "macos", or "windows"
const char *ccord_os_arch(void);   // "x86_64", "arm64", etc.
uint64_t ccord_os_pid(void);
bool ccord_os_hostname(char *out, size_t cap);
bool ccord_os_getcwd(char *out, size_t cap);

// Per-user config directory (e.g. ~/.config/ccord, ~/Library/Application Support/C-Cord, %APPDATA%/C-Cord)
bool ccord_os_config_dir(char *out, size_t cap);
// Per-user runtime/temp directory for local IPC sockets / scratch files.
bool ccord_os_runtime_dir(char *out, size_t cap);

// Sleep & portable time/string helpers ------------------------------------------------------------
void ccord_sleep_ms(uint32_t ms);
void ccord_sleep_us(uint32_t us);
bool ccord_gmtime(time_t t, struct tm *out);
bool ccord_localtime(time_t t, struct tm *out);
int ccord_strcasecmp(const char *a, const char *b);
int ccord_strncasecmp(const char *a, const char *b, size_t n);
#if CCORD_OS_WINDOWS
#ifndef strcasecmp
#define strcasecmp ccord_strcasecmp
#endif
#ifndef strncasecmp
#define strncasecmp ccord_strncasecmp
#endif
#endif

// Portable threads, mutexes, and monotonic condition variables ------------------------------------
typedef struct ccord_mutex ccord_mutex;
typedef struct ccord_cond ccord_cond;
typedef struct ccord_thread ccord_thread;

typedef void (*ccord_thread_fn)(void *arg);

ccord_mutex *ccord_mutex_create(void);
void ccord_mutex_destroy(ccord_mutex *m);
void ccord_mutex_lock(ccord_mutex *m);
void ccord_mutex_unlock(ccord_mutex *m);

ccord_cond *ccord_cond_create(void);
void ccord_cond_destroy(ccord_cond *c);
void ccord_cond_signal(ccord_cond *c);
void ccord_cond_broadcast(ccord_cond *c);
void ccord_cond_wait(ccord_cond *c, ccord_mutex *m);
// Waits up to timeout_ms (monotonic). Returns true if signaled, false on timeout.
bool ccord_cond_wait_timeout(ccord_cond *c, ccord_mutex *m, uint32_t timeout_ms);

ccord_thread *ccord_thread_create(ccord_thread_fn fn, void *arg);
void ccord_thread_join(ccord_thread *t);

// Zero-init static lock (safe for file-scope globals without init functions)
typedef struct ccord_spin_lock {
  _Atomic uint32_t state;
} ccord_spin_lock;
#define CCORD_SPIN_LOCK_INIT {0}
void ccord_spin_lock_acquire(ccord_spin_lock *l);
void ccord_spin_lock_release(ccord_spin_lock *l);

// Portable filesystem & directory iteration -------------------------------------------------------
typedef struct ccord_dir ccord_dir;

ccord_dir *ccord_dir_open(const char *path);
// Copies next entry name (excluding "." and "..") into `out`. Returns false at end.
bool ccord_dir_next(ccord_dir *d, char *out, size_t cap, bool *is_dir);
void ccord_dir_close(ccord_dir *d);

bool ccord_path_exists(const char *path);
bool ccord_path_is_dir(const char *path);
bool ccord_path_is_file(const char *path);
bool ccord_path_is_symlink(const char *path);
bool ccord_file_size(const char *path, uint64_t *out_size);
bool ccord_file_mtime_ns(const char *path, int64_t *out_ns);
bool ccord_file_unlink(const char *path);
bool ccord_file_rename_replace(const char *src, const char *dst);

// Portable sockets & poll -------------------------------------------------------------------------
#if CCORD_OS_WINDOWS
typedef uintptr_t ccord_socket_t;
#define CCORD_INVALID_SOCKET ((ccord_socket_t)~(uintptr_t)0)
#else
typedef int ccord_socket_t;
#define CCORD_INVALID_SOCKET (-1)
#endif

enum {
  CCORD_POLL_IN  = 1u << 0,
  CCORD_POLL_OUT = 1u << 1,
  CCORD_POLL_ERR = 1u << 2,
  CCORD_POLL_HUP = 1u << 3,
};

void ccord_net_os_init(void);
void ccord_sock_close(ccord_socket_t s);
bool ccord_sock_set_nonblock(ccord_socket_t s);
bool ccord_sock_set_cloexec(ccord_socket_t s);
void ccord_sock_set_nosigpipe(ccord_socket_t s);
// Non-blocking send/recv without triggering SIGPIPE. Returns bytes transferred, 0 on EOF (recv),
// or -1 on error (sets *would_block when retryable).
ptrdiff_t ccord_sock_send(ccord_socket_t s, const void *buf, size_t len, bool *would_block);
ptrdiff_t ccord_sock_recv(ccord_socket_t s, void *buf, size_t len, bool *would_block);
// Creates a connected stream socket pair (AF_UNIX on POSIX, loopback TCP on Windows). Both ends CLOEXEC + NOSIGPIPE.
bool ccord_sock_pair(ccord_socket_t out[2]);
// Polls a single socket up to timeout_ms (-1 = infinite). Returns CCORD_POLL_* mask, or -1 on error.
int ccord_sock_poll1(ccord_socket_t s, unsigned events, int timeout_ms);
