// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Shared offline-safe network helpers: poll body builder, media cache path, raw gateway tap flag,
// stream key codec, client identity headers, and secure credential wipe.
#define _POSIX_C_SOURCE 200809L
#include <ccord/net/live_transport.h>
#include <ccord/net/profile_edit.h>
#include <ccord/net/transport.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/base64.h>
#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

// ---- secure wipe ----

void ccord_live_wipe(void *p, size_t n)
{
  ccord_secure_wipe(p, n);
}

// ---- raw gateway tap toggle ----

static _Atomic bool s_gw_raw_tap = false;

void ccord_gateway_set_raw_tap(bool enabled)
{
  atomic_store(&s_gw_raw_tap, enabled);
}

bool ccord_gateway_raw_tap_enabled(void)
{
  return atomic_load(&s_gw_raw_tap);
}

// ---- Discord client identity headers ----

const char *ccord_live_super_properties(void)
{
  static char encoded[1024];
  static bool ready;
  if (!ready) {
    static const char json[] =
      "{\"os\":\"Linux\",\"browser\":\"Discord Client\",\"release_channel\":\"stable\","
      "\"client_version\":\"1.0.171\",\"os_version\":\"6.12.0\",\"os_arch\":\"x64\","
      "\"app_arch\":\"x64\",\"system_locale\":\"en-US\",\"has_client_mods\":false,"
      "\"browser_user_agent\":\"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) discord/1.0.171 Chrome/138.0.7204.251 Electron/37.6.0 Safari/537.36\","
      "\"browser_version\":\"37.6.0\",\"os_version_is_release\":true,"
      "\"client_build_number\":681625,\"client_event_source\":null}";
    (void)ccord_base64_encode((const uint8_t *)json, sizeof json - 1u, encoded, sizeof encoded);
    ready = true;
  }
  return encoded;
}

const char *ccord_live_user_agent(void)
{
  return "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.171 Chrome/138.0.7204.251 Electron/37.6.0 Safari/537.36";
}

static bool tz_iana_valid(const char *s, size_t n)
{
  if (!s || n == 0 || n > 63u) return false;
  if (n == 3u && memcmp(s, "UTC", 3) == 0) return true;
  if (s[0] == '/' || s[n - 1u] == '/') return false;
  bool has_slash = false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '/' || c == '_' || c == '-' || c == '+';
    if (!ok) return false;
    if (c == '/') has_slash = true;
  }
  return has_slash;
}

static bool tz_copy_if_valid(const char *src, size_t len, char *out, size_t cap)
{
  while (len > 0 && (src[len - 1u] == '\n' || src[len - 1u] == '\r' || src[len - 1u] == ' ' || src[len - 1u] == '\t')) {
    len--;
  }
  if (!tz_iana_valid(src, len) || len >= cap) return false;
  memcpy(out, src, len);
  out[len] = '\0';
  return true;
}

void ccord_live_timezone(char *out, size_t cap)
{
  if (!out || cap == 0) return;
  const char *env_tz = getenv("TZ");
  if (env_tz && env_tz[0] != '\0') {
    const char *p = env_tz[0] == ':' ? env_tz + 1 : env_tz;
    const char *zi = strstr(p, "zoneinfo/");
    if (zi) p = zi + 9;
    if (tz_copy_if_valid(p, strlen(p), out, cap)) return;
  }
#if !defined(_WIN32)
  char linkbuf[256];
  ssize_t lr = readlink("/etc/localtime", linkbuf, sizeof linkbuf - 1u);
  if (lr > 0) {
    linkbuf[(size_t)lr] = '\0';
    const char *zi = strstr(linkbuf, "zoneinfo/");
    if (zi && tz_copy_if_valid(zi + 9, strlen(zi + 9), out, cap)) return;
  }
  FILE *f = fopen("/etc/timezone", "r");
  if (f) {
    char line[128];
    bool got = fgets(line, (int)sizeof line, f) != nullptr;
    fclose(f);
    if (got && tz_copy_if_valid(line, strlen(line), out, cap)) return;
  }
#endif
  (void)snprintf(out, cap, "UTC");
}

// ---- Go Live stream key format / parse ----

void ccord_stream_key_format(ccord_id guild_id, ccord_id channel_id, ccord_id user_id, char out[96])
{
  if (!out) return;
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    (void)snprintf(out, 96, "call:%llu:%llu", (unsigned long long)channel_id, (unsigned long long)user_id);
  } else {
    (void)snprintf(out, 96, "guild:%llu:%llu:%llu", (unsigned long long)guild_id, (unsigned long long)channel_id,
                   (unsigned long long)user_id);
  }
}

