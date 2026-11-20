// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Warm-start disk cache: the store snapshot and the last history pages of recent
// channels live under <data_dir>/accounts/<fp>/ so that a non-cold start can draw the guild list, the channel list and
// the last viewed channel before the network answers (stale-while-revalidate). Every file is 0600 in a 0700 directory,
// written atomically, versioned and sha256-checked; anything that fails a check is deleted and the app cold starts.
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/model/model.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>

#define CCORD_WARM_FORMAT_VERSION 1u
#define CCORD_WARM_MAX_FILE_BYTES (8u << 20)
#define CCORD_WARM_MAX_HISTORY_FILE_BYTES (512u << 10)
#define CCORD_WARM_MAX_USERS 4000u
#define CCORD_WARM_MAX_MEMBERS 500u
#define CCORD_WARM_MAX_HISTORY_CHANNELS 8u
#define CCORD_WARM_MAX_ACCOUNT_DIRS 8u
#define CCORD_WARM_SAVE_INTERVAL_MS 60000u
#define CCORD_WARM_FIRST_SAVE_DELAY_MS 5000u

typedef struct ccord_warm ccord_warm;

typedef struct ccord_warm_config {
  const char *data_dir;        // ccord_paths_data_dir()
  const char *token;           // hashed for the fingerprint, never stored; null/"" -> "mock"
  const char *app_version;     // informational, written to the state file
  int64_t (*now_ms)(void *ud); // unix ms; null -> ccord_clock_unix_ms
  void *clock_ud;
  bool enabled;                // false -> every call below is a no-op returning "not loaded"
  size_t max_file_bytes;       // 0 -> CCORD_WARM_MAX_FILE_BYTES (tests lower it to exercise the size fallback)
} ccord_warm_config;

typedef struct ccord_warm_load_result {
  bool loaded;
  int64_t saved_ms;
  size_t guilds, channels, users, history_channels;
  ccord_id self_id, sel_guild, sel_channel;
  uint32_t load_ms;
} ccord_warm_load_result;

// 32 lowercase hex characters: SHA-256 over "ccord-warm-v1" NUL token. The token is not recoverable from it.
void ccord_warm_fingerprint(const char *token, char out[33]);

// True unless `--no-warm-cache`-style opt-outs apply: reads [ui] warm_cache from <data_dir>/settings.ini
// (default true; "0"/"false"/"off"/"no" disable).
[[nodiscard]] bool ccord_warm_pref_enabled(const char *data_dir);

// Creates <data_dir>/accounts/<fp> (0700) and prunes older account directories to CCORD_WARM_MAX_ACCOUNT_DIRS.
// Reads no cache file. Returns nullptr only on out-of-memory or a directory that cannot be created; a disabled config
// returns a handle whose calls are no-ops.
[[nodiscard]] ccord_warm *ccord_warm_open(const ccord_warm_config *cfg, ccord_error *err);
void ccord_warm_close(ccord_warm *w);
bool ccord_warm_enabled(const ccord_warm *w);
// <data_dir>/accounts/<fp>, or nullptr when disabled (pass to ccord_live_config.cache_dir).
const char *ccord_warm_dir(const ccord_warm *w);
// <dir>/warm/state.json
[[nodiscard]] bool ccord_warm_state_path(const ccord_warm *w, char *out, size_t cap);

// Loads the cache into `store` and marks `session` stale. CCORD_OK with loaded == false means a cold start (no file,
// or one that failed a check and was removed). Call before ccord_ui_create so the first frame draws real content.
[[nodiscard]] ccord_status ccord_warm_load(ccord_warm *w, ccord_store *store, ccord_session *session,
                                           ccord_warm_load_result *out, ccord_error *err);

// Serialises the store (never while the caller knows it is stale). `sync` fsyncs the file and its directory.
[[nodiscard]] ccord_status ccord_warm_save(ccord_warm *w, const ccord_store *store, bool sync, ccord_error *err);

// Rate-limited save for the main loop: false (nothing written) while the session is stale or not connected, when the
// store did not change, within CCORD_WARM_SAVE_INTERVAL_MS of the last save, or before CCORD_WARM_FIRST_SAVE_DELAY_MS
// after the connection came up. `now_ms` is any monotonic millisecond clock. Returns true when a file was written.
bool ccord_warm_maybe_save(ccord_warm *w, const ccord_store *store, const ccord_session *session, uint64_t now_ms);

// Fixture/test writer for a channel's history page (raw REST body); 0600, atomic.
[[nodiscard]] ccord_status ccord_warm_put_history(ccord_warm *w, ccord_id channel_id, const char *json, size_t len,
                                                  ccord_error *err);

// Reads and verifies a state file (header, length, version, sha256) and returns its "saved_ms" and body length.
// Used by the slice and the tests; either out pointer may be null.
[[nodiscard]] bool ccord_warm_verify_file(const char *path, int64_t *saved_ms, size_t *body_len, size_t max_bytes);

// Removes this account's directory (sign-out). Further saves are refused.
void ccord_warm_forget(ccord_warm *w);
// Removes <data_dir>/accounts/<fingerprint(token)>; false (nothing removed) for a null/empty token, a missing
// directory or a symlink. The token is only hashed.
bool ccord_warm_forget_token(const char *data_dir, const char *token);
// Removes the account directory whose meta.json names `user_id` (account removal without its token).
void ccord_warm_forget_user(const char *data_dir, ccord_id user_id);
