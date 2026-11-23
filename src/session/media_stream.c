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

#include <stdio.h>
#include <string.h>

#include "media_stream_hub.h"

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>

#define STREAM_LOG_RING 16u
#define STREAM_RANGE_STALL_MS 60000u   // an answer that never comes (a dead transport) counts as a failed attempt
#define STREAM_READ_SLICE_MS 100u
#define STREAM_URL_MAX 4096u

static_assert(CCORD_STREAM_RANGE_BLOCKS * CCORD_STREAM_BLOCK_BYTES == CCORD_STREAM_RANGE_MAX_BYTES,
              "one range request is RANGE_BLOCKS blocks");

typedef enum slot_state : uint8_t { SLOT_EMPTY = 0, SLOT_FILLING, SLOT_READY } slot_state;
typedef struct slot {
  uint8_t *data;      // allocated lazily, kept until close
  int64_t block;      // block index while READY
  uint32_t cap;       // allocated capacity in bytes
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

  ccord_mutex *mtx;
  ccord_cond *cv;

  // ---- shared with the reader thread (under mtx) ----
  uint32_t max_blocks;     // effective slot count <= CCORD_STREAM_CACHE_BLOCKS
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
  ccord_cond_broadcast(ms->cv);
}

static uint32_t stream_readahead_blocks(const ccord_media_stream *ms)
{
  uint32_t ra = ms->max_blocks > 4u ? ms->max_blocks - 4u : 1u;
  return ra < CCORD_STREAM_READAHEAD_BLOCKS ? ra : CCORD_STREAM_READAHEAD_BLOCKS;
}

static slot *slot_find(ccord_media_stream *ms, int64_t blk)
{
  for (size_t i = 0; i < ms->max_blocks; i++) {
    if (ms->slots[i].state == SLOT_READY && ms->slots[i].block == blk) return &ms->slots[i];
  }
  return nullptr;
}

static uint32_t resident_locked(const ccord_media_stream *ms)
{
  uint32_t n = 0;
  for (size_t i = 0; i < ms->max_blocks; i++) n += ms->slots[i].state == SLOT_READY;
  return n;
}

// A slot to hold block `blk`: the one already holding it, an empty one, or the least recently used READY block
// outside the protected window [cursor-1, cursor+READAHEAD]. nullptr when every slot is protected.
static slot *slot_acquire(ccord_media_stream *ms, int64_t blk)
{
  slot *s = slot_find(ms, blk);
  if (s) return s;
  int64_t cb = ms->cursor / (int64_t)CCORD_STREAM_BLOCK_BYTES;
  int64_t ra = (int64_t)stream_readahead_blocks(ms);
  slot *victim = nullptr;
  for (size_t i = 0; i < ms->max_blocks; i++) {
    slot *c = &ms->slots[i];
    if (c->state == SLOT_EMPTY) return c;
    if (c->state == SLOT_FILLING) continue;
    if (c->block >= cb - 1 && c->block <= cb + ra) continue;
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
  for (size_t i = 0; i < ms->max_blocks; i++) ccord_free(ms->slots[i].data);
  ccord_cond_destroy(ms->cv);
  ccord_mutex_destroy(ms->mtx);
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
    ccord_mutex_lock(ms->mtx);
    if (ms->flight == FLIGHT_ACTIVE) {   // the old transport will never answer: issue the same range again
      ms->flight = FLIGHT_RETRY;
      ms->retry_at_ms = 0;
    }
    ccord_mutex_unlock(ms->mtx);
  }
}

