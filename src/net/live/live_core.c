// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "live_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t live_now(const live *l)
{
  return l->now_ms ? l->now_ms(l->clock_ud) : ccord_clock_ms();
}


static void live_chan_guild_from_event(live *l, const ccord_net_event *ev);

bool live_push_with_media(live *l, ccord_live_event *ev, char *owned_media)
{
  if (l->count == LIVE_QUEUE_SIZE) {
    CCORD_LOG_W("live: event queue full, dropping event");
    ccord_live_event_free(ev);
    ccord_free(owned_media);
    return false;
  }
  l->pending[l->tail] = *ev;
  l->pending_media[l->tail] = owned_media;
  if (l->pending[l->tail].ev.type == CCORD_NET_SNAPSHOT) {
    l->pending[l->tail].ev.snapshot = &l->pending[l->tail].snapshot;
  } else if (l->pending[l->tail].ev.type == CCORD_NET_USER_SETTINGS) {
    // The model travels by value with the queued event; repoint at the copy so the answer cannot dangle.
    l->pending[l->tail].ev.user_settings = &l->pending[l->tail].user_settings;
  }
  live_chan_guild_from_event(l, &l->pending[l->tail].ev);
  memset(ev, 0, sizeof *ev);
  ev->seq = -1;
  l->tail = (l->tail + 1u) % LIVE_QUEUE_SIZE;
  l->count++;
  return true;
}

bool live_push(live *l, ccord_live_event *ev)
{
  return live_push_with_media(l, ev, nullptr);
}

static void live_clear_events(live *l)
{
  if (l->have_current) {
    ccord_live_event_free(&l->current);
    ccord_free(l->current_media);
    l->current_media = nullptr;
    l->have_current = false;
  }
  for (size_t i = 0; i < l->count; i++) {
    size_t idx = (l->head + i) % LIVE_QUEUE_SIZE;
    ccord_live_event_free(&l->pending[idx]);
    ccord_free(l->pending_media[idx]);
    l->pending_media[idx] = nullptr;
  }
  l->head = l->tail = l->count = 0;
}

// Drop every queued viewer access unit and PCM sample and release the lazily allocated buffers and decoder.
// Caller holds media_mtx.

static void live_chan_guild_clear_all(live *l)
{
  if (!l) return;
  for (size_t i = 0; i < l->chan_guild_count; i++) {
    ccord_free(l->chan_guilds[i].msg_ids);
    l->chan_guilds[i].msg_ids = nullptr;
    l->chan_guilds[i].msg_count = 0;
    l->chan_guilds[i].msg_cap = 0;
  }
  l->chan_guild_count = 0;
  ccord_idmap_clear(&l->chan_guild_idx);
}

static live_chan_guild *live_chan_guild_lookup(live *l, ccord_id channel_id)
{
  if (!l || channel_id == CCORD_ID_NONE) return nullptr;
  size_t idx = 0;
  if (!ccord_idmap_find(&l->chan_guild_idx, channel_id, &idx)) return nullptr;
  return idx < l->chan_guild_count ? &l->chan_guilds[idx] : nullptr;
}

static const live_chan_guild *live_chan_guild_lookup_const(const live *l, ccord_id channel_id)
{
  if (!l || channel_id == CCORD_ID_NONE) return nullptr;
  size_t idx = 0;
  if (!ccord_idmap_find(&l->chan_guild_idx, channel_id, &idx)) return nullptr;
  return idx < l->chan_guild_count ? &l->chan_guilds[idx] : nullptr;
}

static live_chan_guild *live_chan_guild_ensure(live *l, ccord_id channel_id)
{
  if (!l || channel_id == CCORD_ID_NONE) return nullptr;
  live_chan_guild *existing = live_chan_guild_lookup(l, channel_id);
  if (existing) return existing;
  if (l->chan_guild_count == l->chan_guild_cap) {
    size_t ncap = l->chan_guild_cap ? l->chan_guild_cap * 2u : 64u;
    live_chan_guild *nbuf = ccord_alloc_array(ncap, sizeof *nbuf);
    if (!nbuf) return nullptr;
    if (l->chan_guild_count > 0) memcpy(nbuf, l->chan_guilds, l->chan_guild_count * sizeof *nbuf);
    ccord_free(l->chan_guilds);
    l->chan_guilds = nbuf;
    l->chan_guild_cap = ncap;
  }
  size_t slot = l->chan_guild_count;
  if (!ccord_idmap_put(&l->chan_guild_idx, channel_id, slot)) return nullptr;
  live_chan_guild *row = &l->chan_guilds[l->chan_guild_count++];
  memset(row, 0, sizeof *row);
  row->channel_id = channel_id;
  return row;
}

void live_chan_guild_put(live *l, ccord_id channel_id, ccord_id guild_id)
{
  if (!l || channel_id == CCORD_ID_NONE || guild_id == CCORD_ID_NONE) return;
  live_chan_guild *row = live_chan_guild_ensure(l, channel_id);
  if (row) row->guild_id = guild_id;
}

void live_chan_set_ack(live *l, ccord_id channel_id, ccord_id ack_id)
{
  if (!l || channel_id == CCORD_ID_NONE || ack_id == CCORD_ID_NONE) return;
  live_chan_guild *row = live_chan_guild_ensure(l, channel_id);
  if (row) row->last_ack_id = ack_id;
}

ccord_id live_chan_last_ack(const live *l, ccord_id channel_id)
{
  const live_chan_guild *row = live_chan_guild_lookup_const(l, channel_id);
  return row ? row->last_ack_id : CCORD_ID_NONE;
}

static void live_chan_msg_insert(live *l, ccord_id channel_id, ccord_id msg_id)
{
  if (!l || channel_id == CCORD_ID_NONE || msg_id == CCORD_ID_NONE) return;
  live_chan_guild *row = live_chan_guild_ensure(l, channel_id);
  if (!row) return;
  size_t lo = 0, hi = row->msg_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (row->msg_ids[mid] < msg_id) lo = mid + 1u; else hi = mid;
  }
  if (lo < row->msg_count && row->msg_ids[lo] == msg_id) return;
  if (row->msg_count == LIVE_CHAN_MSG_MAX) {
    if (lo == 0) return;
    memmove(&row->msg_ids[0], &row->msg_ids[1], (lo - 1u) * sizeof *row->msg_ids);
    row->msg_ids[lo - 1u] = msg_id;
    return;
  }
  if (row->msg_count == row->msg_cap) {
    size_t ncap = row->msg_cap ? row->msg_cap * 2u : 32u;
    if (ncap > LIVE_CHAN_MSG_MAX) ncap = LIVE_CHAN_MSG_MAX;
    ccord_id *nbuf = ccord_alloc_array(ncap, sizeof *nbuf);
    if (!nbuf) return;
    if (row->msg_count > 0) memcpy(nbuf, row->msg_ids, row->msg_count * sizeof *nbuf);
    ccord_free(row->msg_ids);
    row->msg_ids = nbuf;
    row->msg_cap = ncap;
  }
  if (lo < row->msg_count) {
    memmove(&row->msg_ids[lo + 1u], &row->msg_ids[lo], (row->msg_count - lo) * sizeof *row->msg_ids);
  }
  row->msg_ids[lo] = msg_id;
  row->msg_count++;
}

static void live_chan_msg_remove(live *l, ccord_id channel_id, ccord_id msg_id)
{
  if (!l || channel_id == CCORD_ID_NONE || msg_id == CCORD_ID_NONE) return;
  live_chan_guild *row = live_chan_guild_lookup(l, channel_id);
  if (!row || row->msg_count == 0) return;
  size_t lo = 0, hi = row->msg_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (row->msg_ids[mid] < msg_id) lo = mid + 1u; else hi = mid;
  }
  if (lo < row->msg_count && row->msg_ids[lo] == msg_id) {
    if (lo + 1u < row->msg_count) {
      memmove(&row->msg_ids[lo], &row->msg_ids[lo + 1u], (row->msg_count - lo - 1u) * sizeof *row->msg_ids);
    }
    row->msg_count--;
  }
}