static bool parse_u64_segment(const char **p, char delim, ccord_id *out)
{
  const char *s = *p;
  if (!s || *s < '0' || *s > '9') return false;
  uint64_t val = 0;
  while (*s >= '0' && *s <= '9') {
    val = val * 10u + (uint64_t)(*s - '0');
    s++;
  }
  if (*s != delim) return false;
  if (delim != '\0') s++;
  *p = s;
  if (out) *out = (ccord_id)val;
  return true;
}

bool ccord_stream_key_parse(const char *stream_key, ccord_id *out_guild_id, ccord_id *out_channel_id,
                            ccord_id *out_user_id)
{
  if (out_guild_id) *out_guild_id = CCORD_ID_NONE;
  if (out_channel_id) *out_channel_id = CCORD_ID_NONE;
  if (out_user_id) *out_user_id = CCORD_ID_NONE;
  if (!stream_key) return false;
  if (strncmp(stream_key, "guild:", 6) == 0) {
    const char *p = stream_key + 6;
    ccord_id gid = 0, cid = 0, uid = 0;
    if (!parse_u64_segment(&p, ':', &gid)) return false;
    if (!parse_u64_segment(&p, ':', &cid)) return false;
    if (!parse_u64_segment(&p, '\0', &uid)) return false;
    if (out_guild_id) *out_guild_id = gid;
    if (out_channel_id) *out_channel_id = cid;
    if (out_user_id) *out_user_id = uid;
    return true;
  }
  if (strncmp(stream_key, "call:", 5) == 0) {
    const char *p = stream_key + 5;
    ccord_id cid = 0, uid = 0;
    if (!parse_u64_segment(&p, ':', &cid)) return false;
    if (!parse_u64_segment(&p, '\0', &uid)) return false;
    if (out_guild_id) *out_guild_id = CCORD_DM_GUILD_ID;
    if (out_channel_id) *out_channel_id = cid;
    if (out_user_id) *out_user_id = uid;
    return true;
  }
  return false;
}

// ---- Poll create JSON builder (shared by live transport, mock transport, and UI poll composer) ----

static size_t poll_codepoints(const char *s)
{
  size_t n = 0;
  for (; *s; s++) {
    if (((unsigned char)*s & 0xC0u) != 0x80u) n++;
  }
  return n;
}

// Checks one poll text: present, valid UTF-8 without NUL, 1..max code points.
static ccord_status poll_text_check(const char *text, size_t max_chars, const char *what, ccord_error *err)
{
  if (!text || text[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "poll: %s is empty", what);
  if (!ccord_utf8_valid_text(text, strlen(text))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "poll: %s is not valid text", what);
  }
  if (poll_codepoints(text) > max_chars) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "poll: %s is longer than %zu characters", what, max_chars);
  }
  return CCORD_OK;
}

ccord_status ccord_poll_build_body(const ccord_poll_create *poll, ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!poll || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "poll: missing poll");
  ccord_status st = poll_text_check(poll->question, CCORD_POLL_QUESTION_MAX, "question", err);
  if (st != CCORD_OK) return st;
  if (!poll->answers || poll->answer_count < CCORD_POLL_MIN_ANSWERS) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "poll: needs at least %u answers", CCORD_POLL_MIN_ANSWERS);
  }
  if (poll->answer_count > CCORD_RICH_MAX_POLL_ANSWERS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "poll: at most %u answers", CCORD_RICH_MAX_POLL_ANSWERS);
  }
  if (poll->duration_hours < 1u || poll->duration_hours > 768u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "poll: duration must be 1 hour to 32 days");
  }
  for (size_t i = 0; i < poll->answer_count; i++) {
    st = poll_text_check(poll->answers[i], CCORD_POLL_ANSWER_MAX, "answer", err);
    if (st != CCORD_OK) return st;
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "poll");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "question");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "text");
  ccord_jw_cstr(&w, poll->question);
  ccord_jw_end_object(&w);
  ccord_jw_key(&w, "answers");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < poll->answer_count; i++) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "poll_media");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "text");
    ccord_jw_cstr(&w, poll->answers[i]);
    ccord_jw_end_object(&w);
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  ccord_jw_key(&w, "duration");
  ccord_jw_int(&w, (int64_t)poll->duration_hours);
  ccord_jw_key(&w, "allow_multiselect");
  ccord_jw_bool(&w, poll->allow_multiselect);
  ccord_jw_key(&w, "layout_type");
  ccord_jw_int(&w, 1);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    return ccord_error_set(err, out->failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "poll: json failed");
  }
  return CCORD_OK;
}

// ---- Media disk cache path ----

