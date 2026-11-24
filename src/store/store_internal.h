// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Unified internal state & cross-TU helpers for ccord_store.
#include <stdatomic.h>
#include <string.h>
#include <ccord/core/idmap.h>
#include <ccord/core/os.h>
#include <ccord/model/rich.h>
#include <ccord/store/store.h>
#include <ccord/store/roles.h>
#include <ccord/store/admin.h>
#include <ccord/store/expressions.h>
#include <ccord/store/listsize.h>
#include <ccord/store/notify.h>
#include "store_index.h"

// M0-3: a local echo the server has not confirmed yet (pending/failed, or any bit-63 local id) is not real history.
static inline bool store_message_is_unconfirmed_echo(const ccord_message *m)
{
  return m && (m->send_state != CCORD_MSG_SEND_CONFIRMED || (m->id & (1ULL << 63)) != 0);
}

// Expression cache (store_expr_guild.c / store_expr_picker.c)
typedef struct ccord_expr_data ccord_expr_data;
void ccord_expr_data_free(ccord_expr_data *d);
ccord_expr_data **ccord_store_expr_slot_(ccord_store *s);
ccord_expr_data *const *ccord_store_expr_slot_const_(const ccord_store *s);
void ccord_store_expr_mutated_(ccord_store *s);

struct forum_cache;
void ccord_store_forum_cache_destroy(struct forum_cache *f);
struct forum_cache **ccord_store_forum_slot(ccord_store *s);
const struct forum_cache *ccord_store_forum_slot_const(const ccord_store *s);
void ccord_store_forum_forget_guild(ccord_store *s, ccord_id guild_id);
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

#define CCORD_STORE_STATUS_MAX_BYTES 128u
#define CCORD_STORE_TOPIC_MAX_BYTES 1024u
#define CCORD_STORE_URL_MAX_BYTES 512u
#define CCORD_STORE_MAX_SAVED_MESSAGES 256u   // local bookmarks; the oldest is dropped past this
#define CCORD_STORE_MAX_EVENTS_PER_GUILD 100u   // Discord's scheduled-event cap per guild
#define CCORD_STORE_MAX_PROFILES 64u            // LRU cap on cached ~25 KiB user profiles
#define CCORD_STORE_TYPING_UNIX_THRESHOLD 1000000000000LL
#define CCORD_STORE_TYPING_PRUNE_MARGIN_MS 15000LL
#define CCORD_STORE_MAX_TYPING 256u
#if CCORD_OS_ANDROID
#define CCORD_STORE_MAX_TOTAL_MESSAGES 4000u
#else
#define CCORD_STORE_MAX_TOTAL_MESSAGES 12000u
#endif

typedef struct note_entry {
  ccord_id user_id;
  char *note;
} note_entry;

// Small per-store string interner for ccord_member.role_name so 50k members don't duplicate heap strings.
// Owned by store_data and released wholesale in data_free().
typedef struct role_str_pool {
  char **items;
  size_t n, cap;
  size_t *slots;   // open-addressed hash table: item index + 1 (0 = empty)
  size_t hcap;     // power of two, or 0
} role_str_pool;

// Owned copy of the snapshot data. Every string pointer below is owned by the store.
typedef struct store_data {
  ccord_id self_id;
  ccord_user *users;                 size_t nusers, cap_users;
  ccord_user_profile *profiles;      size_t nprofiles, cap_profiles;
  ccord_guild *guilds;               ccord_id *guild_last; size_t nguilds, cap_guilds;
  ccord_id dm_last;
  ccord_channel *channels;           size_t nchannels, cap_channels;
  ccord_member *members;             size_t nmembers, cap_members;
  ccord_voice_state *voices;         size_t nvoices, cap_voices;
  ccord_relationship *relationships; size_t nrelationships, cap_relationships;
  note_entry *notes;                 size_t nnotes, capnotes;
  role_str_pool role_pool;
  // Where things are, so that lookups do not scan everything. Kept in step with the arrays above by the helpers below;
  // when one could not be (out of memory) its flag is false and lookups scan, as they always could.
  ccord_idmap user_by_id;  bool user_ix_ok;
  ccord_idmap chan_by_id;  store_index chan_ix;  bool chan_ix_ok;   // channels per guild, in array order
  store_index mem_ix;      bool mem_ix_ok;                          // members per guild, by user
  size_t dm_placeholder_count;                                      // DM channels with placeholder names
  size_t dm_missing_icon_count;                                     // 1:1 DM channels without an icon_url yet
} store_data;

