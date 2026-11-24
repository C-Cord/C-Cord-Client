// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/event_bus.h>
#include <ccord/model/forum.h>
#include <ccord/model/model.h>
#include <ccord/model/rich.h>
// Single source of truth for UI state. The store never talks to the network and never draws.
// Mutators deep-copy their inputs, then post events to the bus (if non-null). Events are delivered on ccord_bus_drain.
typedef struct ccord_store ccord_store;
ccord_store *ccord_store_create(ccord_bus *bus, ccord_error *err);     // bus may be nullptr (tests)
void ccord_store_destroy(ccord_store *s);
void ccord_store_reset(ccord_store *s);
uint64_t ccord_store_revision(const ccord_store *s);                    // bumps on every successful mutation
// Like ccord_store_revision, but it does not move for changes that only concern messages and typing (a message arrived,
// was edited, read, reacted to): the lists of channels and members cannot have changed then. A view that only depends on
// those lists compares this one and skips its rebuild. Anything else (and a conversation moving up the list) moves it.
uint64_t ccord_store_lists_revision(const ccord_store *s);
// Revision of state actually persisted by the warm-start disk cache (snapshot, users, guilds, channels, members,
// relationships, roles, overwrites, notify settings, selection). Does not move on typing, presence/activities,
// messages, reactions, pins, voice states, speaking, or RTC stats.
uint64_t ccord_store_warm_revision(const ccord_store *s);
// Revision of state that can affect unread/mention badge counts (unread/mention counters, read acks, mute/notify
// overrides, channel/guild/role/overwrite/self-member changes, snapshots). Does not move on typing, presence,
// speaking, RTC stats, message edits, or reactions.
uint64_t ccord_store_badge_revision(const ccord_store *s);
// Whether the lookup indexes (users, channels, members) are all in use. They are only missing after running out of memory,
// when every lookup falls back to scanning: still correct, only slower. A diagnostic for tests.
bool ccord_store_indexes_ready(const ccord_store *s);
// ---- mutators ----
// Validates, replaces all guild/channel/member/user data, keeps selection if still valid else selects first guild+first text channel.
// Posts guilds.loaded (+ guild.select/channel.select if selection changed).
[[nodiscard]] ccord_status ccord_store_set_snapshot(ccord_store *s, const ccord_snapshot *snap, ccord_error *err);
// Like ccord_store_set_snapshot; when keep_history is true the message history of every channel that still exists in
// `snap` is kept (not freed) provided snap->self_id == the stored self id, else all history is dropped as today.
[[nodiscard]] ccord_status ccord_store_set_snapshot_ex(ccord_store *s, const ccord_snapshot *snap, bool keep_history,
                                                       ccord_error *err);
// The channel's history was loaded from the on-disk cache: the next ccord_store_set_history for it reconciles
// (drops cached messages the live page proves deleted, keeps local echoes and ids newer than the cache) and clears
// the flag. No-op for a channel without history.
void ccord_store_mark_history_stale(ccord_store *s, ccord_id channel_id);
bool ccord_store_history_stale(const ccord_store *s, ccord_id channel_id);
// Selecting the already-selected guild/channel is OK and posts nothing. Unknown id -> CCORD_ERR_NOT_FOUND.
// select_guild picks the guild's last selected channel (default first TEXT channel); posts guild.select then channel.select.
// select_channel on a channel in another guild also switches guild (guild.select first). Voice/category channels -> CCORD_ERR_INVALID.
[[nodiscard]] ccord_status ccord_store_select_guild(ccord_store *s, ccord_id guild_id, ccord_error *err);
[[nodiscard]] ccord_status ccord_store_select_channel(ccord_store *s, ccord_id channel_id, ccord_error *err);
// Replaces/merges history (ascending id, dedup by id, kept sorted). Posts messages.loaded. Marks history_loaded.
[[nodiscard]] ccord_status ccord_store_set_history(ccord_store *s, ccord_id channel_id, const ccord_message *msgs, size_t n, ccord_error *err);
// Appends one live message (id must be > last id, else inserted sorted; duplicate id -> CCORD_ERR_INVALID). Posts message.create.
[[nodiscard]] ccord_status ccord_store_append_message(ccord_store *s, const ccord_message *m, ccord_error *err);
// ---- live event mutators ----
// Edits an existing message (identified by m->channel_id + m->id). Posts message.update.
[[nodiscard]] ccord_status ccord_store_update_message(ccord_store *s, const ccord_message *m, ccord_error *err);
// Removes an existing message and its content. Posts message.delete. Missing id -> CCORD_ERR_NOT_FOUND.
[[nodiscard]] ccord_status ccord_store_delete_message(ccord_store *s, ccord_id channel_id, ccord_id message_id, ccord_error *err);
// Removes multiple messages from a channel (MESSAGE_DELETE_BULK). Uncached message IDs are ignored.
[[nodiscard]] ccord_status ccord_store_delete_messages_bulk(ccord_store *s, ccord_id channel_id,
                                                            const ccord_id *message_ids, size_t count,
                                                            ccord_error *err);
