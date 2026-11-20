// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Per-user remote camera receive, OP 15 sink-wants, and PLI feedback.
#include "voice_internal.h"

#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>

// ---- per-user camera receive ---------------------------
// Everything below runs under g->mtx, on the UI thread or the audio thread (vg_pump_udp). It depacketizes and
// DAVE-decrypts but never decodes. Kept in one block so the connection failure/resume code stays separate.

#define VG_CAM_PLI_INTERVAL_MS 1000u
#define VG_CAM_IDLE_MS 5000u

static atomic_uint_fast32_t g_cam_generation = 1;

static uint32_t vg_cam_next_generation(void)
{
  uint32_t v = (uint32_t)atomic_fetch_add(&g_cam_generation, 1);
  if (v == 0) v = (uint32_t)atomic_fetch_add(&g_cam_generation, 1);  // 0 is reserved for "none"
  return v;
}

static vg_cam *vg_cam_find_user(ccord_voice_gateway *g, ccord_id user_id)
{
  for (size_t i = 0; i < g->ncams; i++) {
    if (g->cams[i].user_id == user_id) return &g->cams[i];
  }
  return nullptr;
}

vg_cam *vg_cam_find_ssrc(ccord_voice_gateway *g, uint32_t ssrc, bool *is_rtx)
{
  if (is_rtx) *is_rtx = false;
  if (ssrc == 0) return nullptr;
  for (size_t i = 0; i < g->ncams; i++) {
    vg_cam *c = &g->cams[i];
    if (c->video_ssrc == ssrc) return c;
    if (c->rtx_ssrc == ssrc) {
      if (is_rtx) *is_rtx = true;
      return c;
    }
  }
  return nullptr;
}

static size_t vg_cam_admitted_count(const ccord_voice_gateway *g)
{
  size_t n = 0;
  for (size_t i = 0; i < g->ncams; i++) {
    if (g->cams[i].admitted) n++;
  }
  return n;
}

static void vg_cam_flush_ring(vg_cam *c)
{
  c->head = 0;
  c->count = 0;
}

// RTCP PSFB FMT=1 (PLI), sealed like RTP: 8 clear bytes (header + our SSRC), payload = media SSRC. 1 per second per sender.
void vg_send_pli_ssrc(ccord_voice_gateway *g, uint32_t video_ssrc, uint64_t *last_pli_ms, uint64_t now_ms)
{
  if (g->state != CCORD_VOICE_ESTABLISHED || g->aead_mode == CCORD_VOICE_MODE_NONE || g->ssrc == 0 ||
      video_ssrc == 0 || !last_pli_ms) {
    return;
  }
  if (*last_pli_ms != 0 && now_ms >= *last_pli_ms && now_ms - *last_pli_ms < VG_CAM_PLI_INTERVAL_MS) return;
  *last_pli_ms = now_ms ? now_ms : 1;
  uint8_t hdr[8] = {0x81u, 0xCEu, 0x00u, 0x02u, (uint8_t)(g->ssrc >> 24), (uint8_t)(g->ssrc >> 16),
                    (uint8_t)(g->ssrc >> 8), (uint8_t)g->ssrc};
  uint8_t media[4] = {(uint8_t)(video_ssrc >> 24), (uint8_t)(video_ssrc >> 16), (uint8_t)(video_ssrc >> 8),
                      (uint8_t)video_ssrc};
  ccord_error err;
  (void)vg_seal_and_send_rtp(g, hdr, sizeof hdr, media, sizeof media, &err);
}

static void vg_send_pli(ccord_voice_gateway *g, vg_cam *c, uint64_t now_ms)
{
  if (c) vg_send_pli_ssrc(g, c->video_ssrc, &c->last_pli_ms, now_ms);
}

// OP 15 {"any":100}: ask the SFU for every video sink once per connection (never any:0, which would stop video).
void vg_send_sink_wants(ccord_voice_gateway *g)
{
  if (!g->cam_rx || g->cam_wants_sent || g->state != CCORD_VOICE_ESTABLISHED) return;
  static const char wants[] = "{\"op\":15,\"d\":{\"any\":100}}";
  ccord_error err;
  if (vg_send_raw(g, wants, sizeof wants - 1u, &err) == CCORD_OK) g->cam_wants_sent = true;
}

static void vg_cam_release(vg_cam *c)
{
  if (c->depack) {
    ccord_rtp_h264_depack_destroy(c->depack);
    c->depack = nullptr;
  }
  for (size_t i = 0; i < CCORD_VOICE_CAM_RING; i++) {
    ccord_free(c->ring[i].data);
    c->ring[i] = (vg_cam_au){0};
  }
  vg_cam_flush_ring(c);
  c->admitted = false;
  c->need_idr = false;
}

static void vg_cam_admit(ccord_voice_gateway *g, vg_cam *c)
{
  if (c->admitted) return;
  c->depack = ccord_rtp_h264_depack_create();
  if (!c->depack) return;  // stays waiting; a later promotion retries
  c->admitted = true;
  c->demoted = false;
  c->need_idr = true;
  c->generation = vg_cam_next_generation();
  vg_cam_flush_ring(c);
  uint64_t now = vg_now(g);
  c->last_rx_ms = now;
  vg_send_pli(g, c, now);
}

