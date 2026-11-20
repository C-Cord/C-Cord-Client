// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "live_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void live_stream_rx_clear(live *l)
{
  for (size_t i = 0; i < LIVE_STREAM_FRAME_SLOTS; i++) {
    ccord_free(l->rx_frames[i].data);
    l->rx_frames[i].data = nullptr;
    l->rx_frames[i].len = 0;
  }
  l->rx_frame_head = 0;
  l->rx_frame_count = 0;
  ccord_free(l->rx_pcm);
  l->rx_pcm = nullptr;
  l->rx_pcm_head = 0;
  l->rx_pcm_count = 0;
  if (l->stream_dec) {
    opus_decoder_destroy(l->stream_dec);
    l->stream_dec = nullptr;
  }
}

// Reset the sender's partial PCM chunk and stream-audio timestamp and release the lazy encoder. Caller holds media_mtx.
void live_stream_tx_clear(live *l)
{
  l->tx_pcm_acc_len = 0;
  l->tx_audio_ts48k = 0;
  if (l->stream_enc) {
    opus_encoder_destroy(l->stream_enc);
    l->stream_enc = nullptr;
  }
}

// Last-known voice flags keyed by (guild, user), fed by gateway VOICE_STATE_UPDATE. Stage REST answers are 204 with
// no body, so the synthesized completion event reuses these rather than zeroing mute/deaf/self_* it does not own.

void live_vstream_slot_close_media(live *l, live_vstream_slot *slot)
{
  ccord_mutex_lock(l->media_mtx);
  if (slot->vs) {
    ccord_vstream_close(slot->vs);
    slot->vs = nullptr;
  }
  if (slot->sender) live_stream_tx_clear(l); else live_stream_rx_clear(l);
  ccord_mutex_unlock(l->media_mtx);
  slot->attempted = false;
}

void live_vstream_slot_reset(live *l, live_vstream_slot *slot)
{
  bool sender = slot->sender;
  ccord_mutex_lock(l->media_mtx);
  if (slot->vs) {
    ccord_vstream_close(slot->vs);
    slot->vs = nullptr;
  }
  if (sender) live_stream_tx_clear(l); else live_stream_rx_clear(l);
  ccord_mutex_unlock(l->media_mtx);
  ccord_live_wipe(slot->token, sizeof slot->token);
  memset(slot, 0, sizeof *slot);
  slot->sender = sender;
}

void live_streams_stop_all(live *l)
{
  if (l->gw && l->streaming && l->tx_stream.stream_key[0] != '\0') {
    ccord_error ignored;
    (void)ccord_gateway_send_stream_delete_key(l->gw, l->tx_stream.stream_key, &ignored);
  }
  if (l->gw && l->watching_stream && l->rx_stream.stream_key[0] != '\0') {
    ccord_error ignored;
    (void)ccord_gateway_send_stream_delete_key(l->gw, l->rx_stream.stream_key, &ignored);
  }
  live_vstream_slot_reset(l, &l->tx_stream);
  live_vstream_slot_reset(l, &l->rx_stream);
  l->streaming = false;
  l->watching_stream = false;
  l->watched_guild = CCORD_ID_NONE;
  l->watched_channel = CCORD_ID_NONE;
  l->watched_user = CCORD_ID_NONE;
  l->stream_paused = false;
  l->tx_stream_paused = false;
}


static void live_stream_peer_put(live *l, const char *stream_key, ccord_id rtc_server_id, ccord_id rtc_channel_id)
{
  if (!stream_key || stream_key[0] == '\0' || rtc_server_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < l->stream_peer_count; i++) {
    if (strcmp(l->stream_peers[i].stream_key, stream_key) == 0) {
      l->stream_peers[i].rtc_server_id = rtc_server_id;
      l->stream_peers[i].rtc_channel_id = rtc_channel_id;
      return;
    }
  }
  if (l->stream_peer_count == LIVE_STREAM_PEERS_MAX) {
    memmove(&l->stream_peers[0], &l->stream_peers[1],
            (LIVE_STREAM_PEERS_MAX - 1u) * sizeof l->stream_peers[0]);
    l->stream_peer_count = LIVE_STREAM_PEERS_MAX - 1u;
  }
  live_stream_meta *m = &l->stream_peers[l->stream_peer_count++];
  live_copy_cstr(m->stream_key, sizeof m->stream_key, stream_key);
  m->rtc_server_id = rtc_server_id;
  m->rtc_channel_id = rtc_channel_id;
}

static const live_stream_meta *live_stream_peer_find(const live *l, const char *stream_key)
{
  if (!stream_key || stream_key[0] == '\0') return nullptr;
  for (size_t i = 0; i < l->stream_peer_count; i++) {
    if (strcmp(l->stream_peers[i].stream_key, stream_key) == 0) return &l->stream_peers[i];
  }
  return nullptr;
}

