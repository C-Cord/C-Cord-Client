// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Earcon verification; see verify_ui_parity.h. Drives the real app loop (session pump -> earcons pump) over the
// mock transport: each sound is triggered by a real mock gateway event or session call, and the app's own
// ccord_earcons instance reports what it enqueued. Runs with no audio device (SDL dummy driver or none).
#define _POSIX_C_SOURCE 200809L   // open_memstream
#include "verify_ui_parity.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/ini.h>
#include <ccord/net/admin.h>
#include <ccord/net/mock_transport.h>

#include "earcons.h"
#include "settings_m6_prefs.h"
#include "ui_internal.h"

#define EV_GUILD 2001u
#define EV_VOICE 3121u   // "General" voice channel; Ada (1002) is seeded in it
#define EV_STAGE 3123u   // stage channel; users 1003 and 1004 are seeded in it
#define EV_ADA 1002u
#define EV_PEER 1002u
#define EV_FRAME_MS 100  // virtual frame length of the headless loop
#define EV_NOW_MS 1793536200000LL   // mock base time (2026-11-01T12:00Z) + 30 min, as the headless clock

typedef struct ev {
  const ccord_verify_ui_parity_ctx *c;
  ccord_earcons *e;
  ccord_ui *ui;
  ccord_id self;
  ccord_id voice2;         // a second voice channel of the guild (a move target)
  bool played[CCORD_EARCON_COUNT];   // the kind was heard once for its event
  bool muted[CCORD_EARCON_COUNT];    // the kind's own toggle off kept it silent while the event happened
  ccord_id text_channel;   // a guild text channel other than the selected one (mentions notify there)
  ccord_id dm_channel;     // a DM other than the selected channel (every message notifies there)
  int passed;
} ev;

CCORD_PRINTF(2, 3) static int ev_fail(const char *check, const char *fmt, ...)
{
  char msg[192];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  printf("earcons: FAIL %s: %s\n", check, msg);
  fflush(stdout);
  return 1;
}

static void ev_frames(ev *v, int n) { v->c->frames(v->c->ud, n); }

static ccord_earcons_debug ev_dbg(const ev *v)
{
  ccord_earcons_debug d;
  ccord_earcons_get_debug(v->e, &d);
  return d;
}

static const char *kind_name(ccord_earcon_kind k)
{
  return (unsigned)k < CCORD_EARCON_COUNT ? ui_sound_kinds[k].key : "?";
}

// A passed step with sounds on proves the kind plays (toggle on) or stays silent (its toggle off).
static void ev_record(ev *v, bool on, ccord_earcon_kind kind)
{
  if (!on) return;
  if (ui_sound_kind_enabled(v->ui, (unsigned)kind)) v->played[kind] = true;
  else v->muted[kind] = true;
}

// The sound a step expects: its kind when the run plays sounds and the kind's own toggle is on, otherwise none.
static int ev_expect(const ev *v, bool on, ccord_earcon_kind kind)
{
  return on && ui_sound_kind_enabled(v->ui, (unsigned)kind) ? (int)kind : -1;
}

// ---- event helpers (each one is a real mock gateway event or session call) -------------------------

static int ev_post(ev *v, bool mention)
{
  ccord_message_rich rich;
  memset(&rich, 0, sizeof rich);
  ccord_id mentioned[1] = {v->self};
  if (mention) {
    rich.mention_users = mentioned;
    rich.mention_user_count = 1;
  }
  ccord_error err;
  if (ccord_mock_post_rich_message(v->c->transport, mention ? v->text_channel : v->dm_channel, EV_ADA, mention ? "ping" : "hello", &rich, nullptr,
                                   &err) != CCORD_OK) {
    return 1;
  }
  return 0;
}

static int ev_move(ev *v, ccord_id user, ccord_id channel)
{
  ccord_admin_request req;
  ccord_error err;
  if (ccord_admin_move_voice_member(&req, EV_GUILD, user, channel, "verify", &err) != CCORD_OK) return 1;
  if (ccord_session_admin(v->c->session, &req, &err) == CCORD_OK) return 0;
  fprintf(stderr, "earcons: move failed: %s\n", err.message);
  return 1;
}

// Runs `act`, settles, then checks the earcon counters: expect_kind >= 0 means exactly that kind once (non-silent,
// bounded, quiet PCM), expect_kind < 0 means nothing enqueued at all.
typedef int (*ev_act)(ev *v, int arg);
static int ev_step_n(ev *v, const char *name, ev_act act, int arg, int expect_kind, int frames)
{
  ccord_earcons_debug before = ev_dbg(v);
  if (act(v, arg) != 0) return ev_fail(name, "the event itself failed");
  ev_frames(v, frames);
  ccord_earcons_debug after = ev_dbg(v);
  uint32_t delta = after.total - before.total;
  if (expect_kind < 0) {
    if (delta != 0) return ev_fail(name, "%u sound(s) enqueued, expected none", (unsigned)delta);
    v->passed++;
    return 0;
  }
  if (delta != 1) return ev_fail(name, "%u sound(s) enqueued, expected exactly 1", (unsigned)delta);
  if (after.enqueued[expect_kind] != before.enqueued[expect_kind] + 1u) {
    return ev_fail(name, "the enqueued sound was %s, expected %s", kind_name(after.last), kind_name((ccord_earcon_kind)expect_kind));
  }
  if (after.last_peak < 1000 || after.last_peak > 16000)
    return ev_fail(name, "peak %d is silent or too loud", (int)after.last_peak);
  if (after.last_frames == 0 || after.last_frames >= 48000) return ev_fail(name, "length %zu frames", after.last_frames);
  v->passed++;
  return 0;
}

