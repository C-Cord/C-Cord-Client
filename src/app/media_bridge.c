// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"

#include "voice_audio_thread.h"

#include <ccord/net/voice_dsp.h>
#include <ccord/platform/capture.h>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// ---- M5 media coordinator ops (main.c supplies the real session/store/ui/platform seams) ----

static void media_op_prefs(void *ud, ccord_ui_voice_prefs *out) { ccord_ui_voice_settings(((app *)ud)->ui, out); }

static uint8_t media_op_user_volume(void *ud, ccord_id user_id)
{
  return ccord_ui_user_volume(((app *)ud)->ui, user_id);
}

static bool media_op_key_held(void *ud, int scancode)
{
  (void)ud;
  if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT) return false;
  const bool *state = SDL_GetKeyboardState(nullptr);
  return state && state[scancode];
}

static bool media_op_global_ptt_held(void *ud) { return ccord_platform_global_ptt_held(((app *)ud)->platform); }

static uint64_t media_op_now_ms(void *ud) { return app_now_ms((app *)ud); }

static bool media_op_dsp_available(void *ud)
{
  (void)ud;
  return ccord_dsp_available();
}

static void media_op_voice(void *ud, ccord_app_media_voice *out)
{
  app *a = ud;
  memset(out, 0, sizeof *out);
  if (!a->store) return;
  ccord_id self_id = ccord_store_self_id(a->store);
  ccord_id vch = ccord_store_active_voice_channel(a->store);
  if (vch == CCORD_ID_NONE || self_id == CCORD_ID_NONE) return;
  const ccord_channel *vchan = ccord_store_channel(a->store, vch);
  ccord_id g = vchan ? vchan->guild_id : ccord_store_selected_guild(a->store);
  const ccord_voice_state *vs = ccord_store_voice_user(a->store, g, self_id);
  if (!vs || vs->channel_id == CCORD_ID_NONE) {
    vs = ccord_store_voice_user(a->store, CCORD_DM_GUILD_ID, self_id);
    if (vs && vs->channel_id != CCORD_ID_NONE) g = CCORD_DM_GUILD_ID;
  }
  if (!vs || vs->channel_id == CCORD_ID_NONE) {
    size_t ng = ccord_store_guild_count(a->store);
    for (size_t i = 0; i < ng; i++) {
      const ccord_guild *gu = ccord_store_guild_at(a->store, i);
      const ccord_voice_state *cand = gu ? ccord_store_voice_user(a->store, gu->id, self_id) : nullptr;
      if (cand && cand->channel_id != CCORD_ID_NONE) {
        vs = cand;
        g = gu->id;
        break;
      }
    }
  }
  bool ses_mute = a->session && ccord_session_voice_muted(a->session);
  bool ses_deaf = a->session && ccord_session_voice_deafened(a->session);
  out->active = true;
  out->guild_id = g;
  out->channel_id = vch;
  out->deafened = (vs && vs->self_deaf) || ses_deaf;
  out->muted = (vs && (vs->self_mute || vs->self_deaf)) || ses_mute || out->deafened;
  out->suppressed = vs && vs->suppress;
  out->camera = vs && vs->self_video;
}

static size_t media_op_voice_users(void *ud, ccord_id *out, size_t max)
{
  app *a = ud;
  if (!a->store) return 0;
  ccord_id vch = ccord_store_active_voice_channel(a->store);
  ccord_id self_id = ccord_store_self_id(a->store);
  if (vch == CCORD_ID_NONE) return 0;
  size_t n = ccord_store_voice_count(a->store, vch);
  size_t k = 0;
  for (size_t i = 0; i < n && k < max; i++) {
    const ccord_voice_state *vs = ccord_store_voice_at(a->store, vch, i);
    if (vs && vs->user_id != self_id && vs->user_id != CCORD_ID_NONE) out[k++] = vs->user_id;
  }
  return k;
}

static bool media_op_stream_active(void *ud) { return ccord_store_stream_active(((app *)ud)->store); }

static void media_op_stream_config(void *ud, ccord_stream_config *out)
{
  app *a = ud;
  memset(out, 0, sizeof *out);
  const ccord_stream_state *st = ccord_store_stream(a->store);
  if (st) *out = st->config;
}

