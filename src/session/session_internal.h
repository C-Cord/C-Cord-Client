// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Internal state, heap-allocated media cache, and cross-TU prototypes for src/session/session_*.c.
#pragma once

#include <ccord/session/session.h>
#include <ccord/session/media_stream.h>
#include "media_stream_hub.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/base64.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>
#include <ccord/model/rich.h>
#include <ccord/net/discord_proto.h>
#include <ccord/net/profile_edit.h>
#include <ccord/store/admin.h>
#include <ccord/store/expressions.h>
#include <ccord/store/listsize.h>
#include <ccord/store/notify.h>
#include <ccord/store/roles.h>
#include <ccord/store/unread.h>

#include <stdatomic.h>

#define PUMP_BATCH 64u
#define SESSION_MAX_NOTIFICATIONS 32u
#define SESSION_MAX_FAILURES 8u
#define SESSION_MAX_REPORT_EVENTS 4u   // report answers wait here for the UI; newest dropped when full
#define SESSION_MAX_MODALS 4u
#define SESSION_MAX_MEDIA 2048u
#define SESSION_MEDIA_HASH_CAP 4096u
#define SESSION_MEDIA_HASH_MASK (SESSION_MEDIA_HASH_CAP - 1u)
#define SESSION_MEDIA_IDX_END 0xFFFFu
// Decoded textures live in the renderer; these are the compressed bodies kept so a texture evicted there can be
// re-uploaded without a round trip. Bounded by bytes as well as by count now that one body can be a 32 MiB video.
#if CCORD_OS_ANDROID
#define SESSION_MEDIA_BYTE_BUDGET ((size_t)16u * 1024u * 1024u)
#else
#define SESSION_MEDIA_BYTE_BUDGET ((size_t)32u * 1024u * 1024u)
#endif
#define SESSION_MEDIA_FAILED_MAX 256u
#define SESSION_PROFILE_CACHE_MAX 64u
#define SESSION_VERIFICATION_CACHE_MAX 32u
#define SESSION_ACTIVITY_SHELF_CACHE_MAX 32u
#define SESSION_GUILD_PROFILE_CACHE_MAX 64u
// Largest Go Live access unit the session will accept from the transport into its reusable receive scratch.
// Matches the transport media plane (256 KiB per Annex-B access unit).
#define SESSION_STREAM_FRAME_MAX (256u * 1024u)

static_assert((SESSION_MEDIA_HASH_CAP & (SESSION_MEDIA_HASH_CAP - 1u)) == 0u,
              "media hash table capacity must be a power of two");
static_assert(SESSION_MEDIA_HASH_CAP >= SESSION_MAX_MEDIA * 2u,
              "media hash table capacity must keep load factor <= 0.5");
static_assert(SESSION_MAX_MEDIA < SESSION_MEDIA_IDX_END,
              "media 1-based slot index must fit in uint16_t without colliding with END");

typedef struct session_media_entry {
  char *url;
  char *cache_path;
  uint8_t *data;
  size_t len;
  ccord_media_item pub;
  uint64_t access_tick;
  uint32_t url_hash;      // mixed 32-bit FNV-1a URL hash for O(1) lookup & backward-shift removal
  uint16_t unread_prev;   // 0 = read (not queued), SESSION_MEDIA_IDX_END = head, else (idx + 1)
  uint16_t unread_next;   // 0 = read, SESSION_MEDIA_IDX_END = tail, else (idx + 1)
} session_media_entry;

typedef struct session_voice_entry {
  ccord_id guild_id;
  ccord_id channel_id;
  ccord_id user_id;
} session_voice_entry;

typedef struct session_channel_hist {
  ccord_id channel_id;
  ccord_id in_flight_before_id;
  bool has_more;
} session_channel_hist;

typedef struct session_slash_cache {
  ccord_id guild_id;
  uint64_t sent_ms;
  uint64_t fetched_ms;
  bool in_flight;
  bool loaded;
} session_slash_cache;

