// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/platform/platform.h>
#include <ccord/platform/link_watch.h>

#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>
#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include "android_bridge.h"
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#if CCORD_HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#endif

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>

#include <ccord/platform/app_icon.h>

#include "desktop.h"

#define CCORD_PLATFORM_APP_ID "C-Cord"
#include "devices_internal.h"

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 800
#define DROP_QUEUE_CAP 16u
#define DROP_PATH_MAX 1024u
#define HOTKEY_QUEUE_CAP 16u
// Shortcut ids must fit the CCORD_EV_HOTKEY event text buffer whole (no truncation on delivery).
#define HOTKEY_ID_MAX sizeof((ccord_input_event){0}.text)
#define BASE_TITLE_MAX 256u

// libswscale's SIMD yuv->RGBA kernels store whole vectors for the last pixels of a row, so for widths that are
// not a multiple of 16 they write up to 32 bytes (measured: 32 at 200x150 yuv420p) past the end of a destination
// sized exactly w*h*4. That silently corrupts the next heap chunk and glibc aborts or faults later. Every ccord
// buffer handed to sws_scale as a destination is over-allocated by this tail; sizes and copies stay w*h*4.
#define PLATFORM_SWS_DST_PAD 128u

typedef struct platform_drop_buf {
  char queue[DROP_QUEUE_CAP][DROP_PATH_MAX];
  char last[DROP_PATH_MAX];
} platform_drop_buf;

struct ccord_platform {
  SDL_Window *window;
  SDL_Cursor *cursors[CCORD_CURSOR_COUNT];       // indexed by ccord_cursor_kind, created lazily
  bool cursor_unavailable[CCORD_CURSOR_COUNT];   // creation failed once (e.g. dummy driver): do not retry or log again
  ccord_cursor_kind cursor_kind;
  bool text_input_on;
  ccord_text_input_kind text_input_kind;
  bool first_frame_notified;
  char *pending_text;
  size_t pending_text_len;
  size_t pending_text_off;
  bool headless;
  bool focused;
  bool window_visible;            // tracked even under the dummy video driver (start_minimized / tray)
  bool minimized;                 // close request minimized the window (no tray to restore from)
  bool fullscreen;                // borderless desktop fullscreen (tracked even headless)
  int tray_override;              // test hook: -1 force unavailable, 1 force available, 0 use the real tray
  bool close_to_tray;             // window-close request hides instead of quitting
  ccord_tray_action_fn tray_fn;
  void *tray_ud;
  ccord_notify_click_fn notify_click_fn;   // a notification's default action: open its message
  void *notify_click_ud;
  uint32_t notify_count;
  uint8_t *clip_image;
  size_t clip_image_len;
  SDL_Mutex *drop_mutex;
  platform_drop_buf *drop_buf;    // lazily allocated on first file drop/pick/inject
  size_t drop_tail;
  size_t drop_ev_head;
  size_t drop_ev_count;
  size_t drop_pop_head;
  size_t drop_pop_count;
  ccord_desktop *desktop;       // DBus notifications / SNI tray / GlobalShortcuts (offline-safe)
  char base_title[BASE_TITLE_MAX];
  uint32_t badge_unread;
  uint32_t badge_mentions;
  uint32_t badge_tray;
  bool window_icon_accepted;    // SDL_SetWindowIcon returned true
  uint32_t window_icon_sizes;   // images handed to SDL_SetWindowIcon (test hook)
  uint32_t flash_requests;      // taskbar attention requests made (the app never makes any; a test hook)
  SDL_Mutex *hotkey_mutex;
  char hotkey_queue[HOTKEY_QUEUE_CAP][HOTKEY_ID_MAX];
  size_t hotkey_head;
  size_t hotkey_count;
  ccord_linkwatch *link;        // wake-from-sleep / default-route change detector
  ccord_link_event_fn link_fn;
  void *link_ud;
  uint64_t last_bg_pump_ms;     // throttles DBus/link polling during rapid timeout=0 event drains
  bool bg_pumped_once;
  atomic_bool link_dirty;       // set on injected link events for immediate delivery
  atomic_bool global_ptt_held;   // push-to-talk shortcut held (portal Activated .. Deactivated / session lost)
  atomic_bool in_background;     // true while Android app is backgrounded (EGL surface paused)
  atomic_bool pending_bg_ev;
  atomic_bool pending_fg_ev;
  atomic_bool pending_lowmem_ev;
  atomic_bool pending_reset_ev;
  bool pending_safe_area_ev;
  bool pending_keyboard_ev;
  ccord_safe_insets safe_insets;
  float keyboard_inset;
  ccord_form_factor form_factor;
  bool voice_service_active;
};

static char s_last_opened_url[1024];
static uint8_t *s_test_clip_image;
static size_t s_test_clip_image_len;

static float safe_scale(float s)
{
  return (s > 0.0f && isfinite(s)) ? s : 1.0f;
}

static float window_scale(SDL_Window *w)
{
  return safe_scale(SDL_GetWindowDisplayScale(w));
}

static float window_density(SDL_Window *w)
{
  return safe_scale(SDL_GetWindowPixelDensity(w));
}

static void copy_utf8_prefix(char *dst, size_t cap, const char *src, size_t len);

#define PLATFORM_PENDING_TEXT_MAX 65536u

static void pending_text_clear(ccord_platform *p)
{
  if (!p) return;
  ccord_free(p->pending_text);
  p->pending_text = nullptr;
  p->pending_text_len = 0;
  p->pending_text_off = 0;
}

