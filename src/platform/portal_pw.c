// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// PipeWire / portal-fd frame capture. See portal_pw.h for the layering and fd-ownership rules.
#define _POSIX_C_SOURCE 200809L
#include "portal_pw.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>

#if CCORD_HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <pipewire/core.h>
#include <pipewire/loop.h>
#include <pipewire/main-loop.h>
#include <pipewire/properties.h>
#include <pipewire/stream.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#endif

// ---- shared validation ------------------------------------------------------------------------

// A raw frame is usable when it has pixels, a sane stride and a known layout. Sizes are bounded
// so width*4 and width*height*4 cannot overflow int / size_t on the conversion path.
static bool raw_ok(const ccord_portal_pw_raw *r)
{
  if (!r || !r->data || r->width <= 0 || r->height <= 0) return false;
  if (r->width > INT_MAX / 4 || r->height > INT_MAX / 4) return false;
  if (r->stride < r->width * 4) return false;
  return r->format == CCORD_PORTAL_PW_BGRA || r->format == CCORD_PORTAL_PW_BGRX || r->format == CCORD_PORTAL_PW_RGBA;
}

// ---- conversion core ---------------------------------------------------------------------------

bool ccord_portal_pw_convert(uint8_t *dst, int dst_w, int dst_h, const ccord_portal_pw_raw *src)
{
  if (!dst || dst_w <= 0 || dst_h <= 0 || !raw_ok(src)) return false;
  if (dst_w > INT_MAX / 4 || dst_h > INT_MAX / 4) return false;
  const int sw = src->width, sh = src->height;
  const size_t sstride = (size_t)src->stride;
  const size_t dstride = (size_t)dst_w * 4u;

  // Fast 1:1 paths (zero per-pixel 64-bit divisions or per-pixel format branches).
  if (dst_w == sw && dst_h == sh) {
    if (src->format == CCORD_PORTAL_PW_BGRA) {
      if (sstride == dstride) {
        memcpy(dst, src->data, dstride * (size_t)sh);
      } else {
        for (int y = 0; y < sh; y++) memcpy(dst + (size_t)y * dstride, src->data + (size_t)y * sstride, dstride);
      }
      return true;
    }
    if (src->format == CCORD_PORTAL_PW_BGRX) {
      if (sstride == dstride) {
        const uint8_t *restrict p = src->data;
        uint8_t *restrict o = dst;
        const size_t total = (size_t)sw * (size_t)sh;
        for (size_t i = 0; i < total; i++, p += 4, o += 4) {
          uint32_t px;
          memcpy(&px, p, 4u);
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
          px |= 0x000000FFu;
#else
          px |= 0xFF000000u;
#endif
          memcpy(o, &px, 4u);
        }
      } else {
        for (int y = 0; y < sh; y++) {
          const uint8_t *restrict p = src->data + (size_t)y * sstride;
          uint8_t *restrict o = dst + (size_t)y * dstride;
          for (int x = 0; x < sw; x++, p += 4, o += 4) {
            uint32_t px;
            memcpy(&px, p, 4u);
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            px |= 0x000000FFu;
#else
            px |= 0xFF000000u;
#endif
            memcpy(o, &px, 4u);
          }
        }
      }
      return true;
    }
    // CCORD_PORTAL_PW_RGBA -> BGRA (1:1 B<->R channel swap).
    for (int y = 0; y < sh; y++) {
      const uint8_t *restrict p = src->data + (size_t)y * sstride;
      uint8_t *restrict o = dst + (size_t)y * dstride;
      for (int x = 0; x < sw; x++, p += 4, o += 4) {
        o[0] = p[2];
        o[1] = p[1];
        o[2] = p[0];
        o[3] = p[3];
      }
    }
    return true;
  }

  for (int y = 0; y < dst_h; y++) {
    const int sy = (int)((int64_t)y * sh / dst_h);
    const uint8_t *srow = src->data + (size_t)sy * sstride;
    uint8_t *drow = dst + (size_t)y * dstride;
    if (src->format == CCORD_PORTAL_PW_RGBA) {
      for (int x = 0; x < dst_w; x++) {
        const int sx = (int)((int64_t)x * sw / dst_w);
        const uint8_t *p = srow + (size_t)sx * 4u;
        uint8_t *o = drow + (size_t)x * 4u;
        o[0] = p[2];
        o[1] = p[1];
        o[2] = p[0];
        o[3] = p[3];
      }
    } else if (src->format == CCORD_PORTAL_PW_BGRX) {
      for (int x = 0; x < dst_w; x++) {
        const int sx = (int)((int64_t)x * sw / dst_w);
        const uint8_t *p = srow + (size_t)sx * 4u;
        uint8_t *o = drow + (size_t)x * 4u;
        o[0] = p[0];
        o[1] = p[1];
        o[2] = p[2];
        o[3] = 255u;
      }
    } else {
      for (int x = 0; x < dst_w; x++) {
        const int sx = (int)((int64_t)x * sw / dst_w);
        const uint8_t *p = srow + (size_t)sx * 4u;
        uint8_t *o = drow + (size_t)x * 4u;
        o[0] = p[0];
        o[1] = p[1];
        o[2] = p[2];
        o[3] = p[3];
      }
    }
  }
  return true;
}

