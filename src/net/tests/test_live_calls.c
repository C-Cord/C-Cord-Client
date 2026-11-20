// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
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

static const uint8_t k_key[CCORD_VOICE_KEY_BYTES] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
  17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

static uint64_t g_clock_offset;

static uint64_t test_now_ms(void *ud)
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

static void write_le16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void write_le32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

// Builds a 24 kHz mono 16-bit PCM RIFF/WAVE with `frames` samples (resampled 2x -> 48 kHz stereo).
static size_t build_test_wav_24k_mono(uint8_t *buf, size_t cap, size_t frames)
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

static size_t respond_http(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
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

static bool rest_fx_start(rest_fx *fx, rest_scenario sc)
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

static size_t rest_fx_stop(rest_fx *fx, fd_hit *hits, size_t cap)
{
  ccord_transport_destroy(fx->t);
  fx->t = nullptr;
  return fd_stop(&fx->srv, hits, cap);
}

static bool wait_for_ev(ccord_transport *t, ccord_net_event_type want, ccord_net_event *out, uint64_t timeout_ms)
{
  uint64_t end = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < end) {
    if (ccord_transport_poll(t, out) && out->type == want) return true;
    usleep(2000);
  }
  return false;
}

static uint64_t drain_ev(ccord_transport *t, uint64_t window_ms)
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

static void script_voice_gw(void *ud, fg_conn *c)
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

static void script_main_gw(void *ud, fg_conn *c)
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

static void script_voice_gw_drop(void *ud, fg_conn *c)
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

static bool live_fx_start_ex(live_fx *fx, bool drop_script, int refuse_from);

static bool live_fx_start(live_fx *fx)
{
  return live_fx_start_ex(fx, false, -1);
}

static bool live_fx_start_ex(live_fx *fx, bool drop_script, int refuse_from)
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

static void live_fx_stop(live_fx *fx, fd_hit *hits, size_t hcap, size_t *nhits, fg_event *gw_ev, size_t gcap,
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

static bool gw_emit(live_fx *fx, const char *json_line)
{
  size_t n = strlen(json_line);
  if (fx->cmd_pipe[1] < 0) return false;
  if (write(fx->cmd_pipe[1], json_line, n) != (ssize_t)n) return false;
  return write(fx->cmd_pipe[1], "\n", 1) == 1;
}

static void pump_udp_discovery_fd(int udp_fd, struct sockaddr_in *out_client, bool *out_has_client)
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

static void pump_udp_discovery(live_fx *fx)
{
  pump_udp_discovery_fd(fx->udp_fd, &fx->client_udp, &fx->has_client_udp);
}

static bool establish_voice(live_fx *fx, ccord_id guild_id, ccord_id channel_id, bool init_suppress)
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

static size_t drain_udp_rtp(live_fx *fx, rtp_cap *cap, int wait_ms)
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

static bool rtp_is_audible_audio(const uint8_t *pkt, size_t len, OpusDecoder *dec)
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

static void fill_sine_stereo(int16_t *pcm, double amp, double freq, double *phase)
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

static bool send_rx_opus(live_fx *fx, OpusEncoder *enc, uint16_t seq, const int16_t *pcm)
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

static bool fg_has_substr(const fg_event *ev, size_t n, int conn, const char *a, const char *b)
{
  for (size_t i = 0; i < n; i++) {
    if (conn >= 0 && ev[i].conn != conn) continue;
    if (strstr(ev[i].text, a) != nullptr && (!b || strstr(ev[i].text, b) != nullptr)) return true;
  }
  return false;
}

// ---- 1. M5 admin completions: VOICE_STATUS (incl. 500x 4-byte UTF-8), STAGE_TOPIC, 403 refusals --------------

static void test_admin_voice_status_and_stage_topic(void)
{
  rest_fx fx;
  CT_CHECK(rest_fx_start(&fx, SC_OK));
  ccord_error err;
  ccord_admin_request r;
  ccord_net_event ev;

  // 1a. Short UTF-8 status with explicit guild_id.
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, "Studying \xF0\x9F\x93\x9A", &err) == CCORD_OK);
  r.guild_id = GUILD;
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATUS_UPDATE, &ev, 4000));
  CT_CHECK(ev.guild_id == GUILD && ev.channel_id == CHAN);
  CT_CHECK(ev.voice_status && strcmp(ev.voice_status, "Studying \xF0\x9F\x93\x9A") == 0);

  // 1b. 500 four-byte UTF-8 code points (2000 bytes) + selected_guild fallback when req->guild_id == 0.
  CT_CHECK(ccord_transport_select_guild(fx.t, GUILD, &err) == CCORD_OK);
  char notes[CCORD_ADMIN_VOICE_STATUS_MAX * 4u + 1u];
  for (size_t i = 0; i < CCORD_ADMIN_VOICE_STATUS_MAX; i++) {
    memcpy(notes + i * 4u, "\xF0\x9F\x8E\xB5", 4u);
  }
  notes[CCORD_ADMIN_VOICE_STATUS_MAX * 4u] = '\0';
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, notes, &err) == CCORD_OK);
  CT_CHECK(r.guild_id == CCORD_ID_NONE);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATUS_UPDATE, &ev, 4000));
  CT_CHECK(ev.guild_id == GUILD && ev.channel_id == CHAN);
  CT_CHECK(ev.voice_status && strlen(ev.voice_status) == CCORD_ADMIN_VOICE_STATUS_MAX * 4u);
  CT_CHECK(memcmp(ev.voice_status, notes, CCORD_ADMIN_VOICE_STATUS_MAX * 4u + 1u) == 0);

  // 1c. Clearing voice status emits "" and sends {"status":null}.
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, "", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATUS_UPDATE, &ev, 4000));
  CT_CHECK(ev.voice_status && ev.voice_status[0] == '\0');

  // 1d. STAGE_TOPIC with 200 JSON body uses Discord's returned stage instance object.
  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, "Requested Topic", "opening remarks", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev, 4000));
  CT_CHECK(!ev.stage_deleted && ev.stage_instance != nullptr);
  CT_CHECK(ev.stage_instance->guild_id == GUILD && ev.stage_instance->channel_id == CHAN);
  CT_CHECK(strcmp(ev.stage_instance->topic, "Server Topic") == 0);

  fd_hit hits[FD_MAX_HITS];
  size_t nh = rest_fx_stop(&fx, hits, FD_MAX_HITS);
  CT_CHECK(nh == 4);
  CT_CHECK(strcmp(hits[0].method, "PUT") == 0 && strcmp(hits[0].path, "/channels/2222/voice-status") == 0);
  CT_CHECK(strcmp(hits[0].body, "{\"status\":\"Studying \xF0\x9F\x93\x9A\"}") == 0);
  CT_CHECK(hits[1].body_len == 13u + CCORD_ADMIN_VOICE_STATUS_MAX * 4u);
  CT_CHECK(strncmp(hits[1].body, "{\"status\":\"\xF0\x9F\x8E\xB5", 15) == 0);
  CT_CHECK(strcmp(hits[2].body, "{\"status\":null}") == 0);
  CT_CHECK(strcmp(hits[3].method, "PATCH") == 0 && strcmp(hits[3].path, "/stage-instances/2222") == 0);
  CT_CHECK(strcmp(hits[3].audit, "opening%20remarks") == 0);

  // 1e. STAGE_TOPIC with 204 empty body synthesizes stage instance from op->channel_id / op->topic.
  CT_CHECK(rest_fx_start(&fx, SC_EMPTY_204));
  CT_CHECK(ccord_transport_select_guild(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, "Fallback Topic", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev, 4000));
  CT_CHECK(!ev.stage_deleted && ev.stage_instance != nullptr);
  CT_CHECK(ev.stage_instance->guild_id == GUILD && ev.stage_instance->channel_id == CHAN);
  CT_CHECK(strcmp(ev.stage_instance->topic, "Fallback Topic") == 0);
  (void)rest_fx_stop(&fx, hits, FD_MAX_HITS);

  // 1f. STAGE_TOPIC with 200 unparseable JSON emits no state event.
  CT_CHECK(rest_fx_start(&fx, SC_UNPARSEABLE));
  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, "Topic", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(drain_ev(fx.t, 120) == 0);
  (void)rest_fx_stop(&fx, hits, FD_MAX_HITS);

  // 1g. 403 Forbidden across all four M5 admin ops reports ACTION_FAILED and never mutates optimistically.
  CT_CHECK(rest_fx_start(&fx, SC_FORBIDDEN));
  CT_CHECK(ccord_admin_set_voice_status(&r, CHAN, "nope", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_ACTION_FAILED, &ev, 4000));
  CT_CHECK(ev.action == CCORD_ACTION_ADMIN && ev.admin_op == CCORD_ADMIN_VOICE_STATUS && ev.status == CCORD_ERR_PERMISSION);

  CT_CHECK(ccord_admin_stage_request_speak(&r, GUILD, CHAN, 1794312000000LL, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_ACTION_FAILED, &ev, 4000));
  CT_CHECK(ev.action == CCORD_ACTION_ADMIN && ev.admin_op == CCORD_ADMIN_STAGE_REQUEST_SPEAK);

  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CHAN, SELF, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_ACTION_FAILED, &ev, 4000));
  CT_CHECK(ev.action == CCORD_ACTION_ADMIN && ev.admin_op == CCORD_ADMIN_STAGE_SET_SPEAKER);

  CT_CHECK(ccord_admin_stage_topic(&r, CHAN, "nope", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_ACTION_FAILED, &ev, 4000));
  CT_CHECK(ev.action == CCORD_ACTION_ADMIN && ev.admin_op == CCORD_ADMIN_STAGE_TOPIC);
  CT_CHECK(drain_ev(fx.t, 80) == 0);
  (void)rest_fx_stop(&fx, hits, FD_MAX_HITS);
}

// ---- 2. Stage request-to-speak & speaker promotion/demotion + live voice TX suppression ----------------------

static void test_stage_speak_and_speaker_suppression(void)
{
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // Join stage channel starting in the audience (suppress: true).
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, true));

  int16_t loud[FRAME * CH];
  double phase = 0.0;
  fill_sine_stereo(loud, 12000.0, 440.0, &phase);
  rtp_cap cap;

  // Audience (suppressed) cannot transmit microphone RTP packets.
  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 20) == 0);

  // Raise hand (user_id == 0 resolves to l->self_id == SELF).
  ccord_admin_request r;
  ccord_net_event ev;
  CT_CHECK(ccord_admin_stage_request_speak(&r, GUILD, CHAN, 1794312000000LL, &err) == CCORD_OK);
  CT_CHECK(r.user_id == CCORD_ID_NONE);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATE_UPDATE, &ev, 4000));
  CT_CHECK(ev.guild_id == GUILD && ev.channel_id == CHAN && ev.user_id == SELF && ev.voice_state != nullptr);
  CT_CHECK(ev.voice_state->request_to_speak_timestamp == 1794312000000LL);
  CT_CHECK(ev.voice_state->omit_suppress && !ev.voice_state->omit_request_to_speak);

  // Promote self to speaker (stage_speaker = true): clears suppression and opens the TX gate immediately.
  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CHAN, SELF, true, "invite speaker", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATE_UPDATE, &ev, 4000));
  CT_CHECK(ev.voice_state != nullptr && !ev.voice_state->suppress && !ev.voice_state->omit_suppress);
  CT_CHECK(ev.voice_state->request_to_speak_timestamp == 0 && !ev.voice_state->omit_request_to_speak);

  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 30) >= 2);

  // Demote self back to audience (stage_speaker = false): suppresses mic again (after silence tail drains).
  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CHAN, SELF, false, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATE_UPDATE, &ev, 4000));
  CT_CHECK(ev.voice_state != nullptr && ev.voice_state->suppress && !ev.voice_state->omit_suppress);
  CT_CHECK(ev.voice_state->omit_request_to_speak);

  int16_t silence[FRAME * CH] = {0};
  for (int i = 0; i < 6; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, silence, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  (void)drain_udp_rtp(&fx, &cap, 15);

  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 20) == 0);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t nh = 0, ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, &nh, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);
  CT_CHECK(nh == 3);
  CT_CHECK(strcmp(hits[0].path, "/guilds/1111/voice-states/@me") == 0);
  CT_CHECK(strcmp(hits[1].path, "/guilds/1111/voice-states/42") == 0);
  CT_CHECK(strcmp(hits[1].body, "{\"channel_id\":\"2222\",\"suppress\":false}") == 0);
  CT_CHECK(strcmp(hits[2].body, "{\"channel_id\":\"2222\",\"suppress\":true}") == 0);
}

