// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Private seam between store.c and the forum cache in store_forum.c.
#include <ccord/store/store.h>

// M0-3: a local echo the server has not confirmed yet (pending/failed, or any bit-63 local id) is not real history.
// It is appended so it renders after confirmed history, but it must never move a channel's newest-message pointer or
// its unread/mention bookkeeping, exactly like an ephemeral message.
static inline bool store_message_is_unconfirmed_echo(const ccord_message *m)
{
  return m && (m->send_state != CCORD_MSG_SEND_CONFIRMED || (m->id & (1ULL << 63)) != 0);
}

struct forum_cache;
void ccord_store_forum_cache_destroy(struct forum_cache *f);
struct forum_cache **ccord_store_forum_slot(ccord_store *s);
const struct forum_cache *ccord_store_forum_slot_const(const ccord_store *s);
// Bumps the revision and posts channel.upsert for the forum so the UI redraws.
void ccord_store_forum_mutated(ccord_store *s, ccord_id guild_id, ccord_id forum_id);

// Roles / member roles / channel overwrites (store_roles.c).
struct role_cache;
void ccord_store_role_cache_destroy(struct role_cache *r);
struct role_cache **ccord_store_role_slot(ccord_store *s);
const struct role_cache *ccord_store_role_slot_const(const ccord_store *s);
// Drops everything (new snapshot) / everything of one guild (guild removed).
void ccord_store_roles_reset(ccord_store *s);
void ccord_store_roles_forget_guild(ccord_store *s, ccord_id guild_id);
void ccord_store_roles_forget_channel(ccord_store *s, ccord_id channel_id);
// Bumps the revision so the UI redraws (role data changed).
void ccord_store_roles_mutated(ccord_store *s);

// Mute / notification settings (store_notify.c).
struct notify_cache;
void ccord_store_notify_cache_destroy(struct notify_cache *c);
struct notify_cache **ccord_store_notify_slot(ccord_store *s);
const struct notify_cache *ccord_store_notify_slot_const(const ccord_store *s);
void ccord_store_notify_forget_guild(ccord_store *s, ccord_id guild_id);
void ccord_store_notify_forget_channel(ccord_store *s, ccord_id channel_id);
// Bumps the revision and posts notify.update (channel_id 0 = the server's own row).
void ccord_store_notify_mutated(ccord_store *s, ccord_id guild_id, ccord_id channel_id);

// Server Settings lists: bans, invites, audit log (store_admin.c).
struct admin_cache;
void ccord_store_admin_cache_destroy(struct admin_cache *c);
struct admin_cache **ccord_store_admin_slot(ccord_store *s);
const struct admin_cache *ccord_store_admin_slot_const(const ccord_store *s);
void ccord_store_admin_reset(ccord_store *s);

// How big the server says each member list is (store_listsize.c).
struct listsize_cache;
void ccord_store_listsize_cache_destroy(struct listsize_cache *c);
struct listsize_cache **ccord_store_listsize_slot(ccord_store *s);
const struct listsize_cache *ccord_store_listsize_slot_const(const ccord_store *s);
// Bumps the revision (the lists are drawn from it).
void ccord_store_member_list_size_mutated(ccord_store *s);

// M5 DM calls, stage instances, voice-channel status, Go Live streams, per-user audio, DAVE/RTC
// (store_voice.c).
struct voice_cache;
void ccord_store_voice_cache_destroy(struct voice_cache *c);
struct voice_cache **ccord_store_voice_slot(ccord_store *s);
const struct voice_cache *ccord_store_voice_slot_const(const ccord_store *s);
// Drops the connection-scoped rows (new snapshot / one guild / one channel). Local per-user audio
// preferences survive a snapshot, like the UI's own INI values.
void ccord_store_voice_reset(ccord_store *s);
void ccord_store_voice_forget_guild(ccord_store *s, ccord_id guild_id);
void ccord_store_voice_forget_channel(ccord_store *s, ccord_id channel_id);
// Bumps the structural revision (call/stage/status/stream rows changed; no bus event type exists).
void ccord_store_voice_mutated(ccord_store *s);
// Bumps only the traffic revision (local audio preferences, DAVE/RTC debug snapshots).
void ccord_store_voice_traffic(ccord_store *s);

// M6 account settings (store_user.c).
struct user_cache;
void ccord_store_user_cache_destroy(struct user_cache *c);
struct user_cache **ccord_store_user_slot(ccord_store *s);
const struct user_cache *ccord_store_user_slot_const(const ccord_store *s);
// Drops the cached account settings (a fresh READY brings its own).
void ccord_store_user_reset(ccord_store *s);
// Bumps only the traffic revision (a setting changed; no list row is added).
void ccord_store_user_traffic(ccord_store *s);
