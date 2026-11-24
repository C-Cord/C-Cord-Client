// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "app_media_internal.h"

// ---- Go Live send / receive ------------------------------------------------------------------

// Clears the per-stream send-plane warning latches so a new stream reports its first failures
// again. Idempotent; also run on the idle pump to cover a PCM latch set from the audio pthread
// before the send plane ever opened.
static void reset_stream_warns(ccord_app_media *m)
{
  m->warn_capture = false;
  m->warn_camera = false;
  m->warn_encoder = false;
  m->warn_encode = false;
  m->warn_send = false;
  atomic_store(&m->warn_send_pcm, false);
  m->warn_loopback = false;
}

static void free_cam_buf(ccord_app_media *m)
{
  ccord_free(m->cam_buf);
  m->cam_buf = nullptr;
  m->cam_buf_cap = 0;
  m->cam_grab_fails = 0;
}

static bool ensure_pkt(ccord_app_media *m)
{
  if (!m->pkt) m->pkt = ccord_malloc(MEDIA_PKT_CAP);
  return m->pkt != nullptr;
}

static void maybe_free_pkt(ccord_app_media *m)
{
  if (!m->video && !m->cam_video && m->pkt) {
    ccord_free(m->pkt);
    m->pkt = nullptr;
  }
}

void app_media_close_cam_tx(ccord_app_media *m, bool close_cam)
{
  if (m->cam_video) {
    m->ops->video_close(m->ud, m->cam_video);
    m->cam_video = nullptr;
  }
  if (close_cam) {
    if (m->camera) {
      m->ops->camera_close(m->ud, m->camera);
      m->camera = nullptr;
    }
    m->opened_cam[0] = '\0';
    free_cam_buf(m);
    m->camera_retry_ms = 0;
    m->warn_camera = false;
  }
  maybe_free_pkt(m);
  m->cam_streaming = false;
  m->last_cam_ms = 0;
  m->cam_enc_retry_ms = 0;
  m->cam_pending_keyframe = false;
  m->warn_cam_encoder = false;
  m->warn_cam_encode = false;
  m->warn_cam_send = false;
}

void app_media_close_send(ccord_app_media *m)
{
  if (m->screen_cap) {
    m->ops->capture_close(m->ud, m->screen_cap);
    m->screen_cap = nullptr;
  }
  if (m->loopback) {
    m->ops->loopback_close(m->ud, m->loopback);
    m->loopback = nullptr;
  }
  m->loopback_paused = false;
  m->loopback_unsupported = false;
  m->loopback_retry_ms = 0;
  m->loopback_next_ms = 0;
  if (m->camera) {
    m->ops->camera_close(m->ud, m->camera);
    m->camera = nullptr;
  }
  m->opened_cam[0] = '\0';
  free_cam_buf(m);
  if (m->video) {
    m->ops->video_close(m->ud, m->video);
    m->video = nullptr;
  }
  maybe_free_pkt(m);
  m->last_video_ms = 0;
  m->streaming = false;
  m->was_paused = false;
  m->pending_keyframe = false;
  m->encoder_retry_ms = 0;
  m->camera_retry_ms = 0;
  m->capture_retry_ms = 0;
  m->cap_portal_refused = false;
  reset_stream_warns(m);
}