// ---- 2b. Stage REST completion preserves a participant's cached voice flags (mute/deaf/self_*) ---------------

static void test_stage_preserves_voice_flags(void)
{
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;
  ccord_admin_request r;
  ccord_net_event ev;

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, true));

  // A gateway VOICE_STATE_UPDATE for another participant (USER) carries server mute/deaf + self_* flags; the
  // transport caches them keyed by (guild, user).
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":30,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":\"2222\",\"user_id\":\"3333\",\"session_id\":\"vs-user\","
                   "\"mute\":true,\"deaf\":true,\"self_mute\":true,\"self_deaf\":true,\"suppress\":true}}"));
  bool got_user_vs = false;
  uint64_t end = ccord_clock_ms() + 4000u;
  while (ccord_clock_ms() < end && !got_user_vs) {
    if (ccord_transport_poll(fx.t, &ev) && ev.type == CCORD_NET_VOICE_STATE_UPDATE && ev.user_id == USER) {
      got_user_vs = true;
    }
    usleep(2000);
  }
  CT_CHECK(got_user_vs && ev.voice_state != nullptr && ev.voice_state->mute && ev.voice_state->deaf);

  // Promote USER to speaker: the 204 completion synthesizes a VOICE_STATE_UPDATE that must reuse the cached flags
  // instead of zeroing mute/deaf/self_* it does not own.
  CT_CHECK(ccord_admin_stage_set_speaker(&r, GUILD, CHAN, USER, true, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_VOICE_STATE_UPDATE, &ev, 4000));
  CT_CHECK(ev.user_id == USER && ev.voice_state != nullptr);
  CT_CHECK(ev.voice_state->mute && ev.voice_state->deaf);
  CT_CHECK(ev.voice_state->self_mute && ev.voice_state->self_deaf);
  CT_CHECK(!ev.voice_state->suppress && !ev.voice_state->omit_suppress);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t nh = 0, ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, &nh, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);
  CT_CHECK(nh == 1);
  CT_CHECK(strcmp(hits[0].method, "PATCH") == 0 && strcmp(hits[0].path, "/guilds/1111/voice-states/3333") == 0);
}

// ---- 3. DM / Group DM call lifecycle: ring decline preserves guild voice, join/leave, CALL_DELETE ------------

static void test_dm_call_lifecycle(void)
{
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // Argument & state validation.
  CT_CHECK(ccord_transport_call_action(fx.t, CCORD_ID_NONE, CCORD_CALL_JOIN, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, (ccord_call_action)99, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_LEAVE, &err) == CCORD_ERR_NOT_FOUND);

  // Join guild voice channel 1111/2222 first.
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));

  // Incoming DM call on 4444 rings SELF ("42").
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"CALL_CREATE\",\"s\":20,\"d\":{\"channel_id\":\"4444\","
                   "\"region\":\"us-east\",\"ringing\":[\"42\"]}}"));
  ccord_net_event ev;
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_CALL_CREATE, &ev, 4000));

  // Declining the DM ring is a governed REST stop-ringing (NOT a Gateway OP 4 null): it queues at once, and the
  // CALL_UPDATE completion drops self from ringing only after Discord answers 204. Guild voice stays alive.
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_LEAVE, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_CALL_UPDATE, &ev, 4000));
  CT_CHECK(ev.channel_id == DM_CHAN && ev.call != nullptr && ev.call->ringing_count == 0 && ev.call->active);
  // Self is now out of ringing[]; a second decline has nothing to leave.
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_LEAVE, &err) == CCORD_ERR_NOT_FOUND);

  int16_t loud[FRAME * CH];
  double phase = 0.0;
  fill_sine_stereo(loud, 12000.0, 440.0, &phase);
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);
  g_clock_offset += 25u;
  while (ccord_transport_poll(fx.t, &ev)) {}
  rtp_cap cap;
  CT_CHECK(drain_udp_rtp(&fx, &cap, 30) >= 1);

  // Now accept / join DM call 4444: replaces guild voice and identifies voice gateway with server_id = "4444".
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_JOIN, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_JOIN, &err) == CCORD_ERR_INVALID);
  CT_CHECK(establish_voice(&fx, CCORD_DM_GUILD_ID, DM_CHAN, false));
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);

  // CALL_DELETE for a different DM channel (5555) leaves active DM call 4444 untouched.
  CT_CHECK(gw_emit(&fx, "{\"op\":0,\"t\":\"CALL_DELETE\",\"s\":21,\"d\":{\"channel_id\":\"5555\"}}"));
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_CALL_DELETE, &ev, 4000));
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);

  // CALL_DELETE for active DM channel 4444 tears down the DM voice session.
  CT_CHECK(gw_emit(&fx, "{\"op\":0,\"t\":\"CALL_DELETE\",\"s\":22,\"d\":{\"channel_id\":\"4444\"}}"));
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_CALL_DELETE, &ev, 4000));
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_ERR_NOT_FOUND);

  // Re-join DM call 4444 and leave explicitly via CCORD_CALL_LEAVE.
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_JOIN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, CCORD_DM_GUILD_ID, DM_CHAN, false));
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_LEAVE, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_ERR_NOT_FOUND);

  // Drain outbound main-gateway frames so the child logs them before teardown.
  (void)drain_ev(fx.t, 40);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t nh = 0, ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, &nh, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"guild_id\":null,\"channel_id\":\"4444\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"guild_id\":null,\"channel_id\":null"));
  CT_CHECK(fg_has_substr(vg_ev, nv, 0, "IDENTIFY", "\"server_id\":\"1111\""));
  CT_CHECK(fg_has_substr(vg_ev, nv, 1, "IDENTIFY", "\"server_id\":\"4444\""));
  // The ring decline went through governed REST stop-ringing, and only the explicit in-call LEAVE sent OP 4 null.
  bool saw_stop_ringing = false;
  for (size_t i = 0; i < nh; i++) {
    if (strcmp(hits[i].method, "POST") == 0 && strcmp(hits[i].path, "/channels/4444/call/stop-ringing") == 0) {
      saw_stop_ringing = true;
    }
  }
  CT_CHECK(saw_stop_ringing);
  size_t op4_null_count = 0;
  for (size_t i = 0; i < ng; i++) {
    if (strstr(gw_ev[i].text, "\"op\":4") != nullptr &&
        strstr(gw_ev[i].text, "\"guild_id\":null,\"channel_id\":null") != nullptr) {
      op4_null_count++;
    }
  }
  CT_CHECK(op4_null_count == 1);
}

// ---- Embedded activities: launching names the live gateway session, never the session layer's stand-in -------

static void test_activity_launch_gateway_session(void)
{
  ccord_error err;
  fd_hit hits[FD_MAX_HITS];
  ccord_admin_request r;

  // No gateway (REST only): there is no READY session id to name, so the launch is refused and nothing is sent.
  rest_fx rfx;
  CT_CHECK(rest_fx_start(&rfx, SC_EMPTY_204));
  CT_CHECK(ccord_admin_activity_launch(&r, CHAN, 97002u, "ccord-gateway-session", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(rfx.t, &r, &err) == CCORD_ERR_INVALID);
  (void)drain_ev(rfx.t, 100);
  CT_CHECK(rest_fx_stop(&rfx, hits, FD_MAX_HITS) == 0);

  // Connected gateway (the fake one answers IDENTIFY with READY session "sess-1"): the REST body carries that id.
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  CT_CHECK(ccord_admin_activity_launch(&r, CHAN, 97002u, "ccord-gateway-session", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(fx.t, &r, &err) == CCORD_OK);
  (void)drain_ev(fx.t, 400);
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t nh = 0, ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, &nh, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);
  size_t launches = 0;
  for (size_t i = 0; i < nh; i++) {
    if (strcmp(hits[i].method, "POST") != 0 || strcmp(hits[i].path, "/activities/2222/97002") != 0) continue;
    launches++;
    CT_CHECK(strcmp(hits[i].body, "{\"session_id\":\"sess-1\"}") == 0);
    CT_CHECK(strstr(hits[i].body, "ccord-gateway-session") == nullptr);
  }
  CT_CHECK(launches == 1u);
}

// ---- 4. Soundboard in voice: 204 tone, 200 WAV resample, corrupt RIFF, 403 refusal --------------------------

static void test_soundboard_playback(void)
{
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // Cannot play soundboard before joining voice, or with invalid / DM guild IDs.
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, 8801, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_play_soundboard(fx.t, CCORD_ID_NONE, 8801, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_play_soundboard(fx.t, CCORD_DM_GUILD_ID, 8801, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD + 1u, 8801, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);

  int opus_err = OPUS_OK;
  OpusDecoder *dec = opus_decoder_create(48000, (int)CH, &opus_err);
  CT_CHECK(dec != nullptr);

  // 4a. 204 No Content (sound_id = 8801): synthesizes 20 ms tone, emits SPEAKING(true->false), sends RTP + OP 5 flags=2.
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, 8801, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_SPEAKING, &ev, 4000));
  CT_CHECK(ev.guild_id == GUILD && ev.channel_id == CHAN && ev.user_id == SELF && ev.speaking);

  g_clock_offset += 25u;
  while (ccord_transport_poll(fx.t, &ev)) {}
  rtp_cap cap;
  CT_CHECK(drain_udp_rtp(&fx, &cap, 30) == 1);
  if (dec && cap.count >= 1) CT_CHECK(rtp_is_audible_audio(cap.pkt[0], cap.len[0], dec));

  g_clock_offset += 50u;
  bool saw_speak_off = false;
  uint64_t end = ccord_clock_ms() + 1000u;
  while (ccord_clock_ms() < end && !saw_speak_off) {
    if (ccord_transport_poll(fx.t, &ev) && ev.type == CCORD_NET_SPEAKING && ev.user_id == SELF && !ev.speaking) {
      saw_speak_off = true;
    }
    usleep(2000);
  }
  CT_CHECK(saw_speak_off);

  // 4b. 200 OK WAV (sound_id = 8802, 24 kHz mono 960 samples -> resampled to 1920 stereo samples @ 48 kHz = 2 RTP frames).
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, 8802, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_SPEAKING, &ev, 4000));
  CT_CHECK(ev.speaking && ev.user_id == SELF);
  for (int i = 0; i < 3; i++) {
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 30) == 2);
  if (dec && cap.count == 2) {
    CT_CHECK(rtp_is_audible_audio(cap.pkt[0], cap.len[0], dec));
    CT_CHECK(rtp_is_audible_audio(cap.pkt[1], cap.len[1], dec));
  }
  g_clock_offset += 60u;
  (void)drain_ev(fx.t, 30);

  // 4c. Corrupt RIFF payload (sound_id = 8803) is rejected cleanly without SPEAKING or RTP.
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, 8803, &err) == CCORD_OK);
  CT_CHECK(drain_ev(fx.t, 80) == 0);
  CT_CHECK(drain_udp_rtp(&fx, &cap, 15) == 0);

  // 4d. 403 Forbidden (sound_id = 8804) emits ACTION_FAILED with expr_id = 8804 and no audio.
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, 8804, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_ACTION_FAILED, &ev, 4000));
  CT_CHECK(ev.action == CCORD_ACTION_ADMIN && ev.guild_id == GUILD && ev.channel_id == CHAN && ev.expr_id == 8804);
  CT_CHECK(ev.status == CCORD_ERR_PERMISSION);
  CT_CHECK(drain_udp_rtp(&fx, &cap, 15) == 0);

  // 4e. >1 s WAV (sound_id = 8805, 24 kHz mono 48000 frames -> 96000 stereo samples @ 48 kHz = 2 s) must be chunked
  //     into the 1 s voice ring and play fully instead of being rejected with CCORD_ERR_RANGE.
  (void)drain_ev(fx.t, 40);
  CT_CHECK(ccord_transport_play_soundboard(fx.t, GUILD, 8805, &err) == CCORD_OK);
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_SPEAKING, &ev, 4000));
  CT_CHECK(ev.speaking && ev.user_id == SELF);
  size_t long_frames = 0;
  for (int i = 0; i < 130; i++) {
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
    rtp_cap c2;
    size_t n = drain_udp_rtp(&fx, &c2, 5);
    for (size_t j = 0; j < n; j++) {
      if (dec && rtp_is_audible_audio(c2.pkt[j], c2.len[j], dec)) long_frames++;
    }
  }
  // A full 2 s clip is ~100 RTP frames; >= 90 audible frames proves the >1 s WAV was chunked and played, not dropped
  // by the 1 s ring cap.
  CT_CHECK(long_frames >= 90);
  g_clock_offset += 60u;
  (void)drain_ev(fx.t, 30);

  if (dec) opus_decoder_destroy(dec);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t nh = 0, ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, &nh, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);
  CT_CHECK(nh == 5);
  CT_CHECK(strcmp(hits[0].method, "POST") == 0 && strcmp(hits[0].path, "/channels/2222/send-soundboard-sound") == 0);
  CT_CHECK(strcmp(hits[0].body, "{\"sound_id\":\"8801\",\"source_guild_id\":\"1111\"}") == 0);
  CT_CHECK(fg_has_substr(vg_ev, nv, 0, "VG_TX", "\"speaking\":2"));
}