static int ev_step(ev *v, const char *name, ev_act act, int arg, int expect_kind)
{
  return ev_step_n(v, name, act, arg, expect_kind, 4);
}

static int act_message(ev *v, int arg) { (void)arg; return ev_post(v, false); }
static int act_mention(ev *v, int arg) { (void)arg; return ev_post(v, true); }
static int act_selected_message(ev *v, int arg)
{
  (void)arg;
  ccord_id sel = ccord_store_selected_channel(v->c->store);
  ccord_error err;
  return ccord_mock_post_rich_message(v->c->transport, sel, EV_ADA, "selected channel msg", nullptr, nullptr, &err) ==
                 CCORD_OK
             ? 0
             : 1;
}
static int act_self_join(ev *v, int arg)
{
  (void)arg;
  ccord_error err;
  return ccord_session_join_voice(v->c->session, EV_GUILD, EV_VOICE, &err) == CCORD_OK ? 0 : 1;
}
static int act_self_leave(ev *v, int arg)
{
  (void)arg;
  ccord_error err;
  return ccord_session_leave_voice(v->c->session, EV_GUILD, &err) == CCORD_OK ? 0 : 1;
}
static int act_other_join(ev *v, int arg) { return ev_move(v, (ccord_id)arg, EV_VOICE); }
// Leaving my channel by being moved to another voice channel keeps the member connected, so a run can repeat it.
static int act_other_leave(ev *v, int arg) { return ev_move(v, (ccord_id)arg, v->voice2); }
static int act_mute(ev *v, int arg)
{
  ccord_error err;
  return ccord_session_set_voice_mute(v->c->session, arg != 0, &err) == CCORD_OK ? 0 : 1;
}
static int act_deafen(ev *v, int arg)
{
  ccord_error err;
  return ccord_session_set_voice_deafen(v->c->session, arg != 0, &err) == CCORD_OK ? 0 : 1;
}
static int act_stream_start(ev *v, int arg)
{
  (void)arg;
  ccord_stream_config cfg = {0};
  ccord_error err;
  return ccord_session_start_stream(v->c->session, EV_GUILD, EV_VOICE, &cfg, &err) == CCORD_OK ? 0 : 1;
}
static int act_stream_stop(ev *v, int arg)
{
  (void)arg;
  ccord_error err;
  return ccord_session_stop_stream(v->c->session, EV_GUILD, &err) == CCORD_OK ? 0 : 1;
}
// The server reports a new viewer count of my broadcast (the mock has no helper for it; this is the store write the
// session performs for a STREAM_UPDATE).
static int act_viewers(ev *v, int arg)
{
  ccord_error err;
  return ccord_store_stream_set_viewers(v->c->store, EV_VOICE, v->self, (uint32_t)arg, &err) == CCORD_OK ? 0 : 1;
}
static int act_ptt(ev *v, int arg)
{
  ccord_error err;
  return ccord_session_set_ptt_held(v->c->session, arg != 0, &err) == CCORD_OK ? 0 : 1;
}
static int act_ptt_mode(ev *v, int arg)
{
  v->ui->settings_voice_mode = arg;
  ccord_error err;
  return ccord_session_set_ptt_held(v->c->session, arg == 0, &err) == CCORD_OK ? 0 : 1;   // voice activity = gate open
}
static int act_moved(ev *v, int arg)
{
  v->ui->stage_channel = CCORD_ID_NONE;   // the UI did not ask for this
  return ev_move(v, v->self, (ccord_id)arg);
}
// Joins like the UI does: it opens the channel's stage view before asking the session.
static int act_own_join(ev *v, int arg)
{
  v->ui->stage_channel = (ccord_id)arg;
  ccord_error err;
  return ccord_session_join_voice(v->c->session, EV_GUILD, (ccord_id)arg, &err) == CCORD_OK ? 0 : 1;
}
static int act_invite(ev *v, int arg)
{
  (void)arg;
  ccord_admin_request req;
  ccord_error err;
  if (ccord_admin_stage_set_speaker(&req, EV_GUILD, EV_STAGE, v->self, true, "verify", &err) != CCORD_OK) return 1;
  return ccord_session_admin(v->c->session, &req, &err) == CCORD_OK ? 0 : 1;
}
static int act_watch(ev *v, int arg)
{
  ccord_error err;
  if (arg) return ccord_session_watch_stream(v->c->session, EV_GUILD, EV_STAGE, EV_ADA, &err) == CCORD_OK ? 0 : 1;
  return ccord_session_stop_watching(v->c->session, &err) == CCORD_OK ? 0 : 1;
}

