// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "game_detect_app.h"

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/net/detectable.h>
#include <ccord/rpc/game_detect.h>

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if CCORD_OS_LINUX || CCORD_OS_ANDROID
#include <unistd.h>
#endif

typedef enum job_kind { JOB_NONE, JOB_LOAD_CACHE, JOB_LOAD_BODY, JOB_SCAN, JOB_FREE_LIST } job_kind;

typedef struct job_result {
  job_kind kind;
  bool ok;                     // LOAD_*: a usable list is loaded; SCAN: a game matched
  ccord_game_match match;      // SCAN
  int64_t start_unix_ms;       // SCAN: process start as wall-clock milliseconds
  uint32_t epoch;              // the enable epoch the job was submitted in
} job_result;

struct ccord_game_detect_app {
  ccord_game_detect_app_config cfg;
  char *cache_dir;             // owned copies (the worker reads them)
  char *proc_root;
  uint32_t scan_ms;
  ccord_detectable *det;

  // Main-thread state.
  uint32_t epoch;              // bumped whenever detection is switched off: older scan results are dropped
  bool was_enabled;
  bool cache_started;          // the cache load job was submitted
  bool cache_done;             // ... and answered (or there is no cache): only then is the refresh schedule consulted
  bool list_ready;
  bool need_free_list;
  bool scan_due;
  uint64_t next_scan_ms;
  uint64_t last_now_ms;
  char *pending_body;          // fetched body waiting for a free worker
  size_t pending_len;
  bool has_detected;
  ccord_rpc_activity detected;
  int32_t detected_pid;
  uint64_t detected_ticks;
  size_t scans;

  // Worker.
  ccord_game_list *list;       // worker-owned (main thread in synchronous mode)
#if !CCORD_OS_WINDOWS && !CCORD_OS_MACOS
  long clk_tck;                // cached sysconf(_SC_CLK_TCK)
  int64_t boot_unix_ms;        // cached boot wall-clock timestamp for this proc_root
  bool boot_cached;
#endif
  ccord_thread *thread;
  bool thread_started;
  ccord_mutex *mu;
  ccord_cond *cv;
  bool quit;
  bool job_pending;
  bool running;
  bool result_ready;
  _Atomic bool result_ready_atomic;
  _Atomic bool worker_busy_atomic;
  job_kind job;
  char *job_body;
  size_t job_len;
  uint32_t job_epoch;
  job_result result;
};

static void body_cb(void *ud, const char *body, size_t len)
{
  ccord_game_detect_app *g = ud;
  ccord_free(g->pending_body);
  g->pending_body = ccord_malloc(len + 1u);
  if (!g->pending_body) {
    g->pending_len = 0;
    ccord_detectable_result(g->det, false, g->last_now_ms);
    return;
  }
  memcpy(g->pending_body, body, len);
  g->pending_body[len] = '\0';
  g->pending_len = len;
}

// ---- worker -----------------------------------------------------------------------------------------------------

static void replace_list(ccord_game_detect_app *g, ccord_game_list *nl)
{
  ccord_game_list_free(g->list);
  g->list = nl;
}

// Process start as wall-clock ms.
static int64_t start_unix_ms(ccord_game_detect_app *g, uint64_t start_ticks)
{
  int64_t now = ccord_clock_unix_ms();
#if CCORD_OS_WINDOWS
  (void)g;
  const uint64_t epoch_diff = 116444736000000000ULL;
  if (start_ticks >= epoch_diff) return (int64_t)((start_ticks - epoch_diff) / 10000ULL);
  return now;
#elif CCORD_OS_MACOS
  (void)g;
  if (start_ticks > 0) return (int64_t)(start_ticks / 1000ULL);
  return now;
#else
  if (!g->boot_cached) {
    g->clk_tck = sysconf(_SC_CLK_TCK);
    char p[4096];
    (void)snprintf(p, sizeof p, "%s/uptime", g->proc_root);
    FILE *f = fopen(p, "r");
    if (f) {
      double up = 0;
      int ok = fscanf(f, "%lf", &up);
      fclose(f);
      if (ok == 1 && g->clk_tck > 0 && up >= 0.0) {
        g->boot_unix_ms = now - (int64_t)(up * 1000.0);
        g->boot_cached = true;
      }
    }
  }
  if (!g->boot_cached || g->clk_tck <= 0) return now;
  int64_t start_ms = g->boot_unix_ms + (int64_t)(((double)start_ticks / (double)g->clk_tck) * 1000.0);
  return start_ms > now ? now : start_ms;
#endif
}

