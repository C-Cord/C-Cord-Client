// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// M5 runtime media coordinator (m5-app-media). Owns the voice/video media plane that used to live
// inline in main.c's pump_voice_and_video: it syncs the Voice settings page (device ids, gains, PTT,
// VAD sensitivity, NS/AEC/AGC) to the audio pthread and the session, reopens audio devices on a
// settings change or hot-plug (falling back to the default when the chosen device vanishes), forwards
// per-user playback volumes, drives Go Live send (screen/portal capture or camera -> H.264 ->
// ccord_session_stream_send_frame, plus stream audio: the captured mic downmixed to mono, or the
// desktop/monitor loopback mix when "share audio" is enabled -> ccord_session_stream_send_pcm)
// and receive (ccord_session_stream_poll_frame -> H.264 decoder ->
// stage texture, and ccord_session_stream_poll_pcm -> mono-to-stereo playback mix), and decodes
// arrived soundboard CDN clips (ccord_sound_decode -> local speaker mix, bypassing the PTT-gated
// mic path).
//
// Every external effect goes through the injectable `ccord_app_media_ops` vtable so the coordinator
// is unit-testable headless with fakes (tests/test_app_media.c) and so this file never touches SDL,
// the session, the store or the renderer directly. main.c supplies the real ops.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/model/model.h>
#include <ccord/net/transport.h>   // ccord_camera_rx
#include <ccord/platform/capture.h>
#include <ccord/platform/platform.h>
#include <ccord/ui/ui.h>

#include "voice_audio_thread.h"

// Self voice snapshot the coordinator reads each frame (filled by the host from the store).
typedef struct ccord_app_media_voice {
  bool active;          // self is connected to a voice channel
  bool muted;           // mic gate: self_mute || self_deaf
  bool deafened;        // self_deaf
  bool camera;          // self_video (camera on while in voice)
  ccord_id guild_id;
  ccord_id channel_id;
} ccord_app_media_voice;

