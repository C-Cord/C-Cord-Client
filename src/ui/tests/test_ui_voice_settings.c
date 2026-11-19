// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <ftw.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int ccord_test_unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(fpath);
}
static char *ccord_test_data_dir = NULL;
static void ccord_test_cleanup_dir(void) {
    if (ccord_test_data_dir) {
        nftw(ccord_test_data_dir, ccord_test_unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
        free(ccord_test_data_dir);
        ccord_test_data_dir = NULL;
    }
}
[[maybe_unused]] static const char *ccord_test_init_dir(const char *prefix) {
    if (ccord_test_data_dir) return ccord_test_data_dir;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    size_t len = strlen(tmp) + strlen(prefix) + 16;
    ccord_test_data_dir = malloc(len);
    if (!ccord_test_data_dir) abort();
    snprintf(ccord_test_data_dir, len, "%s/%s_XXXXXX", tmp, prefix);
    if (!mkdtemp(ccord_test_data_dir)) {
        free(ccord_test_data_dir);
        ccord_test_data_dir = NULL;
        abort();
    }
    atexit(ccord_test_cleanup_dir);
    return ccord_test_data_dir;
}

// M5 Voice & Video settings (m5-ui-settings): real device selectors, live PTT scancode capture,
// sensitivity slider, speexdsp-gated DSP toggles, INI persistence (devices / scancode / per-user
// volumes) and the RTC/DAVE diagnostics row. Runs headless (dummy SDL drivers): one synthetic audio
// device per direction, zero cameras. The M5 controls live below the fold (rows 9-12), reached via
// the page scrollbar; their rects come from the ui_settings_voice_m5_debug seam in settings.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/event_bus.h>
#include <ccord/core/paths.h>
#include <ccord/model/contrib.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>
#include <ccord/net/voice_dsp.h>
#include <ccord/platform/platform.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"

#include "ccord_test.h"

// Test seam implemented in settings.c (not in a public header).
void ui_settings_voice_m5_debug(const ccord_ui *ui, ccord_rect *out_camera, ccord_rect *out_ptt_key,
                                ccord_rect *out_sens_track, ccord_rect *out_scroll_up, ccord_rect *out_scroll_down,
                                ccord_rect *out_diag, float *out_scroll, float *out_max_scroll, bool *out_in_voice);

#define DATA_DIR "voice_settings_test_data"

typedef struct fixture {
  ccord_theme theme;
  ccord_bus *bus;
  ccord_store *store;
  ccord_session *session;
  ccord_transport *transport;
  ccord_contrib *contrib;
  ccord_platform *platform;
  ccord_renderer *renderer;
  ccord_ui *ui;
  uint64_t now;
} fixture;

static fixture F;

static uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

static void frame(int n)
{
  for (int i = 0; i < n; i++) {
    F.now += 50;
    (void)ccord_session_pump(F.session);
    (void)ccord_bus_drain(F.bus);
    (void)ccord_ui_update(F.ui, F.now);
    ccord_render_begin(F.renderer, F.theme.c.bg_primary);
    ccord_ui_draw(F.ui, F.renderer);
    ccord_render_end(F.renderer);
  }
}

static void send(ccord_input_event ev)
{
  (void)ccord_ui_handle_input(F.ui, &ev);
}

static void mouse(ccord_input_type t, uint8_t button, float x, float y)
{
  send((ccord_input_event){.type = t, .button = button, .clicks = 1, .x = x, .y = y});
}

static void click(float x, float y)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  frame(2);
}

static void key(ccord_key k, uint8_t mods)
{
  send((ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = k, .mods = mods});
  frame(1);
}

static ccord_ui_settings_debug sdbg(void)
{
  ccord_ui_settings_debug s;
  ccord_ui_get_settings_debug(F.ui, &s);
  return s;
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_voice_settings_data"));
remove(DATA_DIR "/settings.ini");
  remove(DATA_DIR "/drafts.json");
  remove(DATA_DIR);
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK) return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 50, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session || !F.transport) return false;
  ccord_platform_config pc = {.title = "voice-settings-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .fixed_now_unix_ms = 1793534400000LL};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  if (ccord_session_start(F.session, &err) != CCORD_OK) return false;
  frame(6);   // let the mock snapshot land so self_id is set (no login screen)
  return true;
}

