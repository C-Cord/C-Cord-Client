// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/platform/capture.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>

#if CCORD_HAVE_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrandr.h>
#include <sys/ipc.h>
#include <sys/shm.h>

static volatile bool g_x11_err = false;
static int x11_ignore_error(Display *dpy, XErrorEvent *ev)
{
  (void)dpy;
  (void)ev;
  g_x11_err = true;
  return 0;
}

static bool x11_available(void)
{
  const char *disp = getenv("DISPLAY");
  if (!disp || !disp[0]) return false;
  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy) return false;
  XCloseDisplay(dpy);
  return true;
}

typedef struct x11_list_atoms {
  Atom net_client_list_stacking;
  Atom net_client_list;
  Atom net_wm_pid;
  Atom net_wm_visible_name;
  Atom net_wm_name;
  Atom utf8_string;
} x11_list_atoms;

static x11_list_atoms x11_intern_list_atoms(Display *dpy)
{
  char n0[] = "_NET_CLIENT_LIST_STACKING";
  char n1[] = "_NET_CLIENT_LIST";
  char n2[] = "_NET_WM_PID";
  char n3[] = "_NET_WM_VISIBLE_NAME";
  char n4[] = "_NET_WM_NAME";
  char n5[] = "UTF8_STRING";
  char *names[6] = {n0, n1, n2, n3, n4, n5};
  Atom atoms[6] = {None, None, None, None, None, None};
  (void)XInternAtoms(dpy, names, 6, False, atoms);
  return (x11_list_atoms){
    .net_client_list_stacking = atoms[0],
    .net_client_list = atoms[1],
    .net_wm_pid = atoms[2],
    .net_wm_visible_name = atoms[3],
    .net_wm_name = atoms[4],
    .utf8_string = atoms[5],
  };
}

static bool x11_get_utf8_prop(Display *dpy, Window w, Atom prop, Atom utf8, char *out, size_t cap)
{
  if (!out || cap == 0 || prop == None) return false;
  out[0] = '\0';
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char *data = nullptr;
  if (XGetWindowProperty(dpy, w, prop, 0, 256, False, AnyPropertyType, &actual_type, &actual_format, &nitems,
                         &bytes_after, &data) != Success ||
      !data) {
    return false;
  }
  bool ok = false;
  if ((actual_type == utf8 || actual_type == XA_STRING) && actual_format == 8 && nitems > 0) {
    size_t len = nitems < cap - 1 ? (size_t)nitems : cap - 1;
    memcpy(out, data, len);
    out[len] = '\0';
    ok = out[0] != '\0';
  }
  XFree(data);
  return ok;
}

static int x11_window_pid(Display *dpy, Window w, Atom net_wm_pid)
{
  if (net_wm_pid == None) return 0;
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char *data = nullptr;
  if (XGetWindowProperty(dpy, w, net_wm_pid, 0, 1, False, XA_CARDINAL, &actual_type, &actual_format, &nitems,
                         &bytes_after, &data) != Success ||
      !data) {
    return 0;
  }
  int pid = 0;
  if (actual_type == XA_CARDINAL && actual_format == 32 && nitems >= 1) {
    unsigned long raw_pid = 0;
    memcpy(&raw_pid, data, sizeof raw_pid);
    pid = (int)raw_pid;
  }
  XFree(data);
  return pid;
}

static void x11_window_title_and_app(Display *dpy, Window w, const x11_list_atoms *atoms, char *title,
                                     size_t title_cap, char *app, size_t app_cap)
{
  title[0] = '\0';
  app[0] = '\0';
  if (!x11_get_utf8_prop(dpy, w, atoms->net_wm_visible_name, atoms->utf8_string, title, title_cap) &&
      !x11_get_utf8_prop(dpy, w, atoms->net_wm_name, atoms->utf8_string, title, title_cap)) {
    (void)x11_get_utf8_prop(dpy, w, XA_WM_NAME, atoms->utf8_string, title, title_cap);
  }
  XClassHint ch = {0};
  if (XGetClassHint(dpy, w, &ch)) {
    const char *cls = ch.res_class ? ch.res_class : ch.res_name;
    if (cls && cls[0]) {
      snprintf(app, app_cap, "%s", cls);
    }
    if (ch.res_name) XFree(ch.res_name);
    if (ch.res_class) XFree(ch.res_class);
  }
}