// Injectable host seams. `ud` is the opaque host pointer passed at create time. Read seams return
// current state; effect seams mirror the session/platform APIs the coordinator drives.
typedef struct ccord_app_media_ops {
  // ---- state reads ----
  void (*prefs)(void *ud, ccord_ui_voice_prefs *out);
  uint8_t (*user_volume)(void *ud, ccord_id user_id);
  bool (*key_held)(void *ud, int scancode);           // SDL_GetKeyboardState in the host
  bool (*global_ptt_held)(void *ud);                  // optional: portal push-to-talk shortcut held (works unfocused)
  uint64_t (*now_ms)(void *ud);
  bool (*dsp_available)(void *ud);
  void (*voice)(void *ud, ccord_app_media_voice *out);
  size_t (*voice_users)(void *ud, ccord_id *out, size_t max);   // other users in the active channel
  bool (*stream_active)(void *ud);
  void (*stream_config)(void *ud, ccord_stream_config *out);    // valid when stream_active
  bool (*stream_paused)(void *ud);                              // optional: true when self stream is paused
  bool (*stream_loopback_audio)(void *ud);                      // optional: Go Live desktop-audio sharing on
  bool (*stream_want_keyframe)(void *ud);                       // optional: consumes transport keyframe request
  bool (*watch_active)(void *ud);                              // watched stream active && !paused
  bool (*capture_source)(void *ud, ccord_capture_source *out);

  // ---- session intents ----
  ccord_status (*set_voice_config)(void *ud, const ccord_voice_config *cfg, ccord_error *err);
  ccord_status (*set_ptt_held)(void *ud, bool held, ccord_error *err);
  ccord_status (*set_user_volume)(void *ud, ccord_id user_id, uint8_t pct, ccord_error *err);
  ccord_status (*push_pcm)(void *ud, const int16_t *pcm, size_t frames, ccord_error *err);
  size_t (*pull_pcm)(void *ud, int16_t *pcm, size_t max_frames);
  ccord_status (*stream_send)(void *ud, const uint8_t *annexb, size_t len, uint32_t ts90k, bool keyframe,
                              ccord_error *err);
  bool (*stream_poll)(void *ud, const uint8_t **annexb, size_t *len, bool *keyframe);
  // Go Live stream audio (48 kHz mono S16LE). Optional: when null the coordinator skips stream
  // audio entirely (older hosts / unit fakes). Send pushes outbound broadcast audio; poll drains
  // the watched stream's audio so it can be mixed into local playback.
  ccord_status (*stream_send_pcm)(void *ud, const int16_t *pcm, size_t samples, ccord_error *err);
  size_t (*stream_poll_pcm)(void *ud, int16_t *out_pcm, size_t cap_samples);
  ccord_status (*voice_video_send)(void *ud, const uint8_t *annexb, size_t len, uint32_t ts90k, bool keyframe,
                                   ccord_error *err);   // optional: standalone voice camera H.264 TX
  ccord_status (*play_soundboard)(void *ud, ccord_id guild_id, ccord_id sound_id, ccord_error *err);
  bool (*pop_soundboard)(void *ud, const uint8_t **data, size_t *len, uint8_t *vol_pct);

  // ---- dedicated audio pthread (live mode) ----
  ccord_voice_audio_thread *(*thread_start)(void *ud, const ccord_voice_audio_io *io, ccord_error *err);
  void (*thread_controls)(void *ud, ccord_voice_audio_thread *t, bool muted, bool deafened, int in_vol,
                          int out_vol);
  void (*thread_stop)(void *ud, ccord_voice_audio_thread *t);

  // ---- inline audio devices (mock mode) + enumeration / hot-plug ----
  ccord_audio *(*audio_open)(void *ud, bool capture, const char *device_id, ccord_error *err);
  void (*audio_close)(void *ud, ccord_audio *a);
  size_t (*audio_read)(void *ud, ccord_audio *a, int16_t *pcm, size_t frames);
  size_t (*audio_write)(void *ud, ccord_audio *a, const int16_t *pcm, size_t frames);
  size_t (*devices_list)(void *ud, bool capture, ccord_audio_device_info *out, size_t max);
  void (*hotplug_register)(void *ud, ccord_audio_hotplug_fn fn, void *cb_ud);

  // ---- camera ----
  ccord_camera *(*camera_open)(void *ud, const char *device_id, ccord_error *err);
  bool (*camera_grab)(void *ud, ccord_camera *c, uint8_t *bgra, size_t cap, int *out_w, int *out_h);
  void (*camera_close)(void *ud, ccord_camera *c);

  // ---- screen / portal capture ----
  ccord_capture *(*capture_open)(void *ud, const ccord_capture_source *src, ccord_error *err);
  bool (*capture_grab)(void *ud, ccord_capture *c, ccord_capture_frame *out);
  bool (*capture_failed)(void *ud, const ccord_capture *c);
  void (*capture_close)(void *ud, ccord_capture *c);

  // ---- desktop / monitor loopback audio (Go Live "share audio") ----
  // Optional seams (host supplies ccord_loopback_audio_*). While loopback is enabled and a stream
  // is active, the coordinator reads 48 kHz mono 20 ms desktop-audio frames here and sends them as
  // the stream audio INSTEAD of the mic downmix; mic voice TX (push_pcm) is unaffected.
  ccord_loopback_audio *(*loopback_open)(void *ud, ccord_error *err);
  size_t (*loopback_read)(void *ud, ccord_loopback_audio *l, int16_t *pcm, size_t max_samples);
  bool (*loopback_failed)(void *ud, const ccord_loopback_audio *l);   // optional: source died
  void (*loopback_close)(void *ud, ccord_loopback_audio *l);

  // ---- H.264 encoder (Go Live send) ----
  void (*resolve_dims)(void *ud, int src_w, int src_h, int target_h, int *out_w, int *out_h);
  ccord_video_stream *(*video_open)(void *ud, const ccord_video_config *cfg, ccord_error *err);
  void (*video_close)(void *ud, ccord_video_stream *v);
  int (*video_fps)(void *ud, const ccord_video_stream *v);
  bool (*video_keyframe)(void *ud, const ccord_video_stream *v);
  ccord_status (*video_encode_bgra)(void *ud, ccord_video_stream *v, const uint8_t *bgra, int w, int h,
                                    int stride, uint8_t *out_pkt, size_t cap, size_t *out_len, ccord_error *err);
  void (*video_force_keyframe)(void *ud, ccord_video_stream *v);   // optional: force next frame to be IDR
  const uint8_t *(*video_rgba)(void *ud, const ccord_video_stream *v, int *out_w, int *out_h);

  // ---- H.264 decoder (Go Live receive) ----
  ccord_video_decoder *(*decoder_open)(void *ud, ccord_error *err);
  ccord_status (*decoder_feed)(void *ud, ccord_video_decoder *d, const uint8_t *annexb, size_t len,
                               ccord_error *err);
  const uint8_t *(*decoder_rgba)(void *ud, const ccord_video_decoder *d, int *out_w, int *out_h);
  void (*decoder_close)(void *ud, ccord_video_decoder *d);

  // ---- soundboard decode + render upload ----
  ccord_status (*sound_decode)(void *ud, const uint8_t *data, size_t len, int16_t *out, size_t cap,
                               size_t *out_samples, ccord_error *err);
  ccord_status (*upload_rgba)(void *ud, const char *key, const uint8_t *rgba, int w, int h, ccord_error *err);

  // ---- remote voice cameras ----
  // All optional: without camera_senders + camera_poll there is no camera receive (Go Live is unaffected).
  // camera_senders lists the announced senders of the call; only `decoding` ones (at most CCORD_CAM_DECODE_MAX) get a
  // decoder. camera_poll pops one Annex-B access unit of a sender (buffer valid until the next camera_poll).
  // texture_evict drops the texture uploaded under a key; decoder_set_max_dim clamps a decoder's output size.
  size_t (*camera_senders)(void *ud, ccord_camera_rx *out, size_t max);
  bool (*camera_poll)(void *ud, ccord_id user_id, const uint8_t **annexb, size_t *len, bool *keyframe);
  void (*texture_evict)(void *ud, const char *key);
  void (*decoder_set_max_dim)(void *ud, ccord_video_decoder *d, int max_w, int max_h);
} ccord_app_media_ops;

