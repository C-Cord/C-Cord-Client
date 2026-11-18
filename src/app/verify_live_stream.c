// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// End-to-end headless live streaming verification (`ccord --headless --verify-slice live-stream`).
#include "verify_live_stream.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <ccord/net/live_transport.h>
#include <ccord/net/rtp_h264.h>
#include <ccord/net/voice_crypto.h>
#include <ccord/net/voice_stream.h>

#define LS_GUILD CCORD_VERIFY_LS_GUILD
#define LS_VCHAN CCORD_VERIFY_LS_VCHAN
#define LS_DM_CHAN CCORD_VERIFY_LS_DM_CHAN
#define LS_SELF CCORD_VERIFY_LS_SELF
#define LS_VOICE_SSRC CCORD_VERIFY_LS_VOICE_SSRC
#define LS_STREAM_SSRC CCORD_VERIFY_LS_STREAM_SSRC

const uint8_t ccord_verify_ls_key[CCORD_VOICE_KEY_BYTES] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
  17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

static void ls_sleep_ms(unsigned ms)
{
  struct timespec ts = {.tv_sec = (time_t)(ms / 1000u), .tv_nsec = (long)(ms % 1000u) * 1000000L};
  nanosleep(&ts, nullptr);
}

static size_t respond_http(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)ud;
  (void)idx;
  if (req && strstr(req->path, "/messages") != nullptr) {
    return fd_reply(out, cap, 200, "OK", nullptr, "[]");
  }
  if (req && strstr(req->path, "/profile") != nullptr) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"user\":{\"id\":\"42\",\"username\":\"me\"}}");
  }
  return fd_reply(out, cap, 204, "No Content", nullptr, "");
}

typedef struct ls_vg_ud {
  int voice_udp_to_close;
  int stream_udp_to_close;
  uint16_t udp_port;
  bool is_stream;
} ls_vg_ud;

static void script_voice_gw(void *ud, fg_conn *c)
{
  ls_vg_ud *v = ud;
  if (v->voice_udp_to_close >= 0) {
    close(v->voice_udp_to_close);
    v->voice_udp_to_close = -1;
  }
  if (v->stream_udp_to_close >= 0) {
    close(v->stream_udp_to_close);
    v->stream_udp_to_close = -1;
  }
  if (!fg_handshake(c)) return;
  if (!fg_send(c, "{\"op\":8,\"d\":{\"heartbeat_interval\":60000}}")) return;

  char buf[4096];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) return;
  bool is_stream = v->is_stream || strstr(buf, "\"streams\"") != nullptr;
  fg_log(c, is_stream ? "STREAM_OP0 %s" : "VOICE_OP0 %s", buf);

  char ready[256];
  (void)snprintf(ready, sizeof ready,
                 "{\"op\":2,\"d\":{\"ssrc\":%u,\"ip\":\"127.0.0.1\",\"port\":%u,"
                 "\"modes\":[\"aead_xchacha20_poly1305_rtpsize\"]}}",
                 is_stream ? LS_STREAM_SSRC : LS_VOICE_SSRC, (unsigned)v->udp_port);
  if (!fg_send(c, ready)) return;

  for (;;) {
    int r = fg_recv(c, buf, sizeof buf, 5000);
    if (r == FG_TIMEOUT) continue;
    if (r != FG_GOT_TEXT) break;
    if (strstr(buf, "\"op\":3") != nullptr) {
      (void)fg_send(c, "{\"op\":6,\"d\":{\"t\":0}}");
      continue;
    }
    if (strstr(buf, "\"op\":12") != nullptr) {
      bool active = strstr(buf, "\"active\":true") != nullptr;
      bool res = strstr(buf, "\"max_resolution\"") != nullptr && strstr(buf, "\"type\":\"fixed\"") != nullptr;
      bool fps = strstr(buf, "\"max_framerate\":30") != nullptr;
      bool br = strstr(buf, "\"max_bitrate\"") != nullptr;
      bool w720 = strstr(buf, "\"width\":1280") != nullptr;
      bool h720 = strstr(buf, "\"height\":720") != nullptr;
      const char *brv = strstr(buf, "\"max_bitrate\":");
      unsigned bitrate = brv ? (unsigned)strtoul(brv + 14, nullptr, 10) : 0u;
      fg_log(c, "%s active=%d res=%d fps=%d br=%d w=%d h=%d bitrate=%u", is_stream ? "STREAM_OP12" : "VOICE_OP12",
             active ? 1 : 0, res ? 1 : 0, fps ? 1 : 0, br ? 1 : 0, w720 ? 1 : 0, h720 ? 1 : 0, bitrate);
      continue;
    }
    if (strstr(buf, "\"op\":1,") != nullptr || strstr(buf, "\"op\":1}") != nullptr) {
      bool has_h264 = strstr(buf, "\"name\":\"H264\"") != nullptr;
      // The opus codec entry is the only one carrying payload_type 120 (H264 uses 101/102).
      bool opus120 = strstr(buf, "\"name\":\"opus\"") != nullptr && strstr(buf, "\"payload_type\":120") != nullptr;
      fg_log(c, "%s%s%s", is_stream ? "STREAM_OP1" : "VOICE_OP1", has_h264 ? "_H264" : "",
             opus120 ? "_OPUS120" : "");
      const char *sdesc =
        "{\"op\":4,\"d\":{\"mode\":\"aead_xchacha20_poly1305_rtpsize\",\"dave_protocol_version\":0,"
        "\"secret_key\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32]}}";
      if (!fg_send(c, sdesc)) break;
      continue;
    }
    if (strstr(buf, "\"op\":5") != nullptr) {
      const char *sp = strstr(buf, "\"speaking\":");
      fg_log(c, "%s_SPEAKING %d", is_stream ? "STREAM" : "VOICE", sp ? atoi(sp + 11) : -1);
      continue;
    }
    fg_log(c, "VG_OTHER %s", buf);
  }
}

