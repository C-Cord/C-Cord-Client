// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Gateway pacing policy: heartbeat jitter and zombie detection, reconnect backoff, identify budget, send limiter.
// All pure logic with an explicit clock, so every number here is exact or bounded, never timing-dependent.
#include <ccord/net/gateway_pace.h>

#include "ccord_test.h"

#define INTERVAL 41250u

static void test_first_heartbeat_is_jittered(void)
{
  uint64_t lo = UINT64_MAX, hi = 0;
  for (uint64_t seed = 1; seed <= 400; seed++) {
    ccord_gwpace p;
    ccord_gwpace_init(&p, seed * 7919u);
    ccord_gwpace_hello(&p, 1000, INTERVAL);
    CT_CHECK(p.hb_next_ms >= 1000 && p.hb_next_ms < 1000 + INTERVAL);   // interval * random(0..1)
    uint64_t d = p.hb_next_ms - 1000;
    if (d < lo) lo = d;
    if (d > hi) hi = d;
    CT_CHECK(ccord_gwpace_heartbeat_due(&p, p.hb_next_ms - 1) == CCORD_GWPACE_BEAT_NONE || p.hb_next_ms == 1000);
    CT_CHECK(ccord_gwpace_heartbeat_due(&p, p.hb_next_ms) == CCORD_GWPACE_BEAT_SEND);
  }
  // Across many clients the first beats are spread over the interval, not bunched at one instant.
  CT_CHECK(lo < INTERVAL / 10 && hi > INTERVAL * 9 / 10);
}

static void test_ack_and_zombie_detection(void)
{
  ccord_gwpace p;
  ccord_gwpace_init(&p, 42);
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, 999999) == CCORD_GWPACE_BEAT_NONE);   // before HELLO nothing is due
  ccord_gwpace_hello(&p, 0, INTERVAL);
  uint64_t t = p.hb_next_ms;
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, t) == CCORD_GWPACE_BEAT_SEND);
  ccord_gwpace_heartbeat_sent(&p, t);
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, t + 1) == CCORD_GWPACE_BEAT_NONE);
  CT_CHECK(ccord_gwpace_heartbeat_ack(&p, t + 80) == 80);                       // round trip measured
  CT_CHECK(ccord_gwpace_heartbeat_ack(&p, t + 90) == 0);                        // a second ack means nothing
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, t + INTERVAL - 1) == CCORD_GWPACE_BEAT_NONE);
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, t + INTERVAL) == CCORD_GWPACE_BEAT_SEND);

  // The next one goes out and is never acknowledged: when the following is due the connection is a zombie.
  ccord_gwpace_heartbeat_sent(&p, t + INTERVAL);
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, t + 2u * INTERVAL - 1) == CCORD_GWPACE_BEAT_NONE);
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, t + 2u * INTERVAL) == CCORD_GWPACE_BEAT_ZOMBIE);
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, t + 5u * INTERVAL) == CCORD_GWPACE_BEAT_ZOMBIE);

  // Discord asked for a heartbeat (op 1) while one was outstanding: answering restarts the interval.
  ccord_gwpace_heartbeat_sent(&p, t + 100000);
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, t + 100000 + INTERVAL - 1) == CCORD_GWPACE_BEAT_NONE);

  // A new connection forgets the old ack state and goes quiet while down.
  ccord_gwpace_disconnected(&p);
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, UINT64_MAX / 2) == CCORD_GWPACE_BEAT_NONE);
  ccord_gwpace_hello(&p, 500000, INTERVAL);
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, 500000 + INTERVAL) == CCORD_GWPACE_BEAT_SEND);   // not ZOMBIE

  ccord_gwpace z;
  ccord_gwpace_init(&z, 3);
  ccord_gwpace_hello(&z, 0, 0);   // a malformed HELLO must not produce a busy loop
  CT_CHECK(z.hb_interval_ms >= 1000);
}

