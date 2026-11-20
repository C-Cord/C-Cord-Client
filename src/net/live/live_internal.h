// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Internal state, REST op descriptor, and cross-TU prototypes for src/net/live/live_*.c.
#pragma once

#include <ccord/net/live_transport.h>
#include <ccord/net/discord_proto.h>
#include <ccord/net/gateway_pace.h>
#include <ccord/net/video_resolve.h>
#include <ccord/net/voice_audio.h>
#include <ccord/net/voice_camera.h>
#include <ccord/net/voice_dsp.h>
#include <ccord/net/voice_stream.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/alloc.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/base64.h>
#include <ccord/core/clock.h>
#include <ccord/core/idmap.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>

#if defined(__has_include) && __has_include(<opus/opus.h>)
#include <opus/opus.h>
#else
#include <opus.h>
#endif

#include "common/tls_internal.h"
#include "discord/rest_internal.h"

#define LIVE_QUEUE_SIZE 64u
#define LIVE_API_MAX 512u
#define LIVE_BODY_MAX (CCORD_SEND_MAX_BYTES * 6u + 64u)
#define LIVE_CALLS_MAX 16u
#define LIVE_SOUNDBOARD_MAX_SAMPLES (48000u * 5u)
#define LIVE_STREAM_PEERS_MAX 8u
// Last-known voice flags per participant, fed by gateway VOICE_STATE_UPDATE. Stage REST answers (204) carry no
// body, so the synthesized completion event must reuse these instead of zeroing mute/deaf/self_* it does not own.
#define LIVE_VOICE_CACHE_MAX 64u
// Go Live media plane (m5-golive-live): bounded viewer Annex-B FIFO and one second of 48 kHz mono stream-audio
// PCM, mirroring the mock transport's limits. The send hooks may run on an encoder/audio thread, so every media
// field (and the tx/rx vstream pointers they use) is guarded by live.media_mtx.
#define LIVE_STREAM_FRAME_SLOTS 8u
#define LIVE_STREAM_FRAME_MAX (256u * 1024u)
#define LIVE_STREAM_PCM_CAP_SAMPLES CCORD_VOICE_SAMPLE_RATE
#define LIVE_STREAM_OPUS_MAX 1500u

typedef struct live_user_vol {
  ccord_id user_id;
  uint8_t pct;
} live_user_vol;

#define LIVE_CHAN_MSG_MAX 512u

typedef struct live_chan_guild {
  ccord_id channel_id;
  ccord_id guild_id;
  ccord_id last_ack_id;
  ccord_id *msg_ids;
  size_t msg_count;
  size_t msg_cap;
} live_chan_guild;

typedef struct live_stream_meta {
  char stream_key[96];
  ccord_id rtc_server_id;
  ccord_id rtc_channel_id;
} live_stream_meta;

// One queued viewer access unit. `data` is a lazily allocated LIVE_STREAM_FRAME_MAX-byte slot reused across
// frames; len == 0 marks the slot empty.
typedef struct live_stream_frame {
  uint8_t *data;
  size_t len;
  uint32_t ts90k;
  bool keyframe;
} live_stream_frame;

typedef struct live_vstream_slot {
  ccord_vstream *vs;
  char stream_key[96];
  char token[CCORD_LIVE_TOKEN_MAX + 1];
  char endpoint[CCORD_VOICE_ENDPOINT_MAX];
  ccord_id rtc_server_id;
  ccord_id rtc_channel_id;
  ccord_id viewer_ids[128];
  size_t viewer_id_count;
  uint32_t width, height, fps, max_bitrate;
  uint64_t next_ping_ms;
  bool attempted;
  bool sender;
} live_vstream_slot;

// Voice reconnect ladder. Idle unless a voice gateway was lost.
enum {
  LV_IDLE,           // nothing pending (a gateway exists, or none is wanted)
  LV_WAIT_ATTEMPT,   // backoff timer running; vlink_plan says what runs when it fires (REIDENTIFY or REJOIN)
  LV_WAIT_SERVER,    // REJOIN sent; waiting for the new VOICE_STATE_UPDATE + VOICE_SERVER_UPDATE
  LV_AWAIT_MAIN,     // 4014 / 4022: the main gateway decides; give up locally when the grace runs out
  LV_WAIT_MAIN_GW    // REJOIN due but the main gateway is down, so OP 4 cannot be sent yet
};

