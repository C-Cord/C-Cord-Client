// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Own-profile edits: ccord_session_edit_profile against the fake Discord (a loopback HTTP server that records every
// request) and the in-process mock backend. What reaches the server is what Discord would see, so the tests assert on
// the recorded requests as well as on the store.
#include "fake_discord.h"   // first: it defines the feature-test macros

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ccord/net/account_edit.h>
#include <ccord/net/mock_transport.h>
#include <ccord/net/profile_edit.h>
#include <ccord/session/session.h>

#include "ccord_test.h"

#define TOKEN "REDACTED_TEST_TOKEN"

static uint64_t g_now = 1000;

static uint64_t fake_now(void *ud)
{
  (void)ud;
  return g_now;
}

typedef struct test_env {
  ccord_bus *bus;
  ccord_store *store;
  ccord_session *s;
  ccord_id self;
} test_env;

static bool env_init(test_env *e)
{
  memset(e, 0, sizeof *e);
  ccord_error err;
  g_now += 1000;
  e->bus = ccord_bus_create(&err);
  e->store = ccord_store_create(e->bus, &err);
  ccord_mock_config cfg = {.seed = 7, .latency_ms = 0, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  if (!e->bus || !e->store || !tr) return false;
  e->s = ccord_session_create(e->bus, e->store, tr, &err);   // takes ownership of tr
  if (!e->s || ccord_session_start(e->s, &err) != CCORD_OK) return false;
  for (int i = 0; i < 8; i++) {
    g_now += 5;
    if (ccord_session_pump(e->s) == 0) break;
  }
  e->self = ccord_store_self_id(e->store);
  return ccord_session_state(e->s) == CCORD_CONN_CONNECTED && e->self != CCORD_ID_NONE &&
         ccord_store_user(e->store, e->self) != nullptr;
}

static void env_done(test_env *e)
{
  ccord_session_destroy(e->s);
  ccord_store_destroy(e->store);
  ccord_bus_destroy(e->bus);
}

typedef struct run {
  int calls;
  ccord_profile_edit_result res;
} run;

static void on_done(void *ud, const ccord_profile_edit_result *r)
{
  run *x = ud;
  x->calls++;
  x->res = *r;
}

// Pumps the session (which pumps the profile REST backend) until the callback ran or 5 s passed.
static bool wait_done(test_env *e, run *x)
{
  for (int i = 0; i < 2500 && x->calls == 0; i++) {
    (void)ccord_session_pump(e->s);
    usleep(2000);
  }
  return x->calls > 0;
}

typedef struct fake {
  fd_server srv;
  fd_profile_cfg cfg;
} fake;

static bool fake_start(fake *f, test_env *e, int fail_idx, int fail_status, const char *fail_body)
{
  memset(f, 0, sizeof *f);
  f->cfg = (fd_profile_cfg){.self_id = e->self, .fail_idx = fail_idx, .fail_status = fail_status, .fail_body = fail_body};
  if (!fd_start(&f->srv, fd_profile_respond, &f->cfg)) return false;
  ccord_error err;
  ccord_profile_rest *rest = ccord_profile_rest_create_live(f->srv.base, TOKEN, &err);
  return rest && ccord_session_set_profile_rest(e->s, rest) == CCORD_OK;
}

static const ccord_user *self_user(test_env *e) { return ccord_store_user(e->store, e->self); }

// Changing only the bio: one PATCH /users/@me/profile whose body has only bio; the store shows the new text.
static void test_bio_only(void)
{
  test_env e;
  CT_CHECK(env_init(&e));
  if (!e.s) return;
  fake f;
  CT_CHECK(fake_start(&f, &e, -1, 0, nullptr));
  ccord_error err;
  ccord_profile_edit edit = CCORD_PROFILE_EDIT_INIT;
  edit.has_bio = true;
  edit.bio = "Writes C, drinks tea";
  run r = {0};
  CT_CHECK(ccord_session_edit_profile(e.s, &edit, on_done, &r, &err) == CCORD_OK);
  CT_CHECK(r.calls == 0);   // never from inside the call
  CT_CHECK(wait_done(&e, &r));
  CT_CHECK(r.res.status == CCORD_OK && r.res.http_status == 200 && r.res.requests_sent == 1);
  CT_CHECK(r.res.applied.fields == (1u << CCORD_PROFILE_FIELD_BIO));
  CT_CHECK(self_user(&e) && strcmp(self_user(&e)->bio, "Writes C, drinks tea") == 0);

  fd_hit hits[8];
  size_t n = fd_stop(&f.srv, hits, 8);
  CT_CHECK(n == 1);
  if (n == 1) {
    CT_CHECK(strcmp(hits[0].method, "PATCH") == 0 && strcmp(hits[0].path, "/users/@me/profile") == 0);
    CT_CHECK(strcmp(hits[0].body, "{\"bio\":\"Writes C, drinks tea\"}") == 0);
    CT_CHECK(strcmp(hits[0].authorization, TOKEN) == 0 && hits[0].super_properties);
    CT_CHECK(strncmp(hits[0].content_type, "application/json", 16) == 0);
  }
  env_done(&e);
}

static const uint8_t PNG[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};

// Avatar from PNG bytes -> data:image/png URI; removing it sends null; the store follows Discord's hash.
static void test_avatar_set_and_remove(void)
{
  test_env e;
  CT_CHECK(env_init(&e));
  if (!e.s) return;
  fake f;
  CT_CHECK(fake_start(&f, &e, -1, 0, nullptr));
  ccord_error err;
  ccord_profile_edit edit = CCORD_PROFILE_EDIT_INIT;
  edit.has_avatar = true;
  edit.avatar = PNG;
  edit.avatar_len = sizeof PNG;
  run r = {0};
  CT_CHECK(ccord_session_edit_profile(e.s, &edit, on_done, &r, &err) == CCORD_OK);
  CT_CHECK(wait_done(&e, &r));
  CT_CHECK(r.res.status == CCORD_OK);
  char want[256];
  (void)snprintf(want, sizeof want, "https://cdn.discordapp.com/avatars/%llu/fakeav1.png?size=128", (unsigned long long)e.self);
  CT_CHECK(self_user(&e) && strcmp(self_user(&e)->avatar_url, want) == 0);
  CT_CHECK(strcmp(r.res.applied.avatar_hash, "fakeav1") == 0);

  edit.avatar = nullptr;   // remove
  edit.avatar_len = 0;
  run r2 = {0};
  CT_CHECK(ccord_session_edit_profile(e.s, &edit, on_done, &r2, &err) == CCORD_OK);
  CT_CHECK(wait_done(&e, &r2));
  CT_CHECK(r2.res.status == CCORD_OK);
  CT_CHECK(self_user(&e) && self_user(&e)->avatar_url[0] == '\0');   // cleared, not "kept"

  fd_hit hits[8];
  size_t n = fd_stop(&f.srv, hits, 8);
  CT_CHECK(n == 2);
  if (n == 2) {
    CT_CHECK(strcmp(hits[0].path, "/users/@me") == 0);
    // base64 of "\x89PNG\r\n\x1a\n\0\0\0\rIHDR" (16 bytes -> 24 chars with padding)
    CT_CHECK(strcmp(hits[0].body, "{\"avatar\":\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUg==\"}") == 0);
    CT_CHECK(strcmp(hits[1].path, "/users/@me") == 0 && strcmp(hits[1].body, "{\"avatar\":null}") == 0);
  }
  env_done(&e);
}

// A 4xx with per-field errors reaches the caller; the store keeps what it had.
static void test_field_errors_leave_store_unchanged(void)
{
  test_env e;
  CT_CHECK(env_init(&e));
  if (!e.s) return;
  static const char body[] =
      "{\"message\":\"Invalid Form Body\",\"code\":50035,\"errors\":{\"banner\":{\"_errors\":[{\"code\":"
      "\"BANNER_PREMIUM_REQUIRED\",\"message\":\"Premium is required to set a banner\"}]},\"global_name\":"
      "{\"_errors\":[{\"code\":\"USERNAME_INVALID\",\"message\":\"Name not allowed\"}]}}}";
  fake f;
  CT_CHECK(fake_start(&f, &e, 0, 400, body));
  char name_before[CCORD_NAME_MAX_BYTES + 1], banner_before[512];
  snprintf(name_before, sizeof name_before, "%s", self_user(&e)->name);
  snprintf(banner_before, sizeof banner_before, "%s", self_user(&e)->banner_url);
  uint64_t rev_before = ccord_store_revision(e.store);

  ccord_error err;
  static const uint8_t JPG[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F'};
  ccord_profile_edit edit = CCORD_PROFILE_EDIT_INIT;
  edit.has_display_name = true;
  edit.display_name = "New Name";
  edit.has_banner = true;
  edit.banner = JPG;
  edit.banner_len = sizeof JPG;
  edit.has_bio = true;
  edit.bio = "never sent";
  run r = {0};
  CT_CHECK(ccord_session_edit_profile(e.s, &edit, on_done, &r, &err) == CCORD_OK);
  CT_CHECK(wait_done(&e, &r));
  CT_CHECK(r.res.status == CCORD_ERR_INVALID && r.res.http_status == 400);
  CT_CHECK(r.res.code == 50035 && strcmp(r.res.message, "Invalid Form Body") == 0);
  CT_CHECK(r.res.field_error_count == 2);
  if (r.res.field_error_count == 2) {
    CT_CHECK(r.res.field_errors[0].field == CCORD_PROFILE_FIELD_BANNER);
    CT_CHECK(strcmp(r.res.field_errors[0].code, "BANNER_PREMIUM_REQUIRED") == 0);
    CT_CHECK(strcmp(r.res.field_errors[0].message, "Premium is required to set a banner") == 0);
    CT_CHECK(r.res.field_errors[1].field == CCORD_PROFILE_FIELD_DISPLAY_NAME);
  }
  CT_CHECK(r.res.applied.fields == 0 && r.res.requests_sent == 1);
  CT_CHECK(strcmp(self_user(&e)->name, name_before) == 0);
  CT_CHECK(strcmp(self_user(&e)->banner_url, banner_before) == 0);
  CT_CHECK(ccord_store_revision(e.store) == rev_before);

  fd_hit hits[8];
  size_t n = fd_stop(&f.srv, hits, 8);
  CT_CHECK(n == 1);   // the profile request (bio) is not sent after the first one failed
  env_done(&e);
}

// Pure parsing of the refusals Discord is known to send.
static void test_parse_refusals(void)
{
  ccord_profile_edit_result r;
  memset(&r, 0, sizeof r);
  const char *rl = "{\"message\":\"You are being rate limited.\",\"retry_after\":2.5,\"global\":false}";
  CT_CHECK(!ccord_profile_edit_parse_answer(1u << CCORD_PROFILE_FIELD_AVATAR, 429, rl, strlen(rl), &r));
  CT_CHECK(r.rate_limited && r.http_status == 429 && r.retry_after_s > 2.49 && r.retry_after_s < 2.51);
  CT_CHECK(r.status == CCORD_ERR_RANGE && strcmp(r.message, "You are being rate limited.") == 0);

  memset(&r, 0, sizeof r);
  const char *cap = "{\"captcha_key\":[\"captcha-required\"],\"captcha_sitekey\":\"abc\",\"captcha_service\":\"hcaptcha\"}";
  CT_CHECK(!ccord_profile_edit_parse_answer(1u << CCORD_PROFILE_FIELD_AVATAR, 400, cap, strlen(cap), &r));
  CT_CHECK(r.captcha_required && r.message[0] != '\0');

  memset(&r, 0, sizeof r);
  const char *pw = "{\"message\":\"Password does not match.\",\"code\":50018}";
  CT_CHECK(!ccord_profile_edit_parse_answer(1u << CCORD_PROFILE_FIELD_DISPLAY_NAME, 400, pw, strlen(pw), &r));
  CT_CHECK(r.password_required && r.code == 50018);

  memset(&r, 0, sizeof r);
  const char *pw2 = "{\"message\":\"Invalid Form Body\",\"code\":50035,\"errors\":{\"password\":{\"_errors\":[{\"code\":"
                    "\"PASSWORD_DOES_NOT_MATCH\",\"message\":\"Password does not match.\"}]}}}";
  CT_CHECK(!ccord_profile_edit_parse_answer(1u << CCORD_PROFILE_FIELD_AVATAR, 400, pw2, strlen(pw2), &r));
  CT_CHECK(r.password_required && r.field_error_count == 1 && r.field_errors[0].field == CCORD_PROFILE_FIELD_UNKNOWN);

  memset(&r, 0, sizeof r);
  const char *mfa = "{\"message\":\"Two factor is required for this operation\",\"code\":60003,\"mfa\":{\"ticket\":\"t\"}}";
  CT_CHECK(!ccord_profile_edit_parse_answer(1u << CCORD_PROFILE_FIELD_AVATAR, 400, mfa, strlen(mfa), &r));
  CT_CHECK(r.mfa_required && r.code == 60003);

  memset(&r, 0, sizeof r);
  CT_CHECK(!ccord_profile_edit_parse_answer(1u << CCORD_PROFILE_FIELD_BIO, 502, "<html>bad gateway</html>", 24, &r));
  CT_CHECK(r.status == CCORD_ERR_IO && r.http_status == 502 && r.message[0] != '\0');

  // A 2xx that is not JSON is not trusted.
  memset(&r, 0, sizeof r);
  CT_CHECK(!ccord_profile_edit_parse_answer(1u << CCORD_PROFILE_FIELD_BIO, 200, "nope", 4, &r));
  CT_CHECK(r.status == CCORD_ERR_PARSE && r.applied.fields == 0);
}

static bool has_any(const char *body, const char *const *names, size_t n)
{
  char pat[48];
  for (size_t i = 0; i < n; i++) {
    (void)snprintf(pat, sizeof pat, "\"%s\"", names[i]);
    if (strstr(body, pat)) return true;
  }
  return false;
}

// Hard limits are enforced before anything is sent; a body never carries credentials.
static void test_limits_and_forbidden_fields(void)
{
  ccord_error err;
  ccord_profile_plan plan;
  ccord_profile_edit e = CCORD_PROFILE_EDIT_INIT;

  // Everything at once: two requests, only the six allowed keys, never an account-security field.
  static const uint8_t GIF[] = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0};
  static const uint8_t WEBP[] = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'E', 'B', 'P'};
  e.has_display_name = e.has_avatar = e.has_banner = e.has_bio = e.has_pronouns = e.has_accent = true;
  e.display_name = "Name";
  e.avatar = GIF;
  e.avatar_len = sizeof GIF;
  e.banner = WEBP;
  e.banner_len = sizeof WEBP;
  e.bio = "bio";
  e.pronouns = "they/them";
  e.accent_color = 0x336699;
  CT_CHECK(ccord_profile_edit_plan(&e, &plan, &err) == CCORD_OK);
  CT_CHECK(plan.count == 2);
  if (plan.count == 2) {
    CT_CHECK(strcmp(plan.req[0].path, "/users/@me") == 0 && strcmp(plan.req[1].path, "/users/@me/profile") == 0);
    static const char *const banned[] = {"username", "discriminator", "email", "password", "new_password", "code",
                                         "mfa", "mfa_code", "ticket", "token", "phone", "captcha_key"};
    CT_CHECK(!has_any(plan.req[0].body, banned, sizeof banned / sizeof *banned));
    CT_CHECK(!has_any(plan.req[1].body, banned, sizeof banned / sizeof *banned));
    CT_CHECK(strstr(plan.req[0].body, "\"global_name\":\"Name\"") && strstr(plan.req[0].body, "data:image/gif;base64,") &&
             strstr(plan.req[0].body, "data:image/webp;base64,"));
    CT_CHECK(strcmp(plan.req[1].body, "{\"bio\":\"bio\",\"pronouns\":\"they/them\",\"accent_color\":3368601}") == 0);
  }
  ccord_profile_plan_free(&plan);

  // Clearing: display name / bio / pronouns -> "" or null, accent < 0 -> null.
  ccord_profile_edit c = CCORD_PROFILE_EDIT_INIT;
  c.has_display_name = c.has_bio = c.has_accent = true;
  c.accent_color = -1;
  CT_CHECK(ccord_profile_edit_plan(&c, &plan, &err) == CCORD_OK && plan.count == 2);
  if (plan.count == 2) {
    CT_CHECK(strcmp(plan.req[0].body, "{\"global_name\":null}") == 0);
    CT_CHECK(strcmp(plan.req[1].body, "{\"bio\":\"\",\"accent_color\":null}") == 0);
  }
  ccord_profile_plan_free(&plan);

  // Limits: exactly at the limit passes (characters, not bytes), one over is refused, nothing is built.
  char bio[CCORD_PROFILE_BIO_MAX * 2u + 8u];
  ccord_profile_edit b = CCORD_PROFILE_EDIT_INIT;
  b.has_bio = true;
  b.bio = bio;
  size_t o = 0;
  for (unsigned i = 0; i < CCORD_PROFILE_BIO_MAX; i++) {
    bio[o++] = (char)0xC3;
    bio[o++] = (char)0xA9;   // "é": 2 bytes, 1 character
  }
  bio[o] = '\0';
  CT_CHECK(ccord_profile_edit_plan(&b, &plan, &err) == CCORD_OK);
  ccord_profile_plan_free(&plan);
  bio[o++] = 'x';
  bio[o] = '\0';
  CT_CHECK(ccord_profile_edit_plan(&b, &plan, &err) == CCORD_ERR_RANGE && plan.count == 0);

  char text[64];
  memset(text, 'p', 41);
  text[41] = '\0';
  ccord_profile_edit p = CCORD_PROFILE_EDIT_INIT;
  p.has_pronouns = true;
  p.pronouns = text;
  CT_CHECK(ccord_profile_edit_plan(&p, &plan, &err) == CCORD_ERR_RANGE);
  text[40] = '\0';
  CT_CHECK(ccord_profile_edit_plan(&p, &plan, &err) == CCORD_OK);
  ccord_profile_plan_free(&plan);

  ccord_profile_edit n = CCORD_PROFILE_EDIT_INIT;
  n.has_display_name = true;
  memset(text, 'n', 33);
  text[33] = '\0';
  n.display_name = text;
  CT_CHECK(ccord_profile_edit_plan(&n, &plan, &err) == CCORD_ERR_RANGE);
  text[32] = '\0';
  CT_CHECK(ccord_profile_edit_plan(&n, &plan, &err) == CCORD_OK);
  ccord_profile_plan_free(&plan);
  n.display_name = "bad\x01name";
  CT_CHECK(ccord_profile_edit_plan(&n, &plan, &err) == CCORD_ERR_INVALID);
  n.display_name = "bad\xFF";
  CT_CHECK(ccord_profile_edit_plan(&n, &plan, &err) == CCORD_ERR_INVALID);

  ccord_profile_edit a = CCORD_PROFILE_EDIT_INIT;
  a.has_accent = true;
  a.accent_color = 0x1000000;
  CT_CHECK(ccord_profile_edit_plan(&a, &plan, &err) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_profile_edit_plan(&(ccord_profile_edit)CCORD_PROFILE_EDIT_INIT, &plan, &err) == CCORD_ERR_INVALID);

  // Images: not an image, text posing as one, empty, and too large are refused.
  static const uint8_t TXT[] = "<html>not an image</html>";
  ccord_profile_edit im = CCORD_PROFILE_EDIT_INIT;
  im.has_avatar = true;
  im.avatar = TXT;
  im.avatar_len = sizeof TXT - 1u;
  CT_CHECK(ccord_profile_edit_plan(&im, &plan, &err) == CCORD_ERR_INVALID && plan.count == 0);
  im.avatar = PNG;
  im.avatar_len = 4;   // truncated magic
  CT_CHECK(ccord_profile_edit_plan(&im, &plan, &err) == CCORD_ERR_INVALID);
  im.avatar = nullptr;
  im.avatar_len = 5;   // length without data
  CT_CHECK(ccord_profile_edit_plan(&im, &plan, &err) == CCORD_ERR_INVALID);
  uint8_t *big = malloc(CCORD_PROFILE_IMAGE_MAX_BYTES + 1u);
  CT_CHECK(big != nullptr);
  if (big) {
    memset(big, 0, CCORD_PROFILE_IMAGE_MAX_BYTES + 1u);
    memcpy(big, PNG, sizeof PNG);
    im.avatar = big;
    im.avatar_len = CCORD_PROFILE_IMAGE_MAX_BYTES + 1u;
    CT_CHECK(ccord_profile_edit_plan(&im, &plan, &err) == CCORD_ERR_RANGE && plan.count == 0);
    im.avatar_len = CCORD_PROFILE_IMAGE_MAX_BYTES;   // exactly 10 MB is allowed
    CT_CHECK(ccord_profile_edit_plan(&im, &plan, &err) == CCORD_OK && plan.count == 1);
    if (plan.count == 1) CT_CHECK(plan.req[0].body_len > CCORD_PROFILE_IMAGE_MAX_BYTES / 3u * 4u);
    ccord_profile_plan_free(&plan);
    free(big);
  }
  CT_CHECK(ccord_profile_image_mime((const uint8_t *)"RIFF\0\0\0\0WAVE", 12) == nullptr);
}

