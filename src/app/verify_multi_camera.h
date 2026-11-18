// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// --verify-slice multi-camera: several remote voice cameras over the offline
// mock transport, through the real session, app_media decoders, renderer textures and call UI. Returns 0 only after
// printing `multi-camera: PASS checks=N`; otherwise prints `multi-camera: FAIL <check>` and returns non-zero.
#include <ccord/net/transport.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "app_media.h"

typedef struct ccord_verify_multi_camera_ctx {
  ccord_session *session;
  ccord_store *store;
  ccord_ui *ui;
  ccord_renderer *renderer;
  ccord_transport *transport;   // the offline mock
  ccord_app_media *media;       // reads ccord_app_media_cam_count / ccord_app_media_cam_debug_of
  void *ud;
  void (*frames)(void *ud, int n);   // n virtual frames: pump + ui update + draw (100 ms each)
  // Writes a PNG of one fresh frame; the pixels are read between draw and present, as the screenshot mode does.
  ccord_status (*screenshot)(void *ud, const char *path, ccord_error *err);
  // Logical size last given to ccord_ui_set_viewport.
  void (*viewport)(void *ud, float *logical_w, float *logical_h);
} ccord_verify_multi_camera_ctx;

[[nodiscard]] int ccord_verify_multi_camera_slice(const ccord_verify_multi_camera_ctx *ctx);
