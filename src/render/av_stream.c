// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Streaming decoder on libavformat + libavcodec + libswscale. See av_stream.h.
// Strictly byte-source-bound: AVFMT_FLAG_CUSTOM_IO, an empty protocol whitelist and a demuxer whitelist.
//
// Two modes. Memory mode (ccord_av_stream_open) demuxes and decodes on the caller's thread from a private copy of
// the file. Source mode (ccord_av_stream_open_source) runs probing and demuxing on a demux thread that reads through
// ccord_av_source (which may block); the caller's ("UI") thread only decodes. Ownership in source mode:
//   - demux thread: fmt, io, pkt, vsi/asi, and everything avs_setup creates, until READY is published under mx;
//     afterwards only fmt/io/pkt (av_read_frame / av_seek_frame), never the codec contexts;
//   - UI thread: codec contexts, frame, sws, the RGBA/PCM buffers and the playhead, after it has seen READY;
//   - both, under mx: vq, aq, demux_eof, gen, seek_req, seek_ts, state, serr;
//   - meta is written by the demux thread before READY is published and is read-only afterwards. The UI thread
//     never dereferences fmt, io or an AVStream once the thread runs (nothing outside the demux-side helpers
//     avs_setup, avs_read_one, avs_seek_demuxer and avs_teardown touches them).
#include "render_internal.h"
#include "lottie.h"

#include <limits.h>
#include <math.h>
#include <ccord/core/os.h>
#include <stdatomic.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/cpu.h>
#include <libavutil/dict.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>

#define AVS_MAX_DIM 8192
#define AVS_MAX_SRC_PIXELS ((int64_t)32 * 1024 * 1024)
#define AVS_IO_BUF 32768
#define AVS_AUDIO_LEAD_MS 400u        // decode audio this far past the playhead
#define AVS_AUDIO_READY_MS 120u       // ... and call it caught up with this much in hand
#define AVS_STALE_MS 100u             // frames older than this while catching up are decoded but not converted
#define AVS_MAX_PCM_FRAMES ((size_t)CCORD_AV_SAMPLE_RATE * 8u)   // FIFO ceiling (badly interleaved files)
#define AVS_VQ_MAX_BYTES ((size_t)3u * 1024u * 1024u)    // source mode: the demux thread waits above this many queued video bytes
#define AVS_VQ_HARD_BYTES ((size_t)8u * 1024u * 1024u)   // ... but reads on past it while audio is starved (badly interleaved files)
#define AVS_AQ_MAX_BYTES ((size_t)1u * 1024u * 1024u)    // audio queue: oldest packets dropped (waits instead for audio-only files)

// libswscale's SIMD yuv->RGBA kernels store whole vectors for the last pixels of a row, so for widths that are
// not a multiple of 16 they write up to 32 bytes (measured: 32 at 200x150 yuv420p) past the end of a destination
// sized exactly w*h*4. That silently corrupts the next heap chunk and glibc aborts or faults later. Every ccord
// buffer handed to sws_scale as a destination is over-allocated by this tail; sizes and copies stay w*h*4.
#define AVS_SWS_DST_PAD 128u

typedef struct avs_membuf {
  const uint8_t *p;
  size_t len, pos;
} avs_membuf;

typedef struct avs_pktq {
  AVPacket **v;
  size_t head, count, cap;
  size_t bytes;   // sum of the queued packets' sizes
  bool dead;      // source mode: the consumer is finished with this stream; the demux thread drops its packets
} avs_pktq;

// Stream parameters the UI thread needs, copied out of AVFormatContext/AVStream by avs_setup (see the header comment).
typedef struct avs_meta {
  AVRational v_tb, v_avg_fr, a_tb;
  int64_t v_start, a_start;
  int64_t fmt_start_time;
  uint32_t duration_ms;
  bool has_video, has_audio;
} avs_meta;

struct ccord_av_stream {
  ccord_lottie_stream *lottie;
  uint8_t *data;
  avs_membuf mb;
  unsigned flags;
  int max_dim;
  AVFormatContext *fmt;
  AVIOContext *io;
  AVCodecContext *vdec, *vdec_alpha, *adec;
  int vsi, asi;
  AVFrame *frame;
  AVFrame *alpha_frame;
  AVPacket *pkt;
  AVPacket *alpha_pkt;
  avs_pktq vq, aq;
  struct SwsContext *sws;
  struct SwsContext *sws_alpha;
  uint8_t *alpha_scaled;   // sw*sh + AVS_SWS_DST_PAD, only when alpha plane needs separate scaling
  bool have_alpha_frame;
  SwrContext *swr;
  int swr_fmt, swr_rate, swr_ch;
  enum AVChannelOrder swr_order;
  uint64_t swr_mask;
  int16_t *swr_tmp;
  size_t swr_tmp_cap;      // capacity of swr_tmp in stereo frames
  uint64_t swr_skip_frames;
  bool last_frame_opaque;  // true when last converted video frame had no alpha channel (A == 255)
  bool demux_eof, v_flushed, a_flushed, v_done, a_done;
  ccord_av_info info;
  int rot;                 // clockwise quarter turns to apply for display (0..3)
  int sw, sh;              // scaled size before rotation
  bool size_known;
  uint8_t *scratch;        // sw*sh*4, only when rot != 0
  uint8_t *cur, *next;     // info.w*info.h*4 each
  bool have_cur, have_next;
  uint32_t cur_pts, next_pts, next_dur;
  uint32_t fallback_pts;   // pts for frames that carry none
  uint32_t serial;
  uint32_t skip_before_ms; // after a seek: video ending before this is not shown, audio before it is dropped
  int16_t *pcm;            // FIFO of interleaved stereo frames
  size_t pcm_head, pcm_len, pcm_cap;
  uint32_t pcm_pos_ms;     // timeline position of pcm[pcm_head]
  uint64_t pcm_pos_rem;    // samples popped since pcm_pos_ms was last a whole ms boundary
  uint32_t a_tail_ms;      // timeline position just past the last buffered sample
  avs_meta meta;
  // ---- source mode ----
  ccord_av_stream *self;   // lets const accessors reach the UI-only fields without a cast
  bool src_mode;
  ccord_av_source src;
  int64_t src_pos;         // demux thread: AVIO position
  ccord_status src_err;    // demux thread: last failure reported by read_at (0 = none)
  ccord_thread *thr;
  bool thr_started, sync_init;
  ccord_mutex *mx;
  ccord_cond *cv;
  atomic_bool stop;
  ccord_av_state state;    // mx
  ccord_error serr;        // mx, valid when state == FAILED
  bool seek_req;           // mx
  int64_t seek_ts;         // mx, AV_TIME_BASE units
  uint64_t gen;            // mx, bumped by every seek; the demux thread drops packets read under an older value
  bool observed;           // UI: the READY snapshot has been copied into info
  bool starved;            // UI: the last sync/seek needed packets the demux thread has not produced
  bool pre_seek;           // UI: a seek requested before READY, applied when READY is first seen
  uint32_t pre_seek_ms;
  uint64_t last_used_ms;   // monotonic clock (ccord_clock_ms) of last open/decode/sync/seek/draw
};

// ---- memory AVIO ----

static int avs_read(void *opaque, uint8_t *buf, int n)
{
  avs_membuf *m = opaque;
  if (n <= 0) return 0;
  if (m->pos >= m->len) return AVERROR_EOF;
  size_t left = m->len - m->pos;
  size_t take = (size_t)n < left ? (size_t)n : left;
  memcpy(buf, m->p + m->pos, take);
  m->pos += take;
  return (int)take;
}

static int64_t avs_seek(void *opaque, int64_t off, int whence)
{
  avs_membuf *m = opaque;
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

// ---- source AVIO (demux thread) ----

static int avs_src_read(void *opaque, uint8_t *buf, int n)
{
  ccord_av_stream *s = opaque;
  if (n <= 0) return 0;
  if (atomic_load(&s->stop)) {
    s->src_err = CCORD_ERR_IO;
    return AVERROR(EIO);
  }
  int64_t size = s->src.size ? s->src.size(s->src.ud) : -1;
  if (size >= 0) {   // never ask for bytes beyond the end of the data
    if (s->src_pos >= size) return AVERROR_EOF;
    if ((int64_t)n > size - s->src_pos) n = (int)(size - s->src_pos);
  }
  int r = s->src.read_at(s->src.ud, s->src_pos, buf, n);
  if (r > n) r = -(int)CCORD_ERR_INTERNAL;   // a source that claims more than it was given
  if (r < 0) {
    s->src_err = (ccord_status)(-r);
    return AVERROR(EIO);
  }
  if (r == 0) return AVERROR_EOF;
  s->src_pos += r;
  return r;
}

static int64_t avs_src_seek(void *opaque, int64_t off, int whence)
{
  ccord_av_stream *s = opaque;
  int64_t size = s->src.size ? s->src.size(s->src.ud) : -1;
  if (whence & AVSEEK_SIZE) return size >= 0 ? size : AVERROR(ENOSYS);
  whence &= ~AVSEEK_FORCE;
  int64_t base;
  switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = s->src_pos; break;
    case SEEK_END:
      if (size < 0) return AVERROR(EINVAL);
      base = size;
      break;
    default: return AVERROR(EINVAL);
  }
  int64_t np = base + off;
  if (np < 0 || (size >= 0 && np > size)) return AVERROR(EINVAL);
  s->src_pos = np;
  return np;
}