// Replaces the aggregate reaction count and the self-reacted flag. Posts reaction.update.
[[nodiscard]] ccord_status ccord_store_set_reactions(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                                     uint32_t count, bool self_reacted, ccord_error *err);
// Adds/replaces/removes one emoji's reaction on a message and recomputes the aggregate.
// count == 0 removes the emoji. Missing message -> CCORD_ERR_NOT_FOUND.
[[nodiscard]] ccord_status ccord_store_set_reaction(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                                    const char *emoji, uint32_t count, bool self_reacted,
                                                    ccord_error *err);
[[nodiscard]] ccord_status ccord_store_set_reaction_ex(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                                       const char *emoji, uint32_t count, bool self_reacted,
                                                       uint32_t burst_count, bool me_burst, ccord_error *err);
// Applies a signed delta (+1/-1) to one emoji's reaction on a message (live MESSAGE_REACTION_ADD/REMOVE).
// When is_self is true, updates the emoji's self_reacted flag (delta > 0 sets it, delta < 0 clears it).
[[nodiscard]] ccord_status ccord_store_adjust_reaction(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                                       const char *emoji, int32_t delta, bool is_self,
                                                       ccord_error *err);
[[nodiscard]] ccord_status ccord_store_adjust_reaction_ex(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                                          const char *emoji, int32_t delta, bool is_self, bool burst,
                                                          ccord_error *err);
// Applies a MESSAGE_POLL_VOTE_ADD/REMOVE gateway event to a stored poll message. Traffic-only (lists_revision stays put).
[[nodiscard]] ccord_status ccord_store_apply_poll_vote(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                                       uint32_t answer_id, bool is_self, bool added,
                                                       ccord_error *err);
// Updates a channel's unread and mention counters. Posts channel.upsert.
[[nodiscard]] ccord_status ccord_store_set_channel_counts(ccord_store *s, ccord_id channel_id, uint32_t unread,
                                                          uint32_t mentions, ccord_error *err);
// A channel's read position moved (the user's own acknowledgement, or another session's): `acked_id` is the newest
// message read. Anything newer stays unread, so a stale acknowledgement does not clear a busy channel: the unread count
// becomes the loaded messages after it (at least 1 when the channel is known to have newer ones) and a channel with
// nothing newer has no unread and no mentions. `mentions` is the server's count after the acknowledgement;
// UINT32_MAX = not told (the current count is kept, never more than the unread ones). Posts channel.upsert.
[[nodiscard]] ccord_status ccord_store_apply_read(ccord_store *s, ccord_id channel_id, ccord_id acked_id, uint32_t mentions,
                                                  ccord_error *err);
// Updates a user's presence (and status text when non-null), inserting a stub user on first sight.
// Posts presence.update.
[[nodiscard]] ccord_status ccord_store_update_presence(ccord_store *s, ccord_id user_id, ccord_presence presence,
                                                       const char *status_text, ccord_error *err);
// Adds/replaces a typing indicator for user in channel, expiring at expires_ms. Posts typing.start.
[[nodiscard]] ccord_status ccord_store_set_typing(ccord_store *s, ccord_id channel_id, ccord_id user_id,
                                                  int64_t expires_ms, ccord_error *err);
// Inserts or replaces a channel; keeps display order within its guild. Posts channel.upsert.
[[nodiscard]] ccord_status ccord_store_upsert_channel(ccord_store *s, const ccord_channel *channel, ccord_error *err);
// Removes a channel, its history and typing state; repairs selection. Posts channel.delete.
[[nodiscard]] ccord_status ccord_store_delete_channel(ccord_store *s, ccord_id channel_id, ccord_error *err);
// ---- guild lifecycle (GUILD_CREATE/UPDATE/DELETE, GUILD_MEMBERS_CHUNK) ----
// Inserts or replaces a guild, its channels and members. Unknown users in `users` are upserted
// first so members can refer to them. Missing guild -> inserted; existing -> name replaced.
// The DM pseudo guild is never inserted as a real guild and does not consume a slot.
[[nodiscard]] ccord_status ccord_store_upsert_guild(ccord_store *s, const ccord_guild *guild,
                                                    const ccord_channel *channels, size_t channel_count,
                                                    const ccord_member *members, size_t member_count,
                                                    const ccord_user *users, size_t user_count, ccord_error *err);