// ---- owned frame staging -----------------------------------------------------------------------

bool ccord_portal_pw_stage(ccord_portal_pw_staging *st, const uint8_t *src, size_t len)
{
  if (!st || !src || len == 0) return false;
  if (len > st->cap) {
    uint8_t *nb = ccord_realloc(st->data, len);
    if (!nb) return false;
    st->data = nb;
    st->cap = len;
  }
  memcpy(st->data, src, len);
  return true;
}

void ccord_portal_pw_staging_free(ccord_portal_pw_staging *st)
{
  if (!st) return;
  ccord_free(st->data);
  st->data = nullptr;
  st->cap = 0;
}

// ---- reader -------------------------------------------------------------------------------------

#if CCORD_HAVE_PIPEWIRE
static void pw_swap_stage(void *ud, uint8_t **bgra, size_t *cap);
#endif

struct ccord_portal_pw {
  ccord_portal_pw_ops ops;
  bool started;
  bool failed;
#if CCORD_HAVE_PIPEWIRE
  bool own_remote;    // true only for ccord_portal_pw_open_remote() (enables O(1) stage/output buffer swap)
#endif
  uint8_t *bgra;      // converted output, tightly packed
  size_t cap;         // bytes allocated in bgra (grows only)
  int w, h;
};

ccord_portal_pw *ccord_portal_pw_create(const ccord_portal_pw_ops *ops, ccord_error *err)
{
  if (!ops || !ops->connect || !ops->read_frame) {
    ccord_error_set(err, CCORD_ERR_INVALID, "portal-pw: incomplete producer seam");
    return nullptr;
  }
  ccord_portal_pw *pw = ccord_calloc(1, sizeof *pw);
  if (!pw) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "portal-pw: out of memory");
    return nullptr;
  }
  pw->ops = *ops;
  return pw;
}

ccord_status ccord_portal_pw_start(ccord_portal_pw *pw, int fd, uint32_t node_id, int timeout_ms, ccord_error *err)
{
  if (!pw || fd < 0 || timeout_ms <= 0)
    return ccord_error_set(err, CCORD_ERR_INVALID, "portal-pw: bad start arguments");
  if (pw->started)
    return ccord_error_set(err, CCORD_ERR_INVALID, "portal-pw: already started");
  ccord_error local;
  ccord_error_clear(&local);
  // The seam consumes fd in every outcome; never close it here.
  if (!pw->ops.connect(pw->ops.ud, fd, node_id, timeout_ms, &local))
    return ccord_error_set(err, local.code != CCORD_OK ? local.code : CCORD_ERR_IO, "portal-pw: connect failed: %s",
                           local.message);
  pw->started = true;
  return CCORD_OK;
}

