// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Chunked (HTTP Range) media streams for clicked chat videos: a 64-slot block cache, one range in flight per stream,
// and a blocking byte source for the demux thread.
//
// Threads: the hub and every stream are driven from the session (UI) thread: open/close/pump/on_range. Only
// ccord_media_stream_read_at runs on another thread (the demux thread), and cancel / size may run anywhere. Everything a
// reader and the session thread share (slots, cursor, demand, total, error, cancelled) is guarded by ms->mtx; ms->cv
// wakes blocked readers when a block arrives, the stream fails or is cancelled. The transport is never called with
// the mutex held.
#define _POSIX_C_SOURCE 200809L
#include <ccord/session/media_stream.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "media_stream_hub.h"

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/log.h>

#define STREAM_LOG_RING 16u
#define STREAM_RANGE_STALL_MS 60000u   // an answer that never comes (a dead transport) counts as a failed attempt
#define STREAM_READ_SLICE_MS 100u
#define STREAM_URL_MAX 4096u

static_assert(CCORD_STREAM_RANGE_BLOCKS * CCORD_STREAM_BLOCK_BYTES == CCORD_STREAM_RANGE_MAX_BYTES,
              "one range request is RANGE_BLOCKS blocks");

typedef enum slot_state : uint8_t { SLOT_EMPTY = 0, SLOT_READY } slot_state;
typedef struct slot {
  uint8_t *data;      // allocated lazily, kept until close
  int64_t block;      // block index while READY
  uint32_t valid;     // bytes in data (the last block of a file is short)
  uint64_t last_use;  // LRU tick
  slot_state state;
} slot;

typedef enum flight_state : uint8_t { FLIGHT_IDLE = 0, FLIGHT_ACTIVE, FLIGHT_RETRY } flight_state;

struct ccord_media_stream {
  ccord_stream_hub *hub;
  ccord_media_stream *next;
  char *url;
  char host[96];

  pthread_mutex_t mtx;
  pthread_cond_t cv;

  // ---- shared with the reader thread (under mtx) ----
  slot slots[CCORD_STREAM_CACHE_BLOCKS];
  int64_t total;           // -1 unknown
  int64_t cursor;          // offset of the last read
  int64_t demand;          // block a blocked read waits for, -1 none
  uint64_t tick;
  bool cancelled;
  ccord_status error;
  char error_text[128];

  // ---- session thread only (also read by stats_get under mtx) ----
  bool probed;
  flight_state flight;
  uint64_t flight_id;
  int64_t flight_start;    // byte offset, block aligned
  size_t flight_len;
  uint32_t attempt;        // failed attempts of the current range
  uint64_t flight_ms;      // when the request was issued
  uint64_t retry_at_ms;
  char etag[96];
  ccord_media_stream_stats st;   // counters; total/error/resident are filled in by stats_get
};

struct ccord_stream_hub {
  ccord_transport *t;
  ccord_media_stream *head;
  uint64_t next_id;
};

// ---------------------------------------------------------------------------------------------------------------

static void host_of(const char *url, char *out, size_t cap)
{
  out[0] = '\0';
  if (!url) return;
  const char *p = strstr(url, "://");
  p = p ? p + 3 : url;
  const char *e = p;
  while (*e && *e != '/' && *e != '?' && *e != '#') e++;
  const char *at = nullptr;
  for (const char *q = p; q < e; q++) {
    if (*q == '@') at = q;
  }
  if (at) p = at + 1;
  size_t n = 0;
  while (p + n < e && p[n] != ':' && n + 1 < cap) n++;
  memcpy(out, p, n);
  out[n] = '\0';
}

static void latch_locked(ccord_media_stream *ms, ccord_status st, const char *text)
{
  if (ms->error == CCORD_OK) {
    ms->error = st;
    snprintf(ms->error_text, sizeof ms->error_text, "%s", text);
  }
  pthread_cond_broadcast(&ms->cv);
}