static int avs_interrupt(void *opaque)
{
  const ccord_av_stream *s = opaque;
  return atomic_load(&s->stop) ? 1 : 0;
}

// ---- packet queues ----

static bool pktq_push(avs_pktq *q, AVPacket *src)
{
  if (q->count == q->cap) {
    size_t ncap = q->cap ? q->cap * 2u : 64u;
    AVPacket **nv = ccord_alloc_array(ncap, sizeof *nv);
    if (!nv) return false;
    for (size_t i = 0; i < q->count; i++) nv[i] = q->v[(q->head + i) % q->cap];
    ccord_free(q->v);
    q->v = nv;
    q->head = 0;
    q->cap = ncap;
  }
  AVPacket *p = av_packet_alloc();
  if (!p) return false;
  av_packet_move_ref(p, src);
  q->v[(q->head + q->count) % q->cap] = p;
  q->count++;
  q->bytes += (size_t)p->size;
  return true;
}

static AVPacket *pktq_pop(avs_pktq *q)
{
  if (q->count == 0) return nullptr;
  AVPacket *p = q->v[q->head];
  q->head = (q->head + 1u) % q->cap;
  q->count--;
  q->bytes -= (size_t)p->size;
  return p;
}

static void pktq_clear(avs_pktq *q)
{
  AVPacket *p;
  while ((p = pktq_pop(q)) != nullptr) av_packet_free(&p);
  q->head = 0;
  q->bytes = 0;
}

// ---- open / close ----

static void avs_teardown(ccord_av_stream *s)
{
  pktq_clear(&s->vq);
  pktq_clear(&s->aq);
  sws_freeContext(s->sws);
  s->sws = nullptr;
  sws_freeContext(s->sws_alpha);
  s->sws_alpha = nullptr;
  swr_free(&s->swr);
  av_packet_free(&s->pkt);
  av_packet_free(&s->alpha_pkt);
  av_frame_free(&s->frame);
  av_frame_free(&s->alpha_frame);
  avcodec_free_context(&s->vdec);
  avcodec_free_context(&s->vdec_alpha);
  avcodec_free_context(&s->adec);
  s->have_alpha_frame = false;
  if (s->fmt) avformat_close_input(&s->fmt);
  if (s->io) {
    av_freep(&s->io->buffer);
    avio_context_free(&s->io);
  }
  s->vsi = s->asi = -1;
}

static int avs_rotation_turns(const AVStream *st)
{
  const AVPacketSideData *sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                       st->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
  if (!sd || !sd->data || sd->size < 9u * sizeof(int32_t)) return 0;
  double deg = av_display_rotation_get((const int32_t *)(const void *)sd->data);   // counter-clockwise
  if (!isfinite(deg)) return 0;
  int cw = (int)lround(-deg / 90.0);
  cw %= 4;
  if (cw < 0) cw += 4;
  return cw;
}

static uint64_t avs_rd_be64(const uint8_t *p)
{
  return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
         ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static AVCodecContext *avs_open_codec(const AVCodec *codec, AVStream *st, int threads)
{
  AVCodecContext *dec = avcodec_alloc_context3(codec);
  if (!dec) return nullptr;
  if (avcodec_parameters_to_context(dec, st->codecpar) < 0) {
    avcodec_free_context(&dec);
    return nullptr;
  }
  dec->pkt_timebase = st->time_base;
  dec->thread_count = threads;
  if (avcodec_open2(dec, codec, nullptr) < 0) avcodec_free_context(&dec);
  return dec;
}

static AVCodecContext *avs_open_alpha_codec(const AVCodec *codec, const AVStream *st)
{
  if (!codec) return nullptr;
  AVCodecContext *dec = avcodec_alloc_context3(codec);
  if (!dec) return nullptr;
  dec->pkt_timebase = st->time_base;
  dec->thread_count = 1;
  if (avcodec_open2(dec, codec, nullptr) < 0) avcodec_free_context(&dec);
  return dec;
}

// Copies what the player reads from fmt/AVStream afterwards into s->meta (demux side, before READY).
static void avs_fill_meta(ccord_av_stream *s)
{
  avs_meta *m = &s->meta;
  *m = (avs_meta){.v_start = AV_NOPTS_VALUE, .a_start = AV_NOPTS_VALUE, .fmt_start_time = s->fmt->start_time};
  if (s->vsi >= 0) {
    const AVStream *vs = s->fmt->streams[s->vsi];
    m->has_video = true;
    m->v_tb = vs->time_base;
    m->v_start = vs->start_time;
    m->v_avg_fr = vs->avg_frame_rate;
  }
  if (s->asi >= 0) {
    const AVStream *as = s->fmt->streams[s->asi];
    m->has_audio = true;
    m->a_tb = as->time_base;
    m->a_start = as->start_time;
  }
  if (s->fmt->duration > 0) {
    int64_t ms = s->fmt->duration / (AV_TIME_BASE / 1000);
    m->duration_ms = ms > 0 ? (ms > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)ms) : 0;
  }
}

static ccord_status avs_setup(ccord_av_stream *s, ccord_error *err)
{
  s->vsi = s->asi = -1;
  s->mb.pos = 0;
  s->src_pos = 0;
  uint8_t *iobuf = av_malloc(AVS_IO_BUF);
  if (!iobuf) return ccord_error_set(err, CCORD_ERR_NOMEM, "av_stream: out of memory");
  s->io = s->src_mode ? avio_alloc_context(iobuf, AVS_IO_BUF, 0, s, avs_src_read, nullptr, avs_src_seek)
                      : avio_alloc_context(iobuf, AVS_IO_BUF, 0, &s->mb, avs_read, nullptr, avs_seek);
  if (!s->io) {
    av_free(iobuf);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "av_stream: out of memory");
  }
  s->fmt = avformat_alloc_context();
  if (!s->fmt) return ccord_error_set(err, CCORD_ERR_NOMEM, "av_stream: out of memory");
  s->fmt->max_streams = 16;
  s->fmt->pb = s->io;
  s->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  if (s->src_mode) s->fmt->interrupt_callback = (AVIOInterruptCB){.callback = avs_interrupt, .opaque = s};
  const char *images = "gif,apng,webp_anim,webp_pipe,png_pipe,jpeg_pipe,bmp_pipe,tiff_pipe";
  const char *media = "wav,ogg,mp3,flac,mov,mp4,m4a,3gp,3g2,mj2,matroska,webm,gif";
  char whitelist[192];
  bool wi = (s->flags & CCORD_AV_IMAGES) != 0, wm = (s->flags & CCORD_AV_MEDIA) != 0;
  snprintf(whitelist, sizeof whitelist, "%s%s%s", wi ? images : "", wi && wm ? "," : "", wm ? media : "");
  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "format_whitelist", whitelist, 0);
  av_dict_set(&opts, "protocol_whitelist", "", 0);
  int orc = avformat_open_input(&s->fmt, "", nullptr, &opts);   // frees fmt on failure
  av_dict_free(&opts);
  if (orc < 0) {
    s->fmt = nullptr;
    if (s->src_err != CCORD_OK) return ccord_error_set(err, s->src_err, "av_stream: source read failed");
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "av_stream: unsupported media format");
  }
  if (avformat_find_stream_info(s->fmt, nullptr) < 0) {
    if (s->src_err != CCORD_OK) return ccord_error_set(err, s->src_err, "av_stream: source read failed");
    return ccord_error_set(err, CCORD_ERR_PARSE, "av_stream: cannot read stream info");
  }

  const AVCodec *vcodec = nullptr, *acodec = nullptr;
  int vsi = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
  if (vsi >= 0 && vcodec) {
    AVStream *vs = s->fmt->streams[vsi];
    int cw = vs->codecpar->width, ch = vs->codecpar->height;
    if (cw > AVS_MAX_DIM || ch > AVS_MAX_DIM || (int64_t)cw * ch > AVS_MAX_SRC_PIXELS) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "av_stream: video dimensions out of range");
    }
    // WebM/Matroska VP8/VP9 streams with alpha_mode=1 carry the alpha plane in BlockAdditional side data
    // (8-byte big-endian BlockAddID == 1 followed by a VP8/VP9 frame whose Y plane is 8-bit alpha).
    // FFmpeg's built-in vp8/vp9 decoders ignore that side data and Android/minimal FFmpeg builds omit libvpx.
    // Pair the built-in software vp8/vp9 decoder with a single-threaded companion decoder for the alpha plane
    // so desktop and Android decode transparent WebM identically without requiring libvpx; fall back to
    // libvpx / libvpx-vp9 if the built-in software decoder is absent.
    const AVCodec *fallback_vcodec = vcodec;
    const AVDictionaryEntry *alpha_tag = av_dict_get(vs->metadata, "alpha_mode", nullptr, 0);
    bool has_alpha = alpha_tag && alpha_tag->value && strcmp(alpha_tag->value, "1") == 0;
    bool is_vpx_alpha = has_alpha &&
                        (vs->codecpar->codec_id == AV_CODEC_ID_VP9 || vs->codecpar->codec_id == AV_CODEC_ID_VP8);
    bool want_companion_alpha = false;
    const AVCodec *alpha_codec = nullptr;
    if (is_vpx_alpha) {
      const char *sw_name = vs->codecpar->codec_id == AV_CODEC_ID_VP9 ? "vp9" : "vp8";
      const char *vpx_name = vs->codecpar->codec_id == AV_CODEC_ID_VP9 ? "libvpx-vp9" : "libvpx";
      const AVCodec *sw = avcodec_find_decoder_by_name(sw_name);
      if (sw) {
        vcodec = sw;
        alpha_codec = sw;
        want_companion_alpha = true;
      } else {
        const AVCodec *vpx = avcodec_find_decoder_by_name(vpx_name);
        if (vpx) vcodec = vpx;
      }
    }
    // Frame threads only for real video: they cost latency and memory that a GIF never earns back.
    // Companion alpha decoding stays single-threaded so color and alpha frames stay strictly 1:1.
    int threads = 1;
    if ((s->flags & CCORD_AV_MEDIA) && !want_companion_alpha) {
      threads = av_cpu_count() / 2;
      if (threads < 1) threads = 1;
      if (threads > 4) threads = 4;
    }
