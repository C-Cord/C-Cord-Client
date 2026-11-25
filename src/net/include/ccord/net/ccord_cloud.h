// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CCORD_CLOUD_MAX_CLAIMED 16u
#define CCORD_CLOUD_MAX_DEVICES 16u
#define CCORD_CLOUD_MAX_COMMENTS 24u
#define CCORD_CLOUD_MAX_REPLIES 8u
#define CCORD_CLOUD_VOTE_CLEAR 99

typedef enum ccord_rating_sentiment : uint8_t {
  CCORD_RATING_UNRATED  = 0,
  CCORD_RATING_NEGATIVE = 1,
  CCORD_RATING_NEUTRAL  = 2,
  CCORD_RATING_POSITIVE = 3,
} ccord_rating_sentiment;

typedef enum ccord_wake_kind : uint8_t {
  CCORD_WAKE_NONE = 0,
  CCORD_WAKE_MSG  = 1,
  CCORD_WAKE_CALL = 2,
} ccord_wake_kind;

typedef struct ccord_cloud_device_info {
  char id[48];
  char name[65];
  char platform[16]; // "desktop" | "android"
  char role[16];     // "companion" | "mobile" | "other"
  bool wake_enabled;
  bool online;
  bool is_current;
} ccord_cloud_device_info;

typedef struct ccord_cloud_claimed_plugin {
  char id[65];
  char name[65];
  char version[33];
  uint64_t downloads;
  int16_t rating_score_x10; // -1 if unrated
  ccord_rating_sentiment rating_sentiment;
} ccord_cloud_claimed_plugin;

typedef struct ccord_cloud_state {
  bool authenticated;
  bool signing_in;
  bool busy;
  bool wake_enabled;
  bool wake_connected;
  bool companion_online;
  bool discord_oauth_ready;
  bool github_link_ready;
  char user_id[48];
  char discord_id[32];
  char username[65];
  char display_name[65];
  char avatar_url[256];
  char github_login[65];
  char device_id[48];
  char device_name[65];
  char device_role[16];
  char status_text[192];
  char error_text[192];
  size_t claimed_count;
  ccord_cloud_claimed_plugin claimed[CCORD_CLOUD_MAX_CLAIMED];
  size_t device_count;
  ccord_cloud_device_info devices[CCORD_CLOUD_MAX_DEVICES];
  uint64_t revision;
} ccord_cloud_state;

typedef struct ccord_cloud_rating {
  int16_t score_x10; // -1 when unrated, else 0..100 (score = score_x10 / 10.0)
  ccord_rating_sentiment sentiment;
  uint32_t pos;
  uint32_t neu;
  uint32_t neg;
  uint32_t total;
  bool has_my_vote;
  int8_t my_vote; // -1 | 0 | 1
} ccord_cloud_rating;

typedef struct ccord_cloud_reply {
  int64_t id;
  char author_username[65];
  char author_display[65];
  char plugin_version[33];
  char created_date[16];
  char body[512];
  bool is_developer;
  bool is_mine;
  bool is_deleted;
} ccord_cloud_reply;

typedef struct ccord_cloud_comment {
  int64_t id;
  char author_username[65];
  char author_display[65];
  char plugin_version[33];
  char created_date[16];
  char body[512];
  bool is_developer;
  bool is_mine;
  bool is_deleted;
  size_t reply_count;
  ccord_cloud_reply replies[CCORD_CLOUD_MAX_REPLIES];
} ccord_cloud_comment;

typedef struct ccord_cloud_social {
  bool loading;
  bool loaded;
  bool submitting;
  bool not_found;
  char plugin_id[65];
  char claimed_dev[65];
  char error_text[192];
  ccord_cloud_rating rating;
  uint32_t total_comments;
  bool has_more_comments;
  int64_t next_comment_cursor;
  size_t comment_count;
  ccord_cloud_comment comments[CCORD_CLOUD_MAX_COMMENTS];
  uint64_t revision;
} ccord_cloud_social;