static slot *slot_find(ccord_media_stream *ms, int64_t blk)
{
  for (size_t i = 0; i < CCORD_STREAM_CACHE_BLOCKS; i++) {
    if (ms->slots[i].state == SLOT_READY && ms->slots[i].block == blk) return &ms->slots[i];
  }
  return nullptr;
}

static uint32_t resident_locked(const ccord_media_stream *ms)
{
  uint32_t n = 0;
  for (size_t i = 0; i < CCORD_STREAM_CACHE_BLOCKS; i++) n += ms->slots[i].state == SLOT_READY;
  return n;
}

// A slot to hold block `blk`: the one already holding it, an empty one, or the least recently used READY block
// outside the protected window [cursor-1, cursor+READAHEAD]. nullptr when every slot is protected.
static slot *slot_acquire(ccord_media_stream *ms, int64_t blk)
{
  slot *s = slot_find(ms, blk);
  if (s) return s;
  int64_t cb = ms->cursor / (int64_t)CCORD_STREAM_BLOCK_BYTES;
  slot *victim = nullptr;
  for (size_t i = 0; i < CCORD_STREAM_CACHE_BLOCKS; i++) {
    slot *c = &ms->slots[i];
    if (c->state == SLOT_EMPTY) return c;
    if (c->block >= cb - 1 && c->block <= cb + (int64_t)CCORD_STREAM_READAHEAD_BLOCKS) continue;
    if (!victim || c->last_use < victim->last_use) victim = c;
  }
  if (victim) {
    victim->state = SLOT_EMPTY;
    ms->st.evictions++;
  }
  return victim;
}

// ---------------------------------------------------------------------------------------------------------------
// Hub

ccord_stream_hub *ccord_stream_hub_create(ccord_transport *t)
{
  ccord_stream_hub *h = ccord_calloc(1, sizeof *h);
  if (!h) return nullptr;
  h->t = t;
  h->next_id = 1;
  return h;
}

static void stream_free(ccord_media_stream *ms)
{
  ccord_media_stream_cancel(ms);
  CCORD_LOG_I("stream: close host=%s fetched=%llu MiB requests=%u retries=%u", ms->host,
              (unsigned long long)(ms->st.bytes_fetched >> 20), ms->st.requests, ms->st.retries);
  for (size_t i = 0; i < CCORD_STREAM_CACHE_BLOCKS; i++) ccord_free(ms->slots[i].data);
  pthread_cond_destroy(&ms->cv);
  pthread_mutex_destroy(&ms->mtx);
  ccord_free(ms->url);
  ccord_free(ms);
}

void ccord_stream_hub_close(ccord_stream_hub *h, ccord_media_stream *ms)
{
  if (!h || !ms) return;
  for (ccord_media_stream **pp = &h->head; *pp; pp = &(*pp)->next) {
    if (*pp == ms) {
      *pp = ms->next;
      stream_free(ms);
      return;
    }
  }
}

void ccord_stream_hub_destroy(ccord_stream_hub *h)
{
  if (!h) return;
  while (h->head) ccord_stream_hub_close(h, h->head);
  ccord_free(h);
}

void ccord_stream_hub_set_transport(ccord_stream_hub *h, ccord_transport *t)
{
  if (!h) return;
  h->t = t;
  for (ccord_media_stream *ms = h->head; ms; ms = ms->next) {
    pthread_mutex_lock(&ms->mtx);
    if (ms->flight == FLIGHT_ACTIVE) {   // the old transport will never answer: issue the same range again
      ms->flight = FLIGHT_RETRY;
      ms->retry_at_ms = 0;
    }
    pthread_mutex_unlock(&ms->mtx);
  }
}

