// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/net/video_resolve.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>

#if defined(CCORD_VIDEO_RESOLVE_LIVE)
#include <ccord/net/live_transport.h>
#include <curl/curl.h>
#include "tls_internal.h"
#if CCORD_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !CCORD_OS_ANDROID
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

// ---- string & URL helpers -----------------------------------------------------------------------------------

static bool vr_ascii_eq_ci(const char *a, const char *b, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

static bool vr_cstr_eq_ci(const char *a, const char *b)
{
  if (!a || !b) return false;
  size_t na = strlen(a), nb = strlen(b);
  return na == nb && vr_ascii_eq_ci(a, b, na);
}

static bool vr_host_ok(const char *host, size_t n)
{
  if (n == 0 || n > 253u) return false;
  bool dot = false, last_numeric = true;
  size_t label = 0;
  for (size_t i = 0; i < n; i++) {
    char c = host[i];
    if (c == '.') {
      if (label == 0) return false;
      dot = true;
      label = 0;
      last_numeric = true;
      continue;
    }
    bool digit = c >= '0' && c <= '9';
    bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!digit && !alpha && c != '-') return false;
    if (!digit) last_numeric = false;
    if (++label > 63u) return false;
  }
  if (!dot || label == 0 || last_numeric) return false;
  static const char *const k_local[] = {"localhost", ".localhost", ".local"};
  for (size_t i = 0; i < sizeof k_local / sizeof k_local[0]; i++) {
    size_t ln = strlen(k_local[i]);
    if (n >= ln && vr_ascii_eq_ci(host + (n - ln), k_local[i], ln) && (k_local[i][0] == '.' || n == ln)) {
      return false;
    }
  }
  return true;
}

static bool vr_extract_host_and_path(const char *url, const char **host_out, size_t *host_len_out, const char **path_out)
{
  if (!url || !vr_ascii_eq_ci(url, "https://", 8)) return false;
  const char *host = url + 8;
  const char *slash = strchr(host, '/');
  if (!slash || slash == host) return false;
  if (slash[1] == '\0' || slash[1] == '?' || slash[1] == '#') return false;
  const char *host_end = slash;
  const char *colon = memchr(host, ':', (size_t)(slash - host));
  if (colon) {
    if (slash - colon - 1 != 3 || memcmp(colon + 1, "443", 3) != 0) return false;
    host_end = colon;
  }
  size_t hlen = (size_t)(host_end - host);
  if (!vr_host_ok(host, hlen)) return false;
  *host_out = host;
  *host_len_out = hlen;
  *path_out = slash;
  return true;
}

bool ccord_video_resolve_url_safe(const char *url, size_t max_len)
{
  if (!url || max_len == 0) return false;
  size_t n = strlen(url);
  if (n == 0 || n >= max_len) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)url[i];
    if (c <= 0x20u || c == 0x7fu) return false;
  }
  const char *host = nullptr, *path = nullptr;
  size_t hlen = 0;
  return vr_extract_host_and_path(url, &host, &hlen, &path);
}

static bool vr_stream_url_allowed(const char *url)
{
  if (!ccord_video_resolve_url_safe(url, CCORD_VIDEO_RESOLVE_STREAM_URL_MAX)) return false;
#if defined(CCORD_VIDEO_RESOLVE_LIVE)
  if (!ccord_live_user_video_url_allowed(url)) return false;
#endif
  return true;
}

static bool vr_host_in_list(const char *host, size_t hlen, const char *const *list, size_t count)
{
  for (size_t i = 0; i < count; i++) {
    size_t ln = strlen(list[i]);
    if (ln == hlen && vr_ascii_eq_ci(host, list[i], hlen)) return true;
  }
  return false;
}

bool ccord_video_url_is_twitter(const char *url, char *tweet_id_out, size_t id_cap)
{
  if (tweet_id_out && id_cap > 0) tweet_id_out[0] = '\0';
  const char *host = nullptr, *path = nullptr;
  size_t hlen = 0;
  if (!vr_extract_host_and_path(url, &host, &hlen, &path)) return false;
  static const char *const k_hosts[] = {
    "x.com", "www.x.com", "mobile.x.com",
    "twitter.com", "www.twitter.com", "mobile.twitter.com",
    "fxtwitter.com", "www.fxtwitter.com",
    "vxtwitter.com", "www.vxtwitter.com",
    "fixupx.com", "www.fixupx.com",
    "fixvx.com", "www.fixvx.com",
    "twittpr.com", "www.twittpr.com",
    "xcancel.com", "www.xcancel.com",
  };
  if (!vr_host_in_list(host, hlen, k_hosts, sizeof k_hosts / sizeof k_hosts[0])) return false;

  const char *path_end = path + strcspn(path, "?#");
  const char *id = nullptr;
  const char *p = path;
  while (p < path_end) {
    if (*p != '/') {
      p++;
      continue;
    }
    const char *seg = p + 1;
    const char *sep = seg;
    while (sep < path_end && *sep != '/') sep++;
    size_t slen = (size_t)(sep - seg);
    if ((slen == 6 && vr_ascii_eq_ci(seg, "status", 6)) ||
        (slen == 8 && vr_ascii_eq_ci(seg, "statuses", 8)) ||
        (slen == 5 && vr_ascii_eq_ci(seg, "tweet", 5))) {
      if (sep < path_end && *sep == '/') {
        id = sep + 1;
        break;
      }
    }
    p = sep;
  }
  if (!id || id >= path_end) return false;
  const char *q = id;
  while (q < path_end && *q >= '0' && *q <= '9') q++;
  size_t ilen = (size_t)(q - id);
  if (ilen == 0 || ilen > 24u) return false;
  if (q < path_end && *q != '/' && *q != '.') return false;
  if (tweet_id_out && id_cap > 0) {
    if (ilen >= id_cap) return false;
    memcpy(tweet_id_out, id, ilen);
    tweet_id_out[ilen] = '\0';
  }
  return true;
}