static void live_stream_peer_drop(live *l, const char *stream_key)
{
  if (!stream_key || stream_key[0] == '\0') return;
  for (size_t i = 0; i < l->stream_peer_count; i++) {
    if (strcmp(l->stream_peers[i].stream_key, stream_key) == 0) {
      memmove(&l->stream_peers[i], &l->stream_peers[i + 1u],
              (l->stream_peer_count - i - 1u) * sizeof l->stream_peers[0]);
      l->stream_peer_count--;
      return;
    }
  }
}

static void live_stream_fill_config(live_vstream_slot *slot, const ccord_stream_config *cfg)
{
  if (!cfg) {
    slot->width = 1920u;
    slot->height = 1080u;
    slot->fps = 30u;
    slot->max_bitrate = 8000000u;
    return;
  }
  switch (cfg->resolution) {
    case CCORD_STREAM_RES_720P:
      slot->width = 1280u;
      slot->height = 720u;
      break;
    case CCORD_STREAM_RES_1440P:
      slot->width = 2560u;
      slot->height = 1440u;
      break;
    case CCORD_STREAM_RES_1080P:
    case CCORD_STREAM_RES_SOURCE:
    default:
      slot->width = 1920u;
      slot->height = 1080u;
      break;
  }
  switch (cfg->fps) {
    case CCORD_STREAM_FPS_15:
      slot->fps = 15u;
      break;
    case CCORD_STREAM_FPS_60:
      slot->fps = 60u;
      break;
    case CCORD_STREAM_FPS_30:
    default:
      slot->fps = 30u;
      break;
  }
  slot->max_bitrate = 8000000u;
}

// Viewer video callback (from ccord_vstream_pump_all, outside every vstream lock): queue one DAVE-decrypted
// Annex-B access unit into the bounded FIFO, evicting the oldest when full.
static void live_stream_on_frame(void *ud, uint32_t ssrc, ccord_id user_id, const uint8_t *annexb, size_t len,
                                 bool keyframe, uint32_t ts90k)
{
  live *l = ud;
  (void)ssrc;
  (void)user_id;
  if (!l || !annexb || len == 0 || len > LIVE_STREAM_FRAME_MAX) return;
  ccord_mutex_lock(l->media_mtx);
  if (l->rx_frame_count == LIVE_STREAM_FRAME_SLOTS) {
    l->rx_frames[l->rx_frame_head].len = 0;
    l->rx_frame_head = (l->rx_frame_head + 1u) % LIVE_STREAM_FRAME_SLOTS;
    l->rx_frame_count--;
  }
  size_t idx = (l->rx_frame_head + l->rx_frame_count) % LIVE_STREAM_FRAME_SLOTS;
  live_stream_frame *f = &l->rx_frames[idx];
  if (!f->data) f->data = ccord_malloc(LIVE_STREAM_FRAME_MAX);
  if (f->data) {
    memcpy(f->data, annexb, len);
    f->len = len;
    f->ts90k = ts90k;
    f->keyframe = keyframe;
    l->rx_frame_count++;
  }
  ccord_mutex_unlock(l->media_mtx);
}

// Decodes one 48 kHz Opus packet into l->rx_pcm. Caller MUST hold l->media_mtx.
static void live_stream_decode_opus_locked(live *l, const uint8_t *opus, size_t len)
{
  if (!l || !opus || len == 0 || len > (size_t)INT32_MAX) return;
  if (!l->stream_dec) {
    int oerr = OPUS_OK;
    l->stream_dec = opus_decoder_create((opus_int32)CCORD_VOICE_SAMPLE_RATE, 1, &oerr);
    if (oerr != OPUS_OK || !l->stream_dec) {
      l->stream_dec = nullptr;
      return;
    }
  }
  if (!l->rx_pcm) {
    l->rx_pcm = ccord_alloc_array(LIVE_STREAM_PCM_CAP_SAMPLES, sizeof *l->rx_pcm);
    if (!l->rx_pcm) return;
  }
  int16_t dec[5760];   // longest Opus frame at 48 kHz (120 ms)
  int n = opus_decode(l->stream_dec, opus, (opus_int32)len, dec, (int)(sizeof dec / sizeof dec[0]), 0);
  if (n > 0) {
    size_t keep = (size_t)n;
    const int16_t *src = dec;
    if (keep > LIVE_STREAM_PCM_CAP_SAMPLES) {
      src += keep - LIVE_STREAM_PCM_CAP_SAMPLES;
      keep = LIVE_STREAM_PCM_CAP_SAMPLES;
    }
    if (l->rx_pcm_count + keep > LIVE_STREAM_PCM_CAP_SAMPLES) {
      size_t drop = l->rx_pcm_count + keep - LIVE_STREAM_PCM_CAP_SAMPLES;
      l->rx_pcm_head = (l->rx_pcm_head + drop) % LIVE_STREAM_PCM_CAP_SAMPLES;
      l->rx_pcm_count -= drop;
    }
    size_t tail = (l->rx_pcm_head + l->rx_pcm_count) % LIVE_STREAM_PCM_CAP_SAMPLES;
    size_t first = keep < LIVE_STREAM_PCM_CAP_SAMPLES - tail ? keep : LIVE_STREAM_PCM_CAP_SAMPLES - tail;
    memcpy(&l->rx_pcm[tail], src, first * sizeof *src);
    if (keep > first) memcpy(&l->rx_pcm[0], src + first, (keep - first) * sizeof *src);
    l->rx_pcm_count += keep;
  }
}