ccord_media_stream *ccord_stream_hub_open(ccord_stream_hub *h, const char *url, ccord_error *err)
{
  ccord_error_clear(err);
  if (!h || !url || !*url || strlen(url) > STREAM_URL_MAX) {
    ccord_error_set(err, CCORD_ERR_INVALID, "stream: bad url");
    return nullptr;
  }
  if (!CCORD_TRANSPORT_OPTIONAL(h->t, fetch_media_range)) {
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "stream: transport has no ranged media fetch");
    return nullptr;
  }
  ccord_media_stream *ms = ccord_calloc(1, sizeof *ms);
  if (!ms) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "stream: out of memory");
    return nullptr;
  }
  ms->url = ccord_strdup(url);
  if (!ms->url) {
    ccord_free(ms);
    ccord_error_set(err, CCORD_ERR_NOMEM, "stream: out of memory");
    return nullptr;
  }
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
  bool ok = pthread_mutex_init(&ms->mtx, nullptr) == 0;
  if (ok && pthread_cond_init(&ms->cv, &ca) != 0) {
    pthread_mutex_destroy(&ms->mtx);
    ok = false;
  }
  pthread_condattr_destroy(&ca);
  if (!ok) {
    ccord_free(ms->url);
    ccord_free(ms);
    ccord_error_set(err, CCORD_ERR_NOMEM, "stream: cannot create synchronisation");
    return nullptr;
  }
  ms->hub = h;
  ms->total = -1;
  ms->demand = -1;
  ms->st.total = -1;
  host_of(url, ms->host, sizeof ms->host);
  ms->next = h->head;
  h->head = ms;
  CCORD_LOG_I("stream: open host=%s total=unknown ranged=0", ms->host);
  return ms;
}

// ---------------------------------------------------------------------------------------------------------------
// Scheduling (session thread)

static bool block_missing(ccord_media_stream *ms, int64_t blk)
{
  return slot_find(ms, blk) == nullptr;
}

// Picks the next range: the probe (bytes 0..1 MiB-1) first, then the demand block, then read-ahead. Under ms->mtx.
static bool choose_range(ccord_media_stream *ms, int64_t *start_blk, uint32_t *nblk)
{
  const int64_t B = (int64_t)CCORD_STREAM_BLOCK_BYTES;
  int64_t total_blocks = ms->total >= 0 ? (ms->total + B - 1) / B : INT64_MAX;
  uint32_t maxn = CCORD_STREAM_RANGE_BLOCKS;
  if (!ms->probed) {
    *start_blk = 0;
    *nblk = (uint32_t)(total_blocks < (int64_t)maxn ? (total_blocks > 0 ? total_blocks : 1) : maxn);
    return true;
  }
  if (ms->demand >= 0) {
    int64_t d = ms->demand;
    if (d < total_blocks && block_missing(ms, d)) {
      uint32_t n = 1;
      while (n < maxn && d + n < total_blocks && block_missing(ms, d + n)) n++;
      *start_blk = d;
      *nblk = n;
      return true;
    }
    ms->demand = -1;
  }
  int64_t cb = ms->cursor / B;
  int64_t end = cb + (int64_t)CCORD_STREAM_READAHEAD_BLOCKS;
  if (end > total_blocks - 1) end = total_blocks - 1;
  int64_t b = cb;
  while (b <= end && !block_missing(ms, b)) b++;
  if (b > end) return false;
  uint32_t n = 1;
  while (n < maxn && b + n <= end && block_missing(ms, b + n)) n++;
  // Small tails wait until the read position moves on, unless they reach the end of the file: fewer, bigger requests.
  if (n < maxn && !(ms->total >= 0 && b + n == total_blocks)) return false;
  *start_blk = b;
  *nblk = n;
  return true;
}

static void failed_attempt_locked(ccord_media_stream *ms, uint64_t now_ms, const char *reason)
{
  ms->attempt++;
  if (ms->attempt >= CCORD_STREAM_MAX_ATTEMPTS) {
    ms->flight = FLIGHT_IDLE;
    char text[128];
    snprintf(text, sizeof text, "range request failed: %s", reason);
    CCORD_LOG_W("stream: range start=%lld failed (%s) attempt=%u/%u host=%s; giving up", (long long)ms->flight_start,
                reason, ms->attempt, CCORD_STREAM_MAX_ATTEMPTS, ms->host);
    latch_locked(ms, CCORD_ERR_IO, text);
    return;
  }
  ms->st.retries++;
  ms->flight = FLIGHT_RETRY;
  ms->retry_at_ms = now_ms + (ms->attempt == 1 ? 250u : 1000u);
  CCORD_LOG_W("stream: range start=%lld failed (%s) attempt=%u/%u host=%s", (long long)ms->flight_start, reason,
              ms->attempt, CCORD_STREAM_MAX_ATTEMPTS, ms->host);
}

