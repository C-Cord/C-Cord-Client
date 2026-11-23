// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "window/platform_internal.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/timerfd.h>
#include <time.h>
#endif

#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include "desktop/android_bridge.h"
#endif

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>
#include <ccord/platform/app_icon.h>

#if CCORD_HAVE_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif

static char s_last_opened_url[1024];
static uint8_t *s_test_clip_image;
static size_t s_test_clip_image_len;

static float safe_scale(float s)
{
  return (s > 0.0f && isfinite(s)) ? s : 1.0f;
}

float platform_window_scale(SDL_Window *w)
{
  return safe_scale(SDL_GetWindowDisplayScale(w));
}

float platform_window_density(SDL_Window *w)
{
  return safe_scale(SDL_GetWindowPixelDensity(w));
}



void platform_pending_text_clear(ccord_platform *p)
{
  if (!p) return;
  ccord_free(p->pending_text);
  p->pending_text = nullptr;
  p->pending_text_len = 0;
  p->pending_text_off = 0;
}

bool platform_pending_text_pop(ccord_platform *p, ccord_input_event *out)
{
  if (!p || !p->pending_text || p->pending_text_off >= p->pending_text_len) return false;
  const char *rem = p->pending_text + p->pending_text_off;
  size_t rem_len = p->pending_text_len - p->pending_text_off;
  size_t max_chunk = sizeof out->text - 1u;
  size_t chunk = rem_len < max_chunk ? rem_len : max_chunk;
  while (chunk > 0 && !ccord_utf8_valid(rem, chunk)) chunk--;
  if (chunk == 0) {
    platform_pending_text_clear(p);
    return false;
  }
  memset(out, 0, sizeof *out);
  out->type = CCORD_IN_TEXT;
  memcpy(out->text, rem, chunk);
  out->text[chunk] = '\0';
  p->pending_text_off += chunk;
  if (p->pending_text_off >= p->pending_text_len) platform_pending_text_clear(p);
  return true;
}

// Global hotkey queue: fed by the portal Activated signal (during desktop pump) or test injection,
// drained by ccord_platform_next_event as CCORD_EV_HOTKEY. Ring buffer, oldest dropped on overflow.
static void hotkey_push(ccord_platform *p, const char *id, size_t len)
{
  if (p->hotkey_mutex) SDL_LockMutex(p->hotkey_mutex);
  size_t tail = (p->hotkey_head + p->hotkey_count) % HOTKEY_QUEUE_CAP;
  memcpy(p->hotkey_queue[tail], id, len + 1);
  if (p->hotkey_count < HOTKEY_QUEUE_CAP) {
    p->hotkey_count++;
  } else {
    p->hotkey_head = (p->hotkey_head + 1) % HOTKEY_QUEUE_CAP;
  }
  if (p->hotkey_mutex) SDL_UnlockMutex(p->hotkey_mutex);
}

// Held state of the portal push-to-talk shortcut. Shortcut id null = portal session ended: release.
static void platform_shortcut_state_cb(void *ud, const char *shortcut_id, bool active)
{
  ccord_platform *p = ud;
  if (!p) return;
  if (!shortcut_id) {
    atomic_store(&p->global_ptt_held, false);
    return;
  }
  if (strcmp(shortcut_id, CCORD_SHORTCUT_PUSH_TO_TALK) == 0) atomic_store(&p->global_ptt_held, active);
}

static void platform_hotkey_cb(void *ud, const char *shortcut_id)
{
  ccord_platform *p = ud;
  if (!p || !shortcut_id || !shortcut_id[0]) return;
  size_t len = strlen(shortcut_id);
  if (len >= HOTKEY_ID_MAX || !ccord_utf8_valid(shortcut_id, len)) return;
  if (strcmp(shortcut_id, CCORD_SHORTCUT_PUSH_TO_TALK) == 0) return;   // held state only, never a toggle event
  hotkey_push(p, shortcut_id, len);
}

// Tray clicks / menu events arrive during ccord_desktop_pump (main thread). ACTIVATE toggles the
// window here; every action is also forwarded to the app callback registered on the platform.
static void platform_tray_cb(void *ud, ccord_tray_action action)
{
  ccord_platform *p = ud;
  if (!p) return;
  if (action == CCORD_TRAY_ACTIVATE) {
    if (!p->window_visible || p->minimized) {
      ccord_platform_show_window(p);
    } else if (p->focused) {
      ccord_platform_set_window_visible(p, false);
    } else {
      ccord_platform_show_window(p);
    }
  }
  if (p->tray_fn) p->tray_fn(p->tray_ud, action);
}

// A notification's default action (its click) arrives during ccord_desktop_pump; the app opens the message.
static void platform_notify_action_cb(void *ud, const ccord_notify_target *target)
{
  ccord_platform *p = ud;
  if (p && target && p->notify_click_fn) p->notify_click_fn(p->notify_click_ud, target);
}

static void apply_title_badge(ccord_platform *p)
{
#if defined(__ANDROID__)
  (void)p;
#else
  if (!p->window) return;
  char buf[BASE_TITLE_MAX + 32u];
  if (p->badge_tray > 0) {
    snprintf(buf, sizeof buf, "%s (%u)", p->base_title, p->badge_tray);
  } else {
    snprintf(buf, sizeof buf, "%s", p->base_title);
  }
  SDL_SetWindowTitle(p->window, buf);
#endif
}

#if !defined(__ANDROID__)
// Sets the window icon from the embedded logo: the largest image is the base surface, the others are attached as
// alternate images so the window system can pick the size it needs. Returns how many images were built; *accepted
// tells whether the driver took the icon (the dummy/headless video driver refuses it).
static uint32_t set_window_icon(SDL_Window *w, bool *accepted)
{
  static const int k_sizes[] = {256, 128, 64, 48, 32, 16};
  SDL_Surface *base = nullptr;
  uint32_t n = 0;
  for (size_t i = 0; i < sizeof k_sizes / sizeof k_sizes[0]; i++) {
    int sz = k_sizes[i];
    SDL_Surface *s = SDL_CreateSurface(sz, sz, SDL_PIXELFORMAT_RGBA32);
    if (!s) continue;
    if (!ccord_app_icon_rgba(sz, s->pixels, (size_t)s->pitch * (size_t)sz) || (base && !SDL_AddSurfaceAlternateImage(base, s))) {
      SDL_DestroySurface(s);
      continue;
    }
    if (!base) base = s;
    else SDL_DestroySurface(s);  // the base keeps its own copy
    n++;
  }
  if (!base) return 0;
  if (!SDL_SetWindowIcon(w, base)) {
    CCORD_LOG_D("platform: window icon: %s", SDL_GetError());
  } else {
    *accepted = true;
  }
  SDL_DestroySurface(base);
  return n;
}
#endif

bool ccord_platform_window_icon_accepted(const ccord_platform *p)
{
  return p && p->window_icon_accepted;
}

const char *ccord_platform_app_id(void)
{
  return SDL_GetHint(SDL_HINT_APP_ID);
}