typedef struct ls_mg_ud {
  int voice_udp_to_close;
  int stream_udp_to_close;
  uint16_t vg_port;
  uint16_t sg_port;
} ls_mg_ud;

static void script_main_gw(void *ud, fg_conn *c)
{
  ls_mg_ud *m = ud;
  if (m->voice_udp_to_close >= 0) {
    close(m->voice_udp_to_close);
    m->voice_udp_to_close = -1;
  }
  if (m->stream_udp_to_close >= 0) {
    close(m->stream_udp_to_close);
    m->stream_udp_to_close = -1;
  }
  if (!fg_handshake(c)) return;
  fg_log(c, "HANDSHAKE");
  if (!fg_send(c, "{\"op\":10,\"d\":{\"heartbeat_interval\":60000}}")) return;

  char buf[4096];
  if (fg_recv(c, buf, sizeof buf, 5000) != FG_GOT_TEXT) return;
  if (strstr(buf, "\"op\":2") == nullptr) return;
  fg_log(c, "IDENTIFY");

  char ready[2048];
  (void)snprintf(
    ready, sizeof ready,
    "{\"op\":0,\"t\":\"READY\",\"s\":1,\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
    "\"session_id\":\"sess-1\",\"resume_gateway_url\":\"%s\","
    "\"guilds\":[{\"id\":\"2001\",\"name\":\"Verify HQ\",\"channels\":[{\"id\":\"3121\",\"type\":2,"
    "\"name\":\"General\",\"guild_id\":\"2001\"}],\"members\":[{\"user\":{\"id\":\"42\",\"username\":\"me\"}}]}],"
    "\"private_channels\":[{\"id\":\"4444\",\"type\":1,\"recipients\":[{\"id\":\"1002\",\"username\":\"ada\"}]}]}}",
    c->self_url);
  if (!fg_send(c, ready)) return;

  int seq = 2;
  bool in_stream = false;
  bool in_voice = false;
  bool is_dm = false;
  char cur_stream_key[96] = "";
  for (;;) {
    int r = fg_recv(c, buf, sizeof buf, 100);
    if (r == FG_TIMEOUT) continue;
    if (r != FG_GOT_TEXT) break;
    if (strstr(buf, "\"op\":1,") != nullptr || strstr(buf, "\"op\":1}") != nullptr) {
      if (!fg_send(c, "{\"op\":11}")) break;
      continue;
    }
    if (strstr(buf, "\"op\":4") != nullptr) {
      bool self_stream = strstr(buf, "\"self_stream\":true") != nullptr;
      bool chan_null = strstr(buf, "\"channel_id\":null") != nullptr;
      bool dm = strstr(buf, "\"guild_id\":null") != nullptr;
      const char *gid_json = dm ? "null" : "\"2001\"";
      const char *cid_json = chan_null ? "null" : (dm ? "\"4444\"" : "\"3121\"");
      char ev[1024];

      if (self_stream) {
        in_stream = true;
        fg_log(c, "OP4_STREAM_ON %s", buf);
        (void)snprintf(ev, sizeof ev,
                       "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":%s,"
                       "\"channel_id\":%s,\"user_id\":\"42\",\"session_id\":\"vs-1\",\"self_stream\":true}}",
                       seq++, gid_json, cid_json);
        if (!fg_send(c, ev)) break;
      } else if (chan_null) {
        in_stream = false;
        in_voice = false;
        fg_log(c, "OP4_VOICE_LEAVE %s", buf);
        (void)snprintf(ev, sizeof ev,
                       "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":%s,"
                       "\"channel_id\":null,\"user_id\":\"42\",\"session_id\":\"vs-1\",\"self_stream\":false}}",
                       seq++, gid_json);
        if (!fg_send(c, ev)) break;
      } else if (in_stream) {
        in_stream = false;
        fg_log(c, "OP4_STREAM_OFF %s", buf);
        (void)snprintf(ev, sizeof ev,
                       "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":%s,"
                       "\"channel_id\":%s,\"user_id\":\"42\",\"session_id\":\"vs-1\",\"self_stream\":false}}",
                       seq++, gid_json, cid_json);
        if (!fg_send(c, ev)) break;
      } else if (in_voice) {
        // Camera toggle while already in voice: echo the new self_video flag only. No
        // VOICE_SERVER_UPDATE, so the client keeps its established voice connection.
        bool cam_on = strstr(buf, "\"self_video\":true") != nullptr;
        fg_log(c, cam_on ? "OP4_CAMERA_ON %s" : "OP4_CAMERA_OFF %s", buf);
        (void)snprintf(ev, sizeof ev,
                       "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":%s,"
                       "\"channel_id\":%s,\"user_id\":\"42\",\"session_id\":\"vs-1\",\"self_video\":%s,"
                       "\"self_stream\":false}}",
                       seq++, gid_json, cid_json, cam_on ? "true" : "false");
        if (!fg_send(c, ev)) break;
      } else {
        is_dm = dm;
        in_voice = true;
        fg_log(c, "OP4_VOICE_JOIN %s", buf);
        (void)snprintf(ev, sizeof ev,
                       "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":%s,"
                       "\"channel_id\":%s,\"user_id\":\"42\",\"session_id\":\"vs-1\",\"self_stream\":false}}",
                       seq++, gid_json, cid_json);
        if (!fg_send(c, ev)) break;
        if (is_dm) {
          (void)snprintf(ev, sizeof ev,
                         "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"s\":%d,\"d\":{\"channel_id\":\"4444\","
                         "\"token\":\"vtok\",\"endpoint\":\"ws://127.0.0.1:%u\"}}",
                         seq++, (unsigned)m->vg_port);
        } else {
          (void)snprintf(ev, sizeof ev,
                         "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":\"2001\","
                         "\"token\":\"vtok\",\"endpoint\":\"ws://127.0.0.1:%u\"}}",
                         seq++, (unsigned)m->vg_port);
        }
        if (!fg_send(c, ev)) break;
      }
      continue;
    }
    if (strstr(buf, "\"op\":18") != nullptr) {
      fg_log(c, "OP18_CREATE %s", buf);
      bool call_stream = strstr(buf, "\"type\":\"call\"") != nullptr;
      const char *rtc_srv = call_stream ? "9002" : "9001";
      const char *rtc_ch = call_stream ? "4444" : "3121";
      (void)snprintf(cur_stream_key, sizeof cur_stream_key, "%s",
                     call_stream ? "call:4444:42" : "guild:2001:3121:42");
      char ev[1024];
      (void)snprintf(ev, sizeof ev,
                     "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":%d,\"d\":{\"stream_key\":\"%s\","
                     "\"rtc_server_id\":\"%s\",\"rtc_channel_id\":\"%s\",\"region\":\"us-east\","
                     "\"viewer_ids\":[]}}",
                     seq++, cur_stream_key, rtc_srv, rtc_ch);
      if (!fg_send(c, ev)) break;
      (void)snprintf(ev, sizeof ev,
                     "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":%d,\"d\":{\"stream_key\":\"%s\","
                     "\"token\":\"stok\",\"endpoint\":\"ws://127.0.0.1:%u\"}}",
                     seq++, cur_stream_key, (unsigned)m->sg_port);
      if (!fg_send(c, ev)) break;
      continue;
    }
    if (strstr(buf, "\"op\":21") != nullptr) {
      fg_log(c, "OP21_PING %s", buf);
      continue;
    }
    if (strstr(buf, "\"op\":22") != nullptr) {
      bool paused = strstr(buf, "\"paused\":true") != nullptr;
      fg_log(c, paused ? "OP22_PAUSE %s" : "OP22_RESUME %s", buf);
      char ev[512];
      (void)snprintf(ev, sizeof ev,
                     "{\"op\":0,\"t\":\"STREAM_UPDATE\",\"s\":%d,\"d\":{\"stream_key\":\"%s\",\"paused\":%s}}",
                     seq++, cur_stream_key, paused ? "true" : "false");
      if (!fg_send(c, ev)) break;
      continue;
    }
    if (strstr(buf, "\"op\":19") != nullptr) {
      fg_log(c, "OP19_DELETE %s", buf);
      char ev[512];
      (void)snprintf(ev, sizeof ev,
                     "{\"op\":0,\"t\":\"STREAM_DELETE\",\"s\":%d,\"d\":{\"stream_key\":\"%s\"}}",
                     seq++, cur_stream_key);
      if (!fg_send(c, ev)) break;
      continue;
    }
    fg_log(c, "GW_OTHER %s", buf);
  }
}

