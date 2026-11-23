// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// RAM-only private self-account vault & governed account-editing intents.
// Private PII (email, phone, 2FA status, standing, passkeys) lives exclusively here on ccord_session
// and is never stored in ccord_store or warm caches.
#include "session_internal.h"

#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>

static void sess_wipe(void *p, size_t n)
{
  if (!p || n == 0) return;
  volatile unsigned char *v = (volatile unsigned char *)p;
  while (n--) *v++ = 0;
}

void sess_account_wipe_vault(ccord_session *s)
{
  if (!s) return;
  sess_wipe(&s->account_private, sizeof s->account_private);
  sess_wipe(&s->account_standing, sizeof s->account_standing);
  sess_wipe(s->webauthn_creds, sizeof s->webauthn_creds);
  s->webauthn_cred_count = 0;
  s->webauthn_loaded = false;
  s->account_revision++;
}

void sess_account_apply_private(ccord_session *s, const ccord_account_private *priv)
{
  if (!s || !priv || !priv->valid) return;
  s->account_private = *priv;
  s->account_revision++;
}

ccord_status ccord_session_set_account_rest(ccord_session *s, ccord_account_rest *rest)
{
  if (!s) return CCORD_ERR_INVALID;
  if (s->account_rest == rest) return CCORD_OK;
  ccord_account_rest_destroy(s->account_rest);
  s->account_rest = rest;
  return CCORD_OK;
}

bool ccord_session_has_account_rest(const ccord_session *s)
{
  return s && s->account_rest != nullptr;
}

const ccord_account_private *ccord_session_account_private(const ccord_session *s)
{
  return (s && s->account_private.valid) ? &s->account_private : nullptr;
}

const ccord_account_standing *ccord_session_account_standing(const ccord_session *s)
{
  return s ? &s->account_standing : nullptr;
}

const ccord_webauthn_cred *ccord_session_account_webauthn(const ccord_session *s, size_t *out_count, bool *out_loaded)
{
  if (out_count) *out_count = s ? s->webauthn_cred_count : 0;
  if (out_loaded) *out_loaded = s && s->webauthn_loaded;
  return (s && s->webauthn_cred_count > 0) ? s->webauthn_creds : nullptr;
}

uint64_t ccord_session_account_revision(const ccord_session *s)
{
  return s ? s->account_revision : 0;
}

bool ccord_session_account_has_mfa_challenge(const ccord_session *s)
{
  return s && s->account_rest && ccord_account_rest_has_mfa_challenge(s->account_rest);
}

typedef struct session_acct_ctx {
  ccord_session *session;
  ccord_account_op_kind op;
  bool flag;
  ccord_account_cb cb;
  void *ud;
} session_acct_ctx;

