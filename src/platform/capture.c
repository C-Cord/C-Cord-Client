// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/platform/capture.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>

static atomic_bool g_fake;
static atomic_int g_own_pid;

void ccord_capture_set_fake(bool on) { atomic_store(&g_fake, on); }
bool ccord_capture_fake_enabled(void) { return atomic_load(&g_fake); }
void ccord_capture_set_own_pid(int pid) { atomic_store(&g_own_pid, pid); }
int ccord_capture_own_pid(void) { return atomic_load(&g_own_pid); }

// ---------------------------------------------------------------- FAKE backend

typedef struct fake_cap {
  ccord_capture base;
  uint8_t *buf;
  int w, h;
  uint32_t seed;
  uint64_t t0;
} fake_cap;

static const struct {
  const char *id, *title, *app;
  ccord_capture_kind kind;
  int w, h;
  uint8_t r, g, b;
} k_fake[] = {
  {"fake:win:1", "General | C-Cord Demo Server", "C-Cord", CCORD_CAPTURE_WINDOW, 1280, 800, 0x31, 0x33, 0x38},
  {"fake:win:2", "~/projects/c-cord: zsh", "Konsole", CCORD_CAPTURE_WINDOW, 960, 600, 0x1b, 0x1e, 0x24},
  {"fake:win:3", "Blender - untitled.blend", "Blender", CCORD_CAPTURE_WINDOW, 1280, 720, 0x3d, 0x3d, 0x3d},
  {"fake:win:4", "Mozilla Firefox", "firefox", CCORD_CAPTURE_WINDOW, 1100, 760, 0x24, 0x4a, 0x7c},
  {"fake:win:5", "main.c - Visual Studio Code", "code", CCORD_CAPTURE_WINDOW, 1200, 800, 0x1e, 0x1e, 0x2e},
  {"fake:win:6", "Spotify Premium", "spotify", CCORD_CAPTURE_WINDOW, 900, 640, 0x12, 0x12, 0x12},
  {"fake:mon:1", "Screen 1 (primary)", "", CCORD_CAPTURE_SCREEN, 1920, 1080, 0x20, 0x4f, 0x6e},
  {"fake:mon:2", "Screen 2", "", CCORD_CAPTURE_SCREEN, 1920, 1080, 0x5b, 0x3a, 0x6e},
};

static size_t fake_list(ccord_capture_kind kind, ccord_capture_source *out, size_t max, ccord_error *err)
{
  (void)err;
  size_t n = 0;
  for (size_t i = 0; i < sizeof k_fake / sizeof k_fake[0] && n < max; i++) {
    if (k_fake[i].kind != kind) continue;
    ccord_capture_source *s = &out[n++];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->backend = CCORD_CAPBE_FAKE;
    snprintf(s->id, sizeof s->id, "%s", k_fake[i].id);
    snprintf(s->title, sizeof s->title, "%s", k_fake[i].title);
    snprintf(s->app, sizeof s->app, "%s", k_fake[i].app);
    s->width = k_fake[i].w;
    s->height = k_fake[i].h;
    s->primary = kind == CCORD_CAPTURE_SCREEN && n == 1;
    s->native = (uint64_t)i + 1u;
  }
  return n;
}