typedef struct session_profile_cache {
  ccord_id user_id;
  ccord_id guild_id;
  uint64_t sent_ms;
  uint64_t fetched_ms;
  uint64_t last_use;
  bool in_flight;
  bool loaded;
} session_profile_cache;

#define SESSION_INVITE_CACHE_SLOTS 32u
#define SESSION_INVITE_TTL_MS 60000u    // a preview is reused this long before it is fetched again
#define SESSION_INVITE_WAIT_MS 15000u   // an unanswered preview blocks an identical request this long
typedef struct session_invite_cache {
  bool valid;
  bool failed;
  char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
  uint64_t sent_ms;      // when the last request went out (0 = not waiting for an answer)
  uint64_t fetched_ms;   // when the last answer arrived (0 = never fetched)
  ccord_invite_preview preview;
} session_invite_cache;

// Membership screening: the rules form is session-owned per guild, because the store's admin cache is dropped when
// Server Settings closes (store_admin.c) while the form must survive until the member passes screening. The strings
// live in the entry's arena; a new GET answer replaces the whole form.
typedef struct session_verification_cache {
  ccord_id guild_id;
  uint64_t last_use;
  ccord_arena arena;
  ccord_member_verification form;
} session_verification_cache;

#define SESSION_DISCOVERY_CACHE_SLOTS 8u
typedef struct session_discovery_cache_entry {
  bool valid;
  uint64_t fetched_ms;
  char query[104];
  uint32_t category_id;
  uint32_t offset;
  uint32_t total;
  size_t count;
  ccord_discover_guild items[CCORD_DISCOVERY_PAGE_MAX];
} session_discovery_cache_entry;

// Embedded activities: the guild's shelf (GET /activities/shelf) is session-owned, one entry per guild.
// The running instances live in the store (per voice channel), fed by ACTIVITY_INSTANCE_UPDATE.
#define SESSION_ACTIVITY_SHELF_MAX 32u      // apps kept from one shelf answer
#define SESSION_ACTIVITY_SHELF_WAIT_MS 15000u   // an unanswered shelf request blocks an identical one this long
#define SESSION_ACTIVITY_SHELF_TTL_MS 60000u    // an answered shelf is reused this long before the next fetch
// ccord_admin_activity_launch wants a non-empty gateway session id, which only the live transport knows (the
// session layer never sees it). The transport replaces this stand-in with its own READY session id when it sends.
#define SESSION_ACTIVITY_SESSION_PLACEHOLDER "ccord-gateway-session"
typedef struct session_activity_shelf {
  ccord_id guild_id;
  bool loading;
  uint64_t sent_ms;
  uint64_t fetched_ms;   // when the last answer arrived (0 = never)
  uint64_t last_use;
  size_t count;
  ccord_activity_app apps[SESSION_ACTIVITY_SHELF_MAX];
} session_activity_shelf;

typedef struct session_guild_profile_cache {
  ccord_id guild_id;
  bool loading;
  bool has_profile;
  uint64_t sent_ms;
  uint64_t fetched_ms;
  uint64_t last_use;
  ccord_guild_profile profile;
} session_guild_profile_cache;

// A favourite-GIF toggle sent to Discord whose settings answer has not been seen yet. A settings answer is authoritative
// for the favourite list, but one that was fetched before our PATCH landed does not know about the toggle; without this
// record it would undo the toggle locally and the next PATCH (built from the store) would push the undo to Discord.
#define SESSION_FAV_PENDING_MAX 16u
#define SESSION_FAV_PENDING_TTL_MS 60000ull   // a toggle the server never confirms stops shielding the store after this
typedef struct session_fav_pending {
  char *url;
  bool starred;        // the local change: true = starred (must survive a list without it), false = unstarred (must not come back)
  uint64_t at_ms;
} session_fav_pending;

