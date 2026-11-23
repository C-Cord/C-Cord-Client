// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Account editing service (account_edit.c) + gateway.raw PII redaction (json_core.c) unit tests.
#include <ccord/core/buf.h>
#include <ccord/net/account_edit.h>
#include <ccord/net/discord_json.h>

#include <string.h>

#include "ccord_test.h"

typedef struct acct_cb_state {
  int calls;
  ccord_account_result last;
} acct_cb_state;

typedef struct tok_cb_state {
  int calls;
  char last_token[128];
} tok_cb_state;

static void on_acct(void *ud, const ccord_account_result *res)
{
  acct_cb_state *st = ud;
  st->calls++;
  st->last = *res;
}

static void on_tok_rotated(void *ud, const char *tok)
{
  tok_cb_state *st = ud;
  st->calls++;
  snprintf(st->last_token, sizeof st->last_token, "%s", tok ? tok : "");
}

static void pump_all(ccord_account_rest *r, uint64_t *now)
{
  for (int i = 0; i < 8 && ccord_account_rest_pending(r); i++) {
    *now += 20;
    ccord_account_rest_pump(r, *now);
  }
}

static void test_totp_secret_and_uri(void)
{
  ccord_error err = {0};
  char s1[33] = {0}, s2[33] = {0};
  CT_CHECK(ccord_account_generate_totp_secret(s1, &err) == CCORD_OK);
  CT_CHECK(ccord_account_generate_totp_secret(s2, &err) == CCORD_OK);
  CT_CHECK(strlen(s1) == 32 && strlen(s2) == 32);
  CT_CHECK(strcmp(s1, s2) != 0);
  for (size_t i = 0; i < 32; i++) {
    char c = s1[i];
    bool valid = (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7');
    CT_CHECK(valid);
  }
  char uri[256] = {0};
  CT_CHECK(ccord_account_totp_uri("testuser", s1, uri, sizeof uri));
  CT_CHECK(strncmp(uri, "otpauth://totp/Discord:", 23) == 0);
  CT_CHECK(strstr(uri, s1) != nullptr);
  CT_CHECK(strstr(uri, "issuer=Discord") != nullptr);
}

static void test_account_mock_operations(void)
{
  ccord_error err = {0};
  ccord_account_rest *r = ccord_account_rest_create_mock(1001ull, &err);
  CT_CHECK(r != nullptr);
  if (!r) return;

  tok_cb_state tok = {0};
  ccord_account_rest_set_token_cb(r, on_tok_rotated, &tok);
  uint64_t now = 1000;

  // 1. Username update
  acct_cb_state cb = {0};
  ccord_account_req req = {.op = CCORD_ACCT_OP_USERNAME, .value = "fresh_handle", .password = "pass1234"};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  CT_CHECK(ccord_account_rest_pending(r));
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK);
  CT_CHECK(cb.last.has_updated_handle && strcmp(cb.last.updated_handle, "fresh_handle") == 0);
  CT_CHECK(strcmp(ccord_account_rest_mock_last_path(r), "/users/@me") == 0);

  // 2. Email update + resend verify
  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_EMAIL, .value = "new@example.test", .password = "pass1234"};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK);
  CT_CHECK(cb.last.has_updated_private && strcmp(cb.last.updated_private.email, "new@example.test") == 0);

  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_RESEND_VERIFY};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK);

  // 3. Two-step Phone add -> verify -> remove
  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_PHONE_SEND, .value = "+15550100199"};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK);

  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_PHONE_VERIFY, .value = "+15550100199", .code = "123456", .password = "pass1234"};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK);
  CT_CHECK(cb.last.has_updated_private && strcmp(cb.last.updated_private.phone, "+15550100199") == 0);

  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_PHONE_REMOVE, .password = "pass1234"};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK && cb.last.http_status == 204);

  // 4. Password change triggers atomic token rotation callback
  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_PASSWORD, .password = "pass1234", .new_password = "new_strong_pw_99"};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK && cb.last.token_rotated);
  CT_CHECK(tok.calls == 1 && tok.last_token[0] != '\0');

  // 5. TOTP enable -> backup codes -> SMS toggle -> TOTP disable
  char secret[33] = {0};
  CT_CHECK(ccord_account_generate_totp_secret(secret, &err) == CCORD_OK);
  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_MFA_TOTP_ENABLE, .value = secret, .code = "654321", .password = "new_strong_pw_99"};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK && cb.last.token_rotated && cb.last.backup_code_count > 0);
  CT_CHECK(tok.calls == 2);

  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_MFA_BACKUP_CODES, .password = "new_strong_pw_99", .flag = true};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK && cb.last.backup_code_count > 0);
  CT_CHECK(cb.last.backup_codes[0].code[0] != '\0');

  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_MFA_SMS_TOGGLE, .password = "new_strong_pw_99", .flag = true};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK && cb.last.http_status == 204);

  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_MFA_TOTP_DISABLE, .code = "654321"};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK && cb.last.token_rotated);
  CT_CHECK(tok.calls == 3);

  // 6. WebAuthn list & delete, Standing fetch, Data Harvest
  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_WEBAUTHN_LIST};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK && cb.last.has_webauthn_creds && cb.last.webauthn_cred_count > 0);
  char cred_id[64];
  snprintf(cred_id, sizeof cred_id, "%s", cb.last.webauthn_creds[0].id);

  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_WEBAUTHN_DELETE, .value = cred_id};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK && strcmp(cb.last.deleted_webauthn_id, cred_id) == 0);

  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_STANDING_FETCH};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK && cb.last.has_standing && cb.last.standing.loaded);

  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_HARVEST_DATA};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK);

  ccord_account_rest_destroy(r);
}