static bool pending_text_pop(ccord_platform *p, ccord_input_event *out)
{
  if (!p || !p->pending_text || p->pending_text_off >= p->pending_text_len) return false;
  const char *rem = p->pending_text + p->pending_text_off;
  size_t rem_len = p->pending_text_len - p->pending_text_off;
  size_t max_chunk = sizeof out->text - 1u;
  size_t chunk = rem_len < max_chunk ? rem_len : max_chunk;
  while (chunk > 0 && !ccord_utf8_valid(rem, chunk)) chunk--;
  if (chunk == 0) {
    pending_text_clear(p);
    return false;
  }
  memset(out, 0, sizeof *out);
  out->type = CCORD_IN_TEXT;
  memcpy(out->text, rem, chunk);
  out->text[chunk] = '\0';
  p->pending_text_off += chunk;
  if (p->pending_text_off >= p->pending_text_len) pending_text_clear(p);
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
  if (!p->window) return;
  char buf[BASE_TITLE_MAX + 32u];
  if (p->badge_tray > 0) {
    snprintf(buf, sizeof buf, "%s (%u)", p->base_title, p->badge_tray);
  } else {
    snprintf(buf, sizeof buf, "%s", p->base_title);
  }
  SDL_SetWindowTitle(p->window, buf);
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

static bool SDLCALL platform_lifecycle_watch(void *userdata, SDL_Event *event)
{
  ccord_platform *p = (ccord_platform *)userdata;
  if (!p || !event) return true;
  switch (event->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
      atomic_store(&p->in_background, true);
      atomic_store(&p->pending_bg_ev, true);
      break;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
      atomic_store(&p->in_background, false);
      atomic_store(&p->pending_fg_ev, true);
      break;
    case SDL_EVENT_LOW_MEMORY:
      atomic_store(&p->pending_lowmem_ev, true);
      break;
    case SDL_EVENT_RENDER_TARGETS_RESET:
    case SDL_EVENT_RENDER_DEVICE_RESET:
      atomic_store(&p->pending_reset_ev, true);
      break;
    default:
      break;
  }
  return true;
}

static void refresh_sdl_safe_area(ccord_platform *p)
{
  if (!p || !p->window || p->headless) return;
  SDL_Rect safe = {0};
  int win_w = 0, win_h = 0;
  if (!SDL_GetWindowSafeArea(p->window, &safe) || !SDL_GetWindowSize(p->window, &win_w, &win_h)) return;
  if (win_w <= 0 || win_h <= 0) return;
  float ratio = window_density(p->window) / window_scale(p->window);
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
#if defined(__ANDROID__)
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
  p->drop_mutex = SDL_CreateMutex();
  SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(__ANDROID__)
  window_flags |= SDL_WINDOW_FULLSCREEN;
#else
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
#if defined(__ANDROID__)
  int ff = ccord_android_get_form_factor();
  p->form_factor = (ff == 2) ? CCORD_FORM_TABLET : CCORD_FORM_PHONE;
  ccord_android_register_platform(p);
  refresh_sdl_safe_area(p);
#else
  p->form_factor = CCORD_FORM_DESKTOP;
  p->window_icon_sizes = set_window_icon(p->window, &p->window_icon_accepted);
  // The window was created in screen coordinates; the requested size is in logical px. Fix it up once the
  // display scale is known (X11 with a content scale, Wayland fractional scaling).
  float scale = window_scale(p->window);
  float density = window_density(p->window);
  int want_w = (int)lroundf((float)c.width * scale / density);
  int want_h = (int)lroundf((float)c.height * scale / density);
  if (want_w != c.width || want_h != c.height) SDL_SetWindowSize(p->window, want_w, want_h);
  if (c.min_width > 0 || c.min_height > 0) {
    SDL_SetWindowMinimumSize(p->window, (int)lroundf((float)c.min_width * scale / density),
                             (int)lroundf((float)c.min_height * scale / density));
  }
#endif
  SDL_AddEventWatch(platform_lifecycle_watch, p);
  copy_utf8_prefix(p->base_title, sizeof p->base_title, c.title, strlen(c.title));
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
  ccord_android_register_platform(nullptr);
#endif
  if (p->text_input_on) SDL_StopTextInput(p->window);
  pending_text_clear(p);
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
    s = window_scale(p->window);
  }
  if (logical_w) *logical_w = (float)pw / s;
  if (logical_h) *logical_h = (float)ph / s;
  if (scale) *scale = s;
}