// Viewer stream-audio callback (from ccord_vstream_pump_all): decode one Opus packet to 48 kHz mono PCM and push
// it into the bounded ring, evicting the oldest samples on overflow so playout always sees the newest audio.
static void live_stream_on_audio(void *ud, uint32_t ssrc, ccord_id user_id, const uint8_t *opus, size_t len,
                                 uint32_t ts48k)
{
  live *l = ud;
  (void)ssrc;
  (void)user_id;
  (void)ts48k;
  if (!l || !opus || len == 0 || len > (size_t)INT32_MAX) return;
  if (l->self_deaf) {
    ccord_mutex_lock(l->media_mtx);
    l->rx_pcm_head = 0;
    l->rx_pcm_count = 0;
    ccord_mutex_unlock(l->media_mtx);
    return;
  }
  ccord_mutex_lock(l->media_mtx);
  live_stream_decode_opus_locked(l, opus, len);
  ccord_mutex_unlock(l->media_mtx);
}

static void live_stream_on_keyframe(void *ud)
{
  live *l = ud;
  if (l) atomic_store(&l->tx_keyframe_requested, true);
}

static void live_vstream_seed_peers(live *l, live_vstream_slot *slot, ccord_vstream *vs)
{
  if (!l || !slot || !vs) return;
  ccord_id owner = slot->sender ? CCORD_ID_NONE : l->watched_user;
  ccord_id peers[512];
  size_t npeers = 0;
  if (owner != CCORD_ID_NONE && npeers < sizeof peers / sizeof peers[0]) peers[npeers++] = owner;
  for (size_t i = 0; i < slot->viewer_id_count && npeers < sizeof peers / sizeof peers[0]; i++) {
    if (slot->viewer_ids[i] != CCORD_ID_NONE) peers[npeers++] = slot->viewer_ids[i];
  }
  for (size_t i = 0; i < l->voice_cache_count && npeers < sizeof peers / sizeof peers[0]; i++) {
    const ccord_voice_state *vc = &l->voice_cache[i];
    if (vc->user_id == CCORD_ID_NONE || vc->channel_id == CCORD_ID_NONE) continue;
    if (vc->channel_id == slot->rtc_channel_id || vc->channel_id == l->watched_channel ||
        vc->channel_id == l->voice_channel) {
      peers[npeers++] = vc->user_id;
    }
  }
  if (l->voice && npeers < sizeof peers / sizeof peers[0]) {
    ccord_id vpeers[256];
    size_t nv = ccord_voice_gateway_dave_peers(l->voice, vpeers, sizeof vpeers / sizeof vpeers[0]);
    if (nv > sizeof vpeers / sizeof vpeers[0]) nv = sizeof vpeers / sizeof vpeers[0];
    for (size_t i = 0; i < nv && npeers < sizeof peers / sizeof peers[0]; i++) {
      peers[npeers++] = vpeers[i];
    }
  }
  ccord_vstream_register_peers(vs, owner, npeers > 0 ? peers : nullptr, npeers);
}

void live_vstream_maybe_open(live *l, live_vstream_slot *slot)
{
  if (!l || !slot || slot->vs || slot->attempted) return;
  bool wanted = slot->sender ? l->streaming : l->watching_stream;
  if (!wanted) return;
  if (slot->rtc_server_id == CCORD_ID_NONE || l->self_id == CCORD_ID_NONE) return;
  if (slot->token[0] == '\0' || slot->endpoint[0] == '\0') return;
  if (l->voice_session_id[0] == '\0') return;

  slot->attempted = true;
  ccord_vstream_params vp = {
    .sender = slot->sender,
    .server_id = slot->rtc_server_id,
    .channel_id = slot->rtc_channel_id,
    .user_id = l->self_id,
    .dave_group_id = 0,
    .session_id = l->voice_session_id,
    .endpoint = slot->endpoint,
    .token = slot->token,
    .width = slot->width,
    .height = slot->height,
    .fps = slot->fps,
    .max_bitrate = slot->max_bitrate,
    // Only the viewer receives media; the sender's access units go out through the send hooks.
    .on_frame = slot->sender ? nullptr : live_stream_on_frame,
    .on_keyframe_request = slot->sender ? live_stream_on_keyframe : nullptr,
    .on_audio = slot->sender ? nullptr : live_stream_on_audio,
    .ud = l,
  };
  ccord_error verr;
  ccord_vstream *vs = ccord_vstream_open(&vp, &verr);
  if (!vs) {
    CCORD_LOG_W("live: vstream open failed (%s): %s", slot->sender ? "sender" : "viewer", verr.message);
    return;
  }
  live_vstream_seed_peers(l, slot, vs);
  ccord_mutex_lock(l->media_mtx);
  slot->vs = vs;
  ccord_mutex_unlock(l->media_mtx);
}

