// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice call-ring: starting a DM call rings the recipients; see verify_ui_parity.h.
//
// Drives the real header "Start Call" / "Join Call" button and the call bar's Leave button over the offline mock
// transport, and reads the mock's request counters: a call started in a DM with no call running sends one voice-state
// join and one POST /channels/{c}/call/ring; joining a call that is already running, or a guild voice channel, sends
// no ring; a refused ring shows Discord's reason in the notice and leaves the user in the call.
#include "verify_ui_parity.h"

#include <stdio.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/net/mock_transport.h>

#include "ui_internal.h"
#include "ui_voice_surfaces.h"

#define CR_PEER_A 1002u
#define CR_PEER_B 1003u
#define CR_PEER_C 1004u
#define CR_GUILD 2001u
#define CR_VOICE 3121u
#define CR_WAIT_FRAMES 200

typedef struct cr {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
} cr;

static int cr_finish(cr *s)
{
  if (s->fail[0]) {
    printf("call-ring: FAIL %s\n", s->fail);
    fflush(stdout);
    return 1;
  }
  printf("call-ring: PASS checks=%d\n", s->checks);
  fflush(stdout);
  return 0;
}

// Records a setup failure; Discord-style messages are cut to fit rather than overflowing the fail line.
static void cr_setup_fail(cr *s, const char *what, const char *msg)
{
  if (s->fail[0]) return;
  size_t n = (size_t)snprintf(s->fail, sizeof s->fail, "%s: ", what);
  size_t room = sizeof s->fail - n - 1u;
  size_t len = strlen(msg);
  memcpy(s->fail + n, msg, len < room ? len : room);
  s->fail[n + (len < room ? len : room)] = '\0';
}