// ---- 5. Per-user volume & voice DSP/VAD/PTT config (persistence + live updates) ------------------------------

static void test_user_volume_and_voice_config(void)
{
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // Validation checks.
  CT_CHECK(ccord_transport_set_user_volume(fx.t, CCORD_ID_NONE, 100, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_set_user_volume(fx.t, USER, 201, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_transport_set_voice_config(fx.t, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_voice_config bad_cfg = {.vad_sensitivity_pct = 101};
  CT_CHECK(ccord_transport_set_voice_config(fx.t, &bad_cfg, &err) == CCORD_ERR_RANGE);

  // Persist user volume (0% = muted) and strict VAD sensitivity (0% = -20 dBFS) BEFORE joining voice.
  CT_CHECK(ccord_transport_set_user_volume(fx.t, USER, 0, &err) == CCORD_OK);
  ccord_voice_config strict_vad = {.ptt = false, .vad_sensitivity_pct = 0, .ns = false, .aec = false, .agc = false};
  CT_CHECK(ccord_transport_set_voice_config(fx.t, &strict_vad, &err) == CCORD_OK);

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));

  int opus_err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(48000, (int)CH, OPUS_APPLICATION_AUDIO, &opus_err);
  CT_CHECK(enc != nullptr);

  int16_t rx_in[FRAME * CH];
  int16_t rx_out[FRAME * CH * 4];
  double phase = 0.0;
  uint16_t seq = 0;

  // Pre-join 0% user volume mutes incoming RX RTP packets for USER (3333).
  // Send 4 frames at once (>= jitter prefill target 3) so the jitter buffer starts and drains muted.
  for (int i = 0; i < 4; i++) {
    fill_sine_stereo(rx_in, 8000.0, 440.0, &phase);
    CT_CHECK(send_rx_opus(&fx, enc, seq++, rx_in));
  }
  usleep(5000);
  for (int i = 0; i < 4; i++) {
    CT_CHECK(ccord_transport_pull_voice_pcm(fx.t, rx_out, FRAME * 2u) == 0);
  }

  // Live update to 100%, prime Opus state, then compare 100% vs 200% (+6 dB) vs 50% (-6 dB).
  CT_CHECK(ccord_transport_set_user_volume(fx.t, USER, 100, &err) == CCORD_OK);
  for (int i = 0; i < 6; i++) {
    fill_sine_stereo(rx_in, 8000.0, 440.0, &phase);
    CT_CHECK(send_rx_opus(&fx, enc, seq++, rx_in));
    usleep(5000);
    CT_CHECK(ccord_transport_pull_voice_pcm(fx.t, rx_out, FRAME) == FRAME);
  }
  fill_sine_stereo(rx_in, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx_opus(&fx, enc, seq++, rx_in));
  usleep(5000);
  CT_CHECK(ccord_transport_pull_voice_pcm(fx.t, rx_out, FRAME) == FRAME);
  float db100 = ccord_pcm_rms_dbfs(rx_out, FRAME, CH);
  CT_CHECK(db100 > -25.0f);

  CT_CHECK(ccord_transport_set_user_volume(fx.t, USER, 200, &err) == CCORD_OK);
  fill_sine_stereo(rx_in, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx_opus(&fx, enc, seq++, rx_in));
  usleep(5000);
  CT_CHECK(ccord_transport_pull_voice_pcm(fx.t, rx_out, FRAME) == FRAME);
  float db200 = ccord_pcm_rms_dbfs(rx_out, FRAME, CH);
  CT_CHECK(db200 - db100 > 4.5f);

  CT_CHECK(ccord_transport_set_user_volume(fx.t, USER, 50, &err) == CCORD_OK);
  fill_sine_stereo(rx_in, 8000.0, 440.0, &phase);
  CT_CHECK(send_rx_opus(&fx, enc, seq++, rx_in));
  usleep(5000);
  CT_CHECK(ccord_transport_pull_voice_pcm(fx.t, rx_out, FRAME) == FRAME);
  float db50 = ccord_pcm_rms_dbfs(rx_out, FRAME, CH);
  CT_CHECK(db100 - db50 > 4.5f);

  // TX check 1: quiet signal (~ -50 dBFS) is gated off by pre-join strict VAD (0% -> -20 dBFS).
  int16_t quiet[FRAME * CH];
  fill_sine_stereo(quiet, 120.0, 440.0, &phase);
  rtp_cap cap;
  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, quiet, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 20) == 0);

  // TX check 2: live update to sensitive VAD (100% -> -70 dBFS) lets the same quiet signal transmit.
  ccord_voice_config sens_vad = {.ptt = false, .vad_sensitivity_pct = 100, .ns = false, .aec = false, .agc = false};
  CT_CHECK(ccord_transport_set_voice_config(fx.t, &sens_vad, &err) == CCORD_OK);
  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, quiet, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 25) >= 2);

  // Drain VAD hangover + silence tail back to idle, then switch to PTT with 0% sensitivity:
  // pushing quiet PCM under PTT activates PTT in live_push_voice_pcm and transmits immediately.
  int16_t zero[FRAME * CH] = {0};
  for (int i = 0; i < 22; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, zero, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  (void)drain_udp_rtp(&fx, &cap, 15);

  ccord_voice_config ptt_cfg = {.ptt = true, .vad_sensitivity_pct = 0, .ns = false, .aec = false, .agc = false};
  CT_CHECK(ccord_transport_set_voice_config(fx.t, &ptt_cfg, &err) == CCORD_OK);

  for (int i = 0; i < 2; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, quiet, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  CT_CHECK(drain_udp_rtp(&fx, &cap, 25) >= 2);

  // Advance past ptt_release_ms (60 ms) so live_poll releases PTT cleanly.
  g_clock_offset += 80u;
  for (int i = 0; i < 22; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, zero, FRAME, &err) == CCORD_OK);
    g_clock_offset += 25u;
    ccord_net_event tmp;
    while (ccord_transport_poll(fx.t, &tmp)) {}
  }
  (void)drain_udp_rtp(&fx, &cap, 15);

  // Deterministic race-free PTT release: queue a burst of mid-amp frames while held (each push re-arms the 60 ms
  // release deadline), then STOP pushing and advance past the deadline so live_poll releases PTT via the atomics.
  // The VAD release hangover passes only the head of the queued tail, so the audible count is >= 1 (PTT held did
  // transmit) but < the full burst (release gated the rest). A subsequent push would re-arm and hide the release.
  ccord_voice_config ptt_burst_cfg = {.ptt = true, .vad_sensitivity_pct = 0, .ns = false, .aec = false, .agc = false};
  CT_CHECK(ccord_transport_set_voice_config(fx.t, &ptt_burst_cfg, &err) == CCORD_OK);
  int16_t mid[FRAME * CH];
  double ph_burst = 0.0;
  fill_sine_stereo(mid, 12000.0, 440.0, &ph_burst);
  for (int i = 0; i < 20; i++) {
    CT_CHECK(ccord_transport_push_voice_pcm(fx.t, mid, FRAME, &err) == CCORD_OK);
  }
  g_clock_offset += 80u;   // past ptt_release_ms; live_poll now releases PTT atomically
  ccord_net_event tmp;
  while (ccord_transport_poll(fx.t, &tmp)) {}
  int ptt_dec_err = OPUS_OK;
  OpusDecoder *ptt_dec = opus_decoder_create(48000, (int)CH, &ptt_dec_err);
  CT_CHECK(ptt_dec != nullptr);
  size_t ptt_audible = 0;
  for (int i = 0; i < 40; i++) {
    g_clock_offset += 25u;
    while (ccord_transport_poll(fx.t, &tmp)) {}
    rtp_cap c2;
    size_t n = drain_udp_rtp(&fx, &c2, 5);
    for (size_t j = 0; j < n; j++) {
      if (ptt_dec && rtp_is_audible_audio(c2.pkt[j], c2.len[j], ptt_dec)) ptt_audible++;
    }
  }
  CT_CHECK(ptt_audible >= 1 && ptt_audible < 20);
  if (ptt_dec) opus_decoder_destroy(ptt_dec);

  if (enc) opus_encoder_destroy(enc);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, nullptr, vg_ev, FG_MAX_EVENTS, nullptr);
}

// ---- 6. Go Live ccord_vstream sender/viewer lifecycle & <= 500 ms teardown (part 3b) ------------------------

typedef struct stream_gw_fx {
  int udp_fd;
  uint16_t udp_port;
  int ev_pipe[2];
  uint32_t base_ssrc;
  int close_on_announce_conn;
  fg_server srv;
} stream_gw_fx;

static void script_stream_gw(void *ud, fg_conn *c)
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

static bool stream_gw_fx_start(stream_gw_fx *s, uint32_t base_ssrc, int close_on_announce_conn)
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

static size_t stream_gw_fx_stop(stream_gw_fx *s, fg_event *ev, size_t cap)
{
  if (s->ev_pipe[0] >= 0) close(s->ev_pipe[0]);
  s->ev_pipe[0] = -1;
  if (s->udp_fd >= 0) close(s->udp_fd);
  s->udp_fd = -1;
  return fg_stop(&s->srv, ev, cap);
}

static void pump_live_and_streams(live_fx *fx, stream_gw_fx *tx_gw, stream_gw_fx *rx_gw)
{
  ccord_net_event ev;
  while (ccord_transport_poll(fx->t, &ev)) {}
  pump_udp_discovery(fx);
  if (tx_gw) pump_udp_discovery_fd(tx_gw->udp_fd, nullptr, nullptr);
  if (rx_gw) pump_udp_discovery_fd(rx_gw->udp_fd, nullptr, nullptr);
}