static bool live_stream_sig_matches_tx(const live *l, const ccord_stream_signal *sig)
{
  if (l->tx_stream.stream_key[0] != '\0' && sig->stream_key && strcmp(l->tx_stream.stream_key, sig->stream_key) == 0) {
    return true;
  }
  if (l->self_id == CCORD_ID_NONE || sig->user_id != l->self_id) return false;
  if (l->watching_stream && l->watched_user == l->self_id) return false;
  return l->streaming || (l->voice_channel != CCORD_ID_NONE && sig->channel_id == l->voice_channel);
}

static bool live_stream_sig_matches_rx(const live *l, const ccord_stream_signal *sig)
{
  if (!l->watching_stream) return false;
  if (l->rx_stream.stream_key[0] != '\0' && sig->stream_key && sig->stream_key[0] != '\0') {
    return strcmp(l->rx_stream.stream_key, sig->stream_key) == 0;
  }
  return sig->user_id != CCORD_ID_NONE && sig->user_id == l->watched_user &&
         (sig->channel_id == CCORD_ID_NONE || sig->channel_id == l->watched_channel);
}

static void live_vstream_slot_mark_unavailable(live *l, live_vstream_slot *slot)
{
  live_vstream_slot_close_media(l, slot);
  ccord_live_wipe(slot->token, sizeof slot->token);
  slot->endpoint[0] = '\0';
  slot->rtc_server_id = CCORD_ID_NONE;
  slot->rtc_channel_id = CCORD_ID_NONE;
}

