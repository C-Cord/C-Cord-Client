// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "warm_cache_internal.h"

#include <ccord/core/os.h>

// ------------------------------------------------------------------------------------------------ small helpers

int64_t warm_now(const ccord_warm *w)
{
  return w->now_ms ? w->now_ms(w->clock_ud) : ccord_clock_unix_ms();
}

static void to_hex(const unsigned char *in, size_t n, char *out)
{
  static const char hx[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hx[in[i] >> 4];
    out[i * 2 + 1] = hx[in[i] & 15];
  }
  out[n * 2] = '\0';
}

void ccord_warm_fingerprint(const char *token, char out[33])
{
  static const char prefix[] = "ccord-warm-v1";   // followed by a NUL byte, then the token
  const char *t = (token && token[0]) ? token : "mock";
  ccord_sha256_ctx c;
  ccord_sha256_init(&c);
  ccord_sha256_update(&c, prefix, sizeof prefix);   // includes the terminating NUL
  ccord_sha256_update(&c, t, strlen(t));
  unsigned char d[32];
  ccord_sha256_final(&c, d);
  to_hex(d, 16, out);
}

typedef struct pref_state { bool enabled; } pref_state;

static bool pref_visit(const char *section, const char *key, const char *value, int line, void *ud, ccord_error *err)
{
  (void)line;
  (void)err;
  pref_state *p = ud;
  if (strcmp(section, "ui") != 0 || strcmp(key, "warm_cache") != 0) return true;
  if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "off") == 0 || strcmp(value, "no") == 0) {
    p->enabled = false;
  } else {
    p->enabled = true;
  }
  return true;
}

bool ccord_warm_pref_enabled(const char *data_dir)
{
  if (!data_dir || !data_dir[0]) return true;
  char path[1100];
  if (snprintf(path, sizeof path, "%s/settings.ini", data_dir) >= (int)sizeof path) return true;
  FILE *f = fopen(path, "rb");
  if (!f) return true;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return true;
  }
  long sz = ftell(f);
  if (sz < 0 || sz > 65536 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return true;   // an oversize or unreadable settings file keeps the default
  }
  size_t cap = (size_t)sz;
  if (cap == 0) {
    fclose(f);
    return true;
  }
  char *buf = ccord_malloc(cap + 1u);
  if (!buf) {
    fclose(f);
    return true;
  }
  size_t n = fread(buf, 1, cap, f);
  int extra = fgetc(f);
  fclose(f);
  if (extra != EOF) {
    ccord_free(buf);
    return true;
  }
  buf[n] = '\0';
  pref_state st = {.enabled = true};
  ccord_error e;
  ccord_status st_parse = ccord_ini_parse(buf, n, pref_visit, &st, &e);
  ccord_free(buf);
  if (st_parse != CCORD_OK) return true;
  return st.enabled;
}


// ------------------------------------------------------------------------------------------------ files

static bool join_path(char *out, size_t cap, const char *a, const char *b)
{
  int n = snprintf(out, cap, "%s/%s", a, b);
  return n > 0 && (size_t)n < cap;
}

bool warm_state_path(const ccord_warm *w, char *out, size_t cap)
{
  int n = snprintf(out, cap, "%s/warm/state.json", w->dir);
  return n > 0 && (size_t)n < cap;
}

bool warm_history_path(const ccord_warm *w, ccord_id ch, char *out, size_t cap)
{
  int n = snprintf(out, cap, "%s/warm/history/%" PRIu64 ".json", w->dir, (uint64_t)ch);
  return n > 0 && (size_t)n < cap;
}

bool ccord_warm_state_path(const ccord_warm *w, char *out, size_t cap)
{
  return w && w->enabled && out && cap && warm_state_path(w, out, cap);
}

// Formats the header line "CCWARM 1 <sha256 hex> <length>\n" directly into the reserved
// WARM_HEADER_MAX prefix of `buf`, avoiding a second multi-megabyte allocation and memcpy.
static const char *wrap_with_header_inplace(ccord_buf *buf, size_t *out_len)
{
  if (!buf || !buf->data || buf->len < WARM_HEADER_MAX) return nullptr;
  const char *body = buf->data + WARM_HEADER_MAX;
  size_t len = buf->len - WARM_HEADER_MAX;
  unsigned char d[32];
  ccord_sha256(body, len, d);
  char hex[65];
  to_hex(d, 32, hex);
  char head[WARM_HEADER_MAX];
  int hn = snprintf(head, sizeof head, WARM_MAGIC " %u %s %zu\n", CCORD_WARM_FORMAT_VERSION, hex, len);
  if (hn <= 0 || (size_t)hn >= sizeof head) return nullptr;
  char *start = buf->data + (WARM_HEADER_MAX - (size_t)hn);
  memcpy(start, head, (size_t)hn);
  *out_len = (size_t)hn + len;
  return start;
}

