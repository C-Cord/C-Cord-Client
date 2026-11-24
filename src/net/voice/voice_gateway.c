// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif
// Voice gateway v8: websocket signaling, UDP media transport, IP discovery, speaking,
// audio RX/TX and Go Live video. Split out of discord_gateway.c (m5-split); the main
// Discord gateway stays there. Shared internals live in voice_internal.h.
#include <ccord/net/live_transport.h>

#include <limits.h>
#include <string.h>

#include <curl/curl.h>
#include "common/tls_internal.h"
#include <curl/websockets.h>
#if defined(__has_include) && __has_include(<opus/opus.h>)
#include <opus/opus.h>
#else
#include <opus.h>
#endif
#include <sodium.h>

#include "voice_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/os.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/net/voice_dsp.h>
#include <ccord/net/voice_stream.h>


uint64_t vg_now(const ccord_voice_gateway *g)
{
  return g->now_ms ? g->now_ms(g->clock_ud) : ccord_clock_boottime_ms();
}

// Per-user camera receive: defined in the "per-user camera receive" section below (voice_camera.h).
// Internal cross-TU forward declarations live in voice_internal.h.

// M0-6: every public entry point takes g->mtx so the app's audio pthread can push/pull PCM while
// the UI thread polls the gateway. Helpers that already run under the lock (vg_pump_udp, vg_drain,
// set_video) call the unlocked vg_* variants instead of the public wrappers.
ccord_mutex *vg_mtx(const ccord_voice_gateway *g)
{
  return g ? g->mtx : nullptr;
}

// Unlocked internals used by vg_pump_udp / vg_drain / set_video while g->mtx is already held.
// Declared in voice_internal.h as vg_set_speaking.

#define VG_VIDEO_SHRINK_THRESHOLD (128u * 1024u)
#define VG_TX_DAVE_SHRINK_THRESHOLD (512u * 1024u)

void vg_copy(char *dst, size_t cap, const char *src)
{
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}


void vg_close_udp(ccord_voice_gateway *g)
{
  if (g->udp_fd >= 0) {
    ccord_sock_close((ccord_socket_t)g->udp_fd);
    g->udp_fd = -1;
  }
  g->peer_valid = false;
  g->udp_connected = false;
  g->udp_video_buf = false;
  g->last_udp_empty_ms = 0;
  g->udp_gen++;
}

static void vg_close(ccord_voice_gateway *g)
{
  vg_close_udp(g);
  if (g->curl) {
    curl_easy_cleanup(g->curl);
    g->curl = nullptr;
  }
  g->open = false;
  g->ws_tx_busy = false;
  g->lifecycle_gen++;
  if (g->io_cond) ccord_cond_broadcast(g->io_cond);
}

// Drops only the WebSocket. UDP socket, secret, SSRCs, DAVE and the speaking/ssrc maps are kept so op 7 can resume.
static void vg_close_ws(ccord_voice_gateway *g)
{
  if (g->curl) {
    curl_easy_cleanup(g->curl);
    g->curl = nullptr;
  }
  g->open = false;
  g->ws_tx_busy = false;
  g->lifecycle_gen++;
  if (g->io_cond) ccord_cond_broadcast(g->io_cond);
  g->hello = false;
  g->heartbeat_pending = false;
  g->heartbeat_nonce = 0;
  g->frame_is_binary = false;
  ccord_buf_shrink(&g->frame, 16384u);
}


void vg_io_leave(ccord_voice_gateway *g)
{
  g->inflight_io--;
  if (g->inflight_io == 0 && g->io_cond) ccord_cond_broadcast(g->io_cond);
}

static ccord_status vg_send_ws_locked(ccord_voice_gateway *g, const void *data, size_t len,
                                      unsigned int ws_flags, ccord_error *err)
{
  if (g->destroying || !g->curl || !g->open) return ccord_error_set(err, CCORD_ERR_IO, "voice: not connected");
  uint32_t gen = g->lifecycle_gen;
  uint64_t busy_deadline = ccord_clock_boottime_ms() + 1000u;
  while (g->ws_tx_busy) {
    if (g->destroying) return ccord_error_set(err, CCORD_ERR_IO, "voice: not connected");
    uint64_t now = ccord_clock_boottime_ms();
    if (now >= busy_deadline) {
      return ccord_error_set(err, CCORD_ERR_TIMEOUT,
                             ws_flags == CURLWS_BINARY ? "voice: binary send timed out" : "voice: send timed out");
    }
    uint32_t wait_ms = (uint32_t)(busy_deadline - now);
    if (wait_ms > 20u) wait_ms = 20u;
    if (g->io_cond) {
      (void)ccord_cond_wait_timeout(g->io_cond, g->mtx, wait_ms);
    } else {
      g->inflight_io++;
      ccord_mutex_unlock(g->mtx);
      ccord_sleep_ms(wait_ms);
      ccord_mutex_lock(g->mtx);
      vg_io_leave(g);
    }
    if (g->destroying || g->lifecycle_gen != gen || !g->curl || !g->open) {
      return ccord_error_set(err, CCORD_ERR_IO, "voice: not connected");
    }
  }
  g->ws_tx_busy = true;
  const uint8_t *p = data;
  size_t sent = 0;
  ccord_status st = CCORD_OK;
  int waits = 0;
  int fast_out = 0;
  uint64_t send_deadline = ccord_clock_boottime_ms() + 1000u;
  while (sent < len) {
    size_t n = 0;
    CURLcode rc = curl_ws_send(g->curl, p + sent, len - sent, &n, 0, ws_flags);
    if (rc == CURLE_AGAIN) {
      if (g->destroying) {
        g->ws_tx_busy = false;
        if (g->io_cond) ccord_cond_broadcast(g->io_cond);
        return ccord_error_set(err, CCORD_ERR_IO, "voice: not connected");
      }
      uint64_t now = ccord_clock_boottime_ms();
      if (++waits > 1000 || now >= send_deadline) {
        st = ccord_error_set(err, CCORD_ERR_TIMEOUT,
                             ws_flags == CURLWS_BINARY ? "voice: binary send timed out" : "voice: send timed out");
        break;
      }
      int poll_ms = (int)(send_deadline - now);
      if (poll_ms > 20) poll_ms = 20;
      if (poll_ms < 1) poll_ms = 1;
      curl_socket_t sock = CURL_SOCKET_BAD;
      (void)curl_easy_getinfo(g->curl, CURLINFO_ACTIVESOCKET, &sock);
      unsigned events = (fast_out >= 2) ? CCORD_POLL_IN : CCORD_POLL_OUT;
      g->inflight_io++;
      ccord_mutex_unlock(g->mtx);
      if (sock != CURL_SOCKET_BAD) {
        (void)ccord_sock_poll1((ccord_socket_t)sock, events, poll_ms);
      } else {
        ccord_sleep_ms((uint32_t)(poll_ms < 5 ? poll_ms : 5));
      }
      uint64_t after = ccord_clock_boottime_ms();
      ccord_mutex_lock(g->mtx);
      vg_io_leave(g);
      if (events == CCORD_POLL_OUT && after == now) fast_out++;
      else fast_out = 0;
      if (g->destroying || g->lifecycle_gen != gen) {
        return ccord_error_set(err, CCORD_ERR_IO, "voice: not connected");
      }
      if (!g->curl || !g->open) {
        g->ws_tx_busy = false;
        if (g->io_cond) ccord_cond_broadcast(g->io_cond);
        return ccord_error_set(err, CCORD_ERR_IO, "voice: not connected");
      }
      continue;
    }
    if (rc != CURLE_OK || n == 0) {
      st = ccord_error_set(err, CCORD_ERR_IO,
                           ws_flags == CURLWS_BINARY ? "voice: binary send failed" : "voice: send failed");
      break;
    }
    sent += n;
    fast_out = 0;
  }
  g->ws_tx_busy = false;
  if (g->io_cond) ccord_cond_broadcast(g->io_cond);
  return st;
}

ccord_status vg_send_raw(ccord_voice_gateway *g, const char *data, size_t len, ccord_error *err)
{
  if (g->send) return g->send(g->send_ud, data, len, err);
  return vg_send_ws_locked(g, data, len, CURLWS_TEXT, err);
}

