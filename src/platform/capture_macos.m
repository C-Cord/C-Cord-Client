// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Native macOS screen & window capture via CoreGraphics / ScreenCaptureKit.
// Zero third-party dependencies beyond Apple system frameworks (CoreGraphics, ScreenCaptureKit, CoreVideo, CoreMedia).
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
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

typedef struct macos_cap {
  ccord_capture base;
  CGDirectDisplayID display_id;
  CGWindowID window_id;
  uint8_t *buf;
  size_t buf_cap;
  int w, h, stride;
  bool has_frame;
  bool dead;
  ccord_mutex *mtx;
#if CCORD_HAVE_SCK
  id sc_stream;    // SCStream*
  id sc_output;    // CCordSCOutput*
#endif
} macos_cap;

#if CCORD_HAVE_SCK
API_AVAILABLE(macos(12.3))
@interface CCordSCOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
  macos_cap *cap;
}
@end

API_AVAILABLE(macos(12.3))
@implementation CCordSCOutput
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
  (void)stream;
  if (type != SCStreamOutputTypeScreen || !cap || !sampleBuffer) return;
  CVImageBufferRef img = CMSampleBufferGetImageBuffer(sampleBuffer);
  if (!img) return;
  if (CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
  size_t w = CVPixelBufferGetWidth(img);
  size_t h = CVPixelBufferGetHeight(img);
  size_t bpr = CVPixelBufferGetBytesPerRow(img);
  const uint8_t *src = (const uint8_t *)CVPixelBufferGetBaseAddress(img);
  if (src && w > 0 && h > 0 && bpr >= w * 4u) {
    size_t need = w * h * 4u;
    ccord_mutex_lock(cap->mtx);
    if (need > cap->buf_cap) {
      uint8_t *nb = (uint8_t *)ccord_realloc(cap->buf, need);
      if (nb) {
        cap->buf = nb;
        cap->buf_cap = need;
      }
    }
    if (cap->buf && cap->buf_cap >= need) {
      for (size_t y = 0; y < h; y++) {
        memcpy(cap->buf + y * w * 4u, src + y * bpr, w * 4u);
      }
      cap->w = (int)w;
      cap->h = (int)h;
      cap->stride = (int)(w * 4u);
      cap->has_frame = true;
    }
    ccord_mutex_unlock(cap->mtx);
  }
  CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
  (void)stream;
  (void)error;
  if (cap) {
    ccord_mutex_lock(cap->mtx);
    cap->dead = true;
    ccord_mutex_unlock(cap->mtx);
  }
}
@end
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
  ccord_mutex_lock(c->mtx);
  if (c->dead) {
    ccord_mutex_unlock(c->mtx);
    return false;
  }
  bool ok = c->has_frame && c->buf != NULL && c->w > 0 && c->h > 0;
  if (ok) {
    *out = (ccord_capture_frame){.data = c->buf, .width = c->w, .height = c->h, .stride = c->stride};
  }
  ccord_mutex_unlock(c->mtx);
  return ok;
}

static bool macos_failed(const ccord_capture *base)
{
  const macos_cap *c = (const macos_cap *)base;
  return c->dead;
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
        [st stopCaptureWithCompletionHandler:^(NSError *error) {
          (void)error;
          dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 500LL * NSEC_PER_MSEC));
        [st release];
        c->sc_stream = nil;
      }
      if (c->sc_output) {
        CCordSCOutput *outp = (CCordSCOutput *)c->sc_output;
        outp->cap = NULL;
        [outp release];
        c->sc_output = nil;
      }
    }
  }
#endif
  ccord_mutex_destroy(c->mtx);
  ccord_free(c->buf);
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
      __block SCContentFilter *filter = nil;
      __block int cap_w = src->width > 0 ? src->width : 1280;
      __block int cap_h = src->height > 0 ? src->height : 720;
      dispatch_semaphore_t sem = dispatch_semaphore_create(0);
      [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content, NSError *sc_err) {
        (void)sc_err;
        if (content) {
          if (src->kind == CCORD_CAPTURE_WINDOW) {
            for (SCWindow *w in content.windows) {
              if (w.windowID == c->window_id) {
                filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:w];
                if (w.frame.size.width > 0 && w.frame.size.height > 0) {
                  cap_w = (int)w.frame.size.width;
                  cap_h = (int)w.frame.size.height;
                }
                break;
              }
            }
          } else {
            SCDisplay *chosen = nil;
            for (SCDisplay *d in content.displays) {
              if (d.displayID == c->display_id) { chosen = d; break; }
            }
            if (!chosen && content.displays.count > 0) chosen = content.displays.firstObject;
            if (chosen) {
              filter = [[SCContentFilter alloc] initWithDisplay:chosen excludingWindows:@[]];
              cap_w = (int)chosen.width;
              cap_h = (int)chosen.height;
            }
          }
        }
        dispatch_semaphore_signal(sem);
      }];
      dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2000LL * NSEC_PER_MSEC));

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
      NSError *add_err = nil;
      BOOL added = [stream addStreamOutput:outp type:SCStreamOutputTypeScreen sampleHandlerQueue:q error:&add_err];
      if (!added) {
        [stream release];
        outp->cap = NULL;
        [outp release];
        macos_close(&c->base);
        ccord_error_set(err, CCORD_ERR_IO, "capture: failed to attach ScreenCaptureKit output");
        return NULL;
      }
      __block BOOL started = NO;
      dispatch_semaphore_t start_sem = dispatch_semaphore_create(0);
      [stream startCaptureWithCompletionHandler:^(NSError *st_err) {
        started = (st_err == nil);
        dispatch_semaphore_signal(start_sem);
      }];
      dispatch_semaphore_wait(start_sem, dispatch_time(DISPATCH_TIME_NOW, 2000LL * NSEC_PER_MSEC));
      if (!started) {
        [stream release];
        outp->cap = NULL;
        [outp release];
        macos_close(&c->base);
        ccord_error_set(err, CCORD_ERR_PERMISSION, "capture: ScreenCaptureKit start failed");
        return NULL;
      }
      c->sc_stream = stream;
      c->sc_output = outp;
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