// Removes a guild, its channels, histories, typing state, members and voice state; repairs selection.
[[nodiscard]] ccord_status ccord_store_delete_guild(ccord_store *s, ccord_id guild_id, ccord_error *err);
// Inserts or replaces members of a guild (GUILD_MEMBERS_CHUNK), upserting their users first.
[[nodiscard]] ccord_status ccord_store_upsert_guild_members(ccord_store *s, ccord_id guild_id,
                                                            const ccord_member *members, size_t member_count,
                                                            const ccord_user *users, size_t user_count,
                                                            ccord_error *err);
// Inserts or replaces one user (used for PRESENCE_UPDATE stubs and guild payload presences).
[[nodiscard]] ccord_status ccord_store_upsert_user(ccord_store *s, const ccord_user *user, ccord_error *err);
// Inserts or updates a user's extended profile (bio, pronouns, banner_url, accent_color, handle, display_name, avatar_url).
[[nodiscard]] ccord_status ccord_store_upsert_user_profile(ccord_store *s, const ccord_user_profile *profile,
                                                           ccord_error *err);
// Inserts, updates, or removes (when kind == CCORD_REL_NONE) a user relationship (friend/blocked/pending).
[[nodiscard]] ccord_status ccord_store_set_relationship(ccord_store *s, ccord_id user_id,
                                                        ccord_relationship_kind kind, ccord_error *err);
[[nodiscard]] ccord_status ccord_store_remove_relationship(ccord_store *s, ccord_id user_id, ccord_error *err);
// ---- voice / streaming / pins ----
// Inserts or replaces a user's voice state (identified by guild_id + user_id).
// channel_id == 0 removes it (the user left). Posts voice.state.
[[nodiscard]] ccord_status ccord_store_set_voice_state(ccord_store *s, const ccord_voice_state *state, ccord_error *err);
// Stores the voice handshake endpoint/token for a guild. The token is copied and never logged.
// Posts voice.server.
[[nodiscard]] ccord_status ccord_store_set_voice_server(ccord_store *s, const ccord_voice_server *server, ccord_error *err);
// Stores/clears the active self stream (guild_id + config + active flag). Posts stream.state.
[[nodiscard]] ccord_status ccord_store_set_stream(ccord_store *s, const ccord_stream_state *state, ccord_error *err);
// Sets a message's pinned flag. Posts message.pin. Missing message -> CCORD_ERR_NOT_FOUND.
[[nodiscard]] ccord_status ccord_store_set_message_pinned(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                                          bool pinned, ccord_error *err);
// Merges the REST pin list into the cache without marking the full channel history as loaded.
[[nodiscard]] ccord_status ccord_store_set_pins(ccord_store *s, ccord_id channel_id, const ccord_message *msgs,
                                                size_t n, ccord_error *err);
// Merges a paginated pin page (newest-pinned first), tracking whether older pins exist and their `before` cursor.
[[nodiscard]] ccord_status ccord_store_set_pins_page(ccord_store *s, ccord_id channel_id, const ccord_message *msgs,
                                                     size_t n, bool append, bool has_more, const char *next_cursor,
                                                     ccord_error *err);
bool ccord_store_channel_pins_loaded(const ccord_store *s, ccord_id channel_id);
const ccord_id *ccord_store_channel_pin_ids(const ccord_store *s, ccord_id channel_id, size_t *out_count);
bool ccord_store_channel_pins_has_more(const ccord_store *s, ccord_id channel_id);
const char *ccord_store_channel_pins_cursor(const ccord_store *s, ccord_id channel_id);
// Updates the self user's thread membership flag on a thread channel. Posts channel.upsert.
[[nodiscard]] ccord_status ccord_store_set_thread_joined(ccord_store *s, ccord_id thread_id, bool joined,
                                                         ccord_error *err);