typedef struct session_media_cache {
  session_media_entry media[SESSION_MAX_MEDIA];
  size_t media_count;
  uint64_t media_tick;
  uint16_t media_hash[SESSION_MEDIA_HASH_CAP];        // 0 = empty, else (media[] index + 1)
  uint16_t media_unread_head;                         // 0 = empty, else (media[] index + 1)
  uint16_t media_unread_tail;                         // 0 = empty, else (media[] index + 1)
  size_t media_unread_count;
  char *media_requested[SESSION_MAX_MEDIA];
  uint32_t media_req_hashes[SESSION_MAX_MEDIA];       // mixed 32-bit FNV-1a hash parallel to media_requested[]
  uint16_t media_req_hash[SESSION_MEDIA_HASH_CAP];    // 0 = empty, else (media_requested[] index + 1)
  size_t media_req_count;
  size_t media_bytes;                                  // sum of media[i].len
  uint64_t media_failed[SESSION_MEDIA_FAILED_MAX];     // FNV-1a of URLs whose last download failed (ring)
  size_t media_failed_head, media_failed_count;
} session_media_cache;

struct ccord_session {
  ccord_mutex *audio_mtx;
  ccord_stream_hub *streams;   // chunked video streams (media_stream.c); lives with the session
  ccord_video_resolved *video_resolved;   // lazy 8-slot ring of third-party video resolutions
  size_t video_resolved_head, video_resolved_count;
  uint64_t next_video_resolve_id;
  ccord_bus *bus;
  ccord_store *store;
  ccord_transport *transport;
  ccord_profile_rest *profile_rest;   // own-profile edits (profile_edit.c); null until attached
  ccord_owned_collectible *collectibles;
  size_t collectible_count;
  ccord_fetch_state collectibles_state;
  uint64_t collectibles_fetched_ms;
  uint64_t collectibles_gen;
  char collectibles_error[160];
  ccord_account_rest *account_rest;   // own-account edits (account_edit.c); null until attached
  ccord_account_private account_private; // RAM-only private self-account metadata (never in store)
  ccord_account_standing account_standing;
  ccord_webauthn_cred webauthn_creds[CCORD_WEBAUTHN_MAX_CREDS];
  size_t webauthn_cred_count;
  bool webauthn_loaded;
  uint64_t account_revision;
  ccord_connection_state state;
  bool stale;               // store seeded from the on-disk cache, READY not yet applied (see ccord_session_set_stale)
  // Background requests that arrived while stale: replayed once when READY clears it.
  struct { ccord_expr_kind kind; char arg[96]; } stale_expr[8];
  size_t nstale_expr;
  ccord_id stale_slash_guild, stale_slash_channel;
  bool stale_has_voice_cfg;
  ccord_voice_config stale_voice_cfg;
  bool stale_has_rich;      // the latest rich presence refused while stale (stale_rich null = clear)
  char *stale_rich;
  // Our own recent status writes (settings-proto/1). Discord echoes each write back as a USER_SETTINGS_PROTO_UPDATE; with
  // two quick changes the first echo would otherwise put the older status back until the second echo arrives.
  struct { bool used; ccord_presence presence; char emoji[CCORD_EMOJI_MAX_BYTES + 1]; char text[129]; uint64_t at_ms; } status_echo[4];
  size_t status_echo_next;
  session_fav_pending fav_pending[SESSION_FAV_PENDING_MAX];   // local GIF star/unstar not yet confirmed by a settings answer
  size_t nfav_pending;
  ccord_id *requested;      // channels whose initial history request is currently in-flight
  size_t nrequested, caprequested;
  session_channel_hist *chan_hist;
  size_t nchan_hist, capchan_hist;
  session_profile_cache *profile_cache;
  size_t nprofile_req, capprofile_req;
  ccord_id retried_channel; // channel whose transient history error was auto-retried once
  ccord_notification notifications[SESSION_MAX_NOTIFICATIONS];
  size_t notif_head, notif_count;
  ccord_action_failure failures[SESSION_MAX_FAILURES];
  size_t fail_head, fail_count;
  ccord_report_event report_events[SESSION_MAX_REPORT_EVENTS];
  size_t report_head, report_count;
  ccord_interaction_modal *modals;   // lazily allocated SESSION_MAX_MODALS ring
  size_t modal_head, modal_count;
  uint64_t cache_tick;               // monotonic LRU tick for bounded session caches
  session_slash_cache *slash_cache;
  size_t nslash_cache, capslash_cache;
  session_activity_shelf *activity_shelves;
  size_t nactivity_shelves, capactivity_shelves;
  session_guild_profile_cache *guild_profiles;
  size_t nguild_profiles, capguild_profiles;
  ccord_slash_choice slash_choices[CCORD_SLASH_MAX_CHOICES];
  size_t slash_choice_count;
  uint64_t slash_choice_seq;
  session_media_cache *mc;                             // heap-allocated media cache (~220 KiB off ccord_session)
  session_voice_entry *voice_track;                    // mirrors active voice participants for O(V) guild replace
  size_t nvoice_track, capvoice_track;
  bool voice_track_synced;
  ccord_message *search_results;
  ccord_message_rich **search_rich;   // owned copies of the results' rich blocks (index-parallel)
  size_t search_count;
  uint32_t search_total_results;
  uint32_t search_offset;
  bool search_loading;
  ccord_arena search_arena;
  ccord_id pins_in_flight[16];
  size_t npins_in_flight;
  ccord_id react_users_channel_id;
  ccord_id react_users_message_id;
  char react_users_emoji[CCORD_EMOJI_MAX_BYTES + 1];
  ccord_user *react_users;
  size_t react_user_count;
  bool react_users_loading;
  ccord_arena react_users_arena;
  ccord_watched_stream watched;
  _Atomic bool watched_active;
  ccord_id unsub_channel_id;   // stream switched away from (one-shot viewer-unsubscribe STREAM_DELETE guard)
  ccord_id unsub_user_id;
  ccord_stream_config pending_stream_cfg;
  uint8_t *cam_frame_buf;      // lazy CCORD_CAM_FRAME_MAX remote-camera receive scratch, owned by the session
  uint8_t *stream_frame_buf;   // lazy SESSION_STREAM_FRAME_MAX Go Live receive scratch, owned by the session
  ccord_id reading_channel;  // set_reading: the channel being read right now (0 = none)
  bool reading_known;        // set_reading was called; until then the open channel counts as being read
  // Paging through the open channel's member list (ccord_session_more_members): which list, how many hundreds are asked
  // for, whether an answer is awaited, and when the last request went out.
  ccord_id paging_guild, paging_channel;
  uint32_t paging_chunks;
  bool paging_wait, paging_done;
  uint64_t paging_sent_ms;
  size_t paging_held;        // members held when the last request went out
  ccord_invite_preview invite_preview;
  bool has_invite_preview;
  bool invite_preview_loading;
  session_invite_cache *invite_cache;   // lazily allocated SESSION_INVITE_CACHE_SLOTS table
  size_t invite_cache_rr;
  session_verification_cache *verification_cache;
  size_t nverification_cache, capverification_cache;
  ccord_id pending_join_guild_id;
  ccord_id pending_join_channel_id;
  ccord_discover_guild *discovery_items;   // lazily allocated CCORD_DISCOVERY_PAGE_MAX array
  size_t discovery_count;
  uint32_t discovery_total;
  uint32_t discovery_offset;
  uint32_t discovery_category_id;
  char discovery_query[104];
  bool discovery_loading;
  uint64_t discovery_sent_ms;
  session_discovery_cache_entry *discovery_cache;   // lazily allocated SESSION_DISCOVERY_CACHE_SLOTS table
  size_t discovery_cache_rr;
  bool has_pending_presence;
  ccord_presence pending_presence;
  // Rich Presence (RPC SET_ACTIVITY): the activities array to splice into every presence update. Session-owned
  // so it survives reconnects; re-pushed to the transport on SNAPSHOT. nullptr = none.
  char *rich_json;
  size_t rich_len;
  bool self_activity_from_rich;
  bool rich_op3_dirty;
  uint64_t rich_op3_retry_ms;   // backoff deadline after a failed rich op 3 push (prevents 0 ms idle spin)
  bool in_background;
  bool suspended;
  // M5 local voice state: the last accepted input-processing config and the per-frame push-to-talk key.
  // Neither is Discord state: they gate the mic path only and never produce traffic.
  ccord_voice_config voice_cfg;
  _Atomic bool has_voice_cfg;
  _Atomic bool ptt_held;
  bool voice_self_mute;
  bool voice_self_deaf;
  ccord_id pending_voice_guild;
  ccord_id pending_voice_channel;
  // M6 account lists. Session-owned by-value rows: they are replaced only by a
  // transport answer (REST 200 / logout 204), never by the click that asked for it.
  ccord_auth_session auth_sessions[CCORD_SESSION_MAX_AUTH_SESSIONS];
  size_t auth_session_count;
  ccord_session_raw_gateway_cb raw_gw_cb;
  void *raw_gw_ud;
  ccord_fetch_state auth_sessions_state;
  uint64_t auth_sessions_sent_ms;     // when the in-flight request went out (0 = none)
  uint64_t auth_sessions_fetched_ms;  // when the last answer arrived (0 = never)
  ccord_connection connections[CCORD_SESSION_MAX_CONNECTIONS];
  size_t connection_count;
  ccord_fetch_state connections_state;
  uint64_t connections_sent_ms;
  uint64_t connections_fetched_ms;
  // M0-3 local echoes: the id counter behind (1ULL<<63)|seq and the nonce uniqueness counter.
  uint64_t local_msg_seq;
  uint64_t nonce_seq;
};

