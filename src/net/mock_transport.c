// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/model/permissions.h>
#include <ccord/model/rich.h>
#include <ccord/model/search.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>
#include <ccord/net/voice_reconnect.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>

#define MOCK_DEFAULT_SEED 0xC0FFEEull
#define MOCK_DEFAULT_BIG 5000u
#define MOCK_DEFAULT_BASE_MS 1793534400000ll // 2026-11-01T12:00:00Z
#define MOCK_MSG_STEP_MS 45000ll
#define MOCK_HISTORY_DEFAULT 50u
#define MOCK_ID_BASE 100000000000ull
#define MOCK_GENERAL_ID 3111u // #general of server 1 (stress channel)
#define MOCK_NUM_USERS 12u
#define MOCK_NUM_GUILDS 3u
#define MOCK_GUILDS_MAX 16u
#define MOCK_ROLES_PER_GUILD 4u   // @everyone, Members, Moderators, Admins
#define MOCK_NOTIFY_MAX 64u          // notification rows (server rows and channel overrides)
#define MOCK_ROLES_MAX 128u          // role table capacity across all servers
#define MOCK_MEMBER_ROLES_MAX 8u     // roles one member can hold
#define MOCK_BANS_MAX 64u
#define MOCK_INVITES_MAX 32u
#define MOCK_AUDIT_MAX 200u
#define MOCK_EMOJIS_MAX (MOCK_GUILDS_MAX * 64u)
#define MOCK_STICKERS_MAX (MOCK_GUILDS_MAX * 32u)
#define MOCK_SOUNDBOARD_MAX (MOCK_GUILDS_MAX * 32u)
#define MOCK_AUTOMOD_MAX (MOCK_GUILDS_MAX * 16u)
#define MOCK_AUTOMOD_ID_BASE 92000u
#define MOCK_WEBHOOKS_MAX (MOCK_GUILDS_MAX * 16u)
#define MOCK_WEBHOOK_ID_BASE 93000u
#define MOCK_APP_PERMS_MAX (MOCK_GUILDS_MAX * 8u)
#define MOCK_ONBOARDING_ID_BASE 96000u
#define MOCK_DISCORD_EPOCH_MS 1420070400000ull
#define MOCK_MAX_OVERWRITE_CHANNELS 64u
#define MOCK_MAX_OVERWRITES_PER_CHAN CCORD_ADMIN_CLONE_OVERWRITES_MAX
#define MOCK_MAX_FORUM_METAS 16u
#define MOCK_ROLE_BASE 5000u      // role ids: MOCK_ROLE_BASE + guild_index * 10 + 1 (Admins) / 2 (Moderators) / 3 (Members)
#define MOCK_VOICE_PCM_CAP (960u * 2u * 16u)
#define MOCK_CAM_FIFO 4u                              // per remote camera sender (== CCORD_VOICE_CAM_RING)
#define MOCK_STREAM_FRAMES_MAX 8u                     // Go Live H.264 Annex-B access-unit FIFO slots
#define MOCK_STREAM_FRAME_MAX_BYTES (256u * 1024u)    // largest access unit the mock accepts
#define MOCK_STREAM_PCM_CAP 48000u                    // 1 s of 48 kHz mono S16LE stream audio
#define MOCK_CALLS_MAX 8u         // one DM/group call row per conversation
#define MOCK_STAGES_MAX 16u       // one stage instance per stage channel
#define MOCK_SOUNDBOARD_PCM_SAMPLES 960u  // 20 ms of 48 kHz stereo per simulated soundboard play
#define MOCK_AUTH_SESSIONS_MAX 16u  // devices listed by GET /auth/sessions
#define MOCK_CONNECTIONS_MAX 32u    // linked accounts listed by GET /users/@me/connections
#define MOCK_EVENTS_MAX 32u         // guild scheduled events across all servers
#define MOCK_ACTIVITIES_MAX 8u      // running embedded-activity instances across all voice channels
#define MOCK_EVENT_ID_BASE 97000u   // scheduled-event ids (REST answers assign them)
#define MOCK_SETTINGS_PROTO_MAX 48000u // raw FrecencyUserSettings bytes one PATCH may carry (base64 fits the body cap)
#define MOCK_UPLOAD_CACHE_MAX 32u      // retained upload bodies served back by fetch_media (voice-message playback)
#define MOCK_UPLOAD_CACHE_BYTES_MAX (32u * 1024u * 1024u) // total bytes kept; oldest uploads are dropped first
#define MOCK_INFLIGHT_UPLOADS_MAX 16u

#define MOCK_RANGE_FILES_MAX 16u       // virtual files registered through ccord_mock_serve_range_file
#define MOCK_RANGE_URL_PART_MAX 128u   // longest url_part kept (longer is refused, never truncated)
#define MOCK_RANGE_LOG 16u             // ring size of ccord_mock_range_stats

typedef struct mock_range_slot {
  ccord_mock_range_file f;             // f.url_part points at url_part below
  char url_part[MOCK_RANGE_URL_PART_MAX];
  uint32_t requests;                   // requests answered for this file (drives fail_after_requests)
} mock_range_slot;

typedef struct mock_item {
  ccord_net_event ev;
  ccord_message *msgs;  // owned array; ev.messages points here. Content strings are owned by the dataset.
  ccord_voice_state *vstate;  // owned; ev.voice_state points here
  ccord_stream_state *stream; // owned; ev.stream points here
  ccord_stream_signal *sig;   // owned; ev.stream_signal points here
  ccord_channel *chan;        // owned; ev.channel / ev.guild_channels points here
  uint8_t *media_data;        // owned; ev.media_data points here
  ccord_user_profile *profile; // owned; ev.user_profile points here
  ccord_forum_post *fposts;   // owned; ev.forum_posts points here
  char *draft;                // owned; ev.draft points here
  ccord_member *member;       // owned; ev.guild_members points here
  ccord_guild *guild;         // owned; ev.guild points here
  ccord_role *roles_own;      // owned; ev.roles points here (strings are stable)
  ccord_ban *bans_own;        // owned; ev.bans points here
  ccord_invite *invites_own;  // owned; ev.invites points here
  ccord_audit_entry *audit_own; // owned; ev.audit points here
  ccord_notify_scope *notify_own; // owned; ev.notify points here
  ccord_interaction_modal *modal_own; // owned; ev.modal points here
  ccord_slash_command *slash_own; // owned; ev.slash_commands points here
  ccord_slash_choice *choices_own; // owned; ev.slash_choices points here
  ccord_user *users_own;      // owned; ev.users points here (REACTION_USERS)
  ccord_guild_folder_item *folder_items_own;
  ccord_id *reorder_ids_own;
  int32_t *reorder_pos_own;
  ccord_id *reorder_par_own;
  uint8_t *reorder_hpar_own;
  ccord_invite_preview *invite_preview_own;
  ccord_discover_guild *discovery_own;
  char *discovery_query_own;
  ccord_emoji *emojis_own;
  ccord_sticker *stickers_own;
  ccord_soundboard_sound *soundboard_own;
  ccord_automod_rule *automod_own;
  ccord_webhook *webhooks_own;
  ccord_app_cmd_perms *app_perms_own;
  ccord_welcome_screen *welcome_own;
  ccord_onboarding *onboarding_own;
  ccord_forum_meta *forum_metas_own;
  ccord_channel_overwrites *overwrites_own;
  ccord_overwrite *overwrite_items_own;
  ccord_call_state *call_own;          // owned; ev.call points here (CALL_CREATE / CALL_UPDATE)
  ccord_activity_app *activity_apps_own; // owned; ev.activity_apps points here (ACTIVITY_SHELF)
  ccord_stage_instance *stage_own;     // owned; ev.stage_instance points here
  ccord_voice_server *vserver_own;     // owned; ev.voice_server points here (token/endpoint are static literals)
  char *voice_status_own;              // owned; ev.voice_status points here
  ccord_user_settings *settings_own;   // owned; ev.user_settings points here (M6)
  ccord_auth_session *auth_sessions_own; // owned; ev.auth_sessions points here (M6)
  ccord_connection *connections_own;   // owned; ev.connections points here (M6)
  ccord_scheduled_event *events_own;   // owned; ev.scheduled_events points here
  ccord_member_verification *mv_own;   // owned; ev.member_verification points here (strings are static literals)
  uint8_t *raw_proto_own;              // owned; ev.raw_proto points here (SETTINGS_PROTO)
  uint64_t due;
} mock_item;

// Server administration state the mock server keeps (see the admin section below).
typedef struct mock_mrow { ccord_id guild_id, user_id; ccord_id ids[MOCK_MEMBER_ROLES_MAX]; size_t count; bool present; } mock_mrow;
typedef struct mock_upload { char *url; uint8_t *data; size_t len; } mock_upload;
typedef struct mock_inflight_upload {
  uint64_t upload_id;
  ccord_id channel_id;
  ccord_id message_id;
  uint32_t file_count;
  uint64_t file_sizes[CCORD_RICH_MAX_ATTACHMENTS];
  uint64_t total_bytes;
  uint64_t start_ms;
  uint64_t due_ms;
  char filenames[CCORD_RICH_MAX_ATTACHMENTS][128];
} mock_inflight_upload;
typedef struct mock_ban { ccord_id guild_id; ccord_ban ban; } mock_ban;
typedef struct mock_invite { ccord_id guild_id; ccord_invite inv; } mock_invite;
typedef struct mock_audit { ccord_id guild_id; ccord_audit_entry e; } mock_audit;

// Frees everything an item owns. The single place that knows the list, so a new owned field cannot be missed.
static void item_release(mock_item *it)
{
  ccord_free(it->msgs);
  ccord_free(it->vstate);
  ccord_free(it->stream);
  ccord_free(it->sig);
  ccord_free(it->chan);
  ccord_free(it->media_data);
  ccord_free(it->profile);
  ccord_free(it->fposts);
  ccord_free(it->draft);
  ccord_free(it->member);
  ccord_free(it->guild);
  ccord_free(it->roles_own);
  ccord_free(it->bans_own);
  ccord_free(it->invites_own);
  ccord_free(it->audit_own);
  ccord_free(it->notify_own);
  ccord_free(it->modal_own);
  ccord_free(it->slash_own);
  ccord_free(it->choices_own);
  ccord_free(it->users_own);
  ccord_free(it->folder_items_own);
  ccord_free(it->reorder_ids_own);
  ccord_free(it->reorder_pos_own);
  ccord_free(it->reorder_par_own);
  ccord_free(it->reorder_hpar_own);
  ccord_free(it->invite_preview_own);
  ccord_free(it->discovery_own);
  ccord_free(it->discovery_query_own);
  ccord_free(it->emojis_own);
  ccord_free(it->stickers_own);
  ccord_free(it->soundboard_own);
  ccord_free(it->automod_own);
  ccord_free(it->webhooks_own);
  ccord_free(it->app_perms_own);
  ccord_free(it->welcome_own);
  ccord_free(it->onboarding_own);
  ccord_free(it->forum_metas_own);
  ccord_free(it->overwrites_own);
  ccord_free(it->overwrite_items_own);
  ccord_free(it->call_own);
  ccord_free(it->activity_apps_own);
  ccord_free(it->stage_own);
  ccord_free(it->vserver_own);
  ccord_free(it->voice_status_own);
  ccord_free(it->settings_own);
  ccord_free(it->auth_sessions_own);
  ccord_free(it->connections_own);
  ccord_free(it->events_own);
  ccord_free(it->mv_own);
  ccord_free(it->raw_proto_own);
}

// Gateway link model. Zero state means "up", so a mock that never sees a fault behaves as it always did.
#define MOCK_LINK_TTL_DEFAULT_MS 120000u
#define MOCK_LINK_HB_INTERVAL_MS 41250u
typedef enum mock_link_state : uint8_t { MOCK_LINK_UP = 0, MOCK_LINK_DOWN, MOCK_LINK_ATTEMPT } mock_link_state;
typedef struct mock_link {
  ccord_gwpace pace;
  mock_link_state state;
  bool dead_silent;       // up from the mock's side but delivering nothing
  bool network_down;      // ccord_mock_set_network(false)
  bool ttl_set;
  bool session_expired;   // Discord answered op 9 d:false: the next attempt must IDENTIFY
  bool attempt_resume;    // the attempt in flight (ATTEMPT) is a RESUME
  uint64_t ttl_ms;
  uint64_t lost_ms;       // when the connection was lost (the session's TTL runs from here)
  uint64_t up_ms;         // ATTEMPT: when the socket finishes opening
  ccord_mock_link_stats stats;
} mock_link;

struct mock_report;
typedef struct mock {
  ccord_transport base; // must be first
  ccord_mock_config cfg;
  char late_poll_fail[160];   // non-empty: create_poll is accepted, then fails later with this text
  char ring_refusal[160];     // non-empty: a call ring is refused with this text (the call itself still starts)
  ccord_id last_ring_channel; // the channel of the last call ring
  ccord_mock_vlink_stats vstats;   // voice outage simulation (ccord_mock_voice_drop): counters of the last drop
  struct { uint64_t due; uint8_t state; } vstate_tl[16]; size_t nvstate_tl;   // m->rtc.state over time
  uint64_t rng;
  uint64_t start_ms;
  uint64_t next_ambient;
  uint64_t next_msg_id;
  ccord_id next_thread_id;
  ccord_id self_id;
  bool connected;
  ccord_user users[MOCK_NUM_USERS];
  ccord_guild guilds[MOCK_GUILDS_MAX]; size_t nguilds;
  ccord_channel *channels; size_t nchannels;
  ccord_member *members; size_t nmembers;
  // Permission data (see build_roles): a role table per guild, one role per member, and a few channel overwrites.
  ccord_role roles[MOCK_ROLES_MAX]; size_t nroles;   // grouped by guild, so a snapshot can send them as they are
  mock_mrow mrows[MOCK_GUILDS_MAX * MOCK_NUM_USERS];
  ccord_member_roles member_roles[MOCK_GUILDS_MAX * MOCK_NUM_USERS]; size_t nmember_roles;   // view of mrows
  ccord_id next_role_id;
  mock_ban bans[MOCK_BANS_MAX]; size_t nbans;
  mock_invite invites[MOCK_INVITES_MAX]; size_t ninvites; uint32_t next_invite;
  mock_audit audit[MOCK_AUDIT_MAX]; size_t naudit; ccord_id last_audit_id;
  ccord_emoji emojis[MOCK_EMOJIS_MAX]; size_t nemojis;
  ccord_sticker stickers[MOCK_STICKERS_MAX]; size_t nstickers;
  ccord_soundboard_sound soundboard[MOCK_SOUNDBOARD_MAX]; size_t nsoundboard;
  ccord_automod_rule automod[MOCK_AUTOMOD_MAX]; size_t nautomod;
  ccord_arena automod_arena;
  ccord_id next_automod_id;
  ccord_webhook webhooks[MOCK_WEBHOOKS_MAX]; size_t nwebhooks;
  ccord_id next_webhook_id;
  ccord_app_cmd_perms app_perms[MOCK_APP_PERMS_MAX]; size_t napp_perms;
  struct {
    ccord_id guild_id;
    bool present;
    ccord_welcome_screen ws;
    ccord_welcome_channel chs[CCORD_WELCOME_MAX_CHANNELS];
  } welcome[MOCK_GUILDS_MAX];
  struct {
    ccord_id guild_id;
    bool present;
    ccord_onboarding ob;
  } onboarding[MOCK_GUILDS_MAX];
  ccord_id next_onboarding_id;
  bool admin_failed;                       // the request being handled was answered with a failure
  ccord_notify_scope notify[MOCK_NOTIFY_MAX]; size_t nnotify;   // the user's own settings, grouped by server
  ccord_channel_overwrites overwrites[MOCK_MAX_OVERWRITE_CHANNELS];
  ccord_overwrite overwrite_pool[MOCK_MAX_OVERWRITE_CHANNELS][MOCK_MAX_OVERWRITES_PER_CHAN];
  size_t noverwrites;
  ccord_message **chmsgs; size_t *chn, *chcap; // per channel message storage
  ccord_voice_state voices[MOCK_NUM_USERS]; size_t nvoices;
  ccord_relationship relationships[MOCK_NUM_USERS]; size_t nrelationships;
  ccord_user_note notes[MOCK_NUM_USERS]; size_t nnotes;
  ccord_forum_meta forum_metas[MOCK_MAX_FORUM_METAS];
  size_t nforum_metas;
  ccord_forum_post *forum_posts; size_t nforum_posts, capforum_posts;
  ccord_stream_state stream; bool has_stream;
  bool self_mute, self_deaf;
  int16_t voice_pcm[MOCK_VOICE_PCM_CAP];
  size_t voice_pcm_len;
  // Outbound Go Live media (M5): access units and 48 kHz mono stream audio queued by send_stream_* and
  // drained by poll_stream_*. Both are cleared when the broadcast or the voice connection ends.
  struct {
    uint8_t *data;  // owned Annex-B access unit
    size_t len;
    uint32_t ts90k;
    bool keyframe;
  } stream_frames[MOCK_STREAM_FRAMES_MAX];
  size_t stream_frame_head, stream_frame_count;
  // Remote voice cameras: registration order = arrival order. A sender is listed
  // only while self is in the call it sits in; the first CCORD_CAM_DECODE_MAX listed ones are `decoding`.
  struct {
    ccord_id user_id;
    uint32_t gen;
    bool decoding;
    struct { uint8_t *data; size_t len; uint32_t ts90k; bool keyframe; } fifo[MOCK_CAM_FIFO];
    size_t head, count;
  } cams[MOCK_NUM_USERS];
  size_t ncams;
  uint32_t cam_gen;
  int16_t stream_pcm[MOCK_STREAM_PCM_CAP];
  size_t stream_pcm_head, stream_pcm_count;
  ccord_snapshot snap;
  mock_item *queue; size_t nq, capq;
  mock_item current;
  bool have_current;
  const char **garbage; size_t ngarbage, capgarbage; // retired message contents kept until destroy
  ccord_message_rich **rgarbage; size_t nrgarbage, caprgarbage; // retired rich blocks, same reason
  ccord_id next_admin_id;                  // ids for channels/threads created through admin_action
  ccord_admin_request *last_admin;         // test hook: the most recent admin request
  size_t admin_count;
  size_t report_menu_calls, report_submits;   // report hooks: GET calls, accepted POSTs
  struct mock_report *last_report;           // deep copy of the last accepted report request
  char report_menu_fail[160];                // non-empty: the menu GET fails later with this text
  char late_report_fail[160];                // non-empty: a submit is accepted, then fails later with this text
  bool report_broken_link;                   // message menu node 1 gets a child pointing at a missing node
  size_t calls[CCORD_MOCK_CALL_COUNT];    // request-budget hooks (ccord_mock_call_count)
  uint32_t last_member_chunks;            // the last request for more of the member list
  size_t interaction_count;
  uint8_t last_interaction_type;
  uint8_t last_interaction_comp_type;
  char last_interaction_custom_id[CCORD_RICH_CUSTOM_ID_MAX_BYTES + 1];
  char *last_interaction_values[CCORD_RICH_MAX_SELECT_OPTIONS];
  size_t last_interaction_nvalues;
  struct {
    ccord_id channel_id;
    uint64_t sent_ms;
    bool has_sent;
  } slowmode_sent[32];
  // M5 voice & video simulation: DM/group calls, stage instances,
  // the local audio prefs and the DAVE/RTC snapshots of the simulated voice connection.
  ccord_call_state dm_calls[MOCK_CALLS_MAX]; size_t ndm_calls;
  ccord_stage_instance stages[MOCK_STAGES_MAX]; size_t nstages;
  struct { ccord_id user_id; uint8_t pct; } user_volume[MOCK_NUM_USERS]; size_t nuser_volume;
  ccord_voice_config voice_cfg; bool has_voice_cfg;
  ccord_dave_status dave;
  ccord_rtc_stats rtc;
  // M6 account settings, devices & connections.
  ccord_user_settings user_settings;
  ccord_auth_session auth_sessions[MOCK_AUTH_SESSIONS_MAX]; size_t nauth_sessions;
  ccord_connection connections[MOCK_CONNECTIONS_MAX]; size_t nconnections;
  // Messaging parity: the last PATCHed FrecencyUserSettings bytes (settings-proto/2). Until the first PATCH the
  // seeded default answers GETs; afterwards the stored bytes round-trip losslessly (unknown wire fields included).
  uint8_t *settings_proto; size_t settings_proto_len; bool has_settings_proto;
  // Type-1 PreloadedUserSettings (settings-proto/1) as the server holds it: set_status_proto patches it, and every
  // PATCH is recorded (raw bytes and the {"settings":"<b64>"} body) so a slice can read exactly what went out.
  // Gateway op 3 as the mock received it: how many, and the last presence with its custom status text.
  bool own_presence_silent;   // set_presence / set_presence_ex push no PRESENCE_UPDATE for the own user
  size_t op3_count; ccord_presence op3_presence; char op3_text[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  uint8_t *status1; size_t status1_len;
  struct mock_status1_patch { uint8_t *raw; size_t len; char *body; } *status1_patches; size_t nstatus1_patches;
  // Rich Presence: the activities array last set through set_rich_activities (RPC SET_ACTIVITY); nullptr = none.
  char *rich_json; size_t rich_len;
  // Upload bodies keyed by the attachment URL the mock answers with, so fetch_media serves exactly what was uploaded
  // (a voice message's Ogg plays back instead of a canned image). FIFO eviction under both caps; freed on destroy.
  mock_upload uploads[MOCK_UPLOAD_CACHE_MAX]; size_t nuploads; size_t upload_bytes;
  mock_inflight_upload inflight_uploads[MOCK_INFLIGHT_UPLOADS_MAX];
  size_t ninflight_uploads;
  uint64_t next_upload_id;
  // Guild scheduled events: the server rows behind /guilds/{g}/scheduled-events (LIST / CREATE / MODIFY / DELETE / RSVP).
  ccord_scheduled_event events[MOCK_EVENTS_MAX]; size_t nevents;
  // Embedded activities: the running instances behind POST /activities/{c}/{app} (LAUNCH) and .../leave (LEAVE).
  ccord_activity_instance activities[MOCK_ACTIVITIES_MAX]; size_t nactivities;
  ccord_id next_event_id;
  ccord_id next_stage_id;
  bool fail_next_admin;                    // test hook: answer the next admin request with ACTION_FAILED
  char fail_media[128];                    // test hook: media URLs containing this fail to download ("" = none)
  // Ranged media: virtual files served by fetch_media_range (ccord_mock_serve_range_file) and the request stats.
  mock_range_slot range_files[MOCK_RANGE_FILES_MAX]; size_t nrange_files;
  ccord_mock_range_stats range_stats;
  mock_link link;                          // gateway link model (ccord_mock_drop_gateway / network_event)
} mock;
static const char *const user_names[MOCK_NUM_USERS] = {
  "You", "Ada", "Bjorn", "Chen", "Dara", "Eli", "Fatima", "Gus", "Hana", "Ivo", "Lua Bot", "Pixel Bot",
};
static const char *const user_avatars[MOCK_NUM_USERS] = {
  "https://cdn.discordapp.com/avatars/1001/a0.png", "https://cdn.discordapp.com/avatars/1002/a1.png",
  "https://cdn.discordapp.com/avatars/1003/a2.png", "https://cdn.discordapp.com/avatars/1004/a3.png",
  "https://cdn.discordapp.com/avatars/1005/a4.png", "https://cdn.discordapp.com/avatars/1006/a5.png",
  "https://cdn.discordapp.com/avatars/1007/a6.png", "https://cdn.discordapp.com/avatars/1008/a7.png",
  "https://cdn.discordapp.com/avatars/1009/a8.png", "https://cdn.discordapp.com/avatars/1010/a9.png",
  "https://cdn.discordapp.com/avatars/1011/a10.png", "https://cdn.discordapp.com/avatars/1012/a11.png",
};
static const char *const guild_names[MOCK_NUM_GUILDS] = {"C-Cord HQ", "Lua Lounge", "Pixel Garden"};
static const char *const guild_icons[MOCK_NUM_GUILDS] = {
  "https://cdn.discordapp.com/icons/2001/g0.png",
  "https://cdn.discordapp.com/icons/2002/g1.png",
  "https://cdn.discordapp.com/icons/2003/g2.png",
};
static const char *const words[] = {
  "ship", "build", "coffee", "meeting", "lua", "pixel", "render", "queue", "bug", "patch", "review", "merge",
  "tests", "release", "sprite", "frame", "vector", "cache", "socket", "plugin", "theme", "layout", "scroll",
  "latency", "bench", "docs", "idea", "sketch", "palette", "font", "thread", "window",
};
#define NWORDS (sizeof words / sizeof words[0])

typedef struct channel_spec {
  const char *name;
  bool voice;
} channel_spec;

static const channel_spec text_names[MOCK_NUM_GUILDS][6] = {
  {{"general", false}, {"announcements", false}, {"dev", false}, {"off-topic", false}, {"design-forum", false}},
  {{"general", false}, {"lua-help", false}, {"showcase", false}, {"off-topic", false}, {"random", false}},
  {{"general", false}, {"art", false}, {"critique", false}, {"empty", false}, {"off-topic", false}, {"mod-lounge", false}},
};
static const size_t text_counts[MOCK_NUM_GUILDS] = {5, 5, 6};

static uint64_t splitmix(uint64_t *s)
{
  uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

static uint32_t rnd(mock *m, uint32_t n)
{
  return n ? (uint32_t)(splitmix(&m->rng) % n) : 0;
}

static uint64_t now_of(const mock *m)
{
  return m->cfg.now_ms ? m->cfg.now_ms(m->cfg.clock_ud) : ccord_clock_ms();
}

// Frees a message content string the mock owns; public structs hold const char *.
static void free_content(const char **p)
{
  union { const char *c; void *v; } u = {.c = *p};
  ccord_free(u.v);
  *p = nullptr;
}

static void free_rich(const ccord_message_rich **p)
{
  union { const ccord_message_rich *c; ccord_message_rich *m; } u = {.c = *p};
  ccord_message_rich_free(u.m);
  *p = nullptr;
}

// Keeps an old content string alive until destroy so queued events never point at freed memory.
static void retire_content(mock *m, const char *content)
{
  if (!content) return;
  if (m->ngarbage == m->capgarbage) {
    size_t ncap = m->capgarbage ? m->capgarbage * 2 : 16;
    const char **grown = ccord_realloc_array(m->garbage, ncap, sizeof *grown);
    if (!grown) return; // leak is preferable to a dangling queued message
    m->garbage = grown;
    m->capgarbage = ncap;
  }
  m->garbage[m->ngarbage++] = content;
}

// A rich block lives as long as the dataset message that holds it, and past that until destroy for the same reason.
static void retire_rich(mock *m, const ccord_message_rich *rich)
{
  if (!rich) return;
  if (m->nrgarbage == m->caprgarbage) {
    size_t ncap = m->caprgarbage ? m->caprgarbage * 2 : 8;
    ccord_message_rich **grown = ccord_realloc_array(m->rgarbage, ncap, sizeof *grown);
    if (!grown) return;   // leak is preferable to a dangling queued message
    m->rgarbage = grown;
    m->caprgarbage = ncap;
  }
  union { const ccord_message_rich *c; ccord_message_rich *m; } u = {.c = rich};
  m->rgarbage[m->nrgarbage++] = u.m;
}

// ---- content generation (deterministic per seed) ----

static void append_words(ccord_buf *b, mock *m, uint32_t count)
{
  for (uint32_t i = 0; i < count; i++) {
    if (i) (void)ccord_buf_append_str(b, " ");
    (void)ccord_buf_append_str(b, words[rnd(m, (uint32_t)NWORDS)]);
  }
}

static char *make_content(mock *m, size_t index)
{
  ccord_buf b;
  ccord_buf_init(&b, 0);
  uint32_t r = rnd(m, 100);
  if (r < 40) {
    append_words(&b, m, 3 + rnd(m, 6));
    (void)ccord_buf_append_str(&b, rnd(m, 3) == 0 ? "?" : ".");
  } else if (r < 55) {
    uint32_t lines = 2 + rnd(m, 2);
    for (uint32_t i = 0; i < lines; i++) {
      if (i) (void)ccord_buf_append_str(&b, "\n");
      append_words(&b, m, 3 + rnd(m, 4));
    }
  } else if (r < 65) {
    for (int i = 0; i < 4; i++) (void)ccord_buf_append_str(&b, "supercalifragilistic");
  } else if (r < 75) {
    ccord_buf_appendf(&b, "https://example.com/c/%zu?ref=mock", index);
  } else if (r < 85) {
    append_words(&b, m, 25 + rnd(m, 20));
    (void)ccord_buf_append_str(&b, ".");
  } else if (r < 95) {
    static const char *const shorts[] = {"ok", "lol", "+1", "see #dev", "nice"};
    (void)ccord_buf_append_str(&b, shorts[rnd(m, 5)]);
  } else {
    ccord_buf_appendf(&b, "Reply with the attached file (v%zu).", index);
  }
  size_t n = 0;
  char *out = ccord_buf_take(&b, &n);
  return out;
}

// ---- dataset ----

static ccord_status chan_append(mock *m, size_t ci, ccord_message msg, ccord_error *err)
{
  if (m->chn[ci] == m->chcap[ci]) {
    size_t ncap = m->chcap[ci] ? m->chcap[ci] * 2 : 64;
    ccord_message *g = ccord_realloc_array(m->chmsgs[ci], ncap, sizeof *g);
    if (!g) {
      free_content(&msg.content);
      free_rich(&msg.rich);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    m->chmsgs[ci] = g;
    m->chcap[ci] = ncap;
  }
  m->chmsgs[ci][m->chn[ci]++] = msg;
  return CCORD_OK;
}

// The mock's roles. Rank is the member-list rank (0 = top); position is the role hierarchy (higher outranks lower).
typedef struct mock_role_spec {
  const char *name;
  uint32_t color;
  uint8_t rank;
  int32_t position;
  uint64_t permissions;
  bool hoist;
} mock_role_spec;
enum { MOCK_ROLE_ADMINS = 0, MOCK_ROLE_MODERATORS, MOCK_ROLE_MEMBERS };
static const mock_role_spec k_role_specs[3] = {
  {"Admins", 0xF0B232u, 0, 3, CCORD_PERM_ADMINISTRATOR, true},
  {"Moderators", 0x2ECC71u, 1, 2,
   CCORD_PERM_KICK_MEMBERS | CCORD_PERM_BAN_MEMBERS | CCORD_PERM_MANAGE_MESSAGES | CCORD_PERM_MODERATE_MEMBERS |
     CCORD_PERM_MANAGE_NICKNAMES | CCORD_PERM_MENTION_EVERYONE | CCORD_PERM_MANAGE_THREADS | CCORD_PERM_PIN_MESSAGES,
   true},
  {"Members", 0u, 2, 1, 0, false},
};
// What everyone may do before roles and overwrites say otherwise.
#define MOCK_EVERYONE_PERMS                                                                                          \
  (CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_SEND_MESSAGES | CCORD_PERM_READ_MESSAGE_HISTORY | CCORD_PERM_ADD_REACTIONS | \
   CCORD_PERM_EMBED_LINKS | CCORD_PERM_ATTACH_FILES | CCORD_PERM_USE_EXTERNAL_EMOJIS | CCORD_PERM_CONNECT |           \
   CCORD_PERM_SPEAK | CCORD_PERM_USE_VAD | CCORD_PERM_CHANGE_NICKNAME | CCORD_PERM_CREATE_PUBLIC_THREADS |           \
   CCORD_PERM_SEND_MESSAGES_IN_THREADS | CCORD_PERM_CREATE_INSTANT_INVITE | CCORD_PERM_STREAM |                    \
   CCORD_PERM_USE_APPLICATION_COMMANDS | CCORD_PERM_USE_EMBEDDED_ACTIVITIES)

static ccord_id mock_role_id(uint32_t guild_index, int kind)
{
  return MOCK_ROLE_BASE + guild_index * 10u + 1u + (ccord_id)kind;
}

// Who holds which role: in C-Cord HQ you are an admin; in Lua Lounge (owned by Ada) a moderator; in Pixel Garden an
// ordinary member. Ada is that server's admin (its owner), Bjorn moderates everywhere else, the rest are members.
static int mock_role_kind(uint32_t g, uint32_t u)
{
  if (u == 0) return g == 0 ? MOCK_ROLE_ADMINS : (g == 1 ? MOCK_ROLE_MODERATORS : MOCK_ROLE_MEMBERS);
  if (u == 1) return g == 1 ? MOCK_ROLE_ADMINS : MOCK_ROLE_MODERATORS;
  if (u == 2) return MOCK_ROLE_MODERATORS;
  return MOCK_ROLE_MEMBERS;
}

// Rebuilds the public view of who holds which role from the rows (kicked members drop out).
static void mock_sync_member_roles(mock *m)
{
  size_t n = 0;
  for (size_t i = 0; i < sizeof m->mrows / sizeof m->mrows[0]; i++) {
    const mock_mrow *r = &m->mrows[i];
    if (!r->present) continue;
    m->member_roles[n++] = (ccord_member_roles){.guild_id = r->guild_id, .user_id = r->user_id, .role_ids = r->ids,
                                                .count = r->count};
  }
  m->nmember_roles = n;
}

static void build_permissions(mock *m)
{
  size_t nr = 0, nmr = 0;
  m->next_role_id = MOCK_ROLE_BASE + 1000u;
  for (uint32_t g = 0; g < MOCK_NUM_GUILDS; g++) {
    ccord_id gid = m->guilds[g].id;
    m->roles[nr++] = (ccord_role){.id = gid, .guild_id = gid, .name = "@everyone", .permissions = MOCK_EVERYONE_PERMS};
    for (int k = MOCK_ROLE_MEMBERS; k >= MOCK_ROLE_ADMINS; k--) {
      const mock_role_spec *rs = &k_role_specs[k];
      m->roles[nr++] = (ccord_role){.id = mock_role_id(g, k), .guild_id = gid, .name = rs->name, .color = rs->color,
                                    .position = rs->position, .permissions = rs->permissions, .hoist = rs->hoist,
                                    .mentionable = k == MOCK_ROLE_MODERATORS};
    }
    for (uint32_t u = 0; u < MOCK_NUM_USERS; u++) {
      mock_mrow *r = &m->mrows[nmr++];
      bool hq_roleless = (g == 0u && (u == 8u || u == 9u));
      *r = (mock_mrow){.guild_id = gid, .user_id = m->users[u].id, .count = hq_roleless ? 0u : 1u, .present = true};
      if (!hq_roleless) r->ids[0] = mock_role_id(g, mock_role_kind(g, u));
    }
  }
  m->nroles = nr;
  mock_sync_member_roles(m);
  // Four channels whose overwrites matter: announcements are read-only for @everyone in C-Cord HQ (Moderators may
  // still post and start threads); #showcase in Lua Lounge is the same; #critique in Pixel Garden is read-only for
  // everyone but admins. Read-only here means what it does on Discord: no sending and no new threads. #mod-lounge in
  // Pixel Garden is a private channel: @everyone may not even view it, Moderators may (and Admins always can), so the
  // plain member the user is there never sees it listed.
  static const uint64_t k_read_only = CCORD_PERM_SEND_MESSAGES | CCORD_PERM_CREATE_PUBLIC_THREADS |
                                      CCORD_PERM_SEND_MESSAGES_IN_THREADS;
  size_t no = 0;
  struct { ccord_id channel, guild; uint32_t guild_index; uint64_t everyone_deny, mods_allow, everyone_allow; } spec[5] = {
    {3112, 2001, 0, k_read_only, k_read_only, 0},
    {3213, 2002, 1, k_read_only, k_read_only, 0},
    {3313, 2003, 2, k_read_only, 0, 0},
    {3316, 2003, 2, CCORD_PERM_VIEW_CHANNEL, CCORD_PERM_VIEW_CHANNEL, 0},
    // The HQ stage lets everyone ask to speak (Discord grants REQUEST_TO_SPEAK on stage channels).
    {3123, 2001, 0, 0, 0, CCORD_PERM_REQUEST_TO_SPEAK},
  };
  for (size_t i = 0; i < 5; i++) {
    size_t cnt = 0;
    m->overwrite_pool[no][cnt++] = (ccord_overwrite){.id = spec[i].guild, .kind = CCORD_OVERWRITE_ROLE,
                                                     .allow = spec[i].everyone_allow, .deny = spec[i].everyone_deny};
    if (spec[i].mods_allow) {
      m->overwrite_pool[no][cnt++] = (ccord_overwrite){.id = mock_role_id(spec[i].guild_index, MOCK_ROLE_MODERATORS),
                                                       .kind = CCORD_OVERWRITE_ROLE, .allow = spec[i].mods_allow};
    }
    m->overwrites[no] = (ccord_channel_overwrites){.channel_id = spec[i].channel, .items = m->overwrite_pool[no],
                                                   .count = cnt};
    no++;
  }
  m->noverwrites = no;
}

static void mock_audit_push_change(ccord_audit_entry *e, const char *key, const char *old_v, const char *new_v)
{
  if (!e || !key || key[0] == '\0' || e->change_count >= CCORD_AUDIT_MAX_CHANGES) return;
  ccord_audit_change *ch = &e->changes[e->change_count++];
  (void)snprintf(ch->key, sizeof ch->key, "%s", key);
  (void)snprintf(ch->old_val, sizeof ch->old_val, "%s", old_v ? old_v : "");
  (void)snprintf(ch->new_val, sizeof ch->new_val, "%s", new_v ? new_v : "");
}

static void mock_audit_fill_changes(ccord_audit_entry *e, const char *detail)
{
  if (!e || !detail || detail[0] == '\0' || e->change_count > 0) return;
  const char *colon = strstr(detail, ": ");
  if (!colon || colon == detail) return;
  char key[48];
  size_t klen = (size_t)(colon - detail);
  if (klen >= sizeof key) klen = sizeof key - 1u;
  memcpy(key, detail, klen);
  key[klen] = '\0';
  const char *rest = colon + 2;
  const char *arrow = strstr(rest, " -> ");
  if (arrow) {
    char old_v[96];
    size_t olen = (size_t)(arrow - rest);
    if (olen >= sizeof old_v) olen = sizeof old_v - 1u;
    memcpy(old_v, rest, olen);
    old_v[olen] = '\0';
    mock_audit_push_change(e, key, old_v, arrow + 4);
  } else {
    mock_audit_push_change(e, key, "", rest);
  }
}

// A little history so the Bans, Invites and Audit Log pages of C-Cord HQ are not empty on first open.
static void mock_seed_admin(mock *m)
{
  int64_t now = (int64_t)now_of(m);
  const int64_t day = 86400000;
  ccord_id hq = m->guilds[0].id;
  m->bans[m->nbans++] = (mock_ban){hq, {.user_id = 7001, .name = "spam_account", .reason = "Posting scam links"}};
  m->bans[m->nbans++] = (mock_ban){hq, {.user_id = 7002, .name = "troll_99", .reason = ""}};
  m->invites[m->ninvites++] = (mock_invite){hq, {.code = "cCordHQ", .channel_id = 3111, .channel_name = "general",
                                                  .inviter_id = m->users[1].id, .inviter_name = user_names[1],
                                                  .uses = 14, .created_ms = now - 9 * day}};
  m->invites[m->ninvites++] = (mock_invite){hq, {.code = "weekend", .channel_id = 3111, .channel_name = "general",
                                                  .inviter_id = m->self_id ? m->self_id : m->users[0].id,
                                                  .inviter_name = user_names[0], .uses = 3, .max_uses = 10,
                                                  .max_age_s = 604800, .created_ms = now - 2 * day,
                                                  .expires_ms = now + 5 * day}};
  m->next_invite = 1;
  struct { ccord_id actor; uint16_t action; ccord_id target; const char *tname, *reason, *detail; int64_t ago; } seed[] = {
    {m->users[1].id, 31, mock_role_id(0, MOCK_ROLE_MODERATORS), "Moderators", "", "color: 0 -> 3066993", 3 * day},
    {m->users[2].id, 20, 7003, "raid_bot", "Spam in #general", "", 2 * day},
    {m->users[1].id, 10, 3115, "design-forum", "", "name: design-forum", day},
  };
  for (size_t i = 0; i < sizeof seed / sizeof seed[0]; i++) {
    m->last_audit_id = (((ccord_id)(now - seed[i].ago) - MOCK_DISCORD_EPOCH_MS) << 22) | (ccord_id)(i + 1);
    ccord_id actor = seed[i].actor;
    m->audit[m->naudit++] = (mock_audit){hq, {.id = m->last_audit_id, .time_ms = now - seed[i].ago, .user_id = actor,
                                              .user_name = actor == m->users[1].id ? user_names[1] : user_names[2],
                                              .target_id = seed[i].target, .target_name = seed[i].tname,
                                              .action = seed[i].action, .reason = seed[i].reason,
                                              .detail = seed[i].detail}};
    mock_audit_fill_changes(&m->audit[m->naudit - 1].e, seed[i].detail);
  }
  static const char *const k_seed_kw[] = {"free nitro", "*discord-gift*"};
  static const char *const k_seed_allow[] = {"discord.gg"};
  m->automod[m->nautomod++] = (ccord_automod_rule){
    .id = 91001u,
    .guild_id = hq,
    .creator_id = m->users[1].id,
    .name = "Block Scam Links",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = CCORD_AUTOMOD_TRIGGER_KEYWORD,
    .enabled = true,
    .keywords = k_seed_kw,
    .keyword_count = 2u,
    .allow_list = k_seed_allow,
    .allow_count = 1u,
    .action_block = true,
    .custom_message = "Scam links are blocked in C-Cord HQ.",
    .action_alert = true,
    .alert_channel_id = 3113u,
    .exempt_role_count = 1u,
    .exempt_role_ids = {mock_role_id(0, MOCK_ROLE_MODERATORS)},
  };
  m->automod[m->nautomod++] = (ccord_automod_rule){
    .id = 91002u,
    .guild_id = hq,
    .creator_id = m->users[1].id,
    .name = "Anti-Mention Raid",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = CCORD_AUTOMOD_TRIGGER_MENTION_SPAM,
    .enabled = true,
    .mention_total_limit = 5u,
    .mention_raid_protection = true,
    .action_block = true,
    .custom_message = "",
    .action_timeout = true,
    .timeout_seconds = 300u,
  };
  m->webhooks[m->nwebhooks++] = (ccord_webhook){
    .id = 92501u,
    .type = CCORD_WEBHOOK_INCOMING,
    .guild_id = hq,
    .channel_id = 3113u,
    .creator_id = m->users[1].id,
    .creator_name = user_names[1],
    .name = "CI Build Hook",
    .avatar = "",
    .token = "mock_wh_tok_92501",
    .source_guild_name = "",
    .source_channel_name = "",
  };
  m->app_perms[m->napp_perms++] = (ccord_app_cmd_perms){
    .id = m->users[10].id,
    .application_id = m->users[10].id,
    .guild_id = hq,
    .rule_count = 2u,
    .rules = {
      {.id = mock_role_id(0, MOCK_ROLE_MODERATORS), .type = CCORD_APP_PERM_ROLE, .permission = true},
      {.id = hq - 1u, .type = CCORD_APP_PERM_CHANNEL, .permission = true},
    },
  };
  m->app_perms[m->napp_perms++] = (ccord_app_cmd_perms){
    .id = 88001u,
    .application_id = m->users[11].id,
    .guild_id = hq,
    .rule_count = 2u,
    .rules = {
      {.id = 3114u, .type = CCORD_APP_PERM_CHANNEL, .permission = true},
      {.id = m->users[5].id, .type = CCORD_APP_PERM_USER, .permission = false},
    },
  };
  m->welcome[0].guild_id = hq;
  m->welcome[0].present = true;
  m->welcome[0].chs[0] = (ccord_welcome_channel){
    .channel_id = 3111u, .description = "Introduce yourself and chat", .emoji_name = "#"
  };
  m->welcome[0].chs[1] = (ccord_welcome_channel){
    .channel_id = 3113u, .description = "Compiler, protocol & UI patches", .emoji_name = "*"
  };
  m->welcome[0].ws = (ccord_welcome_screen){
    .enabled = true,
    .description = "Welcome to C-Cord HQ — native C23 Discord client development.",
    .channels = m->welcome[0].chs,
    .channel_count = 2u,
  };
  static const ccord_onboarding_option k_seed_ob_opts[] = {
    {.id = 95101u, .title = "Protocol & Net", .description = "Gateway, REST governor & voice", .emoji_name = "*",
     .channel_count = 1u, .channel_ids = {3113u}},
    {.id = 95102u, .title = "Design & UI", .description = "Clay layout, themes & widgets", .emoji_name = "#",
     .channel_count = 1u, .channel_ids = {3115u}},
  };
  static const ccord_onboarding_prompt k_seed_ob_prompts[] = {
    {.id = 95001u, .type = CCORD_ONBOARDING_MULTIPLE_CHOICE, .title = "What areas interest you?",
     .single_select = false, .required = true, .in_onboarding = true,
     .options = k_seed_ob_opts, .option_count = 2u},
  };
  m->onboarding[0].guild_id = hq;
  m->onboarding[0].present = true;
  m->onboarding[0].ob = (ccord_onboarding){
    .guild_id = hq,
    .enabled = true,
    .mode = CCORD_ONBOARDING_DEFAULT,
    .default_channel_count = 2u,
    .default_channel_ids = {3111u, 3112u},
    .prompts = k_seed_ob_prompts,
    .prompt_count = 1u,
  };
}

static ccord_status build_dataset(mock *m, ccord_error *err)
{
  // users
  static const char *const user_bios[MOCK_NUM_USERS] = {
    "Building native C23 Discord experiences.",
    "Systems programmer & protocol tinkerer.",
    "Voice engine maintainer.",
    "UI/UX & theme designer.",
    "Shader & rendering enthusiast.",
    "Audio DSP & Opus hacker.",
    "Security & transport hygiene.",
    "Linux desktop integration.",
    "Plugin runtime author.",
    "Benchmarking & profiling.",
    "Automated Lua helper bot.",
    "Pixel art & sprite bot.",
  };
  static const char *const user_pronouns[MOCK_NUM_USERS] = {
    "they/them", "she/her", "he/him", "he/they", "she/they", "he/him",
    "she/her", "they/them", "she/her", "he/him", "bot", "bot",
  };
  static const char *const user_handles[MOCK_NUM_USERS] = {
    "you", "ada", "bjorn", "chen", "dara", "eli",
    "fatima", "gus", "hana", "ivo", "lua_bot", "pixel_bot",
  };
  static const char *const user_banners[MOCK_NUM_USERS] = {
    "https://cdn.discordapp.com/banners/1001/b0.png", "https://cdn.discordapp.com/banners/1002/b1.png",
    "https://cdn.discordapp.com/banners/1003/b2.png", "https://cdn.discordapp.com/banners/1004/b3.png",
    "https://cdn.discordapp.com/banners/1005/b4.png", "https://cdn.discordapp.com/banners/1006/b5.png",
    "https://cdn.discordapp.com/banners/1007/b6.png", "https://cdn.discordapp.com/banners/1008/b7.png",
    "https://cdn.discordapp.com/banners/1009/b8.png", "https://cdn.discordapp.com/banners/1010/b9.png",
    "https://cdn.discordapp.com/banners/1011/b10.png", "https://cdn.discordapp.com/banners/1012/b11.png",
  };
  static const uint32_t user_accents[MOCK_NUM_USERS] = {
    0x5865F2u, 0x57F287u, 0xFEE75Cu, 0xEB459Eu, 0xED4245u, 0x3BA55Cu,
    0xFAA61Au, 0x9B59B6u, 0xE91E63u, 0x1ABC9Cu, 0x3498DBu, 0xE67E22u,
  };
  ccord_arena_init(&m->automod_arena, 4096u);
  m->next_automod_id = MOCK_AUTOMOD_ID_BASE;
  m->next_webhook_id = MOCK_WEBHOOK_ID_BASE;
  m->next_onboarding_id = MOCK_ONBOARDING_ID_BASE;
  for (uint32_t i = 0; i < MOCK_NUM_USERS; i++) {
    bool bot = i >= 10;
    m->users[i] = (ccord_user){
      .id = 1001 + i, .name = user_names[i], .handle = user_handles[i],
      .status_text = (i % 3 == 0) ? "Listening to lo-fi" : "",
      .avatar_url = user_avatars[i],
      .banner_url = user_banners[i],
      .accent_color = user_accents[i],
      .bio = user_bios[i],
      .pronouns = user_pronouns[i],
      .presence = bot ? CCORD_PRESENCE_ONLINE : (ccord_presence)(1 + rnd(m, 3)), .is_bot = bot,
      .avatar_palette = (uint8_t)(i % 6),
    };
    if (i == 1) {
      (void)snprintf(m->users[i].clan_tag, sizeof m->users[i].clan_tag, "CCRD");
      m->users[i].clan_guild_id = m->guilds[0].id;
      (void)snprintf(m->users[i].clan_badge, sizeof m->users[i].clan_badge, "ccrd_badge");
    } else if (i == 2) {
      (void)snprintf(m->users[i].clan_tag, sizeof m->users[i].clan_tag, "LUA");
      m->users[i].clan_guild_id = m->guilds[0].id;
      (void)snprintf(m->users[i].clan_badge, sizeof m->users[i].clan_badge, "ccrd_badge");
    }
    if (i % 3 == 0) {
      m->users[i].has_activity = true;
      m->users[i].activity_type = CCORD_ACTIVITY_LISTENING;
      (void)snprintf(m->users[i].activity_name, sizeof m->users[i].activity_name, "lo-fi");
      (void)snprintf(m->users[i].activity_details, sizeof m->users[i].activity_details, "Beats to code C23 to");
      (void)snprintf(m->users[i].activity_state, sizeof m->users[i].activity_state, "ChilledCow");
      m->users[i].activity_start_ms = m->cfg.base_time_ms - 185000ll;
    }
  }
  m->users[0].presence = CCORD_PRESENCE_ONLINE;
  m->users[8].presence = CCORD_PRESENCE_OFFLINE;
  m->users[9].presence = CCORD_PRESENCE_OFFLINE;
  m->self_id = m->users[0].id;
  m->notes[0] = (ccord_user_note){.user_id = m->users[1].id, .note = "Core maintainer"};
  m->nnotes = 1;
  m->relationships[0] = (ccord_relationship){.user_id = m->users[1].id, .kind = CCORD_RELATIONSHIP_FRIEND};
  m->relationships[1] = (ccord_relationship){.user_id = m->users[2].id, .kind = CCORD_RELATIONSHIP_FRIEND};
  m->relationships[2] = (ccord_relationship){.user_id = m->users[3].id, .kind = CCORD_RELATIONSHIP_PENDING_INCOMING};
  m->relationships[3] = (ccord_relationship){.user_id = m->users[4].id, .kind = CCORD_RELATIONSHIP_PENDING_OUTGOING};
  m->relationships[4] = (ccord_relationship){.user_id = m->users[5].id, .kind = CCORD_RELATIONSHIP_BLOCKED};
  m->nrelationships = 5;
  // M6 account state: Discord default user settings, two devices (one current) and three linked accounts
  // (one revoked), matching
  m->user_settings = (ccord_user_settings){
    .default_guilds_restricted = false,
    .explicit_content_filter = 0,
    .friend_source_all = true,
    .friend_source_mutual_friends = false,
    .friend_source_mutual_guilds = false,
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
  m->auth_sessions[0] = (ccord_auth_session){
    .id_hash = "5f2a9c1d7e3b4a6f8c0d1e2f3a4b5c6d7e8f9012",
    .os = "Linux", .browser = "Firefox 145", .device = "C-Cord Desktop", .location = "Berlin, Germany",
    .approx_last_used_ms = m->cfg.base_time_ms - 60000ll, .current = true,
  };
  m->auth_sessions[1] = (ccord_auth_session){
    .id_hash = "08b1e4a29d6c4f37a5e0b9c8d7f61234567890ab",
    .os = "Windows", .browser = "Chrome 142", .device = "Work Laptop", .location = "Berlin, Germany",
    .approx_last_used_ms = m->cfg.base_time_ms - 86400000ll, .current = false,
  };
  m->nauth_sessions = 2;
  m->connections[0] = (ccord_connection){
    .id = "76561198000000001", .name = "alex", .type = "steam",
    .revoked = false, .verified = true, .friend_sync = true, .show_activity = true,
    .two_way_link = false, .visibility = 1,
  };
  m->connections[1] = (ccord_connection){
    .id = "alex-gh", .name = "alex", .type = "github",
    .revoked = false, .verified = true, .friend_sync = false, .show_activity = true,
    .two_way_link = true, .visibility = 1,
  };
  m->connections[2] = (ccord_connection){
    .id = "2533274900000001", .name = "Alex", .type = "xbox",
    .revoked = true, .verified = false, .friend_sync = false, .show_activity = false,
    .two_way_link = false, .visibility = 0,
  };
  m->nconnections = 3;
  for (uint32_t g = 0; g < MOCK_NUM_GUILDS; g++) {
    m->guilds[g] = (ccord_guild){.id = 2001 + g, .name = guild_names[g], .icon_url = guild_icons[g],
                                 .afk_timeout = 300u, .has_guild_settings = true};
  }
  m->nguilds = MOCK_NUM_GUILDS;
  m->guilds[0].system_channel_id = 3111;
  m->guilds[0].verification_level = 1u;
  m->guilds[0].explicit_content_filter = 1u;
  m->guilds[0].premium_tier = 3u;
  m->guilds[0].premium_subscription_count = 14u;
  m->guilds[0].feature_community = true;
  m->guilds[0].rules_channel_id = 3112u;
  m->guilds[0].public_updates_channel_id = 3113u;
  m->guilds[0].feature_vanity_url = true;
  m->guilds[0].feature_banner = true;
  (void)snprintf(m->guilds[0].vanity_url_code, sizeof m->guilds[0].vanity_url_code, "ccord");
  m->guilds[1].owner_id = m->users[1].id;   // "Lua Lounge" belongs to someone else (Ada); the others have no known owner

  // channels (display order: category, its text channels, category, its voice channels, its stage)
  size_t cap = 8; // extra capacity for seeded forum thread channels
  for (uint32_t g = 0; g < MOCK_NUM_GUILDS; g++) cap += 2 + text_counts[g] + 3;
  m->channels = ccord_alloc_array(cap, sizeof *m->channels);
  m->chmsgs = ccord_calloc(cap, sizeof *m->chmsgs);
  m->chn = ccord_calloc(cap, sizeof *m->chn);
  m->chcap = ccord_calloc(cap, sizeof *m->chcap);
  m->members = ccord_alloc_array(MOCK_NUM_GUILDS * MOCK_NUM_USERS, sizeof *m->members);
  if (!m->channels || !m->chmsgs || !m->chn || !m->chcap || !m->members) {
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  size_t n = 0;
  for (uint32_t g = 0; g < MOCK_NUM_GUILDS; g++) {
    ccord_id gid = m->guilds[g].id;
    ccord_id cat_text = 3000 + (g + 1) * 100 + 10;
    ccord_id cat_voice = 3000 + (g + 1) * 100 + 20;
    m->channels[n++] = (ccord_channel){.id = cat_text, .guild_id = gid, .parent_id = 0,
                                       .kind = CCORD_CHANNEL_CATEGORY, .discord_type = CCORD_DISCORD_CHANNEL_GUILD_CATEGORY,
                                       .name = "TEXT CHANNELS", .topic = ""};
    for (size_t j = 0; j < text_counts[g]; j++) {
      bool is_forum = (g == 0 && j == 4);
      bool is_announcement = (g == 0 && j == 1);
      uint32_t slowmode_s = ((g == 0 && j == 3) || (g == 2 && j == 4)) ? 5u : 0u; // #off-topic in C-Cord HQ & Pixel Garden
      m->channels[n++] = (ccord_channel){.id = cat_text + 1 + j, .guild_id = gid, .parent_id = cat_text,
                                         .kind = CCORD_CHANNEL_TEXT,
                                         .discord_type = is_forum ? CCORD_DISCORD_CHANNEL_GUILD_FORUM
                                                                  : (is_announcement ? CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT : 0),
                                         .name = text_names[g][j].name,
                                         .topic = is_forum ? "UI/UX proposals, architecture RFCs, and bug reports — tag your posts!"
                                                           : ((j == 0) ? "Welcome" : ""),
                                         .rate_limit_per_user = slowmode_s,
                                         .default_auto_archive_duration = 1440u};
    }
    m->channels[n++] = (ccord_channel){.id = cat_voice, .guild_id = gid, .parent_id = 0,
                                       .kind = CCORD_CHANNEL_CATEGORY, .discord_type = CCORD_DISCORD_CHANNEL_GUILD_CATEGORY,
                                       .name = "VOICE CHANNELS", .topic = ""};
    static const char *const voice[] = {"General", "Gaming"};
    for (size_t j = 0; j < 2; j++) {
      m->channels[n++] = (ccord_channel){.id = cat_voice + 1 + j, .guild_id = gid, .parent_id = cat_voice,
                                         .kind = CCORD_CHANNEL_VOICE, .discord_type = CCORD_DISCORD_CHANNEL_GUILD_VOICE,
                                         .name = voice[j], .topic = "", .bitrate = 64000u, .user_limit = 0u};
    }
    m->channels[n++] = (ccord_channel){.id = cat_voice + 3, .guild_id = gid, .parent_id = cat_voice,
                                       .kind = CCORD_CHANNEL_STAGE, .discord_type = CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE,
                                       .name = "Stage", .topic = "", .bitrate = 64000u, .user_limit = 0u};
  }
  size_t snap_nchannels = n;
  m->nchannels = n;

  // members: every user in every guild, each holding one role (see mock_role_kind for who gets which)
  size_t nm = 0;
  int64_t join_ref = (int64_t)now_of(m) >= (int64_t)MOCK_DISCORD_EPOCH_MS ? (int64_t)now_of(m) : m->cfg.base_time_ms;
  for (uint32_t g = 0; g < MOCK_NUM_GUILDS; g++) {
    for (uint32_t u = 0; u < MOCK_NUM_USERS; u++) {
      const mock_role_spec *rs = &k_role_specs[mock_role_kind(g, u)];
      int64_t days_ago = (u == 8u) ? 35ll : ((u == 9u) ? 10ll : (int64_t)(42u - u * 3u - g));
      m->members[nm++] = (ccord_member){.guild_id = m->guilds[g].id, .user_id = m->users[u].id,
                                        .role_name = rs->name, .role_color = rs->color, .role_rank = rs->rank,
                                        .joined_at_ms = join_ref - days_ago * 86400000ll};
    }
  }
  m->nmembers = nm;
  build_permissions(m);
  mock_seed_admin(m);

  // One seeded voice participant (with active stream) so the UI always has a populated voice channel,
  // plus a stage speaker and a suppressed audience member with a pending request-to-speak on the HQ stage.
  m->voices[0] = (ccord_voice_state){.guild_id = m->guilds[0].id, .channel_id = 3121, .user_id = m->users[1].id,
                                     .self_mute = false, .self_deaf = false, .self_video = false,
                                     .self_stream = true};
  m->voices[1] = (ccord_voice_state){.guild_id = m->guilds[0].id, .channel_id = 3123, .user_id = m->users[2].id,
                                     .suppress = false};
  m->voices[2] = (ccord_voice_state){.guild_id = m->guilds[0].id, .channel_id = 3123, .user_id = m->users[3].id,
                                     .suppress = true,
                                     .request_to_speak_timestamp = m->cfg.base_time_ms - 60000ll};
  m->nvoices = 3;

  // A live stage instance per stage channel; they ride STAGE_INSTANCE_UPDATE events after the snapshot
  // (the snapshot itself has no stage rows).
  m->next_stage_id = 96001u;
  m->stages[0] = (ccord_stage_instance){.id = m->next_stage_id++, .channel_id = 3123, .guild_id = 2001,
                                        .topic = "Community Town Hall", .privacy_level = 2,
                                        .guild_scheduled_event_id = MOCK_EVENT_ID_BASE};
  m->stages[1] = (ccord_stage_instance){.id = m->next_stage_id++, .channel_id = 3223, .guild_id = 2002,
                                        .topic = "Lua Lounge Live", .privacy_level = 2};
  m->stages[2] = (ccord_stage_instance){.id = m->next_stage_id++, .channel_id = 3323, .guild_id = 2003,
                                        .topic = "Pixel Garden Stage", .privacy_level = 2};
  m->nstages = 3;

  // Guild scheduled events (REST rows, not snapshot data): one stage event and one external event in C-Cord HQ,
  // one voice event in Lua Lounge. They give EVENT_LIST / RSVP / MODIFY / DELETE something to work on.
  int64_t ev_base = m->cfg.base_time_ms ? m->cfg.base_time_ms : MOCK_DEFAULT_BASE_MS;
  m->next_event_id = MOCK_EVENT_ID_BASE;
  m->events[0] = (ccord_scheduled_event){
    .id = m->next_event_id++, .guild_id = 2001, .channel_id = 3123, .creator_id = m->users[1].id,
    .name = "Community Town Hall", .description = "Monthly community Q&A on the HQ stage.",
    .scheduled_start_ms = ev_base + 86400000ll, .privacy_level = 2,
    .status = CCORD_SCHEDULED_EVENT_SCHEDULED, .entity_type = CCORD_SCHEDULED_EVENT_STAGE_INSTANCE,
    .user_count = 12,
  };
  m->events[1] = (ccord_scheduled_event){
    .id = m->next_event_id++, .guild_id = 2001, .creator_id = m->users[2].id,
    .name = "Pixel Art Meetup", .description = "Bring your latest sprites.", .location = "Online",
    .scheduled_start_ms = ev_base + 2 * 86400000ll, .scheduled_end_ms = ev_base + 2 * 86400000ll + 7200000ll,
    .privacy_level = 2, .status = CCORD_SCHEDULED_EVENT_SCHEDULED, .entity_type = CCORD_SCHEDULED_EVENT_EXTERNAL,
    .user_count = 5,
  };
  m->events[2] = (ccord_scheduled_event){
    .id = m->next_event_id++, .guild_id = 2002, .channel_id = 3221, .creator_id = m->users[1].id,
    .name = "Lua Lounge Listening Party", .scheduled_start_ms = ev_base + 3 * 86400000ll,
    .privacy_level = 2, .status = CCORD_SCHEDULED_EVENT_SCHEDULED, .entity_type = CCORD_SCHEDULED_EVENT_VOICE,
    .user_count = 3,
  };
  m->nevents = 3;

  // messages: text channels only; server 1 #general is the stress channel
  uint64_t ids = MOCK_ID_BASE;
  for (size_t ci = 0; ci < m->nchannels; ci++) {
    const ccord_channel *ch = &m->channels[ci];
    if (ch->kind != CCORD_CHANNEL_TEXT || ccord_discord_type_is_forum(ch->discord_type)) continue;
    size_t count;
    if (ch->id == MOCK_GENERAL_ID) {
      count = m->cfg.big_channel_messages;
    } else if (strcmp(ch->name, "empty") == 0) {
      count = 0;
    } else {
      count = 20 + rnd(m, 181);
    }
    ccord_id prev_author = m->users[0].id;
    for (size_t i = 0; i < count; i++) {
      if (rnd(m, 100) >= 35) prev_author = m->users[rnd(m, 10)].id; // bursts keep the previous author
      char *text = make_content(m, i);
      if (!text) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      ccord_message msg = {
        .id = ++ids, .channel_id = ch->id, .author_id = prev_author,
        .timestamp_ms = m->cfg.base_time_ms - (int64_t)(count - 1 - i) * MOCK_MSG_STEP_MS,
        .content = text,
      };
      ccord_status ast = chan_append(m, ci, msg, err);
      if (ast != CCORD_OK) return ast;
    }
  }

  // Seed forum metadata & 5 rich forum posts for #design-forum (id=3115) in C-Cord HQ (guild=2001).
  m->forum_metas[0] = (ccord_forum_meta){
    .forum_id = 3115,
    .default_sort = CCORD_FORUM_SORT_ACTIVITY,
    .tag_count = 5,
    .tags = {
      {.id = 4001, .name = "RFC", .emoji_name = "\xf0\x9f\x93\x90", .moderated = false},
      {.id = 4002, .name = "Bug", .emoji_name = "\xf0\x9f\x90\x9b", .moderated = false},
      {.id = 4003, .name = "UI/UX", .emoji_name = "\xf0\x9f\x8e\xa8", .moderated = false},
      {.id = 4004, .name = "Performance", .emoji_name = "\xe2\x9a\xa1", .moderated = false},
      {.id = 4005, .name = "Resolved", .emoji_name = "\xe2\x9c\x85", .moderated = true},
    },
  };
  m->nforum_metas = 1u;

  m->capforum_posts = 16;
  m->forum_posts = ccord_calloc(m->capforum_posts, sizeof *m->forum_posts);
  if (!m->forum_posts) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");

  typedef struct seed_forum_post {
    ccord_id thread_id;
    ccord_id owner_id;
    const char *title;
    bool pinned;
    ccord_id tags[2];
    uint8_t tag_count;
    uint32_t reactions;
    const char *emoji;
    bool self_reacted;
    int64_t created_offset_ms;
    int64_t active_offset_ms;
    ccord_id msg_authors[4];
    const char *msgs[4];
    size_t msg_count;
  } seed_forum_post;

  static const seed_forum_post k_seed_posts[5] = {
    {
      .thread_id = 3151, .owner_id = 1002,
      .title = "Welcome & Forum Guidelines \xe2\x80\x94 Read Before Posting",
      .pinned = true, .tags = {4001, 4003}, .tag_count = 2,
      .reactions = 14, .emoji = "\xe2\x9d\xa4\xef\xb8\x8f", .self_reacted = true,
      .created_offset_ms = 7200000ll, .active_offset_ms = 600000ll,
      .msg_authors = {1002, 1003, 1004},
      .msgs = {
        "Welcome to #design-forum! Use tags to categorize RFCs, UI polish ideas, and bug reports.",
        "Pinned! Please include reproduction steps or screenshots when tagging with Bug or UI/UX.",
        "Love the new card layout and tag pills \xe2\x80\x94 super easy to scan active discussions.",
      },
      .msg_count = 3,
    },
    {
      .thread_id = 3152, .owner_id = 1004,
      .title = "RFC: Native C23 Forum Channel View & Tag Filtering",
      .pinned = false, .tags = {4001, 4003}, .tag_count = 2,
      .reactions = 9, .emoji = "\xf0\x9f\x94\xa5", .self_reacted = true,
      .created_offset_ms = 5400000ll, .active_offset_ms = 120000ll,
      .msg_authors = {1004, 1005, 1001, 1002},
      .msgs = {
        "Proposal for rendering Discord guild_forum (type 15) channels with card list, tag filter pills, and inline composer.",
        "Can we support multi-tag AND filtering and instant client-side title search?",
        "Yes! Clicking any post card opens the underlying thread with a header breadcrumb back to the forum.",
        "Ship it! Let's keep unopened forum threads hidden from the channel sidebar to avoid clutter.",
      },
      .msg_count = 4,
    },
    {
      .thread_id = 3153, .owner_id = 1005,
      .title = "Vulkan & SDL_Renderer glyph atlas caching under 4K HiDPI",
      .pinned = false, .tags = {4004, 4003}, .tag_count = 2,
      .reactions = 7, .emoji = "\xe2\x9a\xa1", .self_reacted = false,
      .created_offset_ms = 10800000ll, .active_offset_ms = 300000ll,
      .msg_authors = {1005, 1010, 1005},
      .msgs = {
        "Measured 0.18ms frame times after pre-warming the shelf packer with common emoji and UI icons.",
        "Benchmarks look solid on both X11 and Wayland fractional scaling.",
        "Updated the atlas eviction policy so color Twemoji glyphs never thrash during fast scrolling.",
      },
      .msg_count = 3,
    },
    {
      .thread_id = 3154, .owner_id = 1003,
      .title = "Fixed: Voice gateway UDP IP discovery timeout on dual-stack NAT",
      .pinned = false, .tags = {4002, 4005}, .tag_count = 2,
      .reactions = 11, .emoji = "\xe2\x9c\x85", .self_reacted = true,
      .created_offset_ms = 18000000ll, .active_offset_ms = 900000ll,
      .msg_authors = {1003, 1006, 1003},
      .msgs = {
        "Deferred Opcode 1 SELECT_PROTOCOL until the 74-byte 0x0002 UDP discovery response arrives.",
        "Confirmed this fixes the silent audio drop when switching between voice channels rapidly.",
        "Marked as Resolved \xe2\x80\x94 regression test added to test_live_transport.",
      },
      .msg_count = 3,
    },
    {
      .thread_id = 3155, .owner_id = 1009,
      .title = "Lua 5.5 sandboxed plugin hooks for custom slash commands",
      .pinned = false, .tags = {4001, 0}, .tag_count = 1,
      .reactions = 5, .emoji = "\xe2\x9d\xa4\xef\xb8\x8f", .self_reacted = false,
      .created_offset_ms = 25200000ll, .active_offset_ms = 1800000ll,
      .msg_authors = {1009, 1011},
      .msgs = {
        "Designing a zero-allocation IPC bridge so Lua plugins can register custom slash commands safely.",
        "Beep boop! Ready to test custom Lua slash commands in the sandbox.",
      },
      .msg_count = 2,
    },
  };

  for (size_t p = 0; p < 5; p++) {
    const seed_forum_post *sp = &k_seed_posts[p];
    size_t tci = m->nchannels++;
    m->channels[tci] = (ccord_channel){
      .id = sp->thread_id,
      .guild_id = 2001,
      .parent_id = 3115,
      .kind = CCORD_CHANNEL_TEXT,
      .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD,
      .name = sp->title,
      .topic = "",
    };
    ccord_id first_mid = CCORD_ID_NONE;
    ccord_id last_mid = CCORD_ID_NONE;
    for (size_t k = 0; k < sp->msg_count; k++) {
      char *copy = ccord_strndup(sp->msgs[k], strlen(sp->msgs[k]));
      if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      int64_t ts = m->cfg.base_time_ms - sp->created_offset_ms + (int64_t)k * 60000ll;
      if (k + 1 == sp->msg_count) ts = m->cfg.base_time_ms - sp->active_offset_ms;
      ccord_message msg = {
        .id = ++ids,
        .channel_id = sp->thread_id,
        .author_id = sp->msg_authors[k],
        .timestamp_ms = ts,
        .content = copy,
        .reaction_count = (k == 0) ? sp->reactions : 0,
        .self_reacted = (k == 0) ? sp->self_reacted : false,
      };
      ccord_status ast = chan_append(m, tci, msg, err);
      if (ast != CCORD_OK) return ast;
      if (k == 0) first_mid = msg.id;
      last_mid = msg.id;
    }
    ccord_forum_post fp = {
      .thread_id = sp->thread_id,
      .forum_id = 3115,
      .guild_id = 2001,
      .owner_id = sp->owner_id,
      .title = sp->title,
      .applied_tag_count = sp->tag_count,
      .message_count = (uint32_t)(sp->msg_count > 0 ? sp->msg_count - 1 : 0),
      .create_ts_ms = m->cfg.base_time_ms - sp->created_offset_ms,
      .last_activity_ms = m->cfg.base_time_ms - sp->active_offset_ms,
      .last_message_id = last_mid,
      .pinned = sp->pinned,
      .first_message_id = first_mid,
      .first_author_id = sp->owner_id,
      .first_content = sp->msgs[0],
      .thumbnail_url = "",
      .reaction_emoji = sp->emoji,
      .reaction_count = sp->reactions,
    };
    for (uint8_t t = 0; t < sp->tag_count; t++) fp.applied_tags[t] = sp->tags[t];
    m->forum_posts[m->nforum_posts++] = fp;
  }
  m->next_thread_id = 3156;
  m->next_msg_id = ids + 1;

  m->snap = (ccord_snapshot){
    .self_id = m->self_id, .users = m->users, .user_count = MOCK_NUM_USERS,
    .guilds = m->guilds, .guild_count = m->nguilds, .channels = m->channels, .channel_count = snap_nchannels,
    .members = m->members, .member_count = m->nmembers,
    .voice_states = m->voices, .voice_state_count = m->nvoices,
    .relationships = m->relationships, .relationship_count = m->nrelationships,
    .notes = m->notes, .note_count = m->nnotes,
  };
  return CCORD_OK;
}

static void free_items(mock_item *items, size_t n)
{
  for (size_t i = 0; i < n; i++) item_release(&items[i]);
}

static void dataset_free(mock *m)
{
  for (size_t ci = 0; ci < m->nchannels; ci++) {
    for (size_t k = 0; m->chmsgs && k < m->chn[ci]; k++) {
      free_content(&m->chmsgs[ci][k].content);
      free_rich(&m->chmsgs[ci][k].rich);
    }
  }
  for (size_t ci = 0; m->chmsgs && ci < m->nchannels; ci++) ccord_free(m->chmsgs[ci]);
  ccord_free(m->chmsgs);
  ccord_free(m->chn);
  ccord_free(m->chcap);
  ccord_free(m->channels);
  ccord_free(m->members);
  ccord_free(m->forum_posts);
  for (size_t i = 0; i < m->ngarbage; i++) free_content(&m->garbage[i]);
  ccord_free(m->garbage);
  for (size_t i = 0; i < m->nrgarbage; i++) ccord_message_rich_free(m->rgarbage[i]);
  ccord_free(m->rgarbage);
  m->rgarbage = nullptr;
  m->nrgarbage = m->caprgarbage = 0;
  ccord_free(m->settings_proto);
  ccord_free(m->status1);
  for (size_t i = 0; i < m->nstatus1_patches; i++) {
    ccord_free(m->status1_patches[i].raw);
    ccord_free(m->status1_patches[i].body);
  }
  ccord_free(m->status1_patches);
  ccord_arena_free(&m->automod_arena);
}

// ---- queue ----

static ccord_status push_item(mock *m, mock_item item, ccord_error *err)
{
  if (m->nq == m->capq) {
    size_t ncap = m->capq ? m->capq * 2 : 16;
    mock_item *g = ccord_realloc_array(m->queue, ncap, sizeof *g);
    if (!g) {
      item_release(&item);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    m->queue = g;
    m->capq = ncap;
  }
  m->queue[m->nq++] = item;
  return CCORD_OK;
}

static mock_item make_item(ccord_net_event_type type, uint64_t due)
{
  mock_item it = {.ev = {.type = type}, .msgs = nullptr, .due = due};
  return it;
}

static size_t channel_index(const mock *m, ccord_id channel_id, bool text_only, ccord_status *st, ccord_error *err)
{
  for (size_t i = 0; i < m->nchannels; i++) {
    if (m->channels[i].id == channel_id) {
      // "text_only" means "has a chat": text channels plus a voice/stage channel's own chat.
      ccord_channel_kind k = m->channels[i].kind;
      if (text_only && k != CCORD_CHANNEL_TEXT && k != CCORD_CHANNEL_VOICE && k != CCORD_CHANNEL_STAGE) {
        *st = ccord_error_set(err, CCORD_ERR_INVALID, "mock: channel has no chat");
        return SIZE_MAX;
      }
      *st = CCORD_OK;
      return i;
    }
  }
  *st = ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown channel");
  return SIZE_MAX;
}

// Appends a message to the dataset (content copied) and returns the stored struct. `rich` (may be null) is copied too; the
// dataset owns the copy and queued events share it.
static ccord_message store_new_rich(mock *m, size_t ci, ccord_id author, const char *text, size_t len,
                                    const ccord_message_rich *rich, ccord_error *err, ccord_status *st)
{
  ccord_message msg = {.id = m->next_msg_id, .channel_id = m->channels[ci].id, .author_id = author, .content = nullptr};
  msg.timestamp_ms = (m->cfg.base_time_ms ? m->cfg.base_time_ms : MOCK_DEFAULT_BASE_MS) +
                     (int64_t)(now_of(m) - m->start_ms);
  msg.content = ccord_strndup(text, len);
  if (!msg.content) {
    *st = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    return msg;
  }
  if (rich) {
    msg.rich = ccord_message_rich_clone(rich, err);
    if (!msg.rich) {
      free_content(&msg.content);
      *st = err && err->code != CCORD_OK ? err->code : CCORD_ERR_NOMEM;
      return msg;
    }
  }
  if (ccord_message_is_ephemeral(&msg)) {
    retire_content(m, msg.content);
    retire_rich(m, msg.rich);
    *st = CCORD_OK;
    m->next_msg_id++;
    return msg;
  }
  *st = chan_append(m, ci, msg, err);
  if (*st != CCORD_OK) return msg; // chan_append already freed the content
  m->next_msg_id++;
  return msg;
}

static ccord_message store_new(mock *m, size_t ci, ccord_id author, const char *text, size_t len, ccord_error *err, ccord_status *st)
{
  return store_new_rich(m, ci, author, text, len, nullptr, err, st);
}

static ccord_status queue_message_create(mock *m, ccord_message msg, uint64_t due, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_MESSAGE_CREATE, due);
  it.msgs = ccord_alloc_array(1, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  it.msgs[0] = msg;
  it.ev.messages = it.msgs;
  it.ev.message_count = 1;
  it.ev.channel_id = msg.channel_id;
  it.ev.guild_id = CCORD_ID_NONE;
  return push_item(m, it, err);
}

static ccord_status queue_message_update(mock *m, ccord_message msg, uint64_t due, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_MESSAGE_UPDATE, due);
  it.msgs = ccord_alloc_array(1, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  it.msgs[0] = msg;
  it.ev.messages = it.msgs;
  it.ev.message_count = 1;
  it.ev.channel_id = msg.channel_id;
  return push_item(m, it, err);
}

static int64_t message_time(const mock *m)
{
  return (m->cfg.base_time_ms ? m->cfg.base_time_ms : MOCK_DEFAULT_BASE_MS) +
         (int64_t)(now_of(m) - m->start_ms);
}

static long find_message(const mock *m, size_t ci, ccord_id message_id)
{
  for (size_t i = 0; i < m->chn[ci]; i++) {
    if (m->chmsgs[ci][i].id == message_id) return (long)i;
  }
  return -1;
}

static bool mock_guild_known(const mock *m, ccord_id guild_id)
{
  for (size_t i = 0; i < m->nguilds; i++) if (m->guilds[i].id == guild_id) return true;
  return false;
}

static long mock_voice_index(const mock *m, ccord_id guild_id, ccord_id user_id)
{
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].guild_id == guild_id && m->voices[i].user_id == user_id) return (long)i;
  }
  return -1;
}

static ccord_status queue_activity_instance(mock *m, const ccord_activity_instance *inst, uint64_t due,
                                            ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_ACTIVITY_INSTANCE_UPDATE, due);
  it.ev.guild_id = inst->guild_id;
  it.ev.channel_id = inst->channel_id;
  it.ev.activity_instance = *inst;
  return push_item(m, it, err);
}

static ccord_status queue_voice_state(mock *m, const ccord_voice_state *v, uint64_t due, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_VOICE_STATE_UPDATE, due);
  it.vstate = ccord_alloc_array(1, sizeof *it.vstate);
  if (!it.vstate) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.vstate = *v;
  it.ev.voice_state = it.vstate;
  it.ev.guild_id = v->guild_id;
  it.ev.channel_id = v->channel_id;
  it.ev.user_id = v->user_id;
  return push_item(m, it, err);
}

static ccord_status queue_stream_state(mock *m, const ccord_stream_state *st, uint64_t due, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_STREAM_UPDATE, due);
  it.stream = ccord_alloc_array(1, sizeof *it.stream);
  if (!it.stream) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.stream = *st;
  it.ev.stream = it.stream;
  it.ev.guild_id = st->guild_id;
  it.ev.channel_id = st->channel_id;
  it.ev.user_id = st->user_id;
  return push_item(m, it, err);
}

// ---- M5 voice & video simulation ------------------------------------
// The mock plays the server: calls ring passively, stage instances follow the channel's occupancy,
// and every answer is a typed event. Nothing here touches a ccord_store; the session applies events.

// ---- Remote voice cameras --------------------------------------
// The mock carries the bytes the caller pushes; it applies the gateway's admission rule: first come, first served,
// CCORD_CAM_DECODE_MAX decoding, a freed slot goes to the earliest waiter (the mock has no idle demotion, so no
// just-demoted sender competes with it).
static void mock_cam_free_fifo(mock *m, size_t i)
{
  for (size_t k = 0; k < m->cams[i].count; k++) {
    ccord_free(m->cams[i].fifo[(m->cams[i].head + k) % MOCK_CAM_FIFO].data);
  }
  m->cams[i].head = 0;
  m->cams[i].count = 0;
  memset(m->cams[i].fifo, 0, sizeof m->cams[i].fifo);
}

static void mock_cam_clear(mock *m)
{
  for (size_t i = 0; i < m->ncams; i++) mock_cam_free_fifo(m, i);
  memset(m->cams, 0, sizeof m->cams);
  m->ncams = 0;
}

static void mock_cam_remove_at(mock *m, size_t i)
{
  mock_cam_free_fifo(m, i);
  memmove(&m->cams[i], &m->cams[i + 1], (m->ncams - i - 1u) * sizeof m->cams[0]);
  m->ncams--;
  memset(&m->cams[m->ncams], 0, sizeof m->cams[0]);
}

static long mock_cam_find(const mock *m, ccord_id user_id)
{
  for (size_t i = 0; i < m->ncams; i++) if (m->cams[i].user_id == user_id) return (long)i;
  return -1;
}

// Is the sender at `i` in the call self is in, with its camera on?
static bool mock_cam_listed(const mock *m, size_t i)
{
  const ccord_voice_state *self = nullptr;
  for (size_t v = 0; v < m->nvoices; v++) {
    if (m->voices[v].user_id == m->self_id && m->voices[v].channel_id != CCORD_ID_NONE) { self = &m->voices[v]; break; }
  }
  if (!self) return false;
  for (size_t v = 0; v < m->nvoices; v++) {
    if (m->voices[v].user_id == m->cams[i].user_id && m->voices[v].guild_id == self->guild_id &&
        m->voices[v].channel_id == self->channel_id && m->voices[v].self_video) return true;
  }
  return false;
}

// Re-applies the admission rule. A promotion gets a fresh generation, so a consumer reopens its decoder.
static void mock_cam_rebalance(mock *m)
{
  size_t decoding = 0;
  for (size_t i = 0; i < m->ncams; i++) {
    if (m->cams[i].decoding && !mock_cam_listed(m, i)) {
      m->cams[i].decoding = false;
      mock_cam_free_fifo(m, i);
    }
    if (m->cams[i].decoding) decoding++;
  }
  for (size_t i = 0; i < m->ncams && decoding < CCORD_CAM_DECODE_MAX; i++) {
    if (!m->cams[i].decoding && mock_cam_listed(m, i)) {
      m->cams[i].decoding = true;
      m->cams[i].gen = ++m->cam_gen;
      decoding++;
    }
  }
}

static bool mock_cam_register(mock *m, ccord_id user_id)
{
  if (mock_cam_find(m, user_id) >= 0) return true;
  if (m->ncams >= MOCK_NUM_USERS) return false;
  m->cams[m->ncams].user_id = user_id;
  m->cams[m->ncams].gen = ++m->cam_gen;
  m->ncams++;
  return true;
}

// Self joined a call: it behaves like a fresh OP 12 burst, so every remote self_video user in voice-slot order is
// registered anew.
static void mock_cam_on_join(mock *m)
{
  mock_cam_clear(m);
  for (size_t v = 0; v < m->nvoices; v++) {
    if (m->voices[v].user_id != m->self_id && m->voices[v].self_video && m->voices[v].channel_id != CCORD_ID_NONE) {
      (void)mock_cam_register(m, m->voices[v].user_id);
    }
  }
  mock_cam_rebalance(m);
}

static size_t mock_camera_senders(ccord_transport *t, ccord_camera_rx *out, size_t max)
{
  mock *m = (mock *)t;
  if (!m || !out || max == 0) return 0;
  mock_cam_rebalance(m);
  size_t n = 0;
  for (size_t i = 0; i < m->ncams && n < max && n < CCORD_CAM_RX_MAX; i++) {
    if (!mock_cam_listed(m, i)) continue;
    out[n++] = (ccord_camera_rx){.user_id = m->cams[i].user_id, .generation = m->cams[i].gen,
                                 .decoding = m->cams[i].decoding};
  }
  return n;
}

// Oldest queued unit of a decoding sender. Oversized heads are dropped so one cannot stall the ones behind it.
static bool mock_poll_camera_frame(ccord_transport *t, ccord_id user_id, uint8_t *buf, size_t buf_cap, size_t *out_len,
                                   uint32_t *out_ts90k, bool *out_keyframe)
{
  mock *m = (mock *)t;
  if (!m || !buf || !out_len) return false;
  mock_cam_rebalance(m);
  long ci = mock_cam_find(m, user_id);
  if (ci < 0 || !m->cams[ci].decoding) return false;
  typeof(m->cams[0]) *c = &m->cams[ci];
  while (c->count > 0 && c->fifo[c->head].len > buf_cap) {
    ccord_free(c->fifo[c->head].data);
    c->fifo[c->head].data = nullptr;
    c->head = (c->head + 1u) % MOCK_CAM_FIFO;
    c->count--;
  }
  if (c->count == 0) return false;
  memcpy(buf, c->fifo[c->head].data, c->fifo[c->head].len);
  *out_len = c->fifo[c->head].len;
  if (out_ts90k) *out_ts90k = c->fifo[c->head].ts90k;
  if (out_keyframe) *out_keyframe = c->fifo[c->head].keyframe;
  ccord_free(c->fifo[c->head].data);
  c->fifo[c->head].data = nullptr;
  c->head = (c->head + 1u) % MOCK_CAM_FIFO;
  c->count--;
  return true;
}

// The simulated voice connection's DAVE/RTC snapshots. E2EE is active but never "verified" under the
// mock (there is no real MLS handshake to verify); leaving voice zeroes both, like a closed connection.
static void mock_voice_connect(mock *m, ccord_id channel_id)
{
  m->rtc = (ccord_rtc_stats){.state = CCORD_VOICE_ESTABLISHED, .ssrc = (uint32_t)channel_id + 1000u,
                             .ping_ms = 23, .jitter_depth = 0, .packets_lost = 0,
                             .bitrate_kbps = 64, .dave_epoch = 1};
  m->dave = (ccord_dave_status){.active = true, .verified = false, .protocol_version = 1};
  mock_cam_on_join(m);
}

static void mock_voice_disconnect(mock *m)
{
  memset(&m->rtc, 0, sizeof m->rtc);
  memset(&m->dave, 0, sizeof m->dave);
  mock_cam_clear(m);   // the remote voice states stay (those users are still in the room); our receive side is gone
}

// Drops every queued Go Live access unit and PCM sample. Called when the broadcast or voice connection ends.
static void mock_stream_media_clear(mock *m)
{
  for (size_t i = 0; i < m->stream_frame_count; i++) {
    ccord_free(m->stream_frames[(m->stream_frame_head + i) % MOCK_STREAM_FRAMES_MAX].data);
  }
  memset(m->stream_frames, 0, sizeof m->stream_frames);
  m->stream_frame_head = 0;
  m->stream_frame_count = 0;
  m->stream_pcm_head = 0;
  m->stream_pcm_count = 0;
}

// The sender queues one H.264 Annex-B access unit for an active broadcast (bound to the 8-slot FIFO).
static ccord_status mock_send_stream_frame(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k,
                                           bool keyframe, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty stream frame");
  if (!m->has_stream || !m->stream.active) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: no active Go Live stream");
  }
  if (len > MOCK_STREAM_FRAME_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: stream frame too large");
  }
  if (m->stream_frame_count >= MOCK_STREAM_FRAMES_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: stream frame queue full");
  }
  uint8_t *copy = ccord_alloc_array(len, sizeof *copy);
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  memcpy(copy, annexb, len);
  size_t idx = (m->stream_frame_head + m->stream_frame_count) % MOCK_STREAM_FRAMES_MAX;
  m->stream_frames[idx].data = copy;
  m->stream_frames[idx].len = len;
  m->stream_frames[idx].ts90k = ts90k;
  m->stream_frames[idx].keyframe = keyframe;
  m->stream_frame_count++;
  return CCORD_OK;
}

// Oldest access unit first. A head unit larger than buf_cap is dropped (with any further oversized heads) so one
// oversized frame cannot stall every later one behind it.
static bool mock_poll_stream_frame(ccord_transport *t, uint8_t *buf, size_t buf_cap, size_t *out_len,
                                   uint32_t *out_ts90k, bool *out_keyframe)
{
  mock *m = (mock *)t;
  if (!m || !buf || !out_len) return false;
  while (m->stream_frame_count > 0 && m->stream_frames[m->stream_frame_head].len > buf_cap) {
    ccord_free(m->stream_frames[m->stream_frame_head].data);
    m->stream_frames[m->stream_frame_head].data = nullptr;
    m->stream_frames[m->stream_frame_head].len = 0;
    m->stream_frame_head = (m->stream_frame_head + 1u) % MOCK_STREAM_FRAMES_MAX;
    m->stream_frame_count--;
  }
  if (m->stream_frame_count == 0) return false;
  memcpy(buf, m->stream_frames[m->stream_frame_head].data, m->stream_frames[m->stream_frame_head].len);
  *out_len = m->stream_frames[m->stream_frame_head].len;
  if (out_ts90k) *out_ts90k = m->stream_frames[m->stream_frame_head].ts90k;
  if (out_keyframe) *out_keyframe = m->stream_frames[m->stream_frame_head].keyframe;
  ccord_free(m->stream_frames[m->stream_frame_head].data);
  m->stream_frames[m->stream_frame_head].data = nullptr;
  m->stream_frames[m->stream_frame_head].len = 0;
  m->stream_frame_head = (m->stream_frame_head + 1u) % MOCK_STREAM_FRAMES_MAX;
  m->stream_frame_count--;
  return true;
}

// The sender queues 48 kHz mono S16LE stream audio in a ring bounded to one second of samples.
static ccord_status mock_send_stream_pcm(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!pcm || samples == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty stream pcm");
  if (!m->has_stream || !m->stream.active) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: no active Go Live stream");
  }
  if (samples > MOCK_STREAM_PCM_CAP || m->stream_pcm_count + samples > MOCK_STREAM_PCM_CAP) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: stream pcm ring full");
  }
  size_t tail = (m->stream_pcm_head + m->stream_pcm_count) % MOCK_STREAM_PCM_CAP;
  size_t first = samples < MOCK_STREAM_PCM_CAP - tail ? samples : MOCK_STREAM_PCM_CAP - tail;
  memcpy(&m->stream_pcm[tail], pcm, first * sizeof *pcm);
  if (samples > first) {
    memcpy(&m->stream_pcm[0], pcm + first, (samples - first) * sizeof *pcm);
  }
  m->stream_pcm_count += samples;
  return CCORD_OK;
}

static size_t mock_poll_stream_pcm(ccord_transport *t, int16_t *out_pcm, size_t cap_samples)
{
  mock *m = (mock *)t;
  if (!m || !out_pcm || cap_samples == 0 || m->stream_pcm_count == 0) return 0;
  size_t take = cap_samples < m->stream_pcm_count ? cap_samples : m->stream_pcm_count;
  size_t first = take < MOCK_STREAM_PCM_CAP - m->stream_pcm_head ? take : MOCK_STREAM_PCM_CAP - m->stream_pcm_head;
  memcpy(out_pcm, &m->stream_pcm[m->stream_pcm_head], first * sizeof *out_pcm);
  if (take > first) {
    memcpy(out_pcm + first, &m->stream_pcm[0], (take - first) * sizeof *out_pcm);
  }
  m->stream_pcm_head = (m->stream_pcm_head + take) % MOCK_STREAM_PCM_CAP;
  m->stream_pcm_count -= take;
  return take;
}

static ccord_status queue_voice_server(mock *m, ccord_id guild_id, ccord_id channel_id, uint64_t due, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_VOICE_SERVER_UPDATE, due);
  it.vserver_own = ccord_alloc_array(1, sizeof *it.vserver_own);
  if (!it.vserver_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  // Static literals: the item only owns the struct, never the strings.
  *it.vserver_own = (ccord_voice_server){.guild_id = guild_id, .channel_id = channel_id,
                                         .token = "mock-voice-token", .endpoint = "mock.voice.discord:443"};
  it.ev.voice_server = it.vserver_own;
  it.ev.guild_id = guild_id;
  it.ev.channel_id = channel_id;
  return push_item(m, it, err);
}

static long mock_call_index(const mock *m, ccord_id channel_id)
{
  for (size_t i = 0; i < m->ndm_calls; i++) {
    if (m->dm_calls[i].channel_id == channel_id) return (long)i;
  }
  return -1;
}

// CALL_CREATE / CALL_UPDATE carry an owned copy of the row; CALL_DELETE only the channel id.
static ccord_status queue_call_event(mock *m, const ccord_call_state *call, ccord_net_event_type type,
                                     uint64_t due, ccord_error *err)
{
  mock_item it = make_item(type, due);
  it.ev.channel_id = call->channel_id;
  it.ev.guild_id = CCORD_DM_GUILD_ID;
  if (type != CCORD_NET_CALL_DELETE) {
    it.call_own = ccord_alloc_array(1, sizeof *it.call_own);
    if (!it.call_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    *it.call_own = *call;
    it.ev.call = it.call_own;
  }
  return push_item(m, it, err);
}

static bool call_drop_ringing(ccord_call_state *call, ccord_id user_id)
{
  for (size_t i = 0; i < call->ringing_count; i++) {
    if (call->ringing[i] == user_id) {
      memmove(&call->ringing[i], &call->ringing[i + 1],
              (call->ringing_count - i - 1u) * sizeof call->ringing[0]);
      call->ringing_count--;
      return true;
    }
  }
  return false;
}

static size_t mock_call_participants(const mock *m, ccord_id channel_id)
{
  size_t n = 0;
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].guild_id == CCORD_DM_GUILD_ID && m->voices[i].channel_id == channel_id) n++;
  }
  return n;
}

// A call with nobody left in it ends: the row goes and CALL_DELETE tells the client to stop ringing.
static ccord_status mock_call_maybe_end(mock *m, size_t ci, uint64_t due, ccord_error *err)
{
  if (mock_call_participants(m, m->dm_calls[ci].channel_id) > 0) return CCORD_OK;
  ccord_call_state ended = m->dm_calls[ci];
  memmove(&m->dm_calls[ci], &m->dm_calls[ci + 1], (m->ndm_calls - ci - 1u) * sizeof m->dm_calls[0]);
  m->ndm_calls--;
  return queue_call_event(m, &ended, CCORD_NET_CALL_DELETE, due, err);
}

static long mock_stage_index(const mock *m, ccord_id channel_id)
{
  for (size_t i = 0; i < m->nstages; i++) {
    if (m->stages[i].channel_id == channel_id) return (long)i;
  }
  return -1;
}

static ccord_status queue_stage_instance(mock *m, const ccord_stage_instance *stage, bool deleted,
                                         uint64_t due, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_STAGE_INSTANCE_UPDATE, due);
  it.stage_own = ccord_alloc_array(1, sizeof *it.stage_own);
  if (!it.stage_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.stage_own = *stage;
  it.ev.stage_instance = it.stage_own;
  it.ev.stage_deleted = deleted;
  it.ev.guild_id = stage->guild_id;
  it.ev.channel_id = stage->channel_id;
  return push_item(m, it, err);
}

// Joining a stage channel with no live instance starts one (the mock's stand-in for POST /stage-instances).
static ccord_status mock_stage_ensure(mock *m, ccord_id guild_id, ccord_id channel_id, uint64_t due, ccord_error *err)
{
  if (mock_stage_index(m, channel_id) >= 0) return CCORD_OK;
  if (m->nstages >= MOCK_STAGES_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many stage instances");
  if (m->next_stage_id == 0) m->next_stage_id = 96001u;
  ccord_stage_instance stage = {.id = m->next_stage_id++, .channel_id = channel_id, .guild_id = guild_id,
                                .topic = "", .privacy_level = 2};
  m->stages[m->nstages++] = stage;
  return queue_stage_instance(m, &stage, false, due, err);
}

// The instance ends when the last listener leaves the stage channel.
static ccord_status mock_stage_cleanup(mock *m, ccord_id channel_id, uint64_t due, ccord_error *err)
{
  long si = mock_stage_index(m, channel_id);
  if (si < 0) return CCORD_OK;
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].channel_id == channel_id) return CCORD_OK;
  }
  ccord_stage_instance ended = m->stages[si];
  memmove(&m->stages[si], &m->stages[si + 1], (m->nstages - (size_t)si - 1u) * sizeof m->stages[0]);
  m->nstages--;
  return queue_stage_instance(m, &ended, true, due, err);
}

static ccord_status queue_voice_status(mock *m, ccord_id guild_id, ccord_id channel_id, const char *status,
                                       uint64_t due, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_VOICE_STATUS_UPDATE, due);
  it.voice_status_own = ccord_strndup(status ? status : "", status ? strlen(status) : 0);
  if (!it.voice_status_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  it.ev.voice_status = it.voice_status_own;
  it.ev.guild_id = guild_id;
  it.ev.channel_id = channel_id;
  return push_item(m, it, err);
}

// Go Live signalling. `kind` picks the payload: CREATE names the media connection, SERVER_UPDATE hands
// out its token/endpoint, UPDATE carries pause/viewers, DELETE the reason. Call streams use guild 0.
static ccord_status queue_stream_signal_ex(mock *m, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                           ccord_stream_signal_kind kind, uint32_t viewer_count, bool paused,
                                           const char *reason, ccord_error *err)
{
  bool is_call = guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID;
  char sk[96];
  if (is_call) {
    (void)snprintf(sk, sizeof sk, "call:%llu:%llu", (unsigned long long)channel_id, (unsigned long long)user_id);
  } else {
    (void)snprintf(sk, sizeof sk, "guild:%llu:%llu:%llu", (unsigned long long)guild_id,
                   (unsigned long long)channel_id, (unsigned long long)user_id);
  }
  char *sk_copy = ccord_strndup(sk, strlen(sk));
  ccord_stream_signal *sig = ccord_calloc(1, sizeof *sig);
  if (!sk_copy || !sig) {
    ccord_free(sk_copy);
    ccord_free(sig);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  retire_content(m, sk_copy);
  sig->kind = kind;
  sig->is_call = is_call;
  sig->guild_id = is_call ? CCORD_ID_NONE : guild_id;
  sig->channel_id = channel_id;
  sig->user_id = user_id;
  sig->owner_id = user_id;
  sig->stream_key = sk_copy;
  sig->viewer_count = viewer_count;
  sig->paused = paused;
  sig->server_update = kind == CCORD_STREAM_SIG_SERVER_UPDATE;
  sig->deleted = kind == CCORD_STREAM_SIG_DELETE;
  if (kind == CCORD_STREAM_SIG_CREATE) {
    sig->rtc_server_id = is_call ? channel_id : guild_id;
    sig->rtc_channel_id = channel_id;
  } else if (kind == CCORD_STREAM_SIG_SERVER_UPDATE) {
    sig->token = "mock-stream-token";
    sig->endpoint = "mock.stream.discord:443";
    sig->session_id = "mock-voice-session";
  } else if (kind == CCORD_STREAM_SIG_DELETE) {
    sig->reason = reason ? reason : "";
  }
  mock_item it = make_item(CCORD_NET_STREAM_SIGNAL, now_of(m) + m->cfg.latency_ms);
  it.sig = sig;
  it.ev.guild_id = sig->guild_id;
  it.ev.channel_id = channel_id;
  it.ev.user_id = user_id;
  it.ev.stream_signal = sig;
  return push_item(m, it, err);
}

// ---- vtable ----

static const ccord_emoji k_seed_emojis[] = {
  {.id = 5001, .guild_id = 2001, .name = "ccord", .animated = false, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 5002, .guild_id = 2001, .name = "shipit", .animated = false, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 5003, .guild_id = 2001, .name = "valgrind", .animated = false, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 5004, .guild_id = 2001, .name = "partyblob", .animated = true, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 5005, .guild_id = 2001, .name = "c23", .animated = false, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 5006, .guild_id = 2002, .name = "luajit", .animated = false, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 5007, .guild_id = 2002, .name = "moon", .animated = false, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 5008, .guild_id = 2002, .name = "coroutine", .animated = true, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 5009, .guild_id = 2003, .name = "pixelheart", .animated = false, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 5010, .guild_id = 2003, .name = "sprout", .animated = false, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 5011, .guild_id = 2003, .name = "starlight", .animated = true, .available = true, .user_id = 1002, .user_name = "Ada"},
};

static const ccord_sticker k_seed_stickers[] = {
  {.id = 7001, .guild_id = 2001, .name = "WumpusWave", .description = "Friendly wave", .tags = "wave,hi", .format = CCORD_STICKER_PNG, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 7002, .guild_id = 2001, .name = "CCordRocket", .description = "Blast off", .tags = "rocket,ship", .format = CCORD_STICKER_APNG, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 7003, .guild_id = 2001, .name = "ZeroLeak", .description = "0 bytes lost", .tags = "valgrind,clean", .format = CCORD_STICKER_PNG, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 7004, .guild_id = 2001, .name = "LottieSparkle", .description = "Vector sparkle", .tags = "sparkle,lottie", .format = CCORD_STICKER_LOTTIE, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 7005, .guild_id = 2002, .name = "MoonCat", .description = "Lua cat", .tags = "cat,lua", .format = CCORD_STICKER_PNG, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 7006, .guild_id = 2002, .name = "TableFlip", .description = "Nil index", .tags = "flip,table", .format = CCORD_STICKER_GIF, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 7007, .guild_id = 2003, .name = "PixelBloom", .description = "8-bit flower", .tags = "flower,pixel", .format = CCORD_STICKER_PNG, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 7008, .guild_id = 2003, .name = "CozyTea", .description = "Warm cup", .tags = "tea,cozy", .format = CCORD_STICKER_PNG, .available = true, .user_id = 1002, .user_name = "Ada"},
};

static const ccord_soundboard_sound k_seed_soundboard[] = {
  {.id = 6501, .guild_id = 2001, .name = "airhorn", .volume_pct = 85, .emoji_name = "\xf0\x9f\x93\xa2", .user_id = 1001, .user_name = "You", .available = true},
  {.id = 6502, .guild_id = 2001, .name = "rimshot", .volume_pct = 100, .emoji_name = "\xf0\x9f\xa5\x81", .user_id = 1002, .user_name = "Ada", .available = true},
  {.id = 6503, .guild_id = 2001, .name = "compile_ok", .volume_pct = 75, .emoji_id = 5001, .emoji_name = "ccord", .user_id = 1001, .user_name = "You", .available = true},
  {.id = 6504, .guild_id = 2002, .name = "meow", .volume_pct = 90, .emoji_name = "\xf0\x9f\x90\xb1", .user_id = 1002, .user_name = "Ada", .available = true},
  {.id = 6505, .guild_id = 2003, .name = "chime", .volume_pct = 80, .emoji_name = "\xe2\x9c\xa8", .user_id = 1001, .user_name = "You", .available = true},
};

// Queues CONNECTED and the full SNAPSHOT (plus the gateway dispatches that follow it), all due at `due`. Shared by the
// first connect and by a fresh IDENTIFY after the link model lost the session.
static ccord_status mock_queue_ready(mock *m, uint64_t due, ccord_error *err)
{
  mock_item st = make_item(CCORD_NET_STATE, due);
  st.ev.state = CCORD_CONN_CONNECTED;
  ccord_status s = push_item(m, st, err);
  if (s != CCORD_OK) return s;
  mock_item snap = make_item(CCORD_NET_SNAPSHOT, due);
  snap.ev.snapshot = &m->snap;
  if (m->nemojis > 0) {
    snap.emojis_own = ccord_alloc_array(m->nemojis, sizeof *snap.emojis_own);
    if (!snap.emojis_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    for (size_t i = 0; i < m->nemojis; i++) {
      snap.emojis_own[i] = m->emojis[i];
      snap.emojis_own[i].user_id = CCORD_ID_NONE;
      snap.emojis_own[i].user_name = "";
    }
    snap.ev.emojis = snap.emojis_own;
    snap.ev.emoji_count = m->nemojis;
  }
  if (m->nstickers > 0) {
    snap.stickers_own = ccord_alloc_array(m->nstickers, sizeof *snap.stickers_own);
    if (!snap.stickers_own) {
      item_release(&snap);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    for (size_t i = 0; i < m->nstickers; i++) {
      snap.stickers_own[i] = m->stickers[i];
      snap.stickers_own[i].user_id = CCORD_ID_NONE;
      snap.stickers_own[i].user_name = "";
    }
    snap.ev.stickers = snap.stickers_own;
    snap.ev.sticker_count = m->nstickers;
  }
  if (m->nsoundboard > 0) {
    snap.soundboard_own = ccord_alloc_array(m->nsoundboard, sizeof *snap.soundboard_own);
    if (!snap.soundboard_own) {
      item_release(&snap);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    for (size_t i = 0; i < m->nsoundboard; i++) {
      snap.soundboard_own[i] = m->soundboard[i];
      snap.soundboard_own[i].user_id = CCORD_ID_NONE;
      snap.soundboard_own[i].user_name = "";
    }
    snap.ev.soundboard = snap.soundboard_own;
    snap.ev.soundboard_count = m->nsoundboard;
  }
  snap.ev.forum_metas = m->forum_metas;
  snap.ev.forum_meta_count = m->nforum_metas;
  snap.ev.forum_posts = m->forum_posts;
  snap.ev.forum_post_count = m->nforum_posts;
  snap.ev.roles = m->roles;
  snap.ev.role_count = m->nroles;
  snap.ev.member_roles = m->member_roles;
  snap.ev.member_role_count = m->nmember_roles;
  snap.ev.overwrites = m->overwrites;
  snap.ev.overwrite_count = m->noverwrites;
  snap.ev.notify = m->notify;
  snap.ev.notify_count = m->nnotify;
  s = push_item(m, snap, err);
  if (s != CCORD_OK) return s;
  // Stage instances and scheduled events are gateway dispatches, not snapshot rows: they follow the snapshot.
  for (size_t i = 0; i < m->nstages; i++) {
    s = queue_stage_instance(m, &m->stages[i], false, due, err);
    if (s != CCORD_OK) return s;
  }
  for (size_t gi = 0; gi < m->nguilds; gi++) {
    ccord_id gid = m->guilds[gi].id;
    size_t ev_n = 0;
    for (size_t ei = 0; ei < m->nevents; ei++) {
      if (m->events[ei].guild_id == gid) ev_n++;
    }
    if (ev_n == 0) continue;
    mock_item eit = make_item(CCORD_NET_SCHEDULED_EVENT_LIST, due);
    eit.events_own = ccord_alloc_array(ev_n, sizeof *eit.events_own);
    if (!eit.events_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    size_t k = 0;
    for (size_t ei = 0; ei < m->nevents; ei++) {
      if (m->events[ei].guild_id == gid) eit.events_own[k++] = m->events[ei];
    }
    eit.ev.guild_id = gid;
    eit.ev.scheduled_events = eit.events_own;
    eit.ev.scheduled_event_count = ev_n;
    s = push_item(m, eit, err);
    if (s != CCORD_OK) return s;
  }
  // Running activity instances survive a reconnect: Discord re-sends them with the guild's state.
  for (size_t i = 0; i < m->nactivities; i++) {
    s = queue_activity_instance(m, &m->activities[i], due, err);
    if (s != CCORD_OK) return s;
  }
  return CCORD_OK;
}

static void mock_link_on_connect(mock *m, uint64_t now);

static ccord_status mock_connect(ccord_transport *t, ccord_error *err)
{
  mock *m = (mock *)t;
  if (m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: already connected");
  m->connected = true;
  uint64_t now = now_of(m);
  m->next_ambient = m->cfg.ambient_interval_ms ? now + m->cfg.ambient_interval_ms : 0;
  mock_link_on_connect(m, now);
  mock_item st = make_item(CCORD_NET_STATE, now);
  st.ev.state = CCORD_CONN_CONNECTING;
  ccord_status s = push_item(m, st, err);
  if (s != CCORD_OK) return s;
  return mock_queue_ready(m, now + m->cfg.latency_ms, err);
}

static void mock_disconnect(ccord_transport *t)
{
  mock *m = (mock *)t;
  if (!m->connected) return;
  m->connected = false;
  m->link.state = MOCK_LINK_UP;
  m->link.dead_silent = false;
  mock_stream_media_clear(m);
  mock_cam_clear(m);
  m->ninflight_uploads = 0;
  free_items(m->queue, m->nq);
  m->nq = 0;
  mock_item st = make_item(CCORD_NET_STATE, now_of(m));
  st.ev.state = CCORD_CONN_DISCONNECTED;
  push_item(m, st, nullptr);
}

static ccord_status mock_request_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_HISTORY]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  if (limit == 0) limit = MOCK_HISTORY_DEFAULT;
  size_t total = m->chn[ci];
  size_t take = limit < total ? limit : total;
  size_t from = total - take;

  mock_item it = make_item(CCORD_NET_HISTORY, now_of(m) + m->cfg.latency_ms);
  it.ev.channel_id = channel_id;
  it.msgs = ccord_alloc_array(take ? take : 1, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  if (take) memcpy(it.msgs, m->chmsgs[ci] + from, take * sizeof *it.msgs);
  it.ev.messages = it.msgs;
  it.ev.message_count = take;
  return push_item(m, it, err);
}

// refuse_writes: the call succeeded but Discord says no. Reported on a later poll, with the user's text.
// The nonce (M0-3 sends) rides along so the session can fail exactly the pending echo this refusal belongs to.
static ccord_status queue_action_refused(mock *m, ccord_action_kind action, ccord_id channel_id, ccord_id message_id,
                                         const char *utf8, size_t len, const char *nonce, size_t nonce_len,
                                         ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_ACTION_FAILED, now_of(m) + m->cfg.latency_ms);
  it.draft = ccord_strndup(utf8, len);
  if (!it.draft) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  it.ev.action = action;
  it.ev.channel_id = channel_id;
  it.ev.message_id = message_id;
  it.ev.status = CCORD_ERR_PERMISSION;
  it.ev.text = "You don't have permission to do that.";
  it.ev.draft = it.draft;
  it.ev.draft_len = len;
  if (nonce && nonce_len > 0 && nonce_len < sizeof it.ev.action_nonce) {
    memcpy(it.ev.action_nonce, nonce, nonce_len);
    it.ev.action_nonce[nonce_len] = '\0';
  }
  return push_item(m, it, err);
}

static uint64_t mock_self_perms(mock *m, ccord_id guild);
static uint64_t mock_channel_perms(mock *m, ccord_id guild, ccord_id channel_id);

static bool mock_slowmode_refused(mock *m, size_t ci, ccord_action_kind action, ccord_id message_id,
                                  const char *utf8, size_t len, const char *nonce, size_t nonce_len,
                                  ccord_status *out, ccord_error *err)
{
  const ccord_channel *ch = &m->channels[ci];
  uint32_t rl = ch->rate_limit_per_user;
  if (rl == 0 || ch->guild_id == CCORD_DM_GUILD_ID || ch->guild_id == CCORD_ID_NONE) return false;
  uint64_t perms = mock_self_perms(m, ch->guild_id);
  if ((perms & (CCORD_PERM_BYPASS_SLOWMODE | CCORD_PERM_MANAGE_CHANNELS | CCORD_PERM_MANAGE_MESSAGES)) != 0) {
    return false;
  }
  uint64_t now = now_of(m);
  for (size_t i = 0; i < sizeof m->slowmode_sent / sizeof m->slowmode_sent[0]; i++) {
    if (m->slowmode_sent[i].has_sent && m->slowmode_sent[i].channel_id == ch->id) {
      if (now < m->slowmode_sent[i].sent_ms + (uint64_t)rl * 1000ull) {
        mock_item it = make_item(CCORD_NET_ACTION_FAILED, now + m->cfg.latency_ms);
        it.draft = ccord_strndup(utf8 ? utf8 : "", len);
        if (!it.draft) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.ev.action = action;
        it.ev.channel_id = ch->id;
        it.ev.message_id = message_id;
        it.ev.status = CCORD_ERR_PERMISSION;
        it.ev.text = "This channel is in slowmode. Wait a moment before sending again.";
        it.ev.draft = it.draft;
        it.ev.draft_len = len;
        if (nonce && nonce_len > 0 && nonce_len < sizeof it.ev.action_nonce) {
          memcpy(it.ev.action_nonce, nonce, nonce_len);
          it.ev.action_nonce[nonce_len] = '\0';
        }
        *out = push_item(m, it, err);
        return true;
      }
      break;
    }
  }
  return false;
}

static void mock_slowmode_mark_sent(mock *m, ccord_id channel_id)
{
  size_t slot = 0;
  for (size_t i = 0; i < sizeof m->slowmode_sent / sizeof m->slowmode_sent[0]; i++) {
    if (m->slowmode_sent[i].has_sent && m->slowmode_sent[i].channel_id == channel_id) {
      m->slowmode_sent[i].sent_ms = now_of(m);
      return;
    }
    if (!m->slowmode_sent[i].has_sent && slot == 0 && !m->slowmode_sent[0].has_sent) slot = i;
    else if (!m->slowmode_sent[i].has_sent && m->slowmode_sent[slot].has_sent) slot = i;
  }
  m->slowmode_sent[slot].channel_id = channel_id;
  m->slowmode_sent[slot].sent_ms = now_of(m);
  m->slowmode_sent[slot].has_sent = true;
}

static ccord_status mock_send_message_impl(mock *m, ccord_id channel_id, const char *utf8, size_t len,
                                           const char *nonce, size_t nonce_len, ccord_error *err)
{
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (len == 0 || len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: message length");
  if (!ccord_utf8_valid_text(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message is not valid text");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  if (m->cfg.refuse_writes) {
    return queue_action_refused(m, CCORD_ACTION_SEND, channel_id, CCORD_ID_NONE, utf8, len, nonce, nonce_len, err);
  }
  ccord_status sm_st = CCORD_OK;
  if (mock_slowmode_refused(m, ci, CCORD_ACTION_SEND, CCORD_ID_NONE, utf8, len, nonce, nonce_len, &sm_st, err)) return sm_st;
  ccord_message msg = store_new(m, ci, m->self_id, utf8, len, err, &st);
  if (st != CCORD_OK) return st;
  if (nonce && nonce_len > 0) {
    // The stored row and the queued echo both carry the nonce, like Discord echoing it to the sender.
    memcpy(msg.nonce, nonce, nonce_len);
    msg.nonce[nonce_len] = '\0';
    long mi = find_message(m, ci, msg.id);
    if (mi >= 0) memcpy(m->chmsgs[ci][mi].nonce, msg.nonce, sizeof msg.nonce);
  }
  mock_slowmode_mark_sent(m, channel_id);
  return queue_message_create(m, msg, now_of(m) + m->cfg.latency_ms, err);
}

static ccord_status mock_send_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len, ccord_error *err)
{
  return mock_send_message_impl((mock *)t, channel_id, utf8, len, nullptr, 0, err);
}

// M0-3: send with the session's nonce; the echo (or refusal) carries it back.
static ccord_status mock_send_message_with_nonce(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                                 const char *nonce, size_t nonce_len, ccord_error *err)
{
  if (!nonce || nonce_len == 0 || nonce_len >= 32u || memchr(nonce, 0, nonce_len) != nullptr) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: send nonce");
  }
  return mock_send_message_impl((mock *)t, channel_id, utf8, len, nonce, nonce_len, err);
}

static ccord_status mock_edit_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                      const char *utf8, size_t len, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (len == 0 || len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: message length");
  if (!ccord_utf8_valid_text(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message is not valid text");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  if (m->cfg.refuse_writes) return queue_action_refused(m, CCORD_ACTION_EDIT, channel_id, message_id, utf8, len, nullptr, 0, err);
  char *content = ccord_strndup(utf8, len);
  if (!content) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  ccord_message *msg = &m->chmsgs[ci][mi];
  retire_content(m, msg->content);
  msg->content = content;
  msg->edited = true;
  msg->edited_timestamp_ms = message_time(m);
  return queue_message_update(m, *msg, now_of(m) + m->cfg.latency_ms, err);
}

static ccord_status mock_delete_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  retire_content(m, m->chmsgs[ci][mi].content);
  retire_rich(m, m->chmsgs[ci][mi].rich);
  memmove(&m->chmsgs[ci][mi], &m->chmsgs[ci][mi + 1], (m->chn[ci] - (size_t)mi - 1) * sizeof *m->chmsgs[ci]);
  m->chn[ci]--;
  mock_item it = make_item(CCORD_NET_MESSAGE_DELETE, now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = m->channels[ci].guild_id;
  it.ev.channel_id = channel_id;
  it.ev.message_id = message_id;
  return push_item(m, it, err);
}

static ccord_status mock_send_typing(ccord_transport *t, ccord_id channel_id, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_TYPING]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  uint64_t due = now_of(m) + m->cfg.latency_ms;
  mock_item it = make_item(CCORD_NET_TYPING_START, due);
  it.ev.guild_id = m->channels[ci].guild_id;
  it.ev.channel_id = channel_id;
  it.ev.user_id = m->self_id;
  it.ev.timestamp_ms = (int64_t)due + 8000;
  return push_item(m, it, err);
}

static ccord_status mock_toggle_reaction(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                         const char *emoji, size_t len, bool add, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!emoji || len == 0 || !ccord_utf8_valid_text(emoji, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid reaction");
  }
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  ccord_message *msg = &m->chmsgs[ci][mi];
  if (add) {
    msg->self_reacted = true;
    msg->reaction_count++;
  } else {
    if (msg->reaction_count > 0) msg->reaction_count--;
    if (msg->reaction_count == 0) msg->self_reacted = false;
  }
  mock_item it = make_item(CCORD_NET_REACTION_UPDATE, now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = m->channels[ci].guild_id;
  it.ev.channel_id = channel_id;
  it.ev.message_id = message_id;
  it.ev.user_id = m->self_id;
  it.ev.reaction_count = msg->reaction_count;
  it.ev.self_reacted = msg->self_reacted;
  it.ev.reaction_delta = add ? 1 : -1;
  char *emoji_copy = ccord_strndup(emoji, len);
  if (emoji_copy) {
    retire_content(m, emoji_copy);
    it.ev.reaction_emoji = emoji_copy;
  }
  return push_item(m, it, err);
}

static ccord_status mock_request_reaction_users(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                                const char *emoji, size_t emoji_len, uint32_t limit, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_REACTION_USERS]++;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!emoji || emoji_len == 0 || !ccord_utf8_valid_text(emoji, emoji_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid reaction emoji");
  }
  ccord_status st;
  size_t ci = channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  if (limit == 0) limit = 50u;
  if (limit > 100u) limit = 100u;

  const char *we = emoji;
  size_t wl = emoji_len;
  if (wl > 4 && we[0] == '<' && we[wl - 1] == '>') {
    if (we[1] == ':') { we += 2; wl -= 3; }
    else if ((we[1] == 'a' || we[1] == 'A') && we[2] == ':') { we += 3; wl -= 4; }
  }

  const ccord_message *msg = &m->chmsgs[ci][mi];
  uint32_t want_count = msg->reaction_count;
  bool inc_self = msg->self_reacted;
  if (msg->reactions && msg->reaction_kind_count > 0) {
    want_count = 0;
    inc_self = false;
    for (size_t i = 0; i < msg->reaction_kind_count; i++) {
      const char *re = msg->reactions[i].emoji;
      if (re && strlen(re) == wl && memcmp(re, we, wl) == 0) {
        want_count = msg->reactions[i].count;
        inc_self = msg->reactions[i].self_reacted;
        break;
      }
    }
  }
  if (want_count > limit) want_count = limit;
  if (want_count > MOCK_NUM_USERS) want_count = MOCK_NUM_USERS;

  mock_item it = make_item(CCORD_NET_REACTION_USERS, now_of(m) + m->cfg.latency_ms);
  it.users_own = ccord_alloc_array(want_count ? want_count : 1u, sizeof *it.users_own);
  char *ecopy = ccord_strndup(we, wl);
  if (!it.users_own || !ecopy) {
    ccord_free(it.users_own);
    ccord_free(ecopy);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  retire_content(m, ecopy);

  size_t kept = 0;
  if (inc_self && kept < want_count) {
    it.users_own[kept++] = m->users[0];
  }
  size_t start_u = (size_t)((message_id % (MOCK_NUM_USERS - 1u)) + 1u);
  for (size_t step = 0; step < MOCK_NUM_USERS - 1u && kept < want_count; step++) {
    size_t ui = 1u + ((start_u - 1u + step) % (MOCK_NUM_USERS - 1u));
    it.users_own[kept++] = m->users[ui];
  }
  it.ev.channel_id = channel_id;
  it.ev.message_id = message_id;
  it.ev.reaction_emoji = ecopy;
  it.ev.users = it.users_own;
  it.ev.user_count = kept;
  return push_item(m, it, err);
}

static ccord_status mock_ack_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_ACK]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  if (find_message(m, ci, message_id) < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  return CCORD_OK;
}

static void queue_speaking(mock *m, ccord_id guild_id, ccord_id channel_id, ccord_id user_id, bool speaking,
                           uint64_t due)
{
  mock_item it = make_item(CCORD_NET_SPEAKING, due);
  it.ev.guild_id = guild_id;
  it.ev.channel_id = channel_id;
  it.ev.user_id = user_id;
  it.ev.speaking = speaking;
  (void)push_item(m, it, nullptr);
}

// ---- voice outage simulation ---------------------------
// Queues one CCORD_NET_VOICE_LINK at `due`. `text` must be a static string. Also records the rtc state the
// mock reports from then on (ccord_mock_rtc_stats follows this timeline).
static ccord_status queue_voice_link(mock *m, ccord_voice_link st, uint32_t attempt, uint32_t retry_ms,
                                     const char *text, uint64_t due, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_VOICE_LINK, due);
  it.ev.voice_link = (uint8_t)st;
  it.ev.voice_link_attempt = attempt;
  it.ev.voice_link_retry_ms = retry_ms;
  it.ev.text = text ? text : "";
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].user_id == m->self_id && m->voices[i].channel_id != CCORD_ID_NONE) {
      it.ev.guild_id = m->voices[i].guild_id;
      it.ev.channel_id = m->voices[i].channel_id;
      break;
    }
  }
  ccord_status s = push_item(m, it, err);
  if (s == CCORD_OK && m->nvstate_tl < sizeof m->vstate_tl / sizeof m->vstate_tl[0]) {
    uint8_t rs = st == CCORD_VLINK_CONNECTED ? CCORD_VOICE_ESTABLISHED
               : st == CCORD_VLINK_RECONNECTING ? CCORD_VOICE_RECONNECTING : CCORD_VOICE_FAILED;
    m->vstate_tl[m->nvstate_tl].due = due;
    m->vstate_tl[m->nvstate_tl].state = rs;
    m->nvstate_tl++;
  }
  return s;
}

// A hang-up or a new join cancels a pending outage: queued VOICE_LINK items are dropped and the rtc timeline reset.
static void mock_purge_voice_link(mock *m)
{
  size_t w = 0;
  for (size_t i = 0; i < m->nq; i++) {
    if (m->queue[i].ev.type == CCORD_NET_VOICE_LINK) {
      item_release(&m->queue[i]);
      continue;
    }
    if (w != i) m->queue[w] = m->queue[i];
    w++;
  }
  m->nq = w;
  m->nvstate_tl = 0;
}

static ccord_status mock_join_voice(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  mock *m = (mock *)t;
  mock_purge_voice_link(m);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!mock_guild_known(m, guild_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown guild");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  bool stage = m->channels[ci].kind == CCORD_CHANNEL_STAGE;
  if (m->channels[ci].guild_id != guild_id ||
      (m->channels[ci].kind != CCORD_CHANNEL_VOICE && !stage)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not a voice channel of the guild");
  }
  m->calls[CCORD_MOCK_CALL_VOICE_JOIN]++;
  long vi = mock_voice_index(m, guild_id, m->self_id);
  bool srv_mute = false, srv_deaf = false;
  for (size_t i = 0; i < m->nmembers; i++) {
    if (m->members[i].guild_id == guild_id && m->members[i].user_id == m->self_id) {
      srv_mute = m->members[i].mute;
      srv_deaf = m->members[i].deaf;
      break;
    }
  }
  // Stage joiners are audience: suppressed until a moderator invites them to speak.
  ccord_voice_state v = {.guild_id = guild_id, .channel_id = channel_id, .user_id = m->self_id,
                         .self_mute = m->self_mute, .self_deaf = m->self_deaf,
                         .mute = srv_mute, .deaf = srv_deaf,
                         .self_video = false, .self_stream = false, .suppress = stage};
  uint64_t due = now_of(m) + m->cfg.latency_ms;
  if (stage) {
    st = mock_stage_ensure(m, guild_id, channel_id, due, err);
    if (st != CCORD_OK) return st;
  }
  if (vi >= 0) {
    m->voices[vi] = v;
  } else {
    if (m->nvoices >= MOCK_NUM_USERS) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many voice states");
    m->voices[m->nvoices++] = v;
    m->snap.voice_state_count = m->nvoices;
  }
  mock_voice_connect(m, channel_id);
  st = queue_voice_state(m, &v, due, err);
  if (st == CCORD_OK) st = queue_voice_server(m, guild_id, channel_id, due, err);
  if (st == CCORD_OK) {
    queue_speaking(m, guild_id, channel_id, m->self_id, !stage && !m->self_mute && !srv_mute, due);
    for (size_t i = 0; i < m->nvoices; i++) {
      if (m->voices[i].guild_id == guild_id && m->voices[i].channel_id == channel_id &&
          m->voices[i].user_id != m->self_id && !m->voices[i].self_mute && !m->voices[i].mute &&
          !m->voices[i].suppress) {
        queue_speaking(m, guild_id, channel_id, m->voices[i].user_id, true, due);
      }
    }
  }
  return st;
}

static ccord_status mock_leave_voice(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  mock *m = (mock *)t;
  mock_purge_voice_link(m);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  long vi = mock_voice_index(m, guild_id, m->self_id);
  if (vi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not in voice");
  ccord_voice_state v = m->voices[vi];
  ccord_id old_ch = v.channel_id;
  bool was_stage = v.suppress || mock_stage_index(m, old_ch) >= 0;
  v.channel_id = CCORD_ID_NONE;
  v.self_video = false;
  v.self_stream = false;
  v.suppress = false;
  v.request_to_speak_timestamp = 0;
  memmove(&m->voices[vi], &m->voices[vi + 1], (m->nvoices - (size_t)vi - 1) * sizeof *m->voices);
  m->nvoices--;
  m->snap.voice_state_count = m->nvoices;
  uint64_t due = now_of(m) + m->cfg.latency_ms;
  ccord_status st = CCORD_OK;
  if (m->has_stream && m->stream.guild_id == guild_id) {
    st = queue_stream_signal_ex(m, guild_id, m->stream.channel_id, m->stream.user_id,
                                CCORD_STREAM_SIG_DELETE, 0, false, "broadcaster left voice", err);
  }
  m->has_stream = false;
  mock_stream_media_clear(m);
  mock_voice_disconnect(m);
  if (st == CCORD_OK) st = queue_voice_state(m, &v, due, err);
  if (st == CCORD_OK && was_stage) st = mock_stage_cleanup(m, old_ch, due, err);
  if (st == CCORD_OK) {
    queue_speaking(m, guild_id, CCORD_ID_NONE, m->self_id, false, due);
    for (size_t i = 0; i < m->nvoices; i++) {
      if (m->voices[i].guild_id == guild_id && m->voices[i].channel_id == old_ch) {
        queue_speaking(m, guild_id, old_ch, m->voices[i].user_id, false, due);
      }
    }
  }
  return st;
}

static ccord_status mock_update_voice_state(ccord_transport *t, ccord_id guild_id, bool self_mute, bool self_deaf,
                                            ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (guild_id == CCORD_ID_NONE) {
    m->self_mute = self_mute;
    m->self_deaf = self_deaf;
    return CCORD_OK;
  }
  long vi = mock_voice_index(m, guild_id, m->self_id);
  if (vi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not in voice");
  m->self_mute = self_mute;
  m->self_deaf = self_deaf;
  m->voices[vi].self_mute = self_mute;
  m->voices[vi].self_deaf = self_deaf;
  uint64_t due = now_of(m) + m->cfg.latency_ms;
  ccord_status st = queue_voice_state(m, &m->voices[vi], due, err);
  if (st == CCORD_OK) {
    queue_speaking(m, guild_id, m->voices[vi].channel_id, m->self_id, !self_mute, due);
  }
  return st;
}

static ccord_status mock_start_stream(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                      const ccord_stream_config *config, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!config) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null stream config");
  long vi = mock_voice_index(m, guild_id, m->self_id);
  if (vi < 0 || m->voices[vi].channel_id != channel_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: join the voice channel before streaming");
  }
  m->voices[vi].self_stream = true;
  m->stream = (ccord_stream_state){.guild_id = guild_id, .channel_id = channel_id, .user_id = m->self_id,
                                   .active = true, .config = *config};
  m->has_stream = true;
  ccord_status st = queue_stream_state(m, &m->stream, now_of(m) + m->cfg.latency_ms, err);
  // The sender gets its own media connection, exactly like a viewer: CREATE then SERVER_UPDATE.
  if (st == CCORD_OK) {
    st = queue_stream_signal_ex(m, guild_id, channel_id, m->self_id, CCORD_STREAM_SIG_CREATE, 1, false, nullptr, err);
  }
  if (st == CCORD_OK) {
    st = queue_stream_signal_ex(m, guild_id, channel_id, m->self_id, CCORD_STREAM_SIG_SERVER_UPDATE, 1, false,
                                nullptr, err);
  }
  return st;
}

static ccord_status mock_stop_stream(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!m->has_stream || m->stream.guild_id != guild_id) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: no active stream");
  }
  long vi = mock_voice_index(m, guild_id, m->self_id);
  if (vi >= 0) m->voices[vi].self_stream = false;
  m->stream.active = false;
  ccord_stream_state stopped = m->stream;
  m->has_stream = false;
  mock_stream_media_clear(m);
  ccord_status st = queue_stream_state(m, &stopped, now_of(m) + m->cfg.latency_ms, err);
  if (st == CCORD_OK) {
    st = queue_stream_signal_ex(m, guild_id, stopped.channel_id, stopped.user_id, CCORD_STREAM_SIG_DELETE,
                                0, false, "broadcaster stopped", err);
  }
  return st;
}

static ccord_status mock_watch_stream(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                      ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid stream target");
  }
  ccord_status st = queue_stream_signal_ex(m, guild_id, channel_id, user_id, CCORD_STREAM_SIG_CREATE, 1, false,
                                           nullptr, err);
  if (st == CCORD_OK) {
    st = queue_stream_signal_ex(m, guild_id, channel_id, user_id, CCORD_STREAM_SIG_SERVER_UPDATE, 1, false,
                                nullptr, err);
  }
  return st;
}

static ccord_status mock_stop_watching(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                       ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid stream target");
  }
  return queue_stream_signal_ex(m, guild_id, channel_id, user_id, CCORD_STREAM_SIG_DELETE, 0, false,
                                "viewer stopped watching", err);
}

static ccord_status mock_set_stream_paused(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                           ccord_id user_id, bool paused, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (channel_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid stream target");
  }
  if (m->has_stream && m->stream.user_id == user_id && m->stream.guild_id == guild_id) {
    m->stream.paused = paused;
  }
  return queue_stream_signal_ex(m, guild_id, channel_id, user_id, CCORD_STREAM_SIG_UPDATE, 1, paused, nullptr, err);
}

static ccord_status mock_send_reply_ex(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id,
                                       bool mention_author, const char *utf8, size_t len, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (len == 0 || len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: message length");
  if (!ccord_utf8_valid_text(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message is not valid text");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long tmi = find_message(m, ci, reply_to_id);
  if (tmi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown reply target");
  if (m->cfg.refuse_writes) return queue_action_refused(m, CCORD_ACTION_REPLY, channel_id, reply_to_id, utf8, len, nullptr, 0, err);
  ccord_status sm_st = CCORD_OK;
  if (mock_slowmode_refused(m, ci, CCORD_ACTION_REPLY, reply_to_id, utf8, len, nullptr, 0, &sm_st, err)) return sm_st;
  // The reply carries the message it answers, as Discord attaches it.
  const ccord_message *target = &m->chmsgs[ci][tmi];
  const char *tname = "";
  for (size_t ui = 0; ui < MOCK_NUM_USERS; ui++) {
    if (m->users[ui].id == target->author_id && m->users[ui].name) tname = m->users[ui].name;
  }
  char snippet[CCORD_RICH_SNIPPET_MAX_BYTES + 1];
  size_t sn = 0;
  for (const char *tc = target->content ? target->content : ""; *tc;) {
    unsigned char c = (unsigned char)*tc;
    size_t seq = c < 0x80u ? 1u : (c >= 0xF0u ? 4u : (c >= 0xE0u ? 3u : (c >= 0xC0u ? 2u : 1u)));
    if (sn + seq >= sizeof snippet || strlen(tc) < seq) break;
    memcpy(snippet + sn, tc, seq);
    sn += seq;
    tc += seq;
  }
  snippet[sn] = '\0';
  ccord_id target_author = target->author_id;
  bool ping_target = mention_author && target_author != CCORD_ID_NONE && target_author != m->self_id;
  ccord_message_rich rich = {
    .present = CCORD_RICH_ALL, .type = 19,
    .mention_users = ping_target ? &target_author : nullptr,
    .mention_user_count = ping_target ? 1u : 0u,
    .reference = {.kind = CCORD_REF_REPLY, .message_id = reply_to_id, .channel_id = channel_id},
    .ref_preview = {.present = true, .author_id = target_author, .author_name = tname, .content = snippet,
                    .has_attachments = target->rich && target->rich->attachment_count > 0},
  };
  ccord_message msg = store_new_rich(m, ci, m->self_id, utf8, len, &rich, err, &st);
  if (st != CCORD_OK) return st;
  msg.reply_to_id = reply_to_id;
  if (m->chn[ci] > 0) m->chmsgs[ci][m->chn[ci] - 1].reply_to_id = reply_to_id;
  mock_slowmode_mark_sent(m, channel_id);
  return queue_message_create(m, msg, now_of(m) + m->cfg.latency_ms, err);
}

static ccord_status mock_send_reply(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id,
                                    const char *utf8, size_t len, ccord_error *err)
{
  return mock_send_reply_ex(t, channel_id, reply_to_id, true, utf8, len, err);
}

// ---- messaging parity: TTS, forward, crosspost, mark unread ------------------------------------

// POST /channels/{c}/messages with tts=true. Text rules as send_message; Discord additionally wants
// SEND_TTS_MESSAGES in a guild (DMs always allow it), so the mock refuses it where the role lacks the bit.
static ccord_status mock_send_tts_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                          ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (len == 0 || len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: message length");
  if (!utf8 || !ccord_utf8_valid_text(utf8, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message is not valid text");
  }
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  if (m->cfg.refuse_writes) {
    return queue_action_refused(m, CCORD_ACTION_SEND, channel_id, CCORD_ID_NONE, utf8, len, nullptr, 0, err);
  }
  ccord_id g = m->channels[ci].guild_id;
  if (g != CCORD_DM_GUILD_ID && g != CCORD_ID_NONE &&
      (mock_channel_perms(m, g, channel_id) & CCORD_PERM_SEND_TTS_MESSAGES) == 0u) {
    return queue_action_refused(m, CCORD_ACTION_SEND, channel_id, CCORD_ID_NONE, utf8, len, nullptr, 0, err);
  }
  ccord_status sm_st = CCORD_OK;
  if (mock_slowmode_refused(m, ci, CCORD_ACTION_SEND, CCORD_ID_NONE, utf8, len, nullptr, 0, &sm_st, err)) return sm_st;
  ccord_message msg = store_new(m, ci, m->self_id, utf8, len, err, &st);
  if (st != CCORD_OK) return st;
  msg.tts = true;
  long mi = find_message(m, ci, msg.id);
  if (mi >= 0) m->chmsgs[ci][mi].tts = true;   // the stored row carries it into history too
  mock_slowmode_mark_sent(m, channel_id);
  return queue_message_create(m, msg, now_of(m) + m->cfg.latency_ms, err);
}

// POST /channels/{c}/messages with message_reference type 1: the new message carries a snapshot of the source
// (its text, timestamp and media) in rich->forward, and its own content is empty like Discord's.
static ccord_status mock_forward_message(ccord_transport *t, ccord_id channel_id, ccord_id source_channel_id,
                                         ccord_id message_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  size_t sci = channel_index(m, source_channel_id, false, &st, err);
  if (sci == SIZE_MAX) return st;
  long mi = find_message(m, sci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  if (m->cfg.refuse_writes) {
    return queue_action_refused(m, CCORD_ACTION_SEND, channel_id, CCORD_ID_NONE, "", 0, nullptr, 0, err);
  }
  ccord_id dst_guild = m->channels[ci].guild_id;
  if (dst_guild != CCORD_DM_GUILD_ID && dst_guild != CCORD_ID_NONE &&
      (mock_channel_perms(m, dst_guild, channel_id) & CCORD_PERM_SEND_MESSAGES) == 0u) {
    return queue_action_refused(m, CCORD_ACTION_SEND, channel_id, CCORD_ID_NONE, "", 0, nullptr, 0, err);
  }
  ccord_status sm_st = CCORD_OK;
  if (mock_slowmode_refused(m, ci, CCORD_ACTION_SEND, CCORD_ID_NONE, "", 0, nullptr, 0, &sm_st, err)) return sm_st;
  const ccord_message *src = &m->chmsgs[sci][mi];
  const ccord_message_rich *sr = src->rich;
  ccord_id src_guild = m->channels[sci].guild_id;
  ccord_message_rich rich = {
    .present = CCORD_RICH_ALL,
    .reference = {.kind = CCORD_REF_FORWARD, .message_id = message_id, .channel_id = source_channel_id,
                  .guild_id = (src_guild == CCORD_DM_GUILD_ID || src_guild == CCORD_ID_NONE) ? CCORD_ID_NONE : src_guild},
    .forward = {.present = true, .content = src->content ? src->content : "", .timestamp_ms = src->timestamp_ms,
                .attachments = sr ? sr->attachments : nullptr, .attachment_count = sr ? sr->attachment_count : 0u,
                .embeds = sr ? sr->embeds : nullptr, .embed_count = sr ? sr->embed_count : 0u,
                .stickers = sr ? sr->stickers : nullptr, .sticker_count = sr ? sr->sticker_count : 0u},
  };
  ccord_message msg = store_new_rich(m, ci, m->self_id, "", 0, &rich, err, &st);
  if (st != CCORD_OK) return st;
  mock_slowmode_mark_sent(m, channel_id);
  return queue_message_create(m, msg, now_of(m) + m->cfg.latency_ms, err);
}

// POST /channels/{c}/messages/{m}/crosspost: announcement channels only. The crossposted flag rides the
// MESSAGE_UPDATE (nothing changes locally before the server confirms).
static ccord_status mock_crosspost_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                           ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  if (m->channels[ci].discord_type != CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not an announcement channel");
  }
  long mi = find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  if (m->cfg.refuse_writes) {
    return queue_action_refused(m, CCORD_ACTION_SEND, channel_id, message_id, "", 0, nullptr, 0, err);
  }
  ccord_message *msg = &m->chmsgs[ci][mi];
  ccord_id g = m->channels[ci].guild_id;
  uint64_t need = (msg->author_id == m->self_id) ? CCORD_PERM_SEND_MESSAGES : CCORD_PERM_MANAGE_MESSAGES;
  if (g != CCORD_DM_GUILD_ID && g != CCORD_ID_NONE && (mock_channel_perms(m, g, channel_id) & need) == 0u) {
    return queue_action_refused(m, CCORD_ACTION_SEND, channel_id, message_id, "", 0, nullptr, 0, err);
  }
  if ((ccord_message_flags(msg) & CCORD_MSGFLAG_CROSSPOSTED) != 0u) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message already crossposted");
  }
  ccord_message_rich nr = msg->rich ? *msg->rich : (ccord_message_rich){0};
  nr.present |= CCORD_RICH_HAS_TYPE;   // the merge applies flags with the type part
  nr.flags |= CCORD_MSGFLAG_CROSSPOSTED;
  ccord_message_rich *cloned = ccord_message_rich_clone(&nr, err);
  if (!cloned) return CCORD_ERR_NOMEM;
  retire_rich(m, msg->rich);
  msg->rich = cloned;
  return queue_message_update(m, *msg, now_of(m) + m->cfg.latency_ms, err);
}

// POST /channels/{c}/messages/{m}/ack for the message BEFORE the target: the target and everything after it
// stay unread. When marking the first message of a channel unread, the acked id is `message_id - 1` so every
// stored message remains strictly newer than the read cursor.
static ccord_status mock_mark_unread(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_ACK]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  if (mi == 0 && message_id <= 1u) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: no message before the target");
  uint32_t mentions = 0;
  for (size_t k = (size_t)mi; k < m->chn[ci]; k++) {
    if (m->chmsgs[ci][k].author_id != m->self_id && ccord_message_mentions_user(&m->chmsgs[ci][k], m->self_id)) {
      if (mentions < UINT32_MAX) mentions++;
    }
  }
  mock_item it = make_item(CCORD_NET_MESSAGE_ACK, now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = m->channels[ci].guild_id;
  it.ev.channel_id = channel_id;
  it.ev.message_id = mi > 0 ? m->chmsgs[ci][mi - 1].id : (message_id - 1u);
  it.ev.mention_count = mentions;
  return push_item(m, it, err);
}

static ccord_status mock_toggle_pin(ccord_transport *t, ccord_id channel_id, ccord_id message_id, bool pinned,
                                    ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  m->chmsgs[ci][mi].pinned = pinned;
  mock_item it = make_item(CCORD_NET_MESSAGE_PIN_UPDATE, now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = m->channels[ci].guild_id;
  it.ev.channel_id = channel_id;
  it.ev.message_id = message_id;
  it.ev.pinned = pinned;
  return push_item(m, it, err);
}

static ccord_status mock_request_history_before(ccord_transport *t, ccord_id channel_id, ccord_id before_id,
                                                uint32_t limit, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_HISTORY]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  if (before_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: before id");
  if (limit == 0) limit = MOCK_HISTORY_DEFAULT;
  size_t end = m->chn[ci];
  while (end > 0 && m->chmsgs[ci][end - 1].id >= before_id) end--;
  size_t take = limit < end ? limit : end;
  size_t from = end - take;
  mock_item it = make_item(CCORD_NET_HISTORY, now_of(m) + m->cfg.latency_ms);
  it.ev.channel_id = channel_id;
  it.msgs = ccord_alloc_array(take ? take : 1, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  if (take) memcpy(it.msgs, m->chmsgs[ci] + from, take * sizeof *it.msgs);
  it.ev.messages = it.msgs;
  it.ev.message_count = take;
  return push_item(m, it, err);
}

static ccord_status mock_request_pins_before(ccord_transport *t, ccord_id channel_id, const char *before_cursor,
                                             uint32_t limit, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_PINS]++;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  bool has_before = before_cursor && before_cursor[0] != '\0';
  ccord_id before_id = CCORD_ID_NONE;
  if (has_before && !ccord_id_parse(before_cursor, strlen(before_cursor), &before_id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid pins cursor");
  }
  if (limit == 0) limit = 50;
  if (limit > 50) limit = 50;
  mock_item it = make_item(CCORD_NET_PIN_LIST, now_of(m) + m->cfg.latency_ms);
  it.ev.channel_id = channel_id;
  it.msgs = ccord_alloc_array(limit, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t n = 0;
  bool has_more = false;
  for (size_t i = m->chn[ci]; i > 0; i--) {
    const ccord_message *cand = &m->chmsgs[ci][i - 1];
    if (!cand->pinned) continue;
    if (has_before && cand->id >= before_id) continue;
    if (n < limit) {
      it.msgs[n++] = *cand;
    } else {
      has_more = true;
      break;
    }
  }
  if (n > 0) {
    char cbuf[24];
    int clen = ccord_id_format(it.msgs[n - 1].id, cbuf);
    if (clen > 0) {
      char *ccopy = ccord_strndup(cbuf, (size_t)clen);
      if (ccopy) {
        retire_content(m, ccopy);
        it.ev.pins_next_cursor = ccopy;
      }
    }
  }
  it.ev.messages = it.msgs;
  it.ev.message_count = n;
  it.ev.pins_has_more = has_more;
  it.ev.pins_append = has_before;
  return push_item(m, it, err);
}

static ccord_status mock_request_pins(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  return mock_request_pins_before(t, channel_id, nullptr, limit, err);
}

static ccord_status mock_set_presence_ex(ccord_transport *t, ccord_presence presence, const char *emoji_name,
                                         ccord_id emoji_id, const char *status_text, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (presence > CCORD_PRESENCE_DND) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: presence");
  const char *em = emoji_name ? emoji_name : "";
  const char *st = status_text ? status_text : "";
  size_t em_len = strlen(em);
  size_t st_len = strlen(st);
  if (em_len > CCORD_EMOJI_MAX_BYTES || !ccord_utf8_valid_text(em, em_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid custom status emoji");
  }
  if (st_len > CCORD_ACTIVITY_TEXT_MAX_BYTES || !ccord_utf8_valid_text(st, st_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid custom status text");
  }
  m->users[0].presence = presence;
  m->op3_count++;
  m->op3_presence = presence;
  (void)snprintf(m->op3_text, sizeof m->op3_text, "%s", st);
  if (emoji_id != CCORD_ID_NONE && em_len > 0) {   // the store keeps a custom emoji as name:id, like a gateway presence
    (void)snprintf(m->users[0].custom_status_emoji, sizeof m->users[0].custom_status_emoji, "%.40s:%llu", em,
                   (unsigned long long)emoji_id);
  } else {
    (void)snprintf(m->users[0].custom_status_emoji, sizeof m->users[0].custom_status_emoji, "%s", em);
  }
  (void)snprintf(m->users[0].custom_status_text, sizeof m->users[0].custom_status_text, "%s", st);
  mock_item it = make_item(CCORD_NET_PRESENCE_UPDATE, now_of(m) + m->cfg.latency_ms);
  it.ev.user_id = m->self_id;
  it.ev.presence = presence;
  if (st_len > 0) {
    char *copy = ccord_strndup(st, st_len);
    if (copy) {
      retire_content(m, copy);
      it.ev.status_text = copy;
      m->users[0].status_text = copy;
    }
  } else {
    it.ev.status_text = "";
    m->users[0].status_text = "";
  }
  it.ev.has_presence_detail = true;
  (void)snprintf(it.ev.custom_status_emoji, sizeof it.ev.custom_status_emoji, "%s", m->users[0].custom_status_emoji);
  (void)snprintf(it.ev.custom_status_text, sizeof it.ev.custom_status_text, "%s", m->users[0].custom_status_text);
  it.ev.custom_status_expires_ms = m->users[0].custom_status_expires_ms;
  it.ev.has_activity = m->users[0].has_activity;
  it.ev.activity_type = (ccord_activity_type)m->users[0].activity_type;
  (void)snprintf(it.ev.activity_name, sizeof it.ev.activity_name, "%s", m->users[0].activity_name);
  (void)snprintf(it.ev.activity_details, sizeof it.ev.activity_details, "%s", m->users[0].activity_details);
  (void)snprintf(it.ev.activity_state, sizeof it.ev.activity_state, "%s", m->users[0].activity_state);
  it.ev.activity_start_ms = m->users[0].activity_start_ms;
  it.ev.activity_end_ms = m->users[0].activity_end_ms;
  if (m->own_presence_silent) return CCORD_OK;   // the server does not echo the account's own presence back
  return push_item(m, it, err);
}

static ccord_status mock_set_presence(ccord_transport *t, ccord_presence presence, const char *status_text,
                                      ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (presence > CCORD_PRESENCE_DND) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: presence");
  m->users[0].presence = presence;
  m->op3_count++;
  m->op3_presence = presence;
  (void)snprintf(m->op3_text, sizeof m->op3_text, "%s", status_text ? status_text : "");
  mock_item it = make_item(CCORD_NET_PRESENCE_UPDATE, now_of(m) + m->cfg.latency_ms);
  it.ev.user_id = m->self_id;
  it.ev.presence = presence;
  if (status_text && status_text[0] != '\0') {
    char *copy = ccord_strndup(status_text, strlen(status_text));
    if (copy) {
      retire_content(m, copy);
      it.ev.status_text = copy;
    }
  }
  if (m->own_presence_silent) return CCORD_OK;   // the server does not echo the account's own presence back
  return push_item(m, it, err);
}

// Same contract as the live gateway's ccord_gateway_set_rich_activities.
static void mock_set_self_activity_fields(mock *m, const char *name);

static ccord_status mock_rich_activities_valid(const char *json, size_t len, ccord_error *err)
{
  if (len > CCORD_GATEWAY_RICH_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: rich activities too large");
  }
  ccord_arena a;
  ccord_arena_init(&a, len + 256u);
  const ccord_json *j = ccord_json_parse(&a, json, len, nullptr, err);
  ccord_status st = CCORD_OK;
  if (!j) {
    st = err && err->code != CCORD_OK ? err->code : CCORD_ERR_PARSE;
  } else if (ccord_json_type_of(j) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "mock: rich activities must be an array");
  } else if (ccord_json_array_len(j) > CCORD_GATEWAY_RICH_MAX_ACTIVITIES) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "mock: at most %u rich activities",
                         (unsigned)CCORD_GATEWAY_RICH_MAX_ACTIVITIES);
  } else {
    for (size_t i = 0; i < ccord_json_array_len(j); i++) {
      const ccord_json *e = ccord_json_array_at(j, i);
      const ccord_json *name = ccord_json_get(e, "name");
      const ccord_json *type = ccord_json_get(e, "type");
      int64_t t;
      if (ccord_json_type_of(e) != CCORD_JSON_OBJECT || !name || ccord_json_type_of(name) != CCORD_JSON_STRING ||
          !type || !ccord_json_int64(type, &t)) {
        st = ccord_error_set(err, CCORD_ERR_INVALID, "mock: rich activity needs a name and a type");
        break;
      }
    }
  }
  ccord_arena_free(&a);
  return st;
}

static ccord_status mock_set_rich_activities(ccord_transport *t, const char *json, size_t len, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  char *copy = nullptr;
  if (json && len > 0) {
    ccord_status st = mock_rich_activities_valid(json, len, err);
    if (st != CCORD_OK) return st;
    copy = ccord_malloc(len + 1);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    memcpy(copy, json, len);
    copy[len] = '\0';
  }
  ccord_free(m->rich_json);
  m->rich_json = copy;
  m->rich_len = copy ? len : 0;
  // Like Discord, withdrawing the activities ends the account's Playing line: the next presence echo must not bring it back.
  if (!copy) mock_set_self_activity_fields(m, nullptr);
  return CCORD_OK;
}

static ccord_status mock_set_voice_camera(ccord_transport *t, ccord_id guild_id, bool enabled, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  long vi = mock_voice_index(m, guild_id, m->self_id);
  if (vi < 0 || m->voices[vi].channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not in voice");
  }
  m->voices[vi].self_video = enabled;
  return queue_voice_state(m, &m->voices[vi], now_of(m) + m->cfg.latency_ms, err);
}

static ccord_status mock_send_voice_video(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k,
                                          bool keyframe, ccord_error *err)
{
  mock *m = (mock *)t;
  (void)ts90k;
  (void)keyframe;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty voice video frame");
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].user_id == m->self_id && m->voices[i].channel_id != CCORD_ID_NONE) {
      if (!m->voices[i].self_video) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "mock: voice camera not enabled");
      }
      ccord_error_clear(err);
      return CCORD_OK;
    }
  }
  return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not in voice");
}

static bool mock_consume_keyframe_request(ccord_transport *t)
{
  (void)t;
  return false;
}

static ccord_status mock_select_guild(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (guild_id == CCORD_DM_GUILD_ID) return CCORD_OK;
  for (size_t i = 0; i < m->nguilds; i++) {
    if (m->guilds[i].id == guild_id) return CCORD_OK;
  }
  return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown guild");
}

static ccord_status mock_select_channel(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  (void)channel_id;
  return mock_select_guild(t, guild_id, err);
}

static ccord_status mock_request_members(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, uint32_t chunks,
                                         ccord_error *err)
{
  mock *m = (mock *)t;
  (void)channel_id;
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: member list needs a real guild");
  }
  m->calls[CCORD_MOCK_CALL_MEMBERS]++;
  m->last_member_chunks = chunks;
  return CCORD_OK;
}

static bool has_ci_substr(const char *haystack, const char *needle, size_t needle_len)
{
  if (!haystack || !needle || needle_len == 0) return false;
  size_t hlen = strlen(haystack);
  if (hlen < needle_len) return false;
  for (size_t i = 0; i + needle_len <= hlen; i++) {
    bool match = true;
    for (size_t j = 0; j < needle_len; j++) {
      if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

static const char *infer_mime_type(const char *filename, bool *out_is_image)
{
  if (out_is_image) *out_is_image = false;
  if (!filename) return "application/octet-stream";
  const char *dot = strrchr(filename, '.');
  if (!dot) return "application/octet-stream";
  if (strcmp(dot, ".png") == 0 || strcmp(dot, ".PNG") == 0) {
    if (out_is_image) *out_is_image = true;
    return "image/png";
  }
  if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".JPG") == 0 ||
      strcmp(dot, ".jpeg") == 0 || strcmp(dot, ".JPEG") == 0) {
    if (out_is_image) *out_is_image = true;
    return "image/jpeg";
  }
  if (strcmp(dot, ".webp") == 0 || strcmp(dot, ".WEBP") == 0) {
    if (out_is_image) *out_is_image = true;
    return "image/webp";
  }
  if (strcmp(dot, ".gif") == 0 || strcmp(dot, ".GIF") == 0) {
    if (out_is_image) *out_is_image = true;
    return "image/gif";
  }
  if (strcmp(dot, ".ogg") == 0 || strcmp(dot, ".OGG") == 0) return "audio/ogg";
  return "application/octet-stream";
}

// Retained upload bodies, keyed by the attachment URL the mock answers with. Bounded FIFO: the oldest entry is dropped
// when another upload would exceed either cap. A failed allocation just skips retention; the upload itself still works.
static void upload_cache_remove_at(mock *m, size_t i)
{
  if (i >= m->nuploads) return;
  m->upload_bytes -= m->uploads[i].len;
  ccord_free(m->uploads[i].url);
  ccord_free(m->uploads[i].data);
  memmove(&m->uploads[i], &m->uploads[i + 1], (m->nuploads - i - 1) * sizeof m->uploads[0]);
  m->nuploads--;
}

static void upload_cache_drop_oldest(mock *m)
{
  if (m->nuploads > 0) upload_cache_remove_at(m, 0);
}

static void upload_cache_free(mock *m)
{
  while (m->nuploads > 0) upload_cache_drop_oldest(m);
}

static void upload_cache_add(mock *m, const char *url, const void *data, size_t len)
{
  if (!m || !url || url[0] == '\0' || !data || len == 0 || len > MOCK_UPLOAD_CACHE_BYTES_MAX) return;
  // The mock URL is derived from the filename alone, so a repeat name replaces the earlier body instead of serving stale bytes.
  for (size_t i = 0; i < m->nuploads; i++) {
    if (strcmp(m->uploads[i].url, url) == 0) {
      upload_cache_remove_at(m, i);
      break;
    }
  }
  while (m->nuploads > 0 &&
         (m->nuploads >= MOCK_UPLOAD_CACHE_MAX || m->upload_bytes + len > MOCK_UPLOAD_CACHE_BYTES_MAX)) {
    upload_cache_drop_oldest(m);
  }
  char *url_copy = ccord_strndup(url, strlen(url));
  uint8_t *data_copy = ccord_alloc_array(len, 1);
  if (!url_copy || !data_copy) {
    ccord_free(url_copy);
    ccord_free(data_copy);
    return;
  }
  memcpy(data_copy, data, len);
  m->uploads[m->nuploads++] = (mock_upload){.url = url_copy, .data = data_copy, .len = len};
  m->upload_bytes += len;
}

static const mock_upload *upload_cache_find(const mock *m, const char *url)
{
  for (size_t i = 0; m && url && i < m->nuploads; i++) {
    if (strcmp(m->uploads[i].url, url) == 0) return &m->uploads[i];
  }
  return nullptr;
}

static ccord_status mock_send_message_with_files(ccord_transport *t, ccord_id channel_id,
                                                 ccord_id reply_to_id, bool mention_author,
                                                 const char *utf8, size_t len,
                                                 const ccord_upload_file *files, size_t file_count,
                                                 ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!files || file_count == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: no upload files");
  if (file_count > CCORD_RICH_MAX_ATTACHMENTS) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many files");
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: message length");
  if (len > 0 && (!utf8 || !ccord_utf8_valid_text(utf8, len))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message is not valid text");
  }
  for (size_t i = 0; i < file_count; i++) {
    if (!files[i].filename || files[i].filename[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty filename");
    }
    if (!files[i].data || files[i].size == 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty file data");
    }
  }
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long tmi = -1;
  if (reply_to_id != CCORD_ID_NONE) {
    tmi = find_message(m, ci, reply_to_id);
    if (tmi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown reply target");
  }
  if (m->cfg.refuse_writes) {
    return queue_action_refused(m, CCORD_ACTION_UPLOAD, channel_id, reply_to_id, utf8 ? utf8 : "", len, nullptr, 0, err);
  }
  ccord_status sm_st = CCORD_OK;
  if (mock_slowmode_refused(m, ci, CCORD_ACTION_UPLOAD, reply_to_id, utf8 ? utf8 : "", len, nullptr, 0, &sm_st, err)) return sm_st;

  char wnames[CCORD_RICH_MAX_ATTACHMENTS][256];
  char urls[CCORD_RICH_MAX_ATTACHMENTS][512];
  ccord_attachment atts[CCORD_RICH_MAX_ATTACHMENTS];
  memset(atts, 0, sizeof atts);
  for (size_t i = 0; i < file_count; i++) {
    const char *fn = files[i].filename;
    bool already_spoiler = strncmp(fn, "SPOILER_", 8) == 0;
    if (files[i].spoiler && !already_spoiler) {
      (void)snprintf(wnames[i], sizeof wnames[i], "SPOILER_%.240s", fn);
    } else {
      (void)snprintf(wnames[i], sizeof wnames[i], "%.248s", fn);
    }
    bool is_img = false;
    const char *mime = infer_mime_type(wnames[i], &is_img);
    (void)snprintf(urls[i], sizeof urls[i], "https://cdn.discordapp.com/attachments/mock/%.255s", wnames[i]);
    atts[i] = (ccord_attachment){
      .id = m->next_msg_id + i,
      .filename = wnames[i],
      .description = "",
      .content_type = mime,
      .url = urls[i],
      .proxy_url = "",
      .size = files[i].size,
      .duration_ms = files[i].duration_ms,
      .waveform_b64 = files[i].waveform_b64 ? files[i].waveform_b64 : "",
      .spoiler = files[i].spoiler || already_spoiler,
    };
    // Keep the bytes under the URL the attachment advertises, so fetch_media can play back what was uploaded.
    upload_cache_add(m, urls[i], files[i].data, files[i].size);
  }

  uint32_t msg_flags = 0;
  for (size_t i = 0; i < file_count; i++) {
    if (files[i].duration_ms != 0) msg_flags |= CCORD_MSGFLAG_VOICE_MESSAGE;
  }
  ccord_message_rich rich = {
    .present = CCORD_RICH_ALL, .flags = msg_flags, .attachments = atts, .attachment_count = file_count,
  };
  ccord_id target_author = CCORD_ID_NONE;
  char snippet[CCORD_RICH_SNIPPET_MAX_BYTES + 1] = "";
  if (tmi >= 0) {
    const ccord_message *target = &m->chmsgs[ci][tmi];
    target_author = target->author_id;
    const char *tname = "";
    for (size_t ui = 0; ui < MOCK_NUM_USERS; ui++) {
      if (m->users[ui].id == target_author && m->users[ui].name) tname = m->users[ui].name;
    }
    size_t sn = 0;
    for (const char *tc = target->content ? target->content : ""; *tc;) {
      unsigned char c = (unsigned char)*tc;
      size_t seq = c < 0x80u ? 1u : (c >= 0xF0u ? 4u : (c >= 0xE0u ? 3u : (c >= 0xC0u ? 2u : 1u)));
      if (sn + seq >= sizeof snippet || strlen(tc) < seq) break;
      memcpy(snippet + sn, tc, seq);
      sn += seq;
      tc += seq;
    }
    snippet[sn] = '\0';
    bool ping_target = mention_author && target_author != CCORD_ID_NONE && target_author != m->self_id;
    rich.type = 19;
    rich.mention_users = ping_target ? &target_author : nullptr;
    rich.mention_user_count = ping_target ? 1u : 0u;
    rich.reference = (ccord_msg_reference){.kind = CCORD_REF_REPLY, .message_id = reply_to_id, .channel_id = channel_id};
    rich.ref_preview = (ccord_ref_preview){.present = true, .author_id = target_author, .author_name = tname,
                                           .content = snippet,
                                           .has_attachments = target->rich && target->rich->attachment_count > 0};
  }

  ccord_message msg = store_new_rich(m, ci, m->self_id, utf8 ? utf8 : "", utf8 ? len : 0u, &rich, err, &st);
  if (st != CCORD_OK) return st;
  if (reply_to_id != CCORD_ID_NONE) {
    msg.reply_to_id = reply_to_id;
    if (m->chn[ci] > 0) m->chmsgs[ci][m->chn[ci] - 1].reply_to_id = reply_to_id;
  }
  mock_slowmode_mark_sent(m, channel_id);
  uint64_t now_t = now_of(m);
  uint64_t due_t = now_t + m->cfg.latency_ms;
  st = queue_message_create(m, msg, due_t, err);
  if (st != CCORD_OK) return st;
  if (m->ninflight_uploads >= MOCK_INFLIGHT_UPLOADS_MAX) {
    memmove(&m->inflight_uploads[0], &m->inflight_uploads[1],
            (MOCK_INFLIGHT_UPLOADS_MAX - 1u) * sizeof m->inflight_uploads[0]);
    m->ninflight_uploads = MOCK_INFLIGHT_UPLOADS_MAX - 1u;
  }
  mock_inflight_upload *iu = &m->inflight_uploads[m->ninflight_uploads++];
  memset(iu, 0, sizeof *iu);
  iu->upload_id = ++m->next_upload_id;
  iu->channel_id = channel_id;
  iu->message_id = msg.id;
  iu->file_count = (uint32_t)file_count;
  iu->start_ms = now_t;
  iu->due_ms = due_t;
  for (size_t i = 0; i < file_count; i++) {
    iu->file_sizes[i] = (uint64_t)files[i].size;
    iu->total_bytes += (uint64_t)files[i].size;
    const char *raw_fn = files[i].filename ? files[i].filename : "file";
    const char *sl = strrchr(raw_fn, '/');
    const char *bsl = strrchr(raw_fn, '\\');
    const char *base = raw_fn;
    if (sl && sl + 1 > base) base = sl + 1;
    if (bsl && bsl + 1 > base) base = bsl + 1;
    bool add_sp = files[i].spoiler && strncmp(base, "SPOILER_", 8) != 0;
    (void)snprintf(iu->filenames[i], sizeof iu->filenames[i], "%s%s",
                   add_sp ? "SPOILER_" : "", base[0] ? base : "file");
  }
  return CCORD_OK;
}

static size_t mock_active_uploads(ccord_transport *t, ccord_upload_progress *out, size_t max)
{
  mock *m = (mock *)t;
  if (!m || !m->connected) return 0;
  uint64_t now = now_of(m);
  for (size_t i = 0; i < m->ninflight_uploads; i++) {
    const mock_inflight_upload *iu = &m->inflight_uploads[i];
    if (out && i < max) {
      ccord_upload_progress *p = &out[i];
      memset(p, 0, sizeof *p);
      p->upload_id = iu->upload_id;
      p->channel_id = iu->channel_id;
      p->file_count = iu->file_count > 0 ? iu->file_count : 1u;
      p->total_bytes = iu->total_bytes;
      double frac = 0.0;
      if (iu->due_ms > iu->start_ms && now > iu->start_ms) {
        frac = (double)(now - iu->start_ms) / (double)(iu->due_ms - iu->start_ms);
        if (frac > 1.0) frac = 1.0;
      }
      if (frac < 0.10) {
        p->phase = CCORD_UPLOAD_SLOT;
        p->file_index = 0;
        p->uploaded_bytes = 0;
      } else if (frac >= 0.90) {
        p->phase = CCORD_UPLOAD_FINALIZE;
        p->file_index = p->file_count - 1u;
        p->uploaded_bytes = iu->total_bytes;
      } else {
        p->phase = CCORD_UPLOAD_PUT;
        double put_f = (frac - 0.10) / 0.80;
        uint64_t done = (uint64_t)((double)iu->total_bytes * put_f + 0.5);
        if (done > iu->total_bytes) done = iu->total_bytes;
        p->uploaded_bytes = done;
        uint64_t acc = 0;
        uint32_t fidx = p->file_count - 1u;
        for (uint32_t fi = 0; fi < p->file_count; fi++) {
          acc += iu->file_sizes[fi];
          if (done < acc || fi + 1u == p->file_count) {
            fidx = fi;
            break;
          }
        }
        p->file_index = fidx;
      }
      (void)snprintf(p->filename, sizeof p->filename, "%s", iu->filenames[p->file_index]);
    }
  }
  return m->ninflight_uploads;
}

static ccord_status mock_send_message_with_file(ccord_transport *t, ccord_id channel_id,
                                                const char *utf8, size_t len,
                                                const char *filename, const void *file_data,
                                                size_t file_size, ccord_error *err)
{
  ccord_upload_file f = {.filename = filename, .data = file_data, .size = file_size, .spoiler = false};
  return mock_send_message_with_files(t, channel_id, CCORD_ID_NONE, true, utf8, len, &f, 1, err);
}

// POST a poll: validates exactly like the live body builder, then posts the poll message as the current user. A refused write
// (refuse_writes) fails the call itself with CCORD_ERR_PERMISSION.
static ccord_status mock_create_poll(ccord_transport *t, ccord_id channel_id, const ccord_poll_create *poll,
                                     ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_buf body;
  ccord_buf_init(&body, 0);
  ccord_status st = ccord_poll_build_body(poll, &body, err);
  ccord_buf_free(&body);
  if (st != CCORD_OK) return st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  if (m->cfg.refuse_writes) {   // unlike a text send, refused in the call itself so the poll modal can show the reason
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "You don't have permission to create polls here.");
  }
  if (m->late_poll_fail[0]) {   // accepted by the call, refused later like a Discord 4xx; the poll is never created
    mock_item it = make_item(CCORD_NET_ACTION_FAILED, now_of(m) + m->cfg.latency_ms);
    it.ev.action = CCORD_ACTION_CREATE_POLL;
    it.ev.channel_id = channel_id;
    it.ev.status = CCORD_ERR_PERMISSION;
    it.ev.text = m->late_poll_fail;
    return push_item(m, it, err);
  }
  ccord_message_rich rich = {.present = CCORD_RICH_ALL};
  rich.poll.present = true;
  rich.poll.question = poll->question;
  rich.poll.answer_count = poll->answer_count;
  rich.poll.multiselect = poll->allow_multiselect;
  rich.poll.expiry_ms = message_time(m) + (int64_t)poll->duration_hours * 3600 * 1000;
  for (size_t i = 0; i < poll->answer_count; i++) {
    rich.poll.answers[i] = (ccord_poll_answer){.id = (uint32_t)i + 1u, .text = poll->answers[i], .emoji = ""};
  }
  ccord_message msg = store_new_rich(m, ci, m->self_id, "", 0, &rich, err, &st);
  if (st != CCORD_OK) return st;
  return queue_message_create(m, msg, now_of(m) + m->cfg.latency_ms, err);
}

static ccord_status mock_request_slash_commands(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                                ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_SLASH_INDEX]++;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;

  mock_item it = make_item(CCORD_NET_SLASH_COMMANDS, now_of(m) + m->cfg.latency_ms);
  it.slash_own = ccord_calloc(7, sizeof *it.slash_own);
  if (!it.slash_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");

  ccord_id app_id = m->users[10].id; // Lua Bot (1011)
  size_t count = 0;
  it.slash_own[count++] = (ccord_slash_command){
    .id = 9101, .application_id = app_id, .guild_id = 0, .version = 9101, .command_type = 1,
    .name = "ping", .description = "Check latency with an ephemeral response",
    .default_member_permissions = UINT64_MAX, .dm_permission = true, .option_count = 0,
  };
  it.slash_own[count++] = (ccord_slash_command){
    .id = 9102, .application_id = app_id, .guild_id = 0, .version = 9102, .command_type = 1,
    .name = "echo", .description = "Repeat text publicly or ephemerally",
    .default_member_permissions = UINT64_MAX, .dm_permission = true, .option_count = 2,
    .options = {
      {.kind = CCORD_SLASH_OPT_STRING, .name = "text", .description = "Message text to echo", .required = true},
      {.kind = CCORD_SLASH_OPT_BOOLEAN, .name = "ephemeral", .description = "Only show to you", .required = false},
    },
  };
  it.slash_own[count++] = (ccord_slash_command){
    .id = 9103, .application_id = app_id, .guild_id = 0, .version = 9103, .command_type = 1,
    .name = "topic", .description = "Look up an engineering topic",
    .default_member_permissions = UINT64_MAX, .dm_permission = true, .option_count = 1,
    .options = {
      {.kind = CCORD_SLASH_OPT_STRING, .name = "subject", .description = "Topic name", .required = true, .autocomplete = true},
    },
  };
  if (guild_id != CCORD_DM_GUILD_ID && guild_id != CCORD_ID_NONE) {
    it.slash_own[count++] = (ccord_slash_command){
      .id = 9104, .application_id = app_id, .guild_id = guild_id, .version = 9104, .command_type = 1,
      .name = "purge", .description = "Delete recent messages (moderators only)",
      .default_member_permissions = CCORD_PERM_MANAGE_MESSAGES, .dm_permission = false, .option_count = 1,
      .options = {
        {.kind = CCORD_SLASH_OPT_INTEGER, .name = "count", .description = "How many messages", .required = true},
      },
    };
  }
  it.slash_own[count++] = (ccord_slash_command){
    .id = 9105, .application_id = app_id, .guild_id = 0, .version = 9105, .command_type = 1,
    .name = "config", .description = "Inspect or change bot configuration",
    .default_member_permissions = UINT64_MAX, .dm_permission = true, .option_count = 5,
    .options = {
      {.kind = CCORD_SLASH_OPT_SUB_COMMAND, .name = "show", .description = "Show current settings", .parent_plus_one = 0},
      {.kind = CCORD_SLASH_OPT_SUB_COMMAND_GROUP, .name = "theme", .description = "Manage theme settings", .parent_plus_one = 0},
      {.kind = CCORD_SLASH_OPT_SUB_COMMAND, .name = "set", .description = "Set active theme preset", .parent_plus_one = 2},
      {.kind = CCORD_SLASH_OPT_STRING, .name = "preset", .description = "Theme preset name", .required = true, .parent_plus_one = 3},
      {.kind = CCORD_SLASH_OPT_SUB_COMMAND, .name = "reset", .description = "Reset theme to default", .parent_plus_one = 2},
    },
  };
  it.slash_own[count++] = (ccord_slash_command){
    .id = 9106, .application_id = app_id, .guild_id = 0, .version = 9106, .command_type = 2,
    .name = "UserInfo", .description = "",
    .default_member_permissions = UINT64_MAX, .dm_permission = true, .option_count = 0,
  };
  it.slash_own[count++] = (ccord_slash_command){
    .id = 9107, .application_id = app_id, .guild_id = 0, .version = 9107, .command_type = 3,
    .name = "QuoteMessage", .description = "",
    .default_member_permissions = UINT64_MAX, .dm_permission = true, .option_count = 0,
  };

  it.ev.guild_id = guild_id;
  it.ev.channel_id = channel_id;
  it.ev.slash_commands = it.slash_own;
  it.ev.slash_command_count = count;
  return push_item(m, it, err);
}

static const uint8_t k_mock_png_1x1[67] = {
  0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
  0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x60, 0x00, 0x00, 0x00,
  0x02, 0x00, 0x01, 0xE5, 0x27, 0xDE, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
  0x42, 0x60, 0x82
};

static void ensure_cache_dirs(const char *file_path)
{
  size_t len = strlen(file_path);
  if (len == 0 || len >= 4096) return;
  char tmp[4096];
  memcpy(tmp, file_path, len + 1);
  for (size_t i = 1; i < len; i++) {
    if (tmp[i] != '/') continue;
    tmp[i] = '\0';
    (void)mkdir(tmp, 0700);
    tmp[i] = '/';
  }
}

ccord_status ccord_media_cache_path(const char *url, char *out_path, size_t cap, ccord_error *err)
{
  ccord_error_clear(err);
  if (!url || url[0] == '\0' || !out_path || cap == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "media: invalid cache path arguments");
  }
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
    hash ^= (uint64_t)(*p);
    hash *= 0x100000001b3ULL;
  }
  const char *ext = ".bin";
  size_t qpos = strcspn(url, "?#");
  for (size_t i = qpos; i > 0; i--) {
    if (url[i - 1] == '.') {
      size_t elen = qpos - (i - 1);
      if (elen == 4 && (strncmp(url + i - 1, ".png", 4) == 0 || strncmp(url + i - 1, ".PNG", 4) == 0)) ext = ".png";
      else if (elen == 4 && (strncmp(url + i - 1, ".jpg", 4) == 0 || strncmp(url + i - 1, ".JPG", 4) == 0)) ext = ".jpg";
      else if (elen == 5 && (strncmp(url + i - 1, ".jpeg", 5) == 0 || strncmp(url + i - 1, ".JPEG", 5) == 0)) ext = ".jpeg";
      else if (elen == 5 && (strncmp(url + i - 1, ".webp", 5) == 0 || strncmp(url + i - 1, ".WEBP", 5) == 0)) ext = ".webp";
      else if (elen == 4 && (strncmp(url + i - 1, ".gif", 4) == 0 || strncmp(url + i - 1, ".GIF", 4) == 0)) ext = ".gif";
      break;
    }
    if (url[i - 1] == '/') break;
  }
  char hex[17];
  static const char k_hex[] = "0123456789abcdef";
  for (int i = 15; i >= 0; i--) {
    hex[i] = k_hex[hash & 0xFu];
    hash >>= 4u;
  }
  hex[16] = '\0';

  const char *xdg = getenv("XDG_CACHE_HOME");
  const char *base = nullptr;
  const char *mid = nullptr;
  if (xdg && xdg[0] != '\0') {
    base = xdg;
    mid = "/ccord/media/";
  } else {
    base = getenv("HOME");
    if (!base || base[0] == '\0') base = "/tmp";
    mid = "/.cache/ccord/media/";
  }
  size_t blen = strlen(base);
  while (blen > 1 && base[blen - 1] == '/') blen--;
  size_t mlen = strlen(mid);
  size_t elen = strlen(ext);
  if (blen + mlen + 16u + elen + 1u > cap) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "media: cache path buffer too small");
  }
  memcpy(out_path, base, blen);
  memcpy(out_path + blen, mid, mlen);
  memcpy(out_path + blen + mlen, hex, 16u);
  memcpy(out_path + blen + mlen + 16u, ext, elen + 1u);
  return CCORD_OK;
}

static bool url_has_wav_ext(const char *url)
{
  const char *end = url + strcspn(url, "?#");
  return (size_t)(end - url) >= 4u &&
         (end[-4] == '.') &&
         (end[-3] == 'w' || end[-3] == 'W') &&
         (end[-2] == 'a' || end[-2] == 'A') &&
         (end[-1] == 'v' || end[-1] == 'V');
}

static bool url_has_video_ext(const char *url)
{
  const char *end = url + strcspn(url, "?#");
  size_t len = (size_t)(end - url);
  if (len >= 4u && end[-4] == '.' &&
      (end[-3] == 'm' || end[-3] == 'M') &&
      (end[-2] == 'p' || end[-2] == 'P') &&
      end[-1] == '4') {
    return true;
  }
  return len >= 5u && end[-5] == '.' &&
         (end[-4] == 'w' || end[-4] == 'W') &&
         (end[-3] == 'e' || end[-3] == 'E') &&
         (end[-2] == 'b' || end[-2] == 'B') &&
         (end[-1] == 'm' || end[-1] == 'M');
}

static const uint8_t k_mock_video_gif[] = {
  0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
  0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x08,
  0x05, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x15,
  0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1,
  0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00,
  0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08,
  0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08,
  0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00,
  0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c,
  0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03,
  0x02, 0x00, 0x3b,
};

static size_t build_mock_wav(uint8_t *buf, size_t cap)
{
  const uint32_t rate = 48000u;
  const uint32_t samples = 24000u;   // 500 ms mono S16LE
  const uint32_t data_bytes = samples * 2u;
  if (cap < 44u + data_bytes) return 0;
  uint32_t riff_sz = 36u + data_bytes;
  uint32_t fmt_sz = 16u;
  uint16_t audio_fmt = 1u, channels = 1u, block_align = 2u, bits = 16u;
  uint32_t byte_rate = rate * 2u;
  memcpy(buf + 0, "RIFF", 4);
  memcpy(buf + 4, &riff_sz, 4);
  memcpy(buf + 8, "WAVEfmt ", 8);
  memcpy(buf + 16, &fmt_sz, 4);
  memcpy(buf + 20, &audio_fmt, 2);
  memcpy(buf + 22, &channels, 2);
  memcpy(buf + 24, &rate, 4);
  memcpy(buf + 28, &byte_rate, 4);
  memcpy(buf + 32, &block_align, 2);
  memcpy(buf + 34, &bits, 2);
  memcpy(buf + 36, "data", 4);
  memcpy(buf + 40, &data_bytes, 4);
  for (uint32_t i = 0; i < samples; i++) {
    int16_t v = ((i % 96u) < 48u) ? (int16_t)6000 : (int16_t)-6000;
    memcpy(buf + 44u + i * 2u, &v, 2);
  }
  return 44u + data_bytes;
}

static ccord_status mock_fetch_media(ccord_transport *t, const char *url, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!url || url[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty media url");
  char cache_path[4096];
  ccord_status st = ccord_media_cache_path(url, cache_path, sizeof cache_path, err);
  if (st != CCORD_OK) return st;

  if (m->fail_media[0] && strstr(url, m->fail_media)) {
    // A download that does not arrive: the same failure event the live transport reports.
    char *failed_url = ccord_strndup(url, strlen(url));
    if (!failed_url) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    retire_content(m, failed_url);
    mock_item fit = make_item(CCORD_NET_MEDIA_LOADED, now_of(m) + m->cfg.latency_ms);
    fit.ev.status = CCORD_ERR_IO;
    fit.ev.media_url = failed_url;
    fit.ev.media_path = "";
    return push_item(m, fit, err);
  }

  const mock_upload *uploaded = upload_cache_find(m, url);
  bool is_wav = url_has_wav_ext(url);
  bool is_vid = !is_wav && url_has_video_ext(url);
  size_t payload_len = uploaded ? uploaded->len
                               : (is_wav ? (44u + 24000u * 2u)
                                         : (is_vid ? sizeof k_mock_video_gif : sizeof k_mock_png_1x1));
  uint8_t *bytes = ccord_alloc_array(payload_len, 1);
  char *url_copy = ccord_strndup(url, strlen(url));
  char *path_copy = ccord_strndup(cache_path, strlen(cache_path));
  if (!url_copy || !path_copy || !bytes) {
    ccord_free(url_copy);
    ccord_free(path_copy);
    ccord_free(bytes);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  if (uploaded) memcpy(bytes, uploaded->data, payload_len);
  else if (is_wav) (void)build_mock_wav(bytes, payload_len);
  else if (is_vid) memcpy(bytes, k_mock_video_gif, sizeof k_mock_video_gif);
  else memcpy(bytes, k_mock_png_1x1, sizeof k_mock_png_1x1);

  ensure_cache_dirs(cache_path);
  FILE *fp = fopen(cache_path, "wb");
  if (fp) {
    (void)fwrite(bytes, 1, payload_len, fp);
    fclose(fp);
  }
  retire_content(m, url_copy);
  retire_content(m, path_copy);

  mock_item it = make_item(CCORD_NET_MEDIA_LOADED, now_of(m) + m->cfg.latency_ms);
  it.media_data = bytes;
  it.ev.media_url = url_copy;
  it.ev.media_path = path_copy;
  it.ev.media_data = bytes;
  it.ev.media_len = payload_len;
  return push_item(m, it, err);
}

// ---- Ranged media ----
// Registers a virtual file: `total` bytes synthesised by f->read, so a file of any size needs no buffer. A second
// registration of the same url_part replaces the first and resets its request count.
ccord_status ccord_mock_serve_range_file(ccord_transport *t, const ccord_mock_range_file *f)
{
  mock *m = (mock *)t;
  if (!m || !f || !f->url_part || !f->url_part[0] || !f->read || f->total < 0)
    return CCORD_ERR_INVALID;
  if (strlen(f->url_part) >= MOCK_RANGE_URL_PART_MAX) return CCORD_ERR_INVALID;
  mock_range_slot *slot = nullptr;
  for (size_t i = 0; i < m->nrange_files; i++) {
    if (strcmp(m->range_files[i].url_part, f->url_part) == 0) slot = &m->range_files[i];
  }
  if (!slot) {
    if (m->nrange_files == MOCK_RANGE_FILES_MAX) return CCORD_ERR_NOMEM;
    slot = &m->range_files[m->nrange_files++];
  }
  slot->f = *f;
  snprintf(slot->url_part, sizeof slot->url_part, "%s", f->url_part);
  slot->f.url_part = slot->url_part;
  slot->f.etag[sizeof slot->f.etag - 1] = '\0';
  slot->requests = 0;
  return CCORD_OK;
}

void ccord_mock_range_stats_get(const ccord_transport *t, ccord_mock_range_stats *out)
{
  if (!out) return;
  const mock *m = (const mock *)t;
  if (m) *out = m->range_stats;
  else memset(out, 0, sizeof *out);
}

static void mock_range_log(mock *m, int64_t start, size_t len)
{
  ccord_mock_range_stats *st = &m->range_stats;
  if (st->log_count == MOCK_RANGE_LOG) {   // full: drop the oldest
    memmove(st->log_start, st->log_start + 1, (MOCK_RANGE_LOG - 1u) * sizeof st->log_start[0]);
    memmove(st->log_len, st->log_len + 1, (MOCK_RANGE_LOG - 1u) * sizeof st->log_len[0]);
    st->log_count--;
  }
  st->log_start[st->log_count] = start;
  st->log_len[st->log_count] = (uint32_t)len;
  st->log_count++;
}

// One MEDIA_RANGE answer. Takes ownership of `bytes` (may be null with n == 0).
static ccord_status mock_range_answer(mock *m, const ccord_media_range_req *req, ccord_status status, uint8_t *bytes,
                                      size_t n, int64_t total, uint32_t flags, int32_t http, uint8_t hops,
                                      const char *etag, ccord_error *err)
{
  char *url_copy = ccord_strndup(req->url, strlen(req->url));
  if (!url_copy) {
    ccord_free(bytes);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  retire_content(m, url_copy);
  mock_item it = make_item(CCORD_NET_MEDIA_RANGE, now_of(m) + m->cfg.latency_ms);
  it.media_data = bytes;
  it.ev.status = status;
  it.ev.media_url = url_copy;
  it.ev.media_path = "";
  it.ev.media_data = bytes;
  it.ev.media_len = n;
  it.ev.range_id = req->id;
  it.ev.range_start = req->start;
  it.ev.range_total = total;
  it.ev.range_flags = flags;
  it.ev.range_http = http;
  it.ev.range_redirects = hops;
  if (etag) snprintf(it.ev.range_etag, sizeof it.ev.range_etag, "%s", etag);
  return push_item(m, it, err);
}

static ccord_status mock_fetch_media_range(ccord_transport *t, const ccord_media_range_req *req, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!req || !req->url || !req->url[0]) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty media url");
  if (req->start < 0 || req->len < 1 || req->len > CCORD_STREAM_RANGE_MAX_BYTES)
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad media range");
  // The live gate (ccord_live_user_video_url_allowed) is not linked into the mock; it only insists on https.
  if (strncmp(req->url, "https://", 8) != 0)
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "mock: media url blocked");

  mock_range_slot *slot = nullptr;
  for (size_t i = 0; i < m->nrange_files && !slot; i++) {
    if (strstr(req->url, m->range_files[i].url_part)) slot = &m->range_files[i];
  }
  // Not registered: answered like a server that ignores Range, so the caller falls back to the whole-file path.
  if (!slot)
    return mock_range_answer(m, req, CCORD_OK, nullptr, 0, -1, CCORD_RANGE_UNRANGED, 200, 0, nullptr, err);

  const ccord_mock_range_file *f = &slot->f;
  ccord_mock_range_stats *st = &m->range_stats;
  slot->requests++;
  st->requests++;
  if ((int64_t)req->len > st->max_len) st->max_len = (int64_t)req->len;
  mock_range_log(m, req->start, req->len);

  if (f->flags & CCORD_MOCK_RANGE_REDIRECT_HTTP)   // the chain ends at http://: curl refuses it
    return mock_range_answer(m, req, CCORD_ERR_IO, nullptr, 0, -1, 0, 0, f->redirect_hops, nullptr, err);
  if (f->fail_after_requests && slot->requests >= f->fail_after_requests) {
    ccord_status fs = (f->fail_status == 401 || f->fail_status == 403 || f->fail_status == 410) ? CCORD_ERR_PERMISSION
                                                                                              : CCORD_ERR_IO;
    return mock_range_answer(m, req, fs, nullptr, 0, -1, 0, f->fail_status, f->redirect_hops, nullptr, err);
  }
  const bool unknown = (f->flags & CCORD_MOCK_RANGE_UNKNOWN_TOTAL) != 0;
  const int64_t shown_total = unknown ? -1 : f->total;
  if (f->flags & CCORD_MOCK_RANGE_IGNORE)
    return mock_range_answer(m, req, CCORD_OK, nullptr, 0, shown_total, CCORD_RANGE_UNRANGED, 200, f->redirect_hops,
                             f->etag, err);
  if (req->start >= f->total)   // 416: the start is at or past the end; the total comes from Content-Range: bytes */T
    return mock_range_answer(m, req, CCORD_OK, nullptr, 0, f->total, CCORD_RANGE_ACCEPTS | CCORD_RANGE_EOF, 416,
                             f->redirect_hops, f->etag, err);

  const int64_t left = f->total - req->start;
  const size_t n = left < (int64_t)req->len ? (size_t)left : req->len;
  uint8_t *bytes = ccord_alloc_array(n, 1);
  if (!bytes) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  f->read(f->ud, req->start, bytes, n);
  st->bytes_served += n;
  uint32_t flags = CCORD_RANGE_ACCEPTS;
  // With an unknown total a short body is the only sign of the end.
  if (unknown ? n < req->len : req->start + (int64_t)n == f->total) flags |= CCORD_RANGE_EOF;
  return mock_range_answer(m, req, CCORD_OK, bytes, n, shown_total, flags, 206, f->redirect_hops, f->etag, err);
}

typedef struct mock_search_ud {
  const mock *m;
  ccord_id guild_id;
} mock_search_ud;

static bool mock_search_user_matches(void *ud, ccord_id user_id, const char *needle)
{
  const mock_search_ud *su = ud;
  const mock *m = su->m;
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == user_id) {
      if (ccord_search_contains_ci(m->users[i].name, needle) ||
          ccord_search_contains_ci(m->users[i].handle, needle)) {
        return true;
      }
      break;
    }
  }
  if (su->guild_id != CCORD_ID_NONE && su->guild_id != CCORD_DM_GUILD_ID) {
    for (size_t i = 0; i < m->nmembers; i++) {
      if (m->members[i].guild_id == su->guild_id && m->members[i].user_id == user_id) {
        if (m->members[i].nick[0] != '\0' && ccord_search_contains_ci(m->members[i].nick, needle)) {
          return true;
        }
        break;
      }
    }
  }
  return false;
}

static ccord_status mock_search_messages_ex(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                            const ccord_search_query *query, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_SEARCH]++;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!query) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null search query");
  size_t clen = strlen(query->content);
  if (clen > 0 && !ccord_utf8_valid_text(query->content, clen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid search query");
  }
  ccord_id eff_ch = query->channel_id != CCORD_ID_NONE ? query->channel_id
                    : (query->in_channel[0] != '\0' ? CCORD_ID_NONE : channel_id);
  bool has_any = clen > 0 || query->has_operators || eff_ch != CCORD_ID_NONE;
  if (!has_any) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty search query");

  mock_item it = make_item(CCORD_NET_SEARCH_RESULTS, now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = guild_id;
  it.ev.channel_id = eff_ch;
  it.ev.search_offset = query->offset;
  it.msgs = ccord_alloc_array(CCORD_SEARCH_PAGE_SIZE, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");

  size_t total = 0;
  size_t count = 0;
  for (size_t ci = 0; ci < m->nchannels; ci++) {
    const ccord_channel *ch = &m->channels[ci];
    if (ch->kind != CCORD_CHANNEL_TEXT) continue;
    if (eff_ch != CCORD_ID_NONE && ch->id != eff_ch) continue;
    if (guild_id != CCORD_ID_NONE && ch->guild_id != guild_id) continue;
    mock_search_ud su = {.m = m, .guild_id = ch->guild_id};
    for (size_t mi = m->chn[ci]; mi > 0; mi--) {
      const ccord_message *msg = &m->chmsgs[ci][mi - 1];
      const char *aname = "";
      const char *ahandle = "";
      const char *anick = "";
      for (size_t u = 0; u < MOCK_NUM_USERS; u++) {
        if (m->users[u].id == msg->author_id) {
          aname = m->users[u].name ? m->users[u].name : "";
          ahandle = m->users[u].handle ? m->users[u].handle : "";
          break;
        }
      }
      for (size_t mb = 0; mb < m->nmembers; mb++) {
        if (m->members[mb].guild_id == ch->guild_id && m->members[mb].user_id == msg->author_id) {
          anick = m->members[mb].nick;
          break;
        }
      }
      ccord_search_msg_ctx ctx = {
        .self_id = m->self_id,
        .author_name = aname,
        .author_handle = ahandle,
        .author_nick = anick,
        .channel_name = ch->name ? ch->name : "",
        .user_matches = mock_search_user_matches,
        .ud = &su,
      };
      if (ccord_search_message_matches(query, msg, &ctx)) {
        if (total >= query->offset && count < CCORD_SEARCH_PAGE_SIZE) {
          it.msgs[count++] = *msg;
        }
        total++;
      }
    }
  }
  it.ev.messages = it.msgs;
  it.ev.message_count = count;
  it.ev.search_total_results = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
  it.ev.users = m->users;
  it.ev.user_count = MOCK_NUM_USERS;
  return push_item(m, it, err);
}

static ccord_status mock_search_messages(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                         const char *query, size_t query_len, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_SEARCH]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!query || query_len == 0 || !ccord_utf8_valid_text(query, query_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid search query");
  }
  mock_item it = make_item(CCORD_NET_SEARCH_RESULTS, now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = guild_id;
  it.ev.channel_id = channel_id;
  it.msgs = ccord_alloc_array(50, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t count = 0;
  size_t total = 0;
  for (size_t ci = 0; ci < m->nchannels; ci++) {
    if (channel_id != CCORD_ID_NONE && m->channels[ci].id != channel_id) continue;
    if (guild_id != CCORD_ID_NONE && m->channels[ci].guild_id != guild_id) continue;
    for (size_t mi = m->chn[ci]; mi > 0; mi--) {
      const ccord_message *msg = &m->chmsgs[ci][mi - 1];
      if (has_ci_substr(msg->content, query, query_len)) {
        if (count < 50) it.msgs[count++] = *msg;
        total++;
      }
    }
  }
  it.ev.messages = it.msgs;
  it.ev.message_count = count;
  it.ev.search_total_results = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
  it.ev.users = m->users;
  it.ev.user_count = MOCK_NUM_USERS;
  return push_item(m, it, err);
}

static ccord_status mock_open_dm(ccord_transport *t, ccord_id recipient_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (recipient_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid recipient");

  const char *uname = "Direct Message";
  const char *uavatar = "";
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == recipient_id) {
      uname = m->users[i].name;
      uavatar = m->users[i].avatar_url ? m->users[i].avatar_url : "";
      break;
    }
  }
  ccord_id dm_cid = 900000u + recipient_id;
  size_t existing = SIZE_MAX;
  for (size_t i = 0; i < m->nchannels; i++) {
    if (m->channels[i].id == dm_cid) {
      existing = i;
      break;
    }
  }
  if (existing == SIZE_MAX) {
    size_t n = m->nchannels + 1;
    ccord_channel *nc = ccord_realloc_array(m->channels, n, sizeof *nc);
    ccord_message **nmsgs = ccord_realloc_array(m->chmsgs, n, sizeof *nmsgs);
    size_t *nchn = ccord_realloc_array(m->chn, n, sizeof *nchn);
    size_t *nchcap = ccord_realloc_array(m->chcap, n, sizeof *nchcap);
    if (!nc || !nmsgs || !nchn || !nchcap) {
      if (nc) m->channels = nc;
      if (nmsgs) m->chmsgs = nmsgs;
      if (nchn) m->chn = nchn;
      if (nchcap) m->chcap = nchcap;
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    m->channels = nc;
    m->chmsgs = nmsgs;
    m->chn = nchn;
    m->chcap = nchcap;
    m->channels[m->nchannels] = (ccord_channel){
      .id = dm_cid,
      .guild_id = CCORD_DM_GUILD_ID,
      .parent_id = CCORD_ID_NONE,
      .kind = CCORD_CHANNEL_TEXT,
      .discord_type = CCORD_DISCORD_CHANNEL_DM,
      .name = uname,
      .topic = "Direct Message",
      .user_id = recipient_id,
      .recipient_count = 1u,
      .recipient_ids = {recipient_id},
      .icon_url = uavatar,
    };
    m->chmsgs[m->nchannels] = nullptr;
    m->chn[m->nchannels] = 0;
    m->chcap[m->nchannels] = 0;
    existing = m->nchannels;
    m->nchannels = n;
    m->snap.channels = m->channels;
    m->snap.channel_count = m->nchannels;
  }

  mock_item it = make_item(CCORD_NET_CHANNEL_UPSERT, now_of(m) + m->cfg.latency_ms);
  it.chan = ccord_alloc_array(1, sizeof *it.chan);
  if (!it.chan) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.chan = m->channels[existing];
  it.ev.channel = it.chan;
  it.ev.channel_id = it.chan->id;
  it.ev.guild_id = CCORD_DM_GUILD_ID;
  return push_item(m, it, err);
}

static ccord_status mock_push_voice_pcm(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!pcm && samples > 0) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null pcm");
  size_t elems = samples * 2u;
  if (m->voice_pcm_len + elems > MOCK_VOICE_PCM_CAP) {
    elems = MOCK_VOICE_PCM_CAP - m->voice_pcm_len;
  }
  if (elems > 0) {
    memcpy(m->voice_pcm + m->voice_pcm_len, pcm, elems * sizeof(int16_t));
    m->voice_pcm_len += elems;
  }
  return CCORD_OK;
}

static size_t mock_pull_voice_pcm(ccord_transport *t, int16_t *pcm, size_t max_samples)
{
  mock *m = (mock *)t;
  if (!m || !pcm || max_samples == 0 || m->voice_pcm_len == 0) return 0;
  size_t avail = m->voice_pcm_len / 2u;
  size_t take = avail < max_samples ? avail : max_samples;
  if (take == 0) return 0;
  memcpy(pcm, m->voice_pcm, take * 2u * sizeof(int16_t));
  size_t rem = m->voice_pcm_len - take * 2u;
  if (rem > 0) {
    memmove(m->voice_pcm, m->voice_pcm + take * 2u, rem * sizeof(int16_t));
  }
  m->voice_pcm_len = rem;
  return take;
}

static ccord_status mock_set_relationship(ccord_transport *t, ccord_id user_id, ccord_relationship_kind kind,
                                          ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (user_id == CCORD_ID_NONE || user_id == m->self_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid relationship user id");
  }
  if (kind > CCORD_RELATIONSHIP_PENDING_OUTGOING) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid relationship kind");
  }
  bool user_known = false;
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == user_id) {
      user_known = true;
      break;
    }
  }
  if (!user_known) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown user");

  if (kind == CCORD_RELATIONSHIP_NONE) {
    for (size_t i = 0; i < m->nrelationships; i++) {
      if (m->relationships[i].user_id == user_id) {
        memmove(&m->relationships[i], &m->relationships[i + 1],
                (m->nrelationships - i - 1) * sizeof *m->relationships);
        m->nrelationships--;
        m->snap.relationship_count = m->nrelationships;
        break;
      }
    }
  } else {
    bool updated = false;
    for (size_t i = 0; i < m->nrelationships; i++) {
      if (m->relationships[i].user_id == user_id) {
        m->relationships[i].kind = kind;
        updated = true;
        break;
      }
    }
    if (!updated) {
      if (m->nrelationships >= MOCK_NUM_USERS) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many relationships");
      }
      m->relationships[m->nrelationships++] = (ccord_relationship){.user_id = user_id, .kind = kind};
      m->snap.relationship_count = m->nrelationships;
    }
  }

  mock_item it = make_item(CCORD_NET_RELATIONSHIP_UPDATE, now_of(m) + m->cfg.latency_ms);
  it.ev.user_id = user_id;
  it.ev.relationship_kind = kind;
  return push_item(m, it, err);
}

// Profile extras the mock answers with. Only the second seeded user, Ada (users[1]), carries them, so a test can tell a
// rich profile from a bare one (Chen, users[3], has none). Per-server values apply when the request names the first
// guild (HQ), where Ada is a member, and the per-server theme replaces the user-level one (as the live parser does).
static void mock_profile_extras(const mock *m, ccord_user_profile *p, ccord_id guild_id)
{
  p->has_banner_color = true;
  p->banner_color = 0x2B6CB0u;
  p->has_theme = true;
  p->theme_primary = 0x5865F2u;
  p->theme_accent = 0xEB459Eu;
  p->badge_count = 2;
  (void)snprintf(p->badges[0].id, sizeof p->badges[0].id, "%s", "early_supporter");
  (void)snprintf(p->badges[0].description, sizeof p->badges[0].description, "%s", "Early Supporter");
  (void)snprintf(p->badges[0].icon_url, sizeof p->badges[0].icon_url, "%s",
                 "https://cdn.discordapp.com/badge-icons/0f2a6c1e9b7d4a35.png");
  (void)snprintf(p->badges[1].id, sizeof p->badges[1].id, "%s", "bug_hunter");
  (void)snprintf(p->badges[1].description, sizeof p->badges[1].description, "%s", "Bug Hunter");
  (void)snprintf(p->badges[1].icon_url, sizeof p->badges[1].icon_url, "%s",
                 "https://cdn.discordapp.com/badge-icons/5c8e3b0a2d9f4e71.png");
  p->avatar_decoration_url =
    "https://cdn.discordapp.com/avatar-decoration-presets/9e41c7d2b0a8f356.png?size=160&passthrough=false";
  if (guild_id == CCORD_ID_NONE || guild_id != m->guilds[0].id) return;
  const ccord_member *row = nullptr;
  for (size_t mi = 0; mi < m->nmembers && !row; mi++) {
    if (m->members[mi].guild_id == guild_id && m->members[mi].user_id == p->user_id) row = &m->members[mi];
  }
  if (!row) return;
  p->guild_id = guild_id;
  p->guild_nick = "Ada the Builder";
  p->guild_avatar_url = "https://cdn.discordapp.com/guilds/2001/users/1002/avatars/7d3f0a9e5c21b84f.png?size=64";
  p->guild_banner_url = "https://cdn.discordapp.com/guilds/2001/users/1002/banners/c4b19e2d70a5f368.png?size=64";
  p->guild_bio = "Runs the HQ build bots.";
  p->guild_pronouns = "she/they";
  p->theme_primary = 0xFAA61Au;
  p->theme_accent = 0x3BA55Cu;
  p->guild_joined_at_ms = row->joined_at_ms;
  p->premium_since_ms = row->joined_at_ms + 10ll * 86400000ll;
}

static ccord_status mock_request_profile_ex(ccord_transport *t, ccord_id user_id, ccord_id guild_id, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_PROFILE]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid user id");
  const ccord_user *u = nullptr;
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == user_id) {
      u = &m->users[i];
      break;
    }
  }
  if (!u) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown user");

  mock_item it = make_item(CCORD_NET_USER_PROFILE, now_of(m) + m->cfg.latency_ms);
  it.profile = ccord_alloc_array(1, sizeof *it.profile);
  if (!it.profile) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.profile = (ccord_user_profile){
    .user_id = u->id,
    .display_name = u->name,
    .handle = u->handle,
    .avatar_url = u->avatar_url,
    .banner_url = u->banner_url,
    .accent_color = u->accent_color,
    .bio = u->bio,
    .pronouns = u->pronouns,
    .guild_nick = "",
    .guild_avatar_url = "",
    .guild_banner_url = "",
    .guild_bio = "",
    .guild_pronouns = "",
    .avatar_decoration_url = "",
  };
  (void)snprintf(it.profile->clan_tag, sizeof it.profile->clan_tag, "%s", u->clan_tag);
  it.profile->clan_guild_id = u->clan_guild_id;
  (void)snprintf(it.profile->clan_badge, sizeof it.profile->clan_badge, "%s", u->clan_badge);
  if (u->id == m->users[1].id) mock_profile_extras(m, it.profile, guild_id);
  if (!u->is_bot) {
    size_t cc = 0;
    (void)snprintf(it.profile->connections[cc].type, sizeof it.profile->connections[cc].type, "github");
    (void)snprintf(it.profile->connections[cc].name, sizeof it.profile->connections[cc].name, "%s",
                   u->handle ? u->handle : u->name);
    it.profile->connections[cc].verified = true;
    cc++;
    if ((u->id % 2u) == 0u) {
      (void)snprintf(it.profile->connections[cc].type, sizeof it.profile->connections[cc].type, "website");
      (void)snprintf(it.profile->connections[cc].name, sizeof it.profile->connections[cc].name, "ccord.dev");
      it.profile->connections[cc].verified = true;
      cc++;
    }
    it.profile->connection_count = cc;
  }
  if (u->id != m->self_id) {
    for (size_t gi = 0; gi < m->nguilds && it.profile->mutual_guild_count < CCORD_PROFILE_MAX_MUTUAL_GUILDS; gi++) {
      ccord_id gid = m->guilds[gi].id;
      bool self_in = false, other_in = false;
      for (size_t mi = 0; mi < m->nmembers; mi++) {
        if (m->members[mi].guild_id != gid) continue;
        if (m->members[mi].user_id == m->self_id) self_in = true;
        if (m->members[mi].user_id == u->id) other_in = true;
      }
      if (self_in && other_in) {
        it.profile->mutual_guild_ids[it.profile->mutual_guild_count++] = gid;
      }
    }
    for (size_t ri = 0; ri < m->nrelationships && it.profile->mutual_friend_count < CCORD_PROFILE_MAX_MUTUAL_FRIENDS; ri++) {
      if (m->relationships[ri].kind != CCORD_RELATIONSHIP_FRIEND) continue;
      ccord_id fid = m->relationships[ri].user_id;
      if (fid == u->id || fid == m->self_id) continue;
      for (size_t ui = 0; ui < MOCK_NUM_USERS; ui++) {
        if (m->users[ui].id == fid) {
          ccord_mutual_friend *mf = &it.profile->mutual_friends[it.profile->mutual_friend_count++];
          mf->user_id = fid;
          (void)snprintf(mf->name, sizeof mf->name, "%s", m->users[ui].name ? m->users[ui].name : "");
          (void)snprintf(mf->handle, sizeof mf->handle, "%s", m->users[ui].handle ? m->users[ui].handle : "");
          (void)snprintf(mf->avatar_url, sizeof mf->avatar_url, "%s",
                         m->users[ui].avatar_url ? m->users[ui].avatar_url : "");
          break;
        }
      }
    }
    it.profile->mutual_friend_total = (uint32_t)it.profile->mutual_friend_count;
  }
  it.ev.user_id = u->id;
  it.ev.user_profile = it.profile;
  it.ev.profile = *it.profile;
  it.ev.users = u;
  it.ev.user_count = 1;
  return push_item(m, it, err);
}

static ccord_status mock_request_profile(ccord_transport *t, ccord_id user_id, ccord_error *err)
{
  return mock_request_profile_ex(t, user_id, CCORD_ID_NONE, err);
}

static void pb_varint(ccord_buf *b, uint64_t v)
{
  uint8_t t[10];
  size_t n = 0;
  while (v >= 0x80u) {
    t[n++] = (uint8_t)(v | 0x80u);
    v >>= 7u;
  }
  t[n++] = (uint8_t)v;
  (void)ccord_buf_append(b, t, n);
}

static void pb_u(ccord_buf *b, uint32_t field, uint64_t v)
{
  pb_varint(b, ((uint64_t)field << 3u) | 0u);
  pb_varint(b, v);
}

static void pb_fixed64(ccord_buf *b, uint32_t field, uint64_t v)
{
  pb_varint(b, ((uint64_t)field << 3u) | 1u);
  uint8_t t[8];
  for (unsigned i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8u * i));
  (void)ccord_buf_append(b, t, 8);
}

static void pb_bytes(ccord_buf *b, uint32_t field, const void *p, size_t n)
{
  pb_varint(b, ((uint64_t)field << 3u) | 2u);
  pb_varint(b, n);
  (void)ccord_buf_append(b, p, n);
}

static void pb_str(ccord_buf *b, uint32_t field, const char *s)
{
  pb_bytes(b, field, s, strlen(s));
}

static void pb_gif(ccord_buf *gifs, const char *url, const char *src, uint32_t w, uint32_t h, uint32_t order)
{
  ccord_buf v, e;
  ccord_buf_init(&v, 0);
  ccord_buf_init(&e, 0);
  pb_u(&v, 1, 1);
  pb_str(&v, 2, src);
  pb_u(&v, 3, w);
  pb_u(&v, 4, h);
  pb_u(&v, 5, order);
  pb_str(&e, 1, url);
  pb_bytes(&e, 2, v.data, v.len);
  pb_bytes(gifs, 1, e.data, e.len);
  ccord_buf_free(&v);
  ccord_buf_free(&e);
}

static void pb_emoji_frec(ccord_buf *ef, const char *key, uint32_t score)
{
  ccord_buf v, e;
  ccord_buf_init(&v, 0);
  ccord_buf_init(&e, 0);
  pb_u(&v, 1, 5);
  pb_u(&v, 3, score);
  pb_u(&v, 4, score);
  pb_str(&e, 1, key);
  pb_bytes(&e, 2, v.data, v.len);
  pb_bytes(ef, 1, e.data, e.len);
  ccord_buf_free(&v);
  ccord_buf_free(&e);
}

static void pb_sticker_frec(ccord_buf *sf, uint64_t id, uint32_t score)
{
  ccord_buf v, e;
  ccord_buf_init(&v, 0);
  ccord_buf_init(&e, 0);
  pb_u(&v, 1, 3);
  pb_u(&v, 3, score);
  pb_u(&v, 4, score);
  pb_fixed64(&e, 1, id);
  pb_bytes(&e, 2, v.data, v.len);
  pb_bytes(sf, 1, e.data, e.len);
  ccord_buf_free(&v);
  ccord_buf_free(&e);
}

// {"settings":"<base64 of the raw protobuf>"} — the GET/PATCH /users/@me/settings-proto/2 answer shape.
static void settings_proto_json(ccord_buf *out, const uint8_t *d, size_t n)
{
  static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  (void)ccord_buf_append_str(out, "{\"settings\":\"");
  size_t i = 0;
  for (; i + 2 < n; i += 3) {
    uint32_t v = ((uint32_t)d[i] << 16u) | ((uint32_t)d[i + 1] << 8u) | d[i + 2];
    char q[4] = {k_b64[(v >> 18u) & 63u], k_b64[(v >> 12u) & 63u], k_b64[(v >> 6u) & 63u], k_b64[v & 63u]};
    (void)ccord_buf_append(out, q, 4);
  }
  if (n - i == 1) {
    uint32_t v = (uint32_t)d[i] << 16u;
    char q[4] = {k_b64[(v >> 18u) & 63u], k_b64[(v >> 12u) & 63u], '=', '='};
    (void)ccord_buf_append(out, q, 4);
  } else if (n - i == 2) {
    uint32_t v = ((uint32_t)d[i] << 16u) | ((uint32_t)d[i + 1] << 8u);
    char q[4] = {k_b64[(v >> 18u) & 63u], k_b64[(v >> 12u) & 63u], k_b64[(v >> 6u) & 63u], '='};
    (void)ccord_buf_append(out, q, 4);
  }
  (void)ccord_buf_append_str(out, "\"}");
}

// The seeded FrecencyUserSettings protobuf (favorite GIFs, sticker favorites/frecency, emoji favorites/frecency).
static void build_mock_settings_proto_raw(ccord_buf *raw)
{
  ccord_buf sub;
  ccord_buf_init(raw, 0);

  ccord_buf_init(&sub, 0);
  pb_gif(&sub, "https://tenor.com/view/wumpus-wave-gif-1001",
         "https://media.discordapp.net/attachments/2001/gif_wumpus_wave.gif", 320, 240, 1);
  pb_gif(&sub, "https://tenor.com/view/ccord-ship-it-gif-1002",
         "https://media.discordapp.net/attachments/2001/gif_ship_it.gif", 320, 220, 2);
  pb_bytes(raw, 2, sub.data, sub.len);
  ccord_buf_free(&sub);

  ccord_buf_init(&sub, 0);
  pb_fixed64(&sub, 1, 7001);
  pb_fixed64(&sub, 1, 7002);
  pb_bytes(raw, 3, sub.data, sub.len);
  ccord_buf_free(&sub);

  ccord_buf_init(&sub, 0);
  pb_sticker_frec(&sub, 7001, 90);
  pb_sticker_frec(&sub, 7002, 80);
  pb_sticker_frec(&sub, 7005, 70);
  pb_bytes(raw, 4, sub.data, sub.len);
  ccord_buf_free(&sub);

  ccord_buf_init(&sub, 0);
  pb_str(&sub, 1, "fire");
  pb_str(&sub, 1, "thumbsup");
  pb_str(&sub, 1, "5001");
  pb_str(&sub, 1, "5004");
  pb_bytes(raw, 5, sub.data, sub.len);
  ccord_buf_free(&sub);

  ccord_buf_init(&sub, 0);
  pb_emoji_frec(&sub, "fire", 95);
  pb_emoji_frec(&sub, "thumbsup", 90);
  pb_emoji_frec(&sub, "5001", 88);
  pb_emoji_frec(&sub, "joy", 85);
  pb_emoji_frec(&sub, "heart", 80);
  pb_emoji_frec(&sub, "tada", 75);
  pb_emoji_frec(&sub, "rocket", 70);
  pb_emoji_frec(&sub, "eyes", 65);
  pb_bytes(raw, 6, sub.data, sub.len);
  ccord_buf_free(&sub);
}

static const char k_mock_gif_categories_json[] =
  "{\"categories\":["
  "{\"name\":\"Applause\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_applause.gif\"},"
  "{\"name\":\"Ship It\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_shipit.gif\"},"
  "{\"name\":\"Happy\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_happy.gif\"},"
  "{\"name\":\"High Five\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_highfive.gif\"},"
  "{\"name\":\"Popcorn\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_popcorn.gif\"},"
  "{\"name\":\"Mind Blown\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_mindblown.gif\"},"
  "{\"name\":\"Dance\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_dance.gif\"},"
  "{\"name\":\"Thumbs Up\",\"src\":\"https://media.discordapp.net/attachments/2001/cat_thumbsup.gif\"}"
  "],\"gifs\":["
  "{\"url\":\"https://tenor.com/view/wumpus-wave-gif-1001\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_wumpus_wave.gif\",\"width\":320,\"height\":240},"
  "{\"url\":\"https://tenor.com/view/ccord-ship-it-gif-1002\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_ship_it.gif\",\"width\":320,\"height\":220},"
  "{\"url\":\"https://tenor.com/view/zero-leaks-dance-gif-1003\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_zero_leaks.gif\",\"width\":320,\"height\":260},"
  "{\"url\":\"https://tenor.com/view/party-blob-spin-gif-1004\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_party_blob.gif\",\"width\":320,\"height\":240},"
  "{\"url\":\"https://tenor.com/view/pixel-sunset-vibes-gif-1005\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_pixel_sunset.gif\",\"width\":320,\"height\":200},"
  "{\"url\":\"https://tenor.com/view/lua-rocket-launch-gif-1006\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_lua_rocket.gif\",\"width\":320,\"height\":240}"
  "]}";

static const char k_mock_gif_trending_json[] =
  "["
  "{\"url\":\"https://tenor.com/view/wumpus-wave-gif-1001\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_wumpus_wave.gif\",\"width\":320,\"height\":240},"
  "{\"url\":\"https://tenor.com/view/ccord-ship-it-gif-1002\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_ship_it.gif\",\"width\":320,\"height\":220},"
  "{\"url\":\"https://tenor.com/view/zero-leaks-dance-gif-1003\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_zero_leaks.gif\",\"width\":320,\"height\":260},"
  "{\"url\":\"https://tenor.com/view/party-blob-spin-gif-1004\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_party_blob.gif\",\"width\":320,\"height\":240},"
  "{\"url\":\"https://tenor.com/view/pixel-sunset-vibes-gif-1005\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_pixel_sunset.gif\",\"width\":320,\"height\":200},"
  "{\"url\":\"https://tenor.com/view/lua-rocket-launch-gif-1006\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_lua_rocket.gif\",\"width\":320,\"height\":240}"
  "]";

static const char k_mock_sticker_packs_json[] =
  "{\"sticker_packs\":["
  "{\"id\":\"9001\",\"name\":\"Wumpus & Friends\",\"stickers\":["
  "{\"id\":\"8001\",\"name\":\"WumpusHi\",\"description\":\"Say hello\",\"tags\":\"wave,wumpus\",\"format_type\":1},"
  "{\"id\":\"8002\",\"name\":\"WumpusHype\",\"description\":\"Hyped up\",\"tags\":\"hype,party\",\"format_type\":2},"
  "{\"id\":\"8003\",\"name\":\"WumpusSleep\",\"description\":\"Zzz\",\"tags\":\"sleep,nap\",\"format_type\":1}"
  "]},"
  "{\"id\":\"9002\",\"name\":\"Clyde Classics\",\"stickers\":["
  "{\"id\":\"8004\",\"name\":\"ClydeBot\",\"description\":\"Beep boop\",\"tags\":\"robot,clyde\",\"format_type\":1},"
  "{\"id\":\"8005\",\"name\":\"ClydeThumbsUp\",\"description\":\"Approved\",\"tags\":\"ok,thumbsup\",\"format_type\":1},"
  "{\"id\":\"8006\",\"name\":\"ClydeSparkle\",\"description\":\"Shiny\",\"tags\":\"sparkle,lottie\",\"format_type\":3}"
  "]}"
  "]}";

static ccord_status mock_request_expression_data(ccord_transport *t, ccord_expr_kind kind, const char *arg,
                                                 size_t arg_len, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (kind >= CCORD_EXPR_KIND_COUNT) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad expression kind");
  if (!arg) {
    arg = "";
    arg_len = 0;
  }

  ccord_buf body;
  ccord_buf_init(&body, 0);
  uint8_t *raw_proto_own = nullptr;
  size_t raw_proto_len = 0;
  if (kind == CCORD_EXPR_SETTINGS_PROTO) {
    if (m->has_settings_proto) {
      raw_proto_len = m->settings_proto_len;
      raw_proto_own = ccord_alloc_array(raw_proto_len ? raw_proto_len : 1u, 1);
      if (!raw_proto_own) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      }
      if (raw_proto_len > 0) memcpy(raw_proto_own, m->settings_proto, raw_proto_len);
      settings_proto_json(&body, m->settings_proto, raw_proto_len);
    } else {
      ccord_buf raw;
      build_mock_settings_proto_raw(&raw);
      raw_proto_len = raw.len;
      raw_proto_own = ccord_alloc_array(raw_proto_len ? raw_proto_len : 1u, 1);
      if (!raw_proto_own) {
        ccord_buf_free(&raw);
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      }
      if (raw_proto_len > 0) memcpy(raw_proto_own, raw.data, raw_proto_len);
      settings_proto_json(&body, (const uint8_t *)raw.data, raw.len);
      ccord_buf_free(&raw);
    }
  } else if (kind == CCORD_EXPR_GIF_CATEGORIES) {
    (void)ccord_buf_append_str(&body, k_mock_gif_categories_json);
  } else if (kind == CCORD_EXPR_GIF_TRENDING) {
    (void)ccord_buf_append_str(&body, k_mock_gif_trending_json);
  } else if (kind == CCORD_EXPR_GIF_SEARCH) {
    char clean[64] = {0};
    size_t clen = 0;
    for (size_t i = 0; i < arg_len && clen + 1 < sizeof clean; i++) {
      char c = arg[i];
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
        clean[clen++] = (char)tolower((unsigned char)c);
      } else if (c == ' ') {
        clean[clen++] = '-';
      }
    }
    if (clen == 0) memcpy(clean, "search", 6);
    (void)ccord_buf_appendf(&body,
      "["
      "{\"url\":\"https://tenor.com/view/%s-reaction-gif-2001\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_%s_1.gif\",\"width\":320,\"height\":240},"
      "{\"url\":\"https://tenor.com/view/%s-party-gif-2002\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_%s_2.gif\",\"width\":320,\"height\":220},"
      "{\"url\":\"https://tenor.com/view/%s-epic-gif-2003\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_%s_3.gif\",\"width\":320,\"height\":260},"
      "{\"url\":\"https://tenor.com/view/%s-vibes-gif-2004\",\"gif_src\":\"https://media.discordapp.net/attachments/2001/gif_%s_4.gif\",\"width\":320,\"height\":240}"
      "]",
      clean, clean, clean, clean, clean, clean, clean, clean);
  } else if (kind == CCORD_EXPR_STICKER_PACKS) {
    (void)ccord_buf_append_str(&body, k_mock_sticker_packs_json);
  }

  char *arg_copy = ccord_strndup(arg, arg_len);
  size_t body_len = 0;
  char *body_str = ccord_buf_take(&body, &body_len);
  if (!arg_copy || !body_str) {
    ccord_free(raw_proto_own);
    ccord_free(arg_copy);
    ccord_free(body_str);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  retire_content(m, arg_copy);
  retire_content(m, body_str);

  mock_item it = make_item(CCORD_NET_EXPRESSION_DATA, now_of(m) + m->cfg.latency_ms);
  it.raw_proto_own = raw_proto_own;
  it.ev.status = CCORD_OK;
  it.ev.expr_kind = kind;
  it.ev.expr_arg = arg_copy;
  it.ev.expr_body = body_str;
  it.ev.expr_body_len = body_len;
  it.ev.raw_proto = raw_proto_own;
  it.ev.raw_proto_len = raw_proto_len;
  return push_item(m, it, err);
}

static ccord_status mock_patch_settings_proto(ccord_transport *t, const uint8_t *proto, size_t proto_len,
                                              ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!proto) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null settings proto");
  if (proto_len > MOCK_SETTINGS_PROTO_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: settings proto too large");
  if (m->cfg.refuse_writes) {
    mock_item it = make_item(CCORD_NET_EXPRESSION_DATA, now_of(m) + m->cfg.latency_ms);
    it.ev.status = CCORD_ERR_PERMISSION;
    it.ev.expr_kind = CCORD_EXPR_SETTINGS_PROTO;
    it.ev.text = "You don't have permission to do that. (Missing Permissions)";
    return push_item(m, it, err);
  }
  uint8_t *stored = ccord_alloc_array(proto_len ? proto_len : 1u, 1);
  uint8_t *ev_proto = ccord_alloc_array(proto_len ? proto_len : 1u, 1);
  if (!stored || !ev_proto) {
    ccord_free(stored);
    ccord_free(ev_proto);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  if (proto_len > 0) {
    memcpy(stored, proto, proto_len);
    memcpy(ev_proto, proto, proto_len);
  }
  ccord_buf body;
  ccord_buf_init(&body, 0);
  settings_proto_json(&body, proto, proto_len);
  size_t body_len = 0;
  char *body_str = ccord_buf_take(&body, &body_len);
  char *arg_copy = ccord_strndup("", 0);
  if (!body_str || !arg_copy) {
    ccord_free(stored);
    ccord_free(ev_proto);
    ccord_free(body_str);
    ccord_free(arg_copy);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  ccord_free(m->settings_proto);
  m->settings_proto = stored;
  m->settings_proto_len = proto_len;
  m->has_settings_proto = true;

  retire_content(m, arg_copy);
  retire_content(m, body_str);

  mock_item it = make_item(CCORD_NET_EXPRESSION_DATA, now_of(m) + m->cfg.latency_ms);
  it.raw_proto_own = ev_proto;
  it.ev.status = CCORD_OK;
  it.ev.expr_kind = CCORD_EXPR_SETTINGS_PROTO;
  it.ev.expr_arg = arg_copy;
  it.ev.expr_body = body_str;
  it.ev.expr_body_len = body_len;
  it.ev.raw_proto = ev_proto;
  it.ev.raw_proto_len = proto_len;
  return push_item(m, it, err);
}

// set_status_proto: patch the server's type-1 bytes (the mock is the server, so its bytes are the base a live
// transport would have fetched), record the PATCH, keep the result as the new server state. No echo event: a real
// server's USER_SETTINGS_PROTO_UPDATE for our own write is an echo, never news.
static ccord_status mock_set_status_proto(ccord_transport *t, const ccord_proto_status *st, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!st) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null status");
  if (m->cfg.refuse_writes) return ccord_error_set(err, CCORD_ERR_PERMISSION, "mock: status proto refused");
  uint8_t *out = nullptr;
  size_t out_len = 0;
  ccord_status s = ccord_user_status_patch(m->status1, m->status1_len, st, &out, &out_len, err);
  if (s != CCORD_OK) return s;
  ccord_buf body;
  ccord_buf_init(&body, 0);
  settings_proto_json(&body, out, out_len);
  size_t blen = 0;
  char *body_str = ccord_buf_take(&body, &blen);
  struct mock_status1_patch *grown =
    ccord_realloc_array(m->status1_patches, m->nstatus1_patches + 1u, sizeof *grown);
  if (!body_str || !grown) {
    if (grown) m->status1_patches = grown;
    ccord_free(body_str);
    ccord_free(out);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  m->status1_patches = grown;
  uint8_t *copy = ccord_alloc_array(out_len ? out_len : 1u, 1);
  if (!copy) {
    ccord_free(body_str);
    ccord_free(out);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  if (out_len > 0) memcpy(copy, out, out_len);
  m->status1_patches[m->nstatus1_patches++] = (struct mock_status1_patch){.raw = copy, .len = out_len, .body = body_str};
  ccord_free(m->status1);
  m->status1 = out;
  m->status1_len = out_len;
  return CCORD_OK;
}

static ccord_status mock_send_sticker(ccord_transport *t, ccord_id channel_id, ccord_id sticker_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (sticker_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid sticker id");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;

  const char *sname = "Sticker";
  int sfmt = 1;
  bool found_custom = false;
  for (size_t i = 0; i < m->nstickers; i++) {
    if (m->stickers[i].id == sticker_id) {
      sname = m->stickers[i].name;
      sfmt = (int)m->stickers[i].format;
      found_custom = true;
      break;
    }
  }
  if (!found_custom) {
    if (sticker_id == 8001) { sname = "WumpusHi"; sfmt = 1; }
    else if (sticker_id == 8002) { sname = "WumpusHype"; sfmt = 2; }
    else if (sticker_id == 8003) { sname = "WumpusSleep"; sfmt = 1; }
    else if (sticker_id == 8004) { sname = "ClydeBot"; sfmt = 1; }
    else if (sticker_id == 8005) { sname = "ClydeThumbsUp"; sfmt = 1; }
    else if (sticker_id == 8006) { sname = "ClydeSparkle"; sfmt = 3; }
  }

  ccord_msg_sticker stk = {.id = sticker_id, .name = sname, .format = (uint8_t)sfmt};
  ccord_message_rich rich = {.present = CCORD_RICH_ALL, .stickers = &stk, .sticker_count = 1};
  ccord_message msg = store_new_rich(m, ci, m->self_id, "", 0, &rich, err, &st);
  if (st != CCORD_OK) return st;
  return queue_message_create(m, msg, now_of(m) + m->cfg.latency_ms, err);
}

static bool mock_post_matches_query(const ccord_forum_post *p, const ccord_forum_query *q)
{
  if (p->forum_id != q->forum_id) return false;
  size_t tc = q->tag_count < CCORD_FORUM_MAX_APPLIED_TAGS ? q->tag_count : CCORD_FORUM_MAX_APPLIED_TAGS;
  for (size_t i = 0; i < tc; i++) {
    ccord_id want = q->tag_ids[i];
    if (want == CCORD_ID_NONE) continue;
    bool found = false;
    for (uint8_t k = 0; k < p->applied_tag_count; k++) {
      if (p->applied_tags[k] == want) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  if (q->name && q->name[0] != '\0') {
    size_t nlen = strlen(q->name);
    const char *title = p->title ? p->title : "";
    const char *first = p->first_content ? p->first_content : "";
    if (!has_ci_substr(title, q->name, nlen) && !has_ci_substr(first, q->name, nlen)) {
      return false;
    }
  }
  return true;
}

static size_t mock_find_forum_meta(const mock *m, ccord_id forum_id)
{
  for (size_t i = 0; i < m->nforum_metas; i++) {
    if (m->forum_metas[i].forum_id == forum_id) return i;
  }
  return SIZE_MAX;
}

static ccord_status mock_request_forum_posts(ccord_transport *t, const ccord_forum_query *query, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!query || query->forum_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid forum query");
  }
  if (mock_find_forum_meta(m, query->forum_id) == SIZE_MAX) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown forum channel");
  }
  ccord_id fgid = 2001;
  for (size_t i = 0; i < m->nchannels; i++) {
    if (m->channels[i].id == query->forum_id) {
      fgid = m->channels[i].guild_id;
      break;
    }
  }

  uint32_t limit = query->limit ? query->limit : CCORD_FORUM_PAGE_SIZE;
  size_t total_matches = 0;
  for (size_t i = 0; i < m->nforum_posts; i++) {
    if (mock_post_matches_query(&m->forum_posts[i], query)) total_matches++;
  }

  mock_item it = make_item(CCORD_NET_FORUM_POSTS, now_of(m) + m->cfg.latency_ms);
  it.ev.channel_id = query->forum_id;
  it.ev.guild_id = fgid;
  it.ev.forum_offset = query->offset;
  it.ev.forum_total = (uint32_t)total_matches;

  size_t cap = total_matches > 0 ? total_matches : 1;
  it.fposts = ccord_alloc_array(cap, sizeof *it.fposts);
  if (!it.fposts) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");

  size_t seen = 0;
  size_t out_n = 0;
  for (size_t i = 0; i < m->nforum_posts; i++) {
    if (!mock_post_matches_query(&m->forum_posts[i], query)) continue;
    if (seen >= query->offset && out_n < limit) {
      it.fposts[out_n++] = m->forum_posts[i];
    }
    seen++;
  }
  it.ev.forum_posts = it.fposts;
  it.ev.forum_post_count = out_n;
  it.ev.forum_has_more = (query->offset + out_n) < total_matches;
  return push_item(m, it, err);
}

static ccord_status mock_create_forum_post(ccord_transport *t, ccord_id forum_id,
                                           const char *title, size_t title_len,
                                           const char *content, size_t content_len,
                                           const ccord_id *tag_ids, size_t tag_count,
                                           ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (forum_id == CCORD_ID_NONE || mock_find_forum_meta(m, forum_id) == SIZE_MAX) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown forum channel");
  }
  ccord_id fgid = 2001;
  for (size_t i = 0; i < m->nchannels; i++) {
    if (m->channels[i].id == forum_id) {
      fgid = m->channels[i].guild_id;
      break;
    }
  }
  if (!title || title_len == 0 || title_len > CCORD_FORUM_TITLE_BYTES || !ccord_utf8_valid_text(title, title_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid forum post title");
  }
  if (!content || content_len == 0 || content_len > CCORD_SEND_MAX_BYTES || !ccord_utf8_valid_text(content, content_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid forum post content");
  }
  if (tag_count > CCORD_FORUM_MAX_APPLIED_TAGS || (tag_count > 0 && !tag_ids)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid forum post tags");
  }

  char *title_copy = ccord_strndup(title, title_len);
  if (!title_copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  retire_content(m, title_copy);

  size_t n = m->nchannels + 1;
  ccord_channel *nc = ccord_realloc_array(m->channels, n, sizeof *nc);
  ccord_message **nmsgs = ccord_realloc_array(m->chmsgs, n, sizeof *nmsgs);
  size_t *nchn = ccord_realloc_array(m->chn, n, sizeof *nchn);
  size_t *nchcap = ccord_realloc_array(m->chcap, n, sizeof *nchcap);
  if (!nc || !nmsgs || !nchn || !nchcap) {
    if (nc) m->channels = nc;
    if (nmsgs) m->chmsgs = nmsgs;
    if (nchn) m->chn = nchn;
    if (nchcap) m->chcap = nchcap;
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  m->channels = nc;
  m->chmsgs = nmsgs;
  m->chn = nchn;
  m->chcap = nchcap;
  m->snap.channels = m->channels;

  ccord_id tid = m->next_thread_id++;
  size_t tci = m->nchannels++;
  m->channels[tci] = (ccord_channel){
    .id = tid,
    .guild_id = fgid,
    .parent_id = forum_id,
    .kind = CCORD_CHANNEL_TEXT,
    .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD,
    .name = title_copy,
    .topic = "",
  };
  m->chmsgs[tci] = nullptr;
  m->chn[tci] = 0;
  m->chcap[tci] = 0;

  ccord_status st = CCORD_OK;
  ccord_message msg = store_new(m, tci, m->self_id, content, content_len, err, &st);
  if (st != CCORD_OK) return st;

  ccord_forum_post fp = {
    .thread_id = tid,
    .forum_id = forum_id,
    .guild_id = fgid,
    .owner_id = m->self_id,
    .title = title_copy,
    .message_count = 0,
    .create_ts_ms = msg.timestamp_ms,
    .last_activity_ms = msg.timestamp_ms,
    .last_message_id = msg.id,
    .first_message_id = msg.id,
    .first_author_id = m->self_id,
    .first_content = msg.content,
    .thumbnail_url = "",
    .reaction_emoji = "",
  };
  for (size_t i = 0; i < tag_count; i++) {
    if (tag_ids[i] != CCORD_ID_NONE && fp.applied_tag_count < CCORD_FORUM_MAX_APPLIED_TAGS) {
      fp.applied_tags[fp.applied_tag_count++] = tag_ids[i];
    }
  }

  if (m->nforum_posts == m->capforum_posts) {
    size_t ncap = m->capforum_posts ? m->capforum_posts * 2 : 16;
    ccord_forum_post *g = ccord_realloc_array(m->forum_posts, ncap, sizeof *g);
    if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    m->forum_posts = g;
    m->capforum_posts = ncap;
  }
  m->forum_posts[m->nforum_posts++] = fp;

  mock_item it = make_item(CCORD_NET_FORUM_POST_CREATED, now_of(m) + m->cfg.latency_ms);
  it.ev.channel_id = forum_id;
  it.ev.guild_id = fgid;
  it.chan = ccord_alloc_array(1, sizeof *it.chan);
  it.fposts = ccord_alloc_array(1, sizeof *it.fposts);
  it.msgs = ccord_alloc_array(1, sizeof *it.msgs);
  if (!it.chan || !it.fposts || !it.msgs) {
    ccord_free(it.chan);
    ccord_free(it.fposts);
    ccord_free(it.msgs);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  *it.chan = m->channels[tci];
  *it.fposts = fp;
  *it.msgs = msg;
  it.ev.channel = it.chan;
  it.ev.forum_posts = it.fposts;
  it.ev.forum_post_count = 1;
  it.ev.messages = it.msgs;
  it.ev.message_count = 1;
  return push_item(m, it, err);
}

// ---- server administration: the mock plays a small Discord -----------------------------------------------------------
// Applies the change to the mock's own dataset and emits the events the live server would, so the demo client and the
// UI tests exercise the same path as a real server. refuse_writes makes every request come back as a 403.

static ccord_status admin_reply_failed(mock *m, const ccord_admin_request *req, ccord_status st, const char *text,
                                       ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_ACTION_FAILED, now_of(m) + m->cfg.latency_ms);
  it.ev.action = CCORD_ACTION_ADMIN;
  it.ev.admin_op = req->op;
  it.ev.guild_id = req->guild_id;
  it.ev.channel_id = req->channel_id;
  it.ev.user_id = req->user_id;
  it.ev.expr_id = req->expr_id;
  it.ev.status = st;
  it.ev.text = text;
  return push_item(m, it, err);
}

static const char *mock_user_name(const mock *m, ccord_id id)
{
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == id) return m->users[i].name;
  }
  return "";
}

static const char *admin_owned(mock *m, const char *s)
{
  char *c = ccord_strndup(s, strlen(s));
  if (c) retire_content(m, c);   // lives until destroy, so queued events never point at freed text
  return c;
}

static size_t admin_channel_index(const mock *m, ccord_id id)
{
  for (size_t i = 0; i < m->nchannels; i++) {
    if (m->channels[i].id == id) return i;
  }
  return SIZE_MAX;
}

static long admin_member_index(const mock *m, ccord_id guild_id, ccord_id user_id)
{
  for (size_t i = 0; i < m->nmembers; i++) {
    if (m->members[i].guild_id == guild_id && m->members[i].user_id == user_id) return (long)i;
  }
  return -1;
}

static size_t mock_find_overwrites(const mock *m, ccord_id channel_id)
{
  for (size_t i = 0; i < m->noverwrites; i++) {
    if (m->overwrites[i].channel_id == channel_id) return i;
  }
  return SIZE_MAX;
}

static size_t mock_ensure_overwrites(mock *m, ccord_id channel_id)
{
  size_t oi = mock_find_overwrites(m, channel_id);
  if (oi != SIZE_MAX) return oi;
  if (m->noverwrites >= MOCK_MAX_OVERWRITE_CHANNELS) return SIZE_MAX;
  oi = m->noverwrites++;
  memset(m->overwrite_pool[oi], 0, sizeof m->overwrite_pool[oi]);
  m->overwrites[oi] = (ccord_channel_overwrites){
    .channel_id = channel_id,
    .items = m->overwrite_pool[oi],
    .count = 0u,
  };
  return oi;
}

static void mock_remove_channel_overwrites(mock *m, ccord_id channel_id)
{
  size_t oi = mock_find_overwrites(m, channel_id);
  if (oi == SIZE_MAX) return;
  size_t tail = m->noverwrites - oi - 1u;
  if (tail > 0u) {
    memmove(&m->overwrites[oi], &m->overwrites[oi + 1u], tail * sizeof m->overwrites[0]);
    memmove(&m->overwrite_pool[oi], &m->overwrite_pool[oi + 1u], tail * sizeof m->overwrite_pool[0]);
  }
  m->noverwrites--;
  for (size_t i = oi; i < m->noverwrites; i++) {
    m->overwrites[i].items = m->overwrite_pool[i];
  }
}

static size_t mock_ensure_forum_meta(mock *m, ccord_id forum_id)
{
  size_t fmi = mock_find_forum_meta(m, forum_id);
  if (fmi != SIZE_MAX) return fmi;
  if (m->nforum_metas >= MOCK_MAX_FORUM_METAS) return SIZE_MAX;
  fmi = m->nforum_metas++;
  m->forum_metas[fmi] = (ccord_forum_meta){
    .forum_id = forum_id,
    .default_sort = CCORD_FORUM_SORT_ACTIVITY,
    .tag_count = 0u,
  };
  return fmi;
}

static void mock_remove_forum_meta(mock *m, ccord_id forum_id)
{
  size_t fmi = mock_find_forum_meta(m, forum_id);
  if (fmi == SIZE_MAX) return;
  memmove(&m->forum_metas[fmi], &m->forum_metas[fmi + 1u], (m->nforum_metas - fmi - 1u) * sizeof m->forum_metas[0]);
  m->nforum_metas--;
}

static ccord_status admin_attach_channel_meta(mock *m, mock_item *it, ccord_id cid, ccord_error *err)
{
  size_t oi = mock_find_overwrites(m, cid);
  if (oi != SIZE_MAX) {
    size_t cnt = m->overwrites[oi].count;
    it->overwrites_own = ccord_alloc_array(1u, sizeof *it->overwrites_own);
    it->overwrite_items_own = cnt > 0u ? ccord_alloc_array(cnt, sizeof *it->overwrite_items_own) : nullptr;
    if (!it->overwrites_own || (cnt > 0u && !it->overwrite_items_own)) {
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    if (cnt > 0u) memcpy(it->overwrite_items_own, m->overwrites[oi].items, cnt * sizeof *it->overwrite_items_own);
    it->overwrites_own[0] = (ccord_channel_overwrites){
      .channel_id = cid,
      .items = it->overwrite_items_own,
      .count = cnt,
    };
    it->ev.overwrites = it->overwrites_own;
    it->ev.overwrite_count = 1u;
  }
  size_t fmi = mock_find_forum_meta(m, cid);
  if (fmi != SIZE_MAX) {
    it->forum_metas_own = ccord_alloc_array(1u, sizeof *it->forum_metas_own);
    if (!it->forum_metas_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    it->forum_metas_own[0] = m->forum_metas[fmi];
    it->ev.forum_metas = it->forum_metas_own;
    it->ev.forum_meta_count = 1u;
  }
  return CCORD_OK;
}

static ccord_status admin_emit_channel_op(mock *m, size_t ci, ccord_admin_op op, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_CHANNEL_UPSERT, now_of(m) + m->cfg.latency_ms);
  it.chan = ccord_alloc_array(1, sizeof *it.chan);
  if (!it.chan) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.chan = m->channels[ci];
  it.ev.channel = it.chan;
  it.ev.channel_id = it.chan->id;
  it.ev.guild_id = it.chan->guild_id;
  it.ev.admin_op = op;
  it.ev.users = m->users;
  it.ev.user_count = MOCK_NUM_USERS;
  if (admin_attach_channel_meta(m, &it, it.chan->id, err) != CCORD_OK) {
    item_release(&it);
    return CCORD_ERR_NOMEM;
  }
  return push_item(m, it, err);
}

static ccord_status admin_emit_channel(mock *m, size_t ci, ccord_error *err)
{
  return admin_emit_channel_op(m, ci, CCORD_ADMIN_NONE, err);
}

static ccord_status admin_emit_member(mock *m, size_t mi, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_GUILD_MEMBERS, now_of(m) + m->cfg.latency_ms);
  it.member = ccord_alloc_array(1, sizeof *it.member);
  if (!it.member) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.member = m->members[mi];
  it.ev.guild_id = it.member->guild_id;
  it.ev.guild_members = it.member;
  it.ev.guild_member_count = 1;
  return push_item(m, it, err);
}

// ---- roles, bans, invites and the audit log of the simulated server ----
// The mock answers like Discord does: permission and hierarchy checks use the same engine the client does (as the mock's
// own source of truth, from its own role data), a refusal is a 403 "Missing Permissions", and every change that Discord
// records lands in the audit log.

static const char k_missing_permissions[] = "Missing Permissions";

static size_t mock_role_find(const mock *m, ccord_id guild, ccord_id role)
{
  for (size_t i = 0; i < m->nroles; i++) {
    if (m->roles[i].guild_id == guild && m->roles[i].id == role) return i;
  }
  return SIZE_MAX;
}

// The guild's roles are contiguous; returns the first index and sets *count (0 when the guild has none).
static size_t mock_guild_roles(const mock *m, ccord_id guild, size_t *count)
{
  size_t first = SIZE_MAX, n = 0;
  for (size_t i = 0; i < m->nroles; i++) {
    if (m->roles[i].guild_id != guild) continue;
    if (first == SIZE_MAX) first = i;
    n++;
  }
  *count = n;
  return first == SIZE_MAX ? 0 : first;
}

static mock_mrow *mock_mrow_find(mock *m, ccord_id guild, ccord_id user)
{
  for (size_t i = 0; i < sizeof m->mrows / sizeof m->mrows[0]; i++) {
    if (m->mrows[i].present && m->mrows[i].guild_id == guild && m->mrows[i].user_id == user) return &m->mrows[i];
  }
  return nullptr;
}

static ccord_id mock_guild_owner(const mock *m, ccord_id guild)
{
  for (size_t i = 0; i < m->nguilds; i++) {
    if (m->guilds[i].id == guild) return m->guilds[i].owner_id;
  }
  return CCORD_ID_NONE;
}

static uint64_t mock_self_perms(mock *m, ccord_id guild)
{
  size_t cnt = 0, first = mock_guild_roles(m, guild, &cnt);
  mock_mrow *me = mock_mrow_find(m, guild, m->self_id);
  return ccord_perm_guild(guild, mock_guild_owner(m, guild), m->self_id, cnt ? &m->roles[first] : nullptr, cnt,
                          me ? me->ids : nullptr, me ? me->count : 0);
}

static bool mock_may(mock *m, ccord_id guild, uint64_t need)
{
  return (mock_self_perms(m, guild) & need) == need;
}

static uint64_t mock_channel_perms(mock *m, ccord_id guild, ccord_id channel_id)
{
  uint64_t base = mock_self_perms(m, guild);
  mock_mrow *me = mock_mrow_find(m, guild, m->self_id);
  size_t oi = mock_find_overwrites(m, channel_id);
  const ccord_overwrite *ows = oi != SIZE_MAX ? m->overwrites[oi].items : nullptr;
  size_t nows = oi != SIZE_MAX ? m->overwrites[oi].count : 0u;
  return ccord_perm_channel(base, guild, m->self_id, ows, nows,
                            me ? me->ids : nullptr, me ? me->count : 0u);
}

static bool mock_may_edit_role(mock *m, ccord_id guild, ccord_id role, bool assign)
{
  size_t cnt = 0, first = mock_guild_roles(m, guild, &cnt);
  size_t ri = mock_role_find(m, guild, role);
  mock_mrow *me = mock_mrow_find(m, guild, m->self_id);
  return ccord_perm_can_edit_role(guild, mock_guild_owner(m, guild), m->self_id, mock_self_perms(m, guild),
                                  me ? me->ids : nullptr, me ? me->count : 0, ri == SIZE_MAX ? nullptr : &m->roles[ri],
                                  assign, cnt ? &m->roles[first] : nullptr, cnt) == CCORD_PERM_ALLOWED;
}

// A member's displayed role is their highest one.
static void mock_refresh_display(mock *m, ccord_id guild, ccord_id user)
{
  long mi = admin_member_index(m, guild, user);
  mock_mrow *r = mock_mrow_find(m, guild, user);
  if (mi < 0 || !r) return;
  const ccord_role *top = nullptr;
  for (size_t i = 0; i < r->count; i++) {
    size_t ri = mock_role_find(m, guild, r->ids[i]);
    if (ri != SIZE_MAX && (!top || m->roles[ri].position > top->position)) top = &m->roles[ri];
  }
  ccord_member *mem = &m->members[mi];
  mem->role_name = top ? top->name : "Members";
  mem->role_color = top ? top->color : 0;
  mem->role_rank = !top || top->position < 1 ? 2 : (top->position >= 3 ? 0 : (uint8_t)(3 - top->position));
}

static ccord_status mock_audit_add(mock *m, ccord_id guild, uint16_t action, ccord_id target, const char *target_name,
                                   const char *reason, const char *detail)
{
  if (m->naudit == MOCK_AUDIT_MAX) {
    memmove(&m->audit[0], &m->audit[1], (MOCK_AUDIT_MAX - 1u) * sizeof m->audit[0]);
    m->naudit--;
  }
  uint64_t now = now_of(m);
  ccord_id id = now > MOCK_DISCORD_EPOCH_MS ? (ccord_id)((now - MOCK_DISCORD_EPOCH_MS) << 22) : 0;
  if (id <= m->last_audit_id) id = m->last_audit_id + 1;   // strictly increasing, so "newest first" is well defined
  m->last_audit_id = id;
  const char *tn = admin_owned(m, target_name ? target_name : "");
  const char *rs = admin_owned(m, reason ? reason : "");
  const char *dt = admin_owned(m, detail ? detail : "");
  if (!tn || !rs || !dt) return CCORD_ERR_NOMEM;
  m->audit[m->naudit++] = (mock_audit){guild, {.id = id, .time_ms = (int64_t)now, .user_id = m->self_id,
                                               .user_name = m->users[0].name, .target_id = target, .target_name = tn,
                                               .action = action, .reason = rs, .detail = dt}};
  mock_audit_fill_changes(&m->audit[m->naudit - 1].e, dt);
  return CCORD_OK;
}

static ccord_status admin_emit_roles(mock *m, ccord_net_event_type type, const ccord_id *ids, size_t n, ccord_id guild,
                                     ccord_error *err)
{
  mock_item it = make_item(type, now_of(m) + m->cfg.latency_ms);
  it.roles_own = ccord_alloc_array(n ? n : 1u, sizeof *it.roles_own);
  if (!it.roles_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    size_t ri = mock_role_find(m, guild, ids[i]);
    if (ri != SIZE_MAX) it.roles_own[k++] = m->roles[ri];
  }
  it.ev.roles = it.roles_own;
  it.ev.role_count = k;
  it.ev.guild_id = guild;
  it.ev.role_id = k ? it.roles_own[0].id : CCORD_ID_NONE;
  return push_item(m, it, err);
}

static ccord_status admin_emit_member_roles(mock *m, ccord_id guild, ccord_id user, ccord_id role, bool added,
                                            ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_MEMBER_ROLE_CHANGE, now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = guild;
  it.ev.user_id = user;
  it.ev.role_id = role;
  it.ev.role_added = added;
  ccord_status st = push_item(m, it, err);
  if (st != CCORD_OK) return st;
  long mi = admin_member_index(m, guild, user);
  if (mi < 0) return CCORD_OK;
  // What the gateway's GUILD_MEMBER_UPDATE carries: the member (with the role it now shows) and the full role list.
  mock_item up = make_item(CCORD_NET_GUILD_MEMBERS, now_of(m) + m->cfg.latency_ms);
  up.member = ccord_alloc_array(1, sizeof *up.member);
  if (!up.member) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *up.member = m->members[mi];
  up.ev.guild_id = guild;
  up.ev.guild_members = up.member;
  up.ev.guild_member_count = 1;
  mock_sync_member_roles(m);
  for (size_t i = 0; i < m->nmember_roles; i++) {   // the array never moves, so the pointer stays valid
    if (m->member_roles[i].guild_id == guild && m->member_roles[i].user_id == user) {
      up.ev.member_roles = &m->member_roles[i];
      up.ev.member_role_count = 1;
      break;
    }
  }
  return push_item(m, up, err);
}

static ccord_status admin_emit_bans(mock *m, ccord_id guild, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_BAN_LIST, now_of(m) + m->cfg.latency_ms);
  it.bans_own = ccord_alloc_array(m->nbans ? m->nbans : 1u, sizeof *it.bans_own);
  if (!it.bans_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t k = 0;
  for (size_t i = 0; i < m->nbans; i++) {
    if (m->bans[i].guild_id == guild) it.bans_own[k++] = m->bans[i].ban;
  }
  it.ev.guild_id = guild;
  it.ev.bans = it.bans_own;
  it.ev.ban_count = k;
  return push_item(m, it, err);
}

static ccord_status admin_emit_invites(mock *m, ccord_net_event_type type, ccord_id guild, const char *code,
                                       ccord_error *err)
{
  mock_item it = make_item(type, now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = guild;
  if (type == CCORD_NET_INVITE_DELETE) {
    it.ev.invite_code = code;
    return push_item(m, it, err);
  }
  it.invites_own = ccord_alloc_array(m->ninvites ? m->ninvites : 1u, sizeof *it.invites_own);
  if (!it.invites_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t k = 0;
  for (size_t i = 0; i < m->ninvites; i++) {
    if (m->invites[i].guild_id != guild) continue;
    if (type == CCORD_NET_INVITE_UPSERT && (!code || strcmp(m->invites[i].inv.code, code) != 0)) continue;
    it.invites_own[k++] = m->invites[i].inv;
  }
  it.ev.invites = it.invites_own;
  it.ev.invite_count = k;
  return push_item(m, it, err);
}

// Newest first, at most CCORD_ADMIN_AUDIT_LIMIT, optionally only one actor and / or one action type.
static ccord_status admin_emit_audit(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_AUDIT_LOG, now_of(m) + m->cfg.latency_ms);
  it.audit_own = ccord_alloc_array(m->naudit ? m->naudit : 1u, sizeof *it.audit_own);
  if (!it.audit_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t k = 0;
  for (size_t i = m->naudit; i > 0 && k < CCORD_ADMIN_AUDIT_LIMIT; i--) {
    const mock_audit *a = &m->audit[i - 1];
    if (a->guild_id != req->guild_id) continue;
    if (req->user_id != CCORD_ID_NONE && a->e.user_id != req->user_id) continue;
    if (req->audit_action != 0 && a->e.action != req->audit_action) continue;
    it.audit_own[k++] = a->e;
  }
  it.ev.guild_id = req->guild_id;
  it.ev.audit = it.audit_own;
  it.ev.audit_count = k;
  return push_item(m, it, err);
}

// Appends a channel (and its empty message list) and returns its index, or SIZE_MAX.
static size_t admin_add_channel(mock *m, const ccord_channel *ch, ccord_error *err)
{
  size_t n = m->nchannels + 1;
  ccord_channel *nc = ccord_realloc_array(m->channels, n, sizeof *nc);
  if (nc) m->channels = nc;
  ccord_message **nmsgs = ccord_realloc_array(m->chmsgs, n, sizeof *nmsgs);
  if (nmsgs) m->chmsgs = nmsgs;
  size_t *nchn = ccord_realloc_array(m->chn, n, sizeof *nchn);
  if (nchn) m->chn = nchn;
  size_t *nchcap = ccord_realloc_array(m->chcap, n, sizeof *nchcap);
  if (nchcap) m->chcap = nchcap;
  if (!nc || !nmsgs || !nchn || !nchcap) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    return SIZE_MAX;
  }
  size_t ci = m->nchannels;
  m->channels[ci] = *ch;
  m->chmsgs[ci] = nullptr;
  m->chn[ci] = 0;
  m->chcap[ci] = 0;
  m->nchannels = n;
  m->snap.channels = m->channels;
  if (m->snap.channel_count < m->nchannels) m->snap.channel_count = m->nchannels;
  return ci;
}

static void admin_remove_channel(mock *m, size_t ci)
{
  ccord_id cid = m->channels[ci].id;
  // Retire (not free) the messages' text: an already-queued history event may still point at it.
  for (size_t k = 0; m->chmsgs[ci] && k < m->chn[ci]; k++) {
    retire_content(m, m->chmsgs[ci][k].content);
    retire_rich(m, m->chmsgs[ci][k].rich);
    m->chmsgs[ci][k].content = nullptr;
    m->chmsgs[ci][k].rich = nullptr;
  }
  ccord_free(m->chmsgs[ci]);
  mock_remove_channel_overwrites(m, cid);
  mock_remove_forum_meta(m, cid);
  size_t inv_out = 0u;
  for (size_t i = 0u; i < m->ninvites; i++) {
    if (m->invites[i].inv.channel_id != cid) m->invites[inv_out++] = m->invites[i];
  }
  m->ninvites = inv_out;
  size_t v_out = 0u;
  for (size_t i = 0u; i < m->nvoices; i++) {
    if (m->voices[i].channel_id != cid) m->voices[v_out++] = m->voices[i];
  }
  m->nvoices = v_out;
  m->snap.voice_state_count = m->nvoices;
  size_t stg_out = 0u;
  for (size_t i = 0u; i < m->nstages; i++) {
    if (m->stages[i].channel_id != cid) m->stages[stg_out++] = m->stages[i];
  }
  m->nstages = stg_out;
  if (m->has_stream && m->stream.channel_id == cid) m->has_stream = false;
  size_t tail = m->nchannels - ci - 1;
  memmove(&m->channels[ci], &m->channels[ci + 1], tail * sizeof *m->channels);
  memmove(&m->chmsgs[ci], &m->chmsgs[ci + 1], tail * sizeof *m->chmsgs);
  memmove(&m->chn[ci], &m->chn[ci + 1], tail * sizeof *m->chn);
  memmove(&m->chcap[ci], &m->chcap[ci + 1], tail * sizeof *m->chcap);
  m->nchannels--;
  if (m->snap.channel_count > m->nchannels) m->snap.channel_count = m->nchannels;
}

static void mock_remove_guild(mock *m, size_t gi)
{
  ccord_id gid = m->guilds[gi].id;
  for (size_t ci = m->nchannels; ci > 0; ci--) {
    if (m->channels[ci - 1].guild_id == gid) admin_remove_channel(m, ci - 1);
  }
  size_t m_out = 0;
  for (size_t i = 0; i < m->nmembers; i++) {
    if (m->members[i].guild_id != gid) m->members[m_out++] = m->members[i];
  }
  m->nmembers = m_out;
  m->snap.member_count = m->nmembers;
  for (size_t i = 0; i < sizeof m->mrows / sizeof m->mrows[0]; i++) {
    if (m->mrows[i].guild_id == gid) m->mrows[i].present = false;
  }
  mock_sync_member_roles(m);
  size_t r_out = 0;
  for (size_t i = 0; i < m->nroles; i++) {
    if (m->roles[i].guild_id != gid) m->roles[r_out++] = m->roles[i];
  }
  m->nroles = r_out;
  size_t v_out = 0;
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].guild_id != gid) m->voices[v_out++] = m->voices[i];
  }
  m->nvoices = v_out;
  size_t e_out = 0;
  for (size_t i = 0; i < m->nemojis; i++) {
    if (m->emojis[i].guild_id != gid) m->emojis[e_out++] = m->emojis[i];
  }
  m->nemojis = e_out;
  size_t st_out = 0;
  for (size_t i = 0; i < m->nstickers; i++) {
    if (m->stickers[i].guild_id != gid) m->stickers[st_out++] = m->stickers[i];
  }
  m->nstickers = st_out;
  size_t sb_out = 0;
  for (size_t i = 0; i < m->nsoundboard; i++) {
    if (m->soundboard[i].guild_id != gid) m->soundboard[sb_out++] = m->soundboard[i];
  }
  m->nsoundboard = sb_out;
  size_t ev_out = 0;
  for (size_t i = 0; i < m->nevents; i++) {
    if (m->events[i].guild_id != gid) m->events[ev_out++] = m->events[i];
  }
  m->nevents = ev_out;
  if (m->has_stream && m->stream.guild_id == gid) m->has_stream = false;
  memmove(&m->guilds[gi], &m->guilds[gi + 1], (m->nguilds - gi - 1) * sizeof m->guilds[0]);
  m->nguilds--;
  m->snap.guild_count = m->nguilds;
}

static size_t mock_guild_index(const mock *m, ccord_id gid)
{
  for (size_t i = 0; i < m->nguilds; i++) {
    if (m->guilds[i].id == gid) return i;
  }
  return SIZE_MAX;
}

static ccord_status mock_emit_guild_upsert(mock *m, size_t gi, ccord_admin_op op, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_GUILD_UPSERT, now_of(m) + m->cfg.latency_ms);
  it.guild = ccord_alloc_array(1, sizeof *it.guild);
  if (!it.guild) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.guild = m->guilds[gi];
  it.ev.guild = it.guild;
  it.ev.guild_id = it.guild->id;
  it.ev.admin_op = op;
  return push_item(m, it, err);
}

// Handles the roles / bans / invites / audit-log / server-settings operations. Returns false when `req` is not one of
// them; otherwise *out is the status to hand back to the caller (failures are reported through an ACTION_FAILED event, like Discord).
static const char *mock_jstr(const ccord_json *obj, const char *key, size_t *len)
{
  if (!obj) return nullptr;
  const ccord_json *v = ccord_json_get(obj, key);
  return v ? ccord_json_string(v, len) : nullptr;
}

static bool mock_ji64(const ccord_json *obj, const char *key, int64_t *out)
{
  if (!obj) return false;
  const ccord_json *v = ccord_json_get(obj, key);
  return v ? ccord_json_int64(v, out) : false;
}

static const char **mock_arena_dup_strs(ccord_arena *a, const ccord_json *arr, size_t max_n, size_t *out_n)
{
  *out_n = 0u;
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) return nullptr;
  size_t n = ccord_json_array_len(arr);
  if (n > max_n) n = max_n;
  if (n == 0u) return nullptr;
  const char **list = ccord_arena_alloc(a, n * sizeof *list, alignof(const char *));
  if (!list) return nullptr;
  size_t k = 0u;
  for (size_t i = 0u; i < n; i++) {
    const ccord_json *el = ccord_json_array_at(arr, i);
    size_t slen = 0u;
    const char *s = el ? ccord_json_string(el, &slen) : nullptr;
    if (!s) continue;
    char *dup = ccord_arena_strndup(a, s, slen);
    if (!dup) return nullptr;
    list[k++] = dup;
  }
  *out_n = k;
  return k ? list : nullptr;
}

static size_t mock_json_id_array(const ccord_json *arr, ccord_id *out, size_t cap)
{
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) return 0u;
  size_t n = ccord_json_array_len(arr), k = 0u;
  for (size_t i = 0u; i < n && k < cap; i++) {
    const ccord_json *el = ccord_json_array_at(arr, i);
    size_t slen = 0u;
    const char *s = el ? ccord_json_string(el, &slen) : nullptr;
    ccord_id id = CCORD_ID_NONE;
    if (s && ccord_id_parse(s, slen, &id)) out[k++] = id;
  }
  return k;
}

static bool mock_parse_automod_body(mock *m, const char *body, bool is_create, ccord_automod_rule *io)
{
  ccord_arena tmp;
  ccord_arena_init(&tmp, 4096u);
  ccord_error jerr;
  const ccord_json *root = ccord_json_parse(&tmp, body, strlen(body), nullptr, &jerr);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&tmp);
    return false;
  }
  size_t nlen = 0u;
  const char *nm = mock_jstr(root, "name", &nlen);
  if (nm) {
    char *dup = ccord_arena_strndup(&m->automod_arena, nm, nlen);
    if (!dup) {
      ccord_arena_free(&tmp);
      return false;
    }
    io->name = dup;
  }
  int64_t iv = 0;
  if (mock_ji64(root, "event_type", &iv) && iv > 0 && iv <= 255) {
    io->event_type = (uint8_t)iv;
  } else if (is_create) {
    io->event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND;
  }
  if (is_create && mock_ji64(root, "trigger_type", &iv) && iv > 0 && iv <= 255) {
    io->trigger_type = (uint8_t)iv;
  }
  const ccord_json *en = ccord_json_get(root, "enabled");
  if (en && ccord_json_type_of(en) == CCORD_JSON_BOOL) io->enabled = ccord_json_bool(en, true);

  const ccord_json *meta = ccord_json_get(root, "trigger_metadata");
  if (meta && ccord_json_type_of(meta) == CCORD_JSON_OBJECT) {
    io->keywords = mock_arena_dup_strs(&m->automod_arena, ccord_json_get(meta, "keyword_filter"),
                                       CCORD_AUTOMOD_MAX_KEYWORDS, &io->keyword_count);
    io->regex_patterns = mock_arena_dup_strs(&m->automod_arena, ccord_json_get(meta, "regex_patterns"),
                                             CCORD_AUTOMOD_MAX_REGEX, &io->regex_count);
    io->allow_list = mock_arena_dup_strs(&m->automod_arena, ccord_json_get(meta, "allow_list"),
                                         CCORD_AUTOMOD_MAX_ALLOW_LIST, &io->allow_count);
    const ccord_json *parr = ccord_json_get(meta, "presets");
    if (parr && ccord_json_type_of(parr) == CCORD_JSON_ARRAY) {
      uint32_t mask = 0u;
      for (size_t i = 0u; i < ccord_json_array_len(parr); i++) {
        const ccord_json *pe = ccord_json_array_at(parr, i);
        int64_t pv = 0;
        if (pe && ccord_json_int64(pe, &pv) && pv >= 1 && pv <= 3) {
          mask |= (1u << (uint32_t)(pv - 1));
        }
      }
      io->presets = mask;
    }
    if (mock_ji64(meta, "mention_total_limit", &iv) && iv >= 0 && iv <= 255) {
      io->mention_total_limit = (uint8_t)iv;
    }
    const ccord_json *mrp = ccord_json_get(meta, "mention_raid_protection_enabled");
    if (mrp && ccord_json_type_of(mrp) == CCORD_JSON_BOOL) {
      io->mention_raid_protection = ccord_json_bool(mrp, false);
    }
  }

  const ccord_json *acts = ccord_json_get(root, "actions");
  if (acts && ccord_json_type_of(acts) == CCORD_JSON_ARRAY) {
    io->action_block = false;
    io->custom_message = "";
    io->action_alert = false;
    io->alert_channel_id = CCORD_ID_NONE;
    io->action_timeout = false;
    io->timeout_seconds = 0u;
    io->action_block_interaction = false;
    for (size_t i = 0u; i < ccord_json_array_len(acts); i++) {
      const ccord_json *a = ccord_json_array_at(acts, i);
      if (!a || ccord_json_type_of(a) != CCORD_JSON_OBJECT) continue;
      int64_t at = 0;
      if (!mock_ji64(a, "type", &at)) continue;
      const ccord_json *am = ccord_json_get(a, "metadata");
      if (am && ccord_json_type_of(am) != CCORD_JSON_OBJECT) am = nullptr;
      if (at == CCORD_AUTOMOD_ACTION_BLOCK_MESSAGE) {
        io->action_block = true;
        size_t clen = 0u;
        const char *cm = mock_jstr(am, "custom_message", &clen);
        if (cm && clen > 0u) {
          char *dup = ccord_arena_strndup(&m->automod_arena, cm, clen);
          if (dup) io->custom_message = dup;
        }
      } else if (at == CCORD_AUTOMOD_ACTION_SEND_ALERT_MESSAGE) {
        size_t clen = 0u;
        const char *cs = mock_jstr(am, "channel_id", &clen);
        ccord_id cid = CCORD_ID_NONE;
        if (cs && ccord_id_parse(cs, clen, &cid)) {
          io->action_alert = true;
          io->alert_channel_id = cid;
        }
      } else if (at == CCORD_AUTOMOD_ACTION_TIMEOUT) {
        int64_t dur = 0;
        if (mock_ji64(am, "duration_seconds", &dur) && dur > 0) {
          io->action_timeout = true;
          io->timeout_seconds = (uint32_t)dur;
        }
      } else if (at == CCORD_AUTOMOD_ACTION_BLOCK_MEMBER_INTERACTION) {
        io->action_block_interaction = true;
      }
    }
  }

  const ccord_json *er = ccord_json_get(root, "exempt_roles");
  if (er && ccord_json_type_of(er) == CCORD_JSON_ARRAY) {
    io->exempt_role_count = mock_json_id_array(er, io->exempt_role_ids, CCORD_AUTOMOD_MAX_EXEMPT_ROLES);
  }
  const ccord_json *ec = ccord_json_get(root, "exempt_channels");
  if (ec && ccord_json_type_of(ec) == CCORD_JSON_ARRAY) {
    io->exempt_channel_count = mock_json_id_array(ec, io->exempt_channel_ids, CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS);
  }
  ccord_arena_free(&tmp);
  return true;
}

static const char *mock_arena_dup_cstr(ccord_arena *a, const char *s, size_t len)
{
  if (!s) return "";
  char *d = ccord_arena_strndup(a, s, len);
  return d ? d : "";
}

static bool mock_parse_welcome_body(mock *m, ccord_id g, const char *body, ccord_welcome_screen *out_ws,
                                    ccord_welcome_channel out_chs[CCORD_WELCOME_MAX_CHANNELS])
{
  ccord_arena tmp;
  ccord_arena_init(&tmp, 4096u);
  ccord_error jerr;
  const ccord_json *root = ccord_json_parse(&tmp, body, strlen(body), nullptr, &jerr);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&tmp);
    return false;
  }
  *out_ws = (ccord_welcome_screen){.description = ""};
  const ccord_json *enj = ccord_json_get(root, "enabled");
  out_ws->enabled = (enj && ccord_json_type_of(enj) == CCORD_JSON_BOOL) ? ccord_json_bool(enj, false) : true;
  size_t dlen = 0u;
  const char *desc = mock_jstr(root, "description", &dlen);
  out_ws->description = mock_arena_dup_cstr(&m->automod_arena, desc, dlen);
  const ccord_json *carr = ccord_json_get(root, "welcome_channels");
  if (carr && ccord_json_type_of(carr) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(carr);
    if (n > CCORD_WELCOME_MAX_CHANNELS) n = CCORD_WELCOME_MAX_CHANNELS;
    size_t k = 0u;
    for (size_t i = 0u; i < n; i++) {
      const ccord_json *cj = ccord_json_array_at(carr, i);
      if (!cj || ccord_json_type_of(cj) != CCORD_JSON_OBJECT) continue;
      size_t cslen = 0u;
      const char *cs = mock_jstr(cj, "channel_id", &cslen);
      ccord_id cid = CCORD_ID_NONE;
      if (!cs || !ccord_id_parse(cs, cslen, &cid) || cid == CCORD_ID_NONE) {
        ccord_arena_free(&tmp);
        return false;
      }
      size_t chi = admin_channel_index(m, cid);
      if (chi == SIZE_MAX || m->channels[chi].guild_id != g || m->channels[chi].kind == CCORD_CHANNEL_CATEGORY) {
        ccord_arena_free(&tmp);
        return false;
      }
      size_t cdlen = 0u, emlen = 0u, eidlen = 0u;
      const char *cd = mock_jstr(cj, "description", &cdlen);
      const char *em = mock_jstr(cj, "emoji_name", &emlen);
      const char *eids = mock_jstr(cj, "emoji_id", &eidlen);
      ccord_id eid = CCORD_ID_NONE;
      if (eids) (void)ccord_id_parse(eids, eidlen, &eid);
      out_chs[k++] = (ccord_welcome_channel){
        .channel_id = cid,
        .description = mock_arena_dup_cstr(&m->automod_arena, cd, cdlen),
        .emoji_id = eid,
        .emoji_name = mock_arena_dup_cstr(&m->automod_arena, em, emlen),
      };
    }
    out_ws->channels = k > 0u ? out_chs : nullptr;
    out_ws->channel_count = k;
  }
  ccord_arena_free(&tmp);
  return true;
}

static bool mock_parse_onboarding_body(mock *m, ccord_id g, const char *body, ccord_onboarding *out_ob)
{
  ccord_arena tmp;
  ccord_arena_init(&tmp, 4096u);
  ccord_error jerr;
  const ccord_json *root = ccord_json_parse(&tmp, body, strlen(body), nullptr, &jerr);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&tmp);
    return false;
  }
  *out_ob = (ccord_onboarding){.guild_id = g};
  const ccord_json *enj = ccord_json_get(root, "enabled");
  out_ob->enabled = (enj && ccord_json_type_of(enj) == CCORD_JSON_BOOL) ? ccord_json_bool(enj, false) : false;
  int64_t mode = 0;
  if (mock_ji64(root, "mode", &mode) && mode == 1) out_ob->mode = CCORD_ONBOARDING_ADVANCED;
  const ccord_json *dc = ccord_json_get(root, "default_channel_ids");
  if (dc && ccord_json_type_of(dc) == CCORD_JSON_ARRAY) {
    out_ob->default_channel_count = mock_json_id_array(dc, out_ob->default_channel_ids, CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS);
    for (size_t i = 0u; i < out_ob->default_channel_count; i++) {
      size_t chi = admin_channel_index(m, out_ob->default_channel_ids[i]);
      if (chi == SIZE_MAX || m->channels[chi].guild_id != g || m->channels[chi].kind == CCORD_CHANNEL_CATEGORY) {
        ccord_arena_free(&tmp);
        return false;
      }
    }
  }
  const ccord_json *parr = ccord_json_get(root, "prompts");
  if (parr && ccord_json_type_of(parr) == CCORD_JSON_ARRAY) {
    size_t pn = ccord_json_array_len(parr);
    if (pn > CCORD_ONBOARDING_MAX_PROMPTS) pn = CCORD_ONBOARDING_MAX_PROMPTS;
    ccord_onboarding_prompt *prompts =
      pn > 0u ? ccord_arena_alloc(&m->automod_arena, pn * sizeof *prompts, alignof(ccord_onboarding_prompt)) : nullptr;
    size_t pk = 0u;
    for (size_t i = 0u; prompts && i < pn; i++) {
      const ccord_json *pj = ccord_json_array_at(parr, i);
      if (!pj || ccord_json_type_of(pj) != CCORD_JSON_OBJECT) continue;
      ccord_onboarding_prompt pr = {0};
      size_t pidlen = 0u, ptlen = 0u;
      const char *pids = mock_jstr(pj, "id", &pidlen);
      ccord_id pid = CCORD_ID_NONE;
      if (pids && ccord_id_parse(pids, pidlen, &pid) && pid >= 1000u) pr.id = pid;
      else pr.id = m->next_onboarding_id++;
      int64_t ptype = 0;
      if (mock_ji64(pj, "type", &ptype) && ptype == 1) pr.type = CCORD_ONBOARDING_DROPDOWN;
      const char *pt = mock_jstr(pj, "title", &ptlen);
      if (!pt || ptlen == 0u) continue;
      pr.title = mock_arena_dup_cstr(&m->automod_arena, pt, ptlen);
      const ccord_json *ssj = ccord_json_get(pj, "single_select");
      const ccord_json *rqj = ccord_json_get(pj, "required");
      const ccord_json *ioj = ccord_json_get(pj, "in_onboarding");
      pr.single_select = ssj && ccord_json_bool(ssj, false);
      pr.required = rqj && ccord_json_bool(rqj, false);
      pr.in_onboarding = !ioj || ccord_json_bool(ioj, true);
      const ccord_json *oarr = ccord_json_get(pj, "options");
      if (oarr && ccord_json_type_of(oarr) == CCORD_JSON_ARRAY) {
        size_t on = ccord_json_array_len(oarr);
        if (on > CCORD_ONBOARDING_MAX_OPTIONS) on = CCORD_ONBOARDING_MAX_OPTIONS;
        ccord_onboarding_option *opts =
          on > 0u ? ccord_arena_alloc(&m->automod_arena, on * sizeof *opts, alignof(ccord_onboarding_option)) : nullptr;
        size_t ok = 0u;
        for (size_t j = 0u; opts && j < on; j++) {
          const ccord_json *oj = ccord_json_array_at(oarr, j);
          if (!oj || ccord_json_type_of(oj) != CCORD_JSON_OBJECT) continue;
          ccord_onboarding_option opt = {0};
          size_t oidlen = 0u, otlen = 0u, odlen = 0u, oelen = 0u, oeidlen = 0u;
          const char *oids = mock_jstr(oj, "id", &oidlen);
          ccord_id oid = CCORD_ID_NONE;
          if (oids && ccord_id_parse(oids, oidlen, &oid) && oid >= 1000u) opt.id = oid;
          else opt.id = m->next_onboarding_id++;
          const char *ot = mock_jstr(oj, "title", &otlen);
          if (!ot || otlen == 0u) continue;
          opt.title = mock_arena_dup_cstr(&m->automod_arena, ot, otlen);
          const char *od = mock_jstr(oj, "description", &odlen);
          opt.description = mock_arena_dup_cstr(&m->automod_arena, od, odlen);
          const char *oe = mock_jstr(oj, "emoji_name", &oelen);
          opt.emoji_name = mock_arena_dup_cstr(&m->automod_arena, oe, oelen);
          const char *oeids = mock_jstr(oj, "emoji_id", &oeidlen);
          if (oeids) (void)ccord_id_parse(oeids, oeidlen, &opt.emoji_id);
          opt.channel_count = mock_json_id_array(ccord_json_get(oj, "channel_ids"), opt.channel_ids,
                                                 CCORD_ONBOARDING_MAX_OPT_CHANNELS);
          opt.role_count = mock_json_id_array(ccord_json_get(oj, "role_ids"), opt.role_ids,
                                              CCORD_ONBOARDING_MAX_OPT_ROLES);
          for (size_t k = 0u; k < opt.channel_count; k++) {
            size_t chi = admin_channel_index(m, opt.channel_ids[k]);
            if (chi == SIZE_MAX || m->channels[chi].guild_id != g) {
              ccord_arena_free(&tmp);
              return false;
            }
          }
          opts[ok++] = opt;
        }
        pr.options = ok > 0u ? opts : nullptr;
        pr.option_count = ok;
      }
      if (pr.option_count > 0u) prompts[pk++] = pr;
    }
    out_ob->prompts = pk > 0u ? prompts : nullptr;
    out_ob->prompt_count = pk;
  }
  ccord_arena_free(&tmp);
  return true;
}

static bool mock_member_prunable(mock *m, ccord_id g, size_t mi, uint8_t days, const ccord_id *inc_roles, size_t ninc)
{
  const ccord_member *mem = &m->members[mi];
  if (mem->guild_id != g || mem->user_id == m->self_id || mem->user_id == mock_guild_owner(m, g)) return false;
  const ccord_user *u = nullptr;
  for (size_t i = 0u; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == mem->user_id) { u = &m->users[i]; break; }
  }
  if (!u || u->is_bot || u->presence != CCORD_PRESENCE_OFFLINE) return false;
  int64_t ref_now = (int64_t)now_of(m) >= (int64_t)MOCK_DISCORD_EPOCH_MS ? (int64_t)now_of(m) : m->cfg.base_time_ms;
  if (mem->joined_at_ms > 0 && (ref_now - mem->joined_at_ms) < (int64_t)days * 86400000ll) return false;
  mock_mrow *row = mock_mrow_find(m, g, mem->user_id);
  if (!row || !row->present) return false;
  for (size_t i = 0u; i < row->count; i++) {
    bool allowed = false;
    for (size_t j = 0u; j < ninc; j++) {
      if (inc_roles[j] == row->ids[i]) { allowed = true; break; }
    }
    if (!allowed) return false;
  }
  return true;
}

// ---- M5 admin ops: voice-channel status & stage moderation ----------

static bool admin_m5_voice_status(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  size_t ci = admin_channel_index(m, req->channel_id);
  if (ci == SIZE_MAX) {
    *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "It no longer exists.", err);
    return true;
  }
  ccord_id g = m->channels[ci].guild_id;
  if (g == CCORD_DM_GUILD_ID ||
      (m->channels[ci].kind != CCORD_CHANNEL_VOICE && m->channels[ci].kind != CCORD_CHANNEL_STAGE)) {
    *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "mock: not a voice channel", err);
    return true;
  }
  if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_SET_VOICE_CHANNEL_STATUS) == 0u) {
    *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  *out = queue_voice_status(m, g, req->channel_id, req->voice_status, now_of(m) + m->cfg.latency_ms, err);
  return true;
}

static bool admin_m5_stage_request_speak(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  ccord_id g = req->guild_id;
  if (mock_guild_index(m, g) == SIZE_MAX) {
    *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
    return true;
  }
  size_t ci = admin_channel_index(m, req->channel_id);
  if (ci == SIZE_MAX || m->channels[ci].guild_id != g || m->channels[ci].kind != CCORD_CHANNEL_STAGE) {
    *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "mock: not a stage channel of the guild", err);
    return true;
  }
  if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_REQUEST_TO_SPEAK) == 0u) {
    *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  long vi = mock_voice_index(m, g, m->self_id);
  if (vi < 0 || m->voices[vi].channel_id != req->channel_id) {
    *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "mock: join the stage before asking to speak", err);
    return true;
  }
  if (req->request_to_speak_ms != 0 && !m->voices[vi].suppress) {
    *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "mock: already a stage speaker", err);
    return true;
  }
  m->voices[vi].request_to_speak_timestamp = req->request_to_speak_ms;
  *out = queue_voice_state(m, &m->voices[vi], now_of(m) + m->cfg.latency_ms, err);
  return true;
}

static bool admin_m5_stage_set_speaker(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  ccord_id g = req->guild_id;
  if (mock_guild_index(m, g) == SIZE_MAX) {
    *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
    return true;
  }
  size_t ci = admin_channel_index(m, req->channel_id);
  if (ci == SIZE_MAX || m->channels[ci].guild_id != g || m->channels[ci].kind != CCORD_CHANNEL_STAGE) {
    *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "mock: not a stage channel of the guild", err);
    return true;
  }
  // Discord answers 403 before it looks at the target's state.
  if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_MUTE_MEMBERS) == 0u) {
    *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  long mi = admin_member_index(m, g, req->user_id);
  if (mi < 0) {
    *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Member", err);
    return true;
  }
  long vi = mock_voice_index(m, g, req->user_id);
  if (vi < 0 || m->voices[vi].channel_id != req->channel_id) {
    *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Target user is not connected to voice.", err);
    return true;
  }
  char det[48];
  (void)snprintf(det, sizeof det, "suppress: %s -> %s", m->voices[vi].suppress ? "true" : "false",
                 req->stage_speaker ? "false" : "true");
  m->voices[vi].suppress = !req->stage_speaker;
  if (req->stage_speaker) m->voices[vi].request_to_speak_timestamp = 0;   // the invite answers the ask
  *out = mock_audit_add(m, g, 24, req->user_id, mock_user_name(m, req->user_id), req->reason, det);
  if (*out == CCORD_OK) *out = queue_voice_state(m, &m->voices[vi], now_of(m) + m->cfg.latency_ms, err);
  return true;
}

static bool admin_m5_stage_topic(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  size_t ci = admin_channel_index(m, req->channel_id);
  long si = mock_stage_index(m, req->channel_id);
  if (ci == SIZE_MAX || m->channels[ci].kind != CCORD_CHANNEL_STAGE || si < 0) {
    *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "It no longer exists.", err);
    return true;
  }
  ccord_id g = m->channels[ci].guild_id;
  if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_MUTE_MEMBERS) == 0u) {
    *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  const char *topic = req->has_topic ? req->topic : "";
  // The model's inline topic is byte-sized: refuse what cannot be stored whole rather than cut a
  // multibyte character in half (the builder already enforced the 120-character limit).
  if (strlen(topic) >= sizeof m->stages[si].topic) {
    *out = admin_reply_failed(m, req, CCORD_ERR_RANGE, "topic is too long", err);
    return true;
  }
  char old_topic[sizeof m->stages[si].topic];
  memcpy(old_topic, m->stages[si].topic, sizeof old_topic);
  size_t tn = strlen(topic);   // the bounds check above guarantees tn + 1 fits
  memcpy(m->stages[si].topic, topic, tn + 1u);
  char det[288];
  (void)snprintf(det, sizeof det, "topic: %s -> %s", old_topic, m->stages[si].topic);
  *out = mock_audit_add(m, g, 84, req->channel_id, m->channels[ci].name, req->reason, det);
  if (*out == CCORD_OK) *out = queue_stage_instance(m, &m->stages[si], false, now_of(m) + m->cfg.latency_ms, err);
  return true;
}

static bool admin_m5_stage_create(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  size_t ci = admin_channel_index(m, req->channel_id);
  if (ci == SIZE_MAX || m->channels[ci].kind != CCORD_CHANNEL_STAGE ||
      (req->guild_id != CCORD_ID_NONE && m->channels[ci].guild_id != req->guild_id)) {
    *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "It no longer exists.", err);
    return true;
  }
  ccord_id g = m->channels[ci].guild_id;
  if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_MUTE_MEMBERS) == 0u) {
    *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  if (mock_stage_index(m, req->channel_id) >= 0) {
    *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "A Stage Instance already exists for this channel.", err);
    return true;
  }
  const char *topic = req->has_topic ? req->topic : "";
  if (strlen(topic) >= sizeof m->stages[0].topic) {
    *out = admin_reply_failed(m, req, CCORD_ERR_RANGE, "topic is too long", err);
    return true;
  }
  if (m->nstages >= MOCK_STAGES_MAX) {
    *out = admin_reply_failed(m, req, CCORD_ERR_RANGE, "mock: too many stage instances", err);
    return true;
  }
  if (m->next_stage_id == 0) m->next_stage_id = 96001u;
  ccord_stage_instance stage = {
    .id = m->next_stage_id++, .channel_id = req->channel_id, .guild_id = g, .privacy_level = 2,
  };
  size_t tn = strlen(topic);
  memcpy(stage.topic, topic, tn + 1u);
  m->stages[m->nstages++] = stage;
  *out = mock_audit_add(m, g, 83, req->channel_id, m->channels[ci].name, req->reason, stage.topic);
  if (*out == CCORD_OK) *out = queue_stage_instance(m, &m->stages[m->nstages - 1u], false, now_of(m) + m->cfg.latency_ms, err);
  return true;
}

static bool admin_m5_stage_delete(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  size_t ci = admin_channel_index(m, req->channel_id);
  long si = mock_stage_index(m, req->channel_id);
  if (ci == SIZE_MAX || m->channels[ci].kind != CCORD_CHANNEL_STAGE || si < 0) {
    *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "It no longer exists.", err);
    return true;
  }
  ccord_id g = m->channels[ci].guild_id;
  if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_MUTE_MEMBERS) == 0u) {
    *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  ccord_stage_instance ended = m->stages[si];
  memmove(&m->stages[si], &m->stages[si + 1], (m->nstages - (size_t)si - 1u) * sizeof m->stages[0]);
  m->nstages--;
  *out = mock_audit_add(m, g, 85, req->channel_id, m->channels[ci].name, req->reason, ended.topic);
  if (*out == CCORD_OK) *out = queue_stage_instance(m, &ended, true, now_of(m) + m->cfg.latency_ms, err);
  return true;
}

// ---- Guild scheduled events: /guilds/{g}/scheduled-events (LIST / CREATE / MODIFY / DELETE / RSVP) -

static long mock_event_index(const mock *m, ccord_id guild_id, ccord_id event_id)
{
  for (size_t i = 0; i < m->nevents; i++) {
    if (m->events[i].guild_id == guild_id && m->events[i].id == event_id) return (long)i;
  }
  return -1;
}

static ccord_status queue_scheduled_event_update(mock *m, const ccord_scheduled_event *ev, bool deleted,
                                                 ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_SCHEDULED_EVENT_UPDATE, now_of(m) + m->cfg.latency_ms);
  it.events_own = ccord_alloc_array(1, sizeof *it.events_own);
  if (!it.events_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.events_own = *ev;
  it.ev.guild_id = ev->guild_id;
  it.ev.channel_id = ev->channel_id;
  it.ev.scheduled_events = it.events_own;
  it.ev.scheduled_event_count = 1;
  it.ev.scheduled_event_id = ev->id;
  it.ev.scheduled_event_deleted = deleted;
  return push_item(m, it, err);
}

static bool mock_event_entity_valid(mock *m, ccord_id g, const ccord_scheduled_event *ev, bool check_create_perms,
                                    ccord_status *fail_code, const char **fail_msg)
{
  if (ev->scheduled_start_ms <= 0) {
    *fail_code = CCORD_ERR_RANGE;
    *fail_msg = "Invalid scheduled start time.";
    return false;
  }
  if (ev->entity_type == CCORD_SCHEDULED_EVENT_EXTERNAL) {
    if (ev->channel_id != CCORD_ID_NONE || ev->location[0] == '\0' || ev->scheduled_end_ms <= ev->scheduled_start_ms) {
      *fail_code = CCORD_ERR_INVALID;
      *fail_msg = "External events require a location, an end time after start, and no channel.";
      return false;
    }
    return true;
  }
  if (ev->entity_type != CCORD_SCHEDULED_EVENT_STAGE_INSTANCE && ev->entity_type != CCORD_SCHEDULED_EVENT_VOICE) {
    *fail_code = CCORD_ERR_INVALID;
    *fail_msg = "Invalid scheduled event entity type.";
    return false;
  }
  if (ev->location[0] != '\0' || (ev->scheduled_end_ms != 0 && ev->scheduled_end_ms <= ev->scheduled_start_ms)) {
    *fail_code = CCORD_ERR_INVALID;
    *fail_msg = "Invalid scheduled event configuration.";
    return false;
  }
  size_t ci = admin_channel_index(m, ev->channel_id);
  ccord_channel_kind need_kind = ev->entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE
                                   ? CCORD_CHANNEL_STAGE
                                   : CCORD_CHANNEL_VOICE;
  if (ci == SIZE_MAX || m->channels[ci].guild_id != g || m->channels[ci].kind != need_kind) {
    *fail_code = CCORD_ERR_INVALID;
    *fail_msg = "Invalid event channel.";
    return false;
  }
  if (check_create_perms) {
    uint64_t ch_perms = mock_channel_perms(m, g, ev->channel_id);
    uint64_t need = ev->entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE
                      ? CCORD_PERM_MUTE_MEMBERS
                      : (CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_CONNECT);
    if ((ch_perms & need) != need) {
      *fail_code = CCORD_ERR_PERMISSION;
      *fail_msg = "You don't have permission to do that. (Missing Permissions)";
      return false;
    }
  }
  return true;
}

static bool admin_scheduled_events(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  ccord_id g = req->guild_id;
  if (mock_guild_index(m, g) == SIZE_MAX) {
    *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
    return true;
  }
  if (admin_member_index(m, g, m->self_id) < 0) {
    *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  uint64_t g_perms = mock_self_perms(m, g);
  switch (req->op) {
    case CCORD_ADMIN_EVENT_LIST: {
      size_t count = 0;
      for (size_t i = 0; i < m->nevents; i++) {
        if (m->events[i].guild_id == g) count++;
      }
      mock_item it = make_item(CCORD_NET_SCHEDULED_EVENT_LIST, now_of(m) + m->cfg.latency_ms);
      it.events_own = ccord_alloc_array(count ? count : 1u, sizeof *it.events_own);
      if (!it.events_own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      size_t k = 0;
      for (size_t i = 0; i < m->nevents; i++) {
        if (m->events[i].guild_id == g) it.events_own[k++] = m->events[i];
      }
      it.ev.guild_id = g;
      it.ev.scheduled_events = it.events_own;
      it.ev.scheduled_event_count = count;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_EVENT_CREATE: {
      if ((g_perms & (CCORD_PERM_CREATE_EVENTS | CCORD_PERM_MANAGE_EVENTS)) == 0u) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      ccord_status fcode = CCORD_OK;
      const char *fmsg = nullptr;
      if (!mock_event_entity_valid(m, g, &req->event, true, &fcode, &fmsg)) {
        *out = admin_reply_failed(m, req, fcode, fmsg, err);
        return true;
      }
      if (m->nevents >= MOCK_EVENTS_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of scheduled events reached.", err);
        return true;
      }
      if (m->next_event_id == 0) m->next_event_id = MOCK_EVENT_ID_BASE;
      ccord_scheduled_event ev = req->event;
      ev.id = m->next_event_id++;
      ev.guild_id = g;
      ev.creator_id = m->self_id;
      ev.privacy_level = 2;
      ev.status = CCORD_SCHEDULED_EVENT_SCHEDULED;
      ev.user_count = 0;
      ev.me_rsvped = false;
      m->events[m->nevents++] = ev;
      *out = mock_audit_add(m, g, 100, ev.id, ev.name, req->reason, "");
      if (*out == CCORD_OK) *out = queue_scheduled_event_update(m, &m->events[m->nevents - 1u], false, err);
      return true;
    }
    case CCORD_ADMIN_EVENT_MODIFY: {
      long ei = mock_event_index(m, g, req->event_id);
      if (ei < 0) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild Scheduled Event", err);
        return true;
      }
      bool can_manage = (g_perms & CCORD_PERM_MANAGE_EVENTS) != 0u ||
                        (m->events[ei].creator_id == m->self_id && (g_perms & CCORD_PERM_CREATE_EVENTS) != 0u);
      if (!can_manage) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      ccord_scheduled_event next = m->events[ei];
      uint32_t has = req->event_has;
      if (has == 0) {
        *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Nothing to modify.", err);
        return true;
      }
      if (has & CCORD_ADMIN_EVENT_HAS_NAME) memcpy(next.name, req->event.name, sizeof next.name);
      if (has & CCORD_ADMIN_EVENT_HAS_DESCRIPTION) memcpy(next.description, req->event.description, sizeof next.description);
      if (has & CCORD_ADMIN_EVENT_HAS_LOCATION) memcpy(next.location, req->event.location, sizeof next.location);
      if (has & CCORD_ADMIN_EVENT_HAS_CHANNEL) next.channel_id = req->event.channel_id;
      if (has & CCORD_ADMIN_EVENT_HAS_START) next.scheduled_start_ms = req->event.scheduled_start_ms;
      if (has & CCORD_ADMIN_EVENT_HAS_END) next.scheduled_end_ms = req->event.scheduled_end_ms;
      if (has & CCORD_ADMIN_EVENT_HAS_PRIVACY) next.privacy_level = req->event.privacy_level;
      if (has & CCORD_ADMIN_EVENT_HAS_ENTITY_TYPE) next.entity_type = req->event.entity_type;
      if (has & CCORD_ADMIN_EVENT_HAS_STATUS) {
        uint8_t cur = next.status;
        uint8_t dst = req->event.status;
        bool valid_step = (dst == cur) ||
                          (cur == CCORD_SCHEDULED_EVENT_SCHEDULED &&
                           (dst == CCORD_SCHEDULED_EVENT_ACTIVE || dst == CCORD_SCHEDULED_EVENT_CANCELED)) ||
                          (cur == CCORD_SCHEDULED_EVENT_ACTIVE && dst == CCORD_SCHEDULED_EVENT_COMPLETED);
        if (!valid_step) {
          *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid scheduled event status transition.", err);
          return true;
        }
        next.status = dst;
      }
      ccord_status fcode = CCORD_OK;
      const char *fmsg = nullptr;
      if (!mock_event_entity_valid(m, g, &next, (has & (CCORD_ADMIN_EVENT_HAS_CHANNEL | CCORD_ADMIN_EVENT_HAS_ENTITY_TYPE)) != 0u,
                                   &fcode, &fmsg)) {
        *out = admin_reply_failed(m, req, fcode, fmsg, err);
        return true;
      }
      m->events[ei] = next;
      *out = mock_audit_add(m, g, 101, next.id, next.name, req->reason, "");
      if (*out == CCORD_OK) *out = queue_scheduled_event_update(m, &m->events[ei], false, err);
      return true;
    }
    case CCORD_ADMIN_EVENT_DELETE: {
      long ei = mock_event_index(m, g, req->event_id);
      if (ei < 0) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild Scheduled Event", err);
        return true;
      }
      bool can_manage = (g_perms & CCORD_PERM_MANAGE_EVENTS) != 0u ||
                        (m->events[ei].creator_id == m->self_id && (g_perms & CCORD_PERM_CREATE_EVENTS) != 0u);
      if (!can_manage) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      ccord_scheduled_event removed = m->events[ei];
      memmove(&m->events[ei], &m->events[ei + 1], (m->nevents - (size_t)ei - 1u) * sizeof m->events[0]);
      m->nevents--;
      *out = mock_audit_add(m, g, 102, removed.id, removed.name, req->reason, "");
      if (*out == CCORD_OK) *out = queue_scheduled_event_update(m, &removed, true, err);
      return true;
    }
    case CCORD_ADMIN_EVENT_RSVP_ADD:
    case CCORD_ADMIN_EVENT_RSVP_REMOVE: {
      long ei = mock_event_index(m, g, req->event_id);
      if (ei < 0) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild Scheduled Event", err);
        return true;
      }
      ccord_id ev_ch = m->events[ei].channel_id;
      uint64_t view_perms = ev_ch != CCORD_ID_NONE ? mock_channel_perms(m, g, ev_ch) : g_perms;
      if ((view_perms & CCORD_PERM_VIEW_CHANNEL) == 0u) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      bool adding = (req->op == CCORD_ADMIN_EVENT_RSVP_ADD);
      if (adding && !m->events[ei].me_rsvped) {
        m->events[ei].me_rsvped = true;
        if (m->events[ei].user_count < UINT32_MAX) m->events[ei].user_count++;
      } else if (!adding && m->events[ei].me_rsvped) {
        m->events[ei].me_rsvped = false;
        if (m->events[ei].user_count > 0) m->events[ei].user_count--;
      }
      mock_item it = make_item(CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.scheduled_event_id = m->events[ei].id;
      it.ev.user_id = m->self_id;
      it.ev.scheduled_event_user_added = adding;
      *out = push_item(m, it, err);
      return true;
    }
    default:
      return false;
  }
}

// ---- M6 admin ops: account settings, devices & connections ----------

static ccord_status admin_m6_emit_settings(mock *m, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_USER_SETTINGS, now_of(m) + m->cfg.latency_ms);
  it.settings_own = ccord_alloc_array(1, sizeof *it.settings_own);
  if (!it.settings_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.settings_own = m->user_settings;
  it.ev.user_settings = it.settings_own;
  return push_item(m, it, err);
}

static ccord_status admin_m6_emit_sessions(mock *m, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_AUTH_SESSIONS, now_of(m) + m->cfg.latency_ms);
  size_t n = m->nauth_sessions ? m->nauth_sessions : 1u;
  it.auth_sessions_own = ccord_alloc_array(n, sizeof *it.auth_sessions_own);
  if (!it.auth_sessions_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  if (m->nauth_sessions) memcpy(it.auth_sessions_own, m->auth_sessions, m->nauth_sessions * sizeof *it.auth_sessions_own);
  it.ev.auth_sessions = it.auth_sessions_own;
  it.ev.auth_session_count = m->nauth_sessions;
  return push_item(m, it, err);
}

static ccord_status admin_m6_emit_logout(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_AUTH_SESSIONS_LOGOUT, now_of(m) + m->cfg.latency_ms);
  it.ev.session_hash_count = req->session_hash_count;
  memcpy(it.ev.session_hashes, req->session_hashes, req->session_hash_count * sizeof it.ev.session_hashes[0]);
  return push_item(m, it, err);
}

static ccord_status admin_m6_emit_connections(mock *m, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_CONNECTIONS, now_of(m) + m->cfg.latency_ms);
  size_t n = m->nconnections ? m->nconnections : 1u;
  it.connections_own = ccord_alloc_array(n, sizeof *it.connections_own);
  if (!it.connections_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  if (m->nconnections) memcpy(it.connections_own, m->connections, m->nconnections * sizeof *it.connections_own);
  it.ev.connections = it.connections_own;
  it.ev.connection_count = m->nconnections;
  return push_item(m, it, err);
}

static ccord_status admin_m6_emit_connection(mock *m, const ccord_connection *c, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_CONNECTION_UPSERT, now_of(m) + m->cfg.latency_ms);
  it.ev.connection = *c;
  return push_item(m, it, err);
}

static ccord_status admin_m6_emit_connection_delete(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  mock_item it = make_item(CCORD_NET_CONNECTION_DELETE, now_of(m) + m->cfg.latency_ms);
  (void)snprintf(it.ev.connection_name, sizeof it.ev.connection_name, "%s", req->connection_name);
  (void)snprintf(it.ev.connection_id, sizeof it.ev.connection_id, "%s", req->connection_id);
  return push_item(m, it, err);
}

static void admin_m6_apply_settings_patch(ccord_user_settings *s, const ccord_user_settings_patch *p)
{
  if (p->default_guilds_restricted >= 0) s->default_guilds_restricted = p->default_guilds_restricted != 0;
  if (p->friend_source_all >= 0) s->friend_source_all = p->friend_source_all != 0;
  if (p->friend_source_mutual_friends >= 0) s->friend_source_mutual_friends = p->friend_source_mutual_friends != 0;
  if (p->friend_source_mutual_guilds >= 0) s->friend_source_mutual_guilds = p->friend_source_mutual_guilds != 0;
  if (p->allow_access_to_age_restricted_servers >= 0) {
    s->allow_access_to_age_restricted_servers = p->allow_access_to_age_restricted_servers != 0;
  }
  if (p->developer_mode >= 0) s->developer_mode = p->developer_mode != 0;
  if (p->gif_auto_play >= 0) s->gif_auto_play = p->gif_auto_play != 0;
  if (p->animate_emoji >= 0) s->animate_emoji = p->animate_emoji != 0;
  if (p->enable_tts_command >= 0) s->enable_tts_command = p->enable_tts_command != 0;
  if (p->stream_notifications_enabled >= 0) s->stream_notifications_enabled = p->stream_notifications_enabled != 0;
  if (p->explicit_content_filter >= 0) s->explicit_content_filter = (uint8_t)p->explicit_content_filter;
  if (p->animate_stickers >= 0) s->animate_stickers = (uint8_t)p->animate_stickers;
  if (p->locale[0] != '\0') (void)snprintf(s->locale, sizeof s->locale, "%s", p->locale);
}

static long admin_m6_session_index(const mock *m, const char hash[64])
{
  if (!memchr(hash, '\0', 64u)) return -1;
  for (size_t i = 0; i < m->nauth_sessions; i++) {
    if (strcmp(m->auth_sessions[i].id_hash, hash) == 0) return (long)i;
  }
  return -1;
}

static size_t admin_m6_connection_index(const mock *m, const ccord_admin_request *req)
{
  for (size_t i = 0; i < m->nconnections; i++) {
    if (strcmp(m->connections[i].type, req->connection_name) == 0 &&
        strcmp(m->connections[i].id, req->connection_id) == 0) {
      return i;
    }
  }
  return SIZE_MAX;
}

static bool admin_m6_account(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  switch (req->op) {
    case CCORD_ADMIN_USER_SETTINGS_PATCH:
      admin_m6_apply_settings_patch(&m->user_settings, &req->settings_patch);
      *out = admin_m6_emit_settings(m, err);
      return true;
    case CCORD_ADMIN_AUTH_SESSIONS_LIST:
      *out = admin_m6_emit_sessions(m, err);
      return true;
    case CCORD_ADMIN_AUTH_SESSIONS_LOGOUT: {
      if (req->session_hash_count == 0 || req->session_hash_count > CCORD_ADMIN_SESSION_LOGOUT_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "mock: no session hashes", err);
        return true;
      }
      long idx[CCORD_ADMIN_SESSION_LOGOUT_MAX];
      for (size_t i = 0; i < req->session_hash_count; i++) {
        idx[i] = admin_m6_session_index(m, req->session_hashes[i]);
        if (idx[i] < 0) {
          *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Session", err);
          return true;
        }
        if (m->auth_sessions[idx[i]].current) {
          *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "You cannot log out the current session.", err);
          return true;
        }
      }
      bool drop[MOCK_AUTH_SESSIONS_MAX] = {false};
      for (size_t i = 0; i < req->session_hash_count; i++) drop[idx[i]] = true;
      size_t k = 0;
      for (size_t i = 0; i < m->nauth_sessions; i++) {
        if (!drop[i]) m->auth_sessions[k++] = m->auth_sessions[i];
      }
      m->nauth_sessions = k;
      *out = admin_m6_emit_logout(m, req, err);
      return true;
    }
    case CCORD_ADMIN_CONNECTIONS_LIST:
      *out = admin_m6_emit_connections(m, err);
      return true;
    case CCORD_ADMIN_CONNECTION_EDIT: {
      size_t ci = admin_m6_connection_index(m, req);
      if (ci == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Connection", err);
        return true;
      }
      if (req->connection_edit.visibility >= 0) {
        m->connections[ci].visibility = (uint8_t)req->connection_edit.visibility;
      }
      if (req->connection_edit.friend_sync >= 0) m->connections[ci].friend_sync = req->connection_edit.friend_sync != 0;
      if (req->connection_edit.show_activity >= 0) {
        m->connections[ci].show_activity = req->connection_edit.show_activity != 0;
      }
      *out = admin_m6_emit_connection(m, &m->connections[ci], err);
      return true;
    }
    case CCORD_ADMIN_CONNECTION_DELETE: {
      size_t ci = admin_m6_connection_index(m, req);
      if (ci == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Connection", err);
        return true;
      }
      memmove(&m->connections[ci], &m->connections[ci + 1],
              (m->nconnections - ci - 1u) * sizeof m->connections[0]);
      m->nconnections--;
      *out = admin_m6_emit_connection_delete(m, req, err);
      return true;
    }
    default:
      return false;
  }
}

// ---- M5 vtable hooks: DM calls, soundboard, local audio prefs -------------------------------------

static ccord_status mock_call_action(ccord_transport *t, ccord_id channel_id, ccord_call_action act, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (act != CCORD_CALL_JOIN && act != CCORD_CALL_LEAVE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad call action");
  }
  size_t ci = admin_channel_index(m, channel_id);
  if (ci == SIZE_MAX) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown channel");
  if (m->channels[ci].guild_id != CCORD_DM_GUILD_ID || m->channels[ci].kind != CCORD_CHANNEL_TEXT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: calls only happen in DM channels");
  }
  uint64_t due = now_of(m) + m->cfg.latency_ms;
  long call_i = mock_call_index(m, channel_id);
  long vi = mock_voice_index(m, CCORD_DM_GUILD_ID, m->self_id);
  bool in_this_call = vi >= 0 && m->voices[vi].channel_id == channel_id;
  if (act == CCORD_CALL_JOIN) {
    if (in_this_call) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: already in this call");
    if (call_i >= 0) {
      // Accept: self stops ringing; the call row keeps everyone else.
      if (call_drop_ringing(&m->dm_calls[call_i], m->self_id)) {
        ccord_status st = queue_call_event(m, &m->dm_calls[call_i], CCORD_NET_CALL_UPDATE, due, err);
        if (st != CCORD_OK) return st;
      }
    } else {
      // Outgoing call: ring the DM's recipients.
      if (m->ndm_calls >= MOCK_CALLS_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many calls");
      ccord_call_state call = {.channel_id = channel_id, .region = "rotterdam", .active = true};
      for (size_t i = 0; i < m->channels[ci].recipient_count && call.ringing_count < 8u; i++) {
        ccord_id r = m->channels[ci].recipient_ids[i];
        if (r != m->self_id) call.ringing[call.ringing_count++] = r;
      }
      m->dm_calls[m->ndm_calls++] = call;
      call_i = (long)(m->ndm_calls - 1u);
      ccord_status st = queue_call_event(m, &call, CCORD_NET_CALL_CREATE, due, err);
      if (st != CCORD_OK) return st;
      // The client asks Discord to ring (POST /channels/{c}/call/ring): record it, and refuse it later when told to.
      m->calls[CCORD_MOCK_CALL_RING]++;
      m->last_ring_channel = channel_id;
      if (m->ring_refusal[0]) {
        mock_item it = make_item(CCORD_NET_ACTION_FAILED, due);
        it.ev.action = CCORD_ACTION_ADMIN;
        it.ev.channel_id = channel_id;
        it.ev.guild_id = CCORD_DM_GUILD_ID;
        it.ev.status = CCORD_ERR_PERMISSION;
        it.ev.text = m->ring_refusal;
        st = push_item(m, it, err);
        if (st != CCORD_OK) return st;
      }
    }
    ccord_voice_state v = {.guild_id = CCORD_DM_GUILD_ID, .channel_id = channel_id, .user_id = m->self_id,
                           .self_mute = m->self_mute, .self_deaf = m->self_deaf};
    if (vi >= 0) {
      m->voices[vi] = v;
    } else {
      if (m->nvoices >= MOCK_NUM_USERS) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many voice states");
      m->voices[m->nvoices++] = v;
      m->snap.voice_state_count = m->nvoices;
    }
    mock_voice_connect(m, channel_id);
    ccord_status st = queue_voice_state(m, &v, due, err);
    if (st == CCORD_OK) st = queue_voice_server(m, CCORD_DM_GUILD_ID, channel_id, due, err);
    if (st == CCORD_OK) m->calls[CCORD_MOCK_CALL_VOICE_JOIN]++;
    return st;
  }
  // LEAVE: decline while still ringing, or hang up while connected.
  bool dropped = call_i >= 0 && call_drop_ringing(&m->dm_calls[call_i], m->self_id);
  if (!in_this_call && !dropped) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not in this call");
  ccord_status st = CCORD_OK;
  if (dropped) st = queue_call_event(m, &m->dm_calls[call_i], CCORD_NET_CALL_UPDATE, due, err);
  if (st == CCORD_OK && in_this_call) {
    if (m->has_stream && m->stream.guild_id == CCORD_DM_GUILD_ID) {
      m->stream.active = false;
      m->has_stream = false;
      mock_stream_media_clear(m);
      (void)queue_stream_state(m, &m->stream, due, err);
    }
    ccord_voice_state v = m->voices[vi];
    v.channel_id = CCORD_ID_NONE;
    v.self_video = false;
    v.self_stream = false;
    memmove(&m->voices[vi], &m->voices[vi + 1], (m->nvoices - (size_t)vi - 1u) * sizeof *m->voices);
    m->nvoices--;
    m->snap.voice_state_count = m->nvoices;
    mock_voice_disconnect(m);
    st = queue_voice_state(m, &v, due, err);
  }
  if (st == CCORD_OK && call_i >= 0) st = mock_call_maybe_end(m, (size_t)call_i, due, err);
  return st;
}

static ccord_status mock_play_soundboard(ccord_transport *t, ccord_id guild_id, ccord_id sound_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  const ccord_soundboard_sound *sound = nullptr;
  for (size_t i = 0; i < m->nsoundboard; i++) {
    if (m->soundboard[i].id == sound_id && m->soundboard[i].guild_id == guild_id) {
      sound = &m->soundboard[i];
      break;
    }
  }
  if (!sound || !sound->available) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown soundboard sound");
  long vi = mock_voice_index(m, guild_id, m->self_id);
  if (vi < 0 || m->voices[vi].channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: join voice before playing sounds");
  }
  if ((mock_channel_perms(m, guild_id, m->voices[vi].channel_id) & CCORD_PERM_USE_SOUNDBOARD) == 0u) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, k_missing_permissions);
  }
  // Simulated playback (the mock's stand-in for the CDN fetch + decode): SPEAKING on, 20 ms of
  // deterministic PCM scaled by the sound's volume into the voice ring, then SPEAKING off.
  uint64_t due = now_of(m) + m->cfg.latency_ms;
  int16_t pcm[MOCK_SOUNDBOARD_PCM_SAMPLES * 2u];
  int32_t amp = 3000 * (int32_t)sound->volume_pct / 100;
  for (size_t i = 0; i < MOCK_SOUNDBOARD_PCM_SAMPLES; i++) {
    int16_t s = (int16_t)((i % 48u) < 24u ? amp : -amp);
    pcm[i * 2u] = s;
    pcm[i * 2u + 1u] = s;
  }
  (void)mock_push_voice_pcm(t, pcm, MOCK_SOUNDBOARD_PCM_SAMPLES, err);
  queue_speaking(m, guild_id, m->voices[vi].channel_id, m->self_id, true, due);
  queue_speaking(m, guild_id, m->voices[vi].channel_id, m->self_id, false, due + 200u);
  return CCORD_OK;
}

static ccord_status mock_set_user_volume(ccord_transport *t, ccord_id user_id, uint8_t pct, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid user");
  if (pct > 200u) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: volume must be 0..200");
  for (size_t i = 0; i < m->nuser_volume; i++) {
    if (m->user_volume[i].user_id == user_id) {
      m->user_volume[i].pct = pct;
      return CCORD_OK;
    }
  }
  if (m->nuser_volume >= MOCK_NUM_USERS) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many volume rows");
  m->user_volume[m->nuser_volume].user_id = user_id;
  m->user_volume[m->nuser_volume].pct = pct;
  m->nuser_volume++;
  return CCORD_OK;   // local preference: no Discord traffic, no event
}

static ccord_status mock_set_voice_config(ccord_transport *t, const ccord_voice_config *cfg, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!cfg) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null voice config");
  if (cfg->vad_sensitivity_pct > 100u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: sensitivity must be 0..100");
  }
  m->voice_cfg = *cfg;
  m->has_voice_cfg = true;
  return CCORD_OK;   // local DSP preference: no Discord traffic, no event
}

// ---- M5 test hooks ---------------------------------------------------------------------------------

ccord_status ccord_mock_push_incoming_call(ccord_transport *t, ccord_id recipient_id, ccord_id *out_channel_id,
                                           ccord_error *err)
{
  ccord_error_clear(err);
  if (!t || recipient_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad call args");
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  bool known = false;
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == recipient_id && recipient_id != m->self_id) known = true;
  }
  if (!known) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown recipient");
  ccord_status st = mock_open_dm(t, recipient_id, err);   // DM channel + CHANNEL_UPSERT, as a real ring arrives
  if (st != CCORD_OK) return st;
  ccord_id cid = 900000u + recipient_id;   // mock_open_dm's deterministic DM id
  uint64_t due = now_of(m) + m->cfg.latency_ms;
  ccord_call_state call = {.channel_id = cid, .region = "rotterdam", .active = true};
  call.ringing[0] = m->self_id;
  call.ringing_count = 1;
  long call_i = mock_call_index(m, cid);
  if (call_i >= 0) {
    m->dm_calls[call_i] = call;
  } else {
    if (m->ndm_calls >= MOCK_CALLS_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many calls");
    m->dm_calls[m->ndm_calls++] = call;
  }
  st = queue_call_event(m, &call, CCORD_NET_CALL_CREATE, due, err);
  // The caller is already connected: their voice state arrives with the ring.
  if (st == CCORD_OK) {
    ccord_voice_state v = {.guild_id = CCORD_DM_GUILD_ID, .channel_id = cid, .user_id = recipient_id};
    long vi = mock_voice_index(m, CCORD_DM_GUILD_ID, recipient_id);
    if (vi >= 0) {
      m->voices[vi] = v;
    } else {
      if (m->nvoices >= MOCK_NUM_USERS) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many voice states");
      m->voices[m->nvoices++] = v;
      m->snap.voice_state_count = m->nvoices;
    }
    st = queue_voice_state(m, &v, due, err);
  }
  if (st == CCORD_OK && out_channel_id) *out_channel_id = cid;
  return st;
}

ccord_status ccord_mock_end_call(ccord_transport *t, ccord_id channel_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!t) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  long call_i = mock_call_index(m, channel_id);
  if (call_i < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: no such call");
  uint64_t due = now_of(m) + m->cfg.latency_ms;
  // The remote side hung up: everyone still connected leaves, then the call row goes.
  for (size_t i = 0; i < m->nvoices;) {
    if (m->voices[i].guild_id == CCORD_DM_GUILD_ID && m->voices[i].channel_id == channel_id) {
      ccord_voice_state v = m->voices[i];
      bool was_self = v.user_id == m->self_id;
      v.channel_id = CCORD_ID_NONE;
      v.self_video = false;
      v.self_stream = false;
      memmove(&m->voices[i], &m->voices[i + 1], (m->nvoices - i - 1u) * sizeof *m->voices);
      m->nvoices--;
      m->snap.voice_state_count = m->nvoices;
      if (was_self) mock_voice_disconnect(m);
      ccord_status st = queue_voice_state(m, &v, due, err);
      if (st != CCORD_OK) return st;
    } else {
      i++;
    }
  }
  ccord_call_state ended = m->dm_calls[call_i];
  memmove(&m->dm_calls[call_i], &m->dm_calls[call_i + 1], (m->ndm_calls - (size_t)call_i - 1u) * sizeof m->dm_calls[0]);
  m->ndm_calls--;
  return queue_call_event(m, &ended, CCORD_NET_CALL_DELETE, due, err);
}

void ccord_mock_dave_status(const ccord_transport *t, ccord_dave_status *out)
{
  if (!out) return;
  if (t) {
    const mock *m = (const mock *)t;
    *out = m->dave;
    if (out->active) {
      snprintf(out->privacy_code, sizeof out->privacy_code, "%s", "01234 56789 01234 56789 01234 56789");
    } else {
      out->privacy_code[0] = '\0';
    }
  } else {
    memset(out, 0, sizeof *out);
  }
}

void ccord_mock_rtc_stats(const ccord_transport *t, ccord_rtc_stats *out)
{
  if (!out) return;
  if (!t) {
    memset(out, 0, sizeof *out);
    return;
  }
  const mock *m = (const mock *)t;
  *out = m->rtc;
  if (m->rtc.state == CCORD_VOICE_IDLE) return;
  uint64_t now = now_of(m);
  for (size_t i = 0; i < m->nvstate_tl; i++) {   // an injected outage moves the state; the rest stays as joined
    if (m->vstate_tl[i].due <= now) out->state = (ccord_voice_connection_state)m->vstate_tl[i].state;
  }
}

uint8_t ccord_mock_user_volume(const ccord_transport *t, ccord_id user_id)
{
  if (!t) return 100u;
  const mock *m = (const mock *)t;
  for (size_t i = 0; i < m->nuser_volume; i++) {
    if (m->user_volume[i].user_id == user_id) return m->user_volume[i].pct;
  }
  return 100u;
}

bool ccord_mock_voice_config(const ccord_transport *t, ccord_voice_config *out)
{
  if (!t || !out) return false;
  const mock *m = (const mock *)t;
  if (!m->has_voice_cfg) return false;
  *out = m->voice_cfg;
  return true;
}

bool ccord_mock_user_settings(const ccord_transport *t, ccord_user_settings *out)
{
  if (!t || !out) return false;
  *out = ((const mock *)t)->user_settings;
  return true;
}

ccord_status ccord_mock_set_remote_camera(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                          bool on, ccord_error *err)
{
  ccord_error_clear(err);
  if (!t) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (user_id == m->self_id) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: self is not a remote sender");
  bool known = false;
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) known = known || m->users[i].id == user_id;
  if (!known) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown user");
  uint64_t due = now_of(m) + m->cfg.latency_ms;
  long vi = mock_voice_index(m, guild_id, user_id);
  if (!on) {
    if (vi < 0 || m->voices[vi].channel_id == CCORD_ID_NONE) {
      return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: user is not in voice");
    }
    m->voices[vi].self_video = false;
    ccord_status st = queue_voice_state(m, &m->voices[vi], due, err);
    long ci = mock_cam_find(m, user_id);
    if (ci >= 0) mock_cam_remove_at(m, (size_t)ci);
    mock_cam_rebalance(m);
    return st;
  }
  if (!mock_guild_known(m, guild_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown guild");
  ccord_status st;
  size_t chi = channel_index(m, channel_id, false, &st, err);
  if (chi == SIZE_MAX) return st;
  if (m->channels[chi].guild_id != guild_id ||
      (m->channels[chi].kind != CCORD_CHANNEL_VOICE && m->channels[chi].kind != CCORD_CHANNEL_STAGE)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not a voice channel of the guild");
  }
  if (mock_cam_find(m, user_id) < 0 && m->ncams >= MOCK_NUM_USERS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many camera senders");
  }
  if (vi >= 0) {
    m->voices[vi].channel_id = channel_id;
    m->voices[vi].self_video = true;
  } else {
    if (m->nvoices >= MOCK_NUM_USERS) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many voice states");
    m->voices[m->nvoices++] = (ccord_voice_state){.guild_id = guild_id, .channel_id = channel_id, .user_id = user_id,
                                                  .self_video = true};
    m->snap.voice_state_count = m->nvoices;
    vi = (long)m->nvoices - 1;
  }
  (void)mock_cam_register(m, user_id);
  mock_cam_rebalance(m);
  return queue_voice_state(m, &m->voices[vi], due, err);
}

ccord_status ccord_mock_push_camera_frame(ccord_transport *t, ccord_id user_id, const uint8_t *annexb, size_t len,
                                          uint32_t ts90k, bool keyframe, ccord_error *err)
{
  ccord_error_clear(err);
  if (!t) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock *m = (mock *)t;
  if (!annexb || len == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty camera frame");
  if (len > CCORD_CAM_FRAME_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: camera frame too large");
  mock_cam_rebalance(m);
  long ci = mock_cam_find(m, user_id);
  if (ci < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not a camera sender");
  typeof(m->cams[0]) *c = &m->cams[ci];
  if (!c->decoding) return CCORD_OK;   // mirrors the gateway: a waiting sender's packets are dropped
  if (c->count >= MOCK_CAM_FIFO) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: camera frame queue full");
  uint8_t *copy = ccord_alloc_array(len, sizeof *copy);
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  memcpy(copy, annexb, len);
  size_t idx = (c->head + c->count) % MOCK_CAM_FIFO;
  c->fifo[idx].data = copy;
  c->fifo[idx].len = len;
  c->fifo[idx].ts90k = ts90k;
  c->fifo[idx].keyframe = keyframe;
  c->count++;
  return CCORD_OK;
}

size_t ccord_mock_camera_frame_count(const ccord_transport *t, ccord_id user_id)
{
  if (!t) return 0;
  const mock *m = (const mock *)t;
  long ci = mock_cam_find(m, user_id);
  return ci < 0 ? 0 : m->cams[ci].count;
}

size_t ccord_mock_stream_frame_count(const ccord_transport *t)
{
  return t ? ((const mock *)t)->stream_frame_count : 0;
}

size_t ccord_mock_stream_pcm_count(const ccord_transport *t)
{
  return t ? ((const mock *)t)->stream_pcm_count : 0;
}

void ccord_mock_fail_media(ccord_transport *t, const char *url_part)
{
  mock *m = (mock *)t;
  if (!m) return;
  snprintf(m->fail_media, sizeof m->fail_media, "%s", url_part ? url_part : "");
}

void ccord_mock_fail_next_admin(ccord_transport *t, bool fail)
{
  if (t) ((mock *)t)->fail_next_admin = fail;
}

ccord_status ccord_mock_push_user_settings(ccord_transport *t, const ccord_user_settings *settings, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m || !m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!settings) return ccord_error_set(err, CCORD_ERR_INVALID, "null settings");
  m->user_settings = *settings;
  return admin_m6_emit_settings(m, err);   // gateway USER_SETTINGS_UPDATE: no REST request was made
}

static bool mock_admin_server(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  static const char unknown_role[] = "Unknown Role";
  static const char unknown_member[] = "Unknown Member";
  ccord_id g = req->guild_id;
  *out = CCORD_OK;
  switch (req->op) {
    case CCORD_ADMIN_CREATE_ROLE: {
      size_t cnt = 0, first = mock_guild_roles(m, g, &cnt);
      if (cnt == 0) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      uint64_t perms = req->role_permissions >= 0 ? (uint64_t)req->role_permissions : m->roles[first].permissions;
      if (!mock_may(m, g, CCORD_PERM_MANAGE_ROLES) || m->nroles == MOCK_ROLES_MAX || cnt > CCORD_MAX_ROLES_PER_GUILD ||
          ccord_perm_can_grant(g, mock_guild_owner(m, g), m->self_id, mock_self_perms(m, g), 0, perms) !=
            CCORD_PERM_ALLOWED) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      const char *name = admin_owned(m, req->name[0] != '\0' ? req->name : "new role");
      if (!name) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      ccord_id ids[CCORD_ADMIN_ROLE_REORDER_MAX];
      size_t nids = 1;
      for (size_t i = 0; i < m->nroles; i++) {   // the new role sits just above @everyone; the rest move up one
        if (m->roles[i].guild_id == g && m->roles[i].id != g) {
          m->roles[i].position++;
          if (nids < CCORD_ADMIN_ROLE_REORDER_MAX) ids[nids++] = m->roles[i].id;
        }
      }
      ccord_id id = m->next_role_id++;
      ids[0] = id;
      size_t at = first + cnt;   // the end of the guild's group keeps the table grouped
      memmove(&m->roles[at + 1], &m->roles[at], (m->nroles - at) * sizeof m->roles[0]);
      m->roles[at] = (ccord_role){.id = id, .guild_id = g, .name = name,
                                  .color = req->role_color >= 0 ? (uint32_t)req->role_color : 0u, .position = 1,
                                  .permissions = perms, .hoist = req->role_hoist > 0,
                                  .mentionable = req->role_mentionable > 0};
      m->nroles++;
      *out = mock_audit_add(m, g, 30, id, name, req->reason, name);
      if (*out == CCORD_OK) *out = admin_emit_roles(m, CCORD_NET_ROLE_UPSERT, ids, nids, g, err);
      return true;
    }
    case CCORD_ADMIN_EDIT_ROLE: {
      size_t ri = mock_role_find(m, g, req->role_id);
      if (ri == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_role, err);
        return true;
      }
      uint64_t old = m->roles[ri].permissions;
      uint64_t now = req->role_permissions >= 0 ? (uint64_t)req->role_permissions : old;
      if (!mock_may_edit_role(m, g, req->role_id, false) ||
          ccord_perm_can_grant(g, mock_guild_owner(m, g), m->self_id, mock_self_perms(m, g), old, now) !=
            CCORD_PERM_ALLOWED) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      char detail[128];
      detail[0] = '\0';
      if (req->name[0] != '\0') {
        const char *name = admin_owned(m, req->name);
        if (!name) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        (void)snprintf(detail, sizeof detail, "name: %.40s -> %.40s", m->roles[ri].name, name);
        m->roles[ri].name = name;
      }
      if (req->role_permissions >= 0) {
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "permissions changed");
        m->roles[ri].permissions = now;
      }
      if (req->role_color >= 0) {
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "color: %u -> %d", m->roles[ri].color, req->role_color);
        m->roles[ri].color = (uint32_t)req->role_color;
      }
      if (req->role_hoist >= 0) {
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "hoist: %s", req->role_hoist ? "yes" : "no");
        m->roles[ri].hoist = req->role_hoist != 0;
      }
      if (req->role_mentionable >= 0) {
        if (detail[0] == '\0') {
          (void)snprintf(detail, sizeof detail, "mentionable: %s", req->role_mentionable ? "yes" : "no");
        }
        m->roles[ri].mentionable = req->role_mentionable != 0;
      }
      for (size_t i = 0; i < m->nmembers; i++) {   // names and colours shown in the member list follow the role
        if (m->members[i].guild_id == g) mock_refresh_display(m, g, m->members[i].user_id);
      }
      *out = mock_audit_add(m, g, 31, req->role_id, m->roles[ri].name, req->reason, detail);
      if (*out == CCORD_OK) *out = admin_emit_roles(m, CCORD_NET_ROLE_UPSERT, &req->role_id, 1, g, err);
      return true;
    }
    case CCORD_ADMIN_DELETE_ROLE: {
      size_t ri = mock_role_find(m, g, req->role_id);
      if (ri == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_role, err);
        return true;
      }
      if (!mock_may_edit_role(m, g, req->role_id, false)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      *out = mock_audit_add(m, g, 32, req->role_id, m->roles[ri].name, req->reason, "");
      memmove(&m->roles[ri], &m->roles[ri + 1], (m->nroles - ri - 1) * sizeof m->roles[0]);
      m->nroles--;
      for (size_t i = 0; i < sizeof m->mrows / sizeof m->mrows[0]; i++) {
        mock_mrow *r = &m->mrows[i];
        if (!r->present || r->guild_id != g) continue;
        for (size_t k = 0; k < r->count; k++) {
          if (r->ids[k] != req->role_id) continue;
          memmove(&r->ids[k], &r->ids[k + 1], (r->count - k - 1) * sizeof r->ids[0]);
          r->count--;
          break;
        }
        mock_refresh_display(m, g, r->user_id);
      }
      mock_sync_member_roles(m);
      if (*out != CCORD_OK) return true;
      mock_item it = make_item(CCORD_NET_ROLE_DELETE, now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.role_id = req->role_id;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_REORDER_ROLES: {
      for (size_t i = 0; i < req->reorder_count; i++) {
        if (mock_role_find(m, g, req->reorder_ids[i]) == SIZE_MAX) {
          *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_role, err);
          return true;
        }
        if (!mock_may_edit_role(m, g, req->reorder_ids[i], false)) {
          *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
          return true;
        }
      }
      for (size_t i = 0; i < req->reorder_count; i++) {
        m->roles[mock_role_find(m, g, req->reorder_ids[i])].position = req->reorder_positions[i];
      }
      for (size_t i = 0; i < m->nmembers; i++) {
        if (m->members[i].guild_id == g) mock_refresh_display(m, g, m->members[i].user_id);
      }
      size_t cnt = 0, first = mock_guild_roles(m, g, &cnt);
      ccord_id ids[CCORD_ADMIN_ROLE_REORDER_MAX];
      size_t n = 0;
      for (size_t i = 0; i < cnt && n < CCORD_ADMIN_ROLE_REORDER_MAX; i++) ids[n++] = m->roles[first + i].id;
      *out = mock_audit_add(m, g, 31, req->reorder_ids[0], m->roles[mock_role_find(m, g, req->reorder_ids[0])].name,
                            req->reason, "position changed");
      if (*out == CCORD_OK) *out = admin_emit_roles(m, CCORD_NET_ROLE_UPSERT, ids, n, g, err);
      return true;
    }
    case CCORD_ADMIN_ADD_MEMBER_ROLE:
    case CCORD_ADMIN_REMOVE_MEMBER_ROLE: {
      bool add = req->op == CCORD_ADMIN_ADD_MEMBER_ROLE;
      if (mock_role_find(m, g, req->role_id) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_role, err);
        return true;
      }
      mock_mrow *r = mock_mrow_find(m, g, req->user_id);
      if (!r) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_member, err);
        return true;
      }
      if (!mock_may_edit_role(m, g, req->role_id, true)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t at = SIZE_MAX;
      for (size_t k = 0; k < r->count; k++) {
        if (r->ids[k] == req->role_id) at = k;
      }
      if (add && at == SIZE_MAX) {
        if (r->count == MOCK_MEMBER_ROLES_MAX) {
          *out = admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of roles reached", err);
          return true;
        }
        r->ids[r->count++] = req->role_id;
      } else if (!add && at != SIZE_MAX) {
        memmove(&r->ids[at], &r->ids[at + 1], (r->count - at - 1) * sizeof r->ids[0]);
        r->count--;
      }
      mock_refresh_display(m, g, req->user_id);
      const ccord_user *tu = nullptr;
      for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
        if (m->users[i].id == req->user_id) tu = &m->users[i];
      }
      *out = mock_audit_add(m, g, 25, req->user_id, tu ? tu->name : "", req->reason,
                            add ? "added a role" : "removed a role");
      if (*out == CCORD_OK) *out = admin_emit_member_roles(m, g, req->user_id, req->role_id, add, err);
      return true;
    }
    case CCORD_ADMIN_UNBAN: {
      if (!mock_may(m, g, CCORD_PERM_BAN_MEMBERS)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t at = SIZE_MAX;
      for (size_t i = 0; i < m->nbans; i++) {
        if (m->bans[i].guild_id == g && m->bans[i].ban.user_id == req->user_id) at = i;
      }
      if (at == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Ban", err);
        return true;
      }
      *out = mock_audit_add(m, g, 23, req->user_id, m->bans[at].ban.name, req->reason, "");
      memmove(&m->bans[at], &m->bans[at + 1], (m->nbans - at - 1) * sizeof m->bans[0]);
      m->nbans--;
      if (*out != CCORD_OK) return true;
      mock_item it = make_item(CCORD_NET_BAN_REMOVE, now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.user_id = req->user_id;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_CREATE_INVITE: {
      size_t ci = admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_CREATE_INSTANT_INVITE) || m->ninvites == MOCK_INVITES_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
      (void)snprintf(code, sizeof code, "mock%04u", m->next_invite++);
      const char *own = admin_owned(m, code);
      if (!own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      int64_t now = (int64_t)now_of(m);
      m->invites[m->ninvites++] = (mock_invite){
        g, {.code = own, .channel_id = req->channel_id, .channel_name = m->channels[ci].name, .inviter_id = m->self_id,
            .inviter_name = m->users[0].name, .max_uses = req->invite_max_uses, .max_age_s = req->invite_max_age_s,
            .temporary = req->invite_temporary, .created_ms = now,
            .expires_ms = req->invite_max_age_s ? now + (int64_t)req->invite_max_age_s * 1000 : 0}};
      *out = mock_audit_add(m, g, 40, 0, own, "", "invite created");
      if (*out == CCORD_OK) *out = admin_emit_invites(m, CCORD_NET_INVITE_UPSERT, g, own, err);
      return true;
    }
    case CCORD_ADMIN_REVOKE_INVITE: {
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t at = SIZE_MAX;
      for (size_t i = 0; i < m->ninvites; i++) {
        if (m->invites[i].guild_id == g && strcmp(m->invites[i].inv.code, req->code) == 0) at = i;
      }
      if (at == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Invite", err);
        return true;
      }
      const char *code = m->invites[at].inv.code;   // a literal or a retired string: stable until destroy
      *out = mock_audit_add(m, g, 42, 0, code, req->reason, "");
      memmove(&m->invites[at], &m->invites[at + 1], (m->ninvites - at - 1) * sizeof m->invites[0]);
      m->ninvites--;
      if (*out == CCORD_OK) *out = admin_emit_invites(m, CCORD_NET_INVITE_DELETE, g, code, err);
      return true;
    }
    case CCORD_ADMIN_LIST_BANS:
      *out = mock_may(m, g, CCORD_PERM_BAN_MEMBERS)
               ? admin_emit_bans(m, g, err)
               : admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
      return true;
    case CCORD_ADMIN_LIST_INVITES:
      *out = mock_may(m, g, CCORD_PERM_MANAGE_GUILD)
               ? admin_emit_invites(m, CCORD_NET_INVITE_LIST, g, nullptr, err)
               : admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
      return true;
    case CCORD_ADMIN_LIST_AUDIT_LOG:
      *out = mock_may(m, g, CCORD_PERM_VIEW_AUDIT_LOG)
               ? admin_emit_audit(m, req, err)
               : admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
      return true;
    case CCORD_ADMIN_REORDER_CHANNELS: {
      if (!mock_guild_known(m, g)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_CHANNELS)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      for (size_t i = 0; i < req->reorder_count; i++) {
        size_t ci = admin_channel_index(m, req->reorder_ids[i]);
        if (ci == SIZE_MAX || m->channels[ci].guild_id != g) {
          *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
          return true;
        }
        if (req->reorder_has_parent[i] && req->reorder_parents[i] != CCORD_ID_NONE) {
          size_t pci = admin_channel_index(m, req->reorder_parents[i]);
          if (pci == SIZE_MAX || m->channels[pci].guild_id != g || m->channels[pci].kind != CCORD_CHANNEL_CATEGORY ||
              m->channels[ci].kind == CCORD_CHANNEL_CATEGORY) {
            *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid parent category", err);
            return true;
          }
        }
      }
      for (size_t i = 0; i < req->reorder_count; i++) {
        size_t ci = admin_channel_index(m, req->reorder_ids[i]);
        m->channels[ci].position = req->reorder_positions[i];
        if (req->reorder_has_parent[i]) m->channels[ci].parent_id = req->reorder_parents[i];
      }
      size_t n = req->reorder_count;
      size_t first_ci = admin_channel_index(m, req->reorder_ids[0]);
      *out = mock_audit_add(m, g, 11, req->reorder_ids[0],
                            first_ci != SIZE_MAX ? m->channels[first_ci].name : "", req->reason, "position changed");
      if (*out != CCORD_OK) return true;
      mock_item it = make_item(CCORD_NET_CHANNEL_POSITIONS, now_of(m) + m->cfg.latency_ms);
      it.reorder_ids_own = ccord_alloc_array(n, sizeof *it.reorder_ids_own);
      it.reorder_pos_own = ccord_alloc_array(n, sizeof *it.reorder_pos_own);
      it.reorder_par_own = ccord_alloc_array(n, sizeof *it.reorder_par_own);
      it.reorder_hpar_own = ccord_alloc_array(n, sizeof *it.reorder_hpar_own);
      if (!it.reorder_ids_own || !it.reorder_pos_own || !it.reorder_par_own || !it.reorder_hpar_own) {
        item_release(&it);
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      memcpy(it.reorder_ids_own, req->reorder_ids, n * sizeof *it.reorder_ids_own);
      memcpy(it.reorder_pos_own, req->reorder_positions, n * sizeof *it.reorder_pos_own);
      memcpy(it.reorder_par_own, req->reorder_parents, n * sizeof *it.reorder_par_own);
      memcpy(it.reorder_hpar_own, req->reorder_has_parent, n * sizeof *it.reorder_hpar_own);
      it.ev.guild_id = g;
      it.ev.reorder_ids = it.reorder_ids_own;
      it.ev.reorder_positions = it.reorder_pos_own;
      it.ev.reorder_parents = it.reorder_par_own;
      it.ev.reorder_has_parent = it.reorder_hpar_own;
      it.ev.reorder_count = n;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_EDIT_GUILD: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (req->has_system_channel && req->system_channel_id != CCORD_ID_NONE) {
        size_t sci = admin_channel_index(m, req->system_channel_id);
        if (sci == SIZE_MAX || m->channels[sci].guild_id != g || m->channels[sci].kind != CCORD_CHANNEL_TEXT) {
          *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid system channel", err);
          return true;
        }
      }
      if (req->has_afk_channel && req->afk_channel_id != CCORD_ID_NONE) {
        size_t aci = admin_channel_index(m, req->afk_channel_id);
        if (aci == SIZE_MAX || m->channels[aci].guild_id != g || m->channels[aci].kind != CCORD_CHANNEL_VOICE ||
            m->channels[aci].discord_type == CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE) {
          *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid AFK channel", err);
          return true;
        }
      }
      if (req->has_rules_channel && req->rules_channel_id != CCORD_ID_NONE) {
        size_t rci = admin_channel_index(m, req->rules_channel_id);
        if (rci == SIZE_MAX || m->channels[rci].guild_id != g || m->channels[rci].kind != CCORD_CHANNEL_TEXT) {
          *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid rules channel", err);
          return true;
        }
      }
      if (req->has_public_updates_channel && req->public_updates_channel_id != CCORD_ID_NONE) {
        size_t puci = admin_channel_index(m, req->public_updates_channel_id);
        if (puci == SIZE_MAX || m->channels[puci].guild_id != g || m->channels[puci].kind != CCORD_CHANNEL_TEXT) {
          *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid community updates channel", err);
          return true;
        }
      }
      ccord_guild *gd = &m->guilds[gi];
      ccord_audit_entry changes_tmp = {0};
      char detail[128] = "";
      if (req->has_guild_name || req->name[0] != '\0') {
        const char *old_n = gd->name ? gd->name : "";
        const char *name = admin_owned(m, req->name);
        if (!name) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        mock_audit_push_change(&changes_tmp, "name", old_n, name);
        (void)snprintf(detail, sizeof detail, "name: %.40s -> %.40s", old_n, name);
        gd->name = name;
      }
      if (req->has_guild_icon) {
        const char *old_icon = gd->icon_url ? gd->icon_url : "";
        if (req->clear_guild_icon) {
          gd->icon_url = "";
          mock_audit_push_change(&changes_tmp, "icon_hash", old_icon, "");
          if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "icon_hash: cleared");
        } else {
          char url[96];
          (void)snprintf(url, sizeof url, "https://cdn.discordapp.com/icons/%llu/icon_%zu.png",
                         (unsigned long long)g, m->admin_count);
          const char *owned_url = admin_owned(m, url);
          if (!owned_url) {
            *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
            return true;
          }
          gd->icon_url = owned_url;
          mock_audit_push_change(&changes_tmp, "icon_hash", old_icon, owned_url);
          if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "icon_hash: updated");
        }
      }
      if (req->has_guild_banner) {
        char old_b[256];
        memcpy(old_b, gd->banner_url, sizeof old_b);
        if (req->clear_guild_banner) {
          gd->banner_url[0] = '\0';
          mock_audit_push_change(&changes_tmp, "banner_hash", old_b, "");
          if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "banner_hash: cleared");
        } else {
          (void)snprintf(gd->banner_url, sizeof gd->banner_url,
                         "https://cdn.discordapp.com/banners/%llu/banner_%zu.png",
                         (unsigned long long)g, m->admin_count);
          mock_audit_push_change(&changes_tmp, "banner_hash", old_b, gd->banner_url);
          if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "banner_hash: updated");
        }
      }
      if (req->has_system_channel) {
        char ob[32], nb[32];
        ccord_id_format(gd->system_channel_id, ob);
        ccord_id_format(req->system_channel_id, nb);
        mock_audit_push_change(&changes_tmp, "system_channel_id", gd->system_channel_id ? ob : "",
                               req->system_channel_id ? nb : "");
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "system_channel_id: %s -> %s", ob, nb);
        gd->system_channel_id = req->system_channel_id;
      }
      if (req->guild_system_flags >= 0) {
        char ob[24], nb[24];
        (void)snprintf(ob, sizeof ob, "%u", (unsigned)gd->system_channel_flags);
        (void)snprintf(nb, sizeof nb, "%u", (unsigned)req->guild_system_flags);
        mock_audit_push_change(&changes_tmp, "system_channel_flags", ob, nb);
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "system_channel_flags: %s -> %s", ob, nb);
        gd->system_channel_flags = (uint32_t)req->guild_system_flags;
      }
      if (req->guild_default_notify >= 0) {
        char ob[16], nb[16];
        (void)snprintf(ob, sizeof ob, "%u", (unsigned)gd->default_message_notifications);
        (void)snprintf(nb, sizeof nb, "%u", (unsigned)req->guild_default_notify);
        mock_audit_push_change(&changes_tmp, "default_message_notifications", ob, nb);
        if (detail[0] == '\0') {
          (void)snprintf(detail, sizeof detail, "default_message_notifications: %s -> %s", ob, nb);
        }
        gd->default_message_notifications = (uint8_t)req->guild_default_notify;
      }
      if (req->has_afk_channel) {
        char ob[32], nb[32];
        ccord_id_format(gd->afk_channel_id, ob);
        ccord_id_format(req->afk_channel_id, nb);
        mock_audit_push_change(&changes_tmp, "afk_channel_id", gd->afk_channel_id ? ob : "",
                               req->afk_channel_id ? nb : "");
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "afk_channel_id: %s -> %s", ob, nb);
        gd->afk_channel_id = req->afk_channel_id;
      }
      if (req->guild_afk_timeout >= 0) {
        char ob[24], nb[24];
        (void)snprintf(ob, sizeof ob, "%u", (unsigned)gd->afk_timeout);
        (void)snprintf(nb, sizeof nb, "%u", (unsigned)req->guild_afk_timeout);
        mock_audit_push_change(&changes_tmp, "afk_timeout", ob, nb);
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "afk_timeout: %s -> %s", ob, nb);
        gd->afk_timeout = (uint32_t)req->guild_afk_timeout;
      }
      if (req->guild_verification_level >= 0) {
        char ob[16], nb[16];
        (void)snprintf(ob, sizeof ob, "%u", (unsigned)gd->verification_level);
        (void)snprintf(nb, sizeof nb, "%u", (unsigned)req->guild_verification_level);
        mock_audit_push_change(&changes_tmp, "verification_level", ob, nb);
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "verification_level: %s -> %s", ob, nb);
        gd->verification_level = (uint8_t)req->guild_verification_level;
      }
      if (req->guild_explicit_filter >= 0) {
        char ob[16], nb[16];
        (void)snprintf(ob, sizeof ob, "%u", (unsigned)gd->explicit_content_filter);
        (void)snprintf(nb, sizeof nb, "%u", (unsigned)req->guild_explicit_filter);
        mock_audit_push_change(&changes_tmp, "explicit_content_filter", ob, nb);
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "explicit_content_filter: %s -> %s", ob, nb);
        gd->explicit_content_filter = (uint8_t)req->guild_explicit_filter;
      }
      if (req->has_community) {
        const char *of = gd->feature_community ? "COMMUNITY" : "";
        const char *nf = req->feature_community ? "COMMUNITY" : "";
        mock_audit_push_change(&changes_tmp, "features", of, nf);
        if (detail[0] == '\0') {
          (void)snprintf(detail, sizeof detail, "features: %s", req->feature_community ? "+COMMUNITY" : "-COMMUNITY");
        }
        gd->feature_community = req->feature_community;
      }
      if (req->has_rules_channel) {
        char ob[32], nb[32];
        ccord_id_format(gd->rules_channel_id, ob);
        ccord_id_format(req->rules_channel_id, nb);
        mock_audit_push_change(&changes_tmp, "rules_channel_id", gd->rules_channel_id ? ob : "",
                               req->rules_channel_id ? nb : "");
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "rules_channel_id: %s -> %s", ob, nb);
        gd->rules_channel_id = req->rules_channel_id;
      }
      if (req->has_public_updates_channel) {
        char ob[32], nb[32];
        ccord_id_format(gd->public_updates_channel_id, ob);
        ccord_id_format(req->public_updates_channel_id, nb);
        mock_audit_push_change(&changes_tmp, "public_updates_channel_id", gd->public_updates_channel_id ? ob : "",
                               req->public_updates_channel_id ? nb : "");
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "public_updates_channel_id: %s -> %s", ob, nb);
        gd->public_updates_channel_id = req->public_updates_channel_id;
      }
      gd->has_guild_settings = true;
      *out = mock_audit_add(m, g, 1, g, gd->name, req->reason, detail[0] ? detail : "server updated");
      if (*out == CCORD_OK && changes_tmp.change_count > 0 && m->naudit > 0) {
        m->audit[m->naudit - 1].e.change_count = changes_tmp.change_count;
        memcpy(m->audit[m->naudit - 1].e.changes, changes_tmp.changes, sizeof changes_tmp.changes);
      }
      if (*out == CCORD_OK) *out = mock_emit_guild_upsert(m, gi, req->op, err);
      return true;
    }
    case CCORD_ADMIN_SET_INCIDENT_ACTIONS: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      ccord_guild *gd = &m->guilds[gi];
      gd->has_incidents_data = true;
      gd->invites_disabled_until_ms = req->invites_disabled_until_ms;
      gd->dms_disabled_until_ms = req->dms_disabled_until_ms;
      const char *det = req->invites_disabled_until_ms > 0 ? "invites_disabled_until: paused"
                      : req->dms_disabled_until_ms > 0     ? "dms_disabled_until: paused"
                                                           : "invites_disabled_until: active";
      *out = mock_audit_add(m, g, 1, g, gd->name, req->reason, det);
      if (*out != CCORD_OK) return true;
      mock_item it = make_item(CCORD_NET_GUILD_INCIDENTS, now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.invites_disabled_until_ms = req->invites_disabled_until_ms;
      it.ev.dms_disabled_until_ms = req->dms_disabled_until_ms;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_SET_VANITY_URL: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      ccord_guild *gd = &m->guilds[gi];
      if (!gd->feature_vanity_url && gd->premium_tier < 3u) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION,
                                  "Missing Access (Vanity URL requires Server Boost Level 3)", err);
        return true;
      }
      char old_code[64];
      memcpy(old_code, gd->vanity_url_code, sizeof old_code);
      (void)snprintf(gd->vanity_url_code, sizeof gd->vanity_url_code, "%s", req->code);
      const char *own_code = admin_owned(m, req->code);
      if (!own_code) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      char det[160];
      (void)snprintf(det, sizeof det, "vanity_url_code: %s -> %s", old_code, req->code);
      *out = mock_audit_add(m, g, 1, g, gd->name, req->reason, det);
      if (*out != CCORD_OK) return true;
      mock_item it = make_item(CCORD_NET_GUILD_VANITY, now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.invite_code = own_code;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_LEAVE_GUILD: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (m->guilds[gi].owner_id != CCORD_ID_NONE && m->guilds[gi].owner_id == m->self_id) {
        *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid Guild (Owner cannot leave guild)", err);
        return true;
      }
      mock_remove_guild(m, gi);
      mock_item it = make_item(CCORD_NET_GUILD_DELETE, now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.admin_op = req->op;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_DELETE_GUILD: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (m->guilds[gi].owner_id == CCORD_ID_NONE || m->guilds[gi].owner_id != m->self_id) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, "Only the server owner can delete this server", err);
        return true;
      }
      mock_remove_guild(m, gi);
      mock_item it = make_item(CCORD_NET_GUILD_DELETE, now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.admin_op = req->op;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_TRANSFER_OWNERSHIP: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (m->guilds[gi].owner_id == CCORD_ID_NONE || m->guilds[gi].owner_id != m->self_id) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, "Only the server owner can transfer ownership", err);
        return true;
      }
      if (req->user_id == CCORD_ID_NONE || req->user_id == m->self_id || admin_member_index(m, g, req->user_id) < 0) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_member, err);
        return true;
      }
      char ob[32], nb[32], det[96];
      ccord_id_format(m->guilds[gi].owner_id, ob);
      ccord_id_format(req->user_id, nb);
      (void)snprintf(det, sizeof det, "owner_id: %s -> %s", ob, nb);
      m->guilds[gi].owner_id = req->user_id;
      m->guilds[gi].self_permissions = mock_self_perms(m, g);
      m->guilds[gi].has_guild_settings = true;
      *out = mock_audit_add(m, g, 1, g, m->guilds[gi].name, req->reason, det);
      if (*out == CCORD_OK) *out = mock_emit_guild_upsert(m, gi, req->op, err);
      return true;
    }
    case CCORD_ADMIN_LIST_EMOJIS: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!ccord_perm_can_create_expression(mock_self_perms(m, g))) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      mock_item it = make_item(CCORD_NET_GUILD_EXPRESSIONS, now_of(m) + m->cfg.latency_ms);
      it.emojis_own = ccord_alloc_array(m->nemojis ? m->nemojis : 1u, sizeof *it.emojis_own);
      if (!it.emojis_own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      size_t k = 0;
      for (size_t i = 0; i < m->nemojis; i++) {
        if (m->emojis[i].guild_id == g) it.emojis_own[k++] = m->emojis[i];
      }
      it.ev.guild_id = g;
      it.ev.emojis = it.emojis_own;
      it.ev.emoji_count = k;
      it.ev.replace_emojis = true;
      it.ev.expressions_admin = true;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_CREATE_EMOJI: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!ccord_perm_can_create_expression(mock_self_perms(m, g))) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      uint32_t slot_cnt = 0;
      size_t ins = m->nemojis;
      for (size_t i = 0; i < m->nemojis; i++) {
        if (m->emojis[i].guild_id == g) {
          if (m->emojis[i].animated == req->emoji_animated) slot_cnt++;
          ins = i + 1u;
        }
      }
      uint32_t limit = ccord_guild_emoji_slot_limit(m->guilds[gi].premium_tier);
      if (slot_cnt >= limit || m->nemojis >= MOCK_EMOJIS_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_RANGE,
                                  req->emoji_animated ? "Maximum number of animated emojis reached"
                                                      : "Maximum number of emojis reached",
                                  err);
        return true;
      }
      const char *name = admin_owned(m, req->name);
      if (!name) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      ccord_id id = 70000u + (m->next_admin_id++);
      memmove(&m->emojis[ins + 1u], &m->emojis[ins], (m->nemojis - ins) * sizeof m->emojis[0]);
      m->emojis[ins] = (ccord_emoji){
        .id = id, .guild_id = g, .name = name, .animated = req->emoji_animated, .available = true,
        .user_id = m->self_id, .user_name = m->users[0].name,
      };
      m->nemojis++;
      *out = mock_audit_add(m, g, 60, id, name, req->reason, name);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_EMOJI_UPSERT, now_of(m) + m->cfg.latency_ms);
        it.emojis_own = ccord_alloc_array(1u, sizeof *it.emojis_own);
        if (!it.emojis_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.emojis_own[0] = m->emojis[ins];
        it.ev.guild_id = g;
        it.ev.emojis = it.emojis_own;
        it.ev.emoji_count = 1u;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_EDIT_EMOJI: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t ei = SIZE_MAX;
      for (size_t i = 0; i < m->nemojis; i++) {
        if (m->emojis[i].guild_id == g && m->emojis[i].id == req->expr_id) { ei = i; break; }
      }
      if (ei == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Emoji", err);
        return true;
      }
      if (!ccord_perm_can_modify_expression(mock_self_perms(m, g), m->self_id, m->emojis[ei].user_id)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      const char *name = admin_owned(m, req->name);
      if (!name) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      char det[128];
      (void)snprintf(det, sizeof det, "name: %.40s -> %.40s", m->emojis[ei].name, name);
      m->emojis[ei].name = name;
      *out = mock_audit_add(m, g, 61, req->expr_id, name, req->reason, det);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_EMOJI_UPSERT, now_of(m) + m->cfg.latency_ms);
        it.emojis_own = ccord_alloc_array(1u, sizeof *it.emojis_own);
        if (!it.emojis_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.emojis_own[0] = m->emojis[ei];
        it.ev.guild_id = g;
        it.ev.emojis = it.emojis_own;
        it.ev.emoji_count = 1u;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_DELETE_EMOJI: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t ei = SIZE_MAX;
      for (size_t i = 0; i < m->nemojis; i++) {
        if (m->emojis[i].guild_id == g && m->emojis[i].id == req->expr_id) { ei = i; break; }
      }
      if (ei == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Emoji", err);
        return true;
      }
      if (!ccord_perm_can_modify_expression(mock_self_perms(m, g), m->self_id, m->emojis[ei].user_id)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      const char *old_name = m->emojis[ei].name;
      memmove(&m->emojis[ei], &m->emojis[ei + 1u], (m->nemojis - ei - 1u) * sizeof m->emojis[0]);
      m->nemojis--;
      *out = mock_audit_add(m, g, 62, req->expr_id, old_name, req->reason, old_name);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_EMOJI_DELETE, now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.expr_id = req->expr_id;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_LIST_STICKERS: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!ccord_perm_can_create_expression(mock_self_perms(m, g))) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      mock_item it = make_item(CCORD_NET_GUILD_EXPRESSIONS, now_of(m) + m->cfg.latency_ms);
      it.stickers_own = ccord_alloc_array(m->nstickers ? m->nstickers : 1u, sizeof *it.stickers_own);
      if (!it.stickers_own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      size_t k = 0;
      for (size_t i = 0; i < m->nstickers; i++) {
        if (m->stickers[i].guild_id == g) it.stickers_own[k++] = m->stickers[i];
      }
      it.ev.guild_id = g;
      it.ev.stickers = it.stickers_own;
      it.ev.sticker_count = k;
      it.ev.replace_stickers = true;
      it.ev.expressions_admin = true;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_CREATE_STICKER: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!ccord_perm_can_create_expression(mock_self_perms(m, g))) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      uint32_t slot_cnt = 0;
      size_t ins = m->nstickers;
      for (size_t i = 0; i < m->nstickers; i++) {
        if (m->stickers[i].guild_id == g) {
          slot_cnt++;
          ins = i + 1u;
        }
      }
      uint32_t limit = ccord_guild_sticker_slot_limit(m->guilds[gi].premium_tier);
      if (slot_cnt >= limit || m->nstickers >= MOCK_STICKERS_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of stickers reached", err);
        return true;
      }
      const char *name = admin_owned(m, req->name);
      const char *desc = admin_owned(m, req->sticker_desc);
      const char *tags = admin_owned(m, req->sticker_tags);
      if (!name || !desc || !tags) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      ccord_id id = 70000u + (m->next_admin_id++);
      memmove(&m->stickers[ins + 1u], &m->stickers[ins], (m->nstickers - ins) * sizeof m->stickers[0]);
      m->stickers[ins] = (ccord_sticker){
        .id = id, .guild_id = g, .name = name, .description = desc, .tags = tags,
        .format = req->sticker_format ? req->sticker_format : CCORD_STICKER_PNG, .available = true,
        .user_id = m->self_id, .user_name = m->users[0].name,
      };
      m->nstickers++;
      *out = mock_audit_add(m, g, 90, id, name, req->reason, name);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_STICKER_UPSERT, now_of(m) + m->cfg.latency_ms);
        it.stickers_own = ccord_alloc_array(1u, sizeof *it.stickers_own);
        if (!it.stickers_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.stickers_own[0] = m->stickers[ins];
        it.ev.guild_id = g;
        it.ev.stickers = it.stickers_own;
        it.ev.sticker_count = 1u;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_EDIT_STICKER: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t si = SIZE_MAX;
      for (size_t i = 0; i < m->nstickers; i++) {
        if (m->stickers[i].guild_id == g && m->stickers[i].id == req->expr_id) { si = i; break; }
      }
      if (si == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Sticker", err);
        return true;
      }
      if (!ccord_perm_can_modify_expression(mock_self_perms(m, g), m->self_id, m->stickers[si].user_id)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      char det[128];
      det[0] = '\0';
      if (req->name[0] != '\0') {
        const char *name = admin_owned(m, req->name);
        if (!name) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        (void)snprintf(det, sizeof det, "name: %.40s -> %.40s", m->stickers[si].name, name);
        m->stickers[si].name = name;
      }
      if (req->has_sticker_desc) {
        const char *desc = admin_owned(m, req->sticker_desc);
        if (!desc) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        if (det[0] == '\0') (void)snprintf(det, sizeof det, "description: %.40s -> %.40s", m->stickers[si].description, desc);
        m->stickers[si].description = desc;
      }
      if (req->has_sticker_tags) {
        const char *tags = admin_owned(m, req->sticker_tags);
        if (!tags) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        if (det[0] == '\0') (void)snprintf(det, sizeof det, "tags: %.40s -> %.40s", m->stickers[si].tags, tags);
        m->stickers[si].tags = tags;
      }
      *out = mock_audit_add(m, g, 91, req->expr_id, m->stickers[si].name, req->reason, det);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_STICKER_UPSERT, now_of(m) + m->cfg.latency_ms);
        it.stickers_own = ccord_alloc_array(1u, sizeof *it.stickers_own);
        if (!it.stickers_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.stickers_own[0] = m->stickers[si];
        it.ev.guild_id = g;
        it.ev.stickers = it.stickers_own;
        it.ev.sticker_count = 1u;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_DELETE_STICKER: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t si = SIZE_MAX;
      for (size_t i = 0; i < m->nstickers; i++) {
        if (m->stickers[i].guild_id == g && m->stickers[i].id == req->expr_id) { si = i; break; }
      }
      if (si == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Sticker", err);
        return true;
      }
      if (!ccord_perm_can_modify_expression(mock_self_perms(m, g), m->self_id, m->stickers[si].user_id)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      const char *old_name = m->stickers[si].name;
      memmove(&m->stickers[si], &m->stickers[si + 1u], (m->nstickers - si - 1u) * sizeof m->stickers[0]);
      m->nstickers--;
      *out = mock_audit_add(m, g, 92, req->expr_id, old_name, req->reason, old_name);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_STICKER_DELETE, now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.expr_id = req->expr_id;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_LIST_SOUNDBOARD: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!ccord_perm_can_create_expression(mock_self_perms(m, g))) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      mock_item it = make_item(CCORD_NET_GUILD_EXPRESSIONS, now_of(m) + m->cfg.latency_ms);
      it.soundboard_own = ccord_alloc_array(m->nsoundboard ? m->nsoundboard : 1u, sizeof *it.soundboard_own);
      if (!it.soundboard_own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      size_t k = 0;
      for (size_t i = 0; i < m->nsoundboard; i++) {
        if (m->soundboard[i].guild_id == g) it.soundboard_own[k++] = m->soundboard[i];
      }
      it.ev.guild_id = g;
      it.ev.soundboard = it.soundboard_own;
      it.ev.soundboard_count = k;
      it.ev.replace_soundboard = true;
      it.ev.expressions_admin = true;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_CREATE_SOUNDBOARD: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!ccord_perm_can_create_expression(mock_self_perms(m, g))) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      uint32_t slot_cnt = 0;
      size_t ins = m->nsoundboard;
      for (size_t i = 0; i < m->nsoundboard; i++) {
        if (m->soundboard[i].guild_id == g) {
          slot_cnt++;
          ins = i + 1u;
        }
      }
      uint32_t limit = ccord_guild_soundboard_slot_limit(m->guilds[gi].premium_tier);
      if (slot_cnt >= limit || m->nsoundboard >= MOCK_SOUNDBOARD_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of soundboard sounds reached", err);
        return true;
      }
      const char *name = admin_owned(m, req->name);
      const char *em_name = req->has_sound_emoji ? admin_owned(m, req->sound_emoji_name) : "";
      if (!name || !em_name) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      ccord_id id = 70000u + (m->next_admin_id++);
      memmove(&m->soundboard[ins + 1u], &m->soundboard[ins], (m->nsoundboard - ins) * sizeof m->soundboard[0]);
      m->soundboard[ins] = (ccord_soundboard_sound){
        .id = id, .guild_id = g, .name = name,
        .volume_pct = req->sound_volume_pct >= 0 ? (uint8_t)req->sound_volume_pct : 100u,
        .emoji_id = req->has_sound_emoji ? req->sound_emoji_id : CCORD_ID_NONE,
        .emoji_name = em_name,
        .user_id = m->self_id, .user_name = m->users[0].name, .available = true,
      };
      m->nsoundboard++;
      *out = mock_audit_add(m, g, 130, id, name, req->reason, name);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_SOUNDBOARD_UPSERT, now_of(m) + m->cfg.latency_ms);
        it.soundboard_own = ccord_alloc_array(1u, sizeof *it.soundboard_own);
        if (!it.soundboard_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.soundboard_own[0] = m->soundboard[ins];
        it.ev.guild_id = g;
        it.ev.soundboard = it.soundboard_own;
        it.ev.soundboard_count = 1u;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_EDIT_SOUNDBOARD: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t sbi = SIZE_MAX;
      for (size_t i = 0; i < m->nsoundboard; i++) {
        if (m->soundboard[i].guild_id == g && m->soundboard[i].id == req->expr_id) { sbi = i; break; }
      }
      if (sbi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Soundboard Sound", err);
        return true;
      }
      if (!ccord_perm_can_modify_expression(mock_self_perms(m, g), m->self_id, m->soundboard[sbi].user_id)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      char det[128];
      det[0] = '\0';
      if (req->name[0] != '\0') {
        const char *name = admin_owned(m, req->name);
        if (!name) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        (void)snprintf(det, sizeof det, "name: %.40s -> %.40s", m->soundboard[sbi].name, name);
        m->soundboard[sbi].name = name;
      }
      if (req->sound_volume_pct >= 0) {
        if (det[0] == '\0') {
          (void)snprintf(det, sizeof det, "volume: %u -> %d", (unsigned)m->soundboard[sbi].volume_pct,
                         req->sound_volume_pct);
        }
        m->soundboard[sbi].volume_pct = (uint8_t)req->sound_volume_pct;
      }
      if (req->has_sound_emoji) {
        const char *em_name = admin_owned(m, req->sound_emoji_name);
        if (!em_name) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        if (det[0] == '\0') {
          (void)snprintf(det, sizeof det, "emoji_name: %.30s -> %.30s", m->soundboard[sbi].emoji_name, em_name);
        }
        m->soundboard[sbi].emoji_id = req->sound_emoji_id;
        m->soundboard[sbi].emoji_name = em_name;
      }
      *out = mock_audit_add(m, g, 131, req->expr_id, m->soundboard[sbi].name, req->reason, det);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_SOUNDBOARD_UPSERT, now_of(m) + m->cfg.latency_ms);
        it.soundboard_own = ccord_alloc_array(1u, sizeof *it.soundboard_own);
        if (!it.soundboard_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.soundboard_own[0] = m->soundboard[sbi];
        it.ev.guild_id = g;
        it.ev.soundboard = it.soundboard_own;
        it.ev.soundboard_count = 1u;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_DELETE_SOUNDBOARD: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t sbi = SIZE_MAX;
      for (size_t i = 0; i < m->nsoundboard; i++) {
        if (m->soundboard[i].guild_id == g && m->soundboard[i].id == req->expr_id) { sbi = i; break; }
      }
      if (sbi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Soundboard Sound", err);
        return true;
      }
      if (!ccord_perm_can_modify_expression(mock_self_perms(m, g), m->self_id, m->soundboard[sbi].user_id)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      const char *old_name = m->soundboard[sbi].name;
      memmove(&m->soundboard[sbi], &m->soundboard[sbi + 1u], (m->nsoundboard - sbi - 1u) * sizeof m->soundboard[0]);
      m->nsoundboard--;
      *out = mock_audit_add(m, g, 132, req->expr_id, old_name, req->reason, old_name);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_SOUNDBOARD_DELETE, now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.expr_id = req->expr_id;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_PREVIEW_PRUNE:
    case CCORD_ADMIN_EXECUTE_PRUNE: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_KICK_MEMBERS)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      for (size_t i = 0u; i < req->prune_role_count; i++) {
        if (!mock_may_edit_role(m, g, req->prune_role_ids[i], true)) {
          *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
          return true;
        }
      }
      if (req->op == CCORD_ADMIN_PREVIEW_PRUNE) {
        int32_t cnt = 0;
        for (size_t i = 0u; i < m->nmembers; i++) {
          if (mock_member_prunable(m, g, i, req->prune_days, req->prune_role_ids, req->prune_role_count)) cnt++;
        }
        mock_item it = make_item(CCORD_NET_PRUNE_RESULT, now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.prune_info = (ccord_prune_info){
          .pruned = cnt, .days = req->prune_days, .executed = false, .has_result = true
        };
        *out = push_item(m, it, err);
        return true;
      }
      ccord_id removed[MOCK_NUM_USERS];
      size_t nrem = 0u;
      for (size_t i = 0u; i < m->nmembers; ) {
        if (mock_member_prunable(m, g, i, req->prune_days, req->prune_role_ids, req->prune_role_count)) {
          ccord_id uid = m->members[i].user_id;
          if (nrem < MOCK_NUM_USERS) removed[nrem++] = uid;
          mock_mrow *row = mock_mrow_find(m, g, uid);
          if (row) row->present = false;
          memmove(&m->members[i], &m->members[i + 1u], (m->nmembers - i - 1u) * sizeof *m->members);
          m->nmembers--;
        } else {
          i++;
        }
      }
      mock_sync_member_roles(m);
      m->snap.member_count = m->nmembers;
      char det[48];
      (void)snprintf(det, sizeof det, "pruned: %zu", nrem);
      *out = mock_audit_add(m, g, 21, CCORD_ID_NONE, "", req->reason, det);
      for (size_t i = 0u; *out == CCORD_OK && i < nrem; i++) {
        mock_item rm = make_item(CCORD_NET_MEMBER_REMOVE, now_of(m) + m->cfg.latency_ms);
        rm.ev.guild_id = g;
        rm.ev.user_id = removed[i];
        *out = push_item(m, rm, err);
      }
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_PRUNE_RESULT, now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.prune_info = (ccord_prune_info){
          .pruned = req->prune_compute_count ? (int32_t)nrem : -1,
          .days = req->prune_days,
          .executed = true,
          .has_result = true,
        };
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_LIST_AUTOMOD_RULES: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t cnt = 0u;
      for (size_t i = 0u; i < m->nautomod; i++) {
        if (m->automod[i].guild_id == g) cnt++;
      }
      mock_item it = make_item(CCORD_NET_AUTOMOD_LIST, now_of(m) + m->cfg.latency_ms);
      if (cnt > 0u) {
        it.automod_own = ccord_alloc_array(cnt, sizeof *it.automod_own);
        if (!it.automod_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        size_t k = 0u;
        for (size_t i = 0u; i < m->nautomod; i++) {
          if (m->automod[i].guild_id == g) it.automod_own[k++] = m->automod[i];
        }
      }
      it.ev.guild_id = g;
      it.ev.automod_rules = it.automod_own;
      it.ev.automod_rule_count = cnt;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_CREATE_AUTOMOD_RULE: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (m->nautomod >= MOCK_AUTOMOD_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of AutoMod rules reached", err);
        return true;
      }
      ccord_automod_rule rule = {
        .id = m->next_automod_id++,
        .guild_id = g,
        .creator_id = m->self_id,
        .name = "AutoMod Rule",
        .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
        .enabled = true,
        .custom_message = "",
      };
      if (!mock_parse_automod_body(m, req->body, true, &rule)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid AutoMod rule payload", err);
        return true;
      }
      m->automod[m->nautomod++] = rule;
      *out = mock_audit_add(m, g, 140, rule.id, rule.name, req->reason, rule.name);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_AUTOMOD_UPSERT, now_of(m) + m->cfg.latency_ms);
        it.automod_own = ccord_alloc_array(1u, sizeof *it.automod_own);
        if (!it.automod_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.automod_own[0] = rule;
        it.ev.guild_id = g;
        it.ev.automod_rules = it.automod_own;
        it.ev.automod_rule_count = 1u;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_EDIT_AUTOMOD_RULE: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t ri = SIZE_MAX;
      for (size_t i = 0u; i < m->nautomod; i++) {
        if (m->automod[i].guild_id == g && m->automod[i].id == req->expr_id) { ri = i; break; }
      }
      if (ri == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown AutoMod Rule", err);
        return true;
      }
      ccord_automod_rule updated = m->automod[ri];
      if (!mock_parse_automod_body(m, req->body, false, &updated)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid AutoMod rule payload", err);
        return true;
      }
      m->automod[ri] = updated;
      *out = mock_audit_add(m, g, 141, updated.id, updated.name, req->reason, updated.name);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_AUTOMOD_UPSERT, now_of(m) + m->cfg.latency_ms);
        it.automod_own = ccord_alloc_array(1u, sizeof *it.automod_own);
        if (!it.automod_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.automod_own[0] = updated;
        it.ev.guild_id = g;
        it.ev.automod_rules = it.automod_own;
        it.ev.automod_rule_count = 1u;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_DELETE_AUTOMOD_RULE: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t ri = SIZE_MAX;
      for (size_t i = 0u; i < m->nautomod; i++) {
        if (m->automod[i].guild_id == g && m->automod[i].id == req->expr_id) { ri = i; break; }
      }
      if (ri == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown AutoMod Rule", err);
        return true;
      }
      const char *old_name = m->automod[ri].name;
      memmove(&m->automod[ri], &m->automod[ri + 1u], (m->nautomod - ri - 1u) * sizeof m->automod[0]);
      m->nautomod--;
      *out = mock_audit_add(m, g, 142, req->expr_id, old_name, req->reason, old_name);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_AUTOMOD_DELETE, now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.expr_id = req->expr_id;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_LIST_WEBHOOKS: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_WEBHOOKS)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t cnt = 0u;
      for (size_t i = 0u; i < m->nwebhooks; i++) {
        if (m->webhooks[i].guild_id == g) cnt++;
      }
      mock_item it = make_item(CCORD_NET_WEBHOOK_LIST, now_of(m) + m->cfg.latency_ms);
      if (cnt > 0u) {
        it.webhooks_own = ccord_alloc_array(cnt, sizeof *it.webhooks_own);
        if (!it.webhooks_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        size_t k = 0u;
        for (size_t i = 0u; i < m->nwebhooks; i++) {
          if (m->webhooks[i].guild_id == g) it.webhooks_own[k++] = m->webhooks[i];
        }
      }
      it.ev.guild_id = g;
      it.ev.webhooks = it.webhooks_own;
      it.ev.webhook_count = cnt;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_CREATE_WEBHOOK: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t chi = admin_channel_index(m, req->channel_id);
      if (chi == SIZE_MAX || m->channels[chi].guild_id != g || m->channels[chi].kind != CCORD_CHANNEL_TEXT ||
          ccord_discord_type_is_thread(m->channels[chi].discord_type)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid Webhook Channel", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_WEBHOOKS)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (m->nwebhooks >= MOCK_WEBHOOKS_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of webhooks reached", err);
        return true;
      }
      ccord_id wid = m->next_webhook_id++;
      char tok[48];
      (void)snprintf(tok, sizeof tok, "mock_wh_tok_%llu", (unsigned long long)wid);
      const char *nm = mock_arena_dup_cstr(&m->automod_arena, req->name, strlen(req->name));
      const char *tk = mock_arena_dup_cstr(&m->automod_arena, tok, strlen(tok));
      const char *av = (req->has_webhook_avatar && !req->clear_webhook_avatar) ? "wh_avatar" : "";
      ccord_webhook wh = {
        .id = wid,
        .type = CCORD_WEBHOOK_INCOMING,
        .guild_id = g,
        .channel_id = req->channel_id,
        .creator_id = m->self_id,
        .creator_name = user_names[0],
        .name = nm,
        .avatar = av,
        .token = tk,
        .source_guild_name = "",
        .source_channel_name = "",
      };
      m->webhooks[m->nwebhooks++] = wh;
      *out = mock_audit_add(m, g, 50, wh.id, wh.name, req->reason, wh.name);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_WEBHOOK_UPSERT, now_of(m) + m->cfg.latency_ms);
        it.webhooks_own = ccord_alloc_array(1u, sizeof *it.webhooks_own);
        if (!it.webhooks_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.webhooks_own[0] = wh;
        it.ev.guild_id = g;
        it.ev.webhooks = it.webhooks_own;
        it.ev.webhook_count = 1u;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_EDIT_WEBHOOK: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_WEBHOOKS)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t wi = SIZE_MAX;
      for (size_t i = 0u; i < m->nwebhooks; i++) {
        if (m->webhooks[i].guild_id == g && m->webhooks[i].id == req->expr_id) { wi = i; break; }
      }
      if (wi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Webhook", err);
        return true;
      }
      if (req->target_channel_id != CCORD_ID_NONE) {
        size_t chi = admin_channel_index(m, req->target_channel_id);
        if (chi == SIZE_MAX || m->channels[chi].guild_id != g || m->channels[chi].kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(m->channels[chi].discord_type)) {
          *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid Webhook Channel", err);
          return true;
        }
        m->webhooks[wi].channel_id = req->target_channel_id;
      }
      if (req->name[0] != '\0') {
        m->webhooks[wi].name = mock_arena_dup_cstr(&m->automod_arena, req->name, strlen(req->name));
      }
      if (req->has_webhook_avatar) {
        m->webhooks[wi].avatar = req->clear_webhook_avatar ? "" : "wh_avatar_updated";
      }
      ccord_webhook wh = m->webhooks[wi];
      *out = mock_audit_add(m, g, 51, wh.id, wh.name, req->reason, wh.name);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_WEBHOOK_UPSERT, now_of(m) + m->cfg.latency_ms);
        it.webhooks_own = ccord_alloc_array(1u, sizeof *it.webhooks_own);
        if (!it.webhooks_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.webhooks_own[0] = wh;
        it.ev.guild_id = g;
        it.ev.webhooks = it.webhooks_own;
        it.ev.webhook_count = 1u;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_DELETE_WEBHOOK: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_WEBHOOKS)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t wi = SIZE_MAX;
      for (size_t i = 0u; i < m->nwebhooks; i++) {
        if (m->webhooks[i].guild_id == g && m->webhooks[i].id == req->expr_id) { wi = i; break; }
      }
      if (wi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Webhook", err);
        return true;
      }
      const char *old_name = m->webhooks[wi].name;
      memmove(&m->webhooks[wi], &m->webhooks[wi + 1u], (m->nwebhooks - wi - 1u) * sizeof m->webhooks[0]);
      m->nwebhooks--;
      *out = mock_audit_add(m, g, 52, req->expr_id, old_name, req->reason, old_name);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_WEBHOOK_DELETE, now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.expr_id = req->expr_id;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_FOLLOW_CHANNEL: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t sci = admin_channel_index(m, req->channel_id);
      if (sci == SIZE_MAX || m->channels[sci].discord_type != CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) {
        *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Source channel must be an announcement channel", err);
        return true;
      }
      size_t dci = admin_channel_index(m, req->target_channel_id);
      if (dci == SIZE_MAX || m->channels[dci].guild_id != g || m->channels[dci].kind != CCORD_CHANNEL_TEXT ||
          ccord_discord_type_is_thread(m->channels[dci].discord_type)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid destination channel", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_WEBHOOKS)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (m->nwebhooks >= MOCK_WEBHOOKS_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of webhooks reached", err);
        return true;
      }
      size_t sgi = mock_guild_index(m, m->channels[sci].guild_id);
      const char *sg_name = sgi != SIZE_MAX ? m->guilds[sgi].name : "Server";
      const char *sc_name = m->channels[sci].name;
      char wh_name[CCORD_WEBHOOK_NAME_MAX_BYTES + 1];
      (void)snprintf(wh_name, sizeof wh_name, "%.40s #%.35s", sg_name, sc_name);
      ccord_id wid = m->next_webhook_id++;
      ccord_webhook wh = {
        .id = wid,
        .type = CCORD_WEBHOOK_CHANNEL_FOLLOWER,
        .guild_id = g,
        .channel_id = req->target_channel_id,
        .creator_id = m->self_id,
        .creator_name = user_names[0],
        .name = mock_arena_dup_cstr(&m->automod_arena, wh_name, strlen(wh_name)),
        .avatar = "",
        .token = "",
        .source_guild_id = m->channels[sci].guild_id,
        .source_guild_name = sg_name,
        .source_channel_id = req->channel_id,
        .source_channel_name = sc_name,
      };
      m->webhooks[m->nwebhooks++] = wh;
      *out = mock_audit_add(m, g, 50, wh.id, wh.name, req->reason, wh.name);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_CHANNEL_FOLLOWER_ADDED, now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.channel_id = req->channel_id;
        it.ev.target_channel_id = req->target_channel_id;
        it.ev.expr_id = wid;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_LIST_APP_CMD_PERMS: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD) && !mock_may(m, g, CCORD_PERM_MANAGE_WEBHOOKS)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t cnt = 0u;
      for (size_t i = 0u; i < m->napp_perms; i++) {
        if (m->app_perms[i].guild_id == g) cnt++;
      }
      mock_item it = make_item(CCORD_NET_APP_CMD_PERMS_LIST, now_of(m) + m->cfg.latency_ms);
      if (cnt > 0u) {
        it.app_perms_own = ccord_alloc_array(cnt, sizeof *it.app_perms_own);
        if (!it.app_perms_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        size_t k = 0u;
        for (size_t i = 0u; i < m->napp_perms; i++) {
          if (m->app_perms[i].guild_id == g) it.app_perms_own[k++] = m->app_perms[i];
        }
      }
      it.ev.guild_id = g;
      it.ev.app_cmd_perms = it.app_perms_own;
      it.ev.app_cmd_perms_count = cnt;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_GET_WELCOME_SCREEN:
    case CCORD_ADMIN_EDIT_WELCOME_SCREEN: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (!m->guilds[gi].feature_community) {
        *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Community is not enabled for this server", err);
        return true;
      }
      if (req->op == CCORD_ADMIN_EDIT_WELCOME_SCREEN) {
        ccord_welcome_screen parsed_ws = {0};
        ccord_welcome_channel parsed_chs[CCORD_WELCOME_MAX_CHANNELS] = {0};
        if (!mock_parse_welcome_body(m, g, req->body, &parsed_ws, parsed_chs)) {
          *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid Welcome Screen payload", err);
          return true;
        }
        m->welcome[gi].guild_id = g;
        m->welcome[gi].present = true;
        memcpy(m->welcome[gi].chs, parsed_chs, sizeof parsed_chs);
        m->welcome[gi].ws = parsed_ws;
        m->welcome[gi].ws.channels = parsed_ws.channel_count > 0u ? m->welcome[gi].chs : nullptr;
        *out = mock_audit_add(m, g, 1, g, m->guilds[gi].name, req->reason, "welcome_screen: updated");
        if (*out != CCORD_OK) return true;
      }
      mock_item it = make_item(CCORD_NET_WELCOME_SCREEN, now_of(m) + m->cfg.latency_ms);
      it.welcome_own = ccord_alloc_array(1u, sizeof *it.welcome_own);
      if (!it.welcome_own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      if (m->welcome[gi].present && m->welcome[gi].guild_id == g) {
        *it.welcome_own = m->welcome[gi].ws;
      } else {
        *it.welcome_own = (ccord_welcome_screen){.enabled = false, .description = "", .channels = nullptr, .channel_count = 0u};
      }
      it.ev.guild_id = g;
      it.ev.welcome_screen = it.welcome_own;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_GET_ONBOARDING:
    case CCORD_ADMIN_PUT_ONBOARDING: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (!m->guilds[gi].feature_community) {
        *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Community is not enabled for this server", err);
        return true;
      }
      if (req->op == CCORD_ADMIN_PUT_ONBOARDING) {
        ccord_onboarding parsed_ob = {0};
        if (!mock_parse_onboarding_body(m, g, req->body, &parsed_ob)) {
          *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid Onboarding payload", err);
          return true;
        }
        m->onboarding[gi].guild_id = g;
        m->onboarding[gi].present = true;
        m->onboarding[gi].ob = parsed_ob;
        *out = mock_audit_add(m, g, 167, g, m->guilds[gi].name, req->reason, "onboarding: updated");
        if (*out != CCORD_OK) return true;
      }
      mock_item it = make_item(CCORD_NET_ONBOARDING, now_of(m) + m->cfg.latency_ms);
      it.onboarding_own = ccord_alloc_array(1u, sizeof *it.onboarding_own);
      if (!it.onboarding_own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      if (m->onboarding[gi].present && m->onboarding[gi].guild_id == g) {
        *it.onboarding_own = m->onboarding[gi].ob;
      } else {
        *it.onboarding_own = (ccord_onboarding){.guild_id = g, .enabled = false, .mode = CCORD_ONBOARDING_DEFAULT};
      }
      it.ev.guild_id = g;
      it.ev.onboarding = it.onboarding_own;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_PUT_CHANNEL_OVERWRITE: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t ci = admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].guild_id != g ||
          ccord_discord_type_is_thread(m->channels[ci].discord_type)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
        return true;
      }
      if (req->overwrite.kind == CCORD_OVERWRITE_ROLE) {
        if (mock_role_find(m, g, req->overwrite.id) == SIZE_MAX) {
          *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_role, err);
          return true;
        }
      } else {
        if (admin_member_index(m, g, req->overwrite.id) < 0) {
          *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_member, err);
          return true;
        }
      }
      uint64_t ch_perms = mock_channel_perms(m, g, req->channel_id);
      uint64_t old_allow = 0u, old_deny = 0u;
      size_t oi = mock_find_overwrites(m, req->channel_id);
      size_t at = SIZE_MAX;
      if (oi != SIZE_MAX) {
        for (size_t i = 0u; i < m->overwrites[oi].count; i++) {
          if (m->overwrite_pool[oi][i].id == req->overwrite.id) {
            at = i;
            old_allow = m->overwrite_pool[oi][i].allow;
            old_deny = m->overwrite_pool[oi][i].deny;
            break;
          }
        }
      }
      uint64_t changed = (old_allow ^ req->overwrite.allow) | (old_deny ^ req->overwrite.deny);
      if ((ch_perms & CCORD_PERM_MANAGE_ROLES) == 0u || (changed & ~ch_perms) != 0u) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (oi == SIZE_MAX) oi = mock_ensure_overwrites(m, req->channel_id);
      if (oi == SIZE_MAX || (at == SIZE_MAX && m->overwrites[oi].count >= MOCK_MAX_OVERWRITES_PER_CHAN)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of overwrites reached", err);
        return true;
      }
      bool created = (at == SIZE_MAX);
      if (created) {
        m->overwrite_pool[oi][m->overwrites[oi].count++] = req->overwrite;
      } else {
        m->overwrite_pool[oi][at] = req->overwrite;
      }
      char det[96];
      (void)snprintf(det, sizeof det, "allow: %llu, deny: %llu",
                     (unsigned long long)req->overwrite.allow, (unsigned long long)req->overwrite.deny);
      *out = mock_audit_add(m, g, created ? 13 : 14, req->channel_id, m->channels[ci].name, req->reason, det);
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_CHANNEL_OVERWRITE_UPSERT, now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.channel_id = req->channel_id;
        it.ev.overwrite_item = req->overwrite;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_DELETE_CHANNEL_OVERWRITE: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t ci = admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].guild_id != g ||
          ccord_discord_type_is_thread(m->channels[ci].discord_type)) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
        return true;
      }
      if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_MANAGE_ROLES) == 0u) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t oi = mock_find_overwrites(m, req->channel_id);
      size_t at = SIZE_MAX;
      if (oi != SIZE_MAX) {
        for (size_t i = 0u; i < m->overwrites[oi].count; i++) {
          if (m->overwrite_pool[oi][i].id == req->overwrite.id) {
            at = i;
            break;
          }
        }
      }
      if (at == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Overwrite", err);
        return true;
      }
      memmove(&m->overwrite_pool[oi][at], &m->overwrite_pool[oi][at + 1u],
              (m->overwrites[oi].count - at - 1u) * sizeof m->overwrite_pool[oi][0]);
      m->overwrites[oi].count--;
      *out = mock_audit_add(m, g, 15, req->channel_id, m->channels[ci].name, req->reason, "overwrite deleted");
      if (*out == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_CHANNEL_OVERWRITE_DELETE, now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.channel_id = req->channel_id;
        it.ev.overwrite_item = req->overwrite;
        *out = push_item(m, it, err);
      }
      return true;
    }
    case CCORD_ADMIN_LIST_CHANNEL_INVITES: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t ci = admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].guild_id != g) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
        return true;
      }
      if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      mock_item it = make_item(CCORD_NET_CHANNEL_INVITE_LIST, now_of(m) + m->cfg.latency_ms);
      size_t cnt = 0u;
      for (size_t i = 0u; i < m->ninvites; i++) {
        if (m->invites[i].guild_id == g && m->invites[i].inv.channel_id == req->channel_id) cnt++;
      }
      if (cnt > 0u) {
        it.invites_own = ccord_alloc_array(cnt, sizeof *it.invites_own);
        if (!it.invites_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        size_t k = 0u;
        for (size_t i = 0u; i < m->ninvites; i++) {
          if (m->invites[i].guild_id == g && m->invites[i].inv.channel_id == req->channel_id) {
            it.invites_own[k++] = m->invites[i].inv;
          }
        }
      }
      it.ev.guild_id = g;
      it.ev.channel_id = req->channel_id;
      it.ev.invites = it.invites_own;
      it.ev.invite_count = cnt;
      *out = push_item(m, it, err);
      return true;
    }
    case CCORD_ADMIN_CLONE_CHANNEL: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      uint64_t g_perms = mock_self_perms(m, g);
      if ((g_perms & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (req->parent_id != CCORD_ID_NONE && req->channel_type != 4u) {
        size_t pci = admin_channel_index(m, req->parent_id);
        if (pci == SIZE_MAX || m->channels[pci].guild_id != g || m->channels[pci].kind != CCORD_CHANNEL_CATEGORY) {
          *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid parent category", err);
          return true;
        }
      }
      for (size_t i = 0u; i < req->clone_overwrite_count; i++) {
        const ccord_overwrite *ow = &req->clone_overwrites[i];
        if (((ow->allow | ow->deny) & ~g_perms) != 0u) {
          *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
          return true;
        }
      }
      const char *name = admin_owned(m, req->name);
      const char *topic = req->has_topic ? admin_owned(m, req->topic) : "";
      if (!name || !topic) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      size_t in_guild = 0u;
      for (size_t i = 0u; i < m->nchannels; i++) in_guild += m->channels[i].guild_id == g;
      ccord_channel_kind kind = req->channel_type == 13u ? CCORD_CHANNEL_STAGE
                              : req->channel_type == 2u  ? CCORD_CHANNEL_VOICE
                              : req->channel_type == 4u  ? CCORD_CHANNEL_CATEGORY
                                                         : CCORD_CHANNEL_TEXT;
      ccord_channel ch = {
        .id = m->next_admin_id++,
        .guild_id = g,
        .parent_id = req->channel_type == 4u ? CCORD_ID_NONE : req->parent_id,
        .kind = kind,
        .discord_type = req->channel_type,
        .name = name,
        .topic = topic,
        .position = (int32_t)in_guild,
        .nsfw = req->has_nsfw ? req->nsfw : false,
        .rate_limit_per_user = req->has_slowmode ? req->slowmode_s : 0u,
        .bitrate = req->has_bitrate ? req->bitrate : (kind == CCORD_CHANNEL_VOICE ? 64000u : 0u),
        .user_limit = req->has_user_limit ? req->user_limit : 0u,
        .default_auto_archive_duration = req->has_auto_archive ? req->default_auto_archive_duration : 0u,
      };
      size_t ci = admin_add_channel(m, &ch, err);
      if (ci == SIZE_MAX) {
        *out = CCORD_ERR_NOMEM;
        return true;
      }
      if (req->clone_overwrite_count > 0u) {
        size_t oi = mock_ensure_overwrites(m, ch.id);
        if (oi != SIZE_MAX) {
          size_t ncopy = req->clone_overwrite_count;
          if (ncopy > MOCK_MAX_OVERWRITES_PER_CHAN) ncopy = MOCK_MAX_OVERWRITES_PER_CHAN;
          memcpy(m->overwrite_pool[oi], req->clone_overwrites, ncopy * sizeof m->overwrite_pool[oi][0]);
          m->overwrites[oi].count = ncopy;
        }
      }
      if (req->has_available_tags && ccord_discord_type_is_forum(req->channel_type)) {
        size_t fmi = mock_ensure_forum_meta(m, ch.id);
        if (fmi != SIZE_MAX) {
          m->forum_metas[fmi].tag_count = req->available_tag_count;
          for (uint8_t k = 0u; k < req->available_tag_count; k++) {
            ccord_forum_tag ftag = req->available_tags[k];
            if (ftag.id == CCORD_ID_NONE) ftag.id = 4100u + (m->next_admin_id++);
            m->forum_metas[fmi].tags[k] = ftag;
          }
        }
      }
      *out = mock_audit_add(m, g, 10, ch.id, name, req->reason, "cloned channel");
      if (*out == CCORD_OK) *out = admin_emit_channel_op(m, ci, CCORD_ADMIN_CLONE_CHANNEL, err);
      return true;
    }
    case CCORD_ADMIN_SET_VOICE_MUTE:
    case CCORD_ADMIN_SET_VOICE_DEAF: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      long mi = admin_member_index(m, g, req->user_id);
      if (mi < 0) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_member, err);
        return true;
      }
      long vi = mock_voice_index(m, g, req->user_id);
      if (vi < 0) {
        *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Target user is not connected to voice.", err);
        return true;
      }
      uint64_t need = req->op == CCORD_ADMIN_SET_VOICE_MUTE ? CCORD_PERM_MUTE_MEMBERS : CCORD_PERM_DEAFEN_MEMBERS;
      if ((mock_channel_perms(m, g, m->voices[vi].channel_id) & need) == 0u) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      char det[48];
      if (req->op == CCORD_ADMIN_SET_VOICE_MUTE) {
        (void)snprintf(det, sizeof det, "mute: %s -> %s",
                       m->members[mi].mute ? "true" : "false", req->voice_muted ? "true" : "false");
        m->members[mi].mute = req->voice_muted;
        m->voices[vi].mute = req->voice_muted;
      } else {
        (void)snprintf(det, sizeof det, "deaf: %s -> %s",
                       m->members[mi].deaf ? "true" : "false", req->voice_deafened ? "true" : "false");
        m->members[mi].deaf = req->voice_deafened;
        m->voices[vi].deaf = req->voice_deafened;
      }
      *out = mock_audit_add(m, g, 24, req->user_id, mock_user_name(m, req->user_id), req->reason, det);
      if (*out == CCORD_OK) *out = admin_emit_member(m, (size_t)mi, err);
      if (*out == CCORD_OK) *out = queue_voice_state(m, &m->voices[vi], now_of(m) + m->cfg.latency_ms, err);
      return true;
    }
    case CCORD_ADMIN_MOVE_VOICE_MEMBER: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      long mi = admin_member_index(m, g, req->user_id);
      if (mi < 0) {
        *out = admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_member, err);
        return true;
      }
      long vi = mock_voice_index(m, g, req->user_id);
      if (vi < 0) {
        *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Target user is not connected to voice.", err);
        return true;
      }
      if ((mock_channel_perms(m, g, m->voices[vi].channel_id) & CCORD_PERM_MOVE_MEMBERS) == 0u) {
        *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      uint64_t due = now_of(m) + m->cfg.latency_ms;
      if (req->target_channel_id != CCORD_ID_NONE) {
        size_t tci = admin_channel_index(m, req->target_channel_id);
        bool target_voice = tci != SIZE_MAX && m->channels[tci].guild_id == g &&
                            (m->channels[tci].kind == CCORD_CHANNEL_VOICE ||
                             m->channels[tci].kind == CCORD_CHANNEL_STAGE);
        if (!target_voice) {
          *out = admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid voice channel", err);
          return true;
        }
        if ((mock_channel_perms(m, g, req->target_channel_id) & CCORD_PERM_CONNECT) == 0u) {
          *out = admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
          return true;
        }
        ccord_id from_ch = m->voices[vi].channel_id;
        size_t from_ci = admin_channel_index(m, from_ch);
        bool was_stage = from_ci != SIZE_MAX && m->channels[from_ci].kind == CCORD_CHANNEL_STAGE;
        bool to_stage = m->channels[tci].kind == CCORD_CHANNEL_STAGE;
        if (to_stage) {
          ccord_status ss = mock_stage_ensure(m, g, req->target_channel_id, due, err);
          if (ss != CCORD_OK) { *out = ss; return true; }
        }
        m->voices[vi].channel_id = req->target_channel_id;
        // Stage audience flags belong to the stage the member is in: entering starts suppressed,
        // leaving (or moving between stages) clears the pending ask. A same-channel move keeps them.
        if (from_ch != req->target_channel_id) {
          m->voices[vi].suppress = to_stage;
          m->voices[vi].request_to_speak_timestamp = 0;
        }
        if (m->has_stream && m->stream.guild_id == g && m->stream.user_id == req->user_id) {
          m->voices[vi].self_stream = false;
          ccord_status ss = queue_stream_signal_ex(m, g, m->stream.channel_id, m->stream.user_id,
                                                   CCORD_STREAM_SIG_DELETE, 0, false, "broadcaster moved", err);
          m->has_stream = false;
          if (ss != CCORD_OK) { *out = ss; return true; }
        }
        *out = mock_audit_add(m, g, 26, req->user_id, mock_user_name(m, req->user_id), req->reason,
                              m->channels[tci].name);
        if (*out == CCORD_OK) *out = admin_emit_member(m, (size_t)mi, err);
        if (*out == CCORD_OK) *out = queue_voice_state(m, &m->voices[vi], due, err);
        if (*out == CCORD_OK && was_stage) *out = mock_stage_cleanup(m, from_ch, due, err);
        return true;
      }
      ccord_voice_state v = m->voices[vi];
      ccord_id old_ch = v.channel_id;
      size_t old_ci = admin_channel_index(m, old_ch);
      bool was_stage = old_ci != SIZE_MAX && m->channels[old_ci].kind == CCORD_CHANNEL_STAGE;
      v.channel_id = CCORD_ID_NONE;
      v.self_video = false;
      v.self_stream = false;
      v.suppress = false;
      v.request_to_speak_timestamp = 0;
      memmove(&m->voices[vi], &m->voices[vi + 1u], (m->nvoices - (size_t)vi - 1u) * sizeof *m->voices);
      m->nvoices--;
      m->snap.voice_state_count = m->nvoices;
      if (m->has_stream && m->stream.guild_id == g && m->stream.user_id == req->user_id) {
        *out = queue_stream_signal_ex(m, g, m->stream.channel_id, m->stream.user_id,
                                      CCORD_STREAM_SIG_DELETE, 0, false, "broadcaster disconnected", err);
        m->has_stream = false;
        if (*out != CCORD_OK) return true;
      }
      if (was_stage) {
        *out = mock_stage_cleanup(m, old_ch, due, err);
        if (*out != CCORD_OK) return true;
      }
      *out = mock_audit_add(m, g, 27, req->user_id, mock_user_name(m, req->user_id), req->reason,
                            "disconnected from voice");
      if (*out == CCORD_OK) *out = admin_emit_member(m, (size_t)mi, err);
      if (*out == CCORD_OK) *out = queue_voice_state(m, &v, due, err);
      return true;
    }
    case CCORD_ADMIN_VOICE_STATUS:
      return admin_m5_voice_status(m, req, out, err);
    case CCORD_ADMIN_STAGE_REQUEST_SPEAK:
      return admin_m5_stage_request_speak(m, req, out, err);
    case CCORD_ADMIN_STAGE_SET_SPEAKER:
      return admin_m5_stage_set_speaker(m, req, out, err);
    case CCORD_ADMIN_STAGE_TOPIC:
      return admin_m5_stage_topic(m, req, out, err);
    case CCORD_ADMIN_STAGE_CREATE:
      return admin_m5_stage_create(m, req, out, err);
    case CCORD_ADMIN_STAGE_DELETE:
      return admin_m5_stage_delete(m, req, out, err);
    case CCORD_ADMIN_EVENT_LIST:
    case CCORD_ADMIN_EVENT_CREATE:
    case CCORD_ADMIN_EVENT_MODIFY:
    case CCORD_ADMIN_EVENT_DELETE:
    case CCORD_ADMIN_EVENT_RSVP_ADD:
    case CCORD_ADMIN_EVENT_RSVP_REMOVE:
      return admin_scheduled_events(m, req, out, err);
    case CCORD_ADMIN_USER_SETTINGS_PATCH:   // M6 account settings, devices & connections
    case CCORD_ADMIN_AUTH_SESSIONS_LIST:
    case CCORD_ADMIN_AUTH_SESSIONS_LOGOUT:
    case CCORD_ADMIN_CONNECTIONS_LIST:
    case CCORD_ADMIN_CONNECTION_EDIT:
    case CCORD_ADMIN_CONNECTION_DELETE:
      return admin_m6_account(m, req, out, err);
    case CCORD_ADMIN_NONE:
    case CCORD_ADMIN_CREATE_CHANNEL:
    case CCORD_ADMIN_EDIT_CHANNEL:
    case CCORD_ADMIN_DELETE_CHANNEL:
    case CCORD_ADMIN_CREATE_THREAD:
    case CCORD_ADMIN_CREATE_THREAD_MSG:
    case CCORD_ADMIN_ARCHIVE_THREAD:
    case CCORD_ADMIN_JOIN_THREAD:
    case CCORD_ADMIN_LEAVE_THREAD:
    case CCORD_ADMIN_CLOSE_DM:
    case CCORD_ADMIN_SET_NICK:
    case CCORD_ADMIN_TIMEOUT:
    case CCORD_ADMIN_KICK:
    case CCORD_ADMIN_BAN:
    case CCORD_ADMIN_SET_NOTIFY:
    case CCORD_ADMIN_SET_GUILD_FOLDERS:
    case CCORD_ADMIN_CREATE_GUILD:
    case CCORD_ADMIN_PREVIEW_INVITE:
    case CCORD_ADMIN_ACCEPT_INVITE:
    case CCORD_ADMIN_CREATE_GROUP_DM:
    case CCORD_ADMIN_ADD_DM_RECIPIENT:
    case CCORD_ADMIN_REMOVE_DM_RECIPIENT:
    case CCORD_ADMIN_EDIT_GROUP_DM:
    case CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST:
    case CCORD_ADMIN_REJECT_MESSAGE_REQUEST:
    case CCORD_ADMIN_SET_USER_NOTE:
    case CCORD_ADMIN_SET_CUSTOM_STATUS:
    case CCORD_ADMIN_OP_COUNT:
      return false;
    default: return false;  // M5 ops are served by m5-session-mock
  }
  return false;
}

static const ccord_discover_guild k_mock_discoverable[] = {
  {
    .guild_id = 9001,
    .name = "Linux Gaming Hub",
    .description = "Proton, Vulkan, SDL3, and native Linux gaming discussions.",
    .icon_url = "",
    .vanity_code = "linux-gaming",
    .primary_category_id = 1,
    .online_count = 1420,
    .member_count = 8900,
  },
  {
    .guild_id = 9002,
    .name = "Synthwave & Chiptune",
    .description = "Retro trackers, FM synthesis, and nightly listening parties.",
    .icon_url = "",
    .vanity_code = "synthwave",
    .primary_category_id = 2,
    .online_count = 610,
    .member_count = 4300,
  },
  {
    .guild_id = 9003,
    .name = "Systems Programming Study",
    .description = "C23, OS kernels, compilers, and low-level computer science.",
    .icon_url = "",
    .vanity_code = "c23-study",
    .primary_category_id = 3,
    .online_count = 890,
    .member_count = 6100,
  },
  {
    .guild_id = 9004,
    .name = "Vulkan & Graphics Lab",
    .description = "Real-time rendering, shaders, GPU profiling, and Wayland.",
    .icon_url = "",
    .vanity_code = "vulkan-lab",
    .primary_category_id = 4,
    .online_count = 530,
    .member_count = 3200,
  },
  {
    .guild_id = 9005,
    .name = "Pixel Cinema Club",
    .description = "Indie animation, demoscene showcases, and weekend watchalongs.",
    .icon_url = "",
    .vanity_code = "pixel-cinema",
    .primary_category_id = 5,
    .online_count = 340,
    .member_count = 2100,
  },
};
#define MOCK_DISCOVERABLE_COUNT (sizeof k_mock_discoverable / sizeof k_mock_discoverable[0])

static bool mock_str_eq_ci(const char *a, const char *b)
{
  if (!a || !b) return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return false;
  }
  return *a == '\0' && *b == '\0';
}

// Membership screening: joining this invite materializes a guild whose self member is pending until the canned rules
// form below is accepted (GET /guilds/{g}/member-verification, PUT /guilds/{g}/requests/@me).
#define MOCK_MV_INVITE_CODE "mv-test"
#define MOCK_MV_GUILD_ID 9101u
#define MOCK_MV_GUILD_NAME "Rules Lounge"

static ccord_status mock_materialize_guild(mock *m, ccord_id gid_hint, const char *gname, bool self_owner,
                                           bool self_pending, ccord_admin_op admin_op, ccord_id *out_guild_id,
                                           ccord_id *out_channel_id, ccord_error *err)
{
  if (m->nguilds >= MOCK_GUILDS_MAX || m->nroles + 2u > MOCK_ROLES_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: server limit reached");
  }
  if (m->next_admin_id == 0) m->next_admin_id = 8000;
  ccord_id gid = gid_hint != CCORD_ID_NONE ? gid_hint : m->next_admin_id++;
  while (gid_hint == CCORD_ID_NONE && mock_guild_known(m, gid)) gid = m->next_admin_id++;
  const char *owned_name = admin_owned(m, gname && gname[0] ? gname : "New Server");
  if (!owned_name) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");

  ccord_guild g = {
    .id = gid,
    .name = owned_name,
    .icon_url = "",
    .owner_id = self_owner ? m->self_id : 0,
    .self_permissions = self_owner ? UINT64_MAX : MOCK_EVERYONE_PERMS,
    .afk_timeout = 300u,
    .has_guild_settings = true,
  };
  m->guilds[m->nguilds++] = g;
  m->snap.guild_count = m->nguilds;

  ccord_role r_ev = {.id = gid, .guild_id = gid, .name = "@everyone", .permissions = MOCK_EVERYONE_PERMS};
  m->roles[m->nroles++] = r_ev;
  ccord_id admin_role_id = CCORD_ID_NONE;
  if (self_owner) {
    admin_role_id = m->next_role_id++;
    m->roles[m->nroles++] = (ccord_role){
      .id = admin_role_id,
      .guild_id = gid,
      .name = "Admins",
      .color = 0xF0B232u,
      .position = 1,
      .permissions = CCORD_PERM_ADMINISTRATOR,
      .hoist = true,
    };
  }
  for (size_t i = 0; i < sizeof m->mrows / sizeof m->mrows[0]; i++) {
    if (!m->mrows[i].present) {
      m->mrows[i] = (mock_mrow){.guild_id = gid, .user_id = m->self_id, .present = true};
      if (admin_role_id != CCORD_ID_NONE) {
        m->mrows[i].ids[0] = admin_role_id;
        m->mrows[i].count = 1;
      }
      break;
    }
  }
  mock_sync_member_roles(m);

  ccord_member *nm = ccord_realloc_array(m->members, m->nmembers + 1u, sizeof *nm);
  if (!nm) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  m->members = nm;
  ccord_member self_m = {
    .guild_id = gid,
    .user_id = m->self_id,
    .role_name = self_owner ? "Admins" : "Members",
    .role_color = self_owner ? 0xF0B232u : 0u,
    .role_rank = self_owner ? 0u : 2u,
    .pending = self_pending,   // membership screening: the guild holds this member until the rules form is accepted
  };
  m->members[m->nmembers++] = self_m;
  m->snap.members = m->members;
  m->snap.member_count = m->nmembers;

  ccord_channel cat = {
    .id = m->next_admin_id++,
    .guild_id = gid,
    .parent_id = 0,
    .kind = CCORD_CHANNEL_CATEGORY,
    .name = "TEXT CHANNELS",
    .topic = "",
    .position = 0,
    .discord_type = 4u,
  };
  size_t cci = admin_add_channel(m, &cat, err);
  if (cci == SIZE_MAX) return CCORD_ERR_NOMEM;
  ccord_channel gen = {
    .id = m->next_admin_id++,
    .guild_id = gid,
    .parent_id = cat.id,
    .kind = CCORD_CHANNEL_TEXT,
    .name = "general",
    .topic = "Welcome!",
    .position = 1,
    .discord_type = 0u,
  };
  size_t gci = admin_add_channel(m, &gen, err);
  if (gci == SIZE_MAX) return CCORD_ERR_NOMEM;

  if (out_guild_id) *out_guild_id = gid;
  if (out_channel_id) *out_channel_id = gen.id;

  mock_item it = make_item(CCORD_NET_GUILD_UPSERT, now_of(m) + m->cfg.latency_ms);
  it.guild = ccord_alloc_array(1, sizeof *it.guild);
  it.chan = ccord_alloc_array(2, sizeof *it.chan);
  it.member = ccord_alloc_array(1, sizeof *it.member);
  size_t rcnt = 0, rfirst = mock_guild_roles(m, gid, &rcnt);
  it.roles_own = ccord_alloc_array(rcnt ? rcnt : 1u, sizeof *it.roles_own);
  if (!it.guild || !it.chan || !it.member || !it.roles_own) {
    item_release(&it);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  *it.guild = g;
  it.chan[0] = m->channels[cci];
  it.chan[1] = m->channels[gci];
  *it.member = self_m;
  if (rcnt) memcpy(it.roles_own, &m->roles[rfirst], rcnt * sizeof *it.roles_own);

  it.ev.guild = it.guild;
  it.ev.guild_id = gid;
  it.ev.admin_op = admin_op;
  it.ev.guild_channels = it.chan;
  it.ev.guild_channel_count = 2u;
  it.ev.guild_members = it.member;
  it.ev.guild_member_count = 1u;
  it.ev.users = m->users;
  it.ev.user_count = MOCK_NUM_USERS;
  it.ev.roles = it.roles_own;
  it.ev.role_count = rcnt;
  for (size_t i = 0; i < m->nmember_roles; i++) {
    if (m->member_roles[i].guild_id == gid && m->member_roles[i].user_id == m->self_id) {
      it.ev.member_roles = &m->member_roles[i];
      it.ev.member_role_count = 1u;
      break;
    }
  }
  return push_item(m, it, err);
}

static ccord_status admin_set_guild_folders(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  // Update mock's own guild metadata/order to match what the user persisted.
  ccord_guild reordered[MOCK_GUILDS_MAX];
  bool placed[MOCK_GUILDS_MAX] = {false};
  size_t out_n = 0;
  for (size_t i = 0; i < req->folder_item_count; i++) {
    const ccord_guild_folder_item *fi = &req->folder_items[i];
    for (size_t gi = 0; gi < m->nguilds; gi++) {
      if (!placed[gi] && m->guilds[gi].id == fi->guild_id) {
        ccord_guild g = m->guilds[gi];
        g.folder_id = fi->folder_id;
        memset(g.folder_name, 0, sizeof g.folder_name);
        if (fi->folder_id != 0) {
          (void)snprintf(g.folder_name, sizeof g.folder_name, "%s", fi->folder_name);
          g.folder_color = fi->folder_color;
        } else {
          g.folder_color = 0;
        }
        reordered[out_n++] = g;
        placed[gi] = true;
        break;
      }
    }
  }
  for (size_t gi = 0; gi < m->nguilds; gi++) {
    if (!placed[gi]) {
      ccord_guild g = m->guilds[gi];
      g.folder_id = 0;
      memset(g.folder_name, 0, sizeof g.folder_name);
      g.folder_color = 0;
      reordered[out_n++] = g;
    }
  }
  memcpy(m->guilds, reordered, out_n * sizeof m->guilds[0]);

  mock_item it = make_item(CCORD_NET_GUILD_LAYOUT, now_of(m) + m->cfg.latency_ms);
  size_t n = req->folder_item_count;
  if (n > 0) {
    it.folder_items_own = ccord_alloc_array(n, sizeof *it.folder_items_own);
    if (!it.folder_items_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    memcpy(it.folder_items_own, req->folder_items, n * sizeof *it.folder_items_own);
    it.ev.folder_items = it.folder_items_own;
    it.ev.folder_item_count = n;
  }
  return push_item(m, it, err);
}

static ccord_status admin_handle_invite(mock *m, const ccord_admin_request *req, bool accept, ccord_error *err)
{
  ccord_invite_preview prev = {0};
  bool found = false;

  for (size_t i = 0; i < m->ninvites; i++) {
    if (mock_str_eq_ci(m->invites[i].inv.code, req->code)) {
      ccord_id gid = m->invites[i].guild_id;
      const char *gname = "Server";
      const char *gicon = "";
      for (size_t gi = 0; gi < m->nguilds; gi++) {
        if (m->guilds[gi].id == gid) {
          gname = m->guilds[gi].name ? m->guilds[gi].name : "Server";
          gicon = m->guilds[gi].icon_url ? m->guilds[gi].icon_url : "";
          break;
        }
      }
      (void)snprintf(prev.code, sizeof prev.code, "%s", m->invites[i].inv.code);
      prev.guild_id = gid;
      (void)snprintf(prev.guild_name, sizeof prev.guild_name, "%s", gname);
      (void)snprintf(prev.guild_icon_url, sizeof prev.guild_icon_url, "%s", gicon);
      prev.channel_id = m->invites[i].inv.channel_id;
      (void)snprintf(prev.channel_name, sizeof prev.channel_name, "%s",
                     m->invites[i].inv.channel_name ? m->invites[i].inv.channel_name : "general");
      prev.online_count = 8u;
      prev.member_count = MOCK_NUM_USERS;
      found = true;
      break;
    }
  }

  for (size_t gi = 0; !found && gi < m->nguilds; gi++) {
    if (m->guilds[gi].vanity_url_code[0] != '\0' && mock_str_eq_ci(m->guilds[gi].vanity_url_code, req->code)) {
      (void)snprintf(prev.code, sizeof prev.code, "%s", m->guilds[gi].vanity_url_code);
      prev.guild_id = m->guilds[gi].id;
      (void)snprintf(prev.guild_name, sizeof prev.guild_name, "%s",
                     m->guilds[gi].name ? m->guilds[gi].name : "Server");
      (void)snprintf(prev.guild_icon_url, sizeof prev.guild_icon_url, "%s",
                     m->guilds[gi].icon_url ? m->guilds[gi].icon_url : "");
      prev.channel_id = CCORD_ID_NONE;
      (void)snprintf(prev.channel_name, sizeof prev.channel_name, "general");
      for (size_t ci = 0; ci < m->nchannels; ci++) {
        if (m->channels[ci].guild_id == m->guilds[gi].id && m->channels[ci].kind == CCORD_CHANNEL_TEXT) {
          prev.channel_id = m->channels[ci].id;
          if (m->channels[ci].name) (void)snprintf(prev.channel_name, sizeof prev.channel_name, "%s", m->channels[ci].name);
          break;
        }
      }
      prev.online_count = 8u;
      prev.member_count = MOCK_NUM_USERS;
      found = true;
    }
  }

  for (size_t i = 0; !found && i < MOCK_DISCOVERABLE_COUNT; i++) {
    const ccord_discover_guild *d = &k_mock_discoverable[i];
    if (mock_str_eq_ci(d->vanity_code, req->code)) {
      (void)snprintf(prev.code, sizeof prev.code, "%s", d->vanity_code);
      prev.guild_id = d->guild_id;
      (void)snprintf(prev.guild_name, sizeof prev.guild_name, "%s", d->name);
      (void)snprintf(prev.guild_icon_url, sizeof prev.guild_icon_url, "%s", d->icon_url);
      (void)snprintf(prev.guild_description, sizeof prev.guild_description, "%s", d->description);
      prev.channel_id = CCORD_ID_NONE;
      for (size_t ci = 0; ci < m->nchannels; ci++) {
        if (m->channels[ci].guild_id == d->guild_id && m->channels[ci].kind == CCORD_CHANNEL_TEXT) {
          prev.channel_id = m->channels[ci].id;
          break;
        }
      }
      (void)snprintf(prev.channel_name, sizeof prev.channel_name, "general");
      prev.online_count = d->online_count;
      prev.member_count = d->member_count;
      found = true;
    }
  }

  if (!found && mock_str_eq_ci(req->code, MOCK_MV_INVITE_CODE)) {
    (void)snprintf(prev.code, sizeof prev.code, "%s", MOCK_MV_INVITE_CODE);
    prev.guild_id = MOCK_MV_GUILD_ID;
    (void)snprintf(prev.guild_name, sizeof prev.guild_name, "%s", MOCK_MV_GUILD_NAME);
    (void)snprintf(prev.guild_description, sizeof prev.guild_description, "A server with membership screening.");
    prev.channel_id = CCORD_ID_NONE;
    (void)snprintf(prev.channel_name, sizeof prev.channel_name, "general");
    prev.online_count = 4u;
    prev.member_count = 12u;
    found = true;
  }

  if (!found) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Invite", err);

  if (accept && !mock_guild_known(m, prev.guild_id)) {
    ccord_id mat_gid = CCORD_ID_NONE, mat_cid = CCORD_ID_NONE;
    ccord_status mst = mock_materialize_guild(m, prev.guild_id, prev.guild_name, false,
                                              prev.guild_id == MOCK_MV_GUILD_ID, CCORD_ADMIN_NONE,
                                              &mat_gid, &mat_cid, err);
    if (mst != CCORD_OK) return mst;
    prev.channel_id = mat_cid;
  }

  mock_item it = make_item(accept ? CCORD_NET_INVITE_ACCEPTED : CCORD_NET_INVITE_PREVIEW,
                           now_of(m) + m->cfg.latency_ms);
  it.invite_preview_own = ccord_alloc_array(1, sizeof *it.invite_preview_own);
  if (!it.invite_preview_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.invite_preview_own = prev;
  it.ev.guild_id = prev.guild_id;
  it.ev.channel_id = prev.channel_id;
  it.ev.invite_preview = it.invite_preview_own;
  return push_item(m, it, err);
}

// GET /guilds/{g}/member-verification: only the screening guild has a form (Discord answers 404 for the rest).
static ccord_status admin_member_verification_get(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  if (!mock_guild_known(m, req->guild_id) || req->guild_id != MOCK_MV_GUILD_ID) {
    return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild Member Verification Form", err);
  }
  mock_item it = make_item(CCORD_NET_MEMBER_VERIFICATION, now_of(m) + m->cfg.latency_ms);
  it.mv_own = ccord_alloc_array(1, sizeof *it.mv_own);
  if (!it.mv_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.mv_own = (ccord_member_verification){
    .version = "2026-11-14T00:00:00.000000+00:00",
    .description = "Welcome to " MOCK_MV_GUILD_NAME "! Read the rules and accept them to start chatting.",
    .field_count = 1u,
  };
  it.mv_own->fields[0] = (ccord_member_verification_field){
    .field_type = CCORD_MEMBER_VERIFICATION_TERMS,
    .label = "Server rules",
    .values = {"Be kind and respectful to everyone.", "No spam, scams or self-promotion.",
               "Keep each conversation in the right channel."},
    .value_count = 3u,
    .required = true,
  };
  it.ev.guild_id = req->guild_id;
  it.ev.member_verification = it.mv_own;
  return push_item(m, it, err);
}

// PUT /guilds/{g}/requests/@me: the self member stops being pending once the required TERMS field is accepted
// ("response":true in the submitted form), and the gateway-style member update says so. A form that does not accept
// the rules is refused like Discord's 400 and leaves the member pending.
static ccord_status admin_member_verification_submit(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  long mi = mock_guild_known(m, req->guild_id) ? admin_member_index(m, req->guild_id, m->self_id) : -1;
  if (mi < 0) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
  if (req->guild_id != MOCK_MV_GUILD_ID) {
    return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild Member Verification Form", err);
  }
  if (!strstr(req->body, "\"response\":true")) {
    return admin_reply_failed(m, req, CCORD_ERR_INVALID, "Required form fields were not answered", err);
  }
  if (!m->members[mi].pending) return CCORD_OK;   // already through screening: nothing to confirm
  m->members[mi].pending = false;
  return admin_emit_member(m, (size_t)mi, err);
}

static ccord_status mock_request_discoverable_guilds(ccord_transport *t, const char *query, uint32_t category_id,
                                                     uint32_t offset, uint32_t limit, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_DISCOVERY]++;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  const char *q = query ? query : "";
  size_t qlen = strlen(q);
  if (qlen > 100u || !ccord_utf8_valid_text(q, qlen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid discovery query");
  }
  if (limit == 0 || limit > CCORD_DISCOVERY_PAGE_MAX) limit = CCORD_DISCOVERY_PAGE_MAX;

  mock_item it = make_item(CCORD_NET_DISCOVERY_RESULTS, now_of(m) + m->cfg.latency_ms);
  it.discovery_own = ccord_alloc_array(limit, sizeof *it.discovery_own);
  it.discovery_query_own = ccord_strndup(q, qlen);
  if (!it.discovery_own || !it.discovery_query_own) {
    item_release(&it);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }

  size_t matched = 0, out_n = 0;
  for (size_t i = 0; i < MOCK_DISCOVERABLE_COUNT; i++) {
    const ccord_discover_guild *d = &k_mock_discoverable[i];
    if (category_id != 0 && d->primary_category_id != category_id) continue;
    if (qlen > 0 && !has_ci_substr(d->name, q, qlen) && !has_ci_substr(d->description, q, qlen)) continue;
    if (matched >= offset && out_n < limit) {
      it.discovery_own[out_n++] = *d;
    }
    matched++;
  }

  it.ev.discovery_guilds = it.discovery_own;
  it.ev.discovery_count = out_n;
  it.ev.discovery_total = (uint32_t)matched;
  it.ev.discovery_offset = offset;
  it.ev.discovery_category_id = category_id;
  it.ev.discovery_query = it.discovery_query_own;
  return push_item(m, it, err);
}

// Notification settings are the user's own (no permission involved): the mock keeps the row, then answers with the
// server's whole set, as the gateway's USER_GUILD_SETTINGS_UPDATE does.
static ccord_status admin_set_notify(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  ccord_id g = req->guild_id;
  bool known = g == CCORD_DM_GUILD_ID;
  for (size_t i = 0; !known && i < m->nguilds; i++) known = m->guilds[i].id == g;
  if (!known) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
  if (req->channel_id != CCORD_ID_NONE && admin_channel_index(m, req->channel_id) == SIZE_MAX) {
    return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
  }
  size_t at = SIZE_MAX, last_of_guild = SIZE_MAX;
  for (size_t i = 0; i < m->nnotify; i++) {
    if (m->notify[i].guild_id != g) continue;
    last_of_guild = i;
    if (m->notify[i].channel_id == req->channel_id) at = i;
  }
  if (at == SIZE_MAX) {
    if (m->nnotify == MOCK_NOTIFY_MAX) return admin_reply_failed(m, req, CCORD_ERR_RANGE, "Too many overrides", err);
    at = last_of_guild == SIZE_MAX ? m->nnotify : last_of_guild + 1u;   // rows stay grouped by server
    memmove(&m->notify[at + 1], &m->notify[at], (m->nnotify - at) * sizeof m->notify[0]);
    m->nnotify++;
    m->notify[at] = (ccord_notify_scope){.guild_id = g, .channel_id = req->channel_id, .level = CCORD_NOTIFY_INHERIT};
  }
  ccord_notify_scope *r = &m->notify[at];
  if (req->notify_muted >= 0) {
    r->muted = req->notify_muted == 1;
    r->mute_until_ms = r->muted ? req->until_ms : 0;
  }
  if (req->notify_level >= 0) r->level = (ccord_notify_level)req->notify_level;
  if (req->notify_suppress_everyone >= 0) r->suppress_everyone = req->notify_suppress_everyone == 1;
  if (req->notify_suppress_roles >= 0) r->suppress_roles = req->notify_suppress_roles == 1;
  mock_item it = make_item(CCORD_NET_NOTIFY_SETTINGS, now_of(m) + m->cfg.latency_ms);
  size_t n = 0;
  for (size_t i = 0; i < m->nnotify; i++) n += m->notify[i].guild_id == g;
  it.notify_own = ccord_alloc_array(n ? n : 1u, sizeof *it.notify_own);
  if (!it.notify_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t k = 0;
  for (size_t i = 0; i < m->nnotify; i++) {
    if (m->notify[i].guild_id == g) it.notify_own[k++] = m->notify[i];
  }
  it.ev.guild_id = g;
  it.ev.notify = it.notify_own;
  it.ev.notify_count = k;
  return push_item(m, it, err);
}

static void mock_default_group_dm_name(const mock *m, const ccord_id *recipient_ids, size_t recipient_count,
                                       char *out, size_t cap)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  size_t pos = 0;
  for (size_t i = 0; i < recipient_count; i++) {
    const char *nm = mock_user_name(m, recipient_ids[i]);
    if (!nm || nm[0] == '\0') continue;
    if (pos > 0 && pos + 2u < cap) {
      out[pos++] = ',';
      out[pos++] = ' ';
      out[pos] = '\0';
    }
    size_t nlen = strlen(nm);
    for (size_t k = 0; k < nlen && pos + 1u < cap && pos < CCORD_NAME_MAX_BYTES;) {
      unsigned char c = (unsigned char)nm[k];
      size_t seq = c < 0x80u ? 1u : (c >= 0xF0u ? 4u : (c >= 0xE0u ? 3u : (c >= 0xC0u ? 2u : 1u)));
      if (pos + seq >= cap || pos + seq > CCORD_NAME_MAX_BYTES || k + seq > nlen) break;
      memcpy(out + pos, nm + k, seq);
      pos += seq;
      k += seq;
    }
    out[pos] = '\0';
  }
  if (out[0] == '\0') (void)snprintf(out, cap, "Group DM");
}

// ---- embedded activities (GET /activities/shelf, POST /activities/{c}/{app}, POST .../instances/{i}/leave) -------

// The shelf every mock guild offers, in shelf order (rank 1 first). Application ids sit in their own range so they
// never collide with the mock's user, channel, role or message ids.
#define MOCK_ACTIVITY_APP_BASE 97000u
static const ccord_activity_app k_mock_activity_apps[] = {
  {.application_id = MOCK_ACTIVITY_APP_BASE + 1u, .name = "Sketch Heads",
   .description = "Draw and guess together: one sketches, everyone else races to name it.",
   .icon_hash = "mock-sketch-icon", .cover_hash = "mock-sketch-cover", .max_participants = 8u, .shelf_rank = 1u},
  {.application_id = MOCK_ACTIVITY_APP_BASE + 2u, .name = "Poker Night",
   .description = "Texas Hold'em at a shared table, with chips on the house.",
   .icon_hash = "mock-poker-icon", .cover_hash = "mock-poker-cover", .max_participants = 7u, .shelf_rank = 2u},
  {.application_id = MOCK_ACTIVITY_APP_BASE + 3u, .name = "Watch Together",
   .description = "Queue up videos and watch them in sync with the whole channel.",
   .icon_hash = "mock-watch-icon", .cover_hash = "", .max_participants = 25u, .shelf_rank = 3u},
};
#define MOCK_ACTIVITY_APP_COUNT (sizeof k_mock_activity_apps / sizeof k_mock_activity_apps[0])

static const ccord_activity_app *mock_activity_app(ccord_id application_id)
{
  for (size_t i = 0; i < MOCK_ACTIVITY_APP_COUNT; i++) {
    if (k_mock_activity_apps[i].application_id == application_id) return &k_mock_activity_apps[i];
  }
  return nullptr;
}

static long mock_activity_by_instance(const mock *m, ccord_id instance_id)
{
  for (size_t i = 0; i < m->nactivities; i++) {
    if (m->activities[i].instance_id == instance_id) return (long)i;
  }
  return -1;
}

static long mock_activity_by_app(const mock *m, ccord_id channel_id, ccord_id application_id)
{
  for (size_t i = 0; i < m->nactivities; i++) {
    if (m->activities[i].channel_id == channel_id && m->activities[i].application_id == application_id) {
      return (long)i;
    }
  }
  return -1;
}

static long mock_activity_participant(const ccord_activity_instance *inst, ccord_id user_id)
{
  for (size_t i = 0; i < inst->participant_count; i++) {
    if (inst->participants[i] == user_id) return (long)i;
  }
  return -1;
}

// Drops row `i` from the registry and queues the end-of-instance update (participant_count 0).
static ccord_status mock_activity_end(mock *m, size_t i, uint64_t due, ccord_error *err)
{
  ccord_activity_instance ended = m->activities[i];
  ended.participant_count = 0;
  memset(ended.participants, 0, sizeof ended.participants);
  memmove(&m->activities[i], &m->activities[i + 1u], (m->nactivities - i - 1u) * sizeof m->activities[0]);
  m->nactivities--;
  return queue_activity_instance(m, &ended, due, err);
}

static ccord_status admin_activity(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  uint64_t due = now_of(m) + m->cfg.latency_ms;
  switch (req->op) {
    case CCORD_ADMIN_ACTIVITY_SHELF: {
      if (mock_guild_index(m, req->guild_id) == SIZE_MAX) {
        return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
      }
      mock_item it = make_item(CCORD_NET_ACTIVITY_SHELF, due);
      it.activity_apps_own = ccord_alloc_array(MOCK_ACTIVITY_APP_COUNT, sizeof *it.activity_apps_own);
      if (!it.activity_apps_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      memcpy(it.activity_apps_own, k_mock_activity_apps, sizeof k_mock_activity_apps);
      it.ev.guild_id = req->guild_id;
      it.ev.activity_apps = it.activity_apps_own;
      it.ev.activity_app_count = MOCK_ACTIVITY_APP_COUNT;
      return push_item(m, it, err);
    }
    case CCORD_ADMIN_ACTIVITY_LAUNCH: {
      size_t ci = admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
      const ccord_channel *ch = &m->channels[ci];
      if (ch->kind != CCORD_CHANNEL_VOICE) {
        return admin_reply_failed(m, req, CCORD_ERR_INVALID, "Activities run in voice channels.", err);
      }
      long vi = mock_voice_index(m, ch->guild_id, m->self_id);
      if (vi < 0 || m->voices[vi].channel_id != ch->id) {
        return admin_reply_failed(m, req, CCORD_ERR_INVALID, "Join the voice channel before starting an activity.", err);
      }
      if ((mock_channel_perms(m, ch->guild_id, ch->id) & CCORD_PERM_USE_EMBEDDED_ACTIVITIES) == 0u) {
        return admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
      }
      const ccord_activity_app *app = mock_activity_app(req->application_id);
      if (!app) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Application", err);
      // Launching an app that already runs in the channel joins that instance (Discord answers the same way).
      long ai = mock_activity_by_app(m, ch->id, app->application_id);
      if (ai < 0) {
        if (m->nactivities >= MOCK_ACTIVITIES_MAX) {
          return admin_reply_failed(m, req, CCORD_ERR_RANGE, "Too many running activities.", err);
        }
        if (m->next_admin_id == 0) m->next_admin_id = 8000;
        ai = (long)m->nactivities++;
        m->activities[ai] = (ccord_activity_instance){.instance_id = m->next_admin_id++,
                                                      .application_id = app->application_id,
                                                      .guild_id = ch->guild_id,
                                                      .channel_id = ch->id,
                                                      .location_id = ch->id};
      }
      ccord_activity_instance *inst = &m->activities[ai];
      if (mock_activity_participant(inst, m->self_id) < 0) {
        uint32_t cap = app->max_participants ? app->max_participants : CCORD_ACTIVITY_MAX_PARTICIPANTS;
        if (inst->participant_count >= cap || inst->participant_count >= CCORD_ACTIVITY_MAX_PARTICIPANTS) {
          return admin_reply_failed(m, req, CCORD_ERR_RANGE, "This activity is full.", err);
        }
        inst->participants[inst->participant_count++] = m->self_id;
      }
      return queue_activity_instance(m, inst, due, err);
    }
    case CCORD_ADMIN_ACTIVITY_LEAVE: {
      long ai = mock_activity_by_instance(m, req->instance_id);
      if (ai < 0 || m->activities[ai].application_id != req->application_id) {
        return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Activity Instance", err);
      }
      ccord_activity_instance *inst = &m->activities[ai];
      long pi = mock_activity_participant(inst, m->self_id);
      if (pi >= 0) {
        memmove(&inst->participants[pi], &inst->participants[pi + 1], (inst->participant_count - (size_t)pi - 1u) *
                                                                          sizeof inst->participants[0]);
        inst->participant_count--;
        inst->participants[inst->participant_count] = CCORD_ID_NONE;
      }
      if (inst->participant_count == 0) return mock_activity_end(m, (size_t)ai, due, err);   // the last one out ends it
      return queue_activity_instance(m, inst, due, err);
    }
    default:
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not an activity request");
  }
}

ccord_status ccord_mock_push_activity_instance(ccord_transport *t, ccord_id channel_id, ccord_id application_id,
                                               const ccord_id *participants, size_t count, ccord_id *out_instance_id,
                                               ccord_error *err)
{
  ccord_error_clear(err);
  if (!t || (!participants && count > 0) || count > CCORD_ACTIVITY_MAX_PARTICIPANTS) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad activity arguments");
  }
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  size_t ci = admin_channel_index(m, channel_id);
  if (ci == SIZE_MAX || m->channels[ci].kind != CCORD_CHANNEL_VOICE) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown voice channel");
  }
  if (!mock_activity_app(application_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown activity app");
  for (size_t i = 0; i < count; i++) {
    if (participants[i] == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad participant");
  }
  uint64_t due = now_of(m) + m->cfg.latency_ms;
  long ai = mock_activity_by_app(m, channel_id, application_id);
  if (ai >= 0 && count == 0) {   // an empty list ends the running instance
    if (out_instance_id) *out_instance_id = m->activities[ai].instance_id;
    return mock_activity_end(m, (size_t)ai, due, err);
  }
  if (count == 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: no such activity instance");
  if (ai < 0) {
    if (m->nactivities >= MOCK_ACTIVITIES_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many activities");
    if (m->next_admin_id == 0) m->next_admin_id = 8000;
    ai = (long)m->nactivities++;
    m->activities[ai] = (ccord_activity_instance){.instance_id = m->next_admin_id++,
                                                  .application_id = application_id,
                                                  .guild_id = m->channels[ci].guild_id,
                                                  .channel_id = channel_id,
                                                  .location_id = channel_id};
  }
  ccord_activity_instance *inst = &m->activities[ai];
  memset(inst->participants, 0, sizeof inst->participants);
  memcpy(inst->participants, participants, count * sizeof participants[0]);
  inst->participant_count = count;
  if (out_instance_id) *out_instance_id = inst->instance_id;
  return queue_activity_instance(m, inst, due, err);
}

static ccord_status admin_guild_profile_get(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  const ccord_guild *g = nullptr;
  for (size_t i = 0; i < m->nguilds; i++) {
    if (m->guilds[i].id == req->guild_id) {
      g = &m->guilds[i];
      break;
    }
  }
  ccord_guild_profile gp = {.guild_id = req->guild_id};
  if (g) {
    (void)snprintf(gp.name, sizeof gp.name, "%s", g->name ? g->name : "Mock Server");
    (void)snprintf(gp.description, sizeof gp.description, "Community server for %s", gp.name);
    if (g->icon_url) (void)snprintf(gp.icon_url, sizeof gp.icon_url, "%s", g->icon_url);
    (void)snprintf(gp.tag, sizeof gp.tag, "%s", (g->id == m->guilds[0].id) ? "CCRD" : "MOCK");
    gp.brand_color = 0x5865F2u;
    gp.has_brand_color = true;
    gp.online_count = 42;
    gp.member_count = 128;
  } else {
    gp.unavailable = true;
  }
  mock_item it = make_item(CCORD_NET_GUILD_PROFILE, now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = req->guild_id;
  it.ev.guild_profile = gp;
  return push_item(m, it, err);
}

static ccord_status mock_admin_action(ccord_transport *t, const ccord_admin_request *req, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!req || req->op == CCORD_ADMIN_NONE || req->op >= CCORD_ADMIN_OP_COUNT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad admin request");
  }
  if (!m->last_admin) m->last_admin = ccord_alloc_array(1, sizeof *m->last_admin);
  if (m->last_admin) *m->last_admin = *req;
  m->admin_count++;
  if (m->fail_next_admin) {   // test hook: the request reached the server and Discord answered with an error
    m->fail_next_admin = false;
    return admin_reply_failed(m, req, CCORD_ERR_INTERNAL, "mock: injected admin failure", err);
  }
  if (m->next_admin_id == 0) m->next_admin_id = 8000;
  if (req->op == CCORD_ADMIN_SET_NOTIFY) return admin_set_notify(m, req, err);   // not a permissioned write
  if (req->op == CCORD_ADMIN_PREVIEW_INVITE) return admin_handle_invite(m, req, false, err);
  if (req->op == CCORD_ADMIN_GET_MEMBER_VERIFICATION) return admin_member_verification_get(m, req, err);
  if (req->op == CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION) return admin_member_verification_submit(m, req, err);   // self-service
  if (req->op == CCORD_ADMIN_EVENT_LIST) {
    ccord_status served_ro = CCORD_OK;
    (void)admin_scheduled_events(m, req, &served_ro, err);
    return served_ro;
  }
  if (req->op == CCORD_ADMIN_ACTIVITY_SHELF) return admin_activity(m, req, err);   // a read: refuse_writes spares it
  if (req->op == CCORD_ADMIN_GET_GUILD_PROFILE) return admin_guild_profile_get(m, req, err);
  if (m->cfg.refuse_writes) {
    return admin_reply_failed(m, req, CCORD_ERR_PERMISSION, "You don't have permission to do that. (Missing Permissions)",
                              err);
  }
  if (req->op == CCORD_ADMIN_ACTIVITY_LAUNCH || req->op == CCORD_ADMIN_ACTIVITY_LEAVE) {
    return admin_activity(m, req, err);
  }
  if (req->op == CCORD_ADMIN_SET_GUILD_FOLDERS) return admin_set_guild_folders(m, req, err);
  if (req->op == CCORD_ADMIN_CREATE_GUILD) {
    return mock_materialize_guild(m, CCORD_ID_NONE, req->name, true, false, CCORD_ADMIN_CREATE_GUILD, nullptr, nullptr,
                                  err);
  }
  if (req->op == CCORD_ADMIN_ACCEPT_INVITE) return admin_handle_invite(m, req, true, err);
  static const char gone[] = "It no longer exists.";
  ccord_status served = CCORD_OK;
  if (mock_admin_server(m, req, &served, err)) return served;
  switch (req->op) {
    case CCORD_ADMIN_CREATE_CHANNEL:
    case CCORD_ADMIN_CREATE_THREAD:
    case CCORD_ADMIN_CREATE_THREAD_MSG: {
      bool from_msg = req->op == CCORD_ADMIN_CREATE_THREAD_MSG;
      bool thread = req->op == CCORD_ADMIN_CREATE_THREAD || from_msg;
      size_t parent_ci = SIZE_MAX;
      long starter_mi = -1;
      if (from_msg) {
        parent_ci = admin_channel_index(m, req->channel_id);
        if (parent_ci == SIZE_MAX) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
        starter_mi = find_message(m, parent_ci, req->message_id);
        if (starter_mi < 0) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      const char *name = admin_owned(m, req->name);
      if (!name) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      size_t in_guild = 0;
      for (size_t i = 0; i < m->nchannels; i++) in_guild += m->channels[i].guild_id == req->guild_id;
      ccord_channel ch = {
        .id = m->next_admin_id++,
        .guild_id = req->guild_id,
        .parent_id = thread ? req->channel_id : req->parent_id,
        .kind = req->channel_type == 13 ? CCORD_CHANNEL_STAGE
                : req->channel_type == 2 ? CCORD_CHANNEL_VOICE
                : req->channel_type == 4                          ? CCORD_CHANNEL_CATEGORY
                                                                  : CCORD_CHANNEL_TEXT,
        .name = name,
        .topic = "",
        .position = (int32_t)in_guild,
        .discord_type = thread ? 11u : req->channel_type,
        .thread_has_member = thread,
        .thread_joined = thread,
        .thread_auto_archive_mins = thread ? 1440u : 0u,
      };
      size_t ci = admin_add_channel(m, &ch, err);
      if (ci == SIZE_MAX) return CCORD_ERR_NOMEM;
      if (from_msg && parent_ci != SIZE_MAX && starter_mi >= 0) {
        ccord_message *smsg = &m->chmsgs[parent_ci][starter_mi];
        ccord_message_rich nr = {0};
        if (smsg->rich) nr = *smsg->rich;
        nr.present |= CCORD_RICH_ALL | CCORD_RICH_HAS_THREAD;
        nr.thread = (ccord_msg_thread){.id = ch.id, .name = name, .message_count = 0};
        ccord_message_rich *cloned = ccord_message_rich_clone(&nr, err);
        if (cloned) {
          retire_rich(m, smsg->rich);
          smsg->rich = cloned;
          (void)queue_message_update(m, *smsg, now_of(m) + m->cfg.latency_ms, err);
        }
      }
      ccord_status as = mock_audit_add(m, req->guild_id, thread ? 110 : 10, ch.id, name, req->reason, "");
      if (as != CCORD_OK) return as;
      return admin_emit_channel(m, ci, err);
    }
    case CCORD_ADMIN_ARCHIVE_THREAD: {
      size_t ci = admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || !ccord_discord_type_is_thread(m->channels[ci].discord_type)) {
        return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      m->channels[ci].thread_archived = req->thread_archived;
      ccord_status as = mock_audit_add(m, m->channels[ci].guild_id, 111, req->channel_id, m->channels[ci].name,
                                       req->reason, req->thread_archived ? "archived: true" : "archived: false");
      if (as != CCORD_OK) return as;
      return admin_emit_channel(m, ci, err);
    }
    case CCORD_ADMIN_JOIN_THREAD:
    case CCORD_ADMIN_LEAVE_THREAD: {
      size_t ci = admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || !ccord_discord_type_is_thread(m->channels[ci].discord_type)) {
        return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      bool joined = req->op == CCORD_ADMIN_JOIN_THREAD;
      m->channels[ci].thread_has_member = true;
      m->channels[ci].thread_joined = joined;
      mock_item it = make_item(CCORD_NET_THREAD_MEMBER_SELF, now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = m->channels[ci].guild_id;
      it.ev.channel_id = req->channel_id;
      it.ev.user_id = m->self_id;
      it.ev.thread_joined = joined;
      return push_item(m, it, err);
    }
    case CCORD_ADMIN_EDIT_CHANNEL: {
      size_t ci = admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      if (m->channels[ci].guild_id != CCORD_DM_GUILD_ID &&
          !ccord_discord_type_is_thread(m->channels[ci].discord_type) &&
          (mock_channel_perms(m, m->channels[ci].guild_id, req->channel_id) & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
        return admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
      }
      if (req->name[0] != '\0') {
        const char *name = admin_owned(m, req->name);
        if (!name) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        m->channels[ci].name = name;
      }
      if (req->has_topic) {
        const char *topic = admin_owned(m, req->topic);
        if (!topic) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        m->channels[ci].topic = topic;
      }
      if (req->has_slowmode) {
        m->channels[ci].rate_limit_per_user = req->slowmode_s;
      }
      if (req->has_nsfw) {
        m->channels[ci].nsfw = req->nsfw;
      }
      if (req->has_bitrate) {
        m->channels[ci].bitrate = req->bitrate;
      }
      if (req->has_user_limit) {
        m->channels[ci].user_limit = req->user_limit;
      }
      if (req->has_auto_archive) {
        m->channels[ci].default_auto_archive_duration = req->default_auto_archive_duration;
      }
      if (req->has_available_tags && ccord_discord_type_is_forum(m->channels[ci].discord_type)) {
        size_t fmi = mock_ensure_forum_meta(m, req->channel_id);
        if (fmi != SIZE_MAX) {
          m->forum_metas[fmi].tag_count = req->available_tag_count;
          for (uint8_t k = 0u; k < req->available_tag_count; k++) {
            ccord_forum_tag ftag = req->available_tags[k];
            if (ftag.id == CCORD_ID_NONE) ftag.id = 4100u + (m->next_admin_id++);
            m->forum_metas[fmi].tags[k] = ftag;
          }
        }
      }
      ccord_status as = mock_audit_add(m, m->channels[ci].guild_id, 11, req->channel_id, m->channels[ci].name,
                                       req->reason, req->has_topic ? "topic changed" : "channel updated");
      if (as != CCORD_OK) return as;
      return admin_emit_channel(m, ci, err);
    }
    case CCORD_ADMIN_DELETE_CHANNEL:
    case CCORD_ADMIN_CLOSE_DM:
    case CCORD_ADMIN_REJECT_MESSAGE_REQUEST: {
      size_t ci = admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      ccord_id guild = m->channels[ci].guild_id;
      if (req->op == CCORD_ADMIN_DELETE_CHANNEL) {
        if (guild != CCORD_DM_GUILD_ID && !ccord_discord_type_is_thread(m->channels[ci].discord_type) &&
            (mock_channel_perms(m, guild, req->channel_id) & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
          return admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        }
        ccord_status as = mock_audit_add(m, guild, 12, req->channel_id, m->channels[ci].name, req->reason, "");
        if (as != CCORD_OK) return as;
      }
      admin_remove_channel(m, ci);
      mock_item it = make_item(CCORD_NET_CHANNEL_DELETE, now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = guild;
      it.ev.channel_id = req->channel_id;
      return push_item(m, it, err);
    }
    case CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST: {
      size_t ci = admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].guild_id != CCORD_DM_GUILD_ID) {
        return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      m->channels[ci].is_message_request = false;
      m->channels[ci].is_spam = false;
      mock_item it = make_item(CCORD_NET_MESSAGE_REQUEST_ACCEPTED, now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = CCORD_DM_GUILD_ID;
      it.ev.channel_id = req->channel_id;
      return push_item(m, it, err);
    }
    case CCORD_ADMIN_CREATE_GROUP_DM: {
      if (req->dm_recipient_count == 0 || req->dm_recipient_count > CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX) {
        return admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid recipients", err);
      }
      for (size_t i = 0; i < req->dm_recipient_count; i++) {
        ccord_id rid = req->dm_recipient_ids[i];
        if (rid == CCORD_ID_NONE || rid == m->self_id) {
          return admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid recipient", err);
        }
        bool known = false;
        for (size_t ui = 0; ui < MOCK_NUM_USERS; ui++) {
          if (m->users[ui].id == rid) {
            known = true;
            break;
          }
        }
        if (!known) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown User", err);
      }
      if (req->dm_recipient_count == 1u && req->name[0] == '\0') {
        ccord_id rid = req->dm_recipient_ids[0];
        ccord_id dm_cid = 900000u + rid;
        size_t ci = admin_channel_index(m, dm_cid);
        if (ci == SIZE_MAX) {
          const char *uname = "Direct Message";
          const char *uavatar = "";
          for (size_t ui = 0; ui < MOCK_NUM_USERS; ui++) {
            if (m->users[ui].id == rid) {
              uname = m->users[ui].name;
              uavatar = m->users[ui].avatar_url ? m->users[ui].avatar_url : "";
              break;
            }
          }
          ccord_channel dmc = {
            .id = dm_cid,
            .guild_id = CCORD_DM_GUILD_ID,
            .parent_id = CCORD_ID_NONE,
            .kind = CCORD_CHANNEL_TEXT,
            .discord_type = CCORD_DISCORD_CHANNEL_DM,
            .name = uname,
            .topic = "Direct Message",
            .user_id = rid,
            .recipient_count = 1u,
            .recipient_ids = {rid},
            .icon_url = uavatar,
          };
          ci = admin_add_channel(m, &dmc, err);
          if (ci == SIZE_MAX) return CCORD_ERR_NOMEM;
        }
        return admin_emit_channel_op(m, ci, CCORD_ADMIN_CREATE_GROUP_DM, err);
      }
      char def_name[CCORD_NAME_MAX_BYTES + 1];
      const char *raw_name = req->name;
      if (raw_name[0] == '\0') {
        mock_default_group_dm_name(m, req->dm_recipient_ids, req->dm_recipient_count, def_name, sizeof def_name);
        raw_name = def_name;
      }
      const char *owned_name = admin_owned(m, raw_name);
      if (!owned_name) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      ccord_channel gdm = {
        .id = m->next_admin_id++,
        .guild_id = CCORD_DM_GUILD_ID,
        .parent_id = CCORD_ID_NONE,
        .kind = CCORD_CHANNEL_TEXT,
        .discord_type = CCORD_DISCORD_CHANNEL_GROUP_DM,
        .name = owned_name,
        .topic = "",
        .user_id = req->dm_recipient_ids[0],
        .owner_id = m->self_id,
        .recipient_count = (uint16_t)req->dm_recipient_count,
        .icon_url = "",
      };
      for (size_t i = 0; i < req->dm_recipient_count; i++) {
        gdm.recipient_ids[i] = req->dm_recipient_ids[i];
      }
      size_t ci = admin_add_channel(m, &gdm, err);
      if (ci == SIZE_MAX) return CCORD_ERR_NOMEM;
      return admin_emit_channel_op(m, ci, CCORD_ADMIN_CREATE_GROUP_DM, err);
    }
    case CCORD_ADMIN_ADD_DM_RECIPIENT: {
      size_t ci = admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].discord_type != CCORD_DISCORD_CHANNEL_GROUP_DM) {
        return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      if (req->user_id == CCORD_ID_NONE || req->user_id == m->self_id) {
        return admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid recipient", err);
      }
      const ccord_user *tu = nullptr;
      for (size_t ui = 0; ui < MOCK_NUM_USERS; ui++) {
        if (m->users[ui].id == req->user_id) {
          tu = &m->users[ui];
          break;
        }
      }
      if (!tu) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown User", err);
      ccord_channel *ch = &m->channels[ci];
      bool already = false;
      for (uint16_t i = 0; i < ch->recipient_count; i++) {
        if (ch->recipient_ids[i] == req->user_id) {
          already = true;
          break;
        }
      }
      if (!already) {
        if (ch->recipient_count >= CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX) {
          return admin_reply_failed(m, req, CCORD_ERR_RANGE, "Group DM has reached maximum recipients", err);
        }
        ch->recipient_ids[ch->recipient_count++] = req->user_id;
        if (ch->user_id == CCORD_ID_NONE) ch->user_id = req->user_id;
      }
      mock_item it = make_item(CCORD_NET_DM_RECIPIENT_CHANGE, now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = CCORD_DM_GUILD_ID;
      it.ev.channel_id = req->channel_id;
      it.ev.user_id = req->user_id;
      it.ev.recipient_added = true;
      it.ev.users = tu;
      it.ev.user_count = 1u;
      return push_item(m, it, err);
    }
    case CCORD_ADMIN_REMOVE_DM_RECIPIENT: {
      size_t ci = admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].discord_type != CCORD_DISCORD_CHANNEL_GROUP_DM) {
        return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      ccord_channel *ch = &m->channels[ci];
      if (req->user_id == m->self_id) {
        admin_remove_channel(m, ci);
        mock_item it = make_item(CCORD_NET_CHANNEL_DELETE, now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = CCORD_DM_GUILD_ID;
        it.ev.channel_id = req->channel_id;
        return push_item(m, it, err);
      }
      if (ch->owner_id != m->self_id) {
        return admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
      }
      size_t at = SIZE_MAX;
      for (uint16_t i = 0; i < ch->recipient_count; i++) {
        if (ch->recipient_ids[i] == req->user_id) {
          at = i;
          break;
        }
      }
      if (at == SIZE_MAX) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Recipient", err);
      memmove(&ch->recipient_ids[at], &ch->recipient_ids[at + 1u],
              (ch->recipient_count - at - 1u) * sizeof ch->recipient_ids[0]);
      ch->recipient_count--;
      ch->recipient_ids[ch->recipient_count] = CCORD_ID_NONE;
      ch->user_id = ch->recipient_count > 0 ? ch->recipient_ids[0] : CCORD_ID_NONE;
      mock_item it = make_item(CCORD_NET_DM_RECIPIENT_CHANGE, now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = CCORD_DM_GUILD_ID;
      it.ev.channel_id = req->channel_id;
      it.ev.user_id = req->user_id;
      it.ev.recipient_added = false;
      return push_item(m, it, err);
    }
    case CCORD_ADMIN_EDIT_GROUP_DM: {
      size_t ci = admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].discord_type != CCORD_DISCORD_CHANNEL_GROUP_DM) {
        return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      ccord_channel *ch = &m->channels[ci];
      if (req->has_group_dm_name) {
        char def_name[CCORD_NAME_MAX_BYTES + 1];
        const char *raw_name = req->name;
        if (raw_name[0] == '\0') {
          mock_default_group_dm_name(m, ch->recipient_ids, ch->recipient_count, def_name, sizeof def_name);
          raw_name = def_name;
        }
        const char *owned_name = admin_owned(m, raw_name);
        if (!owned_name) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        ch->name = owned_name;
      }
      if (req->has_group_dm_icon) {
        if (req->clear_group_dm_icon) {
          ch->icon_url = "";
        } else {
          char url[96];
          (void)snprintf(url, sizeof url, "https://cdn.discordapp.com/channel-icons/%llu/gdm_%zu.png",
                         (unsigned long long)ch->id, m->admin_count);
          const char *owned_url = admin_owned(m, url);
          if (!owned_url) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          ch->icon_url = owned_url;
        }
      }
      return admin_emit_channel_op(m, ci, CCORD_ADMIN_EDIT_GROUP_DM, err);
    }
    case CCORD_ADMIN_SET_USER_NOTE: {
      if (req->user_id == CCORD_ID_NONE) {
        return admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid user", err);
      }
      const char *owned_note = admin_owned(m, req->note);
      if (!owned_note) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      if (req->note[0] == '\0') {
        for (size_t i = 0; i < m->nnotes; i++) {
          if (m->notes[i].user_id == req->user_id) {
            memmove(&m->notes[i], &m->notes[i + 1u], (m->nnotes - i - 1u) * sizeof m->notes[0]);
            m->nnotes--;
            m->snap.note_count = m->nnotes;
            break;
          }
        }
      } else {
        bool updated = false;
        for (size_t i = 0; i < m->nnotes; i++) {
          if (m->notes[i].user_id == req->user_id) {
            m->notes[i].note = owned_note;
            updated = true;
            break;
          }
        }
        if (!updated && m->nnotes < MOCK_NUM_USERS) {
          m->notes[m->nnotes++] = (ccord_user_note){.user_id = req->user_id, .note = owned_note};
          m->snap.note_count = m->nnotes;
        }
      }
      mock_item it = make_item(CCORD_NET_USER_NOTE_UPDATE, now_of(m) + m->cfg.latency_ms);
      it.ev.user_id = req->user_id;
      it.ev.text = owned_note;
      return push_item(m, it, err);
    }
    case CCORD_ADMIN_SET_CUSTOM_STATUS: {
      if (req->custom_status_clear) {
        m->users[0].custom_status_emoji[0] = '\0';
        m->users[0].custom_status_text[0] = '\0';
        m->users[0].custom_status_expires_ms = 0;
      } else {
        if (req->custom_status_emoji_id != CCORD_ID_NONE && req->custom_status_emoji[0] != '\0') {   // stored as name:id
          (void)snprintf(m->users[0].custom_status_emoji, sizeof m->users[0].custom_status_emoji, "%.40s:%llu",
                         req->custom_status_emoji, (unsigned long long)req->custom_status_emoji_id);
        } else {
          (void)snprintf(m->users[0].custom_status_emoji, sizeof m->users[0].custom_status_emoji, "%s",
                         req->custom_status_emoji);
        }
        (void)snprintf(m->users[0].custom_status_text, sizeof m->users[0].custom_status_text, "%s",
                       req->custom_status_text);
        m->users[0].custom_status_expires_ms = req->until_ms;
      }
      mock_item it = make_item(CCORD_NET_CUSTOM_STATUS_UPDATE, now_of(m) + m->cfg.latency_ms);
      it.ev.user_id = m->self_id;
      it.ev.custom_status_clear = req->custom_status_clear;
      if (!req->custom_status_clear) {
        (void)snprintf(it.ev.custom_status_emoji, sizeof it.ev.custom_status_emoji, "%s", m->users[0].custom_status_emoji);
        (void)snprintf(it.ev.custom_status_text, sizeof it.ev.custom_status_text, "%s", req->custom_status_text);
        it.ev.custom_status_expires_ms = req->until_ms;
      }
      return push_item(m, it, err);
    }
    case CCORD_ADMIN_SET_NICK:
    case CCORD_ADMIN_TIMEOUT: {
      long mi = admin_member_index(m, req->guild_id, req->user_id);
      if (mi < 0) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      if (req->op == CCORD_ADMIN_SET_NICK) {
        memset(m->members[mi].nick, 0, sizeof m->members[mi].nick);
        (void)snprintf(m->members[mi].nick, sizeof m->members[mi].nick, "%s", req->name);
      } else {
        m->members[mi].timeout_until_ms = req->until_ms;
      }
      ccord_status as = mock_audit_add(m, req->guild_id, 24, req->user_id, mock_user_name(m, req->user_id), req->reason,
                                       req->op == CCORD_ADMIN_SET_NICK ? "nickname changed"
                                       : req->until_ms                 ? "timed out"
                                                                       : "timeout removed");
      if (as != CCORD_OK) return as;
      return admin_emit_member(m, (size_t)mi, err);
    }
    case CCORD_ADMIN_KICK:
    case CCORD_ADMIN_BAN: {
      long mi = admin_member_index(m, req->guild_id, req->user_id);
      if (mi < 0) return admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      const char *who = admin_owned(m, mock_user_name(m, req->user_id));
      mock_mrow *row = mock_mrow_find(m, req->guild_id, req->user_id);
      if (row) row->present = false;
      mock_sync_member_roles(m);
      memmove(&m->members[mi], &m->members[mi + 1], (m->nmembers - (size_t)mi - 1) * sizeof *m->members);
      m->nmembers--;
      m->snap.member_count = m->nmembers;
      if (req->op == CCORD_ADMIN_BAN && m->nbans < MOCK_BANS_MAX && who) {
        const char *why = admin_owned(m, req->reason);
        if (why) m->bans[m->nbans++] = (mock_ban){req->guild_id, {.user_id = req->user_id, .name = who, .reason = why}};
      }
      ccord_status as = mock_audit_add(m, req->guild_id, req->op == CCORD_ADMIN_BAN ? 22 : 20, req->user_id, who,
                                       req->reason, "");
      if (as != CCORD_OK) return as;
      mock_item it = make_item(CCORD_NET_MEMBER_REMOVE, now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = req->guild_id;
      it.ev.user_id = req->user_id;
      return push_item(m, it, err);
    }
    case CCORD_ADMIN_NONE:
    case CCORD_ADMIN_CREATE_ROLE:
    case CCORD_ADMIN_EDIT_ROLE:
    case CCORD_ADMIN_DELETE_ROLE:
    case CCORD_ADMIN_REORDER_ROLES:
    case CCORD_ADMIN_ADD_MEMBER_ROLE:
    case CCORD_ADMIN_REMOVE_MEMBER_ROLE:
    case CCORD_ADMIN_UNBAN:
    case CCORD_ADMIN_CREATE_INVITE:
    case CCORD_ADMIN_REVOKE_INVITE:
    case CCORD_ADMIN_LIST_BANS:
    case CCORD_ADMIN_LIST_INVITES:
    case CCORD_ADMIN_LIST_AUDIT_LOG:   // served by mock_admin_server above
    case CCORD_ADMIN_REORDER_CHANNELS: // served by mock_admin_server above
    case CCORD_ADMIN_EDIT_GUILD:       // served by mock_admin_server above
    case CCORD_ADMIN_SET_INCIDENT_ACTIONS:
    case CCORD_ADMIN_SET_VANITY_URL:
    case CCORD_ADMIN_LEAVE_GUILD:
    case CCORD_ADMIN_DELETE_GUILD:
    case CCORD_ADMIN_TRANSFER_OWNERSHIP:
    case CCORD_ADMIN_LIST_EMOJIS:
    case CCORD_ADMIN_CREATE_EMOJI:
    case CCORD_ADMIN_EDIT_EMOJI:
    case CCORD_ADMIN_DELETE_EMOJI:
    case CCORD_ADMIN_LIST_STICKERS:
    case CCORD_ADMIN_CREATE_STICKER:
    case CCORD_ADMIN_EDIT_STICKER:
    case CCORD_ADMIN_DELETE_STICKER:
    case CCORD_ADMIN_LIST_SOUNDBOARD:
    case CCORD_ADMIN_CREATE_SOUNDBOARD:
    case CCORD_ADMIN_EDIT_SOUNDBOARD:
    case CCORD_ADMIN_DELETE_SOUNDBOARD:
    case CCORD_ADMIN_PREVIEW_PRUNE:
    case CCORD_ADMIN_EXECUTE_PRUNE:
    case CCORD_ADMIN_LIST_AUTOMOD_RULES:
    case CCORD_ADMIN_CREATE_AUTOMOD_RULE:
    case CCORD_ADMIN_EDIT_AUTOMOD_RULE:
    case CCORD_ADMIN_DELETE_AUTOMOD_RULE:
    case CCORD_ADMIN_LIST_WEBHOOKS:
    case CCORD_ADMIN_CREATE_WEBHOOK:
    case CCORD_ADMIN_EDIT_WEBHOOK:
    case CCORD_ADMIN_DELETE_WEBHOOK:
    case CCORD_ADMIN_FOLLOW_CHANNEL:
    case CCORD_ADMIN_LIST_APP_CMD_PERMS:
    case CCORD_ADMIN_GET_WELCOME_SCREEN:
    case CCORD_ADMIN_EDIT_WELCOME_SCREEN:
    case CCORD_ADMIN_GET_ONBOARDING:
    case CCORD_ADMIN_PUT_ONBOARDING:
    case CCORD_ADMIN_PUT_CHANNEL_OVERWRITE:
    case CCORD_ADMIN_DELETE_CHANNEL_OVERWRITE:
    case CCORD_ADMIN_LIST_CHANNEL_INVITES:
    case CCORD_ADMIN_CLONE_CHANNEL:
    case CCORD_ADMIN_SET_VOICE_MUTE:
    case CCORD_ADMIN_SET_VOICE_DEAF:
    case CCORD_ADMIN_MOVE_VOICE_MEMBER:
    case CCORD_ADMIN_SET_NOTIFY:       // served before the permission check
    case CCORD_ADMIN_SET_GUILD_FOLDERS:
    case CCORD_ADMIN_CREATE_GUILD:
    case CCORD_ADMIN_PREVIEW_INVITE:
    case CCORD_ADMIN_ACCEPT_INVITE:
    case CCORD_ADMIN_GET_MEMBER_VERIFICATION:      // served before the permission check
    case CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION:
    case CCORD_ADMIN_GET_GUILD_PROFILE:
    case CCORD_ADMIN_OP_COUNT: break;
    default: break;  // M5 ops are served by m5-session-mock
  }
  return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad admin request");
}

size_t ccord_mock_admin_count(const ccord_transport *t) { return t ? ((const mock *)t)->admin_count : 0; }

size_t ccord_mock_call_count(const ccord_transport *t, ccord_mock_call which)
{
  return t && which < CCORD_MOCK_CALL_COUNT ? ((const mock *)t)->calls[which] : 0;
}

uint32_t ccord_mock_last_member_chunks(const ccord_transport *t)
{
  return t ? ((const mock *)t)->last_member_chunks : 0;
}

const ccord_admin_request *ccord_mock_last_admin(const ccord_transport *t)
{
  return t ? ((const mock *)t)->last_admin : nullptr;
}

const char *ccord_mock_rich_activities(const ccord_transport *t, size_t *len)
{
  const mock *m = (const mock *)t;
  if (len) *len = m ? m->rich_len : 0;
  return m ? m->rich_json : nullptr;
}

static void clear_last_interaction_values(mock *m)
{
  for (size_t i = 0; i < m->last_interaction_nvalues; i++) {
    ccord_free(m->last_interaction_values[i]);
    m->last_interaction_values[i] = nullptr;
  }
  m->last_interaction_nvalues = 0;
}

ccord_status ccord_mock_push_message_request(ccord_transport *t, ccord_id sender_id,
                                             const char *initial_message, bool is_spam,
                                             ccord_id *out_channel_id, ccord_error *err)
{
  if (out_channel_id) *out_channel_id = CCORD_ID_NONE;
  ccord_error_clear(err);
  if (!t) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (sender_id == CCORD_ID_NONE || sender_id == m->self_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid message request sender");
  }
  const ccord_user *sender = nullptr;
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == sender_id) {
      sender = &m->users[i];
      break;
    }
  }
  if (!sender) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown sender");

  ccord_id dm_cid = 900000u + sender_id;
  size_t ci = admin_channel_index(m, dm_cid);
  if (ci == SIZE_MAX) {
    ccord_channel dmc = {
      .id = dm_cid,
      .guild_id = CCORD_DM_GUILD_ID,
      .parent_id = CCORD_ID_NONE,
      .kind = CCORD_CHANNEL_TEXT,
      .discord_type = CCORD_DISCORD_CHANNEL_DM,
      .name = sender->name,
      .topic = "Direct Message",
      .user_id = sender_id,
      .recipient_count = 1u,
      .recipient_ids = {sender_id},
      .is_message_request = true,
      .is_spam = is_spam,
      .icon_url = sender->avatar_url ? sender->avatar_url : "",
    };
    ci = admin_add_channel(m, &dmc, err);
    if (ci == SIZE_MAX) return CCORD_ERR_NOMEM;
  } else {
    m->channels[ci].is_message_request = true;
    m->channels[ci].is_spam = is_spam;
  }
  if (out_channel_id) *out_channel_id = dm_cid;

  ccord_status st = admin_emit_channel_op(m, ci, CCORD_ADMIN_NONE, err);
  if (st != CCORD_OK) return st;

  if (initial_message && initial_message[0] != '\0') {
    size_t len = strlen(initial_message);
    if (len > CCORD_SEND_MAX_BYTES || !ccord_utf8_valid_text(initial_message, len)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid initial message");
    }
    ccord_message msg = store_new(m, ci, sender_id, initial_message, len, err, &st);
    if (st != CCORD_OK) return st;
    m->channels[ci].last_message_id = msg.id;
    st = queue_message_create(m, msg, now_of(m) + m->cfg.latency_ms, err);
    if (st != CCORD_OK) return st;
  }
  return CCORD_OK;
}

ccord_status ccord_mock_post_rich_message(ccord_transport *t, ccord_id channel_id, ccord_id author_id,
                                          const char *content, const ccord_message_rich *rich,
                                          ccord_id *out_id, ccord_error *err)
{
  if (out_id) *out_id = CCORD_ID_NONE;
  ccord_error_clear(err);
  if (!t) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  const char *txt = content ? content : "";
  size_t len = strlen(txt);
  if (len > CCORD_SEND_MAX_BYTES || !ccord_utf8_valid_text(txt, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid rich message text");
  }
  if (rich) {
    st = ccord_message_rich_validate(rich, err);
    if (st != CCORD_OK) return st;
  }
  ccord_id author = author_id != CCORD_ID_NONE ? author_id : m->users[10].id;
  ccord_message msg = store_new_rich(m, ci, author, txt, len, rich, err, &st);
  if (st != CCORD_OK) return st;
  if (out_id) *out_id = msg.id;
  return queue_message_create(m, msg, now_of(m) + m->cfg.latency_ms, err);
}

static ccord_status mock_vote_poll(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                   const uint32_t *answer_ids, size_t answer_count, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (answer_count > CCORD_RICH_MAX_POLL_ANSWERS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many poll answers");
  }
  if (answer_count > 0 && !answer_ids) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null poll answers");
  }
  ccord_status st;
  size_t ci = channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  ccord_message *msg = &m->chmsgs[ci][mi];
  if (!msg->rich || !msg->rich->poll.present) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message has no poll");
  }
  const ccord_poll *oldp = &msg->rich->poll;
  if (oldp->finalized || (oldp->expiry_ms > 0 && message_time(m) >= oldp->expiry_ms)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: poll is closed");
  }
  if (!oldp->multiselect && answer_count > 1) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: poll allows one answer");
  }
  bool want[CCORD_RICH_MAX_POLL_ANSWERS] = {0};
  for (size_t i = 0; i < answer_count; i++) {
    bool found = false;
    for (size_t k = 0; k < oldp->answer_count; k++) {
      if (oldp->answers[k].id == answer_ids[i]) {
        if (want[k]) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: duplicate poll answer");
        want[k] = true;
        found = true;
        break;
      }
    }
    if (!found) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: unknown poll answer");
  }
  if (m->cfg.refuse_writes) {
    return queue_action_refused(m, CCORD_ACTION_POLL_VOTE, channel_id, message_id, "", 0, nullptr, 0, err);
  }

  ccord_message_rich *nr = ccord_message_rich_clone(msg->rich, err);
  if (!nr) return CCORD_ERR_NOMEM;
  uint64_t due = now_of(m) + m->cfg.latency_ms;
  ccord_id gid = m->channels[ci].guild_id;

  // Emit removals first, then additions, matching Discord's poll vote gateway sequence.
  for (size_t k = 0; k < nr->poll.answer_count; k++) {
    ccord_poll_answer *pa = &nr->poll.answers[k];
    if (pa->me_voted && !want[k]) {
      pa->me_voted = false;
      if (pa->votes > 0) pa->votes--;
      mock_item it = make_item(CCORD_NET_POLL_VOTE, due);
      it.ev.guild_id = gid;
      it.ev.channel_id = channel_id;
      it.ev.message_id = message_id;
      it.ev.user_id = m->self_id;
      it.ev.poll_answer_id = pa->id;
      it.ev.poll_vote_added = false;
      st = push_item(m, it, err);
      if (st != CCORD_OK) {
        ccord_message_rich_free(nr);
        return st;
      }
    }
  }
  for (size_t k = 0; k < nr->poll.answer_count; k++) {
    ccord_poll_answer *pa = &nr->poll.answers[k];
    if (!pa->me_voted && want[k]) {
      pa->me_voted = true;
      pa->votes++;
      mock_item it = make_item(CCORD_NET_POLL_VOTE, due);
      it.ev.guild_id = gid;
      it.ev.channel_id = channel_id;
      it.ev.message_id = message_id;
      it.ev.user_id = m->self_id;
      it.ev.poll_answer_id = pa->id;
      it.ev.poll_vote_added = true;
      st = push_item(m, it, err);
      if (st != CCORD_OK) {
        ccord_message_rich_free(nr);
        return st;
      }
    }
  }
  retire_rich(m, msg->rich);
  msg->rich = nr;
  return CCORD_OK;
}

static ccord_status mock_send_interaction(ccord_transport *t, const ccord_interaction_request *req, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!req || req->application_id == CCORD_ID_NONE || req->channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid interaction");
  }
  ccord_status st;
  size_t ci = channel_index(m, req->channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;

  const char *ident = nullptr;
  char ident_buf[CCORD_RICH_CUSTOM_ID_MAX_BYTES + 1];
  bool has_group = false;
  bool has_sub = false;
  const char *grp_name = "";
  const char *sub_name = "";
  long target_mi = -1;
  if (req->type == CCORD_INTERACTION_MESSAGE_COMPONENT) {
    if (!req->custom_id || req->custom_id[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid custom_id");
    }
    size_t idlen = strlen(req->custom_id);
    if (idlen > CCORD_RICH_CUSTOM_ID_MAX_BYTES || !ccord_utf8_valid_text(req->custom_id, idlen)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid custom_id");
    }
    if (req->message_id == CCORD_ID_NONE || find_message(m, ci, req->message_id) < 0) {
      return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
    }
    if (req->component_type < CCORD_COMP_BUTTON || req->component_type > CCORD_COMP_CHANNEL_SELECT ||
        req->component_type == CCORD_COMP_TEXT_INPUT) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid component_type");
    }
    if (req->value_count > CCORD_RICH_MAX_SELECT_OPTIONS || (req->value_count > 0 && !req->values)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid select values");
    }
    ident = req->custom_id;
  } else if (req->type == CCORD_INTERACTION_MODAL_SUBMIT) {
    if (!req->custom_id || req->custom_id[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid custom_id");
    }
    size_t idlen = strlen(req->custom_id);
    if (idlen > CCORD_RICH_CUSTOM_ID_MAX_BYTES || !ccord_utf8_valid_text(req->custom_id, idlen)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid custom_id");
    }
    if (req->field_count == 0 || req->field_count > CCORD_RICH_MAX_COMPONENT_ROWS ||
        !req->field_ids || !req->field_values) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid modal fields");
    }
    ident = req->custom_id;
  } else if (req->type == CCORD_INTERACTION_APPLICATION_COMMAND ||
             req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE) {
    if (req->command_id == CCORD_ID_NONE || !req->command_name || req->command_name[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid slash command");
    }
    if (req->command_type > 3u) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid command_type");
    }
    size_t nlen = strlen(req->command_name);
    if (nlen > CCORD_SLASH_NAME_MAX_BYTES || !ccord_utf8_valid_text(req->command_name, nlen)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid command_name");
    }
    has_group = req->subcommand_group && req->subcommand_group[0] != '\0';
    has_sub = req->subcommand && req->subcommand[0] != '\0';
    if (has_group) grp_name = req->subcommand_group;
    if (has_sub) sub_name = req->subcommand;

    if (req->command_type == 2u || req->command_type == 3u) {
      if (req->type != CCORD_INTERACTION_APPLICATION_COMMAND ||
          req->target_id == CCORD_ID_NONE || req->option_count > 0 || has_group || has_sub) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid context-menu command");
      }
      if (req->command_type == 2u) {
        if (mock_user_name(m, req->target_id)[0] == '\0') {
          return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown target user");
        }
      } else {
        target_mi = find_message(m, ci, req->target_id);
        if (target_mi < 0) {
          return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown target message");
        }
      }
      ident = req->command_name;
    } else {
      if (req->target_id != CCORD_ID_NONE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "mock: unexpected target_id on chat command");
      }
      for (size_t i = 0; i < nlen; i++) {
        char c = req->command_name[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
          return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid command_name");
        }
      }
      if (has_group && !has_sub) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "mock: subcommand_group without subcommand");
      }
      if (has_group) {
        size_t glen = strlen(grp_name);
        if (glen > CCORD_SLASH_NAME_MAX_BYTES || !ccord_utf8_valid_text(grp_name, glen)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid subcommand_group");
        }
        for (size_t i = 0; i < glen; i++) {
          char c = grp_name[i];
          if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
            return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid subcommand_group");
          }
        }
      }
      if (has_sub) {
        size_t slen = strlen(sub_name);
        if (slen > CCORD_SLASH_NAME_MAX_BYTES || !ccord_utf8_valid_text(sub_name, slen)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid subcommand");
        }
        for (size_t i = 0; i < slen; i++) {
          char c = sub_name[i];
          if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
            return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid subcommand");
          }
        }
      }
      if (req->option_count > CCORD_SLASH_MAX_OPTIONS ||
          (req->option_count > 0 && (!req->option_names || !req->option_kinds || !req->option_values))) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid slash options");
      }
      for (size_t i = 0; i < req->option_count; i++) {
        const char *on = req->option_names[i];
        if (!on || on[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid option name");
        size_t olen = strlen(on);
        if (olen > CCORD_SLASH_NAME_MAX_BYTES || !ccord_utf8_valid_text(on, olen)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid option name");
        }
        uint8_t ok = req->option_kinds[i];
        if (ok < CCORD_SLASH_OPT_STRING || ok > CCORD_SLASH_OPT_NUMBER) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid option kind");
        }
        const char *ov = req->option_values[i] ? req->option_values[i] : "";
        size_t ovlen = strlen(ov);
        if (ovlen > CCORD_SEND_MAX_BYTES || !ccord_utf8_valid_text(ov, ovlen)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid option value");
        }
      }
      if (req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE &&
          (req->option_count == 0 || req->focused_option < 0 || (size_t)req->focused_option >= req->option_count)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid focused_option");
      }
      if (has_group) {
        (void)snprintf(ident_buf, sizeof ident_buf, "%s %s %s", req->command_name, grp_name, sub_name);
        ident = ident_buf;
      } else if (has_sub) {
        (void)snprintf(ident_buf, sizeof ident_buf, "%s %s", req->command_name, sub_name);
        ident = ident_buf;
      } else {
        ident = req->command_name;
      }
    }
  } else {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: unsupported interaction type");
  }

  if (m->cfg.refuse_writes && req->type != CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE) {
    return queue_action_refused(m, CCORD_ACTION_INTERACTION, req->channel_id, req->message_id, "", 0, nullptr, 0, err);
  }

  clear_last_interaction_values(m);
  m->interaction_count++;
  m->last_interaction_type = (uint8_t)req->type;
  m->last_interaction_comp_type = req->component_type;
  (void)snprintf(m->last_interaction_custom_id, sizeof m->last_interaction_custom_id, "%s", ident);
  if (req->type == CCORD_INTERACTION_MESSAGE_COMPONENT) {
    for (size_t i = 0; i < req->value_count; i++) {
      const char *v = req->values[i] ? req->values[i] : "";
      m->last_interaction_values[i] = ccord_strndup(v, strlen(v));
      m->last_interaction_nvalues = i + 1u;
    }
    if (strncmp(req->custom_id, "modal:", 6) == 0) {
      mock_item it = make_item(CCORD_NET_INTERACTION_MODAL, now_of(m) + m->cfg.latency_ms);
      it.modal_own = ccord_calloc(1, sizeof *it.modal_own);
      if (!it.modal_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      ccord_interaction_modal *md = it.modal_own;
      md->id = m->next_msg_id++;
      md->application_id = req->application_id;
      md->guild_id = req->guild_id;
      md->channel_id = req->channel_id;
      const char *mcid = req->custom_id[6] ? req->custom_id + 6 : "modal_submit";
      snprintf(md->custom_id, sizeof md->custom_id, "%s", mcid);
      snprintf(md->title, sizeof md->title, "Feedback Form");
      md->field_count = 2;
      snprintf(md->fields[0].custom_id, sizeof md->fields[0].custom_id, "subject");
      snprintf(md->fields[0].label, sizeof md->fields[0].label, "Subject");
      snprintf(md->fields[0].placeholder, sizeof md->fields[0].placeholder, "Short summary");
      md->fields[0].style = 1;
      md->fields[0].min_length = 2;
      md->fields[0].max_length = 80;
      md->fields[0].required = true;
      snprintf(md->fields[1].custom_id, sizeof md->fields[1].custom_id, "details");
      snprintf(md->fields[1].label, sizeof md->fields[1].label, "Details");
      snprintf(md->fields[1].placeholder, sizeof md->fields[1].placeholder, "Optional notes");
      md->fields[1].style = 2;
      md->fields[1].max_length = 500;
      md->fields[1].required = false;
      it.ev.modal = md;
      it.ev.guild_id = md->guild_id;
      it.ev.channel_id = md->channel_id;
      return push_item(m, it, err);
    }
  } else if (req->type == CCORD_INTERACTION_MODAL_SUBMIT) {
    for (size_t i = 0; i < req->field_count; i++) {
      const char *v = req->field_values[i] ? req->field_values[i] : "";
      m->last_interaction_values[i] = ccord_strndup(v, strlen(v));
      m->last_interaction_nvalues = i + 1u;
    }
    char reply[256];
    const char *v0 = (req->field_count > 0 && req->field_values[0]) ? req->field_values[0] : "";
    int rn = snprintf(reply, sizeof reply, "Modal submitted: %s", v0);
    if (rn > 0) {
      size_t rlen = (size_t)rn < sizeof reply ? (size_t)rn : sizeof reply - 1u;
      ccord_message msg = store_new(m, ci, req->application_id, reply, rlen, err, &st);
      if (st != CCORD_OK) return st;
      return queue_message_create(m, msg, now_of(m) + m->cfg.latency_ms, err);
    }
  } else if (req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE) {
    for (size_t i = 0; i < req->option_count && i < CCORD_RICH_MAX_SELECT_OPTIONS; i++) {
      const char *v = req->option_values[i] ? req->option_values[i] : "";
      m->last_interaction_values[i] = ccord_strndup(v, strlen(v));
      m->last_interaction_nvalues = i + 1u;
    }
    const char *q = req->option_values[req->focused_option] ? req->option_values[req->focused_option] : "";
    size_t qlen = strlen(q);
    static const struct { const char *name; const char *value; } k_topics[4] = {
      {"C23 Language & Atomics", "c23"},
      {"SDL3 GPU & Windowing", "sdl3"},
      {"Vulkan Render Pipelines", "vulkan"},
      {"Gateway & REST Networking", "networking"},
    };
    mock_item it = make_item(CCORD_NET_SLASH_AUTOCOMPLETE, now_of(m) + m->cfg.latency_ms);
    it.choices_own = ccord_calloc(4, sizeof *it.choices_own);
    if (!it.choices_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    size_t out_n = 0;
    for (size_t i = 0; i < 4; i++) {
      if (qlen == 0 || has_ci_substr(k_topics[i].value, q, qlen) || has_ci_substr(k_topics[i].name, q, qlen)) {
        (void)snprintf(it.choices_own[out_n].name, sizeof it.choices_own[out_n].name, "%s", k_topics[i].name);
        (void)snprintf(it.choices_own[out_n].value, sizeof it.choices_own[out_n].value, "%s", k_topics[i].value);
        out_n++;
      }
    }
    it.ev.guild_id = req->guild_id;
    it.ev.channel_id = req->channel_id;
    it.ev.slash_choices = it.choices_own;
    it.ev.slash_choice_count = out_n;
    it.ev.interaction_nonce = "mock-nonce";
    return push_item(m, it, err);
  } else if (req->command_type == 2u || req->command_type == 3u) {
    char tbuf[32];
    int tn = snprintf(tbuf, sizeof tbuf, "%llu", (unsigned long long)req->target_id);
    if (tn > 0) {
      m->last_interaction_values[0] = ccord_strndup(tbuf, (size_t)tn);
      m->last_interaction_nvalues = 1u;
    }
    char reply[256];
    int rn = 0;
    bool ephemeral = false;
    if (req->command_type == 2u) {
      const char *uname = mock_user_name(m, req->target_id);
      ephemeral = true;
      rn = snprintf(reply, sizeof reply, "User Info: %s (%llu)", uname, (unsigned long long)req->target_id);
    } else {
      const ccord_message *tmsg = &m->chmsgs[ci][(size_t)target_mi];
      const char *author = mock_user_name(m, tmsg->author_id);
      const char *body = tmsg->content ? tmsg->content : "";
      rn = snprintf(reply, sizeof reply, "Quote (%s): %.160s", author[0] ? author : "user", body);
    }
    if (rn > 0) {
      size_t rlen = (size_t)rn < sizeof reply ? (size_t)rn : sizeof reply - 1u;
      ccord_message_rich r = {
        .present = CCORD_RICH_HAS_TYPE,
        .type = 23,
        .flags = ephemeral ? CCORD_MSGFLAG_EPHEMERAL : 0u,
      };
      ccord_message msg = store_new_rich(m, ci, req->application_id, reply, rlen, &r, err, &st);
      if (st != CCORD_OK) return st;
      return queue_message_create(m, msg, now_of(m) + m->cfg.latency_ms, err);
    }
  } else {
    for (size_t i = 0; i < req->option_count && i < CCORD_RICH_MAX_SELECT_OPTIONS; i++) {
      const char *v = req->option_values[i] ? req->option_values[i] : "";
      m->last_interaction_values[i] = ccord_strndup(v, strlen(v));
      m->last_interaction_nvalues = i + 1u;
    }
    bool ephemeral = strcmp(req->command_name, "ping") == 0;
    const char *text_val = "";
    for (size_t i = 0; i < req->option_count; i++) {
      const char *on = req->option_names[i] ? req->option_names[i] : "";
      const char *ov = req->option_values[i] ? req->option_values[i] : "";
      if (strcmp(on, "ephemeral") == 0 && (strcmp(ov, "true") == 0 || strcmp(ov, "1") == 0)) ephemeral = true;
      if (i == 0 || strcmp(on, "text") == 0 || strcmp(on, "subject") == 0 || strcmp(on, "preset") == 0) text_val = ov;
    }
    char reply[256];
    int rn = 0;
    if (has_sub) {
      if (strcmp(req->command_name, "config") == 0 && !has_group && strcmp(sub_name, "show") == 0) {
        ephemeral = true;
        rn = snprintf(reply, sizeof reply, "Config: theme=dark, compact=false");
      } else if (strcmp(req->command_name, "config") == 0 && has_group &&
                 strcmp(grp_name, "theme") == 0 && strcmp(sub_name, "set") == 0) {
        rn = snprintf(reply, sizeof reply, "Config theme set: %s", text_val);
      } else if (strcmp(req->command_name, "config") == 0 && has_group &&
                 strcmp(grp_name, "theme") == 0 && strcmp(sub_name, "reset") == 0) {
        rn = snprintf(reply, sizeof reply, "Config theme reset to default");
      } else if (has_group) {
        rn = text_val[0]
                 ? snprintf(reply, sizeof reply, "Command /%s %s %s: %s", req->command_name, grp_name, sub_name, text_val)
                 : snprintf(reply, sizeof reply, "Command /%s %s %s", req->command_name, grp_name, sub_name);
      } else {
        rn = text_val[0]
                 ? snprintf(reply, sizeof reply, "Command /%s %s: %s", req->command_name, sub_name, text_val)
                 : snprintf(reply, sizeof reply, "Command /%s %s", req->command_name, sub_name);
      }
    } else if (strcmp(req->command_name, "ping") == 0) {
      rn = snprintf(reply, sizeof reply, "Pong! 12 ms");
    } else if (strcmp(req->command_name, "echo") == 0) {
      rn = snprintf(reply, sizeof reply, "Echo: %s", text_val);
    } else {
      rn = snprintf(reply, sizeof reply, "Command /%s: %s", req->command_name, text_val);
    }
    if (rn > 0) {
      size_t rlen = (size_t)rn < sizeof reply ? (size_t)rn : sizeof reply - 1u;
      ccord_message_rich r = {
        .present = CCORD_RICH_HAS_TYPE,
        .type = 20,
        .flags = ephemeral ? CCORD_MSGFLAG_EPHEMERAL : 0u,
      };
      ccord_message msg = store_new_rich(m, ci, req->application_id, reply, rlen, &r, err, &st);
      if (st != CCORD_OK) return st;
      return queue_message_create(m, msg, now_of(m) + m->cfg.latency_ms, err);
    }
  }
  return CCORD_OK;
}

size_t ccord_mock_interaction_count(const ccord_transport *t)
{
  return t ? ((const mock *)t)->interaction_count : 0;
}

uint8_t ccord_mock_last_interaction_type(const ccord_transport *t)
{
  return t ? ((const mock *)t)->last_interaction_type : 0;
}

uint8_t ccord_mock_last_interaction_component_type(const ccord_transport *t)
{
  return t ? ((const mock *)t)->last_interaction_comp_type : 0;
}

const char *ccord_mock_last_interaction_custom_id(const ccord_transport *t)
{
  if (!t) return nullptr;
  const mock *m = (const mock *)t;
  return m->interaction_count ? m->last_interaction_custom_id : nullptr;
}

size_t ccord_mock_last_interaction_value_count(const ccord_transport *t)
{
  return t ? ((const mock *)t)->last_interaction_nvalues : 0;
}

const char *ccord_mock_last_interaction_value(const ccord_transport *t, size_t index)
{
  if (!t) return nullptr;
  const mock *m = (const mock *)t;
  return index < m->last_interaction_nvalues ? m->last_interaction_values[index] : nullptr;
}

// ---- Gateway link model ----

static uint64_t mock_link_ttl(const mock *m)
{
  return m->link.ttl_set ? m->link.ttl_ms : MOCK_LINK_TTL_DEFAULT_MS;
}

// A fresh connection: heartbeat and uptime start now, the session is valid, and no earlier wake or attempt counts.
static void mock_link_on_connect(mock *m, uint64_t now)
{
  mock_link *l = &m->link;
  l->state = MOCK_LINK_UP;
  l->dead_silent = false;
  l->session_expired = false;
  l->attempt_resume = false;
  ccord_gwpace_reset(&l->pace);
  l->pace.wake_last_ms = 0;
  l->pace.attempt_ms = 0;
  ccord_gwpace_hello(&l->pace, now, MOCK_LINK_HB_INTERVAL_MS);
  ccord_gwpace_connected(&l->pace, now);
}

static void mock_link_push_state(mock *m, ccord_connection_state state, uint64_t due)
{
  mock_item st = make_item(CCORD_NET_STATE, due);
  st.ev.state = state;
  (void)push_item(m, st, nullptr);   // out of memory only loses a state notification
}

// The socket is gone (closed, errored or declared a zombie): the app sees CONNECTING and a reconnect is up to the pace.
static void mock_link_lose(mock *m, uint64_t now)
{
  mock_link *l = &m->link;
  l->state = MOCK_LINK_DOWN;
  l->dead_silent = false;
  ccord_gwpace_disconnected(&l->pace);
  mock_link_push_state(m, CCORD_CONN_CONNECTING, now);
}

// Moves the last `added` queue items to the front, so a reconnect's CONNECTED / SNAPSHOT come before events that were
// held back while the link was down (a real RESUME would replay those after RESUMED).
static void mock_queue_front(mock *m, size_t first)
{
  size_t added = m->nq - first;
  if (first == 0 || added == 0) return;
  mock_item *tmp = ccord_alloc_array(added, sizeof *tmp);
  if (!tmp) return;   // order is cosmetic; keep the items
  // A CONNECTING queued by the loss that this reconnect repairs (same instant when latency is 0) stays ahead of it, or the
  // session would end up CONNECTING after the reconnect finished.
  size_t at = 0;
  for (size_t i = 0; i < first; i++) {
    if (m->queue[i].ev.type == CCORD_NET_STATE && m->queue[i].ev.state == CCORD_CONN_CONNECTING) at = i + 1;
  }
  memcpy(tmp, m->queue + first, added * sizeof *tmp);
  memmove(m->queue + at + added, m->queue + at, (first - at) * sizeof *m->queue);
  memcpy(m->queue + at, tmp, added * sizeof *tmp);
  ccord_free(tmp);
}

static void mock_link_up(mock *m, uint64_t now)
{
  mock_link *l = &m->link;
  l->state = MOCK_LINK_UP;
  ccord_gwpace_connected(&l->pace, now);
  ccord_gwpace_hello(&l->pace, now, MOCK_LINK_HB_INTERVAL_MS);
  size_t first = m->nq;
  if (l->attempt_resume) {
    mock_link_push_state(m, CCORD_CONN_CONNECTED, now);
  } else {
    l->session_expired = false;
    (void)mock_queue_ready(m, now, nullptr);
  }
  mock_queue_front(m, first);
}

// The planned reconnect is due: open the socket (RESUME or IDENTIFY), or fail like a refused open.
static void mock_link_attempt(mock *m, uint64_t now, bool resume)
{
  mock_link *l = &m->link;
  ccord_gwpace_reconnect_started(&l->pace);
  l->stats.attempts++;
  if (l->network_down) {
    (void)ccord_gwpace_plan_reconnect(&l->pace, now, CCORD_GWPACE_NETWORK, !l->session_expired);
    return;
  }
  if (resume && !l->session_expired) {
    if (now >= l->lost_ms && now - l->lost_ms > mock_link_ttl(m)) {
      // op 9 d:false: the session is gone; wait 1-5 s, then IDENTIFY.
      l->session_expired = true;
      (void)ccord_gwpace_plan_reconnect(&l->pace, now, CCORD_GWPACE_INVALID_SESSION, false);
      return;
    }
    l->attempt_resume = true;
    l->stats.resumes++;
  } else {
    l->attempt_resume = false;
    l->stats.identifies++;
    ccord_gwpace_identify_sent(&l->pace, now);
  }
  l->state = MOCK_LINK_ATTEMPT;
  l->up_ms = now + m->cfg.latency_ms;
}

// Poll-side upkeep, like ccord_gateway_poll: heartbeat, zombie detection, due reconnects, a finished socket open.
static void mock_link_tick(mock *m, uint64_t now)
{
  mock_link *l = &m->link;
  if (l->state == MOCK_LINK_UP) {
    ccord_gwpace_tick(&l->pace, now);
    ccord_gwpace_beat beat = ccord_gwpace_heartbeat_due(&l->pace, now);
    if (beat == CCORD_GWPACE_BEAT_SEND) {
      ccord_gwpace_heartbeat_sent(&l->pace, now);
      if (!l->dead_silent) (void)ccord_gwpace_heartbeat_ack(&l->pace, now);
    } else if (beat == CCORD_GWPACE_BEAT_ZOMBIE) {
      mock_link_lose(m, now);
      (void)ccord_gwpace_plan_reconnect(&l->pace, now, CCORD_GWPACE_ZOMBIE, !l->session_expired);
    }
  }
  bool resume = false;
  if (l->state == MOCK_LINK_DOWN && ccord_gwpace_reconnect_due(&l->pace, now, &resume)) mock_link_attempt(m, now, resume);
  if (l->state == MOCK_LINK_ATTEMPT && now >= l->up_ms) mock_link_up(m, now);
}

static ccord_status mock_network_event(ccord_transport *t, ccord_link_event kind, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (kind != CCORD_LINK_RESUMED && kind != CCORD_LINK_CHANGED) return CCORD_OK;
  mock_link *l = &m->link;
  uint64_t now = now_of(m);
  l->stats.wakes_seen++;
  ccord_gwpace_wake_verdict v = CCORD_GWPACE_WAKE_IGNORED;
  if (m->connected) v = ccord_gwpace_plan_wake(&l->pace, now, kind == CCORD_LINK_CHANGED, !l->session_expired);
  l->stats.last_verdict = v;
  if (v != CCORD_GWPACE_WAKE_ACCEPTED) return CCORD_OK;
  l->stats.wakes_accepted++;
  // Close the socket (dead or not) and let the next poll reconnect; the plan is already due.
  if (l->state == MOCK_LINK_UP) {
    if (!l->dead_silent) l->lost_ms = now;
    mock_link_lose(m, now);
  } else {
    l->state = MOCK_LINK_DOWN;
  }
  return CCORD_OK;
}

void ccord_mock_link_stats_get(const ccord_transport *t, ccord_mock_link_stats *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!t) return;
  const mock *m = (const mock *)t;
  *out = m->link.stats;
  out->dead_silent = m->link.dead_silent;
  if (ccord_gwpace_reconnect_pending(&m->link.pace)) {
    uint64_t now = now_of(m);
    uint64_t at = m->link.pace.reconnect_at_ms;
    out->next_attempt_in_ms = at > now ? at - now : 0;
  }
}

void ccord_mock_drop_gateway(ccord_transport *t, bool silent)
{
  if (!t) return;
  mock *m = (mock *)t;
  if (!m->connected) return;
  mock_link *l = &m->link;
  uint64_t now = now_of(m);
  if (l->state != MOCK_LINK_UP) return;
  if (!l->dead_silent) l->lost_ms = now;
  if (silent) {
    l->dead_silent = true;
    return;
  }
  mock_link_lose(m, now);
  (void)ccord_gwpace_plan_reconnect(&l->pace, now, CCORD_GWPACE_NETWORK, !l->session_expired);
}

void ccord_mock_set_network(ccord_transport *t, bool up)
{
  if (t) ((mock *)t)->link.network_down = !up;
}

void ccord_mock_set_session_ttl(ccord_transport *t, uint64_t ms)
{
  if (!t) return;
  mock *m = (mock *)t;
  m->link.ttl_set = true;
  m->link.ttl_ms = ms;
}

static bool mock_poll(ccord_transport *t, ccord_net_event *out)
{
  mock *m = (mock *)t;
  // Pointers handed out by the previous poll end here.
  if (m->have_current) {
    item_release(&m->current);
    m->have_current = false;
    memset(&m->current, 0, sizeof m->current);
  }
  uint64_t now = now_of(m);
  if (m->connected) mock_link_tick(m, now);
  if (m->connected && m->cfg.ambient_interval_ms && now >= m->next_ambient && m->next_ambient) {
    m->next_ambient = now + m->cfg.ambient_interval_ms;
    ccord_status cst;
    size_t ci = channel_index(m, MOCK_GENERAL_ID, true, &cst, nullptr);
    if (ci != SIZE_MAX) {
      char text[64];
      int n = snprintf(text, sizeof text, "ambient message %llu", (unsigned long long)m->next_msg_id);
      ccord_status st;
      ccord_message msg = store_new(m, ci, m->users[10].id, text, (size_t)n, nullptr, &st);
      if (st == CCORD_OK) (void)queue_message_create(m, msg, now, nullptr);
    }
  }
  // Link down: only the mock's own state events get through; a silently dead socket delivers nothing at all.
  size_t pick = 0;
  if (m->connected && (m->link.state != MOCK_LINK_UP || m->link.dead_silent)) {
    pick = SIZE_MAX;
    if (!m->link.dead_silent) {
      for (size_t i = 0; i < m->nq; i++) {
        if (m->queue[i].ev.type == CCORD_NET_STATE && m->queue[i].due <= now) {
          pick = i;
          break;
        }
      }
    }
  }
  if (m->nq == 0 || pick == SIZE_MAX || m->queue[pick].due > now) return false;

  m->current = m->queue[pick];
  m->have_current = true;
  memmove(&m->queue[pick], &m->queue[pick + 1u], (m->nq - pick - 1u) * sizeof *m->queue);
  m->nq--;
  if (m->current.ev.type == CCORD_NET_MESSAGE_CREATE && m->current.ev.message_count > 0 && m->current.ev.messages) {
    ccord_id mid = m->current.ev.messages[0].id;
    for (size_t i = 0; i < m->ninflight_uploads; i++) {
      if (m->inflight_uploads[i].message_id == mid) {
        memmove(&m->inflight_uploads[i], &m->inflight_uploads[i + 1u],
                (m->ninflight_uploads - i - 1u) * sizeof m->inflight_uploads[0]);
        m->ninflight_uploads--;
        break;
      }
    }
  }
  *out = m->current.ev;
  return true;
}

ccord_id ccord_mock_last_ring_channel(const ccord_transport *t)
{
  return t ? ((const mock *)t)->last_ring_channel : CCORD_ID_NONE;
}

void ccord_mock_set_ring_refusal(ccord_transport *t, const char *reason)
{
  if (!t) return;
  mock *m = (mock *)t;
  (void)snprintf(m->ring_refusal, sizeof m->ring_refusal, "%s", reason ? reason : "");
}

void ccord_mock_set_late_poll_failure(ccord_transport *t, const char *reason)
{
  if (!t) return;
  mock *m = (mock *)t;
  (void)snprintf(m->late_poll_fail, sizeof m->late_poll_fail, "%s", reason ? reason : "");
}

// Whole outage computed now with the real policy (voice_reconnect.c) on the mock clock; see mock_transport.h.
ccord_status ccord_mock_voice_drop(ccord_transport *t, const ccord_mock_vdrop *d, ccord_error *err)
{
  ccord_error_clear(err);
  if (!t || !d) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad voice drop args");
  mock *m = (mock *)t;
  long vi = -1;
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].user_id == m->self_id && m->voices[i].channel_id != CCORD_ID_NONE) {
      vi = (long)i;
      break;
    }
  }
  if (vi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: not in voice");
  mock_purge_voice_link(m);
  ccord_voice_state self = m->voices[vi];
  uint32_t drops = m->vstats.drops + 1u;
  memset(&m->vstats, 0, sizeof m->vstats);
  m->vstats.drops = drops;

  ccord_vc vc;
  ccord_vc_init(&vc, 0x5EED0000ull + drops);
  uint16_t code = d->kind == CCORD_MOCK_VDROP_REJOIN ? 4006u : d->kind == CCORD_MOCK_VDROP_AWAIT_MAIN ? 4014u
                : d->kind == CCORD_MOCK_VDROP_FATAL ? 4021u : 0u;
  ccord_vc_cause cause = code ? CCORD_VC_CAUSE_CLOSE : CCORD_VC_CAUSE_IO;
  uint64_t t0 = now_of(m), at = t0;   // `at` = when the next plan is made
  uint32_t failed = 0;
  ccord_status st = CCORD_OK;
  for (;;) {
    ccord_vc_plan p = ccord_vc_plan_next(&vc, at, code, cause, true);
    const char *reason = nullptr;
    bool tell_server = true;
    if (p.action == CCORD_VC_AWAIT_MAIN) {
      st = queue_voice_link(m, CCORD_VLINK_RECONNECTING, 0, p.delay_ms, "", at, err);
      if (st != CCORD_OK) break;
      at += p.delay_ms;
      reason = "you were disconnected";
      tell_server = false;
    } else if (p.action == CCORD_VC_FINAL) {
      reason = p.reason && p.reason[0] ? p.reason : "connection lost";
    }
    if (reason) {   // give-up: own voice state gone first (as the live transport does), then FAILED, then the notice
      ccord_voice_state gone = self;
      gone.channel_id = CCORD_ID_NONE;
      gone.self_video = false;
      gone.self_stream = false;
      st = queue_voice_state(m, &gone, at, err);
      if (st == CCORD_OK) st = queue_voice_link(m, CCORD_VLINK_FAILED, vc.attempt, 0, reason, at, err);
      if (st == CCORD_OK) {
        mock_item it = make_item(CCORD_NET_ACTION_FAILED, at);
        it.ev.action = CCORD_ACTION_VOICE;
        it.ev.guild_id = self.guild_id;
        it.ev.channel_id = self.channel_id;
        it.ev.status = CCORD_ERR_IO;
        it.ev.text = reason;
        st = push_item(m, it, err);
      }
      m->vstats.give_ups++;
      if (tell_server) m->vstats.leaves_sent++;
      break;
    }
    uint64_t ta = at + p.delay_ms;
    st = queue_voice_link(m, CCORD_VLINK_RECONNECTING, p.attempt, p.delay_ms, "", at, err);
    if (st != CCORD_OK) break;
    if (m->vstats.attempts < sizeof m->vstats.attempt_ms / sizeof m->vstats.attempt_ms[0]) {
      m->vstats.attempt_ms[m->vstats.attempts] = ta;
      m->vstats.attempt_action[m->vstats.attempts] = p.action;
    }
    m->vstats.attempts++;
    // An attempt that starts while the network is down waits for it (up to the connect timeout) and then gets
    // through; one that would wait longer times out. fail_attempts refuses the first N attempts at once.
    bool net_up = ta + CCORD_VC_CONNECT_TIMEOUT_MS >= t0 + d->down_ms;
    uint64_t tc = ta > t0 + d->down_ms ? ta : t0 + d->down_ms;   // when the attempt completes
    if (net_up && failed >= d->fail_attempts) {   // this attempt gets through
      ta = tc;
      if (p.action == CCORD_VC_RESUME) {
        m->vstats.resumes++;
      } else {   // a new voice session: fresh credentials, and Discord ends our Go Live stream with the old one
        m->vstats.rejoins++;
        if (m->has_stream && m->stream.user_id == m->self_id) {
          size_t before = m->nq;
          st = queue_stream_signal_ex(m, m->stream.guild_id, m->stream.channel_id, m->stream.user_id,
                                      CCORD_STREAM_SIG_DELETE, 0, false, "voice reconnected", err);
          if (st == CCORD_OK && m->nq > before) m->queue[m->nq - 1].due = ta;
          m->has_stream = false;
          m->voices[vi].self_stream = false;
          mock_stream_media_clear(m);
        }
        if (st == CCORD_OK) st = queue_voice_state(m, &m->voices[vi], ta, err);
        if (st == CCORD_OK) {
          mock_item it = make_item(CCORD_NET_VOICE_SERVER_UPDATE, ta);
          it.vserver_own = ccord_alloc_array(1, sizeof *it.vserver_own);
          if (!it.vserver_own) {
            st = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          } else {
            *it.vserver_own = (ccord_voice_server){.guild_id = self.guild_id, .channel_id = self.channel_id,
                                                   .token = "mock-voice-token-2",
                                                   .endpoint = "mock.voice.discord:443"};
            it.ev.voice_server = it.vserver_own;
            it.ev.guild_id = self.guild_id;
            it.ev.channel_id = self.channel_id;
            st = push_item(m, it, err);
          }
        }
      }
      if (st == CCORD_OK) st = queue_voice_link(m, CCORD_VLINK_CONNECTED, p.attempt, 0, "", ta, err);
      break;
    }
    failed++;
    at = net_up ? ta : ta + CCORD_VC_CONNECT_TIMEOUT_MS;   // down: the attempt times out; refused: at once
    if (d->kind == CCORD_MOCK_VDROP_RESUMABLE) {   // later failures: the socket never came up
      code = 0;
      cause = CCORD_VC_CAUSE_CONNECT;
    }
  }
  return st;
}

void ccord_mock_voice_link_stats(const ccord_transport *t, ccord_mock_vlink_stats *out)
{
  if (!out) return;
  if (t) *out = ((const mock *)t)->vstats;
  else memset(out, 0, sizeof *out);
}

size_t ccord_mock_presence_send_count(const ccord_transport *t)
{
  return t ? ((const mock *)t)->op3_count : 0;
}

ccord_presence ccord_mock_last_presence_send(const ccord_transport *t, const char **custom_text)
{
  const mock *m = (const mock *)t;
  if (custom_text) *custom_text = m ? m->op3_text : "";
  return m ? m->op3_presence : CCORD_PRESENCE_OFFLINE;
}

// Sets the mock account's own activity (a Playing line, or none) in the dataset the next SNAPSHOT is built from.
static void mock_set_self_activity_fields(mock *m, const char *name)
{
  ccord_user *u = &m->users[0];
  u->has_activity = name && name[0] != '\0';
  u->activity_type = CCORD_ACTIVITY_PLAYING;
  memset(u->activity_name, 0, sizeof u->activity_name);
  memset(u->activity_details, 0, sizeof u->activity_details);
  memset(u->activity_state, 0, sizeof u->activity_state);
  u->activity_start_ms = 0;
  u->activity_end_ms = 0;
  if (u->has_activity) (void)snprintf(u->activity_name, sizeof u->activity_name, "%s", name);
}

void ccord_mock_set_own_presence_echo(ccord_transport *t, bool echo)
{
  if (t) ((mock *)t)->own_presence_silent = !echo;
}

ccord_status ccord_mock_set_self_activity(ccord_transport *t, const char *name, bool echo, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock_set_self_activity_fields(m, name);
  if (!echo) return CCORD_OK;
  mock_item it = make_item(CCORD_NET_PRESENCE_UPDATE, now_of(m) + m->cfg.latency_ms);
  it.ev.user_id = m->self_id;
  it.ev.presence = m->users[0].presence;
  it.ev.status_text = "";
  it.ev.has_presence_detail = true;
  (void)snprintf(it.ev.custom_status_emoji, sizeof it.ev.custom_status_emoji, "%s", m->users[0].custom_status_emoji);
  (void)snprintf(it.ev.custom_status_text, sizeof it.ev.custom_status_text, "%s", m->users[0].custom_status_text);
  it.ev.custom_status_expires_ms = m->users[0].custom_status_expires_ms;
  it.ev.has_activity = m->users[0].has_activity;
  it.ev.activity_type = (ccord_activity_type)m->users[0].activity_type;
  (void)snprintf(it.ev.activity_name, sizeof it.ev.activity_name, "%s", m->users[0].activity_name);
  return push_item(m, it, err);
}

ccord_status ccord_mock_inject_sessions_replace_activity(ccord_transport *t, ccord_presence overall,
                                                         const char *activity_name, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock_set_self_activity_fields(m, activity_name);
  mock_item it = make_item(CCORD_NET_SESSIONS_REPLACE, now_of(m) + m->cfg.latency_ms);
  it.ev.presence = overall;
  it.ev.has_presence_detail = true;
  it.ev.has_activity = m->users[0].has_activity;
  it.ev.activity_type = (ccord_activity_type)m->users[0].activity_type;
  (void)snprintf(it.ev.activity_name, sizeof it.ev.activity_name, "%s", m->users[0].activity_name);
  return push_item(m, it, err);
}

ccord_status ccord_mock_inject_sessions_replace(ccord_transport *t, ccord_presence overall, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock_item it = make_item(CCORD_NET_SESSIONS_REPLACE, now_of(m) + m->cfg.latency_ms);
  it.ev.presence = overall;
  return push_item(m, it, err);
}

void ccord_mock_set_status_proto_base(ccord_transport *t, const uint8_t *proto, size_t len)
{
  if (!t || (len > 0 && !proto)) return;
  mock *m = (mock *)t;
  uint8_t *copy = ccord_alloc_array(len ? len : 1u, 1);
  if (!copy) return;
  if (len > 0) memcpy(copy, proto, len);
  ccord_free(m->status1);
  m->status1 = copy;
  m->status1_len = len;
}

const uint8_t *ccord_mock_status_proto_base(const ccord_transport *t, size_t *len)
{
  const mock *m = (const mock *)t;
  if (len) *len = m ? m->status1_len : 0;
  return m ? m->status1 : nullptr;
}

size_t ccord_mock_status_proto_patch_count(const ccord_transport *t)
{
  return t ? ((const mock *)t)->nstatus1_patches : 0;
}

const uint8_t *ccord_mock_status_proto_patch_at(const ccord_transport *t, size_t i, size_t *len)
{
  const mock *m = (const mock *)t;
  if (len) *len = 0;
  if (!m || i >= m->nstatus1_patches) return nullptr;
  if (len) *len = m->status1_patches[i].len;
  return m->status1_patches[i].raw;
}

const char *ccord_mock_status_proto_patch_body(const ccord_transport *t, size_t i)
{
  const mock *m = (const mock *)t;
  return m && i < m->nstatus1_patches ? m->status1_patches[i].body : nullptr;
}

ccord_status ccord_mock_inject_status_proto_update(ccord_transport *t, const uint8_t *proto, size_t len, bool partial,
                                                   ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m || (len > 0 && !proto)) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad status proto update");
  uint8_t *ev_bytes = ccord_alloc_array(len ? len : 1u, 1);
  if (!ev_bytes) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  if (len > 0) memcpy(ev_bytes, proto, len);
  if (!partial) ccord_mock_set_status_proto_base(t, proto, len);   // the server now holds what the other client wrote
  mock_item it = make_item(CCORD_NET_USER_SETTINGS_PROTO, now_of(m) + m->cfg.latency_ms);
  it.raw_proto_own = ev_bytes;   // owned by the item; the event borrows it
  it.ev.settings_proto1 = ev_bytes;
  it.ev.settings_proto1_len = len;
  it.ev.settings_proto1_partial = partial;
  return push_item(m, it, err);
}

ccord_status ccord_mock_inject_settings_proto_update(ccord_transport *t, const uint8_t *proto, size_t len, bool partial,
                                                     ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m || (len > 0 && !proto)) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad settings proto update");
  uint8_t *ev_bytes = ccord_alloc_array(len ? len : 1u, 1);
  if (!ev_bytes) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  if (len > 0) memcpy(ev_bytes, proto, len);
  if (!partial) {   // the server now holds what the other client wrote: later GETs and PATCH bases see it
    uint8_t *own = ccord_alloc_array(len ? len : 1u, 1);
    if (!own) {
      ccord_free(ev_bytes);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    if (len > 0) memcpy(own, proto, len);
    ccord_free(m->settings_proto);
    m->settings_proto = own;
    m->settings_proto_len = len;
    m->has_settings_proto = true;
  }
  mock_item it = make_item(CCORD_NET_USER_SETTINGS_PROTO, now_of(m) + m->cfg.latency_ms);
  it.raw_proto_own = ev_bytes;
  it.ev.raw_proto = ev_bytes;
  it.ev.raw_proto_len = len;
  it.ev.settings_proto1_partial = partial;
  return push_item(m, it, err);
}

void ccord_mock_set_refuse_writes(ccord_transport *t, bool refuse)
{
  if (t) ((mock *)t)->cfg.refuse_writes = refuse;
}

ccord_status ccord_mock_add_guild_member(ccord_transport *t, ccord_id guild_id, ccord_id user_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!t || guild_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad add member args");
  }
  mock *m = (mock *)t;
  if (mock_guild_index(m, guild_id) == SIZE_MAX) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown guild");
  }
  long existing = admin_member_index(m, guild_id, user_id);
  size_t mi = 0;
  if (existing >= 0) {
    mi = (size_t)existing;
  } else {
    ccord_member *nm = ccord_realloc_array(m->members, m->nmembers + 1u, sizeof *nm);
    if (!nm) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    m->members = nm;
    mi = m->nmembers++;
    m->members[mi] = (ccord_member){
      .guild_id = guild_id, .user_id = user_id, .role_name = "Members", .role_color = 0u, .role_rank = 2u,
    };
    m->snap.members = m->members;
    m->snap.member_count = m->nmembers;
    for (size_t i = 0; i < sizeof m->mrows / sizeof m->mrows[0]; i++) {
      if (!m->mrows[i].present) {
        m->mrows[i] = (mock_mrow){.guild_id = guild_id, .user_id = user_id, .present = true};
        mock_sync_member_roles(m);
        break;
      }
    }
  }
  return admin_emit_member(m, mi, err);
}

// ---- Report flow: canned menus (shape of GET /reporting/menu/{type}), validated submit, recorded request -------------
#define MOCK_MENU_TAIL_SPAM \
  "\"2\":{\"id\":2,\"key\":\"SPAM\",\"header\":\"What kind of spam?\",\"children\":[],\"button\":{\"type\":\"next\",\"target\":4}," \
  "\"elements\":[{\"name\":\"spam_kind\",\"type\":\"checkbox\",\"should_submit_data\":true," \
  "\"data\":[[\"phishing\",\"Phishing or scams\"],[\"bot\",\"A spam bot\"]]}],\"is_multi_select_required\":true},"
#define MOCK_MENU_DONE_FAIL \
  "\"90\":{\"id\":90,\"key\":\"SUCCESS\",\"header\":\"Thanks for reporting\",\"children\":[],\"button\":{\"type\":\"done\",\"target\":null}," \
  "\"elements\":[{\"name\":\"success\",\"type\":\"success\",\"data\":null},{\"name\":\"block_users\",\"type\":\"block_users\",\"data\":null}]}," \
  "\"91\":{\"id\":91,\"key\":\"FAIL\",\"header\":\"Something went wrong\",\"children\":[],\"button\":{\"type\":\"cancel\",\"target\":null}," \
  "\"elements\":[{\"name\":\"fail\",\"type\":\"fail\",\"data\":null}]}}}"
#define MOCK_MENU_MESSAGE(EXTRA_CHILD) \
  "{\"name\":\"message\",\"variant\":\"3\",\"version\":\"1.0\",\"language\":\"en\",\"postback_url\":\"/api/reporting/message\"," \
  "\"root_node_id\":1,\"success_node_id\":90,\"fail_node_id\":91,\"nodes\":{" \
  "\"1\":{\"id\":1,\"key\":\"START\",\"header\":\"What's wrong with this message?\",\"children\":[[\"It's spam\",2],[\"It's abusive\",3]," \
  "[\"Something else\",5]" EXTRA_CHILD "],\"button\":null,\"elements\":[]}," \
  MOCK_MENU_TAIL_SPAM \
  "\"3\":{\"id\":3,\"key\":\"ABUSE\",\"header\":\"How is it abusive?\",\"children\":[[\"Harassment\",4]],\"button\":null,\"elements\":[]}," \
  "\"4\":{\"id\":4,\"key\":\"SUMMARY\",\"header\":\"Report summary\",\"children\":[],\"button\":{\"type\":\"submit\",\"target\":null}," \
  "\"report_type\":\"message\",\"elements\":[{\"name\":\"breadcrumbs\",\"type\":\"breadcrumbs\",\"data\":null}," \
  "{\"name\":\"message_preview\",\"type\":\"message_preview\",\"data\":null}," \
  "{\"name\":\"details\",\"type\":\"free_text\",\"should_submit_data\":true,\"data\":{\"placeholder\":\"Anything else?\",\"character_limit\":200}}]}," \
  "\"5\":{\"id\":5,\"key\":\"OTHER\",\"header\":\"Tell us more\",\"children\":[],\"button\":{\"type\":\"submit\",\"target\":null}," \
  "\"report_type\":\"message\",\"elements\":[{\"name\":\"details\",\"type\":\"free_text\",\"should_submit_data\":true,\"data\":{\"rows\":4}}]}," \
  MOCK_MENU_DONE_FAIL
static const char k_mock_report_menu_message[] = MOCK_MENU_MESSAGE("");
static const char k_mock_report_menu_message_broken[] = MOCK_MENU_MESSAGE(",[\"Broken link\",999]");
static const char k_mock_report_menu_user[] =
  "{\"name\":\"user\",\"variant\":\"3\",\"version\":\"1.0\",\"language\":\"en\",\"postback_url\":\"/api/reporting/user\"," \
  "\"root_node_id\":10,\"success_node_id\":90,\"fail_node_id\":91,\"nodes\":{" \
  "\"10\":{\"id\":10,\"key\":\"START\",\"header\":\"What's wrong with this user?\",\"children\":[[\"Impersonation\",11],[\"Harassment\",11]]," \
  "\"button\":null,\"elements\":[]}," \
  "\"11\":{\"id\":11,\"key\":\"SUMMARY\",\"header\":\"Report summary\",\"children\":[],\"button\":{\"type\":\"submit\",\"target\":null}," \
  "\"report_type\":\"user\",\"elements\":[{\"name\":\"breadcrumbs\",\"type\":\"breadcrumbs\",\"data\":null}," \
  "{\"name\":\"user_preview\",\"type\":\"user_preview\",\"data\":null}," \
  "{\"name\":\"details\",\"type\":\"free_text\",\"should_submit_data\":true,\"data\":{\"character_limit\":200}}]}," \
  MOCK_MENU_DONE_FAIL;

typedef struct mock_report {
  ccord_arena arena;           // owns every string and array of req
  ccord_report_request req;
} mock_report;

static void mock_report_free(mock_report *r)
{
  if (!r) return;
  ccord_arena_free(&r->arena);
  ccord_free(r);
}

static const char *mock_report_menu_json(const mock *m, ccord_report_type type)
{
  if (type == CCORD_REPORT_USER) return k_mock_report_menu_user;
  return m->report_broken_link ? k_mock_report_menu_message_broken : k_mock_report_menu_message;
}

static ccord_status mock_request_report_menu(ccord_transport *t, ccord_report_type type, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (type >= CCORD_REPORT_TYPE_COUNT) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad report type");
  m->report_menu_calls++;
  if (m->report_menu_fail[0]) {
    mock_item it = make_item(CCORD_NET_ACTION_FAILED, now_of(m) + m->cfg.latency_ms);
    it.ev.action = CCORD_ACTION_REPORT_MENU;
    it.ev.status = CCORD_ERR_NOT_FOUND;
    char *why = ccord_strndup(m->report_menu_fail, strlen(m->report_menu_fail));   // a copy: the hook may change before the poll
    if (!why) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    retire_content(m, why);
    it.ev.text = why;
    return push_item(m, it, err);
  }
  const char *json = mock_report_menu_json(m, type);
  char *copy = ccord_strndup(json, strlen(json));
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  retire_content(m, copy);
  mock_item it = make_item(CCORD_NET_REPORT_MENU, now_of(m) + m->cfg.latency_ms);
  it.ev.status = CCORD_OK;
  it.ev.expr_arg = ccord_report_type_name(type);
  it.ev.expr_body = copy;
  it.ev.expr_body_len = strlen(copy);
  return push_item(m, it, err);
}

static bool report_is_edge(const ccord_report_node *n, uint32_t to)
{
  for (size_t i = 0; i < n->child_count; i++)
    if (n->children[i].target == to) return true;
  return n->has_button_target && n->button_target == to;
}

static bool report_value_ok(const ccord_report_element *el, const char *v)
{
  if (el->kind != CCORD_REPORT_EL_CHECKBOX && el->kind != CCORD_REPORT_EL_DROPDOWN) return v != nullptr;
  for (size_t i = 0; v && i < el->choice_count; i++)
    if (strcmp(el->choices[i].value, v) == 0) return true;
  return false;
}

// What the server would check: the path follows the menu edges to a submit node, and every value belongs to a submitting element on it.
static bool report_request_valid(const ccord_report_menu *menu, const ccord_report_request *r)
{
  if (!r->breadcrumbs || r->breadcrumb_count == 0 || r->breadcrumb_count > CCORD_REPORT_MAX_PATH) return false;
  if (r->breadcrumbs[0] != menu->root) return false;
  for (size_t i = 0; i < r->breadcrumb_count; i++) {
    const ccord_report_node *n = ccord_report_menu_node(menu, r->breadcrumbs[i]);
    if (!n) return false;
    if (i > 0 && !report_is_edge(ccord_report_menu_node(menu, r->breadcrumbs[i - 1]), r->breadcrumbs[i])) return false;
  }
  if (ccord_report_menu_node(menu, r->breadcrumbs[r->breadcrumb_count - 1])->button != CCORD_REPORT_BTN_SUBMIT) return false;
  if (r->element_count && !r->elements) return false;
  for (size_t e = 0; e < r->element_count; e++) {
    const ccord_report_value *v = &r->elements[e];
    if (!v->name || (v->value_count && !v->values)) return false;
    const ccord_report_element *found = nullptr;
    for (size_t i = 0; i < r->breadcrumb_count && !found; i++) {
      const ccord_report_node *n = ccord_report_menu_node(menu, r->breadcrumbs[i]);
      for (size_t k = 0; k < n->element_count; k++)
        if (n->elements[k].should_submit && n->elements[k].name && strcmp(n->elements[k].name, v->name) == 0) {
          found = &n->elements[k];
          break;
        }
    }
    if (!found) return false;
    for (size_t k = 0; k < v->value_count; k++)
      if (!report_value_ok(found, v->values[k])) return false;
  }
  return true;
}

static mock_report *mock_report_copy(const ccord_report_request *src)
{
  mock_report *r = ccord_calloc(1, sizeof *r);
  if (!r) return nullptr;
  ccord_arena_init(&r->arena, 0);
  ccord_arena *a = &r->arena;
  bool ok = true;
#define MR_STR(s) ((s) ? (ok = ok && (tmp = ccord_arena_strndup(a, (s), strlen(s))) != nullptr, tmp) : nullptr)
  char *tmp = nullptr;
  r->req = *src;
  r->req.version = MR_STR(src->version);
  r->req.variant = MR_STR(src->variant);
  r->req.language = MR_STR(src->language);
  uint32_t *bc = ccord_arena_alloc(a, src->breadcrumb_count * sizeof *bc + 1, alignof(uint32_t));
  ccord_report_value *els = ccord_arena_alloc(a, src->element_count * sizeof *els + 1, alignof(ccord_report_value));
  ok = ok && bc && els;
  if (ok) {
    memcpy(bc, src->breadcrumbs, src->breadcrumb_count * sizeof *bc);
    r->req.breadcrumbs = bc;
    for (size_t i = 0; ok && i < src->element_count; i++) {
      els[i] = src->elements[i];
      els[i].name = MR_STR(src->elements[i].name);
      const char **vals = ccord_arena_alloc(a, src->elements[i].value_count * sizeof *vals + 1, alignof(char *));
      ok = ok && vals;
      for (size_t k = 0; ok && k < src->elements[i].value_count; k++) vals[k] = MR_STR(src->elements[i].values[k]);
      els[i].values = vals;
    }
    r->req.elements = els;
  }
#undef MR_STR
  if (!ok) {
    mock_report_free(r);
    return nullptr;
  }
  return r;
}

static ccord_status mock_submit_report(ccord_transport *t, const ccord_report_request *req, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!req || req->type >= CCORD_REPORT_TYPE_COUNT || !req->version || !req->variant)
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad report request");
  if (req->type == CCORD_REPORT_MESSAGE ? (!req->channel_id || !req->message_id) : !req->reported_user_id)
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: report target missing");
  const char *json = mock_report_menu_json(m, req->type);
  ccord_report_menu menu;
  ccord_status st = ccord_report_menu_parse(&menu, json, strlen(json), err);
  if (st != CCORD_OK) return st;
  bool valid = report_request_valid(&menu, req);
  ccord_report_menu_free(&menu);
  if (!valid) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: report path does not follow the menu");

  if (m->late_report_fail[0]) {   // accepted by the call, refused later like a Discord 4xx; nothing is recorded
    mock_item it = make_item(CCORD_NET_ACTION_FAILED, now_of(m) + m->cfg.latency_ms);
    it.ev.action = CCORD_ACTION_REPORT;
    it.ev.channel_id = req->channel_id;
    it.ev.message_id = req->message_id;
    it.ev.user_id = req->reported_user_id;
    it.ev.status = CCORD_ERR_PERMISSION;
    char *why = ccord_strndup(m->late_report_fail, strlen(m->late_report_fail));   // a copy: the hook may change before the poll
    if (!why) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    retire_content(m, why);
    it.ev.text = why;
    return push_item(m, it, err);
  }
  mock_report *copy = mock_report_copy(req);
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  mock_item it = make_item(CCORD_NET_REPORT_SUBMITTED, now_of(m) + m->cfg.latency_ms);
  it.ev.status = CCORD_OK;
  it.ev.expr_arg = ccord_report_type_name(req->type);
  it.ev.report_id = 700000000000000001ull + m->report_submits;
  it.ev.channel_id = req->channel_id;
  it.ev.message_id = req->message_id;
  it.ev.user_id = req->reported_user_id;
  st = push_item(m, it, err);
  if (st != CCORD_OK) {
    mock_report_free(copy);
    return st;
  }
  mock_report_free(m->last_report);
  m->last_report = copy;
  m->report_submits++;
  return CCORD_OK;
}

void ccord_mock_set_report_menu_failure(ccord_transport *t, const char *reason)
{
  if (!t) return;
  (void)snprintf(((mock *)t)->report_menu_fail, sizeof ((mock *)t)->report_menu_fail, "%s", reason ? reason : "");
}
void ccord_mock_set_late_report_failure(ccord_transport *t, const char *reason)
{
  if (!t) return;
  (void)snprintf(((mock *)t)->late_report_fail, sizeof ((mock *)t)->late_report_fail, "%s", reason ? reason : "");
}
void ccord_mock_set_report_broken_link(ccord_transport *t, bool on)
{
  if (t) ((mock *)t)->report_broken_link = on;
}
size_t ccord_mock_report_menu_count(const ccord_transport *t) { return t ? ((const mock *)t)->report_menu_calls : 0; }
size_t ccord_mock_report_submit_count(const ccord_transport *t) { return t ? ((const mock *)t)->report_submits : 0; }
const ccord_report_request *ccord_mock_last_report(const ccord_transport *t)
{
  const mock *m = (const mock *)t;
  return m && m->last_report ? &m->last_report->req : nullptr;
}

static void mock_destroy(ccord_transport *t)
{
  mock *m = (mock *)t;
  free_items(m->queue, m->nq);
  ccord_free(m->queue);
  if (m->have_current) item_release(&m->current);
  clear_last_interaction_values(m);
  mock_stream_media_clear(m);
  mock_cam_clear(m);
  ccord_free(m->last_admin);
  mock_report_free(m->last_report);
  ccord_free(m->rich_json);
  upload_cache_free(m);
  dataset_free(m);
  ccord_free(m);
}

static const ccord_transport_vtable mock_vtable = {
  .name = "mock",
  .connect = mock_connect,
  .disconnect = mock_disconnect,
  .request_history = mock_request_history,
  .send_message = mock_send_message,
  .send_message_with_nonce = mock_send_message_with_nonce,
  .poll = mock_poll,
  .destroy = mock_destroy,
  .edit_message = mock_edit_message,
  .delete_message = mock_delete_message,
  .send_typing = mock_send_typing,
  .toggle_reaction = mock_toggle_reaction,
  .ack_message = mock_ack_message,
  .select_guild = mock_select_guild,
  .join_voice = mock_join_voice,
  .leave_voice = mock_leave_voice,
  .update_voice_state = mock_update_voice_state,
  .start_stream = mock_start_stream,
  .stop_stream = mock_stop_stream,
  .send_stream_frame = mock_send_stream_frame,
  .poll_stream_frame = mock_poll_stream_frame,
  .camera_senders = mock_camera_senders,
  .poll_camera_frame = mock_poll_camera_frame,
  .send_stream_pcm = mock_send_stream_pcm,
  .poll_stream_pcm = mock_poll_stream_pcm,
  .send_voice_video = mock_send_voice_video,
  .consume_keyframe_request = mock_consume_keyframe_request,
  .watch_stream = mock_watch_stream,
  .stop_watching = mock_stop_watching,
  .set_stream_paused = mock_set_stream_paused,
  .send_reply = mock_send_reply,
  .toggle_pin = mock_toggle_pin,
  .request_history_before = mock_request_history_before,
  .request_pins = mock_request_pins,
  .set_presence = mock_set_presence,
  .admin_action = mock_admin_action,
  .set_voice_camera = mock_set_voice_camera,
  .send_message_with_file = mock_send_message_with_file,
  .fetch_media = mock_fetch_media,
  .fetch_media_range = mock_fetch_media_range,
  .search_messages = mock_search_messages,
  .open_dm = mock_open_dm,
  .push_voice_pcm = mock_push_voice_pcm,
  .pull_voice_pcm = mock_pull_voice_pcm,
  .set_relationship = mock_set_relationship,
  .request_profile = mock_request_profile,
  .select_channel = mock_select_channel,
  .request_members = mock_request_members,
  .request_expression_data = mock_request_expression_data,
  .send_sticker = mock_send_sticker,
  .request_forum_posts = mock_request_forum_posts,
  .create_forum_post = mock_create_forum_post,
  .vote_poll = mock_vote_poll,
  .send_interaction = mock_send_interaction,
  .send_reply_ex = mock_send_reply_ex,
  .send_message_with_files = mock_send_message_with_files,
  .create_poll = mock_create_poll,
  .request_slash_commands = mock_request_slash_commands,
  .search_messages_ex = mock_search_messages_ex,
  .request_pins_before = mock_request_pins_before,
  .request_reaction_users = mock_request_reaction_users,
  .request_discoverable_guilds = mock_request_discoverable_guilds,
  .request_profile_ex = mock_request_profile_ex,
  .set_presence_ex = mock_set_presence_ex,
  .call_action = mock_call_action,
  .play_soundboard = mock_play_soundboard,
  .set_user_volume = mock_set_user_volume,
  .set_voice_config = mock_set_voice_config,
  .send_tts_message = mock_send_tts_message,
  .forward_message = mock_forward_message,
  .crosspost_message = mock_crosspost_message,
  .mark_unread = mock_mark_unread,
  .patch_settings_proto = mock_patch_settings_proto,
  .set_status_proto = mock_set_status_proto,
  .set_rich_activities = mock_set_rich_activities,
  .request_report_menu = mock_request_report_menu,
  .submit_report = mock_submit_report,
  .network_event = mock_network_event,
  .active_uploads = mock_active_uploads,
};

ccord_transport *ccord_mock_transport_create(const ccord_mock_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  mock *m = ccord_calloc(1, sizeof *m);
  if (!m) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    return nullptr;
  }
  m->base.vt = &mock_vtable;
  if (cfg) {
    m->cfg = *cfg;
  }
  m->cfg.seed = cfg && cfg->seed ? cfg->seed : MOCK_DEFAULT_SEED;
  m->cfg.big_channel_messages = cfg && cfg->big_channel_messages ? cfg->big_channel_messages : MOCK_DEFAULT_BIG;
  m->cfg.base_time_ms = cfg && cfg->base_time_ms ? cfg->base_time_ms : MOCK_DEFAULT_BASE_MS;
  m->rng = m->cfg.seed;
  if (build_dataset(m, err) != CCORD_OK) {
    mock_destroy(&m->base);
    return nullptr;
  }
  m->nemojis = sizeof k_seed_emojis / sizeof k_seed_emojis[0];
  memcpy(m->emojis, k_seed_emojis, sizeof k_seed_emojis);
  m->nstickers = sizeof k_seed_stickers / sizeof k_seed_stickers[0];
  memcpy(m->stickers, k_seed_stickers, sizeof k_seed_stickers);
  m->nsoundboard = sizeof k_seed_soundboard / sizeof k_seed_soundboard[0];
  memcpy(m->soundboard, k_seed_soundboard, sizeof k_seed_soundboard);
  m->start_ms = now_of(m);
  ccord_gwpace_init(&m->link.pace, m->cfg.seed ^ 0x6C696E6Bull);
  return &m->base;
}
