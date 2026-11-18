// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Headless chat-video verification (`ccord --headless --mock --verify-slice embed-video`).
//
// Delivers messages shaped like Discord's JSON (parsed by the real ccord_live_json_message) through the mock
// transport: a video attachment whose URLs carry ?ex=&is=&hm=, a native Discord embed, a third-party embed with
// video.proxy_url, an attachment with a 1.6 KB signed URL, an embed on a host the live transport refuses and an
// attachment whose bytes are not a video. Each is clicked through the real hit-rect/click handler; a kind passes
// only when the click led to a decoded first frame whose size equals the clip the transport served (real MP4s of two
// different sizes; the mock's own 16x8 stand-in would not match). The refused and undecodable kinds must instead
// produce exactly one `player:` warning that names the stage and host and carries no query string.
// Prints `embed-video: PASS checks=N` (exit 0) or `embed-video: FAIL <check>` (exit 1).
//
// CCORD_VERIFY_EMBED_VIDEO_CORRUPT=<kind name> serves random bytes for that kind (a negative check of the slice).
#include <stdint.h>

#include <ccord/core/input.h>
#include <ccord/net/transport.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

typedef struct ccord_verify_embed_video_ctx {
  ccord_session *session;
  ccord_store *store;
  ccord_ui *ui;
  ccord_renderer *renderer;
  ccord_transport *transport;               // the offline mock transport
  void *ud;
  void (*frames)(void *ud, int n);          // advance n virtual frames (pump + ui update + draw)
  void (*input)(void *ud, const ccord_input_event *ev);
} ccord_verify_embed_video_ctx;

[[nodiscard]] int ccord_verify_embed_video_run(const ccord_verify_embed_video_ctx *ctx);