static bool vr_yt_id_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool ccord_video_url_is_youtube(const char *url, char *video_id_out, size_t id_cap)
{
  if (video_id_out && id_cap > 0) video_id_out[0] = '\0';
  const char *host = nullptr, *path = nullptr;
  size_t hlen = 0;
  if (!vr_extract_host_and_path(url, &host, &hlen, &path)) return false;
  static const char *const k_hosts[] = {
    "youtube.com", "www.youtube.com", "m.youtube.com", "music.youtube.com",
    "youtu.be", "www.youtu.be",
    "youtube-nocookie.com", "www.youtube-nocookie.com",
  };
  if (!vr_host_in_list(host, hlen, k_hosts, sizeof k_hosts / sizeof k_hosts[0])) return false;

  const char *path_end = path + strcspn(path, "?#");
  const char *id = nullptr;
  size_t ilen = 0;

  if (vr_ascii_eq_ci(host, "youtu.be", hlen) || vr_ascii_eq_ci(host, "www.youtu.be", hlen)) {
    const char *s = path + 1;
    const char *e = s;
    while (e < path_end && *e != '/') e++;
    id = s;
    ilen = (size_t)(e - s);
  } else {
    static const char *const k_pfx[] = {"/embed/", "/shorts/", "/live/", "/v/"};
    for (size_t i = 0; i < sizeof k_pfx / sizeof k_pfx[0]; i++) {
      size_t pl = strlen(k_pfx[i]);
      if ((size_t)(path_end - path) > pl && vr_ascii_eq_ci(path, k_pfx[i], pl)) {
        const char *s = path + pl;
        const char *e = s;
        while (e < path_end && *e != '/') e++;
        id = s;
        ilen = (size_t)(e - s);
        break;
      }
    }
    if (!id && *path_end == '?') {
      const char *q = path_end + 1;
      const char *qend = q + strcspn(q, "#");
      while (q < qend) {
        const char *amp = q;
        while (amp < qend && *amp != '&') amp++;
        if (amp - q > 2 && q[0] == 'v' && q[1] == '=') {
          id = q + 2;
          ilen = (size_t)(amp - id);
          break;
        }
        q = amp < qend ? amp + 1 : qend;
      }
    }
  }
  if (!id || ilen < 6u || ilen > 20u) return false;
  for (size_t i = 0; i < ilen; i++) {
    if (!vr_yt_id_char(id[i])) return false;
  }
  if (video_id_out && id_cap > 0) {
    if (ilen >= id_cap) return false;
    memcpy(video_id_out, id, ilen);
    video_id_out[ilen] = '\0';
  }
  return true;
}

bool ccord_video_twitter_syndication_token(const char *tweet_id, char *out, size_t cap)
{
  if (!tweet_id || !tweet_id[0] || !out || cap < 8u) return false;
  for (const char *p = tweet_id; *p; p++) {
    if (*p < '0' || *p > '9') return false;
  }
  double val = (strtod(tweet_id, nullptr) / 1e15) * 3.14159265358979323846;
  if (!(val > 0.0) || !isfinite(val)) return false;

  static const char k_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  char raw[64];
  size_t n = 0;
  double ip = floor(val);
  double fp = val - ip;

  // Integer part in base 36.
  char irev[32];
  size_t in = 0;
  uint64_t iv = ip < 1e18 ? (uint64_t)ip : 0u;
  if (iv == 0) {
    irev[in++] = '0';
  } else {
    while (iv > 0 && in < sizeof irev) {
      irev[in++] = k_digits[iv % 36u];
      iv /= 36u;
    }
  }
  while (in > 0 && n + 1 < sizeof raw) raw[n++] = irev[--in];

  // Fractional part in base 36 (10 digits, matching JS toString(36) precision for this range).
  for (int d = 0; d < 10 && n + 1 < sizeof raw; d++) {
    fp *= 36.0;
    int digit = (int)floor(fp);
    if (digit < 0) digit = 0;
    if (digit > 35) digit = 35;
    fp -= (double)digit;
    raw[n++] = k_digits[digit];
  }

  // Strip '0' (matches JS `.replace(/(0+|\.)/g, '')`).
  size_t o = 0;
  for (size_t i = 0; i < n && o + 1 < cap; i++) {
    if (raw[i] != '0') out[o++] = raw[i];
  }
  if (o == 0) out[o++] = '1';
  out[o] = '\0';
  return true;
}

// ---- JSON & yt-dlp parsers ----------------------------------------------------------------------------------

static void vr_copy_str(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  dst[0] = '\0';
  if (!src) return;
  size_t n = strlen(src);
  if (n >= cap) return;
  memcpy(dst, src, n + 1u);
}

static bool vr_is_mp4_type(const char *ct, const char *url)
{
  if (ct && ct[0]) return vr_cstr_eq_ci(ct, "video/mp4");
  if (!url) return false;
  size_t n = strcspn(url, "?#");
  return n >= 4 && vr_ascii_eq_ci(url + n - 4, ".mp4", 4);
}

