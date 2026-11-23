// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Authenticated Discord account editing service. See account_edit.h.
#include <ccord/net/account_edit.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/random.h>
#include <ccord/core/utf8.h>

#ifdef CCORD_PROFILE_LIVE
#include <ccord/net/live_transport.h>
#endif

static void acct_wipe(void *p, size_t n)
{
  if (!p || n == 0) return;
  volatile unsigned char *v = (volatile unsigned char *)p;
  while (n--) *v++ = 0;
}

static void acct_wipe_str(char *s)
{
  if (s) acct_wipe(s, strlen(s));
}

static void acct_copy_str(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t len = strlen(src);
  if (len >= cap) len = cap - 1u;
  while (len > 0 && (((unsigned char)src[len] & 0xC0u) == 0x80u)) len--;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static int acct_jstr(const ccord_json *obj, const char *key, char *dst, size_t cap)
{
  const ccord_json *v = obj ? ccord_json_get(obj, key) : nullptr;
  if (!v) return 0;
  if (ccord_json_type_of(v) == CCORD_JSON_NULL) {
    if (dst && cap > 0) dst[0] = '\0';
    return 2;
  }
  size_t n = 0;
  const char *s = ccord_json_string(v, &n);
  if (!s) return 0;
  if (dst && cap > 0) {
    if (n >= cap) n = cap - 1u;
    while (n > 0 && (((unsigned char)s[n] & 0xC0u) == 0x80u)) n--;
    memcpy(dst, s, n);
    dst[n] = '\0';
  }
  return 1;
}

static bool acct_jbool(const ccord_json *obj, const char *key, bool def)
{
  const ccord_json *v = obj ? ccord_json_get(obj, key) : nullptr;
  if (!v || ccord_json_type_of(v) != CCORD_JSON_BOOL) return def;
  return ccord_json_bool(v, def);
}

static int64_t acct_ji64(const ccord_json *obj, const char *key, int64_t def)
{
  const ccord_json *v = obj ? ccord_json_get(obj, key) : nullptr;
  int64_t out = def;
  if (v && ccord_json_int64(v, &out)) return out;
  return def;
}

// ---- TOTP secret & otpauth URI helpers -------------------------------------------------------------------------

ccord_status ccord_account_generate_totp_secret(char out_b32[33], ccord_error *err)
{
  ccord_error_clear(err);
  if (!out_b32) return ccord_error_set(err, CCORD_ERR_INVALID, "account: null secret buffer");
  static const char k_b32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  uint8_t raw[20];
  ccord_status st = ccord_random_bytes(raw, sizeof raw);
  if (st != CCORD_OK) {
    acct_wipe(raw, sizeof raw);
    return ccord_error_set(err, st, "account: failed to generate random TOTP secret");
  }
  // Encode 20 bytes (160 bits) -> 32 Base32 characters (exact multiple of 5 bits).
  size_t out_i = 0;
  uint32_t acc = 0;
  unsigned bits = 0;
  for (size_t i = 0; i < sizeof raw; i++) {
    acc = (acc << 8) | raw[i];
    bits += 8u;
    while (bits >= 5u) {
      bits -= 5u;
      out_b32[out_i++] = k_b32[(acc >> bits) & 31u];
    }
  }
  out_b32[32] = '\0';
  acct_wipe(raw, sizeof raw);
  return CCORD_OK;
}

bool ccord_account_totp_uri(const char *label, const char *secret_b32, char *out, size_t cap)
{
  if (!out || cap == 0 || !secret_b32 || !secret_b32[0]) return false;
  const char *who = (label && label[0]) ? label : "Account";
  char safe_label[96];
  size_t w = 0;
  static const char hex[] = "0123456789ABCDEF";
  for (const unsigned char *p = (const unsigned char *)who; *p && w + 4u < sizeof safe_label; p++) {
    bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_' || *p == '.' || *p == '@';
    if (ok) {
      safe_label[w++] = (char)*p;
    } else {
      safe_label[w++] = '%';
      safe_label[w++] = hex[*p >> 4];
      safe_label[w++] = hex[*p & 0x0Fu];
    }
  }
  safe_label[w] = '\0';
  int n = snprintf(out, cap, "otpauth://totp/Discord:%s?secret=%s&issuer=Discord", safe_label, secret_b32);
  if (n < 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return false;
  }
  return true;
}

// ---- internal HTTP method enum & pending challenge stash -------------------------------------------------------

typedef enum acct_http_method : uint8_t {
  ACCT_GET = 0,
  ACCT_POST,
  ACCT_PATCH,
  ACCT_DELETE,
} acct_http_method;

typedef void (*acct_http_cb)(void *ud, ccord_status st, long http, const char *body, size_t len);

#define ACCT_MOCK_MAX_PENDING 8u

typedef struct acct_mock_pending {
  bool used;
  long http;
  char *body;
  acct_http_cb cb;
  void *ud;
} acct_mock_pending;

typedef struct acct_mock_state {
  ccord_id self_id;
  char username[CCORD_NAME_MAX_BYTES + 1];
  ccord_account_private priv;
  ccord_account_standing standing;
  ccord_webauthn_cred creds[CCORD_WEBAUTHN_MAX_CREDS];
  size_t cred_count;
  ccord_account_backup_code backup_codes[CCORD_ACCOUNT_MAX_BACKUP_CODES];
  size_t backup_code_count;
  unsigned code_gen;
  size_t count;
  char last_path[96];
  bool fail_armed;
  long fail_http;
  char *fail_body;
  bool mfa_next;
  uint32_t mfa_next_methods;
  size_t pending_count;
  acct_mock_pending pending[ACCT_MOCK_MAX_PENDING];
} acct_mock_state;

typedef struct acct_mfa_stash {
  bool active;
  ccord_account_op_kind op;
  acct_http_method method;
  char path[128];
  char *body;
  size_t body_len;
  char ticket[256];
  uint32_t methods;
  char deleted_id[64];
} acct_mfa_stash;

struct ccord_account_rest {
  bool is_mock;
  acct_mock_state *mock;
#ifdef CCORD_PROFILE_LIVE
  ccord_rest_queue *queue;
#endif
  ccord_account_token_cb token_cb;
  void *token_ud;
  acct_mfa_stash mfa;
};

static void acct_mfa_clear(acct_mfa_stash *m)
{
  if (!m) return;
  if (m->body) {
    acct_wipe(m->body, m->body_len);
    ccord_free(m->body);
  }
  acct_wipe(m, sizeof *m);
}

static void acct_mock_init_codes(acct_mock_state *m)
{
  m->code_gen++;
  m->backup_code_count = 10u;
  for (size_t i = 0; i < m->backup_code_count; i++) {
    (void)snprintf(m->backup_codes[i].code, sizeof m->backup_codes[i].code, "%04u-%04u",
                   (unsigned)(1000u + (m->code_gen * 137u + i * 71u) % 8999u),
                   (unsigned)(1000u + (m->code_gen * 251u + i * 97u) % 8999u));
    m->backup_codes[i].consumed = (i == 0 && m->code_gen == 1);
  }
}

#ifdef CCORD_PROFILE_LIVE
ccord_account_rest *ccord_account_rest_create_live(const char *base_url, const char *token, ccord_error *err)
{
  ccord_error_clear(err);
  if (!token || token[0] == '\0') {
    ccord_error_set(err, CCORD_ERR_INVALID, "account: no token");
    return nullptr;
  }
  ccord_account_rest *r = ccord_calloc(1, sizeof *r);
  if (!r) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "account: out of memory");
    return nullptr;
  }
  ccord_rest_config cfg = {.base_url = base_url, .token = token, .timeout_ms = 20000u};
  r->queue = ccord_rest_queue_create(&cfg, err);
  if (!r->queue) {
    ccord_free(r);
    return nullptr;
  }
  return r;
}
#else
ccord_account_rest *ccord_account_rest_create_live(const char *base_url, const char *token, ccord_error *err)
{
  (void)base_url;
  (void)token;
  ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "account: this build has no live transport");
  return nullptr;
}
#endif