bool live_chan_prev_msg_id(const live *l, ccord_id channel_id, ccord_id message_id, ccord_id *out_ack_id)
{
  if (!l || !out_ack_id || channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) return false;
  const live_chan_guild *row = live_chan_guild_lookup_const(l, channel_id);
  if (row && row->msg_count > 0) {
    size_t lo = 0, hi = row->msg_count;
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2u;
      if (row->msg_ids[mid] < message_id) lo = mid + 1u; else hi = mid;
    }
    if (lo < row->msg_count && row->msg_ids[lo] == message_id) {
      if (lo > 0) {
        *out_ack_id = row->msg_ids[lo - 1u];
        return true;
      }
      if (message_id <= 1u) return false;
      *out_ack_id = message_id - 1u;
      return true;
    }
  }
  if (message_id <= 1u) return false;
  *out_ack_id = message_id - 1u;
  return true;
}

static void live_chan_guild_remove(live *l, ccord_id channel_id)
{
  if (!l || channel_id == CCORD_ID_NONE) return;
  size_t idx = 0;
  if (!ccord_idmap_find(&l->chan_guild_idx, channel_id, &idx) || idx >= l->chan_guild_count) return;
  ccord_free(l->chan_guilds[idx].msg_ids);
  (void)ccord_idmap_remove(&l->chan_guild_idx, channel_id);
  size_t last = --l->chan_guild_count;
  if (idx != last) {
    l->chan_guilds[idx] = l->chan_guilds[last];
    (void)ccord_idmap_put(&l->chan_guild_idx, l->chan_guilds[idx].channel_id, idx);
  }
}

static void live_chan_guild_remove_guild(live *l, ccord_id guild_id)
{
  if (!l || guild_id == CCORD_ID_NONE) return;
  size_t w = 0;
  for (size_t i = 0; i < l->chan_guild_count; i++) {
    if (l->chan_guilds[i].guild_id != guild_id) {
      l->chan_guilds[w++] = l->chan_guilds[i];
    } else {
      ccord_free(l->chan_guilds[i].msg_ids);
    }
  }
  if (w == l->chan_guild_count) return;
  l->chan_guild_count = w;
  ccord_idmap_clear(&l->chan_guild_idx);
  for (size_t i = 0; i < w; i++) {
    (void)ccord_idmap_put(&l->chan_guild_idx, l->chan_guilds[i].channel_id, i);
  }
}

ccord_id live_chan_guild_find(const live *l, ccord_id channel_id)
{
  const live_chan_guild *row = live_chan_guild_lookup_const(l, channel_id);
  return row ? row->guild_id : CCORD_ID_NONE;
}

static void live_chan_guild_from_event(live *l, const ccord_net_event *ev)
{
  if (!l || !ev) return;
  switch (ev->type) {
    case CCORD_NET_SNAPSHOT:
      live_chan_guild_clear_all(l);
      if (ev->snapshot && ev->snapshot->channels) {
        if (ev->snapshot->channel_count > 0) (void)ccord_idmap_reserve(&l->chan_guild_idx, ev->snapshot->channel_count);
        for (size_t i = 0; i < ev->snapshot->channel_count; i++) {
          const ccord_channel *ch = &ev->snapshot->channels[i];
          live_chan_guild *row = live_chan_guild_ensure(l, ch->id);
          if (row) {
            if (ch->guild_id != CCORD_ID_NONE) row->guild_id = ch->guild_id;
            if (ch->last_read_id != CCORD_ID_NONE) row->last_ack_id = ch->last_read_id;
          }
        }
      }
      break;
    case CCORD_NET_GUILD_UPSERT:
      if (ev->guild_channels) {
        for (size_t i = 0; i < ev->guild_channel_count; i++) {
          const ccord_channel *ch = &ev->guild_channels[i];
          ccord_id gid = ch->guild_id != CCORD_ID_NONE ? ch->guild_id : ev->guild_id;
          live_chan_guild *row = live_chan_guild_ensure(l, ch->id);
          if (row) {
            if (gid != CCORD_ID_NONE) row->guild_id = gid;
            if (ch->last_read_id != CCORD_ID_NONE) row->last_ack_id = ch->last_read_id;
          }
        }
      }
      break;
    case CCORD_NET_CHANNEL_UPSERT:
      if (ev->guild_channels && ev->guild_channel_count > 0) {
        for (size_t i = 0; i < ev->guild_channel_count; i++) {
          const ccord_channel *ch = &ev->guild_channels[i];
          ccord_id gid = ch->guild_id != CCORD_ID_NONE ? ch->guild_id : ev->guild_id;
          live_chan_guild *row = live_chan_guild_ensure(l, ch->id);
          if (row) {
            if (gid != CCORD_ID_NONE) row->guild_id = gid;
            if (ch->last_read_id != CCORD_ID_NONE) row->last_ack_id = ch->last_read_id;
          }
        }
      } else if (ev->channel) {
        ccord_id gid = ev->channel->guild_id != CCORD_ID_NONE ? ev->channel->guild_id : ev->guild_id;
        live_chan_guild *row = live_chan_guild_ensure(l, ev->channel->id);
        if (row) {
          if (gid != CCORD_ID_NONE) row->guild_id = gid;
          if (ev->channel->last_read_id != CCORD_ID_NONE) row->last_ack_id = ev->channel->last_read_id;
        }
      } else if (ev->channel_id != CCORD_ID_NONE && ev->guild_id != CCORD_ID_NONE) {
        live_chan_guild_put(l, ev->channel_id, ev->guild_id);
      }
      break;
    case CCORD_NET_CHANNEL_DELETE:
      live_chan_guild_remove(l, ev->channel_id);
      break;
    case CCORD_NET_GUILD_DELETE:
      live_chan_guild_remove_guild(l, ev->guild_id);
      break;
    case CCORD_NET_HISTORY:
      if (ev->messages) {
        for (size_t i = 0; i < ev->message_count; i++) {
          const ccord_message *m = &ev->messages[i];
          ccord_id cid = m->channel_id != CCORD_ID_NONE ? m->channel_id : ev->channel_id;
          if (m->send_state == CCORD_MSG_SEND_CONFIRMED) live_chan_msg_insert(l, cid, m->id);
        }
      }
      break;
    case CCORD_NET_MESSAGE_CREATE:
    case CCORD_NET_MESSAGE_UPDATE:
      if (ev->channel_id != CCORD_ID_NONE && ev->guild_id != CCORD_ID_NONE) {
        live_chan_guild_put(l, ev->channel_id, ev->guild_id);
      }
      if (ev->messages) {
        for (size_t i = 0; i < ev->message_count; i++) {
          const ccord_message *m = &ev->messages[i];
          ccord_id cid = m->channel_id != CCORD_ID_NONE ? m->channel_id : ev->channel_id;
          if (m->send_state == CCORD_MSG_SEND_CONFIRMED) live_chan_msg_insert(l, cid, m->id);
        }
      }
      break;
    case CCORD_NET_MESSAGE_DELETE:
      live_chan_msg_remove(l, ev->channel_id, ev->message_id);
      break;
    case CCORD_NET_MESSAGE_ACK:
      if (ev->message_id != CCORD_ID_NONE) live_chan_set_ack(l, ev->channel_id, ev->message_id);
      break;
    default:
      break;
  }
}



static uint64_t live_media_prog_hash(const char *url)
{
  uint64_t h = 14695981039346656037ULL;
  for (const unsigned char *p = (const unsigned char *)url; p && *p; p++) {
    h ^= (uint64_t)*p;
    h *= 1099511628211ULL;
  }
  return h ? h : 1ULL;
}

static void live_media_prog_clear(live *l, const char *url)
{
  if (!l || !url || !url[0]) return;
  uint64_t h = live_media_prog_hash(url);
  for (size_t i = 0; i < sizeof l->media_prog / sizeof l->media_prog[0]; i++) {
    if (l->media_prog[i].active && l->media_prog[i].url_hash == h) {
      l->media_prog[i].active = false;
      l->media_prog[i].url_hash = 0;
      l->media_prog[i].now = 0;
      l->media_prog[i].total = 0;
    }
  }
}