// ---- readers (borrowed; valid until the next mutator call) ----
ccord_id ccord_store_self_id(const ccord_store *s);
size_t ccord_store_user_count(const ccord_store *s);
const ccord_user *ccord_store_user_at(const ccord_store *s, size_t i);
const ccord_user *ccord_store_user(const ccord_store *s, ccord_id id);
const ccord_user_profile *ccord_store_user_profile(const ccord_store *s, ccord_id user_id);
size_t ccord_store_guild_count(const ccord_store *s);
const ccord_guild *ccord_store_guild_at(const ccord_store *s, size_t i);
const ccord_guild *ccord_store_guild(const ccord_store *s, ccord_id id);
ccord_id ccord_store_selected_guild(const ccord_store *s);
ccord_id ccord_store_selected_channel(const ccord_store *s);
size_t ccord_store_channel_count(const ccord_store *s, ccord_id guild_id);
const ccord_channel *ccord_store_channel_at(const ccord_store *s, ccord_id guild_id, size_t i);   // display order
const ccord_channel *ccord_store_channel(const ccord_store *s, ccord_id channel_id);
size_t ccord_store_member_count(const ccord_store *s, ccord_id guild_id);
const ccord_member *ccord_store_member_at(const ccord_store *s, ccord_id guild_id, size_t i);
const ccord_member *ccord_store_find_member(const ccord_store *s, ccord_id guild_id, ccord_id user_id);
bool ccord_store_history_loaded(const ccord_store *s, ccord_id channel_id);
size_t ccord_store_message_count(const ccord_store *s, ccord_id channel_id);
const ccord_message *ccord_store_message_at(const ccord_store *s, ccord_id channel_id, size_t i);   // 0 = oldest
const ccord_message *ccord_store_find_message(const ccord_store *s, ccord_id channel_id, ccord_id message_id);   // binary search
size_t ccord_store_typing_count(const ccord_store *s, ccord_id channel_id);
ccord_id ccord_store_typing_user_at(const ccord_store *s, ccord_id channel_id, size_t i);
int64_t ccord_store_typing_expires_at(const ccord_store *s, ccord_id channel_id, size_t i);
size_t ccord_store_voice_count(const ccord_store *s, ccord_id channel_id);
const ccord_voice_state *ccord_store_voice_at(const ccord_store *s, ccord_id channel_id, size_t i);
const ccord_voice_state *ccord_store_voice_states(const ccord_store *s, size_t *out_count);   // all active voice states, borrowed
const ccord_voice_state *ccord_store_voice_user(const ccord_store *s, ccord_id guild_id, ccord_id user_id);
ccord_id ccord_store_active_voice_channel(const ccord_store *s);           // self's channel, 0 = not connected
const ccord_voice_server *ccord_store_voice_server(const ccord_store *s, ccord_id guild_id);
const ccord_stream_state *ccord_store_stream(const ccord_store *s);        // null = no stream tracked
bool ccord_store_stream_active(const ccord_store *s);
void ccord_store_set_speaking(ccord_store *s, ccord_id user_id, bool speaking);
bool ccord_store_user_speaking(const ccord_store *s, ccord_id user_id);
// ---- M5 voice, DM calls, Go Live, stage, DAVE ----
// These caches hold transport answers only: the session writes them from CALL_*/STAGE_INSTANCE_*/
// VOICE_STATUS_UPDATE and STREAM_SIGNAL events, never from a local click. They post no bus event
// (there is none for them); the revision moves so revision-based views repaint. Inline strings
// (call.region, stage.topic) are copied; a voice status is heap-copied and never borrowed.
// A DM/group call row: channel, region, ringing[] and active/ended. Its participants are the
// channel's voice states (ccord_store_voice_count/ccord_store_voice_at), already maintained above.
[[nodiscard]] ccord_status ccord_store_set_call(ccord_store *s, const ccord_call_state *call, ccord_error *err);
[[nodiscard]] ccord_status ccord_store_remove_call(ccord_store *s, ccord_id channel_id, ccord_error *err);
const ccord_call_state *ccord_store_call(const ccord_store *s, ccord_id channel_id);   // null = none
size_t ccord_store_call_count(const ccord_store *s);
const ccord_call_state *ccord_store_call_at(const ccord_store *s, size_t index);

[[nodiscard]] ccord_status ccord_store_set_stage_instance(ccord_store *s, const ccord_stage_instance *stage,
                                                          ccord_error *err);