static void stream_pump(ccord_stream_hub *h, ccord_media_stream *ms, uint64_t now_ms)
{
  pthread_mutex_lock(&ms->mtx);
  if (ms->cancelled || ms->error != CCORD_OK) {
    pthread_mutex_unlock(&ms->mtx);
    return;
  }
  if (ms->flight == FLIGHT_ACTIVE) {
    if (now_ms - ms->flight_ms >= STREAM_RANGE_STALL_MS) {
      ms->flight_id = 0;   // a late answer no longer maps to this stream
      failed_attempt_locked(ms, now_ms, "no answer");
    }
    if (ms->flight == FLIGHT_ACTIVE || ms->flight == FLIGHT_IDLE) {
      pthread_mutex_unlock(&ms->mtx);
      return;
    }
  }
  if (ms->flight == FLIGHT_RETRY) {
    if (now_ms < ms->retry_at_ms) {
      pthread_mutex_unlock(&ms->mtx);
      return;
    }
  } else {
    int64_t sb = 0;
    uint32_t nb = 0;
    if (!choose_range(ms, &sb, &nb)) {
      pthread_mutex_unlock(&ms->mtx);
      return;
    }
    ms->probed = true;
    ms->flight_start = sb * (int64_t)CCORD_STREAM_BLOCK_BYTES;
    size_t len = (size_t)nb * CCORD_STREAM_BLOCK_BYTES;
    if (ms->total >= 0 && ms->flight_start + (int64_t)len > ms->total) len = (size_t)(ms->total - ms->flight_start);
    ms->flight_len = len;
    ms->attempt = 0;
  }
  ms->flight = FLIGHT_ACTIVE;
  ms->flight_id = h->next_id++;
  ms->flight_ms = now_ms;
  ms->st.requests++;
  ccord_media_range_req req = {.id = ms->flight_id, .url = ms->url, .start = ms->flight_start,
                               .len = ms->flight_len, .if_range = ms->etag[0] ? ms->etag : nullptr};
  pthread_mutex_unlock(&ms->mtx);

  ccord_error e;
  ccord_status st = ccord_transport_fetch_media_range(h->t, &req, &e);
  if (st == CCORD_OK) return;

  pthread_mutex_lock(&ms->mtx);
  if (ms->flight == FLIGHT_ACTIVE && ms->flight_id == req.id) {
    ms->flight_id = 0;
    if (st == CCORD_ERR_PERMISSION || st == CCORD_ERR_UNSUPPORTED || st == CCORD_ERR_INVALID) {
      ms->flight = FLIGHT_IDLE;
      CCORD_LOG_W("stream: range refused by the transport host=%s", ms->host);
      latch_locked(ms, st, st == CCORD_ERR_PERMISSION ? "link refused by the media gate" : "ranged fetch refused");
    } else {
      failed_attempt_locked(ms, now_ms, ccord_status_name(st));
    }
  }
  pthread_mutex_unlock(&ms->mtx);
}

void ccord_stream_hub_pump(ccord_stream_hub *h, uint64_t now_ms)
{
  if (!h) return;
  for (ccord_media_stream *ms = h->head; ms; ms = ms->next) stream_pump(h, ms, now_ms);
}

// ---------------------------------------------------------------------------------------------------------------
// Answers (session thread)