// The whole one-shot event sequence. `on` selects the expectation: the matching earcon exactly once, or nothing.
// `other` is a seeded stage user moved into my channel and disconnected again (0 = skip those two events).
static int ev_sequence(ev *v, bool on, ccord_id other)
{
  const char *tag = on ? "on" : "off";
  char name[64];
#define STEP(label, act, arg, kind)                                                    \
  do {                                                                                 \
    (void)snprintf(name, sizeof name, "%s (%s)", label, tag);                          \
    if (ev_step(v, name, act, arg, ev_expect(v, on, kind)) != 0) return 1;             \
    ev_record(v, on, kind);                                                            \
  } while (0)
#define SILENT(label, act, arg)                                                        \
  do {                                                                                 \
    (void)snprintf(name, sizeof name, "%s (%s)", label, tag);                          \
    if (ev_step(v, name, act, arg, -1) != 0) return 1;                                 \
  } while (0)
#define STEP1(label, act, arg, kind)                                                   \
  do {                                                                                 \
    (void)snprintf(name, sizeof name, "%s (%s)", label, tag);                          \
    if (ev_step_n(v, name, act, arg, ev_expect(v, on, kind), 1) != 0) return 1;        \
    ev_record(v, on, kind);                                                            \
  } while (0)
  STEP("message", act_message, 0, CCORD_EARCON_MESSAGE);
  STEP("mention", act_mention, 0, CCORD_EARCON_MENTION);
  STEP("self connect", act_self_join, 0, CCORD_EARCON_SELF_CONNECT);
  if (other != 0) {
    STEP("voice user join", act_other_join, (int)other, CCORD_EARCON_VOICE_JOIN);
    STEP("voice user leave", act_other_leave, (int)other, CCORD_EARCON_VOICE_LEAVE);
  }
  STEP("self mute", act_mute, 1, CCORD_EARCON_MUTE);
  STEP("self unmute", act_mute, 0, CCORD_EARCON_UNMUTE);
  STEP("self deafen", act_deafen, 1, CCORD_EARCON_DEAFEN);
  STEP("self undeafen", act_deafen, 0, CCORD_EARCON_UNDEAFEN);
  STEP("self unmute (deafening muted me)", act_mute, 0, CCORD_EARCON_UNMUTE);   // the mock keeps my mute across connections
  // Go Live: start, the viewer count of the broadcast going up and down, stop.
  STEP("stream start", act_stream_start, 0, CCORD_EARCON_STREAM_START);
  STEP("viewer join", act_viewers, 3, CCORD_EARCON_VIEWER_JOIN);
  STEP("viewer leave", act_viewers, 2, CCORD_EARCON_VIEWER_LEAVE);
  STEP("stream stop", act_stream_stop, 0, CCORD_EARCON_STREAM_STOP);
  // Watching someone else's stream (a broadcast the mock does not need to carry: the session's watch state is it).
  STEP("watch stream start", act_watch, 1, CCORD_EARCON_STREAM_START);
  STEP("watch stream stop", act_watch, 0, CCORD_EARCON_STREAM_STOP);
  // Push-to-talk (only in that input mode): the key state arrives through the session once per frame. The mic gate
  // of the media pump falls back to the real key one pump later, so each step is a single frame.
  SILENT("ptt mode is silent", act_ptt_mode, 1);
  ev_frames(v, 4);
  STEP1("ptt on", act_ptt, 1, CCORD_EARCON_PTT_ON);
  STEP1("ptt off", act_ptt, 0, CCORD_EARCON_PTT_OFF);
  SILENT("voice activity mode is silent", act_ptt_mode, 0);
  SILENT("ptt key in voice activity mode is silent", act_ptt, 1);
  SILENT("ptt key released in voice activity mode is silent", act_ptt, 0);
  // Moved by a moderator (the UI did not ask for it) versus a switch the user made.
  if (v->voice2 != CCORD_ID_NONE) {
    STEP("moved", act_moved, (int)v->voice2, CCORD_EARCON_USER_MOVED);
    STEP("own channel switch", act_own_join, (int)EV_VOICE, CCORD_EARCON_SELF_CONNECT);
  }
  // Stage: an audience member invited to speak.
  STEP("stage connect", act_own_join, (int)EV_STAGE, CCORD_EARCON_SELF_CONNECT);
  STEP("invited to speak", act_invite, 0, CCORD_EARCON_INVITED_TO_SPEAK);
  STEP("self disconnect", act_self_leave, 0, CCORD_EARCON_SELF_DISCONNECT);
#undef STEP
#undef STEP1
#undef SILENT
  return 0;
}

// ---- incoming call ring ---------------------------------------------------------------------------