void live_stream_on_event(live *l, const ccord_net_event *ev)
{
  if (!ev || ev->type != CCORD_NET_STREAM_SIGNAL || !ev->stream_signal) return;
  const ccord_stream_signal *sig = ev->stream_signal;
  if (sig->deleted || sig->kind == CCORD_STREAM_SIG_DELETE) {
    live_stream_peer_drop(l, sig->stream_key);
    if (live_stream_sig_matches_tx(l, sig)) {
      l->streaming = false;
      l->tx_stream_paused = false;
      live_vstream_slot_reset(l, &l->tx_stream);
    }
    if (live_stream_sig_matches_rx(l, sig)) {
      l->watching_stream = false;
      l->watched_guild = CCORD_ID_NONE;
      l->watched_channel = CCORD_ID_NONE;
      l->watched_user = CCORD_ID_NONE;
      l->stream_paused = false;
      live_vstream_slot_reset(l, &l->rx_stream);
    }
    return;
  }
  if (sig->unavailable) {
    live_stream_peer_drop(l, sig->stream_key);
    if (live_stream_sig_matches_tx(l, sig)) live_vstream_slot_mark_unavailable(l, &l->tx_stream);
    if (live_stream_sig_matches_rx(l, sig)) live_vstream_slot_mark_unavailable(l, &l->rx_stream);
    return;
  }
  if (sig->kind == CCORD_STREAM_SIG_UPDATE) {
    if (live_stream_sig_matches_tx(l, sig)) {
      l->tx_stream_paused = sig->paused;
      if (sig->viewer_ids && sig->viewer_id_count > 0) {
        size_t n = sig->viewer_id_count > 128u ? 128u : sig->viewer_id_count;
        memcpy(l->tx_stream.viewer_ids, sig->viewer_ids, n * sizeof *sig->viewer_ids);
        l->tx_stream.viewer_id_count = n;
        if (l->tx_stream.vs) ccord_vstream_register_peers(l->tx_stream.vs, CCORD_ID_NONE, sig->viewer_ids, n);
      }
    }
    if (live_stream_sig_matches_rx(l, sig)) {
      l->stream_paused = sig->paused;
      if (sig->viewer_ids && sig->viewer_id_count > 0) {
        size_t n = sig->viewer_id_count > 128u ? 128u : sig->viewer_id_count;
        memcpy(l->rx_stream.viewer_ids, sig->viewer_ids, n * sizeof *sig->viewer_ids);
        l->rx_stream.viewer_id_count = n;
        if (l->rx_stream.vs) ccord_vstream_register_peers(l->rx_stream.vs, l->watched_user, sig->viewer_ids, n);
      }
    }
    return;
  }
  if (!sig->server_update) {
    // STREAM_CREATE: record rtc_server_id for peer lookup and active sender/viewer slots.
    ccord_id rtc_chan = sig->rtc_channel_id != CCORD_ID_NONE ? sig->rtc_channel_id : sig->channel_id;
    live_stream_peer_put(l, sig->stream_key, sig->rtc_server_id, rtc_chan);
    if (live_stream_sig_matches_tx(l, sig)) {
      l->streaming = true;
      if (sig->viewer_ids && sig->viewer_id_count > 0) {
        size_t n = sig->viewer_id_count > 128u ? 128u : sig->viewer_id_count;
        memcpy(l->tx_stream.viewer_ids, sig->viewer_ids, n * sizeof *sig->viewer_ids);
        l->tx_stream.viewer_id_count = n;
        if (l->tx_stream.vs) ccord_vstream_register_peers(l->tx_stream.vs, CCORD_ID_NONE, sig->viewer_ids, n);
      }
      if (sig->stream_key) {
        if (l->tx_stream.stream_key[0] != '\0' && strcmp(l->tx_stream.stream_key, sig->stream_key) != 0) {
          live_vstream_slot_close_media(l, &l->tx_stream);
        }
        live_copy_cstr(l->tx_stream.stream_key, sizeof l->tx_stream.stream_key, sig->stream_key);
      }
      if (l->tx_stream.width == 0) live_stream_fill_config(&l->tx_stream, nullptr);
      if (sig->rtc_server_id != CCORD_ID_NONE) {
        if (l->tx_stream.vs && l->tx_stream.rtc_server_id != sig->rtc_server_id) {
          live_vstream_slot_close_media(l, &l->tx_stream);
        }
        l->tx_stream.rtc_server_id = sig->rtc_server_id;
        l->tx_stream.rtc_channel_id = rtc_chan;
        l->tx_stream.attempted = false;
        live_vstream_maybe_open(l, &l->tx_stream);
      }
    }
    if (live_stream_sig_matches_rx(l, sig)) {
      l->stream_paused = sig->paused;
      if (sig->viewer_ids && sig->viewer_id_count > 0) {
        size_t n = sig->viewer_id_count > 128u ? 128u : sig->viewer_id_count;
        memcpy(l->rx_stream.viewer_ids, sig->viewer_ids, n * sizeof *sig->viewer_ids);
        l->rx_stream.viewer_id_count = n;
        if (l->rx_stream.vs) ccord_vstream_register_peers(l->rx_stream.vs, l->watched_user, sig->viewer_ids, n);
      }
      if (sig->stream_key) live_copy_cstr(l->rx_stream.stream_key, sizeof l->rx_stream.stream_key, sig->stream_key);
      if (sig->rtc_server_id != CCORD_ID_NONE) {
        if (l->rx_stream.vs && l->rx_stream.rtc_server_id != sig->rtc_server_id) {
          live_vstream_slot_close_media(l, &l->rx_stream);
        }
        l->rx_stream.rtc_server_id = sig->rtc_server_id;
        l->rx_stream.rtc_channel_id = rtc_chan;
        l->rx_stream.attempted = false;
        live_vstream_maybe_open(l, &l->rx_stream);
      }
    }
    return;
  }

  // STREAM_SERVER_UPDATE: pair token + endpoint with the matching sender or viewer slot.
  live_vstream_slot *slot = nullptr;
  if (live_stream_sig_matches_rx(l, sig)) {
    slot = &l->rx_stream;
  } else if (live_stream_sig_matches_tx(l, sig)) {
    slot = &l->tx_stream;
    if (slot->width == 0) live_stream_fill_config(slot, nullptr);
  }
  if (!slot) return;

  live_vstream_slot_close_media(l, slot);
  ccord_live_wipe(slot->token, sizeof slot->token);
  if (sig->stream_key) live_copy_cstr(slot->stream_key, sizeof slot->stream_key, sig->stream_key);
  live_copy_cstr(slot->token, sizeof slot->token, sig->token);
  live_copy_cstr(slot->endpoint, sizeof slot->endpoint, sig->endpoint);
  if (slot->token[0] == '\0' || slot->endpoint[0] == '\0') {
    ccord_live_wipe(slot->token, sizeof slot->token);
    slot->endpoint[0] = '\0';
    return;
  }
  slot->attempted = false;
  live_vstream_maybe_open(l, slot);
}


