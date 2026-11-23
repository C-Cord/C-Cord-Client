// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "app_profile_rest.h"

#if CCORD_LIVE_ACCOUNT
#include <ccord/net/live_transport.h>
#endif
#include <ccord/net/account_edit.h>
#include <ccord/net/profile_edit.h>

void ccord_app_account_rest_detach(ccord_session *s)
{
  if (!s) return;
  (void)ccord_session_set_account_rest(s, nullptr);
}

void ccord_app_profile_rest_detach(ccord_session *s)
{
  if (!s) return;
  (void)ccord_session_set_profile_rest(s, nullptr);
  (void)ccord_session_set_account_rest(s, nullptr);
}

bool ccord_app_profile_rest_attach_mock(ccord_session *s, ccord_id self_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) {
    ccord_error_set(err, CCORD_ERR_INVALID, "profile: no session");
    return false;
  }
  ccord_profile_rest *r = ccord_profile_rest_create_mock(self_id, err);
  if (!r) return false;
  if (ccord_session_set_profile_rest(s, r) != CCORD_OK) {
    ccord_profile_rest_destroy(r);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "profile: session refused the backend");
    return false;
  }
  return true;
}

bool ccord_app_profile_rest_attach_live(ccord_session *s, const char *token_file, const char *api_base,
                                        ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) {
    ccord_error_set(err, CCORD_ERR_INVALID, "profile: no session");
    return false;
  }
  ccord_app_profile_rest_detach(s);   // never keep the previous account's token alive across a failure
#if CCORD_LIVE_ACCOUNT
  char token[CCORD_LIVE_TOKEN_MAX + 1];
  if (ccord_live_load_token(token_file, token, sizeof token, err) != CCORD_OK) {
    ccord_live_wipe(token, sizeof token);
    return false;
  }
  ccord_profile_rest *r = ccord_profile_rest_create_live(api_base, token, err);   // copies the token
  ccord_live_wipe(token, sizeof token);
  if (!r) return false;
  if (ccord_session_set_profile_rest(s, r) != CCORD_OK) {
    ccord_profile_rest_destroy(r);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "profile: session refused the backend");
    return false;
  }
  return true;
#else
  (void)token_file;
  (void)api_base;
  ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "profile: this build has no live account support");
  return false;
#endif
}

bool ccord_app_account_rest_attach_mock(ccord_session *s, ccord_id self_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) {
    ccord_error_set(err, CCORD_ERR_INVALID, "account: no session");
    return false;
  }
  ccord_account_rest *r = ccord_account_rest_create_mock(self_id, err);
  if (!r) return false;
  if (ccord_session_set_account_rest(s, r) != CCORD_OK) {
    ccord_account_rest_destroy(r);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "account: session refused the backend");
    return false;
  }
  return true;
}

bool ccord_app_account_rest_attach_live(ccord_session *s, const char *token_file, const char *api_base,
                                        ccord_account_token_cb token_cb, void *token_ud, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) {
    ccord_error_set(err, CCORD_ERR_INVALID, "account: no session");
    return false;
  }
  ccord_app_account_rest_detach(s);
#if CCORD_LIVE_ACCOUNT
  char token[CCORD_LIVE_TOKEN_MAX + 1];
  if (ccord_live_load_token(token_file, token, sizeof token, err) != CCORD_OK) {
    ccord_live_wipe(token, sizeof token);
    return false;
  }
  ccord_account_rest *r = ccord_account_rest_create_live(api_base, token, err);
  ccord_live_wipe(token, sizeof token);
  if (!r) return false;
  ccord_account_rest_set_token_cb(r, token_cb, token_ud);
  if (ccord_session_set_account_rest(s, r) != CCORD_OK) {
    ccord_account_rest_destroy(r);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "account: session refused the backend");
    return false;
  }
  return true;
#else
  (void)token_file; (void)api_base; (void)token_cb; (void)token_ud;
  ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "account: this build has no live account support");
  return false;
#endif
}