static size_t x11_list_screens(Display *dpy, Window root, ccord_capture_source *out, size_t max)
{
  size_t count = 0;
  XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
  RROutput primary = XRRGetOutputPrimary(dpy, root);
  if (res) {
    for (int pass = 0; pass < 2 && count < max; pass++) {
      for (int i = 0; i < res->noutput && count < max; i++) {
        RROutput o = res->outputs[i];
        bool is_prim = (o == primary) || (primary == None && i == 0);
        if ((pass == 0) != is_prim) continue;
        XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, o);
        if (!oi) continue;
        if (oi->connection == RR_Connected && oi->crtc != None) {
          XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
          if (ci && ci->width > 0 && ci->height > 0) {
            ccord_capture_source *s = &out[count++];
            memset(s, 0, sizeof *s);
            s->kind = CCORD_CAPTURE_SCREEN;
            s->backend = CCORD_CAPBE_X11;
            snprintf(s->id, sizeof s->id, "x11:mon:%.*s", (int)oi->nameLen, oi->name ? oi->name : "Screen");
            snprintf(s->title, sizeof s->title, "%.*s (%ux%u)", (int)oi->nameLen, oi->name ? oi->name : "Screen",
                     ci->width, ci->height);
            s->x = ci->x;
            s->y = ci->y;
            s->width = (int)ci->width;
            s->height = (int)ci->height;
            s->primary = is_prim;
            s->native = (uint64_t)o;
          }
          if (ci) XRRFreeCrtcInfo(ci);
        }
        XRRFreeOutputInfo(oi);
      }
    }
    XRRFreeScreenResources(res);
  }
  if (count == 0 && max > 0) {
    int scr = DefaultScreen(dpy);
    ccord_capture_source *s = &out[count++];
    memset(s, 0, sizeof *s);
    s->kind = CCORD_CAPTURE_SCREEN;
    s->backend = CCORD_CAPBE_X11;
    snprintf(s->id, sizeof s->id, "x11:mon:root");
    snprintf(s->title, sizeof s->title, "Screen 1 (primary)");
    s->x = 0;
    s->y = 0;
    s->width = DisplayWidth(dpy, scr);
    s->height = DisplayHeight(dpy, scr);
    s->primary = true;
    s->native = (uint64_t)root;
  }
  return count;
}

static size_t x11_list_windows(Display *dpy, Window root, ccord_capture_source *out, size_t max)
{
  const x11_list_atoms atoms = x11_intern_list_atoms(dpy);
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char *data = nullptr;
  if (atoms.net_client_list_stacking == None ||
      XGetWindowProperty(dpy, root, atoms.net_client_list_stacking, 0, 1024, False, XA_WINDOW, &actual_type,
                         &actual_format, &nitems, &bytes_after, &data) != Success ||
      !data || nitems == 0) {
    if (data) XFree(data);
    data = nullptr;
    if (atoms.net_client_list == None ||
        XGetWindowProperty(dpy, root, atoms.net_client_list, 0, 1024, False, XA_WINDOW, &actual_type, &actual_format,
                           &nitems, &bytes_after, &data) != Success ||
        !data || nitems == 0) {
      if (data) XFree(data);
      return 0;
    }
  }
  Window *wins = (Window *)(void *)data;
  int own_pid = ccord_capture_own_pid();
  size_t count = 0;
  for (long i = (long)nitems - 1; i >= 0 && count < max; i--) {
    Window w = wins[i];
    if (w == None) continue;
    if (own_pid > 0 && x11_window_pid(dpy, w, atoms.net_wm_pid) == own_pid) continue;
    XWindowAttributes wa;
    g_x11_err = false;
    if (!XGetWindowAttributes(dpy, w, &wa) || g_x11_err) continue;
    if (wa.map_state != IsViewable || wa.width < 64 || wa.height < 64) continue;
    char title[CCORD_CAPTURE_TITLE_MAX];
    char app[CCORD_CAPTURE_APP_MAX];
    x11_window_title_and_app(dpy, w, &atoms, title, sizeof title, app, sizeof app);
    if (!title[0]) continue;
    ccord_capture_source *s = &out[count++];
    memset(s, 0, sizeof *s);
    s->kind = CCORD_CAPTURE_WINDOW;
    s->backend = CCORD_CAPBE_X11;
    snprintf(s->id, sizeof s->id, "x11:win:0x%lx", (unsigned long)w);
    snprintf(s->title, sizeof s->title, "%s", title);
    snprintf(s->app, sizeof s->app, "%s", app);
    int rx = 0, ry = 0;
    Window child = None;
    if (XTranslateCoordinates(dpy, w, root, 0, 0, &rx, &ry, &child)) {
      s->x = rx;
      s->y = ry;
    }
    s->width = wa.width;
    s->height = wa.height;
    s->native = (uint64_t)w;
  }
  XFree(data);
  return count;
}

