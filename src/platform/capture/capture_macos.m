// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Native macOS screen & window capture via CoreGraphics / ScreenCaptureKit.
// Zero third-party dependencies beyond Apple system frameworks (CoreGraphics, ScreenCaptureKit, CoreVideo, CoreMedia).
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <os/lock.h>
#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#define CCORD_HAVE_SCK 1
#else
#define CCORD_HAVE_SCK 0
#endif

#include <ccord/platform/capture.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(objc_arc)
#define CCORD_MACOS_ARC 1
#else
#define CCORD_MACOS_ARC 0
#endif
#else
#define CCORD_MACOS_ARC 0
#endif

#if CCORD_MACOS_ARC
#define CCORD_DISPATCH_RETAIN(obj) ((void)(obj))
#define CCORD_DISPATCH_RELEASE(obj) ((void)(obj))
#else
#define CCORD_DISPATCH_RETAIN(obj) do { if (obj) dispatch_retain(obj); } while (0)
#define CCORD_DISPATCH_RELEASE(obj) do { if (obj) dispatch_release(obj); } while (0)
#endif

typedef struct macos_cap {
  ccord_capture base;
  CGDirectDisplayID display_id;
  CGWindowID window_id;
  // Triple-buffer pointer swap: front_buf is owned exclusively by the grab() caller between grabs,
  // ready_buf is swapped under mtx, and back_buf is written unlocked on the serial sc_queue.
  uint8_t *front_buf;
  size_t front_cap;
  int front_w, front_h, front_stride;
  uint8_t *ready_buf;
  size_t ready_cap;
  int ready_w, ready_h, ready_stride;
  uint8_t *back_buf;
  size_t back_cap;
  bool has_frame;
  bool dead;
  ccord_mutex *mtx;
#if CCORD_HAVE_SCK
  id sc_stream;             // SCStream*
  id sc_output;             // CCordSCOutput*
  dispatch_queue_t sc_queue;
#endif
} macos_cap;

#if CCORD_HAVE_SCK
API_AVAILABLE(macos(12.3))
@interface CCordSCOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
  macos_cap *cap;
@private
  os_unfair_lock cap_lock;
}
- (void)detach;
@end

API_AVAILABLE(macos(12.3))
@implementation CCordSCOutput
- (instancetype)init {
  self = [super init];
  if (self) {
    cap_lock = OS_UNFAIR_LOCK_INIT;
  }
  return self;
}

- (void)detach {
  os_unfair_lock_lock(&cap_lock);
  cap = NULL;
  os_unfair_lock_unlock(&cap_lock);
}

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
  (void)stream;
  if (type != SCStreamOutputTypeScreen || !sampleBuffer) return;
  os_unfair_lock_lock(&cap_lock);
  macos_cap *c = cap;
  if (!c) {
    os_unfair_lock_unlock(&cap_lock);
    return;
  }
  CVImageBufferRef img = CMSampleBufferGetImageBuffer(sampleBuffer);
  if (!img) {
    os_unfair_lock_unlock(&cap_lock);
    return;
  }
  if (CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
    os_unfair_lock_unlock(&cap_lock);
    return;
  }
  size_t w = CVPixelBufferGetWidth(img);
  size_t h = CVPixelBufferGetHeight(img);
  size_t bpr = CVPixelBufferGetBytesPerRow(img);
  const uint8_t *src = (const uint8_t *)CVPixelBufferGetBaseAddress(img);
  if (src && w > 0 && h > 0 && w <= SIZE_MAX / 4u && bpr >= w * 4u && h <= SIZE_MAX / (w * 4u)) {
    const size_t row_bytes = w * 4u;
    const size_t need = row_bytes * h;
    // Grow and populate the producer-private back_buf outside c->mtx (serial sc_queue only).
    if (need > c->back_cap) {
      uint8_t *nb = (uint8_t *)ccord_malloc(need);
      if (nb) {
        ccord_free(c->back_buf);
        c->back_buf = nb;
        c->back_cap = need;
      }
    }
    if (c->back_buf && c->back_cap >= need) {
      if (bpr == row_bytes) {
        memcpy(c->back_buf, src, need);
      } else {
        for (size_t y = 0; y < h; y++) {
          memcpy(c->back_buf + y * row_bytes, src + y * bpr, row_bytes);
        }
      }
      // Publish completed frame via O(1) pointer swap under mtx.
      ccord_mutex_lock(c->mtx);
      uint8_t *old_ready = c->ready_buf;
      size_t old_ready_cap = c->ready_cap;
      c->ready_buf = c->back_buf;
      c->ready_cap = c->back_cap;
      c->back_buf = old_ready;
      c->back_cap = old_ready_cap;
      c->ready_w = (int)w;
      c->ready_h = (int)h;
      c->ready_stride = (int)row_bytes;
      c->has_frame = true;
      ccord_mutex_unlock(c->mtx);
    }
  }
  CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
  os_unfair_lock_unlock(&cap_lock);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
  (void)stream;
  (void)error;
  os_unfair_lock_lock(&cap_lock);
  macos_cap *c = cap;
  if (c) {
    ccord_mutex_lock(c->mtx);
    c->dead = true;
    ccord_mutex_unlock(c->mtx);
  }
  os_unfair_lock_unlock(&cap_lock);
}
@end

