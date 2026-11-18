// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M5 device plane tests: audio enumeration (borrowed subsystem init), device-id
// selection with default fallback, hot-plug callback delivery through
// ccord_platform_next_event, removal -> reopen-on-default (white-box via
// devices_internal.h), and the camera list/open/grab/close plane.
//
// Everything deterministic runs under the dummy SDL drivers (one synthetic audio
// device per direction, zero cameras). Only the real-camera lifecycle/grab branch
// is conditional on hardware; headless hosts print a note instead of failing.
#define _POSIX_C_SOURCE 200809L
#include <ccord/platform/platform.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/error.h>
#include <ccord/core/input.h>

#include "ccord_test.h"
#include "devices_internal.h"

static void drain(ccord_platform *p)
{
  ccord_input_event ev;
  while (ccord_platform_next_event(p, &ev, 0)) {
  }
}

static void push_audio_event(Uint32 type, SDL_AudioDeviceID which, bool recording)
{
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = type;
  e.adevice.which = which;
  e.adevice.recording = recording;
  (void)SDL_PushEvent(&e);
}

// ---- enumeration (must run before anything else initializes SDL audio) ----

static void test_enumeration(void)
{
  CT_CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);
  ccord_audio_device_info pb[8], rec[8];
  size_t npb = ccord_audio_devices_list(false, pb, 8);
  // Enumeration borrows SDL_INIT_AUDIO and returns it afterwards.
  CT_CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);
  size_t nrec = ccord_audio_devices_list(true, rec, 8);
  CT_CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);
  CT_CHECK(npb >= 1 && nrec >= 1);   // dummy driver: one device per direction
  CT_CHECK(ccord_audio_devices_list(false, nullptr, 8) == 0);
  CT_CHECK(ccord_audio_devices_list(false, pb, 0) == 0);
  if (npb == 0 || nrec == 0) return;
  for (size_t i = 0; i < npb; ++i) {
    uint32_t id = 0;
    CT_CHECK(ccord_devices_parse_id(pb[i].id, &id));   // decimal, non-zero
    CT_CHECK(pb[i].name[0] != '\0');
  }
  for (size_t i = 0; i < nrec; ++i) {
    uint32_t id = 0;
    CT_CHECK(ccord_devices_parse_id(rec[i].id, &id));
    CT_CHECK(rec[i].name[0] != '\0');
  }
  // Playback and recording are distinct SDL device objects.
  CT_CHECK(strcmp(pb[0].id, rec[0].id) != 0);
  // With a single device per direction (dummy), it is the default.
  if (npb == 1) CT_CHECK(pb[0].is_default);
  if (nrec == 1) CT_CHECK(rec[0].is_default);
  // max caps the number of entries written.
  ccord_audio_device_info one[1];
  CT_CHECK(ccord_audio_devices_list(false, one, 1) == 1);
}

// ---- device-id selection + fallback ----

