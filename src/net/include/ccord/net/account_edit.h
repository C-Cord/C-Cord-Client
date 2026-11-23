// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Authenticated Discord account editing service (username, email, phone, password, 2FA/TOTP,
// backup codes, SMS MFA, WebAuthn keys, Account Standing, disable/delete, data harvest).
//
// Security guarantees:
// - Every request body and response buffer is marked sensitive and volatile-wiped on completion/destroy.
// - Unified HTTP 401/403 code 60003 MFA challenge flow: stashes the failed operation in RAM, completes
//   POST /mfa/finish, replays once with X-Discord-MFA-Authorization, and immediately wipes all secrets.
// - Rotated account tokens returned by Discord (password change, TOTP enable/disable) are passed once
//   to the app-owned token callback and immediately volatile-wiped from this module's buffers.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/model/account.h>

typedef enum ccord_account_op_kind : uint8_t {
  CCORD_ACCT_OP_NONE = 0,
  CCORD_ACCT_OP_USERNAME,           // PATCH /users/@me {username, password}
  CCORD_ACCT_OP_EMAIL,              // PATCH /users/@me {email, password}
  CCORD_ACCT_OP_RESEND_VERIFY,      // POST  /auth/verify/resend {}
  CCORD_ACCT_OP_PHONE_SEND,         // POST  /users/@me/phone {phone, change_phone_reason}
  CCORD_ACCT_OP_PHONE_VERIFY,       // POST  /phone-verifications/verify -> POST /users/@me/phone
  CCORD_ACCT_OP_PHONE_REMOVE,       // DELETE /users/@me/phone {password}
  CCORD_ACCT_OP_PASSWORD,           // PATCH /users/@me {password, new_password} (rotates token)
  CCORD_ACCT_OP_MFA_TOTP_ENABLE,    // POST  /users/@me/mfa/totp/enable {secret, code, password}
  CCORD_ACCT_OP_MFA_TOTP_DISABLE,   // POST  /users/@me/mfa/totp/disable {code}
  CCORD_ACCT_OP_MFA_BACKUP_CODES,   // POST  /users/@me/mfa/codes-verification {password, regenerate}
  CCORD_ACCT_OP_MFA_SMS_TOGGLE,     // POST  /users/@me/mfa/sms/{enable|disable} {password}
  CCORD_ACCT_OP_WEBAUTHN_LIST,      // GET   /users/@me/mfa/webauthn/credentials
  CCORD_ACCT_OP_WEBAUTHN_DELETE,    // DELETE /users/@me/mfa/webauthn/credentials/{id}
  CCORD_ACCT_OP_STANDING_FETCH,     // GET   /safety-hub/@me
  CCORD_ACCT_OP_DISABLE_ACCOUNT,    // POST  /users/@me/disable {password}
  CCORD_ACCT_OP_DELETE_ACCOUNT,     // POST  /users/@me/delete {password}
  CCORD_ACCT_OP_HARVEST_DATA,       // POST  /users/@me/harvest {}
} ccord_account_op_kind;

typedef enum ccord_account_mfa_method : uint32_t {
  CCORD_MFA_METHOD_TOTP     = 1u << 0,
  CCORD_MFA_METHOD_BACKUP   = 1u << 1,
  CCORD_MFA_METHOD_SMS      = 1u << 2,
  CCORD_MFA_METHOD_PASSWORD = 1u << 3,
  CCORD_MFA_METHOD_WEBAUTHN = 1u << 4,
} ccord_account_mfa_method;

#define CCORD_ACCOUNT_MAX_BACKUP_CODES 12u

typedef struct ccord_account_backup_code {
  char code[16];
  bool consumed;
} ccord_account_backup_code;

// Caller fills relevant fields, submits via ccord_account_rest_submit(), and immediately wipes its stack copy.
typedef struct ccord_account_req {
  ccord_account_op_kind op;
  const char *value;                // username, email, phone (E.164), webauthn cred id, or TOTP base32 secret
  const char *password;             // current account password
  const char *new_password;         // CCORD_ACCT_OP_PASSWORD
  const char *code;                 // 6-digit TOTP/SMS code or 8-char backup code
  bool flag;                        // regenerate (MFA_BACKUP_CODES) or enable (MFA_SMS_TOGGLE)
} ccord_account_req;

