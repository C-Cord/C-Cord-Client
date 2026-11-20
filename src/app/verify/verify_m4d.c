// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "verify_dispatch.h"

#include <ccord/core/clock.h>
#include <ccord/net/admin.h>
#include <ccord/net/governor.h>
#include <ccord/net/mock_transport.h>
#include <ccord/store/admin.h>
#include <ccord/store/roles.h>

#include <string.h>

static bool verify_click_cs(app *a, const char *name)
{
  ccord_rect rc;
  if (!ccord_ui_channel_settings_find(a->ui, name, &rc)) {
    ccord_input_event top = {.type = CCORD_IN_WHEEL, .wheel = 40.0f, .x = 640.0f, .y = 400.0f};
    app_inject(a, &top);
    app_run_frames(a, 1);
    for (int step_i = 0; step_i < 16 && !ccord_ui_channel_settings_find(a->ui, name, &rc); step_i++) {
      ccord_input_event down = {.type = CCORD_IN_WHEEL, .wheel = -4.0f, .x = 640.0f, .y = 400.0f};
      app_inject(a, &down);
      app_run_frames(a, 1);
    }
  }
  if (!ccord_ui_channel_settings_find(a->ui, name, &rc)) {
    fprintf(stderr, "ccord: --verify-slice m4d: control '%s' not found in Channel Settings\n", name);
    return false;
  }
  float cx = rc.x + rc.w * 0.5f, cy = rc.y + rc.h * 0.5f;
  app_inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, cx, cy);
  app_inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, cx, cy);
  app_inject_mouse(a, CCORD_IN_MOUSE_UP, 1, cx, cy);
  app_run_frames(a, 1);
  return true;
}

static bool verify_click_menu(app *a, const char *label)
{
  ccord_rect rc;
  if (!ccord_ui_context_menu_find(a->ui, label, &rc)) {
    fprintf(stderr, "ccord: --verify-slice m4d: context menu item '%s' not found\n", label);
    return false;
  }
  float cx = rc.x + rc.w * 0.5f, cy = rc.y + rc.h * 0.5f;
  app_inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, cx, cy);
  app_inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, cx, cy);
  app_inject_mouse(a, CCORD_IN_MOUSE_UP, 1, cx, cy);
  app_run_frames(a, 1);
  return true;
}