static ccord_status vg_send_bin(ccord_voice_gateway *g, const uint8_t *data, size_t len, ccord_error *err)
{
  if (g->send_bin) return g->send_bin(g->send_ud, data, len, err);
  if (g->send) {
    ccord_error_clear(err);
    return CCORD_OK;
  }
  return vg_send_ws_locked(g, data, len, CURLWS_BINARY, err);
}

static ccord_status vg_dave_send_cb(void *ud, bool binary, const uint8_t *data, size_t len, ccord_error *err)
{
  ccord_voice_gateway *g = ud;
  if (binary) return vg_send_bin(g, data, len, err);
  return vg_send_raw(g, (const char *)data, len, err);
}

static ccord_status vg_send_buf(ccord_voice_gateway *g, ccord_buf *b, ccord_error *err)
{
  ccord_status st = b->failed ? ccord_error_set(err, CCORD_ERR_RANGE, "voice: payload too large") : CCORD_OK;
  if (st == CCORD_OK) st = vg_send_raw(g, b->data, b->len, err);
  ccord_buf_free(b);
  return st;
}

static ccord_status vg_send_identify(ccord_voice_gateway *g, ccord_error *err)
{
  ccord_buf b;
  ccord_buf_init(&b, 1024);
  ccord_status st = ccord_voice_build_identify(g->server_id, g->user_id, g->session_id, g->token, &b, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  return vg_send_buf(g, &b, err);
}

ccord_status vg_send_select(ccord_voice_gateway *g, ccord_error *err)
{
  const char *addr = g->discovered_ip[0] != '\0' ? g->discovered_ip : g->ip;
  uint16_t port = g->discovered_port != 0 ? g->discovered_port : g->port;
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_status st = ccord_voice_build_select_protocol_ex(addr, port, g->mode, g->video_desc.payload_type,
                                                         g->video_desc.rtx_payload_type, &b, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  return vg_send_buf(g, &b, err);
}

static ccord_status vg_send_heartbeat(ccord_voice_gateway *g, ccord_error *err)
{
  uint64_t nonce = vg_now(g);
  ccord_buf b;
  ccord_buf_init(&b, 128);
  ccord_status st = ccord_voice_build_heartbeat_v8(nonce, g->seq_ack, &b, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  st = vg_send_buf(g, &b, err);
  if (st == CCORD_OK) {
    g->heartbeat_pending = true;
    g->heartbeat_nonce = nonce;
    g->heartbeat_sent_ms = nonce;
  }
  g->next_heartbeat = vg_now(g) + g->heartbeat_ms;
  return st;
}


static ccord_voice_gateway *vg_create_internal(const ccord_voice_gateway_config *cfg, bool video_only,
                                               ccord_error *err)
{
  ccord_error_clear(err);
  if (!cfg || cfg->server_id == CCORD_ID_NONE || cfg->user_id == CCORD_ID_NONE || !cfg->token ||
      cfg->token[0] == '\0') {
    ccord_error_set(err, CCORD_ERR_INVALID, "voice: identify fields");
    return nullptr;
  }
  ccord_voice_gateway *g = ccord_calloc(1, sizeof *g);
  if (!g) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "voice: out of memory");
    return nullptr;
  }
  g->udp_fd = -1;
  g->seq_ack = -1;
  g->ping_ms = -1;
  g->video_only = video_only;
  g->vol_gen = 1u;
  ccord_vc_init(&g->rc, ccord_clock_ms() ^ (uint64_t)(uintptr_t)g);
  if ((cfg->endpoint && strlen(cfg->endpoint) >= CCORD_VOICE_ENDPOINT_MAX) ||
      (cfg->session_id && strlen(cfg->session_id) >= GW_SESSION_MAX)) {
    ccord_free(g);
    ccord_error_set(err, CCORD_ERR_RANGE, "voice: endpoint or session id too long");
    return nullptr;
  }
  g->server_id = cfg->server_id;
  g->channel_id = cfg->channel_id ? cfg->channel_id : cfg->server_id;
  g->user_id = cfg->user_id;
  g->now_ms = cfg->now_ms;
  g->clock_ud = cfg->clock_ud;
  g->send = cfg->send;
  g->send_bin = cfg->send_bin;
  g->send_ud = cfg->send_ud;
  g->connect_timeout_ms = cfg->connect_timeout_ms ? cfg->connect_timeout_ms : CCORD_LIVE_TIMEOUT_DEFAULT_MS;
  size_t tn = strlen(cfg->token);
  if (tn > CCORD_LIVE_TOKEN_MAX) {
    ccord_free(g);
    ccord_error_set(err, CCORD_ERR_RANGE, "voice: token too long");
    return nullptr;
  }
  memcpy(g->token, cfg->token, tn + 1);
  vg_copy(g->session_id, sizeof g->session_id, cfg->session_id);
  vg_copy(g->endpoint, sizeof g->endpoint, cfg->endpoint);
  if (!cfg->send) {
    ccord_status st = ccord_voice_build_url(g->endpoint, g->url, sizeof g->url, err);
    if (st != CCORD_OK) {
      ccord_live_wipe(g->token, sizeof g->token);
      ccord_free(g);
      return nullptr;
    }
  }
  ccord_vad_init(&g->vad, -60.0f, CCORD_VAD_DEFAULT_HANGOVER_MS);
  ccord_ssrc_map_init(&g->ssrc_map);
  ccord_speaking_init(&g->speaking);
  if (ccord_dave_built()) {
    ccord_error derr;
    g->dave = ccord_dave_create(g->user_id, g->channel_id, vg_dave_send_cb, g, &derr);
  }
  g->state = CCORD_VOICE_IDLE;
  atomic_init(&g->keyframe_requested, false);
  ccord_buf_init(&g->frame, GW_MAX_FRAME);
  g->mtx = ccord_mutex_create();
  g->io_cond = ccord_cond_create();
  if (!g->mtx || !g->io_cond) {
    ccord_cond_destroy(g->io_cond);
    ccord_mutex_destroy(g->mtx);
    ccord_buf_free(&g->frame);
    if (g->dave) ccord_dave_destroy(g->dave);
    ccord_live_wipe(g->token, sizeof g->token);
    ccord_free(g);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: mutex init failed");
    return nullptr;
  }
  return g;
}

ccord_voice_gateway *ccord_voice_gateway_create(const ccord_voice_gateway_config *cfg, ccord_error *err)
{
  return vg_create_internal(cfg, false, err);
}

ccord_voice_gateway *ccord_voice_gateway_create_video_only(const ccord_voice_gateway_config *cfg, ccord_error *err)
{
  return vg_create_internal(cfg, true, err);
}

static void vg_stop_unlocked(ccord_voice_gateway *g, bool destroying)
{
  vg_close(g);
  vg_clear_audio_streams(g);
  vg_cam_clear_all(g);
  g->cam_wants_sent = false;
  if (g->video_depack) {
    ccord_rtp_h264_depack_destroy(g->video_depack);
    g->video_depack = nullptr;
  }
  if (g->codec) {
    ccord_voice_codec_destroy(g->codec);
    g->codec = nullptr;
  }
  if (g->dave) {
    ccord_dave_destroy(g->dave);
    g->dave = nullptr;
    if (!destroying && ccord_dave_built() && g->user_id != CCORD_ID_NONE && g->channel_id != CCORD_ID_NONE) {
      ccord_error derr;
      g->dave = ccord_dave_create(g->user_id, g->channel_id, vg_dave_send_cb, g, &derr);
    }
  }
  g->dave_audio_ssrc = 0;
  g->dave_video_ssrc = 0;
  g->last_udp_empty_ms = 0;
  ccord_ssrc_map_free(&g->ssrc_map);
  memset(g->ssrc_cache, 0, sizeof g->ssrc_cache);
  vg_bump_vol_gen(g);
  if (!destroying) {
    ccord_ssrc_map_init(&g->ssrc_map);
    ccord_speaking_init(&g->speaking);
    ccord_vad_init(&g->vad, -60.0f, CCORD_VAD_DEFAULT_HANGOVER_MS);
  }
  if (g->dsp) {
    ccord_voice_dsp_destroy(g->dsp);
    g->dsp = nullptr;
  }
  g->dsp_cfg = (ccord_voice_config){0};
  vg_pcm_free(&g->tx_pcm);
  vg_pcm_free(&g->rx_pcm);
  vg_pcm_free(&g->sb_pcm);
  ccord_free(g->tx_dave_buf);
  g->tx_dave_buf = nullptr;
  g->tx_dave_cap = 0;
  g->sb_speaking = false;
  g->tx_speaking_flags = 0;
  g->suppressed = false;
  g->raw_audio_cb = nullptr;
  g->raw_audio_ud = nullptr;
  g->packets_lost = 0;
  g->bitrate_bytes = 0;
  g->bitrate_window_ms = 0;
  g->bitrate_kbps = 0;
  g->ping_ms = -1;
  g->dave_epoch = 0;
  g->heartbeat_sent_ms = 0;
  g->hello = false;
  g->identified = false;
  g->selected = false;
  g->ready = false;
  g->video = false;
  g->speaking_active = false;
  g->frame_is_binary = false;
  g->seq_ack = -1;
  g->heartbeat_pending = false;
  g->heartbeat_nonce = 0;
  g->ssrc = g->video_ssrc = g->rtx_ssrc = g->remote_video_ssrc = g->remote_video_rtx_ssrc = 0;
  g->remote_video_user = g->default_remote_user;
  g->remote_stream_ssrc_count = 0;
  memset(g->remote_stream_ssrcs, 0, sizeof g->remote_stream_ssrcs);
  memset(g->remote_stream_rtx_ssrcs, 0, sizeof g->remote_stream_rtx_ssrcs);
  g->stream_last_pli_ms = 0;
  g->stream_pli_after_ms = 0;
  g->stream_need_idr = false;
  g->rtp_seq = 0;
  g->video_rtp_seq = 0;
  g->rtp_timestamp = 0;
  g->tx_nonce = 0;
  g->silence_frames_left = 0;
  g->next_audio_tx_ms = 0;
  g->next_rx_mix_ms = 0;
  g->rx_video_ready = false;
  g->rx_video_len = 0;
  g->rx_video_head = 0;
  g->rx_video_count = 0;
  for (size_t i = 0; i < VG_VIDEO_RING_SLOTS; i++) {
    ccord_free(g->rx_video_ring[i].buf);
    g->rx_video_ring[i].buf = nullptr;
    g->rx_video_ring[i].cap = 0;
    g->rx_video_ring[i].len = 0;
  }
  ccord_free(g->rx_video_buf);
  g->rx_video_buf = nullptr;
  g->rx_video_cap = 0;
  g->discovered_ip[0] = '\0';
  g->discovered_port = 0;
  g->aead_mode = CCORD_VOICE_MODE_NONE;
  g->secret_len = 0;
  ccord_voice_aead_clear(&g->aead_ctx);
  sodium_memzero(g->secret, sizeof g->secret);
  atomic_store(&g->keyframe_requested, false);
  g->state = CCORD_VOICE_IDLE;
  ccord_buf_shrink(&g->frame, 16384u);
  if (!destroying) {
    ccord_vc_init(&g->rc, ccord_clock_ms() ^ (uint64_t)(uintptr_t)g);
  }
  g->resume_at_ms = g->resume_deadline_ms = 0;
  g->skip_next_delay = false;
  g->has_failure = false;
  memset(&g->failure, 0, sizeof g->failure);
}

void ccord_voice_gateway_stop(ccord_voice_gateway *g)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  vg_stop_unlocked(g, false);
  ccord_mutex_unlock(g->mtx);
}

