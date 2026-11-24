// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/buf.h>
#include <ccord/core/error.h>
#include <ccord/model/model.h>
#include <ccord/net/voice_reconnect.h>

// Voice Gateway v8 ------------------------------------------------------------
#define CCORD_VOICE_GATEWAY_VERSION 8u
#define CCORD_VOICE_SAMPLE_RATE 48000u
#define CCORD_VOICE_CHANNELS 2u
#define CCORD_VOICE_FRAME_SAMPLES 960u   // 20 ms at 48 kHz
#define CCORD_VOICE_MAX_SECRET 32u
#define CCORD_VOICE_MAX_MODES 8u
#define CCORD_VOICE_MODE_MAX 32u
#define CCORD_VOICE_ENDPOINT_MAX 256u
#define CCORD_VOICE_RTP_HEADER_SIZE 12u
#define CCORD_VOICE_RTP_PAYLOAD_TYPE 120u
#define CCORD_VOICE_IP_DISCOVERY_SIZE 74u

typedef enum ccord_voice_opcode : uint8_t {
  CCORD_VOICE_OP_IDENTIFY = 0,
  CCORD_VOICE_OP_SELECT_PROTOCOL = 1,
  CCORD_VOICE_OP_READY = 2,
  CCORD_VOICE_OP_HEARTBEAT = 3,
  CCORD_VOICE_OP_SESSION_DESCRIPTION = 4,
  CCORD_VOICE_OP_SPEAKING = 5,
  CCORD_VOICE_OP_HEARTBEAT_ACK = 6,
  CCORD_VOICE_OP_HELLO = 8,
  CCORD_VOICE_OP_VIDEO = 12,
  CCORD_VOICE_OP_RESUME = 7,
  CCORD_VOICE_OP_RESUMED = 9,
} ccord_voice_opcode;

typedef struct ccord_voice_ready_info {
  uint32_t ssrc;
  char ip[64];
  uint16_t port;
  char modes[CCORD_VOICE_MAX_MODES][CCORD_VOICE_MODE_MAX];
  size_t mode_count;
} ccord_voice_ready_info;

// One decoded Voice Gateway v8 payload. Fixed storage keeps this type free of
// arena lifetime rules; secret_key is wiped by the state machine on teardown.
typedef struct ccord_voice_event {
  int op;
  int64_t seq;
  bool hello;
  bool heartbeat_ack;
  bool ready;
  bool session_description;
  bool speaking;
  bool video;
  uint32_t heartbeat_interval_ms;
  ccord_voice_ready_info ready_info;
  char mode[CCORD_VOICE_MODE_MAX];
  uint8_t secret_key[CCORD_VOICE_MAX_SECRET];
  size_t secret_key_len;
  uint8_t speaking_flag;
  uint32_t ssrc;
  uint32_t audio_ssrc, video_ssrc, rtx_ssrc;
  uint32_t stream_ssrcs[8];
  uint32_t stream_rtx_ssrcs[8];
  size_t stream_ssrc_count;
  ccord_id user_id;
  uint16_t dave_protocol_version;
  uint16_t transition_id;
  uint32_t epoch;
  ccord_id connected_users[256];
  size_t connected_user_count;
} ccord_voice_event;

// Decodes one Voice Gateway payload (op 8/2/4/6/5/12 + DAVE ops 11/13/21/22/24). Unknown ops are valid
// and only set `op`. No network activity.
[[nodiscard]] ccord_status ccord_voice_gateway_decode(const char *json, size_t len, ccord_voice_event *out,
                                                      ccord_error *err);

