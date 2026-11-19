// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
/* M5 audio device plane: enumeration, device-id parsing and the hot-plug registry.
 *
 * Enumeration borrows SDL_INIT_AUDIO when nothing else has initialized it (and
 * returns it afterwards), so settings pages can list devices before any stream
 * exists. Under the dummy driver SDL reports one synthetic device per direction;
 * that is SDL's honest answer, not a fabricated device.
 *
 * Hot-plug: SDL emits SDL_EVENT_AUDIO_DEVICE_ADDED/REMOVED for both the physical
 * device id and every bound logical stream id. ccord_platform_next_event hands
 * those events here on the main thread; a REMOVED event whose id matches a live
 * handle (logical or opened id) reopens that handle on the system default, then
 * the user callback fires. Streams opened with the DEFAULT alias on the current
 * default device are auto-migrated by SDL itself and need no reopen. */
#define _POSIX_C_SOURCE 200809L
#include "devices_internal.h"

#include <SDL3/SDL.h>
#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>
#include <ccord/platform/capture.h>
#include <ccord/platform/platform.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include "loopback_pw.h"
#elif defined(_WIN32)
typedef struct ccord_loopback_win32 ccord_loopback_win32;
ccord_loopback_win32 *ccord_loopback_win32_open(void);
size_t ccord_loopback_win32_read(ccord_loopback_win32 *w, int16_t *pcm, size_t max_samples);
bool ccord_loopback_win32_failed(const ccord_loopback_win32 *w);
void ccord_loopback_win32_close(ccord_loopback_win32 *w);
#endif

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
#endif

bool ccord_devices_parse_id(const char *s, uint32_t *out)
{
  if (!s || !out || *s == '\0') return false;
  uint64_t v = 0;
  for (const char *p = s; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10 + (uint64_t)(*p - '0');
    if (v > UINT32_MAX) return false;
  }
  if (v == 0) return false;
  *out = (uint32_t)v;
  return true;
}

SDL_AudioDeviceID ccord_audio_parse_device_id(bool capture, const char *id)
{
  SDL_AudioDeviceID def = capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  uint32_t v = 0;
  if (!ccord_devices_parse_id(id, &v)) return def;
  return (SDL_AudioDeviceID)v;
}

