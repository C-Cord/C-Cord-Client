// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice global-ptt: push-to-talk through the XDG GlobalShortcuts portal while the window is unfocused.
// A private headless platform stands in for the portal: its injection hooks feed the same state callback the
// portal's Activated / Deactivated / SessionClosed signals reach. The coordinator's real PTT gate
// (ccord_app_media_ptt_open) reads the held state through the same ops seam main.c wires up.
#include <stdio.h>
#include <string.h>

#include <ccord/platform/platform.h>
#include <ccord/theme/theme.h>

#include "app_desktop.h"
#include "app_media.h"
#include "verify_ui_parity.h"

#define PTT_SCANCODE 20

typedef struct gp {
  ccord_platform *platform;
  bool key_down;   // the focused keyboard state for PTT_SCANCODE
} gp;

static bool op_key_held(void *ud, int scancode)
{
  return scancode == PTT_SCANCODE && ((gp *)ud)->key_down;
}

static bool op_global_ptt_held(void *ud) { return ccord_platform_global_ptt_held(((gp *)ud)->platform); }

int ccord_verify_global_ptt_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  int checks = 0;
  ccord_error err;
  ccord_platform *p = nullptr;
  ccord_app_desktop *desktop = nullptr;
  static ccord_theme theme;
#define CHECK(cond, name)                                      \
  do {                                                         \
    if (!(cond)) {                                             \
      printf("global-ptt: FAIL %s\n", name);                   \
      ccord_app_desktop_destroy(desktop);                      \
      ccord_platform_destroy(p);                               \
      return 1;                                                \
    }                                                          \
    checks++;                                                  \
  } while (0)
  ccord_platform_config cfg = {.title = "C-Cord", .width = 640, .height = 400, .headless = true};
  p = ccord_platform_create(&cfg, &err);
  if (!p || !ctx || !ctx->session || !ctx->store) {
    puts("global-ptt: FAIL setup");
    ccord_platform_destroy(p);
    return 1;
  }
  gp g = {.platform = p};
  ccord_app_media_ops ops = {.key_held = op_key_held, .global_ptt_held = op_global_ptt_held};
  ccord_ui_voice_prefs prefs = {.ptt = true, .ptt_scancode = PTT_SCANCODE};
  ccord_input_event ev;

  // Window unfocused, nothing held: the gate is closed.
  ccord_platform_set_window_focused(p, false);
  CHECK(!ccord_platform_window_focused(p), "unfocused");
  CHECK(!ccord_platform_global_ptt_held(p), "initially-released");
  CHECK(!ccord_app_media_ptt_open(&ops, &g, &prefs), "gate-closed-idle");

  // Portal Activated for the PTT shortcut: held while unfocused, and no toggle event reaches the app.
  CHECK(ccord_platform_inject_hotkey(p, CCORD_SHORTCUT_PUSH_TO_TALK, &err) == CCORD_OK, "inject-activated");
  CHECK(ccord_platform_global_ptt_held(p), "activated-held");
  CHECK(ccord_app_media_ptt_open(&ops, &g, &prefs), "gate-open-unfocused");
  CHECK(!ccord_platform_next_event(p, &ev, 0) || ev.type != CCORD_EV_HOTKEY, "no-hotkey-event-for-ptt");
  CHECK(ccord_platform_global_ptt_held(p), "still-held-after-pump");

  // Portal Deactivated: released.
  CHECK(ccord_platform_inject_shortcut_deactivated(p, CCORD_SHORTCUT_PUSH_TO_TALK, &err) == CCORD_OK,
        "inject-deactivated");
  CHECK(!ccord_platform_global_ptt_held(p), "deactivated-released");
  CHECK(!ccord_app_media_ptt_open(&ops, &g, &prefs), "gate-closed-after-release");

  // A deactivated for some other shortcut leaves a held PTT alone.
  CHECK(ccord_platform_inject_hotkey(p, CCORD_SHORTCUT_PUSH_TO_TALK, &err) == CCORD_OK, "re-activate");
  CHECK(ccord_platform_inject_shortcut_deactivated(p, "toggle-mute", &err) == CCORD_OK, "other-deactivated");
  CHECK(ccord_platform_global_ptt_held(p), "other-shortcut-ignored");

  // Portal session closed while held: the held state clears so the mic is never stuck open.
  ccord_platform_inject_portal_session_closed(p);
  CHECK(!ccord_platform_global_ptt_held(p), "session-close-clears");
  CHECK(!ccord_app_media_ptt_open(&ops, &g, &prefs), "gate-closed-after-session-close");

  // Focused push-to-talk by scancode still works, with and without the portal.
  g.key_down = true;
  CHECK(ccord_app_media_ptt_open(&ops, &g, &prefs), "scancode-held-opens");
  g.key_down = false;
  CHECK(!ccord_app_media_ptt_open(&ops, &g, &prefs), "scancode-released-closes");
  prefs.ptt_scancode = 0;
  g.key_down = true;
  CHECK(!ccord_app_media_ptt_open(&ops, &g, &prefs), "unbound-scancode-ignored");
  prefs.ptt_scancode = PTT_SCANCODE;
  ops.global_ptt_held = nullptr;   // no portal available: behaviour as before
  CHECK(ccord_app_media_ptt_open(&ops, &g, &prefs), "no-portal-scancode-works");
  g.key_down = false;
  CHECK(!ccord_app_media_ptt_open(&ops, &g, &prefs), "no-portal-released");
  ops.global_ptt_held = op_global_ptt_held;
  CHECK(ccord_platform_inject_hotkey(p, CCORD_SHORTCUT_PUSH_TO_TALK, &err) == CCORD_OK, "activate-for-voice-activity");
  prefs.ptt = false;
  CHECK(ccord_app_media_ptt_open(&ops, &g, &prefs), "voice-activity-always-open");
  prefs.ptt = true;
  ccord_platform_inject_portal_session_closed(p);

  // toggle-mute still toggles: injected, delivered as a hotkey event, applied to the voice state.
  ccord_app_desktop_deps deps = {.store = ctx->store, .session = ctx->session, .platform = p, .theme = &theme};
  desktop = ccord_app_desktop_create(&deps, &err);
  CHECK(desktop != nullptr, "desktop-create");
  enum { GUILD = 2001, VOICE = 3121 };
  ctx->frames(ctx->ud, 5);
  ccord_status jst = ccord_session_join_voice(ctx->session, GUILD, VOICE, &err);
  CHECK(jst == CCORD_OK, "join-voice");
  for (int i = 0; i < 400 && ccord_store_active_voice_channel(ctx->store) != VOICE; i++) ctx->frames(ctx->ud, 1);
  CHECK(ccord_store_active_voice_channel(ctx->store) == VOICE, "in-voice");
  ccord_id self = ccord_store_self_id(ctx->store);
  const ccord_voice_state *vs = ccord_store_voice_user(ctx->store, GUILD, self);
  CHECK(vs && !vs->self_mute, "not-muted");
  CHECK(ccord_platform_inject_hotkey(p, "toggle-mute", &err) == CCORD_OK, "inject-toggle-mute");
  CHECK(ccord_platform_next_event(p, &ev, 0) && ev.type == CCORD_EV_HOTKEY && strcmp(ev.text, "toggle-mute") == 0,
        "toggle-mute-event");
  CHECK(ccord_app_desktop_handle_hotkey(desktop, ev.text, &err), "toggle-mute-handled");
  for (int i = 0; i < 400; i++) {
    vs = ccord_store_voice_user(ctx->store, GUILD, self);
    if (vs && vs->self_mute) break;
    ctx->frames(ctx->ud, 1);
  }
  CHECK(vs && vs->self_mute, "toggle-mute-toggles");
  CHECK(ccord_session_leave_voice(ctx->session, GUILD, &err) == CCORD_OK, "leave-voice");

  ccord_app_desktop_destroy(desktop);
  ccord_platform_destroy(p);
  printf("global-ptt: PASS checks=%d\n", checks);
  fflush(stdout);
  return 0;
#undef CHECK
}