static size_t x11_list(ccord_capture_kind kind, ccord_capture_source *out, size_t max, ccord_error *err)
{
  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    ccord_error_set(err, CCORD_ERR_IO, "x11: cannot open DISPLAY");
    return 0;
  }
  int (*prev_handler)(Display *, XErrorEvent *) = XSetErrorHandler(x11_ignore_error);
  Window root = DefaultRootWindow(dpy);
  size_t n = (kind == CCORD_CAPTURE_SCREEN) ? x11_list_screens(dpy, root, out, max)
                                            : x11_list_windows(dpy, root, out, max);
  XSetErrorHandler(prev_handler);
  XCloseDisplay(dpy);
  return n;
}

typedef struct x11_cap {
  ccord_capture base;
  Display *dpy;
  Window root;
  Window win;
  bool has_composite;
  bool shm_supported;   // cached XShmQueryExtension result (probed once in x11_open)
  bool has_shm;
  XShmSegmentInfo shminfo;
  XImage *shm_img;
  int shm_w, shm_h;
  uint8_t *bgra;
  size_t bgra_cap;
  bool dead;
} x11_cap;

static void x11_free_shm(x11_cap *c)
{
  if (!c || !c->shm_img) return;
  if (c->has_shm) {
    XShmDetach(c->dpy, &c->shminfo);
    shmdt(c->shminfo.shmaddr);
    shmctl(c->shminfo.shmid, IPC_RMID, nullptr);
  }
  XDestroyImage(c->shm_img);
  c->shm_img = nullptr;
  c->has_shm = false;
  c->shm_w = 0;
  c->shm_h = 0;
}

static void x11_close(ccord_capture *base)
{
  x11_cap *c = (x11_cap *)base;
  if (!c) return;
  int (*prev_handler)(Display *, XErrorEvent *) = XSetErrorHandler(x11_ignore_error);
  x11_free_shm(c);
  if (c->has_composite && c->win != None) {
    XCompositeUnredirectWindow(c->dpy, c->win, CompositeRedirectAutomatic);
  }
  XSetErrorHandler(prev_handler);
  if (c->dpy) XCloseDisplay(c->dpy);
  ccord_free(c->bgra);
  ccord_free(c);
}

static bool x11_failed(const ccord_capture *base)
{
  const x11_cap *c = (const x11_cap *)base;
  return !c || c->dead;
}