#if defined(__ANDROID__)
    if (!has_alpha) {
      const char *hw_name = nullptr;
      switch (vs->codecpar->codec_id) {
        case AV_CODEC_ID_H264:  hw_name = "h264_mediacodec";  break;
        case AV_CODEC_ID_HEVC:  hw_name = "hevc_mediacodec";  break;
        case AV_CODEC_ID_VP9:   hw_name = "vp9_mediacodec";   break;
        case AV_CODEC_ID_VP8:   hw_name = "vp8_mediacodec";   break;
        case AV_CODEC_ID_MPEG4: hw_name = "mpeg4_mediacodec"; break;
        default: break;
      }
      if (hw_name) {
        const AVCodec *hw = avcodec_find_decoder_by_name(hw_name);
        if (hw) s->vdec = avs_open_codec(hw, vs, 1);
      }
    }
    if (!s->vdec) s->vdec = avs_open_codec(vcodec, vs, threads);
#else
    s->vdec = avs_open_codec(vcodec, vs, threads);
#endif
    if (!s->vdec && vcodec != fallback_vcodec) {
      want_companion_alpha = false;
      alpha_codec = nullptr;
      s->vdec = avs_open_codec(fallback_vcodec, vs, threads);
    }
    if (s->vdec && want_companion_alpha && alpha_codec) {
      s->vdec_alpha = avs_open_alpha_codec(alpha_codec, vs);
      if (s->vdec_alpha) {
        s->alpha_frame = av_frame_alloc();
        s->alpha_pkt = av_packet_alloc();
        if (!s->alpha_frame || !s->alpha_pkt) {
          avcodec_free_context(&s->vdec_alpha);
          av_frame_free(&s->alpha_frame);
          av_packet_free(&s->alpha_pkt);
        }
      }
    }
    if (s->vdec) {
      s->vsi = vsi;
      s->rot = avs_rotation_turns(vs);
    }
  }
  if (!(s->flags & CCORD_AV_NO_AUDIO)) {
    int asi = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
    if (asi >= 0 && acodec) {
      AVStream *as = s->fmt->streams[asi];
      s->adec = avs_open_codec(acodec, as, 1);
      if (s->adec) s->asi = asi;
    }
  }
  if (s->vsi < 0 && s->asi < 0) {
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "av_stream: no decodable audio or video stream");
  }
  for (unsigned i = 0; i < s->fmt->nb_streams; i++) {
    if ((int)i != s->vsi && (int)i != s->asi) s->fmt->streams[i]->discard = AVDISCARD_ALL;
  }
  s->frame = av_frame_alloc();
  s->pkt = av_packet_alloc();
  if (!s->frame || !s->pkt) return ccord_error_set(err, CCORD_ERR_NOMEM, "av_stream: out of memory");
  s->demux_eof = s->v_flushed = s->a_flushed = false;
  s->v_done = s->vsi < 0;
  s->a_done = s->asi < 0;
  avs_fill_meta(s);
  return CCORD_OK;
}

static void avs_reset_playhead(ccord_av_stream *s, uint32_t pos_ms)
{
  swr_free(&s->swr);
  s->swr_skip_frames = 0;
  s->have_next = false;
  s->fallback_pts = 0;
  s->skip_before_ms = pos_ms;
  s->pcm_head = s->pcm_len = 0;
  s->pcm_pos_ms = pos_ms;
  s->pcm_pos_rem = 0;
  s->a_tail_ms = pos_ms;
}

ccord_av_stream *ccord_av_stream_open(const uint8_t *data, size_t len, int max_dim, unsigned flags, ccord_error *err)
{
  ccord_error_clear(err);
  if (!data || len < 8 || !(flags & (CCORD_AV_IMAGES | CCORD_AV_MEDIA))) {
    ccord_error_set(err, CCORD_ERR_INVALID, "av_stream: invalid argument");
    return nullptr;
  }
  if (len > CCORD_AV_MAX_INPUT) {
    ccord_error_set(err, CCORD_ERR_RANGE, "av_stream: input too large");
    return nullptr;
  }
  if ((flags & CCORD_AV_IMAGES) && ccord_lottie_sniff(data, len)) {
    ccord_lottie_stream *ls = ccord_lottie_open(data, len, max_dim, err);
    if (!ls) return nullptr;
    ccord_av_stream *s = ccord_calloc(1, sizeof *s);
    if (!s) {
      ccord_lottie_close(ls);
      ccord_error_set(err, CCORD_ERR_NOMEM, "av_stream: out of memory");
      return nullptr;
    }
    s->self = s;
    s->lottie = ls;
    s->flags = flags;
    s->max_dim = max_dim > 0 ? max_dim : 0;
    s->vsi = s->asi = -1;
    ccord_lottie_info(ls, &s->info.w, &s->info.h, &s->info.duration_ms);
    s->info.has_video = true;
    s->sw = s->info.w;
    s->sh = s->info.h;
    s->size_known = true;
    s->state = CCORD_AV_READY;
    s->last_used_ms = ccord_clock_ms();
    return s;
  }
  if (av_log_get_level() > AV_LOG_ERROR) av_log_set_level(AV_LOG_ERROR);
  ccord_av_stream *s = ccord_calloc(1, sizeof *s);
  uint8_t *copy = s ? ccord_malloc(len) : nullptr;
  if (!s || !copy) {
    ccord_free(s);
    ccord_error_set(err, CCORD_ERR_NOMEM, "av_stream: out of memory");
    return nullptr;
  }
  memcpy(copy, data, len);
  s->self = s;
  s->data = copy;
  s->mb = (avs_membuf){.p = copy, .len = len, .pos = 0};
  s->flags = flags;
  s->max_dim = max_dim > 0 ? max_dim : 0;
  if (avs_setup(s, err) != CCORD_OK) {
    ccord_av_stream_close(s);
    return nullptr;
  }
  s->info.has_video = s->meta.has_video;
  s->info.has_audio = s->meta.has_audio;
  s->info.duration_ms = s->meta.duration_ms;
  avs_reset_playhead(s, 0);
  s->last_used_ms = ccord_clock_ms();
  return s;
}

// ---- source mode: demux thread ----

// Demux side: reads the next packet into s->pkt. Memory mode calls it on the caller's thread, source mode only
// on the demux thread (no lock held: this is where read_at blocks).
static int avs_read_one(ccord_av_stream *s) { return av_read_frame(s->fmt, s->pkt); }

// Demux side: repositions the demuxer at pos (AV_TIME_BASE units, start_time already applied).
static int avs_seek_demuxer(ccord_av_stream *s, int64_t ts) { return av_seek_frame(s->fmt, -1, ts, AVSEEK_FLAG_BACKWARD); }

// mx held. The demux thread has nothing to do: a seek is not pending and either the stream is at its end or the
// queue the player is draining is full.
static bool avs_demux_idle(const ccord_av_stream *s)
{
  if (s->seek_req) return false;
  if (s->demux_eof) return true;
  if (s->meta.has_video) {
    if (s->vq.dead || s->vq.bytes < AVS_VQ_MAX_BYTES) return false;
    // Over the soft limit: keep reading only while audio is wanted and none is queued (badly interleaved file).
    bool starved_audio = s->meta.has_audio && !s->aq.dead && s->aq.count == 0 && s->vq.bytes < AVS_VQ_HARD_BYTES;
    return !starved_audio;
  }
  return s->meta.has_audio && !s->aq.dead && s->aq.bytes >= AVS_AQ_MAX_BYTES;   // audio-only: back-pressure
}

