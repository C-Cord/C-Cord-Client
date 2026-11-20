// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Pure URL / route / CDN allowlist policy (no libcurl or network dependencies).
#include <ccord/net/discord_rest.h>

#include <string.h>

static bool ascii_eq_ci(const char *a, const char *b, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

static bool forbidden_segment(const char *s, size_t n)
{
  static const char *const words[] = {
    "store", "stores", "payment", "payments", "billing", "billings",
    "subscription", "subscriptions", "purchase", "purchases", "checkout",
    "gift", "gifts", "nitro",
  };
  for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
    size_t w = strlen(words[i]);
    if (w == n && ascii_eq_ci(s, words[i], n)) return true;
  }
  return false;
}

bool ccord_live_rest_path_allowed(const char *path)
{
  if (!path || path[0] != '/') return false;
  size_t i = 1;
  while (path[i] != '\0') {
    while (path[i] == '/') i++;
    size_t start = i;
    while (path[i] != '\0' && path[i] != '/' && path[i] != '?' && path[i] != '#') i++;
    if (forbidden_segment(path + start, i - start)) return false;
    if (path[i] == '?' || path[i] == '#') {
      // Query strings cannot smuggle a forbidden route: the path itself is checked above.
      break;
    }
  }
  return true;
}

static bool allowed_media_host(const char *host, size_t n)
{
  static const char *const hosts[] = {
    "cdn.discordapp.com",
    "media.discordapp.net",
    "media.discordapp.com",
    // Discord's external-media proxy: every embed image/thumbnail/video and GIF-picker cover is served from here.
    "images-ext-1.discordapp.net",
    "images-ext-2.discordapp.net",
    // GIF providers behind Discord's picker (Klipy today, Tenor before it).
    "static.klipy.com",
    "media.klipy.com",
    "media.tenor.com",
    "media1.tenor.com",
    "media2.tenor.com",
    "c.tenor.com",
    // Third-party GIF providers the official client also loads (exact hosts only; no wildcard or suffix rule).
    "media.giphy.com",
    "media0.giphy.com",
    "media1.giphy.com",
    "media2.giphy.com",
    "media3.giphy.com",
    "media4.giphy.com",
    "i.giphy.com",
    "i.imgur.com",
    "media.tenor.co",
  };
  for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
    size_t hlen = strlen(hosts[i]);
    if (hlen == n && ascii_eq_ci(host, hosts[i], n)) return true;
  }
  return false;
}

bool ccord_live_media_url_allowed(const char *url)
{
  if (!url || !ascii_eq_ci(url, "https://", 8)) return false;
  const char *host = url + 8;
  const char *slash = strchr(host, '/');
  if (!slash || slash == host) return false;
  if (slash[1] == '\0' || slash[1] == '?' || slash[1] == '#') return false;
  const char *host_end = slash;
  const char *colon = memchr(host, ':', (size_t)(slash - host));
  if (colon) {
    if (colon == host || colon + 1 == slash) return false;
    for (const char *p = colon + 1; p < slash; p++) {
      if (*p < '0' || *p > '9') return false;
    }
    host_end = colon;
  }
  if (!allowed_media_host(host, (size_t)(host_end - host))) return false;
  return ccord_live_rest_path_allowed(slash);
}

// A DNS host name for a user-clicked video: letters, digits, '-' and '.', every label non-empty, at least one dot,
// a last label that is not all digits (so no dotted-quad or shorthand IPv4 literal), and not localhost / .local.
static bool user_video_host_ok(const char *host, size_t n)
{
  if (n == 0 || n > 253) return false;
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
    if (++label > 63) return false;
  }
  if (!dot || label == 0 || last_numeric) return false;
  static const char *const local[] = {"localhost", ".localhost", ".local"};
  for (size_t i = 0; i < sizeof local / sizeof local[0]; i++) {
    size_t ln = strlen(local[i]);
    if (n >= ln && ascii_eq_ci(host + (n - ln), local[i], ln) && (local[i][0] == '.' || n == ln)) return false;
  }
  return true;
}

bool ccord_live_user_video_url_allowed(const char *url)
{
  if (ccord_live_media_url_allowed(url)) return true;
  if (!url || !ascii_eq_ci(url, "https://", 8)) return false;
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
  if (!user_video_host_ok(host, (size_t)(host_end - host))) return false;
  return ccord_live_rest_path_allowed(slash);
}

ccord_status ccord_live_rest_build_url(const char *base_url, const char *path, ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !path || path[0] != '/') return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad path");
  const char *base = base_url && base_url[0] ? base_url : CCORD_LIVE_API_BASE;
  size_t n = strlen(base);
  while (n > 0 && base[n - 1] == '/') n--;
  if (n == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad base URL");
  if (ccord_buf_append(out, base, n) != CCORD_OK) return ccord_error_set(err, CCORD_ERR_RANGE, "rest: URL too long");
  if (ccord_buf_append_str(out, path) != CCORD_OK) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "rest: URL too long");
  }
  return CCORD_OK;
}


bool ccord_live_media_url_refreshable(const char *url)
{
  if (!url || !ccord_live_media_url_allowed(url)) return false;
  const char *host = url + 8;
  size_t hn = strcspn(host, "/");
  static const char *const hosts[] = {"cdn.discordapp.com", "media.discordapp.net", "media.discordapp.com"};
  bool ok = false;
  for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
    if (strlen(hosts[i]) == hn && strncmp(host, hosts[i], hn) == 0) ok = true;
  }
  if (!ok || strncmp(host + hn, "/attachments/", 13) != 0) return false;
  const char *q = strchr(host + hn, '?');
  return q && (strncmp(q, "?ex=", 4) == 0 || strstr(q, "&ex=") != nullptr);
}