ccord_account_rest *ccord_account_rest_create_mock(ccord_id self_id, ccord_error *err)
{
  ccord_error_clear(err);
  ccord_account_rest *r = ccord_calloc(1, sizeof *r);
  acct_mock_state *m = ccord_calloc(1, sizeof *m);
  if (!r || !m) {
    ccord_free(r);
    ccord_free(m);
    ccord_error_set(err, CCORD_ERR_NOMEM, "account: out of memory");
    return nullptr;
  }
  m->self_id = self_id;
  acct_copy_str(m->username, sizeof m->username, "you");
  m->priv.valid = true;
  acct_copy_str(m->priv.email, sizeof m->priv.email, "you@example.test");
  acct_copy_str(m->priv.phone, sizeof m->priv.phone, "+15550100199");
  m->priv.verified = true;
  m->priv.mfa_enabled = true;
  m->priv.sms_mfa_enabled = false;
  m->priv.authenticator_types = CCORD_AUTH_TYPE_TOTP | CCORD_AUTH_TYPE_WEBAUTHN;
  m->priv.nsfw_allowed = true;
  m->priv.premium_type = 2u;
  m->standing.loaded = true;
  m->standing.tier = CCORD_STANDING_ALL_GOOD;
  m->standing.violation_count = 0;
  m->cred_count = 2u;
  acct_copy_str(m->creds[0].id, sizeof m->creds[0].id, "cred_yubikey_01");
  acct_copy_str(m->creds[0].name, sizeof m->creds[0].name, "YubiKey 5C NFC");
  m->creds[0].created_at_ms = 1759276800000ll;
  acct_copy_str(m->creds[1].id, sizeof m->creds[1].id, "cred_platform_02");
  acct_copy_str(m->creds[1].name, sizeof m->creds[1].name, "Linux Passkey");
  m->creds[1].created_at_ms = 1764547200000ll;
  acct_mock_init_codes(m);
  r->is_mock = true;
  r->mock = m;
  return r;
}

void ccord_account_rest_set_token(ccord_account_rest *r, const char *token)
{
  if (!r || r->is_mock) return;
#ifdef CCORD_PROFILE_LIVE
  if (r->queue) ccord_rest_queue_set_token(r->queue, token);
#else
  (void)token;
#endif
}

void ccord_account_rest_set_token_cb(ccord_account_rest *r, ccord_account_token_cb cb, void *ud)
{
  if (!r) return;
  r->token_cb = cb;
  r->token_ud = ud;
}

void ccord_account_rest_mock_require_mfa_next(ccord_account_rest *r, uint32_t methods_mask)
{
  if (!r || !r->is_mock) return;
  r->mock->mfa_next = true;
  r->mock->mfa_next_methods = methods_mask ? methods_mask : (CCORD_MFA_METHOD_TOTP | CCORD_MFA_METHOD_BACKUP | CCORD_MFA_METHOD_PASSWORD);
}

void ccord_account_rest_mock_fail_next(ccord_account_rest *r, long http_status, const char *json_body)
{
  if (!r || !r->is_mock) return;
  ccord_free(r->mock->fail_body);
  r->mock->fail_body = ccord_strdup(json_body ? json_body : "{}");
  r->mock->fail_http = http_status;
  r->mock->fail_armed = r->mock->fail_body != nullptr;
}

size_t ccord_account_rest_mock_count(const ccord_account_rest *r)
{
  return (r && r->is_mock) ? r->mock->count : 0;
}

const char *ccord_account_rest_mock_last_path(const ccord_account_rest *r)
{
  return (r && r->is_mock) ? r->mock->last_path : "";
}

bool ccord_account_rest_has_mfa_challenge(const ccord_account_rest *r)
{
  return r && r->mfa.active;
}

void ccord_account_rest_cancel_mfa(ccord_account_rest *r)
{
  if (!r) return;
  acct_mfa_clear(&r->mfa);
}

// ---- JSON response parsing -------------------------------------------------------------------------------------

static ccord_status status_for_http(long http)
{
  if (http == 429) return CCORD_ERR_RANGE;
  if (http == 401 || http == 403) return CCORD_ERR_PERMISSION;
  if (http >= 500) return CCORD_ERR_IO;
  return CCORD_ERR_INVALID;
}

static void parse_user_private(const ccord_json *j, ccord_account_result *res)
{
  bool any = ccord_json_get(j, "email") != nullptr ||
             ccord_json_get(j, "phone") != nullptr ||
             ccord_json_get(j, "verified") != nullptr ||
             ccord_json_get(j, "mfa_enabled") != nullptr ||
             ccord_json_get(j, "authenticator_types") != nullptr ||
             ccord_json_get(j, "nsfw_allowed") != nullptr ||
             ccord_json_get(j, "premium_type") != nullptr;
  if (any) {
    ccord_account_private *p = &res->updated_private;
    memset(p, 0, sizeof *p);
    p->valid = true;
    (void)acct_jstr(j, "email", p->email, sizeof p->email);
    (void)acct_jstr(j, "phone", p->phone, sizeof p->phone);
    p->verified = acct_jbool(j, "verified", false);
    p->mfa_enabled = acct_jbool(j, "mfa_enabled", false);
    p->nsfw_allowed = acct_jbool(j, "nsfw_allowed", false);
    int64_t pt = acct_ji64(j, "premium_type", 0);
    if (pt < 0) pt = 0;
    if (pt > 3) pt = 3;
    p->premium_type = (uint8_t)pt;
    const ccord_json *at = ccord_json_get(j, "authenticator_types");
    size_t nat = at && ccord_json_type_of(at) == CCORD_JSON_ARRAY ? ccord_json_array_len(at) : 0;
    for (size_t i = 0; i < nat; i++) {
      int64_t v = 0;
      if (!ccord_json_int64(ccord_json_array_at(at, i), &v)) continue;
      if (v == 1) p->authenticator_types |= CCORD_AUTH_TYPE_WEBAUTHN;
      else if (v == 2) p->authenticator_types |= CCORD_AUTH_TYPE_TOTP;
      else if (v == 3) {
        p->authenticator_types |= CCORD_AUTH_TYPE_SMS;
        p->sms_mfa_enabled = true;
      }
    }
    if (p->mfa_enabled && p->authenticator_types == 0) p->authenticator_types = CCORD_AUTH_TYPE_TOTP;
    res->has_updated_private = true;
  }
  if (acct_jstr(j, "username", res->updated_handle, sizeof res->updated_handle) == 1 && res->updated_handle[0]) {
    res->has_updated_handle = true;
  }
}

