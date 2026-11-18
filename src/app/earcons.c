// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Earcons; see earcons.h.
#include "earcons.h"

#include <math.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/platform/platform.h>

#include "ui_internal.h"   // the per-sound toggles (ui_sound_kind_enabled) and which voice channel the UI itself opened

_Static_assert(CCORD_EARCON_COUNT == UI_SOUND_KIND_COUNT, "every earcon kind has one Settings toggle");

#define EAR_RATE 48000
#define EAR_PEAK 0.28f              // well below full scale
#define EAR_MAX_PER_PUMP 4u
#define EAR_RETRY_OPEN_MS 10000
#define EAR_MAX_VOICE 64            // tracked members of the connected channel
#define EAR_PI 3.14159265358979f

// ---------------------------------------------------------------- synthesis

typedef struct ear_note {
  float hz_from, hz_to;   // linear glide; 0 hz = rest
  int ms;
} ear_note;

typedef struct ear_def {
  ear_note notes[4];
} ear_def;

static const ear_def k_defs[CCORD_EARCON_COUNT] = {
  [CCORD_EARCON_MESSAGE] = {{{660, 660, 70}, {880, 880, 120}}},
  [CCORD_EARCON_MENTION] = {{{784, 784, 80}, {988, 988, 80}, {1319, 1319, 200}}},
  [CCORD_EARCON_VOICE_JOIN] = {{{440, 440, 70}, {587, 587, 70}, {880, 880, 140}}},
  [CCORD_EARCON_VOICE_LEAVE] = {{{880, 880, 70}, {587, 587, 70}, {440, 440, 140}}},
  [CCORD_EARCON_MUTE] = {{{520, 340, 150}}},
  [CCORD_EARCON_UNMUTE] = {{{340, 520, 150}}},
  [CCORD_EARCON_DEAFEN] = {{{440, 300, 90}, {300, 200, 150}}},
  [CCORD_EARCON_UNDEAFEN] = {{{200, 300, 90}, {300, 440, 150}}},
  [CCORD_EARCON_CALL_RING] = {{{740, 740, 140}, {0, 0, 80}, {740, 740, 140}, {0, 0, 0}}},
  // Own voice connection: a rising C major triad when I connect, the same notes falling when I leave.
  [CCORD_EARCON_SELF_CONNECT] = {{{523, 523, 80}, {659, 659, 80}, {1047, 1047, 200}}},
  [CCORD_EARCON_SELF_DISCONNECT] = {{{1047, 1047, 80}, {659, 659, 80}, {392, 392, 220}}},
  // Moved: an up-glide, a breath, then a higher up-glide ("carried somewhere else").
  [CCORD_EARCON_USER_MOVED] = {{{494, 740, 120}, {0, 0, 30}, {740, 988, 160}}},
  // Ringback while my own call rings: a slow two-tone pulse (lower and longer than the incoming ring).
  [CCORD_EARCON_OUTGOING_RING] = {{{440, 440, 200}, {0, 0, 60}, {480, 480, 200}}},
  [CCORD_EARCON_STREAM_START] = {{{392, 392, 60}, {523, 523, 60}, {659, 659, 60}, {784, 784, 160}}},
  [CCORD_EARCON_STREAM_STOP] = {{{784, 784, 60}, {659, 659, 60}, {523, 523, 60}, {392, 392, 160}}},
  [CCORD_EARCON_VIEWER_JOIN] = {{{698, 698, 60}, {932, 932, 110}}},
  [CCORD_EARCON_VIEWER_LEAVE] = {{{932, 932, 60}, {698, 698, 110}}},
  // Push-to-talk: a tiny rising / falling chirp.
  [CCORD_EARCON_PTT_ON] = {{{1000, 1250, 45}}},
  [CCORD_EARCON_PTT_OFF] = {{{1250, 1000, 45}}},
  [CCORD_EARCON_INVITED_TO_SPEAK] = {{{587, 587, 90}, {740, 740, 90}, {880, 880, 90}, {1175, 1175, 240}}},
};