// Picks the best MP4 variant: highest bitrate <= 3 Mbps (crisp 720p for chat embeds), else lowest bitrate above it.
static void vr_pick_mp4_variants(const ccord_json *variants, const char *url_key, const char *ct_key,
                                 char *best_url, size_t best_cap, int64_t *best_br, bool *have_under)
{
  if (!variants || ccord_json_type_of(variants) != CCORD_JSON_ARRAY) return;
  const int64_t target_max = 3000000;
  size_t nv = ccord_json_array_len(variants);
  for (size_t i = 0; i < nv; i++) {
    const ccord_json *v = ccord_json_array_at(variants, i);
    if (!v || ccord_json_type_of(v) != CCORD_JSON_OBJECT) continue;
    const char *u = ccord_json_string(ccord_json_get(v, url_key), nullptr);
    const char *ct = ccord_json_string(ccord_json_get(v, ct_key), nullptr);
    if (!u || !vr_is_mp4_type(ct, u) || !vr_stream_url_allowed(u)) continue;
    int64_t br = 0;
    const ccord_json *bj = ccord_json_get(v, "bitrate");
    if (bj) (void)ccord_json_int64(bj, &br);
    bool under = br <= target_max;
    bool take = false;
    if (!best_url[0]) {
      take = true;
    } else if (under && !*have_under) {
      take = true;
    } else if (under && *have_under) {
      if (br >= *best_br) take = true;
    } else if (!under && !*have_under) {
      if (br < *best_br) take = true;
    }
    if (take) {
      vr_copy_str(best_url, best_cap, u);
      *best_br = br;
      *have_under = under;
    }
  }
}

static bool vr_parse_twitter_tweet_obj(const ccord_json *root, ccord_video_resolved *out)
{
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) return false;
  char best_url[CCORD_VIDEO_RESOLVE_STREAM_URL_MAX] = "";
  int64_t best_br = -1;
  bool have_under = false;
  uint32_t dur_ms = 0;

  const ccord_json *md = ccord_json_get(root, "mediaDetails");
  if (md && ccord_json_type_of(md) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(md);
    for (size_t i = 0; i < n && !best_url[0]; i++) {
      const ccord_json *item = ccord_json_array_at(md, i);
      if (!item || ccord_json_type_of(item) != CCORD_JSON_OBJECT) continue;
      const ccord_json *vi = ccord_json_get(item, "video_info");
      if (!vi || ccord_json_type_of(vi) != CCORD_JSON_OBJECT) continue;
      vr_pick_mp4_variants(ccord_json_get(vi, "variants"), "url", "content_type", best_url, sizeof best_url, &best_br,
                           &have_under);
      if (best_url[0]) {
        int64_t dms = 0;
        if (ccord_json_int64(ccord_json_get(vi, "duration_millis"), &dms) && dms > 0 && dms <= (int64_t)UINT32_MAX) {
          dur_ms = (uint32_t)dms;
        }
      }
    }
  }

  if (!best_url[0]) {
    const ccord_json *vid = ccord_json_get(root, "video");
    if (vid && ccord_json_type_of(vid) == CCORD_JSON_OBJECT) {
      vr_pick_mp4_variants(ccord_json_get(vid, "variants"), "src", "type", best_url, sizeof best_url, &best_br,
                           &have_under);
      if (best_url[0]) {
        int64_t dms = 0;
        if (ccord_json_int64(ccord_json_get(vid, "durationMs"), &dms) && dms > 0 && dms <= (int64_t)UINT32_MAX) {
          dur_ms = (uint32_t)dms;
        }
      }
    }
  }

  if (!best_url[0]) return false;
  vr_copy_str(out->video_url, sizeof out->video_url, best_url);
  out->audio_url[0] = '\0';
  out->duration_ms = dur_ms;
  return true;
}

bool ccord_video_parse_twitter_syndication(const char *json, size_t len, ccord_video_resolved *out)
{
  if (!json || len == 0 || !out) return false;
  ccord_arena arena;
  ccord_arena_init(&arena, 16384u);
  const ccord_json_limits lim = {.max_depth = 32u, .max_nodes = 32768u, .max_string = 1024u * 1024u};
  ccord_error err;
  const ccord_json *root = ccord_json_parse(&arena, json, len, &lim, &err);
  bool ok = false;
  if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    ok = vr_parse_twitter_tweet_obj(root, out);
    if (!ok) ok = vr_parse_twitter_tweet_obj(ccord_json_get(root, "quoted_tweet"), out);
  }
  ccord_arena_free(&arena);
  return ok;
}