static void run_job(ccord_game_detect_app *g, job_kind kind, char *body, size_t len, uint32_t epoch, job_result *res)
{
  memset(res, 0, sizeof *res);
  res->kind = kind;
  res->epoch = epoch;
  ccord_error err = {0};
  if (kind == JOB_LOAD_CACHE) {
    size_t n = 0;
    char *buf = ccord_detectable_cache_read(g->cache_dir, &n, nullptr, &err);
    if (buf) {
      ccord_game_list *nl = ccord_game_list_load(buf, n, &err);
      ccord_free(buf);
      if (nl) {
        replace_list(g, nl);
        res->ok = true;
      } else {
        // An unusable cache would otherwise count as fresh for a day: drop it so the list is fetched again.
        CCORD_LOG_W("game detect: cached list unusable (%s); it will be refetched", err.message);
        char p[4096];
        (void)snprintf(p, sizeof p, "%s/%s", g->cache_dir, CCORD_DETECTABLE_CACHE_FILE);
        (void)ccord_file_unlink(p);
      }
    }
  } else if (kind == JOB_LOAD_BODY) {
    ccord_game_list *nl = ccord_game_list_load(body, len, &err);
    if (nl) {
      replace_list(g, nl);
      res->ok = true;
      if (g->cache_dir && ccord_detectable_cache_write(g->cache_dir, body, len, &err) != CCORD_OK) {
        CCORD_LOG_W("game detect: list cache not written: %s", err.message);
      }
    } else {
      CCORD_LOG_W("game detect: fetched list rejected: %s", err.message);
    }
  } else if (kind == JOB_SCAN && g->list) {
    ccord_status st = ccord_game_scan(g->list, g->proc_root, &res->match, &err);
    if (st == CCORD_OK) {
      res->ok = true;
      res->start_unix_ms = start_unix_ms(g, res->match.start_ticks);
    }
  } else if (kind == JOB_FREE_LIST) {
    replace_list(g, nullptr);
    res->ok = true;
  }
  ccord_free(body);
  if (kind == JOB_LOAD_CACHE || kind == JOB_LOAD_BODY || kind == JOB_FREE_LIST) {
    ccord_os_trim_memory();
  }
}

static void worker_main(void *arg)
{
  ccord_game_detect_app *g = arg;
  ccord_mutex_lock(g->mu);
  for (;;) {
    while (!g->job_pending && !g->quit) ccord_cond_wait(g->cv, g->mu);
    if (g->quit) break;
    job_kind kind = g->job;
    char *body = g->job_body;
    size_t len = g->job_len;
    uint32_t epoch = g->job_epoch;
    g->job_body = nullptr;
    g->job_pending = false;
    g->running = true;
    ccord_mutex_unlock(g->mu);
    job_result res;
    run_job(g, kind, body, len, epoch, &res);
    ccord_mutex_lock(g->mu);
    g->result = res;
    g->result_ready = true;
    g->running = false;
    atomic_store_explicit(&g->result_ready_atomic, true, memory_order_release);
    atomic_store_explicit(&g->worker_busy_atomic, true, memory_order_release);
  }
  ccord_mutex_unlock(g->mu);
}

// ---- main thread ------------------------------------------------------------------------------------------------

ccord_game_detect_app *ccord_game_detect_app_create(const ccord_game_detect_app_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  if (!cfg) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "game detect: missing config");
    return nullptr;
  }
  ccord_game_detect_app *g = ccord_calloc(1, sizeof *g);
  if (!g) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "game detect: out of memory");
    return nullptr;
  }
  g->cfg = *cfg;
  atomic_init(&g->result_ready_atomic, false);
  atomic_init(&g->worker_busy_atomic, false);
  g->mu = ccord_mutex_create();
  g->cv = ccord_cond_create();
  if (!g->mu || !g->cv) {
    ccord_cond_destroy(g->cv);
    ccord_mutex_destroy(g->mu);
    ccord_free(g);
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "game detect: out of memory");
    return nullptr;
  }
  if (cfg->cache_dir && cfg->cache_dir[0] != '\0') g->cache_dir = ccord_strndup(cfg->cache_dir, strlen(cfg->cache_dir));
  const char *root = cfg->proc_root && cfg->proc_root[0] != '\0' ? cfg->proc_root : CCORD_GAME_DETECT_PROC_ROOT;
  g->proc_root = ccord_strndup(root, strlen(root));
  g->scan_ms = cfg->scan_interval_ms ? cfg->scan_interval_ms : CCORD_GAME_DETECT_SCAN_MS;
  g->cache_started = g->cache_done = g->cache_dir == nullptr;   // nothing to load without a cache directory
  ccord_detectable_config dc = {.cache_dir = g->cache_dir, .mock = cfg->mock, .retry_min_ms = cfg->retry_min_ms,
                                .on_body = body_cb, .ud = g};
  g->det = ccord_detectable_create(&dc, err);
  bool oom = !g->proc_root || (cfg->cache_dir && cfg->cache_dir[0] != '\0' && !g->cache_dir);
  if (!g->det || oom) {
    if (g->det) (void)ccord_error_set(err, CCORD_ERR_NOMEM, "game detect: out of memory");
    ccord_game_detect_app_destroy(g);
    return nullptr;
  }
  return g;
}