void ccord_devices_copy_utf8(char *dst, size_t cap, const char *src)
{
  if (cap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t len = strlen(src);
  size_t n = len < cap - 1 ? len : cap - 1;
  while (n > 0 && !ccord_utf8_valid(src, n)) n--;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

size_t ccord_audio_devices_list(bool capture, ccord_audio_device_info *out, size_t max)
{
  if (!out || max == 0) return 0;
  bool audio_init = false;
  if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return 0;
    audio_init = true;
  }
  int count = 0;
  SDL_AudioDeviceID *ids = capture ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);
  // SDL 3.4 has no public "default device id" query; compare names against the
  // default alias instead (dummy driver: one device per direction, the default).
  const char *def_name = SDL_GetAudioDeviceName(capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
                                                        : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
  size_t n = 0;
  if (ids && count > 0) {
    for (int i = 0; i < count && n < max; ++i) {
      snprintf(out[n].id, sizeof out[n].id, "%u", (unsigned)ids[i]);
      const char *name = SDL_GetAudioDeviceName(ids[i]);
      ccord_devices_copy_utf8(out[n].name, sizeof out[n].name, name ? name : "Audio device");
      out[n].is_default = def_name && name && strcmp(def_name, name) == 0;
      ++n;
    }
  }
  SDL_free(ids);
  if (audio_init) SDL_QuitSubSystem(SDL_INIT_AUDIO);
  return n;
}

// ---- hot-plug callback + registry of live handles (main thread only) ----

static ccord_audio_hotplug_fn s_hotplug_fn;
static void *s_hotplug_ud;
static ccord_audio *s_registry;
static SDL_Mutex *s_registry_mutex;

void ccord_audio_set_hotplug_callback(ccord_audio_hotplug_fn fn, void *ud)
{
  s_hotplug_fn = fn;
  s_hotplug_ud = ud;
}

static void registry_lock(void)
{
  if (!s_registry_mutex) s_registry_mutex = SDL_CreateMutex();
  if (s_registry_mutex) SDL_LockMutex(s_registry_mutex);
}

static void registry_unlock(void)
{
  if (s_registry_mutex) SDL_UnlockMutex(s_registry_mutex);
}

void ccord_audio_devices_register(ccord_audio *a)
{
  if (!a) return;
  registry_lock();
  a->hotplug_next = s_registry;
  s_registry = a;
  registry_unlock();
}

void ccord_audio_devices_unregister(ccord_audio *a)
{
  if (!a) return;
  registry_lock();
  ccord_audio **link = &s_registry;
  while (*link && *link != a) link = &(*link)->hotplug_next;
  if (*link) *link = a->hotplug_next;
  a->hotplug_next = nullptr;
  registry_unlock();
}

// Reopen a handle whose device went away on the system default device. The old
// logical id dies with the stream; SDL assigns a fresh one on the new open.
static void reopen_on_default(ccord_audio *a)
{
  SDL_DestroyAudioStream(a->stream);
  a->stream = nullptr;
  a->device = 0;
  SDL_AudioDeviceID def = a->capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  a->opened_id = def;
  a->stream = SDL_OpenAudioDeviceStream(def, &a->spec, nullptr, nullptr);
  if (!a->stream) {
    CCORD_LOG_D("audio: hot-plug reopen failed: %s", SDL_GetError());
    return;
  }
  a->device = SDL_GetAudioStreamDevice(a->stream);
  if (!SDL_ResumeAudioStreamDevice(a->stream)) {
    CCORD_LOG_D("audio: hot-plug resume failed: %s", SDL_GetError());
    SDL_DestroyAudioStream(a->stream);
    a->stream = nullptr;
    a->device = 0;
  }
}

bool ccord_audio_devices_handle_event(const SDL_Event *e)
{
  if (!e) return false;
  if (e->type != SDL_EVENT_AUDIO_DEVICE_ADDED && e->type != SDL_EVENT_AUDIO_DEVICE_REMOVED) return false;
  bool added = e->type == SDL_EVENT_AUDIO_DEVICE_ADDED;
  bool capture = e->adevice.recording;
  SDL_AudioDeviceID which = e->adevice.which;
  if (!added && s_registry_mutex) {
    registry_lock();
    for (ccord_audio *a = s_registry; a; a = a->hotplug_next) {
      if (!a->stream || a->capture != capture) continue;
      // SDL reports removals for both the logical stream id and the physical id.
      if (a->device != which && a->opened_id != which) continue;
      reopen_on_default(a);
    }
    registry_unlock();
  }
  if (s_hotplug_fn) s_hotplug_fn(s_hotplug_ud, capture, added);
  return true;
}

// ---- desktop / monitor loopback audio (Go Live "share audio") --------------------------------
// See ccord/platform/capture.h. Fake mode: a deterministic 440 Hz tone (48 kHz mono S16LE, one
// 960-sample frame per read). Real mode (Linux): parec / pw-record reading a PulseAudio/PipeWire
// sink monitor, stdout drained through a non-blocking pipe. An unavailable monitor fails soft at
// open() so callers can fall back and retry; nothing here ever blocks the caller for long.

typedef enum lb_kind : uint8_t { LB_KIND_FAKE = 0, LB_KIND_PIPE, LB_KIND_SDL, LB_KIND_PW, LB_KIND_WIN32 } lb_kind;

struct ccord_loopback_audio {
  lb_kind kind;
  bool failed;
  uint64_t phase;      // fake: samples generated so far (tone phase, deterministic per open)
  int fd;              // pipe: read end (O_NONBLOCK), -1 when unused
  long child;          // pipe: capture child pid, -1 when none
  bool reaped;         // pipe: child already waitpid()ed
  uint8_t pending;     // pipe: odd byte carried between reads (S16LE sample split across reads)
  bool have_pending;
  SDL_AudioStream *sdl_stream;   // SDL monitor fallback stream when helpers are absent
  bool sdl_audio_init;
#if defined(__linux__) && !defined(__ANDROID__)
  ccord_loopback_pw *pw;         // PipeWire self-excluding capture when available
#elif defined(_WIN32)
  ccord_loopback_win32 *win32;   // Windows WASAPI loopback capture
#endif
};

static atomic_bool g_lb_fake;

void ccord_loopback_audio_set_fake(bool on) { atomic_store(&g_lb_fake, on); }

bool ccord_loopback_audio_fake_enabled(void)
{
  if (atomic_load(&g_lb_fake)) return true;
  const char *env = getenv("CCORD_CAPTURE_BACKEND_FAKE");
  if (env && env[0] != '\0' && strcmp(env, "0") != 0) return true;
  return ccord_capture_fake_enabled();
}

#define LB_FAKE_FREQ 440.0
#define LB_FAKE_AMP 8192.0
static const double k_lb_two_pi = 6.28318530717958647692;   // M_PI is not standard C

static size_t lb_fake_read(ccord_loopback_audio *l, int16_t *pcm, size_t max_samples)
{
  size_t n = max_samples < CCORD_LOOPBACK_FRAME_SAMPLES ? max_samples : CCORD_LOOPBACK_FRAME_SAMPLES;
  for (size_t i = 0; i < n; i++) {
    double t = (double)(l->phase + i) / (double)CCORD_LOOPBACK_RATE;
    pcm[i] = (int16_t)(LB_FAKE_AMP * sin(k_lb_two_pi * LB_FAKE_FREQ * t));
  }
  l->phase += n;
  return n;
}

#if defined(__linux__) && !defined(__ANDROID__)

// Mutable argv builder: -Wwrite-strings forbids handing string literals to exec's char *argv[].
typedef struct lb_argv {
  char buf[640];
  size_t used;
  char *argv[12];
  size_t argc;
} lb_argv;

static bool lb_argv_add(lb_argv *a, const char *s)
{
  size_t len = strlen(s) + 1u;
  if (a->argc + 1u >= sizeof a->argv / sizeof a->argv[0] || a->used + len > sizeof a->buf) return false;
  char *dst = a->buf + a->used;
  memcpy(dst, s, len);
  a->used += len;
  a->argv[a->argc++] = dst;
  a->argv[a->argc] = nullptr;
  return true;
}

// Spawns a capture helper with stdout on a non-blocking pipe and stderr silenced. Returns the
// child pid (>= 0) and sets *out_fd, or -1 on failure. execvp resets any inherited signal handlers
// (e.g. SDL's SIGTERM handler), so the child dies normally on SIGTERM.
static long lb_spawn(char *const argv[], int *out_fd)
{
  int p[2];
  if (pipe(p) != 0) return -1;
  posix_spawn_file_actions_t fa;
  if (posix_spawn_file_actions_init(&fa) != 0) {
    close(p[0]);
    close(p[1]);
    return -1;
  }
  (void)posix_spawn_file_actions_addclose(&fa, p[0]);
  (void)posix_spawn_file_actions_adddup2(&fa, p[1], STDOUT_FILENO);
  (void)posix_spawn_file_actions_addclose(&fa, p[1]);
  (void)posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawnattr_t sa;
  bool have_sa = posix_spawnattr_init(&sa) == 0;
  if (have_sa) {
    sigset_t def;
    sigemptyset(&def);
    sigaddset(&def, SIGTERM);
    sigaddset(&def, SIGPIPE);
    (void)posix_spawnattr_setsigdefault(&sa, &def);
    (void)posix_spawnattr_setflags(&sa, POSIX_SPAWN_SETSIGDEF);
  }
  pid_t pid = 0;
  int rc = posix_spawnp(&pid, argv[0], &fa, have_sa ? &sa : nullptr, argv, environ);
  if (have_sa) (void)posix_spawnattr_destroy(&sa);
  (void)posix_spawn_file_actions_destroy(&fa);
  close(p[1]);
  if (rc != 0) {
    close(p[0]);
    return -1;
  }
  int fd_fl = fcntl(p[0], F_GETFD);
  if (fd_fl >= 0) (void)fcntl(p[0], F_SETFD, fd_fl | FD_CLOEXEC);
  int fl = fcntl(p[0], F_GETFL);
  if (fl >= 0) (void)fcntl(p[0], F_SETFL, fl | O_NONBLOCK);
  *out_fd = p[0];
  return (long)pid;
}

// Terminates (if needed) and reaps a capture child, then closes the pipe. Bounded: SIGTERM with a
// ~100 ms grace period, then SIGKILL.
static void lb_stop(int fd, long child, bool reaped)
{
  if (fd >= 0) close(fd);
  if (child < 0 || reaped) return;
  (void)kill((pid_t)child, SIGTERM);
  int st = 0;
  for (int i = 0; i < 20; i++) {
    if (waitpid((pid_t)child, &st, WNOHANG) == (pid_t)child) return;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 5000000};   // 5 ms
    (void)nanosleep(&ts, nullptr);
  }
  (void)kill((pid_t)child, SIGKILL);
  (void)waitpid((pid_t)child, &st, 0);
}

