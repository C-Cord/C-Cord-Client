// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/model/types.h>

typedef enum ccord_stream_encoder : uint8_t {
  CCORD_STREAM_ENCODER_NVENC = 0,     // NVIDIA NVENC (H.264/AV1)
  CCORD_STREAM_ENCODER_VAAPI,         // Linux VA-API
  CCORD_STREAM_ENCODER_AMF,           // AMD AMF
  CCORD_STREAM_ENCODER_VULKAN,        // Vulkan video
  CCORD_STREAM_ENCODER_OPENH264,      // software OpenH264
  CCORD_STREAM_ENCODER_X264,          // software libx264 (ultrafast/zerolatency)
  CCORD_STREAM_ENCODER_AUTO,          // probe best native hardware encoder at runtime (default)
  CCORD_STREAM_ENCODER_QSV,           // Intel Quick Sync Video (Windows/Linux)
  CCORD_STREAM_ENCODER_MF,            // Windows Media Foundation hardware H.264
  CCORD_STREAM_ENCODER_VIDEOTOOLBOX,  // macOS Apple VideoToolbox hardware H.264
} ccord_stream_encoder;
typedef enum ccord_stream_codec : uint8_t {
  CCORD_STREAM_CODEC_H264 = 0,
  CCORD_STREAM_CODEC_AV1,
} ccord_stream_codec;
typedef enum ccord_stream_framework : uint8_t {
  CCORD_STREAM_FRAMEWORK_PIPEWIRE = 0, // Linux xdg-desktop-portal + PipeWire
  CCORD_STREAM_FRAMEWORK_DMABUF,       // Linux DMA-BUF
  CCORD_STREAM_FRAMEWORK_NVFBC,        // Linux NVFBC
  CCORD_STREAM_FRAMEWORK_VULKAN,       // Linux Vulkan
  CCORD_STREAM_FRAMEWORK_X11,          // Linux X11 XShm/XComposite
  CCORD_STREAM_FRAMEWORK_AUTO,         // OS-native automatic capture backend (default)
  CCORD_STREAM_FRAMEWORK_WGC,          // Windows Graphics Capture (Win10+)
  CCORD_STREAM_FRAMEWORK_DXGI,         // Windows DXGI Desktop Duplication
  CCORD_STREAM_FRAMEWORK_GDI,          // Windows GDI compatibility capture
  CCORD_STREAM_FRAMEWORK_SCK,          // macOS ScreenCaptureKit
  CCORD_STREAM_FRAMEWORK_CG,           // macOS CoreGraphics Display Capture
} ccord_stream_framework;
typedef enum ccord_stream_resolution : uint8_t {
  CCORD_STREAM_RES_720P = 0,
  CCORD_STREAM_RES_1080P,
  CCORD_STREAM_RES_1440P,
  CCORD_STREAM_RES_SOURCE,
} ccord_stream_resolution;
typedef enum ccord_stream_fps : uint8_t {
  CCORD_STREAM_FPS_15 = 0,
  CCORD_STREAM_FPS_30,
  CCORD_STREAM_FPS_60,
} ccord_stream_fps;
// The framework value CCORD_STREAM_FRAMEWORK_PIPEWIRE means the xdg-desktop-portal ScreenCast + PipeWire path.
typedef struct ccord_stream_config {
  ccord_stream_encoder encoder;
  ccord_stream_codec codec;
  ccord_stream_framework framework;
  ccord_stream_resolution resolution;
  ccord_stream_fps fps;
} ccord_stream_config;
typedef struct ccord_voice_state {
  ccord_id guild_id, channel_id, user_id;      // channel_id = 0 means disconnected
  int64_t request_to_speak_timestamp;          // unix ms of the pending stage ask (0 = none)
  bool self_mute, self_deaf, self_video, self_stream;
  bool mute, deaf;                             // server-enforced mute / deafen
  bool suppress;                               // stage audience: mic suppressed until invited to speak
  bool omit_suppress, omit_request_to_speak;   // true when partial gateway payload omitted suppress / request_to_speak_timestamp
} ccord_voice_state;
typedef struct ccord_voice_server {
  ccord_id guild_id, channel_id;
  const char *token;                           // voice handshake token; never logged
  const char *endpoint;
} ccord_voice_server;
typedef struct ccord_stream_state {
  ccord_id guild_id, channel_id, user_id;      // user_id = broadcaster
  bool active;
  bool paused;
  ccord_stream_config config;
} ccord_stream_state;
// M5 voice & video state. Call state is for DM/group calls
// ringing[] lists users whose clients still ring; active=false once the call ended.
typedef struct ccord_call_state {
  ccord_id channel_id; char region[64];
  ccord_id ringing[8]; size_t ringing_count; bool active;
} ccord_call_state;
typedef struct ccord_stage_instance {
  ccord_id channel_id, guild_id; char topic[125];
  ccord_id id;                                 // stage instance snowflake (0 = not reported yet)
  uint8_t privacy_level;                       // Discord stage privacy level (2 = GUILD_ONLY; 0 = not reported)
  bool discoverable_disabled;                  // true when the stage is hidden from discovery
  ccord_id guild_scheduled_event_id;           // linked scheduled event (0 = none)
} ccord_stage_instance;
// Guild scheduled events (GUILD_SCHEDULED_EVENT_* gateway + /guilds/{g}/scheduled-events REST). The enums keep
// Discord's wire values; a zero value means "not reported".

typedef enum ccord_call_action : uint8_t { CCORD_CALL_JOIN = 0, CCORD_CALL_LEAVE } ccord_call_action;
// Local voice preferences (PTT vs VAD, sensitivity, DSP toggles); never sent as Discord state.
typedef struct ccord_voice_config { bool ptt; uint32_t vad_sensitivity_pct;
                                  bool ns, aec, agc; } ccord_voice_config;
// Voice connection lifecycle state (shared across live transport, mock transport, and UI diagnostics).
typedef enum ccord_voice_connection_state : uint8_t {
  CCORD_VOICE_IDLE = 0,
  CCORD_VOICE_CONNECTING,
  CCORD_VOICE_HELLO,
  CCORD_VOICE_READY,
  CCORD_VOICE_ESTABLISHED,
  CCORD_VOICE_FAILED,
  CCORD_VOICE_RECONNECTING,   // WebSocket lost, UDP/keys kept, resuming (appended: keeps existing ordinals)
} ccord_voice_connection_state;
// RTC connection snapshot for the debug panel; state is a ccord_voice_connection_state value.
typedef struct ccord_rtc_stats { int state; uint32_t ssrc; int ping_ms;
                                 size_t jitter_depth; uint64_t packets_lost;
                                 uint32_t bitrate_kbps; uint32_t dave_epoch; } ccord_rtc_stats;
typedef struct ccord_dave_status { bool active; bool verified;
                                   uint16_t protocol_version;
                                   char privacy_code[64]; } ccord_dave_status;
