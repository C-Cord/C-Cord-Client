// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// M5 live_transport integration tests (m5-live-transport part 3a):
//   1. Governed admin completions: VOICE_STATUS (including 500-codepoint 4-byte UTF-8),
//      STAGE_REQUEST_SPEAK, STAGE_SET_SPEAKER (+ self_suppressed TX gate), STAGE_TOPIC (200/204/bad/403)
//   2. DM / Group DM calls: call_action JOIN/LEAVE/decline, OP 4 CCORD_DM_GUILD_ID, voice server_id,
//      active guild voice preservation on ring decline, CALL_DELETE voice teardown
//   3. Soundboard in voice: POST /channels/{c}/send-soundboard-sound, 204 tone + 200 RIFF/WAVE PCM16
//      resampling, corrupt RIFF rejection, 403 refusal, SPEAKING flags=2 + CCORD_NET_SPEAKING + Opus RTP
//   4. Per-user volume & voice DSP/VAD/PTT config: validation, pre-join persistence, live mid-call updates
//   5. Go Live ccord_vstream sender/viewer lifecycle (part 3b): arbitrary credential + voice_session_id arrival
//      order, concurrent broadcast + watch, peer STREAM_CREATE cache, and <= 500 ms teardown on STREAM_DELETE /
//      stop_stream / stop_watching / leave_voice / VOICE_STATE_UPDATE(null) / disconnect
#include "fake_discord.h"
#undef _POSIX_C_SOURCE
#undef _DEFAULT_SOURCE
#include "fake_gateway.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>
#include <opus/opus.h>
#include <poll.h>
#include <sys/socket.h>

#include <ccord/core/clock.h>
#include <ccord/net/admin.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/rtp_h264.h>
#include <ccord/net/voice_audio.h>
#include <ccord/net/voice_crypto.h>
#include <ccord/net/voice_stream.h>

#include "ccord_test.h"

#define GUILD 1111ULL
#define CHAN 2222ULL
#define USER 3333ULL
#define DM_CHAN 4444ULL
#define DM_OTHER 5555ULL
#define SELF 42ULL
#define TX_SSRC 1000u
#define RX_SSRC 2000u
#define FRAME CCORD_VOICE_FRAME_SAMPLES
#define CH CCORD_VOICE_CHANNELS

[[maybe_unused]] static const uint8_t k_key[CCORD_VOICE_KEY_BYTES] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
  17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

[[maybe_unused]] static uint64_t g_clock_offset;

[[maybe_unused]] static uint64_t test_now_ms(void *ud)
{
  (void)ud;
  return ccord_clock_ms() + g_clock_offset;
}

typedef enum rest_scenario {
  SC_OK,
  SC_EMPTY_204,
  SC_FORBIDDEN,
  SC_UNPARSEABLE,
} rest_scenario;

[[maybe_unused]] static void write_le16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

[[maybe_unused]] static void write_le32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

// Builds a 24 kHz mono 16-bit PCM RIFF/WAVE with `frames` samples (resampled 2x -> 48 kHz stereo).
[[maybe_unused]] static size_t build_test_wav_24k_mono(uint8_t *buf, size_t cap, size_t frames)
{
  size_t pcm_bytes = frames * 2u;
  if (44u + pcm_bytes > cap) return 0;
  memcpy(buf, "RIFF", 4);
  write_le32(buf + 4, (uint32_t)(36u + pcm_bytes));
  memcpy(buf + 8, "WAVEfmt ", 8);
  write_le32(buf + 16, 16u);
  write_le16(buf + 20, 1u);       // PCM
  write_le16(buf + 22, 1u);       // mono
  write_le32(buf + 24, 24000u);   // 24 kHz
  write_le32(buf + 28, 48000u);   // byte rate
  write_le16(buf + 32, 2u);       // block align
  write_le16(buf + 34, 16u);      // 16-bit
  memcpy(buf + 36, "data", 4);
  write_le32(buf + 40, (uint32_t)pcm_bytes);
  for (size_t i = 0; i < frames; i++) {
    int16_t s = (i % 24u) < 12u ? (int16_t)6000 : (int16_t)-6000;
    uint16_t u = 0;
    memcpy(&u, &s, sizeof u);
    write_le16(buf + 44u + i * 2u, u);
  }
  return 44u + pcm_bytes;
}

