// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// RFC 6184 H.264 RTP payload tests: Annex-B splitting, single NAL / STAP-A / FU-A packetization and
// depacketization, round-trips, sequence wraparound, packet loss, malformed input and allocation failures.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/net/rtp_h264.h>

#include "ccord_test.h"

// ---------------------------------------------------- capture (emit sink) ----------------------------------------------------

#define MAX_PKTS 1024u
#define AU_CAP (256u * 1024u)

// Raw malloc/realloc on purpose: ccord_alloc_test_fail_after() must only hit the depacketizer, not the harness.
typedef struct {
  uint8_t *buf;
  size_t used, cap;
  size_t off[MAX_PKTS], len[MAX_PKTS];
  bool marker[MAX_PKTS];
  size_t n;
  size_t fail_at; // emit call index that returns false (SIZE_MAX = never)
} capture;

static void cap_init(capture *c)
{
  memset(c, 0, sizeof *c);
  c->fail_at = SIZE_MAX;
}

static void cap_free(capture *c)
{
  free(c->buf);
  c->buf = nullptr;
  c->used = c->cap = 0;
  c->n = 0;
}

static bool cap_emit(void *ud, const uint8_t *p, size_t n, bool marker)
{
  capture *c = ud;
  if (c->n == c->fail_at) return false;
  if (c->n >= MAX_PKTS) return false;
  if (c->used + n > c->cap) {
    size_t ncap = c->cap ? c->cap * 2 : 65536u;
    while (ncap < c->used + n) ncap *= 2;
    uint8_t *nb = realloc(c->buf, ncap);
    if (!nb) return false;
    c->buf = nb;
    c->cap = ncap;
  }
  memcpy(c->buf + c->used, p, n);
  c->off[c->n] = c->used;
  c->len[c->n] = n;
  c->marker[c->n] = marker;
  c->used += n;
  c->n++;
  return true;
}

static const uint8_t *cap_pkt(const capture *c, size_t i) { return c->buf + c->off[i]; }

// ---------------------------------------------------- Annex-B builders ----------------------------------------------------

static uint8_t g_au[AU_CAP]; // input access unit under construction
static size_t g_au_len;
static uint8_t g_exp[AU_CAP]; // expected depacketized Annex-B (4-byte start codes, no AUD / trailing zeros)
static size_t g_exp_len;
static uint8_t g_nal[AU_CAP]; // scratch NAL under construction

static void au_reset(void) { g_au_len = 0; }
static void exp_reset(void) { g_exp_len = 0; }

static void au_add(size_t sc_width, const uint8_t *nal, size_t n)
{
  CT_CHECK(g_au_len + sc_width + n <= AU_CAP);
  if (sc_width == 4) g_au[g_au_len++] = 0;
  g_au[g_au_len++] = 0;
  g_au[g_au_len++] = 0;
  g_au[g_au_len++] = 1;
  memcpy(g_au + g_au_len, nal, n);
  g_au_len += n;
}

static void au_zeros(size_t n) // trailing_zero_8bits between NALs / at the end
{
  CT_CHECK(g_au_len + n <= AU_CAP);
  for (size_t i = 0; i < n; i++) g_au[g_au_len++] = 0;
}

static void exp_add(const uint8_t *nal, size_t n)
{
  CT_CHECK(g_exp_len + 4 + n <= AU_CAP);
  g_exp[g_exp_len++] = 0;
  g_exp[g_exp_len++] = 0;
  g_exp[g_exp_len++] = 0;
  g_exp[g_exp_len++] = 1;
  memcpy(g_exp + g_exp_len, nal, n);
  g_exp_len += n;
}

// NAL of `len` bytes (header included); content is 1..251 so it contains no start code and no trailing zero.
static size_t make_nal(uint8_t hdr, size_t len, unsigned seed)
{
  CT_CHECK(len >= 1 && len <= AU_CAP);
  g_nal[0] = hdr;
  for (size_t i = 1; i < len; i++) g_nal[i] = (uint8_t)((i * 7u + seed) % 251u + 1u);
  return len;
}

// ---------------------------------------------------- depack feed helpers ----------------------------------------------------

typedef struct {
  uint8_t *b;
  size_t n;
  bool kf;
  uint32_t ts;
} delivery;

static void delivery_free(delivery *r)
{
  free(r->b);
  r->b = nullptr;
  r->n = 0;
}

// Feeds every captured packet with consecutive sequence numbers (wrapping at 65535) and one timestamp.
// Returns the number of access units delivered; the last delivery is copied into *r (when non-null).
static size_t feed_capture(ccord_rtp_h264_depack *d, const capture *c, uint16_t start_seq, uint32_t ts, delivery *r)
{
  size_t delivered = 0;
  for (size_t i = 0; i < c->n; i++) {
    const uint8_t *out = nullptr;
    size_t out_len = 0;
    bool kf = false;
    uint32_t ots = 0;
    bool ok = ccord_rtp_h264_depack_push(d, cap_pkt(c, i), c->len[i], (uint16_t)(start_seq + i), ts, c->marker[i],
                                         &out, &out_len, &kf, &ots);
    if (!ok) continue;
    delivered++;
    if (r) {
      free(r->b);
      r->b = malloc(out_len ? out_len : 1);
      CT_CHECK(r->b != nullptr);
      if (!r->b) return delivered;
      memcpy(r->b, out, out_len);
      r->n = out_len;
      r->kf = kf;
      r->ts = ots;
    }
  }
  return delivered;
}

// Structural invariants every emitted payload must satisfy (RFC 6184 packetization-mode=1).
static void check_shape(const uint8_t *p, size_t n)
{
  CT_CHECK(n >= 1);
  if (n == 0) return;
  unsigned t = p[0] & 0x1Fu;
  if (t >= 1 && t <= 23) return; // single NAL unit packet
  if (t == 24) {                 // STAP-A: 16-bit sizes, each NAL type 1..23
    CT_CHECK(n >= 4);
    unsigned f = 0, nri = 0;
    size_t pos = 1;
    while (pos < n) {
      CT_CHECK(n - pos >= 2);
      if (n - pos < 2) return;
      size_t sz = ((size_t)p[pos] << 8) | p[pos + 1];
      pos += 2;
      CT_CHECK(sz >= 1 && sz <= n - pos);
      if (sz == 0 || sz > n - pos) return;
      unsigned nt = p[pos] & 0x1Fu;
      CT_CHECK(nt >= 1 && nt <= 23);
      f |= p[pos] & 0x80u;
      if ((p[pos] & 0x60u) > nri) nri = p[pos] & 0x60u;
      pos += sz;
    }
    CT_CHECK(p[0] == (uint8_t)(f | nri | 24u)); // header F = OR of members, NRI = max of members
    return;
  }
  if (t == 28) { // FU-A: at least one payload byte, fragmented type 1..23
    CT_CHECK(n >= 3);
    if (n < 3) return;
    unsigned ft = p[1] & 0x1Fu;
    CT_CHECK(ft >= 1 && ft <= 23);
    return;
  }
  CT_CHECK(false); // the packetizer must never emit STAP-B / MTAP / FU-B / reserved types
}

// Packetizes g_au, checks payload shape / sizes / marker placement, depacketizes and compares with g_exp.
static void check_roundtrip(size_t max_payload, uint16_t start_seq, uint32_t ts, bool exp_kf)
{
  capture c;
  cap_init(&c);
  size_t npk = 0;
  ccord_error err = {0};
  ccord_status st = ccord_rtp_h264_packetize(g_au, g_au_len, max_payload, cap_emit, &c, &npk, &err);
  CT_CHECK(st == CCORD_OK);
  if (st != CCORD_OK) {
    cap_free(&c);
    return;
  }
  CT_CHECK(npk == c.n);
  CT_CHECK(c.n >= 1);
  size_t eff = max_payload > 16384u ? 16384u : max_payload;
  for (size_t i = 0; i < c.n; i++) {
    CT_CHECK(c.len[i] >= 1 && c.len[i] <= eff);
    CT_CHECK(c.marker[i] == (i + 1 == c.n)); // marker only on the last payload of the access unit
    check_shape(cap_pkt(&c, i), c.len[i]);
  }
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) {
    cap_free(&c);
    return;
  }
  delivery r = {0};
  size_t delivered = feed_capture(d, &c, start_seq, ts, &r);
  CT_CHECK(delivered == 1);
  CT_CHECK(r.n == g_exp_len);
  if (r.n == g_exp_len) CT_CHECK(memcmp(r.b, g_exp, g_exp_len) == 0);
  CT_CHECK(r.kf == exp_kf);
  CT_CHECK(r.ts == ts);
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 0);
  CT_CHECK(!ccord_rtp_h264_depack_needs_keyframe(d));
  delivery_free(&r);
  ccord_rtp_h264_depack_destroy(d);
  cap_free(&c);
}

// ---------------------------------------------------- packetizer: arguments ----------------------------------------------------

static void test_packetize_invalid_args(void)
{
  capture c;
  cap_init(&c);
  ccord_error err = {0};
  size_t npk = 123;
  au_reset();
  au_add(4, g_nal, make_nal(0x65, 100, 1));

  CT_CHECK(ccord_rtp_h264_packetize(nullptr, 10, 1100, cap_emit, &c, &npk, &err) == CCORD_ERR_INVALID);
  CT_CHECK(npk == 0);
  CT_CHECK(err.code == CCORD_ERR_INVALID);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, 0, 1100, cap_emit, &c, &npk, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, nullptr, &c, &npk, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 0, cap_emit, &c, &npk, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 63, cap_emit, &c, &npk, &err) == CCORD_ERR_INVALID);
  CT_CHECK(c.n == 0); // nothing emitted on any invalid call
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 64, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(npk == c.n && c.n >= 1);

  // No start code at all / start-code-only / delimiter-only inputs carry no NAL units.
  const uint8_t raw[5] = {1, 2, 3, 4, 5};
  const uint8_t sc3[3] = {0, 0, 1};
  const uint8_t sc4[4] = {0, 0, 0, 1};
  const uint8_t aud[6] = {0, 0, 1, 0x09, 0xF0, 0};
  CT_CHECK(ccord_rtp_h264_packetize(raw, sizeof raw, 1100, cap_emit, &c, &npk, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_rtp_h264_packetize(sc3, sizeof sc3, 1100, cap_emit, &c, &npk, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_rtp_h264_packetize(sc4, sizeof sc4, 1100, cap_emit, &c, &npk, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_rtp_h264_packetize(aud, sizeof aud, 1100, cap_emit, &c, &npk, &err) == CCORD_ERR_INVALID);
  CT_CHECK(err.code == CCORD_ERR_INVALID);

  // err and out_packets are nullable.
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, nullptr, nullptr) == CCORD_OK);
  cap_free(&c);
}

