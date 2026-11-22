// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/ui/layout.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "ccord_test.h"

typedef struct mob_fixture {
  ccord_theme theme;
  ccord_bus *bus;
  ccord_store *store;
  ccord_session *session;
  ccord_contrib *contrib;
  ccord_platform *platform;
  ccord_renderer *renderer;
  ccord_ui *ui;
  uint64_t now;
  bool text_input_active;
  int last_input_kind;
  int last_autofill_mode;
  int last_autofill_active;
  int haptic_calls;
  int last_haptic_style;
  int dismiss_kb_calls;
  int share_calls;
  char clip[256];
} mob_fixture;

static mob_fixture F;

static uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

static void host_clip_set(void *ud, const char *utf8)
{
  (void)ud;
  snprintf(F.clip, sizeof F.clip, "%s", utf8 ? utf8 : "");
}

static char *host_clip_get(void *ud)
{
  (void)ud;
  if (!F.clip[0]) return nullptr;
  return ccord_strdup(F.clip);
}

static void host_text_input_kind(void *ud, int kind)
{
  (void)ud;
  F.last_input_kind = kind;
}

static void host_auth_autofill(void *ud, int mode, int active_field, bool commit,
                               ccord_rect email_r, ccord_rect pw_r, ccord_rect otp_r,
                               const char *email_text, const char *pw_text, const char *otp_text)
{
  (void)ud; (void)commit; (void)email_r; (void)pw_r; (void)otp_r;
  (void)email_text; (void)pw_text; (void)otp_text;
  F.last_autofill_mode = mode;
  F.last_autofill_active = active_field;
}

static void host_text_input(void *ud, bool active, float x, float y, float w, float h)
{
  (void)ud; (void)x; (void)y; (void)w; (void)h;
  F.text_input_active = active;
}

static bool host_haptic_impact(void *ud, int style)
{
  (void)ud;
  F.haptic_calls++;
  F.last_haptic_style = style;
  return true;
}

static void host_dismiss_keyboard(void *ud)
{
  (void)ud;
  F.dismiss_kb_calls++;
}

static bool host_share_text(void *ud, const char *title, const char *text)
{
  (void)ud; (void)title; (void)text;
  F.share_calls++;
  return true;
}

static void step_frames(int n, uint64_t dt_ms)
{
  for (int i = 0; i < n; i++) {
    F.now += dt_ms;
    (void)ccord_session_pump(F.session);
    (void)ccord_bus_drain(F.bus);
    (void)ccord_ui_update(F.ui, F.now);
    ccord_render_begin(F.renderer, F.theme.c.bg_primary);
    ccord_ui_draw(F.ui, F.renderer);
    ccord_render_end(F.renderer);
  }
}

static void touch_ev(ccord_input_type type, int64_t finger, float x, float y)
{
  ccord_input_event ev = {.type = type, .finger_id = finger, .x = x, .y = y};
  (void)ccord_ui_handle_input(F.ui, &ev);
}

static void touch_tap(float x, float y)
{
  touch_ev(CCORD_IN_TOUCH_DOWN, 1, x, y);
  step_frames(1, 30);
  touch_ev(CCORD_IN_TOUCH_UP, 1, x, y);
  step_frames(2, 30);
}

