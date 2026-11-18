// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// RFC 6184 H.264 RTP payload format, packetization-mode=1 (single NAL, STAP-A, FU-A). See rtp_h264.h.
#include <ccord/net/rtp_h264.h>

#include <string.h>

#include <ccord/core/alloc.h>

#define NAL_TYPE_IDR 5
#define NAL_TYPE_SPS 7
#define NAL_TYPE_AUD 9
#define NAL_TYPE_STAP_A 24
#define NAL_TYPE_FU_A 28

#define PKT_CAP 16384u                     // packetize scratch buffer (STAP-A / FU-A); larger max_payload is clamped
#define DEPACK_MAX_AU (8u * 1024u * 1024u) // hard cap on a reassembled access unit
#define DEPACK_INITIAL_CAP (64u * 1024u)

// ===================================================== packetizer =====================================================

typedef struct annexb_iter {
  const uint8_t *p;
  size_t len;
  size_t pos;
} annexb_iter;

// Finds the next 00 00 01 at or after `from`; returns its index or SIZE_MAX.
static size_t find_start_code(const uint8_t *p, size_t len, size_t from) {
  if (len < 3) return SIZE_MAX;
  size_t i = from;
  while (i + 3 <= len) {
    if (p[i + 2] > 1) {
      i += 3;
    } else if (p[i + 2] == 1 && p[i] == 0 && p[i + 1] == 0) {
      return i;
    } else {
      i++;
    }
  }
  return SIZE_MAX;
}

// Next non-empty NAL that is not an access unit delimiter.
static bool next_valid_nal(annexb_iter *it, const uint8_t **nal, size_t *nal_len) {
  for (;;) {
    size_t sc = find_start_code(it->p, it->len, it->pos);
    if (sc == SIZE_MAX) {
      it->pos = it->len;
      return false;
    }
    size_t start = sc + 3;
    size_t next = find_start_code(it->p, it->len, start);
    size_t end = next == SIZE_MAX ? it->len : next;
    it->pos = end;
    bool dave_wrapped = (it->len >= 4u && it->p[it->len - 2u] == 0xFAu && it->p[it->len - 1u] == 0xFAu);
    if (dave_wrapped) {
      // libdave emits canonical 4-byte start codes (00 00 00 01) between encrypted NALs; strip only that
      // single leading zero_byte so legitimate 0x00 ciphertext bytes at the end of a NAL are preserved.
      if (next != SIZE_MAX && end > start && it->p[end - 1u] == 0) end--;
    } else {
      while (end > start && it->p[end - 1] == 0) end--; // trailing_zero_8bits / leading zero of a 4-byte start code
    }
    if (end == start) continue;
    if ((it->p[start] & 0x1F) == NAL_TYPE_AUD) continue;
    *nal = it->p + start;
    *nal_len = end - start;
    return true;
  }
}

static bool emit_checked(ccord_rtp_emit_fn emit, void *ud, const uint8_t *p, size_t n, bool marker, size_t *count) {
  if (!emit(ud, p, n, marker)) return false;
  (*count)++;
  return true;
}