static uint32_t plan(ccord_gwpace *p, uint64_t now, ccord_gwpace_reason r, bool resumable)
{
  uint32_t d = ccord_gwpace_plan_reconnect(p, now, r, resumable);
  bool resume = !resumable;
  CT_CHECK(ccord_gwpace_reconnect_pending(p));
  if (d > 0) CT_CHECK(!ccord_gwpace_reconnect_due(p, now + d - 1u, &resume));   // not a millisecond early
  CT_CHECK(ccord_gwpace_reconnect_due(p, now + d, &resume));
  CT_CHECK(resume == resumable);
  return d;
}

static void test_backoff_grows_and_is_jittered(void)
{
  for (uint64_t seed = 1; seed <= 200; seed++) {
    ccord_gwpace p;
    ccord_gwpace_init(&p, seed * 104729u);
    uint64_t now = 100000;
    CT_CHECK(plan(&p, now, CCORD_GWPACE_NETWORK, true) == 0);   // the first drop is retried at once
    uint32_t full = CCORD_GWPACE_BACKOFF_BASE_MS;
    for (int step = 0; step < 9; step++) {
      uint32_t d = plan(&p, now, CCORD_GWPACE_NETWORK, true);
      uint32_t cap = full > CCORD_GWPACE_BACKOFF_MAX_MS ? CCORD_GWPACE_BACKOFF_MAX_MS : full;
      CT_CHECK(d >= cap / 2 && d <= cap);   // half fixed, half random
      full *= 2;
    }
    // Long outage: the ceiling holds, and it never drops to zero between attempts.
    for (int i = 0; i < 40; i++) {
      uint32_t d = plan(&p, now, CCORD_GWPACE_CLOSED, true);
      CT_CHECK(d >= CCORD_GWPACE_BACKOFF_MAX_MS / 2 && d <= CCORD_GWPACE_BACKOFF_MAX_MS);
    }
  }
  // Different clients draw different delays (that is the point of the jitter).
  uint32_t seen[8];
  for (int i = 0; i < 8; i++) {
    ccord_gwpace p;
    ccord_gwpace_init(&p, 1000u + (uint64_t)i * 31u);
    (void)plan(&p, 0, CCORD_GWPACE_NETWORK, true);
    seen[i] = plan(&p, 0, CCORD_GWPACE_NETWORK, true);
  }
  bool differ = false;
  for (int i = 1; i < 8; i++) differ = differ || seen[i] != seen[0];
  CT_CHECK(differ);
  // Same seed, same delays.
  ccord_gwpace a, b;
  ccord_gwpace_init(&a, 77);
  ccord_gwpace_init(&b, 77);
  for (int i = 0; i < 6; i++) CT_CHECK(plan(&a, 0, CCORD_GWPACE_NETWORK, true) == plan(&b, 0, CCORD_GWPACE_NETWORK, true));
}

static void test_invalid_session_waits_one_to_five_seconds(void)
{
  uint32_t lo = UINT32_MAX, hi = 0;
  for (uint64_t seed = 1; seed <= 300; seed++) {
    ccord_gwpace p;
    ccord_gwpace_init(&p, seed * 15485863u);
    uint32_t d = plan(&p, 50000, CCORD_GWPACE_INVALID_SESSION, false);   // not resumable: a fresh IDENTIFY follows
    CT_CHECK(d >= 1000 && d <= 5000);
    if (d < lo) lo = d;
    if (d > hi) hi = d;
  }
  CT_CHECK(lo < 1500 && hi > 4500);   // the whole 1-5 s range is used
}

static void test_requested_reconnect_resumes_immediately_once(void)
{
  ccord_gwpace p;
  ccord_gwpace_init(&p, 5);
  CT_CHECK(plan(&p, 1000, CCORD_GWPACE_RECONNECT_REQUESTED, true) == 0);
  ccord_gwpace_reconnect_started(&p);
  CT_CHECK(!ccord_gwpace_reconnect_pending(&p));
  bool resume;
  CT_CHECK(!ccord_gwpace_reconnect_due(&p, 999999, &resume));   // nothing pending: nothing due
  // Discord keeps asking and the session keeps dropping straight away: that is a loop, so it backs off.
  CT_CHECK(plan(&p, 1200, CCORD_GWPACE_RECONNECT_REQUESTED, true) >= CCORD_GWPACE_BACKOFF_BASE_MS / 2);
}

