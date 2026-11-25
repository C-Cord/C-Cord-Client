// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/net/ccord_cloud.h>
#include <ccord/net/http_fetch.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/paths.h>
#include <ccord/core/random.h>
#include <ccord/core/sha256.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CCORD_OS_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET cloud_sock_t;
#define CLOUD_INVALID_SOCK INVALID_SOCKET
#define cloud_sock_close(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int cloud_sock_t;
#define CLOUD_INVALID_SOCK (-1)
#define cloud_sock_close(s) close(s)
#endif

#define CLOUD_DEFAULT_ACCOUNT_URL "https://c-cord.org"
#define CLOUD_DEFAULT_CATALOG_URL "https://plugins.c-cord.org"
#define CLOUD_MAX_API_BYTES       (512u * 1024u)
#define CLOUD_JOB_CAP             32u
#define CLOUD_WAKE_QUEUE_CAP      16u
#define CLOUD_TOAST_CAP           8u
#define CLOUD_LOGIN_TIMEOUT_MS    (180u * 1000u)

typedef enum cloud_job_kind : uint8_t {
  CJOB_NONE = 0,
  CJOB_REFRESH_ME,
  CJOB_EXCHANGE_CODE,
  CJOB_SIGN_OUT,
  CJOB_LINK_GITHUB,
  CJOB_UNLINK_GITHUB,
  CJOB_RESCAN_CLAIMS,
  CJOB_PATCH_WAKE,
  CJOB_REVOKE_DEVICE,
  CJOB_FETCH_SOCIAL,
  CJOB_RATE_PLUGIN,
  CJOB_POST_COMMENT,
  CJOB_DELETE_COMMENT,
  CJOB_LOOPBACK_LOGIN,
} cloud_job_kind;

typedef struct cloud_job {
  cloud_job_kind kind;
  char id[65];
  char arg1[256];
  char arg2[256];
  char body[1600];
  int64_t num;
  int vote;
  bool flag;
} cloud_job;

typedef struct cloud_toast_item {
  char msg[192];
  uint8_t level;
} cloud_toast_item;

struct ccord_cloud {
  char data_dir[CCORD_PATH_MAX];
  bool has_data_dir;
  char account_base[256];
  char catalog_base[256];
  bool is_mobile;
  bool disable_network;
  ccord_cloud_cbs cbs;

  ccord_mutex *mu;
  ccord_cond *cv;
  ccord_cond *wake_cv;
  bool stop;

  // Protected by mu
  char device_token[160];
  char pkce_verifier[96];
  char pkce_state[64];
  char pkce_redirect[128];
  bool login_cancel;

  ccord_cloud_state state;
  ccord_cloud_social social;
  uint64_t pumped_state_rev;
  uint64_t pumped_social_rev;

  // Main-thread callbacks queued from worker
  char pending_open_url[768];
  cloud_toast_item toasts[CLOUD_TOAST_CAP];
  size_t toast_count;

  // Incoming wake events (mobile) & outgoing wake signal (desktop)
  ccord_wake_kind rx_wakes[CLOUD_WAKE_QUEUE_CAP];
  size_t rx_wake_count;
  ccord_wake_kind tx_wake_pending;
  bool wake_hb_now;

  // Background job queue
  cloud_job jobs[CLOUD_JOB_CAP];
  size_t job_head;
  size_t job_count;

  ccord_thread *worker;
  ccord_thread *wake_thread;
  bool threads_started;
#if !CCORD_OS_WINDOWS
  int wake_pipe[2]; // self-pipe so wake_thread sleeps full heartbeat interval with zero 500ms polling
#endif
};

// ---------------------------------------------------------------- small helpers

static void cloud_wake_main(ccord_cloud *c)
{
  if (c && c->cbs.wake_main) c->cbs.wake_main(c->cbs.ud);
}

static void cloud_signal_wake_pipe(ccord_cloud *c)
{
#if !CCORD_OS_WINDOWS
  if (!c || c->wake_pipe[1] < 0) return;
  uint8_t b = 1;
  ssize_t r;
  do {
    r = write(c->wake_pipe[1], &b, 1);
  } while (r < 0 && errno == EINTR);
#else
  (void)c;
#endif
}

static void cloud_drain_wake_pipe(ccord_cloud *c)
{
#if !CCORD_OS_WINDOWS
  if (!c || c->wake_pipe[0] < 0) return;
  uint8_t tmp[32];
  while (read(c->wake_pipe[0], tmp, sizeof tmp) > 0) {}
#else
  (void)c;
#endif
}

static void cloud_wipe(void *p, size_t n)
{
  volatile unsigned char *vp = (volatile unsigned char *)p;
  for (size_t i = 0; i < n; i++) vp[i] = 0;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  size_t n = strlen(src);
  if (n >= cap) n = cap - 1u;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static void trim_trailing_slash(char *s)
{
  if (!s) return;
  size_t n = strlen(s);
  while (n > 0 && s[n - 1u] == '/') {
    s[--n] = '\0';
  }
}

static void json_str_copy(const ccord_json *obj, const char *key, char *dst, size_t cap)
{
  if (!dst || cap == 0) return;
  dst[0] = '\0';
  if (!obj) return;
  size_t len = 0;
  const char *s = ccord_json_string(ccord_json_get(obj, key), &len);
  if (!s) return;
  if (len >= cap) len = cap - 1u;
  memcpy(dst, s, len);
  dst[len] = '\0';
}

static void base64url_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
  static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t oi = 0;
  for (size_t i = 0; i < len && oi + 4u < cap; i += 3u) {
    uint32_t v = ((uint32_t)in[i]) << 16;
    if (i + 1u < len) v |= ((uint32_t)in[i + 1u]) << 8;
    if (i + 2u < len) v |= (uint32_t)in[i + 2u];
    out[oi++] = k_b64[(v >> 18) & 63u];
    out[oi++] = k_b64[(v >> 12) & 63u];
    if (i + 1u < len) out[oi++] = k_b64[(v >> 6) & 63u];
    if (i + 2u < len) out[oi++] = k_b64[v & 63u];
  }
  if (cap > 0) out[oi < cap ? oi : cap - 1u] = '\0';
}

static void hex_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
  static const char k_hex[] = "0123456789abcdef";
  size_t oi = 0;
  for (size_t i = 0; i < len && oi + 2u < cap; i++) {
    out[oi++] = k_hex[in[i] >> 4];
    out[oi++] = k_hex[in[i] & 0x0fu];
  }
  if (cap > 0) out[oi < cap ? oi : cap - 1u] = '\0';
}

static void url_encode(const char *src, char *dst, size_t cap)
{
  static const char k_hex[] = "0123456789ABCDEF";
  size_t oi = 0;
  for (const unsigned char *p = (const unsigned char *)src; p && *p && oi + 1u < cap; p++) {
    unsigned char c = *p;
    bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      dst[oi++] = (char)c;
    } else if (oi + 3u < cap) {
      dst[oi++] = '%';
      dst[oi++] = k_hex[c >> 4];
      dst[oi++] = k_hex[c & 0x0fu];
    } else {
      break;
    }
  }
  if (cap > 0) dst[oi < cap ? oi : cap - 1u] = '\0';
}

