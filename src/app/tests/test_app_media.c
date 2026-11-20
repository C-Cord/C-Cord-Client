// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M5 app_media coordinator unit tests. Every external seam is a fake op (no SDL / session / store /
// renderer is linked into the behaviour under test), so the coordination logic — settings sync, PTT,
// per-user volumes + eviction, device selection + hot-plug fallback (inline + live pthread), open
// retry backoff, Go Live send/receive, desktop-audio loopback TX, and local soundboard decode/mix —
// is observed directly through recorded calls. app_media.c is compiled in; only ccord_core
// (alloc / error / log) and the platform loopback fake are real.
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "app_media.h"

#include <ccord/core/log.h>
#include <ccord/platform/capture.h>

#include "ccord_test.h"

#define T_MAX_USERS 72
#define T_MAX_DEVS 8
#define T_FRAME 960u

// Opaque handle sentinels: app_media only passes these back to the ops, never dereferences them.
alignas(max_align_t) static int h_audio, h_camera, h_capture, h_video, h_decoder, h_thread, h_loopback;

typedef struct t_vol {
  ccord_id user;
  uint8_t pct;
} t_vol;

typedef struct fake {
  // ---- inputs the test sets ----
  ccord_ui_voice_prefs prefs;
  ccord_app_media_voice voice;
  bool dsp;
  bool key_down;
  uint64_t now;
  ccord_id users[T_MAX_USERS];
  size_t nusers;
  t_vol vols[T_MAX_USERS];
  size_t nvols;
  bool stream_active;
  bool stream_paused;
  bool stream_loopback_audio;
  bool want_keyframe;
  ccord_stream_config stream_cfg;
  bool watch_active;
  bool have_capture_source;
  ccord_audio_device_info in_devs[T_MAX_DEVS];
  size_t n_in;
  ccord_audio_device_info out_devs[T_MAX_DEVS];
  size_t n_out;
  bool capture_has_frame;
  bool camera_has_frame;
  bool poll_have;
  bool pull_silent;
  bool stream_pcm_poll_have;
  int16_t stream_pcm_poll_val;
  bool stream_send_pcm_fail;
  int16_t loopback_val;
  size_t loopback_emit_samples;
  bool fail_loopback_open;
  bool loopback_dead;
  bool sb_ready;
  uint8_t sb_vol_pct;
  size_t sb_decode_samples;   // interleaved samples the fake decoder emits
  bool play_sb_fail;
  bool encode_fail;
  bool fail_audio_open;
  bool fail_thread_start;
  bool fail_video_open;
  bool fail_hw_video_open;      // video_open fails unless preferred_encoder is software
  bool fail_camera_open;
  bool fail_capture_open;
  bool fail_portal_open;        // capture_open fails for PORTAL-backend sources
  bool fail_x11_open;           // capture_open fails for X11-backend sources
  bool portal_open_cancel;      // portal capture failure is PERMISSION (user cancelled the picker)
  bool fail_decoder_open;
  bool stream_send_fail;

  // ---- recorded calls ----
  int cfg_calls;
  ccord_voice_config last_cfg;
  int ptt_calls;
  bool last_ptt;
  int vol_calls;
  ccord_id last_vol_user;
  uint8_t last_vol_pct;
  int push_calls;
  size_t push_frames_total;
  int16_t push_first[4];
  int pull_calls;
  int thread_starts, thread_stops, thread_controls;
  ccord_voice_audio_io last_thread_io;
  bool thread_muted, thread_deaf;
  int thread_in, thread_out;
  int audio_opens, audio_closes, audio_reads, audio_writes;
  bool last_open_capture;
  char last_open_id[64];
  int16_t write_first[4];
  int devices_list_calls;
  int hotplug_regs;
  ccord_audio_hotplug_fn hotplug_fn;
  void *hotplug_ud;
  int video_opens, video_closes, encode_calls, video_rgba_calls;
  ccord_video_config last_vcfg;
  char last_enc_pref[32];
  int capture_opens, capture_closes, capture_grabs, capture_failed_calls;
  int portal_opens, x11_opens;
  ccord_capture_backend_id last_capture_backend;
  char last_capture_id[64];
  int camera_opens, camera_closes, camera_grabs;
  int stream_send_calls;
  int voice_video_send_calls;
  int force_keyframe_calls;
  uint32_t last_ts90k;
  bool last_keyframe;
  size_t last_send_len;
  int stream_send_pcm_calls;
  size_t stream_send_pcm_samples;
  int16_t stream_send_pcm_first[4];
  int stream_poll_pcm_calls;
  size_t stream_poll_pcm_cap;
  int loopback_opens, loopback_reads, loopback_failed_calls, loopback_closes;
  int decoder_opens, decoder_closes, decoder_feeds, decoder_rgba_calls;
  int upload_calls;
  char last_upload_key[64];
  // remote cameras
  ccord_camera_rx cam_list[8];
  size_t cam_n;
  int cam_pending[8];        // queued access units per cam_list entry
  bool cam_first_delta[8];   // the next queued AU of that entry is a P-frame, then keyframes
  int cam_polls;
  int open_decoders, max_open_decoders;
  int max_dim_calls, max_dim_w, max_dim_h;
  int evicts;
  char evict_keys[16][40];
  int cam_uploads;
  char cam_upload_keys[32][40];
  int play_sb_calls, pop_sb_calls, sound_decode_calls;
  ccord_id last_sb_guild, last_sb_sound;
} fake;

static fake F;

static void fake_reset(void)
{
  memset(&F, 0, sizeof F);
  F.prefs.input_volume = 100;
  F.prefs.output_volume = 100;
  F.prefs.vad_sensitivity = 50;
  F.dsp = true;
  F.now = 1000;
  F.sb_vol_pct = 100;
  F.sb_decode_samples = T_FRAME * 2;   // one 20 ms frame by default
  F.loopback_val = 4321;
  F.loopback_emit_samples = T_FRAME;
  F.voice.active = true;
  F.voice.guild_id = 1;
  F.voice.channel_id = 2;
}

// ---- read ops --------------------------------------------------------------------------------

static void f_prefs(void *ud, ccord_ui_voice_prefs *out) { (void)ud; *out = F.prefs; }

static uint8_t f_user_volume(void *ud, ccord_id user)
{
  (void)ud;
  for (size_t i = 0; i < F.nvols; i++) {
    if (F.vols[i].user == user) return F.vols[i].pct;
  }
  return 100;
}

static bool f_key_held(void *ud, int scancode) { (void)ud; (void)scancode; return F.key_down; }
static uint64_t f_now(void *ud) { (void)ud; return F.now; }
static bool f_dsp(void *ud) { (void)ud; return F.dsp; }
static void f_voice(void *ud, ccord_app_media_voice *out) { (void)ud; *out = F.voice; }

static size_t f_voice_users(void *ud, ccord_id *out, size_t max)
{
  (void)ud;
  size_t n = F.nusers < max ? F.nusers : max;
  for (size_t i = 0; i < n; i++) out[i] = F.users[i];
  return n;
}

static bool f_stream_active(void *ud) { (void)ud; return F.stream_active; }
static void f_stream_config(void *ud, ccord_stream_config *out) { (void)ud; *out = F.stream_cfg; }
static bool f_stream_paused(void *ud) { (void)ud; return F.stream_paused; }
static bool f_stream_loopback_audio(void *ud) { (void)ud; return F.stream_loopback_audio; }
static bool f_stream_want_keyframe(void *ud)
{
  (void)ud;
  bool w = F.want_keyframe;
  F.want_keyframe = false;
  return w;
}
static bool f_watch_active(void *ud) { (void)ud; return F.watch_active; }

static bool f_capture_source(void *ud, ccord_capture_source *out)
{
  (void)ud;
  if (!F.have_capture_source) return false;
  memset(out, 0, sizeof *out);
  out->kind = CCORD_CAPTURE_SCREEN;
  out->backend = CCORD_CAPBE_FAKE;
  snprintf(out->id, sizeof out->id, "fake:screen:0");
  snprintf(out->title, sizeof out->title, "Fake screen");
  out->width = 640;
  out->height = 360;
  return true;
}

// ---- session intent ops ----------------------------------------------------------------------

static ccord_status f_set_voice_config(void *ud, const ccord_voice_config *cfg, ccord_error *err)
{
  (void)ud;
  ccord_error_clear(err);
  F.cfg_calls++;
  F.last_cfg = *cfg;
  return CCORD_OK;
}

static ccord_status f_set_ptt(void *ud, bool held, ccord_error *err)
{
  (void)ud;
  ccord_error_clear(err);
  F.ptt_calls++;
  F.last_ptt = held;
  return CCORD_OK;
}

static ccord_status f_set_user_volume(void *ud, ccord_id user, uint8_t pct, ccord_error *err)
{
  (void)ud;
  ccord_error_clear(err);
  F.vol_calls++;
  F.last_vol_user = user;
  F.last_vol_pct = pct;
  return CCORD_OK;
}

static ccord_status f_push_pcm(void *ud, const int16_t *pcm, size_t frames, ccord_error *err)
{
  (void)ud;
  ccord_error_clear(err);
  F.push_calls++;
  F.push_frames_total += frames;
  for (size_t i = 0; i < 4 && i < frames * 2; i++) F.push_first[i] = pcm[i];
  return CCORD_OK;
}

static size_t f_pull_pcm(void *ud, int16_t *pcm, size_t max_frames)
{
  (void)ud;
  F.pull_calls++;
  if (F.pull_silent) return 0;
  size_t n = max_frames < T_FRAME ? max_frames : T_FRAME;
  for (size_t i = 0; i < n * 2; i++) pcm[i] = 500;
  return n;
}

static ccord_status f_stream_send(void *ud, const uint8_t *annexb, size_t len, uint32_t ts90k, bool keyframe,
                                  ccord_error *err)
{
  (void)ud;
  (void)annexb;
  F.stream_send_calls++;
  F.last_ts90k = ts90k;
  F.last_keyframe = keyframe;
  F.last_send_len = len;
  if (F.stream_send_fail) return ccord_error_set(err, CCORD_ERR_IO, "fake: stream send failed");
  ccord_error_clear(err);
  return CCORD_OK;
}

static bool f_stream_poll(void *ud, const uint8_t **annexb, size_t *len, bool *keyframe)
{
  (void)ud;
  if (!F.poll_have) return false;
  F.poll_have = false;   // one frame per pump
  static const uint8_t pkt[8] = {0, 0, 0, 1, 0x65, 1, 2, 3};
  *annexb = pkt;
  *len = sizeof pkt;
  *keyframe = true;
  return true;
}

