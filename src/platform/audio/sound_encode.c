// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Voice-message encoder: streaming interleaved S16 PCM -> Ogg/Opus (libavcodec libopus encoder, libavformat Ogg
// muxer writing to a memory AVIOContext). Mirrors sound_decode.c: no temp file, the whole Ogg stays in memory
// until ccord_sound_encoder_finish hands it over.
#define _POSIX_C_SOURCE 200809L
#include <ccord/platform/platform.h>

#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include <ccord/core/alloc.h>
#include <ccord/core/error.h>

// Discord plays Opus at 48 kHz; the Ogg header advertises it and the decoder always outputs it.
#define ENC_RATE 48000u
#define ENC_FRAME_MS 20
// Per channel: 32 kbps mono is plenty for speech and keeps a 10 minute message around 2.4 MB.
#define ENC_BITRATE_PER_CHANNEL 32000
// Voice messages are capped at 10 minutes.
#define ENC_MAX_FRAMES ((uint64_t)ENC_RATE * 600u)

struct ccord_sound_encoder {
  AVCodecContext *codec;
  AVFormatContext *fmt;  // fmt->pb is a dynamic buffer until finish/destroy closes it
  AVStream *stream;
  AVFrame *frame;        // persistent S16 frame reused across encode_chunk() calls
  AVPacket *pkt;
  int16_t *pending;      // partial codec frame carried between feeds (frame_size * channels samples)
  size_t pending_frames;
  size_t frame_size;     // samples per channel in one 20 ms codec frame
  uint32_t channels;
  uint64_t total_frames; // frames accepted so far, bounded by ENC_MAX_FRAMES
  int64_t next_pts;      // in 1/ENC_RATE units
  bool failed;           // a libav call failed; the Ogg stream is unusable
  bool finished;
};

// Muxes every packet the codec has ready; EAGAIN/EOF mean it has no more for now.
static ccord_status drain_packets(ccord_sound_encoder *e, ccord_error *err)
{
  for (;;) {
    int rc = avcodec_receive_packet(e->codec, e->pkt);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return CCORD_OK;
    if (rc < 0) {
      e->failed = true;
      return ccord_error_set(err, CCORD_ERR_INTERNAL, "sound encode: Opus encoder failed");
    }
    e->pkt->stream_index = e->stream->index;
    av_packet_rescale_ts(e->pkt, e->codec->time_base, e->stream->time_base);
    rc = av_interleaved_write_frame(e->fmt, e->pkt);
    av_packet_unref(e->pkt);
    if (rc < 0) {
      e->failed = true;
      return ccord_error_set(err, CCORD_ERR_INTERNAL, "sound encode: Ogg mux failed");
    }
  }
}

// Sends `nb` interleaved frames (at most one codec frame) to the codec and muxes the resulting packets.
static ccord_status encode_chunk(ccord_sound_encoder *e, const int16_t *pcm, size_t nb, ccord_error *err)
{
  AVFrame *f = e->frame;
  if (av_frame_make_writable(f) < 0) {
    e->failed = true;
    return ccord_error_set(err, CCORD_ERR_NOMEM, "sound encode: out of memory");
  }
  f->nb_samples = (int)nb;
  memcpy(f->data[0], pcm, nb * e->channels * sizeof(int16_t));
  f->pts = e->next_pts;
  e->next_pts += (int64_t)nb;
  int rc = avcodec_send_frame(e->codec, f);
  if (rc < 0) {
    e->failed = true;
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "sound encode: Opus encoder rejected audio");
  }
  return drain_packets(e, err);
}