static int ev_ring_cycle(ev *v, bool on, bool answer)
{
  const char *tag = on ? (answer ? "ring answered" : "ring ended") : "ring off";
  ccord_error err;
  ccord_id ch = CCORD_ID_NONE;
  ccord_earcons_debug d0 = ev_dbg(v);
  if (ccord_mock_push_incoming_call(v->c->transport, EV_PEER, &ch, &err) != CCORD_OK) return ev_fail(tag, "no call");
  ev_frames(v, 4);
  ccord_earcons_debug d1 = ev_dbg(v);
  if (!on) {
    ev_frames(v, 2 * CCORD_EARCON_RING_PERIOD_MS / EV_FRAME_MS + 4);
    if (ev_dbg(v).total != d0.total) return ev_fail(tag, "the ring played with sounds off");
    if (ccord_mock_end_call(v->c->transport, ch, &err) != CCORD_OK) return ev_fail(tag, "end failed");
    ev_frames(v, 4);
    v->passed++;
    return 0;
  }
  if (!d1.ringing || d1.enqueued[CCORD_EARCON_CALL_RING] != d0.enqueued[CCORD_EARCON_CALL_RING] + 1u)
    return ev_fail(tag, "the ring did not start with exactly one burst");
  if (d1.last_peak < 1000 || d1.last_peak > 16000 || d1.last_frames == 0 || d1.last_frames >= 48000)
    return ev_fail(tag, "ring burst is silent, loud or too long");
  v->passed++;
  ev_frames(v, 2 * CCORD_EARCON_RING_PERIOD_MS / EV_FRAME_MS + 4);   // a bit over two periods
  ccord_earcons_debug d2 = ev_dbg(v);
  uint32_t bursts = d2.enqueued[CCORD_EARCON_CALL_RING] - d0.enqueued[CCORD_EARCON_CALL_RING];
  if (bursts < 3u || bursts > 4u) return ev_fail(tag, "ring repeated %u times in two periods, expected 3", (unsigned)bursts);
  if (d2.total != d0.total + bursts) return ev_fail(tag, "something other than the ring played");
  v->passed++;
  if (answer) {
    if (ccord_session_call_join(v->c->session, ch, &err) != CCORD_OK) return ev_fail(tag, "join failed");
  } else if (ccord_mock_end_call(v->c->transport, ch, &err) != CCORD_OK) {
    return ev_fail(tag, "end failed");
  }
  ev_frames(v, 4);
  ccord_earcons_debug d3 = ev_dbg(v);
  ev_frames(v, 3 * CCORD_EARCON_RING_PERIOD_MS / EV_FRAME_MS);
  ccord_earcons_debug d4 = ev_dbg(v);
  if (d3.ringing || d4.ringing) return ev_fail(tag, "still ringing");
  if (d4.enqueued[CCORD_EARCON_CALL_RING] != d3.enqueued[CCORD_EARCON_CALL_RING])
    return ev_fail(tag, "the ring kept repeating after it should have stopped");
  v->passed++;
  if (answer) {   // leave and hang up so the next section starts clean
    (void)ccord_session_call_leave(v->c->session, ch, &err);
    ev_frames(v, 4);
    (void)ccord_mock_end_call(v->c->transport, ch, &err);
    ev_frames(v, 4);
  }
  return 0;
}

// ---- outgoing call ring -----------------------------------------------------------------------------

// I start a call in the DM with Ada: the call rings her, one self-connect sound plays and the ringback repeats until
// the call stops ringing. mode 0: I cancel (hang up); 1: the call is ended remotely; 2: she answers (the ringing
// list empties while I stay connected). `on` false: the sounds (or this kind's toggle) are off.
static int ev_out_ring_cycle(ev *v, bool on, int mode)
{
  static const char *const modes[3] = {"cancelled", "ended", "answered"};
  char tag[48];
  (void)snprintf(tag, sizeof tag, "outgoing ring %s%s", modes[mode], on ? "" : " (off)");
  ccord_error err;
  bool ring_on = on && ui_sound_kind_enabled(v->ui, CCORD_EARCON_OUTGOING_RING);
  uint32_t connect = on && ui_sound_kind_enabled(v->ui, CCORD_EARCON_SELF_CONNECT) ? 1u : 0u;
  ccord_earcons_debug d0 = ev_dbg(v);
  if (ccord_session_call_join(v->c->session, v->dm_channel, &err) != CCORD_OK) return ev_fail(tag, "call_join failed: %s", err.message);
  ev_frames(v, 4);
  ccord_earcons_debug d1 = ev_dbg(v);
  if (!d1.outgoing_ringing) return ev_fail(tag, "my call is not ringing");
  if (d1.ringing) return ev_fail(tag, "the incoming ring started for my own call");
  if (d1.enqueued[CCORD_EARCON_SELF_CONNECT] != d0.enqueued[CCORD_EARCON_SELF_CONNECT] + connect)
    return ev_fail(tag, "starting the call did not make exactly the expected connect sound");
  if (d1.enqueued[CCORD_EARCON_OUTGOING_RING] != d0.enqueued[CCORD_EARCON_OUTGOING_RING] + (ring_on ? 1u : 0u))
    return ev_fail(tag, "the ringback did not start with exactly one burst");
  if (ring_on && (d1.last_peak < 1000 || d1.last_peak > 9830 || d1.last_frames == 0 || d1.last_frames >= 48000))
    return ev_fail(tag, "ringback burst is silent, loud or too long");
  v->passed++;
  ev_frames(v, 2 * CCORD_EARCON_OUTGOING_RING_PERIOD_MS / EV_FRAME_MS + 4);
  ccord_earcons_debug d2 = ev_dbg(v);
  uint32_t bursts = d2.enqueued[CCORD_EARCON_OUTGOING_RING] - d0.enqueued[CCORD_EARCON_OUTGOING_RING];
  if (ring_on ? (bursts < 3u || bursts > 4u) : bursts != 0u)
    return ev_fail(tag, "ringback repeated %u times in two periods", (unsigned)bursts);
  if (d2.total != d0.total + connect + bursts) return ev_fail(tag, "something other than the ringback played");
  v->passed++;
  if (mode == 0) {
    if (ccord_session_call_leave(v->c->session, v->dm_channel, &err) != CCORD_OK) return ev_fail(tag, "cancel failed");
  } else if (mode == 1) {
    if (ccord_mock_end_call(v->c->transport, v->dm_channel, &err) != CCORD_OK) return ev_fail(tag, "end failed");
  } else {
    const ccord_call_state *row = ccord_store_call(v->c->store, v->dm_channel);
    if (!row) return ev_fail(tag, "no call row");
    ccord_call_state answered = *row;   // what the session stores for the CALL_UPDATE of an answer
    answered.ringing_count = 0;
    if (ccord_store_set_call(v->c->store, &answered, &err) != CCORD_OK) return ev_fail(tag, "answer failed");
  }
  ev_frames(v, 4);
  ccord_earcons_debug d3 = ev_dbg(v);
  ev_frames(v, 3 * CCORD_EARCON_OUTGOING_RING_PERIOD_MS / EV_FRAME_MS);
  ccord_earcons_debug d4 = ev_dbg(v);
  if (d3.outgoing_ringing || d4.outgoing_ringing) return ev_fail(tag, "still ringing");
  if (d4.enqueued[CCORD_EARCON_OUTGOING_RING] != d3.enqueued[CCORD_EARCON_OUTGOING_RING])
    return ev_fail(tag, "the ringback kept repeating after it should have stopped");
  v->passed++;
  ev_record(v, on, CCORD_EARCON_OUTGOING_RING);
  (void)ccord_session_call_leave(v->c->session, v->dm_channel, &err);   // hang up so the next section starts clean
  ev_frames(v, 4);
  (void)ccord_mock_end_call(v->c->transport, v->dm_channel, &err);
  ev_frames(v, 4);
  return 0;
}