int run_verify_slice_m4d(app *a)
{
  for (int i = 0; i < 200; i++) {
    a->virtual_ms += VIRTUAL_FRAME_MS;
    app_step(a, false);
    ccord_id ch = ccord_store_selected_channel(a->store);
    if (ch != CCORD_ID_NONE && ccord_store_history_loaded(a->store, ch)) break;
  }
  if (ccord_session_state(a->session) != CCORD_CONN_CONNECTED) {
    fputs("ccord: --verify-slice m4d: transport did not connect\n", stderr);
    return 1;
  }
  app_run_frames(a, 3);

  const ccord_id gid = 2001u;
  const ccord_id dev = 3113u;
  const ccord_id ada = 1002u;

  // 1. Hover channel #dev in sidebar, click its gear icon to open Channel Settings.
  float dev_y = 0.0f;
  for (float y = 60.0f; y < 800.0f; y += 6.0f) {
    app_inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, 180.0f, y);
    app_run_frames(a, 1);
    ccord_ui_debug d = {0};
    ccord_ui_get_debug(a->ui, &d);
    if (d.hover_channel == dev) {
      dev_y = y;
      break;
    }
  }
  ccord_rect gear_rc = {0};
  if (dev_y <= 0.0f || !ccord_ui_channel_gear_rect(a->ui, dev, &gear_rc)) {
    fputs("ccord: --verify-slice m4d: #dev channel row or gear icon not found\n", stderr);
    return 1;
  }
  float gx = gear_rc.x + gear_rc.w * 0.5f, gy = gear_rc.y + gear_rc.h * 0.5f;
  app_inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, gx, gy);
  app_inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, gx, gy);
  app_inject_mouse(a, CCORD_IN_MOUSE_UP, 1, gx, gy);
  app_run_frames(a, 2);

  ccord_ui_channel_settings_debug csdbg = {0};
  ccord_ui_get_channel_settings_debug(a->ui, &csdbg);
  if (!csdbg.open || csdbg.channel_id != dev) {
    fputs("ccord: --verify-slice m4d: gear click did not open Channel Settings\n", stderr);
    return 1;
  }

  // Overview: toggle NSFW + 10s slowmode, save, verify via store.
  if (!verify_click_cs(a, "nsfw")) return 1;
  if (!verify_click_cs(a, "slowmode:10")) return 1;
  if (!verify_click_cs(a, "save")) return 1;
  app_run_frames(a, 4);
  const ccord_channel *dev_ch = ccord_store_channel(a->store, dev);
  if (!dev_ch || !dev_ch->nsfw || dev_ch->rate_limit_per_user != 10u) {
    fputs("ccord: --verify-slice m4d: Channel Settings Overview save did not persist\n", stderr);
    return 1;
  }

  // 2. Permission Overwrites: add role overwrite for Members (5003), set Allow VIEW_CHANNEL + Deny SEND_MESSAGES, save.
  if (!verify_click_cs(a, "page:permissions")) return 1;
  app_run_frames(a, 2);
  if (!verify_click_cs(a, "ow_add_toggle")) return 1;
  if (!verify_click_cs(a, "ow_add_role:5003")) return 1;
  if (!verify_click_cs(a, "ow_allow:400")) return 1;
  if (!verify_click_cs(a, "ow_deny:800")) return 1;
  if (!verify_click_cs(a, "save")) return 1;
  app_run_frames(a, 4);

  size_t n_overwrites = 0;
  const ccord_overwrite *ows = ccord_store_channel_overwrites(a->store, dev, &n_overwrites);
  bool found_role_ow = false;
  for (size_t i = 0; ows && i < n_overwrites; i++) {
    if (ows[i].id == 5003u && (ows[i].allow & 0x400u) != 0u && (ows[i].deny & 0x800u) != 0u) {
      found_role_ow = true;
      break;
    }
  }
  if (!found_role_ow) {
    fputs("ccord: --verify-slice m4d: Permission Overwrite PUT did not persist\n", stderr);
    return 1;
  }

  // Invites tab: verify channel invites load, then close Channel Settings.
  if (!verify_click_cs(a, "page:invites")) return 1;
  app_run_frames(a, 4);
  if (!ccord_store_channel_invites_loaded(a->store, gid, dev)) {
    fputs("ccord: --verify-slice m4d: Channel Invites did not load\n", stderr);
    return 1;
  }
  if (!verify_click_cs(a, "close")) return 1;
  app_run_frames(a, 2);

  // 3. Clone Channel from channel context menu.
  size_t nch_before = ccord_store_channel_count(a->store, gid);
  app_inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, 180.0f, dev_y);
  app_run_frames(a, 1);
  app_inject_mouse(a, CCORD_IN_MOUSE_DOWN, 3, 180.0f, dev_y);
  app_inject_mouse(a, CCORD_IN_MOUSE_UP, 3, 180.0f, dev_y);
  app_run_frames(a, 2);
  if (!verify_click_menu(a, "Clone Channel")) return 1;
  app_run_frames(a, 4);

  int cloned_channel = 0;
  size_t nch_after = ccord_store_channel_count(a->store, gid);
  if (nch_after == nch_before + 1u) {
    for (size_t i = 0; i < nch_after; i++) {
      const ccord_channel *c = ccord_store_channel_at(a->store, gid, i);
      if (c && c->id != dev && c->name && strcmp(c->name, "dev") == 0 && c->nsfw && c->rate_limit_per_user == 10u) {
        size_t cl_ows = 0;
        (void)ccord_store_channel_overwrites(a->store, c->id, &cl_ows);
        if (cl_ows == n_overwrites) cloned_channel = 1;
        break;
      }
    }
  }
  if (!cloned_channel) {
    fputs("ccord: --verify-slice m4d: Clone Channel did not copy config and overwrites\n", stderr);
    return 1;
  }

  // 4. Voice Moderation: right-click Ada (1002) in voice channel 3121 -> Server Mute, then Disconnect.
  float ada_y = 0.0f;
  for (float y = 60.0f; y < 860.0f; y += 6.0f) {
    app_inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, 180.0f, y);
    app_run_frames(a, 1);
    ccord_ui_debug d = {0};
    ccord_ui_get_debug(a->ui, &d);
    if (d.hover_user == ada) {
      ada_y = y;
      break;
    }
  }
  if (ada_y <= 0.0f) {
    fputs("ccord: --verify-slice m4d: voice participant row for user 1002 not found\n", stderr);
    return 1;
  }
  app_inject_mouse(a, CCORD_IN_MOUSE_DOWN, 3, 180.0f, ada_y);
  app_inject_mouse(a, CCORD_IN_MOUSE_UP, 3, 180.0f, ada_y);
  app_run_frames(a, 2);
  if (!verify_click_menu(a, "Server Mute")) return 1;
  app_run_frames(a, 4);

  const ccord_voice_state *vs = ccord_store_voice_user(a->store, gid, ada);
  const ccord_member *mb = ccord_store_find_member(a->store, gid, ada);
  int voice_muted = (vs && vs->mute && mb && mb->mute) ? 1 : 0;
  if (!voice_muted) {
    fputs("ccord: --verify-slice m4d: Server Mute did not persist\n", stderr);
    return 1;
  }

  app_inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, 180.0f, ada_y);
  app_run_frames(a, 1);
  app_inject_mouse(a, CCORD_IN_MOUSE_DOWN, 3, 180.0f, ada_y);
  app_inject_mouse(a, CCORD_IN_MOUSE_UP, 3, 180.0f, ada_y);
  app_run_frames(a, 2);
  if (!verify_click_menu(a, "Disconnect")) return 1;
  app_run_frames(a, 4);

  int voice_disconnected = (ccord_store_voice_user(a->store, gid, ada) == nullptr) ? 1 : 0;
  if (!voice_disconnected) {
    fputs("ccord: --verify-slice m4d: Voice Disconnect did not remove voice participant\n", stderr);
    return 1;
  }

  printf("m4d_channel_overwrites=%zu\n"
         "m4d_cloned_channel=%d\n"
         "m4d_voice_muted=%d\n"
         "m4d_voice_disconnected=%d\n",
         n_overwrites, cloned_channel, voice_muted, voice_disconnected);
  return 0;
}

