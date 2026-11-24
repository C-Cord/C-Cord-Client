// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Animated image decoding (GIF / APNG / animated WebP) on top of libavformat + libavcodec + libswscale.
// The input is read through a memory AVIO context (no temp files, no protocols); only image demuxers are
// allowed so arbitrary media containers in untrusted attachments are never parsed.
#include <ccord/render/anim.h>
#include "lottie.h"

#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <ccord/core/alloc.h>
#include <ccord/core/os.h>

#define ANIM_MAX_INPUT ((size_t)32u * 1024u * 1024u)
#define ANIM_MAX_DIM 8192
#define ANIM_MAX_SRC_PIXELS ((int64_t)32 * 1024 * 1024)
#if CCORD_OS_ANDROID
#define ANIM_MAX_BYTES ((size_t)32u * 1024u * 1024u)
#else
#define ANIM_MAX_BYTES ((size_t)64u * 1024u * 1024u)
#endif
#define ANIM_IO_BUF 32768
#define ANIM_DEFAULT_DELAY_MS 100u

// libswscale's SIMD yuv->RGBA kernels store whole vectors for the last pixels of a row, so for widths that are
// not a multiple of 16 they write up to 32 bytes (measured: 32 at 200x150 yuv420p) past the end of a destination
// sized exactly w*h*4. That silently corrupts the next heap chunk and glibc aborts or faults later. Every ccord
// buffer handed to sws_scale as a destination is over-allocated by this tail; sizes and copies stay w*h*4.
#define ANIM_SWS_DST_PAD 128u

// ---- sniffing ----

static uint32_t rd_be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t rd_le32(const uint8_t *p)
{
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

bool ccord_anim_sniff(const uint8_t *data, size_t len)
{
  if (!data) return false;
  if (len >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0)) return true;
  if (len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
    size_t pos = 8;
    while (len - pos >= 8 && pos <= len) {
      uint32_t clen = rd_be32(data + pos);
      const uint8_t *type = data + pos + 4;
      if (memcmp(type, "acTL", 4) == 0) return true;
      if (memcmp(type, "IDAT", 4) == 0 || memcmp(type, "IEND", 4) == 0) return false;
      // chunk = length + type + data + crc
      uint64_t next = (uint64_t)pos + 12u + clen;
      if (next > len) return false;
      pos = (size_t)next;
    }
    return false;
  }
  if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
    size_t pos = 12;
    while (len - pos >= 8 && pos <= len) {
      const uint8_t *type = data + pos;
      uint32_t clen = rd_le32(data + pos + 4);
      if (memcmp(type, "ANIM", 4) == 0 || memcmp(type, "ANMF", 4) == 0) return true;
      if (memcmp(type, "VP8X", 4) == 0 && clen >= 1 && len - pos >= 9 && (data[pos + 8] & 0x02u)) return true;
      uint64_t next = (uint64_t)pos + 8u + clen + (clen & 1u);
      if (next > len) return false;
      pos = (size_t)next;
    }
    return false;
  }
  if (ccord_lottie_sniff(data, len)) return true;
  return false;
}

// ---- timing ----

size_t ccord_anim_frame_at(const ccord_anim *a, uint64_t t_ms)
{
  if (!a || a->frame_count <= 1 || a->total_ms == 0 || !a->delay_ms) return 0;
  uint64_t t = t_ms % a->total_ms;
  uint64_t acc = 0;
  for (size_t i = 0; i < a->frame_count; i++) {
    acc += a->delay_ms[i];
    if (t < acc) return i;
  }
  return a->frame_count - 1;
}

uint32_t ccord_anim_next_delay_ms(const ccord_anim *a, uint64_t t_ms)
{
  if (!a || a->frame_count <= 1 || a->total_ms == 0 || !a->delay_ms) return 0;
  uint64_t t = t_ms % a->total_ms;
  uint64_t acc = 0;
  for (size_t i = 0; i < a->frame_count; i++) {
    acc += a->delay_ms[i];
    if (t < acc) {
      uint64_t rem = acc - t;
      return rem > UINT32_MAX ? UINT32_MAX : (uint32_t)rem;
    }
  }
  return a->delay_ms[0] ? a->delay_ms[0] : 1u;
}

void ccord_anim_free(ccord_anim *a)
{
  if (!a) return;
  if (a->frames) {
    for (size_t i = 0; i < a->frame_count; i++) ccord_free(a->frames[i]);
  }
  ccord_free(a->frames);
  ccord_free(a->delay_ms);
  memset(a, 0, sizeof *a);
}

// ---- memory AVIO ----

typedef struct membuf {
  const uint8_t *p;
  size_t len, pos;
} membuf;

