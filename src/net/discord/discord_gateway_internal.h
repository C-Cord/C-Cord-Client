// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Internal gateway hooks shared between discord_gateway.c and its unit tests.
// Not part of the public live_transport.h surface.
#pragma once
#include <ccord/net/live_transport.h>

// zlib-stream transport compression is on by default (negotiated via the URL).
// Tests turn it off to feed raw-JSON fixtures without a deflate dictionary.
// Only effective before ccord_gateway_start; the URL parameter is added at open.
void ccord_gateway_set_transport_compress(ccord_gateway *g, bool enable);

// Internal rich-activities JSON validator shared between gateway_wire.c and discord_gateway.c.
ccord_status ccord_gateway_rich_activities_valid_(const char *json, size_t len, ccord_error *err);

// Returns true when `type` (length `len`) matches an OP 0 dispatch event handled by json_dispatch.c.
// Used by discord_gateway.c to fast-skip unhandled dispatches without allocating a scratch arena or JSON AST.
bool ccord_live_gateway_dispatch_handled(const char *type, size_t len);