static int hex_val(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void url_decode_span(const char *src, size_t slen, char *dst, size_t cap)
{
  size_t oi = 0;
  for (size_t i = 0; i < slen && oi + 1u < cap; i++) {
    if (src[i] == '%' && i + 2u < slen) {
      int hi = hex_val(src[i + 1u]);
      int lo = hex_val(src[i + 2u]);
      if (hi >= 0 && lo >= 0) {
        dst[oi++] = (char)((hi << 4) | lo);
        i += 2u;
        continue;
      }
    } else if (src[i] == '+') {
      dst[oi++] = ' ';
      continue;
    }
    dst[oi++] = src[i];
  }
  if (cap > 0) dst[oi < cap ? oi : cap - 1u] = '\0';
}

bool ccord_cloud_parse_query_param(const char *query, const char *key, char *out, size_t cap)
{
  if (out && cap > 0) out[0] = '\0';
  if (!query || !key || !key[0] || !out || cap == 0) return false;
  size_t klen = strlen(key);
  const char *p = query;
  if (*p == '?') p++;
  while (*p) {
    const char *amp = strchr(p, '&');
    size_t plen = amp ? (size_t)(amp - p) : strlen(p);
    const char *eq = memchr(p, '=', plen);
    if (eq && (size_t)(eq - p) == klen && memcmp(p, key, klen) == 0) {
      const char *val = eq + 1;
      size_t vlen = (size_t)((p + plen) - val);
      url_decode_span(val, vlen, out, cap);
      return true;
    }
    if (!amp) break;
    p = amp + 1;
  }
  return false;
}

bool ccord_cloud_pkce_s256(const char *verifier, char challenge_out[64])
{
  if (!verifier || !challenge_out) return false;
  size_t vlen = strlen(verifier);
  if (vlen < 43u || vlen > 128u) return false;
  unsigned char digest[32];
  ccord_sha256(verifier, vlen, digest);
  base64url_encode(digest, sizeof digest, challenge_out, 64u);
  cloud_wipe(digest, sizeof digest);
  return challenge_out[0] != '\0';
}

static ccord_rating_sentiment parse_sentiment(const char *s)
{
  if (!s) return CCORD_RATING_UNRATED;
  if (strcmp(s, "positive") == 0) return CCORD_RATING_POSITIVE;
  if (strcmp(s, "neutral") == 0) return CCORD_RATING_NEUTRAL;
  if (strcmp(s, "negative") == 0) return CCORD_RATING_NEGATIVE;
  return CCORD_RATING_UNRATED;
}

static void parse_rating_obj(const ccord_json *r, ccord_cloud_rating *out)
{
  memset(out, 0, sizeof *out);
  out->score_x10 = -1;
  out->sentiment = CCORD_RATING_UNRATED;
  if (!r || ccord_json_type_of(r) != CCORD_JSON_OBJECT) return;

  int64_t v = 0;
  if (ccord_json_int64(ccord_json_get(r, "score_x10"), &v) && v >= -1 && v <= 100) {
    out->score_x10 = (int16_t)v;
  } else {
    const ccord_json *sc = ccord_json_get(r, "score");
    if (sc && ccord_json_type_of(sc) == CCORD_JSON_NUMBER) {
      double d = ccord_json_number(sc, -1.0);
      if (d >= 0.0 && d <= 10.0) out->score_x10 = (int16_t)(d * 10.0 + 0.5);
    }
  }
  char sent[24];
  json_str_copy(r, "sentiment", sent, sizeof sent);
  out->sentiment = parse_sentiment(sent);

  if (ccord_json_int64(ccord_json_get(r, "pos"), &v) && v > 0) out->pos = (uint32_t)v;
  if (ccord_json_int64(ccord_json_get(r, "neu"), &v) && v > 0) out->neu = (uint32_t)v;
  if (ccord_json_int64(ccord_json_get(r, "neg"), &v) && v > 0) out->neg = (uint32_t)v;
  if (ccord_json_int64(ccord_json_get(r, "total"), &v) && v > 0) out->total = (uint32_t)v;

  const ccord_json *mv = ccord_json_get(r, "my_vote");
  if (mv && ccord_json_int64(mv, &v) && v >= -1 && v <= 1) {
    out->has_my_vote = true;
    out->my_vote = (int8_t)v;
  }
}

bool ccord_cloud_parse_social_json(const char *json, size_t len, ccord_cloud_social *out)
{
  if (!json || len == 0 || !out) return false;
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error pe = {0};
  const ccord_json_limits lim = {12, 8192, CLOUD_MAX_API_BYTES};
  const ccord_json *root = ccord_json_parse(&arena, json, len, &lim, &pe);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return false;
  }

  uint64_t prev_rev = out->revision;
  memset(out, 0, sizeof *out);
  out->revision = prev_rev;
  out->rating.score_x10 = -1;

  json_str_copy(root, "plugin_id", out->plugin_id, sizeof out->plugin_id);
  parse_rating_obj(ccord_json_get(root, "rating"), &out->rating);

  const ccord_json *devs = ccord_json_get(root, "claimed_developers");
  if (devs && ccord_json_type_of(devs) == CCORD_JSON_ARRAY && ccord_json_array_len(devs) > 0) {
    json_str_copy(ccord_json_array_at(devs, 0), "github_login", out->claimed_dev, sizeof out->claimed_dev);
  }

  const ccord_json *cpage = ccord_json_get(root, "comments");
  if (cpage && ccord_json_type_of(cpage) == CCORD_JSON_OBJECT) {
    int64_t tot = 0, next_cur = 0;
    if (ccord_json_int64(ccord_json_get(cpage, "total"), &tot) && tot > 0) out->total_comments = (uint32_t)tot;
    if (ccord_json_int64(ccord_json_get(cpage, "next_cursor"), &next_cur) && next_cur > 0) {
      out->next_comment_cursor = next_cur;
    }
    out->has_more_comments = ccord_json_bool(ccord_json_get(cpage, "has_more"), false);

    const ccord_json *arr = ccord_json_get(cpage, "comments");
    if (arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY) {
      size_t n = ccord_json_array_len(arr);
      if (n > CCORD_CLOUD_MAX_COMMENTS) n = CCORD_CLOUD_MAX_COMMENTS;
      for (size_t i = 0; i < n; i++) {
        const ccord_json *co = ccord_json_array_at(arr, i);
        if (!co || ccord_json_type_of(co) != CCORD_JSON_OBJECT) continue;
        ccord_cloud_comment *dst = &out->comments[out->comment_count];
        memset(dst, 0, sizeof *dst);
        (void)ccord_json_int64(ccord_json_get(co, "id"), &dst->id);
        json_str_copy(co, "author_username", dst->author_username, sizeof dst->author_username);
        json_str_copy(co, "author_display_name", dst->author_display, sizeof dst->author_display);
        if (!dst->author_display[0]) copy_str(dst->author_display, sizeof dst->author_display, dst->author_username);
        json_str_copy(co, "plugin_version", dst->plugin_version, sizeof dst->plugin_version);
        char dt[32];
        json_str_copy(co, "created_at", dt, sizeof dt);
        if (strlen(dt) >= 10) memcpy(dst->created_date, dt, 10), dst->created_date[10] = '\0';
        json_str_copy(co, "body", dst->body, sizeof dst->body);
        dst->is_developer = ccord_json_bool(ccord_json_get(co, "is_developer"), false);
        dst->is_mine = ccord_json_bool(ccord_json_get(co, "is_mine"), false);
        dst->is_deleted = ccord_json_bool(ccord_json_get(co, "is_deleted"), false);

        const ccord_json *reps = ccord_json_get(co, "replies");
        if (reps && ccord_json_type_of(reps) == CCORD_JSON_ARRAY) {
          size_t rn = ccord_json_array_len(reps);
          if (rn > CCORD_CLOUD_MAX_REPLIES) rn = CCORD_CLOUD_MAX_REPLIES;
          for (size_t k = 0; k < rn; k++) {
            const ccord_json *ro = ccord_json_array_at(reps, k);
            if (!ro || ccord_json_type_of(ro) != CCORD_JSON_OBJECT) continue;
            ccord_cloud_reply *rd = &dst->replies[dst->reply_count];
            memset(rd, 0, sizeof *rd);
            (void)ccord_json_int64(ccord_json_get(ro, "id"), &rd->id);
            json_str_copy(ro, "author_username", rd->author_username, sizeof rd->author_username);
            json_str_copy(ro, "author_display_name", rd->author_display, sizeof rd->author_display);
            if (!rd->author_display[0]) copy_str(rd->author_display, sizeof rd->author_display, rd->author_username);
            json_str_copy(ro, "plugin_version", rd->plugin_version, sizeof rd->plugin_version);
            json_str_copy(ro, "created_at", dt, sizeof dt);
            if (strlen(dt) >= 10) memcpy(rd->created_date, dt, 10), rd->created_date[10] = '\0';
            json_str_copy(ro, "body", rd->body, sizeof rd->body);
            rd->is_developer = ccord_json_bool(ccord_json_get(ro, "is_developer"), false);
            rd->is_mine = ccord_json_bool(ccord_json_get(ro, "is_mine"), false);
            rd->is_deleted = ccord_json_bool(ccord_json_get(ro, "is_deleted"), false);
            dst->reply_count++;
          }
        }
        out->comment_count++;
      }
    }
  }
  out->loaded = true;
  ccord_arena_free(&arena);
  return true;
}

bool ccord_cloud_parse_me_json(const char *json, size_t len, ccord_cloud_state *out)
{
  if (!json || len == 0 || !out) return false;
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error pe = {0};
  const ccord_json_limits lim = {12, 8192, CLOUD_MAX_API_BYTES};
  const ccord_json *root = ccord_json_parse(&arena, json, len, &lim, &pe);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return false;
  }

  out->discord_oauth_ready = ccord_json_bool(ccord_json_get(root, "discord_oauth_ready"), true);
  out->github_link_ready = ccord_json_bool(ccord_json_get(root, "github_link_ready"), true);
  bool authed = ccord_json_bool(ccord_json_get(root, "authenticated"), false);
  out->authenticated = authed;
  if (!authed) {
    out->claimed_count = 0;
    out->device_count = 0;
    out->companion_online = false;
    ccord_arena_free(&arena);
    return true;
  }

  out->companion_online = ccord_json_bool(ccord_json_get(root, "companion_online"), out->companion_online);
  char cur_dev[48];
  json_str_copy(root, "current_device_id", cur_dev, sizeof cur_dev);
  if (cur_dev[0]) copy_str(out->device_id, sizeof out->device_id, cur_dev);

  const ccord_json *u = ccord_json_get(root, "user");
  if (u && ccord_json_type_of(u) == CCORD_JSON_OBJECT) {
    json_str_copy(u, "id", out->user_id, sizeof out->user_id);
    json_str_copy(u, "discord_id", out->discord_id, sizeof out->discord_id);
    json_str_copy(u, "username", out->username, sizeof out->username);
    json_str_copy(u, "display_name", out->display_name, sizeof out->display_name);
    if (!out->display_name[0]) copy_str(out->display_name, sizeof out->display_name, out->username);
    json_str_copy(u, "avatar_url", out->avatar_url, sizeof out->avatar_url);
    json_str_copy(u, "github_login", out->github_login, sizeof out->github_login);
  }

  out->claimed_count = 0;
  const ccord_json *cl = ccord_json_get(root, "claimed_plugins");
  if (cl && ccord_json_type_of(cl) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(cl);
    if (n > CCORD_CLOUD_MAX_CLAIMED) n = CCORD_CLOUD_MAX_CLAIMED;
    for (size_t i = 0; i < n; i++) {
      const ccord_json *po = ccord_json_array_at(cl, i);
      if (!po || ccord_json_type_of(po) != CCORD_JSON_OBJECT) continue;
      ccord_cloud_claimed_plugin *dst = &out->claimed[out->claimed_count];
      memset(dst, 0, sizeof *dst);
      dst->rating_score_x10 = -1;
      json_str_copy(po, "id", dst->id, sizeof dst->id);
      json_str_copy(po, "name", dst->name, sizeof dst->name);
      json_str_copy(po, "version", dst->version, sizeof dst->version);
      int64_t dls = 0;
      if (ccord_json_int64(ccord_json_get(po, "downloads"), &dls) && dls > 0) dst->downloads = (uint64_t)dls;
      ccord_cloud_rating rt;
      parse_rating_obj(ccord_json_get(po, "rating"), &rt);
      dst->rating_score_x10 = rt.score_x10;
      dst->rating_sentiment = rt.sentiment;
      if (dst->id[0]) out->claimed_count++;
    }
  }

  out->device_count = 0;
  const ccord_json *devs = ccord_json_get(root, "devices");
  if (devs && ccord_json_type_of(devs) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(devs);
    if (n > CCORD_CLOUD_MAX_DEVICES) n = CCORD_CLOUD_MAX_DEVICES;
    for (size_t i = 0; i < n; i++) {
      const ccord_json *d = ccord_json_array_at(devs, i);
      if (!d || ccord_json_type_of(d) != CCORD_JSON_OBJECT) continue;
      ccord_cloud_device_info *dst = &out->devices[out->device_count];
      memset(dst, 0, sizeof *dst);
      json_str_copy(d, "id", dst->id, sizeof dst->id);
      json_str_copy(d, "name", dst->name, sizeof dst->name);
      json_str_copy(d, "platform", dst->platform, sizeof dst->platform);
      json_str_copy(d, "role", dst->role, sizeof dst->role);
      dst->wake_enabled = ccord_json_bool(ccord_json_get(d, "wake_enabled"), true);
      dst->online = ccord_json_bool(ccord_json_get(d, "online"), false);
      dst->is_current = out->device_id[0] && strcmp(dst->id, out->device_id) == 0;
      if (dst->is_current) {
        out->wake_enabled = dst->wake_enabled;
        copy_str(out->device_name, sizeof out->device_name, dst->name);
        copy_str(out->device_role, sizeof out->device_role, dst->role);
      }
      if (dst->id[0]) out->device_count++;
    }
  }

  ccord_arena_free(&arena);
  return true;
}

