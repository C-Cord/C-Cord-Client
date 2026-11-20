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

bool verify_click_ss(app *a, const char *name)
{
  ccord_rect rc;
  if (!ccord_ui_server_settings_find(a->ui, name, &rc)) {
    ccord_input_event top = {.type = CCORD_IN_WHEEL, .wheel = 40.0f, .x = 640.0f, .y = 400.0f};
    app_inject(a, &top);
    app_run_frames(a, 1);
    for (int step_i = 0; step_i < 14 && !ccord_ui_server_settings_find(a->ui, name, &rc); step_i++) {
      ccord_input_event down = {.type = CCORD_IN_WHEEL, .wheel = -4.0f, .x = 640.0f, .y = 400.0f};
      app_inject(a, &down);
      app_run_frames(a, 1);
    }
  }
  if (!ccord_ui_server_settings_find(a->ui, name, &rc)) {
    fprintf(stderr, "ccord: --verify-slice: control '%s' not found in Server Settings\n", name);
    return false;
  }
  float cx = rc.x + rc.w * 0.5f, cy = rc.y + rc.h * 0.5f;
  app_inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, cx, cy);
  app_inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, cx, cy);
  app_inject_mouse(a, CCORD_IN_MOUSE_UP, 1, cx, cy);
  app_run_frames(a, 1);
  return true;
}

// ---------------------------------------------------------------- M0-7: --net-stats + --verify-slice m0

#define M0_TOTAL_BUDGET 1u         //one HISTORY fetch is the cold start
#define M0_IDLE_QUIET_FRAMES 10    // consecutive frames without a request = steady idle
#define M0_MAX_START_FRAMES 400    // a request storm must fail, not hang
#define M0_EXTRA_IDLE_FRAMES 60
#define M0_SWITCH_IDLE_FRAMES 30

static size_t m0_total_calls(const ccord_transport *t)
{
  size_t n = ccord_mock_admin_count(t);
  for (int c = 0; c < CCORD_MOCK_CALL_COUNT; c++) n += ccord_mock_call_count(t, (ccord_mock_call)c);
  return n;
}

static int m0_fail(const char *what)
{
  fprintf(stderr, "ccord: --verify-slice m0: %s\n", what);
  return 1;
}

// Frames until no new request has gone out for M0_IDLE_QUIET_FRAMES in a row. False means a request storm.
static bool m0_run_until_idle(app *a)
{
  size_t last = m0_total_calls(a->transport);
  int quiet = 0;
  for (int i = 0; i < M0_MAX_START_FRAMES; i++) {
    app_run_frames(a, 1);
    size_t now_total = m0_total_calls(a->transport);
    if (now_total == last) {
      quiet++;
      if (quiet >= M0_IDLE_QUIET_FRAMES) return true;
    } else {
      quiet = 0;
      last = now_total;
    }
  }
  return false;
}

// Asserts that `which` received exactly `want` requests; names the route on failure.
static int m0_expect_route(const ccord_transport *t, ccord_mock_call which, size_t want, const char *name)
{
  size_t got = ccord_mock_call_count(t, which);
  if (got == want) return 0;
  fprintf(stderr, "ccord: --verify-slice m0: route %s expected %zu request(s), got %zu\n", name, want, got);
  return 1;
}

// The first text channel of `guild` other than `not_a`/`not_b` (the mock seeds several per guild).
static ccord_id m0_other_text_channel(app *a, ccord_id guild, ccord_id not_a, ccord_id not_b)
{
  for (size_t i = 0; i < ccord_store_channel_count(a->store, guild); i++) {
    const ccord_channel *c = ccord_store_channel_at(a->store, guild, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->id != not_a && c->id != not_b) return c->id;
  }
  return CCORD_ID_NONE;
}

typedef struct m0_route_scan {
  size_t count;
  bool saw_messages, saw_members;
} m0_route_scan;

static void m0_route_cb(void *ud, const char *route, uint32_t requests, uint32_t rate_limited, int last_status)
{
  m0_route_scan *s = ud;
  s->count++;
  if (strcmp(route, "GET /channels/2001/messages") == 0 && requests == 2u && rate_limited == 0u &&
      last_status == 200)
    s->saw_messages = true;
  if (strcmp(route, "GET /guilds/2001/members") == 0 && requests == 1u && rate_limited == 0u && last_status == 200)
    s->saw_members = true;
}