static void open_encoder(ccord_app_media *m, bool virtual_clock, uint64_t now)
{
  if (!app_media_open_retry_due(now, m->encoder_retry_ms)) return;
  ccord_stream_config sc;
  m->ops->stream_config(m->ud, &sc);
  int target_h = sc.resolution == CCORD_STREAM_RES_1080P   ? 1080
                 : sc.resolution == CCORD_STREAM_RES_1440P ? 1440
                 : sc.resolution == CCORD_STREAM_RES_SOURCE ? 0
                                                            : 720;
  int vw = 640, vh = 360;
  m->ops->resolve_dims(m->ud, 1280, 720, target_h, &vw, &vh);
  int vfps = sc.fps == CCORD_STREAM_FPS_15 ? 15 : sc.fps == CCORD_STREAM_FPS_60 ? 60 : 30;
  const char *pref_enc = sc.encoder == CCORD_STREAM_ENCODER_NVENC          ? "h264_nvenc"
                         : sc.encoder == CCORD_STREAM_ENCODER_VAAPI        ? "h264_vaapi"
                         : sc.encoder == CCORD_STREAM_ENCODER_AMF          ? "h264_amf"
                         : sc.encoder == CCORD_STREAM_ENCODER_QSV          ? "h264_qsv"
                         : sc.encoder == CCORD_STREAM_ENCODER_MF           ? "h264_mf"
                         : sc.encoder == CCORD_STREAM_ENCODER_VIDEOTOOLBOX ? "h264_videotoolbox"
                         : sc.encoder == CCORD_STREAM_ENCODER_X264         ? "libx264"
                         : sc.encoder == CCORD_STREAM_ENCODER_OPENH264     ? "libopenh264"
                                                                           : "auto";
  if (virtual_clock && (vw > 640 || vh > 360)) {
    vw = 640;
    vh = 360;
  }
  ccord_video_config vcfg = {.width = vw,
                             .height = vh,
                             .fps = vfps,
                             .bitrate = 2000000,
                             // The Go Live wire format is H.264 Annex-B: AV1 stream selections
                             // fall back to H.264 (the platform encodes H.264/MPEG4).
                             .codec = CCORD_VIDEO_CODEC_H264,
                             // Probe PipeWire frames only for the portal/auto capture frameworks;
                             // an explicit X11/GPU framework stays on software frames.
                             .use_pipewire = sc.framework == CCORD_STREAM_FRAMEWORK_PIPEWIRE ||
                                             sc.framework == CCORD_STREAM_FRAMEWORK_AUTO,
                             .preferred_encoder = pref_enc};
  ccord_error err;
  m->video = m->ops->video_open(m->ud, &vcfg, &err);
  if (!m->video && strcmp(pref_enc, "libx264") != 0) {
    // Requested hardware/auto encoder unavailable: fall back to software H.264 right away so
    // the stream still encodes and updates the preview instead of staying dark.
    vcfg.preferred_encoder = "libx264";
    m->video = m->ops->video_open(m->ud, &vcfg, &err);
    if (m->video && !m->warn_encoder) {
      m->warn_encoder = true;
      CCORD_LOG_W("media: encoder %s unavailable, fell back to software H.264 (libx264)", pref_enc);
    }
  }
  if (m->video) {
    m->encoder_retry_ms = 0;
  } else {
    m->encoder_retry_ms = now + MEDIA_OPEN_RETRY_MS;
    if (!m->warn_encoder) {
      m->warn_encoder = true;
      CCORD_LOG_W("media: encoder open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
    }
  }
}

// Default Go Live capture source when the user starts a stream without picking a source card:
// the portal system picker (Wayland, or any host where xdg-desktop-portal answers) first, then
// the primary X11 screen. Both go through the host capture seam; the backends fail cleanly when
// unavailable, so the coordinator never probes the platform itself. The X11 screen source opens
// the root window, which capture_x11 treats as the primary screen.
static void fill_default_capture_source(ccord_capture_source *src, bool portal)
{
  memset(src, 0, sizeof *src);
  src->kind = CCORD_CAPTURE_SCREEN;
  src->primary = true;
#if CCORD_OS_WINDOWS
  (void)portal;
  src->backend = CCORD_CAPBE_WIN32;
  snprintf(src->id, sizeof src->id, "win32:mon:primary");
  snprintf(src->title, sizeof src->title, "Screen 1 (primary)");
#elif CCORD_OS_MACOS
  (void)portal;
  src->backend = CCORD_CAPBE_MACOS;
  snprintf(src->id, sizeof src->id, "macos:mon:main");
  snprintf(src->title, sizeof src->title, "Main Display");
#elif CCORD_OS_ANDROID
  (void)portal;
  src->backend = CCORD_CAPBE_ANDROID;
  snprintf(src->id, sizeof src->id, "android:screen");
  snprintf(src->title, sizeof src->title, "Android Screen (MediaProjection)");
#else
  if (portal) {
    src->backend = CCORD_CAPBE_PORTAL;
    snprintf(src->id, sizeof src->id, "portal:pick");
    snprintf(src->title, sizeof src->title, "Wayland Screen (System Picker)");
    snprintf(src->app, sizeof src->app, "xdg-desktop-portal");
  } else {
    src->backend = CCORD_CAPBE_X11;
    snprintf(src->id, sizeof src->id, "x11:mon:root");
    snprintf(src->title, sizeof src->title, "Screen 1 (primary)");
  }
#endif
}

static void grab_camera_frame(ccord_app_media *m, uint64_t now, const uint8_t **out_bgra, int *out_w, int *out_h,
                              int *out_stride)
{
  *out_bgra = nullptr;
  *out_w = 0;
  *out_h = 0;
  *out_stride = 0;
  bool cam_changed = strcmp(m->prefs.camera_device_id, m->opened_cam) != 0;
  if (cam_changed) m->camera_retry_ms = 0;
  if ((!m->camera || cam_changed) && app_media_open_retry_due(now, m->camera_retry_ms)) {
    if (m->camera) m->ops->camera_close(m->ud, m->camera);
    ccord_error err;
    m->camera = m->ops->camera_open(m->ud, m->prefs.camera_device_id, &err);
    app_media_copy_id(m->opened_cam, m->prefs.camera_device_id);
    if (m->camera && !m->cam_buf) {
      m->cam_buf = ccord_malloc(MEDIA_CAM_INIT_BYTES);
      m->cam_buf_cap = m->cam_buf ? MEDIA_CAM_INIT_BYTES : 0;
      m->cam_grab_fails = 0;
    }
    if (m->camera) {
      m->camera_retry_ms = 0;
      m->cam_grab_fails = 0;
    } else {
      m->camera_retry_ms = now + MEDIA_OPEN_RETRY_MS;
      if (!m->warn_camera) {
        m->warn_camera = true;
        CCORD_LOG_W("media: camera open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
      }
    }
  }
  if (!m->camera || !m->cam_buf || m->cam_buf_cap == 0) return;

  int w = 0, h = 0;
  bool ok = m->ops->camera_grab(m->ud, m->camera, m->cam_buf, m->cam_buf_cap, &w, &h);
  if (!ok) {
    // Grow buffer if the camera backend needs more than our current capacity (up to 1080p BGRA).
    size_t next_cap = 0;
    if (w > 0 && h > 0) {
      size_t need = (size_t)w * (size_t)h * 4u;
      if (need > m->cam_buf_cap && need <= MEDIA_CAM_MAX_BYTES) next_cap = need;
    } else if (m->cam_buf_cap < MEDIA_CAM_MAX_BYTES) {
      if (m->cam_grab_fails < 255u) m->cam_grab_fails++;
      if (m->cam_grab_fails >= 2u) {
        next_cap = m->cam_buf_cap < MEDIA_CAM_HD_BYTES ? MEDIA_CAM_HD_BYTES : MEDIA_CAM_MAX_BYTES;
      }
    }
    if (next_cap > m->cam_buf_cap) {
      uint8_t *grown = ccord_realloc(m->cam_buf, next_cap);
      if (grown) {
        m->cam_buf = grown;
        m->cam_buf_cap = next_cap;
        m->cam_grab_fails = 0;
        w = 0;
        h = 0;
        ok = m->ops->camera_grab(m->ud, m->camera, m->cam_buf, m->cam_buf_cap, &w, &h);
      }
    }
  }
  if (ok && w > 0 && h > 0) {
    m->cam_grab_fails = 0;
    size_t actual = (size_t)w * (size_t)h * 4u;
    if (actual > 0 && actual <= m->cam_buf_cap / 2u) {
      uint8_t *shrunk = ccord_realloc(m->cam_buf, actual);
      if (shrunk) {
        m->cam_buf = shrunk;
        m->cam_buf_cap = actual;
      }
    }
    *out_bgra = m->cam_buf;
    *out_w = w;
    *out_h = h;
    *out_stride = w * 4;
  }
}

void app_media_pump_send(ccord_app_media *m, const ccord_app_media_voice *v, uint64_t now, bool virtual_clock,
                      bool stream_on, bool paused)
{
  if (!stream_on) {
    if (m->streaming || m->video || m->screen_cap || m->loopback ||
        (m->camera && (!v->active || !v->camera || !m->ops->voice_video_send))) {
      app_media_close_send(m);
    } else {
      m->was_paused = false;
      m->pending_keyframe = false;
      reset_stream_warns(m);
    }
    return;
  }
  app_media_close_cam_tx(m, false);
  if (!m->streaming) {
    m->stream_start_ms = now;
    m->streaming = true;
  }
  if (paused) {
    m->was_paused = true;
    return;
  }
  if (m->was_paused) {
    m->was_paused = false;
    m->last_video_ms = 0;
    m->pending_keyframe = true;
  }
  if (!m->video) open_encoder(m, virtual_clock, now);
  if (!m->video) return;

  // Throttle BEFORE capturing/converting a frame so a 60-144 Hz UI loop does not grab and discard
  // frames faster than the configured stream FPS.
  int fps = m->ops->video_fps(m->ud, m->video);
  uint64_t interval = fps > 0 ? (uint64_t)(1000 / fps) : 33u;
  if (m->last_video_ms != 0 && now >= m->last_video_ms && now - m->last_video_ms < interval) return;
  m->last_video_ms = now;

  // Source: camera when self_video is on, otherwise the selected screen/portal capture source.
  const uint8_t *bgra = nullptr;
  int w = 0, h = 0, stride = 0;
  if (v->camera) {
    if (m->screen_cap) {
      m->ops->capture_close(m->ud, m->screen_cap);
      m->screen_cap = nullptr;
    }
    grab_camera_frame(m, now, &bgra, &w, &h, &stride);
  } else {
    if (m->camera) {
      m->ops->camera_close(m->ud, m->camera);
      m->camera = nullptr;
      m->opened_cam[0] = '\0';
      free_cam_buf(m);
    }
    if (!m->screen_cap && app_media_open_retry_due(now, m->capture_retry_ms)) {
      ccord_capture_source src;
      ccord_error err;
      ccord_error_clear(&err);
      if (m->ops->capture_source(m->ud, &src)) {
        m->screen_cap = m->ops->capture_open(m->ud, &src, &err);
      } else {
        // No explicit source selected (Go Live started without picking a card, or Wayland where
        // the portal picker is the source): default to the portal picker, then the primary X11
        // screen, instead of leaving the stream without capture.
        if (!m->cap_portal_refused) {
          fill_default_capture_source(&src, true);
          m->screen_cap = m->ops->capture_open(m->ud, &src, &err);
          // A cancelled picker is the user's answer for this stream: don't re-prompt every retry.
          if (!m->screen_cap && err.code == CCORD_ERR_PERMISSION) m->cap_portal_refused = true;
        }
        if (!m->screen_cap) {
          fill_default_capture_source(&src, false);
          m->screen_cap = m->ops->capture_open(m->ud, &src, &err);
        }
      }
      if (m->screen_cap) {
        m->capture_retry_ms = 0;
      } else {
        m->capture_retry_ms = now + MEDIA_OPEN_RETRY_MS;
        if (!m->warn_capture) {
          m->warn_capture = true;
          CCORD_LOG_W("media: screen capture open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
        }
      }
    }
    if (m->screen_cap) {
      ccord_capture_frame frame;
      if (m->ops->capture_grab(m->ud, m->screen_cap, &frame)) {
        bgra = frame.data;
        w = frame.width;
        h = frame.height;
        stride = frame.stride;
      } else if (m->ops->capture_failed(m->ud, m->screen_cap)) {
        m->ops->capture_close(m->ud, m->screen_cap);
        m->screen_cap = nullptr;
        m->capture_retry_ms = now + MEDIA_OPEN_RETRY_MS;
      }
    }
  }

  if (!bgra || w <= 0 || h <= 0) return;
  if (!ensure_pkt(m)) return;

  if (m->pending_keyframe) {
    m->pending_keyframe = false;
    if (m->ops->video_force_keyframe) m->ops->video_force_keyframe(m->ud, m->video);
  }
  ccord_error err;
  size_t pkt_len = 0;
  if (m->ops->video_encode_bgra(m->ud, m->video, bgra, w, h, stride, m->pkt, MEDIA_PKT_CAP, &pkt_len, &err) !=
          CCORD_OK ||
      pkt_len == 0) {
    if (!m->warn_encode) {
      m->warn_encode = true;
      CCORD_LOG_W("media: stream encode failed: %s", err.message);
    }
    return;
  }
  uint32_t ts90k = (uint32_t)((now - m->stream_start_ms) * 90u);
  bool keyframe = m->ops->video_keyframe(m->ud, m->video);
  if (m->ops->stream_send(m->ud, m->pkt, pkt_len, ts90k, keyframe, &err) != CCORD_OK) {
    if (!m->warn_send) {
      m->warn_send = true;
      CCORD_LOG_W("media: stream send failed: %s", err.message);
    }
  }
  // Local preview: the encoder keeps a scaled RGBA copy of the frame it just encoded.
  int pw = 0, ph = 0;
  const uint8_t *rgba = m->ops->video_rgba(m->ud, m->video, &pw, &ph);
  if (rgba && pw > 0 && ph > 0) {
    (void)m->ops->upload_rgba(m->ud, MEDIA_VIDEO_KEY, rgba, pw, ph, &err);
  }
}

void app_media_pump_voice_camera(ccord_app_media *m, const ccord_app_media_voice *v, uint64_t now, bool virtual_clock,
                              bool stream_on)
{
  if (stream_on || !v->active || !v->camera || !m->ops->voice_video_send) {
    if (m->cam_streaming || m->cam_video || (!stream_on && m->camera)) {
      app_media_close_cam_tx(m, !stream_on);
    }
    return;
  }
  if (!m->cam_streaming) {
    m->cam_start_ms = now;
    m->cam_streaming = true;
  }
  if (!m->cam_video && app_media_open_retry_due(now, m->cam_enc_retry_ms)) {
    int vw = virtual_clock ? 640 : 1280;
    int vh = virtual_clock ? 360 : 720;
    ccord_video_config vcfg = {.width = vw,
                               .height = vh,
                               .fps = 30,
                               .bitrate = 1500000,
                               .codec = CCORD_VIDEO_CODEC_H264,
                               .use_pipewire = false,
                               .preferred_encoder = "auto"};
    ccord_error err;
    m->cam_video = m->ops->video_open(m->ud, &vcfg, &err);
    if (!m->cam_video) {
      vcfg.preferred_encoder = "libx264";
      m->cam_video = m->ops->video_open(m->ud, &vcfg, &err);
    }
    if (m->cam_video) {
      m->cam_enc_retry_ms = 0;
    } else {
      m->cam_enc_retry_ms = now + MEDIA_OPEN_RETRY_MS;
      if (!m->warn_cam_encoder) {
        m->warn_cam_encoder = true;
        CCORD_LOG_W("media: camera encoder open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
      }
    }
  }
  if (!m->cam_video) return;

  // Throttle BEFORE grabbing/converting a camera frame.
  int fps = m->ops->video_fps(m->ud, m->cam_video);
  uint64_t interval = fps > 0 ? (uint64_t)(1000 / fps) : 33u;
  if (m->last_cam_ms != 0 && now >= m->last_cam_ms && now - m->last_cam_ms < interval) return;
  m->last_cam_ms = now;

  const uint8_t *bgra = nullptr;
  int w = 0, h = 0, stride = 0;
  grab_camera_frame(m, now, &bgra, &w, &h, &stride);
  if (!bgra || w <= 0 || h <= 0) return;
  if (!ensure_pkt(m)) return;

  if (m->cam_pending_keyframe) {
    m->cam_pending_keyframe = false;
    if (m->ops->video_force_keyframe) m->ops->video_force_keyframe(m->ud, m->cam_video);
  }
  ccord_error err;
  size_t pkt_len = 0;
  if (m->ops->video_encode_bgra(m->ud, m->cam_video, bgra, w, h, stride, m->pkt, MEDIA_PKT_CAP, &pkt_len, &err) !=
          CCORD_OK ||
      pkt_len == 0) {
    if (!m->warn_cam_encode) {
      m->warn_cam_encode = true;
      CCORD_LOG_W("media: camera encode failed: %s", err.message);
    }
    return;
  }
  uint32_t ts90k = (uint32_t)((now - m->cam_start_ms) * 90u);
  bool keyframe = m->ops->video_keyframe(m->ud, m->cam_video);
  if (m->ops->voice_video_send(m->ud, m->pkt, pkt_len, ts90k, keyframe, &err) != CCORD_OK) {
    if (!m->warn_cam_send) {
      m->warn_cam_send = true;
      CCORD_LOG_W("media: camera send failed: %s", err.message);
    }
  }
}

// ---- remote voice cameras ---------------------------------------------------------------------

static void cam_close_slot(ccord_app_media *m, size_t i)
{
  cam_slot *c = &m->cams[i];
  if (!c->used) return;
  if (c->decoder) m->ops->decoder_close(m->ud, c->decoder);
  if (m->ops->texture_evict) m->ops->texture_evict(m->ud, c->key);
  memset(c, 0, sizeof *c);
}

void app_media_cam_close_all(ccord_app_media *m)
{
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) cam_close_slot(m, i);
}

static cam_slot *cam_find(ccord_app_media *m, ccord_id user)
{
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) {
    if (m->cams[i].used && m->cams[i].user == user) return &m->cams[i];
  }
  return nullptr;
}

// A slot exists iff its sender is listed as decoding; a changed generation restarts the decoder.
static void cam_reconcile(ccord_app_media *m, const ccord_camera_rx *list, size_t n)
{
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) {
    cam_slot *c = &m->cams[i];
    if (!c->used) continue;
    bool keep = false;
    for (size_t k = 0; k < n; k++) {
      if (list[k].decoding && list[k].user_id == c->user) {
        keep = true;
        break;
      }
    }
    if (!keep) cam_close_slot(m, i);
  }
  size_t slots = 0;
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) slots += m->cams[i].used ? 1u : 0u;
  for (size_t k = 0; k < n; k++) {
    if (!list[k].decoding) continue;
    cam_slot *c = cam_find(m, list[k].user_id);
    if (c && c->generation != list[k].generation) {
      cam_close_slot(m, (size_t)(c - m->cams));
      c = nullptr;
      slots--;
    }
    if (c) continue;
    if (slots >= CCORD_CAM_DECODE_MAX) continue;   // never more than the cap, whatever the list says
    for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) {
      if (m->cams[i].used) continue;
      c = &m->cams[i];
      memset(c, 0, sizeof *c);
      c->used = true;
      c->user = list[k].user_id;
      c->generation = list[k].generation;
      c->need_idr = true;
      snprintf(c->key, sizeof c->key, "__cam__:%llu", (unsigned long long)c->user);
      slots++;
      break;
    }
  }
}

