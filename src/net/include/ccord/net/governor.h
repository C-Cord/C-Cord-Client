// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Request governor: the single place that decides whether a Discord REST request may start now.
//
// Pure logic, no I/O, no curl, no wall clock: every call takes `now_ms` so the behaviour is deterministic and
// unit-testable. The REST queue asks `ccord_gov_acquire` before starting a request and reports the outcome with
// `ccord_gov_complete`. Policy:
//   * per-route bucket tracking from X-RateLimit-* headers, global limit, Retry-After with positive jitter;
//   * priority classes (user action > visible state > background) with a global token bucket that keeps a reserve
//     for higher priorities;
//   * bounded in-flight requests;
//   * circuit breaker: 401 latches everything off, repeated 5xx/network errors back off exponentially, an edge
//     (Cloudflare) block or a burst of invalid requests (401/403/429) halts all but a single user-action probe.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ccord_gov_priority : uint8_t {
  CCORD_GOV_USER = 0,        // a direct user action (send, edit, react, join, ...)
  CCORD_GOV_VISIBLE = 1,     // data for what is on screen right now (history, profile being viewed)
  CCORD_GOV_BACKGROUND = 2,  // prefetch, refresh, housekeeping
  CCORD_GOV_PRIORITY_COUNT
} ccord_gov_priority;

typedef enum ccord_gov_verdict : uint8_t {
  CCORD_GOV_GO = 0,   // start the request now; the ticket must be completed
  CCORD_GOV_WAIT,     // not yet; ask again at or after retry_at_ms
  CCORD_GOV_REFUSE    // do not send until the condition is cleared (auth latch)
} ccord_gov_verdict;

typedef enum ccord_gov_reason : uint8_t {
  CCORD_GOV_REASON_NONE = 0,
  CCORD_GOV_REASON_INFLIGHT,  // too many requests in flight
  CCORD_GOV_REASON_BUCKET,    // the route's rate-limit bucket is exhausted or blocked
  CCORD_GOV_REASON_GLOBAL,    // a global rate limit is active
  CCORD_GOV_REASON_BACKOFF,   // recent server/network failures
  CCORD_GOV_REASON_TOKENS,    // our own pacing budget
  CCORD_GOV_REASON_EDGE,      // edge (Cloudflare) block or invalid-request budget exceeded
  CCORD_GOV_REASON_AUTH       // the token was rejected (HTTP 401)
} ccord_gov_reason;

typedef struct ccord_gov_config {
  uint32_t max_inflight;          // concurrent requests for VISIBLE/BACKGROUND (0 -> 4)
  uint32_t user_extra_inflight;   // extra slots only USER requests may use (0 -> 2)
  double tokens_per_sec;          // sustained pacing budget (<=0 -> 8)
  double token_capacity;          // burst size (<=0 -> 24)
  double visible_reserve;         // VISIBLE needs >= 1 + this many tokens (<0 -> 2)
  double background_reserve;      // BACKGROUND needs >= 1 + this many tokens (<0 -> 8)
  uint64_t base_backoff_ms;       // first failure backoff (0 -> 1000)
  uint64_t max_backoff_ms;        // backoff ceiling (0 -> 60000)
  uint32_t jitter_pct;            // positive jitter added to waits, 0..100 (0 -> 20)
  uint32_t invalid_limit;         // 401/403/429 responses per window that trip the edge breaker (0 -> 100)
  uint64_t invalid_window_ms;     // sliding window for invalid_limit (0 -> 600000)
  uint64_t edge_block_ms;         // first edge-block duration, doubles per strike (0 -> 300000)
  uint64_t seed;                  // jitter PRNG seed (0 -> fixed default)
} ccord_gov_config;

typedef struct ccord_gov ccord_gov;

// All defaults filled in. Start from this and override fields; a zero-initialised config also works for every
// field except the two reserves, where 0 means "no reserve" and only a negative value selects the default.
[[nodiscard]] ccord_gov_config ccord_gov_config_default(void);

// Handle returned by a GO decision; pass it back to ccord_gov_complete exactly once.
typedef struct ccord_gov_ticket {
  int32_t bucket;
  uint32_t bucket_gen;
  int32_t route;
  uint32_t route_gen;
  ccord_gov_priority prio;
  bool active;
} ccord_gov_ticket;

typedef struct ccord_gov_decision {
  ccord_gov_verdict verdict;
  ccord_gov_reason reason;
  uint64_t retry_at_ms;  // WAIT: earliest time worth asking again
} ccord_gov_decision;