static void parse_backup_codes(const ccord_json *j, ccord_account_result *res)
{
  const ccord_json *arr = ccord_json_get(j, "backup_codes");
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) return;
  size_t n = ccord_json_array_len(arr);
  if (n > CCORD_ACCOUNT_MAX_BACKUP_CODES) n = CCORD_ACCOUNT_MAX_BACKUP_CODES;
  size_t out_n = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *it = ccord_json_array_at(arr, i);
    if (!it || ccord_json_type_of(it) != CCORD_JSON_OBJECT) continue;
    char code[16] = "";
    if (acct_jstr(it, "code", code, sizeof code) == 1 && code[0]) {
      acct_copy_str(res->backup_codes[out_n].code, sizeof res->backup_codes[out_n].code, code);
      res->backup_codes[out_n].consumed = acct_jbool(it, "consumed", false);
      out_n++;
    }
  }
  res->backup_code_count = out_n;
}

static void parse_standing(const ccord_json *j, ccord_account_result *res)
{
  ccord_account_standing *st = &res->standing;
  memset(st, 0, sizeof *st);
  st->loaded = true;
  st->tier = CCORD_STANDING_ALL_GOOD;
  const ccord_json *as = ccord_json_get(j, "account_standing");
  if (as && ccord_json_type_of(as) == CCORD_JSON_OBJECT) {
    int64_t code = acct_ji64(as, "state", 100);
    if (code >= 500) st->tier = CCORD_STANDING_SUSPENDED;
    else if (code >= 400) st->tier = CCORD_STANDING_AT_RISK;
    else if (code >= 300) st->tier = CCORD_STANDING_VERY_LIM;
    else if (code >= 200) st->tier = CCORD_STANDING_LIMITED;
    else st->tier = CCORD_STANDING_ALL_GOOD;
  }
  const ccord_json *cls = ccord_json_get(j, "classifications");
  if (cls && ccord_json_type_of(cls) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(cls);
    for (size_t i = 0; i < n && st->violation_count < CCORD_STANDING_MAX_VIOLATIONS; i++) {
      const ccord_json *it = ccord_json_array_at(cls, i);
      if (!it || ccord_json_type_of(it) != CCORD_JSON_OBJECT) continue;
      ccord_standing_violation *v = &st->violations[st->violation_count];
      (void)acct_jstr(it, "id", v->id, sizeof v->id);
      if (acct_jstr(it, "description", v->title, sizeof v->title) != 1 || !v->title[0]) {
        acct_copy_str(v->title, sizeof v->title, "Account Policy Notice");
      }
      (void)acct_jstr(it, "detail", v->detail, sizeof v->detail);
      v->expires_at_ms = acct_ji64(it, "expires_at_ms", 0);
      st->violation_count++;
    }
  }
  res->has_standing = true;
}

static void parse_webauthn_list(const ccord_json *j, ccord_account_result *res)
{
  res->has_webauthn_creds = true;
  res->webauthn_cred_count = 0;
  if (!j || ccord_json_type_of(j) != CCORD_JSON_ARRAY) return;
  size_t n = ccord_json_array_len(j);
  for (size_t i = 0; i < n && res->webauthn_cred_count < CCORD_WEBAUTHN_MAX_CREDS; i++) {
    const ccord_json *it = ccord_json_array_at(j, i);
    if (!it || ccord_json_type_of(it) != CCORD_JSON_OBJECT) continue;
    ccord_webauthn_cred *c = &res->webauthn_creds[res->webauthn_cred_count];
    if (acct_jstr(it, "id", c->id, sizeof c->id) == 1 && c->id[0]) {
      if (acct_jstr(it, "name", c->name, sizeof c->name) != 1 || !c->name[0]) {
        acct_copy_str(c->name, sizeof c->name, "Security Key");
      }
      c->created_at_ms = acct_ji64(it, "created_at_ms", 0);
      res->webauthn_cred_count++;
    }
  }
}

static uint32_t parse_mfa_methods(const ccord_json *mfa_obj)
{
  uint32_t mask = 0;
  const ccord_json *arr = mfa_obj ? ccord_json_get(mfa_obj, "methods") : nullptr;
  size_t n = arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY ? ccord_json_array_len(arr) : 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *m = ccord_json_array_at(arr, i);
    char t[32] = "";
    if (acct_jstr(m, "type", t, sizeof t) != 1) continue;
    if (strcmp(t, "totp") == 0) mask |= CCORD_MFA_METHOD_TOTP;
    else if (strcmp(t, "backup") == 0) mask |= CCORD_MFA_METHOD_BACKUP;
    else if (strcmp(t, "sms") == 0) mask |= CCORD_MFA_METHOD_SMS;
    else if (strcmp(t, "password") == 0) mask |= CCORD_MFA_METHOD_PASSWORD;
    else if (strcmp(t, "webauthn") == 0) mask |= CCORD_MFA_METHOD_WEBAUTHN;
  }
  return mask;
}

static void parse_error_body(const ccord_json *j, long http, ccord_account_result *res)
{
  res->status = status_for_http(http);
  res->rate_limited = http == 429;
  if (j && ccord_json_type_of(j) == CCORD_JSON_OBJECT) {
    (void)acct_jstr(j, "message", res->message, sizeof res->message);
    int64_t code = acct_ji64(j, "code", 0);
    if (code >= 0 && code <= INT32_MAX) res->code = (int32_t)code;
    const ccord_json *ra = ccord_json_get(j, "retry_after");
    if (ra) {
      double d = ccord_json_number(ra, 0.0);
      if (isfinite(d) && d > 0.0) res->retry_after_s = d;
    }
    if (ccord_json_get(j, "captcha_key") || ccord_json_get(j, "captcha_sitekey")) {
      res->captcha_required = true;
    }
    const ccord_json *mfa = ccord_json_get(j, "mfa");
    if (res->code == 60003 || (mfa && ccord_json_type_of(mfa) == CCORD_JSON_OBJECT)) {
      if (mfa && ccord_json_type_of(mfa) == CCORD_JSON_OBJECT) {
        (void)acct_jstr(mfa, "ticket", res->mfa_ticket, sizeof res->mfa_ticket);
        res->mfa_methods = parse_mfa_methods(mfa);
      }
      if (res->mfa_ticket[0]) {
        res->mfa_challenge_required = true;
        if (res->mfa_methods == 0) res->mfa_methods = CCORD_MFA_METHOD_TOTP | CCORD_MFA_METHOD_PASSWORD;
      }
    }
    if (res->captcha_required && res->message[0] == '\0') {
      acct_copy_str(res->message, sizeof res->message, "Discord requires CAPTCHA verification in your web browser.");
    }
  }
  if (res->message[0] == '\0') {
    if (res->rate_limited) acct_copy_str(res->message, sizeof res->message, "You are being rate limited. Please try again shortly.");
    else (void)snprintf(res->message, sizeof res->message, "Discord refused the request (HTTP %ld)", http);
  }
}

// ---- mock backend implementation -------------------------------------------------------------------------------