static bool media_op_stream_paused(void *ud)
{
  const ccord_stream_state *st = ccord_store_stream(((app *)ud)->store);
  return st && st->active && st->paused;
}

static bool media_op_stream_want_keyframe(void *ud)
{
  return ccord_session_consume_keyframe_request(((app *)ud)->session);
}

static bool media_op_watch_active(void *ud)
{
  const ccord_watched_stream *w = ccord_session_watched_stream(((app *)ud)->session);
  return w && w->active && !w->paused;
}

static bool media_op_capture_source(void *ud, ccord_capture_source *out)
{
  if (ccord_ui_active_capture_source(((app *)ud)->ui, out)) return true;
  if (ccord_capture_fake_enabled()) {
    ccord_error err;
    return ccord_capture_list(CCORD_CAPBE_FAKE, CCORD_CAPTURE_SCREEN, out, 1, &err) > 0;
  }
  return false;
}

static ccord_status media_op_set_voice_config(void *ud, const ccord_voice_config *cfg, ccord_error *err)
{
  return ccord_session_set_voice_config(((app *)ud)->session, cfg, err);
}

static ccord_status media_op_set_ptt_held(void *ud, bool held, ccord_error *err)
{
  return ccord_session_set_ptt_held(((app *)ud)->session, held, err);
}

static ccord_status media_op_set_user_volume(void *ud, ccord_id user_id, uint8_t pct, ccord_error *err)
{
  return ccord_session_set_user_volume(((app *)ud)->session, user_id, pct, err);
}

static ccord_status media_op_push_pcm(void *ud, const int16_t *pcm, size_t frames, ccord_error *err)
{
  return ccord_session_push_voice_pcm(((app *)ud)->session, pcm, frames, err);
}

static size_t media_op_pull_pcm(void *ud, int16_t *pcm, size_t max_frames)
{
  return ccord_session_pull_voice_pcm(((app *)ud)->session, pcm, max_frames);
}

static ccord_status media_op_stream_send(void *ud, const uint8_t *annexb, size_t len, uint32_t ts90k, bool keyframe,
                                         ccord_error *err)
{
  return ccord_session_stream_send_frame(((app *)ud)->session, annexb, len, ts90k, keyframe, err);
}

static bool media_op_stream_poll(void *ud, const uint8_t **annexb, size_t *len, bool *keyframe)
{
  return ccord_session_stream_poll_frame(((app *)ud)->session, annexb, len, keyframe);
}

static ccord_status media_op_stream_send_pcm(void *ud, const int16_t *pcm, size_t samples, ccord_error *err)
{
  return ccord_session_stream_send_pcm(((app *)ud)->session, pcm, samples, err);
}

static size_t media_op_stream_poll_pcm(void *ud, int16_t *out_pcm, size_t cap_samples)
{
  return ccord_session_stream_poll_pcm(((app *)ud)->session, out_pcm, cap_samples);
}

static ccord_status media_op_voice_video_send(void *ud, const uint8_t *annexb, size_t len, uint32_t ts90k, bool keyframe,
                                              ccord_error *err)
{
  return ccord_session_voice_send_frame(((app *)ud)->session, annexb, len, ts90k, keyframe, err);
}

static ccord_status media_op_play_soundboard(void *ud, ccord_id guild_id, ccord_id sound_id, ccord_error *err)
{
  return ccord_session_play_soundboard(((app *)ud)->session, guild_id, sound_id, err);
}

// Consumes arrived CDN soundboard media ("/soundboard-sounds/<id>.mp3") directly via O(1) filtered
// pop without disturbing avatars/icons/attachments in the session media queue. Also drains leftover
// soundboard clips when discard_pending_soundboard() runs right after leaving voice.
static bool media_op_pop_soundboard(void *ud, const uint8_t **data, size_t *len, uint8_t *vol_pct)
{
  app *a = ud;
  if (!a->session || !a->store) return false;
  const char *url = nullptr;
  const uint8_t *mdata = nullptr;
  size_t mlen = 0;
  while (ccord_session_pop_media_matching(a->session, "/soundboard-sounds/", &url, &mdata, &mlen)) {
    if (!mdata || mlen == 0) continue;
    *data = mdata;
    *len = mlen;
    uint8_t vol = 100u;
    const char *tag = url ? strstr(url, "/soundboard-sounds/") : nullptr;
    if (tag != nullptr) {
      const char *id_str = tag + strlen("/soundboard-sounds/");
      char *end = nullptr;
      errno = 0;
      unsigned long long sid = strtoull(id_str, &end, 10);
      if (errno == 0 && end != id_str && sid != 0u) {
        const ccord_soundboard_sound *sb = ccord_store_find_soundboard(a->store, (ccord_id)sid);
        if (sb) vol = sb->volume_pct;
      }
    }
    *vol_pct = vol;
    return true;
  }
  return false;
}

