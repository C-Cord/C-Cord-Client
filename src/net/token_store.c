// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/net/live_transport.h>
#include <ccord/core/atomic_file.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ccord/core/alloc.h>

#define TOKEN_FILE_MAX 4096u
#define ACCOUNTS_FILE_MAX 16384u
#define ACCOUNTS_MIGRATED_SLOT "legacy"

static char *read_file_max(const char *path, size_t max, size_t *out_len)
{
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return nullptr;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      (uintmax_t)st.st_size > (uintmax_t)max) {
    close(fd);
    return nullptr;
  }
  size_t n = (size_t)st.st_size;
  if (n >= max) {
    close(fd);
    return nullptr;
  }
  char *buf = ccord_malloc(n + 1);
  if (!buf) {
    close(fd);
    return nullptr;
  }
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, buf + got, n - got);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) break;
    got += (size_t)r;
  }
  close(fd);
  if (got == 0) {
    ccord_live_wipe(buf, n);
    ccord_free(buf);
    return nullptr;
  }
  buf[got] = '\0';
  *out_len = got;
  return buf;
}

void ccord_live_wipe(void *p, size_t n)
{
  if (!p) return;
  volatile unsigned char *v = p;
  while (n > 0) {
    *v++ = 0;
    n--;
  }
}

ccord_status ccord_live_token_path(char *out, size_t cap, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || cap == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "token: null path buffer");
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0] != '\0') {
    size_t xn = strlen(xdg);
    while (xn > 1 && xdg[xn - 1] == '/') xn--;
    const char *suffix = "/ccord/token";
    size_t sn = strlen(suffix);
    if (xn + sn + 1 > cap) return ccord_error_set(err, CCORD_ERR_RANGE, "token: config path too long");
    memcpy(out, xdg, xn);
    memcpy(out + xn, suffix, sn + 1);
    return CCORD_OK;
  }
  const char *home = getenv("HOME");
  if (!home || home[0] == '\0') return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "token: HOME is not set");
  size_t hn = strlen(home);
  while (hn > 1 && home[hn - 1] == '/') hn--;
  const char *suffix = "/.config/ccord/token";
  size_t sn = strlen(suffix);
  if (hn + sn + 1 > cap) return ccord_error_set(err, CCORD_ERR_RANGE, "token: home path too long");
  memcpy(out, home, hn);
  memcpy(out + hn, suffix, sn + 1);
  return CCORD_OK;
}

static void trim_token_bounds(const char *s, size_t len, size_t *out_start, size_t *out_len)
{
  size_t a = 0;
  while (a < len && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n' || s[a] == '"')) a++;
  size_t b = len;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n' || s[b - 1] == '"')) {
    b--;
  }
  *out_start = a;
  *out_len = b - a;
}

static ccord_status ensure_parent_dirs(const char *file_path, ccord_error *err)
{
  size_t len = strlen(file_path);
  if (len == 0 || len >= 4096) return ccord_error_set(err, CCORD_ERR_RANGE, "token: path too long");
  char tmp[4096];
  memcpy(tmp, file_path, len + 1);
  for (size_t i = 1; i < len; i++) {
    if (tmp[i] != '/') continue;
    tmp[i] = '\0';
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
      return ccord_error_set(err, CCORD_ERR_IO, "token: cannot create config directory");
    }
    tmp[i] = '/';
  }
  return CCORD_OK;
}

ccord_status ccord_live_save_token(const char *path_or_null, const char *token, ccord_error *err)
{
  ccord_error_clear(err);
  if (!token) return ccord_error_set(err, CCORD_ERR_INVALID, "token: null token");
  size_t raw_len = strlen(token);
  size_t start = 0, tlen = 0;
  trim_token_bounds(token, raw_len, &start, &tlen);
  if (tlen == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "token: empty token");
  if (tlen > CCORD_LIVE_TOKEN_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "token: too long");
  for (size_t i = 0; i < tlen; i++) {
    unsigned char c = (unsigned char)token[start + i];
    if (c <= 0x20 || c >= 0x7f) return ccord_error_set(err, CCORD_ERR_INVALID, "token: invalid character");
  }

  char resolved[4096];
  const char *target = path_or_null;
  if (!target || target[0] == '\0') {
    ccord_status st = ccord_live_token_path(resolved, sizeof resolved, err);
    if (st != CCORD_OK) return st;
    target = resolved;
  }
  ccord_status dst = ensure_parent_dirs(target, err);
  if (dst != CCORD_OK) return dst;
  return ccord_file_write_atomic(target, token + start, tlen, 0600u, true, err);
}