[[maybe_unused]] static size_t respond_http(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)idx;
  rest_scenario sc = *(const rest_scenario *)ud;
  if (sc == SC_FORBIDDEN) {
    return fd_reply(out, cap, 403, "Forbidden", nullptr, "{\"message\":\"Missing Permissions\",\"code\":50013}");
  }
  if (sc == SC_UNPARSEABLE) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"unexpected\":[1,2,3]}");
  }
  if (sc == SC_EMPTY_204) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }

  if (strcmp(req->method, "PATCH") == 0 && strcmp(req->path, "/stage-instances/2222") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"2222\",\"guild_id\":\"1111\",\"channel_id\":\"2222\","
                    "\"topic\":\"Server Topic\",\"privacy_level\":2}");
  }
  if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/channels/2222/send-soundboard-sound") == 0) {
    if (strstr(req->body, "\"sound_id\":\"8802\"") != nullptr) {
      uint8_t wav[2400];
      size_t wlen = build_test_wav_24k_mono(wav, sizeof wav, 960u);  // 40 ms @ 24k -> 1920 frames @ 48k
      int hn = snprintf(out, cap,
                        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %zu\r\n"
                        "Connection: close\r\n\r\n",
                        wlen);
      if (hn <= 0 || (size_t)hn + wlen > cap) return 0;
      memcpy(out + hn, wav, wlen);
      return (size_t)hn + wlen;
    }
    if (strstr(req->body, "\"sound_id\":\"8805\"") != nullptr) {
      // 2 s @ 24 kHz mono -> 96000 stereo samples @ 48 kHz: longer than the 1 s voice ring, so it must be chunked.
      uint8_t wav[96044];
      size_t wlen = build_test_wav_24k_mono(wav, sizeof wav, 48000u);
      int hn = snprintf(out, cap,
                        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %zu\r\n"
                        "Connection: close\r\n\r\n",
                        wlen);
      if (hn <= 0 || (size_t)hn + wlen > cap) return 0;
      memcpy(out + hn, wav, wlen);
      return (size_t)hn + wlen;
    }
    if (strstr(req->body, "\"sound_id\":\"8803\"") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr, "RIFFbad");
    }
    if (strstr(req->body, "\"sound_id\":\"8804\"") != nullptr) {
      return fd_reply(out, cap, 403, "Forbidden", nullptr,
                      "{\"message\":\"Missing Permissions\",\"code\":50013}");
    }
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  return fd_reply(out, cap, 204, "No Content", nullptr, "");
}

// ---- REST-only fixture (disable_gateway = true) ------------------------------------------------------------

typedef struct rest_fx {
  fd_server srv;
  rest_scenario sc;
  ccord_transport *t;
} rest_fx;

[[maybe_unused]] static bool rest_fx_start(rest_fx *fx, rest_scenario sc)
{
  memset(fx, 0, sizeof *fx);
  g_clock_offset = 0;
  fx->sc = sc;
  if (!fd_start(&fx->srv, respond_http, &fx->sc)) return false;
  ccord_error err;
  ccord_live_config cfg = {
    .token = "REDACTED_TEST_TOKEN",
    .api_base = fx->srv.base,
    .disable_gateway = true,
    .request_timeout_ms = 3000,
    .now_ms = test_now_ms,
  };
  fx->t = ccord_live_transport_create(&cfg, &err);
  if (!fx->t || ccord_transport_connect(fx->t, &err) != CCORD_OK) return false;
  ccord_net_event ev;
  for (int i = 0; i < 100; i++) {
    if (ccord_transport_poll(fx->t, &ev) && ev.type == CCORD_NET_STATE && ev.state == CCORD_CONN_CONNECTED) return true;
    usleep(2000);
  }
  return false;
}

[[maybe_unused]] static size_t rest_fx_stop(rest_fx *fx, fd_hit *hits, size_t cap)
{
  ccord_transport_destroy(fx->t);
  fx->t = nullptr;
  return fd_stop(&fx->srv, hits, cap);
}

[[maybe_unused]] static bool wait_for_ev(ccord_transport *t, ccord_net_event_type want, ccord_net_event *out, uint64_t timeout_ms)
{
  uint64_t end = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < end) {
    if (ccord_transport_poll(t, out) && out->type == want) return true;
    usleep(2000);
  }
  return false;
}

[[maybe_unused]] static uint64_t drain_ev(ccord_transport *t, uint64_t window_ms)
{
  uint64_t seen = 0;
  uint64_t end = ccord_clock_ms() + window_ms;
  ccord_net_event ev;
  while (ccord_clock_ms() < end) {
    if (ccord_transport_poll(t, &ev) && ev.type != CCORD_NET_STATE && ev.type < 64) seen |= 1ULL << ev.type;
    usleep(2000);
  }
  return seen;
}

// ---- Full live fixture (main gateway + voice gateway v8 + UDP SFU + REST) -----------------------------------

typedef struct voice_gw_ud {
  int udp_fd_to_close;
  uint16_t udp_port;
  int ready_wr;
} voice_gw_ud;