void ccord_voice_gateway_destroy(ccord_voice_gateway *g)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  g->destroying = true;
  vg_stop_unlocked(g, true);
  while (g->inflight_io > 0 && g->io_cond) {
    ccord_cond_wait(g->io_cond, g->mtx);
  }
  ccord_buf_free(&g->frame);
  ccord_live_wipe(g->token, sizeof g->token);
  ccord_live_wipe(g->session_id, sizeof g->session_id);
  ccord_mutex_unlock(g->mtx);
  ccord_cond_destroy(g->io_cond);
  ccord_mutex_destroy(g->mtx);
  g->mtx = nullptr;
  ccord_free(g);
}

// Performs the blocking WebSocket handshake outside g->mtx.
static ccord_status vg_curl_connect(const char *url, uint32_t timeout_ms, CURL **out_curl, ccord_error *err)
{
  *out_curl = nullptr;
  ccord_curl_global_init_once();
  CURL *curl = curl_easy_init();
  if (!curl) {
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "voice: curl init failed");
  }
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  ccord_curl_apply_tls(curl);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, ccord_live_user_agent());
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "wss,ws,https,http");
  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    curl_easy_cleanup(curl);
    return ccord_error_set(err, rc == CURLE_OPERATION_TIMEDOUT ? CCORD_ERR_TIMEOUT : CCORD_ERR_IO,
                           "voice: connect failed");
  }
  curl_socket_t sock = CURL_SOCKET_BAD;
  (void)curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock);
  if (sock == CURL_SOCKET_BAD) {
    curl_easy_cleanup(curl);
    return ccord_error_set(err, CCORD_ERR_IO, "voice: no socket");
  }
  (void)ccord_sock_set_nonblock((ccord_socket_t)sock);
  *out_curl = curl;
  return CCORD_OK;
}


static bool vg_parse_resumed(const ccord_voice_event *ev)
{
  return ev->op == CCORD_VOICE_OP_RESUMED;
}

static ccord_status vg_send_resume(ccord_voice_gateway *g, ccord_error *err)
{
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_status st = ccord_voice_build_resume(g->server_id, g->session_id, g->token, g->seq_ack, &b, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  return vg_send_buf(g, &b, err);
}

// The WebSocket died (or the server closed it). Asks the policy what to do: RESUME stays inside the gateway
// (state RECONNECTING, everything but the WebSocket kept); any other plan ends in FAILED with the decision stored
// for the owner (ccord_voice_gateway_failure). Runs under g->mtx.
void vg_on_ws_lost(ccord_voice_gateway *g, uint16_t close_code, ccord_vc_cause cause)
{
  uint64_t now = vg_now(g);
  bool est = g->state == CCORD_VOICE_ESTABLISHED || g->state == CCORD_VOICE_RECONNECTING;
  ccord_vc_plan plan = ccord_vc_plan_next(&g->rc, now, close_code, cause, est);
  if (plan.action == CCORD_VC_RESUME) {
    vg_close_ws(g);
    g->state = CCORD_VOICE_RECONNECTING;
    g->resume_at_ms = g->skip_next_delay ? now : now + plan.delay_ms;
    g->skip_next_delay = false;
    return;
  }
  vg_close(g);
  g->skip_next_delay = false;
  g->state = CCORD_VOICE_FAILED;
  g->failure = (ccord_voice_failure){
    .close_code = close_code, .cause = cause, .was_established = est, .state = g->rc, .plan = plan};
  g->has_failure = true;
}

// A malformed handshake is a deterministic fault: retrying cannot help, so the plan is FINAL.
static void vg_fail_protocol(ccord_voice_gateway *g)
{
  vg_close(g);
  g->state = CCORD_VOICE_FAILED;
  g->failure = (ccord_voice_failure){.close_code = 0,
                                     .cause = CCORD_VC_CAUSE_IO,
                                     .was_established = false,
                                     .state = g->rc,
                                     .plan = {.action = CCORD_VC_FINAL, .reason = "protocol error"}};
  g->has_failure = true;
}

// Timer step of a pending resume: open a new WebSocket (bounded by CCORD_VC_CONNECT_TIMEOUT_MS).
// Releases g->mtx across the blocking curl handshake so UDP audio keeps pumping. Op 7 goes out on Hello.
static void vg_try_resume(ccord_voice_gateway *g)
{
  if (g->destroying) return;
  if (g->send) {
    g->open = true;
    g->state = CCORD_VOICE_RECONNECTING;
    g->resume_deadline_ms = vg_now(g) + CCORD_VC_CONNECT_TIMEOUT_MS;   // Hello + op 9 must follow promptly
    return;
  }
  uint32_t timeout = g->connect_timeout_ms;
  if (timeout == 0 || timeout > CCORD_VC_CONNECT_TIMEOUT_MS) timeout = CCORD_VC_CONNECT_TIMEOUT_MS;
  char url[GW_URL_MAX];
  vg_copy(url, sizeof url, g->url);
  uint32_t gen = ++g->lifecycle_gen;
  g->ws_connecting = true;
  g->inflight_io++;
  ccord_mutex_unlock(g->mtx);

  CURL *curl = nullptr;
  ccord_error err;
  ccord_status st = vg_curl_connect(url, timeout, &curl, &err);

  ccord_mutex_lock(g->mtx);
  vg_io_leave(g);
  g->ws_connecting = false;
  if (g->destroying || g->lifecycle_gen != gen || g->state != CCORD_VOICE_RECONNECTING) {
    if (curl) curl_easy_cleanup(curl);
    return;
  }
  if (st != CCORD_OK) {
    if (curl) curl_easy_cleanup(curl);
    vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_CONNECT);
    return;
  }
  g->curl = curl;
  g->open = true;
  g->state = CCORD_VOICE_RECONNECTING;
  g->resume_deadline_ms = vg_now(g) + CCORD_VC_CONNECT_TIMEOUT_MS;   // Hello + op 9 must follow promptly
}