ccord_status ccord_media_cache_path(const char *url, char *out_path, size_t cap, ccord_error *err)
{
  ccord_error_clear(err);
  if (!url || url[0] == '\0' || !out_path || cap == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "media: invalid cache path arguments");
  }
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
    hash ^= (uint64_t)(*p);
    hash *= 0x100000001b3ULL;
  }
  const char *ext = ".bin";
  size_t qpos = strcspn(url, "?#");
  for (size_t i = qpos; i > 0; i--) {
    if (url[i - 1] == '.') {
      size_t elen = qpos - (i - 1);
      if (elen == 4 && (strncmp(url + i - 1, ".png", 4) == 0 || strncmp(url + i - 1, ".PNG", 4) == 0)) ext = ".png";
      else if (elen == 4 && (strncmp(url + i - 1, ".jpg", 4) == 0 || strncmp(url + i - 1, ".JPG", 4) == 0)) ext = ".jpg";
      else if (elen == 5 && (strncmp(url + i - 1, ".jpeg", 5) == 0 || strncmp(url + i - 1, ".JPEG", 5) == 0)) ext = ".jpeg";
      else if (elen == 5 && (strncmp(url + i - 1, ".webp", 5) == 0 || strncmp(url + i - 1, ".WEBP", 5) == 0)) ext = ".webp";
      else if (elen == 4 && (strncmp(url + i - 1, ".gif", 4) == 0 || strncmp(url + i - 1, ".GIF", 4) == 0)) ext = ".gif";
      break;
    }
    if (url[i - 1] == '/') break;
  }
  char hex[17];
  static const char k_hex[] = "0123456789abcdef";
  for (int i = 15; i >= 0; i--) {
    hex[i] = k_hex[hash & 0xFu];
    hash >>= 4u;
  }
  hex[16] = '\0';

  const char *xdg = getenv("XDG_CACHE_HOME");
  const char *base = nullptr;
  const char *mid = nullptr;
  if (xdg && xdg[0] != '\0') {
    base = xdg;
    mid = "/ccord/media/";
  } else {
    base = getenv("HOME");
    if (!base || base[0] == '\0') base = "/tmp";
    mid = "/.cache/ccord/media/";
  }
  size_t blen = strlen(base);
  while (blen > 1 && base[blen - 1] == '/') blen--;
  size_t mlen = strlen(mid);
  size_t elen = strlen(ext);
  if (blen + mlen + 16u + elen + 1u > cap) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "media: cache path buffer too small");
  }
  memcpy(out_path, base, blen);
  memcpy(out_path + blen, mid, mlen);
  memcpy(out_path + blen + mlen, hex, 16u);
  memcpy(out_path + blen + mlen + 16u, ext, elen + 1u);
  return CCORD_OK;
}

// ---- Attachment URL refresh JSON codec ----

ccord_status ccord_live_media_refresh_body(const char *url, ccord_buf *out, ccord_error *err)
{
  if (!url || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "live: refresh body arguments");
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "attachment_urls");
  ccord_jw_begin_array(&w);
  ccord_jw_string(&w, url, strlen(url));
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: refresh body not encodable");
  return CCORD_OK;
}

bool ccord_live_media_refresh_parse(const char *json, size_t len, const char *original, char *out, size_t cap)
{
  if (!json || len == 0 || !out || cap == 0) return false;
  out[0] = '\0';
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  ccord_error jerr;
  ccord_json_limits lim = {.max_depth = 6, .max_nodes = 64, .max_string = 4096};
  const ccord_json *root = ccord_json_parse(&tmp, json, len, &lim, &jerr);
  bool ok = false;
  const ccord_json *arr = root && ccord_json_type_of(root) == CCORD_JSON_OBJECT ? ccord_json_get(root, "refreshed_urls") : nullptr;
  size_t n = arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY ? ccord_json_array_len(arr) : 0;
  for (size_t i = 0; i < n && !ok; i++) {
    const ccord_json *it = ccord_json_array_at(arr, i);
    if (!it || ccord_json_type_of(it) != CCORD_JSON_OBJECT) continue;
    size_t ol = 0, rl = 0;
    const ccord_json *o = ccord_json_get(it, "original");
    const ccord_json *r = ccord_json_get(it, "refreshed");
    const char *os = o ? ccord_json_string(o, &ol) : nullptr;
    const char *rs = r ? ccord_json_string(r, &rl) : nullptr;
    if (!rs || rl == 0 || rl >= cap) continue;
    if (n > 1u && original && os && strcmp(os, original) != 0) continue;
    memcpy(out, rs, rl);
    out[rl] = '\0';
    ok = true;
  }
  ccord_arena_free(&tmp);
  return ok;
}

// ---- Image magic-byte MIME sniffer (shared by UI profile settings & profile_edit) ----

const char *ccord_profile_image_mime(const uint8_t *d, size_t len)
{
  if (!d) return nullptr;
  if (len >= 8u && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
  if (len >= 3u && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "image/jpeg";
  if (len >= 6u && (memcmp(d, "GIF87a", 6) == 0 || memcmp(d, "GIF89a", 6) == 0)) return "image/gif";
  if (len >= 12u && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) return "image/webp";
  return nullptr;
}

