// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Voice gateway WebSocket-only resume. A socket-free WebSocket (send
// hook + injectable clock) and a loopback UDP "SFU" bring a gateway to ESTABLISHED, then the WebSocket is dropped:
//   1. drop -> RECONNECTING, same UDP fd / secret / ssrc / speaking map, media rejected, op 7 (and no op 0 / op 1)
//      after Hello, op 9 -> ESTABLISHED + keyframe request
//   2. a camera admitted before the drop is still admitted (same generation) after the resume
//   3. close codes the policy routes elsewhere end in FAILED with the policy's plan instead of a resume
//   4. three resumes that never see op 9 escalate to REJOIN; a drop before op 4 plans REIDENTIFY
//   5. a network nudge resumes at once and is rate-limited
// The op 7 / op 9 shapes are unconfirmed against Discord's docs (see voice_gateway.c); this test pins our own.
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <ccord/net/live_transport.h>
#include <ccord/net/voice_camera.h>
#include <ccord/net/voice_crypto.h>

#include "ccord_test.h"

#define TX_SSRC 1000u

static const uint8_t k_key[CCORD_VOICE_KEY_BYTES] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
  17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

typedef struct vcap {
  char frames[64][512];
  size_t count;
} vcap;

static ccord_status vcap_send(void *ud, const char *data, size_t len, ccord_error *err)
{
  vcap *c = ud;
  ccord_error_clear(err);
  if (c->count >= 64 || len >= sizeof c->frames[0]) return ccord_error_set(err, CCORD_ERR_RANGE, "capture full");
  memcpy(c->frames[c->count], data, len);
  c->frames[c->count][len] = '\0';
  c->count++;
  return CCORD_OK;
}

static uint64_t tnow;
static uint64_t voice_now(void *ud) { return *(uint64_t *)ud; }

static int frame_op(const char *f)
{
  const char *t = strstr(f, "\"op\":");
  return t ? atoi(t + 5) : -1;
}

static size_t count_op(const vcap *c, int op)
{
  size_t n = 0;
  for (size_t i = 0; i < c->count; i++)
    if (frame_op(c->frames[i]) == op) n++;
  return n;
}

static const char *find_op(const vcap *c, int op)
{
  for (size_t i = 0; i < c->count; i++)
    if (frame_op(c->frames[i]) == op) return c->frames[i];
  return nullptr;
}

typedef struct sfu {
  int fd;
  uint16_t port;
} sfu;

static bool sfu_open(sfu *s)
{
  s->fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (s->fd < 0) return false;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof a;
  if (bind(s->fd, (struct sockaddr *)&a, sizeof a) != 0 || getsockname(s->fd, (struct sockaddr *)&a, &len) != 0) {
    close(s->fd);
    s->fd = -1;
    return false;
  }
  s->port = ntohs(a.sin_port);
  return true;
}

static bool sfu_answer_discovery(sfu *s)
{
  struct pollfd pfd = {.fd = s->fd, .events = POLLIN, .revents = 0};
  if (poll(&pfd, 1, 2000) <= 0) return false;
  uint8_t req[128];
  struct sockaddr_in from;
  socklen_t flen = sizeof from;
  ssize_t n = recvfrom(s->fd, req, sizeof req, 0, (struct sockaddr *)&from, &flen);
  if (n != (ssize_t)CCORD_VOICE_IP_DISCOVERY_SIZE || req[0] != 0x00 || req[1] != 0x01) return false;
  uint8_t resp[CCORD_VOICE_IP_DISCOVERY_SIZE];
  memset(resp, 0, sizeof resp);
  resp[1] = 0x02;
  resp[3] = 0x46;
  memcpy(resp + 4, req + 4, 4);
  memcpy(resp + 8, "127.0.0.1", 9);
  uint16_t gport = ntohs(from.sin_port);
  resp[72] = (uint8_t)(gport >> 8);
  resp[73] = (uint8_t)(gport & 0xFFu);
  return sendto(s->fd, resp, sizeof resp, 0, (struct sockaddr *)&from, flen) == (ssize_t)sizeof resp;
}

static void gw_poll(ccord_voice_gateway *g)
{
  ccord_voice_event ev;
  (void)ccord_voice_gateway_poll(g, &ev);
}

static ccord_status gw_handle(ccord_voice_gateway *g, const char *json)
{
  ccord_voice_event ev;
  ccord_error err;
  return ccord_voice_gateway_handle(g, json, strlen(json), &ev, &err);
}