static char *acct_mock_user_json(const acct_mock_state *m, bool include_token)
{
  ccord_buf out;
  ccord_buf_init(&out, 512);
  ccord_jw w;
  ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_id(&w, m->self_id);
  ccord_jw_key(&w, "username"); ccord_jw_cstr(&w, m->username);
  ccord_jw_key(&w, "email");
  if (m->priv.email[0]) ccord_jw_cstr(&w, m->priv.email);
  else ccord_jw_null(&w);
  ccord_jw_key(&w, "phone");
  if (m->priv.phone[0]) ccord_jw_cstr(&w, m->priv.phone);
  else ccord_jw_null(&w);
  ccord_jw_key(&w, "verified"); ccord_jw_bool(&w, m->priv.verified);
  ccord_jw_key(&w, "mfa_enabled"); ccord_jw_bool(&w, m->priv.mfa_enabled);
  ccord_jw_key(&w, "nsfw_allowed"); ccord_jw_bool(&w, m->priv.nsfw_allowed);
  ccord_jw_key(&w, "premium_type"); ccord_jw_int(&w, m->priv.premium_type);
  ccord_jw_key(&w, "authenticator_types");
  ccord_jw_begin_array(&w);
  if (m->priv.authenticator_types & CCORD_AUTH_TYPE_WEBAUTHN) ccord_jw_int(&w, 1);
  if (m->priv.authenticator_types & CCORD_AUTH_TYPE_TOTP) ccord_jw_int(&w, 2);
  if (m->priv.authenticator_types & CCORD_AUTH_TYPE_SMS) ccord_jw_int(&w, 3);
  ccord_jw_end_array(&w);
  if (include_token) {
    ccord_jw_key(&w, "token");
    ccord_jw_cstr(&w, "REDACTED_TEST_ROTATED_SESSION");
  }
  ccord_jw_end_object(&w);
  size_t n = 0;
  return ccord_jw_ok(&w) ? ccord_buf_take(&out, &n) : (ccord_buf_free(&out), nullptr);
}

static char *acct_mock_codes_json(const acct_mock_state *m, bool include_token)
{
  ccord_buf out;
  ccord_buf_init(&out, 512);
  ccord_jw w;
  ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  if (include_token) {
    ccord_jw_key(&w, "token");
    ccord_jw_cstr(&w, "REDACTED_TEST_MFA_ROTATED");
  }
  ccord_jw_key(&w, "backup_codes");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < m->backup_code_count; i++) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "code"); ccord_jw_cstr(&w, m->backup_codes[i].code);
    ccord_jw_key(&w, "consumed"); ccord_jw_bool(&w, m->backup_codes[i].consumed);
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  size_t n = 0;
  return ccord_jw_ok(&w) ? ccord_buf_take(&out, &n) : (ccord_buf_free(&out), nullptr);
}

static char *acct_mock_mfa_challenge_json(uint32_t methods)
{
  ccord_buf out;
  ccord_buf_init(&out, 256);
  ccord_jw w;
  ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "message"); ccord_jw_cstr(&w, "Two factor is required for this operation");
  ccord_jw_key(&w, "code"); ccord_jw_int(&w, 60003);
  ccord_jw_key(&w, "mfa");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "ticket"); ccord_jw_cstr(&w, "mock_mfa_ticket_60003");
  ccord_jw_key(&w, "methods");
  ccord_jw_begin_array(&w);
  if (methods & CCORD_MFA_METHOD_TOTP) {
    ccord_jw_begin_object(&w); ccord_jw_key(&w, "type"); ccord_jw_cstr(&w, "totp"); ccord_jw_end_object(&w);
  }
  if (methods & CCORD_MFA_METHOD_BACKUP) {
    ccord_jw_begin_object(&w); ccord_jw_key(&w, "type"); ccord_jw_cstr(&w, "backup"); ccord_jw_end_object(&w);
  }
  if (methods & CCORD_MFA_METHOD_SMS) {
    ccord_jw_begin_object(&w); ccord_jw_key(&w, "type"); ccord_jw_cstr(&w, "sms"); ccord_jw_end_object(&w);
  }
  if (methods & CCORD_MFA_METHOD_PASSWORD) {
    ccord_jw_begin_object(&w); ccord_jw_key(&w, "type"); ccord_jw_cstr(&w, "password"); ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  size_t n = 0;
  return ccord_jw_ok(&w) ? ccord_buf_take(&out, &n) : (ccord_buf_free(&out), nullptr);
}

static char *acct_mock_handle(acct_mock_state *m, acct_http_method method, const char *path,
                              const char *body, size_t len, const char *mfa_jwt, long *out_http)
{
  *out_http = 200;
  if (m->mfa_next && (!mfa_jwt || !mfa_jwt[0]) && strcmp(path, "/mfa/finish") != 0) {
    m->mfa_next = false;
    *out_http = 401;
    return acct_mock_mfa_challenge_json(m->mfa_next_methods);
  }
  ccord_arena arena;
  ccord_arena_init(&arena, 2048);
  ccord_error jerr;
  const ccord_json *j = (body && len > 0) ? ccord_json_parse(&arena, body, len, nullptr, &jerr) : nullptr;
  char *resp = nullptr;

  if (strcmp(path, "/mfa/finish") == 0) {
    char data[64] = "";
    (void)acct_jstr(j, "data", data, sizeof data);
    if (!data[0] || strcmp(data, "bad") == 0) {
      *out_http = 400;
      resp = ccord_strdup("{\"message\":\"Invalid two-factor code\",\"code\":60008}");
    } else {
      resp = ccord_strdup("{\"token\":\"REDACTED_TEST_MFA_JWT\"}");
    }
  } else if (strcmp(path, "/users/@me") == 0 && method == ACCT_PATCH) {
    bool rotated = false;
    char tmp[128] = "";
    if (acct_jstr(j, "username", tmp, sizeof tmp) == 1 && tmp[0]) {
      acct_copy_str(m->username, sizeof m->username, tmp);
    }
    if (acct_jstr(j, "email", tmp, sizeof tmp) == 1 && tmp[0]) {
      acct_copy_str(m->priv.email, sizeof m->priv.email, tmp);
      m->priv.verified = false;
    }
    if (ccord_json_get(j, "new_password") != nullptr) {
      rotated = true;
    }
    resp = acct_mock_user_json(m, rotated);
  } else if (strcmp(path, "/auth/verify/resend") == 0) {
    *out_http = 204;
    resp = ccord_strdup("");
  } else if (strcmp(path, "/phone-verifications/verify") == 0) {
    char code[32] = "";
    (void)acct_jstr(j, "code", code, sizeof code);
    if (strlen(code) < 4) {
      *out_http = 400;
      resp = ccord_strdup("{\"message\":\"Invalid verification code\",\"code\":50035}");
    } else {
      resp = ccord_strdup("{\"token\":\"mock_phone_verify_ok\"}");
    }
  } else if (strcmp(path, "/users/@me/phone") == 0) {
    if (method == ACCT_DELETE) {
      m->priv.phone[0] = '\0';
      m->priv.sms_mfa_enabled = false;
      m->priv.authenticator_types &= ~CCORD_AUTH_TYPE_SMS;
      *out_http = 204;
      resp = ccord_strdup("");
    } else {
      char ph[32] = "";
      if (acct_jstr(j, "phone", ph, sizeof ph) == 1 && ph[0]) {
        // Step 1 send SMS code: keep phone pending until verify
        *out_http = 204;
        resp = ccord_strdup("");
      } else {
        // Step 2 confirm phone_token
        if (acct_jstr(j, "phone_hint", ph, sizeof ph) == 1 && ph[0]) {
          acct_copy_str(m->priv.phone, sizeof m->priv.phone, ph);
        } else if (!m->priv.phone[0]) {
          acct_copy_str(m->priv.phone, sizeof m->priv.phone, "+15550100199");
        }
        *out_http = 204;
        resp = ccord_strdup("");
      }
    }
  } else if (strcmp(path, "/users/@me/mfa/totp/enable") == 0) {
    m->priv.mfa_enabled = true;
    m->priv.authenticator_types |= CCORD_AUTH_TYPE_TOTP;
    acct_mock_init_codes(m);
    resp = acct_mock_codes_json(m, true);
  } else if (strcmp(path, "/users/@me/mfa/totp/disable") == 0) {
    m->priv.authenticator_types &= ~(CCORD_AUTH_TYPE_TOTP | CCORD_AUTH_TYPE_SMS);
    m->priv.sms_mfa_enabled = false;
    m->priv.mfa_enabled = (m->priv.authenticator_types != 0);
    resp = ccord_strdup("{\"token\":\"REDACTED_TEST_MFA_ROTATED\"}");
  } else if (strcmp(path, "/users/@me/mfa/codes-verification") == 0) {
    if (acct_jbool(j, "regenerate", false)) acct_mock_init_codes(m);
    resp = acct_mock_codes_json(m, false);
  } else if (strcmp(path, "/users/@me/mfa/sms/enable") == 0) {
    m->priv.sms_mfa_enabled = true;
    m->priv.authenticator_types |= CCORD_AUTH_TYPE_SMS;
    *out_http = 204;
    resp = ccord_strdup("");
  } else if (strcmp(path, "/users/@me/mfa/sms/disable") == 0) {
    m->priv.sms_mfa_enabled = false;
    m->priv.authenticator_types &= ~CCORD_AUTH_TYPE_SMS;
    *out_http = 204;
    resp = ccord_strdup("");
  } else if (strcmp(path, "/users/@me/mfa/webauthn/credentials") == 0 && method == ACCT_GET) {
    ccord_buf out;
    ccord_buf_init(&out, 512);
    ccord_jw w;
    ccord_jw_init(&w, &out);
    ccord_jw_begin_array(&w);
    for (size_t i = 0; i < m->cred_count; i++) {
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, m->creds[i].id);
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, m->creds[i].name);
      ccord_jw_key(&w, "created_at_ms"); ccord_jw_int(&w, m->creds[i].created_at_ms);
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_array(&w);
    size_t n = 0;
    resp = ccord_jw_ok(&w) ? ccord_buf_take(&out, &n) : (ccord_buf_free(&out), nullptr);
  } else if (strncmp(path, "/users/@me/mfa/webauthn/credentials/", 36) == 0 && method == ACCT_DELETE) {
    const char *cid = path + 36;
    for (size_t i = 0; i < m->cred_count; i++) {
      if (strcmp(m->creds[i].id, cid) == 0) {
        for (size_t k = i + 1u; k < m->cred_count; k++) m->creds[k - 1u] = m->creds[k];
        m->cred_count--;
        break;
      }
    }
    if (m->cred_count == 0) {
      m->priv.authenticator_types &= ~CCORD_AUTH_TYPE_WEBAUTHN;
      m->priv.mfa_enabled = (m->priv.authenticator_types != 0);
    }
    *out_http = 204;
    resp = ccord_strdup("");
  } else if (strcmp(path, "/safety-hub/@me") == 0) {
    ccord_buf out;
    ccord_buf_init(&out, 256);
    ccord_jw w;
    ccord_jw_init(&w, &out);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "account_standing");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "state"); ccord_jw_int(&w, 100 + (int)m->standing.tier * 100);
    ccord_jw_end_object(&w);
    ccord_jw_key(&w, "classifications");
    ccord_jw_begin_array(&w);
    ccord_jw_end_array(&w);
    ccord_jw_end_object(&w);
    size_t n = 0;
    resp = ccord_jw_ok(&w) ? ccord_buf_take(&out, &n) : (ccord_buf_free(&out), nullptr);
  } else if (strcmp(path, "/users/@me/disable") == 0 || strcmp(path, "/users/@me/delete") == 0 ||
             strcmp(path, "/users/@me/harvest") == 0) {
    *out_http = 204;
    resp = ccord_strdup("");
  } else {
    *out_http = 404;
    resp = ccord_strdup("{\"message\":\"Unknown endpoint\"}");
  }
  ccord_arena_free(&arena);
  return resp;
}