// Parses the header line "CCWARM <ver> <64 hex> <blen>\n" without hashing the body.
bool warm_parse_header_line(const char *text, size_t n, unsigned *version, char hex[65], size_t *hdr_len,
                              size_t *body_len)
{
  const char *nl = memchr(text, '\n', n < WARM_HEADER_MAX ? n : WARM_HEADER_MAX);
  if (!nl) return false;
  char head[WARM_HEADER_MAX];
  size_t hl = (size_t)(nl - text);
  if (hl >= sizeof head) return false;
  memcpy(head, text, hl);
  head[hl] = '\0';
  char magic[8], hex_in[80];
  unsigned ver = 0;
  size_t blen = 0;
  if (sscanf(head, "%7s %u %79s %zu", magic, &ver, hex_in, &blen) != 4) return false;
  if (strcmp(magic, WARM_MAGIC) != 0 || strlen(hex_in) != 64) return false;
  memcpy(hex, hex_in, 65);
  *version = ver;
  *hdr_len = hl + 1;
  *body_len = blen;
  return true;
}

bool warm_verify_body_sha256(const char *body, size_t blen, const char hex[65])
{
  unsigned char d[32];
  ccord_sha256(body, blen, d);
  char got[65];
  to_hex(d, 32, got);
  return strcmp(got, hex) == 0;
}

ccord_json_limits warm_limits(void)
{
  return (ccord_json_limits){.max_depth = 8, .max_nodes = (size_t)1 << 21, .max_string = 64u << 10};
}

bool ccord_warm_verify_file(const char *path, int64_t *saved_ms, size_t *body_len, size_t max_bytes)
{
  if (!path) return false;
  size_t cap = max_bytes ? max_bytes : CCORD_WARM_MAX_FILE_BYTES + WARM_HEADER_MAX;
  // Validate the short magic/version/length header before allocating & hashing the full file.
  FILE *pf = fopen(path, "rb");
  if (!pf) return false;
  char peek[WARM_HEADER_MAX];
  size_t pn = fread(peek, 1, sizeof peek, pf);
  fclose(pf);
  unsigned pver = 0;
  char phex[65];
  size_t phl = 0, pbl = 0;
  if (!warm_parse_header_line(peek, pn, &pver, phex, &phl, &pbl) || pver != CCORD_WARM_FORMAT_VERSION ||
      pbl > cap || phl > cap - pbl) {
    return false;
  }

  ccord_error e;
  size_t n = 0;
  char *text = ccord_file_read_max(path, cap, &n, &e);
  if (!text) return false;
  unsigned ver = 0;
  char hex[65];
  size_t hl = 0, bl = 0;
  bool ok = warm_parse_header_line(text, n, &ver, hex, &hl, &bl) && ver == CCORD_WARM_FORMAT_VERSION &&
            n >= hl && bl == n - hl && warm_verify_body_sha256(text + hl, bl, hex);
  if (ok && saved_ms) {
    ccord_arena a;
    ccord_arena_init(&a, 0);
    ccord_json_limits lim = warm_limits();
    const ccord_json *root = ccord_json_parse(&a, text + hl, bl, &lim, &e);
    int64_t v = 0;
    ok = root && ccord_json_int64(ccord_json_get(root, "saved_ms"), &v);
    *saved_ms = v;
    ccord_arena_free(&a);
  }
  if (ok && body_len) *body_len = bl;
  ccord_free(text);
  return ok;
}

// ------------------------------------------------------------------------------------------------ open / close

