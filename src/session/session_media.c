// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t media_url_hash(const char *url)
{
  uint64_t h = 0xcbf29ce484222325ULL;
  for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
    h ^= (uint64_t)*p;
    h *= 0x100000001b3ULL;
  }
  return h ? h : 1u;
}

static uint32_t media_hash_mix(uint64_t h)
{
  return (uint32_t)(h ^ (h >> 32));
}

static bool media_is_unread(const session_media_entry *e)
{
  return e->unread_prev != 0u;
}

static void media_unread_push(ccord_session *s, size_t idx)
{
  session_media_entry *e = &s->mc->media[idx];
  if (media_is_unread(e)) return;
  uint16_t slot = (uint16_t)(idx + 1u);
  if (s->mc->media_unread_tail == 0u) {
    e->unread_prev = SESSION_MEDIA_IDX_END;
    e->unread_next = SESSION_MEDIA_IDX_END;
    s->mc->media_unread_head = slot;
    s->mc->media_unread_tail = slot;
  } else {
    size_t tail_idx = (size_t)s->mc->media_unread_tail - 1u;
    s->mc->media[tail_idx].unread_next = slot;
    e->unread_prev = s->mc->media_unread_tail;
    e->unread_next = SESSION_MEDIA_IDX_END;
    s->mc->media_unread_tail = slot;
  }
  s->mc->media_unread_count++;
}

static void media_unread_remove(ccord_session *s, size_t idx)
{
  session_media_entry *e = &s->mc->media[idx];
  if (!media_is_unread(e)) return;
  uint16_t prev = e->unread_prev;
  uint16_t next = e->unread_next;
  if (prev == SESSION_MEDIA_IDX_END) {
    s->mc->media_unread_head = (next == SESSION_MEDIA_IDX_END) ? 0u : next;
  } else {
    s->mc->media[(size_t)prev - 1u].unread_next = next;
  }
  if (next == SESSION_MEDIA_IDX_END) {
    s->mc->media_unread_tail = (prev == SESSION_MEDIA_IDX_END) ? 0u : prev;
  } else {
    s->mc->media[(size_t)next - 1u].unread_prev = prev;
  }
  e->unread_prev = 0u;
  e->unread_next = 0u;
  s->mc->media_unread_count--;
}

static void media_unread_relocate(ccord_session *s, size_t new_idx)
{
  const session_media_entry *e = &s->mc->media[new_idx];
  if (!media_is_unread(e)) return;
  uint16_t new_slot = (uint16_t)(new_idx + 1u);
  if (e->unread_prev == SESSION_MEDIA_IDX_END) {
    s->mc->media_unread_head = new_slot;
  } else {
    s->mc->media[(size_t)e->unread_prev - 1u].unread_next = new_slot;
  }
  if (e->unread_next == SESSION_MEDIA_IDX_END) {
    s->mc->media_unread_tail = new_slot;
  } else {
    s->mc->media[(size_t)e->unread_next - 1u].unread_prev = new_slot;
  }
}

static size_t media_find_index(const ccord_session *s, const char *url, uint32_t h32)
{
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->mc->media_hash[b] != 0u) {
    size_t idx = (size_t)s->mc->media_hash[b] - 1u;
    if (s->mc->media[idx].url_hash == h32 && s->mc->media[idx].url && strcmp(s->mc->media[idx].url, url) == 0) {
      return idx;
    }
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
  return SIZE_MAX;
}

static void media_hash_insert(ccord_session *s, size_t idx, uint32_t h32)
{
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->mc->media_hash[b] != 0u) {
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
  s->mc->media_hash[b] = (uint16_t)(idx + 1u);
}

