// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Warm-start disk cache (warm_cache.c): round trip through a real store, 0600/0700 modes, integrity rejection
// (truncation, flipped byte, wrong version, oversize, group-readable), per-account isolation, forget, pruning and the
// size cap. Everything lives under a fresh directory below $TMPDIR (or /tmp).
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ccord/core/alloc.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/sha256.h>
#include <ccord/net/mock_transport.h>

#include "ccord_test.h"
#include "warm_cache.h"

static uint64_t g_now = 1000;

static uint64_t fake_now(void *ud)
{
  (void)ud;
  return g_now;
}

static int64_t unix_now(void *ud)
{
  (void)ud;
  return 1700000000000;
}

typedef struct env {
  ccord_bus *bus;
  ccord_store *store;
  ccord_session *s;
} env;

static bool env_init(env *e, bool started)
{
  memset(e, 0, sizeof *e);
  ccord_error err;
  e->bus = ccord_bus_create(&err);
  e->store = ccord_store_create(e->bus, &err);
  ccord_mock_config cfg = {.seed = 7, .latency_ms = 0, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  if (!e->bus || !e->store || !tr) return false;
  e->s = ccord_session_create(e->bus, e->store, tr, &err);   // takes ownership of tr
  if (!e->s) return false;
  if (!started) return true;
  if (ccord_session_start(e->s, &err) != CCORD_OK) return false;
  for (int i = 0; i < 40; i++) {
    g_now += 50;
    (void)ccord_session_pump(e->s);
  }
  return ccord_session_state(e->s) == CCORD_CONN_CONNECTED && ccord_store_guild_count(e->store) > 0;
}

static void env_done(env *e)
{
  ccord_session_destroy(e->s);
  ccord_store_destroy(e->store);
  ccord_bus_destroy(e->bus);
}

static ccord_warm *open_warm(const char *dir, const char *token, size_t max_bytes)
{
  ccord_error err;
  ccord_warm_config wc = {.data_dir = dir, .token = token, .app_version = "test", .now_ms = unix_now, .enabled = true,
                          .max_file_bytes = max_bytes};
  return ccord_warm_open(&wc, &err);
}

static unsigned mode_of(const char *path)
{
  struct stat st;
  return lstat(path, &st) == 0 ? (unsigned)(st.st_mode & 0777) : 0xffffu;
}

static char g_root[900];

static void fresh_root(const char *name)
{
  ccord_error err;
  if (g_root[0]) (void)ccord_rmtree(g_root, &err);   // the previous test's directory
  const char *t = getenv("TMPDIR");
  snprintf(g_root, sizeof g_root, "%s/ccord-test-warm-%s-%d", t && *t ? t : "/tmp", name, (int)getpid());
  (void)ccord_rmtree(g_root, &err);
  (void)ccord_mkdir_p(g_root, 0700, &err);
}

static bool write_raw(const char *path, const void *p, size_t n, unsigned mode)
{
  ccord_error err;
  return ccord_file_write_atomic(path, p, n, mode, false, &err) == CCORD_OK;
}

static void test_roundtrip_and_modes(void)
{
  fresh_root("rt");
  (void)umask(0);   // modes must come from the code, not the caller's umask
  env a;
  CT_CHECK(env_init(&a, true));
  ccord_id g = ccord_store_selected_guild(a.store), c = ccord_store_selected_channel(a.store);
  ccord_error err;
  if (c != CCORD_ID_NONE) {
    (void)ccord_session_select_channel(a.s, c, &err);
    for (int i = 0; i < 40; i++) {
      g_now += 50;
      (void)ccord_session_pump(a.s);
    }
  }
  size_t guilds = ccord_store_guild_count(a.store), chans = ccord_store_channel_count(a.store, g);
  size_t msgs = c != CCORD_ID_NONE ? ccord_store_message_count(a.store, c) : 0;
  CT_CHECK(guilds > 0 && chans > 0);

  ccord_warm *w = open_warm(g_root, "tok-a", 0);
  CT_CHECK(w && ccord_warm_enabled(w) && ccord_warm_dir(w));
  char sp[1300];
  CT_CHECK(ccord_warm_state_path(w, sp, sizeof sp));
  CT_CHECK(ccord_warm_save(w, a.store, true, &err) == CCORD_OK);
  CT_CHECK(mode_of(sp) == 0600);
  CT_CHECK(mode_of(ccord_warm_dir(w)) == 0700);
  char top[1000];
  snprintf(top, sizeof top, "%s/accounts", g_root);
  CT_CHECK(mode_of(top) == 0700);
  int64_t saved = 0;
  size_t body = 0;
  CT_CHECK(ccord_warm_verify_file(sp, &saved, &body, 0) && saved == 1700000000000 && body > 100);

  env b;
  CT_CHECK(env_init(&b, false));
  ccord_warm_load_result r = {0};
  CT_CHECK(ccord_warm_load(w, b.store, b.s, &r, &err) == CCORD_OK);
  CT_CHECK(r.loaded && r.saved_ms == 1700000000000);
  CT_CHECK(ccord_session_is_stale(b.s));
  CT_CHECK(ccord_store_guild_count(b.store) == guilds);
  CT_CHECK(ccord_store_selected_guild(b.store) == g && ccord_store_selected_channel(b.store) == c);
  CT_CHECK(ccord_store_channel_count(b.store, g) == chans);
  CT_CHECK(ccord_store_self_id(b.store) == ccord_store_self_id(a.store));
  for (size_t i = 0; i < guilds; i++) {
    const ccord_guild *x = ccord_store_guild_at(a.store, i), *y = ccord_store_guild_at(b.store, i);
    CT_CHECK(x && y && x->id == y->id && strcmp(x->name, y->name) == 0);
  }
  CT_CHECK(c == CCORD_ID_NONE || msgs == 0 || ccord_store_message_count(b.store, c) > 0 || r.history_channels == 0);

  // A second save of an unchanged store through maybe_save is rate limited / needs the connection.
  CT_CHECK(!ccord_warm_maybe_save(w, b.store, b.s, 1000000));   // stale session -> nothing
  env_done(&b);
  ccord_warm_close(w);
  env_done(&a);
}

static void test_rejections(void)
{
  fresh_root("rej");
  env a;
  CT_CHECK(env_init(&a, true));
  ccord_error err;
  ccord_warm *w = open_warm(g_root, "tok-a", 0);
  CT_CHECK(w && ccord_warm_save(w, a.store, false, &err) == CCORD_OK);
  char sp[1300];
  CT_CHECK(ccord_warm_state_path(w, sp, sizeof sp));
  size_t n = 0;
  char *good = ccord_file_read_max(sp, CCORD_WARM_MAX_FILE_BYTES + 200u, &n, &err);
  CT_CHECK(good && n > 200);
  const char *nl = good ? memchr(good, '\n', n) : nullptr;
  size_t head = nl ? (size_t)(nl - good) + 1 : 0;
  CT_CHECK(head > 20 && head < 160);

  for (int kind = 0; kind < 6; kind++) {
    char *bad = ccord_malloc(n + 32);
    CT_CHECK(bad != nullptr);
    size_t bn = n;
    unsigned mode = 0600;
    memcpy(bad, good, n);
    if (kind == 0) bn = n / 2;                                  // truncated
    else if (kind == 1) bad[head + 50] ^= 1;                    // flipped body byte
    else if (kind == 2) memcpy(bad + 7, "9", 1);                // header version "CCWARM 9"
    else if (kind == 3) mode = 0644;                            // group/other readable
    else if (kind == 4) bn = head;                              // header only
    else {                                                      // empty file
      bn = 0;
    }
    CT_CHECK(write_raw(sp, bad, bn, mode));
    ccord_free(bad);
    env b;
    CT_CHECK(env_init(&b, false));
    ccord_warm_load_result r = {0};
    ccord_status st = ccord_warm_load(w, b.store, b.s, &r, &err);
    CT_CHECK(st == CCORD_OK && !r.loaded);
    CT_CHECK(!ccord_session_is_stale(b.s) && ccord_store_guild_count(b.store) == 0);
    CT_CHECK(access(sp, F_OK) != 0);   // the bad file is gone
    env_done(&b);
  }

  // A file bigger than the configured cap is not read.
  CT_CHECK(write_raw(sp, good, n, 0600));
  ccord_warm *small = open_warm(g_root, "tok-a", 1024);
  env c;
  CT_CHECK(small && env_init(&c, false));
  ccord_warm_load_result r = {0};
  CT_CHECK(ccord_warm_load(small, c.store, c.s, &r, &err) == CCORD_OK && !r.loaded);
  env_done(&c);
  ccord_warm_close(small);
  CT_CHECK(write_raw(sp, good, n, 0600));

  // Another token is another account: its own directory, the first one's file is not read and not touched.
  ccord_warm *other = open_warm(g_root, "tok-b", 0);
  CT_CHECK(other && strcmp(ccord_warm_dir(other), ccord_warm_dir(w)) != 0);
  env d;
  CT_CHECK(env_init(&d, false));
  CT_CHECK(ccord_warm_load(other, d.store, d.s, &r, &err) == CCORD_OK && !r.loaded);
  env_done(&d);
  CT_CHECK(access(sp, F_OK) == 0);
  // A state file copied into the other account's directory (fp mismatch inside) is refused too.
  char sp2[1300];
  CT_CHECK(ccord_warm_state_path(other, sp2, sizeof sp2));
  char wd2[1300];
  snprintf(wd2, sizeof wd2, "%s/warm", ccord_warm_dir(other));
  (void)ccord_mkdir_p(wd2, 0700, &err);
  CT_CHECK(write_raw(sp2, good, n, 0600));
  env e2;
  CT_CHECK(env_init(&e2, false));
  CT_CHECK(ccord_warm_load(other, e2.store, e2.s, &r, &err) == CCORD_OK && !r.loaded);
  CT_CHECK(ccord_store_guild_count(e2.store) == 0);
  env_done(&e2);
  ccord_warm_close(other);
  ccord_free(good);
  ccord_warm_close(w);
  env_done(&a);
}

static void test_disabled(void)
{
  fresh_root("off");
  ccord_error err;
  ccord_warm_config wc = {.data_dir = g_root, .token = "tok", .app_version = "t", .enabled = false};
  ccord_warm *w = ccord_warm_open(&wc, &err);
  CT_CHECK(w && !ccord_warm_enabled(w) && ccord_warm_dir(w) == nullptr);
  env a;
  CT_CHECK(env_init(&a, true));
  CT_CHECK(ccord_warm_save(w, a.store, true, &err) == CCORD_OK);
  CT_CHECK(!ccord_warm_maybe_save(w, a.store, a.s, 10000000));
  ccord_warm_load_result r = {0};
  CT_CHECK(ccord_warm_load(w, a.store, a.s, &r, &err) == CCORD_OK && !r.loaded);
  char acc[1000];
  snprintf(acc, sizeof acc, "%s/accounts", g_root);
  CT_CHECK(access(acc, F_OK) != 0);   // nothing was created
  ccord_warm_close(w);
  env_done(&a);
}

static void test_fingerprint_forget_prune_cap(void)
{
  char f1[33], f2[33], f3[33];
  ccord_warm_fingerprint("alpha", f1);
  ccord_warm_fingerprint("beta", f2);
  ccord_warm_fingerprint("alpha", f3);
  CT_CHECK(strlen(f1) == 32 && strcmp(f1, f2) != 0 && strcmp(f1, f3) == 0 && strstr(f1, "alpha") == nullptr);
  for (const char *p = f1; *p; p++) CT_CHECK((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'));

  fresh_root("fg");
  ccord_error err;
  ccord_warm *w = open_warm(g_root, "alpha", 0);
  CT_CHECK(w);
  const char *dir = ccord_warm_dir(w);
  CT_CHECK(ccord_warm_put_history(w, 4242, "[]", 2, &err) == CCORD_OK);
  char hp[1400];
  snprintf(hp, sizeof hp, "%s/warm/history/4242.json", dir);
  CT_CHECK(mode_of(hp) == 0600 && access(hp, F_OK) == 0);
  env a;
  CT_CHECK(env_init(&a, true));
  CT_CHECK(ccord_warm_save(w, a.store, false, &err) == CCORD_OK);
  // forget removes the directory and later saves are refused (no resurrection after sign-out).
  char dircopy[1300];
  snprintf(dircopy, sizeof dircopy, "%s", dir);
  ccord_warm_forget(w);
  CT_CHECK(access(dircopy, F_OK) != 0);
  (void)ccord_warm_save(w, a.store, false, &err);
  CT_CHECK(access(dircopy, F_OK) != 0);
  ccord_warm_close(w);

  // forget_user finds the directory by the user id in meta.json.
  w = open_warm(g_root, "alpha", 0);
  CT_CHECK(w && ccord_warm_save(w, a.store, false, &err) == CCORD_OK);
  snprintf(dircopy, sizeof dircopy, "%s", ccord_warm_dir(w));
  ccord_warm_forget_user(g_root, ccord_store_self_id(a.store));
  CT_CHECK(access(dircopy, F_OK) != 0);
  ccord_warm_close(w);

  // forget_token removes exactly the token's directory, never follows a symlink, ignores empty/NULL tokens.
  {
    ccord_warm *t1 = open_warm(g_root, "tok-one", 0), *t2 = open_warm(g_root, "tok-two", 0);
    CT_CHECK(t1 && t2 && ccord_warm_save(t1, a.store, false, &err) == CCORD_OK &&
             ccord_warm_save(t2, a.store, false, &err) == CCORD_OK);
    char d1[1300], d2[1300];
    snprintf(d1, sizeof d1, "%s", ccord_warm_dir(t1));
    snprintf(d2, sizeof d2, "%s", ccord_warm_dir(t2));
    CT_CHECK(!ccord_warm_forget_token(g_root, nullptr) && !ccord_warm_forget_token(g_root, "") &&
             !ccord_warm_forget_token(nullptr, "tok-one"));
    CT_CHECK(access(d1, F_OK) == 0 && access(d2, F_OK) == 0);
    CT_CHECK(ccord_warm_forget_token(g_root, "tok-one") && access(d1, F_OK) != 0 && access(d2, F_OK) == 0);
    CT_CHECK(!ccord_warm_forget_token(g_root, "tok-one"));   // already gone: no-op
    // tok-two's directory replaced by a symlink to a victim directory: the victim must survive.
    char victim[1300], vfile[1400];
    snprintf(victim, sizeof victim, "%s/victim", g_root);
    snprintf(vfile, sizeof vfile, "%s/keep", victim);
    CT_CHECK(ccord_mkdir_p(victim, 0700, &err) == CCORD_OK);
    FILE *vf = fopen(vfile, "w");
    CT_CHECK(vf != nullptr);
    fputs("x", vf);
    fclose(vf);
    ccord_warm_close(t1);
    ccord_warm_close(t2);
    CT_CHECK(ccord_rmtree(d2, &err) == CCORD_OK && symlink(victim, d2) == 0);
    CT_CHECK(!ccord_warm_forget_token(g_root, "tok-two") && access(vfile, F_OK) == 0);
    unlink(d2);
  }

  // Opening a 12th account keeps CCORD_WARM_MAX_ACCOUNT_DIRS directories.
  for (int i = 0; i < 12; i++) {
    char tok[32];
    snprintf(tok, sizeof tok, "acct-%d", i);
    ccord_warm *x = open_warm(g_root, tok, 0);
    CT_CHECK(x != nullptr);
    ccord_warm_close(x);
  }
  char acc[1000];
  snprintf(acc, sizeof acc, "%s/accounts", g_root);
  size_t count = 0;
  DIR *d = opendir(acc);
  CT_CHECK(d != nullptr);
  struct dirent *de;
  while (d && (de = readdir(d)) != nullptr) {
    if (de->d_name[0] != '.') count++;
  }
  if (d) closedir(d);
  CT_CHECK(count <= CCORD_WARM_MAX_ACCOUNT_DIRS);

  // A tiny cap still produces a file that fits (reduced snapshot) or a clean error, never an oversized file.
  fresh_root("cap");
  ccord_warm *t = open_warm(g_root, "alpha", 4096);
  CT_CHECK(t != nullptr);
  ccord_status st = ccord_warm_save(t, a.store, false, &err);
  char sp[1300];
  CT_CHECK(ccord_warm_state_path(t, sp, sizeof sp));
  struct stat sb;
  if (lstat(sp, &sb) == 0) CT_CHECK(st == CCORD_OK && (size_t)sb.st_size <= 4096u);
  else CT_CHECK(st != CCORD_OK);
  ccord_warm_close(t);
  env_done(&a);
}

static bool write_framed_state(const char *path, const char *body)
{
  size_t blen = strlen(body);
  unsigned char d[32];
  ccord_sha256(body, blen, d);
  char hex[65];
  static const char hx[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i++) {
    hex[i * 2] = hx[d[i] >> 4];
    hex[i * 2 + 1] = hx[d[i] & 15];
  }
  hex[64] = '\0';
  char head[160];
  int hn = snprintf(head, sizeof head, "CCWARM %u %s %zu\n", (unsigned)CCORD_WARM_FORMAT_VERSION, hex, blen);
  if (hn <= 0 || (size_t)hn >= sizeof head) return false;
  char *buf = ccord_malloc((size_t)hn + blen);
  if (!buf) return false;
  memcpy(buf, head, (size_t)hn);
  memcpy(buf + hn, body, blen);
  bool ok = write_raw(path, buf, (size_t)hn + blen, 0600);
  ccord_free(buf);
  return ok;
}

static void test_legacy_state_missing_user_fields(void)
{
  fresh_root("legacy");
  ccord_error err;
  ccord_warm *w = open_warm(g_root, "tok-legacy", 0);
  CT_CHECK(w != nullptr);

  char wdir[1300], sp[1300];
  snprintf(wdir, sizeof wdir, "%s/warm", ccord_warm_dir(w));
  CT_CHECK(ccord_mkdir_p(wdir, 0700, &err) == CCORD_OK);
  CT_CHECK(ccord_warm_state_path(w, sp, sizeof sp));

  char fp[33];
  ccord_warm_fingerprint("tok-legacy", fp);

  // Older format v1 state.json whose user object predates "has_accent", "clan_guild", "clan_badge" (and cs_expires),
  // and whose guild/channel/member/root omit newer optional bool/int64/array keys.
  char body[2048];
  int bn = snprintf(body, sizeof body,
                    "{\"v\":\"1\",\"app\":\"0.1.0\",\"saved_ms\":1700000000000,\"fp\":\"%s\","
                    "\"self_id\":\"1001\",\"sel_guild\":\"2001\",\"sel_channel\":\"3001\","
                    "\"users\":[{\"id\":\"1001\",\"name\":\"LegacyUser\",\"handle\":\"legacy\","
                    "\"avatar\":\"\",\"banner\":\"\",\"accent\":\"16711680\",\"clan\":\"OLD\"}],"
                    "\"guilds\":[{\"id\":\"2001\",\"name\":\"LegacyGuild\"}],"
                    "\"channels\":[{\"id\":\"3001\",\"guild\":\"2001\",\"kind\":\"0\",\"name\":\"general\"}],"
                    "\"members\":[{\"guild\":\"2001\",\"user\":\"1001\"}]}",
                    fp);
  CT_CHECK(bn > 0 && (size_t)bn < sizeof body);
  CT_CHECK(write_framed_state(sp, body));

  env b;
  CT_CHECK(env_init(&b, false));
  ccord_warm_load_result r = {0};
  CT_CHECK(ccord_warm_load(w, b.store, b.s, &r, &err) == CCORD_OK);
  CT_CHECK(r.loaded && r.self_id == 1001 && r.guilds == 1 && r.users == 1);

  const ccord_user *u = ccord_store_user(b.store, 1001);
  CT_CHECK(u != nullptr);
  if (u) {
    CT_CHECK(strcmp(u->name, "LegacyUser") == 0);
    CT_CHECK(u->accent_color == 0xFF0000u);
    CT_CHECK(!u->has_accent_color);
    CT_CHECK(strcmp(u->clan_tag, "OLD") == 0);
    CT_CHECK(u->clan_guild_id == CCORD_ID_NONE);
    CT_CHECK(u->clan_badge[0] == '\0');
    CT_CHECK(!u->is_bot);
    CT_CHECK(u->custom_status_expires_ms == 0);
  }

  env_done(&b);
  ccord_warm_close(w);
}

int main(void)
{
  test_roundtrip_and_modes();
  test_rejections();
  test_disabled();
  test_fingerprint_forget_prune_cap();
  test_legacy_state_missing_user_fields();
  ccord_error err;
  (void)ccord_rmtree(g_root, &err);
  return CT_RESULT("test_warm_cache");
}