static int16_t *s_pcm[CCORD_EARCON_COUNT];
static size_t s_frames[CCORD_EARCON_COUNT];

static void ear_synth(ccord_earcon_kind k)
{
  size_t total = 0;
  for (int i = 0; i < 4; i++) total += (size_t)k_defs[k].notes[i].ms * EAR_RATE / 1000u;
  int16_t *pcm = ccord_calloc(total * 2u, sizeof *pcm);
  if (!pcm) return;
  size_t at = 0;
  for (int i = 0; i < 4; i++) {
    const ear_note *n = &k_defs[k].notes[i];
    size_t len = (size_t)n->ms * EAR_RATE / 1000u;
    float phase = 0.0f;
    size_t attack = EAR_RATE / 200u;                  // 5 ms click-free ramp
    size_t release = len > 0 ? len / 3u : 0;
    for (size_t j = 0; j < len && n->hz_from > 0.0f; j++) {
      float t = (float)j / (float)len;
      float hz = n->hz_from + (n->hz_to - n->hz_from) * t;
      phase += 2.0f * EAR_PI * hz / (float)EAR_RATE;
      if (phase > 2.0f * EAR_PI) phase -= 2.0f * EAR_PI;
      float env = 1.0f;
      if (j < attack) env = (float)j / (float)attack;
      if (len - j < release) env *= (float)(len - j) / (float)release;
      // Fundamental plus a soft second harmonic reads as a "bell" rather than a test tone.
      float s = (sinf(phase) + 0.25f * sinf(2.0f * phase)) / 1.25f;
      int16_t v = (int16_t)(s * env * EAR_PEAK * 32767.0f);
      pcm[(at + j) * 2u] = v;
      pcm[(at + j) * 2u + 1u] = v;
    }
    at += len;
  }
  s_pcm[k] = pcm;
  s_frames[k] = total;
}

const int16_t *ccord_earcon_pcm(ccord_earcon_kind kind, size_t *frames)
{
  if (frames) *frames = 0;
  if ((unsigned)kind >= CCORD_EARCON_COUNT) return nullptr;
  if (!s_pcm[kind]) ear_synth(kind);
  if (frames) *frames = s_frames[kind];
  return s_pcm[kind];
}

// ---------------------------------------------------------------- state

struct ccord_earcons {
  ccord_audio *out;
  int64_t open_tried_ms;
  bool open_tried;
  ccord_app_desktop *desktop;   // last pumped desktop glue (verify slices deliver through it)
  ccord_earcons_debug dbg;
  uint32_t burst;            // enqueues in the current pump
  // voice diff
  bool voice_valid;
  ccord_id voice_channel;
  ccord_id members[EAR_MAX_VOICE];
  size_t member_count;
  bool self_mute, self_deaf;
  bool self_suppress;        // stage audience flag of my voice state
  bool outgoing_call;        // this connection is a DM call I started (not one I answered)
  int64_t quiet_stream_until_ms;   // a stream that ends because my connection ended or moved makes no stop sound
  // push-to-talk
  bool ptt_held;
  int ptt_arm;               // pumps still to settle after PTT mode / the connection came up
  // streams
  bool stream_on, watching;
  bool viewers_valid;
  uint32_t viewers;
  // call rings: incoming (a call rings for me) and outgoing (my call rings the others)
  bool ringing;
  int64_t next_ring_ms;
  ccord_id last_incoming;    // the channel an incoming call last rang for; answering it is not an outgoing call
  bool out_ringing;
  int64_t next_out_ring_ms;
};

static ccord_earcons *s_instance;

ccord_earcons *ccord_earcons_create(void)
{
  ccord_earcons *e = ccord_calloc(1, sizeof *e);
  if (e) s_instance = e;
  return e;
}

