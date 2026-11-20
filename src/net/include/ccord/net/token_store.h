// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>
#include <stddef.h>
#include <stdint.h>

#ifndef CCORD_LIVE_TOKEN_MAX
#define CCORD_LIVE_TOKEN_MAX 512u
#endif

#define CCORD_LIVE_TOKEN_ENV "CCORD_DISCORD_TOKEN"

// Reads CCORD_DISCORD_TOKEN, else reads standalone token file at
// $XDG_CONFIG_HOME/ccord/token (or explicit token_path_or_null when provided).
// `out` is only written on success. `out` is NOT wiped on failure.
[[nodiscard]] ccord_status ccord_live_load_token(const char *token_path_or_null, char *out, size_t cap, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_token_path(char *out, size_t cap, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_save_token(const char *path_or_null, const char *token, ccord_error *err);
[[nodiscard]] ccord_status ccord_live_delete_token(const char *path_or_null, ccord_error *err);
void ccord_live_wipe(void *p, size_t n);
const char *ccord_live_user_agent(void);

// Multi-account keyring ------------------------------------------------------
// Account metadata lives in $XDG_CONFIG_HOME/ccord/accounts.ini (0600); each
// account's token lives in $XDG_CONFIG_HOME/ccord/tokens/<slot_id> (0600) and
// the active account's token is mirrored atomically to the legacy single-token
// file so ccord_live_load_token/ccord_live_save_token keep working unchanged.
// The first list/switch/upsert/remove migrates an existing single token file to
// a "legacy" account without deleting or rewriting the original token.
#define CCORD_MAX_SAVED_ACCOUNTS 8u
#define CCORD_SAVED_ACCOUNT_NAME_MAX 128u
#define CCORD_SAVED_ACCOUNT_SLOT_MAX 64u
#define CCORD_SAVED_ACCOUNT_AVATAR_MAX 128u
#define CCORD_NO_ACTIVE_ACCOUNT (-1)

typedef struct ccord_saved_account {
  uint64_t user_id;                                  // 0 for a migrated token whose user is unknown
  char username[CCORD_SAVED_ACCOUNT_NAME_MAX];
  char display_name[CCORD_SAVED_ACCOUNT_NAME_MAX];
  char avatar_hash[CCORD_SAVED_ACCOUNT_AVATAR_MAX];
  char slot_id[CCORD_SAVED_ACCOUNT_SLOT_MAX];        // decimal user id unless the caller supplied one
  bool active;
} ccord_saved_account;

// Copies at most `cap` saved accounts into `out` (all of them when cap is
// large enough) and reports the total saved and the active index (-1 when no
// account is active). Implemented by token_store.c when CCORD_LIVE_ACCOUNT=1.
[[nodiscard]] ccord_status ccord_live_accounts_list(ccord_saved_account *out, size_t cap, size_t *out_count,
                                                    int *out_active_index, ccord_error *err);
// Inserts or updates an account and writes its token file. When make_active is
// true, or no account is active yet, the account becomes active and its token
// is synced to $XDG_CONFIG_HOME/ccord/token.
[[nodiscard]] ccord_status ccord_live_account_upsert(const ccord_saved_account *account, const char *token,
                                                     bool make_active, ccord_error *err);
// Selects an account by slot_id or decimal user id, writes its token to
// `out_token`, makes it active and syncs the legacy token file.
[[nodiscard]] ccord_status ccord_live_account_switch(const char *user_id_or_slot, char *out_token, size_t cap,
                                                     ccord_error *err);
// Reads a saved account's token into `out_token` without changing the active
// account and without writing any file. Fails with CCORD_ERR_NOT_FOUND for an
// unknown account.
[[nodiscard]] ccord_status ccord_live_account_peek_token(const char *user_id_or_slot, char *out_token, size_t cap,
                                                         ccord_error *err);
// Removes an account and its token. Removing the active account selects the
// next remaining one, or deletes the legacy token file when none remain.
[[nodiscard]] ccord_status ccord_live_account_remove(const char *user_id_or_slot, ccord_error *err);

// REST v9 -------------------------------------------------------------------