static void live_media_progress_cb(void *ud, uint64_t dl_bytes, uint64_t total_bytes)
{
  live_rest_op *op = ud;
  if (!op || !op->l || op->kind != LIVE_REST_MEDIA || !op->media_url || !op->media_url[0]) return;
  live *l = op->l;
  uint64_t h = live_media_prog_hash(op->media_url);
  size_t free_slot = SIZE_MAX;
  const size_t cap = sizeof l->media_prog / sizeof l->media_prog[0];
  for (size_t i = 0; i < cap; i++) {
    if (l->media_prog[i].active && l->media_prog[i].url_hash == h) {
      l->media_prog[i].now = dl_bytes;
      l->media_prog[i].total = total_bytes;
      return;
    }
    if (!l->media_prog[i].active && free_slot == SIZE_MAX) free_slot = i;
  }
  size_t slot = free_slot != SIZE_MAX ? free_slot : (size_t)(h % cap);
  l->media_prog[slot].url_hash = h;
  l->media_prog[slot].now = dl_bytes;
  l->media_prog[slot].total = total_bytes;
  l->media_prog[slot].active = true;
}

static bool live_media_progress(const ccord_transport *t, const char *url, uint64_t *out_now, uint64_t *out_total)
{
  const live *l = (const live *)t;
  if (!l || !url || !url[0]) return false;
  uint64_t h = live_media_prog_hash(url);
  for (size_t i = 0; i < sizeof l->media_prog / sizeof l->media_prog[0]; i++) {
    if (l->media_prog[i].active && l->media_prog[i].url_hash == h) {
      if (out_now) *out_now = l->media_prog[i].now;
      if (out_total) *out_total = l->media_prog[i].total;
      return true;
    }
  }
  return false;
}

void live_rest_op_free(live_rest_op *op)
{
  if (!op) return;
  if (op->kind == LIVE_REST_MEDIA && op->l && op->media_url) {
    live_media_prog_clear(op->l, op->media_url);
  }
  if (op->upload) {
    live_upload_state *u = op->upload;
    if (u->upload_linked && op->l) {
      if (u->upload_prev) u->upload_prev->upload->upload_next = u->upload_next;
      else op->l->upload_head = u->upload_next;
      if (u->upload_next) u->upload_next->upload->upload_prev = u->upload_prev;
      else op->l->upload_tail = u->upload_prev;
      u->upload_linked = false;
      u->upload_prev = u->upload_next = nullptr;
    }
    for (size_t i = 0; i < CCORD_RICH_MAX_ATTACHMENTS; i++) {
      ccord_free(u->filenames[i]);
      ccord_free(u->file_datas[i]);
      ccord_free(u->file_waveforms[i]);
    }
    ccord_free(u);
    op->upload = nullptr;
  }
  ccord_free(op->reorder);
  ccord_free(op->voice_status);
  ccord_free(op->topic);
  ccord_free(op->expr_arg);
  ccord_free(op->emoji);
  ccord_free(op->text);
  ccord_free(op->folder_items);
  ccord_free(op->media_url);
  ccord_free(op->media_path);
  ccord_free(op->cache_path);
  ccord_free(op->proto_data);
  ccord_free(op);
}

// Why a media download produced no bytes, for the log: reason and host only, never the path or query string (signed
// CDN URLs carry tokens). The UI writes its own `player:` line when the failed URL is a clicked video.
void live_log_media_failure(const char *url, ccord_status st, long http_status)
{
  char reason[96];
  if (st != CCORD_OK) {
    const char *why = st == CCORD_ERR_TIMEOUT       ? "timed out (stalled, over the time ceiling or queued too long)"
                      : st == CCORD_ERR_RANGE       ? "over the size cap"
                      : st == CCORD_ERR_UNSUPPORTED ? "unsupported protocol"
                      : st == CCORD_ERR_IO          ? "network error"
                                                    : ccord_status_name(st);
    (void)snprintf(reason, sizeof reason, "%s", why);
  } else if (http_status >= 300 && http_status < 400) {
    (void)snprintf(reason, sizeof reason, "HTTP %ld (redirects are not followed)", http_status);
  } else {
    (void)snprintf(reason, sizeof reason, "HTTP %ld", http_status);
  }
  char host[128] = "?";
  const char *p = url ? strstr(url, "://") : nullptr;
  if (p) {
    p += 3;
    size_t n = strcspn(p, "/?#");
    const char *at = memchr(p, '@', n);   // userinfo is not the host
    if (at) {
      n -= (size_t)(at + 1 - p);
      p = at + 1;
    }
    const char *colon = memchr(p, ':', n);
    if (colon) n = (size_t)(colon - p);
    if (n > 0 && n < sizeof host) {
      memcpy(host, p, n);
      host[n] = '\0';
    }
  }
  CCORD_LOG_W("live: media download %s host=%s", reason, host);
}


// How urgent a request is depends on who is waiting for it (see governor.h): a direct user action is never made to
// queue behind prefetching, data for what is on screen beats housekeeping, and read-state acks go last.
// No default case: adding a kind forces a decision here.
ccord_gov_priority live_kind_priority(live_rest_kind kind)
{
  switch (kind) {
    case LIVE_REST_OPEN_DM:
    case LIVE_REST_UPLOAD_SLOT:
    case LIVE_REST_UPLOAD_PUT:
    case LIVE_REST_UPLOAD_SEND:
    case LIVE_REST_RELATIONSHIP:
    case LIVE_REST_FORUM_CREATE:
    case LIVE_REST_SEND_STICKER:
    case LIVE_REST_SEND_MESSAGE:
    case LIVE_REST_CREATE_POLL:
    case LIVE_REST_REPORT_MENU:
    case LIVE_REST_REPORT:
    case LIVE_REST_SEND_REPLY:
    case LIVE_REST_EDIT_MESSAGE:
    case LIVE_REST_DELETE_MESSAGE:
    case LIVE_REST_REACTION:
    case LIVE_REST_PIN:
    case LIVE_REST_ADMIN:
    case LIVE_REST_SOUNDBOARD:
    case LIVE_REST_CALL_RING:
    case LIVE_REST_POLL_VOTE:
    case LIVE_REST_INTERACTION:
    case LIVE_REST_SEND_TTS:
    case LIVE_REST_FORWARD_MESSAGE:
    case LIVE_REST_CROSSPOST_MESSAGE:
    case LIVE_REST_MARK_UNREAD:
    case LIVE_REST_PATCH_SETTINGS_PROTO:
    case LIVE_REST_STATUS_GET:
    case LIVE_REST_STATUS_PATCH:
    case LIVE_REST_MEDIA_RANGE:      // a click is waiting on it, and raw jobs are not governed anyway
      return CCORD_GOV_USER;
    case LIVE_REST_HISTORY:
    case LIVE_REST_HISTORY_BEFORE:
    case LIVE_REST_PINS:
    case LIVE_REST_PINS_BEFORE:
    case LIVE_REST_SEARCH:
    case LIVE_REST_MEDIA:
    case LIVE_REST_USER_PROFILE:
    case LIVE_REST_PROFILE_EFFECTS:
    case LIVE_REST_COLLECTIBLE_PRODUCT:
    case LIVE_REST_CHANNEL:
    case LIVE_REST_FORUM_POSTS:
    case LIVE_REST_EXPRESSION_DATA:
    case LIVE_REST_TYPING:           // droppable, and must respect backoff rather than bypass it like a user action
    case LIVE_REST_REACTION_REFRESH:
    case LIVE_REST_REACTION_USERS:
    case LIVE_REST_INTERACTION_AUTOCOMPLETE:
    case LIVE_REST_SLASH_COMMANDS:
    case LIVE_REST_DISCOVERY:
      return CCORD_GOV_VISIBLE;
    case LIVE_REST_ACK:
      return CCORD_GOV_BACKGROUND;
  }
  return CCORD_GOV_VISIBLE;
}

// Every queued request goes through here so none can skip the governor or forget a priority. Identical in-flight
// GETs are coalesced into one request on the wire.
ccord_status live_queue_add(live *l, ccord_rest_method method, const char *path, const char *json_body,
                                   live_rest_op *op, ccord_error *err)
{
  ccord_rest_request r = ccord_rest_request_make(method, path, live_kind_priority(op->kind));
  r.json_body = json_body;
  // Only reads whose staleness does not matter are coalesced: a reaction refresh must reflect the change that
  // triggered it, not an older request that happens to be in flight.
  r.dedupe = method == CCORD_REST_GET && op->kind != LIVE_REST_REACTION_REFRESH && op->kind != LIVE_REST_REACTION_USERS;
  r.cb = live_rest_done;
  r.ud = op;
  return ccord_rest_queue_submit(l->restq, &r, err);
}