ccord_status ccord_rtp_h264_packetize(const uint8_t *annexb, size_t len, size_t max_payload, ccord_rtp_emit_fn emit,
                                      void *ud, size_t *out_packets, ccord_error *err) {
  size_t count = 0;
  if (out_packets) *out_packets = 0;
  if (!annexb || len == 0 || !emit) return ccord_error_set(err, CCORD_ERR_INVALID, "empty access unit");
  if (max_payload < 64) return ccord_error_set(err, CCORD_ERR_INVALID, "max_payload must be >= 64");
  if (max_payload > PKT_CAP) max_payload = PKT_CAP;

  uint8_t buf[PKT_CAP];
  annexb_iter it = {annexb, len, 0};
  const uint8_t *cur;
  size_t clen;
  if (!next_valid_nal(&it, &cur, &clen)) return ccord_error_set(err, CCORD_ERR_INVALID, "no NAL units in access unit");

  bool more = true;
  while (more) {
    // it is positioned after cur. Decide the packet shape for cur (+ following NALs for a STAP-A).
    const uint8_t *nxt = nullptr;
    size_t nlen = 0;
    bool aggregate = false;
    if (clen + 3 <= max_payload) {
      annexb_iter peek = it;
      aggregate = next_valid_nal(&peek, &nxt, &nlen) && clen + 3 + 2 + nlen <= max_payload;
    }

    if (aggregate) {
      size_t total = 1;
      uint8_t f = 0, nri = 0;
      size_t used = 1;
      const uint8_t *m = cur;
      size_t mlen = clen;
      for (;;) {
        buf[used++] = (uint8_t)(mlen >> 8);
        buf[used++] = (uint8_t)mlen;
        memcpy(buf + used, m, mlen);
        used += mlen;
        total = used;
        f |= m[0] & 0x80;
        if ((m[0] & 0x60) > nri) nri = m[0] & 0x60;
        annexb_iter peek = it;
        const uint8_t *n2;
        size_t l2;
        if (!next_valid_nal(&peek, &n2, &l2) || total + 2 + l2 > max_payload) break;
        it = peek;
        m = n2;
        mlen = l2;
      }
      buf[0] = (uint8_t)(f | nri | NAL_TYPE_STAP_A);
      more = next_valid_nal(&it, &cur, &clen);
      if (!emit_checked(emit, ud, buf, total, !more, &count)) {
        if (out_packets) *out_packets = count;
        return ccord_error_set(err, CCORD_ERR_IO, "RTP emit callback failed");
      }
      continue;
    }

    if (clen <= max_payload) {
      more = next_valid_nal(&it, &nxt, &nlen);
      if (!emit_checked(emit, ud, cur, clen, !more, &count)) {
        if (out_packets) *out_packets = count;
        return ccord_error_set(err, CCORD_ERR_IO, "RTP emit callback failed");
      }
      cur = nxt;
      clen = nlen;
      continue;
    }

    // FU-A
    const uint8_t *src = cur + 1;
    size_t remaining = clen - 1;
    size_t chunk_max = max_payload - 2;
    uint8_t indicator = (uint8_t)((cur[0] & 0xE0) | NAL_TYPE_FU_A);
    uint8_t type = cur[0] & 0x1F;
    more = next_valid_nal(&it, &nxt, &nlen);
    bool first = true;
    while (remaining > 0) {
      size_t n = remaining < chunk_max ? remaining : chunk_max;
      bool last = n == remaining;
      buf[0] = indicator;
      buf[1] = (uint8_t)((first ? 0x80 : 0) | (last ? 0x40 : 0) | type);
      memcpy(buf + 2, src, n);
      if (!emit_checked(emit, ud, buf, n + 2, last && !more, &count)) {
        if (out_packets) *out_packets = count;
        return ccord_error_set(err, CCORD_ERR_IO, "RTP emit callback failed");
      }
      src += n;
      remaining -= n;
      first = false;
    }
    cur = nxt;
    clen = nlen;
  }
  if (out_packets) *out_packets = count;
  return CCORD_OK;
}

// ==================================================== depacketizer ====================================================

struct ccord_rtp_h264_depack {
  uint8_t *buf;
  size_t len;
  size_t cap;
  uint32_t cur_ts;
  uint16_t last_seq;
  bool have_seq;
  bool in_au;       // an access unit is being assembled (started, marker not yet seen)
  bool au_bad;      // current access unit is corrupt: ignore the rest of it
  bool fu_active;   // inside a FU-A (start seen, end not yet)
  bool has_idr;
  bool has_sps;
  bool needs_keyframe;
  uint64_t lost;
};

ccord_rtp_h264_depack *ccord_rtp_h264_depack_create(void) {
  ccord_rtp_h264_depack *d = ccord_calloc(1, sizeof *d);
  if (!d) return nullptr;
  d->needs_keyframe = true;
  return d;
}