static int mb_read(void *opaque, uint8_t *buf, int n)
{
  membuf *m = opaque;
  if (n <= 0) return 0;
  if (m->pos >= m->len) return AVERROR_EOF;
  size_t left = m->len - m->pos;
  size_t take = (size_t)n < left ? (size_t)n : left;
  memcpy(buf, m->p + m->pos, take);
  m->pos += take;
  return (int)take;
}

static int64_t mb_seek(void *opaque, int64_t off, int whence)
{
  membuf *m = opaque;
  if (whence & AVSEEK_SIZE) return (int64_t)m->len;
  whence &= ~AVSEEK_FORCE;
  int64_t base;
  switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int64_t)m->pos; break;
    case SEEK_END: base = (int64_t)m->len; break;
    default: return AVERROR(EINVAL);
  }
  int64_t np = base + off;
  if (np < 0 || np > (int64_t)m->len) return AVERROR(EINVAL);
  m->pos = (size_t)np;
  return np;
}

// ---- decoder state ----

typedef struct anim_ctx {
  ccord_anim *out;
  size_t cap;
  size_t max_frames;
  int max_dim;
  size_t bytes;
  bool size_known;
  bool full;            // stop decoding (frame/byte cap reached)
  struct SwsContext *sws;
  ccord_status st;
  ccord_error *err;
  AVStream *stream;
} anim_ctx;

static uint32_t norm_delay(int64_t ms)
{
  if (ms <= 10) return ANIM_DEFAULT_DELAY_MS;   // browsers treat <= 10 ms as 100 ms
  if (ms < 20) return 20;
  if (ms > 3600 * 1000) return 3600u * 1000u;
  return (uint32_t)ms;
}

static void ctx_fail(anim_ctx *c, ccord_status st, const char *msg)
{
  if (c->st == CCORD_OK) c->st = ccord_error_set(c->err, st, "%s", msg);
}

static void add_frame(anim_ctx *c, const AVFrame *f)
{
  if (c->full || c->st != CCORD_OK) return;
  ccord_anim *o = c->out;
  if (f->width <= 0 || f->height <= 0 || f->width > ANIM_MAX_DIM || f->height > ANIM_MAX_DIM ||
      (int64_t)f->width * f->height > ANIM_MAX_SRC_PIXELS) {
    ctx_fail(c, CCORD_ERR_RANGE, "anim: dimensions out of range");
    return;
  }
  if (!c->size_known) {
    int dw = f->width, dh = f->height;
    int m = dw > dh ? dw : dh;
    if (c->max_dim > 0 && m > c->max_dim) {
      dw = (int)(((int64_t)dw * c->max_dim + m / 2) / m);
      dh = (int)(((int64_t)dh * c->max_dim + m / 2) / m);
      if (dw < 1) dw = 1;
      if (dh < 1) dh = 1;
    }
    o->w = dw;
    o->h = dh;
    c->size_known = true;
  }
  size_t fbytes = (size_t)o->w * (size_t)o->h * 4u;
  if (c->bytes + fbytes > ANIM_MAX_BYTES) {
    if (o->frame_count == 0) ctx_fail(c, CCORD_ERR_RANGE, "anim: image too large");
    else c->full = true;
    return;
  }
  if (o->frame_count == c->cap) {
    size_t ncap = c->cap ? c->cap * 2u : ((c->max_frames > 0 && c->max_frames < 8u) ? c->max_frames : 8u);
    uint8_t **nf = ccord_realloc_array(o->frames, ncap, sizeof *nf);
    if (!nf) {
      ctx_fail(c, CCORD_ERR_NOMEM, "anim: out of memory");
      return;
    }
    o->frames = nf;
    uint32_t *nd = ccord_realloc_array(o->delay_ms, ncap, sizeof *nd);
    if (!nd) {
      ctx_fail(c, CCORD_ERR_NOMEM, "anim: out of memory");
      return;
    }
    o->delay_ms = nd;
    c->cap = ncap;
  }
  const int row_bytes = o->w * 4;
  const bool direct_rgba = (enum AVPixelFormat)f->format == AV_PIX_FMT_RGBA && f->width == o->w &&
                           f->height == o->h && f->data[0] && f->linesize[0] >= row_bytes;
  uint8_t *px = ccord_malloc(direct_rgba ? fbytes : fbytes + ANIM_SWS_DST_PAD);
  if (!px) {
    ctx_fail(c, CCORD_ERR_NOMEM, "anim: out of memory");
    return;
  }
  if (direct_rgba) {
    if (f->linesize[0] == row_bytes) {
      memcpy(px, f->data[0], fbytes);
    } else {
      for (int y = 0; y < o->h; y++) {
        memcpy(px + (size_t)y * (size_t)row_bytes, f->data[0] + (size_t)y * (size_t)f->linesize[0], (size_t)row_bytes);
      }
    }
  } else {
    int flags = (o->w < f->width || o->h < f->height) ? SWS_AREA : SWS_BILINEAR;
    c->sws = sws_getCachedContext(c->sws, f->width, f->height, (enum AVPixelFormat)f->format, o->w, o->h,
                                  AV_PIX_FMT_RGBA, flags, nullptr, nullptr, nullptr);
    if (!c->sws) {
      ccord_free(px);
      ctx_fail(c, CCORD_ERR_UNSUPPORTED, "anim: unsupported pixel format");
      return;
    }
    uint8_t *dst[4] = {px, nullptr, nullptr, nullptr};
    int dst_ls[4] = {row_bytes, 0, 0, 0};
    int rows = sws_scale(c->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dst_ls);
    if (rows != o->h) {
      ccord_free(px);
      ctx_fail(c, CCORD_ERR_PARSE, "anim: frame conversion failed");
      return;
    }
  }
  int64_t dur = 0;
  if (f->duration > 0 && c->stream) {
    dur = av_rescale_q(f->duration, c->stream->time_base, (AVRational){1, 1000});
  }
  o->frames[o->frame_count] = px;
  o->delay_ms[o->frame_count] = norm_delay(dur);
  o->frame_count++;
  c->bytes += fbytes;
  if (c->max_frames && o->frame_count >= c->max_frames) c->full = true;
}

