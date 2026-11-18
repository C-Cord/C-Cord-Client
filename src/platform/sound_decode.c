// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Soundboard clip decoder: probes MP3 / OGG-Vorbis / WAV from memory (libavformat), decodes
// with libavcodec and resamples to 48 kHz stereo S16LE with libswresample.
#define _POSIX_C_SOURCE 200809L
#include <ccord/platform/platform.h>

#include <limits.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>

#include <ccord/core/alloc.h>
#include <ccord/core/error.h>

#define SOUND_RATE 48000
#define SOUND_MAX_INPUT (16u * 1024u * 1024u)
#define SOUND_MAX_SECONDS 30u
#define SOUND_MAX_CHANNELS 8
#define SOUND_MAX_SAMPLE_RATE 192000
#define SOUND_IO_BUF 4096
// A single decoded frame longer than this is treated as malformed (guards resampler maths).
#define SOUND_MAX_FRAME_SECONDS 8

typedef struct sound_mem {
  const uint8_t *data;
  size_t len;
  size_t pos;
} sound_mem;

typedef struct sound_ctx {
  SwrContext *swr;
  int swr_format;
  int swr_rate;
  int swr_channels;
  int16_t *out;
  size_t cap;
  size_t written;
  size_t frames;
  bool truncated;
} sound_ctx;

static int sound_mem_read(void *opaque, uint8_t *buf, int buf_size)
{
  sound_mem *m = opaque;
  if (buf_size <= 0) return AVERROR(EINVAL);
  size_t remain = m->len - m->pos;
  size_t n = (size_t)buf_size < remain ? (size_t)buf_size : remain;
  if (n == 0) return AVERROR_EOF;
  memcpy(buf, m->data + m->pos, n);
  m->pos += n;
  return (int)n;
}

static ccord_status convert_frame(sound_ctx *c, AVCodecContext *dec, AVFrame *frame, ccord_error *err)
{
  if (frame->nb_samples <= 0) return CCORD_OK;
  int in_rate = frame->sample_rate > 0 ? frame->sample_rate : dec->sample_rate;
  if (in_rate <= 0 || in_rate > SOUND_MAX_SAMPLE_RATE) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "sound decode: invalid sample rate");
  }
  if (frame->nb_samples > in_rate * SOUND_MAX_FRAME_SECONDS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "sound decode: oversized audio frame");
  }
  AVChannelLayout in_layout = frame->ch_layout;
  if (in_layout.nb_channels <= 0) {
    int n = dec->ch_layout.nb_channels > 0 ? dec->ch_layout.nb_channels : 1;
    av_channel_layout_default(&in_layout, n);
  }
  if (in_layout.nb_channels <= 0 || in_layout.nb_channels > SOUND_MAX_CHANNELS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "sound decode: unsupported channel layout");
  }
  int in_ch = in_layout.nb_channels;
  if (c->swr && (frame->format != c->swr_format || in_rate != c->swr_rate || in_ch != c->swr_channels)) {
    swr_free(&c->swr);
  }
  if (!c->swr) {
    c->swr_format = frame->format;
    c->swr_rate = in_rate;
    c->swr_channels = in_ch;
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    int rc = swr_alloc_set_opts2(&c->swr, &out_layout, AV_SAMPLE_FMT_S16, SOUND_RATE, &in_layout,
                                 (enum AVSampleFormat)frame->format, in_rate, 0, nullptr);
    if (rc < 0 || !c->swr) return ccord_error_set(err, CCORD_ERR_NOMEM, "sound decode: resampler alloc failed");
    if (swr_init(c->swr) < 0) return ccord_error_set(err, CCORD_ERR_PARSE, "sound decode: unsupported sample format");
  }
  int64_t delay = swr_get_delay(c->swr, in_rate);
  int64_t max_out = av_rescale_rnd(delay + frame->nb_samples, SOUND_RATE, in_rate, AV_ROUND_UP);
  if (max_out <= 0 || max_out > (int64_t)SOUND_RATE * SOUND_MAX_FRAME_SECONDS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "sound decode: resampler output overflow");
  }
  size_t tmp_bytes = (size_t)max_out * 2u * sizeof(int16_t);
  uint8_t *tmp = ccord_malloc(tmp_bytes);
  if (!tmp) return ccord_error_set(err, CCORD_ERR_NOMEM, "sound decode: out of memory");
  uint8_t *dst[1] = {tmp};
  int got = swr_convert(c->swr, dst, (int)max_out, (const uint8_t *const *)frame->extended_data,
                        frame->nb_samples);
  if (got < 0) {
    ccord_free(tmp);
    return ccord_error_set(err, CCORD_ERR_PARSE, "sound decode: resample failed");
  }
  size_t samples = (size_t)got * 2u;
  size_t remaining = c->cap - c->written;
  if (samples > remaining) {
    memcpy(c->out + c->written, tmp, remaining * sizeof(int16_t));
    c->written += remaining;
    c->truncated = true;
  } else {
    memcpy(c->out + c->written, tmp, samples * sizeof(int16_t));
    c->written += samples;
  }
  ccord_free(tmp);
  c->frames += (size_t)got;
  if (c->frames > (size_t)SOUND_RATE * SOUND_MAX_SECONDS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "sound decode: clip too long");
  }
  return CCORD_OK;
}