// ---------------------------------------------------- packetizer: single NAL ----------------------------------------------------

static void test_packetize_single_nal(void)
{
  capture c;
  cap_init(&c);
  size_t npk = 0;
  ccord_error err = {0};

  // One NAL, 4-byte start code: payload is the NAL itself, marker set.
  au_reset();
  size_t n = make_nal(0x65, 100, 1);
  au_add(4, g_nal, n);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(npk == 1 && c.n == 1);
  CT_CHECK(c.len[0] == n && memcmp(cap_pkt(&c, 0), g_nal, n) == 0);
  CT_CHECK(c.marker[0]);

  // 3-byte start code gives the identical payload.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  au_add(3, g_nal, n);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 1 && c.len[0] == n && memcmp(cap_pkt(&c, 0), g_nal, n) == 0);

  // NAL exactly max_payload stays single; one byte more fragments.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  n = make_nal(0x41, 100, 2);
  au_add(3, g_nal, n);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 1 && c.len[0] == 100);
  cap_free(&c);
  cap_init(&c);
  au_reset();
  n = make_nal(0x41, 101, 2);
  au_add(3, g_nal, n);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 2); // FU-A: 98 + 2 payload bytes
  CT_CHECK(c.len[0] == 100 && c.len[1] == 4);
  CT_CHECK((cap_pkt(&c, 0)[0] & 0x1Fu) == 28 && (cap_pkt(&c, 1)[0] & 0x1Fu) == 28);

  // Trailing zeros after the NAL are not part of it; leading zeros before the first start code are skipped.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  n = make_nal(0x65, 50, 3);
  au_zeros(2);
  au_add(3, g_nal, n);
  au_zeros(3);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 1 && c.len[0] == n && memcmp(cap_pkt(&c, 0), g_nal, n) == 0);

  // AUDs are dropped before and after the last real NAL; the marker still lands on the last payload.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  const uint8_t aud[2] = {0x09, 0xF0};
  au_add(4, aud, sizeof aud);
  n = make_nal(0x65, 60, 4);
  au_add(3, g_nal, n);
  au_add(4, aud, sizeof aud);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 1 && c.len[0] == n && c.marker[0]);

  // Empty NAL between start codes is skipped.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  au_add(3, g_nal, 0); // start code only
  n = make_nal(0x65, 20, 5);
  au_add(3, g_nal, n);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 1 && c.len[0] == n);

  // Several NALs too big to aggregate: one single-NAL packet each, marker only on the last.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  exp_reset();
  const size_t sizes[3] = {80, 90, 70};
  for (size_t i = 0; i < 3; i++) {
    size_t l = make_nal(i == 0 ? 0x67 : 0x41, sizes[i], (unsigned)i + 10);
    au_add(3, g_nal, l);
    exp_add(g_nal, l);
  }
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 3);
  for (size_t i = 0; i < 3; i++) {
    CT_CHECK(c.len[i] == sizes[i]);
    CT_CHECK(c.marker[i] == (i == 2));
  }
  check_roundtrip(100, 0, 90000, false);
  exp_reset();
  cap_free(&c);
}

// ---------------------------------------------------- packetizer: STAP-A ----------------------------------------------------

// NAL content containing 00 01 / 00 xx 01 / 00 00 02 byte sequences is NOT a start code and must not be split.
static void test_packetize_false_start_codes(void)
{
  static const uint8_t content[] = {0xAA, 0x00, 0x01, 0xBB, 0x00, 0x05, 0x01, 0xCC, 0x00,
                                    0x00, 0x02, 0xDD, 0x00, 0x00, 0x00, 0x02, 0xEE};
  au_reset();
  exp_reset();
  g_nal[0] = 0x65;
  memcpy(g_nal + 1, content, sizeof content);
  size_t n = sizeof content + 1;
  au_add(4, g_nal, n);
  exp_add(g_nal, n);
  check_roundtrip(1100, 0, 90001, true);
  check_roundtrip(64, 7, 90002, true); // also fragmented: the pattern spans FU-A chunks
  // A trailing start code with no NAL after it is not part of the last NAL's content.
  au_add(3, g_nal, 0);
  check_roundtrip(1100, 3, 90003, true);
  exp_reset();

  // A NAL ending in 00 00 02 right before the next start code: the scanner's multi-byte skip must not
  // jump over that code (skipping 4 bytes instead of 3 would land inside it and merge the two NALs).
  au_reset();
  exp_reset();
  const uint8_t tail2[] = {0x65, 0xAA, 0x00, 0x00, 0x02};
  au_add(4, tail2, sizeof tail2);
  exp_add(tail2, sizeof tail2);
  size_t nt = make_nal(0x41, 20, 14);
  au_add(3, g_nal, nt);
  exp_add(g_nal, nt);
  check_roundtrip(1100, 5, 90004, true);
  exp_reset();

  // A type-25 NAL (unsupported by the depacketizer) is passed through, not mistaken for an AUD.
  capture c;
  cap_init(&c);
  size_t npk = 0;
  ccord_error err = {0};
  au_reset();
  size_t n25 = make_nal(0x19, 30, 9);
  au_add(3, g_nal, n25);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 1 && c.len[0] == n25 && memcmp(cap_pkt(&c, 0), g_nal, n25) == 0);
  cap_free(&c);
}

static void test_packetize_stap_a(void)
{
  capture c;
  cap_init(&c);
  size_t npk = 0;
  ccord_error err = {0};

  // SPS + PPS aggregate into one STAP-A; header NRI is the max of the members.
  au_reset();
  size_t n1 = make_nal(0x67, 20, 1);
  uint8_t sps[20];
  memcpy(sps, g_nal, n1);
  size_t n2 = make_nal(0x68, 10, 2);
  uint8_t pps[10];
  memcpy(pps, g_nal, n2);
  au_add(4, sps, n1);
  au_add(3, pps, n2);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 1 && c.marker[0]);
  const uint8_t *p = cap_pkt(&c, 0);
  CT_CHECK(c.len[0] == 1 + 2 + n1 + 2 + n2);
  CT_CHECK(p[0] == (0x60u | 24u)); // NRI 3, type STAP-A
  CT_CHECK(p[1] == 0 && p[2] == 20);
  CT_CHECK(memcmp(p + 3, sps, n1) == 0);
  CT_CHECK(p[3 + n1] == 0 && p[4 + n1] == 10);
  CT_CHECK(memcmp(p + 5 + n1, pps, n2) == 0);

  // F is the OR of member F bits, NRI the max: 0x87 (F=1,NRI=0) + 0x68 (NRI=3) -> 0xF8.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  n1 = make_nal(0x87, 8, 3);
  memcpy(sps, g_nal, n1);
  n2 = make_nal(0x68, 8, 4);
  memcpy(pps, g_nal, n2);
  au_add(3, sps, n1);
  au_add(3, pps, n2);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 1 && cap_pkt(&c, 0)[0] == 0xF8u);

  // NRI is the max, not the last member's: 0x68 (NRI 3) + 0x27 (NRI 1) -> 0x60 | 24.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  n1 = make_nal(0x68, 8, 7);
  memcpy(sps, g_nal, n1);
  n2 = make_nal(0x27, 8, 8);
  memcpy(pps, g_nal, n2);
  au_add(3, sps, n1);
  au_add(3, pps, n2);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 1 && cap_pkt(&c, 0)[0] == 0x78u);

  // Aggregation boundary: 1 + 2 + 40 + 2 + 20 = 65 fits exactly at max_payload 65, not at 64.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  uint8_t sps2[64], pps2[64];
  n1 = make_nal(0x67, 40, 5);
  memcpy(sps2, g_nal, n1);
  n2 = make_nal(0x68, 20, 6);
  memcpy(pps2, g_nal, n2);
  au_add(3, sps2, n1);
  au_add(3, pps2, n2);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 65, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 1 && (cap_pkt(&c, 0)[0] & 0x1Fu) == 24);
  CT_CHECK(c.len[0] == 65);
  cap_free(&c);
  cap_init(&c);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 64, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 2); // one byte short: two single-NAL packets instead
  CT_CHECK(c.len[0] == 40 && c.len[1] == 20);
  CT_CHECK((cap_pkt(&c, 0)[0] & 0x1Fu) == 7 && (cap_pkt(&c, 1)[0] & 0x1Fu) == 8);

  // Three small NALs fit, a fourth big one does not: STAP-A then FU-A.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  exp_reset();
  for (size_t i = 0; i < 3; i++) {
    size_t l = make_nal(0x67, 10, (unsigned)i + 7);
    au_add(3, g_nal, l);
    exp_add(g_nal, l);
  }
  size_t big = make_nal(0x65, 100, 99);
  au_add(4, g_nal, big);
  exp_add(g_nal, big);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 64, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n >= 2);
  CT_CHECK((cap_pkt(&c, 0)[0] & 0x1Fu) == 24);
  CT_CHECK(c.len[0] == 1 + 3 * (2 + 10));
  CT_CHECK(!c.marker[0]);
  CT_CHECK((cap_pkt(&c, 1)[0] & 0x1Fu) == 28);
  CT_CHECK(c.marker[c.n - 1]);
  check_roundtrip(64, 100, 12345, true);

  // STAP-A members larger than 255 bytes: the 16-bit sizes must survive packetization and depacketization.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  exp_reset();
  n1 = make_nal(0x67, 300, 15);
  au_add(3, g_nal, n1);
  exp_add(g_nal, n1);
  n2 = make_nal(0x68, 200, 16);
  au_add(3, g_nal, n2);
  exp_add(g_nal, n2);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 1 && (cap_pkt(&c, 0)[0] & 0x1Fu) == 24);
  CT_CHECK(c.len[0] == 1 + 2 + 300 + 2 + 200);
  check_roundtrip(1100, 200, 90005, false);
  exp_reset();

  // STAP-A followed by a single NAL: the third NAL is too big to join the aggregate (35 + 2 + 1080 > 1100)
  // but still fits one packet; marker only on the last packet.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  exp_reset();
  n1 = make_nal(0x67, 20, 11);
  au_add(3, g_nal, n1);
  exp_add(g_nal, n1);
  n2 = make_nal(0x68, 10, 12);
  au_add(3, g_nal, n2);
  exp_add(g_nal, n2);
  big = make_nal(0x65, 1080, 13);
  au_add(4, g_nal, big);
  exp_add(g_nal, big);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 2);
  CT_CHECK((cap_pkt(&c, 0)[0] & 0x1Fu) == 24 && !c.marker[0]);
  CT_CHECK(c.len[1] == 1080 && c.marker[1]);
  check_roundtrip(1100, 0, 777, true);
  exp_reset();

  // Third NAL overshoots max_payload by exactly one byte: the aggregate must stop at two members.
  // total + 2 + l2 == max_payload + 1 must break; a +1 slack would emit a 101-byte packet at mp 100.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  exp_reset();
  uint8_t na[34], nb[34], nc[34];
  n1 = make_nal(0x67, 30, 21);
  memcpy(na, g_nal, n1);
  n2 = make_nal(0x68, 30, 22);
  memcpy(nb, g_nal, n2);
  size_t n3 = make_nal(0x65, 34, 23);
  memcpy(nc, g_nal, n3);
  au_add(3, na, n1);
  au_add(3, nb, n2);
  au_add(3, nc, n3);
  exp_add(na, n1);
  exp_add(nb, n2);
  exp_add(nc, n3);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 2);
  CT_CHECK((cap_pkt(&c, 0)[0] & 0x1Fu) == 24 && c.len[0] == 65 && !c.marker[0]);
  CT_CHECK(c.len[1] == 34 && c.marker[1]);
  check_roundtrip(100, 300, 4242, true);
  exp_reset();
  cap_free(&c);
}

