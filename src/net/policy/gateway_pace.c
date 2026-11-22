// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/net/gateway_pace.h>

#include <string.h>

#define GWPACE_HOUR_MS 3600000u
#define GWPACE_STREAK_MAX 16u
#define GWPACE_INVALID_MIN_MS 1000u
#define GWPACE_INVALID_SPAN_MS 4000u   // 1..5 s, as Discord documents for an invalid session

// xorshift64*: small, fast, good enough for jitter. Never returns to an all-zero state.
static uint64_t pace_rand(ccord_gwpace *p)
{
  uint64_t x = p->rng;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  p->rng = x;
  return x * 0x2545F4914F6CDD1DULL;
}

// Uniform-enough value in [0, n]; n == 0 gives 0.
static uint64_t pace_rand_upto(ccord_gwpace *p, uint64_t n)
{
  return n == 0 ? 0 : (pace_rand(p) >> 11) % (n + 1u);
}

void ccord_gwpace_init(ccord_gwpace *p, uint64_t seed)
{
  if (!p) return;
  memset(p, 0, sizeof *p);
  p->rng = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

// ---- heartbeat ----

void ccord_gwpace_hello(ccord_gwpace *p, uint64_t now_ms, uint32_t interval_ms)
{
  if (!p) return;
  if (interval_ms == 0) interval_ms = 41250;   // Discord's usual value, in case HELLO is malformed
  p->hb_active = true;
  p->hb_awaiting_ack = false;
  p->hb_interval_ms = interval_ms;
  p->hb_sent_ms = 0;
  p->hb_next_ms = now_ms + pace_rand_upto(p, interval_ms - 1u);   // interval * random(0..1)
}

ccord_gwpace_beat ccord_gwpace_heartbeat_due(const ccord_gwpace *p, uint64_t now_ms)
{
  if (!p || !p->hb_active || now_ms < p->hb_next_ms) return CCORD_GWPACE_BEAT_NONE;
  return p->hb_awaiting_ack ? CCORD_GWPACE_BEAT_ZOMBIE : CCORD_GWPACE_BEAT_SEND;
}

void ccord_gwpace_heartbeat_sent(ccord_gwpace *p, uint64_t now_ms)
{
  if (!p || !p->hb_active) return;
  p->hb_awaiting_ack = true;
  p->hb_sent_ms = now_ms;
  p->hb_next_ms = now_ms + p->hb_interval_ms;
}

uint32_t ccord_gwpace_heartbeat_ack(ccord_gwpace *p, uint64_t now_ms)
{
  if (!p || !p->hb_awaiting_ack) return 0;
  p->hb_awaiting_ack = false;
  // If a foreground resume probe tightened hb_next_ms to a short ACK deadline, restore the full interval now.
  uint64_t full_next = p->hb_sent_ms + p->hb_interval_ms;
  if (p->hb_active && p->hb_next_ms < full_next) p->hb_next_ms = full_next;
  uint64_t rtt = now_ms >= p->hb_sent_ms ? now_ms - p->hb_sent_ms : 0;
  if (rtt > UINT32_MAX) rtt = UINT32_MAX;
  p->hb_latency_ms = rtt ? (uint32_t)rtt : 1u;   // 0 is reserved for "no heartbeat was outstanding"
  return p->hb_latency_ms;
}

void ccord_gwpace_probe_deadline(ccord_gwpace *p, uint64_t now_ms, uint32_t ack_timeout_ms)
{
  if (!p || !p->hb_active || !p->hb_awaiting_ack) return;
  if (ack_timeout_ms == 0) ack_timeout_ms = 5000u;
  uint64_t cap = now_ms + ack_timeout_ms;
  if (p->hb_next_ms > cap) p->hb_next_ms = cap;
}

void ccord_gwpace_set_background(ccord_gwpace *p, bool background)
{
  if (p) p->background = background;
}

// ---- reconnect ----

// Delay before the next attempt for a given number of consecutive failures: none for the first (one reconnect after
// a transient drop is normal), then 1 s, 2 s, 4 s ... up to a minute (5 minutes while backgrounded), each reduced by
// up to half at random.
static uint64_t pace_backoff(ccord_gwpace *p)
{
  if (p->fail_streak == 0) return 0;
  uint32_t max_shift = p->background ? 9u : 6u;
  uint64_t cap = p->background ? CCORD_GWPACE_BG_BACKOFF_MAX_MS : CCORD_GWPACE_BACKOFF_MAX_MS;
  uint32_t shift = p->fail_streak - 1u;
  if (shift > max_shift) shift = max_shift;
  uint64_t full = (uint64_t)CCORD_GWPACE_BACKOFF_BASE_MS << shift;
  if (full > cap) full = cap;
  return full / 2u + pace_rand_upto(p, full / 2u);
}

uint32_t ccord_gwpace_plan_reconnect(ccord_gwpace *p, uint64_t now_ms, ccord_gwpace_reason reason, bool resumable)
{
  if (!p) return 0;
  uint64_t delay = pace_backoff(p);
  if (reason == CCORD_GWPACE_INVALID_SESSION) {
    uint64_t wait = GWPACE_INVALID_MIN_MS + pace_rand_upto(p, GWPACE_INVALID_SPAN_MS);
    if (wait > delay) delay = wait;
  }
  if (p->fail_streak < GWPACE_STREAK_MAX) p->fail_streak++;
  bool resume = resumable;
  if (!resume && p->identify_n > 0) {
    // A fresh IDENTIFY is spaced at least 5 s after the previous one ...
    uint64_t earliest = p->identify_ms[p->identify_n - 1u] + CCORD_GWPACE_IDENTIFY_MIN_GAP_MS;
    if (earliest > now_ms && earliest - now_ms > delay) delay = earliest - now_ms;
    // ... and a loop of them is held back until the oldest one is an hour old.
    if (p->identify_n >= CCORD_GWPACE_IDENTIFY_PER_HOUR) {
      uint64_t release = p->identify_ms[0] + GWPACE_HOUR_MS;
      if (release > now_ms && release - now_ms > delay) delay = release - now_ms;
    }
  }
  if (delay > UINT32_MAX) delay = UINT32_MAX;
  p->reconnect_pending = true;
  p->reconnect_resume = resume;
  p->reconnect_at_ms = now_ms + delay;
  p->attempt_ms = p->reconnect_at_ms ? p->reconnect_at_ms : 1u;   // 0 means "no attempt"
  return (uint32_t)delay;
}

bool ccord_gwpace_reconnect_due(const ccord_gwpace *p, uint64_t now_ms, bool *resume)
{
  if (!p || !p->reconnect_pending || now_ms < p->reconnect_at_ms) return false;
  if (resume) *resume = p->reconnect_resume;
  return true;
}

bool ccord_gwpace_reconnect_pending(const ccord_gwpace *p)
{
  return p && p->reconnect_pending;
}

void ccord_gwpace_reconnect_started(ccord_gwpace *p)
{
  if (p) p->reconnect_pending = false;
}

void ccord_gwpace_identify_sent(ccord_gwpace *p, uint64_t now_ms)
{
  if (!p) return;
  if (p->identify_n == CCORD_GWPACE_IDENTIFY_PER_HOUR) {
    memmove(p->identify_ms, p->identify_ms + 1, (p->identify_n - 1u) * sizeof p->identify_ms[0]);
    p->identify_n--;
  }
  p->identify_ms[p->identify_n++] = now_ms;
}

void ccord_gwpace_reset(ccord_gwpace *p)
{
  if (!p) return;
  p->hb_active = false;
  p->hb_awaiting_ack = false;
  p->hb_next_ms = 0;
  p->fail_streak = 0;
  p->wake_streak = 0;
  p->reconnect_pending = false;
  p->connected_since_ms = 0;
}

void ccord_gwpace_connected(ccord_gwpace *p, uint64_t now_ms)
{
  if (!p) return;
  p->connected_since_ms = now_ms ? now_ms : 1u;
}

void ccord_gwpace_tick(ccord_gwpace *p, uint64_t now_ms)
{
  if (!p || p->connected_since_ms == 0) return;
  if (now_ms >= p->connected_since_ms && now_ms - p->connected_since_ms >= CCORD_GWPACE_STABLE_MS) {
    p->fail_streak = 0;
    p->wake_streak = 0;
  }
}

void ccord_gwpace_disconnected(ccord_gwpace *p)
{
  if (!p) return;
  p->connected_since_ms = 0;
  p->hb_active = false;
  p->hb_awaiting_ack = false;
}

// ---- wake ----

// Gap that must pass after an accepted wake. `wake_streak` already counts that wake (it was incremented on accept), so
// the first gap uses shift 0: reading it before the increment would make the first gap 10 s instead of 5 s.
static uint64_t pace_wake_gap(const ccord_gwpace *p, bool net_changed)
{
  if (!net_changed || p->wake_streak == 0) return CCORD_GWPACE_WAKE_GAP_MS;
  uint32_t shift = p->wake_streak - 1u;
  if (shift > CCORD_GWPACE_WAKE_GAP_MAX_SHIFT) shift = CCORD_GWPACE_WAKE_GAP_MAX_SHIFT;
  return (uint64_t)CCORD_GWPACE_WAKE_GAP_MS << shift;
}

ccord_gwpace_wake_verdict ccord_gwpace_plan_wake(ccord_gwpace *p, uint64_t now_ms, bool net_changed, bool resumable)
{
  if (!p) return CCORD_GWPACE_WAKE_IGNORED;
  // Rule 1 first: an accepted wake sets attempt_ms = now, so testing FRESH first would label a burst FRESH.
  if (p->wake_last_ms != 0 && now_ms >= p->wake_last_ms && now_ms - p->wake_last_ms < pace_wake_gap(p, net_changed))
    return CCORD_GWPACE_WAKE_COALESCED;
  if (p->connected_since_ms != 0 && now_ms >= p->connected_since_ms &&
      now_ms - p->connected_since_ms < CCORD_GWPACE_WAKE_FRESH_MS)
    return CCORD_GWPACE_WAKE_FRESH;
  // An attempt still in the future is a backoff wait: exactly what a wake should cut short.
  if (p->attempt_ms != 0 && now_ms >= p->attempt_ms && now_ms - p->attempt_ms < CCORD_GWPACE_WAKE_FRESH_MS)
    return CCORD_GWPACE_WAKE_FRESH;
  p->wake_last_ms = now_ms ? now_ms : 1u;
  if (p->wake_streak < GWPACE_STREAK_MAX) p->wake_streak++;
  p->fail_streak = 0;
  (void)ccord_gwpace_plan_reconnect(p, now_ms, CCORD_GWPACE_NETWORK, resumable);
  return CCORD_GWPACE_WAKE_ACCEPTED;
}

// ---- outbound sends ----

static void pace_prune(ccord_gwpace *p, uint64_t now_ms)
{
  while (p->sent_n > 0) {
    uint64_t oldest = p->sent_ms[p->sent_head];
    if (now_ms < oldest || now_ms - oldest < CCORD_GWPACE_SEND_WINDOW_MS) break;
    p->sent_head = (p->sent_head + 1u) % CCORD_GWPACE_SEND_LIMIT;
    p->sent_n--;
  }
}

bool ccord_gwpace_send_allowed(ccord_gwpace *p, uint64_t now_ms, bool critical)
{
  if (!p) return false;
  pace_prune(p, now_ms);
  size_t cap = critical ? CCORD_GWPACE_SEND_LIMIT : CCORD_GWPACE_SEND_LIMIT - CCORD_GWPACE_SEND_RESERVE;
  if (p->sent_n >= cap) return false;
  p->sent_ms[(p->sent_head + p->sent_n) % CCORD_GWPACE_SEND_LIMIT] = now_ms;
  p->sent_n++;
  return true;
}

size_t ccord_gwpace_sent_in_window(ccord_gwpace *p, uint64_t now_ms)
{
  if (!p) return 0;
  pace_prune(p, now_ms);
  return p->sent_n;
}