void ccord_earcons_destroy(ccord_earcons *e)
{
  if (!e) return;
  if (s_instance == e) s_instance = nullptr;
  if (e->out) ccord_audio_destroy(e->out);
  ccord_free(e);
  for (int k = 0; k < CCORD_EARCON_COUNT; k++) {
    ccord_free(s_pcm[k]);
    s_pcm[k] = nullptr;
    s_frames[k] = 0;
  }
}

ccord_earcons *ccord_earcons_instance(void) { return s_instance; }

ccord_app_desktop *ccord_earcons_desktop(const ccord_earcons *e) { return e ? e->desktop : nullptr; }

void ccord_earcons_get_debug(const ccord_earcons *e, ccord_earcons_debug *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!e) return;
  *out = e->dbg;
  out->ringing = e->ringing;
  out->outgoing_ringing = e->out_ringing;
  out->device = e->out && ccord_audio_available(e->out);
}

// ---------------------------------------------------------------- gating + output

// Message Sounds on, Streamer Mode "Disable Sounds" off and the user not in Do Not Disturb. The per-sound toggle of
// the kind is checked on top of this by ear_emit.
static bool sounds_allowed(const ccord_ui *ui, const ccord_store *store)
{
  if (ui) {
    ccord_ui_settings_debug sd;
    ccord_ui_get_settings_debug(ui, &sd);
    if (!sd.notify_sounds) return false;
    if (sd.streamer_mode && sd.streamer_disable_sounds) return false;
  }
  if (store) {
    const ccord_user *me = ccord_store_user(store, ccord_store_self_id(store));
    if (me && me->presence == CCORD_PRESENCE_DND) return false;
  }
  return true;
}

static void ear_play(ccord_earcons *e, ccord_earcon_kind kind, int64_t now_ms)
{
  if (e->burst >= EAR_MAX_PER_PUMP) {
    e->dbg.dropped++;
    return;
  }
  size_t frames = 0;
  const int16_t *pcm = ccord_earcon_pcm(kind, &frames);
  if (!pcm || frames == 0) return;
  e->burst++;
  e->dbg.enqueued[kind]++;
  e->dbg.total++;
  e->dbg.last = kind;
  e->dbg.last_frames = frames;
  int peak = 0;
  for (size_t i = 0; i < frames * 2u; i++) {
    int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
    if (v > peak) peak = v;
  }
  e->dbg.last_peak = (int16_t)peak;
  if (!e->out && (!e->open_tried || now_ms - e->open_tried_ms >= EAR_RETRY_OPEN_MS)) {
    e->open_tried = true;
    e->open_tried_ms = now_ms;
    ccord_audio_config cfg = {.sample_rate = EAR_RATE, .channels = 2, .frame_samples = 960, .capture = false};
    ccord_error err;
    e->out = ccord_audio_create(&cfg, &err);
  }
  // A second SDL stream on the same device is mixed with the voice/video playback streams.
  if (e->out && ccord_audio_available(e->out)) (void)ccord_audio_write(e->out, pcm, frames);
}

// Plays `kind` when sounds are allowed right now and its own toggle is on.
static void ear_emit(ccord_earcons *e, ccord_earcon_kind kind, const ccord_ui *ui, const ccord_store *store,
                     int64_t now_ms)
{
  if (sounds_allowed(ui, store) && ui_sound_kind_enabled(ui, (unsigned)kind)) ear_play(e, kind, now_ms);
}

// ---------------------------------------------------------------- notifications

// The desktop-notification decision without its "window focused" and "desktop notifications" switches: those
// decide whether the OS popup happens, not whether the sound is wanted.
static bool notification_wants_sound(const ccord_notification *n, const ccord_store *store, int64_t now_ms)
{
  ccord_app_notify_prefs prefs = {.notify_desktop = true, .self_id = store ? ccord_store_self_id(store) : CCORD_ID_NONE};
  return ccord_app_desktop_notify_eligible(n, &prefs, store, now_ms);
}

