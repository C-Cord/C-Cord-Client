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

// Cold-start REST request budget.
//
// Drives a headless cold start — fresh store + session + mock transport + real UI frame loop — and gates how many
// REST-style requests the client is allowed to make before the user has done anything. The scenario is a background
// launch: the window has not been focused yet (FOCUS false before the first frame), so read-acknowledgement and
// member-list paging, which are user-presence behaviours, stay out of the startup budget. Nothing is typed, clicked
// or scrolled; the only later inputs are channel switches and, at the very end, one focus event.
//
// Measured baseline (mock seed 0xC0FFEE, big_channel_messages=200, latency_ms=60, 50 ms frames, window unfocused;
// initial channel = first text channel of guild 1, #general):
//   SEARCH 0, ACK 0, HISTORY 1, TYPING 0, MEMBERS 0, SLASH_INDEX 0, PINS 0, REACTION_USERS 0, DISCOVERY 0, PROFILE 0,
//   admin 0  ->  total 1 request before first idle (the initial channel's history, asked exactly once).
// The budget below is that measurement, not a guess: one history fetch is the whole cold start. Idle frames, channel
// re-entry and a second channel's first fetch must not add anything beyond the one HISTORY per newly opened channel.
// A reconnect storm would re-run the snapshot path and re-ask history, so the HISTORY caps also catch it.
//
// Covers request_history_once / was_requested / select guards in session.c.
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/capture.h>
#include <ccord/platform/platform.h>
#include <ccord/ui/ui.h>

#include "ccord_test.h"

// Tight caps, measured as described above. TOTAL_BUDGET counts every mock call route plus admin requests.
#define TOTAL_BUDGET 1u
#define HISTORY_INITIAL_CAP 1u     // initial channel selection: exactly one fetch, never a duplicate
#define IDLE_QUIET_FRAMES 10       // consecutive frames without a new request = steady idle
#define MAX_START_FRAMES 400       // safety valve: a request storm must fail, not hang
#define EXTRA_IDLE_FRAMES 60       // 3 s of nothing happening after idle
#define SWITCH_IDLE_FRAMES 30      // settle time after each channel switch
#define FRAME_MS 50u
#define MOCK_LATENCY_MS 60u        // replies take >1 frame: the in-flight dedupe window is exercised for real

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
    F.now += FRAME_MS;
    (void)ccord_session_pump(F.session);
    (void)ccord_bus_drain(F.bus);
    (void)ccord_ui_update(F.ui, F.now);
    ccord_render_begin(F.renderer, F.theme.c.bg_primary);
    ccord_ui_draw(F.ui, F.renderer);
    ccord_render_end(F.renderer);
  }
}

static size_t route_count(ccord_mock_call which)
{
  return ccord_mock_call_count(F.transport, which);
}

static size_t total_calls(void)
{
  size_t n = ccord_mock_admin_count(F.transport);
  for (int c = 0; c < CCORD_MOCK_CALL_COUNT; c++) n += route_count((ccord_mock_call)c);
  return n;
}

static void dump_counts(const char *phase)
{
  static const char *names[CCORD_MOCK_CALL_COUNT] = {
    "SEARCH", "ACK", "HISTORY", "TYPING", "MEMBERS", "SLASH_INDEX", "PINS", "REACTION_USERS", "DISCOVERY", "PROFILE",
  };
  fprintf(stdout, "cold-start budget [%s]: total=%zu admin=%zu", phase, total_calls(), ccord_mock_admin_count(F.transport));
  for (int c = 0; c < CCORD_MOCK_CALL_COUNT; c++) fprintf(stdout, " %s=%zu", names[c], route_count((ccord_mock_call)c));
  fprintf(stdout, "\n");
}

