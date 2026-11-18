// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>
#include <ccord/store/store.h>

#include "store_internal.h"

// M6 account settings. Discord owns this state: it is written only
// from a REST answer or a USER_SETTINGS_UPDATE dispatch, never from a local optimistic click. The
// whole model is fixed-size inline data, so the cache holds one value and the store copies it by
// value (no borrowed pointers to invalidate). A fresh snapshot drops it; a new READY carries the
// account's own settings again.
struct user_cache {
  ccord_user_settings settings;
  bool has_settings;
};

// Discord defaults used only when a PATCH arrives before the first full settings object. The UI
// keeps its own nullptr-safe defaults for the same reason; these mirror the values the client
// reproduces today (GIFs/animated emoji on, stickers "always", DMs from server members allowed).
static const ccord_user_settings k_settings_defaults = {
  .default_guilds_restricted = false,
  .explicit_content_filter = 0,
  .friend_source_all = true,
  .friend_source_mutual_friends = true,
  .friend_source_mutual_guilds = true,
  .allow_access_to_age_restricted_servers = false,
  .developer_mode = false,
  .gif_auto_play = true,
  .animate_emoji = true,
  .enable_tts_command = false,
  .stream_notifications_enabled = false,
  .message_display_compact = false,
  .animate_stickers = 0,
  .locale = "en-US",
};

void ccord_store_user_cache_destroy(struct user_cache *c)
{
  ccord_free(c);
}

static struct user_cache *cache_get(ccord_store *s, bool create)
{
  struct user_cache **slot = ccord_store_user_slot(s);
  if (!slot) return nullptr;
  if (!*slot && create) *slot = ccord_calloc(1, sizeof **slot);
  return *slot;
}

static bool inline_text_ok(const char *text, size_t cap)
{
  return memchr(text, '\0', cap) != nullptr;   // the inline arrays must stay NUL-terminated
}

static ccord_status validate_settings(const ccord_user_settings *settings, ccord_error *err)
{
  if (settings->explicit_content_filter > 2u) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: explicit content filter");
  }
  if (settings->animate_stickers > 2u) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: animate stickers");
  }
  if (!inline_text_ok(settings->locale, sizeof settings->locale)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: locale text");
  }
  size_t n = strlen(settings->locale);   // inline_text_ok proved the terminator above
  if (n > 0 && !ccord_utf8_valid_text(settings->locale, n)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: locale text");
  }
  return CCORD_OK;
}

static bool same_settings(const ccord_user_settings *a, const ccord_user_settings *b)
{
  return a->default_guilds_restricted == b->default_guilds_restricted &&
         a->explicit_content_filter == b->explicit_content_filter &&
         a->friend_source_all == b->friend_source_all &&
         a->friend_source_mutual_friends == b->friend_source_mutual_friends &&
         a->friend_source_mutual_guilds == b->friend_source_mutual_guilds &&
         a->allow_access_to_age_restricted_servers == b->allow_access_to_age_restricted_servers &&
         a->developer_mode == b->developer_mode && a->gif_auto_play == b->gif_auto_play &&
         a->animate_emoji == b->animate_emoji && a->enable_tts_command == b->enable_tts_command &&
         a->stream_notifications_enabled == b->stream_notifications_enabled &&
         a->message_display_compact == b->message_display_compact &&
         a->animate_stickers == b->animate_stickers && strcmp(a->locale, b->locale) == 0;
}

ccord_status ccord_store_set_user_settings(ccord_store *s, const ccord_user_settings *settings, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !settings) return ccord_error_set(err, CCORD_ERR_INVALID, "store: user settings arguments");
  ccord_status st = validate_settings(settings, err);
  if (st != CCORD_OK) return st;

  struct user_cache *c = cache_get(s, false);
  if (c && c->has_settings && same_settings(&c->settings, settings)) return CCORD_OK;   // idempotent
  c = cache_get(s, true);
  if (!c) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  c->settings = *settings;
  c->has_settings = true;
  ccord_store_user_traffic(s);
  return CCORD_OK;
}