typedef struct live {
  ccord_transport base;
  char token[CCORD_LIVE_TOKEN_MAX + 1];
  char api_base[LIVE_API_MAX];
  char cache_dir[1024];       // per-account cache dir (<data>/accounts/<fp>); empty = legacy shared API cache, no warm history
  ccord_gateway *gw;
  char *rich_json;            // Rich Presence activities mirror (the gateway owns the copy it splices into op 3)
  size_t rich_len;
  // Status through settings-proto/1: the account's current type-1 bytes (READY's proto, a gateway update, or a GET),
  // the status write waiting for them, and how many requests are in flight.
  uint8_t *status_base;
  size_t status_base_len;
  bool status_base_ok;
  ccord_proto_status status_want;
  bool status_want_ok;
  bool status_get_inflight;
  unsigned status_patches_inflight;
  ccord_voice_gateway *voice;
  char gw_session_id[128];
  char voice_session_id[128];
  char voice_token[CCORD_LIVE_TOKEN_MAX + 1];
  char voice_endpoint[CCORD_VOICE_ENDPOINT_MAX];
  ccord_id voice_server_guild, voice_server_channel;
  bool voice_server_valid;
  bool voice_state_valid;
  bool voice_attempted;
  ccord_vc vc;                  // reconnect budget of the current outage (adopted from the failing gateway)
  uint8_t vlink;                // LV_*
  uint64_t vlink_at_ms;         // when the pending LV_* step fires
  ccord_vc_plan vlink_plan;
  bool voice_swallow_leave;     // our own rejoin's leave echo (VOICE_STATE_UPDATE channel 0) must not reach the UI
  bool vlink_was_down;          // an outage was reported (RECONNECTING); CONNECTED is emitted when it ends
  bool vlink_gw_reported;       // the gateway's own RESUME (state RECONNECTING) was reported for this outage step
  bool vlink_rewatch;           // a REJOIN ended the voice session: re-send the stream watch once ESTABLISHED
  ccord_rest_config rest;
  ccord_rest_queue *restq;
  ccord_live_event pending[LIVE_QUEUE_SIZE];
  char *pending_media[LIVE_QUEUE_SIZE];  // optional heap-owned media payload per pending slot (zero-copy)
  size_t head, tail, count;
  ccord_live_event current;
  char *current_media;                   // optional heap-owned media payload for `current`
  bool have_current;
  ccord_connection_state state;
  ccord_connection_state reported;
  ccord_id self_id;
  ccord_id selected_guild;
  ccord_id selected_channel;
  ccord_id voice_guild, voice_channel;
  bool self_mute, self_deaf;
  bool self_video;
  bool voice_video_synced;
  bool streaming;
  bool self_suppressed;
  // PTT state is shared between the dedicated audio pthread (live_push_voice_pcm) and the UI thread
  // (live_poll / live_set_voice_config / live_voice_stop); every access must be atomic.
  _Atomic bool ptt_active;
  _Atomic uint64_t ptt_release_ms;
  _Atomic bool ptt_enabled;      // mirror of voice_cfg.ptt for the audio thread (voice_cfg itself is UI-owned)
  bool sb_speaking;
  uint64_t sb_speaking_until_ms;
  // Decoded soundboard PCM waiting to be fed into the 1 s voice-gateway ring in <= 20 ms chunks; clips up to 5 s
  // would otherwise be rejected with CCORD_ERR_RANGE by a single oversized push.
  int16_t *sb_pending_pcm;
  size_t sb_pending_samples;
  size_t sb_pending_offset;
  live_user_vol user_volume[CCORD_SPEAK_MAX];
  size_t user_volume_count;
  ccord_voice_config voice_cfg;
  bool has_voice_cfg;
  ccord_call_state dm_calls[LIVE_CALLS_MAX];
  size_t dm_call_count;
  ccord_voice_state voice_cache[LIVE_VOICE_CACHE_MAX];
  size_t voice_cache_count;
  live_chan_guild *chan_guilds;
  size_t chan_guild_count;
  size_t chan_guild_cap;
  ccord_idmap chan_guild_idx;   // channel_id -> slot in chan_guilds[]
  live_vstream_slot tx_stream;
  live_vstream_slot rx_stream;
  live_stream_meta stream_peers[LIVE_STREAM_PEERS_MAX];
  size_t stream_peer_count;
  ccord_id watched_guild, watched_channel, watched_user;
  bool watching_stream;
  bool stream_paused;
  bool tx_stream_paused;
  _Atomic bool tx_keyframe_requested;
  // Go Live media plane: media_mtx guards every field below plus the tx/rx slot `vs` pointers they send through.
  // on_frame/on_audio fire from ccord_vstream_pump_all() (UI thread); the send/poll hooks may run on an
  // encoder/audio thread, and teardown closes the vstream handles under the same lock so a send can never race
  // a close.
  ccord_mutex *media_mtx;
  ccord_mutex *voice_mtx;
  ccord_cond *voice_cond;
  int voice_inflight;
  live_stream_frame rx_frames[LIVE_STREAM_FRAME_SLOTS];
  size_t rx_frame_head, rx_frame_count;
  int16_t *rx_pcm;   // LIVE_STREAM_PCM_CAP_SAMPLES mono ring, lazily allocated on the first stream-audio packet
  size_t rx_pcm_head, rx_pcm_count;
  OpusEncoder *stream_enc;   // lazy 48 kHz mono encoder for send_stream_pcm
  OpusDecoder *stream_dec;   // lazy 48 kHz mono decoder for on_audio
  int16_t tx_pcm_acc[CCORD_VOICE_FRAME_SAMPLES];   // partial 20 ms chunk carried across send_stream_pcm calls
  size_t tx_pcm_acc_len;
  uint32_t tx_audio_ts48k;   // 48 kHz RTP timestamp of the next stream-audio chunk
  uint32_t interaction_seq;
  struct live_rest_op *upload_head, *upload_tail;
  uint64_t next_upload_id;
  ccord_video_resolver *vresolve;
  ccord_profile_effect_config *effect_configs;
  size_t effect_config_count;
  size_t effect_config_cap;
  bool effect_catalog_loaded;
  bool effect_catalog_inflight;
  ccord_profile_frame_config *frame_configs;
  size_t frame_config_count;
  size_t frame_config_cap;
  ccord_id product_queried[64];
  size_t product_queried_count;
  size_t product_inflight;
  struct {
    ccord_id user_id;
    char *json;
    size_t len;
  } pending_decor_profiles[8];
  size_t pending_decor_count;
  uint64_t (*now_ms)(void *ud);
  void *clock_ud;
} live;