// mx held. Queues the packet in s->pkt for the player, or drops it.
static void avs_demux_push(ccord_av_stream *s)
{
  avs_pktq *q = nullptr;
  if (s->pkt->stream_index == s->vsi && s->vdec) q = &s->vq;
  else if (s->pkt->stream_index == s->asi && s->adec) q = &s->aq;
  if (!q || q->dead) return;
  if (!pktq_push(q, s->pkt)) {
    s->demux_eof = true;   // out of memory: finish with what is queued
    return;
  }
  if (q == &s->aq && s->meta.has_video) {   // nobody may be reading audio: keep only the newest
    while (q->bytes > AVS_AQ_MAX_BYTES && q->count > 1) {
      AVPacket *old = pktq_pop(q);
      av_packet_free(&old);
    }
  }
}

// mx held: the demux thread gives up for good.
static void avs_fail(ccord_av_stream *s, ccord_status code, const char *msg)
{
  if (s->state == CCORD_AV_FAILED) return;
  ccord_error_set(&s->serr, code, "%s", msg);
  s->state = CCORD_AV_FAILED;
  ccord_cond_broadcast(s->cv);
}

static void avs_demux_main(void *arg)
{
  ccord_av_stream *s = arg;
  ccord_error err;
  ccord_error_clear(&err);
  ccord_status st = avs_setup(s, &err);
  ccord_mutex_lock(s->mx);
  if (st == CCORD_OK && !atomic_load(&s->stop)) {
    s->src_err = CCORD_OK;
    s->state = CCORD_AV_READY;
    ccord_cond_broadcast(s->cv);
  } else {
    if (st == CCORD_OK) ccord_error_set(&err, CCORD_ERR_IO, "av_stream: closed while opening");
    avs_fail(s, err.code, err.message);
    ccord_mutex_unlock(s->mx);
    avs_teardown(s);   // the player never saw these objects; ccord_av_stream_close finds them already freed
    return;
  }
  while (!atomic_load(&s->stop)) {
    while (!atomic_load(&s->stop) && avs_demux_idle(s)) ccord_cond_wait(s->cv, s->mx);
    if (atomic_load(&s->stop)) break;
    if (s->seek_req) {
      int64_t ts = s->seek_ts;
      s->seek_req = false;
      s->demux_eof = false;
      ccord_mutex_unlock(s->mx);
      int r = avs_seek_demuxer(s, ts);
      ccord_mutex_lock(s->mx);
      if (r < 0) {
        if (s->src_err != CCORD_OK) avs_fail(s, s->src_err, "av_stream: source read failed");
        else avs_fail(s, CCORD_ERR_UNSUPPORTED, "av_stream: cannot seek");
      }
      if (s->state == CCORD_AV_FAILED) break;
      continue;
    }
    uint64_t my_gen = s->gen;   // re-read every iteration: a packet read under an older value is stale
    ccord_mutex_unlock(s->mx);
    int r = avs_read_one(s);
    ccord_mutex_lock(s->mx);
    if (r >= 0) {
      if (my_gen == s->gen) avs_demux_push(s);
      av_packet_unref(s->pkt);
    } else if (my_gen != s->gen) {
      s->src_err = CCORD_OK;   // a seek was posted meanwhile: that read's outcome no longer matters
    } else if (s->src_err != CCORD_OK && !atomic_load(&s->stop)) {
      avs_fail(s, s->src_err, "av_stream: source read failed");
      break;
    } else {
      s->demux_eof = true;
      ccord_cond_broadcast(s->cv);
    }
  }
  ccord_mutex_unlock(s->mx);
}

ccord_av_stream *ccord_av_stream_open_source(const ccord_av_source *src, int max_dim, unsigned flags, ccord_error *err)
{
  ccord_error_clear(err);
  if (!src || !src->read_at || !(flags & CCORD_AV_MEDIA) || (flags & CCORD_AV_IMAGES)) {
    ccord_error_set(err, CCORD_ERR_INVALID, "av_stream: invalid argument");
    return nullptr;
  }
  if (av_log_get_level() > AV_LOG_ERROR) av_log_set_level(AV_LOG_ERROR);
  ccord_av_stream *s = ccord_calloc(1, sizeof *s);
  if (!s) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "av_stream: out of memory");
    return nullptr;
  }
  s->self = s;
  s->src_mode = true;
  s->src = *src;
  s->flags = flags;
  s->max_dim = max_dim > 0 ? max_dim : 0;
  s->vsi = s->asi = -1;
  s->state = CCORD_AV_OPENING;
  atomic_init(&s->stop, false);
  s->mx = ccord_mutex_create();
  s->cv = s->mx ? ccord_cond_create() : nullptr;
  s->sync_init = s->mx != nullptr && s->cv != nullptr;
  s->thr = s->sync_init ? ccord_thread_create(avs_demux_main, s) : nullptr;
  if (!s->thr) {
    ccord_error_set(err, CCORD_ERR_INTERNAL, "av_stream: cannot start the demux thread");
    ccord_av_stream_close(s);
    return nullptr;
  }
  s->thr_started = true;
  s->last_used_ms = ccord_clock_ms();
  return s;
}

// UI thread: the stream's state; the first READY copies the demux thread's snapshot into info.
static ccord_av_state avs_observe(const ccord_av_stream *cs, ccord_error *err)
{
  ccord_av_stream *s = cs->self;
  if (!s->src_mode) return CCORD_AV_READY;
  ccord_mutex_lock(s->mx);
  ccord_av_state st = s->state;
  if (err && st == CCORD_AV_FAILED) *err = s->serr;
  ccord_mutex_unlock(s->mx);
  if (st == CCORD_AV_READY && !s->observed) {
    s->observed = true;   // meta is stable: it was written before READY was published under mx
    s->info.has_video = s->meta.has_video;
    s->info.has_audio = s->meta.has_audio;
    s->info.duration_ms = s->meta.duration_ms;
    avs_reset_playhead(s, 0);
    if (s->pre_seek) {
      s->pre_seek = false;
      ccord_av_stream_seek(s, s->pre_seek_ms);
    }
  }
  return st;
}

ccord_av_state ccord_av_stream_state(const ccord_av_stream *s, ccord_error *err)
{
  if (err) ccord_error_clear(err);
  if (!s) {
    ccord_error_set(err, CCORD_ERR_INVALID, "av_stream: no stream");
    return CCORD_AV_FAILED;
  }
  return avs_observe(s, err);
}

bool ccord_av_stream_starved(const ccord_av_stream *s) { return s && s->starved; }

void ccord_av_stream_close(ccord_av_stream *s)
{
  if (!s) return;
  if (s->lottie) {
    ccord_lottie_close(s->lottie);
    ccord_free(s);
    return;
  }
  if (s->thr_started) {
    // Wake a read_at that is blocked for good, tell the thread to stop, and wait for it: only then is nothing
    // using fmt/io/the source any more and the teardown below race-free.
    if (s->src.cancel) s->src.cancel(s->src.ud);
    ccord_mutex_lock(s->mx);
    atomic_store(&s->stop, true);
    ccord_cond_broadcast(s->cv);
    ccord_mutex_unlock(s->mx);
    ccord_thread_join(s->thr);
    s->thr = nullptr;
  }
  if (s->sync_init || s->mx || s->cv) {
    ccord_cond_destroy(s->cv);
    ccord_mutex_destroy(s->mx);
    s->cv = nullptr;
    s->mx = nullptr;
  }
  avs_teardown(s);
  ccord_free(s->vq.v);
  ccord_free(s->aq.v);
  ccord_free(s->scratch);
  ccord_free(s->alpha_scaled);
  ccord_free(s->cur);
  ccord_free(s->next);
  ccord_free(s->pcm);
  ccord_free(s->swr_tmp);
  ccord_free(s->data);
  ccord_free(s);
}

const ccord_av_info *ccord_av_stream_info(const ccord_av_stream *s)
{
  if (!s) return nullptr;
  (void)avs_observe(s, nullptr);
  return &s->info;
}

// ---- demux / decode ----

// Memory mode only: reads and routes the next packet on the caller's thread.
static void avs_read_packet(ccord_av_stream *s)
{
  if (s->demux_eof) return;
  if (avs_read_one(s) < 0) {
    s->demux_eof = true;
    return;
  }
  bool ok = true;
  if (s->pkt->stream_index == s->vsi && s->vdec && !s->v_done) ok = pktq_push(&s->vq, s->pkt);
  else if (s->pkt->stream_index == s->asi && s->adec && !s->a_done) ok = pktq_push(&s->aq, s->pkt);
  av_packet_unref(s->pkt);
  if (!ok) s->demux_eof = true;   // out of memory: finish with what is queued
}

// The consumer is finished with this stream: in source mode the demux thread stops queueing its packets.
static void avs_finish(ccord_av_stream *s, avs_pktq *q, bool *done)
{
  *done = true;
  if (!s->src_mode) return;
  ccord_mutex_lock(s->mx);
  q->dead = true;
  pktq_clear(q);
  ccord_cond_broadcast(s->cv);
  ccord_mutex_unlock(s->mx);
}

