// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "window/platform_internal.h"

#include <math.h>
#include <string.h>

#if defined(__ANDROID__)
#include "desktop/android_bridge.h"
#endif

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>

// ---- event translation ----

static uint8_t translate_mods(SDL_Keymod m)
{
  uint8_t r = 0;
  if (m & SDL_KMOD_SHIFT) r |= CCORD_MOD_SHIFT;
  if (m & SDL_KMOD_CTRL) r |= CCORD_MOD_CTRL;
  if (m & SDL_KMOD_ALT) r |= CCORD_MOD_ALT;
  if (m & SDL_KMOD_GUI) r |= CCORD_MOD_SUPER;
  return r;
}

static ccord_key translate_key(SDL_Keycode k)
{
  switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return CCORD_KEY_RETURN;
    case SDLK_ESCAPE: return CCORD_KEY_ESCAPE;
    case SDLK_BACKSPACE: return CCORD_KEY_BACKSPACE;
    case SDLK_DELETE: return CCORD_KEY_DELETE;
    case SDLK_TAB: return CCORD_KEY_TAB;
    case SDLK_LEFT: return CCORD_KEY_LEFT;
    case SDLK_RIGHT: return CCORD_KEY_RIGHT;
    case SDLK_UP: return CCORD_KEY_UP;
    case SDLK_DOWN: return CCORD_KEY_DOWN;
    case SDLK_HOME: return CCORD_KEY_HOME;
    case SDLK_END: return CCORD_KEY_END;
    case SDLK_PAGEUP: return CCORD_KEY_PAGEUP;
    case SDLK_PAGEDOWN: return CCORD_KEY_PAGEDOWN;
    case SDLK_A: return CCORD_KEY_A;
    case SDLK_C: return CCORD_KEY_C;
    case SDLK_V: return CCORD_KEY_V;
    case SDLK_X: return CCORD_KEY_X;
    case SDLK_Z: return CCORD_KEY_Z;
    case SDLK_Y: return CCORD_KEY_Y;
    case SDLK_B: return CCORD_KEY_B;
    case SDLK_I: return CCORD_KEY_I;
    case SDLK_U: return CCORD_KEY_U;
    case SDLK_S: return CCORD_KEY_S;
    case SDLK_E: return CCORD_KEY_E;
    case SDLK_F: return CCORD_KEY_F;
    case SDLK_K: return CCORD_KEY_K;
    case SDLK_P: return CCORD_KEY_P;
    case SDLK_G: return CCORD_KEY_G;
    case SDLK_M: return CCORD_KEY_M;
    case SDLK_D: return CCORD_KEY_D;
    case SDLK_T: return CCORD_KEY_T;
    case SDLK_L: return CCORD_KEY_L;
    case SDLK_Q: return CCORD_KEY_Q;
    case SDLK_SLASH: case SDLK_QUESTION: return CCORD_KEY_SLASH;
    case SDLK_AC_BACK: return CCORD_KEY_BACK;
    default: return CCORD_KEY_UNKNOWN;
  }
}

static uint8_t translate_button(uint8_t b)
{
  switch (b) {
    case SDL_BUTTON_LEFT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_RIGHT: return 3;
    case SDL_BUTTON_X1: return 4;
    case SDL_BUTTON_X2: return 5;
    default: return 0;
  }
}

// Screen coordinates -> logical px.
static float to_logical(const ccord_platform *p, float v)
{
  return v * platform_window_density(p->window) / platform_window_scale(p->window);
}

