// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Native C23 mini web runtime (QuickJS-ng + Lexbor + wasm3) rendered through ccord_render (SDL3).
// Designed to faithfully load and display interactive web widgets (such as CAPTCHA challenges)
// directly inside C-Cord with zero browser binary bundled and zero idle memory footprint.

#include <ccord/core/error.h>
#include <ccord/core/input.h>
#include <ccord/render/render.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ccord_web_view ccord_web_view;

// Called when the trusted top-level host document calls window.ccord.postMessage(payload).
// `kind` is "solved", "expired", "error", "ready", or "message".
typedef void (*ccord_web_message_cb)(void *ud, const char *kind, const char *payload);

typedef struct ccord_web_config {
  float viewport_w;             // initial logical width (0 -> 420)
  float viewport_h;             // initial logical height (0 -> 540)
  bool dark_theme;              // prefers-color-scheme: dark
  bool touch_enabled;           // expose touch device capabilities (maxTouchPoints, ontouchstart, coarse pointer)
  bool allow_network;           // true: allow HTTPS fetches via libcurl; false: offline-only
  bool allow_loopback_http;     // tests only: allow http://127.0.0.1 / http://localhost
  const char *user_agent;       // optional custom User-Agent (null -> honest C-Cord/desktop UA)
  const char *captcha_host;     // optional hCaptcha host domain (null -> "discord.com")
  ccord_color page_bg;          // optional theme background (a > 0 overrides default dark/light hex)
  ccord_color page_fg;          // optional theme text color (a > 0 overrides default dark/light hex)
  ccord_web_message_cb on_msg;  // optional trusted host bridge callback
  void *on_msg_ud;
} ccord_web_config;

// Snapshot of embedded CAPTCHA widget layout/visibility state for native adaptive sizing & loading UI.
typedef struct ccord_web_captcha_state {
  bool checkbox_ready;    // checkbox iframe has loaded and laid out DOM content
  bool challenge_open;    // challenge overlay iframe (#frame=challenge) is currently visible
  float challenge_w;      // laid-out width of visible challenge frame (0 if closed)
  float challenge_h;      // laid-out height of visible challenge frame (0 if closed)
} ccord_web_captcha_state;

[[nodiscard]] ccord_web_view *ccord_web_view_create(const ccord_web_config *cfg, ccord_error *err);
void ccord_web_view_destroy(ccord_web_view *v);

// Parses and executes an HTML document in the top-level browsing context under `base_url` (e.g. "https://discord.com/").
[[nodiscard]] ccord_status ccord_web_view_load_html(ccord_web_view *v, const char *html, size_t len,
                                                    const char *base_url, ccord_error *err);
// Loads a remote URL (HTTPS, or loopback HTTP when allow_loopback_http is enabled) into the top-level frame.
[[nodiscard]] ccord_status ccord_web_view_load_url(ccord_web_view *v, const char *url, ccord_error *err);
// Builds and loads the minimal hCaptcha Enterprise host HTML under https://discord.com/ with the given
// sitekey and optional rqdata, wiring hCaptcha callbacks to `on_msg` ("solved" / "expired" / "error" / "ready").
[[nodiscard]] ccord_status ccord_web_view_load_captcha(ccord_web_view *v, const char *service,
                                                       const char *sitekey, const char *rqdata,
                                                       bool dark_theme, ccord_error *err);

// Advances async HTTP fetches, timers, microtasks, postMessage queues, and animation frames.
void ccord_web_view_pump(ccord_web_view *v, uint64_t now_ms);

// Viewport, redraw & idle-wait helpers for integrating cleanly with C-Cord's event-driven main loop.
void ccord_web_view_set_viewport(ccord_web_view *v, float w, float h);
[[nodiscard]] bool ccord_web_view_needs_redraw(const ccord_web_view *v);
[[nodiscard]] uint32_t ccord_web_view_next_wait_ms(const ccord_web_view *v, uint64_t now_ms);

// Paints the laid-out DOM, canvases, images, text, and nested iframes inside `bounds` (logical px).
void ccord_web_view_draw(ccord_web_view *v, ccord_renderer *r, ccord_rect bounds);

// Routes mouse/pointer/wheel/keyboard/text events into the hit-tested frame/element with isTrusted=true.
// Returns true if the event fell within `bounds` or was consumed by a focused control.
bool ccord_web_view_handle_input(ccord_web_view *v, ccord_rect bounds, const ccord_input_event *ev);

// Diagnostic & unit-test helpers.
[[nodiscard]] bool ccord_web_view_eval_debug(ccord_web_view *v, const char *js, char *out, size_t cap);
// Returns recent console/runtime diagnostic lines (NUL-terminated, owned by `v`).
[[nodiscard]] const char *ccord_web_view_diagnostics(const ccord_web_view *v);
// Returns active pending HTTP fetch count (useful for test loops and loading indicators).
[[nodiscard]] size_t ccord_web_view_pending_fetches(const ccord_web_view *v);
// Inspects embedded CAPTCHA iframe readiness and challenge-popup visibility for native modal sizing.
void ccord_web_view_captcha_state(const ccord_web_view *v, ccord_web_captcha_state *out);
