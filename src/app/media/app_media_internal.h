// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include "app_media.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>

#define MEDIA_FRAME 960u                       // 20 ms at 48 kHz, matches the gateway frame size
#define MEDIA_MAX_USERS 64u                    // per-user volume sync table / voice user read cap
#define MEDIA_ID_CAP 64u                       // matches ccord_ui_voice_prefs device id fields
#define MEDIA_PKT_CAP (256u * 1024u)
#define MEDIA_CAM_INIT_BYTES (640u * 480u * 4u)    // initial BGRA camera buffer (~1.17 MiB); resized to fit
#define MEDIA_CAM_HD_BYTES (1280u * 720u * 4u)
#define MEDIA_CAM_MAX_BYTES (1920u * 1080u * 4u)   // largest BGRA camera frame we buffer
#define MEDIA_SB_CAP_SAMPLES (48000u * 2u * 6u)    // 6 s stereo 48 kHz cap for soundboard clips
#define MEDIA_VIDEO_KEY "__video_stream__"     // same texture the voice/stage views draw
#define MEDIA_OPEN_RETRY_MS 2000u              // backoff before retrying a failed device/codec open
#define MEDIA_CAM_AUS_PER_PUMP_TOTAL 4         // remote camera access units decoded per pump across all users
#define MEDIA_CAM_TEX_MAX_W 960                // remote camera decoder output clamp (aspect kept)
#define MEDIA_CAM_TEX_MAX_H 540
#define MEDIA_CAM_KEY_CAP 40u                  // "__cam__:<decimal id>" (== CCORD_CAM_TEXTURE_KEY_MAX)

typedef struct media_user_vol {
  ccord_id user;
  uint8_t pct;
  bool used;
} media_user_vol;

// One remote camera: a decoder per admitted sender.
typedef struct cam_slot {
  bool used;
  ccord_id user;
  uint32_t generation;
  ccord_video_decoder *decoder;
  uint64_t retry_ms;   // open backoff
  bool need_idr;       // discard non-keyframes until the first IDR decodes
  bool fresh;          // a unit decoded since the last texture upload
  int w, h;
  uint64_t frames, decode_errors, dropped_until_idr;
  char key[MEDIA_CAM_KEY_CAP];
} cam_slot;

struct ccord_app_media {
  const ccord_app_media_ops *ops;
  void *ud;

  // settings sync
  ccord_ui_voice_prefs prefs;
  ccord_voice_config last_cfg;
  bool have_cfg;
  bool have_ptt;
  bool last_ptt_held;
  bool last_ptt_mode;

  // per-user playback volume sync
  media_user_vol vol[MEDIA_MAX_USERS];
  size_t vol_count;
  ccord_id last_vol_users[MEDIA_MAX_USERS];
  size_t last_vol_n;

  // device selection (both paths). sel_* = last resolved settings ids, eff_* = ids actually used
  // (default "" when the chosen device is gone), opened_* = ids the inline handles were last tried
  // with, thread_* = ids the live audio pthread was last started with.
  char sel_in[MEDIA_ID_CAP], sel_out[MEDIA_ID_CAP];
  char eff_in[MEDIA_ID_CAP], eff_out[MEDIA_ID_CAP];
  char opened_in[MEDIA_ID_CAP], opened_out[MEDIA_ID_CAP];
  char thread_in[MEDIA_ID_CAP], thread_out[MEDIA_ID_CAP];
  char opened_cam[MEDIA_ID_CAP];
  bool resolved;
  bool devices_dirty;

  // audio
  ccord_audio *audio_cap;
  ccord_audio *audio_play;
  ccord_voice_audio_thread *thread;
  bool inline_cap_gated;
  bool have_thread_ctrl;
  bool last_th_muted;
  bool last_th_deafened;
  bool last_th_cap_suppressed;
  bool last_th_ptt_idle;
  int last_th_in_vol;
  int last_th_out_vol;
  uint64_t ptt_active_until_ms;   // keeps mic open across PTT release hangover + silence tail

  // local soundboard playback ring (shared between main thread decode and live/inline speaker pull)
  ccord_mutex *sb_mtx;
  _Atomic bool sb_active;
  int16_t *sb_pcm;
  size_t sb_samples;
  size_t sb_pos;
  uint64_t sb_next_ms;

  // Go Live send
  ccord_video_stream *video;
  ccord_capture *screen_cap;
  ccord_camera *camera;
  uint8_t *cam_buf;
  size_t cam_buf_cap;
  uint8_t cam_grab_fails;
  uint8_t *pkt;
  uint64_t last_video_ms;
  uint64_t stream_start_ms;
  bool streaming;
  bool was_paused;
  bool pending_keyframe;
  _Atomic bool paused_flag;

