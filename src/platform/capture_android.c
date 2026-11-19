// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Android screen capture backend (MediaProjection + VirtualDisplay + ImageReader RGBA -> BGRA).
#include "android_bridge.h"
#include <ccord/platform/capture.h>

#include <ccord/core/alloc.h>
#include <ccord/core/os.h>

#include <stdio.h>
#include <string.h>

typedef struct android_capture {
  ccord_capture base;
  uint8_t *out_bgra;
  size_t out_cap;
  int out_w;
  int out_h;
  uint64_t last_seq;
} android_capture;

static ccord_spin_lock g_cap_lock = CCORD_SPIN_LOCK_INIT;
static uint8_t *g_shared_bgra = nullptr;
static size_t g_shared_cap = 0;
static int g_shared_w = 0;
static int g_shared_h = 0;
static uint64_t g_shared_seq = 0;
static bool g_shared_stopped = false;
static bool g_shared_active = false;

void ccord_capture_android_push_frame(const uint8_t *rgba, int width, int height, int stride)
{
  if (!rgba || width <= 0 || height <= 0 || stride < width * 4) return;
  size_t need = (size_t)width * (size_t)height * 4u;
  ccord_spin_lock_acquire(&g_cap_lock);
  if (g_shared_active) {
    if (need > g_shared_cap) {
      uint8_t *nb = ccord_malloc(need);
      if (nb) {
        ccord_free(g_shared_bgra);
        g_shared_bgra = nb;
        g_shared_cap = need;
      }
    }
    if (g_shared_bgra && need <= g_shared_cap) {
      for (int y = 0; y < height; y++) {
        const uint8_t *src = rgba + (size_t)y * (size_t)stride;
        uint8_t *dst = g_shared_bgra + (size_t)y * (size_t)width * 4u;
        for (int x = 0; x < width; x++) {
          dst[x * 4 + 0] = src[x * 4 + 2];  // B
          dst[x * 4 + 1] = src[x * 4 + 1];  // G
          dst[x * 4 + 2] = src[x * 4 + 0];  // R
          dst[x * 4 + 3] = 255u;            // A
        }
      }
      g_shared_w = width;
      g_shared_h = height;
      g_shared_seq++;
      g_shared_stopped = false;
    }
  }
  ccord_spin_lock_release(&g_cap_lock);
}

void ccord_capture_android_mark_stopped(void)
{
  ccord_spin_lock_acquire(&g_cap_lock);
  g_shared_stopped = true;
  g_shared_active = false;
  ccord_spin_lock_release(&g_cap_lock);
}

static bool android_cap_available(void)
{
  return true;
}

static size_t android_cap_list(ccord_capture_kind kind, ccord_capture_source *out, size_t max, ccord_error *err)
{
  ccord_error_clear(err);
  if (kind != CCORD_CAPTURE_SCREEN || !out || max == 0) return 0;
  memset(&out[0], 0, sizeof out[0]);
  out[0].kind = CCORD_CAPTURE_SCREEN;
  out[0].backend = CCORD_CAPBE_ANDROID;
  snprintf(out[0].id, sizeof out[0].id, "android:screen");
  snprintf(out[0].title, sizeof out[0].title, "Android Screen (MediaProjection)");
  out[0].primary = true;
  return 1u;
}

static bool android_cap_grab(ccord_capture *base, ccord_capture_frame *out)
{
  android_capture *c = (android_capture *)base;
  if (!c || !out) return false;
  bool got = false;
  ccord_spin_lock_acquire(&g_cap_lock);
  if (g_shared_bgra && g_shared_w > 0 && g_shared_h > 0 && g_shared_seq != c->last_seq) {
    size_t need = (size_t)g_shared_w * (size_t)g_shared_h * 4u;
    if (need > c->out_cap) {
      uint8_t *nb = ccord_malloc(need);
      if (nb) {
        ccord_free(c->out_bgra);
        c->out_bgra = nb;
        c->out_cap = need;
      }
    }
    if (c->out_bgra && need <= c->out_cap) {
      memcpy(c->out_bgra, g_shared_bgra, need);
      c->out_w = g_shared_w;
      c->out_h = g_shared_h;
      c->last_seq = g_shared_seq;
      got = true;
    }
  }
  ccord_spin_lock_release(&g_cap_lock);
  if (!got) return false;
  out->data = c->out_bgra;
  out->width = c->out_w;
  out->height = c->out_h;
  out->stride = c->out_w * 4;
  return true;
}

static bool android_cap_failed(const ccord_capture *base)
{
  (void)base;
  ccord_spin_lock_acquire(&g_cap_lock);
  bool stopped = g_shared_stopped;
  ccord_spin_lock_release(&g_cap_lock);
  return stopped;
}

static void android_cap_close(ccord_capture *base)
{
  android_capture *c = (android_capture *)base;
  if (!c) return;
  ccord_android_stop_screen_capture();
  ccord_spin_lock_acquire(&g_cap_lock);
  g_shared_active = false;
  g_shared_stopped = false;
  ccord_free(g_shared_bgra);
  g_shared_bgra = nullptr;
  g_shared_cap = 0;
  g_shared_w = 0;
  g_shared_h = 0;
  ccord_spin_lock_release(&g_cap_lock);
  ccord_free(c->out_bgra);
  ccord_free(c);
}

static const ccord_capture_ops k_android_ops = {
  .close = android_cap_close,
  .grab = android_cap_grab,
  .failed = android_cap_failed,
};

static ccord_capture *android_cap_open(const ccord_capture_source *src, ccord_error *err)
{
  ccord_error_clear(err);
  if (!src) {
    ccord_error_set(err, CCORD_ERR_INVALID, "capture: null source");
    return nullptr;
  }
  android_capture *c = ccord_calloc(1, sizeof *c);
  if (!c) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "capture: out of memory");
    return nullptr;
  }
  ccord_spin_lock_acquire(&g_cap_lock);
  g_shared_active = true;
  g_shared_stopped = false;
  g_shared_seq = 0;
  ccord_spin_lock_release(&g_cap_lock);

  if (!ccord_android_request_screen_capture()) {
    ccord_spin_lock_acquire(&g_cap_lock);
    g_shared_active = false;
    ccord_spin_lock_release(&g_cap_lock);
    ccord_free(c);
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "capture: Android MediaProjection unavailable");
    return nullptr;
  }
  c->base.ops = &k_android_ops;
  c->base.src = *src;
  return &c->base;
}

const ccord_capture_backend ccord_capture_android_backend = {
  .name = "Android MediaProjection",
  .available = android_cap_available,
  .list = android_cap_list,
  .open = android_cap_open,
  .thumbnail = nullptr,
};