// Frames until no new request has gone out for IDLE_QUIET_FRAMES in a row. Returns false if that never happens
// within MAX_START_FRAMES (a storm): the caller's budget assertions then fail with the counters printed.
static bool run_until_idle(void)
{
  size_t last = total_calls();
  int quiet = 0;
  for (int i = 0; i < MAX_START_FRAMES; i++) {
    frame(1);
    size_t now_total = total_calls();
    if (now_total == last) {
      quiet++;
      if (quiet >= IDLE_QUIET_FRAMES) return true;
    } else {
      quiet = 0;
      last = now_total;
    }
  }
  return false;
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("cold_budget_data"));
// Hermetic settings/drafts, in a directory of this test's own (ctest runs tests in parallel in one cwd).
  remove("cold_budget_data/settings.ini");
  remove("cold_budget_data/drafts.json");
  remove("cold_budget_data");
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK) return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 200, .latency_ms = MOCK_LATENCY_MS, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.transport || !F.session) return false;
  ccord_platform_config pc = {.title = "cold-budget", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .fixed_now_unix_ms = 1793534400000LL + 30 * 60 * 1000};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  // Background launch: the window is not focused yet, so reading-acks and member paging stay out of the budget.
  ccord_input_event unfocus = {.type = CCORD_IN_FOCUS, .focused = false};
  (void)ccord_ui_handle_input(F.ui, &unfocus);
  return true;
}

static void teardown(void)
{
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_contrib_destroy(F.contrib);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
  remove("cold_budget_data/settings.ini");
  remove("cold_budget_data/drafts.json");
  remove("cold_budget_data");
}

// The first text channel of the selected guild other than `not_a`/`not_b` (the mock seeds several per guild).
static ccord_id other_text_channel(ccord_id guild, ccord_id not_a, ccord_id not_b)
{
  for (size_t i = 0; i < ccord_store_channel_count(F.store, guild); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, guild, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->id != not_a && c->id != not_b) return c->id;
  }
  return CCORD_ID_NONE;
}