// Source mode: pops the next packet of q. *eof is true when the queue is empty and the demuxer is at its end.
static AVPacket *avs_take(ccord_av_stream *s, avs_pktq *q, bool *eof)
{
  ccord_mutex_lock(s->mx);
  AVPacket *p = pktq_pop(q);
  *eof = s->demux_eof;
  if (p) ccord_cond_broadcast(s->cv);   // room for the demux thread
  ccord_mutex_unlock(s->mx);
  return p;
}

static void avs_send_alpha_pkt(ccord_av_stream *s, const AVPacket *p)
{
  if (!s->vdec_alpha || !s->alpha_pkt || !s->alpha_frame) return;
  if (!p) {
    (void)avcodec_send_packet(s->vdec_alpha, nullptr);
    return;
  }
  size_t sd_size = 0;
  const uint8_t *sd = av_packet_get_side_data(p, AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &sd_size);
  if (!sd || sd_size <= 8u || avs_rd_be64(sd) != 1u) return;
  size_t alpha_len = sd_size - 8u;
  if (alpha_len > (size_t)INT_MAX - (size_t)AV_INPUT_BUFFER_PADDING_SIZE) return;
  av_packet_unref(s->alpha_pkt);
  if (av_new_packet(s->alpha_pkt, (int)alpha_len) < 0) return;
  memcpy(s->alpha_pkt->data, sd + 8u, alpha_len);
  s->alpha_pkt->pts = p->pts;
  s->alpha_pkt->dts = p->dts;
  s->alpha_pkt->duration = p->duration;
  s->alpha_pkt->flags = p->flags;
  if (avcodec_send_packet(s->vdec_alpha, s->alpha_pkt) == AVERROR(EAGAIN)) {
    while (avcodec_receive_frame(s->vdec_alpha, s->alpha_frame) == 0) av_frame_unref(s->alpha_frame);
    (void)avcodec_send_packet(s->vdec_alpha, s->alpha_pkt);
  }
  av_packet_unref(s->alpha_pkt);
}

static void avs_recv_alpha_frame(ccord_av_stream *s)
{
  s->have_alpha_frame = false;
  if (!s->vdec_alpha || !s->alpha_frame) return;
  av_frame_unref(s->alpha_frame);
  while (avcodec_receive_frame(s->vdec_alpha, s->alpha_frame) == 0) {
    if (s->frame->pts != AV_NOPTS_VALUE && s->alpha_frame->pts != AV_NOPTS_VALUE &&
        s->alpha_frame->pts < s->frame->pts) {
      av_frame_unref(s->alpha_frame);
      continue;
    }
    s->have_alpha_frame = true;
    break;
  }
}

static void avs_unref_video_frames(ccord_av_stream *s)
{
  av_frame_unref(s->frame);
  if (s->alpha_frame) av_frame_unref(s->alpha_frame);
  s->have_alpha_frame = false;
}

// Receives the next frame of one decoder into s->frame. False at end of stream (sets *done), or, in source mode,
// when the demux thread has not produced the packets yet (sets s->starved; *done and the drain stay untouched).
static bool avs_decode(ccord_av_stream *s, AVCodecContext *dec, avs_pktq *q, bool *flushed, bool *done)
{
  if (!dec || *done) return false;
  const bool with_alpha = (dec == s->vdec && s->vdec_alpha != nullptr);
  for (;;) {
    int r = avcodec_receive_frame(dec, s->frame);
    if (r == 0) {
      if (with_alpha) avs_recv_alpha_frame(s);
      return true;
    }
    if (r != AVERROR(EAGAIN)) {
      avs_finish(s, q, done);
      return false;
    }
    AVPacket *p;
    if (s->src_mode) {
      bool eof = false;
      p = avs_take(s, q, &eof);
      if (!p && !eof) {
        s->starved = true;
        return false;
      }
    } else {
      if (q->count == 0 && !s->demux_eof) {
        avs_read_packet(s);
        continue;
      }
      p = pktq_pop(q);
    }
    if (!p) {
      if (*flushed) {
        avs_finish(s, q, done);
        return false;
      }
      *flushed = true;
      (void)avcodec_send_packet(dec, nullptr);
      if (with_alpha) avs_send_alpha_pkt(s, nullptr);
      continue;
    }
    (void)avcodec_send_packet(dec, p);   // a corrupt packet is skipped; the decoder resyncs on the next keyframe
    if (with_alpha) avs_send_alpha_pkt(s, p);
    av_packet_free(&p);
  }
}