ccord_status ccord_live_delete_token(const char *path_or_null, ccord_error *err)
{
  ccord_error_clear(err);
  char resolved[4096];
  const char *target = path_or_null;
  if (!target || target[0] == '\0') {
    ccord_status st = ccord_live_token_path(resolved, sizeof resolved, err);
    if (st != CCORD_OK) return st;
    target = resolved;
  }
  int fd = open(target, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd >= 0) {
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 4096) {
      char zeros[256];
      memset(zeros, 0, sizeof zeros);
      size_t rem = (size_t)st.st_size;
      while (rem > 0) {
        size_t chunk = rem < sizeof zeros ? rem : sizeof zeros;
        ssize_t w = write(fd, zeros, chunk);
        if (w <= 0) break;
        rem -= (size_t)w;
      }
    }
    close(fd);
  }
  if (unlink(target) != 0 && errno != ENOENT) {
    return ccord_error_set(err, CCORD_ERR_IO, "token: unlink failed");
  }
  return CCORD_OK;
}

static ccord_status load_from_token_file(const char *path, char *out, size_t cap, ccord_error *err)
{
  size_t n = 0;
  char *buf = read_file_max(path, TOKEN_FILE_MAX, &n);
  if (!buf) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "token: standalone token file not found");
  size_t start = 0, tlen = 0;
  trim_token_bounds(buf, n, &start, &tlen);
  ccord_status st = CCORD_OK;
  if (tlen == 0) {
    st = ccord_error_set(err, CCORD_ERR_NOT_FOUND, "token: token file is empty");
  } else if (tlen >= cap) {
    st = ccord_error_set(err, CCORD_ERR_RANGE, "token: token too long");
  } else {
    for (size_t i = 0; i < tlen; i++) {
      unsigned char c = (unsigned char)buf[start + i];
      if (c <= 0x20 || c >= 0x7f) {
        ccord_live_wipe(buf, n);
        ccord_free(buf);
        return ccord_error_set(err, CCORD_ERR_INVALID, "token: invalid character");
      }
    }
    memcpy(out, buf + start, tlen);
    out[tlen] = '\0';
  }
  ccord_live_wipe(buf, n);
  ccord_free(buf);
  return st;
}

ccord_status ccord_live_load_token(const char *token_path_or_null, char *out, size_t cap, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || cap == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "token: null output");

  const char *env = getenv(CCORD_LIVE_TOKEN_ENV);
  if (env && env[0] != '\0') {
    size_t start = 0, tlen = 0;
    trim_token_bounds(env, strlen(env), &start, &tlen);
    if (tlen == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "token: CCORD_DISCORD_TOKEN is empty");
    if (tlen >= cap) return ccord_error_set(err, CCORD_ERR_RANGE, "token: too long");
    for (size_t i = 0; i < tlen; i++) {
      unsigned char c = (unsigned char)env[start + i];
      if (c <= 0x20 || c >= 0x7f) return ccord_error_set(err, CCORD_ERR_INVALID, "token: invalid character");
    }
    memcpy(out, env + start, tlen);
    out[tlen] = '\0';
    return CCORD_OK;
  }

  if (token_path_or_null && token_path_or_null[0] != '\0') {
    return load_from_token_file(token_path_or_null, out, cap, err);
  }

  char token_path[4096];
  ccord_status pst = ccord_live_token_path(token_path, sizeof token_path, err);
  if (pst != CCORD_OK) return pst;
  return load_from_token_file(token_path, out, cap, err);
}

// ------------------------------------------------------------- multi-account keyring

