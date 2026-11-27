// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Earcons: short synthesized notification and voice sounds (no sound files) plus the looping incoming-call
// ring. One per-frame call diffs observable session/store state and enqueues the matching earcon on its own
// playback stream, so it mixes with voice/video output on the shared device instead of interrupting it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "app_desktop.h"

typedef enum ccord_earcon_kind : uint8_t {
  CCORD_EARCON_MESSAGE,
  CCORD_EARCON_MENTION,
  CCORD_EARCON_VOICE_JOIN,
  CCORD_EARCON_VOICE_LEAVE,
  CCORD_EARCON_MUTE,
  CCORD_EARCON_UNMUTE,
  CCORD_EARCON_DEAFEN,
  CCORD_EARCON_UNDEAFEN,
  CCORD_EARCON_CALL_RING,   // one burst; repeated every CCORD_EARCON_RING_PERIOD_MS while a call rings
  CCORD_EARCON_SELF_CONNECT,      // I joined a voice channel or call
  CCORD_EARCON_SELF_DISCONNECT,   // I left, or was disconnected from, voice or a call
  CCORD_EARCON_USER_MOVED,        // I was moved to another voice channel (not by my own click)
  CCORD_EARCON_OUTGOING_RING,     // one burst; repeated every CCORD_EARCON_OUTGOING_RING_PERIOD_MS while my call rings
  CCORD_EARCON_STREAM_START,      // my Go Live began, or I started watching someone's stream
  CCORD_EARCON_STREAM_STOP,
  CCORD_EARCON_VIEWER_JOIN,       // the viewer count of my stream went up
  CCORD_EARCON_VIEWER_LEAVE,
  CCORD_EARCON_PTT_ON,            // push-to-talk key pressed / released (only in push-to-talk input mode)
  CCORD_EARCON_PTT_OFF,
  CCORD_EARCON_INVITED_TO_SPEAK,  // a stage moderator moved me from the audience to the speakers
  CCORD_EARCON_COUNT
} ccord_earcon_kind;

#define CCORD_EARCON_RING_PERIOD_MS 2000
#define CCORD_EARCON_OUTGOING_RING_PERIOD_MS 3000

typedef struct ccord_earcons ccord_earcons;

// The stream is opened lazily on the first sound (a missing device is a silent no-op, never an error).
[[nodiscard]] ccord_earcons *ccord_earcons_create(void);
void ccord_earcons_destroy(ccord_earcons *e);
// The most recently created instance (verify slices reach the app's instance through it); nullptr if none.
[[nodiscard]] ccord_earcons *ccord_earcons_instance(void);

// The desktop glue the last ccord_earcons_pump received (nullptr before the first pump).
[[nodiscard]] ccord_app_desktop *ccord_earcons_desktop(const ccord_earcons *e);
// Optional plugin contribution registry for Class 2 earcon overrides.
void ccord_earcons_set_contrib(ccord_earcons *e, const ccord_contrib *contrib);
// Plays a named earcon ("message", "mention", "mute", ...) immediately on the earcon stream.
bool ccord_earcons_play_kind(ccord_earcons *e, const char *kind_name, int64_t now_ms);
// Decodes WAV/OGG/MP3 audio bytes and plays them immediately on the earcon stream.
bool ccord_earcons_play_bytes(ccord_earcons *e, const uint8_t *data, size_t len, int64_t now_ms);

// Call once per app frame, before ccord_app_desktop_pump. It pops the session notification queue (message and
// mention sounds, then delivers the desktop notification like ccord_app_desktop_pump would unless
// `virtual_clock`), and diffs voice/call state. It registers the desktop's notification-sound hook, so every
// eligible notification makes exactly one message/mention earcon whether or not the OS popup was delivered and
// the legacy 880 Hz tone is never written. Nothing plays when Settings > Notifications > Message Sounds is
// off, when Streamer Mode's Disable Sounds is on, or while the user's status is Do Not Disturb; a kind whose own
// toggle (Customize under Sounds, ui_sound_kind_enabled) is off is skipped while the others still play.
// `now_ms` is monotonic ms (for ring timers and audio idle close); `now_unix_ms` is wall-clock Unix ms (for mute expiry).
void ccord_earcons_pump(ccord_earcons *e, ccord_session *session, const ccord_store *store, const ccord_ui *ui,
                        ccord_app_desktop *desktop, bool virtual_clock, int64_t now_ms, int64_t now_unix_ms);
// Max ms before ccord_earcons_pump() needs to run again (e.g. closing an idle playback stream or repeating a ring).
// Returns UINT32_MAX when idle with no open audio stream and no active call ring.
uint32_t ccord_earcons_next_wait_ms(const ccord_earcons *e, int64_t now_ms);

// The synthesized stereo 48 kHz PCM of one kind (static; valid until the process ends); *frames gets its length.
[[nodiscard]] const int16_t *ccord_earcon_pcm(ccord_earcon_kind kind, size_t *frames);

typedef struct ccord_earcons_debug {
  uint32_t enqueued[CCORD_EARCON_COUNT];   // sounds handed to the output, per kind (counted without a device too)
  uint32_t total;
  uint32_t dropped;                        // dropped by the per-pump burst cap
  ccord_earcon_kind last;
  size_t last_frames;
  int16_t last_peak;
  bool ringing;                            // an incoming call is ringing for the user
  bool outgoing_ringing;                   // my own call is ringing the other side
  bool device;                             // a playback stream is open
} ccord_earcons_debug;
void ccord_earcons_get_debug(const ccord_earcons *e, ccord_earcons_debug *out);