// ---------------------------------------------------------------- persistence (0600 atomic JSON)

static bool cloud_account_path(const ccord_cloud *c, char *out, size_t cap)
{
  if (!c || !c->has_data_dir || !c->data_dir[0]) return false;
  ccord_error err = {0};
  return ccord_paths_join(out, cap, c->data_dir, "ccord-account.json", &err) == CCORD_OK;
}

static void cloud_save_disk_locked(const ccord_cloud *c)
{
  char path[CCORD_PATH_MAX];
  if (!cloud_account_path(c, path, sizeof path)) return;
  if (!c->device_token[0]) {
    (void)ccord_file_unlink(path);
    return;
  }
  ccord_error err = {0};
  (void)ccord_mkdir_p(c->data_dir, 0700, &err);
  ccord_buf b;
  ccord_buf_init(&b, 4096);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "schema");       ccord_jw_int(&w, 1);
  ccord_jw_key(&w, "token");        ccord_jw_cstr(&w, c->device_token);
  ccord_jw_key(&w, "user_id");      ccord_jw_cstr(&w, c->state.user_id);
  ccord_jw_key(&w, "discord_id");   ccord_jw_cstr(&w, c->state.discord_id);
  ccord_jw_key(&w, "username");     ccord_jw_cstr(&w, c->state.username);
  ccord_jw_key(&w, "display_name"); ccord_jw_cstr(&w, c->state.display_name);
  ccord_jw_key(&w, "avatar_url");   ccord_jw_cstr(&w, c->state.avatar_url);
  ccord_jw_key(&w, "github_login"); ccord_jw_cstr(&w, c->state.github_login);
  ccord_jw_key(&w, "device_id");    ccord_jw_cstr(&w, c->state.device_id);
  ccord_jw_key(&w, "device_name");  ccord_jw_cstr(&w, c->state.device_name);
  ccord_jw_key(&w, "device_role");  ccord_jw_cstr(&w, c->state.device_role);
  ccord_jw_key(&w, "wake_enabled"); ccord_jw_bool(&w, c->state.wake_enabled);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) {
    (void)ccord_file_write_atomic(path, b.data, b.len, 0600, true, &err);
  }
  cloud_wipe(b.data, b.cap);
  ccord_buf_free(&b);
}

static void cloud_load_disk_locked(ccord_cloud *c)
{
  char path[CCORD_PATH_MAX];
  if (!cloud_account_path(c, path, sizeof path)) return;
  size_t len = 0;
  ccord_error err = {0};
  char *raw = ccord_file_read_max(path, 32768, &len, &err);
  if (!raw || len == 0) {
    ccord_free(raw);
    return;
  }
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  const ccord_json_limits lim = {8, 256, 32768};
  const ccord_json *root = ccord_json_parse(&arena, raw, len, &lim, &err);
  if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    json_str_copy(root, "token", c->device_token, sizeof c->device_token);
    if (c->device_token[0]) {
      c->state.authenticated = true;
      json_str_copy(root, "user_id", c->state.user_id, sizeof c->state.user_id);
      json_str_copy(root, "discord_id", c->state.discord_id, sizeof c->state.discord_id);
      json_str_copy(root, "username", c->state.username, sizeof c->state.username);
      json_str_copy(root, "display_name", c->state.display_name, sizeof c->state.display_name);
      json_str_copy(root, "avatar_url", c->state.avatar_url, sizeof c->state.avatar_url);
      json_str_copy(root, "github_login", c->state.github_login, sizeof c->state.github_login);
      json_str_copy(root, "device_id", c->state.device_id, sizeof c->state.device_id);
      json_str_copy(root, "device_name", c->state.device_name, sizeof c->state.device_name);
      json_str_copy(root, "device_role", c->state.device_role, sizeof c->state.device_role);
      c->state.wake_enabled = ccord_json_bool(ccord_json_get(root, "wake_enabled"), true);
      c->state.revision++;
    }
  }
  ccord_arena_free(&arena);
  cloud_wipe(raw, len);
  ccord_free(raw);
}

// ---------------------------------------------------------------- internal queue & HTTP helpers

static void cloud_push_toast_locked(ccord_cloud *c, const char *msg, uint8_t level)
{
  if (!c || !msg || !msg[0]) return;
  if (c->toast_count >= CLOUD_TOAST_CAP) {
    memmove(&c->toasts[0], &c->toasts[1], (CLOUD_TOAST_CAP - 1u) * sizeof c->toasts[0]);
    c->toast_count = CLOUD_TOAST_CAP - 1u;
  }
  copy_str(c->toasts[c->toast_count].msg, sizeof c->toasts[0].msg, msg);
  c->toasts[c->toast_count].level = level;
  c->toast_count++;
}

static void cloud_worker_fn(void *arg);
static void cloud_wake_fn(void *arg);

static void cloud_ensure_threads_locked(ccord_cloud *c)
{
  if (c->threads_started || c->disable_network || c->stop) return;
  c->worker = ccord_thread_create(cloud_worker_fn, c);
  c->wake_thread = ccord_thread_create(cloud_wake_fn, c);
  c->threads_started = (c->worker != nullptr || c->wake_thread != nullptr);
}

static bool cloud_enqueue_locked(ccord_cloud *c, const cloud_job *j)
{
  if (!c || c->disable_network || c->stop) return false;
  cloud_ensure_threads_locked(c);
  if (c->job_count >= CLOUD_JOB_CAP) return false;
  size_t idx = (c->job_head + c->job_count) % CLOUD_JOB_CAP;
  c->jobs[idx] = *j;
  c->job_count++;
  ccord_cond_signal(c->cv);
  return true;
}

static void build_auth_headers_json(const char *bearer, bool has_json_body, char *out, size_t cap)
{
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "Accept");
  ccord_jw_cstr(&w, "application/json");
  if (has_json_body) {
    ccord_jw_key(&w, "Content-Type");
    ccord_jw_cstr(&w, "application/json");
  }
  if (bearer && bearer[0]) {
    char authv[200];
    snprintf(authv, sizeof authv, "Bearer %s", bearer);
    ccord_jw_key(&w, "Authorization");
    ccord_jw_cstr(&w, authv);
    cloud_wipe(authv, sizeof authv);
  }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) copy_str(out, cap, ccord_buf_cstr(&b));
  else if (cap > 0) out[0] = '\0';
  cloud_wipe(b.data, b.cap);
  ccord_buf_free(&b);
}

static void extract_api_error(const ccord_http_response *hr, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (hr->body && hr->body_len > 0) {
    ccord_arena arena;
    ccord_arena_init(&arena, 0);
    ccord_error pe = {0};
    const ccord_json_limits lim = {8, 256, 32768};
    const ccord_json *root = ccord_json_parse(&arena, (const char *)hr->body, hr->body_len, &lim, &pe);
    if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
      json_str_copy(root, "error", out, cap);
    }
    ccord_arena_free(&arena);
  }
  if (!out[0]) {
    if (hr->error[0]) copy_str(out, cap, hr->error);
    else snprintf(out, cap, "HTTP %ld", hr->status_code);
  }
}

static bool cloud_http_call(const char *url, const char *method, const char *bearer,
                            const char *json_body, size_t body_len, ccord_http_response *out)
{
  char hdrs[384];
  build_auth_headers_json(bearer, json_body != nullptr && body_len > 0, hdrs, sizeof hdrs);
  ccord_http_request_opts opts = {
    .url = url,
    .method = method,
    .headers_json = hdrs,
    .body = (const uint8_t *)json_body,
    .body_len = body_len,
    .timeout_ms = 15000,
    .max_bytes = CLOUD_MAX_API_BYTES,
    .allow_localhost_http = false,
    .follow_redirects = false,
  };
  bool ok = ccord_http_request(&opts, out);
  cloud_wipe(hdrs, sizeof hdrs);
  return ok;
}

static void default_device_name(bool is_mobile, char *out, size_t cap)
{
  char host[64] = "";
  if (!is_mobile && ccord_os_hostname(host, sizeof host) && host[0]) {
    snprintf(out, cap, "C-Cord (%s)", host);
  } else {
    snprintf(out, cap, is_mobile ? "C-Cord Android" : "C-Cord Desktop (%s)", ccord_os_name());
  }
}

// ---------------------------------------------------------------- desktop RFC 8252 loopback OAuth

