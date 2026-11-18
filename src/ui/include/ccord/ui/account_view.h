// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Pure Account-page view model. No renderer, no network, no tokens: only the strings and flags the page needs.
#include <ccord/ui/account_identity.h>

#define CCORD_ACCOUNT_NON_GOAL_COUNT 6u

typedef enum ccord_account_mode {
  CCORD_ACCOUNT_SIGNED_OUT = 0,
  CCORD_ACCOUNT_UNAVAILABLE,
  CCORD_ACCOUNT_WAITING,
  CCORD_ACCOUNT_FINISHING,
  CCORD_ACCOUNT_SIGNED_IN,
  CCORD_ACCOUNT_SIGNED_IN_TRUNCATED,
  CCORD_ACCOUNT_ERROR,
} ccord_account_mode;

typedef enum ccord_account_error {
  CCORD_ACCOUNT_ERROR_NONE = 0,
  CCORD_ACCOUNT_ERROR_DENIED,
  CCORD_ACCOUNT_ERROR_NETWORK,
  CCORD_ACCOUNT_ERROR_STATE_MISMATCH,
  CCORD_ACCOUNT_ERROR_ISS_MISMATCH,
  CCORD_ACCOUNT_ERROR_ATTEMPT_EXPIRED,
  CCORD_ACCOUNT_ERROR_TOKEN_REJECTED,
  CCORD_ACCOUNT_ERROR_SCOPE_MISSING,
  CCORD_ACCOUNT_ERROR_RATE_LIMITED,
  CCORD_ACCOUNT_ERROR_SERVER,
  CCORD_ACCOUNT_ERROR_BAD_RESPONSE,
  CCORD_ACCOUNT_ERROR_LISTENER_UNAVAILABLE,
  CCORD_ACCOUNT_ERROR_CANCELLED,
} ccord_account_error;

typedef struct ccord_account_view {
  ccord_account_mode mode;
  ccord_account_error error;
  bool configured;               // false: no client id in this build
  bool signed_in;
  bool busy;                     // waiting for the browser or finishing the sign-in
  bool can_sign_in;
  bool can_sign_out;
  bool can_refresh;
  bool demo_chip_visible;        // chat header shows "Demo data" only while signed in
  bool show_non_goals;
  const char *heading;           // "Account"
  const char *status;            // short state line, always present
  const char *detail;            // sentence under the status; may be null
  const char *display_name;      // signed in only; points into the identity
  const char *username;          // signed in only; points into the identity
  char user_id_text[21];         // signed in only; decimal snowflake
  size_t guild_count;
  bool guilds_truncated;
  const char *const *non_goals;
  size_t non_goal_count;
  const ccord_identity *identity;   // signed in only; plain profile data, never a token
} ccord_account_view;

// Builds all eight designed states into `out`. `identity` may be null unless the mode is signed in or truncated.
void ccord_account_view_build(ccord_account_view *out, ccord_account_mode mode, ccord_account_error error,
                              bool configured, const ccord_identity *identity);

const char *ccord_account_mode_name(ccord_account_mode mode);       // "signed_out" ... "error"
const char *ccord_account_error_name(ccord_account_error error);    // "DENIED", "NETWORK", ...