static ccord_status config_subpath(const char *suffix, char *out, size_t cap, ccord_error *err)
{
  char token_path[4096];
  ccord_status st = ccord_live_token_path(token_path, sizeof token_path, err);
  if (st != CCORD_OK) return st;
  char *slash = strrchr(token_path, '/');
  if (!slash) return ccord_error_set(err, CCORD_ERR_IO, "token: config path has no directory");
  *slash = '\0';
  size_t dir_len = strlen(token_path);
  size_t suffix_len = strlen(suffix);
  if (dir_len + suffix_len + 1 > cap) return ccord_error_set(err, CCORD_ERR_RANGE, "token: config path too long");
  memcpy(out, token_path, dir_len);
  memcpy(out + dir_len, suffix, suffix_len + 1);
  return CCORD_OK;
}

static ccord_status accounts_path(char *out, size_t cap, ccord_error *err)
{
  return config_subpath("/accounts.ini", out, cap, err);
}

static ccord_status slot_token_path(const char *slot, char *out, size_t cap, ccord_error *err)
{
  char suffix[CCORD_SAVED_ACCOUNT_SLOT_MAX + 16u];
  int n = snprintf(suffix, sizeof suffix, "/tokens/%s", slot);
  if (n < 0 || (size_t)n >= sizeof suffix) return ccord_error_set(err, CCORD_ERR_RANGE, "token: slot id too long");
  return config_subpath(suffix, out, cap, err);
}

static bool slot_is_valid(const char *slot)
{
  if (!slot || !slot[0]) return false;
  size_t n = strlen(slot);
  if (n >= CCORD_SAVED_ACCOUNT_SLOT_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    char c = slot[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static void copy_field(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
  for (size_t i = 0; i < n; i++) {
    if (dst[i] == '\n' || dst[i] == '\r') dst[i] = ' ';
  }
}

static void trim_in_place(char *s)
{
  if (!s) return;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
    s[--n] = '\0';
  }
  size_t first = 0;
  while (s[first] == ' ' || s[first] == '\t') first++;
  if (first > 0) memmove(s, s + first, strlen(s + first) + 1);
}

static uint64_t parse_user_id(const char *s)
{
  if (!s || !s[0]) return 0;
  char *end = nullptr;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 10);
  if (errno != 0 || !end || end == s) return 0;
  return (uint64_t)v;
}

typedef struct account_db {
  ccord_saved_account entries[CCORD_MAX_SAVED_ACCOUNTS];
  size_t count;
  int active;   // CCORD_NO_ACTIVE_ACCOUNT when none
} account_db;

static bool append_text(char *buf, size_t cap, size_t *len, const char *text)
{
  size_t n = strlen(text);
  if (*len >= cap || n >= cap - *len) return false;
  memcpy(buf + *len, text, n);
  *len += n;
  buf[*len] = '\0';
  return true;
}

static ccord_status accounts_save(const account_db *db, ccord_error *err)
{
  char buf[ACCOUNTS_FILE_MAX];
  size_t len = 0;
  buf[0] = '\0';
  if (!append_text(buf, sizeof buf, &len, "# ccord accounts v1\n")) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "token: accounts file too large");
  }

  char line[1024];
  int n;
  if (db->active >= 0 && (size_t)db->active < db->count) {
    n = snprintf(line, sizeof line, "active=%s\n", db->entries[db->active].slot_id);
  } else {
    n = snprintf(line, sizeof line, "active=\n");
  }
  if (n < 0 || (size_t)n >= sizeof line || !append_text(buf, sizeof buf, &len, line)) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "token: accounts file too large");
  }

  for (size_t i = 0; i < db->count; i++) {
    const ccord_saved_account *e = &db->entries[i];
    n = snprintf(line, sizeof line,
                 "[%s]\nuser_id=%llu\nusername=%s\ndisplay_name=%s\navatar_hash=%s\n\n",
                 e->slot_id, (unsigned long long)e->user_id, e->username, e->display_name, e->avatar_hash);
    if (n < 0 || (size_t)n >= sizeof line || !append_text(buf, sizeof buf, &len, line)) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "token: accounts file too large");
    }
  }

  char path[4096];
  ccord_status st = accounts_path(path, sizeof path, err);
  if (st != CCORD_OK) return st;
  st = ensure_parent_dirs(path, err);
  if (st != CCORD_OK) return st;
  return ccord_file_write_atomic(path, buf, len, 0600u, true, err);
}