// RECONNECTING: start the pending attempt when due, or give up on one that never got its op 9.
static void vg_reconnect_step(ccord_voice_gateway *g, uint64_t now)
{
  if (g->ws_connecting) return;
  if (!g->open) {
    if (now >= g->resume_at_ms) vg_try_resume(g);
  } else if (now >= g->resume_deadline_ms) {
    CCORD_LOG_W("voice: resume got no answer, retrying");
    vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);
  }
}

bool ccord_voice_gateway_failure(const ccord_voice_gateway *g, ccord_voice_failure *out)
{
  if (!g || !out) return false;
  ccord_mutex_lock(vg_mtx(g));
  bool has = g->state == CCORD_VOICE_FAILED && g->has_failure;
  if (has) *out = g->failure;
  ccord_mutex_unlock(vg_mtx(g));
  return has;
}

void ccord_voice_gateway_network_event(ccord_voice_gateway *g)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  uint64_t now = vg_now(g);
  if ((g->state == CCORD_VOICE_ESTABLISHED && g->open) ||
      (g->state == CCORD_VOICE_RECONNECTING && !g->open && !g->ws_connecting)) {
    if (ccord_vc_accept_network_event(&g->rc, now)) {
      if (g->state == CCORD_VOICE_ESTABLISHED) {
        g->skip_next_delay = true;
        vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);   // treated as dead: resume at once
      } else {
        g->resume_at_ms = now;                   // pending attempt runs now; an attempt in flight is left alone
      }
    }
  }
  ccord_mutex_unlock(g->mtx);
}

void ccord_voice_gateway_inject_close(ccord_voice_gateway *g, uint16_t close_code)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  if (g->open) vg_on_ws_lost(g, close_code, close_code ? CCORD_VC_CAUSE_CLOSE : CCORD_VC_CAUSE_IO);
  ccord_mutex_unlock(g->mtx);
}

ccord_status ccord_voice_gateway_start(ccord_voice_gateway *g, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null gateway");
  ccord_mutex_lock(g->mtx);
  ccord_status st;
  if (g->destroying || g->open || g->ws_connecting || g->state != CCORD_VOICE_IDLE) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "voice: already connected");
  } else if (g->send) {
    g->has_failure = false;
    g->open = true;
    g->state = CCORD_VOICE_CONNECTING;
    st = CCORD_OK;
  } else {
    g->has_failure = false;
    g->state = CCORD_VOICE_CONNECTING;
    g->ws_connecting = true;
    char url[GW_URL_MAX];
    vg_copy(url, sizeof url, g->url);
    uint32_t timeout = g->connect_timeout_ms;
    uint32_t gen = ++g->lifecycle_gen;
    g->inflight_io++;
    ccord_mutex_unlock(g->mtx);

    CURL *curl = nullptr;
    st = vg_curl_connect(url, timeout, &curl, err);

    ccord_mutex_lock(g->mtx);
    vg_io_leave(g);
    g->ws_connecting = false;
    if (g->destroying || g->lifecycle_gen != gen) {
      if (curl) curl_easy_cleanup(curl);
      st = ccord_error_set(err, CCORD_ERR_IO, "voice: connect cancelled");
    } else if (st != CCORD_OK) {
      if (curl) curl_easy_cleanup(curl);
      vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_CONNECT);   // never established: plan REIDENTIFY
    } else {
      g->curl = curl;
      g->open = true;
      g->state = CCORD_VOICE_CONNECTING;
    }
  }
  ccord_mutex_unlock(g->mtx);
  return st;
}

static const char *vg_choose_mode(ccord_voice_gateway *g)
{
  const char *ptrs[CCORD_VOICE_MAX_MODES];
  for (size_t i = 0; i < g->mode_count; i++) ptrs[i] = g->modes[i];
  ccord_voice_mode chosen = ccord_voice_mode_choose(ptrs, g->mode_count);
  if (chosen != CCORD_VOICE_MODE_NONE) {
    g->aead_mode = chosen;
    return ccord_voice_mode_name(chosen);
  }
  g->aead_mode = CCORD_VOICE_MODE_NONE;
  for (size_t i = 0; i < g->mode_count; i++) {
    if (strcmp(g->modes[i], "xsalsa20_poly1305") == 0) return g->modes[i];
  }
  for (size_t i = 0; i < g->mode_count; i++) {
    if (strncmp(g->modes[i], "xsalsa20_poly1305", 17) == 0) return g->modes[i];
  }
  return g->mode_count > 0 ? g->modes[0] : nullptr;
}

static ccord_status vg_handle_binary_unlocked(ccord_voice_gateway *g, const uint8_t *data, size_t len,
                                              ccord_error *err)
{
  ccord_vframe vf;
  ccord_status st = ccord_vframe_parse(data, len, &vf, err);
  if (st != CCORD_OK) return st;
  g->seq_ack = (int64_t)vf.seq;
  if (!g->dave) return CCORD_OK;
  switch (vf.op) {
    case CCORD_VOICE_OP_DAVE_EXTERNAL_SENDER:
      ccord_dave_on_external_sender(g->dave, vf.payload, vf.payload_len);
      break;
    case CCORD_VOICE_OP_DAVE_PROPOSALS:
      ccord_dave_on_proposals(g->dave, vf.payload, vf.payload_len);
      break;
    case CCORD_VOICE_OP_DAVE_ANNOUNCE_COMMIT: {
      uint16_t tid = 0;
      const uint8_t *mls = nullptr;
      size_t mls_len = 0;
      st = ccord_vframe_split_transition(vf.payload, vf.payload_len, &tid, &mls, &mls_len, err);
      if (st != CCORD_OK) return st;
      ccord_dave_on_announce_commit(g->dave, tid, mls, mls_len);
      break;
    }
    case CCORD_VOICE_OP_DAVE_WELCOME: {
      uint16_t tid = 0;
      const uint8_t *mls = nullptr;
      size_t mls_len = 0;
      st = ccord_vframe_split_transition(vf.payload, vf.payload_len, &tid, &mls, &mls_len, err);
      if (st != CCORD_OK) return st;
      ccord_dave_on_welcome(g->dave, tid, mls, mls_len);
      break;
    }
    default:
      break;
  }
  return CCORD_OK;
}

ccord_status ccord_voice_gateway_handle_binary(ccord_voice_gateway *g, const uint8_t *data, size_t len,
                                               ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !data) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null binary payload");
  ccord_mutex_lock(g->mtx);
  ccord_status st = vg_handle_binary_unlocked(g, data, len, err);
  ccord_mutex_unlock(g->mtx);
  return st;
}