static bool wait_stream_byte(live_fx *fx, stream_gw_fx *tx_gw, stream_gw_fx *rx_gw, stream_gw_fx *target, char want,
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

static bool stream_gw_quiet_for(live_fx *fx, stream_gw_fx *tx_gw, stream_gw_fx *rx_gw, stream_gw_fx *target,
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

static void test_go_live_stream_lifecycle(void)
{
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 3100u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 4100u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // 6a. Validation guards before joining voice.
  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_60};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CCORD_ID_NONE, USER, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_stop_watching(fx.t, GUILD, CCORD_ID_NONE, USER, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_set_stream_paused(fx.t, GUILD, CCORD_ID_NONE, USER, true, &err) == CCORD_ERR_INVALID);

  // Pre-existing peer broadcast arrives BEFORE we join voice: rtc_server_id 9910 is cached in stream_peers.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":30,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"rtc_server_id\":\"9910\",\"rtc_channel_id\":\"2222\"}}"));
  ccord_net_event ev;
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_STREAM_SIGNAL, &ev, 4000));

  // Join voice on (GUILD, CHAN) — stream_peers must retain peer 3333's rtc_server_id across join_voice!
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD + 1u, CHAN, &scfg, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, nullptr, &err) == CCORD_ERR_INVALID);

  // 6b. Sender lifecycle (CREATE -> SERVER_UPDATE) + STREAM_DELETE <= 500 ms close.
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":31,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9901\",\"rtc_channel_id\":\"2222\"}}"));
  // STREAM_CREATE alone (without STREAM_SERVER_UPDATE) must not open a vstream yet.
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 35u));

  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":32,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  // Server-pushed STREAM_DELETE tears down tx_stream WebSocket within <= 500 ms while voice stays alive.
  uint64_t t0 = ccord_clock_ms();
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_DELETE\",\"s\":33,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"reason\":\"stream_full\"}}"));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_ERR_NOT_FOUND);

  int16_t loud[FRAME * CH];
  double phase = 0.0;
  fill_sine_stereo(loud, 10000.0, 440.0, &phase);
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);

  // 6c. Reverse credential order (SERVER_UPDATE before CREATE) + explicit stop_stream <= 500 ms close.
  ccord_stream_config scfg1080 = {.resolution = CCORD_STREAM_RES_1080P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg1080, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":34,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  // STREAM_SERVER_UPDATE alone (without STREAM_CREATE rtc_server_id) must wait for STREAM_CREATE.
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 35u));

  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":35,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9902\",\"rtc_channel_id\":\"2222\"}}"));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  t0 = ccord_clock_ms();
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);

  // 6d. Concurrent broadcast (tx_stream) + viewer (rx_stream using cached peer rtc_server_id 9910):
  //     sender and viewer run simultaneously without clobbering each other.
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":36,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9903\",\"rtc_channel_id\":\"2222\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":37,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  // Watch peer 3333: send ONLY STREAM_SERVER_UPDATE — cached rtc_server_id 9910 pairs immediately!
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":38,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'V', 4000u));
  // tx_stream stayed open while rx_stream opened!
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 20u));

  // Pause/resume signalling + STREAM_UPDATE dispatch leave media sockets intact.
  CT_CHECK(ccord_transport_set_stream_paused(fx.t, GUILD, CHAN, USER, true, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_UPDATE\",\"s\":39,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"paused\":true}}"));
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &rx_gw, 20u));

  // stop_watching closes rx_stream in <= 500 ms while tx_stream continues uninterrupted!
  t0 = ccord_clock_ms();
  CT_CHECK(ccord_transport_stop_watching(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 20u));

  // Re-watch peer 3333 (rtc_server_id 9910 still in stream_peers), then peer STREAM_DELETE closes rx_stream <= 500 ms.
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":40,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'V', 4000u));

  t0 = ccord_clock_ms();
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_DELETE\",\"s\":41,\"d\":{\"stream_key\":\"guild:1111:2222:3333\"}}"));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);
  // Self broadcast tx_stream is STILL unaffected by peer's STREAM_DELETE!
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 20u));

  // Peer 3333 was dropped from stream_peers on STREAM_DELETE: watching again without STREAM_CREATE does NOT open.
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":42,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_STREAM_SIGNAL, &ev, 4000));
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &rx_gw, 25u));
  CT_CHECK(ccord_transport_stop_watching(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);

  // unavailable: true on STREAM_CREATE/UPDATE closes tx_stream within <= 500 ms.
  t0 = ccord_clock_ms();
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":43,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"unavailable\":true}}"));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);

  (void)drain_ev(fx.t, 30u);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  size_t ng = 0, ntx = 0, nrx = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, nullptr);
  ntx = stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  nrx = stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);

  // Verify outbound main-gateway Go Live opcodes (18 create, 19 delete, 20 watch, 22 set_paused).
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":18", "\"channel_id\":\"2222\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":19", "\"stream_key\":\"guild:1111:2222:42\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":20", "\"stream_key\":\"guild:1111:2222:3333\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":22", "\"paused\":true"));

  // Verify sender connections identified with streams=[screen] and rtc_server_id (9901, 9902, 9903) + OP 12.
  CT_CHECK(fg_has_substr(tx_ev, ntx, 0, "S_ID streams=1", "\"server_id\":\"9901\""));
  CT_CHECK(fg_has_substr(tx_ev, ntx, 0, "S_ID streams=1", "\"session_id\":\"vs-guild\""));
  CT_CHECK(fg_has_substr(tx_ev, ntx, 0, "S_OP12", "\"video_ssrc\":"));
  CT_CHECK(fg_has_substr(tx_ev, ntx, 1, "S_ID streams=1", "\"server_id\":\"9902\""));
  CT_CHECK(fg_has_substr(tx_ev, ntx, 2, "S_ID streams=1", "\"server_id\":\"9903\""));

  // Verify viewer connections identified WITHOUT sender streams[] (streams=0), server_id=9910, and sent OP 15.
  CT_CHECK(fg_has_substr(rx_ev, nrx, 0, "S_ID streams=0", "\"server_id\":\"9910\""));
  CT_CHECK(fg_has_substr(rx_ev, nrx, 0, "S_OP15", "\"any\":100"));
  CT_CHECK(fg_has_substr(rx_ev, nrx, 1, "S_ID streams=0", "\"server_id\":\"9910\""));
  CT_CHECK(!fg_has_substr(rx_ev, nrx, 2, "S_ID", nullptr));
}

static void test_go_live_teardown_and_late_voice_session(void)
{
  // conn 2 on tx_gw triggers an SFU-side WebSocket close (4014) right after OP 12 to test live_poll reaping.
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 5100u, 2));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 6100u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;
  char ssu[384];

  // 6e. Credentials arrive BEFORE VOICE_STATE_UPDATE (voice_session_id empty until VOICE_STATE_UPDATE):
  //     also verifies watch_stream called BEFORE join_voice on the same channel is preserved by join_voice!
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);

  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":50,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"rtc_server_id\":\"9920\",\"rtc_channel_id\":\"2222\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":51,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  // Still missing voice_session_id -> must NOT open rx_gw yet!
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &rx_gw, 35u));

  // Now VOICE_STATE_UPDATE arrives with session_id="vs-late": rx_gw opens immediately!
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":52,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":\"2222\",\"user_id\":\"42\",\"session_id\":\"vs-late\",\"suppress\":false}}"));
  char vserver[320];
  (void)snprintf(vserver, sizeof vserver,
                 "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"s\":53,\"d\":{\"guild_id\":\"1111\","
                 "\"channel_id\":\"2222\",\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 fx.voice_gw.url);
  CT_CHECK(gw_emit(&fx, vserver));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'V', 4000u));

  // Start tx_stream as well (conn 0 on tx_gw), then leave_voice must tear down BOTH within <= 500 ms.
  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_1080P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":54,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9921\",\"rtc_channel_id\":\"2222\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":55,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  uint64_t t0 = ccord_clock_ms();
  CT_CHECK(ccord_transport_leave_voice(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);

  // 6f. Server-pushed VOICE_STATE_UPDATE(channel_id: null) kick closes active Go Live stream in <= 500 ms.
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":58,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9922\",\"rtc_channel_id\":\"2222\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":59,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  t0 = ccord_clock_ms();
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":60,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":null,\"user_id\":\"42\",\"session_id\":\"vs-guild\"}}"));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);

  // 6g. SFU-initiated close on conn 2 is reaped by live_poll without reconnect spam; disconnect tears down rx_stream <= 500 ms.
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":63,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9923\",\"rtc_channel_id\":\"2222\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":64,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));
  // Server closed conn 2 with 4014; polling must reap tx_stream.vs cleanly and NOT reconnect (no conn 3 'C').
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 45u));

  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":65,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'V', 4000u));

  t0 = ccord_clock_ms();
  ccord_transport_disconnect(fx.t);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &rx_gw, 'X', 500u));
  CT_CHECK(ccord_clock_ms() - t0 <= 500u);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  size_t ntx = 0, nrx = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, nullptr, vg_ev, FG_MAX_EVENTS, nullptr);
  ntx = stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  nrx = stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);

  CT_CHECK(fg_has_substr(rx_ev, nrx, 0, "S_ID streams=0", "\"session_id\":\"vs-late\""));
  CT_CHECK(fg_has_substr(rx_ev, nrx, 0, "S_ID streams=0", "\"server_id\":\"9920\""));
  CT_CHECK(fg_has_substr(tx_ev, ntx, 2, "S_ID streams=1", "\"server_id\":\"9923\""));
  CT_CHECK(!fg_has_substr(tx_ev, ntx, 3, "S_ID", nullptr));
}

// ---- 7. Go Live media plane: H.264 + Opus/PCM hooks over UDP/RTP loopback (m5-golive-live) ------------------

typedef struct media_rxcap {
  uint8_t pkt[64][2048];
  size_t len[64];
  size_t count;
} media_rxcap;

typedef struct media_fx {
  live_fx *fx;
  stream_gw_fx *tx_gw, *rx_gw;
  struct sockaddr_in rx_client;
  bool has_rx_client;
  uint32_t rx_nonce;
} media_fx;

// Like pump_live_and_streams, but records the viewer's media source address from its IP-discovery request.
static void pump_media(media_fx *m)
{
  ccord_net_event ev;
  while (ccord_transport_poll(m->fx->t, &ev)) {}
  pump_udp_discovery(m->fx);
  pump_udp_discovery_fd(m->tx_gw->udp_fd, nullptr, nullptr);
  pump_udp_discovery_fd(m->rx_gw->udp_fd, &m->rx_client, &m->has_rx_client);
}

static bool wait_stream_byte_media(media_fx *m, stream_gw_fx *target, char want, uint64_t timeout_ms)
{
  uint64_t deadline = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < deadline) {
    pump_media(m);
    char b = 0;
    while (read(target->ev_pipe[0], &b, 1) == 1) {
      if (b == want) return true;
    }
    usleep(1000);
  }
  return false;
}

static void settle_media(media_fx *m, uint64_t window_ms)
{
  uint64_t deadline = ccord_clock_ms() + window_ms;
  while (ccord_clock_ms() < deadline) {
    pump_media(m);
    usleep(1000);
  }
}

static size_t drain_sfu_media(int udp_fd, media_rxcap *cap, int wait_ms)
{
  cap->count = 0;
  for (;;) {
    struct pollfd pfd = {.fd = udp_fd, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, cap->count == 0 ? wait_ms : 20) <= 0) break;
    uint8_t buf[2048];
    ssize_t n = recvfrom(udp_fd, buf, sizeof buf, 0, nullptr, nullptr);
    if (n <= 0) break;
    if (n == (ssize_t)CCORD_VOICE_IP_DISCOVERY_SIZE && buf[0] == 0x00 && buf[1] == 0x01) continue;
    if (cap->count < 64 && (size_t)n <= sizeof cap->pkt[0]) {
      memcpy(cap->pkt[cap->count], buf, (size_t)n);
      cap->len[cap->count] = (size_t)n;
      cap->count++;
    }
  }
  return cap->count;
}

// Annex-B access unit with 4-byte start codes: SPS+PPS+IDR for keyframes, one non-IDR slice otherwise.
static size_t build_media_au(uint8_t *out, bool keyframe, size_t payload_bytes)
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
  for (size_t i = 0; i < payload_bytes; i++) out[n++] = (uint8_t)(i * 7u + 3u);
  return n;
}

// Re-encrypt captured SFU-bound payloads toward the viewer under a remote SSRC (fresh AEAD nonce per packet),
// preserving payload type / seq / timestamp / marker. Works for both H.264 and Opus packets.
static size_t loop_media_to_viewer(media_fx *m, const media_rxcap *cap, uint32_t remote_ssrc)
{
  size_t sent = 0;
  for (size_t i = 0; i < cap->count; i++) {
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    ccord_error err;
    if (ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, cap->pkt[i], cap->len[i], plain, sizeof plain,
                         &plain_len, &view, &err) != CCORD_OK) {
      continue;
    }
    uint8_t hdr[CCORD_RTP_FIXED_HEADER];
    if (ccord_rtp_write_header(hdr, sizeof hdr, view.payload_type, view.marker, view.seq, view.timestamp,
                               remote_ssrc) == 0) {
      continue;
    }
    uint8_t pkt[2048];
    size_t pkt_len = 0;
    if (ccord_voice_seal(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, ++m->rx_nonce, hdr, sizeof hdr, plain,
                         plain_len, pkt, sizeof pkt, &pkt_len, &err) != CCORD_OK) {
      continue;
    }
    if (sendto(m->rx_gw->udp_fd, pkt, pkt_len, 0, (const struct sockaddr *)&m->rx_client, sizeof m->rx_client) ==
        (ssize_t)pkt_len) {
      sent++;
    }
  }
  return sent;
}