typedef enum live_rest_kind {
  LIVE_REST_HISTORY = 0,
  LIVE_REST_HISTORY_BEFORE,
  LIVE_REST_PINS,
  LIVE_REST_PINS_BEFORE,
  LIVE_REST_ACK,
  LIVE_REST_SEARCH,
  LIVE_REST_OPEN_DM,
  LIVE_REST_MEDIA,
  LIVE_REST_UPLOAD_SLOT,
  LIVE_REST_UPLOAD_PUT,
  LIVE_REST_UPLOAD_SEND,
  LIVE_REST_RELATIONSHIP,
  LIVE_REST_USER_PROFILE,
  LIVE_REST_PROFILE_EFFECTS,
  LIVE_REST_COLLECTIBLE_PRODUCT,
  LIVE_REST_CHANNEL,
  LIVE_REST_FORUM_POSTS,
  LIVE_REST_FORUM_CREATE,
  LIVE_REST_EXPRESSION_DATA,
  LIVE_REST_SEND_STICKER,
  LIVE_REST_SEND_MESSAGE,
  LIVE_REST_CREATE_POLL,
  LIVE_REST_REPORT_MENU,   // GET /reporting/menu/{type}
  LIVE_REST_REPORT,        // POST /reporting/{type}: never retried
  LIVE_REST_SEND_REPLY,
  LIVE_REST_EDIT_MESSAGE,
  LIVE_REST_DELETE_MESSAGE,
  LIVE_REST_TYPING,
  LIVE_REST_REACTION,
  LIVE_REST_REACTION_REFRESH,
  LIVE_REST_REACTION_USERS,
  LIVE_REST_PIN,
  LIVE_REST_ADMIN,   // a server-administration call (see ccord/net/admin.h)
  LIVE_REST_SOUNDBOARD,
  LIVE_REST_CALL_RING,   // POST /channels/{c}/call/ring: ring the recipients of a DM / group DM call we just started
  LIVE_REST_POLL_VOTE,
  LIVE_REST_INTERACTION,
  LIVE_REST_INTERACTION_AUTOCOMPLETE,
  LIVE_REST_SLASH_COMMANDS,
  LIVE_REST_DISCOVERY,
  LIVE_REST_SEND_TTS,
  LIVE_REST_FORWARD_MESSAGE,
  LIVE_REST_CROSSPOST_MESSAGE,
  LIVE_REST_MARK_UNREAD,
  LIVE_REST_PATCH_SETTINGS_PROTO,
  LIVE_REST_STATUS_GET,     // GET /users/@me/settings-proto/1: the base for a status write
  LIVE_REST_STATUS_PATCH,   // PATCH /users/@me/settings-proto/1: the status write itself
  LIVE_REST_MEDIA_RANGE,   // one byte range of a clicked video (CCORD_NET_MEDIA_RANGE)
} live_rest_kind;