uint32_t ccord_platform_window_icon_sizes(const ccord_platform *p)
{
  return p ? p->window_icon_sizes : 0;
}

void platform_wake(ccord_platform *p)
{
#if !defined(_WIN32)
  if (!p) return;
  if (SDL_GetCurrentThreadID() == p->main_thread_id &&
      atomic_load_explicit(&p->suppress_main_wake, memory_order_relaxed)) {
    return;
  }
  int fd = atomic_load_explicit(&p->wake_wr_fd, memory_order_acquire);
  if (fd < 0) return;
  uint8_t b = 1;
  ssize_t r;
  do {
    r = write(fd, &b, 1);
  } while (r < 0 && errno == EINTR);
#else
  (void)p;
#endif
}

static bool SDLCALL platform_lifecycle_watch(void *userdata, SDL_Event *event)
{
  ccord_platform *p = (ccord_platform *)userdata;
  if (!p || !event) return true;
  bool wake = false;
  switch (event->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
      atomic_store(&p->in_background, true);
      atomic_store(&p->pending_bg_ev, true);
      wake = true;
      break;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
      atomic_store(&p->in_background, false);
      atomic_store(&p->pending_fg_ev, true);
      wake = true;
      break;
    case SDL_EVENT_LOW_MEMORY:
      atomic_store(&p->pending_lowmem_ev, true);
      wake = true;
      break;
    case SDL_EVENT_RENDER_TARGETS_RESET:
    case SDL_EVENT_RENDER_DEVICE_RESET:
      atomic_store(&p->pending_reset_ev, true);
      wake = true;
      break;
    case SDL_EVENT_TERMINATING:
      atomic_store(&p->pending_quit_ev, true);
      wake = true;
      break;
    case SDL_EVENT_QUIT:
      if (atomic_load_explicit(&p->in_background, memory_order_relaxed)) {
        atomic_store(&p->pending_quit_ev, true);
        wake = true;
      }
      break;
    case SDL_EVENT_POLL_SENTINEL:
      break;
    default:
      if (atomic_load_explicit(&p->in_background, memory_order_relaxed)) wake = true;
      break;
  }
  if (wake) platform_wake(p);
  return true;
}

void platform_refresh_sdl_safe_area(ccord_platform *p)
{
  if (!p || !p->window || p->headless) return;
  SDL_Rect safe = {0};
  int win_w = 0, win_h = 0;
  if (!SDL_GetWindowSafeArea(p->window, &safe) || !SDL_GetWindowSize(p->window, &win_w, &win_h)) return;
  if (win_w <= 0 || win_h <= 0) return;
  float ratio = platform_window_density(p->window) / platform_window_scale(p->window);
  float top = (float)safe.y * ratio;
  float left = (float)safe.x * ratio;
  float right = (float)(win_w - (safe.x + safe.w)) * ratio;
  float bottom = (float)(win_h - (safe.y + safe.h)) * ratio;
  if (top < 0.0f) top = 0.0f;
  if (left < 0.0f) left = 0.0f;
  if (right < 0.0f) right = 0.0f;
  if (bottom < 0.0f) bottom = 0.0f;
  p->safe_insets = (ccord_safe_insets){.top = top, .right = right, .bottom = bottom, .left = left};
}

ccord_platform *ccord_platform_create(const ccord_platform_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  ccord_platform_config c = {.title = "C-Cord", .width = DEFAULT_WIDTH, .height = DEFAULT_HEIGHT};
  if (cfg) {
    c = *cfg;
    if (!c.title) c.title = "C-Cord";
    if (c.width <= 0) c.width = DEFAULT_WIDTH;
    if (c.height <= 0) c.height = DEFAULT_HEIGHT;
  }
  if (c.min_width < 0 || c.min_height < 0) {
    ccord_error_set(err, CCORD_ERR_INVALID, "platform: negative minimum window size");
    return nullptr;
  }
  // "dummy" needs no display and no GPU/EGL probing (the "offscreen" driver loads Mesa, which leaks under LeakSanitizer).
  if (c.headless) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  }
  // Prevent SDL3 from automatically synthesizing SDL_EVENT_QUIT when the sole window requests close:
  // C-Cord manages its own native tray (D-Bus SNI / Win32 / NSStatusBar) instead of SDL_CreateTray,
  // and translate() maps SDL_EVENT_WINDOW_CLOSE_REQUESTED to CCORD_IN_QUIT when close-to-tray is off.
  // Genuine quit requests (SIGINT/SIGTERM, macOS Cmd+Q / Dock Quit) still emit SDL_EVENT_QUIT.
  SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
#if defined(__ANDROID__)
  // Keep SDL_main running while backgrounded so the Discord Gateway stays connected and OS notifications fire.
  SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
  // Work around SDL3 3.4.20 Android busy-loop (libsdl-org/SDL#16713): poll sentinel events signal
  // Android_LifecycleEventSem inside SDL_WaitEventTimeout, preventing sleep and spinning 100% CPU.
  SDL_SetHint(SDL_HINT_POLL_SENTINEL, "0");
  // Use dedicated touch events in our gesture recognizer instead of double-firing synthetic mouse events.
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
  ccord_android_init_ffmpeg_jni();
#endif
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "platform: SDL_Init failed: %s", SDL_GetError());
    return nullptr;
  }
  ccord_platform *p = ccord_calloc(1, sizeof *p);
  if (!p) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    ccord_error_set(err, CCORD_ERR_NOMEM, "platform: out of memory");
    return nullptr;
  }
  p->headless = c.headless;
  p->focused = !c.start_minimized;
  p->window_visible = !c.start_minimized;
  p->wake_pipe[0] = -1;
  p->wake_pipe[1] = -1;
  p->wake_timer_fd = -1;
  p->main_thread_id = SDL_GetCurrentThreadID();
  atomic_init(&p->wake_wr_fd, -1);
  atomic_init(&p->suppress_main_wake, false);
  p->drop_mutex = SDL_CreateMutex();
  SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if !defined(__ANDROID__)
  if (c.start_minimized) window_flags |= SDL_WINDOW_HIDDEN;   // tray-only start: never flash the window