static void media_hash_remove(ccord_session *s, size_t idx, uint32_t h32)
{
  uint16_t target = (uint16_t)(idx + 1u);
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->mc->media_hash[b] != 0u) {
    if (s->mc->media_hash[b] == target) {
      size_t hole = b;
      size_t cur = (hole + 1u) & SESSION_MEDIA_HASH_MASK;
      while (s->mc->media_hash[cur] != 0u) {
        size_t cur_idx = (size_t)s->mc->media_hash[cur] - 1u;
        size_t home = (size_t)(s->mc->media[cur_idx].url_hash & SESSION_MEDIA_HASH_MASK);
        if (((hole - home) & SESSION_MEDIA_HASH_MASK) < ((cur - home) & SESSION_MEDIA_HASH_MASK)) {
          s->mc->media_hash[hole] = s->mc->media_hash[cur];
          hole = cur;
        }
        cur = (cur + 1u) & SESSION_MEDIA_HASH_MASK;
      }
      s->mc->media_hash[hole] = 0u;
      return;
    }
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
}

static void media_hash_relocate(ccord_session *s, size_t old_idx, size_t new_idx, uint32_t h32)
{
  uint16_t target = (uint16_t)(old_idx + 1u);
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->mc->media_hash[b] != 0u) {
    if (s->mc->media_hash[b] == target) {
      s->mc->media_hash[b] = (uint16_t)(new_idx + 1u);
      return;
    }
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
}

static size_t media_req_find_index(const ccord_session *s, const char *url, uint32_t h32)
{
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->mc->media_req_hash[b] != 0u) {
    size_t idx = (size_t)s->mc->media_req_hash[b] - 1u;
    if (s->mc->media_req_hashes[idx] == h32 && s->mc->media_requested[idx] && strcmp(s->mc->media_requested[idx], url) == 0) {
      return idx;
    }
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
  return SIZE_MAX;
}

static void media_req_hash_insert(ccord_session *s, size_t idx, uint32_t h32)
{
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->mc->media_req_hash[b] != 0u) {
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
  s->mc->media_req_hash[b] = (uint16_t)(idx + 1u);
}

static void media_req_hash_remove(ccord_session *s, size_t idx, uint32_t h32)
{
  uint16_t target = (uint16_t)(idx + 1u);
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->mc->media_req_hash[b] != 0u) {
    if (s->mc->media_req_hash[b] == target) {
      size_t hole = b;
      size_t cur = (hole + 1u) & SESSION_MEDIA_HASH_MASK;
      while (s->mc->media_req_hash[cur] != 0u) {
        size_t cur_idx = (size_t)s->mc->media_req_hash[cur] - 1u;
        size_t home = (size_t)(s->mc->media_req_hashes[cur_idx] & SESSION_MEDIA_HASH_MASK);
        if (((hole - home) & SESSION_MEDIA_HASH_MASK) < ((cur - home) & SESSION_MEDIA_HASH_MASK)) {
          s->mc->media_req_hash[hole] = s->mc->media_req_hash[cur];
          hole = cur;
        }
        cur = (cur + 1u) & SESSION_MEDIA_HASH_MASK;
      }
      s->mc->media_req_hash[hole] = 0u;
      return;
    }
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
}

static void media_req_hash_relocate(ccord_session *s, size_t old_idx, size_t new_idx, uint32_t h32)
{
  uint16_t target = (uint16_t)(old_idx + 1u);
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->mc->media_req_hash[b] != 0u) {
    if (s->mc->media_req_hash[b] == target) {
      s->mc->media_req_hash[b] = (uint16_t)(new_idx + 1u);
      return;
    }
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
}

static void media_req_remove_at(ccord_session *s, size_t idx)
{
  media_req_hash_remove(s, idx, s->mc->media_req_hashes[idx]);
  ccord_free(s->mc->media_requested[idx]);
  size_t last = s->mc->media_req_count - 1u;
  if (idx != last) {
    s->mc->media_requested[idx] = s->mc->media_requested[last];
    s->mc->media_req_hashes[idx] = s->mc->media_req_hashes[last];
    media_req_hash_relocate(s, last, idx, s->mc->media_req_hashes[idx]);
  }
  s->mc->media_req_count--;
  s->mc->media_requested[s->mc->media_req_count] = nullptr;
  s->mc->media_req_hashes[s->mc->media_req_count] = 0u;
}