static ccord_status accounts_parse(account_db *db, char *buf, ccord_error *err)
{
  (void)err;
  char active_slot[CCORD_SAVED_ACCOUNT_SLOT_MAX] = {0};
  int current = -1;
  char *line = buf;
  while (line && *line) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    trim_in_place(line);
    if (*line && *line != '#' && *line != ';') {
      if (line[0] == '[') {
        char *close = strchr(line, ']');
        if (close) {
          *close = '\0';
          char *name = line + 1;
          trim_in_place(name);
          current = -1;
          if (name[0] && slot_is_valid(name)) {
            for (size_t i = 0; i < db->count; i++) {
              if (strcmp(db->entries[i].slot_id, name) == 0) {
                current = (int)i;
                break;
              }
            }
            if (current < 0 && db->count < CCORD_MAX_SAVED_ACCOUNTS) {
              current = (int)db->count;
              memset(&db->entries[db->count], 0, sizeof db->entries[db->count]);
              copy_field(db->entries[db->count].slot_id, sizeof db->entries[db->count].slot_id, name);
              db->count++;
            }
          }
        }
      } else {
        char *eq = strchr(line, '=');
        if (eq) {
          *eq = '\0';
          char *key = line;
          char *val = eq + 1;
          trim_in_place(key);
          trim_in_place(val);
          if (current < 0) {
            if (strcmp(key, "active") == 0) copy_field(active_slot, sizeof active_slot, val);
          } else {
            ccord_saved_account *e = &db->entries[current];
            if (strcmp(key, "user_id") == 0) e->user_id = parse_user_id(val);
            else if (strcmp(key, "username") == 0) copy_field(e->username, sizeof e->username, val);
            else if (strcmp(key, "display_name") == 0) copy_field(e->display_name, sizeof e->display_name, val);
            else if (strcmp(key, "avatar_hash") == 0) copy_field(e->avatar_hash, sizeof e->avatar_hash, val);
          }
        }
      }
    }
    line = nl ? nl + 1 : nullptr;
  }

  db->active = CCORD_NO_ACTIVE_ACCOUNT;
  for (size_t i = 0; i < db->count; i++) db->entries[i].active = false;
  if (active_slot[0]) {
    for (size_t i = 0; i < db->count; i++) {
      if (strcmp(db->entries[i].slot_id, active_slot) == 0) {
        db->active = (int)i;
        db->entries[i].active = true;
        break;
      }
    }
  }
  return CCORD_OK;
}

static ccord_status migrate_legacy_token(account_db *db, ccord_error *err)
{
  char legacy_path[4096];
  ccord_status st = ccord_live_token_path(legacy_path, sizeof legacy_path, err);
  if (st != CCORD_OK) return st;

  char token[CCORD_LIVE_TOKEN_MAX + 1];
  st = load_from_token_file(legacy_path, token, sizeof token, err);
  if (st == CCORD_ERR_NOT_FOUND) {
    ccord_error_clear(err);
    return CCORD_OK;
  }
  if (st != CCORD_OK) return st;

  memset(db, 0, sizeof *db);
  db->active = CCORD_NO_ACTIVE_ACCOUNT;
  ccord_saved_account *e = &db->entries[0];
  copy_field(e->slot_id, sizeof e->slot_id, ACCOUNTS_MIGRATED_SLOT);
  copy_field(e->display_name, sizeof e->display_name, "Saved account");
  e->active = true;
  db->count = 1;
  db->active = 0;

  char slot_path[4096];
  st = slot_token_path(e->slot_id, slot_path, sizeof slot_path, err);
  if (st == CCORD_OK) st = ccord_live_save_token(slot_path, token, err);
  ccord_live_wipe(token, sizeof token);
  if (st != CCORD_OK) return st;
  return accounts_save(db, err);
}