// ---- per-sound toggles: persistence and the Settings page ------------------------------------------------

static bool ev_ini_visit(const char *section, const char *key, const char *value, int line, void *ud, ccord_error *err)
{
  (void)line;
  (void)err;
  if (strcmp(section, "ui") == 0) (void)ui_settings_m6_prefs_ini_visit((ccord_ui *)ud, key, value);
  return true;
}

static int ev_ini_load(ccord_ui *ui, const char *text)
{
  ccord_error err;
  return ccord_ini_parse(text, strlen(text), ev_ini_visit, ui, &err) == CCORD_OK ? 0 : 1;
}

// The settings.ini lines the Settings code writes and reads (settings.c delegates every `sound_*` key to these
// two functions); a memory stream keeps the user's real settings.ini out of it.
static int ev_check_persistence(ev *v)
{
  ccord_ui *ui = v->ui;
  const uint32_t want = 0x5A5A5u;
  ui->settings_sounds_off = want;
  char *text = nullptr;
  size_t len = 0;
  FILE *f = open_memstream(&text, &len);
  if (!f) return ev_fail("persistence", "no memory stream");
  fputs("[ui]\n", f);
  ui_settings_m6_prefs_ini_save(ui, f);
  fclose(f);
  if (!text) return ev_fail("persistence", "nothing saved");
  int lines = 0;
  for (const char *p = text; (p = strstr(p, "\nsound_")) != nullptr; p++) lines++;
  bool ok = lines == UI_SOUND_KIND_COUNT;
  for (unsigned k = 0; k < UI_SOUND_KIND_COUNT && ok; k++) {
    char want_line[64];
    (void)snprintf(want_line, sizeof want_line, "\n%s = %d\n", ui_sound_kinds[k].ini, (want >> k & 1u) ? 0 : 1);
    ok = strstr(text, want_line) != nullptr;
  }
  if (!ok) {
    free(text);
    return ev_fail("persistence", "settings.ini does not carry one correct line per sound");
  }
  v->passed++;
  ui->settings_sounds_off = 0xFFFFFu;   // anything else: loading must restore exactly what was saved
  int bad = ev_ini_load(ui, text);
  free(text);
  if (bad || ui->settings_sounds_off != want) return ev_fail("persistence", "reload gave %x, saved %x", (unsigned)ui->settings_sounds_off, (unsigned)want);
  v->passed++;
  // A file from before the toggles (no sound_* keys) keeps every sound on; a garbled value changes nothing.
  ui->settings_sounds_off = 0;
  if (ev_ini_load(ui, "[ui]\nnotify_sounds = 1\n") != 0 || ui->settings_sounds_off != 0u)
    return ev_fail("persistence", "an old settings.ini muted a sound");
  if (ev_ini_load(ui, "[ui]\nsound_ptt_on = banana\n") != 0 || ui->settings_sounds_off != 0u)
    return ev_fail("persistence", "a garbled value changed a toggle");
  if (ev_ini_load(ui, "[ui]\nsound_ptt_on = 0\n") != 0 || ui->settings_sounds_off != 1u << CCORD_EARCON_PTT_ON)
    return ev_fail("persistence", "sound_ptt_on = 0 did not mute push-to-talk on");
  ui->settings_sounds_off = 0;
  v->passed++;
  return 0;
}

