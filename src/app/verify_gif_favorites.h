// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Headless GIF-picker Favorites verification (`ccord --headless --mock --verify-slice gif-favorites`).
//
// Loads favorites shaped like real Discord settings-proto data (a Tenor .mp4, a media.discordapp.net .gif, a
// cdn.discordapp.com attachment with an ?ex=&is=&hm= query, a VIDEO-format src without a file extension) through the
// mock transport's settings-proto path, opens the Favorites view through the real picker draw/load code and pumps
// frames until every tile holds a texture decoded from the bytes the mock served, or its load has failed.
// Prints `gif-favorites: PASS checks=N` (exit 0) or `gif-favorites: FAIL <check>` (exit 1).
#include <stdint.h>

#include <ccord/core/input.h>
#include <ccord/net/transport.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

typedef struct ccord_verify_gif_favorites_ctx {
  ccord_session *session;
  ccord_store *store;
  ccord_ui *ui;
  ccord_renderer *renderer;
  ccord_transport *transport;               // the offline mock transport
  void *ud;
  void (*frames)(void *ud, int n);          // advance n virtual frames (pump + ui update + draw)
  void (*input)(void *ud, const ccord_input_event *ev);
} ccord_verify_gif_favorites_ctx;

[[nodiscard]] int ccord_verify_gif_favorites_run(const ccord_verify_gif_favorites_ctx *ctx);
