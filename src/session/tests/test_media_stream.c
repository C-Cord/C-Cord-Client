// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Block cache, readahead and blocking byte source of a ranged media stream.
// Drives the hub directly with a fake transport that answers fetch_media_range on a later step from a byte generator.
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ccord/net/transport.h>
#include <ccord/session/media_stream.h>

#include "../media_stream_hub.h"
#include "ccord_test.h"

#define B ((int64_t)CCORD_STREAM_BLOCK_BYTES)
#define URL "https://cdn.example.org/clip/big.mp4?ex=1&hm=secret"

static uint8_t gen_byte(int64_t off) { return (uint8_t)((off * 131 + (off >> 8) * 7 + 1) & 0xff); }

typedef struct fake {
  ccord_transport base;
  int64_t total;                // virtual file length
  bool report_total;            // put range_total in the answer (else -1)
  uint32_t flags;               // answer flags
  int fail_first;               // answer the first N requests with fail_http
  int fail_http;                // 503 -> ccord_status IO; 403 -> PERMISSION
  int fail_from;                // 1-based request number from which every request answers fail_http (0 = never)
  ccord_status refuse;          // synchronous refusal from fetch_media_range
  // request log
  int requests, inflight, max_inflight;
  int64_t max_len, max_block;
  bool aligned;
  bool pending;
  ccord_media_range_req req;
  char url[256];
} fake;

static ccord_status fake_fetch(ccord_transport *t, const ccord_media_range_req *req, ccord_error *e)
{
  fake *f = (fake *)t;
  f->requests++;
  if (f->refuse != CCORD_OK) return ccord_error_set(e, f->refuse, "fake: refused");
  f->inflight++;
  if (f->inflight > f->max_inflight) f->max_inflight = f->inflight;
  if ((int64_t)req->len > f->max_len) f->max_len = (int64_t)req->len;
  if (req->start % B != 0) f->aligned = false;
  if (req->start / B > f->max_block) f->max_block = req->start / B;
  f->req = *req;
  snprintf(f->url, sizeof f->url, "%s", req->url);
  f->req.url = f->url;
  f->pending = true;
  return CCORD_OK;
}

static void fake_destroy(ccord_transport *t) { (void)t; }
static const ccord_transport_vtable fake_vt = {.name = "fake-range", .fetch_media_range = fake_fetch,
                                               .destroy = fake_destroy};

static void fake_init(fake *f, int64_t total)
{
  memset(f, 0, sizeof *f);
  f->base.vt = &fake_vt;
  f->total = total;
  f->report_total = true;
  f->flags = CCORD_RANGE_ACCEPTS;
  f->aligned = true;
}

// Answers the pending request (if any). Returns true when one was answered.
static bool fake_deliver(fake *f, ccord_stream_hub *h, uint64_t now)
{
  if (!f->pending) return false;
  f->pending = false;
  f->inflight--;
  static uint8_t body[CCORD_STREAM_RANGE_MAX_BYTES];
  ccord_net_event ev = {.type = CCORD_NET_MEDIA_RANGE, .media_url = f->req.url, .range_id = f->req.id,
                        .range_start = f->req.start, .range_total = -1};
  bool fail = f->requests <= f->fail_first || (f->fail_from && f->requests >= f->fail_from);
  if (fail) {
    ev.range_http = f->fail_http;
    ev.status = f->fail_http == 403 ? CCORD_ERR_PERMISSION : CCORD_ERR_IO;
  } else {
    int64_t n = f->total - f->req.start;
    if (n > (int64_t)f->req.len) n = (int64_t)f->req.len;
    if (n < 0) n = 0;
    for (int64_t i = 0; i < n; i++) body[i] = gen_byte(f->req.start + i);
    ev.media_data = body;
    ev.media_len = (size_t)n;
    ev.range_http = (f->flags & CCORD_RANGE_UNRANGED) ? 200 : 206;
    ev.range_flags = f->flags;
    if ((f->flags & CCORD_RANGE_UNRANGED)) ev.media_len = 0;
    if (f->report_total) ev.range_total = f->total;
  }
  ccord_stream_hub_on_range(h, &ev, now);
  return true;
}