static bool wait_media_frame(media_fx *m, uint8_t *buf, size_t cap, size_t *len, uint32_t *ts, bool *kf,
                             uint64_t timeout_ms)
{
  uint64_t deadline = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < deadline) {
    pump_media(m);
    if (ccord_transport_poll_stream_frame(m->fx->t, buf, cap, len, ts, kf)) return true;
    usleep(1000);
  }
  return false;
}

// Accumulates exactly `want` samples (partial polls consume, so keep draining into the tail of out).
static size_t wait_media_pcm(media_fx *m, int16_t *out, size_t want, uint64_t timeout_ms)
{
  uint64_t deadline = ccord_clock_ms() + timeout_ms;
  size_t got = 0;
  while (got < want && ccord_clock_ms() < deadline) {
    pump_media(m);
    got += ccord_transport_poll_stream_pcm(m->fx->t, out + got, want - got);
    if (got < want) usleep(1000);
  }
  return got;
}

static void test_go_live_media_plane(void)
{
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 7100u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 8100u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  media_fx m = {.fx = &fx, .tx_gw = &tx_gw, .rx_gw = &rx_gw, .rx_nonce = 0};
  ccord_error err;
  uint8_t au[8192], rbuf[8192];
  size_t au_len, rlen;
  uint32_t rts;
  bool rkf;
  int16_t mono[FRAME * 3];
  double phase = 0.0;
  for (size_t i = 0; i < FRAME * 3; i++) {
    mono[i] = (int16_t)(10000.0 * sin(phase));
    phase += 6.28318530717958647692 * 440.0 / 48000.0;
  }

  // 7a. Validation before any broadcast exists.
  au_len = build_media_au(au, true, 40);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, nullptr, 16, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, 0, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 0, true, &err) == CCORD_ERR_INVALID);
  static uint8_t huge[262145];   // 256 KiB + 1: over the per-frame cap
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, huge, sizeof huge, 0, true, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, nullptr, FRAME, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, rbuf, sizeof rbuf, &rlen, &rts, &rkf) == false);
  CT_CHECK(ccord_transport_poll_stream_pcm(fx.t, mono, FRAME) == 0);

  // 7b. Bring up voice + concurrent broadcast (tx) and viewer (rx).
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":70,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"rtc_server_id\":\"9930\",\"rtc_channel_id\":\"2222\"}}"));
  ccord_net_event ev;
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_STREAM_SIGNAL, &ev, 4000));
  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":71,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9931\",\"rtc_channel_id\":\"2222\"}}"));
  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":72,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte_media(&m, &tx_gw, 'S', 4000u));
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":73,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte_media(&m, &rx_gw, 'V', 4000u));
  settle_media(&m, 30u);
  CT_CHECK(m.has_rx_client);

  // Validation with an active broadcast: empty arguments are still rejected (before the vstream sees them).
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, 0, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, nullptr, 16, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, nullptr, FRAME, &err) == CCORD_ERR_INVALID);

  // 7c. Sender H.264: one keyframe AU -> encrypted RTP on the SFU socket -> depacketized byte-identical.
  au_len = build_media_au(au, true, 3000);   // forces FU-A fragmentation
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 9000u, true, &err) == CCORD_OK);
  media_rxcap cap;
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 3u);
  ccord_rtp_h264_depack *depack = ccord_rtp_h264_depack_create();
  CT_CHECK(depack != nullptr);
  const uint8_t *out_au = nullptr;
  size_t out_len = 0;
  bool out_kf = false, done = false;
  uint32_t out_ts = 0;
  for (size_t i = 0; i < cap.count; i++) {
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    CT_CHECK(ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, cap.pkt[i], cap.len[i], plain,
                              sizeof plain, &plain_len, &view, &err) == CCORD_OK);
    CT_CHECK(view.payload_type == CCORD_VSTREAM_H264_PT_DEFAULT);
    CT_CHECK(view.ssrc == 7101u);   // READY ssrc 7100 + 1 = the announced video SSRC
    if (ccord_rtp_h264_depack_push(depack, plain, plain_len, view.seq, view.timestamp, view.marker, &out_au,
                                   &out_len, &out_kf, &out_ts)) {
      done = true;
      break;
    }
  }
  CT_CHECK(done && out_len == au_len && memcmp(out_au, au, au_len) == 0 && out_kf && out_ts == 9000u);

  // 7d. Full UDP/RTP loopback into the viewer: poll_stream_frame returns the same AU.
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  CT_CHECK(wait_media_frame(&m, rbuf, sizeof rbuf, &rlen, &rts, &rkf, 4000u));
  CT_CHECK(rlen == au_len && memcmp(rbuf, au, au_len) == 0 && rkf && rts == 9000u);
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, rbuf, sizeof rbuf, &rlen, &rts, &rkf) == false);

  // 7e. Oversized heads: two ~4 KiB AUs settle at the FIFO head, then a small AU. ONE poll with a 128-byte
  //     buffer must skip BOTH oversized heads and return the small AU (never stall).
  uint8_t big[8192], small[256];
  size_t big_len = build_media_au(big, true, 4000);
  size_t small_len = build_media_au(small, false, 40);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, big, big_len, 20000u, true, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 3u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  settle_media(&m, 150u);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, big, big_len, 21000u, true, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 3u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  settle_media(&m, 150u);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, small, small_len, 30000u, false, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 1u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  settle_media(&m, 150u);
  uint8_t sbuf[128];
  size_t slen;
  uint32_t sts;
  bool skf;
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, sbuf, sizeof sbuf, &slen, &sts, &skf) == true);
  CT_CHECK(slen == small_len && memcmp(sbuf, small, small_len) == 0 && sts == 30000u && !skf);
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, sbuf, sizeof sbuf, &slen, &sts, &skf) == false);

  // 7f. FIFO eviction: 9 AUs into the 8-slot FIFO -> the oldest is evicted, frames 2..9 come out in order.
  for (int i = 1; i <= 9; i++) {
    au_len = build_media_au(au, true, 40);
    CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, (uint32_t)(40000 + i * 1000), true, &err) ==
             CCORD_OK);
  }
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 9u);
  // The gateway video RX keeps only the latest complete AU per pump, so feed one packet per pump to deliver
  // all nine access units to on_frame individually (batching would surface just the last frame).
  for (size_t i = 0; i < cap.count; i++) {
    media_rxcap one;
    one.count = 1;
    memcpy(one.pkt[0], cap.pkt[i], cap.len[i]);
    one.len[0] = cap.len[i];
    CT_CHECK(loop_media_to_viewer(&m, &one, 7777u) == 1u);
    settle_media(&m, 15u);
  }
  for (int i = 2; i <= 9; i++) {
    CT_CHECK(ccord_transport_poll_stream_frame(fx.t, rbuf, sizeof rbuf, &rlen, &rts, &rkf) == true);
    CT_CHECK(rts == (uint32_t)(40000 + i * 1000) && rkf);
  }
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, rbuf, sizeof rbuf, &rlen, &rts, &rkf) == false);

  // 7g. Sender stream audio: a partial chunk emits nothing; each complete 960-sample chunk emits one Opus
  //     packet (PT 120 on the READY audio SSRC) with ts48k advancing by 960.
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, 500, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 60) == 0u);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono + 500, FRAME * 3 - 500, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) == 3u);
  int opus_err = OPUS_OK;
  OpusDecoder *mdec = opus_decoder_create(48000, 1, &opus_err);
  CT_CHECK(mdec != nullptr && opus_err == OPUS_OK);
  for (size_t i = 0; i < cap.count; i++) {
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    CT_CHECK(ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, cap.pkt[i], cap.len[i], plain,
                              sizeof plain, &plain_len, &view, &err) == CCORD_OK);
    CT_CHECK(view.payload_type == CCORD_VSTREAM_AUDIO_PT);
    CT_CHECK(view.ssrc == 7100u);
    CT_CHECK(view.timestamp == (uint32_t)(i * FRAME));
    int16_t dec[FRAME];
    int n = opus_decode(mdec, plain, (opus_int32)plain_len, dec, (int)FRAME, 0);
    CT_CHECK(n == (int)FRAME);
    CT_CHECK(ccord_pcm_rms_dbfs(dec, FRAME, 1) > -30.0f);
  }

  // 7h. Opus -> PCM RX loopback: the viewer decodes the stream audio into the mono ring.
  CT_CHECK(loop_media_to_viewer(&m, &cap, 8888u) == cap.count);
  int16_t rout[FRAME * 3];
  CT_CHECK(wait_media_pcm(&m, rout, FRAME * 3, 4000u) == FRAME * 3);
  CT_CHECK(ccord_transport_poll_stream_pcm(fx.t, rout, FRAME) == 0u);   // exactly 2880 samples, no surplus
  CT_CHECK(ccord_pcm_rms_dbfs(rout, FRAME * 3, 1) > -30.0f);
  size_t zc = 0;
  for (size_t i = 1; i < FRAME * 3; i++) {
    if ((rout[i - 1] < 0) != (rout[i] < 0)) zc++;
  }
  CT_CHECK(zc >= 40u && zc <= 70u);   // ~53 zero crossings for 440 Hz over 60 ms
  // Partial drain: 100 samples now, the rest of the chunk next.
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) == 1u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 8888u) == 1u);
  int16_t part[FRAME];
  CT_CHECK(wait_media_pcm(&m, part, 100, 4000u) == 100u);
  CT_CHECK(wait_media_pcm(&m, part + 100, FRAME - 100, 4000u) == FRAME - 100);

  // 7i. Clearing: stop_watching wipes queued viewer frames + PCM; stop_stream rejects further sender media.
  au_len = build_media_au(au, true, 40);
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 60000u, true, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 1u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  CT_CHECK(wait_media_frame(&m, rbuf, au_len, &rlen, &rts, &rkf, 4000u));   // exact-fit buffer (cap == len)
  CT_CHECK(rts == 60000u && rlen == au_len);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) == 1u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 8888u) == 1u);
  int16_t ten[10];
  CT_CHECK(wait_media_pcm(&m, ten, 10, 4000u) == 10u);
  // Queue one more frame + audio chunk (mixed capture loops back both), then stop watching: both must be wiped.
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 61000u, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 2u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  settle_media(&m, 150u);
  CT_CHECK(ccord_transport_stop_watching(fx.t, GUILD, CHAN, USER, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte_media(&m, &rx_gw, 'X', 500u));
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, rbuf, sizeof rbuf, &rlen, &rts, &rkf) == false);
  CT_CHECK(ccord_transport_poll_stream_pcm(fx.t, rout, FRAME) == 0u);
  // The broadcast survived stop_watching; stop_stream then rejects sender media.
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 62000u, true, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 1u);
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte_media(&m, &tx_gw, 'X', 500u));
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 0, true, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_ERR_INVALID);

  // 7j. Restarting the broadcast resets the stream-audio timestamp to 0.
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":74,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9932\",\"rtc_channel_id\":\"2222\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":75,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte_media(&m, &tx_gw, 'S', 4000u));
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) == 1u);
  {
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    CT_CHECK(ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, cap.pkt[0], cap.len[0], plain,
                              sizeof plain, &plain_len, &view, &err) == CCORD_OK);
    CT_CHECK(view.payload_type == CCORD_VSTREAM_AUDIO_PT && view.timestamp == 0u);
  }

  opus_decoder_destroy(mdec);
  ccord_rtp_h264_depack_destroy(depack);
  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, nullptr, vg_ev, FG_MAX_EVENTS, nullptr);
  (void)stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  (void)stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);
}