static void fake_paint(fake_cap *c, uint8_t br, uint8_t bg, uint8_t bb, uint32_t t_ms)
{
  int w = c->w, h = c->h;
  int title_h = h / 12;
  if (title_h < 24) title_h = 24;
  int side_w = w / 5;
  int prog = (int)(((uint64_t)(t_ms + 1200u) * (uint64_t)w / 4000u) % (uint64_t)w);
  for (int y = 0; y < h; y++) {
    uint8_t *row = c->buf + (size_t)y * (size_t)w * 4u;
    int shade = (y * 28) / h;
    for (int x = 0; x < w; x++) {
      int r = br + shade, g = bg + shade, b = bb + shade;
      if (y < title_h) {
        r = (br * 2) / 3 + 12;
        g = (bg * 2) / 3 + 12;
        b = (bb * 2) / 3 + 16;
        int cy = title_h / 2;
        if (abs(y - cy) <= title_h / 5) {
          if (abs(x - title_h / 2) <= title_h / 5) { r = 0xed; g = 0x42; b = 0x45; }
          else if (abs(x - (title_h + title_h / 4)) <= title_h / 5) { r = 0xfe; g = 0xe7; b = 0x5c; }
          else if (abs(x - (title_h * 2)) <= title_h / 5) { r = 0x57; g = 0xf2; b = 0x87; }
        }
      } else if (x < side_w) {
        r = (br * 3) / 4 + 8;
        g = (bg * 3) / 4 + 8;
        b = (bb * 3) / 4 + 12;
        int ry = (y - title_h) % 44;
        if (x > 16 && x < side_w - 20 && ry >= 14 && ry <= 28) {
          r += 28; g += 28; b += 34;
        }
      } else {
        int cx = x - side_w;
        int cy = y - title_h;
        int card_h = (h - title_h) / 4;
        if (card_h > 20 && cx > 24 && cx < (w - side_w - 24)) {
          int slot = cy / card_h;
          int rem = cy % card_h;
          if (slot < 3 && rem > 12 && rem < card_h - 8) {
            r += 18; g += 20; b += 26;
            if (rem > 22 && rem < 38 && cx < (w - side_w) / (2 + (slot & 1))) {
              if (slot == 0) { r = 0x58; g = 0x65; b = 0xf2; }
              else if (slot == 1) { r = 0x3b; g = 0xa5; b = 0x5c; }
              else { r = 0xeb; g = 0x45; b = 0x9e; }
            }
          }
        }
      }
      if (y > h - 10 && x < prog) { r = 0x58; g = 0x65; b = 0xf2; }
      row[x * 4 + 0] = (uint8_t)(b > 255 ? 255 : b);
      row[x * 4 + 1] = (uint8_t)(g > 255 ? 255 : g);
      row[x * 4 + 2] = (uint8_t)(r > 255 ? 255 : r);
      row[x * 4 + 3] = 255;
    }
  }
}

static bool fake_grab(ccord_capture *base, ccord_capture_frame *out)
{
  fake_cap *c = (fake_cap *)base;
  size_t idx = 0;
  for (size_t i = 0; i < sizeof k_fake / sizeof k_fake[0]; i++) if (strcmp(k_fake[i].id, base->src.id) == 0) idx = i;
  uint32_t t = (uint32_t)(ccord_clock_ms() - c->t0);
  fake_paint(c, k_fake[idx].r, k_fake[idx].g, k_fake[idx].b, t);
  *out = (ccord_capture_frame){.data = c->buf, .width = c->w, .height = c->h, .stride = c->w * 4};
  return true;
}

static bool fake_failed(const ccord_capture *c) { (void)c; return false; }

static void fake_close(ccord_capture *base)
{
  fake_cap *c = (fake_cap *)base;
  ccord_free(c->buf);
  ccord_free(c);
}

static const ccord_capture_ops k_fake_ops = {.close = fake_close, .grab = fake_grab, .failed = fake_failed};

static ccord_capture *fake_open(const ccord_capture_source *src, ccord_error *err)
{
  fake_cap *c = ccord_alloc_array(1, sizeof *c);
  if (!c) { ccord_error_set(err, CCORD_ERR_NOMEM, "capture: out of memory"); return nullptr; }
  c->base.ops = &k_fake_ops;
  c->base.src = *src;
  c->w = src->width > 0 ? src->width : 1280;
  c->h = src->height > 0 ? src->height : 720;
  c->t0 = ccord_clock_ms();
  c->buf = ccord_alloc_array((size_t)c->w * (size_t)c->h, 4);
  if (!c->buf) { ccord_free(c); ccord_error_set(err, CCORD_ERR_NOMEM, "capture: out of memory"); return nullptr; }
  return &c->base;
}