static bool translate(ccord_platform *p, const SDL_Event *e, ccord_input_event *out)
{
  memset(out, 0, sizeof *out);
  switch (e->type) {
    case SDL_EVENT_QUIT:
      out->type = CCORD_IN_QUIT;
      return true;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      // Close-to-tray: hide the window and keep running; the tray icon restores it.
      if (!ccord_platform_request_close(p)) {
        out->type = CCORD_IN_FOCUS;
        out->focused = false;
        return true;
      }
      out->type = CCORD_IN_QUIT;
      return true;
    case SDL_EVENT_WINDOW_MINIMIZED:
      p->minimized = true;
      p->focused = false;
      out->type = CCORD_IN_FOCUS;
      out->focused = false;
      return true;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
      p->minimized = false;
      return false;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
      p->fullscreen = true;
      return false;
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
      p->fullscreen = false;
      return false;
    case SDL_EVENT_WINDOW_SHOWN:
      p->window_visible = true;
      return false;
    case SDL_EVENT_WINDOW_HIDDEN:
      p->window_visible = false;
      p->focused = false;
      out->type = CCORD_IN_FOCUS;
      out->focused = false;
      return true;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_WINDOW_EXPOSED: {
      float w, h, s;
      ccord_platform_size(p, &w, &h, &s);
      out->type = CCORD_IN_RESIZE;
      out->x = w;
      out->y = h;
      return true;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      out->type = CCORD_IN_FOCUS;
      out->focused = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
      p->focused = out->focused;
      if (p->focused && p->window) (void)SDL_FlashWindow(p->window, SDL_FLASH_CANCEL);
#if defined(__ANDROID__)
      if (!p->focused && p->text_input_on && p->window) {
        SDL_StopTextInput(p->window);
        p->text_input_on = false;
      }
#endif
      return true;
    case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED: {
      platform_refresh_sdl_safe_area(p);
      out->type = CCORD_IN_SAFE_AREA;
      out->inset_top = p->safe_insets.top;
      out->inset_right = p->safe_insets.right;
      out->inset_bottom = p->safe_insets.bottom;
      out->inset_left = p->safe_insets.left;
      return true;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      out->type = CCORD_IN_MOUSE_LEAVE;
      return true;
    case SDL_EVENT_MOUSE_MOTION:
      if (e->motion.which == SDL_TOUCH_MOUSEID) return false;
      out->type = CCORD_IN_MOUSE_MOVE;
      out->x = to_logical(p, e->motion.x);
      out->y = to_logical(p, e->motion.y);
      return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (e->button.which == SDL_TOUCH_MOUSEID) return false;
      out->button = translate_button(e->button.button);
      if (out->button == 0) return false;
      out->type = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? CCORD_IN_MOUSE_DOWN : CCORD_IN_MOUSE_UP;
      out->clicks = e->button.clicks;
      out->mods = translate_mods(SDL_GetModState());
      out->x = to_logical(p, e->button.x);
      out->y = to_logical(p, e->button.y);
      return true;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
      float lw = 0.0f, lh = 0.0f, sc = 1.0f;
      ccord_platform_size(p, &lw, &lh, &sc);
      if (e->type == SDL_EVENT_FINGER_DOWN) out->type = CCORD_IN_TOUCH_DOWN;
      else if (e->type == SDL_EVENT_FINGER_MOTION) out->type = CCORD_IN_TOUCH_MOVE;
      else if (e->type == SDL_EVENT_FINGER_UP) out->type = CCORD_IN_TOUCH_UP;
      else out->type = CCORD_IN_TOUCH_CANCEL;
      out->finger_id = (int64_t)e->tfinger.fingerID;
      out->x = e->tfinger.x * lw;
      out->y = e->tfinger.y * lh;
      return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
      out->type = CCORD_IN_WHEEL;
      out->wheel = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
      out->x = to_logical(p, e->wheel.mouse_x);
      out->y = to_logical(p, e->wheel.mouse_y);
      out->mods = translate_mods(SDL_GetModState());
      return out->wheel != 0.0f;
    case SDL_EVENT_KEY_DOWN:
      out->type = CCORD_IN_KEY_DOWN;
      out->key = translate_key(e->key.key);
      out->mods = translate_mods(e->key.mod);
      return out->key != CCORD_KEY_UNKNOWN;
    case SDL_EVENT_TEXT_INPUT: {
      size_t n = e->text.text ? strlen(e->text.text) : 0;
      if (n == 0 || n > PLATFORM_PENDING_TEXT_MAX || !ccord_utf8_valid(e->text.text, n)) return false;
      if (n < sizeof out->text) {
        out->type = CCORD_IN_TEXT;
        memcpy(out->text, e->text.text, n);
        out->text[n] = '\0';
        return true;
      }
      // Long IME commit / mobile clipboard paste (>= 32 bytes): emit the first valid UTF-8 chunk now
      // and queue the remainder for subsequent ccord_platform_next_event() calls so nothing is dropped.
      size_t first = sizeof out->text - 1u;
      while (first > 0 && !ccord_utf8_valid(e->text.text, first)) first--;
      if (first == 0) return false;
      size_t rest = n - first;
      if (rest > 0) {
        char *buf = ccord_malloc(rest + 1u);
        if (buf) {
          memcpy(buf, e->text.text + first, rest);
          buf[rest] = '\0';
          platform_pending_text_clear(p);
          p->pending_text = buf;
          p->pending_text_len = rest;
          p->pending_text_off = 0;
        }
      }
      out->type = CCORD_IN_TEXT;
      memcpy(out->text, e->text.text, first);
      out->text[first] = '\0';
      return true;
    }
    case SDL_EVENT_TEXT_EDITING: {
      const char *text = e->edit.text;
      size_t n = text ? strlen(text) : 0;
      if (n >= sizeof out->text || (n > 0 && !ccord_utf8_valid(text, n))) return false;
      size_t cps = 0;
      for (size_t pos = 0; pos < n; pos = ccord_utf8_next(text, n, pos)) cps++;
      int64_t start = e->edit.start < 0 ? 0 : e->edit.start;   // SDL uses -1 for "unset"
      int64_t length = e->edit.length < 0 ? 0 : e->edit.length;
      if (start + length > (int64_t)cps) return false;   // cursor/selection must stay inside the composition
      out->type = CCORD_EV_IME_PREEDIT;
      if (n > 0) memcpy(out->text, text, n);
      out->ime_start = (int32_t)start;
      out->ime_length = (int32_t)length;
      return true;
    }
    case SDL_EVENT_DROP_FILE: {
      const char *path = e->drop.data;
      size_t n = path ? strlen(path) : 0;
      if (n == 0 || n >= DROP_PATH_MAX || !ccord_utf8_valid(path, n)) return false;
      if (p->drop_mutex) SDL_LockMutex(p->drop_mutex);
      platform_drop_buf *db = platform_drop_buf_ensure(p);
      if (!db) {
        if (p->drop_mutex) SDL_UnlockMutex(p->drop_mutex);
        return false;
      }
      memcpy(db->last, path, n + 1);
      memcpy(db->queue[p->drop_tail], path, n + 1);
      p->drop_tail = (p->drop_tail + 1) % DROP_QUEUE_CAP;
      if (p->drop_pop_count < DROP_QUEUE_CAP) {
        p->drop_pop_count++;
      } else {
        p->drop_pop_head = (p->drop_pop_head + 1) % DROP_QUEUE_CAP;
      }
      if (p->drop_mutex) SDL_UnlockMutex(p->drop_mutex);
      out->type = CCORD_EV_DROP_FILE;
      out->x = to_logical(p, e->drop.x);
      out->y = to_logical(p, e->drop.y);
      platform_copy_utf8_prefix(out->text, sizeof out->text, path, n);
      return true;
    }
    case SDL_EVENT_AUDIO_DEVICE_ADDED:
    case SDL_EVENT_AUDIO_DEVICE_REMOVED:
      // Hot-plug is handled by the device registry (reopen + callback); it is
      // not an input event for the UI event loop, so keep polling.
      ccord_audio_devices_handle_event(e);
      return false;
    default:
      return false;
  }
}

bool ccord_platform_next_event(ccord_platform *p, ccord_input_event *out, uint32_t timeout_ms)
{
  if (!p || !out) return false;
  // Dispatch pending DBus replies/signals and sample clocks/netlink on blocking waits, on first
  // poll, or at most every 8 ms during rapid timeout=0 drains (e.g. mouse-motion bursts).
  // Injected link events (tests / Android JNI) still pump immediately.
  uint64_t now_ms = SDL_GetTicks();
  bool need_bg_pump = timeout_ms > 0 || !p->bg_pumped_once ||
                      (now_ms >= p->last_bg_pump_ms && now_ms - p->last_bg_pump_ms >= 8u);
  if (need_bg_pump) {
    p->bg_pumped_once = true;
    p->last_bg_pump_ms = now_ms;
    ccord_desktop_pump(p->desktop);
    atomic_store(&p->link_dirty, false);
    platform_pump_link(p);
  } else if (atomic_exchange(&p->link_dirty, false)) {
    platform_pump_link(p);
  }

  if (atomic_exchange(&p->pending_bg_ev, false)) {
    memset(out, 0, sizeof *out);
    out->type = CCORD_IN_APP_BACKGROUND;
    return true;
  }
  if (atomic_exchange(&p->pending_fg_ev, false)) {
    memset(out, 0, sizeof *out);
    out->type = CCORD_IN_APP_FOREGROUND;
    return true;
  }
  if (atomic_exchange(&p->pending_lowmem_ev, false)) {
    memset(out, 0, sizeof *out);
    out->type = CCORD_IN_APP_LOW_MEMORY;
    return true;
  }
  if (atomic_exchange(&p->pending_reset_ev, false)) {
    memset(out, 0, sizeof *out);
    out->type = CCORD_IN_RENDER_RESET;
    return true;
  }
#if defined(__ANDROID__)
  ccord_android_drain_events(p);
  {
    float top = 0, right = 0, bottom = 0, left = 0, ime = 0;
    if (ccord_android_poll_insets(&top, &right, &bottom, &left, &ime)) {
      float s = p->window ? platform_window_scale(p->window) : 1.0f;
      if (s <= 0.0f) s = 1.0f;
      p->safe_insets = (ccord_safe_insets){top / s, right / s, bottom / s, left / s};
      p->keyboard_inset = ime / s;
      p->pending_safe_area_ev = true;
      p->pending_keyboard_ev = true;
    }
  }
#endif
  if (p->pending_safe_area_ev) {
    p->pending_safe_area_ev = false;
    memset(out, 0, sizeof *out);
    out->type = CCORD_IN_SAFE_AREA;
    out->inset_top = p->safe_insets.top;
    out->inset_right = p->safe_insets.right;
    out->inset_bottom = p->safe_insets.bottom;
    out->inset_left = p->safe_insets.left;
    return true;
  }
  if (p->pending_keyboard_ev) {
    p->pending_keyboard_ev = false;
    memset(out, 0, sizeof *out);
    out->type = CCORD_IN_KEYBOARD_INSET;
    out->inset_bottom = p->keyboard_inset;
    return true;
  }
  if (platform_pending_text_pop(p, out)) return true;
  bool queued_hotkey = false;
  if (p->hotkey_mutex) SDL_LockMutex(p->hotkey_mutex);
  if (p->hotkey_count > 0) {
    memset(out, 0, sizeof *out);
    const char *id = p->hotkey_queue[p->hotkey_head];
    size_t len = strlen(id);
    p->hotkey_head = (p->hotkey_head + 1) % HOTKEY_QUEUE_CAP;
    p->hotkey_count--;
    out->type = CCORD_EV_HOTKEY;
    platform_copy_utf8_prefix(out->text, sizeof out->text, id, len);
    queued_hotkey = true;
  }
  if (p->hotkey_mutex) SDL_UnlockMutex(p->hotkey_mutex);
  if (queued_hotkey) return true;

  bool queued_drop = false;
  if (p->drop_mutex) SDL_LockMutex(p->drop_mutex);
  if (p->drop_buf && p->drop_ev_count > 0) {
    memset(out, 0, sizeof *out);
    const char *src = p->drop_buf->queue[p->drop_ev_head];
    size_t len = strlen(src);
    memcpy(p->drop_buf->last, src, len + 1);
    p->drop_ev_head = (p->drop_ev_head + 1) % DROP_QUEUE_CAP;
    p->drop_ev_count--;
    if (p->drop_ev_count == 0 && p->drop_pop_count == 0) {
      p->drop_tail = 0;
      p->drop_ev_head = 0;
      p->drop_pop_head = 0;
    }
    out->type = CCORD_EV_DROP_FILE;
    platform_copy_utf8_prefix(out->text, sizeof out->text, p->drop_buf->last, len);
    queued_drop = true;
  }
  if (p->drop_mutex) SDL_UnlockMutex(p->drop_mutex);
  if (queued_drop) return true;

  uint64_t deadline = timeout_ms > 0 ? SDL_GetTicks() + timeout_ms : 0;
  uint32_t wait = timeout_ms;
  for (;;) {
    SDL_Event e;
    bool got = wait > 0 ? SDL_WaitEventTimeout(&e, (int32_t)wait) : SDL_PollEvent(&e);
    if (!got) return false;
    if (translate(p, &e, out)) return true;   // unknown or malformed events are dropped
    if (wait > 0) {
      uint64_t now = SDL_GetTicks();
      wait = now < deadline ? (uint32_t)(deadline - now) : 0;
    }
  }
}

