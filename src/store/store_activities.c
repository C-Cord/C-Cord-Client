// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "store_voice_internal.h"

#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>

// ---- embedded activity instances (per voice channel, keyed by channel id then instance id) ----

static int activity_key_cmp(const ccord_activity_instance *row, ccord_id channel_id, ccord_id instance_id)
{
  if (row->channel_id != channel_id) return row->channel_id < channel_id ? -1 : 1;
  if (row->instance_id != instance_id) return row->instance_id < instance_id ? -1 : 1;
  return 0;
}

static long find_activity(const struct voice_cache *c, ccord_id channel_id, ccord_id instance_id)
{
  if (!c) return -1;
  size_t lo = 0, hi = c->nactivities;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    int cmp = activity_key_cmp(&c->activities[mid], channel_id, instance_id);
    if (cmp == 0) return (long)mid;
    if (cmp < 0) lo = mid + 1u;
    else hi = mid;
  }
  return -1;
}

// The first row of `channel_id` (rows stay grouped by channel), or nactivities when the channel has none.
static size_t activity_channel_start(const struct voice_cache *c, ccord_id channel_id)
{
  if (!c) return 0;
  size_t lo = 0, hi = c->nactivities;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (c->activities[mid].channel_id < channel_id) lo = mid + 1u;
    else hi = mid;
  }
  return lo;
}

static bool same_activity(const ccord_activity_instance *a, const ccord_activity_instance *b)
{
  if (a->instance_id != b->instance_id || a->application_id != b->application_id ||
      a->guild_id != b->guild_id || a->channel_id != b->channel_id || a->location_id != b->location_id ||
      a->participant_count != b->participant_count) {
    return false;
  }
  for (size_t i = 0; i < a->participant_count; i++) {
    if (a->participants[i] != b->participants[i]) return false;
  }
  return true;
}

static bool activity_valid(const ccord_activity_instance *in)
{
  if (in->instance_id == CCORD_ID_NONE || in->application_id == CCORD_ID_NONE ||
      in->guild_id == CCORD_ID_NONE || in->channel_id == CCORD_ID_NONE ||
      in->participant_count > CCORD_ACTIVITY_MAX_PARTICIPANTS) {
    return false;
  }
  for (size_t i = 0; i < in->participant_count; i++) {
    if (in->participants[i] == CCORD_ID_NONE) return false;
  }
  return true;
}

void store_voice_activity_remove_at(struct voice_cache *c, size_t i)
{
  memmove(&c->activities[i], &c->activities[i + 1], (c->nactivities - i - 1u) * sizeof *c->activities);
  c->nactivities--;
}

// Inserts `in` into the (channel, instance)-sorted array, replacing a row with the same key.
// Returns the slot index or -1 on OOM.
static long upsert_activity(struct voice_cache *c, const ccord_activity_instance *in)
{
  long i = find_activity(c, in->channel_id, in->instance_id);
  if (i >= 0) {
    c->activities[i] = *in;
    return i;
  }
  if (c->nactivities == c->capactivities) {
    size_t ncap = c->capactivities ? c->capactivities * 2u : 4u;
    ccord_activity_instance *grown = ccord_realloc_array(c->activities, ncap, sizeof *grown);
    if (!grown) return -1;
    c->activities = grown;
    c->capactivities = ncap;
  }
  size_t at = 0;
  while (at < c->nactivities && activity_key_cmp(&c->activities[at], in->channel_id, in->instance_id) < 0) at++;
  memmove(&c->activities[at + 1], &c->activities[at], (c->nactivities - at) * sizeof *c->activities);
  c->activities[at] = *in;
  c->nactivities++;
  return (long)at;
}