// M0-7 governed counter check: drive a real governor exactly as the REST queue does (acquire + complete),
// then require both the aggregate stats and the per-route counters to reflect it. No network involved.
static int m0_exercise_governor(app *a)
{
  ccord_gov_config cfg = ccord_gov_config_default();
  a->verify_gov = ccord_gov_create(&cfg);
  if (!a->verify_gov) return m0_fail("cannot create verification governor");
  static const struct m0_probe {
    const char *method;
    const char *path;
    ccord_gov_priority prio;
  } probes[] = {
    {"GET", "/channels/2001/messages", CCORD_GOV_USER},
    {"GET", "/channels/2001/messages", CCORD_GOV_USER},
    {"GET", "/guilds/2001/members", CCORD_GOV_VISIBLE},
  };
  for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
    uint64_t now = 1000u + (uint64_t)i;
    ccord_gov_ticket ticket = {0};
    ccord_gov_decision d = ccord_gov_acquire(a->verify_gov, probes[i].method, probes[i].path, probes[i].prio,
                                             now, &ticket);
    if (d.verdict != CCORD_GOV_GO) return m0_fail("verification governor refused a probe request");
    ccord_gov_response resp;
    ccord_gov_response_init(&resp, 200);
    (void)ccord_gov_complete(a->verify_gov, &ticket, &resp, now);
  }
  ccord_gov_stats st = ccord_gov_get_stats(a->verify_gov);
  if (st.started[CCORD_GOV_USER] != 2u || st.started[CCORD_GOV_VISIBLE] != 1u || st.responses_2xx != 3u)
    return m0_fail("governed request counters did not aggregate");
  m0_route_scan scan = {0};
  ccord_gov_foreach_route(a->verify_gov, m0_route_cb, &scan);
  if (scan.count != 2u || !scan.saw_messages || !scan.saw_members)
    return m0_fail("per-route governor counters missing or wrong");
  return 0;
}

// M0-3: wait for the pending/failed/confirmed echo at `idx` to reach `want` (the mock's latency is a few frames).
static int m0_wait_send_state(app *a, ccord_id ch, size_t idx, uint8_t want, const char *name)
{
  for (int i = 0; i < 40; i++) {
    const ccord_message *m = ccord_store_message_at(a->store, ch, idx);
    if (m && m->send_state == want) return 0;
    app_run_frames(a, 1);
  }
  return m0_fail(name);
}

