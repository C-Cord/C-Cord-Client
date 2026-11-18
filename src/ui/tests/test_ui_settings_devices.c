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

// M6 account settings pages (m6-ui-devices): Privacy & Safety governed PATCHes, Devices lazy
// fetch + per-device / all-other logout, Connections lazy fetch + edit/delete, and the
// discord.com browser redirects for the excluded billing/account actions. Real store + mock
// transport + session + renderer, fake OS host, injected input events. Discord state must only
// change after the mock answers: every action is asserted before and after the pump.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/event_bus.h>
#include <ccord/core/paths.h>
#include <ccord/model/contrib.h>
#include <ccord/net/admin.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"

#include "../settings_m6_devices.h"

#include "ccord_test.h"

#define DATA_DIR "ui_settings_devices_data"

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
  int open_url_calls;
  char last_url[256];
} fixture;

static fixture F;

static uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

static void host_clip_set(void *ud, const char *s) { (void)ud; (void)s; }
static bool host_open_url(void *ud, const char *url)
{
  (void)ud;
  F.open_url_calls++;
  snprintf(F.last_url, sizeof F.last_url, "%s", url);
  return true;
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

static void send(ccord_input_event ev) { (void)ccord_ui_handle_input(F.ui, &ev); }

static void mouse(ccord_input_type t, uint8_t button, float x, float y)
{
  send((ccord_input_event){.type = t, .button = button, .clicks = 1, .x = x, .y = y});
}

// Click without pumping: lets a test assert the request went out and nothing changed locally yet.
static void click_raw(float x, float y)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
}

static void click(float x, float y)
{
  click_raw(x, y);
  frame(2);
}

static void click_rect(ccord_rect r)
{
  click(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}

static ccord_ui_settings_debug sdbg(void)
{
  ccord_ui_settings_debug s;
  ccord_ui_get_settings_debug(F.ui, &s);
  return s;
}

static ccord_settings_m6_debug m6dbg(void)
{
  ccord_settings_m6_debug d;
  ui_settings_m6_get_debug(F.ui, &d);
  return d;
}

static ccord_ui_debug udbg(void)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(F.ui, &d);
  return d;
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_settings_devices_data"));
remove(DATA_DIR "/settings.ini");
  remove(DATA_DIR "/drafts.json");
  remove(DATA_DIR);
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK)
    return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 1, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session || !F.transport) return false;
  ccord_platform_config pc = {.title = "settings-devices-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .host = {.clipboard_set = host_clip_set, .open_url = host_open_url},
                     .fixed_now_unix_ms = 1793534400000LL};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  if (ccord_session_start(F.session, &err) != CCORD_OK) return false;
  frame(6);   // let the mock snapshot land so self_id is set (no login screen)
  return true;
}

static void teardown(void)
{
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
  ccord_contrib_destroy(F.contrib);
  F.ui = nullptr;
  F.session = nullptr;
  F.transport = nullptr;
}

