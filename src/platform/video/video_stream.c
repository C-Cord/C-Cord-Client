// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/platform/platform.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#if CCORD_HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#endif

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>

#define PLATFORM_SWS_DST_PAD 128u

// ---- video streaming codec pipeline ----

// Mutable lazily-initialized state for ccord_video_stream (accessed via pointer so const getters
// can lazily materialize last_rgba or probe PipeWire without casting away const).
typedef struct video_stream_lazy {
  bool pipewire_probed;
  bool pipewire_available;
  struct SwsContext *sws_to_rgba;
  struct SwsContext *sws_preview_rgba;
  uint8_t *last_rgba;
  size_t last_rgba_cap;
  int last_w;
  int last_h;
  bool preview_wanted;
  bool enc_preview_dirty;
} video_stream_lazy;

struct ccord_video_stream {
  int width;
  int height;
  int fps;
  int bitrate;
  bool use_pipewire;
  bool last_keyframe;
  bool force_keyframe;
  const AVCodec *enc_codec;
  const AVCodec *dec_codec;
  AVCodecContext *enc_ctx;
  AVCodecContext *dec_ctx;   // opened lazily on first decode_rgba()
  AVFrame *enc_frame;
  AVFrame *dec_frame;        // allocated lazily on first decode_rgba()
  AVPacket *pkt;
  struct SwsContext *sws_to_yuv;
  int64_t pts;
  uint32_t synth_step;
  video_stream_lazy *lazy;
  video_stream_lazy lazy_storage;
};

void ccord_video_resolve_dimensions(int src_w, int src_h, int target_height, int *out_w, int *out_h)
{
  int sw = src_w > 0 ? src_w : 1280;
  int sh = src_h > 0 ? src_h : 720;
  int tw = sw;
  int th = sh;
  if (target_height > 0 && target_height < sh) {
    th = target_height;
    tw = (int)(((int64_t)sw * (int64_t)th + (sh / 2)) / sh);
  } else if (target_height > 0 && src_w <= 0) {
    th = target_height;
    tw = (target_height * 16) / 9;
  }
  if (tw < 16) tw = 16;
  if (th < 16) th = 16;
  if (tw > 3840) tw = 3840;
  if (th > 2160) th = 2160;
  tw = (tw + 1) & ~1;
  th = (th + 1) & ~1;
  if (out_w) *out_w = tw;
  if (out_h) *out_h = th;
}

static bool open_video_codec_pair(ccord_video_stream *v, const AVCodec *enc)
{
  if (!enc) return false;
  const AVCodec *dec = avcodec_find_decoder(enc->id);
  if (!dec) return false;
  AVCodecContext *ec = avcodec_alloc_context3(enc);
  if (!ec) return false;
  ec->width = v->width;
  ec->height = v->height;
  ec->time_base = (AVRational){1, v->fps};
  ec->framerate = (AVRational){v->fps, 1};
  ec->pix_fmt = (enc->name && strcmp(enc->name, "h264_mediacodec") == 0) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
  ec->bit_rate = v->bitrate;
  ec->gop_size = v->fps;
  ec->max_b_frames = 0;
  ec->thread_count = 1;
  AVDictionary *opts = nullptr;
  if (enc->id == AV_CODEC_ID_H264) {
    if (enc->name && strcmp(enc->name, "h264_mediacodec") == 0) {
      av_dict_set(&opts, "bitrate_mode", "cbr", 0);
      av_dict_set(&opts, "i_frame_interval", "1", 0);
    } else if (enc->name && strcmp(enc->name, "libx264") == 0) {
      av_dict_set(&opts, "preset", "ultrafast", 0);
      av_dict_set(&opts, "tune", "zerolatency", 0);
      av_dict_set(&opts, "annexb", "1", 0);
    } else if (enc->name && strcmp(enc->name, "h264_nvenc") == 0) {
      av_dict_set(&opts, "preset", "p1", 0);
      av_dict_set(&opts, "tune", "ull", 0);
      av_dict_set(&opts, "zerolatency", "1", 0);
    } else if (enc->name && strcmp(enc->name, "h264_amf") == 0) {
      av_dict_set(&opts, "usage", "ultralowlatency", 0);
    } else if (enc->name && strcmp(enc->name, "h264_videotoolbox") == 0) {
      av_dict_set(&opts, "realtime", "1", 0);
    } else if (enc->name && strcmp(enc->name, "h264_qsv") == 0) {
      av_dict_set(&opts, "preset", "veryfast", 0);
      av_dict_set(&opts, "async_depth", "1", 0);
    } else if (enc->name && strcmp(enc->name, "h264_mf") == 0) {
      av_dict_set(&opts, "scenario", "live_streaming", 0);
    } else if (enc->name && strcmp(enc->name, "libopenh264") == 0) {
      av_dict_set(&opts, "allow_skip_frames", "0", 0);
    }
  }
  int rc = avcodec_open2(ec, enc, &opts);
  av_dict_free(&opts);
  if (rc < 0) {
    avcodec_free_context(&ec);
    return false;
  }
  // Decoder context is opened lazily in decode_rgba() so outbound-only streams never pay its memory cost.
  v->enc_codec = enc;
  v->dec_codec = dec;
  v->enc_ctx = ec;
  v->dec_ctx = nullptr;
  return true;
}