static bool vg_json_might_be_op6(const char *json, size_t len)
{
  if (len == 0 || len > 256u) return false;
  for (size_t i = 0; i + 5u <= len; i++) {
    if (json[i] == '"' && json[i + 1u] == 'o' && json[i + 2u] == 'p' && json[i + 3u] == '"') {
      size_t j = i + 4u;
      while (j < len && (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' || json[j] == '\n')) j++;
      if (j < len && json[j] == ':') {
        j++;
        while (j < len && (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' || json[j] == '\n')) j++;
        if (j < len && json[j] == '6' && (j + 1u >= len || json[j + 1u] < '0' || json[j + 1u] > '9')) {
          return true;
        }
      }
    }
  }
  return false;
}

// Single-parse decoder for op 6 HEARTBEAT_ACK so d.t is extracted without parsing the JSON twice.
// Returns 1 if decoded as op 6, 0 if another op (caller falls back to ccord_voice_gateway_decode),
// or -1 on parse error (err is populated).
static int vg_try_decode_heartbeat_ack(const char *json, size_t len, ccord_voice_event *ev,
                                       bool *has_nonce, uint64_t *nonce_out, ccord_error *err)
{
  if (!vg_json_might_be_op6(json, len)) return 0;
  ccord_arena a;
  ccord_arena_init(&a, 256);
  const ccord_json *root = ccord_json_parse(&a, json, len, nullptr, err);
  if (!root) {
    ccord_arena_free(&a);
    return -1;
  }
  if (ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&a);
    (void)ccord_error_set(err, CCORD_ERR_PARSE, "voice: payload is not an object");
    return -1;
  }
  const ccord_json *op_j = ccord_json_get(root, "op");
  int64_t op = -1;
  if (!op_j || !ccord_json_int64(op_j, &op) || op != CCORD_VOICE_OP_HEARTBEAT_ACK) {
    ccord_arena_free(&a);
    return 0;
  }
  memset(ev, 0, sizeof *ev);
  ev->op = CCORD_VOICE_OP_HEARTBEAT_ACK;
  ev->heartbeat_ack = true;
  int64_t seq = -1;
  const ccord_json *seq_j = ccord_json_get(root, "seq");
  if (!seq_j || !ccord_json_int64(seq_j, &seq)) {
    const ccord_json *s_j = ccord_json_get(root, "s");
    if (!s_j || !ccord_json_int64(s_j, &seq)) seq = -1;
  }
  ev->seq = seq;
  *has_nonce = false;
  *nonce_out = 0;
  const ccord_json *d = ccord_json_get(root, "d");
  const ccord_json *t = d ? ccord_json_get(d, "t") : nullptr;
  int64_t nonce = 0;
  if (t && ccord_json_int64(t, &nonce)) {
    *has_nonce = true;
    *nonce_out = (uint64_t)nonce;
  }
  ccord_arena_free(&a);
  return 1;
}

static ccord_status vg_handle_unlocked(ccord_voice_gateway *g, const char *json, size_t len,
                                       ccord_voice_event *out, ccord_error *err)
{
  ccord_voice_event ev;
  bool ack_parsed = false;
  bool ack_has_nonce = false;
  uint64_t ack_nonce = 0;
  ccord_status st = CCORD_OK;
  int ack_rc = vg_try_decode_heartbeat_ack(json, len, &ev, &ack_has_nonce, &ack_nonce, err);
  if (ack_rc < 0) return CCORD_ERR_PARSE;
  if (ack_rc > 0) {
    ack_parsed = true;
  } else {
    st = ccord_voice_gateway_decode(json, len, &ev, err);
    if (st != CCORD_OK) return st;
  }
  if (ev.seq >= 0) g->seq_ack = ev.seq;
  switch (ev.op) {
    case CCORD_VOICE_OP_HELLO:
      g->hello = true;
      g->heartbeat_ms = ev.heartbeat_interval_ms;
      g->next_heartbeat = vg_now(g) + g->heartbeat_ms;
      g->heartbeat_pending = false;
      g->heartbeat_nonce = 0;
      if (g->state == CCORD_VOICE_RECONNECTING) {
        // New WebSocket of a resume: op 7 instead of identify; never op 0 / op 1 (the selected protocol is kept).
        st = vg_send_resume(g, err);
        if (st != CCORD_OK) {
          vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);
          return st;
        }
        break;
      }
      g->state = CCORD_VOICE_HELLO;
      if (!g->identified) {
        g->identified = true;
        st = vg_send_identify(g, err);
        if (st != CCORD_OK) {
          vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);
          return st;
        }
      }
      break;
    case CCORD_VOICE_OP_READY:
      g->ready = true;
      g->ssrc = ev.ready_info.ssrc;
      vg_dave_ensure_audio_ssrc(g, g->ssrc);
      vg_copy(g->ip, sizeof g->ip, ev.ready_info.ip);
      g->port = ev.ready_info.port;
      g->mode_count = ev.ready_info.mode_count;
      for (size_t i = 0; i < ev.ready_info.mode_count; i++) vg_copy(g->modes[i], CCORD_VOICE_MODE_MAX, ev.ready_info.modes[i]);
      if (g->ip[0] == '\0' || g->port == 0) {
        vg_fail_protocol(g);
        return ccord_error_set(err, CCORD_ERR_PROTOCOL, "voice: ready has no udp endpoint");
      }
      {
        const char *mode = vg_choose_mode(g);
        if (!mode) {
          vg_fail_protocol(g);
          return ccord_error_set(err, CCORD_ERR_PROTOCOL, "voice: no encryption mode offered");
        }
        vg_copy(g->mode, sizeof g->mode, mode);
      }
      g->state = CCORD_VOICE_READY;
      vg_open_udp_and_discover(g);
      if (g->state == CCORD_VOICE_READY && g->discovered_ip[0] != '\0' && !g->selected) {
        g->selected = true;
        st = vg_send_select(g, err);
        if (st != CCORD_OK) {
          vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);
          return st;
        }
      }
      break;
    case CCORD_VOICE_OP_SESSION_DESCRIPTION:
      if (ev.secret_key_len == 0) {
        vg_fail_protocol(g);
        return ccord_error_set(err, CCORD_ERR_PROTOCOL, "voice: session description has no key");
      }
      memcpy(g->secret, ev.secret_key, ev.secret_key_len);
      g->secret_len = ev.secret_key_len;
      vg_copy(g->mode, sizeof g->mode, ev.mode[0] ? ev.mode : g->mode);
      g->aead_mode = ccord_voice_mode_from_name(g->mode);
      if (g->aead_mode != CCORD_VOICE_MODE_NONE && g->secret_len == CCORD_VOICE_KEY_BYTES) {
        ccord_error aerr;
        if (ccord_voice_aead_init(&g->aead_ctx, g->aead_mode, g->secret, &aerr) != CCORD_OK) {
          ccord_voice_aead_clear(&g->aead_ctx);
        }
      } else {
        ccord_voice_aead_clear(&g->aead_ctx);
      }
      if (!g->video_only && !g->codec) {
        ccord_error cerr;
        g->codec = ccord_voice_codec_create(&cerr);
      }
      if (g->dave) {
        ccord_dave_on_session_description(g->dave, ev.dave_protocol_version);
      }
      g->bitrate_window_ms = vg_now(g);
      g->bitrate_bytes = 0;
      g->bitrate_kbps = 0;
      g->state = CCORD_VOICE_ESTABLISHED;
      ccord_vc_established(&g->rc, vg_now(g));
      atomic_store(&g->keyframe_requested, true);
      vg_send_sink_wants(g);
      if (g->stream_rx_pli && !g->cam_rx && g->remote_video_ssrc != 0) {
        g->stream_need_idr = true;
        g->stream_pli_after_ms = vg_now(g) + 200u;
      }
      break;
    case CCORD_VOICE_OP_RESUMED:
      if (g->state != CCORD_VOICE_RECONNECTING || !vg_parse_resumed(&ev)) break;
      // Only the WebSocket was replaced: UDP, secret, DAVE, ssrc and speaking maps and the camera table carry on.
      g->state = CCORD_VOICE_ESTABLISHED;
      g->has_failure = false;
      g->heartbeat_pending = false;
      g->next_heartbeat = vg_now(g) + g->heartbeat_ms;
      g->bitrate_window_ms = vg_now(g);
      g->bitrate_bytes = 0;
      ccord_vc_established(&g->rc, vg_now(g));
      atomic_store(&g->keyframe_requested, true);   // the encoder emits an IDR; senders were gapped
      for (size_t i = 0; i < g->ncams; i++) {       // admitted cameras stay admitted, but wait for a fresh IDR
        if (g->cams[i].admitted) {
          g->cams[i].need_idr = true;
          g->cams[i].last_rx_ms = vg_now(g);
        }
      }
      break;
    case CCORD_VOICE_OP_HEARTBEAT_ACK: {
      // The ACK echoes the heartbeat nonce in d.t. Only an ACK for the outstanding heartbeat clears it;
      // a stale or foreign nonce must not keep a zombie session alive. ACKs without a nonce stay lenient.
      bool matches = true;
      if (ack_parsed) {
        if (ack_has_nonce) matches = ack_nonce == g->heartbeat_nonce;
      } else {
        ccord_arena a;
        ccord_arena_init(&a, 256);
        ccord_error jerr;
        const ccord_json *root = ccord_json_parse(&a, json, len, nullptr, &jerr);
        if (root) {
          const ccord_json *d = ccord_json_get(root, "d");
          const ccord_json *t = d ? ccord_json_get(d, "t") : nullptr;
          int64_t nonce = 0;
          if (t && ccord_json_int64(t, &nonce)) matches = (uint64_t)nonce == g->heartbeat_nonce;
        }
        ccord_arena_free(&a);
      }
      if (matches) {
        if (g->heartbeat_pending) {
          uint64_t now = vg_now(g);
          if (now >= g->heartbeat_sent_ms) {
            uint64_t rtt = now - g->heartbeat_sent_ms;
            g->ping_ms = rtt > (uint64_t)INT_MAX ? INT_MAX : (int)rtt;
          }
        }
        g->heartbeat_pending = false;
        g->next_heartbeat = vg_now(g) + g->heartbeat_ms;
      }
      break;
    }
    case CCORD_VOICE_OP_SPEAKING:
      if (ev.user_id != CCORD_ID_NONE) {
        if (ev.ssrc != 0) {
          (void)ccord_ssrc_map_set(&g->ssrc_map, ev.ssrc, ev.user_id);
          vg_bump_vol_gen(g);
        }
        if (g->dave) ccord_dave_on_clients_connect(g->dave, &ev.user_id, 1);
        ccord_speaking_note_op(&g->speaking, ev.user_id, ev.speaking_flag, vg_now(g));
      }
      break;
    case CCORD_VOICE_OP_VIDEO:
      if (ev.user_id != CCORD_ID_NONE) {
        bool ssrc_changed = false;
        if (ev.audio_ssrc != 0) {
          (void)ccord_ssrc_map_set(&g->ssrc_map, ev.audio_ssrc, ev.user_id);
          ssrc_changed = true;
        }
        if (ev.video_ssrc != 0) {
          (void)ccord_ssrc_map_set(&g->ssrc_map, ev.video_ssrc, ev.user_id);
          ssrc_changed = true;
        }
        if (ev.rtx_ssrc != 0) {
          (void)ccord_ssrc_map_set(&g->ssrc_map, ev.rtx_ssrc, ev.user_id);
          ssrc_changed = true;
        }
        for (size_t i = 0; i < ev.stream_ssrc_count; i++) {
          if (ev.stream_ssrcs[i] != 0) {
            (void)ccord_ssrc_map_set(&g->ssrc_map, ev.stream_ssrcs[i], ev.user_id);
            ssrc_changed = true;
          }
          if (ev.stream_rtx_ssrcs[i] != 0) {
            (void)ccord_ssrc_map_set(&g->ssrc_map, ev.stream_rtx_ssrcs[i], ev.user_id);
            ssrc_changed = true;
          }
        }
        if (ssrc_changed) vg_bump_vol_gen(g);
        if (g->dave) ccord_dave_on_clients_connect(g->dave, &ev.user_id, 1);
      }
      if (g->cam_rx) {
        // Per-user camera receive replaces the single legacy remote_video_ssrc slot.
        if (ev.user_id != CCORD_ID_NONE && ev.user_id != g->user_id) {
          vg_cam_on_op12(g, ev.user_id, ev.video_ssrc, ev.rtx_ssrc);
        }
      } else if (ev.user_id != g->user_id) {
        bool has_video = (ev.video_ssrc != 0 || ev.stream_ssrc_count > 0);
        if (has_video) {
          if (g->default_remote_user == CCORD_ID_NONE || ev.user_id == g->default_remote_user ||
              g->remote_video_ssrc == 0) {
            g->remote_video_user = ev.user_id;
            g->remote_video_ssrc = ev.video_ssrc;
            g->remote_video_rtx_ssrc = ev.rtx_ssrc;
            g->remote_stream_ssrc_count = 0;
            for (size_t i = 0; i < ev.stream_ssrc_count && i < VG_STREAM_SSRC_MAX; i++) {
              g->remote_stream_ssrcs[i] = ev.stream_ssrcs[i];
              g->remote_stream_rtx_ssrcs[i] = ev.stream_rtx_ssrcs[i];
              g->remote_stream_ssrc_count++;
            }
            if (g->stream_rx_pli && ev.video_ssrc != 0) {
              g->stream_need_idr = true;
              g->stream_pli_after_ms = vg_now(g) + 100u;
            }
          }
        } else if (ev.user_id != CCORD_ID_NONE && ev.user_id == g->remote_video_user) {
          // Only clear the active video SSRC when the actual broadcaster stops video; other
          // viewers joining a Go Live stream also emit OP 12 with video_ssrc == 0.
          g->remote_video_ssrc = 0;
          g->remote_video_rtx_ssrc = 0;
          g->remote_stream_ssrc_count = 0;
          g->stream_need_idr = false;
          g->stream_pli_after_ms = 0;
        }
      }
      break;
    case 18:
    case 20:
      if (g->dave && ev.user_id != CCORD_ID_NONE) {
        ccord_dave_on_clients_connect(g->dave, &ev.user_id, 1);
      }
      break;
    case CCORD_VOICE_OP_CLIENTS_CONNECT:
      if (g->dave && ev.connected_user_count > 0) {
        ccord_dave_on_clients_connect(g->dave, ev.connected_users, ev.connected_user_count);
      }
      break;
    case CCORD_VOICE_OP_CLIENT_DISCONNECT:
      if (ev.user_id != CCORD_ID_NONE) {
        vg_remove_audio_streams_for_user(g, ev.user_id);
        if (g->dave) ccord_dave_on_client_disconnect(g->dave, ev.user_id);
        ccord_ssrc_map_remove_user(&g->ssrc_map, ev.user_id);
        vg_bump_vol_gen(g);
        ccord_speaking_remove(&g->speaking, ev.user_id);
        vg_cam_remove_user(g, ev.user_id);
      }
      break;
    case CCORD_VOICE_OP_DAVE_PREPARE_TRANSITION:
      if (g->dave) ccord_dave_on_prepare_transition(g->dave, ev.transition_id, ev.dave_protocol_version);
      break;
    case CCORD_VOICE_OP_DAVE_EXECUTE_TRANSITION:
      if (g->dave) ccord_dave_on_execute_transition(g->dave, ev.transition_id);
      break;
    case CCORD_VOICE_OP_DAVE_PREPARE_EPOCH:
      g->dave_epoch = ev.epoch;
      if (g->dave) ccord_dave_on_prepare_epoch(g->dave, ev.epoch, ev.dave_protocol_version);
      break;
    default:
      break;
  }
  if (out) *out = ev;
  return CCORD_OK;
}