static inline bool store_dm_is_placeholder_name(const ccord_channel *ch)
{
  return ch && ch->guild_id == CCORD_DM_GUILD_ID &&
         (!ch->name || ch->name[0] == '\0' || strcmp(ch->name, "Direct Message") == 0 ||
          strncmp(ch->name, "User ", 5) == 0);
}

static inline bool store_dm_is_missing_icon(const ccord_channel *ch)
{
  return ch && ch->guild_id == CCORD_DM_GUILD_ID &&
         ch->discord_type != CCORD_DISCORD_CHANNEL_GROUP_DM &&
         (!ch->icon_url || ch->icon_url[0] == '\0');
}

typedef struct history {
  ccord_id channel_id;
  bool loaded;
  bool stale;             // seeded from the on-disk cache: the next ccord_store_set_history reconciles
  ccord_id stale_max_id;  // newest confirmed id cached when it was marked stale
  ccord_message *msgs;
  size_t n, cap;
  uint64_t last_access_tick;
} history;

typedef struct typing_entry {
  ccord_id channel_id, user_id;
  int64_t expires_ms;
} typing_entry;

typedef struct slash_bucket {
  ccord_id guild_id;
  ccord_slash_command *cmds;
  ccord_slash_option *opts_pool;
  size_t n;
} slash_bucket;

typedef struct pin_bucket {
  ccord_id channel_id;
  ccord_id *msg_ids;
  size_t n, cap;
  bool loaded;
  bool has_more;
  char *next_cursor;
} pin_bucket;

// Guild scheduled events: one bucket per guild, value copies (every field is inline).
typedef struct event_bucket {
  ccord_id guild_id;
  ccord_scheduled_event *events;
  size_t n, cap;
} event_bucket;

struct ccord_store {
  ccord_bus *bus;
  uint64_t revision;
  uint64_t traffic_rev;   // how many of those bumps were only message traffic (see ccord_store_lists_revision)
  uint64_t warm_rev;      // bumps only on mutations to state persisted by warm_build_state()
  uint64_t badge_rev;     // bumps only on mutations that can affect unread/mention badge totals
  store_data d;
  ccord_id sel_guild, sel_channel;
  history *hist;
  size_t nhist, caphist;
  uint64_t hist_tick;
  size_t total_msgs;
  ccord_idmap hist_by_chan;
  bool hist_ix_ok;
  typing_entry *typing;
  size_t ntyping, captyping;
  ccord_voice_server *servers;
  size_t nservers, capservers;
  ccord_stream_state stream;
  bool has_stream;
  _Atomic bool stream_active;
  ccord_id speaking_users[64];
  size_t nspeaking;
  ccord_expr_data *expr;   // guild emojis/stickers + expression REST blobs (expressions.c)
  struct forum_cache *forum;   // forum tags/posts (store_forum.c)
  struct role_cache *roles;    // roles, member role ids, channel overwrites (store_roles.c)
  struct admin_cache *admin;   // Server Settings lists: bans, invites, audit log (store_admin.c)
  struct notify_cache *notify; // mute / notification settings (store_notify.c)
  struct listsize_cache *listsize; // how big the server says each member list is (store_listsize.c)
  struct voice_cache *voice;   // M5 calls, stages, voice status, streams, audio prefs, DAVE/RTC (store_voice.c)
  struct user_cache *user;     // M6 account settings (store_user.c)
  slash_bucket *slash;
  size_t nslash, capslash;
  pin_bucket *pins;
  size_t npins, cappins;
  ccord_message *saved;   // local bookmarks, newest first; deep copies that survive snapshots
  size_t nsaved, capsaved;
  event_bucket *events;   // guild scheduled events, written from transport answers only
  size_t nevents, capevents;
  ccord_store_rich_observer_fn rich_obs;
  void *rich_obs_ud;
  ccord_id reaction_actor_id;
};