static ccord_status ensure_video_decoder(ccord_video_stream *v, ccord_error *err)
{
  if (v->dec_ctx && v->dec_frame) return CCORD_OK;
  if (!v->dec_codec) {
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "video decode: no matching decoder");
  }
  if (!v->dec_ctx) {
    AVCodecContext *dc = avcodec_alloc_context3(v->dec_codec);
    if (!dc) return ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: out of memory");
    dc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    dc->thread_count = 1;
    if (avcodec_open2(dc, v->dec_codec, nullptr) < 0) {
      avcodec_free_context(&dc);
      return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "video decode: avcodec_open2 failed");
    }
    v->dec_ctx = dc;
  }
  if (!v->dec_frame) {
    v->dec_frame = av_frame_alloc();
    if (!v->dec_frame) return ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: out of memory");
  }
  return CCORD_OK;
}

static bool probe_h264_encoders(ccord_video_stream *v, const char *preferred)
{
  if (preferred && preferred[0] != '\0' && strcmp(preferred, "auto") != 0) {
    if (open_video_codec_pair(v, avcodec_find_encoder_by_name(preferred))) {
      return true;
    }
  }
  static const char *const k_priority[] = {
#if defined(__APPLE__)
      "h264_videotoolbox",
      "libx264",
      "libopenh264",
#elif defined(_WIN32)
      "h264_nvenc",
      "h264_amf",
      "h264_qsv",
      "h264_mf",
      "libx264",
      "libopenh264",
#elif defined(__ANDROID__)
      "h264_mediacodec",
      "libx264",
      "libopenh264",
#else
      "h264_nvenc",
      "h264_vaapi",
      "h264_amf",
      "libx264",
      "libopenh264",
#endif
  };
  for (size_t i = 0; i < sizeof(k_priority) / sizeof(k_priority[0]); i++) {
    if (open_video_codec_pair(v, avcodec_find_encoder_by_name(k_priority[i]))) {
      return true;
    }
  }
  if (open_video_codec_pair(v, avcodec_find_encoder(AV_CODEC_ID_H264))) {
    return true;
  }
  return false;
}

static bool probe_av1_encoders(ccord_video_stream *v, const char *preferred)
{
  if (preferred && preferred[0] && strstr(preferred, "av1") != nullptr &&
      open_video_codec_pair(v, avcodec_find_encoder_by_name(preferred))) {
    return true;
  }
  static const char *const k_av1[] = {
#if defined(__APPLE__)
      "av1_videotoolbox",
      "libsvtav1",
#elif defined(_WIN32)
      "av1_nvenc",
      "av1_amf",
      "av1_qsv",
      "libsvtav1",
#elif defined(__ANDROID__)
      "av1_mediacodec",
      "libsvtav1",
#else
      "av1_nvenc",
      "av1_vaapi",
      "av1_amf",
      "av1_qsv",
      "libsvtav1",
#endif
  };
  for (size_t i = 0; i < sizeof(k_av1) / sizeof(k_av1[0]); i++) {
    if (open_video_codec_pair(v, avcodec_find_encoder_by_name(k_av1[i]))) {
      return true;
    }
  }
  return open_video_codec_pair(v, avcodec_find_encoder(AV_CODEC_ID_AV1));
}