static bool vr_parse_fxtwitter_tweet(const ccord_json *tweet, ccord_video_resolved *out)
{
  if (!tweet || ccord_json_type_of(tweet) != CCORD_JSON_OBJECT) return false;
  const ccord_json *media = ccord_json_get(tweet, "media");
  if (!media || ccord_json_type_of(media) != CCORD_JSON_OBJECT) return false;
  const ccord_json *videos = ccord_json_get(media, "videos");
  if (!videos || ccord_json_type_of(videos) != CCORD_JSON_ARRAY || ccord_json_array_len(videos) == 0) return false;
  const ccord_json *v0 = ccord_json_array_at(videos, 0);
  if (!v0 || ccord_json_type_of(v0) != CCORD_JSON_OBJECT) return false;

  char best_url[CCORD_VIDEO_RESOLVE_STREAM_URL_MAX] = "";
  int64_t best_br = -1;
  bool have_under = false;

  const ccord_json *formats = ccord_json_get(v0, "formats");
  if (formats && ccord_json_type_of(formats) == CCORD_JSON_ARRAY) {
    size_t nf = ccord_json_array_len(formats);
    const int64_t target_max = 3000000;
    for (size_t i = 0; i < nf; i++) {
      const ccord_json *f = ccord_json_array_at(formats, i);
      if (!f || ccord_json_type_of(f) != CCORD_JSON_OBJECT) continue;
      const char *cont = ccord_json_string(ccord_json_get(f, "container"), nullptr);
      const char *u = ccord_json_string(ccord_json_get(f, "url"), nullptr);
      if (!u || !vr_stream_url_allowed(u)) continue;
      if (cont && cont[0] && !vr_cstr_eq_ci(cont, "mp4")) continue;
      if ((!cont || !cont[0]) && !vr_is_mp4_type(nullptr, u)) continue;
      int64_t br = 0;
      const ccord_json *bj = ccord_json_get(f, "bitrate");
      if (bj) (void)ccord_json_int64(bj, &br);
      bool under = br <= target_max;
      bool take = !best_url[0] || (under && !have_under) || (under && have_under && br >= best_br) ||
                  (!under && !have_under && br < best_br);
      if (take) {
        vr_copy_str(best_url, sizeof best_url, u);
        best_br = br;
        have_under = under;
      }
    }
  }

  if (!best_url[0]) {
    const char *u = ccord_json_string(ccord_json_get(v0, "url"), nullptr);
    if (u && vr_stream_url_allowed(u)) vr_copy_str(best_url, sizeof best_url, u);
  }
  if (!best_url[0]) return false;

  vr_copy_str(out->video_url, sizeof out->video_url, best_url);
  out->audio_url[0] = '\0';
  double dur = ccord_json_number(ccord_json_get(v0, "duration"), 0.0);
  if (dur > 0.0 && dur < 4000000.0) out->duration_ms = (uint32_t)llround(dur * 1000.0);
  return true;
}

bool ccord_video_parse_fxtwitter(const char *json, size_t len, ccord_video_resolved *out)
{
  if (!json || len == 0 || !out) return false;
  ccord_arena arena;
  ccord_arena_init(&arena, 16384u);
  const ccord_json_limits lim = {.max_depth = 32u, .max_nodes = 32768u, .max_string = 1024u * 1024u};
  ccord_error err;
  const ccord_json *root = ccord_json_parse(&arena, json, len, &lim, &err);
  bool ok = false;
  if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    const ccord_json *tw = ccord_json_get(root, "tweet");
    ok = vr_parse_fxtwitter_tweet(tw, out);
    if (!ok && tw && ccord_json_type_of(tw) == CCORD_JSON_OBJECT) {
      ok = vr_parse_fxtwitter_tweet(ccord_json_get(tw, "quote"), out);
    }
  }
  ccord_arena_free(&arena);
  return ok;
}

bool ccord_video_parse_ytdlp_output(const char *text, size_t len, ccord_video_resolved *out)
{
  if (!text || len == 0 || !out) return false;
  out->video_url[0] = '\0';
  out->audio_url[0] = '\0';
  out->duration_ms = 0;

  size_t pos = 0;
  while (pos < len) {
    size_t eol = pos;
    while (eol < len && text[eol] != '\n') eol++;
    size_t s = pos, e = eol;
    while (s < e && (text[s] == ' ' || text[s] == '\t' || text[s] == '\r')) s++;
    while (e > s && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r')) e--;
    pos = eol < len ? eol + 1u : len;

    size_t ln = e - s;
    if (ln == 0) continue;
    if ((ln == 2 && vr_ascii_eq_ci(text + s, "NA", 2)) || (ln == 4 && vr_ascii_eq_ci(text + s, "None", 4))) {
      continue;
    }
    if (ln > 8 && vr_ascii_eq_ci(text + s, "https://", 8)) {
      if (ln >= CCORD_VIDEO_RESOLVE_STREAM_URL_MAX) continue;
      char u[CCORD_VIDEO_RESOLVE_STREAM_URL_MAX];
      memcpy(u, text + s, ln);
      u[ln] = '\0';
      if (!vr_stream_url_allowed(u)) continue;
      if (!out->video_url[0]) {
        memcpy(out->video_url, u, ln + 1u);
      } else if (!out->audio_url[0] && strcmp(out->video_url, u) != 0) {
        memcpy(out->audio_url, u, ln + 1u);
        break;
      }
    } else if (out->duration_ms == 0 && !out->video_url[0] && ln < 32u) {
      char num[32];
      memcpy(num, text + s, ln);
      num[ln] = '\0';
      char *endp = nullptr;
      double secs = strtod(num, &endp);
      if (endp && *endp == '\0' && isfinite(secs) && secs > 0.0 && secs < 4000000.0) {
        out->duration_ms = (uint32_t)llround(secs * 1000.0);
      }
    }
  }
  return out->video_url[0] != '\0';
}

// ---- Async worker -------------------------------------------------------------------------------------------

#if defined(CCORD_VIDEO_RESOLVE_LIVE)

#define VR_CACHE_SLOTS 16u
#define VR_CACHE_TTL_MS (5u * 60u * 1000u)
#define VR_DONE_SLOTS 8u
#define VR_HTTP_MAX_BYTES (2u * 1024u * 1024u)
#define VR_YTDLP_MAX_BYTES (64u * 1024u)
#define VR_YTDLP_TIMEOUT_MS 15000u

typedef struct vr_cache_entry {
  bool used;
  uint64_t expires_ms;
  uint32_t duration_ms;
  char page_url[CCORD_VIDEO_RESOLVE_PAGE_URL_MAX];
  char video_url[CCORD_VIDEO_RESOLVE_STREAM_URL_MAX];
  char audio_url[CCORD_VIDEO_RESOLVE_STREAM_URL_MAX];
} vr_cache_entry;

struct ccord_video_resolver {
  ccord_mutex *mx;
  ccord_cond *cv;
  ccord_thread *thr;
  bool thr_started;
  bool stop;
  _Atomic uint64_t gen;

  // Pending job (single slot: a newer click supersedes an older pending/in-flight resolve).
  bool has_job;
  uint64_t job_req_id;
  uint64_t job_gen;
  char job_url[CCORD_VIDEO_RESOLVE_PAGE_URL_MAX];

  // Completed results ring drained by poll().
  ccord_video_resolved done[VR_DONE_SLOTS];
  size_t done_head, done_count;

  // Short-TTL resolution cache (accessed under mx).
  vr_cache_entry cache[VR_CACHE_SLOTS];
  size_t cache_next;
};

typedef struct vr_http_sink {
  ccord_video_resolver *r;
  uint64_t my_gen;
  char *data;
  size_t len, cap;
  bool overflow;
} vr_http_sink;

static size_t vr_http_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
  vr_http_sink *s = ud;
  if (size != 0 && nmemb > SIZE_MAX / size) { s->overflow = true; return 0; }
  size_t n = size * nmemb;
  if (n == 0) return 0;
  if (n > VR_HTTP_MAX_BYTES || s->len > VR_HTTP_MAX_BYTES - n) { s->overflow = true; return 0; }
  if (s->len + n + 1u > s->cap) {
    size_t ncap = s->cap ? s->cap * 2u : 8192u;
    while (ncap < s->len + n + 1u) ncap *= 2u;
    if (ncap > VR_HTTP_MAX_BYTES + 1u) ncap = VR_HTTP_MAX_BYTES + 1u;
    char *nb = ccord_realloc(s->data, ncap);
    if (!nb) { s->overflow = true; return 0; }
    s->data = nb;
    s->cap = ncap;
  }
  memcpy(s->data + s->len, ptr, n);
  s->len += n;
  s->data[s->len] = '\0';
  return n;
}

