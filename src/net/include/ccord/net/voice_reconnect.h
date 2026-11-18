// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Voice reconnect policy: what to do when a voice connection fails, and when.
//
// Pure logic, no I/O, no wall clock: every call takes `now_ms`, so the behaviour is deterministic and unit-testable.
// One ccord_vc covers one outage and is shared by the voice gateway (WebSocket resume) and the live transport
// (re-identify / rejoin), so the attempt budget is global per outage.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CCORD_VC_MAX_ATTEMPTS 6u          // attempts per outage, counted when an attempt starts
#define CCORD_VC_GIVEUP_MS 45000u         // wall time from the first failure of an outage
#define CCORD_VC_RESUME_TRIES 3u          // RESUME attempts before escalating to REJOIN
#define CCORD_VC_AWAIT_MAIN_MS 3000u      // grace for the main gateway verdict after 4014 / 4022
#define CCORD_VC_CONNECT_TIMEOUT_MS 3000u // per-attempt WebSocket connect timeout
#define CCORD_VC_REJOIN_WAIT_MS 8000u     // wait for the new VOICE_SERVER_UPDATE after a rejoin
#define CCORD_VC_STABLE_MS 10000u         // ESTABLISHED this long resets the budget
#define CCORD_VC_EVENT_MIN_GAP_MS 5000u   // a network-change / wake nudge is accepted at most once per gap

typedef enum ccord_vc_action : uint8_t {
  CCORD_VC_RESUME,       // same voice session: new WebSocket, op 7 Resume
  CCORD_VC_REIDENTIFY,   // same token/session/endpoint, new voice session
  CCORD_VC_REJOIN,       // credentials are stale: OP 4 leave + join on the main gateway
  CCORD_VC_AWAIT_MAIN,   // the voice server says "do not reconnect": wait for the main gateway's verdict
  CCORD_VC_FINAL         // cannot or must not recover
} ccord_vc_action;

typedef enum ccord_vc_cause : uint8_t {
  CCORD_VC_CAUSE_CLOSE,       // the server sent a close frame (close_code says which)
  CCORD_VC_CAUSE_IO,          // transport error / recv error / no close frame
  CCORD_VC_CAUSE_HEARTBEAT,   // heartbeat went unanswered
  CCORD_VC_CAUSE_UDP,         // IP discovery timeout
  CCORD_VC_CAUSE_CONNECT      // the WebSocket never connected
} ccord_vc_cause;

typedef struct ccord_vc {
  uint32_t attempt, resume_tries, auth_failures;
  uint64_t first_fail_ms, established_since_ms, last_event_ms, rng;
} ccord_vc;

typedef struct ccord_vc_plan {
  ccord_vc_action action;
  uint32_t delay_ms;          // wait before acting; 0 = now. AWAIT_MAIN: CCORD_VC_AWAIT_MAIN_MS
  uint32_t attempt;           // 1-based number of this attempt (0 for AWAIT_MAIN / FINAL)
  const char *reason;         // static text for FINAL (under 90 bytes), else ""
} ccord_vc_plan;

void ccord_vc_init(ccord_vc *p, uint64_t seed);

// Stateless classification of one failure. `close_code` 0 = no close frame. "Not established" (before op 4 Session
// Description) turns RESUME into REIDENTIFY: there is nothing to resume.
ccord_vc_action ccord_vc_classify(uint16_t close_code, ccord_vc_cause cause, bool established);

// Decides what to do about a failure and updates the budget. Starts the outage clock on its first call. Gives
// CCORD_VC_FINAL ("connection lost") after attempt CCORD_VC_MAX_ATTEMPTS or CCORD_VC_GIVEUP_MS, whichever is first.
// AWAIT_MAIN and FINAL never consume an attempt.
ccord_vc_plan ccord_vc_plan_next(ccord_vc *p, uint64_t now_ms, uint16_t close_code, ccord_vc_cause cause,
                                 bool established);

void ccord_vc_established(ccord_vc *p, uint64_t now_ms);   // the connection is up (op 4 seen)
void ccord_vc_tick(ccord_vc *p, uint64_t now_ms);          // after CCORD_VC_STABLE_MS established: the outage is over
bool ccord_vc_accept_network_event(ccord_vc *p, uint64_t now_ms);   // true at most once per CCORD_VC_EVENT_MIN_GAP_MS
const char *ccord_vc_action_name(ccord_vc_action a);