void ccord_rtp_h264_depack_destroy(ccord_rtp_h264_depack *d) {
  if (!d) return;
  ccord_free(d->buf);
  ccord_free(d);
}

void ccord_rtp_h264_depack_reset(ccord_rtp_h264_depack *d) {
  if (!d) return;
  d->len = 0;
  d->have_seq = false;
  d->in_au = false;
  d->au_bad = false;
  d->fu_active = false;
  d->has_idr = false;
  d->has_sps = false;
  d->needs_keyframe = true;
}

void ccord_rtp_h264_depack_note_padding(ccord_rtp_h264_depack *d, uint16_t seq) {
  if (!d) return;
  if (!d->have_seq) {
    d->have_seq = true;
    d->last_seq = seq;
    return;
  }
  if (seq == (uint16_t)(d->last_seq + 1u)) d->last_seq = seq;
}

bool ccord_rtp_h264_depack_needs_keyframe(const ccord_rtp_h264_depack *d) { return d ? d->needs_keyframe : true; }
uint64_t ccord_rtp_h264_depack_lost(const ccord_rtp_h264_depack *d) { return d ? d->lost : 0; }

static void begin_au(ccord_rtp_h264_depack *d, uint32_t ts) {
  d->len = 0;
  d->cur_ts = ts;
  d->in_au = true;
  d->au_bad = false;
  d->fu_active = false;
  d->has_idr = false;
  d->has_sps = false;
}

// Discards the current access unit (counted once) and ignores the rest of it.
static void mark_bad(ccord_rtp_h264_depack *d) {
  if (!d->au_bad) {
    d->au_bad = true;
    d->lost++;
    d->needs_keyframe = true;
  }
  d->len = 0;
  d->fu_active = false;
  d->has_idr = false;
  d->has_sps = false;
}

static bool reserve(ccord_rtp_h264_depack *d, size_t add) {
  if (add > DEPACK_MAX_AU || d->len > DEPACK_MAX_AU - add) return false;
  size_t need = d->len + add;
  if (need <= d->cap) return true;
  size_t cap = d->cap ? d->cap : DEPACK_INITIAL_CAP;
  while (cap < need) cap *= 2;
  if (cap > DEPACK_MAX_AU) cap = DEPACK_MAX_AU;
  uint8_t *nb = ccord_realloc(d->buf, cap);
  if (!nb) return false;
  d->buf = nb;
  d->cap = cap;
  return true;
}

static void note_type(ccord_rtp_h264_depack *d, unsigned type) {
  if (type == NAL_TYPE_IDR) d->has_idr = true;
  else if (type == NAL_TYPE_SPS) d->has_sps = true;
}

static bool append_nal(ccord_rtp_h264_depack *d, const uint8_t *nal, size_t n) {
  if (!reserve(d, 4 + n)) return false;
  static const uint8_t sc[4] = {0, 0, 0, 1};
  memcpy(d->buf + d->len, sc, 4);
  memcpy(d->buf + d->len + 4, nal, n);
  d->len += 4 + n;
  note_type(d, nal[0] & 0x1F);
  return true;
}

// Type of the first NAL a packet carries, or -1 when malformed / not a plain NAL carrier.
static int first_nal_type(const uint8_t *p, size_t n) {
  if (!p || n == 0) return -1;
  unsigned t = p[0] & 0x1F;
  if (t >= 1 && t <= 23) return (int)t;
  if (t == NAL_TYPE_STAP_A && n >= 4) return p[3] & 0x1F;
  if (t == NAL_TYPE_FU_A && n >= 3 && (p[1] & 0x80)) { // FU-A start: the fragmented NAL's type
    unsigned ft = p[1] & 0x1F;
    if (ft >= 1 && ft <= 23) return (int)ft;
  }
  return -1;
}