static int vr_http_progress(void *ud, curl_off_t dlt, curl_off_t dln, curl_off_t ult, curl_off_t uln)
{
  (void)dlt; (void)dln; (void)ult; (void)uln;
  vr_http_sink *s = ud;
  return atomic_load_explicit(&s->r->gen, memory_order_acquire) != s->my_gen ? 1 : 0;
}

static bool vr_http_get_json(ccord_video_resolver *r, uint64_t my_gen, const char *url, char **body_out, size_t *len_out)
{
  *body_out = nullptr;
  *len_out = 0;
  if (!ccord_video_resolve_url_safe(url, CCORD_VIDEO_RESOLVE_PAGE_URL_MAX)) return false;
  ccord_curl_global_init_once();
  CURL *curl = curl_easy_init();
  if (!curl) return false;

  vr_http_sink sink = {.r = r, .my_gen = my_gen};
  struct curl_slist *hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Accept: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, ccord_live_user_agent());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vr_http_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, vr_http_progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &sink);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_NETRC, (long)CURL_NETRC_IGNORED);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  ccord_curl_apply_tls(curl);
  ccord_curl_apply_public_opensocket(curl);

  CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK || sink.overflow || code < 200 || code >= 300 || !sink.data || sink.len == 0) {
    ccord_free(sink.data);
    return false;
  }
  *body_out = sink.data;
  *len_out = sink.len;
  return true;
}

static const char k_ytdlp_fmt[] =
    "best[height>=480][height<=720][protocol=https][ext=mp4][vcodec!=none][acodec!=none]"
    "/bestvideo[height<=720][protocol=https][vcodec^=avc]+bestaudio[protocol=https][acodec^=mp4a]"
    "/bestvideo[height<=720][protocol=https][ext=mp4]+bestaudio[protocol=https][ext=m4a]"
    "/bestvideo[height<=720][protocol=https]+bestaudio[protocol=https]"
    "/best[height<=720][protocol=https][vcodec!=none][acodec!=none]"
    "/bestvideo[height<=1080][protocol=https][vcodec^=avc]+bestaudio[protocol=https][acodec^=mp4a]"
    "/best[protocol=https][vcodec!=none][acodec!=none]"
    "/best[protocol=https]";