typedef struct macos_pick_ctx {
  ccord_mutex *mtx;
  bool done;
  bool abandoned;
  id filter;   // SCContentFilter*
  int cap_w;
  int cap_h;
} macos_pick_ctx;

typedef struct macos_start_ctx {
  ccord_mutex *mtx;
  bool done;
  bool abandoned;
  bool started;
} macos_start_ctx;
#endif

static bool macos_available(void) { return true; }

static size_t macos_list(ccord_capture_kind kind, ccord_capture_source *out, size_t max, ccord_error *err)
{
  (void)err;
  if (kind == CCORD_CAPTURE_SCREEN) {
    CGDirectDisplayID displays[16];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(16, displays, &count) != kCGErrorSuccess) return 0;
    size_t n = 0;
    for (uint32_t i = 0; i < count && n < max; i++) {
      CGRect bounds = CGDisplayBounds(displays[i]);
      ccord_capture_source *s = &out[n++];
      memset(s, 0, sizeof *s);
      s->kind = CCORD_CAPTURE_SCREEN;
      s->backend = CCORD_CAPBE_MACOS;
      s->primary = CGDisplayIsMain(displays[i]) != 0;
      s->x = (int)bounds.origin.x;
      s->y = (int)bounds.origin.y;
      s->width = (int)bounds.size.width;
      s->height = (int)bounds.size.height;
      s->native = (uint64_t)displays[i];
      snprintf(s->id, sizeof s->id, "macos:mon:%u", (unsigned)displays[i]);
      snprintf(s->title, sizeof s->title, "Display %u%s", (unsigned)(i + 1u), s->primary ? " (Main)" : "");
    }
    for (size_t i = 1; i < n; i++) {
      if (out[i].primary) {
        ccord_capture_source tmp = out[0];
        out[0] = out[i];
        out[i] = tmp;
        break;
      }
    }
    return n;
  }

  @autoreleasepool {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
                                                 kCGNullWindowID);
    if (!list) return 0;
    int own = ccord_capture_own_pid();
    if (own <= 0) own = (int)getpid();
    size_t n = 0;
    CFIndex total = CFArrayGetCount(list);
    for (CFIndex i = 0; i < total && n < max; i++) {
      NSDictionary *info = (NSDictionary *)CFArrayGetValueAtIndex(list, i);
      NSNumber *layer = info[(NSString *)kCGWindowLayer];
      if (!layer || layer.intValue != 0) continue;
      NSNumber *pid = info[(NSString *)kCGWindowOwnerPID];
      if (pid && pid.intValue == own) continue;
      NSNumber *wid = info[(NSString *)kCGWindowNumber];
      if (!wid) continue;
      NSDictionary *bdict = info[(NSString *)kCGWindowBounds];
      CGRect rect = CGRectZero;
      if (!bdict || !CGRectMakeWithDictionaryRepresentation((CFDictionaryRef)bdict, &rect)) continue;
      if (rect.size.width < 64 || rect.size.height < 64) continue;
      NSString *name = info[(NSString *)kCGWindowName];
      NSString *owner = info[(NSString *)kCGWindowOwnerName];
      const char *title_c = (name && name.length > 0) ? name.UTF8String : (owner ? owner.UTF8String : "Window");
      const char *app_c = owner ? owner.UTF8String : "";

      ccord_capture_source *s = &out[n++];
      memset(s, 0, sizeof *s);
      s->kind = CCORD_CAPTURE_WINDOW;
      s->backend = CCORD_CAPBE_MACOS;
      s->x = (int)rect.origin.x;
      s->y = (int)rect.origin.y;
      s->width = (int)rect.size.width;
      s->height = (int)rect.size.height;
      s->native = (uint64_t)wid.unsignedIntValue;
      snprintf(s->id, sizeof s->id, "macos:win:%u", wid.unsignedIntValue);
      snprintf(s->title, sizeof s->title, "%s", title_c);
      snprintf(s->app, sizeof s->app, "%s", app_c);
    }
    CFRelease(list);
    return n;
  }
}