static ccord_status acct_http_send(ccord_account_rest *r, acct_http_method method, const char *path,
                                   const char *body, size_t len, const char *mfa_jwt,
                                   acct_http_cb cb, void *ud, ccord_error *err)
{
  if (!r->is_mock) {
#ifdef CCORD_PROFILE_LIVE
    ccord_rest_method rm = CCORD_REST_GET;
    if (method == ACCT_POST) rm = CCORD_REST_POST;
    else if (method == ACCT_PATCH) rm = CCORD_REST_PATCH;
    else if (method == ACCT_DELETE) rm = CCORD_REST_DELETE;
    ccord_rest_request req = ccord_rest_request_make(rm, path, CCORD_GOV_USER);
    if (body && len > 0) {
      req.body = body;
      req.body_len = len;
      req.content_type = "application/json";
    }
    req.mfa_auth_token = mfa_jwt;
    req.cb = cb;
    req.ud = ud;
    return ccord_rest_queue_submit(r->queue, &req, err);
#else
    (void)method; (void)path; (void)body; (void)len; (void)mfa_jwt; (void)cb; (void)ud;
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "account: this build has no live transport");
#endif
  }
  acct_mock_state *m = r->mock;
  acct_mock_pending *slot = nullptr;
  for (size_t i = 0; i < ACCT_MOCK_MAX_PENDING && !slot; i++) {
    if (!m->pending[i].used) slot = &m->pending[i];
  }
  if (!slot) return ccord_error_set(err, CCORD_ERR_RANGE, "account: too many requests in flight");
  m->count++;
  acct_copy_str(m->last_path, sizeof m->last_path, path);
  slot->used = true;
  m->pending_count++;
  slot->cb = cb;
  slot->ud = ud;
  if (m->fail_armed) {
    m->fail_armed = false;
    slot->http = m->fail_http;
    slot->body = m->fail_body;
    m->fail_body = nullptr;
  } else {
    long code = 200;
    slot->body = acct_mock_handle(m, method, path, body, len, mfa_jwt, &code);
    slot->http = slot->body ? code : 500;
  }
  return CCORD_OK;
}

static void acct_mock_deliver(acct_mock_state *m, acct_mock_pending *p, ccord_status st)
{
  acct_mock_pending done = *p;
  memset(p, 0, sizeof *p);
  if (m->pending_count > 0) m->pending_count--;
  if (done.cb) {
    done.cb(done.ud, st, st == CCORD_OK ? done.http : 0, done.body ? done.body : "",
            done.body ? strlen(done.body) : 0);
  }
  if (done.body) {
    acct_wipe(done.body, strlen(done.body));
    ccord_free(done.body);
  }
}

void ccord_account_rest_pump(ccord_account_rest *r, uint64_t now_ms)
{
  if (!r) return;
  if (!r->is_mock) {
#ifdef CCORD_PROFILE_LIVE
    if (r->queue && ccord_rest_queue_pending(r->queue) > 0) ccord_rest_queue_pump(r->queue, now_ms);
#else
    (void)now_ms;
#endif
    return;
  }
  if (!r->mock || r->mock->pending_count == 0) return;
  for (size_t i = 0; i < ACCT_MOCK_MAX_PENDING && r->mock->pending_count > 0; i++) {
    if (r->mock->pending[i].used) acct_mock_deliver(r->mock, &r->mock->pending[i], CCORD_OK);
  }
}

bool ccord_account_rest_pending(const ccord_account_rest *r)
{
  if (!r) return false;
  if (r->is_mock) return r->mock && r->mock->pending_count > 0;
#ifdef CCORD_PROFILE_LIVE
  return r->queue && ccord_rest_queue_pending(r->queue) > 0;
#else
  return false;
#endif
}