static void live_upload_progress_cb(void *ud, uint64_t uploaded_bytes, uint64_t total_bytes)
{
  (void)total_bytes;
  live_rest_op *op = ud;
  if (!op || !op->upload || op->kind != LIVE_REST_UPLOAD_PUT || op->upload->upload_idx >= op->upload->file_count) return;
  live_upload_state *u = op->upload;
  uint64_t cap = (uint64_t)u->upload_orig_sizes[u->upload_idx];
  u->upload_cur_file_bytes = uploaded_bytes > cap ? cap : uploaded_bytes;
}

ccord_status live_queue_add_raw(live *l, ccord_rest_method method, const char *url, const char *content_type,
                                       const void *body, size_t body_len, live_rest_op *op, ccord_error *err)
{
  ccord_rest_request r = ccord_rest_request_make(method, nullptr, live_kind_priority(op->kind));
  r.full_url = url;
  r.content_type = content_type;
  r.body = body;
  r.body_len = body_len;
  r.cb = live_rest_done;
  r.ud = op;
  if (op->kind == LIVE_REST_MEDIA) {
    r.bulk = true;
    r.max_response = CCORD_LIVE_MAX_MEDIA;
    r.progress_cb = live_media_progress_cb;
  } else if (op->kind == LIVE_REST_UPLOAD_PUT) {
    r.bulk = true;
    r.progress_cb = live_upload_progress_cb;
  }
  return ccord_rest_queue_submit(l->restq, &r, err);
}

// A signed Discord attachment URL (?ex=&is=&hm=) stops working when `ex` passes: the CDN answers 404 or 403. Discord's
// own client then asks POST /attachments/refresh-urls for a fresh signature, which is what a GIF favorite saved days ago
// or an old embed needs. Only a signed https attachment URL on Discord's own CDN hosts qualifies.
// A media GET the CDN refused as expired: ask for a fresh signature (the op becomes the refresh request). False when
// the failure is not that case or the request could not be queued; the op is then still the caller's.
bool live_media_try_refresh(live *l, live_rest_op *op, long http_status)
{
  if (op->kind != LIVE_REST_MEDIA || op->media_stage != 0 || !l->restq) return false;
  if (http_status != 403 && http_status != 404 && http_status != 410) return false;
  if (!ccord_live_media_url_refreshable(op->media_url)) return false;
  ccord_buf body;
  ccord_buf_init(&body, 0);
  ccord_error err;
  ccord_status st = ccord_live_media_refresh_body(op->media_url, &body, &err);
  if (st == CCORD_OK) {
    op->media_stage = 1;
    st = live_queue_add(l, CCORD_REST_POST, "/attachments/refresh-urls", ccord_buf_cstr(&body), op, &err);
    if (st != CCORD_OK) op->media_stage = 0;
  }
  ccord_buf_free(&body);
  return st == CCORD_OK;
}

// The refresh answered: fetch the fresh URL (same allow-list gate, same op, so the bytes arrive under the URL the UI
// asked for). False on anything unusable; the op is then still the caller's.
bool live_media_refreshed(live *l, live_rest_op *op, const char *body, size_t len)
{
  char fresh[2100];
  if (!ccord_live_media_refresh_parse(body, len, op->media_url, fresh, sizeof fresh)) return false;
  if (!ccord_live_media_url_allowed(fresh) || strcmp(fresh, op->media_url) == 0) return false;
  ccord_error err;
  op->media_stage = 2;
  return live_queue_add_raw(l, CCORD_REST_GET, fresh, nullptr, nullptr, 0, op, &err) == CCORD_OK;
}

live_rest_op *live_action_op(live *l, live_rest_kind kind, ccord_id channel_id, ccord_id message_id,
                                    const char *draft, size_t draft_len)
{
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return nullptr;
  op->l = l;
  op->kind = kind;
  op->channel_id = channel_id;
  op->message_id = message_id;
  if (draft) {
    op->text = ccord_strndup(draft, draft_len);
    op->text_len = draft_len;
    if (!op->text) {
      live_rest_op_free(op);
      return nullptr;
    }
  }
  return op;
}