static bool probe_pipewire(void)
{
#if !CCORD_HAVE_PIPEWIRE
  return false;
#else
  if (!pw_get_library_version()) return false;
  pw_init(nullptr, nullptr);
  struct pw_main_loop *loop = pw_main_loop_new(nullptr);
  if (!loop) {
    pw_deinit();
    return false;
  }
  struct pw_context *ctx = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
  if (!ctx) {
    pw_main_loop_destroy(loop);
    pw_deinit();
    return false;
  }
  struct pw_core *core = pw_context_connect(ctx, nullptr, 0);
  bool available = (core != nullptr);
  if (core) pw_core_disconnect(core);
  pw_context_destroy(ctx);
  pw_main_loop_destroy(loop);
  pw_deinit();
  return available;
#endif
}

ccord_video_stream *ccord_video_stream_create(const ccord_video_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  av_log_set_level(AV_LOG_ERROR);
  ccord_video_config c = {.width = 640, .height = 360, .fps = 30, .bitrate = 1500000, .codec = CCORD_VIDEO_CODEC_H264};
  if (cfg) {
    c = *cfg;
    if (c.width == 0) c.width = 640;
    if (c.height == 0) c.height = 360;
    if (c.fps == 0) c.fps = 30;
    if (c.bitrate == 0) c.bitrate = 1500000;
  }
  if (c.width < 16 || c.width > 3840 || c.height < 16 || c.height > 2160) {
    ccord_error_set(err, CCORD_ERR_INVALID, "video: dimensions out of range");
    return nullptr;
  }
  if (c.fps < 1 || c.fps > 120 || c.bitrate < 64000 || c.bitrate > 50000000) {
    ccord_error_set(err, CCORD_ERR_INVALID, "video: fps or bitrate out of range");
    return nullptr;
  }
  ccord_video_stream *v = ccord_calloc(1, sizeof *v);
  if (!v) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "video: out of memory");
    return nullptr;
  }
  v->width = (c.width + 1) & ~1;
  v->height = (c.height + 1) & ~1;
  v->fps = c.fps;
  v->bitrate = c.bitrate;
  v->use_pipewire = c.use_pipewire;
  v->lazy = &v->lazy_storage;
  bool opened = false;
  if (c.codec == CCORD_VIDEO_CODEC_H264) {
    opened = probe_h264_encoders(v, c.preferred_encoder);
  } else {
    opened = probe_av1_encoders(v, c.preferred_encoder);
    if (!opened) opened = probe_h264_encoders(v, c.preferred_encoder);
    if (!opened) opened = open_video_codec_pair(v, avcodec_find_encoder(AV_CODEC_ID_MPEG4));
  }
  if (!opened) {
    ccord_video_stream_destroy(v);
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "video: no H.264 or MPEG4 codec available");
    return nullptr;
  }
  v->enc_frame = av_frame_alloc();
  v->pkt = av_packet_alloc();
  if (!v->enc_frame || !v->pkt) {
    ccord_video_stream_destroy(v);
    ccord_error_set(err, CCORD_ERR_NOMEM, "video: cannot allocate AVFrame/AVPacket");
    return nullptr;
  }
  v->enc_frame->format = v->enc_ctx->pix_fmt;
  v->enc_frame->width = v->width;
  v->enc_frame->height = v->height;
  if (av_frame_get_buffer(v->enc_frame, 32) < 0) {
    ccord_video_stream_destroy(v);
    ccord_error_set(err, CCORD_ERR_NOMEM, "video: cannot allocate YUV frame buffer");
    return nullptr;
  }
  return v;
}

void ccord_video_stream_destroy(ccord_video_stream *v)
{
  if (!v) return;
  if (v->sws_to_yuv) sws_freeContext(v->sws_to_yuv);
  if (v->lazy) {
    if (v->lazy->sws_to_rgba) sws_freeContext(v->lazy->sws_to_rgba);
    if (v->lazy->sws_preview_rgba) sws_freeContext(v->lazy->sws_preview_rgba);
    ccord_free(v->lazy->last_rgba);
  }
  if (v->enc_frame) av_frame_free(&v->enc_frame);
  if (v->dec_frame) av_frame_free(&v->dec_frame);
  if (v->pkt) av_packet_free(&v->pkt);
  if (v->enc_ctx) avcodec_free_context(&v->enc_ctx);
  if (v->dec_ctx) avcodec_free_context(&v->dec_ctx);
  ccord_free(v);
}

const char *ccord_video_stream_codec_name(const ccord_video_stream *v)
{
  return (v && v->enc_codec && v->enc_codec->name) ? v->enc_codec->name : "";
}

int ccord_video_stream_width(const ccord_video_stream *v)
{
  return v ? v->width : 0;
}

