// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Gateway pacing: how politely the client behaves on the Discord gateway connection.
//
// Pure logic, no I/O, no curl, no wall clock: every call takes `now_ms`, so the behaviour is deterministic and
// unit-testable (like the request governor). The gateway owns one of these and asks it three questions:
//
//   * heartbeat: when to send one, and whether the connection has gone silent. Discord expects the first heartbeat
//     after `interval * random(0..1)`, then one per interval. If the previous heartbeat was never acknowledged by
//     the time the next is due, the connection is a zombie: reconnect and resume instead of waiting forever.
//   * reconnect: when it is acceptable to try again. A reconnect is never immediate after a failure. Delays grow
//     exponentially with random jitter (so a Discord outage does not turn every client into a synchronized storm),
//     an "invalid session" waits 1-5 s as Discord asks, and a fresh IDENTIFY is spaced at least 5 s apart and
//     capped per hour (Discord allows 1000 per day and treats a loop of them as abuse). A connection that stays up
//     for a while earns its failure streak back.
//   * wake: the machine woke from sleep or the network changed. Plans an immediate reconnect, rate limited so a burst
//     of events (or a flapping VPN) cannot turn into a reconnect loop.
//   * outbound sends: Discord disconnects clients that exceed 120 gateway payloads per 60 s. A sliding window keeps
//     us under that and reserves a few slots so heartbeats can always go out.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CCORD_GWPACE_SEND_LIMIT 120u            // payloads per window (Discord's limit)
#define CCORD_GWPACE_SEND_WINDOW_MS 60000u
#define CCORD_GWPACE_SEND_RESERVE 8u            // slots only heartbeat / identify / resume may use
#define CCORD_GWPACE_IDENTIFY_MIN_GAP_MS 5000u
#define CCORD_GWPACE_IDENTIFY_PER_HOUR 6u
#define CCORD_GWPACE_BACKOFF_BASE_MS 1000u
#define CCORD_GWPACE_BACKOFF_MAX_MS 60000u
#define CCORD_GWPACE_STABLE_MS 30000u           // connected this long: the failure streak resets
#define CCORD_GWPACE_WAKE_GAP_MS 5000u          // minimum gap between accepted wakes; doubles per accepted CHANGED wake
#define CCORD_GWPACE_WAKE_GAP_MAX_SHIFT 4u      // 5, 10, 20, 40, 80 s
#define CCORD_GWPACE_WAKE_FRESH_MS 3000u        // an attempt or connection this young already postdates the event

typedef enum ccord_gwpace_beat : uint8_t {
  CCORD_GWPACE_BEAT_NONE = 0,   // nothing to do yet
  CCORD_GWPACE_BEAT_SEND,       // send a heartbeat now
  CCORD_GWPACE_BEAT_ZOMBIE      // the last heartbeat was never acknowledged: the connection is dead
} ccord_gwpace_beat;

typedef enum ccord_gwpace_reason : uint8_t {
  CCORD_GWPACE_RECONNECT_REQUESTED = 0,   // op 7: Discord asked us to reconnect (expected, resume at once)
  CCORD_GWPACE_INVALID_SESSION,           // op 9: wait 1-5 s, then resume or identify
  CCORD_GWPACE_CLOSED,                    // the server closed the connection
  CCORD_GWPACE_NETWORK,                   // connect or receive failed
  CCORD_GWPACE_ZOMBIE                     // heartbeats went unanswered
} ccord_gwpace_reason;

typedef enum ccord_gwpace_wake_verdict : uint8_t {
  CCORD_GWPACE_WAKE_IGNORED = 0,   // (returned by the gateway, never by the pace) not connected or connecting
  CCORD_GWPACE_WAKE_FRESH,         // an attempt began or a connection came up in the last 3 s, no accepted wake caused it
  CCORD_GWPACE_WAKE_COALESCED,     // too soon after the previous accepted wake (the rate-limit verdict)
  CCORD_GWPACE_WAKE_ACCEPTED       // reconnect planned: due now (after the IDENTIFY spacing when there is no session)
} ccord_gwpace_wake_verdict;

typedef struct ccord_gwpace {
  uint64_t rng;
  // heartbeat
  bool hb_active;
  bool hb_awaiting_ack;
  uint32_t hb_interval_ms;
  uint64_t hb_next_ms;
  uint64_t hb_sent_ms;
  uint32_t hb_latency_ms;      // last measured round trip (0 until the first ack)
  // reconnect
  uint32_t fail_streak;
  bool reconnect_pending;
  bool reconnect_resume;
  uint64_t reconnect_at_ms;
  uint64_t connected_since_ms;   // 0 while not connected
  // wake (sleep / network change)
  uint64_t wake_last_ms;         // when the last wake was accepted (0 = none since process start)
  uint32_t wake_streak;          // accepted wakes since the connection last held CCORD_GWPACE_STABLE_MS
  uint64_t attempt_ms;           // when the latest planned attempt starts (plan_reconnect: now + delay); 0 = none
  // fresh IDENTIFY history (the last CCORD_GWPACE_IDENTIFY_PER_HOUR, oldest first)
  uint64_t identify_ms[CCORD_GWPACE_IDENTIFY_PER_HOUR];
  size_t identify_n;
  // outbound payloads in the last window, oldest first
  uint64_t sent_ms[CCORD_GWPACE_SEND_LIMIT];
  size_t sent_head;
  size_t sent_n;
} ccord_gwpace;