static bool cr_check(cr *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

static void cr_frames(cr *s, int n) { s->c->frames(s->c->ud, n); }

static void cr_click(cr *s, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  ccord_input_event mv = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  s->c->input(s->c->ud, &mv);
  ccord_input_event dn = {.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  s->c->input(s->c->ud, &dn);
  ccord_input_event up = {.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y};
  s->c->input(s->c->ud, &up);
  cr_frames(s, 3);
}

static ui_voice_surfaces_debug cr_dbg(const cr *s)
{
  ui_voice_surfaces_debug d;
  ui_voice_surfaces_get_debug(s->c->ui, &d);
  return d;
}

static size_t cr_rings(const cr *s) { return ccord_mock_call_count(s->c->transport, CCORD_MOCK_CALL_RING); }
static size_t cr_joins(const cr *s) { return ccord_mock_call_count(s->c->transport, CCORD_MOCK_CALL_VOICE_JOIN); }

static bool cr_select(cr *s, ccord_id channel)
{
  ccord_error err;
  if (ccord_session_select_channel(s->c->session, channel, &err) != CCORD_OK) return false;
  cr_frames(s, 4);
  return ccord_store_selected_channel(s->c->store) == channel;
}

// Clicks the header call button (Start Call / Join Call) and waits until the call bar shows.
static bool cr_start_by_click(cr *s)
{
  ui_voice_surfaces_debug d = cr_dbg(s);
  if (!d.header_call_visible || d.header_call.w <= 0.0f) return false;
  cr_click(s, d.header_call);
  for (int i = 0; i < CR_WAIT_FRAMES && !ui_call_bar_active(s->c->ui); i++) cr_frames(s, 1);
  cr_frames(s, 5);
  return ui_call_bar_active(s->c->ui);
}

static bool cr_leave_by_click(cr *s)
{
  ui_voice_surfaces_debug d = cr_dbg(s);
  if (!d.call_bar_visible || d.call_bar_leave.w <= 0.0f) return false;
  cr_click(s, d.call_bar_leave);
  for (int i = 0; i < CR_WAIT_FRAMES && ui_call_bar_active(s->c->ui); i++) cr_frames(s, 1);
  cr_frames(s, 5);
  return !ui_call_bar_active(s->c->ui);
}

int ccord_verify_call_ring_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  cr s = {.c = ctx};
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->frames || !ctx->input || !ctx->transport) {
    cr_setup_fail(&s, "setup", "incomplete context");
    return cr_finish(&s);
  }
  ccord_error err = {0};
  ccord_ui *ui = ctx->ui;
  cr_frames(&s, 4);
  if (ccord_session_select_guild(ctx->session, CCORD_DM_GUILD_ID, &err) != CCORD_OK) {
    cr_setup_fail(&s, "setup", err.message);
    return cr_finish(&s);
  }
  cr_frames(&s, 2);

  // --- 1: a call started in a DM with no call running joins once and rings once --------------------------------------
  if (ccord_transport_open_dm(ctx->transport, CR_PEER_A, &err) != CCORD_OK) {
    cr_setup_fail(&s, "setup: open dm", err.message);
    return cr_finish(&s);
  }
  cr_frames(&s, 4);
  ccord_id dm_a = 900000u + CR_PEER_A;
  if (!cr_check(&s, cr_select(&s, dm_a), "the DM opens")) return cr_finish(&s);
  size_t rings0 = cr_rings(&s), joins0 = cr_joins(&s);
  if (!cr_check(&s, cr_start_by_click(&s), "Start Call joins the DM call")) return cr_finish(&s);
  if (!cr_check(&s, cr_joins(&s) == joins0 + 1u, "starting the call sends exactly one voice-state join")) return cr_finish(&s);
  if (!cr_check(&s, cr_rings(&s) == rings0 + 1u, "starting the call sends exactly one ring request")) return cr_finish(&s);
  if (!cr_check(&s, ccord_mock_last_ring_channel(ctx->transport) == dm_a, "the ring request is for the DM channel")) return cr_finish(&s);
  if (!cr_check(&s, cr_leave_by_click(&s), "Leave ends the call")) return cr_finish(&s);

  // --- 2: joining a call that is already running rings nobody --------------------------------------------------------
  ccord_id dm_c = CCORD_ID_NONE;
  if (ccord_mock_push_incoming_call(ctx->transport, CR_PEER_C, &dm_c, &err) != CCORD_OK) {
    cr_setup_fail(&s, "setup: incoming call", err.message);
    return cr_finish(&s);
  }
  cr_frames(&s, 6);
  if (!cr_check(&s, cr_select(&s, dm_c), "the DM with the running call opens")) return cr_finish(&s);
  rings0 = cr_rings(&s);
  joins0 = cr_joins(&s);
  if (!cr_check(&s, cr_start_by_click(&s), "Join Call joins the running call")) return cr_finish(&s);
  if (!cr_check(&s, cr_joins(&s) == joins0 + 1u, "joining a running call sends one voice-state join")) return cr_finish(&s);
  if (!cr_check(&s, cr_rings(&s) == rings0, "joining a call already in progress sends no ring request")) return cr_finish(&s);
  if (!cr_check(&s, cr_leave_by_click(&s), "Leave ends the joined call")) return cr_finish(&s);

  // --- 3: joining a guild voice channel rings nobody ------------------------------------------------------------------
  rings0 = cr_rings(&s);
  joins0 = cr_joins(&s);
  if (ccord_session_join_voice(ctx->session, CR_GUILD, CR_VOICE, &err) != CCORD_OK) {
    cr_setup_fail(&s, "setup: guild voice join", err.message);
    return cr_finish(&s);
  }
  cr_frames(&s, 6);
  if (!cr_check(&s, cr_joins(&s) == joins0 + 1u, "a guild voice join sends one voice-state join")) return cr_finish(&s);
  if (!cr_check(&s, cr_rings(&s) == rings0, "joining a guild voice channel sends no ring request")) return cr_finish(&s);
  (void)ccord_session_leave_voice(ctx->session, CR_GUILD, &err);
  cr_frames(&s, 6);

  // --- 4: a ring Discord refuses shows its reason, and the user stays in the call -------------------------------------
  static const char reason[] = "You cannot ring this user (Missing Access)";
  if (ccord_session_select_guild(ctx->session, CCORD_DM_GUILD_ID, &err) != CCORD_OK) {
    cr_setup_fail(&s, "setup", err.message);
    return cr_finish(&s);
  }
  cr_frames(&s, 2);
  if (ccord_transport_open_dm(ctx->transport, CR_PEER_B, &err) != CCORD_OK) {
    cr_setup_fail(&s, "setup: open dm", err.message);
    return cr_finish(&s);
  }
  cr_frames(&s, 4);
  ccord_id dm_b = 900000u + CR_PEER_B;
  if (!cr_check(&s, cr_select(&s, dm_b), "the second DM opens")) return cr_finish(&s);
  ccord_mock_set_ring_refusal(ctx->transport, reason);
  ui->notice[0] = '\0';
  rings0 = cr_rings(&s);
  if (!cr_check(&s, cr_start_by_click(&s), "Start Call joins even though the ring will be refused")) return cr_finish(&s);
  for (int i = 0; i < CR_WAIT_FRAMES && strstr(ui->notice, reason) == nullptr; i++) cr_frames(&s, 1);
  ccord_mock_set_ring_refusal(ctx->transport, nullptr);
  if (!cr_check(&s, cr_rings(&s) == rings0 + 1u, "the refused ring was still one request")) return cr_finish(&s);
  if (!cr_check(&s, strstr(ui->notice, reason) != nullptr, "the notice carries Discord's reason")) {
    fprintf(stderr, "call-ring: notice was '%s'\n", ui->notice);
    return cr_finish(&s);
  }
  if (!cr_check(&s, ui_call_bar_active(ui), "the user is still in the call after the refusal")) return cr_finish(&s);
  if (!cr_check(&s, cr_leave_by_click(&s), "Leave still ends the call")) return cr_finish(&s);
  return cr_finish(&s);
}