static void copy_ximage_to_bgra(XImage *img, int w, int h, uint8_t *restrict dst)
{
  const size_t row_bytes = (size_t)w * 4u;
  const size_t src_stride = (size_t)img->bytes_per_line;
  if (img->bits_per_pixel == 32) {
    if (src_stride == row_bytes) {
      const uint8_t *restrict s = (const uint8_t *)img->data;
      uint8_t *restrict d = dst;
      const size_t total = (size_t)w * (size_t)h;
      for (size_t i = 0; i < total; i++, s += 4, d += 4) {
        uint32_t px;
        memcpy(&px, s, 4u);
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        px |= 0x000000FFu;
#else
        px |= 0xFF000000u;
#endif
        memcpy(d, &px, 4u);
      }
      return;
    }
    for (int y = 0; y < h; y++) {
      const uint8_t *restrict s = (const uint8_t *)img->data + (size_t)y * src_stride;
      uint8_t *restrict d = dst + (size_t)y * row_bytes;
      for (int x = 0; x < w; x++, s += 4, d += 4) {
        uint32_t px;
        memcpy(&px, s, 4u);
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        px |= 0x000000FFu;
#else
        px |= 0xFF000000u;
#endif
        memcpy(d, &px, 4u);
      }
    }
    return;
  }
  for (int y = 0; y < h; y++) {
    uint8_t *restrict dst_row = dst + (size_t)y * row_bytes;
    for (int x = 0; x < w; x++) {
      unsigned long px = XGetPixel(img, x, y);
      dst_row[(size_t)x * 4u + 0u] = (uint8_t)(px & 0xffu);
      dst_row[(size_t)x * 4u + 1u] = (uint8_t)((px >> 8) & 0xffu);
      dst_row[(size_t)x * 4u + 2u] = (uint8_t)((px >> 16) & 0xffu);
      dst_row[(size_t)x * 4u + 3u] = 255u;
    }
  }
}

static bool x11_grab(ccord_capture *base, ccord_capture_frame *out)
{
  x11_cap *c = (x11_cap *)base;
  if (!c || !c->dpy || c->dead) return false;
  int (*prev_handler)(Display *, XErrorEvent *) = XSetErrorHandler(x11_ignore_error);
  g_x11_err = false;

  Drawable draw = c->root;
  int gx = c->base.src.x, gy = c->base.src.y;
  int gw = c->base.src.width, gh = c->base.src.height;
  Pixmap pix = None;

  if (c->base.src.kind == CCORD_CAPTURE_WINDOW && c->win != None) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(c->dpy, c->win, &wa) || g_x11_err || wa.map_state != IsViewable) {
      c->dead = true;
      XSetErrorHandler(prev_handler);
      return false;
    }
    gw = wa.width;
    gh = wa.height;
    gx = 0;
    gy = 0;
    if (c->has_composite) {
      pix = XCompositeNameWindowPixmap(c->dpy, c->win);
      if (!g_x11_err && pix != None) {
        draw = pix;
      } else {
        pix = None;
        g_x11_err = false;
        draw = c->win;
      }
    } else {
      draw = c->win;
    }
  }
  if (gw <= 0 || gh <= 0) {
    if (pix != None) XFreePixmap(c->dpy, pix);
    XSetErrorHandler(prev_handler);
    return false;
  }

  size_t need = (size_t)gw * (size_t)gh * 4u;
  if (need > c->bgra_cap) {
    uint8_t *nb = ccord_malloc(need);
    if (!nb) {
      if (pix != None) XFreePixmap(c->dpy, pix);
      XSetErrorHandler(prev_handler);
      return false;
    }
    ccord_free(c->bgra);
    c->bgra = nb;
    c->bgra_cap = need;
  }

  bool grabbed = false;
  if (c->shm_supported) {
    if (!c->shm_img || c->shm_w != gw || c->shm_h != gh) {
      x11_free_shm(c);
      int scr = DefaultScreen(c->dpy);
      c->shm_img = XShmCreateImage(c->dpy, DefaultVisual(c->dpy, scr), (unsigned)DefaultDepth(c->dpy, scr), ZPixmap,
                                   nullptr, &c->shminfo, (unsigned)gw, (unsigned)gh);
      if (c->shm_img) {
        c->shminfo.shmid = shmget(IPC_PRIVATE, (size_t)c->shm_img->bytes_per_line * (size_t)c->shm_img->height,
                                  IPC_CREAT | 0600);
        if (c->shminfo.shmid >= 0) {
          c->shminfo.shmaddr = c->shm_img->data = shmat(c->shminfo.shmid, nullptr, 0);
          c->shminfo.readOnly = False;
          g_x11_err = false;
          if (c->shminfo.shmaddr != (char *)-1 && XShmAttach(c->dpy, &c->shminfo) && (XSync(c->dpy, False), !g_x11_err)) {
            c->has_shm = true;
            c->shm_w = gw;
            c->shm_h = gh;
          } else {
            if (c->shminfo.shmaddr != (char *)-1) {
              shmdt(c->shminfo.shmaddr);
              c->shm_supported = false;   // e.g. remote X11 display: avoid retrying XShmAttach/XSync every frame
            }
            shmctl(c->shminfo.shmid, IPC_RMID, nullptr);
            XDestroyImage(c->shm_img);
            c->shm_img = nullptr;
          }
        } else {
          XDestroyImage(c->shm_img);
          c->shm_img = nullptr;
        }
      }
    }
    if (c->has_shm && c->shm_img) {
      g_x11_err = false;
      if (XShmGetImage(c->dpy, draw, c->shm_img, gx, gy, AllPlanes) && !g_x11_err) {
        copy_ximage_to_bgra(c->shm_img, gw, gh, c->bgra);
        grabbed = true;
      }
    }
  }

  if (!grabbed) {
    g_x11_err = false;
    XImage *img = XGetImage(c->dpy, draw, gx, gy, (unsigned)gw, (unsigned)gh, AllPlanes, ZPixmap);
    if (img && !g_x11_err) {
      copy_ximage_to_bgra(img, gw, gh, c->bgra);
      XDestroyImage(img);
      grabbed = true;
    } else if (img) {
      XDestroyImage(img);
    }
  }

  if (pix != None) XFreePixmap(c->dpy, pix);
  XSetErrorHandler(prev_handler);
  if (!grabbed) return false;

  *out = (ccord_capture_frame){.data = c->bgra, .width = gw, .height = gh, .stride = gw * 4};
  return true;
}