[[maybe_unused]] static void script_voice_gw(void *ud, fg_conn *c)
{
  voice_gw_ud *v = ud;
  if (v->udp_fd_to_close >= 0) {
    close(v->udp_fd_to_close);
    v->udp_fd_to_close = -1;
  }
  if (!fg_handshake(c)) return;
  if (!fg_send(c, "{\"op\":8,\"d\":{\"heartbeat_interval\":60000}}")) return;
  char buf[2048];
  if (fg_recv(c, buf, sizeof buf, 4000) != FG_GOT_TEXT) return;
  fg_log(c, "IDENTIFY %s", buf);

  char ready[256];
  (void)snprintf(ready, sizeof ready,
                 "{\"op\":2,\"d\":{\"ssrc\":%u,\"ip\":\"127.0.0.1\",\"port\":%u,"
                 "\"modes\":[\"aead_xchacha20_poly1305_rtpsize\"]}}",
                 TX_SSRC, (unsigned)v->udp_port);
  if (!fg_send(c, ready)) return;

  for (;;) {
    int r = fg_recv(c, buf, sizeof buf, 4000);
    if (r != FG_GOT_TEXT) return;
    if (strstr(buf, "\"op\":3") != nullptr) {
      (void)fg_send(c, "{\"op\":6,\"d\":{\"t\":0}}");
      continue;
    }
    if (strstr(buf, "\"op\":1") != nullptr) {
      fg_log(c, "SELECT %s", buf);
      break;
    }
  }

  const char *sdesc =
    "{\"op\":4,\"d\":{\"mode\":\"aead_xchacha20_poly1305_rtpsize\",\"dave_protocol_version\":0,"
    "\"secret_key\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32]}}";
  if (!fg_send(c, sdesc)) return;

  char map_rx_on[128], map_rx_off[128];
  (void)snprintf(map_rx_on, sizeof map_rx_on, "{\"op\":5,\"d\":{\"user_id\":\"%llu\",\"ssrc\":%u,\"speaking\":1}}",
                 (unsigned long long)USER, RX_SSRC);
  (void)snprintf(map_rx_off, sizeof map_rx_off, "{\"op\":5,\"d\":{\"user_id\":\"%llu\",\"ssrc\":%u,\"speaking\":0}}",
                 (unsigned long long)USER, RX_SSRC);
  if (!fg_send(c, map_rx_on) || !fg_send(c, map_rx_off)) return;

  char rdy_byte = 'R';
  if (v->ready_wr >= 0) (void)!write(v->ready_wr, &rdy_byte, 1);

  for (;;) {
    int r = fg_recv(c, buf, sizeof buf, 5000);
    if (r != FG_GOT_TEXT) break;
    if (strstr(buf, "\"op\":3") != nullptr) {
      (void)fg_send(c, "{\"op\":6,\"d\":{\"t\":0}}");
      continue;
    }
    fg_log(c, "VG_TX %s", buf);
  }
}

typedef struct main_gw_ud {
  int cmd_rd;
  int cmd_wr_to_close;
  int udp_fd_to_close;
  int ready_rd_to_close;
  int ready_wr_to_close;
} main_gw_ud;