static ccord_voice_audio_thread *media_op_thread_start(void *ud, const ccord_voice_audio_io *io, ccord_error *err)
{
  (void)ud;
  return ccord_voice_audio_thread_start(io, err);
}

static void media_op_thread_controls(void *ud, ccord_voice_audio_thread *t, bool muted, bool deafened, int in_vol,
                                     int out_vol)
{
  (void)ud;
  ccord_voice_audio_thread_set_controls(t, muted, deafened, in_vol, out_vol);
}

static void media_op_thread_controls_ex(void *ud, ccord_voice_audio_thread *t, bool muted, bool deafened,
                                        bool cap_suppressed, bool ptt_idle, int in_vol, int out_vol)
{
  (void)ud;
  ccord_voice_audio_thread_set_controls_ex(t, muted, deafened, cap_suppressed, ptt_idle, in_vol, out_vol);
}

static void media_op_thread_stop(void *ud, ccord_voice_audio_thread *t)
{
  (void)ud;
  ccord_voice_audio_thread_stop(t);
}

static ccord_audio *media_op_audio_open(void *ud, bool capture, const char *device_id, ccord_error *err)
{
  (void)ud;
  ccord_audio_config cfg = {.sample_rate = 48000,
                            .channels = 2,
                            .frame_samples = 960,
                            .capture = capture,
                            .device_id = (device_id && device_id[0]) ? device_id : nullptr};
  return ccord_audio_create(&cfg, err);
}

static void media_op_audio_close(void *ud, ccord_audio *a)
{
  (void)ud;
  ccord_audio_destroy(a);
}

static size_t media_op_audio_read(void *ud, ccord_audio *a, int16_t *pcm, size_t frames)
{
  (void)ud;
  return ccord_audio_read(a, pcm, frames);
}

static size_t media_op_audio_write(void *ud, ccord_audio *a, const int16_t *pcm, size_t frames)
{
  (void)ud;
  return ccord_audio_write(a, pcm, frames);
}

static size_t media_op_devices_list(void *ud, bool capture, ccord_audio_device_info *out, size_t max)
{
  (void)ud;
  return ccord_audio_devices_list(capture, out, max);
}

static void media_op_hotplug_register(void *ud, ccord_audio_hotplug_fn fn, void *cb_ud)
{
  (void)ud;
  ccord_audio_set_hotplug_callback(fn, cb_ud);
}

static ccord_camera *media_op_camera_open(void *ud, const char *device_id, ccord_error *err)
{
  (void)ud;
  return ccord_camera_open((device_id && device_id[0]) ? device_id : nullptr, err);
}

static bool media_op_camera_grab(void *ud, ccord_camera *c, uint8_t *bgra, size_t cap, int *out_w, int *out_h)
{
  (void)ud;
  return ccord_camera_grab(c, bgra, cap, out_w, out_h);
}

static void media_op_camera_close(void *ud, ccord_camera *c)
{
  (void)ud;
  ccord_camera_close(c);
}

static ccord_capture *media_op_capture_open(void *ud, const ccord_capture_source *src, ccord_error *err)
{
  (void)ud;
  return ccord_capture_open(src, err);
}

static bool media_op_capture_grab(void *ud, ccord_capture *c, ccord_capture_frame *out)
{
  (void)ud;
  return ccord_capture_grab(c, out);
}

static bool media_op_capture_failed(void *ud, const ccord_capture *c)
{
  (void)ud;
  return ccord_capture_failed(c);
}

static void media_op_capture_close(void *ud, ccord_capture *c)
{
  (void)ud;
  ccord_capture_close(c);
}

// ---- Go Live "Share System Audio": desktop/monitor loopback (never the microphone) ----
static bool media_op_stream_loopback_audio(void *ud)
{
  return ccord_ui_go_live_share_audio(((app *)ud)->ui);
}