static bool fake_available(void) { return true; }

static const ccord_capture_backend k_fake_backend = {.name = "Test pattern", .available = fake_available,
                                                      .list = fake_list, .open = fake_open, .thumbnail = nullptr};

// ---------------------------------------------------------------- dispatch

static const ccord_capture_backend *backend_of(ccord_capture_backend_id id)
{
  switch (id) {
#if defined(_WIN32)
    case CCORD_CAPBE_WIN32: return &ccord_capture_win32_backend;
#elif defined(__APPLE__)
    case CCORD_CAPBE_MACOS: return &ccord_capture_macos_backend;
#elif defined(__ANDROID__)
    case CCORD_CAPBE_ANDROID: return &ccord_capture_android_backend;
#else
    case CCORD_CAPBE_X11: return &ccord_capture_x11_backend;
    case CCORD_CAPBE_PORTAL: return &ccord_capture_portal_backend;
#endif
    case CCORD_CAPBE_FAKE: return &k_fake_backend;
    default: return nullptr;
  }
}

bool ccord_capture_backend_available(ccord_capture_backend_id id)
{
  const ccord_capture_backend *b = backend_of(id);
  return b && b->available && b->available();
}

const char *ccord_capture_backend_name(ccord_capture_backend_id id)
{
  const ccord_capture_backend *b = backend_of(id);
  return b ? b->name : "none";
}

ccord_capture_backend_id ccord_capture_resolve_backend(ccord_capture_backend_id requested)
{
  if (ccord_capture_fake_enabled()) return CCORD_CAPBE_FAKE;
  if (requested != CCORD_CAPBE_AUTO) return ccord_capture_backend_available(requested) ? requested : CCORD_CAPBE_AUTO;
#if defined(_WIN32)
  if (ccord_capture_backend_available(CCORD_CAPBE_WIN32)) return CCORD_CAPBE_WIN32;
  return CCORD_CAPBE_AUTO;
#elif defined(__APPLE__)
  if (ccord_capture_backend_available(CCORD_CAPBE_MACOS)) return CCORD_CAPBE_MACOS;
  return CCORD_CAPBE_AUTO;
#elif defined(__ANDROID__)
  if (ccord_capture_backend_available(CCORD_CAPBE_ANDROID)) return CCORD_CAPBE_ANDROID;
  return CCORD_CAPBE_AUTO;
#else
  const char *session = getenv("XDG_SESSION_TYPE");
  const char *wl_dpy = getenv("WAYLAND_DISPLAY");
  bool wayland = (session && strcmp(session, "wayland") == 0) || (wl_dpy && wl_dpy[0] != '\0');
  if (!wayland && ccord_capture_backend_available(CCORD_CAPBE_X11)) return CCORD_CAPBE_X11;
  if (ccord_capture_backend_available(CCORD_CAPBE_PORTAL)) return CCORD_CAPBE_PORTAL;
  if (ccord_capture_backend_available(CCORD_CAPBE_X11)) return CCORD_CAPBE_X11;   // Xwayland: windows of X11 clients only
  return CCORD_CAPBE_AUTO;
#endif
}

size_t ccord_capture_list(ccord_capture_backend_id be, ccord_capture_kind kind, ccord_capture_source *out,
                          size_t max, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || max == 0) return 0;
  const ccord_capture_backend *b = backend_of(ccord_capture_resolve_backend(be));
  if (!b) { ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "capture: no capture backend available"); return 0; }
  return b->list(kind, out, max, err);
}