static void test_stable_connection_earns_the_streak_back(void)
{
  ccord_gwpace p;
  ccord_gwpace_init(&p, 9);
  for (int i = 0; i < 5; i++) (void)plan(&p, 0, CCORD_GWPACE_NETWORK, true);
  CT_CHECK(p.fail_streak == 5);
  // Up for less than the stable time, then dropped again: no credit (a flapping connection stays throttled).
  ccord_gwpace_connected(&p, 100000);
  ccord_gwpace_tick(&p, 100000 + CCORD_GWPACE_STABLE_MS - 1);
  CT_CHECK(p.fail_streak == 5);
  ccord_gwpace_disconnected(&p);
  ccord_gwpace_tick(&p, 100000 + 10 * CCORD_GWPACE_STABLE_MS);   // down: uptime does not count
  CT_CHECK(p.fail_streak == 5);
  CT_CHECK(plan(&p, 200000, CCORD_GWPACE_CLOSED, true) >= 8000);   // still deep in the backoff
  // Up and stable: the next drop is retried at once again.
  ccord_gwpace_connected(&p, 300000);
  ccord_gwpace_tick(&p, 300000 + CCORD_GWPACE_STABLE_MS);
  CT_CHECK(p.fail_streak == 0);
  ccord_gwpace_disconnected(&p);
  CT_CHECK(plan(&p, 400000, CCORD_GWPACE_CLOSED, true) == 0);
}

static void test_identify_is_spaced_and_capped(void)
{
  ccord_gwpace p;
  ccord_gwpace_init(&p, 11);
  ccord_gwpace_identify_sent(&p, 10000);   // the first connection identified at t=10 s
  // A fresh session wanted 1 s later waits out the 5 s gap; resuming does not need one.
  uint32_t d = ccord_gwpace_plan_reconnect(&p, 11000, CCORD_GWPACE_INVALID_SESSION, false);
  CT_CHECK(d >= 4000);
  ccord_gwpace q;
  ccord_gwpace_init(&q, 11);
  ccord_gwpace_identify_sent(&q, 10000);
  CT_CHECK(ccord_gwpace_plan_reconnect(&q, 11000, CCORD_GWPACE_RECONNECT_REQUESTED, true) == 0);

  // A loop of fresh identifies is held back: after the per-hour budget the next one waits for the oldest to age out.
  ccord_gwpace h;
  ccord_gwpace_init(&h, 13);
  for (uint32_t i = 0; i < CCORD_GWPACE_IDENTIFY_PER_HOUR; i++) {
    ccord_gwpace_identify_sent(&h, 1000 + (uint64_t)i * 10000);
  }
  uint64_t now = 1000 + (uint64_t)CCORD_GWPACE_IDENTIFY_PER_HOUR * 10000;
  d = ccord_gwpace_plan_reconnect(&h, now, CCORD_GWPACE_CLOSED, false);
  CT_CHECK(now + d >= 1000 + 3600000u);
  // Resuming is not an identify and is not held back by that budget.
  ccord_gwpace_init(&h, 13);
  for (uint32_t i = 0; i < CCORD_GWPACE_IDENTIFY_PER_HOUR; i++) ccord_gwpace_identify_sent(&h, 1000 + (uint64_t)i * 10000);
  CT_CHECK(ccord_gwpace_plan_reconnect(&h, now, CCORD_GWPACE_CLOSED, true) == 0);
  // The history is bounded: more identifies than the budget keep only the most recent.
  for (int i = 0; i < 50; i++) ccord_gwpace_identify_sent(&h, now + (uint64_t)i * 1000);
  CT_CHECK(h.identify_n == CCORD_GWPACE_IDENTIFY_PER_HOUR);
}