static void cam_open_decoder(ccord_app_media *m, cam_slot *c, uint64_t now)
{
  if (c->decoder || !app_media_open_retry_due(now, c->retry_ms)) return;
  ccord_error err;
  c->decoder = m->ops->decoder_open(m->ud, &err);
  if (!c->decoder) {
    c->retry_ms = now + MEDIA_OPEN_RETRY_MS;
    CCORD_LOG_W("media: camera decoder open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
    return;
  }
  c->retry_ms = 0;
  c->need_idr = true;
  if (m->ops->decoder_set_max_dim) {
    m->ops->decoder_set_max_dim(m->ud, c->decoder, MEDIA_CAM_TEX_MAX_W, MEDIA_CAM_TEX_MAX_H);
  }
}

// FNV-1a over 4096 bytes of the RGBA sampled at a fixed stride, plus the centre pixel.
// Computed lazily only when ccord_app_media_cam_debug_of() is queried so production decoding
// never pays per-frame hashing overhead.
static void cam_fingerprint(const uint8_t *rgba, int w, int h, uint64_t *out_hash, uint8_t out_rgb[3])
{
  size_t total = (size_t)w * (size_t)h * 4u;
  size_t stride = total / 4096u;
  if (stride == 0) stride = 1;
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0, off = 0; i < 4096u && off < total; i++, off += stride) {
    hash ^= rgba[off];
    hash *= 1099511628211ull;
  }
  *out_hash = hash;
  size_t mid = (((size_t)h / 2u) * (size_t)w + (size_t)w / 2u) * 4u;
  out_rgb[0] = rgba[mid];
  out_rgb[1] = rgba[mid + 1];
  out_rgb[2] = rgba[mid + 2];
}

