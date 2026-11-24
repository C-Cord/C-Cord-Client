// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

#include <ccord/platform/platform.h>
#include <ccord/platform/link_watch.h>
#include <SDL3/SDL.h>
#include <stdatomic.h>

#include "desktop/desktop.h"
#include "audio/devices_internal.h"

#define CCORD_PLATFORM_APP_ID "C-Cord"
#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 800
#define DROP_QUEUE_CAP 16u
#define DROP_PATH_MAX 1024u
#define HOTKEY_QUEUE_CAP 16u
#define HOTKEY_ID_MAX sizeof((ccord_input_event){0}.text)
#define BASE_TITLE_MAX 256u
#define PLATFORM_PENDING_TEXT_MAX 65536u
#define PLATFORM_FDW_MAX_FDS 48u

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
  atomic_bool pending_quit_ev;
  int wake_pipe[2];               // POSIX/Android self-pipe to wake background poll() immediately
  _Atomic int wake_wr_fd;         // mirror of wake_pipe[1] for thread-safe wakeups (-1 when closed)
  int wake_timer_fd;              // Linux/Android CLOCK_BOOTTIME timerfd so poll() never oversleeps across SoC naps
  Uint32 wake_sdl_event;          // SDL_RegisterEvents(1) internal wake event for SDL_WaitEventTimeout()
  atomic_bool wake_sdl_ready;     // true while wake_sdl_event may safely be pushed via SDL_PushEvent
  atomic_bool sdl_wake_pending;   // coalesces bursts of async wake_sdl_event pushes from D-Bus / worker threads
  bool non_input_activity;        // set when tray/notify/shortcut/link callbacks fired during a pump
#if !defined(_WIN32)
  SDL_Mutex *fdw_mtx;             // guards reusable on-demand fd-watcher thread for SDL_WaitEventTimeout()
  SDL_Condition *fdw_cond;
  SDL_Thread *fdw_thread;
  int fdw_cancel_pipe[2];
  bool fdw_started;
  bool fdw_stop;
  bool fdw_armed;
  bool fdw_active;
  uint64_t fdw_gen;
  int fdw_fds[PLATFORM_FDW_MAX_FDS];
  size_t fdw_nfds;
#endif
  SDL_ThreadID main_thread_id;    // thread owning ccord_platform_wait_idle()
  atomic_bool suppress_main_wake; // suppresses main-thread self-wakes during wait_idle poll-prep drain
  bool pending_safe_area_ev;
  bool pending_keyboard_ev;
  bool pending_focus_ev;          // deferred CCORD_IN_FOCUS synthesized when SDL focus flag recovers without an event
  bool pending_focus_val;
  bool android_insets_seen;       // Kotlin WindowInsets arrived; ignore SDL's systemGestures-bloated safe area
  ccord_safe_insets safe_insets;
  float keyboard_inset;
  ccord_form_factor form_factor;
  bool voice_service_active;
  bool keep_alive_active;
};

float platform_window_scale(SDL_Window *w);
float platform_window_density(SDL_Window *w);
void platform_pending_text_clear(ccord_platform *p);
bool platform_pending_text_pop(ccord_platform *p, ccord_input_event *out);
void platform_refresh_sdl_safe_area(ccord_platform *p);
void platform_copy_utf8_prefix(char *dst, size_t cap, const char *src, size_t len);
platform_drop_buf *platform_drop_buf_ensure(ccord_platform *p);
void platform_pump_link(ccord_platform *p);
void platform_wake(ccord_platform *p);
void platform_wake_sdl(ccord_platform *p);
void platform_fdw_destroy(ccord_platform *p);