void ccord_platform_set_title(ccord_platform *p, const char *utf8)
{
  if (!p || !utf8 || !ccord_utf8_valid(utf8, strlen(utf8))) return;
  copy_utf8_prefix(p->base_title, sizeof p->base_title, utf8, strlen(utf8));
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
    float f = window_scale(p->window) / window_density(p->window);   // logical -> screen coordinates
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

static void copy_utf8_prefix(char *dst, size_t cap, const char *src, size_t len)
{
  if (cap == 0) return;
  size_t n = len < cap - 1 ? len : cap - 1;
  while (n > 0 && !ccord_utf8_valid(src, n)) n--;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

static platform_drop_buf *drop_buf_ensure(ccord_platform *p)
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
  platform_drop_buf *db = drop_buf_ensure(p);
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
  return p ? p->focused : false;
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
  if (focused) platform_flash(p, SDL_FLASH_CANCEL);
}

void ccord_platform_set_window_visible(ccord_platform *p, bool visible)
{
  if (!p || p->window_visible == visible) return;
  p->window_visible = visible;
  if (!visible) p->focused = false;
  if (!p->window) return;
  if (visible) {
    SDL_ShowWindow(p->window);
    SDL_RaiseWindow(p->window);
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
  SDL_ShowWindow(p->window);
  if (was_min || (SDL_GetWindowFlags(p->window) & SDL_WINDOW_MINIMIZED) != 0) {
    SDL_RestoreWindow(p->window);
  }
  SDL_RaiseWindow(p->window);
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
}

// Samples the clocks and the route socket, then hands every pending event to the callback (main thread, no waiting).
static void platform_pump_link(ccord_platform *p)
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
  if (!p || p->focused) return false;
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

// ---- audio media plane ----

#define AUDIO_DEFAULT_RATE 48000
#define AUDIO_DEFAULT_CHANNELS 2
#define AUDIO_DEFAULT_FRAMES 960

// struct ccord_audio lives in devices_internal.h (shared with the hot-plug registry).

ccord_audio *ccord_audio_create(const ccord_audio_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  ccord_audio_config c = {.sample_rate = AUDIO_DEFAULT_RATE, .channels = AUDIO_DEFAULT_CHANNELS,
                          .frame_samples = AUDIO_DEFAULT_FRAMES};
  if (cfg) {
    c = *cfg;
    if (c.sample_rate == 0) c.sample_rate = AUDIO_DEFAULT_RATE;
    if (c.channels == 0) c.channels = AUDIO_DEFAULT_CHANNELS;
    if (c.frame_samples == 0) c.frame_samples = AUDIO_DEFAULT_FRAMES;
  }
  if (c.sample_rate < 8000 || c.sample_rate > 192000) {
    ccord_error_set(err, CCORD_ERR_INVALID, "audio: sample rate out of range");
    return nullptr;
  }
  if (c.channels < 1 || c.channels > 8) {
    ccord_error_set(err, CCORD_ERR_INVALID, "audio: channel count out of range");
    return nullptr;
  }
  if (c.frame_samples < 1 || c.frame_samples > 48000) {
    ccord_error_set(err, CCORD_ERR_INVALID, "audio: frame size out of range");
    return nullptr;
  }
  ccord_audio *a = ccord_calloc(1, sizeof *a);
  if (!a) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "audio: out of memory");
    return nullptr;
  }
  a->stream_mtx = SDL_CreateMutex();
  if (!a->stream_mtx) {
    ccord_free(a);
    ccord_error_set(err, CCORD_ERR_NOMEM, "audio: mutex create failed");
    return nullptr;
  }
  a->capture = c.capture;
  a->frame_bytes = (uint64_t)c.channels * sizeof(int16_t);
  const char *vdrv = SDL_GetCurrentVideoDriver();
  if (vdrv && strcmp(vdrv, "dummy") == 0) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  }
  // Headless CI has no audio device. A valid inert handle keeps the media loop
  // simple: it drains silence and drops playback instead of failing to start.
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    CCORD_LOG_D("audio: SDL audio unavailable: %s", SDL_GetError());
    return a;
  }
  a->audio_init = true;
  a->spec = (SDL_AudioSpec){.format = SDL_AUDIO_S16, .channels = c.channels, .freq = c.sample_rate};
  SDL_AudioDeviceID def = c.capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  a->opened_id = ccord_audio_parse_device_id(c.capture, c.device_id);
  a->stream = SDL_OpenAudioDeviceStream(a->opened_id, &a->spec, nullptr, nullptr);
  if (!a->stream && a->opened_id != def) {
    // An explicit device id may be stale (unplugged since the settings page was
    // drawn); fall back to the system default instead of going silent.
    CCORD_LOG_D("audio: device %u unavailable (%s); using default", (unsigned)a->opened_id, SDL_GetError());
    a->opened_id = def;
    a->stream = SDL_OpenAudioDeviceStream(def, &a->spec, nullptr, nullptr);
  }
  if (!a->stream) {
    CCORD_LOG_D("audio: no %s device: %s", c.capture ? "capture" : "playback", SDL_GetError());
    return a;
  }
  if (!SDL_ResumeAudioStreamDevice(a->stream)) {
    CCORD_LOG_D("audio: cannot start %s stream: %s", c.capture ? "capture" : "playback", SDL_GetError());
    SDL_DestroyAudioStream(a->stream);
    a->stream = nullptr;
    return a;
  }
  a->device = SDL_GetAudioStreamDevice(a->stream);
  ccord_audio_devices_register(a);
  return a;
}

void ccord_audio_destroy(ccord_audio *a)
{
  if (!a) return;
  ccord_audio_devices_unregister(a);
  if (a->stream_mtx) SDL_LockMutex(a->stream_mtx);
  if (a->stream) {
    SDL_DestroyAudioStream(a->stream);
    a->stream = nullptr;
  }
  if (a->stream_mtx) {
    SDL_UnlockMutex(a->stream_mtx);
    SDL_DestroyMutex(a->stream_mtx);
  }
  if (a->audio_init) SDL_QuitSubSystem(SDL_INIT_AUDIO);
  ccord_free(a);
}

bool ccord_audio_available(const ccord_audio *a)
{
  if (!a) return false;
  if (a->stream_mtx) SDL_LockMutex(a->stream_mtx);
  bool ok = a->stream != nullptr;
  if (a->stream_mtx) SDL_UnlockMutex(a->stream_mtx);
  return ok;
}