static ccord_status accounts_load(account_db *db, bool migrate, ccord_error *err)
{
  memset(db, 0, sizeof *db);
  db->active = CCORD_NO_ACTIVE_ACCOUNT;

  char path[4096];
  ccord_status st = accounts_path(path, sizeof path, err);
  if (st != CCORD_OK) return st;

  size_t n = 0;
  char *buf = read_file_max(path, ACCOUNTS_FILE_MAX, &n);
  if (!buf) {
    if (access(path, F_OK) == 0) return CCORD_OK;
    return migrate ? migrate_legacy_token(db, err) : CCORD_OK;
  }
  st = accounts_parse(db, buf, err);
  ccord_live_wipe(buf, n);
  ccord_free(buf);
  return st;
}

static int db_find(const account_db *db, const char *key)
{
  if (!key || !key[0]) return -1;
  for (size_t i = 0; i < db->count; i++) {
    if (strcmp(db->entries[i].slot_id, key) == 0) return (int)i;
  }
  uint64_t id = parse_user_id(key);
  if (id != 0) {
    for (size_t i = 0; i < db->count; i++) {
      if (db->entries[i].user_id == id) return (int)i;
    }
  }
  return -1;
}

ccord_status ccord_live_accounts_list(ccord_saved_account *out, size_t cap, size_t *out_count,
                                      int *out_active_index, ccord_error *err)
{
  ccord_error_clear(err);
  account_db db;
  ccord_status st = accounts_load(&db, true, err);
  if (st != CCORD_OK) return st;

  size_t copy = db.count < cap ? db.count : cap;
  if (out) {
    for (size_t i = 0; i < copy; i++) out[i] = db.entries[i];
  }
  if (out_count) *out_count = db.count;
  if (out_active_index) *out_active_index = db.active;
  return CCORD_OK;
}

ccord_status ccord_live_account_upsert(const ccord_saved_account *account, const char *token,
                                       bool make_active, ccord_error *err)
{
  ccord_error_clear(err);
  if (!account) return ccord_error_set(err, CCORD_ERR_INVALID, "token: null account");
  if (!token || !token[0]) return ccord_error_set(err, CCORD_ERR_INVALID, "token: null account token");

  char slot[CCORD_SAVED_ACCOUNT_SLOT_MAX];
  if (account->slot_id[0]) {
    copy_field(slot, sizeof slot, account->slot_id);
  } else if (account->user_id != 0) {
    int n = snprintf(slot, sizeof slot, "%llu", (unsigned long long)account->user_id);
    if (n < 0 || (size_t)n >= sizeof slot) return ccord_error_set(err, CCORD_ERR_RANGE, "token: slot id too long");
  } else {
    return ccord_error_set(err, CCORD_ERR_INVALID, "token: account needs a slot id or user id");
  }
  if (!slot_is_valid(slot)) return ccord_error_set(err, CCORD_ERR_INVALID, "token: invalid slot id");

  account_db db;
  ccord_status st = accounts_load(&db, true, err);
  if (st != CCORD_OK) return st;

  int idx = -1;
  if (account->user_id != 0) {
    for (size_t i = 0; i < db.count; i++) {
      if (db.entries[i].user_id == account->user_id) {
        idx = (int)i;
        break;
      }
    }
  }
  if (idx < 0) idx = db_find(&db, slot);
  if (idx < 0) {
    if (db.count >= CCORD_MAX_SAVED_ACCOUNTS) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "token: too many saved accounts");
    }
    idx = (int)db.count;
    memset(&db.entries[idx], 0, sizeof db.entries[idx]);
    copy_field(db.entries[idx].slot_id, sizeof db.entries[idx].slot_id, slot);
    db.count++;
  }

  ccord_saved_account *e = &db.entries[idx];
  e->user_id = account->user_id;
  copy_field(e->username, sizeof e->username, account->username);
  copy_field(e->display_name, sizeof e->display_name, account->display_name);
  copy_field(e->avatar_hash, sizeof e->avatar_hash, account->avatar_hash);

  char slot_path[4096];
  st = slot_token_path(e->slot_id, slot_path, sizeof slot_path, err);
  if (st == CCORD_OK) st = ccord_live_save_token(slot_path, token, err);
  if (st != CCORD_OK) return st;

  if (make_active || db.active < 0) db.active = idx;
  for (size_t i = 0; i < db.count; i++) db.entries[i].active = ((int)i == db.active);

  st = accounts_save(&db, err);
  if (st != CCORD_OK) return st;
  if (db.active == idx) return ccord_live_save_token(nullptr, token, err);
  return CCORD_OK;
}