bool ccord_portal_pw_grab(ccord_portal_pw *pw, int timeout_ms, ccord_capture_frame *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!pw || !out || timeout_ms < 0) {
    ccord_error_set(err, CCORD_ERR_INVALID, "portal-pw: bad grab arguments");
    return false;
  }
  if (!pw->started || pw->failed) {
    ccord_error_set(err, CCORD_ERR_IO, "portal-pw: capture is not running");
    return false;
  }
  ccord_portal_pw_raw raw;
  memset(&raw, 0, sizeof raw);
  ccord_error local;
  ccord_error_clear(&local);
  if (!pw->ops.read_frame(pw->ops.ud, timeout_ms, &raw, &local)) {
    if (local.code != CCORD_ERR_TIMEOUT) pw->failed = true;
    if (err) *err = local;
    return false;
  }
  if (!raw_ok(&raw)) {
    pw->failed = true;
    ccord_error_set(err, CCORD_ERR_PROTOCOL, "portal-pw: malformed frame from producer");
    return false;
  }
#if CCORD_HAVE_PIPEWIRE
  if (pw->own_remote) {
    // Real PipeWire backend converted directly into tightly packed BGRA in its staging buffer;
    // swap buffer ownership in O(1) to eliminate the second full-screen copy.
    pw_swap_stage(pw->ops.ud, &pw->bgra, &pw->cap);
    pw->w = raw.width;
    pw->h = raw.height;
    *out = (ccord_capture_frame){.data = pw->bgra, .width = pw->w, .height = pw->h, .stride = pw->w * 4};
    return true;
  }
#endif
  const size_t need = (size_t)raw.width * (size_t)raw.height * 4u;
  if (need > pw->cap) {
    uint8_t *nb = ccord_realloc(pw->bgra, need);
    if (!nb) {
      ccord_error_set(err, CCORD_ERR_NOMEM, "portal-pw: out of memory");
      return false;
    }
    pw->bgra = nb;
    pw->cap = need;
  }
  if (!ccord_portal_pw_convert(pw->bgra, raw.width, raw.height, &raw)) {
    pw->failed = true;
    ccord_error_set(err, CCORD_ERR_PROTOCOL, "portal-pw: frame conversion failed");
    return false;
  }
  pw->w = raw.width;
  pw->h = raw.height;
  *out = (ccord_capture_frame){.data = pw->bgra, .width = pw->w, .height = pw->h, .stride = pw->w * 4};
  return true;
}

bool ccord_portal_pw_failed(const ccord_portal_pw *pw)
{
  return !pw || pw->failed;
}

void ccord_portal_pw_destroy(ccord_portal_pw *pw)
{
  if (!pw) return;
  if (pw->ops.destroy) pw->ops.destroy(pw->ops.ud);
  ccord_free(pw->bgra);
  ccord_free(pw);
}

#if CCORD_HAVE_PIPEWIRE
// ---- real libpipewire backend --------------------------------------------------------------------
//
// The portal hands us a connected PipeWire remote fd; pw_context_connect_fd() takes ownership of
// it (locked to that contract by core.h: closed automatically on disconnect or error, so we must
// never close it ourselves). Buffers arrive mmap'd (PW_STREAM_FLAG_MAP_BUFFERS); the process
// callback copies the validated pixels into an owned staging buffer BEFORE returning the pw_buffer
// to the stream, so the frame later converted by grab() can never be overwritten by the producer.

typedef struct pw_backend {
  struct pw_main_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_stream *stream;
  struct spa_hook stream_listener;
  bool inited;              // pw_init() called (pw_deinit on teardown)
  int fd;                   // owned until pw_context_connect_fd takes it; -1 otherwise
  bool dead;                // stream error / closed by the portal
  bool have_format;
  ccord_portal_pw_format format;
  int width, height;
  ccord_portal_pw_staging stage;   // owned copy of the newest accepted frame
  int frame_width, frame_height;
  int frame_stride;
  uint64_t frame_seq;          // bumped per accepted buffer
  uint64_t seen_seq;           // last seq handed to read_frame
} pw_backend;

static void pw_state_changed(void *ud, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
  pw_backend *b = ud;
  (void)error;
  if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_UNCONNECTED))
    b->dead = true;
}