static void ev_click(ev *v, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  ccord_input_event e = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  v->c->input(v->c->ud, &e);
  e = (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  v->c->input(v->c->ud, &e);
  e = (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y};
  v->c->input(v->c->ud, &e);
  ev_frames(v, 2);
}

// Settings > Notifications > SOUNDS > Customize: real clicks flip one toggle at a time and Back returns to the rows.
static int ev_check_settings_page(ev *v)
{
  ccord_ui *ui = v->ui;
  ui->settings_notify_sounds = true;
  ui->settings_sounds_off = 0;
  ui_open_settings(ui, true);
  ev_frames(v, 2);
  ccord_rect tabs[CCORD_M6_PREFS_COUNT];
  ui_settings_m6_prefs_host_rects(ui, tabs);
  ev_click(v, tabs[CCORD_M6_PREFS_NOTIFICATIONS]);
  ccord_settings_m6_prefs_debug pd;
  ui_settings_m6_prefs_get_debug(ui, &pd);
  if (pd.drawn_page != CCORD_M6_PREFS_NOTIFICATIONS) return ev_fail("settings page", "Notifications did not open");
  ccord_rect cz = ui_settings_sounds_customize_rect(ui);
  if (cz.w <= 0.0f) return ev_fail("settings page", "no Customize button on the SOUNDS row");
  ev_click(v, cz);
  if (!ui->settings_sounds_open) return ev_fail("settings page", "Customize did not open the per-sound toggles");
  v->passed++;
  for (int k = 0; k < UI_SOUND_KIND_COUNT; k++) {
    ccord_rect t = ui_settings_sound_toggle_rect(ui, k);
    if (t.w <= 0.0f || t.y + t.h > pd.content.y + pd.content.h)
      return ev_fail("settings page", "toggle %s is not drawn", ui_sound_kinds[k].key);
  }
  for (int k = 0; k < UI_SOUND_KIND_COUNT; k++) {   // every toggle flips its own bit and nothing else
    ev_click(v, ui_settings_sound_toggle_rect(ui, k));
    if (ui->settings_sounds_off != 1u << k) return ev_fail("settings page", "clicking %s gave mask %x", ui_sound_kinds[k].key, (unsigned)ui->settings_sounds_off);
    ev_click(v, ui_settings_sound_toggle_rect(ui, k));
    if (ui->settings_sounds_off != 0u) return ev_fail("settings page", "clicking %s again did not restore it", ui_sound_kinds[k].key);
  }
  v->passed++;
  ui->settings_notify_sounds = false;   // the master switch off: the grid is dimmed and does not change
  ev_frames(v, 2);
  ev_click(v, ui_settings_sound_toggle_rect(ui, CCORD_EARCON_PTT_ON));
  if (ui->settings_sounds_off != 0u) return ev_fail("settings page", "a toggle changed while Sounds is off");
  ui->settings_notify_sounds = true;
  ev_frames(v, 2);
  v->passed++;
  ev_click(v, ui_settings_sounds_back_rect(ui));
  if (ui->settings_sounds_open) return ev_fail("settings page", "Back did not return to the Notifications rows");
  ui_open_settings(ui, false);
  ev_frames(v, 2);
  v->passed++;
  return 0;
}

// ---- delivered desktop notifications ---------------------------------------------------------------

// Delivers a notification through the real ccord_app_desktop_deliver (the headless loop runs under a virtual clock,
// so the app pump does not). expect_kind >= 0: the desktop must accept it and exactly that earcon, one sound, one
// sound request and no legacy 880 Hz tone must result; expect_kind < 0: no sound request and no sound at all.
// `must_deliver` says whether the OS notification itself is expected to be accepted.
static int ev_deliver(ev *v, const char *name, bool mention, bool must_deliver, int expect_kind)
{
  ccord_app_desktop *d = ccord_earcons_desktop(v->e);
  if (!d) return ev_fail(name, "the earcons instance has no desktop glue");
  ccord_notification n = {.guild_id = mention ? EV_GUILD : CCORD_DM_GUILD_ID,
                          .channel_id = mention ? v->text_channel : v->dm_channel,
                          .message_id = 1, .author_id = EV_ADA, .is_dm = !mention, .is_mention = mention};
  ccord_platform_set_window_focused(ccord_app_desktop_platform(d), false);   // a focused window is never notified
  ccord_earcons_debug b = ev_dbg(v);
  ccord_app_desktop_debug db, da;
  ccord_app_desktop_get_debug(d, &db);
  bool delivered = ccord_app_desktop_deliver(d, &n, EV_NOW_MS);
  ccord_earcons_debug a = ev_dbg(v);
  ccord_app_desktop_get_debug(d, &da);
  if (delivered != must_deliver) return ev_fail(name, "delivered=%d, expected %d", (int)delivered, (int)must_deliver);
  if (da.tone_requests != db.tone_requests) return ev_fail(name, "the legacy 880 Hz tone was requested");
  if (expect_kind < 0) {
    if (a.total != b.total) return ev_fail(name, "%u sound(s) enqueued, expected none", (unsigned)(a.total - b.total));
    if (da.beep_requests != db.beep_requests) return ev_fail(name, "a sound was requested");
    v->passed++;
    return 0;
  }
  if (da.beep_requests != db.beep_requests + 1u) return ev_fail(name, "the delivery requested no sound");
  if (a.total != b.total + 1u) return ev_fail(name, "%u sound(s) enqueued, expected exactly 1", (unsigned)(a.total - b.total));
  if (a.enqueued[expect_kind] != b.enqueued[expect_kind] + 1u)
    return ev_fail(name, "the enqueued sound was %s, expected %s", kind_name(a.last), kind_name((ccord_earcon_kind)expect_kind));
  if (a.last_peak < 1000 || a.last_peak > 16000) return ev_fail(name, "peak %d is silent or too loud", (int)a.last_peak);
  v->passed++;
  return 0;
}

// The "stream notifications" override lifts Streamer Mode's notification suppression so a delivery still happens
// while Disable Sounds is on.
static int ev_set_stream_override(ev *v, bool on)
{
  ccord_user_settings_patch p = CCORD_USER_SETTINGS_PATCH_INIT;
  p.stream_notifications_enabled = on ? 1 : 0;
  ccord_error err = {0};
  if (ccord_session_user_settings_patch(v->c->session, &p, &err) != CCORD_OK) return 1;
  for (int i = 0; i < 50; i++) {
    ev_frames(v, 1);
    const ccord_user_settings *us = ccord_store_user_settings(v->c->store);
    if (us && us->stream_notifications_enabled == on) return 0;
  }
  return 1;
}

static int ev_check_delivered(ev *v, ccord_ui *ui)
{
  if (ev_deliver(v, "delivered mention plays the mention earcon", true, true, CCORD_EARCON_MENTION) != 0) return 1;
  if (ev_deliver(v, "delivered message plays the message earcon", false, true, CCORD_EARCON_MESSAGE) != 0) return 1;
  ui->settings_notify_sounds = false;
  ev_frames(v, 2);
  if (ev_deliver(v, "delivered mention with sounds off is silent", true, true, -1) != 0) return 1;
  ui->settings_notify_sounds = true;
  ui->settings_streamer_mode = true;
  ev_frames(v, 2);
  if (ev_set_stream_override(v, true) != 0) return ev_fail("streamer delivered", "cannot enable stream notifications");
  if (ev_deliver(v, "delivered mention with streamer Disable Sounds is silent", true, true, -1) != 0) return 1;
  ui->settings_streamer_mode = false;
  if (ev_set_stream_override(v, false) != 0) return ev_fail("streamer delivered", "cannot restore stream notifications");
  ev_frames(v, 2);
  return 0;
}

// ---- synthesis --------------------------------------------------------------------------------------

static int ev_check_synth(ev *v)
{
  for (int k = 0; k < CCORD_EARCON_COUNT; k++) {
    size_t n = 0;
    const int16_t *pcm = ccord_earcon_pcm((ccord_earcon_kind)k, &n);
    if (!pcm || n == 0 || n >= 48000 * 6 / 10) return ev_fail("synthesis", "%s: bad length %zu", kind_name((ccord_earcon_kind)k), n);
    int peak = 0;
    for (size_t i = 0; i < n * 2u; i++) {
      int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
      if (a > peak) peak = a;
    }
    if (peak < 1000 || peak > 16000) return ev_fail("synthesis", "%s: peak %d", kind_name((ccord_earcon_kind)k), peak);
    if (peak > 9830) return ev_fail("synthesis", "%s: peak %d is above 0.3 of full scale", kind_name((ccord_earcon_kind)k), peak);
  }
  v->passed++;
  for (int a = 0; a < CCORD_EARCON_COUNT; a++) {
    for (int b = a + 1; b < CCORD_EARCON_COUNT; b++) {
      size_t na = 0, nb = 0;
      const int16_t *pa = ccord_earcon_pcm((ccord_earcon_kind)a, &na);
      const int16_t *pb = ccord_earcon_pcm((ccord_earcon_kind)b, &nb);
      if (na == nb && memcmp(pa, pb, na * 2u * sizeof *pa) == 0)
        return ev_fail("synthesis", "%s and %s are the same sound", kind_name((ccord_earcon_kind)a), kind_name((ccord_earcon_kind)b));
    }
  }
  v->passed++;
  return 0;
}

int ccord_verify_earcons_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->frames) return ev_fail("setup", "incomplete context");
  ev v = {.c = ctx, .e = ccord_earcons_instance(), .ui = ctx->ui, .self = CCORD_ID_NONE};
  if (!v.e) return ev_fail("setup", "the app created no earcons instance");
  ev_frames(&v, 8);
  v.self = ccord_store_self_id(ctx->store);
  if (v.self == CCORD_ID_NONE) return ev_fail("setup", "no self user");
  ccord_id selected = ccord_store_selected_channel(ctx->store);
  size_t nch = ccord_store_channel_count(ctx->store, EV_GUILD);
  for (size_t i = 0; i < nch && v.text_channel == CCORD_ID_NONE; i++) {
    const ccord_channel *c = ccord_store_channel_at(ctx->store, EV_GUILD, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->id != selected) v.text_channel = c->id;
  }
  if (v.text_channel == CCORD_ID_NONE) return ev_fail("setup", "no unselected text channel");
  for (size_t i = 0; i < nch && v.voice2 == CCORD_ID_NONE; i++) {
    const ccord_channel *c = ccord_store_channel_at(ctx->store, EV_GUILD, i);
    if (c && c->kind == CCORD_CHANNEL_VOICE && c->id != EV_VOICE) v.voice2 = c->id;
  }
  if (v.voice2 == CCORD_ID_NONE) return ev_fail("setup", "no second voice channel to be moved to");
  ccord_ui *ui = ctx->ui;
  ui->settings_notify_sounds = true;
  ui->settings_notify_desktop = true;
  ui->settings_streamer_mode = false;
  ev_frames(&v, 4);
  ccord_earcons_debug d0 = ev_dbg(&v);
  if (d0.total != 0) return ev_fail("setup", "%u sound(s) played before any event", (unsigned)d0.total);
  // A DM from Ada (every DM message notifies). Creating it may itself notify, so the baseline is taken after.
  ccord_error serr;
  if (ccord_mock_push_message_request(ctx->transport, EV_ADA, nullptr, false, &v.dm_channel, &serr) != CCORD_OK)
    return ev_fail("setup", "cannot create a DM channel");
  ev_frames(&v, 6);

  if (ev_check_synth(&v) != 0) return 1;
  // Sounds on: every one-shot kind exactly once (a seeded stage user is moved into my channel and disconnected).
  if (ev_sequence(&v, true, 1003) != 0) return 1;
  if (ev_ring_cycle(&v, true, false) != 0) return 1;   // rings, repeats, stops when the call ends
  ev_record(&v, true, CCORD_EARCON_CALL_RING);
  if (ev_ring_cycle(&v, true, true) != 0) return 1;    // rings, stops when answered
  for (int mode = 0; mode < 3; mode++) {               // my own call: cancelled, ended remotely, answered
    if (ev_out_ring_cycle(&v, true, mode) != 0) return 1;
  }
  // The preference off: the same sequence enqueues nothing.
  ui->settings_notify_sounds = false;
  ev_frames(&v, 2);
  if (ev_sequence(&v, false, 1004) != 0) return 1;
  if (ev_ring_cycle(&v, false, false) != 0) return 1;
  if (ev_out_ring_cycle(&v, false, 0) != 0) return 1;
  ui->settings_notify_sounds = true;
  // One toggle at a time: with alternating kinds muted (and then the other half), a muted kind enqueues nothing while
  // every kind that is still on plays exactly as before. Together with the all-on run above, each kind is proven both ways.
  for (int pass = 0; pass < 2; pass++) {
    ui->settings_sounds_off = pass == 0 ? 0x55555u : 0xAAAAAu;
    ev_frames(&v, 2);
    if (ev_sequence(&v, true, 1003) != 0) return 1;
    if (ev_ring_cycle(&v, ui_sound_kind_enabled(ui, CCORD_EARCON_CALL_RING), false) != 0) return 1;
    ev_record(&v, true, CCORD_EARCON_CALL_RING);
    for (int mode = 0; mode < 3; mode++) {
      if (ev_out_ring_cycle(&v, true, mode) != 0) return 1;
    }
  }
  ui->settings_sounds_off = 0;
  if (ev_check_persistence(&v) != 0) return 1;
  if (ev_check_settings_page(&v) != 0) return 1;
  // Streamer Mode (Disable Sounds defaults on) and Do Not Disturb silence it too.
  ui->settings_streamer_mode = true;
  ev_frames(&v, 2);
  if (ev_step(&v, "streamer mode disables sounds", act_message, 0, -1) != 0) return 1;
  ui->settings_streamer_mode = false;
  ev_frames(&v, 2);
  if (ev_step(&v, "sounds resume after streamer mode", act_message, 0, CCORD_EARCON_MESSAGE) != 0) return 1;
  ccord_error err;
  if (ccord_store_update_presence(ctx->store, v.self, CCORD_PRESENCE_DND, nullptr, &err) != CCORD_OK)
    return ev_fail("dnd", "cannot set Do Not Disturb");
  ev_frames(&v, 2);
  if (ev_step(&v, "do not disturb silences sounds", act_mention, 0, -1) != 0) return 1;
  (void)ccord_store_update_presence(ctx->store, v.self, CCORD_PRESENCE_ONLINE, nullptr, &err);

  if (ev_check_delivered(&v, ui) != 0) return 1;

  // Focused window suppression: quiet while actively reading the selected channel, audible when covered by Settings
  // or scrolled up in history (still focused, not actively reading).
  {
    ccord_app_desktop *d = ccord_earcons_desktop(v.e);
    ccord_platform *plat = d ? ccord_app_desktop_platform(d) : nullptr;
    if (plat) ccord_platform_set_window_focused(plat, true);
    ui->window_focused = true;
    ui->vlist.stick_bottom = true;
    ui_open_settings(ui, false);
    ev_frames(&v, 2);
    if (ev_step(&v, "focused and reading selected channel is silent", act_selected_message, 0, -1) != 0) return 1;
    ui_open_settings(ui, true);
    ev_frames(&v, 2);
    if (ev_step(&v, "focused with settings covering selected channel plays message earcon", act_selected_message, 0,
                CCORD_EARCON_MESSAGE) != 0) {
      ui_open_settings(ui, false);
      return 1;
    }
    ui_open_settings(ui, false);
    const ccord_layout *lay = ccord_ui_get_layout(ui);
    ccord_input_event wheel_up = {.type = CCORD_IN_WHEEL, .wheel = 600.0f,
                                  .x = lay->message_area.x + 100.0f, .y = lay->message_area.y + 100.0f};
    v.c->input(v.c->ud, &wheel_up);
    ev_frames(&v, 2);
    if (ui->vlist.stick_bottom) return ev_fail("scrolled up", "wheel did not detach from bottom");
    if (ev_step(&v, "focused while scrolled up in selected channel plays message earcon", act_selected_message, 0,
                CCORD_EARCON_MESSAGE) != 0) {
      ccord_vlist_reset_bottom(&ui->vlist);
      return 1;
    }
    ccord_vlist_reset_bottom(&ui->vlist);
    ev_frames(&v, 2);
  }

  // One line per kind, printed only when it was heard for its own event and also kept silent by its own toggle.
  for (int k = 0; k < CCORD_EARCON_COUNT; k++) {
    if (!v.played[k] || !v.muted[k])
      return ev_fail("coverage", "%s: played=%d muted-by-toggle=%d", ui_sound_kinds[k].key, (int)v.played[k], (int)v.muted[k]);
    printf("earcon %s: ok\n", ui_sound_kinds[k].key);
  }
  printf("earcons: PASS checks=%d\n", v.passed);
  fflush(stdout);
  return 0;
}