ccord_status ccord_voice_gateway_handle(ccord_voice_gateway *g, const char *json, size_t len,
                                        ccord_voice_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g || !json) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null payload");
  ccord_mutex_lock(g->mtx);
  ccord_status st = vg_handle_unlocked(g, json, len, out, err);
  ccord_mutex_unlock(g->mtx);
  return st;
}


static bool vg_drain(ccord_voice_gateway *g, ccord_voice_event *out)
{
  if (!g->curl || !g->open) return false;
  bool got = false;
  for (size_t round = 0; round < 64; round++) {
    unsigned char buf[16384];
    size_t n = 0;
    const struct curl_ws_frame *meta = nullptr;
    CURLcode rc = curl_ws_recv(g->curl, buf, sizeof buf, &n, &meta);
    if (rc == CURLE_AGAIN) return got;
    if (rc != CURLE_OK) {
      vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);
      return got;
    }
    if (!meta) return got;
    if ((meta->flags & CURLWS_CLOSE) != 0) {
      // Close frame payload: 2-byte big-endian code (RFC 6455), as discord_gateway.c reads it.
      size_t clen = n;
      curl_off_t left = meta->bytesleft;
      for (int tries = 0; clen < 2u && left > 0 && tries < 8; tries++) {
        size_t m = 0;
        const struct curl_ws_frame *m2 = nullptr;
        CURLcode rc2 = curl_ws_recv(g->curl, buf + clen, sizeof buf - clen, &m, &m2);
        if (rc2 == CURLE_OK) {
          clen += m;
          left = m2 ? m2->bytesleft : 0;
        } else {
          break;
        }
      }
      uint16_t close_code = (uint16_t)(clen >= 2 ? (((unsigned)buf[0] << 8) | (unsigned)buf[1]) : 0u);
      CCORD_LOG_W("voice: websocket closed (code=%u)", (unsigned)close_code);
      vg_on_ws_lost(g, close_code, close_code ? CCORD_VC_CAUSE_CLOSE : CCORD_VC_CAUSE_IO);
      return got;
    }
    if ((meta->flags & CURLWS_PING) != 0) {
      if (!g->ws_tx_busy) {
        size_t psent = 0;
        (void)curl_ws_send(g->curl, buf, n, &psent, 0, CURLWS_PONG);
      }
      continue;
    }
    if ((meta->flags & (CURLWS_TEXT | CURLWS_BINARY | CURLWS_CONT)) == 0) continue;
    if (g->frame.len == 0) {
      g->frame_is_binary = (meta->flags & CURLWS_BINARY) != 0;
    }
    if (n > 0 && ccord_buf_append(&g->frame, buf, n) != CCORD_OK) {
      vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_IO);
      return got;
    }
    if ((meta->flags & CURLWS_CONT) == 0 && meta->bytesleft == 0) {
      ccord_error err;
      if (g->frame_is_binary) {
        (void)vg_handle_binary_unlocked(g, (const uint8_t *)g->frame.data, g->frame.len, &err);
        ccord_buf_shrink(&g->frame, 16384u);
        g->frame_is_binary = false;
      } else {
        ccord_voice_event ev;
        if (vg_handle_unlocked(g, g->frame.data, g->frame.len, &ev, &err) == CCORD_OK) {
          if (ev.op != CCORD_VOICE_OP_HEARTBEAT_ACK) {
            *out = ev;
            ccord_buf_shrink(&g->frame, 16384u);
            return true;
          }
        }
        ccord_buf_shrink(&g->frame, 16384u);
      }
    }
  }
  return got;
}