const ccord_user_settings *ccord_store_user_settings(const ccord_store *s)
{
  const struct user_cache *c = ccord_store_user_slot_const(s);
  return c && c->has_settings ? &c->settings : nullptr;
}

static bool flag_ok(int8_t value)
{
  return value >= -1 && value <= 1;
}

static bool level_ok(int16_t value, int16_t max)
{
  return value >= -1 && value <= max;
}

// Merges the PATCH payload's sentinel fields onto the current settings. The session never writes
// the local patch here before Discord answers; the mock/live echo applies the answer as a full
// object instead. This helper exists for callers that already hold an authoritative base (and for
// the "no settings yet" default world) and must stay equivalent to the wire builder's sentinels.
ccord_status ccord_store_apply_user_settings_patch(ccord_store *s, const ccord_user_settings_patch *patch,
                                                   ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !patch) return ccord_error_set(err, CCORD_ERR_INVALID, "store: user settings patch arguments");
  if (!flag_ok(patch->default_guilds_restricted) || !flag_ok(patch->friend_source_all) ||
      !flag_ok(patch->friend_source_mutual_friends) || !flag_ok(patch->friend_source_mutual_guilds) ||
      !flag_ok(patch->allow_access_to_age_restricted_servers) || !flag_ok(patch->developer_mode) ||
      !flag_ok(patch->gif_auto_play) || !flag_ok(patch->animate_emoji) ||
      !flag_ok(patch->enable_tts_command) || !flag_ok(patch->stream_notifications_enabled)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: user settings patch flags");
  }
  if (!level_ok(patch->explicit_content_filter, 2) || !level_ok(patch->animate_stickers, 2)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: user settings patch values");
  }
  if (!inline_text_ok(patch->locale, sizeof patch->locale)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: user settings patch locale");
  }
  size_t locale_len = strlen(patch->locale);   // inline_text_ok proved the terminator above
  if (locale_len > 0 && !ccord_utf8_valid_text(patch->locale, locale_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: user settings patch locale");
  }

  struct user_cache *c = cache_get(s, false);
  ccord_user_settings next = c && c->has_settings ? c->settings : k_settings_defaults;
  if (patch->default_guilds_restricted >= 0) next.default_guilds_restricted = patch->default_guilds_restricted != 0;
  if (patch->friend_source_all >= 0) next.friend_source_all = patch->friend_source_all != 0;
  if (patch->friend_source_mutual_friends >= 0) {
    next.friend_source_mutual_friends = patch->friend_source_mutual_friends != 0;
  }
  if (patch->friend_source_mutual_guilds >= 0) {
    next.friend_source_mutual_guilds = patch->friend_source_mutual_guilds != 0;
  }
  if (patch->allow_access_to_age_restricted_servers >= 0) {
    next.allow_access_to_age_restricted_servers = patch->allow_access_to_age_restricted_servers != 0;
  }
  if (patch->developer_mode >= 0) next.developer_mode = patch->developer_mode != 0;
  if (patch->gif_auto_play >= 0) next.gif_auto_play = patch->gif_auto_play != 0;
  if (patch->animate_emoji >= 0) next.animate_emoji = patch->animate_emoji != 0;
  if (patch->enable_tts_command >= 0) next.enable_tts_command = patch->enable_tts_command != 0;
  if (patch->stream_notifications_enabled >= 0) {
    next.stream_notifications_enabled = patch->stream_notifications_enabled != 0;
  }
  if (patch->explicit_content_filter >= 0) {
    next.explicit_content_filter = (uint8_t)patch->explicit_content_filter;
  }
  if (patch->animate_stickers >= 0) next.animate_stickers = (uint8_t)patch->animate_stickers;
  if (locale_len > 0) memcpy(next.locale, patch->locale, sizeof next.locale);
  return ccord_store_set_user_settings(s, &next, err);
}

void ccord_store_user_reset(ccord_store *s)
{
  struct user_cache *c = cache_get(s, false);
  if (!c) return;
  bool had = c->has_settings;
  c->settings = (ccord_user_settings){0};
  c->has_settings = false;
  if (had) ccord_store_user_traffic(s);
}
