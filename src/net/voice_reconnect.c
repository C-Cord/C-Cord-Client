// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/net/voice_reconnect.h>

#include <string.h>

#define VC_REJOIN_MIN_MS 1000u

static const uint32_t vc_delay_ms[CCORD_VC_MAX_ATTEMPTS] = {250, 1000, 2000, 4000, 8000, 16000};

static const char vc_lost[] = "connection lost";

// xorshift64*, as in gateway_pace.c. Never returns to an all-zero state.
static uint64_t vc_rand(ccord_vc *p)
{
  uint64_t x = p->rng;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  p->rng = x;
  return x * 0x2545F4914F6CDD1DULL;
}

void ccord_vc_init(ccord_vc *p, uint64_t seed)
{
  if (!p) return;
  memset(p, 0, sizeof *p);
  p->rng = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

ccord_vc_action ccord_vc_classify(uint16_t close_code, ccord_vc_cause cause, bool established)
{
  (void)cause;   // a close code in 4000..4999 decides on its own; every other failure is a dead socket
  switch (close_code) {
    case 4003:
    case 4005: return CCORD_VC_REIDENTIFY;
    case 4004:
    case 4006:
    case 4009:
    case 4011: return CCORD_VC_REJOIN;
    case 4014:
    case 4022: return CCORD_VC_AWAIT_MAIN;
    case 4001:
    case 4002:
    case 4012:
    case 4016:
    case 4017:
    case 4020:
    case 4021: return CCORD_VC_FINAL;
    default: break;
  }
  if (close_code >= 4000 && close_code <= 4999 && close_code != 4015) return CCORD_VC_REJOIN;
  return established ? CCORD_VC_RESUME : CCORD_VC_REIDENTIFY;   // incl. 4015 "try resuming"
}

static const char *vc_final_reason(uint16_t code)
{
  switch (code) {
    case 4021: return "rate limited";
    case 4017: return "this channel needs end-to-end encryption this build cannot do";
    case 4001: return "protocol error (code 4001)";
    case 4002: return "protocol error (code 4002)";
    case 4012: return "protocol error (code 4012)";
    case 4016: return "protocol error (code 4016)";
    case 4020: return "protocol error (code 4020)";
    default: return vc_lost;
  }
}

ccord_vc_plan ccord_vc_plan_next(ccord_vc *p, uint64_t now_ms, uint16_t close_code, ccord_vc_cause cause,
                                 bool established)
{
  ccord_vc_plan plan = {.action = CCORD_VC_FINAL, .delay_ms = 0, .attempt = 0, .reason = vc_lost};
  if (!p) return plan;
  p->established_since_ms = 0;   // the connection is gone; the stable timer restarts at the next op 4
  if (p->attempt == 0 && p->first_fail_ms == 0) p->first_fail_ms = now_ms ? now_ms : 1;   // outage clock

  ccord_vc_action a = ccord_vc_classify(close_code, cause, established);
  if (a == CCORD_VC_FINAL) {
    plan.reason = vc_final_reason(close_code);
    return plan;
  }
  if (a == CCORD_VC_AWAIT_MAIN) {
    plan.action = a;
    plan.delay_ms = CCORD_VC_AWAIT_MAIN_MS;
    plan.reason = "";
    return plan;
  }
  if (close_code == 4004 && ++p->auth_failures >= 2) {   // stale token: fresh credentials once, never twice
    plan.reason = "authentication failed";
    return plan;
  }
  if (p->attempt >= CCORD_VC_MAX_ATTEMPTS || (now_ms > p->first_fail_ms && now_ms - p->first_fail_ms >= CCORD_VC_GIVEUP_MS)) return plan;
  if (a == CCORD_VC_RESUME) {
    if (p->resume_tries >= CCORD_VC_RESUME_TRIES) a = CCORD_VC_REJOIN;
    else p->resume_tries++;
  }
  p->attempt++;
  uint32_t delay = vc_delay_ms[p->attempt - 1];
  if (p->attempt >= 2) delay = (uint32_t)((uint64_t)delay * (800u + vc_rand(p) % 401u) / 1000u);   // +-20 %
  if (a == CCORD_VC_REJOIN && delay < VC_REJOIN_MIN_MS) delay = VC_REJOIN_MIN_MS;
  plan.action = a;
  plan.delay_ms = delay;
  plan.attempt = p->attempt;
  plan.reason = "";
  return plan;
}

void ccord_vc_established(ccord_vc *p, uint64_t now_ms)
{
  if (p) p->established_since_ms = now_ms ? now_ms : 1;
}

void ccord_vc_tick(ccord_vc *p, uint64_t now_ms)
{
  if (!p || p->established_since_ms == 0 || now_ms < p->established_since_ms) return;
  if (now_ms - p->established_since_ms < CCORD_VC_STABLE_MS) return;
  p->attempt = p->resume_tries = p->auth_failures = 0;
  p->first_fail_ms = 0;   // the next failure starts a new outage
}

bool ccord_vc_accept_network_event(ccord_vc *p, uint64_t now_ms)
{
  if (!p) return false;
  if (p->last_event_ms != 0 && now_ms >= p->last_event_ms && now_ms - p->last_event_ms < CCORD_VC_EVENT_MIN_GAP_MS)
    return false;
  p->last_event_ms = now_ms ? now_ms : 1;
  return true;
}

const char *ccord_vc_action_name(ccord_vc_action a)
{
  switch (a) {
    case CCORD_VC_RESUME: return "resume";
    case CCORD_VC_REIDENTIFY: return "reidentify";
    case CCORD_VC_REJOIN: return "rejoin";
    case CCORD_VC_AWAIT_MAIN: return "await_main";
    case CCORD_VC_FINAL: return "final";
  }
  return "unknown";
}
