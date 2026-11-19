// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/net/detectable.h>

#include <ccord/core/alloc.h>
#include <ccord/core/os.h>
#include <ccord/core/log.h>
#include <ccord/net/live_transport.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#ifndef CCORD_DETECTABLE_LIVE
#define CCORD_DETECTABLE_LIVE 0
#endif

#define DETECTABLE_CHECK_MS 60000u   // how often an idle, fresh cache is re-examined

static const char k_fixture[] =
  "[{\"id\":\"900000000000000001\",\"name\":\"Fixture Quest\",\"aliases\":[],\"executables\":["
  "{\"os\":\"linux\",\"name\":\"fixturequest\",\"is_launcher\":false},"
  "{\"os\":\"win32\",\"name\":\"FixtureQuest.exe\",\"is_launcher\":false}]},"
  "{\"id\":\"900000000000000002\",\"name\":\"Pixel Racer\",\"executables\":["
  "{\"os\":\"win32\",\"name\":\"pixelracer.exe\",\"is_launcher\":false}]},"
  "{\"id\":\"900000000000000003\",\"name\":\"Fixture Launcher\",\"executables\":["
  "{\"os\":\"linux\",\"name\":\"fixturelauncher\",\"is_launcher\":true}]}]";

const char *ccord_detectable_fixture(size_t *len)
{
  if (len) *len = sizeof k_fixture - 1u;
  return k_fixture;
}

struct ccord_detectable {
  ccord_detectable_config cfg;
  char *cache_dir;              // owned copy, or nullptr
  uint32_t retry_min_ms, retry_max_ms;
  uint32_t backoff_ms;          // current backoff step (0 = no failure outstanding)
  uint64_t next_ms;             // earliest time the next decision is made
  bool started;                 // next_ms is valid
  bool inflight;                // a request is running
  bool awaiting;                // a body was delivered; waiting for ccord_detectable_result
  bool mem_fresh;               // without a cache dir: a usable list arrived at mem_ms
  uint64_t mem_ms;
  uint64_t now_ms;              // clock of the poll in progress (REST callbacks read it)
  size_t requests;
#if CCORD_DETECTABLE_LIVE
  ccord_rest_queue *q;
#endif
};

ccord_detectable *ccord_detectable_create(const ccord_detectable_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  if (!cfg || !cfg->on_body) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "detectable: missing body callback");
    return nullptr;
  }
  ccord_detectable *d = ccord_calloc(1, sizeof *d);
  if (!d) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "detectable: out of memory");
    return nullptr;
  }
  d->cfg = *cfg;
  if (cfg->cache_dir && cfg->cache_dir[0] != '\0') {
    d->cache_dir = ccord_strndup(cfg->cache_dir, strlen(cfg->cache_dir));
    if (!d->cache_dir) {
      ccord_free(d);
      (void)ccord_error_set(err, CCORD_ERR_NOMEM, "detectable: out of memory");
      return nullptr;
    }
  }
  d->cfg.cache_dir = d->cache_dir;
  d->retry_min_ms = cfg->retry_min_ms ? cfg->retry_min_ms : CCORD_DETECTABLE_RETRY_MIN_MS;
  d->retry_max_ms = cfg->retry_max_ms ? cfg->retry_max_ms : CCORD_DETECTABLE_RETRY_MAX_MS;
  if (d->retry_max_ms < d->retry_min_ms) d->retry_max_ms = d->retry_min_ms;
  return d;
}

void ccord_detectable_destroy(ccord_detectable *d)
{
  if (!d) return;
#if CCORD_DETECTABLE_LIVE
  ccord_rest_queue_destroy(d->q);   // callbacks of requests still running are dropped, never delivered
#endif
  ccord_free(d->cache_dir);
  ccord_free(d);
}

static void cache_path(const ccord_detectable *d, char *out, size_t cap)
{
  (void)snprintf(out, cap, "%s/%s", d->cache_dir, CCORD_DETECTABLE_CACHE_FILE);
}

// Failure (or rejected body): keep the cache, retry after the next backoff step.
static void schedule_retry(ccord_detectable *d, uint64_t now_ms)
{
  d->backoff_ms = d->backoff_ms == 0 ? d->retry_min_ms
                                     : (d->backoff_ms >= d->retry_max_ms / 2u ? d->retry_max_ms : d->backoff_ms * 2u);
  d->next_ms = now_ms + d->backoff_ms;
}

static void deliver(ccord_detectable *d, const char *body, size_t len, uint64_t now_ms)
{
  d->inflight = false;
  if (!body || len == 0) {
    schedule_retry(d, now_ms);
    return;
  }
  d->awaiting = true;
  d->cfg.on_body(d->cfg.ud, body, len);
}

#if CCORD_DETECTABLE_LIVE
static void rest_done(void *ud, ccord_status status, long http_status, const char *body, size_t body_len)
{
  ccord_detectable *d = ud;
  if (status != CCORD_OK || http_status != 200) {
    CCORD_LOG_W("detectable: fetch failed (status %s, http %ld); keeping the cached list", ccord_status_name(status),
                http_status);
    d->inflight = false;
    schedule_retry(d, d->now_ms);
    return;
  }
  deliver(d, body, body_len, d->now_ms);
}
#endif