static void media_forget_request(ccord_session *s, const char *url)
{
  if (!url || s->mc->media_req_count == 0u) return;
  uint32_t h32 = media_hash_mix(media_url_hash(url));
  size_t idx = media_req_find_index(s, url, h32);
  if (idx != SIZE_MAX) media_req_remove_at(s, idx);
}

static void media_failed_set(ccord_session *s, const char *url, bool failed)
{
  if (!failed && s->mc->media_failed_count == 0u) return;
  uint64_t h = media_url_hash(url);
  for (size_t i = 0; i < s->mc->media_failed_count; i++) {
    size_t idx = (s->mc->media_failed_head + i) % SESSION_MEDIA_FAILED_MAX;
    if (s->mc->media_failed[idx] == h) {
      if (!failed) {
        if (i == 0u) {
          s->mc->media_failed[idx] = 0u;
          s->mc->media_failed_head = (s->mc->media_failed_head + 1u) % SESSION_MEDIA_FAILED_MAX;
          s->mc->media_failed_count--;
        } else {
          for (size_t j = i; j + 1u < s->mc->media_failed_count; j++) {
            size_t dst = (s->mc->media_failed_head + j) % SESSION_MEDIA_FAILED_MAX;
            size_t src = (s->mc->media_failed_head + j + 1u) % SESSION_MEDIA_FAILED_MAX;
            s->mc->media_failed[dst] = s->mc->media_failed[src];
          }
          s->mc->media_failed_count--;
          s->mc->media_failed[(s->mc->media_failed_head + s->mc->media_failed_count) % SESSION_MEDIA_FAILED_MAX] = 0u;
        }
        if (s->mc->media_failed_count == 0u) s->mc->media_failed_head = 0u;
      }
      return;
    }
  }
  if (!failed) return;
  size_t idx = (s->mc->media_failed_head + s->mc->media_failed_count) % SESSION_MEDIA_FAILED_MAX;
  if (s->mc->media_failed_count == SESSION_MEDIA_FAILED_MAX) {
    s->mc->media_failed_head = (s->mc->media_failed_head + 1u) % SESSION_MEDIA_FAILED_MAX;
  } else {
    s->mc->media_failed_count++;
  }
  s->mc->media_failed[idx] = h;
}

// Drops one cached body using swap-with-last and updates the hash & unread indices in O(1). Its pending-request
// marker goes too, so a later fetch really asks the transport again (which normally answers from the disk cache).
static void media_evict(ccord_session *s, size_t idx)
{
  session_media_entry *e = &s->mc->media[idx];
  media_unread_remove(s, idx);
  media_hash_remove(s, idx, e->url_hash);
  if (e->url) media_forget_request(s, e->url);
  s->mc->media_bytes -= e->len <= s->mc->media_bytes ? e->len : s->mc->media_bytes;
  ccord_free(e->url);
  ccord_free(e->cache_path);
  ccord_free(e->data);
  size_t last = s->mc->media_count - 1u;
  if (idx != last) {
    s->mc->media[idx] = s->mc->media[last];
    media_hash_relocate(s, last, idx, s->mc->media[idx].url_hash);
    media_unread_relocate(s, idx);
  }
  s->mc->media_count--;
  memset(&s->mc->media[s->mc->media_count], 0, sizeof s->mc->media[0]);
}

// Least recently used entry the UI is not about to pop (`keep` is never chosen); SIZE_MAX when there is none.
static size_t media_lru(const ccord_session *s, size_t keep, bool allow_queued, bool need_bytes)
{
  size_t best = SIZE_MAX;
  for (size_t i = 0; i < s->mc->media_count; i++) {
    if (i == keep || (!allow_queued && media_is_unread(&s->mc->media[i]))) continue;
    if (need_bytes && s->mc->media[i].len == 0u) continue;
    if (best == SIZE_MAX || s->mc->media[i].access_tick < s->mc->media[best].access_tick) best = i;
  }
  return best;
}