#endif
  // X11 WM_CLASS / Wayland app_id: must match StartupWMClass and C-Cord.desktop.
  SDL_SetHint(SDL_HINT_APP_ID, CCORD_PLATFORM_APP_ID);
  p->window = SDL_CreateWindow(c.title, c.width, c.height, window_flags);
  if (!p->window) {
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "platform: cannot create window: %s", SDL_GetError());
    if (p->drop_mutex) SDL_DestroyMutex(p->drop_mutex);
    ccord_free(p);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return nullptr;
  }
  if (!c.headless && !c.start_minimized) {
    const char *drv = SDL_GetCurrentVideoDriver();
    if (!drv || strcmp(drv, "dummy") != 0) {
      p->focused = (SDL_GetWindowFlags(p->window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    }
  }
#if !defined(_WIN32)
  if (pipe(p->wake_pipe) == 0) {
    for (int i = 0; i < 2; i++) {
      int fl = fcntl(p->wake_pipe[i], F_GETFL, 0);
      if (fl >= 0) (void)fcntl(p->wake_pipe[i], F_SETFL, fl | O_NONBLOCK);
      int fdfl = fcntl(p->wake_pipe[i], F_GETFD, 0);
      if (fdfl >= 0) (void)fcntl(p->wake_pipe[i], F_SETFD, fdfl | FD_CLOEXEC);
    }
    atomic_store_explicit(&p->wake_wr_fd, p->wake_pipe[1], memory_order_release);
  } else {
    p->wake_pipe[0] = -1;
    p->wake_pipe[1] = -1;
  }
#if defined(__linux__) && defined(CLOCK_BOOTTIME)
  p->wake_timer_fd = timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK | TFD_CLOEXEC);
#endif
#endif
#if defined(__ANDROID__)
  // Re-assert edge-to-edge window styling after SDL_CreateWindow's async COMMAND_CHANGE_WINDOW_STYLE.
  ccord_android_on_fullscreen_changed(false);
  int ff = ccord_android_get_form_factor();
  p->form_factor = (ff == 2) ? CCORD_FORM_TABLET : CCORD_FORM_PHONE;
  ccord_android_set_wake_fd(p->wake_pipe[1]);
  ccord_android_register_platform(p);
  {
    float top = 0.0f, right = 0.0f, bottom = 0.0f, left = 0.0f, ime = 0.0f;
    if (ccord_android_get_latest_insets(&top, &right, &bottom, &left, &ime)) {
      float s = platform_window_scale(p->window);
      if (s <= 0.0f) s = 1.0f;
      p->safe_insets = (ccord_safe_insets){top / s, right / s, bottom / s, left / s};
      p->keyboard_inset = ime / s;
      p->android_insets_seen = true;
    } else {
      platform_refresh_sdl_safe_area(p);
    }
  }
#else
  p->form_factor = CCORD_FORM_DESKTOP;
  p->window_icon_sizes = set_window_icon(p->window, &p->window_icon_accepted);
  // The window was created in screen coordinates; the requested size is in logical px. Fix it up once the
  // display scale is known (X11 with a content scale, Wayland fractional scaling).
  float scale = platform_window_scale(p->window);
  float density = platform_window_density(p->window);
  int want_w = (int)lroundf((float)c.width * scale / density);
  int want_h = (int)lroundf((float)c.height * scale / density);
  if (want_w != c.width || want_h != c.height) SDL_SetWindowSize(p->window, want_w, want_h);
  if (c.min_width > 0 || c.min_height > 0) {
    SDL_SetWindowMinimumSize(p->window, (int)lroundf((float)c.min_width * scale / density),
                             (int)lroundf((float)c.min_height * scale / density));
  }
#endif
  SDL_AddEventWatch(platform_lifecycle_watch, p);
  platform_copy_utf8_prefix(p->base_title, sizeof p->base_title, c.title, strlen(c.title));
  p->hotkey_mutex = SDL_CreateMutex();
  p->desktop = ccord_desktop_create(CCORD_PLATFORM_APP_ID, c.headless);
  ccord_desktop_set_hotkey_callback(p->desktop, platform_hotkey_cb, p);
  ccord_desktop_set_shortcut_state_callback(p->desktop, platform_shortcut_state_cb, p);
  ccord_desktop_set_tray_callback(p->desktop, platform_tray_cb, p);
  ccord_desktop_set_notify_action_callback(p->desktop, platform_notify_action_cb, p);
  p->link = ccord_linkwatch_create(c.headless);   // nullptr (out of memory) just disables link events
  return p;
}

void ccord_platform_destroy(ccord_platform *p)
{
  if (!p) return;
  SDL_RemoveEventWatch(platform_lifecycle_watch, p);
#if defined(__ANDROID__)
  if (p->voice_service_active) {
    ccord_android_set_voice_service(false, "", false, false);
    ccord_android_set_audio_comm_mode(false);
  }
  if (p->keep_alive_active) {
    ccord_android_set_keep_alive(false);
  }
  ccord_android_set_wake_fd(-1);
  ccord_android_register_platform(nullptr);
#endif
#if !defined(_WIN32)
  atomic_store_explicit(&p->wake_wr_fd, -1, memory_order_release);
  if (p->wake_pipe[0] >= 0) close(p->wake_pipe[0]);
  if (p->wake_pipe[1] >= 0) close(p->wake_pipe[1]);
  if (p->wake_timer_fd >= 0) close(p->wake_timer_fd);
  p->wake_pipe[0] = -1;
  p->wake_pipe[1] = -1;
  p->wake_timer_fd = -1;
#endif
  if (p->text_input_on) SDL_StopTextInput(p->window);
  platform_pending_text_clear(p);
  ccord_desktop_destroy(p->desktop);
  ccord_linkwatch_destroy(p->link);
  SDL_SetCursor(nullptr);
  for (size_t i = 0; i < (size_t)CCORD_CURSOR_COUNT; i++) {
    if (p->cursors[i]) SDL_DestroyCursor(p->cursors[i]);
  }
  ccord_free(p->clip_image);
  ccord_free(p->drop_buf);
  ccord_free(s_test_clip_image);
  s_test_clip_image = nullptr;
  s_test_clip_image_len = 0;
  if (p->drop_mutex) SDL_DestroyMutex(p->drop_mutex);
  if (p->hotkey_mutex) SDL_DestroyMutex(p->hotkey_mutex);
  SDL_DestroyWindow(p->window);
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  ccord_free(p);
}

void ccord_platform_size(const ccord_platform *p, float *logical_w, float *logical_h, float *scale)
{
  int pw = 0, ph = 0;
  float s = 1.0f;
  if (p && p->window) {
    SDL_GetWindowSizeInPixels(p->window, &pw, &ph);
    s = platform_window_scale(p->window);
  }
  if (logical_w) *logical_w = (float)pw / s;
  if (logical_h) *logical_h = (float)ph / s;
  if (scale) *scale = s;
}

void ccord_platform_set_title(ccord_platform *p, const char *utf8)
{
  if (!p || !utf8 || !ccord_utf8_valid(utf8, strlen(utf8))) return;
  platform_copy_utf8_prefix(p->base_title, sizeof p->base_title, utf8, strlen(utf8));
  apply_title_badge(p);
}