// Fills free slots from the waiters: first come first served, idle-demoted senders only after every other waiter.
static void vg_cam_promote(ccord_voice_gateway *g)
{
  while (vg_cam_admitted_count(g) < CCORD_VOICE_CAM_DECODE_MAX) {
    vg_cam *pick = nullptr;
    for (size_t i = 0; i < g->ncams && !pick; i++) {
      if (!g->cams[i].admitted && !g->cams[i].demoted) pick = &g->cams[i];
    }
    for (size_t i = 0; i < g->ncams && !pick; i++) {
      if (!g->cams[i].admitted) pick = &g->cams[i];
    }
    if (!pick) return;
    vg_cam_admit(g, pick);
    if (!pick->admitted) return;  // allocation failed: do not spin
  }
}

void vg_cam_remove_user(ccord_voice_gateway *g, ccord_id user_id)
{
  vg_cam *c = vg_cam_find_user(g, user_id);
  if (!c) return;
  bool was_admitted = c->admitted;
  vg_cam_release(c);
  size_t idx = (size_t)(c - g->cams);
  memmove(&g->cams[idx], &g->cams[idx + 1u], (g->ncams - idx - 1u) * sizeof g->cams[0]);
  g->ncams--;
  memset(&g->cams[g->ncams], 0, sizeof g->cams[0]);
  if (was_admitted) vg_cam_promote(g);
}

void vg_cam_clear_all(ccord_voice_gateway *g)
{
  for (size_t i = 0; i < g->ncams; i++) vg_cam_release(&g->cams[i]);
  memset(g->cams, 0, sizeof g->cams);
  g->ncams = 0;
}

void vg_cam_on_op12(ccord_voice_gateway *g, ccord_id user_id, uint32_t video_ssrc, uint32_t rtx_ssrc)
{
  vg_cam *c = vg_cam_find_user(g, user_id);
  if (video_ssrc == 0) {  // camera off
    if (c) vg_cam_remove_user(g, user_id);
    return;
  }
  if (!c) {
    if (g->ncams >= CCORD_VOICE_CAM_MAX) return;  // over the tracking limit: ignored
    c = &g->cams[g->ncams++];
    memset(c, 0, sizeof *c);
    c->user_id = user_id;
    c->video_ssrc = video_ssrc;
    c->rtx_ssrc = rtx_ssrc;
    c->generation = vg_cam_next_generation();
    vg_cam_promote(g);
    return;
  }
  c->rtx_ssrc = rtx_ssrc;
  if (c->video_ssrc == video_ssrc) return;
  // Camera restarted on a new SSRC: the old AU stream is gone.
  c->video_ssrc = video_ssrc;
  c->generation = vg_cam_next_generation();
  if (c->admitted) {
    ccord_rtp_h264_depack_reset(c->depack);
    vg_cam_flush_ring(c);
    c->need_idr = true;
    c->last_rx_ms = vg_now(g);
    c->last_pli_ms = 0;
    vg_send_pli(g, c, c->last_rx_ms);
  }
}

// Copies (and DAVE-decrypts with this sender's id) one access unit into the next free ring slot.
static bool vg_cam_push_au(ccord_voice_gateway *g, vg_cam *c, const uint8_t *au, size_t len, uint32_t ts, bool key)
{
  uint64_t now = vg_now(g);
  if (c->count >= CCORD_VOICE_CAM_RING) {  // nobody is polling: one IDR, never a growing buffer
    vg_cam_flush_ring(c);
    c->need_idr = true;
    vg_send_pli(g, c, now);
    return false;
  }
  vg_cam_au *slot = &c->ring[(c->head + c->count) % CCORD_VOICE_CAM_RING];
  if (slot->cap < len + 64u) {
    size_t ncap = len + 4096u;
    uint8_t *nb = ccord_realloc(slot->data, ncap);
    if (!nb) return false;
    slot->data = nb;
    slot->cap = ncap;
  }
  size_t plain_len = len;
  if (g->dave) {
    if (ccord_dave_decrypt_ssrc(g->dave, c->video_ssrc, c->user_id, CCORD_DAVE_VIDEO, au, len, slot->data, slot->cap,
                                &plain_len) != CCORD_DAVE_OK) {
      c->need_idr = true;
      vg_send_pli(g, c, now);
      return false;
    }
  } else {
    memcpy(slot->data, au, len);
  }
  if (plain_len == 0) return false;
  if (c->need_idr) {
    if (!key) return false;  // its reference frame is missing
    c->need_idr = false;
  }
  slot->len = plain_len;
  slot->ts90k = ts;
  slot->keyframe = key;
  c->count++;
  return true;
}