static void test_device_id(void)
{
  ccord_error err;
  uint32_t v = 0;
  CT_CHECK(ccord_devices_parse_id("4294967295", &v) && v == UINT32_MAX);
  CT_CHECK(!ccord_devices_parse_id("4294967296", &v));
  CT_CHECK(!ccord_devices_parse_id("0", &v));
  CT_CHECK(!ccord_devices_parse_id("", &v));
  CT_CHECK(!ccord_devices_parse_id(nullptr, &v));
  CT_CHECK(!ccord_devices_parse_id("1", nullptr));
  CT_CHECK(ccord_devices_parse_id("15", &v) && v == 15);

  ccord_audio_device_info pb[8], rec[8];
  // Keep SDL audio initialized while we enumerate and open: the dummy driver
  // re-detects devices (with fresh physical ids) on every subsystem init, so
  // ids are only stable while the subsystem stays up -- as in the real app.
  ccord_audio *keeper = ccord_audio_create(nullptr, &err);
  CT_CHECK(keeper != nullptr && ccord_audio_available(keeper));
  if (!keeper) return;
  size_t npb = ccord_audio_devices_list(false, pb, 8);
  size_t nrec = ccord_audio_devices_list(true, rec, 8);
  CT_CHECK(npb >= 1 && nrec >= 1);
  if (npb == 0 || nrec == 0) {
    ccord_audio_destroy(keeper);
    return;
  }

  // Explicit physical id: kept as opened_id; SDL assigns a distinct logical id.
  ccord_audio_config cfg = {.capture = false, .device_id = pb[0].id};
  ccord_audio *a = ccord_audio_create(&cfg, &err);
  CT_CHECK(a != nullptr);
  if (!a) {
    ccord_audio_destroy(keeper);
    return;
  }
  CT_CHECK(ccord_audio_available(a));
  uint32_t phys = 0;
  CT_CHECK(ccord_devices_parse_id(pb[0].id, &phys));
  CT_CHECK(a->opened_id == phys);
  CT_CHECK(a->device != 0);
  CT_CHECK(a->device != a->opened_id);
  CT_CHECK(SDL_WasInit(SDL_INIT_AUDIO) != 0);
  const char *drv = SDL_GetCurrentAudioDriver();
  CT_CHECK(drv && strcmp(drv, "dummy") == 0);
  // The logical id resolves to the same device we picked by physical id.
  const char *lname = SDL_GetAudioDeviceName(a->device);
  CT_CHECK(lname && strcmp(lname, pb[0].name) == 0);
  int16_t pcm[64] = {0};
  CT_CHECK(ccord_audio_read(a, pcm, 32) == 0);     // playback handle: read is a no-op
  CT_CHECK(ccord_audio_write(a, pcm, 32) == 32);   // dummy sink accepts playback
  ccord_audio_destroy(a);

  // nullptr/""/malformed/zero/overflow ids all select the direction default.
  const char *bad[] = {nullptr, "", "0", "abc", "12x", "-1", " 1", "1 ", "4294967296", "99999999999999999999"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
    ccord_audio_config c2 = {.capture = false, .device_id = bad[i]};
    ccord_audio *b = ccord_audio_create(&c2, &err);
    CT_CHECK(b != nullptr);
    if (!b) continue;
    CT_CHECK(b->opened_id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    CT_CHECK(ccord_audio_available(b));
    ccord_audio_destroy(b);
  }

  // A well-formed but nonexistent id falls back to the default instead of going silent.
  ccord_audio_config c3 = {.capture = false, .device_id = "4294967290"};
  ccord_audio *f = ccord_audio_create(&c3, &err);
  CT_CHECK(f != nullptr && ccord_audio_available(f));
  if (f) {
    CT_CHECK(f->opened_id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    CT_CHECK(f->device != 0);
    ccord_audio_destroy(f);
  }

  // Capture defaults to the recording alias; explicit recording id is honored.
  ccord_audio_config c4 = {.capture = true};
  ccord_audio *cap = ccord_audio_create(&c4, &err);
  CT_CHECK(cap != nullptr && cap->opened_id == SDL_AUDIO_DEVICE_DEFAULT_RECORDING);
  if (cap) {
    CT_CHECK(ccord_audio_write(cap, pcm, 32) == 0);   // capture handle: write is a no-op
    ccord_audio_destroy(cap);
  }
  ccord_audio_config c5 = {.capture = true, .device_id = rec[0].id};
  ccord_audio *cap2 = ccord_audio_create(&c5, &err);
  CT_CHECK(cap2 != nullptr);
  if (cap2) {
    uint32_t rphys = 0;
    CT_CHECK(ccord_devices_parse_id(rec[0].id, &rphys));
    CT_CHECK(cap2->opened_id == rphys);
    CT_CHECK(cap2->capture);
    ccord_audio_destroy(cap2);
  }

  // No config at all: playback defaults.
  ccord_audio *d = ccord_audio_create(nullptr, &err);
  CT_CHECK(d != nullptr && d->opened_id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK && !d->capture);
  if (d) ccord_audio_destroy(d);
  ccord_audio_destroy(keeper);
  CT_CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);
}

// ---- hot-plug callback delivery through the platform event pump ----

static struct {
  bool capture;
  bool added;
} s_hp[16];
static size_t s_hp_n;
static int s_hp_ud_marker;

static void on_hotplug(void *ud, bool capture, bool added)
{
  CT_CHECK(ud == &s_hp_ud_marker);
  if (s_hp_n < sizeof s_hp / sizeof s_hp[0]) {
    s_hp[s_hp_n].capture = capture;
    s_hp[s_hp_n].added = added;
  }
  s_hp_n++;
}

static ccord_platform *make_platform(void)
{
  ccord_error err;
  ccord_platform_config cfg = {.title = "devices", .width = 320, .height = 200, .headless = true};
  ccord_platform *p = ccord_platform_create(&cfg, &err);
  CT_CHECK(p != nullptr);
  if (p) drain(p);
  return p;
}

static void test_hotplug(void)
{
  ccord_platform *p = make_platform();
  if (!p) return;
  s_hp_n = 0;
  ccord_audio_set_hotplug_callback(on_hotplug, &s_hp_ud_marker);
  ccord_input_event ev;

  push_audio_event(SDL_EVENT_AUDIO_DEVICE_ADDED, 1234, false);
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));   // consumed: not an input event
  CT_CHECK(s_hp_n == 1);
  CT_CHECK(!s_hp[0].capture && s_hp[0].added);

  push_audio_event(SDL_EVENT_AUDIO_DEVICE_REMOVED, 999, true);
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(s_hp_n == 2);
  CT_CHECK(s_hp[1].capture && !s_hp[1].added);

  // Clearing the callback stops notifications; the event is still consumed.
  ccord_audio_set_hotplug_callback(nullptr, nullptr);
  push_audio_event(SDL_EVENT_AUDIO_DEVICE_ADDED, 42, false);
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(s_hp_n == 2);
  ccord_platform_destroy(p);
}