// ---------------------------------------------------- packetizer: FU-A ----------------------------------------------------

static void test_packetize_fu_a(void)
{
  capture c;
  cap_init(&c);
  size_t npk = 0;
  ccord_error err = {0};

  // 300-byte IDR at max_payload 100: 299 payload bytes split 98/98/98/5, indicator keeps F/NRI.
  au_reset();
  exp_reset();
  size_t n = make_nal(0x65, 300, 21);
  au_add(4, g_nal, n);
  exp_add(g_nal, n);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 4);
  const size_t want_len[4] = {100, 100, 100, 7};
  const uint8_t want_fu[4] = {0x85, 0x05, 0x05, 0x45};
  size_t off = 1;
  for (size_t i = 0; i < 4; i++) {
    const uint8_t *p = cap_pkt(&c, i);
    CT_CHECK(c.len[i] == want_len[i]);
    CT_CHECK(p[0] == 0x7C); // (0x65 & 0xE0) | 28
    CT_CHECK(p[1] == want_fu[i]);
    CT_CHECK(memcmp(p + 2, g_nal + off, c.len[i] - 2) == 0);
    CT_CHECK(c.marker[i] == (i == 3));
    off += c.len[i] - 2;
  }
  check_roundtrip(100, 0, 4242, true);

  // F bit and NRI survive fragmentation and reassembly (0xE5 = F=1, NRI=3, IDR).
  cap_free(&c);
  cap_init(&c);
  au_reset();
  exp_reset();
  n = make_nal(0xE5, 250, 22);
  au_add(3, g_nal, n);
  exp_add(g_nal, n);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 3);
  for (size_t i = 0; i < c.n; i++) CT_CHECK(cap_pkt(&c, i)[0] == 0xFC); // 0xE0 | 28
  check_roundtrip(100, 0, 4243, true);

  // Exact fragment multiple: 196 payload bytes = 2 x 98 -> two fragments, E on the second.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  exp_reset();
  n = make_nal(0x65, 197, 23);
  au_add(3, g_nal, n);
  exp_add(g_nal, n);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 2);
  CT_CHECK(cap_pkt(&c, 0)[1] == 0x85 && cap_pkt(&c, 1)[1] == 0x45);
  CT_CHECK(!c.marker[0] && c.marker[1]);
  check_roundtrip(100, 0, 4244, true);

  // FU-A followed by a small NAL: the last fragment does NOT carry the marker, the trailing NAL does.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  exp_reset();
  n = make_nal(0x65, 300, 24);
  au_add(3, g_nal, n);
  exp_add(g_nal, n);
  size_t n2 = make_nal(0x41, 10, 25);
  au_add(3, g_nal, n2);
  exp_add(g_nal, n2);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 5);
  CT_CHECK(!c.marker[3]); // last FU-A fragment
  CT_CHECK(c.len[4] == 10 && c.marker[4]);
  check_roundtrip(100, 0, 4245, true);

  // Type 23 (the highest plain NAL type) fragments and reassembles with its type intact, not just its
  // low nibble. A leading SPS opens the fresh depacketizer's keyframe gate without making this a keyframe.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  exp_reset();
  size_t sps = make_nal(0x67, 10, 28);
  au_add(3, g_nal, sps);
  exp_add(g_nal, sps);
  n = make_nal(0x17, 300, 27);
  au_add(3, g_nal, n);
  exp_add(g_nal, n);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 5); // SPS single + 4 FU-A fragments (299 = 3 x 98 + 5)
  CT_CHECK(c.len[0] == 10 && (cap_pkt(&c, 0)[0] & 0x1Fu) == 7);
  CT_CHECK((cap_pkt(&c, 1)[1] & 0x1Fu) == 23);
  check_roundtrip(100, 0, 4247, false);
  exp_reset();

  // max_payload above the 16 KiB scratch cap is clamped, not overflowed.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  exp_reset();
  n = make_nal(0x65, 40000, 26);
  au_add(4, g_nal, n);
  exp_add(g_nal, n);
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100000, cap_emit, &c, &npk, &err) == CCORD_OK);
  CT_CHECK(c.n == 3); // 39999 = 16382 + 16382 + 7235
  CT_CHECK(c.len[0] == 16384 && c.len[1] == 16384 && c.len[2] == 7237);
  check_roundtrip(100000, 0, 4246, true);
  exp_reset();
  cap_free(&c);
}

static void test_packetize_emit_failure(void)
{
  capture c;
  cap_init(&c);
  size_t npk = 0;
  ccord_error err = {0};

  // Three single-NAL packets; aborting emit #k reports CCORD_ERR_IO and the count emitted so far.
  au_reset();
  for (size_t i = 0; i < 3; i++) {
    size_t l = make_nal(0x67, 100, (unsigned)i + 30);
    au_add(3, g_nal, l);
  }
  c.fail_at = 0;
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, &npk, &err) == CCORD_ERR_IO);
  CT_CHECK(npk == 0 && c.n == 0 && err.code == CCORD_ERR_IO);
  cap_free(&c);
  cap_init(&c);
  c.fail_at = 2;
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, &npk, &err) == CCORD_ERR_IO);
  CT_CHECK(npk == 2 && c.n == 2);

  // Failure inside a FU-A run counts the fragments already emitted.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  size_t n = make_nal(0x65, 300, 33);
  au_add(3, g_nal, n);
  c.fail_at = 2;
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, &npk, &err) == CCORD_ERR_IO);
  CT_CHECK(npk == 2 && c.n == 2);

  // Failure on a STAP-A emit.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  n = make_nal(0x67, 20, 34);
  au_add(3, g_nal, n);
  n = make_nal(0x68, 10, 35);
  au_add(3, g_nal, n);
  c.fail_at = 0;
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_ERR_IO);
  CT_CHECK(npk == 0 && c.n == 0);

  // Failure on the packet AFTER a STAP-A: the aggregate already emitted must be counted.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  n = make_nal(0x67, 20, 36);
  au_add(3, g_nal, n);
  n = make_nal(0x68, 10, 37);
  au_add(3, g_nal, n);
  n = make_nal(0x65, 1080, 38);
  au_add(4, g_nal, n);
  c.fail_at = 1;
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_ERR_IO);
  CT_CHECK(npk == 1 && c.n == 1);

  // Failure on a STAP-A that is NOT the first packet: a big single NAL is emitted first (count 1), then the
  // aggregate's emit fails. The STAP-A error path must still report the count emitted so far.
  cap_free(&c);
  cap_init(&c);
  au_reset();
  n = make_nal(0x65, 1080, 39);
  au_add(4, g_nal, n);
  n = make_nal(0x67, 20, 40);
  au_add(3, g_nal, n);
  n = make_nal(0x68, 10, 41);
  au_add(3, g_nal, n);
  c.fail_at = 1;
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, &npk, &err) == CCORD_ERR_IO);
  CT_CHECK(npk == 1 && c.n == 1);
  cap_free(&c);

  // The error paths tolerate a null out_packets too (FU-A, STAP-A and single-NAL emits).
  cap_init(&c);
  au_reset();
  n = make_nal(0x65, 300, 42);
  au_add(3, g_nal, n);
  c.fail_at = 0;
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, nullptr, &err) == CCORD_ERR_IO);
  cap_free(&c);
  cap_init(&c);
  au_reset();
  n = make_nal(0x67, 20, 43);
  au_add(3, g_nal, n);
  n = make_nal(0x68, 10, 44);
  au_add(3, g_nal, n);
  c.fail_at = 0;
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 1100, cap_emit, &c, nullptr, &err) == CCORD_ERR_IO);
  cap_free(&c);
  cap_init(&c);
  au_reset();
  n = make_nal(0x65, 100, 45);
  au_add(3, g_nal, n);
  c.fail_at = 0;
  CT_CHECK(ccord_rtp_h264_packetize(g_au, g_au_len, 100, cap_emit, &c, nullptr, &err) == CCORD_ERR_IO);
  cap_free(&c);
}