static ccord_status f_stream_send_pcm(void *ud, const int16_t *pcm, size_t samples, ccord_error *err)
{
  (void)ud;
  F.stream_send_pcm_calls++;
  F.stream_send_pcm_samples = samples;
  for (size_t i = 0; i < 4 && i < samples; i++) F.stream_send_pcm_first[i] = pcm[i];
  if (F.stream_send_pcm_fail) return ccord_error_set(err, CCORD_ERR_INVALID, "fake: no stream");
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status f_voice_video_send(void *ud, const uint8_t *annexb, size_t len, uint32_t ts90k, bool keyframe,
                                       ccord_error *err)
{
  (void)ud;
  (void)annexb;
  F.voice_video_send_calls++;
  F.last_ts90k = ts90k;
  F.last_keyframe = keyframe;
  F.last_send_len = len;
  ccord_error_clear(err);
  return CCORD_OK;
}

static size_t f_stream_poll_pcm(void *ud, int16_t *out_pcm, size_t cap_samples)
{
  (void)ud;
  F.stream_poll_pcm_calls++;
  F.stream_poll_pcm_cap = cap_samples;
  if (!F.stream_pcm_poll_have) return 0;
  F.stream_pcm_poll_have = false;   // one batch per pump
  size_t n = cap_samples < T_FRAME ? cap_samples : T_FRAME;
  for (size_t i = 0; i < n; i++) out_pcm[i] = F.stream_pcm_poll_val;
  return n;
}

static ccord_status f_play_soundboard(void *ud, ccord_id guild, ccord_id sound, ccord_error *err)
{
  (void)ud;
  F.play_sb_calls++;
  F.last_sb_guild = guild;
  F.last_sb_sound = sound;
  if (F.play_sb_fail) return ccord_error_set(err, CCORD_ERR_INVALID, "fake: no voice");
  ccord_error_clear(err);
  return CCORD_OK;
}

static bool f_pop_soundboard(void *ud, const uint8_t **data, size_t *len, uint8_t *vol_pct)
{
  (void)ud;
  F.pop_sb_calls++;
  if (!F.sb_ready) return false;
  F.sb_ready = false;
  static const uint8_t bytes[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  *data = bytes;
  *len = sizeof bytes;
  *vol_pct = F.sb_vol_pct;
  return true;
}

// ---- audio pthread ops -----------------------------------------------------------------------

static ccord_voice_audio_thread *f_thread_start(void *ud, const ccord_voice_audio_io *io, ccord_error *err)
{
  (void)ud;
  F.thread_starts++;
  if (io) F.last_thread_io = *io;
  if (F.fail_thread_start) {
    (void)ccord_error_set(err, CCORD_ERR_IO, "fake: thread start failed");
    return nullptr;
  }
  ccord_error_clear(err);
  return (ccord_voice_audio_thread *)&h_thread;
}

static void f_thread_controls(void *ud, ccord_voice_audio_thread *t, bool muted, bool deaf, int in_vol, int out_vol)
{
  (void)ud;
  (void)t;
  F.thread_controls++;
  F.thread_muted = muted;
  F.thread_deaf = deaf;
  F.thread_in = in_vol;
  F.thread_out = out_vol;
}

static void f_thread_stop(void *ud, ccord_voice_audio_thread *t)
{
  (void)ud;
  (void)t;
  F.thread_stops++;
}

// ---- inline audio + device ops ---------------------------------------------------------------

static ccord_audio *f_audio_open(void *ud, bool capture, const char *device_id, ccord_error *err)
{
  (void)ud;
  F.audio_opens++;
  F.last_open_capture = capture;
  snprintf(F.last_open_id, sizeof F.last_open_id, "%s", device_id ? device_id : "");
  if (F.fail_audio_open) {
    (void)ccord_error_set(err, CCORD_ERR_IO, "fake: audio open failed");
    return nullptr;
  }
  ccord_error_clear(err);
  return (ccord_audio *)&h_audio;
}

static void f_audio_close(void *ud, ccord_audio *a) { (void)ud; (void)a; F.audio_closes++; }

static size_t f_audio_read(void *ud, ccord_audio *a, int16_t *pcm, size_t frames)
{
  (void)ud;
  (void)a;
  F.audio_reads++;
  size_t n = frames < T_FRAME ? frames : T_FRAME;
  for (size_t i = 0; i < n * 2; i++) pcm[i] = 1000;
  return n;
}

static size_t f_audio_write(void *ud, ccord_audio *a, const int16_t *pcm, size_t frames)
{
  (void)ud;
  (void)a;
  F.audio_writes++;
  for (size_t i = 0; i < 4 && i < frames * 2; i++) F.write_first[i] = pcm[i];
  return frames;
}

static size_t f_devices_list(void *ud, bool capture, ccord_audio_device_info *out, size_t max)
{
  (void)ud;
  F.devices_list_calls++;
  const ccord_audio_device_info *src = capture ? F.in_devs : F.out_devs;
  size_t n = capture ? F.n_in : F.n_out;
  if (n > max) n = max;
  for (size_t i = 0; i < n; i++) out[i] = src[i];
  return n;
}

static void f_hotplug_register(void *ud, ccord_audio_hotplug_fn fn, void *cb_ud)
{
  (void)ud;
  F.hotplug_regs++;
  F.hotplug_fn = fn;
  F.hotplug_ud = cb_ud;
}

// ---- camera / capture ops --------------------------------------------------------------------

static ccord_camera *f_camera_open(void *ud, const char *id, ccord_error *err)
{
  (void)ud;
  (void)id;
  F.camera_opens++;
  if (F.fail_camera_open) {
    (void)ccord_error_set(err, CCORD_ERR_IO, "fake: camera open failed");
    return nullptr;
  }
  ccord_error_clear(err);
  return (ccord_camera *)&h_camera;
}

static bool f_camera_grab(void *ud, ccord_camera *c, uint8_t *bgra, size_t cap, int *w, int *h)
{
  (void)ud;
  (void)c;
  F.camera_grabs++;
  if (!F.camera_has_frame || cap < 16) return false;
  memset(bgra, 0x7f, 16);
  *w = 2;
  *h = 2;
  return true;
}

static void f_camera_close(void *ud, ccord_camera *c) { (void)ud; (void)c; F.camera_closes++; }

static ccord_capture *f_capture_open(void *ud, const ccord_capture_source *src, ccord_error *err)
{
  (void)ud;
  F.capture_opens++;
  F.last_capture_backend = src->backend;
  snprintf(F.last_capture_id, sizeof F.last_capture_id, "%s", src->id);
  if (src->backend == CCORD_CAPBE_PORTAL) F.portal_opens++;
  if (src->backend == CCORD_CAPBE_X11) F.x11_opens++;
  bool fail = F.fail_capture_open || (src->backend == CCORD_CAPBE_PORTAL && F.fail_portal_open) ||
              (src->backend == CCORD_CAPBE_X11 && F.fail_x11_open);
  if (fail) {
    ccord_status code =
        (src->backend == CCORD_CAPBE_PORTAL && F.portal_open_cancel) ? CCORD_ERR_PERMISSION : CCORD_ERR_IO;
    (void)ccord_error_set(err, code, "fake: capture open failed");
    return nullptr;
  }
  ccord_error_clear(err);
  return (ccord_capture *)&h_capture;
}

static bool f_capture_grab(void *ud, ccord_capture *c, ccord_capture_frame *out)
{
  (void)ud;
  (void)c;
  F.capture_grabs++;
  if (!F.capture_has_frame) return false;
  static uint8_t pixels[64 * 4];
  out->data = pixels;
  out->width = 8;
  out->height = 8;
  out->stride = 8 * 4;
  return true;
}

static bool f_capture_failed(void *ud, const ccord_capture *c) { (void)ud; (void)c; F.capture_failed_calls++; return false; }
static void f_capture_close(void *ud, ccord_capture *c) { (void)ud; (void)c; F.capture_closes++; }

static ccord_loopback_audio *f_loopback_open(void *ud, ccord_error *err)
{
  (void)ud;
  F.loopback_opens++;
  if (F.fail_loopback_open) {
    (void)ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "fake: no monitor source");
    return nullptr;
  }
  ccord_error_clear(err);
  return (ccord_loopback_audio *)&h_loopback;
}

static size_t f_loopback_read(void *ud, ccord_loopback_audio *l, int16_t *pcm, size_t max_samples)
{
  (void)ud;
  (void)l;
  F.loopback_reads++;
  size_t n = F.loopback_emit_samples < max_samples ? F.loopback_emit_samples : max_samples;
  for (size_t i = 0; i < n; i++) pcm[i] = F.loopback_val;
  return n;
}

static bool f_loopback_failed(void *ud, const ccord_loopback_audio *l)
{
  (void)ud;
  (void)l;
  F.loopback_failed_calls++;
  return F.loopback_dead;
}

static void f_loopback_close(void *ud, ccord_loopback_audio *l)
{
  (void)ud;
  (void)l;
  F.loopback_closes++;
}

// ---- video encoder / decoder ops -------------------------------------------------------------

static void f_resolve_dims(void *ud, int sw, int sh, int th, int *w, int *h)
{
  (void)ud;
  (void)sw;
  (void)sh;
  if (th == 1080) { *w = 1920; *h = 1080; }
  else if (th == 1440) { *w = 2560; *h = 1440; }
  else if (th == 0) { *w = 1280; *h = 720; }
  else { *w = 1280; *h = 720; }
}

static ccord_video_stream *f_video_open(void *ud, const ccord_video_config *cfg, ccord_error *err)
{
  (void)ud;
  F.video_opens++;
  F.last_vcfg = *cfg;
  snprintf(F.last_enc_pref, sizeof F.last_enc_pref, "%s", cfg->preferred_encoder ? cfg->preferred_encoder : "");
  bool software = cfg->preferred_encoder && (strcmp(cfg->preferred_encoder, "libx264") == 0 ||
                                             strcmp(cfg->preferred_encoder, "libopenh264") == 0);
  if (F.fail_video_open || (F.fail_hw_video_open && !software)) {
    (void)ccord_error_set(err, CCORD_ERR_IO, "fake: video open failed");
    return nullptr;
  }
  ccord_error_clear(err);
  return (ccord_video_stream *)&h_video;
}

static void f_video_close(void *ud, ccord_video_stream *v) { (void)ud; (void)v; F.video_closes++; }
static int f_video_fps(void *ud, const ccord_video_stream *v) { (void)ud; (void)v; return 30; }
static bool f_video_keyframe(void *ud, const ccord_video_stream *v) { (void)ud; (void)v; return true; }

static ccord_status f_video_encode(void *ud, ccord_video_stream *v, const uint8_t *bgra, int w, int h, int stride,
                                   uint8_t *pkt, size_t cap, size_t *len, ccord_error *err)
{
  (void)ud;
  (void)v;
  (void)bgra;
  (void)w;
  (void)h;
  (void)stride;
  F.encode_calls++;
  if (F.encode_fail) return ccord_error_set(err, CCORD_ERR_IO, "fake: encode");
  if (cap < 4) return ccord_error_set(err, CCORD_ERR_RANGE, "fake: cap");
  pkt[0] = 0; pkt[1] = 0; pkt[2] = 0; pkt[3] = 1;
  *len = 4;
  ccord_error_clear(err);
  return CCORD_OK;
}

static void f_video_force_keyframe(void *ud, ccord_video_stream *v)
{
  (void)ud;
  (void)v;
  F.force_keyframe_calls++;
}

static const uint8_t *f_video_rgba(void *ud, const ccord_video_stream *v, int *w, int *h)
{
  (void)ud;
  (void)v;
  F.video_rgba_calls++;
  static const uint8_t rgba[16] = {0};
  *w = 2;
  *h = 2;
  return rgba;
}

static ccord_video_decoder *f_decoder_open(void *ud, ccord_error *err)
{
  (void)ud;
  F.decoder_opens++;
  if (F.fail_decoder_open) {
    (void)ccord_error_set(err, CCORD_ERR_IO, "fake: decoder open failed");
    return nullptr;
  }
  ccord_error_clear(err);
  if (++F.open_decoders > F.max_open_decoders) F.max_open_decoders = F.open_decoders;
  return (ccord_video_decoder *)&h_decoder;
}

static ccord_status f_decoder_feed(void *ud, ccord_video_decoder *d, const uint8_t *annexb, size_t len, ccord_error *err)
{
  (void)ud;
  (void)d;
  (void)annexb;
  (void)len;
  ccord_error_clear(err);
  F.decoder_feeds++;
  return CCORD_OK;
}

static const uint8_t *f_decoder_rgba(void *ud, const ccord_video_decoder *d, int *w, int *h)
{
  (void)ud;
  (void)d;
  F.decoder_rgba_calls++;
  static const uint8_t rgba[16] = {0};
  *w = 2;
  *h = 2;
  return rgba;
}

static void f_decoder_close(void *ud, ccord_video_decoder *d)
{
  (void)ud;
  (void)d;
  F.decoder_closes++;
  F.open_decoders--;
}

static ccord_status f_sound_decode(void *ud, const uint8_t *data, size_t len, int16_t *out, size_t cap,
                                   size_t *samples, ccord_error *err)
{
  (void)ud;
  (void)data;
  (void)len;
  ccord_error_clear(err);
  F.sound_decode_calls++;
  size_t n = F.sb_decode_samples;
  if (n > cap) n = cap;
  for (size_t i = 0; i < n; i++) out[i] = 2000;
  *samples = n;
  return CCORD_OK;
}

static ccord_status f_upload(void *ud, const char *key, const uint8_t *rgba, int w, int h, ccord_error *err)
{
  (void)ud;
  (void)rgba;
  (void)w;
  (void)h;
  ccord_error_clear(err);
  F.upload_calls++;
  snprintf(F.last_upload_key, sizeof F.last_upload_key, "%s", key);
  if (strncmp(key, "__cam__:", 8) == 0 && F.cam_uploads < 32) {
    snprintf(F.cam_upload_keys[F.cam_uploads++], 40, "%s", key);
  }
  return CCORD_OK;
}

static size_t f_camera_senders(void *ud, ccord_camera_rx *out, size_t max)
{
  (void)ud;
  size_t n = F.cam_n < max ? F.cam_n : max;
  for (size_t i = 0; i < n; i++) out[i] = F.cam_list[i];
  return n;
}

static bool f_camera_poll(void *ud, ccord_id user, const uint8_t **annexb, size_t *len, bool *keyframe)
{
  (void)ud;
  F.cam_polls++;
  for (size_t i = 0; i < F.cam_n; i++) {
    if (F.cam_list[i].user_id != user || F.cam_pending[i] <= 0) continue;
    F.cam_pending[i]--;
    static const uint8_t pkt[8] = {0, 0, 0, 1, 0x65, 1, 2, 3};
    *annexb = pkt;
    *len = sizeof pkt;
    *keyframe = !F.cam_first_delta[i];
    F.cam_first_delta[i] = false;
    return true;
  }
  return false;
}

static void f_texture_evict(void *ud, const char *key)
{
  (void)ud;
  if (F.evicts < 16) snprintf(F.evict_keys[F.evicts], 40, "%s", key);
  F.evicts++;
}

static void f_decoder_set_max_dim(void *ud, ccord_video_decoder *d, int w, int h)
{
  (void)ud;
  (void)d;
  F.max_dim_calls++;
  F.max_dim_w = w;
  F.max_dim_h = h;
}

static const ccord_app_media_ops k_ops = {
  .prefs = f_prefs,
  .user_volume = f_user_volume,
  .key_held = f_key_held,
  .now_ms = f_now,
  .dsp_available = f_dsp,
  .voice = f_voice,
  .voice_users = f_voice_users,
  .stream_active = f_stream_active,
  .stream_config = f_stream_config,
  .stream_paused = f_stream_paused,
  .stream_loopback_audio = f_stream_loopback_audio,
  .stream_want_keyframe = f_stream_want_keyframe,
  .watch_active = f_watch_active,
  .capture_source = f_capture_source,
  .set_voice_config = f_set_voice_config,
  .set_ptt_held = f_set_ptt,
  .set_user_volume = f_set_user_volume,
  .push_pcm = f_push_pcm,
  .pull_pcm = f_pull_pcm,
  .stream_send = f_stream_send,
  .stream_poll = f_stream_poll,
  .stream_send_pcm = f_stream_send_pcm,
  .stream_poll_pcm = f_stream_poll_pcm,
  .voice_video_send = f_voice_video_send,
  .play_soundboard = f_play_soundboard,
  .pop_soundboard = f_pop_soundboard,
  .thread_start = f_thread_start,
  .thread_controls = f_thread_controls,
  .thread_stop = f_thread_stop,
  .audio_open = f_audio_open,
  .audio_close = f_audio_close,
  .audio_read = f_audio_read,
  .audio_write = f_audio_write,
  .devices_list = f_devices_list,
  .hotplug_register = f_hotplug_register,
  .camera_open = f_camera_open,
  .camera_grab = f_camera_grab,
  .camera_close = f_camera_close,
  .capture_open = f_capture_open,
  .capture_grab = f_capture_grab,
  .capture_failed = f_capture_failed,
  .capture_close = f_capture_close,
  .loopback_open = f_loopback_open,
  .loopback_read = f_loopback_read,
  .loopback_failed = f_loopback_failed,
  .loopback_close = f_loopback_close,
  .resolve_dims = f_resolve_dims,
  .video_open = f_video_open,
  .video_close = f_video_close,
  .video_fps = f_video_fps,
  .video_keyframe = f_video_keyframe,
  .video_encode_bgra = f_video_encode,
  .video_force_keyframe = f_video_force_keyframe,
  .video_rgba = f_video_rgba,
  .decoder_open = f_decoder_open,
  .decoder_feed = f_decoder_feed,
  .decoder_rgba = f_decoder_rgba,
  .decoder_close = f_decoder_close,
  .sound_decode = f_sound_decode,
  .upload_rgba = f_upload,
  .camera_senders = f_camera_senders,
  .camera_poll = f_camera_poll,
  .texture_evict = f_texture_evict,
  .decoder_set_max_dim = f_decoder_set_max_dim,
};

static ccord_app_media *make(void)
{
  ccord_app_media_config cfg = {.ops = &k_ops, .ud = &F};
  ccord_error err;
  ccord_app_media *m = ccord_app_media_create(&cfg, &err);
  CT_CHECK(m != nullptr);
  return m;
}

static void add_dev(bool capture, const char *id)
{
  ccord_audio_device_info *d = capture ? &F.in_devs[F.n_in] : &F.out_devs[F.n_out];
  snprintf(d->id, sizeof d->id, "%s", id);
  snprintf(d->name, sizeof d->name, "dev %s", id);
  if (capture) F.n_in++;
  else F.n_out++;
}

static void set_vol(ccord_id user, uint8_t pct)
{
  for (size_t i = 0; i < F.nvols; i++) {
    if (F.vols[i].user == user) {
      F.vols[i].pct = pct;
      return;
    }
  }
  F.vols[F.nvols].user = user;
  F.vols[F.nvols].pct = pct;
  F.nvols++;
}

// ---- warning-log capture (per-stream once-latch diagnostics) ------------------------------------

static struct {
  int warns;
  int capture_open, camera_open, encoder_open, fallback, encode, send, send_pcm, loopback_open, loopback_died;
} LOG;

static void log_sink(ccord_log_level level, const char *line, void *ud)
{
  (void)ud;
  if (level != CCORD_LOG_WARN) return;
  LOG.warns++;
  if (strstr(line, "screen capture open failed")) LOG.capture_open++;
  if (strstr(line, "camera open failed")) LOG.camera_open++;
  if (strstr(line, "encoder open failed")) LOG.encoder_open++;
  if (strstr(line, "fell back to software")) LOG.fallback++;
  if (strstr(line, "stream encode failed")) LOG.encode++;
  if (strstr(line, "stream send failed")) LOG.send++;
  if (strstr(line, "stream audio send failed")) LOG.send_pcm++;
  if (strstr(line, "loopback audio open failed")) LOG.loopback_open++;
  if (strstr(line, "loopback audio source died")) LOG.loopback_died++;
}

static void log_capture_begin(void)
{
  memset(&LOG, 0, sizeof LOG);
  ccord_log_set_sink(log_sink, nullptr);
  ccord_log_set_level(CCORD_LOG_WARN);
}

static void log_capture_end(void)
{
  ccord_log_set_sink(nullptr, nullptr);
  ccord_log_set_level(CCORD_LOG_INFO);
}

// ---- tests -----------------------------------------------------------------------------------

static void test_create_destroy(void)
{
  fake_reset();
  ccord_app_media *m = make();
  CT_CHECK(F.hotplug_regs == 1);
  CT_CHECK(F.hotplug_fn != nullptr);
  ccord_app_media_destroy(&m);
  CT_CHECK(m == nullptr);
  CT_CHECK(F.hotplug_regs == 2);
  CT_CHECK(F.hotplug_fn == nullptr);   // unregistered
  ccord_app_media_destroy(&m);         // idempotent on second call (no double-free / UAF)
  CT_CHECK(F.hotplug_regs == 2);
  ccord_app_media_destroy(nullptr);    // null-safe

  // Missing ops rejected.
  ccord_app_media_ops bad = k_ops;
  bad.push_pcm = nullptr;
  ccord_app_media_config cfg = {.ops = &bad, .ud = &F};
  ccord_error err;
  CT_CHECK(ccord_app_media_create(&cfg, &err) == nullptr);
  CT_CHECK(ccord_app_media_create(nullptr, &err) == nullptr);
}

static void test_config_sync(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.prefs.ptt = true;
  F.prefs.vad_sensitivity = 70;
  F.prefs.noise_suppression = true;
  F.prefs.echo_cancel = true;
  F.prefs.auto_gain = true;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.cfg_calls == 1);
  CT_CHECK(F.last_cfg.ptt == true);
  CT_CHECK(F.last_cfg.vad_sensitivity_pct == 70);
  CT_CHECK(F.last_cfg.ns && F.last_cfg.aec && F.last_cfg.agc);
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.cfg_calls == 1);   // unchanged -> not re-forwarded
  F.prefs.vad_sensitivity = 20;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.cfg_calls == 2);
  CT_CHECK(F.last_cfg.vad_sensitivity_pct == 20);
  // DSP absent masks the toggles.
  F.dsp = false;
  F.prefs.vad_sensitivity = 21;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.cfg_calls == 3);
  CT_CHECK(!F.last_cfg.ns && !F.last_cfg.aec && !F.last_cfg.agc);
  // Sensitivity clamped to 0..100.
  F.prefs.vad_sensitivity = 250;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.last_cfg.vad_sensitivity_pct == 100);
  ccord_app_media_destroy(&m);
}