void ccord_platform_set_cursor(ccord_platform *p, ccord_cursor_kind kind)
{
  if (!p || p->cursor_kind == kind) return;
  static const SDL_SystemCursor map[CCORD_CURSOR_COUNT] = {
    [CCORD_CURSOR_DEFAULT] = SDL_SYSTEM_CURSOR_DEFAULT,
    [CCORD_CURSOR_POINTER] = SDL_SYSTEM_CURSOR_POINTER,
    [CCORD_CURSOR_TEXT] = SDL_SYSTEM_CURSOR_TEXT,
    [CCORD_CURSOR_NS_RESIZE] = SDL_SYSTEM_CURSOR_NS_RESIZE,
    [CCORD_CURSOR_EW_RESIZE] = SDL_SYSTEM_CURSOR_EW_RESIZE,
    [CCORD_CURSOR_NWSE_RESIZE] = SDL_SYSTEM_CURSOR_NWSE_RESIZE,
    [CCORD_CURSOR_NESW_RESIZE] = SDL_SYSTEM_CURSOR_NESW_RESIZE,
    [CCORD_CURSOR_MOVE] = SDL_SYSTEM_CURSOR_MOVE,
  };
  if ((size_t)kind >= (size_t)CCORD_CURSOR_COUNT || p->cursor_unavailable[kind]) return;
  if (!p->cursors[kind]) {
    p->cursors[kind] = SDL_CreateSystemCursor(map[kind]);
    if (!p->cursors[kind]) {
      CCORD_LOG_D("platform: cursor %d unavailable: %s", (int)kind, SDL_GetError());
      p->cursor_unavailable[kind] = true;
      return;
    }
  }
  SDL_SetCursor(p->cursors[kind]);
  p->cursor_kind = kind;
}

void ccord_platform_text_input_ex(ccord_platform *p, bool active, ccord_text_input_kind kind,
                                  float x, float y, float w, float h)
{
  if (!p) return;
  if (active) {
    float f = platform_window_scale(p->window) / platform_window_density(p->window);   // logical -> screen coordinates
    SDL_Rect r = {(int)(x * f), (int)(y * f), (int)(w * f), (int)(h * f)};
    SDL_SetTextInputArea(p->window, &r, 0);
    if (!p->text_input_on || p->text_input_kind != kind) {
      SDL_PropertiesID props = SDL_CreateProperties();
      if (props) {
        SDL_TextInputType stype = SDL_TEXTINPUT_TYPE_TEXT;
        SDL_Capitalization scap = SDL_CAPITALIZE_SENTENCES;
        bool autocorrect = true;
        bool multiline = true;
        switch (kind) {
          case CCORD_INPUT_EMAIL:
            stype = SDL_TEXTINPUT_TYPE_TEXT_EMAIL;
            scap = SDL_CAPITALIZE_NONE;
            autocorrect = false;
            multiline = false;
            break;
          case CCORD_INPUT_PASSWORD:
            stype = SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN;
            scap = SDL_CAPITALIZE_NONE;
            autocorrect = false;
            multiline = false;
            break;
          case CCORD_INPUT_OTP:
            stype = SDL_TEXTINPUT_TYPE_NUMBER;
            scap = SDL_CAPITALIZE_NONE;
            autocorrect = false;
            multiline = false;
            break;
          case CCORD_INPUT_SINGLE_LINE:
            stype = SDL_TEXTINPUT_TYPE_TEXT;
            scap = SDL_CAPITALIZE_NONE;
            autocorrect = true;
            multiline = false;
            break;
          case CCORD_INPUT_TEXT:
          default:
            break;
        }
        SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, (Sint64)stype);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER, (Sint64)scap);
        SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN, autocorrect);
        SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, multiline);
        if (SDL_StartTextInputWithProperties(p->window, props)) {
          p->text_input_on = true;
          p->text_input_kind = kind;
        }
        SDL_DestroyProperties(props);
      } else if (!p->text_input_on && SDL_StartTextInput(p->window)) {
        p->text_input_on = true;
        p->text_input_kind = kind;
      }
    }
  } else if (p->text_input_on) {
    SDL_StopTextInput(p->window);
    p->text_input_on = false;
  }
}

void ccord_platform_text_input(ccord_platform *p, bool active, float x, float y, float w, float h)
{
  ccord_platform_text_input_ex(p, active, CCORD_INPUT_TEXT, x, y, w, h);
}

ccord_status ccord_platform_clipboard_set(ccord_platform *p, const char *utf8, ccord_error *err)
{
  if (!p || !utf8) return ccord_error_set(err, CCORD_ERR_INVALID, "clipboard: null argument");
  if (!ccord_utf8_valid(utf8, strlen(utf8))) return ccord_error_set(err, CCORD_ERR_INVALID, "clipboard: text is not valid UTF-8");
  if (!SDL_SetClipboardText(utf8)) return ccord_error_set(err, CCORD_ERR_IO, "clipboard: %s", SDL_GetError());
  return CCORD_OK;
}

char *ccord_platform_clipboard_get(ccord_platform *p)
{
  if (!p) return nullptr;
  char *sdl = SDL_GetClipboardText();
  if (!sdl) return nullptr;
  char *out = nullptr;
  size_t len = strlen(sdl);
  if (len > 0 && ccord_utf8_valid(sdl, len)) out = ccord_strndup(sdl, len);
  SDL_free(sdl);
  return out;
}

void *ccord_platform_sdl_window(ccord_platform *p)
{
  return p ? (void *)p->window : nullptr;
}

// ---- browser URL launch, image clipboard, file drop/picker, notifications ----

static bool valid_http_url(const char *url)
{
  if (!url) return false;
  size_t len = strlen(url);
  if (len < 10 || len >= sizeof s_last_opened_url || !ccord_utf8_valid(url, len)) return false;
  bool https = strncmp(url, "https://", 8) == 0 && url[8] != '\0';
  bool http = strncmp(url, "http://", 7) == 0 && url[7] != '\0';
  if (!https && !http) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)url[i];
    if (ch <= 0x20u || ch == 0x7fu) return false;
  }
  return true;
}

bool ccord_platform_open_url(const char *url)
{
  if (!valid_http_url(url)) return false;
  size_t len = strlen(url);
  memcpy(s_last_opened_url, url, len + 1);
  const char *drv = SDL_GetCurrentVideoDriver();
  if (!drv || strcmp(drv, "dummy") == 0 || strcmp(drv, "offscreen") == 0) {
    return true;
  }
  return SDL_OpenURL(url);
}

const char *ccord_platform_last_opened_url(void)
{
  return s_last_opened_url[0] ? s_last_opened_url : nullptr;
}

ccord_status ccord_platform_set_clipboard_image(ccord_platform *p, const uint8_t *data, size_t len, ccord_error *err)
{
  ccord_error_clear(err);
  uint8_t *copy = nullptr;
  if (data && len > 0) {
    if (len > 32u * 1024u * 1024u) return ccord_error_set(err, CCORD_ERR_RANGE, "clipboard image: exceeds 32 MiB cap");
    copy = ccord_malloc(len);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "clipboard image: out of memory");
    memcpy(copy, data, len);
  }
  if (p) {
    ccord_free(p->clip_image);
    p->clip_image = copy;
    p->clip_image_len = copy ? len : 0;
  } else {
    ccord_free(s_test_clip_image);
    s_test_clip_image = copy;
    s_test_clip_image_len = copy ? len : 0;
  }
  return CCORD_OK;
}