// ---------------------------------------------------- depacketizer: basics ----------------------------------------------------

static void test_depack_null_guards(void)
{
  CT_CHECK(ccord_rtp_h264_depack_push(nullptr, nullptr, 0, 0, 0, false, nullptr, nullptr, nullptr, nullptr) == false);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(nullptr));
  CT_CHECK(ccord_rtp_h264_depack_lost(nullptr) == 0);
  ccord_rtp_h264_depack_reset(nullptr);
  ccord_rtp_h264_depack_destroy(nullptr);
}

static void test_depack_single_nal(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 0);

  uint8_t sps[8], idr[12], pframe[6];
  make_nal(0x67, sizeof sps, 40);
  memcpy(sps, g_nal, sizeof sps);
  make_nal(0x65, sizeof idr, 41);
  memcpy(idr, g_nal, sizeof idr);
  make_nal(0x41, sizeof pframe, 42);
  memcpy(pframe, g_nal, sizeof pframe);

  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = true;
  uint32_t ots = 0;

  // SPS alone does not complete an access unit.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 0, 1000, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));
  // IDR with marker completes it: Annex-B with 4-byte start codes, keyframe flag, timestamp preserved.
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 1, 1000, true, &out, &out_len, &kf, &ots));
  CT_CHECK(out_len == 4 + sizeof sps + 4 + sizeof idr);
  CT_CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 1);
  CT_CHECK(memcmp(out + 4, sps, sizeof sps) == 0);
  CT_CHECK(out[4 + sizeof sps] == 0 && out[5 + sizeof sps] == 0);
  CT_CHECK(memcmp(out + 4 + sizeof sps + 4, idr, sizeof idr) == 0);
  CT_CHECK(kf);
  CT_CHECK(ots == 1000);
  CT_CHECK(!ccord_rtp_h264_depack_needs_keyframe(d));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 0);

  // A following P-frame is delivered without SPS/IDR and reports keyframe=false.
  CT_CHECK(ccord_rtp_h264_depack_push(d, pframe, sizeof pframe, 2, 2000, true, &out, &out_len, &kf, &ots));
  CT_CHECK(out_len == 4 + sizeof pframe && memcmp(out + 4, pframe, sizeof pframe) == 0);
  CT_CHECK(!kf && ots == 2000);

  // All out parameters are nullable.
  CT_CHECK(ccord_rtp_h264_depack_push(d, pframe, sizeof pframe, 3, 3000, true, nullptr, nullptr, nullptr, nullptr));

  // Type 23, the highest plain single-NAL type, is accepted (boundary of the 1..23 range).
  uint8_t t23[8];
  make_nal(0x17, sizeof t23, 43);
  memcpy(t23, g_nal, sizeof t23);
  CT_CHECK(ccord_rtp_h264_depack_push(d, t23, sizeof t23, 4, 4000, true, &out, &out_len, &kf, &ots));
  CT_CHECK(out_len == 4 + sizeof t23 && memcmp(out + 4, t23, sizeof t23) == 0);
  CT_CHECK(!kf && ots == 4000);
  ccord_rtp_h264_depack_destroy(d);
}

static void test_depack_stap_a(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;

  // Hand-built STAP-A: SPS(3 bytes) + PPS(2 bytes).
  const uint8_t stap[10] = {0x78, 0, 3, 0x67, 0xAA, 0xBB, 0, 2, 0x68, 0xCC};
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = true;
  uint32_t ots = 0;
  CT_CHECK(ccord_rtp_h264_depack_push(d, stap, sizeof stap, 0, 500, true, &out, &out_len, &kf, &ots));
  const uint8_t want[13] = {0, 0, 0, 1, 0x67, 0xAA, 0xBB, 0, 0, 0, 1, 0x68, 0xCC};
  CT_CHECK(out_len == sizeof want && memcmp(out, want, sizeof want) == 0);
  CT_CHECK(!kf); // SPS present, but no IDR slice
  CT_CHECK(ots == 500);
  CT_CHECK(!ccord_rtp_h264_depack_needs_keyframe(d)); // SPS satisfies the keyframe requirement

  // Minimal legal STAP-A: one 1-byte NAL.
  const uint8_t mini[4] = {0x78, 0, 1, 0x67};
  CT_CHECK(ccord_rtp_h264_depack_push(d, mini, sizeof mini, 1, 600, true, &out, &out_len, &kf, &ots));
  CT_CHECK(out_len == 5 && out[4] == 0x67);

  // STAP-A carrying an IDR sets the keyframe flag.
  const uint8_t stap_idr[7] = {0x78, 0, 4, 0x65, 1, 2, 3};
  CT_CHECK(ccord_rtp_h264_depack_push(d, stap_idr, sizeof stap_idr, 2, 700, true, &out, &out_len, &kf, &ots));
  CT_CHECK(kf && out_len == 8);

  // Plain types 16 and 23 are legal STAP-A members (the 1..23 range is not just the low nibble).
  const uint8_t stap_hi[9] = {0x78, 0, 2, 0x10, 0xAB, 0, 2, 0x17, 0xCD};
  CT_CHECK(ccord_rtp_h264_depack_push(d, stap_hi, sizeof stap_hi, 3, 800, true, &out, &out_len, &kf, &ots));
  CT_CHECK(out_len == 12 && out[4] == 0x10 && out[5] == 0xAB && out[10] == 0x17 && !kf);
  ccord_rtp_h264_depack_destroy(d);
}

static void test_depack_fu_a(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;

  // Hand-built FU-A fragmentation of 0x65 + {1,2,3,4,5}: indicator 0x7C, S/E bits across three packets.
  const uint8_t f0[5] = {0x7C, 0x85, 1, 2, 3};
  const uint8_t f1[4] = {0x7C, 0x05, 4, 5};
  const uint8_t f2[3] = {0x7C, 0x45, 6};
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;
  CT_CHECK(!ccord_rtp_h264_depack_push(d, f0, sizeof f0, 0, 900, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, f1, sizeof f1, 1, 900, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, f2, sizeof f2, 2, 900, true, &out, &out_len, &kf, &ots));
  const uint8_t want[11] = {0, 0, 0, 1, 0x65, 1, 2, 3, 4, 5, 6};
  CT_CHECK(out_len == sizeof want && memcmp(out, want, sizeof want) == 0);
  CT_CHECK(kf && ots == 900);
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 0);

  // Two-byte NAL via two minimal FU-A packets (one payload byte each).
  const uint8_t g0[3] = {0x7C, 0x87, 0xAA};
  const uint8_t g1[3] = {0x7C, 0x47, 0xBB};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, g0, sizeof g0, 3, 1000, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, g1, sizeof g1, 4, 1000, true, &out, &out_len, &kf, &ots));
  CT_CHECK(!kf); // SPS, not IDR
  CT_CHECK(out_len == 7 && memcmp(out + 4, "\x67\xAA\xBB", 3) == 0);
  ccord_rtp_h264_depack_destroy(d);
}

// ---------------------------------------------------- depacketizer: sequence / loss ----------------------------------------------------

static void test_depack_seq_wrap(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;
  uint8_t sps[4], idr[4], pf[4];
  make_nal(0x67, sizeof sps, 50);
  memcpy(sps, g_nal, sizeof sps);
  make_nal(0x65, sizeof idr, 51);
  memcpy(idr, g_nal, sizeof idr);
  make_nal(0x41, sizeof pf, 52);
  memcpy(pf, g_nal, sizeof pf);

  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;
  // One access unit spanning 65534 -> 65535 -> 0: no gap, no loss.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 65534, 500, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, idr, sizeof idr, 65535, 500, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, pf, sizeof pf, 0, 500, true, &out, &out_len, &kf, &ots));
  CT_CHECK(out_len == 4 + sizeof sps + 4 + sizeof idr + 4 + sizeof pf);
  CT_CHECK(kf && ots == 500);
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 0);

  // A gap right after the wrap (0 -> 2) is still detected.
  CT_CHECK(ccord_rtp_h264_depack_push(d, sps, sizeof sps, 2, 600, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 1);
  CT_CHECK(!kf && ots == 600); // SPS-only frame delivered after the between-frame loss
  ccord_rtp_h264_depack_destroy(d);
}

static void test_depack_loss_mid_fu(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;
  const uint8_t f0[5] = {0x7C, 0x85, 1, 2, 3};
  const uint8_t f2[3] = {0x7C, 0x45, 6};
  uint8_t sps[4], idr[4];
  make_nal(0x67, sizeof sps, 53);
  memcpy(sps, g_nal, sizeof sps);
  make_nal(0x65, sizeof idr, 54);
  memcpy(idr, g_nal, sizeof idr);
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;

  // Middle fragment lost: the reassembled AU is corrupt and must be discarded whole.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, f0, sizeof f0, 10, 100, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, f2, sizeof f2, 12, 100, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 1);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));

  // Recovery with a fresh SPS+IDR frame.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 13, 200, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 14, 200, true, &out, &out_len, &kf, &ots));
  CT_CHECK(kf && ots == 200);
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 1);
  CT_CHECK(!ccord_rtp_h264_depack_needs_keyframe(d));

  // A gap after an unfinished FU-A: the recovery SPS starts a fresh AU (fu_active must be cleared).
  CT_CHECK(!ccord_rtp_h264_depack_push(d, f0, sizeof f0, 15, 300, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, sps, sizeof sps, 20, 400, true, &out, &out_len, &kf, &ots));
  CT_CHECK(!kf && ots == 400);
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 2);
  ccord_rtp_h264_depack_destroy(d);
}