static void test_ptt(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.prefs.ptt = true;
  F.prefs.ptt_scancode = 42;
  F.key_down = false;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.last_ptt == false);
  F.key_down = true;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.last_ptt == true);
  F.prefs.ptt = false;   // voice activity: gate always open
  F.key_down = false;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.last_ptt == true);
  F.prefs.ptt = true;
  F.prefs.ptt_scancode = 0;   // unbound -> never held
  F.key_down = true;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.last_ptt == false);
  CT_CHECK(F.ptt_calls >= 4);
  ccord_app_media_destroy(&m);
}

static void test_user_volumes(void)
{
  fake_reset();
  ccord_app_media *m = make();
  // Fill all 64 sync slots so departed-user eviction is exercised.
  for (size_t i = 0; i < 64u; i++) {
    ccord_id uid = (ccord_id)(1000u + i);
    F.users[i] = uid;
    set_vol(uid, (uint8_t)(80u + (i % 40u)));
  }
  F.nusers = 64u;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.vol_calls == 64);
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.vol_calls == 64);   // unchanged
  set_vol(1000u, 50);
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.vol_calls == 65);
  CT_CHECK(F.last_vol_user == 1000u && F.last_vol_pct == 50);

  // Member rotation: user 1000 leaves, user 2000 joins into a previously full 64-slot table.
  F.users[0] = 2000u;
  set_vol(2000u, 135);
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.vol_calls == 66);
  CT_CHECK(F.last_vol_user == 2000u && F.last_vol_pct == 135);
  ccord_app_media_destroy(&m);
}

