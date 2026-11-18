// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Voice reconnect policy: the failure table, backoff bounds, the
// attempt and time limits, the stable reset and the network-event gate. Pure logic with an explicit clock.
#include <ccord/net/voice_reconnect.h>

#include <string.h>

#include "ccord_test.h"

#define CLOSE CCORD_VC_CAUSE_CLOSE

typedef struct row {
  uint16_t code;
  ccord_vc_action established, fresh;   // action when established / not yet established
  const char *reason;                   // FINAL reason ("" = just not empty)
} row;

static const row table[] = {
  {1000, CCORD_VC_RESUME, CCORD_VC_REIDENTIFY, nullptr},
  {1001, CCORD_VC_RESUME, CCORD_VC_REIDENTIFY, nullptr},
  {1006, CCORD_VC_RESUME, CCORD_VC_REIDENTIFY, nullptr},
  {1015, CCORD_VC_RESUME, CCORD_VC_REIDENTIFY, nullptr},
  {0, CCORD_VC_RESUME, CCORD_VC_REIDENTIFY, nullptr},
  {4015, CCORD_VC_RESUME, CCORD_VC_REIDENTIFY, nullptr},
  {4003, CCORD_VC_REIDENTIFY, CCORD_VC_REIDENTIFY, nullptr},
  {4005, CCORD_VC_REIDENTIFY, CCORD_VC_REIDENTIFY, nullptr},
  {4006, CCORD_VC_REJOIN, CCORD_VC_REJOIN, nullptr},
  {4009, CCORD_VC_REJOIN, CCORD_VC_REJOIN, nullptr},
  {4004, CCORD_VC_REJOIN, CCORD_VC_REJOIN, nullptr},
  {4011, CCORD_VC_REJOIN, CCORD_VC_REJOIN, nullptr},
  {4014, CCORD_VC_AWAIT_MAIN, CCORD_VC_AWAIT_MAIN, nullptr},
  {4022, CCORD_VC_AWAIT_MAIN, CCORD_VC_AWAIT_MAIN, nullptr},
  {4021, CCORD_VC_FINAL, CCORD_VC_FINAL, "rate limited"},
  {4017, CCORD_VC_FINAL, CCORD_VC_FINAL, "this channel needs end-to-end encryption this build cannot do"},
  {4001, CCORD_VC_FINAL, CCORD_VC_FINAL, "protocol error (code 4001)"},
  {4002, CCORD_VC_FINAL, CCORD_VC_FINAL, "protocol error (code 4002)"},
  {4012, CCORD_VC_FINAL, CCORD_VC_FINAL, "protocol error (code 4012)"},
  {4016, CCORD_VC_FINAL, CCORD_VC_FINAL, "protocol error (code 4016)"},
  {4020, CCORD_VC_FINAL, CCORD_VC_FINAL, "protocol error (code 4020)"},
  {4000, CCORD_VC_REJOIN, CCORD_VC_REJOIN, nullptr},   // any other 4000-4999
  {4999, CCORD_VC_REJOIN, CCORD_VC_REJOIN, nullptr},
  {4100, CCORD_VC_REJOIN, CCORD_VC_REJOIN, nullptr},
};