static bool fixture_init(void)
{
  memset(&F, 0, sizeof F);
  F.now = 1000;
  ccord_error err = {0};
  if (ccord_paths_init("test_ui_mobile", &err) != CCORD_OK) return false;
  char tpath[CCORD_PATH_MAX];
  if (ccord_paths_join(tpath, sizeof tpath, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK) {
    return false;
  }
  if (ccord_theme_load(&F.theme, tpath, &err) != CCORD_OK) return false;

  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  ccord_mock_config mc = {.latency_ms = 0, .now_ms = fake_clock, .clock_ud = &F};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, tr, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_platform_config pc = {.title = "MobileTest", .width = 412, .height = 915, .min_width = 320, .min_height = 400,
                              .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.bus || !F.store || !F.session || !F.contrib || !F.platform || !F.renderer) return false;

  ccord_ui_deps deps = {
    .bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
    .host = {
      .ud = &F,
      .clipboard_set = host_clip_set,
      .clipboard_get = host_clip_get,
      .text_input = host_text_input,
      .text_input_kind = host_text_input_kind,
      .auth_autofill = host_auth_autofill,
      .haptic_impact = host_haptic_impact,
      .dismiss_keyboard = host_dismiss_keyboard,
      .share_text = host_share_text,
    },
    .fixed_now_unix_ms = 1793534400000LL,
  };
  F.ui = ccord_ui_create(&deps, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 412.0f, 915.0f);
  ccord_ui_set_mobile_mode(F.ui, true);
  ccord_ui_set_touch_mode(F.ui, true);
  ccord_ui_set_safe_insets(F.ui, 44.0f, 0.0f, 34.0f, 0.0f);
  if (ccord_session_start(F.session, &err) != CCORD_OK) return false;
  step_frames(6, 50);
  return true;
}

static void fixture_destroy(void)
{
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_contrib_destroy(F.contrib);
  ccord_session_destroy(F.session);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
}

static void test_mobile_layout_geometry(void)
{
  ccord_theme_metrics m = {
    .server_list_width = 72.0f, .channel_list_width = 240.0f, .member_list_width = 240.0f,
    .header_height = 48.0f, .user_panel_height = 52.0f, .composer_min_height = 44.0f,
    .composer_max_height = 160.0f, .hide_members_below_width = 900.0f,
  };
  ccord_layout_insets safe = {.top = 44.0f, .right = 12.0f, .bottom = 34.0f, .left = 12.0f};
  ccord_layout lo = {0};

  // Closed drawers, no soft keyboard: chat spans full safe area.
  ccord_layout_compute_mobile(&m, 412.0f, 915.0f, 44.0f, safe, 0.0f, 0.0f, 0.0f, &lo);
  CT_CHECK(lo.mobile);
  CT_CHECK(!lo.left_drawer_visible);
  CT_CHECK(!lo.right_drawer_visible);
  CT_CHECK(!lo.member_list_visible);
  CT_CHECK(fabsf(lo.chat_header.y - 44.0f) < 0.5f);
  CT_CHECK(fabsf(lo.chat_header.x - 12.0f) < 0.5f);
  CT_CHECK(fabsf(lo.chat_header.w - (412.0f - 24.0f)) < 0.5f);
  CT_CHECK(lo.composer.y + lo.composer.h <= 915.0f - 34.0f + 0.5f);
  CT_CHECK(lo.message_area.y >= lo.chat_header.y + lo.chat_header.h - 0.5f);
  CT_CHECK(lo.message_area.y + lo.message_area.h <= lo.composer.y + 0.5f);

  // Soft keyboard IME inset lifts composer and shrinks message area without overlap.
  float prev_composer_y = lo.composer.y;
  float prev_msg_h = lo.message_area.h;
  ccord_layout_compute_mobile(&m, 412.0f, 915.0f, 44.0f, safe, 300.0f, 0.0f, 0.0f, &lo);
  CT_CHECK(lo.composer.y < prev_composer_y - 200.0f);
  CT_CHECK(lo.composer.y + lo.composer.h <= 915.0f - 300.0f + 0.5f);
  CT_CHECK(lo.message_area.h < prev_msg_h - 200.0f);
  CT_CHECK(lo.message_area.y + lo.message_area.h <= lo.composer.y + 0.5f);

  // Left drawer slide interpolation (0.0 -> 0.5 -> 1.0).
  ccord_layout lo_mid = {0}, lo_open = {0};
  ccord_layout_compute_mobile(&m, 412.0f, 915.0f, 44.0f, safe, 0.0f, 0.5f, 0.0f, &lo_mid);
  ccord_layout_compute_mobile(&m, 412.0f, 915.0f, 44.0f, safe, 0.0f, 1.0f, 0.0f, &lo_open);
  CT_CHECK(lo_mid.left_drawer_visible);
  CT_CHECK(lo_open.left_drawer_visible);
  CT_CHECK(lo_mid.server_list.x < lo_open.server_list.x);
  CT_CHECK(fabsf(lo_open.server_list.x - safe.left) < 0.5f);
  CT_CHECK(fabsf(lo_open.channel_list.x - (lo_open.server_list.x + lo_open.server_list.w)) < 0.5f);

  // Right drawer slide interpolation (0.0 -> 1.0).
  ccord_layout lo_right = {0};
  ccord_layout_compute_mobile(&m, 412.0f, 915.0f, 44.0f, safe, 0.0f, 0.0f, 1.0f, &lo_right);
  CT_CHECK(lo_right.right_drawer_visible);
  CT_CHECK(lo_right.member_list_visible);
  CT_CHECK(fabsf((lo_right.member_list.x + lo_right.member_list.w) - (412.0f - safe.right)) < 0.5f);
}

static void test_mobile_drawers_and_gestures(void)
{
  CT_CHECK(ccord_ui_is_mobile_layout(F.ui));
  CT_CHECK(!ccord_ui_left_drawer_open(F.ui));
  CT_CHECK(!ccord_ui_right_drawer_open(F.ui));
  // Touch mode gates IME until user explicitly taps a text field.
  CT_CHECK(!F.text_input_active);

  // 1. Tap hamburger button in chat header -> opens left drawer.
  ccord_rect hb = ui_drawer_btn_rect(F.ui);
  CT_CHECK(hb.w > 20.0f && hb.h > 20.0f);
  touch_tap(hb.x + hb.w * 0.5f, hb.y + hb.h * 0.5f);
  step_frames(10, 40);
  CT_CHECK(ccord_ui_left_drawer_open(F.ui));
  CT_CHECK(F.ui->left_drawer_t > 0.95f);
  CT_CHECK(!F.ui->mouse_in);

  // 2. Tap scrim to the right of left drawer -> closes left drawer.
  float nav_right = F.ui->layout.channel_list.x + F.ui->layout.channel_list.w;
  CT_CHECK(nav_right < 400.0f);
  touch_tap(404.0f, 450.0f);
  step_frames(10, 40);
  CT_CHECK(!ccord_ui_left_drawer_open(F.ui));
  CT_CHECK(F.ui->left_drawer_t < 0.05f);

  // 3. Left edge horizontal swipe opens left drawer and fires haptic.
  int haptics_before = F.haptic_calls;
  touch_ev(CCORD_IN_TOUCH_DOWN, 1, 10.0f, 400.0f);
  step_frames(1, 16);
  touch_ev(CCORD_IN_TOUCH_MOVE, 1, 100.0f, 402.0f);
  step_frames(1, 16);
  touch_ev(CCORD_IN_TOUCH_MOVE, 1, 240.0f, 405.0f);
  step_frames(1, 16);
  touch_ev(CCORD_IN_TOUCH_UP, 1, 240.0f, 405.0f);
  step_frames(10, 40);
  CT_CHECK(ccord_ui_left_drawer_open(F.ui));
  CT_CHECK(F.haptic_calls > haptics_before);

  // Programmatic close, then right-edge swipe opens right member drawer.
  ccord_ui_set_left_drawer(F.ui, false);
  step_frames(10, 40);
  CT_CHECK(!ccord_ui_left_drawer_open(F.ui));

  touch_ev(CCORD_IN_TOUCH_DOWN, 2, 402.0f, 400.0f);
  step_frames(1, 16);
  touch_ev(CCORD_IN_TOUCH_MOVE, 2, 310.0f, 402.0f);
  step_frames(1, 16);
  touch_ev(CCORD_IN_TOUCH_MOVE, 2, 190.0f, 404.0f);
  step_frames(1, 16);
  touch_ev(CCORD_IN_TOUCH_UP, 2, 190.0f, 404.0f);
  step_frames(10, 40);
  CT_CHECK(ccord_ui_right_drawer_open(F.ui));

  ccord_ui_set_right_drawer(F.ui, false);
  step_frames(10, 40);
  CT_CHECK(!ccord_ui_right_drawer_open(F.ui));
}

static void test_mobile_composer_and_send(void)
{
  ccord_id ch = ccord_store_selected_channel(F.store);
  CT_CHECK(ch != CCORD_ID_NONE);
  size_t count_before = ccord_store_message_count(F.store, ch);

  // Tap composer to focus IME.
  ccord_layout_rect comp = F.ui->layout.composer;
  touch_tap(comp.x + comp.w * 0.5f, comp.y + comp.h * 0.5f);
  CT_CHECK(F.ui->touch_text_focused);
  CT_CHECK(F.text_input_active);

  // Type message text -> circular brand Send button appears.
  ccord_input_event tev = {.type = CCORD_IN_TEXT};
  snprintf(tev.text, sizeof tev.text, "Hello from Android!");
  (void)ccord_ui_handle_input(F.ui, &tev);
  step_frames(2, 30);
  CT_CHECK(F.ui->edit.len > 0);
  ccord_rect sbtn = F.ui->mobile_send_btn_rect;
  CT_CHECK(sbtn.w >= 24.0f && sbtn.h >= 24.0f);

  // Tap Send button -> submits message & clears composer.
  touch_tap(sbtn.x + sbtn.w * 0.5f, sbtn.y + sbtn.h * 0.5f);
  step_frames(4, 30);
  CT_CHECK(F.ui->edit.len == 0);
  CT_CHECK(ccord_store_message_count(F.store, ch) > count_before);
}

static void test_mobile_scroll_and_long_press_sheet(void)
{
  // Seed extra messages so the message timeline is guaranteed scrollable.
  ccord_id ch = ccord_store_selected_channel(F.store);
  ccord_error err = {0};
  for (int i = 0; i < 30; i++) {
    char msg[64];
    snprintf(msg, sizeof msg, "Mobile scroll test message #%d", i);
    (void)ccord_session_send_message(F.session, ch, msg, strlen(msg), &err);
  }
  step_frames(6, 30);

  ccord_layout_rect ma = F.ui->layout.message_area;
  float top_before = 0.0f, sz_before = 0.0f;
  ccord_vlist_thumb(&F.ui->vlist, ma.h, &top_before, &sz_before);
  CT_CHECK(F.ui->vlist.stick_bottom);
  // Drag finger downward to scroll up into history, then release with velocity for kinetic fling.
  touch_ev(CCORD_IN_TOUCH_DOWN, 3, ma.x + ma.w * 0.5f, ma.y + 120.0f);
  step_frames(1, 16);
  touch_ev(CCORD_IN_TOUCH_MOVE, 3, ma.x + ma.w * 0.5f, ma.y + 180.0f);
  step_frames(1, 16);
  touch_ev(CCORD_IN_TOUCH_MOVE, 3, ma.x + ma.w * 0.5f, ma.y + 260.0f);
  step_frames(1, 16);
  touch_ev(CCORD_IN_TOUCH_MOVE, 3, ma.x + ma.w * 0.5f, ma.y + 350.0f);
  step_frames(1, 16);
  float top_after_drag = 0.0f, sz_drag = 0.0f;
  ccord_vlist_thumb(&F.ui->vlist, ma.h, &top_after_drag, &sz_drag);
  CT_CHECK(!F.ui->vlist.stick_bottom);
  CT_CHECK(top_after_drag < top_before - 0.02f);
  touch_ev(CCORD_IN_TOUCH_UP, 3, ma.x + ma.w * 0.5f, ma.y + 350.0f);
  step_frames(8, 25);
  // Kinetic momentum should continue gliding past the release position.
  float top_after_fling = 0.0f, sz_fling = 0.0f;
  ccord_vlist_thumb(&F.ui->vlist, ma.h, &top_after_fling, &sz_fling);
  CT_CHECK(top_after_fling < top_after_drag - 0.005f);

  // Long-press on a visible message row -> triggers medium haptic + opens mobile bottom sheet.
  int haptics_before = F.haptic_calls;
  float tx = ma.x + ma.w * 0.5f;
  float ty = ma.y + ma.h * 0.5f;
  touch_ev(CCORD_IN_TOUCH_DOWN, 4, tx, ty);
  step_frames(12, 50);   // 600ms > 420ms TOUCH_LONG_PRESS_MS
  CT_CHECK(F.ui->menu.open);
  CT_CHECK(F.haptic_calls > haptics_before);
  CT_CHECK(F.last_haptic_style == 1);
  touch_ev(CCORD_IN_TOUCH_UP, 4, tx, ty);
  step_frames(2, 30);
  // Mobile bottom sheet should span near full viewport width at the bottom.
  CT_CHECK(F.ui->menu.w >= 340.0f);
  CT_CHECK(F.ui->menu.y + F.ui->menu.h >= 840.0f);

  // Close sheet via Back.
  CT_CHECK(ccord_ui_handle_back(F.ui));
  CT_CHECK(!F.ui->menu.open);
}

static void test_mobile_lightbox_pinch_and_back_cascade(void)
{
  // 2-finger pinch zoom while lightbox is open.
  F.ui->lightbox_open = true;
  F.ui->lightbox_zoom = 1.0f;
  step_frames(1, 16);
  touch_ev(CCORD_IN_TOUCH_DOWN, 10, 150.0f, 400.0f);
  touch_ev(CCORD_IN_TOUCH_DOWN, 11, 250.0f, 400.0f);   // initial distance = 100px
  step_frames(1, 16);
  touch_ev(CCORD_IN_TOUCH_MOVE, 10, 100.0f, 400.0f);
  touch_ev(CCORD_IN_TOUCH_MOVE, 11, 300.0f, 400.0f);   // new distance = 200px -> 2.0x zoom
  step_frames(1, 16);
  CT_CHECK(fabsf(F.ui->lightbox_zoom - 2.0f) < 0.05f);
  touch_ev(CCORD_IN_TOUCH_UP, 11, 300.0f, 400.0f);
  touch_ev(CCORD_IN_TOUCH_UP, 10, 100.0f, 400.0f);
  step_frames(1, 16);
  // Back closes lightbox.
  CT_CHECK(ccord_ui_handle_back(F.ui));
  CT_CHECK(!F.ui->lightbox_open);

  // Full Android Back unwind cascade: IME -> settings detail -> settings -> drawer -> root (false).
  ccord_ui_set_left_drawer(F.ui, true);
  F.ui->settings_open = true;
  F.ui->mobile_settings_detail = true;
  F.ui->touch_text_focused = true;
  ccord_ui_set_keyboard_inset(F.ui, 260.0f);
  int kb_dismiss_before = F.dismiss_kb_calls;

  // Step 1: Back dismisses IME.
  CT_CHECK(ccord_ui_handle_back(F.ui));
  CT_CHECK(!F.ui->touch_text_focused);
  CT_CHECK(F.dismiss_kb_calls > kb_dismiss_before);
  ccord_ui_set_keyboard_inset(F.ui, 0.0f);

  // Step 2: Back pops mobile settings detail back to category list.
  CT_CHECK(ccord_ui_handle_back(F.ui));
  CT_CHECK(F.ui->settings_open);
  CT_CHECK(!F.ui->mobile_settings_detail);

  // Step 3: Back closes settings.
  CT_CHECK(ccord_ui_handle_back(F.ui));
  CT_CHECK(!F.ui->settings_open);

  // Step 4: Back closes left drawer.
  CT_CHECK(ccord_ui_handle_back(F.ui));
  CT_CHECK(!ccord_ui_left_drawer_open(F.ui));

  // Step 5: At root chat, Back returns false so Android minimizes the Activity.
  CT_CHECK(!ccord_ui_handle_back(F.ui));
}

static void test_mobile_copy_paste_and_autofill_login(void)
{
  // 1. Long-press composer on mobile -> Paste from clipboard -> Copy back out.
  snprintf(F.clip, sizeof F.clip, "Pasted on mobile!");
  ccord_layout_rect comp = F.ui->layout.composer;
  touch_ev(CCORD_IN_TOUCH_DOWN, 20, comp.x + comp.w * 0.5f, comp.y + comp.h * 0.5f);
  step_frames(12, 50);
  CT_CHECK(F.ui->menu.open);
  touch_ev(CCORD_IN_TOUCH_UP, 20, comp.x + comp.w * 0.5f, comp.y + comp.h * 0.5f);
  step_frames(2, 30);

  ccord_rect paste_r = {0};
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Paste", &paste_r));
  touch_tap(paste_r.x + paste_r.w * 0.5f, paste_r.y + paste_r.h * 0.5f);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "Pasted on mobile!") == 0);

  // Clear clipboard, long-press composer again, tap Copy -> clipboard gets composer text.
  F.clip[0] = '\0';
  touch_ev(CCORD_IN_TOUCH_DOWN, 21, comp.x + comp.w * 0.5f, comp.y + comp.h * 0.5f);
  step_frames(12, 50);
  CT_CHECK(F.ui->menu.open);
  touch_ev(CCORD_IN_TOUCH_UP, 21, comp.x + comp.w * 0.5f, comp.y + comp.h * 0.5f);
  step_frames(2, 30);
  ccord_rect copy_r = {0};
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Copy", &copy_r));
  touch_tap(copy_r.x + copy_r.w * 0.5f, copy_r.y + copy_r.h * 0.5f);
  CT_CHECK(strcmp(F.clip, "Pasted on mobile!") == 0);

  // 2. Mobile Login screen (fresh unauthenticated store): input kinds (email/password/OTP),
  //    Android Autofill virtual structure sync, long-press Copy/Paste on login fields, and 2FA popup Back.
  ccord_error err = {0};
  ccord_bus *lbus = ccord_bus_create(&err);
  ccord_store *lstore = ccord_store_create(lbus, &err);
  ccord_mock_config lmc = {.latency_ms = 0, .now_ms = fake_clock, .clock_ud = &F};
  ccord_transport *ltr = ccord_mock_transport_create(&lmc, &err);
  ccord_session *lsess = ccord_session_create(lbus, lstore, ltr, &err);
  CT_CHECK(lbus && lstore && lsess);
  ccord_ui_deps ldeps = {
    .bus = lbus, .store = lstore, .session = lsess, .contrib = F.contrib, .theme = &F.theme,
    .host = {
      .ud = &F,
      .clipboard_set = host_clip_set,
      .clipboard_get = host_clip_get,
      .text_input = host_text_input,
      .text_input_kind = host_text_input_kind,
      .auth_autofill = host_auth_autofill,
      .haptic_impact = host_haptic_impact,
      .dismiss_keyboard = host_dismiss_keyboard,
      .share_text = host_share_text,
    },
    .fixed_now_unix_ms = 1793534400000LL,
  };
  ccord_ui *lui = ccord_ui_create(&ldeps, &err);
  CT_CHECK(lui != nullptr);
  ccord_ui_set_viewport(lui, 412.0f, 915.0f);
  ccord_ui_set_mobile_mode(lui, true);
  ccord_ui_set_touch_mode(lui, true);
  ccord_ui_set_safe_insets(lui, 44.0f, 0.0f, 34.0f, 0.0f);
  ccord_ui_open_account_settings(lui);
  ccord_ui_set_login_tab(lui, 1);
  (void)ccord_ui_update(lui, F.now += 30);

  ccord_ui_settings_debug sd = {0};
  ccord_ui_get_settings_debug(lui, &sd);
  CT_CHECK(sd.login_screen && sd.login_tab == 1);
  CT_CHECK(!sd.account_mfa_required && sd.account_mfa_box.w == 0.0f);

  // Tap Email field -> INPUT_EMAIL (1) + autofill mode=1, active=1.
  ccord_input_event td = {.type = CCORD_IN_TOUCH_DOWN, .finger_id = 30,
                          .x = sd.account_email_box.x + 20.0f, .y = sd.account_email_box.y + 12.0f};
  ccord_input_event tu = {.type = CCORD_IN_TOUCH_UP, .finger_id = 30,
                          .x = sd.account_email_box.x + 20.0f, .y = sd.account_email_box.y + 12.0f};
  (void)ccord_ui_handle_input(lui, &td);
  (void)ccord_ui_update(lui, F.now += 30);
  (void)ccord_ui_handle_input(lui, &tu);
  (void)ccord_ui_update(lui, F.now += 30);
  CT_CHECK(F.text_input_active && F.last_input_kind == (int)CCORD_INPUT_EMAIL);
  CT_CHECK(F.last_autofill_mode == 1 && F.last_autofill_active == 1);

  // Long-press Email field -> mobile Paste bottom sheet -> pastes clipboard text into Email!
  snprintf(F.clip, sizeof F.clip, "user@mobile.test");
  ccord_input_event lp = {.type = CCORD_IN_TOUCH_DOWN, .finger_id = 31,
                          .x = sd.account_email_box.x + 20.0f, .y = sd.account_email_box.y + 12.0f};
  (void)ccord_ui_handle_input(lui, &lp);
  for (int i = 0; i < 12; i++) (void)ccord_ui_update(lui, F.now += 50);
  CT_CHECK(lui->menu.open);
  lp.type = CCORD_IN_TOUCH_UP;
  (void)ccord_ui_handle_input(lui, &lp);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(lui, F.renderer);
  ccord_render_end(F.renderer);
  ccord_rect lpaste = {0};
  CT_CHECK(ccord_ui_context_menu_find(lui, "Paste", &lpaste));
  td = (ccord_input_event){.type = CCORD_IN_TOUCH_DOWN, .finger_id = 32,
                           .x = lpaste.x + lpaste.w * 0.5f, .y = lpaste.y + lpaste.h * 0.5f};
  tu = td; tu.type = CCORD_IN_TOUCH_UP;
  (void)ccord_ui_handle_input(lui, &td);
  (void)ccord_ui_update(lui, F.now += 30);
  (void)ccord_ui_handle_input(lui, &tu);
  (void)ccord_ui_update(lui, F.now += 30);
  const char *em = nullptr, *pw = nullptr, *mfa = nullptr;
  ccord_ui_account_login_fields(lui, &em, &pw, &mfa);
  CT_CHECK(strcmp(em, "user@mobile.test") == 0);

  // Tap Password field -> INPUT_PASSWORD (2) + autofill mode=1, active=2.
  td = (ccord_input_event){.type = CCORD_IN_TOUCH_DOWN, .finger_id = 33,
                           .x = sd.account_password_box.x + 20.0f, .y = sd.account_password_box.y + 12.0f};
  tu = td; tu.type = CCORD_IN_TOUCH_UP;
  (void)ccord_ui_handle_input(lui, &td);
  (void)ccord_ui_update(lui, F.now += 30);
  (void)ccord_ui_handle_input(lui, &tu);
  (void)ccord_ui_update(lui, F.now += 30);
  CT_CHECK(F.text_input_active && F.last_input_kind == (int)CCORD_INPUT_PASSWORD);
  CT_CHECK(F.last_autofill_mode == 1 && F.last_autofill_active == 2);

  // Simulate Bitwarden autofill populating Email + Password directly.
  ccord_ui_set_account_login_field(lui, 1, "bitwarden@example.com");
  ccord_ui_set_account_login_field(lui, 2, "s3cr3t-pass-from-bitwarden");
  ccord_ui_account_login_fields(lui, &em, &pw, &mfa);
  CT_CHECK(strcmp(em, "bitwarden@example.com") == 0);
  CT_CHECK(strcmp(pw, "s3cr3t-pass-from-bitwarden") == 0);

  // Transition to secondary 2FA popup -> INPUT_OTP (3) + autofill mode=2, active=3.
  ccord_ui_set_login_status(lui, "Enter your 6-digit code.", true);
  (void)ccord_ui_update(lui, F.now += 30);
  CT_CHECK(F.last_input_kind == (int)CCORD_INPUT_OTP);
  CT_CHECK(F.last_autofill_mode == 2 && F.last_autofill_active == 3);
  ccord_ui_set_account_login_field(lui, 3, "987654");
  ccord_ui_account_login_fields(lui, &em, &pw, &mfa);
  CT_CHECK(strcmp(mfa, "987654") == 0);

  // Android Back dismisses keyboard first (if focused), then dismisses the 2FA popup back to Email & Password.
  lui->touch_text_focused = false;
  CT_CHECK(ccord_ui_handle_back(lui));
  CT_CHECK(!lui->account_mfa_required);

  ccord_ui_destroy(lui);
  ccord_session_destroy(lsess);
  ccord_store_destroy(lstore);
  ccord_bus_destroy(lbus);
}