extern const char store_k_empty_str[];
extern const char store_k_default_role_str[];

// Cross-TU internal helpers
const char *store_dup_cstr(const char *s);
void store_free_cstr(const char **p);
const char *store_role_pool_intern(role_str_pool *p, const char *s);
size_t store_next_cap_geom(size_t cur, size_t need, size_t min_cap);
bool store_ensure_users(store_data *d, size_t need);
bool store_ensure_profiles(store_data *d, size_t need);
bool store_ensure_guilds(store_data *d, size_t need);
bool store_ensure_channels(store_data *d, size_t need);
bool store_ensure_members(store_data *d, size_t need);
bool store_ensure_voices(store_data *d, size_t need);
bool store_ensure_relationships(store_data *d, size_t need);
void store_profile_free(ccord_user_profile *p);
ccord_user_activities *store_dup_activities(const ccord_user_activities *src, bool *out_oom);
void store_free_activities(ccord_user_activities **p);
bool store_activities_equal(const ccord_user_activities *a, const ccord_user_activities *b);
void store_voice_server_free(ccord_voice_server *srv);
void store_users_index_add(store_data *d, size_t i);
void store_members_index_add(store_data *d, size_t i);
void store_members_reindex(store_data *d);
void store_channels_reindex(store_data *d);
void store_post(ccord_store *s, ccord_event_type t, ccord_id g, ccord_id c, ccord_id m, ccord_id u, int64_t value);
void store_voice_servers_clear(ccord_store *s);
void store_slash_clear(ccord_store *s);
void store_slash_forget_guild(ccord_store *s, ccord_id guild_id);
void ccord_store_expr_forget_guild(ccord_store *s, ccord_id guild_id);
void store_evict_lru_histories(ccord_store *s, ccord_id protect_channel_id, size_t target_total);
void store_pins_clear(ccord_store *s);
void store_saved_clear(ccord_store *s);
void store_events_clear(ccord_store *s);
void store_event_bucket_drop(ccord_store *s, ccord_id guild_id);
pin_bucket *store_find_pin_bucket(const ccord_store *s, ccord_id channel_id);
pin_bucket *store_pin_bucket_for(ccord_store *s, ccord_id channel_id, ccord_error *err);
void store_bump_traffic(ccord_store *s);
long store_find_guild(const store_data *d, ccord_id id);
long store_find_channel(const store_data *d, ccord_id id);
ccord_id store_default_channel(const store_data *d, ccord_id guild);
ccord_id store_default_channel_for(const ccord_store *s, ccord_id guild);
bool store_text_ok(const char *s, size_t max);
long store_find_msg_index(const history *h, ccord_id message_id);
void store_typing_remove(ccord_store *s, ccord_id channel_id, ccord_id user_id);
void store_hist_reindex(ccord_store *s);
void store_history_remove(ccord_store *s, ccord_id channel_id);
void store_voice_remove_channel(ccord_store *s, ccord_id channel_id);
void store_resort_channels(store_data *d, ccord_id guild_id);
void store_sort_guild_channels(store_data *d, ccord_id guild_id);
history *store_history_for(ccord_store *s, ccord_id channel_id, ccord_error *err);
void store_msg_free(ccord_message *m);
history *store_find_history(const ccord_store *s, ccord_id channel_id);
long store_find_user(const store_data *d, ccord_id id);
bool store_inline_text_ok(const char *buf, size_t cap);
long store_find_member(const store_data *d, ccord_id guild_id, ccord_id user_id);