// Open Settings (if closed) via the user-panel gear.
static void open_settings(void)
{
  if (sdbg().open) return;
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  click(l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
  CT_CHECK(sdbg().open);
}

static void goto_m6_page(int page)
{
  open_settings();
  ccord_rect tabs[CCORD_M6_PAGE_COUNT];
  ui_settings_m6_host_rects(F.ui, tabs, nullptr);
  click_raw(tabs[page].x + 10.0f, tabs[page].y + 10.0f);
  frame(1);   // draw the page once: list fetches go out on this frame, answers land on the NEXT pump
  CT_CHECK(sdbg().page == 9 + page);   // SP_PRIVACY/SP_DEVICES/SP_CONNECTIONS append after the legacy 9
}

static const ccord_admin_request *last_admin(void)
{
  const ccord_admin_request *r = ccord_mock_last_admin(F.transport);
  static const ccord_admin_request empty;   // zeroed (op NONE) so a missing request fails checks, not segfaults
  return r ? r : &empty;
}

// ------------------------------------------------------------------ Privacy & Safety

static void test_privacy_page(void)
{
  goto_m6_page(CCORD_M6_PAGE_PRIVACY);
  ccord_settings_m6_debug d = m6dbg();
  CT_CHECK(d.drawn_page == CCORD_M6_PAGE_PRIVACY);
  CT_CHECK(d.dm_toggle.w > 0.0f && d.filter_chips[2].w > 0.0f && d.age_toggle.w > 0.0f);
  // The privacy page never fetches the account lists.
  CT_CHECK(ccord_mock_admin_count(F.transport) == 0);
  // No settings object has arrived yet: the page shows Discord defaults, not a fabricated store write.
  CT_CHECK(ccord_store_user_settings(F.store) == nullptr);

  // Friend requests "everyone" defaults ON before any answer arrives: the first click must
  // PATCH 0; the restore click puts it back for the friend-source section below.
  click_raw(d.friend_all_toggle.x + d.friend_all_toggle.w - 8.0f, d.friend_all_toggle.y + 12.0f);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_USER_SETTINGS_PATCH);
  CT_CHECK(last_admin()->settings_patch.friend_source_all == 0);
  CT_CHECK(ccord_store_user_settings(F.store) == nullptr);   // no optimistic write before the first answer
  frame(2);
  click_rect(m6dbg().friend_all_toggle);
  CT_CHECK(last_admin()->settings_patch.friend_source_all == 1);
  frame(2);

  // Allow-DMs toggle is inverted: default_guilds_restricted false -> toggle on -> PATCH 1.
  click_raw(d.dm_toggle.x + d.dm_toggle.w - 8.0f, d.dm_toggle.y + d.dm_toggle.h * 0.5f);
  CT_CHECK(ccord_mock_admin_count(F.transport) == 3);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_USER_SETTINGS_PATCH);
  CT_CHECK(last_admin()->settings_patch.default_guilds_restricted == 1);
  CT_CHECK(last_admin()->settings_patch.explicit_content_filter == -1);
  CT_CHECK(last_admin()->settings_patch.friend_source_all == -1);
  // No optimistic mutation before the answer: the confirmed DM flag is still off.
  const ccord_user_settings *pre = ccord_store_user_settings(F.store);
  CT_CHECK(pre && !pre->default_guilds_restricted);
  frame(2);
  const ccord_user_settings *us = ccord_store_user_settings(F.store);
  CT_CHECK(us != nullptr);
  if (us) CT_CHECK(us->default_guilds_restricted);
  ccord_user_settings mock;
  CT_CHECK(ccord_mock_user_settings(F.transport, &mock) && mock.default_guilds_restricted);

  // Toggle back: the confirmed value is the base for the next patch.
  click_rect(m6dbg().dm_toggle);
  CT_CHECK(last_admin()->settings_patch.default_guilds_restricted == 0);
  frame(2);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(us && !us->default_guilds_restricted);

  // DM spam filter chips: pick "Everyone" (2), then verify the same chip is a no-op.
  click_rect(m6dbg().filter_chips[2]);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_USER_SETTINGS_PATCH);
  CT_CHECK(last_admin()->settings_patch.explicit_content_filter == 2);
  frame(2);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(us && us->explicit_content_filter == 2);
  size_t before = ccord_mock_admin_count(F.transport);
  click_rect(m6dbg().filter_chips[2]);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);

  // Friend-source flags: everyone starts on (Discord default), the other two start off.
  click_rect(m6dbg().friend_all_toggle);
  CT_CHECK(last_admin()->settings_patch.friend_source_all == 0);
  CT_CHECK(last_admin()->settings_patch.friend_source_mutual_friends == -1);
  frame(2);
  click_rect(m6dbg().friend_mutual_toggle);
  CT_CHECK(last_admin()->settings_patch.friend_source_mutual_friends == 1);
  frame(2);
  click_rect(m6dbg().friend_guild_toggle);
  CT_CHECK(last_admin()->settings_patch.friend_source_mutual_guilds == 1);
  frame(2);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(us && !us->friend_source_all && us->friend_source_mutual_friends && us->friend_source_mutual_guilds);

  // A refused PATCH keeps the confirmed value and surfaces the failure as a notice.
  ccord_mock_fail_next_admin(F.transport, true);
  click_rect(m6dbg().age_toggle);
  frame(2);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(us && !us->allow_access_to_age_restricted_servers);
  CT_CHECK(strstr(udbg().notice, "Couldn't") != nullptr);
  click_rect(m6dbg().age_toggle);   // the one-shot failure is consumed; retry succeeds
  frame(2);
  us = ccord_store_user_settings(F.store);
  CT_CHECK(us && us->allow_access_to_age_restricted_servers);
}

