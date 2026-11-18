// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE   // mkdtemp, symlink, nftw under -std=c2x
#include <ccord/rpc/game_detect.h>

#include <ccord/core/alloc.h>

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ccord_test.h"

static char g_root[512];

static void make_root(void)
{
  const char *base = getenv("TMPDIR");
  snprintf(g_root, sizeof g_root, "%s/ccord-gd-XXXXXX", base && *base ? base : "/tmp");
  if (!mkdtemp(g_root)) { perror("mkdtemp"); exit(1); }
}

static int rm_cb(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
  (void)sb; (void)flag; (void)ftw;
  return remove(path);
}

static void remove_root(void)
{
  nftw(g_root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static void write_file(const char *dir, const char *name, const char *data, size_t n)
{
  char path[640];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); exit(1); }
  if (n) fwrite(data, 1, n, f);
  fclose(f);
}

static void make_proc_state(int pid, char state, const char *exe, const char *cmdline, const char *comm,
                            unsigned long long start)
{
  char dir[600], buf[1024];
  snprintf(dir, sizeof dir, "%s/%d", g_root, pid);
  if (mkdir(dir, 0755) != 0) { perror(dir); exit(1); }
  int n = snprintf(buf, sizeof buf, "%d (%s) %c 1 1 1 0 -1 4194304 0 0 0 0 0 0 0 0 20 0 1 0 %llu 0 0\n", pid, comm,
                   state, start);
  write_file(dir, "stat", buf, (size_t)n);
  n = snprintf(buf, sizeof buf, "%s\n", comm);
  write_file(dir, "comm", buf, (size_t)n);
  size_t cl = strlen(cmdline);
  memcpy(buf, cmdline, cl + 1);
  for (size_t i = 0; i < cl; i++) if (buf[i] == '|') buf[i] = '\0';
  write_file(dir, "cmdline", buf, cl ? cl + 1 : 0);
  if (exe) {
    char link[700];
    snprintf(link, sizeof link, "%s/exe", dir);
    if (symlink(exe, link) != 0) { perror(link); exit(1); }
  }
}

// Creates <root>/<pid>/{stat,comm,cmdline,exe}. cmdline uses '|' as the argument separator. exe may be nullptr.
static void make_proc(int pid, const char *exe, const char *cmdline, const char *comm, unsigned long long start)
{
  make_proc_state(pid, 'S', exe, cmdline, comm, start);
}

static const char k_list[] =
  "[{\"id\":\"1001\",\"name\":\"Native Game\",\"aliases\":[\"NG\"],\"hook\":true,\"executables\":["
  "{\"os\":\"linux\",\"name\":\"nativegame\",\"is_launcher\":false},"
  "{\"os\":\"darwin\",\"name\":\"NativeGame.app\",\"is_launcher\":false}]},"
  "{\"id\":\"1002\",\"name\":\"Foo\",\"executables\":["
  "{\"os\":\"win32\",\"name\":\"foo.exe\",\"is_launcher\":false},"
  "{\"os\":\"win32\",\"name\":\">Foo/Bar.exe\",\"is_launcher\":false}]},"
  "{\"id\":\"1003\",\"name\":\"Launcher Only\",\"executables\":["
  "{\"os\":\"linux\",\"name\":\"launcher\",\"is_launcher\":true},"
  "{\"os\":\"win32\",\"name\":\"launcher.exe\",\"is_launcher\":true}]},"
  "{\"id\":\"1004\",\"name\":\"Mac Only\",\"executables\":["
  "{\"os\":\"darwin\",\"name\":\"macgame\",\"is_launcher\":false}]},"
  "{\"id\":\"1005\",\"name\":\"Pathy\",\"executables\":["
  "{\"os\":\"linux\",\"name\":\"bin/pathy\",\"is_launcher\":false}]}]";

static ccord_game_list *load_list(const char *text)
{
  ccord_error err = {0};
  ccord_game_list *l = ccord_game_list_load(text, strlen(text), &err);
  if (!l) fprintf(stderr, "load failed: %s\n", err.message);
  return l;
}

