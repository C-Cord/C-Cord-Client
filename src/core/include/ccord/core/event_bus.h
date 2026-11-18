// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>
// Event names (strings) are API: they are shown to plugins and used in logs.
typedef enum ccord_event_type : uint8_t {
  CCORD_EVT_NONE = 0,
  CCORD_EVT_ANY,               // subscribe-only wildcard
  CCORD_EVT_APP_READY,         // "app.ready"        no fields
  CCORD_EVT_APP_QUIT,          // "app.quit"         no fields
  CCORD_EVT_CONNECTION_STATE,  // "connection.state" value = ccord_connection_state
  CCORD_EVT_GUILDS_LOADED,     // "guilds.loaded"    value = guild count
  CCORD_EVT_GUILD_SELECT,      // "guild.select"     guild_id
  CCORD_EVT_CHANNEL_SELECT,    // "channel.select"   guild_id, channel_id
  CCORD_EVT_MESSAGES_LOADED,   // "messages.loaded"  channel_id, value = count (history for a channel arrived)
  CCORD_EVT_MESSAGE_CREATE,    // "message.create"   guild_id, channel_id, message_id, user_id (= author)
  CCORD_EVT_PLUGIN_STATE,      // "plugin.state"     value = ccord_plugin_state (id is in contrib registry), user_id unused
  CCORD_EVT_MESSAGE_UPDATE,    // "message.update"   guild_id, channel_id, message_id, user_id (= author)
  CCORD_EVT_MESSAGE_DELETE,    // "message.delete"   guild_id, channel_id, message_id
  CCORD_EVT_PRESENCE_UPDATE,   // "presence.update"  user_id, value = ccord_presence
  CCORD_EVT_TYPING_START,      // "typing.start"     guild_id, channel_id, user_id, value = expiry ms
  CCORD_EVT_CHANNEL_UPSERT,    // "channel.upsert"   guild_id, channel_id
  CCORD_EVT_CHANNEL_DELETE,    // "channel.delete"   guild_id, channel_id
  CCORD_EVT_REACTION_UPDATE,   // "reaction.update"  guild_id, channel_id, message_id, user_id, value = count
  CCORD_EVT_VOICE_STATE,       // "voice.state"      guild_id, channel_id, user_id, value = flag bits
  CCORD_EVT_VOICE_SERVER,      // "voice.server"     guild_id, channel_id
  CCORD_EVT_STREAM_STATE,      // "stream.state"     guild_id, channel_id, user_id, value = active
  CCORD_EVT_MESSAGE_PIN,       // "message.pin"      guild_id, channel_id, message_id, value = pinned
  CCORD_EVT_NOTIFY_UPDATE,     // "notify.update"   guild_id, channel_id (0 = the server itself): mute / notification settings changed
  CCORD_EVT_VIEW_SELECT,       // "view.select"      guild_id, value = ccord_nav_view
  CCORD_EVT_NAV_AUX,           // "navigation.aux_open" guild_id, channel_id, message_id (= parent_id), value = ccord_nav_view
  CCORD_EVT__COUNT
} ccord_event_type;
typedef enum ccord_nav_view : uint8_t {
  CCORD_NAV_VIEW_CHANNEL = 0,
  CCORD_NAV_VIEW_FRIENDS = 1,
  CCORD_NAV_VIEW_REQUESTS = 2,
  CCORD_NAV_VIEW_DISCOVERY = 3,
  CCORD_NAV_VIEW_GUILD = 4,
  CCORD_NAV_VIEW_THREAD = 5,
} ccord_nav_view;
typedef struct ccord_event {
  ccord_event_type type;
  uint64_t seq;                // set by the bus on post, strictly increasing
  uint64_t guild_id, channel_id, message_id, user_id;   // ccord_id; 0 = unset
  int64_t value;
} ccord_event;
typedef struct ccord_bus ccord_bus;
typedef void (*ccord_event_fn)(const ccord_event *ev, void *ud);
typedef uint32_t ccord_sub_id;                          // 0 = invalid
ccord_bus *ccord_bus_create(ccord_error *err);
void ccord_bus_destroy(ccord_bus *bus);
// Handlers run in subscription order, only inside ccord_bus_drain, on the caller's thread.
// (Un)subscribing from inside a handler is allowed and takes effect for the next event.
ccord_sub_id ccord_bus_subscribe(ccord_bus *bus, ccord_event_type type, ccord_event_fn fn, void *ud);  // 0 on OOM
void ccord_bus_unsubscribe(ccord_bus *bus, ccord_sub_id id);
// Enqueue a copy. Queue cap 4096: past that returns CCORD_ERR_RANGE and counts the drop (never silent: logged + counter).
[[nodiscard]] ccord_status ccord_bus_post(ccord_bus *bus, ccord_event ev);
// Dispatch FIFO until the queue is empty or 1024 events were delivered in this call (loop guard). Returns delivered count.
size_t ccord_bus_drain(ccord_bus *bus);
size_t ccord_bus_pending(const ccord_bus *bus);
uint64_t ccord_bus_dropped(const ccord_bus *bus);
const char *ccord_event_name(ccord_event_type t);                  // "message.create"; "?" if invalid
ccord_event_type ccord_event_type_from_name(const char *name);     // CCORD_EVT_NONE if unknown