#if !CCORD_OS_WINDOWS && !CCORD_OS_ANDROID
static void vr_set_cloexec(int fd)
{
  int fl = fcntl(fd, F_GETFD);
  if (fl >= 0) (void)fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

static bool vr_run_ytdlp(ccord_video_resolver *r, uint64_t my_gen, const char *target_url,
                         ccord_video_resolved *out, bool *missing_out)
{
  *missing_out = false;
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (pipe(out_pipe) != 0) return false;
  if (pipe(err_pipe) != 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    return false;
  }
  vr_set_cloexec(out_pipe[0]);
  vr_set_cloexec(out_pipe[1]);
  vr_set_cloexec(err_pipe[0]);
  vr_set_cloexec(err_pipe[1]);

  pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]); close(out_pipe[1]);
    close(err_pipe[0]); close(err_pipe[1]);
    return false;
  }
  if (pid == 0) {
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      (void)dup2(devnull, STDIN_FILENO);
      (void)dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) close(devnull);
    }
    (void)dup2(out_pipe[1], STDOUT_FILENO);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);

    char *const argv[] = {
      (char *)(uintptr_t)"yt-dlp",
      (char *)(uintptr_t)"--no-playlist",
      (char *)(uintptr_t)"--no-config",
      (char *)(uintptr_t)"--no-warnings",
      (char *)(uintptr_t)"--no-color",
      (char *)(uintptr_t)"--socket-timeout",
      (char *)(uintptr_t)"10",
      (char *)(uintptr_t)"-f",
      (char *)(uintptr_t)k_ytdlp_fmt,
      (char *)(uintptr_t)"--print",
      (char *)(uintptr_t)"%(duration)s",
      (char *)(uintptr_t)"--print",
      (char *)(uintptr_t)"%(urls)s",
      (char *)(uintptr_t)"--",
      (char *)(uintptr_t)target_url,
      nullptr,
    };
    execvp("yt-dlp", argv);
    int e = errno;
    (void)write(err_pipe[1], &e, sizeof e);
    _exit(127);
  }

  close(out_pipe[1]);
  close(err_pipe[1]);

  // Wait for exec outcome (EOF on err_pipe means execvp succeeded).
  int exec_err = 0;
  ssize_t er = read(err_pipe[0], &exec_err, sizeof exec_err);
  close(err_pipe[0]);
  if (er == (ssize_t)sizeof exec_err) {
    close(out_pipe[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (exec_err == ENOENT || exec_err == EACCES) *missing_out = true;
    return false;
  }

  int fl = fcntl(out_pipe[0], F_GETFL);
  if (fl >= 0) (void)fcntl(out_pipe[0], F_SETFL, fl | O_NONBLOCK);

  char *buf = ccord_malloc(VR_YTDLP_MAX_BYTES + 1u);
  size_t len = 0;
  bool aborted = !buf;
  uint64_t deadline = ccord_clock_ms() + VR_YTDLP_TIMEOUT_MS;

  while (!aborted) {
    if (atomic_load_explicit(&r->gen, memory_order_acquire) != my_gen || ccord_clock_ms() >= deadline) {
      aborted = true;
      break;
    }
    struct pollfd pfd = {.fd = out_pipe[0], .events = POLLIN};
    int pr = poll(&pfd, 1, 50);
    if (pr < 0) {
      if (errno == EINTR) continue;
      aborted = true;
      break;
    }
    if (pr == 0) continue;
    if (pfd.revents & (POLLIN | POLLHUP)) {
      ssize_t rd = read(out_pipe[0], buf + len, VR_YTDLP_MAX_BYTES - len);
      if (rd > 0) {
        len += (size_t)rd;
        if (len >= VR_YTDLP_MAX_BYTES) { aborted = true; break; }
      } else if (rd == 0) {
        break;
      } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        aborted = true;
        break;
      }
    } else if (pfd.revents & (POLLERR | POLLNVAL)) {
      aborted = true;
      break;
    }
  }
  close(out_pipe[0]);

  if (aborted) (void)kill(pid, SIGKILL);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

  bool ok = false;
  if (!aborted && buf && WIFEXITED(status) && WEXITSTATUS(status) == 0 && len > 0) {
    buf[len] = '\0';
    ok = ccord_video_parse_ytdlp_output(buf, len, out);
  }
  ccord_free(buf);
  return ok;
}
#elif CCORD_OS_WINDOWS
static bool vr_run_ytdlp(ccord_video_resolver *r, uint64_t my_gen, const char *target_url,
                         ccord_video_resolved *out, bool *missing_out)
{
  *missing_out = false;
  // Reject quotes/backslashes so command-line quoting cannot be escaped on Windows.
  for (const char *p = target_url; *p; p++) {
    if (*p == '"' || *p == '\\' || (unsigned char)*p <= 0x20u || (unsigned char)*p == 0x7fu) return false;
  }
  SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
  HANDLE rd_h = nullptr, wr_h = nullptr;
  if (!CreatePipe(&rd_h, &wr_h, &sa, 0)) return false;
  (void)SetHandleInformation(rd_h, HANDLE_FLAG_INHERIT, 0);

  HANDLE nul_h = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  char cmd[4096];
  int cn = snprintf(cmd, sizeof cmd,
                    "yt-dlp --no-playlist --no-config --no-warnings --no-color --socket-timeout 10 "
                    "-f \"%s\" --print \"%%(duration)s\" --print \"%%(urls)s\" -- \"%s\"",
                    k_ytdlp_fmt, target_url);
  if (cn <= 0 || (size_t)cn >= sizeof cmd) {
    CloseHandle(rd_h); CloseHandle(wr_h);
    if (nul_h != INVALID_HANDLE_VALUE) CloseHandle(nul_h);
    return false;
  }

  STARTUPINFOA si;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr_h;
  si.hStdInput = nul_h != INVALID_HANDLE_VALUE ? nul_h : GetStdHandle(STD_INPUT_HANDLE);
  si.hStdError = nul_h != INVALID_HANDLE_VALUE ? nul_h : GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof pi);
  BOOL ok_proc = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  DWORD create_err = ok_proc ? 0 : GetLastError();
  CloseHandle(wr_h);
  if (nul_h != INVALID_HANDLE_VALUE) CloseHandle(nul_h);
  if (!ok_proc) {
    CloseHandle(rd_h);
    if (create_err == ERROR_FILE_NOT_FOUND || create_err == ERROR_PATH_NOT_FOUND) *missing_out = true;
    return false;
  }

  char *buf = ccord_malloc(VR_YTDLP_MAX_BYTES + 1u);
  size_t len = 0;
  bool aborted = !buf;
  uint64_t deadline = ccord_clock_ms() + VR_YTDLP_TIMEOUT_MS;
  while (!aborted) {
    if (atomic_load_explicit(&r->gen, memory_order_acquire) != my_gen || ccord_clock_ms() >= deadline) {
      aborted = true;
      break;
    }
    DWORD avail = 0;
    if (PeekNamedPipe(rd_h, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      DWORD want = (DWORD)(VR_YTDLP_MAX_BYTES - len);
      if (avail < want) want = avail;
      DWORD got = 0;
      if (ReadFile(rd_h, buf + len, want, &got, nullptr) && got > 0) {
        len += (size_t)got;
        if (len >= VR_YTDLP_MAX_BYTES) { aborted = true; break; }
      }
      continue;
    }
    if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
      for (;;) {
        DWORD got = 0;
        if (len >= VR_YTDLP_MAX_BYTES || !ReadFile(rd_h, buf + len, (DWORD)(VR_YTDLP_MAX_BYTES - len), &got, nullptr) ||
            got == 0) {
          break;
        }
        len += (size_t)got;
      }
      break;
    }
  }
  CloseHandle(rd_h);
  if (aborted) (void)TerminateProcess(pi.hProcess, 1);
  (void)WaitForSingleObject(pi.hProcess, 2000);
  DWORD code = 1;
  (void)GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  bool ok = false;
  if (!aborted && buf && code == 0 && len > 0) {
    buf[len] = '\0';
    ok = ccord_video_parse_ytdlp_output(buf, len, out);
  }
  ccord_free(buf);
  return ok;
}
#else
static bool vr_run_ytdlp(ccord_video_resolver *r, uint64_t my_gen, const char *target_url,
                         ccord_video_resolved *out, bool *missing_out)
{
  (void)r; (void)my_gen; (void)target_url; (void)out;
  *missing_out = true;
  return false;
}
#endif