static void test_native_match(void)
{
  make_root();
  make_proc(100, "/usr/bin/bash", "bash|-l", "bash", 50);
  make_proc(101, "/opt/games/NativeGame", "/opt/games/NativeGame|--fullscreen", "NativeGame", 60);
  ccord_game_list *l = load_list(k_list);
  CT_CHECK(l != nullptr);
  CT_CHECK(ccord_game_list_app_count(l) == 3);   // launcher-only and darwin-only apps have no usable executable
  ccord_game_match m;
  ccord_error err = {0};
  CT_CHECK(ccord_game_scan(l, g_root, &m, &err) == CCORD_OK);
  CT_CHECK(strcmp(m.app_id, "1001") == 0);
  CT_CHECK(strcmp(m.name, "Native Game") == 0);
  CT_CHECK(m.pid == 101);
  CT_CHECK(m.start_ticks == 60);
  ccord_game_list_free(l);
  remove_root();
}

static void test_cmdline_and_components(void)
{
  make_root();
  // exe unreadable (no link): argv[0] with a trailing "bin/pathy" component must match a "bin/pathy" entry.
  make_proc(200, nullptr, "/home/u/Pathy/bin/pathy|-x", "pathy", 10);
  ccord_game_list *l = load_list(k_list);
  ccord_game_match m;
  CT_CHECK(ccord_game_scan(l, g_root, &m, nullptr) == CCORD_OK);
  CT_CHECK(strcmp(m.app_id, "1005") == 0 && m.pid == 200);
  ccord_game_list_free(l);
  remove_root();
  make_root();
  make_proc(201, nullptr, "/home/u/Pathy/other/pathy|-x", "pathy2", 10);   // "pathy" alone is not "bin/pathy"
  l = load_list(k_list);
  CT_CHECK(ccord_game_scan(l, g_root, &m, nullptr) == CCORD_ERR_NOT_FOUND);
  ccord_game_list_free(l);
  remove_root();
}

static void test_wine(void)
{
  ccord_game_list *l = load_list(k_list);
  ccord_game_match m;
  make_root();
  make_proc(300, "/usr/bin/wine64-preloader", "Z:\\games\\Foo\\FOO.EXE|-windowed", "FOO.EXE", 5);
  CT_CHECK(ccord_game_scan(l, g_root, &m, nullptr) == CCORD_OK);
  CT_CHECK(strcmp(m.app_id, "1002") == 0 && m.pid == 300);
  remove_root();

  make_root();
  // Non-wine wrappers (python3 proton waitforexitandrun, reaper) must NOT match argv[1..].
  make_proc(301, "/usr/bin/python3", "python3|/steam/proton|waitforexitandrun|/home/u/.steam/steam/steamapps/common/Foo/Foo.exe", "python3", 5);
  CT_CHECK(ccord_game_scan(l, g_root, &m, nullptr) == CCORD_ERR_NOT_FOUND);
  remove_root();

  // Directory-qualified win32 name, and a single argument that also carries flags.
  make_root();
  make_proc(302, nullptr, "C:\\Games\\Foo\\bar.exe /nosplash", "bar.exe", 5);
  CT_CHECK(ccord_game_scan(l, g_root, &m, nullptr) == CCORD_OK);
  CT_CHECK(strcmp(m.app_id, "1002") == 0);
  remove_root();

  // A native process whose argv[0] ends in a listed win32 name also matches (Wine launchers often exec that way);
  // an unrelated .exe never matches.
  make_root();
  make_proc(303, "/home/u/foo.exe", "/home/u/foo.exe", "foo.exe", 5);   // native process literally named foo.exe
  make_proc(304, nullptr, "wine|C:\\other\\baz.exe", "baz.exe", 5);
  CT_CHECK(ccord_game_scan(l, g_root, &m, nullptr) == CCORD_OK && m.pid == 303);
  remove_root();
  ccord_game_list_free(l);
}

static void test_never_match(void)
{
  ccord_game_list *l = load_list(k_list);
  ccord_game_match m;
  make_root();
  make_proc(400, "/usr/bin/launcher", "/usr/bin/launcher", "launcher", 5);          // is_launcher linux
  make_proc(401, nullptr, "wine|C:\\x\\launcher.exe", "launcher.exe", 6);           // is_launcher win32
  make_proc(402, "/Applications/NativeGame.app", "/Applications/macgame", "macgame", 7);   // darwin only
  CT_CHECK(ccord_game_scan(l, g_root, &m, nullptr) == CCORD_ERR_NOT_FOUND);
  ccord_game_list_free(l);
  remove_root();
}

