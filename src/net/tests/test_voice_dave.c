// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdlib.h>
#include <string.h>

#include <ccord/net/voice_dave.h>

#if defined(CCORD_HAVE_DAVE) && CCORD_HAVE_DAVE
#include "external_sender_wrapper.h"
#endif

#include "ccord_test.h"

typedef struct sent_msg {
  bool is_binary;
  uint8_t opcode;
  uint8_t data[4096];
  size_t len;
} sent_msg;

typedef struct peer_queue {
  sent_msg msgs[16];
  size_t count;
} peer_queue;

#if defined(CCORD_HAVE_DAVE) && CCORD_HAVE_DAVE
static ccord_status capture_send(void *ud, bool is_binary, const uint8_t *payload, size_t len, ccord_error *err)
{
  (void)err;
  peer_queue *q = ud;
  if (!q || q->count >= 16 || len > sizeof q->msgs[0].data) return CCORD_ERR_RANGE;
  sent_msg *m = &q->msgs[q->count++];
  m->is_binary = is_binary;
  m->opcode = (is_binary && len > 0 && payload) ? payload[0] : 0;
  if (!is_binary && payload && len > 0) {
    if (strstr((const char *)payload, "\"op\":23") || strstr((const char *)payload, "\"op\": 23"))
      m->opcode = CCORD_VOICE_OP_DAVE_TRANSITION_READY;
    else if (strstr((const char *)payload, "\"op\":31") || strstr((const char *)payload, "\"op\": 31"))
      m->opcode = CCORD_VOICE_OP_DAVE_INVALID_COMMIT;
  }
  if (len > 0 && payload) memcpy(m->data, payload, len);
  m->len = len;
  return CCORD_OK;
}

static sent_msg *find_sent(peer_queue *q, bool is_binary, uint8_t opcode)
{
  for (size_t i = 0; i < q->count; i++) {
    if (q->msgs[i].is_binary == is_binary && q->msgs[i].opcode == opcode) return &q->msgs[i];
  }
  return nullptr;
}
#endif