// Waits briefly for the first bytes (or the child's death). A live child still silent after the
// timeout is accepted: a monitor of an idle sink streams silence, and a helper that cannot reach
// the audio server exits quickly instead.
static bool lb_probe(int fd, long child, bool *reaped)
{
  struct pollfd pf = {.fd = fd, .events = POLLIN};
  int pr = poll(&pf, 1, 150);
  if (pr > 0 && (pf.revents & POLLIN) != 0) return true;
  int st = 0;
  if (waitpid((pid_t)child, &st, WNOHANG) == (pid_t)child) {
    *reaped = true;
    return false;
  }
  return pr == 0;
}

// Finds a sink monitor source name via `pactl list short sources` (works against PulseAudio and
// pipewire-pulse). Prefers a RUNNING monitor; falls back to the first monitor listed.
static bool lb_discover_monitor(char *out, size_t cap)
{
  lb_argv av;
  memset(&av, 0, sizeof av);
  (void)lb_argv_add(&av, "pactl");
  (void)lb_argv_add(&av, "list");
  (void)lb_argv_add(&av, "short");
  (void)lb_argv_add(&av, "sources");
  int fd = -1;
  long child = lb_spawn(av.argv, &fd);
  if (child < 0) return false;
  char buf[8192];
  size_t len = 0;
  for (int i = 0; i < 30; i++) {   // <= ~300 ms total
    struct pollfd pf = {.fd = fd, .events = POLLIN};
    int pr = poll(&pf, 1, 10);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;
    if ((pf.revents & POLLIN) == 0) break;   // HUP/ERR: child gone
    ssize_t r = read(fd, buf + len, sizeof buf - len);
    if (r <= 0) break;
    len += (size_t)r;
    if (len >= sizeof buf) break;
  }
  buf[len >= sizeof buf ? sizeof buf - 1 : len] = '\0';
  lb_stop(fd, child, false);
  char best[256] = "";
  bool best_running = false;
  char *save = nullptr;
  for (char *line = strtok_r(buf, "\n", &save); line != nullptr; line = strtok_r(nullptr, "\n", &save)) {
    char *lsave = nullptr;
    (void)strtok_r(line, "\t", &lsave);            // source index
    char *name = strtok_r(nullptr, "\t", &lsave);  // source name
    if (!name) continue;
    size_t nlen = strlen(name);
    if (nlen < 8 || strcmp(name + nlen - 8, ".monitor") != 0) continue;
    char *state = nullptr;
    for (char *f = strtok_r(nullptr, "\t", &lsave); f != nullptr; f = strtok_r(nullptr, "\t", &lsave)) state = f;
    bool running = state && strcmp(state, "RUNNING") == 0;
    if (best[0] == '\0' || (running && !best_running)) {
      snprintf(best, sizeof best, "%s", name);
      best_running = running;
      if (running) break;
    }
  }
  if (best[0] == '\0') return false;
  snprintf(out, cap, "%s", best);
  return true;
}