static void test_device_selection_hotplug(void)
{
  fake_reset();
  ccord_app_media *m = make();
  add_dev(true, "5");
  add_dev(false, "7");
  snprintf(F.prefs.input_device_id, sizeof F.prefs.input_device_id, "5");
  snprintf(F.prefs.output_device_id, sizeof F.prefs.output_device_id, "7");
  ccord_app_media_pump(m, false, false);   // inline (mock) path
  CT_CHECK(F.audio_opens == 2);
  CT_CHECK(F.audio_reads >= 1);
  // Change the selected input device via settings -> reopen.
  add_dev(true, "9");
  snprintf(F.prefs.input_device_id, sizeof F.prefs.input_device_id, "9");
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_opens == 3);
  CT_CHECK(F.last_open_capture == true);
  CT_CHECK(strcmp(F.last_open_id, "9") == 0);
  // Hot-plug removes device "9": fall back to the default ("").
  F.n_in = 1;   // only "5" remains
  snprintf(F.in_devs[0].id, sizeof F.in_devs[0].id, "5");
  CT_CHECK(F.hotplug_fn != nullptr);
  F.hotplug_fn(F.hotplug_ud, true, false);
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_opens == 4);
  CT_CHECK(F.last_open_capture == true);
  CT_CHECK(strcmp(F.last_open_id, "") == 0);   // default fallback
  // Device "9" returns -> reselect it.
  add_dev(true, "9");
  F.hotplug_fn(F.hotplug_ud, true, true);
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_opens == 5);
  CT_CHECK(strcmp(F.last_open_id, "9") == 0);
  ccord_app_media_destroy(&m);
  CT_CHECK(F.audio_closes >= 2);
}

static void test_live_thread(void)
{
  fake_reset();
  ccord_app_media *m = make();
  add_dev(true, "5");
  add_dev(false, "7");
  snprintf(F.prefs.input_device_id, sizeof F.prefs.input_device_id, "5");
  snprintf(F.prefs.output_device_id, sizeof F.prefs.output_device_id, "7");
  F.voice.muted = true;
  F.voice.deafened = true;
  F.prefs.input_volume = 150;
  F.prefs.output_volume = 60;
  ccord_app_media_pump(m, true, false);
  CT_CHECK(F.thread_starts == 1);
  CT_CHECK(strcmp(F.last_thread_io.input_device_id, "5") == 0);
  CT_CHECK(strcmp(F.last_thread_io.output_device_id, "7") == 0);
  CT_CHECK(F.thread_controls >= 1);
  CT_CHECK(F.thread_muted == true && F.thread_deaf == true);
  CT_CHECK(F.thread_in == 150 && F.thread_out == 60);
  CT_CHECK(F.audio_opens == 0);   // live uses the pthread, not inline handles
  ccord_app_media_pump(m, true, false);
  CT_CHECK(F.thread_starts == 1);   // unchanged -> not restarted

  // Settings change on live path restarts the thread with the new device ID.
  add_dev(true, "9");
  snprintf(F.prefs.input_device_id, sizeof F.prefs.input_device_id, "9");
  ccord_app_media_pump(m, true, false);
  CT_CHECK(F.thread_stops == 1);
  CT_CHECK(F.thread_starts == 2);
  CT_CHECK(strcmp(F.last_thread_io.input_device_id, "9") == 0);

  // Hot-plug removes device "9": live thread restarts on default "".
  F.n_in = 1;
  snprintf(F.in_devs[0].id, sizeof F.in_devs[0].id, "5");
  F.hotplug_fn(F.hotplug_ud, true, false);
  ccord_app_media_pump(m, true, false);
  CT_CHECK(F.thread_stops == 2);
  CT_CHECK(F.thread_starts == 3);
  CT_CHECK(strcmp(F.last_thread_io.input_device_id, "") == 0);

  F.voice.active = false;
  ccord_app_media_pump(m, true, false);
  CT_CHECK(F.thread_stops == 3);
  ccord_app_media_destroy(&m);
}

static void test_open_retry_backoff(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.fail_audio_open = true;
  F.fail_video_open = true;
  F.fail_decoder_open = true;
  F.stream_active = true;
  F.watch_active = true;

  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_opens == 2);     // capture + playback attempted once
  CT_CHECK(F.video_opens == 2);     // requested encoder + software H.264 fallback both failed
  CT_CHECK(F.decoder_opens == 1);

  // Within MEDIA_OPEN_RETRY_MS (2000 ms): no retry churn.
  F.now += 500;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_opens == 2);
  CT_CHECK(F.video_opens == 2);
  CT_CHECK(F.decoder_opens == 1);

  // Past backoff window: retried and succeeds once failure clears.
  F.fail_audio_open = false;
  F.fail_video_open = false;
  F.fail_decoder_open = false;
  F.now += 1600;   // total +2100 ms >= 2000 ms
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_opens == 4);
  CT_CHECK(F.video_opens == 3);   // first open of the retry succeeds (no fallback needed)
  CT_CHECK(F.decoder_opens == 2);

  ccord_app_media_destroy(&m);
}

static void test_inline_audio_gates(void)
{
  fake_reset();
  ccord_app_media *m = make();
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_reads >= 1 && F.push_calls >= 1);
  CT_CHECK(F.audio_writes >= 1);
  int pushes = F.push_calls;
  F.voice.muted = true;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.push_calls == pushes);   // muted -> no mic push
  F.voice.muted = false;
  F.voice.deafened = true;
  int writes = F.audio_writes;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_writes == writes);   // deafened -> no playback
  // Input gain scales the captured tone (1000 * 200% = 2000).
  F.voice.deafened = false;
  F.prefs.input_volume = 200;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.push_first[0] == 2000);
  // Output gain scales the pulled playback (500 * 200% = 1000).
  F.prefs.output_volume = 200;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.write_first[0] == 1000);
  ccord_app_media_destroy(&m);
}

static void test_virtual_clock_skips_audio(void)
{
  fake_reset();
  ccord_app_media *m = make();
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.audio_opens == 0);
  CT_CHECK(F.thread_starts == 0);
  CT_CHECK(F.cfg_calls == 1);   // settings still sync
  ccord_app_media_destroy(&m);
}

static void test_golive_send_screen(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.stream_active = true;
  F.have_capture_source = true;
  F.capture_has_frame = true;
  F.stream_cfg.resolution = CCORD_STREAM_RES_720P;
  F.stream_cfg.fps = CCORD_STREAM_FPS_30;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.video_opens == 1);
  CT_CHECK(F.last_vcfg.width == 640 && F.last_vcfg.height == 360);   // virtual clock caps 720p preview
  CT_CHECK(F.capture_opens == 1);
  CT_CHECK(F.encode_calls == 1);
  CT_CHECK(F.stream_send_calls == 1);
  CT_CHECK(F.last_keyframe == true);
  CT_CHECK(F.last_send_len == 4);
  CT_CHECK(F.upload_calls >= 1);
  CT_CHECK(strcmp(F.last_upload_key, "__video_stream__") == 0);
  // Within the frame interval: no new encode.
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.encode_calls == 1);
  // Advance past the interval (30 fps -> 33 ms): encode + send again, ts90k advances.
  F.now += 100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.encode_calls == 2);
  CT_CHECK(F.stream_send_calls == 2);
  CT_CHECK(F.last_ts90k == 9000);   // 100 ms * 90
  // Stream stops -> encoder + capture torn down.
  F.stream_active = false;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.video_closes == 1);
  CT_CHECK(F.capture_closes == 1);
  ccord_app_media_destroy(&m);
}

static void test_golive_send_camera(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.stream_active = true;
  F.voice.camera = true;
  F.camera_has_frame = true;
  snprintf(F.prefs.camera_device_id, sizeof F.prefs.camera_device_id, "cam0");
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.camera_opens == 1);
  CT_CHECK(F.capture_opens == 0);   // camera replaces screen capture
  CT_CHECK(F.encode_calls == 1);
  CT_CHECK(F.stream_send_calls == 1);
  F.stream_active = false;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.camera_closes == 1);
  ccord_app_media_destroy(&m);
}

static void test_golive_receive(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.watch_active = true;
  F.poll_have = true;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.decoder_opens == 1);
  CT_CHECK(F.decoder_feeds == 1);
  CT_CHECK(F.upload_calls == 1);
  CT_CHECK(strcmp(F.last_upload_key, "__video_stream__") == 0);
  // Idle pump with no newly decoded packet must not re-upload stale RGBA.
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.decoder_feeds == 1);
  CT_CHECK(F.upload_calls == 1);
  // Next arrived packet triggers a fresh upload.
  F.poll_have = true;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.decoder_feeds == 2);
  CT_CHECK(F.upload_calls == 2);
  F.watch_active = false;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.decoder_closes == 1);
  ccord_app_media_destroy(&m);
}

static void test_golive_stream_audio_send(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.stream_active = true;
  // Inline mic capture (1000 stereo) is downmixed to mono (1000) and pushed to the broadcast.
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.stream_send_pcm_calls == 1);
  CT_CHECK(F.stream_send_pcm_samples == T_FRAME);
  CT_CHECK(F.stream_send_pcm_first[0] == 1000 && F.stream_send_pcm_first[1] == 1000);
  // Input gain scales the captured frame before the downmix (1000 * 200% = 2000).
  F.prefs.input_volume = 200;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.stream_send_pcm_calls == 2);
  CT_CHECK(F.stream_send_pcm_first[0] == 2000);
  // Mic muted -> no capture, so no stream audio either.
  F.voice.muted = true;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.stream_send_pcm_calls == 2);
  ccord_app_media_destroy(&m);

  // No active outbound stream -> the mic frame is not pushed as stream audio.
  fake_reset();
  m = make();
  F.stream_active = false;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.push_calls >= 1);   // voice mic still pushed
  CT_CHECK(F.stream_send_pcm_calls == 0);
  ccord_app_media_destroy(&m);
}

static void test_golive_stream_audio_receive(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.watch_active = true;
  F.pull_silent = true;   // no remote voice this pump
  F.stream_pcm_poll_have = true;
  F.stream_pcm_poll_val = 700;
  ccord_app_media_pump(m, false, false);
  // Stream audio plays on its own, duplicated to both stereo channels.
  CT_CHECK(F.stream_poll_pcm_calls >= 1);
  CT_CHECK(F.audio_writes >= 1);
  CT_CHECK(F.write_first[0] == 700 && F.write_first[1] == 700);

  // Additive mix with remote voice (500 + 700 = 1200) on both channels.
  F.pull_silent = false;
  F.stream_pcm_poll_have = true;
  F.stream_pcm_poll_val = 700;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.write_first[0] == 1200 && F.write_first[1] == 1200);

  // Additive sum clamps to int16 max (500 + 32700 -> 32767).
  F.stream_pcm_poll_have = true;
  F.stream_pcm_poll_val = 32700;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.write_first[0] == 32767 && F.write_first[1] == 32767);

  // 10 ms off-beat between 20 ms voice frames (pull_silent = true at now + 10) must NOT double-pull stream audio.
  F.pull_silent = true;
  F.now += 10;
  F.stream_pcm_poll_have = true;
  int writes_before = F.audio_writes;
  int polls_before = F.stream_poll_pcm_calls;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.stream_poll_pcm_calls == polls_before);
  CT_CHECK(F.audio_writes == writes_before);
  // At now + 20 ms, standalone stream audio resumes on schedule.
  F.now += 10;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.stream_poll_pcm_calls == polls_before + 1);
  CT_CHECK(F.audio_writes == writes_before + 1);

  // Not watching -> the stream poll seam is not called and nothing is mixed.
  F.watch_active = false;
  F.stream_pcm_poll_have = true;
  int polls = F.stream_poll_pcm_calls;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.stream_poll_pcm_calls == polls);
  ccord_app_media_destroy(&m);
}