[[nodiscard]] ccord_status ccord_store_remove_stage_instance(ccord_store *s, ccord_id channel_id, ccord_error *err);
const ccord_stage_instance *ccord_store_stage_instance(const ccord_store *s, ccord_id channel_id);
// Stage speaker / audience state lives on ccord_voice_state.suppress and request_to_speak_timestamp.
// The targeted setters update an existing voice state in `channel_id` (NOT_FOUND when the user is not
// there); a speaker is a joined user with suppress == false.
[[nodiscard]] ccord_status ccord_store_set_stage_suppress(ccord_store *s, ccord_id guild_id, ccord_id channel_id,
                                                          ccord_id user_id, bool suppress, ccord_error *err);
[[nodiscard]] ccord_status ccord_store_set_stage_request_to_speak(ccord_store *s, ccord_id guild_id,
                                                                  ccord_id channel_id, ccord_id user_id,
                                                                  int64_t requested_at_ms, ccord_error *err);
size_t ccord_store_stage_speaker_count(const ccord_store *s, ccord_id guild_id, ccord_id channel_id);
size_t ccord_store_stage_request_count(const ccord_store *s, ccord_id guild_id, ccord_id channel_id);

// PUT /channels/{id}/voice-status text. nullptr or "" clears it; at most 500 characters of valid UTF-8.
[[nodiscard]] ccord_status ccord_store_set_voice_status(ccord_store *s, ccord_id channel_id, const char *status,
                                                        ccord_error *err);
const char *ccord_store_voice_status(const ccord_store *s, ccord_id channel_id);   // null = cleared

// Go Live broadcasts, keyed by (channel_id, user_id), with the server-reported viewer count
// (STREAM_SIGNAL carries a count, not viewer identities). ccord_store_stream()/stream_active()
// above stay the self-stream record used by start/stop.
[[nodiscard]] ccord_status ccord_store_stream_upsert(ccord_store *s, const ccord_stream_state *stream,
                                                     uint32_t viewer_count, ccord_error *err);
[[nodiscard]] ccord_status ccord_store_stream_remove(ccord_store *s, ccord_id channel_id, ccord_id user_id,
                                                     ccord_error *err);
const ccord_stream_state *ccord_store_stream_find(const ccord_store *s, ccord_id channel_id, ccord_id user_id);
// STREAM_UPDATE viewer-count change for an existing broadcast (media traffic; NOT_FOUND before CREATE).
[[nodiscard]] ccord_status ccord_store_stream_set_viewers(ccord_store *s, ccord_id channel_id, ccord_id user_id,
                                                          uint32_t viewer_count, ccord_error *err);
uint32_t ccord_store_stream_viewer_count(const ccord_store *s, ccord_id channel_id, ccord_id user_id);
size_t ccord_store_stream_list_count(const ccord_store *s);
const ccord_stream_state *ccord_store_stream_list_at(const ccord_store *s, size_t index);

// ---- embedded activities (per voice channel) ----
// Running activity instances of a voice channel, written only from gateway EMBEDDED_ACTIVITY_UPDATE_V2 (or a REST
// answer). `set` replaces the whole channel list; `upsert` applies one update and removes the instance when
// participant_count == 0 (it ended). Values are copied; readers are borrowed until the next mutator.
[[nodiscard]] ccord_status ccord_store_set_activity_instances(ccord_store *s, ccord_id channel_id,
                                                             const ccord_activity_instance *instances, size_t count,
                                                             ccord_error *err);
[[nodiscard]] ccord_status ccord_store_upsert_activity_instance(ccord_store *s,
                                                                const ccord_activity_instance *instance,
                                                                ccord_error *err);
size_t ccord_store_activity_instance_count(const ccord_store *s, ccord_id channel_id);
const ccord_activity_instance *ccord_store_activity_instance_at(const ccord_store *s, ccord_id channel_id, size_t i);
const ccord_activity_instance *ccord_store_find_activity_instance(const ccord_store *s, ccord_id instance_id);

// Per-user playback gain (0..200 %, 100 = unity) and local mute. Client-only preferences: they never
// change Discord state, and only the traffic revision moves (no list row is added).
[[nodiscard]] ccord_status ccord_store_set_user_volume(ccord_store *s, ccord_id user_id, uint8_t pct, ccord_error *err);
uint8_t ccord_store_user_volume(const ccord_store *s, ccord_id user_id);   // default 100
[[nodiscard]] ccord_status ccord_store_set_user_local_mute(ccord_store *s, ccord_id user_id, bool muted,
                                                           ccord_error *err);
bool ccord_store_user_local_mute(const ccord_store *s, ccord_id user_id);