static void test_cold_start_budget(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_start(F.session, &err) == CCORD_OK);

  // --- phase 1: cold start to steady idle, no user input at all ---
  bool idle = run_until_idle();
  dump_counts("cold start");
  CT_CHECK(idle);                                            // no request storm: it went quiet
  CT_CHECK(ccord_session_state(F.session) == CCORD_CONN_CONNECTED);
  CT_CHECK(total_calls() <= TOTAL_BUDGET);
  CT_CHECK(ccord_mock_admin_count(F.transport) == 0);
  CT_CHECK(route_count(CCORD_MOCK_CALL_HISTORY) == HISTORY_INITIAL_CAP);
  CT_CHECK(route_count(CCORD_MOCK_CALL_SEARCH) == 0);        // nothing searched: no unsolicited search
  CT_CHECK(route_count(CCORD_MOCK_CALL_ACK) == 0);           // nobody reading yet: no unsolicited ack
  CT_CHECK(route_count(CCORD_MOCK_CALL_TYPING) == 0);
  CT_CHECK(route_count(CCORD_MOCK_CALL_MEMBERS) == 0);
  CT_CHECK(route_count(CCORD_MOCK_CALL_SLASH_INDEX) == 0);
  CT_CHECK(route_count(CCORD_MOCK_CALL_PINS) == 0);
  CT_CHECK(route_count(CCORD_MOCK_CALL_REACTION_USERS) == 0);
  CT_CHECK(route_count(CCORD_MOCK_CALL_DISCOVERY) == 0);
  CT_CHECK(route_count(CCORD_MOCK_CALL_PROFILE) == 0);

  ccord_id guild = ccord_store_selected_guild(F.store);
  ccord_id home = ccord_store_selected_channel(F.store);
  CT_CHECK(guild != CCORD_ID_NONE && home != CCORD_ID_NONE);
  // The one allowed request actually delivered the initial channel's history.
  CT_CHECK(ccord_store_history_loaded(F.store, home));
  CT_CHECK(ccord_store_message_count(F.store, home) > 0);

  // --- phase 2: extra idle frames must not ask for anything ---
  size_t before_idle = total_calls();
  frame(EXTRA_IDLE_FRAMES);
  dump_counts("extra idle");
  CT_CHECK(total_calls() == before_idle);
  CT_CHECK(ccord_session_state(F.session) == CCORD_CONN_CONNECTED);

  // --- phase 3: switch away and back; a fetched channel is never fetched again ---
  ccord_id b = other_text_channel(guild, home, CCORD_ID_NONE);
  ccord_id c = other_text_channel(guild, home, b);
  CT_CHECK(b != CCORD_ID_NONE && c != CCORD_ID_NONE);
  size_t before_switch = total_calls();

  CT_CHECK(ccord_session_select_channel(F.session, b, &err) == CCORD_OK);
  CT_CHECK(run_until_idle());
  CT_CHECK(route_count(CCORD_MOCK_CALL_HISTORY) == 2);       // b fetched once
  CT_CHECK(ccord_store_history_loaded(F.store, b));

  CT_CHECK(ccord_session_select_channel(F.session, home, &err) == CCORD_OK);
  CT_CHECK(run_until_idle());
  CT_CHECK(route_count(CCORD_MOCK_CALL_HISTORY) == 2);       // re-entry: no duplicate history for home

  CT_CHECK(ccord_session_select_channel(F.session, b, &err) == CCORD_OK);
  CT_CHECK(run_until_idle());
  CT_CHECK(route_count(CCORD_MOCK_CALL_HISTORY) == 2);       // and none for b either

  // In-flight dedupe: c is asked for, then selected again before the reply lands (latency > one frame).
  // was_requested() must swallow the second selection; only the reply clears the guard.
  CT_CHECK(ccord_session_select_channel(F.session, c, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(!ccord_store_history_loaded(F.store, c));         // reply still in flight
  CT_CHECK(ccord_session_select_channel(F.session, c, &err) == CCORD_OK);
  CT_CHECK(run_until_idle());
  CT_CHECK(route_count(CCORD_MOCK_CALL_HISTORY) == 3);       // c fetched exactly once
  CT_CHECK(ccord_store_history_loaded(F.store, c));

  CT_CHECK(ccord_session_select_channel(F.session, home, &err) == CCORD_OK);
  CT_CHECK(run_until_idle());
  frame(SWITCH_IDLE_FRAMES);
  dump_counts("after switches");
  CT_CHECK(route_count(CCORD_MOCK_CALL_HISTORY) == 3);
  CT_CHECK(total_calls() == before_switch + 2);              // only b's and c's first fetch were added
  CT_CHECK(ccord_session_state(F.session) == CCORD_CONN_CONNECTED);

  // --- phase 4: focus sanity — the ack counter works, and the only ack is the solicited reading ack ---
  ccord_id unread_mid = 0;
  ccord_message_rich unread_r = {.present = CCORD_RICH_ALL};
  CT_CHECK(ccord_mock_post_rich_message(F.transport, home, 1002, "unread", &unread_r, &unread_mid, &err) == CCORD_OK);
  CT_CHECK(run_until_idle());
  ccord_input_event focus = {.type = CCORD_IN_FOCUS, .focused = true};
  (void)ccord_ui_handle_input(F.ui, &focus);
  frame(20);                                                 // > UI_ACK_SETTLE_MS: the open channel is read once
  dump_counts("focused");
  CT_CHECK(route_count(CCORD_MOCK_CALL_ACK) == 1);
  CT_CHECK(route_count(CCORD_MOCK_CALL_SEARCH) == 0);
  frame(EXTRA_IDLE_FRAMES);
  CT_CHECK(route_count(CCORD_MOCK_CALL_ACK) == 1);           // no new messages: no repeat ack
  CT_CHECK(route_count(CCORD_MOCK_CALL_HISTORY) == 3);
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_cold_start_budget")) {
    fprintf(stderr, "cold-start budget test: setup failed or no display driver; skipping\n");
    return 77;
  }
  test_cold_start_budget();
  teardown();
  return CT_RESULT("test_cold_start_budget");
}