ccord_status live_start_stream(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                      const ccord_stream_config *config, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (l->voice_guild != guild_id || l->voice_channel != channel_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: join the voice channel before streaming");
  }
  ccord_status st = ccord_gateway_send_voice_state(l->gw, guild_id, channel_id, l->self_mute, l->self_deaf,
                                                   l->self_video, true, err);
  if (st != CCORD_OK) return st;
  st = ccord_gateway_send_stream_create(l->gw, guild_id, channel_id, config, err);
  if (st == CCORD_OK) {
    live_vstream_slot_reset(l, &l->tx_stream);
    live_stream_fill_config(&l->tx_stream, config);
    if (l->self_id != CCORD_ID_NONE) {
      ccord_stream_key_format(guild_id, channel_id, l->self_id, l->tx_stream.stream_key);
    }
    l->streaming = true;
    l->tx_stream_paused = false;
  }
  return st;
}

ccord_status live_stop_stream(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (!l->streaming || l->voice_guild != guild_id) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "live: no active stream");
  }
  const char *key = l->tx_stream.stream_key;
  char key_buf[96];
  if (key[0] == '\0') {
    if (l->self_id == CCORD_ID_NONE) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: stream key unavailable");
    }
    ccord_stream_key_format(guild_id, l->voice_channel, l->self_id, key_buf);
    key = key_buf;
  }
  ccord_status st = ccord_gateway_send_stream_delete_key(l->gw, key, err);
  if (st == CCORD_OK) {
    if (l->voice_channel != CCORD_ID_NONE) {
      ccord_error verr;
      (void)ccord_gateway_send_voice_state(l->gw, guild_id, l->voice_channel, l->self_mute, l->self_deaf,
                                           l->self_video, false, &verr);
    }
    l->streaming = false;
    l->tx_stream_paused = false;
    live_vstream_slot_reset(l, &l->tx_stream);
  }
  return st;
}

ccord_status live_watch_stream(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                      ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: watch stream ids");
  }
  char sk[96];
  ccord_stream_key_format(guild_id, channel_id, user_id, sk);
  if (l->watching_stream && l->rx_stream.stream_key[0] != '\0' && strcmp(l->rx_stream.stream_key, sk) == 0) {
    return CCORD_OK;
  }
  bool had_prev = l->rx_stream.stream_key[0] != '\0';
  if (had_prev) {
    char old_sk[sizeof l->rx_stream.stream_key];
    live_copy_cstr(old_sk, sizeof old_sk, l->rx_stream.stream_key);
    live_vstream_slot_reset(l, &l->rx_stream);
    l->watching_stream = false;
    l->watched_guild = CCORD_ID_NONE;
    l->watched_channel = CCORD_ID_NONE;
    l->watched_user = CCORD_ID_NONE;
    l->stream_paused = false;
    ccord_status dst = ccord_gateway_send_stream_delete_key(l->gw, old_sk, err);
    if (dst != CCORD_OK) return dst;
  }
  ccord_status st = ccord_gateway_send_stream_watch(l->gw, sk, err);
  if (st == CCORD_OK) {
    if (!had_prev) live_vstream_slot_reset(l, &l->rx_stream);
    l->watched_guild = guild_id;
    l->watched_channel = channel_id;
    l->watched_user = user_id;
    l->watching_stream = true;
    l->stream_paused = false;
    live_copy_cstr(l->rx_stream.stream_key, sizeof l->rx_stream.stream_key, sk);
    const live_stream_meta *peer = live_stream_peer_find(l, sk);
    if (peer) {
      l->rx_stream.rtc_server_id = peer->rtc_server_id;
      l->rx_stream.rtc_channel_id = peer->rtc_channel_id;
    }
  }
  return st;
}

ccord_status live_stop_watching(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                       ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: stop watching stream ids");
  }
  char sk[96];
  ccord_stream_key_format(guild_id, channel_id, user_id, sk);
  ccord_status st = ccord_gateway_send_stream_delete_key(l->gw, sk, err);
  if (st == CCORD_OK) {
    if ((l->rx_stream.stream_key[0] != '\0' && strcmp(l->rx_stream.stream_key, sk) == 0) ||
        (l->watching_stream && l->watched_user == user_id && l->watched_channel == channel_id)) {
      l->watching_stream = false;
      l->watched_guild = CCORD_ID_NONE;
      l->watched_channel = CCORD_ID_NONE;
      l->watched_user = CCORD_ID_NONE;
      l->stream_paused = false;
      live_vstream_slot_reset(l, &l->rx_stream);
    }
  }
  return st;
}

ccord_status live_set_stream_paused(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                           ccord_id user_id, bool paused, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!l->gw) return ccord_error_set(err, CCORD_ERR_IO, "live: gateway disabled");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: pause stream ids");
  }
  char sk[96];
  ccord_stream_key_format(guild_id, channel_id, user_id, sk);
  ccord_status st = ccord_gateway_send_stream_set_paused(l->gw, sk, paused, err);
  if (st == CCORD_OK) {
    if (l->self_id != CCORD_ID_NONE && user_id == l->self_id) {
      l->tx_stream_paused = paused;
      if (!paused) atomic_store(&l->tx_keyframe_requested, true);
    } else {
      l->stream_paused = paused;
    }
  }
  return st;
}


