// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "audio/devices_internal.h"

#include <SDL3/SDL.h>
#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>
#include <ccord/platform/platform.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ccord_devices_parse_id(const char *s, uint32_t *out)
{
  if (!s || !out || *s == '\0') return false;
  uint64_t v = 0;
  for (const char *p = s; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10 + (uint64_t)(*p - '0');
    if (v > UINT32_MAX) return false;
  }
  if (v == 0) return false;
  *out = (uint32_t)v;
  return true;
}

SDL_AudioDeviceID ccord_audio_parse_device_id(bool capture, const char *id)
{
  SDL_AudioDeviceID def = capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  uint32_t v = 0;
  if (!ccord_devices_parse_id(id, &v)) return def;
  return (SDL_AudioDeviceID)v;
}

void ccord_devices_copy_utf8(char *dst, size_t cap, const char *src)
{
  if (cap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t len = strlen(src);
  size_t n = len < cap - 1 ? len : cap - 1;
  while (n > 0 && !ccord_utf8_valid(src, n)) n--;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

size_t ccord_audio_devices_list(bool capture, ccord_audio_device_info *out, size_t max)
{
  if (!out || max == 0) return 0;
  bool audio_init = false;
  if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return 0;
    audio_init = true;
  }
  int count = 0;
  SDL_AudioDeviceID *ids = capture ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);
  // SDL 3.4 has no public "default device id" query; compare names against the
  // default alias instead (dummy driver: one device per direction, the default).
  const char *def_name = SDL_GetAudioDeviceName(capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
                                                        : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
  size_t n = 0;
  if (ids && count > 0) {
    for (int i = 0; i < count && n < max; ++i) {
      snprintf(out[n].id, sizeof out[n].id, "%u", (unsigned)ids[i]);
      const char *name = SDL_GetAudioDeviceName(ids[i]);
      ccord_devices_copy_utf8(out[n].name, sizeof out[n].name, name ? name : "Audio device");
      out[n].is_default = def_name && name && strcmp(def_name, name) == 0;
      ++n;
    }
  }
  SDL_free(ids);
  if (audio_init) SDL_QuitSubSystem(SDL_INIT_AUDIO);
  return n;
}

// ---- hot-plug callback + registry of live handles (main thread only) ----

static ccord_audio_hotplug_fn s_hotplug_fn;
static void *s_hotplug_ud;
static ccord_audio *s_registry;
static SDL_InitState s_registry_init;
static SDL_Mutex *s_registry_mutex;

void ccord_audio_set_hotplug_callback(ccord_audio_hotplug_fn fn, void *ud)
{
  s_hotplug_fn = fn;
  s_hotplug_ud = ud;
}

static void registry_lock(void)
{
  if (SDL_ShouldInit(&s_registry_init)) {
    s_registry_mutex = SDL_CreateMutex();
    SDL_SetInitialized(&s_registry_init, s_registry_mutex != nullptr);
  }
  if (s_registry_mutex) SDL_LockMutex(s_registry_mutex);
}

static void registry_unlock(void)
{
  if (s_registry_mutex) SDL_UnlockMutex(s_registry_mutex);
}

void ccord_audio_devices_register(ccord_audio *a)
{
  if (!a) return;
  registry_lock();
  a->hotplug_next = s_registry;
  s_registry = a;
  registry_unlock();
}

void ccord_audio_devices_unregister(ccord_audio *a)
{
  if (!a) return;
  registry_lock();
  ccord_audio **link = &s_registry;
  while (*link && *link != a) link = &(*link)->hotplug_next;
  if (*link) *link = a->hotplug_next;
  a->hotplug_next = nullptr;
  registry_unlock();
}

// Reopen a handle whose device went away on the system default device. The old
// logical id dies with the stream; SDL assigns a fresh one on the new open.
// Caller holds a->stream_mtx.
static void reopen_on_default_locked(ccord_audio *a)
{
  SDL_DestroyAudioStream(a->stream);
  a->stream = nullptr;
  a->device = 0;
  SDL_AudioDeviceID def = a->capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  a->opened_id = def;
  a->stream = SDL_OpenAudioDeviceStream(def, &a->spec, nullptr, nullptr);
  if (!a->stream) {
    CCORD_LOG_D("audio: hot-plug reopen failed: %s", SDL_GetError());
    return;
  }
  a->device = SDL_GetAudioStreamDevice(a->stream);
  if (!SDL_ResumeAudioStreamDevice(a->stream)) {
    CCORD_LOG_D("audio: hot-plug resume failed: %s", SDL_GetError());
    SDL_DestroyAudioStream(a->stream);
    a->stream = nullptr;
    a->device = 0;
  }
}

bool ccord_audio_devices_handle_event(const SDL_Event *e)
{
  if (!e) return false;
  if (e->type != SDL_EVENT_AUDIO_DEVICE_ADDED && e->type != SDL_EVENT_AUDIO_DEVICE_REMOVED) return false;
  bool added = e->type == SDL_EVENT_AUDIO_DEVICE_ADDED;
  bool capture = e->adevice.recording;
  SDL_AudioDeviceID which = e->adevice.which;
  if (!added) {
    registry_lock();
    for (ccord_audio *a = s_registry; a; a = a->hotplug_next) {
      if (a->capture != capture) continue;
      if (a->stream_mtx) SDL_LockMutex(a->stream_mtx);
      // SDL reports removals for both the logical stream id and the physical id.
      if (a->stream && (a->device == which || a->opened_id == which)) {
        reopen_on_default_locked(a);
      }
      if (a->stream_mtx) SDL_UnlockMutex(a->stream_mtx);
    }
    registry_unlock();
  }
  if (s_hotplug_fn) s_hotplug_fn(s_hotplug_ud, capture, added);
  return true;
}