int ccord_video_stream_height(const ccord_video_stream *v)
{
  return v ? v->height : 0;
}

int ccord_video_stream_fps(const ccord_video_stream *v)
{
  return v ? v->fps : 0;
}

bool ccord_video_stream_last_keyframe(const ccord_video_stream *v)
{
  return v ? v->last_keyframe : false;
}

void ccord_video_stream_force_keyframe(ccord_video_stream *v)
{
  if (v) v->force_keyframe = true;
}

bool ccord_video_stream_pipewire_available(const ccord_video_stream *v)
{
  if (!v || !v->use_pipewire || !v->lazy) return false;
  if (!v->lazy->pipewire_probed) {
    v->lazy->pipewire_probed = true;
    v->lazy->pipewire_available = probe_pipewire();
  }
  return v->lazy->pipewire_available;
}

static bool video_stream_ensure_rgba_buf(video_stream_lazy *lz, int w, int h)
{
  size_t need = (size_t)w * (size_t)h * 4u + PLATFORM_SWS_DST_PAD;
  if (!lz->last_rgba || need > lz->last_rgba_cap) {
    uint8_t *nbuf = ccord_malloc(need);
    if (!nbuf) return false;
    ccord_free(lz->last_rgba);
    lz->last_rgba = nbuf;
    lz->last_rgba_cap = need;
  }
  lz->last_w = w;
  lz->last_h = h;
  return true;
}

static void video_stream_update_preview(ccord_video_stream *v, const uint8_t *pixels, int width, int height,
                                        int src_stride, enum AVPixelFormat src_fmt)
{
  video_stream_lazy *lz = v->lazy;
  if (!video_stream_ensure_rgba_buf(lz, v->width, v->height)) return;
  if (width == v->width && height == v->height) {
    const size_t row_bytes = (size_t)v->width * 4u;
    if (src_fmt == AV_PIX_FMT_RGBA) {
      if ((size_t)src_stride == row_bytes) {
        memcpy(lz->last_rgba, pixels, row_bytes * (size_t)v->height);
      } else {
        for (int y = 0; y < v->height; y++) {
          memcpy(lz->last_rgba + (size_t)y * row_bytes, pixels + (size_t)y * (size_t)src_stride, row_bytes);
        }
      }
      lz->enc_preview_dirty = false;
      return;
    }
    if (src_fmt == AV_PIX_FMT_BGRA) {
      for (int y = 0; y < v->height; y++) {
        const uint8_t *restrict s = pixels + (size_t)y * (size_t)src_stride;
        uint8_t *restrict d = lz->last_rgba + (size_t)y * row_bytes;
        for (int x = 0; x < v->width; x++, s += 4, d += 4) {
          d[0] = s[2];
          d[1] = s[1];
          d[2] = s[0];
          d[3] = s[3];
        }
      }
      lz->enc_preview_dirty = false;
      return;
    }
  }
  lz->sws_preview_rgba = sws_getCachedContext(lz->sws_preview_rgba, width, height, src_fmt, v->width, v->height,
                                              AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (lz->sws_preview_rgba) {
    const uint8_t *src_slices[4] = {pixels, nullptr, nullptr, nullptr};
    int src_linesize[4] = {src_stride, 0, 0, 0};
    uint8_t *dst_slices[4] = {lz->last_rgba, nullptr, nullptr, nullptr};
    int dst_linesize[4] = {v->width * 4, 0, 0, 0};
    sws_scale(lz->sws_preview_rgba, src_slices, src_linesize, 0, height, dst_slices, dst_linesize);
    lz->enc_preview_dirty = false;
  }
}

static ccord_status encode_pixels_internal(ccord_video_stream *v, const uint8_t *pixels, int width, int height,
                                           int stride, enum AVPixelFormat src_fmt, uint8_t *out_pkt, size_t out_cap,
                                           size_t *out_len, ccord_error *err)
{
  ccord_error_clear(err);
  if (out_len) *out_len = 0;
  if (!v || !pixels || !out_pkt || !out_len || width <= 0 || height <= 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "video encode: invalid argument");
  }
  int src_stride = stride > 0 ? stride : width * 4;
  if (src_stride < width * 4) return ccord_error_set(err, CCORD_ERR_INVALID, "video encode: stride too small");
  if (av_frame_make_writable(v->enc_frame) < 0) {
    return ccord_error_set(err, CCORD_ERR_IO, "video encode: frame not writable");
  }
  v->sws_to_yuv = sws_getCachedContext(v->sws_to_yuv, width, height, src_fmt, v->width, v->height,
                                       v->enc_ctx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!v->sws_to_yuv) return ccord_error_set(err, CCORD_ERR_NOMEM, "video encode: sws_getCachedContext failed");
  const uint8_t *src_slices[4] = {pixels, nullptr, nullptr, nullptr};
  int src_linesize[4] = {src_stride, 0, 0, 0};
  sws_scale(v->sws_to_yuv, src_slices, src_linesize, 0, height, v->enc_frame->data, v->enc_frame->linesize);

  // Only allocate/update last_rgba when a caller actually reads the preview; otherwise defer lazily.
  if (v->lazy->preview_wanted) {
    video_stream_update_preview(v, pixels, width, height, src_stride, src_fmt);
  } else {
    v->lazy->enc_preview_dirty = true;
  }

  v->enc_frame->pts = v->pts++;
  if (v->force_keyframe) {
    v->enc_frame->pict_type = AV_PICTURE_TYPE_I;
    v->force_keyframe = false;
  } else {
    v->enc_frame->pict_type = AV_PICTURE_TYPE_NONE;
  }
  av_packet_unref(v->pkt);
  if (avcodec_send_frame(v->enc_ctx, v->enc_frame) < 0) {
    return ccord_error_set(err, CCORD_ERR_IO, "video encode: avcodec_send_frame failed");
  }
  if (avcodec_receive_packet(v->enc_ctx, v->pkt) < 0 || v->pkt->size <= 0) {
    return ccord_error_set(err, CCORD_ERR_IO, "video encode: avcodec_receive_packet failed");
  }
  v->last_keyframe = (v->pkt->flags & AV_PKT_FLAG_KEY) != 0;
  size_t pkt_sz = (size_t)v->pkt->size;
  if (pkt_sz > out_cap) {
    av_packet_unref(v->pkt);
    return ccord_error_set(err, CCORD_ERR_RANGE, "video encode: output buffer too small");
  }
  memcpy(out_pkt, v->pkt->data, pkt_sz);
  *out_len = pkt_sz;
  av_packet_unref(v->pkt);
  return CCORD_OK;
}

