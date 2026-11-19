// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
/* M5 camera plane: SDL3 camera enumeration, open/grab/close (BGRA frames).
 *
 * Camera capture is deliberately independent of the audio plane: the dummy
 * SDL drivers report zero cameras, so list/open stay honest (empty list,
 * NOT_FOUND) instead of fabricating a device. A handle borrows the CAMERA
 * subsystem when nothing else has initialized it and returns it on close. */
#include "devices_internal.h"

#include <SDL3/SDL.h>
#include <ccord/core/alloc.h>
#include <ccord/core/error.h>
#include <ccord/platform/platform.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct ccord_camera {
  SDL_Camera *cam;
  bool cam_init; /* this handle borrowed SDL_INIT_CAMERA; quit it on close */
};

size_t ccord_camera_list(ccord_camera_info *out, size_t max)
{
  if (!out || max == 0) return 0;
  bool cam_init = false;
  if (SDL_WasInit(SDL_INIT_CAMERA) == 0) {
    if (!SDL_InitSubSystem(SDL_INIT_CAMERA)) return 0;
    cam_init = true;
  }
  int count = 0;
  SDL_CameraID *ids = SDL_GetCameras(&count);
  size_t n = 0;
  if (ids && count > 0) {
    for (int i = 0; i < count && n < max; ++i) {
      char idbuf[64];
      snprintf(idbuf, sizeof idbuf, "%u", (unsigned)ids[i]);
      ccord_devices_copy_utf8(out[n].id, sizeof out[n].id, idbuf);
      const char *name = SDL_GetCameraName(ids[i]);
      ccord_devices_copy_utf8(out[n].name, sizeof out[n].name, name ? name : "Camera");
      ++n;
    }
  }
  SDL_free(ids);
  if (cam_init) SDL_QuitSubSystem(SDL_INIT_CAMERA);
  return n;
}

ccord_camera *ccord_camera_open(const char *device_id, ccord_error *err)
{
  ccord_error_clear(err);
  uint32_t id = 0;
  if (!ccord_devices_parse_id(device_id, &id)) {
    ccord_error_set(err, CCORD_ERR_INVALID, "camera: invalid device id");
    return nullptr;
  }
  bool cam_init = false;
  if (SDL_WasInit(SDL_INIT_CAMERA) == 0) {
    if (!SDL_InitSubSystem(SDL_INIT_CAMERA)) {
      ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "camera subsystem unavailable: %s", SDL_GetError());
      return nullptr;
    }
    cam_init = true;
  }
  /* Ask SDL to convert native frames to BGRA32 so grab() can hand the UI a
   * stable layout; width/height/framerate 0 keeps the device's first spec. */
  SDL_CameraSpec spec = {
    .format = SDL_PIXELFORMAT_BGRA32,
    .width = 0,
    .height = 0,
    .framerate_numerator = 0,
    .framerate_denominator = 0,
  };
  SDL_Camera *cam = SDL_OpenCamera((SDL_CameraID)id, &spec);
  if (!cam) {
    if (cam_init) SDL_QuitSubSystem(SDL_INIT_CAMERA);
    ccord_error_set(err, CCORD_ERR_NOT_FOUND, "camera %u unavailable: %s", (unsigned)id, SDL_GetError());
    return nullptr;
  }
  ccord_camera *c = ccord_calloc(1, sizeof *c);
  if (!c) {
    SDL_CloseCamera(cam);
    if (cam_init) SDL_QuitSubSystem(SDL_INIT_CAMERA);
    ccord_error_set(err, CCORD_ERR_NOMEM, "out of memory");
    return nullptr;
  }
  c->cam = cam;
  c->cam_init = cam_init;
  return c;
}

bool ccord_camera_grab(ccord_camera *c, uint8_t *bgra, size_t cap, int *out_w, int *out_h)
{
  if (!c || !c->cam || !bgra) return false;
  Uint64 timestamp_ns = 0;
  SDL_Surface *frame = SDL_AcquireCameraFrame(c->cam, &timestamp_ns);
  if (!frame) return false; /* no frame ready yet; not an error */
  bool ok = false;
  SDL_Surface *converted = nullptr;
  SDL_Surface *use = frame;
  if (frame->format != SDL_PIXELFORMAT_BGRA32 && frame->w > 0 && frame->h > 0) {
    // Android Camera2 delivers NV21/IYUV frames; convert to BGRA32 so the UI pipeline stays uniform.
    converted = SDL_ConvertSurface(frame, SDL_PIXELFORMAT_BGRA32);
    if (converted) use = converted;
  }
  if (use->format == SDL_PIXELFORMAT_BGRA32 && use->w > 0 && use->h > 0 && use->pitch >= 0) {
    size_t w = (size_t)use->w;
    size_t h = (size_t)use->h;
    size_t pitch = (size_t)use->pitch;
    if (pitch >= w * 4 && cap >= w * h * 4) {
      const uint8_t *src = (const uint8_t *)use->pixels;
      for (size_t y = 0; y < h; ++y) {
        uint8_t *dst = bgra + y * w * 4;
        const uint8_t *row = src + y * pitch;
        for (size_t x = 0; x < w * 4; ++x) dst[x] = row[x];
      }
      if (out_w) *out_w = use->w;
      if (out_h) *out_h = use->h;
      ok = true;
    }
  }
  if (converted) SDL_DestroySurface(converted);
  SDL_ReleaseCameraFrame(c->cam, frame);
  return ok;
}

void ccord_camera_close(ccord_camera *c)
{
  if (!c) return;
  if (c->cam) SDL_CloseCamera(c->cam);
  if (c->cam_init) SDL_QuitSubSystem(SDL_INIT_CAMERA);
  ccord_free(c);
}
