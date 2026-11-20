// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/session/session.h>
#include <ccord/session/media_stream.h>

#include "media_stream_hub.h"
#include <ccord/store/admin.h>
#include <ccord/store/notify.h>
#include <ccord/store/roles.h>
#include <ccord/store/unread.h>

#include <ccord/core/os.h>
#include <stdatomic.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/store/listsize.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>
#include <ccord/model/rich.h>
#include <ccord/net/discord_proto.h>
#include <ccord/net/live_transport.h>   // ccord_voice_connection_state (rtc.state values)
#include <ccord/net/profile_edit.h>
#include <ccord/store/expressions.h>

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

struct ccord_session {
  ccord_mutex *audio_mtx;
  ccord_stream_hub *streams;   // chunked video streams (media_stream.c); lives with the session
  ccord_bus *bus;
  ccord_store *store;
  ccord_transport *transport;
  ccord_profile_rest *profile_rest;   // own-profile edits (profile_edit.c); null until attached
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

static session_slash_cache *slash_cache_for(ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nslash_cache; i++) {
    if (s->slash_cache[i].guild_id == guild_id) return &s->slash_cache[i];
  }
  if (s->nslash_cache == s->capslash_cache) {
    size_t ncap = s->capslash_cache ? s->capslash_cache * 2u : 8u;
    session_slash_cache *g = ccord_realloc_array(s->slash_cache, ncap, sizeof *g);
    if (!g) return nullptr;
    s->slash_cache = g;
    s->capslash_cache = ncap;
  }
  session_slash_cache *ent = &s->slash_cache[s->nslash_cache++];
  memset(ent, 0, sizeof *ent);
  ent->guild_id = guild_id;
  return ent;
}

static session_activity_shelf *activity_shelf_for(ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nactivity_shelves; i++) {
    if (s->activity_shelves[i].guild_id == guild_id) {
      s->activity_shelves[i].last_use = ++s->cache_tick;
      return &s->activity_shelves[i];
    }
  }
  session_activity_shelf *ent;
  if (s->nactivity_shelves >= SESSION_ACTIVITY_SHELF_CACHE_MAX) {
    size_t victim = SIZE_MAX;
    for (size_t i = 0; i < s->nactivity_shelves; i++) {
      if (s->activity_shelves[i].loading) continue;
      if (victim == SIZE_MAX || s->activity_shelves[i].last_use < s->activity_shelves[victim].last_use) victim = i;
    }
    if (victim == SIZE_MAX) {
      victim = 0;
      for (size_t i = 1; i < s->nactivity_shelves; i++) {
        if (s->activity_shelves[i].last_use < s->activity_shelves[victim].last_use) victim = i;
      }
    }
    ent = &s->activity_shelves[victim];
  } else {
    if (s->nactivity_shelves == s->capactivity_shelves) {
      size_t ncap = s->capactivity_shelves ? s->capactivity_shelves * 2u : 4u;
      if (ncap > SESSION_ACTIVITY_SHELF_CACHE_MAX) ncap = SESSION_ACTIVITY_SHELF_CACHE_MAX;
      session_activity_shelf *g = ccord_realloc_array(s->activity_shelves, ncap, sizeof *g);
      if (!g) return nullptr;
      s->activity_shelves = g;
      s->capactivity_shelves = ncap;
    }
    ent = &s->activity_shelves[s->nactivity_shelves++];
  }
  memset(ent, 0, sizeof *ent);
  ent->guild_id = guild_id;
  ent->last_use = ++s->cache_tick;
  return ent;
}

static void reset_invite_cache(ccord_session *s);
static void session_repush_rich_presence(ccord_session *s);
static void session_replay_stale(ccord_session *s);
static void session_apply_status_proto(ccord_session *s, const ccord_net_event *ev);
static void session_apply_frecency_update(ccord_session *s, const ccord_net_event *ev);
static void reset_verification_cache(ccord_session *s);

static int64_t session_now_unix_ms(const ccord_session *s)
{
  (void)s;
  return ccord_clock_unix_ms();
}

static ccord_status check_thread_lock(const ccord_session *s, const ccord_channel *ch, ccord_error *err)
{
  if (ch && ccord_discord_type_is_thread(ch->discord_type) && ch->thread_locked) {
    ccord_id self = ccord_store_self_id(s->store);
    if (!ccord_store_permissions_exact(s->store, ch->guild_id, self) ||
        (ccord_store_permissions(s->store, ch->guild_id, ch->id, self, session_now_unix_ms(s)) & CCORD_PERM_MANAGE_THREADS) == 0) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION, "thread is locked");
    }
  }
  return CCORD_OK;
}

// A new transport or a new READY belongs to another connection (and possibly another account): drop the
// account lists and their fetch state. The store drops its cached settings on SNAPSHOT itself.
static void reset_account_lists(ccord_session *s)
{
  if (!s) return;
  s->auth_session_count = 0;
  s->auth_sessions_state = CCORD_FETCH_IDLE;
  s->auth_sessions_sent_ms = 0;
  s->auth_sessions_fetched_ms = 0;
  s->connection_count = 0;
  s->connections_state = CCORD_FETCH_IDLE;
  s->connections_sent_ms = 0;
  s->connections_fetched_ms = 0;
}

ccord_session *ccord_session_create(ccord_bus *bus, ccord_store *store, ccord_transport *transport, ccord_error *err)
{
  ccord_error_clear(err);
  if (!store || !transport) {
    ccord_transport_destroy(transport);
    ccord_error_set(err, CCORD_ERR_INVALID, "session: store and transport are required");
    return nullptr;
  }
  ccord_session *s = ccord_calloc(1, sizeof *s);
  if (!s) {
    ccord_transport_destroy(transport);
    ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
    return nullptr;
  }
  s->streams = ccord_stream_hub_create(transport);
  if (!s->streams) {
    ccord_free(s);
    ccord_transport_destroy(transport);
    ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
    return nullptr;
  }
  s->bus = bus;
  s->store = store;
  s->transport = transport;
  s->audio_mtx = ccord_mutex_create();
  s->state = CCORD_CONN_DISCONNECTED;
  s->voice_track_synced = (ccord_store_guild_count(store) == 0u &&
                           ccord_store_channel_count(store, CCORD_DM_GUILD_ID) == 0u);
  return s;
}

static void clear_search_results(ccord_session *s);
static void clear_reaction_users(ccord_session *s);

void ccord_session_destroy(ccord_session *s)
{
  if (!s) return;
  ccord_stream_hub_destroy(s->streams);   // before the transport: closes the streams that read through it
  ccord_mutex_lock(s->audio_mtx);
  ccord_transport_destroy(s->transport);
  s->transport = nullptr;
  ccord_mutex_unlock(s->audio_mtx);
  ccord_mutex_destroy(s->audio_mtx);
  ccord_profile_rest_destroy(s->profile_rest);   // pending edits answer CCORD_ERR_IO without touching the store
  for (size_t i = 0; i < s->nfav_pending; i++) ccord_free(s->fav_pending[i].url);
  ccord_free(s->requested);
  ccord_free(s->chan_hist);
  ccord_free(s->profile_cache);
  ccord_free(s->modals);
  ccord_free(s->slash_cache);
  ccord_free(s->activity_shelves);
  ccord_free(s->guild_profiles);
  ccord_free(s->voice_track);
  ccord_free(s->invite_cache);
  ccord_free(s->discovery_items);
  ccord_free(s->discovery_cache);
  ccord_free(s->stream_frame_buf);
  ccord_free(s->cam_frame_buf);
  ccord_free(s->rich_json);
  ccord_free(s->stale_rich);
  for (size_t i = 0; i < s->media_count; i++) {
    ccord_free(s->media[i].url);
    ccord_free(s->media[i].cache_path);
    ccord_free(s->media[i].data);
  }
  for (size_t i = 0; i < s->media_req_count; i++) {
    ccord_free(s->media_requested[i]);
  }
  for (size_t i = 0; i < s->fail_count; i++) {
    ccord_free(s->failures[(s->fail_head + i) % SESSION_MAX_FAILURES].draft);
  }
  for (size_t i = 0; i < s->report_count; i++) {
    ccord_report_event_free(&s->report_events[(s->report_head + i) % SESSION_MAX_REPORT_EVENTS]);
  }
  clear_search_results(s);
  clear_reaction_users(s);
  reset_verification_cache(s);
  ccord_free(s->verification_cache);
  ccord_free(s);
}

ccord_status ccord_session_start(ccord_session *s, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  return ccord_transport_connect(s->transport, err);
}

void ccord_session_set_stale(ccord_session *s, bool stale)
{
  if (s) s->stale = stale;
}

ccord_status ccord_session_link_event(ccord_session *s, ccord_link_event kind, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  return ccord_transport_network_event(s->transport, kind, err);
}

bool ccord_session_is_stale(const ccord_session *s)
{
  return s && s->stale;
}

ccord_status ccord_session_set_transport(ccord_session *s, ccord_transport *transport, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !transport) {
    ccord_transport_destroy(transport);
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session or transport");
  }
  ccord_mutex_lock(s->audio_mtx);
  ccord_transport_destroy(s->transport);
  s->transport = transport;
  ccord_mutex_unlock(s->audio_mtx);
  ccord_stream_hub_set_transport(s->streams, transport);
  s->nrequested = 0;
  s->nchan_hist = 0;
  s->nprofile_req = 0;
  s->nslash_cache = 0;
  s->nactivity_shelves = 0;
  s->nguild_profiles = 0;
  s->slash_choice_count = 0;
  s->npins_in_flight = 0;
  s->notif_head = 0;
  s->notif_count = 0;
  clear_search_results(s);
  clear_reaction_users(s);
  s->has_invite_preview = false;
  s->invite_preview_loading = false;
  reset_invite_cache(s);
  reset_verification_cache(s);
  s->pending_join_guild_id = CCORD_ID_NONE;
  s->pending_join_channel_id = CCORD_ID_NONE;
  s->discovery_count = 0;
  s->discovery_total = 0;
  s->discovery_loading = false;
  if (s->discovery_cache) {
    memset(s->discovery_cache, 0, SESSION_DISCOVERY_CACHE_SLOTS * sizeof *s->discovery_cache);
  }
  s->discovery_cache_rr = 0;
  ccord_mutex_lock(s->audio_mtx);
  atomic_store_explicit(&s->has_voice_cfg, false, memory_order_release);
  ccord_mutex_unlock(s->audio_mtx);
  s->ptt_held = false;
  reset_account_lists(s);
  s->state = CCORD_CONN_DISCONNECTED;
  return ccord_transport_connect(s->transport, err);
}

ccord_connection_state ccord_session_state(const ccord_session *s)
{
  return s ? s->state : CCORD_CONN_DISCONNECTED;
}

static bool was_requested(const ccord_session *s, ccord_id id)
{
  for (size_t i = 0; i < s->nrequested; i++) {
    if (s->requested[i] == id) return true;
  }
  return false;
}

static void clear_requested(ccord_session *s, ccord_id id)
{
  if (!s || id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < s->nrequested; ) {
    if (s->requested[i] == id) {
      memmove(&s->requested[i], &s->requested[i + 1], (s->nrequested - i - 1) * sizeof *s->requested);
      s->nrequested--;
    } else {
      i++;
    }
  }
}

static session_channel_hist *find_chan_hist(const ccord_session *s, ccord_id channel_id)
{
  if (!s || channel_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nchan_hist; i++) {
    if (s->chan_hist[i].channel_id == channel_id) return &s->chan_hist[i];
  }
  return nullptr;
}

static session_channel_hist *chan_hist_for(ccord_session *s, ccord_id channel_id)
{
  session_channel_hist *e = find_chan_hist(s, channel_id);
  if (e) return e;
  if (s->nchan_hist == s->capchan_hist) {
    size_t ncap = s->capchan_hist ? s->capchan_hist * 2 : 32;
    session_channel_hist *g = ccord_realloc_array(s->chan_hist, ncap, sizeof *g);
    if (!g) return nullptr;
    s->chan_hist = g;
    s->capchan_hist = ncap;
  }
  e = &s->chan_hist[s->nchan_hist++];
  *e = (session_channel_hist){
    .channel_id = channel_id,
    .in_flight_before_id = CCORD_ID_NONE,
    .has_more = true,
  };
  return e;
}

static session_profile_cache *find_profile_cache(const ccord_session *s, ccord_id user_id)
{
  if (!s || user_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nprofile_req; i++) {
    if (s->profile_cache[i].user_id == user_id) return &s->profile_cache[i];
  }
  return nullptr;
}

static session_profile_cache *profile_cache_for(ccord_session *s, ccord_id user_id)
{
  session_profile_cache *pe = find_profile_cache(s, user_id);
  if (pe) {
    pe->last_use = ++s->cache_tick;
    return pe;
  }
  if (!s || user_id == CCORD_ID_NONE) return nullptr;
  if (s->nprofile_req >= SESSION_PROFILE_CACHE_MAX) {
    size_t victim = SIZE_MAX;
    for (size_t i = 0; i < s->nprofile_req; i++) {
      if (s->profile_cache[i].in_flight) continue;
      if (victim == SIZE_MAX || s->profile_cache[i].last_use < s->profile_cache[victim].last_use) victim = i;
    }
    if (victim == SIZE_MAX) {
      victim = 0;
      for (size_t i = 1; i < s->nprofile_req; i++) {
        if (s->profile_cache[i].last_use < s->profile_cache[victim].last_use) victim = i;
      }
    }
    pe = &s->profile_cache[victim];
  } else {
    if (s->nprofile_req == s->capprofile_req) {
      size_t ncap = s->capprofile_req ? s->capprofile_req * 2u : 16u;
      if (ncap > SESSION_PROFILE_CACHE_MAX) ncap = SESSION_PROFILE_CACHE_MAX;
      session_profile_cache *g = ccord_realloc_array(s->profile_cache, ncap, sizeof *g);
      if (!g) return nullptr;
      s->profile_cache = g;
      s->capprofile_req = ncap;
    }
    pe = &s->profile_cache[s->nprofile_req++];
  }
  memset(pe, 0, sizeof *pe);
  pe->user_id = user_id;
  pe->last_use = ++s->cache_tick;
  return pe;
}

static bool was_profile_requested(const ccord_session *s, ccord_id user_id)
{
  const session_profile_cache *pe = find_profile_cache(s, user_id);
  return pe && (pe->in_flight || pe->loaded);
}

static void mark_profile_loaded(ccord_session *s, ccord_id user_id)
{
  session_profile_cache *pe = profile_cache_for(s, user_id);
  if (!pe) return;
  pe->in_flight = false;
  pe->loaded = true;
  uint64_t fms = pe->sent_ms ? pe->sent_ms : ccord_clock_ms();
  pe->fetched_ms = fms ? fms : 1u;
}

static void clear_profile_requested(ccord_session *s, ccord_id user_id)
{
  session_profile_cache *pe = find_profile_cache(s, user_id);
  if (pe) pe->in_flight = false;
}

// Invite previews are small and immutable enough to key by code: the cache owns value copies, never store pointers.
static session_invite_cache *find_invite_cache(ccord_session *s, const char *code)
{
  if (!s || !s->invite_cache || !code || code[0] == '\0') return nullptr;
  for (size_t i = 0; i < SESSION_INVITE_CACHE_SLOTS; i++) {
    if (s->invite_cache[i].valid && strcmp(s->invite_cache[i].code, code) == 0) return &s->invite_cache[i];
  }
  return nullptr;
}

static session_invite_cache *invite_cache_for(ccord_session *s, const char *code)
{
  session_invite_cache *entry = find_invite_cache(s, code);
  if (entry) return entry;
  if (!s || !code || code[0] == '\0') return nullptr;
  if (!s->invite_cache) {
    s->invite_cache = ccord_calloc(SESSION_INVITE_CACHE_SLOTS, sizeof *s->invite_cache);
    if (!s->invite_cache) return nullptr;
  }
  entry = &s->invite_cache[s->invite_cache_rr++ % SESSION_INVITE_CACHE_SLOTS];
  memset(entry, 0, sizeof *entry);
  entry->valid = true;
  (void)snprintf(entry->code, sizeof entry->code, "%s", code);
  return entry;
}

static void invalidate_invite_cache(ccord_session *s, const char *code)
{
  session_invite_cache *entry = find_invite_cache(s, code);
  if (entry) memset(entry, 0, sizeof *entry);
}

static void reset_invite_cache(ccord_session *s)
{
  if (s->invite_cache) memset(s->invite_cache, 0, SESSION_INVITE_CACHE_SLOTS * sizeof *s->invite_cache);
  s->invite_cache_rr = 0;
}

static void reset_verification_cache(ccord_session *s)
{
  for (size_t i = 0; i < s->nverification_cache; i++) ccord_arena_free(&s->verification_cache[i].arena);
  s->nverification_cache = 0;
}

static session_verification_cache *verification_cache_for(ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nverification_cache; i++) {
    if (s->verification_cache[i].guild_id == guild_id) {
      s->verification_cache[i].last_use = ++s->cache_tick;
      return &s->verification_cache[i];
    }
  }
  session_verification_cache *ent;
  if (s->nverification_cache >= SESSION_VERIFICATION_CACHE_MAX) {
    size_t victim = 0;
    for (size_t i = 1; i < s->nverification_cache; i++) {
      if (s->verification_cache[i].last_use < s->verification_cache[victim].last_use) victim = i;
    }
    ent = &s->verification_cache[victim];
    ccord_arena_free(&ent->arena);
  } else {
    if (s->nverification_cache == s->capverification_cache) {
      size_t ncap = s->capverification_cache ? s->capverification_cache * 2u : 4u;
      if (ncap > SESSION_VERIFICATION_CACHE_MAX) ncap = SESSION_VERIFICATION_CACHE_MAX;
      session_verification_cache *g = ccord_realloc_array(s->verification_cache, ncap, sizeof *g);
      if (!g) return nullptr;
      s->verification_cache = g;
      s->capverification_cache = ncap;
    }
    ent = &s->verification_cache[s->nverification_cache++];
  }
  memset(ent, 0, sizeof *ent);
  ent->guild_id = guild_id;
  ent->last_use = ++s->cache_tick;
  return ent;
}

// Copies one form string into the arena, truncated to the model cap (the parser truncates too; this keeps a hostile
// transport answer from allocating without bound). nullptr reads as "".
static char *verification_str(ccord_arena *a, const char *src, size_t cap)
{
  if (!src) src = "";
  size_t n = strlen(src);
  if (n > cap) n = cap;
  return ccord_arena_strndup(a, src, n);
}

// Deep-copies a GET answer into the entry. The copy is built in a fresh arena first, so an out-of-memory answer
// leaves the previously cached form untouched.
static bool cache_verification_form(session_verification_cache *ent, const ccord_member_verification *form)
{
  ccord_arena fresh;
  ccord_arena_init(&fresh, 1024u);
  ccord_member_verification copy = {0};
  bool ok = true;
  copy.version = verification_str(&fresh, form->version, CCORD_MEMBER_VERIFICATION_VERSION_MAX_BYTES);
  copy.description = verification_str(&fresh, form->description, CCORD_MEMBER_VERIFICATION_DESC_MAX_BYTES);
  if (!copy.version || !copy.description) ok = false;
  size_t nf = form->field_count;
  if (nf > CCORD_MEMBER_VERIFICATION_MAX_FIELDS) nf = CCORD_MEMBER_VERIFICATION_MAX_FIELDS;
  for (size_t i = 0; ok && i < nf; i++) {
    const ccord_member_verification_field *f = &form->fields[i];
    ccord_member_verification_field *out = &copy.fields[i];
    out->field_type = f->field_type;
    out->required = f->required;
    out->label = verification_str(&fresh, f->label, CCORD_MEMBER_VERIFICATION_LABEL_MAX_BYTES);
    if (!out->label) { ok = false; break; }
    size_t nv = f->value_count;
    if (nv > CCORD_MEMBER_VERIFICATION_MAX_VALUES) nv = CCORD_MEMBER_VERIFICATION_MAX_VALUES;
    for (size_t v = 0; v < nv; v++) {
      out->values[v] = verification_str(&fresh, f->values[v], CCORD_MEMBER_VERIFICATION_VALUE_MAX_BYTES);
      if (!out->values[v]) { ok = false; break; }
    }
    out->value_count = nv;
  }
  if (!ok) {
    ccord_arena_free(&fresh);
    return false;
  }
  copy.field_count = nf;
  ccord_arena_free(&ent->arena);
  ent->arena = fresh;
  ent->form = copy;
  return true;
}

// A store seeded from disk has a self id but no live connection: nothing may be sent until READY.
static bool session_is_authenticated(const ccord_session *s)
{
  return s && !s->stale && (s->state == CCORD_CONN_CONNECTED || ccord_store_self_id(s->store) != CCORD_ID_NONE);
}

static ccord_status session_stale_guard(const ccord_session *s, ccord_error *err)
{
  if (s && s->stale) return ccord_error_set(err, CCORD_ERR_INVALID, "Reconnecting - sending is available in a moment");
  return CCORD_OK;
}

static void queue_report_event(ccord_session *s, const ccord_net_event *ev);

// First statement of an intent that reaches the transport: refused while the session is stale.
#define SESSION_STALE_GUARD(s, err)                                  \
  do {                                                               \
    ccord_status stale_guard_st_ = session_stale_guard((s), (err));  \
    if (stale_guard_st_ != CCORD_OK) return stale_guard_st_;         \
  } while (0)

static void maybe_request_dm_profile(ccord_session *s, ccord_id channel_id)
{
  if (!s || channel_id == CCORD_ID_NONE || !session_is_authenticated(s)) return;
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (ch && ch->guild_id == CCORD_DM_GUILD_ID && ch->user_id != CCORD_ID_NONE) {
    ccord_error ignored;
    (void)ccord_session_request_profile(s, ch->user_id, &ignored);
  }
}

// Asks for the channel history once, if the store does not have it yet.
static void request_history_once(ccord_session *s, ccord_id channel_id)
{
  if (channel_id == CCORD_ID_NONE || !session_is_authenticated(s)) return;
  if (ccord_store_channel_is_forum(s->store, channel_id)) {
    const ccord_forum_state *fst = ccord_store_forum_state(s->store, channel_id);
    if (!fst || (!fst->loaded && !fst->loading)) {
      const ccord_forum_meta *fmeta = ccord_store_forum_meta(s->store, channel_id);
      ccord_forum_query q = {
        .forum_id = channel_id,
        .offset = 0,
        .limit = CCORD_FORUM_PAGE_SIZE,
        .sort = fmeta ? (ccord_forum_sort)fmeta->default_sort : CCORD_FORUM_SORT_ACTIVITY,
      };
      ccord_error ferr;
      (void)ccord_session_request_forum_posts(s, &q, &ferr);
    }
    return;
  }
  if ((ccord_store_history_loaded(s->store, channel_id) && !ccord_store_history_stale(s->store, channel_id)) ||
      was_requested(s, channel_id)) {
    return;
  }
  if (s->nrequested == s->caprequested) {
    size_t ncap = s->caprequested ? s->caprequested * 2 : 32;
    ccord_id *n = ccord_realloc_array(s->requested, ncap, sizeof *n);
    if (!n) {
      CCORD_LOG_W("session: out of memory while tracking history requests");
      return;
    }
    s->requested = n;
    s->caprequested = ncap;
  }
  ccord_error err;
  ccord_status st = ccord_transport_request_history(s->transport, channel_id, CCORD_SESSION_HISTORY_LIMIT, &err);
  if (st != CCORD_OK) {
    CCORD_LOG_W("session: history request failed: %s", err.message);
    return;
  }
  s->requested[s->nrequested++] = channel_id;
}

static void post_state(ccord_session *s)
{
  if (!s->bus) return;
  ccord_event ev = {.type = CCORD_EVT_CONNECTION_STATE, .value = (int64_t)s->state};
  if (ccord_bus_post(s->bus, ev) != CCORD_OK) CCORD_LOG_W("session: connection.state event dropped");
}

// Whether the message pings one of the roles the user holds. Unknown roles (the member list is lazy) answer false: a
// ping is only claimed when it is certain.
static bool message_pings_own_role(const ccord_session *s, ccord_id guild_id, const ccord_message_rich *rich)
{
  if (!rich || rich->mention_role_count == 0 || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return false;
  const ccord_id *roles = nullptr;
  size_t n = 0;
  if (!ccord_store_member_roles(s->store, guild_id, ccord_store_self_id(s->store), &roles, &n)) return false;
  for (size_t i = 0; i < n; i++) {
    if (ccord_rich_mentions_role(rich, roles[i])) return true;
  }
  return false;
}

static void queue_notification(ccord_session *s, ccord_notification n)
{
  if (s->notif_count == SESSION_MAX_NOTIFICATIONS) {
    s->notif_head = (s->notif_head + 1u) % SESSION_MAX_NOTIFICATIONS;
    s->notif_count--;
  }
  size_t idx = (s->notif_head + s->notif_count) % SESSION_MAX_NOTIFICATIONS;
  s->notifications[idx] = n;
  s->notif_count++;
}

static uint64_t media_url_hash(const char *url)
{
  uint64_t h = 0xcbf29ce484222325ULL;
  for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
    h ^= (uint64_t)*p;
    h *= 0x100000001b3ULL;
  }
  return h ? h : 1u;
}

static uint32_t media_hash_mix(uint64_t h)
{
  return (uint32_t)(h ^ (h >> 32));
}

static bool media_is_unread(const session_media_entry *e)
{
  return e->unread_prev != 0u;
}

static void media_unread_push(ccord_session *s, size_t idx)
{
  session_media_entry *e = &s->media[idx];
  if (media_is_unread(e)) return;
  uint16_t slot = (uint16_t)(idx + 1u);
  if (s->media_unread_tail == 0u) {
    e->unread_prev = SESSION_MEDIA_IDX_END;
    e->unread_next = SESSION_MEDIA_IDX_END;
    s->media_unread_head = slot;
    s->media_unread_tail = slot;
  } else {
    size_t tail_idx = (size_t)s->media_unread_tail - 1u;
    s->media[tail_idx].unread_next = slot;
    e->unread_prev = s->media_unread_tail;
    e->unread_next = SESSION_MEDIA_IDX_END;
    s->media_unread_tail = slot;
  }
  s->media_unread_count++;
}

static void media_unread_remove(ccord_session *s, size_t idx)
{
  session_media_entry *e = &s->media[idx];
  if (!media_is_unread(e)) return;
  uint16_t prev = e->unread_prev;
  uint16_t next = e->unread_next;
  if (prev == SESSION_MEDIA_IDX_END) {
    s->media_unread_head = (next == SESSION_MEDIA_IDX_END) ? 0u : next;
  } else {
    s->media[(size_t)prev - 1u].unread_next = next;
  }
  if (next == SESSION_MEDIA_IDX_END) {
    s->media_unread_tail = (prev == SESSION_MEDIA_IDX_END) ? 0u : prev;
  } else {
    s->media[(size_t)next - 1u].unread_prev = prev;
  }
  e->unread_prev = 0u;
  e->unread_next = 0u;
  s->media_unread_count--;
}

static void media_unread_relocate(ccord_session *s, size_t new_idx)
{
  const session_media_entry *e = &s->media[new_idx];
  if (!media_is_unread(e)) return;
  uint16_t new_slot = (uint16_t)(new_idx + 1u);
  if (e->unread_prev == SESSION_MEDIA_IDX_END) {
    s->media_unread_head = new_slot;
  } else {
    s->media[(size_t)e->unread_prev - 1u].unread_next = new_slot;
  }
  if (e->unread_next == SESSION_MEDIA_IDX_END) {
    s->media_unread_tail = new_slot;
  } else {
    s->media[(size_t)e->unread_next - 1u].unread_prev = new_slot;
  }
}

static size_t media_find_index(const ccord_session *s, const char *url, uint32_t h32)
{
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->media_hash[b] != 0u) {
    size_t idx = (size_t)s->media_hash[b] - 1u;
    if (s->media[idx].url_hash == h32 && s->media[idx].url && strcmp(s->media[idx].url, url) == 0) {
      return idx;
    }
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
  return SIZE_MAX;
}

static void media_hash_insert(ccord_session *s, size_t idx, uint32_t h32)
{
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->media_hash[b] != 0u) {
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
  s->media_hash[b] = (uint16_t)(idx + 1u);
}

static void media_hash_remove(ccord_session *s, size_t idx, uint32_t h32)
{
  uint16_t target = (uint16_t)(idx + 1u);
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->media_hash[b] != 0u) {
    if (s->media_hash[b] == target) {
      size_t hole = b;
      size_t cur = (hole + 1u) & SESSION_MEDIA_HASH_MASK;
      while (s->media_hash[cur] != 0u) {
        size_t cur_idx = (size_t)s->media_hash[cur] - 1u;
        size_t home = (size_t)(s->media[cur_idx].url_hash & SESSION_MEDIA_HASH_MASK);
        if (((hole - home) & SESSION_MEDIA_HASH_MASK) < ((cur - home) & SESSION_MEDIA_HASH_MASK)) {
          s->media_hash[hole] = s->media_hash[cur];
          hole = cur;
        }
        cur = (cur + 1u) & SESSION_MEDIA_HASH_MASK;
      }
      s->media_hash[hole] = 0u;
      return;
    }
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
}

static void media_hash_relocate(ccord_session *s, size_t old_idx, size_t new_idx, uint32_t h32)
{
  uint16_t target = (uint16_t)(old_idx + 1u);
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->media_hash[b] != 0u) {
    if (s->media_hash[b] == target) {
      s->media_hash[b] = (uint16_t)(new_idx + 1u);
      return;
    }
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
}

static size_t media_req_find_index(const ccord_session *s, const char *url, uint32_t h32)
{
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->media_req_hash[b] != 0u) {
    size_t idx = (size_t)s->media_req_hash[b] - 1u;
    if (s->media_req_hashes[idx] == h32 && s->media_requested[idx] && strcmp(s->media_requested[idx], url) == 0) {
      return idx;
    }
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
  return SIZE_MAX;
}

static void media_req_hash_insert(ccord_session *s, size_t idx, uint32_t h32)
{
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->media_req_hash[b] != 0u) {
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
  s->media_req_hash[b] = (uint16_t)(idx + 1u);
}

static void media_req_hash_remove(ccord_session *s, size_t idx, uint32_t h32)
{
  uint16_t target = (uint16_t)(idx + 1u);
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->media_req_hash[b] != 0u) {
    if (s->media_req_hash[b] == target) {
      size_t hole = b;
      size_t cur = (hole + 1u) & SESSION_MEDIA_HASH_MASK;
      while (s->media_req_hash[cur] != 0u) {
        size_t cur_idx = (size_t)s->media_req_hash[cur] - 1u;
        size_t home = (size_t)(s->media_req_hashes[cur_idx] & SESSION_MEDIA_HASH_MASK);
        if (((hole - home) & SESSION_MEDIA_HASH_MASK) < ((cur - home) & SESSION_MEDIA_HASH_MASK)) {
          s->media_req_hash[hole] = s->media_req_hash[cur];
          hole = cur;
        }
        cur = (cur + 1u) & SESSION_MEDIA_HASH_MASK;
      }
      s->media_req_hash[hole] = 0u;
      return;
    }
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
}

static void media_req_hash_relocate(ccord_session *s, size_t old_idx, size_t new_idx, uint32_t h32)
{
  uint16_t target = (uint16_t)(old_idx + 1u);
  size_t b = (size_t)(h32 & SESSION_MEDIA_HASH_MASK);
  while (s->media_req_hash[b] != 0u) {
    if (s->media_req_hash[b] == target) {
      s->media_req_hash[b] = (uint16_t)(new_idx + 1u);
      return;
    }
    b = (b + 1u) & SESSION_MEDIA_HASH_MASK;
  }
}

static void media_req_remove_at(ccord_session *s, size_t idx)
{
  media_req_hash_remove(s, idx, s->media_req_hashes[idx]);
  ccord_free(s->media_requested[idx]);
  size_t last = s->media_req_count - 1u;
  if (idx != last) {
    s->media_requested[idx] = s->media_requested[last];
    s->media_req_hashes[idx] = s->media_req_hashes[last];
    media_req_hash_relocate(s, last, idx, s->media_req_hashes[idx]);
  }
  s->media_req_count--;
  s->media_requested[s->media_req_count] = nullptr;
  s->media_req_hashes[s->media_req_count] = 0u;
}

static void media_forget_request(ccord_session *s, const char *url)
{
  if (!url || s->media_req_count == 0u) return;
  uint32_t h32 = media_hash_mix(media_url_hash(url));
  size_t idx = media_req_find_index(s, url, h32);
  if (idx != SIZE_MAX) media_req_remove_at(s, idx);
}

static void media_failed_set(ccord_session *s, const char *url, bool failed)
{
  if (!failed && s->media_failed_count == 0u) return;
  uint64_t h = media_url_hash(url);
  for (size_t i = 0; i < s->media_failed_count; i++) {
    size_t idx = (s->media_failed_head + i) % SESSION_MEDIA_FAILED_MAX;
    if (s->media_failed[idx] == h) {
      if (!failed) {
        if (i == 0u) {
          s->media_failed[idx] = 0u;
          s->media_failed_head = (s->media_failed_head + 1u) % SESSION_MEDIA_FAILED_MAX;
          s->media_failed_count--;
        } else {
          for (size_t j = i; j + 1u < s->media_failed_count; j++) {
            size_t dst = (s->media_failed_head + j) % SESSION_MEDIA_FAILED_MAX;
            size_t src = (s->media_failed_head + j + 1u) % SESSION_MEDIA_FAILED_MAX;
            s->media_failed[dst] = s->media_failed[src];
          }
          s->media_failed_count--;
          s->media_failed[(s->media_failed_head + s->media_failed_count) % SESSION_MEDIA_FAILED_MAX] = 0u;
        }
        if (s->media_failed_count == 0u) s->media_failed_head = 0u;
      }
      return;
    }
  }
  if (!failed) return;
  size_t idx = (s->media_failed_head + s->media_failed_count) % SESSION_MEDIA_FAILED_MAX;
  if (s->media_failed_count == SESSION_MEDIA_FAILED_MAX) {
    s->media_failed_head = (s->media_failed_head + 1u) % SESSION_MEDIA_FAILED_MAX;
  } else {
    s->media_failed_count++;
  }
  s->media_failed[idx] = h;
}

// Drops one cached body using swap-with-last and updates the hash & unread indices in O(1). Its pending-request
// marker goes too, so a later fetch really asks the transport again (which normally answers from the disk cache).
static void media_evict(ccord_session *s, size_t idx)
{
  session_media_entry *e = &s->media[idx];
  media_unread_remove(s, idx);
  media_hash_remove(s, idx, e->url_hash);
  if (e->url) media_forget_request(s, e->url);
  s->media_bytes -= e->len <= s->media_bytes ? e->len : s->media_bytes;
  ccord_free(e->url);
  ccord_free(e->cache_path);
  ccord_free(e->data);
  size_t last = s->media_count - 1u;
  if (idx != last) {
    s->media[idx] = s->media[last];
    media_hash_relocate(s, last, idx, s->media[idx].url_hash);
    media_unread_relocate(s, idx);
  }
  s->media_count--;
  memset(&s->media[s->media_count], 0, sizeof s->media[0]);
}

// Least recently used entry the UI is not about to pop (`keep` is never chosen); SIZE_MAX when there is none.
static size_t media_lru(const ccord_session *s, size_t keep, bool allow_queued)
{
  size_t best = SIZE_MAX;
  for (size_t i = 0; i < s->media_count; i++) {
    if (i == keep || (!allow_queued && media_is_unread(&s->media[i]))) continue;
    if (best == SIZE_MAX || s->media[i].access_tick < s->media[best].access_tick) best = i;
  }
  return best;
}

static void media_trim_bytes(ccord_session *s, const char *keep_url, uint32_t keep_h32)
{
  while (s->media_bytes > SESSION_MEDIA_BYTE_BUDGET && s->media_count > 1) {
    size_t keep = media_find_index(s, keep_url, keep_h32);
    size_t victim = media_lru(s, keep, false);
    if (victim == SIZE_MAX) return;
    media_evict(s, victim);
  }
}

static void store_media_loaded(ccord_session *s, const ccord_net_event *ev)
{
  if (!ev->media_url || ev->media_url[0] == '\0') return;
  if (ev->status != CCORD_OK) {
    // The download failed: forget that it is pending so the next fetch is a real retry.
    media_forget_request(s, ev->media_url);
    media_failed_set(s, ev->media_url, true);
    return;
  }
  media_failed_set(s, ev->media_url, false);
  uint32_t h32 = media_hash_mix(media_url_hash(ev->media_url));
  size_t existing = media_find_index(s, ev->media_url, h32);
  if (existing != SIZE_MAX) {
    session_media_entry *em = &s->media[existing];
    s->media_bytes -= em->len <= s->media_bytes ? em->len : s->media_bytes;
    ccord_free(em->cache_path);
    ccord_free(em->data);
    em->cache_path = ccord_strndup(ev->media_path ? ev->media_path : "",
                                   ev->media_path ? strlen(ev->media_path) : 0);
    em->data = ev->media_len ? ccord_alloc_array(ev->media_len, 1) : nullptr;
    if (em->data && ev->media_data) memcpy(em->data, ev->media_data, ev->media_len);
    em->len = em->data ? ev->media_len : 0;
    s->media_bytes += em->len;
    em->pub = (ccord_media_item){
      .url = em->url,
      .cache_path = em->cache_path ? em->cache_path : "",
      .data = em->data,
      .len = em->len,
    };
    em->access_tick = ++s->media_tick;
    media_unread_push(s, existing);
    media_forget_request(s, ev->media_url);
    media_trim_bytes(s, ev->media_url, h32);
    return;
  }
  if (s->media_count >= SESSION_MAX_MEDIA) {
    size_t evict = media_lru(s, SIZE_MAX, false);
    if (evict == SIZE_MAX) evict = media_lru(s, SIZE_MAX, true);
    media_evict(s, evict);
  }
  size_t new_idx = s->media_count;
  session_media_entry *e = &s->media[new_idx];
  memset(e, 0, sizeof *e);
  e->url = ccord_strndup(ev->media_url, strlen(ev->media_url));
  e->cache_path = ccord_strndup(ev->media_path ? ev->media_path : "",
                                ev->media_path ? strlen(ev->media_path) : 0);
  e->data = ev->media_len ? ccord_alloc_array(ev->media_len, 1) : nullptr;
  if (!e->url || !e->cache_path || (ev->media_len > 0 && !e->data)) {
    ccord_free(e->url);
    ccord_free(e->cache_path);
    ccord_free(e->data);
    memset(e, 0, sizeof *e);
    media_forget_request(s, ev->media_url);
    return;
  }
  if (ev->media_len > 0 && ev->media_data) memcpy(e->data, ev->media_data, ev->media_len);
  e->len = ev->media_len;
  e->pub = (ccord_media_item){
    .url = e->url,
    .cache_path = e->cache_path,
    .data = e->data,
    .len = e->len,
  };
  e->access_tick = ++s->media_tick;
  e->url_hash = h32;
  s->media_count++;
  media_hash_insert(s, new_idx, h32);
  media_unread_push(s, new_idx);
  s->media_bytes += e->len;
  media_forget_request(s, ev->media_url);
  media_trim_bytes(s, ev->media_url, h32);
}

static const char *dup_in_search_arena(ccord_arena *a, const char *str)
{
  if (!str || str[0] == '\0') return "";
  char *c = ccord_arena_strndup(a, str, strlen(str));
  return c ? c : "";
}

static void clear_search_results(ccord_session *s)
{
  for (size_t i = 0; s->search_rich && i < s->search_count; i++) ccord_message_rich_free(s->search_rich[i]);
  ccord_free(s->search_rich);
  s->search_rich = nullptr;
  ccord_free(s->search_results);
  s->search_results = nullptr;
  s->search_count = 0;
  s->search_total_results = 0;
  s->search_offset = 0;
  s->search_loading = false;
  ccord_arena_free(&s->search_arena);
}

static void clear_reaction_users(ccord_session *s)
{
  ccord_free(s->react_users);
  s->react_users = nullptr;
  s->react_user_count = 0;
  s->react_users_loading = false;
  s->react_users_channel_id = CCORD_ID_NONE;
  s->react_users_message_id = CCORD_ID_NONE;
  s->react_users_emoji[0] = '\0';
  ccord_arena_free(&s->react_users_arena);
}

static void store_reaction_users(ccord_session *s, const ccord_net_event *ev)
{
  ccord_free(s->react_users);
  s->react_users = nullptr;
  s->react_user_count = 0;
  ccord_arena_free(&s->react_users_arena);
  s->react_users_loading = false;
  s->react_users_channel_id = ev->channel_id;
  s->react_users_message_id = ev->message_id;
  if (ev->reaction_emoji && ev->reaction_emoji[0] != '\0') {
    size_t elen = strlen(ev->reaction_emoji);
    if (elen >= sizeof s->react_users_emoji) elen = sizeof s->react_users_emoji - 1u;
    memcpy(s->react_users_emoji, ev->reaction_emoji, elen);
    s->react_users_emoji[elen] = '\0';
  }
  if (!ev->users || ev->user_count == 0) return;
  ccord_arena_init(&s->react_users_arena, ev->user_count * 128u + 512u);
  s->react_users = ccord_calloc(ev->user_count, sizeof *s->react_users);
  if (!s->react_users) {
    ccord_arena_free(&s->react_users_arena);
    return;
  }
  for (size_t i = 0; i < ev->user_count; i++) {
    ccord_user u = ev->users[i];
    u.name = dup_in_search_arena(&s->react_users_arena, u.name);
    u.handle = dup_in_search_arena(&s->react_users_arena, u.handle);
    u.status_text = dup_in_search_arena(&s->react_users_arena, u.status_text);
    u.avatar_url = dup_in_search_arena(&s->react_users_arena, u.avatar_url);
    u.banner_url = dup_in_search_arena(&s->react_users_arena, u.banner_url);
    u.bio = dup_in_search_arena(&s->react_users_arena, u.bio);
    u.pronouns = dup_in_search_arena(&s->react_users_arena, u.pronouns);
    s->react_users[i] = u;
  }
  s->react_user_count = ev->user_count;
}

static bool pins_is_in_flight(const ccord_session *s, ccord_id channel_id)
{
  if (!s || channel_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < s->npins_in_flight; i++) {
    if (s->pins_in_flight[i] == channel_id) return true;
  }
  return false;
}

static void pins_mark_in_flight(ccord_session *s, ccord_id channel_id)
{
  if (!s || channel_id == CCORD_ID_NONE || pins_is_in_flight(s, channel_id)) return;
  if (s->npins_in_flight < sizeof s->pins_in_flight / sizeof s->pins_in_flight[0]) {
    s->pins_in_flight[s->npins_in_flight++] = channel_id;
  }
}

static void pins_clear_in_flight(ccord_session *s, ccord_id channel_id)
{
  if (!s || channel_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < s->npins_in_flight; ) {
    if (s->pins_in_flight[i] == channel_id) {
      memmove(&s->pins_in_flight[i], &s->pins_in_flight[i + 1],
              (s->npins_in_flight - i - 1) * sizeof s->pins_in_flight[0]);
      s->npins_in_flight--;
    } else {
      i++;
    }
  }
}

static void store_search_results(ccord_session *s, const ccord_net_event *ev)
{
  clear_search_results(s);
  s->search_offset = ev->search_offset;
  s->search_total_results = ev->search_total_results >= ev->message_count
                            ? ev->search_total_results
                            : (uint32_t)ev->message_count;
  if (!ev->messages || ev->message_count == 0) return;
  ccord_arena_init(&s->search_arena, ev->message_count * 256u + 1024u);
  s->search_results = ccord_calloc(ev->message_count, sizeof *s->search_results);
  s->search_rich = ccord_calloc(ev->message_count, sizeof *s->search_rich);
  if (!s->search_results || !s->search_rich) {
    ccord_free(s->search_results);
    ccord_free(s->search_rich);
    s->search_results = nullptr;
    s->search_rich = nullptr;
    ccord_arena_free(&s->search_arena);
    return;
  }
  for (size_t i = 0; i < ev->message_count; i++) {
    ccord_message m = ev->messages[i];
    m.content = dup_in_search_arena(&s->search_arena, m.content);
    // The event's rich block goes away with the event: keep a copy of our own (none if it cannot be had).
    s->search_rich[i] = ccord_message_rich_clone(m.rich, nullptr);
    m.rich = s->search_rich[i];
    if (m.reactions && m.reaction_kind_count > 0) {
      size_t rcount = m.reaction_kind_count;
      ccord_reaction *rs = ccord_arena_alloc(&s->search_arena, rcount * sizeof *rs, _Alignof(ccord_reaction));
      if (rs) {
        for (size_t r = 0; r < rcount; r++) {
          rs[r] = m.reactions[r];
          rs[r].emoji = dup_in_search_arena(&s->search_arena, m.reactions[r].emoji);
        }
        m.reactions = rs;
      } else {
        m.reactions = nullptr;
        m.reaction_kind_count = 0;
      }
    } else {
      m.reactions = nullptr;
      m.reaction_kind_count = 0;
    }
    s->search_results[i] = m;
  }
  s->search_count = ev->message_count;
}

static void flush_seeded_members(ccord_store *store, ccord_id gid, const ccord_member *batch, size_t nbatch,
                                 bool fallback_per_item)
{
  if (nbatch == 0) return;
  ccord_error ignored;
  if (ccord_store_upsert_guild_members(store, gid, batch, nbatch, nullptr, 0, &ignored) != CCORD_OK &&
      fallback_per_item && nbatch > 1u) {
    for (size_t i = 0; i < nbatch; i++) {
      (void)ccord_store_upsert_guild_members(store, gid, &batch[i], 1u, nullptr, 0, &ignored);
    }
  }
}

static void seed_channel_message_members(ccord_session *s, ccord_id channel_id, ccord_id guild_hint,
                                         const ccord_message *msgs, size_t msg_count,
                                         const ccord_member *ev_members, size_t ev_member_count)
{
  if (!s || !s->store) return;
  const ccord_channel *ch = (channel_id != CCORD_ID_NONE) ? ccord_store_channel(s->store, channel_id) : nullptr;
  ccord_id gid = ch ? ch->guild_id : guild_hint;
  if (gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) return;
  if (!ccord_store_guild(s->store, gid)) return;

  ccord_member batch[64];
  size_t nbatch = 0;
  if (ev_members && ev_member_count > 0) {
    for (size_t i = 0; i < ev_member_count; i++) {
      if (ev_members[i].guild_id == gid && ccord_store_user(s->store, ev_members[i].user_id)) {
        batch[nbatch++] = ev_members[i];
        if (nbatch == sizeof batch / sizeof batch[0]) {
          flush_seeded_members(s->store, gid, batch, nbatch, true);
          nbatch = 0;
        }
      }
    }
    flush_seeded_members(s->store, gid, batch, nbatch, true);
    nbatch = 0;
  }
  if (!msgs || msg_count == 0) return;
  for (size_t i = 0; i < msg_count; i++) {
    ccord_id uid = msgs[i].author_id;
    if (uid == CCORD_ID_NONE || !ccord_store_user(s->store, uid)) continue;
    // Seeing an author says nothing new about a member we know: the sighting has no nickname or roles, so it must not
    // overwrite the ones the list already has.
    if (ccord_store_find_member(s->store, gid, uid)) continue;
    bool dup = false;
    for (size_t b = 0; b < nbatch; b++) {
      if (batch[b].user_id == uid) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    batch[nbatch++] = (ccord_member){
      .guild_id = gid,
      .user_id = uid,
      .role_name = "Members",
      .role_color = 0,
      .role_rank = 255,
    };
    if (nbatch == sizeof batch / sizeof batch[0]) {
      flush_seeded_members(s->store, gid, batch, nbatch, false);
      nbatch = 0;
    }
  }
  flush_seeded_members(s->store, gid, batch, nbatch, false);
}

// Applies the emoji/sticker lists carried by SNAPSHOT / GUILD_UPSERT / GUILD_EXPRESSIONS events. The lists are grouped
// by guild (each entry carries its guild_id); `fallback_guild` receives an empty replacement when the payload said the
// list is authoritative (replace flag) but contained no entries.
static void apply_expression_lists(ccord_session *s, const ccord_net_event *ev, ccord_id fallback_guild)
{
  ccord_error ignored;
  for (size_t i = 0; i < ev->emoji_count;) {
    size_t j = i;
    while (j < ev->emoji_count && ev->emojis[j].guild_id == ev->emojis[i].guild_id) j++;
    if (ev->emojis[i].guild_id != CCORD_ID_NONE) {
      if (ev->expressions_admin) {
        (void)ccord_store_set_guild_emojis_admin(s->store, ev->emojis[i].guild_id, &ev->emojis[i], j - i, &ignored);
      } else {
        (void)ccord_store_set_guild_emojis(s->store, ev->emojis[i].guild_id, &ev->emojis[i], j - i, &ignored);
      }
    }
    i = j;
  }
  for (size_t i = 0; i < ev->sticker_count;) {
    size_t j = i;
    while (j < ev->sticker_count && ev->stickers[j].guild_id == ev->stickers[i].guild_id) j++;
    if (ev->stickers[i].guild_id != CCORD_ID_NONE) {
      if (ev->expressions_admin) {
        (void)ccord_store_set_guild_stickers_admin(s->store, ev->stickers[i].guild_id, &ev->stickers[i], j - i, &ignored);
      } else {
        (void)ccord_store_set_guild_stickers(s->store, ev->stickers[i].guild_id, &ev->stickers[i], j - i, &ignored);
      }
    }
    i = j;
  }
  for (size_t i = 0; i < ev->soundboard_count;) {
    size_t j = i;
    while (j < ev->soundboard_count && ev->soundboard[j].guild_id == ev->soundboard[i].guild_id) j++;
    if (ev->soundboard[i].guild_id != CCORD_ID_NONE) {
      if (ev->expressions_admin) {
        (void)ccord_store_set_guild_soundboard_admin(s->store, ev->soundboard[i].guild_id, &ev->soundboard[i], j - i,
                                                     &ignored);
      } else {
        (void)ccord_store_set_guild_soundboard(s->store, ev->soundboard[i].guild_id, &ev->soundboard[i], j - i,
                                               &ignored);
      }
    }
    i = j;
  }
  if (fallback_guild != CCORD_ID_NONE) {
    if (ev->replace_emojis && ev->emoji_count == 0) {
      if (ev->expressions_admin) {
        (void)ccord_store_set_guild_emojis_admin(s->store, fallback_guild, nullptr, 0, &ignored);
      } else {
        (void)ccord_store_set_guild_emojis(s->store, fallback_guild, nullptr, 0, &ignored);
      }
    }
    if (ev->replace_stickers && ev->sticker_count == 0) {
      if (ev->expressions_admin) {
        (void)ccord_store_set_guild_stickers_admin(s->store, fallback_guild, nullptr, 0, &ignored);
      } else {
        (void)ccord_store_set_guild_stickers(s->store, fallback_guild, nullptr, 0, &ignored);
      }
    }
    if (ev->replace_soundboard && ev->soundboard_count == 0) {
      if (ev->expressions_admin) {
        (void)ccord_store_set_guild_soundboard_admin(s->store, fallback_guild, nullptr, 0, &ignored);
      } else {
        (void)ccord_store_set_guild_soundboard(s->store, fallback_guild, nullptr, 0, &ignored);
      }
    }
  }
}

// Applies the roles, member role ids and channel overwrites an event carries. Roles are grouped by guild (each
// carries its guild_id); an event without them changes nothing (unknown is not "none").
static void apply_permission_data(ccord_session *s, const ccord_net_event *ev)
{
  ccord_error ignored;
  for (size_t i = 0; i < ev->role_count;) {
    size_t j = i;
    while (j < ev->role_count && ev->roles[j].guild_id == ev->roles[i].guild_id) j++;
    if (ev->roles[i].guild_id != CCORD_ID_NONE) {
      (void)ccord_store_set_guild_roles(s->store, ev->roles[i].guild_id, &ev->roles[i], j - i, &ignored);
    }
    i = j;
  }
  for (size_t i = 0; i < ev->member_role_count; i++) {
    const ccord_member_roles *m = &ev->member_roles[i];
    (void)ccord_store_set_member_roles(s->store, m->guild_id, m->user_id, m->role_ids, m->count, &ignored);
  }
  for (size_t i = 0; i < ev->overwrite_count; i++) {
    const ccord_channel_overwrites *o = &ev->overwrites[i];
    const ccord_channel *c = ccord_store_channel(s->store, o->channel_id);
    if (c) (void)ccord_store_set_channel_overwrites(s->store, o->channel_id, c->guild_id, o->items, o->count, &ignored);
  }
}

// Stores the notification rows an event carries. Rows come grouped by server and each group replaces what was known of
// that server (a READY carries them whole; the store was cleared by the snapshot).
static void apply_notify_rows(ccord_session *s, const ccord_notify_scope *rows, size_t n)
{
  ccord_error ignored;
  for (size_t i = 0; i < n;) {
    size_t j = i;
    while (j < n && rows[j].guild_id == rows[i].guild_id) j++;
    if (ccord_store_set_notify(s->store, rows[i].guild_id, &rows[i], j - i, &ignored) != CCORD_OK) {
      CCORD_LOG_W("session: notification settings rejected: %s", ignored.message);
    }
    i = j;
  }
}

// What applies to a channel right now. When the user never chose an override level, a guild channel falls back to its
// server's default_message_notifications (0 = All Messages, 1 = Only @mentions) when guild settings are known, and
// otherwise notifies for mentions and direct messages only.
static ccord_notify_effective notify_effective(const ccord_session *s, ccord_id channel_id)
{
  ccord_notify_effective eff = ccord_store_channel_notify(s->store, channel_id, ccord_clock_unix_ms());
  if (!eff.level_set) {
    const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
    const ccord_guild *g = (ch && ch->guild_id != CCORD_ID_NONE && ch->guild_id != CCORD_DM_GUILD_ID)
                               ? ccord_store_guild(s->store, ch->guild_id)
                               : nullptr;
    if (g && g->has_guild_settings) {
      eff.level = g->default_message_notifications == 0 ? CCORD_NOTIFY_ALL : CCORD_NOTIFY_MENTIONS;
    } else {
      eff.level = CCORD_NOTIFY_MENTIONS;
    }
  }
  return eff;
}

// Fixed-size inline text from a transport is only trusted when a terminator sits inside its buffer: a
// malformed row must compare unequal rather than read past the array.
static bool fixed_text_equal(const char *a, size_t acap, const char *b, size_t bcap)
{
  const char *ae = memchr(a, '\0', acap);
  const char *be = memchr(b, '\0', bcap);
  if (!ae || !be) return false;
  size_t alen = (size_t)(ae - a);
  size_t blen = (size_t)(be - b);
  return alen == blen && memcmp(a, b, alen) == 0;
}

// A logout ack names exactly the hashes the server closed: drop only those cached rows, in place.
static void drop_auth_sessions(ccord_session *s, const char hashes[][64], size_t count)
{
  if (!s || !hashes) return;
  for (size_t i = 0; i < s->auth_session_count;) {
    bool drop = false;
    for (size_t j = 0; j < count && !drop; j++) {
      drop = fixed_text_equal(s->auth_sessions[i].id_hash, sizeof s->auth_sessions[i].id_hash, hashes[j], 64u);
    }
    if (drop) {
      memmove(&s->auth_sessions[i], &s->auth_sessions[i + 1],
              (s->auth_session_count - i - 1u) * sizeof s->auth_sessions[0]);
      s->auth_session_count--;
    } else {
      i++;
    }
  }
}

// A PATCH answer carries one connection: replace the cached row it names, append it when a loaded list
// simply predates it. Before the list is loaded there is nothing to patch, and inventing a one-row list
// would be the fake local state the UI must not show.
static void upsert_connection(ccord_session *s, const ccord_connection *c)
{
  if (!s || !c || s->connections_state != CCORD_FETCH_READY) return;
  for (size_t i = 0; i < s->connection_count; i++) {
    if (fixed_text_equal(s->connections[i].type, sizeof s->connections[i].type, c->type, sizeof c->type) &&
        fixed_text_equal(s->connections[i].id, sizeof s->connections[i].id, c->id, sizeof c->id)) {
      s->connections[i] = *c;
      return;
    }
  }
  if (s->connection_count < CCORD_SESSION_MAX_CONNECTIONS) s->connections[s->connection_count++] = *c;
}

static void drop_connection(ccord_session *s, const char *name, const char *id)
{
  if (!s || !name || !id) return;
  for (size_t i = 0; i < s->connection_count; i++) {
    if (fixed_text_equal(s->connections[i].type, sizeof s->connections[i].type, name, 32u) &&
        fixed_text_equal(s->connections[i].id, sizeof s->connections[i].id, id, 64u)) {
      memmove(&s->connections[i], &s->connections[i + 1],
              (s->connection_count - i - 1u) * sizeof s->connections[0]);
      s->connection_count--;
      return;
    }
  }
}

// ---- M0-3 local pending/failed echoes ----
// A send inserts a visibly-pending echo with a local id whose top bit no current snowflake uses, so it sorts after
// confirmed history and cannot collide with a server id. The server's nonce-matched MESSAGE_CREATE replaces it; a
// refusal marks it failed. Echoes never move unread/mention counts: they are the user's own unconfirmed text.
#define SESSION_LOCAL_ID_FLAG (1ULL << 63)

typedef struct session_local_echo {
  ccord_id id;
  ccord_id author_id;
  int64_t timestamp_ms;
  uint8_t send_state;
  char nonce[32];
  char text[CCORD_SEND_MAX_BYTES + 1];
} session_local_echo;

static bool session_is_local_echo_id(ccord_id id)
{
  return (id & SESSION_LOCAL_ID_FLAG) != 0;
}

// Unique decimal nonce: monotonic timestamp + a per-session counter (well under Discord's 25-character cap).
static void make_send_nonce(ccord_session *s, char out[static 32])
{
  (void)snprintf(out, 32, "%llu%06llu", (unsigned long long)ccord_clock_ms(),
                 (unsigned long long)(++s->nonce_seq % 1000000ull));
}

// Copies the newest pending/failed echo of `channel_id` carrying `nonce` into `out` (by value: store pointers do not
// survive the upserts that follow). Newest-first: echoes sort after confirmed history.
static bool find_local_echo_by_nonce(const ccord_session *s, ccord_id channel_id, const char *nonce,
                                     session_local_echo *out)
{
  if (!nonce || nonce[0] == '\0') return false;
  size_t n = ccord_store_message_count(s->store, channel_id);
  for (size_t i = n; i > 0; i--) {
    const ccord_message *m = ccord_store_message_at(s->store, channel_id, i - 1u);
    if (!m || !session_is_local_echo_id(m->id) || m->send_state == CCORD_MSG_SEND_CONFIRMED) continue;
    if (strcmp(m->nonce, nonce) != 0) continue;
    out->id = m->id;
    out->author_id = m->author_id;
    out->timestamp_ms = m->timestamp_ms;
    out->send_state = m->send_state;
    memcpy(out->nonce, m->nonce, sizeof out->nonce);
    (void)snprintf(out->text, sizeof out->text, "%s", m->content);
    return true;
  }
  return false;
}

// Deletes an echo without touching unread/mention counts: the echo never contributed to them, but the store's
// delete would otherwise forget an "unread" row whose local id sits past the read position.
static void remove_local_echo(ccord_session *s, ccord_id channel_id, ccord_id echo_id)
{
  const ccord_channel *c = ccord_store_channel(s->store, channel_id);
  if (!c) return;
  uint32_t unread = c->unread_count, mentions = c->mention_count;
  ccord_error err;
  if (ccord_store_delete_message(s->store, channel_id, echo_id, &err) == CCORD_OK) {
    (void)ccord_store_set_channel_counts(s->store, channel_id, unread, mentions, &err);
  }
}

// Re-states an echo (pending <-> failed) by delete + re-append at the same id, preserving the counts.
static bool set_local_echo_state(ccord_session *s, ccord_id channel_id, const session_local_echo *e, uint8_t state)
{
  const ccord_channel *c = ccord_store_channel(s->store, channel_id);
  if (!c) return false;
  uint32_t unread = c->unread_count, mentions = c->mention_count;
  ccord_message m = {
    .id = e->id,
    .channel_id = channel_id,
    .author_id = e->author_id,
    .timestamp_ms = e->timestamp_ms,
    .content = e->text,
    .send_state = state,
  };
  memcpy(m.nonce, e->nonce, sizeof m.nonce);
  ccord_error err;
  if (ccord_store_delete_message(s->store, channel_id, e->id, &err) != CCORD_OK) return false;
  if (ccord_store_append_message(s->store, &m, &err) != CCORD_OK) return false;
  (void)ccord_store_set_channel_counts(s->store, channel_id, unread, mentions, &err);
  return true;
}

static void record_failure(ccord_session *s, const ccord_net_event *ev);

// Standard or url-safe base64 (padding optional, ASCII whitespace ignored) into a fresh ccord_malloc buffer.
// Mirrors the settings-proto reader's decoder so the cached wire bytes are exactly what the server sent.
static ccord_status session_b64_decode(const char *b64, size_t len, uint8_t **out, size_t *out_len, ccord_error *err)
{
  *out = nullptr;
  *out_len = 0;
  if (len > 0 && !b64) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null base64");
  if (len > CCORD_PROTO_MAX_INPUT * 2u) return ccord_error_set(err, CCORD_ERR_RANGE, "session: base64 too large");
  uint8_t *raw = ccord_malloc(len / 4 * 3 + 4);
  if (!raw) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
  size_t n = 0;
  uint32_t acc = 0;
  unsigned bits = 0, chars = 0;
  bool padding = false;
  ccord_status st = CCORD_OK;
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)b64[i];
    if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
    if (ch == '=') {
      padding = true;
      continue;
    }
    int v;
    if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
    else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
    else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
    else if (ch == '+' || ch == '-') v = 62;
    else if (ch == '/' || ch == '_') v = 63;
    else v = -1;
    if (v < 0 || padding) {
      st = ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid base64");
      break;
    }
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    chars++;
    if (bits >= 8) {
      bits -= 8;
      raw[n++] = (uint8_t)((acc >> bits) & 0xffu);
    }
  }
  if (st == CCORD_OK && chars % 4 == 1) st = ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid base64 length");
  if (st != CCORD_OK) {
    ccord_free(raw);
    return st;
  }
  *out = raw;
  *out_len = n;
  return CCORD_OK;
}

// Keeps the lossless wire bytes of the last settings-proto answer: the transport may hand them over directly
// (raw_proto), otherwise they are the base64 "settings" string of the GET body. Favorite-GIF edits re-encode
// from this cache, so versions, frecency and unknown fields survive a PATCH.
static void cache_settings_proto(ccord_session *s, const ccord_net_event *ev)
{
  ccord_error err;
  if (ev->raw_proto && ev->raw_proto_len > 0) {
    if (ccord_store_set_raw_settings_proto(s->store, ev->raw_proto, ev->raw_proto_len, &err) != CCORD_OK) {
      CCORD_LOG_W("session: settings proto cache rejected: %s", err.message);
    }
    return;
  }
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  ccord_json_limits lim = {.max_depth = 8, .max_nodes = 256, .max_string = CCORD_PROTO_MAX_INPUT * 2u};
  const ccord_json *root = ccord_json_parse(&tmp, ev->expr_body, ev->expr_body_len, &lim, &err);
  const ccord_json *node =
    root && ccord_json_type_of(root) == CCORD_JSON_OBJECT ? ccord_json_get(root, "settings") : nullptr;
  size_t blen = 0;
  const char *b64 = node ? ccord_json_string(node, &blen) : nullptr;
  if (b64) {
    uint8_t *raw = nullptr;
    size_t rlen = 0;
    if (session_b64_decode(b64, blen, &raw, &rlen, &err) == CCORD_OK) {
      if (ccord_store_set_raw_settings_proto(s->store, raw, rlen, &err) != CCORD_OK) {
        CCORD_LOG_W("session: settings proto cache rejected: %s", err.message);
      }
      ccord_free(raw);
    }
  }
  ccord_arena_free(&tmp);
}

// Re-encodes the store's favorite GIF list over the cached settings-proto wire bytes (lossless: every other
// top-level record is copied verbatim) and PATCHes it. The cache moves to the sent bytes only once the request
// went out; the server's answer refreshes it again through the SETTINGS_PROTO ingest.
// Debug aid for "my favourite GIFs are out of order": the count and the first 10 favourites as the picker draws them,
// each as order:hash (an FNV-1a hash of the GIF page URL, so the log holds no URL).
static void log_favorite_gifs(const ccord_store *store)
{
  char line[256];
  size_t n = ccord_store_favorite_gif_count(store), off = 0;
  line[0] = '\0';
  for (size_t i = 0; i < n && i < 10u && off < sizeof line; i++) {
    const ccord_gif_item *it = ccord_store_favorite_gif_at(store, i);
    if (!it) break;
    uint32_t h = 2166136261u;
    for (const char *p = it->url; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    int w = snprintf(line + off, sizeof line - off, " %u:%08x", (unsigned)it->order, (unsigned)h);
    if (w < 0 || (size_t)w >= sizeof line - off) break;
    off += (size_t)w;
  }
  CCORD_LOG_D("session: favourite gifs loaded: count=%zu first (order:urlhash):%s", n, line);
}

static session_fav_pending *fav_pending_find(ccord_session *s, const char *url)
{
  for (size_t i = 0; i < s->nfav_pending; i++) {
    if (strcmp(s->fav_pending[i].url, url) == 0) return &s->fav_pending[i];
  }
  return nullptr;
}

static void fav_pending_drop(ccord_session *s, size_t i)
{
  ccord_free(s->fav_pending[i].url);
  memmove(&s->fav_pending[i], &s->fav_pending[i + 1], (s->nfav_pending - i - 1u) * sizeof s->fav_pending[0]);
  s->nfav_pending--;
}

// Records a local toggle (latest one per URL wins). When the table is full the oldest record is evicted.
static void fav_pending_note(ccord_session *s, const char *url, bool starred)
{
  session_fav_pending *p = fav_pending_find(s, url);
  if (!p) {
    char *copy = ccord_strndup(url, strlen(url));
    if (!copy) return;   // out of memory: no shield, the pre-existing additive behaviour of the next load stays
    if (s->nfav_pending == SESSION_FAV_PENDING_MAX) fav_pending_drop(s, 0);
    p = &s->fav_pending[s->nfav_pending++];
    p->url = copy;
  }
  p->starred = starred;
  p->at_ms = ccord_clock_ms();
}

// Makes the store's favourite GIFs equal the wire list `ux` (authoritative: favorite_gifs was present), except for local
// toggles that the server has not confirmed yet. Confirmed or expired toggles are forgotten. `authoritative` false (a
// partial gateway delta) only adds and re-orders: absence from a delta proves nothing.
static void apply_favorite_gifs_wire(ccord_session *s, const ccord_user_expressions *ux, bool authoritative)
{
  uint64_t now = ccord_clock_ms();
  for (size_t i = s->nfav_pending; i > 0; i--) {
    if (now - s->fav_pending[i - 1u].at_ms > SESSION_FAV_PENDING_TTL_MS) fav_pending_drop(s, i - 1u);
  }
  // ux.gifs is lodash sortBy("order") (ascending, ties in wire order). Discord then calls .reverse() on it, so
  // the store is fed from the end: highest order first, and tied entries in reverse wire order.
  for (size_t k = 0; k < ux->gif_count; k++) {
    size_t i = ux->gif_count - 1u - k;
    ccord_gif_item g = {0};
    if (ux->gifs[i].url) strncpy(g.url, ux->gifs[i].url, sizeof g.url - 1);
    if (ux->gifs[i].src) strncpy(g.src, ux->gifs[i].src, sizeof g.src - 1);
    g.width = ux->gifs[i].width ? ux->gifs[i].width : 320;
    g.height = ux->gifs[i].height ? ux->gifs[i].height : 240;
    g.format = ux->gifs[i].format;   // wire values kept so a later PATCH re-encodes them losslessly
    g.order = ux->gifs[i].order;
    if (!g.url[0]) continue;
    const session_fav_pending *p = fav_pending_find(s, g.url);
    if (p && !p->starred) continue;   // unstarred here, not yet confirmed: an older answer must not bring it back
    if (!ccord_store_is_favorite_gif(s->store, g.url)) (void)ccord_store_toggle_favorite_gif(s->store, &g);
    // The wire order is authoritative: a new favorite keeps it (not the next free order), and an existing
    // one that a settings update re-orders moves to its new position.
    (void)ccord_store_set_favorite_gif_order(s->store, g.url, g.order);
  }
  if (ux->has_favorite_gifs && authoritative) {
    // Unstarred on another device: in the store but not in the authoritative list.
    for (size_t i = ccord_store_favorite_gif_count(s->store); i > 0; i--) {
      const ccord_gif_item *it = ccord_store_favorite_gif_at(s->store, i - 1u);
      if (!it) continue;
      bool on_wire = false;
      for (size_t k = 0; k < ux->gif_count && !on_wire; k++) on_wire = ux->gifs[k].url && strcmp(ux->gifs[k].url, it->url) == 0;
      if (on_wire) continue;
      const session_fav_pending *p = fav_pending_find(s, it->url);
      if (p && p->starred) continue;   // starred here, PATCH not confirmed yet: keep it, the next PATCH carries it
      ccord_gif_item gone = *it;       // by value: the toggle shifts the store's array under `it`
      (void)ccord_store_toggle_favorite_gif(s->store, &gone);
    }
    // A toggle is confirmed once the list agrees with it.
    for (size_t i = s->nfav_pending; i > 0; i--) {
      bool on_wire = false;
      for (size_t k = 0; k < ux->gif_count && !on_wire; k++) {
        on_wire = ux->gifs[k].url && strcmp(ux->gifs[k].url, s->fav_pending[i - 1u].url) == 0;
      }
      if (on_wire == s->fav_pending[i - 1u].starred) fav_pending_drop(s, i - 1u);
    }
  }
  if (ux->gif_count > 0) log_favorite_gifs(s->store);
}

// Another client changed the account's type-2 settings (gateway USER_SETTINGS_PROTO_UPDATE): favourite GIFs follow it
// through the same path as a settings-proto answer. A whole (non-partial) proto also becomes the base of our next PATCH.
// Only a non-partial proto that carries favorite_gifs can remove a favourite; a delta, an absent field or bytes that do
// not decode remove nothing.
static void session_apply_frecency_update(ccord_session *s, const ccord_net_event *ev)
{
  ccord_error err;
  ccord_user_expressions ux;
  if (ccord_user_expressions_decode(ev->raw_proto, ev->raw_proto_len, &ux, &err) != CCORD_OK) {
    CCORD_LOG_W("session: settings proto update undecodable: %s", err.message);
    return;
  }
  if (!ev->settings_proto1_partial &&
      ccord_store_set_raw_settings_proto(s->store, ev->raw_proto, ev->raw_proto_len, &err) != CCORD_OK) {
    CCORD_LOG_W("session: settings proto cache rejected: %s", err.message);
  }
  apply_favorite_gifs_wire(s, &ux, !ev->settings_proto1_partial);
  ccord_user_expressions_free(&ux);
}

static ccord_status session_patch_favorite_gifs(ccord_session *s, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  size_t n = ccord_store_favorite_gif_count(s->store);
  ccord_proto_gif *gifs = nullptr;
  if (n > 0) {
    gifs = ccord_alloc_array(n, sizeof *gifs);
    if (!gifs) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
    for (size_t i = 0; i < n; i++) {
      const ccord_gif_item *it = ccord_store_favorite_gif_at(s->store, i);
      gifs[i] = (ccord_proto_gif){.url = it->url, .src = it->src, .format = it->format,
                                  .width = it->width, .height = it->height, .order = it->order};
    }
  }
  size_t existing_len = 0;
  const uint8_t *existing = ccord_store_get_raw_settings_proto(s->store, &existing_len);
  uint8_t *out = nullptr;
  size_t out_len = 0;
  ccord_status st = ccord_user_expressions_encode_gifs(gifs, n, existing, existing_len, &out, &out_len, err);
  ccord_free(gifs);
  if (st != CCORD_OK) return st;
  st = ccord_transport_patch_settings_proto(s->transport, out, out_len, err);
  if (st == CCORD_OK) {
    ccord_error ignored;
    if (ccord_store_set_raw_settings_proto(s->store, out, out_len, &ignored) != CCORD_OK) {
      CCORD_LOG_W("session: settings proto cache rejected: %s", ignored.message);
    }
  }
  ccord_free(out);
  return st;
}

// Voice disconnect (or a move to another channel): leave every activity instance of the old channel that still
// lists us as a participant, so no ghost participant is left behind. Fire-and-forget: each store row disappears
// when Discord's instance update confirms it. The server may already have removed us; then nothing is sent.
static void session_leave_channel_activities(ccord_session *s, ccord_id channel_id)
{
  ccord_id self = ccord_store_self_id(s->store);
  if (self == CCORD_ID_NONE) return;
  size_t n = ccord_store_activity_instance_count(s->store, channel_id);
  for (size_t i = 0; i < n; i++) {
    const ccord_activity_instance *row = ccord_store_activity_instance_at(s->store, channel_id, i);
    if (!row) break;
    ccord_activity_instance inst = *row;   // copy: the send below must not borrow a row it could invalidate
    bool participant = false;
    for (size_t p = 0; p < inst.participant_count; p++) {
      if (inst.participants[p] == self) {
        participant = true;
        break;
      }
    }
    if (!participant) continue;
    ccord_id location = inst.location_id != CCORD_ID_NONE ? inst.location_id : inst.channel_id;
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_activity_leave(&req, inst.application_id, location, inst.instance_id, &err) != CCORD_OK) {
      CCORD_LOG_W("session: activity leave request rejected: %s", err.message);
      continue;
    }
    if (ccord_session_admin(s, &req, &err) != CCORD_OK) {
      CCORD_LOG_W("session: activity leave failed: %s", err.message);
    }
  }
}

// The own user's "Playing X" line in the store. Discord does not reliably send a user its own PRESENCE_UPDATE, so an
// activity that ended elsewhere (the RPC client left, another session stopped it) would stay shown forever: the
// events that carry the account's own activity (SESSIONS_REPLACE, READY) and our own clear of the Rich Presence set
// or clear it here. Only the activity fields change; presence and custom status stay as the store has them.
static session_guild_profile_cache *guild_profile_for(ccord_session *s, ccord_id guild_id)
{
  for (size_t i = 0; i < s->nguild_profiles; i++) {
    if (s->guild_profiles[i].guild_id == guild_id) {
      s->guild_profiles[i].last_use = ++s->cache_tick;
      return &s->guild_profiles[i];
    }
  }
  session_guild_profile_cache *ent;
  if (s->nguild_profiles >= SESSION_GUILD_PROFILE_CACHE_MAX) {
    size_t victim = SIZE_MAX;
    for (size_t i = 0; i < s->nguild_profiles; i++) {
      if (s->guild_profiles[i].loading) continue;
      if (victim == SIZE_MAX || s->guild_profiles[i].last_use < s->guild_profiles[victim].last_use) victim = i;
    }
    if (victim == SIZE_MAX) {
      victim = 0;
      for (size_t i = 1; i < s->nguild_profiles; i++) {
        if (s->guild_profiles[i].last_use < s->guild_profiles[victim].last_use) victim = i;
      }
    }
    ent = &s->guild_profiles[victim];
  } else {
    if (s->nguild_profiles == s->capguild_profiles) {
      size_t ncap = s->capguild_profiles ? s->capguild_profiles * 2u : 8u;
      if (ncap > SESSION_GUILD_PROFILE_CACHE_MAX) ncap = SESSION_GUILD_PROFILE_CACHE_MAX;
      session_guild_profile_cache *g = ccord_realloc_array(s->guild_profiles, ncap, sizeof *g);
      if (!g) return nullptr;
      s->guild_profiles = g;
      s->capguild_profiles = ncap;
    }
    ent = &s->guild_profiles[s->nguild_profiles++];
  }
  memset(ent, 0, sizeof *ent);
  ent->guild_id = guild_id;
  ent->last_use = ++s->cache_tick;
  return ent;
}

static void session_set_self_activity(ccord_session *s, bool has, ccord_activity_type type, const char *name,
                                      const char *details, const char *state, int64_t start_ms, int64_t end_ms)
{
  ccord_id self = ccord_store_self_id(s->store);
  const ccord_user *me = self != CCORD_ID_NONE ? ccord_store_user(s->store, self) : nullptr;
  if (!me) return;
  if (!name) name = "";
  if (!details) details = "";
  if (!state) state = "";
  has = has && name[0] != '\0';
  if (!has && !me->has_activity && (!me->status_text || me->status_text[0] == '\0')) return;
  if (has && me->has_activity && me->activity_type == type && strcmp(me->activity_name, name) == 0 &&
      strcmp(me->activity_details, details) == 0 && strcmp(me->activity_state, state) == 0 &&
      me->activity_start_ms == start_ms && me->activity_end_ms == end_ms) {
    return;
  }
  char status_buf[CCORD_ACTIVITY_TEXT_MAX_BYTES + 32];
  status_buf[0] = '\0';
  if (has) {
    const char *prefix = "";
    switch (type) {
      case CCORD_ACTIVITY_PLAYING: prefix = "Playing "; break;
      case CCORD_ACTIVITY_STREAMING: prefix = "Streaming "; break;
      case CCORD_ACTIVITY_LISTENING: prefix = "Listening to "; break;
      case CCORD_ACTIVITY_WATCHING: prefix = "Watching "; break;
      case CCORD_ACTIVITY_COMPETING: prefix = "Competing in "; break;
      default: break;
    }
    (void)snprintf(status_buf, sizeof status_buf, "%s%s", prefix, name);
  }
  char emoji[sizeof me->custom_status_emoji], text[sizeof me->custom_status_text];
  memcpy(emoji, me->custom_status_emoji, sizeof emoji);
  memcpy(text, me->custom_status_text, sizeof text);
  ccord_error err;
  if (ccord_store_update_presence_detail(s->store, self, me->presence, status_buf, emoji, text,
                                         me->custom_status_expires_ms, has, type, name, details, state, start_ms,
                                         end_ms, &err) != CCORD_OK) {
    CCORD_LOG_W("session: own activity not applied: %s", err.message);
  }
}

static void voice_track_upsert(ccord_session *s, ccord_id guild_id, ccord_id channel_id, ccord_id user_id)
{
  if (user_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < s->nvoice_track; i++) {
    if (s->voice_track[i].guild_id == guild_id && s->voice_track[i].user_id == user_id) {
      if (channel_id == CCORD_ID_NONE) {
        s->voice_track[i] = s->voice_track[--s->nvoice_track];
      } else {
        s->voice_track[i].channel_id = channel_id;
      }
      return;
    }
  }
  if (channel_id == CCORD_ID_NONE) return;
  if (s->nvoice_track == s->capvoice_track) {
    size_t ncap = s->capvoice_track ? s->capvoice_track * 2u : 16u;
    session_voice_entry *g = ccord_realloc_array(s->voice_track, ncap, sizeof *g);
    if (!g) {
      s->voice_track_synced = false;
      return;
    }
    s->voice_track = g;
    s->capvoice_track = ncap;
  }
  s->voice_track[s->nvoice_track++] = (session_voice_entry){
    .guild_id = guild_id,
    .channel_id = channel_id,
    .user_id = user_id,
  };
}

static void voice_track_remove_guild(ccord_session *s, ccord_id guild_id)
{
  for (size_t i = 0; i < s->nvoice_track; ) {
    if (s->voice_track[i].guild_id == guild_id) {
      s->voice_track[i] = s->voice_track[--s->nvoice_track];
    } else {
      i++;
    }
  }
}

static void voice_track_remove_channel(ccord_session *s, ccord_id channel_id)
{
  for (size_t i = 0; i < s->nvoice_track; ) {
    if (s->voice_track[i].channel_id == channel_id) {
      s->voice_track[i] = s->voice_track[--s->nvoice_track];
    } else {
      i++;
    }
  }
}

static size_t voice_id_slot(ccord_id id, size_t mask)
{
  uint64_t x = (uint64_t)id;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return (size_t)(x & (uint64_t)mask);
}

static bool voice_keep_lookup(const ccord_id *keep_set, size_t keep_mask,
                              const ccord_voice_state *states, size_t count,
                              ccord_id guild_id, ccord_id user_id)
{
  if (keep_set) {
    size_t b = voice_id_slot(user_id, keep_mask);
    while (keep_set[b] != CCORD_ID_NONE) {
      if (keep_set[b] == user_id) return true;
      b = (b + 1u) & keep_mask;
    }
    return false;
  }
  for (size_t k = 0; states && k < count; k++) {
    if (states[k].guild_id == guild_id && states[k].user_id == user_id &&
        states[k].channel_id != CCORD_ID_NONE) {
      return true;
    }
  }
  return false;
}

static bool push_stale_voice_id(ccord_id **buf, ccord_id **heap_buf, size_t *count, size_t *cap, ccord_id uid)
{
  if (*count == *cap) {
    size_t ncap = (*cap) * 2u;
    ccord_id *g = ccord_alloc_array(ncap, sizeof *g);
    if (!g) return false;
    memcpy(g, *buf, (*count) * sizeof *g);
    ccord_free(*heap_buf);
    *heap_buf = g;
    *buf = g;
    *cap = ncap;
  }
  (*buf)[(*count)++] = uid;
  return true;
}

static void session_apply_voice_state(ccord_session *s, const ccord_voice_state *raw_vs, const ccord_user *opt_user)
{
  if (!s || !raw_vs) return;
  ccord_error err;
  if (opt_user) {
    (void)ccord_store_upsert_user(s->store, opt_user, &err);
    ccord_error_clear(&err);
  } else if (raw_vs->channel_id != CCORD_ID_NONE && ccord_store_user(s->store, raw_vs->user_id) == nullptr) {
    char idbuf[21];
    ccord_id_format(raw_vs->user_id, idbuf);
    char namebuf[sizeof("User ") + sizeof idbuf];
    memcpy(namebuf, "User ", 5);
    size_t idlen = strlen(idbuf);
    memcpy(namebuf + 5, idbuf, idlen + 1);
    ccord_user stub = {
      .id = raw_vs->user_id,
      .name = namebuf,
      .status_text = "",
      .presence = CCORD_PRESENCE_ONLINE,
      .is_bot = false,
      .avatar_palette = (uint8_t)(raw_vs->user_id % 6u),
    };
    (void)ccord_store_upsert_user(s->store, &stub, &err);
    ccord_error_clear(&err);
  }
  // A partial gateway payload may omit suppress / request_to_speak_timestamp: keep the last known
  // stage state instead of silently clearing it (the omit flags say which fields were not sent).
  ccord_voice_state vs = *raw_vs;
  const ccord_voice_state *cur = ccord_store_voice_user(s->store, vs.guild_id, vs.user_id);
  ccord_id prev_voice_ch = cur ? cur->channel_id : CCORD_ID_NONE;
  bool was_streaming = cur ? cur->self_stream : false;
  if (vs.omit_suppress || vs.omit_request_to_speak) {
    if (cur) {
      if (vs.omit_suppress) vs.suppress = cur->suppress;
      if (vs.omit_request_to_speak) vs.request_to_speak_timestamp = cur->request_to_speak_timestamp;
    }
    vs.omit_suppress = false;
    vs.omit_request_to_speak = false;
  }
  if (ccord_store_set_voice_state(s->store, &vs, &err) != CCORD_OK) {
    if (err.code != CCORD_ERR_NOT_FOUND) {
      CCORD_LOG_W("session: voice state rejected: %s", err.message);
    }
  } else {
    voice_track_upsert(s, vs.guild_id, vs.channel_id, vs.user_id);
  }
  if (vs.user_id == ccord_store_self_id(s->store)) {
    s->pending_voice_guild = CCORD_ID_NONE;
    s->pending_voice_channel = CCORD_ID_NONE;
    if (vs.channel_id != CCORD_ID_NONE) {
      s->voice_self_mute = vs.self_mute;
      s->voice_self_deaf = vs.self_deaf;
    }
  }
  // Our own connection health belongs to one voice session: leaving or moving to another channel ends it, so a
  // later call never starts out showing "reconnecting" from the previous one.
  if (vs.user_id == ccord_store_self_id(s->store) && vs.channel_id != prev_voice_ch &&
      (vs.channel_id == CCORD_ID_NONE || prev_voice_ch != CCORD_ID_NONE)) {
    ccord_rtc_stats none = {0};
    (void)ccord_store_set_rtc_stats(s->store, &none, &err);
    ccord_error_clear(&err);
  }
  // The signed-in user left (or moved out of) a voice channel: Discord keeps a participant in an Activity until
  // told otherwise, so leave every instance there that still lists us (no ghost participant behind).
  if (prev_voice_ch != CCORD_ID_NONE && vs.channel_id != prev_voice_ch && vs.user_id == ccord_store_self_id(s->store)) {
    session_leave_channel_activities(s, prev_voice_ch);
  }
  if (prev_voice_ch != CCORD_ID_NONE && (vs.channel_id != prev_voice_ch || (was_streaming && !vs.self_stream))) {
    (void)ccord_store_stream_remove(s->store, prev_voice_ch, vs.user_id, &err);
    if (s->unsub_channel_id == prev_voice_ch && s->unsub_user_id == vs.user_id) {
      s->unsub_channel_id = CCORD_ID_NONE;
      s->unsub_user_id = CCORD_ID_NONE;
    }
  }
  if (s->watched.active && vs.user_id == s->watched.user_id && vs.guild_id == s->watched.guild_id) {
    if (vs.channel_id != s->watched.channel_id || !vs.self_stream) {
      s->watched.active = false;
      atomic_store_explicit(&s->watched_active, false, memory_order_release);
      s->watched.paused = false;
    }
  }
}

static void session_apply_voice_states(ccord_session *s, ccord_id replace_guild_id, const ccord_voice_state *states,
                                       size_t count, const ccord_user *users, size_t user_count)
{
  if (!s) return;
  ccord_error err;
  for (size_t u = 0; users && u < user_count; u++) {
    (void)ccord_store_upsert_user(s->store, &users[u], &err);
    ccord_error_clear(&err);
  }
  if (replace_guild_id != CCORD_ID_NONE && replace_guild_id != CCORD_DM_GUILD_ID) {
    ccord_id keep_stack[128];
    ccord_id *keep_heap = nullptr;
    ccord_id *keep_set = nullptr;
    size_t keep_mask = 0;
    if (states && count > 8u) {
      size_t keep_cap = 32u;
      while (keep_cap < count * 2u && keep_cap <= (SIZE_MAX >> 1)) keep_cap <<= 1u;
      if (keep_cap <= sizeof keep_stack / sizeof keep_stack[0]) {
        memset(keep_stack, 0, keep_cap * sizeof keep_stack[0]);
        keep_set = keep_stack;
      } else {
        keep_heap = ccord_calloc(keep_cap, sizeof *keep_heap);
        keep_set = keep_heap;
      }
      if (keep_set) {
        keep_mask = keep_cap - 1u;
        for (size_t k = 0; k < count; k++) {
          if (states[k].guild_id != replace_guild_id || states[k].channel_id == CCORD_ID_NONE ||
              states[k].user_id == CCORD_ID_NONE) {
            continue;
          }
          size_t b = voice_id_slot(states[k].user_id, keep_mask);
          while (keep_set[b] != CCORD_ID_NONE && keep_set[b] != states[k].user_id) {
            b = (b + 1u) & keep_mask;
          }
          keep_set[b] = states[k].user_id;
        }
      }
    }

    ccord_id stale_stack[64];
    ccord_id *stale_heap = nullptr;
    ccord_id *stale = stale_stack;
    size_t nstale = 0;
    size_t stale_cap = sizeof stale_stack / sizeof stale_stack[0];

    if (s->voice_track_synced) {
      for (size_t i = 0; i < s->nvoice_track; i++) {
        if (s->voice_track[i].guild_id != replace_guild_id) continue;
        ccord_id uid = s->voice_track[i].user_id;
        if (!voice_keep_lookup(keep_set, keep_mask, states, count, replace_guild_id, uid)) {
          if (!push_stale_voice_id(&stale, &stale_heap, &nstale, &stale_cap, uid)) break;
        }
      }
    } else {
      size_t nc = ccord_store_channel_count(s->store, replace_guild_id);
      for (size_t ci = 0; ci < nc; ci++) {
        const ccord_channel *ch = ccord_store_channel_at(s->store, replace_guild_id, ci);
        if (!ch || (ch->kind != CCORD_CHANNEL_VOICE && ch->kind != CCORD_CHANNEL_STAGE)) continue;
        for (size_t vi = 0; ; vi++) {
          const ccord_voice_state *cur = ccord_store_voice_at(s->store, ch->id, vi);
          if (!cur) break;
          if (!voice_keep_lookup(keep_set, keep_mask, states, count, replace_guild_id, cur->user_id)) {
            if (!push_stale_voice_id(&stale, &stale_heap, &nstale, &stale_cap, cur->user_id)) break;
          }
        }
      }
    }

    ccord_free(keep_heap);
    for (size_t i = 0; i < nstale; i++) {
      ccord_voice_state leave = {.guild_id = replace_guild_id, .channel_id = CCORD_ID_NONE, .user_id = stale[i]};
      session_apply_voice_state(s, &leave, nullptr);
    }
    ccord_free(stale_heap);
  }
  for (size_t i = 0; states && i < count; i++) {
    session_apply_voice_state(s, &states[i], nullptr);
  }
}

static bool ascii_strcasecmp_eq(const char *a, const char *b)
{
  if (!a || !b) return false;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a++;
    unsigned char cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
    if (ca != cb) return false;
  }
  return *a == '\0' && *b == '\0';
}

// True when the Rich Presence activities array `json` has an activity called `name` (case-insensitive).
static bool rich_json_has_name(const char *json, const char *name)
{
  if (!json || json[0] == '\0' || !name || name[0] == '\0') return false;
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  bool found = false;
  const ccord_json *arr = ccord_json_parse(&tmp, json, strlen(json), nullptr, nullptr);
  for (size_t i = 0; arr && i < ccord_json_array_len(arr) && !found; i++) {
    const ccord_json *nj = ccord_json_get(ccord_json_array_at(arr, i), "name");
    const char *n = nj ? ccord_json_string(nj, nullptr) : nullptr;
    found = n && ascii_strcasecmp_eq(n, name);
  }
  ccord_arena_free(&tmp);
  return found;
}

static bool rich_json_has_any(const char *json)
{
  if (!json || json[0] == '\0') return false;
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  const ccord_json *arr = ccord_json_parse(&tmp, json, strlen(json), nullptr, nullptr);
  bool any = arr && ccord_json_array_len(arr) > 0;
  ccord_arena_free(&tmp);
  return any;
}

static ccord_presence session_current_presence(const ccord_session *s);
static ccord_status session_send_presence_with_custom(ccord_session *s, ccord_presence presence, ccord_error *err);

static void apply_event(ccord_session *s, const ccord_net_event *ev)
{
  ccord_error err;
  switch (ev->type) {
    case CCORD_NET_STATE:
      s->state = ev->state;
      post_state(s);
      if (session_is_authenticated(s)) {
        ccord_id sel_ch = ccord_store_selected_channel(s->store);
        request_history_once(s, sel_ch);
        maybe_request_dm_profile(s, sel_ch);
      }
      if (s->state == CCORD_CONN_CONNECTED && s->rich_op3_dirty && !s->stale &&
          ccord_store_self_id(s->store) != CCORD_ID_NONE) {
        ccord_error gw_err;
        if (session_send_presence_with_custom(s, session_current_presence(s), &gw_err) == CCORD_OK) {
          s->rich_op3_dirty = false;
        }
      }
      break;
    case CCORD_NET_SNAPSHOT:
      // READY: the stale flag goes first so everything below (history, presence, profile requests) is live again.
      {
        ccord_id prev_self = ccord_store_self_id(s->store);
        bool was_stale = s->stale;
        s->stale = false;
        if (ccord_store_set_snapshot_ex(s->store, ev->snapshot, was_stale, &err) != CCORD_OK) {
          CCORD_LOG_E("session: snapshot rejected: %s", err.message);
          s->state = CCORD_CONN_FAILED;
          post_state(s);
        } else {
          ccord_id new_self = ev->snapshot ? ev->snapshot->self_id : CCORD_ID_NONE;
          if (prev_self != CCORD_ID_NONE && new_self != CCORD_ID_NONE && prev_self != new_self) {
            ccord_store_clear_all_expressions(s->store);
          }
          ccord_store_forum_reset(s->store);
        if (ev->forum_meta_count > 0 && ev->forum_metas) {
          (void)ccord_store_forum_set_metas(s->store, ev->forum_metas, ev->forum_meta_count, &err);
        }
        if (ev->forum_post_count > 0 && ev->forum_posts) {
          (void)ccord_store_forum_upsert_posts(s->store, ev->forum_posts, ev->forum_post_count, &err);
        }
        ccord_store_clear_guild_expressions(s->store);
        apply_expression_lists(s, ev, CCORD_ID_NONE);
        apply_permission_data(s, ev);
        if (ev->notify_count > 0 && ev->notify) apply_notify_rows(s, ev->notify, ev->notify_count);
        if (ev->folder_item_count > 0 && ev->folder_items) {
          (void)ccord_store_set_guild_layout(s->store, ev->folder_items, ev->folder_item_count, &err);
        }
        for (size_t i = 0; ev->scheduled_events && i < ev->scheduled_event_count; i++) {
          (void)ccord_store_upsert_scheduled_event(s->store, &ev->scheduled_events[i], &err);
        }
        s->nrequested = 0;
        s->nchan_hist = 0;
        s->nprofile_req = 0;
        s->nslash_cache = 0;
        s->nactivity_shelves = 0;
        s->nguild_profiles = 0;
        s->nvoice_track = 0;
        s->voice_track_synced = true;
        if (ev->snapshot && ev->snapshot->voice_states) {
          for (size_t i = 0; i < ev->snapshot->voice_state_count; i++) {
            const ccord_voice_state *svs = &ev->snapshot->voice_states[i];
            if (svs->channel_id != CCORD_ID_NONE) {
              voice_track_upsert(s, svs->guild_id, svs->channel_id, svs->user_id);
            }
          }
        }
        reset_invite_cache(s);
        reset_verification_cache(s);
        s->slash_choice_count = 0;
        s->npins_in_flight = 0;
        s->retried_channel = CCORD_ID_NONE;
        s->has_pending_presence = false;
        ccord_mutex_lock(s->audio_mtx);
        atomic_store_explicit(&s->has_voice_cfg, false, memory_order_release);
        ccord_mutex_unlock(s->audio_mtx);
        s->ptt_held = false;
        reset_account_lists(s);
        if (s->state != CCORD_CONN_CONNECTED) {
          s->state = CCORD_CONN_CONNECTED;
          post_state(s);
        }
        // A fresh connection starts with an empty gateway: restore the Rich Presence activities.
        session_repush_rich_presence(s);
        if (was_stale) session_replay_stale(s);
        ccord_id sel_ch = ccord_store_selected_channel(s->store);
        if (sel_ch == CCORD_ID_NONE) {
          size_t ng = ccord_store_guild_count(s->store);
          for (size_t gi = 0; gi < ng && sel_ch == CCORD_ID_NONE; gi++) {
            const ccord_guild *g = ccord_store_guild_at(s->store, gi);
            if (!g) continue;
            size_t nc = ccord_store_channel_count(s->store, g->id);
            for (size_t ci = 0; ci < nc; ci++) {
              const ccord_channel *c = ccord_store_channel_at(s->store, g->id, ci);
              if (c && c->kind == CCORD_CHANNEL_TEXT) {
                if (ccord_store_select_channel(s->store, c->id, &err) == CCORD_OK) {
                  sel_ch = c->id;
                }
                break;
              }
            }
          }
          if (sel_ch == CCORD_ID_NONE) {
            size_t ndm = ccord_store_channel_count(s->store, CCORD_DM_GUILD_ID);
            for (size_t ci = 0; ci < ndm; ci++) {
              const ccord_channel *c = ccord_store_channel_at(s->store, CCORD_DM_GUILD_ID, ci);
              if (c && c->kind == CCORD_CHANNEL_TEXT && !c->is_message_request) {
                if (ccord_store_select_channel(s->store, c->id, &err) == CCORD_OK) {
                  sel_ch = c->id;
                }
                break;
              }
            }
          }
        }
        ccord_id sel_guild = ccord_store_selected_guild(s->store);
        if (sel_guild != CCORD_ID_NONE) {
          ccord_error hook_err;
          (void)ccord_transport_select_channel(s->transport, sel_guild, sel_ch, &hook_err);
        }
        request_history_once(s, sel_ch);
        maybe_request_dm_profile(s, sel_ch);
        if (s->reading_known && s->reading_channel != CCORD_ID_NONE) {
          ccord_id self_id = ccord_store_self_id(s->store);
          if (self_id != CCORD_ID_NONE && !ccord_store_user_profile(s->store, self_id)) {
            ccord_error ignored;
            (void)ccord_session_request_profile(s, self_id, &ignored);
          }
        }
        }
      }
      break;
    case CCORD_NET_HISTORY: {
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      seed_channel_message_members(s, ev->channel_id, ev->guild_id, ev->messages, ev->message_count,
                                   ev->guild_members, ev->guild_member_count);
      clear_requested(s, ev->channel_id);
      session_channel_hist *chist = chan_hist_for(s, ev->channel_id);
      if (chist) {
        chist->in_flight_before_id = CCORD_ID_NONE;
        chist->has_more = (ev->message_count >= CCORD_SESSION_HISTORY_LIMIT);
      }
      if (ccord_store_set_history(s->store, ev->channel_id, ev->messages, ev->message_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: history rejected: %s", err.message);
      } else if (s->retried_channel == ev->channel_id) {
        s->retried_channel = CCORD_ID_NONE;
      }
      break;
    }
    case CCORD_NET_HISTORY_FAILED: {
      clear_requested(s, ev->channel_id);
      session_channel_hist *chist = find_chan_hist(s, ev->channel_id);
      if (chist) {
        chist->in_flight_before_id = CCORD_ID_NONE;
      }
      break;
    }
    case CCORD_NET_MESSAGE_CREATE:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      seed_channel_message_members(s, ev->channel_id, ev->guild_id, ev->messages, ev->message_count,
                                   ev->guild_members, ev->guild_member_count);
      for (size_t i = 0; i < ev->message_count; i++) {
        const ccord_message *in = &ev->messages[i];
        const ccord_channel *existing_ch = ccord_store_channel(s->store, in->channel_id);
        if (!existing_ch) {
          if (ccord_store_forum_apply_message(s->store, in, &err) == CCORD_OK) {
            continue;
          }
          if (err.code != CCORD_ERR_NOT_FOUND &&
              ccord_store_forum_find_post(s->store, in->channel_id) != nullptr) {
            continue;
          }
        } else if (ccord_discord_type_is_thread(existing_ch->discord_type) ||
                   (existing_ch->parent_id != CCORD_ID_NONE &&
                    ccord_store_channel_is_forum(s->store, existing_ch->parent_id))) {
          (void)ccord_store_forum_apply_message(s->store, in, &err);
        }
        if (!existing_ch &&
            (ev->guild_id == CCORD_ID_NONE || ev->guild_id == CCORD_DM_GUILD_ID) &&
            in->author_id != CCORD_ID_NONE && ccord_store_self_id(s->store) != CCORD_ID_NONE) {
          ccord_id dm_uid = (in->author_id != ccord_store_self_id(s->store)) ? in->author_id : CCORD_ID_NONE;
          const ccord_user *au = dm_uid != CCORD_ID_NONE ? ccord_store_user(s->store, dm_uid) : nullptr;
          const char *dm_name = (au && au->name && au->name[0] != '\0') ? au->name : "Direct Message";
          const char *dm_icon = (au && au->avatar_url) ? au->avatar_url : "";
          ccord_channel synth_dm = {
            .id = in->channel_id,
            .guild_id = CCORD_DM_GUILD_ID,
            .kind = CCORD_CHANNEL_TEXT,
            .name = dm_name,
            .topic = "",
            .last_message_id = ccord_message_is_ephemeral(in) ? CCORD_ID_NONE : in->id,
            .user_id = dm_uid,
            .icon_url = dm_icon,
            .discord_type = CCORD_DISCORD_CHANNEL_UNKNOWN,
          };
          ccord_error dm_err;
          (void)ccord_store_upsert_channel(s->store, &synth_dm, &dm_err);
          // The placeholder is named after the author, which is wrong for group DMs.
          // Ask for the real channel (name, icon, recipients); CHANNEL_UPSERT replaces it.
          (void)ccord_transport_request_channel(s->transport, in->channel_id, &dm_err);
        }
        // M0-3: a nonce-matched server echo confirms the local pending/failed row: drop it, then append the real
        // message below (dedup by id). A foreign nonce matches nothing and appends normally.
        if (in->nonce[0] != '\0') {
          session_local_echo le;
          if (find_local_echo_by_nonce(s, in->channel_id, in->nonce, &le)) {
            remove_local_echo(s, in->channel_id, le.id);
          }
        }
        if (ccord_store_append_message(s->store, in, &err) != CCORD_OK) {
          CCORD_LOG_W("session: message rejected: %s", err.message);
          continue;
        }
        const ccord_channel *c = ccord_store_channel(s->store, in->channel_id);
        if (c && c->guild_id != CCORD_ID_NONE && c->guild_id != CCORD_DM_GUILD_ID &&
            in->author_id != CCORD_ID_NONE && ccord_store_user(s->store, in->author_id) &&
            !ccord_store_find_member(s->store, c->guild_id, in->author_id)) {
          ccord_error m_err;
          ccord_member m = {
            .guild_id = c->guild_id,
            .user_id = in->author_id,
            .role_name = "Members",
            .role_color = 0,
            .role_rank = 255,
          };
          (void)ccord_store_upsert_guild_members(s->store, c->guild_id, &m, 1, nullptr, 0, &m_err);
        }
        // Ephemeral interaction responses exist only in local session memory: never notify, ACK, or increment unreads.
        if (ccord_message_is_ephemeral(in)) continue;
        bool is_dm = (c && c->guild_id == CCORD_DM_GUILD_ID);
        ccord_id self_id = ccord_store_self_id(s->store);
        ccord_notify_effective notify_eff = notify_effective(s, in->channel_id);
        // A mention is what Discord says it is: the user's id in the message's mention list (which includes a reply that
        // pings), or the id in the text; @everyone / @here and a held @role count unless the server's settings suppress them.
        bool direct = ccord_message_mentions_user(in, self_id);
        bool everyone = in->rich && in->rich->mention_everyone;
        bool role = message_pings_own_role(s, c ? c->guild_id : CCORD_ID_NONE, in->rich);
        bool is_mention = direct || (everyone && !notify_eff.suppress_everyone) || (role && !notify_eff.suppress_roles);
        // A blocked or ignored author never notifies and never counts as unread or a mention.
        bool from_blocked_or_ignored = in->author_id != CCORD_ID_NONE &&
            (ccord_store_relationship(s->store, in->author_id) == CCORD_REL_BLOCKED ||
             ccord_store_relationship(s->store, in->author_id) == CCORD_REL_IGNORED);
        bool suppress_notifs = (ccord_message_flags(in) & CCORD_MSGFLAG_SUPPRESS_NOTIFICATIONS) != 0;
        ccord_id reading = s->reading_known ? s->reading_channel : ccord_store_selected_channel(s->store);
        if (in->author_id != CCORD_ID_NONE && in->author_id != self_id && !(c && c->is_spam) && !from_blocked_or_ignored &&
            !suppress_notifs &&
            !(in->channel_id == reading && !is_mention) &&
            ccord_notify_should_notify(&notify_eff, is_dm || direct, everyone, role)) {
          queue_notification(s, (ccord_notification){
            .guild_id = c ? c->guild_id : CCORD_ID_NONE,
            .channel_id = in->channel_id,
            .message_id = in->id,
            .author_id = in->author_id,
            .is_dm = is_dm,
            .is_mention = is_mention,
          });
        }
        // The user's own message (typed here, or on another device) is read by definition: it moves the read position to it
        // and clears the channel. Anyone else's counts as unread unless the user is reading that channel right now.
        bool unjoined_thread = c && ccord_discord_type_is_thread(c->discord_type) && c->thread_has_member && !c->thread_joined;
        if (in->author_id != CCORD_ID_NONE && in->author_id == self_id) {
          if (c) (void)ccord_store_apply_read(s->store, in->channel_id, in->id, UINT32_MAX, &err);
        } else if (in->channel_id != reading && !from_blocked_or_ignored && !unjoined_thread) {
          if (c && !c->is_spam && !c->is_message_request) {
            bool dm_muted = is_dm && (notify_eff.muted || notify_eff.level == CCORD_NOTIFY_NOTHING);
            bool count_mention = (is_dm && !dm_muted) || is_mention;
            uint32_t unread = c->unread_count + 1u;
            uint32_t mentions = c->mention_count + (count_mention ? 1u : 0u);
            ccord_error ignored;
            (void)ccord_store_set_channel_counts(s->store, in->channel_id, unread, mentions, &ignored);
          }
        }
      }
      break;
    case CCORD_NET_MESSAGE_UPDATE:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      for (size_t i = 0; i < ev->message_count; i++) {
        // A live update can race the initial history: silently ignore uncached messages.
        if (!ccord_store_find_message(s->store, ev->messages[i].channel_id, ev->messages[i].id)) continue;
        if (ccord_store_update_message(s->store, &ev->messages[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: message update rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_MESSAGE_DELETE:
      if (ccord_store_delete_message(s->store, ev->channel_id, ev->message_id, &err) != CCORD_OK) {
        CCORD_LOG_W("session: message delete rejected: %s", err.message);
      }
      break;
    case CCORD_NET_MESSAGE_DELETE_BULK:
      if (ccord_store_delete_messages_bulk(s->store, ev->channel_id, ev->message_ids, ev->message_id_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: bulk message delete rejected: %s", err.message);
      }
      break;
    case CCORD_NET_PRESENCE_UPDATE:
      if (ev->user_id != CCORD_ID_NONE && ev->user_id == ccord_store_self_id(s->store)) {
        s->has_pending_presence = false;
      }
      if (ev->has_presence_detail) {
        if (ccord_store_update_presence_detail(
              s->store, ev->user_id, ev->presence, ev->status_text, ev->custom_status_emoji, ev->custom_status_text,
              ev->custom_status_expires_ms, ev->has_activity, ev->activity_type, ev->activity_name,
              ev->activity_details, ev->activity_state, ev->activity_start_ms, ev->activity_end_ms, &err) !=
            CCORD_OK) {
          CCORD_LOG_W("session: presence detail rejected: %s", err.message);
        }
      } else if (ccord_store_update_presence(s->store, ev->user_id, ev->presence, ev->status_text, &err) != CCORD_OK) {
        CCORD_LOG_W("session: presence rejected: %s", err.message);
      }
      break;
    case CCORD_NET_TYPING_START:
      if (ccord_store_set_typing(s->store, ev->channel_id, ev->user_id, ev->timestamp_ms, &err) != CCORD_OK) {
        CCORD_LOG_W("session: typing rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CHANNEL_UPSERT:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      if (ev->forum_meta_count > 0 && ev->forum_metas) {
        (void)ccord_store_forum_set_metas(s->store, ev->forum_metas, ev->forum_meta_count, &err);
      }
      if (ev->forum_post_count > 0 && ev->forum_posts) {
        (void)ccord_store_forum_upsert_posts(s->store, ev->forum_posts, ev->forum_post_count, &err);
      }
      if (ev->channel && ccord_discord_type_is_thread(ev->channel->discord_type) &&
          ev->channel->parent_id != CCORD_ID_NONE &&
          ccord_store_channel_is_forum(s->store, ev->channel->parent_id) &&
          ccord_store_channel(s->store, ev->channel->id) == nullptr) {
        break;
      }
      if (!ev->channel || ccord_store_upsert_channel(s->store, ev->channel, &err) != CCORD_OK) {
        CCORD_LOG_W("session: channel upsert rejected: %s", ev->channel ? err.message : "null channel");
      } else {
        if (ev->forum_meta_count > 0 && ev->forum_metas) {
          (void)ccord_store_forum_set_metas(s->store, ev->forum_metas, ev->forum_meta_count, &err);
        }
        apply_permission_data(s, ev);
        if (ev->admin_op == CCORD_ADMIN_CREATE_GROUP_DM) {
          (void)ccord_session_select_channel(s, ev->channel->id, &err);
        }
        if (ccord_discord_type_is_thread(ev->channel->discord_type) && ev->channel->parent_id != CCORD_ID_NONE &&
            !ccord_store_channel_is_forum(s->store, ev->channel->parent_id)) {
          const ccord_message *starter = ccord_store_find_message(s->store, ev->channel->parent_id, ev->channel->id);
          if (starter && (!starter->rich || starter->rich->thread.id == CCORD_ID_NONE)) {
            ccord_message_rich rpatch = {
              .present = CCORD_RICH_HAS_THREAD,
              .text_absent = true,
              .thread = {
                .id = ev->channel->id,
                .name = ev->channel->name ? ev->channel->name : "Thread",
                .message_count = ev->channel->thread_message_count,
              },
            };
            ccord_message mpatch = {
              .id = starter->id,
              .channel_id = starter->channel_id,
              .author_id = starter->author_id,
              .timestamp_ms = starter->timestamp_ms,
              .content = starter->content,
              .rich = &rpatch,
            };
            ccord_error merr;
            (void)ccord_store_update_message(s->store, &mpatch, &merr);
          }
        }
      }
      break;
    case CCORD_NET_CHANNEL_DELETE:
      (void)ccord_store_forum_remove_post(s->store, ev->channel_id, &err);
      if (ccord_store_channel(s->store, ev->channel_id)) {
        if (ccord_store_delete_channel(s->store, ev->channel_id, &err) != CCORD_OK) {
          CCORD_LOG_W("session: channel delete rejected: %s", err.message);
        } else {
          voice_track_remove_channel(s, ev->channel_id);
        }
      }
      break;
    case CCORD_NET_REACTION_UPDATE: {
      // Ignore reactions for messages that have not reached the cache yet.
      const ccord_message *rmsg = ccord_store_find_message(s->store, ev->channel_id, ev->message_id);
      if (!rmsg) break;
      if (ev->message_count > 0 && ev->messages) {
        const ccord_message *authoritative = &ev->messages[0];
        if (authoritative->reaction_kind_count == 0) {
          if (ccord_store_set_reactions(s->store, ev->channel_id, ev->message_id,
                                        authoritative->reaction_count, authoritative->self_reacted,
                                        &err) != CCORD_OK) {
            CCORD_LOG_W("session: reaction clear rejected: %s", err.message);
          }
        } else {
          // Diff against existing reactions in place to avoid clearing & reallocating unchanged emojis.
          size_t valid_auth = 0;
          for (size_t ri = 0; ri < authoritative->reaction_kind_count; ri++) {
            const ccord_reaction *ar = &authoritative->reactions[ri];
            if (ar->emoji && ar->count > 0) valid_auth++;
          }
          if (valid_auth == 0) {
            if (ccord_store_set_reactions(s->store, ev->channel_id, ev->message_id, 0, false, &err) != CCORD_OK) {
              CCORD_LOG_W("session: reaction clear rejected: %s", err.message);
            }
          } else {
            char stale_emojis[CCORD_MAX_REACTIONS_PER_MESSAGE][CCORD_EMOJI_MAX_BYTES + 1];
            size_t nstale = 0;
            for (size_t ci = 0; ci < rmsg->reaction_kind_count; ci++) {
              const char *cur_emoji = rmsg->reactions[ci].emoji;
              if (!cur_emoji) continue;
              bool kept = false;
              for (size_t ri = 0; ri < authoritative->reaction_kind_count; ri++) {
                const ccord_reaction *ar = &authoritative->reactions[ri];
                if (ar->emoji && ar->count > 0 && strcmp(cur_emoji, ar->emoji) == 0) {
                  kept = true;
                  break;
                }
              }
              if (!kept && nstale < CCORD_MAX_REACTIONS_PER_MESSAGE) {
                size_t elen = strlen(cur_emoji);
                if (elen > CCORD_EMOJI_MAX_BYTES) elen = CCORD_EMOJI_MAX_BYTES;
                memcpy(stale_emojis[nstale], cur_emoji, elen);
                stale_emojis[nstale][elen] = '\0';
                nstale++;
              }
            }
            if (nstale > 0) {
              if (nstale == rmsg->reaction_kind_count) {
                (void)ccord_store_set_reactions(s->store, ev->channel_id, ev->message_id, 0, false, &err);
              } else {
                for (size_t si = 0; si < nstale; si++) {
                  (void)ccord_store_set_reaction_ex(s->store, ev->channel_id, ev->message_id, stale_emojis[si],
                                                    0, false, 0, false, &err);
                }
              }
              rmsg = ccord_store_find_message(s->store, ev->channel_id, ev->message_id);
            }
            for (size_t ri = 0; rmsg && ri < authoritative->reaction_kind_count; ri++) {
              const ccord_reaction *ar = &authoritative->reactions[ri];
              if (!ar->emoji || ar->count == 0) continue;
              uint32_t want_burst = ar->burst_count > ar->count ? ar->count : ar->burst_count;
              bool want_me_burst = want_burst == 0 ? false : ar->me_burst;
              bool same = false;
              for (size_t ci = 0; ci < rmsg->reaction_kind_count; ci++) {
                const ccord_reaction *cr = &rmsg->reactions[ci];
                if (cr->emoji && strcmp(cr->emoji, ar->emoji) == 0) {
                  same = (cr->count == ar->count && cr->self_reacted == ar->self_reacted &&
                          cr->burst_count == want_burst && cr->me_burst == want_me_burst);
                  break;
                }
              }
              if (same) continue;
              if (ccord_store_set_reaction_ex(s->store, ev->channel_id, ev->message_id, ar->emoji,
                                              ar->count, ar->self_reacted, ar->burst_count, ar->me_burst,
                                              &err) != CCORD_OK) {
                CCORD_LOG_W("session: reaction sync rejected: %s", err.message);
              }
              rmsg = ccord_store_find_message(s->store, ev->channel_id, ev->message_id);
            }
          }
        }
      } else if (ev->reaction_delta != 0 && ev->reaction_emoji) {
        if (ccord_store_adjust_reaction_ex(s->store, ev->channel_id, ev->message_id, ev->reaction_emoji,
                                           ev->reaction_delta, ev->user_id == ccord_store_self_id(s->store),
                                           ev->reaction_burst, &err) != CCORD_OK) {
          CCORD_LOG_W("session: reaction delta rejected: %s", err.message);
        }
      } else if (ev->reaction_emoji) {
        bool self_flag = ev->self_reacted;
        if (ev->user_id != CCORD_ID_NONE && ev->user_id != ccord_store_self_id(s->store)) {
          self_flag = false;
          for (size_t ri = 0; ri < rmsg->reaction_kind_count; ri++) {
            if (rmsg->reactions[ri].emoji && strcmp(rmsg->reactions[ri].emoji, ev->reaction_emoji) == 0) {
              self_flag = rmsg->reactions[ri].self_reacted;
              break;
            }
          }
        }
        if (ccord_store_set_reaction(s->store, ev->channel_id, ev->message_id, ev->reaction_emoji,
                                     ev->reaction_count, self_flag, &err) != CCORD_OK) {
          CCORD_LOG_W("session: reaction rejected: %s", err.message);
        }
      } else if (ccord_store_set_reactions(s->store, ev->channel_id, ev->message_id, ev->reaction_count,
                                           ev->self_reacted, &err) != CCORD_OK) {
        CCORD_LOG_W("session: reaction rejected: %s", err.message);
      }
      break;
    }
    case CCORD_NET_VOICE_STATE_UPDATE:
      if (!ev->voice_state) {
        CCORD_LOG_W("session: voice state rejected: null voice state");
        break;
      }
      session_apply_voice_state(s, ev->voice_state, (ev->users && ev->user_count > 0) ? &ev->users[0] : nullptr);
      break;
    case CCORD_NET_VOICE_STATES:
      session_apply_voice_states(s, ev->replace_voice_states ? ev->guild_id : CCORD_ID_NONE, ev->voice_states,
                                 ev->voice_state_count, ev->users, ev->user_count);
      break;
    case CCORD_NET_VOICE_SERVER_UPDATE:
      if (!ev->voice_server || ccord_store_set_voice_server(s->store, ev->voice_server, &err) != CCORD_OK) {
        CCORD_LOG_W("session: voice server rejected: %s", ev->voice_server ? err.message : "null voice server");
      }
      break;
    case CCORD_NET_STREAM_UPDATE:
      if (!ev->stream || ccord_store_set_stream(s->store, ev->stream, &err) != CCORD_OK) {
        CCORD_LOG_W("session: stream rejected: %s", ev->stream ? err.message : "null stream");
      }
      break;
    case CCORD_NET_MESSAGE_PIN_UPDATE:
      if (ccord_store_set_message_pinned(s->store, ev->channel_id, ev->message_id, ev->pinned, &err) != CCORD_OK) {
        CCORD_LOG_W("session: pin rejected: %s", err.message);
      }
      break;
    case CCORD_NET_GUILD_UPSERT:
      if (!ev->guild) {
        CCORD_LOG_W("session: guild upsert without guild");
      } else if (ccord_store_upsert_guild(s->store, ev->guild, ev->guild_channels, ev->guild_channel_count,
                                          ev->guild_members, ev->guild_member_count, ev->users, ev->user_count,
                                          &err) != CCORD_OK) {
        CCORD_LOG_W("session: guild upsert rejected: %s", err.message);
      } else {
        if (ev->forum_meta_count > 0 && ev->forum_metas) {
          (void)ccord_store_forum_set_metas(s->store, ev->forum_metas, ev->forum_meta_count, &err);
        }
        if (ev->forum_post_count > 0 && ev->forum_posts) {
          (void)ccord_store_forum_upsert_posts(s->store, ev->forum_posts, ev->forum_post_count, &err);
        }
        apply_expression_lists(s, ev, ev->guild->id);
        apply_permission_data(s, ev);
        if (ev->replace_voice_states || (ev->voice_states && ev->voice_state_count > 0)) {
          session_apply_voice_states(s, ev->replace_voice_states ? ev->guild->id : CCORD_ID_NONE,
                                     ev->voice_states, ev->voice_state_count, nullptr, 0);
        }
        if (ccord_store_selected_guild(s->store) == ev->guild->id) {
          ccord_id self_id = ccord_store_self_id(s->store);
          ccord_id cur_ch = ccord_store_selected_channel(s->store);
          if (cur_ch != CCORD_ID_NONE && ccord_store_permissions_exact(s->store, ev->guild->id, self_id) &&
              (ccord_store_permissions(s->store, ev->guild->id, cur_ch, self_id, session_now_unix_ms(s)) & CCORD_PERM_VIEW_CHANNEL) == 0) {
            size_t nc = ccord_store_channel_count(s->store, ev->guild->id);
            for (size_t ci = 0; ci < nc; ci++) {
              const ccord_channel *cand = ccord_store_channel_at(s->store, ev->guild->id, ci);
              if (cand && cand->kind == CCORD_CHANNEL_TEXT &&
                  (ccord_store_permissions(s->store, ev->guild->id, cand->id, self_id, session_now_unix_ms(s)) & CCORD_PERM_VIEW_CHANNEL) != 0) {
                (void)ccord_session_select_channel(s, cand->id, &err);
                break;
              }
            }
          }
        }
        if (ev->scheduled_event_count > 0 && ev->scheduled_events) {
          (void)ccord_store_set_scheduled_events(s->store, ev->guild->id, ev->scheduled_events,
                                                 ev->scheduled_event_count, &err);
        }
        if (ev->admin_op == CCORD_ADMIN_CREATE_GUILD ||
            (s->pending_join_guild_id != CCORD_ID_NONE && s->pending_join_guild_id == ev->guild->id)) {
          ccord_id join_ch = (s->pending_join_guild_id == ev->guild->id) ? s->pending_join_channel_id : CCORD_ID_NONE;
          s->pending_join_guild_id = CCORD_ID_NONE;
          s->pending_join_channel_id = CCORD_ID_NONE;
          const ccord_channel *jch = join_ch != CCORD_ID_NONE ? ccord_store_channel(s->store, join_ch) : nullptr;
          if (jch && jch->guild_id == ev->guild->id && jch->kind == CCORD_CHANNEL_TEXT) {
            (void)ccord_session_select_channel(s, join_ch, &err);
          } else {
            (void)ccord_session_select_guild(s, ev->guild->id, &err);
          }
        }
        request_history_once(s, ccord_store_selected_channel(s->store));
      }
      break;
    case CCORD_NET_GUILD_DELETE:
      if (ccord_store_delete_guild(s->store, ev->guild_id, &err) != CCORD_OK) {
        CCORD_LOG_W("session: guild delete rejected: %s", err.message);
      } else {
        voice_track_remove_guild(s, ev->guild_id);
      }
      break;
    case CCORD_NET_MEMBER_REMOVE:
      // NOT_FOUND is normal: the member list is lazy, so we may never have loaded this member.
      if (ccord_store_remove_member(s->store, ev->guild_id, ev->user_id, &err) != CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: member remove rejected: %s", err.message);
      }
      break;
    case CCORD_NET_GUILD_MEMBERS:
      if (ccord_store_upsert_guild_members(s->store, ev->guild_id, ev->guild_members, ev->guild_member_count,
                                           ev->users, ev->user_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: guild members rejected: %s", err.message);
      } else {
        apply_permission_data(s, ev);
      }
      // A member list update says how big the list is and how big its groups are; that is what the interface prints on the
      // group headers and what tells it whether more of the list is still to come.
      if (ev->member_total > 0 || ev->member_group_count > 0) {
        if (ccord_store_set_member_list_size(s->store, ev->guild_id, ev->member_total, ev->member_online, ev->member_groups,
                                             ev->member_group_count, &err) != CCORD_OK) {
          CCORD_LOG_W("session: member list size rejected: %s", err.message);
        }
      }
      // The piece that was asked for has arrived when a SYNC reaches the end of what was asked: not just any update of the
      // list (a presence change inside a range already held is one too). If it added nobody, the list has no more.
      if (s->paging_wait && s->paging_guild == ev->guild_id && ev->member_synced_to >= s->paging_chunks * CCORD_MEMBER_CHUNK) {
        s->paging_wait = false;
        if (ccord_store_member_count(s->store, ev->guild_id) <= s->paging_held) s->paging_done = true;
      }
      break;
    case CCORD_NET_ROLE_UPSERT:
      // One role (create / edit) or several (a reorder, or a new role that pushed the others up).
      for (size_t i = 0; i < ev->role_count; i++) {
        if (ccord_store_upsert_role(s->store, &ev->roles[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: role upsert rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_MEMBER_ROLE_CHANGE:
      // NOT_FOUND is normal: this member's roles may not be known (lazy member list); the gateway update fills them in.
      if (ccord_store_apply_member_role(s->store, ev->guild_id, ev->user_id, ev->role_id, ev->role_added, &err) != CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: member role change rejected: %s", err.message);
      }
      break;
    case CCORD_NET_BAN_LIST:
      if (ccord_store_set_bans(s->store, ev->guild_id, ev->bans, ev->ban_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: ban list rejected: %s", err.message);
      }
      break;
    case CCORD_NET_BAN_REMOVE:
      (void)ccord_store_remove_ban(s->store, ev->guild_id, ev->user_id);
      break;
    case CCORD_NET_INVITE_LIST:
      if (ccord_store_set_invites(s->store, ev->guild_id, ev->invites, ev->invite_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: invite list rejected: %s", err.message);
      }
      break;
    case CCORD_NET_INVITE_UPSERT:
      for (size_t i = 0; i < ev->invite_count; i++) {
        if (ccord_store_upsert_invite(s->store, ev->guild_id, &ev->invites[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: invite rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_INVITE_DELETE:
      (void)ccord_store_remove_invite(s->store, ev->guild_id, ev->invite_code);
      break;
    case CCORD_NET_NOTIFY_SETTINGS: {
      ccord_error nerr;
      if (ccord_store_set_notify(s->store, ev->guild_id, ev->notify, ev->notify_count, &nerr) != CCORD_OK) {
        CCORD_LOG_W("session: notification settings rejected: %s", nerr.message);
      }
      break;
    }
    case CCORD_NET_AUDIT_LOG:
      if (ccord_store_set_audit(s->store, ev->guild_id, ev->audit, ev->audit_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: audit log rejected: %s", err.message);
      }
      break;
    case CCORD_NET_ROLE_DELETE:
      // NOT_FOUND is normal: a role we never saw (the table arrives with the guild) cannot be removed.
      if (ccord_store_remove_role(s->store, ev->guild_id, ev->role_id, &err) != CCORD_OK && err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: role delete rejected: %s", err.message);
      }
      break;
    case CCORD_NET_PIN_LIST:
      pins_clear_in_flight(s, ev->channel_id);
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      seed_channel_message_members(s, ev->channel_id, ev->guild_id, ev->messages, ev->message_count,
                                   ev->guild_members, ev->guild_member_count);
      if (ccord_store_set_pins_page(s->store, ev->channel_id, ev->messages, ev->message_count,
                                    ev->pins_append, ev->pins_has_more, ev->pins_next_cursor, &err) != CCORD_OK) {
        CCORD_LOG_W("session: pins rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CHANNEL_PINS_UPDATE:
      if (session_is_authenticated(s) && ccord_store_history_loaded(s->store, ev->channel_id)) {
        (void)ccord_transport_request_pins(s->transport, ev->channel_id, 50u, &err);
      }
      break;
    case CCORD_NET_MESSAGE_ACK: {
      // Another session read up to a message: what is newer than it stays unread.
      const ccord_channel *ac = ccord_store_channel(s->store, ev->channel_id);
      if (ac) {
        ccord_id upto = ev->message_id != CCORD_ID_NONE ? ev->message_id : ac->last_message_id;
        if (ac->unread_count == 0 && ac->mention_count == 0 && ev->mention_count == 0 &&
            ac->last_message_id > upto) {
          upto = ac->last_message_id;
        }
        if (ccord_store_apply_read(s->store, ev->channel_id, upto, ev->mention_count, &err) != CCORD_OK) {
          CCORD_LOG_W("session: message ack rejected: %s", err.message);
        }
      }
      break;
    }
    case CCORD_NET_RELATIONSHIP_UPDATE:
      if (ccord_store_set_relationship(s->store, ev->user_id, ev->relationship_kind, &err) != CCORD_OK) {
        CCORD_LOG_W("session: relationship update rejected: %s", err.message);
      }
      break;
    case CCORD_NET_MEDIA_LOADED:
      store_media_loaded(s, ev);
      break;
    case CCORD_NET_MEDIA_RANGE:
      ccord_stream_hub_on_range(s->streams, ev, ccord_clock_ms());
      break;
    case CCORD_NET_SEARCH_RESULTS:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      store_search_results(s, ev);
      break;
    case CCORD_NET_USER_PROFILE:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      if (ev->user_profile) {
        if (ccord_store_upsert_user_profile(s->store, ev->user_profile, &err) != CCORD_OK) {
          CCORD_LOG_W("session: user profile rejected: %s", err.message);
        } else {
          mark_profile_loaded(s, ev->user_profile->user_id);
        }
      }
      break;
    case CCORD_NET_GUILD_EXPRESSIONS: {
      ccord_net_event e = *ev;
      if (!e.replace_emojis) e.emoji_count = 0;
      if (!e.replace_stickers) e.sticker_count = 0;
      if (!e.replace_soundboard) e.soundboard_count = 0;
      apply_expression_lists(s, &e, ev->guild_id);
      break;
    }
    case CCORD_NET_EXPRESSION_DATA: {
      bool expr_ok = ev->status == CCORD_OK &&
                     (ev->expr_body != nullptr ||
                      (ev->expr_kind == CCORD_EXPR_SETTINGS_PROTO && ev->raw_proto && ev->raw_proto_len > 0));
      const char *blob_body = ev->expr_body ? ev->expr_body : (expr_ok ? "{}" : nullptr);
      size_t blob_len = ev->expr_body ? ev->expr_body_len : (expr_ok ? 2u : 0u);
      if (ccord_store_set_expr_blob(s->store, ev->expr_kind, ev->expr_arg, blob_body, blob_len, expr_ok, &err) !=
          CCORD_OK) {
        CCORD_LOG_W("session: expression data rejected: %s", err.message);
      } else if (ev->expr_kind == CCORD_EXPR_SETTINGS_PROTO && expr_ok) {
        cache_settings_proto(s, ev);
        ccord_user_expressions ux;
        ccord_status ux_st =
          (ev->raw_proto && ev->raw_proto_len > 0)
            ? ccord_user_expressions_decode(ev->raw_proto, ev->raw_proto_len, &ux, &err)
            : ccord_user_expressions_from_settings_json(ev->expr_body, ev->expr_body_len, &ux, &err);
        if (ux_st == CCORD_OK) {
          apply_favorite_gifs_wire(s, &ux, true);
          for (size_t i = 0; i < ux.favorite_emoji_count; i++) {
            if (ux.favorite_emojis[i] && !ccord_store_is_favorite_emoji(s->store, ux.favorite_emojis[i])) {
              (void)ccord_store_toggle_favorite_emoji(s->store, ux.favorite_emojis[i]);
            }
          }
          for (size_t i = 0; i < ux.emoji_frecency_count; i++) {
            ccord_store_seed_emoji_frecency(s->store, ux.emoji_frecency[i].key, ux.emoji_frecency[i].score);
          }
          for (size_t i = 0; i < ux.favorite_sticker_count; i++) {
            if (!ccord_store_is_favorite_sticker(s->store, ux.favorite_stickers[i])) {
              (void)ccord_store_toggle_favorite_sticker(s->store, ux.favorite_stickers[i]);
            }
          }
          for (size_t i = 0; i < ux.sticker_frecency_count; i++) {
            ccord_store_seed_sticker_frecency(s->store, ux.sticker_frecency[i].id, ux.sticker_frecency[i].score);
          }
          ccord_user_expressions_free(&ux);
        }
      }
      break;
    }
    case CCORD_NET_STREAM_SIGNAL:
      if (ev->stream_signal) {
        const ccord_stream_signal *sig = ev->stream_signal;
        ccord_id self_id = ccord_store_self_id(s->store);
        // Go Live broadcast cache: the server's signals are the only source (never a local click).
        // SERVER_UPDATE carries media credentials for the transport, not a list row.
        if (sig->channel_id != CCORD_ID_NONE && sig->user_id != CCORD_ID_NONE && !sig->server_update) {
          if (sig->kind == CCORD_STREAM_SIG_CREATE && !sig->unavailable) {
            ccord_stream_state bst = {
              .guild_id = sig->guild_id,
              .channel_id = sig->channel_id,
              .user_id = sig->user_id,
              .active = true,
              .paused = sig->paused,
              .config = (sig->user_id == self_id) ? s->pending_stream_cfg : (ccord_stream_config){0},
            };
            if (ccord_store_stream_upsert(s->store, &bst, sig->viewer_count, &err) != CCORD_OK) {
              CCORD_LOG_W("session: stream signal rejected: %s", err.message);
            }
          } else if (sig->kind == CCORD_STREAM_SIG_UPDATE) {
            // Viewer-count / pause change for a broadcast we must already know (NOT_FOUND before CREATE).
            ccord_status vst = ccord_store_stream_set_viewers(s->store, sig->channel_id, sig->user_id,
                                                              sig->viewer_count, &err);
            if (vst != CCORD_OK && vst != CCORD_ERR_NOT_FOUND) {
              CCORD_LOG_W("session: stream viewers rejected: %s", err.message);
            }
            const ccord_stream_state *existing = ccord_store_stream_find(s->store, sig->channel_id, sig->user_id);
            if (existing && existing->paused != sig->paused) {
              ccord_stream_state upd = *existing;
              upd.paused = sig->paused;
              (void)ccord_store_stream_upsert(s->store, &upd, sig->viewer_count, &err);
            }
          } else if (sig->kind == CCORD_STREAM_SIG_DELETE || sig->unavailable) {
            bool keep_peer_row = false;
            if (!sig->unavailable && sig->channel_id == s->unsub_channel_id && sig->user_id == s->unsub_user_id) {
              s->unsub_channel_id = CCORD_ID_NONE;
              s->unsub_user_id = CCORD_ID_NONE;
              const char *r = sig->reason ? sig->reason : "";
              bool explicit_end = strncmp(r, "broadcaster", 11) == 0 || strcmp(r, "stream_ended") == 0 ||
                                  strcmp(r, "stream_full") == 0 || strcmp(r, "unauthorized") == 0;
              if (strcmp(r, "viewer stopped watching") == 0) {
                keep_peer_row = true;
              } else if (!explicit_end) {
                ccord_id vguild = (sig->is_call || sig->guild_id == CCORD_ID_NONE) ? CCORD_DM_GUILD_ID : sig->guild_id;
                const ccord_voice_state *bvs = ccord_store_voice_user(s->store, vguild, sig->user_id);
                if (bvs && bvs->channel_id == sig->channel_id && bvs->self_stream) keep_peer_row = true;
              }
            }
            if (!keep_peer_row) {
              (void)ccord_store_stream_remove(s->store, sig->channel_id, sig->user_id, &err);
            }
          }
        }
        if (sig->user_id != CCORD_ID_NONE && sig->user_id == self_id && !sig->server_update) {
          const ccord_stream_state *prev = ccord_store_stream(s->store);
          bool paused = sig->kind == CCORD_STREAM_SIG_UPDATE ? sig->paused
                                                             : (sig->paused || (prev && prev->active && prev->paused));
          if (sig->deleted) paused = false;
          ccord_stream_state st = {
            .guild_id = sig->guild_id,
            .channel_id = sig->channel_id,
            .user_id = sig->user_id,
            .active = !sig->deleted,
            .paused = paused,
            .config = s->pending_stream_cfg,
          };
          (void)ccord_store_set_stream(s->store, &st, &err);
        }
        if (s->watched.active && sig->user_id == s->watched.user_id &&
            (sig->channel_id == CCORD_ID_NONE || sig->channel_id == s->watched.channel_id)) {
          if (sig->deleted || sig->unavailable) {
            s->watched.active = false;
            atomic_store_explicit(&s->watched_active, false, memory_order_release);
            s->watched.paused = false;
          } else if (!sig->server_update) {
            s->watched.paused = sig->paused;
          }
        }
      }
      break;
    case CCORD_NET_FORUM_POSTS:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      if (ev->forum_post_count > 0 && ev->forum_posts) {
        if (ccord_store_forum_upsert_posts(s->store, ev->forum_posts, ev->forum_post_count, &err) != CCORD_OK) {
          CCORD_LOG_W("session: forum posts rejected: %s", err.message);
        }
      }
      if (ev->channel_id != CCORD_ID_NONE && ccord_store_channel_is_forum(s->store, ev->channel_id)) {
        const ccord_forum_state *prev = ccord_store_forum_state(s->store, ev->channel_id);
        ccord_forum_state fst = prev ? *prev : (ccord_forum_state){0};
        fst.loaded = true;
        fst.loading = false;
        fst.has_more = ev->forum_has_more;
        fst.next_offset = ev->forum_offset + (uint32_t)ev->forum_post_count;
        if (ev->forum_total > 0 || ev->forum_offset == 0) fst.total = ev->forum_total;
        fst.error[0] = '\0';
        (void)ccord_store_forum_set_state(s->store, ev->channel_id, &fst, &err);
      }
      break;
    case CCORD_NET_FORUM_POST_CREATED:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      if (ev->forum_post_count > 0 && ev->forum_posts) {
        if (ccord_store_forum_upsert_posts(s->store, ev->forum_posts, ev->forum_post_count, &err) != CCORD_OK) {
          CCORD_LOG_W("session: created forum post rejected: %s", err.message);
        }
      }
      if (ev->channel_id != CCORD_ID_NONE && ccord_store_channel_is_forum(s->store, ev->channel_id)) {
        const ccord_forum_state *prev = ccord_store_forum_state(s->store, ev->channel_id);
        ccord_forum_state fst = prev ? *prev : (ccord_forum_state){0};
        fst.creating = false;
        fst.error[0] = '\0';
        if (fst.total > 0) fst.total++;
        (void)ccord_store_forum_set_state(s->store, ev->channel_id, &fst, &err);
      }
      if (ev->forum_post_count > 0 && ev->forum_posts && ev->forum_posts[0].thread_id != CCORD_ID_NONE) {
        ccord_id tid = ev->forum_posts[0].thread_id;
        if (ccord_store_forum_open_post(s->store, tid, &err) == CCORD_OK) {
          if (ev->message_count > 0 && ev->messages) {
            seed_channel_message_members(s, tid, ev->guild_id, ev->messages, ev->message_count,
                                         ev->guild_members, ev->guild_member_count);
            (void)ccord_store_set_history(s->store, tid, ev->messages, ev->message_count, &err);
            session_channel_hist *chist = chan_hist_for(s, tid);
            if (chist) {
              chist->in_flight_before_id = CCORD_ID_NONE;
              chist->has_more = false;
            }
          } else {
            request_history_once(s, tid);
          }
        }
      }
      break;
    case CCORD_NET_FORUM_FAILED:
      if (ev->channel_id != CCORD_ID_NONE && ccord_store_channel_is_forum(s->store, ev->channel_id)) {
        const ccord_forum_state *prev = ccord_store_forum_state(s->store, ev->channel_id);
        ccord_forum_state fst = prev ? *prev : (ccord_forum_state){0};
        if (ev->forum_creating) fst.creating = false;
        else fst.loading = false;
        const char *msg = (ev->text && ev->text[0]) ? ev->text : ccord_status_name(ev->status);
        size_t mlen = strlen(msg);
        if (mlen >= sizeof fst.error) mlen = sizeof fst.error - 1;
        memcpy(fst.error, msg, mlen);
        fst.error[mlen] = '\0';
        (void)ccord_store_forum_set_state(s->store, ev->channel_id, &fst, &err);
      }
      break;
    case CCORD_NET_ERROR:
      CCORD_LOG_W("session: transport error %s: %s", ccord_status_name(ev->status), ev->text ? ev->text : "");
      if (ev->user_id != CCORD_ID_NONE) {
        clear_profile_requested(s, ev->user_id);
      }
      if (ev->channel_id == CCORD_ID_NONE && ev->user_id == CCORD_ID_NONE) {
        s->discovery_loading = false;
      }
      if (ev->channel_id != CCORD_ID_NONE) {
        clear_requested(s, ev->channel_id);
        pins_clear_in_flight(s, ev->channel_id);
        if (s->react_users_loading && s->react_users_channel_id == ev->channel_id) {
          s->react_users_loading = false;
        }
        session_channel_hist *chist = find_chan_hist(s, ev->channel_id);
        if (chist) {
          chist->in_flight_before_id = CCORD_ID_NONE;
        }
        if (ev->channel_id == ccord_store_selected_channel(s->store) &&
            !ccord_store_history_loaded(s->store, ev->channel_id) &&
            s->retried_channel != ev->channel_id) {
          s->retried_channel = ev->channel_id;
          request_history_once(s, ev->channel_id);
        }
      }
      break;
    case CCORD_NET_SPEAKING:
      ccord_store_set_speaking(s->store, ev->user_id, ev->speaking);
      break;
    case CCORD_NET_VOICE_LINK: {
      // The call stays joined while the transport reconnects: only the connection state changes. A stale event
      // after the user hung up finds no active channel and is ignored.
      if (ccord_store_active_voice_channel(s->store) == CCORD_ID_NONE) break;
      ccord_rtc_stats rtc;
      ccord_store_rtc_stats(s->store, &rtc);
      rtc.state = ev->voice_link == CCORD_VLINK_CONNECTED ? CCORD_VOICE_ESTABLISHED
                : ev->voice_link == CCORD_VLINK_RECONNECTING ? CCORD_VOICE_RECONNECTING : CCORD_VOICE_FAILED;
      (void)ccord_store_set_rtc_stats(s->store, &rtc, &err);
      ccord_error_clear(&err);
      break;
    }
    case CCORD_NET_ACTION_FAILED:
      if (ev->action == CCORD_ACTION_ADMIN &&
          (ev->admin_op == CCORD_ADMIN_PREVIEW_INVITE || ev->admin_op == CCORD_ADMIN_ACCEPT_INVITE)) {
        s->invite_preview_loading = false;
        // A failed preview must not keep the in-flight guard closed: the next ask retries immediately.
        if (s->invite_cache) {
          for (size_t i = 0; i < SESSION_INVITE_CACHE_SLOTS; i++) {
            if (s->invite_cache[i].sent_ms != 0 && ev->admin_op == CCORD_ADMIN_PREVIEW_INVITE) {
              s->invite_cache[i].failed = true;
            }
            s->invite_cache[i].sent_ms = 0;
          }
        }
      }
      if (ev->action == CCORD_ACTION_ADMIN && ev->admin_op == CCORD_ADMIN_GET_GUILD_PROFILE) {
        for (size_t i = 0; i < s->nguild_profiles; i++) {
          if (ev->guild_id == CCORD_ID_NONE || s->guild_profiles[i].guild_id == ev->guild_id) {
            s->guild_profiles[i].loading = false;
            s->guild_profiles[i].sent_ms = 0;
          }
        }
      }
      if (ev->action == CCORD_ACTION_ADMIN && ev->admin_op == CCORD_ADMIN_AUTH_SESSIONS_LIST) {
        s->auth_sessions_state = CCORD_FETCH_FAILED;   // the old list stays; the guard reopens for a retry
        s->auth_sessions_sent_ms = 0;
      } else if (ev->action == CCORD_ACTION_ADMIN && ev->admin_op == CCORD_ADMIN_CONNECTIONS_LIST) {
        s->connections_state = CCORD_FETCH_FAILED;
        s->connections_sent_ms = 0;
      } else if (ev->action == CCORD_ACTION_ADMIN && ev->admin_op == CCORD_ADMIN_ACTIVITY_SHELF) {
        for (size_t i = 0; i < s->nactivity_shelves; i++) {
          if (s->activity_shelves[i].guild_id == ev->guild_id) {
            s->activity_shelves[i].loading = false;   // the cached shelf stays; the next ask retries
            s->activity_shelves[i].sent_ms = 0;
          }
        }
      }
      // M0-3: a refused send with a nonce fails exactly its pending echo; the failed bubble keeps the draft, so
      // record_failure must not also queue it (the composer would restore a second copy). Without a matching echo
      // (legacy send, or history replaced meanwhile) the failure keeps the legacy draft-restore ring.
      if (ev->action == CCORD_ACTION_SEND && ev->action_nonce[0] != '\0') {
        session_local_echo le;
        if (find_local_echo_by_nonce(s, ev->channel_id, ev->action_nonce, &le) &&
            set_local_echo_state(s, ev->channel_id, &le, CCORD_MSG_SEND_FAILED)) {
          break;
        }
      }
      if (ev->action == CCORD_ACTION_VOICE) {
        s->pending_voice_guild = CCORD_ID_NONE;
        s->pending_voice_channel = CCORD_ID_NONE;
      }
      record_failure(s, ev);
      break;
    case CCORD_NET_POLL_VOTE: {
      bool is_self = ev->user_id != CCORD_ID_NONE && ev->user_id == ccord_store_self_id(s->store);
      if (ccord_store_apply_poll_vote(s->store, ev->channel_id, ev->message_id, ev->poll_answer_id,
                                      is_self, ev->poll_vote_added, &err) != CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: poll vote rejected: %s", err.message);
      }
      break;
    }
    case CCORD_NET_INTERACTION_MODAL:
      if (ev->modal) {
        if (!s->modals) {
          s->modals = ccord_calloc(SESSION_MAX_MODALS, sizeof *s->modals);
          if (!s->modals) {
            CCORD_LOG_W("session: out of memory queuing interaction modal");
            break;
          }
        }
        if (s->modal_count == SESSION_MAX_MODALS) {
          s->modal_head = (s->modal_head + 1u) % SESSION_MAX_MODALS;
          s->modal_count--;
        }
        size_t idx = (s->modal_head + s->modal_count) % SESSION_MAX_MODALS;
        s->modals[idx] = *ev->modal;
        s->modal_count++;
      }
      break;
    case CCORD_NET_SLASH_COMMANDS: {
      ccord_id gid = ev->guild_id;
      if (gid == CCORD_ID_NONE && ev->channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, ev->channel_id);
        gid = ch ? ch->guild_id : CCORD_DM_GUILD_ID;
      }
      if (gid == CCORD_ID_NONE) gid = CCORD_DM_GUILD_ID;
      if (ccord_store_set_slash_commands(s->store, gid, ev->slash_commands, ev->slash_command_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: slash commands rejected: %s", err.message);
      } else {
        session_slash_cache *ent = slash_cache_for(s, gid);
        if (ent) {
          ent->in_flight = false;
          ent->loaded = true;
          ent->fetched_ms = ent->sent_ms ? ent->sent_ms : 1u;
        }
      }
      break;
    }
    case CCORD_NET_SLASH_AUTOCOMPLETE: {
      size_t n = ev->slash_choice_count;
      if (n > CCORD_SLASH_MAX_CHOICES) n = CCORD_SLASH_MAX_CHOICES;
      if (n > 0 && ev->slash_choices) {
        memcpy(s->slash_choices, ev->slash_choices, n * sizeof s->slash_choices[0]);
        s->slash_choice_count = n;
      } else {
        s->slash_choice_count = 0;
      }
      s->slash_choice_seq++;
      break;
    }
    case CCORD_NET_REACTION_USERS:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      store_reaction_users(s, ev);
      break;
    case CCORD_NET_THREAD_MEMBER_SELF:
      if (ev->channel_id != CCORD_ID_NONE &&
          (ev->user_id == CCORD_ID_NONE || ev->user_id == ccord_store_self_id(s->store))) {
        if (ccord_store_set_thread_joined(s->store, ev->channel_id, ev->thread_joined, &err) != CCORD_OK &&
            err.code != CCORD_ERR_NOT_FOUND) {
          CCORD_LOG_W("session: thread member update rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_GUILD_LAYOUT:
      if (ccord_store_set_guild_layout(s->store, ev->folder_items, ev->folder_item_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: guild layout rejected: %s", err.message);
      }
      session_apply_status_proto(s, ev);   // the same update may carry the status too
      break;
    case CCORD_NET_USER_SETTINGS_PROTO:
      if (ev->raw_proto && ev->raw_proto_len > 0) session_apply_frecency_update(s, ev);   // type 2
      else session_apply_status_proto(s, ev);
      break;
    case CCORD_NET_SESSIONS_REPLACE: {
      // Discord never sends a user its own PRESENCE_UPDATE: the status it is shown with comes from the session list.
      // Display only: op 3 is not re-sent (that would turn another client's status into this session's).
      ccord_id self = ccord_store_self_id(s->store);
      const ccord_user *me = self != CCORD_ID_NONE ? ccord_store_user(s->store, self) : nullptr;
      if (me && me->presence != ev->presence) {
        const char *st_keep = (ev->has_presence_detail && !ev->has_activity)
                                ? ""
                                : (me->status_text ? me->status_text : "");
        char *keep = ccord_strdup(st_keep);
        if (ccord_store_update_presence(s->store, self, ev->presence, keep, &err) != CCORD_OK) {
          CCORD_LOG_W("session: own status from sessions not applied: %s", err.message);
        }
        ccord_free(keep);
      }
      // The sessions list also says what this account is playing: an activity another session or the RPC client
      // stopped is gone here too.
      if (ev->has_presence_detail) {
        if (!ev->has_activity) s->self_activity_from_rich = false;
        session_set_self_activity(s, ev->has_activity, ev->activity_type, ev->activity_name, ev->activity_details,
                                  ev->activity_state, ev->activity_start_ms, ev->activity_end_ms);
      }
      break;
    }
    case CCORD_NET_CHANNEL_POSITIONS:
      if (ccord_store_reorder_channels(s->store, ev->guild_id, ev->reorder_ids, ev->reorder_positions,
                                       ev->reorder_parents, ev->reorder_has_parent, ev->reorder_count, &err) !=
          CCORD_OK) {
        CCORD_LOG_W("session: channel reorder rejected: %s", err.message);
      }
      break;
    case CCORD_NET_MEMBER_VERIFICATION:
      if (ev->member_verification && ev->guild_id != CCORD_ID_NONE) {
        session_verification_cache *ent = verification_cache_for(s, ev->guild_id);
        if (!ent || !cache_verification_form(ent, ev->member_verification)) {
          CCORD_LOG_W("session: verification form not cached: out of memory");
        }
      }
      break;
    case CCORD_NET_INVITE_PREVIEW:
      s->invite_preview_loading = false;
      if (ev->invite_preview) {
        s->invite_preview = *ev->invite_preview;
        s->has_invite_preview = true;
        session_invite_cache *entry = invite_cache_for(s, ev->invite_preview->code);
        if (entry) {
          uint64_t fms = entry->sent_ms ? entry->sent_ms : ccord_clock_ms();
          entry->fetched_ms = fms ? fms : 1u;
          entry->sent_ms = 0;
          entry->failed = false;
          entry->preview = *ev->invite_preview;
        }
      } else if (s->invite_cache) {
        for (size_t i = 0; i < SESSION_INVITE_CACHE_SLOTS; i++) s->invite_cache[i].sent_ms = 0;
      }
      break;
    case CCORD_NET_INVITE_ACCEPTED: {
      s->invite_preview_loading = false;
      s->has_invite_preview = false;
      ccord_id gid = ev->guild_id != CCORD_ID_NONE
                       ? ev->guild_id
                       : (ev->invite_preview ? ev->invite_preview->guild_id : CCORD_ID_NONE);
      ccord_id cid = ev->channel_id != CCORD_ID_NONE
                       ? ev->channel_id
                       : (ev->invite_preview ? ev->invite_preview->channel_id : CCORD_ID_NONE);
      if (gid != CCORD_ID_NONE) {
        if (ccord_store_guild(s->store, gid)) {
          const ccord_channel *jch = cid != CCORD_ID_NONE ? ccord_store_channel(s->store, cid) : nullptr;
          if (jch && jch->guild_id == gid && jch->kind == CCORD_CHANNEL_TEXT) {
            (void)ccord_session_select_channel(s, cid, &err);
          } else {
            (void)ccord_session_select_guild(s, gid, &err);
          }
        } else {
          s->pending_join_guild_id = gid;
          s->pending_join_channel_id = cid;
        }
      }
      break;
    }
    case CCORD_NET_DISCOVERY_RESULTS: {
      s->discovery_loading = false;
      size_t n = ev->discovery_count;
      if (n > CCORD_DISCOVERY_PAGE_MAX) n = CCORD_DISCOVERY_PAGE_MAX;
      if (n > 0 && ev->discovery_guilds) {
        if (!s->discovery_items) {
          s->discovery_items = ccord_calloc(CCORD_DISCOVERY_PAGE_MAX, sizeof *s->discovery_items);
        }
        if (s->discovery_items) {
          memcpy(s->discovery_items, ev->discovery_guilds, n * sizeof s->discovery_items[0]);
        } else {
          n = 0;
        }
      }
      s->discovery_count = n;
      s->discovery_total = ev->discovery_total >= n ? ev->discovery_total : (uint32_t)n;
      s->discovery_offset = ev->discovery_offset;
      s->discovery_category_id = ev->discovery_category_id;
      const char *dq = ev->discovery_query ? ev->discovery_query : "";
      size_t dqlen = strlen(dq);
      if (dqlen >= sizeof s->discovery_query) dqlen = sizeof s->discovery_query - 1u;
      memcpy(s->discovery_query, dq, dqlen);
      s->discovery_query[dqlen] = '\0';

      if (!s->discovery_cache) {
        s->discovery_cache = ccord_calloc(SESSION_DISCOVERY_CACHE_SLOTS, sizeof *s->discovery_cache);
      }
      if (s->discovery_cache) {
        size_t slot = SIZE_MAX;
        for (size_t i = 0; i < SESSION_DISCOVERY_CACHE_SLOTS; i++) {
          if (s->discovery_cache[i].valid &&
              s->discovery_cache[i].category_id == ev->discovery_category_id &&
              s->discovery_cache[i].offset == ev->discovery_offset &&
              strcmp(s->discovery_cache[i].query, s->discovery_query) == 0) {
            slot = i;
            break;
          }
        }
        if (slot == SIZE_MAX) {
          slot = s->discovery_cache_rr % SESSION_DISCOVERY_CACHE_SLOTS;
          s->discovery_cache_rr++;
        }
        session_discovery_cache_entry *ce = &s->discovery_cache[slot];
        ce->valid = true;
        uint64_t fms = s->discovery_sent_ms ? s->discovery_sent_ms : ccord_clock_ms();
        ce->fetched_ms = fms ? fms : 1u;
        memcpy(ce->query, s->discovery_query, dqlen + 1u);
        ce->category_id = ev->discovery_category_id;
        ce->offset = ev->discovery_offset;
        ce->total = s->discovery_total;
        ce->count = n;
        if (n > 0 && s->discovery_items) memcpy(ce->items, s->discovery_items, n * sizeof ce->items[0]);
      }
      break;
    }
    case CCORD_NET_DM_RECIPIENT_CHANGE:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      if (ccord_store_change_dm_recipient(s->store, ev->channel_id, ev->user_id, ev->recipient_added, &err) !=
            CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: dm recipient change rejected: %s", err.message);
      }
      break;
    case CCORD_NET_MESSAGE_REQUEST_ACCEPTED:
      if (ccord_store_accept_message_request(s->store, ev->channel_id, &err) != CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: message request accept rejected: %s", err.message);
      }
      break;
    case CCORD_NET_USER_NOTE_UPDATE:
      if (ccord_store_set_user_note(s->store, ev->user_id, ev->text ? ev->text : "", &err) != CCORD_OK) {
        CCORD_LOG_W("session: user note update rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CUSTOM_STATUS_UPDATE:
      if (ccord_store_set_self_custom_status(s->store, ev->custom_status_emoji, ev->custom_status_text,
                                             ev->custom_status_expires_ms, ev->custom_status_clear, &err) != CCORD_OK) {
        CCORD_LOG_W("session: custom status update rejected: %s", err.message);
      }
      break;
    case CCORD_NET_GUILD_INCIDENTS:
      if (ccord_store_set_guild_incidents(s->store, ev->guild_id, ev->invites_disabled_until_ms,
                                          ev->dms_disabled_until_ms, &err) != CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: guild incidents update rejected: %s", err.message);
      }
      break;
    case CCORD_NET_GUILD_VANITY:
      if (ccord_store_set_guild_vanity(s->store, ev->guild_id, ev->invite_code ? ev->invite_code : "", &err) !=
            CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: guild vanity update rejected: %s", err.message);
      }
      break;
    case CCORD_NET_EMOJI_UPSERT:
      for (size_t i = 0; i < ev->emoji_count; i++) {
        ccord_id egid = ev->guild_id != CCORD_ID_NONE ? ev->guild_id : ev->emojis[i].guild_id;
        if (ccord_store_upsert_guild_emoji(s->store, egid, &ev->emojis[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: emoji upsert rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_EMOJI_DELETE:
      (void)ccord_store_remove_guild_emoji(s->store, ev->guild_id, ev->expr_id);
      break;
    case CCORD_NET_STICKER_UPSERT:
      for (size_t i = 0; i < ev->sticker_count; i++) {
        ccord_id sgid = ev->guild_id != CCORD_ID_NONE ? ev->guild_id : ev->stickers[i].guild_id;
        if (ccord_store_upsert_guild_sticker(s->store, sgid, &ev->stickers[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: sticker upsert rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_STICKER_DELETE:
      (void)ccord_store_remove_guild_sticker(s->store, ev->guild_id, ev->expr_id);
      break;
    case CCORD_NET_SOUNDBOARD_UPSERT:
      for (size_t i = 0; i < ev->soundboard_count; i++) {
        ccord_id sbgid = ev->guild_id != CCORD_ID_NONE ? ev->guild_id : ev->soundboard[i].guild_id;
        if (ccord_store_upsert_guild_soundboard(s->store, sbgid, &ev->soundboard[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: soundboard upsert rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_SOUNDBOARD_DELETE:
      (void)ccord_store_remove_guild_soundboard(s->store, ev->guild_id, ev->expr_id);
      break;
    case CCORD_NET_PRUNE_RESULT:
      if (ccord_store_set_prune_info(s->store, ev->guild_id, &ev->prune_info, &err) != CCORD_OK) {
        CCORD_LOG_W("session: prune info rejected: %s", err.message);
      }
      break;
    case CCORD_NET_AUTOMOD_LIST:
      if (ccord_store_set_automod_rules(s->store, ev->guild_id, ev->automod_rules, ev->automod_rule_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: automod list rejected: %s", err.message);
      }
      break;
    case CCORD_NET_AUTOMOD_UPSERT:
      for (size_t i = 0; ev->automod_rules && i < ev->automod_rule_count; i++) {
        ccord_id amgid = ev->guild_id != CCORD_ID_NONE ? ev->guild_id : ev->automod_rules[i].guild_id;
        if (ccord_store_upsert_automod_rule(s->store, amgid, &ev->automod_rules[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: automod upsert rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_AUTOMOD_DELETE:
      (void)ccord_store_remove_automod_rule(s->store, ev->guild_id, ev->expr_id);
      break;
    case CCORD_NET_WEBHOOK_LIST:
      if (ccord_store_set_webhooks(s->store, ev->guild_id, ev->webhooks, ev->webhook_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: webhook list rejected: %s", err.message);
      }
      break;
    case CCORD_NET_WEBHOOK_UPSERT:
      for (size_t i = 0; ev->webhooks && i < ev->webhook_count; i++) {
        ccord_id whgid = ev->guild_id != CCORD_ID_NONE ? ev->guild_id : ev->webhooks[i].guild_id;
        if (ccord_store_upsert_webhook(s->store, whgid, &ev->webhooks[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: webhook upsert rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_WEBHOOK_DELETE:
      (void)ccord_store_remove_webhook(s->store, ev->guild_id, ev->expr_id);
      break;
    case CCORD_NET_WEBHOOKS_UPDATE:
      ccord_store_note_webhooks_update(s->store, ev->guild_id, ev->channel_id);
      break;
    case CCORD_NET_CHANNEL_FOLLOWER_ADDED: {
      ccord_followed_channel fc = {
        .source_channel_id = ev->channel_id,
        .target_channel_id = ev->target_channel_id,
        .webhook_id = ev->expr_id,
      };
      if (ccord_store_add_followed_channel(s->store, ev->guild_id, &fc, &err) != CCORD_OK) {
        CCORD_LOG_W("session: followed channel rejected: %s", err.message);
      }
      if (ccord_store_webhooks_loaded(s->store, ev->guild_id)) {
        ccord_admin_request wreq;
        if (ccord_admin_list_webhooks(&wreq, ev->guild_id, nullptr) == CCORD_OK) {
          (void)ccord_transport_admin_action(s->transport, &wreq, nullptr);
        }
      }
      break;
    }
    case CCORD_NET_APP_CMD_PERMS_LIST:
      if (ccord_store_set_app_cmd_perms(s->store, ev->guild_id, ev->app_cmd_perms, ev->app_cmd_perms_count, &err) !=
          CCORD_OK) {
        CCORD_LOG_W("session: app cmd perms rejected: %s", err.message);
      }
      break;
    case CCORD_NET_WELCOME_SCREEN:
      if (ev->welcome_screen &&
          ccord_store_set_welcome_screen(s->store, ev->guild_id, ev->welcome_screen, &err) != CCORD_OK) {
        CCORD_LOG_W("session: welcome screen rejected: %s", err.message);
      }
      break;
    case CCORD_NET_ONBOARDING:
      if (ev->onboarding && ccord_store_set_onboarding(s->store, ev->guild_id, ev->onboarding, &err) != CCORD_OK) {
        CCORD_LOG_W("session: onboarding rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CHANNEL_OVERWRITE_UPSERT:
      if (ccord_store_upsert_channel_overwrite(s->store, ev->channel_id, ev->guild_id, &ev->overwrite_item, &err) !=
          CCORD_OK) {
        CCORD_LOG_W("session: channel overwrite upsert rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CHANNEL_OVERWRITE_DELETE:
      (void)ccord_store_remove_channel_overwrite(s->store, ev->channel_id, ev->overwrite_item.id);
      break;
    case CCORD_NET_CHANNEL_INVITE_LIST:
      if (ccord_store_set_channel_invites(s->store, ev->guild_id, ev->channel_id, ev->invites, ev->invite_count,
                                          &err) != CCORD_OK) {
        CCORD_LOG_W("session: channel invite list rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CALL_CREATE:
    case CCORD_NET_CALL_UPDATE:
      // Ringing is passive: the call row (and the caller's voice state, a separate event) arrives
      // unprompted. Nothing here was asked for by a local click.
      if (ev->call && ccord_store_set_call(s->store, ev->call, &err) != CCORD_OK) {
        CCORD_LOG_W("session: call rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CALL_DELETE:
      (void)ccord_store_remove_call(s->store, ev->channel_id, &err);
      break;
    case CCORD_NET_STAGE_INSTANCE_UPDATE:
      if (ev->stage_deleted) {
        (void)ccord_store_remove_stage_instance(s->store, ev->channel_id, &err);
      } else if (ev->stage_instance &&
                 ccord_store_set_stage_instance(s->store, ev->stage_instance, &err) != CCORD_OK) {
        CCORD_LOG_W("session: stage instance rejected: %s", err.message);
      }
      break;
    case CCORD_NET_SCHEDULED_EVENT_LIST:
      // The REST answer replaces the guild's cached list (an empty answer drops it).
      if (ccord_store_set_scheduled_events(s->store, ev->guild_id, ev->scheduled_events, ev->scheduled_event_count,
                                           &err) != CCORD_OK) {
        CCORD_LOG_W("session: scheduled event list rejected: %s", err.message);
      }
      break;
    case CCORD_NET_SCHEDULED_EVENT_UPDATE:
      if (ev->scheduled_event_deleted) {
        (void)ccord_store_remove_scheduled_event(s->store, ev->guild_id, ev->scheduled_event_id);
      } else if (ev->scheduled_events && ev->scheduled_event_count > 0 &&
                 ccord_store_upsert_scheduled_event(s->store, &ev->scheduled_events[0], &err) != CCORD_OK) {
        CCORD_LOG_W("session: scheduled event rejected: %s", err.message);
      }
      break;
    case CCORD_NET_SCHEDULED_EVENT_USER_CHANGE: {
      // About us: the RSVP state (the store moves user_count with it). About someone else: only the count,
      // and only when it is known (0 means "not reported", never a guess).
      ccord_id self = ccord_store_self_id(s->store);
      if (self != CCORD_ID_NONE && ev->user_id == self) {
        ccord_status rs = ccord_store_set_scheduled_event_rsvp(s->store, ev->guild_id, ev->scheduled_event_id,
                                                               ev->scheduled_event_user_added, &err);
        if (rs != CCORD_OK && rs != CCORD_ERR_NOT_FOUND) {
          CCORD_LOG_W("session: scheduled event rsvp rejected: %s", err.message);
        }
      } else {
        const ccord_scheduled_event *e = ccord_store_find_scheduled_event(s->store, ev->scheduled_event_id);
        if (e && e->guild_id == ev->guild_id && e->user_count > 0) {
          ccord_scheduled_event upd = *e;
          upd.user_count = ev->scheduled_event_user_added ? upd.user_count + 1u : upd.user_count - 1u;
          if (ccord_store_upsert_scheduled_event(s->store, &upd, &err) != CCORD_OK) {
            CCORD_LOG_W("session: scheduled event count rejected: %s", err.message);
          }
        }
      }
      break;
    }
    case CCORD_NET_VOICE_STATUS_UPDATE:
      if (ccord_store_set_voice_status(s->store, ev->channel_id, ev->voice_status, &err) != CCORD_OK) {
        CCORD_LOG_W("session: voice status rejected: %s", err.message);
      }
      break;
    case CCORD_NET_GUILD_PROFILE: {
      session_guild_profile_cache *ent = guild_profile_for(s, ev->guild_id);
      if (ent) {
        ent->profile = ev->guild_profile;
        if (ent->profile.guild_id == CCORD_ID_NONE) ent->profile.guild_id = ev->guild_id;
        ent->has_profile = true;
        ent->loading = false;
        ent->sent_ms = 0;
        uint64_t now = ccord_clock_ms();
        ent->fetched_ms = now ? now : 1u;
      }
      break;
    }
    case CCORD_NET_ACTIVITY_SHELF: {
      // The REST answer replaces the guild's cached shelf (an empty answer drops it).
      session_activity_shelf *ent = activity_shelf_for(s, ev->guild_id);
      if (ent) {
        size_t n = ev->activity_apps ? ev->activity_app_count : 0u;
        if (n > SESSION_ACTIVITY_SHELF_MAX) n = SESSION_ACTIVITY_SHELF_MAX;
        if (n > 0) memcpy(ent->apps, ev->activity_apps, n * sizeof ent->apps[0]);
        ent->count = n;
        ent->loading = false;
        ent->sent_ms = 0;
        uint64_t now = ccord_clock_ms();
        ent->fetched_ms = now ? now : 1u;
      }
      break;
    }
    case CCORD_NET_ACTIVITY_INSTANCE_UPDATE:
      // participant_count 0 ends the instance; the store upsert removes the row.
      if (ccord_store_upsert_activity_instance(s->store, &ev->activity_instance, &err) != CCORD_OK) {
        CCORD_LOG_W("session: activity instance rejected: %s", err.message);
      }
      break;
    case CCORD_NET_USER_SETTINGS:
      // REST answer and gateway USER_SETTINGS_UPDATE are the same full object: the store is the single source.
      if (ev->user_settings &&
          ccord_store_set_user_settings(s->store, ev->user_settings, &err) != CCORD_OK) {
        CCORD_LOG_W("session: user settings rejected: %s", err.message);
      }
      break;
    case CCORD_NET_AUTH_SESSIONS: {
      size_t n = ev->auth_sessions ? ev->auth_session_count : 0u;
      if (n > CCORD_SESSION_MAX_AUTH_SESSIONS) n = CCORD_SESSION_MAX_AUTH_SESSIONS;
      if (n > 0) memcpy(s->auth_sessions, ev->auth_sessions, n * sizeof s->auth_sessions[0]);
      s->auth_session_count = n;
      s->auth_sessions_state = CCORD_FETCH_READY;
      s->auth_sessions_fetched_ms = s->auth_sessions_sent_ms
                                        ? s->auth_sessions_sent_ms
                                        : (ccord_clock_ms() ? ccord_clock_ms() : 1u);
      s->auth_sessions_sent_ms = 0;
      break;
    }
    case CCORD_NET_AUTH_SESSIONS_LOGOUT: {
      size_t n = ev->session_hash_count;
      if (n > CCORD_ADMIN_SESSION_LOGOUT_MAX) n = CCORD_ADMIN_SESSION_LOGOUT_MAX;
      drop_auth_sessions(s, ev->session_hashes, n);
      break;
    }
    case CCORD_NET_CONNECTIONS: {
      size_t n = ev->connections ? ev->connection_count : 0u;
      if (n > CCORD_SESSION_MAX_CONNECTIONS) n = CCORD_SESSION_MAX_CONNECTIONS;
      if (n > 0) memcpy(s->connections, ev->connections, n * sizeof s->connections[0]);
      s->connection_count = n;
      s->connections_state = CCORD_FETCH_READY;
      s->connections_fetched_ms =
        s->connections_sent_ms ? s->connections_sent_ms : (ccord_clock_ms() ? ccord_clock_ms() : 1u);
      s->connections_sent_ms = 0;
      break;
    }
    case CCORD_NET_CONNECTION_UPSERT:
      upsert_connection(s, &ev->connection);
      break;
    case CCORD_NET_CONNECTION_DELETE:
      drop_connection(s, ev->connection_name, ev->connection_id);
      break;
    case CCORD_NET_REPORT_MENU:
    case CCORD_NET_REPORT_SUBMITTED:
      queue_report_event(s, ev);
      break;
    case CCORD_NET_NONE:
      break;
    default: break;
  }
}

static void queue_report_event(ccord_session *s, const ccord_net_event *ev)
{
  if (s->report_count == SESSION_MAX_REPORT_EVENTS) return;   // the UI drains every frame; never grow unbounded
  ccord_report_event e = {.kind = ev->type == CCORD_NET_REPORT_MENU ? CCORD_REPORT_EV_MENU : CCORD_REPORT_EV_SUBMITTED};
  if (ev->expr_arg && strcmp(ev->expr_arg, ccord_report_type_name(CCORD_REPORT_USER)) == 0) e.type = CCORD_REPORT_USER;
  else e.type = CCORD_REPORT_MESSAGE;
  if (e.kind == CCORD_REPORT_EV_MENU) {
    if (!ev->expr_body) return;
    e.menu_json = ccord_strndup(ev->expr_body, ev->expr_body_len);
    if (!e.menu_json) return;
    e.menu_len = ev->expr_body_len;
  } else {
    e.report_id = ev->report_id;
  }
  s->report_events[(s->report_head + s->report_count) % SESSION_MAX_REPORT_EVENTS] = e;
  s->report_count++;
}

bool ccord_session_pop_report_event(ccord_session *s, ccord_report_event *out)
{
  if (!s || !out || s->report_count == 0) return false;
  *out = s->report_events[s->report_head];
  s->report_events[s->report_head] = (ccord_report_event){0};   // ownership of menu_json moves to the caller
  s->report_head = (s->report_head + 1u) % SESSION_MAX_REPORT_EVENTS;
  s->report_count--;
  return true;
}

void ccord_report_event_free(ccord_report_event *e)
{
  if (!e) return;
  ccord_free(e->menu_json);
  e->menu_json = nullptr;
  e->menu_len = 0;
}

static void record_failure(ccord_session *s, const ccord_net_event *ev)
{
  CCORD_LOG_W("session: action %d failed: %s", (int)ev->action, ev->text ? ev->text : "");
  if (s->fail_count == SESSION_MAX_FAILURES) {  // unreachable while the UI drains every frame; never grow unbounded
    ccord_free(s->failures[s->fail_head].draft);
    s->fail_head = (s->fail_head + 1u) % SESSION_MAX_FAILURES;
    s->fail_count--;
  }
  ccord_action_failure *f = &s->failures[(s->fail_head + s->fail_count) % SESSION_MAX_FAILURES];
  memset(f, 0, sizeof *f);
  f->action = ev->action;
  f->channel_id = ev->channel_id;
  f->message_id = ev->message_id;
  f->guild_id = ev->guild_id;
  f->user_id = ev->user_id;
  f->expr_id = ev->expr_id;
  f->admin_op = ev->admin_op;
  f->status = ev->status;
  const char *reason = ev->text && ev->text[0] ? ev->text : ccord_status_name(ev->status);
  size_t rlen = strlen(reason);
  if (rlen >= sizeof f->reason) rlen = sizeof f->reason - 1u;
  memcpy(f->reason, reason, rlen);
  f->reason[rlen] = '\0';
  if (ev->draft && ev->draft_len > 0) {
    f->draft = ccord_strndup(ev->draft, ev->draft_len);
    if (f->draft) f->draft_len = ev->draft_len;
  }
  s->fail_count++;
}

bool ccord_session_pop_failure(ccord_session *s, ccord_action_failure *out)
{
  if (!s || !out || s->fail_count == 0) return false;
  *out = s->failures[s->fail_head];
  s->failures[s->fail_head].draft = nullptr;  // ownership moves to the caller
  s->fail_head = (s->fail_head + 1u) % SESSION_MAX_FAILURES;
  s->fail_count--;
  return true;
}

void ccord_action_failure_free(ccord_action_failure *f)
{
  if (!f) return;
  ccord_free(f->draft);
  f->draft = nullptr;
  f->draft_len = 0;
}

size_t ccord_session_pump(ccord_session *s)
{
  if (!s) return 0;
  size_t n = 0;
  ccord_net_event ev;
  while (n < PUMP_BATCH && ccord_transport_poll(s->transport, &ev)) {
    apply_event(s, &ev);
    n++;
  }
  ccord_stream_hub_pump(s->streams, ccord_clock_ms());
  ccord_profile_rest_pump(s->profile_rest, ccord_clock_ms());
  if (s->rich_op3_dirty && s->state == CCORD_CONN_CONNECTED && !s->stale &&
      ccord_store_self_id(s->store) != CCORD_ID_NONE) {
    ccord_error gw_err;
    if (session_send_presence_with_custom(s, session_current_presence(s), &gw_err) == CCORD_OK) {
      s->rich_op3_dirty = false;
    }
  }
  return n;
}

ccord_status ccord_session_select_guild(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_status st = ccord_store_select_guild(s->store, guild_id, err);
  if (st == CCORD_OK) {
    ccord_id sel_ch = ccord_store_selected_channel(s->store);
    ccord_error hook_err;
    ccord_status hst = s->stale ? CCORD_OK : ccord_transport_select_channel(s->transport, guild_id, sel_ch, &hook_err);
    if (hst != CCORD_OK && hst != CCORD_ERR_UNSUPPORTED) {
      CCORD_LOG_W("session: guild select hook failed: %s", hook_err.message);
    }
    s->retried_channel = CCORD_ID_NONE;
    request_history_once(s, sel_ch);
    maybe_request_dm_profile(s, sel_ch);
  }
  return st;
}

// Text, voice and stage channels all carry a message timeline (a voice/stage channel's own "Text Chat"); categories do not.
static bool session_channel_has_chat(const ccord_channel *ch)
{
  return ch && (ch->kind == CCORD_CHANNEL_TEXT || ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE);
}

ccord_status ccord_session_select_channel(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_id prev_guild = ccord_store_selected_guild(s->store);
  ccord_id prev_ch = ccord_store_selected_channel(s->store);
  ccord_status st = ccord_store_select_channel(s->store, channel_id, err);
  if (st == CCORD_OK) {
    ccord_id new_guild = ccord_store_selected_guild(s->store);
    if (new_guild != CCORD_ID_NONE && !s->stale) {   // stale: READY re-subscribes the selected channel
      if (new_guild != prev_guild) {
        ccord_error hook_err;
        ccord_status hst = ccord_transport_select_channel(s->transport, new_guild, channel_id, &hook_err);
        if (hst != CCORD_OK && hst != CCORD_ERR_UNSUPPORTED) {
          CCORD_LOG_W("session: guild select hook failed: %s", hook_err.message);
        }
      } else if (channel_id != prev_ch && new_guild != CCORD_DM_GUILD_ID &&
                 CCORD_TRANSPORT_OPTIONAL(s->transport, select_channel)) {
        ccord_error hook_err;
        ccord_status hst = s->transport->vt->select_channel(s->transport, new_guild, channel_id, &hook_err);
        if (hst != CCORD_OK && hst != CCORD_ERR_UNSUPPORTED) {
          CCORD_LOG_W("session: channel select hook failed: %s", hook_err.message);
        }
      }
    }
    s->retried_channel = CCORD_ID_NONE;
    request_history_once(s, channel_id);
    maybe_request_dm_profile(s, channel_id);
  }
  return st;
}

static bool blank(const char *s, size_t len)
{
  for (size_t i = 0; i < len; i++) {
    if (s[i] != ' ' && s[i] != '\n' && s[i] != '\t' && s[i] != '\r') return false;
  }
  return true;
}

// Membership screening: while the signed-in member is pending, the guild refuses their messages. The store owns the
// flag (ccord_member.pending); it clears only when Discord confirms the submitted form, never optimistically.
static ccord_status session_screening_gate(const ccord_session *s, const ccord_channel *ch, ccord_error *err)
{
  if (ch->guild_id != CCORD_DM_GUILD_ID && ccord_session_self_pending(s, ch->guild_id)) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION,
                           "Complete the server's membership screening before sending messages.");
  }
  return CCORD_OK;
}

ccord_status ccord_session_send_message(ccord_session *s, ccord_id channel_id, const char *utf8, size_t len, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s || !utf8) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_INVALID, "message is longer than %u bytes", CCORD_SEND_MAX_BYTES);
  if (blank(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "message is empty");
  if (memchr(utf8, 0, len) || !ccord_utf8_valid_text(utf8, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message is not valid text");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  // M0-3: the pending echo needs a nonce the transport can carry and confirm; hook-less transports (and a store
  // without a self id yet) keep the legacy no-echo path.
  ccord_id self_id = ccord_store_self_id(s->store);
  if (!CCORD_TRANSPORT_OPTIONAL(s->transport, send_message_with_nonce) || self_id == CCORD_ID_NONE) {
    return ccord_transport_send_message(s->transport, channel_id, utf8, len, err);
  }
  char text[CCORD_SEND_MAX_BYTES + 1];   // NUL-terminated copy: the caller's buffer is borrowed, and the store
  memcpy(text, utf8, len);               // upsert below may reallocate the vectors the channel pointer lives in
  text[len] = '\0';
  char nonce[32];
  make_send_nonce(s, nonce);
  session_local_echo echo = {
    .id = SESSION_LOCAL_ID_FLAG | (++s->local_msg_seq & (SESSION_LOCAL_ID_FLAG - 1u)),
    .author_id = self_id,
    .timestamp_ms = (int64_t)ccord_clock_ms(),
    .send_state = CCORD_MSG_SEND_PENDING,
  };
  memcpy(echo.nonce, nonce, sizeof echo.nonce);
  memcpy(echo.text, text, len + 1u);
  ccord_message m = {
    .id = echo.id,
    .channel_id = channel_id,
    .author_id = self_id,
    .timestamp_ms = echo.timestamp_ms,
    .content = text,
    .send_state = CCORD_MSG_SEND_PENDING,
  };
  memcpy(m.nonce, nonce, sizeof m.nonce);
  ccord_error aerr;
  bool echoed = ccord_store_append_message(s->store, &m, &aerr) == CCORD_OK;
  if (!echoed) CCORD_LOG_W("session: pending echo not stored: %s", aerr.message);
  ccord_status st = ccord_transport_send_message_with_nonce(s->transport, channel_id, text, len,
                                                            nonce, strlen(nonce), err);
  if (st != CCORD_OK && echoed) {
    // Synchronous refusal: roll the echo back. The caller still holds the text (the composer keeps it on error).
    remove_local_echo(s, channel_id, echo.id);
  }
  return st;
}

ccord_status ccord_session_retry_message(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!session_is_local_echo_id(message_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "not a failed message");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (!m || m->send_state != CCORD_MSG_SEND_FAILED || m->nonce[0] == '\0') {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "not a failed message");
  }
  if (!CCORD_TRANSPORT_OPTIONAL(s->transport, send_message_with_nonce)) {
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "transport: retry not supported");
  }
  // Copy the draft and the ORIGINAL nonce to the stack before the store mutates (Discord dedupes by the nonce).
  session_local_echo echo = {
    .id = m->id,
    .author_id = m->author_id,
    .timestamp_ms = m->timestamp_ms,
    .send_state = CCORD_MSG_SEND_PENDING,
  };
  memcpy(echo.nonce, m->nonce, sizeof echo.nonce);
  (void)snprintf(echo.text, sizeof echo.text, "%s", m->content);
  if (!set_local_echo_state(s, channel_id, &echo, CCORD_MSG_SEND_PENDING)) {
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "session: could not re-pend the message");
  }
  ccord_status st = ccord_transport_send_message_with_nonce(s->transport, channel_id, echo.text, strlen(echo.text),
                                                            echo.nonce, strlen(echo.nonce), err);
  if (st != CCORD_OK) {
    // Synchronous refusal: the bubble stays failed with its draft.
    (void)set_local_echo_state(s, channel_id, &echo, CCORD_MSG_SEND_FAILED);
  }
  return st;
}

static ccord_status validate_text(const char *utf8, size_t len, ccord_error *err)
{
  if (!utf8) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null text");
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_INVALID, "text is longer than %u bytes", CCORD_SEND_MAX_BYTES);
  if (blank(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "text is empty");
  if (memchr(utf8, 0, len) || !ccord_utf8_valid_text(utf8, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "text is not valid text");
  }
  return CCORD_OK;
}

ccord_status ccord_session_edit_message(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                        const char *utf8, size_t len, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_status st = validate_text(utf8, len, err);
  if (st != CCORD_OK) return st;
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  const ccord_message *em = ccord_store_find_message(s->store, channel_id, message_id);
  if (!em) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(em->id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message has not been sent yet");
  }
  return ccord_transport_edit_message(s->transport, channel_id, message_id, utf8, len, err);
}

ccord_status ccord_session_delete_message(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  const ccord_message *dm = ccord_store_find_message(s->store, channel_id, message_id);
  if (!dm) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(dm->id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message has not been sent yet");
  }
  return ccord_transport_delete_message(s->transport, channel_id, message_id, err);
}

ccord_status ccord_session_send_typing(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  return ccord_transport_send_typing(s->transport, channel_id, err);
}

ccord_status ccord_session_toggle_reaction(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                           const char *emoji, size_t len, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!emoji || len == 0 || memchr(emoji, 0, len) || !ccord_utf8_valid_text(emoji, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid reaction");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(m->id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message has not been sent yet");
  }
  bool already_reacted = m->self_reacted;
  const char *send_emoji = emoji;
  size_t send_len = len;
  if (m->reaction_kind_count > 0 && m->reactions) {
    already_reacted = false;
    for (size_t i = 0; i < m->reaction_kind_count; i++) {
      const char *re = m->reactions[i].emoji;
      if (!re) continue;
      size_t rlen = strlen(re);
      bool match = (rlen == len && memcmp(re, emoji, len) == 0);
      if (!match && !memchr(emoji, ':', len)) {
        const char *colon = strrchr(re, ':');
        if (colon && (size_t)(colon - re) == len && memcmp(re, emoji, len) == 0) {
          match = true;
        }
      }
      if (match) {
        already_reacted = m->reactions[i].self_reacted;
        send_emoji = re;
        send_len = rlen;
        break;
      }
    }
  }
  return ccord_transport_toggle_reaction(s->transport, channel_id, message_id, send_emoji, send_len,
                                         !already_reacted, err);
}

ccord_status ccord_session_ack_message(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (message_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: ack message id");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (m && ccord_message_is_ephemeral(m)) {
    ccord_error_clear(err);
    return CCORD_OK;
  }
  if (m && session_is_local_echo_id(m->id)) {
    ccord_error_clear(err);   // unconfirmed echo: there is no server message to acknowledge yet
    return CCORD_OK;
  }
  ccord_id upto = message_id;
  if (ch->unread_count == 0 && ch->mention_count == 0 && ch->last_message_id > upto) {
    upto = ch->last_message_id;
  }
  ccord_status st = ccord_transport_ack_message(s->transport, channel_id, message_id, err);
  if (st == CCORD_OK) {
    ccord_error ignored;
    (void)ccord_store_apply_read(s->store, channel_id, upto, UINT32_MAX, &ignored);
  }
  return st;
}

ccord_status ccord_session_dismiss_ephemeral(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                             ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  return ccord_store_dismiss_ephemeral(s->store, channel_id, message_id, err);
}

bool ccord_session_more_members(ccord_session *s, uint64_t now_ms)
{
  if (!s || s->stale) return false;
  ccord_id guild = ccord_store_selected_guild(s->store), channel = ccord_store_selected_channel(s->store);
  if (guild == CCORD_ID_NONE || guild == CCORD_DM_GUILD_ID || channel == CCORD_ID_NONE) return false;
  if (s->paging_guild != guild || s->paging_channel != channel) {   // another list: it starts at the top again
    s->paging_guild = guild;
    s->paging_channel = channel;
    s->paging_chunks = 1;
    s->paging_wait = s->paging_done = false;
  }
  if (s->paging_done || s->paging_chunks >= CCORD_MEMBER_CHUNKS_MAX) return false;
  // Only once the server has said how big the list is (its first update does): before that there is no telling whether
  // there is more, and a request would only be a guess. And not when everything it holds is here already.
  uint32_t total = 0;
  size_t held = ccord_store_member_count(s->store, guild);
  if (!ccord_store_member_list_size(s->store, guild, &total, nullptr) || held >= total) return false;
  // One piece at a time, and not faster than SESSION_MEMBER_PAGE_GAP_MS: a piece is awaited (a silent server is given
  // SESSION_MEMBER_PAGE_WAIT_MS before asking again), so scrolling a long list cannot become a stream of requests.
  if (s->paging_sent_ms != 0 && now_ms < s->paging_sent_ms + SESSION_MEMBER_PAGE_GAP_MS) return false;
  if (s->paging_wait && now_ms < s->paging_sent_ms + SESSION_MEMBER_PAGE_WAIT_MS) return false;
  ccord_error err;
  s->paging_sent_ms = now_ms ? now_ms : 1u;   // a failed request is not retried at once either
  if (ccord_transport_request_members(s->transport, guild, channel, s->paging_chunks + 1u, &err) != CCORD_OK) return false;
  s->paging_chunks++;
  s->paging_wait = true;
  s->paging_held = held;
  return true;
}

void ccord_session_set_reading(ccord_session *s, ccord_id channel_id)
{
  if (!s) return;
  s->reading_channel = channel_id;
  s->reading_known = true;
  if (channel_id != CCORD_ID_NONE && s->state == CCORD_CONN_CONNECTED && !s->stale) {
    ccord_id self_id = ccord_store_self_id(s->store);
    if (self_id != CCORD_ID_NONE && !ccord_store_user_profile(s->store, self_id)) {
      ccord_error ignored;
      (void)ccord_session_request_profile(s, self_id, &ignored);
    }
  }
}

ccord_status ccord_session_mark_read(ccord_session *s, ccord_id guild_id, ccord_id channel_id, size_t *acked, ccord_error *err)
{
  if (acked) *acked = 0;
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (guild_id != CCORD_DM_GUILD_ID && !ccord_store_guild(s->store, guild_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That server isn't available.");
  }
  if (channel_id != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
    if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
    if (ch->guild_id != guild_id) return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't in this server.");
  }
  ccord_read_target targets[CCORD_MARK_READ_MAX];
  size_t n = ccord_store_read_targets(s->store, guild_id, channel_id, ccord_clock_unix_ms(), targets, CCORD_MARK_READ_MAX);
  if (n > CCORD_MARK_READ_MAX) n = CCORD_MARK_READ_MAX;
  size_t acked_count = 0;
  for (size_t i = 0; i < n; i++) {
    if (session_is_local_echo_id(targets[i].message_id)) continue;   // no server message to acknowledge yet
    ccord_status st = ccord_session_ack_message(s, targets[i].channel_id, targets[i].message_id, err);
    if (st != CCORD_OK) {
      if (acked_count == 0) return st;
      break;   // some went out: report those, the rest are still unread
    }
    acked_count++;
  }
  if (acked_count > 0) ccord_error_clear(err);
  if (acked) *acked = acked_count;
  return CCORD_OK;
}

static const ccord_voice_state *self_voice(const ccord_session *s, ccord_id guild_id)
{
  return ccord_store_voice_user(s->store, guild_id, ccord_store_self_id(s->store));
}

static const ccord_voice_state *find_active_self_voice(const ccord_session *s)
{
  ccord_id self_id = ccord_store_self_id(s->store);
  if (self_id == CCORD_ID_NONE) return nullptr;
  ccord_id ch_id = ccord_store_active_voice_channel(s->store);
  if (ch_id == CCORD_ID_NONE) return nullptr;
  const ccord_channel *ch = ccord_store_channel(s->store, ch_id);
  if (ch) {
    const ccord_voice_state *v = ccord_store_voice_user(s->store, ch->guild_id, self_id);
    if (v && v->channel_id != CCORD_ID_NONE) return v;
  }
  const ccord_voice_state *dm = ccord_store_voice_user(s->store, CCORD_DM_GUILD_ID, self_id);
  if (dm && dm->channel_id != CCORD_ID_NONE) return dm;
  size_t nv = ccord_store_voice_count(s->store, ch_id);
  for (size_t i = 0; i < nv; i++) {
    const ccord_voice_state *v = ccord_store_voice_at(s->store, ch_id, i);
    if (v && v->user_id == self_id && v->channel_id != CCORD_ID_NONE) return v;
  }
  return nullptr;
}

ccord_status ccord_session_join_voice(ccord_session *s, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!ccord_store_guild(s->store, guild_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown guild");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch || ch->guild_id != guild_id) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown voice channel");
  // Stage channels join as audience: the server answers with suppress=true until invited to speak.
  if (ch->kind != CCORD_CHANNEL_VOICE && ch->kind != CCORD_CHANNEL_STAGE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "channel is not a voice channel");
  }
  ccord_status st = ccord_transport_join_voice(s->transport, guild_id, channel_id, err);
  if (st == CCORD_OK) {
    s->pending_voice_guild = guild_id;
    s->pending_voice_channel = channel_id;
  }
  return st;
}

ccord_status ccord_session_leave_voice(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_voice_state *v = self_voice(s, guild_id);
  bool pending = s->pending_voice_guild == guild_id && s->pending_voice_channel != CCORD_ID_NONE;
  if ((!v || v->channel_id == CCORD_ID_NONE) && !pending) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: not connected to voice");
  }
  ccord_status st = ccord_transport_leave_voice(s->transport, guild_id, err);
  if (st == CCORD_OK && pending) {
    s->pending_voice_guild = CCORD_ID_NONE;
    s->pending_voice_channel = CCORD_ID_NONE;
  }
  return st;
}

static ccord_status session_update_self_voice(ccord_session *s, ccord_id gid, bool next_mute, bool next_deaf,
                                              ccord_error *err)
{
  if (gid != CCORD_ID_NONE || (s->transport && s->transport->vt && s->transport->vt->update_voice_state)) {
    ccord_status st = ccord_transport_update_voice_state(s->transport, gid, next_mute, next_deaf, err);
    if (st != CCORD_OK) return st;
  }
  s->voice_self_mute = next_mute;
  s->voice_self_deaf = next_deaf;
  return CCORD_OK;
}

ccord_status ccord_session_set_voice_mute(ccord_session *s, bool muted, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_voice_state *v = find_active_self_voice(s);
  ccord_id gid = v ? v->guild_id : s->pending_voice_guild;
  bool cur_deaf = v ? v->self_deaf : s->voice_self_deaf;
  bool next_deaf = !muted ? false : cur_deaf;
  return session_update_self_voice(s, gid, muted, next_deaf, err);
}

ccord_status ccord_session_set_voice_deafen(ccord_session *s, bool deafened, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_voice_state *v = find_active_self_voice(s);
  ccord_id gid = v ? v->guild_id : s->pending_voice_guild;
  bool cur_mute = v ? v->self_mute : s->voice_self_mute;
  // Deafened implies muted in Discord; keep both flags consistent.
  bool next_mute = deafened ? true : cur_mute;
  return session_update_self_voice(s, gid, next_mute, deafened, err);
}

bool ccord_session_voice_muted(const ccord_session *s)
{
  if (!s) return false;
  const ccord_voice_state *v = find_active_self_voice(s);
  return v ? v->self_mute : s->voice_self_mute;
}

bool ccord_session_voice_deafened(const ccord_session *s)
{
  if (!s) return false;
  const ccord_voice_state *v = find_active_self_voice(s);
  return v ? v->self_deaf : s->voice_self_deaf;
}

ccord_status ccord_session_start_stream(ccord_session *s, ccord_id guild_id, ccord_id channel_id,
                                        const ccord_stream_config *config, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!config) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null stream config");
  if (config->encoder > CCORD_STREAM_ENCODER_AUTO || config->codec > CCORD_STREAM_CODEC_AV1 ||
      config->framework > CCORD_STREAM_FRAMEWORK_AUTO || config->resolution > CCORD_STREAM_RES_SOURCE ||
      config->fps > CCORD_STREAM_FPS_60) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: stream config");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  bool dm_call = guild_id == CCORD_DM_GUILD_ID && ch && ch->guild_id == CCORD_DM_GUILD_ID &&
                 ch->kind == CCORD_CHANNEL_TEXT;
  bool guild_voice = ch && ch->guild_id == guild_id &&
                     (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE);
  if (!ch || (!dm_call && ch->guild_id != guild_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown voice channel");
  if (!dm_call && !guild_voice) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "channel is not a voice channel");
  }
  const ccord_voice_state *v = self_voice(s, guild_id);
  if (!v || v->channel_id != channel_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: join the voice channel before streaming");
  }
  s->pending_stream_cfg = *config;
  return ccord_transport_start_stream(s->transport, guild_id, channel_id, config, err);
}

ccord_status ccord_session_stop_stream(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_stream_state *st = ccord_store_stream(s->store);
  if (!st || !st->active || st->guild_id != guild_id) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: no active stream");
  }
  return ccord_transport_stop_stream(s->transport, guild_id, err);
}

static void session_drain_watched_rx(ccord_session *s)
{
  if (!s || !s->transport) return;
  uint8_t small_buf[1024];
  uint8_t *fbuf = s->stream_frame_buf ? s->stream_frame_buf : small_buf;
  size_t fcap = s->stream_frame_buf ? SESSION_STREAM_FRAME_MAX : sizeof small_buf;
  for (size_t i = 0; i < 64; i++) {
    size_t out_len = 0;
    if (!ccord_transport_poll_stream_frame(s->transport, fbuf, fcap, &out_len, nullptr, nullptr)) break;
  }
  int16_t pcm_buf[960];
  for (size_t i = 0; i < 256; i++) {
    if (ccord_transport_poll_stream_pcm(s->transport, pcm_buf, sizeof pcm_buf / sizeof pcm_buf[0]) == 0) break;
  }
}

ccord_status ccord_session_watch_stream(ccord_session *s, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                        ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid stream target");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  // Guild broadcasts ride voice/stage channels; DM-call broadcasts ride the call's text channel.
  bool call_ch = ch && ch->guild_id == CCORD_DM_GUILD_ID && ch->kind == CCORD_CHANNEL_TEXT;
  bool voice_ch = ch && ch->guild_id == guild_id &&
                  (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE);
  if (!call_ch && !voice_ch) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: unknown voice channel");
  }
  if (call_ch && guild_id != CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid stream target");
  }
  if (s->watched.active && s->watched.guild_id == guild_id && s->watched.channel_id == channel_id &&
      s->watched.user_id == user_id) {
    ccord_error_clear(err);
    return CCORD_OK;
  }
  bool switching = s->watched.active && (s->watched.channel_id != channel_id || s->watched.user_id != user_id);
  ccord_id prev_ch = s->watched.channel_id;
  ccord_id prev_user = s->watched.user_id;
  if (switching) session_drain_watched_rx(s);
  ccord_status st = ccord_transport_watch_stream(s->transport, guild_id, channel_id, user_id, err);
  if (st == CCORD_OK) {
    if (switching) {
      s->unsub_channel_id = prev_ch;
      s->unsub_user_id = prev_user;
    } else {
      s->unsub_channel_id = CCORD_ID_NONE;
      s->unsub_user_id = CCORD_ID_NONE;
    }
    const ccord_stream_state *known = ccord_store_stream_find(s->store, channel_id, user_id);
    s->watched = (ccord_watched_stream){
      .guild_id = guild_id,
      .channel_id = channel_id,
      .user_id = user_id,
      .active = true,
      .paused = known ? known->paused : false,
    };
    atomic_store_explicit(&s->watched_active, true, memory_order_release);
  }
  return st;
}

ccord_status ccord_session_stop_watching(ccord_session *s, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!s->watched.active) return CCORD_OK;
  s->unsub_channel_id = CCORD_ID_NONE;
  s->unsub_user_id = CCORD_ID_NONE;
  ccord_status st = ccord_transport_stop_watching(s->transport, s->watched.guild_id, s->watched.channel_id,
                                                  s->watched.user_id, err);
  s->watched.active = false;
  atomic_store_explicit(&s->watched_active, false, memory_order_release);
  s->watched.paused = false;
  return st;
}

ccord_status ccord_session_set_stream_paused(ccord_session *s, bool paused, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (s->watched.active) {
    ccord_status st = ccord_transport_set_stream_paused(s->transport, s->watched.guild_id, s->watched.channel_id,
                                                        s->watched.user_id, paused, err);
    if (st == CCORD_OK) s->watched.paused = paused;
    return st;
  }
  const ccord_stream_state *cur = ccord_store_stream(s->store);
  if (cur && cur->active) {
    ccord_id uid = cur->user_id != CCORD_ID_NONE ? cur->user_id : ccord_store_self_id(s->store);
    ccord_status st = ccord_transport_set_stream_paused(s->transport, cur->guild_id, cur->channel_id,
                                                        uid, paused, err);
    if (st == CCORD_OK) {
      ccord_stream_state updated = *cur;
      updated.user_id = uid;
      updated.paused = paused;
      (void)ccord_store_set_stream(s->store, &updated, err);
    }
    return st;
  }
  return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: not watching a stream");
}

const ccord_watched_stream *ccord_session_watched_stream(const ccord_session *s)
{
  return (s && s->watched.active) ? &s->watched : nullptr;
}

ccord_status ccord_session_send_reply_ex(ccord_session *s, ccord_id channel_id, ccord_id reply_to_id,
                                         bool mention_author, const char *utf8, size_t len, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_status st = validate_text(utf8, len, err);
  if (st != CCORD_OK) return st;
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  const ccord_message *target = ccord_store_find_message(s->store, channel_id, reply_to_id);
  if (!target) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown reply target");
  }
  if (ccord_message_is_ephemeral(target)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "cannot reply to ephemeral message");
  }
  return ccord_transport_send_reply_ex(s->transport, channel_id, reply_to_id, mention_author, utf8, len, err);
}

ccord_status ccord_session_send_reply(ccord_session *s, ccord_id channel_id, ccord_id reply_to_id,
                                      const char *utf8, size_t len, ccord_error *err)
{
  return ccord_session_send_reply_ex(s, channel_id, reply_to_id, true, utf8, len, err);
}

ccord_status ccord_session_toggle_pin(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  return ccord_transport_toggle_pin(s->transport, channel_id, message_id, !m->pinned, err);
}

ccord_status ccord_session_request_pins(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: channel id");
  if (!ccord_store_channel(s->store, channel_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  pins_clear_in_flight(s, channel_id);
  return ccord_transport_request_pins(s->transport, channel_id, 50u, err);
}

ccord_status ccord_session_request_more_pins(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: channel id");
  if (!ccord_store_channel(s->store, channel_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!ccord_store_channel_pins_has_more(s->store, channel_id)) return CCORD_OK;
  const char *cursor = ccord_store_channel_pins_cursor(s->store, channel_id);
  if (!cursor || cursor[0] == '\0') return CCORD_OK;
  if (pins_is_in_flight(s, channel_id)) return CCORD_OK;
  ccord_status st = ccord_transport_request_pins_before(s->transport, channel_id, cursor, 50u, err);
  if (st == CCORD_OK) pins_mark_in_flight(s, channel_id);
  return st;
}

bool ccord_session_pins_loading(const ccord_session *s, ccord_id channel_id)
{
  return pins_is_in_flight(s, channel_id);
}

ccord_status ccord_session_request_history_before(ccord_session *s, ccord_id channel_id, ccord_id before_id,
                                                  ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || before_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: history ids");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  session_channel_hist *chist = chan_hist_for(s, channel_id);
  if (chist) {
    if (!chist->has_more) return CCORD_OK;
    if (chist->in_flight_before_id != CCORD_ID_NONE) return CCORD_OK;
  }
  ccord_status st =
    ccord_transport_request_history_before(s->transport, channel_id, before_id, CCORD_SESSION_HISTORY_LIMIT, err);
  if (st == CCORD_OK && chist) {
    chist->in_flight_before_id = before_id;
  }
  return st;
}

bool ccord_session_history_has_more(const ccord_session *s, ccord_id channel_id)
{
  if (!s || channel_id == CCORD_ID_NONE) return false;
  if (!ccord_store_channel(s->store, channel_id)) return false;
  const session_channel_hist *chist = find_chan_hist(s, channel_id);
  if (chist) return chist->has_more;
  if (ccord_store_history_loaded(s->store, channel_id)) {
    return ccord_store_message_count(s->store, channel_id) >= CCORD_SESSION_HISTORY_LIMIT;
  }
  return true;
}

static void split_stored_emoji(const char *stored, char name_out[CCORD_EMOJI_MAX_BYTES + 1], ccord_id *id_out)
{
  name_out[0] = '\0';
  *id_out = CCORD_ID_NONE;
  if (!stored || stored[0] == '\0') return;
  const char *colon = strrchr(stored, ':');
  if (colon && colon > stored && colon[1] != '\0') {
    uint64_t eid = 0;
    bool digits = true;
    for (const char *p = colon + 1; *p; p++) {
      if (*p < '0' || *p > '9') { digits = false; break; }
      eid = eid * 10u + (uint64_t)(*p - '0');
    }
    if (digits && eid > 0) {
      size_t nlen = (size_t)(colon - stored);
      if (nlen > CCORD_EMOJI_MAX_BYTES) nlen = CCORD_EMOJI_MAX_BYTES;
      memcpy(name_out, stored, nlen);
      name_out[nlen] = '\0';
      *id_out = eid;
      return;
    }
  }
  size_t len = strlen(stored);
  if (len > CCORD_EMOJI_MAX_BYTES) len = CCORD_EMOJI_MAX_BYTES;
  memcpy(name_out, stored, len);
  name_out[len] = '\0';
}

// The presence to broadcast when none was asked for: the last accepted set_presence wins until the server
// echoes it back, then the store's view of self.
static ccord_presence session_current_presence(const ccord_session *s)
{
  if (s->has_pending_presence) return s->pending_presence;
  const ccord_user *me = ccord_store_user(s->store, ccord_store_self_id(s->store));
  return me ? me->presence : CCORD_PRESENCE_ONLINE;
}

// Broadcasts `presence` together with the stored custom status (the set_presence path without a status text).
static ccord_status session_send_presence_with_custom(ccord_session *s, ccord_presence presence, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  const ccord_user *me = ccord_store_user(s->store, ccord_store_self_id(s->store));
  if (me && (me->custom_status_emoji[0] != '\0' || me->custom_status_text[0] != '\0')) {
    char ename[CCORD_EMOJI_MAX_BYTES + 1];
    ccord_id eid = CCORD_ID_NONE;
    split_stored_emoji(me->custom_status_emoji, ename, &eid);
    return ccord_transport_set_presence_ex(s->transport, presence, ename[0] ? ename : nullptr, eid,
                                           me->custom_status_text[0] ? me->custom_status_text : nullptr, err);
  }
  return ccord_transport_set_presence(s->transport, presence, nullptr, err);
}

// ---- status through settings-proto/1 ----
// Op 3 and the legacy settings PATCH reach this connection and the account's old settings, but other open clients
// follow the settings proto, so every status change is also written there (the transport patches the account's
// current type-1 bytes, keeping everything else). A transport without it keeps what it already sent.

static const char *const k_status_names[] = {
  [CCORD_PRESENCE_OFFLINE] = "invisible", [CCORD_PRESENCE_ONLINE] = "online",
  [CCORD_PRESENCE_IDLE] = "idle",         [CCORD_PRESENCE_DND] = "dnd",
};

// Fills the custom-status half of `ps` from the store's encoding ("name:id" for a custom emoji). Nothing set = none.
// False when a string does not fit the wire struct (the write is skipped rather than truncated).
static bool status_proto_custom(ccord_proto_status *ps, const char *stored_emoji, const char *text, int64_t expires_ms)
{
  char ename[CCORD_EMOJI_MAX_BYTES + 1];
  ccord_id eid = CCORD_ID_NONE;
  split_stored_emoji(stored_emoji ? stored_emoji : "", ename, &eid);
  if (!text) text = "";
  if (ename[0] == '\0' && text[0] == '\0') return true;
  if (strlen(text) > CCORD_PROTO_CUSTOM_TEXT_MAX || strlen(ename) > CCORD_PROTO_EMOJI_NAME_MAX) return false;
  ps->has_custom = true;
  memcpy(ps->custom_text, text, strlen(text) + 1u);
  memcpy(ps->emoji_name, ename, strlen(ename) + 1u);
  ps->emoji_id = eid;
  ps->custom_expires_at_ms = expires_ms > 0 ? (uint64_t)expires_ms : 0u;
  return true;
}

static void session_write_status_proto(ccord_session *s, ccord_presence presence, const char *stored_emoji,
                                       const char *text, int64_t expires_ms)
{
  if (presence > CCORD_PRESENCE_DND) return;
  ccord_proto_status ps = {0};
  ps.has_status = true;
  memcpy(ps.status, k_status_names[presence], strlen(k_status_names[presence]) + 1u);
  if (!status_proto_custom(&ps, stored_emoji, text, expires_ms)) {
    CCORD_LOG_W("session: status proto not written: custom status too long");
    return;
  }
  ccord_error err;
  ccord_status st = ccord_transport_set_status_proto(s->transport, &ps, &err);
  if (st != CCORD_OK && st != CCORD_ERR_UNSUPPORTED) CCORD_LOG_W("session: status proto write rejected: %s", err.message);
  if (st == CCORD_OK) {   // remember it so its echo is not taken for another client's change
    typeof(s->status_echo[0]) *e = &s->status_echo[s->status_echo_next++ % (sizeof s->status_echo / sizeof s->status_echo[0])];
    e->used = true;
    e->presence = presence;
    snprintf(e->emoji, sizeof e->emoji, "%s", stored_emoji ? stored_emoji : "");
    snprintf(e->text, sizeof e->text, "%s", text ? text : "");
    e->at_ms = ccord_clock_ms();
  }
}

// Another client changed the account's status: type-1 bytes arrived (a USER_SETTINGS_PROTO_UPDATE). Only a proto that
// carries the status submessage says anything; the presence and custom status of the store follow it and op 3 goes out
// again so this connection shows the same. In a partial (delta) update an absent custom_status means "unchanged".
static void session_apply_status_proto(ccord_session *s, const ccord_net_event *ev)
{
  if (!ev->settings_proto1 || ev->settings_proto1_len == 0) return;
  ccord_proto_status ps;
  ccord_error err;
  if (ccord_user_status_decode(ev->settings_proto1, ev->settings_proto1_len, &ps, &err) != CCORD_OK) {
    CCORD_LOG_W("session: settings proto status undecodable: %s", err.message);
    return;
  }
  ccord_id self = ccord_store_self_id(s->store);
  const ccord_user *me = self != CCORD_ID_NONE ? ccord_store_user(s->store, self) : nullptr;
  if (!ps.present || !me) return;

  ccord_presence cur = session_current_presence(s), np = cur;
  if (ps.has_status) {
    for (size_t i = 0; i < sizeof k_status_names / sizeof k_status_names[0]; i++) {
      if (strcmp(ps.status, k_status_names[i]) == 0) np = (ccord_presence)i;
    }
  }
  char emoji[CCORD_EMOJI_MAX_BYTES + 1];
  char text[129];   // the store keeps at most 128 bytes of status text
  int64_t exp_ms = me->custom_status_expires_ms;
  bool clear_custom = false;
  snprintf(emoji, sizeof emoji, "%s", me->custom_status_emoji);
  snprintf(text, sizeof text, "%s", me->custom_status_text);
  if (ps.has_custom && (strlen(ps.custom_text) > sizeof text - 1u || strlen(ps.emoji_name) > 40u)) {
    CCORD_LOG_W("session: custom status from settings proto too long, ignored");
  } else if (ps.has_custom) {
    if (ps.emoji_id != 0 && ps.emoji_name[0] != '\0') {
      snprintf(emoji, sizeof emoji, "%.40s:%llu", ps.emoji_name, (unsigned long long)ps.emoji_id);
    } else {
      snprintf(emoji, sizeof emoji, "%.40s", ps.emoji_name);
    }
    // The guard above already ignored text longer than 128 bytes; the clamp only lets the compiler prove the copy fits.
    size_t text_len = strlen(ps.custom_text);
    if (text_len > sizeof text - 1u) text_len = sizeof text - 1u;
    memcpy(text, ps.custom_text, text_len);
    text[text_len] = '\0';
    exp_ms = (int64_t)ps.custom_expires_at_ms;
    clear_custom = emoji[0] == '\0' && text[0] == '\0';
  } else if (!ev->settings_proto1_partial) {
    emoji[0] = text[0] = '\0';
    exp_ms = 0;
    clear_custom = true;
  }
  bool custom_changed = strcmp(emoji, me->custom_status_emoji) != 0 || strcmp(text, me->custom_status_text) != 0 ||
                        (!clear_custom && exp_ms != me->custom_status_expires_ms);
  if (np == cur && !custom_changed && me->presence == cur) return;
  // The echo of one of our own writes that a newer write has already replaced: ignore it (the newer echo follows). The
  // newest write is never skipped, so another client choosing the same status as our last write still applies.
  size_t latest = (s->status_echo_next + sizeof s->status_echo / sizeof s->status_echo[0] - 1u) %
                  (sizeof s->status_echo / sizeof s->status_echo[0]);
  for (size_t i = 0; i < sizeof s->status_echo / sizeof s->status_echo[0]; i++) {
    typeof(s->status_echo[0]) *e = &s->status_echo[i];
    if (!e->used || i == latest || ccord_clock_ms() - e->at_ms >= 10000u) continue;
    if (e->presence == np && strcmp(e->emoji, emoji) == 0 && strcmp(e->text, text) == 0) {
      e->used = false;   // consumed
      return;
    }
  }

  if (np != cur || me->presence != np) {
    char *keep = ccord_strdup(me->status_text ? me->status_text : "");
    if (ccord_store_update_presence(s->store, self, np, keep, &err) != CCORD_OK) {
      CCORD_LOG_W("session: status from settings proto not applied: %s", err.message);
    }
    ccord_free(keep);
  }
  if (custom_changed &&
      ccord_store_set_self_custom_status(s->store, emoji, text, exp_ms, clear_custom, &err) != CCORD_OK) {
    CCORD_LOG_W("session: custom status from settings proto not applied: %s", err.message);
  }
  s->has_pending_presence = false;   // the store now holds what the account says
  if (session_send_presence_with_custom(s, np, &err) != CCORD_OK) {
    CCORD_LOG_D("session: status op 3 not re-sent: %s", err.message);
  }
}

// A new READY (SNAPSHOT) belongs to a fresh gateway that knows nothing about the Rich Presence activities:
// push the stored array again and re-broadcast the presence so the activity goes out without waiting for the
// next status change. No-op when no activity is stored.
static void session_repush_rich_presence(ccord_session *s)
{
  if (!s->rich_json || s->stale_has_rich) return;   // a change held back while stale goes out by the replay
  ccord_error err;
  if (ccord_transport_set_rich_activities(s->transport, s->rich_json, s->rich_len, &err) != CCORD_OK) {
    CCORD_LOG_W("session: rich presence re-push rejected: %s", err.message);
    return;
  }
  if (session_send_presence_with_custom(s, session_current_presence(s), &err) != CCORD_OK) {
    CCORD_LOG_W("session: rich presence resend rejected: %s", err.message);
  } else {
    s->rich_op3_dirty = false;
  }
}

// READY cleared stale: send what was held back while it was set (each once).
static void session_replay_stale(ccord_session *s)
{
  ccord_error err;
  if (s->stale_has_rich) {
    char *json = s->stale_rich;
    s->stale_rich = nullptr;
    s->stale_has_rich = false;
    if (ccord_session_set_rich_presence(s, json, &err) != CCORD_OK) {
      CCORD_LOG_W("session: deferred rich presence rejected: %s", err.message);
    }
    ccord_free(json);
  }
  if (s->stale_has_voice_cfg) {
    s->stale_has_voice_cfg = false;
    if (ccord_session_set_voice_config(s, &s->stale_voice_cfg, &err) != CCORD_OK) {
      CCORD_LOG_W("session: deferred voice config rejected: %s", err.message);
    }
  }
  size_t n = s->nstale_expr;
  s->nstale_expr = 0;
  for (size_t i = 0; i < n; i++) {
    (void)ccord_session_request_expression_data(s, s->stale_expr[i].kind, s->stale_expr[i].arg, false, &err);
  }
  if (s->stale_slash_channel != CCORD_ID_NONE) {
    ccord_id g = s->stale_slash_guild, c = s->stale_slash_channel;
    s->stale_slash_guild = s->stale_slash_channel = CCORD_ID_NONE;
    uint64_t now = ccord_clock_ms();
    (void)ccord_session_ensure_slash_commands(s, g, c, now ? now : 1u);
  }
}

ccord_status ccord_session_set_presence(ccord_session *s, ccord_presence presence, const char *status_text,
                                        ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  // CCORD_PRESENCE_OFFLINE as a request means Invisible (the gateway status "invisible").
  if (presence > CCORD_PRESENCE_DND) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: presence");
  }
  ccord_status st;
  if (status_text) {
    size_t n = strlen(status_text);
    if (n > 128u || !ccord_utf8_valid_text(status_text, n)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: presence text");
    }
    st = ccord_transport_set_presence(s->transport, presence, status_text, err);
  } else {
    st = session_send_presence_with_custom(s, presence, err);
  }
  if (st != CCORD_OK) return st;
  s->has_pending_presence = true;
  s->pending_presence = presence;
  // Discord does not reliably echo our own op 3 back as a PRESENCE_UPDATE, so the store (and with it the user panel)
  // would keep showing the old status forever: apply it here. The custom status is left as it is.
  ccord_id self = ccord_store_self_id(s->store);
  if (self != CCORD_ID_NONE) {
    ccord_error serr;
    if (ccord_store_update_presence(s->store, self, presence, status_text, &serr) != CCORD_OK) {
      CCORD_LOG_W("session: own presence not applied locally: %s", serr.message);
    }
  }
  {   // other open clients follow the settings proto, not op 3 (the custom status stays as the store has it)
    const ccord_user *own = self != CCORD_ID_NONE ? ccord_store_user(s->store, self) : nullptr;
    session_write_status_proto(s, presence, own ? own->custom_status_emoji : nullptr,
                               own ? own->custom_status_text : nullptr, own ? own->custom_status_expires_ms : 0);
  }
  // Op 3 only changes this connection. The account's status setting is what survives a reconnect and what other
  // sessions follow, so persist it the way the web client does. A refusal comes back as an action failure.
  ccord_admin_request req;
  st = ccord_admin_set_status(&req, presence, err);
  if (st != CCORD_OK) return st;
  st = ccord_session_admin(s, &req, err);
  // A transport with no account REST (a bare gateway) keeps the op 3 it already accepted.
  if (st == CCORD_ERR_UNSUPPORTED) {
    ccord_error_clear(err);
    return CCORD_OK;
  }
  return st;
}

ccord_status ccord_session_set_rich_presence(ccord_session *s, const char *activities_json, ccord_error *err)
{
  if (s && s->stale) {   // refused now, remembered, applied once after READY
    char *copy = activities_json && activities_json[0] ? ccord_strndup(activities_json, strlen(activities_json)) : nullptr;
    if (copy || !activities_json || !activities_json[0]) {
      ccord_free(s->stale_rich);
      s->stale_rich = copy;
      s->stale_has_rich = true;
    }
  }
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  size_t len = activities_json ? strlen(activities_json) : 0;
  char *copy = nullptr;
  if (len > 0) {
    copy = ccord_strndup(activities_json, len);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
  }
  // The transport validates the array (shape and size limits); a rejected set keeps the stored one.
  ccord_status st = ccord_transport_set_rich_activities(s->transport, activities_json, len, err);
  if (st != CCORD_OK) {
    ccord_free(copy);
    return st;
  }
  {   // Discord may never echo our own presence back: an activity we published and now withdraw ends here too
    ccord_id self = ccord_store_self_id(s->store);
    const ccord_user *me = self != CCORD_ID_NONE ? ccord_store_user(s->store, self) : nullptr;
    if (me && me->has_activity &&
        (rich_json_has_name(s->rich_json, me->activity_name) || s->self_activity_from_rich) &&
        !rich_json_has_name(copy, me->activity_name)) {
      session_set_self_activity(s, false, CCORD_ACTIVITY_PLAYING, "", "", "", 0, 0);
      s->self_activity_from_rich = false;
    }
  }
  ccord_free(s->rich_json);
  ccord_free(s->stale_rich);
  s->stale_rich = nullptr;
  s->stale_has_rich = false;
  s->rich_json = copy;
  s->rich_len = copy ? len : 0;
  s->self_activity_from_rich = rich_json_has_any(copy);
  s->rich_op3_dirty = true;
  if (s->state == CCORD_CONN_CONNECTED) {
    ccord_status gw_st = session_send_presence_with_custom(s, session_current_presence(s), err);
    if (gw_st != CCORD_OK) return gw_st;
    s->rich_op3_dirty = false;
  }
  return CCORD_OK;
}

ccord_status ccord_session_set_voice_camera(ccord_session *s, bool enabled, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_voice_state *v = find_active_self_voice(s);
  if (!v) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: not connected to voice");
  }
  return ccord_transport_set_voice_camera(s->transport, v->guild_id, enabled, err);
}

static ccord_status session_fetch_media(ccord_session *s, const char *url, bool user, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!url || url[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty media url");
  uint32_t h32 = media_hash_mix(media_url_hash(url));
  size_t midx = media_find_index(s, url, h32);
  if (midx != SIZE_MAX) {
    s->media[midx].access_tick = ++s->media_tick;
    media_unread_push(s, midx);
    return CCORD_OK;
  }
  if (media_req_find_index(s, url, h32) != SIZE_MAX) {
    return CCORD_OK;
  }
  ccord_status st = user ? ccord_transport_fetch_media_user(s->transport, url, err)
                         : ccord_transport_fetch_media(s->transport, url, err);
  if (st == CCORD_ERR_PERMISSION || st == CCORD_ERR_UNSUPPORTED) media_failed_set(s, url, true);
  if (st == CCORD_OK) {
    media_failed_set(s, url, false);   // a new attempt is under way
    char *copy = ccord_strndup(url, strlen(url));
    if (copy) {
      if (s->media_req_count >= SESSION_MAX_MEDIA) {
        media_req_remove_at(s, 0u);
      }
      size_t ridx = s->media_req_count++;
      s->media_requested[ridx] = copy;
      s->media_req_hashes[ridx] = h32;
      media_req_hash_insert(s, ridx, h32);
    }
  }
  return st;
}

ccord_media_stream *ccord_session_open_media_stream(ccord_session *s, const char *url, ccord_error *err)
{
  if (!s) {
    ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
    return nullptr;
  }
  return ccord_stream_hub_open(s->streams, url, err);
}

void ccord_session_close_media_stream(ccord_session *s, ccord_media_stream *ms)
{
  if (s) ccord_stream_hub_close(s->streams, ms);
}

ccord_status ccord_session_fetch_media(ccord_session *s, const char *url, ccord_error *err)
{
  return session_fetch_media(s, url, false, err);
}

ccord_status ccord_session_fetch_media_user(ccord_session *s, const char *url, ccord_error *err)
{
  return session_fetch_media(s, url, true, err);
}

ccord_status ccord_session_request_media(ccord_session *s, const char *url, ccord_error *err)
{
  return ccord_session_fetch_media(s, url, err);
}

bool ccord_session_pop_media(ccord_session *s, ccord_media_item *out)
{
  if (!s || !out || s->media_unread_head == 0u) return false;
  size_t idx = (size_t)s->media_unread_head - 1u;
  media_unread_remove(s, idx);
  *out = s->media[idx].pub;
  return true;
}

bool ccord_session_pop_media_matching(ccord_session *s, const char *url_substring,
                                      const char **url, const uint8_t **data, size_t *len)
{
  if (!s || s->media_unread_count == 0u || !url_substring || url_substring[0] == '\0') return false;
  uint16_t cur = s->media_unread_head;
  while (cur != 0u && cur != SESSION_MEDIA_IDX_END) {
    size_t idx = (size_t)cur - 1u;
    session_media_entry *e = &s->media[idx];
    if (e->url && strstr(e->url, url_substring) != nullptr) {
      media_unread_remove(s, idx);
      if (url) *url = e->pub.url;
      if (data) *data = e->pub.data;
      if (len) *len = e->pub.len;
      return true;
    }
    cur = e->unread_next;
  }
  return false;
}

const ccord_media_item *ccord_session_get_media(const ccord_session *s, const char *url)
{
  if (!s || s->media_count == 0u || !url || url[0] == '\0') return nullptr;
  uint32_t h32 = media_hash_mix(media_url_hash(url));
  size_t idx = media_find_index(s, url, h32);
  return idx != SIZE_MAX ? &s->media[idx].pub : nullptr;
}

bool ccord_session_media_failed(const ccord_session *s, const char *url)
{
  if (!s || s->media_failed_count == 0u || !url || url[0] == '\0') return false;
  uint64_t h = media_url_hash(url);
  for (size_t i = 0; i < s->media_failed_count; i++) {
    if (s->media_failed[(s->media_failed_head + i) % SESSION_MEDIA_FAILED_MAX] == h) return true;
  }
  return false;
}

ccord_status ccord_session_send_message_with_files(ccord_session *s, ccord_id channel_id,
                                                   ccord_id reply_to_id, bool mention_author,
                                                   const char *utf8, size_t len,
                                                   const ccord_upload_file *files, size_t file_count,
                                                   ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (file_count > CCORD_RICH_MAX_ATTACHMENTS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "session: too many attachments");
  }
  if (!files || file_count == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty attachment list");
  }
  for (size_t i = 0; i < file_count; i++) {
    if (!files[i].filename || files[i].filename[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty filename");
    }
    if (!files[i].data || files[i].size == 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty file data");
    }
  }
  if (len > CCORD_SEND_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message is longer than %u bytes", CCORD_SEND_MAX_BYTES);
  }
  if (len > 0 && (!utf8 || memchr(utf8, 0, len) || !ccord_utf8_valid_text(utf8, len))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message is not valid text");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  if (reply_to_id != CCORD_ID_NONE) {
    const ccord_message *rt = ccord_store_find_message(s->store, channel_id, reply_to_id);
    if (!rt) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown reply target");
    if (ccord_message_is_ephemeral(rt)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "cannot reply to ephemeral message");
    }
  }
  return ccord_transport_send_message_with_files(s->transport, channel_id, reply_to_id, mention_author,
                                                 utf8 ? utf8 : "", len, files, file_count, err);
}

ccord_status ccord_session_send_poll(ccord_session *s, ccord_id channel_id, const ccord_poll_create *poll,
                                     ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!poll) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null poll");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before posting a poll.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  return ccord_transport_create_poll(s->transport, channel_id, poll, err);
}

ccord_status ccord_session_report_menu_request(ccord_session *s, ccord_report_type type, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!ccord_report_type_name(type)) return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad report type");
  return ccord_transport_request_report_menu(s->transport, type, err);
}

ccord_status ccord_session_report_submit(ccord_session *s, const ccord_report_request *req, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!req || !ccord_report_type_name(req->type)) return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad report");
  ccord_id self = ccord_store_self_id(s->store);
  if (req->type == CCORD_REPORT_MESSAGE) {
    const ccord_message *m = req->channel_id != CCORD_ID_NONE && req->message_id != CCORD_ID_NONE
                               ? ccord_store_find_message(s->store, req->channel_id, req->message_id)
                               : nullptr;
    if (!m) return ccord_error_set(err, CCORD_ERR_INVALID, "That message is no longer available to report.");
    if (m->send_state != CCORD_MSG_SEND_CONFIRMED || ccord_message_is_ephemeral(m)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "That message can't be reported.");
    }
    if (m->author_id == self) return ccord_error_set(err, CCORD_ERR_INVALID, "You can't report your own message.");
  } else {
    if (req->reported_user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad report");
    if (req->reported_user_id == self) return ccord_error_set(err, CCORD_ERR_INVALID, "You can't report yourself.");
  }
  return ccord_transport_submit_report(s->transport, req, err);
}

ccord_status ccord_session_send_message_with_file(ccord_session *s, ccord_id channel_id,
                                                  const char *utf8, size_t len,
                                                  const char *filename, const void *file_data,
                                                  size_t file_size, ccord_error *err)
{
  ccord_upload_file single = {
    .filename = filename,
    .data = file_data,
    .size = file_size,
    .spoiler = false,
  };
  return ccord_session_send_message_with_files(s, channel_id, CCORD_ID_NONE, true, utf8, len, &single, 1u, err);
}

ccord_status ccord_session_send_voice_message(ccord_session *s, ccord_id channel_id,
                                              const uint8_t *ogg, size_t len, uint32_t duration_ms,
                                              const char *waveform_b64, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !ogg) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (len == 0 || duration_ms == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice message: empty clip or duration");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  ccord_id self = ccord_store_self_id(s->store);
  if (ch->guild_id != CCORD_DM_GUILD_ID && ccord_store_permissions_exact(s->store, ch->guild_id, self)) {
    uint64_t ch_perms = ccord_store_permissions(s->store, ch->guild_id, channel_id, self, session_now_unix_ms(s));
    uint64_t send_bit = ccord_discord_type_is_thread(ch->discord_type) ? CCORD_PERM_SEND_MESSAGES_IN_THREADS
                                                                       : CCORD_PERM_SEND_MESSAGES;
    if ((ch_perms & send_bit) == 0u || (ch_perms & CCORD_PERM_ATTACH_FILES) == 0u) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION,
                             "You need Send Messages and Attach Files permissions in this channel.");
    }
  }
  ccord_upload_file voice = {
    .filename = "voice-message.ogg",
    .data = ogg,
    .size = len,
    .spoiler = false,
    .duration_ms = duration_ms,
    .waveform_b64 = waveform_b64,
  };
  return ccord_session_send_message_with_files(s, channel_id, CCORD_ID_NONE, true, "", 0, &voice, 1u, err);
}

size_t ccord_session_active_uploads(const ccord_session *s, ccord_id channel_id,
                                    ccord_upload_progress *out, size_t max)
{
  if (!s || !s->transport) return 0;
  if (channel_id == CCORD_ID_NONE) {
    return ccord_transport_active_uploads(s->transport, out, max);
  }
  ccord_upload_progress buf[32];
  size_t n = ccord_transport_active_uploads(s->transport, buf, sizeof buf / sizeof buf[0]);
  size_t scanned = n < (sizeof buf / sizeof buf[0]) ? n : (sizeof buf / sizeof buf[0]);
  size_t matched = 0;
  for (size_t i = 0; i < scanned; i++) {
    if (buf[i].channel_id != channel_id) continue;
    if (out && matched < max) out[matched] = buf[i];
    matched++;
  }
  return matched;
}

static bool str_eq_ascii_ci(const char *a, const char *b)
{
  if (!a || !b) return false;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a++;
    unsigned char cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
    if (ca != cb) return false;
  }
  return *a == '\0' && *b == '\0';
}

static ccord_id resolve_search_user(const ccord_store *st, ccord_id guild_id, const char *needle)
{
  if (!st || !needle || needle[0] == '\0') return CCORD_ID_NONE;
  if (str_eq_ascii_ci(needle, "me")) return ccord_store_self_id(st);

  bool in_guild = guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID;
  // Pass 1: exact case-insensitive match on handle, server nick (in guild), or display name.
  if (in_guild) {
    size_t nm = ccord_store_member_count(st, guild_id);
    for (size_t i = 0; i < nm; i++) {
      const ccord_member *m = ccord_store_member_at(st, guild_id, i);
      if (!m) continue;
      if (m->nick[0] != '\0' && str_eq_ascii_ci(m->nick, needle)) return m->user_id;
      const ccord_user *u = ccord_store_user(st, m->user_id);
      if (u && ((u->handle && str_eq_ascii_ci(u->handle, needle)) ||
                (u->name && str_eq_ascii_ci(u->name, needle)))) {
        return u->id;
      }
    }
  }
  size_t nu = ccord_store_user_count(st);
  for (size_t i = 0; i < nu; i++) {
    const ccord_user *u = ccord_store_user_at(st, i);
    if (!u) continue;
    if ((u->handle && str_eq_ascii_ci(u->handle, needle)) ||
        (u->name && str_eq_ascii_ci(u->name, needle))) {
      return u->id;
    }
  }
  // Pass 2: case-insensitive substring match (guild members first, then known users).
  if (in_guild) {
    size_t nm = ccord_store_member_count(st, guild_id);
    for (size_t i = 0; i < nm; i++) {
      const ccord_member *m = ccord_store_member_at(st, guild_id, i);
      if (!m) continue;
      if (m->nick[0] != '\0' && ccord_search_contains_ci(m->nick, needle)) return m->user_id;
      const ccord_user *u = ccord_store_user(st, m->user_id);
      if (u && ((u->handle && ccord_search_contains_ci(u->handle, needle)) ||
                (u->name && ccord_search_contains_ci(u->name, needle)))) {
        return u->id;
      }
    }
  }
  for (size_t i = 0; i < nu; i++) {
    const ccord_user *u = ccord_store_user_at(st, i);
    if (!u) continue;
    if ((u->handle && ccord_search_contains_ci(u->handle, needle)) ||
        (u->name && ccord_search_contains_ci(u->name, needle))) {
      return u->id;
    }
  }
  return CCORD_ID_NONE;
}

static ccord_id resolve_search_channel(const ccord_store *st, ccord_id guild_id, const char *needle)
{
  if (!st || guild_id == CCORD_ID_NONE || !needle || needle[0] == '\0') return CCORD_ID_NONE;
  size_t nc = ccord_store_channel_count(st, guild_id);
  for (size_t i = 0; i < nc; i++) {
    const ccord_channel *c = ccord_store_channel_at(st, guild_id, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->name && str_eq_ascii_ci(c->name, needle)) return c->id;
  }
  for (size_t i = 0; i < nc; i++) {
    const ccord_channel *c = ccord_store_channel_at(st, guild_id, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->name && ccord_search_contains_ci(c->name, needle)) return c->id;
  }
  return CCORD_ID_NONE;
}

ccord_status ccord_session_search_messages_page(ccord_session *s, ccord_id guild_id, ccord_id channel_id,
                                                const char *query, size_t query_len, uint32_t offset,
                                                ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!query || query_len == 0 || blank(query, query_len) ||
      memchr(query, 0, query_len) || !ccord_utf8_valid_text(query, query_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid search query");
  }
  if (guild_id == CCORD_ID_NONE && channel_id == CCORD_ID_NONE) {
    channel_id = ccord_store_selected_channel(s->store);
    guild_id = ccord_store_selected_guild(s->store);
  }
  ccord_search_query sq;
  ccord_search_parse(query, query_len, &sq);
  sq.offset = offset;
  if (sq.author_id == CCORD_ID_NONE && sq.from[0] != '\0') {
    sq.author_id = resolve_search_user(s->store, guild_id, sq.from);
  }
  if (sq.mentions_id == CCORD_ID_NONE && sq.mentions[0] != '\0') {
    sq.mentions_id = resolve_search_user(s->store, guild_id, sq.mentions);
  }
  if (sq.channel_id == CCORD_ID_NONE && sq.in_channel[0] != '\0' && guild_id != CCORD_ID_NONE) {
    sq.channel_id = resolve_search_channel(s->store, guild_id, sq.in_channel);
  }
  ccord_status st = ccord_transport_search_messages_ex(s->transport, guild_id, channel_id, &sq, err);
  if (st == CCORD_OK) s->search_loading = true;
  return st;
}

ccord_status ccord_session_search_messages(ccord_session *s, ccord_id guild_id, ccord_id channel_id,
                                           const char *query, size_t query_len, ccord_error *err)
{
  return ccord_session_search_messages_page(s, guild_id, channel_id, query, query_len, 0u, err);
}

size_t ccord_session_search_result_count(const ccord_session *s)
{
  return s ? s->search_count : 0;
}

const ccord_message *ccord_session_search_result_at(const ccord_session *s, size_t index)
{
  if (!s || index >= s->search_count) return nullptr;
  return &s->search_results[index];
}

const ccord_message *ccord_session_search_results(const ccord_session *s, size_t *out_count)
{
  if (out_count) *out_count = s ? s->search_count : 0;
  return s ? s->search_results : nullptr;
}

uint32_t ccord_session_search_total_results(const ccord_session *s)
{
  return s ? s->search_total_results : 0u;
}

uint32_t ccord_session_search_offset(const ccord_session *s)
{
  return s ? s->search_offset : 0u;
}

bool ccord_session_search_loading(const ccord_session *s)
{
  return s && s->search_loading;
}

// What the roles Discord told us say about a role request, but only when they are complete: with partial data
// everything is left to Discord (it has the final word). Returns CCORD_PERM_ALLOWED when nothing is certain.
static ccord_perm_verdict session_role_verdict(const ccord_session *s, const ccord_admin_request *req, ccord_id self)
{
  const ccord_store *st = s->store;
  if (!ccord_store_permissions_exact(st, req->guild_id, self)) return CCORD_PERM_ALLOWED;
  bool assign = req->op == CCORD_ADMIN_ADD_MEMBER_ROLE || req->op == CCORD_ADMIN_REMOVE_MEMBER_ROLE;
  const ccord_guild *g = ccord_store_guild(st, req->guild_id);
  uint64_t mine = ccord_store_permissions(st, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
  ccord_id owner = g ? g->owner_id : CCORD_ID_NONE;
  switch (req->op) {
    case CCORD_ADMIN_CREATE_ROLE:
      if (req->role_permissions > 0) {
        return ccord_perm_can_grant(req->guild_id, owner, self, mine, 0, (uint64_t)req->role_permissions);
      }
      return CCORD_PERM_ALLOWED;
    case CCORD_ADMIN_EDIT_ROLE: {
      ccord_perm_verdict v = ccord_store_can_edit_role(st, req->guild_id, self, req->role_id, false);
      const ccord_role *r = ccord_store_role(st, req->guild_id, req->role_id);
      if (v == CCORD_PERM_ALLOWED && r && req->role_permissions >= 0) {
        v = ccord_perm_can_grant(req->guild_id, owner, self, mine, r->permissions, (uint64_t)req->role_permissions);
      }
      return v;
    }
    case CCORD_ADMIN_REORDER_ROLES:
      for (size_t i = 0; i < req->reorder_count; i++) {
        ccord_perm_verdict v = ccord_store_can_edit_role(st, req->guild_id, self, req->reorder_ids[i], false);
        if (v != CCORD_PERM_ALLOWED) return v;
      }
      return CCORD_PERM_ALLOWED;
    case CCORD_ADMIN_DELETE_ROLE:
    case CCORD_ADMIN_ADD_MEMBER_ROLE:
    case CCORD_ADMIN_REMOVE_MEMBER_ROLE:
      return ccord_store_can_edit_role(st, req->guild_id, self, req->role_id, assign);
    default: return CCORD_PERM_ALLOWED;
  }
}

ccord_status ccord_session_admin_ex(ccord_session *s, const ccord_admin_request *req, uint64_t now_ms, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s || !req) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (req->op == CCORD_ADMIN_NONE || req->op >= CCORD_ADMIN_OP_COUNT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad admin request");
  }
  ccord_id self = ccord_store_self_id(s->store);
  // The direct messages are not a server we hold a record of: closing one, and muting them, address them by the pseudo id.
  // Account-global operations (guild folders, creating a server, previewing/accepting an invite, fetching a public
  // server/clan profile) also need no store guild.
  bool dm = req->op == CCORD_ADMIN_CLOSE_DM || req->op == CCORD_ADMIN_ADD_DM_RECIPIENT ||
            req->op == CCORD_ADMIN_REMOVE_DM_RECIPIENT || req->op == CCORD_ADMIN_EDIT_GROUP_DM ||
            req->op == CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST || req->op == CCORD_ADMIN_REJECT_MESSAGE_REQUEST ||
            (req->op == CCORD_ADMIN_SET_NOTIFY && req->guild_id == CCORD_DM_GUILD_ID);
  bool global_op = req->op == CCORD_ADMIN_SET_GUILD_FOLDERS || req->op == CCORD_ADMIN_CREATE_GUILD ||
                   req->op == CCORD_ADMIN_PREVIEW_INVITE || req->op == CCORD_ADMIN_ACCEPT_INVITE ||
                   req->op == CCORD_ADMIN_GET_GUILD_PROFILE ||
                   req->op == CCORD_ADMIN_CREATE_GROUP_DM || req->op == CCORD_ADMIN_SET_USER_NOTE ||
                   req->op == CCORD_ADMIN_SET_CUSTOM_STATUS || req->op == CCORD_ADMIN_USER_SETTINGS_PATCH ||
                   req->op == CCORD_ADMIN_AUTH_SESSIONS_LIST || req->op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT ||
                   req->op == CCORD_ADMIN_CONNECTIONS_LIST || req->op == CCORD_ADMIN_CONNECTION_EDIT ||
                   req->op == CCORD_ADMIN_CONNECTION_DELETE;
  // Voice status, stage topic/end and activity launch/leave carry no guild id in their REST path. The first three
  // and LAUNCH look the channel row up in their cases below; LEAVE only checks the instance in the store.
  bool channel_op = req->op == CCORD_ADMIN_VOICE_STATUS || req->op == CCORD_ADMIN_STAGE_TOPIC ||
                    req->op == CCORD_ADMIN_STAGE_DELETE || req->op == CCORD_ADMIN_ACTIVITY_LAUNCH ||
                    req->op == CCORD_ADMIN_ACTIVITY_LEAVE;
  const ccord_guild *g = (dm || global_op || channel_op) ? nullptr : ccord_store_guild(s->store, req->guild_id);
  if (!dm && !global_op && !channel_op && !g) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That server isn't available.");
  // At -O3 the compiler cannot tie req->op back to the dm/global/channel classification above, so it sees the
  // guild-scoped cases below as reachable with a null row. Settle the two fields they read once, here, where the
  // impossible null case simply keeps the "unknown" defaults instead of dereferencing.
  ccord_id guild_owner = g ? g->owner_id : CCORD_ID_NONE;
  uint8_t guild_premium_tier = g ? g->premium_tier : 0u;
  switch (req->op) {
    case CCORD_ADMIN_EDIT_CHANNEL:
    case CCORD_ADMIN_DELETE_CHANNEL:
    case CCORD_ADMIN_CREATE_THREAD:
    case CCORD_ADMIN_CREATE_THREAD_MSG:
    case CCORD_ADMIN_ARCHIVE_THREAD:
    case CCORD_ADMIN_JOIN_THREAD:
    case CCORD_ADMIN_LEAVE_THREAD:
    case CCORD_ADMIN_CLOSE_DM:
    case CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST:
    case CCORD_ADMIN_REJECT_MESSAGE_REQUEST: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (dm ? ch->guild_id != CCORD_DM_GUILD_ID : ch->guild_id != req->guild_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't in this server.");
      }
      if (req->op == CCORD_ADMIN_CREATE_THREAD_MSG) {
        if (req->message_id == CCORD_ID_NONE ||
            !ccord_store_find_message(s->store, req->channel_id, req->message_id)) {
          return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That message isn't available.");
        }
      }
      if ((req->op == CCORD_ADMIN_EDIT_CHANNEL || req->op == CCORD_ADMIN_DELETE_CHANNEL) && !dm &&
          !ccord_discord_type_is_thread(ch->discord_type) &&
          ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, req->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Manage Channels permission for this channel.");
        }
      }
      break;
    }
    case CCORD_ADMIN_ADD_DM_RECIPIENT:
    case CCORD_ADMIN_REMOVE_DM_RECIPIENT:
    case CCORD_ADMIN_EDIT_GROUP_DM: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That conversation isn't available.");
      if (ch->guild_id != CCORD_DM_GUILD_ID || ch->discord_type != CCORD_DISCORD_CHANNEL_GROUP_DM) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That conversation isn't a Group DM.");
      }
      if (req->op == CCORD_ADMIN_ADD_DM_RECIPIENT) {
        if (req->user_id == CCORD_ID_NONE || req->user_id == self) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Choose someone else to add to the Group DM.");
        }
        if (ch->recipient_count >= CCORD_MAX_DM_RECIPIENTS) {
          return ccord_error_set(err, CCORD_ERR_RANGE, "Group DMs can have at most 10 members.");
        }
      } else if (req->op == CCORD_ADMIN_REMOVE_DM_RECIPIENT) {
        if (req->user_id == CCORD_ID_NONE) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Choose a member to remove.");
        }
        if (ch->owner_id != CCORD_ID_NONE && self != CCORD_ID_NONE && req->user_id != self && ch->owner_id != self) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "Only the group owner can remove members.");
        }
      }
      break;
    }
    case CCORD_ADMIN_CREATE_GROUP_DM: {
      if (req->dm_recipient_count == 0 || req->dm_recipient_count > CCORD_MAX_DM_RECIPIENTS) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Select between 1 and 9 recipients.");
      }
      for (size_t i = 0; i < req->dm_recipient_count; i++) {
        if (req->dm_recipient_ids[i] == CCORD_ID_NONE || req->dm_recipient_ids[i] == self) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Invalid Group DM recipient.");
        }
      }
      break;
    }
    case CCORD_ADMIN_SET_USER_NOTE:
      if (req->user_id == CCORD_ID_NONE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Invalid user for note.");
      }
      break;
    case CCORD_ADMIN_REORDER_CHANNELS: {
      if (req->reorder_count == 0 || req->reorder_count > CCORD_ADMIN_CHANNEL_REORDER_MAX) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid channel reorder count");
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_MANAGE_CHANNELS) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Manage Channels permission to reorder channels.");
        }
      }
      for (size_t i = 0; i < req->reorder_count; i++) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->reorder_ids[i]);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
        if (ch->guild_id != req->guild_id) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't in this server.");
        }
        if (req->reorder_has_parent[i] && req->reorder_parents[i] != CCORD_ID_NONE) {
          if (ch->kind == CCORD_CHANNEL_CATEGORY) {
            return ccord_error_set(err, CCORD_ERR_INVALID, "A category cannot sit inside another category.");
          }
          const ccord_channel *par = ccord_store_channel(s->store, req->reorder_parents[i]);
          if (!par || par->guild_id != req->guild_id || par->kind != CCORD_CHANNEL_CATEGORY) {
            return ccord_error_set(err, CCORD_ERR_INVALID, "Invalid parent category.");
          }
        }
      }
      break;
    }
    case CCORD_ADMIN_KICK:
    case CCORD_ADMIN_BAN:
    case CCORD_ADMIN_TIMEOUT:
    case CCORD_ADMIN_SET_NICK: {
      static const ccord_perm_action action_of[] = {
        [CCORD_ADMIN_KICK] = CCORD_PERM_ACT_KICK, [CCORD_ADMIN_BAN] = CCORD_PERM_ACT_BAN,
        [CCORD_ADMIN_TIMEOUT] = CCORD_PERM_ACT_TIMEOUT, [CCORD_ADMIN_SET_NICK] = CCORD_PERM_ACT_SET_NICKNAME,
      };
      ccord_perm_action action = action_of[req->op];
      ccord_perm_verdict v = ccord_store_can_act_on(s->store, action, req->guild_id, self, req->user_id);
      bool lifting = req->op == CCORD_ADMIN_TIMEOUT && req->until_ms == 0;
      // Only what is certain stops a request here: whom the action may not touch (yourself, the owner, someone at or
      // above your highest role). Whether you hold the permission is Discord's to say, since our picture of your
      // roles can lag; a refusal then comes back as a failure notice with Discord's own words.
      if (v == CCORD_PERM_TARGET_SELF) return ccord_error_set(err, CCORD_ERR_INVALID, "%s", ccord_perm_verdict_text(v, action));
      if (v == CCORD_PERM_TARGET_OWNER || v == CCORD_PERM_TARGET_HIGHER || (v == CCORD_PERM_TARGET_ADMIN && !lifting)) {
        return ccord_error_set(err, CCORD_ERR_PERMISSION, "%s", ccord_perm_verdict_text(v, action));
      }
      break;
    }
    case CCORD_ADMIN_CREATE_ROLE:
    case CCORD_ADMIN_EDIT_ROLE:
    case CCORD_ADMIN_DELETE_ROLE:
    case CCORD_ADMIN_REORDER_ROLES:
    case CCORD_ADMIN_ADD_MEMBER_ROLE:
    case CCORD_ADMIN_REMOVE_MEMBER_ROLE: {
      ccord_perm_verdict v = session_role_verdict(s, req, self);
      if (v == CCORD_PERM_TARGET_HIGHER || v == CCORD_PERM_ROLE_MANAGED || v == CCORD_PERM_ESCALATION) {
        return ccord_error_set(err, CCORD_ERR_PERMISSION, "%s", ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES));
      }
      break;
    }
    case CCORD_ADMIN_SET_NOTIFY:   // the user's own setting: no permission applies, but the channel must be one of this server's
      if (req->channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
        if (ch->guild_id != req->guild_id) return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't in this server.");
      }
      break;
    case CCORD_ADMIN_EDIT_GUILD:
      if (req->has_system_channel && req->system_channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->system_channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That system channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(ch->discord_type) || ccord_store_channel_is_forum(s->store, ch->id)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "System messages must point to a text channel in this server.");
        }
      }
      if (req->has_afk_channel && req->afk_channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->afk_channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That inactive channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_VOICE ||
            ch->discord_type == CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Inactive channel must be a voice channel in this server.");
        }
      }
      if (req->has_rules_channel && req->rules_channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->rules_channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That rules channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(ch->discord_type) || ccord_store_channel_is_forum(s->store, ch->id)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Rules channel must be a text channel in this server.");
        }
      }
      if (req->has_public_updates_channel && req->public_updates_channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->public_updates_channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That community updates channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(ch->discord_type) || ccord_store_channel_is_forum(s->store, ch->id)) {
          return ccord_error_set(err, CCORD_ERR_INVALID,
                                 "Community updates channel must be a text channel in this server.");
        }
      }
      if ((req->has_community || req->has_rules_channel || req->has_public_updates_channel) &&
          ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_MANAGE_GUILD) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Server permission to update Community settings.");
        }
      }
      break;
    case CCORD_ADMIN_LEAVE_GUILD:
      if (guild_owner != CCORD_ID_NONE && guild_owner == self) {
        return ccord_error_set(err, CCORD_ERR_PERMISSION,
                               "Server owners cannot leave their server. Transfer ownership or delete the server first.");
      }
      break;
    case CCORD_ADMIN_DELETE_GUILD:
      if (guild_owner != CCORD_ID_NONE && guild_owner != self) {
        return ccord_error_set(err, CCORD_ERR_PERMISSION, "Only the server owner can delete the server.");
      }
      break;
    case CCORD_ADMIN_TRANSFER_OWNERSHIP:
      if (req->user_id == CCORD_ID_NONE || req->user_id == self) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Choose another member to transfer ownership to.");
      }
      if (guild_owner != CCORD_ID_NONE && guild_owner != self) {
        return ccord_error_set(err, CCORD_ERR_PERMISSION, "Only the server owner can transfer ownership.");
      }
      break;
    case CCORD_ADMIN_LIST_EMOJIS:
    case CCORD_ADMIN_LIST_STICKERS:
    case CCORD_ADMIN_LIST_SOUNDBOARD:
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
      }
      break;
    case CCORD_ADMIN_CREATE_EMOJI: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
      }
      uint32_t used = 0;
      size_t n = ccord_store_guild_emoji_count(s->store, req->guild_id);
      for (size_t i = 0; i < n; i++) {
        const ccord_emoji *e = ccord_store_guild_emoji_at(s->store, req->guild_id, i);
        if (e && e->animated == req->emoji_animated) used++;
      }
      if (used >= ccord_guild_emoji_slot_limit(guild_premium_tier)) {
        return ccord_error_set(err, CCORD_ERR_RANGE,
                               req->emoji_animated ? "This server has reached its animated emoji slot limit."
                                                   : "This server has reached its static emoji slot limit.");
      }
      break;
    }
    case CCORD_ADMIN_CREATE_STICKER: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
      }
      if (ccord_store_guild_sticker_count(s->store, req->guild_id) >=
          ccord_guild_sticker_slot_limit(guild_premium_tier)) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "This server has reached its sticker slot limit.");
      }
      break;
    }
    case CCORD_ADMIN_CREATE_SOUNDBOARD: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
      }
      if (ccord_store_guild_soundboard_count(s->store, req->guild_id) >=
          ccord_guild_soundboard_slot_limit(guild_premium_tier)) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "This server has reached its soundboard slot limit.");
      }
      break;
    }
    case CCORD_ADMIN_EDIT_EMOJI:
    case CCORD_ADMIN_DELETE_EMOJI: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
        const ccord_emoji *e = ccord_store_find_emoji(s->store, req->expr_id);
        if (e && e->guild_id == req->guild_id && e->user_id != CCORD_ID_NONE &&
            !ccord_perm_can_modify_expression(perms, self, e->user_id)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Expressions permission to modify expressions created by others.");
        }
      }
      break;
    }
    case CCORD_ADMIN_EDIT_STICKER:
    case CCORD_ADMIN_DELETE_STICKER: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
        const ccord_sticker *st_it = ccord_store_find_sticker(s->store, req->expr_id);
        if (st_it && st_it->guild_id == req->guild_id && st_it->user_id != CCORD_ID_NONE &&
            !ccord_perm_can_modify_expression(perms, self, st_it->user_id)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Expressions permission to modify expressions created by others.");
        }
      }
      break;
    }
    case CCORD_ADMIN_EDIT_SOUNDBOARD:
    case CCORD_ADMIN_DELETE_SOUNDBOARD: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
        const ccord_soundboard_sound *sb = ccord_store_find_soundboard(s->store, req->expr_id);
        if (sb && sb->guild_id == req->guild_id && sb->user_id != CCORD_ID_NONE &&
            !ccord_perm_can_modify_expression(perms, self, sb->user_id)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Expressions permission to modify expressions created by others.");
        }
      }
      break;
    }
    case CCORD_ADMIN_PREVIEW_PRUNE:
    case CCORD_ADMIN_EXECUTE_PRUNE:
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_KICK_MEMBERS) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Kick Members permission to prune members.");
        }
        if (g && guild_owner != self && req->prune_role_count > 0) {
          size_t nroles = 0, nactor = 0;
          const ccord_role *roles = ccord_store_roles(s->store, req->guild_id, &nroles);
          const ccord_id *actor_roles = nullptr;
          if (roles && nroles > 0 && ccord_store_member_roles(s->store, req->guild_id, self, &actor_roles, &nactor)) {
            const ccord_role *actor_top = ccord_perm_top_role(req->guild_id, roles, nroles, actor_roles, nactor);
            for (size_t i = 0; i < req->prune_role_count; i++) {
              const ccord_role *ro = ccord_store_role(s->store, req->guild_id, req->prune_role_ids[i]);
              if (!ro || ro->managed || !ccord_perm_outranks(actor_top, ro)) {
                return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                       "You can only prune roles lower than your highest role.");
              }
            }
          }
        }
      }
      break;
    case CCORD_ADMIN_LIST_AUTOMOD_RULES:
    case CCORD_ADMIN_CREATE_AUTOMOD_RULE:
    case CCORD_ADMIN_EDIT_AUTOMOD_RULE:
    case CCORD_ADMIN_DELETE_AUTOMOD_RULE:
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_MANAGE_GUILD) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Server permission to manage AutoMod rules.");
        }
      }
      break;
    case CCORD_ADMIN_LIST_WEBHOOKS:
    case CCORD_ADMIN_CREATE_WEBHOOK:
    case CCORD_ADMIN_EDIT_WEBHOOK:
    case CCORD_ADMIN_DELETE_WEBHOOK:
    case CCORD_ADMIN_FOLLOW_CHANNEL: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_MANAGE_WEBHOOKS) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Webhooks permission to manage integrations.");
        }
      }
      if (req->op == CCORD_ADMIN_CREATE_WEBHOOK) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That webhook channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(ch->discord_type)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Webhooks must post to a text channel in this server.");
        }
      } else if (req->op == CCORD_ADMIN_EDIT_WEBHOOK && req->target_channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->target_channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That webhook channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(ch->discord_type)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Webhooks must post to a text channel in this server.");
        }
      } else if (req->op == CCORD_ADMIN_FOLLOW_CHANNEL) {
        const ccord_channel *src = ccord_store_channel(s->store, req->channel_id);
        if (!src) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That announcement channel isn't available.");
        if (src->discord_type != CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Only announcement channels can be followed.");
        }
        const ccord_channel *dst = ccord_store_channel(s->store, req->target_channel_id);
        if (!dst) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That destination channel isn't available.");
        if (dst->guild_id != req->guild_id || dst->kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(dst->discord_type)) {
          return ccord_error_set(err, CCORD_ERR_INVALID,
                                 "Followed announcements must post to a text channel in this server.");
        }
      }
      break;
    }
    case CCORD_ADMIN_LIST_APP_CMD_PERMS:
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & (CCORD_PERM_MANAGE_GUILD | CCORD_PERM_MANAGE_WEBHOOKS)) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Server or Manage Webhooks permission to view integration permissions.");
        }
      }
      break;
    case CCORD_ADMIN_GET_WELCOME_SCREEN:
    case CCORD_ADMIN_EDIT_WELCOME_SCREEN:
    case CCORD_ADMIN_GET_ONBOARDING:
    case CCORD_ADMIN_PUT_ONBOARDING:
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_MANAGE_GUILD) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Server permission to manage Community settings.");
        }
      }
      break;
    case CCORD_ADMIN_PUT_CHANNEL_OVERWRITE:
    case CCORD_ADMIN_DELETE_CHANNEL_OVERWRITE: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (ch->guild_id != req->guild_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't in this server.");
      }
      if (ccord_discord_type_is_thread(ch->discord_type)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Threads inherit permissions from their parent channel.");
      }
      if (req->op == CCORD_ADMIN_PUT_CHANNEL_OVERWRITE && req->overwrite.kind == CCORD_OVERWRITE_ROLE &&
          ccord_store_roles_known(s->store, req->guild_id) &&
          !ccord_store_role(s->store, req->guild_id, req->overwrite.id)) {
        return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That role isn't available.");
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, req->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_MANAGE_ROLES) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Permissions in this channel to edit overwrites.");
        }
        if (req->op == CCORD_ADMIN_PUT_CHANNEL_OVERWRITE) {
          uint64_t old_allow = 0u, old_deny = 0u;
          size_t nows = 0u;
          const ccord_overwrite *ows = ccord_store_channel_overwrites(s->store, req->channel_id, &nows);
          for (size_t i = 0u; ows && i < nows; i++) {
            if (ows[i].id == req->overwrite.id) {
              old_allow = ows[i].allow;
              old_deny = ows[i].deny;
              break;
            }
          }
          uint64_t changed = (old_allow ^ req->overwrite.allow) | (old_deny ^ req->overwrite.deny);
          if ((changed & ~ch_perms) != 0u) {
            return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                   "You can't grant or deny permissions you don't hold in this channel.");
          }
        }
      }
      break;
    }
    case CCORD_ADMIN_LIST_CHANNEL_INVITES: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (ch->guild_id != req->guild_id || ch->kind == CCORD_CHANNEL_CATEGORY ||
          ccord_discord_type_is_thread(ch->discord_type)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Channel invites are not available for this channel.");
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, req->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Channels permission to view channel invites.");
        }
      }
      break;
    }
    case CCORD_ADMIN_CLONE_CHANNEL: {
      if (req->parent_id != CCORD_ID_NONE && req->channel_type != 4u) {
        const ccord_channel *par = ccord_store_channel(s->store, req->parent_id);
        if (!par || par->guild_id != req->guild_id || par->kind != CCORD_CHANNEL_CATEGORY) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Invalid parent category.");
        }
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Manage Channels permission to clone channels.");
        }
        for (size_t i = 0u; i < req->clone_overwrite_count; i++) {
          uint64_t bits = req->clone_overwrites[i].allow | req->clone_overwrites[i].deny;
          if ((bits & ~perms) != 0u) {
            return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                   "You can't clone permission overwrites you don't hold.");
          }
        }
      }
      break;
    }
    case CCORD_ADMIN_SET_VOICE_MUTE:
    case CCORD_ADMIN_SET_VOICE_DEAF:
    case CCORD_ADMIN_MOVE_VOICE_MEMBER: {
      if (req->user_id == CCORD_ID_NONE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Choose a voice participant.");
      }
      const ccord_voice_state *vs = ccord_store_voice_user(s->store, req->guild_id, req->user_id);
      if (!vs || vs->channel_id == CCORD_ID_NONE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That member is not connected to a voice channel.");
      }
      if (req->op == CCORD_ADMIN_MOVE_VOICE_MEMBER && req->target_channel_id != CCORD_ID_NONE) {
        const ccord_channel *dst = ccord_store_channel(s->store, req->target_channel_id);
        if (!dst) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That voice channel isn't available.");
        if (dst->guild_id != req->guild_id || dst->kind != CCORD_CHANNEL_VOICE) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Choose a voice channel in this server.");
        }
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t vperms = ccord_store_permissions(s->store, req->guild_id, vs->channel_id, self, session_now_unix_ms(s));
        if (req->op == CCORD_ADMIN_SET_VOICE_MUTE && (vperms & CCORD_PERM_MUTE_MEMBERS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Mute Members permission in this voice channel.");
        }
        if (req->op == CCORD_ADMIN_SET_VOICE_DEAF && (vperms & CCORD_PERM_DEAFEN_MEMBERS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Deafen Members permission in this voice channel.");
        }
        if (req->op == CCORD_ADMIN_MOVE_VOICE_MEMBER) {
          if ((vperms & CCORD_PERM_MOVE_MEMBERS) == 0u) {
            return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                   "You need Move Members permission in this voice channel.");
          }
          if (req->target_channel_id != CCORD_ID_NONE) {
            uint64_t dperms = ccord_store_permissions(s->store, req->guild_id, req->target_channel_id, self, session_now_unix_ms(s));
            if ((dperms & CCORD_PERM_CONNECT) == 0u) {
              return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                     "You need Connect permission in the destination voice channel.");
            }
          }
        }
      }
      break;
    }
    case CCORD_ADMIN_VOICE_STATUS: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (ch->guild_id == CCORD_DM_GUILD_ID ||
          (ch->kind != CCORD_CHANNEL_VOICE && ch->kind != CCORD_CHANNEL_STAGE)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Voice status needs a voice or stage channel.");
      }
      if (ccord_store_permissions_exact(s->store, ch->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, ch->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_SET_VOICE_CHANNEL_STATUS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Set Voice Channel Status permission in this channel.");
        }
      }
      break;
    }
    case CCORD_ADMIN_STAGE_REQUEST_SPEAK: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch || ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_STAGE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't a stage in this server.");
      }
      const ccord_voice_state *vs = self_voice(s, req->guild_id);
      if (!vs || vs->channel_id != req->channel_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Join the stage before asking to speak.");
      }
      if (req->request_to_speak_ms != 0 && !vs->suppress) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "You're already a speaker on this stage.");
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, req->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_REQUEST_TO_SPEAK) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Request to Speak permission in this stage.");
        }
      }
      break;
    }
    case CCORD_ADMIN_STAGE_SET_SPEAKER: {
      if (req->user_id == CCORD_ID_NONE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Choose a stage participant.");
      }
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch || ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_STAGE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't a stage in this server.");
      }
      // Discord answers 403 before it looks at the target's state (same order as the server).
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, req->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_MUTE_MEMBERS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Mute Members permission to manage stage speakers.");
        }
      }
      const ccord_voice_state *vs = ccord_store_voice_user(s->store, req->guild_id, req->user_id);
      if (!vs || vs->channel_id != req->channel_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That member is not connected to this stage.");
      }
      break;
    }
    case CCORD_ADMIN_STAGE_TOPIC:
    case CCORD_ADMIN_STAGE_DELETE: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (ch->kind != CCORD_CHANNEL_STAGE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't a stage.");
      }
      if (ccord_store_permissions_exact(s->store, ch->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, ch->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_MUTE_MEMBERS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 req->op == CCORD_ADMIN_STAGE_DELETE
                                   ? "You need Mute Members permission to end the stage."
                                   : "You need Mute Members permission to change the stage topic.");
        }
      }
      break;
    }
    case CCORD_ADMIN_STAGE_CREATE: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_STAGE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't a stage in this server.");
      }
      if (ccord_store_stage_instance(s->store, req->channel_id) != nullptr) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "This stage already has an active stage instance.");
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, req->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_MUTE_MEMBERS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Mute Members permission to start a stage.");
        }
      }
      break;
    }
    case CCORD_ADMIN_ACTIVITY_SHELF:
      break;   // the guild row was validated above; the answer is cached per guild
    case CCORD_ADMIN_ACTIVITY_LAUNCH: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (ch->kind != CCORD_CHANNEL_VOICE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Activities run in voice channels.");
      }
      const ccord_voice_state *vs = self_voice(s, ch->guild_id);
      if (!vs || vs->channel_id != req->channel_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Join the voice channel before starting an activity.");
      }
      if (ccord_store_permissions_exact(s->store, ch->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, ch->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_USE_EMBEDDED_ACTIVITIES) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Use Embedded Activities permission in this channel.");
        }
      }
      break;
    }
    case CCORD_ADMIN_ACTIVITY_LEAVE: {
      // Only a running instance we know can be left (no voice-connection gate: leaving after a disconnect is the
      // point). The REST location segment is the instance's own location, so it is not compared to a channel here.
      const ccord_activity_instance *inst = ccord_store_find_activity_instance(s->store, req->instance_id);
      if (!inst || inst->application_id != req->application_id) {
        return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That activity isn't running.");
      }
      break;
    }
    case CCORD_ADMIN_EVENT_CREATE: {
      if (req->event.entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->event.channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That stage channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_STAGE) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Stage events require a stage channel in this server.");
        }
      } else if (req->event.entity_type == CCORD_SCHEDULED_EVENT_VOICE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->event.channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That voice channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_VOICE) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Voice events require a voice channel in this server.");
        }
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        ccord_id perm_ch = (req->event.entity_type == CCORD_SCHEDULED_EVENT_EXTERNAL)
                             ? CCORD_ID_NONE
                             : req->event.channel_id;
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, perm_ch, self, session_now_unix_ms(s));
        if ((perms & (CCORD_PERM_CREATE_EVENTS | CCORD_PERM_MANAGE_EVENTS)) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Create Events or Manage Events permission.");
        }
      }
      break;
    }
    case CCORD_ADMIN_EVENT_MODIFY:
    case CCORD_ADMIN_EVENT_DELETE: {
      if (req->op == CCORD_ADMIN_EVENT_MODIFY && (req->event_has & CCORD_ADMIN_EVENT_HAS_CHANNEL) != 0u &&
          req->event.channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->event.channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
        if (ch->guild_id != req->guild_id ||
            (ch->kind != CCORD_CHANNEL_VOICE && ch->kind != CCORD_CHANNEL_STAGE)) {
          return ccord_error_set(err, CCORD_ERR_INVALID,
                                 "Scheduled events require a voice or stage channel in this server.");
        }
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        const ccord_scheduled_event *ev = ccord_store_find_scheduled_event(s->store, req->event_id);
        ccord_id perm_ch = (ev && ev->guild_id == req->guild_id) ? ev->channel_id : CCORD_ID_NONE;
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, perm_ch, self, session_now_unix_ms(s));
        if ((perms & (CCORD_PERM_CREATE_EVENTS | CCORD_PERM_MANAGE_EVENTS)) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Create Events or Manage Events permission.");
        }
        if (ev && ev->guild_id == req->guild_id && ev->creator_id != CCORD_ID_NONE && ev->creator_id != self &&
            (perms & CCORD_PERM_MANAGE_EVENTS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Events permission to modify events created by others.");
        }
      }
      break;
    }
    case CCORD_ADMIN_EVENT_LIST:
    case CCORD_ADMIN_EVENT_RSVP_ADD:
    case CCORD_ADMIN_EVENT_RSVP_REMOVE:
      break;
    case CCORD_ADMIN_CREATE_CHANNEL:
    case CCORD_ADMIN_SET_INCIDENT_ACTIONS:
    case CCORD_ADMIN_SET_VANITY_URL:
    case CCORD_ADMIN_UNBAN:
    case CCORD_ADMIN_CREATE_INVITE:
    case CCORD_ADMIN_REVOKE_INVITE:
    case CCORD_ADMIN_LIST_BANS:
    case CCORD_ADMIN_LIST_INVITES:
    case CCORD_ADMIN_LIST_AUDIT_LOG:
    case CCORD_ADMIN_SET_GUILD_FOLDERS:
    case CCORD_ADMIN_CREATE_GUILD:
    case CCORD_ADMIN_PREVIEW_INVITE:
    case CCORD_ADMIN_ACCEPT_INVITE:
    case CCORD_ADMIN_SET_CUSTOM_STATUS:
    case CCORD_ADMIN_GET_MEMBER_VERIFICATION:
    case CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION:
    case CCORD_ADMIN_NONE:
    case CCORD_ADMIN_OP_COUNT: break;
    case CCORD_ADMIN_GET_GUILD_PROFILE:
      if (req->guild_id == CCORD_ID_NONE || req->guild_id == CCORD_DM_GUILD_ID) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid guild id");
      }
      break;
    default: break;
  }
  if (req->op == CCORD_ADMIN_PREVIEW_INVITE) {
    if (req->code[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad invite preview request");
    }
    uint64_t t = now_ms ? now_ms : (ccord_clock_ms() ? ccord_clock_ms() : 1u);
    session_invite_cache *entry = find_invite_cache(s, req->code);
    if (entry) {
      if (entry->fetched_ms != 0 && t >= entry->fetched_ms && t < entry->fetched_ms + SESSION_INVITE_TTL_MS) {
        s->invite_preview = entry->preview;
        s->has_invite_preview = true;
        s->invite_preview_loading = false;
        return CCORD_OK;
      }
      if (entry->sent_ms != 0 && t < entry->sent_ms + SESSION_INVITE_WAIT_MS) return CCORD_OK;
    }
    ccord_status preview_st = ccord_transport_admin_action(s->transport, req, err);
    if (preview_st == CCORD_OK) {
      entry = invite_cache_for(s, req->code);
      if (entry) {
        entry->sent_ms = t;
        entry->failed = false;
      }
      s->has_invite_preview = false;
      s->invite_preview_loading = true;
    }
    return preview_st;
  }

  ccord_status st = ccord_transport_admin_action(s->transport, req, err);
  if (st == CCORD_OK && req->op == CCORD_ADMIN_REVOKE_INVITE) {
    invalidate_invite_cache(s, req->code);
  } else if (st == CCORD_OK && req->op == CCORD_ADMIN_ACCEPT_INVITE) {
    // Expire TTL so an explicit PREVIEW_INVITE refreshes counts, while keeping the preview struct for inline cards.
    session_invite_cache *entry = find_invite_cache(s, req->code);
    if (entry) entry->fetched_ms = 0;
  }
  return st;
}

ccord_status ccord_session_admin(ccord_session *s, const ccord_admin_request *req, ccord_error *err)
{
  return ccord_session_admin_ex(s, req, 0, err);
}

// ---- M6 account settings, active devices, and linked connections --------------------------------

ccord_status ccord_session_user_settings_patch(ccord_session *s, const ccord_user_settings_patch *patch,
                                                ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_user_settings_patch(&req, patch, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);   // the store changes only when the full answer arrives
}

static ccord_status fetch_account_list(ccord_session *s, bool sessions, uint64_t now_ms, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  uint64_t now = now_ms ? now_ms : (ccord_clock_ms() ? ccord_clock_ms() : 1u);
  ccord_fetch_state state = sessions ? s->auth_sessions_state : s->connections_state;
  uint64_t sent = sessions ? s->auth_sessions_sent_ms : s->connections_sent_ms;
  uint64_t fetched = sessions ? s->auth_sessions_fetched_ms : s->connections_fetched_ms;
  if (state == CCORD_FETCH_LOADING && sent != 0 && now < sent + CCORD_SESSION_ACCOUNT_WAIT_MS) {
    return CCORD_OK;   // one identical request is already awaiting its answer
  }
  if (state == CCORD_FETCH_READY && now >= fetched && now < fetched + CCORD_SESSION_ACCOUNT_TTL_MS) {
    return CCORD_OK;   // the list is still fresh
  }
  ccord_admin_request req;
  ccord_status st = sessions ? ccord_admin_auth_sessions_list(&req, err)
                             : ccord_admin_connections_list(&req, err);
  if (st != CCORD_OK) return st;
  if (sessions) {
    s->auth_sessions_state = CCORD_FETCH_LOADING;
    s->auth_sessions_sent_ms = now;
  } else {
    s->connections_state = CCORD_FETCH_LOADING;
    s->connections_sent_ms = now;
  }
  st = ccord_session_admin(s, &req, err);
  if (st != CCORD_OK) {   // the queue refused it: keep the last list and let the next ask retry
    if (sessions) {
      s->auth_sessions_state = CCORD_FETCH_FAILED;
      s->auth_sessions_sent_ms = 0;
    } else {
      s->connections_state = CCORD_FETCH_FAILED;
      s->connections_sent_ms = 0;
    }
  }
  return st;
}

ccord_status ccord_session_fetch_auth_sessions_ex(ccord_session *s, uint64_t now_ms, ccord_error *err)
{
  return fetch_account_list(s, true, now_ms, err);
}

ccord_status ccord_session_fetch_auth_sessions(ccord_session *s, ccord_error *err)
{
  return fetch_account_list(s, true, 0, err);
}

ccord_status ccord_session_fetch_connections_ex(ccord_session *s, uint64_t now_ms, ccord_error *err)
{
  return fetch_account_list(s, false, now_ms, err);
}

ccord_status ccord_session_fetch_connections(ccord_session *s, ccord_error *err)
{
  return fetch_account_list(s, false, 0, err);
}

ccord_status ccord_session_logout_auth_sessions(ccord_session *s, const char hashes[][64], size_t count,
                                                 ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!hashes || count == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "session: no sessions to log out");
  for (size_t off = 0; off < count;) {
    size_t batch = count - off;
    if (batch > CCORD_ADMIN_SESSION_LOGOUT_MAX) batch = CCORD_ADMIN_SESSION_LOGOUT_MAX;
    ccord_admin_request req;
    ccord_status st = ccord_admin_auth_sessions_logout(&req, hashes + off, batch, err);
    if (st != CCORD_OK) return st;
    st = ccord_session_admin(s, &req, err);
    if (st != CCORD_OK) return st;
    off += batch;
  }
  return CCORD_OK;   // the cached list drops each row when its own 204 ack arrives
}

ccord_status ccord_session_edit_connection(ccord_session *s, const char *name, const char *id,
                                            const ccord_connection_edit *edit, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_connection_edit(&req, name, id, edit, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);
}

ccord_status ccord_session_delete_connection(ccord_session *s, const char *name, const char *id, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_connection_delete(&req, name, id, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);
}

const ccord_auth_session *ccord_session_auth_sessions(const ccord_session *s, size_t *count)
{
  if (count) *count = s ? s->auth_session_count : 0u;
  return s ? s->auth_sessions : nullptr;
}

const ccord_connection *ccord_session_connections(const ccord_session *s, size_t *count)
{
  if (count) *count = s ? s->connection_count : 0u;
  return s ? s->connections : nullptr;
}

ccord_fetch_state ccord_session_auth_sessions_state(const ccord_session *s)
{
  return s ? s->auth_sessions_state : CCORD_FETCH_IDLE;
}

ccord_fetch_state ccord_session_connections_state(const ccord_session *s)
{
  return s ? s->connections_state : CCORD_FETCH_IDLE;
}

ccord_status ccord_session_open_thread(ccord_session *s, ccord_id guild_id, ccord_id parent_id,
                                       ccord_id thread_id, const char *name, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (thread_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: thread id");

  if (!ccord_store_channel(s->store, thread_id)) {
    const ccord_channel *parent = parent_id != CCORD_ID_NONE ? ccord_store_channel(s->store, parent_id) : nullptr;
    if (guild_id == CCORD_ID_NONE && parent) guild_id = parent->guild_id;
    if (guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown thread guild");
    ccord_channel synth = {
      .id = thread_id,
      .guild_id = guild_id,
      .parent_id = parent ? parent->id : CCORD_ID_NONE,
      .kind = CCORD_CHANNEL_TEXT,
      .name = (name && name[0] != '\0') ? name : "Thread",
      .topic = "",
      .icon_url = "",
      .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD,
    };
    ccord_status ust = ccord_store_upsert_channel(s->store, &synth, err);
    if (ust != CCORD_OK) return ust;
    ccord_error ignored;
    (void)ccord_transport_request_channel(s->transport, thread_id, &ignored);
  }
  return ccord_session_select_channel(s, thread_id, err);
}

ccord_status ccord_session_request_reaction_users(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                                  const char *emoji, uint32_t limit, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: reaction user ids");
  }
  if (!emoji || emoji[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "session: reaction emoji");
  size_t elen = strlen(emoji);
  if (elen > CCORD_EMOJI_MAX_BYTES || !ccord_utf8_valid_text(emoji, elen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: reaction emoji text");
  }
  if (!ccord_store_channel(s->store, channel_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  }
  if (s->react_users_loading && s->react_users_channel_id == channel_id &&
      s->react_users_message_id == message_id && strcmp(s->react_users_emoji, emoji) == 0) {
    return CCORD_OK;
  }
  clear_reaction_users(s);
  s->react_users_channel_id = channel_id;
  s->react_users_message_id = message_id;
  memcpy(s->react_users_emoji, emoji, elen);
  s->react_users_emoji[elen] = '\0';
  s->react_users_loading = true;
  ccord_status st =
    ccord_transport_request_reaction_users(s->transport, channel_id, message_id, emoji, elen, limit ? limit : 50u, err);
  if (st != CCORD_OK) s->react_users_loading = false;
  return st;
}

const ccord_user *ccord_session_reaction_users(const ccord_session *s, ccord_id *out_channel_id,
                                               ccord_id *out_message_id, const char **out_emoji,
                                               size_t *out_count, bool *out_loading)
{
  if (out_channel_id) *out_channel_id = s ? s->react_users_channel_id : CCORD_ID_NONE;
  if (out_message_id) *out_message_id = s ? s->react_users_message_id : CCORD_ID_NONE;
  if (out_emoji) *out_emoji = s ? s->react_users_emoji : "";
  if (out_count) *out_count = s ? s->react_user_count : 0u;
  if (out_loading) *out_loading = s ? s->react_users_loading : false;
  return s ? s->react_users : nullptr;
}

ccord_status ccord_session_open_dm(ccord_session *s, ccord_id recipient_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (recipient_id == CCORD_ID_NONE || recipient_id == ccord_store_self_id(s->store)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid DM recipient");
  }
  ccord_status st = ccord_transport_open_dm(s->transport, recipient_id, err);
  if (st == CCORD_OK) {
    ccord_error ignored;
    (void)ccord_session_request_profile(s, recipient_id, &ignored);
  }
  return st;
}

ccord_status ccord_session_request_profile_ex(ccord_session *s, ccord_id user_id, ccord_id guild_id,
                                              uint64_t now_ms, bool force, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid user id");
  if (guild_id == CCORD_DM_GUILD_ID) guild_id = CCORD_ID_NONE;
  uint64_t t = now_ms ? now_ms : (ccord_clock_ms() ? ccord_clock_ms() : 1u);

  session_profile_cache *pe = profile_cache_for(s, user_id);
  if (!force && pe && pe->guild_id == guild_id) {
    if (pe->in_flight && t < pe->sent_ms + 15000u) return CCORD_OK;
    if (pe->loaded && ccord_store_user_profile(s->store, user_id) != nullptr &&
        t >= pe->fetched_ms && t < pe->fetched_ms + CCORD_PROFILE_TTL_MS) {
      return CCORD_OK;
    }
  }
  ccord_status st = ccord_transport_request_profile_ex(s->transport, user_id, guild_id, err);
  if (st == CCORD_OK && pe) {
    pe->guild_id = guild_id;
    pe->sent_ms = t;
    pe->in_flight = true;
  }
  return st;
}

ccord_status ccord_session_request_profile(ccord_session *s, ccord_id user_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid user id");
  if (ccord_store_user_profile(s->store, user_id) != nullptr || was_profile_requested(s, user_id)) {
    return CCORD_OK;
  }
  return ccord_session_request_profile_ex(s, user_id, CCORD_ID_NONE, ccord_clock_ms(), false, err);
}

ccord_status ccord_session_set_relationship(ccord_session *s, ccord_id user_id,
                                            ccord_relationship_kind kind, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (user_id == CCORD_ID_NONE || user_id == ccord_store_self_id(s->store)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid relationship target");
  }
  if (kind > CCORD_RELATIONSHIP_PENDING_OUTGOING) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid relationship kind");
  }
  return ccord_transport_set_relationship(s->transport, user_id, kind, err);
}

bool ccord_session_pop_notification(ccord_session *s, ccord_notification *out)
{
  if (!s || !out || s->notif_count == 0) return false;
  *out = s->notifications[s->notif_head];
  s->notif_head = (s->notif_head + 1u) % SESSION_MAX_NOTIFICATIONS;
  s->notif_count--;
  return true;
}

ccord_status ccord_session_push_voice_pcm(ccord_session *s, const int16_t *pcm,
                                          size_t samples, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_mutex_lock(s->audio_mtx);
  if (!s->transport) {
    ccord_mutex_unlock(s->audio_mtx);
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: transport is null");
  }
  // Push-to-talk gate: while PTT mode is on and the key is not held, mic frames are dropped before
  // they reach the transport (the live voice gateway applies the same gate; no traffic either way).
  if (atomic_load(&s->has_voice_cfg) && s->voice_cfg.ptt && !atomic_load(&s->ptt_held)) {
    ccord_mutex_unlock(s->audio_mtx);
    return CCORD_OK;
  }
  ccord_status st = ccord_transport_push_voice_pcm(s->transport, pcm, samples, err);
  ccord_mutex_unlock(s->audio_mtx);
  return st;
}

size_t ccord_session_pull_voice_pcm(ccord_session *s, int16_t *pcm, size_t max_samples)
{
  if (!s || !pcm || max_samples == 0) return 0;
  ccord_mutex_lock(s->audio_mtx);
  if (!s->transport) {
    ccord_mutex_unlock(s->audio_mtx);
    return 0;
  }
  size_t n = ccord_transport_pull_voice_pcm(s->transport, pcm, max_samples);
  ccord_mutex_unlock(s->audio_mtx);
  return n;
}

ccord_status ccord_session_request_expression_data(ccord_session *s, ccord_expr_kind kind, const char *arg, bool force,
                                                   ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (kind >= CCORD_EXPR_KIND_COUNT) return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad expression kind");
  if (!arg) arg = "";
  ccord_expr_blob_state st = ccord_store_expr_blob_state(s->store, kind, arg);
  if (st == CCORD_EXPR_BLOB_PENDING || (st == CCORD_EXPR_BLOB_READY && !force)) return CCORD_OK;
  if (s->stale) {   // background load: remember it, READY replays it
    size_t alen = strlen(arg);
    bool known = false;
    for (size_t i = 0; i < s->nstale_expr; i++) {
      if (s->stale_expr[i].kind == kind && strcmp(s->stale_expr[i].arg, arg) == 0) known = true;
    }
    if (!known && alen < sizeof s->stale_expr[0].arg && s->nstale_expr < 8) {
      s->stale_expr[s->nstale_expr].kind = kind;
      memcpy(s->stale_expr[s->nstale_expr].arg, arg, alen + 1u);
      s->nstale_expr++;
    }
    return CCORD_OK;
  }
  ccord_status rc = ccord_transport_request_expression_data(s->transport, kind, arg, strlen(arg), err);
  ccord_error ignored;
  if (rc == CCORD_OK) {
    ccord_store_expr_blob_mark_pending(s->store, kind, arg);
  } else {
    (void)ccord_store_set_expr_blob(s->store, kind, arg, nullptr, 0, false, &ignored);   // surface as FAILED, no retry storm
  }
  return rc;
}

ccord_status ccord_session_send_sticker(ccord_session *s, ccord_id channel_id, ccord_id sticker_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (sticker_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: sticker id");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  return ccord_transport_send_sticker(s->transport, channel_id, sticker_id, err);
}

ccord_status ccord_session_toggle_favorite_gif(ccord_session *s, const ccord_gif_item *gif, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !gif) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (gif->url[0] == '\0' || !memchr(gif->url, '\0', sizeof gif->url) || !memchr(gif->src, '\0', sizeof gif->src)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid favorite GIF URL");
  }
  if (!ccord_utf8_valid_text(gif->url, strlen(gif->url)) ||
      (gif->src[0] != '\0' && !ccord_utf8_valid_text(gif->src, strlen(gif->src)))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid favorite GIF URL");
  }
  ccord_gif_item prev_item = *gif;
  bool was_fav = false;
  for (size_t i = 0, n = ccord_store_favorite_gif_count(s->store); i < n; i++) {
    const ccord_gif_item *it = ccord_store_favorite_gif_at(s->store, i);
    if (it && strcmp(it->url, gif->url) == 0) {
      prev_item = *it;
      was_fav = true;
      break;
    }
  }
  bool now_fav = ccord_store_toggle_favorite_gif(s->store, gif);
  if (was_fav == now_fav) {
    return ccord_error_set(err, ccord_store_favorite_gif_count(s->store) > 0u ? CCORD_ERR_RANGE : CCORD_ERR_NOMEM,
                           "Favorite GIF limit reached.");
  }
  ccord_status st = session_patch_favorite_gifs(s, err);
  if (st != CCORD_OK) {
    (void)ccord_store_toggle_favorite_gif(s->store, &prev_item);
  } else {
    fav_pending_note(s, gif->url, now_fav);
  }
  return st;
}

ccord_status ccord_session_request_forum_posts(ccord_session *s, const ccord_forum_query *query, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !query) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (query->forum_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: forum id");
  if (!ccord_store_channel_is_forum(s->store, query->forum_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: unknown forum channel");
  }
  if (query->name && query->name[0] != '\0') {
    size_t nlen = strlen(query->name);
    if (!ccord_utf8_valid_text(query->name, nlen)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid forum search query");
    }
  }
  const ccord_forum_state *prev = ccord_store_forum_state(s->store, query->forum_id);
  if (prev && prev->loading) return CCORD_OK;

  ccord_status st = ccord_transport_request_forum_posts(s->transport, query, err);
  if (st == CCORD_OK) {
    ccord_forum_state fst = prev ? *prev : (ccord_forum_state){0};
    fst.loading = true;
    fst.error[0] = '\0';
    ccord_error ignored;
    (void)ccord_store_forum_set_state(s->store, query->forum_id, &fst, &ignored);
  }
  return st;
}

ccord_status ccord_session_create_forum_post(ccord_session *s, ccord_id forum_id,
                                             const char *title, size_t title_len,
                                             const char *content, size_t content_len,
                                             const ccord_id *tag_ids, size_t tag_count,
                                             ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (forum_id == CCORD_ID_NONE || !ccord_store_channel_is_forum(s->store, forum_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: unknown forum channel");
  }
  if (!title || title_len == 0 || title_len > CCORD_FORUM_TITLE_BYTES || blank(title, title_len) ||
      memchr(title, 0, title_len) || !ccord_utf8_valid_text(title, title_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid post title");
  }
  ccord_status tst = validate_text(content, content_len, err);
  if (tst != CCORD_OK) return tst;
  if (tag_count > CCORD_FORUM_MAX_APPLIED_TAGS || (tag_count > 0 && !tag_ids)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid post tags");
  }
  const ccord_forum_meta *meta = ccord_store_forum_meta(s->store, forum_id);
  if (meta && meta->require_tag && tag_count == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: at least one tag is required");
  }
  const ccord_forum_state *prev = ccord_store_forum_state(s->store, forum_id);
  if (prev && prev->creating) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: post creation already in flight");
  }
  ccord_status st = ccord_transport_create_forum_post(s->transport, forum_id, title, title_len,
                                                      content, content_len, tag_ids, tag_count, err);
  if (st == CCORD_OK) {
    ccord_forum_state fst = prev ? *prev : (ccord_forum_state){0};
    fst.creating = true;
    fst.error[0] = '\0';
    ccord_error ignored;
    (void)ccord_store_forum_set_state(s->store, forum_id, &fst, &ignored);
  }
  return st;
}

ccord_status ccord_session_open_forum_post(ccord_session *s, ccord_id thread_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (thread_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid thread id");
  ccord_status st = ccord_store_forum_open_post(s->store, thread_id, err);
  if (st == CCORD_OK) {
    ccord_id gid = ccord_store_selected_guild(s->store);
    if (gid != CCORD_ID_NONE && !s->stale && CCORD_TRANSPORT_OPTIONAL(s->transport, select_channel)) {
      ccord_error hook_err;
      (void)s->transport->vt->select_channel(s->transport, gid, thread_id, &hook_err);
    }
    s->retried_channel = CCORD_ID_NONE;
    request_history_once(s, thread_id);
  }
  return st;
}

ccord_status ccord_session_vote_poll(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                     const uint32_t *answer_ids, size_t answer_count, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid poll ids");
  }
  if (answer_count > CCORD_RICH_MAX_POLL_ANSWERS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "session: too many poll answers");
  }
  if (answer_count > 0 && !answer_ids) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: null poll answers");
  }
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: unknown poll message");
  if (!m->rich || !m->rich->poll.present) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: message has no poll");
  }
  if (m->rich->poll.finalized) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: poll is finalized");
  }
  if (!m->rich->poll.multiselect && answer_count > 1) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: poll allows one answer");
  }
  return ccord_transport_vote_poll(s->transport, channel_id, message_id, answer_ids, answer_count, err);
}

ccord_status ccord_session_send_interaction(ccord_session *s, const ccord_interaction_request *req,
                                            ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!req || req->channel_id == CCORD_ID_NONE || req->application_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid interaction request");
  }
  if (req->type == CCORD_INTERACTION_APPLICATION_COMMAND ||
      req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE) {
    if (req->command_id == CCORD_ID_NONE || !req->command_name || req->command_name[0] == '\0' ||
        req->command_type > 3u || req->option_count > CCORD_SLASH_MAX_OPTIONS) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid slash command interaction");
    }
    if (req->command_type >= 2u) {
      if (req->type != CCORD_INTERACTION_APPLICATION_COMMAND || req->target_id == CCORD_ID_NONE ||
          session_is_local_echo_id(req->target_id) || req->option_count > 0 ||
          (req->subcommand && req->subcommand[0] != '\0') ||
          (req->subcommand_group && req->subcommand_group[0] != '\0')) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid context menu command interaction");
      }
    } else if (req->subcommand_group && req->subcommand_group[0] != '\0' &&
               (!req->subcommand || req->subcommand[0] == '\0')) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: subcommand_group requires subcommand");
    }
    if (req->option_count > 0 && (!req->option_names || !req->option_kinds || !req->option_values)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: null slash command options");
    }
    if (req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE &&
        (req->focused_option < 0 || (size_t)req->focused_option >= req->option_count)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid focused autocomplete option");
    }
  } else if (req->type == CCORD_INTERACTION_MESSAGE_COMPONENT ||
             req->type == CCORD_INTERACTION_MODAL_SUBMIT) {
    if (!req->custom_id || req->custom_id[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid interaction custom_id");
    }
  } else {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: unsupported interaction type");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: unknown channel");
  ccord_interaction_request filled = *req;
  if (ch->guild_id == CCORD_DM_GUILD_ID || filled.guild_id == CCORD_DM_GUILD_ID) {
    filled.guild_id = CCORD_ID_NONE;
  } else if (filled.guild_id == CCORD_ID_NONE) {
    filled.guild_id = ch->guild_id;
  }
  ccord_status st = ccord_transport_send_interaction(s->transport, &filled, err);
  if (st == CCORD_OK && filled.type == CCORD_INTERACTION_APPLICATION_COMMAND) {
    s->slash_choice_count = 0;
  }
  return st;
}

bool ccord_session_pop_interaction_modal(ccord_session *s, ccord_interaction_modal *out)
{
  if (!s || !s->modals || s->modal_count == 0) return false;
  if (out) *out = s->modals[s->modal_head];
  s->modal_head = (s->modal_head + 1u) % SESSION_MAX_MODALS;
  s->modal_count--;
  return true;
}

bool ccord_session_ensure_slash_commands(ccord_session *s, ccord_id guild_id, ccord_id channel_id, uint64_t now_ms)
{
  if (!s || channel_id == CCORD_ID_NONE) return false;
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch || ch->kind != CCORD_CHANNEL_TEXT) return false;
  ccord_id gid = (guild_id != CCORD_ID_NONE) ? guild_id : ch->guild_id;
  if (gid == CCORD_ID_NONE) return false;
  if (s->stale) {   // background load: READY replays the latest one
    s->stale_slash_guild = gid;
    s->stale_slash_channel = channel_id;
    return false;
  }
  session_slash_cache *ent = slash_cache_for(s, gid);
  if (!ent) return false;
  uint64_t t = now_ms ? now_ms : 1u;
  if (ent->loaded && t < ent->fetched_ms + SESSION_SLASH_TTL_MS) return false;
  if (ent->in_flight && t < ent->sent_ms + SESSION_SLASH_WAIT_MS) return false;
  if (ent->sent_ms != 0 && t < ent->sent_ms + SESSION_SLASH_RETRY_MS) return false;
  ent->sent_ms = t;
  ccord_error err;
  if (ccord_transport_request_slash_commands(s->transport, gid, channel_id, &err) != CCORD_OK) {
    ent->in_flight = false;
    return false;
  }
  ent->in_flight = true;
  return true;
}

const ccord_slash_choice *ccord_session_slash_choices(const ccord_session *s, size_t *out_count, uint64_t *out_seq)
{
  if (out_count) *out_count = s ? s->slash_choice_count : 0;
  if (out_seq) *out_seq = s ? s->slash_choice_seq : 0;
  return (s && s->slash_choice_count > 0) ? s->slash_choices : nullptr;
}

void ccord_session_clear_slash_choices(ccord_session *s)
{
  if (!s) return;
  s->slash_choice_count = 0;
  s->slash_choice_seq++;
}

const ccord_invite_preview *ccord_session_invite_preview(const ccord_session *s, bool *out_loading)
{
  if (out_loading) *out_loading = s ? s->invite_preview_loading : false;
  return (s && s->has_invite_preview) ? &s->invite_preview : nullptr;
}

void ccord_session_clear_invite_preview(ccord_session *s)
{
  if (!s) return;
  s->has_invite_preview = false;
  s->invite_preview_loading = false;
}

ccord_status ccord_session_invite_fetch(ccord_session *s, const char *code_or_url, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s || !code_or_url || !code_or_url[0]) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad invite fetch args");
  }
  char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
  ccord_status st = ccord_admin_parse_invite_code(code_or_url, code, err);
  if (st != CCORD_OK) return st;
  uint64_t t = ccord_clock_ms();
  if (t == 0) t = 1u;
  session_invite_cache *entry = find_invite_cache(s, code);
  if (entry) {
    if (entry->fetched_ms != 0 && t >= entry->fetched_ms && t < entry->fetched_ms + SESSION_INVITE_TTL_MS) {
      return CCORD_OK;
    }
    if (entry->failed || (entry->sent_ms != 0 && t < entry->sent_ms + SESSION_INVITE_WAIT_MS)) {
      return CCORD_OK;
    }
  }
  ccord_admin_request req;
  if ((st = ccord_admin_preview_invite(&req, code, err)) != CCORD_OK) return st;
  st = ccord_transport_admin_action(s->transport, &req, err);
  if (st == CCORD_OK) {
    entry = invite_cache_for(s, code);
    if (entry) {
      entry->sent_ms = t;
      entry->failed = false;
    }
  }
  return st;
}

const ccord_invite_preview *ccord_session_invite_lookup(const ccord_session *s, const char *code_or_url,
                                                        bool *out_loading, bool *out_failed)
{
  if (out_loading) *out_loading = false;
  if (out_failed) *out_failed = false;
  if (!s || !s->invite_cache || !code_or_url || !code_or_url[0]) return nullptr;
  char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
  ccord_error err;
  if (ccord_admin_parse_invite_code(code_or_url, code, &err) != CCORD_OK) return nullptr;
  for (size_t i = 0; i < SESSION_INVITE_CACHE_SLOTS; i++) {
    const session_invite_cache *e = &s->invite_cache[i];
    if (!e->valid || strcmp(e->code, code) != 0) continue;
    if (e->preview.guild_id != CCORD_ID_NONE) return &e->preview;
    if (out_loading) *out_loading = e->sent_ms != 0;
    if (out_failed) *out_failed = e->failed;
    return nullptr;
  }
  return nullptr;
}

ccord_status ccord_session_request_discovery(ccord_session *s, const char *query, uint32_t category_id,
                                             uint32_t offset, uint64_t now_ms, bool force, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const char *q = query ? query : "";
  size_t qlen = strlen(q);
  if (qlen > 100u || memchr(q, 0, qlen) || !ccord_utf8_valid_text(q, qlen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid discovery query");
  }
  uint64_t t = now_ms ? now_ms : (ccord_clock_ms() ? ccord_clock_ms() : 1u);

  if (!force) {
    if (s->discovery_cache) {
      for (size_t i = 0; i < SESSION_DISCOVERY_CACHE_SLOTS; i++) {
        const session_discovery_cache_entry *ce = &s->discovery_cache[i];
        if (!ce->valid) continue;
        if (ce->category_id != category_id || ce->offset != offset || strcmp(ce->query, q) != 0) continue;
        if (t >= ce->fetched_ms && t < ce->fetched_ms + CCORD_DISCOVERY_TTL_MS) {
          if (ce->count > 0 && !s->discovery_items) {
            s->discovery_items = ccord_calloc(CCORD_DISCOVERY_PAGE_MAX, sizeof *s->discovery_items);
            if (!s->discovery_items) break;
          }
          s->discovery_count = ce->count;
          s->discovery_total = ce->total;
          s->discovery_offset = ce->offset;
          s->discovery_category_id = ce->category_id;
          memcpy(s->discovery_query, ce->query, strlen(ce->query) + 1u);
          if (ce->count > 0) memcpy(s->discovery_items, ce->items, ce->count * sizeof s->discovery_items[0]);
          s->discovery_loading = false;
          return CCORD_OK;
        }
      }
    }
    if (s->discovery_loading && s->discovery_category_id == category_id && s->discovery_offset == offset &&
        strcmp(s->discovery_query, q) == 0 && t < s->discovery_sent_ms + 15000u) {
      return CCORD_OK;
    }
  }

  s->discovery_category_id = category_id;
  s->discovery_offset = offset;
  memcpy(s->discovery_query, q, qlen + 1u);
  s->discovery_sent_ms = t;
  s->discovery_loading = true;
  ccord_status st = ccord_transport_request_discoverable_guilds(s->transport, q, category_id, offset,
                                                                CCORD_DISCOVERY_PAGE_MAX, err);
  if (st != CCORD_OK) s->discovery_loading = false;
  return st;
}

const ccord_discover_guild *ccord_session_discovery_results(const ccord_session *s, size_t *out_count,
                                                            uint32_t *out_total, uint32_t *out_offset,
                                                            bool *out_loading)
{
  if (out_count) *out_count = (s && s->discovery_items) ? s->discovery_count : 0u;
  if (out_total) *out_total = s ? s->discovery_total : 0u;
  if (out_offset) *out_offset = s ? s->discovery_offset : 0u;
  if (out_loading) *out_loading = s ? s->discovery_loading : false;
  return (s && s->discovery_items && s->discovery_count > 0) ? s->discovery_items : nullptr;
}

ccord_status ccord_session_set_custom_status(ccord_session *s, const char *emoji_name, ccord_id emoji_id,
                                             const char *text, int64_t expires_unix_ms, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const char *ename = emoji_name ? emoji_name : "";
  const char *txt = text ? text : "";
  size_t elen = strlen(ename);
  size_t tlen = strlen(txt);
  if (elen > CCORD_EMOJI_MAX_BYTES || memchr(ename, 0, elen) || !ccord_utf8_valid_text(ename, elen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid custom status emoji");
  }
  if (tlen > 128u || memchr(txt, 0, tlen) || !ccord_utf8_valid_text(txt, tlen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid custom status text");
  }
  bool clear = (elen == 0 && emoji_id == CCORD_ID_NONE && tlen == 0);
  if (!clear && expires_unix_ms < 0) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "session: invalid custom status expiry");
  }
  ccord_admin_request req;
  ccord_status bst = ccord_admin_set_custom_status(&req, ename, emoji_id, txt, clear ? 0 : expires_unix_ms, err);
  if (bst != CCORD_OK) return bst;
  ccord_status st = ccord_session_admin(s, &req, err);
  if (st != CCORD_OK) return st;
  // The REST write persists it; op 3 makes it show on this connection at once. A gateway that refuses the frame is
  // the caller's problem too (the status would otherwise look set while nobody sees it).
  ccord_presence cur = session_current_presence(s);
  st = ccord_transport_set_presence_ex(s->transport, cur, clear ? nullptr : (elen ? ename : nullptr),
                                       clear ? CCORD_ID_NONE : emoji_id, clear ? nullptr : (tlen ? txt : nullptr), err);
  // The settings proto is what other clients follow: write the new custom status (or its removal) there too. The
  // store holds a custom emoji as "name:id".
  char stored[CCORD_EMOJI_MAX_BYTES + 32u];
  if (!clear && emoji_id != CCORD_ID_NONE) snprintf(stored, sizeof stored, "%s:%llu", ename, (unsigned long long)emoji_id);
  else snprintf(stored, sizeof stored, "%s", clear ? "" : ename);
  session_write_status_proto(s, cur, stored, clear ? "" : txt, clear ? 0 : expires_unix_ms);
  return st;
}

// ---- own profile edits (PATCH /users/@me, PATCH /users/@me/profile) ----

ccord_status ccord_session_set_profile_rest(ccord_session *s, ccord_profile_rest *rest)
{
  if (!s) return CCORD_ERR_INVALID;
  if (s->profile_rest == rest) return CCORD_OK;
  ccord_profile_rest_destroy(s->profile_rest);
  s->profile_rest = rest;
  return CCORD_OK;
}

typedef struct session_profile_ctx {
  ccord_session *session;
  ccord_profile_edit_cb cb;
  void *ud;
} session_profile_ctx;

// Discord accepted (part of) the edit: the store takes the values it answered with, nothing else.
static void session_apply_profile(ccord_session *s, const ccord_profile_applied *a)
{
  ccord_id self = ccord_store_self_id(s->store);
  if (self == CCORD_ID_NONE || a->fields == 0) return;
  ccord_self_profile_update u = {0};
  char avatar[256] = "", banner[256] = "";
  if (a->fields & (1u << CCORD_PROFILE_FIELD_DISPLAY_NAME)) {
    u.set_display_name = true;
    u.display_name = a->display_name;
  }
  if (a->fields & (1u << CCORD_PROFILE_FIELD_AVATAR)) {
    (void)ccord_profile_cdn_url("avatars", self, a->avatar_hash, avatar, sizeof avatar);
    u.set_avatar = true;
    u.avatar_url = avatar;
  }
  if (a->fields & (1u << CCORD_PROFILE_FIELD_BANNER)) {
    (void)ccord_profile_cdn_url("banners", self, a->banner_hash, banner, sizeof banner);
    u.set_banner = true;
    u.banner_url = banner;
  }
  if (a->fields & (1u << CCORD_PROFILE_FIELD_BIO)) {
    u.set_bio = true;
    u.bio = a->bio;
  }
  if (a->fields & (1u << CCORD_PROFILE_FIELD_PRONOUNS)) {
    u.set_pronouns = true;
    u.pronouns = a->pronouns;
  }
  if (a->fields & (1u << CCORD_PROFILE_FIELD_ACCENT)) {
    u.set_accent = true;
    u.accent_color = a->accent_color >= 0 ? (uint32_t)a->accent_color : 0u;
  }
  ccord_error err;
  if (ccord_store_set_self_profile(s->store, &u, &err) != CCORD_OK) {
    CCORD_LOG_W("session: own profile not applied: %s", err.message);
  }
}

static void session_profile_done(void *ud, const ccord_profile_edit_result *res)
{
  session_profile_ctx *ctx = ud;
  if (res->applied.fields != 0) session_apply_profile(ctx->session, &res->applied);
  if (ctx->cb) ctx->cb(ctx->ud, res);
  ccord_free(ctx);
}

ccord_status ccord_session_edit_profile(ccord_session *s, const ccord_profile_edit *edit, ccord_profile_edit_cb cb,
                                        void *ud, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !edit) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (!s->profile_rest) return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "session: profile editing is not available");
  ccord_id self = ccord_store_self_id(s->store);
  if (self == CCORD_ID_NONE || !ccord_store_user(s->store, self)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: not signed in yet");
  }
  session_profile_ctx *ctx = ccord_calloc(1, sizeof *ctx);
  if (!ctx) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
  ctx->session = s;
  ctx->cb = cb;
  ctx->ud = ud;
  ccord_status st = ccord_profile_edit_start(s->profile_rest, edit, session_profile_done, ctx, err);
  if (st != CCORD_OK) ccord_free(ctx);   // nothing was sent and the callback will not run
  return st;
}

// ---- M5 voice & video: DM calls, soundboard, local audio prefs, Go Live frames, stage/DAVE getters ----

ccord_status ccord_session_call_join(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid call channel");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch || ch->guild_id != CCORD_DM_GUILD_ID || ch->kind != CCORD_CHANNEL_TEXT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: calls only happen in DM channels");
  }
  ccord_id self = ccord_store_self_id(s->store);
  const ccord_voice_state *v = ccord_store_voice_user(s->store, CCORD_DM_GUILD_ID, self);
  if (v && v->channel_id == channel_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: already in this call");
  }
  // There is one voice state per DM pseudo-guild: joining a second call would silently abandon the
  // first (its row would keep ringing with nobody to answer it). Leave the current call first.
  if (v && v->channel_id != CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: leave the current call first");
  }
  // Ringing is passive (CALL_CREATE); answering only sends OP 4. The store changes on the events.
  ccord_status st = ccord_transport_call_action(s->transport, channel_id, CCORD_CALL_JOIN, err);
  if (st == CCORD_OK) {
    s->pending_voice_guild = CCORD_DM_GUILD_ID;
    s->pending_voice_channel = channel_id;
  }
  return st;
}

ccord_status ccord_session_call_leave(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid call channel");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch || ch->guild_id != CCORD_DM_GUILD_ID || ch->kind != CCORD_CHANNEL_TEXT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: calls only happen in DM channels");
  }
  ccord_id self = ccord_store_self_id(s->store);
  const ccord_voice_state *v = ccord_store_voice_user(s->store, CCORD_DM_GUILD_ID, self);
  bool in_call = v && v->channel_id == channel_id;
  bool ringing = false;
  const ccord_call_state *call = ccord_store_call(s->store, channel_id);
  for (size_t i = 0; call && i < call->ringing_count; i++) {
    if (call->ringing[i] == self) {
      ringing = true;
      break;
    }
  }
  if (!in_call && !ringing) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: not in this call");
  // Decline while ringing, or hang up while connected; the server decides whether the call ends.
  return ccord_transport_call_action(s->transport, channel_id, CCORD_CALL_LEAVE, err);
}

ccord_status ccord_session_play_soundboard(ccord_session *s, ccord_id guild_id, ccord_id sound_id,
                                           ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (guild_id == CCORD_ID_NONE || sound_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid soundboard sound");
  }
  if (!ccord_store_guild(s->store, guild_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown guild");
  const ccord_voice_state *v = self_voice(s, guild_id);
  if (!v || v->channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: join voice before playing sounds");
  }
  // Sound existence, availability and USE_SOUNDBOARD are the server's answer: the soundboard list is
  // lazy, so the store may not hold it. Playback itself is the governed media path, not a store write.
  return ccord_transport_play_soundboard(s->transport, guild_id, sound_id, err);
}

ccord_status ccord_session_set_user_volume(ccord_session *s, ccord_id user_id, uint8_t pct, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid user");
  if (pct > 200u) return ccord_error_set(err, CCORD_ERR_RANGE, "session: volume must be 0..200");
  ccord_status st = ccord_transport_set_user_volume(s->transport, user_id, pct, err);
  if (st == CCORD_OK) {
    // Local playback preference: it mirrors the transport's gain table and is never Discord state.
    ccord_error pref_err;
    (void)ccord_store_set_user_volume(s->store, user_id, pct, &pref_err);
  }
  return st;
}

ccord_status ccord_session_set_voice_config(ccord_session *s, const ccord_voice_config *cfg, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!cfg) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null voice config");
  if (cfg->vad_sensitivity_pct > 100u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "session: sensitivity must be 0..100");
  }
  if (s->stale) {   // no transport call before READY; the latest config is applied then
    s->stale_voice_cfg = *cfg;
    s->stale_has_voice_cfg = true;
    return CCORD_OK;
  }
  ccord_status st = ccord_transport_set_voice_config(s->transport, cfg, err);
  if (st == CCORD_OK) {
    // Remembered for the local push-to-talk gate on push_voice_pcm; no Discord traffic.
    ccord_mutex_lock(s->audio_mtx);
    s->voice_cfg = *cfg;
    atomic_store_explicit(&s->has_voice_cfg, true, memory_order_release);
    ccord_mutex_unlock(s->audio_mtx);
  }
  return st;
}

ccord_status ccord_session_set_ptt_held(ccord_session *s, bool held, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  s->ptt_held = held;   // per-frame key state from the app; gates the mic path, never sends anything
  return CCORD_OK;
}

bool ccord_session_ptt_held(const ccord_session *s) { return s && s->ptt_held; }

ccord_status ccord_session_stream_send_frame(ccord_session *s, const uint8_t *annexb, size_t len,
                                             uint32_t ts90k, bool keyframe, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty stream frame");
  if (!ccord_store_stream_active(s->store)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: start a stream before sending frames");
  }
  return ccord_transport_send_stream_frame(s->transport, annexb, len, ts90k, keyframe, err);
}

bool ccord_session_stream_poll_frame(ccord_session *s, const uint8_t **annexb, size_t *len, bool *keyframe)
{
  if (!s || !annexb || !len) return false;
  *annexb = nullptr;
  *len = 0;
  if (!s->watched.active) return false;
  if (!s->stream_frame_buf) {
    s->stream_frame_buf = ccord_alloc_array(SESSION_STREAM_FRAME_MAX, sizeof *s->stream_frame_buf);
    if (!s->stream_frame_buf) return false;
  }
  size_t out_len = 0;
  bool out_key = false;
  bool got = ccord_transport_poll_stream_frame(s->transport, s->stream_frame_buf, SESSION_STREAM_FRAME_MAX,
                                               &out_len, nullptr, &out_key);
  if (!got) return false;
  *annexb = s->stream_frame_buf;
  *len = out_len;
  if (keyframe) *keyframe = out_key;
  return true;
}

size_t ccord_session_camera_senders(ccord_session *s, ccord_camera_rx *out, size_t max)
{
  if (!s || !out || max == 0 || !find_active_self_voice(s)) return 0;
  return ccord_transport_camera_senders(s->transport, out, max);
}

bool ccord_session_camera_poll_frame(ccord_session *s, ccord_id user_id, const uint8_t **annexb, size_t *len,
                                     bool *keyframe)
{
  if (!s || !annexb || !len) return false;
  *annexb = nullptr;
  *len = 0;
  if (user_id == CCORD_ID_NONE || !find_active_self_voice(s)) return false;
  if (!s->cam_frame_buf) {
    s->cam_frame_buf = ccord_alloc_array(CCORD_CAM_FRAME_MAX, sizeof *s->cam_frame_buf);
    if (!s->cam_frame_buf) return false;
  }
  size_t out_len = 0;
  bool out_key = false;
  if (!ccord_transport_poll_camera_frame(s->transport, user_id, s->cam_frame_buf, CCORD_CAM_FRAME_MAX, &out_len,
                                         nullptr, &out_key)) {
    return false;
  }
  *annexb = s->cam_frame_buf;
  *len = out_len;
  if (keyframe) *keyframe = out_key;
  return true;
}

void ccord_session_camera_texture_key(ccord_id user_id, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  (void)snprintf(out, cap, "__cam__:%llu", (unsigned long long)user_id);
}

ccord_status ccord_session_voice_send_frame(ccord_session *s, const uint8_t *annexb, size_t len,
                                            uint32_t ts90k, bool keyframe, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty voice frame");
  const ccord_voice_state *v = find_active_self_voice(s);
  if (!v) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: not connected to voice");
  if (!v->self_video) return ccord_error_set(err, CCORD_ERR_INVALID, "session: voice camera not enabled");
  return ccord_transport_send_voice_video(s->transport, annexb, len, ts90k, keyframe, err);
}

bool ccord_session_consume_keyframe_request(ccord_session *s)
{
  if (!s) return false;
  return ccord_transport_consume_keyframe_request(s->transport);
}

ccord_status ccord_session_stream_send_pcm(ccord_session *s, const int16_t *pcm, size_t samples, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_mutex_lock(s->audio_mtx);
  if (!s->transport) {
    ccord_mutex_unlock(s->audio_mtx);
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: transport is null");
  }
  if (!pcm || samples == 0) {
    ccord_mutex_unlock(s->audio_mtx);
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty stream pcm");
  }
  if (!ccord_store_stream_active(s->store)) {
    ccord_mutex_unlock(s->audio_mtx);
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: start a stream before sending audio");
  }
  ccord_status st = ccord_transport_send_stream_pcm(s->transport, pcm, samples, err);
  ccord_mutex_unlock(s->audio_mtx);
  return st;
}

size_t ccord_session_stream_poll_pcm(ccord_session *s, int16_t *out_pcm, size_t cap_samples)
{
  if (!s || !out_pcm || cap_samples == 0) return 0;
  ccord_mutex_lock(s->audio_mtx);
  if (!s->transport || !atomic_load_explicit(&s->watched_active, memory_order_acquire)) {
    ccord_mutex_unlock(s->audio_mtx);
    return 0;
  }
  size_t n = ccord_transport_poll_stream_pcm(s->transport, out_pcm, cap_samples);
  ccord_mutex_unlock(s->audio_mtx);
  return n;
}

const ccord_call_state *ccord_session_incoming_call(const ccord_session *s)
{
  if (!s) return nullptr;
  ccord_id self = ccord_store_self_id(s->store);
  if (self == CCORD_ID_NONE) return nullptr;
  const ccord_voice_state *v = ccord_store_voice_user(s->store, CCORD_DM_GUILD_ID, self);
  for (size_t i = 0; i < ccord_store_call_count(s->store); i++) {
    const ccord_call_state *call = ccord_store_call_at(s->store, i);
    if (!call || !call->active) continue;
    if (v && v->channel_id == call->channel_id) continue;   // already answered
    for (size_t k = 0; k < call->ringing_count; k++) {
      if (call->ringing[k] == self) return call;
    }
  }
  return nullptr;
}

const ccord_stage_instance *ccord_session_stage_instance(const ccord_session *s, ccord_id channel_id)
{
  return s ? ccord_store_stage_instance(s->store, channel_id) : nullptr;
}

void ccord_session_voice_stats(const ccord_session *s, ccord_rtc_stats *out)
{
  if (!out) return;
  if (!s || ccord_store_active_voice_channel(s->store) == CCORD_ID_NONE) {
    *out = (ccord_rtc_stats){0};
    return;
  }
  ccord_store_rtc_stats(s->store, out);
}

void ccord_session_dave_status(const ccord_session *s, ccord_dave_status *out)
{
  if (!out) return;
  if (!s || ccord_store_active_voice_channel(s->store) == CCORD_ID_NONE) {
    *out = (ccord_dave_status){0};
    return;
  }
  ccord_store_dave_status(s->store, out);
}

// ---- messaging parity: TTS, forward, crosspost, mark unread, saved messages ----

static const ccord_message *find_known_message(const ccord_session *s, ccord_id channel_id, ccord_id message_id)
{
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (m) return m;
  for (size_t i = 0; i < s->search_count; i++) {
    if (s->search_results[i].channel_id == channel_id && s->search_results[i].id == message_id) {
      return &s->search_results[i];
    }
  }
  size_t nsaved = ccord_store_saved_message_count(s->store);
  for (size_t i = 0; i < nsaved; i++) {
    const ccord_message *sm = ccord_store_saved_message_at(s->store, i);
    if (sm && sm->channel_id == channel_id && sm->id == message_id) return sm;
  }
  return nullptr;
}

ccord_status ccord_session_send_tts_message(ccord_session *s, ccord_id channel_id, const char *utf8, size_t len,
                                            ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s || !utf8) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (len > CCORD_SEND_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message is longer than %u bytes", CCORD_SEND_MAX_BYTES);
  }
  if (blank(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "message is empty");
  if (memchr(utf8, 0, len) || !ccord_utf8_valid_text(utf8, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message is not valid text");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  ccord_id self = ccord_store_self_id(s->store);
  if (ch->guild_id != CCORD_DM_GUILD_ID && ccord_store_permissions_exact(s->store, ch->guild_id, self)) {
    uint64_t ch_perms = ccord_store_permissions(s->store, ch->guild_id, channel_id, self, session_now_unix_ms(s));
    uint64_t send_bit = ccord_discord_type_is_thread(ch->discord_type) ? CCORD_PERM_SEND_MESSAGES_IN_THREADS
                                                                       : CCORD_PERM_SEND_MESSAGES;
    if ((ch_perms & send_bit) == 0u || (ch_perms & CCORD_PERM_SEND_TTS_MESSAGES) == 0u) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Send TTS Messages permission in this channel.");
    }
  }
  return ccord_transport_send_tts_message(s->transport, channel_id, utf8, len, err);
}

ccord_status ccord_session_forward_message(ccord_session *s, ccord_id target_channel_id, ccord_id source_channel_id,
                                           ccord_id message_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (target_channel_id == CCORD_ID_NONE || source_channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid forward ids");
  }
  const ccord_channel *dst = ccord_store_channel(s->store, target_channel_id);
  if (!dst) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown target channel");
  if (dst->kind != CCORD_CHANNEL_TEXT || ccord_store_channel_is_forum(s->store, target_channel_id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "target channel is not a text channel");
  }
  if (dst->guild_id == CCORD_DM_GUILD_ID && dst->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, dst, err);
  if (gate != CCORD_OK) return gate;
  const ccord_channel *src = ccord_store_channel(s->store, source_channel_id);
  if (!src) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown source channel");
  const ccord_message *m = find_known_message(s, source_channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(m->id) || ccord_message_is_ephemeral(m)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message cannot be forwarded");
  }
  ccord_id self = ccord_store_self_id(s->store);
  if (src->guild_id != CCORD_DM_GUILD_ID && ccord_store_permissions_exact(s->store, src->guild_id, self)) {
    uint64_t src_perms = ccord_store_permissions(s->store, src->guild_id, source_channel_id, self, session_now_unix_ms(s));
    if ((src_perms & (CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_READ_MESSAGE_HISTORY)) !=
        (CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_READ_MESSAGE_HISTORY)) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION,
                             "You need Read Message History permission in the source channel.");
    }
  }
  if (dst->guild_id != CCORD_DM_GUILD_ID && ccord_store_permissions_exact(s->store, dst->guild_id, self)) {
    uint64_t dst_perms = ccord_store_permissions(s->store, dst->guild_id, target_channel_id, self, session_now_unix_ms(s));
    uint64_t send_bit = ccord_discord_type_is_thread(dst->discord_type) ? CCORD_PERM_SEND_MESSAGES_IN_THREADS
                                                                        : CCORD_PERM_SEND_MESSAGES;
    if ((dst_perms & send_bit) == 0u) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Send Messages permission in the target channel.");
    }
  }
  return ccord_transport_forward_message(s->transport, target_channel_id, source_channel_id, message_id, err);
}

ccord_status ccord_session_crosspost_message(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                             ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid crosspost ids");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (ch->discord_type != CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "Only announcement channel messages can be published.");
  }
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(m->id) || ccord_message_is_ephemeral(m)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message has not been sent yet");
  }
  ccord_id self = ccord_store_self_id(s->store);
  if (ccord_store_permissions_exact(s->store, ch->guild_id, self)) {
    uint64_t ch_perms = ccord_store_permissions(s->store, ch->guild_id, channel_id, self, session_now_unix_ms(s));
    if (m->author_id == self) {
      if ((ch_perms & CCORD_PERM_SEND_MESSAGES) == 0u) {
        return ccord_error_set(err, CCORD_ERR_PERMISSION,
                               "You need Send Messages permission to publish your message.");
      }
    } else if ((ch_perms & CCORD_PERM_MANAGE_MESSAGES) == 0u) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION,
                             "You need Manage Messages permission to publish messages from others.");
    }
  }
  return ccord_transport_crosspost_message(s->transport, channel_id, message_id, err);
}

ccord_status ccord_session_mark_unread(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: mark unread ids");
  }
  if (!ccord_store_channel(s->store, channel_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  }
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(m->id) || ccord_message_is_ephemeral(m)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message cannot be marked unread");
  }
  ccord_status st = ccord_transport_mark_unread(s->transport, channel_id, message_id, err);
  if (st != CCORD_OK) return st;
  return ccord_store_mark_unread(s->store, channel_id, message_id, err);
}

ccord_status ccord_session_save_message(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: save message ids");
  }
  if (!ccord_store_channel(s->store, channel_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  }
  const ccord_message *m = find_known_message(s, channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(m->id) || ccord_message_is_ephemeral(m)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message cannot be saved");
  }
  return ccord_store_save_message(s->store, m, err);
}

ccord_status ccord_session_unsave_message(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: unsave message ids");
  }
  return ccord_store_unsave_message(s->store, channel_id, message_id, err);
}

bool ccord_session_message_saved(const ccord_session *s, ccord_id channel_id, ccord_id message_id)
{
  return s ? ccord_store_message_saved(s->store, channel_id, message_id) : false;
}

size_t ccord_session_saved_message_count(const ccord_session *s)
{
  return s ? ccord_store_saved_message_count(s->store) : 0u;
}

const ccord_message *ccord_session_saved_message_at(const ccord_session *s, size_t i)
{
  return s ? ccord_store_saved_message_at(s->store, i) : nullptr;
}

// ---- stage lifecycle & guild scheduled events ----

ccord_status ccord_session_stage_start(ccord_session *s, ccord_id guild_id, ccord_id channel_id, const char *topic,
                                       ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_stage_create(&req, guild_id, channel_id, topic, 2u, nullptr, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);
}

ccord_status ccord_session_stage_end(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_stage_delete(&req, channel_id, nullptr, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);
}

// ---- embedded activities ----

ccord_status ccord_session_activity_shelf_fetch(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "Activities need a server.");
  }
  uint64_t now = ccord_clock_ms();
  if (now == 0) now = 1u;
  session_activity_shelf *ent = activity_shelf_for(s, guild_id);
  if (!ent) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
  if (ent->loading && ent->sent_ms != 0 && now < ent->sent_ms + SESSION_ACTIVITY_SHELF_WAIT_MS) {
    return CCORD_OK;   // the same guild's request is already awaiting its answer
  }
  if (ent->fetched_ms != 0 && now >= ent->fetched_ms && now < ent->fetched_ms + SESSION_ACTIVITY_SHELF_TTL_MS) {
    return CCORD_OK;   // the cached shelf is still fresh
  }
  ccord_admin_request req;
  ccord_status st = ccord_admin_activity_shelf(&req, guild_id, err);
  if (st != CCORD_OK) return st;
  ent->loading = true;
  ent->sent_ms = now;
  st = ccord_session_admin(s, &req, err);
  if (st != CCORD_OK) {   // refused before it left: keep the cached shelf and let the next ask retry
    ent = activity_shelf_for(s, guild_id);
    if (ent) {
      ent->loading = false;
      ent->sent_ms = 0;
    }
  }
  return st;
}

const ccord_activity_app *ccord_session_activity_shelf(const ccord_session *s, ccord_id guild_id, size_t *out_count)
{
  if (out_count) *out_count = 0;
  if (!s) return nullptr;
  for (size_t i = 0; i < s->nactivity_shelves; i++) {
    if (s->activity_shelves[i].guild_id == guild_id) {
      if (out_count) *out_count = s->activity_shelves[i].count;
      return s->activity_shelves[i].count > 0 ? s->activity_shelves[i].apps : nullptr;
    }
  }
  return nullptr;
}

ccord_status ccord_session_activity_launch(ccord_session *s, ccord_id channel_id, ccord_id application_id,
                                           ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st =
    ccord_admin_activity_launch(&req, channel_id, application_id, SESSION_ACTIVITY_SESSION_PLACEHOLDER, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);   // gates on the voice connection and the permission; the row follows
}

ccord_status ccord_session_activity_leave(ccord_session *s, const ccord_activity_instance *instance, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !instance) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  ccord_id location = instance->location_id != CCORD_ID_NONE ? instance->location_id : instance->channel_id;
  ccord_admin_request req;
  ccord_status st = ccord_admin_activity_leave(&req, instance->application_id, location, instance->instance_id, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);   // the local row changes when Discord's instance update arrives
}

const ccord_activity_instance *ccord_session_activity_instances(const ccord_session *s, ccord_id channel_id,
                                                                size_t *out_count)
{
  size_t n = s ? ccord_store_activity_instance_count(s->store, channel_id) : 0u;
  if (out_count) *out_count = n;
  // The store keeps a channel's rows contiguous, so the first row addresses all `n` of them.
  return n > 0 ? ccord_store_activity_instance_at(s->store, channel_id, 0) : nullptr;
}

ccord_status ccord_session_request_scheduled_events(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_event_list(&req, guild_id, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);
}

ccord_status ccord_session_rsvp_scheduled_event(ccord_session *s, ccord_id guild_id, ccord_id event_id, bool rsvp,
                                                ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = rsvp ? ccord_admin_event_rsvp_add(&req, guild_id, event_id, err)
                         : ccord_admin_event_rsvp_remove(&req, guild_id, event_id, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);
}

// ---- membership screening ----

ccord_status ccord_session_member_verification_fetch(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_get_member_verification(&req, guild_id, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);   // the cached form changes only when the answer arrives
}

const ccord_member_verification *ccord_session_member_verification(const ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nverification_cache; i++) {
    // An entry whose first answer could not be copied (out of memory) holds no form yet: version is never null in a real one.
    if (s->verification_cache[i].guild_id == guild_id) {
      return s->verification_cache[i].form.version ? &s->verification_cache[i].form : nullptr;
    }
  }
  return nullptr;
}

ccord_status ccord_session_member_verification_submit(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_member_verification *form = ccord_session_member_verification(s, guild_id);
  if (!form) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: no cached verification form");
  ccord_admin_request req;
  ccord_status st = ccord_admin_submit_member_verification(&req, guild_id, form, err);
  if (st != CCORD_OK) return st;
  // Nothing changes locally here: Discord's confirmation (the REST answer / GUILD_MEMBER_UPDATE) clears the
  // member's pending flag in the store, which reopens the send gate.
  return ccord_session_admin(s, &req, err);
}

bool ccord_session_self_pending(const ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return false;
  ccord_id self = ccord_store_self_id(s->store);
  if (self == CCORD_ID_NONE) return false;
  const ccord_member *m = ccord_store_find_member(s->store, guild_id, self);
  return m && m->pending;
}

ccord_status ccord_session_guild_profile_fetch(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid guild id");
  }
  uint64_t now = ccord_clock_ms();
  if (now == 0) now = 1u;
  session_guild_profile_cache *ent = guild_profile_for(s, guild_id);
  if (!ent) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
  if (ent->loading && ent->sent_ms != 0 && now < ent->sent_ms + SESSION_ACTIVITY_SHELF_WAIT_MS) {
    return CCORD_OK;
  }
  if (ent->fetched_ms != 0 && now >= ent->fetched_ms && now < ent->fetched_ms + SESSION_ACTIVITY_SHELF_TTL_MS) {
    return CCORD_OK;
  }
  ccord_admin_request req;
  ccord_status st = ccord_admin_get_guild_profile(&req, guild_id, err);
  if (st != CCORD_OK) return st;
  ent->loading = true;
  ent->sent_ms = now;
  st = ccord_session_admin(s, &req, err);
  if (st != CCORD_OK) {
    ent = guild_profile_for(s, guild_id);
    if (ent) {
      ent->loading = false;
      ent->sent_ms = 0;
    }
  }
  return st;
}

const ccord_guild_profile *ccord_session_guild_profile(const ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return nullptr;
  for (size_t i = 0; i < s->nguild_profiles; i++) {
    if (s->guild_profiles[i].guild_id == guild_id && s->guild_profiles[i].has_profile) {
      return &s->guild_profiles[i].profile;
    }
  }
  return nullptr;
}

bool ccord_session_guild_profile_loading(const ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return false;
  for (size_t i = 0; i < s->nguild_profiles; i++) {
    if (s->guild_profiles[i].guild_id == guild_id) {
      return s->guild_profiles[i].loading && !s->guild_profiles[i].has_profile;
    }
  }
  return false;
}