static void cam_upload(ccord_app_media *m, cam_slot *c)
{
  int w = 0, h = 0;
  const uint8_t *rgba = m->ops->decoder_rgba(m->ud, c->decoder, &w, &h);
  if (!rgba || w <= 0 || h <= 0) {
    c->fresh = false;
    return;
  }
  ccord_error err;
  if (m->ops->upload_rgba(m->ud, c->key, rgba, w, h, &err) != CCORD_OK) return;   // retried next pump
  c->fresh = false;
  c->w = w;
  c->h = h;
}

void app_media_pump_cameras(ccord_app_media *m, const ccord_app_media_voice *v, uint64_t now)
{
  if (!m->ops->camera_senders || !m->ops->camera_poll) return;
  if (!v->active) {
    app_media_cam_close_all(m);
    return;
  }
  ccord_camera_rx list[CCORD_CAM_RX_MAX];
  size_t n = m->ops->camera_senders(m->ud, list, CCORD_CAM_RX_MAX);
  if (n > CCORD_CAM_RX_MAX) n = CCORD_CAM_RX_MAX;
  cam_reconcile(m, list, n);
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) {
    if (m->cams[i].used) cam_open_decoder(m, &m->cams[i], now);
  }
  // Round robin from a rotating start so one busy sender cannot starve the others; no clock involved.
  int budget = MEDIA_CAM_AUS_PER_PUMP_TOTAL;
  bool progress = true;
  while (budget > 0 && progress) {
    progress = false;
    for (size_t k = 0; k < CCORD_CAM_RX_MAX && budget > 0; k++) {
      cam_slot *c = &m->cams[(m->cam_rr + k) % CCORD_CAM_RX_MAX];
      if (!c->used || !c->decoder) continue;
      const uint8_t *annexb = nullptr;
      size_t len = 0;
      bool keyframe = false;
      if (!m->ops->camera_poll(m->ud, c->user, &annexb, &len, &keyframe)) continue;
      progress = true;
      budget--;
      if (!annexb || len == 0) continue;
      if (c->need_idr && !keyframe) {
        c->dropped_until_idr++;
        continue;
      }
      ccord_error err;
      if (m->ops->decoder_feed(m->ud, c->decoder, annexb, len, &err) == CCORD_OK) {
        c->need_idr = false;
        c->fresh = true;
        c->frames++;
      } else {
        c->need_idr = true;
        c->decode_errors++;
      }
    }
  }
  m->cam_rr = (m->cam_rr + 1) % CCORD_CAM_RX_MAX;
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) {
    cam_slot *c = &m->cams[i];
    if (c->used && c->decoder && c->fresh) cam_upload(m, c);
  }
}