static void test_mobile_safe_area_rail_and_background_focus(void)
{
  ccord_ui_set_viewport(F.ui, 412.0f, 915.0f);
  ccord_ui_set_mobile_mode(F.ui, true);
  ccord_ui_set_touch_mode(F.ui, true);
  ccord_ui_set_safe_insets(F.ui, 48.0f, 0.0f, 34.0f, 12.0f);
  ccord_ui_set_left_drawer(F.ui, true);
  for (int i = 0; i < 20; i++) (void)ccord_ui_update(F.ui, F.now += 25);

  // Server rail DM Home icon must sit below safe_insets.top so it is never clipped at the top edge.
  ccord_rect dm_r1 = ui_guild_icon_rect(F.ui, 0);
  CT_CHECK(F.ui->layout.server_list.y >= 47.9f);
  CT_CHECK(dm_r1.y >= F.ui->layout.server_list.y);

  // Updating safe_insets.top must invalidate the rail slot cache and shift icons below the new top inset.
  ccord_ui_set_safe_insets(F.ui, 64.0f, 0.0f, 34.0f, 12.0f);
  (void)ccord_ui_update(F.ui, F.now += 25);
  ccord_rect dm_r2 = ui_guild_icon_rect(F.ui, 0);
  CT_CHECK(F.ui->layout.server_list.y >= 63.9f);
  CT_CHECK(dm_r2.y >= F.ui->layout.server_list.y);
  CT_CHECK(dm_r2.y > dm_r1.y + 15.0f);

  // Draw left drawer, right drawer, and discovery overlay with non-zero safe insets cleanly.
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_render_end(F.renderer);

  ccord_ui_set_left_drawer(F.ui, false);
  ccord_ui_set_right_drawer(F.ui, true);
  for (int i = 0; i < 20; i++) (void)ccord_ui_update(F.ui, F.now += 25);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_render_end(F.renderer);

  ccord_ui_set_right_drawer(F.ui, false);
  F.ui->discovery_open = true;
  (void)ccord_ui_update(F.ui, F.now += 25);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_render_end(F.renderer);
  F.ui->discovery_open = false;

  // Entering background must clear window_focused, pause inline media playback, destroy open media_audio,
  // cancel in-progress voice message recording, and flush dirty settings.
  ccord_input_event focus_ev = {.type = CCORD_IN_FOCUS, .focused = true};
  (void)ccord_ui_handle_input(F.ui, &focus_ev);
  CT_CHECK(F.ui->window_focused);
  F.ui->media_playing = true;
  snprintf(F.ui->media_url, sizeof F.ui->media_url, "https://cdn.discordapp.com/attachments/1/2/clip.mp4");
  {
    ccord_error aerr = {0};
    ccord_audio_config acfg = {.sample_rate = 48000, .channels = 2, .frame_samples = 960, .capture = false};
    F.ui->media_audio = ccord_audio_create(&acfg, &aerr);
    CT_CHECK(F.ui->media_audio != nullptr);
  }
  CT_CHECK(ui_vm_test_begin(F.ui));
  CT_CHECK(F.ui->vm.recording);
  F.ui->settings_save_dirty = true;
  ccord_input_event bg_ev = {.type = CCORD_IN_APP_BACKGROUND};
  (void)ccord_ui_handle_input(F.ui, &bg_ev);
  CT_CHECK(!F.ui->window_focused);
  CT_CHECK(!ui_is_reading(F.ui));
  CT_CHECK(!F.ui->media_playing);
  CT_CHECK(F.ui->media_audio == nullptr);
  CT_CHECK(F.ui->media_stream == nullptr);
  CT_CHECK(F.ui->media_net == nullptr);
  CT_CHECK(F.ui->media_url[0] == '\0');
  CT_CHECK(!F.ui->vm.recording);
  CT_CHECK(!F.ui->settings_save_dirty);

  // Returning to foreground restores window_focused and marks the UI dirty for immediate repaint.
  ccord_input_event fg_ev = {.type = CCORD_IN_APP_FOREGROUND};
  CT_CHECK(ccord_ui_handle_input(F.ui, &fg_ev));
  CT_CHECK(F.ui->window_focused);
  CT_CHECK(F.ui->dirty);
}