// ---- removal -> reopen on the system default (white-box) ----

static void test_removal_fallback(void)
{
  ccord_platform *p = make_platform();
  if (!p) return;
  s_hp_n = 0;
  ccord_audio_set_hotplug_callback(on_hotplug, &s_hp_ud_marker);

  ccord_error err;
  // Keeper handle: SDL audio stays initialized so physical ids stay stable and
  // create/destroy of the test handles posts no subsystem-level device events.
  ccord_audio *keeper = ccord_audio_create(nullptr, &err);
  CT_CHECK(keeper != nullptr && ccord_audio_available(keeper));
  if (!keeper) {
    ccord_platform_destroy(p);
    return;
  }
  drain(p);   // absorb SDL's real device-ADDED events from subsystem init
  ccord_audio_device_info pb[8], rec[8];
  size_t npb = ccord_audio_devices_list(false, pb, 8);
  size_t nrec = ccord_audio_devices_list(true, rec, 8);
  CT_CHECK(npb >= 1 && nrec >= 1);
  if (npb == 0 || nrec == 0) {
    ccord_audio_destroy(keeper);
    ccord_platform_destroy(p);
    return;
  }
  ccord_audio_config pcfg = {.capture = false, .device_id = pb[0].id};
  ccord_audio *a = ccord_audio_create(&pcfg, &err);
  CT_CHECK(a != nullptr && ccord_audio_available(a));
  if (!a) {
    ccord_audio_destroy(keeper);
    ccord_platform_destroy(p);
    return;
  }
  drain(p);
  s_hp_n = 0;   // count only the synthetic events below
  SDL_AudioDeviceID dev0 = a->device;
  SDL_AudioDeviceID phys = a->opened_id;
  ccord_input_event ev;

  // Unrelated removal: nothing changes.
  push_audio_event(SDL_EVENT_AUDIO_DEVICE_REMOVED, 999999, false);
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(a->device == dev0 && a->opened_id == phys && ccord_audio_available(a));

  // Wrong direction: a recording removal must not touch the playback handle.
  push_audio_event(SDL_EVENT_AUDIO_DEVICE_REMOVED, phys, true);
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(a->device == dev0 && ccord_audio_available(a));

  // Physical-id removal: reopen on the system default with a fresh logical id.
  push_audio_event(SDL_EVENT_AUDIO_DEVICE_REMOVED, phys, false);
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(ccord_audio_available(a));
  CT_CHECK(a->opened_id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
  CT_CHECK(a->device != 0 && a->device != dev0);
  int16_t pcm[64] = {0};
  CT_CHECK(ccord_audio_write(a, pcm, 32) == 32);   // reopened stream still works
  SDL_AudioDeviceID dev1 = a->device;

  // SDL also emits the removal for the logical stream id: reopen again.
  push_audio_event(SDL_EVENT_AUDIO_DEVICE_REMOVED, dev1, false);
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(ccord_audio_available(a));
  CT_CHECK(a->opened_id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
  CT_CHECK(a->device != 0 && a->device != dev1);

  // ADDED never reopens an existing stream.
  SDL_AudioDeviceID dev2 = a->device;
  push_audio_event(SDL_EVENT_AUDIO_DEVICE_ADDED, dev2, false);
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(a->device == dev2 && ccord_audio_available(a));

  // Capture handle reopens on the recording default.
  ccord_audio_config rcfg = {.capture = true, .device_id = rec[0].id};
  ccord_audio *cap = ccord_audio_create(&rcfg, &err);
  CT_CHECK(cap != nullptr && ccord_audio_available(cap));
  if (cap) {
    SDL_AudioDeviceID cphys = cap->opened_id;
    SDL_AudioDeviceID cdev0 = cap->device;
    push_audio_event(SDL_EVENT_AUDIO_DEVICE_REMOVED, cphys, true);
    CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
    CT_CHECK(ccord_audio_available(cap));
    CT_CHECK(cap->opened_id == SDL_AUDIO_DEVICE_DEFAULT_RECORDING);
    CT_CHECK(cap->device != 0 && cap->device != cdev0);
    ccord_audio_destroy(cap);
    drain(p);
  }

  // Registry integrity after unregister: removing the playback device again
  // still reopens it (a dangling capture entry would crash under ASan).
  SDL_AudioDeviceID dev3 = a->device;
  push_audio_event(SDL_EVENT_AUDIO_DEVICE_REMOVED, dev3, false);
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(ccord_audio_available(a));
  CT_CHECK(a->device != 0 && a->device != dev3);

  ccord_audio_destroy(a);
  // Stale event after destroy: consumed, no crash, no reopen.
  push_audio_event(SDL_EVENT_AUDIO_DEVICE_REMOVED, dev3, false);
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(s_hp_n == 8);   // one callback per pushed event, including the stale one
  ccord_audio_set_hotplug_callback(nullptr, nullptr);
  ccord_audio_destroy(keeper);
  ccord_platform_destroy(p);
}

// ---- camera plane ----

static void test_camera(void)
{
  ccord_error err;
  CT_CHECK(SDL_WasInit(SDL_INIT_CAMERA) == 0);
  ccord_camera_info cams[8];
  size_t n = ccord_camera_list(cams, 8);
  CT_CHECK(SDL_WasInit(SDL_INIT_CAMERA) == 0);   // borrowed subsystem returned
  CT_CHECK(ccord_camera_list(nullptr, 8) == 0);
  CT_CHECK(ccord_camera_list(cams, 0) == 0);

  bool dummy_cam = false;
  if (SDL_InitSubSystem(SDL_INIT_CAMERA)) {
    const char *d = SDL_GetCurrentCameraDriver();
    dummy_cam = d && strcmp(d, "dummy") == 0;
    SDL_QuitSubSystem(SDL_INIT_CAMERA);
  }
  if (dummy_cam) CT_CHECK(n == 0);   // honest empty list, no fabricated device
  for (size_t i = 0; i < n; ++i) {
    uint32_t id = 0;
    CT_CHECK(ccord_devices_parse_id(cams[i].id, &id));
    CT_CHECK(cams[i].name[0] != '\0');
  }

  // Malformed ids are rejected before touching SDL.
  const char *bad[] = {nullptr, "", "0", "abc", "4294967296", "-3"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
    ccord_camera *c = ccord_camera_open(bad[i], &err);
    CT_CHECK(c == nullptr);
    CT_CHECK(err.code == CCORD_ERR_INVALID);
  }
  CT_CHECK(SDL_WasInit(SDL_INIT_CAMERA) == 0);

  ccord_camera *c1 = ccord_camera_open("1", &err);
  if (dummy_cam) {
    CT_CHECK(c1 == nullptr);
    CT_CHECK(err.code == CCORD_ERR_NOT_FOUND);
    CT_CHECK(SDL_WasInit(SDL_INIT_CAMERA) == 0);   // failure path returns the borrow
  } else if (c1) {
    ccord_camera_close(c1);
  }

  // Null-safety.
  uint8_t buf[64];
  int w = 0, h = 0;
  CT_CHECK(!ccord_camera_grab(nullptr, buf, sizeof buf, &w, &h));
  ccord_camera_close(nullptr);

  if (n > 0) {
    // Real hardware: open the first camera and try to grab a BGRA frame.
    ccord_camera *cam = ccord_camera_open(cams[0].id, &err);
    CT_CHECK(cam != nullptr);
    if (cam) {
      size_t cap = 1920u * 1080u * 4u;
      uint8_t *frame = malloc(cap);
      CT_CHECK(frame != nullptr);
      bool got = false;
      if (frame) {
        for (int i = 0; i < 50 && !got; ++i) {
          got = ccord_camera_grab(cam, frame, cap, &w, &h);
          if (!got) SDL_Delay(20);
        }
        if (got) {
          CT_CHECK(w > 0 && h > 0);
          CT_CHECK((size_t)w * (size_t)h * 4u <= cap);
        } else {
          printf("test_platform_devices: camera present but no frame ready; grab assertion skipped\n");
        }
        free(frame);
      }
      ccord_camera_close(cam);
      CT_CHECK(SDL_WasInit(SDL_INIT_CAMERA) == 0);
    }
  } else {
    printf("test_platform_devices: no camera hardware; lifecycle/grab checks skipped\n");
  }
}

int main(void)
{
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  SDL_SetHint(SDL_HINT_CAMERA_DRIVER, "dummy");
  // Immediate push-one/poll-one delivery (see test_platform_desktop.c).
  SDL_SetHint(SDL_HINT_POLL_SENTINEL, "0");
  test_enumeration();   // first: exercises borrowed SDL_INIT_AUDIO
  test_device_id();
  test_hotplug();
  test_removal_fallback();
  test_camera();
  return CT_RESULT("test_platform_devices");
}