static void session_acct_apply_result(ccord_session *s, const ccord_account_result *res, bool flag)
{
  if (!s || !res || res->status != CCORD_OK) return;
  bool changed = false;

  if (res->has_updated_private && res->updated_private.valid) {
    if (res->op == CCORD_ACCT_OP_PHONE_VERIFY && s->account_private.valid) {
      // Step 2 phone verification returns 204; preserve existing email/MFA while updating phone.
      memcpy(s->account_private.phone, res->updated_private.phone, sizeof s->account_private.phone);
    } else {
      s->account_private = res->updated_private;
    }
    changed = true;
  }

  if (res->has_updated_handle && res->updated_handle[0] != '\0') {
    ccord_self_profile_update u = {
      .set_handle = true,
      .handle = res->updated_handle,
    };
    ccord_error err;
    if (ccord_store_set_self_profile(s->store, &u, &err) != CCORD_OK) {
      CCORD_LOG_W("session: failed to sync updated username: %s", err.message);
    }
    changed = true;
  }

  switch (res->op) {
    case CCORD_ACCT_OP_PHONE_REMOVE:
      if (s->account_private.valid) {
        sess_wipe(s->account_private.phone, sizeof s->account_private.phone);
        s->account_private.sms_mfa_enabled = false;
        s->account_private.authenticator_types &= ~CCORD_AUTH_TYPE_SMS;
        changed = true;
      }
      break;
    case CCORD_ACCT_OP_MFA_TOTP_ENABLE:
      s->account_private.valid = true;
      s->account_private.mfa_enabled = true;
      s->account_private.authenticator_types |= CCORD_AUTH_TYPE_TOTP;
      changed = true;
      break;
    case CCORD_ACCT_OP_MFA_TOTP_DISABLE:
      if (s->account_private.valid) {
        s->account_private.authenticator_types &= ~(CCORD_AUTH_TYPE_TOTP | CCORD_AUTH_TYPE_SMS);
        s->account_private.sms_mfa_enabled = false;
        s->account_private.mfa_enabled = (s->account_private.authenticator_types != 0);
        changed = true;
      }
      break;
    case CCORD_ACCT_OP_MFA_SMS_TOGGLE:
      if (s->account_private.valid) {
        s->account_private.sms_mfa_enabled = flag;
        if (flag) s->account_private.authenticator_types |= CCORD_AUTH_TYPE_SMS;
        else s->account_private.authenticator_types &= ~CCORD_AUTH_TYPE_SMS;
        changed = true;
      }
      break;
    case CCORD_ACCT_OP_WEBAUTHN_DELETE:
      if (res->deleted_webauthn_id[0] != '\0') {
        for (size_t i = 0; i < s->webauthn_cred_count; i++) {
          if (strcmp(s->webauthn_creds[i].id, res->deleted_webauthn_id) == 0) {
            for (size_t k = i + 1u; k < s->webauthn_cred_count; k++) {
              s->webauthn_creds[k - 1u] = s->webauthn_creds[k];
            }
            s->webauthn_cred_count--;
            if (s->webauthn_cred_count == 0 && s->account_private.valid) {
              s->account_private.authenticator_types &= ~CCORD_AUTH_TYPE_WEBAUTHN;
              s->account_private.mfa_enabled = (s->account_private.authenticator_types != 0);
            }
            changed = true;
            break;
          }
        }
      }
      break;
    default:
      break;
  }

  if (res->has_standing) {
    s->account_standing = res->standing;
    changed = true;
  }

  if (res->has_webauthn_creds) {
    size_t n = res->webauthn_cred_count;
    if (n > CCORD_WEBAUTHN_MAX_CREDS) n = CCORD_WEBAUTHN_MAX_CREDS;
    if (n > 0) memcpy(s->webauthn_creds, res->webauthn_creds, n * sizeof s->webauthn_creds[0]);
    s->webauthn_cred_count = n;
    s->webauthn_loaded = true;
    changed = true;
  }

  if (changed) s->account_revision++;
}

static void session_acct_done(void *ud, const ccord_account_result *res)
{
  session_acct_ctx *ctx = ud;
  session_acct_apply_result(ctx->session, res, ctx->flag);
  if (ctx->cb) ctx->cb(ctx->ud, res);
  ccord_free(ctx);
}

ccord_status ccord_session_account_submit(ccord_session *s, const ccord_account_req *req,
                                          ccord_account_cb cb, void *ud, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !req) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (!s->account_rest) return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "session: account editing is not available");
  session_acct_ctx *ctx = ccord_calloc(1, sizeof *ctx);
  if (!ctx) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
  ctx->session = s;
  ctx->op = req->op;
  ctx->flag = req->flag;
  ctx->cb = cb;
  ctx->ud = ud;
  ccord_status st = ccord_account_rest_submit(s->account_rest, req, session_acct_done, ctx, err);
  if (st != CCORD_OK) ccord_free(ctx);
  return st;
}

ccord_status ccord_session_account_finish_mfa(ccord_session *s, const char *mfa_type, const char *mfa_data,
                                              ccord_account_cb cb, void *ud, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!s->account_rest) return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "session: account editing is not available");
  session_acct_ctx *ctx = ccord_calloc(1, sizeof *ctx);
  if (!ctx) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
  ctx->session = s;
  ctx->cb = cb;
  ctx->ud = ud;
  ccord_status st = ccord_account_rest_finish_mfa(s->account_rest, mfa_type, mfa_data, session_acct_done, ctx, err);
  if (st != CCORD_OK) ccord_free(ctx);
  return st;
}

void ccord_session_account_cancel_mfa(ccord_session *s)
{
  if (s && s->account_rest) ccord_account_rest_cancel_mfa(s->account_rest);
}

ccord_status ccord_session_account_fetch_standing(ccord_session *s, ccord_account_cb cb, void *ud, ccord_error *err)
{
  ccord_account_req req = {.op = CCORD_ACCT_OP_STANDING_FETCH};
  return ccord_session_account_submit(s, &req, cb, ud, err);
}

ccord_status ccord_session_account_fetch_webauthn(ccord_session *s, ccord_account_cb cb, void *ud, ccord_error *err)
{
  ccord_account_req req = {.op = CCORD_ACCT_OP_WEBAUTHN_LIST};
  return ccord_session_account_submit(s, &req, cb, ud, err);
}