// Queues a user action and returns at once. The outcome is reported from live_rest_done: success events as before,
// or CCORD_NET_ACTION_FAILED. Takes ownership of `op` (freed here on failure).
ccord_status live_action_submit(live *l, ccord_rest_method method, const char *path, const char *json_body,
                                       live_rest_op *op, ccord_error *err)
{
  if (!l->restq) {
    live_rest_op_free(op);
    return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  }
  ccord_status st = live_queue_add(l, method, path, json_body, op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

const char *live_rest_name(live_rest_kind kind)
{
  switch (kind) {
    case LIVE_REST_HISTORY: return "history";
    case LIVE_REST_HISTORY_BEFORE: return "history before";
    case LIVE_REST_PINS: return "pins";
    case LIVE_REST_PINS_BEFORE: return "pins before";
    case LIVE_REST_ACK: return "ack";
    case LIVE_REST_SEARCH: return "search";
    case LIVE_REST_OPEN_DM: return "open dm";
    case LIVE_REST_MEDIA: return "media";
    case LIVE_REST_MEDIA_RANGE: return "media range";
    case LIVE_REST_UPLOAD_SLOT: return "upload slot";
    case LIVE_REST_UPLOAD_PUT: return "upload put";
    case LIVE_REST_UPLOAD_SEND: return "upload send";
    case LIVE_REST_RELATIONSHIP: return "relationship";
    case LIVE_REST_USER_PROFILE: return "user profile";
    case LIVE_REST_PROFILE_EFFECTS: return "profile effects";
    case LIVE_REST_COLLECTIBLE_PRODUCT: return "collectible product";
    case LIVE_REST_CHANNEL: return "channel";
    case LIVE_REST_FORUM_POSTS: return "forum posts";
    case LIVE_REST_FORUM_CREATE: return "create forum post";
    case LIVE_REST_EXPRESSION_DATA: return "expression data";
    case LIVE_REST_SEND_STICKER: return "send sticker";
    case LIVE_REST_SEND_MESSAGE: return "send";
    case LIVE_REST_CREATE_POLL: return "create poll";
    case LIVE_REST_REPORT_MENU: return "report menu";
    case LIVE_REST_REPORT: return "report";
    case LIVE_REST_SEND_REPLY: return "reply";
    case LIVE_REST_EDIT_MESSAGE: return "edit";
    case LIVE_REST_DELETE_MESSAGE: return "delete";
    case LIVE_REST_TYPING: return "typing";
    case LIVE_REST_REACTION: return "reaction";
    case LIVE_REST_REACTION_REFRESH: return "reaction refresh";
    case LIVE_REST_REACTION_USERS: return "reaction users";
    case LIVE_REST_PIN: return "pin";
    case LIVE_REST_ADMIN: return "admin";
    case LIVE_REST_SOUNDBOARD: return "soundboard";
    case LIVE_REST_CALL_RING: return "call ring";
    case LIVE_REST_POLL_VOTE: return "poll vote";
    case LIVE_REST_INTERACTION: return "interaction";
    case LIVE_REST_INTERACTION_AUTOCOMPLETE: return "slash autocomplete";
    case LIVE_REST_SLASH_COMMANDS: return "slash commands";
    case LIVE_REST_DISCOVERY: return "discovery";
    case LIVE_REST_SEND_TTS: return "send tts";
    case LIVE_REST_FORWARD_MESSAGE: return "forward";
    case LIVE_REST_CROSSPOST_MESSAGE: return "crosspost";
    case LIVE_REST_MARK_UNREAD: return "mark unread";
    case LIVE_REST_PATCH_SETTINGS_PROTO: return "patch settings proto";
    case LIVE_REST_STATUS_GET: return "status proto get";
    case LIVE_REST_STATUS_PATCH: return "status proto patch";
  }
  return "rest";
}

void live_ensure_live_cache_dirs(const char *file_path)
{
  size_t len = strlen(file_path);
  if (len == 0 || len >= CCORD_PATH_MAX) return;
  char tmp[CCORD_PATH_MAX];
  memcpy(tmp, file_path, len + 1);
  char *slash = strrchr(tmp, '/');
#if defined(_WIN32)
  char *bs = strrchr(tmp, '\\');
  if (bs && (!slash || bs > slash)) slash = bs;
#endif
  if (!slash || slash == tmp) return;
  *slash = '\0';
  ccord_error derr;
  (void)ccord_mkdir_p(tmp, 0700, &derr);
}

// The API cache is on for the official API base, and for any base once the caller scopes it to a per-account dir.
bool live_api_cache_enabled(const live *l)
{
  return l && (l->cache_dir[0] != '\0' || strcmp(l->api_base, CCORD_LIVE_API_BASE) == 0);
}

// With a per-account cache_dir: "<cache_dir>/api/<fnv16>.json" (so two accounts never share an entry). Without one
// the legacy shared "$XDG_CACHE_HOME/ccord/api/<fnv16>.json" path is kept.
bool live_api_cache_path(const live *l, const char *path, char *out, size_t cap)
{
  if (!live_api_cache_enabled(l) || !path || !out || cap < 64) return false;
  uint64_t h = 14695981039346656037ull;
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
    h ^= (uint64_t)*p;
    h *= 1099511628211ull;
  }
  int n = -1;
  if (l->cache_dir[0] != '\0') {
    n = snprintf(out, cap, "%s/api/%016llx.json", l->cache_dir, (unsigned long long)h);
    return n > 0 && (size_t)n < cap;
  }
  const char *xdg = getenv("XDG_CACHE_HOME");
  const char *home = getenv("HOME");
  if (xdg && xdg[0] != '\0') {
    n = snprintf(out, cap, "%s/ccord/api/%016llx.json", xdg, (unsigned long long)h);
  } else if (home && home[0] != '\0') {
    n = snprintf(out, cap, "%s/.cache/ccord/api/%016llx.json", home, (unsigned long long)h);
  }
  return n > 0 && (size_t)n < cap;
}

bool ccord_live_history_cache_path(const char *cache_dir, ccord_id channel_id, char *out, size_t cap)
{
  if (!cache_dir || cache_dir[0] == '\0' || !out || cap == 0) return false;
  int n = snprintf(out, cap, "%s/warm/history/%llu.json", cache_dir, (unsigned long long)channel_id);
  return n > 0 && (size_t)n < cap;
}

char *live_api_cache_read(const char *cache_path, uint32_t ttl_sec, size_t *out_len)
{
  if (out_len) *out_len = 0;
  if (!cache_path || cache_path[0] == '\0') return nullptr;
  uint64_t fsz = 0;
  int64_t mtime_ns = 0;
  if (!ccord_file_size(cache_path, &fsz) || fsz == 0 || fsz > (uint64_t)CCORD_LIVE_MAX_RESPONSE ||
      !ccord_file_mtime_ns(cache_path, &mtime_ns)) {
    return nullptr;
  }
  if (ttl_sec > 0) {
    int64_t now_s = (int64_t)time(nullptr);
    int64_t mtime_s = mtime_ns / 1000000000LL;
    if (now_s > mtime_s && (uint64_t)(now_s - mtime_s) > (uint64_t)ttl_sec) return nullptr;
  }
  FILE *fp = fopen(cache_path, "rb");
  if (!fp) return nullptr;
  size_t sz = (size_t)fsz;
  char *buf = ccord_alloc_array(sz + 1u, 1);
  if (!buf) {
    fclose(fp);
    return nullptr;
  }
  size_t rd = fread(buf, 1, sz, fp);
  fclose(fp);
  if (rd != sz) {
    ccord_free(buf);
    return nullptr;
  }
  buf[sz] = '\0';
  if (out_len) *out_len = sz;
  return buf;
}

void live_api_cache_write(const char *cache_path, const char *body, size_t len)
{
  if (!cache_path || cache_path[0] == '\0' || !body || len == 0) return;
  char dir[4096];
  const char *slash = strrchr(cache_path, '/');
  if (slash && slash != cache_path && (size_t)(slash - cache_path) < sizeof dir) {
    memcpy(dir, cache_path, (size_t)(slash - cache_path));
    dir[slash - cache_path] = '\0';
    ccord_error e;
    (void)ccord_mkdir_p(dir, 0700, &e);
  }
  ccord_error werr;
  if (ccord_file_write_atomic(cache_path, body, len, 0600, false, &werr) != CCORD_OK) {
    CCORD_LOG_W("live: api cache write failed: %s", werr.message);
  }
}

// Keeps the 8 most recently written files in <cache_dir>/warm/history.
#define LIVE_WARM_HISTORY_KEEP 8u
#define LIVE_WARM_HISTORY_MAX_BODY (512u * 1024u)

typedef struct live_hist_file {
  char name[64];
  int64_t mtime_ns;
  bool just_written;
} live_hist_file;

static int live_hist_file_newest_first(const void *a, const void *b)
{
  const live_hist_file *x = a, *y = b;
  if (x->just_written != y->just_written) return x->just_written ? -1 : 1;
  return (x->mtime_ns < y->mtime_ns) - (x->mtime_ns > y->mtime_ns);
}

static void live_warm_history_prune(const char *dir, const char *just_written)
{
  ccord_dir *d = ccord_dir_open(dir);
  if (!d) return;
  live_hist_file files[128];
  size_t n = 0;
  char name[256];
  bool is_dir = false;
  char full[CCORD_PATH_MAX];
  while (ccord_dir_next(d, name, sizeof name, &is_dir) && n < sizeof files / sizeof files[0]) {
    if (is_dir || name[0] == '.' || strlen(name) >= sizeof files[0].name) continue;
    int w = snprintf(full, sizeof full, "%s/%s", dir, name);
    int64_t mt = 0;
    if (w < 0 || (size_t)w >= sizeof full || !ccord_file_mtime_ns(full, &mt)) continue;
    memcpy(files[n].name, name, strlen(name) + 1);
    files[n].mtime_ns = mt;
    files[n].just_written = just_written && strcmp(name, just_written) == 0;
    n++;
  }
  ccord_dir_close(d);
  if (n <= LIVE_WARM_HISTORY_KEEP) return;
  qsort(files, n, sizeof files[0], live_hist_file_newest_first);
  for (size_t i = LIVE_WARM_HISTORY_KEEP; i < n; i++) {
    int w = snprintf(full, sizeof full, "%s/%s", dir, files[i].name);
    if (w > 0 && (size_t)w < sizeof full) (void)ccord_file_unlink(full);
  }
}

// Persists a channel's initial history page for the next warm start. Best effort; failures are only logged.
void live_warm_history_write(const live *l, ccord_id channel_id, const char *body, size_t len)
{
  if (l->cache_dir[0] == '\0' || !body || len == 0 || len > LIVE_WARM_HISTORY_MAX_BODY) return;
  char file[4096], dir[4096];
  if (!ccord_live_history_cache_path(l->cache_dir, channel_id, file, sizeof file)) return;
  if (snprintf(dir, sizeof dir, "%s/warm/history", l->cache_dir) >= (int)sizeof dir) return;
  ccord_error e;
  if (ccord_mkdir_p(dir, 0700, &e) != CCORD_OK ||
      ccord_file_write_atomic(file, body, len, 0600, false, &e) != CCORD_OK) {
    CCORD_LOG_W("live: warm history write failed: %s", e.message);
    return;
  }
  const char *base = strrchr(file, '/');
  live_warm_history_prune(dir, base ? base + 1 : nullptr);
}

void live_api_cache_invalidate_channel_history(const live *l, ccord_id channel_id)
{
  if (!l || channel_id == CCORD_ID_NONE) return;
  char path[128], cpath[4096];
  snprintf(path, sizeof path, "/channels/%llu/messages?limit=%u", (unsigned long long)channel_id,
           (unsigned)CCORD_LIVE_HISTORY_DEFAULT);
  if (live_api_cache_path(l, path, cpath, sizeof cpath)) {
    (void)ccord_file_unlink(cpath);
  }
  if (ccord_live_history_cache_path(l->cache_dir, channel_id, cpath, sizeof cpath)) (void)ccord_file_unlink(cpath);
}

// Zero-copy media event: takes ownership of `owned_data` (freed on failure or when the event expires on next poll).

ccord_status live_enqueue_rest_cached(live *l, ccord_rest_method method, const char *path, live_rest_kind kind,
                                             ccord_id channel_id, const char *cache_path, ccord_error *err)
{
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  live_rest_op *op = ccord_calloc(1, sizeof *op);
  if (!op) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  op->l = l;
  op->kind = kind;
  op->channel_id = channel_id;
  if (cache_path && cache_path[0] != '\0') {
    op->cache_path = ccord_strndup(cache_path, strlen(cache_path));
  }
  ccord_status st = live_queue_add(l, method, path, nullptr, op, err);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}

ccord_status live_enqueue_rest(live *l, ccord_rest_method method, const char *path, live_rest_kind kind,
                                      ccord_id channel_id, ccord_error *err)
{
  return live_enqueue_rest_cached(l, method, path, kind, channel_id, nullptr, err);
}



static ccord_status live_connect(ccord_transport *t, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (l->state != CCORD_CONN_DISCONNECTED) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: already connected");
  }
  if (l->gw) {
    ccord_status st = ccord_gateway_start(l->gw, err);
    if (st != CCORD_OK) {
      l->state = CCORD_CONN_FAILED;
      l->reported = CCORD_CONN_FAILED;
      return st;
    }
    l->state = CCORD_CONN_CONNECTING;
  } else {
    l->state = CCORD_CONN_CONNECTED;
  }
  l->reported = CCORD_CONN_DISCONNECTED;
  return CCORD_OK;
}