uint8_t *ccord_platform_get_clipboard_image(ccord_platform *p, size_t *out_len)
{
  if (out_len) *out_len = 0;
  const uint8_t *injected = (p && p->clip_image && p->clip_image_len > 0) ? p->clip_image : s_test_clip_image;
  size_t injected_len = (p && p->clip_image && p->clip_image_len > 0) ? p->clip_image_len : s_test_clip_image_len;
  if (injected && injected_len > 0) {
    uint8_t *copy = ccord_malloc(injected_len);
    if (!copy) return nullptr;
    memcpy(copy, injected, injected_len);
    if (out_len) *out_len = injected_len;
    return copy;
  }
  if (p && p->headless) return nullptr;
  static const char *const k_mimes[] = {"image/png", "image/webp", "image/jpeg"};
  for (size_t i = 0; i < sizeof k_mimes / sizeof k_mimes[0]; i++) {
    size_t sz = 0;
    void *raw = SDL_GetClipboardData(k_mimes[i], &sz);
    if (raw) {
      uint8_t *copy = nullptr;
      if (sz > 0 && sz <= 32u * 1024u * 1024u) {
        copy = ccord_malloc(sz);
        if (copy) {
          memcpy(copy, raw, sz);
          if (out_len) *out_len = sz;
        }
      }
      SDL_free(raw);
      if (copy) return copy;
    }
  }
  return nullptr;
}

void platform_copy_utf8_prefix(char *dst, size_t cap, const char *src, size_t len)
{
  if (cap == 0) return;
  size_t n = len < cap - 1 ? len : cap - 1;
  while (n > 0 && !ccord_utf8_valid(src, n)) n--;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

platform_drop_buf *platform_drop_buf_ensure(ccord_platform *p)
{
  if (!p->drop_buf) p->drop_buf = ccord_calloc(1, sizeof *p->drop_buf);
  return p->drop_buf;
}

ccord_status ccord_platform_inject_drop_file(ccord_platform *p, const char *path, ccord_error *err)
{
  ccord_error_clear(err);
  if (!p || !path) return ccord_error_set(err, CCORD_ERR_INVALID, "drop file: null argument");
  size_t len = strlen(path);
  if (len == 0 || len >= DROP_PATH_MAX || !ccord_utf8_valid(path, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "drop file: invalid path");
  }
  if (p->drop_mutex) SDL_LockMutex(p->drop_mutex);
  platform_drop_buf *db = platform_drop_buf_ensure(p);
  if (!db) {
    if (p->drop_mutex) SDL_UnlockMutex(p->drop_mutex);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "drop file: out of memory");
  }
  memcpy(db->last, path, len + 1);
  memcpy(db->queue[p->drop_tail], path, len + 1);
  p->drop_tail = (p->drop_tail + 1) % DROP_QUEUE_CAP;

  if (p->drop_ev_count < DROP_QUEUE_CAP) {
    p->drop_ev_count++;
  } else {
    p->drop_ev_head = (p->drop_ev_head + 1) % DROP_QUEUE_CAP;
  }

  if (p->drop_pop_count < DROP_QUEUE_CAP) {
    p->drop_pop_count++;
  } else {
    p->drop_pop_head = (p->drop_pop_head + 1) % DROP_QUEUE_CAP;
  }
  if (p->drop_mutex) SDL_UnlockMutex(p->drop_mutex);
  platform_wake(p);
  return CCORD_OK;
}

#if !defined(__ANDROID__)
static void SDLCALL on_file_dialog(void *userdata, const char *const *filelist, int filter)
{
  (void)filter;
  ccord_platform *p = (ccord_platform *)userdata;
  if (!p || !filelist) return;
  for (size_t i = 0; filelist[i] != nullptr; i++) {
    ccord_error err;
    (void)ccord_platform_inject_drop_file(p, filelist[i], &err);
  }
}
#endif

void ccord_platform_open_file_dialog(ccord_platform *p)
{
  if (!p || p->headless) return;
#if defined(__ANDROID__)
  (void)ccord_android_open_file_picker();
#else
  static const SDL_DialogFileFilter k_filters[] = {
    {"Images and files", "png;jpg;jpeg;webp;gif;txt;md;json;pdf;*"},
    {"All files", "*"},
  };
  SDL_ShowOpenFileDialog(on_file_dialog, p, p->window, k_filters, 2, nullptr, true);
#endif
}

const char *ccord_platform_last_drop_file(const ccord_platform *p)
{
  if (!p) return nullptr;
  if (p->drop_mutex) SDL_LockMutex(p->drop_mutex);
  const char *res = (p->drop_buf && p->drop_buf->last[0]) ? p->drop_buf->last : nullptr;
  if (p->drop_mutex) SDL_UnlockMutex(p->drop_mutex);
  return res;
}

bool ccord_platform_pop_drop_file(ccord_platform *p, char *out, size_t cap)
{
  if (!p || !out || cap == 0) return false;
  out[0] = '\0';
  bool ok = false;
  if (p->drop_mutex) SDL_LockMutex(p->drop_mutex);
  if (p->drop_buf && p->drop_pop_count > 0) {
    const char *src = p->drop_buf->queue[p->drop_pop_head];
    size_t len = strlen(src);
    if (len < cap) {
      memcpy(out, src, len + 1);
      memcpy(p->drop_buf->last, src, len + 1);
      ok = true;
    }
    p->drop_pop_head = (p->drop_pop_head + 1) % DROP_QUEUE_CAP;
    p->drop_pop_count--;
    if (p->drop_pop_count == 0 && p->drop_ev_count == 0) {
      p->drop_tail = 0;
      p->drop_ev_head = 0;
      p->drop_pop_head = 0;
    }
  }
  if (p->drop_mutex) SDL_UnlockMutex(p->drop_mutex);
  return ok;
}

bool ccord_platform_window_focused(const ccord_platform *p)
{
  return p && p->focused && !atomic_load(&p->in_background);
}

// Every window-attention call goes through here. C-Cord never asks the shell to highlight its taskbar entry:
// the unread count is shown on the tray icon instead. Only the cancel (clearing a flash another program or an
// older build left behind) is issued; any other operation is counted so ccord_platform_flash_requests exposes it.
static void platform_flash(ccord_platform *p, SDL_FlashOperation op)
{
  if (!p->window) return;
  if (op != SDL_FLASH_CANCEL) p->flash_requests++;
  (void)SDL_FlashWindow(p->window, op);
}

uint32_t ccord_platform_flash_requests(const ccord_platform *p)
{
  return p ? p->flash_requests : 0;
}

void ccord_platform_set_window_focused(ccord_platform *p, bool focused)
{
  if (!p) return;
  p->focused = focused;
  p->pending_focus_ev = false;
  if (focused) platform_flash(p, SDL_FLASH_CANCEL);
}

#if CCORD_HAVE_X11
// Extracts the X11 server timestamp from a startup-notification / ActivationToken string ("..._TIME<u32>").
static unsigned long x11_parse_token_time(const char *token)
{
  if (!token || !token[0]) return 0ul;
  const char *t = strstr(token, "_TIME");
  if (!t) return 0ul;
  t += 5;
  if (*t < '0' || *t > '9') return 0ul;
  char *end = nullptr;
  unsigned long long val = strtoull(t, &end, 10);
  if (end == t || val == 0ull || val > 0xFFFFFFFFull) return 0ul;
  return (unsigned long)val;
}
#endif