size_t ccord_audio_read(ccord_audio *a, int16_t *pcm, size_t frames)
{
  if (!a || !pcm || frames == 0 || !a->capture || a->frame_bytes == 0) return 0;
  if (frames > (size_t)(INT_MAX / (int)a->frame_bytes)) return 0;
  int want = (int)(frames * (size_t)a->frame_bytes);
  int got = 0;
  if (a->stream_mtx) SDL_LockMutex(a->stream_mtx);
  if (a->stream) got = SDL_GetAudioStreamData(a->stream, pcm, want);
  if (a->stream_mtx) SDL_UnlockMutex(a->stream_mtx);
  if (got <= 0) return 0;
  return (size_t)got / (size_t)a->frame_bytes;
}

size_t ccord_audio_write(ccord_audio *a, const int16_t *pcm, size_t frames)
{
  if (!a || !pcm || frames == 0 || a->capture || a->frame_bytes == 0) return 0;
  if (frames > (size_t)(INT_MAX / (int)a->frame_bytes)) return 0;
  int bytes = (int)(frames * (size_t)a->frame_bytes);
  bool ok = false;
  if (a->stream_mtx) SDL_LockMutex(a->stream_mtx);
  if (a->stream) ok = SDL_PutAudioStreamData(a->stream, pcm, bytes);
  if (a->stream_mtx) SDL_UnlockMutex(a->stream_mtx);
  return ok ? frames : 0;
}

void ccord_audio_clear(ccord_audio *a)
{
  if (!a) return;
  if (a->stream_mtx) SDL_LockMutex(a->stream_mtx);
  if (a->stream) (void)SDL_ClearAudioStream(a->stream);
  if (a->stream_mtx) SDL_UnlockMutex(a->stream_mtx);
}

// ---- video streaming codec pipeline ----

// Mutable lazily-initialized state for ccord_video_stream (accessed via pointer so const getters
// can lazily materialize last_rgba or probe PipeWire without casting away const).
typedef struct video_stream_lazy {
  bool pipewire_probed;
  bool pipewire_available;
  struct SwsContext *sws_to_rgba;
  struct SwsContext *sws_preview_rgba;
  uint8_t *last_rgba;
  size_t last_rgba_cap;
  int last_w;
  int last_h;
  bool preview_wanted;
  bool enc_preview_dirty;
} video_stream_lazy;

struct ccord_video_stream {
  int width;
  int height;
  int fps;
  int bitrate;
  bool use_pipewire;
  bool last_keyframe;
  bool force_keyframe;
  const AVCodec *enc_codec;
  const AVCodec *dec_codec;
  AVCodecContext *enc_ctx;
  AVCodecContext *dec_ctx;   // opened lazily on first decode_rgba()
  AVFrame *enc_frame;
  AVFrame *dec_frame;        // allocated lazily on first decode_rgba()
  AVPacket *pkt;
  struct SwsContext *sws_to_yuv;
  int64_t pts;
  uint32_t synth_step;
  video_stream_lazy *lazy;
  video_stream_lazy lazy_storage;
};

void ccord_video_resolve_dimensions(int src_w, int src_h, int target_height, int *out_w, int *out_h)
{
  int sw = src_w > 0 ? src_w : 1280;
  int sh = src_h > 0 ? src_h : 720;
  int tw = sw;
  int th = sh;
  if (target_height > 0 && target_height < sh) {
    th = target_height;
    tw = (int)(((int64_t)sw * (int64_t)th + (sh / 2)) / sh);
  } else if (target_height > 0 && src_w <= 0) {
    th = target_height;
    tw = (target_height * 16) / 9;
  }
  if (tw < 16) tw = 16;
  if (th < 16) th = 16;
  if (tw > 3840) tw = 3840;
  if (th > 2160) th = 2160;
  tw = (tw + 1) & ~1;
  th = (th + 1) & ~1;
  if (out_w) *out_w = tw;
  if (out_h) *out_h = th;
}

static bool open_video_codec_pair(ccord_video_stream *v, const AVCodec *enc)
{
  if (!enc) return false;
  const AVCodec *dec = avcodec_find_decoder(enc->id);
  if (!dec) return false;
  AVCodecContext *ec = avcodec_alloc_context3(enc);
  if (!ec) return false;
  ec->width = v->width;
  ec->height = v->height;
  ec->time_base = (AVRational){1, v->fps};
  ec->framerate = (AVRational){v->fps, 1};
  ec->pix_fmt = (enc->name && strcmp(enc->name, "h264_mediacodec") == 0) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
  ec->bit_rate = v->bitrate;
  ec->gop_size = v->fps;
  ec->max_b_frames = 0;
  ec->thread_count = 1;
  AVDictionary *opts = nullptr;
  if (enc->id == AV_CODEC_ID_H264) {
    if (enc->name && strcmp(enc->name, "h264_mediacodec") == 0) {
      av_dict_set(&opts, "bitrate_mode", "cbr", 0);
      av_dict_set(&opts, "i_frame_interval", "1", 0);
    } else if (enc->name && strcmp(enc->name, "libx264") == 0) {
      av_dict_set(&opts, "preset", "ultrafast", 0);
      av_dict_set(&opts, "tune", "zerolatency", 0);
      av_dict_set(&opts, "annexb", "1", 0);
    } else if (enc->name && strcmp(enc->name, "h264_nvenc") == 0) {
      av_dict_set(&opts, "preset", "p1", 0);
      av_dict_set(&opts, "tune", "ull", 0);
      av_dict_set(&opts, "zerolatency", "1", 0);
    } else if (enc->name && strcmp(enc->name, "h264_amf") == 0) {
      av_dict_set(&opts, "usage", "ultralowlatency", 0);
    } else if (enc->name && strcmp(enc->name, "h264_videotoolbox") == 0) {
      av_dict_set(&opts, "realtime", "1", 0);
    } else if (enc->name && strcmp(enc->name, "h264_qsv") == 0) {
      av_dict_set(&opts, "preset", "veryfast", 0);
      av_dict_set(&opts, "async_depth", "1", 0);
    } else if (enc->name && strcmp(enc->name, "h264_mf") == 0) {
      av_dict_set(&opts, "scenario", "live_streaming", 0);
    } else if (enc->name && strcmp(enc->name, "libopenh264") == 0) {
      av_dict_set(&opts, "allow_skip_frames", "0", 0);
    }
  }
  int rc = avcodec_open2(ec, enc, &opts);
  av_dict_free(&opts);
  if (rc < 0) {
    avcodec_free_context(&ec);
    return false;
  }
  // Decoder context is opened lazily in decode_rgba() so outbound-only streams never pay its memory cost.
  v->enc_codec = enc;
  v->dec_codec = dec;
  v->enc_ctx = ec;
  v->dec_ctx = nullptr;
  return true;
}

