// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "store_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>

void store_events_clear(ccord_store *s)
{
  if (!s) return;
  for (size_t i = 0; i < s->nevents; i++) ccord_free(s->events[i].events);
  ccord_free(s->events);
  s->events = nullptr;
  s->nevents = 0;
  s->capevents = 0;
}

void store_event_bucket_drop(ccord_store *s, ccord_id guild_id)
{
  for (size_t i = 0; s && i < s->nevents; i++) {
    if (s->events[i].guild_id != guild_id) continue;
    ccord_free(s->events[i].events);
    memmove(&s->events[i], &s->events[i + 1], (s->nevents - i - 1) * sizeof *s->events);
    s->nevents--;
    return;
  }
}

pin_bucket *store_find_pin_bucket(const ccord_store *s, ccord_id channel_id)
{
  for (size_t i = 0; s && i < s->npins; i++) {
    if (s->pins[i].channel_id == channel_id) return &s->pins[i];
  }
  return nullptr;
}

pin_bucket *store_pin_bucket_for(ccord_store *s, ccord_id channel_id, ccord_error *err)
{
  pin_bucket *pb = store_find_pin_bucket(s, channel_id);
  if (pb) return pb;
  if (s->npins == s->cappins) {
    size_t ncap = s->cappins ? s->cappins * 2u : 8u;
    pin_bucket *grown = ccord_realloc_array(s->pins, ncap, sizeof *grown);
    if (!grown) {
      ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      return nullptr;
    }
    s->pins = grown;
    s->cappins = ncap;
  }
  pb = &s->pins[s->npins++];
  *pb = (pin_bucket){.channel_id = channel_id};
  return pb;
}


// ---- guild scheduled events ----

static event_bucket *event_bucket_find(const ccord_store *s, ccord_id guild_id)
{
  for (size_t i = 0; s && i < s->nevents; i++) {
    if (s->events[i].guild_id == guild_id) return &s->events[i];
  }
  return nullptr;
}

static event_bucket *event_bucket_for(ccord_store *s, ccord_id guild_id, ccord_error *err)
{
  event_bucket *b = event_bucket_find(s, guild_id);
  if (b) return b;
  if (s->nevents == s->capevents) {
    size_t ncap = s->capevents ? s->capevents * 2u : 8u;
    event_bucket *grown = ccord_realloc_array(s->events, ncap, sizeof *grown);
    if (!grown) {
      ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      return nullptr;
    }
    s->events = grown;
    s->capevents = ncap;
  }
  b = &s->events[s->nevents++];
  *b = (event_bucket){.guild_id = guild_id};
  return b;
}

static long event_find(const event_bucket *b, ccord_id event_id)
{
  for (size_t i = 0; b && i < b->n; i++) {
    if (b->events[i].id == event_id) return (long)i;
  }
  return -1;
}

static ccord_status event_validate(const ccord_scheduled_event *e, ccord_id guild_id, ccord_error *err)
{
  if (!e || e->id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: event id");
  if (e->guild_id != guild_id) return ccord_error_set(err, CCORD_ERR_INVALID, "store: event in wrong guild");
  if (e->name[0] == '\0' || !memchr(e->name, '\0', sizeof e->name) ||
      !memchr(e->description, '\0', sizeof e->description) || !memchr(e->location, '\0', sizeof e->location)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: event text");
  }
  if (e->status > CCORD_SCHEDULED_EVENT_CANCELED || e->entity_type > CCORD_SCHEDULED_EVENT_EXTERNAL) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: event status or entity type");
  }
  if (e->scheduled_start_ms < 0 || e->scheduled_end_ms < 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: event timestamp");
  }
  return CCORD_OK;
}

// Gateway CREATE/UPDATE and LIST answers never carry the signed-in user's RSVP, and user_count 0 means "unknown":
// an event we have already seen keeps what we know about it.
static void event_merge_known(ccord_scheduled_event *dst, const ccord_scheduled_event *prev)
{
  dst->me_rsvped = prev->me_rsvped;
  if (dst->user_count == 0) dst->user_count = prev->user_count;
}

