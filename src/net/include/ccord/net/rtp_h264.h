// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// RFC 6184 H.264 RTP payload format (packetization-mode=1: single NAL, STAP-A, FU-A). Pure functions, no I/O.
//
// DAVE ordering (Discord voice E2EE operates on whole frames, so it WRAPS packetization):
//   send: encoder access unit (Annex-B) -> DAVE encrypt (whole frame) -> ccord_rtp_h264_packetize -> RTP + transport AEAD
//   recv: transport AEAD -> RTP -> ccord_rtp_h264_depack_push (whole access unit) -> DAVE decrypt -> decoder
// Both directions therefore treat the buffer as an opaque Annex-B stream: NAL boundaries are found by start codes
// (00 00 01 / 00 00 00 01) only; the NAL payload bytes are never interpreted except the 1-byte NAL header.
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>

// Payload types this client offers in SELECT_PROTOCOL `codecs` (the server's SESSION_DESCRIPTION / the voice layer
// decide what is actually negotiated; the voice layer should pass the negotiated value to its RTP header writer).
#define CCORD_RTP_H264_PT 101
#define CCORD_RTP_H264_RTX_PT 102
#define CCORD_RTP_CLOCK_HZ 90000u
#define CCORD_RTP_DEFAULT_MAX_PAYLOAD 1100u   // leaves room for RTP header + extensions + AEAD tag + nonce within a 1200 byte MTU budget

// Called once per RTP payload, in order. `marker` is true on the last payload of the access unit (set the RTP marker
// bit). Return false to abort packetization (e.g. socket error); packetize then returns CCORD_ERR_IO.
typedef bool (*ccord_rtp_emit_fn)(void *ud, const uint8_t *payload, size_t len, bool marker);

// Splits one Annex-B access unit into RTP payloads. Small consecutive NAL units (< max_payload together, e.g.
// SPS+PPS) are aggregated into a STAP-A (type 24); NAL units larger than max_payload are fragmented as FU-A (type 28,
// S/E bits, original F/NRI + type preserved); everything else is sent as a single NAL unit packet. NAL units that are
// access-unit delimiters (type 9) are dropped. Zero-length / start-code-only input -> CCORD_ERR_INVALID.
// max_payload must be >= 64 (else CCORD_ERR_INVALID). *out_packets (nullable) receives the packet count.
[[nodiscard]] ccord_status ccord_rtp_h264_packetize(const uint8_t *annexb, size_t len, size_t max_payload,
                                                    ccord_rtp_emit_fn emit, void *ud, size_t *out_packets,
                                                    ccord_error *err);

// ---- depacketizer ----
typedef struct ccord_rtp_h264_depack ccord_rtp_h264_depack;
[[nodiscard]] ccord_rtp_h264_depack *ccord_rtp_h264_depack_create(void);
void ccord_rtp_h264_depack_destroy(ccord_rtp_h264_depack *d);
void ccord_rtp_h264_depack_reset(ccord_rtp_h264_depack *d);   // drop partial state, require a keyframe

// Feed one RTP payload (RTP header + extension + padding already stripped), in sequence order; `seq`/`ts` are the
// RTP header values. A gap in `seq` (or a ts change before the previous access unit saw its marker, or an FU-A
// continuation without its start) discards the partially assembled access unit, bumps the loss counter and sets
// needs_keyframe until an IDR (NAL 5) or SPS (NAL 7) is received. Packets of a frame that arrives while
// needs_keyframe is set and that do not contain SPS/IDR are discarded.
// Returns true when `marker` completes an access unit: *out/*out_len point at an Annex-B buffer (4-byte start codes,
// valid until the next push/reset/destroy), *keyframe says it contains an IDR slice, *out_ts is the RTP timestamp.
// Returns false when more packets are needed or the packet was dropped.
bool ccord_rtp_h264_depack_push(ccord_rtp_h264_depack *d, const uint8_t *payload, size_t len, uint16_t seq,
                                uint32_t ts, bool marker, const uint8_t **out, size_t *out_len, bool *keyframe,
                                uint32_t *out_ts);
// Advance sequence tracking across a pure RTP padding probe (plain_len == 0) without disturbing the active AU.
void ccord_rtp_h264_depack_note_padding(ccord_rtp_h264_depack *d, uint16_t seq);
bool ccord_rtp_h264_depack_needs_keyframe(const ccord_rtp_h264_depack *d);   // caller should send PLI/FIR
uint64_t ccord_rtp_h264_depack_lost(const ccord_rtp_h264_depack *d);          // count of discarded access units