// DAVE (E2EE) and RTC debug snapshots; copied out, zeroed when no voice connection has reported yet.
[[nodiscard]] ccord_status ccord_store_set_dave_status(ccord_store *s, const ccord_dave_status *status,
                                                       ccord_error *err);
void ccord_store_dave_status(const ccord_store *s, ccord_dave_status *out);
[[nodiscard]] ccord_status ccord_store_set_rtc_stats(ccord_store *s, const ccord_rtc_stats *stats, ccord_error *err);
void ccord_store_rtc_stats(const ccord_store *s, ccord_rtc_stats *out);

size_t ccord_store_relationship_count(const ccord_store *s);
const ccord_relationship *ccord_store_relationship_at(const ccord_store *s, size_t i);
ccord_relationship_kind ccord_store_relationship(const ccord_store *s, ccord_id user_id);
// ---- forums (type 15 / 16 channels; posts are threads) ----
// The forum cache is separate from the channel list: a post only becomes a channel (so it can be selected and
// read like a text channel) when ccord_store_forum_open_post materialises it. Everything is deep-copied.
// Replaces the tag list / settings of each forum in `metas`. One channel.upsert event is posted.
[[nodiscard]] ccord_status ccord_store_forum_set_metas(ccord_store *s, const ccord_forum_meta *metas, size_t n,
                                                       ccord_error *err);
// Inserts or merges posts (by thread_id). A post without starter-message data keeps the data already known.
[[nodiscard]] ccord_status ccord_store_forum_upsert_posts(ccord_store *s, const ccord_forum_post *posts, size_t n,
                                                          ccord_error *err);
[[nodiscard]] ccord_status ccord_store_forum_remove_post(ccord_store *s, ccord_id thread_id, ccord_error *err);
// Counts a message in its post (thread): replies += 1, last activity; the starter message (id == thread id) fills
// the excerpt instead. CCORD_ERR_NOT_FOUND when the channel is not a cached post.
[[nodiscard]] ccord_status ccord_store_forum_apply_message(ccord_store *s, const ccord_message *m, ccord_error *err);
[[nodiscard]] ccord_status ccord_store_forum_set_state(ccord_store *s, ccord_id forum_id, const ccord_forum_state *st,
                                                       ccord_error *err);
// Drops every forum's posts and paging state (a fresh READY); tags are replaced by the following set_metas.
void ccord_store_forum_reset(ccord_store *s);
// Materialises the post as a thread channel (parent = forum) and selects it. Needs the forum channel in the store.
[[nodiscard]] ccord_status ccord_store_forum_open_post(ccord_store *s, ccord_id thread_id, ccord_error *err);
const ccord_forum_meta *ccord_store_forum_meta(const ccord_store *s, ccord_id forum_id);
const ccord_forum_post *ccord_store_forum_posts(const ccord_store *s, ccord_id forum_id, size_t *out_count);   // contiguous, unordered
const ccord_forum_post *ccord_store_forum_find_post(const ccord_store *s, ccord_id thread_id);
const ccord_forum_state *ccord_store_forum_state(const ccord_store *s, ccord_id forum_id);   // never null for a known forum; null if unknown
// True when `channel_id` is a forum or media channel known to the store.
bool ccord_store_channel_is_forum(const ccord_store *s, ccord_id channel_id);
// Computes whether `user_id` holds all bits in `perm_mask` in `guild_id` (and optionally `channel_id`).
// Returns true for the guild owner or any member with CCORD_PERM_ADMINISTRATOR.
bool ccord_store_has_permission(const ccord_store *s, ccord_id guild_id, ccord_id channel_id,
                                ccord_id user_id, uint64_t perm_mask);
// Removes a member from a guild (used on kick/ban or GUILD_MEMBER_REMOVE).
[[nodiscard]] ccord_status ccord_store_remove_member(ccord_store *s, ccord_id guild_id, ccord_id user_id,
                                                     ccord_error *err);
// Locally dismisses an ephemeral message (CCORD_MSGFLAG_EPHEMERAL). Rejects non-ephemeral messages with CCORD_ERR_PERMISSION.
[[nodiscard]] ccord_status ccord_store_dismiss_ephemeral(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                                         ccord_error *err);
// Per-guild (or CCORD_DM_GUILD_ID) application slash commands from GET /channels/{id}/application-command-index.
[[nodiscard]] ccord_status ccord_store_set_slash_commands(ccord_store *s, ccord_id guild_id,
                                                          const ccord_slash_command *cmds, size_t count,
                                                          ccord_error *err);