static void platform_reveal_window(ccord_platform *p, bool restore_if_min)
{
  if (!p || !p->window) return;
  char token[256];
  bool has_token = ccord_desktop_take_activation_token(p->desktop, token, sizeof token);
  const char *drv = !p->headless ? SDL_GetCurrentVideoDriver() : nullptr;

  SDL_Environment *env = nullptr;
  bool set_wayland_token = false;
  if (has_token && drv && strcmp(drv, "wayland") == 0) {
    env = SDL_GetEnvironment();
    if (env) set_wayland_token = SDL_SetEnvironmentVariable(env, "XDG_ACTIVATION_TOKEN", token, true);
  }

#if CCORD_HAVE_X11
  Display *x_dpy = nullptr;
  int x_screen = 0;
  Window x_win = 0;
  unsigned long x_ts = 0ul;
  if (drv && strcmp(drv, "x11") == 0) {
    SDL_PropertiesID props = SDL_GetWindowProperties(p->window);
    x_dpy = (Display *)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    x_screen = (int)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_SCREEN_NUMBER, 0);
    x_win = (Window)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (has_token) x_ts = x11_parse_token_time(token);
    if (x_dpy && x_win && x_ts != 0ul) {
      Atom user_time = XInternAtom(x_dpy, "_NET_WM_USER_TIME", False);
      if (user_time != None) {
        (void)XChangeProperty(x_dpy, x_win, user_time, XA_CARDINAL, 32, PropModeReplace,
                              (const unsigned char *)&x_ts, 1);
      }
    }
  }
#endif

  SDL_ShowWindow(p->window);
  if (restore_if_min) {
    SDL_RestoreWindow(p->window);
  }

#if defined(_WIN32)
  char prev_force[16] = {0};
  bool had_force = false;
  if (!p->headless) {
    const char *pf = SDL_GetHint(SDL_HINT_FORCE_RAISEWINDOW);
    if (pf) {
      had_force = true;
      snprintf(prev_force, sizeof prev_force, "%s", pf);
    }
    (void)SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
  }
  SDL_RaiseWindow(p->window);
  if (!p->headless) {
    if (had_force) (void)SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, prev_force);
    else (void)SDL_ResetHint(SDL_HINT_FORCE_RAISEWINDOW);
  }
#else
  SDL_RaiseWindow(p->window);
#endif

#if CCORD_HAVE_X11
  if (x_dpy && x_win) {
    // SDL_RaiseWindow sends _NET_ACTIVE_WINDOW with source=1 (normal app), which KWin/Mutter focus-stealing
    // prevention rejects when triggered from a desktop notification or tray click. Use source=2 (user-initiated
    // desktop activation) and XMapRaised so hidden/minimized/background windows reliably reveal and focus.
    (void)XMapRaised(x_dpy, x_win);
    Atom net_active = XInternAtom(x_dpy, "_NET_ACTIVE_WINDOW", False);
    if (net_active != None) {
      XEvent xev;
      memset(&xev, 0, sizeof xev);
      xev.xclient.type = ClientMessage;
      xev.xclient.window = x_win;
      xev.xclient.message_type = net_active;
      xev.xclient.format = 32;
      xev.xclient.data.l[0] = 2;
      xev.xclient.data.l[1] = (long)x_ts;
      xev.xclient.data.l[2] = 0;
      (void)XSendEvent(x_dpy, RootWindow(x_dpy, x_screen), False,
                       SubstructureRedirectMask | SubstructureNotifyMask, &xev);
    }
    (void)XFlush(x_dpy);
  }
#endif

  if (set_wayland_token && env) {
    const char *cur = SDL_GetEnvironmentVariable(env, "XDG_ACTIVATION_TOKEN");
    if (cur && strcmp(cur, token) == 0) {
      (void)SDL_UnsetEnvironmentVariable(env, "XDG_ACTIVATION_TOKEN");
    }
  }
}

void ccord_platform_set_window_visible(ccord_platform *p, bool visible)
{
  if (!p || p->window_visible == visible) return;
  p->window_visible = visible;
  if (!visible) {
    p->focused = false;
    p->pending_focus_ev = false;
  }
  if (!p->window) return;
  if (visible) {
    bool is_min = p->minimized || (SDL_GetWindowFlags(p->window) & SDL_WINDOW_MINIMIZED) != 0;
    p->minimized = false;
    platform_reveal_window(p, is_min);
  } else {
    SDL_HideWindow(p->window);
  }
}

void ccord_platform_show_window(ccord_platform *p)
{
  if (!p) return;
  bool was_min = p->minimized;
  p->minimized = false;
  p->window_visible = true;
  if (!p->window) return;
  bool restore = was_min || (SDL_GetWindowFlags(p->window) & SDL_WINDOW_MINIMIZED) != 0;
  platform_reveal_window(p, restore);
}

bool ccord_platform_window_visible(const ccord_platform *p)
{
  return p ? p->window_visible : false;
}

bool ccord_platform_set_fullscreen(ccord_platform *p, bool fullscreen)
{
  if (!p) return false;
  if (p->window && !p->headless) {
    if (!SDL_SetWindowFullscreen(p->window, fullscreen)) {
      CCORD_LOG_W("platform: fullscreen(%d) failed: %s", fullscreen ? 1 : 0, SDL_GetError());
      return false;
    }
#if defined(__ANDROID__)
    ccord_android_on_fullscreen_changed(fullscreen);
#endif
  }
  p->fullscreen = fullscreen;
  return true;
}

bool ccord_platform_window_fullscreen(const ccord_platform *p)
{
  return p ? p->fullscreen : false;
}

void ccord_platform_set_close_to_tray(ccord_platform *p, bool enabled)
{
  if (!p) return;
  p->close_to_tray = enabled;
}

bool ccord_platform_request_close(ccord_platform *p)
{
  if (!p || !p->close_to_tray) return true;
  p->focused = false;
  p->pending_focus_ev = false;
  if (ccord_platform_tray_available(p)) {
    ccord_platform_set_window_visible(p, false);
  } else {
    // No StatusNotifierWatcher: hiding would leave the window unreachable, so minimize it instead.
    p->minimized = true;
    if (p->window) (void)SDL_MinimizeWindow(p->window);
  }
  return false;
}

bool ccord_platform_window_minimized(const ccord_platform *p)
{
  return p && p->minimized;
}

void ccord_platform_set_tray_override(ccord_platform *p, int mode)
{
  if (p) p->tray_override = mode;
}

void ccord_platform_inject_tray_action(ccord_platform *p, ccord_tray_action action)
{
  platform_tray_cb(p, action);
}

bool ccord_platform_close_to_tray(const ccord_platform *p)
{
  return p ? p->close_to_tray : false;
}

void ccord_platform_set_tray_callback(ccord_platform *p, ccord_tray_action_fn fn, void *ud)
{
  if (!p) return;
  p->tray_fn = fn;
  p->tray_ud = ud;
}

void ccord_platform_set_link_callback(ccord_platform *p, ccord_link_event_fn fn, void *ud)
{
  if (!p) return;
  p->link_fn = fn;
  p->link_ud = ud;
}