void vg_handle_rx_cam_packet(ccord_voice_gateway *g, vg_cam *c, const ccord_rtp_view *rtp,
                                    const uint8_t *payload, size_t len)
{
  uint64_t now = vg_now(g);
  c->last_rx_ms = now;
  if (!c->depack) return;
  const uint8_t *au = nullptr;
  size_t au_len = 0;
  bool keyframe = false;
  uint32_t ts = 0;
  bool done = ccord_rtp_h264_depack_push(c->depack, payload, len, rtp->seq, rtp->timestamp, rtp->marker, &au,
                                         &au_len, &keyframe, &ts);
  if (!done || !au || au_len == 0) {
    if (ccord_rtp_h264_depack_needs_keyframe(c->depack)) {
      c->need_idr = true;
      vg_send_pli(g, c, now);
    }
    return;
  }
  if (au_len > CCORD_VOICE_CAM_AU_MAX) {
    c->need_idr = true;
    vg_send_pli(g, c, now);
    return;
  }
  (void)vg_cam_push_au(g, c, au, au_len, ts, keyframe);
}

// Called from ccord_voice_gateway_poll: keeps asking for an IDR while one is missing, and hands the slot of an
// admitted sender that went silent to a waiting one (safety net when OP 12 video_ssrc:0 never arrives).
void vg_cam_tick(ccord_voice_gateway *g, uint64_t now_ms)
{
  if (!g->cam_rx || g->ncams == 0 || g->state != CCORD_VOICE_ESTABLISHED) return;  // no idle demotion mid-resume
  for (size_t i = 0; i < g->ncams; i++) {
    vg_cam *c = &g->cams[i];
    if (c->admitted && c->need_idr) vg_send_pli(g, c, now_ms);
  }
  for (size_t guard = 0; guard < CCORD_VOICE_CAM_MAX; guard++) {
    bool waiter = false;
    for (size_t i = 0; i < g->ncams; i++) {
      if (!g->cams[i].admitted && !g->cams[i].demoted) waiter = true;
    }
    if (!waiter) return;
    vg_cam *idle = nullptr;
    for (size_t i = 0; i < g->ncams; i++) {
      vg_cam *c = &g->cams[i];
      if (c->admitted && now_ms >= c->last_rx_ms + VG_CAM_IDLE_MS && (!idle || c->last_rx_ms < idle->last_rx_ms)) {
        idle = c;
      }
    }
    if (!idle) return;
    vg_cam_release(idle);
    idle->demoted = true;  // behind every waiting sender, wherever it sits in the table
    vg_cam_promote(g);
  }
}

void ccord_voice_gateway_set_camera_rx(ccord_voice_gateway *g, bool enabled)
{
  if (!g) return;
  ccord_mutex_lock(g->mtx);
  if (enabled) {
    g->cam_rx = true;
    vg_upgrade_udp_video_buf(g);
    vg_send_sink_wants(g);
  } else {
    g->cam_rx = false;
    vg_cam_clear_all(g);
  }
  ccord_mutex_unlock(g->mtx);
}

size_t ccord_voice_gateway_cameras(ccord_voice_gateway *g, ccord_voice_cam_info *out, size_t max)
{
  if (!g || !out) return 0;
  ccord_mutex_lock(g->mtx);
  size_t n = g->ncams < max ? g->ncams : max;
  for (size_t i = 0; i < n; i++) {
    const vg_cam *c = &g->cams[i];
    out[i] = (ccord_voice_cam_info){.user_id = c->user_id,
                                    .video_ssrc = c->video_ssrc,
                                    .generation = c->generation,
                                    .decoding = c->admitted,
                                    .need_keyframe = c->admitted && c->need_idr};
  }
  ccord_mutex_unlock(g->mtx);
  return n;
}

bool ccord_voice_gateway_poll_camera(ccord_voice_gateway *g, ccord_id user_id, uint8_t *buf, size_t buf_cap,
                                     size_t *out_len, uint32_t *out_ts90k, bool *out_keyframe)
{
  if (!g || !buf) return false;
  ccord_mutex_lock(g->mtx);
  vg_pump_udp(g);
  vg_cam *c = vg_cam_find_user(g, user_id);
  if (!c || !c->admitted || c->count == 0) {
    ccord_mutex_unlock(g->mtx);
    return false;
  }
  vg_cam_au *au = &c->ring[c->head];
  if (au->len > buf_cap) {  // cannot deliver it: drop it and wait for the next IDR
    vg_cam_flush_ring(c);
    c->need_idr = true;
    vg_send_pli(g, c, vg_now(g));
    ccord_mutex_unlock(g->mtx);
    return false;
  }
  memcpy(buf, au->data, au->len);
  if (out_len) *out_len = au->len;
  if (out_ts90k) *out_ts90k = au->ts90k;
  if (out_keyframe) *out_keyframe = au->keyframe;
  au->len = 0;
  vg_shrink_video_buf(&au->data, &au->cap);
  c->head = (uint8_t)((c->head + 1u) % CCORD_VOICE_CAM_RING);
  c->count--;
  ccord_mutex_unlock(g->mtx);
  return true;
}
// ---- end per-user camera receive -------------------------------------------------------------------------------

