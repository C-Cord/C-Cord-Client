// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Fuzz harness for the RFC 6184 H.264 packetizer / depacketizer (rtp_h264.c). Two directions:
//  1. packetize -> emit callback -> depack roundtrip. The emit callback feeds each RTP payload straight
//     into a depacketizer; a completed access unit must begin with a 4-byte Annex-B start code, and its
//     bytes are folded into a volatile sink so the work is not optimised away.
//  2. the raw input is treated as a stream of RTP payloads and fed to the depacketizer in chunks, with
//     markers, sequence gaps and resets, to exercise loss / needs-keyframe handling.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/net/rtp_h264.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static volatile uint64_t sink;

static void hash_au(const uint8_t *out, size_t out_len)
{
  if (out_len >= 4u && out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 1) {
    for (size_t i = 0; i < out_len; i++) {
      sink = (sink ^ out[i]) * 0x100000001B3ULL;
    }
  }
}

static bool push(ccord_rtp_h264_depack *d, const uint8_t *p, size_t n, uint16_t seq, uint32_t ts, bool marker)
{
  const uint8_t *out = nullptr;
  size_t out_len = 0;
  bool keyframe = false;
  uint32_t out_ts = 0;
  bool done = ccord_rtp_h264_depack_push(d, p, n, seq, ts, marker, &out, &out_len, &keyframe, &out_ts);
  if (done) { hash_au(out, out_len); }
  return done;
}

typedef struct rtp_feed {
  ccord_rtp_h264_depack *d;
  uint16_t seq;
  uint32_t ts;
} rtp_feed;

static bool feed_emit(void *ud, const uint8_t *payload, size_t len, bool marker)
{
  rtp_feed *f = ud;
  (void)push(f->d, payload, len, f->seq, f->ts, marker);
  f->seq = (uint16_t)(f->seq + 1u);
  return true;
}

static void packetize_roundtrip(const uint8_t *au, size_t len, size_t max_payload)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  if (d == nullptr) { return; }
  rtp_feed f = { d, 0, 90000u };
  ccord_error err;
  ccord_error_clear(&err);
  size_t packets = 0;
  (void)ccord_rtp_h264_packetize(au, len, max_payload, feed_emit, &f, &packets, &err);
  (void)ccord_rtp_h264_depack_needs_keyframe(d);
  (void)ccord_rtp_h264_depack_lost(d);
  ccord_rtp_h264_depack_destroy(d);
}

// Prefix a SPS so the depacketizer has a keyframe reference and reassembly actually runs.
static void roundtrip_with_sps(const uint8_t *data, size_t size)
{
  static const uint8_t sps[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x0A };
  uint8_t au[4096];
  if (size > 4000u) { size = 4000u; }
  memcpy(au, sps, sizeof sps);
  memcpy(au + sizeof sps, data, size);
  packetize_roundtrip(au, sizeof sps + size, 1100u);
  packetize_roundtrip(au, sizeof sps + size, 64u);
}

static void depack_chunks(const uint8_t *data, size_t size, size_t chunk, bool mark_each)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  if (d == nullptr) { return; }
  uint16_t seq = 0;
  size_t off = 0;
  while (off < size) {
    size_t n = size - off;
    if (n > chunk) { n = chunk; }
    bool marker = mark_each || (off + n >= size);
    (void)push(d, data + off, n, seq, 90000u, marker);
    seq = (uint16_t)(seq + 1u);
    off += n;
  }
  ccord_rtp_h264_depack_destroy(d);
}

static void depack_single(const uint8_t *data, size_t size)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  if (d == nullptr) { return; }
  (void)push(d, data, size, 0, 90000u, true);
  ccord_rtp_h264_depack_reset(d);
  (void)push(d, data, size, 1, 90000u, true);
  ccord_rtp_h264_depack_destroy(d);
}

// Introduce a sequence gap part-way through to exercise loss counting and needs-keyframe.
static void depack_gap(const uint8_t *data, size_t size)
{
  ccord_rtp_h264_depack *d = ccord_rtp_h264_depack_create();
  if (d == nullptr) { return; }
  uint16_t seq = 0;
  size_t off = 0;
  size_t idx = 0;
  while (off < size) {
    size_t n = size - off;
    if (n > 16u) { n = 16u; }
    if (idx == 3u) { seq = (uint16_t)(seq + 5u); }
    bool marker = (off + n >= size);
    (void)push(d, data + off, n, seq, 90000u, marker);
    seq = (uint16_t)(seq + 1u);
    off += n;
    idx++;
  }
  (void)ccord_rtp_h264_depack_needs_keyframe(d);
  (void)ccord_rtp_h264_depack_lost(d);
  ccord_rtp_h264_depack_destroy(d);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  static const uint8_t empty = 0;
  if (data == nullptr) { data = &empty; size = 0; }

  packetize_roundtrip(data, size, 1100u);
  packetize_roundtrip(data, size, 64u);
  packetize_roundtrip(data, size, 0u);    // invalid max_payload -> error path
  packetize_roundtrip(data, size, 63u);   // invalid max_payload -> error path
  roundtrip_with_sps(data, size);

  depack_chunks(data, size, 16u, false);
  depack_chunks(data, size, 64u, true);
  depack_single(data, size);
  depack_gap(data, size);
  return 0;
}
