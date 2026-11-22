// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice close-background: close-to-background behaviour of the platform layer and main-loop gate.
// Drives a private headless platform (dummy video driver), since the tray and window state are platform-level.
#include <stdio.h>

#include <SDL3/SDL_hints.h>

#include <ccord/platform/platform.h>

#include "app_desktop.h"
#include "verify_ui_parity.h"

static int g_quit_actions;

static void on_tray(void *ud, ccord_tray_action action)
{
  (void)ud;
  if (action == CCORD_TRAY_QUIT) g_quit_actions++;
}

int ccord_verify_close_background_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  (void)ctx;
  int checks = 0;
#define CHECK(cond, name)                                      \
  do {                                                         \
    if (!(cond)) {                                             \
      printf("close-background: FAIL %s\n", name);             \
      ccord_platform_destroy(p);                               \
      return 1;                                                \
    }                                                          \
    checks++;                                                  \
  } while (0)
  ccord_error err;
  ccord_platform_config cfg = {.title = "C-Cord", .width = 640, .height = 400, .headless = true};
  ccord_platform *p = ccord_platform_create(&cfg, &err);
  if (!p) {
    puts("close-background: FAIL platform");
    return 1;
  }
  ccord_platform_set_tray_callback(p, on_tray, nullptr);
  CHECK(!SDL_GetHintBoolean(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, true), "sdl-no-quit-on-last-close");

  ccord_app_startup st;
  ccord_app_desktop_startup_defaults(&st);
  CHECK(st.close_to_tray, "default-on");

  // Pref ON + tray registered: close hides, no quit; tray Activate shows again.
  ccord_platform_set_close_to_tray(p, true);
  ccord_platform_set_tray_override(p, 1);
  CHECK(ccord_platform_window_visible(p) && ccord_app_desktop_should_draw(p), "initially-visible");
  CHECK(!ccord_platform_request_close(p), "close-no-quit");
  CHECK(!ccord_platform_window_visible(p), "close-hides");
  CHECK(!ccord_app_desktop_should_draw(p), "hidden-no-frame");
  ccord_platform_inject_tray_action(p, CCORD_TRAY_ACTIVATE);
  CHECK(ccord_platform_window_visible(p) && ccord_app_desktop_should_draw(p), "activate-shows");

  // Quit from the tray menu while hidden still reaches the app's quit handler.
  CHECK(!ccord_platform_request_close(p) && !ccord_platform_window_visible(p), "hide-again");
  ccord_platform_inject_tray_action(p, CCORD_TRAY_QUIT);
  CHECK(g_quit_actions == 1 && !ccord_platform_window_visible(p), "tray-quit-while-hidden");
  ccord_platform_inject_tray_action(p, CCORD_TRAY_ACTIVATE);

  // Pref ON, no tray: minimize, never hide.
  ccord_platform_set_tray_override(p, -1);
  CHECK(!ccord_platform_request_close(p), "notray-no-quit");
  CHECK(ccord_platform_window_visible(p) && ccord_platform_window_minimized(p), "notray-minimizes");
  CHECK(!ccord_app_desktop_should_draw(p), "minimized-no-frame");
  ccord_platform_inject_tray_action(p, CCORD_TRAY_ACTIVATE);
  CHECK(!ccord_platform_window_minimized(p) && ccord_app_desktop_should_draw(p), "restore-from-minimize");

  // Pref OFF: close quits; the change applies immediately.
  ccord_platform_set_close_to_tray(p, false);
  CHECK(ccord_platform_request_close(p), "pref-off-quits");
  CHECK(ccord_platform_window_visible(p), "pref-off-stays-visible");
  ccord_platform_set_close_to_tray(p, true);
  CHECK(!ccord_platform_request_close(p), "pref-on-applies-live");

  // Second launch ('show' IPC): the handler restores a minimized window (no tray) and raises it.
  CHECK(ccord_platform_window_visible(p) && ccord_platform_window_minimized(p), "show-precondition-minimized");
  ccord_app_desktop_handle_show(p);
  CHECK(ccord_platform_window_visible(p) && !ccord_platform_window_minimized(p) && ccord_app_desktop_should_draw(p),
        "show-restores-minimized");

  // 'show' on a window hidden to the tray while still minimized: it must come back restored, not just visible.
  ccord_platform_set_tray_override(p, -1);
  CHECK(!ccord_platform_request_close(p) && ccord_platform_window_minimized(p), "show-precondition-minimized-again");
  ccord_platform_set_tray_override(p, 1);
  CHECK(!ccord_platform_request_close(p) && !ccord_platform_window_visible(p) && ccord_platform_window_minimized(p),
        "show-precondition-hidden");
  ccord_app_desktop_handle_show(p);
  CHECK(ccord_platform_window_visible(p) && !ccord_platform_window_minimized(p) && ccord_app_desktop_should_draw(p),
        "show-shows-hidden");

  // 'show' on a visible, focused window keeps it shown (the tray Activate toggle would hide it).
  ccord_platform_set_window_focused(p, true);
  ccord_app_desktop_handle_show(p);
  CHECK(ccord_platform_window_visible(p) && !ccord_platform_window_minimized(p), "show-keeps-focused");

  // 'show' on a visible, unfocused window raises it and leaves it shown.
  ccord_platform_set_window_focused(p, false);
  ccord_app_desktop_handle_show(p);
  CHECK(ccord_platform_window_visible(p) && !ccord_platform_window_minimized(p), "show-raises-unfocused");

  ccord_platform_destroy(p);
  printf("close-background: PASS checks=%d\n", checks);
  return 0;
}