ccord_status ccord_live_account_switch(const char *user_id_or_slot, char *out_token, size_t cap, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out_token || cap == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "token: null output");

  account_db db;
  ccord_status st = accounts_load(&db, true, err);
  if (st != CCORD_OK) return st;
  int idx = db_find(&db, user_id_or_slot);
  if (idx < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "token: saved account not found");

  char slot_path[4096];
  st = slot_token_path(db.entries[idx].slot_id, slot_path, sizeof slot_path, err);
  if (st != CCORD_OK) return st;
  st = load_from_token_file(slot_path, out_token, cap, err);
  if (st != CCORD_OK) return st;

  db.active = idx;
  for (size_t i = 0; i < db.count; i++) db.entries[i].active = ((int)i == db.active);
  st = accounts_save(&db, err);
  if (st != CCORD_OK) return st;
  return ccord_live_save_token(nullptr, out_token, err);
}

ccord_status ccord_live_account_peek_token(const char *user_id_or_slot, char *out_token, size_t cap, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out_token || cap == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "token: null output");
  out_token[0] = '\0';

  account_db db;
  ccord_status st = accounts_load(&db, false, err);  // no legacy migration: nothing is written
  if (st != CCORD_OK) return st;
  int idx = db_find(&db, user_id_or_slot);
  if (idx < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "token: saved account not found");

  char slot_path[4096];
  st = slot_token_path(db.entries[idx].slot_id, slot_path, sizeof slot_path, err);
  if (st != CCORD_OK) return st;
  return load_from_token_file(slot_path, out_token, cap, err);
}

ccord_status ccord_live_account_remove(const char *user_id_or_slot, ccord_error *err)
{
  ccord_error_clear(err);
  account_db db;
  ccord_status st = accounts_load(&db, true, err);
  if (st != CCORD_OK) return st;
  int idx = db_find(&db, user_id_or_slot);
  if (idx < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "token: saved account not found");

  char removed_slot[CCORD_SAVED_ACCOUNT_SLOT_MAX];
  copy_field(removed_slot, sizeof removed_slot, db.entries[idx].slot_id);
  bool was_active = db.active == idx;

  for (size_t i = (size_t)idx; i + 1 < db.count; i++) db.entries[i] = db.entries[i + 1];
  db.count--;
  if (was_active) {
    if (db.count == 0) db.active = CCORD_NO_ACTIVE_ACCOUNT;
    else if ((size_t)idx >= db.count) db.active = (int)db.count - 1;
    else db.active = idx;
  } else if (db.active > idx) {
    db.active--;
  }
  for (size_t i = 0; i < db.count; i++) db.entries[i].active = ((int)i == db.active);

  char slot_path[4096];
  st = slot_token_path(removed_slot, slot_path, sizeof slot_path, err);
  if (st == CCORD_OK) st = ccord_live_delete_token(slot_path, err);
  if (st != CCORD_OK) return st;

  st = accounts_save(&db, err);
  if (st != CCORD_OK) return st;

  if (db.active < 0) return ccord_live_delete_token(nullptr, err);
  if (!was_active) return CCORD_OK;

  char token[CCORD_LIVE_TOKEN_MAX + 1];
  st = slot_token_path(db.entries[db.active].slot_id, slot_path, sizeof slot_path, err);
  if (st == CCORD_OK) st = load_from_token_file(slot_path, token, sizeof token, err);
  if (st == CCORD_OK) st = ccord_live_save_token(nullptr, token, err);
  ccord_live_wipe(token, sizeof token);
  return st;
}
