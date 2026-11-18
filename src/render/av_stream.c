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
#include <ccord/render/av_stream.h>

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/cpu.h>
#include <libavutil/dict.h>
#include <libavutil/display.h>
#include <libavutil/samplefmt.h>
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
  uint8_t *data;
  avs_membuf mb;
  unsigned flags;
  int max_dim;
  AVFormatContext *fmt;
  AVIOContext *io;
  AVCodecContext *vdec, *adec;
  int vsi, asi;
  AVFrame *frame;
  AVPacket *pkt;
  avs_pktq vq, aq;
  struct SwsContext *sws;
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
  pthread_t thr;
  bool thr_started, sync_init;
  pthread_mutex_t mx;
  pthread_cond_t cv;
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
  av_packet_free(&s->pkt);
  av_frame_free(&s->frame);
  avcodec_free_context(&s->vdec);
  avcodec_free_context(&s->adec);
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
    // Frame threads only for real video: they cost latency and memory that a GIF never earns back.
    int threads = 1;
    if (s->flags & CCORD_AV_MEDIA) {
      threads = av_cpu_count() / 2;
      if (threads < 1) threads = 1;
      if (threads > 4) threads = 4;
    }
    s->vdec = avs_open_codec(vcodec, vs, threads);
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
  pthread_cond_broadcast(&s->cv);
}

static void *avs_demux_main(void *arg)
{
  ccord_av_stream *s = arg;
  ccord_error err;
  ccord_error_clear(&err);
  ccord_status st = avs_setup(s, &err);
  pthread_mutex_lock(&s->mx);
  if (st == CCORD_OK && !atomic_load(&s->stop)) {
    s->src_err = CCORD_OK;
    s->state = CCORD_AV_READY;
    pthread_cond_broadcast(&s->cv);
  } else {
    if (st == CCORD_OK) ccord_error_set(&err, CCORD_ERR_IO, "av_stream: closed while opening");
    avs_fail(s, err.code, err.message);
    pthread_mutex_unlock(&s->mx);
    avs_teardown(s);   // the player never saw these objects; ccord_av_stream_close finds them already freed
    return nullptr;
  }
  while (!atomic_load(&s->stop)) {
    while (!atomic_load(&s->stop) && avs_demux_idle(s)) pthread_cond_wait(&s->cv, &s->mx);
    if (atomic_load(&s->stop)) break;
    if (s->seek_req) {
      int64_t ts = s->seek_ts;
      s->seek_req = false;
      s->demux_eof = false;
      pthread_mutex_unlock(&s->mx);
      int r = avs_seek_demuxer(s, ts);
      pthread_mutex_lock(&s->mx);
      if (r < 0) {
        if (s->src_err != CCORD_OK) avs_fail(s, s->src_err, "av_stream: source read failed");
        else avs_fail(s, CCORD_ERR_UNSUPPORTED, "av_stream: cannot seek");
      }
      if (s->state == CCORD_AV_FAILED) break;
      continue;
    }
    uint64_t my_gen = s->gen;   // re-read every iteration: a packet read under an older value is stale
    pthread_mutex_unlock(&s->mx);
    int r = avs_read_one(s);
    pthread_mutex_lock(&s->mx);
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
      pthread_cond_broadcast(&s->cv);
    }
  }
  pthread_mutex_unlock(&s->mx);
  return nullptr;
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
  s->sync_init = pthread_mutex_init(&s->mx, nullptr) == 0;
  if (s->sync_init && pthread_cond_init(&s->cv, nullptr) != 0) {
    pthread_mutex_destroy(&s->mx);
    s->sync_init = false;
  }
  if (!s->sync_init || pthread_create(&s->thr, nullptr, avs_demux_main, s) != 0) {
    ccord_error_set(err, CCORD_ERR_INTERNAL, "av_stream: cannot start the demux thread");
    ccord_av_stream_close(s);
    return nullptr;
  }
  s->thr_started = true;
  return s;
}