static void test_depack_loss_counts_au_not_packets(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;
  const uint8_t f0[3] = {0x7C, 0x85, 1};
  const uint8_t f1[3] = {0x7C, 0x05, 2};
  const uint8_t f2[3] = {0x7C, 0x45, 3};
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;
  // Two consecutive fragments lost: still exactly one discarded access unit.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, f0, sizeof f0, 0, 100, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, f1, sizeof f1, 3, 100, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, f2, sizeof f2, 4, 100, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 1);
  ccord_rtp_h264_depack_destroy(d);
}

static void test_depack_loss_between_frames(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;
  uint8_t sps[4], idr[4], pf[4];
  make_nal(0x67, sizeof sps, 55);
  memcpy(sps, g_nal, sizeof sps);
  make_nal(0x65, sizeof idr, 56);
  memcpy(idr, g_nal, sizeof idr);
  make_nal(0x41, sizeof pf, 57);
  memcpy(pf, g_nal, sizeof pf);
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;

  // Loss between frames, next frame opens with SPS: the new frame survives.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 0, 100, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 5, 200, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 6, 200, true, &out, &out_len, &kf, &ots));
  CT_CHECK(kf && ccord_rtp_h264_depack_lost(d) == 1);

  // Loss between frames, next frame opens with a single IDR: also the start of a frame, must survive.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 7, 300, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 12, 400, true, &out, &out_len, &kf, &ots));
  CT_CHECK(kf && ots == 400);
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 2);
  CT_CHECK(!ccord_rtp_h264_depack_needs_keyframe(d));

  // Loss between frames, next frame's SPS is itself fragmented (FU-A start): must survive too.
  const uint8_t fs[3] = {0x7C, 0x87, 0xAA};
  const uint8_t fe[3] = {0x7C, 0x47, 0xBB};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 13, 500, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fs, sizeof fs, 20, 600, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, fe, sizeof fe, 21, 600, true, &out, &out_len, &kf, &ots));
  CT_CHECK(!kf && ots == 600);
  CT_CHECK(out_len == 7 && memcmp(out + 4, "\x67\xAA\xBB", 3) == 0);
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 3);
  CT_CHECK(!ccord_rtp_h264_depack_needs_keyframe(d));

  // Loss inside a frame (same timestamp) poisons it even when the packet itself is valid.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 22, 700, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, idr, sizeof idr, 23, 700, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 25, 700, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 4);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));

  // A gap inside one timestamp poisons the AU even when the recovery packet opens with SPS or IDR.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 26, 800, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, idr, sizeof idr, 27, 800, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 29, 800, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 5);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 30, 900, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, idr, sizeof idr, 32, 900, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 6);

  // Different-timestamp gap recovered by a STAP-A: its first inner NAL (SPS) decides, the aggregate survives.
  const uint8_t stap[10] = {0x78, 0, 3, 0x67, 0xAA, 0xBB, 0, 2, 0x68, 0xCC};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 33, 1000, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, stap, sizeof stap, 40, 1100, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 41, 1100, true, &out, &out_len, &kf, &ots));
  CT_CHECK(kf && ots == 1100 && ccord_rtp_h264_depack_lost(d) == 7);
  CT_CHECK(out_len == 4 + 3 + 4 + 2 + 4 + sizeof idr);
  CT_CHECK(out[4] == 0x67 && out[5] == 0xAA && out[6] == 0xBB && out[11] == 0x68 && out[12] == 0xCC);

  // Minimal 4-byte STAP-A after a gap: the first-NAL lookup must accept n == 4.
  const uint8_t mini[4] = {0x78, 0, 1, 0x67};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 42, 1200, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, mini, sizeof mini, 50, 1300, true, &out, &out_len, &kf, &ots));
  CT_CHECK(out_len == 5 && out[4] == 0x67 && !kf && ccord_rtp_h264_depack_lost(d) == 8);

  // A truncated 3-byte STAP-A after a gap is malformed (and must not read past its end).
  const uint8_t st3[3] = {0x78, 0, 1};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 51, 1400, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, st3, sizeof st3, 60, 1500, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 9);

  // Different-timestamp gap whose recovery packet is a plain P-frame: the AU is poisoned. The keyframe
  // gate is open here, so only the gap heuristic (not the gate) can reject it.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 61, 1600, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 62, 1600, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 9);
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 63, 1700, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 65, 1800, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 10);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));
  // The gap re-armed the keyframe gate: a following P-only AU is gated, not delivered.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 66, 1900, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 10);

  // A gap whose timestamp matches the LAST DELIVERED AU is still between frames: `same` must require an
  // AU actually in progress, so an SPS opening a fresh AU at the same ts survives.
  CT_CHECK(ccord_rtp_h264_depack_push(d, sps, sizeof sps, 68, 1900, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 11);
  CT_CHECK(out_len == 8 && !kf && ots == 1900);
  CT_CHECK(!ccord_rtp_h264_depack_needs_keyframe(d));

  // A 1-byte SPS after a gap is a valid frame opener: first_nal_type must accept n == 1.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 69, 2000, false, &out, &out_len, &kf, &ots));
  const uint8_t one[1] = {0x67};
  CT_CHECK(ccord_rtp_h264_depack_push(d, one, sizeof one, 72, 2100, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 12);
  CT_CHECK(out_len == 5 && out[4] == 0x67 && !kf && ots == 2100);

  // A null payload after a gap must be rejected without dereferencing it in the gap heuristic.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 73, 2200, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, nullptr, 5, 80, 2300, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 13);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));

  // A 2-byte FU-A start after a gap is malformed: the loss is counted once (the gap), not twice.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 81, 2400, false, &out, &out_len, &kf, &ots));
  const uint8_t fu2[2] = {0x7C, 0x87};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu2, sizeof fu2, 85, 2500, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 14);

  // An FU-A end packet after a gap is not a frame opener either: still counted once.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 86, 2600, false, &out, &out_len, &kf, &ots));
  const uint8_t fe2[3] = {0x7C, 0x45, 6};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fe2, sizeof fe2, 90, 2700, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 15);

  // A zero-length payload after a gap must be rejected without reading its (possibly null) first byte.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 91, 2800, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, nullptr, 0, 95, 2900, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 16);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));

  // A non-null zero-length payload after a gap is malformed too: the gap heuristic must not read its first
  // byte and mistake an SPS header for a frame opener (the loss is counted once, not twice).
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 96, 3000, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, 0, 100, 3100, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 17);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));

  // Open the keyframe gate again for the next two probes.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 101, 3200, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 102, 3200, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 17);

  // A STAP-A whose first member is type 23 is NOT a frame opener: only types 5 and 7 recover a gap,
  // and the member type must be read with the full 5-bit mask (23 & 0x0F would masquerade as SPS).
  const uint8_t stap23[5] = {0x78, 0, 2, 0x17, 0xCD};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 103, 3300, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, stap23, sizeof stap23, 110, 3400, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 18);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));

  // Same for a FU-A start fragmenting type 23: not a frame opener, and the AU ends inside the fragment.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 111, 3500, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 112, 3500, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 18);
  const uint8_t fs23[3] = {0x7C, 0x97, 0xAA};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 113, 3600, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fs23, sizeof fs23, 120, 3700, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 19);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));

  // A STAP-A opening with type 23 but also carrying an IDR: the keyframe gate would pass it, so only
  // the full 5-bit first-member mask can reject it as a frame opener (23 & 0x0F masquerades as SPS).
  const uint8_t stap23i[8] = {0x78, 0, 2, 0x17, 0xCD, 0, 1, 0x65};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 121, 3800, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, stap23i, sizeof stap23i, 130, 3900, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 20);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));
  ccord_rtp_h264_depack_destroy(d);
}

static void test_depack_out_of_order_and_duplicates(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;
  uint8_t sps[4], idr[4], pf[4];
  make_nal(0x67, sizeof sps, 58);
  memcpy(sps, g_nal, sizeof sps);
  make_nal(0x65, sizeof idr, 59);
  memcpy(idr, g_nal, sizeof idr);
  make_nal(0x41, sizeof pf, 60);
  memcpy(pf, g_nal, sizeof pf);
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;

  CT_CHECK(ccord_rtp_h264_depack_push(d, sps, sizeof sps, 5, 100, true, &out, &out_len, &kf, &ots));
  // Late/duplicate packet (seq goes backwards): treated as a gap, P-frame AU discarded.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 3, 200, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 1);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));
  // The next in-order IDR recovers.
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 4, 300, true, &out, &out_len, &kf, &ots));
  CT_CHECK(kf && ccord_rtp_h264_depack_lost(d) == 1);

  // Exact duplicate of the last seq: gap detected, but an SPS opens a fresh AU and is delivered again.
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 4, 400, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 2);
  CT_CHECK(kf && ots == 400);
  ccord_rtp_h264_depack_destroy(d);
}