static void run_desktop_loopback_login(ccord_cloud *c)
{
  char verifier[96], challenge[64], state_hex[64];
  uint8_t rnd_v[32], rnd_s[16];
  if (ccord_random_bytes(rnd_v, sizeof rnd_v) != CCORD_OK ||
      ccord_random_bytes(rnd_s, sizeof rnd_s) != CCORD_OK) {
    ccord_mutex_lock(c->mu);
    c->state.signing_in = false;
    copy_str(c->state.error_text, sizeof c->state.error_text, "OS CSPRNG unavailable");
    c->state.revision++;
    ccord_mutex_unlock(c->mu);
    return;
  }
  base64url_encode(rnd_v, sizeof rnd_v, verifier, sizeof verifier);
  hex_encode(rnd_s, sizeof rnd_s, state_hex, sizeof state_hex);
  cloud_wipe(rnd_v, sizeof rnd_v);
  cloud_wipe(rnd_s, sizeof rnd_s);
  if (!ccord_cloud_pkce_s256(verifier, challenge)) {
    cloud_wipe(verifier, sizeof verifier);
    return;
  }

  cloud_sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls == CLOUD_INVALID_SOCK) {
    cloud_wipe(verifier, sizeof verifier);
    ccord_mutex_lock(c->mu);
    c->state.signing_in = false;
    copy_str(c->state.error_text, sizeof c->state.error_text, "Couldn't open local OAuth callback port");
    c->state.revision++;
    ccord_mutex_unlock(c->mu);
    return;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0; // ephemeral port assigned by OS
  socklen_t alen = (socklen_t)sizeof addr;
  if (bind(ls, (struct sockaddr *)&addr, alen) != 0 ||
      getsockname(ls, (struct sockaddr *)&addr, &alen) != 0 ||
      listen(ls, 2) != 0) {
    cloud_sock_close(ls);
    cloud_wipe(verifier, sizeof verifier);
    ccord_mutex_lock(c->mu);
    c->state.signing_in = false;
    copy_str(c->state.error_text, sizeof c->state.error_text, "Couldn't bind loopback OAuth callback");
    c->state.revision++;
    ccord_mutex_unlock(c->mu);
    return;
  }

  uint16_t port = ntohs(addr.sin_port);
  char redirect_uri[96], enc_ru[192], auth_url[768];
  snprintf(redirect_uri, sizeof redirect_uri, "http://127.0.0.1:%u/callback", (unsigned)port);
  url_encode(redirect_uri, enc_ru, sizeof enc_ru);
  snprintf(auth_url, sizeof auth_url,
           "%s/auth/discord/login?mode=native&redirect_uri=%s&code_challenge=%s&code_challenge_method=S256&state=%s",
           c->account_base, enc_ru, challenge, state_hex);

  ccord_mutex_lock(c->mu);
  copy_str(c->pkce_verifier, sizeof c->pkce_verifier, verifier);
  copy_str(c->pkce_state, sizeof c->pkce_state, state_hex);
  copy_str(c->pkce_redirect, sizeof c->pkce_redirect, redirect_uri);
  copy_str(c->pending_open_url, sizeof c->pending_open_url, auth_url);
  copy_str(c->state.status_text, sizeof c->state.status_text, "Complete Discord sign-in in your browser...");
  c->state.error_text[0] = '\0';
  c->state.revision++;
  ccord_mutex_unlock(c->mu);
  cloud_wake_main(c);

  // Wait up to 180s for a single loopback callback connection, waking every 250ms to check cancel/stop.
  uint64_t deadline = ccord_clock_ms() + CLOUD_LOGIN_TIMEOUT_MS;
  char got_code[256] = "";
  char got_state[128] = "";
  bool got_cb = false;
  while (ccord_clock_ms() < deadline) {
    ccord_mutex_lock(c->mu);
    bool aborted = c->stop || c->login_cancel;
    ccord_mutex_unlock(c->mu);
    if (aborted) break;

#if CCORD_OS_WINDOWS
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(ls, &rfds);
    struct timeval tv = {.tv_sec = 0, .tv_usec = 250000};
    int pr = select(0, &rfds, nullptr, nullptr, &tv);
#else
    struct pollfd pfd = {.fd = ls, .events = POLLIN};
    int pr = poll(&pfd, 1, 250);
#endif
    if (pr <= 0) continue;

    cloud_sock_t cs = accept(ls, nullptr, nullptr);
    if (cs == CLOUD_INVALID_SOCK) continue;

    char req[2048];
    int rn = (int)recv(cs, req, sizeof req - 1u, 0);
    bool valid_req = false;
    if (rn > 0) {
      req[rn] = '\0';
      if (strncmp(req, "GET /callback?", 14) == 0) {
        char *sp = strchr(req + 14, ' ');
        if (sp) *sp = '\0';
        (void)ccord_cloud_parse_query_param(req + 14, "code", got_code, sizeof got_code);
        (void)ccord_cloud_parse_query_param(req + 14, "state", got_state, sizeof got_state);
        valid_req = got_code[0] != '\0';
      }
    }
    static const char k_ok_html[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
      "<!doctype html><body style=\"font-family:sans-serif;background:#141417;color:#ededef;padding:40px;text-align:center;\">"
      "<h2>Signed in to C-Cord!</h2><p>You can close this tab and return to C-Cord.</p></body>";
    static const char k_err_html[] =
      "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nInvalid callback.\n";
    const char *resp = valid_req ? k_ok_html : k_err_html;
#if CCORD_OS_WINDOWS
    (void)send(cs, resp, (int)strlen(resp), 0);
#else
    (void)send(cs, resp, strlen(resp), 0);
#endif
    cloud_sock_close(cs);
    if (valid_req) {
      got_cb = true;
      break;
    }
  }
  cloud_sock_close(ls);

  if (!got_cb) {
    cloud_wipe(verifier, sizeof verifier);
    ccord_mutex_lock(c->mu);
    c->state.signing_in = false;
    if (c->login_cancel) {
      copy_str(c->state.status_text, sizeof c->state.status_text, "Sign-in cancelled.");
    } else if (!c->stop) {
      copy_str(c->state.error_text, sizeof c->state.error_text, "Sign-in timed out. Please try again.");
    }
    cloud_wipe(c->pkce_verifier, sizeof c->pkce_verifier);
    c->pkce_state[0] = '\0';
    c->state.revision++;
    ccord_mutex_unlock(c->mu);
    return;
  }

  if (strcmp(got_state, state_hex) != 0) {
    cloud_wipe(verifier, sizeof verifier);
    ccord_mutex_lock(c->mu);
    c->state.signing_in = false;
    copy_str(c->state.error_text, sizeof c->state.error_text, "OAuth state mismatch (CSRF check failed)");
    cloud_wipe(c->pkce_verifier, sizeof c->pkce_verifier);
    c->state.revision++;
    ccord_mutex_unlock(c->mu);
    return;
  }

  // Exchange the one-time code right on this worker turn.
  cloud_job ex = {.kind = CJOB_EXCHANGE_CODE};
  copy_str(ex.arg1, sizeof ex.arg1, got_code);
  copy_str(ex.arg2, sizeof ex.arg2, redirect_uri);
  copy_str(ex.body, sizeof ex.body, verifier);
  cloud_wipe(verifier, sizeof verifier);
  ccord_mutex_lock(c->mu);
  cloud_wipe(c->pkce_verifier, sizeof c->pkce_verifier);
  c->pkce_state[0] = '\0';
  (void)cloud_enqueue_locked(c, &ex);
  ccord_mutex_unlock(c->mu);
}

// ---------------------------------------------------------------- job handlers

static void exec_refresh_me(ccord_cloud *c)
{
  char tok[160];
  ccord_mutex_lock(c->mu);
  copy_str(tok, sizeof tok, c->device_token);
  ccord_mutex_unlock(c->mu);

  char url[320];
  snprintf(url, sizeof url, "%s/api/v2/account/me", c->account_base);
  ccord_http_response hr = {0};
  bool ok = cloud_http_call(url, "GET", tok, nullptr, 0, &hr);
  cloud_wipe(tok, sizeof tok);

  ccord_mutex_lock(c->mu);
  c->state.busy = false;
  if (ok && hr.status_code == 200 && hr.body && hr.body_len > 0) {
    bool was_authed = c->state.authenticated;
    (void)ccord_cloud_parse_me_json((const char *)hr.body, hr.body_len, &c->state);
    if (was_authed && !c->state.authenticated) {
      // Device token was revoked remotely -> clear persisted credentials.
      cloud_wipe(c->device_token, sizeof c->device_token);
      cloud_save_disk_locked(c);
      copy_str(c->state.status_text, sizeof c->state.status_text, "Session expired — sign in again.");
      ccord_cond_signal(c->wake_cv);
    } else if (c->state.authenticated) {
      cloud_save_disk_locked(c);
      c->state.error_text[0] = '\0';
    }
    c->state.revision++;
  } else {
    extract_api_error(&hr, c->state.error_text, sizeof c->state.error_text);
    c->state.revision++;
  }
  ccord_mutex_unlock(c->mu);
  ccord_http_response_free(&hr);
}

static void exec_exchange_code(ccord_cloud *c, const cloud_job *j)
{
  char dev_name[65];
  default_device_name(c->is_mobile, dev_name, sizeof dev_name);

  ccord_buf b;
  ccord_buf_init(&b, 1024);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "code");          ccord_jw_cstr(&w, j->arg1);
  ccord_jw_key(&w, "code_verifier"); ccord_jw_cstr(&w, j->body);
  ccord_jw_key(&w, "redirect_uri");  ccord_jw_cstr(&w, j->arg2);
  ccord_jw_key(&w, "device_name");   ccord_jw_cstr(&w, dev_name);
  ccord_jw_key(&w, "platform");      ccord_jw_cstr(&w, c->is_mobile ? "android" : "desktop");
  ccord_jw_key(&w, "role");          ccord_jw_cstr(&w, c->is_mobile ? "mobile" : "companion");
  ccord_jw_end_object(&w);

  char url[320];
  snprintf(url, sizeof url, "%s/api/v2/auth/exchange", c->account_base);
  ccord_http_response hr = {0};
  bool ok = ccord_jw_ok(&w) && cloud_http_call(url, "POST", nullptr, ccord_buf_cstr(&b), b.len, &hr);
  cloud_wipe(b.data, b.cap);
  ccord_buf_free(&b);

  bool exchanged = false;
  if (ok && hr.status_code == 200 && hr.body && hr.body_len > 0) {
    ccord_arena arena;
    ccord_arena_init(&arena, 0);
    ccord_error pe = {0};
    const ccord_json_limits lim = {12, 1024, 65536};
    const ccord_json *root = ccord_json_parse(&arena, (const char *)hr.body, hr.body_len, &lim, &pe);
    if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
      char new_tok[160];
      json_str_copy(root, "token", new_tok, sizeof new_tok);
      if (new_tok[0]) {
        ccord_mutex_lock(c->mu);
        copy_str(c->device_token, sizeof c->device_token, new_tok);
        c->state.authenticated = true;
        c->state.signing_in = false;
        c->state.busy = false;
        c->state.wake_enabled = true;
        c->state.error_text[0] = '\0';
        const ccord_json *u = ccord_json_get(root, "user");
        if (u && ccord_json_type_of(u) == CCORD_JSON_OBJECT) {
          json_str_copy(u, "id", c->state.user_id, sizeof c->state.user_id);
          json_str_copy(u, "discord_id", c->state.discord_id, sizeof c->state.discord_id);
          json_str_copy(u, "username", c->state.username, sizeof c->state.username);
          json_str_copy(u, "display_name", c->state.display_name, sizeof c->state.display_name);
          json_str_copy(u, "avatar_url", c->state.avatar_url, sizeof c->state.avatar_url);
          json_str_copy(u, "github_login", c->state.github_login, sizeof c->state.github_login);
        }
        const ccord_json *dev = ccord_json_get(root, "device");
        if (dev && ccord_json_type_of(dev) == CCORD_JSON_OBJECT) {
          json_str_copy(dev, "id", c->state.device_id, sizeof c->state.device_id);
          json_str_copy(dev, "name", c->state.device_name, sizeof c->state.device_name);
          json_str_copy(dev, "role", c->state.device_role, sizeof c->state.device_role);
          c->state.wake_enabled = ccord_json_bool(ccord_json_get(dev, "wake_enabled"), true);
        }
        copy_str(c->state.status_text, sizeof c->state.status_text, "Signed in to c-cord.org.");
        cloud_save_disk_locked(c);
        cloud_push_toast_locked(c, "Signed in to c-cord.org!", 1);
        c->state.revision++;
        ccord_cond_signal(c->wake_cv);
        ccord_mutex_unlock(c->mu);
        exchanged = true;
      }
      cloud_wipe(new_tok, sizeof new_tok);
    }
    ccord_arena_free(&arena);
  }
  if (!exchanged) {
    ccord_mutex_lock(c->mu);
    c->state.signing_in = false;
    c->state.busy = false;
    extract_api_error(&hr, c->state.error_text, sizeof c->state.error_text);
    c->state.revision++;
    ccord_mutex_unlock(c->mu);
  }
  ccord_http_response_free(&hr);
  if (exchanged) exec_refresh_me(c);
}