// ------------------------------------------------------------------ Devices

static void test_devices_page(void)
{
  size_t before = ccord_mock_admin_count(F.transport);
  goto_m6_page(CCORD_M6_PAGE_DEVICES);
  // The fetch went out on the first drawn frame; the answer has not been pumped yet.
  CT_CHECK(ccord_mock_admin_count(F.transport) == before + 1);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_AUTH_SESSIONS_LIST);
  ccord_settings_m6_debug d = m6dbg();
  CT_CHECK(d.devices_state == CCORD_FETCH_LOADING);
  CT_CHECK(d.device_count == 0);   // nothing rendered before the answer
  frame(2);
  d = m6dbg();
  CT_CHECK(d.devices_state == CCORD_FETCH_READY);
  CT_CHECK(d.device_count == 2);
  // The current session has no Log Out button (the API refuses it); the other one does.
  CT_CHECK(d.device_logout[0].w == 0.0f);
  CT_CHECK(d.device_logout[1].w > 0.0f);
  // The "This device" badge is not a button: clicking its area arms nothing and sends nothing.
  size_t before_cur = ccord_mock_admin_count(F.transport);
  click_raw(d.content.x + d.content.w - 96.0f + 10.0f, d.content.y + 70.0f + 8.0f + 10.0f);
  frame(1);
  CT_CHECK(m6dbg().confirm_kind == 0);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before_cur);

  // Re-visiting within the TTL must not re-fetch.
  goto_m6_page(CCORD_M6_PAGE_PRIVACY);
  goto_m6_page(CCORD_M6_PAGE_DEVICES);
  frame(2);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before + 1);

  // Log Out All Known Devices: two-click confirm, only non-current hashes, post-response removal.
  before = ccord_mock_admin_count(F.transport);
  click_raw(d.logout_all_btn.x + 20.0f, d.logout_all_btn.y + 10.0f);
  frame(1);
  CT_CHECK(m6dbg().confirm_kind == 1);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);   // arming sends nothing
  click_raw(d.logout_all_btn.x + 20.0f, d.logout_all_btn.y + 10.0f);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before + 1);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT);
  CT_CHECK(last_admin()->session_hash_count == 1);
  size_t n = 0;
  const ccord_auth_session *list = ccord_session_auth_sessions(F.session, &n);
  CT_CHECK(n == 2);   // no optimistic drop before the 204 ack
  CT_CHECK(strcmp(last_admin()->session_hashes[0], list[1].id_hash) == 0);
  CT_CHECK(strcmp(last_admin()->session_hashes[0], list[0].id_hash) != 0);   // current never included
  frame(2);
  d = m6dbg();
  CT_CHECK(d.device_count == 1);
  list = ccord_session_auth_sessions(F.session, &n);
  CT_CHECK(n == 1 && list[0].current);

  // With only the current device left the button is inert (no request, honest notice).
  before = ccord_mock_admin_count(F.transport);
  click_rect(m6dbg().logout_all_btn);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);
  CT_CHECK(strstr(udbg().notice, "No other devices") != nullptr);
}