static void live_disconnect(ccord_transport *t)
{
  live *l = (live *)t;
  live_streams_stop_all(l);
  l->stream_peer_count = 0;
  live_voice_stop(l);
  l->voice_guild = CCORD_ID_NONE;
  l->voice_channel = CCORD_ID_NONE;
  l->self_video = false;
  l->streaming = false;
  l->self_suppressed = false;
  l->dm_call_count = 0;
  l->voice_cache_count = 0;
  live_chan_guild_clear_all(l);
  if (l->gw) ccord_gateway_stop(l->gw);
  live_clear_events(l);
  live_clear_pending_decor_profiles(l);
  l->state = CCORD_CONN_DISCONNECTED;
  l->reported = CCORD_CONN_CONNECTED;
}

// Placeholder until ccord_gateway_wake (gateway task) exists: applies only the design's step 1 (a gateway that is not
// connected or connecting has nothing to reconnect -> IGNORED) and plans nothing else, so a live gateway reports
// COALESCED ("no reconnect planned"). The gateway task (T2) replaces the body with
// `return ccord_gateway_wake(l->gw, kind);`.
static ccord_gwpace_wake_verdict live_gateway_wake(live *l, ccord_link_event kind)
{
  if (!l->gw) return CCORD_GWPACE_WAKE_IGNORED;
  return ccord_gateway_wake(l->gw, kind);
}

// Voice hook: called for every
// wake verdict except IGNORED. A sleep/wake or network change makes the voice link suspect, so a pending attempt runs
// now and an ESTABLISHED gateway is treated as dead (resumed at once). At most one nudge per CCORD_VC_EVENT_MIN_GAP_MS;
// it never resets the attempt count.
static void live_voice_link_event(live *l, ccord_link_event kind)
{
  (void)kind;
  if (l->voice_guild == CCORD_ID_NONE) return;
  uint64_t now = live_now(l);
  if (!ccord_vc_accept_network_event(&l->vc, now)) return;
  if (l->voice) ccord_voice_gateway_network_event(l->voice);
  if (l->tx_stream.vs) ccord_vstream_network_event(l->tx_stream.vs);
  if (l->rx_stream.vs) ccord_vstream_network_event(l->rx_stream.vs);
  if (l->vlink == LV_WAIT_ATTEMPT || l->vlink == LV_WAIT_MAIN_GW) l->vlink_at_ms = now;
}

static ccord_status live_network_event(ccord_transport *t, ccord_link_event kind, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (kind == CCORD_LINK_OFFLINE) {
    if (l->gw) ccord_gateway_set_network_online(l->gw, false);
    return CCORD_OK;
  }
  if (kind == CCORD_LINK_ONLINE) {
    if (l->gw) ccord_gateway_set_network_online(l->gw, true);
    live_voice_link_event(l, CCORD_LINK_CHANGED);
    return CCORD_OK;
  }
  if (kind == CCORD_LINK_PROBE) {
    ccord_gwpace_wake_verdict verdict = l->gw ? ccord_gateway_probe(l->gw) : CCORD_GWPACE_WAKE_IGNORED;
    if (verdict == CCORD_GWPACE_WAKE_ACCEPTED) live_voice_link_event(l, CCORD_LINK_RESUMED);
    return CCORD_OK;
  }
  if (kind != CCORD_LINK_RESUMED && kind != CCORD_LINK_CHANGED) return CCORD_OK;
  ccord_gwpace_wake_verdict verdict = live_gateway_wake(l, kind);
  static const char *const names[] = {"ignored", "fresh", "coalesced", "accepted"};
  CCORD_LOG_D("live: network event %s -> wake %s", kind == CCORD_LINK_CHANGED ? "changed" : "resumed",
              (unsigned)verdict < 4 ? names[verdict] : "?");
  if (verdict != CCORD_GWPACE_WAKE_IGNORED) live_voice_link_event(l, kind);
  return CCORD_OK;
}

static void live_set_background(ccord_transport *t, bool background)
{
  live *l = (live *)t;
  if (l && l->gw) ccord_gateway_set_background(l->gw, background);
}

static void live_set_suspended(ccord_transport *t, bool suspended)
{
  live *l = (live *)t;
  if (l && l->gw) ccord_gateway_set_suspended(l->gw, suspended);
}

static void live_drain_voice_speaking(live *l)
{
  ccord_id spk_uid = CCORD_ID_NONE;
  bool spk_on = false;
  while (ccord_voice_gateway_poll_speaking(l->voice, &spk_uid, &spk_on)) {
    ccord_live_event sev;
    ccord_live_event_init(&sev);
    sev.ev.type = CCORD_NET_SPEAKING;
    sev.ev.guild_id = l->voice_guild;
    sev.ev.channel_id = l->voice_channel;
    sev.ev.user_id = spk_uid;
    sev.ev.speaking = spk_on;
    (void)live_push(l, &sev);
  }
}