static void test_failure_table(void)
{
  for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
    const row *r = &table[i];
    CT_CHECK(ccord_vc_classify(r->code, CLOSE, true) == r->established);
    CT_CHECK(ccord_vc_classify(r->code, CLOSE, false) == r->fresh);
    for (int est = 0; est < 2; est++) {
      ccord_vc p;
      ccord_vc_init(&p, 7);
      ccord_vc_plan pl = ccord_vc_plan_next(&p, 1000, r->code, CLOSE, est);
      ccord_vc_action want = est ? r->established : r->fresh;
      CT_CHECK(pl.action == want);
      if (want == CCORD_VC_FINAL) {
        CT_CHECK(pl.attempt == 0 && pl.reason && strlen(pl.reason) < 90);
        if (r->reason) CT_CHECK(strcmp(pl.reason, r->reason) == 0);
        CT_CHECK(p.attempt == 0);
      } else if (want == CCORD_VC_AWAIT_MAIN) {
        CT_CHECK(pl.delay_ms == CCORD_VC_AWAIT_MAIN_MS && pl.attempt == 0 && p.attempt == 0);
      } else {
        CT_CHECK(pl.attempt == 1 && p.attempt == 1);
      }
    }
  }
  // Non-close causes are dead sockets: resumable once established, a fresh identify before that.
  ccord_vc_cause causes[] = {CCORD_VC_CAUSE_IO, CCORD_VC_CAUSE_HEARTBEAT, CCORD_VC_CAUSE_UDP, CCORD_VC_CAUSE_CONNECT};
  for (size_t i = 0; i < 4; i++) {
    CT_CHECK(ccord_vc_classify(0, causes[i], true) == CCORD_VC_RESUME);
    CT_CHECK(ccord_vc_classify(0, causes[i], false) == CCORD_VC_REIDENTIFY);
  }
}

static void test_backoff_bounds_and_attempt_limit(void)
{
  static const uint32_t base[] = {250, 1000, 2000, 4000, 8000, 16000};
  for (uint64_t seed = 1; seed <= 300; seed++) {
    ccord_vc p;
    ccord_vc_init(&p, seed);
    uint64_t now = 1000;
    for (uint32_t n = 1; n <= CCORD_VC_MAX_ATTEMPTS; n++) {
      ccord_vc_plan pl = ccord_vc_plan_next(&p, now, 0, CCORD_VC_CAUSE_IO, true);
      CT_CHECK(pl.attempt == n && pl.action != CCORD_VC_FINAL);
      if (n == 1) CT_CHECK(pl.delay_ms == 250);                                        // attempt 1 is exact
      else CT_CHECK(pl.delay_ms >= base[n - 1] * 8 / 10 && pl.delay_ms <= base[n - 1] * 12 / 10);
      CT_CHECK(pl.delay_ms <= 19200);                                                    // cap: 16 s + 20 %
      now += 100;   // failures arrive quickly, well inside 45 s
    }
    ccord_vc_plan last = ccord_vc_plan_next(&p, now, 0, CCORD_VC_CAUSE_IO, true);        // attempt 7
    CT_CHECK(last.action == CCORD_VC_FINAL && last.attempt == 0 && strcmp(last.reason, "connection lost") == 0);
    CT_CHECK(p.attempt == CCORD_VC_MAX_ATTEMPTS);
  }
  // Jitter really spreads attempt 2 over the +-20 % band.
  uint32_t lo = UINT32_MAX, hi = 0;
  for (uint64_t seed = 1; seed <= 400; seed++) {
    ccord_vc p;
    ccord_vc_init(&p, seed * 7919u);
    (void)ccord_vc_plan_next(&p, 1000, 0, CCORD_VC_CAUSE_IO, true);
    ccord_vc_plan pl = ccord_vc_plan_next(&p, 1300, 0, CCORD_VC_CAUSE_IO, true);
    if (pl.delay_ms < lo) lo = pl.delay_ms;
    if (pl.delay_ms > hi) hi = pl.delay_ms;
  }
  CT_CHECK(lo >= 800 && lo < 900 && hi <= 1200 && hi > 1100);
  // Same seed, same timeline.
  ccord_vc a, b;
  ccord_vc_init(&a, 99);
  ccord_vc_init(&b, 99);
  for (int i = 0; i < 4; i++)
    CT_CHECK(ccord_vc_plan_next(&a, 1000u + (uint32_t)i, 0, CCORD_VC_CAUSE_IO, true).delay_ms ==
             ccord_vc_plan_next(&b, 1000u + (uint32_t)i, 0, CCORD_VC_CAUSE_IO, true).delay_ms);
}