  // Go Live desktop-audio loopback ("share audio"): while enabled, the broadcast carries the
  // monitor mix instead of the mic downmix. The handle is main-thread only; loopback_flag tells
  // the audio pthread's mic->stream path to stand down (same cross-thread class as paused_flag).
  ccord_loopback_audio *loopback;
  uint64_t loopback_retry_ms;
  uint64_t loopback_next_ms;
  bool loopback_paused;
  bool loopback_unsupported;
  _Atomic bool loopback_enabled;   // coordinator-level switch (ccord_app_media_set_loopback_audio)
  _Atomic bool loopback_flag;      // effective this pump: stream_on && (setter || host read seam)

  // Lifecycle & visibility hints (power efficiency)
  bool in_background;
  bool stage_visible;
  bool preview_hidden;

  // Standalone voice camera TX (self_video while not Go Live streaming)
  ccord_video_stream *cam_video;
  uint64_t cam_start_ms;
  uint64_t last_cam_ms;
  uint64_t cam_enc_retry_ms;
  bool cam_streaming;
  bool cam_pending_keyframe;
  bool warn_cam_encoder, warn_cam_encode, warn_cam_send;

  // Go Live receive
  ccord_video_decoder *decoder;
  uint64_t stream_next_ms;   // 20 ms playout pace when stream audio runs without remote voice
  bool recv_fresh;   // a packet decoded cleanly since the last stage texture upload
  bool recv_need_idr;

  // Remote voice cameras (one decoder per admitted sender, at most CCORD_CAM_DECODE_MAX)
  cam_slot cams[CCORD_CAM_RX_MAX];
  size_t cam_rr;   // rotating start index for the per-pump AU budget

  // Failed-open backoff: retry no sooner than these timestamps (0 = try now). Prevents per-pump
  // open churn and repeated warning logs when a device/encoder/camera is unavailable.
  uint64_t audio_cap_retry_ms, audio_play_retry_ms, thread_retry_ms;
  uint64_t camera_retry_ms, encoder_retry_ms, capture_retry_ms, decoder_retry_ms;

  // Per-stream Go Live warning latches: each send-plane error class logs one CCORD_LOG_W per
  // stream (reset when the stream stops or restarts) so live failures are visible on stderr
  // without per-frame spam. warn_send_pcm may be set from the audio pthread (same plain-bool
  // cross-thread class as the PTT gate); the rest are main-thread only.
  bool warn_capture, warn_camera, warn_encoder, warn_encode, warn_send, warn_loopback;
  _Atomic bool warn_send_pcm;
  // The user cancelled the portal picker for this stream: offer the X11 default instead of
  // re-prompting on every retry. Reset with the stream.
  bool cap_portal_refused;
};


static inline int app_media_clamp_vol(int v)
{
  if (v < 0) return 0;
  if (v > 200) return 200;
  return v;
}

static inline bool app_media_open_retry_due(uint64_t now, uint64_t retry_ms)
{
  return retry_ms == 0 || now >= retry_ms;
}

static inline void app_media_copy_id(char *dst, const char *src)
{
  (void)snprintf(dst, MEDIA_ID_CAP, "%s", src ? src : "");
}

// app_media_audio.c
void app_media_clear_soundboard(ccord_app_media *m);
void app_media_discard_pending_soundboard(ccord_app_media *m);
void app_media_pump_stream_loopback(ccord_app_media *m, uint64_t now, bool loop_on, bool paused);
void app_media_pump_soundboard(ccord_app_media *m, uint64_t now);
void app_media_hotplug(void *ud, bool capture, bool added);
void app_media_close_inline(ccord_app_media *m);
void app_media_stop_thread(ccord_app_media *m);
void app_media_sync_live_thread(ccord_app_media *m, const ccord_app_media_voice *v, bool cap_suppressed,
                                bool ptt_idle, int in_vol, int out_vol, uint64_t now);
void app_media_sync_devices(ccord_app_media *m, uint64_t now);
void app_media_pump_inline(ccord_app_media *m, bool muted, bool deafened, bool cap_suppressed, bool ptt_idle,
                           int in_vol, int out_vol);

// app_media_video.c
void app_media_close_cam_tx(ccord_app_media *m, bool close_cam);
void app_media_close_send(ccord_app_media *m);
void app_media_cam_close_all(ccord_app_media *m);
void app_media_pump_send(ccord_app_media *m, const ccord_app_media_voice *v, uint64_t now, bool virtual_clock, bool stream_on, bool paused);
void app_media_pump_voice_camera(ccord_app_media *m, const ccord_app_media_voice *v, uint64_t now, bool virtual_clock, bool stream_on);
void app_media_pump_cameras(ccord_app_media *m, const ccord_app_media_voice *v, uint64_t now);
void app_media_pump_receive(ccord_app_media *m, uint64_t now);