static uint32_t avs_ms(int64_t v)
{
  if (v <= 0) return 0;
  return v > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

static void avs_rotate(const uint8_t *src, int sw, int sh, uint8_t *dst, int turns)
{
  const uint32_t *in = (const uint32_t *)(const void *)src;
  uint32_t *out = (uint32_t *)(void *)dst;
  if (turns == 2) {
    size_t n = (size_t)sw * (size_t)sh;
    for (size_t i = 0; i < n; i++) out[i] = in[n - 1u - i];
    return;
  }
  int dw = sh;   // 90 or 270: the output is sh wide and sw tall
  for (int y = 0; y < sh; y++) {
    const uint32_t *row = in + (size_t)y * (size_t)sw;
    for (int x = 0; x < sw; x++) {
      int dx = turns == 1 ? sh - 1 - y : y;
      int dy = turns == 1 ? x : sw - 1 - x;
      out[(size_t)dy * (size_t)dw + (size_t)dx] = row[x];
    }
  }
}

// Timing of s->frame as a video frame.
static void avs_video_times(ccord_av_stream *s, uint32_t *pts_ms, uint32_t *dur_ms)
{
  const AVFrame *f = s->frame;
  uint32_t pts = s->fallback_pts;
  const avs_meta *m = &s->meta;
  if (f->best_effort_timestamp != AV_NOPTS_VALUE && m->has_video) {
    int64_t start = m->v_start != AV_NOPTS_VALUE ? m->v_start : 0;
    pts = avs_ms(av_rescale_q(f->best_effort_timestamp - start, m->v_tb, (AVRational){1, 1000}));
  }
  uint32_t dur = 0;
  if (f->duration > 0 && m->has_video) {
    dur = avs_ms(av_rescale_q(f->duration, m->v_tb, (AVRational){1, 1000}));
  }
  if (dur == 0 && m->has_video && (s->flags & CCORD_AV_MEDIA)) {
    AVRational fr = m->v_avg_fr;
    if (fr.num > 0 && fr.den > 0) dur = avs_ms(av_rescale(1000, fr.den, fr.num));
  }
  if (dur > 60000u) dur = 60000u;
  s->fallback_pts = pts + (dur ? dur : 33u);
  *pts_ms = pts;
  *dur_ms = dur;
}

// Converts s->frame into `dst` (allocating the buffers on the first frame). False on failure.
static bool avs_convert(ccord_av_stream *s, uint8_t **dst)
{
  const AVFrame *f = s->frame;
  if (f->width <= 0 || f->height <= 0 || f->width > AVS_MAX_DIM || f->height > AVS_MAX_DIM ||
      (int64_t)f->width * f->height > AVS_MAX_SRC_PIXELS) {
    return false;
  }
  if (!s->size_known) {
    int dw = f->width, dh = f->height;
    int m = dw > dh ? dw : dh;
    if (s->max_dim > 0 && m > s->max_dim) {
      dw = (int)(((int64_t)dw * s->max_dim + m / 2) / m);
      dh = (int)(((int64_t)dh * s->max_dim + m / 2) / m);
      if (dw < 1) dw = 1;
      if (dh < 1) dh = 1;
    }
    s->sw = dw;
    s->sh = dh;
    bool quarter = (s->rot & 1) != 0;
    s->info.w = quarter ? dh : dw;
    s->info.h = quarter ? dw : dh;
    s->size_known = true;
  }
  size_t bytes = (size_t)s->sw * (size_t)s->sh * 4u;
  if (!*dst) *dst = ccord_malloc(bytes + AVS_SWS_DST_PAD);
  if (s->rot != 0 && !s->scratch) s->scratch = ccord_malloc(bytes + AVS_SWS_DST_PAD);
  if (!*dst || (s->rot != 0 && !s->scratch)) return false;
  uint8_t *target = s->rot != 0 ? s->scratch : *dst;
  enum AVPixelFormat src_fmt = (enum AVPixelFormat)f->format;
  const uint8_t *src_data[4] = {f->data[0], f->data[1], f->data[2], f->data[3]};
  int src_linesize[4] = {f->linesize[0], f->linesize[1], f->linesize[2], f->linesize[3]};
  bool attached_alpha = false;
  bool post_apply_alpha = false;
  const AVFrame *af = (s->have_alpha_frame && s->alpha_frame) ? s->alpha_frame : nullptr;
  if (af && af->data[0] && af->width > 0 && af->height > 0 && af->linesize[0] >= af->width) {
    const AVPixFmtDescriptor *adesc = av_pix_fmt_desc_get((enum AVPixelFormat)af->format);
    bool alpha_u8_planar = adesc != nullptr && adesc->nb_components >= 1 &&
                           adesc->comp[0].depth == 8 && adesc->comp[0].step == 1 && adesc->comp[0].offset == 0;
    if (alpha_u8_planar && af->width == f->width && af->height == f->height) {
      enum AVPixelFormat yuva_fmt = AV_PIX_FMT_NONE;
      switch (src_fmt) {
        case AV_PIX_FMT_YUV420P: yuva_fmt = AV_PIX_FMT_YUVA420P; break;
        case AV_PIX_FMT_YUV422P: yuva_fmt = AV_PIX_FMT_YUVA422P; break;
        case AV_PIX_FMT_YUV444P: yuva_fmt = AV_PIX_FMT_YUVA444P; break;
        case AV_PIX_FMT_GBRP:    yuva_fmt = AV_PIX_FMT_GBRAP;    break;
        default: break;
      }
      if (yuva_fmt != AV_PIX_FMT_NONE) {
        src_fmt = yuva_fmt;
        src_data[3] = af->data[0];
        src_linesize[3] = af->linesize[0];
        attached_alpha = true;
      } else {
        post_apply_alpha = true;
      }
    } else if (alpha_u8_planar) {
      post_apply_alpha = true;
    }
  }
  if (!attached_alpha && !post_apply_alpha && src_fmt == AV_PIX_FMT_RGBA && f->width == s->sw && f->height == s->sh &&
      f->data[0] && f->linesize[0] >= s->sw * 4) {
    const int row_bytes = s->sw * 4;
    if (f->linesize[0] == row_bytes) {
      memcpy(target, f->data[0], bytes);
    } else {
      for (int y = 0; y < s->sh; y++) {
        memcpy(target + (size_t)y * (size_t)row_bytes, f->data[0] + (size_t)y * (size_t)f->linesize[0], (size_t)row_bytes);
      }
    }
  } else {
    int sws_flags = (s->sw < f->width || s->sh < f->height) ? SWS_AREA : SWS_BILINEAR;
    s->sws = sws_getCachedContext(s->sws, f->width, f->height, src_fmt, s->sw, s->sh,
                                  AV_PIX_FMT_RGBA, sws_flags, nullptr, nullptr, nullptr);
    if (!s->sws) return false;
    uint8_t *planes[4] = {target, nullptr, nullptr, nullptr};
    int strides[4] = {s->sw * 4, 0, 0, 0};
    if (sws_scale(s->sws, src_data, src_linesize, 0, f->height, planes, strides) != s->sh) {
      return false;
    }
  }
  if (post_apply_alpha && af) {
    const uint8_t *aplane = af->data[0];
    int astride = af->linesize[0];
    if (af->width != s->sw || af->height != s->sh) {
      size_t abytes = (size_t)s->sw * (size_t)s->sh;
      if (!s->alpha_scaled) s->alpha_scaled = ccord_malloc(abytes + AVS_SWS_DST_PAD);
      if (!s->alpha_scaled) return false;
      int aflags = (s->sw < af->width || s->sh < af->height) ? SWS_AREA : SWS_BILINEAR;
      s->sws_alpha = sws_getCachedContext(s->sws_alpha, af->width, af->height, AV_PIX_FMT_GRAY8, s->sw, s->sh,
                                          AV_PIX_FMT_GRAY8, aflags, nullptr, nullptr, nullptr);
      if (!s->sws_alpha) return false;
      const uint8_t *asrc[4] = {af->data[0], nullptr, nullptr, nullptr};
      int asrc_ls[4] = {af->linesize[0], 0, 0, 0};
      uint8_t *adst[4] = {s->alpha_scaled, nullptr, nullptr, nullptr};
      int adst_ls[4] = {s->sw, 0, 0, 0};
      if (sws_scale(s->sws_alpha, asrc, asrc_ls, 0, af->height, adst, adst_ls) != s->sh) {
        return false;
      }
      aplane = s->alpha_scaled;
      astride = s->sw;
    }
    for (int y = 0; y < s->sh; y++) {
      const uint8_t *arow = aplane + (size_t)y * (size_t)astride;
      uint8_t *drow = target + (size_t)y * (size_t)s->sw * 4u;
      for (int x = 0; x < s->sw; x++) {
        drow[(size_t)x * 4u + 3u] = arow[x];
      }
    }
  }
  if (s->rot != 0) avs_rotate(s->scratch, s->sw, s->sh, *dst, s->rot);
  const AVPixFmtDescriptor *pdesc = av_pix_fmt_desc_get(src_fmt);
  s->last_frame_opaque = !attached_alpha && !post_apply_alpha &&
                         pdesc != nullptr && !(pdesc->flags & AV_PIX_FMT_FLAG_ALPHA) &&
                         src_fmt != AV_PIX_FMT_PAL8;
  return true;
}

bool av_stream_next_frame_ex(ccord_av_stream *s, bool want_premul, const uint8_t **rgba, uint32_t *pts_ms,
                             uint32_t *dur_ms, bool *out_already_premul)
{
  if (rgba) *rgba = nullptr;
  if (out_already_premul) *out_already_premul = false;
  if (!s || !rgba) return false;
  s->last_used_ms = ccord_clock_ms();
  if (s->lottie) {
    if (want_premul) {
      if (!ccord_lottie_next_frame_premul(s->lottie, rgba, pts_ms, dur_ms)) return false;
      if (out_already_premul) *out_already_premul = true;
      return true;
    }
    return ccord_lottie_next_frame(s->lottie, rgba, pts_ms, dur_ms);
  }
  if (avs_observe(s, nullptr) != CCORD_AV_READY) return false;
  if (!avs_decode(s, s->vdec, &s->vq, &s->v_flushed, &s->v_done)) return false;
  uint32_t pts = 0, dur = 0;
  avs_video_times(s, &pts, &dur);
  bool ok = avs_convert(s, &s->next);
  avs_unref_video_frames(s);
  if (!ok) {
    avs_finish(s, &s->vq, &s->v_done);
    return false;
  }
  *rgba = s->next;
  if (pts_ms) *pts_ms = pts;
  if (dur_ms) *dur_ms = dur;
  if (out_already_premul) *out_already_premul = want_premul && s->last_frame_opaque;
  return true;
}

bool ccord_av_stream_next_frame(ccord_av_stream *s, const uint8_t **rgba, uint32_t *pts_ms, uint32_t *dur_ms)
{
  return av_stream_next_frame_ex(s, false, rgba, pts_ms, dur_ms, nullptr);
}

// ---- audio ----

static bool avs_pcm_reserve(ccord_av_stream *s, size_t extra)
{
  if (s->pcm_head > 0 && s->pcm_len == 0) s->pcm_head = 0;
  if (s->pcm_head + s->pcm_len + extra <= s->pcm_cap) return true;
  if (s->pcm_head > 0) {   // compact before growing
    memmove(s->pcm, s->pcm + s->pcm_head * 2u, s->pcm_len * 2u * sizeof *s->pcm);
    s->pcm_head = 0;
    if (s->pcm_len + extra <= s->pcm_cap) return true;
  }
  size_t ncap = s->pcm_cap ? s->pcm_cap : 9600u;
  while (ncap < s->pcm_len + extra) ncap *= 2u;
  int16_t *np = ccord_realloc_array(s->pcm, ncap * 2u, sizeof *np);
  if (!np) return false;
  s->pcm = np;
  s->pcm_cap = ncap;
  return true;
}

static bool avs_swr_tmp_reserve(ccord_av_stream *s, size_t frames)
{
  if (frames <= s->swr_tmp_cap) return true;
  size_t ncap = s->swr_tmp_cap ? s->swr_tmp_cap : 4096u;
  while (ncap < frames) ncap *= 2u;
  int16_t *np = ccord_realloc_array(s->swr_tmp, ncap * 2u, sizeof *np);
  if (!np) return false;
  s->swr_tmp = np;
  s->swr_tmp_cap = ncap;
  return true;
}

static void avs_pcm_commit(ccord_av_stream *s, size_t take, uint32_t pts, uint64_t skip)
{
  s->pcm_len += take;
  if (s->pcm_len == take) {   // FIFO was empty: its head sits where this frame (after any skip) starts
    uint32_t head_ms = pts + (uint32_t)(((uint64_t)skip * 1000u) / (uint64_t)CCORD_AV_SAMPLE_RATE);
    if (head_ms > s->pcm_pos_ms) {
      s->pcm_pos_ms = head_ms;
      s->pcm_pos_rem = 0;
    }
  }
  s->a_tail_ms = s->pcm_pos_ms + (uint32_t)((((uint64_t)s->pcm_len + s->pcm_pos_rem) * 1000u) /
                                            (uint64_t)CCORD_AV_SAMPLE_RATE);
}

// Resamples s->frame to 48 kHz stereo S16 (via fast-path memcpy or SIMD libswresample) and appends to FIFO,
// dropping anything before skip_before_ms.
static void avs_append_audio(ccord_av_stream *s)
{
  const AVFrame *f = s->frame;
  if (f->nb_samples <= 0 || f->sample_rate <= 0 || f->sample_rate > 384000) return;
  AVChannelLayout in_layout = f->ch_layout;
  bool owns_layout = false;
  if (in_layout.nb_channels <= 0 && s->adec && s->adec->ch_layout.nb_channels > 0) {
    in_layout = s->adec->ch_layout;
  }
  if (in_layout.nb_channels <= 0 || in_layout.nb_channels > 16) return;
  if (in_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
    int nch = in_layout.nb_channels;
    memset(&in_layout, 0, sizeof in_layout);
    av_channel_layout_default(&in_layout, nch);
    owns_layout = true;
  }
  int nb_ch = in_layout.nb_channels;
  uint32_t pts = s->a_tail_ms;
  if (f->best_effort_timestamp != AV_NOPTS_VALUE && s->meta.has_audio) {
    int64_t start = s->meta.a_start != AV_NOPTS_VALUE ? s->meta.a_start : 0;
    pts = avs_ms(av_rescale_q(f->best_effort_timestamp - start, s->meta.a_tb, (AVRational){1, 1000}));
  }

  // Zero-resample memcpy fast path when source is already 48 kHz stereo packed S16.
  if ((enum AVSampleFormat)f->format == AV_SAMPLE_FMT_S16 && f->sample_rate == CCORD_AV_SAMPLE_RATE && nb_ch == 2 &&
      f->extended_data && f->extended_data[0]) {
    if (s->swr) swr_free(&s->swr);
    s->swr_skip_frames = 0;
    uint64_t out_n = (uint64_t)f->nb_samples;
    uint64_t dur_ms = (out_n * 1000u) / (uint64_t)CCORD_AV_SAMPLE_RATE;
    uint64_t skip = 0;
    if ((uint64_t)pts + dur_ms <= (uint64_t)s->skip_before_ms) goto done;
    if (pts < s->skip_before_ms) {
      skip = ((uint64_t)(s->skip_before_ms - pts) * (uint64_t)CCORD_AV_SAMPLE_RATE) / 1000u;
      if (skip >= out_n) goto done;
    }
    uint64_t take64 = out_n - skip;
    if (take64 > (uint64_t)AVS_MAX_PCM_FRAMES || s->pcm_len > AVS_MAX_PCM_FRAMES - (size_t)take64) goto done;
    size_t take = (size_t)take64;
    if (!avs_pcm_reserve(s, take)) goto done;
    const int16_t *src_pcm = (const int16_t *)(const void *)f->extended_data[0];
    memcpy(s->pcm + (s->pcm_head + s->pcm_len) * 2u, src_pcm + skip * 2u, take * 2u * sizeof(int16_t));
    avs_pcm_commit(s, take, pts, skip);
    goto done;
  }

  uint64_t est_out = ((uint64_t)f->nb_samples * (uint64_t)CCORD_AV_SAMPLE_RATE + (uint64_t)f->sample_rate - 1u) /
                     (uint64_t)f->sample_rate;
  uint64_t est_dur_ms = (est_out * 1000u) / (uint64_t)CCORD_AV_SAMPLE_RATE;
  if (!s->swr && (uint64_t)pts + est_dur_ms <= (uint64_t)s->skip_before_ms) goto done;

  uint64_t in_mask = in_layout.order == AV_CHANNEL_ORDER_NATIVE ? in_layout.u.mask : 0u;
  if (s->swr && (f->format != s->swr_fmt || f->sample_rate != s->swr_rate || nb_ch != s->swr_ch ||
                 in_layout.order != s->swr_order || in_mask != s->swr_mask)) {
    swr_free(&s->swr);
  }
  if (!s->swr) {
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    int rc = swr_alloc_set_opts2(&s->swr, &out_layout, AV_SAMPLE_FMT_S16, CCORD_AV_SAMPLE_RATE, &in_layout,
                                 (enum AVSampleFormat)f->format, f->sample_rate, 0, nullptr);
    if (rc < 0 || !s->swr) goto done;
    if (nb_ch == 1) {
      // Preserve unity gain on mono -> stereo duplication (matches voice messages & WAV tests).
      const double mono_mat[2] = {1.0, 1.0};
      (void)swr_set_matrix(s->swr, mono_mat, 1);
    }
    if (swr_init(s->swr) < 0) {
      swr_free(&s->swr);
      goto done;
    }
    s->swr_fmt = f->format;
    s->swr_rate = f->sample_rate;
    s->swr_ch = nb_ch;
    s->swr_order = in_layout.order;
    s->swr_mask = in_mask;
    if (pts < s->skip_before_ms) {
      s->swr_skip_frames = ((uint64_t)(s->skip_before_ms - pts) * (uint64_t)CCORD_AV_SAMPLE_RATE) / 1000u;
    }
  }

  int64_t delay = swr_get_delay(s->swr, f->sample_rate);
  int64_t max_out = av_rescale_rnd(delay + f->nb_samples, CCORD_AV_SAMPLE_RATE, f->sample_rate, AV_ROUND_UP);
  if (max_out <= 0 || max_out > (int64_t)AVS_MAX_PCM_FRAMES) goto done;

  if (s->swr_skip_frames == 0) {
    if ((uint64_t)max_out > (uint64_t)AVS_MAX_PCM_FRAMES || s->pcm_len > AVS_MAX_PCM_FRAMES - (size_t)max_out) goto done;
    if (!avs_pcm_reserve(s, (size_t)max_out)) goto done;
    uint8_t *dst[1] = {(uint8_t *)(s->pcm + (s->pcm_head + s->pcm_len) * 2u)};
    int got = swr_convert(s->swr, dst, (int)max_out, (const uint8_t *const *)f->extended_data, f->nb_samples);
    if (got > 0) avs_pcm_commit(s, (size_t)got, pts, 0u);
  } else {
    if (!avs_swr_tmp_reserve(s, (size_t)max_out)) goto done;
    uint8_t *dst[1] = {(uint8_t *)s->swr_tmp};
    int got = swr_convert(s->swr, dst, (int)max_out, (const uint8_t *const *)f->extended_data, f->nb_samples);
    if (got > 0) {
      uint64_t drop = (uint64_t)got < s->swr_skip_frames ? (uint64_t)got : s->swr_skip_frames;
      s->swr_skip_frames -= drop;
      size_t take = (size_t)((uint64_t)got - drop);
      if (take > 0 && take <= AVS_MAX_PCM_FRAMES && s->pcm_len <= AVS_MAX_PCM_FRAMES - take && avs_pcm_reserve(s, take)) {
        memcpy(s->pcm + (s->pcm_head + s->pcm_len) * 2u, s->swr_tmp + drop * 2u, take * 2u * sizeof(int16_t));
        avs_pcm_commit(s, take, pts, drop);
      }
    }
  }
done:
  if (owns_layout) av_channel_layout_uninit(&in_layout);
}

// Drains any tail samples buffered inside libswresample's FIR filter when the audio decoder reaches EOF.
static void avs_flush_swr(ccord_av_stream *s)
{
  if (!s->swr) return;
  for (;;) {
    int64_t rem = swr_get_out_samples(s->swr, 0);
    int chunk = rem > 0 ? (rem > 4096 ? 4096 : (int)rem) : 256;
    if (s->swr_skip_frames == 0) {
      if ((size_t)chunk > AVS_MAX_PCM_FRAMES || s->pcm_len > AVS_MAX_PCM_FRAMES - (size_t)chunk) break;
      if (!avs_pcm_reserve(s, (size_t)chunk)) break;
      uint8_t *dst[1] = {(uint8_t *)(s->pcm + (s->pcm_head + s->pcm_len) * 2u)};
      int got = swr_convert(s->swr, dst, chunk, nullptr, 0);
      if (got <= 0) break;
      avs_pcm_commit(s, (size_t)got, s->a_tail_ms, 0u);
    } else {
      if (!avs_swr_tmp_reserve(s, (size_t)chunk)) break;
      uint8_t *dst[1] = {(uint8_t *)s->swr_tmp};
      int got = swr_convert(s->swr, dst, chunk, nullptr, 0);
      if (got <= 0) break;
      uint64_t drop = (uint64_t)got < s->swr_skip_frames ? (uint64_t)got : s->swr_skip_frames;
      s->swr_skip_frames -= drop;
      size_t take = (size_t)((uint64_t)got - drop);
      if (take > 0 && take <= AVS_MAX_PCM_FRAMES && s->pcm_len <= AVS_MAX_PCM_FRAMES - take && avs_pcm_reserve(s, take)) {
        memcpy(s->pcm + (s->pcm_head + s->pcm_len) * 2u, s->swr_tmp + drop * 2u, take * 2u * sizeof(int16_t));
        avs_pcm_commit(s, take, s->a_tail_ms, drop);
      }
    }
  }
  swr_free(&s->swr);
}

uint32_t ccord_av_stream_pcm_pos_ms(const ccord_av_stream *s)
{
  if (!s) return 0;
  return s->pcm_pos_ms + (uint32_t)((s->pcm_pos_rem * 1000u) / (uint64_t)CCORD_AV_SAMPLE_RATE);
}

size_t ccord_av_stream_read_pcm(ccord_av_stream *s, int16_t *out, size_t max_frames)
{
  if (!s || !out || max_frames == 0 || s->pcm_len == 0) return 0;
  s->last_used_ms = ccord_clock_ms();
  size_t n = max_frames < s->pcm_len ? max_frames : s->pcm_len;
  memcpy(out, s->pcm + s->pcm_head * 2u, n * 2u * sizeof *out);
  s->pcm_head += n;
  s->pcm_len -= n;
  s->pcm_pos_rem += n;
  uint64_t whole_ms = (s->pcm_pos_rem * 1000u) / (uint64_t)CCORD_AV_SAMPLE_RATE;
  s->pcm_pos_ms += (uint32_t)whole_ms;
  s->pcm_pos_rem -= (whole_ms * (uint64_t)CCORD_AV_SAMPLE_RATE) / 1000u;
  return n;
}

// ---- player ----

bool ccord_av_stream_sync(ccord_av_stream *s, uint32_t pos_ms, uint32_t budget_ms)
{
  if (!s) return true;
  s->last_used_ms = ccord_clock_ms();
  if (s->src_mode) {
    ccord_av_state st = avs_observe(s, nullptr);
    if (st != CCORD_AV_READY) {   // still opening: buffering. failed: nothing more will ever arrive
      s->starved = st == CCORD_AV_OPENING;
      return st == CCORD_AV_FAILED;
    }
    s->starved = false;
  }
  uint64_t t0 = budget_ms ? ccord_clock_ms() : 0;
  for (;;) {
    bool progressed = false;
    if (s->vdec) {
      if (!s->have_next && !s->v_done && avs_decode(s, s->vdec, &s->vq, &s->v_flushed, &s->v_done)) {
        uint32_t pts = 0, dur = 0;
        avs_video_times(s, &pts, &dur);
        uint32_t end = pts + (dur ? dur : 33u);
        // Far behind the playhead (after a seek, or a slow machine): keep decoding, skip the costly conversion.
        bool stale = end <= s->skip_before_ms || (end + AVS_STALE_MS <= pos_ms);
        if (!stale && avs_convert(s, &s->next)) {
          s->have_next = true;
          s->next_pts = pts;
          s->next_dur = dur;
        }
        avs_unref_video_frames(s);
        progressed = true;
      }
      if (s->have_next && (s->next_pts <= pos_ms || !s->have_cur)) {
        uint8_t *t = s->cur;
        s->cur = s->next;
        s->next = t;
        s->cur_pts = s->next_pts;
        s->have_cur = true;
        s->have_next = false;
        s->serial++;
        progressed = true;
      }
    }
    // A full FIFO means nobody is reading audio (no device): do not decode the rest of the file to throw it away.
    bool a_room = s->pcm_len + (size_t)CCORD_AV_SAMPLE_RATE <= AVS_MAX_PCM_FRAMES;
    if (s->adec && !s->a_done && a_room && s->a_tail_ms < pos_ms + AVS_AUDIO_LEAD_MS) {
      if (avs_decode(s, s->adec, &s->aq, &s->a_flushed, &s->a_done)) {
        avs_append_audio(s);
        av_frame_unref(s->frame);
      } else if (s->a_done) {
        avs_flush_swr(s);
      }
      progressed = true;
    }
    bool v_ok = !s->vdec || s->v_done || (s->have_next && s->next_pts > pos_ms);
    bool a_ok = !s->adec || s->a_done || !a_room || s->a_tail_ms >= pos_ms + AVS_AUDIO_READY_MS;
    if (v_ok && a_ok) {
      s->starved = false;
      return true;
    }
    if (s->starved) return false;   // waiting for the demux thread (never reported as caught up)
    if (!progressed) return true;
    if (budget_ms && ccord_clock_ms() - t0 >= budget_ms) return false;
  }
}

const uint8_t *ccord_av_stream_frame(const ccord_av_stream *s, uint32_t *serial)
{
  if (serial) *serial = s ? s->serial : 0;
  return s && s->have_cur ? s->cur : nullptr;
}

void ccord_av_stream_seek(ccord_av_stream *s, uint32_t pos_ms)
{
  if (!s) return;
  s->last_used_ms = ccord_clock_ms();
  if (s->lottie) {
    ccord_lottie_seek(s->lottie, pos_ms);
    return;
  }
  if (s->src_mode) {
    ccord_av_state st = avs_observe(s, nullptr);
    if (st == CCORD_AV_OPENING) {   // the decoders are not ours yet: apply the seek once READY is seen
      s->pre_seek = true;
      s->pre_seek_ms = pos_ms;
      return;
    }
    if (st == CCORD_AV_FAILED) return;
    int64_t ts = (int64_t)pos_ms * (AV_TIME_BASE / 1000);
    if (s->meta.fmt_start_time != AV_NOPTS_VALUE) ts += s->meta.fmt_start_time;
    // Post the request; the demux thread repositions. Packets of the old generation are dropped there.
    ccord_mutex_lock(s->mx);
    s->gen++;
    s->seek_ts = ts;
    s->seek_req = true;
    s->demux_eof = false;
    s->vq.dead = s->aq.dead = false;
    pktq_clear(&s->vq);
    pktq_clear(&s->aq);
    ccord_cond_broadcast(s->cv);
    ccord_mutex_unlock(s->mx);
    if (s->vdec) avcodec_flush_buffers(s->vdec);
    if (s->vdec_alpha) avcodec_flush_buffers(s->vdec_alpha);
    if (s->alpha_frame) av_frame_unref(s->alpha_frame);
    s->have_alpha_frame = false;
    if (s->adec) avcodec_flush_buffers(s->adec);
    s->v_flushed = s->a_flushed = false;
    s->v_done = s->vsi < 0;
    s->a_done = s->asi < 0;
    s->starved = true;
    avs_reset_playhead(s, pos_ms);
    return;
  }
  if (!s->vdec && !s->adec) return;   // memory mode after a failed re-open: nothing left to seek
  int64_t ts = (int64_t)pos_ms * (AV_TIME_BASE / 1000);
  if (s->meta.fmt_start_time != AV_NOPTS_VALUE) ts += s->meta.fmt_start_time;
  bool ok = avs_seek_demuxer(s, ts) >= 0;
  if (ok) {
    if (s->vdec) avcodec_flush_buffers(s->vdec);
    if (s->vdec_alpha) avcodec_flush_buffers(s->vdec_alpha);
    if (s->alpha_frame) av_frame_unref(s->alpha_frame);
    s->have_alpha_frame = false;
    if (s->adec) avcodec_flush_buffers(s->adec);
    pktq_clear(&s->vq);
    pktq_clear(&s->aq);
    s->demux_eof = s->v_flushed = s->a_flushed = false;
    s->v_done = s->vsi < 0;
    s->a_done = s->asi < 0;
  } else {
    // The demuxer cannot seek (GIF, some streams): start over and decode forward to the target.
    ccord_error err;
    avs_teardown(s);
    if (avs_setup(s, &err) != CCORD_OK) {
      ccord_error_clear(&err);
      avs_teardown(s);
      s->v_done = s->a_done = true;
    }
  }
  avs_reset_playhead(s, pos_ms);
}

bool ccord_av_stream_ended(const ccord_av_stream *s)
{
  if (!s) return true;
  if (s->src_mode) {
    ccord_av_state st = avs_observe(s, nullptr);
    if (st != CCORD_AV_READY) return st == CCORD_AV_FAILED;
  }
  return (!s->vdec || (s->v_done && !s->have_next)) && (!s->adec || (s->a_done && s->pcm_len == 0));
}

uint64_t av_stream_last_used_ms(const ccord_av_stream *s)
{
  return s ? s->last_used_ms : 0;
}

void av_stream_touch(ccord_av_stream *s, uint64_t now_ms)
{
  if (s) s->last_used_ms = now_ms;
}

size_t av_stream_mem_bytes(const ccord_av_stream *s)
{
  if (!s) return 0;
  if (s->lottie) return ccord_lottie_mem_bytes(s->lottie);
  size_t total = s->mb.len;
  size_t fbytes = (s->sw > 0 && s->sh > 0) ? (size_t)s->sw * (size_t)s->sh * 4u : 0;
  if (s->cur) total += fbytes;
  if (s->next) total += fbytes;
  if (s->scratch) total += fbytes;
  if (s->alpha_scaled && s->sw > 0 && s->sh > 0) total += (size_t)s->sw * (size_t)s->sh;
  total += s->pcm_cap * 2u * sizeof(int16_t);
  total += s->swr_tmp_cap * 2u * sizeof(int16_t);
  if (s->src_mode && s->mx) {
    ccord_mutex_lock(s->mx);
    total += s->vq.bytes + s->aq.bytes;
    ccord_mutex_unlock(s->mx);
  } else {
    total += s->vq.bytes + s->aq.bytes;
  }
  return total;
}