void ccord_game_detect_app_destroy(ccord_game_detect_app *g)
{
  if (!g) return;
  if (g->thread_started) {
    ccord_mutex_lock(g->mu);
    g->quit = true;
    ccord_cond_signal(g->cv);
    ccord_mutex_unlock(g->mu);
    ccord_thread_join(g->thread);
    g->thread = nullptr;
  }
  ccord_detectable_destroy(g->det);
  ccord_game_list_free(g->list);
  ccord_free(g->job_body);
  ccord_free(g->pending_body);
  ccord_free(g->cache_dir);
  ccord_free(g->proc_root);
  ccord_mutex_destroy(g->mu);
  ccord_cond_destroy(g->cv);
  ccord_free(g);
}

static bool worker_busy(const ccord_game_detect_app *g)
{
  return atomic_load_explicit(&g->worker_busy_atomic, memory_order_acquire);
}

// Hands one job to the worker (or runs it inline in synchronous mode). The caller checked the worker is idle.
static void submit(ccord_game_detect_app *g, job_kind kind, char *body, size_t len)
{
  if (kind == JOB_SCAN) g->scans++;
  if (g->cfg.synchronous) {
    ccord_mutex_lock(g->mu);
    run_job(g, kind, body, len, g->epoch, &g->result);
    g->result_ready = true;
    atomic_store_explicit(&g->result_ready_atomic, true, memory_order_release);
    atomic_store_explicit(&g->worker_busy_atomic, true, memory_order_release);
    ccord_mutex_unlock(g->mu);
    return;
  }
  if (!g->thread_started) {
    g->thread = ccord_thread_create(worker_main, g);
    if (!g->thread) {
      CCORD_LOG_W("game detect: cannot start the worker thread");
      ccord_free(body);
      if (kind == JOB_LOAD_BODY) ccord_detectable_result(g->det, false, g->last_now_ms);
      return;
    }
    g->thread_started = true;
  }
  ccord_mutex_lock(g->mu);
  g->job = kind;
  g->job_body = body;
  g->job_len = len;
  g->job_epoch = g->epoch;
  g->job_pending = true;
  atomic_store_explicit(&g->worker_busy_atomic, true, memory_order_release);
  ccord_cond_signal(g->cv);
  ccord_mutex_unlock(g->mu);
}

static void clear_detected(ccord_game_detect_app *g)
{
  g->has_detected = false;
  memset(&g->detected, 0, sizeof g->detected);
  g->detected_pid = 0;
  g->detected_ticks = 0;
}

// Moves a finished job into the main-thread state; returns true when the detected activity changed.
static bool take_result(ccord_game_detect_app *g, uint64_t now_ms)
{
  if (!atomic_load_explicit(&g->result_ready_atomic, memory_order_acquire)) return false;
  ccord_mutex_lock(g->mu);
  bool have = g->result_ready;
  job_result r = g->result;
  g->result_ready = false;
  atomic_store_explicit(&g->result_ready_atomic, false, memory_order_release);
  atomic_store_explicit(&g->worker_busy_atomic, g->job_pending || g->running, memory_order_release);
  ccord_mutex_unlock(g->mu);
  if (!have) return false;
  if (r.kind == JOB_FREE_LIST) return false;
  if (r.kind == JOB_LOAD_CACHE || r.kind == JOB_LOAD_BODY) {
    if (r.kind == JOB_LOAD_CACHE) g->cache_done = true;
    if (r.kind == JOB_LOAD_BODY) ccord_detectable_result(g->det, r.ok, now_ms);
    if (r.ok) {
      g->list_ready = true;
      g->scan_due = true;   // a new list: look at the process table now
    }
    return false;
  }
  if (r.kind != JOB_SCAN || r.epoch != g->epoch) return false;   // answered for a period when detection was off
  if (!r.ok) {
    bool had = g->has_detected;
    clear_detected(g);
    return had;
  }
  if (g->has_detected && g->detected_pid == r.match.pid && g->detected_ticks == r.match.start_ticks &&
      strcmp(g->detected.application_id, r.match.app_id) == 0) {
    return false;   // same process still running: keep the first computed start time
  }
  memset(&g->detected, 0, sizeof g->detected);
  g->detected.type = 0;   // Playing
  (void)snprintf(g->detected.name, sizeof g->detected.name, "%s", r.match.name);
  (void)snprintf(g->detected.application_id, sizeof g->detected.application_id, "%s", r.match.app_id);
  g->detected.start_ms = r.start_unix_ms;
  g->has_detected = true;
  g->detected_pid = r.match.pid;
  g->detected_ticks = r.match.start_ticks;
  return true;
}