// M0-3: a headless nonce round-trip through the real session + mock transport: pending appears at once, a refusal
// fails that echo, retry resubmits the SAME nonce, and the nonce-matched MESSAGE_CREATE confirms without duplicates.
static int m0_verify_nonce_retry(app *a)
{
  ccord_id ch = ccord_store_selected_channel(a->store);
  if (ch == CCORD_ID_NONE) return m0_fail("nonce check has no selected channel");
  ccord_error err = {0};
  ccord_mock_set_refuse_writes(a->transport, false);

  size_t idx = ccord_store_message_count(a->store, ch);
  if (ccord_session_send_message(a->session, ch, "m0 nonce", strlen("m0 nonce"), &err) != CCORD_OK)
    return m0_fail("nonce send was refused");
  const ccord_message *p = ccord_store_message_at(a->store, ch, idx);
  if (!p || p->send_state != CCORD_MSG_SEND_PENDING || p->nonce[0] == '\0')
    return m0_fail("sent message did not appear as a pending echo");
  char nonce[32];
  snprintf(nonce, sizeof nonce, "%s", p->nonce);
  if (m0_wait_send_state(a, ch, idx, CCORD_MSG_SEND_CONFIRMED, "pending echo never confirmed") != 0) return 1;
  p = ccord_store_message_at(a->store, ch, idx);
  if (!p || strcmp(p->nonce, nonce) != 0) return m0_fail("confirmed echo lost its nonce");
  if (ccord_store_message_count(a->store, ch) != idx + 1) return m0_fail("confirmation duplicated the message");

  ccord_mock_set_refuse_writes(a->transport, true);
  idx = ccord_store_message_count(a->store, ch);
  if (ccord_session_send_message(a->session, ch, "m0 retry", strlen("m0 retry"), &err) != CCORD_OK)
    return m0_fail("refused nonce send should be accepted and fail asynchronously");
  p = ccord_store_message_at(a->store, ch, idx);
  if (!p || p->send_state != CCORD_MSG_SEND_PENDING) return m0_fail("refused send did not start pending");
  char retry_nonce[32];
  snprintf(retry_nonce, sizeof retry_nonce, "%s", p->nonce);
  if (m0_wait_send_state(a, ch, idx, CCORD_MSG_SEND_FAILED, "refusal did not mark the echo failed") != 0) return 1;
  p = ccord_store_message_at(a->store, ch, idx);
  if (!p || strcmp(p->nonce, retry_nonce) != 0) return m0_fail("failed echo lost its nonce");
  ccord_id failed_id = p->id;

  ccord_mock_set_refuse_writes(a->transport, false);
  if (ccord_session_retry_message(a->session, ch, failed_id, &err) != CCORD_OK)
    return m0_fail("retry of the failed echo was refused");
  p = ccord_store_message_at(a->store, ch, idx);
  if (!p || p->send_state != CCORD_MSG_SEND_PENDING || strcmp(p->nonce, retry_nonce) != 0)
    return m0_fail("retry did not re-pend with the original nonce");
  if (m0_wait_send_state(a, ch, idx, CCORD_MSG_SEND_CONFIRMED, "retried echo never confirmed") != 0) return 1;
  p = ccord_store_message_at(a->store, ch, idx);
  if (!p || strcmp(p->nonce, retry_nonce) != 0 || strcmp(p->content, "m0 retry") != 0)
    return m0_fail("retried echo confirmed with the wrong content/nonce");
  if (ccord_store_message_count(a->store, ch) != idx + 1) return m0_fail("retry duplicated the message");
  printf("m0: nonce round-trip pending->failed->retry->confirmed\n");
  return 0;
}