static void pw_param_changed(void *ud, uint32_t id, const struct spa_pod *param)
{
  pw_backend *b = ud;
  if (id != SPA_PARAM_Format || !param) return;
  struct spa_video_info info;
  memset(&info, 0, sizeof info);
  if (spa_format_video_parse(param, &info) < 0 || info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
    b->dead = true;
    return;
  }
  const struct spa_video_info_raw *raw = &info.info.raw;
  switch (raw->format) {
    case SPA_VIDEO_FORMAT_BGRx: b->format = CCORD_PORTAL_PW_BGRX; break;
    case SPA_VIDEO_FORMAT_BGRA: b->format = CCORD_PORTAL_PW_BGRA; break;
    case SPA_VIDEO_FORMAT_RGBA: b->format = CCORD_PORTAL_PW_RGBA; break;
    default: b->dead = true; return;   // negotiated something we never offered
  }
  if (raw->size.width == 0 || raw->size.height == 0 || raw->size.width > (uint32_t)(INT_MAX / 4) ||
      raw->size.height > (uint32_t)(INT_MAX / 4)) {
    b->dead = true;
    return;
  }
  b->width = (int)raw->size.width;
  b->height = (int)raw->size.height;
  b->have_format = true;
}

static void pw_process(void *ud)
{
  pw_backend *b = ud;
  struct pw_buffer *pb = pw_stream_dequeue_buffer(b->stream);
  if (!pb) return;
  struct spa_buffer *buf = pb->buffer;
  if (b->have_format && buf->n_datas >= 1 && buf->datas[0].data) {
    struct spa_data *d = &buf->datas[0];
    struct spa_chunk *ch = d->chunk;
    const int stride = ch->stride > 0 ? (int)ch->stride : b->width * 4;
    if (stride >= b->width * 4 && b->height <= INT_MAX / stride) {
      const uint32_t need = (uint32_t)((b->height - 1) * stride + b->width * 4);
      if (!((uint32_t)ch->flags & SPA_CHUNK_FLAG_CORRUPTED) && ch->size >= need && ch->offset <= d->maxsize &&
          need <= d->maxsize - ch->offset) {
        // Convert directly into owned tight BGRA storage while the buffer is still ours;
        // the mmap pointer dies at pw_stream_queue_buffer below.
        const size_t bgra_bytes = (size_t)b->width * (size_t)b->height * 4u;
        if (bgra_bytes > b->stage.cap) {
          uint8_t *nb = ccord_realloc(b->stage.data, bgra_bytes);
          if (nb) {
            b->stage.data = nb;
            b->stage.cap = bgra_bytes;
          }
        }
        if (b->stage.data && b->stage.cap >= bgra_bytes) {
          ccord_portal_pw_raw raw_in = {.data = (const uint8_t *)d->data + ch->offset,
                                        .width = b->width,
                                        .height = b->height,
                                        .stride = stride,
                                        .format = b->format};
          if (ccord_portal_pw_convert(b->stage.data, b->width, b->height, &raw_in)) {
            b->frame_width = b->width;
            b->frame_height = b->height;
            b->frame_stride = b->width * 4;
            b->frame_seq++;
          }
        }
      }
    }
  }
  pw_stream_queue_buffer(b->stream, pb);
}

static const struct pw_stream_events k_pw_events = {
  .version = PW_VERSION_STREAM_EVENTS,
  .state_changed = pw_state_changed,
  .param_changed = pw_param_changed,
  .process = pw_process,
};

// EnumFormat offering the three layouts we can convert, any size up to 8K, variable framerate.
static const struct spa_pod *pw_build_enum_format(uint8_t *buf, size_t cap)
{
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, (uint32_t)cap);
  struct spa_pod_frame f;
  const struct spa_rectangle def = {1920, 1080}, min = {1, 1}, max = {8192, 8192};
  const struct spa_fraction fr_def = {30, 1}, fr_min = {0, 1}, fr_max = {1000, 1};
  spa_pod_builder_push_object(&builder, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  spa_pod_builder_add(&builder,
                      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                      SPA_FORMAT_VIDEO_format,
                      SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA),
                      SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def, &min, &max),
                      SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&fr_def, &fr_min, &fr_max), 0);
  return spa_pod_builder_pop(&builder, &f);
}