bool ccord_game_detect_app_poll(ccord_game_detect_app *g, uint64_t now_ms)
{
  if (!g) return false;
  g->last_now_ms = now_ms;
  bool changed = false;
  bool enabled = !g->cfg.enabled || g->cfg.enabled(g->cfg.enabled_ud);
  if (!enabled) {
    // Off: nothing is scanned or fetched; whatever was detected disappears now and late scan results are dropped.
    if (g->was_enabled || g->has_detected) {
      g->epoch++;
      if (g->has_detected) changed = true;
      clear_detected(g);
    }
    g->was_enabled = false;
    if (take_result(g, now_ms)) changed = true;   // keeps the list/fetch bookkeeping moving
    // Release the multi-megabyte detectable game list while detection is off; re-enabling reloads from disk cache.
    if (g->pending_body) {
      ccord_free(g->pending_body);
      g->pending_body = nullptr;
      g->pending_len = 0;
      ccord_detectable_result(g->det, false, now_ms);
    }
    if (g->list_ready) g->need_free_list = true;
    if (g->need_free_list && !worker_busy(g)) {
      g->need_free_list = false;
      g->list_ready = false;
      g->cache_started = g->cache_done = (g->cache_dir == nullptr);
      if (g->thread_started || g->cfg.synchronous) {
        submit(g, JOB_FREE_LIST, nullptr, 0);
        if (g->cfg.synchronous) (void)take_result(g, now_ms);
      }
    }
    ccord_detectable_poll(g->det, now_ms, ccord_clock_unix_ms() / 1000, false);
    return changed;
  }
  if (!g->was_enabled) {
    g->was_enabled = true;
    g->need_free_list = false;
    g->scan_due = true;   // resume with a scan right away
  }
  for (int pass = 0; pass < 4; pass++) {
    if (take_result(g, now_ms)) changed = true;
    // The refresh schedule reads the cache file's age, and a fetched list must not be overwritten by a late cache
    // load: nothing is requested until the cache job has answered.
    ccord_detectable_poll(g->det, now_ms, ccord_clock_unix_ms() / 1000, g->cache_done);
    if (worker_busy(g)) break;
    if (g->pending_body) {
      char *b = g->pending_body;
      size_t n = g->pending_len;
      g->pending_body = nullptr;
      g->pending_len = 0;
      submit(g, JOB_LOAD_BODY, b, n);
    } else if (!g->cache_started) {
      g->cache_started = true;
      submit(g, JOB_LOAD_CACHE, nullptr, 0);
    } else if (g->list_ready && (g->scan_due || now_ms >= g->next_scan_ms)) {
      g->scan_due = false;
      g->next_scan_ms = now_ms + g->scan_ms;
      submit(g, JOB_SCAN, nullptr, 0);
    } else {
      break;
    }
    if (!g->cfg.synchronous) break;   // the worker answers on a later poll
  }
  if (g->cfg.synchronous && take_result(g, now_ms)) changed = true;
  return changed;
}

bool ccord_game_detect_app_current(const ccord_game_detect_app *g, ccord_rpc_activity *out)
{
  if (!g || !g->has_detected) return false;
  if (out) *out = g->detected;
  return true;
}

static bool same_name_ci(const char *a, const char *b)
{
  for (; *a && *b; a++, b++) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
  }
  return *a == *b;
}

size_t ccord_game_detect_app_merge(const ccord_game_detect_app *g, ccord_rpc_activity *acts, size_t n, size_t cap)
{
  if (!g || !g->has_detected || n >= cap) return n;
  for (size_t i = 0; i < n; i++) {
    // An RPC client already reports this application: its (richer) activity wins, the detected one is dropped.
    if (acts[i].application_id[0] != '\0' && strcmp(acts[i].application_id, g->detected.application_id) == 0) return n;
    if (same_name_ci(acts[i].name, g->detected.name)) return n;
  }
  acts[n++] = g->detected;
  return n;
}

size_t ccord_game_detect_app_scan_count(const ccord_game_detect_app *g)
{
  return g ? g->scans : 0;
}

size_t ccord_game_detect_app_request_count(const ccord_game_detect_app *g)
{
  return g ? ccord_detectable_request_count(g->det) : 0;
}

bool ccord_game_detect_app_list_ready(const ccord_game_detect_app *g)
{
  return g && g->list_ready;
}