static void test_resume_escalates_to_rejoin(void)
{
  ccord_vc p;
  ccord_vc_init(&p, 3);
  uint64_t now = 5000;
  ccord_vc_action want[] = {CCORD_VC_RESUME, CCORD_VC_RESUME, CCORD_VC_RESUME,
                            CCORD_VC_REJOIN, CCORD_VC_REJOIN, CCORD_VC_REJOIN};
  for (uint32_t i = 0; i < 6; i++) {
    ccord_vc_plan pl = ccord_vc_plan_next(&p, now, 1006, CLOSE, true);
    CT_CHECK(pl.action == want[i] && pl.attempt == i + 1);
    if (pl.action == CCORD_VC_REJOIN) CT_CHECK(pl.delay_ms >= 1000);                    // REJOIN minimum delay
    now += pl.delay_ms;
  }
  CT_CHECK(p.resume_tries == CCORD_VC_RESUME_TRIES);
  // A fresh outage with REJOIN as attempt 1 still waits at least 1 s.
  ccord_vc q;
  ccord_vc_init(&q, 3);
  CT_CHECK(ccord_vc_plan_next(&q, 100, 4006, CLOSE, true).delay_ms == 1000);
}

static void test_time_limit(void)
{
  ccord_vc p;
  ccord_vc_init(&p, 11);
  CT_CHECK(ccord_vc_plan_next(&p, 10000, 0, CCORD_VC_CAUSE_IO, true).action == CCORD_VC_RESUME);   // outage starts at 10 s
  ccord_vc_plan pl = ccord_vc_plan_next(&p, 10000 + CCORD_VC_GIVEUP_MS - 1, 0, CCORD_VC_CAUSE_IO, true);
  CT_CHECK(pl.action != CCORD_VC_FINAL && p.attempt == 2);
  pl = ccord_vc_plan_next(&p, 10000 + CCORD_VC_GIVEUP_MS, 0, CCORD_VC_CAUSE_IO, true);   // 45 s, only attempt 3 would be next
  CT_CHECK(pl.action == CCORD_VC_FINAL && strcmp(pl.reason, "connection lost") == 0 && p.attempt == 2);
  // Even a different failure class gives up once the window is gone.
  CT_CHECK(ccord_vc_plan_next(&p, 10000 + CCORD_VC_GIVEUP_MS + 5, 4006, CLOSE, true).action == CCORD_VC_FINAL);
  // AWAIT_MAIN never consumes an attempt and is still allowed after the budget is spent.
  ccord_vc q;
  ccord_vc_init(&q, 11);
  (void)ccord_vc_plan_next(&q, 1000, 0, CCORD_VC_CAUSE_IO, true);
  ccord_vc_plan w = ccord_vc_plan_next(&q, 1100, 4014, CLOSE, true);
  CT_CHECK(w.action == CCORD_VC_AWAIT_MAIN && w.delay_ms == 3000 && q.attempt == 1);
}

static void test_second_4004_is_final(void)
{
  ccord_vc p;
  ccord_vc_init(&p, 5);
  ccord_vc_plan a = ccord_vc_plan_next(&p, 1000, 4004, CLOSE, true);
  CT_CHECK(a.action == CCORD_VC_REJOIN && p.auth_failures == 1);
  ccord_vc_plan b = ccord_vc_plan_next(&p, 3000, 4004, CLOSE, true);
  CT_CHECK(b.action == CCORD_VC_FINAL && b.attempt == 0 && b.reason[0] && strlen(b.reason) < 90);
  CT_CHECK(p.attempt == 1);   // the refused one used no attempt
  // A stable connection forgives the first 4004.
  ccord_vc q;
  ccord_vc_init(&q, 5);
  (void)ccord_vc_plan_next(&q, 1000, 4004, CLOSE, true);
  ccord_vc_established(&q, 3000);
  ccord_vc_tick(&q, 3000 + CCORD_VC_STABLE_MS);
  CT_CHECK(ccord_vc_plan_next(&q, 20000, 4004, CLOSE, true).action == CCORD_VC_REJOIN);
}