static void test_depack_ts_boundaries(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;
  uint8_t sps[4], idr[4], pf[4];
  make_nal(0x67, sizeof sps, 61);
  memcpy(sps, g_nal, sizeof sps);
  make_nal(0x65, sizeof idr, 62);
  memcpy(idr, g_nal, sizeof idr);
  make_nal(0x41, sizeof pf, 68);
  memcpy(pf, g_nal, sizeof pf);
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;

  // Timestamp change without a marker: the unfinished AU is dropped (one loss), the new one delivered.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 0, 100, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 1, 200, true, &out, &out_len, &kf, &ots));
  CT_CHECK(kf && ots == 200);
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 1);

  // A timestamp change after an already-discarded AU does not count a second loss.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 2, 300, false, &out, &out_len, &kf, &ots));
  const uint8_t badfu[3] = {0x7C, 0x05, 1}; // continuation without start
  CT_CHECK(!ccord_rtp_h264_depack_push(d, badfu, sizeof badfu, 3, 300, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 2);
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 4, 400, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 2);

  // 32-bit timestamp wraparound is preserved, not treated as a new AU mid-frame.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 5, 0xFFFFFFFFu, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 6, 0xFFFFFFFFu, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ots == 0xFFFFFFFFu);
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 7, 0, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ots == 0 && ccord_rtp_h264_depack_lost(d) == 2);

  // The timestamp-change loss re-arms the keyframe gate: a following P-only AU is gated, not delivered.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 8, 500, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 9, 600, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 3);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));

  // A timestamp change after a poisoned (marker-less) AU starts a clean AU: begin_au must clear au_bad,
  // and the already-counted loss is not counted again.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 10, 700, false, &out, &out_len, &kf, &ots));
  const uint8_t badfu2[3] = {0x7C, 0x05, 1};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, badfu2, sizeof badfu2, 11, 700, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 4);
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 12, 800, true, &out, &out_len, &kf, &ots));
  CT_CHECK(kf && ots == 800);
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 4);
  ccord_rtp_h264_depack_destroy(d);
}

static void test_depack_keyframe_gating(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;
  uint8_t sps[4], idr[4], pf[4];
  make_nal(0x67, sizeof sps, 63);
  memcpy(sps, g_nal, sizeof sps);
  make_nal(0x65, sizeof idr, 64);
  memcpy(idr, g_nal, sizeof idr);
  make_nal(0x41, sizeof pf, 65);
  memcpy(pf, g_nal, sizeof pf);
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;

  // P-frames before any SPS/IDR are silently dropped (not counted as loss).
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 0, 100, true, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 1, 200, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 0);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));
  // A type-23 NAL is not an SPS in disguise: the gate stays closed (23 & 0x0F would alias to type 7).
  uint8_t t23[4];
  make_nal(0x17, sizeof t23, 66);
  memcpy(t23, g_nal, sizeof t23);
  CT_CHECK(!ccord_rtp_h264_depack_push(d, t23, sizeof t23, 2, 250, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 0);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));
  // SPS+IDR unlocks delivery.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 3, 300, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 4, 300, true, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_needs_keyframe(d));
  CT_CHECK(ccord_rtp_h264_depack_push(d, pf, sizeof pf, 5, 400, true, &out, &out_len, &kf, &ots));
  CT_CHECK(!kf);
  ccord_rtp_h264_depack_destroy(d);
}

// ---------------------------------------------------- depacketizer: malformed input ----------------------------------------------------

// One malformed packet on a fresh AU: rejected, counted once, keyframe required again.
static void check_malformed(ccord_rtp_h264_depack *d, const uint8_t *p, size_t n)
{
  ccord_rtp_h264_depack_reset(d);
  uint64_t lost = ccord_rtp_h264_depack_lost(d);
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;
  CT_CHECK(!ccord_rtp_h264_depack_push(d, p, n, 0, 100, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == lost + 1);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));
}

static void test_depack_malformed(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;
  const uint8_t dummy[1] = {0};

  check_malformed(d, nullptr, 0);
  check_malformed(d, nullptr, 5);
  check_malformed(d, dummy, 0);

  // FU-A: too short, S+E together, forbidden fragmented types, continuation/end without start.
  const uint8_t fu_short1[1] = {0x7C};
  const uint8_t fu_short2[2] = {0x7C, 0x85};
  const uint8_t fu_se[3] = {0x7C, 0xC5, 1};
  const uint8_t fu_t0[3] = {0x7C, 0x80, 1};
  const uint8_t fu_t24[3] = {0x7C, 0x98, 1};
  const uint8_t fu_t28[3] = {0x7C, 0x9C, 1};
  const uint8_t fu_t31[3] = {0x7C, 0x9F, 1};
  const uint8_t fu_cont[3] = {0x7C, 0x05, 1};
  const uint8_t fu_end[3] = {0x7C, 0x45, 1};
  check_malformed(d, fu_short1, sizeof fu_short1);
  check_malformed(d, fu_short2, sizeof fu_short2);

  // A 2-byte FU-A start must not open a fragment: the following end packet cannot complete it, and the
  // loss is counted once. (Accepting n == 2 would reassemble and deliver a header-only NAL here.)
  ccord_rtp_h264_depack_reset(d);
  uint64_t l0 = ccord_rtp_h264_depack_lost(d);
  const uint8_t fu_end_x[3] = {0x7C, 0x45, 9};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_short2, sizeof fu_short2, 0, 100, false, nullptr, nullptr, nullptr, nullptr));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_end_x, sizeof fu_end_x, 1, 100, true, nullptr, nullptr, nullptr, nullptr));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == l0 + 1);

  // A FU-A start fragmenting type 24 is forbidden too: it must not assemble into a deliverable NAL.
  ccord_rtp_h264_depack_reset(d);
  l0 = ccord_rtp_h264_depack_lost(d);
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_t24, sizeof fu_t24, 0, 100, false, nullptr, nullptr, nullptr, nullptr));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_end_x, sizeof fu_end_x, 1, 100, true, nullptr, nullptr, nullptr, nullptr));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == l0 + 1);

  check_malformed(d, fu_se, sizeof fu_se);
  check_malformed(d, fu_t0, sizeof fu_t0);
  check_malformed(d, fu_t24, sizeof fu_t24);
  check_malformed(d, fu_t28, sizeof fu_t28);
  check_malformed(d, fu_t31, sizeof fu_t31);
  check_malformed(d, fu_cont, sizeof fu_cont);
  check_malformed(d, fu_end, sizeof fu_end);

  // STAP-A: too short, zero / oversized / truncated sizes, forbidden inner types.
  const uint8_t st_n2[2] = {0x78, 0};
  const uint8_t st_n3[3] = {0x78, 0, 1};
  const uint8_t st_over[4] = {0x78, 0, 5, 0x67};
  const uint8_t st_over1[6] = {0x78, 0, 4, 0x67, 1, 2}; // size exceeds the remainder by exactly one
  const uint8_t st_zero[4] = {0x78, 0, 0, 0x67};
  const uint8_t st_trunc[5] = {0x78, 0, 1, 0x67, 0};
  const uint8_t st_t0[4] = {0x78, 0, 1, 0x00};
  const uint8_t st_t24[4] = {0x78, 0, 1, 0x18};
  const uint8_t st_t28[4] = {0x78, 0, 1, 0x1C};
  const uint8_t st_t31[4] = {0x78, 0, 1, 0x1F};
  check_malformed(d, st_n2, sizeof st_n2);
  check_malformed(d, st_n3, sizeof st_n3);
  check_malformed(d, st_over, sizeof st_over);
  check_malformed(d, st_over1, sizeof st_over1);
  check_malformed(d, st_zero, sizeof st_zero);
  check_malformed(d, st_trunc, sizeof st_trunc);
  check_malformed(d, st_t0, sizeof st_t0);
  check_malformed(d, st_t24, sizeof st_t24);
  check_malformed(d, st_t28, sizeof st_t28);
  check_malformed(d, st_t31, sizeof st_t31);

  // A zero-size entry must be rejected even when the bytes after it would re-parse as a valid NAL
  // (size 0x0100 = 256 starting at the 0x01 byte, type 7, exactly filling the packet).
  static uint8_t st_zero_reparse[261];
  memset(st_zero_reparse, 0x5A, sizeof st_zero_reparse);
  st_zero_reparse[0] = 0x78;
  st_zero_reparse[1] = 0;
  st_zero_reparse[2] = 0;
  st_zero_reparse[3] = 0x01;
  st_zero_reparse[4] = 0x00;
  st_zero_reparse[5] = 0x67;
  check_malformed(d, st_zero_reparse, sizeof st_zero_reparse);

  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;

  // A second FU-A start while one is active is malformed.
  ccord_rtp_h264_depack_reset(d);
  uint64_t lost = ccord_rtp_h264_depack_lost(d);
  const uint8_t fu_start[3] = {0x7C, 0x85, 1};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_start, sizeof fu_start, 0, 100, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == lost);
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_start, sizeof fu_start, 1, 100, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == lost + 1);

  // A marker on the FU-A start packet itself: the AU ends inside a fragmented NAL and is discarded.
  ccord_rtp_h264_depack_reset(d);
  lost = ccord_rtp_h264_depack_lost(d);
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_start, sizeof fu_start, 0, 100, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == lost + 1);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));

  // An access unit that ends (marker) inside a fragmented NAL is discarded.
  ccord_rtp_h264_depack_reset(d);
  lost = ccord_rtp_h264_depack_lost(d);
  uint8_t idr[4];
  make_nal(0x65, sizeof idr, 66);
  memcpy(idr, g_nal, sizeof idr);
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_start, sizeof fu_start, 0, 100, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, idr, sizeof idr, 1, 100, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == lost + 1);

  // A malformed packet poisons the rest of its access unit, even valid packets.
  ccord_rtp_h264_depack_reset(d);
  lost = ccord_rtp_h264_depack_lost(d);
  uint8_t sps[4];
  make_nal(0x67, sizeof sps, 67);
  memcpy(sps, g_nal, sizeof sps);
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 0, 100, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_cont, sizeof fu_cont, 1, 100, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == lost + 1);
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_cont, sizeof fu_cont, 2, 100, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == lost + 1); // a second malformed packet does not count again
  CT_CHECK(!ccord_rtp_h264_depack_push(d, idr, sizeof idr, 3, 100, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == lost + 1); // not counted twice
  // Next frame is clean again.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 4, 200, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 5, 200, true, &out, &out_len, &kf, &ots));

  // Unsupported but well-formed types (0, STAP-B 25, FU-B 29) are ignored, not counted as loss. The
  // keyframe gate is opened first so an ignored type cannot hide behind it (it must not be appended).
  ccord_rtp_h264_depack_reset(d);
  lost = ccord_rtp_h264_depack_lost(d);
  const uint8_t t0[3] = {0x00, 1, 2};
  const uint8_t t25[2] = {0x19, 1};
  const uint8_t t29[2] = {0x1D, 1};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 0, 50, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 1, 50, true, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, t0, sizeof t0, 2, 100, true, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, t25, sizeof t25, 3, 200, true, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, t29, sizeof t29, 4, 300, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == lost);
  // ... but they do not silently terminate an active FU-A either.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_start, sizeof fu_start, 5, 400, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, t25, sizeof t25, 6, 400, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == lost + 1);

  // A plain NAL between FU-A start and end is malformed: the fragment never finished.
  ccord_rtp_h264_depack_reset(d);
  lost = ccord_rtp_h264_depack_lost(d);
  const uint8_t fu_end_ok[3] = {0x7C, 0x45, 2};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_start, sizeof fu_start, 0, 500, false, &out, &out_len, &kf, &ots));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, idr, sizeof idr, 1, 500, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == lost + 1);
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_end_ok, sizeof fu_end_ok, 2, 500, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == lost + 1);
  ccord_rtp_h264_depack_destroy(d);
}