static void test_vframe_framing(void)
{
  const uint8_t body[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  uint8_t buf[32];
  size_t n = ccord_vframe_build(CCORD_VOICE_OP_DAVE_KEY_PACKAGE, body, sizeof body, buf, sizeof buf);
  CT_CHECK(n == 5u);
  CT_CHECK(buf[0] == CCORD_VOICE_OP_DAVE_KEY_PACKAGE);

  // Incoming server->client binary frame has [seq_hi][seq_lo][op][body...]
  uint8_t srv_frame[10] = {0x00, 0x2A, CCORD_VOICE_OP_DAVE_WELCOME, 0x01, 0x02, 0xAA, 0xBB, 0xCC};
  ccord_vframe vf;
  ccord_error err;
  CT_CHECK(ccord_vframe_parse(srv_frame, 8u, &vf, &err) == CCORD_OK);
  CT_CHECK(vf.seq == 42u);
  CT_CHECK(vf.op == CCORD_VOICE_OP_DAVE_WELCOME);
  CT_CHECK(vf.payload_len == 5u);

  uint16_t tid = 0;
  const uint8_t *inner = nullptr;
  size_t inner_len = 0;
  CT_CHECK(ccord_vframe_split_transition(vf.payload, vf.payload_len, &tid, &inner, &inner_len, &err) == CCORD_OK);
  CT_CHECK(tid == 0x0102u);
  CT_CHECK(inner_len == 3u && inner[0] == 0xAAu);
}

static void test_dave_two_party_e2ee(void)
{
  CT_CHECK(ccord_dave_built());
  CT_CHECK(ccord_dave_max_protocol_version() >= 1u);

#if defined(CCORD_HAVE_DAVE) && CCORD_HAVE_DAVE
  const ccord_id group_id = 3121u;
  const ccord_id alice_id = 1001u;
  const ccord_id bob_id = 1002u;
  const uint32_t alice_audio_ssrc = 11111u;
  const uint32_t alice_video_ssrc = 22222u;

  peer_queue alice_q = {0};
  peer_queue bob_q = {0};
  ccord_error err;

  ccord_dave *alice = ccord_dave_create(alice_id, group_id, capture_send, &alice_q, &err);
  ccord_dave *bob = ccord_dave_create(bob_id, group_id, capture_send, &bob_q, &err);
  CT_CHECK(alice != nullptr && bob != nullptr);
  if (!alice || !bob) return;

  ccord_dave_assign_ssrc(alice, alice_audio_ssrc, CCORD_DAVE_OPUS);
  ccord_dave_assign_ssrc(alice, alice_video_ssrc, CCORD_DAVE_H264);
  const ccord_id both_ids[2] = {alice_id, bob_id};
  ccord_dave_on_clients_connect(alice, both_ids, 2);
  ccord_dave_on_clients_connect(bob, both_ids, 2);

  uint16_t ver = ccord_dave_max_protocol_version();
  ccord_dave_on_session_description(alice, ver);
  ccord_dave_on_session_description(bob, ver);

  // Both Alice and Bob must have emitted Opcode 26 (MLS KeyPackage)
  sent_msg *alice_kp = find_sent(&alice_q, true, CCORD_VOICE_OP_DAVE_KEY_PACKAGE);
  sent_msg *bob_kp = find_sent(&bob_q, true, CCORD_VOICE_OP_DAVE_KEY_PACKAGE);
  CT_CHECK(alice_kp != nullptr && alice_kp->len > 1u);
  CT_CHECK(bob_kp != nullptr && bob_kp->len > 1u);

  // Create Voice Gateway ExternalSender for the channel
  DAVEExternalSenderHandle ext = daveExternalSenderCreate(group_id);
  CT_CHECK(ext != nullptr);
  uint8_t *ext_pkg = nullptr;
  size_t ext_pkg_len = 0;
  daveExternalSenderGetMarshalledExternalSender(ext, &ext_pkg, &ext_pkg_len);
  CT_CHECK(ext_pkg != nullptr && ext_pkg_len > 0);

  // Deliver Opcode 25 (External Sender Package) to Alice and Bob
  ccord_dave_on_external_sender(alice, ext_pkg, ext_pkg_len);
  ccord_dave_on_external_sender(bob, ext_pkg, ext_pkg_len);

  // ExternalSender proposes adding Bob (epoch 0) to Alice's pending group
  // Note: client binary frame starts with [opcode=26][key_package...]
  uint8_t *prop_bob = nullptr;
  size_t prop_bob_len = 0;
  daveExternalSenderProposeAdd(ext, 0, bob_kp->data + 1, bob_kp->len - 1, &prop_bob, &prop_bob_len);
  CT_CHECK(prop_bob != nullptr && prop_bob_len > 0);

  // Deliver Bob's proposal to Alice so she creates the MLS group and welcomes Bob!
  alice_q.count = 0;
  ccord_dave_on_proposals(alice, prop_bob, prop_bob_len);

  sent_msg *cw = find_sent(&alice_q, true, CCORD_VOICE_OP_DAVE_COMMIT_WELCOME);
  CT_CHECK(cw != nullptr && cw->len > 1u);
  if (!cw) return;

  uint8_t *commit_bytes = nullptr;
  size_t commit_len = 0;
  uint8_t *welcome_bytes = nullptr;
  size_t welcome_len = 0;
  daveExternalSenderSplitCommitWelcome(ext, cw->data + 1, cw->len - 1, &commit_bytes, &commit_len, &welcome_bytes,
                                       &welcome_len);
  CT_CHECK(commit_bytes != nullptr && commit_len > 0);
  CT_CHECK(welcome_bytes != nullptr && welcome_len > 0);

  // Server broadcasts Opcode 29 (Announce Commit, transition_id=1) to Alice, and Opcode 30 (Welcome, transition_id=1) to Bob
  alice_q.count = 0;
  bob_q.count = 0;
  ccord_dave_on_announce_commit(alice, 1u, commit_bytes, commit_len);
  ccord_dave_on_welcome(bob, 1u, welcome_bytes, welcome_len);

  // Both should emit Opcode 23 (DAVE_TRANSITION_READY) JSON for transition_id 1
  const sent_msg *alice_rdy = find_sent(&alice_q, false, CCORD_VOICE_OP_DAVE_TRANSITION_READY);
  const sent_msg *bob_rdy = find_sent(&bob_q, false, CCORD_VOICE_OP_DAVE_TRANSITION_READY);
  CT_CHECK(alice_rdy != nullptr && bob_rdy != nullptr);

  // Server sends Opcode 22 (DAVE_EXECUTE_TRANSITION)
  ccord_dave_on_execute_transition(alice, 1u);
  ccord_dave_on_execute_transition(bob, 1u);

  CT_CHECK(ccord_dave_e2ee_active(alice));
  CT_CHECK(ccord_dave_e2ee_active(bob));
  CT_CHECK(ccord_dave_can_send(alice));
  CT_CHECK(ccord_dave_can_send(bob));

  // Encrypt an Opus frame from Alice -> decrypt on Bob
  const uint8_t opus_in[16] = {0xF8, 0xFF, 0xFE, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130};
  uint8_t dave_ct[256];
  size_t dave_ct_len = 0;
  CT_CHECK(ccord_dave_encrypt(alice, CCORD_DAVE_AUDIO, alice_audio_ssrc, opus_in, sizeof opus_in, dave_ct,
                              sizeof dave_ct, &dave_ct_len) == CCORD_DAVE_OK);
  CT_CHECK(dave_ct_len > sizeof opus_in);

  uint8_t opus_out[256];
  size_t opus_out_len = 0;
  CT_CHECK(ccord_dave_decrypt(bob, alice_id, CCORD_DAVE_AUDIO, dave_ct, dave_ct_len, opus_out, sizeof opus_out,
                              &opus_out_len) == CCORD_DAVE_OK);
  CT_CHECK(opus_out_len == sizeof opus_in);
  CT_CHECK(memcmp(opus_out, opus_in, sizeof opus_in) == 0);

  // Encrypt an H.264 Annex-B video frame from Alice -> decrypt on Bob (verify NALU start codes stay unencrypted for RTP packetizer)
  const uint8_t h264_in[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E, 0xAA, 0xBB,
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0x11, 0x22, 0x33, 0x44,
  };
  uint8_t v_ct[512];
  size_t v_ct_len = 0;
  CT_CHECK(ccord_dave_encrypt(alice, CCORD_DAVE_VIDEO, alice_video_ssrc, h264_in, sizeof h264_in, v_ct, sizeof v_ct,
                              &v_ct_len) == CCORD_DAVE_OK);
  CT_CHECK(v_ct_len > sizeof h264_in);

  uint8_t v_out[512];
  size_t v_out_len = 0;
  CT_CHECK(ccord_dave_decrypt(bob, alice_id, CCORD_DAVE_VIDEO, v_ct, v_ct_len, v_out, sizeof v_out, &v_out_len) ==
           CCORD_DAVE_OK);
  CT_CHECK(v_out_len == sizeof h264_in);
  CT_CHECK(memcmp(v_out, h264_in, sizeof h264_in) == 0);

  free(ext_pkg);
  free(prop_bob);
  free(commit_bytes);
  free(welcome_bytes);
  daveExternalSenderDestroy(ext);
  ccord_dave_destroy(alice);
  ccord_dave_destroy(bob);
#endif
}

int main(void)
{
  test_vframe_framing();
  test_dave_two_party_e2ee();
  return CT_RESULT("test_voice_dave");
}
