// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Go Live / video connection ("vstream") built on the same voice-connection core as the audio connection
// (src/net/voice_gateway.c): Voice Gateway v8 websocket, UDP + IP discovery, aead_*_rtpsize transport
// encryption and DAVE E2EE (libdave), but video only (no microphone, no Opus).
//
// Threading: everything runs on the caller's (main) thread, exactly like the audio connection. The live
// transport drives all open vstreams from ccord_transport_poll() (via ccord_vstream_pump_all()), so the
// stream owner never has to call anything periodically. The only exception is ccord_vstream_send_frame(),
// which is thread-safe (the encoder thread may call it directly; it takes an internal mutex around DAVE
// encryption, RTP sequencing, the AEAD nonce counter and the UDP send).
//
// Callbacks (on_frame / on_state / on_keyframe_request) are invoked from ccord_vstream_pump_all(), i.e.
// on the main thread, never from inside ccord_vstream_send_frame().
//
// Frame pipeline (DAVE operates on whole frames, so it wraps the packetizer):
//   send: Annex-B access unit --daveEncryptorEncrypt(video, ssrc, DAVE_CODEC_H264)--> ccord_rtp_h264_packetize
//         --> RTP header (PT = video_payload_type, 90 kHz timestamp, marker on last packet)
//         --> aead_*_rtpsize --> UDP
//   recv: UDP --> aead_*_rtpsize open --> ccord_rtp_h264_depack (per SSRC) --> daveDecryptorDecrypt (per user)
//         --> on_frame(ssrc, user_id, annexb, ...)
// The H.264 (de)packetizer is the stream agent's src/net/include/ccord/net/rtp_h264.h; voice_stream.c uses it
// when that header exists (otherwise send/recv of video frames return CCORD_ERR_UNSUPPORTED).
//
// DAVE group id: a Go Live stream is a separate media session with its own MLS group. Its group id is
// rtc_server_id - 1 (the "media session id"), NOT the channel id. Pass it explicitly via dave_group_id (0 means
// "derive it as server_id - 1").
//
// Identify (built here): server_id = rtc_server_id, channel_id = rtc_channel_id (both from STREAM_CREATE),
// user_id, session_id (the MAIN voice session id), token (STREAM_SERVER_UPDATE), video:true,
// max_dave_protocol_version, and for a sender streams:[{type:"screen",rid:"100",quality:100}].
// After READY we send SELECT_PROTOCOL with codecs {opus pt 120, H264 pt video_payload_type/rtx_payload_type},
// then (sender) op 12 VIDEO with the negotiated SSRCs, or (viewer) op 15 MEDIA_SINK_WANTS {"any":100}.
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/model/model.h>
#include <ccord/net/voice_gateway.h>

#define CCORD_VSTREAM_AUDIO_PT 120u
#define CCORD_VSTREAM_H264_PT_DEFAULT 101u
#define CCORD_VSTREAM_H264_RTX_PT_DEFAULT 102u
// Opcode 5 SPEAKING bitfield (voice gateway v8):
// https://docs.discord.com/developers/topics/voice-connections#speaking
// Microphone = 1 << 0, Soundshare = 1 << 1 ("context audio for video, no speaking indicator"),
// Priority = 1 << 2. Go Live stream audio is soundshare, never microphone or priority.
#define CCORD_VSTREAM_SPEAKING_SOUNDSHARE 2u

typedef struct ccord_vstream ccord_vstream;

typedef enum ccord_vstream_state : uint8_t {
  CCORD_VSTREAM_CONNECTING = 0,  // websocket/identify/ready/select-protocol in flight
  CCORD_VSTREAM_READY = 1,       // session description received; media may flow (DAVE may still be settling)
  CCORD_VSTREAM_CLOSED = 2,      // closed or failed (see `why`); the handle must still be closed by the owner
} ccord_vstream_state;