bool live_poll(ccord_transport *t, ccord_net_event *out)
{
  live *l = (live *)t;
  if (!out) return false;
  if (l->have_current) {
    ccord_live_event_free(&l->current);
    ccord_free(l->current_media);
    l->current_media = nullptr;
    l->have_current = false;
  }
  uint64_t now = live_now(l);
  if (l->restq) ccord_rest_queue_pump(l->restq, now);
  if (l->vresolve) {
    ccord_video_resolved vr;
    while (ccord_video_resolver_poll(l->vresolve, &vr)) {
      ccord_live_event vev;
      ccord_live_event_init(&vev);
      vev.ev.type = CCORD_NET_VIDEO_RESOLVED;
      vev.ev.status = vr.status;
      vev.ev.resolve_id = vr.req_id;
      vev.ev.resolved_duration_ms = vr.duration_ms;
      vev.ev.resolved_missing_helper = vr.missing_helper;
      vev.ev.media_url = ccord_arena_strndup(&vev.arena, vr.page_url, strlen(vr.page_url));
      vev.ev.resolved_video_url = ccord_arena_strndup(&vev.arena, vr.video_url, strlen(vr.video_url));
      vev.ev.resolved_audio_url = ccord_arena_strndup(&vev.arena, vr.audio_url, strlen(vr.audio_url));
      vev.ev.text = ccord_arena_strndup(&vev.arena, vr.error, strlen(vr.error));
      if (!vev.ev.media_url) vev.ev.media_url = "";
      if (!vev.ev.resolved_video_url) vev.ev.resolved_video_url = "";
      if (!vev.ev.resolved_audio_url) vev.ev.resolved_audio_url = "";
      if (!vev.ev.text) vev.ev.text = "";
      (void)live_push(l, &vev);
    }
  }
  if (l->voice) {
    if (atomic_load(&l->ptt_active) && now >= atomic_load(&l->ptt_release_ms)) {
      ccord_voice_gateway_set_ptt(l->voice, false);
      atomic_store(&l->ptt_active, false);
    }
    live_soundboard_feed(l);
    ccord_voice_event vev;
    for (int vround = 0; vround < 32 && ccord_voice_gateway_poll(l->voice, &vev); vround++) {
      live_drain_voice_speaking(l);
    }
    live_drain_voice_speaking(l);
    if (ccord_voice_gateway_state(l->voice) == CCORD_VOICE_ESTABLISHED && l->self_video && !l->voice_video_synced) {
      ccord_error verr;
      if (ccord_voice_gateway_set_video(l->voice, true, &verr) == CCORD_OK) {
        l->voice_video_synced = true;
      }
    }
    if (ccord_voice_gateway_consume_keyframe_request(l->voice)) {
      atomic_store(&l->tx_keyframe_requested, true);
    }
    if (l->sb_speaking && now >= l->sb_speaking_until_ms) {
      l->sb_speaking = false;
      l->sb_speaking_until_ms = 0;
      if (l->self_id != CCORD_ID_NONE) {
        ccord_live_event sev;
        ccord_live_event_init(&sev);
        sev.ev.type = CCORD_NET_SPEAKING;
        sev.ev.guild_id = l->voice_guild;
        sev.ev.channel_id = l->voice_channel;
        sev.ev.user_id = l->self_id;
        sev.ev.speaking = false;
        (void)live_push(l, &sev);
      }
    }
    live_voice_watch(l, now);
  }
  live_voice_link_tick(l, now);
  ccord_vstream_pump_all();
  if (l->gw && l->streaming && l->tx_stream.stream_key[0] != '\0' && l->tx_stream.vs &&
      ccord_vstream_state_of(l->tx_stream.vs) == CCORD_VSTREAM_READY && now >= l->tx_stream.next_ping_ms) {
    ccord_error perr;
    (void)ccord_gateway_send_stream_ping(l->gw, l->tx_stream.stream_key, &perr);
    l->tx_stream.next_ping_ms = now + 5000u;
  }
  if (l->gw && l->watching_stream && l->rx_stream.stream_key[0] != '\0' && l->rx_stream.vs &&
      ccord_vstream_state_of(l->rx_stream.vs) == CCORD_VSTREAM_READY && now >= l->rx_stream.next_ping_ms) {
    ccord_error perr;
    (void)ccord_gateway_send_stream_ping(l->gw, l->rx_stream.stream_key, &perr);
    l->rx_stream.next_ping_ms = now + 5000u;
  }
  if (l->tx_stream.vs && ccord_vstream_state_of(l->tx_stream.vs) == CCORD_VSTREAM_CLOSED) {
    ccord_mutex_lock(l->media_mtx);
    ccord_vstream_close(l->tx_stream.vs);
    l->tx_stream.vs = nullptr;
    live_stream_tx_clear(l);
    ccord_mutex_unlock(l->media_mtx);
  }
  if (l->rx_stream.vs && ccord_vstream_state_of(l->rx_stream.vs) == CCORD_VSTREAM_CLOSED) {
    ccord_mutex_lock(l->media_mtx);
    ccord_vstream_close(l->rx_stream.vs);
    l->rx_stream.vs = nullptr;
    live_stream_rx_clear(l);
    ccord_mutex_unlock(l->media_mtx);
  }
  if (l->gw) {
    ccord_net_event ev;
    if (ccord_gateway_poll(l->gw, &ev)) {
      live_voice_cache_from_event(l, &ev);
      live_chan_guild_from_event(l, &ev);
      live_status_on_event(l, &ev);
      bool swallowed = false;
      if (ev.type == CCORD_NET_STATE) {
        l->state = ev.state;
        l->reported = ev.state;
      } else if (ev.type == CCORD_NET_SNAPSHOT && ev.snapshot) {
        l->self_id = ev.snapshot->self_id;
        l->stream_peer_count = 0;
        l->voice_cache_count = 0;
        for (size_t i = 0; ev.snapshot->voice_states && i < ev.snapshot->voice_state_count; i++) {
          live_voice_cache_put(l, &ev.snapshot->voice_states[i]);
        }
        const char *sid = ccord_gateway_session_id(l->gw);
        if (sid) live_copy_cstr(l->gw_session_id, sizeof l->gw_session_id, sid);
        if (l->voice_guild != CCORD_ID_NONE) live_voice_main_session_replaced(l);
      } else if (ev.type == CCORD_NET_VOICE_SERVER_UPDATE || ev.type == CCORD_NET_VOICE_STATE_UPDATE) {
        swallowed = live_voice_on_event(l, &ev, ccord_gateway_voice_session_id(l->gw));
      } else if (ev.type == CCORD_NET_CALL_CREATE || ev.type == CCORD_NET_CALL_UPDATE ||
                 ev.type == CCORD_NET_CALL_DELETE) {
        live_call_on_event(l, &ev);
      } else if (ev.type == CCORD_NET_STREAM_SIGNAL) {
        live_stream_on_event(l, &ev);
      }
      if (!swallowed) {
        *out = ev;
        return true;
      }
    }
    ccord_connection_state gs = ccord_gateway_state(l->gw);
    if (gs != l->reported) {
      l->reported = gs;
      l->state = gs;
      out->type = CCORD_NET_STATE;
      out->state = gs;
      return true;
    }
  } else if (l->state != l->reported) {
    l->reported = l->state;
    out->type = CCORD_NET_STATE;
    out->state = l->state;
    return true;
  }
  if (l->count == 0) return false;
  l->current = l->pending[l->head];
  l->current_media = l->pending_media[l->head];
  l->pending_media[l->head] = nullptr;
  if (l->current.ev.type == CCORD_NET_SNAPSHOT) l->current.ev.snapshot = &l->current.snapshot;
  if (l->current.ev.type == CCORD_NET_USER_SETTINGS) l->current.ev.user_settings = &l->current.user_settings;
  memset(&l->pending[l->head], 0, sizeof l->pending[l->head]);
  l->pending[l->head].seq = -1;
  l->head = (l->head + 1u) % LIVE_QUEUE_SIZE;
  l->count--;
  l->have_current = true;
  *out = l->current.ev;
  return true;
}


void live_copy_cstr(char *dst, size_t cap, const char *src)
{
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}


static bool live_wait_info(const ccord_transport *t, ccord_wait_info *out)
{
  const live *l = (const live *)t;
  if (!l || !out) return false;
  *out = (ccord_wait_info){.timeout_ms = UINT32_MAX};
  if (l->count > 0 || (!l->gw && l->state != l->reported) ||
      (l->gw && ccord_gateway_state(l->gw) != l->reported)) {
    out->immediate = true;
    out->timeout_ms = 0;
    return true;
  }
  if (l->gw) {
    uint32_t gw_to = UINT32_MAX;
    bool gw_pending = false;
    ccord_socket_t sock = ccord_gateway_wait_info(l->gw, &gw_to, &gw_pending);
    if (sock != CCORD_INVALID_SOCKET && out->fd_count < CCORD_WAIT_MAX_FDS) {
      out->fds[out->fd_count++] = sock;
    }
    if (gw_pending) {
      out->immediate = true;
      out->timeout_ms = 0;
      return true;
    }
    if (gw_to < out->timeout_ms) out->timeout_ms = gw_to;
  }
  if (l->sb_pending_pcm != nullptr) {
    if (out->timeout_ms > 20u) out->timeout_ms = 20u;
  }
  if (l->voice != nullptr || l->vlink != LV_IDLE || l->tx_stream.vs != nullptr || l->rx_stream.vs != nullptr) {
    if (out->timeout_ms > 50u) out->timeout_ms = 50u;
  }
  if (l->restq && ccord_rest_queue_pending(l->restq) > 0) {
    if (out->timeout_ms > 50u) out->timeout_ms = 50u;
  }
  if (l->vresolve && ccord_video_resolver_busy(l->vresolve)) {
    if (out->timeout_ms > 50u) out->timeout_ms = 50u;
  }
  return true;
}

static void live_destroy(ccord_transport *t)
{
  live *l = (live *)t;
  if (!l) return;
  live_streams_stop_all(l);
  l->stream_peer_count = 0;
  live_voice_stop(l);
  if (l->gw) ccord_gateway_destroy(l->gw);
  if (l->restq) ccord_rest_queue_destroy(l->restq);
  if (l->vresolve) ccord_video_resolver_destroy(l->vresolve);
  live_clear_events(l);
  live_clear_pending_decor_profiles(l);
  ccord_free(l->effect_configs);
  ccord_free(l->frame_configs);
  live_chan_guild_clear_all(l);
  ccord_idmap_free(&l->chan_guild_idx);
  ccord_free(l->chan_guilds);
  ccord_free(l->rich_json);
  ccord_free(l->status_base);
  for (size_t i = 0; i < LIVE_STREAM_FRAME_SLOTS; i++) ccord_free(l->rx_frames[i].data);
  ccord_free(l->rx_pcm);
  if (l->stream_enc) opus_encoder_destroy(l->stream_enc);
  if (l->stream_dec) opus_decoder_destroy(l->stream_dec);
  ccord_mutex_destroy(l->media_mtx);
  ccord_cond_destroy(l->voice_cond);
  ccord_mutex_destroy(l->voice_mtx);
  ccord_live_wipe(l->token, sizeof l->token);
  ccord_free(l);
}