// Open Settings via the user-panel gear and switch to the Voice & Video page.
// Open Settings (if not already open) and switch to the Voice & Video page. Idempotent: when a
// previous test left the modal open, clicking the user-panel gear again would land on the modal
// backdrop and close it, so only click the gear while settings is closed.
static void goto_voice(void)
{
  ccord_ui_settings_debug s = sdbg();
  if (!s.open) {
    const ccord_layout *l = ccord_ui_get_layout(F.ui);
    click(l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
    s = sdbg();
    CT_CHECK(s.open);
  }
  click(s.tabs[3].x + 10.0f, s.tabs[3].y + 10.0f);
  frame(1);
  CT_CHECK(sdbg().page == 3);
}

// Scroll the Voice page to the bottom so rows 9-12 (camera / PTT / sensitivity / diagnostics) show.
static void scroll_voice_bottom(void)
{
  for (int i = 0; i < 8; i++) {
    ccord_rect up, down;
    float scroll, max;
    ui_settings_voice_m5_debug(F.ui, nullptr, nullptr, nullptr, &up, &down, nullptr, &scroll, &max, nullptr);
    if (scroll >= max) break;
    click(down.x + down.w * 0.5f, down.y + down.h * 0.5f);
  }
}

// Scroll back to the top so rows 0-8 (devices / mode / DSP toggles) are on screen again. The page
// scroll offset persists across tests, so each test positions it explicitly.
static void scroll_voice_top(void)
{
  for (int i = 0; i < 8; i++) {
    ccord_rect up, down;
    float scroll;
    ui_settings_voice_m5_debug(F.ui, nullptr, nullptr, nullptr, &up, &down, nullptr, &scroll, nullptr, nullptr);
    if (scroll <= 0.0f) break;
    click(up.x + up.w * 0.5f, up.y + up.h * 0.5f);
  }
}

static ccord_id find_voice_channel(ccord_id *out_guild)
{
  for (size_t g = 0; g < ccord_store_guild_count(F.store); g++) {
    const ccord_guild *gu = ccord_store_guild_at(F.store, g);
    if (!gu || gu->id == CCORD_DM_GUILD_ID) continue;
    for (size_t c = 0; c < ccord_store_channel_count(F.store, gu->id); c++) {
      const ccord_channel *ch = ccord_store_channel_at(F.store, gu->id, c);
      if (ch && ch->kind == CCORD_CHANNEL_VOICE) {
        *out_guild = gu->id;
        return ch->id;
      }
    }
  }
  return CCORD_ID_NONE;
}

// ---- device selectors --------------------------------------------------------------------------

static void test_device_selectors(void)
{
  goto_voice();
  ccord_ui_settings_debug s = sdbg();
  // Default selected initially; the id field is empty.
  CT_CHECK(s.mic_device_value == 0 && s.speaker_device_value == 0);
  CT_CHECK(F.ui->settings_mic_device_id[0] == '\0');

  // One synthetic recording device under the dummy driver: Default + 1 entry, so one click selects
  // it (index 1) and stores its id; a second click wraps back to Default (index 0, empty id).
  click(s.input_device.x + 20.0f, s.input_device.y + 12.0f);
  s = sdbg();
  CT_CHECK(s.mic_device_value == 1);
  CT_CHECK(F.ui->settings_mic_device_id[0] != '\0');
  click(s.input_device.x + 20.0f, s.input_device.y + 12.0f);
  s = sdbg();
  CT_CHECK(s.mic_device_value == 0);
  CT_CHECK(F.ui->settings_mic_device_id[0] == '\0');

  click(s.output_device.x + 20.0f, s.output_device.y + 12.0f);
  s = sdbg();
  CT_CHECK(s.speaker_device_value == 1);
  CT_CHECK(F.ui->settings_speaker_device_id[0] != '\0');

  // The getter seam reports the same selection to the app media pump.
  ccord_ui_voice_prefs prefs;
  ccord_ui_voice_settings(F.ui, &prefs);
  CT_CHECK(strcmp(prefs.output_device_id, F.ui->settings_speaker_device_id) == 0);

  // Camera: no cameras under the dummy driver -> the selector is inert (stays Default).
  scroll_voice_bottom();
  ccord_rect cam;
  ui_settings_voice_m5_debug(F.ui, &cam, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  click(cam.x + 20.0f, cam.y + 12.0f);
  CT_CHECK(F.ui->settings_camera_device == 0);
  CT_CHECK(F.ui->settings_camera_device_id[0] == '\0');
}

// ---- PTT mode + live scancode capture ----------------------------------------------------------

static void test_ptt_capture(void)
{
  goto_voice();
  scroll_voice_top();
  ccord_ui_settings_debug s = sdbg();
  CT_CHECK(s.voice_mode_value == 0);   // voice activity by default

  // Switch to Push to Talk; the choice is forwarded to the session/transport.
  click(s.voice_mode[1].x + 12.0f, s.voice_mode[1].y + 12.0f);
  s = sdbg();
  CT_CHECK(s.voice_mode_value == 1);
  ccord_voice_config cfg;
  CT_CHECK(ccord_mock_voice_config(F.transport, &cfg) && cfg.ptt);

  // Arm live capture on the PTT key row, then press V: the SDL scancode is stored.
  scroll_voice_bottom();
  ccord_rect ptt;
  ui_settings_voice_m5_debug(F.ui, nullptr, &ptt, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  CT_CHECK(F.ui->settings_ptt_scancode == 0);   // unbound
  click(ptt.x + 20.0f, ptt.y + 12.0f);
  CT_CHECK(F.ui->account_focus_field == 5);      // capture armed (key routing sentinel)
  key(CCORD_KEY_V, 0);
  CT_CHECK(F.ui->settings_ptt_scancode == SDL_SCANCODE_V);
  CT_CHECK(F.ui->account_focus_field == 0);      // disarmed after binding

  // Rebind to a different key.
  click(ptt.x + 20.0f, ptt.y + 12.0f);
  key(CCORD_KEY_T, 0);
  CT_CHECK(F.ui->settings_ptt_scancode == SDL_SCANCODE_T);

  // Arming then clicking elsewhere cancels capture without binding.
  click(ptt.x + 20.0f, ptt.y + 12.0f);
  CT_CHECK(F.ui->account_focus_field == 5);
  click(ptt.x + 200.0f, ptt.y - 40.0f);   // a click off the row
  CT_CHECK(F.ui->account_focus_field == 0);
  CT_CHECK(F.ui->settings_ptt_scancode == SDL_SCANCODE_T);   // unchanged

  // Back to voice activity; the forwarded config follows. The mode chips are row 4, so return to
  // the top of the page before clicking them.
  scroll_voice_top();
  s = sdbg();
  click(s.voice_mode[0].x + 12.0f, s.voice_mode[0].y + 12.0f);
  CT_CHECK(ccord_mock_voice_config(F.transport, &cfg) && !cfg.ptt);
}

// ---- sensitivity slider ------------------------------------------------------------------------

static void test_sensitivity(void)
{
  goto_voice();
  // Sensitivity only applies to voice activity; make sure that mode is selected (row 4, at top).
  scroll_voice_top();
  ccord_ui_settings_debug s = sdbg();
  click(s.voice_mode[0].x + 12.0f, s.voice_mode[0].y + 12.0f);
  scroll_voice_bottom();
  ccord_rect sens;
  ui_settings_voice_m5_debug(F.ui, nullptr, nullptr, &sens, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  // Click at ~75% of the track.
  click(sens.x + sens.w * 0.75f, sens.y + sens.h * 0.5f);
  int v = F.ui->settings_vad_sensitivity;
  CT_CHECK(v >= 70 && v <= 80);
  ccord_voice_config cfg;
  CT_CHECK(ccord_mock_voice_config(F.transport, &cfg) && cfg.vad_sensitivity_pct == (uint32_t)v);

  // The minus button steps down by 5 (it sits 34px left and 8px above the track origin).
  ccord_rect minus = {sens.x - 34.0f, sens.y - 8.0f, 24.0f, 24.0f};
  click(minus.x + 12.0f, minus.y + 12.0f);
  CT_CHECK(F.ui->settings_vad_sensitivity == v - 5);
}

// ---- DSP gating + forwarding -------------------------------------------------------------------

static void test_dsp_gating(void)
{
  goto_voice();
  scroll_voice_top();
  ccord_ui_settings_debug s = sdbg();
  bool dsp = ccord_dsp_available();
  // On this host speexdsp is linked, so the toggles are live. (The !dsp branch is compile-time gated.)
  CT_CHECK(dsp);
  bool ns0 = s.noise_suppression;
  // NS is row 5, hardware acceleration is row 8: same right-edge x, three 54px rows up.
  ccord_rect ns = s.hardware_toggle;
  ns.y -= 3.0f * 54.0f;
  click(ns.x + ns.w - 10.0f, ns.y + ns.h * 0.5f);
  s = sdbg();
  CT_CHECK(s.noise_suppression == !ns0);
  ccord_voice_config cfg;
  CT_CHECK(ccord_mock_voice_config(F.transport, &cfg) && cfg.ns == s.noise_suppression);
}

// ---- INI round-trip ----------------------------------------------------------------------------

static void test_ini_round_trip(void)
{
  goto_voice();
  ccord_ui_settings_debug s = sdbg();
  // Select the synthetic input device, bind a PTT key, set sensitivity and per-user volumes.
  click(s.input_device.x + 20.0f, s.input_device.y + 12.0f);
  scroll_voice_bottom();
  ccord_rect ptt, sens;
  ui_settings_voice_m5_debug(F.ui, nullptr, &ptt, &sens, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  click(ptt.x + 20.0f, ptt.y + 12.0f);
  key(CCORD_KEY_B, 0);
  click(sens.x + sens.w * 0.30f, sens.y + sens.h * 0.5f);
  ccord_ui_set_user_volume(F.ui, 777001, 150);
  ccord_ui_set_user_volume(F.ui, 777002, 0);     // muted
  ccord_ui_set_user_volume(F.ui, 777003, 200);
  ui_settings_save(F.ui);

  int mic_idx = F.ui->settings_mic_device;
  char mic_id[64];
  snprintf(mic_id, sizeof mic_id, "%s", F.ui->settings_mic_device_id);
  int scancode = F.ui->settings_ptt_scancode;
  int sensitivity = F.ui->settings_vad_sensitivity;
  CT_CHECK(scancode == SDL_SCANCODE_B);

  // Recreate the UI: ccord_ui_create reloads the INI from the same data dir.
  ccord_ui_destroy(F.ui);
  ccord_error err;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .fixed_now_unix_ms = 1793534400000LL};
  F.ui = ccord_ui_create(&d, &err);
  CT_CHECK(F.ui != nullptr);
  ccord_ui_set_viewport(F.ui, 1280, 800);
  frame(2);

  CT_CHECK(F.ui->settings_mic_device == mic_idx);
  CT_CHECK(strcmp(F.ui->settings_mic_device_id, mic_id) == 0);
  CT_CHECK(F.ui->settings_ptt_scancode == scancode);
  CT_CHECK(F.ui->settings_vad_sensitivity == sensitivity);
  CT_CHECK(ccord_ui_user_volume(F.ui, 777001) == 150);
  CT_CHECK(ccord_ui_user_volume(F.ui, 777002) == 0);
  CT_CHECK(ccord_ui_user_volume(F.ui, 777003) == 200);
  CT_CHECK(ccord_ui_user_volume(F.ui, 777999) == 100);   // untouched default
}

// ---- RTC diagnostics + DAVE status -------------------------------------------------------------

static void test_rtc_dave(void)
{
  goto_voice();
  ccord_error err;
  ccord_id guild = CCORD_ID_NONE;
  ccord_id voice = find_voice_channel(&guild);
  CT_CHECK(voice != CCORD_ID_NONE);
  CT_CHECK(ccord_session_join_voice(F.session, guild, voice, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_store_active_voice_channel(F.store) == voice);

  // Seed the snapshots the app media pump would deliver (the UI reads them via the session).
  ccord_rtc_stats rtc = {.state = CCORD_VOICE_ESTABLISHED, .ssrc = 4242, .ping_ms = 37, .jitter_depth = 2,
                         .packets_lost = 5, .bitrate_kbps = 96, .dave_epoch = 9};
  ccord_dave_status dave = {.active = true, .verified = false, .protocol_version = 1};
  CT_CHECK(ccord_store_set_rtc_stats(F.store, &rtc, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_dave_status(F.store, &dave, &err) == CCORD_OK);

  ccord_rtc_stats got_rtc;
  ccord_dave_status got_dave;
  ccord_session_voice_stats(F.session, &got_rtc);
  ccord_session_dave_status(F.session, &got_dave);
  CT_CHECK(got_rtc.ping_ms == 37 && got_rtc.ssrc == 4242 && got_rtc.dave_epoch == 9);
  CT_CHECK(got_dave.active && !got_dave.verified && got_dave.protocol_version == 1);

  // The diagnostics card is drawn while in voice.
  scroll_voice_bottom();
  ccord_rect diag;
  bool in_voice = false;
  ui_settings_voice_m5_debug(F.ui, nullptr, nullptr, nullptr, nullptr, nullptr, &diag, nullptr, nullptr, &in_voice);
  CT_CHECK(in_voice);
  CT_CHECK(diag.w > 0.0f && diag.h > 0.0f);
  frame(2);   // draw with the diagnostics populated (must not crash)

  // Leaving voice zeroes the snapshots the panel reads.
  CT_CHECK(ccord_session_leave_voice(F.session, guild, &err) == CCORD_OK);
  frame(4);
  ccord_session_voice_stats(F.session, &got_rtc);
  ccord_session_dave_status(F.session, &got_dave);
  CT_CHECK(got_rtc.ping_ms == 0 && got_rtc.ssrc == 0);
  CT_CHECK(!got_dave.active);
  ui_settings_voice_m5_debug(F.ui, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &in_voice);
  CT_CHECK(!in_voice);
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_voice_settings")) {
    fprintf(stderr, "voice settings test: setup failed or no display driver; skipping\n");
    return 77;
  }
  test_device_selectors();
  test_ptt_capture();
  test_sensitivity();
  test_dsp_gating();
  test_ini_round_trip();
  test_rtc_dave();
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_contrib_destroy(F.contrib);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
  remove(DATA_DIR "/settings.ini");
  remove(DATA_DIR "/drafts.json");
  remove(DATA_DIR);
  return CT_RESULT("test_ui_voice_settings");
}