static void media_trim_bytes(ccord_session *s, const char *keep_url, uint32_t keep_h32)
{
  while (s->mc->media_bytes > SESSION_MEDIA_BYTE_BUDGET && s->mc->media_count > 1) {
    size_t keep = media_find_index(s, keep_url, keep_h32);
    size_t victim = media_lru(s, keep, false, true);
    if (victim == SIZE_MAX) return;
    media_evict(s, victim);
  }
}

void sess_store_media_loaded(ccord_session *s, const ccord_net_event *ev)
{
  if (!ev->media_url || ev->media_url[0] == '\0') return;
  if (ev->status != CCORD_OK) {
    // The download failed: forget that it is pending so the next fetch is a real retry.
    media_forget_request(s, ev->media_url);
    media_failed_set(s, ev->media_url, true);
    return;
  }
  media_failed_set(s, ev->media_url, false);
  uint32_t h32 = media_hash_mix(media_url_hash(ev->media_url));
  size_t existing = media_find_index(s, ev->media_url, h32);
  if (existing != SIZE_MAX) {
    session_media_entry *em = &s->mc->media[existing];
    s->mc->media_bytes -= em->len <= s->mc->media_bytes ? em->len : s->mc->media_bytes;
    ccord_free(em->cache_path);
    ccord_free(em->data);
    em->cache_path = ccord_strndup(ev->media_path ? ev->media_path : "",
                                   ev->media_path ? strlen(ev->media_path) : 0);
    uint8_t *taken = (ev->media_len > 0 && ev->media_data)
                         ? ccord_transport_take_media_data(s->transport, ev->media_data)
                         : nullptr;
    if (taken) {
      em->data = taken;
    } else {
      em->data = ev->media_len ? ccord_alloc_array(ev->media_len, 1) : nullptr;
      if (em->data && ev->media_data) memcpy(em->data, ev->media_data, ev->media_len);
    }
    em->len = em->data ? ev->media_len : 0;
    s->mc->media_bytes += em->len;
    em->pub = (ccord_media_item){
      .url = em->url,
      .cache_path = em->cache_path ? em->cache_path : "",
      .data = em->data,
      .len = em->len,
    };
    em->access_tick = ++s->mc->media_tick;
    media_unread_push(s, existing);
    media_forget_request(s, ev->media_url);
    media_trim_bytes(s, ev->media_url, h32);
    return;
  }
  if (s->mc->media_count >= SESSION_MAX_MEDIA) {
    size_t evict = media_lru(s, SIZE_MAX, false, false);
    if (evict == SIZE_MAX) evict = media_lru(s, SIZE_MAX, true, false);
    media_evict(s, evict);
  }
  size_t new_idx = s->mc->media_count;
  session_media_entry *e = &s->mc->media[new_idx];
  memset(e, 0, sizeof *e);
  e->url = ccord_strndup(ev->media_url, strlen(ev->media_url));
  e->cache_path = ccord_strndup(ev->media_path ? ev->media_path : "",
                                ev->media_path ? strlen(ev->media_path) : 0);
  if (!e->url || !e->cache_path) {
    ccord_free(e->url);
    ccord_free(e->cache_path);
    memset(e, 0, sizeof *e);
    media_forget_request(s, ev->media_url);
    return;
  }
  uint8_t *taken = (ev->media_len > 0 && ev->media_data)
                       ? ccord_transport_take_media_data(s->transport, ev->media_data)
                       : nullptr;
  if (taken) {
    e->data = taken;
  } else {
    e->data = ev->media_len ? ccord_alloc_array(ev->media_len, 1) : nullptr;
    if (ev->media_len > 0 && !e->data) {
      ccord_free(e->url);
      ccord_free(e->cache_path);
      memset(e, 0, sizeof *e);
      media_forget_request(s, ev->media_url);
      return;
    }
    if (ev->media_len > 0 && ev->media_data) memcpy(e->data, ev->media_data, ev->media_len);
  }
  e->len = ev->media_len;
  e->pub = (ccord_media_item){
    .url = e->url,
    .cache_path = e->cache_path,
    .data = e->data,
    .len = e->len,
  };
  e->access_tick = ++s->mc->media_tick;
  e->url_hash = h32;
  s->mc->media_count++;
  media_hash_insert(s, new_idx, h32);
  media_unread_push(s, new_idx);
  s->mc->media_bytes += e->len;
  media_forget_request(s, ev->media_url);
  media_trim_bytes(s, ev->media_url, h32);
}


