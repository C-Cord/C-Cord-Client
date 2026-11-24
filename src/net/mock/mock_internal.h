// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Internal state and cross-TU prototypes for src/net/mock/mock_*.c.
#pragma once

#include <ccord/model/permissions.h>
#include <ccord/model/rich.h>
#include <ccord/model/search.h>
#include <ccord/net/mock_transport.h>
#include <ccord/net/voice_reconnect.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/base64.h>
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
#define MOCK_VIDEO_RESOLVE_MAX 8u      // entries registered through ccord_mock_serve_video_resolve

typedef struct mock_video_resolve_slot {
  char url_part[MOCK_RANGE_URL_PART_MAX];
  char video_url[2048];
  char audio_url[2048];
  char error[192];
  uint32_t duration_ms;
  ccord_status status;
  bool missing_helper;
} mock_video_resolve_slot;

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
  ccord_account_private *account_private_own; // owned; ev.account_private points here
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

typedef struct mock_report mock_report;
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
  mock_item *queue; size_t qhead, nq, capq;
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
  mock_video_resolve_slot video_resolves[MOCK_VIDEO_RESOLVE_MAX]; size_t nvideo_resolves;
  mock_link link;                          // gateway link model (ccord_mock_drop_gateway / network_event)
} mock;

#define MOCK_MV_INVITE_CODE "mv-test"
#define MOCK_MV_GUILD_ID 9101u
#define MOCK_MV_GUILD_NAME "Rules Lounge"
#define k_missing_permissions "Missing Permissions"

extern const char *const mk_user_names[MOCK_NUM_USERS];