static void start_request(ccord_detectable *d, uint64_t now_ms)
{
  d->requests++;
  if (d->cfg.mock) {
    size_t n = 0;
    const char *b = ccord_detectable_fixture(&n);
    deliver(d, b, n, now_ms);
    return;
  }
#if CCORD_DETECTABLE_LIVE
  ccord_error err = {0};
  if (!d->q) {
    ccord_rest_config rc = {.timeout_ms = 30000, .max_response_bytes = CCORD_DETECTABLE_MAX_BYTES};
    d->q = ccord_rest_queue_create(&rc, &err);   // no token: the list is public
  }
  ccord_status st = d->q ? ccord_rest_queue_add(d->q, CCORD_REST_GET, CCORD_DETECTABLE_PATH, nullptr, rest_done, d, &err)
                         : ccord_error_set(&err, CCORD_ERR_IO, "detectable: no REST queue");
  if (st != CCORD_OK) {
    CCORD_LOG_W("detectable: request not started: %s", err.message);
    schedule_retry(d, now_ms);
    return;
  }
  d->inflight = true;
#else
  schedule_retry(d, now_ms);   // no network support in this build: stay on the cache
#endif
}

void ccord_detectable_poll(ccord_detectable *d, uint64_t now_ms, int64_t now_unix_s, bool enabled)
{
  if (!d) return;
  d->now_ms = now_ms;
#if CCORD_DETECTABLE_LIVE
  if (d->q && d->inflight) ccord_rest_queue_pump(d->q, now_ms);
#endif
  if (!enabled || d->inflight || d->awaiting) return;
  if (!d->started) {
    d->started = true;
    d->next_ms = now_ms;
  }
  if (now_ms < d->next_ms) return;
  // Is the cache fresh enough to skip the request?
  bool fresh = false;
  if (d->backoff_ms == 0) {
    if (d->cache_dir) {
      char p[4096];
      struct stat sb;
      cache_path(d, p, sizeof p);
      if (stat(p, &sb) == 0 && S_ISREG(sb.st_mode)) {
        int64_t age = now_unix_s - (int64_t)sb.st_mtime;
        fresh = age < CCORD_DETECTABLE_REFRESH_S;   // a clock set back (negative age) also counts as fresh
      }
    } else if (d->mem_fresh) {
      fresh = now_ms - d->mem_ms < (uint64_t)CCORD_DETECTABLE_REFRESH_S * 1000u;
    }
  }
  if (fresh) {
    d->next_ms = now_ms + DETECTABLE_CHECK_MS;
    return;
  }
  start_request(d, now_ms);
}

void ccord_detectable_result(ccord_detectable *d, bool usable, uint64_t now_ms)
{
  if (!d) return;
  d->awaiting = false;
  if (usable) {
    d->backoff_ms = 0;
    d->mem_fresh = true;
    d->mem_ms = now_ms;
    d->next_ms = now_ms + DETECTABLE_CHECK_MS;
  } else {
    schedule_retry(d, now_ms);
  }
}

size_t ccord_detectable_request_count(const ccord_detectable *d)
{
  return d ? d->requests : 0;
}

bool ccord_detectable_busy(const ccord_detectable *d)
{
  return d && (d->inflight || d->awaiting);
}

char *ccord_detectable_cache_read(const char *dir, size_t *len, int64_t *mtime_s, ccord_error *err)
{
  ccord_error_clear(err);
  if (len) *len = 0;
  if (!dir || dir[0] == '\0') {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "detectable: no cache dir");
    return nullptr;
  }
  char p[4096];
  (void)snprintf(p, sizeof p, "%s/%s", dir, CCORD_DETECTABLE_CACHE_FILE);
  FILE *f = fopen(p, "rb");
  if (!f) {
    (void)ccord_error_set(err, CCORD_ERR_NOT_FOUND, "detectable: no cache file");
    return nullptr;
  }
  struct stat sb;
  if (stat(p, &sb) != 0 || sb.st_size <= 0 ||
      (uint64_t)sb.st_size > CCORD_DETECTABLE_MAX_BYTES) {
    fclose(f);
    (void)ccord_error_set(err, CCORD_ERR_RANGE, "detectable: cache file empty or too large");
    return nullptr;
  }
  size_t n = (size_t)sb.st_size;
  char *buf = ccord_malloc(n + 1u);
  if (!buf) {
    fclose(f);
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "detectable: out of memory");
    return nullptr;
  }
  size_t got = fread(buf, 1, n, f);
  fclose(f);
  if (got != n) {
    ccord_free(buf);
    (void)ccord_error_set(err, CCORD_ERR_IO, "detectable: short read of the cache");
    return nullptr;
  }
  buf[n] = '\0';
  if (len) *len = n;
  if (mtime_s) *mtime_s = (int64_t)sb.st_mtime;
  return buf;
}

ccord_status ccord_detectable_cache_write(const char *dir, const char *body, size_t len, ccord_error *err)
{
  ccord_error_clear(err);
  if (!dir || dir[0] == '\0' || !body || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "detectable: bad cache write");
  char p[4096], tmp[4112];
  (void)snprintf(p, sizeof p, "%s/%s", dir, CCORD_DETECTABLE_CACHE_FILE);
  (void)snprintf(tmp, sizeof tmp, "%s.tmp", p);
  FILE *f = fopen(tmp, "wb");
  if (!f) return ccord_error_set(err, CCORD_ERR_IO, "detectable: cannot create the cache file");
  bool ok = fwrite(body, 1, len, f) == len;
  ok = (fflush(f) == 0) && ok;
  ok = (fclose(f) == 0) && ok;
  if (!ok || !ccord_file_rename_replace(tmp, p)) {
    (void)ccord_file_unlink(tmp);
    return ccord_error_set(err, CCORD_ERR_IO, "detectable: cannot write the cache file");
  }
  return CCORD_OK;
}