// Pumps and answers in steps of 300 ms of synthetic time until *flag is set or steps run out.
static void drive(fake *f, ccord_stream_hub *h, uint64_t *now, atomic_int *flag, int steps)
{
  for (int i = 0; i < steps; i++) {
    if (flag && atomic_load(flag)) return;
    *now += 300;
    ccord_stream_hub_pump(h, *now);
    if (!fake_deliver(f, h, *now)) {
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 200000};
      nanosleep(&ts, nullptr);
    }
  }
}

static void sleep_ms(long ms)
{
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
  nanosleep(&ts, nullptr);
}

static bool bytes_ok(int64_t off, const uint8_t *buf, int n)
{
  for (int i = 0; i < n; i++) {
    if (buf[i] != gen_byte(off + i)) return false;
  }
  return true;
}

typedef struct reader {
  ccord_media_stream *ms;
  int64_t off;
  int n, chunk;
  bool sequential;              // read to EOF in chunk-sized reads, verifying bytes
  atomic_int done;
  int rc;
  int64_t read_total;
  bool bytes_good;
  uint8_t *buf;
} reader;

static void *reader_main(void *p)
{
  reader *r = p;
  r->bytes_good = true;
  if (!r->sequential) {
    r->rc = ccord_media_stream_read_at(r->ms, r->off, r->buf, r->n);
    if (r->rc > 0) r->bytes_good = bytes_ok(r->off, r->buf, r->rc);
  } else {
    int64_t off = 0;
    for (;;) {
      int got = ccord_media_stream_read_at(r->ms, off, r->buf, r->chunk);
      if (got < 0) { r->rc = got; break; }
      if (got == 0) { r->rc = 0; break; }
      if (!bytes_ok(off, r->buf, got)) r->bytes_good = false;
      off += got;
    }
    r->read_total = off;
  }
  atomic_store(&r->done, 1);
  return nullptr;
}

static void reader_start(pthread_t *th, reader *r) { CT_CHECK(pthread_create(th, nullptr, reader_main, r) == 0); }

// ---------------------------------------------------------------------------------------------------------------

// One stream, one hub, one fake; the usual setup.
typedef struct rig { fake f; ccord_stream_hub *h; ccord_media_stream *ms; uint64_t now; } rig;

static bool rig_open(rig *r, int64_t total)
{
  fake_init(&r->f, total);
  r->now = 1000;
  r->h = ccord_stream_hub_create(&r->f.base);
  CT_CHECK(r->h != nullptr);
  if (!r->h) return false;
  ccord_error err;
  r->ms = ccord_stream_hub_open(r->h, URL, &err);
  CT_CHECK(r->ms != nullptr);
  return r->ms != nullptr;
}
static void rig_close(rig *r) { ccord_stream_hub_destroy(r->h); }

static void test_open_needs_ranged_transport(void)
{
  static const ccord_transport_vtable vt = {.name = "no-range", .destroy = fake_destroy};
  ccord_transport t = {.vt = &vt};
  ccord_stream_hub *h = ccord_stream_hub_create(&t);
  ccord_error err;
  CT_CHECK(ccord_stream_hub_open(h, URL, &err) == nullptr);
  CT_CHECK(err.code == CCORD_ERR_UNSUPPORTED);
  ccord_stream_hub_destroy(h);
}