static void test_mobile_drawer_occlusion(void)
{
  ccord_ui_set_viewport(F.ui, 390.0f, 844.0f);
  ccord_ui_set_mobile_mode(F.ui, true);
  ccord_ui_set_touch_mode(F.ui, true);
  ccord_ui_set_safe_insets(F.ui, 0.0f, 0.0f, 0.0f, 0.0f);
  ccord_ui_set_left_drawer(F.ui, false);
  ccord_ui_set_right_drawer(F.ui, false);
  F.ui->discovery_open = false;
  step_frames(15, 25);

  // Give self a Server Tag (clan tag) and send a fresh ungrouped message (past the 5-min grouping window)
  // so its Server Tag pill is guaranteed visible in the chat timeline behind the mobile left drawer.
  ccord_error err = {0};
  ccord_id g = ccord_store_selected_guild(F.store);
  ccord_id ch = ccord_store_selected_channel(F.store);
  ccord_id self_id = ccord_store_self_id(F.store);
  const ccord_user *author = ccord_store_user(F.store, self_id);
  CT_CHECK(author != nullptr);
  if (!author) return;

  ccord_user tagged = *author;
  char a_name[128], a_dname[128], a_handle[128], a_status[256], a_avatar[256], a_banner[256], a_bio[512], a_pronouns[128];
  snprintf(a_name, sizeof a_name, "%s", author->name ? author->name : "");
  snprintf(a_dname, sizeof a_dname, "%s", author->display_name ? author->display_name : "");
  snprintf(a_handle, sizeof a_handle, "%s", author->handle ? author->handle : "");
  snprintf(a_status, sizeof a_status, "%s", author->status_text ? author->status_text : "");
  snprintf(a_avatar, sizeof a_avatar, "%s", author->avatar_url ? author->avatar_url : "");
  snprintf(a_banner, sizeof a_banner, "%s", author->banner_url ? author->banner_url : "");
  snprintf(a_bio, sizeof a_bio, "%s", author->bio ? author->bio : "");
  snprintf(a_pronouns, sizeof a_pronouns, "%s", author->pronouns ? author->pronouns : "");
  tagged.name = a_name;
  tagged.display_name = a_dname;
  tagged.handle = a_handle;
  tagged.status_text = a_status;
  tagged.avatar_url = a_avatar;
  tagged.banner_url = a_banner;
  tagged.bio = a_bio;
  tagged.pronouns = a_pronouns;
  snprintf(tagged.clan_tag, sizeof tagged.clan_tag, "MOBS");
  tagged.clan_guild_id = g;
  CT_CHECK(ccord_store_upsert_user(F.store, &tagged, &err) == CCORD_OK);

  F.now += 10ULL * 60ULL * 1000ULL;
  const char *tag_msg = "Server tag occlusion check";
  CT_CHECK(ccord_session_send_message(F.session, ch, tag_msg, strlen(tag_msg), &err) == CCORD_OK);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  step_frames(6, 25);

  // 1. With drawers closed, tapping the Server Tag in chat opens the Server Tag popover, and Back closes it.
  ccord_rect tag_rect = {0};
  CT_CHECK(ui_clan_popover_hit_rect(F.ui, "MOBS", &tag_rect));
  CT_CHECK(tag_rect.w > 0.0f && tag_rect.h > 0.0f);
  touch_tap(tag_rect.x + tag_rect.w * 0.5f, tag_rect.y + tag_rect.h * 0.5f);
  CT_CHECK(ui_clan_popover_is_open(F.ui));
  CT_CHECK(ccord_ui_handle_back(F.ui));
  step_frames(2, 25);
  CT_CHECK(!ui_clan_popover_is_open(F.ui));

  // 2. Open the left drawer (DM / servers side menu): tapping the exact coordinates of the chat Server Tag
  //    underneath must NOT open the Server Tag popover.
  ccord_ui_set_left_drawer(F.ui, true);
  step_frames(15, 25);
  CT_CHECK(F.ui->layout.left_drawer_visible);
  CT_CHECK(tag_rect.x < F.ui->layout.channel_list.x + F.ui->layout.channel_list.w);
  CT_CHECK(!ui_clan_popover_hit_rect(F.ui, "MOBS", nullptr));
  touch_tap(tag_rect.x + tag_rect.w * 0.5f, tag_rect.y + tag_rect.h * 0.5f);
  CT_CHECK(!ui_clan_popover_is_open(F.ui));

  // 3. Even if the drawer flips open before the next frame redraws stale chat hits, input-time occlusion blocks it.
  ccord_ui_set_left_drawer(F.ui, false);
  step_frames(15, 25);
  CT_CHECK(ui_clan_popover_hit_rect(F.ui, "MOBS", &tag_rect));
  F.ui->left_drawer_open = true;
  touch_ev(CCORD_IN_TOUCH_DOWN, 30, tag_rect.x + tag_rect.w * 0.5f, tag_rect.y + tag_rect.h * 0.5f);
  touch_ev(CCORD_IN_TOUCH_UP, 30, tag_rect.x + tag_rect.w * 0.5f, tag_rect.y + tag_rect.h * 0.5f);
  CT_CHECK(!ui_clan_popover_is_open(F.ui));
  F.ui->left_drawer_open = false;
  step_frames(4, 25);

  // 4. Right drawer open also blocks chat Server Tag hits behind it / its scrim.
  ccord_ui_set_right_drawer(F.ui, true);
  step_frames(15, 25);
  CT_CHECK(F.ui->layout.right_drawer_visible);
  CT_CHECK(!ui_clan_popover_hit_rect(F.ui, "MOBS", nullptr));
  touch_tap(tag_rect.x + tag_rect.w * 0.5f, tag_rect.y + tag_rect.h * 0.5f);
  CT_CHECK(!ui_clan_popover_is_open(F.ui));
  ccord_ui_set_right_drawer(F.ui, false);
  step_frames(15, 25);

  // 5. Long-pressing inside the open left drawer (over the user panel, overlapping the composer behind it)
  //    must NOT open a chat/composer context menu underneath.
  ccord_ui_set_left_drawer(F.ui, true);
  step_frames(15, 25);
  ccord_layout_rect up = F.ui->layout.user_panel;
  touch_ev(CCORD_IN_TOUCH_DOWN, 31, up.x + up.w * 0.5f, up.y + up.h * 0.5f);
  step_frames(12, 50);
  CT_CHECK(!F.ui->menu.open);
  touch_ev(CCORD_IN_TOUCH_UP, 31, up.x + up.w * 0.5f, up.y + up.h * 0.5f);
  step_frames(2, 25);

  // 6. With Server Discovery open and the left drawer open over it, tapping the DM Home rail icon works
  //    instead of being swallowed by Discovery behind the drawer.
  F.ui->discovery_open = true;
  step_frames(2, 25);
  ccord_rect dm_r = ui_guild_icon_rect(F.ui, 0);
  CT_CHECK(dm_r.w > 0.0f && dm_r.h > 0.0f);
  touch_tap(dm_r.x + dm_r.w * 0.5f, dm_r.y + dm_r.h * 0.5f);
  CT_CHECK(ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID);
  CT_CHECK(!F.ui->discovery_open);

  // 7. Closing the DM left drawer clears dm_header_plus_rect so tapping chat where '+' used to sit
  //    does not open the New DM modal.
  step_frames(4, 25);
  ccord_rect old_plus = F.ui->dm_header_plus_rect;
  CT_CHECK(old_plus.w > 0.0f && old_plus.h > 0.0f);
  ccord_ui_set_left_drawer(F.ui, false);
  step_frames(15, 25);
  CT_CHECK(F.ui->dm_header_plus_rect.w == 0.0f);
  touch_tap(old_plus.x + old_plus.w * 0.5f, old_plus.y + old_plus.h * 0.5f);
  CT_CHECK(F.ui->modal_kind != UI_MODAL_NEW_DM);

  // 8. A Server Tag inside the open mobile right drawer (1:1 DM profile panel) remains clickable,
  //    and Back closes the Server Tag popover without dismissing the right drawer.
  size_t ndm = ccord_store_channel_count(F.store, CCORD_DM_GUILD_ID);
  ccord_id dm_ch_id = CCORD_ID_NONE;
  ccord_id dm_uid = CCORD_ID_NONE;
  for (size_t i = 0; i < ndm; i++) {
    const ccord_channel *dc = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, i);
    if (dc && !ui_channel_is_group_dm(dc)) {
      const ccord_user *du = ui_dm_recipient(F.ui, dc);
      ccord_id uid = du ? du->id : dc->user_id;
      if (uid != CCORD_ID_NONE && ccord_store_user(F.store, uid) != nullptr) {
        dm_ch_id = dc->id;
        dm_uid = uid;
        break;
      }
    }
  }
  CT_CHECK(dm_ch_id != CCORD_ID_NONE && dm_uid != CCORD_ID_NONE);
  if (dm_ch_id != CCORD_ID_NONE && dm_uid != CCORD_ID_NONE) {
    const ccord_user *du = ccord_store_user(F.store, dm_uid);
    if (du) {
      ccord_user dm_tagged = *du;
      char dn[128], ddn[128], dh[128], ds[256], da[256], db[256], dbio[512], dpr[128];
      snprintf(dn, sizeof dn, "%s", du->name ? du->name : "");
      snprintf(ddn, sizeof ddn, "%s", du->display_name ? du->display_name : "");
      snprintf(dh, sizeof dh, "%s", du->handle ? du->handle : "");
      snprintf(ds, sizeof ds, "%s", du->status_text ? du->status_text : "");
      snprintf(da, sizeof da, "%s", du->avatar_url ? du->avatar_url : "");
      snprintf(db, sizeof db, "%s", du->banner_url ? du->banner_url : "");
      snprintf(dbio, sizeof dbio, "%s", du->bio ? du->bio : "");
      snprintf(dpr, sizeof dpr, "%s", du->pronouns ? du->pronouns : "");
      dm_tagged.name = dn;
      dm_tagged.display_name = ddn;
      dm_tagged.handle = dh;
      dm_tagged.status_text = ds;
      dm_tagged.avatar_url = da;
      dm_tagged.banner_url = db;
      dm_tagged.bio = dbio;
      dm_tagged.pronouns = dpr;
      snprintf(dm_tagged.clan_tag, sizeof dm_tagged.clan_tag, "DMTG");
      dm_tagged.clan_guild_id = g;
      CT_CHECK(ccord_store_upsert_user(F.store, &dm_tagged, &err) == CCORD_OK);
    }
    F.ui->dm_friends_view = false;
    F.ui->dm_requests_view = false;
    CT_CHECK(ccord_session_select_channel(F.session, dm_ch_id, &err) == CCORD_OK);
    ccord_ui_set_right_drawer(F.ui, true);
    step_frames(15, 25);
    const ccord_user_profile *dprof = ccord_store_user_profile(F.store, dm_uid);
    const char *want_dm_tag = (dprof && dprof->clan_tag[0]) ? dprof->clan_tag : "DMTG";
    ccord_rect dm_tag = {0};
    CT_CHECK(ui_clan_popover_hit_rect(F.ui, want_dm_tag, &dm_tag));
    touch_tap(dm_tag.x + dm_tag.w * 0.5f, dm_tag.y + dm_tag.h * 0.5f);
    CT_CHECK(ui_clan_popover_is_open(F.ui));
    CT_CHECK(ccord_ui_handle_back(F.ui));
    step_frames(2, 25);
    CT_CHECK(!ui_clan_popover_is_open(F.ui));
    CT_CHECK(ccord_ui_right_drawer_open(F.ui));
    ccord_ui_set_right_drawer(F.ui, false);
    step_frames(15, 25);
  }
}

