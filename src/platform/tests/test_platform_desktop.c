// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Desktop integration tests: offline notification sink + fallbacks, dead-bus fallback, IME preedit
// translation, hotkey queue, and (when gio is available) live round-trips against a private
// dbus-daemon with mock org.freedesktop.Notifications / StatusNotifierWatcher / GlobalShortcuts
// portal services. Headless platforms never touch the real session bus.
#define _POSIX_C_SOURCE 200809L
#include <ccord/platform/platform.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/error.h>
#include <ccord/core/input.h>
#include <ccord/core/os.h>

#include "ccord_test.h"

#if CCORD_HAVE_PORTAL
#include <gio/gio.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static void drain(ccord_platform *p)
{
  ccord_input_event ev;
  while (ccord_platform_next_event(p, &ev, 0)) {
  }
}

// The platform does not expose its SDL_Window; read the title through SDL's window list.
static const char *window_title(void)
{
  static char buf[512];
  buf[0] = '\0';
  int n = 0;
  SDL_Window **ws = SDL_GetWindows(&n);
  if (ws && n > 0) {
    const char *t = SDL_GetWindowTitle(ws[0]);
    if (t) snprintf(buf, sizeof buf, "%s", t);
  }
  if (ws) SDL_free(ws);
  return buf;
}

// SDL borrows the text pointer until the event is translated; only pass static storage.
static void push_editing(const char *text, int32_t start, int32_t length)
{
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = SDL_EVENT_TEXT_EDITING;
  e.edit.text = text;
  e.edit.start = start;
  e.edit.length = length;
  (void)SDL_PushEvent(&e);
}

// Push a window event tagged with the platform's real window id (headless close-request simulation).
static void push_window_event(ccord_platform *p, Uint32 type)
{
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = type;
  e.window.windowID = SDL_GetWindowID((SDL_Window *)ccord_platform_sdl_window(p));
  (void)SDL_PushEvent(&e);
}

// Drains every pending input event; true if any of them was CCORD_IN_QUIT. SDL show/hide/resize
// chatter is drained alongside, so only the quit decision is asserted.
static bool drain_saw_quit(ccord_platform *p)
{
  ccord_input_event ev;
  bool quit = false;
  while (ccord_platform_next_event(p, &ev, 0)) {
    if (ev.type == CCORD_IN_QUIT) quit = true;
  }
  return quit;
}

static void test_window_lifecycle(void)
{
  ccord_error err;
  // start_minimized creates the window hidden and tracks window_visible=false even headless.
  ccord_platform_config cfg = {.title = "C-Cord", .width = 320, .height = 200, .headless = true, .start_minimized = true};
  ccord_platform *p = ccord_platform_create(&cfg, &err);
  CT_CHECK(p != nullptr);
  if (!p) return;
  // SDL3 must not synthesize SDL_EVENT_QUIT when the last window requests close, or close-to-tray exits immediately.
  CT_CHECK(!SDL_GetHintBoolean(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, true));
  drain(p);
  CT_CHECK(!ccord_platform_window_visible(p));
  CT_CHECK(!ccord_platform_window_focused(p));
  CT_CHECK(!ccord_platform_close_to_tray(p));   // default off
  ccord_platform_set_window_visible(p, true);
  CT_CHECK(ccord_platform_window_visible(p));
  ccord_platform_set_window_focused(p, true);
  CT_CHECK(ccord_platform_window_focused(p));
  ccord_platform_set_window_visible(p, false);
  CT_CHECK(!ccord_platform_window_visible(p));
  CT_CHECK(!ccord_platform_window_focused(p));
  // Offline tray/voice setters are safe no-ops (no bus), must not crash.
  ccord_platform_set_tray_callback(p, nullptr, nullptr);
  ccord_platform_set_voice_state(p, true, true);
  ccord_platform_set_voice_state(p, false, false);

  // close_to_tray=false: a window-close request still emits CCORD_IN_QUIT (legacy behavior).
  ccord_platform_set_window_visible(p, true);
  drain(p);
  push_window_event(p, SDL_EVENT_WINDOW_CLOSE_REQUESTED);
  CT_CHECK(drain_saw_quit(p));

  // close_to_tray=true: the close request hides the window, clears focused state, and emits CCORD_IN_FOCUS(false) without CCORD_IN_QUIT.
  ccord_platform_set_window_visible(p, true);
  ccord_platform_set_window_focused(p, true);
  drain(p);
  ccord_platform_set_close_to_tray(p, true);
  ccord_platform_set_tray_override(p, 1);   // headless has no tray: pretend one registered so close hides
  CT_CHECK(ccord_platform_close_to_tray(p));
  push_window_event(p, SDL_EVENT_WINDOW_CLOSE_REQUESTED);
  {
    ccord_input_event cev;
    bool saw_quit = false, saw_unfocus = false;
    while (ccord_platform_next_event(p, &cev, 0)) {
      if (cev.type == CCORD_IN_QUIT) saw_quit = true;
      if (cev.type == CCORD_IN_FOCUS && !cev.focused) saw_unfocus = true;
    }
    CT_CHECK(!saw_quit);
    CT_CHECK(saw_unfocus);
  }
  CT_CHECK(!ccord_platform_window_visible(p));
  CT_CHECK(!ccord_platform_window_focused(p));

  // Without a tray available, close_to_tray minimizes instead, still clearing focus and emitting CCORD_IN_FOCUS(false).
  ccord_platform_set_window_visible(p, true);
  ccord_platform_set_window_focused(p, true);
  ccord_platform_set_tray_override(p, -1);
  drain(p);
  push_window_event(p, SDL_EVENT_WINDOW_CLOSE_REQUESTED);
  {
    ccord_input_event cev;
    bool saw_quit = false, saw_unfocus = false;
    while (ccord_platform_next_event(p, &cev, 0)) {
      if (cev.type == CCORD_IN_QUIT) saw_quit = true;
      if (cev.type == CCORD_IN_FOCUS && !cev.focused) saw_unfocus = true;
    }
    CT_CHECK(!saw_quit);
    CT_CHECK(saw_unfocus);
  }
  CT_CHECK(ccord_platform_window_minimized(p));
  CT_CHECK(!ccord_platform_window_focused(p));
  ccord_platform_set_tray_override(p, 1);

  // Restore from the tray and close again: still hidden, still no quit.
  ccord_platform_set_window_visible(p, true);
  CT_CHECK(ccord_platform_window_visible(p));
  drain(p);
  push_window_event(p, SDL_EVENT_WINDOW_CLOSE_REQUESTED);
  CT_CHECK(!drain_saw_quit(p));
  CT_CHECK(!ccord_platform_window_visible(p));
  // SDL_EVENT_QUIT still quits even with close-to-tray enabled.
  SDL_Event q;
  memset(&q, 0, sizeof q);
  q.type = SDL_EVENT_QUIT;
  (void)SDL_PushEvent(&q);
  CT_CHECK(drain_saw_quit(p));
  ccord_platform_destroy(p);

  // Default config: the window starts visible.
  ccord_platform_config cfg2 = {.title = "C-Cord", .width = 320, .height = 200, .headless = true};
  p = ccord_platform_create(&cfg2, &err);
  CT_CHECK(p != nullptr);
  if (p) {
    CT_CHECK(ccord_platform_window_visible(p));
    ccord_platform_destroy(p);
  }
}

static ccord_notify_target g_click_target;
static int g_clicks;
static void on_notify_click(void *ud, const ccord_notify_target *t)
{
  (void)ud;
  g_click_target = *t;
  g_clicks++;
}

// A message notification carries the default action; its id maps to the target until the server closes it.
static void test_notify_click_offline(ccord_platform *p)
{
  ccord_platform_set_notify_click_callback(p, on_notify_click, nullptr);
  g_clicks = 0;
  ccord_notify_target t = {.guild_id = 11, .channel_id = 22, .message_id = 33}, got = {0};
  CT_CHECK(ccord_platform_notify(p, "plain", "no action"));
  CT_CHECK(strcmp(ccord_platform_last_notify_actions(p), "") == 0);
  CT_CHECK(ccord_platform_notify_map_count(p) == 0);
  CT_CHECK(ccord_platform_notify_message(p, "Ada in #general", "hi", &t));
  uint32_t id = ccord_platform_last_notify_id(p);
  CT_CHECK(id != 0);
  CT_CHECK(strcmp(ccord_platform_last_notify_actions(p), "default") == 0);
  CT_CHECK(ccord_platform_notify_target_for(p, id, &got) && got.guild_id == 11 && got.channel_id == 22 &&
           got.message_id == 33);
  ccord_platform_inject_notify_action(p, id, "default");
  CT_CHECK(g_clicks == 1 && g_click_target.channel_id == 22 && g_click_target.message_id == 33);
  ccord_platform_inject_notify_action(p, id, "snooze");          // not the default action
  ccord_platform_inject_notify_action(p, id + 50, "default");    // unknown id
  CT_CHECK(g_clicks == 1);
  ccord_platform_inject_notify_closed(p, id);
  CT_CHECK(ccord_platform_notify_map_count(p) == 0 && !ccord_platform_notify_target_for(p, id, &got));
  ccord_platform_inject_notify_action(p, id, "default");
  CT_CHECK(g_clicks == 1);
  // Bounded: the table never grows past its cap and evicts the oldest.
  uint32_t first = 0, last = 0;
  for (int i = 0; i < 100; i++) {
    t.message_id = 100u + (uint64_t)i;
    CT_CHECK(ccord_platform_notify_message(p, "t", "b", &t));
    last = ccord_platform_last_notify_id(p);
    if (!first) first = last;
  }
  CT_CHECK(ccord_platform_notify_map_count(p) == 32);
  CT_CHECK(!ccord_platform_notify_target_for(p, first, &got));
  CT_CHECK(ccord_platform_notify_target_for(p, last, &got) && got.message_id == 199);

  // Close-by-channel dismisses matching entries (all or up to message_id) and leaves other channels intact.
  CT_CHECK(ccord_platform_close_channel_notifications(p, 22, 0) == 32);
  CT_CHECK(ccord_platform_notify_map_count(p) == 0);
  ccord_notify_target ta = {.guild_id = 11, .channel_id = 22, .message_id = 30};
  ccord_notify_target tb = {.guild_id = 11, .channel_id = 22, .message_id = 50};
  ccord_notify_target tc = {.guild_id = 11, .channel_id = 99, .message_id = 40};
  CT_CHECK(ccord_platform_notify_message(p, "a", "m1", &ta));
  uint32_t ida = ccord_platform_last_notify_id(p);
  CT_CHECK(ccord_platform_notify_message(p, "b", "m2", &tb));
  uint32_t idb = ccord_platform_last_notify_id(p);
  CT_CHECK(ccord_platform_notify_message(p, "c", "m3", &tc));
  uint32_t idc = ccord_platform_last_notify_id(p);
  CT_CHECK(ccord_platform_notify_map_count(p) == 3);
  CT_CHECK(ccord_platform_close_channel_notifications(p, 22, 40) == 1);
  CT_CHECK(!ccord_platform_notify_target_for(p, ida, &got));
  CT_CHECK(ccord_platform_notify_target_for(p, idb, &got) && got.message_id == 50);
  CT_CHECK(ccord_platform_notify_target_for(p, idc, &got) && got.channel_id == 99);
  CT_CHECK(ccord_platform_close_channel_notifications(p, 22, 0) == 1);
  CT_CHECK(!ccord_platform_notify_target_for(p, idb, &got));
  CT_CHECK(ccord_platform_notify_map_count(p) == 1);
  // Activation token injection before ActionInvoked keeps click routing working and is safe to consume on show.
  ccord_platform_inject_notify_activation_token(p, idc, "plasma_offline_TIME12345");
  ccord_platform_inject_notify_action(p, idc, "default");
  CT_CHECK(g_clicks == 2 && g_click_target.channel_id == 99 && g_click_target.message_id == 40);
  CT_CHECK(ccord_platform_close_channel_notifications(p, 99, 0) == 1);
  CT_CHECK(ccord_platform_notify_map_count(p) == 0);
  ccord_platform_set_notify_click_callback(p, nullptr, nullptr);
}