ccord_status ccord_video_stream_encode_rgba(ccord_video_stream *v, const uint8_t *rgba, int width, int height,
                                            int stride, uint8_t *out_pkt, size_t out_cap, size_t *out_len,
                                            ccord_error *err)
{
  return encode_pixels_internal(v, rgba, width, height, stride, AV_PIX_FMT_RGBA, out_pkt, out_cap, out_len, err);
}

ccord_status ccord_video_stream_encode_bgra(ccord_video_stream *v, const uint8_t *bgra, int width, int height,
                                            int stride, uint8_t *out_pkt, size_t out_cap, size_t *out_len,
                                            ccord_error *err)
{
  return encode_pixels_internal(v, bgra, width, height, stride, AV_PIX_FMT_BGRA, out_pkt, out_cap, out_len, err);
}

ccord_status ccord_video_stream_decode_rgba(ccord_video_stream *v, const uint8_t *pkt, size_t pkt_len,
                                            uint8_t **out_rgba, int *out_w, int *out_h, ccord_error *err)
{
  ccord_error_clear(err);
  if (out_rgba) *out_rgba = nullptr;
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (!v || !pkt || pkt_len == 0 || pkt_len > (size_t)INT_MAX) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "video decode: invalid argument");
  }
  ccord_status dec_st = ensure_video_decoder(v, err);
  if (dec_st != CCORD_OK) return dec_st;
  av_packet_unref(v->pkt);
  if (av_new_packet(v->pkt, (int)pkt_len) < 0) {
    return ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: av_new_packet failed");
  }
  memcpy(v->pkt->data, pkt, pkt_len);
  int send_rc = avcodec_send_packet(v->dec_ctx, v->pkt);
  av_packet_unref(v->pkt);
  if (send_rc < 0) return ccord_error_set(err, CCORD_ERR_PARSE, "video decode: invalid bitstream packet");
  if (avcodec_receive_frame(v->dec_ctx, v->dec_frame) < 0) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "video decode: no frame produced");
  }
  int dw = v->dec_frame->width;
  int dh = v->dec_frame->height;
  if (dw <= 0 || dh <= 0 || dw > 3840 || dh > 2160) {
    av_frame_unref(v->dec_frame);
    return ccord_error_set(err, CCORD_ERR_PARSE, "video decode: invalid decoded dimensions");
  }
  size_t bytes = (size_t)dw * (size_t)dh * 4u;
  video_stream_lazy *lz = v->lazy;
  if (!video_stream_ensure_rgba_buf(lz, dw, dh)) {
    av_frame_unref(v->dec_frame);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: out of memory");
  }
  lz->sws_to_rgba = sws_getCachedContext(lz->sws_to_rgba, dw, dh, (enum AVPixelFormat)v->dec_frame->format, dw, dh,
                                         AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!lz->sws_to_rgba) {
    av_frame_unref(v->dec_frame);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: sws_getCachedContext failed");
  }
  uint8_t *dst_slices[4] = {lz->last_rgba, nullptr, nullptr, nullptr};
  int dst_linesize[4] = {dw * 4, 0, 0, 0};
  sws_scale(lz->sws_to_rgba, (const uint8_t *const *)v->dec_frame->data, v->dec_frame->linesize, 0, dh, dst_slices,
            dst_linesize);
  av_frame_unref(v->dec_frame);
  lz->enc_preview_dirty = false;
  if (out_rgba) {
    uint8_t *copy = ccord_malloc(bytes);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: out of memory");
    memcpy(copy, lz->last_rgba, bytes);
    *out_rgba = copy;
  }
  if (out_w) *out_w = dw;
  if (out_h) *out_h = dh;
  return CCORD_OK;
}