static void test_golive_stream_audio_live_thread(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.stream_active = true;
  ccord_app_media_pump(m, true, false);   // live pthread owns the devices
  CT_CHECK(F.thread_starts == 1);
  // The pthread's push seam downmixes the mic frame to mono stream audio (L=1000, R=3000 -> 2000).
  int16_t stereo[T_FRAME * 2];
  for (size_t i = 0; i < T_FRAME; i++) {
    stereo[i * 2] = 1000;
    stereo[i * 2 + 1] = 3000;
  }
  ccord_error err;
  CT_CHECK(F.last_thread_io.push(F.last_thread_io.ud, stereo, T_FRAME, &err) == CCORD_OK);
  CT_CHECK(F.stream_send_pcm_calls == 1);
  CT_CHECK(F.stream_send_pcm_samples == T_FRAME);
  CT_CHECK(F.stream_send_pcm_first[0] == 2000);
  // No active stream -> the push seam forwards voice only, no stream audio.
  F.stream_active = false;
  CT_CHECK(F.last_thread_io.push(F.last_thread_io.ud, stereo, T_FRAME, &err) == CCORD_OK);
  CT_CHECK(F.stream_send_pcm_calls == 1);
  ccord_app_media_destroy(&m);
}

static void test_stream_ops_optional(void)
{
  fake_reset();
  ccord_app_media_ops ops = k_ops;
  ops.stream_send_pcm = nullptr;
  ops.stream_poll_pcm = nullptr;
  ccord_app_media_config cfg = {.ops = &ops, .ud = &F};
  ccord_error err;
  ccord_app_media *m = ccord_app_media_create(&cfg, &err);
  CT_CHECK(m != nullptr);   // optional seams may be absent
  F.stream_active = true;
  F.watch_active = true;
  F.stream_pcm_poll_have = true;
  ccord_app_media_pump(m, false, false);   // must not dereference the null stream seams
  ccord_app_media_pump(m, true, false);
  ccord_app_media_destroy(&m);
}

// ---- Go Live default capture + encoder fallback + once-latched warnings -------------------------

static void test_golive_default_capture(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.stream_active = true;
  F.have_capture_source = false;   // Go Live started without picking a source card
  F.capture_has_frame = true;
  ccord_app_media_pump(m, false, true);
  // Default source: the portal system picker, opened through the host capture seam.
  CT_CHECK(F.capture_opens == 1);
  CT_CHECK(F.portal_opens == 1 && F.x11_opens == 0);
  CT_CHECK(F.last_capture_backend == CCORD_CAPBE_PORTAL);
  CT_CHECK(strcmp(F.last_capture_id, "portal:pick") == 0);
  // Frames flow even though no explicit source was selected.
  CT_CHECK(F.encode_calls == 1 && F.stream_send_calls == 1);
  CT_CHECK(F.upload_calls >= 1);
  // An explicit selection still wins over the default.
  F.stream_active = false;
  ccord_app_media_pump(m, false, true);
  F.have_capture_source = true;
  F.stream_active = true;
  F.now += 100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.capture_opens == 2);
  CT_CHECK(F.last_capture_backend == CCORD_CAPBE_FAKE);
  CT_CHECK(F.portal_opens == 1);   // no second portal attempt
  CT_CHECK(F.stream_send_calls == 2);
  ccord_app_media_destroy(&m);
}

static void test_golive_default_capture_x11(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.stream_active = true;
  F.have_capture_source = false;
  F.capture_has_frame = true;
  F.fail_portal_open = true;   // no portal on this host
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.capture_opens == 2);   // portal attempt, then the primary X11 screen
  CT_CHECK(F.portal_opens == 1 && F.x11_opens == 1);
  CT_CHECK(F.last_capture_backend == CCORD_CAPBE_X11);
  CT_CHECK(strcmp(F.last_capture_id, "x11:mon:root") == 0);
  CT_CHECK(F.encode_calls == 1 && F.stream_send_calls == 1);
  ccord_app_media_destroy(&m);
}

static void test_golive_default_capture_cancel(void)
{
  fake_reset();
  log_capture_begin();
  ccord_app_media *m = make();
  F.stream_active = true;
  F.have_capture_source = false;
  F.fail_portal_open = true;
  F.portal_open_cancel = true;   // user cancels the picker
  F.fail_x11_open = true;        // no X11 fallback either (Wayland)
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.portal_opens == 1 && F.x11_opens == 1);
  CT_CHECK(F.encode_calls == 0);
  CT_CHECK(LOG.capture_open == 1);
  // Past the retry backoff: X11 is retried, but the cancelled picker is not re-prompted.
  F.now += 2100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.portal_opens == 1);
  CT_CHECK(F.x11_opens == 2);
  CT_CHECK(LOG.capture_open == 1);   // once-latched per stream
  // Restarting the stream offers the picker again and re-arms the warning.
  F.stream_active = false;
  ccord_app_media_pump(m, false, true);
  F.stream_active = true;
  F.now += 100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.portal_opens == 2);
  CT_CHECK(LOG.capture_open == 2);
  ccord_app_media_destroy(&m);
  log_capture_end();
}

static void test_golive_encoder_fallback(void)
{
  fake_reset();
  log_capture_begin();
  ccord_app_media *m = make();
  F.stream_active = true;
  F.have_capture_source = true;
  F.capture_has_frame = true;
  F.stream_cfg.encoder = CCORD_STREAM_ENCODER_NVENC;
  F.fail_hw_video_open = true;   // hardware encoder init fails
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.video_opens == 2);   // nvenc attempt + software fallback
  CT_CHECK(strcmp(F.last_enc_pref, "libx264") == 0);
  CT_CHECK(F.encode_calls == 1 && F.stream_send_calls == 1);
  CT_CHECK(F.upload_calls >= 1);   // preview still updates
  CT_CHECK(LOG.fallback == 1 && LOG.encoder_open == 0);
  ccord_app_media_destroy(&m);

  // A software preference opens directly: no duplicate fallback attempt.
  fake_reset();
  m = make();
  F.stream_active = true;
  F.have_capture_source = true;
  F.capture_has_frame = true;
  F.stream_cfg.encoder = CCORD_STREAM_ENCODER_X264;
  F.fail_hw_video_open = true;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.video_opens == 1);
  CT_CHECK(strcmp(F.last_enc_pref, "libx264") == 0);
  CT_CHECK(F.stream_send_calls == 1);
  ccord_app_media_destroy(&m);
  log_capture_end();
}

static void test_golive_encoder_config(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.stream_active = true;
  F.have_capture_source = true;
  // Explicit X11 framework: no PipeWire probe; HEVC selection falls back to the H.264 wire codec.
  F.stream_cfg.framework = CCORD_STREAM_FRAMEWORK_X11;
  F.stream_cfg.codec = CCORD_STREAM_CODEC_HEVC;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.video_opens == 1);
  CT_CHECK(F.last_vcfg.use_pipewire == false);
  CT_CHECK(F.last_vcfg.codec == CCORD_VIDEO_CODEC_H264);
  // Portal and AUTO frameworks keep the PipeWire probe.
  F.stream_active = false;
  ccord_app_media_pump(m, false, true);
  F.stream_cfg.framework = CCORD_STREAM_FRAMEWORK_PIPEWIRE;
  F.stream_active = true;
  F.now += 100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.last_vcfg.use_pipewire == true);
  F.stream_active = false;
  ccord_app_media_pump(m, false, true);
  F.stream_cfg.framework = CCORD_STREAM_FRAMEWORK_AUTO;
  F.stream_active = true;
  F.now += 100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.last_vcfg.use_pipewire == true);
  ccord_app_media_destroy(&m);
}

static void test_golive_warn_latches(void)
{
  fake_reset();
  log_capture_begin();
  ccord_app_media *m = make();
  F.stream_active = true;
  F.have_capture_source = true;
  F.capture_has_frame = true;
  F.stream_send_fail = true;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.stream_send_calls == 1);
  CT_CHECK(LOG.send == 1);
  // Further failed sends stay silent for the rest of the stream (no per-frame spam).
  F.now += 100;
  ccord_app_media_pump(m, false, true);
  F.now += 100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.stream_send_calls == 3);
  CT_CHECK(LOG.send == 1);
  // Encode failures latch the same way.
  F.stream_send_fail = false;
  F.encode_fail = true;
  F.now += 100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(LOG.encode == 1);
  F.now += 100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(LOG.encode == 1);
  // A new stream re-arms both latches.
  F.encode_fail = false;
  F.stream_active = false;
  ccord_app_media_pump(m, false, true);
  F.stream_send_fail = true;
  F.stream_active = true;
  F.now += 100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(LOG.send == 2);
  ccord_app_media_destroy(&m);
  log_capture_end();
}

static void test_golive_open_warn_latch(void)
{
  fake_reset();
  log_capture_begin();
  ccord_app_media *m = make();
  F.stream_active = true;
  F.have_capture_source = true;
  F.fail_capture_open = true;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(LOG.capture_open == 1);
  CT_CHECK(F.capture_opens == 1);
  // Retries after the backoff stay silent: one warning per class per stream.
  F.now += 2100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.capture_opens == 2);
  CT_CHECK(LOG.capture_open == 1);
  ccord_app_media_destroy(&m);

  // Encoder open failures latch the same way (requested encoder + software fallback both fail;
  // capture is only attempted once the encoder is open).
  fake_reset();
  log_capture_begin();
  m = make();
  F.stream_active = true;
  F.have_capture_source = true;
  F.fail_video_open = true;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(LOG.encoder_open == 1);
  CT_CHECK(F.video_opens == 2);
  F.now += 2100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.video_opens == 4);
  CT_CHECK(LOG.encoder_open == 1);
  ccord_app_media_destroy(&m);
  log_capture_end();
}

static void test_golive_camera_warn_latch(void)
{
  fake_reset();
  log_capture_begin();
  ccord_app_media *m = make();
  F.stream_active = true;
  F.voice.camera = true;
  F.fail_camera_open = true;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.camera_opens == 1);
  CT_CHECK(LOG.camera_open == 1);
  F.now += 2100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.camera_opens == 2);
  CT_CHECK(LOG.camera_open == 1);   // retried, but not re-logged
  ccord_app_media_destroy(&m);
  log_capture_end();
}

static void test_golive_pcm_warn_latch(void)
{
  fake_reset();
  log_capture_begin();
  ccord_app_media *m = make();
  F.stream_active = true;
  F.stream_send_pcm_fail = true;
  ccord_app_media_pump(m, false, false);   // inline mic path pushes stream audio
  CT_CHECK(F.stream_send_pcm_calls == 1);
  CT_CHECK(LOG.send_pcm == 1);
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.stream_send_pcm_calls == 2);
  CT_CHECK(LOG.send_pcm == 1);
  // Stream restart re-arms the latch.
  F.stream_active = false;
  ccord_app_media_pump(m, false, false);
  F.stream_active = true;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(LOG.send_pcm == 2);
  ccord_app_media_destroy(&m);
  log_capture_end();
}