static void exec_fetch_social(ccord_cloud *c, const char *plugin_id)
{
  char tok[160];
  ccord_mutex_lock(c->mu);
  copy_str(tok, sizeof tok, c->device_token);
  ccord_mutex_unlock(c->mu);

  char enc_id[192], url[512];
  url_encode(plugin_id, enc_id, sizeof enc_id);
  snprintf(url, sizeof url, "%s/api/v2/plugins/%s/social", c->catalog_base, enc_id);
  ccord_http_response hr = {0};
  bool ok = cloud_http_call(url, "GET", tok, nullptr, 0, &hr);
  cloud_wipe(tok, sizeof tok);

  ccord_mutex_lock(c->mu);
  if (strcmp(c->social.plugin_id, plugin_id) == 0) {
    c->social.loading = false;
    if (ok && hr.status_code == 200 && hr.body && hr.body_len > 0) {
      if (ccord_cloud_parse_social_json((const char *)hr.body, hr.body_len, &c->social)) {
        copy_str(c->social.plugin_id, sizeof c->social.plugin_id, plugin_id);
        c->social.not_found = false;
        c->social.error_text[0] = '\0';
      } else {
        copy_str(c->social.error_text, sizeof c->social.error_text, "Malformed social response");
      }
    } else if (ok && hr.status_code == 404) {
      c->social.loaded = false;
      c->social.not_found = true;
      c->social.error_text[0] = '\0';
    } else {
      extract_api_error(&hr, c->social.error_text, sizeof c->social.error_text);
    }
    c->social.revision++;
  }
  ccord_mutex_unlock(c->mu);
  ccord_http_response_free(&hr);
}

static void cloud_worker_fn(void *arg)
{
  ccord_cloud *c = arg;
  for (;;) {
    ccord_mutex_lock(c->mu);
    while (!c->stop && c->job_count == 0) {
      ccord_cond_wait(c->cv, c->mu);
    }
    if (c->stop) {
      ccord_mutex_unlock(c->mu);
      break;
    }
    cloud_job j = c->jobs[c->job_head];
    c->job_head = (c->job_head + 1u) % CLOUD_JOB_CAP;
    c->job_count--;
    ccord_mutex_unlock(c->mu);

    char tok[160] = "";
    ccord_mutex_lock(c->mu);
    copy_str(tok, sizeof tok, c->device_token);
    ccord_mutex_unlock(c->mu);

    switch (j.kind) {
      case CJOB_LOOPBACK_LOGIN:
        run_desktop_loopback_login(c);
        break;
      case CJOB_EXCHANGE_CODE:
        exec_exchange_code(c, &j);
        break;
      case CJOB_REFRESH_ME:
        exec_refresh_me(c);
        break;
      case CJOB_SIGN_OUT: {
        if (tok[0]) {
          char url[320];
          snprintf(url, sizeof url, "%s/auth/user/logout", c->account_base);
          ccord_http_response hr = {0};
          (void)cloud_http_call(url, "POST", tok, "{}", 2, &hr);
          ccord_http_response_free(&hr);
        }
        ccord_mutex_lock(c->mu);
        cloud_wipe(c->device_token, sizeof c->device_token);
        uint64_t rev = c->state.revision + 1u;
        bool dr = c->state.discord_oauth_ready, gr = c->state.github_link_ready;
        memset(&c->state, 0, sizeof c->state);
        c->state.discord_oauth_ready = dr;
        c->state.github_link_ready = gr;
        c->state.revision = rev;
        copy_str(c->state.status_text, sizeof c->state.status_text, "Signed out of c-cord.org.");
        cloud_save_disk_locked(c);
        cloud_push_toast_locked(c, "Signed out of c-cord.org", 0);
        ccord_cond_signal(c->wake_cv);
        ccord_mutex_unlock(c->mu);
        break;
      }
      case CJOB_LINK_GITHUB: {
        char url[320];
        snprintf(url, sizeof url, "%s/api/v2/account/github/link-url", c->account_base);
        const char *payload = c->is_mobile ? "{\"return_uri\":\"ccord://account/github-linked\"}" : "{}";
        ccord_http_response hr = {0};
        bool ok = cloud_http_call(url, "POST", tok, payload, strlen(payload), &hr);
        ccord_mutex_lock(c->mu);
        c->state.busy = false;
        if (ok && hr.status_code == 200 && hr.body && hr.body_len > 0) {
          ccord_arena arena;
          ccord_arena_init(&arena, 0);
          ccord_error pe = {0};
          const ccord_json_limits lim = {8, 128, 16384};
          const ccord_json *root = ccord_json_parse(&arena, (const char *)hr.body, hr.body_len, &lim, &pe);
          char handoff[512] = "";
          if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
            json_str_copy(root, "url", handoff, sizeof handoff);
          }
          ccord_arena_free(&arena);
          if (handoff[0]) {
            copy_str(c->pending_open_url, sizeof c->pending_open_url, handoff);
            copy_str(c->state.status_text, sizeof c->state.status_text, "Opened GitHub linking in your browser.");
            c->state.error_text[0] = '\0';
          } else {
            copy_str(c->state.error_text, sizeof c->state.error_text, "No handoff URL returned");
          }
        } else {
          extract_api_error(&hr, c->state.error_text, sizeof c->state.error_text);
        }
        c->state.revision++;
        ccord_mutex_unlock(c->mu);
        ccord_http_response_free(&hr);
        break;
      }
      case CJOB_UNLINK_GITHUB: {
        char url[320];
        snprintf(url, sizeof url, "%s/api/v2/account/github/unlink", c->account_base);
        ccord_http_response hr = {0};
        bool ok = cloud_http_call(url, "POST", tok, "{}", 2, &hr);
        ccord_mutex_lock(c->mu);
        c->state.busy = false;
        if (ok && hr.status_code == 200) {
          c->state.github_login[0] = '\0';
          c->state.claimed_count = 0;
          copy_str(c->state.status_text, sizeof c->state.status_text, "GitHub account unlinked.");
          cloud_save_disk_locked(c);
          cloud_push_toast_locked(c, "GitHub account unlinked", 0);
        } else {
          extract_api_error(&hr, c->state.error_text, sizeof c->state.error_text);
        }
        c->state.revision++;
        ccord_mutex_unlock(c->mu);
        ccord_http_response_free(&hr);
        break;
      }
      case CJOB_RESCAN_CLAIMS: {
        char url[320];
        snprintf(url, sizeof url, "%s/api/v2/account/github/claim", c->account_base);
        ccord_http_response hr = {0};
        bool ok = cloud_http_call(url, "POST", tok, "{}", 2, &hr);
        ccord_http_response_free(&hr);
        if (ok) exec_refresh_me(c);
        else {
          ccord_mutex_lock(c->mu);
          c->state.busy = false;
          c->state.revision++;
          ccord_mutex_unlock(c->mu);
        }
        break;
      }
      case CJOB_PATCH_WAKE: {
        char dev_id[48];
        ccord_mutex_lock(c->mu);
        copy_str(dev_id, sizeof dev_id, c->state.device_id);
        ccord_mutex_unlock(c->mu);
        if (dev_id[0]) {
          char enc_id[96], url[384];
          url_encode(dev_id, enc_id, sizeof enc_id);
          snprintf(url, sizeof url, "%s/api/v2/account/devices/%s", c->account_base, enc_id);
          const char *payload = j.flag ? "{\"wake_enabled\":true}" : "{\"wake_enabled\":false}";
          ccord_http_response hr = {0};
          (void)cloud_http_call(url, "PATCH", tok, payload, strlen(payload), &hr);
          ccord_http_response_free(&hr);
        }
        ccord_mutex_lock(c->mu);
        c->state.wake_enabled = j.flag;
        cloud_save_disk_locked(c);
        c->state.revision++;
        ccord_cond_signal(c->wake_cv);
        ccord_mutex_unlock(c->mu);
        break;
      }
      case CJOB_REVOKE_DEVICE: {
        char enc_id[96], url[384];
        url_encode(j.arg1, enc_id, sizeof enc_id);
        snprintf(url, sizeof url, "%s/api/v2/account/devices/%s", c->account_base, enc_id);
        ccord_http_response hr = {0};
        (void)cloud_http_call(url, "DELETE", tok, nullptr, 0, &hr);
        ccord_http_response_free(&hr);
        exec_refresh_me(c);
        break;
      }
      case CJOB_FETCH_SOCIAL:
        exec_fetch_social(c, j.id);
        break;
      case CJOB_RATE_PLUGIN: {
        char enc_id[192], url[512];
        url_encode(j.id, enc_id, sizeof enc_id);
        snprintf(url, sizeof url, "%s/api/v2/plugins/%s/rating", c->catalog_base, enc_id);
        ccord_http_response hr = {0};
        bool ok = false;
        if (j.vote == CCORD_CLOUD_VOTE_CLEAR) {
          ok = cloud_http_call(url, "DELETE", tok, nullptr, 0, &hr);
        } else {
          char payload[48];
          snprintf(payload, sizeof payload, "{\"vote\":%d}", j.vote);
          ok = cloud_http_call(url, "PUT", tok, payload, strlen(payload), &hr);
        }
        ccord_mutex_lock(c->mu);
        c->social.submitting = false;
        if (ok && hr.status_code == 200 && hr.body && hr.body_len > 0) {
          ccord_arena arena;
          ccord_arena_init(&arena, 0);
          ccord_error pe = {0};
          const ccord_json_limits lim = {8, 256, 32768};
          const ccord_json *root = ccord_json_parse(&arena, (const char *)hr.body, hr.body_len, &lim, &pe);
          if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT && strcmp(c->social.plugin_id, j.id) == 0) {
            parse_rating_obj(ccord_json_get(root, "rating"), &c->social.rating);
            c->social.error_text[0] = '\0';
          }
          ccord_arena_free(&arena);
        } else {
          if (ok && hr.status_code == 404 && strcmp(c->social.plugin_id, j.id) == 0) {
            c->social.not_found = true;
          }
          extract_api_error(&hr, c->social.error_text, sizeof c->social.error_text);
        }
        c->social.revision++;
        ccord_mutex_unlock(c->mu);
        ccord_http_response_free(&hr);
        break;
      }
      case CJOB_POST_COMMENT: {
        char enc_id[192], url[512];
        url_encode(j.id, enc_id, sizeof enc_id);
        snprintf(url, sizeof url, "%s/api/v2/plugins/%s/comments", c->catalog_base, enc_id);
        ccord_buf b;
        ccord_buf_init(&b, 2048);
        ccord_jw w;
        ccord_jw_init(&w, &b);
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "body");
        ccord_jw_cstr(&w, j.body);
        if (j.num > 0) {
          ccord_jw_key(&w, "parent_id");
          ccord_jw_int(&w, j.num);
        }
        ccord_jw_end_object(&w);
        ccord_http_response hr = {0};
        bool ok = ccord_jw_ok(&w) && cloud_http_call(url, "POST", tok, ccord_buf_cstr(&b), b.len, &hr);
        ccord_buf_free(&b);

        bool posted = ok && (hr.status_code == 200 || hr.status_code == 201);
        ccord_mutex_lock(c->mu);
        c->social.submitting = false;
        if (!posted) {
          if (ok && hr.status_code == 404 && strcmp(c->social.plugin_id, j.id) == 0) {
            c->social.not_found = true;
          }
          extract_api_error(&hr, c->social.error_text, sizeof c->social.error_text);
          c->social.revision++;
        }
        ccord_mutex_unlock(c->mu);
        ccord_http_response_free(&hr);
        if (posted) exec_fetch_social(c, j.id);
        break;
      }
      case CJOB_DELETE_COMMENT: {
        char url[384];
        snprintf(url, sizeof url, "%s/api/v2/comments/%" PRId64, c->catalog_base, j.num);
        ccord_http_response hr = {0};
        bool ok = cloud_http_call(url, "DELETE", tok, nullptr, 0, &hr);
        bool deleted = ok && hr.status_code == 200;
        ccord_mutex_lock(c->mu);
        c->social.submitting = false;
        if (!deleted) {
          extract_api_error(&hr, c->social.error_text, sizeof c->social.error_text);
          c->social.revision++;
        }
        ccord_mutex_unlock(c->mu);
        ccord_http_response_free(&hr);
        if (deleted && j.id[0]) exec_fetch_social(c, j.id);
        break;
      }
      case CJOB_NONE:
        break;
    }
    cloud_wipe(tok, sizeof tok);
    cloud_wipe(j.body, sizeof j.body);
    cloud_signal_wake_pipe(c);
    cloud_wake_main(c);
  }
}