// Heap-allocated only for LIVE_REST_UPLOAD_SLOT / PUT / SEND so normal REST ops stay < 1 KiB.
typedef struct live_upload_state {
  size_t file_count;
  size_t upload_idx;
  uint64_t upload_id;
  size_t upload_orig_sizes[CCORD_RICH_MAX_ATTACHMENTS];
  uint64_t upload_total_bytes;
  uint64_t upload_completed_bytes;
  uint64_t upload_cur_file_bytes;
  bool upload_linked;
  struct live_rest_op *upload_prev, *upload_next;
  char *filenames[CCORD_RICH_MAX_ATTACHMENTS];
  uint8_t *file_datas[CCORD_RICH_MAX_ATTACHMENTS];
  size_t file_sizes[CCORD_RICH_MAX_ATTACHMENTS];
  bool file_spoilers[CCORD_RICH_MAX_ATTACHMENTS];
  uint32_t file_durations_ms[CCORD_RICH_MAX_ATTACHMENTS];   // voice message length; 0 for an ordinary file
  char *file_waveforms[CCORD_RICH_MAX_ATTACHMENTS];         // voice message waveform (base64) or nullptr
  char upload_urls[CCORD_RICH_MAX_ATTACHMENTS][2048];
  char upload_filenames[CCORD_RICH_MAX_ATTACHMENTS][256];
} live_upload_state;