// ---- 8. Go Live stop_stream carries stream_key and never drops the active voice call -----------------------
//
// OP 19 must always carry Discord's stream_key. A bare {guild_id,channel_id} delete is rejected with a 4002
// gateway close whose reconnect wipes voice, so these tests prove the well-formed payload AND that the voice
// session stays connected (PCM still flows) with no OP 4 disconnect, for both guild and DM-call Go Live.

static size_t count_op4_null(const fg_event *ev, size_t n)
{
  size_t c = 0;
  for (size_t i = 0; i < n; i++) {
    if (strstr(ev[i].text, "\"op\":4") != nullptr && strstr(ev[i].text, "\"channel_id\":null") != nullptr) c++;
  }
  return c;
}

static void test_go_live_guild_stop_keeps_voice(void)
{
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 7100u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 7200u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));

  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":80,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9940\",\"rtc_channel_id\":\"2222\"}}"));
  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":81,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  // End the broadcast: OP 19 by stream_key, voice must survive, no OP 4 disconnect.
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));

  int16_t loud[FRAME * CH];
  double phase = 0.0;
  fill_sine_stereo(loud, 12000.0, 440.0, &phase);
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);
  g_clock_offset += 25u;
  ccord_net_event ev;
  while (ccord_transport_poll(fx.t, &ev)) {}
  rtp_cap cap;
  CT_CHECK(drain_udp_rtp(&fx, &cap, 30) >= 1);

  (void)drain_ev(fx.t, 40u);
  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  size_t ng = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, nullptr);
  (void)stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  (void)stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);

  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":18", "\"type\":\"guild\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":19", "\"stream_key\":\"guild:1111:2222:42\""));
  CT_CHECK(count_op4_null(gw_ev, ng) == 0);
}

static void test_go_live_dm_stop_keeps_voice(void)
{
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 7300u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 7400u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // DM-call Go Live: voice_guild is the CCORD_DM_GUILD_ID sentinel, never a real snowflake.
  CT_CHECK(ccord_transport_call_action(fx.t, DM_CHAN, CCORD_CALL_JOIN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, CCORD_DM_GUILD_ID, DM_CHAN, false));

  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, CCORD_DM_GUILD_ID, DM_CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":82,\"d\":{\"stream_key\":\"call:4444:42\","
                   "\"rtc_server_id\":\"9941\",\"rtc_channel_id\":\"4444\"}}"));
  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":83,\"d\":{\"stream_key\":\"call:4444:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  CT_CHECK(ccord_transport_stop_stream(fx.t, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));

  int16_t loud[FRAME * CH];
  double phase = 0.0;
  fill_sine_stereo(loud, 12000.0, 440.0, &phase);
  CT_CHECK(ccord_transport_push_voice_pcm(fx.t, loud, FRAME, &err) == CCORD_OK);
  g_clock_offset += 25u;
  ccord_net_event ev;
  while (ccord_transport_poll(fx.t, &ev)) {}
  rtp_cap cap;
  CT_CHECK(drain_udp_rtp(&fx, &cap, 30) >= 1);

  (void)drain_ev(fx.t, 40u);
  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  size_t ng = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, nullptr);
  (void)stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  (void)stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);

  // DM OP 18 opens with type "call" + guild_id null; OP 19 closes by call stream_key; no OP 4 disconnect.
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":18", "\"type\":\"call\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":18", "\"guild_id\":null"));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":19", "\"stream_key\":\"call:4444:42\""));
  CT_CHECK(count_op4_null(gw_ev, ng) == 0);
}

static void test_go_live_reopen_on_server_update(void)
{
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 7500u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 7600u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));

  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":84,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9942\",\"rtc_channel_id\":\"2222\"}}"));

  // First credentials point at a closed port: ccord_vstream_open fails synchronously and latches attempted.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":85,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"ws://127.0.0.1:1\"}}"));
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &tx_gw, 60u));

  // A fresh STREAM_SERVER_UPDATE with a reachable endpoint must clear the latch and re-open the media stream.
  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":86,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));

  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, nullptr, vg_ev, FG_MAX_EVENTS, nullptr);
  size_t ntx = stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  (void)stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);
  // Exactly one successful sender identify: the failed open never connected, the re-open did.
  CT_CHECK(fg_has_substr(tx_ev, ntx, 0, "S_ID streams=1", "\"server_id\":\"9942\""));
  CT_CHECK(!fg_has_substr(tx_ev, ntx, 1, "S_ID", nullptr));
}

// ---- 9. Standalone voice camera H.264 TX, OP 21 ping, and viewer_ids fallback ------------------------------

static void test_voice_camera_tx_and_stream_ping(void)
{
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 7700u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 7800u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));

  // Enable standalone camera -> voice gateway sends OP 12 active:true and accepts H.264 Annex-B frames.
  CT_CHECK(ccord_transport_set_voice_camera(fx.t, GUILD, true, &err) == CCORD_OK);
  (void)drain_ev(fx.t, 40u);
  uint8_t au[2048];
  size_t au_len = build_media_au(au, true, 120);
  CT_CHECK(ccord_transport_send_voice_video(fx.t, au, au_len, 9000u, true, &err) == CCORD_OK);
  media_rxcap vcap;
  CT_CHECK(drain_sfu_media(fx.udp_fd, &vcap, 200) >= 1u);
  {
    uint8_t plain[2048];
    size_t plain_len = 0;
    ccord_rtp_view view;
    CT_CHECK(ccord_voice_open(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, k_key, vcap.pkt[0], vcap.len[0], plain,
                              sizeof plain, &plain_len, &view, &err) == CCORD_OK);
    CT_CHECK(view.payload_type == CCORD_VSTREAM_H264_PT_DEFAULT);
    CT_CHECK(view.ssrc == TX_SSRC + 1u);
  }
  CT_CHECK(ccord_transport_set_voice_camera(fx.t, GUILD, false, &err) == CCORD_OK);

  // STREAM_CREATE with viewer_ids fallback (no explicit viewers integer) populates viewer_count == 2.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":90,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"rtc_server_id\":\"9950\",\"rtc_channel_id\":\"2222\",\"viewer_ids\":[\"101\",\"102\"]}}"));
  ccord_net_event ev;
  CT_CHECK(wait_for_ev(fx.t, CCORD_NET_STREAM_SIGNAL, &ev, 4000));
  CT_CHECK(ev.stream_signal != nullptr && ev.stream_signal->viewer_count == 2);

  // Active stream emits OP 21 STREAM_PING after advancing > 5000 ms.
  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":91,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9951\",\"rtc_channel_id\":\"2222\"}}"));
  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":92,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'S', 4000u));
  g_clock_offset += 5100u;
  (void)drain_ev(fx.t, 40u);
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte(&fx, &tx_gw, &rx_gw, &tx_gw, 'X', 500u));

  // Switching voice channel while streaming sends OP 19 for the active stream key ("guild:1111:2223:42"),
  // and leaving voice while streaming sends OP 19 for the active stream key ("guild:1111:2224:42").
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN + 1u, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN + 1u, &scfg, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN + 2u, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN + 2u, &scfg, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_leave_voice(fx.t, GUILD, &err) == CCORD_OK);
  (void)drain_ev(fx.t, 40u);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);
  (void)stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  (void)stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);

  CT_CHECK(fg_has_substr(vg_ev, nv, 0, "VG_TX", "\"active\":true"));
  CT_CHECK(fg_has_substr(vg_ev, nv, 0, "VG_TX", "\"video_ssrc\":0"));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"self_stream\":true"));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"self_stream\":false"));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":19", "\"stream_key\":\"guild:1111:2222:42\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":19", "\"stream_key\":\"guild:1111:2223:42\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":19", "\"stream_key\":\"guild:1111:2224:42\""));
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":21", "\"stream_key\":\"guild:1111:2222:42\""));
}

// ---- 10. Switching watched streams sends OP 19(old) -> OP 20(new) and resets rx slot -----------------------

static long find_gw_op_stream(const fg_event *ev, size_t n, size_t start_idx, const char *op_pat, const char *sk_pat)
{
  for (size_t i = start_idx; i < n; i++) {
    if (strstr(ev[i].text, op_pat) != nullptr && strstr(ev[i].text, sk_pat) != nullptr) return (long)i;
  }
  return -1;
}

static size_t count_gw_op_stream(const fg_event *ev, size_t n, const char *op_pat, const char *sk_pat)
{
  size_t c = 0;
  for (size_t i = 0; i < n; i++) {
    if (strstr(ev[i].text, op_pat) != nullptr && strstr(ev[i].text, sk_pat) != nullptr) c++;
  }
  return c;
}