// ccord_app_desktop's sound hook: a delivered notification plays the message/mention earcon, not the legacy tone.
// Without a live instance (earcons destroyed) it declines so the desktop falls back to its tone.
static bool notification_sound_hook(const ccord_notification *n, const ccord_ui *ui, const ccord_store *store,
                                    int64_t now_ms)
{
  ccord_earcons *e = s_instance;
  if (!e || !n) return false;
  ear_emit(e, n->is_mention ? CCORD_EARCON_MENTION : CCORD_EARCON_MESSAGE, ui, store, now_ms);
  return true;
}

static void pump_notifications(ccord_earcons *e, ccord_session *session, const ccord_store *store, const ccord_ui *ui,
                               ccord_app_desktop *desktop, bool virtual_clock, int64_t now_ms)
{
  ccord_notification n;
  ccord_platform *plat = desktop ? ccord_app_desktop_platform(desktop) : nullptr;
  bool focused = plat && ccord_platform_window_focused(plat);
  ccord_id selected = store ? ccord_store_selected_channel(store) : CCORD_ID_NONE;
  while (session && ccord_session_pop_notification(session, &n)) {
    // Delivery requests its sound through notification_sound_hook for a notification the desktop accepted;
    // every other case is ours.
    bool delivered = !virtual_clock && desktop && ccord_app_desktop_deliver(desktop, &n, now_ms);
    if (delivered || !notification_wants_sound(&n, store, now_ms)) continue;
    // Suppress the message earcon when the user is actively looking at that channel.
    if (!n.is_mention && focused && selected != CCORD_ID_NONE && n.channel_id == selected) continue;
    ear_emit(e, n.is_mention ? CCORD_EARCON_MENTION : CCORD_EARCON_MESSAGE, ui, store, now_ms);
  }
}

// ---------------------------------------------------------------- voice

static const ccord_voice_state *self_voice(const ccord_store *store, ccord_id channel)
{
  ccord_id self = ccord_store_self_id(store);
  if (self == CCORD_ID_NONE) return nullptr;
  const ccord_channel *ch = ccord_store_channel(store, channel);
  ccord_id guild = ch ? ch->guild_id : ccord_store_selected_guild(store);
  return ccord_store_voice_user(store, guild, self);
}

static bool has_member(const ccord_id *ids, size_t n, ccord_id id)
{
  for (size_t i = 0; i < n; i++) {
    if (ids[i] == id) return true;
  }
  return false;
}

static bool is_dm_channel(const ccord_store *store, ccord_id ch)
{
  const ccord_channel *c = ccord_store_channel(store, ch);
  return c && c->guild_id == CCORD_DM_GUILD_ID;
}

static bool is_stage_channel(const ccord_store *store, ccord_id ch)
{
  const ccord_channel *c = ccord_store_channel(store, ch);
  return c && c->kind == CCORD_CHANNEL_STAGE;
}

