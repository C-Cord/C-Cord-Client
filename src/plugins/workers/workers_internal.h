// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include "plugin_workers.h"

#include <ccord/core/alloc.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>
#include <ccord/core/random.h>
#include <ccord/core/sha256.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if !CCORD_OS_WINDOWS
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/inotify.h>
#endif
#endif

#define PW_MAX_EVENTS 512u
#define PW_MAX_WATCHES 64u
#define PW_MAX_WS 32u
#define PW_MAX_PROCS 32u
#define PW_MAX_HTTP_BODY (4u * 1024u * 1024u)

typedef struct pw_watch {
  bool active;
  char owner[65];
  uint64_t watch_id;
  char path[CCORD_PATH_MAX];
#if defined(__linux__) && !CCORD_OS_WINDOWS
  int wd;
#endif
  int64_t last_mtime_ns;
  int64_t last_size;
  bool existed;
} pw_watch;

typedef struct pw_ws_slot {
  bool active;
  _Atomic bool stop;
  char owner[65];
  uint64_t ws_id;
  pthread_t thr;
  bool joined;
  pthread_mutex_t send_mu;
  int tcp_fd;            // >=0 for built-in loopback ws://
  void *ext_handle;      // non-null when using ops.ws_*
  char url[1024];
  char *headers_json;
  plugin_workers *parent;
} pw_ws_slot;

typedef struct pw_proc_slot {
  bool active;
  _Atomic bool stop;
  char owner[65];
  uint64_t proc_id;
  pthread_t thr;
  bool joined;
#if !CCORD_OS_WINDOWS
  pid_t pid;
  int stdin_fd;
  int stdout_fd;
  int stderr_fd;
#endif
  pthread_mutex_t io_mu;
  plugin_workers *parent;
} pw_proc_slot;

struct plugin_workers {
  ccord_plugin_platform_ops ops;
  pthread_mutex_t ev_mu;
  pw_event *events;
  size_t nevents, capevents;
  _Atomic bool shutting_down;
  _Atomic int active_jobs;
  // fs.watch
#if defined(__linux__) && !CCORD_OS_WINDOWS
  int inotify_fd;
#endif
  pw_watch *watches[PW_MAX_WATCHES];
  uint64_t last_watch_poll_ms;
  // ws & spawned procs
  pthread_mutex_t slot_mu;
  pw_ws_slot *ws[PW_MAX_WS];
  pw_proc_slot *procs[PW_MAX_PROCS];
};


#include <ccord/core/base64.h>

void pw_push_ev(plugin_workers *w, const pw_event *ev);
void pw_b64_encode(const uint8_t *in, size_t n, ccord_buf *out);
bool pw_extract_url_host_port(const char *url, char *scheme_out, size_t scap, char *host_out, size_t hcap,
                              int *port_out, char *path_out, size_t pcap);
bool pw_is_localhost_host(const char *host);
#if !CCORD_OS_WINDOWS
bool pw_ws_send_tcp_frame(int fd, uint8_t opcode, const uint8_t *data, size_t len);
#endif
#if defined(__linux__) && !CCORD_OS_WINDOWS
static inline void pw_inotify_rm_watch(int fd, int wd)
{
  if (fd >= 0 && wd >= 0) {
#if defined(__ANDROID__)
    (void)inotify_rm_watch(fd, (uint32_t)wd);
#else
    (void)inotify_rm_watch(fd, wd);
#endif
  }
}
#endif
void pw_poll_watches(plugin_workers *w);