static bool open_loopback_udp(int *out_fd, uint16_t *out_port)
{
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  struct sockaddr_in sin = {0};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t slen = sizeof sin;
  if (bind(fd, (struct sockaddr *)&sin, sizeof sin) != 0 || getsockname(fd, (struct sockaddr *)&sin, &slen) != 0) {
    close(fd);
    return false;
  }
  *out_fd = fd;
  *out_port = ntohs(sin.sin_port);
  return true;
}

// Drains the socket, decrypting every H.264 RTP packet; returns how many opened cleanly and sets
// *out_idr when a complete IDR access unit depacketized.
static size_t drain_udp_h264(int fd, ccord_rtp_h264_depack *depack, bool *out_idr)
{
  size_t got = 0;
  uint8_t pkt[2048];
  for (;;) {
    ssize_t n = recvfrom(fd, pkt, sizeof pkt, 0, nullptr, nullptr);
    if (n <= 0) break;
    if ((size_t)n < 28u) continue;
    uint8_t pt = pkt[1] & 0x7Fu;
    if (pt != CCORD_VSTREAM_H264_PT_DEFAULT) continue;
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    if (ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, ccord_verify_ls_key, pkt, (size_t)n, plain, sizeof plain,
                         &plain_len, &view, nullptr) != CCORD_OK) {
      continue;
    }
    got++;
    const uint8_t *au = nullptr;
    size_t au_len = 0;
    bool key = false;
    uint32_t ts = 0;
    if (ccord_rtp_h264_depack_push(depack, plain, plain_len, view.seq, view.timestamp, view.marker, &au, &au_len, &key,
                                   &ts)) {
      if (key && out_idr) *out_idr = true;
    }
  }
  return got;
}