bool ccord_voice_gateway_poll(ccord_voice_gateway *g, ccord_voice_event *out)
{
  if (!g || !out) return false;
  ccord_mutex_lock(g->mtx);
  if (g->state == CCORD_VOICE_RECONNECTING) vg_reconnect_step(g, vg_now(g));   // before the !open guard: timer
  if (!g->open) {
    if (g->state == CCORD_VOICE_RECONNECTING) vg_pump_udp(g);   // keep draining the kept UDP socket
    ccord_mutex_unlock(g->mtx);
    return false;
  }
  bool got = vg_drain(g, out);
  vg_pump_udp(g);
  vg_send_sink_wants(g);
  vg_cam_tick(g, vg_now(g));
  if (g->stream_rx_pli && !g->cam_rx && g->state == CCORD_VOICE_ESTABLISHED && g->remote_video_ssrc != 0 &&
      g->stream_need_idr && (g->stream_pli_after_ms == 0 || vg_now(g) >= g->stream_pli_after_ms)) {
    vg_send_pli_ssrc(g, g->remote_video_ssrc, &g->stream_last_pli_ms, vg_now(g));
  }
  if (g->state == CCORD_VOICE_ESTABLISHED) ccord_vc_tick(&g->rc, vg_now(g));
  if (g->open && g->hello && g->state != CCORD_VOICE_RECONNECTING && vg_now(g) >= g->next_heartbeat) {
    if (g->heartbeat_pending) {
      // The last heartbeat was never acknowledged: the session is dead even though the socket looks open.
      CCORD_LOG_W("voice: heartbeat not acknowledged, dropping session");
      vg_on_ws_lost(g, 0, CCORD_VC_CAUSE_HEARTBEAT);
      ccord_mutex_unlock(g->mtx);
      return got;
    }
    ccord_error err;
    if (vg_send_heartbeat(g, &err) != CCORD_OK) {
      g->next_heartbeat = vg_now(g) + 250u;
    }
  }
  ccord_mutex_unlock(g->mtx);
  return got;
}

ccord_voice_connection_state ccord_voice_gateway_state(const ccord_voice_gateway *g)
{
  if (!g) return CCORD_VOICE_IDLE;
  ccord_mutex_lock(vg_mtx(g));
  ccord_voice_connection_state st = g->state;
  ccord_mutex_unlock(vg_mtx(g));
  return st;
}

uint32_t ccord_voice_gateway_ssrc(const ccord_voice_gateway *g)
{
  if (!g) return 0;
  ccord_mutex_lock(vg_mtx(g));
  uint32_t ssrc = g->ssrc;
  ccord_mutex_unlock(vg_mtx(g));
  return ssrc;
}

int ccord_voice_gateway_udp_fd(const ccord_voice_gateway *g)
{
  if (!g) return -1;
  ccord_mutex_lock(vg_mtx(g));
  int fd = g->udp_fd;
  ccord_mutex_unlock(vg_mtx(g));
  return fd;
}

static void vg_clamp_deadline(uint64_t now, uint64_t deadline, uint32_t *timeout_ms)
{
  if (deadline <= now) {
    *timeout_ms = 0u;
    return;
  }
  uint64_t rem = deadline - now;
  if (rem < (uint64_t)*timeout_ms) *timeout_ms = (uint32_t)rem;
}

size_t ccord_voice_gateway_wait_info(const ccord_voice_gateway *g, ccord_socket_t *out_fds, size_t cap,
                                     uint32_t *out_timeout_ms)
{
  if (out_timeout_ms) *out_timeout_ms = UINT32_MAX;
  if (!g) return 0;
  ccord_mutex_lock(vg_mtx(g));
  size_t nfds = 0;
  if (g->open && g->curl) {
    curl_socket_t sock = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(g->curl, CURLINFO_ACTIVESOCKET, &sock) == CURLE_OK && sock != CURL_SOCKET_BAD) {
      if (out_fds && nfds < cap) out_fds[nfds++] = (ccord_socket_t)sock;
    }
  }
  if (g->udp_fd >= 0 && out_fds && nfds < cap) {
    out_fds[nfds++] = (ccord_socket_t)g->udp_fd;
  }
  uint64_t now = vg_now(g);
  uint32_t timeout = UINT32_MAX;
  if (g->state == CCORD_VOICE_RECONNECTING && !g->ws_connecting) {
    if (!g->open) {
      vg_clamp_deadline(now, g->resume_at_ms, &timeout);
    } else {
      vg_clamp_deadline(now, g->resume_deadline_ms, &timeout);
    }
  }
  if (g->open && g->hello && g->state != CCORD_VOICE_RECONNECTING) {
    vg_clamp_deadline(now, g->next_heartbeat, &timeout);
  }
  if (g->state == CCORD_VOICE_READY && !g->selected && g->ip_discovery_deadline != 0) {
    vg_clamp_deadline(now, g->ip_discovery_deadline, &timeout);
  }
  if (g->state == CCORD_VOICE_ESTABLISHED &&
      (g->tx_pcm.len >= CCORD_VOICE_FRAME_SAMPLES || g->sb_pcm.len >= CCORD_VOICE_FRAME_SAMPLES ||
       g->silence_frames_left > 0)) {
    if (g->next_audio_tx_ms == 0) timeout = 0u;
    else vg_clamp_deadline(now, g->next_audio_tx_ms, &timeout);
  }
  if (g->stream_rx_pli && !g->cam_rx && g->state == CCORD_VOICE_ESTABLISHED && g->remote_video_ssrc != 0 &&
      g->stream_need_idr) {
    uint64_t due = g->stream_pli_after_ms;
    if (g->stream_last_pli_ms != 0 && g->stream_last_pli_ms + 1000u > due) {
      due = g->stream_last_pli_ms + 1000u;
    }
    vg_clamp_deadline(now, due, &timeout);
  }
  ccord_mutex_unlock(vg_mtx(g));
  if (out_timeout_ms) *out_timeout_ms = timeout;
  return nfds;
}