static void test_unified_mfa_challenge_and_captcha(void)
{
  ccord_error err = {0};
  ccord_account_rest *r = ccord_account_rest_create_mock(1002ull, &err);
  CT_CHECK(r != nullptr);
  if (!r) return;
  uint64_t now = 2000;

  // 1. Trigger unified 60003 MFA challenge -> finish MFA -> original op completes!
  ccord_account_rest_mock_require_mfa_next(r, CCORD_MFA_METHOD_TOTP | CCORD_MFA_METHOD_BACKUP);
  acct_cb_state cb = {0};
  ccord_account_req req = {.op = CCORD_ACCT_OP_EMAIL, .value = "mfa@example.test", .password = "pw123456"};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status != CCORD_OK && cb.last.mfa_challenge_required);
  CT_CHECK((cb.last.mfa_methods & CCORD_MFA_METHOD_TOTP) != 0);
  CT_CHECK(ccord_account_rest_has_mfa_challenge(r));

  cb = (acct_cb_state){0};
  CT_CHECK(ccord_account_rest_finish_mfa(r, "totp", "123456", on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status == CCORD_OK);
  CT_CHECK(cb.last.has_updated_private && strcmp(cb.last.updated_private.email, "mfa@example.test") == 0);
  CT_CHECK(!ccord_account_rest_has_mfa_challenge(r));

  // 2. Cancel MFA clears stashed challenge cleanly
  ccord_account_rest_mock_require_mfa_next(r, CCORD_MFA_METHOD_TOTP);
  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_DISABLE_ACCOUNT, .password = "pw123456"};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(ccord_account_rest_has_mfa_challenge(r));
  ccord_account_rest_cancel_mfa(r);
  CT_CHECK(!ccord_account_rest_has_mfa_challenge(r));

  // 3. CAPTCHA response sets captcha_required flag
  ccord_account_rest_mock_fail_next(r, 400, "{\"captcha_key\":[\"captcha-required\"],\"captcha_service\":\"hcaptcha\"}");
  cb = (acct_cb_state){0};
  req = (ccord_account_req){.op = CCORD_ACCT_OP_USERNAME, .value = "cap_user", .password = "pw123456"};
  CT_CHECK(ccord_account_rest_submit(r, &req, on_acct, &cb, &err) == CCORD_OK);
  pump_all(r, &now);
  CT_CHECK(cb.calls == 1 && cb.last.status != CCORD_OK && cb.last.captcha_required);

  ccord_account_rest_destroy(r);
}

static void test_gateway_raw_pii_redaction(void)
{
  ccord_buf out;
  ccord_buf_init(&out, 0);

  // Sensitive events are dropped completely
  const char *blocked[] = {
    "READY", "READY_SUPPLEMENTAL", "AUTH_SESSION_CHANGE", "SESSIONS_REPLACE",
    "AUTHENTICATOR_UPDATE", "USER_REQUIRED_ACTION_UPDATE", "USER_SETTINGS_UPDATE",
    "USER_SETTINGS_PROTO_UPDATE", "SAFETY_HUB_UPDATE",
  };
  const char *sample = "{\"email\":\"secret@example.test\",\"phone\":\"+15550100199\"}";
  for (size_t i = 0; i < sizeof blocked / sizeof blocked[0]; i++) {
    out.len = 0;
    CT_CHECK(!ccord_live_gateway_redact_raw(blocked[i], sample, strlen(sample), &out));
  }

  // USER_UPDATE strips all PII fields while preserving public profile fields
  const char *uu =
      "{\"id\":\"42\",\"username\":\"alice\",\"global_name\":\"Alice\",\"avatar\":\"av1\","
      "\"email\":\"private@example.test\",\"phone\":\"+15550100199\",\"mfa_enabled\":true,"
      "\"verified\":true,\"authenticator_types\":[2],\"token\":\"REDACTED_TEST_SECRET\"}";
  out.len = 0;
  CT_CHECK(ccord_live_gateway_redact_raw("USER_UPDATE", uu, strlen(uu), &out));
  CT_CHECK(out.data != nullptr && out.len > 0);
  const char *red = (const char *)out.data;
  CT_CHECK(strstr(red, "\"username\":\"alice\"") != nullptr);
  CT_CHECK(strstr(red, "\"global_name\":\"Alice\"") != nullptr);
  CT_CHECK(strstr(red, "private@example.test") == nullptr);
  CT_CHECK(strstr(red, "+15550100199") == nullptr);
  CT_CHECK(strstr(red, "\"email\"") == nullptr);
  CT_CHECK(strstr(red, "\"phone\"") == nullptr);
  CT_CHECK(strstr(red, "mfa_enabled") == nullptr);
  CT_CHECK(strstr(red, "authenticator_types") == nullptr);
  CT_CHECK(strstr(red, "REDACTED_TEST_SECRET") == nullptr);

  // Non-sensitive events pass through untouched
  const char *msg = "{\"id\":\"99\",\"content\":\"hello\"}";
  out.len = 0;
  CT_CHECK(ccord_live_gateway_redact_raw("MESSAGE_CREATE", msg, strlen(msg), &out));
  CT_CHECK(out.data != nullptr && strcmp((const char *)out.data, msg) == 0);

  ccord_buf_free(&out);
}

int main(void)
{
  test_totp_secret_and_uri();
  test_account_mock_operations();
  test_unified_mfa_challenge_and_captcha();
  test_gateway_raw_pii_redaction();
  return CT_RESULT("test_account_edit");
}