// ---------------------------------------------------- depacketizer: limits / OOM / reset ----------------------------------------------------

static void test_depack_au_cap(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;
  uint8_t sps[4], idr[4];
  make_nal(0x67, sizeof sps, 70);
  memcpy(sps, g_nal, sizeof sps);
  make_nal(0x65, sizeof idr, 71);
  memcpy(idr, g_nal, sizeof idr);
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 0, 100, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 1, 100, true, &out, &out_len, &kf, &ots));

  // One access unit larger than the 8 MiB reassembly cap: rejected without overflowing.
  static uint8_t big[65536];
  memset(big, 0x5A, sizeof big);
  big[0] = 0x41; // non-IDR slice
  bool rejected = false;
  for (size_t i = 0; i < 130; i++) {
    bool marker = i == 129;
    bool ok = ccord_rtp_h264_depack_push(d, big, sizeof big, (uint16_t)(2 + i), 300, marker, &out, &out_len, &kf, &ots);
    CT_CHECK(!ok);
    if (ccord_rtp_h264_depack_lost(d) > 0) rejected = true;
  }
  CT_CHECK(rejected);
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 1);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));
  // Still usable afterwards.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 132, 400, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 133, 400, true, &out, &out_len, &kf, &ots));
  CT_CHECK(kf);
  ccord_rtp_h264_depack_destroy(d);
}

// The 8 MiB reassembly cap is exact: an AU of precisely DEPACK_MAX_AU bytes is delivered, one byte more
// is rejected, and a single NAL larger than the cap is refused before any copy happens.
static void test_depack_au_cap_exact(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;

  // 128 NALs x (4-byte start code + 65532 bytes) = exactly 8 MiB.
  static uint8_t big[65536];
  memset(big, 0x5A, sizeof big);
  for (unsigned i = 0; i < 128u; i++) {
    big[0] = i == 0 ? 0x67 : 0x41; // SPS first so the keyframe gate opens
    bool marker = i == 127u;
    bool ok = ccord_rtp_h264_depack_push(d, big, 65532u, (uint16_t)i, 100, marker, &out, &out_len, &kf, &ots);
    CT_CHECK(ok == marker);
  }
  CT_CHECK(out_len == 8u * 1024u * 1024u);
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 0);
  CT_CHECK(!kf && ots == 100);

  // A single NAL of exactly DEPACK_MAX_AU - 4 bytes also fits: a reservation of exactly the cap is legal.
  ccord_rtp_h264_depack_reset(d);
  static uint8_t one[8u * 1024u * 1024u - 4u];
  memset(one, 0x5A, sizeof one);
  one[0] = 0x67;
  CT_CHECK(ccord_rtp_h264_depack_push(d, one, sizeof one, 200, 150, true, &out, &out_len, &kf, &ots));
  CT_CHECK(out_len == 8u * 1024u * 1024u);
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 0);

  // Fresh depacketizer: a 65528-byte NAL reserves exactly DEPACK_INITIAL_CAP (65536), then a 5-byte
  // append must grow past it. A boundary check that admits need == cap + 1 writes one byte past the
  // allocation (caught by ASan).
  ccord_rtp_h264_depack *d2 = ccord_rtp_h264_depack_create();
  CT_CHECK(d2 != nullptr);
  if (d2) {
    static uint8_t b1[65528];
    memset(b1, 0x5A, sizeof b1);
    b1[0] = 0x67; // SPS opens the keyframe gate
    CT_CHECK(!ccord_rtp_h264_depack_push(d2, b1, sizeof b1, 300, 250, false, &out, &out_len, &kf, &ots));
    static const uint8_t b2[1] = {0x65}; // IDR
    CT_CHECK(ccord_rtp_h264_depack_push(d2, b2, sizeof b2, 301, 250, true, &out, &out_len, &kf, &ots));
    CT_CHECK(out_len == 65537u); // (4 + 65528) + (4 + 1)
    CT_CHECK(ccord_rtp_h264_depack_lost(d2) == 0);
    ccord_rtp_h264_depack_destroy(d2);
  }

  // A single NAL larger than twice the initial capacity: the buffer must grow through several doublings
  // (64K -> 128K -> 256K) in one reservation, not just one.
  ccord_rtp_h264_depack *d3 = ccord_rtp_h264_depack_create();
  CT_CHECK(d3 != nullptr);
  if (d3) {
    static uint8_t b3[200000];
    memset(b3, 0x5A, sizeof b3);
    b3[0] = 0x67; // SPS opens the keyframe gate
    CT_CHECK(ccord_rtp_h264_depack_push(d3, b3, sizeof b3, 400, 350, true, &out, &out_len, &kf, &ots));
    CT_CHECK(out_len == 4u + sizeof b3);
    CT_CHECK(ccord_rtp_h264_depack_lost(d3) == 0);
    ccord_rtp_h264_depack_destroy(d3);
  }

  // One byte over the cap: the whole AU is rejected and counted lost once.
  ccord_rtp_h264_depack_reset(d);
  for (unsigned i = 0; i < 128u; i++) {
    big[0] = i == 0 ? 0x67 : 0x41;
    size_t n = i == 127u ? 65533u : 65532u;
    bool marker = i == 127u;
    CT_CHECK(!ccord_rtp_h264_depack_push(d, big, n, (uint16_t)(128u + i), 200, marker, &out, &out_len, &kf, &ots));
  }
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 1);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));

  // A single NAL larger than the cap: rejected up front, no oversized copy.
  ccord_rtp_h264_depack_reset(d);
  static uint8_t huge[9u * 1024u * 1024u];
  memset(huge, 0x5A, sizeof huge);
  huge[0] = 0x67;
  CT_CHECK(!ccord_rtp_h264_depack_push(d, huge, sizeof huge, 256, 300, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 2);
  ccord_rtp_h264_depack_destroy(d);
}

// FU-A reservations must cover the reconstructed header byte (start) and the full fragment payload
// (continuation) even when the reservation lands exactly on the current capacity: an off-by-one there
// writes one byte past the allocation (caught by ASan).
static void test_depack_fu_reserve(void)
{
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;
  const uint8_t fs[3] = {0x7C, 0x85, 1}; // FU-A start, IDR, 1 payload byte
  const uint8_t fc[3] = {0x7C, 0x05, 2}; // continuation, 1 payload byte
  const uint8_t fe[3] = {0x7C, 0x45, 3}; // end, 1 payload byte

  // len == cap - 5 before a 3-byte start: reserving 4 + (n - 2) instead of 5 + (n - 2) fits the check
  // but the 6-byte write (start code + header + payload) runs one byte past the buffer.
  ccord_rtp_h264_depack *da = ccord_rtp_h264_depack_create();
  CT_CHECK(da != nullptr);
  if (da) {
    static uint8_t sa[65527];
    memset(sa, 0x5A, sizeof sa);
    sa[0] = 0x67; // SPS opens the keyframe gate; len becomes 65531, cap 65536
    CT_CHECK(!ccord_rtp_h264_depack_push(da, sa, sizeof sa, 0, 100, false, &out, &out_len, &kf, &ots));
    CT_CHECK(!ccord_rtp_h264_depack_push(da, fs, sizeof fs, 1, 100, false, &out, &out_len, &kf, &ots));
    CT_CHECK(ccord_rtp_h264_depack_push(da, fe, sizeof fe, 2, 100, true, &out, &out_len, &kf, &ots));
    CT_CHECK(out_len == 65538u && kf);
    CT_CHECK(ccord_rtp_h264_depack_lost(da) == 0);
    ccord_rtp_h264_depack_destroy(da);
  }

  // len == cap before a 3-byte continuation: reserving n - 3 instead of n - 2 fits the check but the
  // 1-byte write lands exactly one byte past the buffer.
  ccord_rtp_h264_depack *db = ccord_rtp_h264_depack_create();
  CT_CHECK(db != nullptr);
  if (db) {
    static uint8_t sb[65526];
    memset(sb, 0x5A, sizeof sb);
    sb[0] = 0x67; // len becomes 65530, cap 65536
    CT_CHECK(!ccord_rtp_h264_depack_push(db, sb, sizeof sb, 0, 200, false, &out, &out_len, &kf, &ots));
    CT_CHECK(!ccord_rtp_h264_depack_push(db, fs, sizeof fs, 1, 200, false, &out, &out_len, &kf, &ots));
    CT_CHECK(!ccord_rtp_h264_depack_push(db, fc, sizeof fc, 2, 200, false, &out, &out_len, &kf, &ots));
    CT_CHECK(ccord_rtp_h264_depack_push(db, fe, sizeof fe, 3, 200, true, &out, &out_len, &kf, &ots));
    CT_CHECK(out_len == 65538u && kf);
    CT_CHECK(ccord_rtp_h264_depack_lost(db) == 0);
    ccord_rtp_h264_depack_destroy(db);
  }
}