ccord_status ccord_store_set_scheduled_events(ccord_store *s, ccord_id guild_id,
                                              const ccord_scheduled_event *events, size_t n, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: guild id");
  if (n > 0 && !events) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null events");
  if (n > CCORD_STORE_MAX_EVENTS_PER_GUILD) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many events");
  for (size_t i = 0; i < n; i++) {
    ccord_status vs = event_validate(&events[i], guild_id, err);
    if (vs != CCORD_OK) return vs;
  }
  if (n == 0) {
    if (event_bucket_find(s, guild_id)) {
      store_event_bucket_drop(s, guild_id);
      s->revision++;
    }
    return CCORD_OK;
  }
  ccord_scheduled_event *copy = ccord_alloc_array(n, sizeof *copy);
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  const event_bucket *prev = event_bucket_find(s, guild_id);
  for (size_t i = 0; i < n; i++) {
    copy[i] = events[i];
    long pi = event_find(prev, events[i].id);
    if (pi >= 0) event_merge_known(&copy[i], &prev->events[pi]);
  }
  event_bucket *b = event_bucket_for(s, guild_id, err);
  if (!b) {
    ccord_free(copy);
    return CCORD_ERR_NOMEM;
  }
  ccord_free(b->events);
  b->events = copy;
  b->n = n;
  b->cap = n;
  s->revision++;
  return CCORD_OK;
}

ccord_status ccord_store_upsert_scheduled_event(ccord_store *s, const ccord_scheduled_event *event, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (!event || event->guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: guild id");
  ccord_status vs = event_validate(event, event->guild_id, err);
  if (vs != CCORD_OK) return vs;
  event_bucket *b = event_bucket_for(s, event->guild_id, err);
  if (!b) return CCORD_ERR_NOMEM;
  long at = event_find(b, event->id);
  if (at >= 0) {
    ccord_scheduled_event next = *event;
    event_merge_known(&next, &b->events[at]);
    b->events[at] = next;
    s->revision++;
    return CCORD_OK;
  }
  if (b->n >= CCORD_STORE_MAX_EVENTS_PER_GUILD) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many events");
  }
  if (b->n == b->cap) {
    size_t ncap = b->cap ? b->cap * 2u : 8u;
    if (ncap > CCORD_STORE_MAX_EVENTS_PER_GUILD) ncap = CCORD_STORE_MAX_EVENTS_PER_GUILD;
    ccord_scheduled_event *grown = ccord_realloc_array(b->events, ncap, sizeof *grown);
    if (!grown) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    b->events = grown;
    b->cap = ncap;
  }
  b->events[b->n++] = *event;
  s->revision++;
  return CCORD_OK;
}

bool ccord_store_remove_scheduled_event(ccord_store *s, ccord_id guild_id, ccord_id event_id)
{
  event_bucket *b = event_bucket_find(s, guild_id);
  long at = event_find(b, event_id);
  if (!b || at < 0) return false;
  memmove(&b->events[at], &b->events[at + 1], (b->n - (size_t)at - 1u) * sizeof *b->events);
  b->n--;
  if (b->n == 0) store_event_bucket_drop(s, guild_id);
  s->revision++;
  return true;
}

ccord_status ccord_store_set_scheduled_event_rsvp(ccord_store *s, ccord_id guild_id, ccord_id event_id,
                                                  bool rsvped, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  event_bucket *b = event_bucket_find(s, guild_id);
  long at = event_find(b, event_id);
  if (!b || at < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown event");
  ccord_scheduled_event *e = &b->events[at];
  if (e->me_rsvped == rsvped) return CCORD_OK;   // repeated USER_CHANGE / REST ack never double-counts
  e->me_rsvped = rsvped;
  if (e->user_count > 0) e->user_count = rsvped ? e->user_count + 1u : e->user_count - 1u;
  s->revision++;
  return CCORD_OK;
}

size_t ccord_store_scheduled_event_count(const ccord_store *s, ccord_id guild_id)
{
  const event_bucket *b = event_bucket_find(s, guild_id);
  return b ? b->n : 0;
}

const ccord_scheduled_event *ccord_store_scheduled_event_at(const ccord_store *s, ccord_id guild_id, size_t i)
{
  const event_bucket *b = event_bucket_find(s, guild_id);
  return (b && i < b->n) ? &b->events[i] : nullptr;
}

const ccord_scheduled_event *ccord_store_find_scheduled_event(const ccord_store *s, ccord_id event_id)
{
  for (size_t i = 0; s && i < s->nevents; i++) {
    long at = event_find(&s->events[i], event_id);
    if (at >= 0) return &s->events[i].events[at];
  }
  return nullptr;
}