static void test_mobile_anim_pacing(void)
{
  // 16x8 2-frame GIF (100 ms per frame): verifies animated avatars/GIFs pace redraws to frame deadlines
  // instead of locking the main loop at 60-120 FPS (timeout = 0).
  static const uint8_t k_anim_gif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0xff, 0xff, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x04,
    0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x15,
    0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1,
    0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x05, 0x0a, 0x00, 0x02, 0x00, 0x2c, 0x02,
    0x00, 0x02, 0x00, 0x04, 0x00, 0x04, 0x00, 0x81, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x09, 0x00, 0x03, 0x08, 0x1c, 0x48, 0xb0, 0x60, 0x80, 0x80, 0x00,
    0x3b,
  };
  ccord_ui_set_viewport(F.ui, 412.0f, 915.0f);
  ccord_ui_set_mobile_mode(F.ui, true);
  ccord_ui_set_touch_mode(F.ui, true);
  ccord_ui_set_left_drawer(F.ui, false);
  ccord_ui_set_right_drawer(F.ui, false);

  // Select a guild channel with messages and give a visible chat author an animated avatar texture.
  ccord_error err;
  const ccord_guild *g0 = ccord_store_guild_at(F.store, 0);
  CT_CHECK(g0 != nullptr);
  if (g0) {
    F.ui->dm_friends_view = false;
    F.ui->dm_requests_view = false;
    CT_CHECK(ccord_session_select_guild(F.session, g0->id, &err) == CCORD_OK);
    step_frames(5, 25);
  }
  ccord_id ch = ccord_store_selected_channel(F.store);
  size_t nm = ccord_store_message_count(F.store, ch);
  ccord_id author_id = CCORD_ID_NONE;
  for (size_t i = nm; i > 0; i--) {
    const ccord_message *m = ccord_store_message_at(F.store, ch, i - 1);
    if (m && m->author_id != CCORD_ID_NONE) {
      author_id = m->author_id;
      break;
    }
  }
  CT_CHECK(author_id != CCORD_ID_NONE);
  const ccord_user *u = author_id != CCORD_ID_NONE ? ccord_store_user(F.store, author_id) : nullptr;
  CT_CHECK(u != nullptr);
  if (!u) return;
  const char *anim_url = "https://cdn.discordapp.com/avatars/1002/a_pacing_test.gif";
  ccord_user upd = *u;
  char n[128], dn[128], h[128], s[256], b[256], bio[512], pr[128];
  snprintf(n, sizeof n, "%s", u->name ? u->name : "");
  snprintf(dn, sizeof dn, "%s", u->display_name ? u->display_name : "");
  snprintf(h, sizeof h, "%s", u->handle ? u->handle : "");
  snprintf(s, sizeof s, "%s", u->status_text ? u->status_text : "");
  snprintf(b, sizeof b, "%s", u->banner_url ? u->banner_url : "");
  snprintf(bio, sizeof bio, "%s", u->bio ? u->bio : "");
  snprintf(pr, sizeof pr, "%s", u->pronouns ? u->pronouns : "");
  upd.name = n;
  upd.display_name = dn;
  upd.handle = h;
  upd.status_text = s;
  upd.avatar_url = anim_url;
  upd.banner_url = b;
  upd.bio = bio;
  upd.pronouns = pr;
  CT_CHECK(ccord_store_upsert_user(F.store, &upd, &err) == CCORD_OK);
  ccord_media_opts opts = {.max_dim = 128};
  CT_CHECK(ccord_render_upload_media(F.renderer, anim_url, k_anim_gif, sizeof k_anim_gif, &opts, nullptr, nullptr, &err) == CCORD_OK);

  // Settle all smooth UI transitions first, then draw one frame at F.now.
  step_frames(20, 40);
  F.now += 100u;
  (void)ccord_ui_update(F.ui, F.now);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_render_end(F.renderer);

  CT_CHECK(F.ui->media_animating);
  uint32_t wait = ccord_ui_next_wait_ms(F.ui, F.now);
  CT_CHECK(wait > 0u && wait <= 100u);
  // Before the next frame boundary, update must not request a redundant redraw.
  if (wait > 15u) {
    CT_CHECK(!ccord_ui_update(F.ui, F.now + 10u));
    uint32_t wait2 = ccord_ui_next_wait_ms(F.ui, F.now + 10u);
    CT_CHECK(wait2 == wait - 10u);
  }
  // At the exact frame transition deadline, update requests a redraw so the GIF advances on schedule.
  CT_CHECK(ccord_ui_update(F.ui, F.now + wait));
}

int main(void)
{
  test_mobile_layout_geometry();
  if (!fixture_init()) {
    fprintf(stderr, "test_ui_mobile: fixture_init failed\n");
    return 1;
  }
  test_mobile_drawers_and_gestures();
  test_mobile_composer_and_send();
  test_mobile_scroll_and_long_press_sheet();
  test_mobile_lightbox_pinch_and_back_cascade();
  test_mobile_copy_paste_and_autofill_login();
  test_mobile_safe_area_rail_and_background_focus();
  test_mobile_drawer_occlusion();
  test_mobile_anim_pacing();
  fixture_destroy();
  return CT_RESULT("test_ui_mobile");
}