static ccord_status ensure_video_decoder(ccord_video_stream *v, ccord_error *err)
{
  if (v->dec_ctx && v->dec_frame) return CCORD_OK;
  if (!v->dec_codec) {
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "video decode: no matching decoder");
  }
  if (!v->dec_ctx) {
    AVCodecContext *dc = avcodec_alloc_context3(v->dec_codec);
    if (!dc) return ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: out of memory");
    dc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    dc->thread_count = 1;
    if (avcodec_open2(dc, v->dec_codec, nullptr) < 0) {
      avcodec_free_context(&dc);
      return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "video decode: avcodec_open2 failed");
    }
    v->dec_ctx = dc;
  }
  if (!v->dec_frame) {
    v->dec_frame = av_frame_alloc();
    if (!v->dec_frame) return ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: out of memory");
  }
  return CCORD_OK;
}

static bool probe_h264_encoders(ccord_video_stream *v, const char *preferred)
{
  if (preferred && preferred[0] != '\0' && strcmp(preferred, "auto") != 0) {
    if (open_video_codec_pair(v, avcodec_find_encoder_by_name(preferred))) {
      return true;
    }
  }
  static const char *const k_priority[] = {
#if defined(__APPLE__)
      "h264_videotoolbox",
      "libx264",
      "libopenh264",
#elif defined(_WIN32)
      "h264_nvenc",
      "h264_amf",
      "h264_qsv",
      "h264_mf",
      "libx264",
      "libopenh264",
#elif defined(__ANDROID__)
      "h264_mediacodec",
      "libx264",
      "libopenh264",
#else
      "h264_nvenc",
      "h264_vaapi",
      "h264_amf",
      "libx264",
      "libopenh264",
#endif
  };
  for (size_t i = 0; i < sizeof(k_priority) / sizeof(k_priority[0]); i++) {
    if (open_video_codec_pair(v, avcodec_find_encoder_by_name(k_priority[i]))) {
      return true;
    }
  }
  if (open_video_codec_pair(v, avcodec_find_encoder(AV_CODEC_ID_H264))) {
    return true;
  }
  return false;
}

static bool probe_pipewire(void)
{
#if !CCORD_HAVE_PIPEWIRE
  return false;
#else
  if (!pw_get_library_version()) return false;
  pw_init(nullptr, nullptr);
  struct pw_main_loop *loop = pw_main_loop_new(nullptr);
  if (!loop) {
    pw_deinit();
    return false;
  }
  struct pw_context *ctx = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
  if (!ctx) {
    pw_main_loop_destroy(loop);
    pw_deinit();
    return false;
  }
  struct pw_core *core = pw_context_connect(ctx, nullptr, 0);
  bool available = (core != nullptr);
  if (core) pw_core_disconnect(core);
  pw_context_destroy(ctx);
  pw_main_loop_destroy(loop);
  pw_deinit();
  return available;
#endif
}

ccord_video_stream *ccord_video_stream_create(const ccord_video_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  av_log_set_level(AV_LOG_ERROR);
  ccord_video_config c = {.width = 640, .height = 360, .fps = 30, .bitrate = 1500000, .codec = CCORD_VIDEO_CODEC_H264};
  if (cfg) {
    c = *cfg;
    if (c.width == 0) c.width = 640;
    if (c.height == 0) c.height = 360;
    if (c.fps == 0) c.fps = 30;
    if (c.bitrate == 0) c.bitrate = 1500000;
  }
  if (c.width < 16 || c.width > 3840 || c.height < 16 || c.height > 2160) {
    ccord_error_set(err, CCORD_ERR_INVALID, "video: dimensions out of range");
    return nullptr;
  }
  if (c.fps < 1 || c.fps > 120 || c.bitrate < 64000 || c.bitrate > 50000000) {
    ccord_error_set(err, CCORD_ERR_INVALID, "video: fps or bitrate out of range");
    return nullptr;
  }
  ccord_video_stream *v = ccord_calloc(1, sizeof *v);
  if (!v) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "video: out of memory");
    return nullptr;
  }
  v->width = (c.width + 1) & ~1;
  v->height = (c.height + 1) & ~1;
  v->fps = c.fps;
  v->bitrate = c.bitrate;
  v->use_pipewire = c.use_pipewire;
  v->lazy = &v->lazy_storage;
  bool opened = false;
  if (c.codec == CCORD_VIDEO_CODEC_H264) {
    opened = probe_h264_encoders(v, c.preferred_encoder);
  } else {
    opened = open_video_codec_pair(v, avcodec_find_encoder(AV_CODEC_ID_MPEG4));
    if (!opened) opened = probe_h264_encoders(v, c.preferred_encoder);
  }
  if (!opened) {
    ccord_video_stream_destroy(v);
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "video: no H.264 or MPEG4 codec available");
    return nullptr;
  }
  v->enc_frame = av_frame_alloc();
  v->pkt = av_packet_alloc();
  if (!v->enc_frame || !v->pkt) {
    ccord_video_stream_destroy(v);
    ccord_error_set(err, CCORD_ERR_NOMEM, "video: cannot allocate AVFrame/AVPacket");
    return nullptr;
  }
  v->enc_frame->format = v->enc_ctx->pix_fmt;
  v->enc_frame->width = v->width;
  v->enc_frame->height = v->height;
  if (av_frame_get_buffer(v->enc_frame, 32) < 0) {
    ccord_video_stream_destroy(v);
    ccord_error_set(err, CCORD_ERR_NOMEM, "video: cannot allocate YUV frame buffer");
    return nullptr;
  }
  return v;
}