size_t ccord_app_media_cam_count(const ccord_app_media *m)
{
  size_t n = 0;
  if (!m) return 0;
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) n += m->cams[i].used ? 1u : 0u;
  return n;
}

bool ccord_app_media_cam_debug_of(const ccord_app_media *m, ccord_id user_id, ccord_app_media_cam_debug *out)
{
  if (!m || !out) return false;
  for (size_t i = 0; i < CCORD_CAM_RX_MAX; i++) {
    const cam_slot *c = &m->cams[i];
    if (!c->used || c->user != user_id) continue;
    memset(out, 0, sizeof *out);
    out->open = c->decoder != nullptr;
    out->generation = c->generation;
    out->w = c->w;
    out->h = c->h;
    out->frames = c->frames;
    out->decode_errors = c->decode_errors;
    out->dropped_until_idr = c->dropped_until_idr;
    if (c->decoder && c->w > 0 && c->h > 0) {
      int dw = 0, dh = 0;
      const uint8_t *rgba = m->ops->decoder_rgba(m->ud, c->decoder, &dw, &dh);
      if (rgba && dw > 0 && dh > 0) {
        cam_fingerprint(rgba, dw, dh, &out->frame_hash, out->center_rgb);
      }
    }
    snprintf(out->key, sizeof out->key, "%s", c->key);
    return true;
  }
  return false;
}