static void test_soundboard(void)
{
  fake_reset();
  ccord_app_media *m = make();
  // Enable PTT with key NOT held and mic muted: soundboard must still play locally and never push mic TX.
  F.prefs.ptt = true;
  F.prefs.ptt_scancode = 42;
  F.key_down = false;
  F.voice.muted = true;
  F.pull_silent = true;

  ccord_error err;
  CT_CHECK(ccord_app_media_play_soundboard(m, 1, 777, &err) == CCORD_OK);
  CT_CHECK(F.play_sb_calls == 1);
  CT_CHECK(F.last_sb_guild == 1 && F.last_sb_sound == 777);

  // Clip not yet arrived from governed CDN fetch: no decode or playback.
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.sound_decode_calls == 0);
  CT_CHECK(F.audio_writes == 0);
  CT_CHECK(F.push_calls == 0);

  // Clip arrives at 50% soundboard volume: decoded (2000 * 50% = 1000) and written to playback.
  F.sb_ready = true;
  F.sb_vol_pct = 50;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.sound_decode_calls == 1);
  CT_CHECK(F.audio_writes == 1);
  CT_CHECK(F.write_first[0] == 1000);
  CT_CHECK(F.push_calls == 0);   // never routed through PTT-gated push_pcm

  // Drained on next frame.
  F.now += 20;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_writes == 1);

  // Mixes additively when remote RX audio (500) is active simultaneously (500 + 2000 = 2500).
  F.pull_silent = false;
  F.sb_ready = true;
  F.sb_vol_pct = 100;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.sound_decode_calls == 2);
  CT_CHECK(F.audio_writes == 2);
  CT_CHECK(F.write_first[0] == 2500);

  // Live pthread io.pull path also receives decoded soundboard PCM.
  F.pull_silent = true;
  F.sb_ready = true;
  F.sb_vol_pct = 100;
  ccord_app_media_pump(m, true, false);
  CT_CHECK(F.thread_starts == 1);
  CT_CHECK(F.sound_decode_calls == 3);
  int16_t live_pcm[T_FRAME * 2] = {0};
  size_t got = F.last_thread_io.pull(F.last_thread_io.ud, live_pcm, T_FRAME);
  CT_CHECK(got == T_FRAME);
  CT_CHECK(live_pcm[0] == 2000);

  ccord_app_media_destroy(&m);
}

static void test_soundboard_multiframe_pacing(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.voice.muted = true;
  F.pull_silent = true;
  F.sb_decode_samples = T_FRAME * 2 * 3;   // three 20 ms frames
  F.sb_ready = true;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_writes == 1);
  // Sub-frame pump does not emit early.
  F.now += 5;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_writes == 1);
  F.now += 15;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_writes == 2);
  F.now += 20;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_writes == 3);
  F.now += 20;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_writes == 3);   // drained
  ccord_app_media_destroy(&m);
}

static void test_soundboard_intent_fail(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.play_sb_fail = true;
  ccord_error err;
  CT_CHECK(ccord_app_media_play_soundboard(m, 1, 5, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_app_media_play_soundboard(nullptr, 1, 5, &err) == CCORD_ERR_INVALID);
  ccord_app_media_destroy(&m);
}

static void test_voice_exit_teardown(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.pull_silent = true;
  F.voice.muted = true;
  F.sb_decode_samples = T_FRAME * 2 * 3;   // three frames: two remain when voice exits
  F.sb_ready = true;
  ccord_app_media_pump(m, false, false);   // opens devices, decodes, writes only the first frame
  CT_CHECK(F.audio_opens == 2);
  CT_CHECK(F.audio_writes == 1);
  F.voice.active = false;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_closes == 2);   // devices closed on leave
  // Rejoin voice later: remaining soundboard frames were cleared on exit, so nothing replays.
  F.voice.active = true;
  F.now += 100;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.audio_writes == 1);
  ccord_app_media_destroy(&m);
}

static void test_golive_pause_resume_and_keyframe(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.stream_active = true;
  F.have_capture_source = true;
  F.capture_has_frame = true;
  F.stream_cfg.resolution = CCORD_STREAM_RES_720P;
  F.stream_cfg.fps = CCORD_STREAM_FPS_30;

  // Initial unpaused frame encodes + sends video + inline stream audio.
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.encode_calls == 1 && F.stream_send_calls == 1 && F.stream_send_pcm_calls == 1);
  CT_CHECK(F.force_keyframe_calls == 0);

  // Pause: skips video encode/send and skips stream audio TX even as time advances.
  F.stream_paused = true;
  F.now += 100;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.encode_calls == 1 && F.stream_send_calls == 1 && F.stream_send_pcm_calls == 1);

  // Also skips live pthread push->stream_send_pcm while paused.
  ccord_app_media_pump(m, true, false);
  int16_t stereo[T_FRAME * 2] = {0};
  ccord_error err;
  CT_CHECK(F.last_thread_io.push(F.last_thread_io.ud, stereo, T_FRAME, &err) == CCORD_OK);
  CT_CHECK(F.stream_send_pcm_calls == 1);

  // Resume: forces an IDR keyframe immediately and resumes TX.
  F.stream_paused = false;
  F.now += 100;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.force_keyframe_calls == 1);
  CT_CHECK(F.encode_calls == 2 && F.stream_send_calls == 2 && F.stream_send_pcm_calls == 2);

  // PLI/FIR keyframe request also forces IDR on the next encoded frame.
  F.want_keyframe = true;
  F.now += 100;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.force_keyframe_calls == 2);
  CT_CHECK(F.encode_calls == 3 && F.stream_send_calls == 3);

  ccord_app_media_destroy(&m);
}

static void test_standalone_voice_camera_tx(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.voice.active = true;
  F.voice.camera = true;
  F.camera_has_frame = true;
  snprintf(F.prefs.camera_device_id, sizeof F.prefs.camera_device_id, "cam0");

  // No Go Live stream active: standalone camera opens encoder + camera and sends via voice_video_send.
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.camera_opens == 1);
  CT_CHECK(F.video_opens == 1);
  CT_CHECK(F.encode_calls == 1);
  CT_CHECK(F.voice_video_send_calls == 1);
  CT_CHECK(F.stream_send_calls == 0);

  // Keyframe request propagates to standalone camera encoder.
  F.want_keyframe = true;
  F.now += 100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.force_keyframe_calls == 1);
  CT_CHECK(F.voice_video_send_calls == 2);

  // Turning on Go Live tears down standalone cam_video and routes through stream_send instead.
  F.stream_active = true;
  F.now += 100;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.video_closes == 1);
  CT_CHECK(F.video_opens == 2);
  CT_CHECK(F.stream_send_calls == 1);
  CT_CHECK(F.voice_video_send_calls == 2);

  // Stopping both closes encoder + camera.
  F.stream_active = false;
  F.voice.camera = false;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.video_closes == 2);
  CT_CHECK(F.camera_closes == 1);
  ccord_app_media_destroy(&m);
}

// ---- desktop / monitor loopback audio capture + Go Live TX ----------------------------------

static ccord_loopback_audio *real_lb_open(void *ud, ccord_error *err)
{
  (void)ud;
  return ccord_loopback_audio_open(err);
}

static size_t real_lb_read(void *ud, ccord_loopback_audio *l, int16_t *pcm, size_t max_samples)
{
  (void)ud;
  return ccord_loopback_audio_read(l, pcm, max_samples);
}

static bool real_lb_failed(void *ud, const ccord_loopback_audio *l)
{
  (void)ud;
  return ccord_loopback_audio_failed(l);
}

static void real_lb_close(void *ud, ccord_loopback_audio *l)
{
  (void)ud;
  ccord_loopback_audio_close(l);
}