// ---- Go Live media plane (m5-golive-live): H.264 + stream-audio hooks bridged to the tx/rx vstreams ----------

// Sender: one Annex-B access unit out through the active broadcast vstream (thread-safe; may run on the encoder
// thread). media_mtx is held across the vstream call so teardown cannot close the handle mid-send.
ccord_status live_send_stream_frame(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k,
                                           bool keyframe, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty stream frame");
  if (len > LIVE_STREAM_FRAME_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "live: stream frame too large");
  ccord_mutex_lock(l->media_mtx);
  ccord_status st;
  if (!l->streaming || !l->tx_stream.vs) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "live: no active Go Live stream");
  } else {
    st = ccord_vstream_send_frame(l->tx_stream.vs, annexb, len, ts90k, keyframe);
    if (st != CCORD_OK) ccord_error_set(err, st, "live: stream frame send failed");
  }
  ccord_mutex_unlock(l->media_mtx);
  return st;
}

_Static_assert(CCORD_CAM_RX_MAX == CCORD_VOICE_CAM_MAX, "camera limits must match the voice gateway");
_Static_assert(CCORD_CAM_DECODE_MAX == CCORD_VOICE_CAM_DECODE_MAX, "camera limits must match the voice gateway");
_Static_assert(CCORD_CAM_FRAME_MAX == CCORD_VOICE_CAM_AU_MAX, "camera limits must match the voice gateway");

// Remote voice cameras: straight forwards to the main voice gateway's per-user table (nullptr voice -> nothing).
size_t live_camera_senders(ccord_transport *t, ccord_camera_rx *out, size_t max)
{
  live *l = (live *)t;
  if (!l->voice || !out || max == 0) return 0;
  ccord_voice_cam_info info[CCORD_VOICE_CAM_MAX];
  size_t n = ccord_voice_gateway_cameras(l->voice, info, CCORD_VOICE_CAM_MAX);
  if (n > max) n = max;
  for (size_t i = 0; i < n; i++) {
    out[i] = (ccord_camera_rx){.user_id = info[i].user_id, .generation = info[i].generation,
                               .decoding = info[i].decoding};
  }
  return n;
}

bool live_poll_camera_frame(ccord_transport *t, ccord_id user_id, uint8_t *buf, size_t buf_cap, size_t *out_len,
                                   uint32_t *out_ts90k, bool *out_keyframe)
{
  live *l = (live *)t;
  if (!l->voice || !buf || buf_cap == 0 || !out_len) return false;
  return ccord_voice_gateway_poll_camera(l->voice, user_id, buf, buf_cap, out_len, out_ts90k, out_keyframe);
}

// Viewer: oldest queued access unit first. Every head larger than buf_cap is dropped (with any further oversized
// heads) so one huge frame cannot stall the ones behind it; the first fitting frame is copied out.
bool live_poll_stream_frame(ccord_transport *t, uint8_t *buf, size_t buf_cap, size_t *out_len,
                                   uint32_t *out_ts90k, bool *out_keyframe)
{
  live *l = (live *)t;
  if (!buf || buf_cap == 0 || !out_len) return false;
  ccord_mutex_lock(l->media_mtx);
  while (l->rx_frame_count > 0 && l->rx_frames[l->rx_frame_head].len > buf_cap) {
    l->rx_frames[l->rx_frame_head].len = 0;
    l->rx_frame_head = (l->rx_frame_head + 1u) % LIVE_STREAM_FRAME_SLOTS;
    l->rx_frame_count--;
  }
  bool got = false;
  if (l->rx_frame_count > 0) {
    live_stream_frame *f = &l->rx_frames[l->rx_frame_head];
    memcpy(buf, f->data, f->len);
    *out_len = f->len;
    if (out_ts90k) *out_ts90k = f->ts90k;
    if (out_keyframe) *out_keyframe = f->keyframe;
    f->len = 0;
    l->rx_frame_head = (l->rx_frame_head + 1u) % LIVE_STREAM_FRAME_SLOTS;
    l->rx_frame_count--;
    got = true;
  }
  ccord_mutex_unlock(l->media_mtx);
  return got;
}