static void test_reset_keeps_process_wide_state(void)
{
  ccord_gwpace p;
  ccord_gwpace_init(&p, 21);
  ccord_gwpace_identify_sent(&p, 5000);
  for (int i = 0; i < 40; i++) CT_CHECK(ccord_gwpace_send_allowed(&p, 5000, false));
  ccord_gwpace_hello(&p, 5000, INTERVAL);
  (void)plan(&p, 5000, CCORD_GWPACE_NETWORK, true);
  (void)plan(&p, 5000, CCORD_GWPACE_NETWORK, true);
  ccord_gwpace_reset(&p);
  CT_CHECK(p.fail_streak == 0 && !ccord_gwpace_reconnect_pending(&p));
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, UINT64_MAX / 2) == CCORD_GWPACE_BEAT_NONE);
  // What belongs to the process, not the connection, survives: restarting cannot be used to dodge the limits.
  CT_CHECK(p.identify_n == 1);
  CT_CHECK(ccord_gwpace_sent_in_window(&p, 5001) == 40);
}

static void test_send_limiter(void)
{
  ccord_gwpace p;
  ccord_gwpace_init(&p, 1);
  size_t ordinary = CCORD_GWPACE_SEND_LIMIT - CCORD_GWPACE_SEND_RESERVE;
  for (size_t i = 0; i < ordinary; i++) CT_CHECK(ccord_gwpace_send_allowed(&p, 1000, false));
  CT_CHECK(!ccord_gwpace_send_allowed(&p, 1000, false));   // ordinary traffic stops short of the limit ...
  for (size_t i = 0; i < CCORD_GWPACE_SEND_RESERVE; i++) CT_CHECK(ccord_gwpace_send_allowed(&p, 1000, true));  // ... heartbeats still fit
  CT_CHECK(!ccord_gwpace_send_allowed(&p, 1000, true));    // and even they never pass Discord's 120
  CT_CHECK(ccord_gwpace_sent_in_window(&p, 1000) == CCORD_GWPACE_SEND_LIMIT);
  CT_CHECK(!ccord_gwpace_send_allowed(&p, 1000 + CCORD_GWPACE_SEND_WINDOW_MS - 1, false));
  CT_CHECK(ccord_gwpace_send_allowed(&p, 1000 + CCORD_GWPACE_SEND_WINDOW_MS, false));   // the window slid past them
  CT_CHECK(ccord_gwpace_sent_in_window(&p, 1000 + CCORD_GWPACE_SEND_WINDOW_MS) == 1);

  // It is a sliding window, not a fixed one: payloads expire one batch at a time.
  ccord_gwpace s;
  ccord_gwpace_init(&s, 2);
  for (int i = 0; i < 100; i++) CT_CHECK(ccord_gwpace_send_allowed(&s, 0, false));
  for (int i = 0; i < 12; i++) CT_CHECK(ccord_gwpace_send_allowed(&s, 30000, false));
  CT_CHECK(!ccord_gwpace_send_allowed(&s, 40000, false));                // 112 in the window
  CT_CHECK(ccord_gwpace_sent_in_window(&s, 60000) == 12);                // the first batch expired
  for (int i = 0; i < 100; i++) CT_CHECK(ccord_gwpace_send_allowed(&s, 60000, false));
  CT_CHECK(!ccord_gwpace_send_allowed(&s, 60000, false));
  CT_CHECK(!ccord_gwpace_send_allowed(nullptr, 0, true));
}

// ---- wake (sleep / network change) ----

#define WAKE_T0 100000u

static ccord_gwpace_wake_verdict wake(ccord_gwpace *p, uint64_t now, bool changed, bool resumable)
{
  return ccord_gwpace_plan_wake(p, now, changed, resumable);
}

static void test_wake_skips_an_accumulated_backoff(void)
{
  ccord_gwpace p;
  ccord_gwpace_init(&p, 31);
  uint64_t t = 0;
  for (int i = 0; i < 8; i++) { t += 1000; (void)plan(&p, t, CCORD_GWPACE_NETWORK, true); }   // streak 8: ~30-60 s waits
  CT_CHECK(p.reconnect_at_ms > t + 10000);
  bool resume = false;
  CT_CHECK(!ccord_gwpace_reconnect_due(&p, t + 10000, &resume));
  uint64_t now = t + 10000;   // the attempt is still waiting: the wake must not call that FRESH
  CT_CHECK(wake(&p, now, true, true) == CCORD_GWPACE_WAKE_ACCEPTED);
  CT_CHECK(ccord_gwpace_reconnect_pending(&p));
  CT_CHECK(ccord_gwpace_reconnect_due(&p, now, &resume) && resume);   // due at once, RESUME
  CT_CHECK(p.fail_streak == 1);                                          // a failed wake attempt retries after 0.5-1 s
  uint32_t d = plan(&p, now, CCORD_GWPACE_NETWORK, true);
  CT_CHECK(d >= 500 && d <= 1000);
}