static void test_go_live_switch_watched_streams(void)
{
  const ccord_id PEER_A = 3333u;
  const ccord_id PEER_B = 3334u;
  stream_gw_fx tx_gw, rx_gw;
  CT_CHECK(stream_gw_fx_start(&tx_gw, 7900u, -1));
  CT_CHECK(stream_gw_fx_start(&rx_gw, 8000u, -1));
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  media_fx m = {.fx = &fx, .tx_gw = &tx_gw, .rx_gw = &rx_gw, .has_rx_client = false, .rx_nonce = 0};
  ccord_error err;

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  CT_CHECK(establish_voice(&fx, GUILD, CHAN, false));

  // Open a sender stream so we can synthesize real encrypted H.264 + Opus RTP packets to loop back into rx_gw.
  ccord_stream_config scfg = {.resolution = CCORD_STREAM_RES_720P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_transport_start_stream(fx.t, GUILD, CHAN, &scfg, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":100,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                   "\"rtc_server_id\":\"9960\",\"rtc_channel_id\":\"2222\"}}"));
  char ssu[384];
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":101,\"d\":{\"stream_key\":\"guild:1111:2222:42\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 tx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte_media(&m, &tx_gw, 'S', 4000u));

  // Two concurrent remote broadcasts A (3333) and B (3334) exist in the same voice channel.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":102,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"rtc_server_id\":\"9961\",\"rtc_channel_id\":\"2222\"}}"));
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_CREATE\",\"s\":103,\"d\":{\"stream_key\":\"guild:1111:2222:3334\","
                   "\"rtc_server_id\":\"9962\",\"rtc_channel_id\":\"2222\"}}"));
  settle_media(&m, 30u);

  // Watch stream A -> opens viewer vstream on rx_gw.
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, PEER_A, &err) == CCORD_OK);
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":104,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte_media(&m, &rx_gw, 'V', 4000u));
  settle_media(&m, 40u);
  CT_CHECK(m.has_rx_client);

  // Watching the exact same stream key again is an idempotent no-op (vstream stays open, no extra OP 19/20).
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, PEER_A, &err) == CCORD_OK);
  CT_CHECK(stream_gw_quiet_for(&fx, &tx_gw, &rx_gw, &rx_gw, 25u));

  // Feed an H.264 keyframe + Opus chunk into A's rx ring and leave a second frame + audio chunk queued.
  uint8_t au[2048], rbuf[2048];
  size_t au_len = build_media_au(au, true, 64), rlen = 0;
  uint32_t rts = 0;
  bool rkf = false;
  media_rxcap cap;
  int16_t mono[FRAME];
  double phase = 0.0;
  for (size_t i = 0; i < FRAME; i++) {
    mono[i] = (int16_t)(10000.0 * sin(phase));
    phase += 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;
  }
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 10000u, true, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 1u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  CT_CHECK(wait_media_frame(&m, rbuf, sizeof rbuf, &rlen, &rts, &rkf, 4000u));
  CT_CHECK(rts == 10000u && rkf);

  // Queue stale media on A before switching directly to B.
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 13000u, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 2u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 7777u) == cap.count);
  settle_media(&m, 120u);

  // Direct switch A -> B: closes A's rx vstream, wipes A's queued frames + PCM, sends OP 19(A) then OP 20(B).
  m.has_rx_client = false;
  CT_CHECK(ccord_transport_watch_stream(fx.t, GUILD, CHAN, PEER_B, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte_media(&m, &rx_gw, 'X', 500u));
  int16_t rout[FRAME];
  CT_CHECK(ccord_transport_poll_stream_frame(fx.t, rbuf, sizeof rbuf, &rlen, &rts, &rkf) == false);
  CT_CHECK(ccord_transport_poll_stream_pcm(fx.t, rout, FRAME) == 0u);

  // A's delayed viewer-unsubscribe STREAM_DELETE must NOT cancel B's active watch slot.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"STREAM_DELETE\",\"s\":105,\"d\":{\"stream_key\":\"guild:1111:2222:3333\","
                   "\"reason\":\"stream_stopped\"}}"));
  (void)snprintf(ssu, sizeof ssu,
                 "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"s\":106,\"d\":{\"stream_key\":\"guild:1111:2222:3334\","
                 "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 rx_gw.srv.url);
  CT_CHECK(gw_emit(&fx, ssu));
  CT_CHECK(wait_stream_byte_media(&m, &rx_gw, 'V', 4000u));
  settle_media(&m, 40u);
  CT_CHECK(m.has_rx_client);

  // Media on B flows cleanly after the switch and Opus decoder reset.
  CT_CHECK(ccord_transport_send_stream_frame(fx.t, au, au_len, 25000u, true, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_send_stream_pcm(fx.t, mono, FRAME, &err) == CCORD_OK);
  CT_CHECK(drain_sfu_media(tx_gw.udp_fd, &cap, 500) >= 2u);
  CT_CHECK(loop_media_to_viewer(&m, &cap, 8888u) == cap.count);
  CT_CHECK(wait_media_frame(&m, rbuf, sizeof rbuf, &rlen, &rts, &rkf, 4000u));
  CT_CHECK(rts == 25000u && rkf);
  CT_CHECK(wait_media_pcm(&m, rout, FRAME, 4000u) == FRAME);

  CT_CHECK(ccord_transport_stop_watching(fx.t, GUILD, CHAN, PEER_B, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte_media(&m, &rx_gw, 'X', 500u));
  CT_CHECK(ccord_transport_stop_stream(fx.t, GUILD, &err) == CCORD_OK);
  CT_CHECK(wait_stream_byte_media(&m, &tx_gw, 'X', 500u));

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS], tx_ev[FG_MAX_EVENTS], rx_ev[FG_MAX_EVENTS];
  size_t ng = 0, nrx = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, nullptr);
  (void)stream_gw_fx_stop(&tx_gw, tx_ev, FG_MAX_EVENTS);
  nrx = stream_gw_fx_stop(&rx_gw, rx_ev, FG_MAX_EVENTS);

  long w_a = find_gw_op_stream(gw_ev, ng, 0, "\"op\":20", "\"stream_key\":\"guild:1111:2222:3333\"");
  CT_CHECK(w_a >= 0);
  CT_CHECK(count_gw_op_stream(gw_ev, ng, "\"op\":20", "\"stream_key\":\"guild:1111:2222:3333\"") == 1u);
  long d_a = w_a >= 0 ? find_gw_op_stream(gw_ev, ng, (size_t)w_a + 1u, "\"op\":19",
                                          "\"stream_key\":\"guild:1111:2222:3333\"")
                      : -1;
  CT_CHECK(d_a > w_a);
  CT_CHECK(count_gw_op_stream(gw_ev, ng, "\"op\":19", "\"stream_key\":\"guild:1111:2222:3333\"") == 1u);
  long w_b = d_a >= 0 ? find_gw_op_stream(gw_ev, ng, (size_t)d_a + 1u, "\"op\":20",
                                          "\"stream_key\":\"guild:1111:2222:3334\"")
                      : -1;
  CT_CHECK(w_b > d_a);
  CT_CHECK(fg_has_substr(rx_ev, nrx, 0, "S_ID streams=0", "\"server_id\":\"9961\""));
  CT_CHECK(fg_has_substr(rx_ev, nrx, 1, "S_ID streams=0", "\"server_id\":\"9962\""));
}

// ---- 11. Voice reconnect ladder --------------------------------------

typedef struct lk_seen {
  unsigned reconnecting, connected, failed, left_state, action_voice;
  uint32_t last_attempt, last_retry_ms;
  uint64_t connected_at_ms;
  char action_text[96];
  char fail_text[96];
} lk_seen;

static void lk_note(lk_seen *s, const ccord_net_event *ev)
{
  if (ev->type == CCORD_NET_VOICE_LINK) {
    if (ev->voice_link == CCORD_VLINK_RECONNECTING) {
      s->reconnecting++;
      s->last_attempt = ev->voice_link_attempt;
      s->last_retry_ms = ev->voice_link_retry_ms;
    } else if (ev->voice_link == CCORD_VLINK_CONNECTED) {
      s->connected++;
      s->connected_at_ms = ccord_clock_ms();
    } else if (ev->voice_link == CCORD_VLINK_FAILED) {
      s->failed++;
      (void)snprintf(s->fail_text, sizeof s->fail_text, "%s", ev->text ? ev->text : "");
    }
  } else if (ev->type == CCORD_NET_VOICE_STATE_UPDATE && ev->user_id == SELF && ev->channel_id == CCORD_ID_NONE) {
    s->left_state++;
  } else if (ev->type == CCORD_NET_ACTION_FAILED && ev->action == CCORD_ACTION_VOICE) {
    s->action_voice++;
    (void)snprintf(s->action_text, sizeof s->action_text, "%s", ev->text ? ev->text : "");
  }
}

// Polls the transport (and serves the SFU's IP discovery) for `ms`, recording the voice-link events.
static void lk_pump(live_fx *fx, uint64_t ms, lk_seen *s)
{
  uint64_t end = ccord_clock_ms() + ms;
  while (ccord_clock_ms() < end) {
    ccord_net_event ev;
    while (ccord_transport_poll(fx->t, &ev)) lk_note(s, &ev);
    pump_udp_discovery(fx);
    usleep(2000);
  }
}

static bool lk_wait(live_fx *fx, lk_seen *s, const unsigned *counter, unsigned target, uint64_t timeout_ms)
{
  uint64_t end = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < end) {
    lk_pump(fx, 5u, s);
    if (*counter >= target) return true;
  }
  return false;
}

static bool vdrop_close(live_fx *fx, unsigned code)
{
  char cmd[32];
  int n = snprintf(cmd, sizeof cmd, "CLOSE %u\n", code);
  return n > 0 && write(fx->vcmd_pipe[1], cmd, (size_t)n) == (ssize_t)n;
}

static bool lk_join(live_fx *fx, int refuse_from)
{
  if (!live_fx_start_ex(fx, true, refuse_from)) return false;
  ccord_error err;
  return ccord_transport_join_voice(fx->t, GUILD, CHAN, &err) == CCORD_OK && establish_voice(fx, GUILD, CHAN, false);
}

// Echo of our own join, as Discord sends it after an OP 4 / after a rejoin: new voice state plus new credentials.
static bool lk_emit_join(live_fx *fx, const char *session_id, const char *token)
{
  char vstate[320], vserver[320];
  (void)snprintf(vstate, sizeof vstate,
                 "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":\"%llu\","
                 "\"channel_id\":\"%llu\",\"user_id\":\"%llu\",\"session_id\":\"%s\"}}",
                 fx->gw_seq++, (unsigned long long)GUILD, (unsigned long long)CHAN, (unsigned long long)SELF,
                 session_id);
  (void)snprintf(vserver, sizeof vserver,
                 "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":\"%llu\","
                 "\"channel_id\":\"%llu\",\"token\":\"%s\",\"endpoint\":\"%s\"}}",
                 fx->gw_seq++, (unsigned long long)GUILD, (unsigned long long)CHAN, token, fx->voice_gw.url);
  return gw_emit(fx, vstate) && gw_emit(fx, vserver);
}

static size_t count_op4_join(const fg_event *ev, size_t n)
{
  size_t c = 0;
  for (size_t i = 0; i < n; i++) {
    if (strstr(ev[i].text, "\"op\":4") != nullptr && strstr(ev[i].text, "\"channel_id\":\"2222\"") != nullptr) c++;
  }
  return c;
}

static void lk_stop(live_fx *fx, fg_event *gw_ev, size_t *ngw, fg_event *vg_ev, size_t *nvg)
{
  fd_hit hits[FD_MAX_HITS];
  live_fx_stop(fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, ngw, vg_ev, FG_MAX_EVENTS, nvg);
}

// A resumable failure (4015: voice server crashed) is handled by the voice gateway: the call stays joined, the
// transport reports RECONNECTING then CONNECTED, never leaves, never re-identifies.
static void test_voice_reconnect_resumable_keeps_call(void)
{
  live_fx fx;
  lk_seen s = {0};
  CT_CHECK(lk_join(&fx, -1));
  CT_CHECK(vdrop_close(&fx, 4015));
  CT_CHECK(lk_wait(&fx, &s, &s.reconnecting, 1, 3000));
  CT_CHECK(lk_wait(&fx, &s, &s.connected, 1, 4000));
  lk_pump(&fx, 100, &s);
  CT_CHECK(s.left_state == 0 && s.failed == 0 && s.action_voice == 0);
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  lk_stop(&fx, gw_ev, &ng, vg_ev, &nv);
  CT_CHECK(fg_count(vg_ev, nv, 0, "IDENTIFY") == 1);   // the original session ...
  CT_CHECK(fg_count(vg_ev, nv, 1, "RESUME") == 1);     // ... was resumed, not replaced
  CT_CHECK(fg_count(vg_ev, nv, 1, "IDENTIFY") == 0);
  CT_CHECK(count_op4_null(gw_ev, ng) == 0);            // no OP 4 leave
  CT_CHECK(count_op4_join(gw_ev, ng) == 1);            // no OP 4 rejoin either
}

// 4005 -> REIDENTIFY with the same credentials (no OP 4). A network-change event during the next backoff runs that
// attempt at once instead of after the delay, and the camera table is rebuilt on every new gateway.
static void test_voice_reconnect_reidentify_and_link_event(void)
{
  live_fx fx;
  lk_seen s = {0};
  CT_CHECK(lk_join(&fx, -1));
  CT_CHECK(vdrop_close(&fx, 4005));
  CT_CHECK(lk_wait(&fx, &s, &s.reconnecting, 1, 3000));
  CT_CHECK(s.last_attempt == 1 && s.last_retry_ms == 250u);
  CT_CHECK(lk_wait(&fx, &s, &s.connected, 1, 4000));
  CT_CHECK(s.left_state == 0);
  {
    ccord_camera_rx cams[4];
    uint64_t end = ccord_clock_ms() + 2000u;
    size_t n = 0;
    while (ccord_clock_ms() < end && (n = ccord_transport_camera_senders(fx.t, cams, 4)) == 0) lk_pump(&fx, 5u, &s);
    CT_CHECK(n == 1 && cams[0].user_id == USER);   // camera receive was re-enabled on the replacement gateway
  }

  // Second drop inside the same outage window: attempt 2 backs off at least ~800 ms.
  CT_CHECK(vdrop_close(&fx, 4005));
  CT_CHECK(lk_wait(&fx, &s, &s.reconnecting, 2, 3000));
  CT_CHECK(s.last_attempt == 2 && s.last_retry_ms >= 800u);
  lk_pump(&fx, 100, &s);
  CT_CHECK(s.connected == 1);   // still backing off
  ccord_error err;
  uint64_t nudged = ccord_clock_ms();
  CT_CHECK(ccord_transport_network_event(fx.t, CCORD_LINK_CHANGED, &err) == CCORD_OK);
  CT_CHECK(lk_wait(&fx, &s, &s.connected, 2, 1500));
  CT_CHECK(s.connected_at_ms - nudged < 700u);   // far inside the backoff delay
  CT_CHECK(s.left_state == 0 && s.failed == 0);

  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  lk_stop(&fx, gw_ev, &ng, vg_ev, &nv);
  CT_CHECK(fg_count(vg_ev, nv, 1, "IDENTIFY") == 1 && fg_count(vg_ev, nv, 2, "IDENTIFY") == 1);
  CT_CHECK(fg_count(vg_ev, nv, 1, "RESUME") == 0);
  CT_CHECK(count_op4_null(gw_ev, ng) == 0 && count_op4_join(gw_ev, ng) == 1);
}