size_t ccord_voice_gateway_secret(const ccord_voice_gateway *g, uint8_t out[CCORD_VOICE_MAX_SECRET])
{
  if (!g || !out) return 0;
  ccord_mutex_lock(vg_mtx(g));
  memcpy(out, g->secret, g->secret_len);
  size_t n = g->secret_len;
  ccord_mutex_unlock(vg_mtx(g));
  return n;
}


ccord_status vg_set_speaking(ccord_voice_gateway *g, uint8_t speaking, uint32_t delay_ms,
                             ccord_error *err)
{
  if (g->state != CCORD_VOICE_ESTABLISHED && g->state != CCORD_VOICE_READY) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: not established");
  }
  ccord_buf b;
  ccord_buf_init(&b, 256);
  ccord_status st = ccord_voice_build_speaking(g->ssrc, speaking, delay_ms, &b, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  return vg_send_buf(g, &b, err);
}

ccord_status ccord_voice_gateway_set_speaking(ccord_voice_gateway *g, uint8_t speaking, uint32_t delay_ms,
                                              ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null gateway");
  ccord_mutex_lock(g->mtx);
  ccord_status st = vg_set_speaking(g, speaking, delay_ms, err);
  ccord_mutex_unlock(g->mtx);
  return st;
}

static ccord_status vg_send_video_op(ccord_voice_gateway *g, uint32_t video_ssrc, uint32_t rtx_ssrc,
                                     ccord_error *err)
{
  if (g->state != CCORD_VOICE_ESTABLISHED && g->state != CCORD_VOICE_READY) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice: not established");
  }
  vg_dave_ensure_video_ssrc(g, video_ssrc);
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_status st = ccord_voice_build_video_ex(g->ssrc, video_ssrc, rtx_ssrc, &g->video_desc, &b, err);
  if (st != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  st = vg_send_buf(g, &b, err);
  if (st == CCORD_OK) {
    g->video = (video_ssrc != 0);
    g->video_ssrc = video_ssrc;
    g->rtx_ssrc = rtx_ssrc;
    if (video_ssrc != 0) vg_upgrade_udp_video_buf(g);
  }
  return st;
}

ccord_status ccord_voice_gateway_send_video_op(ccord_voice_gateway *g, uint32_t video_ssrc, uint32_t rtx_ssrc,
                                               ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null gateway");
  ccord_mutex_lock(g->mtx);
  ccord_status st = vg_send_video_op(g, video_ssrc, rtx_ssrc, err);
  ccord_mutex_unlock(g->mtx);
  return st;
}

ccord_status ccord_voice_gateway_set_video(ccord_voice_gateway *g, bool enabled, ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null gateway");
  ccord_mutex_lock(g->mtx);
  ccord_status st;
  if (enabled && g->ssrc == 0) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "voice: no audio ssrc");
  } else {
    uint32_t vs = enabled ? (g->video_ssrc ? g->video_ssrc : g->ssrc + 1u) : 0;
    uint32_t rs = enabled ? (g->rtx_ssrc ? g->rtx_ssrc : g->ssrc + 2u) : 0;
    st = vg_send_video_op(g, vs, rs, err);
  }
  ccord_mutex_unlock(g->mtx);
  return st;
}

ccord_status ccord_voice_gateway_set_video_desc(ccord_voice_gateway *g, const ccord_voice_video_desc *desc,
                                                ccord_error *err)
{
  ccord_error_clear(err);
  if (!g) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null gateway");
  ccord_mutex_lock(g->mtx);
  g->video_desc = desc ? *desc : (ccord_voice_video_desc){0};
  ccord_mutex_unlock(g->mtx);
  return CCORD_OK;
}



bool ccord_voice_gateway_consume_keyframe_request(ccord_voice_gateway *g)
{
  if (!g) return false;
  return atomic_exchange(&g->keyframe_requested, false);
}

bool ccord_voice_gateway_user_speaking(const ccord_voice_gateway *g, ccord_id user_id)
{
  if (!g || user_id == CCORD_ID_NONE) return false;
  ccord_mutex_lock(vg_mtx(g));
  bool speaking = ccord_speaking_is(&g->speaking, user_id, vg_now(g));
  ccord_mutex_unlock(vg_mtx(g));
  return speaking;
}

bool ccord_voice_gateway_self_speaking(const ccord_voice_gateway *g)
{
  if (!g) return false;
  ccord_mutex_lock(vg_mtx(g));
  bool speaking = ccord_speaking_is(&g->speaking, g->user_id, vg_now(g));
  ccord_mutex_unlock(vg_mtx(g));
  return speaking;
}

bool ccord_voice_gateway_poll_speaking(ccord_voice_gateway *g, ccord_id *out_user_id, bool *out_speaking)
{
  if (!g) return false;
  ccord_mutex_lock(g->mtx);
  bool got = ccord_speaking_poll(&g->speaking, vg_now(g), out_user_id, out_speaking);
  ccord_mutex_unlock(g->mtx);
  return got;
}

// ---- M5 voice controls (local preferences; never sent as Discord state) --------------------------------------

void ccord_voice_gateway_set_user_volume(ccord_voice_gateway *g, ccord_id user_id, uint8_t pct)
{
  if (!g || user_id == CCORD_ID_NONE) return;
  if (pct > 200u) pct = 200u;
  ccord_mutex_lock(g->mtx);
  for (size_t i = 0; i < g->user_volume_count; i++) {
    if (g->user_volume[i].user_id == user_id) {
      g->user_volume[i].pct = pct;
      vg_bump_vol_gen(g);
      ccord_mutex_unlock(g->mtx);
      return;
    }
  }
  if (g->user_volume_count == CCORD_SPEAK_MAX) {
    // Bounded table: evict the oldest entry to make room for the new preference.
    memmove(&g->user_volume[0], &g->user_volume[1], (CCORD_SPEAK_MAX - 1u) * sizeof g->user_volume[0]);
    g->user_volume_count = CCORD_SPEAK_MAX - 1u;
  }
  g->user_volume[g->user_volume_count++] = (vg_user_volume){.user_id = user_id, .pct = pct};
  vg_bump_vol_gen(g);
  ccord_mutex_unlock(g->mtx);
}

void ccord_voice_gateway_set_ptt(ccord_voice_gateway *g, bool held)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  g->vad.ptt_held = held;
  ccord_mutex_unlock(g->mtx);
}

void ccord_voice_gateway_set_deafened(ccord_voice_gateway *g, bool deafened)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  g->deafened = deafened;
  if (deafened) {
    vg_pcm_reset(&g->rx_pcm);
    g->next_rx_mix_ms = 0;
    for (size_t i = 0; i < VOICE_MAX_STREAMS; i++) {
      if (g->audio_streams[i].used && g->audio_streams[i].jitter) {
        ccord_jitter_reset(g->audio_streams[i].jitter);
      }
    }
  }
  ccord_mutex_unlock(g->mtx);
}

void ccord_voice_gateway_set_vad_sensitivity(ccord_voice_gateway *g, uint32_t pct)
{
  if (!g) return;
  if (pct > 100u) pct = 100u;
  ccord_mutex_lock(g->mtx);
  g->dsp_cfg.vad_sensitivity_pct = pct;
  g->vad.threshold_db = ccord_vad_threshold_from_sensitivity(pct);
  ccord_mutex_unlock(g->mtx);
}

void ccord_voice_gateway_set_suppressed(ccord_voice_gateway *g, bool suppressed)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  g->suppressed = suppressed;
  if (suppressed) {
    g->vad.hangover_left_ms = 0;
    g->vad.active = false;
  }
  ccord_mutex_unlock(g->mtx);
}


size_t ccord_voice_gateway_dave_peers(ccord_voice_gateway *g, ccord_id *out_ids, size_t cap)
{
  if (!g) return 0;
  ccord_mutex_lock(g->mtx);
  size_t n = g->dave ? ccord_dave_peers(g->dave, out_ids, cap) : 0;
  ccord_mutex_unlock(g->mtx);
  return n;
}