#define SESSION_MEMBER_PAGE_GAP_MS 1000u     // at least this long between two requests for more of a member list
#define SESSION_MEMBER_PAGE_WAIT_MS 8000u    // an answer is awaited this long before another request goes out
#define SESSION_SLASH_TTL_MS 300000u         // slash command index is kept for 5 minutes per guild/DM
#define SESSION_SLASH_RETRY_MS 5000u         // minimum gap between slash command index requests on failure
#define SESSION_SLASH_WAIT_MS 15000u         // wait for an in-flight slash command index response


typedef struct session_local_echo {
  ccord_id id;
  ccord_id author_id;
  int64_t timestamp_ms;
  uint8_t send_state;
  char nonce[32];
  char text[CCORD_SEND_MAX_BYTES + 1];
} session_local_echo;


// Cross-TU internal helpers
static inline ccord_status session_stale_guard(const ccord_session *s, ccord_error *err)
{
  if (s && s->stale) return ccord_error_set(err, CCORD_ERR_INVALID, "Reconnecting - sending is available in a moment");
  return CCORD_OK;
}

#define SESSION_STALE_GUARD(s, err)                                  \
  do {                                                               \
    ccord_status stale_guard_st_ = session_stale_guard((s), (err));  \
    if (stale_guard_st_ != CCORD_OK) return stale_guard_st_;         \
  } while (0)