static bool macos_grab(ccord_capture *base, ccord_capture_frame *out)
{
  macos_cap *c = (macos_cap *)base;
  bool ok = false;
  ccord_mutex_lock(c->mtx);
  if (!c->dead) {
    if (c->has_frame && c->ready_buf != NULL && c->ready_w > 0 && c->ready_h > 0) {
      // O(1) pointer swap: caller owns front_buf exclusively until the next grab/close.
      uint8_t *prev = c->front_buf;
      size_t prev_cap = c->front_cap;
      c->front_buf = c->ready_buf;
      c->front_cap = c->ready_cap;
      c->ready_buf = prev;
      c->ready_cap = prev_cap;
      c->front_w = c->ready_w;
      c->front_h = c->ready_h;
      c->front_stride = c->ready_stride;
      c->has_frame = false;
    }
    ok = c->front_buf != NULL && c->front_w > 0 && c->front_h > 0;
  }
  ccord_mutex_unlock(c->mtx);
  if (ok) {
    *out = (ccord_capture_frame){.data = c->front_buf, .width = c->front_w, .height = c->front_h,
                                 .stride = c->front_stride};
  }
  return ok;
}

static bool macos_failed(const ccord_capture *base)
{
  const macos_cap *c = (const macos_cap *)base;
  ccord_mutex_lock(c->mtx);
  bool dead = c->dead;
  ccord_mutex_unlock(c->mtx);
  return dead;
}

static void macos_close(ccord_capture *base)
{
  macos_cap *c = (macos_cap *)base;
#if CCORD_HAVE_SCK
  if (@available(macOS 12.3, *)) {
    @autoreleasepool {
      if (c->sc_stream) {
        SCStream *st = (SCStream *)c->sc_stream;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        if (sem) {
          CCORD_DISPATCH_RETAIN(sem);
          [st stopCaptureWithCompletionHandler:^(NSError *error) {
            (void)error;
            dispatch_semaphore_signal(sem);
            CCORD_DISPATCH_RELEASE(sem);
          }];
          dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 500LL * NSEC_PER_MSEC));
          CCORD_DISPATCH_RELEASE(sem);
        } else {
          [st stopCaptureWithCompletionHandler:^(NSError *error) { (void)error; }];
        }
        [st release];
        c->sc_stream = nil;
      }
      if (c->sc_output) {
        CCordSCOutput *outp = (CCordSCOutput *)c->sc_output;
        // Synchronously detach both SCStreamOutput (sc_queue) and SCStreamDelegate (arbitrary queue)
        // callbacks before destroying c->mtx and freeing c.
        [outp detach];
        if (c->sc_queue) {
          dispatch_sync(c->sc_queue, ^{});
        }
        [outp release];
        c->sc_output = nil;
      }
      if (c->sc_queue) {
        CCORD_DISPATCH_RELEASE(c->sc_queue);
        c->sc_queue = NULL;
      }
    }
  }
#endif
  ccord_mutex_destroy(c->mtx);
  ccord_free(c->front_buf);
  ccord_free(c->ready_buf);
  ccord_free(c->back_buf);
  ccord_free(c);
}

static const ccord_capture_ops k_macos_ops = {.close = macos_close, .grab = macos_grab, .failed = macos_failed};

static ccord_capture *macos_open(const ccord_capture_source *src, ccord_error *err)
{
  macos_cap *c = ccord_calloc(1, sizeof *c);
  if (!c) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "capture: out of memory");
    return NULL;
  }
  c->mtx = ccord_mutex_create();
  if (!c->mtx) {
    ccord_free(c);
    ccord_error_set(err, CCORD_ERR_NOMEM, "capture: out of memory");
    return NULL;
  }
  c->base.ops = &k_macos_ops;
  c->base.src = *src;
  if (src->kind == CCORD_CAPTURE_WINDOW) {
    c->window_id = (CGWindowID)src->native;
  } else {
    c->display_id = src->native ? (CGDirectDisplayID)src->native : CGMainDisplayID();
  }

