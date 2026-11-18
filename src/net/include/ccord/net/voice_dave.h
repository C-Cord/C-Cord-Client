// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// DAVE (Discord Audio/Video E2EE) glue around the official libdave C API (<dave/dave.h>), plus the pure
// binary-frame helpers of the voice websocket. See https://github.com/discord/dave-protocol (protocol.md,
// "Voice Gateway Opcodes") and libdave's samples/typescript/DaveSessionManager.ts, which this state machine
// follows opcode by opcode.
//
// Binary websocket frames:
//   server -> client:  [u16 big-endian sequence][u8 opcode][payload]
//   client -> server:  [u8 opcode][payload]                      (no sequence number)
//   op 25 external sender package   (s->c) payload = ExternalSender        -> daveSessionSetExternalSender
//   op 26 key package               (c->s) payload = MLS KeyPackage message
//   op 27 proposals                 (s->c) payload = operation_type + proposals/refs   -> ProcessProposals
//   op 28 commit/welcome            (c->s) payload = commit [+ welcome]
//   op 29 announce commit transition(s->c) payload = [u16 transition_id][MLS commit]
//   op 30 welcome                   (s->c) payload = [u16 transition_id][MLS welcome]
// JSON ops: 11 clients_connect {user_ids[]}, 13 client_disconnect {user_id}, 21 prepare transition
// {protocol_version, transition_id}, 22 execute transition {transition_id}, 23 transition ready (c->s),
// 24 prepare epoch {protocol_version, epoch}, 31 invalid commit/welcome (c->s).
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/model/model.h>

#define CCORD_VOICE_OP_CLIENTS_CONNECT 11
#define CCORD_VOICE_OP_CLIENT_DISCONNECT 13
#define CCORD_VOICE_OP_DAVE_PREPARE_TRANSITION 21
#define CCORD_VOICE_OP_DAVE_EXECUTE_TRANSITION 22
#define CCORD_VOICE_OP_DAVE_TRANSITION_READY 23
#define CCORD_VOICE_OP_DAVE_PREPARE_EPOCH 24
#define CCORD_VOICE_OP_DAVE_EXTERNAL_SENDER 25
#define CCORD_VOICE_OP_DAVE_KEY_PACKAGE 26
#define CCORD_VOICE_OP_DAVE_PROPOSALS 27
#define CCORD_VOICE_OP_DAVE_COMMIT_WELCOME 28
#define CCORD_VOICE_OP_DAVE_ANNOUNCE_COMMIT 29
#define CCORD_VOICE_OP_DAVE_WELCOME 30
#define CCORD_VOICE_OP_DAVE_INVALID_COMMIT 31

// ---- Binary frame helpers (always available; no libdave needed) -----------------------------------------------
typedef struct ccord_vframe {
  uint16_t seq;
  uint8_t op;
  const uint8_t *payload;  // points into the parsed buffer
  size_t payload_len;
} ccord_vframe;

// Parses a server->client binary frame (needs >= 3 bytes).
[[nodiscard]] ccord_status ccord_vframe_parse(const uint8_t *buf, size_t len, ccord_vframe *out, ccord_error *err);
// Builds a client->server binary frame [op][payload]; returns bytes written, or 0 if cap is too small.
size_t ccord_vframe_build(uint8_t op, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t cap);
// Splits ops 29/30 payloads: reads the big-endian u16 transition id and returns the remaining MLS bytes.
[[nodiscard]] ccord_status ccord_vframe_split_transition(const uint8_t *payload, size_t len, uint16_t *transition_id,
                                                         const uint8_t **mls, size_t *mls_len, ccord_error *err);

// ---- DAVE state machine ----------------------------------------------------------------------------------------
typedef struct ccord_dave ccord_dave;

typedef enum ccord_dave_media : uint8_t { CCORD_DAVE_AUDIO = 0, CCORD_DAVE_VIDEO = 1 } ccord_dave_media;
typedef enum ccord_dave_codec : uint8_t { CCORD_DAVE_OPUS = 1, CCORD_DAVE_H264 = 4 } ccord_dave_codec;