// Reads are block aligned, no request exceeds 1 MiB, one range in flight, readahead stops 8 blocks past the read block.
static void test_readahead_window(void)
{
  rig r;
  if (!rig_open(&r, 40 * 1024 * 1024)) return;
  drive(&r.f, r.h, &r.now, nullptr, 40);
  ccord_media_stream_stats st;
  ccord_media_stream_stats_get(r.ms, &st);
  CT_CHECK(r.f.requests == 2);                  // probe 0..3, then 4..7; 8 alone waits for the read position to move
  CT_CHECK(st.resident_blocks == 8);
  CT_CHECK(r.f.max_block == 4);                 // last request started at block 4
  CT_CHECK(st.total == 40 * 1024 * 1024 && st.ranged);
  CT_CHECK(r.f.req.start == 4 * B && r.f.req.len == 4 * (size_t)B);
  CT_CHECK(st.log_count == 2 && st.log_start[0] == 0 && st.log_len[0] == 4 * (uint32_t)B);

  uint8_t buf[10];
  CT_CHECK(ccord_media_stream_read_at(r.ms, 3 * B, buf, 10) == 10);   // resident: no blocking
  CT_CHECK(bytes_ok(3 * B, buf, 10));
  drive(&r.f, r.h, &r.now, nullptr, 40);
  CT_CHECK(r.f.max_block == 8);                 // window [3, 11]: blocks 8..11
  CT_CHECK(r.f.requests == 3);
  CT_CHECK(ccord_media_stream_read_at(r.ms, 11 * B, buf, 10) == 10);
  drive(&r.f, r.h, &r.now, nullptr, 40);
  CT_CHECK(r.f.requests == 5);                  // 12..15 and 16..19; nothing beyond block 11 + 8
  CT_CHECK(r.f.max_block == 16);
  ccord_media_stream_stats_get(r.ms, &st);
  CT_CHECK(st.resident_blocks == 20 && st.evictions == 0);
  CT_CHECK(r.f.max_inflight == 1 && r.f.max_len == (int64_t)CCORD_STREAM_RANGE_MAX_BYTES && r.f.aligned);
  CT_CHECK(strstr(st.error_text, "ex=") == nullptr);
  rig_close(&r);
}

// A file larger than 32 MiB read end to end by a demux-like thread: right bytes, cache never above 64 blocks.
static void test_full_read_bounded(void)
{
  const int64_t total = 40 * 1024 * 1024 + 12345;
  rig r;
  if (!rig_open(&r, total)) return;
  reader rd = {.ms = r.ms, .sequential = true, .chunk = 100003};
  rd.buf = malloc(100003);
  pthread_t th;
  reader_start(&th, &rd);
  drive(&r.f, r.h, &r.now, &rd.done, 2000000);
  pthread_join(th, nullptr);
  CT_CHECK(atomic_load(&rd.done) == 1);
  CT_CHECK(rd.rc == 0 && rd.read_total == total && rd.bytes_good);
  ccord_media_stream_stats st;
  ccord_media_stream_stats_get(r.ms, &st);
  CT_CHECK(st.peak_resident_blocks <= CCORD_STREAM_CACHE_BLOCKS && st.peak_resident_blocks > 0);
  CT_CHECK(st.resident_blocks <= CCORD_STREAM_CACHE_BLOCKS);
  CT_CHECK(st.evictions > 0);
  CT_CHECK(st.bytes_fetched == (uint64_t)total);
  CT_CHECK(r.f.max_inflight == 1 && r.f.max_len <= (int64_t)CCORD_STREAM_RANGE_MAX_BYTES && r.f.aligned);
  CT_CHECK(st.error == CCORD_OK && ccord_media_stream_size(r.ms) == total);
  uint8_t b1[4];
  CT_CHECK(ccord_media_stream_read_at(r.ms, total, b1, 4) == 0);        // at EOF
  CT_CHECK(ccord_media_stream_read_at(r.ms, total + 99, b1, 4) == 0);   // past EOF
  free(rd.buf);
  rig_close(&r);
}

// A read blocked on another thread returns the right bytes, across a block boundary, once the transport delivers.
static void test_blocked_read(void)
{
  rig r;
  if (!rig_open(&r, 40 * 1024 * 1024)) return;
  reader rd = {.ms = r.ms, .off = 9 * B - 50, .n = 200};
  rd.buf = malloc(200);
  pthread_t th;
  reader_start(&th, &rd);
  sleep_ms(150);                                                 // nothing delivered yet
  CT_CHECK(atomic_load(&rd.done) == 0);
  drive(&r.f, r.h, &r.now, &rd.done, 400);
  pthread_join(th, nullptr);
  CT_CHECK(atomic_load(&rd.done) == 1 && rd.rc == 200 && rd.bytes_good);
  ccord_media_stream_stats st;
  ccord_media_stream_stats_get(r.ms, &st);
  CT_CHECK(st.log_count >= 2 && st.log_start[0] == 0);           // the probe first, then the demand ranges
  free(rd.buf);
  rig_close(&r);
}