// Tries the monitor helpers in order: parec on the default monitor (PulseAudio >= 13 /
// pipewire-pulse understand @DEFAULT_MONITOR@), pw-record sink-capture on the default sink, then
// parec / pw-record on a discovered .monitor source name. Returns true with l->fd/l->child
// populated on the first helper that streams.
static bool lb_open_pipe(ccord_loopback_audio *l)
{
  char mon[256] = "";
  bool have_mon = false;
  for (int cand = 0; cand < 4; cand++) {
    if (cand >= 2 && !have_mon) {
      have_mon = lb_discover_monitor(mon, sizeof mon);
      if (!have_mon) break;
    }
    lb_argv av;
    memset(&av, 0, sizeof av);
    if (cand == 1) {
      (void)lb_argv_add(&av, "pw-record");
      (void)lb_argv_add(&av, "-P");
      (void)lb_argv_add(&av, "stream.capture.sink=true");
      (void)lb_argv_add(&av, "--rate=48000");
      (void)lb_argv_add(&av, "--channels=1");
      (void)lb_argv_add(&av, "--format=S16LE");
      (void)lb_argv_add(&av, "-");
    } else if (cand == 3) {
      char target[300];
      snprintf(target, sizeof target, "--target=%s", mon);
      (void)lb_argv_add(&av, "pw-record");
      (void)lb_argv_add(&av, "--rate=48000");
      (void)lb_argv_add(&av, "--channels=1");
      (void)lb_argv_add(&av, "--format=S16LE");
      (void)lb_argv_add(&av, target);
      (void)lb_argv_add(&av, "-");
    } else {
      char dev[300];
      snprintf(dev, sizeof dev, "--device=%s", cand == 0 ? "@DEFAULT_MONITOR@" : mon);
      (void)lb_argv_add(&av, "parec");
      (void)lb_argv_add(&av, "--raw");
      (void)lb_argv_add(&av, "--format=s16le");
      (void)lb_argv_add(&av, "--rate=48000");
      (void)lb_argv_add(&av, "--channels=1");
      (void)lb_argv_add(&av, dev);
      (void)lb_argv_add(&av, "--latency-msec=20");
    }
    int fd = -1;
    long child = lb_spawn(av.argv, &fd);
    if (child < 0) continue;
    bool reaped = false;
    if (lb_probe(fd, child, &reaped)) {
      l->kind = LB_KIND_PIPE;
      l->fd = fd;
      l->child = child;
      return true;
    }
    lb_stop(fd, child, reaped);
  }
  return false;
}