// Over-limit input through the session never produces traffic.
static void test_invalid_edit_sends_nothing(void)
{
  test_env e;
  CT_CHECK(env_init(&e));
  if (!e.s) return;
  fake f;
  CT_CHECK(fake_start(&f, &e, -1, 0, nullptr));
  ccord_error err;
  char long_bio[CCORD_PROFILE_BIO_MAX + 2u];
  memset(long_bio, 'b', sizeof long_bio);
  long_bio[sizeof long_bio - 1u] = '\0';
  ccord_profile_edit edit = CCORD_PROFILE_EDIT_INIT;
  edit.has_bio = true;
  edit.bio = long_bio;
  run r = {0};
  CT_CHECK(ccord_session_edit_profile(e.s, &edit, on_done, &r, &err) == CCORD_ERR_RANGE);
  static const uint8_t TXT[] = "GIF";
  ccord_profile_edit im = CCORD_PROFILE_EDIT_INIT;
  im.has_banner = true;
  im.banner = TXT;
  im.banner_len = 3;
  CT_CHECK(ccord_session_edit_profile(e.s, &im, on_done, &r, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_edit_profile(e.s, nullptr, on_done, &r, &err) == CCORD_ERR_INVALID);
  usleep(50 * 1000);
  (void)ccord_session_pump(e.s);
  CT_CHECK(r.calls == 0);
  fd_hit hits[4];
  CT_CHECK(fd_stop(&f.srv, hits, 4) == 0);

  // No backend attached: refused up front, nothing queued.
  ccord_profile_edit ok = CCORD_PROFILE_EDIT_INIT;
  ok.has_bio = true;
  ok.bio = "hi";
  CT_CHECK(ccord_session_set_profile_rest(e.s, nullptr) == CCORD_OK);
  CT_CHECK(ccord_session_edit_profile(e.s, &ok, on_done, &r, &err) == CCORD_ERR_UNSUPPORTED);
  env_done(&e);
}

// The mock backend (--mock): same two routes, answered in-process, store updated from the answer.
static void test_mock_backend(void)
{
  test_env e;
  CT_CHECK(env_init(&e));
  if (!e.s) return;
  ccord_error err;
  ccord_profile_rest *rest = ccord_profile_rest_create_mock(e.self, &err);
  CT_CHECK(rest != nullptr && ccord_session_set_profile_rest(e.s, rest) == CCORD_OK);
  if (!rest) return;

  ccord_profile_edit edit = CCORD_PROFILE_EDIT_INIT;
  edit.has_display_name = edit.has_avatar = edit.has_bio = edit.has_pronouns = edit.has_accent = true;
  edit.display_name = "Mocky";
  edit.avatar = PNG;
  edit.avatar_len = sizeof PNG;
  edit.bio = "from the mock";
  edit.pronouns = "it/its";
  edit.accent_color = 0xFF8800;
  run r = {0};
  CT_CHECK(ccord_session_edit_profile(e.s, &edit, on_done, &r, &err) == CCORD_OK);
  CT_CHECK(wait_done(&e, &r));
  CT_CHECK(r.res.status == CCORD_OK && r.res.requests_sent == 2);
  const ccord_user *u = self_user(&e);
  CT_CHECK(u && strcmp(u->name, "Mocky") == 0 && strcmp(u->bio, "from the mock") == 0 &&
           strcmp(u->pronouns, "it/its") == 0 && u->accent_color == 0xFF8800 && strstr(u->avatar_url, "mockav1"));
  CT_CHECK(ccord_profile_rest_mock_count(rest) == 2);
  CT_CHECK(strcmp(ccord_profile_rest_mock_last_path(rest), "/users/@me/profile") == 0);

  // Clearing really clears (the store's usual upsert would keep the old value).
  ccord_profile_edit clr = CCORD_PROFILE_EDIT_INIT;
  clr.has_display_name = clr.has_avatar = clr.has_bio = clr.has_accent = true;
  clr.accent_color = -1;
  run r2 = {0};
  CT_CHECK(ccord_session_edit_profile(e.s, &clr, on_done, &r2, &err) == CCORD_OK);
  CT_CHECK(wait_done(&e, &r2));
  u = self_user(&e);
  CT_CHECK(r2.res.status == CCORD_OK && u && u->name[0] != '\0' && strcmp(u->name, "Mocky") != 0);   // back to username
  CT_CHECK(u && u->bio[0] == '\0' && u->avatar_url[0] == '\0' && u->accent_color == 0);

  // Injected refusal (e.g. banner needs Nitro) is delivered as a failure and changes nothing.
  ccord_profile_rest_mock_fail_next(rest, 403, "{\"message\":\"Missing Access\",\"code\":50001}");
  ccord_profile_edit bn = CCORD_PROFILE_EDIT_INIT;
  bn.has_bio = true;
  bn.bio = "refused";
  run r3 = {0};
  CT_CHECK(ccord_session_edit_profile(e.s, &bn, on_done, &r3, &err) == CCORD_OK);
  CT_CHECK(wait_done(&e, &r3));
  CT_CHECK(r3.res.status == CCORD_ERR_PERMISSION && r3.res.http_status == 403 && r3.res.code == 50001);
  CT_CHECK(self_user(&e)->bio[0] == '\0');

  // Destroying the session with a request in flight still answers the callback (as a failure), exactly once.
  run r4 = {0};
  CT_CHECK(ccord_session_edit_profile(e.s, &bn, on_done, &r4, &err) == CCORD_OK);
  env_done(&e);
  CT_CHECK(r4.calls == 1 && r4.res.status == CCORD_ERR_IO && r4.res.applied.fields == 0);
}

// Extended profile fields: 2-color theme, avatar decoration, profile effect, nameplate, clan tag, and per-server profile.
static void test_extended_cosmetics_and_guild_profile(void)
{
  test_env e;
  CT_CHECK(env_init(&e));
  if (!e.s) return;
  ccord_error err = {0};
  ccord_profile_rest *rest = ccord_profile_rest_create_mock(e.self, &err);
  CT_CHECK(rest != nullptr && ccord_session_set_profile_rest(e.s, rest) == CCORD_OK);
  if (!rest) { env_done(&e); return; }

  const ccord_guild *g0 = ccord_store_guild_at(e.store, 0);
  ccord_id gid = g0 ? g0->id : 555001ull;

  // 1. Global cosmetics + 2-color Nitro theme + Server Tag (PUT /users/@me/clan)
  ccord_profile_edit ed = CCORD_PROFILE_EDIT_INIT;
  ed.has_theme_colors = true;
  ed.theme_primary = 0x1E3A8Au;
  ed.theme_accent = 0x3B82F6u;
  ed.has_avatar_decoration = true;
  ed.avatar_decoration_sku_id = 121000000000000001ull;
  ed.avatar_decoration_asset = "a_starlight_halo";
  ed.has_profile_effect = true;
  ed.profile_effect_id = "aurora_drift";
  ed.has_nameplate = true;
  ed.nameplate_sku_id = 123000000000000001ull;
  ed.nameplate_asset = "nameplates/cobalt_wave/";
  ed.nameplate_palette = "cobalt";
  ed.has_clan_guild = true;
  ed.clan_guild_id = gid;
  ed.clan_tag = "CORD";
  run r = {0};
  CT_CHECK(ccord_session_edit_profile(e.s, &ed, on_done, &r, &err) == CCORD_OK);
  CT_CHECK(wait_done(&e, &r));
  CT_CHECK(r.res.status == CCORD_OK && r.res.requests_sent == 3);
  CT_CHECK(strcmp(ccord_profile_rest_mock_last_path(rest), "/users/@me/clan") == 0);
  const ccord_user *u = self_user(&e);
  const ccord_user_profile *p = ccord_store_user_profile(e.store, e.self);
  CT_CHECK(u && strcmp(u->nameplate_asset, "nameplates/cobalt_wave/") == 0 && strcmp(u->nameplate_palette, "cobalt") == 0);
  CT_CHECK(u && u->clan_guild_id == gid && strcmp(u->clan_tag, "CORD") == 0);
  CT_CHECK(p && p->has_theme && p->theme_primary == 0x1E3A8Au && p->theme_accent == 0x3B82F6u);

  // 1b. Standalone nameplate PATCH uses Discord's real top-level "nameplate_sku_id" key on /users/@me
  ccord_profile_edit nped = CCORD_PROFILE_EDIT_INIT;
  nped.has_nameplate = true;
  nped.nameplate_sku_id = 123000000000000001ull;
  nped.nameplate_asset = "nameplates/nameplates/cobalt/";
  nped.nameplate_palette = "cobalt";
  run rnp = {0};
  CT_CHECK(ccord_session_edit_profile(e.s, &nped, on_done, &rnp, &err) == CCORD_OK);
  CT_CHECK(wait_done(&e, &rnp));
  CT_CHECK(rnp.res.status == CCORD_OK);
  CT_CHECK(strcmp(ccord_profile_rest_mock_last_path(rest), "/users/@me") == 0);
  CT_CHECK(strstr(ccord_profile_rest_mock_last_body(rest), "\"nameplate_sku_id\":\"123000000000000001\"") != nullptr);

  // 2. Per-server profile edit targets /guilds/{gid}/members/@me and /guilds/{gid}/profile/@me
  //    (and per-server banner rides /guilds/{gid}/profile/@me, NOT /members/@me).
  ccord_profile_edit ged = CCORD_PROFILE_EDIT_INIT;
  ged.guild_id = gid;
  ged.has_display_name = true;
  ged.display_name = "ServerNick";
  ged.has_banner = true;
  ged.banner = PNG;
  ged.banner_len = sizeof PNG;
  ged.has_bio = true;
  ged.bio = "Server-only bio";
  run r2 = {0};
  CT_CHECK(ccord_session_edit_profile(e.s, &ged, on_done, &r2, &err) == CCORD_OK);
  CT_CHECK(wait_done(&e, &r2));
  CT_CHECK(r2.res.status == CCORD_OK && r2.res.requests_sent == 2);
  char expect_path[96];
  snprintf(expect_path, sizeof expect_path, "/guilds/%llu/profile/@me", (unsigned long long)gid);
  CT_CHECK(strcmp(ccord_profile_rest_mock_last_path(rest), expect_path) == 0);
  CT_CHECK(strstr(ccord_profile_rest_mock_last_body(rest), "\"banner\":\"data:image/png;base64,") != nullptr);
  const ccord_member *mem = ccord_store_find_member(e.store, gid, e.self);
  if (mem) CT_CHECK(strcmp(mem->nick, "ServerNick") == 0);
  const ccord_user_profile *gp = ccord_store_user_profile(e.store, e.self);
  CT_CHECK(gp && gp->guild_banner_url && strstr(gp->guild_banner_url, "banners/mockbn") != nullptr);

  // 3. Owned collectibles inventory fetch parses real JSON + enriches profile effects + includes Profile Frames.
  CT_CHECK(ccord_session_fetch_collectibles(e.s, false, &err) == CCORD_OK);
  for (int i = 0; i < 100 && ccord_session_collectibles_state(e.s) == CCORD_FETCH_LOADING; i++) {
    (void)ccord_session_pump(e.s);
    usleep(1000);
  }
  CT_CHECK(ccord_session_collectibles_state(e.s) == CCORD_FETCH_READY);
  size_t ncol = 0;
  const ccord_owned_collectible *cols = ccord_session_collectibles(e.s, &ncol);
  CT_CHECK(cols != nullptr && ncol >= 4);
  bool has_deco = false, has_eff = false, has_np = false, has_frm = false;
  ccord_id eff_id = CCORD_ID_NONE, frm_sku = CCORD_ID_NONE;
  for (size_t i = 0; i < ncol; i++) {
    if (cols[i].kind == CCORD_COLLECTIBLE_AVATAR_DECORATION) has_deco = true;
    if (cols[i].kind == CCORD_COLLECTIBLE_PROFILE_EFFECT) {
      has_eff = true;
      eff_id = cols[i].item_id;
      CT_CHECK(cols[i].has_effect && cols[i].effect.layer_count > 0);
    }
    if (cols[i].kind == CCORD_COLLECTIBLE_NAMEPLATE) has_np = true;
    if (cols[i].kind == CCORD_COLLECTIBLE_PROFILE_FRAME) {
      has_frm = true;
      frm_sku = cols[i].sku_id;
      CT_CHECK(cols[i].has_frame && cols[i].frame.layer_count > 0);
    }
  }
  CT_CHECK(has_deco && has_eff && has_np && has_frm);
  CT_CHECK(ccord_session_find_owned_effect(e.s, eff_id) != nullptr);
  CT_CHECK(ccord_session_find_owned_frame(e.s, frm_sku) != nullptr);

  // 4. Standalone Profile Frame PATCH sends "profile_frame_sku_id" on /users/@me/profile and updates store.
  ccord_profile_edit fed = CCORD_PROFILE_EDIT_INIT;
  fed.has_profile_frame = true;
  fed.profile_frame_sku_id = frm_sku;
  run rf = {0};
  CT_CHECK(ccord_session_edit_profile(e.s, &fed, on_done, &rf, &err) == CCORD_OK);
  CT_CHECK(wait_done(&e, &rf));
  CT_CHECK(rf.res.status == CCORD_OK);
  CT_CHECK(strcmp(ccord_profile_rest_mock_last_path(rest), "/users/@me/profile") == 0);
  CT_CHECK(strstr(ccord_profile_rest_mock_last_body(rest), "\"profile_frame_sku_id\":") != nullptr);
  gp = ccord_store_user_profile(e.store, e.self);
  CT_CHECK(gp && gp->profile_frame_sku_id == frm_sku && gp->profile_frame.sku_id == frm_sku);

  env_done(&e);
}

typedef struct acct_run {
  int calls;
  ccord_account_result res;
} acct_run;

static void on_acct_done(void *ud, const ccord_account_result *res)
{
  acct_run *ar = ud;
  ar->calls++;
  ar->res = *res;
}

static bool wait_acct(test_env *e, acct_run *ar)
{
  for (int i = 0; i < 100 && ar->calls == 0; i++) {
    (void)ccord_session_pump(e->s);
    usleep(1000);
  }
  return ar->calls > 0;
}

// Session RAM-only account vault syncs updates from ccord_session_account_submit and wipes on destroy.
static void test_session_account_vault(void)
{
  test_env e;
  CT_CHECK(env_init(&e));
  if (!e.s) return;
  ccord_error err = {0};
  ccord_account_rest *ar = ccord_account_rest_create_mock(e.self, &err);
  CT_CHECK(ar != nullptr && ccord_session_set_account_rest(e.s, ar) == CCORD_OK);

  const ccord_account_private *ap = ccord_session_account_private(e.s);
  CT_CHECK(ap != nullptr && ap->valid && ap->email[0] != '\0');
  uint64_t rev0 = ccord_session_account_revision(e.s);

  // TOTP enable updates session vault mfa_enabled and bumps revision
  char secret[33] = {0};
  CT_CHECK(ccord_account_generate_totp_secret(secret, &err) == CCORD_OK);
  acct_run run1 = {0};
  ccord_account_req req = {.op = CCORD_ACCT_OP_MFA_TOTP_ENABLE, .value = secret, .code = "123456", .password = "pw123456"};
  CT_CHECK(ccord_session_account_submit(e.s, &req, on_acct_done, &run1, &err) == CCORD_OK);
  CT_CHECK(wait_acct(&e, &run1));
  CT_CHECK(run1.res.status == CCORD_OK);
  ap = ccord_session_account_private(e.s);
  CT_CHECK(ap && ap->mfa_enabled && (ap->authenticator_types & CCORD_AUTH_TYPE_TOTP) != 0);
  CT_CHECK(ccord_session_account_revision(e.s) > rev0);

  // Standing fetch caches standing on session
  acct_run run2 = {0};
  CT_CHECK(ccord_session_account_fetch_standing(e.s, on_acct_done, &run2, &err) == CCORD_OK);
  CT_CHECK(wait_acct(&e, &run2));
  const ccord_account_standing *st = ccord_session_account_standing(e.s);
  CT_CHECK(st && st->loaded && st->tier == CCORD_STANDING_ALL_GOOD);

  // WebAuthn fetch caches credentials on session
  acct_run run3 = {0};
  CT_CHECK(ccord_session_account_fetch_webauthn(e.s, on_acct_done, &run3, &err) == CCORD_OK);
  CT_CHECK(wait_acct(&e, &run3));
  size_t nkeys = 0;
  bool loaded = false;
  const ccord_webauthn_cred *keys = ccord_session_account_webauthn(e.s, &nkeys, &loaded);
  CT_CHECK(loaded && nkeys > 0 && keys != nullptr);

  env_done(&e);
}

int main(void)
{
  test_bio_only();
  test_avatar_set_and_remove();
  test_field_errors_leave_store_unchanged();
  test_parse_refusals();
  test_limits_and_forbidden_fields();
  test_invalid_edit_sends_nothing();
  test_mock_backend();
  test_extended_cosmetics_and_guild_profile();
  test_session_account_vault();
  return CT_RESULT("test_session_profile_edit");
}
