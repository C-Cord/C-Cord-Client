// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Per-user remote camera receive on the main voice gateway.
//
// The gateway tracks every sender that announced a camera (OP 12 VIDEO), depacketizes at most
// CCORD_VOICE_CAM_DECODE_MAX of them (one depacketizer each, first come first served) and queues whole H.264
// access units per sender. It never decodes: the caller polls the units and decodes them on its own thread.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/net/live_transport.h>

#define CCORD_VOICE_CAM_MAX        16u            // tracked senders
#define CCORD_VOICE_CAM_DECODE_MAX 4u             // admitted (depacketized) senders
#define CCORD_VOICE_CAM_RING       4u             // AUs buffered per admitted sender
#define CCORD_VOICE_CAM_AU_MAX     (512u * 1024u) // larger AU = dropped, sender waits for next IDR

typedef struct ccord_voice_cam_info {
  ccord_id user_id;
  uint32_t video_ssrc;
  uint32_t generation;   // never 0; changes whenever the AU stream for this user is discontinuous
  bool decoding;         // holds one of the CCORD_VOICE_CAM_DECODE_MAX slots
  bool need_keyframe;    // no IDR admitted yet since (re)start
} ccord_voice_cam_info;

// Turns on per-user camera receive on this (main voice) gateway. Off by default: Go Live viewer gateways keep the
// legacy single-SSRC path (rx_video_*). Safe to call at any time; when the gateway is already ESTABLISHED the
// OP 15 sink-wants message is sent now, else at SESSION_DESCRIPTION. Turning it off drops every tracked sender.
void ccord_voice_gateway_set_camera_rx(ccord_voice_gateway *g, bool enabled);
// Copies up to `max` tracked senders (arrival order). Returns the count written.
size_t ccord_voice_gateway_cameras(ccord_voice_gateway *g, ccord_voice_cam_info *out, size_t max);
// Pops the oldest queued AU of `user_id` into buf. False when none, when the user is not decoding, or when the AU
// does not fit buf_cap (that AU is then dropped and the sender re-gated on an IDR). out_ts90k/out_keyframe nullable.
bool ccord_voice_gateway_poll_camera(ccord_voice_gateway *g, ccord_id user_id, uint8_t *buf, size_t buf_cap,
                                     size_t *out_len, uint32_t *out_ts90k, bool *out_keyframe);