session_slash_cache *sess_slash_cache_for(ccord_session *s, ccord_id guild_id);
session_activity_shelf *sess_activity_shelf_for(ccord_session *s, ccord_id guild_id);
int64_t session_now_unix_ms(const ccord_session *s);
ccord_status sess_check_thread_lock(const ccord_session *s, const ccord_channel *ch, ccord_error *err);
void sess_reset_account_lists(ccord_session *s);
void sess_clear_requested(ccord_session *s, ccord_id id);
session_channel_hist *sess_find_chan_hist(const ccord_session *s, ccord_id channel_id);
session_channel_hist *sess_chan_hist_for(ccord_session *s, ccord_id channel_id);
session_profile_cache *sess_find_profile_cache(const ccord_session *s, ccord_id user_id);
session_profile_cache *sess_profile_cache_for(ccord_session *s, ccord_id user_id);
void sess_mark_profile_loaded(ccord_session *s, ccord_id user_id);
void sess_clear_profile_requested(ccord_session *s, ccord_id user_id);
session_invite_cache *sess_find_invite_cache(ccord_session *s, const char *code);
session_invite_cache *sess_invite_cache_for(ccord_session *s, const char *code);
void sess_reset_invite_cache(ccord_session *s);
void sess_reset_verification_cache(ccord_session *s);
session_verification_cache *sess_verification_cache_for(ccord_session *s, ccord_id guild_id);
bool sess_cache_verification_form(session_verification_cache *ent, const ccord_member_verification *form);
bool session_is_authenticated(const ccord_session *s);
void sess_maybe_request_dm_profile(ccord_session *s, ccord_id channel_id);
void sess_request_history_once(ccord_session *s, ccord_id channel_id);
void sess_post_state(ccord_session *s);
bool sess_message_pings_own_role(const ccord_session *s, ccord_id guild_id, const ccord_message_rich *rich);
void sess_queue_notification(ccord_session *s, ccord_notification n);
void sess_store_media_loaded(ccord_session *s, const ccord_net_event *ev);
void sess_clear_search_results(ccord_session *s);
void sess_clear_reaction_users(ccord_session *s);
void sess_store_reaction_users(ccord_session *s, const ccord_net_event *ev);
void sess_pins_clear_in_flight(ccord_session *s, ccord_id channel_id);
void sess_store_search_results(ccord_session *s, const ccord_net_event *ev);
void sess_drop_auth_sessions(ccord_session *s, const char hashes[][64], size_t count);
void sess_upsert_connection(ccord_session *s, const ccord_connection *c);
void sess_drop_connection(ccord_session *s, const char *name, const char *id);
bool session_is_local_echo_id(ccord_id id);
bool sess_find_local_echo_by_nonce(const ccord_session *s, ccord_id channel_id, const char *nonce, session_local_echo *out);
void sess_remove_local_echo(ccord_session *s, ccord_id channel_id, ccord_id echo_id);
bool sess_set_local_echo_state(ccord_session *s, ccord_id channel_id, const session_local_echo *e, uint8_t state);
void sess_cache_settings_proto(ccord_session *s, const ccord_net_event *ev);
void sess_apply_favorite_gifs_wire(ccord_session *s, const ccord_user_expressions *ux, bool authoritative);
void session_apply_frecency_update(ccord_session *s, const ccord_net_event *ev);
void session_leave_channel_activities(ccord_session *s, ccord_id channel_id);
session_guild_profile_cache *sess_guild_profile_for(ccord_session *s, ccord_id guild_id);
void session_set_self_activity(ccord_session *s, bool has, ccord_activity_type type, const char *name, const char *details, const char *state, int64_t start_ms, int64_t end_ms);
void sess_voice_track_upsert(ccord_session *s, ccord_id guild_id, ccord_id channel_id, ccord_id user_id);
void sess_voice_track_remove_guild(ccord_session *s, ccord_id guild_id);
void sess_voice_track_remove_channel(ccord_session *s, ccord_id channel_id);
void session_apply_voice_state(ccord_session *s, const ccord_voice_state *raw_vs, const ccord_user *opt_user);
void session_apply_voice_states(ccord_session *s, ccord_id replace_guild_id, const ccord_voice_state *states, size_t count, const ccord_user *users, size_t user_count);
void sess_apply_event(ccord_session *s, const ccord_net_event *ev);
void sess_apply_event_ext(ccord_session *s, const ccord_net_event *ev);
void sess_queue_report_event(ccord_session *s, const ccord_net_event *ev);
void sess_record_failure(ccord_session *s, const ccord_net_event *ev);
bool session_channel_has_chat(const ccord_channel *ch);
bool sess_blank(const char *s, size_t len);
ccord_status session_screening_gate(const ccord_session *s, const ccord_channel *ch, ccord_error *err);
ccord_status sess_validate_text(const char *utf8, size_t len, ccord_error *err);
const ccord_voice_state *sess_self_voice(const ccord_session *s, ccord_id guild_id);
ccord_presence session_current_presence(const ccord_session *s);
ccord_status session_send_presence_with_custom(ccord_session *s, ccord_presence presence, ccord_error *err);
void session_apply_status_proto(ccord_session *s, const ccord_net_event *ev);
void session_repush_rich_presence(ccord_session *s);
void session_replay_stale(ccord_session *s);
void sess_account_wipe_vault(ccord_session *s);
void sess_account_apply_private(ccord_session *s, const ccord_account_private *priv);