static void log_range_locked(ccord_media_stream *ms, int64_t start, uint32_t len)
{
  if (ms->st.log_count == STREAM_LOG_RING) {
    memmove(&ms->st.log_start[0], &ms->st.log_start[1], (STREAM_LOG_RING - 1) * sizeof ms->st.log_start[0]);
    memmove(&ms->st.log_len[0], &ms->st.log_len[1], (STREAM_LOG_RING - 1) * sizeof ms->st.log_len[0]);
    ms->st.log_count--;
  }
  ms->st.log_start[ms->st.log_count] = start;
  ms->st.log_len[ms->st.log_count] = len;
  ms->st.log_count++;
}

// Copies the complete blocks of an accepted body into the cache. A trailing partial block is stored only when it is
// the last block of the file; otherwise it is dropped and fetched again. Returns the number of blocks stored.
static uint32_t store_body_locked(ccord_media_stream *ms, int64_t start, const uint8_t *data, size_t len)
{
  const size_t B = CCORD_STREAM_BLOCK_BYTES;
  uint32_t stored = 0;
  for (size_t off = 0; off < len; off += B) {
    size_t chunk = len - off < B ? len - off : B;
    int64_t pos = start + (int64_t)off;
    if (chunk < B && !(ms->total >= 0 && pos + (int64_t)chunk == ms->total)) break;
    slot *s = slot_acquire(ms, pos / (int64_t)B);
    if (!s) continue;
    if (!s->data) {
      s->data = ccord_malloc(B);
      if (!s->data) continue;
    }
    memcpy(s->data, data + off, chunk);
    s->block = pos / (int64_t)B;
    s->valid = (uint32_t)chunk;
    s->state = SLOT_READY;
    s->last_use = ++ms->tick;
    stored++;
  }
  uint32_t r = resident_locked(ms);
  ms->st.resident_blocks = r;
  if (r > ms->st.peak_resident_blocks) ms->st.peak_resident_blocks = r;
  return stored;
}

