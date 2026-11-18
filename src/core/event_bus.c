// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/event_bus.h>

#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>

#define BUS_QUEUE_CAP 4096u
#define BUS_DRAIN_LIMIT 1024u

typedef struct bus_sub {
  ccord_event_type type;
  ccord_event_fn fn;
  void *ud;
  ccord_sub_id id;
  bool live;
} bus_sub;

struct ccord_bus {
  bus_sub *subs;
  size_t nsubs, capsubs;
  ccord_sub_id next_id;
  bool need_compact;
  int dispatching;
  ccord_event *queue; // ring buffer of BUS_QUEUE_CAP entries
  size_t head, count;
  uint64_t seq, dropped;
};

static const char *const event_names[CCORD_EVT__COUNT] = {
  [CCORD_EVT_NONE] = "?",
  [CCORD_EVT_ANY] = "*",
  [CCORD_EVT_APP_READY] = "app.ready",
  [CCORD_EVT_APP_QUIT] = "app.quit",
  [CCORD_EVT_CONNECTION_STATE] = "connection.state",
  [CCORD_EVT_GUILDS_LOADED] = "guilds.loaded",
  [CCORD_EVT_GUILD_SELECT] = "guild.select",
  [CCORD_EVT_CHANNEL_SELECT] = "channel.select",
  [CCORD_EVT_MESSAGES_LOADED] = "messages.loaded",
  [CCORD_EVT_MESSAGE_CREATE] = "message.create",
  [CCORD_EVT_PLUGIN_STATE] = "plugin.state",
  [CCORD_EVT_MESSAGE_UPDATE] = "message.update",
  [CCORD_EVT_MESSAGE_DELETE] = "message.delete",
  [CCORD_EVT_PRESENCE_UPDATE] = "presence.update",
  [CCORD_EVT_TYPING_START] = "typing.start",
  [CCORD_EVT_CHANNEL_UPSERT] = "channel.upsert",
  [CCORD_EVT_CHANNEL_DELETE] = "channel.delete",
  [CCORD_EVT_REACTION_UPDATE] = "reaction.update",
  [CCORD_EVT_VOICE_STATE] = "voice.state",
  [CCORD_EVT_VOICE_SERVER] = "voice.server",
  [CCORD_EVT_STREAM_STATE] = "stream.state",
  [CCORD_EVT_MESSAGE_PIN] = "message.pin",
  [CCORD_EVT_NOTIFY_UPDATE] = "notify.update",
  [CCORD_EVT_VIEW_SELECT] = "view.select",
  [CCORD_EVT_NAV_AUX] = "navigation.aux_open",
};

ccord_bus *ccord_bus_create(ccord_error *err)
{
  ccord_error_clear(err);
  ccord_bus *bus = ccord_calloc(1, sizeof *bus);
  if (!bus) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "event bus: out of memory");
    return nullptr;
  }
  bus->queue = ccord_alloc_array(BUS_QUEUE_CAP, sizeof *bus->queue);
  if (!bus->queue) {
    ccord_free(bus);
    ccord_error_set(err, CCORD_ERR_NOMEM, "event bus: out of memory");
    return nullptr;
  }
  bus->next_id = 1;
  return bus;
}

void ccord_bus_destroy(ccord_bus *bus)
{
  if (!bus) return;
  ccord_free(bus->subs);
  ccord_free(bus->queue);
  ccord_free(bus);
}

ccord_sub_id ccord_bus_subscribe(ccord_bus *bus, ccord_event_type type, ccord_event_fn fn, void *ud)
{
  if (!bus || !fn || type <= CCORD_EVT_NONE || type >= CCORD_EVT__COUNT) return 0;
  if (bus->nsubs == bus->capsubs) {
    size_t ncap = bus->capsubs ? bus->capsubs * 2 : 8;
    bus_sub *grown = ccord_realloc_array(bus->subs, ncap, sizeof *grown);
    if (!grown) return 0;
    bus->subs = grown;
    bus->capsubs = ncap;
  }
  ccord_sub_id id = bus->next_id++;
  if (id == 0) id = bus->next_id++; // 0 is reserved for "invalid"
  bus->subs[bus->nsubs++] = (bus_sub){.type = type, .fn = fn, .ud = ud, .id = id, .live = true};
  return id;
}

static void compact(ccord_bus *bus)
{
  size_t w = 0;
  for (size_t r = 0; r < bus->nsubs; r++) {
    if (bus->subs[r].live) {
      bus->subs[w++] = bus->subs[r];
    }
  }
  bus->nsubs = w;
  bus->need_compact = false;
}

void ccord_bus_unsubscribe(ccord_bus *bus, ccord_sub_id id)
{
  if (!bus || id == 0) return;
  for (size_t i = 0; i < bus->nsubs; i++) {
    if (bus->subs[i].id == id && bus->subs[i].live) {
      bus->subs[i].live = false;
      bus->need_compact = true;
      break;
    }
  }
  if (bus->dispatching == 0 && bus->need_compact) {
    compact(bus);
  }
}

ccord_status ccord_bus_post(ccord_bus *bus, ccord_event ev)
{
  if (!bus) return CCORD_ERR_INVALID;
  if (ev.type <= CCORD_EVT_NONE || ev.type >= CCORD_EVT__COUNT || ev.type == CCORD_EVT_ANY) {
    return CCORD_ERR_INVALID;
  }
  if (bus->count >= BUS_QUEUE_CAP) {
    bus->dropped++;
    CCORD_LOG_W("event bus full, dropped %s (total dropped %llu)", event_names[ev.type],
                (unsigned long long)bus->dropped);
    return CCORD_ERR_RANGE;
  }
  ev.seq = ++bus->seq;
  bus->queue[(bus->head + bus->count) % BUS_QUEUE_CAP] = ev;
  bus->count++;
  return CCORD_OK;
}

size_t ccord_bus_drain(ccord_bus *bus)
{
  if (!bus || bus->dispatching) return 0; // re-entrant drain is ignored
  size_t delivered = 0;
  bus->dispatching++;
  while (bus->count > 0 && delivered < BUS_DRAIN_LIMIT) {
    ccord_event ev = bus->queue[bus->head];
    bus->head = (bus->head + 1) % BUS_QUEUE_CAP;
    bus->count--;
    delivered++;

    // Subscriptions added by a handler start with the next event.
    size_t n = bus->nsubs;
    for (size_t i = 0; i < n; i++) {
      bus_sub s = bus->subs[i];
      if (!s.live) continue;
      if (s.type != ev.type && s.type != CCORD_EVT_ANY) continue;
      s.fn(&ev, s.ud);
    }
  }
  bus->dispatching--;
  if (bus->dispatching == 0 && bus->need_compact) {
    compact(bus);
  }
  return delivered;
}

size_t ccord_bus_pending(const ccord_bus *bus)
{
  return bus ? bus->count : 0;
}

uint64_t ccord_bus_dropped(const ccord_bus *bus)
{
  return bus ? bus->dropped : 0;
}

const char *ccord_event_name(ccord_event_type t)
{
  if (t <= CCORD_EVT_NONE || t >= CCORD_EVT__COUNT) return "?";
  return event_names[t];
}

ccord_event_type ccord_event_type_from_name(const char *name)
{
  if (!name) return CCORD_EVT_NONE;
  for (unsigned t = CCORD_EVT_APP_READY; t < CCORD_EVT__COUNT; t++) {
    if (strcmp(event_names[t], name) == 0) return (ccord_event_type)t;
  }
  return CCORD_EVT_NONE;
}