static void test_platform_loopback_fake(void)
{
  CT_CHECK(CCORD_LOOPBACK_RATE == 48000);
  CT_CHECK(CCORD_LOOPBACK_FRAME_SAMPLES == 960u);

  const char *prev_env = getenv("CCORD_CAPTURE_BACKEND_FAKE");
  char saved_env[64] = "";
  bool had_env = prev_env != nullptr;
  if (had_env) snprintf(saved_env, sizeof saved_env, "%s", prev_env);

  // Fake enablement switches: explicit setter, CCORD_CAPTURE_BACKEND_FAKE env, and capture fake switch.
  ccord_capture_set_fake(false);
  ccord_loopback_audio_set_fake(false);
  (void)unsetenv("CCORD_CAPTURE_BACKEND_FAKE");
  CT_CHECK(!ccord_loopback_audio_fake_enabled());

  (void)setenv("CCORD_CAPTURE_BACKEND_FAKE", "0", 1);
  CT_CHECK(!ccord_loopback_audio_fake_enabled());

  (void)setenv("CCORD_CAPTURE_BACKEND_FAKE", "1", 1);
  CT_CHECK(ccord_loopback_audio_fake_enabled());
  (void)unsetenv("CCORD_CAPTURE_BACKEND_FAKE");

  ccord_capture_set_fake(true);
  CT_CHECK(ccord_loopback_audio_fake_enabled());
  ccord_capture_set_fake(false);

  ccord_loopback_audio_set_fake(true);
  CT_CHECK(ccord_loopback_audio_fake_enabled());

  // Deterministic 48 kHz mono 960-sample (20 ms) 440 Hz tone generator.
  ccord_error err;
  ccord_loopback_audio *a = ccord_loopback_audio_open(&err);
  ccord_loopback_audio *b = ccord_loopback_audio_open(&err);
  CT_CHECK(a != nullptr && b != nullptr);
  CT_CHECK(!ccord_loopback_audio_failed(a));

  int16_t fa0[2048] = {0}, fa1[960] = {0};
  int16_t fb0[960] = {0}, fb1[960] = {0};
  // Even when max_samples > 960, one fake read emits exactly one 20 ms 960-sample mono frame.
  CT_CHECK(ccord_loopback_audio_read(a, fa0, 2048) == CCORD_LOOPBACK_FRAME_SAMPLES);
  CT_CHECK(ccord_loopback_audio_read(a, fa1, 960) == CCORD_LOOPBACK_FRAME_SAMPLES);
  // Half-frame reads advance phase deterministically and match a full-frame read byte-for-byte.
  CT_CHECK(ccord_loopback_audio_read(b, fb0, 480) == 480u);
  CT_CHECK(ccord_loopback_audio_read(b, fb0 + 480, 480) == 480u);
  CT_CHECK(ccord_loopback_audio_read(b, fb1, 960) == CCORD_LOOPBACK_FRAME_SAMPLES);
  CT_CHECK(memcmp(fa0, fb0, 960u * sizeof(int16_t)) == 0);
  CT_CHECK(memcmp(fa1, fb1, 960u * sizeof(int16_t)) == 0);
  // Phase advances across frames (440 Hz over 960 samples = 8.8 cycles, not integer-periodic).
  CT_CHECK(memcmp(fa0, fa1, 960u * sizeof(int16_t)) != 0);

  int16_t min_s = 0, max_s = 0;
  for (size_t i = 0; i < CCORD_LOOPBACK_FRAME_SAMPLES; i++) {
    if (fa0[i] < min_s) min_s = fa0[i];
    if (fa0[i] > max_s) max_s = fa0[i];
  }
  CT_CHECK(max_s > 8000 && min_s < -8000);

  // Null / zero-length guard rails.
  CT_CHECK(ccord_loopback_audio_read(nullptr, fa0, 960) == 0);
  CT_CHECK(ccord_loopback_audio_read(a, nullptr, 960) == 0);
  CT_CHECK(ccord_loopback_audio_read(a, fa0, 0) == 0);
  CT_CHECK(ccord_loopback_audio_failed(nullptr));
  ccord_loopback_audio_close(nullptr);
  ccord_loopback_audio_close(a);
  ccord_loopback_audio_close(b);

  // Real-mode soft failure when audio servers are unreachable (must never crash).
  ccord_loopback_audio_set_fake(false);
  const char *p_ps = getenv("PULSE_SERVER");
  const char *p_pw = getenv("PIPEWIRE_REMOTE");
  const char *p_xdg = getenv("XDG_RUNTIME_DIR");
  char s_ps[128] = "", s_pw[128] = "", s_xdg[128] = "";
  bool h_ps = p_ps != nullptr, h_pw = p_pw != nullptr, h_xdg = p_xdg != nullptr;
  if (h_ps) snprintf(s_ps, sizeof s_ps, "%s", p_ps);
  if (h_pw) snprintf(s_pw, sizeof s_pw, "%s", p_pw);
  if (h_xdg) snprintf(s_xdg, sizeof s_xdg, "%s", p_xdg);
  (void)setenv("PULSE_SERVER", "/nonexistent/pulse", 1);
  (void)setenv("PIPEWIRE_REMOTE", "/nonexistent/pipewire-0", 1);
  (void)setenv("XDG_RUNTIME_DIR", "/nonexistent", 1);
  ccord_loopback_audio *real_try = ccord_loopback_audio_open(&err);
  if (real_try) {
    ccord_loopback_audio_close(real_try);
  } else {
    CT_CHECK(err.code != CCORD_OK);
  }
  if (h_ps) (void)setenv("PULSE_SERVER", s_ps, 1); else (void)unsetenv("PULSE_SERVER");
  if (h_pw) (void)setenv("PIPEWIRE_REMOTE", s_pw, 1); else (void)unsetenv("PIPEWIRE_REMOTE");
  if (h_xdg) (void)setenv("XDG_RUNTIME_DIR", s_xdg, 1); else (void)unsetenv("XDG_RUNTIME_DIR");

#ifdef __linux__
  // Exercise LB_KIND_PIPE (posix_spawnp PATH lookup, non-blocking pipe read, SIGTERM reap) via a
  // temporary parec shim in $TMPDIR.
  const char *tbase = getenv("TMPDIR");
  if (!tbase || tbase[0] == '\0') tbase = ".";
  char tdir[512];
  if (snprintf(tdir, sizeof tdir, "%s/ccord-lb.XXXXXX", tbase) > 0 &&
      (size_t)snprintf(tdir, sizeof tdir, "%s/ccord-lb.XXXXXX", tbase) < sizeof tdir &&
      mkdtemp(tdir) != nullptr) {
    char shim[560];
    snprintf(shim, sizeof shim, "%s/parec", tdir);
    FILE *fp = fopen(shim, "wb");
    CT_CHECK(fp != nullptr);
    if (fp) {
      fputs("#!/bin/sh\nprintf '\\001\\002\\003\\004\\005\\006\\007\\010'\nsleep 2\n", fp);
      fclose(fp);
      CT_CHECK(chmod(shim, 0700) == 0);
      const char *old_path_env = getenv("PATH");
      char old_path[2048] = "";
      bool had_path = old_path_env != nullptr;
      if (had_path) snprintf(old_path, sizeof old_path, "%s", old_path_env);
      char new_path[2600];
      snprintf(new_path, sizeof new_path, "%s:%s", tdir, old_path);
      (void)setenv("PATH", new_path, 1);
      // Pin the legacy backend: on a host with a live PipeWire daemon the self-excluding
      // PW capture would otherwise win over the shim (CCORD_LOOPBACK_BACKEND, audio_devices.c).
      const char *old_lb_env = getenv("CCORD_LOOPBACK_BACKEND");
      char old_lb[64] = "";
      bool had_lb = old_lb_env != nullptr;
      if (had_lb) snprintf(old_lb, sizeof old_lb, "%s", old_lb_env);
      (void)setenv("CCORD_LOOPBACK_BACKEND", "parec", 1);

      ccord_loopback_audio *pl = ccord_loopback_audio_open(&err);
      CT_CHECK(pl != nullptr);
      if (pl) {
        int16_t samples[8] = {0};
        size_t got = ccord_loopback_audio_read(pl, samples, 8);
        CT_CHECK(got == 4u);
        CT_CHECK(samples[0] == 0x0201 && samples[1] == 0x0403 &&
                 samples[2] == 0x0605 && samples[3] == 0x0807);
        CT_CHECK(!ccord_loopback_audio_failed(pl));
        ccord_loopback_audio_close(pl);
      }
      if (had_path) (void)setenv("PATH", old_path, 1);
      else (void)unsetenv("PATH");
      if (had_lb) (void)setenv("CCORD_LOOPBACK_BACKEND", old_lb, 1);
      else (void)unsetenv("CCORD_LOOPBACK_BACKEND");
      (void)unlink(shim);
    }
    (void)rmdir(tdir);
  }
#endif

  if (had_env) (void)setenv("CCORD_CAPTURE_BACKEND_FAKE", saved_env, 1);
  else (void)unsetenv("CCORD_CAPTURE_BACKEND_FAKE");
}

static void test_golive_loopback_only_tx_and_mic_coexistence(void)
{
  fake_reset();
  ccord_app_media *m = make();
  CT_CHECK(!ccord_app_media_loopback_audio(m));
  CT_CHECK(!ccord_app_media_loopback_audio(nullptr));
  ccord_app_media_set_loopback_audio(nullptr, true);   // null-safe

  F.stream_active = true;
  F.loopback_val = 4321;   // distinct from inline mic stereo 1000 and live mic downmix 2000
  ccord_app_media_set_loopback_audio(m, true);
  CT_CHECK(ccord_app_media_loopback_audio(m));

  // 1. Inline path: mic voice TX (1000 stereo -> push_pcm) coexists with loopback-only stream TX (4321 mono).
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.push_calls == 1 && F.push_frames_total == T_FRAME);
  CT_CHECK(F.push_first[0] == 1000 && F.push_first[1] == 1000);
  CT_CHECK(F.loopback_opens == 1 && F.loopback_reads == 1);
  CT_CHECK(F.stream_send_pcm_calls == 1);
  CT_CHECK(F.stream_send_pcm_samples == T_FRAME);
  CT_CHECK(F.stream_send_pcm_first[0] == 4321 && F.stream_send_pcm_first[1] == 4321);

  // Sub-20ms pump: mic voice TX runs again, while paced loopback waits for its 20 ms slot.
  F.now += 5;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.push_calls == 2);
  CT_CHECK(F.stream_send_pcm_calls == 1);

  // At +20 ms total: next 960-sample loopback frame transmits.
  F.now += 15;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.push_calls == 3);
  CT_CHECK(F.stream_send_pcm_calls == 2);
  CT_CHECK(F.stream_send_pcm_first[0] == 4321);

  // 60 Hz UI pump cadence (16-17 ms steps over 100 ms = 6 ticks) sustains 5 frames/100 ms (50 fps = 48 kHz).
  static const uint64_t dt60[6] = {17, 16, 17, 17, 16, 17};
  int before_60 = F.stream_send_pcm_calls;
  for (int i = 0; i < 6; i++) {
    F.now += dt60[i];
    ccord_app_media_pump(m, false, false);
  }
  CT_CHECK(F.stream_send_pcm_calls - before_60 == 5);

  // Muting the mic stops push_pcm while desktop loopback stream audio keeps transmitting.
  F.voice.muted = true;
  int pushes_before_mute = F.push_calls;
  int stream_before_mute = F.stream_send_pcm_calls;
  F.now += 20;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.push_calls == pushes_before_mute);
  CT_CHECK(F.stream_send_pcm_calls == stream_before_mute + 1);
  CT_CHECK(F.stream_send_pcm_first[0] == 4321);

  // 2. Live pthread path: pthread mic push forwards voice TX (1000/3000) without overriding loopback stream TX.
  F.voice.muted = false;
  F.loopback_val = 5678;
  F.now += 20;
  ccord_app_media_pump(m, true, false);
  CT_CHECK(F.thread_starts == 1);
  int stream_after_pump = F.stream_send_pcm_calls;
  CT_CHECK(F.stream_send_pcm_first[0] == 5678);

  int16_t stereo[T_FRAME * 2];
  for (size_t i = 0; i < T_FRAME; i++) {
    stereo[i * 2] = 1000;
    stereo[i * 2 + 1] = 3000;
  }
  ccord_error err;
  int pushes_before_live = F.push_calls;
  CT_CHECK(F.last_thread_io.push(F.last_thread_io.ud, stereo, T_FRAME, &err) == CCORD_OK);
  CT_CHECK(F.push_calls == pushes_before_live + 1);
  CT_CHECK(F.push_first[0] == 1000 && F.push_first[1] == 3000);
  CT_CHECK(F.stream_send_pcm_calls == stream_after_pump);   // mic->stream stood down

  // 3. Host read seam (ops->stream_loopback_audio) also keeps loopback active when setter is false.
  ccord_app_media_set_loopback_audio(m, false);
  F.stream_loopback_audio = true;
  F.now += 20;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.loopback_closes == 0);
  CT_CHECK(F.stream_send_pcm_first[0] == 5678);

  // Turning both switches off closes loopback and restores mic->stream downmix (1000).
  F.stream_loopback_audio = false;
  F.now += 20;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.loopback_closes == 1);
  CT_CHECK(F.stream_send_pcm_first[0] == 1000);

  ccord_app_media_destroy(&m);
}

static void test_golive_loopback_pause_gating(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.stream_active = true;
  F.have_capture_source = true;
  F.capture_has_frame = true;
  F.loopback_val = 6789;
  ccord_app_media_set_loopback_audio(m, true);

  // Unpaused: transmits loopback audio + video + mic voice TX.
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.stream_send_pcm_calls == 1 && F.stream_send_pcm_first[0] == 6789);
  CT_CHECK(F.push_calls == 1 && F.push_first[0] == 1000);
  CT_CHECK(F.loopback_opens == 1 && F.loopback_closes == 0);

  // Pause Go Live: loopback TX is gated (no reads or stream_send_pcm), handle stays open, mic voice TX continues!
  F.stream_paused = true;
  F.now += 40;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.stream_send_pcm_calls == 1);
  CT_CHECK(F.loopback_reads == 1);
  CT_CHECK(F.loopback_closes == 0);
  CT_CHECK(F.push_calls == 2 && F.push_first[0] == 1000);

  // Live pthread push while paused also keeps mic voice TX alive while stream_send_pcm stays gated.
  F.now += 40;
  ccord_app_media_pump(m, true, false);
  int16_t stereo[T_FRAME * 2];
  for (size_t i = 0; i < T_FRAME * 2; i++) stereo[i] = 1200;
  ccord_error err;
  CT_CHECK(F.last_thread_io.push(F.last_thread_io.ud, stereo, T_FRAME, &err) == CCORD_OK);
  CT_CHECK(F.push_calls == 3 && F.push_first[0] == 1200);
  CT_CHECK(F.stream_send_pcm_calls == 1);

  // Unpause: resumes loopback TX immediately on the next pump without reopening the handle.
  F.stream_paused = false;
  F.now += 5;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.loopback_opens == 1);
  CT_CHECK(F.stream_send_pcm_calls == 2 && F.stream_send_pcm_first[0] == 6789);
  CT_CHECK(F.push_calls == 4);

  // Stopping Go Live closes the loopback handle.
  F.stream_active = false;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.loopback_closes == 1);
  ccord_app_media_destroy(&m);
}