ccord_status ccord_video_stream_step_test_frame(ccord_video_stream *v, ccord_error *err)
{
  ccord_error_clear(err);
  if (!v) return ccord_error_set(err, CCORD_ERR_INVALID, "video: null stream");
  size_t rgba_bytes = (size_t)v->width * (size_t)v->height * 4u;
  size_t pkt_cap = rgba_bytes + 65536u;
  uint8_t *src = ccord_malloc(rgba_bytes);
  uint8_t *pkt = ccord_malloc(pkt_cap);
  if (!src || !pkt) {
    ccord_free(src);
    ccord_free(pkt);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "video: out of memory");
  }
  uint32_t step = v->synth_step++;
  for (int y = 0; y < v->height; y++) {
    for (int x = 0; x < v->width; x++) {
      uint8_t *px = &src[((size_t)y * (size_t)v->width + (size_t)x) * 4u];
      px[0] = (uint8_t)((x * 255) / v->width);
      px[1] = (uint8_t)((y * 255) / v->height);
      px[2] = (uint8_t)((step * 17u) & 0xffu);
      px[3] = 255u;
    }
  }
  size_t pkt_len = 0;
  ccord_status st = ccord_video_stream_encode_rgba(v, src, v->width, v->height, v->width * 4, pkt, pkt_cap, &pkt_len, err);
  if (st == CCORD_OK) {
    st = ccord_video_stream_decode_rgba(v, pkt, pkt_len, nullptr, nullptr, nullptr, err);
  }
  ccord_free(src);
  ccord_free(pkt);
  return st;
}

const uint8_t *ccord_video_stream_last_rgba(const ccord_video_stream *v, int *out_w, int *out_h)
{
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (!v || !v->lazy) return nullptr;
  video_stream_lazy *lz = v->lazy;
  lz->preview_wanted = true;
  if (lz->enc_preview_dirty && v->enc_frame && v->enc_frame->data[0]) {
    if (video_stream_ensure_rgba_buf(lz, v->width, v->height)) {
      lz->sws_to_rgba = sws_getCachedContext(lz->sws_to_rgba, v->width, v->height,
                                             (enum AVPixelFormat)v->enc_frame->format, v->width, v->height,
                                             AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (lz->sws_to_rgba) {
        uint8_t *dst_slices[4] = {lz->last_rgba, nullptr, nullptr, nullptr};
        int dst_linesize[4] = {v->width * 4, 0, 0, 0};
        sws_scale(lz->sws_to_rgba, (const uint8_t *const *)v->enc_frame->data, v->enc_frame->linesize, 0, v->height,
                  dst_slices, dst_linesize);
        lz->enc_preview_dirty = false;
      }
    }
  }
  if (!lz->last_rgba || lz->last_w <= 0 || lz->last_h <= 0) return nullptr;
  if (out_w) *out_w = lz->last_w;
  if (out_h) *out_h = lz->last_h;
  return lz->last_rgba;
}