bool ccord_capture_downscale_rgba(const ccord_capture_frame *in, int max_w, int max_h, uint8_t *rgba, int *out_w,
                                  int *out_h)
{
  if (!in || !in->data || in->width <= 0 || in->height <= 0 || max_w <= 0 || max_h <= 0 || !rgba) return false;
  // Fit inside max_w x max_h without upscaling.
  double sx = (double)max_w / (double)in->width, sy = (double)max_h / (double)in->height;
  double s = sx < sy ? sx : sy;
  if (s > 1.0) s = 1.0;
  int ow = (int)((double)in->width * s), oh = (int)((double)in->height * s);
  if (ow < 1) ow = 1;
  if (oh < 1) oh = 1;
  for (int y = 0; y < oh; y++) {
    int y0 = (int)((int64_t)y * in->height / oh), y1 = (int)((int64_t)(y + 1) * in->height / oh);
    if (y1 <= y0) y1 = y0 + 1;
    if (y1 > in->height) y1 = in->height;
    for (int x = 0; x < ow; x++) {
      int x0 = (int)((int64_t)x * in->width / ow), x1 = (int)((int64_t)(x + 1) * in->width / ow);
      if (x1 <= x0) x1 = x0 + 1;
      if (x1 > in->width) x1 = in->width;
      // Box filter, sampling at most 4x4 points per output pixel for speed.
      int stepx = (x1 - x0 + 3) / 4, stepy = (y1 - y0 + 3) / 4;
      if (stepx < 1) stepx = 1;
      if (stepy < 1) stepy = 1;
      unsigned sb = 0, sg = 0, sr = 0, cnt = 0;
      for (int yy = y0; yy < y1; yy += stepy) {
        const uint8_t *row = in->data + (size_t)yy * (size_t)in->stride;
        for (int xx = x0; xx < x1; xx += stepx) {
          const uint8_t *p = row + (size_t)xx * 4u;
          sb += p[0]; sg += p[1]; sr += p[2]; cnt++;
        }
      }
      uint8_t *o = rgba + ((size_t)y * (size_t)ow + (size_t)x) * 4u;
      o[0] = (uint8_t)(sr / cnt); o[1] = (uint8_t)(sg / cnt); o[2] = (uint8_t)(sb / cnt); o[3] = 255;
    }
  }
  if (out_w) *out_w = ow;
  if (out_h) *out_h = oh;
  return true;
}

ccord_capture *ccord_capture_open(const ccord_capture_source *src, ccord_error *err)
{
  ccord_error_clear(err);
  if (!src) { ccord_error_set(err, CCORD_ERR_INVALID, "capture: null source"); return nullptr; }
  const ccord_capture_backend *b = backend_of(src->backend);
  if (!b || !b->open) { ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "capture: backend unavailable"); return nullptr; }
  ccord_capture *c = b->open(src, err);
  return c;
}

bool ccord_capture_grab(ccord_capture *c, ccord_capture_frame *out)
{
  return c && c->ops && out && c->ops->grab(c, out);
}

bool ccord_capture_failed(const ccord_capture *c)
{
  return !c || !c->ops || (c->ops->failed && c->ops->failed(c));
}

void ccord_capture_close(ccord_capture *c)
{
  if (c && c->ops) c->ops->close(c);
}

bool ccord_capture_thumbnail(const ccord_capture_source *src, int max_w, int max_h, uint8_t *rgba, int *out_w,
                             int *out_h, ccord_error *err)
{
  ccord_error_clear(err);
  if (!src || !rgba) { ccord_error_set(err, CCORD_ERR_INVALID, "capture: bad thumbnail args"); return false; }
  const ccord_capture_backend *b = backend_of(src->backend);
  if (!b) { ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "capture: backend unavailable"); return false; }
  if (b->thumbnail) return b->thumbnail(src, max_w, max_h, rgba, out_w, out_h, err);
  ccord_capture *c = b->open(src, err);
  if (!c) return false;
  ccord_capture_frame f;
  bool ok = false;
  for (int attempt = 0; attempt < 3 && !ok; attempt++) ok = ccord_capture_grab(c, &f);
  if (ok) ok = ccord_capture_downscale_rgba(&f, max_w, max_h, rgba, out_w, out_h);
  if (!ok) ccord_error_set(err, CCORD_ERR_IO, "capture: no frame for thumbnail");
  ccord_capture_close(c);
  return ok;
}