static ccord_voice_gateway *gw_create(vcap *cap)
{
  memset(cap, 0, sizeof *cap);
  tnow = 1000;
  ccord_voice_gateway_config cfg = {
    .server_id = 77,
    .channel_id = 88,
    .user_id = 42,
    .session_id = "voice-session",
    .token = "REDACTED_TEST_TOKEN",
    .endpoint = "voice.example",
    .connect_timeout_ms = 100,
    .now_ms = voice_now,
    .clock_ud = &tnow,
    .send = vcap_send,
    .send_ud = cap,
  };
  ccord_error err;
  ccord_voice_gateway *g = ccord_voice_gateway_create(&cfg, &err);
  if (!g) return nullptr;
  if (ccord_voice_gateway_start(g, &err) != CCORD_OK) {
    ccord_voice_gateway_destroy(g);
    return nullptr;
  }
  if (gw_handle(g, "{\"op\":8,\"d\":{\"heartbeat_interval\":1000}}") != CCORD_OK) {
    ccord_voice_gateway_destroy(g);
    return nullptr;
  }
  return g;
}

// Brings a fresh gateway to ESTABLISHED (xchacha mode, DAVE passthrough) with camera receive on.
static ccord_voice_gateway *bring_up(vcap *cap, sfu *s)
{
  memset(s, 0, sizeof *s);
  s->fd = -1;
  ccord_voice_gateway *g = gw_create(cap);
  if (!g) return nullptr;
  ccord_voice_gateway_set_camera_rx(g, true);
  if (!sfu_open(s)) goto fail;
  char ready[256];
  snprintf(ready, sizeof ready,
           "{\"op\":2,\"d\":{\"ssrc\":%u,\"ip\":\"127.0.0.1\",\"port\":%u,"
           "\"modes\":[\"aead_xchacha20_poly1305_rtpsize\"]}}",
           TX_SSRC, (unsigned)s->port);
  if (gw_handle(g, ready) != CCORD_OK) goto fail;
  if (!sfu_answer_discovery(s)) goto fail;
  tnow += 300;
  gw_poll(g);  // consumes the discovery response -> SELECT_PROTOCOL
  char key_json[256];
  size_t off = 0;
  off += (size_t)snprintf(key_json + off, sizeof key_json - off,
                          "{\"op\":4,\"d\":{\"mode\":\"aead_xchacha20_poly1305_rtpsize\","
                          "\"dave_protocol_version\":0,\"secret_key\":[");
  for (unsigned i = 0; i < CCORD_VOICE_KEY_BYTES; i++) {
    off += (size_t)snprintf(key_json + off, sizeof key_json - off, "%s%u", i ? "," : "", (unsigned)k_key[i]);
    if (off >= sizeof key_json) off = sizeof key_json - 1;
  }
  snprintf(key_json + off, sizeof key_json - off, "]}}");
  if (gw_handle(g, key_json) != CCORD_OK) goto fail;
  if (ccord_voice_gateway_state(g) != CCORD_VOICE_ESTABLISHED) goto fail;
  return g;
fail:
  ccord_voice_gateway_destroy(g);
  if (s->fd >= 0) close(s->fd);
  s->fd = -1;
  return nullptr;
}

static void sfu_close(sfu *s)
{
  if (s->fd >= 0) close(s->fd);
  s->fd = -1;
}