static void test_skips_and_recency(void)
{
  ccord_game_list *l = load_list(k_list);
  ccord_game_match m;
  ccord_error err = {0};
  make_root();
  char path[640];
  // A vanished pid (empty dir), a non-numeric entry, and a pid dir with mode 000 (unreadable unless run as root).
  snprintf(path, sizeof path, "%s/500", g_root);
  CT_CHECK(mkdir(path, 0755) == 0);
  snprintf(path, sizeof path, "%s/self", g_root);
  CT_CHECK(mkdir(path, 0755) == 0);
  make_proc(501, "/usr/bin/nativegame", "/usr/bin/nativegame", "nativegame", 1000);
  make_proc(502, "/usr/bin/nativegame", "/usr/bin/nativegame", "nativegame", 2000);   // newer
  make_proc(503, "/usr/bin/nativegame", "/usr/bin/nativegame", "nativegame", 1500);
  snprintf(path, sizeof path, "%s/503", g_root);
  CT_CHECK(chmod(path, 0) == 0);
  CT_CHECK(ccord_game_scan(l, g_root, &m, &err) == CCORD_OK);
  CT_CHECK(m.pid == 502 && m.start_ticks == 2000);
  CT_CHECK(chmod(path, 0755) == 0);
  ccord_game_list_free(l);
  remove_root();
  CT_CHECK(ccord_game_scan(nullptr, g_root, &m, &err) == CCORD_ERR_INVALID);
  l = load_list(k_list);
  CT_CHECK(ccord_game_scan(l, "/nonexistent/ccord-proc", &m, &err) == CCORD_ERR_IO);
  ccord_game_list_free(l);
}

static void test_malformed_list(void)
{
  static const char text[] =
    "[42,null,\"x\",{},{\"id\":\"abc\",\"name\":\"BadId\",\"executables\":[{\"os\":\"linux\",\"name\":\"badid\"}]},"
    "{\"id\":\"7\",\"name\":\"\",\"executables\":[{\"os\":\"linux\",\"name\":\"noname\"}]},"
    "{\"id\":\"8\",\"name\":\"NoExes\"},"
    "{\"id\":\"9\",\"name\":\"ExesNotArray\",\"executables\":{}},"
    "{\"id\":\"10\",\"name\":\"Mixed\",\"future\":{\"a\":[1,2]},\"executables\":[1,{\"os\":5,\"name\":\"x\"},"
    "{\"os\":\"linux\"},{\"os\":\"linux\",\"name\":7},{\"os\":\"freebsd\",\"name\":\"bsd\"},"
    "{\"os\":\"linux\",\"name\":\"mixedgame\",\"is_launcher\":null,\"extra\":true}]},"
    "{\"id\":11,\"name\":\"NumericId\",\"executables\":[{\"os\":\"linux\",\"name\":\"numid\"}]}]";
  ccord_game_list *l = load_list(text);
  CT_CHECK(l != nullptr);
  CT_CHECK(ccord_game_list_app_count(l) == 1);
  CT_CHECK(ccord_game_list_exe_count(l) == 1);
  make_root();
  make_proc(600, "/usr/bin/mixedgame", "mixedgame", "mixedgame", 5);
  ccord_game_match m;
  CT_CHECK(ccord_game_scan(l, g_root, &m, nullptr) == CCORD_OK && strcmp(m.app_id, "10") == 0);
  ccord_game_list_free(l);
  remove_root();

  ccord_error err = {0};
  CT_CHECK(ccord_game_list_load("{\"a\":1}", 7, &err) == nullptr && err.code == CCORD_ERR_INVALID);
  CT_CHECK(ccord_game_list_load("[{", 2, &err) == nullptr && err.code == CCORD_ERR_PARSE);
  l = ccord_game_list_load("[]", 2, &err);
  CT_CHECK(l && ccord_game_list_app_count(l) == 0);
  ccord_game_list_free(l);
  ccord_game_list_free(nullptr);
}