ccord_status ccord_store_set_activity_instances(ccord_store *s, ccord_id channel_id,
                                                const ccord_activity_instance *instances, size_t count,
                                                ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || channel_id == CCORD_ID_NONE || (!instances && count > 0)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: activity arguments");
  }
  for (size_t i = 0; i < count; i++) {
    if (!activity_valid(&instances[i]) || instances[i].channel_id != channel_id) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "store: bad activity instance");
    }
    for (size_t j = 0; j < i; j++) {
      if (instances[j].instance_id == instances[i].instance_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "store: duplicate activity instance");
      }
    }
  }
  struct voice_cache *c = store_voice_cache_get(s, count > 0);
  if (!c) {
    if (count > 0) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    return CCORD_OK;   // no cache and nothing to set: the channel already has no instances
  }
  // Drop the channel's current rows, then insert the answer's rows in order.
  bool changed = false;
  size_t start = activity_channel_start(c, channel_id);
  size_t nold = 0;
  while (start + nold < c->nactivities && c->activities[start + nold].channel_id == channel_id) nold++;
  if (nold != count) changed = true;
  for (size_t i = 0; i < count && !changed; i++) {   // rows are stored sorted, the answer may be in any order
    long at = find_activity(c, channel_id, instances[i].instance_id);
    if (at < 0 || !same_activity(&c->activities[at], &instances[i])) changed = true;
  }
  if (!changed) return CCORD_OK;   // idempotent: the same list again moves no revision
  if (nold > 0) {
    memmove(&c->activities[start], &c->activities[start + nold],
            (c->nactivities - start - nold) * sizeof *c->activities);
    c->nactivities -= nold;
  }
  for (size_t i = 0; i < count; i++) {
    if (upsert_activity(c, &instances[i]) < 0) {
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
  }
  ccord_store_voice_mutated(s);
  return CCORD_OK;
}

ccord_status ccord_store_upsert_activity_instance(ccord_store *s, const ccord_activity_instance *instance,
                                                  ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !instance) return ccord_error_set(err, CCORD_ERR_INVALID, "store: activity arguments");
  // participant_count 0 is the end-of-instance signal: it only needs the instance id to find the row (the channel
  // may be absent from an end payload, then the row is found by instance id alone).
  bool ended = instance->participant_count == 0;
  if (instance->instance_id == CCORD_ID_NONE || (!ended && !activity_valid(instance))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: bad activity instance");
  }
  struct voice_cache *c = store_voice_cache_get(s, !ended);
  if (!c) {
    if (ended) return CCORD_OK;   // nothing cached: the instance already is gone
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  long i = -1;
  if (instance->channel_id != CCORD_ID_NONE) {
    i = find_activity(c, instance->channel_id, instance->instance_id);
  } else if (ended) {
    for (size_t k = 0; k < c->nactivities; k++) {
      if (c->activities[k].instance_id == instance->instance_id) {
        i = (long)k;
        break;
      }
    }
  }
  if (ended) {
    if (i < 0) return CCORD_OK;   // an end for an instance we never saw: nothing to drop
    store_voice_activity_remove_at(c, (size_t)i);
    ccord_store_voice_mutated(s);
    return CCORD_OK;
  }
  if (i >= 0 && same_activity(&c->activities[i], instance)) return CCORD_OK;   // idempotent
  if (upsert_activity(c, instance) < 0) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  ccord_store_voice_mutated(s);
  return CCORD_OK;
}

size_t ccord_store_activity_instance_count(const ccord_store *s, ccord_id channel_id)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  if (!c) return 0;
  size_t start = activity_channel_start(c, channel_id);
  size_t n = 0;
  while (start + n < c->nactivities && c->activities[start + n].channel_id == channel_id) n++;
  return n;
}

const ccord_activity_instance *ccord_store_activity_instance_at(const ccord_store *s, ccord_id channel_id, size_t i)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  if (!c) return nullptr;
  size_t start = activity_channel_start(c, channel_id);
  if (start + i >= c->nactivities || c->activities[start + i].channel_id != channel_id) return nullptr;
  return &c->activities[start + i];
}

const ccord_activity_instance *ccord_store_find_activity_instance(const ccord_store *s, ccord_id instance_id)
{
  const struct voice_cache *c = ccord_store_voice_slot_const(s);
  if (!c || instance_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < c->nactivities; i++) {
    if (c->activities[i].instance_id == instance_id) return &c->activities[i];
  }
  return nullptr;
}