static void handle_answer_locked(ccord_media_stream *ms, const ccord_net_event *ev, uint64_t now_ms)
{
  const int64_t asked_start = ms->flight_start;
  const size_t asked_len = ms->flight_len;
  ms->st.last_redirects = ev->range_redirects;

  if (ev->range_http == 416) {   // asked past the end
    ms->flight = FLIGHT_IDLE;
    if (asked_start == 0 && (ev->range_total == 0 || ev->range_total < 0)) {
      latch_locked(ms, CCORD_ERR_IO, "empty file");
    } else if (ev->range_total >= 0) {
      ms->total = ev->range_total;
      ms->demand = ev->range_total <= ms->cursor ? -1 : ms->demand;
      pthread_cond_broadcast(&ms->cv);
    } else {
      latch_locked(ms, CCORD_ERR_IO, "range not satisfiable");
    }
    return;
  }

  if (ev->status != CCORD_OK) {
    int http = ev->range_http;
    if (ev->status == CCORD_ERR_PERMISSION || http == 401 || http == 403 || http == 410) {
      ms->flight = FLIGHT_IDLE;
      CCORD_LOG_W("stream: link expired or refused (HTTP %d) host=%s", http, ms->host);
      char text[128];
      snprintf(text, sizeof text, "link expired or refused (HTTP %d)", http ? http : 403);
      latch_locked(ms, CCORD_ERR_PERMISSION, text);
    } else if (ev->status == CCORD_ERR_NOT_FOUND || http == 404) {
      ms->flight = FLIGHT_IDLE;
      latch_locked(ms, CCORD_ERR_NOT_FOUND, "file not found (HTTP 404)");
    } else if (ev->status == CCORD_ERR_UNSUPPORTED) {
      ms->flight = FLIGHT_IDLE;
      CCORD_LOG_W("stream: redirect refused host=%s", ms->host);
      latch_locked(ms, CCORD_ERR_UNSUPPORTED, "redirect refused");
    } else if (http >= 400 && http < 500) {
      ms->flight = FLIGHT_IDLE;
      char text[128];
      snprintf(text, sizeof text, "request refused (HTTP %d)", http);
      latch_locked(ms, CCORD_ERR_IO, text);
    } else {
      char reason[48];
      if (http) snprintf(reason, sizeof reason, "HTTP %d", http);
      else snprintf(reason, sizeof reason, "%s", ccord_status_name(ev->status));
      failed_attempt_locked(ms, now_ms, reason);
    }
    return;
  }

  if (ev->range_flags & CCORD_RANGE_UNRANGED) {
    ms->flight = FLIGHT_IDLE;
    ms->st.no_range = true;
    if (ev->range_total >= 0) ms->total = ev->range_total;
    CCORD_LOG_W("stream: %s server ignores Range (HTTP 200)", ms->host);
    latch_locked(ms, CCORD_ERR_UNSUPPORTED, "server ignores Range requests");
    return;
  }

  size_t len = ev->media_len;
  if (ev->range_start != asked_start || len > asked_len || (len > 0 && !ev->media_data)) {
    ms->flight = FLIGHT_IDLE;
    latch_locked(ms, CCORD_ERR_IO, "range mismatch");
    return;
  }
  if (ms->total >= 0 && ev->range_total >= 0 && ev->range_total != ms->total) {
    ms->flight = FLIGHT_IDLE;
    latch_locked(ms, CCORD_ERR_IO, "file changed while streaming");
    return;
  }
  if (ev->range_total >= 0) {
    ms->total = ev->range_total;
  } else if (ms->total < 0 && ((ev->range_flags & CCORD_RANGE_EOF) || len < asked_len)) {
    ms->total = asked_start + (int64_t)len;   // a short body at an unknown total is the end of the file
  }
  if (asked_start + (int64_t)len > ms->total && ms->total >= 0) {
    ms->flight = FLIGHT_IDLE;
    latch_locked(ms, CCORD_ERR_IO, "range mismatch");
    return;
  }
  if (ev->range_http == 206 || (ev->range_flags & CCORD_RANGE_ACCEPTS)) ms->st.ranged = true;
  if (ev->range_etag[0] && !ms->etag[0]) snprintf(ms->etag, sizeof ms->etag, "%s", ev->range_etag);

  uint32_t stored = store_body_locked(ms, asked_start, ev->media_data, len);
  bool at_end = ms->total >= 0 && asked_start >= ms->total;   // asked exactly at the end: nothing to store
  if (stored == 0 && !at_end && len > 0) {   // not one whole block: no progress, try again
    failed_attempt_locked(ms, now_ms, "short body");
    return;
  }
  if (stored == 0 && len == 0 && !at_end && ms->total < 0) {
    failed_attempt_locked(ms, now_ms, "empty body");
    return;
  }
  ms->flight = FLIGHT_IDLE;
  ms->attempt = 0;
  ms->st.bytes_fetched += len;
  log_range_locked(ms, asked_start, (uint32_t)len);
  CCORD_LOG_D("stream: range start=%lld len=%zu http=%d redirects=%u host=%s", (long long)asked_start, len,
              (int)ev->range_http, (unsigned)ev->range_redirects, ms->host);
  if (ms->demand >= 0 && slot_find(ms, ms->demand)) ms->demand = -1;
  pthread_cond_broadcast(&ms->cv);
}

void ccord_stream_hub_on_range(ccord_stream_hub *h, const ccord_net_event *ev, uint64_t now_ms)
{
  if (!h || !ev || ev->type != CCORD_NET_MEDIA_RANGE) return;
  for (ccord_media_stream *ms = h->head; ms; ms = ms->next) {
    pthread_mutex_lock(&ms->mtx);
    bool mine = ms->flight == FLIGHT_ACTIVE && ms->flight_id == ev->range_id && ev->range_id != 0;
    if (mine) {
      if (ms->cancelled) {
        ms->flight = FLIGHT_IDLE;
      } else {
        handle_answer_locked(ms, ev, now_ms);
      }
      pthread_mutex_unlock(&ms->mtx);
      return;
    }
    pthread_mutex_unlock(&ms->mtx);
  }
  CCORD_LOG_D("stream: dropped a late range answer");   // its stream is closed (or the request was abandoned)
}