void ccord_video_stream_destroy(ccord_video_stream *v)
{
  if (!v) return;
  if (v->sws_to_yuv) sws_freeContext(v->sws_to_yuv);
  if (v->lazy) {
    if (v->lazy->sws_to_rgba) sws_freeContext(v->lazy->sws_to_rgba);
    if (v->lazy->sws_preview_rgba) sws_freeContext(v->lazy->sws_preview_rgba);
    ccord_free(v->lazy->last_rgba);
  }
  if (v->enc_frame) av_frame_free(&v->enc_frame);
  if (v->dec_frame) av_frame_free(&v->dec_frame);
  if (v->pkt) av_packet_free(&v->pkt);
  if (v->enc_ctx) avcodec_free_context(&v->enc_ctx);
  if (v->dec_ctx) avcodec_free_context(&v->dec_ctx);
  ccord_free(v);
}

const char *ccord_video_stream_codec_name(const ccord_video_stream *v)
{
  return (v && v->enc_codec && v->enc_codec->name) ? v->enc_codec->name : "";
}

int ccord_video_stream_width(const ccord_video_stream *v)
{
  return v ? v->width : 0;
}

int ccord_video_stream_height(const ccord_video_stream *v)
{
  return v ? v->height : 0;
}

int ccord_video_stream_fps(const ccord_video_stream *v)
{
  return v ? v->fps : 0;
}

bool ccord_video_stream_last_keyframe(const ccord_video_stream *v)
{
  return v ? v->last_keyframe : false;
}

void ccord_video_stream_force_keyframe(ccord_video_stream *v)
{
  if (v) v->force_keyframe = true;
}

bool ccord_video_stream_pipewire_available(const ccord_video_stream *v)
{
  if (!v || !v->use_pipewire || !v->lazy) return false;
  if (!v->lazy->pipewire_probed) {
    v->lazy->pipewire_probed = true;
    v->lazy->pipewire_available = probe_pipewire();
  }
  return v->lazy->pipewire_available;
}

static bool video_stream_ensure_rgba_buf(video_stream_lazy *lz, int w, int h)
{
  size_t need = (size_t)w * (size_t)h * 4u + PLATFORM_SWS_DST_PAD;
  if (!lz->last_rgba || need > lz->last_rgba_cap) {
    uint8_t *nbuf = ccord_malloc(need);
    if (!nbuf) return false;
    ccord_free(lz->last_rgba);
    lz->last_rgba = nbuf;
    lz->last_rgba_cap = need;
  }
  lz->last_w = w;
  lz->last_h = h;
  return true;
}

