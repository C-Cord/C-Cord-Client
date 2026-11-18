// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// App wiring of the own-profile REST backend (app_profile_rest.c): the --mock backend makes a profile edit through the
// session work (and it does not without the attach call), and the live backend follows the account: after a switch it
// sends with the new token only, after sign-out (detach) or an unreadable token it sends nothing.
#include "fake_discord.h"   // first: it defines the feature-test macros

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ccord/net/mock_transport.h>
#include <ccord/net/profile_edit.h>
#include <ccord/session/session.h>
#if CCORD_LIVE_ACCOUNT
#include <ccord/net/live_transport.h>
#endif

#include "app_profile_rest.h"
#include "ccord_test.h"

static uint64_t g_now = 1000;

static uint64_t fake_now(void *ud)
{
  (void)ud;
  return g_now;
}

typedef struct env {
  ccord_bus *bus;
  ccord_store *store;
  ccord_session *s;
  ccord_id self;
} env;

static bool env_init(env *e)
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

static void env_done(env *e)
{
  ccord_app_profile_rest_detach(e->s);   // what main does at exit; the session would free it too
  ccord_session_destroy(e->s);
  ccord_store_destroy(e->store);
  ccord_bus_destroy(e->bus);
}

typedef struct run {
  int calls;
  ccord_status status;
} run;

static void on_done(void *ud, const ccord_profile_edit_result *r)
{
  run *x = ud;
  x->calls++;
  x->status = r->status;
}

static bool wait_done(env *e, run *x)
{
  for (int i = 0; i < 2500 && x->calls == 0; i++) {
    (void)ccord_session_pump(e->s);
    usleep(2000);
  }
  return x->calls > 0;
}

static ccord_status edit_bio(env *e, const char *bio, run *r)
{
  ccord_error err;
  ccord_profile_edit edit = CCORD_PROFILE_EDIT_INIT;
  edit.has_bio = true;
  edit.bio = bio;
  return ccord_session_edit_profile(e->s, &edit, on_done, r, &err);
}

// --mock: an edit completes and the store's own profile shows it; without the attach there is no backend.
static void test_mock_edit(void)
{
  env e;
  CT_CHECK(env_init(&e));
  if (!e.s) return;
  run r0 = {0};
  CT_CHECK(edit_bio(&e, "nobody home", &r0) == CCORD_ERR_UNSUPPORTED);   // not attached yet
  CT_CHECK(r0.calls == 0);

  ccord_error err;
  CT_CHECK(ccord_app_profile_rest_attach_mock(e.s, e.self, &err));
  run r = {0};
  CT_CHECK(edit_bio(&e, "Writes C, drinks tea", &r) == CCORD_OK);
  CT_CHECK(wait_done(&e, &r));
  CT_CHECK(r.status == CCORD_OK);
  const ccord_user *u = ccord_store_user(e.store, e.self);
  CT_CHECK(u && strcmp(u->bio, "Writes C, drinks tea") == 0);

  ccord_app_profile_rest_detach(e.s);
  run r2 = {0};
  CT_CHECK(edit_bio(&e, "again", &r2) == CCORD_ERR_UNSUPPORTED);
  env_done(&e);
}

#if CCORD_LIVE_ACCOUNT
#define TOKEN_A "REDACTED_TEST_TOKEN_A"
#define TOKEN_B "REDACTED_TEST_TOKEN_B"

// Account switch / sign-out: every request carries the token of the account connected at that moment.
static void test_live_follows_account(void)
{
  unsetenv(CCORD_LIVE_TOKEN_ENV);   // the env token would win over the token file
  const char *tmp = getenv("TMPDIR");
  char dir[256];
  (void)snprintf(dir, sizeof dir, "%s/ccord_prof_XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
  CT_CHECK(mkdtemp(dir) != nullptr);
  char path[300];
  (void)snprintf(path, sizeof path, "%s/token", dir);

  env e;
  CT_CHECK(env_init(&e));
  if (!e.s) return;
  struct { fd_server srv; } f = {0};
  fd_profile_cfg cfg = {.self_id = e.self, .fail_idx = -1};
  CT_CHECK(fd_start(&f.srv, fd_profile_respond, &cfg));

  ccord_error err;
  // Account A.
  CT_CHECK(ccord_live_save_token(path, TOKEN_A, &err) == CCORD_OK);
  CT_CHECK(ccord_app_profile_rest_attach_live(e.s, path, f.srv.base, &err));
  run r1 = {0};
  CT_CHECK(edit_bio(&e, "from A", &r1) == CCORD_OK);
  CT_CHECK(wait_done(&e, &r1) && r1.status == CCORD_OK);

  // Switch to account B: the app rewrites the token file and rebuilds the backend (app_connect_live).
  CT_CHECK(ccord_live_save_token(path, TOKEN_B, &err) == CCORD_OK);
  CT_CHECK(ccord_app_profile_rest_attach_live(e.s, path, f.srv.base, &err));
  run r2 = {0};
  CT_CHECK(edit_bio(&e, "from B", &r2) == CCORD_OK);
  CT_CHECK(wait_done(&e, &r2) && r2.status == CCORD_OK);

  // Sign-out (app_disconnect_live): nothing can be sent any more.
  ccord_app_profile_rest_detach(e.s);
  run r3 = {0};
  CT_CHECK(edit_bio(&e, "signed out", &r3) == CCORD_ERR_UNSUPPORTED);

  // A token that cannot be read leaves no backend, not the previous account's.
  CT_CHECK(ccord_live_save_token(path, TOKEN_A, &err) == CCORD_OK);
  CT_CHECK(ccord_app_profile_rest_attach_live(e.s, path, f.srv.base, &err));
  (void)unlink(path);
  CT_CHECK(!ccord_app_profile_rest_attach_live(e.s, path, f.srv.base, &err));
  run r4 = {0};
  CT_CHECK(edit_bio(&e, "no token", &r4) == CCORD_ERR_UNSUPPORTED);

  fd_hit hits[8];
  size_t n = fd_stop(&f.srv, hits, 8);
  CT_CHECK(n == 2);
  if (n == 2) {
    CT_CHECK(strcmp(hits[0].authorization, TOKEN_A) == 0);
    CT_CHECK(strcmp(hits[1].authorization, TOKEN_B) == 0);
  }
  env_done(&e);
  (void)rmdir(dir);
}
#endif

int main(void)
{
  test_mock_edit();
#if CCORD_LIVE_ACCOUNT
  test_live_follows_account();
#endif
  return CT_RESULT("test_app_profile_rest");
}