// Wire builders, exposed for unit tests.
[[nodiscard]] ccord_status ccord_voice_build_url(const char *endpoint, char *out, size_t cap, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_identify(ccord_id server_id, ccord_id user_id, const char *session_id,
                                                      const char *token, ccord_buf *out, ccord_error *err);
typedef struct ccord_voice_video_desc {
  uint32_t width, height, fps, max_bitrate;
  uint8_t payload_type, rtx_payload_type;
} ccord_voice_video_desc;
[[nodiscard]] ccord_status ccord_voice_build_select_protocol(const char *address, uint16_t port, const char *mode,
                                                             ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_select_protocol_ex(const char *address, uint16_t port, const char *mode,
                                                                uint8_t video_pt, uint8_t video_rtx_pt,
                                                                ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_heartbeat(ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_heartbeat_v8(uint64_t nonce, int64_t seq_ack, ccord_buf *out,
                                                          ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_speaking(uint32_t ssrc, uint8_t speaking, uint32_t delay_ms,
                                                      ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_video(uint32_t audio_ssrc, uint32_t video_ssrc, uint32_t rtx_ssrc,
                                                   ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_build_video_ex(uint32_t audio_ssrc, uint32_t video_ssrc, uint32_t rtx_ssrc,
                                                      const ccord_voice_video_desc *desc, ccord_buf *out,
                                                      ccord_error *err);

// Discord 74-byte UDP IP Discovery packet (0x0001 request, 0x0002 response).
size_t ccord_voice_ip_discovery_build(uint32_t ssrc, uint8_t *out);
[[nodiscard]] ccord_status ccord_voice_ip_discovery_parse(const uint8_t *pkt, size_t len, uint32_t *out_ssrc,
                                                          char *out_ip, uint16_t *out_port,
                                                          ccord_error *err);

typedef struct ccord_voice_gateway ccord_voice_gateway;
typedef struct ccord_voice_gateway_config {
  ccord_id server_id;             // guild id
  ccord_id channel_id;            // voice channel id (MLS group id; 0 falls back to server_id)
  ccord_id user_id;               // self id
  const char *session_id;         // VOICE_STATE_UPDATE session id (or READY fallback)
  const char *token;              // VOICE_SERVER_UPDATE token; never logged
  const char *endpoint;           // VOICE_SERVER_UPDATE endpoint host[:port]
  uint32_t connect_timeout_ms;    // 0 -> default
  uint64_t (*now_ms)(void *ud);   // injectable clock; null -> ccord_clock_boottime_ms
  void *clock_ud;
  // Test/embedding hook: when set, frames are delivered here instead of curl.
  ccord_status (*send)(void *ud, const char *data, size_t len, ccord_error *err);
  ccord_status (*send_bin)(void *ud, const uint8_t *data, size_t len, ccord_error *err);
  void *send_ud;
} ccord_voice_gateway_config;

[[nodiscard]] ccord_voice_gateway *ccord_voice_gateway_create(const ccord_voice_gateway_config *cfg,
                                                              ccord_error *err);
void ccord_voice_gateway_destroy(ccord_voice_gateway *g);
[[nodiscard]] ccord_status ccord_voice_gateway_start(ccord_voice_gateway *g, ccord_error *err);
void ccord_voice_gateway_stop(ccord_voice_gateway *g);
// Drains the socket, pumps UDP audio & IP discovery, sends due heartbeats, and returns the next decoded event.
bool ccord_voice_gateway_poll(ccord_voice_gateway *g, ccord_voice_event *out);
// Applies one payload and emits Identify/Select Protocol as the state machine
// requires. Exposed so the handshake is unit-testable without a socket.
[[nodiscard]] ccord_status ccord_voice_gateway_handle(ccord_voice_gateway *g, const char *json, size_t len,
                                                      ccord_voice_event *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_gateway_handle_binary(ccord_voice_gateway *g, const uint8_t *data, size_t len,
                                                             ccord_error *err);
ccord_voice_connection_state ccord_voice_gateway_state(const ccord_voice_gateway *g);
// Voice reconnect. Implemented by the gateway resume task.
[[nodiscard]] ccord_status ccord_voice_build_resume(ccord_id server_id, const char *session_id, const char *token,
                                                    int64_t seq_ack, ccord_buf *out, ccord_error *err);
typedef struct ccord_voice_failure {
  uint16_t close_code;          // 0 = no close frame
  ccord_vc_cause cause;
  bool was_established;
  ccord_vc state;               // policy state after the decision; the owner adopts it
  ccord_vc_plan plan;           // never CCORD_VC_RESUME (those are handled inside the gateway)
} ccord_voice_failure;
bool ccord_voice_gateway_failure(const ccord_voice_gateway *g, ccord_voice_failure *out);   // true only in FAILED
void ccord_voice_gateway_network_event(ccord_voice_gateway *g);   // ESTABLISHED: lose WS now (RESUME, delay 0); RECONNECTING: attempt now
void ccord_voice_gateway_inject_close(ccord_voice_gateway *g, uint16_t close_code);   // test hook, 0 = abnormal drop
uint32_t ccord_voice_gateway_ssrc(const ccord_voice_gateway *g);
int ccord_voice_gateway_udp_fd(const ccord_voice_gateway *g);
size_t ccord_voice_gateway_secret(const ccord_voice_gateway *g, uint8_t out[CCORD_VOICE_MAX_SECRET]);
[[nodiscard]] ccord_status ccord_voice_gateway_set_speaking(ccord_voice_gateway *g, uint8_t speaking,
                                                            uint32_t delay_ms, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_gateway_set_video(ccord_voice_gateway *g, bool enabled, ccord_error *err);
// Descriptor advertised by Voice OP 12 (max_bitrate / max_framerate / max_resolution) and the H.264 payload
// types. Go Live vstreams set their own at open; the standalone camera sets it before enabling video.
// nullptr clears it. No network activity.
[[nodiscard]] ccord_status ccord_voice_gateway_set_video_desc(ccord_voice_gateway *g,
                                                              const ccord_voice_video_desc *desc, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_gateway_send_video_op(ccord_voice_gateway *g, uint32_t video_ssrc,
                                                             uint32_t rtx_ssrc, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_gateway_send_h264(ccord_voice_gateway *g, const uint8_t *annexb, size_t len,
                                                         uint32_t timestamp_90khz, ccord_error *err);
bool ccord_voice_gateway_poll_video(ccord_voice_gateway *g, const uint8_t **out_annexb, size_t *out_len,
                                    bool *out_keyframe);
bool ccord_voice_gateway_consume_keyframe_request(ccord_voice_gateway *g);
bool ccord_voice_gateway_user_speaking(const ccord_voice_gateway *g, ccord_id user_id);
bool ccord_voice_gateway_self_speaking(const ccord_voice_gateway *g);
bool ccord_voice_gateway_poll_speaking(ccord_voice_gateway *g, ccord_id *out_user_id, bool *out_speaking);
// Queues 48kHz stereo S16LE PCM frames for 20ms Opus+RTP+AEAD UDP transmission.
[[nodiscard]] ccord_status ccord_voice_gateway_push_pcm(ccord_voice_gateway *g, const int16_t *pcm,
                                                        size_t stereo_samples, ccord_error *err);
// Reads decoded 48kHz stereo S16LE PCM frames received from the UDP voice socket.
size_t ccord_voice_gateway_pull_pcm(ccord_voice_gateway *g, int16_t *out_pcm, size_t max_stereo_samples);

// ---- M5 voice controls (implemented by m5-voice-core / m5-live-transport) --------------------------------------
// Per-user playback gain applied before mixing; pct is clamped to 0..200 (100 = unity). Unknown users are
// remembered until CCORD_SPEAK_MAX entries. No network activity.
void ccord_voice_gateway_set_user_volume(ccord_voice_gateway *g, ccord_id user_id, uint8_t pct);
// Push-to-talk gate: while `held` is false and PTT is enabled (ccord_voice_gateway_set_dsp), mic frames are
// dropped (silence frames are still sent on gate close). No network activity.
void ccord_voice_gateway_set_ptt(ccord_voice_gateway *g, bool held);
// Local deafen state: clears buffered RX audio and drops incoming RX PCM while deafened.
void ccord_voice_gateway_set_deafened(ccord_voice_gateway *g, bool deafened);
// Maps the 0..100 input-sensitivity slider to the VAD threshold (ccord_vad_threshold_from_sensitivity).
void ccord_voice_gateway_set_vad_sensitivity(ccord_voice_gateway *g, uint32_t pct);
// Queues 48kHz stereo S16LE soundboard PCM; it is mixed with the mic and transmitted with SPEAKING flags 2.
[[nodiscard]] ccord_status ccord_voice_gateway_push_soundboard_pcm(ccord_voice_gateway *g, const int16_t *pcm,
                                                                   size_t stereo_samples, ccord_error *err);
// Snapshot of connection/media counters for the RTC debug panel. Zeroed when idle. No network activity.
void ccord_voice_gateway_stats(const ccord_voice_gateway *g, ccord_rtc_stats *out);
// Applies input-processing settings (PTT mode, VAD sensitivity, NS/AEC/AGC). nullptr restores defaults.
void ccord_voice_gateway_set_dsp(ccord_voice_gateway *g, const ccord_voice_config *cfg);
// Copies up to `cap` recognized DAVE peer user IDs into `out_ids` and returns total peer count.
size_t ccord_voice_gateway_dave_peers(ccord_voice_gateway *g, ccord_id *out_ids, size_t cap);

// Opus + RTP/xsalsa20_poly1305 media framing (linked when CCORD_LIVE_ACCOUNT=ON).
typedef struct ccord_voice_codec ccord_voice_codec;
[[nodiscard]] ccord_voice_codec *ccord_voice_codec_create(ccord_error *err);
void ccord_voice_codec_destroy(ccord_voice_codec *c);
// Encodes `samples` interleaved S16 stereo frames (20 ms = 960 at 48 kHz).
[[nodiscard]] ccord_status ccord_voice_encode(ccord_voice_codec *c, const int16_t *pcm, size_t samples,
                                              uint8_t *out, size_t cap, size_t *out_len, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_decode(ccord_voice_codec *c, const uint8_t *packet, size_t len,
                                              int16_t *pcm, size_t samples, size_t *out_samples, ccord_error *err);
// Writes a 12-byte RTP v2 header in network byte order; returns bytes written.
size_t ccord_voice_rtp_header(uint8_t *out, size_t cap, uint16_t seq, uint32_t timestamp, uint32_t ssrc);
// Discord xsalsa20_poly1305: 12-byte RTP header || MAC(16) || ciphertext(Opus).
// `out` must hold header_len + frame_len + 16 bytes.
[[nodiscard]] ccord_status ccord_voice_encrypt(const uint8_t key[CCORD_VOICE_MAX_SECRET], const uint8_t *header,
                                               size_t header_len, const uint8_t *frame, size_t frame_len,
                                               uint8_t *out, size_t cap, size_t *out_len, ccord_error *err);
[[nodiscard]] ccord_status ccord_voice_decrypt(const uint8_t key[CCORD_VOICE_MAX_SECRET], const uint8_t *packet,
                                               size_t len, size_t header_len, uint8_t *out, size_t cap,
                                               size_t *out_len, ccord_error *err);