// `seed` feeds the jitter; pass anything unpredictable (the clock and an address are enough). Same seed, same jitter.
void ccord_gwpace_init(ccord_gwpace *p, uint64_t seed);

// ---- heartbeat ----
// HELLO arrived: schedules the first heartbeat after interval * random(0..1) and forgets any earlier ack state.
void ccord_gwpace_hello(ccord_gwpace *p, uint64_t now_ms, uint32_t interval_ms);
[[nodiscard]] ccord_gwpace_beat ccord_gwpace_heartbeat_due(const ccord_gwpace *p, uint64_t now_ms);
// A heartbeat went out (either on schedule or because Discord asked with op 1): the next is one interval away.
void ccord_gwpace_heartbeat_sent(ccord_gwpace *p, uint64_t now_ms);
// op 11. Returns the round-trip time in ms (0 when no heartbeat was outstanding).
uint32_t ccord_gwpace_heartbeat_ack(ccord_gwpace *p, uint64_t now_ms);

// ---- reconnect ----
// Decides when to reconnect and whether to try RESUME. `resumable`: a session exists that Discord may still accept.
// Replaces any reconnect already pending. Returns the delay in ms (0 = immediately).
uint32_t ccord_gwpace_plan_reconnect(ccord_gwpace *p, uint64_t now_ms, ccord_gwpace_reason reason, bool resumable);
// True once the planned reconnect may start; `*resume` says whether to RESUME (false: fresh IDENTIFY).
[[nodiscard]] bool ccord_gwpace_reconnect_due(const ccord_gwpace *p, uint64_t now_ms, bool *resume);
[[nodiscard]] bool ccord_gwpace_reconnect_pending(const ccord_gwpace *p);
// The reconnect was started (its socket is opening). Clears the pending flag; the streak stays until it holds.
void ccord_gwpace_reconnect_started(ccord_gwpace *p);
// A fresh IDENTIFY was actually sent (not merely a socket opened: a dead network must not use up the budget).
void ccord_gwpace_identify_sent(ccord_gwpace *p, uint64_t now_ms);
// A new explicit start (or stop): forgets heartbeat, pending reconnect and the failure streak. Keeps the identify
// history, the outbound window and the jitter state, which belong to the process rather than to one connection.
void ccord_gwpace_reset(ccord_gwpace *p);
// READY or RESUMED arrived: the session is up. The failure streak resets once it has stayed up for a while.
void ccord_gwpace_connected(ccord_gwpace *p, uint64_t now_ms);
// Call regularly while connected; resets the streak after CCORD_GWPACE_STABLE_MS of uptime.
void ccord_gwpace_tick(ccord_gwpace *p, uint64_t now_ms);
// The connection is down (any reason): uptime stops counting.
void ccord_gwpace_disconnected(ccord_gwpace *p);

// ---- wake ----
// The machine resumed from sleep (`net_changed` false) or the network changed (`net_changed` true). Rules in order:
//   1. COALESCED when a wake was accepted before and less than the gap ago. The gap is CCORD_GWPACE_WAKE_GAP_MS for a
//      resume; for a network change it is CCORD_GWPACE_WAKE_GAP_MS << min(max(wake_streak - 1, 0), 4), where
//      `wake_streak` is read AFTER the previous accepted wake incremented it: gaps 5, 10, 20, 40, 80 s, then 80 s.
//      This comes first so the rest of a burst is COALESCED, not FRESH (an accepted wake sets attempt_ms = now).
//   2. FRESH when no wake caused it but a connection came up, or an attempt began, in the last 3 s.
//   3. Otherwise ACCEPTED: the failure streak is cleared and a reconnect is planned through ccord_gwpace_plan_reconnect
//      (so the IDENTIFY spacing and hourly cap still hold when `resumable` is false), due at once when resumable.
//      It leaves fail_streak == 1, so a failed attempt is retried after the normal 0.5-1 s, not immediately again.
// Never returns IGNORED: the caller decides that from the connection state.
ccord_gwpace_wake_verdict ccord_gwpace_plan_wake(ccord_gwpace *p, uint64_t now_ms, bool net_changed, bool resumable);

// ---- outbound sends ----
// Asks to send one payload. `critical` payloads (heartbeat, identify, resume) may use the reserved slots.
// On true the send is recorded; on false do not send (and retry later if it matters).
[[nodiscard]] bool ccord_gwpace_send_allowed(ccord_gwpace *p, uint64_t now_ms, bool critical);
// Payloads sent in the current window (for tests and diagnostics).
[[nodiscard]] size_t ccord_gwpace_sent_in_window(ccord_gwpace *p, uint64_t now_ms);