static void video_stream_update_preview(ccord_video_stream *v, const uint8_t *pixels, int width, int height,
                                        int src_stride, enum AVPixelFormat src_fmt)
{
  video_stream_lazy *lz = v->lazy;
  if (!video_stream_ensure_rgba_buf(lz, v->width, v->height)) return;
  if (width == v->width && height == v->height) {
    const size_t row_bytes = (size_t)v->width * 4u;
    if (src_fmt == AV_PIX_FMT_RGBA) {
      if ((size_t)src_stride == row_bytes) {
        memcpy(lz->last_rgba, pixels, row_bytes * (size_t)v->height);
      } else {
        for (int y = 0; y < v->height; y++) {
          memcpy(lz->last_rgba + (size_t)y * row_bytes, pixels + (size_t)y * (size_t)src_stride, row_bytes);
        }
      }
      lz->enc_preview_dirty = false;
      return;
    }
    if (src_fmt == AV_PIX_FMT_BGRA) {
      for (int y = 0; y < v->height; y++) {
        const uint8_t *restrict s = pixels + (size_t)y * (size_t)src_stride;
        uint8_t *restrict d = lz->last_rgba + (size_t)y * row_bytes;
        for (int x = 0; x < v->width; x++, s += 4, d += 4) {
          d[0] = s[2];
          d[1] = s[1];
          d[2] = s[0];
          d[3] = s[3];
        }
      }
      lz->enc_preview_dirty = false;
      return;
    }
  }
  lz->sws_preview_rgba = sws_getCachedContext(lz->sws_preview_rgba, width, height, src_fmt, v->width, v->height,
                                              AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (lz->sws_preview_rgba) {
    const uint8_t *src_slices[4] = {pixels, nullptr, nullptr, nullptr};
    int src_linesize[4] = {src_stride, 0, 0, 0};
    uint8_t *dst_slices[4] = {lz->last_rgba, nullptr, nullptr, nullptr};
    int dst_linesize[4] = {v->width * 4, 0, 0, 0};
    sws_scale(lz->sws_preview_rgba, src_slices, src_linesize, 0, height, dst_slices, dst_linesize);
    lz->enc_preview_dirty = false;
  }
}

static ccord_status encode_pixels_internal(ccord_video_stream *v, const uint8_t *pixels, int width, int height,
                                           int stride, enum AVPixelFormat src_fmt, uint8_t *out_pkt, size_t out_cap,
                                           size_t *out_len, ccord_error *err)
{
  ccord_error_clear(err);
  if (out_len) *out_len = 0;
  if (!v || !pixels || !out_pkt || !out_len || width <= 0 || height <= 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "video encode: invalid argument");
  }
  int src_stride = stride > 0 ? stride : width * 4;
  if (src_stride < width * 4) return ccord_error_set(err, CCORD_ERR_INVALID, "video encode: stride too small");
  if (av_frame_make_writable(v->enc_frame) < 0) {
    return ccord_error_set(err, CCORD_ERR_IO, "video encode: frame not writable");
  }
  v->sws_to_yuv = sws_getCachedContext(v->sws_to_yuv, width, height, src_fmt, v->width, v->height,
                                       v->enc_ctx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!v->sws_to_yuv) return ccord_error_set(err, CCORD_ERR_NOMEM, "video encode: sws_getCachedContext failed");
  const uint8_t *src_slices[4] = {pixels, nullptr, nullptr, nullptr};
  int src_linesize[4] = {src_stride, 0, 0, 0};
  sws_scale(v->sws_to_yuv, src_slices, src_linesize, 0, height, v->enc_frame->data, v->enc_frame->linesize);

  // Only allocate/update last_rgba when a caller actually reads the preview; otherwise defer lazily.
  if (v->lazy->preview_wanted) {
    video_stream_update_preview(v, pixels, width, height, src_stride, src_fmt);
  } else {
    v->lazy->enc_preview_dirty = true;
  }

  v->enc_frame->pts = v->pts++;
  if (v->force_keyframe) {
    v->enc_frame->pict_type = AV_PICTURE_TYPE_I;
    v->force_keyframe = false;
  } else {
    v->enc_frame->pict_type = AV_PICTURE_TYPE_NONE;
  }
  av_packet_unref(v->pkt);
  if (avcodec_send_frame(v->enc_ctx, v->enc_frame) < 0) {
    return ccord_error_set(err, CCORD_ERR_IO, "video encode: avcodec_send_frame failed");
  }
  if (avcodec_receive_packet(v->enc_ctx, v->pkt) < 0 || v->pkt->size <= 0) {
    return ccord_error_set(err, CCORD_ERR_IO, "video encode: avcodec_receive_packet failed");
  }
  v->last_keyframe = (v->pkt->flags & AV_PKT_FLAG_KEY) != 0;
  size_t pkt_sz = (size_t)v->pkt->size;
  if (pkt_sz > out_cap) {
    av_packet_unref(v->pkt);
    return ccord_error_set(err, CCORD_ERR_RANGE, "video encode: output buffer too small");
  }
  memcpy(out_pkt, v->pkt->data, pkt_sz);
  *out_len = pkt_sz;
  av_packet_unref(v->pkt);
  return CCORD_OK;
}

ccord_status ccord_video_stream_encode_rgba(ccord_video_stream *v, const uint8_t *rgba, int width, int height,
                                            int stride, uint8_t *out_pkt, size_t out_cap, size_t *out_len,
                                            ccord_error *err)
{
  return encode_pixels_internal(v, rgba, width, height, stride, AV_PIX_FMT_RGBA, out_pkt, out_cap, out_len, err);
}

ccord_status ccord_video_stream_encode_bgra(ccord_video_stream *v, const uint8_t *bgra, int width, int height,
                                            int stride, uint8_t *out_pkt, size_t out_cap, size_t *out_len,
                                            ccord_error *err)
{
  return encode_pixels_internal(v, bgra, width, height, stride, AV_PIX_FMT_BGRA, out_pkt, out_cap, out_len, err);
}

ccord_status ccord_video_stream_decode_rgba(ccord_video_stream *v, const uint8_t *pkt, size_t pkt_len,
                                            uint8_t **out_rgba, int *out_w, int *out_h, ccord_error *err)
{
  ccord_error_clear(err);
  if (out_rgba) *out_rgba = nullptr;
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (!v || !pkt || pkt_len == 0 || pkt_len > (size_t)INT_MAX) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "video decode: invalid argument");
  }
  ccord_status dec_st = ensure_video_decoder(v, err);
  if (dec_st != CCORD_OK) return dec_st;
  av_packet_unref(v->pkt);
  if (av_new_packet(v->pkt, (int)pkt_len) < 0) {
    return ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: av_new_packet failed");
  }
  memcpy(v->pkt->data, pkt, pkt_len);
  int send_rc = avcodec_send_packet(v->dec_ctx, v->pkt);
  av_packet_unref(v->pkt);
  if (send_rc < 0) return ccord_error_set(err, CCORD_ERR_PARSE, "video decode: invalid bitstream packet");
  if (avcodec_receive_frame(v->dec_ctx, v->dec_frame) < 0) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "video decode: no frame produced");
  }
  int dw = v->dec_frame->width;
  int dh = v->dec_frame->height;
  if (dw <= 0 || dh <= 0 || dw > 3840 || dh > 2160) {
    av_frame_unref(v->dec_frame);
    return ccord_error_set(err, CCORD_ERR_PARSE, "video decode: invalid decoded dimensions");
  }
  size_t bytes = (size_t)dw * (size_t)dh * 4u;
  video_stream_lazy *lz = v->lazy;
  if (!video_stream_ensure_rgba_buf(lz, dw, dh)) {
    av_frame_unref(v->dec_frame);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: out of memory");
  }
  lz->sws_to_rgba = sws_getCachedContext(lz->sws_to_rgba, dw, dh, (enum AVPixelFormat)v->dec_frame->format, dw, dh,
                                         AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!lz->sws_to_rgba) {
    av_frame_unref(v->dec_frame);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: sws_getCachedContext failed");
  }
  uint8_t *dst_slices[4] = {lz->last_rgba, nullptr, nullptr, nullptr};
  int dst_linesize[4] = {dw * 4, 0, 0, 0};
  sws_scale(lz->sws_to_rgba, (const uint8_t *const *)v->dec_frame->data, v->dec_frame->linesize, 0, dh, dst_slices,
            dst_linesize);
  av_frame_unref(v->dec_frame);
  lz->enc_preview_dirty = false;
  if (out_rgba) {
    uint8_t *copy = ccord_malloc(bytes);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "video decode: out of memory");
    memcpy(copy, lz->last_rgba, bytes);
    *out_rgba = copy;
  }
  if (out_w) *out_w = dw;
  if (out_h) *out_h = dh;
  return CCORD_OK;
}