void ccord_account_rest_destroy(ccord_account_rest *r)
{
  if (!r) return;
  acct_mfa_clear(&r->mfa);
  if (r->is_mock && r->mock) {
    for (size_t i = 0; i < ACCT_MOCK_MAX_PENDING && r->mock->pending_count > 0; i++) {
      if (r->mock->pending[i].used) acct_mock_deliver(r->mock, &r->mock->pending[i], CCORD_ERR_IO);
    }
    ccord_free(r->mock->fail_body);
    acct_wipe(r->mock, sizeof *r->mock);
    ccord_free(r->mock);
  }
#ifdef CCORD_PROFILE_LIVE
  if (r->queue) ccord_rest_queue_destroy(r->queue);
#endif
  ccord_free(r);
}

// ---- operation job state machine -------------------------------------------------------------------------------

typedef struct acct_job {
  ccord_account_rest *rest;
  ccord_account_op_kind op;
  acct_http_method method;
  char path[128];
  char *body;
  size_t body_len;
  // Two-step phone verify stores password/phone until step 2 finishes (wiped on free).
  bool phone_step1;
  char phone_val[32];
  char phone_pw[128];
  char deleted_id[64];
  bool is_mfa_replay;
  bool flag;
  ccord_account_cb cb;
  void *ud;
} acct_job;

static void acct_job_free(acct_job *job)
{
  if (!job) return;
  if (job->body) {
    acct_wipe(job->body, job->body_len);
    ccord_free(job->body);
  }
  acct_wipe(job, sizeof *job);
  ccord_free(job);
}

static void acct_deliver_and_wipe(acct_job *job, ccord_account_result *res)
{
  if (job->cb) job->cb(job->ud, res);
  acct_wipe(res, sizeof *res);
  acct_job_free(job);
}

static void acct_on_http(void *ud, ccord_status st, long http, const char *body, size_t len)
{
  acct_job *job = ud;
  ccord_account_result res;
  memset(&res, 0, sizeof res);
  res.op = job->op;
  res.http_status = http;
  if (job->deleted_id[0]) acct_copy_str(res.deleted_webauthn_id, sizeof res.deleted_webauthn_id, job->deleted_id);

  if (st != CCORD_OK && !(http >= 100)) {
    res.status = st;
    acct_copy_str(res.message, sizeof res.message,
                  st == CCORD_ERR_TIMEOUT ? "Discord did not answer in time" : "Could not reach Discord");
    acct_deliver_and_wipe(job, &res);
    return;
  }

  ccord_arena arena;
  ccord_arena_init(&arena, 4096);
  ccord_error jerr;
  const ccord_json *j = (body && len > 0) ? ccord_json_parse(&arena, body, len, nullptr, &jerr) : nullptr;

  if (http >= 200 && http < 300) {
    // Two-step phone verification step 1: exchange phone_token into POST /users/@me/phone
    if (job->phone_step1) {
      char ptok[256] = "";
      if (!j || acct_jstr(j, "token", ptok, sizeof ptok) != 1 || !ptok[0]) {
        ccord_arena_free(&arena);
        res.status = CCORD_ERR_PARSE;
        acct_copy_str(res.message, sizeof res.message, "Missing phone verification token");
        acct_deliver_and_wipe(job, &res);
        return;
      }
      ccord_arena_free(&arena);
      ccord_buf b2;
      ccord_buf_init(&b2, 512);
      ccord_jw w;
      ccord_jw_init(&w, &b2);
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "phone_token"); ccord_jw_cstr(&w, ptok);
      ccord_jw_key(&w, "password"); ccord_jw_cstr(&w, job->phone_pw);
      ccord_jw_key(&w, "change_phone_reason"); ccord_jw_cstr(&w, "user_settings_update");
      if (job->rest->is_mock && job->phone_val[0]) {
        ccord_jw_key(&w, "phone_hint"); ccord_jw_cstr(&w, job->phone_val);
      }
      ccord_jw_end_object(&w);
      acct_wipe_str(ptok);
      acct_wipe(job->phone_pw, sizeof job->phone_pw);
      if (!ccord_jw_ok(&w)) {
        if (b2.data) acct_wipe(b2.data, b2.len);
        ccord_buf_free(&b2);
        res.status = CCORD_ERR_NOMEM;
        acct_copy_str(res.message, sizeof res.message, "Out of memory building phone confirmation");
        acct_deliver_and_wipe(job, &res);
        return;
      }
      if (job->body) {
        acct_wipe(job->body, job->body_len);
        ccord_free(job->body);
      }
      job->body = ccord_buf_take(&b2, &job->body_len);
      job->phone_step1 = false;
      job->method = ACCT_POST;
      acct_copy_str(job->path, sizeof job->path, "/users/@me/phone");
      ccord_error serr;
      ccord_status sst = acct_http_send(job->rest, job->method, job->path, job->body, job->body_len,
                                        nullptr, acct_on_http, job, &serr);
      if (sst != CCORD_OK) {
        res.status = sst;
        acct_copy_str(res.message, sizeof res.message, serr.message);
        acct_deliver_and_wipe(job, &res);
      }
      return;
    }

    res.status = CCORD_OK;
    if (j && ccord_json_type_of(j) == CCORD_JSON_OBJECT) {
      char new_tok[512] = "";
      if (acct_jstr(j, "token", new_tok, sizeof new_tok) == 1 && new_tok[0]) {
        res.token_rotated = true;
        ccord_account_rest_set_token(job->rest, new_tok);
        if (job->rest->token_cb) job->rest->token_cb(job->rest->token_ud, new_tok);
        acct_wipe(new_tok, sizeof new_tok);
      }
      parse_user_private(j, &res);
      parse_backup_codes(j, &res);
      if (job->op == CCORD_ACCT_OP_STANDING_FETCH) parse_standing(j, &res);
    } else if (j && job->op == CCORD_ACCT_OP_WEBAUTHN_LIST) {
      parse_webauthn_list(j, &res);
    }
    // For 204 ops that mutate private metadata without returning a user object, reflect the confirmed state.
    if (!res.has_updated_private && job->op == CCORD_ACCT_OP_PHONE_VERIFY && job->phone_val[0]) {
      res.has_updated_private = true;
      res.updated_private.valid = true;
      acct_copy_str(res.updated_private.phone, sizeof res.updated_private.phone, job->phone_val);
    }
    ccord_arena_free(&arena);
    acct_deliver_and_wipe(job, &res);
    return;
  }

  // Error or 60003 MFA challenge.
  parse_error_body(j, http, &res);
  ccord_arena_free(&arena);

  if (res.mfa_challenge_required && !job->is_mfa_replay) {
    acct_mfa_clear(&job->rest->mfa);
    job->rest->mfa.active = true;
    job->rest->mfa.op = job->op;
    job->rest->mfa.method = job->method;
    acct_copy_str(job->rest->mfa.path, sizeof job->rest->mfa.path, job->path);
    acct_copy_str(job->rest->mfa.ticket, sizeof job->rest->mfa.ticket, res.mfa_ticket);
    job->rest->mfa.methods = res.mfa_methods;
    acct_copy_str(job->rest->mfa.deleted_id, sizeof job->rest->mfa.deleted_id, job->deleted_id);
    if (job->body && job->body_len > 0) {
      job->rest->mfa.body = ccord_calloc(1, job->body_len + 1u);
      if (job->rest->mfa.body) {
        memcpy(job->rest->mfa.body, job->body, job->body_len);
        job->rest->mfa.body[job->body_len] = '\0';
        job->rest->mfa.body_len = job->body_len;
      }
    }
  }
  acct_deliver_and_wipe(job, &res);
}