void ccord_platform_inject_link_event(ccord_platform *p, ccord_link_event ev)
{
  if (!p) return;
  ccord_linkwatch_inject(p->link, ev);
  atomic_store(&p->link_dirty, true);
  platform_wake(p);
}

// Samples the clocks and the route socket, then hands every pending event to the callback (main thread, no waiting).
void platform_pump_link(ccord_platform *p)
{
  if (!p->link) return;
  ccord_linkwatch_sample(p->link);
  for (ccord_link_event ev = ccord_linkwatch_take(p->link); ev != CCORD_LINK_NONE; ev = ccord_linkwatch_take(p->link)) {
    if (p->link_fn) p->link_fn(p->link_ud, ev);
  }
}

void ccord_platform_set_voice_state(ccord_platform *p, bool muted, bool deafened)
{
  if (!p) return;
  ccord_desktop_set_voice_state(p->desktop, muted, deafened);
}

bool ccord_platform_notify(ccord_platform *p, const char *title, const char *body)
{
  return ccord_platform_notify_message(p, title, body, nullptr);
}

bool ccord_platform_notify_message(ccord_platform *p, const char *title, const char *body,
                                   const ccord_notify_target *target)
{
  if (!p || ccord_platform_window_focused(p)) return false;
  if (title && !ccord_utf8_valid(title, strlen(title))) return false;
  if (body && !ccord_utf8_valid(body, strlen(body))) return false;
  p->notify_count++;
  // DBus Notify when a notification server is reachable; the sink records it either way. No window flash:
  // that would highlight the taskbar entry.
  ccord_desktop_notify_target(p->desktop, title, body, target);
  return true;
}

void ccord_platform_set_notify_click_callback(ccord_platform *p, ccord_notify_click_fn fn, void *ud)
{
  if (!p) return;
  p->notify_click_fn = fn;
  p->notify_click_ud = ud;
}

uint32_t ccord_platform_last_notify_id(const ccord_platform *p)
{
  return ccord_desktop_last_notify_id(p ? p->desktop : nullptr);
}

const char *ccord_platform_last_notify_actions(const ccord_platform *p)
{
  return ccord_desktop_last_notify_actions(p ? p->desktop : nullptr);
}

bool ccord_platform_notify_target_for(const ccord_platform *p, uint32_t notification_id, ccord_notify_target *out)
{
  return ccord_desktop_notify_target_for(p ? p->desktop : nullptr, notification_id, out);
}

size_t ccord_platform_notify_map_count(const ccord_platform *p)
{
  return ccord_desktop_notify_map_count(p ? p->desktop : nullptr);
}

void ccord_platform_inject_notify_action(ccord_platform *p, uint32_t notification_id, const char *action_key)
{
  if (p) ccord_desktop_inject_notify_action(p->desktop, notification_id, action_key);
}

void ccord_platform_inject_notify_closed(ccord_platform *p, uint32_t notification_id)
{
  if (p) ccord_desktop_inject_notify_closed(p->desktop, notification_id);
}

void ccord_platform_inject_notify_activation_token(ccord_platform *p, uint32_t notification_id, const char *token)
{
  if (p) ccord_desktop_inject_activation_token(p->desktop, notification_id, token);
}

size_t ccord_platform_close_channel_notifications(ccord_platform *p, uint64_t channel_id, uint64_t upto_message_id)
{
  return p ? ccord_desktop_close_channel_notifications(p->desktop, channel_id, upto_message_id) : 0;
}

uint32_t ccord_platform_notify_count(const ccord_platform *p)
{
  return p ? p->notify_count : 0;
}

bool ccord_platform_notifications_available(const ccord_platform *p)
{
  return p && ccord_desktop_notifications_available(p->desktop);
}

bool ccord_platform_tray_available(const ccord_platform *p)
{
  if (p && p->tray_override != 0) return p->tray_override > 0;
  return p && ccord_desktop_tray_available(p->desktop);
}

bool ccord_platform_global_shortcuts_available(const ccord_platform *p)
{
  return p && ccord_desktop_global_shortcuts_available(p->desktop);
}

const char *ccord_platform_last_notify_title(const ccord_platform *p)
{
  return ccord_desktop_last_notify_title(p ? p->desktop : nullptr);
}

const char *ccord_platform_last_notify_body(const ccord_platform *p)
{
  return ccord_desktop_last_notify_body(p ? p->desktop : nullptr);
}

void ccord_platform_set_unread_badge(ccord_platform *p, uint32_t unread_count, uint32_t mention_count)
{
  ccord_platform_set_badge_counts(p, unread_count, mention_count, mention_count);
}

void ccord_platform_set_badge_counts(ccord_platform *p, uint32_t unread_count, uint32_t mention_count,
                                     uint32_t tray_count)
{
  if (!p) return;
  p->badge_unread = unread_count;
  p->badge_mentions = mention_count;
  p->badge_tray = tray_count;
  ccord_desktop_set_badge(p->desktop, unread_count, mention_count, tray_count);
  apply_title_badge(p);
}

bool ccord_platform_tray_pixmap(const ccord_platform *p, size_t index, int *w, int *h, const uint8_t **argb)
{
  return p && ccord_desktop_tray_pixmap(p->desktop, index, w, h, argb);
}

uint32_t ccord_platform_tray_new_icon_count(const ccord_platform *p)
{
  return p ? ccord_desktop_new_icon_count(p->desktop) : 0;
}

void ccord_platform_unread_badge(const ccord_platform *p, uint32_t *unread_count, uint32_t *mention_count)
{
  if (unread_count) *unread_count = p ? p->badge_unread : 0;
  if (mention_count) *mention_count = p ? p->badge_mentions : 0;
}

bool ccord_platform_bind_global_shortcuts(ccord_platform *p, const ccord_global_shortcut *shortcuts, size_t count)
{
  return p && ccord_desktop_bind_shortcuts(p->desktop, shortcuts, count);
}

ccord_status ccord_platform_inject_hotkey(ccord_platform *p, const char *shortcut_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!p || !shortcut_id) return ccord_error_set(err, CCORD_ERR_INVALID, "hotkey: null argument");
  size_t len = strlen(shortcut_id);
  if (len == 0 || len >= HOTKEY_ID_MAX || !ccord_utf8_valid(shortcut_id, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "hotkey: invalid shortcut id");
  }
  if (strcmp(shortcut_id, CCORD_SHORTCUT_PUSH_TO_TALK) == 0) {
    platform_shortcut_state_cb(p, shortcut_id, true);   // same path as the portal's Activated signal
    return CCORD_OK;
  }
  hotkey_push(p, shortcut_id, len);
  platform_wake(p);
  return CCORD_OK;
}

ccord_status ccord_platform_inject_shortcut_deactivated(ccord_platform *p, const char *shortcut_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!p || !shortcut_id || !shortcut_id[0]) return ccord_error_set(err, CCORD_ERR_INVALID, "hotkey: null argument");
  platform_shortcut_state_cb(p, shortcut_id, false);
  return CCORD_OK;
}