static void test_eof_known_and_short_body(void)
{
  rig r;
  if (!rig_open(&r, 300000)) return;
  uint8_t buf[5000];
  reader rd = {.ms = r.ms, .off = B + 37000, .n = 5000, .buf = buf};
  pthread_t th;
  reader_start(&th, &rd);
  drive(&r.f, r.h, &r.now, &rd.done, 400);
  pthread_join(th, nullptr);
  CT_CHECK(rd.rc == 300000 - (B + 37000) && rd.bytes_good);       // short read at the end of the file
  CT_CHECK(ccord_media_stream_size(r.ms) == 300000);
  CT_CHECK(ccord_media_stream_read_at(r.ms, 300000, buf, 10) == 0);
  CT_CHECK(ccord_media_stream_read_at(r.ms, 1 << 30, buf, 10) == 0);
  CT_CHECK(ccord_media_stream_read_at(r.ms, 0, buf, 0) == 0);
  CT_CHECK(ccord_media_stream_read_at(r.ms, -1, buf, 10) < 0);
  rig_close(&r);

  // 206 without a known total: a short body is the end of the file.
  if (!rig_open(&r, 300000)) return;
  r.f.report_total = false;
  uint8_t b2[16];
  reader rd2 = {.ms = r.ms, .off = 299990, .n = 16, .buf = b2};
  reader_start(&th, &rd2);
  drive(&r.f, r.h, &r.now, &rd2.done, 400);
  pthread_join(th, nullptr);
  CT_CHECK(rd2.rc == 10 && rd2.bytes_good);
  CT_CHECK(ccord_media_stream_size(r.ms) == 300000);
  CT_CHECK(ccord_media_stream_read_at(r.ms, 300000, b2, 16) == 0);
  rig_close(&r);
}

static void test_server_ignores_range(void)
{
  rig r;
  if (!rig_open(&r, 5 * 1024 * 1024)) return;
  r.f.flags = CCORD_RANGE_UNRANGED;
  drive(&r.f, r.h, &r.now, nullptr, 30);
  ccord_media_stream_stats st;
  ccord_media_stream_stats_get(r.ms, &st);
  CT_CHECK(st.no_range && !st.ranged && st.error == CCORD_ERR_UNSUPPORTED);
  CT_CHECK(st.total == 5 * 1024 * 1024);                       // Content-Length survives for the UI's size decision
  CT_CHECK(r.f.requests == 1);
  uint8_t buf[8];
  CT_CHECK(ccord_media_stream_read_at(r.ms, 0, buf, 8) == -(int)CCORD_ERR_UNSUPPORTED);
  CT_CHECK(ccord_media_stream_read_at(r.ms, 6 * 1024 * 1024, buf, 8) == -(int)CCORD_ERR_UNSUPPORTED);
  rig_close(&r);
}