// Own connection state and the other members of my channel. Returns whether I am connected.
static bool pump_voice(ccord_earcons *e, const ccord_store *store, const ccord_ui *ui, int64_t now_ms)
{
  ccord_id ch = ccord_store_active_voice_channel(store);
  ccord_id self = ccord_store_self_id(store);
  if (ch == CCORD_ID_NONE || self == CCORD_ID_NONE) {
    if (e->voice_valid) {   // I left, or the server dropped me
      ear_emit(e, CCORD_EARCON_SELF_DISCONNECT, ui, store, now_ms);
      e->quiet_stream_until_ms = now_ms + 500;
    }
    e->voice_valid = false;
    e->voice_channel = CCORD_ID_NONE;
    e->member_count = 0;
    e->outgoing_call = false;
    e->self_suppress = false;
    return false;
  }
  ccord_id cur[EAR_MAX_VOICE];
  size_t cn = 0;
  size_t total = ccord_store_voice_count(store, ch);
  for (size_t i = 0; i < total && cn < EAR_MAX_VOICE; i++) {
    const ccord_voice_state *v = ccord_store_voice_at(store, ch, i);
    if (v && v->user_id != self && v->channel_id == ch) cur[cn++] = v->user_id;
  }
  const ccord_voice_state *me = self_voice(store, ch);
  bool mute = me && me->self_mute, deaf = me && me->self_deaf, suppress = me && me->suppress;
  if (e->voice_valid && e->voice_channel == ch) {   // same connection: diff
    for (size_t i = 0; i < cn; i++) {
      if (!has_member(e->members, e->member_count, cur[i])) ear_emit(e, CCORD_EARCON_VOICE_JOIN, ui, store, now_ms);
    }
    for (size_t i = 0; i < e->member_count; i++) {
      if (!has_member(cur, cn, e->members[i])) ear_emit(e, CCORD_EARCON_VOICE_LEAVE, ui, store, now_ms);
    }
    if (deaf != e->self_deaf) {   // deafening also mutes: one sound for the action the user took
      ear_emit(e, deaf ? CCORD_EARCON_DEAFEN : CCORD_EARCON_UNDEAFEN, ui, store, now_ms);
    } else if (mute != e->self_mute) {
      ear_emit(e, mute ? CCORD_EARCON_MUTE : CCORD_EARCON_UNMUTE, ui, store, now_ms);
    }
    if (e->self_suppress && !suppress && is_stage_channel(store, ch)) {   // a moderator invited me to speak
      ear_emit(e, CCORD_EARCON_INVITED_TO_SPEAK, ui, store, now_ms);
    }
  } else {   // a (re)connect only sets the member baseline
    if (e->voice_valid) {
      // Channel changed while connected. The UI opens the stage view of the channel it asked for before the server
      // answers, so a switch the user made is a connect; anything else was somebody moving me.
      bool own = ui && ui->stage_channel == ch;
      ear_emit(e, own ? CCORD_EARCON_SELF_CONNECT : CCORD_EARCON_USER_MOVED, ui, store, now_ms);
      e->quiet_stream_until_ms = now_ms + 500;
    } else {
      ear_emit(e, CCORD_EARCON_SELF_CONNECT, ui, store, now_ms);
    }
    e->outgoing_call = is_dm_channel(store, ch) && ch != e->last_incoming;
  }
  e->voice_valid = true;
  e->voice_channel = ch;
  memcpy(e->members, cur, cn * sizeof cur[0]);
  e->member_count = cn;
  e->self_mute = mute;
  e->self_deaf = deaf;
  e->self_suppress = suppress;
  return true;
}

// ---------------------------------------------------------------- push-to-talk

// Only in push-to-talk input mode while connected. A few pumps after the mode or the connection comes up are only
// baseline: the media pump takes a frame to replace the voice-activity "always open" gate with the real key state.
static void pump_ptt(ccord_earcons *e, const ccord_session *session, const ccord_store *store, const ccord_ui *ui,
                     bool connected, int64_t now_ms)
{
  ccord_ui_voice_prefs vp = {0};
  if (ui) ccord_ui_voice_settings(ui, &vp);
  if (!connected || !ui || !vp.ptt) {
    e->ptt_arm = 2;
    e->ptt_held = false;
    return;
  }
  bool held = ccord_session_ptt_held(session);
  if (e->ptt_arm > 0) {
    e->ptt_arm--;
    e->ptt_held = held;
    return;
  }
  if (held != e->ptt_held) {
    e->ptt_held = held;
    ear_emit(e, held ? CCORD_EARCON_PTT_ON : CCORD_EARCON_PTT_OFF, ui, store, now_ms);
  }
}

// ---------------------------------------------------------------- Go Live