static void test_second_wake_under_five_seconds_is_coalesced(void)
{
  ccord_gwpace p;
  ccord_gwpace_init(&p, 32);
  CT_CHECK(wake(&p, WAKE_T0, false, true) == CCORD_GWPACE_WAKE_ACCEPTED);
  // Within 3 s the attempt it planned is "fresh" too, but the verdict is COALESCED: rule 1 is tested first.
  CT_CHECK(wake(&p, WAKE_T0 + 1, false, true) == CCORD_GWPACE_WAKE_COALESCED);
  CT_CHECK(wake(&p, WAKE_T0 + 2999, true, true) == CCORD_GWPACE_WAKE_COALESCED);
  CT_CHECK(wake(&p, WAKE_T0 + 3000, true, true) == CCORD_GWPACE_WAKE_COALESCED);
  CT_CHECK(wake(&p, WAKE_T0 + 4999, false, true) == CCORD_GWPACE_WAKE_COALESCED);
  CT_CHECK(p.wake_last_ms == WAKE_T0 && p.wake_streak == 1);   // a dropped event changes nothing
  CT_CHECK(wake(&p, WAKE_T0 + 5000, false, true) == CCORD_GWPACE_WAKE_ACCEPTED);
}

// Gaps after the 1st, 2nd, ... accepted CHANGED wake in a row: 5, 10, 20, 40, 80 s, then 80 s. Reading wake_streak
// before its increment (shift = streak instead of streak - 1) makes the first gap 10 s and fails at the first step.
static void test_changed_gap_sequence(void)
{
  static const uint64_t gaps[] = {5000, 10000, 20000, 40000, 80000, 80000, 80000};
  ccord_gwpace p;
  ccord_gwpace_init(&p, 33);
  uint64_t t = WAKE_T0;
  CT_CHECK(wake(&p, t, true, true) == CCORD_GWPACE_WAKE_ACCEPTED);
  for (size_t i = 0; i < sizeof gaps / sizeof gaps[0]; i++) {
    CT_CHECK(p.wake_streak == i + 1);
    CT_CHECK(wake(&p, t + gaps[i] - 1u, true, true) == CCORD_GWPACE_WAKE_COALESCED);
    CT_CHECK(wake(&p, t + gaps[i], true, true) == CCORD_GWPACE_WAKE_ACCEPTED);
    t += gaps[i];
  }
}

static void test_resumed_gap_stays_five_seconds(void)
{
  ccord_gwpace p;
  ccord_gwpace_init(&p, 34);
  uint64_t t = WAKE_T0;
  CT_CHECK(wake(&p, t, false, true) == CCORD_GWPACE_WAKE_ACCEPTED);
  for (int i = 0; i < 6; i++) {
    CT_CHECK(wake(&p, t + 4999, false, true) == CCORD_GWPACE_WAKE_COALESCED);
    CT_CHECK(wake(&p, t + 5000, false, true) == CCORD_GWPACE_WAKE_ACCEPTED);
    t += 5000;
  }
  // A resume after flapping is held to 5 s, a network change right after it to the grown gap.
  CT_CHECK(p.wake_streak == 7);
  CT_CHECK(wake(&p, t + 5000, true, true) == CCORD_GWPACE_WAKE_COALESCED);
}