typedef struct ccord_app_media ccord_app_media;

// Push-to-talk gate used each frame: true when the mic may transmit. Always true outside PTT mode; in PTT mode true
// while the bound scancode is held (ops->key_held) or the portal shortcut is held (optional ops->global_ptt_held).
bool ccord_app_media_ptt_open(const ccord_app_media_ops *ops, void *ud, const ccord_ui_voice_prefs *prefs);

typedef struct ccord_app_media_config {
  const ccord_app_media_ops *ops;   // required; every seam the coordinator uses must be non-null
  void *ud;                         // host pointer forwarded to every op
} ccord_app_media_config;

// Creates the coordinator and registers the audio hot-plug callback. Returns null and sets err on
// failure (missing ops / out of memory).
[[nodiscard]] ccord_app_media *ccord_app_media_create(const ccord_app_media_config *cfg, ccord_error *err);
// Stops/joins the audio thread, closes every device/capture/camera/encoder/decoder, unregisters
// the hot-plug callback, and nulls *pm. Null-safe and idempotent. Call before destroying session/SDL.
void ccord_app_media_destroy(ccord_app_media **pm);
// One media frame. `live` selects the dedicated audio pthread (true) vs inline devices (false);
// `virtual_clock` (headless screenshot/verify) skips real SDL audio I/O but still runs the video plane.
void ccord_app_media_pump(ccord_app_media *m, bool live, bool virtual_clock);
// Forwards a soundboard play intent to the session; when Discord accepts and the governed CDN clip
// arrives in the session media queue, pump() decodes and mixes it into local playback.
[[nodiscard]] ccord_status ccord_app_media_play_soundboard(ccord_app_media *m, ccord_id guild_id,
                                                           ccord_id sound_id, ccord_error *err);
// Coordinator-level Go Live desktop-audio loopback switch (host UI toggle / verify harness). The
// optional ops->stream_loopback_audio read seam can enable it too; either switch being on activates
// loopback TX while a stream is active (requires the loopback_* seams). Null-safe.
void ccord_app_media_set_loopback_audio(ccord_app_media *m, bool on);
bool ccord_app_media_loopback_audio(const ccord_app_media *m);

// Per-user remote camera state for tests and the multi-camera slice. `key` is the texture name "__cam__:<uid>".
// frame_hash = FNV-1a over 4096 bytes of the last uploaded RGBA sampled at a fixed stride; center_rgb = its centre pixel.
typedef struct ccord_app_media_cam_debug {
  bool open;                       // a decoder is open for this user
  uint32_t generation;
  int w, h;                        // size of the last uploaded frame (0 before the first)
  uint64_t frames;                 // access units decoded
  uint64_t decode_errors;
  uint64_t dropped_until_idr;      // non-keyframes discarded while waiting for an IDR
  uint64_t frame_hash;
  uint8_t center_rgb[3];
  char key[40];
} ccord_app_media_cam_debug;

// Number of remote camera slots in use (admitted senders, whether or not their decoder opened yet).
size_t ccord_app_media_cam_count(const ccord_app_media *m);
// Fills `out` for one user's slot; false when that user has none. Null-safe.
bool ccord_app_media_cam_debug_of(const ccord_app_media *m, ccord_id user_id, ccord_app_media_cam_debug *out);