// My own stream and a stream I watch start / stop; the viewer count of my stream going up / down. A stream that
// ends because my connection ended or moved is covered by the connect/disconnect sound (quiet_stream_until_ms).
static void pump_streams(ccord_earcons *e, const ccord_session *session, const ccord_store *store, const ccord_ui *ui,
                         int64_t now_ms)
{
  ccord_id self = ccord_store_self_id(store);
  bool quiet = now_ms < e->quiet_stream_until_ms;
  bool mine = ccord_store_stream_active(store);
  if (mine != e->stream_on) {
    e->stream_on = mine;
    e->viewers_valid = false;
    if (mine) ear_emit(e, CCORD_EARCON_STREAM_START, ui, store, now_ms);
    else if (!quiet) ear_emit(e, CCORD_EARCON_STREAM_STOP, ui, store, now_ms);
  }
  const ccord_watched_stream *w = ccord_session_watched_stream(session);
  bool watching = w && w->active && w->user_id != self;   // watching my own preview is covered by `mine`
  if (watching != e->watching) {
    e->watching = watching;
    if (watching) ear_emit(e, CCORD_EARCON_STREAM_START, ui, store, now_ms);
    else if (!quiet) ear_emit(e, CCORD_EARCON_STREAM_STOP, ui, store, now_ms);
  }
  const ccord_stream_state *ms = mine ? ccord_store_stream(store) : nullptr;
  const ccord_stream_state *row = ms ? ccord_store_stream_find(store, ms->channel_id, self) : nullptr;
  if (!row) {   // the server has not reported the broadcast yet: the first count is only the baseline
    e->viewers_valid = false;
    return;
  }
  uint32_t n = ccord_store_stream_viewer_count(store, ms->channel_id, self);
  if (e->viewers_valid && n != e->viewers) {
    ear_emit(e, n > e->viewers ? CCORD_EARCON_VIEWER_JOIN : CCORD_EARCON_VIEWER_LEAVE, ui, store, now_ms);
  }
  e->viewers = n;
  e->viewers_valid = true;
}

// ---------------------------------------------------------------- incoming call ring

static void pump_ring(ccord_earcons *e, const ccord_session *session, const ccord_store *store, const ccord_ui *ui,
                      int64_t now_ms)
{
  const ccord_call_state *in = ccord_session_incoming_call(session);   // answering or ending the call stops it
  if (in) e->last_incoming = in->channel_id;
  else if (!e->voice_valid) e->last_incoming = CCORD_ID_NONE;
  if (!in) {
    e->ringing = false;
  } else if (!e->ringing || now_ms >= e->next_ring_ms) {
    e->ringing = true;
    e->next_ring_ms = now_ms + CCORD_EARCON_RING_PERIOD_MS;
    ear_emit(e, CCORD_EARCON_CALL_RING, ui, store, now_ms);
  }
  // My own call: I am connected to a DM call I started and the others still ring. It stops when one answers (the
  // ringing list empties), when I cancel, and when the call ends.
  const ccord_call_state *call = e->voice_valid && e->outgoing_call ? ccord_store_call(store, e->voice_channel) : nullptr;
  bool out = call && call->active && call->ringing_count > 0;
  if (!out) {
    e->out_ringing = false;
  } else if (!e->out_ringing || now_ms >= e->next_out_ring_ms) {
    e->out_ringing = true;
    e->next_out_ring_ms = now_ms + CCORD_EARCON_OUTGOING_RING_PERIOD_MS;
    ear_emit(e, CCORD_EARCON_OUTGOING_RING, ui, store, now_ms);
  }
}

void ccord_earcons_pump(ccord_earcons *e, ccord_session *session, const ccord_store *store, const ccord_ui *ui,
                        ccord_app_desktop *desktop, bool virtual_clock, int64_t now_ms)
{
  if (!e) return;
  e->burst = 0;
  e->desktop = desktop;
  if (desktop) ccord_app_desktop_set_sound_hook(desktop, notification_sound_hook);
  if (!store || !session) return;
  pump_notifications(e, session, store, ui, desktop, virtual_clock, now_ms);
  bool connected = pump_voice(e, store, ui, now_ms);
  pump_ptt(e, session, store, ui, connected, now_ms);
  pump_streams(e, session, store, ui, now_ms);
  pump_ring(e, session, store, ui, now_ms);
}