static size_t lb_pipe_read(ccord_loopback_audio *l, int16_t *pcm, size_t max_samples)
{
  if (l->failed || l->fd < 0) return 0;
  size_t bytes = 0;
  if (l->have_pending) {
    ((uint8_t *)pcm)[0] = l->pending;
    l->have_pending = false;
    bytes = 1;
  }
  size_t cap = max_samples * 2u;
  while (bytes < cap) {
    ssize_t r = read(l->fd, (uint8_t *)pcm + bytes, cap - bytes);
    if (r > 0) {
      bytes += (size_t)r;
      continue;
    }
    if (r == 0) {   // EOF: the capture child is gone
      l->failed = true;
      break;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN) break;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (errno == EWOULDBLOCK) break;
#endif
    l->failed = true;
    break;
  }
  if ((bytes & 1u) != 0) {   // stash the half sample for the next read
    l->pending = ((uint8_t *)pcm)[bytes - 1];
    l->have_pending = true;
    bytes--;
  }
  size_t got = bytes / 2u;
  if (got == 0 && !l->failed && l->child >= 0) {
    int st = 0;
    if (waitpid((pid_t)l->child, &st, WNOHANG) == (pid_t)l->child) {
      l->reaped = true;
      l->failed = true;
    }
  }
  return got;
}

#endif   // __linux__

static bool lb_name_is_monitor(const char *name)
{
  if (!name) return false;
  if (strncmp(name, "Monitor of ", 11) == 0) return true;
  size_t len = strlen(name);
  return len >= 8 && strcmp(name + len - 8, ".monitor") == 0;
}

static bool lb_open_sdl_monitor(ccord_loopback_audio *l)
{
  bool audio_init = false;
  if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    audio_init = true;
  }
  const char *drv = SDL_GetCurrentAudioDriver();
  if (!drv || strcmp(drv, "dummy") == 0) {
    if (audio_init) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }
  int count = 0;
  SDL_AudioDeviceID *ids = SDL_GetAudioRecordingDevices(&count);
  SDL_AudioDeviceID chosen = 0;
  if (ids && count > 0) {
    for (int i = 0; i < count; i++) {
      if (lb_name_is_monitor(SDL_GetAudioDeviceName(ids[i]))) {
        chosen = ids[i];
        break;
      }
    }
  }
  SDL_free(ids);
  if (chosen == 0) {
    if (audio_init) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }
  SDL_AudioSpec spec = {.format = SDL_AUDIO_S16LE, .channels = 1, .freq = CCORD_LOOPBACK_RATE};
  SDL_AudioStream *st = SDL_OpenAudioDeviceStream(chosen, &spec, nullptr, nullptr);
  if (!st || !SDL_ResumeAudioStreamDevice(st)) {
    if (st) SDL_DestroyAudioStream(st);
    if (audio_init) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }
  l->kind = LB_KIND_SDL;
  l->sdl_stream = st;
  l->sdl_audio_init = audio_init;
  return true;
}

static size_t lb_sdl_read(ccord_loopback_audio *l, int16_t *pcm, size_t max_samples)
{
  if (l->failed || !l->sdl_stream) return 0;
  if (SDL_GetAudioStreamDevice(l->sdl_stream) == 0) {
    l->failed = true;
    return 0;
  }
  int avail = SDL_GetAudioStreamAvailable(l->sdl_stream);
  if (avail < 2) return 0;
  size_t avail_samples = (size_t)avail / 2u;
  size_t want = avail_samples < max_samples ? avail_samples : max_samples;
  if (want > (size_t)(INT32_MAX / 2)) want = (size_t)(INT32_MAX / 2);
  int got = SDL_GetAudioStreamData(l->sdl_stream, pcm, (int)(want * 2u));
  if (got < 0) {
    l->failed = true;
    return 0;
  }
  return (size_t)got / 2u;
}

