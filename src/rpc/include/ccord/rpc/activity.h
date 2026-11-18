// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>
#include <ccord/core/json.h>

#include <stddef.h>
#include <stdint.h>

// Rich Presence activity model shared by the local Discord RPC bridge and the gateway presence writer.
//
// An activity comes from a Discord RPC SET_ACTIVITY payload (an object) and is serialized into the gateway op 3
// `activities` array. Text fields keep the first CCORD_RPC_ACTIVITY_TEXT_MAX bytes on a UTF-8 boundary; all buffers
// are fixed-size so a parsed activity owns its strings and never points into the input arena.
#define CCORD_RPC_ACTIVITY_TEXT_MAX 128u
#define CCORD_RPC_ACTIVITY_MAX 4u          // at most four activities are serialized into one presence update
#define CCORD_RPC_ACTIVITY_BUTTONS_MAX 2u

// Discord RPC frame: u32 little-endian op, u32 little-endian payload length, then that many UTF-8 JSON bytes.
#define CCORD_RPC_FRAME_MAX_OP 4u
#define CCORD_RPC_FRAME_MAX_PAYLOAD 65536u

typedef struct ccord_rpc_activity {
  char name[CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];       // empty when the payload has no name (serialized as "Rich Presence")
  int type;                                          // 0 Playing, 1 Streaming, 2 Listening, 3 Watching, 5 Competing
  char application_id[CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];
  char details[CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];
  char state[CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];
  char url[CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];         // streaming URL (type 1), empty otherwise
  int64_t start_ms;                                  // Unix milliseconds; 0 when unset
  int64_t end_ms;
  char large_image[CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];
  char large_text[CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];
  char small_image[CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];
  char small_text[CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];
  char party_id[CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];
  int party_size_cur;                                // 0 when no party size
  int party_size_max;
  char secret_join[CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];
  char secret_spectate[CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];
  char secret_match[CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];
  size_t button_count;                               // 0..CCORD_RPC_ACTIVITY_BUTTONS_MAX
  char button_labels[CCORD_RPC_ACTIVITY_BUTTONS_MAX][CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];
  char button_urls[CCORD_RPC_ACTIVITY_BUTTONS_MAX][CCORD_RPC_ACTIVITY_TEXT_MAX + 1u];
  bool instance;
} ccord_rpc_activity;

// Parses one activity object. Rejects an unknown type (only 0, 1, 2, 3, 5), more than two buttons, and a button URL
// that is not http:// or https://. Timestamps below 1e11 are seconds and are converted to milliseconds. On success
// `out` is fully initialized; on failure it is zeroed and `err` explains why.
[[nodiscard]] ccord_status ccord_rpc_activity_parse(const ccord_json *json_value, ccord_rpc_activity *out,
                                                    ccord_error *err);

// Writes the gateway `activities` array for `n` activities into `out` as NUL-terminated JSON. At most
// CCORD_RPC_ACTIVITY_MAX entries are written; the rest are ignored. Buttons serialize as `buttons: [labels]` plus
// `metadata: { button_urls: [urls] }`, matching what the Discord client sends. Returns CCORD_ERR_RANGE when `out` is
// too small or the writer fails.
[[nodiscard]] ccord_status ccord_rpc_activity_to_gateway_json(const ccord_rpc_activity *acts, size_t n, char *out,
                                                             size_t cap);

// Encodes a frame header plus payload. Requires op <= CCORD_RPC_FRAME_MAX_OP, len <= CCORD_RPC_FRAME_MAX_PAYLOAD and
// cap >= 8 + len. `json` may be null only when `len` is 0.
[[nodiscard]] ccord_status ccord_rpc_frame_encode(char *out, size_t cap, uint32_t op, const char *json, size_t len);

typedef enum ccord_rpc_frame_result {
  CCORD_RPC_FRAME_NEED_MORE,  // fewer than 8 header bytes, or the payload is still incomplete
  CCORD_RPC_FRAME_FRAME,      // a complete frame: payload/plen/consumed are set
  CCORD_RPC_FRAME_BAD,        // op > 4 or length > CCORD_RPC_FRAME_MAX_PAYLOAD
} ccord_rpc_frame_result;

// Decodes one frame from the front of `buf`. `payload` points into `buf`, `consumed` is 8 + payload length.
ccord_rpc_frame_result ccord_rpc_frame_decode(const char *buf, size_t len, uint32_t *op, const char **payload,
                                              size_t *plen, size_t *consumed);