static void vr_resolve_one(ccord_video_resolver *r, uint64_t my_gen, const char *url, ccord_video_resolved *res)
{
  res->status = CCORD_ERR_IO;
  res->video_url[0] = '\0';
  res->audio_url[0] = '\0';
  res->duration_ms = 0;
  res->missing_helper = false;
  res->error[0] = '\0';

  if (!ccord_video_resolve_url_safe(url, CCORD_VIDEO_RESOLVE_PAGE_URL_MAX)) {
    res->status = CCORD_ERR_PERMISSION;
    snprintf(res->error, sizeof res->error, "invalid or blocked video page URL");
    return;
  }

  char tweet_id[32];
  if (ccord_video_url_is_twitter(url, tweet_id, sizeof tweet_id)) {
    // 1. Native Twitter public syndication API (zero external dependencies, fast).
    char tok[32];
    if (ccord_video_twitter_syndication_token(tweet_id, tok, sizeof tok)) {
      char api_url[256];
      snprintf(api_url, sizeof api_url,
               "https://cdn.syndication.twimg.com/tweet-result?id=%s&lang=en&token=%s", tweet_id, tok);
      char *body = nullptr;
      size_t blen = 0;
      if (vr_http_get_json(r, my_gen, api_url, &body, &blen)) {
        bool ok = ccord_video_parse_twitter_syndication(body, blen, res);
        ccord_free(body);
        if (ok) { res->status = CCORD_OK; return; }
      }
    }
    if (atomic_load_explicit(&r->gen, memory_order_acquire) != my_gen) return;

    // 2. Try yt-dlp with canonical x.com status URL.
    char canon[128];
    snprintf(canon, sizeof canon, "https://x.com/i/status/%s", tweet_id);
    bool missing = false;
    if (vr_run_ytdlp(r, my_gen, canon, res, &missing)) {
      res->status = CCORD_OK;
      return;
    }
    if (atomic_load_explicit(&r->gen, memory_order_acquire) != my_gen) return;

    // 3. Fallback to public FxTwitter JSON API (useful when yt-dlp is absent or syndication omits the video).
    char fx_url[128];
    snprintf(fx_url, sizeof fx_url, "https://api.fxtwitter.com/i/status/%s", tweet_id);
    char *body = nullptr;
    size_t blen = 0;
    if (vr_http_get_json(r, my_gen, fx_url, &body, &blen)) {
      bool ok = ccord_video_parse_fxtwitter(body, blen, res);
      ccord_free(body);
      if (ok) { res->status = CCORD_OK; return; }
    }
    snprintf(res->error, sizeof res->error, "no playable video found in X/Twitter post");
    return;
  }

  // Canonicalize YouTube /embed/ URLs to /watch?v= so yt-dlp always receives a clean watch page.
  char yt_id[32];
  char target[CCORD_VIDEO_RESOLVE_PAGE_URL_MAX];
  if (ccord_video_url_is_youtube(url, yt_id, sizeof yt_id)) {
    snprintf(target, sizeof target, "https://www.youtube.com/watch?v=%s", yt_id);
  } else {
    vr_copy_str(target, sizeof target, url);
  }

  bool missing = false;
  if (vr_run_ytdlp(r, my_gen, target, res, &missing)) {
    res->status = CCORD_OK;
    return;
  }
  res->missing_helper = missing;
  if (missing) {
    res->status = CCORD_ERR_UNSUPPORTED;
    snprintf(res->error, sizeof res->error, "yt-dlp is not installed on PATH");
  } else {
    res->status = CCORD_ERR_IO;
    snprintf(res->error, sizeof res->error, "could not resolve stream URL");
  }
}

static void vr_push_done_locked(ccord_video_resolver *r, const ccord_video_resolved *res)
{
  if (r->done_count == VR_DONE_SLOTS) {
    r->done_head = (r->done_head + 1u) % VR_DONE_SLOTS;
    r->done_count--;
  }
  r->done[(r->done_head + r->done_count) % VR_DONE_SLOTS] = *res;
  r->done_count++;
}