// Cross-TU internal helpers
void mk_item_release(mock_item *it);
uint64_t mk_now_of(const mock *m);
void mk_free_content(const char **p);
void mk_free_rich(const ccord_message_rich **p);
void mk_retire_content(mock *m, const char *content);
void mk_retire_rich(mock *m, const ccord_message_rich *rich);
ccord_status mk_chan_append(mock *m, size_t ci, ccord_message msg, ccord_error *err);
void mock_sync_member_roles(mock *m);
void mock_audit_push_change(ccord_audit_entry *e, const char *key, const char *old_v, const char *new_v);
void mock_audit_fill_changes(ccord_audit_entry *e, const char *detail);
ccord_status mk_build_dataset(mock *m, ccord_error *err);
size_t mk_qidx(const mock *m, size_t logical);
ccord_status mk_push_item(mock *m, const mock_item *item, ccord_error *err);
mock_item mk_make_item(ccord_net_event_type type, uint64_t due);
size_t mk_channel_index(const mock *m, ccord_id channel_id, bool text_only, ccord_status *st, ccord_error *err);
ccord_message mk_store_new_rich(mock *m, size_t ci, ccord_id author, const char *text, size_t len, const ccord_message_rich *rich, ccord_error *err, ccord_status *st);
ccord_message mk_store_new(mock *m, size_t ci, ccord_id author, const char *text, size_t len, ccord_error *err, ccord_status *st);
ccord_status mk_queue_message_create(mock *m, ccord_message msg, uint64_t due, ccord_error *err);
ccord_status mk_queue_message_update(mock *m, ccord_message msg, uint64_t due, ccord_error *err);
int64_t mk_message_time(const mock *m);
long mk_find_message(const mock *m, size_t ci, ccord_id message_id);
bool mock_guild_known(const mock *m, ccord_id guild_id);
long mock_voice_index(const mock *m, ccord_id guild_id, ccord_id user_id);
ccord_status mk_queue_activity_instance(mock *m, const ccord_activity_instance *inst, uint64_t due, ccord_error *err);
ccord_status mk_queue_voice_state(mock *m, const ccord_voice_state *v, uint64_t due, ccord_error *err);
void mock_cam_clear(mock *m);
size_t mock_camera_senders(ccord_transport *t, ccord_camera_rx *out, size_t max);
bool mock_poll_camera_frame(ccord_transport *t, ccord_id user_id, uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t *out_ts90k, bool *out_keyframe);
void mock_stream_media_clear(mock *m);
ccord_status mock_send_stream_frame(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k, bool keyframe, ccord_error *err);
bool mock_poll_stream_frame(ccord_transport *t, uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t *out_ts90k, bool *out_keyframe);
ccord_status mock_send_stream_pcm(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err);
size_t mock_poll_stream_pcm(ccord_transport *t, int16_t *out_pcm, size_t cap_samples);
long mock_stage_index(const mock *m, ccord_id channel_id);
ccord_status mk_queue_stage_instance(mock *m, const ccord_stage_instance *stage, bool deleted, uint64_t due, ccord_error *err);
ccord_status mock_stage_ensure(mock *m, ccord_id guild_id, ccord_id channel_id, uint64_t due, ccord_error *err);
ccord_status mock_stage_cleanup(mock *m, ccord_id channel_id, uint64_t due, ccord_error *err);
ccord_status mk_queue_voice_status(mock *m, ccord_id guild_id, ccord_id channel_id, const char *status, uint64_t due, ccord_error *err);
ccord_status mk_queue_stream_signal_ex(mock *m, ccord_id guild_id, ccord_id channel_id, ccord_id user_id, ccord_stream_signal_kind kind, uint32_t viewer_count, bool paused, const char *reason, ccord_error *err);
ccord_status mock_request_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err);
ccord_status mk_queue_action_refused(mock *m, ccord_action_kind action, ccord_id channel_id, ccord_id message_id, const char *utf8, size_t len, const char *nonce, size_t nonce_len, ccord_error *err);
ccord_status mock_send_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len, ccord_error *err);
ccord_status mock_send_message_with_nonce(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len, const char *nonce, size_t nonce_len, ccord_error *err);
ccord_status mock_edit_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, const char *utf8, size_t len, ccord_error *err);
ccord_status mock_delete_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err);
ccord_status mock_send_typing(ccord_transport *t, ccord_id channel_id, ccord_error *err);
ccord_status mock_toggle_reaction(ccord_transport *t, ccord_id channel_id, ccord_id message_id, const char *emoji, size_t len, bool add, ccord_error *err);
ccord_status mock_request_reaction_users(ccord_transport *t, ccord_id channel_id, ccord_id message_id, const char *emoji, size_t emoji_len, uint32_t limit, ccord_error *err);
ccord_status mock_ack_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err);
ccord_status mock_join_voice(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err);
ccord_status mock_leave_voice(ccord_transport *t, ccord_id guild_id, ccord_error *err);
ccord_status mock_update_voice_state(ccord_transport *t, ccord_id guild_id, bool self_mute, bool self_deaf, ccord_error *err);
ccord_status mock_start_stream(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, const ccord_stream_config *config, ccord_error *err);
ccord_status mock_stop_stream(ccord_transport *t, ccord_id guild_id, ccord_error *err);
ccord_status mock_watch_stream(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id, ccord_error *err);
ccord_status mock_stop_watching(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id, ccord_error *err);
ccord_status mock_set_stream_paused(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id, bool paused, ccord_error *err);
ccord_status mock_send_reply_ex(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id, bool mention_author, const char *utf8, size_t len, ccord_error *err);
ccord_status mock_send_reply(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id, const char *utf8, size_t len, ccord_error *err);
ccord_status mock_send_tts_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len, ccord_error *err);
ccord_status mock_forward_message(ccord_transport *t, ccord_id channel_id, ccord_id source_channel_id, ccord_id message_id, ccord_error *err);
ccord_status mock_crosspost_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err);
ccord_status mock_mark_unread(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err);
ccord_status mock_toggle_pin(ccord_transport *t, ccord_id channel_id, ccord_id message_id, bool pinned, ccord_error *err);
ccord_status mock_request_history_before(ccord_transport *t, ccord_id channel_id, ccord_id before_id, uint32_t limit, ccord_error *err);
ccord_status mock_request_pins_before(ccord_transport *t, ccord_id channel_id, const char *before_cursor, uint32_t limit, ccord_error *err);
ccord_status mock_request_pins(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err);
ccord_status mock_set_presence_ex(ccord_transport *t, ccord_presence presence, const char *emoji_name, ccord_id emoji_id, const char *status_text, ccord_error *err);
ccord_status mock_set_presence(ccord_transport *t, ccord_presence presence, const char *status_text, ccord_error *err);
ccord_status mock_set_rich_activities(ccord_transport *t, const char *json, size_t len, ccord_error *err);
ccord_status mock_set_voice_camera(ccord_transport *t, ccord_id guild_id, bool enabled, ccord_error *err);
ccord_status mock_send_voice_video(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k, bool keyframe, ccord_error *err);
bool mock_consume_keyframe_request(ccord_transport *t);
bool mk_has_ci_substr(const char *haystack, const char *needle, size_t needle_len);
void mk_upload_cache_free(mock *m);
const mock_upload *mk_upload_cache_find(const mock *m, const char *url);
ccord_status mock_send_message_with_files(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id, bool mention_author, const char *utf8, size_t len, const ccord_upload_file *files, size_t file_count, ccord_error *err);
size_t mock_active_uploads(ccord_transport *t, ccord_upload_progress *out, size_t max);
ccord_status mock_send_message_with_file(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len, const char *filename, const void *file_data, size_t file_size, ccord_error *err);
ccord_status mock_create_poll(ccord_transport *t, ccord_id channel_id, const ccord_poll_create *poll, ccord_error *err);
ccord_status mock_request_slash_commands(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err);
ccord_status mock_fetch_media(ccord_transport *t, const char *url, ccord_error *err);
ccord_status mock_fetch_media_range(ccord_transport *t, const ccord_media_range_req *req, ccord_error *err);
ccord_status mock_resolve_video(ccord_transport *t, uint64_t req_id, const char *url, ccord_error *err);
ccord_status mock_search_messages_ex(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, const ccord_search_query *query, ccord_error *err);
ccord_status mock_search_messages(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, const char *query, size_t query_len, ccord_error *err);
ccord_status mock_open_dm(ccord_transport *t, ccord_id recipient_id, ccord_error *err);
ccord_status mock_push_voice_pcm(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err);
size_t mock_pull_voice_pcm(ccord_transport *t, int16_t *pcm, size_t max_samples);
ccord_status mock_set_relationship(ccord_transport *t, ccord_id user_id, ccord_relationship_kind kind, ccord_error *err);
ccord_status mock_request_profile_ex(ccord_transport *t, ccord_id user_id, ccord_id guild_id, ccord_error *err);
ccord_status mock_request_profile(ccord_transport *t, ccord_id user_id, ccord_error *err);
ccord_status mock_request_expression_data(ccord_transport *t, ccord_expr_kind kind, const char *arg, size_t arg_len, ccord_error *err);
ccord_status mock_patch_settings_proto(ccord_transport *t, const uint8_t *proto, size_t proto_len, ccord_error *err);
ccord_status mock_set_status_proto(ccord_transport *t, const ccord_proto_status *st, ccord_error *err);
ccord_status mock_send_sticker(ccord_transport *t, ccord_id channel_id, ccord_id sticker_id, ccord_error *err);
ccord_status mock_send_greet(ccord_transport *t, ccord_id channel_id, ccord_id sticker_id, ccord_id reply_to_id, ccord_error *err);
size_t mock_find_forum_meta(const mock *m, ccord_id forum_id);
ccord_status mock_request_forum_posts(ccord_transport *t, const ccord_forum_query *query, ccord_error *err);
ccord_status mock_create_forum_post(ccord_transport *t, ccord_id forum_id, const char *title, size_t title_len, const char *content, size_t content_len, const ccord_id *tag_ids, size_t tag_count, ccord_error *err);
ccord_status mk_admin_reply_failed(mock *m, const ccord_admin_request *req, ccord_status st, const char *text, ccord_error *err);
const char *mock_user_name(const mock *m, ccord_id id);
const char *mk_admin_owned(mock *m, const char *s);
size_t mk_admin_channel_index(const mock *m, ccord_id id);
long mk_admin_member_index(const mock *m, ccord_id guild_id, ccord_id user_id);
size_t mock_find_overwrites(const mock *m, ccord_id channel_id);
size_t mock_ensure_overwrites(mock *m, ccord_id channel_id);
size_t mock_ensure_forum_meta(mock *m, ccord_id forum_id);
ccord_status mk_admin_emit_channel_op(mock *m, size_t ci, ccord_admin_op op, ccord_error *err);
ccord_status mk_admin_emit_channel(mock *m, size_t ci, ccord_error *err);
ccord_status mk_admin_emit_member(mock *m, size_t mi, ccord_error *err);
size_t mock_role_find(const mock *m, ccord_id guild, ccord_id role);
size_t mock_guild_roles(const mock *m, ccord_id guild, size_t *count);
mock_mrow *mock_mrow_find(mock *m, ccord_id guild, ccord_id user);
ccord_id mock_guild_owner(const mock *m, ccord_id guild);
uint64_t mock_self_perms(mock *m, ccord_id guild);
bool mock_may(mock *m, ccord_id guild, uint64_t need);
uint64_t mock_channel_perms(mock *m, ccord_id guild, ccord_id channel_id);
bool mock_may_edit_role(mock *m, ccord_id guild, ccord_id role, bool assign);
void mock_refresh_display(mock *m, ccord_id guild, ccord_id user);
ccord_status mock_audit_add(mock *m, ccord_id guild, uint16_t action, ccord_id target, const char *target_name, const char *reason, const char *detail);
ccord_status mk_admin_emit_roles(mock *m, ccord_net_event_type type, const ccord_id *ids, size_t n, ccord_id guild, ccord_error *err);
ccord_status mk_admin_emit_member_roles(mock *m, ccord_id guild, ccord_id user, ccord_id role, bool added, ccord_error *err);
ccord_status mk_admin_emit_bans(mock *m, ccord_id guild, ccord_error *err);
ccord_status mk_admin_emit_invites(mock *m, ccord_net_event_type type, ccord_id guild, const char *code, ccord_error *err);
ccord_status mk_admin_emit_audit(mock *m, const ccord_admin_request *req, ccord_error *err);
size_t mk_admin_add_channel(mock *m, const ccord_channel *ch, ccord_error *err);
void mk_admin_remove_channel(mock *m, size_t ci);
void mock_remove_guild(mock *m, size_t gi);
size_t mock_guild_index(const mock *m, ccord_id gid);
ccord_status mock_emit_guild_upsert(mock *m, size_t gi, ccord_admin_op op, ccord_error *err);
bool mock_parse_automod_body(mock *m, const char *body, bool is_create, ccord_automod_rule *io);
const char *mock_arena_dup_cstr(ccord_arena *a, const char *s, size_t len);
bool mock_parse_welcome_body(mock *m, ccord_id g, const char *body, ccord_welcome_screen *out_ws, ccord_welcome_channel out_chs[CCORD_WELCOME_MAX_CHANNELS]);
bool mock_parse_onboarding_body(mock *m, ccord_id g, const char *body, ccord_onboarding *out_ob);
bool mock_member_prunable(mock *m, ccord_id g, size_t mi, uint8_t days, const ccord_id *inc_roles, size_t ninc);
bool mk_admin_m5_voice_status(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err);
bool mk_admin_m5_stage_request_speak(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err);
bool mk_admin_m5_stage_set_speaker(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err);
bool mk_admin_m5_stage_topic(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err);
bool mk_admin_m5_stage_create(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err);
bool mk_admin_m5_stage_delete(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err);
bool mk_admin_scheduled_events(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err);
ccord_status mk_admin_m6_emit_settings(mock *m, ccord_error *err);
ccord_status mock_call_action(ccord_transport *t, ccord_id channel_id, ccord_call_action act, ccord_error *err);
ccord_status mock_play_soundboard(ccord_transport *t, ccord_id guild_id, ccord_id sound_id, ccord_error *err);
ccord_status mock_set_user_volume(ccord_transport *t, ccord_id user_id, uint8_t pct, ccord_error *err);
ccord_status mock_set_voice_config(ccord_transport *t, const ccord_voice_config *cfg, ccord_error *err);
bool mock_admin_server_guild(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err);
bool mock_admin_server(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err);
bool mock_str_eq_ci(const char *a, const char *b);
ccord_status mock_materialize_guild(mock *m, ccord_id gid_hint, const char *gname, bool self_owner, bool self_pending, ccord_admin_op admin_op, ccord_id *out_guild_id, ccord_id *out_channel_id, ccord_error *err);
ccord_status mock_request_discoverable_guilds(ccord_transport *t, const char *query, uint32_t category_id, uint32_t offset, uint32_t limit, ccord_error *err);
ccord_status mock_admin_action(ccord_transport *t, const ccord_admin_request *req, ccord_error *err);
void mk_clear_last_interaction_values(mock *m);
ccord_status mock_vote_poll(ccord_transport *t, ccord_id channel_id, ccord_id message_id, const uint32_t *answer_ids, size_t answer_count, ccord_error *err);
ccord_status mock_send_interaction(ccord_transport *t, const ccord_interaction_request *req, ccord_error *err);
void mock_report_free(mock_report *r);
ccord_status mock_request_report_menu(ccord_transport *t, ccord_report_type type, ccord_error *err);
ccord_status mock_submit_report(ccord_transport *t, const ccord_report_request *req, ccord_error *err);