static bool is_warm_fp_dir(const char *name)
{
  if (!name) return false;
  for (size_t i = 0; i < 32u; i++) {
    char c = name[i];
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return name[32] == '\0';
}

static void prune_account_dirs(const ccord_warm *w)
{
  char root[1100];
  if (snprintf(root, sizeof root, "%s/accounts", w->data_dir) >= (int)sizeof root) return;
  ccord_dir *d = ccord_dir_open(root);
  if (!d) return;
  struct victim {
    char name[64];
    int64_t mtime;
  } list[64];
  size_t n = 0;
  char ename[256];
  while (ccord_dir_next(d, ename, sizeof ename, nullptr) && n < 64) {
    if (ename[0] == '.' || strcmp(ename, w->fp) == 0 || strlen(ename) >= sizeof list[0].name) continue;
    // Only prune warm-cache 32-hex fingerprint directories; leave per-user scheduled_messages directories intact.
    if (!is_warm_fp_dir(ename)) continue;
    char p[1300];
    int64_t mtime_ns = 0;
    if (snprintf(p, sizeof p, "%s/%s/scheduled_messages.json", root, ename) < (int)sizeof p &&
        ccord_file_mtime_ns(p, &mtime_ns)) {
      continue;
    }
    if (snprintf(p, sizeof p, "%s/%s/meta.json", root, ename) >= (int)sizeof p || !ccord_file_mtime_ns(p, &mtime_ns)) {
      if (snprintf(p, sizeof p, "%s/%s", root, ename) >= (int)sizeof p || !ccord_file_mtime_ns(p, &mtime_ns)) continue;
    }
    memcpy(list[n].name, ename, strlen(ename) + 1);
    list[n].mtime = mtime_ns;
    n++;
  }
  ccord_dir_close(d);
  // Keep CCORD_WARM_MAX_ACCOUNT_DIRS in total: this account plus the newest others.
  while (n > CCORD_WARM_MAX_ACCOUNT_DIRS - 1) {
    size_t oldest = 0;
    for (size_t i = 1; i < n; i++) {
      if (list[i].mtime < list[oldest].mtime) oldest = i;
    }
    char p[1300];
    if (snprintf(p, sizeof p, "%s/%s", root, list[oldest].name) < (int)sizeof p) {
      ccord_error re;
      (void)ccord_rmtree(p, &re);
    }
    list[oldest] = list[--n];
  }
}

ccord_warm *ccord_warm_open(const ccord_warm_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  if (!cfg) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "warm: null config");
    return nullptr;
  }
  ccord_warm *w = ccord_calloc(1, sizeof *w);
  if (!w) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "warm: out of memory");
    return nullptr;
  }
  w->now_ms = cfg->now_ms;
  w->clock_ud = cfg->clock_ud;
  w->max_bytes = cfg->max_file_bytes ? cfg->max_file_bytes : CCORD_WARM_MAX_FILE_BYTES;
  snprintf(w->app_version, sizeof w->app_version, "%s", nz(cfg->app_version));
  if (!cfg->enabled || !cfg->data_dir || !cfg->data_dir[0] ||
      snprintf(w->data_dir, sizeof w->data_dir, "%s", cfg->data_dir) >= (int)sizeof w->data_dir) {
    return w;   // disabled: every call is a no-op
  }
  ccord_warm_fingerprint(cfg->token, w->fp);
  if (snprintf(w->dir, sizeof w->dir, "%s/accounts/%s", w->data_dir, w->fp) >= (int)sizeof w->dir) return w;
  if (ccord_mkdir_p(w->dir, 0700, err) != CCORD_OK) {
    CCORD_LOG_W("warm: cannot create %s: %s; warm start is off", w->dir, err ? err->message : "?");
    return w;
  }
  w->enabled = true;
  prune_account_dirs(w);
  return w;
}

void ccord_warm_close(ccord_warm *w)
{
  ccord_free(w);
}

bool ccord_warm_enabled(const ccord_warm *w)
{
  return w && w->enabled && !w->forgotten;
}

const char *ccord_warm_dir(const ccord_warm *w)
{
  return (w && w->enabled) ? w->dir : nullptr;
}

// ------------------------------------------------------------------------------------------------ save