typedef struct ccord_account_result {
  ccord_account_op_kind op;
  ccord_status status;
  long http_status;
  int32_t code;                     // Discord error code (e.g. 60003, 50018, 50035)
  char message[200];
  bool rate_limited;
  double retry_after_s;
  bool captcha_required;
  bool mfa_challenge_required;      // true on code 60003: call ccord_account_rest_finish_mfa or _cancel_mfa
  char mfa_ticket[256];
  uint32_t mfa_methods;             // bitmask of ccord_account_mfa_method
  bool token_rotated;               // true when Discord returned a rotated token (delivered to token_cb)
  bool has_updated_private;
  ccord_account_private updated_private;
  bool has_updated_handle;
  char updated_handle[CCORD_NAME_MAX_BYTES + 1];
  size_t backup_code_count;
  ccord_account_backup_code backup_codes[CCORD_ACCOUNT_MAX_BACKUP_CODES];
  bool has_standing;
  ccord_account_standing standing;
  bool has_webauthn_creds;
  size_t webauthn_cred_count;
  ccord_webauthn_cred webauthn_creds[CCORD_WEBAUTHN_MAX_CREDS];
  char deleted_webauthn_id[64];
} ccord_account_result;

typedef void (*ccord_account_cb)(void *ud, const ccord_account_result *res);
typedef void (*ccord_account_token_cb)(void *ud, const char *new_token);

typedef struct ccord_account_rest ccord_account_rest;

[[nodiscard]] ccord_account_rest *ccord_account_rest_create_live(const char *base_url, const char *token,
                                                                 ccord_error *err);
[[nodiscard]] ccord_account_rest *ccord_account_rest_create_mock(ccord_id self_id, ccord_error *err);
void ccord_account_rest_destroy(ccord_account_rest *r);

void ccord_account_rest_set_token(ccord_account_rest *r, const char *token);
void ccord_account_rest_set_token_cb(ccord_account_rest *r, ccord_account_token_cb cb, void *ud);
void ccord_account_rest_pump(ccord_account_rest *r, uint64_t now_ms);
[[nodiscard]] bool ccord_account_rest_pending(const ccord_account_rest *r);
[[nodiscard]] bool ccord_account_rest_has_mfa_challenge(const ccord_account_rest *r);

// Validates and submits an account operation. Returns CCORD_OK once queued (cb runs asynchronously from pump).
[[nodiscard]] ccord_status ccord_account_rest_submit(ccord_account_rest *r, const ccord_account_req *req,
                                                     ccord_account_cb cb, void *ud, ccord_error *err);

// Completes a pending code 60003 MFA challenge: sends POST /mfa/finish, then replays the original operation
// with X-Discord-MFA-Authorization. `mfa_type` is "totp", "backup", "sms", or "password".
[[nodiscard]] ccord_status ccord_account_rest_finish_mfa(ccord_account_rest *r, const char *mfa_type,
                                                         const char *mfa_data, ccord_account_cb cb, void *ud,
                                                         ccord_error *err);
void ccord_account_rest_cancel_mfa(ccord_account_rest *r);

// Generates a 160-bit (32-char RFC 4648 Base32) TOTP secret using OS CSPRNG.
[[nodiscard]] ccord_status ccord_account_generate_totp_secret(char out_b32[33], ccord_error *err);
// Builds an otpauth://totp/Discord:<label>?secret=<secret_b32>&issuer=Discord URI.
[[nodiscard]] bool ccord_account_totp_uri(const char *label, const char *secret_b32, char *out, size_t cap);

// Mock test helpers (no-op on live backend).
void ccord_account_rest_mock_require_mfa_next(ccord_account_rest *r, uint32_t methods_mask);
void ccord_account_rest_mock_fail_next(ccord_account_rest *r, long http_status, const char *json_body);
[[nodiscard]] size_t ccord_account_rest_mock_count(const ccord_account_rest *r);
[[nodiscard]] const char *ccord_account_rest_mock_last_path(const ccord_account_rest *r);