// Heap-allocated only for CCORD_ADMIN_REORDER_CHANNELS.
typedef struct live_reorder_data {
  size_t count;
  ccord_id ids[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  int32_t positions[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  ccord_id parents[CCORD_ADMIN_CHANNEL_REORDER_MAX];
  uint8_t has_parent[CCORD_ADMIN_CHANNEL_REORDER_MAX];
} live_reorder_data;

typedef struct live_rest_op {
  live *l;
  live_rest_kind kind;
  ccord_id channel_id;
  ccord_id user_id;
  ccord_id message_id;
  ccord_id guild_id;      // admin: the server (CCORD_DM_GUILD_ID for a DM)
  ccord_id role_id;       // admin: the role the call is about
  ccord_id expr_id;       // admin: the emoji/sticker/soundboard/automod/webhook id the call is about
  ccord_id event_id;      // admin: scheduled event id (EVENT_MODIFY / EVENT_DELETE / EVENT_RSVP_*)
  ccord_id target_channel_id; // admin: FOLLOW_CHANNEL / MOVE_VOICE_MEMBER destination channel_id
  ccord_overwrite overwrite;  // admin: PUT/DELETE_CHANNEL_OVERWRITE
  uint8_t prune_days;     // admin: PREVIEW_PRUNE / EXECUTE_PRUNE days
  char invite_code[CCORD_ADMIN_INVITE_CODE_MAX + 1];   // admin: REVOKE_INVITE
  char nonce[32];         // send: the client nonce, echoed on ACTION_FAILED so the session fails the right echo
  ccord_admin_op admin_op;
  bool flag;              // reaction: add; pin: pinned; upload: mention_author
  char *emoji;            // reaction: wire emoji (not URL-encoded)
  ccord_relationship_kind rel_kind;
  ccord_expr_kind expr_kind;
  char *expr_arg;
  uint32_t forum_offset;
  uint32_t search_offset;
  size_t folder_item_count;
  ccord_guild_folder_item *folder_items;
  live_reorder_data *reorder;   // CCORD_ADMIN_REORDER_CHANNELS (heap-allocated on demand)
  char *voice_status;           // CCORD_ADMIN_VOICE_STATUS (heap-allocated on demand)
  int64_t request_to_speak_ms;
  bool stage_speaker;
  bool has_topic;
  char *topic;                  // CCORD_ADMIN_STAGE_TOPIC / STAGE_CREATE (heap-allocated on demand)
  char *text;
  size_t text_len;
  live_upload_state *upload;    // LIVE_REST_UPLOAD_* (heap-allocated on demand)
  char *media_url;
  char *media_path;
  uint8_t media_stage;   // LIVE_REST_MEDIA only: 0 first GET, 1 POST /attachments/refresh-urls in flight, 2 GET of the refreshed URL
  char *cache_path;
  uint8_t *proto_data;
  size_t proto_len;
  // Mutually exclusive op-specific value payloads (keyed by kind / admin_op).
  union {
    struct {
      uint32_t discovery_category_id;
      uint32_t discovery_offset;
      char discovery_query[128];
    };
    char user_note[CCORD_USER_NOTE_MAX_BYTES + 1];
    struct {
      char custom_status_emoji[CCORD_EMOJI_MAX_BYTES + 1];
      char custom_status_text[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
      ccord_id custom_status_emoji_id;
      int64_t custom_status_expires_ms;
      bool custom_status_clear;
    };
    struct {
      int64_t invites_disabled_until_ms;
      int64_t dms_disabled_until_ms;
    };
    // M6 account state: 204 answers carry the request target so the UI can drop exactly what Discord accepted.
    struct {
      char connection_name[CCORD_ADMIN_CONNECTION_NAME_MAX + 1u];
      char connection_id[CCORD_ADMIN_CONNECTION_ID_MAX + 1u];
    };
    struct {
      char session_hashes[CCORD_ADMIN_SESSION_LOGOUT_MAX][64];
      size_t session_hash_count;
    };
    // LIVE_REST_MEDIA_RANGE: the request as asked (echoed in the event) and the answer's headers (filled by the queue).
    struct {
      uint64_t range_id;
      int64_t range_start;
      size_t range_len;
      ccord_rest_range_info range_info;
    };
  };
} live_rest_op;


// Cross-TU internal helpers
uint64_t live_now(const live *l);
ccord_voice_gateway *live_voice_acquire(live *l);
void live_voice_release(live *l);
bool live_push_with_media(live *l, ccord_live_event *ev, char *owned_media);
bool live_push(live *l, ccord_live_event *ev);
void live_stream_rx_clear(live *l);
void live_stream_tx_clear(live *l);
void live_voice_cache_put(live *l, const ccord_voice_state *v);
const ccord_voice_state *live_voice_cache_find(const live *l, ccord_id guild_id, ccord_id user_id);
void live_voice_cache_from_event(live *l, const ccord_net_event *ev);
void live_chan_guild_put(live *l, ccord_id channel_id, ccord_id guild_id);
void live_chan_set_ack(live *l, ccord_id channel_id, ccord_id ack_id);
ccord_id live_chan_last_ack(const live *l, ccord_id channel_id);
bool live_chan_prev_msg_id(const live *l, ccord_id channel_id, ccord_id message_id, ccord_id *out_ack_id);
ccord_id live_chan_guild_find(const live *l, ccord_id channel_id);
ccord_status live_http_status_error(long status, ccord_error *err, const char *what);
ccord_status live_content_body(const char *utf8, size_t len, const char *nonce, size_t nonce_len, ccord_buf *body, ccord_error *err);
ccord_status live_upload_voice_check(const ccord_upload_file *f, ccord_error *err);
ccord_status live_append_encoded(ccord_buf *out, const char *s, size_t len, ccord_error *err);
void live_rest_op_free(live_rest_op *op);
void live_log_media_failure(const char *url, ccord_status st, long http_status);
ccord_gov_priority live_kind_priority(live_rest_kind kind);
ccord_status live_queue_add(live *l, ccord_rest_method method, const char *path, const char *json_body, live_rest_op *op, ccord_error *err);
ccord_status live_queue_add_raw(live *l, ccord_rest_method method, const char *url, const char *content_type, const void *body, size_t body_len, live_rest_op *op, ccord_error *err);
bool live_media_try_refresh(live *l, live_rest_op *op, long http_status);
bool live_media_refreshed(live *l, live_rest_op *op, const char *body, size_t len);
live_rest_op *live_action_op(live *l, live_rest_kind kind, ccord_id channel_id, ccord_id message_id, const char *draft, size_t draft_len);
ccord_status live_action_submit(live *l, ccord_rest_method method, const char *path, const char *json_body, live_rest_op *op, ccord_error *err);
const char *live_rest_name(live_rest_kind kind);
void live_ensure_live_cache_dirs(const char *file_path);
bool live_api_cache_enabled(const live *l);
bool live_api_cache_path(const live *l, const char *path, char *out, size_t cap);
char *live_api_cache_read(const char *cache_path, uint32_t ttl_sec, size_t *out_len);
void live_api_cache_write(const char *cache_path, const char *body, size_t len);
void live_warm_history_write(const live *l, ccord_id channel_id, const char *body, size_t len);
void live_api_cache_invalidate_channel_history(const live *l, ccord_id channel_id);
bool live_push_media_event_owned(live *l, const char *url, const char *path, char *owned_data, size_t len);
bool live_is_authenticated(const live *l);
void live_fill_guild_profile_unavailable(ccord_live_event *ev, ccord_id guild_id);
void live_admin_done(live *l, const live_rest_op *op, const char *body, size_t len);
bool live_b64_decode_alloc(const char *b64, size_t len, uint8_t **out_raw, size_t *out_len);
ccord_status live_b64_encode(const uint8_t *src, size_t len, ccord_buf *out);
void live_push_expression_data_ok(live *l, ccord_expr_kind kind, const char *arg, size_t arg_len, const char *body, size_t body_len, const uint8_t *fallback_proto, size_t fallback_proto_len, bool has_fallback_proto, const char *cache_path);
void live_status_on_event(live *l, const ccord_net_event *ev);
void live_status_get_done(live *l, const char *body, size_t len);
void live_status_patch_done(live *l, const char *body, size_t len);
void live_status_failed(live *l, const live_rest_op *op);
void live_upsert_effect_config(live *l, const ccord_profile_effect_config *cfg);
void live_upsert_frame_config(live *l, const ccord_profile_frame_config *cfg);
void live_clear_pending_decor_profiles(live *l);
void live_enrich_user_profile(live *l, ccord_user_profile *prof, const char *raw_body, size_t raw_len, bool queue_missing);
void live_flush_pending_decor_profiles(live *l);
void live_rest_done(void *ud, ccord_status status, long http_status, const char *body, size_t len);
ccord_status live_enqueue_rest_cached(live *l, ccord_rest_method method, const char *path, live_rest_kind kind, ccord_id channel_id, const char *cache_path, ccord_error *err);
ccord_status live_enqueue_rest(live *l, ccord_rest_method method, const char *path, live_rest_kind kind, ccord_id channel_id, ccord_error *err);
bool live_poll(ccord_transport *t, ccord_net_event *out);
ccord_status live_request_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err);
ccord_status live_request_history_before(ccord_transport *t, ccord_id channel_id, ccord_id before_id, uint32_t limit, ccord_error *err);
ccord_status live_request_pins(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err);
ccord_status live_request_pins_before(ccord_transport *t, ccord_id channel_id, const char *before_cursor, uint32_t limit, ccord_error *err);
ccord_status live_send_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len, ccord_error *err);
ccord_status live_send_message_with_nonce(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len, const char *nonce, size_t nonce_len, ccord_error *err);
ccord_status live_send_tts_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len, ccord_error *err);
ccord_status live_create_poll(ccord_transport *t, ccord_id channel_id, const ccord_poll_create *poll, ccord_error *err);
ccord_status live_request_report_menu(ccord_transport *t, ccord_report_type type, ccord_error *err);
ccord_status live_submit_report(ccord_transport *t, const ccord_report_request *req, ccord_error *err);
ccord_status live_forward_message(ccord_transport *t, ccord_id channel_id, ccord_id source_channel_id, ccord_id message_id, ccord_error *err);
ccord_status live_crosspost_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err);
ccord_status live_edit_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, const char *utf8, size_t len, ccord_error *err);
ccord_status live_delete_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err);
ccord_status live_send_typing(ccord_transport *t, ccord_id channel_id, ccord_error *err);
ccord_status live_toggle_reaction(ccord_transport *t, ccord_id channel_id, ccord_id message_id, const char *emoji, size_t len, bool add, ccord_error *err);
ccord_status live_request_reaction_users(ccord_transport *t, ccord_id channel_id, ccord_id message_id, const char *emoji, size_t emoji_len, uint32_t limit, ccord_error *err);
ccord_status live_ack_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err);
ccord_status live_mark_unread(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err);
void live_vstream_slot_close_media(live *l, live_vstream_slot *slot);
void live_vstream_slot_reset(live *l, live_vstream_slot *slot);
void live_streams_stop_all(live *l);
void live_voice_stop(live *l);
void live_copy_cstr(char *dst, size_t cap, const char *src);
void live_vstream_maybe_open(live *l, live_vstream_slot *slot);
void live_stream_on_event(live *l, const ccord_net_event *ev);
void live_voice_maybe_start(live *l);
void live_voice_lost(live *l);
void live_voice_watch(live *l, uint64_t now);
void live_voice_link_tick(live *l, uint64_t now);
void live_voice_main_session_replaced(live *l);
void live_set_self_voice_flags(live *l, bool self_mute, bool self_deaf);
bool live_voice_on_event(live *l, const ccord_net_event *ev, const char *voice_session_id);
long live_call_index(const live *l, ccord_id channel_id);
bool live_call_drop_ringing(ccord_call_state *call, ccord_id user_id);
void live_call_on_event(live *l, const ccord_net_event *ev);
ccord_status live_join_voice(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err);
ccord_status live_leave_voice(ccord_transport *t, ccord_id guild_id, ccord_error *err);
ccord_status live_update_voice_state(ccord_transport *t, ccord_id guild_id, bool self_mute, bool self_deaf, ccord_error *err);
ccord_status live_start_stream(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, const ccord_stream_config *config, ccord_error *err);
ccord_status live_stop_stream(ccord_transport *t, ccord_id guild_id, ccord_error *err);
ccord_status live_watch_stream(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id, ccord_error *err);
ccord_status live_stop_watching(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id, ccord_error *err);
ccord_status live_set_stream_paused(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id, bool paused, ccord_error *err);
ccord_status live_send_reply_ex(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id, bool mention_author, const char *utf8, size_t len, ccord_error *err);
ccord_status live_send_reply(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id, const char *utf8, size_t len, ccord_error *err);
ccord_status live_toggle_pin(ccord_transport *t, ccord_id channel_id, ccord_id message_id, bool pinned, ccord_error *err);
ccord_status live_admin_action(ccord_transport *t, const ccord_admin_request *req, ccord_error *err);
ccord_status live_set_presence_ex(ccord_transport *t, ccord_presence presence, const char *emoji_name, ccord_id emoji_id, const char *status_text, ccord_error *err);
ccord_status live_set_presence(ccord_transport *t, ccord_presence presence, const char *status_text, ccord_error *err);
ccord_status live_set_rich_activities(ccord_transport *t, const char *json, size_t len, ccord_error *err);
ccord_status live_set_voice_camera(ccord_transport *t, ccord_id guild_id, bool enabled, ccord_error *err);
ccord_status live_send_message_with_files(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id, bool mention_author, const char *utf8, size_t len, const ccord_upload_file *files, size_t file_count, ccord_error *err);
size_t live_active_uploads(ccord_transport *t, ccord_upload_progress *out, size_t max);
ccord_status live_send_message_with_file(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len, const char *filename, const void *file_data, size_t file_size, ccord_error *err);
ccord_status live_request_slash_commands(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err);
ccord_status live_fetch_media(ccord_transport *t, const char *url, ccord_error *err);
ccord_status live_fetch_media_user(ccord_transport *t, const char *url, ccord_error *err);
ccord_status live_fetch_media_range(ccord_transport *t, const ccord_media_range_req *req, ccord_error *err);
ccord_status live_resolve_video(ccord_transport *t, uint64_t req_id, const char *url, ccord_error *err);
ccord_status live_search_messages_ex(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, const ccord_search_query *query, ccord_error *err);
ccord_status live_search_messages(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, const char *query, size_t query_len, ccord_error *err);
ccord_status live_open_dm(ccord_transport *t, ccord_id recipient_id, ccord_error *err);
ccord_status live_request_channel(ccord_transport *t, ccord_id channel_id, ccord_error *err);
ccord_status live_push_voice_pcm(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err);
size_t live_pull_voice_pcm(ccord_transport *t, int16_t *pcm, size_t max_samples);
void live_soundboard_done(live *l, const live_rest_op *op, const char *body, size_t len);
void live_soundboard_feed(live *l);
ccord_status live_call_action(ccord_transport *t, ccord_id channel_id, ccord_call_action act, ccord_error *err);
ccord_status live_play_soundboard(ccord_transport *t, ccord_id guild_id, ccord_id sound_id, ccord_error *err);
ccord_status live_set_user_volume(ccord_transport *t, ccord_id user_id, uint8_t pct, ccord_error *err);
ccord_status live_set_voice_config(ccord_transport *t, const ccord_voice_config *cfg, ccord_error *err);
ccord_status live_set_relationship(ccord_transport *t, ccord_id user_id, ccord_relationship_kind kind, ccord_error *err);
ccord_status live_request_profile_ex(ccord_transport *t, ccord_id user_id, ccord_id guild_id, ccord_error *err);
ccord_status live_request_profile(ccord_transport *t, ccord_id user_id, ccord_error *err);
ccord_status live_select_channel(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err);
ccord_status live_request_members(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, uint32_t chunks, ccord_error *err);
ccord_status live_select_guild(ccord_transport *t, ccord_id guild_id, ccord_error *err);
ccord_status live_request_forum_posts(ccord_transport *t, const ccord_forum_query *query, ccord_error *err);
ccord_status live_create_forum_post(ccord_transport *t, ccord_id forum_id, const char *title, size_t title_len, const char *content, size_t content_len, const ccord_id *tag_ids, size_t tag_count, ccord_error *err);
ccord_status live_request_expression_data(ccord_transport *t, ccord_expr_kind kind, const char *arg, size_t arg_len, ccord_error *err);
ccord_status live_patch_settings_proto(ccord_transport *t, const uint8_t *proto, size_t proto_len, ccord_error *err);
ccord_status live_set_status_proto(ccord_transport *t, const ccord_proto_status *st, ccord_error *err);
ccord_status live_send_sticker(ccord_transport *t, ccord_id channel_id, ccord_id sticker_id, ccord_error *err);
ccord_status live_vote_poll(ccord_transport *t, ccord_id channel_id, ccord_id message_id, const uint32_t *answer_ids, size_t answer_count, ccord_error *err);
ccord_status live_send_interaction(ccord_transport *t, const ccord_interaction_request *req, ccord_error *err);
ccord_status live_request_discoverable_guilds(ccord_transport *t, const char *query, uint32_t category_id, uint32_t offset, uint32_t limit, ccord_error *err);
ccord_status live_send_stream_frame(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k, bool keyframe, ccord_error *err);
size_t live_camera_senders(ccord_transport *t, ccord_camera_rx *out, size_t max);
bool live_poll_camera_frame(ccord_transport *t, ccord_id user_id, uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t *out_ts90k, bool *out_keyframe);
bool live_poll_stream_frame(ccord_transport *t, uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t *out_ts90k, bool *out_keyframe);
ccord_status live_send_stream_pcm(ccord_transport *t, const int16_t *pcm, size_t samples, ccord_error *err);
size_t live_poll_stream_pcm(ccord_transport *t, int16_t *out_pcm, size_t cap_samples);
ccord_status live_send_voice_video(ccord_transport *t, const uint8_t *annexb, size_t len, uint32_t ts90k, bool keyframe, ccord_error *err);
bool live_consume_keyframe_request(ccord_transport *t);