// UI thread: the stream's state; the first READY copies the demux thread's snapshot into info.
static ccord_av_state avs_observe(const ccord_av_stream *cs, ccord_error *err)
{
  ccord_av_stream *s = cs->self;
  if (!s->src_mode) return CCORD_AV_READY;
  pthread_mutex_lock(&s->mx);
  ccord_av_state st = s->state;
  if (err && st == CCORD_AV_FAILED) *err = s->serr;
  pthread_mutex_unlock(&s->mx);
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
  if (s->thr_started) {
    // Wake a read_at that is blocked for good, tell the thread to stop, and wait for it: only then is nothing
    // using fmt/io/the source any more and the teardown below race-free.
    if (s->src.cancel) s->src.cancel(s->src.ud);
    pthread_mutex_lock(&s->mx);
    atomic_store(&s->stop, true);
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mx);
    pthread_join(s->thr, nullptr);
  }
  if (s->sync_init) {
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mx);
  }
  avs_teardown(s);
  ccord_free(s->vq.v);
  ccord_free(s->aq.v);
  ccord_free(s->scratch);
  ccord_free(s->cur);
  ccord_free(s->next);
  ccord_free(s->pcm);
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
  pthread_mutex_lock(&s->mx);
  q->dead = true;
  pktq_clear(q);
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->mx);
}

// Source mode: pops the next packet of q. *eof is true when the queue is empty and the demuxer is at its end.
static AVPacket *avs_take(ccord_av_stream *s, avs_pktq *q, bool *eof)
{
  pthread_mutex_lock(&s->mx);
  AVPacket *p = pktq_pop(q);
  *eof = s->demux_eof;
  if (p) pthread_cond_broadcast(&s->cv);   // room for the demux thread
  pthread_mutex_unlock(&s->mx);
  return p;
}

// Receives the next frame of one decoder into s->frame. False at end of stream (sets *done), or, in source mode,
// when the demux thread has not produced the packets yet (sets s->starved; *done and the drain stay untouched).
static bool avs_decode(ccord_av_stream *s, AVCodecContext *dec, avs_pktq *q, bool *flushed, bool *done)
{
  if (!dec || *done) return false;
  for (;;) {
    int r = avcodec_receive_frame(dec, s->frame);
    if (r == 0) return true;
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
      continue;
    }
    (void)avcodec_send_packet(dec, p);   // a corrupt packet is skipped; the decoder resyncs on the next keyframe
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
  s->sws = sws_getCachedContext(s->sws, f->width, f->height, (enum AVPixelFormat)f->format, s->sw, s->sh,
                                AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!s->sws) return false;
  uint8_t *target = s->rot != 0 ? s->scratch : *dst;
  uint8_t *planes[4] = {target, nullptr, nullptr, nullptr};
  int strides[4] = {s->sw * 4, 0, 0, 0};
  if (sws_scale(s->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, planes, strides) != s->sh) {
    return false;
  }
  if (s->rot != 0) avs_rotate(s->scratch, s->sw, s->sh, *dst, s->rot);
  return true;
}

bool ccord_av_stream_next_frame(ccord_av_stream *s, const uint8_t **rgba, uint32_t *pts_ms, uint32_t *dur_ms)
{
  if (rgba) *rgba = nullptr;
  if (!s || !rgba) return false;
  if (avs_observe(s, nullptr) != CCORD_AV_READY) return false;
  if (!avs_decode(s, s->vdec, &s->vq, &s->v_flushed, &s->v_done)) return false;
  uint32_t pts = 0, dur = 0;
  avs_video_times(s, &pts, &dur);
  bool ok = avs_convert(s, &s->next);
  av_frame_unref(s->frame);
  if (!ok) {
    avs_finish(s, &s->vq, &s->v_done);
    return false;
  }
  *rgba = s->next;
  if (pts_ms) *pts_ms = pts;
  if (dur_ms) *dur_ms = dur;
  return true;
}

// ---- audio ----

static float avs_sample(const AVFrame *f, int ch, int idx)
{
  enum AVSampleFormat fmt = (enum AVSampleFormat)f->format;
  int nb_ch = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : 1;
  if (ch >= nb_ch) ch = 0;
  bool planar = av_sample_fmt_is_planar(fmt) != 0;
  const uint8_t *plane = planar ? f->extended_data[ch] : f->extended_data[0];
  if (!plane) return 0.0f;
  size_t off = planar ? (size_t)idx : ((size_t)idx * (size_t)nb_ch + (size_t)ch);
  switch (av_get_packed_sample_fmt(fmt)) {
    case AV_SAMPLE_FMT_U8: return ((float)plane[off] - 128.0f) / 128.0f;
    case AV_SAMPLE_FMT_S16: {
      int16_t v = 0;
      memcpy(&v, plane + off * sizeof v, sizeof v);
      return (float)v / 32768.0f;
    }
    case AV_SAMPLE_FMT_S32: {
      int32_t v = 0;
      memcpy(&v, plane + off * sizeof v, sizeof v);
      return (float)((double)v / 2147483648.0);
    }
    case AV_SAMPLE_FMT_FLT: {
      float v = 0.0f;
      memcpy(&v, plane + off * sizeof v, sizeof v);
      return isfinite(v) ? v : 0.0f;
    }
    case AV_SAMPLE_FMT_DBL: {
      double v = 0.0;
      memcpy(&v, plane + off * sizeof v, sizeof v);
      return isfinite(v) ? (float)v : 0.0f;
    }
    default: return 0.0f;
  }
}