// ---------------------------------------------------------------------------------------------------------------
// Public stream API

void ccord_media_stream_cancel(void *p)
{
  ccord_media_stream *ms = p;
  if (!ms) return;
  pthread_mutex_lock(&ms->mtx);
  ms->cancelled = true;
  pthread_cond_broadcast(&ms->cv);
  pthread_mutex_unlock(&ms->mtx);
}

int64_t ccord_media_stream_size(void *p)
{
  ccord_media_stream *ms = p;
  if (!ms) return -1;
  pthread_mutex_lock(&ms->mtx);
  int64_t t = ms->total;
  pthread_mutex_unlock(&ms->mtx);
  return t;
}

static void deadline_after_ms(struct timespec *ts, unsigned ms)
{
  clock_gettime(CLOCK_MONOTONIC, ts);
  ts->tv_sec += ms / 1000u;
  ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec++;
    ts->tv_nsec -= 1000000000L;
  }
}

// Copies from consecutive READY blocks starting at off. Returns bytes copied (0 when the first block is missing).
static int copy_ready_locked(ccord_media_stream *ms, int64_t off, uint8_t *buf, int n)
{
  const int64_t B = (int64_t)CCORD_STREAM_BLOCK_BYTES;
  int done = 0;
  while (done < n) {
    int64_t pos = off + done;
    slot *s = slot_find(ms, pos / B);
    if (!s) break;
    int64_t within = pos - s->block * B;
    if (within >= (int64_t)s->valid) break;   // end of a short last block
    int64_t avail = (int64_t)s->valid - within;
    int take = (int64_t)(n - done) < avail ? n - done : (int)avail;
    memcpy(buf + done, s->data + within, (size_t)take);
    s->last_use = ++ms->tick;
    done += take;
  }
  return done;
}

int ccord_media_stream_read_at(void *p, int64_t off, uint8_t *buf, int n)
{
  ccord_media_stream *ms = p;
  if (!ms || !buf || off < 0) return -(int)CCORD_ERR_INVALID;
  if (n <= 0) return 0;
  pthread_mutex_lock(&ms->mtx);
  uint64_t waited_from = ccord_clock_ms();
  int rc;
  for (;;) {
    if (ms->cancelled) {
      rc = -(int)CCORD_ERR_IO;
      break;
    }
    if (ms->st.no_range) {   // nothing was kept from the 200 probe
      rc = -(int)ms->error;
      break;
    }
    ms->cursor = off;
    int got = copy_ready_locked(ms, off, buf, n);
    if (got > 0) {
      rc = got;
      break;
    }
    if (ms->total >= 0 && off >= ms->total) {
      rc = 0;
      break;
    }
    if (ms->error != CCORD_OK) {   // buffered blocks stay readable (above); a missing one will never come
      rc = -(int)ms->error;
      break;
    }
    ms->demand = off / (int64_t)CCORD_STREAM_BLOCK_BYTES;
    if (ccord_clock_ms() - waited_from >= CCORD_STREAM_READ_TIMEOUT_MS) {
      rc = -(int)CCORD_ERR_TIMEOUT;
      break;
    }
    struct timespec dl;
    deadline_after_ms(&dl, STREAM_READ_SLICE_MS);
    (void)pthread_cond_timedwait(&ms->cv, &ms->mtx, &dl);
  }
  pthread_mutex_unlock(&ms->mtx);
  return rc;
}

void ccord_media_stream_stats_get(const ccord_media_stream *cms, ccord_media_stream_stats *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  out->total = -1;
  if (!cms) return;
  ccord_media_stream *ms = (ccord_media_stream *)(uintptr_t)cms;   // locking does not change what the caller sees
  pthread_mutex_lock(&ms->mtx);
  *out = ms->st;
  out->total = ms->total;
  out->error = ms->error;
  memcpy(out->error_text, ms->error_text, sizeof out->error_text);
  out->resident_blocks = resident_locked(ms);
  pthread_mutex_unlock(&ms->mtx);
}
