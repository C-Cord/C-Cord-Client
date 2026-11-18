// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Internal declarations shared by the split voice translation units (voice_gateway.c,
// voice_stream.c, voice_codec.c) and the main gateway (discord_gateway.c).
// Not part of the public live_transport.h surface.
#pragma once

#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>

#include <curl/curl.h>
#include <opus/opus.h>

#include <ccord/net/live_transport.h>
#include <ccord/net/rtp_h264.h>
#include <ccord/net/voice_audio.h>
#include <ccord/net/voice_camera.h>
#include <ccord/net/voice_crypto.h>
#include <ccord/net/voice_dave.h>
#include <ccord/net/voice_stream.h>

// Connection limits shared with the main gateway (discord_gateway.c).
#define GW_MAX_FRAME (32u * 1024u * 1024u)
#define GW_URL_MAX 512u
#define GW_SESSION_MAX 128u

#define VOICE_PCM_CAP_SAMPLES (CCORD_VOICE_FRAME_SAMPLES * 50u) // 1 second of 48kHz stereo frames
#define VOICE_MAX_STREAMS 16u
#define VG_VIDEO_RING_SLOTS 8u
#define VG_STREAM_SSRC_MAX 8u

typedef struct vg_video_slot {
  uint8_t *buf;
  size_t len;
  size_t cap;
  uint32_t ssrc;
  uint32_t ts90k;
  bool keyframe;
} vg_video_slot;

typedef struct vg_audio_stream {
  bool used;
  bool talking;
  uint8_t plc_streak;
  uint32_t ssrc;
  uint64_t last_rx_ms;
  OpusDecoder *dec;
  ccord_jitter *jitter;
} vg_audio_stream;

// Per-user playback gain (m5-voice-core); pct 0..200, 100 = unity.
typedef struct vg_user_volume {
  ccord_id user_id;
  uint8_t pct;
} vg_user_volume;

// Per-user camera receive (voice_camera.h). Owned by the camera section of voice_gateway.c.
typedef struct vg_cam_au {
  uint8_t *data;
  size_t len, cap;
  uint32_t ts90k;
  bool keyframe;
} vg_cam_au;

typedef struct vg_cam {
  ccord_id user_id;
  uint32_t video_ssrc, rtx_ssrc;
  uint32_t generation;
  bool admitted, need_idr;
  bool demoted;                          // released for being idle; promoted only after non-demoted waiters
  ccord_rtp_h264_depack *depack;         // created on admission, destroyed on release
  vg_cam_au ring[CCORD_VOICE_CAM_RING];  // buffers grown lazily, freed on release
  uint8_t head, count;
  uint64_t last_rx_ms, last_pli_ms;
} vg_cam;

struct ccord_voice_dsp;  // speexdsp preprocess chain (voice_dsp.h, m5-voice-core)