#if defined(__linux__) && !defined(__ANDROID__)
// Backend preference. Default (unset / "auto" / "pipewire") tries the self-excluding
// PipeWire capture first; "parec" / "pipe" / "sdl" / "legacy" force the whole-sink
// fallback path (used by tests and as an escape hatch).
static bool lb_pw_preferred(void)
{
  const char *e = getenv("CCORD_LOOPBACK_BACKEND");
  if (!e || e[0] == '\0') return true;
  if (strcmp(e, "auto") == 0 || strcmp(e, "pipewire") == 0 || strcmp(e, "pw") == 0) return true;
  return false;
}
#endif

ccord_loopback_audio *ccord_loopback_audio_open(ccord_error *err)
{
  ccord_error_clear(err);
  ccord_loopback_audio *l = ccord_calloc(1, sizeof *l);
  if (!l) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "loopback: out of memory");
    return nullptr;
  }
  l->fd = -1;
  l->child = -1;
  if (ccord_loopback_audio_fake_enabled()) {
    l->kind = LB_KIND_FAKE;
    return l;
  }
#if defined(__linux__) && !defined(__ANDROID__)
  // Self-excluding PipeWire capture first: it never folds our own playback back out.
  if (lb_pw_preferred()) {
    l->pw = ccord_loopback_pw_open();
    if (l->pw) {
      l->kind = LB_KIND_PW;
      return l;
    }
    static atomic_bool warned_pw;
    if (!atomic_exchange(&warned_pw, true))
      CCORD_LOG_W("loopback: PipeWire capture unavailable, falling back to whole-sink monitor");
  }
  if (lb_open_pipe(l)) return l;
#elif defined(_WIN32)
  l->win32 = ccord_loopback_win32_open();
  if (l->win32) {
    l->kind = LB_KIND_WIN32;
    return l;
  }
#endif
  if (lb_open_sdl_monitor(l)) return l;
  ccord_free(l);
  ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "loopback: no desktop monitor capture available");
  return nullptr;
}

size_t ccord_loopback_audio_read(ccord_loopback_audio *l, int16_t *pcm, size_t max_samples)
{
  if (!l || !pcm || max_samples == 0) return 0;
  if (l->kind == LB_KIND_FAKE) return lb_fake_read(l, pcm, max_samples);
#if defined(__linux__) && !defined(__ANDROID__)
  if (l->kind == LB_KIND_PW) return ccord_loopback_pw_read(l->pw, pcm, max_samples);
#elif defined(_WIN32)
  if (l->kind == LB_KIND_WIN32) return ccord_loopback_win32_read(l->win32, pcm, max_samples);
#endif
  if (l->kind == LB_KIND_SDL) return lb_sdl_read(l, pcm, max_samples);
#if defined(__linux__) && !defined(__ANDROID__)
  return lb_pipe_read(l, pcm, max_samples);
#else
  return 0;
#endif
}

bool ccord_loopback_audio_failed(const ccord_loopback_audio *l)
{
  if (!l) return true;
#if defined(__linux__) && !defined(__ANDROID__)
  if (l->kind == LB_KIND_PW) return ccord_loopback_pw_failed(l->pw);
#elif defined(_WIN32)
  if (l->kind == LB_KIND_WIN32) return ccord_loopback_win32_failed(l->win32);
#endif
  return l->failed;
}

void ccord_loopback_audio_close(ccord_loopback_audio *l)
{
  if (!l) return;
#if defined(__linux__) && !defined(__ANDROID__)
  if (l->kind == LB_KIND_PIPE) lb_stop(l->fd, l->child, l->reaped);
  if (l->kind == LB_KIND_PW) ccord_loopback_pw_close(l->pw);
#elif defined(_WIN32)
  if (l->kind == LB_KIND_WIN32) ccord_loopback_win32_close(l->win32);
#endif
  if (l->kind == LB_KIND_SDL) {
    if (l->sdl_stream) SDL_DestroyAudioStream(l->sdl_stream);
    if (l->sdl_audio_init) SDL_QuitSubSystem(SDL_INIT_AUDIO);
  }
  ccord_free(l);
}