typedef struct ccord_vstream_params {
  bool sender;                    // true = we broadcast (op 12 streams[] entry); false = viewer (receive only)
  ccord_id server_id;             // rtc_server_id from STREAM_CREATE (Identify.server_id)
  ccord_id channel_id;            // rtc_channel_id from STREAM_CREATE (Identify.channel_id)
  ccord_id user_id;               // self
  ccord_id dave_group_id;         // MLS group id; 0 = server_id - 1
  const char *session_id;         // the MAIN voice connection's session id
  const char *endpoint;           // STREAM_SERVER_UPDATE endpoint (host[:port])
  const char *token;              // STREAM_SERVER_UPDATE token; never logged
  uint32_t width, height, fps;    // sender: goes into op 12 streams[0].max_resolution / max_framerate
  uint32_t max_bitrate;           // sender: bits per second
  uint8_t video_payload_type;     // 0 = CCORD_VSTREAM_H264_PT_DEFAULT
  uint8_t rtx_payload_type;       // 0 = CCORD_VSTREAM_H264_RTX_PT_DEFAULT
  // Called for every complete, DAVE-decrypted H.264 access unit (Annex-B). user_id is 0 when the SSRC is not
  // (yet) mapped to a user. ts90k is the RTP timestamp. Main thread, from ccord_vstream_pump_all().
  void (*on_frame)(void *ud, uint32_t ssrc, ccord_id user_id, const uint8_t *annexb, size_t len, bool keyframe,
                   uint32_t ts90k);
  void (*on_state)(void *ud, ccord_vstream_state state, const char *why);
  // Sender only: the SFU asked for a new keyframe (RTCP PLI/FIR, or a new viewer / DAVE epoch change that
  // requires an IDR). Main thread.
  void (*on_keyframe_request)(void *ud);
  // Stream audio (Opus PT CCORD_VSTREAM_AUDIO_PT on audio_ssrc): one decoded packet per call. user_id is 0
  // when the SSRC is not (yet) mapped. ts48k is the 48 kHz RTP timestamp. Main thread, from pump_all().
  void (*on_audio)(void *ud, uint32_t ssrc, ccord_id user_id, const uint8_t *opus, size_t len, uint32_t ts48k);
  void *ud;
  // Loopback test hooks (all optional; null/0 keeps the real curl websocket + own UDP socket):
  ccord_status (*send)(void *ud, const char *data, size_t len, ccord_error *err);
  ccord_status (*send_bin)(void *ud, const uint8_t *data, size_t len, ccord_error *err);
  void *send_ud;
  int udp_fd;  // 0 = create own socket (default); > 0 = use this already-connected fd (tests only)
} ccord_vstream_params;

// Opens the connection (non-blocking after the websocket handshake) and registers it for pumping. Returns
// nullptr and sets err on invalid parameters or connect failure.
[[nodiscard]] ccord_vstream *ccord_vstream_open(const ccord_vstream_params *p, ccord_error *err);
// Thread-safe. `annexb` is one access unit (SPS/PPS/IDR for keyframes). ts90k = 90 kHz timestamp of the frame.
// Returns CCORD_ERR_INVALID until the connection is READY and (when DAVE is active) the encryptor has a key;
// callers should just drop the frame and keep encoding (request a keyframe via on_keyframe_request).
[[nodiscard]] ccord_status ccord_vstream_send_frame(ccord_vstream *s, const uint8_t *annexb, size_t len,
                                                    uint32_t ts90k, bool keyframe);
// Sender stream audio: one Opus packet (20 ms) on audio_ssrc with PT CCORD_VSTREAM_AUDIO_PT. Thread-safe like
// ccord_vstream_send_frame. ts48k = 48 kHz RTP timestamp. Returns CCORD_ERR_INVALID until READY.
[[nodiscard]] ccord_status ccord_vstream_send_audio(ccord_vstream *s, const uint8_t *opus, size_t len,
                                                    uint32_t ts48k, ccord_error *err);
ccord_vstream_state ccord_vstream_state_of(const ccord_vstream *s);
// SSRC of our video stream once READY (sender), else 0.
uint32_t ccord_vstream_video_ssrc(const ccord_vstream *s);
// Network change / wake-from-sleep nudge: the stream's WebSocket is treated as dead and resumes at once (the gateway
// rate-limits nudges). Main thread; no-op on nullptr.
void ccord_vstream_network_event(ccord_vstream *s);
// Drives every open vstream once (called by the live transport's poll). Safe to call from the main thread only.
void ccord_vstream_pump_all(void);
// Seeds known stream participants (broadcaster + viewers) into the stream's DAVE MLS roster and sets the
// expected video owner so non-broadcaster OP 12 messages (video_ssrc:0) never wipe the active video SSRC.
void ccord_vstream_register_peers(ccord_vstream *s, ccord_id owner_id, const ccord_id *peer_ids, size_t count);
// Thread-safe UDP + stream-audio popper for the real-time audio pthread: drains incoming UDP packets when
// established and pops one decrypted Opus packet (returns 0 when empty).
size_t ccord_vstream_pop_rx_audio(ccord_vstream *s, uint8_t *opus_out, size_t cap);
// Unregisters, closes the socket, wipes keys and frees. Safe on nullptr.
void ccord_vstream_close(ccord_vstream *s);