// ---------------------------------------------------------------- Companion Wake WebSocket loop

static void build_wss_url(const char *https_base, char *out, size_t cap)
{
  if (strncmp(https_base, "https://", 8) == 0) {
    snprintf(out, cap, "wss://%s/api/v2/wake/ws", https_base + 8);
  } else if (strncmp(https_base, "http://", 7) == 0) {
    snprintf(out, cap, "ws://%s/api/v2/wake/ws", https_base + 7);
  } else {
    snprintf(out, cap, "wss://c-cord.org/api/v2/wake/ws");
  }
}

static void handle_wake_frame(ccord_cloud *c, const char *msg, size_t len)
{
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error pe = {0};
  const ccord_json_limits lim = {6, 128, 4096};
  const ccord_json *root = ccord_json_parse(&arena, msg, len, &lim, &pe);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return;
  }
  char op[24];
  json_str_copy(root, "op", op, sizeof op);

  bool should_wake_main = false;
  ccord_mutex_lock(c->mu);
  if (strcmp(op, "hello") == 0 || strcmp(op, "ack") == 0) {
    bool online = ccord_json_bool(ccord_json_get(root, "companion_online"), false);
    if (online != c->state.companion_online) {
      c->state.companion_online = online;
      c->state.revision++;
      should_wake_main = true;
    }
  } else if (strcmp(op, "companion") == 0) {
    bool online = ccord_json_bool(ccord_json_get(root, "online"), false);
    if (online != c->state.companion_online) {
      c->state.companion_online = online;
      c->state.revision++;
      should_wake_main = true;
    }
  } else if (strcmp(op, "wake") == 0) {
    char k[16];
    json_str_copy(root, "k", k, sizeof k);
    ccord_wake_kind wk = (strcmp(k, "call") == 0) ? CCORD_WAKE_CALL : CCORD_WAKE_MSG;
    if (c->rx_wake_count < CLOUD_WAKE_QUEUE_CAP) {
      c->rx_wakes[c->rx_wake_count++] = wk;
    }
    should_wake_main = true;
  }
  ccord_mutex_unlock(c->mu);
  ccord_arena_free(&arena);
  if (should_wake_main) cloud_wake_main(c);
}

static void cloud_wake_fn(void *arg)
{
  ccord_cloud *c = arg;
  uint32_t backoff_ms = 2000;

  for (;;) {
    char tok[160] = "";
    bool want_connect = false;
    ccord_mutex_lock(c->mu);
    while (!c->stop && (!c->state.authenticated || !c->state.wake_enabled || !c->device_token[0])) {
      c->state.wake_connected = false;
      ccord_cond_wait(c->wake_cv, c->mu);
    }
    if (c->stop) {
      ccord_mutex_unlock(c->mu);
      break;
    }
    copy_str(tok, sizeof tok, c->device_token);
    want_connect = tok[0] != '\0' && c->state.wake_enabled;
    ccord_mutex_unlock(c->mu);
    if (!want_connect) continue;

    char ws_url[320], hdrs[320], err_buf[256] = "";
    build_wss_url(c->account_base, ws_url, sizeof ws_url);
    build_auth_headers_json(tok, false, hdrs, sizeof hdrs);
    ccord_ws_client *ws = ccord_ws_client_open(ws_url, hdrs, err_buf, sizeof err_buf);
    cloud_wipe(tok, sizeof tok);
    cloud_wipe(hdrs, sizeof hdrs);

    if (!ws) {
      CCORD_LOG_D("cloud: wake ws connect failed (%s); retry in %u ms", err_buf, backoff_ms);
      ccord_mutex_lock(c->mu);
      if (c->state.wake_connected) {
        c->state.wake_connected = false;
        c->state.revision++;
      }
      (void)ccord_cond_wait_timeout(c->wake_cv, c->mu, backoff_ms);
      ccord_mutex_unlock(c->mu);
      backoff_ms = backoff_ms < 60000u ? backoff_ms * 2u : 60000u;
      continue;
    }

    backoff_ms = 2000;
    ccord_mutex_lock(c->mu);
    c->state.wake_connected = true;
    c->state.revision++;
    ccord_mutex_unlock(c->mu);
    cloud_wake_main(c);

    const uint64_t hb_interval_ms = c->is_mobile ? (570ULL * 1000ULL) : (120ULL * 1000ULL);
    uint64_t next_hb = ccord_clock_ms() + hb_interval_ms;
    uint8_t rx_buf[4096];

    for (;;) {
      cloud_drain_wake_pipe(c);
      ccord_wake_kind tx_wake = CCORD_WAKE_NONE;
      bool do_hb = false;
      bool keep_running = false;
      ccord_mutex_lock(c->mu);
      keep_running = !c->stop && c->state.authenticated && c->state.wake_enabled && c->device_token[0] != '\0';
      if (c->tx_wake_pending != CCORD_WAKE_NONE) {
        tx_wake = c->tx_wake_pending;
        c->tx_wake_pending = CCORD_WAKE_NONE;
      }
      if (c->wake_hb_now) {
        do_hb = true;
        c->wake_hb_now = false;
      }
      ccord_mutex_unlock(c->mu);
      if (!keep_running) break;

      if (tx_wake != CCORD_WAKE_NONE && !c->is_mobile) {
        const char *frame = (tx_wake == CCORD_WAKE_CALL) ? "{\"op\":\"wake\",\"k\":\"call\"}" : "{\"op\":\"wake\",\"k\":\"msg\"}";
        if (!ccord_ws_client_send(ws, (const uint8_t *)frame, strlen(frame), false)) break;
      }

      uint64_t now = ccord_clock_ms();
      if (do_hb || now >= next_hb) {
        static const char k_hb[] = "{\"op\":\"hb\"}";
        if (!ccord_ws_client_send(ws, (const uint8_t *)k_hb, sizeof k_hb - 1u, false)) break;
        next_hb = now + hb_interval_ms;
      }

      uint64_t rem_ms = next_hb > now ? (next_hb - now) : 0u;
      int wait_ms = rem_ms > 600000u ? 600000 : (int)rem_ms;
#if CCORD_OS_WINDOWS
      if (wait_ms > 500) wait_ms = 500;
      int wake_fd = -1;
#else
      int wake_fd = c->wake_pipe[0];
      if (wake_fd < 0 && wait_ms > 500) wait_ms = 500;
#endif
      bool is_bin = false;
      int n = ccord_ws_client_recv_wake(ws, rx_buf, sizeof rx_buf - 1u, &is_bin, wake_fd, wait_ms);
      if (n < 0) break;
      if (n > 0 && !is_bin) {
        rx_buf[n] = '\0';
        handle_wake_frame(c, (const char *)rx_buf, (size_t)n);
      }
    }

    ccord_ws_client_close(ws, 1000, "closing");
    ccord_mutex_lock(c->mu);
    bool changed = c->state.wake_connected || (c->is_mobile && c->state.companion_online);
    c->state.wake_connected = false;
    if (c->is_mobile) c->state.companion_online = false; // fail safe to standalone if socket drops
    if (changed) c->state.revision++;
    ccord_mutex_unlock(c->mu);
    if (changed) cloud_wake_main(c);

    ccord_mutex_lock(c->mu);
    if (!c->stop && c->state.authenticated && c->state.wake_enabled) {
      (void)ccord_cond_wait_timeout(c->wake_cv, c->mu, backoff_ms);
    }
    ccord_mutex_unlock(c->mu);
  }
}

// ---------------------------------------------------------------- public API