[[maybe_unused]] static void script_main_gw(void *ud, fg_conn *c)
{
  main_gw_ud *m = ud;
  if (m->cmd_wr_to_close >= 0) {
    close(m->cmd_wr_to_close);
    m->cmd_wr_to_close = -1;
  }
  if (m->udp_fd_to_close >= 0) {
    close(m->udp_fd_to_close);
    m->udp_fd_to_close = -1;
  }
  if (m->ready_rd_to_close >= 0) {
    close(m->ready_rd_to_close);
    m->ready_rd_to_close = -1;
  }
  if (m->ready_wr_to_close >= 0) {
    close(m->ready_wr_to_close);
    m->ready_wr_to_close = -1;
  }
  if (!fg_open_session(c, 60000)) return;

  char cmd_buf[4096];
  size_t cmd_len = 0;
  for (;;) {
    struct pollfd pfds[2] = {
      {.fd = c->fd, .events = POLLIN, .revents = 0},
      {.fd = m->cmd_rd, .events = POLLIN, .revents = 0},
    };
    int pr = poll(pfds, 2, 30000);
    if (pr <= 0) break;

    if ((pfds[1].revents & (POLLIN | POLLHUP)) != 0) {
      ssize_t n = read(m->cmd_rd, cmd_buf + cmd_len, sizeof cmd_buf - 1u - cmd_len);
      if (n > 0) {
        cmd_len += (size_t)n;
        cmd_buf[cmd_len] = '\0';
        for (;;) {
          char *nl = strchr(cmd_buf, '\n');
          if (!nl) break;
          *nl = '\0';
          if (cmd_buf[0] != '\0' && !fg_send(c, cmd_buf)) return;
          size_t rem = cmd_len - (size_t)(nl - cmd_buf) - 1u;
          memmove(cmd_buf, nl + 1, rem + 1u);
          cmd_len = rem;
        }
      } else if (n == 0) {
        break;
      }
    }

    if ((pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      char ws_buf[2048];
      int r = fg_recv(c, ws_buf, sizeof ws_buf, 200);
      if (r != FG_GOT_TEXT) break;
      if (strstr(ws_buf, "\"op\":1,") != nullptr || strstr(ws_buf, "\"op\":1}") != nullptr) {
        if (!fg_send(c, "{\"op\":11}")) break;
      } else {
        fg_log(c, "GW_TX %s", ws_buf);
      }
    }
  }
}

// Voice server for the reconnect tests: connection 0 does the full handshake; "CLOSE <code>" on `cmd_rd` makes the
// connection in service close with that code (0 = drop with no close frame). A connection that opens with op 7 is
// a resume and is answered with op 9. Connections from `refuse_from` on are dropped before the handshake.
typedef struct vdrop_ud {
  int udp_fd_to_close;
  uint16_t udp_port;
  int ready_wr;
  int cmd_rd;
  int refuse_from;
} vdrop_ud;

[[maybe_unused]] static void script_voice_gw_drop(void *ud, fg_conn *c)
{
  vdrop_ud *v = ud;
  if (v->udp_fd_to_close >= 0) {
    close(v->udp_fd_to_close);
    v->udp_fd_to_close = -1;
  }
  if (v->refuse_from >= 0 && c->idx >= v->refuse_from) return;
  if (!fg_handshake(c)) return;
  if (!fg_send(c, "{\"op\":8,\"d\":{\"heartbeat_interval\":60000}}")) return;
  char buf[2048];
  if (fg_recv(c, buf, sizeof buf, 4000) != FG_GOT_TEXT) return;
  if (strstr(buf, "\"op\":7") != nullptr) {
    fg_log(c, "RESUME %s", buf);
    if (!fg_send(c, "{\"op\":9,\"d\":null}")) return;
  } else {
    fg_log(c, "IDENTIFY %s", buf);
    char ready[256];
    (void)snprintf(ready, sizeof ready,
                   "{\"op\":2,\"d\":{\"ssrc\":%u,\"ip\":\"127.0.0.1\",\"port\":%u,"
                   "\"modes\":[\"aead_xchacha20_poly1305_rtpsize\"]}}",
                   TX_SSRC, (unsigned)v->udp_port);
    if (!fg_send(c, ready)) return;
    for (;;) {
      int r = fg_recv(c, buf, sizeof buf, 4000);
      if (r != FG_GOT_TEXT) return;
      if (strstr(buf, "\"op\":3") != nullptr) {
        (void)fg_send(c, "{\"op\":6,\"d\":{\"t\":0}}");
        continue;
      }
      if (strstr(buf, "\"op\":1") != nullptr) break;
    }
    const char *sdesc =
      "{\"op\":4,\"d\":{\"mode\":\"aead_xchacha20_poly1305_rtpsize\",\"dave_protocol_version\":0,"
      "\"secret_key\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32]}}";
    if (!fg_send(c, sdesc)) return;
    char map_rx_on[128], map_rx_off[128];
    (void)snprintf(map_rx_on, sizeof map_rx_on, "{\"op\":5,\"d\":{\"user_id\":\"%llu\",\"ssrc\":%u,\"speaking\":1}}",
                   (unsigned long long)USER, RX_SSRC);
    (void)snprintf(map_rx_off, sizeof map_rx_off, "{\"op\":5,\"d\":{\"user_id\":\"%llu\",\"ssrc\":%u,\"speaking\":0}}",
                   (unsigned long long)USER, RX_SSRC);
    if (!fg_send(c, map_rx_on) || !fg_send(c, map_rx_off)) return;
    if (c->idx >= 1) {   // a remote camera announced on every reconnected session
      char cam[160];
      (void)snprintf(cam, sizeof cam,
                     "{\"op\":12,\"d\":{\"user_id\":\"%llu\",\"audio_ssrc\":%u,\"video_ssrc\":%u,\"rtx_ssrc\":%u}}",
                     (unsigned long long)USER, RX_SSRC, RX_SSRC + 1u, RX_SSRC + 2u);
      if (!fg_send(c, cam)) return;
    }
  }
  char rdy_byte = 'R';
  if (v->ready_wr >= 0) (void)!write(v->ready_wr, &rdy_byte, 1);

  for (;;) {
    struct pollfd pfds[2] = {
      {.fd = c->fd, .events = POLLIN, .revents = 0},
      {.fd = v->cmd_rd, .events = POLLIN, .revents = 0},
    };
    if (poll(pfds, 2, 30000) <= 0) return;
    if ((pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      int r = fg_recv(c, buf, sizeof buf, 200);
      if (r != FG_GOT_TEXT) return;
      if (strstr(buf, "\"op\":3") != nullptr) {
        (void)fg_send(c, "{\"op\":6,\"d\":{\"t\":0}}");
        continue;
      }
      fg_log(c, "VG_TX %s", buf);
      continue;
    }
    if ((pfds[1].revents & POLLIN) != 0) {
      char cmd[32];
      ssize_t n = read(v->cmd_rd, cmd, sizeof cmd - 1u);
      if (n > 0) {
        cmd[n] = '\0';
        unsigned code = 0;
        if (sscanf(cmd, "CLOSE %u", &code) == 1) {
          fg_log(c, "CLOSING %u", code);
          if (code != 0) fg_close(c, code);
          return;
        }
      }
    }
  }
}

typedef struct live_fx {
  fd_server http;
  rest_scenario sc;
  int udp_fd;
  uint16_t udp_port;
  struct sockaddr_in client_udp;
  bool has_client_udp;
  int vready_pipe[2];
  int cmd_pipe[2];
  voice_gw_ud vud;
  vdrop_ud vdud;
  int vcmd_pipe[2];
  main_gw_ud mud;
  fg_server voice_gw;
  fg_server main_gw;
  int gw_seq;
  ccord_transport *t;
} live_fx;

[[maybe_unused]] static bool open_loopback_udp(int *out_fd, uint16_t *out_port)
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

[[maybe_unused]] static bool live_fx_start_ex(live_fx *fx, bool drop_script, int refuse_from);

[[maybe_unused]] static bool live_fx_start(live_fx *fx)
{
  return live_fx_start_ex(fx, false, -1);
}

[[maybe_unused]] static bool live_fx_start_ex(live_fx *fx, bool drop_script, int refuse_from)
{
  memset(fx, 0, sizeof *fx);
  fx->vcmd_pipe[0] = fx->vcmd_pipe[1] = -1;
  g_clock_offset = 0;
  fx->sc = SC_OK;
  fx->udp_fd = -1;
  fx->vready_pipe[0] = fx->vready_pipe[1] = -1;
  fx->cmd_pipe[0] = fx->cmd_pipe[1] = -1;
  fx->gw_seq = 2;

  if (!fd_start(&fx->http, respond_http, &fx->sc)) return false;
  if (!open_loopback_udp(&fx->udp_fd, &fx->udp_port)) return false;
  if (pipe(fx->vready_pipe) != 0 || pipe(fx->cmd_pipe) != 0) return false;
  if (drop_script && pipe(fx->vcmd_pipe) != 0) return false;

  int rflags = fcntl(fx->vready_pipe[0], F_GETFL, 0);
  if (rflags >= 0) (void)fcntl(fx->vready_pipe[0], F_SETFL, rflags | O_NONBLOCK);
  int cflags = fcntl(fx->cmd_pipe[0], F_GETFL, 0);
  if (cflags >= 0) (void)fcntl(fx->cmd_pipe[0], F_SETFL, cflags | O_NONBLOCK);

  fx->vud = (voice_gw_ud){
    .udp_fd_to_close = fx->udp_fd,
    .udp_port = fx->udp_port,
    .ready_wr = fx->vready_pipe[1],
  };
  if (drop_script) {
    fx->vdud = (vdrop_ud){
      .udp_fd_to_close = fx->udp_fd,
      .udp_port = fx->udp_port,
      .ready_wr = fx->vready_pipe[1],
      .cmd_rd = fx->vcmd_pipe[0],
      .refuse_from = refuse_from,
    };
    if (!fg_start(&fx->voice_gw, script_voice_gw_drop, &fx->vdud)) return false;
    close(fx->vcmd_pipe[0]);
    fx->vcmd_pipe[0] = -1;
  } else if (!fg_start(&fx->voice_gw, script_voice_gw, &fx->vud)) {
    return false;
  }
  close(fx->vready_pipe[1]);
  fx->vready_pipe[1] = -1;

  fx->mud = (main_gw_ud){
    .cmd_rd = fx->cmd_pipe[0],
    .cmd_wr_to_close = fx->cmd_pipe[1],
    .udp_fd_to_close = fx->udp_fd,
    .ready_rd_to_close = fx->vready_pipe[0],
    .ready_wr_to_close = -1,
  };
  if (!fg_start(&fx->main_gw, script_main_gw, &fx->mud)) return false;
  close(fx->cmd_pipe[0]);
  fx->cmd_pipe[0] = -1;

  ccord_error err;
  ccord_live_config cfg = {
    .token = "REDACTED_TEST_TOKEN",
    .api_base = fx->http.base,
    .gateway_url = fx->main_gw.url,
    .connect_timeout_ms = 3000,
    .request_timeout_ms = 3000,
    .now_ms = test_now_ms,
  };
  fx->t = ccord_live_transport_create(&cfg, &err);
  if (!fx->t || ccord_transport_connect(fx->t, &err) != CCORD_OK) return false;

  bool saw_snapshot = false;
  uint64_t deadline = ccord_clock_ms() + 4000u;
  while (ccord_clock_ms() < deadline) {
    ccord_net_event ev;
    while (ccord_transport_poll(fx->t, &ev)) {
      if (ev.type == CCORD_NET_SNAPSHOT) saw_snapshot = true;
    }
    if (saw_snapshot) return true;
    usleep(3000);
  }
  return false;
}

[[maybe_unused]] static void live_fx_stop(live_fx *fx, fd_hit *hits, size_t hcap, size_t *nhits, fg_event *gw_ev, size_t gcap,
                         size_t *ngw, fg_event *vg_ev, size_t vcap, size_t *nvg)
{
  ccord_transport_destroy(fx->t);
  fx->t = nullptr;
  if (fx->cmd_pipe[1] >= 0) close(fx->cmd_pipe[1]);
  fx->cmd_pipe[1] = -1;
  if (fx->vcmd_pipe[1] >= 0) close(fx->vcmd_pipe[1]);
  fx->vcmd_pipe[1] = -1;
  if (fx->vready_pipe[0] >= 0) close(fx->vready_pipe[0]);
  fx->vready_pipe[0] = -1;
  if (fx->udp_fd >= 0) close(fx->udp_fd);
  fx->udp_fd = -1;
  size_t nh = fd_stop(&fx->http, hits, hcap);
  size_t ng = fg_stop(&fx->main_gw, gw_ev, gcap);
  size_t nv = fg_stop(&fx->voice_gw, vg_ev, vcap);
  if (nhits) *nhits = nh;
  if (ngw) *ngw = ng;
  if (nvg) *nvg = nv;
}

[[maybe_unused]] static bool gw_emit(live_fx *fx, const char *json_line)
{
  size_t n = strlen(json_line);
  if (fx->cmd_pipe[1] < 0) return false;
  if (write(fx->cmd_pipe[1], json_line, n) != (ssize_t)n) return false;
  return write(fx->cmd_pipe[1], "\n", 1) == 1;
}

[[maybe_unused]] static void pump_udp_discovery_fd(int udp_fd, struct sockaddr_in *out_client, bool *out_has_client)
{
  if (udp_fd < 0) return;
  for (;;) {
    uint8_t req[128];
    struct sockaddr_in from = {0};
    socklen_t flen = sizeof from;
    ssize_t n = recvfrom(udp_fd, req, sizeof req, 0, (struct sockaddr *)&from, &flen);
    if (n < 0) break;
    if (n == (ssize_t)CCORD_VOICE_IP_DISCOVERY_SIZE && req[0] == 0x00 && req[1] == 0x01) {
      if (out_client) *out_client = from;
      if (out_has_client) *out_has_client = true;
      uint8_t resp[CCORD_VOICE_IP_DISCOVERY_SIZE] = {0};
      resp[0] = 0x00;
      resp[1] = 0x02;
      resp[2] = 0x00;
      resp[3] = 0x46;
      memcpy(resp + 4, req + 4, 4);
      memcpy(resp + 8, "127.0.0.1", 9);
      uint16_t port = ntohs(from.sin_port);
      resp[72] = (uint8_t)(port >> 8);
      resp[73] = (uint8_t)(port & 0xFFu);
      (void)sendto(udp_fd, resp, sizeof resp, 0, (const struct sockaddr *)&from, flen);
    }
  }
}

[[maybe_unused]] static void pump_udp_discovery(live_fx *fx)
{
  pump_udp_discovery_fd(fx->udp_fd, &fx->client_udp, &fx->has_client_udp);
}

[[maybe_unused]] static bool establish_voice(live_fx *fx, ccord_id guild_id, ccord_id channel_id, bool init_suppress)
{
  fx->has_client_udp = false;
  char drain_b;
  while (read(fx->vready_pipe[0], &drain_b, 1) > 0) {}

  char vstate[320];
  if (guild_id == CCORD_DM_GUILD_ID) {
    (void)snprintf(vstate, sizeof vstate,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":null,"
                   "\"channel_id\":\"%llu\",\"user_id\":\"%llu\",\"session_id\":\"vs-dm\",\"suppress\":%s}}",
                   fx->gw_seq++, (unsigned long long)channel_id, (unsigned long long)SELF,
                   init_suppress ? "true" : "false");
  } else {
    (void)snprintf(vstate, sizeof vstate,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":\"%llu\","
                   "\"channel_id\":\"%llu\",\"user_id\":\"%llu\",\"session_id\":\"vs-guild\",\"suppress\":%s}}",
                   fx->gw_seq++, (unsigned long long)guild_id, (unsigned long long)channel_id,
                   (unsigned long long)SELF, init_suppress ? "true" : "false");
  }
  char vserver[320];
  (void)snprintf(vserver, sizeof vserver,
                 "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":\"%llu\","
                 "\"channel_id\":\"%llu\",\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 fx->gw_seq++, (unsigned long long)guild_id, (unsigned long long)channel_id, fx->voice_gw.url);

  if (!gw_emit(fx, vstate) || !gw_emit(fx, vserver)) return false;

  bool got_ready_byte = false;
  bool got_ssrc_map = false;
  uint64_t deadline = ccord_clock_ms() + 4000u;
  while (ccord_clock_ms() < deadline) {
    ccord_net_event ev;
    while (ccord_transport_poll(fx->t, &ev)) {
      if (ev.type == CCORD_NET_SPEAKING && ev.user_id == USER && !ev.speaking) got_ssrc_map = true;
    }
    pump_udp_discovery(fx);
    char b = 0;
    if (read(fx->vready_pipe[0], &b, 1) == 1) got_ready_byte = true;
    if (got_ready_byte && fx->has_client_udp && got_ssrc_map) return true;
    usleep(2000);
  }
  return false;
}

typedef struct rtp_cap {
  uint8_t pkt[32][512];
  size_t len[32];
  size_t count;
} rtp_cap;

[[maybe_unused]] static size_t drain_udp_rtp(live_fx *fx, rtp_cap *cap, int wait_ms)
{
  cap->count = 0;
  for (;;) {
    struct pollfd pfd = {.fd = fx->udp_fd, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, cap->count == 0 ? wait_ms : 5) <= 0) break;
    uint8_t buf[1024];
    ssize_t n = recvfrom(fx->udp_fd, buf, sizeof buf, 0, nullptr, nullptr);
    if (n <= 0) break;
    if (n == (ssize_t)CCORD_VOICE_IP_DISCOVERY_SIZE && buf[0] == 0x00 && buf[1] == 0x01) continue;
    if (cap->count < 32 && (size_t)n <= sizeof cap->pkt[0]) {
      memcpy(cap->pkt[cap->count], buf, (size_t)n);
      cap->len[cap->count] = (size_t)n;
      cap->count++;
    }
  }
  return cap->count;
}

[[maybe_unused]] static bool rtp_is_audible_audio(const uint8_t *pkt, size_t len, OpusDecoder *dec)
{
  uint8_t plain[512];
  size_t plain_len = 0;
  ccord_error err;
  if (ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, pkt, len, plain, sizeof plain, &plain_len, nullptr,
                       &err) != CCORD_OK) {
    return false;
  }
  if (plain_len == 3 && plain[0] == 0xF8u && plain[1] == 0xFFu && plain[2] == 0xFEu) return false;
  int16_t pcm[FRAME * CH];
  int n = opus_decode(dec, plain, (opus_int32)plain_len, pcm, (int)FRAME, 0);
  if (n != (int)FRAME) return false;
  return ccord_pcm_rms_dbfs(pcm, FRAME, CH) > -35.0f;
}

[[maybe_unused]] static void fill_sine_stereo(int16_t *pcm, double amp, double freq, double *phase)
{
  const double two_pi = 6.28318530717958647692;
  for (size_t i = 0; i < FRAME; i++) {
    double s = amp * sin(*phase);
    *phase += two_pi * freq / 48000.0;
    int16_t v = (int16_t)(s > 32767.0 ? 32767.0 : (s < -32768.0 ? -32768.0 : s));
    pcm[i * CH] = v;
    pcm[i * CH + 1] = v;
  }
}

[[maybe_unused]] static bool send_rx_opus(live_fx *fx, OpusEncoder *enc, uint16_t seq, const int16_t *pcm)
{
  if (!fx->has_client_udp) return false;
  uint8_t opus[512];
  int n = opus_encode(enc, pcm, (int)FRAME, opus, (int)sizeof opus);
  if (n <= 0) return false;
  uint8_t hdr[CCORD_VOICE_RTP_HEADER_SIZE];
  if (ccord_rtp_write_header(hdr, sizeof hdr, CCORD_VOICE_RTP_PAYLOAD_TYPE, false, seq,
                             (uint32_t)seq * (uint32_t)FRAME, RX_SSRC) == 0) {
    return false;
  }
  uint8_t pkt[640];
  size_t pkt_len = 0;
  ccord_error err;
  if (ccord_voice_seal(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, (uint32_t)seq + 1u, hdr, sizeof hdr, opus,
                       (size_t)n, pkt, sizeof pkt, &pkt_len, &err) != CCORD_OK) {
    return false;
  }
  return sendto(fx->udp_fd, pkt, pkt_len, 0, (const struct sockaddr *)&fx->client_udp, sizeof fx->client_udp) ==
         (ssize_t)pkt_len;
}

[[maybe_unused]] static bool fg_has_substr(const fg_event *ev, size_t n, int conn, const char *a, const char *b)
{
  for (size_t i = 0; i < n; i++) {
    if (conn >= 0 && ev[i].conn != conn) continue;
    if (strstr(ev[i].text, a) != nullptr && (!b || strstr(ev[i].text, b) != nullptr)) return true;
  }
  return false;
}

// ---- 1. M5 admin completions: VOICE_STATUS (incl. 500x 4-byte UTF-8), STAGE_TOPIC, 403 refusals --------------


typedef struct stream_gw_fx {
  int udp_fd;
  uint16_t udp_port;
  int ev_pipe[2];
  uint32_t base_ssrc;
  int close_on_announce_conn;
  fg_server srv;
} stream_gw_fx;

[[maybe_unused]] static void script_stream_gw(void *ud, fg_conn *c)
{
  stream_gw_fx *s = ud;
  if (s->udp_fd >= 0) {
    close(s->udp_fd);
    s->udp_fd = -1;
  }
  if (s->ev_pipe[0] >= 0) {
    close(s->ev_pipe[0]);
    s->ev_pipe[0] = -1;
  }
  if (!fg_handshake(c)) return;
  char cb = 'C';
  if (s->ev_pipe[1] >= 0) (void)!write(s->ev_pipe[1], &cb, 1);

  if (!fg_send(c, "{\"op\":8,\"d\":{\"heartbeat_interval\":60000}}")) return;
  char buf[2048];
  if (fg_recv(c, buf, sizeof buf, 4000) != FG_GOT_TEXT) return;
  bool has_streams = strstr(buf, "\"streams\":[{\"type\":\"screen\"") != nullptr;
  fg_log(c, "S_ID streams=%d %s", has_streams ? 1 : 0, buf);

  uint32_t ssrc = s->base_ssrc + (uint32_t)c->idx * 10u;
  char ready[256];
  (void)snprintf(ready, sizeof ready,
                 "{\"op\":2,\"d\":{\"ssrc\":%u,\"ip\":\"127.0.0.1\",\"port\":%u,"
                 "\"modes\":[\"aead_xchacha20_poly1305_rtpsize\"]}}",
                 ssrc, (unsigned)s->udp_port);
  if (!fg_send(c, ready)) return;

  for (;;) {
    int r = fg_recv(c, buf, sizeof buf, 4000);
    if (r != FG_GOT_TEXT) {
      char xb = 'X';
      if (s->ev_pipe[1] >= 0) (void)!write(s->ev_pipe[1], &xb, 1);
      return;
    }
    if (strstr(buf, "\"op\":3") != nullptr) {
      (void)fg_send(c, "{\"op\":6,\"d\":{\"t\":0}}");
      continue;
    }
    if (strstr(buf, "\"op\":1") != nullptr) {
      fg_log(c, "S_SEL %s", buf);
      break;
    }
  }

  const char *sdesc =
    "{\"op\":4,\"d\":{\"mode\":\"aead_xchacha20_poly1305_rtpsize\",\"dave_protocol_version\":0,"
    "\"secret_key\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32]}}";
  if (!fg_send(c, sdesc)) return;

  for (;;) {
    int r = fg_recv(c, buf, sizeof buf, 5000);
    if (r != FG_GOT_TEXT) {
      fg_log(c, "S_CLOSE r=%d", r);
      char xb = 'X';
      if (s->ev_pipe[1] >= 0) (void)!write(s->ev_pipe[1], &xb, 1);
      return;
    }
    if (strstr(buf, "\"op\":3") != nullptr) {
      (void)fg_send(c, "{\"op\":6,\"d\":{\"t\":0}}");
      continue;
    }
    if (strstr(buf, "\"op\":12") != nullptr) {
      fg_log(c, "S_OP12 %s", buf);
      char sb = 'S';
      if (s->ev_pipe[1] >= 0) (void)!write(s->ev_pipe[1], &sb, 1);
      if (s->close_on_announce_conn >= 0 && c->idx == s->close_on_announce_conn) {
        fg_close(c, 4014);
        return;
      }
      continue;
    }
    if (strstr(buf, "\"op\":15") != nullptr) {
      fg_log(c, "S_OP15 %s", buf);
      char vb = 'V';
      if (s->ev_pipe[1] >= 0) (void)!write(s->ev_pipe[1], &vb, 1);
      continue;
    }
    fg_log(c, "S_TX %s", buf);
  }
}

[[maybe_unused]] static bool stream_gw_fx_start(stream_gw_fx *s, uint32_t base_ssrc, int close_on_announce_conn)
{
  memset(s, 0, sizeof *s);
  s->udp_fd = -1;
  s->ev_pipe[0] = s->ev_pipe[1] = -1;
  s->base_ssrc = base_ssrc;
  s->close_on_announce_conn = close_on_announce_conn;
  if (!open_loopback_udp(&s->udp_fd, &s->udp_port)) return false;
  if (pipe(s->ev_pipe) != 0) {
    close(s->udp_fd);
    s->udp_fd = -1;
    return false;
  }
  int pflags = fcntl(s->ev_pipe[0], F_GETFL, 0);
  if (pflags >= 0) (void)fcntl(s->ev_pipe[0], F_SETFL, pflags | O_NONBLOCK);
  if (!fg_start(&s->srv, script_stream_gw, s)) {
    close(s->udp_fd);
    close(s->ev_pipe[0]);
    close(s->ev_pipe[1]);
    s->udp_fd = -1;
    s->ev_pipe[0] = s->ev_pipe[1] = -1;
    return false;
  }
  close(s->ev_pipe[1]);
  s->ev_pipe[1] = -1;
  return true;
}

[[maybe_unused]] static size_t stream_gw_fx_stop(stream_gw_fx *s, fg_event *ev, size_t cap)
{
  if (s->ev_pipe[0] >= 0) close(s->ev_pipe[0]);
  s->ev_pipe[0] = -1;
  if (s->udp_fd >= 0) close(s->udp_fd);
  s->udp_fd = -1;
  return fg_stop(&s->srv, ev, cap);
}

[[maybe_unused]] static void pump_live_and_streams(live_fx *fx, stream_gw_fx *tx_gw, stream_gw_fx *rx_gw)
{
  ccord_net_event ev;
  while (ccord_transport_poll(fx->t, &ev)) {}
  pump_udp_discovery(fx);
  if (tx_gw) pump_udp_discovery_fd(tx_gw->udp_fd, nullptr, nullptr);
  if (rx_gw) pump_udp_discovery_fd(rx_gw->udp_fd, nullptr, nullptr);
}

[[maybe_unused]] static bool wait_stream_byte(live_fx *fx, stream_gw_fx *tx_gw, stream_gw_fx *rx_gw, stream_gw_fx *target, char want,
                             uint64_t timeout_ms)
{
  uint64_t deadline = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < deadline) {
    pump_live_and_streams(fx, tx_gw, rx_gw);
    char b = 0;
    while (read(target->ev_pipe[0], &b, 1) == 1) {
      if (b == want) return true;
    }
    usleep(1000);
  }
  return false;
}

[[maybe_unused]] static bool stream_gw_quiet_for(live_fx *fx, stream_gw_fx *tx_gw, stream_gw_fx *rx_gw, stream_gw_fx *target,
                                uint64_t window_ms)
{
  uint64_t deadline = ccord_clock_ms() + window_ms;
  while (ccord_clock_ms() < deadline) {
    pump_live_and_streams(fx, tx_gw, rx_gw);
    char b = 0;
    if (read(target->ev_pipe[0], &b, 1) == 1) return false;
    usleep(1000);
  }
  return true;
}