static void test_retry_and_permission(void)
{
  // 5xx twice, then fine: retried with backoff (250 ms, then 1 s) and the stream recovers.
  rig r;
  if (!rig_open(&r, 2 * 1024 * 1024)) return;
  r.f.fail_first = 2;
  r.f.fail_http = 503;
  ccord_stream_hub_pump(r.h, r.now);
  CT_CHECK(r.f.requests == 1);
  fake_deliver(&r.f, r.h, r.now);
  ccord_stream_hub_pump(r.h, r.now + 100);
  CT_CHECK(r.f.requests == 1);                                  // backoff not over
  ccord_stream_hub_pump(r.h, r.now + 250);
  CT_CHECK(r.f.requests == 2);
  fake_deliver(&r.f, r.h, r.now + 250);
  ccord_stream_hub_pump(r.h, r.now + 250 + 900);
  CT_CHECK(r.f.requests == 2);
  ccord_stream_hub_pump(r.h, r.now + 250 + 1000);
  CT_CHECK(r.f.requests == 3);
  CT_CHECK(r.f.req.start == 0 && r.f.req.len == 4 * (size_t)B);   // the same probe range each time
  rig_close(&r);

  // Persistent 5xx: three attempts, then the error latches and no more requests follow.
  if (!rig_open(&r, 2 * 1024 * 1024)) return;
  r.f.fail_first = 100;
  r.f.fail_http = 503;
  drive(&r.f, r.h, &r.now, nullptr, 40);
  ccord_media_stream_stats st;
  ccord_media_stream_stats_get(r.ms, &st);
  CT_CHECK(r.f.requests == 3 && st.retries == 2 && st.error == CCORD_ERR_IO && st.error_text[0]);
  uint8_t buf[4];
  CT_CHECK(ccord_media_stream_read_at(r.ms, 0, buf, 4) == -(int)CCORD_ERR_IO);
  rig_close(&r);

  // 403 mid-stream: latched at once (no retry storm); blocks already buffered stay readable.
  if (!rig_open(&r, 40 * 1024 * 1024)) return;
  r.f.fail_from = 2;
  r.f.fail_http = 403;
  drive(&r.f, r.h, &r.now, nullptr, 40);
  ccord_media_stream_stats_get(r.ms, &st);
  CT_CHECK(r.f.requests == 2 && st.error == CCORD_ERR_PERMISSION && strstr(st.error_text, "403"));
  CT_CHECK(ccord_media_stream_read_at(r.ms, 100, buf, 4) == 4 && bytes_ok(100, buf, 4));
  CT_CHECK(ccord_media_stream_read_at(r.ms, 6 * B, buf, 4) == -(int)CCORD_ERR_PERMISSION);
  drive(&r.f, r.h, &r.now, nullptr, 40);
  CT_CHECK(r.f.requests == 2);
  rig_close(&r);

  // The transport refuses the URL outright (the media gate): PERMISSION latches, no retry.
  if (!rig_open(&r, 1024)) return;
  r.f.refuse = CCORD_ERR_PERMISSION;
  drive(&r.f, r.h, &r.now, nullptr, 20);
  ccord_media_stream_stats_get(r.ms, &st);
  CT_CHECK(r.f.requests == 1 && st.error == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_media_stream_read_at(r.ms, 0, buf, 4) == -(int)CCORD_ERR_PERMISSION);
  rig_close(&r);
}

static void test_cancel_and_close(void)
{
  rig r;
  if (!rig_open(&r, 40 * 1024 * 1024)) return;
  uint8_t buf[64];
  reader rd = {.ms = r.ms, .off = 0, .n = 64, .buf = buf};
  pthread_t th;
  reader_start(&th, &rd);                                       // nothing is ever delivered
  sleep_ms(100);
  CT_CHECK(atomic_load(&rd.done) == 0);
  ccord_media_stream_cancel(r.ms);
  ccord_media_stream_cancel(r.ms);                              // idempotent
  pthread_join(th, nullptr);
  CT_CHECK(rd.rc == -(int)CCORD_ERR_IO);
  rig_close(&r);

  // Close while a range is pending: the late answer is dropped, and it never lands in another stream.
  if (!rig_open(&r, 40 * 1024 * 1024)) return;
  ccord_stream_hub_pump(r.h, r.now);
  CT_CHECK(r.f.pending);
  ccord_stream_hub_close(r.h, r.ms);
  ccord_error err;
  ccord_media_stream *ms2 = ccord_stream_hub_open(r.h, URL, &err);
  CT_CHECK(ms2 != nullptr);
  CT_CHECK(fake_deliver(&r.f, r.h, r.now + 10));                // late event for the closed stream
  ccord_media_stream_stats st;
  ccord_media_stream_stats_get(ms2, &st);
  CT_CHECK(st.bytes_fetched == 0 && st.resident_blocks == 0);
  drive(&r.f, r.h, &r.now, nullptr, 10);
  ccord_media_stream_stats_get(ms2, &st);
  CT_CHECK(st.bytes_fetched > 0 && st.error == CCORD_OK);       // the second stream works on its own
  ccord_stream_hub_close(r.h, nullptr);                         // safe
  rig_close(&r);
}

int main(void)
{
  test_open_needs_ranged_transport();
  test_readahead_window();
  test_full_read_bounded();
  test_blocked_read();
  test_eof_known_and_short_body();
  test_server_ignores_range();
  test_retry_and_permission();
  test_cancel_and_close();
  return CT_RESULT("test_media_stream");
}