ccord_status ccord_video_stream_step_test_frame(ccord_video_stream *v, ccord_error *err)
{
  ccord_error_clear(err);
  if (!v) return ccord_error_set(err, CCORD_ERR_INVALID, "video: null stream");
  size_t rgba_bytes = (size_t)v->width * (size_t)v->height * 4u;
  size_t pkt_cap = rgba_bytes + 65536u;
  uint8_t *src = ccord_malloc(rgba_bytes);
  uint8_t *pkt = ccord_malloc(pkt_cap);
  if (!src || !pkt) {
    ccord_free(src);
    ccord_free(pkt);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "video: out of memory");
  }
  uint32_t step = v->synth_step++;
  for (int y = 0; y < v->height; y++) {
    for (int x = 0; x < v->width; x++) {
      uint8_t *px = &src[((size_t)y * (size_t)v->width + (size_t)x) * 4u];
      px[0] = (uint8_t)((x * 255) / v->width);
      px[1] = (uint8_t)((y * 255) / v->height);
      px[2] = (uint8_t)((step * 17u) & 0xffu);
      px[3] = 255u;
    }
  }
  size_t pkt_len = 0;
  ccord_status st = ccord_video_stream_encode_rgba(v, src, v->width, v->height, v->width * 4, pkt, pkt_cap, &pkt_len, err);
  if (st == CCORD_OK) {
    st = ccord_video_stream_decode_rgba(v, pkt, pkt_len, nullptr, nullptr, nullptr, err);
  }
  ccord_free(src);
  ccord_free(pkt);
  return st;
}

const uint8_t *ccord_video_stream_last_rgba(const ccord_video_stream *v, int *out_w, int *out_h)
{
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (!v || !v->lazy) return nullptr;
  video_stream_lazy *lz = v->lazy;
  lz->preview_wanted = true;
  if (lz->enc_preview_dirty && v->enc_frame && v->enc_frame->data[0]) {
    if (video_stream_ensure_rgba_buf(lz, v->width, v->height)) {
      lz->sws_to_rgba = sws_getCachedContext(lz->sws_to_rgba, v->width, v->height,
                                             (enum AVPixelFormat)v->enc_frame->format, v->width, v->height,
                                             AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (lz->sws_to_rgba) {
        uint8_t *dst_slices[4] = {lz->last_rgba, nullptr, nullptr, nullptr};
        int dst_linesize[4] = {v->width * 4, 0, 0, 0};
        sws_scale(lz->sws_to_rgba, (const uint8_t *const *)v->enc_frame->data, v->enc_frame->linesize, 0, v->height,
                  dst_slices, dst_linesize);
        lz->enc_preview_dirty = false;
      }
    }
  }
  if (!lz->last_rgba || lz->last_w <= 0 || lz->last_h <= 0) return nullptr;
  if (out_w) *out_w = lz->last_w;
  if (out_h) *out_h = lz->last_h;
  return lz->last_rgba;
}

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
  return v * window_density(p->window) / window_scale(p->window);
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
      refresh_sdl_safe_area(p);
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
          pending_text_clear(p);
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
      platform_drop_buf *db = drop_buf_ensure(p);
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
      copy_utf8_prefix(out->text, sizeof out->text, path, n);
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
  {
    float top = 0, right = 0, bottom = 0, left = 0, ime = 0;
    if (ccord_android_poll_insets(&top, &right, &bottom, &left, &ime)) {
      float s = p->window ? window_scale(p->window) : 1.0f;
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
  if (pending_text_pop(p, out)) return true;
  bool queued_hotkey = false;
  if (p->hotkey_mutex) SDL_LockMutex(p->hotkey_mutex);
  if (p->hotkey_count > 0) {
    memset(out, 0, sizeof *out);
    const char *id = p->hotkey_queue[p->hotkey_head];
    size_t len = strlen(id);
    p->hotkey_head = (p->hotkey_head + 1) % HOTKEY_QUEUE_CAP;
    p->hotkey_count--;
    out->type = CCORD_EV_HOTKEY;
    copy_utf8_prefix(out->text, sizeof out->text, id, len);
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
    copy_utf8_prefix(out->text, sizeof out->text, p->drop_buf->last, len);
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

void ccord_platform_set_auth_autofill(ccord_platform *p, const ccord_auth_autofill_state *st)
{
  if (!p || !st || p->headless) return;
#if defined(__ANDROID__)
  float s = p->window ? window_scale(p->window) : 1.0f;
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
