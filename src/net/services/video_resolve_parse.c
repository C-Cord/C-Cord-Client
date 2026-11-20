// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/net/video_resolve.h>

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/json.h>
#include <ccord/net/discord_rest.h>


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