static void test_fresh_only_when_no_wake_was_accepted(void)
{
  ccord_gwpace p;
  ccord_gwpace_init(&p, 35);
  ccord_gwpace_connected(&p, WAKE_T0);
  CT_CHECK(p.wake_last_ms == 0);
  CT_CHECK(wake(&p, WAKE_T0 + 1000, true, true) == CCORD_GWPACE_WAKE_FRESH);
  CT_CHECK(wake(&p, WAKE_T0 + 2999, false, true) == CCORD_GWPACE_WAKE_FRESH);
  CT_CHECK(!ccord_gwpace_reconnect_pending(&p) && p.wake_last_ms == 0 && p.wake_streak == 0);   // FRESH changes nothing
  CT_CHECK(wake(&p, WAKE_T0 + 3000, false, true) == CCORD_GWPACE_WAKE_ACCEPTED);                // 3 s old: no longer fresh

  // An attempt that began a moment ago (backoff-paced, no wake) is fresh as well ...
  ccord_gwpace q;
  ccord_gwpace_init(&q, 36);
  CT_CHECK(ccord_gwpace_plan_reconnect(&q, WAKE_T0, CCORD_GWPACE_CLOSED, true) == 0);   // due at once
  CT_CHECK(wake(&q, WAKE_T0 + 2999, true, true) == CCORD_GWPACE_WAKE_FRESH);
  CT_CHECK(wake(&q, WAKE_T0 + 3000, true, true) == CCORD_GWPACE_WAKE_ACCEPTED);

  // ... but once a wake was accepted, the same young connection reports COALESCED, never FRESH.
  ccord_gwpace r;
  ccord_gwpace_init(&r, 37);
  CT_CHECK(wake(&r, WAKE_T0, true, true) == CCORD_GWPACE_WAKE_ACCEPTED);
  ccord_gwpace_connected(&r, WAKE_T0 + 500);
  CT_CHECK(wake(&r, WAKE_T0 + 1000, true, true) == CCORD_GWPACE_WAKE_COALESCED);
}

static void test_wake_during_the_first_handshake_is_accepted(void)
{
  // ccord_gateway_start: no attempt recorded, nothing connected, no wake yet.
  ccord_gwpace p;
  ccord_gwpace_init(&p, 38);
  ccord_gwpace_hello(&p, WAKE_T0, INTERVAL);   // HELLO arrived, READY has not
  CT_CHECK(p.connected_since_ms == 0 && p.attempt_ms == 0 && p.wake_last_ms == 0);
  CT_CHECK(wake(&p, WAKE_T0 + 500, true, false) == CCORD_GWPACE_WAKE_ACCEPTED);
  bool resume = true;
  CT_CHECK(ccord_gwpace_reconnect_due(&p, WAKE_T0 + 500, &resume) && !resume);   // no session, no identify yet: at once
}

static void test_wake_without_a_session_keeps_the_identify_spacing(void)
{
  ccord_gwpace p;
  ccord_gwpace_init(&p, 39);
  ccord_gwpace_identify_sent(&p, WAKE_T0);
  CT_CHECK(wake(&p, WAKE_T0 + 1000, true, false) == CCORD_GWPACE_WAKE_ACCEPTED);
  bool resume = true;
  CT_CHECK(!ccord_gwpace_reconnect_due(&p, WAKE_T0 + 4999, &resume));
  CT_CHECK(ccord_gwpace_reconnect_due(&p, WAKE_T0 + 5000, &resume) && !resume);   // 5 s after the last IDENTIFY
  CT_CHECK(p.fail_streak == 1);
}