static void test_resume_keeps_session(void)
{
  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s);
  CT_CHECK(g != nullptr);
  if (!g) return;

  // Session state that must survive: speaking map, a seq_ack, an admitted camera.
  CT_CHECK(gw_handle(g, "{\"op\":5,\"seq\":17,\"d\":{\"speaking\":1,\"ssrc\":7777,\"user_id\":\"7\"}}") == CCORD_OK);
  CT_CHECK(ccord_voice_gateway_user_speaking(g, 7));
  CT_CHECK(gw_handle(g, "{\"op\":12,\"d\":{\"user_id\":\"9\",\"audio_ssrc\":1009,\"video_ssrc\":9000,"
                        "\"rtx_ssrc\":10000}}") == CCORD_OK);
  ccord_voice_cam_info cams[4];
  CT_CHECK(ccord_voice_gateway_cameras(g, cams, 4) == 1);
  CT_CHECK(cams[0].decoding);
  uint32_t cam_gen = cams[0].generation;
  CT_CHECK(cam_gen != 0);

  int fd = ccord_voice_gateway_udp_fd(g);
  uint32_t ssrc = ccord_voice_gateway_ssrc(g);
  uint8_t key_before[CCORD_VOICE_MAX_SECRET], key_after[CCORD_VOICE_MAX_SECRET];
  size_t klen = ccord_voice_gateway_secret(g, key_before);
  CT_CHECK(fd >= 0 && klen == CCORD_VOICE_KEY_BYTES);
  CT_CHECK(ccord_voice_gateway_consume_keyframe_request(g));
  CT_CHECK(!ccord_voice_gateway_consume_keyframe_request(g));
  cap.count = 0;

  // 1. the WebSocket drops
  tnow += 500;
  ccord_voice_gateway_inject_close(g, 0);
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_RECONNECTING);
  CT_CHECK(ccord_voice_gateway_udp_fd(g) == fd);
  CT_CHECK(ccord_voice_gateway_ssrc(g) == ssrc);
  CT_CHECK(ccord_voice_gateway_secret(g, key_after) == klen && memcmp(key_before, key_after, klen) == 0);
  CT_CHECK(ccord_voice_gateway_user_speaking(g, 7));
  ccord_voice_failure f;
  CT_CHECK(!ccord_voice_gateway_failure(g, &f));
  int16_t pcm[960 * 2] = {0};
  ccord_error err;
  CT_CHECK(ccord_voice_gateway_push_pcm(g, pcm, 960, &err) != CCORD_OK);  // media rejected while resuming
  CT_CHECK(ccord_voice_gateway_cameras(g, cams, 4) == 1 && cams[0].decoding);

  // Before the 250 ms backoff nothing happens; afterwards the poll re-opens the socket, still without op 7.
  tnow += 100;
  gw_poll(g);
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_RECONNECTING);
  CT_CHECK(cap.count == 0);
  tnow += 200;
  gw_poll(g);
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_RECONNECTING);
  CT_CHECK(count_op(&cap, 7) == 0);

  // 3. Hello on the new socket -> exactly one op 7, no identify, no select protocol
  CT_CHECK(gw_handle(g, "{\"op\":8,\"d\":{\"heartbeat_interval\":1000}}") == CCORD_OK);
  CT_CHECK(count_op(&cap, 7) == 1);
  CT_CHECK(count_op(&cap, 0) == 0);
  CT_CHECK(count_op(&cap, 1) == 0);
  const char *op7 = find_op(&cap, 7);
  CT_CHECK(op7 && strstr(op7, "\"server_id\":\"77\"") && strstr(op7, "\"session_id\":\"voice-session\"") &&
           strstr(op7, "\"token\":\"REDACTED_TEST_TOKEN\"") && strstr(op7, "\"seq_ack\":17"));
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_RECONNECTING);

  // 4. op 9 -> ESTABLISHED, same socket / key / speaking map / camera, and a keyframe is requested
  CT_CHECK(gw_handle(g, "{\"op\":9,\"d\":null}") == CCORD_OK);
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_ESTABLISHED);
  CT_CHECK(ccord_voice_gateway_udp_fd(g) == fd);
  CT_CHECK(ccord_voice_gateway_ssrc(g) == ssrc);
  CT_CHECK(ccord_voice_gateway_secret(g, key_after) == klen && memcmp(key_before, key_after, klen) == 0);
  CT_CHECK(ccord_voice_gateway_user_speaking(g, 7));
  CT_CHECK(ccord_voice_gateway_consume_keyframe_request(g));
  CT_CHECK(ccord_voice_gateway_push_pcm(g, pcm, 960, &err) == CCORD_OK);
  CT_CHECK(count_op(&cap, 0) == 0 && count_op(&cap, 1) == 0);

  // 2. the camera admitted before the drop is still admitted, with the same identity
  CT_CHECK(ccord_voice_gateway_cameras(g, cams, 4) == 1);
  CT_CHECK(cams[0].user_id == 9 && cams[0].video_ssrc == 9000 && cams[0].decoding && cams[0].generation == cam_gen);

  // Even a long idle spell right after the resume does not demote it (the outage was not the sender's silence).
  tnow += 2000;
  gw_poll(g);
  CT_CHECK(ccord_voice_gateway_cameras(g, cams, 4) == 1 && cams[0].decoding);

  ccord_voice_gateway_destroy(g);
  sfu_close(&s);
}

static void expect_failure(int close_code, ccord_vc_action action, const char *reason)
{
  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s);
  CT_CHECK(g != nullptr);
  if (!g) return;
  ccord_voice_gateway_inject_close(g, (uint16_t)close_code);
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_FAILED);
  ccord_voice_failure f;
  CT_CHECK(ccord_voice_gateway_failure(g, &f));
  CT_CHECK(f.close_code == close_code && f.was_established);
  CT_CHECK(f.plan.action == action);
  if (reason) CT_CHECK(strcmp(f.plan.reason, reason) == 0);
  CT_CHECK(ccord_voice_gateway_udp_fd(g) == -1);  // a new voice session needs a new UDP socket
  ccord_voice_gateway_destroy(g);
  sfu_close(&s);
}