static const ccord_capture_ops k_x11_ops = {.close = x11_close, .grab = x11_grab, .failed = x11_failed};

static ccord_capture *x11_open(const ccord_capture_source *src, ccord_error *err)
{
  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    ccord_error_set(err, CCORD_ERR_IO, "x11: cannot open DISPLAY");
    return nullptr;
  }
  x11_cap *c = ccord_calloc(1, sizeof *c);
  if (!c) {
    XCloseDisplay(dpy);
    ccord_error_set(err, CCORD_ERR_NOMEM, "x11: out of memory");
    return nullptr;
  }
  c->base.ops = &k_x11_ops;
  c->base.src = *src;
  c->dpy = dpy;
  c->root = DefaultRootWindow(dpy);
  c->shm_supported = XShmQueryExtension(dpy) != 0;
  if (src->kind == CCORD_CAPTURE_SCREEN && (c->base.src.width <= 0 || c->base.src.height <= 0)) {
    int scr = DefaultScreen(dpy);
    c->base.src.x = 0;
    c->base.src.y = 0;
    c->base.src.width = DisplayWidth(dpy, scr);
    c->base.src.height = DisplayHeight(dpy, scr);
  }
  if (src->kind == CCORD_CAPTURE_WINDOW) {
    c->win = (Window)src->native;
    int ev_base = 0, err_base = 0;
    if (XCompositeQueryExtension(dpy, &ev_base, &err_base)) {
      int (*prev_handler)(Display *, XErrorEvent *) = XSetErrorHandler(x11_ignore_error);
      g_x11_err = false;
      XCompositeRedirectWindow(dpy, c->win, CompositeRedirectAutomatic);
      XSync(dpy, False);
      c->has_composite = !g_x11_err;
      XSetErrorHandler(prev_handler);
    }
  }
  return &c->base;
}

const ccord_capture_backend ccord_capture_x11_backend = {
  .name = "X11",
  .available = x11_available,
  .list = x11_list,
  .open = x11_open,
  .thumbnail = nullptr,
};
#else
const ccord_capture_backend ccord_capture_x11_backend = {.name = "X11"};
#endif