static int16_t avs_s16(float x)
{
  if (x >= 1.0f) return 32767;
  if (x <= -1.0f) return -32768;
  float v = x * 32767.0f;
  return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

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

// Resamples s->frame to 48 kHz stereo S16 and appends it to the FIFO, dropping anything before skip_before_ms.
static void avs_append_audio(ccord_av_stream *s)
{
  const AVFrame *f = s->frame;
  if (f->nb_samples <= 0 || f->sample_rate <= 0 || f->sample_rate > 384000 || f->ch_layout.nb_channels <= 0) return;
  int nb_ch = f->ch_layout.nb_channels;
  uint32_t pts = s->a_tail_ms;
  if (f->best_effort_timestamp != AV_NOPTS_VALUE && s->meta.has_audio) {
    int64_t start = s->meta.a_start != AV_NOPTS_VALUE ? s->meta.a_start : 0;
    pts = avs_ms(av_rescale_q(f->best_effort_timestamp - start, s->meta.a_tb, (AVRational){1, 1000}));
  }
  uint64_t out_n = ((uint64_t)f->nb_samples * (uint64_t)CCORD_AV_SAMPLE_RATE + (uint64_t)f->sample_rate - 1u) /
                   (uint64_t)f->sample_rate;
  uint32_t dur_ms = (uint32_t)((out_n * 1000u) / (uint64_t)CCORD_AV_SAMPLE_RATE);
  size_t skip = 0;
  if (pts + dur_ms <= s->skip_before_ms) return;
  if (pts < s->skip_before_ms) {
    skip = (size_t)(((uint64_t)(s->skip_before_ms - pts) * (uint64_t)CCORD_AV_SAMPLE_RATE) / 1000u);
    if (skip >= out_n) return;
  }
  size_t take = (size_t)out_n - skip;
  if (s->pcm_len + take > AVS_MAX_PCM_FRAMES) return;   // nobody is reading: drop instead of growing forever
  if (!avs_pcm_reserve(s, take)) return;
  double step = (double)f->sample_rate / (double)CCORD_AV_SAMPLE_RATE;
  int16_t *dst = s->pcm + (s->pcm_head + s->pcm_len) * 2u;
  for (size_t i = 0; i < take; i++) {
    double src_pos = (double)(i + skip) * step;
    int i0 = (int)src_pos;
    if (i0 >= f->nb_samples) i0 = f->nb_samples - 1;
    int i1 = i0 + 1 < f->nb_samples ? i0 + 1 : i0;
    float frac = (float)(src_pos - (double)i0);
    float l0 = avs_sample(f, 0, i0), l1 = avs_sample(f, 0, i1);
    float r0 = nb_ch > 1 ? avs_sample(f, 1, i0) : l0;
    float r1 = nb_ch > 1 ? avs_sample(f, 1, i1) : l1;
    dst[i * 2u] = avs_s16(l0 + (l1 - l0) * frac);
    dst[i * 2u + 1u] = avs_s16(r0 + (r1 - r0) * frac);
  }
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

uint32_t ccord_av_stream_pcm_pos_ms(const ccord_av_stream *s)
{
  if (!s) return 0;
  return s->pcm_pos_ms + (uint32_t)((s->pcm_pos_rem * 1000u) / (uint64_t)CCORD_AV_SAMPLE_RATE);
}

size_t ccord_av_stream_read_pcm(ccord_av_stream *s, int16_t *out, size_t max_frames)
{
  if (!s || !out || max_frames == 0 || s->pcm_len == 0) return 0;
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
        av_frame_unref(s->frame);
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
    pthread_mutex_lock(&s->mx);
    s->gen++;
    s->seek_ts = ts;
    s->seek_req = true;
    s->demux_eof = false;
    s->vq.dead = s->aq.dead = false;
    pktq_clear(&s->vq);
    pktq_clear(&s->aq);
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mx);
    if (s->vdec) avcodec_flush_buffers(s->vdec);
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