static void test_wake_streak_resets_when_stable_or_restarted(void)
{
  ccord_gwpace p;
  ccord_gwpace_init(&p, 40);
  CT_CHECK(wake(&p, WAKE_T0, true, true) == CCORD_GWPACE_WAKE_ACCEPTED);
  CT_CHECK(wake(&p, WAKE_T0 + 5000, true, true) == CCORD_GWPACE_WAKE_ACCEPTED);   // streak 2: next gap 10 s
  CT_CHECK(wake(&p, WAKE_T0 + 14000, true, true) == CCORD_GWPACE_WAKE_COALESCED);
  ccord_gwpace_connected(&p, WAKE_T0 + 6000);
  ccord_gwpace_tick(&p, WAKE_T0 + 6000 + CCORD_GWPACE_STABLE_MS - 1u);
  CT_CHECK(p.wake_streak == 2);
  ccord_gwpace_tick(&p, WAKE_T0 + 6000 + CCORD_GWPACE_STABLE_MS);
  CT_CHECK(p.wake_streak == 0 && p.fail_streak == 0);
  uint64_t t = WAKE_T0 + 6000 + CCORD_GWPACE_STABLE_MS;
  CT_CHECK(wake(&p, t, true, true) == CCORD_GWPACE_WAKE_ACCEPTED);   // 5 s gap again: a lid opened every few minutes
  CT_CHECK(wake(&p, t + 4999, true, true) == CCORD_GWPACE_WAKE_COALESCED);
  CT_CHECK(wake(&p, t + 5000, true, true) == CCORD_GWPACE_WAKE_ACCEPTED);
  ccord_gwpace_reset(&p);
  CT_CHECK(p.wake_streak == 0);
  CT_CHECK(!ccord_gwpace_plan_wake(nullptr, 0, true, true));   // IGNORED
}

static void test_probe_deadline_and_background_backoff(void)
{
  // Probe tightens ACK deadline to 5 s; receiving OP 11 restores the full heartbeat interval.
  ccord_gwpace p;
  ccord_gwpace_init(&p, 41);
  ccord_gwpace_hello(&p, 1000, INTERVAL);
  ccord_gwpace_heartbeat_sent(&p, 2000);
  CT_CHECK(p.hb_next_ms == 2000 + INTERVAL);
  ccord_gwpace_probe_deadline(&p, 2000, 5000);
  CT_CHECK(p.hb_next_ms == 7000);
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, 6999) == CCORD_GWPACE_BEAT_NONE);
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, 7000) == CCORD_GWPACE_BEAT_ZOMBIE);
  // If ACK lands at 2080, full interval (2000 + INTERVAL) is restored.
  CT_CHECK(ccord_gwpace_heartbeat_ack(&p, 2080) == 80);
  CT_CHECK(p.hb_next_ms == 2000 + INTERVAL);
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, 7000) == CCORD_GWPACE_BEAT_NONE);
  CT_CHECK(ccord_gwpace_heartbeat_due(&p, 2000 + INTERVAL) == CCORD_GWPACE_BEAT_SEND);

  // Background mode raises reconnect backoff ceiling to 5 min (CCORD_GWPACE_BG_BACKOFF_MAX_MS).
  ccord_gwpace q;
  ccord_gwpace_init(&q, 42);
  ccord_gwpace_set_background(&q, true);
  uint32_t max_seen = 0;
  for (int i = 0; i < 12; i++) {
    uint32_t d = ccord_gwpace_plan_reconnect(&q, (uint64_t)i * 1000u, CCORD_GWPACE_NETWORK, true);
    if (d > max_seen) max_seen = d;
    CT_CHECK(d <= CCORD_GWPACE_BG_BACKOFF_MAX_MS);
  }
  CT_CHECK(max_seen > CCORD_GWPACE_BACKOFF_MAX_MS);
  CT_CHECK(max_seen >= CCORD_GWPACE_BG_BACKOFF_MAX_MS / 2u);
}

int main(void)
{
  test_first_heartbeat_is_jittered();
  test_ack_and_zombie_detection();
  test_backoff_grows_and_is_jittered();
  test_invalid_session_waits_one_to_five_seconds();
  test_requested_reconnect_resumes_immediately_once();
  test_stable_connection_earns_the_streak_back();
  test_identify_is_spaced_and_capped();
  test_reset_keeps_process_wide_state();
  test_send_limiter();
  test_wake_skips_an_accumulated_backoff();
  test_second_wake_under_five_seconds_is_coalesced();
  test_changed_gap_sequence();
  test_resumed_gap_stays_five_seconds();
  test_fresh_only_when_no_wake_was_accepted();
  test_wake_during_the_first_handshake_is_accepted();
  test_wake_without_a_session_keeps_the_identify_spacing();
  test_wake_streak_resets_when_stable_or_restarted();
  test_probe_deadline_and_background_backoff();
  return CT_RESULT("test_gateway_pace");
}