static bool drain_udp_for_h264_idr(int fd, ccord_rtp_h264_depack *depack)
{
  bool saw_idr = false;
  (void)drain_udp_h264(fd, depack, &saw_idr);
  return saw_idr;
}

// Synthetic H.264 access unit (Annex-B, 4-byte start codes); the IDR payload is large enough to
// force FU-A fragmentation. Same shape as src/net/tests/test_voice_loopback.c.
static size_t build_au(uint8_t *out, bool keyframe, size_t idr_bytes)
{
  size_t n = 0;
  static const uint8_t sc[4] = {0, 0, 0, 1};
  if (keyframe) {
    memcpy(out + n, sc, 4); n += 4;
    out[n++] = 0x67;   // SPS (NAL 7)
    for (size_t i = 0; i < 8; i++) out[n++] = (uint8_t)(0xA0 + i);
    memcpy(out + n, sc, 4); n += 4;
    out[n++] = 0x68;   // PPS (NAL 8)
    for (size_t i = 0; i < 4; i++) out[n++] = (uint8_t)(0xB0 + i);
  }
  memcpy(out + n, sc, 4); n += 4;
  out[n++] = keyframe ? 0x65 : 0x41;   // IDR (NAL 5) or non-IDR slice (NAL 1)
  for (size_t i = 0; i < idr_bytes; i++) out[n++] = (uint8_t)(i * 7u + 3u);
  return n;
}

static int ls_fail(const char *what)
{
  fprintf(stderr, "ccord: --verify-slice live-stream: FAILED %s\n", what);
  return 1;
}

static bool has_prefix_substr(const fg_event *ev, size_t n, const char *prefix, const char *sub)
{
  size_t plen = strlen(prefix);
  for (size_t i = 0; i < n; i++) {
    if (strncmp(ev[i].text, prefix, plen) == 0 && (!sub || strstr(ev[i].text, sub) != nullptr)) return true;
  }
  return false;
}

bool ccord_verify_ls_loopback_start(ccord_verify_ls_loopback *lb, const char **fail_reason)
{
  if (!lb) {
    if (fail_reason) *fail_reason = "invalid loopback harness";
    return false;
  }
  memset(lb, 0, sizeof *lb);
  lb->voice_udp = -1;
  lb->stream_udp = -1;
  if (!open_loopback_udp(&lb->voice_udp, &lb->voice_port) || !open_loopback_udp(&lb->stream_udp, &lb->stream_port)) {
    if (lb->voice_udp >= 0) close(lb->voice_udp);
    if (lb->stream_udp >= 0) close(lb->stream_udp);
    lb->voice_udp = lb->stream_udp = -1;
    if (fail_reason) *fail_reason = "open loopback UDP sockets";
    return false;
  }

  // SDL installs a SIGTERM handler (quit event); the forked fake servers would inherit it and
  // survive fg_stop/fd_stop's SIGTERM until their alarm(40) fires, stalling cleanup ~40 s. Fork
  // them with the default disposition, then restore the app's handler.
  struct sigaction ls_sa_dfl, ls_sa_old;
  memset(&ls_sa_dfl, 0, sizeof ls_sa_dfl);
  ls_sa_dfl.sa_handler = SIG_DFL;
  sigemptyset(&ls_sa_dfl.sa_mask);
  (void)sigaction(SIGTERM, &ls_sa_dfl, &ls_sa_old);

  if (!fd_start(&lb->http, respond_http, nullptr)) {
    (void)sigaction(SIGTERM, &ls_sa_old, nullptr);
    close(lb->voice_udp);
    close(lb->stream_udp);
    lb->voice_udp = lb->stream_udp = -1;
    if (fail_reason) *fail_reason = "start fake HTTP server";
    return false;
  }

  ls_vg_ud vg_ud = {
    .voice_udp_to_close = lb->voice_udp,
    .stream_udp_to_close = lb->stream_udp,
    .udp_port = lb->voice_port,
    .is_stream = false,
  };
  if (!fg_start(&lb->voice_gw, script_voice_gw, &vg_ud)) {
    fd_stop(&lb->http, nullptr, 0);
    (void)sigaction(SIGTERM, &ls_sa_old, nullptr);
    close(lb->voice_udp);
    close(lb->stream_udp);
    lb->voice_udp = lb->stream_udp = -1;
    if (fail_reason) *fail_reason = "start fake voice gateway";
    return false;
  }

  ls_vg_ud sg_ud = {
    .voice_udp_to_close = lb->voice_udp,
    .stream_udp_to_close = lb->stream_udp,
    .udp_port = lb->stream_port,
    .is_stream = true,
  };
  if (!fg_start(&lb->stream_gw, script_voice_gw, &sg_ud)) {
    fg_stop(&lb->voice_gw, nullptr, 0);
    fd_stop(&lb->http, nullptr, 0);
    (void)sigaction(SIGTERM, &ls_sa_old, nullptr);
    close(lb->voice_udp);
    close(lb->stream_udp);
    lb->voice_udp = lb->stream_udp = -1;
    if (fail_reason) *fail_reason = "start fake stream gateway";
    return false;
  }

  ls_mg_ud mg_ud = {
    .voice_udp_to_close = lb->voice_udp,
    .stream_udp_to_close = lb->stream_udp,
    .vg_port = (uint16_t)lb->voice_gw.port,
    .sg_port = (uint16_t)lb->stream_gw.port,
  };
  if (!fg_start(&lb->main_gw, script_main_gw, &mg_ud)) {
    fg_stop(&lb->stream_gw, nullptr, 0);
    fg_stop(&lb->voice_gw, nullptr, 0);
    fd_stop(&lb->http, nullptr, 0);
    (void)sigaction(SIGTERM, &ls_sa_old, nullptr);
    close(lb->voice_udp);
    close(lb->stream_udp);
    lb->voice_udp = lb->stream_udp = -1;
    if (fail_reason) *fail_reason = "start fake main gateway";
    return false;
  }
  (void)sigaction(SIGTERM, &ls_sa_old, nullptr);
  return true;
}