// What the transport learned from the response (all fields optional; zero means "not present").
typedef struct ccord_gov_response {
  int status;                  // HTTP status; 0 for a network error or timeout
  bool network_error;          // transport-level failure (no HTTP status)
  char bucket[64];             // X-RateLimit-Bucket ("" when absent)
  int32_t limit;               // X-RateLimit-Limit (0 when absent)
  int32_t remaining;           // X-RateLimit-Remaining (-1 when absent)
  uint64_t reset_after_ms;     // X-RateLimit-Reset-After in ms (0 when absent)
  uint64_t retry_after_ms;     // Retry-After header or JSON body value in ms (0 when absent)
  bool global;                 // X-RateLimit-Global or "global": true
  bool edge_html;              // a 403/429 whose body is an HTML page (Cloudflare), not Discord JSON
  bool pre_auth;               // login/MFA request: a 401 counts as a normal 4xx, not a token rejection
} ccord_gov_response;

// Zeroes *r and sets the "header absent" markers (remaining = -1).
void ccord_gov_response_init(ccord_gov_response *r, int status);
// Feeds one raw response header line ("Name: value\r\n") into *r. Understands X-RateLimit-Bucket/Limit/Remaining/
// Reset-After/Global, Retry-After and Content-Type (text/html marks an edge block page). Unknown lines are ignored.
// Waits are clamped to one hour so a buggy or hostile server cannot park the client indefinitely.
void ccord_gov_response_header(ccord_gov_response *r, const char *line, size_t len);
// Reads the JSON body of a 429 ("retry_after", "global") when the headers did not already say, and flags an HTML
// body (Cloudflare block page) as edge_html.
void ccord_gov_response_body(ccord_gov_response *r, const char *body, size_t len);

typedef struct ccord_gov_completion {
  uint64_t not_before_ms;  // earliest sensible time to retry the same request (0 = no constraint)
  bool should_retry;       // 429, or a retryable server failure: the caller may requeue it
} ccord_gov_completion;

typedef struct ccord_gov_stats {
  uint64_t started[CCORD_GOV_PRIORITY_COUNT];
  uint64_t waits;
  uint64_t refusals;
  uint64_t responses_2xx, responses_4xx, responses_5xx, rate_limited, network_errors;
  uint64_t edge_trips;
  uint32_t inflight;
} ccord_gov_stats;

[[nodiscard]] ccord_gov *ccord_gov_create(const ccord_gov_config *cfg);
void ccord_gov_destroy(ccord_gov *g);

// Writes "METHOD /normalised/path" (query stripped; the first channel/guild/webhook id kept as the major
// parameter, other ids replaced with :id) and the major parameter ("0" when none). Returns false on overflow.
[[nodiscard]] bool ccord_gov_route_key(const char *method, const char *path, char *route, size_t route_cap,
                                       char *major, size_t major_cap);

[[nodiscard]] ccord_gov_decision ccord_gov_acquire(ccord_gov *g, const char *method, const char *path,
                                                   ccord_gov_priority prio, uint64_t now_ms,
                                                   ccord_gov_ticket *ticket);
// Like ccord_gov_acquire, but the 401 auth latch does not refuse the request: login/MFA must be able to run
// while a stale token is latched off. Rate limits, budgets and the edge breaker still apply.
[[nodiscard]] ccord_gov_decision ccord_gov_acquire_preauth(ccord_gov *g, const char *method, const char *path,
                                                           ccord_gov_priority prio, uint64_t now_ms,
                                                           ccord_gov_ticket *ticket);
[[nodiscard]] ccord_gov_completion ccord_gov_complete(ccord_gov *g, ccord_gov_ticket *ticket,
                                                      const ccord_gov_response *resp, uint64_t now_ms);

// Releases a ticket without reporting an outcome (the request never reached the network, or was abandoned).
void ccord_gov_cancel(ccord_gov *g, ccord_gov_ticket *ticket);

// Clears the 401 latch (call after a successful re-login).
void ccord_gov_reset_auth(ccord_gov *g);
[[nodiscard]] bool ccord_gov_auth_failed(const ccord_gov *g);
[[nodiscard]] ccord_gov_stats ccord_gov_get_stats(const ccord_gov *g);

// Per-route counters for `--net-stats`.
typedef void (*ccord_gov_route_cb)(void *ud, const char *route, uint32_t requests, uint32_t rate_limited,
                                   int last_status);
void ccord_gov_foreach_route(const ccord_gov *g, ccord_gov_route_cb cb, void *ud);

[[nodiscard]] const char *ccord_gov_reason_name(ccord_gov_reason r);