static ccord_status warm_save_impl(ccord_warm *w, const ccord_store *store, bool sync, uint64_t now_ms, ccord_error *err)
{
  ccord_error_clear(err);
  if (!w || !store || !w->enabled || w->forgotten) return CCORD_OK;   // disabled: nothing to do
  ccord_id self = ccord_store_self_id(store);
  if (self == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "warm: nothing to save yet");
  if (w->bound_self != CCORD_ID_NONE && w->bound_self != self) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "warm: the signed-in account changed; not saving into this cache");
  }
  if (w->max_bytes <= WARM_HEADER_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "warm: state does not fit the cache limit");
  }
  uint64_t t0 = ccord_clock_ms();
  static const char k_hdr_pad[WARM_HEADER_MAX] = {0};
  ccord_buf buf;
  ccord_buf_init(&buf, w->max_bytes);
  ccord_status st = ccord_buf_append(&buf, k_hdr_pad, WARM_HEADER_MAX);
  if (st == CCORD_OK) st = warm_build_state(w, store, false, &buf);
  if (st == CCORD_ERR_RANGE) {   // too big: drop members, overwrites and most users, and say so
    ccord_buf_free(&buf);
    ccord_buf_init(&buf, w->max_bytes);
    st = ccord_buf_append(&buf, k_hdr_pad, WARM_HEADER_MAX);
    if (st == CCORD_OK) st = warm_build_state(w, store, true, &buf);
  }
  if (st != CCORD_OK || buf.len < WARM_HEADER_MAX) {
    ccord_buf_free(&buf);
    return ccord_error_set(err, st == CCORD_OK ? CCORD_ERR_INTERNAL : st, "warm: state does not fit the cache limit");
  }
  size_t flen = 0;
  const char *file = wrap_with_header_inplace(&buf, &flen);
  if (!file) {
    ccord_buf_free(&buf);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "warm: out of memory");
  }

  char wdir[1200], hdir[1300], path[1300], meta[1300];
  if (snprintf(wdir, sizeof wdir, "%s/warm", w->dir) >= (int)sizeof wdir ||
      snprintf(hdir, sizeof hdir, "%s/warm/history", w->dir) >= (int)sizeof hdir ||
      !warm_state_path(w, path, sizeof path) || !join_path(meta, sizeof meta, w->dir, "meta.json")) {
    ccord_buf_free(&buf);
    return ccord_error_set(err, CCORD_ERR_RANGE, "warm: path too long");
  }
  st = ccord_mkdir_p(hdir, 0700, err);
  if (st == CCORD_OK) st = ccord_file_write_atomic(path, file, flen, 0600, sync, err);
  ccord_buf_free(&buf);
  if (st != CCORD_OK) return st;
  char mj[160];
  int mn = snprintf(mj, sizeof mj, "{\"user_id\":\"%" PRIu64 "\",\"saved_ms\":%" PRId64 "}", (uint64_t)self,
                    warm_now(w));
  ccord_error me;
  if (mn > 0 && (size_t)mn < sizeof mj && ccord_file_write_atomic(meta, mj, (size_t)mn, 0600, false, &me) != CCORD_OK) {
    CCORD_LOG_W("warm: meta.json write failed: %s", me.message);
  }
  w->bound_self = self;
  w->saved_warm_rev = ccord_store_warm_revision(store);
  w->saved_once = true;
  uint64_t stamp = now_ms ? now_ms : t0;
  w->last_save_mono = stamp ? stamp : 1u;
  CCORD_LOG_I("warm: saved %zu bytes in %u ms%s", flen, (unsigned)(ccord_clock_ms() - t0), sync ? " (synced)" : "");
  ccord_os_trim_memory();
  return CCORD_OK;
}

ccord_status ccord_warm_save(ccord_warm *w, const ccord_store *store, bool sync, ccord_error *err)
{
  return warm_save_impl(w, store, sync, 0, err);
}

bool ccord_warm_save_if_dirty(ccord_warm *w, const ccord_store *store, bool sync, uint64_t now_ms, ccord_error *err)
{
  ccord_error_clear(err);
  if (!w || !store || !w->enabled || w->forgotten) return false;
  if (w->saved_once && ccord_store_warm_revision(store) == w->saved_warm_rev) return false;
  return warm_save_impl(w, store, sync, now_ms, err) == CCORD_OK;
}