void ccord_verify_ls_loopback_stop(ccord_verify_ls_loopback *lb, fg_event *gw_ev, size_t *ng, fg_event *sg_ev,
                                   size_t *ns, fg_event *vg_ev, size_t *nv)
{
  if (!lb) return;
  if (ng) *ng = 0;
  if (ns) *ns = 0;
  if (nv) *nv = 0;
  if (lb->main_gw.pid > 0) {
    size_t n = fg_stop(&lb->main_gw, gw_ev, gw_ev ? FG_MAX_EVENTS : 0);
    if (ng) *ng = n;
  }
  if (lb->stream_gw.pid > 0) {
    size_t n = fg_stop(&lb->stream_gw, sg_ev, sg_ev ? FG_MAX_EVENTS : 0);
    if (ns) *ns = n;
  }
  if (lb->voice_gw.pid > 0) {
    size_t n = fg_stop(&lb->voice_gw, vg_ev, vg_ev ? FG_MAX_EVENTS : 0);
    if (nv) *nv = n;
  }
  if (lb->http.pid > 0) fd_stop(&lb->http, nullptr, 0);
  if (lb->voice_udp >= 0) close(lb->voice_udp);
  if (lb->stream_udp >= 0) close(lb->stream_udp);
  lb->voice_udp = lb->stream_udp = -1;
}