static void vr_worker_fn(void *arg)
{
  ccord_video_resolver *r = arg;
  for (;;) {
    ccord_mutex_lock(r->mx);
    while (!r->stop && !r->has_job) ccord_cond_wait(r->cv, r->mx);
    if (r->stop) {
      ccord_mutex_unlock(r->mx);
      return;
    }
    uint64_t req_id = r->job_req_id;
    uint64_t my_gen = r->job_gen;
    char url[CCORD_VIDEO_RESOLVE_PAGE_URL_MAX];
    vr_copy_str(url, sizeof url, r->job_url);
    r->has_job = false;
    ccord_mutex_unlock(r->mx);

    ccord_video_resolved res;
    memset(&res, 0, sizeof res);
    res.req_id = req_id;
    vr_copy_str(res.page_url, sizeof res.page_url, url);
    vr_resolve_one(r, my_gen, url, &res);

    ccord_mutex_lock(r->mx);
    if (!r->stop && atomic_load_explicit(&r->gen, memory_order_acquire) == my_gen) {
      if (res.status == CCORD_OK && res.video_url[0]) {
        vr_cache_entry *ce = &r->cache[r->cache_next % VR_CACHE_SLOTS];
        r->cache_next++;
        ce->used = true;
        ce->expires_ms = ccord_clock_ms() + VR_CACHE_TTL_MS;
        ce->duration_ms = res.duration_ms;
        vr_copy_str(ce->page_url, sizeof ce->page_url, url);
        vr_copy_str(ce->video_url, sizeof ce->video_url, res.video_url);
        vr_copy_str(ce->audio_url, sizeof ce->audio_url, res.audio_url);
      }
      vr_push_done_locked(r, &res);
    }
    ccord_mutex_unlock(r->mx);
  }
}

ccord_video_resolver *ccord_video_resolver_create(void)
{
  ccord_video_resolver *r = ccord_calloc(1, sizeof *r);
  if (!r) return nullptr;
  r->mx = ccord_mutex_create();
  r->cv = r->mx ? ccord_cond_create() : nullptr;
  if (!r->mx || !r->cv) {
    ccord_cond_destroy(r->cv);
    ccord_mutex_destroy(r->mx);
    ccord_free(r);
    return nullptr;
  }
  atomic_init(&r->gen, 1u);
  return r;
}

void ccord_video_resolver_destroy(ccord_video_resolver *r)
{
  if (!r) return;
  atomic_fetch_add_explicit(&r->gen, 1u, memory_order_acq_rel);
  ccord_mutex_lock(r->mx);
  r->stop = true;
  ccord_cond_broadcast(r->cv);
  ccord_mutex_unlock(r->mx);
  if (r->thr_started && r->thr) ccord_thread_join(r->thr);
  ccord_cond_destroy(r->cv);
  ccord_mutex_destroy(r->mx);
  ccord_free(r);
}

ccord_status ccord_video_resolver_submit(ccord_video_resolver *r, uint64_t req_id, const char *url, ccord_error *err)
{
  ccord_error_clear(err);
  if (!r || !url || !url[0]) return ccord_error_set(err, CCORD_ERR_INVALID, "video_resolve: invalid argument");
  if (!ccord_video_resolve_url_safe(url, CCORD_VIDEO_RESOLVE_PAGE_URL_MAX)) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "video_resolve: URL blocked");
  }

  uint64_t now = ccord_clock_ms();
  uint64_t next_gen = atomic_fetch_add_explicit(&r->gen, 1u, memory_order_acq_rel) + 1u;

  ccord_mutex_lock(r->mx);
  for (size_t i = 0; i < VR_CACHE_SLOTS; i++) {
    const vr_cache_entry *ce = &r->cache[i];
    if (ce->used && now < ce->expires_ms && strcmp(ce->page_url, url) == 0) {
      ccord_video_resolved hit;
      memset(&hit, 0, sizeof hit);
      hit.req_id = req_id;
      hit.status = CCORD_OK;
      hit.duration_ms = ce->duration_ms;
      vr_copy_str(hit.page_url, sizeof hit.page_url, ce->page_url);
      vr_copy_str(hit.video_url, sizeof hit.video_url, ce->video_url);
      vr_copy_str(hit.audio_url, sizeof hit.audio_url, ce->audio_url);
      r->has_job = false;
      vr_push_done_locked(r, &hit);
      ccord_mutex_unlock(r->mx);
      return CCORD_OK;
    }
  }
  if (!r->thr_started) {
    r->thr = ccord_thread_create(vr_worker_fn, r);
    if (!r->thr) {
      ccord_mutex_unlock(r->mx);
      return ccord_error_set(err, CCORD_ERR_INTERNAL, "video_resolve: cannot start worker thread");
    }
    r->thr_started = true;
  }
  r->has_job = true;
  r->job_req_id = req_id;
  r->job_gen = next_gen;
  vr_copy_str(r->job_url, sizeof r->job_url, url);
  ccord_cond_signal(r->cv);
  ccord_mutex_unlock(r->mx);
  return CCORD_OK;
}

bool ccord_video_resolver_poll(ccord_video_resolver *r, ccord_video_resolved *out)
{
  if (!r || !out) return false;
  ccord_mutex_lock(r->mx);
  if (r->done_count == 0) {
    ccord_mutex_unlock(r->mx);
    return false;
  }
  *out = r->done[r->done_head];
  r->done_head = (r->done_head + 1u) % VR_DONE_SLOTS;
  r->done_count--;
  ccord_mutex_unlock(r->mx);
  return true;
}

#else

ccord_video_resolver *ccord_video_resolver_create(void) { return nullptr; }
void ccord_video_resolver_destroy(ccord_video_resolver *r) { (void)r; }
ccord_status ccord_video_resolver_submit(ccord_video_resolver *r, uint64_t req_id, const char *url, ccord_error *err)
{
  (void)r; (void)req_id; (void)url;
  return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "video_resolve: offline build");
}
bool ccord_video_resolver_poll(ccord_video_resolver *r, ccord_video_resolved *out)
{
  (void)r; (void)out;
  return false;
}

#endif
