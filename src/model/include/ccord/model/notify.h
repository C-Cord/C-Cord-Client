// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Notification settings: what the user told Discord about a server, a category or a channel (Discord calls the rows
// "user_guild_settings"). Pure logic, no I/O: which of the three rows applies to a channel, whether a mute is still
// running, and the labels the menus show.
//
// The rules are Discord's:
//  - A server, a category and a channel can each be muted, for a while or until the user turns it back on. A channel is
//    muted when any of the three is (a muted server mutes everything in it).
//  - The notification level is the nearest row that sets one: the channel, else its category, else the server, else
//    "All Messages". `CCORD_NOTIFY_INHERIT` is a channel or category that sets none.
//  - "Suppress @everyone / @here" and "Suppress all role @mentions" belong to the server row.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/model/model.h>

typedef enum ccord_notify_level : uint8_t {
  CCORD_NOTIFY_ALL = 0,        // every message
  CCORD_NOTIFY_MENTIONS = 1,   // only @mentions
  CCORD_NOTIFY_NOTHING = 2,
  CCORD_NOTIFY_INHERIT = 3     // channel / category rows only: use the one above
} ccord_notify_level;

// Rows held across all servers (a server row plus its channel overrides); more are dropped, never trusted.
#define CCORD_NOTIFY_MAX_ROWS 20000u

// One row. channel_id == CCORD_ID_NONE is the server itself (guild_id == CCORD_DM_GUILD_ID: direct messages);
// otherwise it is an override for that channel or category.
typedef struct ccord_notify_scope {
  ccord_id guild_id;
  ccord_id channel_id;
  int64_t mute_until_ms;           // unix ms; with `muted`, 0 = until the user turns it back on
  ccord_notify_level level;
  bool muted;
  bool suppress_everyone, suppress_roles;
} ccord_notify_scope;

// True while the row mutes: muted, and either with no end or with an end that has not come.
// `now_unix_ms == 0` (clock unknown) counts a timed mute as still running.
[[nodiscard]] bool ccord_notify_scope_muted(const ccord_notify_scope *s, int64_t now_unix_ms);

typedef struct ccord_notify_effective {
  bool muted;
  ccord_notify_level level;        // never INHERIT
  bool level_set;                  // some row chose it; false = the user never did (`level` is then ALL, only as a fallback)
  bool suppress_everyone, suppress_roles;
} ccord_notify_effective;
// What applies to a channel given its server row, its category's row and its own row (each may be nullptr).
[[nodiscard]] ccord_notify_effective ccord_notify_resolve(const ccord_notify_scope *guild,
                                                          const ccord_notify_scope *category,
                                                          const ccord_notify_scope *channel, int64_t now_unix_ms);

// Whether a new message should notify (a sound, a desktop notification) under `eff`: nothing when muted or
// "Nothing"; every message under "All Messages"; mentions only under "Only @mentions". `everyone` is an @everyone /
// @here mention and `role` an @role mention of one of the user's roles; `direct` is an @user mention or a reply to
// the user. Suppress flags drop the broad mentions.
[[nodiscard]] bool ccord_notify_should_notify(const ccord_notify_effective *eff, bool direct, bool everyone, bool role);

// "All Messages", "Only @mentions", "Nothing", "Server Default" (INHERIT).
[[nodiscard]] const char *ccord_notify_level_label(ccord_notify_level level);

// The durations the Mute menus offer, in Discord's order; the last one has `duration_ms == 0` ("until I turn it back
// on").
typedef struct ccord_mute_option {
  const char *label;
  int64_t duration_ms;
} ccord_mute_option;
[[nodiscard]] const ccord_mute_option *ccord_notify_mute_options(size_t *count);
// "Muted until 3:45 PM"-style text needs a clock and a time zone, which this module has neither of: it gives the
// remaining time instead ("1 hour left", "12 minutes left", "until you turn it back on").
// `out` is always NUL-terminated; returns false when the scope is not muted.
bool ccord_notify_mute_remaining_text(const ccord_notify_scope *s, int64_t now_unix_ms, char *out, size_t cap);
