// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Private to the session module (not under include/): routes CCORD_NET_MEDIA_RANGE events to the open media streams
// and issues their next range requests. Implemented in media_stream.c.
#pragma once
#include <stdint.h>

#include <ccord/net/transport.h>

typedef struct ccord_stream_hub ccord_stream_hub;
ccord_stream_hub *ccord_stream_hub_create(ccord_transport *t);
void ccord_stream_hub_destroy(ccord_stream_hub *h);                                        // cancels + frees streams
void ccord_stream_hub_on_range(ccord_stream_hub *h, const ccord_net_event *ev, uint64_t now_ms);   // MEDIA_RANGE
void ccord_stream_hub_pump(ccord_stream_hub *h, uint64_t now_ms);   // issues the next range per stream, retries, expires
// Max ms before ccord_stream_hub_pump() needs to run again (UINT32_MAX when all open streams are idle/buffered).
uint32_t ccord_stream_hub_next_wait_ms(const ccord_stream_hub *h, uint64_t now_ms);
// Used by ccord_session_open_media_stream / _close_media_stream / ccord_session_set_transport (session.c).
#include <ccord/session/media_stream.h>
ccord_media_stream *ccord_stream_hub_open(ccord_stream_hub *h, const char *url, ccord_error *err);
ccord_media_stream *ccord_stream_hub_open_ex(ccord_stream_hub *h, const char *url, uint32_t max_blocks,
                                             ccord_error *err);
void ccord_stream_hub_close(ccord_stream_hub *h, ccord_media_stream *ms);                 // nullptr-safe
void ccord_stream_hub_set_transport(ccord_stream_hub *h, ccord_transport *t);             // in-flight ranges are re-issued