static void test_golive_loopback_platform_bridge_and_failures(void)
{
  // 1. End-to-end bridge through the real platform fake loopback tone generator.
  fake_reset();
  ccord_loopback_audio_set_fake(true);
  ccord_app_media_ops real_ops = k_ops;
  real_ops.loopback_open = real_lb_open;
  real_ops.loopback_read = real_lb_read;
  real_ops.loopback_failed = real_lb_failed;
  real_ops.loopback_close = real_lb_close;
  ccord_app_media_config cfg = {.ops = &real_ops, .ud = &F};
  ccord_error err;
  ccord_app_media *m = ccord_app_media_create(&cfg, &err);
  CT_CHECK(m != nullptr);
  F.stream_active = true;
  ccord_app_media_set_loopback_audio(m, true);
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.stream_send_pcm_calls == 1);
  CT_CHECK(F.stream_send_pcm_samples == CCORD_LOOPBACK_FRAME_SAMPLES);
  // Frame 0 of 440 Hz sin starts at 0, then ~471, ~940, ~1406 (distinct from mic 1000).
  CT_CHECK(F.stream_send_pcm_first[0] == 0 && F.stream_send_pcm_first[1] > 400);
  CT_CHECK(F.push_calls == 1 && F.push_first[0] == 1000);
  ccord_app_media_destroy(&m);
  ccord_loopback_audio_set_fake(false);

  // 2. Soft open failure + 2000 ms retry backoff + once-per-stream warning + dead-source recovery.
  fake_reset();
  log_capture_begin();
  m = make();
  F.stream_active = true;
  F.fail_loopback_open = true;
  ccord_app_media_set_loopback_audio(m, true);
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.loopback_opens == 1);
  CT_CHECK(LOG.loopback_open == 1);
  CT_CHECK(F.stream_send_pcm_calls == 0);   // never leaks mic to stream when loopback is requested
  CT_CHECK(F.push_calls == 1);              // mic voice TX still works

  // Within backoff: not retried, not re-logged.
  F.now += 500;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.loopback_opens == 1 && LOG.loopback_open == 1);

  // Past backoff: opens and streams.
  F.fail_loopback_open = false;
  F.now += 1600;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.loopback_opens == 2);
  CT_CHECK(F.stream_send_pcm_calls == 1 && F.stream_send_pcm_first[0] == 4321);

  // Source dies mid-stream: closed, backed off 2000 ms, then reopened.
  F.loopback_dead = true;
  F.now += 20;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.loopback_closes == 1);
  CT_CHECK(F.stream_send_pcm_calls == 1);
  F.loopback_dead = false;
  F.now += 2100;
  ccord_app_media_pump(m, false, false);
  CT_CHECK(F.loopback_opens == 3 && F.stream_send_pcm_calls == 2);

  ccord_app_media_destroy(&m);
  log_capture_end();
}

// ---- remote voice cameras --------------------------------------------------------------------

static void cam_set(size_t idx, ccord_id user, uint32_t gen, bool decoding, int pending)
{
  F.cam_list[idx] = (ccord_camera_rx){.user_id = user, .generation = gen, .decoding = decoding};
  F.cam_pending[idx] = pending;
  if (idx >= F.cam_n) F.cam_n = idx + 1;
}

static int cam_uploaded(const char *key)
{
  int n = 0;
  for (int i = 0; i < F.cam_uploads; i++) n += strcmp(F.cam_upload_keys[i], key) == 0 ? 1 : 0;
  return n;
}

static void test_app_media_cameras(void)
{
  fake_reset();
  ccord_app_media *m = make();
  CT_CHECK(ccord_app_media_cam_count(m) == 0);

  // Three admitted users, one AU each: three decoders (clamped), three '__cam__:<uid>' textures.
  cam_set(0, 1005, 1, true, 1);
  cam_set(1, 1006, 1, true, 1);
  cam_set(2, 1007, 1, true, 1);
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.decoder_opens == 3 && F.open_decoders == 3);
  CT_CHECK(F.max_dim_calls == 3 && F.max_dim_w == 960 && F.max_dim_h == 540);
  CT_CHECK(F.decoder_feeds == 3 && F.cam_uploads == 3);
  CT_CHECK(cam_uploaded("__cam__:1005") == 1 && cam_uploaded("__cam__:1006") == 1 && cam_uploaded("__cam__:1007") == 1);
  CT_CHECK(ccord_app_media_cam_count(m) == 3);
  ccord_app_media_cam_debug dbg;
  CT_CHECK(ccord_app_media_cam_debug_of(m, 1006, &dbg));
  CT_CHECK(dbg.open && dbg.generation == 1 && dbg.w == 2 && dbg.h == 2 && dbg.frames == 1);
  CT_CHECK(strcmp(dbg.key, "__cam__:1006") == 0 && dbg.frame_hash != 0);
  CT_CHECK(!ccord_app_media_cam_debug_of(m, 4242, &dbg));

  // Nothing new decoded: no re-upload.
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.cam_uploads == 3);

  // User 1006 stops: its decoder and texture are released, the others stay.
  F.cam_list[1] = F.cam_list[2];
  F.cam_n = 2;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.decoder_closes == 1 && F.open_decoders == 2);
  CT_CHECK(F.evicts == 1 && strcmp(F.evict_keys[0], "__cam__:1006") == 0);
  CT_CHECK(ccord_app_media_cam_count(m) == 2);
  CT_CHECK(!ccord_app_media_cam_debug_of(m, 1006, &dbg));

  // A user who is waiting (decoding == false) gets no decoder.
  cam_set(2, 1008, 1, false, 3);
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.open_decoders == 2 && ccord_app_media_cam_count(m) == 2);

  // Promotion: decoding with a new generation opens one; a generation change restarts the decoder.
  F.cam_list[2].decoding = true;
  F.cam_list[2].generation = 2;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.open_decoders == 3);
  int closes = F.decoder_closes, evicts = F.evicts;
  F.cam_list[0].generation = 7;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.decoder_closes == closes + 1 && F.evicts == evicts + 1 && F.open_decoders == 3);
  CT_CHECK(ccord_app_media_cam_debug_of(m, 1005, &dbg) && dbg.generation == 7);

  // Leaving the call releases everything.
  F.voice.active = false;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.open_decoders == 0 && ccord_app_media_cam_count(m) == 0);
  ccord_app_media_destroy(&m);
  CT_CHECK(F.open_decoders == 0);
}

static void test_app_media_cameras_idr_gate(void)
{
  fake_reset();
  ccord_app_media *m = make();
  cam_set(0, 1005, 1, true, 2);
  F.cam_first_delta[0] = true;   // first AU is a P-frame, the second an IDR
  ccord_app_media_pump(m, false, true);
  ccord_app_media_cam_debug dbg;
  CT_CHECK(ccord_app_media_cam_debug_of(m, 1005, &dbg));
  CT_CHECK(dbg.dropped_until_idr == 1 && dbg.frames == 1 && F.decoder_feeds == 1 && F.cam_uploads == 1);
  ccord_app_media_destroy(&m);
}

static void test_app_media_cameras_cap_and_budget(void)
{
  fake_reset();
  ccord_app_media *m = make();
  // Five senders all claim to be decoding: never more than 4 decoders.
  for (size_t i = 0; i < 5; i++) cam_set(i, 1005 + i, 1, true, 3);
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.max_open_decoders == 4 && F.open_decoders == 4 && ccord_app_media_cam_count(m) == 4);
  // At most 4 AUs decoded per pump across all users.
  CT_CHECK(F.decoder_feeds == 4);
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.decoder_feeds == 8 && F.max_open_decoders == 4);
  // Round robin: a busy sender cannot starve the rest.
  ccord_app_media_destroy(&m);
  fake_reset();
  m = make();
  cam_set(0, 1005, 1, true, 100);
  cam_set(1, 1006, 1, true, 100);
  cam_set(2, 1007, 1, true, 100);
  ccord_app_media_pump(m, false, true);
  ccord_app_media_pump(m, false, true);
  ccord_app_media_pump(m, false, true);
  for (size_t i = 0; i < 3; i++) {
    ccord_app_media_cam_debug d;
    CT_CHECK(ccord_app_media_cam_debug_of(m, 1005 + i, &d) && d.frames >= 3);
  }
  ccord_app_media_destroy(&m);
}

static void test_app_media_cameras_optional(void)
{
  fake_reset();
  ccord_app_media_ops ops = k_ops;
  ops.camera_senders = nullptr;
  ops.camera_poll = nullptr;
  ops.texture_evict = nullptr;
  ops.decoder_set_max_dim = nullptr;
  ccord_app_media_config cfg = {.ops = &ops, .ud = &F};
  ccord_error err;
  ccord_app_media *m = ccord_app_media_create(&cfg, &err);
  CT_CHECK(m != nullptr);
  cam_set(0, 1005, 1, true, 1);
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.decoder_opens == 0 && F.cam_polls == 0 && ccord_app_media_cam_count(m) == 0);
  ccord_app_media_destroy(&m);
  // Without evict / max-dim ops cameras still decode.
  fake_reset();
  ops = k_ops;
  ops.texture_evict = nullptr;
  ops.decoder_set_max_dim = nullptr;
  m = ccord_app_media_create(&cfg, &err);
  cam_set(0, 1005, 1, true, 1);
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.decoder_opens == 1 && F.cam_uploads == 1);
  ccord_app_media_destroy(&m);
}

static void test_app_media_cameras_open_retry(void)
{
  fake_reset();
  ccord_app_media *m = make();
  F.fail_decoder_open = true;
  cam_set(0, 1005, 1, true, 1);
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.decoder_opens == 1 && F.decoder_feeds == 0);
  F.now += 500;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.decoder_opens == 1);
  F.fail_decoder_open = false;
  F.now += 2000;
  ccord_app_media_pump(m, false, true);
  CT_CHECK(F.decoder_opens == 2 && F.decoder_feeds == 1);
  ccord_app_media_destroy(&m);
}

int main(void)
{
  test_create_destroy();
  test_config_sync();
  test_ptt();
  test_user_volumes();
  test_device_selection_hotplug();
  test_live_thread();
  test_open_retry_backoff();
  test_inline_audio_gates();
  test_virtual_clock_skips_audio();
  test_golive_send_screen();
  test_golive_send_camera();
  test_golive_receive();
  test_golive_stream_audio_send();
  test_golive_stream_audio_receive();
  test_golive_stream_audio_live_thread();
  test_stream_ops_optional();
  test_golive_default_capture();
  test_golive_default_capture_x11();
  test_golive_default_capture_cancel();
  test_golive_encoder_fallback();
  test_golive_encoder_config();
  test_golive_warn_latches();
  test_golive_open_warn_latch();
  test_golive_camera_warn_latch();
  test_golive_pcm_warn_latch();
  test_soundboard();
  test_soundboard_multiframe_pacing();
  test_soundboard_intent_fail();
  test_voice_exit_teardown();
  test_golive_pause_resume_and_keyframe();
  test_standalone_voice_camera_tx();
  test_platform_loopback_fake();
  test_golive_loopback_only_tx_and_mic_coexistence();
  test_golive_loopback_pause_gating();
  test_golive_loopback_platform_bridge_and_failures();
  test_app_media_cameras();
  test_app_media_cameras_idr_gate();
  test_app_media_cameras_cap_and_budget();
  test_app_media_cameras_optional();
  test_app_media_cameras_open_retry();
  return CT_RESULT("test_app_media");
}