static void test_devices_single_logout(void)
{
  // Fresh mock: two sessions again. The restart also proves ui_settings_m6_reset clears the
  // transient page state (confirmations / scroll) between UI instances.
  teardown();
  CT_CHECK(setup("test_ui_settings_devices"));

  // A failed fetch shows the error state; Retry asks again and succeeds.
  ccord_mock_fail_next_admin(F.transport, true);
  goto_m6_page(CCORD_M6_PAGE_DEVICES);
  frame(2);
  ccord_settings_m6_debug d = m6dbg();
  CT_CHECK(d.devices_state == CCORD_FETCH_FAILED);
  CT_CHECK(d.devices_retry_btn.w > 0.0f);
  CT_CHECK(d.device_count == 0);
  click_rect(d.devices_retry_btn);
  frame(2);
  d = m6dbg();
  CT_CHECK(d.devices_state == CCORD_FETCH_READY);
  CT_CHECK(d.device_count == 2);
  CT_CHECK(d.confirm_kind == 0 && d.scroll == 0.0f);

  // Arm, then click elsewhere: the confirmation cancels without a request.
  size_t before = ccord_mock_admin_count(F.transport);
  click_raw(d.device_logout[1].x + 10.0f, d.device_logout[1].y + 10.0f);
  frame(1);
  CT_CHECK(m6dbg().confirm_kind == 2 && m6dbg().confirm_index == 1);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);
  click_raw(d.content.x + 4.0f, d.content.y + 50.0f);
  frame(1);
  CT_CHECK(m6dbg().confirm_kind == 0);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);

  // A refused logout keeps the row and reports the failure.
  ccord_mock_fail_next_admin(F.transport, true);
  click_raw(d.device_logout[1].x + 10.0f, d.device_logout[1].y + 10.0f);
  frame(1);
  click_raw(d.device_logout[1].x + 10.0f, d.device_logout[1].y + 10.0f);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before + 1);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT);
  frame(2);
  CT_CHECK(m6dbg().device_count == 2);
  CT_CHECK(strstr(udbg().notice, "Couldn't") != nullptr);

  // Successful per-device logout removes exactly that row after the ack.
  click_raw(d.device_logout[1].x + 10.0f, d.device_logout[1].y + 10.0f);
  frame(1);
  click_raw(d.device_logout[1].x + 10.0f, d.device_logout[1].y + 10.0f);
  frame(2);
  d = m6dbg();
  CT_CHECK(d.device_count == 1);
  CT_CHECK(d.device_logout[0].w == 0.0f);   // the survivor is the current session
}

// ------------------------------------------------------------------ Connections