struct ccord_voice_gateway {
  char token[CCORD_LIVE_TOKEN_MAX + 1];
  char session_id[GW_SESSION_MAX];
  char endpoint[CCORD_VOICE_ENDPOINT_MAX];
  char url[GW_URL_MAX];
  ccord_id server_id, channel_id, user_id;
  CURL *curl;
  int udp_fd;
  struct sockaddr_in peer_addr;
  bool peer_valid;
  bool open;
  bool hello;
  bool identified;
  bool selected;
  bool ready;
  bool video;
  bool speaking_active;
  bool frame_is_binary;
  int64_t seq_ack;
  uint32_t ssrc, video_ssrc, rtx_ssrc, remote_video_ssrc, remote_video_rtx_ssrc;
  ccord_id default_remote_user, remote_video_user;
  uint32_t remote_stream_ssrcs[VG_STREAM_SSRC_MAX];
  uint32_t remote_stream_rtx_ssrcs[VG_STREAM_SSRC_MAX];
  size_t remote_stream_ssrc_count;
  uint16_t rtp_seq;
  uint16_t video_rtp_seq;
  uint32_t rtp_timestamp;
  uint32_t tx_nonce;
  uint32_t silence_frames_left;
  uint64_t next_audio_tx_ms;
  uint64_t ip_discovery_deadline;
  uint32_t heartbeat_ms;
  uint32_t connect_timeout_ms;
  uint64_t next_heartbeat;
  bool heartbeat_pending;      // a heartbeat was sent and its ACK (echoing the nonce) has not arrived yet
  uint64_t heartbeat_nonce;    // nonce of the outstanding heartbeat
  char ip[64];
  uint16_t port;
  char discovered_ip[64];
  uint16_t discovered_port;
  char modes[CCORD_VOICE_MAX_MODES][CCORD_VOICE_MODE_MAX];
  size_t mode_count;
  char mode[CCORD_VOICE_MODE_MAX];
  ccord_voice_mode aead_mode;
  uint8_t secret[CCORD_VOICE_MAX_SECRET];
  size_t secret_len;
  ccord_voice_codec *codec;
  ccord_dave *dave;
  ccord_vad vad;
  ccord_ssrc_map ssrc_map;
  ccord_speaking speaking;
  vg_audio_stream audio_streams[VOICE_MAX_STREAMS];
  ccord_rtp_h264_depack *video_depack;
  vg_video_slot rx_video_ring[VG_VIDEO_RING_SLOTS];
  uint8_t rx_video_head;
  uint8_t rx_video_count;
  uint8_t *rx_video_buf;
  size_t rx_video_len;
  size_t rx_video_cap;
  bool rx_video_keyframe;
  bool rx_video_ready;
  uint32_t rx_video_ssrc;
  uint32_t rx_video_ts90k;
  uint64_t stream_last_pli_ms;
  uint64_t stream_pli_after_ms;
  bool stream_need_idr;
  bool stream_rx_pli;
  int16_t tx_pcm[VOICE_PCM_CAP_SAMPLES * CCORD_VOICE_CHANNELS];
  size_t tx_samples;
  int16_t rx_pcm[VOICE_PCM_CAP_SAMPLES * CCORD_VOICE_CHANNELS];
  size_t rx_samples;
  uint64_t next_rx_mix_ms;
  bool deafened;
  ccord_voice_connection_state state;
  uint64_t (*now_ms)(void *ud);
  void *clock_ud;
  ccord_status (*send)(void *ud, const char *data, size_t len, ccord_error *err);
  ccord_status (*send_bin)(void *ud, const uint8_t *data, size_t len, ccord_error *err);
  void *send_ud;
  ccord_buf frame;
  // ---- M5 fields (owned by m5-voice-core) ------------------------------------------------------------------
  ccord_voice_config dsp_cfg;   // last applied input-processing settings (PTT/VAD/NS/AEC/AGC)
  struct ccord_voice_dsp *dsp;  // nullptr when DSP is unavailable or disabled
  vg_user_volume user_volume[CCORD_SPEAK_MAX];
  size_t user_volume_count;
  int16_t sb_pcm[VOICE_PCM_CAP_SAMPLES * CCORD_VOICE_CHANNELS];  // soundboard TX queue, mixed with mic
  size_t sb_samples;
  bool sb_speaking;             // SPEAKING flags 2 asserted while the soundboard queue drains
  uint64_t heartbeat_sent_ms;   // when the outstanding heartbeat was sent (ACK-timeout check)
  int ping_ms;                  // last heartbeat round-trip; -1 = unknown
  uint64_t packets_lost;        // cumulative jitter-buffer losses
  uint64_t bitrate_bytes;       // media bytes received in the current window
  uint64_t bitrate_window_ms;   // start of the current 1 s bitrate window
  uint32_t bitrate_kbps;        // last completed window
  bool suppressed;              // stage suppress: mic TX gated like closed PTT (local-only)
  uint32_t dave_epoch;          // last DAVE PREPARE_EPOCH value (telemetry only)
  uint8_t tx_speaking_flags;    // SPEAKING flags last sent (mic=1, soundboard=2)
  // ---- Go Live stream-audio tap (m5-live-transport) ----------------------------------------------------
  // When set, decrypted Opus RX packets are delivered here (with RTP seq / 48 kHz timestamp) instead of
  // entering the jitter/PCM pipeline: a vstream connection wants raw stream audio, not mixed microphone PCM.
  // Called under g->mtx from the pump; nullptr keeps the normal audio path.
  void (*raw_audio_cb)(void *ud, uint32_t ssrc, uint16_t seq, uint32_t ts48k, const uint8_t *opus, size_t len);
  void *raw_audio_ud;
  ccord_voice_video_desc video_desc;
  _Atomic bool keyframe_requested;
  // ---- per-user camera receive (voice_camera.h) ---------------------------------------------------------------
  vg_cam cams[CCORD_VOICE_CAM_MAX];  // arrival order
  size_t ncams;
  bool cam_rx, cam_wants_sent;
  // ---- WebSocket-only resume ----------------------------------------
  // State RECONNECTING with !open = waiting for resume_at_ms; with open = op 7 sent / awaiting op 9 by
  // resume_deadline_ms (no separate "resuming" flag: state + open already say which).
  ccord_vc rc;                   // policy state for the current outage; the owner adopts failure.state
  uint64_t resume_at_ms, resume_deadline_ms;
  bool skip_next_delay;          // network nudge: the next resume attempt runs without its backoff delay
  ccord_voice_failure failure;   // valid only while state == FAILED && has_failure
  bool has_failure;
  // ---- M0-6 dedicated audio thread (item 5b) ----------------------------------------------------------
  // Serializes every public entry point (PCM push/pull, pump, signaling, controls, getters) so the
  // app's audio pthread can move PCM while the UI thread polls the gateway. Internal helpers that
  // run under an already-held lock use the unlocked vg_* variants.
  pthread_mutex_t mtx;
};

// Shared with the main gateway TU (discord_gateway.c): one curl_global_init for both websockets.
void gw_ensure_curl(void);

// Raw text-frame send used by voice_gateway.c and the vstream wrapper (voice_stream.c).
ccord_status vg_send_raw(ccord_voice_gateway *g, const char *data, size_t len, ccord_error *err);
ccord_status vg_set_speaking(ccord_voice_gateway *g, uint8_t speaking, uint32_t delay_ms, ccord_error *err);
void vg_pump_udp_established(ccord_voice_gateway *g);

// The voice gateway behind a vstream handle (loopback tests inject signaling with ccord_voice_gateway_handle).
// Defined in voice_stream.c; nullptr for a null handle.
ccord_voice_gateway *ccord_vstream_gateway(ccord_vstream *s);