ccord_cloud *ccord_cloud_create(const ccord_cloud_config *cfg)
{
  ccord_cloud *c = ccord_calloc(1, sizeof *c);
  if (!c) return nullptr;
#if !CCORD_OS_WINDOWS
  c->wake_pipe[0] = -1;
  c->wake_pipe[1] = -1;
  if (pipe(c->wake_pipe) == 0) {
    for (int i = 0; i < 2; i++) {
      int fl = fcntl(c->wake_pipe[i], F_GETFL, 0);
      if (fl >= 0) (void)fcntl(c->wake_pipe[i], F_SETFL, fl | O_NONBLOCK);
      int fdfl = fcntl(c->wake_pipe[i], F_GETFD, 0);
      if (fdfl >= 0) (void)fcntl(c->wake_pipe[i], F_SETFD, fdfl | FD_CLOEXEC);
    }
  } else {
    c->wake_pipe[0] = -1;
    c->wake_pipe[1] = -1;
  }
#endif
  c->mu = ccord_mutex_create();
  c->cv = ccord_cond_create();
  c->wake_cv = ccord_cond_create();
  if (!c->mu || !c->cv || !c->wake_cv) {
    ccord_cloud_destroy(c);
    return nullptr;
  }

  c->state.discord_oauth_ready = true;
  c->state.github_link_ready = true;
  c->state.wake_enabled = true;
  c->social.rating.score_x10 = -1;

  const char *env_acct = getenv("CCORD_ACCOUNT_URL");
  const char *env_cat = getenv("CCORD_CATALOG_URL");
  const char *ab = (cfg && cfg->account_base_url && cfg->account_base_url[0])
                     ? cfg->account_base_url
                     : ((env_acct && env_acct[0]) ? env_acct : CLOUD_DEFAULT_ACCOUNT_URL);
  const char *cb = (cfg && cfg->catalog_base_url && cfg->catalog_base_url[0])
                     ? cfg->catalog_base_url
                     : ((env_cat && env_cat[0]) ? env_cat : CLOUD_DEFAULT_CATALOG_URL);
  copy_str(c->account_base, sizeof c->account_base, ab);
  copy_str(c->catalog_base, sizeof c->catalog_base, cb);
  trim_trailing_slash(c->account_base);
  trim_trailing_slash(c->catalog_base);

  if (cfg) {
    c->is_mobile = cfg->is_mobile;
    c->disable_network = cfg->disable_network;
    c->cbs = cfg->cbs;
    if (cfg->data_dir && cfg->data_dir[0]) {
      copy_str(c->data_dir, sizeof c->data_dir, cfg->data_dir);
      c->has_data_dir = true;
    }
  }

  ccord_mutex_lock(c->mu);
  cloud_load_disk_locked(c);
  if (c->state.authenticated && !c->disable_network) {
    cloud_job j = {.kind = CJOB_REFRESH_ME};
    (void)cloud_enqueue_locked(c, &j);
    ccord_cond_signal(c->wake_cv);
  }
  ccord_mutex_unlock(c->mu);
  return c;
}

void ccord_cloud_destroy(ccord_cloud *c)
{
  if (!c) return;
  if (c->mu) {
    ccord_mutex_lock(c->mu);
    c->stop = true;
    c->login_cancel = true;
    if (c->cv) ccord_cond_broadcast(c->cv);
    if (c->wake_cv) ccord_cond_broadcast(c->wake_cv);
    ccord_mutex_unlock(c->mu);
  }
  cloud_signal_wake_pipe(c);
  if (c->worker) ccord_thread_join(c->worker);
  if (c->wake_thread) ccord_thread_join(c->wake_thread);
  cloud_wipe(c->device_token, sizeof c->device_token);
  cloud_wipe(c->pkce_verifier, sizeof c->pkce_verifier);
#if !CCORD_OS_WINDOWS
  if (c->wake_pipe[0] >= 0) close(c->wake_pipe[0]);
  if (c->wake_pipe[1] >= 0) close(c->wake_pipe[1]);
#endif
  if (c->wake_cv) ccord_cond_destroy(c->wake_cv);
  if (c->cv) ccord_cond_destroy(c->cv);
  if (c->mu) ccord_mutex_destroy(c->mu);
  ccord_free(c);
}

bool ccord_cloud_pump(ccord_cloud *c)
{
  if (!c) return false;
  char url_to_open[768] = "";
  cloud_toast_item toasts[CLOUD_TOAST_CAP];
  size_t ntoasts = 0;
  bool changed = false;

  ccord_mutex_lock(c->mu);
  if (c->pending_open_url[0]) {
    copy_str(url_to_open, sizeof url_to_open, c->pending_open_url);
    c->pending_open_url[0] = '\0';
  }
  if (c->toast_count > 0) {
    ntoasts = c->toast_count;
    memcpy(toasts, c->toasts, ntoasts * sizeof toasts[0]);
    c->toast_count = 0;
  }
  if (c->state.revision != c->pumped_state_rev || c->social.revision != c->pumped_social_rev) {
    c->pumped_state_rev = c->state.revision;
    c->pumped_social_rev = c->social.revision;
    changed = true;
  }
  ccord_mutex_unlock(c->mu);

  if (url_to_open[0] && c->cbs.open_url) {
    (void)c->cbs.open_url(c->cbs.ud, url_to_open);
  }
  for (size_t i = 0; i < ntoasts; i++) {
    if (c->cbs.toast) c->cbs.toast(c->cbs.ud, toasts[i].msg, toasts[i].level);
  }
  return changed;
}

void ccord_cloud_get_state(const ccord_cloud *c, ccord_cloud_state *out)
{
  if (!out) return;
  if (!c) { memset(out, 0, sizeof *out); return; }
  ccord_mutex_lock(c->mu);
  *out = c->state;
  ccord_mutex_unlock(c->mu);
}

void ccord_cloud_get_social(const ccord_cloud *c, ccord_cloud_social *out)
{
  if (!out) return;
  if (!c) { memset(out, 0, sizeof *out); out->rating.score_x10 = -1; return; }
  ccord_mutex_lock(c->mu);
  *out = c->social;
  ccord_mutex_unlock(c->mu);
}

uint64_t ccord_cloud_revision(const ccord_cloud *c)
{
  if (!c) return 0;
  ccord_mutex_lock(c->mu);
  uint64_t r = c->state.revision;
  ccord_mutex_unlock(c->mu);
  return r;
}

uint64_t ccord_cloud_social_revision(const ccord_cloud *c)
{
  if (!c) return 0;
  ccord_mutex_lock(c->mu);
  uint64_t r = c->social.revision;
  ccord_mutex_unlock(c->mu);
  return r;
}

void ccord_cloud_start_login(ccord_cloud *c)
{
  if (!c || c->disable_network) return;
  ccord_mutex_lock(c->mu);
  if (c->state.signing_in) {
    ccord_mutex_unlock(c->mu);
    return;
  }
  c->state.signing_in = true;
  c->login_cancel = false;
  c->state.error_text[0] = '\0';

  if (c->is_mobile) {
    uint8_t rnd_v[32], rnd_s[16];
    char verifier[96], challenge[64], state_hex[64], enc_ru[128], auth_url[768];
    if (ccord_random_bytes(rnd_v, sizeof rnd_v) == CCORD_OK &&
        ccord_random_bytes(rnd_s, sizeof rnd_s) == CCORD_OK) {
      base64url_encode(rnd_v, sizeof rnd_v, verifier, sizeof verifier);
      hex_encode(rnd_s, sizeof rnd_s, state_hex, sizeof state_hex);
      if (ccord_cloud_pkce_s256(verifier, challenge)) {
        const char *ru = "ccord://auth/callback";
        url_encode(ru, enc_ru, sizeof enc_ru);
        snprintf(auth_url, sizeof auth_url,
                 "%s/auth/discord/login?mode=native&redirect_uri=%s&code_challenge=%s&code_challenge_method=S256&state=%s",
                 c->account_base, enc_ru, challenge, state_hex);
        copy_str(c->pkce_verifier, sizeof c->pkce_verifier, verifier);
        copy_str(c->pkce_state, sizeof c->pkce_state, state_hex);
        copy_str(c->pkce_redirect, sizeof c->pkce_redirect, ru);
        copy_str(c->pending_open_url, sizeof c->pending_open_url, auth_url);
        copy_str(c->state.status_text, sizeof c->state.status_text, "Complete Discord sign-in in your browser...");
      } else {
        c->state.signing_in = false;
      }
      cloud_wipe(verifier, sizeof verifier);
    } else {
      c->state.signing_in = false;
      copy_str(c->state.error_text, sizeof c->state.error_text, "OS CSPRNG unavailable");
    }
    cloud_wipe(rnd_v, sizeof rnd_v);
    cloud_wipe(rnd_s, sizeof rnd_s);
    c->state.revision++;
  } else {
    copy_str(c->state.status_text, sizeof c->state.status_text, "Opening Discord sign-in...");
    c->state.revision++;
    cloud_job j = {.kind = CJOB_LOOPBACK_LOGIN};
    (void)cloud_enqueue_locked(c, &j);
  }
  ccord_mutex_unlock(c->mu);
}

void ccord_cloud_cancel_login(ccord_cloud *c)
{
  if (!c) return;
  ccord_mutex_lock(c->mu);
  c->login_cancel = true;
  if (c->is_mobile) {
    c->state.signing_in = false;
    cloud_wipe(c->pkce_verifier, sizeof c->pkce_verifier);
    c->pkce_state[0] = '\0';
    copy_str(c->state.status_text, sizeof c->state.status_text, "Sign-in cancelled.");
    c->state.revision++;
  }
  ccord_mutex_unlock(c->mu);
}