static void test_connections_page(void)
{
  // A failed fetch shows the error state; Retry asks again and succeeds.
  ccord_mock_fail_next_admin(F.transport, true);
  goto_m6_page(CCORD_M6_PAGE_CONNECTIONS);
  frame(2);
  CT_CHECK(m6dbg().connections_state == CCORD_FETCH_FAILED);
  CT_CHECK(m6dbg().connections_retry_btn.w > 0.0f);
  click_rect(m6dbg().connections_retry_btn);
  frame(2);
  size_t before = ccord_mock_admin_count(F.transport);
  ccord_settings_m6_debug d = m6dbg();
  CT_CHECK(d.connections_state == CCORD_FETCH_READY);
  CT_CHECK(d.connection_count == 3);

  // Re-visiting within the TTL must not re-fetch (the Retry answer is fresh).
  goto_m6_page(CCORD_M6_PAGE_PRIVACY);
  goto_m6_page(CCORD_M6_PAGE_CONNECTIONS);
  frame(2);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);

  // Visibility toggle on the steam row: PATCH keyed by service type + id, single field only.
  size_t n = 0;
  const ccord_connection *list = ccord_session_connections(F.session, &n);
  CT_CHECK(n == 3 && list[0].visibility == 1);
  click_raw(d.conn_visibility[0].x + d.conn_visibility[0].w - 8.0f, d.conn_visibility[0].y + 12.0f);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_CONNECTION_EDIT);
  CT_CHECK(strcmp(last_admin()->connection_name, "steam") == 0);
  CT_CHECK(strcmp(last_admin()->connection_id, "76561198000000001") == 0);
  CT_CHECK(last_admin()->connection_edit.visibility == 0);
  CT_CHECK(last_admin()->connection_edit.friend_sync == -1);
  CT_CHECK(last_admin()->connection_edit.show_activity == -1);
  CT_CHECK(ccord_session_connections(F.session, &n)[0].visibility == 1);   // unchanged before the answer
  frame(2);
  list = ccord_session_connections(F.session, &n);
  CT_CHECK(list[0].visibility == 0);

  // Show-activity and friend-sync toggles on the github row.
  d = m6dbg();
  click_rect(d.conn_activity[1]);
  CT_CHECK(last_admin()->connection_edit.show_activity == 0);
  CT_CHECK(strcmp(last_admin()->connection_name, "github") == 0);
  frame(2);
  click_rect(m6dbg().conn_sync[1]);
  CT_CHECK(last_admin()->connection_edit.friend_sync == 1);
  frame(2);
  list = ccord_session_connections(F.session, &n);
  CT_CHECK(!list[1].show_activity && list[1].friend_sync);

  // The list scrolls (3 cards overflow the panel): the fixed scrollbar moves the rows, and
  // hit-testing follows the shifted geometry. Settings is full-window now, so an 800 px window
  // fits all three cards; a 560 px window gives the 512 px content height these checks assume.
  ccord_ui_set_viewport(F.ui, 1280, 560);
  frame(2);
  d = m6dbg();
  CT_CHECK(d.scroll == 0.0f);
  ccord_rect track = {d.content.x + d.content.w + 12.0f, d.content.y + 44.0f, 8.0f, d.content.h - 44.0f};
  ccord_rect down = {track.x - 4.0f, track.y + track.h + 4.0f, 16.0f, 20.0f};
  float y_before = d.conn_visibility[0].y;
  // Raw clicks (no frame between): the click-path scroll clamp must hold before the next draw.
  click_raw(down.x + 8.0f, down.y + 10.0f);
  d = m6dbg();
  CT_CHECK(d.scroll > 0.0f);
  CT_CHECK(d.conn_visibility[0].y < y_before);
  before = ccord_mock_admin_count(F.transport);
  click_raw(d.conn_visibility[0].x + d.conn_visibility[0].w * 0.5f,
            d.conn_visibility[0].y + d.conn_visibility[0].h * 0.5f);   // shifted rect must still hit
  CT_CHECK(ccord_mock_admin_count(F.transport) == before + 1);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_CONNECTION_EDIT);
  CT_CHECK(last_admin()->connection_edit.visibility == 1);   // steam was toggled off earlier
  CT_CHECK(last_admin()->connection_edit.show_activity == -1);   // not the neighbouring row
  frame(2);
  ccord_rect up = {track.x - 4.0f, track.y - 24.0f, 16.0f, 20.0f};
  click(up.x + 8.0f, up.y + 10.0f);
  CT_CHECK(m6dbg().scroll == 0.0f);

  // Down again, then page up by clicking the track above the thumb.
  click(down.x + 8.0f, down.y + 10.0f);
  CT_CHECK(m6dbg().scroll > 0.0f);
  click(track.x + 4.0f, track.y + 2.0f);
  CT_CHECK(m6dbg().scroll == 0.0f);
  click(down.x + 8.0f, down.y + 10.0f);
  CT_CHECK(m6dbg().scroll > 0.0f);
  // The footer button follows the scroll too (still three rows here).
  int urls_scrolled = F.open_url_calls;
  click_rect(m6dbg().conn_add_btn);
  CT_CHECK(F.open_url_calls == urls_scrolled + 1);

  // Disconnect (xbox, revoked row) while the list is scrolled: two-click confirm, DELETE keyed
  // by type + id, post-ack removal. The row-count drop must clamp the stale scroll away.
  d = m6dbg();
  before = ccord_mock_admin_count(F.transport);
  click_raw(d.conn_disconnect[2].x + 10.0f, d.conn_disconnect[2].y + 10.0f);
  frame(1);
  CT_CHECK(m6dbg().confirm_kind == 3 && m6dbg().confirm_index == 2);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);
  click_raw(d.conn_disconnect[2].x + 10.0f, d.conn_disconnect[2].y + 10.0f);
  CT_CHECK(last_admin()->op == CCORD_ADMIN_CONNECTION_DELETE);
  CT_CHECK(strcmp(last_admin()->connection_name, "xbox") == 0);
  CT_CHECK(strcmp(last_admin()->connection_id, "2533274900000001") == 0);
  CT_CHECK(m6dbg().connection_count == 3);   // still three rows before the 204 ack
  frame(2);
  d = m6dbg();
  CT_CHECK(d.connection_count == 2);

  // Adding a new account is an OAuth flow: browser redirect, no local form, no REST request.
  // The footer sits at the clamped (unscrolled) position now that two cards fit.
  before = ccord_mock_admin_count(F.transport);
  int urls = F.open_url_calls;
  click_rect(m6dbg().conn_add_btn);
  CT_CHECK(F.open_url_calls == urls + 1);
  CT_CHECK(strcmp(F.last_url, "https://discord.com/connections") == 0);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);

  // A refused edit keeps the confirmed value.
  list = ccord_session_connections(F.session, &n);
  uint8_t vis = list[0].visibility;
  ccord_mock_fail_next_admin(F.transport, true);
  click_rect(m6dbg().conn_visibility[0]);
  frame(2);
  list = ccord_session_connections(F.session, &n);
  CT_CHECK(list[0].visibility == vis);
  CT_CHECK(strstr(udbg().notice, "Couldn't") != nullptr);

  // Arm a disconnect, then restart the whole UI: ui_settings_init must clear the transient
  // M6 state (pending confirmation, visited-page fetch, scroll).
  d = m6dbg();
  click_raw(d.conn_disconnect[0].x + 10.0f, d.conn_disconnect[0].y + 10.0f);
  frame(1);
  CT_CHECK(m6dbg().confirm_kind == 3);
  teardown();
  CT_CHECK(setup("test_ui_settings_devices"));
  goto_m6_page(CCORD_M6_PAGE_CONNECTIONS);
  CT_CHECK(ccord_mock_admin_count(F.transport) == 1);   // fresh fixture: the visit fetches again
  frame(2);
  d = m6dbg();
  CT_CHECK(d.scroll == 0.0f && d.confirm_kind == 0);
  CT_CHECK(d.connection_count == 3);
}

