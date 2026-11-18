// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Token-only Account glue (M0-8): token-state transitions, the failed-reconnect transport ownership contract
// behind app_connect_live, and the retired fixture-OAuth CLI surface. No fixture, OAuth or HTTP object exists.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ccord/core/event_bus.h>
#include <ccord/net/transport.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>

#include "account.h"

#include "ccord_test.h"

#ifndef CCORD_BIN
#error "CCORD_BIN must point to the ccord executable"
#endif

// ---------------------------------------------------------------- token-state cases

static void test_default_construction(void)
{
  ccord_error err = {0};
  ccord_account *a = ccord_account_create(&err);
  CT_CHECK(a != nullptr);
  if (!a) return;
  const ccord_account_view *v = ccord_account_view_of(a);
  CT_CHECK(v && v->mode == CCORD_ACCOUNT_UNAVAILABLE);
  CT_CHECK(strcmp(v->status, "Not configured in this build") == 0);
  CT_CHECK(!v->signed_in && !v->can_sign_in && !v->can_sign_out && !v->demo_chip_visible);
  CT_CHECK(ccord_account_identity(a) == nullptr);
  CT_CHECK(!ccord_account_has_token_session(a));

  // Sign-in and refresh are answered, never started: no OAuth flow exists in this build.
  CT_CHECK(ccord_account_begin(a, &err) == CCORD_ERR_UNSUPPORTED);
  CT_CHECK(ccord_account_refresh(a, &err) == CCORD_ERR_UNSUPPORTED);
  CT_CHECK(ccord_account_sign_out(a, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_account_view_of(a)->mode == CCORD_ACCOUNT_UNAVAILABLE);
  ccord_account_destroy(a);

  // Null handles are ignored everywhere.
  CT_CHECK(ccord_account_view_of(nullptr) == nullptr);
  CT_CHECK(ccord_account_identity(nullptr) == nullptr);
  CT_CHECK(!ccord_account_has_token_session(nullptr));
  ccord_account_set_token_state(nullptr, true, true, "x", "y", 1, 1);
  ccord_account_destroy(nullptr);
  CT_CHECK(ccord_account_begin(nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_account_refresh(nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_account_sign_out(nullptr, &err) == CCORD_ERR_INVALID);
}

static void test_token_state_transitions(void)
{
  ccord_error err = {0};
  ccord_account *a = ccord_account_create(&err);
  CT_CHECK(a != nullptr);
  if (!a) return;

  // Saved token, gateway not connected yet.
  ccord_account_set_token_state(a, true, false, "Alice", "alice", 123456789012345678ULL, 2);
  const ccord_account_view *v = ccord_account_view_of(a);
  CT_CHECK(v->mode == CCORD_ACCOUNT_SIGNED_IN && v->signed_in);
  CT_CHECK(strcmp(v->status, "Saved Discord token active") == 0);
  CT_CHECK(v->can_sign_out && !v->can_refresh && !v->can_sign_in);
  CT_CHECK(!v->demo_chip_visible && !v->show_non_goals);
  CT_CHECK(strcmp(v->display_name, "Alice") == 0 && strcmp(v->username, "alice") == 0);
  CT_CHECK(strcmp(v->user_id_text, "123456789012345678") == 0);
  CT_CHECK(v->guild_count == 2 && !v->guilds_truncated);
  CT_CHECK(ccord_account_has_token_session(a));
  const ccord_identity *id = ccord_account_identity(a);
  CT_CHECK(id != nullptr && id->user_id == 123456789012345678ULL && id->guild_count == 2);
  CT_CHECK(v->identity == id);

  // Connected: the status line follows the live flag.
  ccord_account_set_token_state(a, true, true, "Alice", "alice", 123456789012345678ULL, 2);
  CT_CHECK(strcmp(v->status, "Signed in to Discord Live") == 0);
  CT_CHECK(strcmp(v->detail, "Connected to Discord Gateway v9 and REST v9.") == 0);

  // Sign out clears the session and the identity.
  CT_CHECK(ccord_account_sign_out(a, &err) == CCORD_OK);
  CT_CHECK(v->mode == CCORD_ACCOUNT_UNAVAILABLE && !v->signed_in);
  CT_CHECK(!ccord_account_has_token_session(a));
  CT_CHECK(ccord_account_identity(a) == nullptr);
  CT_CHECK(ccord_account_sign_out(a, &err) == CCORD_ERR_INVALID);
  ccord_account_destroy(a);
}

static void test_token_state_defaults(void)
{
  ccord_error err = {0};
  ccord_account *a = ccord_account_create(&err);
  CT_CHECK(a != nullptr);
  if (!a) return;

  // Missing names and a zero user id fall back to readable defaults.
  ccord_account_set_token_state(a, true, false, nullptr, nullptr, 0, 0);
  const ccord_account_view *v = ccord_account_view_of(a);
  CT_CHECK(strcmp(v->display_name, "Discord User") == 0);
  CT_CHECK(strcmp(v->username, "discord_user") == 0);
  CT_CHECK(strcmp(v->user_id_text, "1") == 0);
  CT_CHECK(v->guild_count == 0);
  ccord_account_set_token_state(a, true, false, "", "", 7, 0);
  CT_CHECK(strcmp(v->display_name, "Discord User") == 0);
  CT_CHECK(strcmp(v->username, "discord_user") == 0);

  // The guild count is clamped to the documented identity limit.
  ccord_account_set_token_state(a, true, false, "A", "a", 7, CCORD_IDENTITY_MAX_GUILDS + 50);
  CT_CHECK(v->guild_count == CCORD_IDENTITY_MAX_GUILDS);
  CT_CHECK(ccord_account_identity(a)->guild_count == CCORD_IDENTITY_MAX_GUILDS);
  ccord_account_set_token_state(a, true, false, "A", "a", 7, CCORD_IDENTITY_MAX_GUILDS);
  CT_CHECK(v->guild_count == CCORD_IDENTITY_MAX_GUILDS);

  // A fresh activation never leaks the previous session's names.
  ccord_account_set_token_state(a, false, false, nullptr, nullptr, 0, 0);
  ccord_account_set_token_state(a, true, false, nullptr, nullptr, 0, 0);
  CT_CHECK(strcmp(v->display_name, "Discord User") == 0);
  CT_CHECK(strcmp(v->username, "discord_user") == 0);
  CT_CHECK(ccord_account_identity(a)->guild_count == 0);
  ccord_account_destroy(a);
}

static int entry_index(const ccord_account_entry *entries, size_t count, uint64_t user_id)
{
  for (size_t i = 0; i < count; i++) {
    if (entries[i].user_id == user_id) return (int)i;
  }
  return -1;
}

static void test_account_roster(void)
{
  ccord_error err = {0};
  ccord_account *a = ccord_account_create(&err);
  CT_CHECK(a != nullptr);
  if (!a) return;

  ccord_account_entry entries[CCORD_ACCOUNT_MAX_SAVED];
  CT_CHECK(ccord_account_list(a, entries, CCORD_ACCOUNT_MAX_SAVED) == 0);
  CT_CHECK(ccord_account_list(nullptr, entries, CCORD_ACCOUNT_MAX_SAVED) == 0);

  // The first explicit roster row becomes active even when the caller leaves the flag clear.
  ccord_account *fresh = ccord_account_create(&err);
  ccord_account_entry first = {0};
  first.user_id = 444;
  (void)snprintf(first.username, sizeof first.username, "first");
  CT_CHECK(fresh && ccord_account_upsert(fresh, &first, &err) == CCORD_OK);
  CT_CHECK(ccord_account_list(fresh, entries, CCORD_ACCOUNT_MAX_SAVED) == 1 && entries[0].active);
  ccord_account_destroy(fresh);

  // Signing in auto-upserts the identity; a second sign-in keeps both rows.
  ccord_account_set_token_state(a, true, false, "Alice", "alice", 111, 2);
  CT_CHECK(ccord_account_list(a, entries, CCORD_ACCOUNT_MAX_SAVED) == 1);
  CT_CHECK(entries[0].user_id == 111 && strcmp(entries[0].username, "alice") == 0 &&
           strcmp(entries[0].display_name, "Alice") == 0 && entries[0].active);
  ccord_account_set_avatar_hash(a, "av_alice");

  ccord_account_set_token_state(a, true, false, "Bob", "bob", 222, 0);
  CT_CHECK(ccord_account_list(a, entries, CCORD_ACCOUNT_MAX_SAVED) == 2);
  CT_CHECK(entry_index(entries, 2, 111) >= 0 && entry_index(entries, 2, 222) >= 0);
  CT_CHECK(entries[entry_index(entries, 2, 222)].active);
  CT_CHECK(strcmp(entries[entry_index(entries, 2, 111)].avatar_hash, "av_alice") == 0);

  // An explicit roster row with metadata (including the avatar) is stored.
  ccord_account_entry gamma = {0};
  gamma.user_id = 333;
  (void)snprintf(gamma.username, sizeof gamma.username, "gamma");
  (void)snprintf(gamma.display_name, sizeof gamma.display_name, "Gamma");
  (void)snprintf(gamma.avatar_hash, sizeof gamma.avatar_hash, "av_gamma");
  gamma.active = false;
  CT_CHECK(ccord_account_upsert(a, &gamma, &err) == CCORD_OK);
  CT_CHECK(ccord_account_list(a, entries, CCORD_ACCOUNT_MAX_SAVED) == 3);
  int gi = entry_index(entries, 3, 333);
  CT_CHECK(gi >= 0 && strcmp(entries[gi].display_name, "Gamma") == 0 &&
           strcmp(entries[gi].avatar_hash, "av_gamma") == 0 && !entries[gi].active);

  // Switching rebuilds the identity and view without a new token session.
  CT_CHECK(ccord_account_switch(a, 111, &err) == CCORD_OK);
  CT_CHECK(ccord_account_list(a, entries, CCORD_ACCOUNT_MAX_SAVED) == 3);
  CT_CHECK(entries[entry_index(entries, 3, 111)].active);
  const ccord_identity *id = ccord_account_identity(a);
  const ccord_account_view *v = ccord_account_view_of(a);
  CT_CHECK(id && id->user_id == 111 && strcmp(id->display_name, "Alice") == 0);
  CT_CHECK(v->signed_in && strcmp(v->display_name, "Alice") == 0 && strcmp(v->username, "alice") == 0);
  CT_CHECK(strcmp(v->user_id_text, "111") == 0);

  // Removing the active account selects the next row and its identity.
  CT_CHECK(ccord_account_remove(a, 111, &err) == CCORD_OK);
  CT_CHECK(ccord_account_list(a, entries, CCORD_ACCOUNT_MAX_SAVED) == 2);
  CT_CHECK(ccord_account_identity(a)->user_id == 222);
  CT_CHECK(strcmp(ccord_account_identity(a)->display_name, "Bob") == 0);

  CT_CHECK(ccord_account_remove(a, 222, &err) == CCORD_OK);
  CT_CHECK(ccord_account_list(a, entries, CCORD_ACCOUNT_MAX_SAVED) == 1);
  CT_CHECK(ccord_account_identity(a)->user_id == 333);
  CT_CHECK(ccord_account_remove(a, 333, &err) == CCORD_OK);
  CT_CHECK(ccord_account_list(a, entries, CCORD_ACCOUNT_MAX_SAVED) == 0);
  CT_CHECK(ccord_account_identity(a) == nullptr);
  CT_CHECK(!ccord_account_has_token_session(a));
  CT_CHECK(ccord_account_view_of(a)->mode == CCORD_ACCOUNT_UNAVAILABLE);

  // Unknown users and null handles are rejected.
  CT_CHECK(ccord_account_switch(a, 999, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_account_remove(a, 999, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_account_switch(nullptr, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_account_remove(nullptr, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_account_upsert(a, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_account_set_avatar_hash(nullptr, "x");
  ccord_account_set_avatar_hash(a, "x");   // no active account: ignored

  // Begin/refresh with an active token session keep the roster in sync.
  ccord_account_set_token_state(a, true, false, "Alice", "alice", 111, 2);
  CT_CHECK(ccord_account_list(a, entries, CCORD_ACCOUNT_MAX_SAVED) == 1);
  CT_CHECK(ccord_account_begin(a, &err) == CCORD_OK);
  CT_CHECK(ccord_account_refresh(a, &err) == CCORD_OK);
  CT_CHECK(ccord_account_list(a, entries, CCORD_ACCOUNT_MAX_SAVED) == 1);
  ccord_account_destroy(a);
}

// ---------------------------------------------------------------- failed-reconnect ownership regression
//
// app_connect_live (src/app/main.c) swaps the session transport on every "Save & Connect". The session destroys
// the old transport BEFORE connecting and keeps the new one even when connect fails, so the app's non-owning
// mirror must be updated unconditionally. This pins that contract with a transport whose connect fails: the old
// object is destroyed exactly once, the new one stays session-owned, and reading the mirror afterwards (what
// --net-stats does at exit) is safe. Under ASan a stale mirror would be a heap-use-after-free here.

typedef struct stub_transport {
  ccord_transport base;
  bool fail_connect;
  int *destroys;
} stub_transport;

static ccord_status stub_connect(ccord_transport *t, ccord_error *err)
{
  stub_transport *s = (stub_transport *)t;
  if (s->fail_connect) {
    return ccord_error_set(err, CCORD_ERR_IO, "stub: connect failed");
  }
  return CCORD_OK;
}

static void stub_disconnect(ccord_transport *t)
{
  (void)t;
}

static bool stub_poll(ccord_transport *t, ccord_net_event *out)
{
  (void)t;
  (void)out;
  return false;
}

static void stub_destroy(ccord_transport *t)
{
  stub_transport *s = (stub_transport *)t;
  (*s->destroys)++;
  free(s);
}

static const ccord_transport_vtable k_stub_vt = {
  .name = "stub",
  .connect = stub_connect,
  .disconnect = stub_disconnect,
  .poll = stub_poll,
  .destroy = stub_destroy,
};

static ccord_transport *stub_create(bool fail_connect, int *destroys)
{
  stub_transport *s = calloc(1, sizeof *s);
  if (!s) return nullptr;
  s->base.vt = &k_stub_vt;
  s->fail_connect = fail_connect;
  s->destroys = destroys;
  return &s->base;
}

static void test_failed_reconnect_transport_ownership(void)
{
  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = bus ? ccord_store_create(bus, &err) : nullptr;
  int destroys_a = 0, destroys_b = 0;
  ccord_transport *a = stub_create(false, &destroys_a);
  CT_CHECK(bus && store && a);
  if (!bus || !store || !a) return;
  ccord_session *s = ccord_session_create(bus, store, a, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  ccord_transport *mirror = a;   // the app's non-owning mirror after the first connect

  // Repeat connect: the new transport's connect fails synchronously (network error analogue).
  ccord_transport *b = stub_create(true, &destroys_b);
  CT_CHECK(b != nullptr);
  if (!b) return;
  ccord_status st = ccord_session_set_transport(s, b, &err);
  CT_CHECK(st != CCORD_OK);
  CT_CHECK(destroys_a == 1);   // the old transport is gone exactly once, before connect was attempted
  CT_CHECK(destroys_b == 0);   // the session keeps the new one even though connect failed

  // The fixed app_connect_live mirrors the new pointer unconditionally; reading it must be safe.
  mirror = b;
  CT_CHECK(mirror->vt == &k_stub_vt);
  CT_CHECK(ccord_session_state(s) == CCORD_CONN_DISCONNECTED);
  CT_CHECK(ccord_session_pump(s) == 0);   // the session still runs on the retained transport

  ccord_session_destroy(s);
  CT_CHECK(destroys_a == 1 && destroys_b == 1);   // no double free, no leak
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// ---------------------------------------------------------------- retired fixture CLI

// Runs ccord and captures stdout+stderr into out (NUL-terminated, truncated to cap - 1).
static int run_capture(const char *const *argv, char *out, size_t cap)
{
  int fds[2];
  if (pipe(fds) != 0) return -1;
  char *copy[40];
  size_t n = 0;
  for (; argv[n] && n + 1 < sizeof copy / sizeof copy[0]; n++) {
    copy[n] = strdup(argv[n]);
    if (!copy[n]) return -1;
  }
  copy[n] = nullptr;
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    execv(CCORD_BIN, copy);
    _exit(127);
  }
  close(fds[1]);
  for (size_t i = 0; i < n; i++) free(copy[i]);
  size_t used = 0;
  ssize_t got;
  while (used + 1 < cap && (got = read(fds[0], out + used, cap - 1 - used)) > 0) used += (size_t)got;
  out[used] = '\0';
  close(fds[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

static void test_fixture_cli_retired(void)
{
  char out[8192];
  char data_dir[64];
  snprintf(data_dir, sizeof data_dir, "app_account_data_%ld", (long)getpid());
  const char *shot = "app_account_token.png";

  // Default offline run: the Account page reports the signed-out state, no demo chip.
  const char *const plain[] = {CCORD_BIN, "--headless", "--no-plugins", "--data-dir", data_dir,
                               "--screenshot", shot, "--size", "640x400", nullptr};
  remove(shot);
  CT_CHECK(run_capture(plain, out, sizeof out) == 0);
  CT_CHECK(strstr(out, "account=unavailable") != nullptr);
  CT_CHECK(strstr(out, "demo_chip=0") != nullptr);
  CT_CHECK(access(shot, F_OK) == 0);
  remove(shot);

  // The fixture-OAuth flags are gone: each is an unknown option (exit 2), and --help no longer lists them.
  const char *const fixture[] = {CCORD_BIN, "--headless", "--no-plugins", "--oauth-fixture", "/nonexistent", nullptr};
  CT_CHECK(run_capture(fixture, out, sizeof out) == 2);
  CT_CHECK(strstr(out, "unknown option --oauth-fixture") != nullptr);
  const char *const client_id[] = {CCORD_BIN, "--headless", "--no-plugins", "--oauth-client-id", "123", nullptr};
  CT_CHECK(run_capture(client_id, out, sizeof out) == 2);
  CT_CHECK(strstr(out, "unknown option --oauth-client-id") != nullptr);
  const char *const sign_in[] = {CCORD_BIN, "--headless", "--no-plugins", "--sign-in", nullptr};
  CT_CHECK(run_capture(sign_in, out, sizeof out) == 2);
  CT_CHECK(strstr(out, "unknown option --sign-in") != nullptr);
  const char *const help[] = {CCORD_BIN, "--help", nullptr};
  CT_CHECK(run_capture(help, out, sizeof out) == 0);
  CT_CHECK(strstr(out, "--oauth-fixture") == nullptr);
  CT_CHECK(strstr(out, "--oauth-client-id") == nullptr);
  CT_CHECK(strstr(out, "--sign-in") == nullptr);

  char path[128];
  snprintf(path, sizeof path, "%s/settings.ini", data_dir);
  remove(path);
  snprintf(path, sizeof path, "%s/drafts.json", data_dir);
  remove(path);
  (void)rmdir(data_dir);
}

int main(void)
{
  (void)setenv("SDL_VIDEODRIVER", "dummy", 1);
  test_default_construction();
  test_token_state_transitions();
  test_token_state_defaults();
  test_account_roster();
  test_failed_reconnect_transport_ownership();
  test_fixture_cli_retired();
  return CT_RESULT("test_app_account");
}