static void test_stable_reset(void)
{
  ccord_vc p;
  ccord_vc_init(&p, 8);
  for (int i = 0; i < 4; i++) (void)ccord_vc_plan_next(&p, 1000u + 100u * (uint32_t)i, 0, CCORD_VC_CAUSE_IO, true);
  CT_CHECK(p.attempt == 4 && p.resume_tries == 3);
  ccord_vc_tick(&p, 9000);                                   // never established: nothing resets
  CT_CHECK(p.attempt == 4);
  ccord_vc_established(&p, 9000);
  ccord_vc_tick(&p, 9000 + CCORD_VC_STABLE_MS - 1);          // not stable yet
  CT_CHECK(p.attempt == 4);
  ccord_vc_tick(&p, 9000 + CCORD_VC_STABLE_MS);
  CT_CHECK(p.attempt == 0 && p.resume_tries == 0 && p.auth_failures == 0);
  // A new failure is a new outage: its clock starts afresh, so 60 s after the old one is not a give-up.
  ccord_vc_plan pl = ccord_vc_plan_next(&p, 60000, 0, CCORD_VC_CAUSE_IO, true);
  CT_CHECK(pl.action == CCORD_VC_RESUME && pl.attempt == 1 && pl.delay_ms == 250);
  // A drop before the stable window keeps the budget.
  ccord_vc_established(&p, 61000);
  ccord_vc_plan again = ccord_vc_plan_next(&p, 65000, 0, CCORD_VC_CAUSE_IO, true);
  CT_CHECK(again.attempt == 2);
  ccord_vc_tick(&p, 80000);                                  // established_since was cleared by the failure
  CT_CHECK(p.attempt == 2);
}

static void test_network_event_gate(void)
{
  ccord_vc p;
  ccord_vc_init(&p, 1);
  CT_CHECK(ccord_vc_accept_network_event(&p, 100));
  CT_CHECK(!ccord_vc_accept_network_event(&p, 100 + CCORD_VC_EVENT_MIN_GAP_MS - 1));
  CT_CHECK(ccord_vc_accept_network_event(&p, 100 + CCORD_VC_EVENT_MIN_GAP_MS));
  CT_CHECK(!ccord_vc_accept_network_event(&p, 200 + CCORD_VC_EVENT_MIN_GAP_MS));
  CT_CHECK(p.attempt == 0);                                   // never touches the budget
  CT_CHECK(!ccord_vc_accept_network_event(nullptr, 0));
  ccord_vc z;
  ccord_vc_init(&z, 1);
  CT_CHECK(ccord_vc_accept_network_event(&z, 0));             // a clock at 0 is still a valid first event
  CT_CHECK(!ccord_vc_accept_network_event(&z, 10));
}

static void test_names(void)
{
  CT_CHECK(strcmp(ccord_vc_action_name(CCORD_VC_RESUME), "resume") == 0);
  CT_CHECK(strcmp(ccord_vc_action_name(CCORD_VC_REIDENTIFY), "reidentify") == 0);
  CT_CHECK(strcmp(ccord_vc_action_name(CCORD_VC_REJOIN), "rejoin") == 0);
  CT_CHECK(strcmp(ccord_vc_action_name(CCORD_VC_AWAIT_MAIN), "await_main") == 0);
  CT_CHECK(strcmp(ccord_vc_action_name(CCORD_VC_FINAL), "final") == 0);
}

int main(void)
{
  test_failure_table();
  test_backoff_bounds_and_attempt_limit();
  test_resume_escalates_to_rejoin();
  test_time_limit();
  test_second_4004_is_final();
  test_stable_reset();
  test_network_event_gate();
  test_names();
  return CT_RESULT("test_voice_reconnect");
}