typedef enum ccord_dave_result : uint8_t {
  CCORD_DAVE_OK = 0,       // `out` holds the (en|de)crypted frame
  CCORD_DAVE_NOT_READY,    // sender: no key ratchet yet (drop the frame); receiver: no key for that user yet
  CCORD_DAVE_FAILED,       // authentication/format failure (receiver should drop the frame)
} ccord_dave_result;

// Outgoing voice-websocket message produced by the state machine. `binary` messages already include the opcode byte.
typedef ccord_status (*ccord_dave_send_fn)(void *ud, bool binary, const uint8_t *data, size_t len, ccord_error *err);

// True when this build links libdave (CCORD_DAVE=ON); otherwise ccord_dave_create() fails and Identify announces
// max_dave_protocol_version 0.
bool ccord_dave_built(void);
uint16_t ccord_dave_max_protocol_version(void);  // 0 without libdave

// `group_id`: MLS group id (the voice channel id; for Go Live streams rtc_server_id - 1).
[[nodiscard]] ccord_dave *ccord_dave_create(ccord_id self_id, ccord_id group_id, ccord_dave_send_fn send, void *send_ud,
                                            ccord_error *err);
void ccord_dave_destroy(ccord_dave *d);

// Inbound protocol events (call with the parsed gateway message).
void ccord_dave_on_session_description(ccord_dave *d, uint16_t protocol_version);          // op 4
void ccord_dave_on_clients_connect(ccord_dave *d, const ccord_id *user_ids, size_t count);  // op 11
void ccord_dave_on_client_disconnect(ccord_dave *d, ccord_id user_id);                      // op 13
void ccord_dave_on_prepare_transition(ccord_dave *d, uint16_t transition_id, uint16_t protocol_version);  // op 21
void ccord_dave_on_execute_transition(ccord_dave *d, uint16_t transition_id);                // op 22
void ccord_dave_on_prepare_epoch(ccord_dave *d, uint32_t epoch, uint16_t protocol_version);  // op 24
void ccord_dave_on_external_sender(ccord_dave *d, const uint8_t *data, size_t len);         // op 25 payload
void ccord_dave_on_proposals(ccord_dave *d, const uint8_t *data, size_t len);                // op 27 payload
void ccord_dave_on_announce_commit(ccord_dave *d, uint16_t transition_id, const uint8_t *commit, size_t len);  // op 29
void ccord_dave_on_welcome(ccord_dave *d, uint16_t transition_id, const uint8_t *welcome, size_t len);        // op 30

// Media. SSRCs sent with Encrypt must first be assigned a codec with ccord_dave_assign_ssrc().
void ccord_dave_assign_ssrc(ccord_dave *d, uint32_t ssrc, ccord_dave_codec codec);
ccord_dave_result ccord_dave_encrypt(ccord_dave *d, ccord_dave_media media, uint32_t ssrc, const uint8_t *in, size_t len,
                                     uint8_t *out, size_t cap, size_t *out_len);
// Upper bound for the ciphertext of `len` plaintext bytes.
size_t ccord_dave_max_ciphertext(ccord_dave *d, ccord_dave_media media, size_t len);
ccord_dave_result ccord_dave_decrypt(ccord_dave *d, ccord_id sender, ccord_dave_media media, const uint8_t *in,
                                     size_t len, uint8_t *out, size_t cap, size_t *out_len);

uint16_t ccord_dave_protocol_version(const ccord_dave *d);  // latest prepared protocol version (0 = transport only)
bool ccord_dave_e2ee_active(const ccord_dave *d);           // our encryptor is currently producing E2EE frames
bool ccord_dave_can_send(const ccord_dave *d);              // encryptor is either E2EE-keyed or in passthrough
void ccord_dave_get_privacy_code(ccord_dave *d, char *out, size_t cap);
size_t ccord_dave_peers(const ccord_dave *d, ccord_id *out_ids, size_t cap);