typedef struct ccord_cloud_cbs {
  void *ud;
  bool (*open_url)(void *ud, const char *url);
  void (*toast)(void *ud, const char *msg, uint8_t level);
  // Thread-safe wakeup for the UI/app main loop (called when wakes, companion changes, or async jobs complete).
  void (*wake_main)(void *ud);
} ccord_cloud_cbs;

typedef struct ccord_cloud_config {
  const char *data_dir;          // directory holding ccord-account.json (0600)
  const char *account_base_url;  // optional override (default "https://c-cord.org")
  const char *catalog_base_url;  // optional override (default "https://plugins.c-cord.org")
  bool is_mobile;                // true on Android/mobile -> role "mobile", redirect ccord://auth/callback
  bool disable_network;          // true in headless/mock/verify runs (no sockets or HTTP calls)
  ccord_cloud_cbs cbs;
} ccord_cloud_config;

typedef struct ccord_cloud ccord_cloud;

ccord_cloud *ccord_cloud_create(const ccord_cloud_config *cfg);
void ccord_cloud_destroy(ccord_cloud *c);

// Pump completed async notifications (toasts / open_url requests) on the main thread.
// Returns true if state or social revision changed since last pump.
bool ccord_cloud_pump(ccord_cloud *c);

// Thread-safe snapshots (copy by value; zero dangling pointers).
void ccord_cloud_get_state(const ccord_cloud *c, ccord_cloud_state *out);
void ccord_cloud_get_social(const ccord_cloud *c, ccord_cloud_social *out);
uint64_t ccord_cloud_revision(const ccord_cloud *c);
uint64_t ccord_cloud_social_revision(const ccord_cloud *c);

// Account & Developer Actions (non-blocking; run on background worker).
void ccord_cloud_start_login(ccord_cloud *c);
void ccord_cloud_cancel_login(ccord_cloud *c);
// Handles ccord://auth/callback?... and ccord://account/github-linked?... deep links.
// Returns true if the URL belonged to ccord_cloud.
bool ccord_cloud_handle_callback_url(ccord_cloud *c, const char *url);
void ccord_cloud_refresh(ccord_cloud *c);
void ccord_cloud_sign_out(ccord_cloud *c);
void ccord_cloud_link_github(ccord_cloud *c);
void ccord_cloud_unlink_github(ccord_cloud *c);
void ccord_cloud_rescan_claims(ccord_cloud *c);
void ccord_cloud_set_wake_enabled(ccord_cloud *c, bool enabled);
void ccord_cloud_revoke_device(ccord_cloud *c, const char *device_id);

// Plugin Social Actions (Ratings & Comments).
void ccord_cloud_fetch_social(ccord_cloud *c, const char *plugin_id);
// vote: -1 (Negative), 0 (Neutral), 1 (Positive), or CCORD_CLOUD_VOTE_CLEAR to remove vote.
void ccord_cloud_rate_plugin(ccord_cloud *c, const char *plugin_id, int vote);
void ccord_cloud_post_comment(ccord_cloud *c, const char *plugin_id, int64_t parent_id, const char *body);
void ccord_cloud_delete_comment(ccord_cloud *c, const char *plugin_id, int64_t comment_id);

// Companion Wake API (bare "msg" / "call" relay — zero Discord tokens, zero message content).
void ccord_cloud_emit_wake(ccord_cloud *c, ccord_wake_kind kind);
bool ccord_cloud_pop_wake(ccord_cloud *c, ccord_wake_kind *out_kind);
bool ccord_cloud_companion_online(const ccord_cloud *c);
void ccord_cloud_wake_heartbeat_now(ccord_cloud *c);

// Exposed pure helpers for unit testing (no network required).
bool ccord_cloud_pkce_s256(const char *verifier, char challenge_out[64]);
bool ccord_cloud_parse_social_json(const char *json, size_t len, ccord_cloud_social *out);
bool ccord_cloud_parse_me_json(const char *json, size_t len, ccord_cloud_state *out);
bool ccord_cloud_parse_query_param(const char *query, const char *key, char *out, size_t cap);