static ccord_loopback_audio *media_op_loopback_open(void *ud, ccord_error *err)
{
  (void)ud;
  return ccord_loopback_audio_open(err);
}

static size_t media_op_loopback_read(void *ud, ccord_loopback_audio *l, int16_t *pcm, size_t max_samples)
{
  (void)ud;
  return ccord_loopback_audio_read(l, pcm, max_samples);
}

static bool media_op_loopback_failed(void *ud, const ccord_loopback_audio *l)
{
  (void)ud;
  return ccord_loopback_audio_failed(l);
}

static void media_op_loopback_set_paused(void *ud, ccord_loopback_audio *l, bool paused)
{
  (void)ud;
  ccord_loopback_audio_set_paused(l, paused);
}

static void media_op_loopback_close(void *ud, ccord_loopback_audio *l)
{
  (void)ud;
  ccord_loopback_audio_close(l);
}

static void media_op_resolve_dims(void *ud, int src_w, int src_h, int target_h, int *out_w, int *out_h)
{
  (void)ud;
  ccord_video_resolve_dimensions(src_w, src_h, target_h, out_w, out_h);
}

static ccord_video_stream *media_op_video_open(void *ud, const ccord_video_config *cfg, ccord_error *err)
{
  (void)ud;
  return ccord_video_stream_create(cfg, err);
}

static void media_op_video_close(void *ud, ccord_video_stream *v)
{
  (void)ud;
  ccord_video_stream_destroy(v);
}

static int media_op_video_fps(void *ud, const ccord_video_stream *v)
{
  (void)ud;
  return ccord_video_stream_fps(v);
}

static bool media_op_video_keyframe(void *ud, const ccord_video_stream *v)
{
  (void)ud;
  return ccord_video_stream_last_keyframe(v);
}

static ccord_status media_op_video_encode_bgra(void *ud, ccord_video_stream *v, const uint8_t *bgra, int w, int h,
                                               int stride, uint8_t *out_pkt, size_t cap, size_t *out_len,
                                               ccord_error *err)
{
  (void)ud;
  return ccord_video_stream_encode_bgra(v, bgra, w, h, stride, out_pkt, cap, out_len, err);
}

static void media_op_video_force_keyframe(void *ud, ccord_video_stream *v)
{
  (void)ud;
  ccord_video_stream_force_keyframe(v);
}

static const uint8_t *media_op_video_rgba(void *ud, const ccord_video_stream *v, int *out_w, int *out_h)
{
  (void)ud;
  return ccord_video_stream_last_rgba(v, out_w, out_h);
}

static ccord_video_decoder *media_op_decoder_open(void *ud, ccord_error *err)
{
  (void)ud;
  return ccord_video_decoder_create(err);
}

static ccord_status media_op_decoder_feed(void *ud, ccord_video_decoder *d, const uint8_t *annexb, size_t len,
                                          ccord_error *err)
{
  (void)ud;
  return ccord_video_decoder_feed(d, annexb, len, err);
}

static const uint8_t *media_op_decoder_rgba(void *ud, const ccord_video_decoder *d, int *out_w, int *out_h)
{
  (void)ud;
  return ccord_video_decoder_last_rgba(d, out_w, out_h);
}

static void media_op_decoder_close(void *ud, ccord_video_decoder *d)
{
  (void)ud;
  ccord_video_decoder_destroy(d);
}

static ccord_status media_op_sound_decode(void *ud, const uint8_t *data, size_t len, int16_t *out, size_t cap,
                                          size_t *out_samples, ccord_error *err)
{
  (void)ud;
  return ccord_sound_decode(data, len, out, cap, out_samples, err);
}

static ccord_status media_op_upload_rgba(void *ud, const char *key, const uint8_t *rgba, int w, int h, ccord_error *err)
{
  app *a = ud;
  if (!a->renderer) return ccord_error_set(err, CCORD_ERR_INVALID, "media: no renderer");
  if (a->platform && ccord_platform_in_background(a->platform)) return CCORD_OK;
  ccord_status st = ccord_render_upload_rgba(a->renderer, key, rgba, w, h, err);
  if (st == CCORD_OK) a->media_redraw = true;
  return st;
}

// Remote voice cameras (multi-camera): one decoder per admitted sender, one '__cam__:<uid>' texture each.
static size_t media_op_camera_senders(void *ud, ccord_camera_rx *out, size_t max)
{
  return ccord_session_camera_senders(((app *)ud)->session, out, max);
}