static bool valid_id_segment(const char *s)
{
  if (!s || !s[0] || strlen(s) > 63u) return false;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_';
    if (!ok) return false;
  }
  return true;
}

ccord_status ccord_account_rest_submit(ccord_account_rest *r, const ccord_account_req *req,
                                       ccord_account_cb cb, void *ud, ccord_error *err)
{
  ccord_error_clear(err);
  if (!r || !req) return ccord_error_set(err, CCORD_ERR_INVALID, "account: null argument");

  acct_http_method method = ACCT_POST;
  char path[128] = "";
  ccord_buf body;
  ccord_buf_init(&body, 512);
  ccord_jw w;
  ccord_jw_init(&w, &body);
  bool has_body = true;
  bool phone_step1 = false;
  char deleted_id[64] = "";

  switch (req->op) {
    case CCORD_ACCT_OP_USERNAME:
      if (!req->value || strlen(req->value) < 2u || strlen(req->value) > 32u) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "Username must be 2 to 32 characters.");
      }
      if (!req->password || !req->password[0]) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "Current password is required.");
      }
      method = ACCT_PATCH;
      acct_copy_str(path, sizeof path, "/users/@me");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "username"); ccord_jw_cstr(&w, req->value);
      ccord_jw_key(&w, "password"); ccord_jw_cstr(&w, req->password);
      ccord_jw_end_object(&w);
      break;

    case CCORD_ACCT_OP_EMAIL:
      if (!req->value || !strchr(req->value, '@') || strlen(req->value) > 127u) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "Enter a valid email address.");
      }
      if (!req->password || !req->password[0]) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "Current password is required.");
      }
      method = ACCT_PATCH;
      acct_copy_str(path, sizeof path, "/users/@me");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "email"); ccord_jw_cstr(&w, req->value);
      ccord_jw_key(&w, "password"); ccord_jw_cstr(&w, req->password);
      ccord_jw_end_object(&w);
      break;

    case CCORD_ACCT_OP_RESEND_VERIFY:
      method = ACCT_POST;
      acct_copy_str(path, sizeof path, "/auth/verify/resend");
      ccord_jw_begin_object(&w);
      ccord_jw_end_object(&w);
      break;

    case CCORD_ACCT_OP_PHONE_SEND:
      if (!req->value || req->value[0] != '+' || strlen(req->value) < 8u || strlen(req->value) > 24u) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "Enter phone number in E.164 format (e.g. +15550100199).");
      }
      method = ACCT_POST;
      acct_copy_str(path, sizeof path, "/users/@me/phone");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "phone"); ccord_jw_cstr(&w, req->value);
      ccord_jw_key(&w, "change_phone_reason"); ccord_jw_cstr(&w, "user_settings_update");
      ccord_jw_end_object(&w);
      break;

    case CCORD_ACCT_OP_PHONE_VERIFY:
      if (!req->value || !req->value[0] || !req->code || strlen(req->code) < 4u || !req->password || !req->password[0]) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "Phone number, SMS code, and password are required.");
      }
      method = ACCT_POST;
      acct_copy_str(path, sizeof path, "/phone-verifications/verify");
      phone_step1 = true;
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "phone"); ccord_jw_cstr(&w, req->value);
      ccord_jw_key(&w, "code"); ccord_jw_cstr(&w, req->code);
      ccord_jw_end_object(&w);
      break;

    case CCORD_ACCT_OP_PHONE_REMOVE:
      if (!req->password || !req->password[0]) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "Current password is required.");
      }
      method = ACCT_DELETE;
      acct_copy_str(path, sizeof path, "/users/@me/phone");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "password"); ccord_jw_cstr(&w, req->password);
      ccord_jw_key(&w, "change_phone_reason"); ccord_jw_cstr(&w, "user_settings_update");
      ccord_jw_end_object(&w);
      break;

    case CCORD_ACCT_OP_PASSWORD:
      if (!req->password || !req->password[0] || !req->new_password || strlen(req->new_password) < 8u) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "Current password and new password (8+ chars) are required.");
      }
      method = ACCT_PATCH;
      acct_copy_str(path, sizeof path, "/users/@me");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "password"); ccord_jw_cstr(&w, req->password);
      ccord_jw_key(&w, "new_password"); ccord_jw_cstr(&w, req->new_password);
      ccord_jw_end_object(&w);
      break;

    case CCORD_ACCT_OP_MFA_TOTP_ENABLE:
      if (!req->value || strlen(req->value) < 16u || !req->code || strlen(req->code) < 6u ||
          !req->password || !req->password[0]) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "TOTP secret, 6-digit code, and password are required.");
      }
      method = ACCT_POST;
      acct_copy_str(path, sizeof path, "/users/@me/mfa/totp/enable");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "secret"); ccord_jw_cstr(&w, req->value);
      ccord_jw_key(&w, "code"); ccord_jw_cstr(&w, req->code);
      ccord_jw_key(&w, "password"); ccord_jw_cstr(&w, req->password);
      ccord_jw_end_object(&w);
      break;

    case CCORD_ACCT_OP_MFA_TOTP_DISABLE:
      if (!req->code || strlen(req->code) < 6u) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "Enter your 6-digit authenticator code or 8-digit backup code.");
      }
      method = ACCT_POST;
      acct_copy_str(path, sizeof path, "/users/@me/mfa/totp/disable");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "code"); ccord_jw_cstr(&w, req->code);
      ccord_jw_end_object(&w);
      break;

    case CCORD_ACCT_OP_MFA_BACKUP_CODES:
      if (!req->password || !req->password[0]) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "Current password is required.");
      }
      method = ACCT_POST;
      acct_copy_str(path, sizeof path, "/users/@me/mfa/codes-verification");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "password"); ccord_jw_cstr(&w, req->password);
      ccord_jw_key(&w, "regenerate"); ccord_jw_bool(&w, req->flag);
      ccord_jw_end_object(&w);
      break;

    case CCORD_ACCT_OP_MFA_SMS_TOGGLE:
      if (!req->password || !req->password[0]) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "Current password is required.");
      }
      method = ACCT_POST;
      acct_copy_str(path, sizeof path, req->flag ? "/users/@me/mfa/sms/enable" : "/users/@me/mfa/sms/disable");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "password"); ccord_jw_cstr(&w, req->password);
      ccord_jw_end_object(&w);
      break;

    case CCORD_ACCT_OP_WEBAUTHN_LIST:
      method = ACCT_GET;
      has_body = false;
      acct_copy_str(path, sizeof path, "/users/@me/mfa/webauthn/credentials");
      break;

    case CCORD_ACCT_OP_WEBAUTHN_DELETE:
      if (!valid_id_segment(req->value)) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "Invalid security key identifier.");
      }
      method = ACCT_DELETE;
      has_body = false;
      acct_copy_str(deleted_id, sizeof deleted_id, req->value);
      (void)snprintf(path, sizeof path, "/users/@me/mfa/webauthn/credentials/%s", req->value);
      break;

    case CCORD_ACCT_OP_STANDING_FETCH:
      method = ACCT_GET;
      has_body = false;
      acct_copy_str(path, sizeof path, "/safety-hub/@me");
      break;

    case CCORD_ACCT_OP_DISABLE_ACCOUNT:
    case CCORD_ACCT_OP_DELETE_ACCOUNT:
      if (!req->password || !req->password[0]) {
        ccord_buf_free(&body);
        return ccord_error_set(err, CCORD_ERR_INVALID, "Current password is required.");
      }
      method = ACCT_POST;
      acct_copy_str(path, sizeof path,
                    req->op == CCORD_ACCT_OP_DISABLE_ACCOUNT ? "/users/@me/disable" : "/users/@me/delete");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "password"); ccord_jw_cstr(&w, req->password);
      ccord_jw_end_object(&w);
      break;

    case CCORD_ACCT_OP_HARVEST_DATA:
      method = ACCT_POST;
      acct_copy_str(path, sizeof path, "/users/@me/harvest");
      ccord_jw_begin_object(&w);
      ccord_jw_end_object(&w);
      break;

    default:
      ccord_buf_free(&body);
      return ccord_error_set(err, CCORD_ERR_INVALID, "account: unknown operation");
  }

  if (has_body && !ccord_jw_ok(&w)) {
    if (body.data) acct_wipe(body.data, body.len);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "account: out of memory");
  }

  acct_job *job = ccord_calloc(1, sizeof *job);
  if (!job) {
    if (body.data) acct_wipe(body.data, body.len);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "account: out of memory");
  }
  job->rest = r;
  job->op = req->op;
  job->method = method;
  acct_copy_str(job->path, sizeof job->path, path);
  job->phone_step1 = phone_step1;
  job->flag = req->flag;
  acct_copy_str(job->deleted_id, sizeof job->deleted_id, deleted_id);
  if (phone_step1) {
    acct_copy_str(job->phone_val, sizeof job->phone_val, req->value);
    acct_copy_str(job->phone_pw, sizeof job->phone_pw, req->password);
  }
  if (has_body) {
    job->body = ccord_buf_take(&body, &job->body_len);
  } else {
    ccord_buf_free(&body);
  }
  job->cb = cb;
  job->ud = ud;

  ccord_status st = acct_http_send(r, job->method, job->path, job->body, job->body_len,
                                   nullptr, acct_on_http, job, err);
  if (st != CCORD_OK) {
    acct_job_free(job);
    return st;
  }
  return CCORD_OK;
}