// ------------------------------------------------------------------ excluded actions stay web redirects

static void test_redirects(void)
{
  goto_m6_page(CCORD_M6_PAGE_PRIVACY);   // leave the account pages first
  open_settings();
  ccord_ui_settings_debug s = sdbg();
  // The legacy fixed-size snapshot keeps its nine tabs; the M6 tabs live in the host seam.
  CT_CHECK(s.page_count == 9);
  // Slot 1 is the retired Profiles page: it keeps its index but has no tab.
  for (int i = 0; i < s.page_count; i++) {
    if (i == 1) CT_CHECK(s.tabs[i].w == 0.0f && s.tabs[i].h == 0.0f);
    else CT_CHECK(s.tabs[i].w > 0.0f && s.tabs[i].h > 0.0f);
  }
  click_rect(s.tabs[0]);   // My Account
  CT_CHECK(sdbg().page == 0);

  size_t before = ccord_mock_admin_count(F.transport);
  int urls = F.open_url_calls;
  s = sdbg();
  click_rect(s.billing_btn);
  CT_CHECK(strcmp(F.last_url, "https://discord.com/settings/billing") == 0);
  click_rect(s.nitro_btn);
  CT_CHECK(strcmp(F.last_url, "https://discord.com/nitro") == 0);
  click_rect(s.shop_btn);
  CT_CHECK(strcmp(F.last_url, "https://discord.com/shop") == 0);
  click_rect(s.boost_btn);
  CT_CHECK(strcmp(F.last_url, "https://discord.com/nitro") == 0);
  // Account credential edits (email/password/username/avatar/2FA) open discord.com, never a local form.
  ccord_rect account_btn;
  ui_settings_m6_host_rects(F.ui, nullptr, &account_btn);
  CT_CHECK(account_btn.w > 0.0f);
  click_rect(account_btn);
  CT_CHECK(strcmp(F.last_url, "https://discord.com/settings/account") == 0);
  CT_CHECK(F.open_url_calls == urls + 5);
  CT_CHECK(ccord_mock_admin_count(F.transport) == before);   // redirects never touch the REST queue
}

int main(int argc, char **argv)
{
  (void)argc;
  if (!setup(argv[0])) return 77;
  test_privacy_page();
  test_devices_page();
  test_devices_single_logout();
  test_connections_page();
  test_redirects();
  teardown();
  return CT_RESULT("test_ui_settings_devices");
}