#if CCORD_HAVE_SCK
  if (@available(macOS 12.3, *)) {
    @autoreleasepool {
      macos_pick_ctx *pctx = (macos_pick_ctx *)ccord_calloc(1, sizeof *pctx);
      dispatch_semaphore_t sem = dispatch_semaphore_create(0);
      if (!pctx || !sem) {
        if (sem) CCORD_DISPATCH_RELEASE(sem);
        ccord_free(pctx);
        macos_close(&c->base);
        ccord_error_set(err, CCORD_ERR_NOMEM, "capture: out of memory");
        return NULL;
      }
      pctx->mtx = ccord_mutex_create();
      if (!pctx->mtx) {
        CCORD_DISPATCH_RELEASE(sem);
        ccord_free(pctx);
        macos_close(&c->base);
        ccord_error_set(err, CCORD_ERR_NOMEM, "capture: out of memory");
        return NULL;
      }
      pctx->cap_w = src->width > 0 ? src->width : 1280;
      pctx->cap_h = src->height > 0 ? src->height : 720;
      const ccord_capture_kind req_kind = src->kind;
      const CGWindowID req_win = c->window_id;
      const CGDirectDisplayID req_disp = c->display_id;

      CCORD_DISPATCH_RETAIN(sem);
      [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content, NSError *sc_err) {
        (void)sc_err;
        SCContentFilter *local_filter = nil;
        int local_w = 0;
        int local_h = 0;
        if (content) {
          if (req_kind == CCORD_CAPTURE_WINDOW) {
            for (SCWindow *w in content.windows) {
              if (w.windowID == req_win) {
                local_filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:w];
                if (w.frame.size.width > 0 && w.frame.size.height > 0) {
                  local_w = (int)w.frame.size.width;
                  local_h = (int)w.frame.size.height;
                }
                break;
              }
            }
          } else {
            SCDisplay *chosen = nil;
            for (SCDisplay *d in content.displays) {
              if (d.displayID == req_disp) { chosen = d; break; }
            }
            if (!chosen && content.displays.count > 0) chosen = content.displays.firstObject;
            if (chosen) {
              local_filter = [[SCContentFilter alloc] initWithDisplay:chosen excludingWindows:@[]];
              local_w = (int)chosen.width;
              local_h = (int)chosen.height;
            }
          }
        }
        ccord_mutex_lock(pctx->mtx);
        bool abandoned = pctx->abandoned;
        if (!abandoned) {
          pctx->filter = local_filter;
          if (local_w > 0 && local_h > 0) {
            pctx->cap_w = local_w;
            pctx->cap_h = local_h;
          }
          pctx->done = true;
        }
        ccord_mutex_unlock(pctx->mtx);
        if (abandoned) {
          [local_filter release];
          ccord_mutex_destroy(pctx->mtx);
          ccord_free(pctx);
        }
        dispatch_semaphore_signal(sem);
        CCORD_DISPATCH_RELEASE(sem);
      }];
      dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2000LL * NSEC_PER_MSEC));
      CCORD_DISPATCH_RELEASE(sem);

      SCContentFilter *filter = nil;
      int cap_w = 1280;
      int cap_h = 720;
      ccord_mutex_lock(pctx->mtx);
      bool pick_done = pctx->done;
      if (pick_done) {
        filter = (SCContentFilter *)pctx->filter;
        cap_w = pctx->cap_w;
        cap_h = pctx->cap_h;
      } else {
        pctx->abandoned = true;
      }
      ccord_mutex_unlock(pctx->mtx);
      if (pick_done) {
        ccord_mutex_destroy(pctx->mtx);
        ccord_free(pctx);
      }

      if (!filter) {
        macos_close(&c->base);
        ccord_error_set(err, CCORD_ERR_PERMISSION, "capture: Screen Recording permission or target unavailable");
        return NULL;
      }

      SCStreamConfiguration *cfg = [[SCStreamConfiguration alloc] init];
      cfg.width = (size_t)cap_w;
      cfg.height = (size_t)cap_h;
      cfg.pixelFormat = kCVPixelFormatType_32BGRA;
      cfg.minimumFrameInterval = CMTimeMake(1, 60);
      cfg.showsCursor = YES;

      CCordSCOutput *outp = [[CCordSCOutput alloc] init];
      outp->cap = c;
      SCStream *stream = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:outp];
      [filter release];
      [cfg release];
      if (!stream) {
        outp->cap = NULL;
        [outp release];
        macos_close(&c->base);
        ccord_error_set(err, CCORD_ERR_IO, "capture: failed to create ScreenCaptureKit stream");
        return NULL;
      }
      dispatch_queue_t q = dispatch_queue_create("org.ccord.capture", DISPATCH_QUEUE_SERIAL);
      if (!q) {
        [stream release];
        outp->cap = NULL;
        [outp release];
        macos_close(&c->base);
        ccord_error_set(err, CCORD_ERR_NOMEM, "capture: out of memory");
        return NULL;
      }
      NSError *add_err = nil;
      BOOL added = [stream addStreamOutput:outp type:SCStreamOutputTypeScreen sampleHandlerQueue:q error:&add_err];
      if (!added) {
        [stream release];
        dispatch_sync(q, ^{ outp->cap = NULL; });
        [outp release];
        CCORD_DISPATCH_RELEASE(q);
        macos_close(&c->base);
        ccord_error_set(err, CCORD_ERR_IO, "capture: failed to attach ScreenCaptureKit output");
        return NULL;
      }
      macos_start_ctx *sctx = (macos_start_ctx *)ccord_calloc(1, sizeof *sctx);
      dispatch_semaphore_t start_sem = dispatch_semaphore_create(0);
      if (!sctx || !start_sem || !(sctx->mtx = ccord_mutex_create())) {
        if (start_sem) CCORD_DISPATCH_RELEASE(start_sem);
        ccord_free(sctx);
        [stream release];
        dispatch_sync(q, ^{ outp->cap = NULL; });
        [outp release];
        CCORD_DISPATCH_RELEASE(q);
        macos_close(&c->base);
        ccord_error_set(err, CCORD_ERR_NOMEM, "capture: out of memory");
        return NULL;
      }
      CCORD_DISPATCH_RETAIN(start_sem);
      [stream startCaptureWithCompletionHandler:^(NSError *st_err) {
        ccord_mutex_lock(sctx->mtx);
        bool abandoned = sctx->abandoned;
        if (!abandoned) {
          sctx->started = (st_err == nil);
          sctx->done = true;
        }
        ccord_mutex_unlock(sctx->mtx);
        if (abandoned) {
          ccord_mutex_destroy(sctx->mtx);
          ccord_free(sctx);
        }
        dispatch_semaphore_signal(start_sem);
        CCORD_DISPATCH_RELEASE(start_sem);
      }];
      dispatch_semaphore_wait(start_sem, dispatch_time(DISPATCH_TIME_NOW, 2000LL * NSEC_PER_MSEC));
      CCORD_DISPATCH_RELEASE(start_sem);

      bool started = false;
      ccord_mutex_lock(sctx->mtx);
      bool start_done = sctx->done;
      if (start_done) {
        started = sctx->started;
      } else {
        sctx->abandoned = true;
      }
      ccord_mutex_unlock(sctx->mtx);
      if (start_done) {
        ccord_mutex_destroy(sctx->mtx);
        ccord_free(sctx);
      }
      if (!started) {
        [outp detach];
        [stream stopCaptureWithCompletionHandler:^(NSError *stop_err) { (void)stop_err; }];
        [stream release];
        dispatch_sync(q, ^{});
        [outp release];
        CCORD_DISPATCH_RELEASE(q);
        macos_close(&c->base);
        ccord_error_set(err, CCORD_ERR_PERMISSION, "capture: ScreenCaptureKit start failed");
        return NULL;
      }
      c->sc_stream = stream;
      c->sc_output = outp;
      c->sc_queue = q;
      return &c->base;
    }
  }
#endif
  macos_close(&c->base);
  ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "capture: requires macOS 12.3+ ScreenCaptureKit");
  return NULL;
}

const ccord_capture_backend ccord_capture_macos_backend = {
  .name = "ScreenCaptureKit",
  .available = macos_available,
  .list = macos_list,
  .open = macos_open,
  .thumbnail = NULL,
};