const ccord_slash_command *ccord_store_slash_commands(const ccord_store *s, ccord_id guild_id, size_t *out_count);
// Reorders the guild rail and applies folder assignments (USER_SETTINGS guild_folders). Unknown guild ids are ignored;
// guilds already in the store but omitted from `items` keep their relative order at the tail. Posts guilds.loaded.
[[nodiscard]] ccord_status ccord_store_set_guild_layout(ccord_store *s, const ccord_guild_folder_item *items,
                                                        size_t count, ccord_error *err);
// Updates display positions (and optional category parents when `has_parent && has_parent[i]`) for channels in a guild,
// resorts the guild's channel list, and posts channel.upsert.
[[nodiscard]] ccord_status ccord_store_reorder_channels(ccord_store *s, ccord_id guild_id, const ccord_id *channel_ids,
                                                        const int32_t *positions, const ccord_id *parent_ids,
                                                        const uint8_t *has_parent, size_t count, ccord_error *err);
// Personal user notes (READY.notes + USER_NOTE_UPDATE). Returns "" when no note is stored for `user_id`.
const char *ccord_store_user_note(const ccord_store *s, ccord_id user_id);
[[nodiscard]] ccord_status ccord_store_set_user_note(ccord_store *s, ccord_id user_id, const char *note,
                                                     ccord_error *err);
// Authoritative presence + custom status + Rich Presence activity update (PRESENCE_UPDATE).
[[nodiscard]] ccord_status ccord_store_update_presence_detail(
  ccord_store *s, ccord_id user_id, ccord_presence presence, const char *status_text, const char *custom_emoji,
  const char *custom_text, int64_t custom_expires_ms, bool has_activity, ccord_activity_type activity_type,
  const char *activity_name, const char *activity_details, const char *activity_state, int64_t activity_start_ms,
  int64_t activity_end_ms, ccord_error *err);
// Own profile after a successful edit (REST answer of PATCH /users/@me[/profile]). Unlike ccord_store_upsert_user,
// "" really clears: only the fields flagged set_* change. display_name "" falls back to the username.
typedef struct ccord_self_profile_update {
  bool set_display_name, set_avatar, set_banner, set_bio, set_pronouns, set_accent;
  const char *display_name, *avatar_url, *banner_url, *bio, *pronouns;   // CDN URLs for avatar/banner
  uint32_t accent_color;                                                 // 0 = none
  // Extended global / per-server profile fields (zero-init safe).
  ccord_id guild_id;                                                     // CCORD_ID_NONE = global; non-zero = per-server
  bool set_handle;
  const char *handle;
  bool set_theme_colors;
  bool theme_colors_set;
  uint32_t theme_primary;
  uint32_t theme_accent;
  bool set_avatar_decoration;
  ccord_id avatar_decoration_sku_id;
  const char *avatar_decoration_url;
  bool set_profile_effect;
  ccord_id profile_effect_id;
  const ccord_profile_effect_config *profile_effect;   // optional resolved config (nullptr keeps/clears)
  bool set_profile_frame;
  ccord_id profile_frame_sku_id;
  const ccord_profile_frame_config *profile_frame;     // optional resolved frame config (nullptr keeps/clears)
  bool set_nameplate;
  ccord_id nameplate_sku_id;
  const char *nameplate_asset;
  const char *nameplate_palette;
  bool set_clan;
  ccord_id clan_guild_id;
  const char *clan_tag;
  const char *clan_badge;
} ccord_self_profile_update;
[[nodiscard]] ccord_status ccord_store_set_self_profile(ccord_store *s, const ccord_self_profile_update *u,
                                                        ccord_error *err);
// Updates the authenticated user's custom status from USER_SETTINGS_UPDATE / USER_SETTINGS_PROTO_UPDATE.
[[nodiscard]] ccord_status ccord_store_set_self_custom_status(ccord_store *s, const char *emoji, const char *text,
                                                              int64_t expires_ms, bool clear, ccord_error *err);
// Mutates recipients on a Group DM channel (CHANNEL_RECIPIENT_ADD / CHANNEL_RECIPIENT_REMOVE).
[[nodiscard]] ccord_status ccord_store_change_dm_recipient(ccord_store *s, ccord_id channel_id, ccord_id user_id,
                                                           bool added, ccord_error *err);