// Sender: accumulate 48 kHz mono S16 PCM and emit one Opus packet per complete 960-sample (20 ms) chunk on the
// broadcast's audio SSRC. The 48 kHz RTP timestamp advances by 960 per successfully sent chunk; a partial tail
// is carried into the next call.
ccord_status live_send_stream_pcm(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!pcm || samples == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty stream pcm");
  ccord_mutex_lock(l->media_mtx);
  ccord_status st = CCORD_OK;
  if (!l->streaming || !l->tx_stream.vs) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "live: no active Go Live stream");
  } else {
    if (!l->stream_enc) {
      int oerr = OPUS_OK;
      l->stream_enc = opus_encoder_create((opus_int32)CCORD_VOICE_SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO, &oerr);
      if (oerr != OPUS_OK || !l->stream_enc) {
        l->stream_enc = nullptr;
        st = ccord_error_set(err, CCORD_ERR_INTERNAL, "live: stream opus encoder init failed");
      } else {
        (void)opus_encoder_ctl(l->stream_enc, OPUS_SET_BITRATE(64000));
        (void)opus_encoder_ctl(l->stream_enc, OPUS_SET_COMPLEXITY(5));
      }
    }
    size_t off = 0;
    while (st == CCORD_OK && off < samples) {
      size_t take = CCORD_VOICE_FRAME_SAMPLES - l->tx_pcm_acc_len;
      if (take > samples - off) take = samples - off;
      memcpy(&l->tx_pcm_acc[l->tx_pcm_acc_len], &pcm[off], take * sizeof *pcm);
      l->tx_pcm_acc_len += take;
      off += take;
      if (l->tx_pcm_acc_len < CCORD_VOICE_FRAME_SAMPLES) break;
      uint8_t opus[LIVE_STREAM_OPUS_MAX];
      int n = opus_encode(l->stream_enc, l->tx_pcm_acc, (int)CCORD_VOICE_FRAME_SAMPLES, opus,
                          (opus_int32)sizeof opus);
      l->tx_pcm_acc_len = 0;
      if (n < 0) {
        st = ccord_error_set(err, CCORD_ERR_INTERNAL, "live: stream opus encode failed");
        break;
      }
      st = ccord_vstream_send_audio(l->tx_stream.vs, opus, (size_t)n, l->tx_audio_ts48k, err);
      if (st != CCORD_OK) break;
      l->tx_audio_ts48k += CCORD_VOICE_FRAME_SAMPLES;
    }
  }
  ccord_mutex_unlock(l->media_mtx);
  return st;
}

// Viewer: drain up to cap_samples of decoded 48 kHz mono stream audio from the ring.
size_t live_poll_stream_pcm(ccord_transport *t, int16_t *out_pcm, size_t cap_samples)
{
  live *l = (live *)t;
  if (!out_pcm || cap_samples == 0) return 0;
  ccord_mutex_lock(l->media_mtx);
  if (l->rx_stream.vs) {
    // Drain UDP + decrypted Opus packets directly on the caller (audio pthread) so UI-thread
    // stalls or idle sleeps never starve stream audio or overflow the kernel UDP buffer.
    uint8_t opus[CCORD_JITTER_MAX_PACKET];
    for (int i = 0; i < 64; i++) {
      size_t olen = ccord_vstream_pop_rx_audio(l->rx_stream.vs, opus, sizeof opus);
      if (olen == 0) break;
      if (!l->self_deaf) live_stream_decode_opus_locked(l, opus, olen);
    }
    if (l->self_deaf) {
      l->rx_pcm_head = 0;
      l->rx_pcm_count = 0;
    }
  }
  size_t take = cap_samples < l->rx_pcm_count ? cap_samples : l->rx_pcm_count;
  if (take > 0 && l->rx_pcm) {
    size_t first = take < LIVE_STREAM_PCM_CAP_SAMPLES - l->rx_pcm_head
                     ? take
                     : LIVE_STREAM_PCM_CAP_SAMPLES - l->rx_pcm_head;
    memcpy(out_pcm, &l->rx_pcm[l->rx_pcm_head], first * sizeof *out_pcm);
    if (take > first) memcpy(out_pcm + first, &l->rx_pcm[0], (take - first) * sizeof *out_pcm);
    l->rx_pcm_head = (l->rx_pcm_head + take) % LIVE_STREAM_PCM_CAP_SAMPLES;
    l->rx_pcm_count -= take;
  }
  ccord_mutex_unlock(l->media_mtx);
  return take;
}

ccord_status live_send_voice_video(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k,
                                         bool keyframe, ccord_error *err)
{
  live *l = (live *)t;
  (void)keyframe;
  ccord_error_clear(err);
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty voice video frame");
  if (!l->self_video) return ccord_error_set(err, CCORD_ERR_INVALID, "live: voice camera not enabled");
  ccord_voice_gateway *vg = live_voice_acquire(l);
  if (!vg) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "live: voice gateway not connected");
  ccord_status st = ccord_voice_gateway_send_h264(vg, annexb, len, ts90k, err);
  live_voice_release(l);
  return st;
}

bool live_consume_keyframe_request(ccord_transport *t)
{
  live *l = (live *)t;
  if (!l) return false;
  return atomic_exchange(&l->tx_keyframe_requested, false);
}