static const ccord_transport_vtable k_live_vt = {
  .name = "live",
  .connect = live_connect,
  .disconnect = live_disconnect,
  .request_history = live_request_history,
  .send_message = live_send_message,
  .send_message_with_nonce = live_send_message_with_nonce,
  .poll = live_poll,
  .destroy = live_destroy,
  .edit_message = live_edit_message,
  .delete_message = live_delete_message,
  .send_typing = live_send_typing,
  .toggle_reaction = live_toggle_reaction,
  .ack_message = live_ack_message,
  .select_guild = live_select_guild,
  .join_voice = live_join_voice,
  .leave_voice = live_leave_voice,
  .update_voice_state = live_update_voice_state,
  .start_stream = live_start_stream,
  .stop_stream = live_stop_stream,
  .watch_stream = live_watch_stream,
  .stop_watching = live_stop_watching,
  .set_stream_paused = live_set_stream_paused,
  .send_reply = live_send_reply,
  .toggle_pin = live_toggle_pin,
  .request_history_before = live_request_history_before,
  .request_pins = live_request_pins,
  .set_presence = live_set_presence,
  .admin_action = live_admin_action,
  .set_voice_camera = live_set_voice_camera,
  .send_message_with_file = live_send_message_with_file,
  .fetch_media = live_fetch_media,
  .fetch_media_user = live_fetch_media_user,
  .fetch_media_range = live_fetch_media_range,
  .media_progress = live_media_progress,
  .network_event = live_network_event,
  .search_messages = live_search_messages,
  .open_dm = live_open_dm,
  .request_channel = live_request_channel,
  .push_voice_pcm = live_push_voice_pcm,
  .pull_voice_pcm = live_pull_voice_pcm,
  .set_relationship = live_set_relationship,
  .request_profile = live_request_profile,
  .select_channel = live_select_channel,
  .request_members = live_request_members,
  .request_forum_posts = live_request_forum_posts,
  .create_forum_post = live_create_forum_post,
  .request_expression_data = live_request_expression_data,
  .send_sticker = live_send_sticker,
  .vote_poll = live_vote_poll,
  .send_interaction = live_send_interaction,
  .send_reply_ex = live_send_reply_ex,
  .send_message_with_files = live_send_message_with_files,
  .create_poll = live_create_poll,
  .request_report_menu = live_request_report_menu,
  .submit_report = live_submit_report,
  .request_slash_commands = live_request_slash_commands,
  .search_messages_ex = live_search_messages_ex,
  .request_pins_before = live_request_pins_before,
  .request_reaction_users = live_request_reaction_users,
  .request_discoverable_guilds = live_request_discoverable_guilds,
  .request_profile_ex = live_request_profile_ex,
  .set_presence_ex = live_set_presence_ex,
  .call_action = live_call_action,
  .play_soundboard = live_play_soundboard,
  .set_user_volume = live_set_user_volume,
  .set_voice_config = live_set_voice_config,
  .send_stream_frame = live_send_stream_frame,
  .poll_stream_frame = live_poll_stream_frame,
  .camera_senders = live_camera_senders,
  .poll_camera_frame = live_poll_camera_frame,
  .send_stream_pcm = live_send_stream_pcm,
  .poll_stream_pcm = live_poll_stream_pcm,
  .send_voice_video = live_send_voice_video,
  .consume_keyframe_request = live_consume_keyframe_request,
  .send_tts_message = live_send_tts_message,
  .forward_message = live_forward_message,
  .crosspost_message = live_crosspost_message,
  .mark_unread = live_mark_unread,
  .patch_settings_proto = live_patch_settings_proto,
  .set_status_proto = live_set_status_proto,
  .set_rich_activities = live_set_rich_activities,
  .active_uploads = live_active_uploads,
  .resolve_video = live_resolve_video,
  .wait_info = live_wait_info,
  .set_background = live_set_background,
  .set_suspended = live_set_suspended,
};

ccord_gov *ccord_live_transport_governor(ccord_transport *t)
{
  if (!t || t->vt != &k_live_vt) return nullptr;
  return ccord_rest_queue_governor(((live *)t)->restq);
}

ccord_transport *ccord_live_transport_create(const ccord_live_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  char loaded[CCORD_LIVE_TOKEN_MAX + 1];
  bool borrowed = false;
  const char *token = cfg ? cfg->token : nullptr;
  if (token && token[0] != '\0') {
    borrowed = true;
  } else {
    ccord_status st = ccord_live_load_token(cfg ? cfg->token_file : nullptr, loaded, sizeof loaded, err);
    if (st != CCORD_OK) return nullptr;
    token = loaded;
  }
  size_t tn = strlen(token);
  if (tn > CCORD_LIVE_TOKEN_MAX) {
    ccord_live_wipe(loaded, sizeof loaded);
    ccord_error_set(err, CCORD_ERR_RANGE, "live: token too long");
    return nullptr;
  }
  for (size_t i = 0; i < tn; i++) {
    unsigned char c = (unsigned char)token[i];
    if (c <= 0x20 || c >= 0x7f) {
      if (!borrowed) ccord_live_wipe(loaded, sizeof loaded);
      ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid token bytes");
      return nullptr;
    }
  }
  live *l = ccord_calloc(1, sizeof *l);
  if (!l) {
    ccord_live_wipe(loaded, sizeof loaded);
    ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    return nullptr;
  }
  l->tx_stream.sender = true;
  l->rx_stream.sender = false;
  l->media_mtx = ccord_mutex_create();
  l->voice_mtx = ccord_mutex_create();
  l->voice_cond = ccord_cond_create();
  if (!l->media_mtx || !l->voice_mtx || !l->voice_cond) {
    if (!borrowed) ccord_live_wipe(loaded, sizeof loaded);
    live_destroy((ccord_transport *)l);
    ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    return nullptr;
  }
  atomic_init(&l->ptt_active, false);
  atomic_init(&l->ptt_release_ms, 0);
  atomic_init(&l->ptt_enabled, false);
  atomic_init(&l->tx_keyframe_requested, false);
  memcpy(l->token, token, tn + 1);
  if (!borrowed) ccord_live_wipe(loaded, sizeof loaded);

  const char *base = cfg && cfg->api_base && cfg->api_base[0] ? cfg->api_base : CCORD_LIVE_API_BASE;
  size_t bn = strlen(base);
  if (bn >= sizeof l->api_base) {
    live_destroy((ccord_transport *)l);
    ccord_error_set(err, CCORD_ERR_RANGE, "live: API base too long");
    return nullptr;
  }
  memcpy(l->api_base, base, bn + 1);
  if (cfg && cfg->cache_dir && cfg->cache_dir[0] != '\0') {
    size_t cn = strlen(cfg->cache_dir);
    if (cn >= sizeof l->cache_dir) {
      live_destroy((ccord_transport *)l);
      ccord_error_set(err, CCORD_ERR_RANGE, "live: cache dir too long");
      return nullptr;
    }
    memcpy(l->cache_dir, cfg->cache_dir, cn + 1);
  }
  l->rest.base_url = l->api_base;
  l->rest.token = l->token;
  l->rest.timeout_ms = cfg ? cfg->request_timeout_ms : 0;
  l->rest.max_response_bytes = CCORD_LIVE_MAX_RESPONSE;
  l->restq = ccord_rest_queue_create(&l->rest, err);
  if (!l->restq) {
    live_destroy((ccord_transport *)l);
    return nullptr;
  }
  if (cfg) {
    l->now_ms = cfg->now_ms;
    l->clock_ud = cfg->clock_ud;
  }
  ccord_vc_init(&l->vc, live_now(l) ^ (uint64_t)(uintptr_t)l);
  l->state = CCORD_CONN_DISCONNECTED;
  l->reported = CCORD_CONN_DISCONNECTED;
  ccord_live_event_init(&l->current);
  l->base.vt = &k_live_vt;

  if (!cfg || !cfg->disable_gateway) {
    l->gw = ccord_gateway_create(cfg, l->token, err);
    if (!l->gw) {
      live_destroy((ccord_transport *)l);
      return nullptr;
    }
  }
  return &l->base;
}

