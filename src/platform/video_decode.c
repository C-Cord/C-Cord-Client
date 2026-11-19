// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Standalone H.264 Annex-B decoder -> RGBA32 (Go Live receive, camera preview).
// libavcodec parses SPS/PPS and the VCL NAL units; libswscale converts the decoded
// planar frame to tightly packed RGBA. Only the newest decoded frame is retained.
#define _POSIX_C_SOURCE 200809L
#include <ccord/platform/platform.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include <ccord/core/alloc.h>
#include <ccord/core/error.h>

#define VIDEO_DECODE_MAX_W 3840
#define VIDEO_DECODE_MAX_H 2160

// libswscale's SIMD yuv->RGBA kernels store whole vectors for the last pixels of a row, so for widths that are
// not a multiple of 16 they write up to 32 bytes (measured: 32 at 200x150 yuv420p) past the end of a destination
// sized exactly w*h*4. That silently corrupts the next heap chunk and glibc aborts or faults later. Every ccord
// buffer handed to sws_scale as a destination is over-allocated by this tail; sizes and copies stay w*h*4.
#define VIDEO_DECODE_SWS_DST_PAD 128u

// Lazily materialized RGBA output cache (accessed via non-const pointer from const decoder handle).
typedef struct video_decode_rgba {
  struct SwsContext *sws;
  uint8_t *last_rgba;
  size_t last_cap;
  int last_w;
  int last_h;
  bool rgba_dirty;
  bool have_frame;
} video_decode_rgba;

struct ccord_video_decoder {
  AVCodecContext *ctx;
  AVFrame *frame;         // scratch frame for avcodec_receive_frame
  AVFrame *latest;        // newest valid decoded picture, converted lazily on last_rgba()
  AVPacket *pkt;          // persistent ref-counted packet buffer reused across feed() calls
  size_t pkt_cap;
  int max_w;  // output clamp (<= 0: native); scale down only
  int max_h;
  bool have_idr;
  video_decode_rgba *out;
  video_decode_rgba out_storage;
};

// Scan an Annex-B access unit for start codes, IDR/non-IDR slices, and leading SPS/PPS parameter sets.
static bool inspect_annexb(const uint8_t *p, size_t len, bool *has_idr, bool *has_param_set, size_t *param_prefix_len)
{
  bool found_sc = false;
  *has_idr = false;
  *has_param_set = false;
  *param_prefix_len = len;
  bool saw_non_idr_slice = false;
  size_t i = 0;
  while (i + 3u < len) {
    if (p[i] == 0 && p[i + 1u] == 0 && p[i + 2u] == 1u) {
      found_sc = true;
      size_t sc_pos = (i > 0 && p[i - 1u] == 0) ? (i - 1u) : i;
      uint8_t nal_type = p[i + 3u] & 0x1Fu;
      if (nal_type == 5u) {
        *has_idr = true;
      } else if (nal_type == 7u || nal_type == 8u) {
        if (!saw_non_idr_slice) *has_param_set = true;
      } else if (nal_type >= 1u && nal_type <= 4u) {
        if (!saw_non_idr_slice) {
          saw_non_idr_slice = true;
          *param_prefix_len = sc_pos;
        }
      }
      i += 4u;
    } else {
      i++;
    }
  }
  if (!found_sc && len >= 3u && p[len - 3u] == 0 && p[len - 2u] == 0 && p[len - 1u] == 1u) {
    found_sc = true;
  }
  return found_sc;
}

// Output size for a w x h source under the clamp: never larger than the source or the clamp on either axis, aspect
// ratio kept, rounded down to even (min 2). A clamp axis <= 0 is unbounded; with no clamp the size is native.
static void clamp_output_dims(const ccord_video_decoder *d, int w, int h, int *out_w, int *out_h)
{
  *out_w = w;
  *out_h = h;
  bool over_w = d->max_w > 0 && w > d->max_w;
  bool over_h = d->max_h > 0 && h > d->max_h;
  if (!over_w && !over_h) return;
  // Scale factor = min(max_w / w, max_h / h) over the bounded axes, done in 64-bit integers.
  int64_t num = 1;
  int64_t den = 1;
  if (over_w) {
    num = d->max_w;
    den = w;
  }
  if (over_h && (!over_w || (int64_t)d->max_h * den < num * h)) {
    num = d->max_h;
    den = h;
  }
  int64_t ow = (int64_t)w * num / den;
  int64_t oh = (int64_t)h * num / den;
  ow &= ~(int64_t)1;
  oh &= ~(int64_t)1;
  if (ow < 2) ow = 2;
  if (oh < 2) oh = 2;
  *out_w = (int)ow;
  *out_h = (int)oh;
}