int ccord_verify_live_stream_run(const ccord_verify_live_stream_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->frames || !ctx->advance_ms || !ctx->now_ms ||
      !ctx->set_transport) {
    return ls_fail("invalid context");
  }

  ccord_verify_ls_loopback lb;
  const char *lb_fail = nullptr;
  if (!ccord_verify_ls_loopback_start(&lb, &lb_fail)) return ls_fail(lb_fail);
  int voice_udp = lb.voice_udp, stream_udp = lb.stream_udp;

  ccord_rtp_h264_depack *depack = ccord_rtp_h264_depack_create();
  if (!depack) {
    ccord_verify_ls_loopback_stop(&lb, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    return ls_fail("create H.264 depacketizer");
  }
  ccord_rtp_h264_depack *cam_depack = ccord_rtp_h264_depack_create();
  if (!cam_depack) {
    ccord_rtp_h264_depack_destroy(depack);
    ccord_verify_ls_loopback_stop(&lb, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    return ls_fail("create camera H.264 depacketizer");
  }

  int rc = 1;
  bool dm_stream_store_active = false;
  bool sfu_h264_idr = false;
  bool resume_h264_idr = false;
  bool cam_h264_idr = false;
  size_t cam_h264_tx = 0;
  uint8_t cam_au[2048];
  uint32_t cam_ts = 0;

  ccord_error err = {0};
  ccord_live_config lcfg = {
    .token = "REDACTED_VERIFY_TOKEN",
    .api_base = lb.http.base,
    .gateway_url = lb.main_gw.url,
    .request_timeout_ms = 3000,
    .now_ms = ctx->now_ms,
    .clock_ud = ctx->ud,
  };
  ccord_transport *tr = ccord_live_transport_create(&lcfg, &err);
  if (!tr) {
    ls_fail(err.message);
    goto cleanup;
  }
  if (ctx->set_transport(ctx->ud, tr, &err) != CCORD_OK) {
    ls_fail(err.message);
    goto cleanup;
  }

  // 1. Wait for READY and guild/channel state.
  for (int i = 0; i < 400; i++) {
    ctx->frames(ctx->ud, 1);
    if (ccord_session_state(ctx->session) == CCORD_CONN_CONNECTED &&
        ccord_store_channel(ctx->store, LS_VCHAN) != nullptr &&
        ccord_store_channel(ctx->store, LS_DM_CHAN) != nullptr) {
      break;
    }
    ls_sleep_ms(5);
  }
  if (ccord_session_state(ctx->session) != CCORD_CONN_CONNECTED ||
      ccord_store_channel(ctx->store, LS_VCHAN) == nullptr) {
    ls_fail("gateway did not connect and populate guild/DM channels");
    goto cleanup;
  }

  // 2. Join guild voice channel 3121 and let voice gateway reach ESTABLISHED.
  if (ccord_session_join_voice(ctx->session, LS_GUILD, LS_VCHAN, &err) != CCORD_OK) {
    ls_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 300; i++) {
    ctx->advance_ms(ctx->ud, 40);
    ctx->frames(ctx->ud, 1);
    if (ccord_store_active_voice_channel(ctx->store) == LS_VCHAN && i >= 15) break;
    ls_sleep_ms(5);
  }
  if (ccord_store_active_voice_channel(ctx->store) != LS_VCHAN) {
    ls_fail("voice channel did not become active in store");
    goto cleanup;
  }

  // 2.5 Standalone camera (self_video, no Go Live): Voice OP 12 must advertise the camera descriptor
  // (active + max_bitrate/max_framerate/max_resolution), and H.264 access units sent through the same
  // session seam app_media uses must arrive sealed on the voice UDP socket. The host has no camera
  // device, so synthetic Annex-B AUs stand in for the encoder output.
  if (ccord_session_set_voice_camera(ctx->session, true, &err) != CCORD_OK) {
    ls_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 300; i++) {
    ctx->advance_ms(ctx->ud, 40);
    ctx->frames(ctx->ud, 1);
    const ccord_voice_state *vs = ccord_store_voice_user(ctx->store, LS_GUILD, LS_SELF);
    if (vs && vs->self_video) {
      size_t au_len = build_au(cam_au, !cam_h264_idr, 1200);
      (void)ccord_session_voice_send_frame(ctx->session, cam_au, au_len, cam_ts, !cam_h264_idr, &err);
      cam_ts += 3600u;
    }
    if (drain_udp_for_h264_idr(voice_udp, cam_depack)) cam_h264_idr = true;
    if (cam_h264_idr) break;
    ls_sleep_ms(5);
  }
  if (!cam_h264_idr) {
    ls_fail("standalone camera did not emit sealed H.264 IDR on the voice UDP socket");
    goto cleanup;
  }
  // Keep transmitting non-IDR slices after the IDR: continuous encrypted RTP, not a one-shot.
  for (int i = 0; i < 10 && cam_h264_tx == 0; i++) {
    ctx->advance_ms(ctx->ud, 40);
    ctx->frames(ctx->ud, 1);
    size_t au_len = build_au(cam_au, false, 600);
    (void)ccord_session_voice_send_frame(ctx->session, cam_au, au_len, cam_ts, false, &err);
    cam_ts += 3600u;
    cam_h264_tx = drain_udp_h264(voice_udp, cam_depack, nullptr);
    ls_sleep_ms(5);
  }
  if (cam_h264_tx == 0) {
    ls_fail("standalone camera stopped transmitting after the IDR");
    goto cleanup;
  }
  // Camera off: OP 4 self_video=false and OP 12 with no active stream.
  if (ccord_session_set_voice_camera(ctx->session, false, &err) != CCORD_OK) {
    ls_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 100; i++) {
    ctx->advance_ms(ctx->ud, 20);
    ctx->frames(ctx->ud, 1);
    const ccord_voice_state *vs = ccord_store_voice_user(ctx->store, LS_GUILD, LS_SELF);
    if (vs && !vs->self_video) break;
    ls_sleep_ms(5);
  }
  (void)drain_udp_h264(voice_udp, cam_depack, nullptr);

  // 3. Start Go Live broadcast and verify H.264 IDR arrives on loopback UDP SFU.
  ccord_stream_config scfg = {
    .encoder = CCORD_STREAM_ENCODER_X264,
    .codec = CCORD_STREAM_CODEC_H264,
    .framework = CCORD_STREAM_FRAMEWORK_X11,
    .resolution = CCORD_STREAM_RES_720P,
    .fps = CCORD_STREAM_FPS_30,
  };
  if (ccord_session_start_stream(ctx->session, LS_GUILD, LS_VCHAN, &scfg, &err) != CCORD_OK) {
    ls_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 600; i++) {
    ctx->advance_ms(ctx->ud, 40);
    ctx->frames(ctx->ud, 1);
    if (drain_udp_for_h264_idr(stream_udp, depack)) {
      sfu_h264_idr = true;
      if (ccord_store_stream_active(ctx->store)) break;
    }
    ls_sleep_ms(5);
  }
  if (!ccord_store_stream_active(ctx->store)) {
    ls_fail("guild stream did not become active in store");
    goto cleanup;
  }
  if (!sfu_h264_idr) {
    ls_fail("SFU UDP socket did not receive sealed H.264 IDR frame");
    goto cleanup;
  }

  // 4. Advance virtual clock > 5000 ms so live_poll emits OP 21 STREAM_PING.
  ctx->advance_ms(ctx->ud, 5200);
  for (int i = 0; i < 30; i++) {
    ctx->frames(ctx->ud, 1);
    ls_sleep_ms(5);
  }

  // 5. Pause broadcast -> OP 22 paused=true, store reflects paused.
  if (ccord_session_set_stream_paused(ctx->session, true, &err) != CCORD_OK) {
    ls_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 100; i++) {
    ctx->advance_ms(ctx->ud, 20);
    ctx->frames(ctx->ud, 1);
    const ccord_stream_state *st = ccord_store_stream(ctx->store);
    if (st && st->active && st->paused) break;
    ls_sleep_ms(5);
  }
  {
    const ccord_stream_state *st = ccord_store_stream(ctx->store);
    if (!st || !st->paused) {
      ls_fail("stream did not enter paused state in store");
      goto cleanup;
    }
  }
  // Drain any stale UDP packets while paused, then verify no H.264 frames transmit while paused.
  (void)drain_udp_for_h264_idr(stream_udp, depack);
  for (int i = 0; i < 6; i++) {
    ctx->advance_ms(ctx->ud, 40);
    ctx->frames(ctx->ud, 1);
    uint8_t dummy[2048];
    if (recvfrom(stream_udp, dummy, sizeof dummy, 0, nullptr, nullptr) > 0) {
      ls_fail("outbound UDP packet emitted while stream is paused");
      goto cleanup;
    }
    ls_sleep_ms(5);
  }

  // 6. Resume broadcast -> OP 22 paused=false, forces keyframe on resume.
  if (ccord_session_set_stream_paused(ctx->session, false, &err) != CCORD_OK) {
    ls_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 200; i++) {
    ctx->advance_ms(ctx->ud, 40);
    ctx->frames(ctx->ud, 1);
    if (drain_udp_for_h264_idr(stream_udp, depack)) resume_h264_idr = true;
    const ccord_stream_state *st = ccord_store_stream(ctx->store);
    if (st && st->active && !st->paused && resume_h264_idr) break;
    ls_sleep_ms(5);
  }
  if (!resume_h264_idr) {
    ls_fail("resumed stream did not emit H.264 IDR frame");
    goto cleanup;
  }

  // 7. Stop Go Live stream -> OP 19 + OP 4 self_stream=false, then leave guild voice.
  if (ccord_session_stop_stream(ctx->session, LS_GUILD, &err) != CCORD_OK) {
    ls_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 100; i++) {
    ctx->advance_ms(ctx->ud, 20);
    ctx->frames(ctx->ud, 1);
    if (!ccord_store_stream_active(ctx->store)) break;
    ls_sleep_ms(5);
  }
  if (ccord_store_stream_active(ctx->store)) {
    ls_fail("stream remained active after stop_stream");
    goto cleanup;
  }
  if (ccord_session_leave_voice(ctx->session, LS_GUILD, &err) != CCORD_OK) {
    ls_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 100; i++) {
    ctx->advance_ms(ctx->ud, 20);
    ctx->frames(ctx->ud, 1);
    if (ccord_store_active_voice_channel(ctx->store) == CCORD_ID_NONE) break;
    ls_sleep_ms(5);
  }

  // 8. DM Call Go Live verification: join DM call 4444, start DM stream, verify store active and H.264 IDR TX.
  (void)drain_udp_for_h264_idr(stream_udp, depack);
  if (ccord_session_call_join(ctx->session, LS_DM_CHAN, &err) != CCORD_OK) {
    ls_fail(err.message);
    goto cleanup;
  }
  for (int i = 0; i < 300; i++) {
    ctx->advance_ms(ctx->ud, 40);
    ctx->frames(ctx->ud, 1);
    if (ccord_store_active_voice_channel(ctx->store) == LS_DM_CHAN && i >= 15) break;
    ls_sleep_ms(5);
  }
  if (ccord_store_active_voice_channel(ctx->store) != LS_DM_CHAN) {
    ls_fail("DM call did not become active voice channel");
    goto cleanup;
  }
  if (ccord_session_start_stream(ctx->session, CCORD_DM_GUILD_ID, LS_DM_CHAN, &scfg, &err) != CCORD_OK) {
    ls_fail(err.message);
    goto cleanup;
  }
  bool dm_sfu_idr = false;
  for (int i = 0; i < 600; i++) {
    ctx->advance_ms(ctx->ud, 40);
    ctx->frames(ctx->ud, 1);
    if (drain_udp_for_h264_idr(stream_udp, depack)) dm_sfu_idr = true;
    const ccord_stream_state *st = ccord_store_stream(ctx->store);
    if (st && st->active && st->guild_id == CCORD_DM_GUILD_ID && st->channel_id == LS_DM_CHAN) {
      dm_stream_store_active = true;
      if (dm_sfu_idr) break;
    }
    ls_sleep_ms(5);
  }
  if (!dm_stream_store_active) {
    ls_fail("DM call stream did not become active in store");
    goto cleanup;
  }
  if (!dm_sfu_idr) {
    ls_fail("DM call stream did not emit H.264 IDR frame over UDP");
    goto cleanup;
  }
  (void)ccord_session_stop_stream(ctx->session, CCORD_DM_GUILD_ID, &err);
  (void)ccord_session_call_leave(ctx->session, LS_DM_CHAN, &err);
  for (int i = 0; i < 60; i++) {
    ctx->advance_ms(ctx->ud, 20);
    ctx->frames(ctx->ud, 1);
    ls_sleep_ms(5);
  }
  rc = 0;

cleanup:
  ccord_rtp_h264_depack_destroy(depack);
  ccord_rtp_h264_depack_destroy(cam_depack);
  fg_event gw_ev[FG_MAX_EVENTS], sg_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, ns = 0, nv = 0;
  ccord_verify_ls_loopback_stop(&lb, gw_ev, &ng, sg_ev, &ns, vg_ev, &nv);

  if (rc != 0) return 1;

  bool op4_stream_on = has_prefix_substr(gw_ev, ng, "OP4_STREAM_ON ", nullptr);
  bool op18_create = has_prefix_substr(gw_ev, ng, "OP18_CREATE ", nullptr);
  bool op21_ping = has_prefix_substr(gw_ev, ng, "OP21_PING ", nullptr);
  bool op22_pause = has_prefix_substr(gw_ev, ng, "OP22_PAUSE ", nullptr);
  bool op22_resume = has_prefix_substr(gw_ev, ng, "OP22_RESUME ", nullptr);
  bool op19_delete = has_prefix_substr(gw_ev, ng, "OP19_DELETE ", nullptr);
  bool op4_stream_off = has_prefix_substr(gw_ev, ng, "OP4_STREAM_OFF ", nullptr);

  bool op0_identify = has_prefix_substr(sg_ev, ns, "STREAM_OP0 ", "\"channel_id\":\"3121\"");
  bool op1_select_h264 = has_prefix_substr(sg_ev, ns, "STREAM_OP1_H264", nullptr);
  bool op12_video_active = has_prefix_substr(sg_ev, ns, "STREAM_OP12 ", "active=1 res=1 fps=1 br=1");

  bool op4_camera_on = has_prefix_substr(gw_ev, ng, "OP4_CAMERA_ON ", "\"self_video\":true");
  bool op4_camera_off = has_prefix_substr(gw_ev, ng, "OP4_CAMERA_OFF ", "\"self_video\":false");
  bool cam_op12_active =
    has_prefix_substr(vg_ev, nv, "VOICE_OP12 ", "active=1 res=1 fps=1 br=1 w=1 h=1 bitrate=2500000");
  bool cam_op12_off = has_prefix_substr(vg_ev, nv, "VOICE_OP12 ", "active=0");

  if (!op4_stream_on || !op18_create || !op21_ping || !op22_pause || !op22_resume || !op19_delete ||
      !op4_stream_off) {
    fprintf(stderr,
            "ccord: --verify-slice live-stream: missing gateway TX (on=%d c=%d p=%d pa=%d re=%d d=%d off=%d)\n",
            op4_stream_on, op18_create, op21_ping, op22_pause, op22_resume, op19_delete, op4_stream_off);
    return 1;
  }
  if (!op0_identify || !op1_select_h264 || !op12_video_active) {
    fprintf(stderr, "ccord: --verify-slice live-stream: missing voice TX (op0=%d op1=%d op12=%d)\n", op0_identify,
            op1_select_h264, op12_video_active);
    return 1;
  }
  if (!op4_camera_on || !op4_camera_off || !cam_op12_active || !cam_op12_off || !cam_h264_idr || cam_h264_tx == 0) {
    fprintf(stderr,
            "ccord: --verify-slice live-stream: missing camera TX (on=%d off=%d op12=%d op12off=%d idr=%d tx=%zu)\n",
            op4_camera_on ? 1 : 0, op4_camera_off ? 1 : 0, cam_op12_active ? 1 : 0, cam_op12_off ? 1 : 0,
            cam_h264_idr ? 1 : 0, cam_h264_tx);
    return 1;
  }

  printf("gateway_tx: op4_stream_on=%d op18_create=%d op21_ping=%d op22_pause=%d op22_resume=%d op19_delete=%d "
         "op4_stream_off=%d\n",
         op4_stream_on ? 1 : 0, op18_create ? 1 : 0, op21_ping ? 1 : 0, op22_pause ? 1 : 0, op22_resume ? 1 : 0,
         op19_delete ? 1 : 0, op4_stream_off ? 1 : 0);
  printf("voice_tx: op0_identify=%d op1_select_h264=%d op12_video_active=%d\n", op0_identify ? 1 : 0,
         op1_select_h264 ? 1 : 0, op12_video_active ? 1 : 0);
  printf("sfu_udp_rx: h264_idr=%d\n", sfu_h264_idr ? 1 : 0);
  printf("dm_stream_store_active=%d\n", dm_stream_store_active ? 1 : 0);
  printf("camera: op4_on=%d op4_off=%d op12_active=%d op12_off=%d h264_idr=%d h264_tx=%zu\n", op4_camera_on ? 1 : 0,
         op4_camera_off ? 1 : 0, cam_op12_active ? 1 : 0, cam_op12_off ? 1 : 0, cam_h264_idr ? 1 : 0, cam_h264_tx);
  return 0;
}
