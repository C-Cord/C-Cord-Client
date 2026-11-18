// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// App glue for the Account page: token-session state only. Sign-in happens through the governed live login/MFA
// queue or a saved user token (see src/app/main.c); no OAuth, HTTP, socket or fixture object exists here.
#include <ccord/core/error.h>
#include <ccord/ui/account_identity.h>
#include <ccord/ui/account_view.h>

typedef struct ccord_account ccord_account;

[[nodiscard]] ccord_account *ccord_account_create(ccord_error *err);
void ccord_account_destroy(ccord_account *a);

// No OAuth flow exists in this build: sign-in and refresh are answered, never started.
[[nodiscard]] ccord_status ccord_account_begin(ccord_account *a, ccord_error *err);
[[nodiscard]] ccord_status ccord_account_refresh(ccord_account *a, ccord_error *err);
[[nodiscard]] ccord_status ccord_account_sign_out(ccord_account *a, ccord_error *err);

const ccord_account_view *ccord_account_view_of(const ccord_account *a);
const ccord_identity *ccord_account_identity(const ccord_account *a);   // null unless a token session is active

// Multi-account roster (up to CCORD_ACCOUNT_MAX_SAVED). The roster is in-memory; new sign-ins are upserted
// automatically by ccord_account_set_token_state and by a refresh/begin while a token session is active.
#define CCORD_ACCOUNT_MAX_SAVED 8u
#define CCORD_ACCOUNT_NAME_MAX 128u
#define CCORD_ACCOUNT_AVATAR_MAX 128u

typedef struct ccord_account_entry {
  uint64_t user_id;
  char username[CCORD_ACCOUNT_NAME_MAX];
  char display_name[CCORD_ACCOUNT_NAME_MAX];
  char avatar_hash[CCORD_ACCOUNT_AVATAR_MAX];
  bool active;
} ccord_account_entry;

// Returns the number of saved accounts and copies at most `cap` entries into `out` (which may be null).
size_t ccord_account_list(const ccord_account *a, ccord_account_entry *out, size_t cap);
// Inserts or updates an account. `entry->active` (or having no active account) selects it as active.
[[nodiscard]] ccord_status ccord_account_upsert(ccord_account *a, const ccord_account_entry *entry, ccord_error *err);
// Makes the saved account active and rebuilds the identity/view from its stored metadata.
[[nodiscard]] ccord_status ccord_account_switch(ccord_account *a, uint64_t user_id, ccord_error *err);
// Removes a saved account; removing the active one selects the next, or signs out when none remain.
[[nodiscard]] ccord_status ccord_account_remove(ccord_account *a, uint64_t user_id, ccord_error *err);
// Stores an avatar hash on the active roster entry (no-op when no account is active).
void ccord_account_set_avatar_hash(ccord_account *a, const char *avatar_hash);

// Standalone token & live Discord session state.
void ccord_account_set_token_state(ccord_account *a, bool active, bool connected,
                                   const char *display_name, const char *username,
                                   uint64_t user_id, size_t guild_count);
bool ccord_account_has_token_session(const ccord_account *a);