// Prepares d->pkt with a copy of [src, src+len) plus zeroed AV_INPUT_BUFFER_PADDING_SIZE tail,
// reusing the existing writable AVBufferRef whenever it fits (zero per-AU heap allocs in steady state).
static bool prepare_pkt(ccord_video_decoder *d, const uint8_t *src, size_t len)
{
  const size_t max_len = (size_t)INT_MAX - (size_t)AV_INPUT_BUFFER_PADDING_SIZE;
  if (len > max_len) return false;
  if (!d->pkt->buf || !av_buffer_is_writable(d->pkt->buf) || d->pkt_cap < len) {
    size_t new_cap = d->pkt_cap ? d->pkt_cap : 4096u;
    while (new_cap < len) {
      if (new_cap > max_len / 2u) {
        new_cap = len;
        break;
      }
      new_cap *= 2u;
    }
    av_packet_unref(d->pkt);
    if (av_new_packet(d->pkt, (int)new_cap) < 0) {
      d->pkt_cap = 0;
      return false;
    }
    d->pkt_cap = new_cap;
  }
  d->pkt->data = d->pkt->buf->data;
  d->pkt->size = (int)len;
  memcpy(d->pkt->data, src, len);
  memset(d->pkt->data + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
  return true;
}

static bool materialize_rgba(const ccord_video_decoder *d)
{
  video_decode_rgba *o = d->out;
  if (!o->rgba_dirty) return o->last_rgba != nullptr;
  if (!o->have_frame || !d->latest || !d->latest->data[0]) return false;
  int src_w = d->latest->width;
  int src_h = d->latest->height;
  int w = o->last_w;
  int h = o->last_h;
  if (src_w <= 0 || src_h <= 0 || w <= 0 || h <= 0) return false;
  size_t need = (size_t)w * (size_t)h * 4u + VIDEO_DECODE_SWS_DST_PAD;
  if (!o->last_rgba || need > o->last_cap) {
    uint8_t *nbuf = ccord_malloc(need);
    if (!nbuf) return false;
    ccord_free(o->last_rgba);
    o->last_rgba = nbuf;
    o->last_cap = need;
  }
  o->sws = sws_getCachedContext(o->sws, src_w, src_h, (enum AVPixelFormat)d->latest->format, w, h, AV_PIX_FMT_RGBA,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!o->sws) return false;
  uint8_t *dst_slices[1] = {o->last_rgba};
  int dst_linesize[1] = {w * 4};
  sws_scale(o->sws, (const uint8_t *const *)d->latest->data, d->latest->linesize, 0, src_h, dst_slices, dst_linesize);
  av_frame_unref(d->latest);
  o->rgba_dirty = false;
  return true;
}

ccord_video_decoder *ccord_video_decoder_create(ccord_error *err)
{
  ccord_error_clear(err);
  const AVCodec *codec = nullptr;
#if defined(__ANDROID__)
  codec = avcodec_find_decoder_by_name("h264_mediacodec");
#endif
  if (!codec) codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "video decode: no H.264 decoder");
    return nullptr;
  }
  ccord_video_decoder *d = ccord_calloc(1, sizeof *d);
  if (!d) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: out of memory");
    return nullptr;
  }
  d->out = &d->out_storage;
  d->ctx = avcodec_alloc_context3(codec);
  d->frame = av_frame_alloc();
  d->latest = av_frame_alloc();
  d->pkt = av_packet_alloc();
  if (!d->ctx || !d->frame || !d->latest || !d->pkt) {
    ccord_video_decoder_destroy(d);
    ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: out of memory");
    return nullptr;
  }
  d->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
  d->ctx->thread_count = 1;
  if (avcodec_open2(d->ctx, codec, nullptr) < 0) {
    ccord_video_decoder_destroy(d);
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "video decode: avcodec_open2 failed");
    return nullptr;
  }
  return d;
}