// Marks a DM / Group DM channel as accepted (clears is_message_request and is_spam).
[[nodiscard]] ccord_status ccord_store_accept_message_request(ccord_store *s, ccord_id channel_id, ccord_error *err);
// Updates raid-protection / incident action timestamps on a server (PUT /guilds/{g}/incident-actions).
[[nodiscard]] ccord_status ccord_store_set_guild_incidents(ccord_store *s, ccord_id guild_id, int64_t invites_until_ms,
                                                           int64_t dms_until_ms, ccord_error *err);
// Updates the server's vanity invite code (PATCH /guilds/{g}/vanity-url).
[[nodiscard]] ccord_status ccord_store_set_guild_vanity(ccord_store *s, ccord_id guild_id, const char *code,
                                                        ccord_error *err);
// ---- M6 account settings ----
// The single source of truth for Discord account settings. The session applies only a full REST
// answer or a USER_SETTINGS_UPDATE here (never a local optimistic click); a fresh snapshot drops
// them because a new READY carries its own account. All fields are fixed-size, so the store copies
// the struct by value. nullptr is returned until the first update. Changes move only the traffic
// revision (no list row is added). `ccord_store_apply_user_settings_patch` merges the -1/"" sentinel
// fields of a PATCH payload onto the stored settings (or Discord defaults when none arrived yet).
[[nodiscard]] ccord_status ccord_store_set_user_settings(ccord_store *s, const ccord_user_settings *settings,
                                                         ccord_error *err);
[[nodiscard]] ccord_status ccord_store_apply_user_settings_patch(ccord_store *s,
                                                                 const ccord_user_settings_patch *patch,
                                                                 ccord_error *err);
const ccord_user_settings *ccord_store_user_settings(const ccord_store *s);

// ---- messaging parity: mark unread & saved messages (bookmarks) ----
// "Mark Unread" from `message_id`: that message and everything after it counts as unread again (the read position
// moves to the message before it). The transport ack rides ccord_session_mark_unread; this is the local state half.
// Unknown channel / message -> CCORD_ERR_NOT_FOUND. Posts channel.upsert.
[[nodiscard]] ccord_status ccord_store_mark_unread(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                                   ccord_error *err);
// Saved messages are a local bookmark list (deep copies, newest first) persisted by the app across runs.
// Saving an already-saved message refreshes its copy and moves it to the front; unknown ids on unsave are ignored.
[[nodiscard]] ccord_status ccord_store_save_message(ccord_store *s, const ccord_message *m, ccord_error *err);
[[nodiscard]] ccord_status ccord_store_unsave_message(ccord_store *s, ccord_id channel_id, ccord_id message_id,
                                                      ccord_error *err);
bool ccord_store_message_saved(const ccord_store *s, ccord_id channel_id, ccord_id message_id);
size_t ccord_store_saved_message_count(const ccord_store *s);
const ccord_message *ccord_store_saved_message_at(const ccord_store *s, size_t i);   // 0 = most recently saved

// ---- guild scheduled events ----
// Per-guild event lists, written only from transport answers (SCHEDULED_EVENT_LIST replaces the guild's list;
// SCHEDULED_EVENT_UPDATE upserts / removes one; USER_CHANGE adjusts user_count / me_rsvped). Deep-copied;
// readers are borrowed until the next mutator. A fresh READY / guild removal drops the guild's events.
[[nodiscard]] ccord_status ccord_store_set_scheduled_events(ccord_store *s, ccord_id guild_id,
                                                            const ccord_scheduled_event *events, size_t n,
                                                            ccord_error *err);
[[nodiscard]] ccord_status ccord_store_upsert_scheduled_event(ccord_store *s, const ccord_scheduled_event *event,
                                                              ccord_error *err);
bool ccord_store_remove_scheduled_event(ccord_store *s, ccord_id guild_id, ccord_id event_id);
// RSVP state change for the signed-in user (USER_CHANGE about self, or a REST ack). NOT_FOUND for an unknown event.
[[nodiscard]] ccord_status ccord_store_set_scheduled_event_rsvp(ccord_store *s, ccord_id guild_id, ccord_id event_id,
                                                                bool rsvped, ccord_error *err);
size_t ccord_store_scheduled_event_count(const ccord_store *s, ccord_id guild_id);
const ccord_scheduled_event *ccord_store_scheduled_event_at(const ccord_store *s, ccord_id guild_id, size_t i);
const ccord_scheduled_event *ccord_store_find_scheduled_event(const ccord_store *s, ccord_id event_id);   // any guild
// Releases expendable store caches (inactive channel histories / shrinking maps) under memory pressure.
void ccord_store_trim_memory(ccord_store *s);
