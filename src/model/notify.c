// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/model/notify.h>

#include <stdio.h>

bool ccord_notify_scope_muted(const ccord_notify_scope *s, int64_t now_unix_ms)
{
  if (!s || !s->muted) return false;
  if (s->mute_until_ms <= 0) return true;                 // until turned back on
  if (now_unix_ms <= 0) return true;                      // clock unknown: believe the mute
  return s->mute_until_ms > now_unix_ms;
}

ccord_notify_effective ccord_notify_resolve(const ccord_notify_scope *guild, const ccord_notify_scope *category,
                                            const ccord_notify_scope *channel, int64_t now_unix_ms)
{
  ccord_notify_effective e = {.muted = false, .level = CCORD_NOTIFY_ALL};
  e.muted = ccord_notify_scope_muted(guild, now_unix_ms) || ccord_notify_scope_muted(category, now_unix_ms) ||
            ccord_notify_scope_muted(channel, now_unix_ms);
  const ccord_notify_scope *order[3] = {channel, category, guild};
  for (size_t i = 0; i < 3; i++) {
    if (order[i] && order[i]->level != CCORD_NOTIFY_INHERIT && order[i]->level <= CCORD_NOTIFY_NOTHING) {
      e.level = order[i]->level;
      e.level_set = true;
      break;
    }
  }
  if (guild) {
    e.suppress_everyone = guild->suppress_everyone;
    e.suppress_roles = guild->suppress_roles;
  }
  return e;
}

bool ccord_notify_should_notify(const ccord_notify_effective *eff, bool direct, bool everyone, bool role)
{
  if (!eff || eff->muted || eff->level == CCORD_NOTIFY_NOTHING) return false;
  if (everyone && eff->suppress_everyone) everyone = false;
  if (role && eff->suppress_roles) role = false;
  if (eff->level == CCORD_NOTIFY_ALL) return true;
  return direct || everyone || role;
}

const char *ccord_notify_level_label(ccord_notify_level level)
{
  switch (level) {
    case CCORD_NOTIFY_ALL: return "All Messages";
    case CCORD_NOTIFY_MENTIONS: return "Only @mentions";
    case CCORD_NOTIFY_NOTHING: return "Nothing";
    case CCORD_NOTIFY_INHERIT: return "Server Default";
  }
  return "All Messages";
}

static const ccord_mute_option k_mute_options[] = {
  {"For 15 Minutes", 15LL * 60 * 1000},
  {"For 1 Hour", 60LL * 60 * 1000},
  {"For 3 Hours", 3LL * 60 * 60 * 1000},
  {"For 8 Hours", 8LL * 60 * 60 * 1000},
  {"For 24 Hours", 24LL * 60 * 60 * 1000},
  {"Until I turn it back on", 0},
};

const ccord_mute_option *ccord_notify_mute_options(size_t *count)
{
  if (count) *count = sizeof k_mute_options / sizeof k_mute_options[0];
  return k_mute_options;
}

bool ccord_notify_mute_remaining_text(const ccord_notify_scope *s, int64_t now_unix_ms, char *out, size_t cap)
{
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!ccord_notify_scope_muted(s, now_unix_ms)) return false;
  if (s->mute_until_ms <= 0 || now_unix_ms <= 0) {
    (void)snprintf(out, cap, "until you turn it back on");
    return true;
  }
  int64_t left = s->mute_until_ms - now_unix_ms;
  int64_t minutes = (left + 59999) / 60000;   // round up: "1 minute left" until it really ends
  if (minutes < 60) {
    (void)snprintf(out, cap, "%lld minute%s left", (long long)minutes, minutes == 1 ? "" : "s");
  } else if (minutes < 24 * 60) {
    int64_t hours = (minutes + 59) / 60;
    (void)snprintf(out, cap, "%lld hour%s left", (long long)hours, hours == 1 ? "" : "s");
  } else {
    int64_t days = (minutes + 24 * 60 - 1) / (24 * 60);
    (void)snprintf(out, cap, "%lld day%s left", (long long)days, days == 1 ? "" : "s");
  }
  return true;
}