void app_media_pump_receive(ccord_app_media *m, uint64_t now)
{
  if (!m->ops->watch_active(m->ud)) {
    if (m->decoder) {
      m->ops->decoder_close(m->ud, m->decoder);
      m->decoder = nullptr;
    }
    m->recv_fresh = false;
    m->recv_need_idr = true;
    m->decoder_retry_ms = 0;
    return;
  }
  if (!m->decoder) {
    if (!app_media_open_retry_due(now, m->decoder_retry_ms)) return;
    ccord_error err;
    m->decoder = m->ops->decoder_open(m->ud, &err);
    if (!m->decoder) {
      m->decoder_retry_ms = now + MEDIA_OPEN_RETRY_MS;
      CCORD_LOG_W("media: decoder open failed (retry in %u ms): %s", MEDIA_OPEN_RETRY_MS, err.message);
      return;
    }
    if (m->ops->decoder_set_max_dim) {
      m->ops->decoder_set_max_dim(m->ud, m->decoder, 1920, 1080);
    }
    m->decoder_retry_ms = 0;
    m->recv_need_idr = true;
  }
  ccord_error err;
  const uint8_t *annexb = nullptr;
  size_t len = 0;
  bool keyframe = false;
  // Drain a bounded batch per frame so a burst can't stall the UI loop.
  bool fed = false;
  for (int i = 0; i < 16 && m->ops->stream_poll(m->ud, &annexb, &len, &keyframe); i++) {
    if (!annexb || len == 0) continue;
    if (m->recv_need_idr && !keyframe) continue;
    if (m->ops->decoder_feed(m->ud, m->decoder, annexb, len, &err) == CCORD_OK) {
      m->recv_need_idr = false;
      fed = true;
    } else {
      m->recv_need_idr = true;
    }
  }
  // Only re-upload the stage texture when a packet decoded cleanly since the last upload.
  if (fed) m->recv_fresh = true;
  if (!m->recv_fresh) return;
  int w = 0, h = 0;
  const uint8_t *rgba = m->ops->decoder_rgba(m->ud, m->decoder, &w, &h);
  if (!rgba || w <= 0 || h <= 0) {
    m->recv_fresh = false;
    return;
  }
  if (m->ops->upload_rgba(m->ud, MEDIA_VIDEO_KEY, rgba, w, h, &err) == CCORD_OK) {
    m->recv_fresh = false;
  }
}

