// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/arena.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>
// Frame = u32 little-endian payload length, then that many bytes of UTF-8 JSON (one object). Max payload 1 MiB.
typedef struct ccord_ipc ccord_ipc;
typedef enum ccord_ipc_kind : uint8_t { CCORD_IPC_REQ, CCORD_IPC_RES, CCORD_IPC_NTF } ccord_ipc_kind;
typedef struct ccord_ipc_msg {
  ccord_ipc_kind kind;
  uint32_t id;                  // REQ/RES, >= 1
  const char *method;           // REQ/NTF
  const ccord_json *params;     // REQ/NTF: object or nullptr
  bool ok;                      // RES
  const ccord_json *result;     // RES ok: any JSON or nullptr
  const char *err_code;         // RES !ok: one of IPC error codes below
  const char *err_message;
} ccord_ipc_msg;
typedef enum ccord_ipc_read : uint8_t { CCORD_IPC_READ_MSG, CCORD_IPC_READ_EMPTY, CCORD_IPC_READ_EOF, CCORD_IPC_READ_ERROR } ccord_ipc_read;
// Takes ownership of fd (a connected stream socket). Sets non-blocking, close-on-exec, and no-SIGPIPE.
ccord_ipc *ccord_ipc_open(ccord_socket_t fd, ccord_error *err);
void ccord_ipc_close(ccord_ipc *ipc);
ccord_socket_t ccord_ipc_fd(const ccord_ipc *ipc);                       // for ccord_sock_poll1 / poll(2)
// Non-blocking incremental read. On MSG, `msg` points into `arena` (caller resets it after use).
// Oversize frame, invalid JSON, or a bad envelope -> READ_ERROR with err->code CCORD_ERR_PROTOCOL; the caller must drop the peer.
ccord_ipc_read ccord_ipc_try_read(ccord_ipc *ipc, ccord_arena *arena, ccord_ipc_msg *msg, ccord_error *err);
// Blocks (poll) up to timeout_ms until a full message is readable. CCORD_ERR_TIMEOUT on timeout.
[[nodiscard]] ccord_status ccord_ipc_wait(ccord_ipc *ipc, ccord_arena *arena, ccord_ipc_msg *msg, int timeout_ms, ccord_error *err);
// Writers splice `params_json`/`result_json` (a complete JSON value, may be nullptr) into the envelope. Output is queued
// (cap 1 MiB; overflow -> CCORD_ERR_RANGE and the caller drops the peer) and flushed opportunistically.
[[nodiscard]] ccord_status ccord_ipc_send_request(ccord_ipc *ipc, uint32_t id, const char *method, const char *params_json, ccord_error *err);
[[nodiscard]] ccord_status ccord_ipc_send_notify(ccord_ipc *ipc, const char *method, const char *params_json, ccord_error *err);
[[nodiscard]] ccord_status ccord_ipc_send_ok(ccord_ipc *ipc, uint32_t id, const char *result_json, ccord_error *err);
[[nodiscard]] ccord_status ccord_ipc_send_error(ccord_ipc *ipc, uint32_t id, const char *code, const char *message, ccord_error *err);
[[nodiscard]] ccord_status ccord_ipc_flush(ccord_ipc *ipc, int timeout_ms, ccord_error *err);   // 0 = try once
bool ccord_ipc_has_pending_output(const ccord_ipc *ipc);
// IPC error codes (strings): "permission_denied" "invalid_argument" "limit_exceeded" "not_found" "unsupported" "timeout" "internal"