static void test_offline(void)
{
  ccord_error err;
  ccord_platform_config cfg = {.title = "C-Cord", .width = 320, .height = 200, .headless = true};
  ccord_platform *p = ccord_platform_create(&cfg, &err);
  CT_CHECK(p != nullptr);
  if (!p) return;
  drain(p);

  // Honest capabilities: headless never probes a bus, everything reports unavailable.
  CT_CHECK(!ccord_platform_notifications_available(p));
  CT_CHECK(!ccord_platform_tray_available(p));
  CT_CHECK(!ccord_platform_global_shortcuts_available(p));
  CT_CHECK(strcmp(ccord_platform_last_notify_title(p), "") == 0);
  CT_CHECK(strcmp(ccord_platform_last_notify_body(p), "") == 0);

  // Focus-gated notify; the sink records regardless of transport.
  CT_CHECK(!ccord_platform_notify(p, "t", "b"));
  CT_CHECK(ccord_platform_notify_count(p) == 0);
  ccord_platform_set_window_focused(p, false);
  CT_CHECK(ccord_platform_notify(p, "New mention", "Hello"));
  CT_CHECK(ccord_platform_notify_count(p) == 1);
  CT_CHECK(strcmp(ccord_platform_last_notify_title(p), "New mention") == 0);
  CT_CHECK(strcmp(ccord_platform_last_notify_body(p), "Hello") == 0);
  CT_CHECK(ccord_platform_notify(p, nullptr, nullptr));
  CT_CHECK(ccord_platform_notify_count(p) == 2);
  CT_CHECK(strcmp(ccord_platform_last_notify_title(p), "") == 0);
  CT_CHECK(strcmp(ccord_platform_last_notify_body(p), "") == 0);
  // Invalid UTF-8 is rejected without touching sink or count.
  CT_CHECK(!ccord_platform_notify(p, "\xff\xfe", "ok"));
  CT_CHECK(!ccord_platform_notify(p, "ok", "\xff"));
  CT_CHECK(ccord_platform_notify_count(p) == 2);
  CT_CHECK(strcmp(ccord_platform_last_notify_title(p), "") == 0);
  test_notify_click_offline(p);
  ccord_platform_set_window_focused(p, true);

  // Window-title badge fallback (notification/tray count) and its interplay with set_title.
  uint32_t unread = 123, mentions = 123;
  ccord_platform_unread_badge(p, &unread, &mentions);
  CT_CHECK(unread == 0 && mentions == 0);
  ccord_platform_set_unread_badge(p, 5, 0);
  CT_CHECK(strcmp(window_title(), "C-Cord") == 0);   // plain unreads with tray_count == 0 keep base title
  ccord_platform_unread_badge(p, &unread, &mentions);
  CT_CHECK(unread == 5 && mentions == 0);
  ccord_platform_set_title(p, "Chat");
  CT_CHECK(strcmp(window_title(), "Chat") == 0);
  ccord_platform_set_unread_badge(p, 0, 3);
  CT_CHECK(strcmp(window_title(), "Chat (3)") == 0);
  ccord_platform_unread_badge(p, &unread, &mentions);
  CT_CHECK(unread == 0 && mentions == 3);
  ccord_platform_set_badge_counts(p, 5, 2, 7);
  CT_CHECK(strcmp(window_title(), "Chat (7)") == 0);
  ccord_platform_set_badge_counts(p, 5, 0, 0);
  CT_CHECK(strcmp(window_title(), "Chat") == 0);
  ccord_platform_set_unread_badge(p, UINT32_MAX, UINT32_MAX);
  CT_CHECK(strcmp(window_title(), "Chat (4294967295)") == 0);
  ccord_platform_set_unread_badge(p, 0, 0);
  CT_CHECK(strcmp(window_title(), "Chat") == 0);
  ccord_platform_set_title(p, "\xff");   // invalid UTF-8 keeps the previous title
  CT_CHECK(strcmp(window_title(), "Chat") == 0);

  // Minimize and hide clear focused state and emit CCORD_IN_FOCUS (focused = false).
  ccord_platform_set_window_focused(p, true);
  CT_CHECK(ccord_platform_window_focused(p));
  drain(p);
  push_window_event(p, SDL_EVENT_WINDOW_MINIMIZED);
  ccord_input_event focus_ev;
  bool saw_unfocus = false;
  while (ccord_platform_next_event(p, &focus_ev, 0)) {
    if (focus_ev.type == CCORD_IN_FOCUS && !focus_ev.focused) saw_unfocus = true;
  }
  CT_CHECK(saw_unfocus);
  CT_CHECK(!ccord_platform_window_focused(p));

  ccord_platform_set_window_focused(p, true);
  drain(p);
  push_window_event(p, SDL_EVENT_WINDOW_HIDDEN);
  saw_unfocus = false;
  while (ccord_platform_next_event(p, &focus_ev, 0)) {
    if (focus_ev.type == CCORD_IN_FOCUS && !focus_ev.focused) saw_unfocus = true;
  }
  CT_CHECK(saw_unfocus);
  CT_CHECK(!ccord_platform_window_focused(p));
  ccord_platform_set_window_focused(p, true);

  // Window fullscreen toggle + resize/move system cursor kinds (safe even under dummy video driver).
  CT_CHECK(!ccord_platform_window_fullscreen(p));
  CT_CHECK(ccord_platform_set_fullscreen(p, true));
  CT_CHECK(ccord_platform_window_fullscreen(p));
  CT_CHECK(ccord_platform_set_fullscreen(p, false));
  CT_CHECK(!ccord_platform_window_fullscreen(p));
  CT_CHECK(!ccord_platform_set_fullscreen(nullptr, true));
  for (int ck = 0; ck < (int)CCORD_CURSOR_COUNT; ck++) {
    ccord_platform_set_cursor(p, (ccord_cursor_kind)ck);
  }
  ccord_platform_set_cursor(p, CCORD_CURSOR_DEFAULT);

  // Binding shortcuts offline fails honestly; invalid definitions fail everywhere.
  ccord_global_shortcut defs[1] = {{.id = "toggle-mute", .description = "Toggle mute", .preferred_trigger = "CTRL+SHIFT+M"}};
  CT_CHECK(!ccord_platform_bind_global_shortcuts(p, defs, 1));
  CT_CHECK(!ccord_platform_bind_global_shortcuts(p, defs, 0));
  CT_CHECK(!ccord_platform_bind_global_shortcuts(nullptr, defs, 1));
  ccord_global_shortcut bad[1] = {{.id = nullptr, .description = nullptr, .preferred_trigger = nullptr}};
  CT_CHECK(!ccord_platform_bind_global_shortcuts(p, bad, 1));

  // Hotkey injection: validation, round-trip, overflow (oldest dropped), priority over drops.
  CT_CHECK(ccord_platform_inject_hotkey(p, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_platform_inject_hotkey(p, "", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_platform_inject_hotkey(p, "01234567890123456789012345678901", &err) == CCORD_ERR_INVALID);   // 32 bytes: no room for NUL
  CT_CHECK(ccord_platform_inject_hotkey(p, "\xff", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_platform_inject_hotkey(p, "toggle-mute", &err) == CCORD_OK);
  ccord_input_event ev;
  CT_CHECK(ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(ev.type == CCORD_EV_HOTKEY);
  CT_CHECK(strcmp(ev.text, "toggle-mute") == 0);
  CT_CHECK(ev.ime_start == 0 && ev.ime_length == 0);
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));

  for (int i = 1; i <= 17; i++) {
    char id[8];
    snprintf(id, sizeof id, "hk%02d", i);
    CT_CHECK(ccord_platform_inject_hotkey(p, id, &err) == CCORD_OK);
  }
  int delivered = 0;
  char first_id[sizeof ev.text] = "", last_id[sizeof ev.text] = "";
  while (ccord_platform_next_event(p, &ev, 0)) {
    CT_CHECK(ev.type == CCORD_EV_HOTKEY);
    if (delivered == 0) snprintf(first_id, sizeof first_id, "%s", ev.text);
    snprintf(last_id, sizeof last_id, "%s", ev.text);
    delivered++;
  }
  CT_CHECK(delivered == 16);
  CT_CHECK(strcmp(first_id, "hk02") == 0);   // hk01 was dropped on overflow
  CT_CHECK(strcmp(last_id, "hk17") == 0);

  CT_CHECK(ccord_platform_inject_hotkey(p, "hk-first", &err) == CCORD_OK);
  CT_CHECK(ccord_platform_inject_drop_file(p, "/tmp/ccord_desktop_test.png", &err) == CCORD_OK);
  CT_CHECK(ccord_platform_next_event(p, &ev, 0) && ev.type == CCORD_EV_HOTKEY);
  CT_CHECK(strcmp(ev.text, "hk-first") == 0);
  CT_CHECK(ccord_platform_next_event(p, &ev, 0) && ev.type == CCORD_EV_DROP_FILE);

  // SDL3 file drag-and-drop hover lifecycle: BEGIN -> POSITION -> COMPLETE.
  {
    SDL_Event sd = {0};
    sd.type = SDL_EVENT_DROP_BEGIN;
    SDL_PushEvent(&sd);
    CT_CHECK(ccord_platform_next_event(p, &ev, 0) && ev.type == CCORD_EV_DROP_BEGIN);

    memset(&sd, 0, sizeof sd);
    sd.type = SDL_EVENT_DROP_POSITION;
    sd.drop.x = 320.0f;
    sd.drop.y = 240.0f;
    SDL_PushEvent(&sd);
    CT_CHECK(ccord_platform_next_event(p, &ev, 0) && ev.type == CCORD_EV_DROP_POSITION);
    CT_CHECK(ev.x > 0.0f && ev.y > 0.0f);

    memset(&sd, 0, sizeof sd);
    sd.type = SDL_EVENT_DROP_COMPLETE;
    SDL_PushEvent(&sd);
    CT_CHECK(ccord_platform_next_event(p, &ev, 0) && ev.type == CCORD_EV_DROP_COMPLETE);
  }

  // IME preedit translation: codepoint cursor/selection bounds, UTF-8 and size validation.
  push_editing("にほん", 1, 2);   // 3 codepoints: start+length == 3 is inside
  CT_CHECK(ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(ev.type == CCORD_EV_IME_PREEDIT);
  CT_CHECK(strcmp(ev.text, "にほん") == 0);
  CT_CHECK(ev.ime_start == 1 && ev.ime_length == 2);
  push_editing("にほん", 2, 2);   // 4 > 3 codepoints: dropped
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  push_editing("にほん", 4, 0);   // start alone out of range: dropped
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  push_editing("にほん", -1, -1);   // SDL "unset" normalizes to 0
  CT_CHECK(ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(ev.type == CCORD_EV_IME_PREEDIT && ev.ime_start == 0 && ev.ime_length == 0);
  push_editing("", -1, -1);   // empty preedit clears the composition
  CT_CHECK(ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(ev.type == CCORD_EV_IME_PREEDIT && ev.text[0] == '\0');
  CT_CHECK(ev.ime_start == 0 && ev.ime_length == 0);
  push_editing("", 1, 0);   // non-zero cursor on empty text: dropped
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  push_editing(nullptr, -1, -1);
  CT_CHECK(ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(ev.type == CCORD_EV_IME_PREEDIT && ev.text[0] == '\0');
  push_editing("\xff", 0, 0);   // invalid UTF-8: dropped
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  static char big[33];
  memset(big, 'a', 32);
  big[32] = '\0';
  push_editing(big, 0, 0);   // 32 bytes does not fit text[32] with NUL: dropped
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  push_editing(big + 1, 31, 0);   // 31 bytes, cursor at the end: allowed
  CT_CHECK(ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(ev.type == CCORD_EV_IME_PREEDIT && ev.ime_start == 31 && ev.ime_length == 0);
  push_editing(big + 1, 30, 2);   // 32 > 31 codepoints: dropped
  CT_CHECK(!ccord_platform_next_event(p, &ev, 0));
  push_editing("aé", 1, 1);   // cursor counts codepoints, not bytes
  CT_CHECK(ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(ev.type == CCORD_EV_IME_PREEDIT && ev.ime_start == 1 && ev.ime_length == 1);
  CT_CHECK(strcmp(ev.text, "aé") == 0);

  // Plain text input still works alongside preedit.
  SDL_Event te;
  memset(&te, 0, sizeof te);
  te.type = SDL_EVENT_TEXT_INPUT;
  te.text.text = "hi";
  (void)SDL_PushEvent(&te);
  CT_CHECK(ccord_platform_next_event(p, &ev, 0));
  CT_CHECK(ev.type == CCORD_IN_TEXT && strcmp(ev.text, "hi") == 0);

  ccord_platform_destroy(p);
}

static void test_dead_bus(void)
{
  // A non-headless platform with an unreachable bus address must fall back to offline mode.
  const char *orig = getenv("DBUS_SESSION_BUS_ADDRESS");
  char saved[512];
  saved[0] = '\0';
  if (orig) snprintf(saved, sizeof saved, "%s", orig);
  setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-ccord-desktop-test", 1);
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  ccord_error err;
  ccord_platform_config cfg = {.title = "C-Cord", .width = 320, .height = 200, .headless = false};
  ccord_platform *p = ccord_platform_create(&cfg, &err);
  CT_CHECK(p != nullptr);
  if (p) {
    drain(p);
    CT_CHECK(!ccord_platform_notifications_available(p));
    CT_CHECK(!ccord_platform_tray_available(p));
    CT_CHECK(!ccord_platform_global_shortcuts_available(p));
    ccord_platform_set_window_focused(p, false);
    CT_CHECK(ccord_platform_notify(p, "Offline", "Still recorded"));
    CT_CHECK(ccord_platform_notify_count(p) == 1);
    CT_CHECK(strcmp(ccord_platform_last_notify_title(p), "Offline") == 0);
    CT_CHECK(strcmp(ccord_platform_last_notify_body(p), "Still recorded") == 0);
    ccord_global_shortcut defs[1] = {{.id = "toggle-mute", .description = nullptr, .preferred_trigger = nullptr}};
    CT_CHECK(!ccord_platform_bind_global_shortcuts(p, defs, 1));
    ccord_platform_set_unread_badge(p, 3, 1);
    CT_CHECK(strcmp(window_title(), "C-Cord (1)") == 0);
    CT_CHECK(ccord_platform_inject_hotkey(p, "toggle-mute", &err) == CCORD_OK);
    ccord_input_event ev;
    CT_CHECK(ccord_platform_next_event(p, &ev, 0) && ev.type == CCORD_EV_HOTKEY);
    ccord_platform_destroy(p);
  }
  if (saved[0]) setenv("DBUS_SESSION_BUS_ADDRESS", saved, 1);
  else unsetenv("DBUS_SESSION_BUS_ADDRESS");
}

#if CCORD_HAVE_PORTAL

// ---- Live bus: private dbus-daemon + mock services ----------------------------------------

#define MOCK_PORTAL_PATH "/org/freedesktop/portal/desktop"
#define MOCK_SESSION_PATH "/org/freedesktop/portal/desktop/session/ccord_mock/s1"
#define MOCK_REQ_SESSION "/org/freedesktop/portal/desktop/request/ccord_mock/session"
#define MOCK_REQ_BIND "/org/freedesktop/portal/desktop/request/ccord_mock/bind"
#define SNI_IFACE "org.kde.StatusNotifierItem"

typedef struct {
  GDBusConnection *conn;
  guint names_acquired;
  guint notify_count;
  guint32 notify_next_id;    // 0 = fixed id 42 (legacy assertions); >0 = incrementing ids
  char notify_app[64];
  char notify_icon[64];
  char notify_desktop_entry[64];
  char notify_summary[256];
  char notify_body[256];
  char notify_actions[64];   // the action list of the last Notify: keys and labels joined by ','
  guint close_calls;
  guint32 closed_ids[16];
  guint tray_registrations;
  char tray_service[128];
  guint new_status_signals;
  guint new_icon_signals;
  guint new_tooltip_signals;
  guint create_session_calls;
  guint bind_calls;
  char bound_ids[8][64];
  guint bound_count;
  char request_path[128];
  bool session_response;
  guint response_idle;
  guint response_emissions;
  guint response_bind_target;   // session response: stop re-emitting once bind_calls reaches this
  guint owner_ids[3];
  guint reg_ids[3];
  guint portal_props_reg;
  guint sni_signal_sub;
} mock_state;

static mock_state g_mock;

static const char k_mock_notify_xml[] =
    "<node><interface name='org.freedesktop.Notifications'>"
    "  <method name='Notify'>"
    "    <arg type='s' name='app' direction='in'/><arg type='u' name='replaces' direction='in'/>"
    "    <arg type='s' name='icon' direction='in'/><arg type='s' name='summary' direction='in'/>"
    "    <arg type='s' name='body' direction='in'/><arg type='as' name='actions' direction='in'/>"
    "    <arg type='a{sv}' name='hints' direction='in'/><arg type='i' name='timeout' direction='in'/>"
    "    <arg type='u' name='id' direction='out'/>"
    "  </method>"
    "  <method name='CloseNotification'>"
    "    <arg type='u' name='id' direction='in'/>"
    "  </method>"
    "</interface></node>";

static const char k_mock_watcher_xml[] =
    "<node><interface name='org.kde.StatusNotifierWatcher'>"
    "  <method name='RegisterStatusNotifierItem'><arg type='s' name='service' direction='in'/></method>"
    "</interface></node>";

static const char k_mock_portal_xml[] =
    "<node>"
    "  <interface name='org.freedesktop.portal.GlobalShortcuts'>"
    "    <method name='CreateSession'>"
    "      <arg type='a{sv}' name='options' direction='in'/><arg type='o' name='request' direction='out'/>"
    "    </method>"
    "    <method name='BindShortcuts'>"
    "      <arg type='o' name='session' direction='in'/><arg type='a(sa{sv})' name='shortcuts' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/><arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='o' name='request' direction='out'/>"
    "    </method>"
    "  </interface>"
    "  <interface name='org.freedesktop.DBus.Properties'>"
    "    <method name='Get'>"
    "      <arg type='s' name='iface' direction='in'/><arg type='s' name='prop' direction='in'/>"
    "      <arg type='v' name='value' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

// The client subscribes to Response only after processing the method reply, and its AddMatch
// still has to reach dbus-daemon — a single emission races that round-trip and can be lost.
// Re-emit every 20ms until the follow-up call proves delivery (session response) or a bounded
// number of attempts is reached (bind response). The client unsubscribes after the first
// Response, so extra emissions are ignored.
static gboolean mock_emit_response(gpointer user_data)
{
  mock_state *m = user_data;
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  if (m->session_response) {
    g_variant_builder_add(&b, "{sv}", "session_handle", g_variant_new_object_path(MOCK_SESSION_PATH));
  }
  GError *gerr = nullptr;
  if (!g_dbus_connection_emit_signal(m->conn, nullptr, m->request_path, "org.freedesktop.portal.Request",
                                     "Response", g_variant_new("(ua{sv})", (guint32)0, &b), &gerr)) {
    if (gerr) g_error_free(gerr);
  }
  m->response_emissions++;
  if (m->bind_calls >= m->response_bind_target || m->response_emissions >= 100) {
    m->response_idle = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void mock_schedule_response(mock_state *m)
{
  if (m->response_idle) g_source_remove(m->response_idle);
  m->response_emissions = 0;
  m->response_idle = g_timeout_add(20, mock_emit_response, m);
}

// Activated / SessionClosed are re-emitted on a short timer until the test removes the source,
// so a subscription whose AddMatch is still in flight cannot miss them. The client unsubscribes
// after the first delivery, so repeats are ignored.
static guint s_activated_src, s_closed_src, s_deactivated_src;
static guint s_activated_emissions, s_closed_emissions, s_deactivated_emissions;
static const char *s_activated_id = "toggle-mute";

static gboolean mock_emit_activated(gpointer user_data)
{
  mock_state *m = user_data;
  GVariantBuilder opts;
  g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
  GError *gerr = nullptr;
  if (!g_dbus_connection_emit_signal(m->conn, nullptr, MOCK_PORTAL_PATH,
                                     "org.freedesktop.portal.GlobalShortcuts", "Activated",
                                     g_variant_new("(osta{sv})", MOCK_SESSION_PATH, s_activated_id, (guint64)123, &opts),
                                     &gerr)) {
    if (gerr) g_error_free(gerr);
  }
  if (++s_activated_emissions >= 250) {
    s_activated_src = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean mock_emit_deactivated(gpointer user_data)
{
  mock_state *m = user_data;
  GVariantBuilder opts;
  g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
  GError *gerr = nullptr;
  if (!g_dbus_connection_emit_signal(m->conn, nullptr, MOCK_PORTAL_PATH,
                                     "org.freedesktop.portal.GlobalShortcuts", "Deactivated",
                                     g_variant_new("(osta{sv})", MOCK_SESSION_PATH, s_activated_id, (guint64)124, &opts),
                                     &gerr)) {
    if (gerr) g_error_free(gerr);
  }
  if (++s_deactivated_emissions >= 250) {
    s_deactivated_src = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean mock_emit_closed(gpointer user_data)
{
  mock_state *m = user_data;
  GError *gerr = nullptr;
  if (!g_dbus_connection_emit_signal(m->conn, nullptr, MOCK_PORTAL_PATH,
                                     "org.freedesktop.portal.GlobalShortcuts", "SessionClosed",
                                     g_variant_new("(o)", MOCK_SESSION_PATH), &gerr)) {
    if (gerr) g_error_free(gerr);
  }
  if (++s_closed_emissions >= 250) {
    s_closed_src = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void mock_notify_call(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                             const gchar *interface_name, const gchar *method_name, GVariant *parameters,
                             GDBusMethodInvocation *invocation, gpointer user_data)
{
  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;
  mock_state *m = user_data;
  if (strcmp(method_name, "CloseNotification") == 0) {
    guint32 nid = 0;
    g_variant_get(parameters, "(u)", &nid);
    if (m->close_calls < (guint)(sizeof m->closed_ids / sizeof m->closed_ids[0])) {
      m->closed_ids[m->close_calls] = nid;
    }
    m->close_calls++;
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (strcmp(method_name, "Notify") != 0) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "no %s", method_name);
    return;
  }
  const gchar *app = nullptr, *icon = nullptr, *summary = nullptr, *body = nullptr;
  guint32 replaces = 0;
  gint32 timeout = 0;
  GVariant *actions = nullptr, *hints = nullptr;
  g_variant_get(parameters, "(&su&s&s&s@as@a{sv}i)", &app, &replaces, &icon, &summary, &body, &actions, &hints, &timeout);
  (void)replaces;
  (void)timeout;
  m->notify_count++;
  snprintf(m->notify_app, sizeof m->notify_app, "%s", app ? app : "");
  snprintf(m->notify_icon, sizeof m->notify_icon, "%s", icon ? icon : "");
  snprintf(m->notify_summary, sizeof m->notify_summary, "%s", summary ? summary : "");
  snprintf(m->notify_body, sizeof m->notify_body, "%s", body ? body : "");
  m->notify_desktop_entry[0] = '\0';
  m->notify_actions[0] = '\0';
  if (actions) {
    GVariantIter it;
    const gchar *a = nullptr;
    g_variant_iter_init(&it, actions);
    while (g_variant_iter_next(&it, "&s", &a)) {
      size_t used = strlen(m->notify_actions);
      snprintf(m->notify_actions + used, sizeof m->notify_actions - used, "%s%s", used ? "," : "", a);
    }
    g_variant_unref(actions);
  }
  if (hints) {
    const gchar *de = nullptr;
    if (g_variant_lookup(hints, "desktop-entry", "&s", &de) && de) {
      snprintf(m->notify_desktop_entry, sizeof m->notify_desktop_entry, "%s", de);
    }
    g_variant_unref(hints);
  }
  guint32 out_id = m->notify_next_id ? m->notify_next_id++ : (guint32)42;
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", out_id));
}

static void mock_watcher_call(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                              const gchar *interface_name, const gchar *method_name, GVariant *parameters,
                              GDBusMethodInvocation *invocation, gpointer user_data)
{
  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;
  mock_state *m = user_data;
  if (strcmp(method_name, "RegisterStatusNotifierItem") != 0) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "no %s", method_name);
    return;
  }
  const gchar *service = nullptr;
  g_variant_get(parameters, "(&s)", &service);
  m->tray_registrations++;
  snprintf(m->tray_service, sizeof m->tray_service, "%s", service ? service : "");
  g_dbus_method_invocation_return_value(invocation, nullptr);
}

static void mock_portal_call(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                             const gchar *interface_name, const gchar *method_name, GVariant *parameters,
                             GDBusMethodInvocation *invocation, gpointer user_data)
{
  (void)connection;
  (void)sender;
  (void)object_path;
  mock_state *m = user_data;
  if (strcmp(interface_name, "org.freedesktop.DBus.Properties") == 0 && strcmp(method_name, "Get") == 0) {
    const gchar *iface = nullptr, *prop = nullptr;
    g_variant_get(parameters, "(&s&s)", &iface, &prop);
    if (strcmp(iface, "org.freedesktop.portal.GlobalShortcuts") == 0 && strcmp(prop, "version") == 0) {
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", g_variant_new_uint32(1)));
    } else {
      g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "no %s.%s",
                                            iface, prop);
    }
    return;
  }
  if (strcmp(method_name, "CreateSession") == 0) {
    GVariant *opts = nullptr;
    g_variant_get(parameters, "(@a{sv})", &opts);
    if (opts) g_variant_unref(opts);
    m->create_session_calls++;
    snprintf(m->request_path, sizeof m->request_path, "%s", MOCK_REQ_SESSION);
    m->session_response = true;
    m->response_bind_target = m->bind_calls + 1;
    mock_schedule_response(m);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", MOCK_REQ_SESSION));
    return;
  }
  if (strcmp(method_name, "BindShortcuts") == 0) {
    const gchar *session = nullptr, *parent = nullptr;
    GVariant *shortcuts = nullptr, *opts = nullptr;
    g_variant_get(parameters, "(&o@a(sa{sv})&s@a{sv})", &session, &shortcuts, &parent, &opts);
    (void)session;
    (void)parent;
    m->bind_calls++;
    m->bound_count = 0;
    if (shortcuts) {
      GVariantIter it;
      g_variant_iter_init(&it, shortcuts);
      const gchar *id = nullptr;
      GVariant *sopts = nullptr;
      while (g_variant_iter_next(&it, "(&s@a{sv})", &id, &sopts)) {
        if (m->bound_count < 8) snprintf(m->bound_ids[m->bound_count++], sizeof m->bound_ids[0], "%s", id);
        if (sopts) g_variant_unref(sopts);
      }
      g_variant_unref(shortcuts);
    }
    if (opts) g_variant_unref(opts);
    snprintf(m->request_path, sizeof m->request_path, "%s", MOCK_REQ_BIND);
    m->session_response = false;
    m->response_bind_target = G_MAXUINT;   // no follow-up call; the emission cap stops it
    mock_schedule_response(m);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", MOCK_REQ_BIND));
    return;
  }
  g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "no %s", method_name);
}

static const GDBusInterfaceVTable k_mock_notify_vtable = {.method_call = mock_notify_call};
static const GDBusInterfaceVTable k_mock_watcher_vtable = {.method_call = mock_watcher_call};
static const GDBusInterfaceVTable k_mock_portal_vtable = {.method_call = mock_portal_call};

static void mock_sni_signal(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path,
                            const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
                            gpointer user_data)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)parameters;
  mock_state *m = user_data;
  if (strcmp(signal_name, "NewStatus") == 0) m->new_status_signals++;
  if (strcmp(signal_name, "NewIcon") == 0) m->new_icon_signals++;
  if (strcmp(signal_name, "NewToolTip") == 0) m->new_tooltip_signals++;
}

static void mock_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  (void)connection;
  (void)name;
  mock_state *m = user_data;
  m->names_acquired++;
}

static bool mock_start(const char *addr)
{
  GError *gerr = nullptr;
  g_mock.conn = g_dbus_connection_new_for_address_sync(addr,
                                                       G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                           G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                       nullptr, nullptr, &gerr);
  if (gerr) {
    fprintf(stderr, "mock bus connect failed: %s\n", gerr->message);
    g_error_free(gerr);
  }
  if (!g_mock.conn) return false;
  GDBusNodeInfo *nodes[3];
  const char *xmls[3] = {k_mock_notify_xml, k_mock_watcher_xml, k_mock_portal_xml};
  const char *paths[3] = {"/org/freedesktop/Notifications", "/StatusNotifierWatcher", MOCK_PORTAL_PATH};
  const GDBusInterfaceVTable *vtables[3] = {&k_mock_notify_vtable, &k_mock_watcher_vtable, &k_mock_portal_vtable};
  const char *names[3] = {"org.freedesktop.Notifications", "org.kde.StatusNotifierWatcher", "org.freedesktop.portal.Desktop"};
  for (int i = 0; i < 3; i++) {
    gerr = nullptr;
    nodes[i] = g_dbus_node_info_new_for_xml(xmls[i], &gerr);
    if (gerr) g_error_free(gerr);
    if (!nodes[i]) return false;
    gerr = nullptr;
    g_mock.reg_ids[i] = g_dbus_connection_register_object(g_mock.conn, paths[i], nodes[i]->interfaces[0], vtables[i],
                                                          &g_mock, nullptr, &gerr);
    if (gerr) g_error_free(gerr);
    // The capability probe reads GlobalShortcuts.version via org.freedesktop.DBus.Properties;
    // register that second interface of the portal node too (GDBus does not synthesize it).
    if (i == 2 && nodes[i]->interfaces[1]) {
      gerr = nullptr;
      g_mock.portal_props_reg = g_dbus_connection_register_object(g_mock.conn, paths[i], nodes[i]->interfaces[1],
                                                                  vtables[i], &g_mock, nullptr, &gerr);
      if (gerr) g_error_free(gerr);
    }
    g_dbus_node_info_unref(nodes[i]);
    if (!g_mock.reg_ids[i] || (i == 2 && !g_mock.portal_props_reg)) return false;
    g_mock.owner_ids[i] = g_bus_own_name_on_connection(g_mock.conn, names[i], G_BUS_NAME_OWNER_FLAGS_NONE,
                                                       mock_name_acquired, nullptr, &g_mock, nullptr);
  }
  g_mock.sni_signal_sub = g_dbus_connection_signal_subscribe(g_mock.conn, nullptr, SNI_IFACE, nullptr,
                                                             "/StatusNotifierItem", nullptr,
                                                             G_DBUS_SIGNAL_FLAGS_NONE, mock_sni_signal, &g_mock,
                                                             nullptr);
  for (int i = 0; i < 5000 && g_mock.names_acquired < 3; i++) {
    g_main_context_iteration(nullptr, false);
    g_usleep(1000);
  }
  return g_mock.names_acquired == 3;
}

static void mock_stop(void)
{
  if (!g_mock.conn) return;
  if (g_mock.response_idle) g_source_remove(g_mock.response_idle);
  if (s_activated_src) g_source_remove(s_activated_src);
  if (s_closed_src) g_source_remove(s_closed_src);
  s_activated_src = s_closed_src = 0;
  if (g_mock.sni_signal_sub) g_dbus_connection_signal_unsubscribe(g_mock.conn, g_mock.sni_signal_sub);
  for (int i = 0; i < 3; i++) {
    if (g_mock.owner_ids[i]) g_bus_unown_name(g_mock.owner_ids[i]);
    if (g_mock.reg_ids[i]) g_dbus_connection_unregister_object(g_mock.conn, g_mock.reg_ids[i]);
  }
  if (g_mock.portal_props_reg) g_dbus_connection_unregister_object(g_mock.conn, g_mock.portal_props_reg);
  g_object_unref(g_mock.conn);
  memset(&g_mock, 0, sizeof g_mock);
}

static GPid spawn_bus(char *addr, size_t cap)
{
  // Private bus with service activation disabled (no <servicedir>): a probe on an empty bus
  // must never be able to spawn real desktop services such as xdg-desktop-portal.
  GError *gerr = nullptr;
  gchar *cfg_path = nullptr;
  gint cfgfd = g_file_open_tmp("ccord-dbus-XXXXXX", &cfg_path, &gerr);
  if (cfgfd < 0) {
    if (gerr) {
      fprintf(stderr, "dbus config tmpfile failed: %s\n", gerr->message);
      g_error_free(gerr);
    }
    return 0;
  }
  gchar *cfg = g_strdup_printf("<busconfig>\n"
                               "  <type>session</type>\n"
                               "  <listen>unix:tmpdir=%s</listen>\n"
                               "  <auth>EXTERNAL</auth>\n"
                               "  <policy context=\"default\">\n"
                               "    <allow user=\"*\"/>\n"
                               "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
                               "    <allow eavesdrop=\"true\"/>\n"
                               "    <allow own=\"*\"/>\n"
                               "  </policy>\n"
                               "</busconfig>\n",
                               g_get_tmp_dir());
  size_t cfg_len = strlen(cfg);
  ssize_t written = write(cfgfd, cfg, cfg_len);
  close(cfgfd);
  g_free(cfg);
  if (written != (ssize_t)cfg_len) {
    unlink(cfg_path);
    g_free(cfg_path);
    return 0;
  }
  static char arg0[] = "dbus-daemon";
  static char arg1[] = "--config-file";
  static char arg2[] = "--nofork";
  static char arg3[] = "--nopidfile";
  static char arg4[] = "--print-address";
  gchar *argv[] = {arg0, arg1, cfg_path, arg2, arg3, arg4, nullptr};
  gint outfd = -1;
  GPid pid = 0;
  if (!g_spawn_async_with_pipes(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, nullptr,
                                nullptr, &pid, nullptr, &outfd, nullptr, &gerr)) {
    if (gerr) {
      fprintf(stderr, "dbus-daemon spawn failed: %s\n", gerr->message);
      g_error_free(gerr);
    }
    unlink(cfg_path);
    g_free(cfg_path);
    return 0;
  }
  char buf[512];
  size_t used = 0;
  while (used < sizeof buf - 1) {
    char c = 0;
    ssize_t r = read(outfd, &c, 1);
    if (r <= 0 || c == '\n') break;
    buf[used++] = c;
  }
  buf[used] = '\0';
  close(outfd);
  // The daemon parsed the config before printing its address; the file is no longer needed.
  unlink(cfg_path);
  g_free(cfg_path);
  if (used == 0) {
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    g_spawn_close_pid(pid);
    return 0;
  }
  snprintf(addr, cap, "%s", buf);
  return pid;
}

// Pump the platform event loop until cond() or the timeout elapses. All DBus dispatch goes through
// ccord_platform_next_event -> ccord_desktop_pump (the production path); the test never iterates
// the main context itself, so a broken pump cannot be masked here.
static ccord_input_event s_hotkeys[32];
static size_t s_hotkey_count;

static bool wait_for(ccord_platform *p, bool (*cond)(void), int timeout_ms)
{
  for (int i = 0; i < timeout_ms; i++) {
    ccord_input_event ev;
    while (ccord_platform_next_event(p, &ev, 0)) {
      if (ev.type == CCORD_EV_HOTKEY && s_hotkey_count < 32) s_hotkeys[s_hotkey_count++] = ev;
    }
    if (cond()) return true;
    g_usleep(1000);
  }
  return cond();
}

static ccord_platform *g_live_p;   // lets wait conditions observe client-side state too

static bool cond_never(void) { return false; }
static bool cond_notify_cap(void) { return g_live_p && ccord_platform_notifications_available(g_live_p); }
static bool cond_shortcuts_cap(void) { return g_live_p && ccord_platform_global_shortcuts_available(g_live_p); }
static bool cond_tray(void)
{
  // The mock counter increments before the registration reply reaches the client; require the
  // client-side capability as well so tray_available is settled when wait_for() returns.
  return g_mock.tray_registrations > 0 && g_live_p && ccord_platform_tray_available(g_live_p);
}
static bool cond_notify(void) { return g_mock.notify_count > 0; }
static bool cond_notify2(void) { return g_mock.notify_count > 1; }
static bool cond_id42(void) { return g_live_p && ccord_platform_notify_target_for(g_live_p, 42, nullptr); }
static bool cond_click(void) { return g_clicks > 0; }
static bool cond_unmapped(void) { return g_live_p && !ccord_platform_notify_target_for(g_live_p, 42, nullptr); }
// The notification server's signals, emitted from the mock's own connection.
static void mock_emit_notify_signal(const char *name, GVariant *params)
{
  GError *gerr = nullptr;
  if (!g_dbus_connection_emit_signal(g_mock.conn, nullptr, "/org/freedesktop/Notifications",
                                     "org.freedesktop.Notifications", name, params, &gerr) && gerr) {
    fprintf(stderr, "emit %s failed: %s\n", name, gerr->message);
    g_error_free(gerr);
  }
}
static bool cond_badge_signals(void) { return g_mock.new_icon_signals > 0 && g_mock.new_tooltip_signals > 0; }
static bool cond_bind(void) { return g_mock.bind_calls > 0; }
static bool cond_rebind(void) { return g_mock.create_session_calls >= 2 && g_mock.bind_calls >= 2; }
static bool cond_ptt_held(void) { return g_live_p && ccord_platform_global_ptt_held(g_live_p); }
static bool cond_ptt_released(void) { return g_live_p && !ccord_platform_global_ptt_held(g_live_p); }
static bool cond_hotkey(void)
{
  for (size_t i = 0; i < s_hotkey_count; i++) {
    if (strcmp(s_hotkeys[i].text, "toggle-mute") == 0) return true;
  }
  return false;
}

// SNI property reads must be asynchronous: a synchronous call from this (main) thread to the
// client's in-process SNI object would deadlock, because call_sync's private wait loop never
// dispatches the client's incoming messages. Fire the Get, then pump until the reply lands.
static GVariant *s_sni_prop;   // inner value of the last completed Get
static bool s_sni_prop_done;

static void sni_prop_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)user_data;
  GError *gerr = nullptr;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &gerr);
  if (gerr) g_error_free(gerr);
  if (ret) {
    g_variant_get(ret, "(v)", &s_sni_prop);
    g_variant_unref(ret);
  }
  s_sni_prop_done = true;
}

static bool cond_sni_prop(void) { return s_sni_prop_done; }

// Returns the property's inner value (caller unrefs), or nullptr on timeout/error.
static GVariant *sni_read_prop(ccord_platform *p, const char *service, const char *prop)
{
  s_sni_prop = nullptr;
  s_sni_prop_done = false;
  g_dbus_connection_call(g_mock.conn, service, "/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Get",
                         g_variant_new("(ss)", SNI_IFACE, prop), G_VARIANT_TYPE("(v)"),
                         G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, sni_prop_done, nullptr);
  if (!wait_for(p, cond_sni_prop, 5000)) return nullptr;
  return s_sni_prop;
}

static bool sni_get_string(ccord_platform *p, const char *service, const char *prop, char *out, size_t cap)
{
  GVariant *v = sni_read_prop(p, service, prop);
  if (!v) return false;
  bool ok = g_variant_is_of_type(v, G_VARIANT_TYPE_STRING);
  if (ok) snprintf(out, cap, "%s", g_variant_get_string(v, nullptr));
  g_variant_unref(v);
  return ok;
}

static bool sni_get_tooltip(ccord_platform *p, const char *service, char *out, size_t cap)
{
  GVariant *v = sni_read_prop(p, service, "ToolTip");
  if (!v) return false;
  bool ok = g_variant_is_of_type(v, G_VARIANT_TYPE("(sa(iiay)ss)"));
  if (ok) {
    const gchar *icon = nullptr, *title = nullptr, *desc = nullptr;
    GVariant *arr = nullptr;
    g_variant_get(v, "(&s@a(iiay)&s&s)", &icon, &arr, &title, &desc);
    (void)icon;
    (void)desc;
    snprintf(out, cap, "%s", title);
    if (arr) g_variant_unref(arr);
  }
  g_variant_unref(v);
  return ok;
}

// ---- Tray menu / SNI click round-trips ----------------------------------------------------
// Like SNI property reads, method calls into the client's in-process objects must be async:
// fire the call, then pump the platform loop until the reply lands (a sync call would deadlock).
#define TEST_MENU_IFACE "com.canonical.dbusmenu"

static GVariant *s_call_ret;
static bool s_call_done;

static void bus_call_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)user_data;
  GError *gerr = nullptr;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &gerr);
  if (gerr) {
    fprintf(stderr, "tray bus call failed: %s\n", gerr->message);
    g_error_free(gerr);
  }
  s_call_ret = ret;
  s_call_done = true;
}

static bool cond_call_done(void) { return s_call_done; }

// Returns the reply tuple (caller unrefs), or nullptr on timeout/error. `params` is consumed.
static GVariant *bus_call(ccord_platform *p, const char *path, const char *iface, const char *method,
                          GVariant *params, const GVariantType *reply_type)
{
  s_call_ret = nullptr;
  s_call_done = false;
  g_dbus_connection_call(g_mock.conn, g_mock.tray_service, path, iface, method, params, reply_type,
                         G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, bus_call_done, nullptr);
  if (!wait_for(p, cond_call_done, 5000)) return nullptr;
  return s_call_ret;
}

static ccord_tray_action s_tray_actions[32];
static size_t s_tray_count;
static size_t s_tray_expected;

static void test_tray_cb(void *ud, ccord_tray_action action)
{
  (void)ud;
  if (s_tray_count < 32) s_tray_actions[s_tray_count++] = action;
}

static bool cond_tray_count(void) { return s_tray_count >= s_tray_expected; }

static void send_menu_click(ccord_platform *p, gint32 id)
{
  GVariant *r = bus_call(p, "/MenuBar", TEST_MENU_IFACE, "Event",
                         g_variant_new("(isvu)", id, "clicked", g_variant_new_string(""), (guint32)0), nullptr);
  if (r) g_variant_unref(r);
}

static void send_sni_click(ccord_platform *p, const char *method)
{
  GVariant *r = bus_call(p, "/StatusNotifierItem", SNI_IFACE, method, g_variant_new("(ii)", (gint32)0, (gint32)0),
                         nullptr);
  if (r) g_variant_unref(r);
}

// The action lands while bus_call pumps the reply, so snapshot the queue index BEFORE the click.
static bool menu_click_expect(ccord_platform *p, gint32 id, ccord_tray_action a)
{
  size_t idx = s_tray_count;
  s_tray_expected = idx + 1;
  send_menu_click(p, id);
  if (!wait_for(p, cond_tray_count, 5000)) return false;
  return idx < s_tray_count && s_tray_actions[idx] == a;
}

static bool sni_click_expect(ccord_platform *p, const char *method, ccord_tray_action a)
{
  size_t idx = s_tray_count;
  s_tray_expected = idx + 1;
  send_sni_click(p, method);
  if (!wait_for(p, cond_tray_count, 5000)) return false;
  return idx < s_tray_count && s_tray_actions[idx] == a;
}

// GetLayout(0,-1,[]) must return the four items in order with synced Mute/Deafen check states.
static bool check_menu_layout(ccord_platform *p, int expect_mute, int expect_deafen)
{
  GVariant *empty_names = g_variant_new_array(G_VARIANT_TYPE_STRING, nullptr, 0);
  GVariant *ret = bus_call(p, "/MenuBar", TEST_MENU_IFACE, "GetLayout",
                           g_variant_new("(ii@as)", (gint32)0, (gint32)-1, empty_names),
                           G_VARIANT_TYPE("(u(ia{sv}av))"));
  if (!ret) return false;
  guint32 revision = 0;
  GVariant *layout = nullptr;
  g_variant_get(ret, "(u@(ia{sv}av))", &revision, &layout);
  (void)revision;
  gint32 root_id = -1;
  GVariant *root_props = nullptr, *children = nullptr;
  g_variant_get(layout, "(i@a{sv}@av)", &root_id, &root_props, &children);
  bool ok = root_id == 0;
  static const char *want_labels[4] = {"Open C-Cord", "Mute Microphone", "Deafen Audio", "Quit C-Cord"};
  gsize n = g_variant_n_children(children);
  ok = ok && n == 4;
  for (gsize i = 0; i < n && i < 4; i++) {
    GVariant *child_v = g_variant_get_child_value(children, i);
    GVariant *item = g_variant_get_variant(child_v);
    gint32 id = 0;
    GVariant *props = nullptr, *sub = nullptr;
    g_variant_get(item, "(i@a{sv}@av)", &id, &props, &sub);
    const gchar *label = nullptr;
    g_variant_lookup(props, "label", "&s", &label);
    ok = ok && id == (gint32)(i + 1) && label && strcmp(label, want_labels[i]) == 0;
    if (id == 2 || id == 3) {
      const gchar *ttype = nullptr;
      gint32 tstate = -1;
      g_variant_lookup(props, "toggle-type", "&s", &ttype);
      g_variant_lookup(props, "toggle-state", "i", &tstate);
      ok = ok && ttype && strcmp(ttype, "checkmark") == 0;
      ok = ok && tstate == (id == 2 ? expect_mute : expect_deafen);
    }
    g_variant_unref(props);
    g_variant_unref(sub);
    g_variant_unref(item);
    g_variant_unref(child_v);
  }
  g_variant_unref(root_props);
  g_variant_unref(children);
  g_variant_unref(layout);
  g_variant_unref(ret);
  return ok;
}

// Exercises the DBusMenu object and SNI clicks end-to-end. Called from test_live with the tray up.
static void test_tray_menu_live(ccord_platform *p)
{
  s_tray_count = 0;
  ccord_platform_set_tray_callback(p, test_tray_cb, nullptr);

  // SNI advertises the real menu path; ItemIsMenu stays false so left clicks send Activate.
  GVariant *menu_v = sni_read_prop(p, g_mock.tray_service, "Menu");
  CT_CHECK(menu_v && g_variant_is_of_type(menu_v, G_VARIANT_TYPE_OBJECT_PATH));
  if (menu_v) {
    CT_CHECK(strcmp(g_variant_get_string(menu_v, nullptr), "/MenuBar") == 0);
    g_variant_unref(menu_v);
  }
  GVariant *iim = sni_read_prop(p, g_mock.tray_service, "ItemIsMenu");
  CT_CHECK(iim && g_variant_is_of_type(iim, G_VARIANT_TYPE_BOOLEAN));
  if (iim) {
    CT_CHECK(!g_variant_get_boolean(iim));
    g_variant_unref(iim);
  }

  // Initial layout: four items, both checkmarks off.
  CT_CHECK(check_menu_layout(p, 0, 0));

  // GetProperty returns a single property value.
  GVariant *gp = bus_call(p, "/MenuBar", TEST_MENU_IFACE, "GetProperty", g_variant_new("(is)", (gint32)4, "label"),
                          G_VARIANT_TYPE("(v)"));
  CT_CHECK(gp != nullptr);
  if (gp) {
    GVariant *inner = nullptr;
    g_variant_get(gp, "(v)", &inner);
    CT_CHECK(inner && g_variant_is_of_type(inner, G_VARIANT_TYPE_STRING));
    if (inner) {
      CT_CHECK(strcmp(g_variant_get_string(inner, nullptr), "Quit C-Cord") == 0);
      g_variant_unref(inner);
    }
    g_variant_unref(gp);
  }

  // AboutToShow reports no pending update.
  GVariant *at = bus_call(p, "/MenuBar", TEST_MENU_IFACE, "AboutToShow", g_variant_new("(i)", (gint32)0),
                          G_VARIANT_TYPE("(b)"));
  CT_CHECK(at != nullptr);
  if (at) {
    gboolean need = TRUE;
    g_variant_get(at, "(b)", &need);
    CT_CHECK(!need);
    g_variant_unref(at);
  }

  // Voice state syncs the checkmarks and the tray icon overlay.
  ccord_platform_set_voice_state(p, true, false);
  CT_CHECK(check_menu_layout(p, 1, 0));
  char overlay[64];
  CT_CHECK(sni_get_string(p, g_mock.tray_service, "OverlayIconName", overlay, sizeof overlay));
  CT_CHECK(strcmp(overlay, "audio-input-microphone-muted") == 0);
  ccord_platform_set_voice_state(p, false, true);
  CT_CHECK(check_menu_layout(p, 0, 1));
  CT_CHECK(sni_get_string(p, g_mock.tray_service, "OverlayIconName", overlay, sizeof overlay));
  CT_CHECK(strcmp(overlay, "audio-volume-muted") == 0);

  // GetGroupProperties for the two checkable items reflects the current state (mute=0, deafen=1).
  gint32 idarr[2] = {2, 3};
  GVariant *ids = g_variant_new_fixed_array(G_VARIANT_TYPE_INT32, idarr, 2, sizeof(gint32));
  GVariant *empty_names = g_variant_new_array(G_VARIANT_TYPE_STRING, nullptr, 0);
  GVariant *gg = bus_call(p, "/MenuBar", TEST_MENU_IFACE, "GetGroupProperties", g_variant_new("(@ai@as)", ids, empty_names),
                          G_VARIANT_TYPE("(a(ia{sv}))"));
  CT_CHECK(gg != nullptr);
  if (gg) {
    GVariant *arr = nullptr;
    g_variant_get(gg, "(@a(ia{sv}))", &arr);
    CT_CHECK(g_variant_n_children(arr) == 2);
    for (gsize i = 0; i < g_variant_n_children(arr); i++) {
      GVariant *entry = g_variant_get_child_value(arr, i);
      gint32 id = 0;
      GVariant *props = nullptr;
      g_variant_get(entry, "(i@a{sv})", &id, &props);
      gint32 tstate = -1;
      g_variant_lookup(props, "toggle-state", "i", &tstate);
      CT_CHECK(tstate == (id == 2 ? 0 : 1));
      g_variant_unref(props);
      g_variant_unref(entry);
    }
    g_variant_unref(arr);
    g_variant_unref(gg);
  }
  ccord_platform_set_voice_state(p, false, false);
  CT_CHECK(check_menu_layout(p, 0, 0));

  // Menu item clicks emit the matching tray actions; ACTIVATE also toggles window visibility.
  CT_CHECK(ccord_platform_window_visible(p));
  CT_CHECK(menu_click_expect(p, 1, CCORD_TRAY_ACTIVATE));
  CT_CHECK(!ccord_platform_window_visible(p));
  CT_CHECK(menu_click_expect(p, 2, CCORD_TRAY_TOGGLE_MUTE));
  CT_CHECK(menu_click_expect(p, 3, CCORD_TRAY_TOGGLE_DEAFEN));
  CT_CHECK(menu_click_expect(p, 4, CCORD_TRAY_QUIT));

  // SNI Activate toggles the window back; SecondaryActivate is a mute toggle.
  CT_CHECK(sni_click_expect(p, "Activate", CCORD_TRAY_ACTIVATE));
  CT_CHECK(ccord_platform_window_visible(p));
  CT_CHECK(sni_click_expect(p, "SecondaryActivate", CCORD_TRAY_TOGGLE_MUTE));

  ccord_platform_set_tray_callback(p, nullptr, nullptr);
}

static void test_empty_bus(void)
{
  // A reachable bus with no desktop services: capabilities must stay honestly false (a bus
  // connection alone is not a capability), while the sink and title-badge fallback keep working.
  char addr[512];
  GPid pid = spawn_bus(addr, sizeof addr);
  if (!pid) {
    fprintf(stderr, "test_platform_desktop: dbus-daemon unavailable; empty bus checks skipped\n");
    return;
  }
  const char *orig = getenv("DBUS_SESSION_BUS_ADDRESS");
  char saved[512];
  saved[0] = '\0';
  if (orig) snprintf(saved, sizeof saved, "%s", orig);
  setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  ccord_error err;
  ccord_platform_config cfg = {.title = "C-Cord", .width = 320, .height = 200, .headless = false};
  ccord_platform *p = ccord_platform_create(&cfg, &err);
  CT_CHECK(p != nullptr);
  if (p) {
    drain(p);
    wait_for(p, cond_never, 300);   // let the async capability probes settle (they must fail)
    CT_CHECK(!ccord_platform_notifications_available(p));
    CT_CHECK(!ccord_platform_tray_available(p));
    CT_CHECK(!ccord_platform_global_shortcuts_available(p));
    ccord_platform_set_window_focused(p, false);
    CT_CHECK(ccord_platform_notify(p, "No server", "Still recorded"));
    CT_CHECK(ccord_platform_notify_count(p) == 1);
    CT_CHECK(strcmp(ccord_platform_last_notify_title(p), "No server") == 0);
    ccord_global_shortcut defs[1] = {{.id = "toggle-mute", .description = nullptr, .preferred_trigger = nullptr}};
    CT_CHECK(!ccord_platform_bind_global_shortcuts(p, defs, 1));
    ccord_platform_set_unread_badge(p, 4, 2);
    CT_CHECK(strcmp(window_title(), "C-Cord (2)") == 0);
    ccord_platform_destroy(p);
  }
  if (saved[0]) setenv("DBUS_SESSION_BUS_ADDRESS", saved, 1);
  else unsetenv("DBUS_SESSION_BUS_ADDRESS");
  kill(pid, SIGTERM);
  waitpid(pid, nullptr, 0);
  g_spawn_close_pid(pid);
}

static bool cond_notify_unavailable(void) { return g_live_p && !ccord_platform_notifications_available(g_live_p); }
static bool cond_map3(void) { return g_live_p && ccord_platform_notify_map_count(g_live_p) == 3; }
static bool cond_close2(void) { return g_mock.close_calls >= 2; }
static bool cond_close3(void) { return g_mock.close_calls >= 3; }

static void test_late_notify_daemon(void)
{
  // If the notification daemon starts after C-Cord (common at desktop login or after a daemon restart),
  // notifications_available must transition from false -> true and deliver notifications, then return to false if it vanishes.
  char addr[512];
  GPid pid = spawn_bus(addr, sizeof addr);
  if (!pid) {
    fprintf(stderr, "test_platform_desktop: dbus-daemon unavailable; late daemon checks skipped\n");
    return;
  }
  const char *orig = getenv("DBUS_SESSION_BUS_ADDRESS");
  char saved[512];
  saved[0] = '\0';
  if (orig) snprintf(saved, sizeof saved, "%s", orig);
  setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  ccord_error err;
  ccord_platform_config cfg = {.title = "C-Cord", .width = 320, .height = 200, .headless = false};
  ccord_platform *p = ccord_platform_create(&cfg, &err);
  CT_CHECK(p != nullptr);
  if (p) {
    g_live_p = p;
    drain(p);
    wait_for(p, cond_never, 300);
    CT_CHECK(!ccord_platform_notifications_available(p));

    CT_CHECK(mock_start(addr));
    if (g_mock.names_acquired == 3) {
      CT_CHECK(wait_for(p, cond_notify_cap, 5000));
      CT_CHECK(ccord_platform_notifications_available(p));

      ccord_platform_set_window_focused(p, false);
      CT_CHECK(ccord_platform_notify(p, "Late daemon", "Delivered after startup"));
      CT_CHECK(wait_for(p, cond_notify, 5000));
      CT_CHECK(g_mock.notify_count == 1);
      CT_CHECK(strcmp(g_mock.notify_summary, "Late daemon") == 0);
      CT_CHECK(strcmp(g_mock.notify_body, "Delivered after startup") == 0);

      // Daemon vanishes: capability drops back to false until it returns.
      g_bus_unown_name(g_mock.owner_ids[0]);
      g_mock.owner_ids[0] = 0;
      CT_CHECK(wait_for(p, cond_notify_unavailable, 5000));
      CT_CHECK(!ccord_platform_notifications_available(p));
    }
    ccord_platform_destroy(p);
    g_live_p = nullptr;
  }
  mock_stop();
  if (saved[0]) setenv("DBUS_SESSION_BUS_ADDRESS", saved, 1);
  else unsetenv("DBUS_SESSION_BUS_ADDRESS");
  kill(pid, SIGTERM);
  waitpid(pid, nullptr, 0);
  g_spawn_close_pid(pid);
}

static void test_live(void)
{
  char addr[512];
  GPid pid = spawn_bus(addr, sizeof addr);
  if (!pid) {
    fprintf(stderr, "test_platform_desktop: dbus-daemon unavailable; live bus checks skipped\n");
    return;
  }
  const char *orig = getenv("DBUS_SESSION_BUS_ADDRESS");
  char saved[512];
  saved[0] = '\0';
  if (orig) snprintf(saved, sizeof saved, "%s", orig);
  setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
  CT_CHECK(mock_start(addr));
  if (g_mock.names_acquired == 3) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    ccord_error err;
    ccord_platform_config cfg = {.title = "C-Cord", .width = 320, .height = 200, .headless = false};
    ccord_platform *p = ccord_platform_create(&cfg, &err);
    CT_CHECK(p != nullptr);
    if (p) {
      g_live_p = p;
      drain(p);
      // Capability probes are async; wait for the mock's replies to settle them.
      CT_CHECK(wait_for(p, cond_notify_cap, 5000));
      CT_CHECK(wait_for(p, cond_shortcuts_cap, 5000));
      // Tray registration happens once the watcher appears (async).
      CT_CHECK(wait_for(p, cond_tray, 5000));
      CT_CHECK(ccord_platform_tray_available(p));
      CT_CHECK(g_mock.tray_registrations == 1);
      CT_CHECK(g_mock.tray_service[0] == ':');

      // Notify round-trip: the mock server sees app id, icon, desktop-entry hint, summary and body.
      ccord_platform_set_window_focused(p, false);
      CT_CHECK(ccord_platform_notify(p, "Ping", "Body text"));
      CT_CHECK(wait_for(p, cond_notify, 5000));
      CT_CHECK(g_mock.notify_count == 1);
      CT_CHECK(strcmp(g_mock.notify_app, "C-Cord") == 0);
      CT_CHECK(strcmp(g_mock.notify_icon, "C-Cord") == 0);
      CT_CHECK(strcmp(g_mock.notify_desktop_entry, "C-Cord") == 0);
      CT_CHECK(strcmp(g_mock.notify_summary, "Ping") == 0);
      CT_CHECK(strcmp(g_mock.notify_body, "Body text") == 0);
      CT_CHECK(strcmp(ccord_platform_last_notify_title(p), "Ping") == 0);
      CT_CHECK(g_mock.notify_actions[0] == '\0');   // a plain notification has no click action

      // Click routing: the mock server answers Notify with id 42, emits ActivationToken + ActionInvoked / NotificationClosed.
      g_clicks = 0;
      ccord_platform_set_notify_click_callback(p, on_notify_click, nullptr);
      ccord_notify_target tgt = {.guild_id = 5, .channel_id = 6, .message_id = 7};
      CT_CHECK(ccord_platform_notify_message(p, "Ada in #general", "hello", &tgt));
      CT_CHECK(wait_for(p, cond_notify2, 5000));
      CT_CHECK(strcmp(g_mock.notify_actions, "default,Open") == 0);
      CT_CHECK(wait_for(p, cond_id42, 5000));
      ccord_notify_target got = {0};
      CT_CHECK(ccord_platform_notify_target_for(p, 42, &got) && got.channel_id == 6 && got.message_id == 7);
      mock_emit_notify_signal("ActivationToken", g_variant_new("(us)", (guint32)42, "plasma_live_TIME98765"));
      mock_emit_notify_signal("ActionInvoked", g_variant_new("(us)", (guint32)42, "default"));
      CT_CHECK(wait_for(p, cond_click, 5000));
      CT_CHECK(g_clicks == 1 && g_click_target.guild_id == 5 && g_click_target.channel_id == 6 &&
               g_click_target.message_id == 7);
      mock_emit_notify_signal("ActionInvoked", g_variant_new("(us)", (guint32)99, "default"));   // unknown id
      mock_emit_notify_signal("NotificationClosed", g_variant_new("(uu)", (guint32)42, (guint32)2));
      CT_CHECK(wait_for(p, cond_unmapped, 5000));
      mock_emit_notify_signal("ActionInvoked", g_variant_new("(us)", (guint32)42, "default"));   // closed already
      CT_CHECK(!wait_for(p, cond_never, 300));   // give the signal time to arrive: it must do nothing
      CT_CHECK(g_clicks == 1);

      // Dismissing channel notifications invokes CloseNotification over D-Bus for matching ids only.
      g_mock.notify_next_id = 100;
      g_mock.close_calls = 0;
      ccord_notify_target c500_a = {.guild_id = 5, .channel_id = 500, .message_id = 10};
      ccord_notify_target c500_b = {.guild_id = 5, .channel_id = 500, .message_id = 20};
      ccord_notify_target c600_a = {.guild_id = 5, .channel_id = 600, .message_id = 30};
      CT_CHECK(ccord_platform_notify_message(p, "DM", "one", &c500_a));
      CT_CHECK(ccord_platform_notify_message(p, "DM", "two", &c500_b));
      CT_CHECK(ccord_platform_notify_message(p, "Other", "three", &c600_a));
      CT_CHECK(wait_for(p, cond_map3, 5000));
      CT_CHECK(ccord_platform_close_channel_notifications(p, 500, 0) == 2);
      CT_CHECK(wait_for(p, cond_close2, 5000));
      CT_CHECK(g_mock.close_calls == 2);
      CT_CHECK((g_mock.closed_ids[0] == 100 && g_mock.closed_ids[1] == 101) ||
               (g_mock.closed_ids[0] == 101 && g_mock.closed_ids[1] == 100));
      CT_CHECK(ccord_platform_notify_map_count(p) == 1);
      CT_CHECK(ccord_platform_notify_target_for(p, 102, &got) && got.channel_id == 600);
      CT_CHECK(ccord_platform_close_channel_notifications(p, 600, 0) == 1);
      CT_CHECK(wait_for(p, cond_close3, 5000));
      CT_CHECK(g_mock.close_calls == 3 && g_mock.closed_ids[2] == 102);
      CT_CHECK(ccord_platform_notify_map_count(p) == 0);
      g_mock.notify_next_id = 0;

      ccord_platform_set_notify_click_callback(p, nullptr, nullptr);
      ccord_platform_set_window_focused(p, true);

      // Badge: the tray number (mentions here) goes into the IconPixmap and tooltip; the Status never asks for
      // attention (that would highlight the taskbar entry) and no NewStatus is emitted.
      ccord_platform_set_unread_badge(p, 7, 2);
      CT_CHECK(wait_for(p, cond_badge_signals, 5000));
      char status[64], tip[256];
      CT_CHECK(sni_get_string(p, g_mock.tray_service, "Status", status, sizeof status));
      CT_CHECK(strcmp(status, "Active") == 0);
      CT_CHECK(sni_get_tooltip(p, g_mock.tray_service, tip, sizeof tip));
      CT_CHECK(strcmp(tip, "C-Cord (2)") == 0);
      {
        GVariant *pix = sni_read_prop(p, g_mock.tray_service, "IconPixmap");
        CT_CHECK(pix != nullptr);
        if (pix) {
          CT_CHECK(g_variant_is_of_type(pix, G_VARIANT_TYPE("a(iiay)")));
          static const gint32 sizes[4] = {22, 24, 32, 48};
          CT_CHECK(g_variant_n_children(pix) == 4);
          for (gsize i = 0; i < g_variant_n_children(pix) && i < 4; i++) {
            gint32 w = 0, h = 0;
            GVariant *data = nullptr;
            GVariant *child = g_variant_get_child_value(pix, i);
            g_variant_get(child, "(ii@ay)", &w, &h, &data);
            CT_CHECK(w == sizes[i] && h == sizes[i]);
            CT_CHECK(g_variant_get_size(data) == (gsize)w * (gsize)h * 4u);
            g_variant_unref(data);
            g_variant_unref(child);
          }
          g_variant_unref(pix);
        }
      }
      CT_CHECK(g_mock.new_status_signals == 0);
      // Unread without a number: the dot picture, tooltip says unread.
      ccord_platform_set_unread_badge(p, 4, 0);
      CT_CHECK(sni_get_string(p, g_mock.tray_service, "Status", status, sizeof status));
      CT_CHECK(strcmp(status, "Active") == 0);
      CT_CHECK(sni_get_tooltip(p, g_mock.tray_service, tip, sizeof tip));
      CT_CHECK(strcmp(tip, "C-Cord (unread)") == 0);
      ccord_platform_set_unread_badge(p, 0, 0);
      CT_CHECK(sni_get_string(p, g_mock.tray_service, "Status", status, sizeof status));
      CT_CHECK(strcmp(status, "Active") == 0);
      CT_CHECK(sni_get_tooltip(p, g_mock.tray_service, tip, sizeof tip));
      CT_CHECK(strcmp(tip, "C-Cord") == 0);
      CT_CHECK(g_mock.new_status_signals == 0);

      // GlobalShortcuts: CreateSession -> Response -> BindShortcuts -> Activated -> CCORD_EV_HOTKEY.
      ccord_global_shortcut defs[2] = {
          {.id = "toggle-mute", .description = "Toggle mute", .preferred_trigger = "CTRL+SHIFT+M"},
          {.id = "toggle-deafen", .description = "Toggle deafen", .preferred_trigger = nullptr},
      };
      CT_CHECK(ccord_platform_bind_global_shortcuts(p, defs, 2));
      CT_CHECK(wait_for(p, cond_bind, 5000));
      CT_CHECK(g_mock.create_session_calls == 1);
      CT_CHECK(g_mock.bind_calls == 1);
      CT_CHECK(g_mock.bound_count == 2);
      CT_CHECK(strcmp(g_mock.bound_ids[0], "toggle-mute") == 0);
      CT_CHECK(strcmp(g_mock.bound_ids[1], "toggle-deafen") == 0);
      s_activated_emissions = 0;
      s_activated_src = g_timeout_add(20, mock_emit_activated, &g_mock);
      CT_CHECK(wait_for(p, cond_hotkey, 5000));
      if (s_activated_src) g_source_remove(s_activated_src);
      s_activated_src = 0;
      bool found = false;
      for (size_t i = 0; i < s_hotkey_count; i++) {
        if (s_hotkeys[i].type == CCORD_EV_HOTKEY && strcmp(s_hotkeys[i].text, "toggle-mute") == 0) found = true;
      }
      CT_CHECK(found);

      // Push-to-talk over the real D-Bus handlers: Activated holds it (and queues no hotkey event), the
      // portal's Deactivated releases it, and a SessionClosed while held releases it too.
      size_t hotkeys_before = s_hotkey_count;
      s_activated_id = "push-to-talk";
      s_activated_emissions = 0;
      s_activated_src = g_timeout_add(20, mock_emit_activated, &g_mock);
      CT_CHECK(wait_for(p, cond_ptt_held, 5000));
      if (s_activated_src) g_source_remove(s_activated_src);
      s_activated_src = 0;
      for (size_t i = hotkeys_before; i < s_hotkey_count; i++) CT_CHECK(strcmp(s_hotkeys[i].text, "push-to-talk") != 0);
      s_deactivated_emissions = 0;
      s_deactivated_src = g_timeout_add(20, mock_emit_deactivated, &g_mock);
      CT_CHECK(wait_for(p, cond_ptt_released, 5000));
      if (s_deactivated_src) g_source_remove(s_deactivated_src);
      s_deactivated_src = 0;
      s_activated_emissions = 0;
      s_activated_src = g_timeout_add(20, mock_emit_activated, &g_mock);
      CT_CHECK(wait_for(p, cond_ptt_held, 5000));
      if (s_activated_src) g_source_remove(s_activated_src);
      s_activated_src = 0;
      s_activated_id = "toggle-mute";

      // SessionClosed drops the session (and releases a held push-to-talk); a rebind creates a fresh one. Re-emit on a
      // timer because the client's AddMatch for the signal may still be in flight.
      s_closed_emissions = 0;
      s_closed_src = g_timeout_add(20, mock_emit_closed, &g_mock);
      CT_CHECK(wait_for(p, cond_ptt_released, 5000));
      wait_for(p, cond_never, 200);
      if (s_closed_src) g_source_remove(s_closed_src);
      s_closed_src = 0;
      CT_CHECK(ccord_platform_bind_global_shortcuts(p, defs, 2));
      CT_CHECK(wait_for(p, cond_rebind, 5000));

      // Tray DBusMenu + SNI click actions (window visibility, mute/deafen checkmarks).
      test_tray_menu_live(p);

      ccord_platform_destroy(p);
      g_live_p = nullptr;
    }
  }
  mock_stop();
  if (saved[0]) setenv("DBUS_SESSION_BUS_ADDRESS", saved, 1);
  else unsetenv("DBUS_SESSION_BUS_ADDRESS");
  kill(pid, SIGTERM);
  waitpid(pid, nullptr, 0);
  g_spawn_close_pid(pid);
}

#endif  // CCORD_HAVE_PORTAL

static void test_long_text_input_chunking(void)
{
  ccord_error err;
  ccord_platform_config cfg = {.title = "C-Cord", .width = 320, .height = 200, .headless = true};
  ccord_platform *p = ccord_platform_create(&cfg, &err);
  CT_CHECK(p != nullptr);
  if (!p) return;
  drain(p);

  // Verify text_input_ex accepts every input kind (email, password, OTP, single-line, text) cleanly.
  ccord_platform_text_input_ex(p, true, CCORD_INPUT_EMAIL, 10, 20, 200, 36);
  ccord_platform_text_input_ex(p, true, CCORD_INPUT_PASSWORD, 10, 60, 200, 36);
  ccord_platform_text_input_ex(p, true, CCORD_INPUT_OTP, 10, 100, 200, 36);
  ccord_platform_text_input_ex(p, true, CCORD_INPUT_SINGLE_LINE, 10, 140, 200, 36);
  ccord_platform_text_input_ex(p, false, CCORD_INPUT_TEXT, 0, 0, 0, 0);

  // Push a long (>32 byte) UTF-8 string containing multi-byte code points (simulating mobile keyboard
  // clipboard paste or password-manager commitText) and verify every byte arrives intact across chunks.
  static const char long_paste[] =
      "MySuperLongP@ssw0rd_FromBitwarden_AndClipboard_éèê_🚀✨_EndOfPaste_1234567890!";
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = SDL_EVENT_TEXT_INPUT;
  e.text.text = long_paste;
  (void)SDL_PushEvent(&e);

  char got[256] = {0};
  size_t got_len = 0;
  int chunks = 0;
  ccord_input_event ev;
  while (ccord_platform_next_event(p, &ev, 0)) {
    if (ev.type == CCORD_IN_TEXT) {
      size_t clen = strlen(ev.text);
      CT_CHECK(clen > 0 && clen < sizeof ev.text);
      if (got_len + clen < sizeof got) {
        memcpy(got + got_len, ev.text, clen);
        got_len += clen;
        got[got_len] = '\0';
      }
      chunks++;
    }
  }
  CT_CHECK(chunks >= 3);
  CT_CHECK(strcmp(got, long_paste) == 0);
  ccord_platform_destroy(p);
}

static void test_mobile_background_lifecycle_and_notify(void)
{
  ccord_error err;
  ccord_platform_config cfg = {.title = "C-Cord", .width = 412, .height = 915, .headless = true};
  ccord_platform *p = ccord_platform_create(&cfg, &err);
  CT_CHECK(p != nullptr);
  if (!p) return;
  drain(p);

  ccord_platform_set_window_focused(p, true);
  CT_CHECK(ccord_platform_window_focused(p));
  ccord_platform_set_keep_alive(p, true);

  // Entering background (even without a preceding FOCUS_LOST) must wake next_event immediately,
  // clear window_focused, and allow message notifications to fire.
  SDL_Event bg = {.type = SDL_EVENT_WILL_ENTER_BACKGROUND};
  (void)SDL_PushEvent(&bg);
  uint64_t t0 = SDL_GetTicks();
  ccord_input_event ev;
  bool got = ccord_platform_next_event(p, &ev, 250);
  uint64_t elapsed = SDL_GetTicks() - t0;
  CT_CHECK(got && ev.type == CCORD_IN_APP_BACKGROUND);
  CT_CHECK(elapsed < 150u);
  CT_CHECK(ccord_platform_in_background(p));
  CT_CHECK(!ccord_platform_window_focused(p));

  ccord_notify_target target = {.guild_id = 100, .channel_id = 200, .message_id = 300};
  CT_CHECK(ccord_platform_notify_message(p, "Alice", "Ping while backgrounded", &target));
  CT_CHECK(ccord_platform_notify_count(p) == 1u);
  CT_CHECK(strcmp(ccord_platform_last_notify_title(p), "Alice") == 0);
  CT_CHECK(strcmp(ccord_platform_last_notify_body(p), "Ping while backgrounded") == 0);

  // Simulate minimized state (e.g. Android Back key) before returning to foreground.
  SDL_Event min_ev = {.type = SDL_EVENT_WINDOW_MINIMIZED};
  (void)SDL_PushEvent(&min_ev);
  drain(p);
  CT_CHECK(ccord_platform_window_minimized(p));

  // Returning to foreground must wake immediately and clear minimized/background state so drawing resumes at once.
  SDL_Event fg = {.type = SDL_EVENT_DID_ENTER_FOREGROUND};
  (void)SDL_PushEvent(&fg);
  t0 = SDL_GetTicks();
  got = ccord_platform_next_event(p, &ev, 250);
  elapsed = SDL_GetTicks() - t0;
  CT_CHECK(got && ev.type == CCORD_IN_APP_FOREGROUND);
  CT_CHECK(elapsed < 150u);
  CT_CHECK(!ccord_platform_in_background(p));
  CT_CHECK(!ccord_platform_window_minimized(p));
  CT_CHECK(ccord_platform_window_visible(p));

  // Render target reset must also wake immediately without waiting out timeout_ms.
  SDL_Event rst = {.type = SDL_EVENT_RENDER_TARGETS_RESET};
  (void)SDL_PushEvent(&rst);
  t0 = SDL_GetTicks();
  got = ccord_platform_next_event(p, &ev, 250);
  elapsed = SDL_GetTicks() - t0;
  CT_CHECK(got && ev.type == CCORD_IN_RENDER_RESET);
  CT_CHECK(elapsed < 150u);

  ccord_platform_set_keep_alive(p, false);
  ccord_platform_destroy(p);
}

#if !defined(_WIN32)
typedef struct wake_sock_arg {
  ccord_socket_t fd;
  uint32_t delay_ms;
} wake_sock_arg;

static void wake_sock_thread(void *ud)
{
  wake_sock_arg *a = (wake_sock_arg *)ud;
  ccord_sleep_ms(a->delay_ms);
  uint8_t b = 0x5Au;
  bool wb = false;
  (void)ccord_sock_send(a->fd, &b, 1, &wb);
}

static void wake_fg_thread(void *ud)
{
  uint32_t *delay = (uint32_t *)ud;
  ccord_sleep_ms(*delay);
  SDL_Event fg = {.type = SDL_EVENT_DID_ENTER_FOREGROUND};
  (void)SDL_PushEvent(&fg);
}

static void test_background_wait_idle_poll(void)
{
  ccord_error err;
  ccord_platform_config cfg = {.title = "C-Cord", .width = 412, .height = 915, .headless = true};
  ccord_platform *p = ccord_platform_create(&cfg, &err);
  CT_CHECK(p != nullptr);
  if (!p) return;
  drain(p);

  // Move platform into background state so headless wait_idle exercises kernel poll() + wake_pipe.
  SDL_Event bg = {.type = SDL_EVENT_DID_ENTER_BACKGROUND};
  (void)SDL_PushEvent(&bg);
  ccord_input_event ev;
  CT_CHECK(ccord_platform_next_event(p, &ev, 250) && ev.type == CCORD_IN_APP_BACKGROUND);
  CT_CHECK(ccord_platform_in_background(p));

  // 1. Readable extra_fd must wake poll() immediately rather than sleeping out a 3000 ms timeout.
  ccord_socket_t sv[2] = {CCORD_INVALID_SOCKET, CCORD_INVALID_SOCKET};
  CT_CHECK(ccord_sock_pair(sv));
  wake_sock_arg arg = {.fd = sv[1], .delay_ms = 30};
  ccord_thread *th = ccord_thread_create(wake_sock_thread, &arg);
  CT_CHECK(th != nullptr);
  uint64_t t0 = SDL_GetTicks();
  bool got = ccord_platform_wait_idle(p, &ev, &sv[0], 1, 3000);
  uint64_t elapsed = SDL_GetTicks() - t0;
  ccord_thread_join(th);
  CT_CHECK(!got);          // extra_fd woke poll(), not an input event
  CT_CHECK(elapsed < 1000u);
  uint8_t rb = 0;
  bool wb = false;
  CT_CHECK(ccord_sock_recv(sv[0], &rb, 1, &wb) == 1 && rb == 0x5Au);
  ccord_sock_close(sv[0]);
  ccord_sock_close(sv[1]);

  // 1b. Idle sleep with no events or readable fds must honor the requested timeout (timerfd + no main-thread self-wake).
  {
    // Push an unrelated SDL event on the main thread before entering wait_idle to ensure poll-prep draining
    // does not re-trigger wake_pipe[1] and abort the upcoming poll() with 0 ms sleep.
    SDL_Event dummy = {.type = SDL_EVENT_USER};
    (void)SDL_PushEvent(&dummy);
    t0 = SDL_GetTicks();
    got = ccord_platform_wait_idle(p, &ev, nullptr, 0, 120);
    elapsed = SDL_GetTicks() - t0;
    CT_CHECK(!got);
    CT_CHECK(elapsed >= 90u && elapsed < 1000u);
  }

  // 2. Foreground transition from another thread must wake wake_pipe immediately and deliver CCORD_IN_APP_FOREGROUND.
  uint32_t fg_delay = 30;
  ccord_thread *th2 = ccord_thread_create(wake_fg_thread, &fg_delay);
  CT_CHECK(th2 != nullptr);
  t0 = SDL_GetTicks();
  got = ccord_platform_wait_idle(p, &ev, nullptr, 0, 3000);
  elapsed = SDL_GetTicks() - t0;
  ccord_thread_join(th2);
  CT_CHECK(got && ev.type == CCORD_IN_APP_FOREGROUND);
  CT_CHECK(elapsed < 1000u);
  CT_CHECK(!ccord_platform_in_background(p));

  ccord_platform_destroy(p);
}
#endif

int main(void)
{
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  // SDL3 ends a zero-timeout poll cycle at an internal POLL_SENTINEL event; with the sentinel on,
  // an event pushed after a poll lands behind it and is only delivered one poll later. The
  // push-one/poll-one checks below need immediate delivery, so disable the sentinel here.
  SDL_SetHint(SDL_HINT_POLL_SENTINEL, "0");
  test_offline();
  test_long_text_input_chunking();
  test_mobile_background_lifecycle_and_notify();
#if !defined(_WIN32)
  test_background_wait_idle_poll();
#endif
  test_window_lifecycle();
  test_dead_bus();
#if CCORD_HAVE_PORTAL
  test_empty_bus();
  test_late_notify_daemon();
  test_live();
#endif
  return CT_RESULT("test_platform_desktop");
}