// 4006 -> REJOIN: OP 4 leave then OP 4 join for the same channel; the leave echo is swallowed (the call goes on);
// the new credentials start a new gateway and the outage ends with CONNECTED.
static void test_voice_reconnect_rejoin(void)
{
  live_fx fx;
  lk_seen s = {0};
  CT_CHECK(lk_join(&fx, -1));
  CT_CHECK(vdrop_close(&fx, 4006));
  CT_CHECK(lk_wait(&fx, &s, &s.reconnecting, 1, 3000));
  CT_CHECK(s.last_attempt == 1 && s.last_retry_ms >= 1000u);   // REJOIN waits at least 1 s
  lk_pump(&fx, 100, &s);
  g_clock_offset += 1500u;   // the backoff is over
  lk_pump(&fx, 300, &s);

  char leave_echo[256];
  (void)snprintf(leave_echo, sizeof leave_echo,
                 "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":\"%llu\","
                 "\"channel_id\":null,\"user_id\":\"%llu\",\"session_id\":\"vs-guild\"}}",
                 fx.gw_seq++, (unsigned long long)GUILD, (unsigned long long)SELF);
  CT_CHECK(gw_emit(&fx, leave_echo));
  lk_pump(&fx, 150, &s);
  CT_CHECK(s.left_state == 0);   // swallowed: the store never sees us leave
  CT_CHECK(lk_emit_join(&fx, "vs-guild-2", "REDACTED_TEST_TOKEN"));
  CT_CHECK(lk_wait(&fx, &s, &s.connected, 1, 4000));
  CT_CHECK(s.left_state == 0 && s.failed == 0 && s.action_voice == 0);

  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  lk_stop(&fx, gw_ev, &ng, vg_ev, &nv);
  CT_CHECK(count_op4_null(gw_ev, ng) == 1);   // exactly one leave ...
  CT_CHECK(count_op4_join(gw_ev, ng) == 2);   // ... between the original join and the rejoin
  size_t leave_i = SIZE_MAX, join2_i = SIZE_MAX, joins = 0;
  for (size_t i = 0; i < ng; i++) {
    if (strstr(gw_ev[i].text, "\"op\":4") == nullptr) continue;
    if (strstr(gw_ev[i].text, "\"channel_id\":null") != nullptr) leave_i = i;
    else if (strstr(gw_ev[i].text, "\"channel_id\":\"2222\"") != nullptr && ++joins == 2) join2_i = i;
  }
  CT_CHECK(leave_i != SIZE_MAX && join2_i != SIZE_MAX && leave_i < join2_i);
  CT_CHECK(fg_count(vg_ev, nv, 1, "IDENTIFY") == 1);
}

// 4014 -> AWAIT_MAIN: no voice attempt and no OP 4 at all; without a verdict from the main gateway the call is given
// up locally after the grace period.
static void test_voice_reconnect_4014_gives_up_locally(void)
{
  live_fx fx;
  lk_seen s = {0};
  CT_CHECK(lk_join(&fx, -1));
  CT_CHECK(vdrop_close(&fx, 4014));
  CT_CHECK(lk_wait(&fx, &s, &s.reconnecting, 1, 3000));
  CT_CHECK(s.last_attempt == 0 && s.last_retry_ms == 3000u);
  lk_pump(&fx, 200, &s);
  CT_CHECK(s.failed == 0 && s.left_state == 0);   // still waiting for the main gateway
  g_clock_offset += 3100u;
  CT_CHECK(lk_wait(&fx, &s, &s.failed, 1, 3000));
  lk_pump(&fx, 200, &s);
  CT_CHECK(s.failed == 1 && s.left_state == 1 && s.action_voice == 1);
  CT_CHECK(strcmp(s.action_text, "you were disconnected") == 0 && strcmp(s.fail_text, "you were disconnected") == 0);
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  lk_stop(&fx, gw_ev, &ng, vg_ev, &nv);
  CT_CHECK(count_op4_null(gw_ev, ng) == 0);   // no OP 4 leave: the user may be in another channel now
  CT_CHECK(fg_connections(vg_ev, nv) == 1);   // and no new voice connection
}

// 4022 -> AWAIT_MAIN, and the main gateway answers with new credentials in time: the wait is cancelled and the call
// reconnects to the new server.
static void test_voice_reconnect_4022_new_server_cancels_wait(void)
{
  live_fx fx;
  lk_seen s = {0};
  CT_CHECK(lk_join(&fx, -1));
  CT_CHECK(vdrop_close(&fx, 4022));
  CT_CHECK(lk_wait(&fx, &s, &s.reconnecting, 1, 3000));
  char vserver[320];
  (void)snprintf(vserver, sizeof vserver,
                 "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"s\":%d,\"d\":{\"guild_id\":\"%llu\","
                 "\"channel_id\":\"%llu\",\"token\":\"REDACTED_TEST_TOKEN_2\",\"endpoint\":\"%s\"}}",
                 fx.gw_seq++, (unsigned long long)GUILD, (unsigned long long)CHAN, fx.voice_gw.url);
  CT_CHECK(gw_emit(&fx, vserver));
  CT_CHECK(lk_wait(&fx, &s, &s.connected, 1, 4000));
  g_clock_offset += 4000u;   // past the 3 s grace: nothing may give up now
  lk_pump(&fx, 300, &s);
  CT_CHECK(s.failed == 0 && s.left_state == 0 && s.action_voice == 0);
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  lk_stop(&fx, gw_ev, &ng, vg_ev, &nv);
  CT_CHECK(fg_count(vg_ev, nv, 1, "IDENTIFY") == 1);
  CT_CHECK(count_op4_null(gw_ev, ng) == 0);
}

// Every reconnect attempt fails: after the policy's bound the call is given up exactly once (one channel-0 state, one
// FAILED link event, one failed-action notice, one OP 4 leave) and nothing keeps retrying afterwards.
static void test_voice_reconnect_gives_up_after_budget(void)
{
  live_fx fx;
  lk_seen s = {0};
  CT_CHECK(lk_join(&fx, 1));   // connections after the first are refused
  CT_CHECK(vdrop_close(&fx, 4005));
  for (int i = 0; i < 16 && s.failed == 0; i++) {
    lk_pump(&fx, 300, &s);
    g_clock_offset += 20000u;
  }
  CT_CHECK(s.failed == 1);
  CT_CHECK(s.reconnecting >= 1);
  lk_pump(&fx, 300, &s);
  g_clock_offset += 60000u;
  lk_pump(&fx, 300, &s);
  CT_CHECK(s.failed == 1 && s.left_state == 1 && s.action_voice == 1);
  CT_CHECK(strcmp(s.action_text, "connection lost") == 0 && strcmp(s.fail_text, "connection lost") == 0);
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  lk_stop(&fx, gw_ev, &ng, vg_ev, &nv);
  CT_CHECK(count_op4_null(gw_ev, ng) == 1);   // tells the server: no ghost presence
  CT_CHECK(fg_connections(vg_ev, nv) <= 1 + (int)CCORD_VC_MAX_ATTEMPTS);
}

static void test_voice_other_users_and_mute_deafen_op4(void)
{
  live_fx fx;
  CT_CHECK(live_fx_start(&fx));
  ccord_error err;

  // Pre-join mute persists into initial OP 4 join.
  CT_CHECK(ccord_transport_update_voice_state(fx.t, CCORD_ID_NONE, true, false, &err) == CCORD_OK);

  // Another user's VOICE_STATE_UPDATE with their session_id arrives BEFORE and AFTER our own join.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":2,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":\"2222\",\"user_id\":\"3333\",\"session_id\":\"other-vs-before\"}}"));
  (void)drain_ev(fx.t, 40u);

  CT_CHECK(ccord_transport_join_voice(fx.t, GUILD, CHAN, &err) == CCORD_OK);
  // Emit self VOICE_STATE_UPDATE ("vs-self-live") then another user's VOICE_STATE_UPDATE ("other-vs-after"),
  // then VOICE_SERVER_UPDATE so voice_gateway starts with self's session_id.
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":3,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":\"2222\",\"user_id\":\"42\",\"session_id\":\"vs-self-live\","
                   "\"self_mute\":true,\"self_deaf\":false}}"));
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":4,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":\"2222\",\"user_id\":\"3333\",\"session_id\":\"other-vs-after\"}}"));
  char vserver[320];
  (void)snprintf(vserver, sizeof vserver,
                 "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"s\":5,\"d\":{\"guild_id\":\"1111\","
                 "\"channel_id\":\"2222\",\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"%s\"}}",
                 fx.voice_gw.url);
  CT_CHECK(gw_emit(&fx, vserver));

  uint64_t deadline = ccord_clock_ms() + 4000u;
  bool ready = false;
  while (ccord_clock_ms() < deadline) {
    ccord_net_event ev;
    while (ccord_transport_poll(fx.t, &ev)) {
      if (ev.type == CCORD_NET_SPEAKING && ev.user_id == USER && !ev.speaking) ready = true;
    }
    pump_udp_discovery(&fx);
    char b = 0;
    if (read(fx.vready_pipe[0], &b, 1) == 1 && ready) break;
    usleep(2000);
  }
  CT_CHECK(ready);

  // Deafen OP 4 while in voice, then simulate server move to CHAN + 1 and unmute+undeafen.
  CT_CHECK(ccord_transport_update_voice_state(fx.t, GUILD, true, true, &err) == CCORD_OK);
  CT_CHECK(gw_emit(&fx,
                   "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"s\":6,\"d\":{\"guild_id\":\"1111\","
                   "\"channel_id\":\"2223\",\"user_id\":\"42\",\"session_id\":\"vs-self-live\","
                   "\"self_mute\":true,\"self_deaf\":true}}"));
  (void)drain_ev(fx.t, 40u);
  CT_CHECK(ccord_transport_update_voice_state(fx.t, GUILD, false, false, &err) == CCORD_OK);
  (void)drain_ev(fx.t, 40u);

  fd_hit hits[FD_MAX_HITS];
  fg_event gw_ev[FG_MAX_EVENTS], vg_ev[FG_MAX_EVENTS];
  size_t ng = 0, nv = 0;
  live_fx_stop(&fx, hits, FD_MAX_HITS, nullptr, gw_ev, FG_MAX_EVENTS, &ng, vg_ev, FG_MAX_EVENTS, &nv);

  // Voice gateway IDENTIFY must carry self's session_id, never the other user's.
  CT_CHECK(fg_has_substr(vg_ev, nv, 0, "IDENTIFY", "\"session_id\":\"vs-self-live\""));
  CT_CHECK(!fg_has_substr(vg_ev, nv, 0, "IDENTIFY", "other-vs-"));
  // Join OP 4 carried pre-join self_mute=true.
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"channel_id\":\"2222\",\"self_mute\":true,\"self_deaf\":false"));
  // Deafen OP 4 sent self_mute=true, self_deaf=true.
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"channel_id\":\"2222\",\"self_mute\":true,\"self_deaf\":true"));
  // Post-move OP 4 targets the new channel ("2223").
  CT_CHECK(fg_has_substr(gw_ev, ng, 0, "\"op\":4", "\"channel_id\":\"2223\",\"self_mute\":false,\"self_deaf\":false"));
}

int main(void)
{
  test_admin_voice_status_and_stage_topic();
  test_stage_speak_and_speaker_suppression();
  test_stage_preserves_voice_flags();
  test_dm_call_lifecycle();
  test_activity_launch_gateway_session();
  test_soundboard_playback();
  test_user_volume_and_voice_config();
  test_go_live_stream_lifecycle();
  test_go_live_teardown_and_late_voice_session();
  test_go_live_media_plane();
  test_go_live_guild_stop_keeps_voice();
  test_go_live_dm_stop_keeps_voice();
  test_go_live_reopen_on_server_update();
  test_voice_camera_tx_and_stream_ping();
  test_go_live_switch_watched_streams();
  test_voice_reconnect_resumable_keeps_call();
  test_voice_reconnect_reidentify_and_link_event();
  test_voice_reconnect_rejoin();
  test_voice_reconnect_4014_gives_up_locally();
  test_voice_reconnect_4022_new_server_cancels_wait();
  test_voice_reconnect_gives_up_after_budget();
  test_voice_other_users_and_mute_deafen_op4();
  return CT_RESULT("live_calls");
}