void ccord_platform_inject_portal_session_closed(ccord_platform *p)
{
  if (p) platform_shortcut_state_cb(p, nullptr, false);
}

bool ccord_platform_global_ptt_held(const ccord_platform *p)
{
  return p && atomic_load(&p->global_ptt_held);
}


// ---- Mobile & Cross-Platform Capabilities ----

bool ccord_platform_is_mobile(const ccord_platform *p)
{
  if (p && p->form_factor != CCORD_FORM_DESKTOP) return true;
  return ccord_os_is_mobile();
}

ccord_form_factor ccord_platform_form_factor(const ccord_platform *p)
{
  if (p) return p->form_factor;
  return ccord_os_is_mobile() ? CCORD_FORM_PHONE : CCORD_FORM_DESKTOP;
}

bool ccord_platform_has_touch(const ccord_platform *p)
{
  if (ccord_platform_is_mobile(p)) return true;
  int count = 0;
  SDL_TouchID *devs = SDL_GetTouchDevices(&count);
  SDL_free(devs);
  return count > 0;
}

bool ccord_platform_has_haptics(const ccord_platform *p)
{
  (void)p;
#if defined(__ANDROID__)
  return true;
#else
  return false;
#endif
}

bool ccord_platform_has_share(const ccord_platform *p)
{
  (void)p;
#if defined(__ANDROID__)
  return true;
#else
  return false;
#endif
}

bool ccord_platform_in_background(const ccord_platform *p)
{
  return p && atomic_load(&p->in_background);
}

ccord_safe_insets ccord_platform_safe_area(const ccord_platform *p)
{
  return p ? p->safe_insets : (ccord_safe_insets){0};
}

float ccord_platform_keyboard_inset(const ccord_platform *p)
{
  return p ? p->keyboard_inset : 0.0f;
}

bool ccord_platform_haptic_impact(ccord_platform *p, ccord_haptic_style style)
{
  if (!p || p->headless) return false;
#if defined(__ANDROID__)
  return ccord_android_haptic_impact((int)style);
#else
  (void)style;
  return false;
#endif
}

bool ccord_platform_haptic_notify(ccord_platform *p, ccord_haptic_notify_kind kind)
{
  if (!p || p->headless) return false;
#if defined(__ANDROID__)
  return ccord_android_haptic_notify((int)kind);
#else
  (void)kind;
  return false;
#endif
}

bool ccord_platform_share_text(ccord_platform *p, const char *title, const char *text)
{
  if (!p || p->headless || !text || !text[0]) return false;
  if (!ccord_utf8_valid(text, strlen(text))) return false;
  if (title && !ccord_utf8_valid(title, strlen(title))) return false;
#if defined(__ANDROID__)
  return ccord_android_share_text(title, text);
#else
  (void)title;
  return false;
#endif
}

bool ccord_platform_keep_screen_on(ccord_platform *p, bool on)
{
  if (!p || p->headless) return false;
  if (on) (void)SDL_DisableScreenSaver();
  else (void)SDL_EnableScreenSaver();
#if defined(__ANDROID__)
  return ccord_android_keep_screen_on(on);
#else
  return true;
#endif
}

bool ccord_platform_set_orientation(ccord_platform *p, ccord_orientation_mode mode)
{
  if (!p || p->headless) return false;
#if defined(__ANDROID__)
  return ccord_android_set_orientation((int)mode);
#else
  (void)mode;
  return false;
#endif
}

void ccord_platform_dismiss_keyboard(ccord_platform *p)
{
  if (!p || !p->window) return;
  if (p->text_input_on) {
    SDL_StopTextInput(p->window);
    p->text_input_on = false;
  }
}

void ccord_platform_set_voice_call_active(ccord_platform *p, bool active, const char *channel_name,
                                          bool muted, bool deafened)
{
  if (!p) return;
  p->voice_service_active = active;
#if defined(__ANDROID__)
  if (!p->headless) {
    ccord_android_set_voice_service(active, channel_name, muted, deafened);
    ccord_android_set_audio_comm_mode(active);
  }
#else
  (void)channel_name;
  (void)muted;
  (void)deafened;
#endif
}

void ccord_platform_set_keep_alive(ccord_platform *p, bool active)
{
  if (!p || p->keep_alive_active == active) return;
  p->keep_alive_active = active;
#if defined(__ANDROID__)
  if (!p->headless) ccord_android_set_keep_alive(active);
#endif
}

bool ccord_platform_request_permission(ccord_platform *p, const char *permission)
{
  if (!p || !permission || !permission[0]) return false;
#if defined(__ANDROID__)
  if (p->headless) return true;
  return SDL_RequestAndroidPermission(permission, nullptr, nullptr);
#else
  return true;
#endif
}

void ccord_platform_notify_first_frame(ccord_platform *p)
{
  if (!p || p->first_frame_notified) return;
  p->first_frame_notified = true;
#if defined(__ANDROID__)
  if (!p->headless) ccord_android_on_first_frame();
#endif
}

void ccord_platform_set_theme_bg(ccord_platform *p, uint32_t argb, bool is_light)
{
  if (!p || p->headless) return;
#if defined(__ANDROID__)
  ccord_android_set_startup_theme(argb, is_light);
#else
  (void)argb;
  (void)is_light;
#endif
}

void ccord_platform_set_auth_autofill(ccord_platform *p, const ccord_auth_autofill_state *st)
{
  if (!p || !st || p->headless) return;
#if defined(__ANDROID__)
  float s = p->window ? platform_window_scale(p->window) : 1.0f;
  if (s <= 0.0f) s = 1.0f;
  ccord_android_set_auth_autofill(
      (int)st->mode, (int)st->active_field, st->commit,
      (int)lroundf(st->email_x * s), (int)lroundf(st->email_y * s),
      (int)lroundf(st->email_w * s), (int)lroundf(st->email_h * s),
      (int)lroundf(st->pw_x * s), (int)lroundf(st->pw_y * s),
      (int)lroundf(st->pw_w * s), (int)lroundf(st->pw_h * s),
      (int)lroundf(st->otp_x * s), (int)lroundf(st->otp_y * s),
      (int)lroundf(st->otp_w * s), (int)lroundf(st->otp_h * s),
      st->email_text ? st->email_text : "",
      st->pw_text ? st->pw_text : "",
      st->otp_text ? st->otp_text : "");
#else
  (void)st;
#endif
}

bool ccord_platform_poll_autofill(ccord_platform *p, ccord_auth_autofill_field *out_field, char *out_text, size_t cap)
{
  if (!p || p->headless) return false;
#if defined(__ANDROID__)
  int field = 0;
  if (!ccord_android_poll_autofill(&field, out_text, cap)) return false;
  if (out_field) {
    *out_field = (field >= 1 && field <= 3) ? (ccord_auth_autofill_field)field : CCORD_AUTOFILL_FIELD_NONE;
  }
  return true;
#else
  (void)out_field;
  (void)out_text;
  (void)cap;
  return false;
#endif
}
