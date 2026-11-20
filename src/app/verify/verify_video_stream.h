// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Headless streamed-video verification (`ccord --headless --mock --verify-slice video-stream`).
//
// Proves the Range-streaming path
// session stream hub and the real demux thread, with the mock transport serving byte ranges of a *virtual* file:
//   * a small real MP4 (H.264 320x180 + mono AAC, moov at the end) is re-laid out so its media regions sit a stride
//     apart and its moov (patched in place) is last, giving a clip of more than 40 MiB that is synthesised range by
//     range and never held in memory;
//   * the first frame must draw while only a fraction of that file was fetched, a seek must fetch only the ranges it
//     needs, a play-through must keep the stream's cache within 16 MiB, redirects must be carried, a server that
//     ignores Range must fail a big file clearly and fall back to a whole-file download for a small one, an expired
//     link (403) must stop the stream with one warning, and no log line may carry a query string.
// Prints `video-stream: PASS checks=N` (exit 0) or `video-stream: FAIL <check>` (exit 1).
//
// CCORD_VERIFY_VIDEO_STREAM_WHOLE=1 makes the mock's big file answer every range as a whole 200 (a negative check of
// the slice: the first-frame check must then FAIL).
#include <stdint.h>

#include <ccord/core/input.h>
#include <ccord/net/transport.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

typedef struct ccord_verify_video_stream_ctx {
  ccord_session *session;
  ccord_store *store;
  ccord_ui *ui;
  ccord_renderer *renderer;
  ccord_transport *transport;               // the offline mock transport
  void *ud;
  void (*frames)(void *ud, int n);          // advance n virtual frames (pump + ui update + draw)
  void (*input)(void *ud, const ccord_input_event *ev);
} ccord_verify_video_stream_ctx;

[[nodiscard]] int ccord_verify_video_stream_run(const ccord_verify_video_stream_ctx *ctx);
