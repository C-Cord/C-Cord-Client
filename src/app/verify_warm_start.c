// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice warm-start: see verify_warm_start.h and
#define _POSIX_C_SOURCE 200809L
#include "verify_warm_start.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <ccord/core/alloc.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/paths.h>
#include <ccord/core/sha256.h>
#include <ccord/net/mock_transport.h>
#include <ccord/store/store.h>

#include "verify_ui_parity.h"
#include "ui_internal.h"

static ccord_verify_warm_env g_env;

void ccord_verify_warm_start_configure(const ccord_verify_warm_env *env)
{
  g_env = *env;
}

// Virtual-time latency the children give the mock: nothing "from the network" arrives before the slice moves the clock.
#define SLICE_LATENCY_MS 60000u
#define SLICE_ADVANCE_MS 61000u

#define CHECK(cond, name)                                               \
  do {                                                                  \
    checks++;                                                           \
    if (!(cond)) {                                                      \
      printf("warm-start[%s]: FAIL %s\n", phase, name);                \
      return 1;                                                         \
    }                                                                   \
  } while (0)

// ------------------------------------------------------------------------------------------------ helpers

static bool g_loading_reappeared;

// Runs `total_ms` of virtual time in `step_ms` frames; notes whether the loading screen ever showed.
static void adv(const ccord_verify_ui_parity_ctx *c, uint32_t total_ms, uint32_t step_ms)
{
  for (uint32_t t = 0; t < total_ms; t += step_ms) {
    c->frame_ms(c->ud, step_ms);
    ccord_ui_debug d;
    ccord_ui_get_debug(c->ui, &d);
    if (d.loading_screen_open) g_loading_reappeared = true;
  }
}

static bool stat_mode(const char *path, mode_t *mode, off_t *size)
{
  struct stat st;
  if (lstat(path, &st) != 0) return false;
  if (mode) *mode = (mode_t)(st.st_mode & 07777);
  if (size) *size = st.st_size;
  return true;
}

static bool path_missing(const char *path)
{
  struct stat st;
  return lstat(path, &st) != 0 && errno == ENOENT;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
  const unsigned char *b = p;
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ULL;
  return h;
}

static uint64_t fnv_u64(uint64_t h, uint64_t v)
{
  return fnv(h, &v, sizeof v);
}

static uint64_t fnv_str(uint64_t h, const char *s)
{
  return fnv(h, s ? s : "", s ? strlen(s) : 0);
}

// Guild rail order (with folder fields), channel order per guild and the DM list.
static uint64_t order_hash(const ccord_store *s)
{
  uint64_t h = 1469598103934665603ULL;
  size_t ng = ccord_store_guild_count(s);
  for (size_t i = 0; i <= ng; i++) {
    ccord_id g = CCORD_DM_GUILD_ID;
    if (i < ng) {
      const ccord_guild *gu = ccord_store_guild_at(s, i);
      g = gu->id;
      h = fnv_u64(h, gu->id);
      h = fnv_u64(h, gu->folder_id);
      h = fnv_str(h, gu->folder_name);
      h = fnv_u64(h, gu->folder_color);
      h = fnv_str(h, gu->name);
    }
    for (size_t k = 0, n = ccord_store_channel_count(s, g); k < n; k++) {
      const ccord_channel *c = ccord_store_channel_at(s, g, k);
      h = fnv_u64(h, c->id);
      h = fnv_u64(h, c->parent_id);
      h = fnv_u64(h, (uint64_t)(int64_t)c->position);
      h = fnv_u64(h, c->kind);
      h = fnv_str(h, c->name);
    }
  }
  return h;
}

static size_t channel_total(const ccord_store *s)
{
  size_t t = ccord_store_channel_count(s, CCORD_DM_GUILD_ID);
  for (size_t i = 0, n = ccord_store_guild_count(s); i < n; i++) {
    t += ccord_store_channel_count(s, ccord_store_guild_at(s, i)->id);
  }
  return t;
}

static bool join(char *out, size_t cap, const char *a, const char *b)
{
  int n = snprintf(out, cap, "%s/%s", a, b);
  return n > 0 && (size_t)n < cap;
}

// 0 when every file and directory below `path` is private (no group/other bits, no symlinks, no temp leftovers).
static int walk_private(const char *path, size_t *seen)
{
  struct stat st;
  if (lstat(path, &st) != 0) return 1;
  (*seen)++;
  if (S_ISLNK(st.st_mode) || (st.st_mode & 077) != 0) return 1;
  if (strstr(path, ".tmp.") != nullptr) return 1;
  if (!S_ISDIR(st.st_mode)) return 0;
  DIR *d = opendir(path);
  if (!d) return 1;
  int bad = 0;
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char child[1500];
    if (!join(child, sizeof child, path, e->d_name)) {
      bad = 1;
      break;
    }
    bad |= walk_private(child, seen);
  }
  closedir(d);
  return bad;
}

static void iso_time(int64_t ms, char *out, size_t cap)
{
  time_t t = (time_t)(ms / 1000);
  struct tm tm;
  gmtime_r(&t, &tm);
  snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.000000+00:00", (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100,
           tm.tm_mday % 100, tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100);
}

#define FIXTURE_ATTACHMENT "warm-fixture.png"
#define FIXTURE_GHOST_TEXT "ghost: deleted while the app was closed"