ccord_sound_encoder *ccord_sound_encoder_create(uint32_t sample_rate, uint32_t channels, ccord_error *err)
{
  ccord_error_clear(err);
  if (sample_rate != ENC_RATE) {
    ccord_error_set(err, CCORD_ERR_INVALID, "sound encode: sample rate must be %u Hz", ENC_RATE);
    return nullptr;
  }
  if (channels != 1u && channels != 2u) {
    ccord_error_set(err, CCORD_ERR_INVALID, "sound encode: channels must be 1 or 2");
    return nullptr;
  }
  const AVCodec *codec = avcodec_find_encoder_by_name("libopus");
  const AVOutputFormat *ofmt = av_guess_format("opus", nullptr, nullptr);
  if (!ofmt) ofmt = av_guess_format("ogg", nullptr, nullptr);
  if (!codec || !ofmt) {
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "sound encode: this build has no Opus encoder or Ogg muxer");
    return nullptr;
  }
  if (av_log_get_level() > AV_LOG_ERROR) av_log_set_level(AV_LOG_ERROR);

  ccord_sound_encoder *e = ccord_calloc(1, sizeof *e);
  if (!e) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "sound encode: out of memory");
    return nullptr;
  }
  e->channels = channels;

  e->codec = avcodec_alloc_context3(codec);
  e->pkt = av_packet_alloc();
  if (!e->codec || !e->pkt || avformat_alloc_output_context2(&e->fmt, ofmt, nullptr, nullptr) < 0 || !e->fmt) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "sound encode: out of memory");
    goto fail;
  }
  e->codec->sample_fmt = AV_SAMPLE_FMT_S16;
  e->codec->sample_rate = (int)ENC_RATE;
  e->codec->time_base = (AVRational){1, (int)ENC_RATE};
  e->codec->bit_rate = (int64_t)ENC_BITRATE_PER_CHANNEL * channels;
  e->codec->flags |= AV_CODEC_FLAG_BITEXACT;  // no encoder/version string in OpusTags, reproducible output
  av_channel_layout_default(&e->codec->ch_layout, (int)channels);
  if (e->fmt->oformat->flags & AVFMT_GLOBALHEADER) e->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  av_opt_set(e->codec->priv_data, "application", "voip", 0);
  av_opt_set_double(e->codec->priv_data, "frame_duration", ENC_FRAME_MS, 0);
  if (avcodec_open2(e->codec, codec, nullptr) < 0 || e->codec->frame_size <= 0) {
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "sound encode: Opus encoder could not be opened");
    goto fail;
  }
  e->frame_size = (size_t)e->codec->frame_size;
  e->frame = av_frame_alloc();
  e->pending = ccord_malloc(e->frame_size * channels * sizeof(int16_t));
  e->stream = avformat_new_stream(e->fmt, nullptr);
  if (!e->frame || !e->pending || !e->stream) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "sound encode: out of memory");
    goto fail;
  }
  e->frame->nb_samples = e->codec->frame_size;
  e->frame->format = AV_SAMPLE_FMT_S16;
  e->frame->sample_rate = (int)ENC_RATE;
  if (av_channel_layout_copy(&e->frame->ch_layout, &e->codec->ch_layout) < 0 ||
      av_frame_get_buffer(e->frame, 0) < 0) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "sound encode: out of memory");
    goto fail;
  }
  if (avcodec_parameters_from_context(e->stream->codecpar, e->codec) < 0) {
    ccord_error_set(err, CCORD_ERR_INTERNAL, "sound encode: cannot describe the Opus stream");
    goto fail;
  }
  e->stream->time_base = e->codec->time_base;
  e->fmt->flags |= AVFMT_FLAG_BITEXACT;
  if (avio_open_dyn_buf(&e->fmt->pb) < 0) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "sound encode: out of memory");
    goto fail;
  }
  if (avformat_write_header(e->fmt, nullptr) < 0) {
    ccord_error_set(err, CCORD_ERR_INTERNAL, "sound encode: cannot write the Ogg header");
    goto fail;
  }
  return e;

fail:
  ccord_sound_encoder_destroy(e);
  return nullptr;
}