static bool media_op_camera_poll(void *ud, ccord_id user_id, const uint8_t **annexb, size_t *len, bool *keyframe)
{
  return ccord_session_camera_poll_frame(((app *)ud)->session, user_id, annexb, len, keyframe);
}

static void media_op_texture_evict(void *ud, const char *key)
{
  app *a = ud;
  if (a->platform && ccord_platform_in_background(a->platform)) {
    a->deferred_lowmem_evict = true;
    return;
  }
  if (a->renderer) {
    ccord_render_evict_texture(a->renderer, key);
    a->media_redraw = true;
  }
}

static void media_op_decoder_set_max_dim(void *ud, ccord_video_decoder *d, int max_w, int max_h)
{
  (void)ud;
  ccord_video_decoder_set_max_dim(d, max_w, max_h);
}

const ccord_app_media_ops k_app_media_ops = {
  .prefs = media_op_prefs,
  .user_volume = media_op_user_volume,
  .key_held = media_op_key_held,
  .global_ptt_held = media_op_global_ptt_held,
  .now_ms = media_op_now_ms,
  .dsp_available = media_op_dsp_available,
  .voice = media_op_voice,
  .voice_users = media_op_voice_users,
  .stream_active = media_op_stream_active,
  .stream_config = media_op_stream_config,
  .stream_paused = media_op_stream_paused,
  .stream_loopback_audio = media_op_stream_loopback_audio,
  .stream_want_keyframe = media_op_stream_want_keyframe,
  .watch_active = media_op_watch_active,
  .capture_source = media_op_capture_source,
  .set_voice_config = media_op_set_voice_config,
  .set_ptt_held = media_op_set_ptt_held,
  .set_user_volume = media_op_set_user_volume,
  .push_pcm = media_op_push_pcm,
  .pull_pcm = media_op_pull_pcm,
  .stream_send = media_op_stream_send,
  .stream_poll = media_op_stream_poll,
  .stream_send_pcm = media_op_stream_send_pcm,
  .stream_poll_pcm = media_op_stream_poll_pcm,
  .voice_video_send = media_op_voice_video_send,
  .play_soundboard = media_op_play_soundboard,
  .pop_soundboard = media_op_pop_soundboard,
  .thread_start = media_op_thread_start,
  .thread_controls = media_op_thread_controls,
  .thread_controls_ex = media_op_thread_controls_ex,
  .thread_stop = media_op_thread_stop,
  .audio_open = media_op_audio_open,
  .audio_close = media_op_audio_close,
  .audio_read = media_op_audio_read,
  .audio_write = media_op_audio_write,
  .devices_list = media_op_devices_list,
  .hotplug_register = media_op_hotplug_register,
  .camera_open = media_op_camera_open,
  .camera_grab = media_op_camera_grab,
  .camera_close = media_op_camera_close,
  .capture_open = media_op_capture_open,
  .capture_grab = media_op_capture_grab,
  .capture_failed = media_op_capture_failed,
  .capture_close = media_op_capture_close,
  .loopback_open = media_op_loopback_open,
  .loopback_read = media_op_loopback_read,
  .loopback_failed = media_op_loopback_failed,
  .loopback_set_paused = media_op_loopback_set_paused,
  .loopback_close = media_op_loopback_close,
  .resolve_dims = media_op_resolve_dims,
  .video_open = media_op_video_open,
  .video_close = media_op_video_close,
  .video_fps = media_op_video_fps,
  .video_keyframe = media_op_video_keyframe,
  .video_encode_bgra = media_op_video_encode_bgra,
  .video_force_keyframe = media_op_video_force_keyframe,
  .video_rgba = media_op_video_rgba,
  .decoder_open = media_op_decoder_open,
  .decoder_feed = media_op_decoder_feed,
  .decoder_rgba = media_op_decoder_rgba,
  .decoder_close = media_op_decoder_close,
  .sound_decode = media_op_sound_decode,
  .upload_rgba = media_op_upload_rgba,
  .camera_senders = media_op_camera_senders,
  .camera_poll = media_op_camera_poll,
  .texture_evict = media_op_texture_evict,
  .decoder_set_max_dim = media_op_decoder_set_max_dim,
};