// A raw Discord history body (newest first) built from the channel's live page: the oldest and the newest message are
// missing (they arrive with the live page), message `attach_idx` has an attachment, and `ghost_id` is a message the
// live page does not have (deleted while away). It is what a previous run would have fetched.
static bool build_fixture(const ccord_store *s, ccord_id ch, size_t attach_idx, bool drop_edges, ccord_id ghost_id,
                          ccord_buf *out)
{
  size_t n = ccord_store_message_count(s, ch);
  if (n < 4) return false;
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_array(&w);
  const ccord_message *newest = ccord_store_message_at(s, ch, n - 1);
  for (size_t pass = 0; pass <= n; pass++) {
    bool ghost = pass == 0;
    size_t i = n - pass;                       // pass 1 -> n-1 (newest) ... pass n -> 0 (oldest)
    const ccord_message *m = ghost ? nullptr : ccord_store_message_at(s, ch, i);
    if (ghost && ghost_id == CCORD_ID_NONE) continue;
    if (!ghost && drop_edges && (i == 0 || i == n - 1)) continue;
    uint64_t id = ghost ? ghost_id : m->id;
    ccord_id author = ghost ? newest->author_id : m->author_id;
    int64_t ts = ghost ? newest->timestamp_ms + 1000 : m->timestamp_ms;
    const char *content = ghost ? FIXTURE_GHOST_TEXT : (m->content ? m->content : "");
    const ccord_user *u = ccord_store_user(s, author);
    char iso[48];
    iso_time(ts, iso, sizeof iso);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id");
    ccord_jw_id(&w, id);
    ccord_jw_key(&w, "channel_id");
    ccord_jw_id(&w, ch);
    ccord_jw_key(&w, "author");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id");
    ccord_jw_id(&w, author);
    ccord_jw_key(&w, "username");
    ccord_jw_cstr(&w, (u && u->handle && u->handle[0]) ? u->handle : "unknown");
    ccord_jw_key(&w, "global_name");
    ccord_jw_cstr(&w, (u && u->name && u->name[0]) ? u->name : "Unknown");
    ccord_jw_key(&w, "discriminator");
    ccord_jw_cstr(&w, "0");
    ccord_jw_end_object(&w);
    ccord_jw_key(&w, "content");
    ccord_jw_cstr(&w, content);
    ccord_jw_key(&w, "timestamp");
    ccord_jw_cstr(&w, iso);
    ccord_jw_key(&w, "edited_timestamp");
    ccord_jw_null(&w);
    ccord_jw_key(&w, "tts");
    ccord_jw_bool(&w, false);
    ccord_jw_key(&w, "mention_everyone");
    ccord_jw_bool(&w, false);
    ccord_jw_key(&w, "mentions");
    ccord_jw_begin_array(&w);
    ccord_jw_end_array(&w);
    ccord_jw_key(&w, "mention_roles");
    ccord_jw_begin_array(&w);
    ccord_jw_end_array(&w);
    ccord_jw_key(&w, "attachments");
    ccord_jw_begin_array(&w);
    if (!ghost && i == attach_idx) {
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "id");
      ccord_jw_id(&w, 9000001u);
      ccord_jw_key(&w, "filename");
      ccord_jw_cstr(&w, FIXTURE_ATTACHMENT);
      ccord_jw_key(&w, "size");
      ccord_jw_int(&w, 1234);
      ccord_jw_key(&w, "url");
      ccord_jw_cstr(&w, "https://cdn.discordapp.com/attachments/1/2/" FIXTURE_ATTACHMENT);
      ccord_jw_key(&w, "proxy_url");
      ccord_jw_cstr(&w, "https://media.discordapp.net/attachments/1/2/" FIXTURE_ATTACHMENT);
      ccord_jw_key(&w, "content_type");
      ccord_jw_cstr(&w, "image/png");
      ccord_jw_key(&w, "width");
      ccord_jw_int(&w, 64);
      ccord_jw_key(&w, "height");
      ccord_jw_int(&w, 64);
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_array(&w);
    ccord_jw_key(&w, "embeds");
    ccord_jw_begin_array(&w);
    ccord_jw_end_array(&w);
    ccord_jw_key(&w, "pinned");
    ccord_jw_bool(&w, false);
    ccord_jw_key(&w, "type");
    ccord_jw_int(&w, 0);
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  return ccord_jw_ok(&w);
}

static bool has_attachment(const ccord_store *s, ccord_id ch)
{
  for (size_t i = 0, n = ccord_store_message_count(s, ch); i < n; i++) {
    const ccord_message *m = ccord_store_message_at(s, ch, i);
    if (m && m->rich && m->rich->attachment_count > 0 && m->rich->attachments[0].filename &&
        strcmp(m->rich->attachments[0].filename, FIXTURE_ATTACHMENT) == 0) {
      return true;
    }
  }
  return false;
}

static bool ids_ascending(const ccord_store *s, ccord_id ch)
{
  for (size_t i = 1, n = ccord_store_message_count(s, ch); i < n; i++) {
    if (ccord_store_message_at(s, ch, i - 1)->id >= ccord_store_message_at(s, ch, i)->id) return false;
  }
  return true;
}

// ---- the values the cold phase leaves for the warm phase (slice-expect.txt, private, outside accounts/)

typedef struct expect {
  uint64_t guilds, channels, users, hash, sel_guild, sel_channel, ch2, fixture_n, ch2_n, lo, hi, n, saved_ms;
} expect;

static bool expect_path(char *out, size_t cap)
{
  return join(out, cap, g_env.data_dir, "slice-expect.txt");
}

static bool expect_write(const expect *e)
{
  char text[1024];
  int n = snprintf(text, sizeof text,
                   "guilds=%" PRIu64 "\nchannels=%" PRIu64 "\nusers=%" PRIu64 "\nhash=%" PRIu64 "\nsel_guild=%" PRIu64
                   "\nsel_channel=%" PRIu64 "\nch2=%" PRIu64 "\nfixture_n=%" PRIu64 "\nch2_n=%" PRIu64 "\nlo=%" PRIu64
                   "\nhi=%" PRIu64 "\nn=%" PRIu64 "\nsaved_ms=%" PRIu64 "\n",
                   e->guilds, e->channels, e->users, e->hash, e->sel_guild, e->sel_channel, e->ch2, e->fixture_n,
                   e->ch2_n, e->lo, e->hi, e->n, e->saved_ms);
  char path[1100];
  ccord_error err;
  return n > 0 && (size_t)n < sizeof text && expect_path(path, sizeof path) &&
         ccord_file_write_atomic(path, text, (size_t)n, 0600, false, &err) == CCORD_OK;
}

static bool expect_get(const char *text, const char *key, uint64_t *out)
{
  char needle[40];
  snprintf(needle, sizeof needle, "%s=", key);
  const char *p = text;
  while ((p = strstr(p, needle)) != nullptr) {
    if (p == text || p[-1] == '\n') {
      *out = strtoull(p + strlen(needle), nullptr, 10);
      return true;
    }
    p += strlen(needle);
  }
  return false;
}

static bool expect_read(expect *e)
{
  char path[1100];
  ccord_error err;
  size_t n = 0;
  if (!expect_path(path, sizeof path)) return false;
  char *text = ccord_file_read_max(path, 4096, &n, &err);
  if (!text) return false;
  bool ok = expect_get(text, "guilds", &e->guilds) && expect_get(text, "channels", &e->channels) &&
            expect_get(text, "users", &e->users) && expect_get(text, "hash", &e->hash) &&
            expect_get(text, "sel_guild", &e->sel_guild) && expect_get(text, "sel_channel", &e->sel_channel) &&
            expect_get(text, "ch2", &e->ch2) && expect_get(text, "fixture_n", &e->fixture_n) &&
            expect_get(text, "ch2_n", &e->ch2_n) && expect_get(text, "lo", &e->lo) && expect_get(text, "hi", &e->hi) &&
            expect_get(text, "n", &e->n) && expect_get(text, "saved_ms", &e->saved_ms);
  ccord_free(text);
  return ok;
}

static bool contains(const char *hay, size_t n, const char *needle)
{
  size_t m = strlen(needle);
  if (m == 0 || n < m) return false;
  for (size_t i = 0; i + m <= n; i++) {
    if (memcmp(hay + i, needle, m) == 0) return true;
  }
  return false;
}

static ccord_id other_text_channel(const ccord_store *s, ccord_id guild, ccord_id not_this)
{
  for (size_t i = 0, n = ccord_store_channel_count(s, guild); i < n; i++) {
    const ccord_channel *c = ccord_store_channel_at(s, guild, i);
    if (c && c->id != not_this && c->kind == CCORD_CHANNEL_TEXT && c->discord_type == 0) return c->id;
  }
  return CCORD_ID_NONE;
}

// ------------------------------------------------------------------------------------------------ child phases

static int phase_cold(const ccord_verify_ui_parity_ctx *c, int *out_checks)
{
  const char *phase = "cold";
  int checks = 0;
  ccord_warm *w = g_env.warm;
  ccord_error err;
  CHECK(!g_env.load.loaded, "cold: a start with no cache reports loaded == false");
  c->frame_ms(c->ud, 1);
  ccord_ui_debug d;
  ccord_ui_get_debug(c->ui, &d);
  CHECK(d.loading_screen_open, "cold: frame 0 shows the loading screen");
  CHECK(!ccord_session_is_stale(c->session) && !d.stale_bar_open, "cold: a cold start is not stale");
  adv(c, SLICE_ADVANCE_MS, 1000);   // the mock snapshot is due after SLICE_LATENCY_MS of virtual time
  ccord_ui_get_debug(c->ui, &d);
  CHECK(!d.loading_screen_open && !ccord_session_is_stale(c->session), "cold: after the snapshot the loading screen is gone");
  ccord_id g1 = ccord_store_selected_guild(c->store), ch1 = ccord_store_selected_channel(c->store);
  adv(c, SLICE_ADVANCE_MS, 1000);   // the live history page of the open channel
  CHECK(ccord_store_history_loaded(c->store, ch1) && ccord_store_message_count(c->store, ch1) >= 10,
        "cold: the open channel has its live page");
  ccord_id ch2 = other_text_channel(c->store, g1, ch1);
  CHECK(ch2 != CCORD_ID_NONE && ccord_session_select_channel(c->session, ch2, &err) == CCORD_OK, "cold: open a second channel");
  adv(c, SLICE_ADVANCE_MS, 1000);
  CHECK(ccord_store_message_count(c->store, ch2) >= 4, "cold: the second channel has a live page");

  // Fixture pages, the way a previous run would have fetched them.
  size_t n1 = ccord_store_message_count(c->store, ch1), n2 = ccord_store_message_count(c->store, ch2);
  ccord_id lo = ccord_store_message_at(c->store, ch1, 0)->id, hi = ccord_store_message_at(c->store, ch1, n1 - 1)->id;
  ccord_buf b1, b2;
  ccord_buf_init(&b1, 0);
  ccord_buf_init(&b2, 0);
  bool built = build_fixture(c->store, ch1, n1 / 2, true, hi + 1, &b1) && build_fixture(c->store, ch2, SIZE_MAX, true, CCORD_ID_NONE, &b2);
  CHECK(built, "cold: build the fixture pages");
  ccord_status s1 = ccord_warm_put_history(w, ch1, b1.data, b1.len, &err);
  ccord_status s2 = ccord_warm_put_history(w, ch2, b2.data, b2.len, &err);
  ccord_buf_free(&b1);
  ccord_buf_free(&b2);
  char hp[1400], dir_hist[1300];
  const char *wd = ccord_warm_dir(w);
  CHECK(wd != nullptr, "cold: the account directory exists");
  snprintf(hp, sizeof hp, "%s/warm/history/%" PRIu64 ".json", wd, (uint64_t)ch1);
  snprintf(dir_hist, sizeof dir_hist, "%s/warm/history", wd);
  mode_t mode = 0;
  CHECK(s1 == CCORD_OK && s2 == CCORD_OK && stat_mode(hp, &mode, nullptr) && mode == 0600, "cold: fixture history file is 0600");

  // Back to the first channel, then save what the user would see next time.
  CHECK(ccord_session_select_channel(c->session, ch1, &err) == CCORD_OK, "cold: reselect the first channel");
  adv(c, 100, 50);
  ccord_id sel_g = ccord_store_selected_guild(c->store), sel_c = ccord_store_selected_channel(c->store);
  CHECK(sel_c == ch1 && sel_g == g1, "cold: selection is back on the first channel");
  CHECK(ccord_warm_save(w, c->store, true, &err) == CCORD_OK, "cold: ccord_warm_save succeeds");
  char sp[1300];
  CHECK(ccord_warm_state_path(w, sp, sizeof sp), "cold: state path");
  off_t size = 0;
  CHECK(stat_mode(sp, &mode, &size) && mode == 0600, "cold: state.json is mode 0600");
  size_t acc_seen = 0;
  char acc[1200];
  join(acc, sizeof acc, g_env.data_dir, "accounts");
  struct stat st;
  bool dirs_ok = lstat(acc, &st) == 0 && (st.st_mode & 07777) == 0700 && lstat(wd, &st) == 0 && (st.st_mode & 07777) == 0700;
  char warm_dir[1300];
  snprintf(warm_dir, sizeof warm_dir, "%s/warm", wd);
  dirs_ok = dirs_ok && lstat(warm_dir, &st) == 0 && (st.st_mode & 07777) == 0700 && lstat(dir_hist, &st) == 0 &&
            (st.st_mode & 07777) == 0700;
  CHECK(dirs_ok, "cold: accounts/, the account dir, warm/ and warm/history/ are mode 0700");
  int64_t saved_ms = 0;
  size_t body_len = 0;
  CHECK(ccord_warm_verify_file(sp, &saved_ms, &body_len, 0) && (size_t)size <= CCORD_WARM_MAX_FILE_BYTES + 160u && body_len > 0,
        "cold: header CCWARM 1, sha256 and length verify; size within the cap");
  {
    size_t n = 0;
    char *text = ccord_file_read_max(sp, CCORD_WARM_MAX_FILE_BYTES + 160u, &n, &err);
    CHECK(text && strncmp(text, "CCWARM 1 ", 9) == 0, "cold: the header names format 1");
    bool leak = text && (contains(text, n, "token") || contains(text, n, "\"email\"") || contains(text, n, "\"phone\"") ||
                         contains(text, n, "session_id") || contains(text, n, "resume_gateway"));
    ccord_free(text);
    CHECK(!leak, "cold: state holds no token, email, phone or session_id");
  }

  // maybe_save: never while stale, never when nothing changed.
  uint64_t now = g_env.now_ms ? g_env.now_ms(g_env.ud) : 0;
  ccord_session_set_stale(c->session, true);
  CHECK(!ccord_warm_maybe_save(w, c->store, c->session, now + 1000000u), "cold: maybe_save refuses while stale");
  ccord_session_set_stale(c->session, false);
  bool first = ccord_warm_maybe_save(w, c->store, c->session, now + 2000000u);
  bool second = ccord_warm_maybe_save(w, c->store, c->session, now + 4000000u);
  CHECK(!first && !second, "cold: maybe_save refuses when the store did not change since the save");
  size_t seen = 0;
  CHECK(walk_private(acc, &seen) == 0 && seen >= 6, "cold: no file or directory under accounts/ has group/other bits");
  (void)acc_seen;

  expect e = {.guilds = ccord_store_guild_count(c->store), .channels = channel_total(c->store),
              .users = ccord_store_user_count(c->store), .hash = order_hash(c->store), .sel_guild = sel_g,
              .sel_channel = sel_c, .ch2 = ch2, .fixture_n = n1 - 2 + 1, .ch2_n = n2 - 2, .lo = lo, .hi = hi, .n = n1,
              .saved_ms = (uint64_t)saved_ms};
  CHECK(expect_write(&e), "cold: leave the expected values for the warm phase");
  *out_checks = checks;
  return 0;
}

static int phase_warm(const ccord_verify_ui_parity_ctx *c, int *out_checks)
{
  const char *phase = "warm";
  int checks = 0;
  ccord_warm *w = g_env.warm;
  ccord_error err;
  expect e = {0};
  CHECK(expect_read(&e), "warm: read the cold phase's values");
  CHECK(g_env.load.loaded && g_env.load.load_ms <= 2000u, "warm: the cache loaded in at most 2000 ms");
  CHECK(g_env.load.guilds == e.guilds && g_env.load.channels == e.channels && g_env.load.users == e.users,
        "warm: guild, channel and user counts equal the cold phase's");

  // Frame 0, before any network answer.
  c->frame_ms(c->ud, 1);
  ccord_ui_debug d;
  ccord_ui_get_debug(c->ui, &d);
  CHECK(!d.loading_screen_open, "warm: frame 0 draws the UI, not the loading screen");
  CHECK(order_hash(c->store) == e.hash, "warm: guild rail, folder fields, channel order and DM list equal the cold phase's");
  ccord_id ch1 = e.sel_channel, ch2 = e.ch2;
  CHECK(ccord_store_selected_guild(c->store) == e.sel_guild && ccord_store_selected_channel(c->store) == ch1,
        "warm: the saved guild and channel are selected");
  CHECK(ccord_store_history_loaded(c->store, ch1) && ccord_store_history_stale(c->store, ch1) &&
            ccord_store_message_count(c->store, ch1) == e.fixture_n && has_attachment(c->store, ch1),
        "warm: the open channel shows the cached page, marked stale, attachment included");
  CHECK(ccord_store_history_loaded(c->store, ch2) && ccord_store_history_stale(c->store, ch2) &&
            ccord_store_message_count(c->store, ch2) == e.ch2_n,
        "warm: a second channel is seeded too");
  CHECK(d.stale_bar_open && ccord_session_is_stale(c->session) &&
            ccord_session_state(c->session) == CCORD_CONN_CONNECTING,
        "warm: the stale bar shows while the session is still connecting");
  CHECK(d.message_count == e.fixture_n, "warm: the message list drew the cached page");

  // While stale: reads and navigation, no sending, no requests.
  const ccord_message *newest = ccord_store_message_at(c->store, ch1, e.fixture_n - 1);
  ccord_id newest_id = newest ? newest->id : CCORD_ID_NONE;
  size_t before = ccord_store_message_count(c->store, ch1);
  ccord_input_event typed = {.type = CCORD_IN_TEXT};
  snprintf(typed.text, sizeof typed.text, "warm draft");
  c->input(c->ud, &typed);
  CHECK(strcmp(ccord_ui_composer_text(c->ui), "warm draft") == 0, "warm: type a draft while stale");
  ccord_error se, re, ee, de;
  bool send_refused = ccord_session_send_message(c->session, ch1, "hello", 5, &se) != CCORD_OK && strstr(se.message, "Reconnecting");
  bool react_refused = ccord_session_toggle_reaction(c->session, ch1, newest_id, "\xf0\x9f\x91\x8d", 4, &re) != CCORD_OK && strstr(re.message, "Reconnecting");
  bool edit_refused = ccord_session_edit_message(c->session, ch1, newest_id, "x", 1, &ee) != CCORD_OK && strstr(ee.message, "Reconnecting");
  bool delete_refused = ccord_session_delete_message(c->session, ch1, newest_id, &de) != CCORD_OK && strstr(de.message, "Reconnecting");
  CHECK(send_refused && react_refused && edit_refused && delete_refused, "warm: send, react, edit and delete are refused with \"Reconnecting\"");
  CHECK(ccord_store_message_count(c->store, ch1) == before && strcmp(ccord_ui_composer_text(c->ui), "warm draft") == 0,
        "warm: no echo was created and the composer text is kept");
  CHECK(ccord_mock_call_count(c->transport, CCORD_MOCK_CALL_HISTORY) == 0 &&
            ccord_mock_call_count(c->transport, CCORD_MOCK_CALL_PROFILE) == 0 &&
            ccord_mock_call_count(c->transport, CCORD_MOCK_CALL_PINS) == 0,
        "warm: no history, profile or pin request reached the transport while stale");
  {
    ccord_error ge;
    ccord_id sg = ccord_store_selected_guild(c->store);
    (void)ccord_session_search_messages(c->session, sg, ch1, "warm", 4, &ge);
    (void)ccord_session_request_discovery(c->session, "warm", 0, 0, 5000, true, &ge);
    (void)ccord_session_request_reaction_users(c->session, ch1, ccord_store_message_at(c->store, ch1, 0)->id, "x", 10, &ge);
    (void)ccord_session_open_thread(c->session, sg, ch1, 123456789u, "t", &ge);
    (void)ccord_session_ensure_slash_commands(c->session, sg, ch1, 5000);
    (void)ccord_session_request_expression_data(c->session, CCORD_EXPR_GIF_TRENDING, "", false, &ge);
    ccord_voice_config vc = {.ptt = true, .vad_sensitivity_pct = 33};
    (void)ccord_session_set_voice_config(c->session, &vc, &ge);
    (void)ccord_session_set_rich_presence(c->session, "[{\"name\":\"WarmGame\",\"type\":0}]", &ge);
    ccord_voice_config vseen;
    CHECK(ccord_mock_call_count(c->transport, CCORD_MOCK_CALL_SEARCH) == 0 &&
              ccord_mock_call_count(c->transport, CCORD_MOCK_CALL_DISCOVERY) == 0 &&
              ccord_mock_call_count(c->transport, CCORD_MOCK_CALL_REACTION_USERS) == 0 &&
              ccord_mock_call_count(c->transport, CCORD_MOCK_CALL_SLASH_INDEX) == 0 &&
              ccord_store_channel(c->store, 123456789u) == nullptr &&
              ccord_store_expr_blob_state(c->store, CCORD_EXPR_GIF_TRENDING, "") == CCORD_EXPR_BLOB_NONE &&
              !ccord_mock_voice_config(c->transport, &vseen) && ccord_mock_rich_activities(c->transport, nullptr) == nullptr,
          "warm: search, discovery, reaction users, thread, slash index, expressions, voice config and rich presence sent nothing before the snapshot");
  }

  // A draft and a scroll position to survive READY.
  ccord_input_event wheel = {.type = CCORD_IN_WHEEL, .x = 640.0f, .y = 300.0f, .wheel = 6.0f};
  c->input(c->ud, &wheel);
  adv(c, 800, 16);
  ccord_ui_get_debug(c->ui, &d);
  const ccord_message *fv = d.first_visible < ccord_store_message_count(c->store, ch1) ? ccord_store_message_at(c->store, ch1, d.first_visible) : nullptr;
  ccord_id first_id = fv ? fv->id : CCORD_ID_NONE;
  CHECK(!d.stick_bottom && first_id != CCORD_ID_NONE && first_id != newest_id, "warm: scrolled up one page; record the first visible message");
  CHECK(strcmp(ccord_ui_composer_text(c->ui), "warm draft") == 0, "warm: the draft is intact after scrolling");

  // The network answers: advance the virtual clock past the latency, frame by frame.
  g_loading_reappeared = false;
  for (int i = 0; i < 80 && ccord_session_is_stale(c->session); i++) adv(c, 1000, 1000);
  ccord_ui_get_debug(c->ui, &d);
  CHECK(!ccord_session_is_stale(c->session) && !d.stale_bar_open, "warm: READY applied: not stale, stale bar gone");
  CHECK(!g_loading_reappeared && !d.loading_screen_open, "warm: the loading screen never appeared");
  {
    ccord_voice_config vseen;
    const char *rj = ccord_mock_rich_activities(c->transport, nullptr);
    CHECK(rj && strstr(rj, "WarmGame") && ccord_mock_voice_config(c->transport, &vseen) && vseen.vad_sensitivity_pct == 33,
          "warm: the rich presence and voice config held back while stale were applied after READY");
  }
  CHECK(ccord_store_selected_guild(c->store) == e.sel_guild && ccord_store_selected_channel(c->store) == ch1,
        "warm: selected guild and channel unchanged after READY");
  CHECK(strcmp(ccord_ui_composer_text(c->ui), "warm draft") == 0, "warm: the draft is unchanged after READY");
  {
    const ccord_message *m = d.first_visible < ccord_store_message_count(c->store, ch1) ? ccord_store_message_at(c->store, ch1, d.first_visible) : nullptr;
    CHECK(m && m->id == first_id, "warm: the first visible message is the same after READY");
  }

  // The live history page reconciles with the cached one.
  for (int i = 0; i < 80 && ccord_store_history_stale(c->store, ch1); i++) adv(c, 1000, 1000);
  ccord_ui_get_debug(c->ui, &d);
  size_t n = ccord_store_message_count(c->store, ch1);
  CHECK(!ccord_store_history_stale(c->store, ch1) && ccord_store_find_message(c->store, ch1, e.hi + 1) == nullptr,
        "warm: the live page replaced the cache: the ghost message is gone, stale cleared");
  CHECK(n == e.n && ccord_store_message_at(c->store, ch1, 0)->id == e.lo && ccord_store_message_at(c->store, ch1, n - 1)->id == e.hi &&
            ccord_store_find_message(c->store, ch1, e.hi) != nullptr && ids_ascending(c->store, ch1),
        "warm: the live-only newest and oldest messages are present, ids contiguous, no hole");
  {
    const ccord_message *m = d.first_visible < n ? ccord_store_message_at(c->store, ch1, d.first_visible) : nullptr;
    CHECK(m && m->id == first_id && !g_loading_reappeared, "warm: the scroll anchor survived the page swap (id-based)");
    CHECK(strcmp(ccord_ui_composer_text(c->ui), "warm draft") == 0, "warm: the draft survived the page swap");
  }

  // Opening the other seeded channel revalidates it.
  size_t calls = ccord_mock_call_count(c->transport, CCORD_MOCK_CALL_HISTORY);
  CHECK(ccord_store_history_stale(c->store, ch2), "warm: the second seeded channel is still stale");
  CHECK(ccord_session_select_channel(c->session, ch2, &err) == CCORD_OK, "warm: open the second seeded channel");
  adv(c, 100, 50);
  CHECK(ccord_mock_call_count(c->transport, CCORD_MOCK_CALL_HISTORY) > calls, "warm: opening it issued a history request");

  // After the reconnect the cache is rewritten.
  uint64_t now = g_env.now_ms ? g_env.now_ms(g_env.ud) : 0;
  bool early = ccord_warm_maybe_save(w, c->store, c->session, now);
  adv(c, 7000, 1000);
  now = g_env.now_ms ? g_env.now_ms(g_env.ud) : 0;
  bool wrote = ccord_warm_maybe_save(w, c->store, c->session, now);
  char sp[1300];
  int64_t saved_ms = 0;
  CHECK(!early && wrote && ccord_warm_state_path(w, sp, sizeof sp) && ccord_warm_verify_file(sp, &saved_ms, nullptr, 0) &&
            (uint64_t)saved_ms > e.saved_ms,
        "warm: maybe_save after reconnect rewrote the file (sha verifies, saved_ms advanced)");
  (void)err;
  *out_checks = checks;
  return 0;
}

// A start whose cache was corrupted by the parent: truncated, flipped byte, other version, 0644. Cold, no crash.
static int phase_corrupt(const ccord_verify_ui_parity_ctx *c, int *out_checks)
{
  const char *phase = "corrupt";
  int checks = 0;
  char sp[1300];
  CHECK(ccord_warm_state_path(g_env.warm, sp, sizeof sp), "corrupt: state path");
  CHECK(!g_env.load.loaded, "corrupt: the damaged cache is ignored (loaded == false)");
  CHECK(path_missing(sp), "corrupt: the damaged file was removed");
  c->frame_ms(c->ud, 1);
  ccord_ui_debug d;
  ccord_ui_get_debug(c->ui, &d);
  CHECK(d.loading_screen_open && !d.stale_bar_open, "corrupt: the connecting screen shows, as on a cold start");
  adv(c, SLICE_ADVANCE_MS, 1000);
  ccord_ui_get_debug(c->ui, &d);
  CHECK(!d.loading_screen_open && !ccord_session_is_stale(c->session) && ccord_store_guild_count(c->store) > 0,
        "corrupt: the app then starts normally from the network");
  *out_checks = checks;
  return 0;
}

static int phase_other_account(const ccord_verify_ui_parity_ctx *c, int *out_checks)
{
  const char *phase = "other-account";
  int checks = 0;
  char fp_mock[33], fp_other[33];
  ccord_warm_fingerprint("mock", fp_mock);
  ccord_warm_fingerprint("mock-other", fp_other);
  const char *wd = ccord_warm_dir(g_env.warm);
  CHECK(wd && strstr(wd, fp_other) != nullptr && strstr(wd, fp_mock) == nullptr, "other-account: a different token uses a different directory");
  CHECK(!g_env.load.loaded, "other-account: the first account's cache is not loaded");
  c->frame_ms(c->ud, 1);
  ccord_ui_debug d;
  ccord_ui_get_debug(c->ui, &d);
  CHECK(d.loading_screen_open, "other-account: the connecting screen shows");
  adv(c, SLICE_ADVANCE_MS, 1000);
  CHECK(!ccord_session_is_stale(c->session) && ccord_store_guild_count(c->store) > 0, "other-account: starts from the network");
  *out_checks = checks;
  return 0;
}

// --no-warm-cache: nothing is read (a valid cache may be sitting there) and nothing is written.
static int phase_off(const ccord_verify_ui_parity_ctx *c, int *out_checks)
{
  const char *phase = "off";
  int checks = 0;
  ccord_error err;
  CHECK(!ccord_warm_enabled(g_env.warm) && ccord_warm_dir(g_env.warm) == nullptr, "off: the cache is disabled");
  CHECK(!g_env.load.loaded, "off: nothing was loaded");
  c->frame_ms(c->ud, 1);
  ccord_ui_debug d;
  ccord_ui_get_debug(c->ui, &d);
  CHECK(d.loading_screen_open && !d.stale_bar_open, "off: a cache on disk is ignored: the connecting screen shows");
  adv(c, SLICE_ADVANCE_MS, 1000);
  CHECK(!ccord_session_is_stale(c->session) && ccord_store_guild_count(c->store) > 0, "off: starts from the network");
  uint64_t now = g_env.now_ms ? g_env.now_ms(g_env.ud) : 0;
  CHECK(ccord_warm_save(g_env.warm, c->store, true, &err) == CCORD_OK, "off: save is a no-op");
  CHECK(!ccord_warm_maybe_save(g_env.warm, c->store, c->session, now + 100000u), "off: maybe_save writes nothing");
  *out_checks = checks;
  return 0;
}

// Sign-out through main.c's real handler: the account's directory goes, and nothing later in the process brings it back.
static int phase_signout(const ccord_verify_ui_parity_ctx *c, int *out_checks)
{
  const char *phase = "signout";
  int checks = 0;
  ccord_error err;
  char wd[1200] = "";
  const char *d0 = ccord_warm_dir(g_env.warm);
  if (d0) snprintf(wd, sizeof wd, "%s", d0);
  CHECK(g_env.load.loaded && wd[0] && !path_missing(wd) && g_env.sign_out, "signout: a warm cache is on disk");
  c->frame_ms(c->ud, 1);
  g_env.sign_out(g_env.ud);
  CHECK(path_missing(wd), "signout: the sign-out handler removed the account's cache directory");
  adv(c, SLICE_ADVANCE_MS, 1000);
  uint64_t now = g_env.now_ms ? g_env.now_ms(g_env.ud) : 0;
  CHECK(!ccord_warm_maybe_save(g_env.warm, c->store, c->session, now + 100000u) && path_missing(wd),
        "signout: a later periodic save does not recreate it");
  CHECK(ccord_warm_save(g_env.warm, c->store, true, &err) == CCORD_OK && path_missing(wd),
        "signout: a later synced save (as at exit) does not recreate it");
  *out_checks = checks;
  return 0;
}

// Account removal through main.c's real handler: that account's directory goes, another account's stays as it was.
static int phase_remove(const ccord_verify_ui_parity_ctx *c, int *out_checks)
{
  const char *phase = "remove";
  int checks = 0;
  ccord_error err;
  char wd[1200] = "";
  const char *d0 = ccord_warm_dir(g_env.warm);
  if (d0) snprintf(wd, sizeof wd, "%s", d0);
  CHECK(g_env.load.loaded && wd[0] && !path_missing(wd) && g_env.remove_account, "remove: a warm cache is on disk");
  c->frame_ms(c->ud, 1);
  ccord_id self = ccord_store_self_id(c->store);
  CHECK(self != CCORD_ID_NONE, "remove: the account has a user id");
  g_env.remove_account(g_env.ud, self);
  CHECK(path_missing(wd), "remove: the removal handler deleted the account's cache directory");
  adv(c, SLICE_ADVANCE_MS, 1000);
  uint64_t now = g_env.now_ms ? g_env.now_ms(g_env.ud) : 0;
  CHECK(!ccord_warm_maybe_save(g_env.warm, c->store, c->session, now + 100000u) &&
            ccord_warm_save(g_env.warm, c->store, true, &err) == CCORD_OK && path_missing(wd),
        "remove: no later save recreates it");
  *out_checks = checks;
  return 0;
}

// A legacy roster entry (id 0) never matches meta.json's user id: removal must find the directory by token fingerprint.
// A second removal with no cache directory left is a no-op.
static int phase_remove_legacy(const ccord_verify_ui_parity_ctx *c, int *out_checks)
{
  const char *phase = "remove-legacy";
  int checks = 0;
  ccord_error err;
  char wd[1200] = "";
  const char *d0 = ccord_warm_dir(g_env.warm);
  if (d0) snprintf(wd, sizeof wd, "%s", d0);
  CHECK(g_env.load.loaded && wd[0] && !path_missing(wd) && g_env.remove_account, "remove-legacy: a warm cache is on disk");
  c->frame_ms(c->ud, 1);
  g_env.remove_account(g_env.ud, 0);
  CHECK(path_missing(wd), "remove-legacy: a roster id of 0 still deleted the cache directory by token fingerprint");
  g_env.remove_account(g_env.ud, 0);
  CHECK(path_missing(wd), "remove-legacy: removing an account with no cache directory is a no-op");
  adv(c, SLICE_ADVANCE_MS, 1000);
  uint64_t now = g_env.now_ms ? g_env.now_ms(g_env.ud) : 0;
  CHECK(!ccord_warm_maybe_save(g_env.warm, c->store, c->session, now + 100000u) &&
            ccord_warm_save(g_env.warm, c->store, true, &err) == CCORD_OK && path_missing(wd),
        "remove-legacy: no later save recreates it");
  *out_checks = checks;
  return 0;
}

// settings.ini [ui] warm_cache survives a settings save: "settings-off" starts with it disabled, "settings-unset" without it.
static int phase_settings(const ccord_verify_ui_parity_ctx *c, bool off, int *out_checks)
{
  const char *phase = off ? "settings-off" : "settings-unset";
  int checks = 0;
  CHECK(ccord_warm_pref_enabled(g_env.data_dir) == !off, "settings: the pref reads as written before the save");
  ui_settings_save(c->ui);
  CHECK(ccord_warm_pref_enabled(g_env.data_dir) == !off, "settings: the pref reads the same after ui_settings_save");
  ui_settings_save(c->ui);
  CHECK(ccord_warm_pref_enabled(g_env.data_dir) == !off, "settings: and after a second save");
  *out_checks = checks;
  return 0;
}

static int run_phase(const ccord_verify_ui_parity_ctx *c)
{
  const char *phase = g_env.phase;
  int checks = 0;
  int rc;
  if (!c || !c->frame_ms || !c->input || !c->transport || !c->ui) {
    printf("warm-start[%s]: FAIL slice context incomplete\n", phase);
    return 1;
  }
  if (strcmp(phase, "cold") == 0) rc = phase_cold(c, &checks);
  else if (strcmp(phase, "warm") == 0) rc = phase_warm(c, &checks);
  else if (strcmp(phase, "corrupt") == 0) rc = phase_corrupt(c, &checks);
  else if (strcmp(phase, "other-account") == 0) rc = phase_other_account(c, &checks);
  else if (strcmp(phase, "off") == 0) rc = phase_off(c, &checks);
  else if (strcmp(phase, "signout") == 0) rc = phase_signout(c, &checks);
  else if (strcmp(phase, "remove") == 0) rc = phase_remove(c, &checks);
  else if (strcmp(phase, "remove-legacy") == 0) rc = phase_remove_legacy(c, &checks);
  else if (strcmp(phase, "settings-off") == 0) rc = phase_settings(c, true, &checks);
  else if (strcmp(phase, "settings-unset") == 0) rc = phase_settings(c, false, &checks);
  else {
    printf("warm-start[%s]: FAIL unknown phase\n", phase);
    return 2;
  }
  if (rc != 0) return rc;
  printf("warm-start[%s]: PASS checks=%d\n", phase, checks);
  return 0;
}

// ------------------------------------------------------------------------------------------------ parent

typedef struct parent {
  char ws[1100];            // scratch data dir shared by the children
  char state[1300];         // ws/accounts/<fp>/warm/state.json of the mock account
  int checks;
  bool failed;
} parent;

static void p_fail(parent *p, const char *name)
{
  printf("warm-start: FAIL %s\n", name);
  p->failed = true;
}

#define PCHECK(p, cond, name)  \
  do {                         \
    (p)->checks++;             \
    if (!(cond)) {             \
      p_fail((p), (name));     \
      return false;            \
    }                          \
  } while (0)

// Spawns /proc/self/exe as a child that runs the slice phase `phase` against `data_dir`; adds its checks on PASS.
static bool run_child(parent *p, const char *phase, const char *data_dir, bool no_warm)
{
  int fds[2];
  if (pipe(fds) != 0) {
    p_fail(p, "pipe");
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    p_fail(p, "fork");
    return false;
  }
  if (pid == 0) {
    close(fds[0]);
    if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(126);
    close(fds[1]);
    char lat[16];
    snprintf(lat, sizeof lat, "%u", SLICE_LATENCY_MS);
    const char *argv[24];
    int n = 0;
    argv[n++] = "ccord";
    argv[n++] = "--headless";
    argv[n++] = "--mock";
    argv[n++] = "--verify-slice";
    argv[n++] = "warm-start";
    argv[n++] = "--data-dir";
    argv[n++] = data_dir;
    argv[n++] = "--warm-phase";
    argv[n++] = phase;
    argv[n++] = "--mock-latency-ms";
    argv[n++] = lat;
    argv[n++] = "--log-level";
    argv[n++] = strcmp(phase, "warm") == 0 ? "info" : "warn";
    if (no_warm) argv[n++] = "--no-warm-cache";
    argv[n] = nullptr;
    execv("/proc/self/exe", (char *const *)(uintptr_t)argv);
    _exit(127);
  }
  close(fds[1]);
  char out[8192];
  size_t used = 0;
  for (;;) {
    ssize_t r = read(fds[0], out + used, sizeof out - 1 - used);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    used += (size_t)r;
    if (used >= sizeof out - 1) break;
  }
  out[used] = '\0';
  char drain[256];
  while (read(fds[0], drain, sizeof drain) > 0) {
  }
  close(fds[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  char want[64];
  snprintf(want, sizeof want, "warm-start[%s]: PASS checks=", phase);
  const char *line = strstr(out, want);
  int m = line ? atoi(line + strlen(want)) : -1;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || m <= 0) {
    fprintf(stderr, "warm-start: child phase '%s' failed (status %d)\n%s", phase, status, out);
    char name[96];
    snprintf(name, sizeof name, "child phase %s", phase);
    p_fail(p, name);
    return false;
  }
  p->checks += m;
  return true;
}

static bool put_state(const char *path, const void *data, size_t len, unsigned mode)
{
  ccord_error err;
  return ccord_file_write_atomic(path, data, len, mode, false, &err) == CCORD_OK;
}

// Header + body for a body of our own making (a valid sha for a changed body).
static char *wrap(const char *body, size_t len, size_t *out_len)
{
  unsigned char d[32];
  ccord_sha256(body, len, d);
  char hex[65];
  for (size_t i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
  char head[160];
  int hn = snprintf(head, sizeof head, "CCWARM 1 %s %zu\n", hex, len);
  if (hn <= 0) return nullptr;
  char *all = ccord_malloc((size_t)hn + len + 1);
  if (!all) return nullptr;
  memcpy(all, head, (size_t)hn);
  memcpy(all + hn, body, len);
  all[(size_t)hn + len] = '\0';
  *out_len = (size_t)hn + len;
  return all;
}

static bool same_bytes(const char *path, const char *want, size_t n)
{
  ccord_error err;
  size_t got = 0;
  char *now = ccord_file_read_max(path, CCORD_WARM_MAX_FILE_BYTES + 160u, &got, &err);
  bool ok = now && got == n && memcmp(now, want, n) == 0;
  ccord_free(now);
  return ok;
}

static bool run_parent(parent *p)
{
  const char *base = g_env.data_dir ? g_env.data_dir : "/tmp";
  ccord_error err;
  PCHECK(p, base && snprintf(p->ws, sizeof p->ws, "%s/warm-slice", base) < (int)sizeof p->ws &&
                ccord_mkdir_p(p->ws, 0700, &err) == CCORD_OK,
         "create the scratch data dir");
  char fp[33];
  ccord_warm_fingerprint("mock", fp);
  PCHECK(p, snprintf(p->state, sizeof p->state, "%s/accounts/%s/warm/state.json", p->ws, fp) < (int)sizeof p->state, "state path");

  if (!run_child(p, "cold", p->ws, false)) return false;
  if (!run_child(p, "warm", p->ws, false)) return false;

  size_t olen = 0;
  char *orig = ccord_file_read_max(p->state, CCORD_WARM_MAX_FILE_BYTES + 160u, &olen, &err);
  PCHECK(p, orig && olen > 200, "the warm phase left a readable state file");
  const char *nl = memchr(orig, '\n', olen);
  size_t head_len = nl ? (size_t)(nl - orig) + 1 : 0;
  PCHECK(p, head_len > 0 && head_len < 160, "state header line");

  // Four ways to damage the cache; each start must ignore it and remove the file.
  for (int kind = 0; kind < 4; kind++) {
    char *bad = nullptr;
    size_t blen = 0;
    unsigned mode = 0600;
    if (kind == 0) {   // truncated
      blen = olen / 2;
      bad = ccord_malloc(blen + 1);
      if (bad) memcpy(bad, orig, blen);
    } else if (kind == 1) {   // one flipped byte in the body
      blen = olen;
      bad = ccord_malloc(blen + 1);
      if (bad) {
        memcpy(bad, orig, blen);
        bad[head_len + 100] = (char)(bad[head_len + 100] ^ 0x01);
      }
    } else if (kind == 2) {   // codec version 999 with a valid sha
      const char *body = orig + head_len;
      const char *v = strstr(body, "\"v\":\"1\"");
      if (v) {
        size_t blen_body = olen - head_len;
        char *nb = ccord_malloc(blen_body + 8);
        if (nb) {
          size_t pre = (size_t)(v - body);
          memcpy(nb, body, pre);
          memcpy(nb + pre, "\"v\":\"999\"", 9);
          memcpy(nb + pre + 9, v + 7, blen_body - pre - 7);
          bad = wrap(nb, blen_body + 2, &blen);
          ccord_free(nb);
        }
      }
    } else {   // readable by others
      blen = olen;
      bad = ccord_malloc(blen + 1);
      if (bad) memcpy(bad, orig, blen);
      mode = 0644;
    }
    PCHECK(p, bad != nullptr, "build the damaged state file");
    bool wrote = put_state(p->state, bad, blen, mode);
    ccord_free(bad);
    PCHECK(p, wrote, "write the damaged state file");
    if (!run_child(p, "corrupt", p->ws, false)) {
      ccord_free(orig);
      return false;
    }
    PCHECK(p, path_missing(p->state), "the damaged state file was removed");
    PCHECK(p, put_state(p->state, orig, olen, 0600), "restore the good state file");
  }

  // A different account (token) starts cold in its own directory and leaves the first account's cache alone.
  if (!run_child(p, "other-account", p->ws, false)) {
    ccord_free(orig);
    return false;
  }
  PCHECK(p, same_bytes(p->state, orig, olen), "the first account's cache is untouched by the other account");
  // --no-warm-cache with a valid cache on disk: not read (the child sees a cold start) and not written.
  struct stat before, after;
  PCHECK(p, lstat(p->state, &before) == 0, "stat the cache before --no-warm-cache");
  if (!run_child(p, "off", p->ws, true)) {
    ccord_free(orig);
    return false;
  }
  PCHECK(p, lstat(p->state, &after) == 0 && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
                before.st_mtim.tv_nsec == after.st_mtim.tv_nsec && same_bytes(p->state, orig, olen),
         "--no-warm-cache did not touch the cache file");
  // ...and on an empty data dir it creates no account directory at all.
  char fresh[1200], fresh_acc[1300];
  PCHECK(p, snprintf(fresh, sizeof fresh, "%s/off-fresh", p->ws) < (int)sizeof fresh &&
                snprintf(fresh_acc, sizeof fresh_acc, "%s/accounts", fresh) < (int)sizeof fresh_acc &&
                ccord_mkdir_p(fresh, 0700, &err) == CCORD_OK,
         "fresh dir for --no-warm-cache");
  if (!run_child(p, "off", fresh, true)) {
    ccord_free(orig);
    return false;
  }
  PCHECK(p, path_missing(fresh_acc), "--no-warm-cache created no cache directory");

  // Sign-out and account removal delete the cache; run in their own data dirs so the checks above stay as they were.
  {
    char so[1200], so_acc[1300];
    PCHECK(p, snprintf(so, sizeof so, "%s/signout", p->ws) < (int)sizeof so &&
                  snprintf(so_acc, sizeof so_acc, "%s/accounts/%s", so, fp) < (int)sizeof so_acc,
           "signout dir paths");
    if (!run_child(p, "cold", so, false)) {
      ccord_free(orig);
      return false;
    }
    PCHECK(p, !path_missing(so_acc), "signout: the cold start left a cache directory");
    if (!run_child(p, "signout", so, false)) {
      ccord_free(orig);
      return false;
    }
    PCHECK(p, path_missing(so_acc), "signout: the directory is still absent after the process exited");

    char rm[1200], rm_acc[1300], other[1300], other_meta[1400], other_state[1400];
    PCHECK(p, snprintf(rm, sizeof rm, "%s/remove", p->ws) < (int)sizeof rm &&
                  snprintf(rm_acc, sizeof rm_acc, "%s/accounts/%s", rm, fp) < (int)sizeof rm_acc &&
                  snprintf(other, sizeof other, "%s/accounts/0123456789abcdef0123456789abcdef", rm) < (int)sizeof other &&
                  snprintf(other_meta, sizeof other_meta, "%s/meta.json", other) < (int)sizeof other_meta &&
                  snprintf(other_state, sizeof other_state, "%s/warm/state.json", other) < (int)sizeof other_state,
           "remove dir paths");
    if (!run_child(p, "cold", rm, false)) {
      ccord_free(orig);
      return false;
    }
    static const char other_meta_txt[] = "{\"user_id\":\"777\"}\n";
    static const char other_state_txt[] = "CCWARM 1 not-a-real-cache\nother account bytes\n";
    char other_warm[1400];
    snprintf(other_warm, sizeof other_warm, "%s/warm", other);
    PCHECK(p, ccord_mkdir_p(other_warm, 0700, &err) == CCORD_OK &&
                  put_state(other_meta, other_meta_txt, sizeof other_meta_txt - 1, 0600) &&
                  put_state(other_state, other_state_txt, sizeof other_state_txt - 1, 0600),
           "seed a second account's cache directory");
    PCHECK(p, !path_missing(rm_acc), "remove: the cold start left a cache directory");
    if (!run_child(p, "remove", rm, false)) {
      ccord_free(orig);
      return false;
    }
    PCHECK(p, path_missing(rm_acc), "remove: the removed account's directory is gone after the process exited");
    PCHECK(p, same_bytes(other_meta, other_meta_txt, sizeof other_meta_txt - 1) &&
                  same_bytes(other_state, other_state_txt, sizeof other_state_txt - 1),
           "remove: the other account's cache is byte-identical");

    // Legacy roster id 0: removal by token fingerprint; another account's directory is untouched.
    char rl[1200], rl_acc[1300], rl_other[1300], rl_meta[1400], rl_state[1400], rl_warm[1400];
    PCHECK(p, snprintf(rl, sizeof rl, "%s/remove-legacy", p->ws) < (int)sizeof rl &&
                  snprintf(rl_acc, sizeof rl_acc, "%s/accounts/%s", rl, fp) < (int)sizeof rl_acc &&
                  snprintf(rl_other, sizeof rl_other, "%s/accounts/fedcba9876543210fedcba9876543210", rl) < (int)sizeof rl_other &&
                  snprintf(rl_meta, sizeof rl_meta, "%s/meta.json", rl_other) < (int)sizeof rl_meta &&
                  snprintf(rl_state, sizeof rl_state, "%s/warm/state.json", rl_other) < (int)sizeof rl_state &&
                  snprintf(rl_warm, sizeof rl_warm, "%s/warm", rl_other) < (int)sizeof rl_warm,
           "remove-legacy dir paths");
    if (!run_child(p, "cold", rl, false)) {
      ccord_free(orig);
      return false;
    }
    PCHECK(p, ccord_mkdir_p(rl_warm, 0700, &err) == CCORD_OK &&
                  put_state(rl_meta, other_meta_txt, sizeof other_meta_txt - 1, 0600) &&
                  put_state(rl_state, other_state_txt, sizeof other_state_txt - 1, 0600),
           "remove-legacy: seed a second account's cache directory");
    PCHECK(p, !path_missing(rl_acc), "remove-legacy: the cold start left a cache directory");
    if (!run_child(p, "remove-legacy", rl, false)) {
      ccord_free(orig);
      return false;
    }
    PCHECK(p, path_missing(rl_acc), "remove-legacy: the directory is gone after the process exited");
    PCHECK(p, same_bytes(rl_meta, other_meta_txt, sizeof other_meta_txt - 1) &&
                  same_bytes(rl_state, other_state_txt, sizeof other_state_txt - 1),
           "remove-legacy: the other account's cache is byte-identical");

    // settings.ini keeps [ui] warm_cache across a settings save; without the key the cache stays enabled.
    for (int off = 0; off < 2; off++) {
      char sd[1200], sp[1300];
      PCHECK(p, snprintf(sd, sizeof sd, "%s/settings-%s", p->ws, off ? "off" : "unset") < (int)sizeof sd &&
                    snprintf(sp, sizeof sp, "%s/settings.ini", sd) < (int)sizeof sp && ccord_mkdir_p(sd, 0700, &err) == CCORD_OK,
             "settings dir");
      const char *ini = off ? "[ui]\nwarm_cache = 0\nembeds = 1\n" : "[ui]\nembeds = 1\n";
      PCHECK(p, put_state(sp, ini, strlen(ini), 0600), "write settings.ini");
      if (!run_child(p, off ? "settings-off" : "settings-unset", sd, false)) {
        ccord_free(orig);
        return false;
      }
      size_t sl = 0;
      char *saved = ccord_file_read_max(sp, 65536, &sl, &err);
      bool has = saved && strstr(saved, "warm_cache = 0") != nullptr;
      bool any = saved && strstr(saved, "warm_cache") != nullptr;
      ccord_free(saved);
      PCHECK(p, off ? has : !any, off ? "settings.ini still holds warm_cache = 0 after the saves"
                                      : "settings.ini gained no warm_cache key");
    }
  }

  // The account directories: private all the way down, forgotten by user id and by handle.
  size_t seen = 0;
  char acc[1200];
  snprintf(acc, sizeof acc, "%s/accounts", p->ws);
  PCHECK(p, walk_private(acc, &seen) == 0 && seen >= 6, "after all phases nothing under accounts/ has group/other bits");
  char meta[1300], acct[1200];
  snprintf(acct, sizeof acct, "%s/accounts/%s", p->ws, fp);
  snprintf(meta, sizeof meta, "%s/meta.json", acct);
  size_t mlen = 0;
  char *mtext = ccord_file_read_max(meta, 4096, &mlen, &err);
  const char *uid = mtext ? strstr(mtext, "\"user_id\":\"") : nullptr;
  ccord_id user_id = uid ? (ccord_id)strtoull(uid + 11, nullptr, 10) : CCORD_ID_NONE;
  ccord_free(mtext);
  ccord_free(orig);
  PCHECK(p, user_id != CCORD_ID_NONE, "meta.json names the account's user id");
  ccord_warm_forget_user(p->ws, user_id);
  PCHECK(p, path_missing(acct), "ccord_warm_forget_user removed the account directory by user id");
  ccord_warm_config wc = {.data_dir = p->ws, .token = "mock", .app_version = "slice", .enabled = true};
  ccord_warm *w = ccord_warm_open(&wc, &err);
  PCHECK(p, w != nullptr && ccord_warm_dir(w) != nullptr, "reopen the account directory");
  PCHECK(p, ccord_warm_put_history(w, 4242, "[]", 2, &err) == CCORD_OK && !path_missing(acct), "a history file lands in it");
  ccord_warm_forget(w);
  PCHECK(p, path_missing(acct), "ccord_warm_forget removed the account directory");
  ccord_warm_close(w);
  (void)ccord_rmtree(p->ws, &err);
  return true;
}

int ccord_verify_warm_start_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  if (g_env.phase) return run_phase(ctx);
  parent p = {0};
  bool ok = run_parent(&p);
  if (!ok || p.failed) return 1;
  printf("warm-start: PASS checks=%d\n", p.checks);
  return 0;
}