// ---- unified 60003 MFA finish & replay -------------------------------------------------------------------------

typedef struct acct_mfa_finish_job {
  ccord_account_rest *rest;
  ccord_account_cb cb;
  void *ud;
} acct_mfa_finish_job;

static void acct_on_mfa_finish(void *ud, ccord_status st, long http, const char *body, size_t len)
{
  acct_mfa_finish_job *fj = ud;
  ccord_account_rest *r = fj->rest;
  ccord_account_cb cb = fj->cb;
  void *cb_ud = fj->ud;
  ccord_free(fj);

  ccord_account_result res;
  memset(&res, 0, sizeof res);
  res.op = r->mfa.op;
  res.http_status = http;

  if (!r->mfa.active) {
    res.status = CCORD_ERR_INVALID;
    acct_copy_str(res.message, sizeof res.message, "MFA challenge expired or cancelled.");
    if (cb) cb(cb_ud, &res);
    return;
  }
  if (st != CCORD_OK && !(http >= 100)) {
    res.status = st;
    acct_copy_str(res.message, sizeof res.message, "Could not reach Discord for MFA verification.");
    if (cb) cb(cb_ud, &res);
    return;
  }

  ccord_arena arena;
  ccord_arena_init(&arena, 2048);
  ccord_error jerr;
  const ccord_json *j = (body && len > 0) ? ccord_json_parse(&arena, body, len, nullptr, &jerr) : nullptr;
  char mfa_jwt[1536] = "";
  bool got_jwt = http >= 200 && http < 300 && j && acct_jstr(j, "token", mfa_jwt, sizeof mfa_jwt) == 1 && mfa_jwt[0];
  if (!got_jwt) {
    parse_error_body(j, http, &res);
    // Keep r->mfa active so user can retry a typo'd TOTP code without re-entering the original form!
    res.mfa_challenge_required = true;
    acct_copy_str(res.mfa_ticket, sizeof res.mfa_ticket, r->mfa.ticket);
    res.mfa_methods = r->mfa.methods;
    ccord_arena_free(&arena);
    acct_wipe(mfa_jwt, sizeof mfa_jwt);
    if (cb) cb(cb_ud, &res);
    acct_wipe(&res, sizeof res);
    return;
  }
  ccord_arena_free(&arena);

  // Build replay job from stash, then immediately wipe & clear the stash.
  acct_job *replay = ccord_calloc(1, sizeof *replay);
  if (!replay) {
    acct_wipe(mfa_jwt, sizeof mfa_jwt);
    acct_mfa_clear(&r->mfa);
    res.status = CCORD_ERR_NOMEM;
    acct_copy_str(res.message, sizeof res.message, "Out of memory replaying MFA request.");
    if (cb) cb(cb_ud, &res);
    return;
  }
  replay->rest = r;
  replay->op = r->mfa.op;
  replay->method = r->mfa.method;
  acct_copy_str(replay->path, sizeof replay->path, r->mfa.path);
  acct_copy_str(replay->deleted_id, sizeof replay->deleted_id, r->mfa.deleted_id);
  replay->body = r->mfa.body;
  replay->body_len = r->mfa.body_len;
  r->mfa.body = nullptr;
  r->mfa.body_len = 0;
  replay->is_mfa_replay = true;
  replay->cb = cb;
  replay->ud = cb_ud;
  acct_mfa_clear(&r->mfa);

  ccord_error serr;
  ccord_status sst = acct_http_send(r, replay->method, replay->path, replay->body, replay->body_len,
                                    mfa_jwt, acct_on_http, replay, &serr);
  acct_wipe(mfa_jwt, sizeof mfa_jwt);
  if (sst != CCORD_OK) {
    res.status = sst;
    acct_copy_str(res.message, sizeof res.message, serr.message);
    acct_deliver_and_wipe(replay, &res);
  }
}

ccord_status ccord_account_rest_finish_mfa(ccord_account_rest *r, const char *mfa_type, const char *mfa_data,
                                           ccord_account_cb cb, void *ud, ccord_error *err)
{
  ccord_error_clear(err);
  if (!r || !r->mfa.active) return ccord_error_set(err, CCORD_ERR_INVALID, "No active MFA challenge.");
  if (!mfa_type || !mfa_type[0] || !mfa_data || !mfa_data[0]) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "Enter your verification code or password.");
  }
  ccord_buf body;
  ccord_buf_init(&body, 512);
  ccord_jw w;
  ccord_jw_init(&w, &body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "ticket"); ccord_jw_cstr(&w, r->mfa.ticket);
  ccord_jw_key(&w, "mfa_type"); ccord_jw_cstr(&w, mfa_type);
  ccord_jw_key(&w, "data"); ccord_jw_cstr(&w, mfa_data);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    if (body.data) acct_wipe(body.data, body.len);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "account: out of memory");
  }
  acct_mfa_finish_job *fj = ccord_calloc(1, sizeof *fj);
  if (!fj) {
    if (body.data) acct_wipe(body.data, body.len);
    ccord_buf_free(&body);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "account: out of memory");
  }
  fj->rest = r;
  fj->cb = cb;
  fj->ud = ud;
  ccord_status st = acct_http_send(r, ACCT_POST, "/mfa/finish", body.data, body.len, nullptr,
                                   acct_on_mfa_finish, fj, err);
  if (body.data) acct_wipe(body.data, body.len);
  ccord_buf_free(&body);
  if (st != CCORD_OK) {
    ccord_free(fj);
    return st;
  }
  return CCORD_OK;
}