ccord_status ccord_sound_encoder_feed(ccord_sound_encoder *e, const int16_t *pcm, size_t frames, ccord_error *err)
{
  ccord_error_clear(err);
  if (!e || (frames > 0 && !pcm)) return ccord_error_set(err, CCORD_ERR_INVALID, "sound encode: invalid argument");
  if (e->finished) return ccord_error_set(err, CCORD_ERR_INVALID, "sound encode: encoder already finished");
  if (e->failed) return ccord_error_set(err, CCORD_ERR_INTERNAL, "sound encode: encoder failed earlier");
  // Rejected whole, before touching `pcm`, so the caller can still finish what was recorded so far.
  if (frames > ENC_MAX_FRAMES - e->total_frames) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "sound encode: voice message exceeds 10 minutes");
  }
  e->total_frames += frames;

  const size_t ch = e->channels;
  while (frames > 0) {
    if (e->pending_frames == 0 && frames >= e->frame_size) {
      ccord_status st = encode_chunk(e, pcm, e->frame_size, err);
      if (st != CCORD_OK) return st;
      pcm += e->frame_size * ch;
      frames -= e->frame_size;
      continue;
    }
    size_t take = e->frame_size - e->pending_frames;
    if (take > frames) take = frames;
    memcpy(e->pending + e->pending_frames * ch, pcm, take * ch * sizeof(int16_t));
    e->pending_frames += take;
    pcm += take * ch;
    frames -= take;
    if (e->pending_frames == e->frame_size) {
      ccord_status st = encode_chunk(e, e->pending, e->frame_size, err);
      e->pending_frames = 0;
      if (st != CCORD_OK) return st;
    }
  }
  return CCORD_OK;
}

ccord_status ccord_sound_encoder_finish(ccord_sound_encoder *e, uint8_t **out, size_t *out_len, ccord_error *err)
{
  ccord_error_clear(err);
  if (out) *out = nullptr;
  if (out_len) *out_len = 0;
  if (!e || !out || !out_len) return ccord_error_set(err, CCORD_ERR_INVALID, "sound encode: invalid argument");
  if (e->finished) return ccord_error_set(err, CCORD_ERR_INVALID, "sound encode: encoder already finished");
  if (e->failed) return ccord_error_set(err, CCORD_ERR_INTERNAL, "sound encode: encoder failed earlier");
  if (e->total_frames == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "sound encode: no audio recorded");

  ccord_status st = CCORD_OK;
  if (e->pending_frames > 0) {
    // The codec accepts one short final frame; the muxer trims the padding via the end granule position.
    st = encode_chunk(e, e->pending, e->pending_frames, err);
    e->pending_frames = 0;
    if (st != CCORD_OK) return st;
  }
  if (avcodec_send_frame(e->codec, nullptr) < 0) {
    e->failed = true;
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "sound encode: Opus flush failed");
  }
  st = drain_packets(e, err);
  if (st != CCORD_OK) return st;
  if (av_write_trailer(e->fmt) < 0) {
    e->failed = true;
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "sound encode: cannot finish the Ogg stream");
  }
  e->finished = true;

  uint8_t *av_buf = nullptr;
  int n = avio_close_dyn_buf(e->fmt->pb, &av_buf);
  e->fmt->pb = nullptr;
  if (n <= 0 || !av_buf) {
    av_free(av_buf);
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "sound encode: empty Ogg output");
  }
  uint8_t *copy = ccord_malloc((size_t)n);
  if (!copy) {
    av_free(av_buf);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "sound encode: out of memory");
  }
  memcpy(copy, av_buf, (size_t)n);
  av_free(av_buf);
  *out = copy;
  *out_len = (size_t)n;
  return CCORD_OK;
}

void ccord_sound_encoder_destroy(ccord_sound_encoder *e)
{
  if (!e) return;
  if (e->fmt) {
    if (e->fmt->pb) {  // dynamic buffer still open (destroyed before finish, or finish failed)
      uint8_t *buf = nullptr;
      avio_close_dyn_buf(e->fmt->pb, &buf);
      av_free(buf);
      e->fmt->pb = nullptr;
    }
    avformat_free_context(e->fmt);
  }
  avcodec_free_context(&e->codec);
  av_frame_free(&e->frame);
  av_packet_free(&e->pkt);
  ccord_free(e->pending);
  ccord_free(e);
}