static ccord_status session_fetch_media(ccord_session *s, const char *url, bool user, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!url || url[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty media url");
  uint32_t h32 = media_hash_mix(media_url_hash(url));
  size_t midx = media_find_index(s, url, h32);
  if (midx != SIZE_MAX && s->mc->media[midx].data != nullptr && s->mc->media[midx].len > 0u) {
    s->mc->media[midx].access_tick = ++s->mc->media_tick;
    media_unread_push(s, midx);
    return CCORD_OK;
  }
  if (media_req_find_index(s, url, h32) != SIZE_MAX) {
    return CCORD_OK;
  }
  ccord_status st = user ? ccord_transport_fetch_media_user(s->transport, url, err)
                         : ccord_transport_fetch_media(s->transport, url, err);
  if (st == CCORD_ERR_PERMISSION || st == CCORD_ERR_UNSUPPORTED) media_failed_set(s, url, true);
  if (st == CCORD_OK) {
    media_failed_set(s, url, false);   // a new attempt is under way
    char *copy = ccord_strndup(url, strlen(url));
    if (copy) {
      if (s->mc->media_req_count >= SESSION_MAX_MEDIA) {
        media_req_remove_at(s, 0u);
      }
      size_t ridx = s->mc->media_req_count++;
      s->mc->media_requested[ridx] = copy;
      s->mc->media_req_hashes[ridx] = h32;
      media_req_hash_insert(s, ridx, h32);
    }
  }
  return st;
}

ccord_media_stream *ccord_session_open_media_stream_ex(ccord_session *s, const char *url, uint32_t max_blocks,
                                                         ccord_error *err)
{
  if (!s) {
    ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
    return nullptr;
  }
  return ccord_stream_hub_open_ex(s->streams, url, max_blocks, err);
}

ccord_media_stream *ccord_session_open_media_stream(ccord_session *s, const char *url, ccord_error *err)
{
  return ccord_session_open_media_stream_ex(s, url, 0u, err);
}

void ccord_session_close_media_stream(ccord_session *s, ccord_media_stream *ms)
{
  if (s) ccord_stream_hub_close(s->streams, ms);
}

ccord_status ccord_session_resolve_video(ccord_session *s, const char *url, uint64_t *out_req_id, ccord_error *err)
{
  if (out_req_id) *out_req_id = 0;
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!url || url[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty video url");
  uint64_t id = ++s->next_video_resolve_id;
  if (id == 0) id = ++s->next_video_resolve_id;
  ccord_status st = ccord_transport_resolve_video(s->transport, id, url, err);
  if (st == CCORD_OK && out_req_id) *out_req_id = id;
  return st;
}

bool ccord_session_pop_video_resolved(ccord_session *s, ccord_video_resolved *out)
{
  enum { SESSION_VIDEO_RESOLVED_MAX = 8u };
  if (!s || !out || !s->video_resolved || s->video_resolved_count == 0) return false;
  *out = s->video_resolved[s->video_resolved_head];
  s->video_resolved_head = (s->video_resolved_head + 1u) % SESSION_VIDEO_RESOLVED_MAX;
  s->video_resolved_count--;
  return true;
}

ccord_status ccord_session_fetch_media(ccord_session *s, const char *url, ccord_error *err)
{
  return session_fetch_media(s, url, false, err);
}

ccord_status ccord_session_fetch_media_user(ccord_session *s, const char *url, ccord_error *err)
{
  return session_fetch_media(s, url, true, err);
}

ccord_status ccord_session_request_media(ccord_session *s, const char *url, ccord_error *err)
{
  return ccord_session_fetch_media(s, url, err);
}

bool ccord_session_pop_media(ccord_session *s, ccord_media_item *out)
{
  if (!s || !out || s->mc->media_unread_head == 0u) return false;
  size_t idx = (size_t)s->mc->media_unread_head - 1u;
  media_unread_remove(s, idx);
  *out = s->mc->media[idx].pub;
  return true;
}

bool ccord_session_pop_media_matching(ccord_session *s, const char *url_substring,
                                      const char **url, const uint8_t **data, size_t *len)
{
  if (!s || s->mc->media_unread_count == 0u || !url_substring || url_substring[0] == '\0') return false;
  uint16_t cur = s->mc->media_unread_head;
  while (cur != 0u && cur != SESSION_MEDIA_IDX_END) {
    size_t idx = (size_t)cur - 1u;
    session_media_entry *e = &s->mc->media[idx];
    if (e->url && strstr(e->url, url_substring) != nullptr) {
      media_unread_remove(s, idx);
      if (url) *url = e->pub.url;
      if (data) *data = e->pub.data;
      if (len) *len = e->pub.len;
      return true;
    }
    cur = e->unread_next;
  }
  return false;
}

const ccord_media_item *ccord_session_get_media(const ccord_session *s, const char *url)
{
  if (!s || s->mc->media_count == 0u || !url || url[0] == '\0') return nullptr;
  uint32_t h32 = media_hash_mix(media_url_hash(url));
  size_t idx = media_find_index(s, url, h32);
  return idx != SIZE_MAX ? &s->mc->media[idx].pub : nullptr;
}

static void media_drop_ram_body(ccord_session *s, size_t idx)
{
  session_media_entry *e = &s->mc->media[idx];
  media_unread_remove(s, idx);
  s->mc->media_bytes -= e->len <= s->mc->media_bytes ? e->len : s->mc->media_bytes;
  ccord_free(e->data);
  e->data = nullptr;
  e->len = 0;
  e->pub.data = nullptr;
  e->pub.len = 0;
}

void ccord_session_release_media_bytes(ccord_session *s, const char *url)
{
  if (!s || !s->mc || !url || url[0] == '\0') return;
  uint32_t h32 = media_hash_mix(media_url_hash(url));
  size_t idx = media_find_index(s, url, h32);
  if (idx == SIZE_MAX) return;
  const session_media_entry *e = &s->mc->media[idx];
  if (!e->data || !e->cache_path || e->cache_path[0] == '\0') return;
  media_drop_ram_body(s, idx);
}

void sess_trim_media_cache_bytes(ccord_session *s)
{
  if (!s || !s->mc) return;
  for (size_t i = 0; i < s->mc->media_count; i++) {
    const session_media_entry *e = &s->mc->media[i];
    if (e->data && e->cache_path && e->cache_path[0] != '\0' && !media_is_unread(e)) {
      media_drop_ram_body(s, i);
    }
  }
}

void sess_clear_media_cache(ccord_session *s)
{
  if (!s || !s->mc) return;
  for (size_t i = 0; i < s->mc->media_count; i++) {
    ccord_free(s->mc->media[i].url);
    ccord_free(s->mc->media[i].cache_path);
    ccord_free(s->mc->media[i].data);
  }
  for (size_t i = 0; i < s->mc->media_req_count; i++) {
    ccord_free(s->mc->media_requested[i]);
  }
  memset(s->mc, 0, sizeof *s->mc);
}

bool ccord_session_media_failed(const ccord_session *s, const char *url)
{
  if (!s || s->mc->media_failed_count == 0u || !url || url[0] == '\0') return false;
  uint64_t h = media_url_hash(url);
  for (size_t i = 0; i < s->mc->media_failed_count; i++) {
    if (s->mc->media_failed[(s->mc->media_failed_head + i) % SESSION_MEDIA_FAILED_MAX] == h) return true;
  }
  return false;
}

bool ccord_session_media_progress(const ccord_session *s, const char *url, uint64_t *out_now, uint64_t *out_total)
{
  if (out_now) *out_now = 0;
  if (out_total) *out_total = 0;
  if (!s || !s->transport || !url || url[0] == '\0') return false;
  return ccord_transport_media_progress(s->transport, url, out_now, out_total);
}