ccord_status ccord_video_decoder_feed(ccord_video_decoder *d, const uint8_t *annexb, size_t len, ccord_error *err)
{
  ccord_error_clear(err);
  if (!d || !annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "video decode: invalid argument");
  if (len > (size_t)INT_MAX - (size_t)AV_INPUT_BUFFER_PADDING_SIZE) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "video decode: access unit too large");
  }
  bool has_idr = false;
  bool has_param_set = false;
  size_t param_prefix_len = len;
  if (!inspect_annexb(annexb, len, &has_idr, &has_param_set, &param_prefix_len)) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "video decode: missing Annex-B start code");
  }
  if (!d->have_idr && !has_idr) {
    // Cache leading SPS/PPS parameter sets without feeding pre-IDR P/B-slices, which would cause
    // libavcodec to synthesize a solid grey YUV(128,128,128) error-concealment reference frame.
    if (has_param_set && param_prefix_len > 0) {
      if (prepare_pkt(d, annexb, param_prefix_len)) {
        (void)avcodec_send_packet(d->ctx, d->pkt);
        while (avcodec_receive_frame(d->ctx, d->frame) >= 0) av_frame_unref(d->frame);
      }
    }
    return ccord_error_set(err, CCORD_ERR_PARSE, "video decode: waiting for IDR keyframe");
  }
  if (!prepare_pkt(d, annexb, len)) return ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: out of memory");
  int send_rc = avcodec_send_packet(d->ctx, d->pkt);
  if (send_rc < 0) {
    d->have_idr = false;
    return ccord_error_set(err, CCORD_ERR_PARSE, "video decode: invalid bitstream packet");
  }

  bool got_frame = false;
  for (;;) {
    int rc = avcodec_receive_frame(d->ctx, d->frame);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
    if (rc < 0) {
      d->have_idr = false;
      return ccord_error_set(err, CCORD_ERR_PARSE, "video decode: corrupt frame");
    }
    if ((d->frame->flags & AV_FRAME_FLAG_CORRUPT) != 0 || d->frame->decode_error_flags != 0) {
      av_frame_unref(d->frame);
      d->have_idr = false;
      avcodec_flush_buffers(d->ctx);
      return ccord_error_set(err, CCORD_ERR_PARSE, "video decode: corrupt decoded picture");
    }
    int src_w = d->frame->width;
    int src_h = d->frame->height;
    if (src_w <= 0 || src_h <= 0 || src_w > VIDEO_DECODE_MAX_W || src_h > VIDEO_DECODE_MAX_H) {
      av_frame_unref(d->frame);
      return ccord_error_set(err, CCORD_ERR_PARSE, "video decode: invalid decoded dimensions");
    }
    int w = src_w;
    int h = src_h;
    clamp_output_dims(d, src_w, src_h, &w, &h);
    // Retain only the newest decoded YUV frame; defer sws_scale until last_rgba() is requested.
    av_frame_unref(d->latest);
    av_frame_move_ref(d->latest, d->frame);
    d->out->last_w = w;
    d->out->last_h = h;
    d->out->have_frame = true;
    d->out->rgba_dirty = true;
    d->have_idr = true;
    got_frame = true;
  }
  if (!got_frame) return ccord_error_set(err, CCORD_ERR_PARSE, "video decode: no frame produced");
  return CCORD_OK;
}

void ccord_video_decoder_set_max_dim(ccord_video_decoder *d, int max_w, int max_h)
{
  if (!d) return;
  if (max_w < 0) max_w = 0;
  if (max_h < 0) max_h = 0;
  if (d->max_w == max_w && d->max_h == max_h) return;
  d->max_w = max_w;
  d->max_h = max_h;
  // Next decoded frame computes clamped dimensions and sws_getCachedContext updates the scaler.
}

const uint8_t *ccord_video_decoder_last_rgba(const ccord_video_decoder *d, int *out_w, int *out_h)
{
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (!d || !d->out || !d->out->have_frame) return nullptr;
  if (!materialize_rgba(d)) return nullptr;
  if (out_w) *out_w = d->out->last_w;
  if (out_h) *out_h = d->out->last_h;
  return d->out->last_rgba;
}

void ccord_video_decoder_destroy(ccord_video_decoder *d)
{
  if (!d) return;
  if (d->out) {
    sws_freeContext(d->out->sws);
    ccord_free(d->out->last_rgba);
  }
  av_packet_free(&d->pkt);
  av_frame_free(&d->latest);
  av_frame_free(&d->frame);
  avcodec_free_context(&d->ctx);
  ccord_free(d);
}