// M0-7: same cold-start budget scenario as src/ui/tests/test_cold_start_budget.c, driven through the real UI loop.
int run_verify_slice_m0(app *a)
{
  if (!a->mock || !a->transport) return m0_fail("requires the offline mock transport");

  // Background launch: nothing is focused yet, so reading-acks and member paging stay out of the budget.
  ccord_input_event unfocus = {.type = CCORD_IN_FOCUS, .focused = false};
  app_inject(a, &unfocus);

  // --- phase 1: cold start to steady idle, no user input at all ---
  if (!m0_run_until_idle(a)) return m0_fail("cold start never went idle (request storm)");
  size_t total = m0_total_calls(a->transport);
  if (ccord_session_state(a->session) != CCORD_CONN_CONNECTED) return m0_fail("mock transport did not connect");
  if (total > M0_TOTAL_BUDGET) {
    fprintf(stderr, "ccord: --verify-slice m0: cold start used %zu requests (budget %u)\n", total, M0_TOTAL_BUDGET);
    return 1;
  }
  if (ccord_mock_admin_count(a->transport) != 0) return m0_fail("cold start issued an admin request");
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 1, "HISTORY") != 0) return 1;
  static const struct m0_route_quiet {
    ccord_mock_call route;
    const char *name;
  } quiet[] = {
    {CCORD_MOCK_CALL_SEARCH, "SEARCH"},         {CCORD_MOCK_CALL_ACK, "ACK"},
    {CCORD_MOCK_CALL_TYPING, "TYPING"},         {CCORD_MOCK_CALL_MEMBERS, "MEMBERS"},
    {CCORD_MOCK_CALL_SLASH_INDEX, "SLASH_INDEX"}, {CCORD_MOCK_CALL_PINS, "PINS"},
    {CCORD_MOCK_CALL_REACTION_USERS, "REACTION_USERS"}, {CCORD_MOCK_CALL_DISCOVERY, "DISCOVERY"},
    {CCORD_MOCK_CALL_PROFILE, "PROFILE"},
  };
  for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; i++)
    if (m0_expect_route(a->transport, quiet[i].route, 0, quiet[i].name) != 0) return 1;

  ccord_id guild = ccord_store_selected_guild(a->store);
  ccord_id home = ccord_store_selected_channel(a->store);
  if (guild == CCORD_ID_NONE || home == CCORD_ID_NONE) return m0_fail("no guild/channel selected");
  if (!ccord_store_history_loaded(a->store, home)) return m0_fail("initial channel history not loaded");
  if (ccord_store_message_count(a->store, home) == 0) return m0_fail("initial channel history is empty");

  // --- phase 2: extra idle frames must not ask for anything ---
  size_t before_idle = total;
  app_run_frames(a, M0_EXTRA_IDLE_FRAMES);
  if (m0_total_calls(a->transport) != before_idle) return m0_fail("idle frames issued requests");

  // --- phase 3: switch away and back; a fetched channel is never fetched again ---
  ccord_id b = m0_other_text_channel(a, guild, home, CCORD_ID_NONE);
  ccord_id c = m0_other_text_channel(a, guild, home, b);
  if (b == CCORD_ID_NONE || c == CCORD_ID_NONE) return m0_fail("mock server lacks the channels for the switch checks");
  size_t before_switch = m0_total_calls(a->transport);
  ccord_error err = {0};

  if (ccord_session_select_channel(a->session, b, &err) != CCORD_OK) return m0_fail("cannot select channel b");
  if (!m0_run_until_idle(a)) return m0_fail("channel b did not settle");
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 2, "HISTORY") != 0) return 1;
  if (!ccord_store_history_loaded(a->store, b)) return m0_fail("channel b history not loaded");

  if (ccord_session_select_channel(a->session, home, &err) != CCORD_OK) return m0_fail("cannot re-select home");
  if (!m0_run_until_idle(a)) return m0_fail("home did not settle");
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 2, "HISTORY") != 0) return 1;   // no duplicate

  if (ccord_session_select_channel(a->session, b, &err) != CCORD_OK) return m0_fail("cannot re-select channel b");
  if (!m0_run_until_idle(a)) return m0_fail("channel b re-entry did not settle");
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 2, "HISTORY") != 0) return 1;

  // In-flight dedupe: c is asked for, then selected again before the reply lands (mock latency > one frame).
  if (ccord_session_select_channel(a->session, c, &err) != CCORD_OK) return m0_fail("cannot select channel c");
  app_run_frames(a, 1);
  if (ccord_store_history_loaded(a->store, c)) return m0_fail("mock reply for c arrived before the dedupe check");
  if (ccord_session_select_channel(a->session, c, &err) != CCORD_OK) return m0_fail("cannot re-select channel c");
  if (!m0_run_until_idle(a)) return m0_fail("channel c did not settle");
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 3, "HISTORY") != 0) return 1;
  if (!ccord_store_history_loaded(a->store, c)) return m0_fail("channel c history not loaded");

  if (ccord_session_select_channel(a->session, home, &err) != CCORD_OK) return m0_fail("cannot return home");
  if (!m0_run_until_idle(a)) return m0_fail("home did not settle after switches");
  app_run_frames(a, M0_SWITCH_IDLE_FRAMES);
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 3, "HISTORY") != 0) return 1;
  if (m0_total_calls(a->transport) != before_switch + 2u) return m0_fail("channel switches issued extra requests");

  // --- phase 4: focus sanity — the only ACK is the solicited reading ack for the open channel ---
  ccord_id unread_mid = 0;
  ccord_message_rich unread_r = {.present = CCORD_RICH_ALL};
  if (ccord_mock_post_rich_message(a->transport, home, 1002, "unread", &unread_r, &unread_mid, &err) != CCORD_OK) {
    return m0_fail("failed to post unread message for focus ack check");
  }
  if (!m0_run_until_idle(a)) return m0_fail("unread message did not settle before focus");
  ccord_input_event focus = {.type = CCORD_IN_FOCUS, .focused = true};
  app_inject(a, &focus);
  app_run_frames(a, 20);
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_ACK, 1, "ACK") != 0) return 1;
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_SEARCH, 0, "SEARCH") != 0) return 1;
  app_run_frames(a, M0_EXTRA_IDLE_FRAMES);
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_ACK, 1, "ACK") != 0) return 1;
  if (m0_expect_route(a->transport, CCORD_MOCK_CALL_HISTORY, 3, "HISTORY") != 0) return 1;

  if (m0_verify_nonce_retry(a) != 0) return 1;
  if (m0_exercise_governor(a) != 0) return 1;
  printf("m0: PASS budget=%zu/%u\n", total, M0_TOTAL_BUDGET);
  return 0;
}

