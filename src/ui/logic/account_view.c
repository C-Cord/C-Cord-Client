// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/ui/account_view.h>

#include <stdio.h>
#include <string.h>

static const char *const k_non_goals[CCORD_ACCOUNT_NON_GOAL_COUNT] = {
  "Server channels and messages",
  "Message history",
  "Live messages",
  "Sending messages",
  "Direct messages",
  "Voice",
};

static const char *error_detail(ccord_account_error error)
{
  switch (error) {
    case CCORD_ACCOUNT_ERROR_DENIED: return "Discord did not grant access. Try again when you are ready.";
    case CCORD_ACCOUNT_ERROR_NETWORK: return "Could not reach Discord. Check the connection and try again.";
    case CCORD_ACCOUNT_ERROR_STATE_MISMATCH: return "The sign-in response did not match this attempt. Start again.";
    case CCORD_ACCOUNT_ERROR_ISS_MISMATCH: return "The sign-in response came from an unexpected issuer. Start again.";
    case CCORD_ACCOUNT_ERROR_ATTEMPT_EXPIRED: return "The sign-in attempt expired. Start again.";
    case CCORD_ACCOUNT_ERROR_TOKEN_REJECTED: return "Discord rejected the sign-in token. Start again.";
    case CCORD_ACCOUNT_ERROR_SCOPE_MISSING: return "Discord did not grant the identify and guilds permissions this page needs.";
    case CCORD_ACCOUNT_ERROR_RATE_LIMITED: return "Discord is limiting requests. Wait a moment and try again.";
    case CCORD_ACCOUNT_ERROR_SERVER: return "Discord returned an error. Try again later.";
    case CCORD_ACCOUNT_ERROR_BAD_RESPONSE: return "Discord's response could not be read. Try again.";
    case CCORD_ACCOUNT_ERROR_LISTENER_UNAVAILABLE: return "The local sign-in listener could not start. Restart the app and try again.";
    case CCORD_ACCOUNT_ERROR_CANCELLED: return "Sign-in was cancelled.";
    case CCORD_ACCOUNT_ERROR_NONE: break;
  }
  return nullptr;
}

void ccord_account_view_build(ccord_account_view *out, ccord_account_mode mode, ccord_account_error error,
                              bool configured, const ccord_identity *identity)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  out->mode = mode;
  out->error = error;
  out->configured = configured;
  out->heading = "Account";

  if ((mode == CCORD_ACCOUNT_SIGNED_IN || mode == CCORD_ACCOUNT_SIGNED_IN_TRUNCATED) && !identity) {
    mode = CCORD_ACCOUNT_ERROR;
    error = CCORD_ACCOUNT_ERROR_BAD_RESPONSE;
    out->mode = mode;
    out->error = error;
  }

  switch (mode) {
    case CCORD_ACCOUNT_SIGNED_OUT:
      out->status = "Not signed in";
      out->detail = "Sign in with your browser to see your Discord profile and guild names.";
      out->can_sign_in = configured;
      out->show_non_goals = true;
      break;
    case CCORD_ACCOUNT_UNAVAILABLE:
      out->status = configured ? "Live sign-in is not available in this build" : "Not configured in this build";
      out->detail = configured ? "This build has no live Discord sign-in: live sign-in is not available in this build."
                               : "This build has no public client id: not configured in this build.";
      out->show_non_goals = true;
      break;
    case CCORD_ACCOUNT_WAITING:
      out->status = "Waiting for your browser";
      out->detail = "Finish signing in at the page that just opened. Nothing is sent until you approve.";
      out->busy = true;
      break;
    case CCORD_ACCOUNT_FINISHING:
      out->status = "Finishing sign-in";
      out->detail = "Reading your profile and guild list.";
      out->busy = true;
      break;
    case CCORD_ACCOUNT_SIGNED_IN:
    case CCORD_ACCOUNT_SIGNED_IN_TRUNCATED:
      out->signed_in = true;
      out->status = mode == CCORD_ACCOUNT_SIGNED_IN_TRUNCATED ? "Signed in (guild list truncated)" : "Signed in";
      out->detail = mode == CCORD_ACCOUNT_SIGNED_IN_TRUNCATED
                        ? "Your guild list is longer than the 200-entry limit; only the first 200 are shown."
                        : nullptr;
      out->display_name = identity->display_name;
      out->username = identity->username;
      snprintf(out->user_id_text, sizeof out->user_id_text, "%llu", (unsigned long long)identity->user_id);
      out->guild_count = identity->guild_count;
      out->guilds_truncated = identity->guilds_truncated;
      out->identity = identity;
      out->can_sign_out = true;
      out->can_refresh = true;
      out->demo_chip_visible = true;
      out->show_non_goals = true;
      break;
    case CCORD_ACCOUNT_ERROR:
      out->status = "Sign-in failed";
      out->detail = error_detail(error);
      if (!out->detail) out->detail = "Sign-in could not be completed. Try again.";
      out->can_sign_in = configured;
      out->show_non_goals = true;
      break;
  }

  if (out->show_non_goals) {
    out->non_goals = k_non_goals;
    out->non_goal_count = CCORD_ACCOUNT_NON_GOAL_COUNT;
  }
}

const char *ccord_account_mode_name(ccord_account_mode mode)
{
  switch (mode) {
    case CCORD_ACCOUNT_SIGNED_OUT: return "signed_out";
    case CCORD_ACCOUNT_UNAVAILABLE: return "unavailable";
    case CCORD_ACCOUNT_WAITING: return "waiting";
    case CCORD_ACCOUNT_FINISHING: return "finishing";
    case CCORD_ACCOUNT_SIGNED_IN: return "signed_in";
    case CCORD_ACCOUNT_SIGNED_IN_TRUNCATED: return "signed_in_truncated";
    case CCORD_ACCOUNT_ERROR: return "error";
  }
  return "error";
}

const char *ccord_account_error_name(ccord_account_error error)
{
  switch (error) {
    case CCORD_ACCOUNT_ERROR_NONE: return "NONE";
    case CCORD_ACCOUNT_ERROR_DENIED: return "DENIED";
    case CCORD_ACCOUNT_ERROR_NETWORK: return "NETWORK";
    case CCORD_ACCOUNT_ERROR_STATE_MISMATCH: return "STATE_MISMATCH";
    case CCORD_ACCOUNT_ERROR_ISS_MISMATCH: return "ISS_MISMATCH";
    case CCORD_ACCOUNT_ERROR_ATTEMPT_EXPIRED: return "ATTEMPT_EXPIRED";
    case CCORD_ACCOUNT_ERROR_TOKEN_REJECTED: return "TOKEN_REJECTED";
    case CCORD_ACCOUNT_ERROR_SCOPE_MISSING: return "SCOPE_MISSING";
    case CCORD_ACCOUNT_ERROR_RATE_LIMITED: return "RATE_LIMITED";
    case CCORD_ACCOUNT_ERROR_SERVER: return "SERVER";
    case CCORD_ACCOUNT_ERROR_BAD_RESPONSE: return "BAD_RESPONSE";
    case CCORD_ACCOUNT_ERROR_LISTENER_UNAVAILABLE: return "LISTENER_UNAVAILABLE";
    case CCORD_ACCOUNT_ERROR_CANCELLED: return "CANCELLED";
  }
  return "BAD_RESPONSE";
}