bool ccord_cloud_handle_callback_url(ccord_cloud *c, const char *url)
{
  if (!c || !url) return false;
  if (ccord_strncasecmp(url, "ccord://account/github-linked", 29) == 0) {
    char gh[65] = "", claimed_s[16] = "";
    const char *q = strchr(url, '?');
    if (q) {
      (void)ccord_cloud_parse_query_param(q + 1, "github", gh, sizeof gh);
      (void)ccord_cloud_parse_query_param(q + 1, "claimed", claimed_s, sizeof claimed_s);
    }
    ccord_mutex_lock(c->mu);
    char msg[192];
    if (gh[0]) snprintf(msg, sizeof msg, "Linked GitHub @%s (%s plugin(s) claimed)", gh, claimed_s[0] ? claimed_s : "0");
    else snprintf(msg, sizeof msg, "GitHub account linked!");
    copy_str(c->state.status_text, sizeof c->state.status_text, msg);
    cloud_push_toast_locked(c, msg, 1);
    c->state.revision++;
    cloud_job j = {.kind = CJOB_REFRESH_ME};
    (void)cloud_enqueue_locked(c, &j);
    ccord_mutex_unlock(c->mu);
    return true;
  }

  if (ccord_strncasecmp(url, "ccord://auth/callback", 21) != 0) return false;
  const char *q = strchr(url, '?');
  if (!q) return true;
  char code[256] = "", st[128] = "";
  (void)ccord_cloud_parse_query_param(q + 1, "code", code, sizeof code);
  (void)ccord_cloud_parse_query_param(q + 1, "state", st, sizeof st);

  ccord_mutex_lock(c->mu);
  if (!c->pkce_verifier[0] || !c->pkce_state[0] || strcmp(st, c->pkce_state) != 0 || !code[0]) {
    c->state.signing_in = false;
    copy_str(c->state.error_text, sizeof c->state.error_text, "OAuth callback state mismatch or expired");
    cloud_wipe(c->pkce_verifier, sizeof c->pkce_verifier);
    c->pkce_state[0] = '\0';
    c->state.revision++;
    ccord_mutex_unlock(c->mu);
    return true;
  }
  cloud_job j = {.kind = CJOB_EXCHANGE_CODE};
  copy_str(j.arg1, sizeof j.arg1, code);
  copy_str(j.arg2, sizeof j.arg2, c->pkce_redirect);
  copy_str(j.body, sizeof j.body, c->pkce_verifier);
  cloud_wipe(c->pkce_verifier, sizeof c->pkce_verifier);
  c->pkce_state[0] = '\0';
  c->state.busy = true;
  copy_str(c->state.status_text, sizeof c->state.status_text, "Finishing sign-in...");
  c->state.revision++;
  (void)cloud_enqueue_locked(c, &j);
  ccord_mutex_unlock(c->mu);
  return true;
}

void ccord_cloud_refresh(ccord_cloud *c)
{
  if (!c || c->disable_network) return;
  ccord_mutex_lock(c->mu);
  c->state.busy = true;
  c->state.revision++;
  cloud_job j = {.kind = CJOB_REFRESH_ME};
  (void)cloud_enqueue_locked(c, &j);
  ccord_mutex_unlock(c->mu);
}

void ccord_cloud_sign_out(ccord_cloud *c)
{
  if (!c) return;
  ccord_mutex_lock(c->mu);
  if (c->disable_network) {
    cloud_wipe(c->device_token, sizeof c->device_token);
    c->state.authenticated = false;
    cloud_save_disk_locked(c);
    c->state.revision++;
  } else {
    cloud_job j = {.kind = CJOB_SIGN_OUT};
    (void)cloud_enqueue_locked(c, &j);
  }
  ccord_mutex_unlock(c->mu);
}

void ccord_cloud_link_github(ccord_cloud *c)
{
  if (!c || c->disable_network) return;
  ccord_mutex_lock(c->mu);
  if (!c->state.authenticated) { ccord_mutex_unlock(c->mu); return; }
  c->state.busy = true;
  c->state.error_text[0] = '\0';
  c->state.revision++;
  cloud_job j = {.kind = CJOB_LINK_GITHUB};
  (void)cloud_enqueue_locked(c, &j);
  ccord_mutex_unlock(c->mu);
}

void ccord_cloud_unlink_github(ccord_cloud *c)
{
  if (!c || c->disable_network) return;
  ccord_mutex_lock(c->mu);
  if (!c->state.authenticated) { ccord_mutex_unlock(c->mu); return; }
  c->state.busy = true;
  c->state.revision++;
  cloud_job j = {.kind = CJOB_UNLINK_GITHUB};
  (void)cloud_enqueue_locked(c, &j);
  ccord_mutex_unlock(c->mu);
}

void ccord_cloud_rescan_claims(ccord_cloud *c)
{
  if (!c || c->disable_network) return;
  ccord_mutex_lock(c->mu);
  if (!c->state.authenticated) { ccord_mutex_unlock(c->mu); return; }
  c->state.busy = true;
  c->state.revision++;
  cloud_job j = {.kind = CJOB_RESCAN_CLAIMS};
  (void)cloud_enqueue_locked(c, &j);
  ccord_mutex_unlock(c->mu);
}

void ccord_cloud_set_wake_enabled(ccord_cloud *c, bool enabled)
{
  if (!c) return;
  ccord_mutex_lock(c->mu);
  c->state.wake_enabled = enabled;
  c->state.revision++;
  if (!c->disable_network && c->state.authenticated) {
    cloud_job j = {.kind = CJOB_PATCH_WAKE, .flag = enabled};
    (void)cloud_enqueue_locked(c, &j);
  } else {
    cloud_save_disk_locked(c);
  }
  ccord_cond_signal(c->wake_cv);
  ccord_mutex_unlock(c->mu);
  cloud_signal_wake_pipe(c);
}

void ccord_cloud_revoke_device(ccord_cloud *c, const char *device_id)
{
  if (!c || c->disable_network || !device_id || !device_id[0]) return;
  ccord_mutex_lock(c->mu);
  if (!c->state.authenticated) { ccord_mutex_unlock(c->mu); return; }
  cloud_job j = {.kind = CJOB_REVOKE_DEVICE};
  copy_str(j.arg1, sizeof j.arg1, device_id);
  (void)cloud_enqueue_locked(c, &j);
  ccord_mutex_unlock(c->mu);
}

void ccord_cloud_fetch_social(ccord_cloud *c, const char *plugin_id)
{
  if (!c || !plugin_id || !plugin_id[0]) return;
  ccord_mutex_lock(c->mu);
  if (strcmp(c->social.plugin_id, plugin_id) != 0) {
    uint64_t rev = c->social.revision + 1u;
    memset(&c->social, 0, sizeof c->social);
    c->social.rating.score_x10 = -1;
    copy_str(c->social.plugin_id, sizeof c->social.plugin_id, plugin_id);
    c->social.loading = !c->disable_network;
    c->social.revision = rev;
  } else {
    c->social.loading = !c->disable_network;
    c->social.not_found = false;
    c->social.revision++;
  }
  if (!c->disable_network) {
    cloud_job j = {.kind = CJOB_FETCH_SOCIAL};
    copy_str(j.id, sizeof j.id, plugin_id);
    (void)cloud_enqueue_locked(c, &j);
  }
  ccord_mutex_unlock(c->mu);
}

void ccord_cloud_rate_plugin(ccord_cloud *c, const char *plugin_id, int vote)
{
  if (!c || c->disable_network || !plugin_id || !plugin_id[0]) return;
  if (vote != -1 && vote != 0 && vote != 1 && vote != CCORD_CLOUD_VOTE_CLEAR) return;
  ccord_mutex_lock(c->mu);
  if (!c->state.authenticated) {
    copy_str(c->social.error_text, sizeof c->social.error_text, "Sign in to c-cord.org in Settings > C-Cord to rate plugins.");
    c->social.revision++;
    ccord_mutex_unlock(c->mu);
    return;
  }
  c->social.submitting = true;
  c->social.error_text[0] = '\0';
  c->social.revision++;
  cloud_job j = {.kind = CJOB_RATE_PLUGIN, .vote = vote};
  copy_str(j.id, sizeof j.id, plugin_id);
  (void)cloud_enqueue_locked(c, &j);
  ccord_mutex_unlock(c->mu);
}

void ccord_cloud_post_comment(ccord_cloud *c, const char *plugin_id, int64_t parent_id, const char *body)
{
  if (!c || c->disable_network || !plugin_id || !plugin_id[0] || !body || !body[0]) return;
  ccord_mutex_lock(c->mu);
  if (!c->state.authenticated) {
    copy_str(c->social.error_text, sizeof c->social.error_text, "Sign in to c-cord.org in Settings > C-Cord to comment.");
    c->social.revision++;
    ccord_mutex_unlock(c->mu);
    return;
  }
  c->social.submitting = true;
  c->social.error_text[0] = '\0';
  c->social.revision++;
  cloud_job j = {.kind = CJOB_POST_COMMENT, .num = parent_id};
  copy_str(j.id, sizeof j.id, plugin_id);
  copy_str(j.body, sizeof j.body, body);
  (void)cloud_enqueue_locked(c, &j);
  ccord_mutex_unlock(c->mu);
}

void ccord_cloud_delete_comment(ccord_cloud *c, const char *plugin_id, int64_t comment_id)
{
  if (!c || c->disable_network || comment_id <= 0) return;
  ccord_mutex_lock(c->mu);
  if (!c->state.authenticated) { ccord_mutex_unlock(c->mu); return; }
  c->social.submitting = true;
  c->social.revision++;
  cloud_job j = {.kind = CJOB_DELETE_COMMENT, .num = comment_id};
  if (plugin_id) copy_str(j.id, sizeof j.id, plugin_id);
  (void)cloud_enqueue_locked(c, &j);
  ccord_mutex_unlock(c->mu);
}

void ccord_cloud_emit_wake(ccord_cloud *c, ccord_wake_kind kind)
{
  if (!c || c->is_mobile || c->disable_network || kind == CCORD_WAKE_NONE) return;
  bool sig = false;
  ccord_mutex_lock(c->mu);
  if (c->state.authenticated && c->state.wake_enabled) {
    // Call wakes always take priority over pending msg wakes.
    if (c->tx_wake_pending != CCORD_WAKE_CALL) c->tx_wake_pending = kind;
    ccord_cond_signal(c->wake_cv);
    sig = true;
  }
  ccord_mutex_unlock(c->mu);
  if (sig) cloud_signal_wake_pipe(c);
}

bool ccord_cloud_pop_wake(ccord_cloud *c, ccord_wake_kind *out_kind)
{
  if (out_kind) *out_kind = CCORD_WAKE_NONE;
  if (!c) return false;
  ccord_mutex_lock(c->mu);
  bool have = c->rx_wake_count > 0;
  if (have) {
    if (out_kind) *out_kind = c->rx_wakes[0];
    c->rx_wake_count--;
    if (c->rx_wake_count > 0) {
      memmove(&c->rx_wakes[0], &c->rx_wakes[1], c->rx_wake_count * sizeof c->rx_wakes[0]);
    }
  }
  ccord_mutex_unlock(c->mu);
  return have;
}

bool ccord_cloud_companion_online(const ccord_cloud *c)
{
  if (!c) return false;
  ccord_mutex_lock(c->mu);
  bool on = c->state.authenticated && c->state.wake_enabled && c->state.wake_connected && c->state.companion_online;
  ccord_mutex_unlock(c->mu);
  return on;
}

void ccord_cloud_wake_heartbeat_now(ccord_cloud *c)
{
  if (!c) return;
  ccord_mutex_lock(c->mu);
  c->wake_hb_now = true;
  ccord_cond_signal(c->wake_cv);
  ccord_mutex_unlock(c->mu);
  cloud_signal_wake_pipe(c);
}
