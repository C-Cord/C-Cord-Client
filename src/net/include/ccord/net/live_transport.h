// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/net/governor.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/error.h>
#include <ccord/core/link_event.h>
#include <ccord/net/gateway_pace.h>
#include <ccord/net/transport.h>
#include <ccord/net/voice_reconnect.h>

// Live Discord transport: HTTPS REST v9 + Gateway v9 WebSocket, plus the
// standalone C-Cord token loader. No payment or store route is ever reachable.

#define CCORD_LIVE_TOKEN_MAX 512u
#define CCORD_LIVE_API_BASE "https://discord.com/api/v9"
#define CCORD_LIVE_GATEWAY_URL "wss://gateway.discord.gg/?v=9&encoding=json"
#define CCORD_LIVE_HISTORY_DEFAULT 50u
#define CCORD_LIVE_TIMEOUT_DEFAULT_MS 15000u
#define CCORD_LIVE_MAX_RESPONSE (8u * 1024u * 1024u)
// Largest media body (image, GIF, video, audio) fetched for inline display; matches the decoders' input limit.
#define CCORD_LIVE_MAX_MEDIA (32u * 1024u * 1024u)
// Web-client capability bitmask sent in IDENTIFY (user accounts need these to
// receive lazy guild payloads, replies and attachment/embed metadata).
#define CCORD_LIVE_CAPABILITIES 1734653u

typedef struct ccord_live_config {
  const char *token;             // explicit token; null/empty -> env or standalone token file
  const char *token_file;        // optional token file override; null -> $XDG_CONFIG_HOME/ccord/token
  const char *api_base;          // null -> CCORD_LIVE_API_BASE
  const char *gateway_url;       // null -> CCORD_LIVE_GATEWAY_URL
  uint32_t connect_timeout_ms;   // 0 -> default
  uint32_t request_timeout_ms;   // 0 -> default
  const char *cache_dir;         // per-account dir (<data>/accounts/<fp>): scopes the API cache to <dir>/api and enables
                                 // warm history files; null = legacy shared API cache, no warm history
  bool disable_gateway;          // tests/offline: no socket is opened
  uint64_t (*now_ms)(void *ud);  // injectable clock; null -> ccord_clock_ms
  void *clock_ud;
} ccord_live_config;

// Wraps the token in a ccord_transport vtable. The token is copied and wiped on
// destroy; it is never logged.
[[nodiscard]] ccord_transport *ccord_live_transport_create(const ccord_live_config *cfg, ccord_error *err);

// "<cache_dir>/warm/history/<channel_id>.json"; false on null/empty/overflow. Written by the transport when a
// channel's initial history page arrives, read back by the warm-start loader.
[[nodiscard]] bool ccord_live_history_cache_path(const char *cache_dir, ccord_id channel_id, char *out, size_t cap);

// Environment variable checked before the standalone token file.

// Domain-split public headers (re-exported here for backwards compatibility).
#include <ccord/net/token_store.h>
#include <ccord/net/discord_rest.h>
#include <ccord/net/discord_json.h>
#include <ccord/net/discord_gateway.h>
#include <ccord/net/voice_gateway.h>