ccord_media_stream *ccord_stream_hub_open_ex(ccord_stream_hub *h, const char *url, uint32_t max_blocks,
                                             ccord_error *err)
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
  ms->mtx = ccord_mutex_create();
  ms->cv = ms->mtx ? ccord_cond_create() : nullptr;
  if (!ms->mtx || !ms->cv) {
    ccord_cond_destroy(ms->cv);
    ccord_mutex_destroy(ms->mtx);
    ccord_free(ms->url);
    ccord_free(ms);
    ccord_error_set(err, CCORD_ERR_NOMEM, "stream: cannot create synchronisation");
    return nullptr;
  }
  ms->hub = h;
  ms->max_blocks = max_blocks == 0u ? CCORD_STREAM_CACHE_BLOCKS
                 : (max_blocks < 4u ? 4u
                 : (max_blocks > CCORD_STREAM_CACHE_BLOCKS ? CCORD_STREAM_CACHE_BLOCKS : max_blocks));
  ms->total = -1;
  ms->demand = -1;
  ms->st.total = -1;
  host_of(url, ms->host, sizeof ms->host);
  ms->next = h->head;
  h->head = ms;
  CCORD_LOG_I("stream: open host=%s total=unknown ranged=0", ms->host);
  return ms;
}

ccord_media_stream *ccord_stream_hub_open(ccord_stream_hub *h, const char *url, ccord_error *err)
{
  return ccord_stream_hub_open_ex(h, url, 0u, err);
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
  uint32_t ra = stream_readahead_blocks(ms);
  int64_t end = cb + (int64_t)ra;
  if (end > total_blocks - 1) end = total_blocks - 1;
  int64_t b = cb;
  while (b <= end && !block_missing(ms, b)) b++;
  if (b > end) return false;
  uint32_t n = 1;
  while (n < maxn && b + n <= end && block_missing(ms, b + n)) n++;
  // Small tails wait until the read position moves on, unless they reach the end of the file: fewer, bigger requests.
  uint32_t ra_maxn = ra < maxn ? ra : maxn;
  if (n < ra_maxn && !(ms->total >= 0 && b + n == total_blocks)) return false;
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
  ccord_mutex_lock(ms->mtx);
  if (ms->cancelled || ms->error != CCORD_OK) {
    ccord_mutex_unlock(ms->mtx);
    return;
  }
  if (ms->flight == FLIGHT_ACTIVE) {
    if (now_ms - ms->flight_ms >= STREAM_RANGE_STALL_MS) {
      ms->flight_id = 0;   // a late answer no longer maps to this stream
      failed_attempt_locked(ms, now_ms, "no answer");
    }
    if (ms->flight == FLIGHT_ACTIVE || ms->flight == FLIGHT_IDLE) {
      ccord_mutex_unlock(ms->mtx);
      return;
    }
  }
  if (ms->flight == FLIGHT_RETRY) {
    if (now_ms < ms->retry_at_ms) {
      ccord_mutex_unlock(ms->mtx);
      return;
    }
  } else {
    int64_t sb = 0;
    uint32_t nb = 0;
    if (!choose_range(ms, &sb, &nb)) {
      ccord_mutex_unlock(ms->mtx);
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
  ccord_mutex_unlock(ms->mtx);

  ccord_error e;
  ccord_status st = ccord_transport_fetch_media_range(h->t, &req, &e);
  if (st == CCORD_OK) return;

  ccord_mutex_lock(ms->mtx);
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
  ccord_mutex_unlock(ms->mtx);
}

void ccord_stream_hub_pump(ccord_stream_hub *h, uint64_t now_ms)
{
  if (!h) return;
  for (ccord_media_stream *ms = h->head; ms; ms = ms->next) stream_pump(h, ms, now_ms);
}

uint32_t ccord_stream_hub_next_wait_ms(const ccord_stream_hub *h, uint64_t now_ms)
{
  if (!h) return UINT32_MAX;
  uint32_t best = UINT32_MAX;
  for (ccord_media_stream *ms = h->head; ms; ms = ms->next) {
    ccord_mutex_lock(ms->mtx);
    if (!ms->cancelled && ms->error == CCORD_OK) {
      uint32_t rem = UINT32_MAX;
      if (ms->flight == FLIGHT_ACTIVE) {
        uint64_t elapsed = now_ms >= ms->flight_ms ? (now_ms - ms->flight_ms) : 0u;
        rem = elapsed >= STREAM_RANGE_STALL_MS ? 0u : (uint32_t)(STREAM_RANGE_STALL_MS - elapsed);
      } else if (ms->flight == FLIGHT_RETRY) {
        if (now_ms >= ms->retry_at_ms) rem = 0u;
        else {
          uint64_t d = ms->retry_at_ms - now_ms;
          rem = d > UINT32_MAX ? UINT32_MAX : (uint32_t)d;
        }
      } else {
        int64_t sb = 0;
        uint32_t nb = 0;
        if (choose_range(ms, &sb, &nb)) rem = 0u;
      }
      if (rem < best) best = rem;
    }
    ccord_mutex_unlock(ms->mtx);
    if (best == 0u) break;
  }
  return best;
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

typedef struct block_stage {
  size_t slot_idx;
  size_t off;
  size_t chunk;
  size_t buf_cap;
  int64_t block;
  uint8_t *buf;
  bool already_ready;   // slot stayed SLOT_READY with existing data; swap under mtx and free old outside
} block_stage;

// Copies the complete blocks of an accepted body into the cache. A trailing partial block is stored only when it is
// the last block of the file; otherwise it is dropped and fetched again. Releases ms->mtx while allocating and
// copying chunk buffers so the demux reader thread is never stalled on malloc/memcpy, then re-acquires ms->mtx to
// commit ready slots. Caller enters and exits with ms->mtx held. Returns the number of blocks stored.
static uint32_t store_body_locked(ccord_media_stream *ms, int64_t start, const uint8_t *data, size_t len)
{
  const size_t B = CCORD_STREAM_BLOCK_BYTES;
  uint32_t stored = 0;

  for (size_t batch_off = 0; batch_off < len; batch_off += CCORD_STREAM_RANGE_MAX_BYTES) {
    if (ms->cancelled) break;
    block_stage plan[CCORD_STREAM_RANGE_BLOCKS];
    size_t nplan = 0;
    size_t batch_end = batch_off + CCORD_STREAM_RANGE_MAX_BYTES;
    if (batch_end > len) batch_end = len;

    // Phase 1 (under ms->mtx): reserve target slots and detach reusable buffers from non-READY slots.
    for (size_t off = batch_off; off < batch_end; off += B) {
      size_t chunk = len - off < B ? len - off : B;
      int64_t pos = start + (int64_t)off;
      if (chunk < B && !(ms->total >= 0 && pos + (int64_t)chunk == ms->total)) break;
      int64_t blk = pos / (int64_t)B;
      slot *s = slot_acquire(ms, blk);
      if (!s) continue;
      if (s->state == SLOT_READY && s->block == blk && s->valid >= (uint32_t)chunk) {
        s->last_use = ++ms->tick;
        stored++;
        continue;
      }
      block_stage st = {
        .slot_idx = (size_t)(s - ms->slots),
        .off = off,
        .chunk = chunk,
        .block = blk,
        .buf = nullptr,
        .already_ready = (s->state == SLOT_READY),
      };
      if (!st.already_ready) {
        st.buf = s->data;
        st.buf_cap = s->cap;
        s->data = nullptr;
        s->cap = 0;
        s->state = SLOT_FILLING;
      }
      plan[nplan++] = st;
    }

    if (nplan == 0) continue;

    // Phase 2 (unlocked): allocate missing block buffers and memcpy payload without stalling the demux thread.
    ccord_mutex_unlock(ms->mtx);
    for (size_t i = 0; i < nplan; i++) {
      if (plan[i].buf && plan[i].buf_cap < plan[i].chunk) {
        ccord_free(plan[i].buf);
        plan[i].buf = nullptr;
        plan[i].buf_cap = 0;
      }
      if (!plan[i].buf) {
        size_t alloc_sz = plan[i].chunk > 0 ? plan[i].chunk : 1u;
        plan[i].buf = ccord_malloc(alloc_sz);
        plan[i].buf_cap = plan[i].buf ? alloc_sz : 0u;
      }
      if (plan[i].buf && plan[i].chunk > 0) memcpy(plan[i].buf, data + plan[i].off, plan[i].chunk);
    }

    // Phase 3 (under ms->mtx): commit ready slots, or roll back cleanly on cancel/OOM.
    uint8_t *to_free[CCORD_STREAM_RANGE_BLOCKS];
    size_t nfree = 0;
    ccord_mutex_lock(ms->mtx);
    for (size_t i = 0; i < nplan; i++) {
      slot *s = &ms->slots[plan[i].slot_idx];
      if (ms->cancelled || !plan[i].buf) {
        if (plan[i].already_ready) {
          if (plan[i].buf) to_free[nfree++] = plan[i].buf;
        } else {
          s->data = plan[i].buf;
          s->cap = (uint32_t)plan[i].buf_cap;
          s->state = SLOT_EMPTY;
        }
        continue;
      }
      if (plan[i].already_ready && s->data) {
        to_free[nfree++] = s->data;
      }
      s->data = plan[i].buf;
      s->cap = (uint32_t)plan[i].buf_cap;
      s->block = plan[i].block;
      s->valid = (uint32_t)plan[i].chunk;
      s->state = SLOT_READY;
      s->last_use = ++ms->tick;
      stored++;
    }
    if (nfree > 0) {
      ccord_mutex_unlock(ms->mtx);
      for (size_t i = 0; i < nfree; i++) ccord_free(to_free[i]);
      ccord_mutex_lock(ms->mtx);
    }
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
      ccord_cond_broadcast(ms->cv);
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
  if (ms->cancelled) {
    ms->flight = FLIGHT_IDLE;
    return;
  }
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
  ccord_cond_broadcast(ms->cv);
}

void ccord_stream_hub_on_range(ccord_stream_hub *h, const ccord_net_event *ev, uint64_t now_ms)
{
  if (!h || !ev || ev->type != CCORD_NET_MEDIA_RANGE) return;
  for (ccord_media_stream *ms = h->head; ms; ms = ms->next) {
    ccord_mutex_lock(ms->mtx);
    bool mine = ms->flight == FLIGHT_ACTIVE && ms->flight_id == ev->range_id && ev->range_id != 0;
    if (mine) {
      if (ms->cancelled) {
        ms->flight = FLIGHT_IDLE;
      } else {
        handle_answer_locked(ms, ev, now_ms);
      }
      ccord_mutex_unlock(ms->mtx);
      return;
    }
    ccord_mutex_unlock(ms->mtx);
  }
  CCORD_LOG_D("stream: dropped a late range answer");   // its stream is closed (or the request was abandoned)
}

// ---------------------------------------------------------------------------------------------------------------
// Public stream API

void ccord_media_stream_cancel(void *p)
{
  ccord_media_stream *ms = p;
  if (!ms) return;
  ccord_mutex_lock(ms->mtx);
  ms->cancelled = true;
  ccord_cond_broadcast(ms->cv);
  ccord_mutex_unlock(ms->mtx);
}

int64_t ccord_media_stream_size(void *p)
{
  ccord_media_stream *ms = p;
  if (!ms) return -1;
  ccord_mutex_lock(ms->mtx);
  int64_t t = ms->total;
  ccord_mutex_unlock(ms->mtx);
  return t;
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
  ccord_mutex_lock(ms->mtx);
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
    (void)ccord_cond_wait_timeout(ms->cv, ms->mtx, STREAM_READ_SLICE_MS);
  }
  ccord_mutex_unlock(ms->mtx);
  return rc;
}

void ccord_media_stream_stats_get(const ccord_media_stream *cms, ccord_media_stream_stats *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  out->total = -1;
  if (!cms) return;
  ccord_media_stream *ms = (ccord_media_stream *)(uintptr_t)cms;   // locking does not change what the caller sees
  ccord_mutex_lock(ms->mtx);
  *out = ms->st;
  out->total = ms->total;
  out->error = ms->error;
  memcpy(out->error_text, ms->error_text, sizeof out->error_text);
  out->resident_blocks = resident_locked(ms);
  ccord_mutex_unlock(ms->mtx);
}