bool ccord_warm_maybe_save(ccord_warm *w, const ccord_store *store, const ccord_session *session, uint64_t now_ms)
{
  if (!w || !store || !session || !w->enabled || w->forgotten) return false;
  if (ccord_session_is_stale(session) || ccord_session_state(session) != CCORD_CONN_CONNECTED) {
    w->connected_since = 0;
    return false;
  }
  if (w->connected_since == 0) w->connected_since = now_ms ? now_ms : 1;
  if (now_ms < w->connected_since + CCORD_WARM_FIRST_SAVE_DELAY_MS) return false;
  if (w->saved_once && ccord_store_warm_revision(store) == w->saved_warm_rev) return false;
  if (w->saved_once && w->last_save_mono != 0 && now_ms >= w->last_save_mono &&
      now_ms - w->last_save_mono < CCORD_WARM_SAVE_INTERVAL_MS) {
    return false;
  }
  ccord_error e;
  w->last_save_mono = now_ms ? now_ms : 1;
  if (warm_save_impl(w, store, false, now_ms, &e) != CCORD_OK) {
    CCORD_LOG_W("warm: save failed: %s", e.message);
    return false;
  }
  return true;
}

ccord_status ccord_warm_put_history(ccord_warm *w, ccord_id channel_id, const char *json, size_t len, ccord_error *err)
{
  ccord_error_clear(err);
  if (!w || !w->enabled || w->forgotten) return CCORD_OK;
  if (!json || channel_id == CCORD_ID_NONE || len > CCORD_WARM_MAX_HISTORY_FILE_BYTES) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "warm: bad history page");
  }
  char hdir[1300], path[1400];
  if (snprintf(hdir, sizeof hdir, "%s/warm/history", w->dir) >= (int)sizeof hdir ||
      !warm_history_path(w, channel_id, path, sizeof path)) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "warm: path too long");
  }
  ccord_status st = ccord_mkdir_p(hdir, 0700, err);
  if (st != CCORD_OK) return st;
  return ccord_file_write_atomic(path, json, len, 0600, false, err);
}

void ccord_warm_forget(ccord_warm *w)
{
  if (!w || !w->enabled) return;
  ccord_error e;
  (void)ccord_rmtree(w->dir, &e);
  w->forgotten = true;
}

bool ccord_warm_forget_token(const char *data_dir, const char *token)
{
  if (!data_dir || !data_dir[0] || !token || !token[0]) return false;
  char fp[33];
  ccord_warm_fingerprint(token, fp);
  char dir[1300];
  if (snprintf(dir, sizeof dir, "%s/accounts/%s", data_dir, fp) >= (int)sizeof dir) return false;
  if (ccord_path_is_symlink(dir) || !ccord_path_is_dir(dir)) return false;   // absent, or a symlink: never followed
  ccord_error e;
  return ccord_rmtree(dir, &e) == CCORD_OK;
}

void ccord_warm_forget_user(const char *data_dir, ccord_id user_id)
{
  if (!data_dir || !data_dir[0] || user_id == CCORD_ID_NONE) return;
  char root[1100];
  if (snprintf(root, sizeof root, "%s/accounts", data_dir) >= (int)sizeof root) return;
  ccord_dir *d = ccord_dir_open(root);
  if (!d) return;
  char names[64][64];
  size_t n = 0;
  char ename[256];
  while (ccord_dir_next(d, ename, sizeof ename, nullptr) && n < 64) {
    if (ename[0] == '.' || strlen(ename) >= sizeof names[0]) continue;
    memcpy(names[n++], ename, strlen(ename) + 1);
  }
  ccord_dir_close(d);
  for (size_t i = 0; i < n; i++) {
    char mp[1300];
    if (snprintf(mp, sizeof mp, "%s/%s/meta.json", root, names[i]) >= (int)sizeof mp) continue;
    ccord_error e2;
    size_t len = 0;
    char *text = ccord_file_read_max(mp, 4096, &len, &e2);
    if (!text) continue;
    ccord_arena a;
    ccord_arena_init(&a, 0);
    ccord_json_limits lim = {.max_depth = 4, .max_nodes = 64, .max_string = 256};
    const ccord_json *root_j = ccord_json_parse(&a, text, len, &lim, &e2);
    size_t sl = 0;
    const char *us = root_j ? ccord_json_string(ccord_json_get(root_j, "user_id"), &sl) : nullptr;
    bool match = us && strtoull(us, nullptr, 10) == (unsigned long long)user_id;
    ccord_arena_free(&a);
    ccord_free(text);
    if (match) {
      char dir[1300];
      if (snprintf(dir, sizeof dir, "%s/%s", root, names[i]) < (int)sizeof dir) (void)ccord_rmtree(dir, &e2);
    }
  }
}

