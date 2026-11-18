// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// End-to-end headless live streaming verification (`ccord --headless --verify-slice live-stream`).
//
// Exercises the real app/UI/session/app_media/live_transport/voice_gateway/vstream pipeline against
// forked loopback Discord HTTP, Main Gateway, Voice/Stream Gateway, and UDP SFU sockets.
//
// The forked loopback servers are shared with the stream-audio slice (verify_stream_audio.c) through
// ccord_verify_ls_loopback_start/stop below; both slices run against the same scripted guild,
// channels, SSRCs and AEAD key.
//
// The fake server headers come first: they set the POSIX feature macros every system header in
// this chain needs (clock_gettime, sigaction, fork, ...). The #undef between them mirrors the
// original live-stream include order: redefining the feature macros, even identically, is an
// error under this project's warning set.
#include "fake_discord.h"
#undef _POSIX_C_SOURCE
#undef _DEFAULT_SOURCE
#include "fake_gateway.h"

#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/core/input.h>
#include <ccord/net/transport.h>
#include <ccord/net/voice_crypto.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

typedef struct ccord_verify_live_stream_ctx {
  ccord_session *session;
  ccord_store *store;
  void *ud;
  void (*frames)(void *ud, int n);
  void (*advance_ms)(void *ud, uint64_t ms);
  uint64_t (*now_ms)(void *ud);
  ccord_status (*set_transport)(void *ud, ccord_transport *tr, ccord_error *err);
} ccord_verify_live_stream_ctx;

[[nodiscard]] int ccord_verify_live_stream_run(const ccord_verify_live_stream_ctx *ctx);

// ---- shared loopback Discord harness -----------------------------------------------------------

// Fixed identities the loopback scripts serve: guild 2001 with voice channel 3121, DM call 4444,
// self user 42. The voice connection's READY hands out CCORD_VERIFY_LS_VOICE_SSRC, the Go Live
// stream connection's READY hands out CCORD_VERIFY_LS_STREAM_SSRC (its audio SSRC: stream audio
// rides it with PT CCORD_VSTREAM_AUDIO_PT). Both session descriptions use ccord_verify_ls_key.
#define CCORD_VERIFY_LS_GUILD 2001ULL
#define CCORD_VERIFY_LS_VCHAN 3121ULL
#define CCORD_VERIFY_LS_DM_CHAN 4444ULL
#define CCORD_VERIFY_LS_SELF 42ULL
#define CCORD_VERIFY_LS_VOICE_SSRC 1000u
#define CCORD_VERIFY_LS_STREAM_SSRC 2000u

extern const uint8_t ccord_verify_ls_key[CCORD_VOICE_KEY_BYTES];

typedef struct ccord_verify_ls_loopback {
  fd_server http;        // fake REST API (http.base is the api_base for ccord_live_config)
  fg_server main_gw;     // Main Gateway (main_gw.url is the gateway_url for ccord_live_config)
  fg_server voice_gw;    // Voice Gateway for the microphone/camera connection
  fg_server stream_gw;   // Voice Gateway for the Go Live stream connection
  int voice_udp;         // SFU socket receiving the voice connection's RTP
  int stream_udp;        // SFU socket receiving the stream connection's RTP (PT 101 video, PT 120 audio)
  uint16_t voice_port;
  uint16_t stream_port;
} ccord_verify_ls_loopback;

// Forks the loopback HTTP/Main/Voice/Stream servers and opens the two UDP SFU sockets. On failure
// everything already started is torn down, *fail_reason (when non-null) names the step, and false
// is returned. The forked scripts inherit the UDP fds and close their copies, so only the parent
// keeps them open.
[[nodiscard]] bool ccord_verify_ls_loopback_start(ccord_verify_ls_loopback *lb, const char **fail_reason);

// Stops the servers (SIGTERM + reap), returns what each gateway script logged into the caller's
// FG_MAX_EVENTS-sized arrays, stops the HTTP server and closes the UDP sockets. Null-safe on
// zeroed members, so it can also unwind a partially started harness.
void ccord_verify_ls_loopback_stop(ccord_verify_ls_loopback *lb, fg_event *gw_ev, size_t *ng, fg_event *sg_ev,
                                   size_t *ns, fg_event *vg_ev, size_t *nv);

// ---- stream-audio slice (`ccord --headless --verify-slice stream-audio`) ------------------------

typedef struct ccord_verify_stream_audio_ctx {
  ccord_session *session;
  ccord_store *store;
  ccord_ui *ui;
  void *ud;
  void (*frames)(void *ud, int n);
  void (*advance_ms)(void *ud, uint64_t ms);
  uint64_t (*now_ms)(void *ud);
  void (*input)(void *ud, const ccord_input_event *ev);   // raw UI input event (Go Live modal clicks)
  ccord_status (*set_transport)(void *ud, ccord_transport *tr, ccord_error *err);
} ccord_verify_stream_audio_ctx;

// Runs all stream-audio checks. Returns 0 iff every check passed (prints `stream-audio: PASS checks=N`).
[[nodiscard]] int ccord_verify_stream_audio_run(const ccord_verify_stream_audio_ctx *ctx);