static void test_depack_oom(void)
{
  // create() allocation failure.
  ccord_alloc_test_fail_after(0);
  CT_CHECK(ccord_rtp_h264_depack_create() == nullptr);
  ccord_alloc_test_fail_after(-1);

  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;
  uint8_t sps[4], idr[4];
  make_nal(0x67, sizeof sps, 72);
  memcpy(sps, g_nal, sizeof sps);
  make_nal(0x65, sizeof idr, 73);
  memcpy(idr, g_nal, sizeof idr);
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;

  // Buffer growth failure on a single NAL: packet dropped, AU counted lost, recovery still works.
  ccord_alloc_test_fail_after(0);
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 0, 100, true, &out, &out_len, &kf, &ots));
  ccord_alloc_test_fail_after(-1);
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 1);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 1, 200, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 2, 200, true, &out, &out_len, &kf, &ots));

  ccord_rtp_h264_depack_destroy(d);

  // Growth failure on a FU-A start: a fresh depacketizer (cap 0) must actually allocate, so the
  // injected failure hits reserve() instead of being skipped by an already-grown buffer.
  ccord_rtp_h264_depack *df = ccord_rtp_h264_depack_create();
  CT_CHECK(df != nullptr);
  if (df) {
    const uint8_t fu_start[3] = {0x7C, 0x85, 1}; // FU-A start, IDR fragment
    ccord_alloc_test_fail_after(0);
    CT_CHECK(!ccord_rtp_h264_depack_push(df, fu_start, sizeof fu_start, 0, 300, false, &out, &out_len, &kf, &ots));
    ccord_alloc_test_fail_after(-1);
    CT_CHECK(ccord_rtp_h264_depack_lost(df) == 1);
    CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(df));
    ccord_rtp_h264_depack_destroy(df);
  }

  // Growth failure inside a STAP-A: the first aggregated NAL fails to append, the whole AU is dropped.
  ccord_rtp_h264_depack *ds = ccord_rtp_h264_depack_create();
  CT_CHECK(ds != nullptr);
  if (ds) {
    const uint8_t stap[10] = {0x78, 0, 3, 0x67, 0xAA, 0xBB, 0, 2, 0x68, 0xCC}; // STAP-A: SPS(3) + PPS(2)
    ccord_alloc_test_fail_after(0);
    CT_CHECK(!ccord_rtp_h264_depack_push(ds, stap, sizeof stap, 4, 400, true, &out, &out_len, &kf, &ots));
    ccord_alloc_test_fail_after(-1);
    CT_CHECK(ccord_rtp_h264_depack_lost(ds) == 1);
    CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(ds));
    ccord_rtp_h264_depack_destroy(ds);
  }
}

static void test_depack_reset(void)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  CT_CHECK(d != nullptr);
  if (!d) return;
  uint8_t sps[4], idr[4], pf[4];
  make_nal(0x67, sizeof sps, 74);
  memcpy(sps, g_nal, sizeof sps);
  make_nal(0x65, sizeof idr, 75);
  memcpy(idr, g_nal, sizeof idr);
  make_nal(0x41, sizeof pf, 76);
  memcpy(pf, g_nal, sizeof pf);
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool kf = false;
  uint32_t ots = 0;

  // Partial AU + reset: state dropped, keyframe required again, loss counter is cumulative.
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 0, 100, false, &out, &out_len, &kf, &ots));
  ccord_rtp_h264_depack_reset(d);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 1, 200, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 0); // gated, not lost
  CT_CHECK(!ccord_rtp_h264_depack_push(d, sps, sizeof sps, 2, 300, false, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_push(d, idr, sizeof idr, 3, 300, true, &out, &out_len, &kf, &ots));

  // Reset after a delivery re-arms the keyframe gate: P-only frames are gated again, not delivered.
  ccord_rtp_h264_depack_reset(d);
  CT_CHECK(ccord_rtp_h264_depack_needs_keyframe(d));
  CT_CHECK(!ccord_rtp_h264_depack_push(d, pf, sizeof pf, 4, 350, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 0); // gated, not lost

  // Reset mid-FU-A: the continuation afterwards has no start and is rejected.
  const uint8_t fu_start[3] = {0x7C, 0x85, 1};
  const uint8_t fu_cont[3] = {0x7C, 0x05, 2};
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_start, sizeof fu_start, 5, 400, false, &out, &out_len, &kf, &ots));
  ccord_rtp_h264_depack_reset(d);
  CT_CHECK(!ccord_rtp_h264_depack_push(d, fu_cont, sizeof fu_cont, 6, 500, true, &out, &out_len, &kf, &ots));
  CT_CHECK(ccord_rtp_h264_depack_lost(d) == 1);
  ccord_rtp_h264_depack_destroy(d);
}

static void test_large_roundtrip(void)
{
  // ~200 KB IDR: exercises repeated buffer growth (64K -> 128K -> 256K) and many FU-A fragments.
  au_reset();
  exp_reset();
  size_t n = make_nal(0x65, 200000, 80);
  au_add(4, g_nal, n);
  exp_add(g_nal, n);
  check_roundtrip(1200, 65500, 0xDEADBEEFu, true); // sequence wraps mid-frame
  exp_reset();
}

// ---------------------------------------------------- randomized round-trips ----------------------------------------------------

static uint32_t rng_state;

static uint32_t rnd(void) // xorshift32: deterministic, seed fixed below
{
  uint32_t x = rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state = x;
  return x;
}

// Random access units: mixed NAL types / sizes / start-code widths, AUDs, leading / interior / trailing zeros,
// random max_payload (incl. > PKT_CAP), random start sequence (incl. wrap) and timestamp. Every AU starts with
// SPS or IDR so the fresh depacketizer's keyframe gate opens.
static void test_random_roundtrips(void)
{
  static const unsigned types[] = {1, 5, 6, 7, 8, 12, 23};
  rng_state = 0xC0FFEEu;
  for (unsigned iter = 0; iter < 400u; iter++) {
    au_reset();
    exp_reset();
    size_t mp = 64u + rnd() % 1400u;
    if (rnd() % 25u == 0) mp = 20000u; // above PKT_CAP: packetizer must clamp
    size_t budget = mp < 256u ? 40000u : 150000u; // keep the packet count below MAX_PKTS
    if (rnd() % 4u == 0) au_zeros(rnd() % 4u);    // leading_zero_8bits before the first start code
    unsigned count = 1u + rnd() % 8u;
    bool exp_kf = false;
    for (unsigned k = 0; k < count && budget > 16u; k++) {
      if (k > 0 && rnd() % 8u == 0) { // access-unit delimiter: dropped by the packetizer
        uint8_t aud = 0x09;
        au_add(rnd() % 2u ? 3u : 4u, &aud, 1);
        continue;
      }
      unsigned type = k == 0 ? (rnd() % 2u ? 7u : 5u) : types[rnd() % 7u];
      size_t maxlen = budget - 8u;
      size_t len = 1u + rnd() % (maxlen < 3000u ? maxlen : 3000u);
      if (rnd() % 10u == 0 && maxlen > 6000u) len = 3000u + rnd() % (maxlen - 3000u);
      uint8_t hdr = (uint8_t)(((rnd() % 4u) << 5) | type);
      if (rnd() % 16u == 0) hdr |= 0x80u; // F bit set: must survive STAP-A aggregation unchanged
      g_nal[0] = hdr;
      bool prev_zero = false;
      for (size_t i = 1; i < len; i++) {
        uint32_t r = rnd();
        // Isolated zero bytes are legal NAL content; never two in a row (fake start code), never trailing
        // (the packetizer strips trailing_zero_8bits).
        bool zero = !prev_zero && i + 1 < len && r % 32u == 0;
        g_nal[i] = (uint8_t)(zero ? 0u : (r % 255u + 1u));
        prev_zero = zero;
      }
      au_add(rnd() % 2u ? 3u : 4u, g_nal, len);
      exp_add(g_nal, len);
      if (type == 5u) exp_kf = true;
      budget -= len + 4u;
      if (rnd() % 6u == 0) au_zeros(1u + rnd() % 3u); // trailing_zero_8bits between NALs
    }
    if (rnd() % 3u == 0) au_zeros(rnd() % 5u); // trailing zeros after the last NAL
    CT_CHECK(g_au_len > 0);
    check_roundtrip(mp, (uint16_t)(rnd() & 0xFFFFu), rnd(), exp_kf);
  }
  exp_reset();
}

static void test_constants(void)
{
  CT_CHECK(CCORD_RTP_H264_PT == 101);
  CT_CHECK(CCORD_RTP_H264_RTX_PT == 102);
  CT_CHECK(CCORD_RTP_CLOCK_HZ == 90000u);
  CT_CHECK(CCORD_RTP_DEFAULT_MAX_PAYLOAD >= 64u);
  CT_CHECK(CCORD_RTP_DEFAULT_MAX_PAYLOAD <= 16384u);
}

int main(void)
{
  test_constants();
  test_packetize_invalid_args();
  test_packetize_single_nal();
  test_packetize_false_start_codes();
  test_packetize_stap_a();
  test_packetize_fu_a();
  test_packetize_emit_failure();
  test_depack_null_guards();
  test_depack_single_nal();
  test_depack_stap_a();
  test_depack_fu_a();
  test_depack_seq_wrap();
  test_depack_loss_mid_fu();
  test_depack_loss_counts_au_not_packets();
  test_depack_loss_between_frames();
  test_depack_out_of_order_and_duplicates();
  test_depack_ts_boundaries();
  test_depack_keyframe_gating();
  test_depack_malformed();
  test_depack_au_cap();
  test_depack_au_cap_exact();
  test_depack_fu_reserve();
  test_depack_oom();
  test_depack_reset();
  test_large_roundtrip();
  test_random_roundtrips();
  return CT_RESULT("test_rtp_h264");
}