// Tears down whatever was built, in reverse order. Closes fd only while we still own it.
static void pw_teardown(pw_backend *b)
{
  if (b->stream) {
    pw_stream_disconnect(b->stream);
    pw_stream_destroy(b->stream);
    b->stream = nullptr;
  }
  ccord_portal_pw_staging_free(&b->stage);
  if (b->core) {
    pw_core_disconnect(b->core);
    b->core = nullptr;
  }
  if (b->context) {
    pw_context_destroy(b->context);
    b->context = nullptr;
  }
  if (b->loop) {
    pw_main_loop_destroy(b->loop);
    b->loop = nullptr;
  }
  if (b->fd >= 0) {
    close(b->fd);
    b->fd = -1;
  }
  if (b->inited) {
    pw_deinit();
    b->inited = false;
  }
}

// One bounded loop iteration; false + err when the deadline passed or the loop errored.
// Callers re-check their condition and call again until it holds.
static bool pw_iterate_until(pw_backend *b, uint64_t deadline, ccord_error *err)
{
  const int64_t now = (int64_t)ccord_clock_ms();
  if ((int64_t)deadline <= now) {
    ccord_error_set(err, CCORD_ERR_TIMEOUT, "portal-pw: timed out waiting for PipeWire");
    return false;
  }
  struct pw_loop *loop = pw_main_loop_get_loop(b->loop);
  pw_loop_enter(loop);
  const int r = pw_loop_iterate(loop, (int)((int64_t)deadline - now));
  pw_loop_leave(loop);
  if (r < 0) {
    ccord_error_set(err, CCORD_ERR_IO, "portal-pw: PipeWire loop error: %s", spa_strerror(r));
    return false;
  }
  return true;
}

static bool pw_connect(void *ud, int fd, uint32_t node_id, int timeout_ms, ccord_error *err)
{
  pw_backend *b = ud;
  b->fd = fd;
  pw_init(nullptr, nullptr);
  b->inited = true;
  b->loop = pw_main_loop_new(nullptr);
  if (!b->loop) {
    ccord_error_set(err, CCORD_ERR_IO, "portal-pw: cannot create PipeWire main loop");
    goto fail;
  }
  b->context = pw_context_new(pw_main_loop_get_loop(b->loop), nullptr, 0);
  if (!b->context) {
    ccord_error_set(err, CCORD_ERR_IO, "portal-pw: cannot create PipeWire context");
    goto fail;
  }
  b->core = pw_context_connect_fd(b->context, fd, nullptr, 0);
  if (!b->core) {
    // core.h: connect_fd takes the fd and closes it automatically on error. Closing it here
    // would risk hitting an unrelated descriptor once the fd number is reused.
    b->fd = -1;
    ccord_error_set(err, CCORD_ERR_IO, "portal-pw: cannot connect to the portal PipeWire remote");
    goto fail;
  }
  b->fd = -1;   // the context owns the fd now
  b->stream = pw_stream_new(b->core, "ccord-screencast",
                            pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                              PW_KEY_MEDIA_ROLE, "Screen", nullptr));
  if (!b->stream) {
    ccord_error_set(err, CCORD_ERR_IO, "portal-pw: cannot create PipeWire stream");
    goto fail;
  }
  pw_stream_add_listener(b->stream, &b->stream_listener, &k_pw_events, b);
  uint8_t pod_buf[1024];
  const struct spa_pod *params[1] = {pw_build_enum_format(pod_buf, sizeof pod_buf)};
  if (!params[0] || pw_stream_connect(b->stream, PW_DIRECTION_INPUT, node_id,
                                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0) {
    ccord_error_set(err, CCORD_ERR_IO, "portal-pw: cannot connect PipeWire stream to node %u", (unsigned)node_id);
    goto fail;
  }
  // Wait for the format negotiation and a paused (ready) stream, bounded by timeout_ms.
  const uint64_t deadline = ccord_clock_ms() + (uint64_t)timeout_ms;
  while (!b->dead &&
         !(b->have_format && pw_stream_get_state(b->stream, nullptr) >= PW_STREAM_STATE_PAUSED)) {
    if (!pw_iterate_until(b, deadline, err)) goto fail;
  }
  if (b->dead) {
    ccord_error_set(err, CCORD_ERR_IO, "portal-pw: PipeWire stream closed during setup");
    goto fail;
  }
  return true;
fail:
  pw_teardown(b);
  return false;
}