// Returns false when the packet is malformed or the buffer cap / allocation failed.
static bool consume_payload(ccord_rtp_h264_depack *d, const uint8_t *p, size_t n) {
  if (!p || n == 0) return false;
  unsigned type = p[0] & 0x1F;

  if (type == NAL_TYPE_FU_A) {
    if (n < 3) return false;
    bool s = p[1] & 0x80, e = p[1] & 0x40;
    unsigned ntype = p[1] & 0x1F;
    if ((s && e) || ntype == 0 || ntype >= NAL_TYPE_STAP_A) return false;
    if (s) {
      if (d->fu_active) return false;
      if (!reserve(d, 5 + (n - 2))) return false;
      static const uint8_t sc[4] = {0, 0, 0, 1};
      memcpy(d->buf + d->len, sc, 4);
      d->buf[d->len + 4] = (uint8_t)((p[0] & 0xE0) | ntype);
      memcpy(d->buf + d->len + 5, p + 2, n - 2);
      d->len += 5 + (n - 2);
      note_type(d, ntype);
      d->fu_active = !e;
    } else {
      if (!d->fu_active) return false; // continuation without its start
      if (!reserve(d, n - 2)) return false;
      memcpy(d->buf + d->len, p + 2, n - 2);
      d->len += n - 2;
      if (e) d->fu_active = false;
    }
    return true;
  }

  if (d->fu_active) return false; // previous fragmented NAL never ended

  if (type >= 1 && type <= 23) return append_nal(d, p, n);

  if (type == NAL_TYPE_STAP_A) {
    size_t pos = 1;
    if (n < 4) return false;
    while (pos < n) {
      if (n - pos < 2) return false;
      size_t sz = ((size_t)p[pos] << 8) | p[pos + 1];
      pos += 2;
      if (sz == 0 || sz > n - pos) return false;
      unsigned nt = p[pos] & 0x1F;
      if (nt == 0 || nt >= NAL_TYPE_STAP_A) return false;
      if (!append_nal(d, p + pos, sz)) return false;
      pos += sz;
    }
    return true;
  }

  return true; // STAP-B / MTAP / FU-B / reserved: unsupported, ignored
}

bool ccord_rtp_h264_depack_push(ccord_rtp_h264_depack *d, const uint8_t *payload, size_t len, uint16_t seq, uint32_t ts,
                                bool marker, const uint8_t **out, size_t *out_len, bool *keyframe, uint32_t *out_ts) {
  if (!d) return false;
  bool gap = d->have_seq && seq != (uint16_t)(d->last_seq + 1u);
  d->have_seq = true;
  d->last_seq = seq;
  bool same = d->in_au && ts == d->cur_ts;

  if (gap) {
    d->lost++;
    d->needs_keyframe = true;
    begin_au(d, ts);
    // A packet that opens with an SPS or IDR of a new timestamp is the start of its frame, so the loss was
    // between frames (the keyframe gate accepts either; see rtp_h264.h).
    int first = first_nal_type(payload, len);
    if (same || (first != NAL_TYPE_SPS && first != NAL_TYPE_IDR)) d->au_bad = true;
  } else if (!d->in_au) {
    begin_au(d, ts);
  } else if (!same) {
    bool was_bad = d->au_bad;
    begin_au(d, ts);   // previous access unit never saw its marker
    if (!was_bad) {
      d->lost++;
      d->needs_keyframe = true;
    }
  }

  if (!d->au_bad && !consume_payload(d, payload, len)) mark_bad(d);

  if (!marker) return false;

  d->in_au = false;
  if (!d->au_bad && d->fu_active) mark_bad(d); // access unit ends inside a fragmented NAL
  if (d->au_bad) {
    d->au_bad = false;
    d->len = 0;
    return false;
  }
  if (d->len == 0) return false;
  if (d->needs_keyframe && !(d->has_idr || d->has_sps)) {
    d->len = 0;
    return false;
  }
  d->needs_keyframe = false;
  if (out) *out = d->buf;
  if (out_len) *out_len = d->len;
  if (keyframe) *keyframe = d->has_idr;
  if (out_ts) *out_ts = d->cur_ts;
  return true;
}