static void test_refused_resume_plans(void)
{
  expect_failure(4006, CCORD_VC_REJOIN, nullptr);
  expect_failure(4009, CCORD_VC_REJOIN, nullptr);
  expect_failure(4003, CCORD_VC_REIDENTIFY, nullptr);
  expect_failure(4005, CCORD_VC_REIDENTIFY, nullptr);
  expect_failure(4014, CCORD_VC_AWAIT_MAIN, nullptr);
  expect_failure(4022, CCORD_VC_AWAIT_MAIN, nullptr);
  expect_failure(4021, CCORD_VC_FINAL, "rate limited");
  // 4015 "try resuming" and an ordinary close stay inside the gateway.
  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s);
  CT_CHECK(g != nullptr);
  if (!g) return;
  ccord_voice_gateway_inject_close(g, 4015);
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_RECONNECTING);
  ccord_voice_gateway_destroy(g);
  sfu_close(&s);
}

static void test_resume_without_answer_escalates(void)
{
  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s);
  CT_CHECK(g != nullptr);
  if (!g) return;
  ccord_voice_gateway_inject_close(g, 0);
  for (int i = 0; i < 3; i++) {
    CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_RECONNECTING);
    tnow += 5000;
    gw_poll(g);              // backoff elapsed: the socket is re-opened
    tnow += 3500;
    gw_poll(g);              // Hello / op 9 never arrived within CCORD_VC_CONNECT_TIMEOUT_MS
  }
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_FAILED);
  ccord_voice_failure f;
  CT_CHECK(ccord_voice_gateway_failure(g, &f));
  CT_CHECK(f.plan.action == CCORD_VC_REJOIN && f.plan.attempt == 4 && f.state.attempt == 4 && f.was_established);
  CT_CHECK(ccord_voice_gateway_udp_fd(g) == -1);
  ccord_voice_gateway_destroy(g);
  sfu_close(&s);

  // A drop before op 4 has nothing to resume: REIDENTIFY, straight to FAILED.
  g = gw_create(&cap);
  CT_CHECK(g != nullptr);
  if (!g) return;
  ccord_voice_gateway_inject_close(g, 0);
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_FAILED);
  CT_CHECK(ccord_voice_gateway_failure(g, &f));
  CT_CHECK(f.plan.action == CCORD_VC_REIDENTIFY && !f.was_established);
  ccord_voice_gateway_destroy(g);
}

static void test_network_nudge(void)
{
  vcap cap;
  sfu s;
  ccord_voice_gateway *g = bring_up(&cap, &s);
  CT_CHECK(g != nullptr);
  if (!g) return;
  cap.count = 0;
  ccord_voice_gateway_network_event(g);  // no backoff: the next poll re-opens
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_RECONNECTING);
  gw_poll(g);
  CT_CHECK(gw_handle(g, "{\"op\":8,\"d\":{\"heartbeat_interval\":1000}}") == CCORD_OK);
  CT_CHECK(count_op(&cap, 7) == 1);
  CT_CHECK(gw_handle(g, "{\"op\":9,\"d\":null}") == CCORD_OK);
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_ESTABLISHED);
  tnow += 1000;
  ccord_voice_gateway_network_event(g);  // within CCORD_VC_EVENT_MIN_GAP_MS of the last one: ignored
  CT_CHECK(ccord_voice_gateway_state(g) == CCORD_VOICE_ESTABLISHED);
  ccord_voice_gateway_destroy(g);
  sfu_close(&s);
}

static void test_build_resume(void)
{
  ccord_buf b;
  ccord_buf_init(&b, 256);
  ccord_error err;
  CT_CHECK(ccord_voice_build_resume(77, "sess", "tok", 5, &b, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b),
                  "{\"op\":7,\"d\":{\"server_id\":\"77\",\"session_id\":\"sess\",\"token\":\"tok\",\"seq_ack\":5}}") == 0);
  ccord_buf_free(&b);
  ccord_buf_init(&b, 256);
  CT_CHECK(ccord_voice_build_resume(CCORD_ID_NONE, "sess", "tok", 5, &b, &err) != CCORD_OK);
  ccord_buf_free(&b);
}

int main(void)
{
  test_build_resume();
  test_resume_keeps_session();
  test_refused_resume_plans();
  test_resume_without_answer_escalates();
  test_network_nudge();
  return CT_RESULT("test_voice_resume");
}