static void drain(anim_ctx *c, AVCodecContext *dec, AVFrame *frame)
{
  while (!c->full && c->st == CCORD_OK) {
    int r = avcodec_receive_frame(dec, frame);
    if (r < 0) return;
    add_frame(c, frame);
    av_frame_unref(frame);
  }
}

ccord_status ccord_anim_decode(const uint8_t *data, size_t len, int max_dim, size_t max_frames, ccord_anim *out,
                               ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "anim: invalid argument");
  memset(out, 0, sizeof *out);
  if (!data || len < 4) return ccord_error_set(err, CCORD_ERR_INVALID, "anim: invalid argument");
  if (len > ANIM_MAX_INPUT) return ccord_error_set(err, CCORD_ERR_RANGE, "anim: input too large");

  if (ccord_lottie_sniff(data, len)) {
    ccord_lottie_stream *ls = ccord_lottie_open(data, len, max_dim, err);
    if (!ls) return err && err->code != CCORD_OK ? err->code : CCORD_ERR_PARSE;
    int lw = 0, lh = 0;
    ccord_lottie_info(ls, &lw, &lh, nullptr);
    out->w = lw;
    out->h = lh;
    size_t fbytes = (size_t)lw * (size_t)lh * 4u;
    size_t cap = 0, used = 0;
    const uint8_t *px = nullptr;
    uint32_t pts = 0, dur = 0;
    while ((!max_frames || out->frame_count < max_frames) && ccord_lottie_next_frame(ls, &px, &pts, &dur)) {
      if (used + fbytes > ANIM_MAX_BYTES) {
        if (out->frame_count == 0) {
          ccord_lottie_close(ls);
          ccord_anim_free(out);
          return ccord_error_set(err, CCORD_ERR_RANGE, "anim: lottie frame too large");
        }
        break;
      }
      if (out->frame_count == cap) {
        size_t ncap = cap ? cap * 2u : 16u;
        uint8_t **nf = ccord_realloc_array(out->frames, ncap, sizeof *nf);
        uint32_t *nd = nf ? ccord_realloc_array(out->delay_ms, ncap, sizeof *nd) : nullptr;
        if (!nf || !nd) {
          if (nf) out->frames = nf;
          ccord_lottie_close(ls);
          ccord_anim_free(out);
          return ccord_error_set(err, CCORD_ERR_NOMEM, "anim: out of memory");
        }
        out->frames = nf;
        out->delay_ms = nd;
        cap = ncap;
      }
      uint8_t *copy = ccord_malloc(fbytes);
      if (!copy) {
        ccord_lottie_close(ls);
        ccord_anim_free(out);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "anim: out of memory");
      }
      memcpy(copy, px, fbytes);
      uint32_t d = norm_delay(dur);
      out->frames[out->frame_count] = copy;
      out->delay_ms[out->frame_count] = d;
      out->frame_count++;
      uint64_t tot = (uint64_t)out->total_ms + d;
      out->total_ms = tot > UINT32_MAX ? UINT32_MAX : (uint32_t)tot;
      used += fbytes;
    }
    ccord_lottie_close(ls);
    if (out->frame_count == 0) {
      ccord_anim_free(out);
      return ccord_error_set(err, CCORD_ERR_PARSE, "anim: lottie produced no frames");
    }
    return CCORD_OK;
  }

  if (av_log_get_level() > AV_LOG_ERROR) av_log_set_level(AV_LOG_ERROR);   // same level the platform uses
  membuf mb = {.p = data, .len = len, .pos = 0};
  AVFormatContext *fmt = nullptr;
  AVIOContext *io = nullptr;
  AVCodecContext *dec = nullptr;
  AVFrame *frame = nullptr;
  AVPacket *pkt = nullptr;
  AVDictionary *opts = nullptr;
  const AVCodec *codec = nullptr;
  int si = -1;
  anim_ctx c = {.out = out, .max_frames = max_frames, .max_dim = max_dim, .st = CCORD_OK, .err = err};

  uint8_t *iobuf = av_malloc(ANIM_IO_BUF);
  if (!iobuf) return ccord_error_set(err, CCORD_ERR_NOMEM, "anim: out of memory");
  io = avio_alloc_context(iobuf, ANIM_IO_BUF, 0, &mb, mb_read, nullptr, mb_seek);
  if (!io) {
    av_free(iobuf);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "anim: out of memory");
  }
  fmt = avformat_alloc_context();
  if (!fmt) {
    ctx_fail(&c, CCORD_ERR_NOMEM, "anim: out of memory");
    goto done;
  }
  fmt->max_streams = 16;
  fmt->pb = io;
  fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  av_dict_set(&opts, "format_whitelist", "gif,apng,webp_anim,webp_pipe,png_pipe,jpeg_pipe,bmp_pipe,tiff_pipe", 0);
  av_dict_set(&opts, "protocol_whitelist", "", 0);
  if (avformat_open_input(&fmt, "", nullptr, &opts) < 0) {   // frees fmt on failure
    fmt = nullptr;
    ctx_fail(&c, CCORD_ERR_UNSUPPORTED, "anim: unsupported or corrupt image");
    goto done;
  }
  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    ctx_fail(&c, CCORD_ERR_PARSE, "anim: cannot read image info");
    goto done;
  }
  si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if (si < 0 || !codec) {
    ctx_fail(&c, CCORD_ERR_UNSUPPORTED, "anim: no image stream");
    goto done;
  }
  c.stream = fmt->streams[si];
  for (unsigned i = 0; i < fmt->nb_streams; i++) {
    if ((int)i != si) fmt->streams[i]->discard = AVDISCARD_ALL;
  }
  if (c.stream->codecpar->width > ANIM_MAX_DIM || c.stream->codecpar->height > ANIM_MAX_DIM ||
      (int64_t)c.stream->codecpar->width * c.stream->codecpar->height > ANIM_MAX_SRC_PIXELS) {
    ctx_fail(&c, CCORD_ERR_RANGE, "anim: dimensions out of range");
    goto done;
  }
  dec = avcodec_alloc_context3(codec);
  if (!dec || avcodec_parameters_to_context(dec, c.stream->codecpar) < 0) {
    ctx_fail(&c, CCORD_ERR_NOMEM, "anim: cannot create decoder");
    goto done;
  }
  dec->pkt_timebase = c.stream->time_base;
  dec->thread_count = 1;
  if (avcodec_open2(dec, codec, nullptr) < 0) {
    ctx_fail(&c, CCORD_ERR_UNSUPPORTED, "anim: cannot open decoder");
    goto done;
  }
  frame = av_frame_alloc();
  pkt = av_packet_alloc();
  if (!frame || !pkt) {
    ctx_fail(&c, CCORD_ERR_NOMEM, "anim: out of memory");
    goto done;
  }
  while (!c.full && c.st == CCORD_OK && av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == si && avcodec_send_packet(dec, pkt) >= 0) drain(&c, dec, frame);
    av_packet_unref(pkt);
  }
  if (!c.full && c.st == CCORD_OK) {
    if (avcodec_send_packet(dec, nullptr) >= 0) drain(&c, dec, frame);
  }
  if (c.st == CCORD_OK && out->frame_count == 0) ctx_fail(&c, CCORD_ERR_PARSE, "anim: no decodable frames");

done:
  av_dict_free(&opts);
  sws_freeContext(c.sws);
  av_packet_free(&pkt);
  av_frame_free(&frame);
  avcodec_free_context(&dec);
  if (fmt) avformat_close_input(&fmt);
  if (io) {
    av_freep(&io->buffer);
    avio_context_free(&io);
  }
  if (c.st != CCORD_OK) {
    ccord_anim_free(out);
    return c.st;
  }
  uint64_t total = 0;
  for (size_t i = 0; i < out->frame_count; i++) total += out->delay_ms[i];
  out->total_ms = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
  return CCORD_OK;
}