static double now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void test_large(void)
{
  // ~5 MB synthetic list: unknown fields, three executables per app, one of them a launcher.
  size_t cap = 6u * 1024u * 1024u, len = 0;
  char *buf = ccord_malloc(cap);
  CT_CHECK(buf != nullptr);
  if (!buf) return;
  buf[len++] = '[';
  unsigned apps = 0;
  while (len < 5u * 1024u * 1024u) {
    int n = snprintf(buf + len, cap - len,
      "%s{\"id\":\"%u\",\"name\":\"Synthetic Application Number %u\",\"aliases\":[\"alias %u\"],"
      "\"icon\":\"0123456789abcdef0123456789abcdef\",\"description\":\"Filler text to give each entry a realistic size.\","
      "\"executables\":[{\"os\":\"linux\",\"name\":\"synthgame%u\",\"is_launcher\":false},"
      "{\"os\":\"win32\",\"name\":\"SynthGame%u.exe\",\"is_launcher\":false},"
      "{\"os\":\"win32\",\"name\":\"synthlauncher%u.exe\",\"is_launcher\":true}]}",
      apps ? "," : "", 100000u + apps, apps, apps, apps, apps, apps);
    len += (size_t)n;
    apps++;
  }
  buf[len++] = ']';
  ccord_error err = {0};
  double t0 = now_ms();
  ccord_game_list *l = ccord_game_list_load(buf, len, &err);
  double load_ms = now_ms() - t0;
  if (!l) fprintf(stderr, "large load failed: %s\n", err.message);
  CT_CHECK(l != nullptr);
  ccord_free(buf);
  if (!l) return;
  CT_CHECK(ccord_game_list_app_count(l) == apps);
  CT_CHECK(ccord_game_list_exe_count(l) == apps * 2u);

  make_root();
  char exe[128], cmd[160], comm[32];
  for (int i = 0; i < 500; i++) {
    snprintf(exe, sizeof exe, "/usr/bin/proc%d", i);
    snprintf(cmd, sizeof cmd, "/usr/bin/proc%d|--flag", i);
    snprintf(comm, sizeof comm, "proc%d", i);
    make_proc(1000 + i, exe, cmd, comm, 100u + (unsigned)i);
  }
  make_proc(2000, "/usr/bin/x", "wine|Z:\\g\\synthgame77.exe", "x", 3);   // the one real game, oldest
  ccord_game_match m;
  double t1 = now_ms();
  ccord_status st = ccord_game_scan(l, g_root, &m, &err);
  double scan_ms = now_ms() - t1;
  CT_CHECK(st == CCORD_OK);
  CT_CHECK(strcmp(m.app_id, "100077") == 0 && m.pid == 2000);
  CT_CHECK(scan_ms < 100.0);
  fprintf(stdout, "game_detect: %u apps load %.1f ms, 501 procs scan %.2f ms\n", apps, load_ms, scan_ms);
  ccord_game_list_free(l);
  remove_root();
}

static void test_zombie_and_wrappers(void)
{
  static const char tipsy_list[] =
    "[{\"id\":\"2001\",\"name\":\"Tipsy\",\"executables\":["
    "{\"os\":\"linux\",\"name\":\"tipsy\",\"is_launcher\":false},"
    "{\"os\":\"win32\",\"name\":\"tipsy.exe\",\"is_launcher\":false}]}]";
  ccord_game_list *l = load_list(tipsy_list);
  CT_CHECK(l != nullptr);
  ccord_game_match m;

  // Zombie (State: Z, X, x) with short comm "tipsy" and no exe/cmdline must never match.
  make_root();
  make_proc_state(700, 'Z', nullptr, "", "tipsy", 5);
  make_proc_state(701, 'X', nullptr, "", "tipsy", 6);
  make_proc_state(702, 'x', nullptr, "", "tipsy", 7);
  CT_CHECK(ccord_game_scan(l, g_root, &m, nullptr) == CCORD_ERR_NOT_FOUND);
  remove_root();

  // Non-wine wrappers (reaper, python3 proton waitforexitandrun) holding tipsy.exe in argv[1..] must not match.
  make_root();
  make_proc(710, "/home/u/.steam/reaper", "reaper|SteamLaunch|AppId=1|--|/games/tipsy.exe", "reaper", 5);
  make_proc(711, "/usr/bin/python3", "python3|/steam/proton|waitforexitandrun|/games/tipsy.exe", "python3", 6);
  CT_CHECK(ccord_game_scan(l, g_root, &m, nullptr) == CCORD_ERR_NOT_FOUND);
  remove_root();

  // Real wine64 + tipsy.exe MUST match.
  make_root();
  make_proc(712, "/usr/bin/wine64", "wine64|C:\\Games\\tipsy.exe", "wine64", 8);
  CT_CHECK(ccord_game_scan(l, g_root, &m, nullptr) == CCORD_OK);
  CT_CHECK(strcmp(m.app_id, "2001") == 0 && m.pid == 712);
  remove_root();

  ccord_game_list_free(l);
}

int main(void)
{
  test_native_match();
  test_cmdline_and_components();
  test_wine();
  test_never_match();
  test_skips_and_recency();
  test_malformed_list();
  test_large();
  test_zombie_and_wrappers();
  return CT_RESULT("test_game_detect");
}