static ccord_status drain_audio(sound_ctx *c, AVCodecContext *dec, AVFrame *frame, ccord_error *err)
{
  for (;;) {
    int rc = avcodec_receive_frame(dec, frame);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return CCORD_OK;
    if (rc < 0) return ccord_error_set(err, CCORD_ERR_PARSE, "sound decode: corrupt audio frame");
    ccord_status st = convert_frame(c, dec, frame, err);
    av_frame_unref(frame);
    if (st != CCORD_OK) return st;
    if (c->truncated) return ccord_error_set(err, CCORD_ERR_RANGE, "sound decode: output exceeds capacity");
  }
}

ccord_status ccord_sound_decode(const uint8_t *data, size_t len, int16_t *out, size_t cap, size_t *out_samples,
                                ccord_error *err)
{
  ccord_error_clear(err);
  if (out_samples) *out_samples = 0;
  if (!data || len == 0 || !out || cap == 0 || !out_samples) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "sound decode: invalid argument");
  }
  if (len > SOUND_MAX_INPUT) return ccord_error_set(err, CCORD_ERR_RANGE, "sound decode: input too large");
  if (av_log_get_level() > AV_LOG_ERROR) av_log_set_level(AV_LOG_ERROR);

  sound_mem mb = {.data = data, .len = len, .pos = 0};
  sound_ctx c = {.out = out, .cap = cap};
  AVFormatContext *fmt = nullptr;
  AVIOContext *io = nullptr;
  AVCodecContext *dec = nullptr;
  AVFrame *frame = nullptr;
  AVPacket *pkt = nullptr;
  AVDictionary *opts = nullptr;
  ccord_status st = CCORD_OK;

  uint8_t *iobuf = av_malloc(SOUND_IO_BUF);
  if (!iobuf) return ccord_error_set(err, CCORD_ERR_NOMEM, "sound decode: out of memory");
  io = avio_alloc_context(iobuf, SOUND_IO_BUF, 0, &mb, sound_mem_read, nullptr, nullptr);
  if (!io) {
    av_free(iobuf);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "sound decode: out of memory");
  }
  fmt = avformat_alloc_context();
  if (!fmt) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "sound decode: out of memory");
    goto done;
  }
  fmt->pb = io;
  fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  av_dict_set(&opts, "format_whitelist", "mp3,ogg,wav", 0);
  av_dict_set(&opts, "protocol_whitelist", "", 0);
  if (avformat_open_input(&fmt, "", nullptr, &opts) < 0) {
    fmt = nullptr;
    st = ccord_error_set(err, CCORD_ERR_PARSE, "sound decode: unrecognized audio container");
    goto done;
  }
  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "sound decode: cannot read stream info");
    goto done;
  }
  const AVCodec *codec = nullptr;
  int si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (si < 0 || !codec) {
    st = ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "sound decode: no audio stream");
    goto done;
  }
  AVStream *stream = fmt->streams[si];
  if (stream->codecpar->sample_rate <= 0 || stream->codecpar->sample_rate > SOUND_MAX_SAMPLE_RATE) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "sound decode: invalid sample rate");
    goto done;
  }
  if (stream->codecpar->ch_layout.nb_channels <= 0 ||
      stream->codecpar->ch_layout.nb_channels > SOUND_MAX_CHANNELS) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "sound decode: unsupported channel layout");
    goto done;
  }
  dec = avcodec_alloc_context3(codec);
  if (!dec) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "sound decode: out of memory");
    goto done;
  }
  if (avcodec_parameters_to_context(dec, stream->codecpar) < 0) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "sound decode: invalid codec parameters");
    goto done;
  }
  dec->pkt_timebase = stream->time_base;
  dec->thread_count = 1;
  if (avcodec_open2(dec, codec, nullptr) < 0) {
    st = ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "sound decode: avcodec_open2 failed");
    goto done;
  }
  frame = av_frame_alloc();
  pkt = av_packet_alloc();
  if (!frame || !pkt) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "sound decode: out of memory");
    goto done;
  }

  while (av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == si) {
      int rc = avcodec_send_packet(dec, pkt);
      if (rc < 0) {
        av_packet_unref(pkt);
        st = ccord_error_set(err, CCORD_ERR_PARSE, "sound decode: corrupt audio packet");
        goto done;
      }
      st = drain_audio(&c, dec, frame, err);
      if (st != CCORD_OK) {
        av_packet_unref(pkt);
        goto done;
      }
    }
    av_packet_unref(pkt);
  }
  if (avcodec_send_packet(dec, nullptr) >= 0) st = drain_audio(&c, dec, frame, err);
  if (st == CCORD_OK && c.written == 0) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "sound decode: clip produced no samples");
  }

done:
  av_dict_free(&opts);
  swr_free(&c.swr);
  av_packet_free(&pkt);
  av_frame_free(&frame);
  avcodec_free_context(&dec);
  if (fmt) avformat_close_input(&fmt);
  if (io) {
    av_freep(&io->buffer);
    avio_context_free(&io);
  }
  *out_samples = c.written;
  return st;
}