static void pw_swap_stage(void *ud, uint8_t **bgra, size_t *cap)
{
  pw_backend *b = ud;
  uint8_t *tmp = *bgra;
  size_t tmp_cap = *cap;
  *bgra = b->stage.data;
  *cap = b->stage.cap;
  b->stage.data = tmp;
  b->stage.cap = tmp_cap;
}

static bool pw_read_frame(void *ud, int timeout_ms, ccord_portal_pw_raw *out, ccord_error *err)
{
  pw_backend *b = ud;
  if (timeout_ms == 0) {
    // Non-blocking poll: drain already-queued PipeWire events without sleeping on the UI thread.
    struct pw_loop *loop = pw_main_loop_get_loop(b->loop);
    pw_loop_enter(loop);
    for (int i = 0; i < 16 && b->seen_seq == b->frame_seq && !b->dead; i++) {
      const int r = pw_loop_iterate(loop, 0);
      if (r < 0) {
        pw_loop_leave(loop);
        ccord_error_set(err, CCORD_ERR_IO, "portal-pw: PipeWire loop error: %s", spa_strerror(r));
        return false;
      }
      if (r == 0) break;
    }
    pw_loop_leave(loop);
    if (b->dead) {
      ccord_error_set(err, CCORD_ERR_IO, "portal-pw: PipeWire stream closed");
      return false;
    }
    if (b->seen_seq == b->frame_seq) {
      ccord_error_set(err, CCORD_ERR_TIMEOUT, "portal-pw: no frame ready");
      return false;
    }
  } else {
    const uint64_t deadline = ccord_clock_ms() + (uint64_t)timeout_ms;
    while (b->seen_seq == b->frame_seq) {
      if (b->dead) {
        ccord_error_set(err, CCORD_ERR_IO, "portal-pw: PipeWire stream closed");
        return false;
      }
      if (!pw_iterate_until(b, deadline, err)) return false;
    }
  }
  b->seen_seq = b->frame_seq;
  *out = (ccord_portal_pw_raw){.data = b->stage.data,
                               .width = b->frame_width,
                               .height = b->frame_height,
                               .stride = b->frame_stride,
                               .format = CCORD_PORTAL_PW_BGRA};
  return true;
}

static void pw_destroy(void *ud)
{
  pw_backend *b = ud;
  pw_teardown(b);
  free(b);
}

ccord_portal_pw *ccord_portal_pw_open_remote(ccord_error *err)
{
  pw_backend *b = calloc(1, sizeof *b);
  if (!b) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "portal-pw: out of memory");
    return nullptr;
  }
  b->fd = -1;
  const ccord_portal_pw_ops ops = {.ud = b, .connect = pw_connect, .read_frame = pw_read_frame, .destroy = pw_destroy};
  ccord_portal_pw *pw = ccord_portal_pw_create(&ops, err);
  if (!pw) {
    free(b);
    return nullptr;
  }
  pw->own_remote = true;
  return pw;
}
#else   // !CCORD_HAVE_PIPEWIRE
// Built without libpipewire: the portal PipeWire backend is unavailable. The seam
// reader above still works for tests; real screen capture falls back elsewhere.
ccord_portal_pw *ccord_portal_pw_open_remote(ccord_error *err)
{
  ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "portal-pw: built without PipeWire support");
  return nullptr;
}
#endif   // CCORD_HAVE_PIPEWIRE
