// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/net/live_transport.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/idmap.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>
#include <ccord/model/rich.h>

#define LIVE_JSON_MAX_DEPTH 32u
#define LIVE_JSON_MAX_NODES 2500000u
#define LIVE_JSON_MAX_STRING (1024u * 1024u)
#define LIVE_STATUS_MAX_BYTES 128u
#define LIVE_TOPIC_MAX_BYTES 1024u

void ccord_live_event_init(ccord_live_event *ev)
{
  memset(ev, 0, sizeof *ev);
  ccord_arena_init(&ev->arena, 0);
  ev->ev.type = CCORD_NET_NONE;
  ev->seq = -1;
}

void ccord_live_event_free(ccord_live_event *ev)
{
  if (!ev) return;
  ccord_arena_free(&ev->arena);
  memset(ev, 0, sizeof *ev);
  ev->seq = -1;
}

static size_t scratch_chunk_size(size_t json_len)
{
  if (json_len > 65536u) return 256u * 1024u;
  if (json_len > 4096u) return 16u * 1024u;
  return 4096u;
}

static const ccord_json *parse_json(ccord_arena *a, const char *text, size_t len, ccord_error *err)
{
  ccord_json_limits lim = {LIVE_JSON_MAX_DEPTH, LIVE_JSON_MAX_NODES, LIVE_JSON_MAX_STRING};
  return ccord_json_parse(a, text, len, &lim, err);
}

static const char *arena_dup(ccord_arena *a, const char *s)
{
  if (!s || s[0] == '\0' || !a) return "";
  char *d = ccord_arena_strndup(a, s, strlen(s));
  return d ? d : "";
}

static const char *arena_dup_nullable(ccord_arena *a, const char *s)
{
  if (!s || s[0] == '\0' || !a) return nullptr;
  return ccord_arena_strndup(a, s, strlen(s));
}

static const char *sanitize_str_inplace(char *s)
{
  if (!s || s[0] == '\0') return s;
  size_t r = 0, w = 0;
  while (s[r] != '\0') {
    unsigned char c = (unsigned char)s[r];
    if (c == '\r') {
      s[w++] = '\n';
      if (s[r + 1] == '\n') r += 2;
      else r += 1;
      continue;
    }
    if ((c < 0x20u && c != '\n' && c != '\t') || c == 0x7Fu) {
      r++;
      continue;
    }
    if (c == 0xC2u && (unsigned char)s[r + 1] >= 0x80u && (unsigned char)s[r + 1] <= 0x9Fu) {
      r += 2;
      continue;
    }
    s[w++] = s[r++];
  }
  s[w] = '\0';
  return s;
}

static size_t utf8_trunc_len(const char *s, size_t len, size_t max)
{
  if (!s) return 0;
  if (len <= max) return len;
  if ((((unsigned char)s[max]) & 0xC0u) != 0x80u) return max;
  return ccord_utf8_prev(s, max);
}

static const char *jstr(const ccord_json *obj, const char *key)
{
  if (!obj) return nullptr;
  const ccord_json *j = ccord_json_get(obj, key);
  if (!j) return nullptr;
  size_t n = 0;
  union { const char *c; char *m; } u = {.c = ccord_json_string(j, &n)};
  return sanitize_str_inplace(u.m);
}

static size_t jarray_len(const ccord_json *obj, const char *key)
{
  if (!obj) return 0;
  const ccord_json *j = ccord_json_get(obj, key);
  return j ? ccord_json_array_len(j) : 0;
}

static bool json_bool_key(const ccord_json *obj, const char *key, bool fallback);

static bool json_id(const ccord_json *j, ccord_id *out)
{
  if (!j) return false;
  if (ccord_json_type_of(j) == CCORD_JSON_STRING) {
    size_t n = 0;
    const char *s = ccord_json_string(j, &n);
    return s && ccord_id_parse(s, n, out);
  }
  int64_t v = 0;
  if (ccord_json_int64(j, &v) && v > 0) {
    *out = (ccord_id)v;
    return true;
  }
  return false;
}

static bool json_id_key(const ccord_json *obj, const char *key, ccord_id *out)
{
  if (!obj) return false;
  return json_id(ccord_json_get(obj, key), out);
}

static int64_t json_i64(const ccord_json *obj, const char *key, int64_t fallback)
{
  if (!obj) return fallback;
  const ccord_json *j = ccord_json_get(obj, key);
  if (!j) return fallback;
  int64_t v = fallback;
  if (!ccord_json_int64(j, &v)) return fallback;
  return v;
}

static bool two_digits(const char *s, int *out)
{
  if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return false;
  *out = (s[0] - '0') * 10 + (s[1] - '0');
  return true;
}

static bool four_digits(const char *s, int *out)
{
  if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9' || s[2] < '0' || s[2] > '9' ||
      s[3] < '0' || s[3] > '9') {
    return false;
  }
  *out = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
  return true;
}

// Howard Hinnant's civil-date algorithm; valid for the range Discord emits.
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
  unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static int64_t iso_ms(const char *s, size_t len)
{
  if (!s || len < 20) return -1;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (!four_digits(s, &y) || s[4] != '-' || !two_digits(s + 5, &mo) || s[7] != '-' ||
      !two_digits(s + 8, &d) || (s[10] != 'T' && s[10] != 't') || !two_digits(s + 11, &h) ||
      s[13] != ':' || !two_digits(s + 14, &mi) || s[16] != ':' || !two_digits(s + 17, &se)) {
    return -1;
  }
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return -1;
  size_t i = 19;
  int frac = 0, fdigits = 0;
  if (i < len && s[i] == '.') {
    i++;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
      if (fdigits < 3) {
        frac = frac * 10 + (s[i] - '0');
        fdigits++;
      }
      i++;
    }
    while (fdigits < 3) {
      frac *= 10;
      fdigits++;
    }
  }
  int64_t offset = 0;
  if (i < len) {
    if (s[i] == 'Z' || s[i] == 'z') {
      i++;
    } else if (s[i] == '+' || s[i] == '-') {
      int oh = 0, om = 0;
      if (i + 6 > len || !two_digits(s + i + 1, &oh) || s[i + 3] != ':' || !two_digits(s + i + 4, &om)) {
        return -1;
      }
      offset = (int64_t)(oh * 3600 + om * 60);
      if (s[i] == '-') offset = -offset;
      i += 6;
    } else {
      return -1;
    }
  }
  if (i != len) return -1;
  int64_t days = days_from_civil(y, (unsigned)mo, (unsigned)d);
  int64_t secs = days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + se - offset;
  return secs * 1000 + frac;
}

static size_t b64_encode(const unsigned char *in, size_t n, char *out, size_t cap)
{
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t need = 4u * ((n + 2u) / 3u);
  if (need + 1u > cap) return 0;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = (unsigned)in[i] << 16;
    if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
    if (i + 2 < n) v |= (unsigned)in[i + 2];
    out[o++] = table[(v >> 18) & 63u];
    out[o++] = table[(v >> 12) & 63u];
    out[o++] = i + 1 < n ? table[(v >> 6) & 63u] : '=';
    out[o++] = i + 2 < n ? table[v & 63u] : '=';
  }
  out[o] = '\0';
  return o;
}

const char *ccord_live_super_properties(void)
{
  static char encoded[1024];
  static bool ready;
  if (!ready) {
    static const char json[] =
      "{\"os\":\"Linux\",\"browser\":\"Discord Client\",\"release_channel\":\"stable\","
      "\"client_version\":\"0.0.72\",\"os_version\":\"6.12.0\",\"os_arch\":\"x64\","
      "\"app_arch\":\"x64\",\"system_locale\":\"en-US\","
      "\"browser_user_agent\":\"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) discord/0.0.72 Chrome/128.0.6613.186 Electron/32.2.2 Safari/537.36\","
      "\"browser_version\":\"32.2.2\",\"client_build_number\":352419,"
      "\"native_build_number\":null,\"client_event_source\":null,\"design_id\":0}";
    (void)b64_encode((const unsigned char *)json, sizeof json - 1, encoded, sizeof encoded);
    ready = true;
  }
  return encoded;
}

const char *ccord_live_user_agent(void)
{
  return "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) discord/0.0.72 Chrome/128.0.6613.186 Electron/32.2.2 Safari/537.36";
}

static bool text_ok(const char *s, size_t max)
{
  if (!s || s[0] == '\0') return false;
  size_t n = strlen(s);
  return n <= max && ccord_utf8_valid_text(s, n);
}

static const char *opt_text(const char *s, size_t max)
{
  if (!s || s[0] == '\0') return "";
  size_t n = strlen(s);
  return (n <= max && ccord_utf8_valid_text(s, n)) ? s : "";
}

static const char *opt_text_dup(ccord_arena *a, const char *s, size_t max)
{
  const char *ok = opt_text(s, max);
  if (ok[0] == '\0') return "";
  return arena_dup(a, ok);
}

static bool topic_ok(const char *s)
{
  if (!s) return true;
  size_t n = strlen(s);
  return n <= LIVE_TOPIC_MAX_BYTES && ccord_utf8_valid_text(s, n);
}

static bool supported_channel_type(int64_t type)
{
  return type == 0 || type == 1 || type == 2 || type == 3 || type == 4 ||
         type == 5 || type == 10 || type == 11 || type == 12 || type == 13 ||
         type == 15 || type == 16;
}

static ccord_channel_kind channel_kind(int64_t type)
{
  if (type == 4) return CCORD_CHANNEL_CATEGORY;
  if (type == 13) return CCORD_CHANNEL_STAGE;
  if (type == 2) return CCORD_CHANNEL_VOICE;
  return CCORD_CHANNEL_TEXT;
}

static const char *build_cdn_url(ccord_arena *a, const char *kind, ccord_id id, const char *hash);
typedef ccord_idmap id_index;

static const char *dm_name(ccord_arena *a, const ccord_json *j, const ccord_user *users, size_t user_count,
                           const id_index *user_idx, ccord_id self_id, ccord_id *out_uid, const char **out_icon)
{
  if (out_uid) *out_uid = CCORD_ID_NONE;
  if (out_icon) *out_icon = "";
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_id primary_uid = CCORD_ID_NONE;
  const char *primary_icon = "";
  const ccord_json *recs = j ? ccord_json_get(j, "recipients") : nullptr;
  size_t count = recs ? ccord_json_array_len(recs) : 0;
  for (size_t i = 0; i < count; i++) {
    const ccord_json *ru = ccord_json_array_at(recs, i);
    ccord_id ruid = CCORD_ID_NONE;
    (void)json_id_key(ru, "id", &ruid);
    if (count > 1 && self_id != CCORD_ID_NONE && ruid == self_id) continue;
    const char *name = jstr(ru, "global_name");
    if (!name || name[0] == '\0') name = jstr(ru, "username");
    const char *av = (ruid != CCORD_ID_NONE) ? build_cdn_url(a, "avatars", ruid, jstr(ru, "avatar")) : "";
    if (users && user_count > 0) {
      size_t k = 0;
      if (user_idx && ruid != CCORD_ID_NONE && ccord_idmap_find(user_idx, ruid, &k) && k < user_count) {
        if (!name || name[0] == '\0') name = users[k].name;
        if ((!av || av[0] == '\0') && users[k].avatar_url) av = users[k].avatar_url;
      } else {
        for (k = 0; k < user_count; k++) {
          if ((ruid != CCORD_ID_NONE && users[k].id == ruid) ||
              (ruid == CCORD_ID_NONE && users[k].id != self_id && name && name[0] != '\0' &&
               strcmp(users[k].name, name) == 0)) {
            if (ruid == CCORD_ID_NONE) ruid = users[k].id;
            if (!name || name[0] == '\0') name = users[k].name;
            if ((!av || av[0] == '\0') && users[k].avatar_url) av = users[k].avatar_url;
            break;
          }
        }
      }
    }
    if (primary_uid == CCORD_ID_NONE && ruid != CCORD_ID_NONE) {
      primary_uid = ruid;
      primary_icon = av ? av : "";
    }
    if (!name || name[0] == '\0') continue;
    if (b.len > 0) (void)ccord_buf_append_str(&b, ", ");
    (void)ccord_buf_append_str(&b, name);
  }
  if (b.len == 0) {
    const ccord_json *rids = j ? ccord_json_get(j, "recipient_ids") : nullptr;
    size_t nids = rids ? ccord_json_array_len(rids) : 0;
    for (size_t i = 0; i < nids; i++) {
      ccord_id ruid = CCORD_ID_NONE;
      if (!json_id(ccord_json_array_at(rids, i), &ruid)) continue;
      if (nids > 1 && self_id != CCORD_ID_NONE && ruid == self_id) continue;
      const char *name = nullptr;
      const char *av = "";
      if (users && user_count > 0) {
        size_t k = 0;
        if (user_idx && ccord_idmap_find(user_idx, ruid, &k) && k < user_count) {
          name = users[k].name;
          if (users[k].avatar_url) av = users[k].avatar_url;
        } else {
          for (k = 0; k < user_count; k++) {
            if (users[k].id == ruid) {
              name = users[k].name;
              if (users[k].avatar_url) av = users[k].avatar_url;
              break;
            }
          }
        }
      }
      if (primary_uid == CCORD_ID_NONE) {
        primary_uid = ruid;
        primary_icon = av ? av : "";
      }
      if (!name || name[0] == '\0') continue;
      if (b.len > 0) (void)ccord_buf_append_str(&b, ", ");
      (void)ccord_buf_append_str(&b, name);
    }
  }
  if (b.len == 0) (void)ccord_buf_append_str(&b, "Direct Message");
  size_t n = utf8_trunc_len(b.data, b.len, CCORD_NAME_MAX_BYTES);
  char *s = ccord_arena_strndup(a, ccord_buf_cstr(&b), n);
  ccord_buf_free(&b);
  if (out_uid) *out_uid = primary_uid;
  if (out_icon) *out_icon = primary_icon ? primary_icon : "";
  return s;
}

static bool map_channel_ex(ccord_arena *a, const ccord_user *users, size_t user_count, const id_index *user_idx,
                           ccord_id self_id, const ccord_json *j, ccord_id guild_hint, ccord_channel *out)
{
  if (!j || !out) return false;
  *out = (ccord_channel){0};
  ccord_id id = CCORD_ID_NONE;
  if (!json_id_key(j, "id", &id)) return false;
  int64_t type = json_i64(j, "type", 0);
  if (!supported_channel_type(type)) return false;
  ccord_id gid = guild_hint;
  if (gid == CCORD_ID_NONE) (void)json_id_key(j, "guild_id", &gid);
  if (gid == CCORD_ID_NONE && (type == 1 || type == 3)) gid = CCORD_DM_GUILD_ID;
  size_t kept = 0, total = 0;
  ccord_id owner = CCORD_ID_NONE;
  bool msg_req = false;
  bool spam = false;
  if (gid == CCORD_DM_GUILD_ID || type == 1 || type == 3) {
    (void)json_id_key(j, "owner_id", &owner);
    const ccord_json *mr_j = ccord_json_get(j, "is_message_request");
    int64_t consent = json_i64(j, "consent_status", 0);
    msg_req = (consent == 2) ? false : (mr_j ? ccord_json_bool(mr_j, false) : (consent == 1));
    spam = (consent == 2) ? false : json_bool_key(j, "is_spam", false);
    if (spam && consent != 2) msg_req = true;
    const ccord_json *recs = ccord_json_get(j, "recipients");
    size_t nrec = recs ? ccord_json_array_len(recs) : 0;
    for (size_t i = 0; i < nrec; i++) {
      ccord_id ruid = CCORD_ID_NONE;
      if (!json_id_key(ccord_json_array_at(recs, i), "id", &ruid) || ruid == CCORD_ID_NONE) continue;
      if (self_id != CCORD_ID_NONE && ruid == self_id) continue;
      total++;
      if (kept < CCORD_MAX_DM_RECIPIENTS) out->recipient_ids[kept++] = ruid;
    }
    if (total == 0) {
      const ccord_json *rids = ccord_json_get(j, "recipient_ids");
      size_t nids = rids ? ccord_json_array_len(rids) : 0;
      for (size_t i = 0; i < nids; i++) {
        ccord_id ruid = CCORD_ID_NONE;
        if (!json_id(ccord_json_array_at(rids, i), &ruid) || ruid == CCORD_ID_NONE) continue;
        if (self_id != CCORD_ID_NONE && ruid == self_id) continue;
        total++;
        if (kept < CCORD_MAX_DM_RECIPIENTS) out->recipient_ids[kept++] = ruid;
      }
    }
    if (total > 1 || ccord_json_get(j, "owner_id") != nullptr) {
      type = 3;
      if (gid == CCORD_ID_NONE) gid = CCORD_DM_GUILD_ID;
    }
  }
  ccord_id dm_uid = CCORD_ID_NONE;
  const char *dm_icon = "";
  const char *resolved_dm = nullptr;
  if (gid == CCORD_DM_GUILD_ID || type == 1 || type == 3) {
    resolved_dm = dm_name(a, j, users, user_count, user_idx, self_id, &dm_uid, &dm_icon);
    // A group DM's icon and identity are the channel's own, never the first recipient's user_id or avatar —
    // leaving dm_uid non-zero on group DMs made them impersonate a 1:1 DM with the first recipient.
    if (type == 3) {
      dm_uid = CCORD_ID_NONE;
      dm_icon = "";
    }
    const char *gdm_icon = jstr(j, "icon");
    if (gdm_icon && gdm_icon[0] != '\0') {
      const char *cicon = build_cdn_url(a, "channel-icons", id, gdm_icon);
      if (cicon && cicon[0] != '\0') dm_icon = cicon;
    }
  }
  const char *name = jstr(j, "name");
  bool name_owned = false;
  if ((!name || name[0] == '\0') && resolved_dm) {
    name = resolved_dm;
    name_owned = true;
  }
  // Discord caps names at 100 *characters*; emoji and styled Unicode can push that well past
  // CCORD_NAME_MAX_BYTES. Truncate on a code point boundary instead of dropping the channel
  // (dropping it hid named group DMs and made their messages spawn a fake 1:1 DM).
  if (name && strlen(name) > CCORD_NAME_MAX_BYTES) {
    size_t cut = utf8_trunc_len(name, strlen(name), CCORD_NAME_MAX_BYTES);
    name = cut > 0 ? ccord_arena_strndup(a, name, cut) : nullptr;
    name_owned = true;
  }
  if (!text_ok(name, CCORD_NAME_MAX_BYTES)) {
    if (resolved_dm && text_ok(resolved_dm, CCORD_NAME_MAX_BYTES)) {
      name = resolved_dm;
      name_owned = true;
    } else if (type != 1 && type != 3) {
      name = "unnamed";
      name_owned = true;
    } else {
      return false;
    }
  }
  if (!name_owned) {
    name = ccord_arena_strndup(a, name, strlen(name));
    if (!name) return false;
  }
  const char *topic = jstr(j, "topic");
  if (!topic || topic[0] == '\0' || !topic_ok(topic)) topic = "";
  else topic = arena_dup(a, topic);
  ccord_id parent = CCORD_ID_NONE;
  if (type != 4) (void)json_id_key(j, "parent_id", &parent);
  int64_t pos = json_i64(j, "position", 0);
  if (pos < (int64_t)INT32_MIN) pos = (int64_t)INT32_MIN;
  if (pos > (int64_t)INT32_MAX) pos = (int64_t)INT32_MAX;
  ccord_id lmid = CCORD_ID_NONE;
  (void)json_id_key(j, "last_message_id", &lmid);
  int64_t unread = json_i64(j, "unread_count", 0);
  int64_t mentions = json_i64(j, "mention_count", 0);
  if (unread < 0) unread = 0;
  if (unread > (int64_t)UINT32_MAX) unread = (int64_t)UINT32_MAX;
  if (mentions < 0) mentions = 0;
  if (mentions > (int64_t)UINT32_MAX) mentions = (int64_t)UINT32_MAX;
  out->id = id;
  out->guild_id = gid;
  out->parent_id = parent;
  out->kind = channel_kind(type);
  out->name = name;
  out->topic = topic;
  out->position = (int32_t)pos;
  out->last_message_id = lmid;
  out->unread_count = (uint32_t)unread;
  out->mention_count = (uint32_t)mentions;
  out->user_id = dm_uid;
  out->icon_url = dm_icon ? dm_icon : "";
  out->discord_type = (uint8_t)type;
  int64_t rl = json_i64(j, "rate_limit_per_user", 0);
  if (rl < 0) rl = 0;
  if (rl > 21600) rl = 21600;
  out->rate_limit_per_user = (uint16_t)rl;
  const ccord_json *nsfw_j = ccord_json_get(j, "nsfw");
  out->nsfw = nsfw_j && ccord_json_bool(nsfw_j, false);
  int64_t br = json_i64(j, "bitrate", 0);
  if (br < 0) br = 0;
  if (br > (int64_t)UINT32_MAX) br = (int64_t)UINT32_MAX;
  out->bitrate = (uint32_t)br;
  int64_t ul = json_i64(j, "user_limit", 0);
  if (ul < 0) ul = 0;
  if (ul > (int64_t)UINT32_MAX) ul = (int64_t)UINT32_MAX;
  out->user_limit = (uint32_t)ul;
  int64_t daad = json_i64(j, "default_auto_archive_duration", 0);
  if (daad < 0) daad = 0;
  if (daad > (int64_t)UINT32_MAX) daad = (int64_t)UINT32_MAX;
  out->default_auto_archive_duration = (uint32_t)daad;
  if (type == 1 || type == 3) {
    if (total > UINT16_MAX) total = UINT16_MAX;
    out->recipient_count = (uint16_t)total;
    out->owner_id = owner;
    out->is_message_request = msg_req;
    out->is_spam = spam;
  }
  const ccord_json *tm = ccord_json_get(j, "thread_metadata");
  if (tm && ccord_json_type_of(tm) == CCORD_JSON_OBJECT) {
    const ccord_json *arch_j = ccord_json_get(tm, "archived");
    const ccord_json *lock_j = ccord_json_get(tm, "locked");
    out->thread_archived = arch_j && ccord_json_bool(arch_j, false);
    out->thread_locked = lock_j && ccord_json_bool(lock_j, false);
    int64_t aad = json_i64(tm, "auto_archive_duration", 0);
    if (aad < 0) aad = 0;
    if (aad > UINT16_MAX) aad = UINT16_MAX;
    out->thread_auto_archive_mins = (uint16_t)aad;
  }
  const ccord_json *mem_j = ccord_json_get(j, "member");
  if (mem_j && ccord_json_type_of(mem_j) == CCORD_JSON_OBJECT) {
    out->thread_has_member = true;
    out->thread_joined = true;
  }
  int64_t tmc = json_i64(j, "message_count", 0);
  if (tmc < 0) tmc = 0;
  if (tmc > (int64_t)UINT32_MAX) tmc = (int64_t)UINT32_MAX;
  out->thread_message_count = (uint32_t)tmc;
  return true;
}

static bool map_channel(ccord_arena *a, const ccord_json *j, ccord_id guild_hint, ccord_channel *out)
{
  return map_channel_ex(a, nullptr, 0, nullptr, CCORD_ID_NONE, j, guild_hint, out);
}

static void message_reactions(const ccord_json *j, uint32_t *count, bool *self)
{
  if (!j) {
    *count = 0;
    *self = false;
    return;
  }
  const ccord_json *arr = ccord_json_get(j, "reactions");
  size_t n = arr ? ccord_json_array_len(arr) : 0;
  int64_t total = 0;
  bool me = false;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *r = ccord_json_array_at(arr, i);
    int64_t c = json_i64(r, "count", 0);
    if (c <= 0 && r) {
      const ccord_json *cd = ccord_json_get(r, "count_details");
      if (cd && ccord_json_type_of(cd) == CCORD_JSON_OBJECT) {
        int64_t nc = json_i64(cd, "normal", 0);
        int64_t bc = json_i64(cd, "burst", 0);
        if (nc < 0) nc = 0;
        if (bc < 0) bc = 0;
        c = nc + bc;
      }
    }
    if (c > 0) total += c;
    const ccord_json *mj = r ? ccord_json_get(r, "me") : nullptr;
    const ccord_json *mbj = r ? ccord_json_get(r, "me_burst") : nullptr;
    if ((mj && ccord_json_bool(mj, false)) || (mbj && ccord_json_bool(mbj, false))) me = true;
  }
  if (total < 0) total = 0;
  if (total > (int64_t)UINT32_MAX) total = (int64_t)UINT32_MAX;
  *count = (uint32_t)total;
  *self = me;
}

// Formats a Discord emoji JSON object into a canonical UTF-8 reaction identifier:
// standard Unicode emojis keep `name`, custom emojis with a snowflake `id` format as `name:id`.
static const char *format_reaction_emoji(ccord_arena *a, const ccord_json *emj, ccord_id *out_id,
                                         bool *out_animated)
{
  if (out_id) *out_id = CCORD_ID_NONE;
  if (out_animated) *out_animated = false;
  if (!emj) return nullptr;
  ccord_id eid = CCORD_ID_NONE;
  (void)json_id_key(emj, "id", &eid);
  const ccord_json *aj = ccord_json_get(emj, "animated");
  bool anim = aj && ccord_json_bool(aj, false);
  const char *name = jstr(emj, "name");
  if (eid != CCORD_ID_NONE) {
    if (!name || name[0] == '\0' || !text_ok(name, CCORD_EMOJI_MAX_BYTES)) name = "emoji";
    char idbuf[21];
    int idlen = ccord_id_format(eid, idbuf);
    if (idlen <= 0) return nullptr;
    size_t max_nlen = CCORD_EMOJI_MAX_BYTES > (size_t)idlen + 1u
                        ? CCORD_EMOJI_MAX_BYTES - (size_t)idlen - 1u
                        : 0u;
    size_t nlen = utf8_trunc_len(name, strlen(name), max_nlen);
    size_t total_len = nlen + 1u + (size_t)idlen;
    char *buf = ccord_arena_alloc(a, total_len + 1u, 1);
    if (!buf) return nullptr;
    memcpy(buf, name, nlen);
    buf[nlen] = ':';
    memcpy(buf + nlen + 1u, idbuf, (size_t)idlen);
    buf[total_len] = '\0';
    if (out_id) *out_id = eid;
    if (out_animated) *out_animated = anim;
    return buf;
  }
  if (!text_ok(name, CCORD_EMOJI_MAX_BYTES)) return nullptr;
  return ccord_arena_strndup(a, name, strlen(name));
}

// Fills the per-emoji breakdown from a Discord message's `reactions` array.
static void map_reaction_list(ccord_arena *a, const ccord_json *j, ccord_message *out)
{
  if (!j) return;
  const ccord_json *arr = ccord_json_get(j, "reactions");
  size_t n = arr ? ccord_json_array_len(arr) : 0;
  if (n == 0) return;
  if (n > CCORD_MAX_REACTIONS_PER_MESSAGE) n = CCORD_MAX_REACTIONS_PER_MESSAGE;
  ccord_reaction *rs = ccord_arena_alloc(a, n * sizeof *rs, _Alignof(ccord_reaction));
  if (!rs) return;
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *r = ccord_json_array_at(arr, i);
    const ccord_json *emj = r ? ccord_json_get(r, "emoji") : nullptr;
    ccord_id eid = CCORD_ID_NONE;
    bool anim = false;
    const char *emoji_str = format_reaction_emoji(a, emj, &eid, &anim);
    if (!emoji_str) continue;
    const ccord_json *cd = ccord_json_get(r, "count_details");
    int64_t bc = (cd && ccord_json_type_of(cd) == CCORD_JSON_OBJECT) ? json_i64(cd, "burst", 0) : 0;
    if (bc < 0) bc = 0;
    if (bc > (int64_t)UINT32_MAX) bc = (int64_t)UINT32_MAX;
    int64_t c = json_i64(r, "count", 0);
    if (c <= 0 && bc > 0) {
      int64_t nc = json_i64(cd, "normal", 0);
      if (nc < 0) nc = 0;
      c = nc + bc;
    }
    if (c <= 0) continue;
    if (c > (int64_t)UINT32_MAX) c = (int64_t)UINT32_MAX;
    if (bc > c) bc = c;
    const ccord_json *mj = ccord_json_get(r, "me");
    const ccord_json *mbj = ccord_json_get(r, "me_burst");
    bool me_norm = mj && ccord_json_bool(mj, false);
    bool me_burst = mbj && ccord_json_bool(mbj, false);
    rs[kept++] = (ccord_reaction){
      .emoji = emoji_str,
      .emoji_id = eid,
      .count = (uint32_t)c,
      .burst_count = (uint32_t)bc,
      .self_reacted = me_norm,
      .me_burst = me_burst,
      .animated = anim,
    };
  }
  if (kept) {
    out->reactions = rs;
    out->reaction_kind_count = kept;
  }
}

static const char *clamp_message_text(ccord_arena *a, const char *s)
{
  if (!s || s[0] == '\0') return "";
  size_t len = strlen(s);
  size_t n = (len <= CCORD_MESSAGE_MAX_BYTES) ? len : utf8_trunc_len(s, len, CCORD_MESSAGE_MAX_BYTES);
  if (n == 0) return "";
  const char *out = ccord_arena_strndup(a, s, n);
  return out ? out : "";
}

// ---------------------------------------------------------------- rich message data
// Attachments, embeds, stickers, mentions, the replied-to message, a forwarded snapshot, a poll and a started thread,
// copied into the event's arena as a ccord_message_rich (the store takes its own deep copy). Anything over a cap is
// dropped rather than failing the message; text over its cap is cut on a character boundary; an over-long URL is
// dropped (half a link is worse than none).

static uint32_t clamp_u32(int64_t v)
{
  if (v < 0) return 0u;
  return v > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

static bool is_json_object(const ccord_json *j)
{
  return j && ccord_json_type_of(j) == CCORD_JSON_OBJECT;
}

static bool is_json_array(const ccord_json *j)
{
  return j && ccord_json_type_of(j) == CCORD_JSON_ARRAY;
}

// An arena copy of obj[key], "" when it is missing, not text, or not valid UTF-8; cut to `max` bytes.
static const char *astr(ccord_arena *a, const ccord_json *obj, const char *key, size_t max)
{
  const char *s = jstr(obj, key);
  if (!s || s[0] == '\0') return "";
  size_t n = utf8_trunc_len(s, strlen(s), max);
  if (n == 0) return "";
  char *d = ccord_arena_strndup(a, s, n);
  if (!d || !ccord_utf8_valid_text(d, n)) return "";
  return d;
}

static const char *aurl(ccord_arena *a, const ccord_json *obj, const char *key)
{
  const char *s = jstr(obj, key);
  if (!s || s[0] == '\0' || strlen(s) > CCORD_RICH_URL_MAX_BYTES) return "";
  return astr(a, obj, key, CCORD_RICH_URL_MAX_BYTES);
}

static void *rich_alloc(ccord_arena *a, size_t n, size_t size)
{
  if (n == 0) return nullptr;
  void *p = ccord_arena_alloc(a, n * size, _Alignof(max_align_t));
  if (p) memset(p, 0, n * size);
  return p;
}

static void rich_media(ccord_arena *a, const ccord_json *m, ccord_embed_media *out)
{
  *out = (ccord_embed_media){.url = "", .proxy_url = ""};
  if (!is_json_object(m)) return;
  out->url = aurl(a, m, "url");
  out->proxy_url = aurl(a, m, "proxy_url");
  out->width = clamp_u32(json_i64(m, "width", 0));
  out->height = clamp_u32(json_i64(m, "height", 0));
}

static bool starts_with_spoiler(const char *name)
{
  return name && strncmp(name, "SPOILER_", 8) == 0;
}

// A voice-message waveform is base64 text. Anything longer than the cap, or with a byte outside the base64
// alphabet, is dropped whole (a truncated waveform would decode to the wrong bars) so the attachment still parses.
static const char *awaveform(ccord_arena *a, const ccord_json *obj)
{
  const char *s = jstr(obj, "waveform");
  if (!s || s[0] == '\0') return "";
  size_t n = strlen(s);
  if (n > CCORD_RICH_WAVEFORM_MAX_BYTES) return "";
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    bool ok = (c >= (unsigned char)'A' && c <= (unsigned char)'Z') || (c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
              (c >= (unsigned char)'0' && c <= (unsigned char)'9') || c == (unsigned char)'+' || c == (unsigned char)'/' ||
              c == (unsigned char)'=';
    if (!ok) return "";
  }
  char *d = ccord_arena_strndup(a, s, n);
  return d ? d : "";
}

static ccord_attachment *rich_attachments(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_RICH_MAX_ATTACHMENTS) n = CCORD_RICH_MAX_ATTACHMENTS;
  ccord_attachment *out = rich_alloc(a, n, sizeof *out);
  for (size_t i = 0; out && i < n; i++) {
    const ccord_json *j = ccord_json_array_at(arr, i);
    if (!is_json_object(j)) continue;
    ccord_attachment at = {0};
    (void)json_id_key(j, "id", &at.id);
    at.filename = astr(a, j, "filename", CCORD_RICH_SHORT_MAX_BYTES);
    at.description = astr(a, j, "description", CCORD_RICH_TEXT_MAX_BYTES);
    at.content_type = astr(a, j, "content_type", CCORD_RICH_SHORT_MAX_BYTES);
    at.url = aurl(a, j, "url");
    at.proxy_url = aurl(a, j, "proxy_url");
    if (at.url[0] == '\0' && at.proxy_url[0] == '\0' && at.filename[0] == '\0') continue;   // nothing to show or fetch
    int64_t size = json_i64(j, "size", 0);
    at.size = size > 0 ? (uint64_t)size : 0u;
    at.width = clamp_u32(json_i64(j, "width", 0));
    at.height = clamp_u32(json_i64(j, "height", 0));
    const ccord_json *dj = ccord_json_get(j, "duration_secs");
    double secs = dj ? ccord_json_number(dj, 0.0) : 0.0;
    at.duration_ms = (secs > 0.0 && secs < 4000000.0) ? (uint32_t)(secs * 1000.0) : 0u;
    at.waveform_b64 = awaveform(a, j);
    at.flags = clamp_u32(json_i64(j, "flags", 0));
    at.spoiler = starts_with_spoiler(at.filename);
    out[(*count)++] = at;
  }
  return *count ? out : nullptr;
}

static ccord_embed_kind embed_kind_of(const char *t)
{
  if (!t || t[0] == '\0' || strcmp(t, "rich") == 0) return CCORD_EMBED_RICH;
  if (strcmp(t, "image") == 0) return CCORD_EMBED_IMAGE;
  if (strcmp(t, "video") == 0) return CCORD_EMBED_VIDEO;
  if (strcmp(t, "gifv") == 0) return CCORD_EMBED_GIFV;
  if (strcmp(t, "article") == 0) return CCORD_EMBED_ARTICLE;
  if (strcmp(t, "link") == 0) return CCORD_EMBED_LINK;
  if (strcmp(t, "poll_result") == 0) return CCORD_EMBED_POLL_RESULT;
  return CCORD_EMBED_OTHER;
}

static ccord_embed *rich_embeds(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_RICH_MAX_EMBEDS) n = CCORD_RICH_MAX_EMBEDS;
  ccord_embed *out = rich_alloc(a, n, sizeof *out);
  for (size_t i = 0; out && i < n; i++) {
    const ccord_json *j = ccord_json_array_at(arr, i);
    if (!is_json_object(j)) continue;
    ccord_embed e = {0};
    e.kind = embed_kind_of(jstr(j, "type"));
    e.title = astr(a, j, "title", CCORD_RICH_SHORT_MAX_BYTES * 2u);
    e.description = astr(a, j, "description", CCORD_RICH_TEXT_MAX_BYTES);
    e.url = aurl(a, j, "url");
    const char *ts = jstr(j, "timestamp");
    int64_t t = ts ? iso_ms(ts, strlen(ts)) : -1;
    e.timestamp_ms = t > 0 ? t : 0;
    const ccord_json *cj = ccord_json_get(j, "color");
    int64_t color = -1;
    if (cj && ccord_json_int64(cj, &color) && color >= 0 && color <= 0xFFFFFF) {
      e.color = (uint32_t)color;
      e.has_color = true;
    }
    const ccord_json *prov = ccord_json_get(j, "provider");
    e.provider = is_json_object(prov) ? astr(a, prov, "name", CCORD_RICH_SHORT_MAX_BYTES) : "";
    const ccord_json *au = ccord_json_get(j, "author");
    e.author_name = e.author_url = e.author_icon = "";
    if (is_json_object(au)) {
      e.author_name = astr(a, au, "name", CCORD_RICH_SHORT_MAX_BYTES);
      e.author_url = aurl(a, au, "url");
      // Icons are drawn, never opened: prefer Discord's proxied copy (the origin host is not fetched directly).
      e.author_icon = aurl(a, au, "proxy_icon_url");
      if (e.author_icon[0] == '\0') e.author_icon = aurl(a, au, "icon_url");
    }
    const ccord_json *ft = ccord_json_get(j, "footer");
    e.footer_text = e.footer_icon = "";
    if (is_json_object(ft)) {
      e.footer_text = astr(a, ft, "text", CCORD_RICH_TEXT_MAX_BYTES / 2u);
      e.footer_icon = aurl(a, ft, "proxy_icon_url");
      if (e.footer_icon[0] == '\0') e.footer_icon = aurl(a, ft, "icon_url");
    }
    rich_media(a, ccord_json_get(j, "image"), &e.image);
    rich_media(a, ccord_json_get(j, "thumbnail"), &e.thumbnail);
    rich_media(a, ccord_json_get(j, "video"), &e.video);
    const ccord_json *fa = ccord_json_get(j, "fields");
    size_t nf = is_json_array(fa) ? ccord_json_array_len(fa) : 0;
    if (nf > CCORD_RICH_MAX_EMBED_FIELDS) nf = CCORD_RICH_MAX_EMBED_FIELDS;
    ccord_embed_field *fields = rich_alloc(a, nf, sizeof *fields);
    size_t kept = 0;
    for (size_t k = 0; fields && k < nf; k++) {
      const ccord_json *fj = ccord_json_array_at(fa, k);
      if (!is_json_object(fj)) continue;
      const char *name = astr(a, fj, "name", CCORD_RICH_SHORT_MAX_BYTES);
      const char *value = astr(a, fj, "value", CCORD_RICH_FIELD_VALUE_MAX_BYTES);
      if (name[0] == '\0' && value[0] == '\0') continue;
      const ccord_json *ij = ccord_json_get(fj, "inline");
      fields[kept++] = (ccord_embed_field){.name = name, .value = value, .inline_ = ij && ccord_json_bool(ij, false)};
    }
    e.fields = kept ? fields : nullptr;
    e.field_count = kept;
    out[(*count)++] = e;
  }
  return *count ? out : nullptr;
}

static ccord_msg_sticker *rich_stickers(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_RICH_MAX_STICKERS) n = CCORD_RICH_MAX_STICKERS;
  ccord_msg_sticker *out = rich_alloc(a, n, sizeof *out);
  for (size_t i = 0; out && i < n; i++) {
    const ccord_json *j = ccord_json_array_at(arr, i);
    if (!is_json_object(j)) continue;
    ccord_msg_sticker st = {0};
    if (!json_id_key(j, "id", &st.id)) continue;
    st.name = astr(a, j, "name", CCORD_RICH_SHORT_MAX_BYTES);
    int64_t fmt = json_i64(j, "format_type", 0);
    st.format = (fmt >= 1 && fmt <= 4) ? (uint8_t)fmt : 0u;
    out[(*count)++] = st;
  }
  return *count ? out : nullptr;
}

static void rich_poll(ccord_arena *a, const ccord_json *p, ccord_poll *out)
{
  *out = (ccord_poll){.question = ""};
  if (!is_json_object(p)) return;
  const ccord_json *q = ccord_json_get(p, "question");
  const char *qtext = is_json_object(q) ? astr(a, q, "text", CCORD_RICH_SHORT_MAX_BYTES * 2u) : "";
  out->present = true;
  out->question = qtext[0] != '\0' ? qtext : "Poll";
  const ccord_json *ex = ccord_json_get(p, "expiry");
  const char *exs = (ex && ccord_json_type_of(ex) == CCORD_JSON_STRING) ? jstr(p, "expiry") : nullptr;
  int64_t exm = exs ? iso_ms(exs, strlen(exs)) : -1;
  out->expiry_ms = exm > 0 ? exm : 0;
  const ccord_json *ms = ccord_json_get(p, "allow_multiselect");
  out->multiselect = ms && ccord_json_bool(ms, false);
  const ccord_json *answers = ccord_json_get(p, "answers");
  size_t n = is_json_array(answers) ? ccord_json_array_len(answers) : 0;
  if (n > CCORD_RICH_MAX_POLL_ANSWERS) n = CCORD_RICH_MAX_POLL_ANSWERS;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *aj = ccord_json_array_at(answers, i);
    if (!is_json_object(aj)) continue;
    ccord_poll_answer *pa = &out->answers[out->answer_count];
    *pa = (ccord_poll_answer){.text = "", .emoji = ""};
    pa->id = clamp_u32(json_i64(aj, "answer_id", (int64_t)i + 1));
    const ccord_json *media = ccord_json_get(aj, "poll_media");
    if (is_json_object(media)) {
      pa->text = astr(a, media, "text", CCORD_RICH_SHORT_MAX_BYTES);
      const ccord_json *em = ccord_json_get(media, "emoji");
      if (is_json_object(em)) {
        pa->emoji = astr(a, em, "name", CCORD_EMOJI_MAX_BYTES);
        (void)json_id_key(em, "id", &pa->emoji_id);
      }
    }
    out->answer_count++;
  }
  const ccord_json *res = ccord_json_get(p, "results");
  if (is_json_object(res)) {
    const ccord_json *fin = ccord_json_get(res, "is_finalized");
    out->finalized = fin && ccord_json_bool(fin, false);
    const ccord_json *counts = ccord_json_get(res, "answer_counts");
    size_t nc = is_json_array(counts) ? ccord_json_array_len(counts) : 0;
    for (size_t i = 0; i < nc; i++) {
      const ccord_json *cj = ccord_json_array_at(counts, i);
      if (!is_json_object(cj)) continue;
      uint32_t aid = clamp_u32(json_i64(cj, "id", 0));
      for (size_t k = 0; k < out->answer_count; k++) {
        if (out->answers[k].id != aid) continue;
        out->answers[k].votes = clamp_u32(json_i64(cj, "count", 0));
        const ccord_json *me = ccord_json_get(cj, "me_voted");
        out->answers[k].me_voted = me && ccord_json_bool(me, false);
      }
    }
  }
}

static const char *display_name_of(ccord_arena *a, const ccord_json *user)
{
  const char *g = jstr(user, "global_name");
  if (g && g[0] != '\0') return astr(a, user, "global_name", CCORD_RICH_SHORT_MAX_BYTES);
  return astr(a, user, "username", CCORD_RICH_SHORT_MAX_BYTES);
}

static void rich_ref_preview(ccord_arena *a, const ccord_json *j, ccord_ref_preview *out)
{
  *out = (ccord_ref_preview){.author_name = "", .content = ""};
  const ccord_json *rm = ccord_json_get(j, "referenced_message");
  if (!rm) return;                                   // Discord did not say: unknown, not deleted
  if (!is_json_object(rm)) {                         // null: the message is gone
    out->deleted = true;
    return;
  }
  out->present = true;
  const ccord_json *au = ccord_json_get(rm, "author");
  if (is_json_object(au)) {
    (void)json_id_key(au, "id", &out->author_id);
    out->author_name = display_name_of(a, au);
  }
  out->content = astr(a, rm, "content", CCORD_RICH_SNIPPET_MAX_BYTES);
  out->has_attachments = jarray_len(rm, "attachments") > 0;
}

static void rich_forward(ccord_arena *a, const ccord_json *j, ccord_forward *out)
{
  *out = (ccord_forward){.content = ""};
  const ccord_json *snaps = ccord_json_get(j, "message_snapshots");
  if (!is_json_array(snaps) || ccord_json_array_len(snaps) == 0) return;
  const ccord_json *first = ccord_json_array_at(snaps, 0);
  const ccord_json *msg = is_json_object(first) ? ccord_json_get(first, "message") : nullptr;
  if (!is_json_object(msg)) return;
  out->present = true;
  out->content = astr(a, msg, "content", CCORD_RICH_TEXT_MAX_BYTES);
  const char *ts = jstr(msg, "timestamp");
  int64_t t = ts ? iso_ms(ts, strlen(ts)) : -1;
  out->timestamp_ms = t > 0 ? t : 0;
  out->attachments = rich_attachments(a, ccord_json_get(msg, "attachments"), &out->attachment_count);
  out->embeds = rich_embeds(a, ccord_json_get(msg, "embeds"), &out->embed_count);
  const ccord_json *st = ccord_json_get(msg, "sticker_items");
  if (!st) st = ccord_json_get(msg, "stickers");
  out->stickers = rich_stickers(a, st, &out->sticker_count);
}

static ccord_id *rich_user_ids(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_RICH_MAX_MENTIONS) n = CCORD_RICH_MAX_MENTIONS;
  ccord_id *out = rich_alloc(a, n, sizeof *out);
  for (size_t i = 0; out && i < n; i++) {
    ccord_id id = CCORD_ID_NONE;
    if (json_id_key(ccord_json_array_at(arr, i), "id", &id)) out[(*count)++] = id;
  }
  return *count ? out : nullptr;
}

static ccord_id *rich_role_ids(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_RICH_MAX_MENTIONS) n = CCORD_RICH_MAX_MENTIONS;
  ccord_id *out = rich_alloc(a, n, sizeof *out);
  for (size_t i = 0; out && i < n; i++) {
    ccord_id id = CCORD_ID_NONE;
    if (json_id(ccord_json_array_at(arr, i), &id)) out[(*count)++] = id;
  }
  return *count ? out : nullptr;
}

static ccord_select_option *rich_select_options(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_RICH_MAX_SELECT_OPTIONS) n = CCORD_RICH_MAX_SELECT_OPTIONS;
  ccord_select_option *out = rich_alloc(a, n, sizeof *out);
  for (size_t i = 0; out && i < n; i++) {
    const ccord_json *oj = ccord_json_array_at(arr, i);
    if (!is_json_object(oj)) continue;
    const char *val = astr(a, oj, "value", CCORD_RICH_SHORT_MAX_BYTES);
    if (val[0] == '\0') continue;
    const char *lbl = astr(a, oj, "label", CCORD_RICH_SHORT_MAX_BYTES);
    const ccord_json *defj = ccord_json_get(oj, "default");
    ccord_select_option *o = &out[(*count)++];
    *o = (ccord_select_option){
      .label = lbl[0] != '\0' ? lbl : val,
      .value = val,
      .description = astr(a, oj, "description", CCORD_RICH_SHORT_MAX_BYTES),
      .emoji = "",
      .default_ = defj && ccord_json_bool(defj, false),
    };
    const ccord_json *ej = ccord_json_get(oj, "emoji");
    if (is_json_object(ej)) {
      o->emoji = astr(a, ej, "name", CCORD_EMOJI_MAX_BYTES);
      (void)json_id_key(ej, "id", &o->emoji_id);
    }
  }
  return *count ? out : nullptr;
}

// A button or select menu: the mapping v1 rows and the v2 tree share. False when the component cannot be kept
// (a link button without a url, an interactive component without a custom_id).
static bool comp_interactive(ccord_arena *a, const ccord_json *cj, int64_t ct, ccord_component *out)
{
  const ccord_json *disj = ccord_json_get(cj, "disabled");
  ccord_component c = {
    .kind = (ccord_component_kind)ct,
    .label = astr(a, cj, "label", CCORD_RICH_SHORT_MAX_BYTES),
    .custom_id = astr(a, cj, "custom_id", CCORD_RICH_CUSTOM_ID_MAX_BYTES),
    .url = astr(a, cj, "url", CCORD_RICH_URL_MAX_BYTES),
    .placeholder = astr(a, cj, "placeholder", CCORD_RICH_SHORT_MAX_BYTES),
    .emoji = "",
    .disabled = disj && ccord_json_bool(disj, false),
  };
  int64_t style = json_i64(cj, "style", ct == CCORD_COMP_BUTTON ? CCORD_BTN_SECONDARY : 0);
  c.style = (style >= 0 && style <= UINT8_MAX) ? (uint8_t)style : 0u;
  const ccord_json *ej = ccord_json_get(cj, "emoji");
  if (is_json_object(ej)) {
    c.emoji = astr(a, ej, "name", CCORD_EMOJI_MAX_BYTES);
    (void)json_id_key(ej, "id", &c.emoji_id);
  }
  if (ct == CCORD_COMP_BUTTON) {
    if (c.style == CCORD_BTN_LINK && c.url[0] == '\0') return false;
    if (c.style != CCORD_BTN_LINK && c.style != CCORD_BTN_PREMIUM && c.custom_id[0] == '\0') return false;
  } else {
    if (c.custom_id[0] == '\0') return false;
    int64_t minv = json_i64(cj, "min_values", 1);
    int64_t maxv = json_i64(cj, "max_values", 1);
    c.min_values = minv < 0 ? 0u
                            : (minv > (int64_t)CCORD_RICH_MAX_SELECT_OPTIONS ? CCORD_RICH_MAX_SELECT_OPTIONS
                                                                             : (uint8_t)minv);
    c.max_values = maxv < 1 ? 1u
                            : (maxv > (int64_t)CCORD_RICH_MAX_SELECT_OPTIONS ? CCORD_RICH_MAX_SELECT_OPTIONS
                                                                             : (uint8_t)maxv);
    if (ct == CCORD_COMP_STRING_SELECT) {
      c.options = rich_select_options(a, ccord_json_get(cj, "options"), &c.option_count);
    }
  }
  *out = c;
  return true;
}

static ccord_component_row *rich_components(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t nr = is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (nr > CCORD_RICH_MAX_COMPONENT_ROWS) nr = CCORD_RICH_MAX_COMPONENT_ROWS;
  ccord_component_row *rows = rich_alloc(a, nr, sizeof *rows);
  for (size_t ri = 0; rows && ri < nr; ri++) {
    const ccord_json *rj = ccord_json_array_at(arr, ri);
    if (!is_json_object(rj) || json_i64(rj, "type", 0) != CCORD_COMP_ACTION_ROW) continue;
    const ccord_json *carr = ccord_json_get(rj, "components");
    size_t nc = is_json_array(carr) ? ccord_json_array_len(carr) : 0;
    if (nc > CCORD_RICH_MAX_ROW_COMPONENTS) nc = CCORD_RICH_MAX_ROW_COMPONENTS;
    ccord_component *cs = rich_alloc(a, nc, sizeof *cs);
    size_t kept = 0;
    for (size_t ci = 0; cs && ci < nc; ci++) {
      const ccord_json *cj = ccord_json_array_at(carr, ci);
      if (!is_json_object(cj)) continue;
      int64_t ct = json_i64(cj, "type", 0);
      if (ct < CCORD_COMP_BUTTON || ct > CCORD_COMP_CHANNEL_SELECT || ct == CCORD_COMP_TEXT_INPUT) continue;
      ccord_component c;
      if (!comp_interactive(a, cj, ct, &c)) continue;
      cs[kept++] = c;
    }
    if (kept > 0) {
      rows[(*count)++] = (ccord_component_row){.components = cs, .component_count = kept};
    }
  }
  return *count ? rows : nullptr;
}

// ---------------------------------------------------------------- components v2
// The IS_COMPONENTS_V2 tree: layout and content nodes instead of action rows. Parsing truncates (node budget, depth,
// per-kind caps) and skips what it cannot place or does not know, so a newer or oversized payload still renders the
// rest; the result always satisfies ccord_message_rich_validate.

// Where a v2 node sits, which decides the kinds it may be (the model's placement rules).
typedef enum { COMP2_TOP, COMP2_CONTAINER, COMP2_SECTION_TEXT, COMP2_ACCESSORY, COMP2_ROW } comp2_where;

static bool comp2_allowed(comp2_where where, int64_t t)
{
  switch (where) {
    case COMP2_TOP:
      return t == CCORD_COMP_CONTAINER || t == CCORD_COMP_ACTION_ROW || t == CCORD_COMP_SECTION ||
             t == CCORD_COMP_TEXT_DISPLAY || t == CCORD_COMP_MEDIA_GALLERY || t == CCORD_COMP_FILE ||
             t == CCORD_COMP_SEPARATOR;
    case COMP2_CONTAINER:
      return t == CCORD_COMP_ACTION_ROW || t == CCORD_COMP_SECTION || t == CCORD_COMP_TEXT_DISPLAY ||
             t == CCORD_COMP_MEDIA_GALLERY || t == CCORD_COMP_FILE || t == CCORD_COMP_SEPARATOR;
    case COMP2_SECTION_TEXT: return t == CCORD_COMP_TEXT_DISPLAY;
    case COMP2_ACCESSORY: return t == CCORD_COMP_THUMBNAIL || t == CCORD_COMP_BUTTON;
    case COMP2_ROW:
      return t == CCORD_COMP_BUTTON ||
             (t >= CCORD_COMP_STRING_SELECT && t <= CCORD_COMP_CHANNEL_SELECT && t != CCORD_COMP_TEXT_INPUT);
  }
  return false;
}

// One message's whole tree shares the node budget; only kept nodes count against it.
typedef struct { ccord_arena *a; size_t nodes; } comp2_ctx;

static bool comp2_parse(comp2_ctx *cx, const ccord_json *j, unsigned depth, comp2_where where, ccord_component *out);

// Up to `cap` children of one placement into a fresh arena array; returns how many were kept.
static size_t comp2_children(comp2_ctx *cx, const ccord_json *arr, unsigned depth, comp2_where where, size_t cap,
                             const ccord_component **out)
{
  *out = nullptr;
  size_t n = is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  ccord_component *kids = rich_alloc(cx->a, n < cap ? n : cap, sizeof *kids);   // skipped entries do not use up the cap
  size_t kept = 0;
  for (size_t i = 0; kids && i < n && kept < cap; i++) {
    ccord_component c;
    if (comp2_parse(cx, ccord_json_array_at(arr, i), depth, where, &c)) kids[kept++] = c;
  }
  if (kept == 0) return 0;
  *out = kids;
  return kept;
}

// An unfurled media item (Thumbnail `media`, gallery item `media`, File `file`). Description and spoiler live on the
// component or gallery item around it, not here.
static void comp2_media(ccord_arena *a, const ccord_json *m, ccord_media_ref *out)
{
  *out = (ccord_media_ref){.url = "", .proxy_url = "", .content_type = "", .description = ""};
  if (!is_json_object(m)) return;
  out->url = aurl(a, m, "url");
  out->proxy_url = aurl(a, m, "proxy_url");
  out->width = clamp_u32(json_i64(m, "width", 0));
  out->height = clamp_u32(json_i64(m, "height", 0));
  out->content_type = astr(a, m, "content_type", CCORD_RICH_SHORT_MAX_BYTES);
}

static bool comp2_has_media(const ccord_media_ref *m)
{
  return m->url[0] != '\0' || m->proxy_url[0] != '\0';
}

// The per-kind fields of one v2 node, false when it cannot be kept (a required field is missing). Its budget slot is
// already reserved by comp2_parse; children reserve their own.
static bool comp2_fields(comp2_ctx *cx, const ccord_json *j, int64_t t, unsigned depth, ccord_component *c)
{
  ccord_arena *a = cx->a;
  switch (t) {
    case CCORD_COMP_BUTTON:
    case CCORD_COMP_STRING_SELECT:
    case CCORD_COMP_USER_SELECT:
    case CCORD_COMP_ROLE_SELECT:
    case CCORD_COMP_MENTIONABLE_SELECT:
    case CCORD_COMP_CHANNEL_SELECT:
      return comp_interactive(a, j, t, c);
    case CCORD_COMP_ACTION_ROW:
      c->child_count = comp2_children(cx, ccord_json_get(j, "components"), depth + 1u, COMP2_ROW,
                                      CCORD_RICH_MAX_ROW_COMPONENTS, &c->children);
      return c->child_count > 0;
    case CCORD_COMP_SECTION: {
      ccord_component acc;
      if (!comp2_parse(cx, ccord_json_get(j, "accessory"), depth + 1u, COMP2_ACCESSORY, &acc)) return false;
      c->child_count = comp2_children(cx, ccord_json_get(j, "components"), depth + 1u, COMP2_SECTION_TEXT,
                                      CCORD_RICH_MAX_V2_SECTION_TEXTS, &c->children);
      if (c->child_count == 0) return false;
      ccord_component *ap = rich_alloc(a, 1, sizeof *ap);
      if (!ap) return false;
      *ap = acc;
      c->accessory = ap;
      return true;
    }
    case CCORD_COMP_TEXT_DISPLAY:
      c->content = astr(a, j, "content", CCORD_RICH_TEXT_MAX_BYTES);
      return true;
    case CCORD_COMP_THUMBNAIL:
      comp2_media(a, ccord_json_get(j, "media"), &c->media);
      if (!comp2_has_media(&c->media)) return false;
      c->media.description = astr(a, j, "description", CCORD_RICH_FIELD_VALUE_MAX_BYTES);
      c->media.spoiler = json_bool_key(j, "spoiler", false);
      return true;
    case CCORD_COMP_MEDIA_GALLERY: {
      const ccord_json *items = ccord_json_get(j, "items");
      size_t n = is_json_array(items) ? ccord_json_array_len(items) : 0;
      if (n > CCORD_RICH_MAX_V2_GALLERY_ITEMS) n = CCORD_RICH_MAX_V2_GALLERY_ITEMS;
      ccord_media_ref *g = rich_alloc(a, n, sizeof *g);
      size_t kept = 0;
      for (size_t i = 0; g && i < n; i++) {
        const ccord_json *ij = ccord_json_array_at(items, i);
        if (!is_json_object(ij)) continue;
        ccord_media_ref mr;
        comp2_media(a, ccord_json_get(ij, "media"), &mr);
        if (!comp2_has_media(&mr)) continue;
        mr.description = astr(a, ij, "description", CCORD_RICH_FIELD_VALUE_MAX_BYTES);
        mr.spoiler = json_bool_key(ij, "spoiler", false);
        g[kept++] = mr;
      }
      if (kept == 0) return false;
      c->gallery = g;
      c->gallery_count = kept;
      return true;
    }
    case CCORD_COMP_FILE: {
      comp2_media(a, ccord_json_get(j, "file"), &c->media);
      if (!comp2_has_media(&c->media)) return false;
      c->spoiler = json_bool_key(j, "spoiler", false);
      c->file_name = astr(a, j, "name", CCORD_RICH_SHORT_MAX_BYTES);
      int64_t sz = json_i64(j, "size", 0);
      c->file_size = sz > 0 ? (uint64_t)sz : 0u;
      return true;
    }
    case CCORD_COMP_SEPARATOR: {
      c->divider = json_bool_key(j, "divider", true);   // Discord's default is a visible divider
      int64_t sp = json_i64(j, "spacing", 1);
      c->spacing = sp == 2 ? 2u : 1u;
      return true;
    }
    case CCORD_COMP_CONTAINER: {
      int64_t ac = json_i64(j, "accent_color", -1);
      if (ac >= 0 && ac <= 0xFFFFFF) {
        c->accent_color = (uint32_t)ac;
        c->has_accent_color = true;
      }
      c->spoiler = json_bool_key(j, "spoiler", false);
      c->child_count = comp2_children(cx, ccord_json_get(j, "components"), depth + 1u, COMP2_CONTAINER,
                                      CCORD_RICH_MAX_V2_NODES, &c->children);
      return true;
    }
    default: return false;
  }
}

// One node of the tree, false when it must be skipped (unknown kind, wrong placement, missing required field, or the
// budget/depth is spent). The slot is reserved before the children parse, so an oversized tree truncates its deepest
// nodes instead of dropping whole branches, and the kept tree always fits the budget.
static bool comp2_parse(comp2_ctx *cx, const ccord_json *j, unsigned depth, comp2_where where, ccord_component *out)
{
  if (!is_json_object(j) || depth > CCORD_RICH_MAX_V2_DEPTH || cx->nodes >= CCORD_RICH_MAX_V2_NODES) return false;
  int64_t t = json_i64(j, "type", 0);
  if (!comp2_allowed(where, t)) return false;
  size_t before = cx->nodes;
  cx->nodes++;
  ccord_component c = {.kind = (ccord_component_kind)t};
  if (!comp2_fields(cx, j, t, depth, &c)) {
    cx->nodes = before;   // also gives back the slots of children parsed before this node was dropped
    return false;
  }
  *out = c;
  return true;
}

static ccord_component *rich_components_v2(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  ccord_component *nodes = rich_alloc(a, n < CCORD_RICH_MAX_V2_NODES ? n : CCORD_RICH_MAX_V2_NODES, sizeof *nodes);
  comp2_ctx cx = {.a = a, .nodes = 0};
  for (size_t i = 0; nodes && i < n && *count < CCORD_RICH_MAX_V2_NODES; i++) {
    ccord_component c;
    if (comp2_parse(&cx, ccord_json_array_at(arr, i), 1u, COMP2_TOP, &c)) nodes[(*count)++] = c;
  }
  return *count ? nodes : nullptr;
}

// A v2 payload: the IS_COMPONENTS_V2 flag says so, or a top-level node is a v2-only kind (covers an update that
// carries components without the flag).
static bool components_are_v2(const ccord_json *arr, uint32_t flags)
{
  if ((flags & CCORD_MSGFLAG_IS_COMPONENTS_V2) != 0u) return true;
  size_t n = is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *cj = ccord_json_array_at(arr, i);
    if (!is_json_object(cj)) continue;
    int64_t t = json_i64(cj, "type", 0);
    if ((t >= CCORD_COMP_SECTION && t <= CCORD_COMP_SEPARATOR) || t == CCORD_COMP_CONTAINER) return true;
  }
  return false;
}

// `partial` is a MESSAGE_UPDATE: Discord then sends only what changed, so a part counts as carried only when its key is
// there (an empty list under the key means "now empty"). Whole messages carry every part. Returns nullptr when a whole
// message has nothing beyond its text.
static const ccord_message_rich *map_rich(ccord_arena *a, const ccord_json *j, bool partial)
{
  if (!is_json_object(j)) return nullptr;
  ccord_message_rich r = {0};
  r.ref_preview.author_name = r.ref_preview.content = "";
  r.forward.content = "";
  r.poll.question = "";
  r.thread.name = "";
  uint32_t present = 0;
  bool any = false;

  const ccord_json *tj = ccord_json_get(j, "type");
  const ccord_json *fj = ccord_json_get(j, "flags");
  if (tj || fj) present |= CCORD_RICH_HAS_TYPE;
  int64_t type = json_i64(j, "type", 0);
  r.type = (type >= 0 && type <= UINT16_MAX) ? (uint16_t)type : 0u;
  r.flags = clamp_u32(json_i64(j, "flags", 0));
  any |= r.type != 0 || r.flags != 0;

  const ccord_json *mu = ccord_json_get(j, "mentions");
  const ccord_json *mr = ccord_json_get(j, "mention_roles");
  const ccord_json *me = ccord_json_get(j, "mention_everyone");
  if (mu || mr || me) present |= CCORD_RICH_HAS_MENTIONS;
  r.mention_users = rich_user_ids(a, mu, &r.mention_user_count);
  r.mention_roles = rich_role_ids(a, mr, &r.mention_role_count);
  r.mention_everyone = me && ccord_json_bool(me, false);
  any |= r.mention_user_count || r.mention_role_count || r.mention_everyone;

  const ccord_json *at = ccord_json_get(j, "attachments");
  if (at) present |= CCORD_RICH_HAS_ATTACHMENTS;
  r.attachments = rich_attachments(a, at, &r.attachment_count);
  any |= r.attachment_count > 0;

  const ccord_json *em = ccord_json_get(j, "embeds");
  if (em) present |= CCORD_RICH_HAS_EMBEDS;
  r.embeds = rich_embeds(a, em, &r.embed_count);
  any |= r.embed_count > 0;

  const ccord_json *st = ccord_json_get(j, "sticker_items");
  if (!st) st = ccord_json_get(j, "stickers");
  if (st) present |= CCORD_RICH_HAS_STICKERS;
  r.stickers = rich_stickers(a, st, &r.sticker_count);
  any |= r.sticker_count > 0;

  const ccord_json *ref = ccord_json_get(j, "message_reference");
  if (ref || ccord_json_get(j, "referenced_message")) present |= CCORD_RICH_HAS_REFERENCE;
  if (is_json_object(ref)) {
    int64_t rt = json_i64(ref, "type", 0);
    ccord_id mid = CCORD_ID_NONE;
    (void)json_id_key(ref, "message_id", &mid);
    (void)json_id_key(ref, "channel_id", &r.reference.channel_id);
    (void)json_id_key(ref, "guild_id", &r.reference.guild_id);
    r.reference.message_id = mid;
    r.reference.kind = rt == 1 ? CCORD_REF_FORWARD : (mid != CCORD_ID_NONE ? CCORD_REF_REPLY : CCORD_REF_NONE);
  }
  rich_ref_preview(a, j, &r.ref_preview);
  if (r.reference.kind == CCORD_REF_NONE && (r.ref_preview.present || r.ref_preview.deleted)) r.reference.kind = CCORD_REF_REPLY;
  any |= r.reference.kind != CCORD_REF_NONE || r.ref_preview.present || r.ref_preview.deleted;

  if (ccord_json_get(j, "message_snapshots")) present |= CCORD_RICH_HAS_FORWARD;
  rich_forward(a, j, &r.forward);
  // A type-1 reference alone marks a forward: Discord can deliver it before (or without) the snapshot,
  // and the snapshot content then fills in on a later update.
  if (r.reference.kind == CCORD_REF_FORWARD) {
    present |= CCORD_RICH_HAS_FORWARD;
    r.forward.present = true;
  }
  any |= r.forward.present;

  const ccord_json *pj = ccord_json_get(j, "poll");
  if (pj) present |= CCORD_RICH_HAS_POLL;
  rich_poll(a, pj, &r.poll);
  any |= r.poll.present;

  const ccord_json *th = ccord_json_get(j, "thread");
  if (th) present |= CCORD_RICH_HAS_THREAD;
  if (is_json_object(th) && json_id_key(th, "id", &r.thread.id)) {
    r.thread.name = astr(a, th, "name", CCORD_RICH_SHORT_MAX_BYTES);
    r.thread.message_count = clamp_u32(json_i64(th, "message_count", 0));
    any = true;
  }

  (void)json_id_key(j, "application_id", &r.application_id);
  if (r.application_id == CCORD_ID_NONE) {
    const ccord_json *au = ccord_json_get(j, "author");
    const ccord_json *botj = is_json_object(au) ? ccord_json_get(au, "bot") : nullptr;
    if (botj && ccord_json_bool(botj, false)) {
      (void)json_id_key(au, "id", &r.application_id);
    }
  }
  const ccord_json *cmps = ccord_json_get(j, "components");
  if (cmps) present |= CCORD_RICH_HAS_COMPONENTS;
  if (components_are_v2(cmps, r.flags)) {
    r.v2_components = rich_components_v2(a, cmps, &r.v2_component_count);
    any |= r.v2_component_count > 0;
  } else {
    r.components = rich_components(a, cmps, &r.component_row_count);
    any |= r.component_row_count > 0;
  }

  if (!partial) present = CCORD_RICH_ALL;
  const ccord_json *cj = ccord_json_get(j, "content");
  size_t clen = 0;
  r.text_absent = partial && !(cj && ccord_json_string(cj, &clen) != nullptr);
  if (partial ? present == 0 : !any) return nullptr;
  r.present = present;
  ccord_message_rich *out = ccord_arena_alloc(a, sizeof *out, _Alignof(ccord_message_rich));
  if (!out) return nullptr;
  *out = r;
  return out;
}

static bool map_message_ex(ccord_arena *a, const ccord_json *j, ccord_id fallback_channel,
                           bool allow_missing_author, ccord_message *out)
{
  if (!j) return false;
  ccord_id id = CCORD_ID_NONE;
  if (!json_id_key(j, "id", &id)) return false;
  ccord_id channel = fallback_channel;
  if (!json_id_key(j, "channel_id", &channel) && channel == CCORD_ID_NONE) return false;
  const ccord_json *author = ccord_json_get(j, "author");
  ccord_id author_id = CCORD_ID_NONE;
  if (!author || !json_id_key(author, "id", &author_id)) {
    if (!json_id_key(j, "author_id", &author_id) && !json_id_key(j, "webhook_id", &author_id)) {
      if (!allow_missing_author) author_id = id;
    }
  }
  const char *content = jstr(j, "content");
  if (!content) content = "";
  const char *ts = jstr(j, "timestamp");
  size_t ts_len = 0;
  if (ts) ts_len = strlen(ts);
  int64_t stamp = iso_ms(ts, ts_len);
  if (stamp < 0) stamp = 0;
  const char *edited = jstr(j, "edited_timestamp");
  int64_t edited_ms = 0;
  if (edited && edited[0] != '\0') {
    edited_ms = iso_ms(edited, strlen(edited));
    if (edited_ms < 0) edited_ms = 0;
  }
  uint32_t reactions = 0;
  bool self = false;
  message_reactions(j, &reactions, &self);
  ccord_id reply_to = CCORD_ID_NONE;
  const ccord_json *ref = ccord_json_get(j, "message_reference");
  if (ref && json_i64(ref, "type", 0) != 1) (void)json_id_key(ref, "message_id", &reply_to);   // type 1 is a forward
  const ccord_json *contentj = ccord_json_get(j, "content");
  const ccord_json *pinj = ccord_json_get(j, "pinned");
  const ccord_json *ttsj = ccord_json_get(j, "tts");
  memset(out, 0, sizeof *out);
  out->id = id;
  out->channel_id = channel;
  out->author_id = author_id;
  out->timestamp_ms = stamp;
  out->content = clamp_message_text(a, content);
  out->edited_timestamp_ms = edited_ms;
  out->reaction_count = reactions;
  out->self_reacted = self;
  out->reactions = nullptr;
  out->reaction_kind_count = 0;
  map_reaction_list(a, j, out);
  out->edited = edited_ms > 0;
  out->reply_to_id = reply_to;
  out->pinned = pinj && ccord_json_bool(pinj, false);
  out->tts = json_bool_key(j, "tts", false);
  out->has_pinned = pinj != nullptr && ccord_json_type_of(pinj) == CCORD_JSON_BOOL;
  out->omit_content = allow_missing_author && (!contentj || ccord_json_type_of(contentj) != CCORD_JSON_STRING);
  out->omit_tts = allow_missing_author && (!ttsj || ccord_json_type_of(ttsj) != CCORD_JSON_BOOL);
  // M0-3: the server echoes the client nonce back on the sender's message; it confirms the pending local echo.
  const char *nonce = jstr(j, "nonce");
  out->nonce[0] = '\0';
  if (nonce) {
    size_t nlen = strlen(nonce);
    if (nlen >= sizeof out->nonce) nlen = sizeof out->nonce - 1u;
    memcpy(out->nonce, nonce, nlen);
    out->nonce[nlen] = '\0';
  }
  out->send_state = CCORD_MSG_SEND_CONFIRMED;
  out->rich = map_rich(a, j, allow_missing_author);
  return true;
}

static bool map_message(ccord_arena *a, const ccord_json *j, ccord_id fallback_channel, ccord_message *out)
{
  return map_message_ex(a, j, fallback_channel, false, out);
}

typedef struct pair_map_entry {
  ccord_id k1;
  ccord_id k2;
  size_t slot;   // array index + 1; 0 marks a free slot
} pair_map_entry;

typedef struct pair_map {
  pair_map_entry *entries;
  size_t count;
  size_t cap;    // power of two, or 0 before the first put
} pair_map;

static uint64_t pair_hash(ccord_id a, ccord_id b)
{
  uint64_t h = (a * 0x9e3779b97f4a7c15ULL) ^ b;
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ULL;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebULL;
  h ^= h >> 31;
  return h;
}

static bool pair_map_find(const pair_map *m, ccord_id k1, ccord_id k2, size_t *value)
{
  if (!m || !m->entries || m->cap == 0) return false;
  size_t mask = m->cap - 1u;
  size_t at = (size_t)pair_hash(k1, k2) & mask;
  for (;;) {
    const pair_map_entry *e = &m->entries[at];
    if (e->slot == 0) return false;
    if (e->k1 == k1 && e->k2 == k2) {
      if (value) *value = e->slot - 1u;
      return true;
    }
    at = (at + 1u) & mask;
  }
}

static bool pair_map_put(pair_map *m, ccord_id k1, ccord_id k2, size_t value)
{
  if (!m || value == SIZE_MAX) return false;
  if (!m->entries || (m->count + 1u) * 2u >= m->cap) {
    if (m->cap > SIZE_MAX / 2u) return false;
    size_t ncap = m->cap ? m->cap * 2u : 16u;
    pair_map_entry *ne = ccord_calloc(ncap, sizeof *ne);
    if (!ne) return false;
    size_t nmask = ncap - 1u;
    for (size_t i = 0; i < m->cap; i++) {
      if (m->entries[i].slot == 0) continue;
      size_t at = (size_t)pair_hash(m->entries[i].k1, m->entries[i].k2) & nmask;
      while (ne[at].slot != 0) at = (at + 1u) & nmask;
      ne[at] = m->entries[i];
    }
    ccord_free(m->entries);
    m->entries = ne;
    m->cap = ncap;
  }
  size_t mask = m->cap - 1u;
  size_t at = (size_t)pair_hash(k1, k2) & mask;
  for (;;) {
    pair_map_entry *e = &m->entries[at];
    if (e->slot == 0) {
      e->k1 = k1;
      e->k2 = k2;
      e->slot = value + 1u;
      m->count++;
      return true;
    }
    if (e->k1 == k1 && e->k2 == k2) {
      e->slot = value + 1u;
      return true;
    }
    at = (at + 1u) & mask;
  }
}

static void pair_map_free(pair_map *m)
{
  if (!m) return;
  ccord_free(m->entries);
  *m = (pair_map){0};
}

typedef struct snapshot_builder {
  ccord_arena *arena;
  ccord_id self_id;
  ccord_user *users;
  size_t user_count, user_cap;
  id_index user_idx;
  ccord_guild *guilds;
  size_t guild_count, guild_cap;
  id_index guild_idx;
  ccord_channel *channels;
  size_t channel_count, channel_cap;
  id_index chan_idx;
  ccord_member *members;
  size_t member_count, member_cap;
  pair_map member_idx;
  id_index guild_mem_counts;
  ccord_voice_state *voices;
  size_t voice_count, voice_cap;
  pair_map voice_idx;
  ccord_relationship *relationships;
  size_t relationship_count, relationship_cap;
  id_index rel_idx;
  ccord_forum_meta *forum_metas;
  size_t forum_meta_count, forum_meta_cap;
  id_index forum_meta_idx;
  ccord_forum_post *forum_posts;
  size_t forum_post_count, forum_post_cap;
  id_index forum_post_idx;
} snapshot_builder;

static bool grow(void **items, size_t *cap, size_t need, size_t size)
{
  if (need <= *cap) return true;
  size_t ncap = *cap ? *cap * 2 : 16;
  while (ncap < need) ncap *= 2;
  void *p = ccord_realloc_array(*items, ncap, size);
  if (!p) return false;
  *items = p;
  *cap = ncap;
  return true;
}

static const char *build_cdn_url_sized(ccord_arena *a, const char *kind, ccord_id id, const char *hash,
                                       const char *size_str)
{
  if (!a || !kind || id == CCORD_ID_NONE || !hash || hash[0] == '\0') return "";
  size_t hlen = strlen(hash);
  if (hlen > 128u || !ccord_utf8_valid_text(hash, hlen)) return "";
  char idbuf[21];
  int idlen = ccord_id_format(id, idbuf);
  const char *prefix = "https://cdn.discordapp.com/";
  bool is_anim = (hlen > 2u && hash[0] == 'a' && hash[1] == '_');
  const char *sz = size_str ? size_str : (strcmp(kind, "banners") == 0 ? "512" : "64");
  char suffix[24];
  snprintf(suffix, sizeof suffix, "%s?size=%s", is_anim ? ".gif" : ".png", sz);
  size_t plen = strlen(prefix), klen = strlen(kind), slen = strlen(suffix);
  size_t total = plen + klen + 1u + (size_t)idlen + 1u + hlen + slen;
  if (total > 512u) return "";
  char *out = ccord_arena_alloc(a, total + 1u, 1);
  if (!out) return "";
  size_t pos = 0;
  memcpy(out + pos, prefix, plen); pos += plen;
  memcpy(out + pos, kind, klen); pos += klen;
  out[pos++] = '/';
  memcpy(out + pos, idbuf, (size_t)idlen); pos += (size_t)idlen;
  out[pos++] = '/';
  memcpy(out + pos, hash, hlen); pos += hlen;
  memcpy(out + pos, suffix, slen); pos += slen;
  out[pos] = '\0';
  return out;
}

static const char *build_cdn_url(ccord_arena *a, const char *kind, ccord_id id, const char *hash)
{
  return build_cdn_url_sized(a, kind, id, hash, nullptr);
}

static bool sb_user(snapshot_builder *b, const ccord_user *u)
{
  size_t i = 0;
  if (ccord_idmap_find(&b->user_idx, u->id, &i)) {
    bool existing_is_stub = !b->users[i].name || b->users[i].name[0] == '\0' ||
                            ((!b->users[i].handle || b->users[i].handle[0] == '\0') &&
                             strncmp(b->users[i].name, "User ", 5) == 0);
    if (existing_is_stub && u->name && u->name[0] != '\0' && strncmp(u->name, "User ", 5) != 0) {
      b->users[i].name = u->name;
    }
    if ((!b->users[i].handle || b->users[i].handle[0] == '\0') &&
        u->handle && u->handle[0] != '\0') {
      b->users[i].handle = u->handle;
    }
    if ((!b->users[i].avatar_url || b->users[i].avatar_url[0] == '\0') &&
        u->avatar_url && u->avatar_url[0] != '\0') {
      b->users[i].avatar_url = u->avatar_url;
    }
    if ((!b->users[i].banner_url || b->users[i].banner_url[0] == '\0') &&
        u->banner_url && u->banner_url[0] != '\0') {
      b->users[i].banner_url = u->banner_url;
    }
    if (b->users[i].accent_color == 0 && u->accent_color != 0) {
      b->users[i].accent_color = u->accent_color;
    }
    if ((!b->users[i].bio || b->users[i].bio[0] == '\0') &&
        u->bio && u->bio[0] != '\0') {
      b->users[i].bio = u->bio;
    }
    if ((!b->users[i].pronouns || b->users[i].pronouns[0] == '\0') &&
        u->pronouns && u->pronouns[0] != '\0') {
      b->users[i].pronouns = u->pronouns;
    }
    if ((!b->users[i].status_text || b->users[i].status_text[0] == '\0') &&
        u->status_text && u->status_text[0] != '\0') {
      b->users[i].status_text = u->status_text;
    }
    if (b->users[i].presence == CCORD_PRESENCE_OFFLINE && u->presence != CCORD_PRESENCE_OFFLINE) {
      b->users[i].presence = u->presence;
    }
    return true;
  }
  if (!grow((void **)&b->users, &b->user_cap, b->user_count + 1, sizeof *b->users)) return false;
  if (u->id == CCORD_ID_NONE || !ccord_idmap_put(&b->user_idx, u->id, b->user_count)) return false;
  b->users[b->user_count++] = *u;
  return true;
}

static bool sb_relationship(snapshot_builder *b, const ccord_relationship *rel)
{
  if (!rel || rel->user_id == CCORD_ID_NONE || rel->kind == CCORD_REL_NONE) return true;
  size_t i = 0;
  if (ccord_idmap_find(&b->rel_idx, rel->user_id, &i)) {
    b->relationships[i].kind = rel->kind;
    return true;
  }
  if (b->relationship_count >= CCORD_MAX_RELATIONSHIPS) return true;
  if (!grow((void **)&b->relationships, &b->relationship_cap, b->relationship_count + 1, sizeof *b->relationships)) {
    return false;
  }
  if (!ccord_idmap_put(&b->rel_idx, rel->user_id, b->relationship_count)) return false;
  b->relationships[b->relationship_count++] = *rel;
  return true;
}

static bool sb_guild(snapshot_builder *b, const ccord_guild *g)
{
  if (ccord_idmap_find(&b->guild_idx, g->id, nullptr)) return true;
  if (!grow((void **)&b->guilds, &b->guild_cap, b->guild_count + 1, sizeof *b->guilds)) return false;
  if (!ccord_idmap_put(&b->guild_idx, g->id, b->guild_count)) return false;
  b->guilds[b->guild_count++] = *g;
  return true;
}

static bool sb_channel(snapshot_builder *b, const ccord_channel *c)
{
  if (ccord_idmap_find(&b->chan_idx, c->id, nullptr)) return true;
  if (!grow((void **)&b->channels, &b->channel_cap, b->channel_count + 1, sizeof *b->channels)) return false;
  if (c->id == CCORD_ID_NONE || !ccord_idmap_put(&b->chan_idx, c->id, b->channel_count)) return false;
  b->channels[b->channel_count++] = *c;
  return true;
}

static bool sb_member(snapshot_builder *b, const ccord_member *m)
{
  size_t i = 0;
  if (pair_map_find(&b->member_idx, m->guild_id, m->user_id, &i)) {
    bool existing_default = (!b->members[i].role_name || b->members[i].role_name[0] == '\0' ||
                             strcmp(b->members[i].role_name, "Members") == 0) &&
                            b->members[i].role_rank == 255 && b->members[i].role_color == 0;
    bool incoming_default = (!m->role_name || m->role_name[0] == '\0' ||
                             strcmp(m->role_name, "Members") == 0) &&
                            m->role_rank == 255 && m->role_color == 0;
    if (!incoming_default || existing_default) {
      b->members[i] = *m;
    }
    return true;
  }
  size_t per_guild = 0;
  (void)ccord_idmap_find(&b->guild_mem_counts, m->guild_id, &per_guild);
  if (per_guild >= CCORD_MAX_MEMBERS_PER_GUILD) return true;
  if (!grow((void **)&b->members, &b->member_cap, b->member_count + 1, sizeof *b->members)) return false;
  if (!pair_map_put(&b->member_idx, m->guild_id, m->user_id, b->member_count)) return false;
  if (!ccord_idmap_put(&b->guild_mem_counts, m->guild_id, per_guild + 1u)) return false;
  b->members[b->member_count++] = *m;
  return true;
}

static bool sb_voice(snapshot_builder *b, const ccord_voice_state *v)
{
  size_t i = 0;
  if (pair_map_find(&b->voice_idx, v->guild_id, v->user_id, &i)) {
    b->voices[i] = *v;
    return true;
  }
  if (!grow((void **)&b->voices, &b->voice_cap, b->voice_count + 1, sizeof *b->voices)) return false;
  if (!pair_map_put(&b->voice_idx, v->guild_id, v->user_id, b->voice_count)) return false;
  b->voices[b->voice_count++] = *v;
  return true;
}

static bool sb_forum_meta(snapshot_builder *b, const ccord_forum_meta *m)
{
  if (!m || m->forum_id == CCORD_ID_NONE) return true;
  size_t i = 0;
  if (ccord_idmap_find(&b->forum_meta_idx, m->forum_id, &i)) {
    b->forum_metas[i] = *m;
    return true;
  }
  if (!grow((void **)&b->forum_metas, &b->forum_meta_cap, b->forum_meta_count + 1, sizeof *b->forum_metas)) {
    return false;
  }
  if (!ccord_idmap_put(&b->forum_meta_idx, m->forum_id, b->forum_meta_count)) return false;
  b->forum_metas[b->forum_meta_count++] = *m;
  return true;
}

static bool sb_forum_post(snapshot_builder *b, const ccord_forum_post *p)
{
  if (!p || p->thread_id == CCORD_ID_NONE || p->forum_id == CCORD_ID_NONE) return true;
  size_t i = 0;
  if (ccord_idmap_find(&b->forum_post_idx, p->thread_id, &i)) {
    b->forum_posts[i] = *p;
    return true;
  }
  if (!grow((void **)&b->forum_posts, &b->forum_post_cap, b->forum_post_count + 1, sizeof *b->forum_posts)) {
    return false;
  }
  if (!ccord_idmap_put(&b->forum_post_idx, p->thread_id, b->forum_post_count)) return false;
  b->forum_posts[b->forum_post_count++] = *p;
  return true;
}

static void sb_free(snapshot_builder *b)
{
  ccord_idmap_free(&b->user_idx);
  ccord_idmap_free(&b->guild_idx);
  ccord_idmap_free(&b->chan_idx);
  pair_map_free(&b->member_idx);
  ccord_idmap_free(&b->guild_mem_counts);
  pair_map_free(&b->voice_idx);
  ccord_idmap_free(&b->rel_idx);
  ccord_idmap_free(&b->forum_meta_idx);
  ccord_idmap_free(&b->forum_post_idx);
  ccord_free(b->users);
  ccord_free(b->guilds);
  ccord_free(b->channels);
  ccord_free(b->members);
  ccord_free(b->voices);
  ccord_free(b->relationships);
  ccord_free(b->forum_metas);
  ccord_free(b->forum_posts);
  memset(b, 0, sizeof *b);
}

static bool sb_publish(snapshot_builder *b, ccord_live_event *ev)
{
  ccord_arena *a = b->arena;
  if (b->user_count > 0) {
    ev->users = ccord_arena_alloc(a, b->user_count * sizeof *ev->users, _Alignof(ccord_user));
    if (!ev->users) return false;
    memcpy(ev->users, b->users, b->user_count * sizeof *ev->users);
    ev->user_count = b->user_count;
  }
  if (b->guild_count > 0) {
    ev->guilds = ccord_arena_alloc(a, b->guild_count * sizeof *ev->guilds, _Alignof(ccord_guild));
    if (!ev->guilds) return false;
    memcpy(ev->guilds, b->guilds, b->guild_count * sizeof *ev->guilds);
    ev->guild_count = b->guild_count;
  }
  if (b->channel_count > 0) {
    ev->snap_channels =
      ccord_arena_alloc(a, b->channel_count * sizeof *ev->snap_channels, _Alignof(ccord_channel));
    if (!ev->snap_channels) return false;
    memcpy(ev->snap_channels, b->channels, b->channel_count * sizeof *ev->snap_channels);
    ev->snap_channel_count = b->channel_count;
  }
  if (b->member_count > 0) {
    ev->members = ccord_arena_alloc(a, b->member_count * sizeof *ev->members, _Alignof(ccord_member));
    if (!ev->members) return false;
    memcpy(ev->members, b->members, b->member_count * sizeof *ev->members);
    ev->member_count = b->member_count;
  }
  if (b->voice_count > 0) {
    ev->voice_states = ccord_arena_alloc(a, b->voice_count * sizeof *ev->voice_states, _Alignof(ccord_voice_state));
    if (!ev->voice_states) return false;
    memcpy(ev->voice_states, b->voices, b->voice_count * sizeof *ev->voice_states);
    ev->voice_state_count = b->voice_count;
  }
  if (b->relationship_count > 0) {
    ev->relationships =
      ccord_arena_alloc(a, b->relationship_count * sizeof *ev->relationships, _Alignof(ccord_relationship));
    if (!ev->relationships) return false;
    memcpy(ev->relationships, b->relationships, b->relationship_count * sizeof *ev->relationships);
    ev->relationship_count = b->relationship_count;
  }
  if (b->forum_meta_count > 0) {
    ccord_forum_meta *fms =
      ccord_arena_alloc(a, b->forum_meta_count * sizeof *fms, _Alignof(ccord_forum_meta));
    if (!fms) return false;
    memcpy(fms, b->forum_metas, b->forum_meta_count * sizeof *fms);
    ev->ev.forum_metas = fms;
    ev->ev.forum_meta_count = b->forum_meta_count;
  }
  if (b->forum_post_count > 0) {
    ccord_forum_post *fps =
      ccord_arena_alloc(a, b->forum_post_count * sizeof *fps, _Alignof(ccord_forum_post));
    if (!fps) return false;
    memcpy(fps, b->forum_posts, b->forum_post_count * sizeof *fps);
    ev->ev.forum_posts = fps;
    ev->ev.forum_post_count = b->forum_post_count;
  }
  // Drop parent links whose category or parent text channel was not present or belongs elsewhere.
  for (size_t i = 0; i < ev->snap_channel_count; i++) {
    ccord_channel *c = &ev->snap_channels[i];
    if (c->parent_id == CCORD_ID_NONE) continue;
    size_t pidx = 0;
    bool ok = false;
    if (ccord_idmap_find(&b->chan_idx, c->parent_id, &pidx) && pidx < ev->snap_channel_count) {
      const ccord_channel *p = &ev->snap_channels[pidx];
      if (p->id != c->id && p->guild_id == c->guild_id &&
          (p->kind == CCORD_CHANNEL_CATEGORY ||
           (c->kind == CCORD_CHANNEL_TEXT && p->kind == CCORD_CHANNEL_TEXT))) {
        ok = true;
      }
    }
    if (!ok) c->parent_id = CCORD_ID_NONE;
  }
  return true;
}

static bool sb_finish(snapshot_builder *b, ccord_live_event *ev)
{
  if (!sb_publish(b, ev)) return false;
  ev->snapshot.self_id = ev->user_count > 0 ? ev->users[0].id : CCORD_ID_NONE;
  ev->snapshot.users = ev->users;
  ev->snapshot.user_count = ev->user_count;
  ev->snapshot.guilds = ev->guilds;
  ev->snapshot.guild_count = ev->guild_count;
  ev->snapshot.channels = ev->snap_channels;
  ev->snapshot.channel_count = ev->snap_channel_count;
  ev->snapshot.members = ev->members;
  ev->snapshot.member_count = ev->member_count;
  ev->snapshot.voice_states = ev->voice_states;
  ev->snapshot.voice_state_count = ev->voice_state_count;
  ev->snapshot.relationships = ev->relationships;
  ev->snapshot.relationship_count = ev->relationship_count;
  return true;
}

static ccord_presence presence_from(const char *status);

static const ccord_json *guild_props(const ccord_json *gj)
{
  const ccord_json *p = gj ? ccord_json_get(gj, "properties") : nullptr;
  return (p && ccord_json_type_of(p) == CCORD_JSON_OBJECT) ? p : nullptr;
}

static const char *guild_str_p(const ccord_json *gj, const ccord_json *props, const char *key)
{
  const char *s = jstr(gj, key);
  if (s && s[0] != '\0') return s;
  return props ? jstr(props, key) : s;
}

static bool guild_id_key_p(const ccord_json *gj, const ccord_json *props, const char *key, ccord_id *out)
{
  if (json_id_key(gj, key, out)) return true;
  return props && json_id_key(props, key, out);
}

static const ccord_json *guild_arr_p(const ccord_json *gj, const ccord_json *props, const char *key)
{
  const ccord_json *r = gj ? ccord_json_get(gj, key) : nullptr;
  if (r && ccord_json_type_of(r) == CCORD_JSON_ARRAY) return r;
  r = props ? ccord_json_get(props, key) : nullptr;
  return (r && ccord_json_type_of(r) == CCORD_JSON_ARRAY) ? r : nullptr;
}

static int64_t guild_i64_p(const ccord_json *gj, const ccord_json *props, const char *key, int64_t def)
{
  const ccord_json *v = gj ? ccord_json_get(gj, key) : nullptr;
  int64_t out = 0;
  if (v && ccord_json_int64(v, &out)) return out;
  v = props ? ccord_json_get(props, key) : nullptr;
  if (v && ccord_json_int64(v, &out)) return out;
  return def;
}

static const ccord_json *guild_obj_p(const ccord_json *gj, const ccord_json *props, const char *key)
{
  const ccord_json *v = gj ? ccord_json_get(gj, key) : nullptr;
  if (v && ccord_json_type_of(v) == CCORD_JSON_OBJECT) return v;
  v = props ? ccord_json_get(props, key) : nullptr;
  return (v && ccord_json_type_of(v) == CCORD_JSON_OBJECT) ? v : nullptr;
}

static const ccord_json *guild_arr(const ccord_json *gj, const char *key)
{
  return guild_arr_p(gj, guild_props(gj), key);
}

static const ccord_json *guild_roles(const ccord_json *gj)
{
  return guild_arr_p(gj, guild_props(gj), "roles");
}

static bool parse_u64_str(const char *s, size_t len, uint64_t *out)
{
  if (!s || len == 0 || len > 20) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    uint64_t d = (uint64_t)(s[i] - '0');
    if (v > (UINT64_MAX - d) / 10u) return false;
    v = v * 10u + d;
  }
  *out = v;
  return true;
}

static bool json_u64_key(const ccord_json *obj, const char *key, uint64_t *out)
{
  const ccord_json *v = obj ? ccord_json_get(obj, key) : nullptr;
  if (!v) return false;
  size_t n = 0;
  const char *s = ccord_json_string(v, &n);
  if (s) return parse_u64_str(s, n, out);
  int64_t iv = 0;
  if (ccord_json_int64(v, &iv) && iv >= 0) {
    *out = (uint64_t)iv;
    return true;
  }
  return false;
}

#define DISCORD_PERM_ADMINISTRATOR (1ULL << 3)
#define DISCORD_PERM_VIEW_CHANNEL  (1ULL << 10)

typedef struct guild_perm_ctx {
  ccord_id guild_id;
  ccord_id self_id;
  ccord_id owner_id;
  uint64_t base_perms;
  ccord_id self_role_ids[CCORD_MAX_ROLES_PER_GUILD];
  size_t self_role_count;
  bool has_self_roles;
  bool is_admin;
} guild_perm_ctx;

static bool perm_has_self_role(const guild_perm_ctx *ctx, ccord_id rid)
{
  if (!ctx || rid == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < ctx->self_role_count; i++) {
    if (ctx->self_role_ids[i] == rid) return true;
  }
  return false;
}

static const ccord_json *extract_member_roles(const ccord_json *entry, ccord_id self_id, bool allow_unnamed)
{
  if (!entry) return nullptr;
  if (ccord_json_type_of(entry) == CCORD_JSON_OBJECT) {
    const ccord_json *mu = ccord_json_get(entry, "user");
    ccord_id uid = CCORD_ID_NONE;
    bool has_uid = (mu && json_id_key(mu, "id", &uid)) || json_id_key(entry, "user_id", &uid);
    if ((has_uid && uid == self_id) || (!has_uid && allow_unnamed)) {
      const ccord_json *r = ccord_json_get(entry, "roles");
      if (r && ccord_json_type_of(r) == CCORD_JSON_ARRAY) return r;
    }
    return nullptr;
  }
  if (ccord_json_type_of(entry) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(entry);
    for (size_t i = 0; i < n; i++) {
      const ccord_json *r = extract_member_roles(ccord_json_array_at(entry, i), self_id, false);
      if (r) return r;
    }
    if (allow_unnamed && n > 0) {
      return extract_member_roles(ccord_json_array_at(entry, 0), self_id, true);
    }
  }
  return nullptr;
}

static void init_guild_perm_ctx(guild_perm_ctx *ctx, const ccord_json *gj, const ccord_json *props,
                                const ccord_json *ready_merged_entry, ccord_id gid, ccord_id self_id)
{
  *ctx = (guild_perm_ctx){
    .guild_id = gid,
    .self_id = self_id,
    .base_perms = DISCORD_PERM_VIEW_CHANNEL,
  };
  (void)guild_id_key_p(gj, props, "owner_id", &ctx->owner_id);
  if (self_id == CCORD_ID_NONE || (ctx->owner_id != CCORD_ID_NONE && ctx->owner_id == self_id)) {
    ctx->is_admin = true;
    return;
  }
  const ccord_json *self_roles = extract_member_roles(guild_arr_p(gj, props, "members"), self_id, false);
  if (!self_roles && gj) {
    self_roles = extract_member_roles(ccord_json_get(gj, "merged_members"), self_id, true);
  }
  if (!self_roles && ready_merged_entry) {
    self_roles = extract_member_roles(ready_merged_entry, self_id, true);
  }
  if (self_roles) {
    ctx->has_self_roles = true;
    size_t nsr = ccord_json_array_len(self_roles);
    for (size_t i = 0; i < nsr && ctx->self_role_count < CCORD_MAX_ROLES_PER_GUILD; i++) {
      ccord_id rid = CCORD_ID_NONE;
      if (json_id(ccord_json_array_at(self_roles, i), &rid) && rid != CCORD_ID_NONE) {
        ctx->self_role_ids[ctx->self_role_count++] = rid;
      }
    }
  }
  const ccord_json *roles = guild_arr_p(gj, props, "roles");
  size_t nr = roles ? ccord_json_array_len(roles) : 0;
  bool found_everyone = false;
  uint64_t everyone_perms = DISCORD_PERM_VIEW_CHANNEL;
  uint64_t role_perms = 0;
  for (size_t i = 0; i < nr; i++) {
    const ccord_json *rj = ccord_json_array_at(roles, i);
    ccord_id rid = CCORD_ID_NONE;
    if (!json_id_key(rj, "id", &rid)) continue;
    if (rid == gid) {
      if (!found_everyone) {
        uint64_t p = 0;
        if (json_u64_key(rj, "permissions", &p)) {
          everyone_perms = p;
          found_everyone = true;
        }
      }
    } else if (ctx->has_self_roles && perm_has_self_role(ctx, rid)) {
      uint64_t p = 0;
      if (json_u64_key(rj, "permissions", &p)) role_perms |= p;
    }
  }
  ctx->base_perms = everyone_perms | role_perms;
  if ((ctx->base_perms & DISCORD_PERM_ADMINISTRATOR) != 0) {
    ctx->is_admin = true;
  }
}

static bool channel_can_view(const guild_perm_ctx *ctx, const ccord_json *cj)
{
  if (!ctx || ctx->is_admin || !ctx->has_self_roles) return true;
  uint64_t perms = ctx->base_perms;
  const ccord_json *ows = cj ? ccord_json_get(cj, "permission_overwrites") : nullptr;
  size_t n = ows ? ccord_json_array_len(ows) : 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *ow = ccord_json_array_at(ows, i);
    ccord_id id = CCORD_ID_NONE;
    if (!json_id_key(ow, "id", &id) || id != ctx->guild_id) continue;
    uint64_t allow = 0, deny = 0;
    (void)json_u64_key(ow, "allow", &allow);
    (void)json_u64_key(ow, "deny", &deny);
    perms = (perms & ~deny) | allow;
  }
  if (ctx->self_role_count > 0) {
    uint64_t r_allow = 0, r_deny = 0;
    for (size_t i = 0; i < n; i++) {
      const ccord_json *ow = ccord_json_array_at(ows, i);
      ccord_id id = CCORD_ID_NONE;
      if (!json_id_key(ow, "id", &id) || id == ctx->guild_id) continue;
      if (!perm_has_self_role(ctx, id)) continue;
      uint64_t allow = 0, deny = 0;
      (void)json_u64_key(ow, "allow", &allow);
      (void)json_u64_key(ow, "deny", &deny);
      r_allow |= allow;
      r_deny |= deny;
    }
    perms = (perms & ~r_deny) | r_allow;
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_json *ow = ccord_json_array_at(ows, i);
    ccord_id id = CCORD_ID_NONE;
    if (!json_id_key(ow, "id", &id) || id != ctx->self_id) continue;
    uint64_t allow = 0, deny = 0;
    (void)json_u64_key(ow, "allow", &allow);
    (void)json_u64_key(ow, "deny", &deny);
    perms = (perms & ~deny) | allow;
  }
  return (perms & DISCORD_PERM_VIEW_CHANNEL) != 0;
}

typedef struct raw_guild_channel {
  ccord_channel chan;
  int64_t position;
  size_t first_child;
  size_t last_child;
  size_t next_sibling;
  bool emitted;
  bool hidden_cat;
  bool has_visible_child;
} raw_guild_channel;

static int cmp_raw_guild_channel(const void *a, const void *b)
{
  const raw_guild_channel *x = a, *y = b;
  int xv = (x->chan.kind == CCORD_CHANNEL_VOICE || x->chan.kind == CCORD_CHANNEL_STAGE) ? 1 : 0;
  int yv = (y->chan.kind == CCORD_CHANNEL_VOICE || y->chan.kind == CCORD_CHANNEL_STAGE) ? 1 : 0;
  if (xv != yv) return xv - yv;
  if (x->position != y->position) return x->position < y->position ? -1 : 1;
  if (x->chan.id != y->chan.id) return x->chan.id < y->chan.id ? -1 : 1;
  return 0;
}

typedef struct raw_dm_channel {
  ccord_channel chan;
  ccord_id last_message_id;
} raw_dm_channel;

static int cmp_raw_dm_channel(const void *a, const void *b)
{
  const raw_dm_channel *x = a, *y = b;
  ccord_id kx = x->last_message_id ? x->last_message_id : x->chan.id;
  ccord_id ky = y->last_message_id ? y->last_message_id : y->chan.id;
  if (kx != ky) {
    return kx > ky ? -1 : 1;
  }
  if (x->chan.id != y->chan.id) {
    return x->chan.id > y->chan.id ? -1 : 1;
  }
  return 0;
}

// ---- permission data: roles, member role ids and channel overwrites ----
// These travel on the net event next to the snapshot data (see transport.h). A payload that lacks the key says
// nothing about it, so nothing is emitted for it: "unknown" must never be reported as "none".

typedef struct perm_lists {
  ccord_role *roles; size_t nroles, caproles;
  ccord_member_roles *mr; size_t nmr, capmr;
  ccord_channel_overwrites *ow; size_t now, capow;
} perm_lists;

static bool perm_grow(void **arr, size_t *cap, size_t n, size_t elem)
{
  if (n < *cap) return true;
  size_t ncap = *cap ? *cap * 2u : 32u;
  void *p = ccord_realloc_array(*arr, ncap, elem);
  if (!p) return false;
  *arr = p;
  *cap = ncap;
  return true;
}

static bool perm_bool(const ccord_json *obj, const char *key)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  return j && ccord_json_bool(j, false);
}

static bool perm_add_role(ccord_arena *a, perm_lists *L, const ccord_json *rj, ccord_id gid)
{
  ccord_id rid = CCORD_ID_NONE;
  if (!rj || !json_id_key(rj, "id", &rid)) return true;   // a malformed entry is skipped, not fatal
  if (!perm_grow((void **)&L->roles, &L->caproles, L->nroles, sizeof *L->roles)) return false;
  uint64_t perms = 0;
  (void)json_u64_key(rj, "permissions", &perms);
  int64_t pos = json_i64(rj, "position", 0);
  if (pos < INT32_MIN) pos = INT32_MIN;
  if (pos > INT32_MAX) pos = INT32_MAX;
  int64_t color = json_i64(rj, "color", 0);
  L->roles[L->nroles++] = (ccord_role){
    .id = rid, .guild_id = gid,
    .name = opt_text_dup(a, jstr(rj, "name"), CCORD_NAME_MAX_BYTES * 4u),
    .color = color > 0 ? (uint32_t)(color & 0xFFFFFF) : 0u,
    .position = (int32_t)pos, .permissions = perms,
    .hoist = perm_bool(rj, "hoist"), .mentionable = perm_bool(rj, "mentionable"), .managed = perm_bool(rj, "managed"),
  };
  return true;
}

static bool perm_add_roles(ccord_arena *a, perm_lists *L, const ccord_json *arr, ccord_id gid)
{
  size_t n = arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_MAX_ROLES_PER_GUILD + 1u) n = CCORD_MAX_ROLES_PER_GUILD + 1u;
  for (size_t i = 0; i < n; i++) {
    if (!perm_add_role(a, L, ccord_json_array_at(arr, i), gid)) return false;
  }
  return true;
}

// `role_arr` is a JSON array of role id strings.
static bool perm_add_member_roles(ccord_arena *a, perm_lists *L, ccord_id gid, ccord_id uid, const ccord_json *role_arr)
{
  if (uid == CCORD_ID_NONE || !role_arr || ccord_json_type_of(role_arr) != CCORD_JSON_ARRAY) return true;
  size_t n = ccord_json_array_len(role_arr);
  if (n > CCORD_MAX_ROLES_PER_GUILD) n = CCORD_MAX_ROLES_PER_GUILD;
  ccord_id *ids = n > 0 ? ccord_arena_alloc(a, n * sizeof *ids, _Alignof(ccord_id)) : nullptr;
  if (n > 0 && !ids) return false;
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    ccord_id rid = CCORD_ID_NONE;
    if (json_id(ccord_json_array_at(role_arr, i), &rid)) ids[k++] = rid;
  }
  if (!perm_grow((void **)&L->mr, &L->capmr, L->nmr, sizeof *L->mr)) return false;
  L->mr[L->nmr++] = (ccord_member_roles){.guild_id = gid, .user_id = uid, .role_ids = ids, .count = k};
  return true;
}

// A member object: {user:{id}|user_id, roles:[...]}. Without a roles array nothing is said about the member.
static bool perm_add_member(ccord_arena *a, perm_lists *L, ccord_id gid, const ccord_json *mj)
{
  if (!mj) return true;
  ccord_id uid = CCORD_ID_NONE;
  const ccord_json *u = ccord_json_get(mj, "user");
  if (!(u && json_id_key(u, "id", &uid)) && !json_id_key(mj, "user_id", &uid)) return true;
  return perm_add_member_roles(a, L, gid, uid, ccord_json_get(mj, "roles"));
}

static bool perm_overwrite_kind(const ccord_json *ow, ccord_overwrite_kind *out)
{
  const ccord_json *t = ow ? ccord_json_get(ow, "type") : nullptr;
  if (!t) return false;
  int64_t v = -1;
  if (ccord_json_int64(t, &v)) {
    if (v == 0 || v == 1) { *out = (ccord_overwrite_kind)v; return true; }
    return false;
  }
  size_t n = 0;
  const char *str = ccord_json_string(t, &n);
  if (!str) return false;
  if (n == 1 && (str[0] == '0' || str[0] == '1')) { *out = (ccord_overwrite_kind)(str[0] - '0'); return true; }
  if (n == 4 && memcmp(str, "role", 4) == 0) { *out = CCORD_OVERWRITE_ROLE; return true; }
  if (n == 6 && memcmp(str, "member", 6) == 0) { *out = CCORD_OVERWRITE_MEMBER; return true; }
  return false;
}

// A channel object: {id, permission_overwrites:[{id,type,allow,deny}]}. Absent key = unknown.
static bool perm_add_channel(ccord_arena *a, perm_lists *L, const ccord_json *cj)
{
  ccord_id cid = CCORD_ID_NONE;
  const ccord_json *oa = cj ? ccord_json_get(cj, "permission_overwrites") : nullptr;
  if (!cj || !json_id_key(cj, "id", &cid) || !oa || ccord_json_type_of(oa) != CCORD_JSON_ARRAY) return true;
  size_t n = ccord_json_array_len(oa);
  if (n > CCORD_MAX_OVERWRITES_PER_CHANNEL) n = CCORD_MAX_OVERWRITES_PER_CHANNEL;
  ccord_overwrite *items = n > 0 ? ccord_arena_alloc(a, n * sizeof *items, _Alignof(ccord_overwrite)) : nullptr;
  if (n > 0 && !items) return false;
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *ow = ccord_json_array_at(oa, i);
    ccord_id id = CCORD_ID_NONE;
    ccord_overwrite_kind kind = CCORD_OVERWRITE_ROLE;
    if (!ow || !json_id_key(ow, "id", &id) || !perm_overwrite_kind(ow, &kind)) continue;
    uint64_t allow = 0, deny = 0;
    (void)json_u64_key(ow, "allow", &allow);
    (void)json_u64_key(ow, "deny", &deny);
    items[k++] = (ccord_overwrite){.id = id, .kind = kind, .allow = allow, .deny = deny};
  }
  if (!perm_grow((void **)&L->ow, &L->capow, L->now, sizeof *L->ow)) return false;
  L->ow[L->now++] = (ccord_channel_overwrites){.channel_id = cid, .items = items, .count = k};
  return true;
}

// Moves the heap lists into the event arena and publishes them; frees the lists. Returns false on out-of-memory
// (the event is then simply missing permission data, which readers treat as unknown).
static bool perm_publish(ccord_live_event *ev, perm_lists *L)
{
  bool ok = true;
  if (L->nroles > 0) {
    ccord_role *dst = ccord_arena_alloc(&ev->arena, L->nroles * sizeof *dst, _Alignof(ccord_role));
    if (dst) {
      memcpy(dst, L->roles, L->nroles * sizeof *dst);
      ev->ev.roles = dst;
      ev->ev.role_count = L->nroles;
    } else {
      ok = false;
    }
  }
  if (L->nmr > 0) {
    ccord_member_roles *dst = ccord_arena_alloc(&ev->arena, L->nmr * sizeof *dst, _Alignof(ccord_member_roles));
    if (dst) {
      memcpy(dst, L->mr, L->nmr * sizeof *dst);
      ev->ev.member_roles = dst;
      ev->ev.member_role_count = L->nmr;
    } else {
      ok = false;
    }
  }
  if (L->now > 0) {
    ccord_channel_overwrites *dst = ccord_arena_alloc(&ev->arena, L->now * sizeof *dst, _Alignof(ccord_channel_overwrites));
    if (dst) {
      memcpy(dst, L->ow, L->now * sizeof *dst);
      ev->ev.overwrites = dst;
      ev->ev.overwrite_count = L->now;
    } else {
      ok = false;
    }
  }
  ccord_free(L->roles);
  ccord_free(L->mr);
  ccord_free(L->ow);
  *L = (perm_lists){0};
  return ok;
}

// READY.guilds[] (with the aligned merged_members[] entries) or one GUILD_CREATE / GUILD_UPDATE object.
static void perm_collect_guilds(ccord_live_event *ev, const ccord_json *const *guilds, const ccord_json *const *merged,
                                size_t ng, ccord_id self_id)
{
  perm_lists L = {0};
  bool ok = true;
  for (size_t i = 0; ok && i < ng; i++) {
    const ccord_json *gj = guilds[i];
    const ccord_json *props = guild_props(gj);
    ccord_id gid = CCORD_ID_NONE;
    if (!guild_id_key_p(gj, props, "id", &gid) || gid == CCORD_DM_GUILD_ID) continue;
    ok = perm_add_roles(&ev->arena, &L, guild_arr_p(gj, props, "roles"), gid);
    const ccord_json *members = guild_arr_p(gj, props, "members");
    size_t nm = members ? ccord_json_array_len(members) : 0;
    if (nm > CCORD_MAX_MEMBERS_PER_GUILD) nm = CCORD_MAX_MEMBERS_PER_GUILD;
    size_t first_member = L.nmr;
    for (size_t k = 0; ok && k < nm; k++) ok = perm_add_member(&ev->arena, &L, gid, ccord_json_array_at(members, k));
    bool self_listed = false;
    for (size_t k = first_member; k < L.nmr; k++) self_listed |= L.mr[k].user_id == self_id;
    if (ok && self_id != CCORD_ID_NONE && !self_listed) {
      // Discord's user gateway tells us our own roles in merged_members even when the member list is lazy.
      const ccord_json *self_roles = extract_member_roles(members, self_id, false);
      if (!self_roles) self_roles = extract_member_roles(ccord_json_get(gj, "merged_members"), self_id, true);
      if (!self_roles && merged && merged[i]) self_roles = extract_member_roles(merged[i], self_id, true);
      if (self_roles) ok = perm_add_member_roles(&ev->arena, &L, gid, self_id, self_roles);
    }
    const ccord_json *chans = guild_arr_p(gj, props, "channels");
    size_t nc = chans ? ccord_json_array_len(chans) : 0;
    if (nc > CCORD_MAX_CHANNELS_PER_GUILD) nc = CCORD_MAX_CHANNELS_PER_GUILD;
    for (size_t k = 0; ok && k < nc; k++) ok = perm_add_channel(&ev->arena, &L, ccord_json_array_at(chans, k));
  }
  (void)perm_publish(ev, &L);
}

// ---- notification settings ----
// READY's "user_guild_settings" (an array, or {entries:[...]}) and the gateway's USER_GUILD_SETTINGS_UPDATE carry one
// entry per server: {guild_id (null = direct messages), message_notifications (0 all, 1 mentions, 2 nothing, 3 = none
// set), muted, mute_config:{end_time (null = until turned back on)}, suppress_everyone, suppress_roles,
// channel_overrides:[{channel_id, message_notifications, muted, mute_config}]}. An entry holds a server whole.

static ccord_notify_level notify_level_from(const ccord_json *obj)
{
  int64_t v = json_i64(obj, "message_notifications", 3);
  return (v >= 0 && v <= 2) ? (ccord_notify_level)v : CCORD_NOTIFY_INHERIT;
}

static void notify_mute_from(const ccord_json *obj, ccord_notify_scope *row)
{
  row->muted = perm_bool(obj, "muted");
  row->mute_until_ms = 0;
  const ccord_json *mc = obj ? ccord_json_get(obj, "mute_config") : nullptr;
  if (!row->muted || !mc || ccord_json_type_of(mc) != CCORD_JSON_OBJECT) return;
  const char *end = jstr(mc, "end_time");
  if (end && end[0] != '\0') {
    int64_t t = iso_ms(end, strlen(end));
    if (t > 0) row->mute_until_ms = t;
  }
}

// The server a settings entry is about. A null (or missing-from-a-present-key) guild_id is the direct messages.
static bool notify_entry_guild(const ccord_json *e, ccord_id *gid)
{
  const ccord_json *g = e ? ccord_json_get(e, "guild_id") : nullptr;
  if (!g) return false;
  if (ccord_json_type_of(g) == CCORD_JSON_NULL) {
    *gid = CCORD_DM_GUILD_ID;
    return true;
  }
  return json_id(g, gid);
}

static size_t notify_entry_rows(const ccord_json *e)
{
  return 1u + jarray_len(e, "channel_overrides");
}

// Writes the server row then each channel override; returns how many rows went into `out` (at most `cap`).
static size_t notify_entry_fill(const ccord_json *e, ccord_id gid, ccord_notify_scope *out, size_t cap)
{
  size_t n = 0;
  if (cap == 0) return 0;
  ccord_notify_scope srv = {.guild_id = gid, .channel_id = CCORD_ID_NONE, .level = notify_level_from(e)};
  notify_mute_from(e, &srv);
  srv.suppress_everyone = perm_bool(e, "suppress_everyone");
  srv.suppress_roles = perm_bool(e, "suppress_roles");
  out[n++] = srv;
  const ccord_json *ovs = e ? ccord_json_get(e, "channel_overrides") : nullptr;
  size_t no = ovs ? ccord_json_array_len(ovs) : 0;
  for (size_t i = 0; i < no && n < cap; i++) {
    const ccord_json *oj = ccord_json_array_at(ovs, i);
    ccord_id cid = CCORD_ID_NONE;
    if (!oj || !json_id_key(oj, "channel_id", &cid)) continue;
    ccord_notify_scope row = {.guild_id = gid, .channel_id = cid, .level = notify_level_from(oj)};
    notify_mute_from(oj, &row);
    out[n++] = row;
  }
  return n;
}

static const ccord_json *notify_entries(const ccord_json *ugs)
{
  if (!ugs) return nullptr;
  if (ccord_json_type_of(ugs) == CCORD_JSON_ARRAY) return ugs;
  const ccord_json *arr = ccord_json_get(ugs, "entries");
  return (arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY) ? arr : nullptr;
}

static void notes_collect_ready(ccord_live_event *ev, const ccord_json *d)
{
  const ccord_json *nobj = d ? ccord_json_get(d, "notes") : nullptr;
  if (!nobj || ccord_json_type_of(nobj) != CCORD_JSON_OBJECT) return;
  size_t n = ccord_json_object_len(nobj);
  if (n == 0) return;
  size_t cap = n < CCORD_MAX_USER_NOTES ? n : CCORD_MAX_USER_NOTES;
  ccord_user_note *notes = ccord_arena_alloc(&ev->arena, cap * sizeof *notes, _Alignof(ccord_user_note));
  if (!notes) return;
  size_t kept = 0;
  for (size_t i = 0; i < n && kept < cap; i++) {
    const ccord_json *val = nullptr;
    const char *key = ccord_json_object_key(nobj, i, &val);
    ccord_id uid = CCORD_ID_NONE;
    if (!key || !ccord_id_parse(key, strlen(key), &uid) || uid == CCORD_ID_NONE) continue;
    size_t slen = 0;
    const char *s = val ? ccord_json_string(val, &slen) : nullptr;
    if (!s || slen == 0) continue;
    bool dup = false;
    for (size_t k = 0; k < kept; k++) {
      if (notes[k].user_id == uid) { dup = true; break; }
    }
    if (dup) continue;
    size_t cut = utf8_trunc_len(s, slen, CCORD_USER_NOTE_MAX_BYTES);
    if (cut == 0 || !ccord_utf8_valid_text(s, cut)) continue;
    char *ndup = ccord_arena_strndup(&ev->arena, s, cut);
    if (!ndup) continue;
    sanitize_str_inplace(ndup);
    if (ndup[0] == '\0') continue;
    ccord_user_note *dst = &notes[kept++];
    memset(dst, 0, sizeof *dst);
    dst->user_id = uid;
    dst->note = ndup;
  }
  if (kept > 0) {
    ev->snapshot.notes = notes;
    ev->snapshot.note_count = kept;
  }
}
// READY: every entry's rows, grouped by server. Nothing is published when the payload has none.
static void notify_collect_ready(ccord_live_event *ev, const ccord_json *d)
{
  const ccord_json *arr = notify_entries(ccord_json_get(d, "user_guild_settings"));
  size_t ne = arr ? ccord_json_array_len(arr) : 0;
  size_t cap = 0;
  for (size_t i = 0; i < ne; i++) {
    ccord_id gid = CCORD_ID_NONE;
    const ccord_json *e = ccord_json_array_at(arr, i);
    if (!notify_entry_guild(e, &gid)) continue;
    cap += notify_entry_rows(e);
    if (cap >= CCORD_NOTIFY_MAX_ROWS) {
      cap = CCORD_NOTIFY_MAX_ROWS;
      break;
    }
  }
  if (cap == 0) return;
  ccord_notify_scope *rows = ccord_arena_alloc(&ev->arena, cap * sizeof *rows, _Alignof(ccord_notify_scope));
  if (!rows) return;
  size_t n = 0;
  for (size_t i = 0; i < ne && n < cap; i++) {
    ccord_id gid = CCORD_ID_NONE;
    const ccord_json *e = ccord_json_array_at(arr, i);
    if (!notify_entry_guild(e, &gid)) continue;
    n += notify_entry_fill(e, gid, rows + n, cap - n);
  }
  ev->ev.notify = rows;
  ev->ev.notify_count = n;
}

// USER_GUILD_SETTINGS_UPDATE: the entry itself.
static bool map_notify_settings(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  if (!notify_entry_guild(d, &gid)) return false;
  size_t cap = notify_entry_rows(d);
  if (cap > CCORD_NOTIFY_MAX_ROWS) cap = CCORD_NOTIFY_MAX_ROWS;
  ccord_notify_scope *rows = ccord_arena_alloc(&ev->arena, cap * sizeof *rows, _Alignof(ccord_notify_scope));
  if (!rows) return false;
  ev->ev.type = CCORD_NET_NOTIFY_SETTINGS;
  ev->ev.guild_id = gid;
  ev->ev.notify = rows;
  ev->ev.notify_count = notify_entry_fill(d, gid, rows, cap);
  return true;
}

typedef struct role_table role_table;
static bool sb_ensure_user_stub(snapshot_builder *b, ccord_id uid);
static void resolve_member_role(ccord_arena *a, ccord_id gid, const role_table *rt, const ccord_json *mj,
                                const char **out_name, uint32_t *out_color, uint8_t *out_rank);

static void read_clan_obj(const ccord_json *clan, char out_tag[16], ccord_id *out_guild_id, char out_badge[64])
{
  if (!clan || ccord_json_type_of(clan) != CCORD_JSON_OBJECT) return;
  const ccord_json *ie = ccord_json_get(clan, "identity_enabled");
  if (ie && ccord_json_type_of(ie) == CCORD_JSON_BOOL && !ccord_json_bool(ie, true)) return;
  if (out_tag && out_tag[0] == '\0') {
    const char *tag = jstr(clan, "tag");
    if (tag && tag[0] != '\0') {
      size_t len = utf8_trunc_len(tag, strlen(tag), 15);
      if (len > 0 && ccord_utf8_valid_text(tag, len)) {
        memcpy(out_tag, tag, len);
        out_tag[len] = '\0';
      }
    }
  }
  if (out_guild_id && *out_guild_id == CCORD_ID_NONE) {
    ccord_id gid = CCORD_ID_NONE;
    if (json_id_key(clan, "identity_guild_id", &gid)) *out_guild_id = gid;
  }
  if (out_badge && out_badge[0] == '\0') {
    const char *badge = jstr(clan, "badge");
    if (!badge || badge[0] == '\0') badge = jstr(clan, "badge_hash");
    if (badge && badge[0] != '\0') {
      size_t blen = utf8_trunc_len(badge, strlen(badge), 63);
      if (blen > 0 && ccord_utf8_valid_text(badge, blen)) {
        memcpy(out_badge, badge, blen);
        out_badge[blen] = '\0';
      }
    }
  }
}

static void extract_clan_info(const ccord_json *uj, char out_tag[16], ccord_id *out_guild_id,
                              char out_badge[64])
{
  if (out_tag) out_tag[0] = '\0';
  if (out_guild_id) *out_guild_id = CCORD_ID_NONE;
  if (out_badge) out_badge[0] = '\0';
  if (!uj) return;
  read_clan_obj(ccord_json_get(uj, "primary_guild"), out_tag, out_guild_id, out_badge);
  read_clan_obj(ccord_json_get(uj, "clan"), out_tag, out_guild_id, out_badge);
}

static void copy_trunc_utf8(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  dst[0] = '\0';
  if (!src || src[0] == '\0') return;
  size_t n = utf8_trunc_len(src, strlen(src), cap - 1u);
  if (n > 0 && ccord_utf8_valid_text(src, n)) {
    memcpy(dst, src, n);
    dst[n] = '\0';
  }
}

static int64_t json_ts_ms(const ccord_json *obj, const char *key)
{
  const ccord_json *v = obj ? ccord_json_get(obj, key) : nullptr;
  if (!v) return 0;
  int64_t iv = 0;
  if (ccord_json_int64(v, &iv) && iv > 0) return iv;
  size_t slen = 0;
  const char *s = ccord_json_string(v, &slen);
  if (!s || slen == 0) return 0;
  uint64_t uid = 0;
  if (ccord_id_parse(s, slen, &uid) && uid > 0 && uid <= (uint64_t)INT64_MAX) return (int64_t)uid;
  int64_t t = iso_ms(s, slen);
  return t > 0 ? t : 0;
}

typedef struct parsed_presence_detail {
  char custom_status_emoji[65];
  char custom_status_text[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  int64_t custom_status_expires_ms;
  bool has_activity;
  ccord_activity_type activity_type;
  char activity_name[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  char activity_details[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  char activity_state[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
  int64_t activity_start_ms;
  int64_t activity_end_ms;
} parsed_presence_detail;

static void extract_presence_detail(const ccord_json *obj, parsed_presence_detail *out)
{
  memset(out, 0, sizeof *out);
  if (!obj) return;
  const ccord_json *acts = ccord_json_get(obj, "activities");
  size_t na = acts ? ccord_json_array_len(acts) : 0;
  bool saw_custom = false;
  for (size_t i = 0; i < na; i++) {
    const ccord_json *act = ccord_json_array_at(acts, i);
    if (!act || ccord_json_type_of(act) != CCORD_JSON_OBJECT) continue;
    int64_t type = json_i64(act, "type", -1);
    if (type == 4 && !saw_custom) {
      saw_custom = true;
      copy_trunc_utf8(out->custom_status_text, sizeof out->custom_status_text, jstr(act, "state"));
      const ccord_json *ej = ccord_json_get(act, "emoji");
      if (ej && ccord_json_type_of(ej) == CCORD_JSON_OBJECT) {
        const char *ename = jstr(ej, "name");
        ccord_id eid = CCORD_ID_NONE;
        (void)json_id_key(ej, "id", &eid);
        if (eid != CCORD_ID_NONE && ename && ename[0] != '\0') {
          char nbuf[44];
          copy_trunc_utf8(nbuf, sizeof nbuf, ename);
          if (nbuf[0] != '\0') {
            char ebuf[96];
            snprintf(ebuf, sizeof ebuf, "%s:%llu", nbuf, (unsigned long long)eid);
            copy_trunc_utf8(out->custom_status_emoji, sizeof out->custom_status_emoji, ebuf);
          }
        } else {
          copy_trunc_utf8(out->custom_status_emoji, sizeof out->custom_status_emoji, ename);
        }
      }
      const ccord_json *tsj = ccord_json_get(act, "timestamps");
      if (tsj && ccord_json_type_of(tsj) == CCORD_JSON_OBJECT) {
        out->custom_status_expires_ms = json_ts_ms(tsj, "end");
      }
      continue;
    }
    if (!out->has_activity && (type == 0 || type == 1 || type == 2 || type == 3 || type == 5)) {
      const char *name = jstr(act, "name");
      char nbuf[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
      copy_trunc_utf8(nbuf, sizeof nbuf, name);
      if (nbuf[0] == '\0') continue;
      out->has_activity = true;
      out->activity_type = (ccord_activity_type)type;
      memcpy(out->activity_name, nbuf, sizeof out->activity_name);
      copy_trunc_utf8(out->activity_details, sizeof out->activity_details, jstr(act, "details"));
      copy_trunc_utf8(out->activity_state, sizeof out->activity_state, jstr(act, "state"));
      const ccord_json *tsj = ccord_json_get(act, "timestamps");
      if (tsj && ccord_json_type_of(tsj) == CCORD_JSON_OBJECT) {
        out->activity_start_ms = json_ts_ms(tsj, "start");
        out->activity_end_ms = json_ts_ms(tsj, "end");
      }
    }
  }
}

static void apply_presence_detail_to_user(ccord_user *u, const parsed_presence_detail *pd)
{
  if (!u || !pd) return;
  u->has_presence = true;
  memcpy(u->custom_status_emoji, pd->custom_status_emoji, sizeof u->custom_status_emoji);
  memcpy(u->custom_status_text, pd->custom_status_text, sizeof u->custom_status_text);
  u->custom_status_expires_ms = pd->custom_status_expires_ms;
  u->has_activity = pd->has_activity;
  u->activity_type = pd->activity_type;
  memcpy(u->activity_name, pd->activity_name, sizeof u->activity_name);
  memcpy(u->activity_details, pd->activity_details, sizeof u->activity_details);
  memcpy(u->activity_state, pd->activity_state, sizeof u->activity_state);
  u->activity_start_ms = pd->activity_start_ms;
  u->activity_end_ms = pd->activity_end_ms;
}

static const char *extract_activity_status(ccord_arena *a, const ccord_json *obj)
{
  if (!obj) return "";
  const ccord_json *acts = ccord_json_get(obj, "activities");
  size_t na = acts ? ccord_json_array_len(acts) : 0;
  for (size_t i = 0; i < na; i++) {
    const ccord_json *act = ccord_json_array_at(acts, i);
    if (json_i64(act, "type", -1) != 4) continue;
    const char *state = jstr(act, "state");
    if (text_ok(state, LIVE_STATUS_MAX_BYTES)) return arena_dup(a, state);
  }
  for (size_t i = 0; i < na; i++) {
    const ccord_json *act = ccord_json_array_at(acts, i);
    int64_t type = json_i64(act, "type", -1);
    const char *name = jstr(act, "name");
    if (!text_ok(name, LIVE_STATUS_MAX_BYTES)) continue;
    const char *prefix = nullptr;
    switch (type) {
      case 0: prefix = "Playing "; break;
      case 1: prefix = "Streaming "; break;
      case 2: prefix = "Listening to "; break;
      case 3: prefix = "Watching "; break;
      case 5: prefix = "Competing in "; break;
      default: break;
    }
    if (!prefix) continue;
    if (!a) return "";
    size_t plen = strlen(prefix);
    size_t nlen = strlen(name);
    char buf[LIVE_STATUS_MAX_BYTES + 32];
    if (plen + nlen >= sizeof buf) nlen = (sizeof buf) - plen - 1u;
    memcpy(buf, prefix, plen);
    memcpy(buf + plen, name, nlen);
    buf[plen + nlen] = '\0';
    size_t clen = utf8_trunc_len(buf, plen + nlen, LIVE_STATUS_MAX_BYTES);
    const char *dup = ccord_arena_strndup(a, buf, clen);
    if (dup && ccord_utf8_valid_text(dup, clen)) return dup;
  }
  return "";
}

static bool map_member_with_pres_ex(snapshot_builder *b, const role_table *rt, const id_index *pres_map,
                                    const ccord_json *mj, ccord_id gid, ccord_id fallback_uid)
{
  if (!mj || ccord_json_type_of(mj) != CCORD_JSON_OBJECT) return true;
  const ccord_json *mu = ccord_json_get(mj, "user");
  ccord_id uid = CCORD_ID_NONE;
  if (!json_id_key(mu, "id", &uid) && !json_id_key(mj, "user_id", &uid)) {
    uid = fallback_uid;
  }
  if (uid == CCORD_ID_NONE) return true;

  ccord_presence pres = CCORD_PRESENCE_OFFLINE;
  const char *stext = "";
  parsed_presence_detail pd = {0};
  bool have_pd = false;
  const ccord_json *mpres = ccord_json_get(mj, "presence");
  if (mpres && ccord_json_type_of(mpres) == CCORD_JSON_OBJECT) {
    pres = presence_from(jstr(mpres, "status"));
    stext = extract_activity_status(b->arena, mpres);
    extract_presence_detail(mpres, &pd);
    have_pd = true;
  } else {
    size_t pval = 0;
    if (pres_map && ccord_idmap_find(pres_map, uid, &pval) && pval <= CCORD_PRESENCE_DND) {
      pres = (ccord_presence)pval;
    }
  }

  const char *nick = jstr(mj, "nick");
  bool nick_ok = nick && text_ok(nick, CCORD_NAME_MAX_BYTES);
  if (mu && ccord_json_type_of(mu) == CCORD_JSON_OBJECT) {
    const char *name = nick_ok ? nick : jstr(mu, "global_name");
    if (!name || name[0] == '\0') name = jstr(mu, "username");
    if (!text_ok(name, CCORD_NAME_MAX_BYTES)) name = "User";
    else name = arena_dup(b->arena, name);
    const ccord_json *botj = ccord_json_get(mu, "bot");
    bool bot = botj && ccord_json_bool(botj, false);
    const char *av_hash = jstr(mu, "avatar");
    if (!av_hash || av_hash[0] == '\0') av_hash = jstr(mj, "avatar");
    ccord_user u = {
      .id = uid,
      .name = name,
      .handle = opt_text_dup(b->arena, jstr(mu, "username"), CCORD_NAME_MAX_BYTES),
      .status_text = stext,
      .avatar_url = build_cdn_url(b->arena, "avatars", uid, av_hash),
      .banner_url = build_cdn_url(b->arena, "banners", uid, jstr(mu, "banner")),
      .presence = pres,
      .is_bot = bot,
      .avatar_palette = (uint8_t)(uid % 6u),
    };
    extract_clan_info(mu, u.clan_tag, &u.clan_guild_id, u.clan_badge);
    if (have_pd) apply_presence_detail_to_user(&u, &pd);
    size_t existing_idx = 0;
    bool had_user = ccord_idmap_find(&b->user_idx, uid, &existing_idx);
    if (!sb_user(b, &u)) return false;
    if (had_user && existing_idx < b->user_count) {
      // Only fill b->users[existing_idx].name from a server nickname if the user entry was a stub;
      // never overwrite a user's global display name / DM identity with a server-only nickname.
      if (nick_ok && uid != b->self_id &&
          (!b->users[existing_idx].name || b->users[existing_idx].name[0] == '\0' ||
           strncmp(b->users[existing_idx].name, "User ", 5) == 0)) {
        b->users[existing_idx].name = arena_dup(b->arena, nick);
      }
      if (u.clan_tag[0] != '\0') {
        memcpy(b->users[existing_idx].clan_tag, u.clan_tag, sizeof u.clan_tag);
      }
      if (u.clan_guild_id != CCORD_ID_NONE) {
        b->users[existing_idx].clan_guild_id = u.clan_guild_id;
      }
      if (u.clan_badge[0] != '\0') {
        memcpy(b->users[existing_idx].clan_badge, u.clan_badge, sizeof u.clan_badge);
      }
      if (have_pd) {
        b->users[existing_idx].presence = pres;
        b->users[existing_idx].status_text = stext;
        apply_presence_detail_to_user(&b->users[existing_idx], &pd);
      }
    }
  } else {
    if (!sb_ensure_user_stub(b, uid)) return false;
    size_t existing_idx = 0;
    if (ccord_idmap_find(&b->user_idx, uid, &existing_idx) && existing_idx < b->user_count) {
      if (nick_ok && uid != b->self_id &&
          (!b->users[existing_idx].name || b->users[existing_idx].name[0] == '\0' ||
           strncmp(b->users[existing_idx].name, "User ", 5) == 0)) {
        b->users[existing_idx].name = arena_dup(b->arena, nick);
      }
      if (pres != CCORD_PRESENCE_OFFLINE) {
        b->users[existing_idx].presence = pres;
      }
      if (stext[0] != '\0') {
        b->users[existing_idx].status_text = stext;
      }
      if (have_pd) {
        apply_presence_detail_to_user(&b->users[existing_idx], &pd);
      }
    }
  }

  const char *role_name = "Members";
  uint32_t color = 0;
  uint8_t rank = 255;
  resolve_member_role(b->arena, gid, rt, mj, &role_name, &color, &rank);
  // Present (an ISO timestamp) only while the member is timed out; absent or null otherwise.
  int64_t timeout_until = 0;
  const char *cdu = jstr(mj, "communication_disabled_until");
  if (cdu && cdu[0] != '\0') {
    int64_t t = iso_ms(cdu, strlen(cdu));
    if (t > 0) timeout_until = t;
  }
  int64_t joined_at = 0;
  const char *jat = jstr(mj, "joined_at");
  if (jat && jat[0] != '\0') {
    int64_t t = iso_ms(jat, strlen(jat));
    if (t > 0) joined_at = t;
  }
  const ccord_json *mute_j = ccord_json_get(mj, "mute");
  const ccord_json *deaf_j = ccord_json_get(mj, "deaf");
  bool has_mute = mute_j && ccord_json_type_of(mute_j) == CCORD_JSON_BOOL;
  bool has_deaf = deaf_j && ccord_json_type_of(deaf_j) == CCORD_JSON_BOOL;
  // Membership screening: true until the member accepted the rules form. A payload without it says nothing.
  const ccord_json *pending_j = ccord_json_get(mj, "pending");
  bool has_pending = pending_j && ccord_json_type_of(pending_j) == CCORD_JSON_BOOL;
  ccord_member m = {
    .guild_id = gid,
    .user_id = uid,
    .role_name = role_name,
    .role_color = color,
    .role_rank = rank,
    .timeout_until_ms = timeout_until,
    .joined_at_ms = joined_at,
    .mute = has_mute && ccord_json_bool(mute_j, false),
    .deaf = has_deaf && ccord_json_bool(deaf_j, false),
    .omit_mute = !has_mute,
    .omit_deaf = !has_deaf,
    .pending = has_pending && ccord_json_bool(pending_j, false),
    .omit_pending = !has_pending,
  };
  if (nick_ok) {
    size_t nlen = strlen(nick);
    memcpy(m.nick, nick, nlen + 1);
  }
  return sb_member(b, &m);
}

static bool map_member_with_pres(snapshot_builder *b, const role_table *rt, const id_index *pres_map,
                                 const ccord_json *mj, ccord_id gid)
{
  return map_member_with_pres_ex(b, rt, pres_map, mj, gid, CCORD_ID_NONE);
}

typedef struct cached_role_entry {
  ccord_id id;
  uint32_t color;
  uint32_t name_off;
  uint16_t name_len;
  uint8_t rank;
} cached_role_entry;

struct role_table {
  cached_role_entry *roles;
  size_t count;
  char *names;
  ccord_idmap idx;   // populated when count > 8: role_id -> index in roles[]
};

typedef struct cached_guild_roles {
  ccord_id guild_id;
  role_table table;
} cached_guild_roles;

#define LIVE_ROLE_CACHE_MAX_GUILDS 256u
#define LIVE_ROLE_TABLE_MAX_ROLES  512u
static cached_guild_roles *g_role_cache = nullptr;
static size_t g_role_cache_count = 0;
static size_t g_role_cache_cap = 0;
static ccord_idmap g_role_cache_idx = {0};

static void role_table_free(role_table *t)
{
  if (!t) return;
  ccord_free(t->roles);
  ccord_free(t->names);
  ccord_idmap_free(&t->idx);
  *t = (role_table){0};
}

static void role_table_build(role_table *t, const ccord_json *roles)
{
  role_table_free(t);
  if (!roles) return;
  size_t nr = ccord_json_array_len(roles);
  if (nr == 0) return;
  if (nr > LIVE_ROLE_TABLE_MAX_ROLES) nr = LIVE_ROLE_TABLE_MAX_ROLES;

  size_t valid = 0;
  size_t name_bytes = 0;
  for (size_t i = 0; i < nr; i++) {
    const ccord_json *rj = ccord_json_array_at(roles, i);
    ccord_id rid = CCORD_ID_NONE;
    if (!json_id_key(rj, "id", &rid) || rid == CCORD_ID_NONE) continue;
    valid++;
    const char *rn = jstr(rj, "name");
    if (text_ok(rn, CCORD_NAME_MAX_BYTES)) {
      name_bytes += strlen(rn) + 1u;
    }
  }
  if (valid == 0) return;

  cached_role_entry *entries = ccord_alloc_array(valid, sizeof *entries);
  if (!entries) return;
  char *names = name_bytes > 0 ? ccord_malloc(name_bytes) : nullptr;
  if (name_bytes > 0 && !names) {
    ccord_free(entries);
    return;
  }

  bool use_idx = valid > 8u;
  if (use_idx && !ccord_idmap_reserve(&t->idx, valid)) {
    ccord_idmap_free(&t->idx);
    use_idx = false;
  }

  size_t count = 0;
  size_t off = 0;
  for (size_t i = 0; i < nr && count < valid; i++) {
    const ccord_json *rj = ccord_json_array_at(roles, i);
    ccord_id rid = CCORD_ID_NONE;
    if (!json_id_key(rj, "id", &rid) || rid == CCORD_ID_NONE) continue;
    cached_role_entry *e = &entries[count];
    e->id = rid;
    e->name_off = 0;
    e->name_len = 0;
    const char *rn = jstr(rj, "name");
    if (names && text_ok(rn, CCORD_NAME_MAX_BYTES)) {
      size_t rlen = strlen(rn);
      if (off + rlen + 1u <= name_bytes) {
        memcpy(names + off, rn, rlen);
        names[off + rlen] = '\0';
        e->name_off = (uint32_t)off;
        e->name_len = (uint16_t)rlen;
        off += rlen + 1u;
      }
    }
    int64_t c = json_i64(rj, "color", 0);
    if (c < 0) c = 0;
    if (c > (int64_t)0xFFFFFF) c = (int64_t)0xFFFFFF;
    e->color = (uint32_t)c;
    int64_t pos = json_i64(rj, "position", 0);
    if (pos < 0) pos = 0;
    if (pos > 255) pos = 255;
    e->rank = (uint8_t)pos;
    if (use_idx && !ccord_idmap_find(&t->idx, rid, nullptr)) {
      if (!ccord_idmap_put(&t->idx, rid, count)) {
        ccord_idmap_free(&t->idx);
        use_idx = false;
      }
    }
    count++;
  }
  t->roles = entries;
  t->count = count;
  t->names = names;
}

static const cached_role_entry *role_table_find(const role_table *t, ccord_id rid)
{
  if (!t || t->count == 0 || rid == CCORD_ID_NONE) return nullptr;
  if (t->idx.cap > 0) {
    size_t idx = 0;
    if (ccord_idmap_find(&t->idx, rid, &idx) && idx < t->count) return &t->roles[idx];
    return nullptr;
  }
  for (size_t i = 0; i < t->count; i++) {
    if (t->roles[i].id == rid) return &t->roles[i];
  }
  return nullptr;
}

static const role_table *find_cached_guild_roles(ccord_id gid)
{
  if (gid == CCORD_ID_NONE || !g_role_cache) return nullptr;
  size_t slot_idx = 0;
  if (ccord_idmap_find(&g_role_cache_idx, gid, &slot_idx) && slot_idx < g_role_cache_count) {
    return g_role_cache[slot_idx].table.count > 0 ? &g_role_cache[slot_idx].table : nullptr;
  }
  return nullptr;
}

static const role_table *cache_guild_roles(ccord_id gid, const ccord_json *roles)
{
  if (gid == CCORD_ID_NONE || !roles) return nullptr;
  size_t nr = ccord_json_array_len(roles);
  if (nr == 0) return nullptr;
  size_t slot_idx = 0;
  if (!ccord_idmap_find(&g_role_cache_idx, gid, &slot_idx) || slot_idx >= g_role_cache_count) {
    if (g_role_cache_count < LIVE_ROLE_CACHE_MAX_GUILDS) {
      if (g_role_cache_count == g_role_cache_cap) {
        size_t ncap = g_role_cache_cap ? g_role_cache_cap * 2u : 8u;
        if (ncap > LIVE_ROLE_CACHE_MAX_GUILDS) ncap = LIVE_ROLE_CACHE_MAX_GUILDS;
        cached_guild_roles *np = ccord_realloc_array(g_role_cache, ncap, sizeof *np);
        if (!np) return nullptr;
        g_role_cache = np;
        g_role_cache_cap = ncap;
      }
      slot_idx = g_role_cache_count++;
      g_role_cache[slot_idx] = (cached_guild_roles){.guild_id = gid};
      if (!ccord_idmap_put(&g_role_cache_idx, gid, slot_idx)) {
        g_role_cache_count--;
        return nullptr;
      }
    } else {
      slot_idx = (size_t)(gid % LIVE_ROLE_CACHE_MAX_GUILDS);
      if (g_role_cache[slot_idx].guild_id != CCORD_ID_NONE) {
        (void)ccord_idmap_remove(&g_role_cache_idx, g_role_cache[slot_idx].guild_id);
      }
      role_table_free(&g_role_cache[slot_idx].table);
      g_role_cache[slot_idx].guild_id = gid;
      if (!ccord_idmap_put(&g_role_cache_idx, gid, slot_idx)) {
        g_role_cache[slot_idx].guild_id = CCORD_ID_NONE;
        return nullptr;
      }
    }
  }
  role_table_build(&g_role_cache[slot_idx].table, roles);
  return g_role_cache[slot_idx].table.count > 0 ? &g_role_cache[slot_idx].table : nullptr;
}

static void resolve_member_role(ccord_arena *a, ccord_id gid, const role_table *rt, const ccord_json *mj,
                                const char **out_name, uint32_t *out_color, uint8_t *out_rank)
{
  const char *role_name = "Members";
  uint32_t color = 0;
  uint8_t rank = 255;
  const ccord_json *mroles = mj ? ccord_json_get(mj, "roles") : nullptr;
  size_t nr = mroles ? ccord_json_array_len(mroles) : 0;
  if (nr > 0) {
    if ((!rt || rt->count == 0) && gid != CCORD_ID_NONE) {
      rt = find_cached_guild_roles(gid);
    }
    for (size_t i = 0; i < nr; i++) {
      ccord_id rid = CCORD_ID_NONE;
      if (!json_id(ccord_json_array_at(mroles, i), &rid)) continue;
      const cached_role_entry *e = role_table_find(rt, rid);
      if (e) {
        if (e->name_len > 0 && rt->names && a) {
          const char *dup = ccord_arena_strndup(a, rt->names + e->name_off, e->name_len);
          if (dup) role_name = dup;
        }
        color = e->color;
        rank = e->rank;
      }
      break; // the first role is the member's highest displayed role
    }
  }
  *out_name = role_name;
  *out_color = color;
  *out_rank = rank;
}

static bool map_merged_member_entry(snapshot_builder *b, const role_table *rt, const id_index *pres_map,
                                    const ccord_json *entry, ccord_id gid)
{
  if (!entry) return true;
  if (ccord_json_type_of(entry) == CCORD_JSON_OBJECT) {
    return map_member_with_pres_ex(b, rt, pres_map, entry, gid, b->self_id);
  }
  if (ccord_json_type_of(entry) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(entry);
    for (size_t i = 0; i < n && i < CCORD_MAX_MEMBERS_PER_GUILD; i++) {
      ccord_id fb = (i == 0) ? b->self_id : CCORD_ID_NONE;
      if (!map_member_with_pres_ex(b, rt, pres_map, ccord_json_array_at(entry, i), gid, fb)) {
        return false;
      }
    }
  }
  return true;
}

// Maps one voice_states entry. Entries without a channel are disconnected and skipped.
static bool map_voice_state(const ccord_json *v, ccord_id guild_hint, ccord_voice_state *out)
{
  if (!v) return false;
  ccord_id guild = guild_hint, channel = CCORD_ID_NONE, user = CCORD_ID_NONE;
  (void)json_id_key(v, "guild_id", &guild);
  (void)json_id_key(v, "channel_id", &channel);
  if (!json_id_key(v, "user_id", &user)) return false;
  out->guild_id = guild;
  out->channel_id = channel;
  out->user_id = user;
  const ccord_json *m = ccord_json_get(v, "self_mute");
  const ccord_json *d = ccord_json_get(v, "self_deaf");
  const ccord_json *vid = ccord_json_get(v, "self_video");
  const ccord_json *str = ccord_json_get(v, "self_stream");
  const ccord_json *sm = ccord_json_get(v, "mute");
  const ccord_json *sd = ccord_json_get(v, "deaf");
  out->self_mute = m && ccord_json_bool(m, false);
  out->self_deaf = d && ccord_json_bool(d, false);
  out->self_video = vid && ccord_json_bool(vid, false);
  out->self_stream = str && ccord_json_bool(str, false);
  out->mute = sm && ccord_json_bool(sm, false);
  out->deaf = sd && ccord_json_bool(sd, false);
  // Stage fields: a partial update may omit them, and an absent key must not clobber what the store already has.
  const ccord_json *sup = ccord_json_get(v, "suppress");
  bool has_suppress = sup && ccord_json_type_of(sup) == CCORD_JSON_BOOL;
  out->suppress = has_suppress && ccord_json_bool(sup, false);
  out->omit_suppress = !has_suppress;
  const ccord_json *rts = ccord_json_get(v, "request_to_speak_timestamp");
  out->request_to_speak_timestamp = 0;
  out->omit_request_to_speak = !rts;
  if (rts && ccord_json_type_of(rts) == CCORD_JSON_STRING) {
    size_t rn = 0;
    const char *rs = ccord_json_string(rts, &rn);
    if (rs && rn > 0) {
      int64_t ms = iso_ms(rs, rn);
      if (ms > 0) out->request_to_speak_timestamp = ms;
    }
  }
  return true;
}

static bool map_user_obj(ccord_arena *a, const ccord_json *uj, ccord_presence pres, ccord_user *out);

static bool sb_ensure_user_stub(snapshot_builder *b, ccord_id uid)
{
  if (!b || uid == CCORD_ID_NONE) return false;
  if (ccord_idmap_find(&b->user_idx, uid, nullptr)) return true;
  char idbuf[21];
  ccord_id_format(uid, idbuf);
  char namebuf[sizeof("User ") + sizeof idbuf];
  memcpy(namebuf, "User ", 5);
  size_t idlen = strlen(idbuf);
  memcpy(namebuf + 5, idbuf, idlen + 1);
  const char *aname = ccord_arena_strndup(b->arena, namebuf, 5 + idlen);
  if (!aname) return false;
  ccord_user stub = {
    .id = uid,
    .name = aname,
    .status_text = "",
    .avatar_url = "",
    .bio = "",
    .pronouns = "",
    .presence = CCORD_PRESENCE_OFFLINE,
    .is_bot = false,
    .avatar_palette = (uint8_t)(uid % 6u),
  };
  return sb_user(b, &stub);
}

static bool sb_has_voice_channel(const snapshot_builder *b, ccord_id gid, ccord_id cid)
{
  if (!b || cid == CCORD_ID_NONE) return false;
  size_t idx = 0;
  if (!ccord_idmap_find(&b->chan_idx, cid, &idx) || idx >= b->channel_count) return false;
  return b->channels[idx].guild_id == gid &&
         (b->channels[idx].kind == CCORD_CHANNEL_VOICE || b->channels[idx].kind == CCORD_CHANNEL_STAGE);
}

static bool collect_voice_state_user(snapshot_builder *b, const ccord_json *vj, ccord_id user_id, bool stub_if_missing)
{
  const ccord_json *vmem = vj ? ccord_json_get(vj, "member") : nullptr;
  const ccord_json *vu = vmem ? ccord_json_get(vmem, "user") : (vj ? ccord_json_get(vj, "user") : nullptr);
  ccord_user parsed_u;
  if (vu && map_user_obj(b->arena, vu, CCORD_PRESENCE_OFFLINE, &parsed_u)) {
    parsed_u.id = user_id;
    return sb_user(b, &parsed_u);
  }
  return stub_if_missing ? sb_ensure_user_stub(b, user_id) : true;
}

static bool collect_voice_state_entry(snapshot_builder *b, const ccord_json *vj, ccord_id guild_hint,
                                      bool allow_disconnect, bool require_channel_known, bool stub_if_missing)
{
  ccord_voice_state v;
  if (!map_voice_state(vj, guild_hint, &v)) return true;
  if (v.guild_id == CCORD_ID_NONE || v.guild_id == CCORD_DM_GUILD_ID) return true;
  if (!allow_disconnect && v.channel_id == CCORD_ID_NONE) return true;
  if (require_channel_known && v.channel_id != CCORD_ID_NONE &&
      !sb_has_voice_channel(b, v.guild_id, v.channel_id)) {
    return true;
  }
  if (!collect_voice_state_user(b, vj, v.user_id, stub_if_missing)) return false;
  return sb_voice(b, &v);
}

static bool collect_member_users_from_array(snapshot_builder *b, const ccord_json *arr, int depth)
{
  if (!arr || !is_json_array(arr) || depth > 2) return true;
  size_t n = ccord_json_array_len(arr);
  for (size_t i = 0; i < n && i < CCORD_MAX_MEMBERS_PER_GUILD; i++) {
    const ccord_json *item = ccord_json_array_at(arr, i);
    if (!item) continue;
    if (is_json_array(item)) {
      if (!collect_member_users_from_array(b, item, depth + 1)) return false;
      continue;
    }
    const ccord_json *mu = ccord_json_get(item, "user");
    ccord_user u;
    if (mu && map_user_obj(b->arena, mu, CCORD_PRESENCE_OFFLINE, &u)) {
      if (!sb_user(b, &u)) return false;
    }
  }
  return true;
}

static void map_forum_meta(const ccord_json *cj, ccord_id forum_id, uint8_t discord_type, ccord_forum_meta *out)
{
  memset(out, 0, sizeof *out);
  out->forum_id = forum_id;
  out->is_media = (discord_type == CCORD_DISCORD_CHANNEL_GUILD_MEDIA);
  int64_t flags = json_i64(cj, "flags", 0);
  out->require_tag = (flags & CCORD_FORUM_FLAG_REQUIRE_TAG) != 0;
  int64_t sort_order = json_i64(cj, "default_sort_order", 0);
  out->default_sort = (sort_order == 1) ? CCORD_FORUM_SORT_CREATION : CCORD_FORUM_SORT_ACTIVITY;
  const ccord_json *tags_arr = cj ? ccord_json_get(cj, "available_tags") : nullptr;
  size_t ntags = tags_arr ? ccord_json_array_len(tags_arr) : 0;
  for (size_t i = 0; i < ntags && out->tag_count < CCORD_FORUM_MAX_TAGS; i++) {
    const ccord_json *tj = ccord_json_array_at(tags_arr, i);
    ccord_id tid = CCORD_ID_NONE;
    if (!tj || !json_id_key(tj, "id", &tid) || tid == CCORD_ID_NONE) continue;
    const char *tname = jstr(tj, "name");
    if (!tname || tname[0] == '\0') continue;
    size_t nlen = utf8_trunc_len(tname, strlen(tname), CCORD_FORUM_TAG_NAME_BYTES - 1u);
    if (nlen == 0 || !ccord_utf8_valid_text(tname, nlen)) continue;
    ccord_forum_tag *dst = &out->tags[out->tag_count++];
    memset(dst, 0, sizeof *dst);
    dst->id = tid;
    memcpy(dst->name, tname, nlen);
    dst->name[nlen] = '\0';
    dst->moderated = json_bool_key(tj, "moderated", false);
    (void)json_id_key(tj, "emoji_id", &dst->emoji_id);
    const char *ename = jstr(tj, "emoji_name");
    if (ename && ename[0] != '\0') {
      size_t elen = utf8_trunc_len(ename, strlen(ename), CCORD_FORUM_TAG_EMOJI_BYTES - 1u);
      if (elen > 0 && ccord_utf8_valid_text(ename, elen)) {
        memcpy(dst->emoji_name, ename, elen);
        dst->emoji_name[elen] = '\0';
      }
    }
  }
}

static const char *extract_message_thumbnail(ccord_arena *a, const ccord_json *msg)
{
  if (!a || !msg) return "";
  const ccord_json *atts = ccord_json_get(msg, "attachments");
  size_t natts = atts ? ccord_json_array_len(atts) : 0;
  for (size_t i = 0; i < natts; i++) {
    const ccord_json *att = ccord_json_array_at(atts, i);
    const char *url = att ? jstr(att, "url") : nullptr;
    if (!url || url[0] == '\0') url = att ? jstr(att, "proxy_url") : nullptr;
    if (!url || url[0] == '\0' || strlen(url) > CCORD_URL_MAX_BYTES) continue;
    const char *ct = jstr(att, "content_type");
    int64_t w = json_i64(att, "width", 0);
    if ((ct && strncmp(ct, "image/", 6) == 0) || w > 0) {
      char *dup = ccord_arena_strndup(a, url, strlen(url));
      return dup ? dup : "";
    }
  }
  const ccord_json *embs = ccord_json_get(msg, "embeds");
  size_t nemb = embs ? ccord_json_array_len(embs) : 0;
  for (size_t i = 0; i < nemb; i++) {
    const ccord_json *emb = ccord_json_array_at(embs, i);
    const ccord_json *img = emb ? ccord_json_get(emb, "image") : nullptr;
    const ccord_json *th = emb ? ccord_json_get(emb, "thumbnail") : nullptr;
    const char *url = img ? jstr(img, "url") : nullptr;
    if (!url || url[0] == '\0') url = th ? jstr(th, "url") : nullptr;
    if (url && url[0] != '\0' && strlen(url) <= CCORD_URL_MAX_BYTES) {
      char *dup = ccord_arena_strndup(a, url, strlen(url));
      return dup ? dup : "";
    }
  }
  return "";
}

static bool map_forum_post(ccord_arena *a, const ccord_json *tj, const ccord_json *first_msg,
                           ccord_id forum_fallback, ccord_id guild_fallback, ccord_forum_post *out)
{
  if (!a || !tj || !out) return false;
  *out = (ccord_forum_post){0};
  ccord_id tid = CCORD_ID_NONE;
  if (!json_id_key(tj, "id", &tid) || tid == CCORD_ID_NONE) return false;
  int64_t type = json_i64(tj, "type", CCORD_DISCORD_CHANNEL_PUBLIC_THREAD);
  if (!ccord_discord_type_is_thread((uint8_t)type)) return false;
  ccord_id fid = forum_fallback;
  (void)json_id_key(tj, "parent_id", &fid);
  if (fid == CCORD_ID_NONE) return false;
  ccord_id gid = guild_fallback;
  (void)json_id_key(tj, "guild_id", &gid);
  ccord_id owner_id = CCORD_ID_NONE;
  (void)json_id_key(tj, "owner_id", &owner_id);
  const char *title = jstr(tj, "name");
  if (!title || title[0] == '\0') title = "Untitled Post";
  size_t tlen = utf8_trunc_len(title, strlen(title), CCORD_FORUM_TITLE_BYTES);
  const char *title_dup = ccord_arena_strndup(a, title, tlen);
  if (!title_dup || !ccord_utf8_valid_text(title_dup, strlen(title_dup))) title_dup = "Untitled Post";

  out->thread_id = tid;
  out->forum_id = fid;
  out->guild_id = gid;
  out->owner_id = owner_id;
  out->title = title_dup;
  int64_t flags = json_i64(tj, "flags", 0);
  out->pinned = (flags & CCORD_FORUM_FLAG_PINNED) != 0;
  int64_t mc = json_i64(tj, "message_count", 0);
  if (mc < 0) mc = 0;
  if (mc > (int64_t)UINT32_MAX) mc = (int64_t)UINT32_MAX;
  out->message_count = (uint32_t)mc;
  (void)json_id_key(tj, "last_message_id", &out->last_message_id);

  const ccord_json *atags = ccord_json_get(tj, "applied_tags");
  size_t nat = atags ? ccord_json_array_len(atags) : 0;
  for (size_t i = 0; i < nat && out->applied_tag_count < CCORD_FORUM_MAX_APPLIED_TAGS; i++) {
    ccord_id tag_id = CCORD_ID_NONE;
    if (json_id(ccord_json_array_at(atags, i), &tag_id) && tag_id != CCORD_ID_NONE) {
      out->applied_tags[out->applied_tag_count++] = tag_id;
    }
  }

  const ccord_json *tmeta = ccord_json_get(tj, "thread_metadata");
  if (tmeta && ccord_json_type_of(tmeta) == CCORD_JSON_OBJECT) {
    out->archived = json_bool_key(tmeta, "archived", false);
    out->locked = json_bool_key(tmeta, "locked", false);
    const char *cts = jstr(tmeta, "create_timestamp");
    if (cts && cts[0] != '\0') out->create_ts_ms = iso_ms(cts, strlen(cts));
    const char *ats = jstr(tmeta, "archive_timestamp");
    if (ats && ats[0] != '\0') out->last_activity_ms = iso_ms(ats, strlen(ats));
  }
  if (out->create_ts_ms <= 0) out->create_ts_ms = ccord_snowflake_unix_ms(tid);
  int64_t lmid_ms = ccord_snowflake_unix_ms(out->last_message_id);
  if (lmid_ms > out->last_activity_ms) out->last_activity_ms = lmid_ms;
  if (out->last_activity_ms <= 0) out->last_activity_ms = out->create_ts_ms;

  out->first_content = "";
  out->thumbnail_url = "";
  out->reaction_emoji = "";

  if (!first_msg) {
    const ccord_json *nested = ccord_json_get(tj, "message");
    if (nested && ccord_json_type_of(nested) == CCORD_JSON_OBJECT) first_msg = nested;
  }
  if (first_msg && ccord_json_type_of(first_msg) == CCORD_JSON_OBJECT) {
    (void)json_id_key(first_msg, "id", &out->first_message_id);
    if (out->first_message_id == CCORD_ID_NONE) out->first_message_id = tid;
    const ccord_json *au = ccord_json_get(first_msg, "author");
    if (!json_id_key(au, "id", &out->first_author_id)) out->first_author_id = owner_id;
    if (out->owner_id == CCORD_ID_NONE) out->owner_id = out->first_author_id;
    const char *raw_c = jstr(first_msg, "content");
    if (raw_c && raw_c[0] != '\0') {
      size_t clen = utf8_trunc_len(raw_c, strlen(raw_c), CCORD_FORUM_EXCERPT_BYTES - 1u);
      char *cdup = ccord_arena_strndup(a, raw_c, clen);
      if (cdup && ccord_utf8_valid_text(cdup, clen)) out->first_content = cdup;
    }
    out->thumbnail_url = extract_message_thumbnail(a, first_msg);
    const ccord_json *rx_arr = ccord_json_get(first_msg, "reactions");
    size_t nrx = rx_arr ? ccord_json_array_len(rx_arr) : 0;
    int64_t total_rx = 0, best_rx = 0;
    const char *best_emoji = "";
    for (size_t i = 0; i < nrx; i++) {
      const ccord_json *rj = ccord_json_array_at(rx_arr, i);
      int64_t c = json_i64(rj, "count", 0);
      if (c <= 0) continue;
      total_rx += c;
      if (c > best_rx) {
        const ccord_json *emj = rj ? ccord_json_get(rj, "emoji") : nullptr;
        const char *en = emj ? jstr(emj, "name") : nullptr;
        if (en && en[0] != '\0') {
          best_rx = c;
          best_emoji = en;
        }
      }
    }
    if (total_rx > (int64_t)UINT32_MAX) total_rx = (int64_t)UINT32_MAX;
    out->reaction_count = (uint32_t)total_rx;
    if (best_emoji[0] != '\0') {
      size_t elen = utf8_trunc_len(best_emoji, strlen(best_emoji), CCORD_EMOJI_MAX_BYTES);
      char *edup = ccord_arena_strndup(a, best_emoji, elen);
      if (edup && ccord_utf8_valid_text(edup, elen)) out->reaction_emoji = edup;
    }
  } else if (out->owner_id != CCORD_ID_NONE) {
    out->first_author_id = out->owner_id;
  }
  return true;
}

static void parse_incidents_obj(const ccord_json *inc, int64_t *out_inv_ms, int64_t *out_dms_ms)
{
  *out_inv_ms = 0;
  *out_dms_ms = 0;
  if (!inc || ccord_json_type_of(inc) != CCORD_JSON_OBJECT) return;
  const char *inv = jstr(inc, "invites_disabled_until");
  if (inv && inv[0] != '\0') {
    int64_t ms = iso_ms(inv, strlen(inv));
    if (ms > 0) *out_inv_ms = ms;
  }
  const char *dms = jstr(inc, "dms_disabled_until");
  if (dms && dms[0] != '\0') {
    int64_t ms = iso_ms(dms, strlen(dms));
    if (ms > 0) *out_dms_ms = ms;
  }
}

// Maps one full guild object (READY entry, GUILD_CREATE or GUILD_UPDATE).
static bool map_guild_ex(snapshot_builder *b, const ccord_json *gj, const ccord_json *ready_merged_entry)
{
  const ccord_json *props = guild_props(gj);
  ccord_id gid = CCORD_ID_NONE;
  if (!guild_id_key_p(gj, props, "id", &gid)) return true;
  const char *gname = guild_str_p(gj, props, "name");
  bool gname_owned = false;
  if (gname && strlen(gname) > CCORD_NAME_MAX_BYTES) {
    size_t cut = utf8_trunc_len(gname, strlen(gname), CCORD_NAME_MAX_BYTES);
    gname = cut > 0 ? ccord_arena_strndup(b->arena, gname, cut) : nullptr;
    gname_owned = true;
  }
  if (!text_ok(gname, CCORD_NAME_MAX_BYTES)) {
    gname = "Guild";
    gname_owned = true;
  } else if (!gname_owned) {
    gname = arena_dup(b->arena, gname);
    if (gname[0] == '\0') gname = "Guild";
  }
  guild_perm_ctx pctx;
  init_guild_perm_ctx(&pctx, gj, props, ready_merged_entry, gid, b->self_id);
  uint64_t self_perms = 0;
  if (!json_u64_key(gj, "permissions", &self_perms)) {
    (void)json_u64_key(props, "permissions", &self_perms);
  }
  if (b->self_id != CCORD_ID_NONE) {
    self_perms = pctx.is_admin ? UINT64_MAX : pctx.base_perms;
  }
  ccord_guild g = {
    .id = gid,
    .owner_id = pctx.owner_id,
    .self_permissions = self_perms,
    .name = gname,
    .icon_url = build_cdn_url(b->arena, "icons", gid, guild_str_p(gj, props, "icon")),
    .has_guild_settings = true,
  };
  const char *burl = build_cdn_url(b->arena, "banners", gid, guild_str_p(gj, props, "banner"));
  copy_trunc_utf8(g.banner_url, sizeof g.banner_url, burl);
  (void)guild_id_key_p(gj, props, "system_channel_id", &g.system_channel_id);
  int64_t sflags = guild_i64_p(gj, props, "system_channel_flags", 0);
  if (sflags >= 0) g.system_channel_flags = (uint32_t)sflags & CCORD_SYSCHAN_KNOWN_MASK;
  int64_t dmn = guild_i64_p(gj, props, "default_message_notifications", 0);
  g.default_message_notifications = (dmn == 1) ? 1u : 0u;
  (void)guild_id_key_p(gj, props, "afk_channel_id", &g.afk_channel_id);
  int64_t afkt = guild_i64_p(gj, props, "afk_timeout", 300);
  if (afkt > 0 && afkt <= 3600) g.afk_timeout = (uint32_t)afkt;
  int64_t vlev = guild_i64_p(gj, props, "verification_level", 0);
  if (vlev >= 0 && vlev <= 4) g.verification_level = (uint8_t)vlev;
  int64_t ecf = guild_i64_p(gj, props, "explicit_content_filter", 0);
  if (ecf >= 0 && ecf <= 2) g.explicit_content_filter = (uint8_t)ecf;
  int64_t ptier = guild_i64_p(gj, props, "premium_tier", 0);
  if (ptier >= 0 && ptier <= 3) g.premium_tier = (uint8_t)ptier;
  int64_t psubs = guild_i64_p(gj, props, "premium_subscription_count", 0);
  if (psubs > 0 && psubs <= UINT32_MAX) g.premium_subscription_count = (uint32_t)psubs;
  copy_trunc_utf8(g.vanity_url_code, sizeof g.vanity_url_code, guild_str_p(gj, props, "vanity_url_code"));
  (void)guild_id_key_p(gj, props, "rules_channel_id", &g.rules_channel_id);
  (void)guild_id_key_p(gj, props, "public_updates_channel_id", &g.public_updates_channel_id);
  const ccord_json *feats = guild_arr_p(gj, props, "features");
  size_t nfeat = feats ? ccord_json_array_len(feats) : 0;
  for (size_t fi = 0; fi < nfeat; fi++) {
    size_t flen = 0;
    const char *fs = ccord_json_string(ccord_json_array_at(feats, fi), &flen);
    if (!fs) continue;
    if (strcmp(fs, "COMMUNITY") == 0) g.feature_community = true;
    else if (strcmp(fs, "VANITY_URL") == 0) g.feature_vanity_url = true;
    else if (strcmp(fs, "BANNER") == 0) g.feature_banner = true;
  }
  const ccord_json *inc_raw = gj ? ccord_json_get(gj, "incidents_data") : nullptr;
  if (!inc_raw && props) inc_raw = ccord_json_get(props, "incidents_data");
  if (inc_raw) {
    g.has_incidents_data = true;
    const ccord_json *inc = guild_obj_p(gj, props, "incidents_data");
    parse_incidents_obj(inc, &g.invites_disabled_until_ms, &g.dms_disabled_until_ms);
  }
  if (!sb_guild(b, &g)) return false;

  const ccord_json *ch_arr = guild_arr_p(gj, props, "channels");
  const ccord_json *th_arr = guild_arr_p(gj, props, "threads");
  size_t nc = ch_arr ? ccord_json_array_len(ch_arr) : 0;
  size_t nt = th_arr ? ccord_json_array_len(th_arr) : 0;
  size_t max_raw = nc + nt;
  if (max_raw > CCORD_MAX_CHANNELS_PER_GUILD) max_raw = CCORD_MAX_CHANNELS_PER_GUILD;
  raw_guild_channel *raw = max_raw > 0 ? ccord_calloc(max_raw, sizeof *raw) : nullptr;
  if (max_raw > 0 && !raw) return false;
  id_index raw_idx = {0};
  if (max_raw > 0 && !ccord_idmap_reserve(&raw_idx, max_raw)) {
    ccord_free(raw);
    return false;
  }
  size_t nraw = 0;
  for (size_t k = 0; k < nc && nraw < max_raw; k++) {
    const ccord_json *cj = ccord_json_array_at(ch_arr, k);
    bool can_view = channel_can_view(&pctx, cj);
    if (!can_view && json_i64(cj, "type", 0) != 4) continue;
    ccord_channel c;
    if (!map_channel(b->arena, cj, gid, &c)) continue;
    if (ccord_idmap_find(&raw_idx, c.id, nullptr)) continue;
    if (ccord_discord_type_is_forum(c.discord_type)) {
      ccord_forum_meta fm;
      map_forum_meta(cj, c.id, c.discord_type, &fm);
      if (!sb_forum_meta(b, &fm)) {
        ccord_idmap_free(&raw_idx);
        ccord_free(raw);
        return false;
      }
    }
    if (!ccord_idmap_put(&raw_idx, c.id, nraw)) {
      ccord_idmap_free(&raw_idx);
      ccord_free(raw);
      return false;
    }
    raw[nraw++] = (raw_guild_channel){
      .chan = c,
      .position = c.position,
      .first_child = SIZE_MAX,
      .last_child = SIZE_MAX,
      .next_sibling = SIZE_MAX,
      .emitted = false,
      .hidden_cat = !can_view && c.kind == CCORD_CHANNEL_CATEGORY,
      .has_visible_child = false,
    };
  }
  for (size_t k = 0; k < nt && nraw < max_raw; k++) {
    const ccord_json *tj = ccord_json_array_at(th_arr, k);
    if (!channel_can_view(&pctx, tj)) continue;
    ccord_channel c;
    if (!map_channel(b->arena, tj, gid, &c)) continue;
    bool parent_is_forum = false;
    if (c.parent_id != CCORD_ID_NONE) {
      size_t pidx = 0;
      if (ccord_idmap_find(&raw_idx, c.parent_id, &pidx) && pidx < nraw &&
          ccord_discord_type_is_forum(raw[pidx].chan.discord_type)) {
        parent_is_forum = true;
      }
    }
    if (parent_is_forum) {
      ccord_forum_post fp;
      if (map_forum_post(b->arena, tj, nullptr, c.parent_id, gid, &fp)) {
        if (!sb_forum_post(b, &fp)) {
          ccord_idmap_free(&raw_idx);
          ccord_free(raw);
          return false;
        }
      }
      continue;
    }
    if (ccord_idmap_find(&raw_idx, c.id, nullptr)) continue;
    if (!ccord_idmap_put(&raw_idx, c.id, nraw)) {
      ccord_idmap_free(&raw_idx);
      ccord_free(raw);
      return false;
    }
    raw[nraw++] = (raw_guild_channel){
      .chan = c,
      .position = c.position,
      .first_child = SIZE_MAX,
      .last_child = SIZE_MAX,
      .next_sibling = SIZE_MAX,
      .emitted = false,
      .hidden_cat = false,
      .has_visible_child = false,
    };
  }
  // Drop categories that failed channel_can_view and have no visible child channels inside them.
  for (size_t k = 0; k < nraw; k++) {
    if (raw[k].hidden_cat || raw[k].chan.kind == CCORD_CHANNEL_CATEGORY ||
        raw[k].chan.parent_id == CCORD_ID_NONE) {
      continue;
    }
    size_t pidx = 0;
    if (ccord_idmap_find(&raw_idx, raw[k].chan.parent_id, &pidx) && pidx < nraw && pidx != k) {
      raw[pidx].has_visible_child = true;
    }
  }
  {
    size_t w = 0;
    for (size_t i = 0; i < nraw; i++) {
      if (raw[i].hidden_cat && !raw[i].has_visible_child) continue;
      if (w != i) raw[w] = raw[i];
      w++;
    }
    nraw = w;
  }
  if (nraw > 1) {
    qsort(raw, nraw, sizeof *raw, cmp_raw_guild_channel);
  }
  // Rebuild id -> sorted index map once for O(1) parent normalization and child-list linking.
  ccord_idmap_clear(&raw_idx);
  for (size_t i = 0; i < nraw; i++) {
    raw[i].first_child = SIZE_MAX;
    raw[i].last_child = SIZE_MAX;
    raw[i].next_sibling = SIZE_MAX;
    (void)ccord_idmap_put(&raw_idx, raw[i].chan.id, i);
  }
  // Normalize parent_id against visible categories and text channels in this guild and link child lists in sorted order.
  for (size_t i = 0; i < nraw; i++) {
    ccord_channel *c = &raw[i].chan;
    if (c->parent_id == CCORD_ID_NONE) continue;
    size_t pidx = 0;
    bool parent_ok = false;
    if (ccord_idmap_find(&raw_idx, c->parent_id, &pidx) && pidx < nraw && pidx != i) {
      const ccord_channel *p = &raw[pidx].chan;
      if (p->kind == CCORD_CHANNEL_CATEGORY ||
          (c->kind == CCORD_CHANNEL_TEXT && p->kind == CCORD_CHANNEL_TEXT)) {
        parent_ok = true;
      }
    }
    if (!parent_ok) {
      c->parent_id = CCORD_ID_NONE;
    } else if (c->kind != CCORD_CHANNEL_CATEGORY) {
      if (raw[pidx].first_child == SIZE_MAX) {
        raw[pidx].first_child = i;
      } else {
        raw[raw[pidx].last_child].next_sibling = i;
      }
      raw[pidx].last_child = i;
    }
  }
  bool ok = true;
  // 1. Emit uncategorized channels in (is_voice, position, id) order, followed by any child threads.
  for (size_t i = 0; ok && i < nraw; i++) {
    if (raw[i].emitted || raw[i].chan.kind == CCORD_CHANNEL_CATEGORY ||
        raw[i].chan.parent_id != CCORD_ID_NONE) {
      continue;
    }
    raw[i].emitted = true;
    ok = sb_channel(b, &raw[i].chan);
    if (raw[i].chan.kind == CCORD_CHANNEL_TEXT) {
      for (size_t t = raw[i].first_child; ok && t != SIZE_MAX; t = raw[t].next_sibling) {
        if (!raw[t].emitted && raw[t].chan.kind == CCORD_CHANNEL_TEXT) {
          raw[t].emitted = true;
          ok = sb_channel(b, &raw[t].chan);
        }
      }
    }
  }
  // 2. Emit categories in (position, id) order, each followed by its child channels in (is_voice, position, id) order.
  for (size_t i = 0; ok && i < nraw; i++) {
    if (raw[i].emitted || raw[i].chan.kind != CCORD_CHANNEL_CATEGORY) continue;
    raw[i].emitted = true;
    ok = sb_channel(b, &raw[i].chan);
    for (size_t k = raw[i].first_child; ok && k != SIZE_MAX; k = raw[k].next_sibling) {
      if (raw[k].emitted || raw[k].chan.kind == CCORD_CHANNEL_CATEGORY) continue;
      raw[k].emitted = true;
      ok = sb_channel(b, &raw[k].chan);
      if (raw[k].chan.kind == CCORD_CHANNEL_TEXT) {
        for (size_t t = raw[k].first_child; ok && t != SIZE_MAX; t = raw[t].next_sibling) {
          if (!raw[t].emitted && raw[t].chan.kind == CCORD_CHANNEL_TEXT) {
            raw[t].emitted = true;
            ok = sb_channel(b, &raw[t].chan);
          }
        }
      }
    }
  }
  // 3. Emit any remaining channels (e.g., nested threads).
  for (size_t i = 0; ok && i < nraw; i++) {
    if (!raw[i].emitted) {
      raw[i].emitted = true;
      ok = sb_channel(b, &raw[i].chan);
    }
  }
  ccord_idmap_free(&raw_idx);
  ccord_free(raw);
  if (!ok) return false;

  const ccord_json *roles = guild_arr_p(gj, props, "roles");
  const role_table *rt = cache_guild_roles(gid, roles);
  const ccord_json *mem_arr = guild_arr_p(gj, props, "members");
  const ccord_json *gj_merged = gj ? ccord_json_get(gj, "merged_members") : nullptr;
  size_t nm = mem_arr ? ccord_json_array_len(mem_arr) : 0;
  if (nm > 0 || gj_merged || ready_merged_entry) {
    const ccord_json *pres_arr = guild_arr_p(gj, props, "presences");
    size_t np = pres_arr ? ccord_json_array_len(pres_arr) : 0;
    id_index pres_map = {0};
    for (size_t i = 0; i < np; i++) {
      const ccord_json *pj = ccord_json_array_at(pres_arr, i);
      const ccord_json *pu = pj ? ccord_json_get(pj, "user") : nullptr;
      ccord_id pid = CCORD_ID_NONE;
      if (json_id_key(pu, "id", &pid) || json_id_key(pj, "user_id", &pid)) {
        (void)ccord_idmap_put(&pres_map, pid, (size_t)presence_from(jstr(pj, "status")));
      }
    }
    for (size_t k = 0; ok && k < nm && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
      ok = map_member_with_pres(b, rt, &pres_map, ccord_json_array_at(mem_arr, k), gid);
    }
    if (ok && gj_merged) {
      ok = map_merged_member_entry(b, rt, &pres_map, gj_merged, gid);
    }
    if (ok && ready_merged_entry) {
      ok = map_merged_member_entry(b, rt, &pres_map, ready_merged_entry, gid);
    }
    for (size_t i = 0; ok && i < np; i++) {
      const ccord_json *pj = ccord_json_array_at(pres_arr, i);
      const ccord_json *pu = pj ? ccord_json_get(pj, "user") : nullptr;
      ccord_id pid = CCORD_ID_NONE;
      if (!json_id_key(pu, "id", &pid) && !json_id_key(pj, "user_id", &pid)) continue;
      size_t uidx = 0;
      if (ccord_idmap_find(&b->user_idx, pid, &uidx) && uidx < b->user_count) {
        b->users[uidx].presence = presence_from(jstr(pj, "status"));
        const char *stext = extract_activity_status(b->arena, pj);
        b->users[uidx].status_text = stext;
        parsed_presence_detail pd;
        extract_presence_detail(pj, &pd);
        apply_presence_detail_to_user(&b->users[uidx], &pd);
      }
    }
    ccord_idmap_free(&pres_map);
    if (!ok) return false;
  }

  const ccord_json *vs_arr = guild_arr_p(gj, props, "voice_states");
  size_t nv = vs_arr ? ccord_json_array_len(vs_arr) : 0;
  for (size_t k = 0; k < nv && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
    if (!collect_voice_state_entry(b, ccord_json_array_at(vs_arr, k), gid, false, true, true)) return false;
  }
  return true;
}

static bool map_guild(snapshot_builder *b, const ccord_json *gj)
{
  return map_guild_ex(b, gj, nullptr);
}

static ccord_relationship_kind rel_kind_from(int64_t type)
{
  switch (type) {
    case 1: return CCORD_REL_FRIEND;
    case 2: return CCORD_REL_BLOCKED;
    case 3: return CCORD_REL_INCOMING_REQUEST;
    case 4: return CCORD_REL_OUTGOING_REQUEST;
    default: return CCORD_REL_NONE;
  }
}

static bool map_user_obj(ccord_arena *a, const ccord_json *uj, ccord_presence pres, ccord_user *out)
{
  if (!uj || !out) return false;
  ccord_id uid = CCORD_ID_NONE;
  if (!json_id_key(uj, "id", &uid)) return false;
  const char *name = jstr(uj, "global_name");
  if (!name || name[0] == '\0') name = jstr(uj, "username");
  if (!text_ok(name, CCORD_NAME_MAX_BYTES)) name = "User";
  else name = arena_dup(a, name);
  const ccord_json *botj = ccord_json_get(uj, "bot");
  const ccord_json *acj = ccord_json_get(uj, "accent_color");
  bool has_accent = acj && ccord_json_type_of(acj) == CCORD_JSON_NUMBER;
  int64_t accent = has_accent ? json_i64(uj, "accent_color", 0) : 0;
  if (accent < 0 || accent > (int64_t)0xFFFFFF) {
    accent = 0;
    has_accent = false;
  }
  *out = (ccord_user){
    .id = uid,
    .name = name,
    .handle = opt_text_dup(a, jstr(uj, "username"), CCORD_NAME_MAX_BYTES),
    .status_text = "",
    .avatar_url = build_cdn_url(a, "avatars", uid, jstr(uj, "avatar")),
    .banner_url = build_cdn_url(a, "banners", uid, jstr(uj, "banner")),
    .accent_color = (uint32_t)accent,
    .has_accent_color = has_accent,
    .bio = opt_text_dup(a, jstr(uj, "bio"), CCORD_USER_BIO_MAX_BYTES),
    .pronouns = opt_text_dup(a, jstr(uj, "pronouns"), CCORD_USER_PRONOUNS_MAX_BYTES),
    .presence = pres,
    .is_bot = botj && ccord_json_bool(botj, false),
    .avatar_palette = (uint8_t)(uid % 6u),
  };
  extract_clan_info(uj, out->clan_tag, &out->clan_guild_id, out->clan_badge);
  return true;
}

static void apply_ready_read_state(snapshot_builder *b, const ccord_json *d)
{
  const ccord_json *rs = ccord_json_get(d, "read_state");
  if (!rs) return;
  if (ccord_json_type_of(rs) == CCORD_JSON_OBJECT) {
    rs = ccord_json_get(rs, "entries");
  }
  size_t n = rs ? ccord_json_array_len(rs) : 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *e = ccord_json_array_at(rs, i);
    if (json_i64(e, "read_state_type", 0) != 0) continue;
    ccord_id cid = CCORD_ID_NONE;
    if (!json_id_key(e, "id", &cid)) continue;
    size_t k = 0;
    if (!ccord_idmap_find(&b->chan_idx, cid, &k) || k >= b->channel_count) continue;
    if (b->channels[k].kind != CCORD_CHANNEL_TEXT) continue;
    int64_t mc = json_i64(e, "mention_count", 0);
    int64_t uc = json_i64(e, "unread_count", -1);
    if (mc < 0) mc = 0;
    if (mc > (int64_t)UINT32_MAX) mc = (int64_t)UINT32_MAX;
    ccord_id ack_id = CCORD_ID_NONE;
    (void)json_id_key(e, "last_message_id", &ack_id);
    if (ack_id != CCORD_ID_NONE && ack_id < (UINT64_C(1) << 62)) b->channels[k].last_read_id = ack_id;   // the read position
    b->channels[k].mention_count = (uint32_t)mc;
    if (uc >= 0) {
      if (uc > (int64_t)UINT32_MAX) uc = (int64_t)UINT32_MAX;
      b->channels[k].unread_count = (uint32_t)uc;
    } else {
      ccord_id last_msg = b->channels[k].last_message_id;
      if (last_msg != CCORD_ID_NONE && (ack_id == CCORD_ID_NONE || last_msg > ack_id)) {
        uint32_t est = mc > 0 ? (uint32_t)mc : 1u;
        if (b->channels[k].unread_count < est) b->channels[k].unread_count = est;
      } else if (mc > 0 && b->channels[k].unread_count == 0) {
        b->channels[k].unread_count = (uint32_t)mc;
      }
    }
    if (b->channels[k].guild_id == CCORD_DM_GUILD_ID &&
        b->channels[k].last_message_id == CCORD_ID_NONE &&
        ack_id > b->channels[k].id && ack_id < (UINT64_C(1) << 62)) {
      b->channels[k].last_message_id = ack_id;
    }
  }
}

static int cmp_dm_channel_desc(const void *a, const void *b)
{
  const ccord_channel *x = a, *y = b;
  ccord_id kx = x->last_message_id ? x->last_message_id : x->id;
  ccord_id ky = y->last_message_id ? y->last_message_id : y->id;
  if (kx != ky) return kx > ky ? -1 : 1;
  if (x->id != y->id) return x->id > y->id ? -1 : 1;
  return 0;
}

static int b64_val(unsigned char c)
{
  if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
  if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
  if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

static uint8_t *b64_decode_alloc(const char *in, size_t in_len, size_t *out_len)
{
  if (out_len) *out_len = 0;
  if (!in || in_len == 0) return nullptr;
  size_t cap = (in_len * 3u) / 4u + 4u;
  uint8_t *out = ccord_calloc(cap, 1);
  if (!out) return nullptr;
  uint32_t acc = 0;
  int bits = 0;
  size_t o = 0;
  for (size_t i = 0; i < in_len; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '=') break;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    int v = b64_val(c);
    if (v < 0) {
      ccord_free(out);
      return nullptr;
    }
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (uint8_t)((acc >> bits) & 0xFFu);
    }
  }
  if (out_len) *out_len = o;
  return out;
}

static bool pb_read_varint(const uint8_t *buf, size_t len, size_t *pos, uint64_t *out)
{
  uint64_t val = 0;
  unsigned shift = 0;
  while (*pos < len && shift < 64u) {
    uint8_t b = buf[(*pos)++];
    val |= (uint64_t)(b & 0x7Fu) << shift;
    if ((b & 0x80u) == 0) {
      *out = val;
      return true;
    }
    shift += 7u;
  }
  return false;
}

static bool pb_read_fixed64(const uint8_t *buf, size_t len, size_t *pos, uint64_t *out)
{
  if (*pos + 8u > len) return false;
  uint64_t v = 0;
  for (unsigned i = 0; i < 8u; i++) {
    v |= (uint64_t)buf[*pos + i] << (i * 8u);
  }
  *pos += 8u;
  *out = v;
  return true;
}

static bool pb_skip_field(const uint8_t *buf, size_t len, size_t *pos, uint32_t wire_type)
{
  uint64_t v = 0;
  switch (wire_type) {
    case 0:
      return pb_read_varint(buf, len, pos, &v);
    case 1:
      if (*pos + 8u > len) return false;
      *pos += 8u;
      return true;
    case 2:
      if (!pb_read_varint(buf, len, pos, &v) || v > len - *pos) return false;
      *pos += (size_t)v;
      return true;
    case 5:
      if (*pos + 4u > len) return false;
      *pos += 4u;
      return true;
    default:
      return false;
  }
}

static void stamp_guild_folder(ccord_guild *g, ccord_id folder_id, const char *folder_name, uint32_t folder_color)
{
  if (!g) return;
  g->folder_id = folder_id;
  memset(g->folder_name, 0, sizeof g->folder_name);
  if (folder_id != CCORD_ID_NONE) {
    if (folder_name && folder_name[0] != '\0') {
      size_t n = utf8_trunc_len(folder_name, strlen(folder_name), sizeof g->folder_name - 1u);
      if (ccord_utf8_valid_text(folder_name, n)) memcpy(g->folder_name, folder_name, n);
    }
    g->folder_color = folder_color & 0xFFFFFFu;
  } else {
    g->folder_color = 0;
  }
}

static void emit_want_guild_folder(snapshot_builder *b, const ccord_json *guilds_arr,
                                   const ccord_json *merged_arr, const id_index *ready_guild_idx,
                                   size_t ng, size_t guild_cap, bool *visited, ccord_id want_gid,
                                   ccord_id folder_id, const char *folder_name, uint32_t folder_color,
                                   bool *ok)
{
  if (!*ok || want_gid == CCORD_ID_NONE || b->guild_count >= guild_cap || !ready_guild_idx) return;
  size_t i = 0;
  if (!ccord_idmap_find(ready_guild_idx, want_gid, &i) || i >= ng || visited[i]) return;
  visited[i] = true;
  size_t prev = b->guild_count;
  const ccord_json *gj = ccord_json_array_at(guilds_arr, i);
  const ccord_json *mj = merged_arr ? ccord_json_array_at(merged_arr, i) : nullptr;
  *ok = map_guild_ex(b, gj, mj);
  if (*ok && b->guild_count > prev) {
    stamp_guild_folder(&b->guilds[prev], folder_id, folder_name, folder_color);
  }
}

static void emit_want_guild(snapshot_builder *b, const ccord_json *guilds_arr,
                            const ccord_json *merged_arr, const id_index *ready_guild_idx,
                            size_t ng, size_t guild_cap, bool *visited, ccord_id want_gid, bool *ok)
{
  emit_want_guild_folder(b, guilds_arr, merged_arr, ready_guild_idx, ng, guild_cap, visited, want_gid,
                         CCORD_ID_NONE, nullptr, 0, ok);
}

static bool pb_read_wrapped_u64(const uint8_t *sub, size_t nsub, uint64_t *out)
{
  size_t p = 0;
  while (p < nsub) {
    uint64_t tag = 0;
    if (!pb_read_varint(sub, nsub, &p, &tag)) return false;
    uint32_t fnum = (uint32_t)(tag >> 3);
    uint32_t wt = (uint32_t)(tag & 7u);
    if (fnum == 1 && wt == 0) return pb_read_varint(sub, nsub, &p, out);
    if (fnum == 1 && wt == 1) return pb_read_fixed64(sub, nsub, &p, out);
    if (!pb_skip_field(sub, nsub, &p, wt)) return false;
  }
  return false;
}

static void pb_read_wrapped_str(const uint8_t *sub, size_t nsub, char out[64])
{
  out[0] = '\0';
  if (nsub == 0) return;
  if (sub[0] == 0x0Au) {
    size_t p = 1;
    uint64_t slen = 0;
    if (pb_read_varint(sub, nsub, &p, &slen) && p + (size_t)slen == nsub) {
      const char *s = (const char *)(sub + p);
      size_t n = utf8_trunc_len(s, (size_t)slen, 63u);
      if (ccord_utf8_valid_text(s, n)) {
        memcpy(out, s, n);
        out[n] = '\0';
      }
      return;
    }
  }
  const char *s = (const char *)sub;
  size_t n = utf8_trunc_len(s, nsub, 63u);
  if (ccord_utf8_valid_text(s, n)) {
    memcpy(out, s, n);
    out[n] = '\0';
  }
}

typedef struct pb_folder_parsed {
  ccord_id gids[CCORD_MAX_GUILDS];
  size_t ngids;
  ccord_id folder_id;
  char name[64];
  uint32_t color;
} pb_folder_parsed;

static void pb_decode_one_folder(const uint8_t *buf, size_t len, const id_index *ready_guild_idx,
                                 size_t folder_seq, pb_folder_parsed *out)
{
  memset(out, 0, sizeof *out);
  size_t pos = 0;
  while (pos < len) {
    uint64_t tag = 0;
    if (!pb_read_varint(buf, len, &pos, &tag)) break;
    uint32_t field_num = (uint32_t)(tag >> 3);
    uint32_t wire_type = (uint32_t)(tag & 7u);
    if (field_num == 1) {
      uint64_t v = 0;
      if (wire_type == 0) {
        if (!pb_read_varint(buf, len, &pos, &v)) break;
        if (v != 0 && out->ngids < CCORD_MAX_GUILDS) out->gids[out->ngids++] = (ccord_id)v;
      } else if (wire_type == 1) {
        if (!pb_read_fixed64(buf, len, &pos, &v)) break;
        if (v != 0 && out->ngids < CCORD_MAX_GUILDS) out->gids[out->ngids++] = (ccord_id)v;
      } else if (wire_type == 2) {
        uint64_t slen = 0;
        if (!pb_read_varint(buf, len, &pos, &slen) || slen > len - pos) break;
        const uint8_t *sub = buf + pos;
        size_t nsub = (size_t)slen;
        pos += nsub;
        bool use_fixed64 = false;
        if (nsub > 0 && (nsub % 8u) == 0) {
          if (ready_guild_idx && ready_guild_idx->count > 0) {
            size_t fpos = 0;
            while (fpos < nsub) {
              uint64_t fv = 0;
              if (!pb_read_fixed64(sub, nsub, &fpos, &fv)) break;
              if (fv != 0 && ccord_idmap_find(ready_guild_idx, (ccord_id)fv, nullptr)) {
                use_fixed64 = true;
                break;
              }
            }
          } else {
            // Discord's PreloadedUserSettings.GuildFolder.guild_ids uses packed fixed64 snowflakes (> 2^40).
            size_t fpos = 0;
            uint64_t fv = 0;
            if (pb_read_fixed64(sub, nsub, &fpos, &fv) && fv >= (UINT64_C(1) << 22) && fv < (UINT64_C(1) << 62)) {
              use_fixed64 = true;
            }
          }
        }
        if (use_fixed64) {
          size_t fpos = 0;
          while (fpos < nsub) {
            uint64_t fv = 0;
            if (!pb_read_fixed64(sub, nsub, &fpos, &fv)) break;
            if (fv != 0 && out->ngids < CCORD_MAX_GUILDS) out->gids[out->ngids++] = (ccord_id)fv;
          }
        } else {
          size_t vpos = 0;
          while (vpos < nsub) {
            uint64_t vv = 0;
            if (!pb_read_varint(sub, nsub, &vpos, &vv)) break;
            if (vv != 0 && out->ngids < CCORD_MAX_GUILDS) out->gids[out->ngids++] = (ccord_id)vv;
          }
        }
      } else {
        if (!pb_skip_field(buf, len, &pos, wire_type)) break;
      }
    } else if (field_num == 2) {
      uint64_t v = 0;
      if (wire_type == 0) {
        if (!pb_read_varint(buf, len, &pos, &v)) break;
        if (v > 0 && v < CCORD_DM_GUILD_ID) out->folder_id = (ccord_id)v;
      } else if (wire_type == 2) {
        uint64_t slen = 0;
        if (!pb_read_varint(buf, len, &pos, &slen) || slen > len - pos) break;
        if (pb_read_wrapped_u64(buf + pos, (size_t)slen, &v) && v > 0 && v < CCORD_DM_GUILD_ID) {
          out->folder_id = (ccord_id)v;
        }
        pos += (size_t)slen;
      } else {
        if (!pb_skip_field(buf, len, &pos, wire_type)) break;
      }
    } else if (field_num == 3 && wire_type == 2) {
      uint64_t slen = 0;
      if (!pb_read_varint(buf, len, &pos, &slen) || slen > len - pos) break;
      pb_read_wrapped_str(buf + pos, (size_t)slen, out->name);
      pos += (size_t)slen;
    } else if (field_num == 4) {
      uint64_t v = 0;
      if (wire_type == 0) {
        if (!pb_read_varint(buf, len, &pos, &v)) break;
        out->color = (uint32_t)(v & 0xFFFFFFu);
      } else if (wire_type == 2) {
        uint64_t slen = 0;
        if (!pb_read_varint(buf, len, &pos, &slen) || slen > len - pos) break;
        if (pb_read_wrapped_u64(buf + pos, (size_t)slen, &v)) out->color = (uint32_t)(v & 0xFFFFFFu);
        pos += (size_t)slen;
      } else {
        if (!pb_skip_field(buf, len, &pos, wire_type)) break;
      }
    } else {
      if (!pb_skip_field(buf, len, &pos, wire_type)) break;
    }
  }
  if (out->folder_id == CCORD_ID_NONE && (out->ngids > 1 || out->name[0] != '\0' || out->color != 0)) {
    out->folder_id = (ccord_id)(0xF01DE000ULL + (uint64_t)folder_seq + 1ULL);
  }
}

static void pb_parse_guild_folder(const uint8_t *buf, size_t len, snapshot_builder *b,
                                  const ccord_json *guilds_arr, const ccord_json *merged_arr,
                                  const id_index *ready_guild_idx, size_t ng, size_t guild_cap,
                                  size_t folder_seq, bool *visited, bool *ok)
{
  pb_folder_parsed fp;
  pb_decode_one_folder(buf, len, ready_guild_idx, folder_seq, &fp);
  for (size_t i = 0; *ok && i < fp.ngids && b->guild_count < guild_cap; i++) {
    emit_want_guild_folder(b, guilds_arr, merged_arr, ready_guild_idx, ng, guild_cap, visited,
                           fp.gids[i], fp.folder_id, fp.name, fp.color, ok);
  }
}

static void expr_collect_guild(ccord_live_event *ev, const ccord_json *const *guilds, size_t ng);

static void apply_proto_guild_order(const char *proto_b64, snapshot_builder *b,
                                    const ccord_json *guilds_arr, const ccord_json *merged_arr,
                                    const id_index *ready_guild_idx, size_t ng, size_t guild_cap,
                                    bool *visited, bool *ok)
{
  if (!proto_b64 || proto_b64[0] == '\0' || !*ok) return;
  size_t raw_len = 0;
  uint8_t *raw = b64_decode_alloc(proto_b64, strlen(proto_b64), &raw_len);
  if (!raw || raw_len == 0) {
    ccord_free(raw);
    return;
  }
  size_t pos = 0;
  size_t folder_seq = 0;
  while (*ok && pos < raw_len && b->guild_count < guild_cap) {
    uint64_t tag = 0;
    if (!pb_read_varint(raw, raw_len, &pos, &tag)) break;
    uint32_t field_num = (uint32_t)(tag >> 3);
    uint32_t wire_type = (uint32_t)(tag & 7u);
    if (field_num == 14 && wire_type == 2) {
      uint64_t gf_len = 0;
      if (!pb_read_varint(raw, raw_len, &pos, &gf_len) || gf_len > raw_len - pos) break;
      const uint8_t *gf = raw + pos;
      size_t ngf = (size_t)gf_len;
      pos += ngf;
      size_t gpos = 0;
      while (*ok && gpos < ngf && b->guild_count < guild_cap) {
        uint64_t ftag = 0;
        if (!pb_read_varint(gf, ngf, &gpos, &ftag)) break;
        uint32_t fnum = (uint32_t)(ftag >> 3);
        uint32_t fwire = (uint32_t)(ftag & 7u);
        if (fnum == 1 && fwire == 2) {
          uint64_t flen = 0;
          if (!pb_read_varint(gf, ngf, &gpos, &flen) || flen > ngf - gpos) break;
          pb_parse_guild_folder(gf + gpos, (size_t)flen, b, guilds_arr, merged_arr, ready_guild_idx,
                                ng, guild_cap, folder_seq++, visited, ok);
          gpos += (size_t)flen;
        } else if (fnum == 1 && (fwire == 0 || fwire == 1)) {
          uint64_t v = 0;
          bool r = (fwire == 0) ? pb_read_varint(gf, ngf, &gpos, &v)
                                : pb_read_fixed64(gf, ngf, &gpos, &v);
          if (!r) break;
          emit_want_guild(b, guilds_arr, merged_arr, ready_guild_idx, ng, guild_cap, visited, (ccord_id)v, ok);
        } else {
          if (!pb_skip_field(gf, ngf, &gpos, fwire)) break;
        }
      }
    } else {
      if (!pb_skip_field(raw, raw_len, &pos, wire_type)) break;
    }
  }
  ccord_free(raw);
}

static void parse_json_folder_meta(const ccord_json *fj, size_t ngids, size_t folder_seq,
                                   ccord_id *out_fid, char out_name[64], uint32_t *out_color)
{
  *out_fid = CCORD_ID_NONE;
  out_name[0] = '\0';
  *out_color = 0;
  if (!fj) return;
  ccord_id fid = CCORD_ID_NONE;
  if (json_id_key(fj, "id", &fid) && fid != CCORD_DM_GUILD_ID) *out_fid = fid;
  const char *fname = jstr(fj, "name");
  if (fname && fname[0] != '\0') {
    size_t n = utf8_trunc_len(fname, strlen(fname), 63u);
    if (ccord_utf8_valid_text(fname, n)) {
      memcpy(out_name, fname, n);
      out_name[n] = '\0';
    }
  }
  int64_t col = json_i64(fj, "color", 0);
  if (col > 0 && col <= 0xFFFFFF) *out_color = (uint32_t)col;
  if (*out_fid == CCORD_ID_NONE && (ngids > 1 || out_name[0] != '\0' || *out_color != 0)) {
    *out_fid = (ccord_id)(0xF01DE000ULL + (uint64_t)folder_seq + 1ULL);
  }
}

// Bounded integer field: absent/invalid keeps the fallback, out-of-range values clamp to [0, max].
static uint8_t json_u8_clamped(const ccord_json *obj, const char *key, uint8_t max, uint8_t fallback)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  int64_t v = 0;
  if (!j || !ccord_json_int64(j, &v)) return fallback;
  if (v < 0) return 0;
  if (v > (int64_t)max) return max;
  return (uint8_t)v;
}

// Defaults mirror store_user.c k_settings_defaults: GIFs / animated emoji on, stickers "always",
// DMs from server members allowed, locale en-US. Partial payloads (USER_SETTINGS_UPDATE) keep these.
static ccord_user_settings user_settings_defaults(void)
{
  return (ccord_user_settings){
    .default_guilds_restricted = false,
    .explicit_content_filter = 0,
    .friend_source_all = true,
    .friend_source_mutual_friends = true,
    .friend_source_mutual_guilds = true,
    .allow_access_to_age_restricted_servers = false,
    .developer_mode = false,
    .gif_auto_play = true,
    .animate_emoji = true,
    .enable_tts_command = false,
    .stream_notifications_enabled = false,
    .message_display_compact = false,
    .animate_stickers = 0,
    .locale = "en-US",
  };
}

// M6 account settings. Does not touch ev.type: callers keep the existing
// guild-folders / custom-status event mapping; has_user_settings tells the glue a full model is present.
static void map_user_settings_model(ccord_live_event *ev, const ccord_json *d)
{
  if (!ev || !d || ccord_json_type_of(d) != CCORD_JSON_OBJECT) return;
  ccord_user_settings s = user_settings_defaults();
  s.default_guilds_restricted = json_bool_key(d, "default_guilds_restricted", s.default_guilds_restricted);
  s.explicit_content_filter = json_u8_clamped(d, "explicit_content_filter", 2u, s.explicit_content_filter);
  s.friend_source_all = json_bool_key(d, "friend_source_all", s.friend_source_all);
  s.friend_source_mutual_friends = json_bool_key(d, "friend_source_mutual_friends", s.friend_source_mutual_friends);
  s.friend_source_mutual_guilds = json_bool_key(d, "friend_source_mutual_guilds", s.friend_source_mutual_guilds);
  const ccord_json *fsf = ccord_json_get(d, "friend_source_flags");
  if (fsf && ccord_json_type_of(fsf) == CCORD_JSON_OBJECT) {
    s.friend_source_all = json_bool_key(fsf, "all", s.friend_source_all);
    s.friend_source_mutual_friends = json_bool_key(fsf, "mutual_friends", s.friend_source_mutual_friends);
    s.friend_source_mutual_guilds = json_bool_key(fsf, "mutual_guilds", s.friend_source_mutual_guilds);
  }
  s.allow_access_to_age_restricted_servers =
    json_bool_key(d, "allow_access_to_age_restricted_servers", s.allow_access_to_age_restricted_servers);
  s.developer_mode = json_bool_key(d, "developer_mode", s.developer_mode);
  s.gif_auto_play = json_bool_key(d, "gif_auto_play", s.gif_auto_play);
  s.animate_emoji = json_bool_key(d, "animate_emoji", s.animate_emoji);
  s.enable_tts_command = json_bool_key(d, "enable_tts_command", s.enable_tts_command);
  s.stream_notifications_enabled =
    json_bool_key(d, "stream_notifications_enabled", s.stream_notifications_enabled);
  s.message_display_compact = json_bool_key(d, "message_display_compact", s.message_display_compact);
  s.animate_stickers = json_u8_clamped(d, "animate_stickers", 2u, s.animate_stickers);
  copy_trunc_utf8(s.locale, sizeof s.locale, jstr(d, "locale"));
  if (s.locale[0] == '\0') memcpy(s.locale, "en-US", sizeof "en-US");
  ev->user_settings = s;
  ev->has_user_settings = true;
}

static bool map_user_settings_custom_status(ccord_live_event *ev, const ccord_json *d)
{
  if (!d || ccord_json_type_of(d) != CCORD_JSON_OBJECT) return false;
  const ccord_json *cs = ccord_json_get(d, "custom_status");
  if (!cs) return false;
  ev->ev.type = CCORD_NET_CUSTOM_STATUS_UPDATE;
  if (ccord_json_type_of(cs) == CCORD_JSON_NULL) {
    ev->ev.custom_status_clear = true;
    return true;
  }
  if (ccord_json_type_of(cs) != CCORD_JSON_OBJECT) return false;
  copy_trunc_utf8(ev->ev.custom_status_text, sizeof ev->ev.custom_status_text, jstr(cs, "text"));
  copy_trunc_utf8(ev->ev.custom_status_emoji, sizeof ev->ev.custom_status_emoji, jstr(cs, "emoji_name"));
  ccord_id cs_eid = CCORD_ID_NONE;
  (void)json_id_key(cs, "emoji_id", &cs_eid);
  if (cs_eid != CCORD_ID_NONE && ev->ev.custom_status_emoji[0] != '\0') {   // the store keeps a custom emoji as name:id
    char nbuf[44];
    copy_trunc_utf8(nbuf, sizeof nbuf, ev->ev.custom_status_emoji);
    snprintf(ev->ev.custom_status_emoji, sizeof ev->ev.custom_status_emoji, "%s:%llu", nbuf, (unsigned long long)cs_eid);
  }
  ev->ev.custom_status_expires_ms = json_ts_ms(cs, "expires_at");
  if (ev->ev.custom_status_text[0] == '\0' && ev->ev.custom_status_emoji[0] == '\0') {
    ev->ev.custom_status_clear = true;
  }
  return true;
}

static bool map_user_settings_folders(ccord_live_event *ev, const ccord_json *d, bool require_folders)
{
  if (!d || ccord_json_type_of(d) != CCORD_JSON_OBJECT) return false;
  map_user_settings_model(ev, d);
  const ccord_json *gf = ccord_json_get(d, "guild_folders");
  if (!gf || ccord_json_type_of(gf) != CCORD_JSON_ARRAY) {
    if (require_folders) return false;
    (void)map_user_settings_custom_status(ev, d);
    return true;
  }
  size_t nf = ccord_json_array_len(gf);
  ccord_guild_folder_item *items =
    ccord_arena_alloc(&ev->arena, CCORD_MAX_GUILDS * sizeof *items, _Alignof(ccord_guild_folder_item));
  if (!items) return false;
  size_t n = 0;
  for (size_t f = 0; f < nf && n < CCORD_MAX_GUILDS; f++) {
    const ccord_json *fj = ccord_json_array_at(gf, f);
    const ccord_json *gids = fj ? ccord_json_get(fj, "guild_ids") : nullptr;
    size_t ngids = gids ? ccord_json_array_len(gids) : 0;
    ccord_id fid = CCORD_ID_NONE;
    char fname[64];
    uint32_t fcolor = 0;
    parse_json_folder_meta(fj, ngids, f, &fid, fname, &fcolor);
    for (size_t k = 0; k < ngids && n < CCORD_MAX_GUILDS; k++) {
      ccord_id gid = CCORD_ID_NONE;
      if (!json_id(ccord_json_array_at(gids, k), &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) continue;
      bool dup = false;
      for (size_t p = 0; p < n; p++) {
        if (items[p].guild_id == gid) { dup = true; break; }
      }
      if (dup) continue;
      ccord_guild_folder_item *it = &items[n++];
      memset(it, 0, sizeof *it);
      it->guild_id = gid;
      it->folder_id = fid;
      if (fid != CCORD_ID_NONE) {
        memcpy(it->folder_name, fname, sizeof it->folder_name);
        it->folder_color = fcolor;
      }
    }
  }
  ev->ev.type = CCORD_NET_GUILD_LAYOUT;
  ev->ev.folder_items = items;
  ev->ev.folder_item_count = n;
  return true;
}

static bool map_user_settings_proto_update(ccord_live_event *ev, const ccord_json *d)
{
  if (!d || ccord_json_type_of(d) != CCORD_JSON_OBJECT) return false;
  const ccord_json *settings = ccord_json_get(d, "settings");
  if (!settings || ccord_json_type_of(settings) != CCORD_JSON_OBJECT) return true;
  int64_t proto_type = json_i64(settings, "type", 0);
  if (proto_type != 1 && proto_type != 2) return true;
  const char *proto_b64 = jstr(settings, "proto");
  if (!proto_b64 || proto_b64[0] == '\0') return true;
  size_t raw_len = 0;
  uint8_t *raw = b64_decode_alloc(proto_b64, strlen(proto_b64), &raw_len);
  if (!raw || raw_len == 0) {
    ccord_free(raw);
    return true;
  }
  if (proto_type == 2) {
    // FrecencyUserSettings (favourite GIFs, emoji): the whole proto rides along as raw_proto; the session decides what it
    // says. _partial says it is a delta (no removal can be concluded from it).
    uint8_t *keep2 = ccord_arena_alloc(&ev->arena, raw_len, 1u);
    if (!keep2) {
      ccord_free(raw);
      return false;
    }
    memcpy(keep2, raw, raw_len);
    ccord_free(raw);
    ev->ev.raw_proto = keep2;
    ev->ev.raw_proto_len = raw_len;
    ev->ev.settings_proto1_partial = json_bool_key(d, "partial", false);
    ev->ev.type = CCORD_NET_USER_SETTINGS_PROTO;
    return true;
  }
  ccord_guild_folder_item *items =
    ccord_arena_alloc(&ev->arena, CCORD_MAX_GUILDS * sizeof *items, _Alignof(ccord_guild_folder_item));
  // The whole type-1 proto rides along (status, and the base for writing it back); the session decodes what it needs.
  uint8_t *keep = ccord_arena_alloc(&ev->arena, raw_len, 1u);
  if (!items || !keep) {
    ccord_free(raw);
    return false;
  }
  memcpy(keep, raw, raw_len);
  ev->ev.settings_proto1 = keep;
  ev->ev.settings_proto1_len = raw_len;
  ev->ev.settings_proto1_partial = json_bool_key(d, "partial", false);
  ev->ev.type = CCORD_NET_USER_SETTINGS_PROTO;
  bool saw_field14 = false;
  size_t n = 0;
  size_t pos = 0;
  size_t folder_seq = 0;
  while (pos < raw_len && n < CCORD_MAX_GUILDS) {
    uint64_t tag = 0;
    if (!pb_read_varint(raw, raw_len, &pos, &tag)) break;
    uint32_t field_num = (uint32_t)(tag >> 3);
    uint32_t wire_type = (uint32_t)(tag & 7u);
    if (field_num == 14 && wire_type == 2) {
      saw_field14 = true;
      uint64_t gf_len = 0;
      if (!pb_read_varint(raw, raw_len, &pos, &gf_len) || gf_len > raw_len - pos) break;
      const uint8_t *gf = raw + pos;
      size_t ngf = (size_t)gf_len;
      pos += ngf;
      size_t gpos = 0;
      while (gpos < ngf && n < CCORD_MAX_GUILDS) {
        uint64_t ftag = 0;
        if (!pb_read_varint(gf, ngf, &gpos, &ftag)) break;
        uint32_t fnum = (uint32_t)(ftag >> 3);
        uint32_t fwire = (uint32_t)(ftag & 7u);
        if (fnum == 1 && fwire == 2) {
          uint64_t flen = 0;
          if (!pb_read_varint(gf, ngf, &gpos, &flen) || flen > ngf - gpos) break;
          pb_folder_parsed fp;
          pb_decode_one_folder(gf + gpos, (size_t)flen, nullptr, folder_seq++, &fp);
          gpos += (size_t)flen;
          for (size_t k = 0; k < fp.ngids && n < CCORD_MAX_GUILDS; k++) {
            ccord_id gid = fp.gids[k];
            if (gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) continue;
            bool dup = false;
            for (size_t p = 0; p < n; p++) {
              if (items[p].guild_id == gid) { dup = true; break; }
            }
            if (dup) continue;
            ccord_guild_folder_item *it = &items[n++];
            memset(it, 0, sizeof *it);
            it->guild_id = gid;
            it->folder_id = fp.folder_id;
            if (fp.folder_id != CCORD_ID_NONE) {
              memcpy(it->folder_name, fp.name, sizeof it->folder_name);
              it->folder_color = fp.color;
            }
          }
        } else {
          if (!pb_skip_field(gf, ngf, &gpos, fwire)) break;
        }
      }
    } else {
      if (!pb_skip_field(raw, raw_len, &pos, wire_type)) break;
    }
  }
  ccord_free(raw);
  if (saw_field14) {
    ev->ev.type = CCORD_NET_GUILD_LAYOUT;
    ev->ev.folder_items = items;
    ev->ev.folder_item_count = n;
  }
  return true;
}

// SESSIONS_REPLACE: the list of the account's gateway sessions. A user never receives a PRESENCE_UPDATE for itself, so this
// is how the status it is shown with is learned: the session with session_id "all" is the overall presence (what other
// users see). Without an "all" entry (one session, or every session in the same state) any session stands for it. Same
// reading as discord.py-self's ConnectionState.parse_sessions_replace (public source, read): the payload is an array of
// {session_id, status, active, activities, client_info}, at most 15 entries, our own session may be missing, and "all"
// may be absent. Nothing is emitted when no session carries a status this client can show (e.g. "unknown").
// Its activities ride along as the account's own activity (has_presence_detail marks that they were listed).
static int session_status_rank(const char *status)
{
  if (!status) return -1;
  if (strcmp(status, "dnd") == 0) return 3;
  if (strcmp(status, "online") == 0) return 2;
  if (strcmp(status, "idle") == 0) return 1;
  if (strcmp(status, "invisible") == 0 || strcmp(status, "offline") == 0) return 0;
  return -1;
}

static void map_sessions_replace(ccord_live_event *ev, const ccord_json *d)
{
  if (!d || ccord_json_type_of(d) != CCORD_JSON_ARRAY) return;
  size_t n = ccord_json_array_len(d);
  if (n == 0) {
    ev->ev.type = CCORD_NET_SESSIONS_REPLACE;
    ev->ev.presence = CCORD_PRESENCE_OFFLINE;
    ev->ev.has_presence_detail = true;
    ev->ev.has_activity = false;
    return;
  }
  const ccord_json *pick = nullptr;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *sj = ccord_json_array_at(d, i);
    const char *sid = sj ? jstr(sj, "session_id") : nullptr;
    if (sid && strcmp(sid, "all") == 0) pick = sj;
  }
  if (pick) {
    const char *status = jstr(pick, "status");
    if (session_status_rank(status) < 0) return;
    ev->ev.type = CCORD_NET_SESSIONS_REPLACE;
    ev->ev.presence = presence_from(status);
    const ccord_json *acts = ccord_json_get(pick, "activities");
    if (acts && ccord_json_type_of(acts) == CCORD_JSON_ARRAY) {
      parsed_presence_detail pd;
      extract_presence_detail(pick, &pd);
      ev->ev.has_presence_detail = true;
      ev->ev.has_activity = pd.has_activity;
      ev->ev.activity_type = pd.activity_type;
      memcpy(ev->ev.activity_name, pd.activity_name, sizeof ev->ev.activity_name);
      memcpy(ev->ev.activity_details, pd.activity_details, sizeof ev->ev.activity_details);
      memcpy(ev->ev.activity_state, pd.activity_state, sizeof ev->ev.activity_state);
      ev->ev.activity_start_ms = pd.activity_start_ms;
      ev->ev.activity_end_ms = pd.activity_end_ms;
    }
    return;
  }
  // Without an "all" entry, aggregate highest priority status (dnd > online > idle > offline)
  // and pick activity from the first session with non-empty activities.
  const ccord_json *best_status_sj = nullptr;
  int best_rank = -1;
  bool any_acts = false;
  const ccord_json *act_pick = nullptr;
  parsed_presence_detail best_pd = {0};
  for (size_t i = 0; i < n; i++) {
    const ccord_json *sj = ccord_json_array_at(d, i);
    if (!sj) continue;
    const char *st = jstr(sj, "status");
    int rk = session_status_rank(st);
    if (rk > best_rank) {
      best_rank = rk;
      best_status_sj = sj;
    }
    const ccord_json *acts = ccord_json_get(sj, "activities");
    if (acts && ccord_json_type_of(acts) == CCORD_JSON_ARRAY) {
      any_acts = true;
      if (ccord_json_array_len(acts) > 0) {
        parsed_presence_detail cand;
        extract_presence_detail(sj, &cand);
        if (!act_pick || (!best_pd.has_activity && cand.has_activity)) {
          act_pick = sj;
          best_pd = cand;
        }
      }
    }
  }
  if (!best_status_sj) return;
  ev->ev.type = CCORD_NET_SESSIONS_REPLACE;
  ev->ev.presence = presence_from(jstr(best_status_sj, "status"));
  if (act_pick) {
    ev->ev.has_presence_detail = true;
    ev->ev.has_activity = best_pd.has_activity;
    ev->ev.activity_type = best_pd.activity_type;
    memcpy(ev->ev.activity_name, best_pd.activity_name, sizeof ev->ev.activity_name);
    memcpy(ev->ev.activity_details, best_pd.activity_details, sizeof ev->ev.activity_details);
    memcpy(ev->ev.activity_state, best_pd.activity_state, sizeof ev->ev.activity_state);
    ev->ev.activity_start_ms = best_pd.activity_start_ms;
    ev->ev.activity_end_ms = best_pd.activity_end_ms;
  } else if (any_acts) {
    ev->ev.has_presence_detail = true;
    ev->ev.has_activity = false;
  }
}

static bool parse_hex_color(const char *s, uint32_t *out);

static bool parse_ready(ccord_live_event *ev, const ccord_json *d, ccord_error *err)
{
  if (!d || ccord_json_type_of(d) != CCORD_JSON_OBJECT) return false;
  const ccord_json *me = ccord_json_get(d, "user");
  ccord_id self = CCORD_ID_NONE;
  if (!me || !json_id_key(me, "id", &self)) {
    ccord_error_set(err, CCORD_ERR_PARSE, "gateway: READY without user");
    return false;
  }
  const char *uname = jstr(me, "global_name");
  if (!uname || uname[0] == '\0') uname = jstr(me, "username");
  if (!text_ok(uname, CCORD_NAME_MAX_BYTES)) uname = "You";
  else {
    uname = arena_dup(&ev->arena, uname);
    if (uname[0] == '\0') uname = "You";
  }
  ev->session_id = arena_dup_nullable(&ev->arena, jstr(d, "session_id"));
  ev->resume_gateway_url = arena_dup_nullable(&ev->arena, jstr(d, "resume_gateway_url"));
  // M6 account settings ride READY; the snapshot event keeps ev.type == CCORD_NET_SNAPSHOT.
  const ccord_json *uset = ccord_json_get(d, "user_settings");
  map_user_settings_model(ev, uset);
  {   // READY's type-1 proto is the base a later status write patches (the snapshot itself ignores it here)
    const char *p1 = jstr(d, "user_settings_proto");
    size_t p1_len = 0;
    uint8_t *p1_raw = p1 && p1[0] != '\0' ? b64_decode_alloc(p1, strlen(p1), &p1_len) : nullptr;
    uint8_t *p1_keep = p1_raw && p1_len > 0 ? ccord_arena_alloc(&ev->arena, p1_len, 1u) : nullptr;
    if (p1_keep) {
      memcpy(p1_keep, p1_raw, p1_len);
      ev->ev.settings_proto1 = p1_keep;
      ev->ev.settings_proto1_len = p1_len;
    }
    ccord_free(p1_raw);
  }
  snapshot_builder b = {.arena = &ev->arena, .self_id = self};
  const ccord_json *botj = ccord_json_get(me, "bot");
  bool is_bot = botj && ccord_json_bool(botj, false);
  const ccord_json *prof = ccord_json_get(d, "user_profile");
  const char *self_bio = jstr(me, "bio");
  if ((!self_bio || self_bio[0] == '\0') && prof) self_bio = jstr(prof, "bio");
  const char *self_pronouns = jstr(me, "pronouns");
  if ((!self_pronouns || self_pronouns[0] == '\0') && prof) self_pronouns = jstr(prof, "pronouns");
  const char *self_banner = jstr(me, "banner");
  if ((!self_banner || self_banner[0] == '\0') && prof) self_banner = jstr(prof, "banner");
  const ccord_json *acj = ccord_json_get(me, "accent_color");
  bool has_self_accent = acj && ccord_json_type_of(acj) == CCORD_JSON_NUMBER;
  int64_t self_accent = has_self_accent ? json_i64(me, "accent_color", 0) : -1;
  if (!has_self_accent && prof) {
    const ccord_json *pac = ccord_json_get(prof, "accent_color");
    if (pac && ccord_json_type_of(pac) == CCORD_JSON_NUMBER) {
      has_self_accent = true;
      self_accent = json_i64(prof, "accent_color", 0);
    }
  }
  if (self_accent < 0 || self_accent > (int64_t)0xFFFFFF) {
    self_accent = 0;
    if (!has_self_accent) {
      uint32_t bcol = 0;
      if (parse_hex_color(jstr(me, "banner_color"), &bcol) ||
          (prof && parse_hex_color(jstr(prof, "banner_color"), &bcol))) {
        self_accent = (int64_t)bcol;
        has_self_accent = true;
      }
    }
  }
  ccord_user self_user = {
    .id = self,
    .name = uname,
    .handle = opt_text_dup(&ev->arena, jstr(me, "username"), CCORD_NAME_MAX_BYTES),
    .status_text = "",
    .avatar_url = build_cdn_url(&ev->arena, "avatars", self, jstr(me, "avatar")),
    .banner_url = build_cdn_url(&ev->arena, "banners", self, self_banner),
    .accent_color = (uint32_t)self_accent,
    .has_accent_color = has_self_accent,
    .bio = opt_text_dup(&ev->arena, self_bio, CCORD_USER_BIO_MAX_BYTES),
    .pronouns = opt_text_dup(&ev->arena, self_pronouns, CCORD_USER_PRONOUNS_MAX_BYTES),
    .presence = CCORD_PRESENCE_ONLINE,
    .is_bot = is_bot,
    .avatar_palette = (uint8_t)(self % 6u),
  };
  extract_clan_info(me, self_user.clan_tag, &self_user.clan_guild_id, self_user.clan_badge);
  {
    const ccord_json *cs_init = uset ? ccord_json_get(uset, "custom_status") : nullptr;
    if (cs_init && ccord_json_type_of(cs_init) == CCORD_JSON_OBJECT) {
      copy_trunc_utf8(self_user.custom_status_text, sizeof self_user.custom_status_text, jstr(cs_init, "text"));
      copy_trunc_utf8(self_user.custom_status_emoji, sizeof self_user.custom_status_emoji, jstr(cs_init, "emoji_name"));
      self_user.custom_status_expires_ms = json_ts_ms(cs_init, "expires_at");
      const char *cs_st = opt_text_dup(&ev->arena, jstr(cs_init, "text"), LIVE_STATUS_MAX_BYTES);
      if (cs_st[0] != '\0') self_user.status_text = cs_st;
    }
  }
  bool ok = sb_user(&b, &self_user);
  const ccord_json *users_arr = ccord_json_get(d, "users");
  size_t nu = users_arr ? ccord_json_array_len(users_arr) : 0;
  for (size_t i = 0; ok && i < nu; i++) {
    ccord_user top_u;
    if (map_user_obj(&ev->arena, ccord_json_array_at(users_arr, i), CCORD_PRESENCE_OFFLINE, &top_u)) {
      ok = sb_user(&b, &top_u);
    }
  }
  const ccord_json *guilds_arr = ccord_json_get(d, "guilds");
  const ccord_json *merged_arr = ccord_json_get(d, "merged_members");
  const ccord_json *pchs_arr = ccord_json_get(d, "private_channels");
  const ccord_json *rels_arr = ccord_json_get(d, "relationships");
  size_t ng = guilds_arr ? ccord_json_array_len(guilds_arr) : 0;
  size_t np = pchs_arr ? ccord_json_array_len(pchs_arr) : 0;
  // Seed DM recipients and relationship user objects BEFORE guild members so a server nickname in
  // d.guilds[].members never claims the user's global display name ahead of their real account name.
  for (size_t i = 0; ok && i < np; i++) {
    const ccord_json *pc = ccord_json_array_at(pchs_arr, i);
    const ccord_json *recs = pc ? ccord_json_get(pc, "recipients") : nullptr;
    size_t nr = recs ? ccord_json_array_len(recs) : 0;
    for (size_t r = 0; ok && r < nr; r++) {
      ccord_user dm_u;
      if (map_user_obj(&ev->arena, ccord_json_array_at(recs, r), CCORD_PRESENCE_OFFLINE, &dm_u)) {
        ok = sb_user(&b, &dm_u);
      }
    }
  }
  size_t nrel = rels_arr ? ccord_json_array_len(rels_arr) : 0;
  for (size_t i = 0; ok && i < nrel; i++) {
    const ccord_json *rj = ccord_json_array_at(rels_arr, i);
    const ccord_json *ru = rj ? ccord_json_get(rj, "user") : nullptr;
    ccord_id ruid = CCORD_ID_NONE;
    if (!json_id_key(rj, "user_id", &ruid) && (!ru || !json_id_key(ru, "id", &ruid))) {
      (void)json_id_key(rj, "id", &ruid);
    }
    if (ruid == CCORD_ID_NONE || ruid == self) continue;
    ccord_relationship_kind rkind = rel_kind_from(json_i64(rj, "type", 0));
    if (rkind == CCORD_REL_NONE) continue;
    ccord_user rel_u;
    if (ru && map_user_obj(&ev->arena, ru, CCORD_PRESENCE_OFFLINE, &rel_u)) {
      rel_u.id = ruid;
      ok = sb_user(&b, &rel_u);
    } else {
      ok = sb_ensure_user_stub(&b, ruid);
      if (!ok) break;
    }
    if (ok) {
      ccord_relationship rel = {.user_id = ruid, .kind = rkind};
      ok = sb_relationship(&b, &rel);
    }
  }
  // Always reserve one guild slot for the DM pseudo guild so DM Home is always available.
  size_t guild_cap = CCORD_MAX_REAL_GUILDS;
  if (ng > 0 && ok) {
    bool *visited = ccord_calloc(ng, sizeof *visited);
    id_index ready_guild_idx = {0};
    if (!visited || !ccord_idmap_reserve(&ready_guild_idx, ng)) {
      ccord_idmap_free(&ready_guild_idx);
      ccord_free(visited);
      ok = false;
    } else {
      for (size_t i = 0; i < ng; i++) {
        const ccord_json *gj = ccord_json_array_at(guilds_arr, i);
        const ccord_json *gprops = guild_props(gj);
        ccord_id gid = CCORD_ID_NONE;
        if (guild_id_key_p(gj, gprops, "id", &gid) && gid != CCORD_ID_NONE &&
            !ccord_idmap_find(&ready_guild_idx, gid, nullptr)) {
          if (!ccord_idmap_put(&ready_guild_idx, gid, i)) {
            ok = false;
            break;
          }
        }
      }
      const ccord_json *gfolders = uset ? ccord_json_get(uset, "guild_folders") : nullptr;
      size_t nfolders = gfolders ? ccord_json_array_len(gfolders) : 0;
      for (size_t f = 0; ok && f < nfolders && b.guild_count < guild_cap; f++) {
        const ccord_json *fj = ccord_json_array_at(gfolders, f);
        const ccord_json *gids = fj ? ccord_json_get(fj, "guild_ids") : nullptr;
        size_t ngids = gids ? ccord_json_array_len(gids) : 0;
        ccord_id fid = CCORD_ID_NONE;
        char fname[64];
        uint32_t fcolor = 0;
        parse_json_folder_meta(fj, ngids, f, &fid, fname, &fcolor);
        for (size_t k = 0; ok && k < ngids && b.guild_count < guild_cap; k++) {
          ccord_id want_gid = CCORD_ID_NONE;
          if (!json_id(ccord_json_array_at(gids, k), &want_gid)) continue;
          emit_want_guild_folder(&b, guilds_arr, merged_arr, &ready_guild_idx, ng, guild_cap, visited,
                                 want_gid, fid, fname, fcolor, &ok);
        }
      }
      const ccord_json *gpos = uset ? ccord_json_get(uset, "guild_positions") : nullptr;
      size_t npos = gpos ? ccord_json_array_len(gpos) : 0;
      for (size_t p = 0; ok && p < npos && b.guild_count < guild_cap; p++) {
        ccord_id want_gid = CCORD_ID_NONE;
        if (!json_id(ccord_json_array_at(gpos, p), &want_gid)) continue;
        emit_want_guild(&b, guilds_arr, merged_arr, &ready_guild_idx, ng, guild_cap, visited, want_gid, &ok);
      }
      const char *proto_b64 = jstr(d, "user_settings_proto");
      if (proto_b64 && proto_b64[0] != '\0') {
        apply_proto_guild_order(proto_b64, &b, guilds_arr, merged_arr, &ready_guild_idx, ng, guild_cap, visited, &ok);
      }
      for (size_t i = 0; ok && i < ng && b.guild_count < guild_cap; i++) {
        if (visited[i]) continue;
        visited[i] = true;
        ok = map_guild_ex(&b, ccord_json_array_at(guilds_arr, i),
                          merged_arr ? ccord_json_array_at(merged_arr, i) : nullptr);
      }
      ccord_idmap_free(&ready_guild_idx);
      ccord_free(visited);
    }
  }
  for (size_t i = 0; ok && i < np; i++) {
    const ccord_json *pc = ccord_json_array_at(pchs_arr, i);
    const ccord_json *rids = pc ? ccord_json_get(pc, "recipient_ids") : nullptr;
    size_t nids = rids ? ccord_json_array_len(rids) : 0;
    for (size_t r = 0; ok && r < nids; r++) {
      ccord_id ruid = CCORD_ID_NONE;
      if (json_id(ccord_json_array_at(rids, r), &ruid) && ruid != CCORD_ID_NONE) {
        ok = sb_ensure_user_stub(&b, ruid);
      }
    }
  }
  const ccord_json *pres_arr = ccord_json_get(d, "presences");
  size_t npres = pres_arr ? ccord_json_array_len(pres_arr) : 0;
  for (size_t i = 0; ok && i < npres; i++) {
    const ccord_json *pj = ccord_json_array_at(pres_arr, i);
    const ccord_json *pu = pj ? ccord_json_get(pj, "user") : nullptr;
    ccord_id pid = CCORD_ID_NONE;
    if (!json_id_key(pu, "id", &pid) && !json_id_key(pj, "user_id", &pid)) continue;
    ccord_presence pres = presence_from(jstr(pj, "status"));
    const char *stext = extract_activity_status(&ev->arena, pj);
    size_t uidx = 0;
    if (ccord_idmap_find(&b.user_idx, pid, &uidx) && uidx < b.user_count) {
      b.users[uidx].presence = pres;
      b.users[uidx].status_text = stext;
      parsed_presence_detail pd;
      extract_presence_detail(pj, &pd);
      apply_presence_detail_to_user(&b.users[uidx], &pd);
    }
  }
  size_t dm_start = b.channel_count;
  if (ok && np > 0) {
    raw_dm_channel *dms = ccord_calloc(np, sizeof *dms);
    if (!dms) {
      ok = false;
    } else {
      size_t ndms = 0;
      for (size_t i = 0; i < np; i++) {
        const ccord_json *pc = ccord_json_array_at(pchs_arr, i);
        ccord_channel c;
        if (!map_channel_ex(&ev->arena, b.users, b.user_count, &b.user_idx, self, pc, CCORD_DM_GUILD_ID, &c)) {
          continue;
        }
        dms[ndms++] = (raw_dm_channel){.chan = c, .last_message_id = c.last_message_id};
      }
      if (ndms > 1) {
        qsort(dms, ndms, sizeof *dms, cmp_raw_dm_channel);
      }
      for (size_t i = 0; ok && i < ndms; i++) {
        ok = sb_channel(&b, &dms[i].chan);
      }
      ccord_free(dms);
    }
  }
  if (ok) {
    apply_ready_read_state(&b, d);
    if (b.channel_count > dm_start + 1u) {
      qsort(&b.channels[dm_start], b.channel_count - dm_start, sizeof *b.channels, cmp_dm_channel_desc);
      // Deduplicate 1:1 DM channels that share the same recipient user_id (keep the first, most recent one).
      id_index dm_uid_seen = {0};
      bool use_dm_map = ccord_idmap_reserve(&dm_uid_seen, b.channel_count - dm_start);
      size_t out_dm = dm_start;
      for (size_t i = dm_start; i < b.channel_count; i++) {
        const ccord_channel *ch = &b.channels[i];
        bool dup = false;
        if (ch->discord_type != CCORD_DISCORD_CHANNEL_GROUP_DM && ch->user_id != CCORD_ID_NONE) {
          if (use_dm_map) {
            if (ccord_idmap_find(&dm_uid_seen, ch->user_id, nullptr)) {
              dup = true;
            } else if (!ccord_idmap_put(&dm_uid_seen, ch->user_id, out_dm)) {
              ccord_idmap_free(&dm_uid_seen);
              use_dm_map = false;
            }
          } else {
            for (size_t j = dm_start; j < out_dm; j++) {
              if (b.channels[j].discord_type != CCORD_DISCORD_CHANNEL_GROUP_DM &&
                  b.channels[j].user_id == ch->user_id) {
                dup = true;
                break;
              }
            }
          }
        }
        if (!dup) {
          if (out_dm != i) b.channels[out_dm] = *ch;
          out_dm++;
        }
      }
      ccord_idmap_free(&dm_uid_seen);
      b.channel_count = out_dm;
    }
    if (b.channel_count - dm_start > CCORD_MAX_CHANNELS_PER_GUILD) {
      b.channel_count = dm_start + CCORD_MAX_CHANNELS_PER_GUILD;
    }
    if (b.channel_count > dm_start) {
      ccord_idmap_free(&b.chan_idx);
      for (size_t i = 0; i < b.channel_count; i++) {
        (void)ccord_idmap_put(&b.chan_idx, b.channels[i].id, i);
      }
    }
  }
  const ccord_json *vs_arr = ccord_json_get(d, "voice_states");
  size_t nv = vs_arr ? ccord_json_array_len(vs_arr) : 0;
  for (size_t i = 0; ok && i < nv; i++) {
    ok = collect_voice_state_entry(&b, ccord_json_array_at(vs_arr, i), CCORD_ID_NONE, false, true, true);
  }
  if (ok) ok = sb_finish(&b, ev);
  if (ok && ng > 0) {
    const ccord_json **gl = ccord_calloc(ng, sizeof *gl);
    if (gl) {
      const ccord_json **ml = ccord_calloc(ng, sizeof *ml);
      for (size_t i = 0; i < ng; i++) {
        gl[i] = ccord_json_array_at(guilds_arr, i);
        if (ml) ml[i] = merged_arr ? ccord_json_array_at(merged_arr, i) : nullptr;
      }
      expr_collect_guild(ev, gl, ng);
      perm_collect_guilds(ev, gl, ml, ng, ev->snapshot.self_id);
      ccord_free(ml);
      ccord_free(gl);
    }
  }
  if (ok) notify_collect_ready(ev, d);
  if (ok) notes_collect_ready(ev, d);
  if (!ok) ccord_error_set(err, CCORD_ERR_NOMEM, "gateway: READY too large");
  sb_free(&b);
  return ok;
}

static bool map_typing(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id channel = CCORD_ID_NONE, user = CCORD_ID_NONE;
  if (!json_id_key(d, "channel_id", &channel)) return false;
  if (!json_id_key(d, "user_id", &user)) return false;
  int64_t ts = json_i64(d, "timestamp", 0);
  int64_t expiry;
  if (ts > 0 && ts < INT64_MAX / 1000) {
    expiry = ts * 1000 + 10000;
  } else {
    expiry = ccord_clock_unix_ms() + 10000;
  }
  ev->ev.type = CCORD_NET_TYPING_START;
  ev->ev.channel_id = channel;
  ev->ev.user_id = user;
  ev->ev.timestamp_ms = expiry;
  return true;
}

static ccord_presence presence_from(const char *status)
{
  if (status) {
    if (strcmp(status, "online") == 0) return CCORD_PRESENCE_ONLINE;
    if (strcmp(status, "idle") == 0) return CCORD_PRESENCE_IDLE;
    if (strcmp(status, "dnd") == 0) return CCORD_PRESENCE_DND;
  }
  return CCORD_PRESENCE_OFFLINE;
}

static bool map_presence(ccord_live_event *ev, const ccord_json *d)
{
  const ccord_json *user = ccord_json_get(d, "user");
  ccord_id uid = CCORD_ID_NONE;
  if (!user || !json_id_key(user, "id", &uid)) {
    if (!json_id_key(d, "user_id", &uid)) return false;
  }
  const char *status = jstr(d, "status");
  ccord_presence p = presence_from(status);
  const char *text = extract_activity_status(&ev->arena, d);
  parsed_presence_detail pd;
  extract_presence_detail(d, &pd);
  ev->ev.type = CCORD_NET_PRESENCE_UPDATE;
  ev->ev.user_id = uid;
  ev->ev.presence = p;
  ev->ev.status_text = text;
  ev->ev.has_presence_detail = true;
  memcpy(ev->ev.custom_status_emoji, pd.custom_status_emoji, sizeof ev->ev.custom_status_emoji);
  memcpy(ev->ev.custom_status_text, pd.custom_status_text, sizeof ev->ev.custom_status_text);
  ev->ev.custom_status_expires_ms = pd.custom_status_expires_ms;
  ev->ev.has_activity = pd.has_activity;
  ev->ev.activity_type = pd.activity_type;
  memcpy(ev->ev.activity_name, pd.activity_name, sizeof ev->ev.activity_name);
  memcpy(ev->ev.activity_details, pd.activity_details, sizeof ev->ev.activity_details);
  memcpy(ev->ev.activity_state, pd.activity_state, sizeof ev->ev.activity_state);
  ev->ev.activity_start_ms = pd.activity_start_ms;
  ev->ev.activity_end_ms = pd.activity_end_ms;
  return true;
}

static bool map_dm_recipient_change(ccord_live_event *ev, const ccord_json *d, bool added)
{
  if (!d || ccord_json_type_of(d) != CCORD_JSON_OBJECT) return false;
  ccord_id cid = CCORD_ID_NONE, uid = CCORD_ID_NONE;
  if (!json_id_key(d, "channel_id", &cid)) return false;
  const ccord_json *uj = ccord_json_get(d, "user");
  if (!uj || !json_id_key(uj, "id", &uid)) {
    if (!json_id_key(d, "user_id", &uid)) return false;
  }
  if (uj && ccord_json_type_of(uj) == CCORD_JSON_OBJECT) {
    ccord_user parsed;
    if (map_user_obj(&ev->arena, uj, CCORD_PRESENCE_OFFLINE, &parsed)) {
      ccord_user *slot = ccord_arena_alloc(&ev->arena, sizeof *slot, _Alignof(ccord_user));
      if (slot) {
        *slot = parsed;
        ev->users = slot;
        ev->user_count = 1;
        ev->ev.users = slot;
        ev->ev.user_count = 1;
      }
    }
  }
  ev->ev.type = CCORD_NET_DM_RECIPIENT_CHANGE;
  ev->ev.guild_id = CCORD_DM_GUILD_ID;
  ev->ev.channel_id = cid;
  ev->ev.user_id = uid;
  ev->ev.recipient_added = added;
  return true;
}

static bool map_user_note_update(ccord_live_event *ev, const ccord_json *d, ccord_id fallback_uid)
{
  if (!d || ccord_json_type_of(d) != CCORD_JSON_OBJECT) return false;
  ccord_id uid = fallback_uid;
  if (!json_id_key(d, "id", &uid) && !json_id_key(d, "user_id", &uid) && uid == CCORD_ID_NONE) {
    return false;
  }
  const ccord_json *nj = ccord_json_get(d, "note");
  size_t nlen = 0;
  const char *nstr = (nj && ccord_json_type_of(nj) == CCORD_JSON_STRING) ? ccord_json_string(nj, &nlen) : "";
  if (!nstr) nstr = "";
  size_t cut = utf8_trunc_len(nstr, nlen, CCORD_USER_NOTE_MAX_BYTES);
  if (cut > 0 && !ccord_utf8_valid_text(nstr, cut)) cut = 0;
  char *copy = ccord_arena_strndup(&ev->arena, nstr, cut);
  if (!copy) return false;
  ev->ev.type = CCORD_NET_USER_NOTE_UPDATE;
  ev->ev.user_id = uid;
  ev->ev.text = copy;
  return true;
}

static void attach_channel_recipients(ccord_live_event *ev, const ccord_json *d)
{
  const ccord_json *recs = d ? ccord_json_get(d, "recipients") : nullptr;
  size_t nr = recs ? ccord_json_array_len(recs) : 0;
  if (nr == 0) return;
  if (nr > 32) nr = 32;
  ccord_user *users = ccord_arena_alloc(&ev->arena, nr * sizeof *users, _Alignof(ccord_user));
  if (!users) return;
  size_t kept = 0;
  for (size_t i = 0; i < nr; i++) {
    if (map_user_obj(&ev->arena, ccord_json_array_at(recs, i), CCORD_PRESENCE_OFFLINE, &users[kept])) {
      kept++;
    }
  }
  if (kept > 0) {
    ev->users = users;
    ev->user_count = kept;
    ev->ev.users = users;
    ev->ev.user_count = kept;
  }
}

static bool map_channel_event(ccord_live_event *ev, const ccord_json *d, bool upsert)
{
  ccord_id id = CCORD_ID_NONE, gid = CCORD_ID_NONE;
  (void)json_id_key(d, "guild_id", &gid);
  if (!upsert) {
    if (!json_id_key(d, "id", &id)) return false;
    ev->ev.type = CCORD_NET_CHANNEL_DELETE;
    ev->ev.guild_id = gid;
    ev->ev.channel_id = id;
    return true;
  }
  ccord_channel c;
  if (!map_channel(&ev->arena, d, gid, &c)) return false;
  ccord_channel *slot = ccord_arena_alloc(&ev->arena, sizeof *slot, _Alignof(ccord_channel));
  if (!slot) return false;
  *slot = c;
  ev->channels = slot;
  ev->channel_count = 1;
  attach_channel_recipients(ev, d);
  {
    perm_lists pl = {0};
    (void)perm_add_channel(&ev->arena, &pl, d);
    (void)perm_publish(ev, &pl);
  }
  if (ccord_discord_type_is_forum(c.discord_type)) {
    ccord_forum_meta *fm = ccord_arena_alloc(&ev->arena, sizeof *fm, _Alignof(ccord_forum_meta));
    if (fm) {
      map_forum_meta(d, c.id, c.discord_type, fm);
      ev->ev.forum_metas = fm;
      ev->ev.forum_meta_count = 1;
    }
  } else if (ccord_discord_type_is_thread(c.discord_type) && c.parent_id != CCORD_ID_NONE) {
    ccord_forum_post *fp = ccord_arena_alloc(&ev->arena, sizeof *fp, _Alignof(ccord_forum_post));
    if (fp && map_forum_post(&ev->arena, d, ccord_json_get(d, "message"), c.parent_id, c.guild_id, fp)) {
      ev->ev.forum_posts = fp;
      ev->ev.forum_post_count = 1;
    }
  }
  ev->ev.type = CCORD_NET_CHANNEL_UPSERT;
  ev->ev.guild_id = c.guild_id;
  ev->ev.channel_id = c.id;
  ev->ev.channel = slot;
  return true;
}

static bool map_thread_list_sync(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  (void)json_id_key(d, "guild_id", &gid);
  const ccord_json *th_arr = d ? ccord_json_get(d, "threads") : nullptr;
  size_t nt = th_arr ? ccord_json_array_len(th_arr) : 0;
  if (nt > CCORD_FORUM_MAX_POSTS) nt = CCORD_FORUM_MAX_POSTS;
  ccord_forum_post *posts = nt > 0
    ? ccord_arena_alloc(&ev->arena, nt * sizeof *posts, _Alignof(ccord_forum_post))
    : nullptr;
  if (nt > 0 && !posts) return false;
  size_t kept = 0;
  for (size_t i = 0; i < nt; i++) {
    const ccord_json *tj = ccord_json_array_at(th_arr, i);
    if (map_forum_post(&ev->arena, tj, nullptr, CCORD_ID_NONE, gid, &posts[kept])) {
      kept++;
    }
  }
  ev->ev.type = CCORD_NET_FORUM_POSTS;
  ev->ev.guild_id = gid;
  ev->ev.channel_id = CCORD_ID_NONE;
  ev->ev.forum_posts = posts;
  ev->ev.forum_post_count = kept;
  return true;
}

static bool map_reaction(ccord_live_event *ev, const ccord_json *d, const char *type)
{
  ccord_id channel = CCORD_ID_NONE, message = CCORD_ID_NONE, user = CCORD_ID_NONE, gid = CCORD_ID_NONE;
  if (!json_id_key(d, "channel_id", &channel)) return false;
  if (!json_id_key(d, "message_id", &message)) return false;
  (void)json_id_key(d, "user_id", &user);
  (void)json_id_key(d, "guild_id", &gid);
  bool is_remove_all = type && strcmp(type, "MESSAGE_REACTION_REMOVE_ALL") == 0;
  bool is_remove_emoji = type && strcmp(type, "MESSAGE_REACTION_REMOVE_EMOJI") == 0;
  bool is_add = type && strstr(type, "ADD") != nullptr;
  ev->ev.type = CCORD_NET_REACTION_UPDATE;
  ev->ev.guild_id = gid;
  ev->ev.channel_id = channel;
  ev->ev.message_id = message;
  ev->ev.user_id = user;
  if (is_remove_all) {
    ev->ev.reaction_count = 0;
    ev->ev.reaction_delta = 0;
    ev->ev.self_reacted = false;
    ev->ev.reaction_emoji = nullptr;
    return true;
  }
  const ccord_json *emj = ccord_json_get(d, "emoji");
  ev->ev.reaction_emoji = format_reaction_emoji(&ev->arena, emj, nullptr, nullptr);
  if (is_remove_emoji) {
    ev->ev.reaction_count = 0;
    ev->ev.reaction_delta = 0;
    ev->ev.self_reacted = false;
    return true;
  }
  const ccord_json *cj = ccord_json_get(d, "count");
  int64_t count = 0;
  bool has_count = cj && ccord_json_int64(cj, &count);
  if (has_count) {
    if (count < 0) count = 0;
    if (count > (int64_t)UINT32_MAX) count = (int64_t)UINT32_MAX;
    ev->ev.reaction_count = (uint32_t)count;
    ev->ev.reaction_delta = 0;
  } else {
    ev->ev.reaction_count = is_add ? 1u : 0u;
    ev->ev.reaction_delta = is_add ? 1 : -1;
  }
  const ccord_json *bj = ccord_json_get(d, "burst");
  ev->ev.reaction_burst = (bj && ccord_json_bool(bj, false)) || json_i64(d, "type", 0) == 1;
  ev->ev.self_reacted = is_add;
  return true;
}

static bool map_thread_member_update(ccord_live_event *ev, const ccord_json *d, bool is_members_batch)
{
  if (!is_json_object(d)) return false;
  ccord_id tid = CCORD_ID_NONE, gid = CCORD_ID_NONE, uid = CCORD_ID_NONE;
  if (!json_id_key(d, "id", &tid)) return false;
  (void)json_id_key(d, "guild_id", &gid);
  bool joined = true;
  if (!is_members_batch) {
    (void)json_id_key(d, "user_id", &uid);
  } else {
    const ccord_json *rem = ccord_json_get(d, "removed_member_ids");
    const ccord_json *add = ccord_json_get(d, "added_members");
    if (is_json_array(rem) && ccord_json_array_len(rem) == 1 && json_id(ccord_json_array_at(rem, 0), &uid)) {
      joined = false;
    } else if (is_json_array(add) && ccord_json_array_len(add) == 1) {
      const ccord_json *am = ccord_json_array_at(add, 0);
      if (!json_id_key(am, "user_id", &uid)) return true;
      joined = true;
    } else {
      return true;
    }
  }
  ev->ev.type = CCORD_NET_THREAD_MEMBER_SELF;
  ev->ev.guild_id = gid;
  ev->ev.channel_id = tid;
  ev->ev.user_id = uid;
  ev->ev.thread_joined = joined;
  return true;
}

static bool map_message_delete_bulk(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id channel = CCORD_ID_NONE, gid = CCORD_ID_NONE;
  if (!json_id_key(d, "channel_id", &channel)) return false;
  (void)json_id_key(d, "guild_id", &gid);
  const ccord_json *id_arr = d ? ccord_json_get(d, "ids") : nullptr;
  size_t n = id_arr ? ccord_json_array_len(id_arr) : 0;
  if (n == 0) return false;
  if (n > CCORD_MAX_MESSAGES_PER_CHANNEL) n = CCORD_MAX_MESSAGES_PER_CHANNEL;
  ccord_id *ids = ccord_arena_alloc(&ev->arena, n * sizeof *ids, _Alignof(ccord_id));
  if (!ids) return false;
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    ccord_id mid = CCORD_ID_NONE;
    if (json_id(ccord_json_array_at(id_arr, i), &mid) && mid != CCORD_ID_NONE) {
      ids[kept++] = mid;
    }
  }
  if (kept == 0) return false;
  ev->message_ids = ids;
  ev->message_id_count = kept;
  ev->ev.type = CCORD_NET_MESSAGE_DELETE_BULK;
  ev->ev.channel_id = channel;
  ev->ev.guild_id = gid;
  ev->ev.message_ids = ids;
  ev->ev.message_id_count = kept;
  return true;
}

static bool map_channel_pins_update(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id channel = CCORD_ID_NONE, gid = CCORD_ID_NONE;
  if (!json_id_key(d, "channel_id", &channel)) return false;
  (void)json_id_key(d, "guild_id", &gid);
  const char *ts = jstr(d, "last_pin_timestamp");
  int64_t stamp = (ts && ts[0] != '\0') ? iso_ms(ts, strlen(ts)) : 0;
  if (stamp < 0) stamp = 0;
  ev->ev.type = CCORD_NET_CHANNEL_PINS_UPDATE;
  ev->ev.channel_id = channel;
  ev->ev.guild_id = gid;
  ev->ev.timestamp_ms = stamp;
  return true;
}

static bool map_message_ack(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id channel = CCORD_ID_NONE, message = CCORD_ID_NONE;
  if (!json_id_key(d, "channel_id", &channel)) return false;
  (void)json_id_key(d, "message_id", &message);
  int64_t mentions = json_i64(d, "mention_count", 0);
  if (mentions < 0) mentions = 0;
  if (mentions > (int64_t)UINT32_MAX) mentions = (int64_t)UINT32_MAX;
  ev->ev.type = CCORD_NET_MESSAGE_ACK;
  ev->ev.channel_id = channel;
  ev->ev.message_id = message;
  ev->ev.mention_count = (uint32_t)mentions;
  return true;
}

static bool map_relationship_event(ccord_live_event *ev, const ccord_json *d, bool is_remove)
{
  const ccord_json *ru = ccord_json_get(d, "user");
  ccord_id uid = CCORD_ID_NONE;
  if (!json_id_key(d, "user_id", &uid) && (!ru || !json_id_key(ru, "id", &uid))) {
    if (!json_id_key(d, "id", &uid)) return false;
  }
  ccord_relationship_kind kind = is_remove ? CCORD_REL_NONE : rel_kind_from(json_i64(d, "type", 0));
  if (ru) {
    ccord_user u;
    if (map_user_obj(&ev->arena, ru, CCORD_PRESENCE_OFFLINE, &u)) {
      u.id = uid;
      ccord_user *slot = ccord_arena_alloc(&ev->arena, sizeof *slot, _Alignof(ccord_user));
      if (slot) {
        *slot = u;
        ev->users = slot;
        ev->user_count = 1;
        ev->ev.users = slot;
        ev->ev.user_count = 1;
      }
    }
  }
  ev->ev.type = CCORD_NET_RELATIONSHIP_UPDATE;
  ev->ev.user_id = uid;
  ev->ev.relationship_kind = kind;
  return true;
}

// ---- guild expressions (custom emoji, stickers, soundboard) ----

static bool json_bool_key(const ccord_json *obj, const char *key, bool fallback)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  return j ? ccord_json_bool(j, fallback) : fallback;
}

static bool expr_parse_uploader(ccord_arena *a, const ccord_json *obj, ccord_id *out_uid, const char **out_uname)
{
  *out_uid = CCORD_ID_NONE;
  *out_uname = "";
  const ccord_json *uj = obj ? ccord_json_get(obj, "user") : nullptr;
  if (uj && ccord_json_type_of(uj) == CCORD_JSON_OBJECT) {
    (void)json_id_key(uj, "id", out_uid);
    const char *gn = jstr(uj, "global_name");
    const char *un = jstr(uj, "username");
    const char *pick = text_ok(gn, CCORD_NAME_MAX_BYTES) ? gn : (text_ok(un, CCORD_NAME_MAX_BYTES) ? un : nullptr);
    if (pick) {
      char *dup = ccord_arena_strndup(a, pick, strlen(pick));
      if (!dup) return false;
      *out_uname = dup;
    }
  }
  if (*out_uid == CCORD_ID_NONE && obj) (void)json_id_key(obj, "user_id", out_uid);
  return true;
}

static bool map_one_emoji(ccord_arena *a, const ccord_json *ej, ccord_id fallback_gid, ccord_emoji *out, bool *oom)
{
  if (oom) *oom = false;
  ccord_id eid = CCORD_ID_NONE;
  if (!ej || ccord_json_type_of(ej) != CCORD_JSON_OBJECT) return false;
  if (!json_id_key(ej, "id", &eid) || eid == CCORD_ID_NONE) return false; // unicode emoji in a payload: no id
  const char *name = jstr(ej, "name");
  if (!text_ok(name, CCORD_NAME_MAX_BYTES)) return false;
  ccord_id gid = fallback_gid;
  (void)json_id_key(ej, "guild_id", &gid);
  ccord_id uid = CCORD_ID_NONE;
  const char *uname = "";
  if (!expr_parse_uploader(a, ej, &uid, &uname)) {
    if (oom) *oom = true;
    return false;
  }
  char *copy = ccord_arena_strndup(a, name, strlen(name));
  if (!copy) {
    if (oom) *oom = true;
    return false;
  }
  *out = (ccord_emoji){
    .id = eid,
    .guild_id = gid,
    .name = copy,
    .animated = json_bool_key(ej, "animated", false),
    .available = json_bool_key(ej, "available", true),
    .user_id = uid,
    .user_name = uname,
  };
  return true;
}

static bool map_one_sticker(ccord_arena *a, const ccord_json *sj, ccord_id fallback_gid, ccord_sticker *out, bool *oom)
{
  if (oom) *oom = false;
  ccord_id sid = CCORD_ID_NONE;
  if (!sj || ccord_json_type_of(sj) != CCORD_JSON_OBJECT) return false;
  if (!json_id_key(sj, "id", &sid) || sid == CCORD_ID_NONE) return false;
  const char *name = jstr(sj, "name");
  if (!text_ok(name, CCORD_NAME_MAX_BYTES)) return false;
  int64_t fmt = json_i64(sj, "format_type", 1);
  if (fmt < 1 || fmt > 4) fmt = 1;
  ccord_id gid = fallback_gid;
  (void)json_id_key(sj, "guild_id", &gid);
  ccord_id pack = CCORD_ID_NONE;
  (void)json_id_key(sj, "pack_id", &pack);
  ccord_id uid = CCORD_ID_NONE;
  const char *uname = "";
  if (!expr_parse_uploader(a, sj, &uid, &uname)) {
    if (oom) *oom = true;
    return false;
  }
  char *ncopy = ccord_arena_strndup(a, name, strlen(name));
  if (!ncopy) {
    if (oom) *oom = true;
    return false;
  }
  const char *draw = opt_text(jstr(sj, "description"), 256);
  const char *traw = opt_text(jstr(sj, "tags"), 200);
  const char *dcopy = draw[0] ? ccord_arena_strndup(a, draw, strlen(draw)) : "";
  const char *tcopy = traw[0] ? ccord_arena_strndup(a, traw, strlen(traw)) : "";
  if (!dcopy || !tcopy) {
    if (oom) *oom = true;
    return false;
  }
  *out = (ccord_sticker){
    .id = sid,
    .guild_id = gid,
    .pack_id = pack,
    .name = ncopy,
    .description = dcopy,
    .tags = tcopy,
    .format = (ccord_sticker_format)fmt,
    .available = json_bool_key(sj, "available", true),
    .user_id = uid,
    .user_name = uname,
  };
  return true;
}

static bool map_one_soundboard(ccord_arena *a, const ccord_json *sbj, ccord_id fallback_gid,
                               ccord_soundboard_sound *out, bool *oom)
{
  if (oom) *oom = false;
  ccord_id sid = CCORD_ID_NONE;
  if (!sbj || ccord_json_type_of(sbj) != CCORD_JSON_OBJECT) return false;
  if ((!json_id_key(sbj, "sound_id", &sid) && !json_id_key(sbj, "id", &sid)) || sid == CCORD_ID_NONE) return false;
  const char *name = jstr(sbj, "name");
  if (!text_ok(name, CCORD_NAME_MAX_BYTES)) return false;
  ccord_id gid = fallback_gid;
  (void)json_id_key(sbj, "guild_id", &gid);
  const ccord_json *vj = ccord_json_get(sbj, "volume");
  double vol = (vj && ccord_json_type_of(vj) == CCORD_JSON_NUMBER) ? ccord_json_number(vj, 1.0) : 1.0;
  if (!(vol >= 0.0)) vol = 0.0;
  if (vol > 1.0) vol = 1.0;
  uint8_t vol_pct = (uint8_t)(vol * 100.0 + 0.5);
  ccord_id em_id = CCORD_ID_NONE;
  (void)json_id_key(sbj, "emoji_id", &em_id);
  const char *em_raw = opt_text(jstr(sbj, "emoji_name"), CCORD_NAME_MAX_BYTES);
  const char *em_name = em_raw[0] ? ccord_arena_strndup(a, em_raw, strlen(em_raw)) : "";
  if (!em_name) {
    if (oom) *oom = true;
    return false;
  }
  ccord_id uid = CCORD_ID_NONE;
  const char *uname = "";
  if (!expr_parse_uploader(a, sbj, &uid, &uname)) {
    if (oom) *oom = true;
    return false;
  }
  char *ncopy = ccord_arena_strndup(a, name, strlen(name));
  if (!ncopy) {
    if (oom) *oom = true;
    return false;
  }
  *out = (ccord_soundboard_sound){
    .id = sid,
    .guild_id = gid,
    .name = ncopy,
    .volume_pct = vol_pct,
    .emoji_id = em_id,
    .emoji_name = em_name,
    .user_id = uid,
    .user_name = uname,
    .available = json_bool_key(sbj, "available", true),
  };
  return true;
}

// Appends the emojis of one guild-like object (or a bare array via `arr`) to a growable list.
static bool expr_append_emojis(ccord_arena *a, const ccord_json *arr, ccord_id gid, ccord_emoji **list, size_t *n,
                               size_t *cap)
{
  size_t cnt = arr ? ccord_json_array_len(arr) : 0;
  if (cnt > CCORD_MAX_EMOJIS_PER_GUILD) cnt = CCORD_MAX_EMOJIS_PER_GUILD;
  for (size_t i = 0; i < cnt; i++) {
    ccord_emoji e;
    bool oom = false;
    if (!map_one_emoji(a, ccord_json_array_at(arr, i), gid, &e, &oom)) {
      if (oom) return false;
      continue;
    }
    e.guild_id = gid;
    if (*n == *cap) {
      size_t ncap = *cap ? *cap * 2 : 32;
      ccord_emoji *g = ccord_realloc_array(*list, ncap, sizeof **list);
      if (!g) return false;
      *list = g;
      *cap = ncap;
    }
    (*list)[(*n)++] = e;
  }
  return true;
}

static bool expr_append_stickers(ccord_arena *a, const ccord_json *arr, ccord_id gid, ccord_sticker **list, size_t *n,
                                 size_t *cap)
{
  size_t cnt = arr ? ccord_json_array_len(arr) : 0;
  if (cnt > CCORD_MAX_STICKERS_PER_GUILD) cnt = CCORD_MAX_STICKERS_PER_GUILD;
  for (size_t i = 0; i < cnt; i++) {
    ccord_sticker st;
    bool oom = false;
    if (!map_one_sticker(a, ccord_json_array_at(arr, i), gid, &st, &oom)) {
      if (oom) return false;
      continue;
    }
    st.guild_id = gid;
    if (*n == *cap) {
      size_t ncap = *cap ? *cap * 2 : 16;
      ccord_sticker *g = ccord_realloc_array(*list, ncap, sizeof **list);
      if (!g) return false;
      *list = g;
      *cap = ncap;
    }
    (*list)[(*n)++] = st;
  }
  return true;
}

static bool expr_append_soundboard(ccord_arena *a, const ccord_json *arr, ccord_id gid, ccord_soundboard_sound **list,
                                   size_t *n, size_t *cap)
{
  size_t cnt = arr ? ccord_json_array_len(arr) : 0;
  if (cnt > CCORD_MAX_SOUNDBOARD_PER_GUILD) cnt = CCORD_MAX_SOUNDBOARD_PER_GUILD;
  for (size_t i = 0; i < cnt; i++) {
    ccord_soundboard_sound sb;
    bool oom = false;
    if (!map_one_soundboard(a, ccord_json_array_at(arr, i), gid, &sb, &oom)) {
      if (oom) return false;
      continue;
    }
    sb.guild_id = gid;
    if (*n == *cap) {
      size_t ncap = *cap ? *cap * 2 : 16;
      ccord_soundboard_sound *g = ccord_realloc_array(*list, ncap, sizeof **list);
      if (!g) return false;
      *list = g;
      *cap = ncap;
    }
    (*list)[(*n)++] = sb;
  }
  return true;
}

// Moves the heap lists into the event arena and publishes them on the net event.
static bool expr_publish(ccord_live_event *ev, ccord_emoji *el, size_t en, ccord_sticker *sl, size_t sn,
                         ccord_soundboard_sound *sbl, size_t sbn)
{
  bool ok = true;
  if (en > 0) {
    ccord_emoji *dst = ccord_arena_alloc(&ev->arena, en * sizeof *dst, _Alignof(ccord_emoji));
    if (dst) {
      memcpy(dst, el, en * sizeof *dst);
      ev->ev.emojis = dst;
      ev->ev.emoji_count = en;
    } else {
      ok = false;
    }
  }
  if (sn > 0) {
    ccord_sticker *dst = ccord_arena_alloc(&ev->arena, sn * sizeof *dst, _Alignof(ccord_sticker));
    if (dst) {
      memcpy(dst, sl, sn * sizeof *dst);
      ev->ev.stickers = dst;
      ev->ev.sticker_count = sn;
    } else {
      ok = false;
    }
  }
  if (sbn > 0) {
    ccord_soundboard_sound *dst =
      ccord_arena_alloc(&ev->arena, sbn * sizeof *dst, _Alignof(ccord_soundboard_sound));
    if (dst) {
      memcpy(dst, sbl, sbn * sizeof *dst);
      ev->ev.soundboard = dst;
      ev->ev.soundboard_count = sbn;
    } else {
      ok = false;
    }
  }
  ccord_free(el);
  ccord_free(sl);
  ccord_free(sbl);
  return ok;
}

// READY.guilds[] or a single GUILD_CREATE/UPDATE object.
static void expr_collect_guild(ccord_live_event *ev, const ccord_json *const *guilds, size_t ng)
{
  ccord_emoji *el = nullptr;
  ccord_sticker *sl = nullptr;
  ccord_soundboard_sound *sbl = nullptr;
  size_t en = 0, ecap = 0, sn = 0, scap = 0, sbn = 0, sbcap = 0;
  bool ok = true;
  for (size_t i = 0; ok && i < ng; i++) {
    const ccord_json *props = guild_props(guilds[i]);
    ccord_id gid = CCORD_ID_NONE;
    if (!guild_id_key_p(guilds[i], props, "id", &gid)) continue;
    const ccord_json *ea = guild_arr_p(guilds[i], props, "emojis");
    const ccord_json *sa = guild_arr_p(guilds[i], props, "stickers");
    const ccord_json *sba = guild_arr_p(guilds[i], props, "soundboard_sounds");
    if (ng == 1) {
      ev->ev.replace_emojis = ea != nullptr;
      ev->ev.replace_stickers = sa != nullptr;
      ev->ev.replace_soundboard = sba != nullptr;
    }
    ok = expr_append_emojis(&ev->arena, ea, gid, &el, &en, &ecap) &&
         expr_append_stickers(&ev->arena, sa, gid, &sl, &sn, &scap) &&
         expr_append_soundboard(&ev->arena, sba, gid, &sbl, &sbn, &sbcap);
  }
  (void)expr_publish(ev, el, en, sl, sn, sbl, sbn);
}

static bool map_guild_expressions_update(ccord_live_event *ev, const ccord_json *d, int kind)
{
  // kind: 0 = emojis, 1 = stickers, 2 = soundboard_sounds
  ccord_id gid = CCORD_ID_NONE;
  if (!json_id_key(d, "guild_id", &gid)) return false;
  ccord_emoji *el = nullptr;
  ccord_sticker *sl = nullptr;
  ccord_soundboard_sound *sbl = nullptr;
  size_t en = 0, ecap = 0, sn = 0, scap = 0, sbn = 0, sbcap = 0;
  bool ok = false;
  if (kind == 0) {
    ok = expr_append_emojis(&ev->arena, ccord_json_get(d, "emojis"), gid, &el, &en, &ecap);
  } else if (kind == 1) {
    ok = expr_append_stickers(&ev->arena, ccord_json_get(d, "stickers"), gid, &sl, &sn, &scap);
  } else {
    ok = expr_append_soundboard(&ev->arena, ccord_json_get(d, "soundboard_sounds"), gid, &sbl, &sbn, &sbcap);
  }
  if (ok) ok = expr_publish(ev, el, en, sl, sn, sbl, sbn);
  else {
    ccord_free(el);
    ccord_free(sl);
    ccord_free(sbl);
  }
  if (!ok) return false;
  ev->ev.type = CCORD_NET_GUILD_EXPRESSIONS;
  ev->ev.guild_id = gid;
  ev->ev.replace_emojis = (kind == 0);
  ev->ev.replace_stickers = (kind == 1);
  ev->ev.replace_soundboard = (kind == 2);
  return true;
}

static bool map_soundboard_upsert_dispatch(ccord_live_event *ev, const ccord_json *d)
{
  ccord_soundboard_sound *sb =
    ccord_arena_alloc(&ev->arena, sizeof *sb, _Alignof(ccord_soundboard_sound));
  if (!sb) return false;
  bool oom = false;
  if (!map_one_soundboard(&ev->arena, d, CCORD_ID_NONE, sb, &oom)) return false;
  if (sb->guild_id == CCORD_ID_NONE || sb->guild_id == CCORD_DM_GUILD_ID) return false;
  ev->ev.type = CCORD_NET_SOUNDBOARD_UPSERT;
  ev->ev.guild_id = sb->guild_id;
  ev->ev.soundboard = sb;
  ev->ev.soundboard_count = 1;
  return true;
}

static bool map_soundboard_delete_dispatch(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE, sid = CCORD_ID_NONE;
  if (!json_id_key(d, "guild_id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) return false;
  if ((!json_id_key(d, "sound_id", &sid) && !json_id_key(d, "id", &sid)) || sid == CCORD_ID_NONE) return false;
  ev->ev.type = CCORD_NET_SOUNDBOARD_DELETE;
  ev->ev.guild_id = gid;
  ev->ev.expr_id = sid;
  return true;
}

static bool decode_automod_str_list(ccord_arena *a, const ccord_json *tm, const char *key, size_t max_count,
                                    size_t max_bytes, const char *const **dst_out, size_t *dst_count, bool *oom)
{
  *dst_out = nullptr;
  *dst_count = 0;
  if (!tm) return true;
  const ccord_json *arr = ccord_json_get(tm, key);
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) return true;
  size_t n = ccord_json_array_len(arr);
  if (n > max_count) n = max_count;
  if (n == 0) return true;
  const char **items = ccord_arena_alloc(a, n * sizeof *items, _Alignof(const char *));
  if (!items) {
    if (oom) *oom = true;
    return false;
  }
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *el = ccord_json_array_at(arr, i);
    size_t slen = 0;
    const char *raw = el ? ccord_json_string(el, &slen) : nullptr;
    if (!raw) continue;
    union { const char *c; char *m; } u = {.c = raw};
    const char *clean = sanitize_str_inplace(u.m);
    size_t clen = strlen(clean);
    if (clen == 0) continue;
    size_t tlen = utf8_trunc_len(clean, clen, max_bytes);
    char *cp = ccord_arena_strndup(a, clean, tlen);
    if (!cp) {
      if (oom) *oom = true;
      return false;
    }
    items[k++] = cp;
  }
  *dst_out = k > 0 ? items : nullptr;
  *dst_count = k;
  return true;
}

static bool map_one_automod_rule(ccord_arena *a, const ccord_json *obj, ccord_id guild_hint, ccord_automod_rule *out,
                                 bool *oom)
{
  if (!obj || ccord_json_type_of(obj) != CCORD_JSON_OBJECT) return false;
  *out = (ccord_automod_rule){0};
  if (!json_id_key(obj, "id", &out->id) || out->id == CCORD_ID_NONE) return false;
  if (!json_id_key(obj, "guild_id", &out->guild_id) || out->guild_id == CCORD_ID_NONE) out->guild_id = guild_hint;
  if (out->guild_id == CCORD_ID_NONE || out->guild_id == CCORD_DM_GUILD_ID) return false;
  (void)json_id_key(obj, "creator_id", &out->creator_id);

  const char *nm = jstr(obj, "name");
  if (!nm) nm = "";
  size_t nlen = utf8_trunc_len(nm, strlen(nm), CCORD_NAME_MAX_BYTES);
  out->name = ccord_arena_strndup(a, nm, nlen);
  if (!out->name) {
    if (oom) *oom = true;
    return false;
  }

  int64_t et = json_i64(obj, "event_type", CCORD_AUTOMOD_EVENT_MESSAGE_SEND);
  out->event_type = (et >= 1 && et <= 2) ? (uint8_t)et : (uint8_t)CCORD_AUTOMOD_EVENT_MESSAGE_SEND;
  int64_t tt = json_i64(obj, "trigger_type", 0);
  if (tt < 1 || tt > 6) return false;
  out->trigger_type = (uint8_t)tt;
  out->enabled = json_bool_key(obj, "enabled", true);

  const ccord_json *tm = ccord_json_get(obj, "trigger_metadata");
  if (tm && ccord_json_type_of(tm) == CCORD_JSON_OBJECT) {
    if (!decode_automod_str_list(a, tm, "keyword_filter", CCORD_AUTOMOD_MAX_KEYWORDS, CCORD_AUTOMOD_KEYWORD_MAX_BYTES,
                                 &out->keywords, &out->keyword_count, oom)) {
      return false;
    }
    if (!decode_automod_str_list(a, tm, "regex_patterns", CCORD_AUTOMOD_MAX_REGEX, CCORD_AUTOMOD_REGEX_MAX_BYTES,
                                 &out->regex_patterns, &out->regex_count, oom)) {
      return false;
    }
    if (!decode_automod_str_list(a, tm, "allow_list", CCORD_AUTOMOD_MAX_ALLOW_LIST, CCORD_AUTOMOD_KEYWORD_MAX_BYTES,
                                 &out->allow_list, &out->allow_count, oom)) {
      return false;
    }
    const ccord_json *pa = ccord_json_get(tm, "presets");
    if (pa && ccord_json_type_of(pa) == CCORD_JSON_ARRAY) {
      size_t pn = ccord_json_array_len(pa);
      for (size_t i = 0; i < pn; i++) {
        int64_t pv = 0;
        if (!ccord_json_int64(ccord_json_array_at(pa, i), &pv)) continue;
        if (pv == 1) out->presets |= CCORD_AUTOMOD_PRESET_PROFANITY;
        else if (pv == 2) out->presets |= CCORD_AUTOMOD_PRESET_SEXUAL_CONTENT;
        else if (pv == 3) out->presets |= CCORD_AUTOMOD_PRESET_SLURS;
      }
    }
    int64_t ml = json_i64(tm, "mention_total_limit", 0);
    if (ml >= 1 && ml <= (int64_t)CCORD_AUTOMOD_MENTION_LIMIT_MAX) out->mention_total_limit = (uint8_t)ml;
    out->mention_raid_protection = json_bool_key(tm, "mention_raid_protection_enabled", false);
  }

  const char *cmsg = "";
  const ccord_json *acts = ccord_json_get(obj, "actions");
  if (acts && ccord_json_type_of(acts) == CCORD_JSON_ARRAY) {
    size_t an = ccord_json_array_len(acts);
    for (size_t i = 0; i < an; i++) {
      const ccord_json *aj = ccord_json_array_at(acts, i);
      int64_t at = json_i64(aj, "type", 0);
      const ccord_json *md = aj ? ccord_json_get(aj, "metadata") : nullptr;
      if (at == CCORD_AUTOMOD_ACTION_BLOCK_MESSAGE) {
        out->action_block = true;
        const char *cm = md ? jstr(md, "custom_message") : nullptr;
        if (cm && cm[0] != '\0') cmsg = cm;
      } else if (at == CCORD_AUTOMOD_ACTION_SEND_ALERT_MESSAGE) {
        out->action_alert = true;
        if (md) (void)json_id_key(md, "channel_id", &out->alert_channel_id);
      } else if (at == CCORD_AUTOMOD_ACTION_TIMEOUT) {
        out->action_timeout = true;
        int64_t ds = md ? json_i64(md, "duration_seconds", 0) : 0;
        if (ds > 0 && ds <= (int64_t)CCORD_AUTOMOD_TIMEOUT_MAX_S) out->timeout_seconds = (uint32_t)ds;
      } else if (at == CCORD_AUTOMOD_ACTION_BLOCK_MEMBER_INTERACTION) {
        out->action_block_interaction = true;
      }
    }
  }
  size_t cmlen = utf8_trunc_len(cmsg, strlen(cmsg), CCORD_AUTOMOD_CUSTOM_MSG_MAX_BYTES);
  out->custom_message = ccord_arena_strndup(a, cmsg, cmlen);
  if (!out->custom_message) {
    if (oom) *oom = true;
    return false;
  }

  const ccord_json *er = ccord_json_get(obj, "exempt_roles");
  if (er && ccord_json_type_of(er) == CCORD_JSON_ARRAY) {
    size_t rn = ccord_json_array_len(er);
    for (size_t i = 0; i < rn && out->exempt_role_count < CCORD_AUTOMOD_MAX_EXEMPT_ROLES; i++) {
      ccord_id rid = CCORD_ID_NONE;
      if (json_id(ccord_json_array_at(er, i), &rid) && rid != CCORD_ID_NONE) {
        out->exempt_role_ids[out->exempt_role_count++] = rid;
      }
    }
  }
  const ccord_json *ec = ccord_json_get(obj, "exempt_channels");
  if (ec && ccord_json_type_of(ec) == CCORD_JSON_ARRAY) {
    size_t cn = ccord_json_array_len(ec);
    for (size_t i = 0; i < cn && out->exempt_channel_count < CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS; i++) {
      ccord_id cid = CCORD_ID_NONE;
      if (json_id(ccord_json_array_at(ec, i), &cid) && cid != CCORD_ID_NONE) {
        out->exempt_channel_ids[out->exempt_channel_count++] = cid;
      }
    }
  }
  return true;
}

static bool map_automod_upsert_dispatch(ccord_live_event *ev, const ccord_json *d)
{
  ccord_automod_rule *r = ccord_arena_alloc(&ev->arena, sizeof *r, _Alignof(ccord_automod_rule));
  if (!r) return false;
  bool oom = false;
  if (!map_one_automod_rule(&ev->arena, d, CCORD_ID_NONE, r, &oom)) return false;
  ev->automod_rules = r;
  ev->automod_rule_count = 1;
  ev->ev.type = CCORD_NET_AUTOMOD_UPSERT;
  ev->ev.guild_id = r->guild_id;
  ev->ev.automod_rules = r;
  ev->ev.automod_rule_count = 1;
  return true;
}

static bool map_automod_delete_dispatch(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE, rid = CCORD_ID_NONE;
  if (!json_id_key(d, "guild_id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) return false;
  if (!json_id_key(d, "id", &rid) || rid == CCORD_ID_NONE) return false;
  ev->ev.type = CCORD_NET_AUTOMOD_DELETE;
  ev->ev.guild_id = gid;
  ev->ev.expr_id = rid;
  return true;
}

static bool map_webhooks_update_dispatch(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE, cid = CCORD_ID_NONE;
  if (!json_id_key(d, "guild_id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) return false;
  (void)json_id_key(d, "channel_id", &cid);
  ev->ev.type = CCORD_NET_WEBHOOKS_UPDATE;
  ev->ev.guild_id = gid;
  ev->ev.channel_id = cid;
  return true;
}

static bool map_guild_upsert_ex(ccord_live_event *ev, const ccord_json *d, ccord_id self_id)
{
  ccord_id gid = CCORD_ID_NONE;
  if (!json_id_key(d, "id", &gid)) return false;
  snapshot_builder b = {.arena = &ev->arena, .self_id = self_id};
  bool ok = map_guild(&b, d);
  if (ok) ok = sb_publish(&b, ev);
  sb_free(&b);
  if (!ok) return false;
  ev->guild_op = CCORD_LIVE_GUILD_UPSERT;
  ev->ev.guild_id = gid;
  ev->ev.voice_states = ev->voice_states;
  ev->ev.voice_state_count = ev->voice_state_count;
  ev->ev.replace_voice_states = (guild_arr(d, "voice_states") != nullptr);
  expr_collect_guild(ev, &d, 1);
  perm_collect_guilds(ev, &d, nullptr, 1, self_id);
  return true;
}

static bool map_guild_upsert(ccord_live_event *ev, const ccord_json *d)
{
  return map_guild_upsert_ex(ev, d, CCORD_ID_NONE);
}

static bool map_guild_delete(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  if (!json_id_key(d, "id", &gid)) return false;
  const ccord_json *unavailable = ccord_json_get(d, "unavailable");
  ev->ev.guild_id = gid;
  // An unavailable guild is a temporary outage, not a membership removal.
  ev->guild_op = unavailable && ccord_json_bool(unavailable, false) ? CCORD_LIVE_GUILD_NONE
                                                                   : CCORD_LIVE_GUILD_DELETE;
  return true;
}

static bool map_guild_members(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  if (!json_id_key(d, "guild_id", &gid)) return false;
  snapshot_builder b = {.arena = &ev->arena};
  perm_lists pl = {0};
  const ccord_json *props = guild_props(d);
  const ccord_json *mem_arr = d ? ccord_json_get(d, "members") : nullptr;
  const ccord_json *roles_arr = guild_arr_p(d, props, "roles");
  role_table rt = {0};
  if (roles_arr) role_table_build(&rt, roles_arr);
  const role_table *rtp = rt.count > 0 ? &rt : nullptr;
  size_t nm = mem_arr ? ccord_json_array_len(mem_arr) : 0;
  bool ok = true;
  for (size_t i = 0; ok && i < nm && i < CCORD_MAX_MEMBERS_PER_GUILD; i++) {
    const ccord_json *mj = ccord_json_array_at(mem_arr, i);
    ok = map_member_with_pres(&b, rtp, nullptr, mj, gid);
    if (ok) ok = perm_add_member(&ev->arena, &pl, gid, mj);
  }
  role_table_free(&rt);
  if (ok) {
    const ccord_json *pres_arr = guild_arr_p(d, props, "presences");
    size_t np = pres_arr ? ccord_json_array_len(pres_arr) : 0;
    for (size_t i = 0; i < np; i++) {
      const ccord_json *pj = ccord_json_array_at(pres_arr, i);
      const ccord_json *pu = pj ? ccord_json_get(pj, "user") : nullptr;
      ccord_id pid = CCORD_ID_NONE;
      if (!json_id_key(pu, "id", &pid) && !json_id_key(pj, "user_id", &pid)) continue;
      size_t uidx = 0;
      if (ccord_idmap_find(&b.user_idx, pid, &uidx) && uidx < b.user_count) {
        b.users[uidx].presence = presence_from(jstr(pj, "status"));
        const char *stext = extract_activity_status(&ev->arena, pj);
        b.users[uidx].status_text = stext;
        parsed_presence_detail pd;
        extract_presence_detail(pj, &pd);
        apply_presence_detail_to_user(&b.users[uidx], &pd);
      }
    }
  }
  if (ok) ok = sb_publish(&b, ev);
  sb_free(&b);
  (void)perm_publish(ev, &pl);
  if (!ok) return false;
  ev->guild_op = CCORD_LIVE_GUILD_MEMBERS;
  ev->ev.type = CCORD_NET_GUILD_MEMBERS;
  ev->ev.guild_id = gid;
  ev->ev.guild_members = ev->members;
  ev->ev.guild_member_count = ev->member_count;
  ev->ev.users = ev->users;
  ev->ev.user_count = ev->user_count;
  return true;
}

// GUILD_MEMBER_ADD / GUILD_MEMBER_UPDATE: the payload is the member object itself plus guild_id. There is no role table
// in it, so the member's role is resolved from the roles cached when the guild was created.
static bool map_guild_member_event(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  if (!json_id_key(d, "guild_id", &gid)) return false;
  snapshot_builder b = {.arena = &ev->arena};
  perm_lists pl = {0};
  bool ok = map_member_with_pres(&b, nullptr, nullptr, d, gid);
  if (ok) ok = perm_add_member(&ev->arena, &pl, gid, d);
  if (ok) ok = sb_publish(&b, ev);
  sb_free(&b);
  (void)perm_publish(ev, &pl);
  if (!ok) return false;
  ev->guild_op = CCORD_LIVE_GUILD_MEMBERS;
  ev->ev.type = CCORD_NET_GUILD_MEMBERS;
  ev->ev.guild_id = gid;
  ev->ev.guild_members = ev->members;
  ev->ev.guild_member_count = ev->member_count;
  ev->ev.users = ev->users;
  ev->ev.user_count = ev->user_count;
  return true;
}

// GUILD_ROLE_CREATE / GUILD_ROLE_UPDATE: {guild_id, role:{...}}.
static bool map_role_upsert(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  const ccord_json *rj = ccord_json_get(d, "role");
  ccord_id rid = CCORD_ID_NONE;
  if (!json_id_key(d, "guild_id", &gid) || !rj || !json_id_key(rj, "id", &rid)) return false;
  perm_lists pl = {0};
  bool ok = perm_add_role(&ev->arena, &pl, rj, gid);
  if (!ok || pl.nroles != 1) {
    ccord_free(pl.roles);
    return false;
  }
  ok = perm_publish(ev, &pl);
  if (!ok || ev->ev.role_count != 1) return false;
  ev->ev.type = CCORD_NET_ROLE_UPSERT;
  ev->ev.guild_id = gid;
  ev->ev.role_id = rid;
  return true;
}

// GUILD_ROLE_DELETE: {guild_id, role_id}.
static bool map_role_delete(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE, rid = CCORD_ID_NONE;
  if (!json_id_key(d, "guild_id", &gid) || !json_id_key(d, "role_id", &rid)) return false;
  ev->ev.type = CCORD_NET_ROLE_DELETE;
  ev->ev.guild_id = gid;
  ev->ev.role_id = rid;
  return true;
}

// GUILD_MEMBER_REMOVE: {guild_id, user:{id}}. Sent when a member leaves, is kicked or is banned.
static bool map_guild_member_remove(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE, uid = CCORD_ID_NONE;
  if (!json_id_key(d, "guild_id", &gid) || !json_id_key(ccord_json_get(d, "user"), "id", &uid)) return false;
  ev->ev.type = CCORD_NET_MEMBER_REMOVE;
  ev->ev.guild_id = gid;
  ev->ev.user_id = uid;
  return true;
}

// What the server says about the whole member list: how many members it holds, how many are online, and the size of each
// group ("online", "offline" or a hoisted role's id). The list arrives a range at a time, so these are what tells the
// interface how much of it is still to come. Anything unreadable is simply not told.
static void member_list_sizes(ccord_live_event *ev, const ccord_json *d)
{
  int64_t v = 0;
  const ccord_json *mc = ccord_json_get(d, "member_count");
  if (mc && ccord_json_int64(mc, &v) && v > 0 && v <= UINT32_MAX) ev->ev.member_total = (uint32_t)v;
  const ccord_json *oc = ccord_json_get(d, "online_count");
  if (oc && ccord_json_int64(oc, &v) && v >= 0 && v <= UINT32_MAX) ev->ev.member_online = (uint32_t)v;
  const ccord_json *groups = d ? ccord_json_get(d, "groups") : nullptr;
  size_t n = groups ? ccord_json_array_len(groups) : 0;
  if (n == 0 || n > CCORD_MAX_ROLES_PER_GUILD + 2u) return;
  ccord_member_group *out = ccord_arena_alloc(&ev->arena, n * sizeof *out, _Alignof(ccord_member_group));
  if (!out) return;
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *gj = ccord_json_array_at(groups, i);
    const char *id = gj ? jstr(gj, "id") : nullptr;
    const ccord_json *cj = gj ? ccord_json_get(gj, "count") : nullptr;
    if (!id || !cj || !ccord_json_int64(cj, &v) || v < 0 || v > UINT32_MAX) continue;
    ccord_member_group g = {.count = (uint32_t)v};
    if (strcmp(id, "online") == 0) {
      g.kind = CCORD_MEMBER_GROUP_ONLINE;
    } else if (strcmp(id, "offline") == 0) {
      g.kind = CCORD_MEMBER_GROUP_OFFLINE;
    } else {
      ccord_id rid = CCORD_ID_NONE;
      if (!json_id(ccord_json_get(gj, "id"), &rid)) continue;
      g.kind = CCORD_MEMBER_GROUP_ROLE;
      g.role_id = rid;
    }
    out[k++] = g;
  }
  if (k > 0) {
    ev->ev.member_groups = out;
    ev->ev.member_group_count = k;
  }
}

static bool map_guild_member_list_update(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id gid = CCORD_ID_NONE;
  if (!json_id_key(d, "guild_id", &gid)) return false;
  snapshot_builder b = {.arena = &ev->arena};
  perm_lists pl = {0};
  const ccord_json *roles = guild_roles(d);
  role_table rt = {0};
  if (roles) role_table_build(&rt, roles);
  const role_table *rtp = rt.count > 0 ? &rt : nullptr;
  const ccord_json *ops = d ? ccord_json_get(d, "ops") : nullptr;
  size_t nops = ops ? ccord_json_array_len(ops) : 0;
  bool ok = true;
  uint32_t synced_to = 0;
  for (size_t i = 0; ok && i < nops; i++) {
    const ccord_json *opj = ccord_json_array_at(ops, i);
    const char *op_name = jstr(opj, "op");
    if (!op_name) continue;
    if (strcmp(op_name, "SYNC") == 0) {
      // The range this answers (the last position of it): how far down the list the update reaches.
      int64_t first = 0, last = 0;
      const ccord_json *range = opj ? ccord_json_get(opj, "range") : nullptr;
      const bool two = range && ccord_json_array_len(range) == 2;
      const ccord_json *first_j = two ? ccord_json_array_at(range, 0) : nullptr;
      const ccord_json *last_j = two ? ccord_json_array_at(range, 1) : nullptr;
      if (first_j && last_j && ccord_json_int64(first_j, &first) && ccord_json_int64(last_j, &last) && first >= 0 &&
          last >= first && last < UINT32_MAX && (uint32_t)last + 1u > synced_to) {
        synced_to = (uint32_t)last + 1u;
      }
    }
    if (strcmp(op_name, "SYNC") == 0 || strcmp(op_name, "INSERT") == 0 || strcmp(op_name, "UPDATE") == 0) {
      const ccord_json *single = ccord_json_get(opj, "item");
      const ccord_json *smem = single ? ccord_json_get(single, "member") : nullptr;
      if (smem) {
        ok = map_member_with_pres(&b, rtp, nullptr, smem, gid) && perm_add_member(&ev->arena, &pl, gid, smem);
      }
      const ccord_json *items_arr = opj ? ccord_json_get(opj, "items") : nullptr;
      size_t nitems = items_arr ? ccord_json_array_len(items_arr) : 0;
      for (size_t k = 0; ok && k < nitems; k++) {
        const ccord_json *it = ccord_json_array_at(items_arr, k);
        const ccord_json *mj = it ? ccord_json_get(it, "member") : nullptr;
        if (mj) {
          ok = map_member_with_pres(&b, rtp, nullptr, mj, gid) && perm_add_member(&ev->arena, &pl, gid, mj);
        }
      }
    }
  }
  role_table_free(&rt);
  if (ok) ok = sb_publish(&b, ev);
  sb_free(&b);
  (void)perm_publish(ev, &pl);
  if (!ok) return false;
  ev->guild_op = CCORD_LIVE_GUILD_MEMBERS;
  ev->ev.type = CCORD_NET_GUILD_MEMBERS;
  ev->ev.guild_id = gid;
  ev->ev.guild_members = ev->members;
  ev->ev.guild_member_count = ev->member_count;
  ev->ev.users = ev->users;
  ev->ev.user_count = ev->user_count;
  member_list_sizes(ev, d);
  ev->ev.member_synced_to = synced_to;
  return true;
}

static bool map_voice_update(ccord_live_event *ev, const ccord_json *d)
{
  ccord_voice_state *v = ccord_arena_alloc(&ev->arena, sizeof *v, _Alignof(ccord_voice_state));
  if (!v) return false;
  if (!map_voice_state(d, CCORD_ID_NONE, v)) return false;
  // A voice state without a guild belongs to a DM / group call: file it under the synthetic DM guild.
  if (v->guild_id == CCORD_ID_NONE) v->guild_id = CCORD_DM_GUILD_ID;
  const ccord_json *vmem = ccord_json_get(d, "member");
  const ccord_json *vu = vmem ? ccord_json_get(vmem, "user") : ccord_json_get(d, "user");
  if (vu) {
    ccord_user u;
    if (map_user_obj(&ev->arena, vu, CCORD_PRESENCE_ONLINE, &u)) {
      u.id = v->user_id;
      ccord_user *slot = ccord_arena_alloc(&ev->arena, sizeof *slot, _Alignof(ccord_user));
      if (slot) {
        *slot = u;
        ev->users = slot;
        ev->user_count = 1;
        ev->ev.users = slot;
        ev->ev.user_count = 1;
      }
    }
  }
  ev->voice_states = v;
  ev->voice_state_count = 1;
  ev->ev.type = CCORD_NET_VOICE_STATE_UPDATE;
  ev->ev.guild_id = v->guild_id;
  ev->ev.channel_id = v->channel_id;
  ev->ev.user_id = v->user_id;
  ev->ev.voice_state = v;
  const char *session = jstr(d, "session_id");
  if (session) {
    char *copy = ccord_arena_strndup(&ev->arena, session, strlen(session));
    if (!copy) return false;
    ev->voice_session_id = copy;
  }
  return true;
}

static bool publish_voice_states_event(snapshot_builder *b, ccord_live_event *ev, ccord_id guild_id,
                                       bool replace_voice_states)
{
  bool ok = sb_publish(b, ev);
  sb_free(b);
  if (!ok) return false;
  if (replace_voice_states || ev->voice_state_count > 0 || ev->user_count > 0) {
    ev->ev.type = CCORD_NET_VOICE_STATES;
    ev->ev.guild_id = guild_id;
    ev->ev.voice_states = ev->voice_states;
    ev->ev.voice_state_count = ev->voice_state_count;
    ev->ev.replace_voice_states = replace_voice_states;
    ev->ev.users = ev->users;
    ev->ev.user_count = ev->user_count;
  }
  return true;
}

static bool map_ready_supplemental(ccord_live_event *ev, const ccord_json *d)
{
  if (!is_json_object(d)) return false;
  snapshot_builder b = {.arena = &ev->arena};
  const ccord_json *users_arr = ccord_json_get(d, "users");
  size_t nu = users_arr ? ccord_json_array_len(users_arr) : 0;
  for (size_t i = 0; i < nu; i++) {
    ccord_user u;
    if (map_user_obj(&ev->arena, ccord_json_array_at(users_arr, i), CCORD_PRESENCE_OFFLINE, &u)) {
      if (!sb_user(&b, &u)) {
        sb_free(&b);
        return false;
      }
    }
  }
  if (!collect_member_users_from_array(&b, ccord_json_get(d, "merged_members"), 0)) {
    sb_free(&b);
    return false;
  }
  const ccord_json *guilds_arr = ccord_json_get(d, "guilds");
  size_t ng = guilds_arr ? ccord_json_array_len(guilds_arr) : 0;
  for (size_t gi = 0; gi < ng && gi < CCORD_MAX_GUILDS; gi++) {
    const ccord_json *gj = ccord_json_array_at(guilds_arr, gi);
    ccord_id gid = CCORD_ID_NONE;
    if (!gj || !json_id_key(gj, "id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) continue;
    const ccord_json *vs_arr = ccord_json_get(gj, "voice_states");
    size_t nv = vs_arr && is_json_array(vs_arr) ? ccord_json_array_len(vs_arr) : 0;
    for (size_t k = 0; k < nv && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
      if (!collect_voice_state_entry(&b, ccord_json_array_at(vs_arr, k), gid, false, false, false)) {
        sb_free(&b);
        return false;
      }
    }
  }
  const ccord_json *top_vs_arr = ccord_json_get(d, "voice_states");
  size_t top_nv = top_vs_arr ? ccord_json_array_len(top_vs_arr) : 0;
  for (size_t k = 0; k < top_nv && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
    if (!collect_voice_state_entry(&b, ccord_json_array_at(top_vs_arr, k), CCORD_ID_NONE, false, false, false)) {
      sb_free(&b);
      return false;
    }
  }
  return publish_voice_states_event(&b, ev, CCORD_ID_NONE, false);
}

static bool map_passive_update_v2(ccord_live_event *ev, const ccord_json *d)
{
  if (!is_json_object(d)) return false;
  ccord_id gid = CCORD_ID_NONE;
  if (!json_id_key(d, "guild_id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) return false;
  snapshot_builder b = {.arena = &ev->arena};
  if (!collect_member_users_from_array(&b, ccord_json_get(d, "updated_members"), 0)) {
    sb_free(&b);
    return false;
  }
  const ccord_json *uvs_arr = ccord_json_get(d, "updated_voice_states");
  size_t nv = uvs_arr ? ccord_json_array_len(uvs_arr) : 0;
  for (size_t k = 0; k < nv && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
    if (!collect_voice_state_entry(&b, ccord_json_array_at(uvs_arr, k), gid, true, false, false)) {
      sb_free(&b);
      return false;
    }
  }
  const ccord_json *rvs_arr = ccord_json_get(d, "removed_voice_states");
  size_t nr = rvs_arr ? ccord_json_array_len(rvs_arr) : 0;
  for (size_t k = 0; k < nr && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
    const ccord_json *rj = ccord_json_array_at(rvs_arr, k);
    ccord_id uid = CCORD_ID_NONE;
    if (!json_id(rj, &uid)) (void)json_id_key(rj, "user_id", &uid);
    if (uid == CCORD_ID_NONE) continue;
    ccord_voice_state removed = {.guild_id = gid, .channel_id = CCORD_ID_NONE, .user_id = uid};
    if (!sb_voice(&b, &removed)) {
      sb_free(&b);
      return false;
    }
  }
  return publish_voice_states_event(&b, ev, gid, false);
}

static bool map_passive_update_v1(ccord_live_event *ev, const ccord_json *d)
{
  if (!is_json_object(d)) return false;
  ccord_id gid = CCORD_ID_NONE;
  if (!json_id_key(d, "guild_id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) return false;
  snapshot_builder b = {.arena = &ev->arena};
  if (!collect_member_users_from_array(&b, ccord_json_get(d, "members"), 0)) {
    sb_free(&b);
    return false;
  }
  const ccord_json *vs_arr = ccord_json_get(d, "voice_states");
  bool has_vs = vs_arr && is_json_array(vs_arr);
  size_t nv = has_vs ? ccord_json_array_len(vs_arr) : 0;
  for (size_t k = 0; k < nv && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
    if (!collect_voice_state_entry(&b, ccord_json_array_at(vs_arr, k), gid, false, false, false)) {
      sb_free(&b);
      return false;
    }
  }
  return publish_voice_states_event(&b, ev, gid, has_vs);
}

static bool map_voice_server(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id guild = CCORD_ID_NONE, channel = CCORD_ID_NONE;
  const ccord_json *gj = ccord_json_get(d, "guild_id");
  if (!gj || ccord_json_type_of(gj) == CCORD_JSON_NULL) {
    // A DM / group-DM call VOICE_SERVER_UPDATE carries guild_id: null; file it under the synthetic DM guild.
    guild = CCORD_DM_GUILD_ID;
  } else if (!json_id(gj, &guild)) {
    return false;   // a non-null guild_id must still be a valid snowflake
  }
  (void)json_id_key(d, "channel_id", &channel);
  const char *token = jstr(d, "token");
  const char *endpoint = jstr(d, "endpoint");
  if (!token) token = "";
  if (!endpoint) endpoint = "";
  ccord_voice_server *srv = ccord_arena_alloc(&ev->arena, sizeof *srv, _Alignof(ccord_voice_server));
  if (!srv) return false;
  srv->guild_id = guild;
  srv->channel_id = channel;
  srv->token = ccord_arena_strndup(&ev->arena, token, strlen(token));
  srv->endpoint = ccord_arena_strndup(&ev->arena, endpoint, strlen(endpoint));
  if (!srv->token || !srv->endpoint) return false;
  ev->ev.type = CCORD_NET_VOICE_SERVER_UPDATE;
  ev->ev.guild_id = guild;
  ev->ev.channel_id = channel;
  ev->ev.voice_server = srv;
  return true;
}

static bool map_stream_signal(ccord_live_event *ev, const ccord_json *d, ccord_stream_signal_kind kind)
{
  const char *sk = jstr(d, "stream_key");
  if (!sk || sk[0] == '\0') return false;
  ccord_id gid = CCORD_ID_NONE, cid = CCORD_ID_NONE, uid = CCORD_ID_NONE;
  if (!ccord_stream_key_parse(sk, &gid, &cid, &uid)) {
    (void)json_id_key(d, "guild_id", &gid);
    (void)json_id_key(d, "channel_id", &cid);
    (void)json_id_key(d, "user_id", &uid);
  }
  ccord_stream_signal *sig = ccord_arena_alloc(&ev->arena, sizeof *sig, _Alignof(ccord_stream_signal));
  if (!sig) return false;
  memset(sig, 0, sizeof *sig);
  bool server_update = kind == CCORD_STREAM_SIG_SERVER_UPDATE;
  bool deleted = kind == CCORD_STREAM_SIG_DELETE;
  sig->kind = kind;
  sig->guild_id = gid;
  sig->channel_id = cid;
  sig->user_id = uid;
  sig->owner_id = uid;
  sig->is_call = gid == CCORD_DM_GUILD_ID;
  (void)json_id_key(d, "rtc_server_id", &sig->rtc_server_id);
  (void)json_id_key(d, "rtc_channel_id", &sig->rtc_channel_id);
  sig->stream_key = ccord_arena_strndup(&ev->arena, sk, strlen(sk));
  if (!sig->stream_key) return false;
  sig->server_update = server_update;
  sig->deleted = deleted;
  sig->paused = json_bool_key(d, "paused", false);
  sig->unavailable = json_bool_key(d, "unavailable", false);
  int64_t viewers = json_i64(d, "viewer_count", -1);
  const ccord_json *vids = ccord_json_get(d, "viewer_ids");
  if (vids && ccord_json_type_of(vids) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(vids);
    if (viewers < 0) sig->viewer_count = n > (size_t)UINT32_MAX ? UINT32_MAX : (uint32_t)n;
    size_t cap = n > 128u ? 128u : n;
    if (cap > 0) {
      ccord_id *arr = ccord_arena_alloc(&ev->arena, cap * sizeof *arr, _Alignof(ccord_id));
      if (arr) {
        size_t got = 0;
        for (size_t i = 0; i < cap; i++) {
          ccord_id vid = CCORD_ID_NONE;
          if (json_id(ccord_json_array_at(vids, i), &vid) && vid != CCORD_ID_NONE) arr[got++] = vid;
        }
        sig->viewer_ids = arr;
        sig->viewer_id_count = got;
      }
    }
  }
  if (viewers >= 0) {
    sig->viewer_count = viewers > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)viewers;
  }
  const char *reason = jstr(d, "reason");
  sig->reason = reason ? ccord_arena_strndup(&ev->arena, reason, strlen(reason)) : "";
  if (!sig->reason) sig->reason = "";
  if (server_update) {
    const char *token = jstr(d, "token");
    const char *endpoint = jstr(d, "endpoint");
    if (!token) token = "";
    if (!endpoint) endpoint = "";
    sig->token = ccord_arena_strndup(&ev->arena, token, strlen(token));
    sig->endpoint = ccord_arena_strndup(&ev->arena, endpoint, strlen(endpoint));
    if (!sig->token || !sig->endpoint) return false;
  }
  ev->ev.type = CCORD_NET_STREAM_SIGNAL;
  ev->ev.guild_id = gid;
  ev->ev.channel_id = cid;
  ev->ev.user_id = uid;
  ev->ev.stream_signal = sig;
  return true;
}

static void map_message_author(ccord_arena *a, const ccord_json *j, ccord_id author_id, ccord_user *out)
{
  const ccord_json *au = j ? ccord_json_get(j, "author") : nullptr;
  const ccord_json *mem = j ? ccord_json_get(j, "member") : nullptr;
  const char *name = mem ? jstr(mem, "nick") : nullptr;
  if (!name || name[0] == '\0') name = au ? jstr(au, "global_name") : nullptr;
  if (!name || name[0] == '\0') name = au ? jstr(au, "username") : nullptr;
  if (!name || name[0] == '\0') name = j ? jstr(j, "username") : nullptr;
  if (!text_ok(name, CCORD_NAME_MAX_BYTES)) name = "System";
  else {
    name = arena_dup(a, name);
    if (name[0] == '\0') name = "System";
  }
  const char *handle = au ? jstr(au, "username") : (j ? jstr(j, "username") : nullptr);
  const ccord_json *botj = au ? ccord_json_get(au, "bot") : nullptr;
  *out = (ccord_user){
    .id = author_id,
    .name = name,
    .handle = opt_text_dup(a, handle, CCORD_NAME_MAX_BYTES),
    .status_text = "",
    .avatar_url = au ? build_cdn_url(a, "avatars", author_id, jstr(au, "avatar")) : "",
    .bio = "",
    .pronouns = "",
    .presence = CCORD_PRESENCE_OFFLINE,
    .is_bot = (botj && ccord_json_bool(botj, false)) || (j && ccord_json_get(j, "webhook_id") != nullptr),
    .avatar_palette = (uint8_t)(author_id % 6u),
  };
  extract_clan_info(au, out->clan_tag, &out->clan_guild_id, out->clan_badge);
}

static void collect_message_mentions(ccord_arena *a, const ccord_json *j,
                                     ccord_user *users, size_t *ukept, size_t max_users)
{
  if (!j || !users || !ukept) return;
  const ccord_json *ments = ccord_json_get(j, "mentions");
  size_t nm = ments ? ccord_json_array_len(ments) : 0;
  for (size_t i = 0; i < nm && *ukept < max_users; i++) {
    const ccord_json *mu = ccord_json_array_at(ments, i);
    ccord_id uid = CCORD_ID_NONE;
    if (!mu || !json_id_key(mu, "id", &uid)) continue;
    bool seen = false;
    for (size_t u = 0; u < *ukept; u++) {
      if (users[u].id == uid) { seen = true; break; }
    }
    if (seen) continue;
    const ccord_json *mem = ccord_json_get(mu, "member");
    const char *name = mem ? jstr(mem, "nick") : nullptr;
    if (!name || name[0] == '\0') name = jstr(mu, "global_name");
    if (!name || name[0] == '\0') name = jstr(mu, "username");
    if (!text_ok(name, CCORD_NAME_MAX_BYTES)) continue;
    const char *name_dup = arena_dup(a, name);
    if (name_dup[0] == '\0') continue;
    const char *handle = jstr(mu, "username");
    const ccord_json *botj = ccord_json_get(mu, "bot");
    users[(*ukept)++] = (ccord_user){
      .id = uid,
      .name = name_dup,
      .handle = opt_text_dup(a, handle, CCORD_NAME_MAX_BYTES),
      .status_text = "",
      .avatar_url = build_cdn_url(a, "avatars", uid, jstr(mu, "avatar")),
      .bio = "",
      .pronouns = "",
      .presence = CCORD_PRESENCE_OFFLINE,
      .is_bot = botj && ccord_json_bool(botj, false),
      .avatar_palette = (uint8_t)(uid % 6u),
    };
  }
}

static void copy_json_str(char *dst, size_t cap, const ccord_json *obj, const char *key)
{
  if (!dst || cap == 0) return;
  dst[0] = '\0';
  const char *s = jstr(obj, key);
  if (!s || s[0] == '\0') return;
  size_t n = utf8_trunc_len(s, strlen(s), cap - 1u);
  memcpy(dst, s, n);
  dst[n] = '\0';
}

// ---- M5 voice & video dispatches ---------------------------------------------------------

// CALL_CREATE / CALL_UPDATE: a DM / group call. Ringing is passive (the server pushes it); active stays true until
// CALL_DELETE. CREATE also carries the participants' voice_states: they are decoded into the internal array and the
// gateway surfaces each as its own VOICE_STATE_UPDATE (the public event has a single voice_state slot).
#define LIVE_CALL_MAX_VOICE_STATES (CCORD_MAX_DM_RECIPIENTS + 2u)   // a group call plus a little headroom
static bool map_call_event(ccord_live_event *ev, const ccord_json *d, bool created)
{
  ccord_id cid = CCORD_ID_NONE;
  if (!json_id_key(d, "channel_id", &cid) || cid == CCORD_ID_NONE) return false;
  ccord_call_state *call = ccord_arena_alloc(&ev->arena, sizeof *call, _Alignof(ccord_call_state));
  if (!call) return false;
  memset(call, 0, sizeof *call);
  call->channel_id = cid;
  call->active = true;
  copy_json_str(call->region, sizeof call->region, d, "region");
  const ccord_json *ring = ccord_json_get(d, "ringing");
  size_t nr = is_json_array(ring) ? ccord_json_array_len(ring) : 0;
  for (size_t i = 0; i < nr && call->ringing_count < sizeof call->ringing / sizeof call->ringing[0]; i++) {
    ccord_id uid = CCORD_ID_NONE;
    if (json_id(ccord_json_array_at(ring, i), &uid) && uid != CCORD_ID_NONE) {
      call->ringing[call->ringing_count++] = uid;
    }
  }
  ev->ev.type = created ? CCORD_NET_CALL_CREATE : CCORD_NET_CALL_UPDATE;
  ev->ev.guild_id = CCORD_DM_GUILD_ID;
  ev->ev.channel_id = cid;
  ev->ev.call = call;
  if (created) {
    const ccord_json *vsarr = ccord_json_get(d, "voice_states");
    size_t nvs = is_json_array(vsarr) ? ccord_json_array_len(vsarr) : 0;
    if (nvs > LIVE_CALL_MAX_VOICE_STATES) nvs = LIVE_CALL_MAX_VOICE_STATES;
    if (nvs > 0) {
      ccord_voice_state *vs = ccord_arena_alloc(&ev->arena, nvs * sizeof *vs, _Alignof(ccord_voice_state));
      if (!vs) return false;
      size_t kept = 0;
      for (size_t i = 0; i < nvs; i++) {
        if (map_voice_state(ccord_json_array_at(vsarr, i), CCORD_DM_GUILD_ID, &vs[kept])) kept++;
      }
      ev->voice_states = vs;
      ev->voice_state_count = kept;
    }
  }
  return true;
}

// CALL_DELETE: the call ended; only the channel id is authoritative.
static bool map_call_delete(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id cid = CCORD_ID_NONE;
  if (!json_id_key(d, "channel_id", &cid) || cid == CCORD_ID_NONE) return false;
  ev->ev.type = CCORD_NET_CALL_DELETE;
  ev->ev.guild_id = CCORD_DM_GUILD_ID;
  ev->ev.channel_id = cid;
  return true;
}

// VOICE_CHANNEL_STATUS_UPDATE: the channel's voice status text (null / absent = cleared).
static bool map_voice_channel_status(ccord_live_event *ev, const ccord_json *d)
{
  ccord_id cid = CCORD_ID_NONE, gid = CCORD_ID_NONE;
  if (!json_id_key(d, "channel_id", &cid) || cid == CCORD_ID_NONE) return false;
  (void)json_id_key(d, "guild_id", &gid);
  const char *status = jstr(d, "status");
  const char *copy = "";
  if (status && status[0] != '\0') {
    copy = ccord_arena_strndup(&ev->arena, status, strlen(status));
    if (!copy) return false;
  }
  ev->ev.type = CCORD_NET_VOICE_STATUS_UPDATE;
  ev->ev.guild_id = gid;
  ev->ev.channel_id = cid;
  ev->ev.voice_status = copy;
  return true;
}

// One stage instance object: the STAGE_INSTANCE_* dispatch payload and the PATCH /stage-instances/{c} REST answer.
// The instance's own `id` is distinct from the channel it runs in; hints fill what the payload omits.
static bool map_stage_instance_obj(const ccord_json *d, ccord_id guild_hint, ccord_id channel_hint,
                                   ccord_stage_instance *out)
{
  ccord_id cid = channel_hint, gid = guild_hint;
  (void)json_id_key(d, "channel_id", &cid);
  (void)json_id_key(d, "guild_id", &gid);
  if (cid == CCORD_ID_NONE) return false;
  out->channel_id = cid;
  out->guild_id = gid;
  copy_json_str(out->topic, sizeof out->topic, d, "topic");
  out->id = CCORD_ID_NONE;
  (void)json_id_key(d, "id", &out->id);
  int64_t pl = json_i64(d, "privacy_level", 0);
  out->privacy_level = (pl > 0 && pl <= UINT8_MAX) ? (uint8_t)pl : 0u;
  const ccord_json *ddj = ccord_json_get(d, "discoverable_disabled");
  out->discoverable_disabled = ddj && ccord_json_bool(ddj, false);
  out->guild_scheduled_event_id = CCORD_ID_NONE;
  (void)json_id_key(d, "guild_scheduled_event_id", &out->guild_scheduled_event_id);
  return true;
}

static bool map_stage_instance_dispatch(ccord_live_event *ev, const ccord_json *d, bool deleted)
{
  ccord_stage_instance *si = ccord_arena_alloc(&ev->arena, sizeof *si, _Alignof(ccord_stage_instance));
  if (!si) return false;
  memset(si, 0, sizeof *si);
  if (!map_stage_instance_obj(d, CCORD_ID_NONE, CCORD_ID_NONE, si)) return false;
  ev->ev.type = CCORD_NET_STAGE_INSTANCE_UPDATE;
  ev->ev.guild_id = si->guild_id;
  ev->ev.channel_id = si->channel_id;
  ev->ev.stage_instance = si;
  ev->ev.stage_deleted = deleted;
  return true;
}

// One guild scheduled event object: the GUILD_SCHEDULED_EVENT_* dispatch payloads and the REST answers.
// `deleted` relaxes the name requirement: a DELETE may arrive with only the identifying fields.
static bool map_scheduled_event_obj(const ccord_json *d, ccord_id guild_hint, bool deleted,
                                    ccord_scheduled_event *out)
{
  ccord_id id = CCORD_ID_NONE, gid = guild_hint;
  if (!json_id_key(d, "id", &id) || id == CCORD_ID_NONE) return false;
  (void)json_id_key(d, "guild_id", &gid);
  if (gid == CCORD_ID_NONE) return false;
  memset(out, 0, sizeof *out);
  out->id = id;
  out->guild_id = gid;
  (void)json_id_key(d, "channel_id", &out->channel_id);   // null for EXTERNAL events
  (void)json_id_key(d, "creator_id", &out->creator_id);
  copy_json_str(out->name, sizeof out->name, d, "name");
  if (!deleted && out->name[0] == '\0') return false;
  copy_json_str(out->description, sizeof out->description, d, "description");
  const ccord_json *meta = ccord_json_get(d, "entity_metadata");
  if (is_json_object(meta)) copy_json_str(out->location, sizeof out->location, meta, "location");
  const char *start = jstr(d, "scheduled_start_time");
  int64_t start_ms = start ? iso_ms(start, strlen(start)) : -1;
  out->scheduled_start_ms = start_ms > 0 ? start_ms : 0;
  const char *end = jstr(d, "scheduled_end_time");
  int64_t end_ms = end ? iso_ms(end, strlen(end)) : -1;
  out->scheduled_end_ms = end_ms > 0 ? end_ms : 0;
  int64_t pl = json_i64(d, "privacy_level", 0);
  out->privacy_level = (pl > 0 && pl <= UINT8_MAX) ? (uint8_t)pl : 0u;
  int64_t st = json_i64(d, "status", 0);
  out->status = (st >= CCORD_SCHEDULED_EVENT_SCHEDULED && st <= CCORD_SCHEDULED_EVENT_CANCELED) ? (uint8_t)st : 0u;
  int64_t et = json_i64(d, "entity_type", 0);
  out->entity_type =
    (et >= CCORD_SCHEDULED_EVENT_STAGE_INSTANCE && et <= CCORD_SCHEDULED_EVENT_EXTERNAL) ? (uint8_t)et : 0u;
  out->user_count = clamp_u32(json_i64(d, "user_count", 0));
  return true;
}

static bool map_scheduled_event_dispatch(ccord_live_event *ev, const ccord_json *d, bool deleted)
{
  ccord_scheduled_event *se = ccord_arena_alloc(&ev->arena, sizeof *se, _Alignof(ccord_scheduled_event));
  if (!se) return false;
  if (!map_scheduled_event_obj(d, CCORD_ID_NONE, deleted, se)) return false;
  ev->ev.type = CCORD_NET_SCHEDULED_EVENT_UPDATE;
  ev->ev.guild_id = se->guild_id;
  ev->ev.scheduled_events = se;
  ev->ev.scheduled_event_count = 1;
  ev->ev.scheduled_event_id = se->id;
  ev->ev.scheduled_event_deleted = deleted;
  return true;
}

// GUILD_SCHEDULED_EVENT_USER_ADD / USER_REMOVE: {guild_scheduled_event_id, guild_id, user_id}.
static bool map_scheduled_event_user(ccord_live_event *ev, const ccord_json *d, bool added)
{
  ccord_id eid = CCORD_ID_NONE, gid = CCORD_ID_NONE, uid = CCORD_ID_NONE;
  if (!json_id_key(d, "guild_scheduled_event_id", &eid) || eid == CCORD_ID_NONE) return false;
  if (!json_id_key(d, "guild_id", &gid) || gid == CCORD_ID_NONE) return false;
  if (!json_id_key(d, "user_id", &uid) || uid == CCORD_ID_NONE) return false;
  ev->ev.type = CCORD_NET_SCHEDULED_EVENT_USER_CHANGE;
  ev->ev.guild_id = gid;
  ev->ev.scheduled_event_id = eid;
  ev->ev.user_id = uid;
  ev->ev.scheduled_event_user_added = added;
  return true;
}

// EMBEDDED_ACTIVITY_UPDATE_V2: one running activity instance in a voice channel. Empty or absent participants
// mean the instance ended (participant_count 0); the ids still travel so the store can drop exactly that instance.
// The shapes come from unofficial docs: location may be absent (top-level ids fill in) and its `id` may be a
// non-numeric kind, in which case the voice channel stands in as the leave/update path segment.
static bool map_activity_instance_obj(const ccord_json *d, ccord_activity_instance *out)
{
  memset(out, 0, sizeof *out);
  if (!json_id_key(d, "instance_id", &out->instance_id) || out->instance_id == CCORD_ID_NONE) return false;
  (void)json_id_key(d, "application_id", &out->application_id);
  const ccord_json *loc = ccord_json_get(d, "location");
  const ccord_json *ids = is_json_object(loc) ? loc : d;
  (void)json_id_key(ids, "channel_id", &out->channel_id);
  (void)json_id_key(ids, "guild_id", &out->guild_id);
  if (!json_id_key(loc, "id", &out->location_id) || out->location_id == CCORD_ID_NONE) {
    out->location_id = out->channel_id;
  }
  const ccord_json *parr = ccord_json_get(d, "participants");
  size_t pn = is_json_array(parr) ? ccord_json_array_len(parr) : 0;
  for (size_t i = 0; i < pn && out->participant_count < CCORD_ACTIVITY_MAX_PARTICIPANTS; i++) {
    const ccord_json *pj = ccord_json_array_at(parr, i);
    ccord_id uid = CCORD_ID_NONE;
    if (!json_id(pj, &uid) && is_json_object(pj)) {
      // Tolerate participant objects carrying the user id under either key.
      if (!json_id_key(pj, "user_id", &uid)) (void)json_id_key(pj, "id", &uid);
    }
    if (uid != CCORD_ID_NONE) out->participants[out->participant_count++] = uid;
  }
  return true;
}

static bool map_activity_instance_dispatch(ccord_live_event *ev, const ccord_json *d)
{
  if (!is_json_object(d)) return false;
  ccord_activity_instance *inst = &ev->ev.activity_instance;   // value copy in the public event
  if (!map_activity_instance_obj(d, inst)) return false;
  ev->ev.type = CCORD_NET_ACTIVITY_INSTANCE_UPDATE;
  ev->ev.guild_id = inst->guild_id;
  ev->ev.channel_id = inst->channel_id;
  return true;
}

static bool map_poll_vote(ccord_live_event *ev, const ccord_json *d, bool added)
{
  if (!is_json_object(d)) return false;
  ccord_id uid = CCORD_ID_NONE, cid = CCORD_ID_NONE, mid = CCORD_ID_NONE, gid = CCORD_ID_NONE;
  if (!json_id_key(d, "user_id", &uid) || !json_id_key(d, "channel_id", &cid) || !json_id_key(d, "message_id", &mid)) {
    return false;
  }
  int64_t aid = json_i64(d, "answer_id", 0);
  if (aid <= 0 || aid > (int64_t)UINT32_MAX) return false;
  (void)json_id_key(d, "guild_id", &gid);
  ev->ev.type = CCORD_NET_POLL_VOTE;
  ev->ev.user_id = uid;
  ev->ev.channel_id = cid;
  ev->ev.message_id = mid;
  ev->ev.guild_id = gid;
  ev->ev.poll_answer_id = (uint32_t)aid;
  ev->ev.poll_vote_added = added;
  return true;
}

static void add_modal_field(ccord_interaction_modal *m, const ccord_json *fj)
{
  if (!is_json_object(fj) || m->field_count >= CCORD_RICH_MAX_COMPONENT_ROWS) return;
  if (json_i64(fj, "type", 0) != CCORD_COMP_TEXT_INPUT) return;
  ccord_modal_field f = {0};
  copy_json_str(f.custom_id, sizeof f.custom_id, fj, "custom_id");
  if (f.custom_id[0] == '\0') return;
  copy_json_str(f.label, sizeof f.label, fj, "label");
  copy_json_str(f.placeholder, sizeof f.placeholder, fj, "placeholder");
  copy_json_str(f.value, sizeof f.value, fj, "value");
  int64_t st = json_i64(fj, "style", 1);
  f.style = st == 2 ? 2u : 1u;
  int64_t minl = json_i64(fj, "min_length", 0);
  int64_t maxl = json_i64(fj, "max_length", 0);
  f.min_length = minl < 0 ? 0u : (minl > (int64_t)CCORD_RICH_TEXT_MAX_BYTES ? (uint16_t)CCORD_RICH_TEXT_MAX_BYTES : (uint16_t)minl);
  f.max_length = maxl < 0 ? 0u : (maxl > (int64_t)CCORD_RICH_TEXT_MAX_BYTES ? (uint16_t)CCORD_RICH_TEXT_MAX_BYTES : (uint16_t)maxl);
  const ccord_json *reqj = ccord_json_get(fj, "required");
  f.required = reqj ? ccord_json_bool(reqj, true) : true;
  m->fields[m->field_count++] = f;
}

static bool map_interaction_modal(ccord_live_event *ev, const ccord_json *d)
{
  if (!is_json_object(d)) return false;
  ccord_interaction_modal *m = ccord_arena_alloc(&ev->arena, sizeof *m, _Alignof(ccord_interaction_modal));
  if (!m) return false;
  memset(m, 0, sizeof *m);
  (void)json_id_key(d, "id", &m->id);
  (void)json_id_key(d, "application_id", &m->application_id);
  (void)json_id_key(d, "guild_id", &m->guild_id);
  (void)json_id_key(d, "channel_id", &m->channel_id);
  const ccord_json *mdata = is_json_object(ccord_json_get(d, "data")) ? ccord_json_get(d, "data") : d;
  copy_json_str(m->custom_id, sizeof m->custom_id, mdata, "custom_id");
  copy_json_str(m->title, sizeof m->title, mdata, "title");
  if (m->application_id == CCORD_ID_NONE || m->custom_id[0] == '\0' || m->title[0] == '\0') return false;
  const ccord_json *rows = ccord_json_get(mdata, "components");
  size_t nr = is_json_array(rows) ? ccord_json_array_len(rows) : 0;
  for (size_t i = 0; i < nr && m->field_count < CCORD_RICH_MAX_COMPONENT_ROWS; i++) {
    const ccord_json *rj = ccord_json_array_at(rows, i);
    if (!is_json_object(rj)) continue;
    int64_t rt = json_i64(rj, "type", 0);
    if (rt == CCORD_COMP_ACTION_ROW) {
      const ccord_json *carr = ccord_json_get(rj, "components");
      size_t nc = is_json_array(carr) ? ccord_json_array_len(carr) : 0;
      for (size_t k = 0; k < nc && m->field_count < CCORD_RICH_MAX_COMPONENT_ROWS; k++) {
        add_modal_field(m, ccord_json_array_at(carr, k));
      }
    } else if (rt == CCORD_COMP_TEXT_INPUT) {
      add_modal_field(m, rj);
    }
  }
  if (m->field_count == 0) return false;
  ev->ev.type = CCORD_NET_INTERACTION_MODAL;
  ev->ev.modal = m;
  ev->ev.guild_id = m->guild_id;
  ev->ev.channel_id = m->channel_id;
  return true;
}

static bool map_slash_autocomplete(ccord_live_event *ev, const ccord_json *d)
{
  if (!is_json_object(d)) return false;
  const char *nonce = jstr(d, "nonce");
  const ccord_json *carr = ccord_json_get(d, "choices");
  if (!is_json_array(carr)) return false;
  size_t raw_n = ccord_json_array_len(carr);
  size_t cap = raw_n < CCORD_SLASH_MAX_CHOICES ? raw_n : CCORD_SLASH_MAX_CHOICES;
  ccord_slash_choice *choices = nullptr;
  if (cap > 0) {
    choices = ccord_arena_alloc(&ev->arena, cap * sizeof *choices, _Alignof(ccord_slash_choice));
    if (!choices) return false;
    memset(choices, 0, cap * sizeof *choices);
  }
  size_t out_n = 0;
  for (size_t i = 0; i < raw_n && out_n < CCORD_SLASH_MAX_CHOICES; i++) {
    const ccord_json *cj = ccord_json_array_at(carr, i);
    if (!is_json_object(cj)) continue;
    ccord_slash_choice ch = {0};
    copy_json_str(ch.name, sizeof ch.name, cj, "name");
    if (ch.name[0] == '\0') continue;
    const ccord_json *vj = ccord_json_get(cj, "value");
    if (!vj) continue;
    ccord_json_type vt = ccord_json_type_of(vj);
    if (vt == CCORD_JSON_STRING) {
      copy_json_str(ch.value, sizeof ch.value, cj, "value");
    } else if (vt == CCORD_JSON_NUMBER) {
      int64_t iv = 0;
      if (ccord_json_int64(vj, &iv)) {
        (void)snprintf(ch.value, sizeof ch.value, "%lld", (long long)iv);
      } else {
        double dv = ccord_json_number(vj, 0.0);
        (void)snprintf(ch.value, sizeof ch.value, "%g", dv);
      }
    } else {
      continue;
    }
    if (ch.value[0] == '\0') continue;
    choices[out_n++] = ch;
  }
  ev->ev.type = CCORD_NET_SLASH_AUTOCOMPLETE;
  ev->ev.slash_choices = choices;
  ev->ev.slash_choice_count = out_n;
  ev->ev.interaction_nonce = nonce ? ccord_arena_strndup(&ev->arena, nonce, strlen(nonce)) : "";
  return true;
}

static ccord_status decode_gateway_root(const ccord_json *root, ccord_id self_id, ccord_live_event *out,
                                        ccord_error *err)
{
  if (ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: payload is not an object");
  }
  out->op = (int)json_i64(root, "op", -1);
  out->seq = json_i64(root, "s", -1);
  if (out->op == 7) out->reconnect = true;
  if (out->op == 9) {
    out->invalid_session = true;
    out->resumable = json_bool_key(root, "d", false);
  }
  if (out->op == 10) {
    out->hello = true;
    int64_t hb = json_i64(ccord_json_get(root, "d"), "heartbeat_interval", 0);
    if (hb < 1000) hb = 1000;
    if (hb > (int64_t)UINT32_MAX) hb = (int64_t)UINT32_MAX;
    out->heartbeat_interval_ms = (uint32_t)hb;
    return CCORD_OK;
  }
  if (out->op != 0) return CCORD_OK;
  const char *type = jstr(root, "t");
  const ccord_json *d = ccord_json_get(root, "d");
  if (!type || !d) return CCORD_OK;

  if (strcmp(type, "READY") == 0) {
    if (!parse_ready(out, d, err)) {
      if (err && err->code == CCORD_OK) ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad READY");
      return CCORD_ERR_PARSE;
    }
    out->ev.type = CCORD_NET_SNAPSHOT;
    out->ev.snapshot = &out->snapshot;
  } else if (strcmp(type, "READY_SUPPLEMENTAL") == 0) {
    if (!map_ready_supplemental(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad READY_SUPPLEMENTAL");
  } else if (strcmp(type, "PASSIVE_UPDATE_V2") == 0) {
    if (!map_passive_update_v2(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad PASSIVE_UPDATE_V2");
  } else if (strcmp(type, "PASSIVE_UPDATE_V1") == 0) {
    if (!map_passive_update_v1(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad PASSIVE_UPDATE_V1");
  } else if (strcmp(type, "RESUMED") == 0) {
    out->resumed = true;
  } else if (strcmp(type, "GUILD_CREATE") == 0 || strcmp(type, "GUILD_UPDATE") == 0) {
    if (!map_guild_upsert_ex(out, d, self_id)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad guild");
  } else if (strcmp(type, "GUILD_EMOJIS_UPDATE") == 0 || strcmp(type, "GUILD_STICKERS_UPDATE") == 0 ||
             strcmp(type, "GUILD_SOUNDBOARD_SOUNDS_UPDATE") == 0) {
    int kind = strcmp(type, "GUILD_STICKERS_UPDATE") == 0 ? 1
               : (strcmp(type, "GUILD_SOUNDBOARD_SOUNDS_UPDATE") == 0 ? 2 : 0);
    if (!map_guild_expressions_update(out, d, kind)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad expressions update");
    }
  } else if (strcmp(type, "GUILD_SOUNDBOARD_SOUND_CREATE") == 0 ||
             strcmp(type, "GUILD_SOUNDBOARD_SOUND_UPDATE") == 0) {
    if (!map_soundboard_upsert_dispatch(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad soundboard sound");
    }
  } else if (strcmp(type, "GUILD_SOUNDBOARD_SOUND_DELETE") == 0) {
    if (!map_soundboard_delete_dispatch(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad soundboard delete");
    }
  } else if (strcmp(type, "AUTO_MODERATION_RULE_CREATE") == 0 ||
             strcmp(type, "AUTO_MODERATION_RULE_UPDATE") == 0) {
    if (!map_automod_upsert_dispatch(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad automod rule");
    }
  } else if (strcmp(type, "AUTO_MODERATION_RULE_DELETE") == 0) {
    if (!map_automod_delete_dispatch(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad automod rule delete");
    }
  } else if (strcmp(type, "WEBHOOKS_UPDATE") == 0) {
    if (!map_webhooks_update_dispatch(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad webhooks update");
    }
  } else if (strcmp(type, "GUILD_DELETE") == 0) {
    if (!map_guild_delete(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad guild delete");
  } else if (strcmp(type, "GUILD_MEMBERS_CHUNK") == 0) {
    if (!map_guild_members(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad member chunk");
  } else if (strcmp(type, "GUILD_MEMBER_ADD") == 0 || strcmp(type, "GUILD_MEMBER_UPDATE") == 0) {
    if (!map_guild_member_event(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad member");
  } else if (strcmp(type, "GUILD_ROLE_CREATE") == 0 || strcmp(type, "GUILD_ROLE_UPDATE") == 0) {
    if (!map_role_upsert(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad role");
  } else if (strcmp(type, "USER_GUILD_SETTINGS_UPDATE") == 0) {
    if (!map_notify_settings(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad notification settings");
  } else if (strcmp(type, "USER_SETTINGS_UPDATE") == 0) {
    if (!map_user_settings_folders(out, d, false)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad user settings");
  } else if (strcmp(type, "USER_SETTINGS_PROTO_UPDATE") == 0) {
    if (!map_user_settings_proto_update(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad settings proto");
  } else if (strcmp(type, "SESSIONS_REPLACE") == 0) {
    map_sessions_replace(out, d);
  } else if (strcmp(type, "GUILD_ROLE_DELETE") == 0) {
    if (!map_role_delete(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad role delete");
  } else if (strcmp(type, "GUILD_MEMBER_REMOVE") == 0) {
    if (!map_guild_member_remove(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad member remove");
  } else if (strcmp(type, "GUILD_MEMBER_LIST_UPDATE") == 0) {
    if (!map_guild_member_list_update(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad member list update");
    }
  } else if (strcmp(type, "MESSAGE_CREATE") == 0 || strcmp(type, "MESSAGE_UPDATE") == 0) {
    bool updated = strcmp(type, "MESSAGE_UPDATE") == 0;
    ccord_message *m = ccord_arena_alloc(&out->arena, sizeof *m, _Alignof(ccord_message));
    if (!m) return ccord_error_set(err, CCORD_ERR_NOMEM, "gateway: out of memory");
    if (!map_message_ex(&out->arena, d, CCORD_ID_NONE, updated, m)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad message");
    }
    if (updated) {
      // A partial update with neither text nor any part of the structure says nothing about the message.
      const ccord_json *cj = ccord_json_get(d, "content");
      size_t clen = 0;
      bool has_content = cj && ccord_json_string(cj, &clen) != nullptr;
      if (!has_content && !m->rich && !m->has_pinned && m->omit_tts) return CCORD_OK;
    }
    ccord_id msg_gid = CCORD_ID_NONE;
    (void)json_id_key(d, "guild_id", &msg_gid);
    if (!updated) {
      size_t ucap = 1u + jarray_len(d, "mentions");
      ccord_user *u = ccord_arena_alloc(&out->arena, ucap * sizeof *u, _Alignof(ccord_user));
      if (u) {
        size_t ucnt = 0;
        if (m->author_id != CCORD_ID_NONE) {
          map_message_author(&out->arena, d, m->author_id, &u[ucnt++]);
        }
        collect_message_mentions(&out->arena, d, u, &ucnt, ucap);
        out->users = u;
        out->user_count = ucnt;
        out->ev.users = u;
        out->ev.user_count = ucnt;
      }
      if (msg_gid != CCORD_ID_NONE && msg_gid != CCORD_DM_GUILD_ID && m->author_id != CCORD_ID_NONE) {
        ccord_member *gm = ccord_arena_alloc(&out->arena, sizeof *gm, _Alignof(ccord_member));
        if (gm) {
          const ccord_json *mem_obj = ccord_json_get(d, "member");
          const ccord_json *d_roles = guild_roles(d);
          role_table rt = {0};
          if (d_roles) role_table_build(&rt, d_roles);
          const char *rname = "Members";
          uint32_t rcol = 0;
          uint8_t rrank = 255;
          resolve_member_role(&out->arena, msg_gid, rt.count > 0 ? &rt : nullptr, mem_obj, &rname, &rcol, &rrank);
          role_table_free(&rt);
          *gm = (ccord_member){
            .guild_id = msg_gid,
            .user_id = m->author_id,
            .role_name = rname,
            .role_color = rcol,
            .role_rank = rrank,
            .omit_pending = true,   // a message's partial member object never says whether screening is done
          };
          // The author's server nickname travels with the message; leaving it out would clear one the list already has.
          const char *nick = mem_obj ? jstr(mem_obj, "nick") : nullptr;
          if (nick && text_ok(nick, CCORD_NAME_MAX_BYTES)) memcpy(gm->nick, nick, strlen(nick) + 1);
          const char *jat = mem_obj ? jstr(mem_obj, "joined_at") : nullptr;
          if (jat && jat[0] != '\0') {
            int64_t jt = iso_ms(jat, strlen(jat));
            if (jt > 0) gm->joined_at_ms = jt;
          }
          out->members = gm;
          out->member_count = 1;
          out->ev.guild_members = gm;
          out->ev.guild_member_count = 1;
        }
      }
    }
    out->messages = m;
    out->message_count = 1;
    out->ev.type = updated ? CCORD_NET_MESSAGE_UPDATE : CCORD_NET_MESSAGE_CREATE;
    out->ev.guild_id = msg_gid;
    out->ev.channel_id = m->channel_id;
    out->ev.messages = m;
    out->ev.message_count = 1;
  } else if (strcmp(type, "MESSAGE_DELETE") == 0) {
    ccord_id channel = CCORD_ID_NONE, message = CCORD_ID_NONE, gid = CCORD_ID_NONE;
    if (!json_id_key(d, "channel_id", &channel) || !json_id_key(d, "id", &message)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad delete");
    }
    (void)json_id_key(d, "guild_id", &gid);
    out->ev.type = CCORD_NET_MESSAGE_DELETE;
    out->ev.channel_id = channel;
    out->ev.message_id = message;
    out->ev.guild_id = gid;
  } else if (strcmp(type, "MESSAGE_DELETE_BULK") == 0) {
    if (!map_message_delete_bulk(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad bulk delete");
  } else if (strcmp(type, "CHANNEL_PINS_UPDATE") == 0) {
    if (!map_channel_pins_update(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad pins update");
  } else if (strcmp(type, "MESSAGE_ACK") == 0) {
    if (!map_message_ack(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad message ack");
  } else if (strcmp(type, "RELATIONSHIP_ADD") == 0 || strcmp(type, "RELATIONSHIP_REMOVE") == 0) {
    if (!map_relationship_event(out, d, strcmp(type, "RELATIONSHIP_REMOVE") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad relationship");
    }
  } else if (strcmp(type, "TYPING_START") == 0) {
    if (!map_typing(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad typing");
  } else if (strcmp(type, "PRESENCE_UPDATE") == 0) {
    if (!map_presence(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad presence");
  } else if (strcmp(type, "VOICE_STATE_UPDATE") == 0) {
    if (!map_voice_update(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad voice state");
  } else if (strcmp(type, "VOICE_SERVER_UPDATE") == 0) {
    if (!map_voice_server(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad voice server");
  } else if (strcmp(type, "STREAM_CREATE") == 0) {
    if (!map_stream_signal(out, d, CCORD_STREAM_SIG_CREATE)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad stream signal");
  } else if (strcmp(type, "STREAM_UPDATE") == 0) {
    if (!map_stream_signal(out, d, CCORD_STREAM_SIG_UPDATE)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad stream update");
  } else if (strcmp(type, "STREAM_SERVER_UPDATE") == 0) {
    if (!map_stream_signal(out, d, CCORD_STREAM_SIG_SERVER_UPDATE)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad stream server");
  } else if (strcmp(type, "STREAM_DELETE") == 0) {
    if (!map_stream_signal(out, d, CCORD_STREAM_SIG_DELETE)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad stream delete");
  } else if (strcmp(type, "CALL_CREATE") == 0 || strcmp(type, "CALL_UPDATE") == 0) {
    if (!map_call_event(out, d, strcmp(type, "CALL_CREATE") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad call");
    }
  } else if (strcmp(type, "CALL_DELETE") == 0) {
    if (!map_call_delete(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad call delete");
  } else if (strcmp(type, "VOICE_CHANNEL_STATUS_UPDATE") == 0) {
    if (!map_voice_channel_status(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad voice channel status");
  } else if (strcmp(type, "STAGE_INSTANCE_CREATE") == 0 || strcmp(type, "STAGE_INSTANCE_UPDATE") == 0 ||
             strcmp(type, "STAGE_INSTANCE_DELETE") == 0) {
    if (!map_stage_instance_dispatch(out, d, strcmp(type, "STAGE_INSTANCE_DELETE") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad stage instance");
    }
  } else if (strcmp(type, "GUILD_SCHEDULED_EVENT_CREATE") == 0 ||
             strcmp(type, "GUILD_SCHEDULED_EVENT_UPDATE") == 0 ||
             strcmp(type, "GUILD_SCHEDULED_EVENT_DELETE") == 0) {
    if (!map_scheduled_event_dispatch(out, d, strcmp(type, "GUILD_SCHEDULED_EVENT_DELETE") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad scheduled event");
    }
  } else if (strcmp(type, "GUILD_SCHEDULED_EVENT_USER_ADD") == 0 ||
             strcmp(type, "GUILD_SCHEDULED_EVENT_USER_REMOVE") == 0) {
    if (!map_scheduled_event_user(out, d, strcmp(type, "GUILD_SCHEDULED_EVENT_USER_ADD") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad scheduled event user");
    }
  } else if (strcmp(type, "EMBEDDED_ACTIVITY_UPDATE_V2") == 0) {
    if (!map_activity_instance_dispatch(out, d)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad activity instance");
    }
  } else if (strcmp(type, "CHANNEL_CREATE") == 0 || strcmp(type, "CHANNEL_UPDATE") == 0 ||
             strcmp(type, "THREAD_CREATE") == 0 || strcmp(type, "THREAD_UPDATE") == 0) {
    if (!map_channel_event(out, d, true)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad channel");
  } else if (strcmp(type, "CHANNEL_DELETE") == 0 || strcmp(type, "THREAD_DELETE") == 0) {
    if (!map_channel_event(out, d, false)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad channel");
  } else if (strcmp(type, "CHANNEL_RECIPIENT_ADD") == 0 || strcmp(type, "CHANNEL_RECIPIENT_REMOVE") == 0) {
    if (!map_dm_recipient_change(out, d, strcmp(type, "CHANNEL_RECIPIENT_ADD") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad recipient change");
    }
  } else if (strcmp(type, "USER_NOTE_UPDATE") == 0) {
    if (!map_user_note_update(out, d, CCORD_ID_NONE)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad user note");
    }
  } else if (strcmp(type, "THREAD_LIST_SYNC") == 0) {
    if (!map_thread_list_sync(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad thread list sync");
  } else if (strcmp(type, "THREAD_MEMBER_UPDATE") == 0) {
    if (!map_thread_member_update(out, d, false)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad thread member");
  } else if (strcmp(type, "THREAD_MEMBERS_UPDATE") == 0) {
    if (!map_thread_member_update(out, d, true)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad thread members");
  } else if (strncmp(type, "MESSAGE_REACTION_", 17) == 0) {
    if (!map_reaction(out, d, type)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad reaction");
  } else if (strcmp(type, "MESSAGE_POLL_VOTE_ADD") == 0 || strcmp(type, "MESSAGE_POLL_VOTE_REMOVE") == 0) {
    if (!map_poll_vote(out, d, strcmp(type, "MESSAGE_POLL_VOTE_ADD") == 0)) {
      return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad poll vote");
    }
  } else if (strcmp(type, "INTERACTION_MODAL_CREATE") == 0) {
    if (!map_interaction_modal(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad interaction modal");
  } else if (strcmp(type, "APPLICATION_COMMAND_AUTOCOMPLETE_RESULT") == 0) {
    if (!map_slash_autocomplete(out, d)) return ccord_error_set(err, CCORD_ERR_PARSE, "gateway: bad slash autocomplete");
  }
  return CCORD_OK;
}

ccord_status ccord_live_gateway_decode_ex(const char *json, size_t len, ccord_id self_id, ccord_live_event *out,
                                          ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "gateway: null payload");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  const ccord_json *root = parse_json(&scratch, json, len, err);
  ccord_status st = root ? decode_gateway_root(root, self_id, out, err) : CCORD_ERR_PARSE;
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_gateway_decode(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  return ccord_live_gateway_decode_ex(json, len, CCORD_ID_NONE, out, err);
}

ccord_status ccord_live_json_message(const char *json, size_t len, ccord_net_event_type type,
                                     ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null message");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    st = CCORD_ERR_PARSE;
    goto done;
  }
  ccord_message *m = ccord_arena_alloc(&out->arena, sizeof *m, _Alignof(ccord_message));
  if (!m) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  if (!map_message(&out->arena, root, CCORD_ID_NONE, m)) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad message object");
    goto done;
  }
  if (m->author_id != CCORD_ID_NONE) {
    ccord_user *u = ccord_arena_alloc(&out->arena, sizeof *u, _Alignof(ccord_user));
    if (u) {
      map_message_author(&out->arena, root, m->author_id, u);
      out->users = u;
      out->user_count = 1;
      out->ev.users = u;
      out->ev.user_count = 1;
    }
  }
  out->messages = m;
  out->message_count = 1;
  out->ev.type = type;
  out->ev.channel_id = m->channel_id;
  out->ev.messages = m;
  out->ev.message_count = 1;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_history(const char *json, size_t len, ccord_id channel_id,
                                     ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null history");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    st = CCORD_ERR_PARSE;
    goto done;
  }
  size_t n = ccord_json_array_len(root);
  if (n == 0) {
    out->ev.type = CCORD_NET_HISTORY;
    out->ev.channel_id = channel_id;
    goto done;
  }
  size_t ucap = n;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *mj = ccord_json_array_at(root, i);
    ucap += mj ? jarray_len(mj, "mentions") : 0;
  }
  ccord_message *msgs = ccord_arena_alloc(&out->arena, n * sizeof *msgs, _Alignof(ccord_message));
  ccord_user *users = ccord_arena_alloc(&out->arena, ucap * sizeof *users, _Alignof(ccord_user));
  if (!msgs || !users) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: history too large");
    goto done;
  }
  size_t kept = 0, ukept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *mj = ccord_json_array_at(root, i);
    if (!map_message(&out->arena, mj, channel_id, &msgs[kept])) continue;
    ccord_id aid = msgs[kept].author_id;
    kept++;
    if (aid != CCORD_ID_NONE) {
      bool seen = false;
      for (size_t u = 0; u < ukept; u++) {
        if (users[u].id == aid) { seen = true; break; }
      }
      if (!seen && ukept < ucap) {
        map_message_author(&out->arena, mj, aid, &users[ukept++]);
      }
    }
    collect_message_mentions(&out->arena, mj, users, &ukept, ucap);
  }
  out->messages = msgs;
  out->message_count = kept;
  out->users = users;
  out->user_count = ukept;
  out->ev.type = CCORD_NET_HISTORY;
  out->ev.channel_id = channel_id;
  out->ev.messages = msgs;
  out->ev.message_count = kept;
  out->ev.users = users;
  out->ev.user_count = ukept;
done:
  if (st != CCORD_OK) ccord_live_event_free(out);
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_search(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null search");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    st = CCORD_ERR_PARSE;
    goto done;
  }

  const ccord_json *arr = root;
  bool nested = false;
  int64_t total_raw = -1;
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    arr = ccord_json_get(root, "messages");
    nested = true;
    total_raw = json_i64(root, "total_results", -1);
  }
  size_t n = arr ? ccord_json_array_len(arr) : 0;
  if (n == 0) {
    out->ev.type = CCORD_NET_SEARCH_RESULTS;
    out->ev.search_total_results = total_raw >= 0 ? clamp_u32(total_raw) : 0u;
    goto done;
  }
  size_t ucap = n * 4u;
  ccord_message *msgs = ccord_arena_alloc(&out->arena, n * sizeof *msgs, _Alignof(ccord_message));
  ccord_user *users = ccord_arena_alloc(&out->arena, ucap * sizeof *users, _Alignof(ccord_user));
  if (!msgs || !users) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: search results too large");
    goto done;
  }
  size_t kept = 0, ukept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *item = ccord_json_array_at(arr, i);
    const ccord_json *mj = item;
    if (nested && item && ccord_json_type_of(item) == CCORD_JSON_ARRAY) {
      size_t group_len = ccord_json_array_len(item);
      mj = group_len > 0 ? ccord_json_array_at(item, 0) : nullptr;
      for (size_t k = 0; k < group_len; k++) {
        const ccord_json *cand = ccord_json_array_at(item, k);
        const ccord_json *hit = cand ? ccord_json_get(cand, "hit") : nullptr;
        if (hit && ccord_json_bool(hit, false)) {
          mj = cand;
          break;
        }
      }
    }
    if (mj && map_message(&out->arena, mj, CCORD_ID_NONE, &msgs[kept])) {
      ccord_id aid = msgs[kept].author_id;
      kept++;
      if (aid != CCORD_ID_NONE) {
        bool seen = false;
        for (size_t u = 0; u < ukept; u++) {
          if (users[u].id == aid) { seen = true; break; }
        }
        if (!seen && ukept < ucap) {
          map_message_author(&out->arena, mj, aid, &users[ukept++]);
        }
      }
      collect_message_mentions(&out->arena, mj, users, &ukept, ucap);
    }
  }
  out->messages = msgs;
  out->message_count = kept;
  out->users = users;
  out->user_count = ukept;
  out->ev.type = CCORD_NET_SEARCH_RESULTS;
  out->ev.messages = msgs;
  out->ev.message_count = kept;
  out->ev.users = users;
  out->ev.user_count = ukept;
  out->ev.search_total_results = total_raw >= 0 ? clamp_u32(total_raw) : (uint32_t)kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_pins(const char *json, size_t len, ccord_id channel_id, bool append,
                                  ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad pins args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    st = CCORD_ERR_PARSE;
    goto done;
  }

  const ccord_json *arr = nullptr;
  bool wrapped_items = false;
  bool has_more = false;
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    arr = ccord_json_get(root, "items");
    if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) {
      st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad pins items");
      goto done;
    }
    wrapped_items = true;
    const ccord_json *hm = ccord_json_get(root, "has_more");
    has_more = hm && ccord_json_bool(hm, false);
  } else if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    arr = root;
  } else {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad pins payload");
    goto done;
  }

  size_t n = ccord_json_array_len(arr);
  out->ev.type = CCORD_NET_PIN_LIST;
  out->ev.channel_id = channel_id;
  out->ev.pins_has_more = has_more;
  out->ev.pins_append = append;
  if (n == 0) {
    out->ev.pins_has_more = false;
    goto done;
  }

  size_t ucap = n * 4u;
  ccord_message *msgs = ccord_arena_alloc(&out->arena, n * sizeof *msgs, _Alignof(ccord_message));
  ccord_user *users = ccord_arena_alloc(&out->arena, ucap * sizeof *users, _Alignof(ccord_user));
  if (!msgs || !users) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: pins too large");
    goto done;
  }
  size_t kept = 0, ukept = 0;
  const char *last_pinned_at = nullptr;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *entry = ccord_json_array_at(arr, i);
    const ccord_json *mj = wrapped_items && entry ? ccord_json_get(entry, "message") : entry;
    if (!mj || !map_message(&out->arena, mj, channel_id, &msgs[kept])) continue;
    msgs[kept].pinned = true;
    if (wrapped_items && entry) {
      const char *pa = jstr(entry, "pinned_at");
      if (pa && pa[0] != '\0') last_pinned_at = pa;
    }
    ccord_id aid = msgs[kept].author_id;
    kept++;
    if (aid != CCORD_ID_NONE) {
      bool seen = false;
      for (size_t u = 0; u < ukept; u++) {
        if (users[u].id == aid) { seen = true; break; }
      }
      if (!seen && ukept < ucap) {
        map_message_author(&out->arena, mj, aid, &users[ukept++]);
      }
    }
    collect_message_mentions(&out->arena, mj, users, &ukept, ucap);
  }
  if (kept > 0) {
    if (last_pinned_at && last_pinned_at[0] != '\0') {
      out->ev.pins_next_cursor = ccord_arena_strndup(&out->arena, last_pinned_at, strlen(last_pinned_at));
    } else {
      char idbuf[24];
      int idlen = ccord_id_format(msgs[kept - 1].id, idbuf);
      if (idlen > 0) {
        out->ev.pins_next_cursor = ccord_arena_strndup(&out->arena, idbuf, (size_t)idlen);
      }
    }
  } else {
    out->ev.pins_has_more = false;
  }
  out->messages = msgs;
  out->message_count = kept;
  out->users = users;
  out->user_count = ukept;
  out->ev.messages = msgs;
  out->ev.message_count = kept;
  out->ev.users = users;
  out->ev.user_count = ukept;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_reaction_users(const char *json, size_t len, ccord_id channel_id,
                                            ccord_id message_id, const char *emoji,
                                            ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad reaction users args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad reaction users array");
    goto done;
  }
  size_t n = ccord_json_array_len(root);
  if (n > 100u) n = 100u;
  ccord_user *users = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *users, _Alignof(ccord_user)) : nullptr;
  if (n > 0 && !users) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *uj = ccord_json_array_at(root, i);
    if (uj && map_user_obj(&out->arena, uj, CCORD_PRESENCE_OFFLINE, &users[kept])) {
      kept++;
    }
  }
  const char *em = emoji ? emoji : "";
  out->users = users;
  out->user_count = kept;
  out->ev.type = CCORD_NET_REACTION_USERS;
  out->ev.channel_id = channel_id;
  out->ev.message_id = message_id;
  out->ev.reaction_emoji = ccord_arena_strndup(&out->arena, em, strlen(em));
  out->ev.users = users;
  out->ev.user_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_channel(const char *json, size_t len, ccord_id guild_hint,
                                     ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null channel");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad channel object");
    goto done;
  }
  ccord_channel c;
  if (!map_channel(&out->arena, root, guild_hint, &c)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: invalid channel fields");
    goto done;
  }
  ccord_channel *slot = ccord_arena_alloc(&out->arena, sizeof *slot, _Alignof(ccord_channel));
  if (!slot) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  *slot = c;
  out->channels = slot;
  out->channel_count = 1;
  attach_channel_recipients(out, root);
  {
    perm_lists pl = {0};
    (void)perm_add_channel(&out->arena, &pl, root);
    (void)perm_publish(out, &pl);
  }
  if (ccord_discord_type_is_forum(c.discord_type)) {
    ccord_forum_meta *fm = ccord_arena_alloc(&out->arena, sizeof *fm, _Alignof(ccord_forum_meta));
    if (fm) {
      map_forum_meta(root, c.id, c.discord_type, fm);
      out->ev.forum_metas = fm;
      out->ev.forum_meta_count = 1;
    }
  }
  out->ev.type = CCORD_NET_CHANNEL_UPSERT;
  out->ev.guild_id = c.guild_id;
  out->ev.channel_id = c.id;
  out->ev.channel = slot;
done:
  ccord_arena_free(&scratch);
  return st;
}

// A REST member object (PATCH /guilds/{g}/members/{u}) as a GUILD_MEMBERS event, exactly like the gateway's
// GUILD_MEMBER_UPDATE, so one code path updates the store.
ccord_status ccord_live_json_member(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                    ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad member args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad member object");
    goto done;
  }
  snapshot_builder b = {.arena = &out->arena};
  perm_lists pl = {0};
  bool ok = map_member_with_pres(&b, nullptr, nullptr, root, guild_id);
  if (ok) ok = perm_add_member(&out->arena, &pl, guild_id, root);
  if (ok) ok = sb_publish(&b, out);
  sb_free(&b);
  (void)perm_publish(out, &pl);
  if (!ok || out->member_count == 0) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: invalid member fields");
    goto done;
  }
  out->guild_op = CCORD_LIVE_GUILD_MEMBERS;
  out->ev.type = CCORD_NET_GUILD_MEMBERS;
  out->ev.guild_id = guild_id;
  out->ev.guild_members = out->members;
  out->ev.guild_member_count = out->member_count;
  out->ev.users = out->users;
  out->ev.user_count = out->user_count;
done:
  ccord_arena_free(&scratch);
  return st;
}

// A REST guild object (PATCH /guilds/{g}) as a GUILD_UPSERT event, shaped exactly as the gateway shapes
// GUILD_UPDATE. It carries no channels, so the store keeps the ones it has.
ccord_status ccord_live_json_guild(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null guild");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT || !map_guild_upsert(out, root) || out->guild_count == 0) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad guild object");
    goto done;
  }
  out->ev.type = CCORD_NET_GUILD_UPSERT;
  out->ev.guild = &out->guilds[0];
  out->ev.guild_channels = out->snap_channels;
  out->ev.guild_channel_count = out->snap_channel_count;
  out->ev.guild_members = out->members;
  out->ev.guild_member_count = out->member_count;
  out->ev.users = out->users;
  out->ev.user_count = out->user_count;
  out->ev.guild_id = out->guilds[0].id;
done:
  ccord_arena_free(&scratch);
  return st;
}

// Discord's error body is {"code":50013,"message":"Missing Permissions"}; validation failures (400) also carry
// {"errors":{"name":{"_errors":[{"code":"BASE_TYPE_MAX_LENGTH","message":"Must be 100 or fewer in length."}]}}}.
// Writes "Missing Permissions" or "Invalid Form Body: Must be 100 or fewer in length." (text only, never markup).
static const char *first_error_detail(const ccord_json *node, int depth)
{
  if (!node || depth > 6 || ccord_json_type_of(node) != CCORD_JSON_OBJECT) return nullptr;
  const ccord_json *errs = ccord_json_get(node, "_errors");
  if (errs && ccord_json_array_len(errs) > 0) {
    const char *m = jstr(ccord_json_array_at(errs, 0), "message");
    if (m && m[0] != '\0') return m;
  }
  for (size_t i = 0; i < ccord_json_object_len(node); i++) {
    const ccord_json *child = nullptr;
    (void)ccord_json_object_key(node, i, &child);
    const char *m = first_error_detail(child, depth + 1);
    if (m) return m;
  }
  return nullptr;
}

bool ccord_live_json_error_message(const char *json, size_t len, char *out, size_t cap)
{
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!json || len == 0 || len > 65536u) return false;
  ccord_arena arena;
  ccord_arena_init(&arena, len * 4u + 1024u);
  ccord_error perr;
  const ccord_json *root = parse_json(&arena, json, len, &perr);
  bool ok = false;
  if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    const char *msg = jstr(root, "message");
    const char *detail = first_error_detail(ccord_json_get(root, "errors"), 0);
    char line[256];
    if (msg && msg[0] != '\0' && detail) (void)snprintf(line, sizeof line, "%s: %s", msg, detail);
    else if (msg && msg[0] != '\0') (void)snprintf(line, sizeof line, "%s", msg);
    else if (detail) (void)snprintf(line, sizeof line, "%s", detail);
    else line[0] = '\0';
    if (line[0] != '\0') {
      // Keep it to one printable line: it ends up in a notice.
      size_t w = 0;
      for (size_t r = 0; line[r] != '\0' && w + 1 < cap; r++) {
        unsigned char c = (unsigned char)line[r];
        out[w++] = (c < 0x20u || c == 0x7Fu) ? ' ' : (char)c;
      }
      out[w] = '\0';
      if (!ccord_utf8_valid(out, w)) {   // a truncation may have split a character
        size_t keep = w;
        while (keep > 0 && !ccord_utf8_valid(out, keep)) keep--;
        out[keep] = '\0';
      }
      ok = out[0] != '\0';
    }
  }
  ccord_arena_free(&arena);
  return ok;
}

ccord_status ccord_live_json_forum_posts(const char *json, size_t len, ccord_id forum_id,
                                         uint32_t offset, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || forum_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: invalid forum posts args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: forum posts response not an object");
    goto done;
  }
  const ccord_json *th_arr = ccord_json_get(root, "threads");
  const ccord_json *fm_arr = ccord_json_get(root, "first_messages");
  size_t nt = th_arr ? ccord_json_array_len(th_arr) : 0;
  size_t nfm = fm_arr ? ccord_json_array_len(fm_arr) : 0;
  if (nt > CCORD_FORUM_MAX_POSTS) nt = CCORD_FORUM_MAX_POSTS;
  int64_t total_res = json_i64(root, "total_results", (int64_t)nt);
  if (total_res < 0) total_res = 0;
  if (total_res > (int64_t)UINT32_MAX) total_res = (int64_t)UINT32_MAX;
  bool has_more = json_bool_key(root, "has_more", false);

  ccord_forum_post *posts = nt > 0
    ? ccord_arena_alloc(&out->arena, nt * sizeof *posts, _Alignof(ccord_forum_post))
    : nullptr;
  size_t ucap = nt + nfm;
  ccord_user *users = ucap > 0
    ? ccord_arena_alloc(&out->arena, ucap * sizeof *users, _Alignof(ccord_user))
    : nullptr;
  if ((nt > 0 && !posts) || (ucap > 0 && !users)) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }

  size_t kept = 0, ukept = 0;
  for (size_t i = 0; i < nt; i++) {
    const ccord_json *tj = ccord_json_array_at(th_arr, i);
    ccord_id tid = CCORD_ID_NONE;
    if (!tj || !json_id_key(tj, "id", &tid) || tid == CCORD_ID_NONE) continue;
    const ccord_json *matched_fm = nullptr;
    for (size_t m = 0; m < nfm; m++) {
      const ccord_json *fmj = ccord_json_array_at(fm_arr, m);
      ccord_id fmid = CCORD_ID_NONE, fmch = CCORD_ID_NONE;
      (void)json_id_key(fmj, "id", &fmid);
      (void)json_id_key(fmj, "channel_id", &fmch);
      if (fmid == tid || fmch == tid) {
        matched_fm = fmj;
        break;
      }
    }
    if (!map_forum_post(&out->arena, tj, matched_fm, forum_id, CCORD_ID_NONE, &posts[kept])) continue;
    ccord_id aid = posts[kept].first_author_id;
    kept++;
    if (matched_fm && aid != CCORD_ID_NONE && ukept < ucap) {
      bool seen = false;
      for (size_t u = 0; u < ukept; u++) {
        if (users[u].id == aid) { seen = true; break; }
      }
      if (!seen) {
        map_message_author(&out->arena, matched_fm, aid, &users[ukept++]);
      }
    }
  }

  out->users = users;
  out->user_count = ukept;
  out->ev.type = CCORD_NET_FORUM_POSTS;
  out->ev.channel_id = forum_id;
  out->ev.forum_posts = posts;
  out->ev.forum_post_count = kept;
  out->ev.forum_offset = offset;
  out->ev.forum_total = (uint32_t)total_res;
  out->ev.forum_has_more = has_more;
  out->ev.users = users;
  out->ev.user_count = ukept;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_forum_post_created(const char *json, size_t len, ccord_id forum_id,
                                                ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || forum_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: invalid forum post created args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad created thread object");
    goto done;
  }
  ccord_channel c;
  if (!map_channel(&out->arena, root, CCORD_ID_NONE, &c)) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: invalid created thread channel");
    goto done;
  }
  if (c.parent_id == CCORD_ID_NONE) c.parent_id = forum_id;
  ccord_channel *slot_c = ccord_arena_alloc(&out->arena, sizeof *slot_c, _Alignof(ccord_channel));
  ccord_forum_post *slot_p = ccord_arena_alloc(&out->arena, sizeof *slot_p, _Alignof(ccord_forum_post));
  if (!slot_c || !slot_p) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  *slot_c = c;
  const ccord_json *msg_j = ccord_json_get(root, "message");
  if (!map_forum_post(&out->arena, root, msg_j, forum_id, c.guild_id, slot_p)) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: invalid created forum post");
    goto done;
  }
  out->channels = slot_c;
  out->channel_count = 1;
  out->ev.type = CCORD_NET_FORUM_POST_CREATED;
  out->ev.guild_id = c.guild_id;
  out->ev.channel_id = forum_id;
  out->ev.channel = slot_c;
  out->ev.forum_posts = slot_p;
  out->ev.forum_post_count = 1;

  if (msg_j && ccord_json_type_of(msg_j) == CCORD_JSON_OBJECT) {
    ccord_message *slot_m = ccord_arena_alloc(&out->arena, sizeof *slot_m, _Alignof(ccord_message));
    if (slot_m && map_message(&out->arena, msg_j, c.id, slot_m)) {
      out->messages = slot_m;
      out->message_count = 1;
      out->ev.messages = slot_m;
      out->ev.message_count = 1;
      if (slot_m->author_id != CCORD_ID_NONE) {
        ccord_user *slot_u = ccord_arena_alloc(&out->arena, sizeof *slot_u, _Alignof(ccord_user));
        if (slot_u) {
          map_message_author(&out->arena, msg_j, slot_m->author_id, slot_u);
          out->users = slot_u;
          out->user_count = 1;
          out->ev.users = slot_u;
          out->ev.user_count = 1;
        }
      }
    }
  }
done:
  ccord_arena_free(&scratch);
  return st;
}

// Profile extras of GET /users/{id}/profile. Source: docs.discord.food/resources/user, which lists the user-object keys
// (banner, accent_color, avatar_decoration_data). The Get User Profile keys here (banner_color, user_profile.theme_colors,
// guild_member_profile, guild_member, badges) follow the task contract; the docs' profile section was not checked.
// Every key is optional: absent, null or malformed leaves the field empty and never fails the parse.

// "#rrggbb" (user.banner_color): exactly '#' and six hex digits.
static bool parse_hex_color(const char *s, uint32_t *out)
{
  if (!s || s[0] != '#' || strlen(s) != 7) return false;
  uint32_t v = 0;
  for (size_t i = 1; i < 7; i++) {
    char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a') + 10u;
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A') + 10u;
    else return false;
    v = (v << 4) | d;
  }
  *out = v;
  return true;
}

// obj[key] is [primary, accent], each an integer 0..0xFFFFFF. Both must be valid, else nothing is written.
static bool theme_pair(const ccord_json *obj, const char *key, uint32_t *primary, uint32_t *accent)
{
  const ccord_json *arr = obj ? ccord_json_get(obj, key) : nullptr;
  if (!arr || ccord_json_array_len(arr) < 2) return false;
  int64_t p = -1, a = -1;
  const ccord_json *pj2 = ccord_json_array_at(arr, 0);
  const ccord_json *aj = ccord_json_array_at(arr, 1);
  if (!pj2 || !aj || !ccord_json_int64(pj2, &p) || !ccord_json_int64(aj, &a)) return false;
  if (p < 0 || a < 0 || p > (int64_t)0xFFFFFF || a > (int64_t)0xFFFFFF) return false;
  *primary = (uint32_t)p;
  *accent = (uint32_t)a;
  return true;
}

// A CDN name (hash, badge icon, decoration asset) is letters, digits, '_' and '-': it cannot steer the URL path.
static bool cdn_name_ok(const char *s)
{
  if (!s || s[0] == '\0') return false;
  size_t n = strlen(s);
  if (n > 128u) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// https://cdn.discordapp.com/<path>/<name><suffix>, "" when the name is not a safe CDN name.
static const char *cdn_asset_url(ccord_arena *a, const char *path, const char *name, const char *suffix)
{
  if (!a || !path || !cdn_name_ok(name)) return "";
  size_t total = strlen("https://cdn.discordapp.com/") + strlen(path) + 1u + strlen(name) + strlen(suffix);
  if (total > 512u) return "";
  char *out = ccord_arena_alloc(a, total + 1u, 1);
  if (!out) return "";
  (void)snprintf(out, total + 1u, "https://cdn.discordapp.com/%s/%s%s", path, name, suffix);
  return out;
}

// Per-server avatar or banner: guilds/<guild>/users/<user>/<kind>/<hash>, animated hashes (a_...) as .gif.
static const char *guild_member_asset_url(ccord_arena *a, ccord_id gid, ccord_id uid, const char *kind, const char *hash)
{
  if (gid == CCORD_ID_NONE || uid == CCORD_ID_NONE || !cdn_name_ok(hash)) return "";
  char path[96];
  (void)snprintf(path, sizeof path, "guilds/%llu/users/%llu/%s", (unsigned long long)gid,
                 (unsigned long long)uid, kind);
  bool anim = hash[0] == 'a' && hash[1] == '_';
  const char *sz = (kind && strcmp(kind, "banners") == 0) ? "512" : "64";
  char suffix[24];
  snprintf(suffix, sizeof suffix, "%s?size=%s", anim ? ".gif" : ".png", sz);
  return cdn_asset_url(a, path, hash, suffix);
}

// An ISO timestamp key as unix ms; 0 when absent, null or not a timestamp.
static int64_t iso_key_ms(const ccord_json *obj, const char *key)
{
  const char *s = jstr(obj, key);
  if (!s || s[0] == '\0') return 0;
  int64_t t = iso_ms(s, strlen(s));
  return t > 0 ? t : 0;
}

static void parse_profile_extras(ccord_arena *a, const ccord_json *root, const ccord_json *uj, const ccord_json *pj,
                                 ccord_id uid, ccord_user_profile *prof)
{
  // user.banner_color / user_profile.banner_color, then the user-level theme from user_profile.theme_colors.
  if (parse_hex_color(jstr(uj, "banner_color"), &prof->banner_color) ||
      (pj && parse_hex_color(jstr(pj, "banner_color"), &prof->banner_color))) {
    prof->has_banner_color = true;
  }
  if (theme_pair(pj, "theme_colors", &prof->theme_primary, &prof->theme_accent)) prof->has_theme = true;

  const ccord_json *gmp = ccord_json_get(root, "guild_member_profile");
  if (gmp && ccord_json_type_of(gmp) != CCORD_JSON_OBJECT) gmp = nullptr;
  const ccord_json *gm = ccord_json_get(root, "guild_member");
  if (gm && ccord_json_type_of(gm) != CCORD_JSON_OBJECT) gm = nullptr;

  // Per-server profile: its theme replaces the user-level theme when it is present and valid.
  ccord_id gid = CCORD_ID_NONE;
  if (json_id_key(gmp, "guild_id", &gid) && gid != CCORD_DM_GUILD_ID) prof->guild_id = gid;
  else gid = CCORD_ID_NONE;
  if (theme_pair(gmp, "theme_colors", &prof->theme_primary, &prof->theme_accent)) prof->has_theme = true;
  if (gmp) {
    if (parse_hex_color(jstr(gmp, "banner_color"), &prof->banner_color)) prof->has_banner_color = true;
    const ccord_json *gac = ccord_json_get(gmp, "accent_color");
    if (gac && ccord_json_type_of(gac) == CCORD_JSON_NUMBER) {
      int64_t ga = json_i64(gmp, "accent_color", 0);
      if (ga >= 0 && ga <= (int64_t)0xFFFFFF) {
        prof->accent_color = (uint32_t)ga;
        prof->has_accent_color = true;
      }
    }
  }
  prof->guild_nick = opt_text_dup(a, jstr(gm, "nick"), CCORD_NAME_MAX_BYTES);
  prof->guild_avatar_url = guild_member_asset_url(a, gid, uid, "avatars", jstr(gm, "avatar"));
  prof->guild_banner_url = guild_member_asset_url(a, gid, uid, "banners", jstr(gmp, "banner"));
  prof->guild_bio = opt_text_dup(a, jstr(gmp, "bio"), CCORD_USER_BIO_MAX_BYTES);
  prof->guild_pronouns = opt_text_dup(a, jstr(gmp, "pronouns"), CCORD_USER_PRONOUNS_MAX_BYTES);
  prof->guild_joined_at_ms = iso_key_ms(gm, "joined_at");
  prof->premium_since_ms = iso_key_ms(gm, "premium_since");

  // Top-level badges: [{id, description, icon}], capped at CCORD_PROFILE_MAX_BADGES.
  const ccord_json *badges_arr = root ? ccord_json_get(root, "badges") : nullptr;
  size_t nb = badges_arr ? ccord_json_array_len(badges_arr) : 0;
  for (size_t i = 0; i < nb && prof->badge_count < CCORD_PROFILE_MAX_BADGES; i++) {
    const ccord_json *bj = ccord_json_array_at(badges_arr, i);
    if (!bj || ccord_json_type_of(bj) != CCORD_JSON_OBJECT) continue;
    ccord_profile_badge *dst = &prof->badges[prof->badge_count];
    copy_trunc_utf8(dst->id, sizeof dst->id, jstr(bj, "id"));
    if (dst->id[0] == '\0') continue;
    copy_trunc_utf8(dst->description, sizeof dst->description, jstr(bj, "description"));
    copy_trunc_utf8(dst->icon_url, sizeof dst->icon_url,
                    cdn_asset_url(a, "badge-icons", jstr(bj, "icon"), ".png"));
    prof->badge_count++;
  }

  // user.avatar_decoration_data.asset
  const ccord_json *adj = ccord_json_get(uj, "avatar_decoration_data");
  if (adj && ccord_json_type_of(adj) != CCORD_JSON_OBJECT) adj = nullptr;
  prof->avatar_decoration_url = cdn_asset_url(a, "avatar-decoration-presets", jstr(adj, "asset"),
                                              ".png?size=128&passthrough=true");
}

ccord_status ccord_live_json_user_profile(const char *json, size_t len, ccord_id user_id,
                                          ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null user profile");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad user profile object");
    goto done;
  }
  const ccord_json *uj = ccord_json_get(root, "user");
  if (!uj || ccord_json_type_of(uj) != CCORD_JSON_OBJECT) uj = root;
  const ccord_json *pj = ccord_json_get(root, "user_profile");
  if (pj && ccord_json_type_of(pj) != CCORD_JSON_OBJECT) pj = nullptr;

  ccord_id uid = user_id;
  (void)json_id_key(uj, "id", &uid);
  if (uid == CCORD_ID_NONE) (void)json_id_key(root, "id", &uid);
  if (uid == CCORD_ID_NONE) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: user profile missing id");
    goto done;
  }

  const char *display_name = jstr(uj, "global_name");
  if (!display_name || display_name[0] == '\0') display_name = jstr(uj, "username");
  display_name = opt_text_dup(&out->arena, display_name, CCORD_NAME_MAX_BYTES);

  const char *handle = opt_text_dup(&out->arena, jstr(uj, "username"), CCORD_NAME_MAX_BYTES);

  const char *av_hash = jstr(uj, "avatar");
  const char *avatar_url = build_cdn_url(&out->arena, "avatars", uid, av_hash);

  const char *ban_hash = pj ? jstr(pj, "banner") : nullptr;
  if (!ban_hash || ban_hash[0] == '\0') ban_hash = jstr(uj, "banner");
  const char *banner_url = build_cdn_url(&out->arena, "banners", uid, ban_hash);

  const ccord_json *pac = pj ? ccord_json_get(pj, "accent_color") : nullptr;
  const ccord_json *uac = ccord_json_get(uj, "accent_color");
  bool has_accent = (pac && ccord_json_type_of(pac) == CCORD_JSON_NUMBER) ||
                    (uac && ccord_json_type_of(uac) == CCORD_JSON_NUMBER);
  int64_t accent = (pac && ccord_json_type_of(pac) == CCORD_JSON_NUMBER) ? json_i64(pj, "accent_color", 0)
                                                                          : json_i64(uj, "accent_color", 0);
  if (accent < 0 || accent > (int64_t)0xFFFFFF) {
    accent = 0;
    has_accent = false;
  }

  const char *bio = pj ? jstr(pj, "bio") : nullptr;
  if (!bio || bio[0] == '\0') bio = jstr(uj, "bio");
  bio = opt_text_dup(&out->arena, bio, CCORD_USER_BIO_MAX_BYTES);

  const char *pronouns = pj ? jstr(pj, "pronouns") : nullptr;
  if (!pronouns || pronouns[0] == '\0') pronouns = jstr(uj, "pronouns");
  pronouns = opt_text_dup(&out->arena, pronouns, CCORD_USER_PRONOUNS_MAX_BYTES);

  ccord_user_profile *prof = ccord_arena_alloc(&out->arena, sizeof *prof, _Alignof(ccord_user_profile));
  ccord_user *u = ccord_arena_alloc(&out->arena, sizeof *u, _Alignof(ccord_user));
  if (!prof || !u) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }

  *prof = (ccord_user_profile){
    .user_id = uid,
    .display_name = display_name,
    .handle = handle,
    .avatar_url = avatar_url,
    .banner_url = banner_url,
    .accent_color = (uint32_t)accent,
    .has_accent_color = has_accent,
    .bio = bio,
    .pronouns = pronouns,
  };
  extract_clan_info(uj, prof->clan_tag, &prof->clan_guild_id, prof->clan_badge);
  parse_profile_extras(&out->arena, root, uj, pj, uid, prof);
  {
    const ccord_json *conn_arr = ccord_json_get(root, "connected_accounts");
    size_t nconn = conn_arr ? ccord_json_array_len(conn_arr) : 0;
    for (size_t i = 0; i < nconn && prof->connection_count < CCORD_PROFILE_MAX_CONNECTIONS; i++) {
      const ccord_json *cj = ccord_json_array_at(conn_arr, i);
      if (!cj || ccord_json_type_of(cj) != CCORD_JSON_OBJECT) continue;
      const char *ctype = jstr(cj, "type");
      const char *cname = jstr(cj, "name");
      if (!ctype || ctype[0] == '\0' || !cname || cname[0] == '\0') continue;
      ccord_profile_connection *dst = &prof->connections[prof->connection_count];
      copy_trunc_utf8(dst->type, sizeof dst->type, ctype);
      copy_trunc_utf8(dst->name, sizeof dst->name, cname);
      if (dst->type[0] == '\0' || dst->name[0] == '\0') continue;
      dst->verified = json_bool_key(cj, "verified", false);
      prof->connection_count++;
    }
    const ccord_json *mg_arr = ccord_json_get(root, "mutual_guilds");
    size_t nmg = mg_arr ? ccord_json_array_len(mg_arr) : 0;
    for (size_t i = 0; i < nmg && prof->mutual_guild_count < CCORD_PROFILE_MAX_MUTUAL_GUILDS; i++) {
      const ccord_json *mgj = ccord_json_array_at(mg_arr, i);
      ccord_id mgid = CCORD_ID_NONE;
      if (!json_id_key(mgj, "id", &mgid) && !json_id(mgj, &mgid)) continue;
      if (mgid == CCORD_ID_NONE || mgid == CCORD_DM_GUILD_ID) continue;
      bool dup = false;
      for (size_t k = 0; k < prof->mutual_guild_count; k++) {
        if (prof->mutual_guild_ids[k] == mgid) { dup = true; break; }
      }
      if (!dup) prof->mutual_guild_ids[prof->mutual_guild_count++] = mgid;
    }
    const ccord_json *mf_arr = ccord_json_get(root, "mutual_friends");
    size_t nmf = mf_arr ? ccord_json_array_len(mf_arr) : 0;
    for (size_t i = 0; i < nmf && prof->mutual_friend_count < CCORD_PROFILE_MAX_MUTUAL_FRIENDS; i++) {
      const ccord_json *mfj = ccord_json_array_at(mf_arr, i);
      ccord_id fuid = CCORD_ID_NONE;
      if (!mfj || !json_id_key(mfj, "id", &fuid) || fuid == CCORD_ID_NONE) continue;
      bool dup = false;
      for (size_t k = 0; k < prof->mutual_friend_count; k++) {
        if (prof->mutual_friends[k].user_id == fuid) { dup = true; break; }
      }
      if (dup) continue;
      const char *fname = jstr(mfj, "global_name");
      if (!fname || fname[0] == '\0') fname = jstr(mfj, "username");
      if (!fname || fname[0] == '\0') fname = "User";
      ccord_mutual_friend *mf = &prof->mutual_friends[prof->mutual_friend_count];
      mf->user_id = fuid;
      copy_trunc_utf8(mf->name, sizeof mf->name, fname);
      if (mf->name[0] == '\0') copy_trunc_utf8(mf->name, sizeof mf->name, "User");
      copy_trunc_utf8(mf->handle, sizeof mf->handle, jstr(mfj, "username"));
      const char *fav = build_cdn_url(&scratch, "avatars", fuid, jstr(mfj, "avatar"));
      copy_trunc_utf8(mf->avatar_url, sizeof mf->avatar_url, fav);
      prof->mutual_friend_count++;
    }
    int64_t mfc = json_i64(root, "mutual_friends_count", -1);
    if (mfc < 0) mfc = json_i64(root, "mutual_friend_count", -1);
    if (mfc < 0) mfc = (int64_t)prof->mutual_friend_count;
    if (mfc > (int64_t)UINT32_MAX) mfc = (int64_t)UINT32_MAX;
    prof->mutual_friend_total = (uint32_t)mfc;
    if (prof->mutual_friend_total < prof->mutual_friend_count) {
      prof->mutual_friend_total = (uint32_t)prof->mutual_friend_count;
    }
  }

  const ccord_json *botj = ccord_json_get(uj, "bot");
  *u = (ccord_user){
    .id = uid,
    .name = (display_name && display_name[0] != '\0') ? display_name : "User",
    .handle = handle,
    .status_text = "",
    .avatar_url = avatar_url,
    .banner_url = banner_url,
    .accent_color = (uint32_t)accent,
    .bio = bio,
    .pronouns = pronouns,
    .presence = CCORD_PRESENCE_OFFLINE,
    .is_bot = botj && ccord_json_bool(botj, false),
    .avatar_palette = (uint8_t)(uid % 6u),
  };
  memcpy(u->clan_tag, prof->clan_tag, sizeof u->clan_tag);

  out->users = u;
  out->user_count = 1;
  out->ev.type = CCORD_NET_USER_PROFILE;
  out->ev.user_id = uid;
  out->ev.user_profile = prof;
  out->ev.users = u;
  out->ev.user_count = 1;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_upload_slots(const char *json, size_t len, size_t expected_count,
                                          char out_upload_urls[][2048],
                                          char out_upload_filenames[][256], ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out_upload_urls || !out_upload_filenames || expected_count == 0 ||
      expected_count > CCORD_RICH_MAX_ATTACHMENTS) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad upload slot args");
  }
  for (size_t i = 0; i < expected_count; i++) {
    out_upload_urls[i][0] = '\0';
    out_upload_filenames[i][0] = '\0';
  }
  ccord_arena arena;
  ccord_arena_init(&arena, scratch_chunk_size(len));
  const ccord_json *root = parse_json(&arena, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return ccord_error_set(err, CCORD_ERR_PARSE, "rest: upload slot not an object");
  }
  const ccord_json *atts = ccord_json_get(root, "attachments");
  if (!atts || !is_json_array(atts) || ccord_json_array_len(atts) < expected_count) {
    ccord_arena_free(&arena);
    return ccord_error_set(err, CCORD_ERR_PARSE, "rest: upload slot missing attachments");
  }
  size_t n = ccord_json_array_len(atts);
  for (size_t i = 0; i < n; i++) {
    const ccord_json *slot = ccord_json_array_at(atts, i);
    if (!is_json_object(slot)) continue;
    size_t idx = i;
    const ccord_json *idj = ccord_json_get(slot, "id");
    if (idj) {
      int64_t iv = -1;
      if (ccord_json_type_of(idj) == CCORD_JSON_NUMBER && ccord_json_int64(idj, &iv) && iv >= 0) {
        idx = (size_t)iv;
      } else if (ccord_json_type_of(idj) == CCORD_JSON_STRING) {
        size_t sl = 0;
        const char *s = ccord_json_string(idj, &sl);
        ccord_id parsed = 0;
        if (s && sl == 1 && s[0] == '0') idx = 0;
        else if (s && ccord_id_parse(s, sl, &parsed)) idx = (size_t)parsed;
      }
    }
    if (idx >= expected_count) continue;
    const char *uurl = jstr(slot, "upload_url");
    const char *ufname = jstr(slot, "upload_filename");
    if (!uurl || uurl[0] == '\0' || !ufname || ufname[0] == '\0') {
      ccord_arena_free(&arena);
      return ccord_error_set(err, CCORD_ERR_PARSE, "rest: upload slot missing url or filename");
    }
    size_t ulen = strlen(uurl), flen = strlen(ufname);
    if (ulen >= 2048u || flen >= 256u) {
      ccord_arena_free(&arena);
      return ccord_error_set(err, CCORD_ERR_RANGE, "rest: upload slot string too long");
    }
    memcpy(out_upload_urls[idx], uurl, ulen + 1);
    memcpy(out_upload_filenames[idx], ufname, flen + 1);
  }
  for (size_t i = 0; i < expected_count; i++) {
    if (out_upload_urls[i][0] == '\0' || out_upload_filenames[i][0] == '\0') {
      ccord_arena_free(&arena);
      return ccord_error_set(err, CCORD_ERR_PARSE, "rest: incomplete upload slots");
    }
  }
  ccord_arena_free(&arena);
  return CCORD_OK;
}

ccord_status ccord_live_json_upload_slot(const char *json, size_t len, char *out_upload_url,
                                         size_t url_cap, char *out_upload_filename,
                                         size_t filename_cap, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out_upload_url || url_cap == 0 || !out_upload_filename || filename_cap == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null upload slot args");
  }
  char urls[1][2048];
  char names[1][256];
  ccord_status st = ccord_live_json_upload_slots(json, len, 1, urls, names, err);
  if (st != CCORD_OK) return st;
  size_t ulen = strlen(urls[0]), flen = strlen(names[0]);
  if (ulen >= url_cap || flen >= filename_cap) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "rest: upload slot string too long");
  }
  memcpy(out_upload_url, urls[0], ulen + 1);
  memcpy(out_upload_filename, names[0], flen + 1);
  return CCORD_OK;
}

// Discord nests options at most SUB_COMMAND_GROUP -> SUB_COMMAND -> leaf. Flatten the tree depth-first into
// cmd->options; parent_plus_one is the 1-based index of the owning container option (0 for root options).
// Malformed nesting (a group inside a group, a leaf inside a group, ...) is skipped, which also bounds the
// recursion depth regardless of the payload.
static void slash_flatten_options(ccord_slash_command *cmd, const ccord_json *opts, uint8_t parent_plus_one)
{
  size_t no = is_json_array(opts) ? ccord_json_array_len(opts) : 0;
  for (size_t k = 0; k < no && cmd->option_count < CCORD_SLASH_MAX_OPTIONS; k++) {
    const ccord_json *oj = ccord_json_array_at(opts, k);
    if (!is_json_object(oj)) continue;
    int64_t okind = json_i64(oj, "type", 0);
    if (okind < CCORD_SLASH_OPT_SUB_COMMAND || okind > CCORD_SLASH_OPT_NUMBER) continue;
    ccord_slash_option opt = {0};
    opt.kind = (ccord_slash_opt_kind)okind;
    copy_json_str(opt.name, sizeof opt.name, oj, "name");
    copy_json_str(opt.description, sizeof opt.description, oj, "description");
    if (opt.name[0] == '\0') continue;
    const ccord_slash_option *parent = parent_plus_one ? &cmd->options[parent_plus_one - 1u] : nullptr;
    if (opt.kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP) {
      if (parent) continue;   // groups only live at the root
    } else if (opt.kind == CCORD_SLASH_OPT_SUB_COMMAND) {
      if (parent && parent->kind != CCORD_SLASH_OPT_SUB_COMMAND_GROUP) continue;
    } else {
      if (parent && parent->kind != CCORD_SLASH_OPT_SUB_COMMAND) continue;   // leaves live at the root or in a subcommand
      opt.required = json_bool_key(oj, "required", false);
      opt.autocomplete = json_bool_key(oj, "autocomplete", false) &&
                         (opt.kind == CCORD_SLASH_OPT_STRING || opt.kind == CCORD_SLASH_OPT_INTEGER ||
                          opt.kind == CCORD_SLASH_OPT_NUMBER);
    }
    opt.parent_plus_one = parent_plus_one;
    cmd->options[cmd->option_count++] = opt;
    if (opt.kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP || opt.kind == CCORD_SLASH_OPT_SUB_COMMAND) {
      slash_flatten_options(cmd, ccord_json_get(oj, "options"), (uint8_t)cmd->option_count);
    }
  }
}

ccord_status ccord_live_json_slash_commands(const char *json, size_t len, ccord_id guild_id,
                                            ccord_id channel_id, ccord_live_event *out,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null slash commands");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    st = CCORD_ERR_PARSE;
    goto done;
  }
  const ccord_json *arr = nullptr;
  if (is_json_array(root)) {
    arr = root;
  } else if (is_json_object(root)) {
    arr = ccord_json_get(root, "application_commands");
    if (!arr) arr = ccord_json_get(root, "commands");
  }
  if (!is_json_array(arr)) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: slash commands not an array");
    goto done;
  }
  size_t raw_n = ccord_json_array_len(arr);
  size_t cap = raw_n < CCORD_SLASH_MAX_COMMANDS ? raw_n : CCORD_SLASH_MAX_COMMANDS;
  ccord_slash_command *cmds = nullptr;
  if (cap > 0) {
    cmds = ccord_arena_alloc(&out->arena, cap * sizeof *cmds, _Alignof(ccord_slash_command));
    if (!cmds) {
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    memset(cmds, 0, cap * sizeof *cmds);
  }
  size_t out_n = 0;
  for (size_t i = 0; i < raw_n && out_n < CCORD_SLASH_MAX_COMMANDS; i++) {
    const ccord_json *cj = ccord_json_array_at(arr, i);
    if (!is_json_object(cj)) continue;
    int64_t ctype = json_i64(cj, "type", 1);
    if (ctype < 1 || ctype > 3) continue; // CHAT_INPUT slash + USER / MESSAGE context-menu commands
    ccord_slash_command cmd = {0};
    if (!json_id_key(cj, "id", &cmd.id) || !json_id_key(cj, "application_id", &cmd.application_id)) continue;
    cmd.command_type = (uint8_t)ctype;
    (void)json_id_key(cj, "guild_id", &cmd.guild_id);
    if (!json_id_key(cj, "version", &cmd.version) || cmd.version == CCORD_ID_NONE) cmd.version = cmd.id;
    copy_json_str(cmd.name, sizeof cmd.name, cj, "name");
    copy_json_str(cmd.description, sizeof cmd.description, cj, "description");
    cmd.default_member_permissions = UINT64_MAX;
    const ccord_json *dpj = ccord_json_get(cj, "default_member_permissions");
    if (dpj && ccord_json_type_of(dpj) == CCORD_JSON_STRING) {
      size_t plen = 0;
      const char *ps = ccord_json_string(dpj, &plen);
      if (ps && plen > 0 && plen <= 20) {
        uint64_t mask = 0;
        bool ok = true;
        for (size_t k = 0; k < plen; k++) {
          if (ps[k] < '0' || ps[k] > '9') { ok = false; break; }
          uint64_t next = mask * 10u + (uint64_t)(ps[k] - '0');
          if (next < mask) { ok = false; break; }
          mask = next;
        }
        if (ok) cmd.default_member_permissions = mask;
      }
    }
    const ccord_json *dmj = ccord_json_get(cj, "dm_permission");
    cmd.dm_permission = dmj && ccord_json_type_of(dmj) == CCORD_JSON_BOOL ? ccord_json_bool(dmj, true) : true;
    // Context-menu commands carry no options; CHAT_INPUT options arrive as a group/subcommand/leaf tree.
    if (cmd.command_type == 1) slash_flatten_options(&cmd, ccord_json_get(cj, "options"), 0);
    ccord_error verr;
    if (ccord_slash_command_validate(&cmd, &verr) != CCORD_OK) continue;
    cmds[out_n++] = cmd;
  }
  out->ev.type = CCORD_NET_SLASH_COMMANDS;
  out->ev.guild_id = guild_id;
  out->ev.channel_id = channel_id;
  out->ev.slash_commands = cmds;
  out->ev.slash_command_count = out_n;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_auth_response(const char *json, size_t len, long http_status,
                                           char *out_token, size_t token_cap,
                                           char *out_mfa_ticket, size_t ticket_cap, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out_token || token_cap < 2) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "auth: invalid token buffer");
  }
  out_token[0] = '\0';
  if (out_mfa_ticket && ticket_cap > 0) out_mfa_ticket[0] = '\0';
  if (!json || len == 0) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "auth: empty response (HTTP %ld)", http_status);
  }
  ccord_arena arena;
  ccord_arena_init(&arena, scratch_chunk_size(len));
  const ccord_json *root = parse_json(&arena, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return ccord_error_set(err, CCORD_ERR_PARSE, "auth: invalid response JSON");
  }
  if (http_status < 200 || http_status >= 300) {
    const char *msg = jstr(root, "message");
    ccord_status st = (http_status == 400 || http_status == 401 || http_status == 403)
                        ? CCORD_ERR_PERMISSION
                        : CCORD_ERR_IO;
    if (msg && msg[0] != '\0') {
      ccord_error_set(err, st, "auth: %s", msg);
    } else {
      ccord_error_set(err, st, "auth: HTTP %ld", http_status);
    }
    ccord_arena_free(&arena);
    return st;
  }
  const char *tok = jstr(root, "token");
  if (tok && tok[0] != '\0') {
    size_t tlen = strlen(tok);
    if (tlen >= token_cap) {
      ccord_arena_free(&arena);
      return ccord_error_set(err, CCORD_ERR_RANGE, "auth: token too long");
    }
    memcpy(out_token, tok, tlen + 1);
    ccord_arena_free(&arena);
    return CCORD_OK;
  }
  const ccord_json *mfa_j = ccord_json_get(root, "mfa");
  bool mfa = mfa_j && ccord_json_bool(mfa_j, false);
  const char *ticket = jstr(root, "ticket");
  if (mfa && ticket && ticket[0] != '\0') {
    if (!out_mfa_ticket || ticket_cap < 2) {
      ccord_arena_free(&arena);
      return ccord_error_set(err, CCORD_ERR_PERMISSION, "auth: 2FA required");
    }
    size_t klen = strlen(ticket);
    if (klen >= ticket_cap) {
      ccord_arena_free(&arena);
      return ccord_error_set(err, CCORD_ERR_RANGE, "auth: MFA ticket too long");
    }
    memcpy(out_mfa_ticket, ticket, klen + 1);
    ccord_arena_free(&arena);
    return CCORD_OK;
  }
  const char *msg = jstr(root, "message");
  ccord_error_set(err, CCORD_ERR_PERMISSION, "auth: %s", (msg && msg[0]) ? msg : "missing token in response");
  ccord_arena_free(&arena);
  return CCORD_ERR_PERMISSION;
}

// ---- Voice Gateway v8 payload decoding ----

static void voice_copy(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

ccord_status ccord_voice_gateway_decode(const char *json, size_t len, ccord_voice_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "voice: null payload");
  memset(out, 0, sizeof *out);
  ccord_arena arena;
  ccord_arena_init(&arena, scratch_chunk_size(len));
  const ccord_json *root = parse_json(&arena, json, len, err);
  if (!root) {
    ccord_arena_free(&arena);
    return CCORD_ERR_PARSE;
  }
  if (ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return ccord_error_set(err, CCORD_ERR_PARSE, "voice: payload is not an object");
  }
  out->op = (int)json_i64(root, "op", -1);
  int64_t seq = json_i64(root, "seq", -1);
  if (seq < 0) seq = json_i64(root, "s", -1);
  out->seq = seq;
  const ccord_json *d = ccord_json_get(root, "d");
  switch (out->op) {
    case CCORD_VOICE_OP_HELLO: {
      int64_t hb = json_i64(d, "heartbeat_interval", 0);
      if (hb < 1000) hb = 1000;
      if (hb > (int64_t)UINT32_MAX) hb = (int64_t)UINT32_MAX;
      out->hello = true;
      out->heartbeat_interval_ms = (uint32_t)hb;
      break;
    }
    case CCORD_VOICE_OP_READY: {
      out->ready = true;
      int64_t ssrc = json_i64(d, "ssrc", 0);
      out->ready_info.ssrc = ssrc > 0 && ssrc <= (int64_t)UINT32_MAX ? (uint32_t)ssrc : 0;
      voice_copy(out->ready_info.ip, sizeof out->ready_info.ip, jstr(d, "ip"));
      int64_t port = json_i64(d, "port", 0);
      out->ready_info.port = port > 0 && port <= 65535 ? (uint16_t)port : 0;
      const ccord_json *modes = d ? ccord_json_get(d, "modes") : nullptr;
      size_t nmodes = modes ? ccord_json_array_len(modes) : 0;
      for (size_t i = 0; i < nmodes && out->ready_info.mode_count < CCORD_VOICE_MAX_MODES; i++) {
        size_t slen = 0;
        const char *s = ccord_json_string(ccord_json_array_at(modes, i), &slen);
        if (!s) continue;
        char *dst = out->ready_info.modes[out->ready_info.mode_count];
        size_t n = slen < CCORD_VOICE_MODE_MAX - 1 ? slen : CCORD_VOICE_MODE_MAX - 1;
        memcpy(dst, s, n);
        dst[n] = '\0';
        out->ready_info.mode_count++;
      }
      break;
    }
    case CCORD_VOICE_OP_SESSION_DESCRIPTION: {
      out->session_description = true;
      voice_copy(out->mode, sizeof out->mode, jstr(d, "mode"));
      int64_t dpv = json_i64(d, "dave_protocol_version", 0);
      out->dave_protocol_version = (dpv > 0 && dpv <= 65535) ? (uint16_t)dpv : 0;
      const ccord_json *key = d ? ccord_json_get(d, "secret_key") : nullptr;
      size_t nkey = key ? ccord_json_array_len(key) : 0;
      for (size_t i = 0; i < nkey && out->secret_key_len < CCORD_VOICE_MAX_SECRET; i++) {
        int64_t v = 0;
        if (!ccord_json_int64(ccord_json_array_at(key, i), &v) || v < 0 || v > 255) continue;
        out->secret_key[out->secret_key_len++] = (uint8_t)v;
      }
      break;
    }
    case CCORD_VOICE_OP_HEARTBEAT_ACK:
      out->heartbeat_ack = true;
      break;
    case CCORD_VOICE_OP_SPEAKING: {
      out->speaking = true;
      int64_t flag = json_i64(d, "speaking", 0);
      out->speaking_flag = flag >= 0 && flag <= 255 ? (uint8_t)flag : 0;
      int64_t ssrc = json_i64(d, "ssrc", 0);
      out->ssrc = ssrc > 0 && ssrc <= (int64_t)UINT32_MAX ? (uint32_t)ssrc : 0;
      (void)json_id_key(d, "user_id", &out->user_id);
      break;
    }
    case CCORD_VOICE_OP_VIDEO: {
      out->video = true;
      int64_t a = json_i64(d, "audio_ssrc", 0), v = json_i64(d, "video_ssrc", 0), r = json_i64(d, "rtx_ssrc", 0);
      out->audio_ssrc = a > 0 && a <= (int64_t)UINT32_MAX ? (uint32_t)a : 0;
      out->video_ssrc = v > 0 && v <= (int64_t)UINT32_MAX ? (uint32_t)v : 0;
      out->rtx_ssrc = r > 0 && r <= (int64_t)UINT32_MAX ? (uint32_t)r : 0;
      (void)json_id_key(d, "user_id", &out->user_id);
      const ccord_json *streams = d ? ccord_json_get(d, "streams") : nullptr;
      size_t ns = streams ? ccord_json_array_len(streams) : 0;
      uint32_t best_ssrc = 0, best_rtx = 0;
      bool best_active = false;
      for (size_t i = 0; i < ns; i++) {
        const ccord_json *se = ccord_json_array_at(streams, i);
        int64_t ss = json_i64(se, "ssrc", 0), rs = json_i64(se, "rtx_ssrc", 0);
        uint32_t s32 = ss > 0 && ss <= (int64_t)UINT32_MAX ? (uint32_t)ss : 0;
        uint32_t r32 = rs > 0 && rs <= (int64_t)UINT32_MAX ? (uint32_t)rs : 0;
        if (s32 == 0 && r32 == 0) continue;
        if (out->stream_ssrc_count < sizeof out->stream_ssrcs / sizeof out->stream_ssrcs[0]) {
          out->stream_ssrcs[out->stream_ssrc_count] = s32;
          out->stream_rtx_ssrcs[out->stream_ssrc_count] = r32;
          out->stream_ssrc_count++;
        }
        bool active = json_bool_key(se, "active", true);
        if (s32 != 0 && (best_ssrc == 0 || (active && !best_active))) {
          best_ssrc = s32;
          best_rtx = r32;
          best_active = active;
        }
      }
      if (out->video_ssrc == 0 && best_ssrc != 0) {
        out->video_ssrc = best_ssrc;
        if (out->rtx_ssrc == 0) out->rtx_ssrc = best_rtx;
      } else if (out->rtx_ssrc == 0 && best_rtx != 0) {
        out->rtx_ssrc = best_rtx;
      }
      break;
    }
    case 11: {  // CCORD_VOICE_OP_CLIENTS_CONNECT
      const ccord_json *uids = d ? ccord_json_get(d, "user_ids") : nullptr;
      size_t nuids = uids ? ccord_json_array_len(uids) : 0;
      size_t cap = sizeof out->connected_users / sizeof out->connected_users[0];
      for (size_t i = 0; i < nuids && out->connected_user_count < cap; i++) {
        ccord_id uid = CCORD_ID_NONE;
        if (json_id(ccord_json_array_at(uids, i), &uid) && uid != CCORD_ID_NONE) {
          out->connected_users[out->connected_user_count++] = uid;
        }
      }
      break;
    }
    case 13:  // CCORD_VOICE_OP_CLIENT_DISCONNECT
    case 18:  // FLAGS (carries user_id of a connected peer)
    case 20:  // PLATFORM (carries user_id of a connected peer)
      (void)json_id_key(d, "user_id", &out->user_id);
      break;
    case 21: {  // CCORD_VOICE_OP_DAVE_PREPARE_TRANSITION
      int64_t tid = json_i64(d, "transition_id", 0);
      int64_t pv = json_i64(d, "protocol_version", 0);
      out->transition_id = (tid >= 0 && tid <= 65535) ? (uint16_t)tid : 0;
      out->dave_protocol_version = (pv >= 0 && pv <= 65535) ? (uint16_t)pv : 0;
      break;
    }
    case 22: {  // CCORD_VOICE_OP_DAVE_EXECUTE_TRANSITION
      int64_t tid = json_i64(d, "transition_id", 0);
      out->transition_id = (tid >= 0 && tid <= 65535) ? (uint16_t)tid : 0;
      break;
    }
    case 24: {  // CCORD_VOICE_OP_DAVE_PREPARE_EPOCH
      int64_t ep = json_i64(d, "epoch", 0);
      int64_t pv = json_i64(d, "protocol_version", 0);
      out->epoch = (ep > 0 && ep <= (int64_t)UINT32_MAX) ? (uint32_t)ep : 0;
      out->dave_protocol_version = (pv >= 0 && pv <= 65535) ? (uint16_t)pv : 0;
      break;
    }
    default:
      break;
  }
  ccord_arena_free(&arena);
  return CCORD_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Server administration answers (REST): roles, bans, invites, audit log
// ---------------------------------------------------------------------------------------------------------------

#define LIVE_ADMIN_LIST_MAX 1000u
#define LIVE_AUDIT_DETAIL_MAX 220u
#define LIVE_AUDIT_CHANGES_SHOWN 3u

static const char *user_label(ccord_arena *a, const ccord_json *u)
{
  const char *n = jstr(u, "global_name");
  if (!n || n[0] == '\0') n = jstr(u, "username");
  return opt_text_dup(a, n, CCORD_NAME_MAX_BYTES * 4u);
}

// A role object, or the array of them PATCH /guilds/{g}/roles answers with, as a ROLE_UPSERT.
// PATCH /users/@me/guilds/{id}/settings answers with the updated entry (the same object as the gateway's
// USER_GUILD_SETTINGS_UPDATE).
ccord_status ccord_live_json_notify_settings(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad notification args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  const ccord_json *root = parse_json(&scratch, json, len, err);
  ccord_status st = CCORD_OK;
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT || !map_notify_settings(out, root)) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad notification settings");
  }
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_role(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad role args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad role object");
    goto done;
  }
  perm_lists pl = {0};
  bool ok = false;
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    ok = perm_add_role(&out->arena, &pl, root, guild_id);
  } else if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    ok = perm_add_roles(&out->arena, &pl, root, guild_id);
  }
  if (!ok || pl.nroles == 0) {
    ccord_free(pl.roles);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: invalid role fields");
    goto done;
  }
  ok = perm_publish(out, &pl);
  if (!ok || out->ev.role_count == 0) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: out of memory");
    goto done;
  }
  out->ev.type = CCORD_NET_ROLE_UPSERT;
  out->ev.guild_id = guild_id;
  out->ev.role_id = out->ev.roles[0].id;
done:
  ccord_arena_free(&scratch);
  return st;
}

// GET /guilds/{g}/bans: [{reason, user:{id, username, global_name}}].
ccord_status ccord_live_json_bans(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad ban args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad ban list");
    goto done;
  }
  size_t n = ccord_json_array_len(root);
  if (n > LIVE_ADMIN_LIST_MAX) n = LIVE_ADMIN_LIST_MAX;
  ccord_ban *bans = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *bans, _Alignof(ccord_ban)) : nullptr;
  if (n > 0 && !bans) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *e = ccord_json_array_at(root, i);
    const ccord_json *u = e ? ccord_json_get(e, "user") : nullptr;
    ccord_id uid = CCORD_ID_NONE;
    if (!u || !json_id_key(u, "id", &uid)) continue;   // a malformed entry is skipped
    bans[kept++] = (ccord_ban){.user_id = uid, .name = user_label(&out->arena, u),
                               .reason = opt_text_dup(&out->arena, jstr(e, "reason"), CCORD_NAME_MAX_BYTES * 8u)};
  }
  out->bans = bans;
  out->ban_count = kept;
  out->ev.type = CCORD_NET_BAN_LIST;
  out->ev.guild_id = guild_id;
  out->ev.bans = bans;
  out->ev.ban_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

static bool map_invite(ccord_arena *a, const ccord_json *j, ccord_invite *out)
{
  const char *code = jstr(j, "code");
  if (!j || !code || code[0] == '\0' || strlen(code) > CCORD_ADMIN_INVITE_CODE_MAX) return false;
  const char *code_dup = arena_dup(a, code);
  if (code_dup[0] == '\0') return false;
  const ccord_json *ch = ccord_json_get(j, "channel");
  const ccord_json *inv = ccord_json_get(j, "inviter");
  ccord_id cid = CCORD_ID_NONE, iid = CCORD_ID_NONE;
  (void)json_id_key(ch, "id", &cid);
  (void)json_id_key(inv, "id", &iid);
  int64_t created = -1, expires = -1;
  const char *cs = jstr(j, "created_at");
  if (cs) created = iso_ms(cs, strlen(cs));
  int64_t age = json_i64(j, "max_age", 0);
  if (age < 0) age = 0;
  if (age > UINT32_MAX) age = UINT32_MAX;
  int64_t max_uses = json_i64(j, "max_uses", 0);
  if (max_uses < 0) max_uses = 0;
  if (max_uses > UINT32_MAX) max_uses = UINT32_MAX;
  int64_t uses = json_i64(j, "uses", 0);
  if (uses < 0) uses = 0;
  if (uses > UINT32_MAX) uses = UINT32_MAX;
  const char *es = jstr(j, "expires_at");
  if (es) expires = iso_ms(es, strlen(es));
  if (expires < 0 && created >= 0 && age > 0) expires = created + age * 1000;
  *out = (ccord_invite){
    .code = code_dup, .channel_id = cid, .channel_name = opt_text_dup(a, jstr(ch, "name"), CCORD_NAME_MAX_BYTES * 4u),
    .inviter_id = iid, .inviter_name = user_label(a, inv),
    .uses = (uint32_t)uses, .max_uses = (uint32_t)max_uses, .max_age_s = (uint32_t)age,
    .temporary = perm_bool(j, "temporary"),
    .created_ms = created > 0 ? created : 0, .expires_ms = expires > 0 ? expires : 0,
  };
  return true;
}

// An array of invites (GET /guilds/{g}/invites) or one invite object (POST /channels/{c}/invites).
ccord_status ccord_live_json_invites(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad invite args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad invite");
    goto done;
  }
  bool single = ccord_json_type_of(root) == CCORD_JSON_OBJECT;
  if (!single && ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad invite");
    goto done;
  }
  size_t n = single ? 1u : ccord_json_array_len(root);
  if (n > LIVE_ADMIN_LIST_MAX) n = LIVE_ADMIN_LIST_MAX;
  ccord_invite *items = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_invite)) : nullptr;
  if (n > 0 && !items) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    if (map_invite(&out->arena, single ? root : ccord_json_array_at(root, i), &items[kept])) kept++;
  }
  if (single && kept == 0) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: invalid invite fields");
    goto done;
  }
  out->invites = items;
  out->invite_count = kept;
  out->ev.type = single ? CCORD_NET_INVITE_UPSERT : CCORD_NET_INVITE_LIST;
  out->ev.guild_id = guild_id;
  out->ev.invites = items;
  out->ev.invite_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_channel_invites(const char *json, size_t len, ccord_id guild_id, ccord_id channel_id,
                                             ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad channel invite args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad channel invites");
    goto done;
  }
  size_t n = ccord_json_array_len(root);
  if (n > LIVE_ADMIN_LIST_MAX) n = LIVE_ADMIN_LIST_MAX;
  ccord_invite *items = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_invite)) : nullptr;
  if (n > 0 && !items) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    if (map_invite(&out->arena, ccord_json_array_at(root, i), &items[kept])) {
      if (items[kept].channel_id == CCORD_ID_NONE) items[kept].channel_id = channel_id;
      kept++;
    }
  }
  out->invites = items;
  out->invite_count = kept;
  out->ev.type = CCORD_NET_CHANNEL_INVITE_LIST;
  out->ev.guild_id = guild_id;
  out->ev.channel_id = channel_id;
  out->ev.invites = items;
  out->ev.invite_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

// A short human rendering of one audit-log change value ("Red", 5, yes ...). Long text is cut on a character boundary.
static void audit_value(ccord_buf *b, const ccord_json *v)
{
  switch (v ? ccord_json_type_of(v) : CCORD_JSON_NULL) {
    case CCORD_JSON_STRING: {
      size_t n = 0;
      const char *s = ccord_json_string(v, &n);
      size_t cut = utf8_trunc_len(s, n, 48);
      (void)ccord_buf_append(b, s, cut);
      if (cut < n) (void)ccord_buf_append_str(b, "...");
      break;
    }
    case CCORD_JSON_NUMBER: {
      int64_t i = 0;
      if (ccord_json_int64(v, &i)) (void)ccord_buf_appendf(b, "%lld", (long long)i);
      else (void)ccord_buf_appendf(b, "%g", ccord_json_number(v, 0));
      break;
    }
    case CCORD_JSON_BOOL: (void)ccord_buf_append_str(b, ccord_json_bool(v, false) ? "yes" : "no"); break;
    case CCORD_JSON_ARRAY: (void)ccord_buf_appendf(b, "%zu items", ccord_json_array_len(v)); break;
    case CCORD_JSON_OBJECT: (void)ccord_buf_append_str(b, "..."); break;
    case CCORD_JSON_NULL: (void)ccord_buf_append_str(b, "none"); break;
  }
}

// "name: Old -> New; color: 0 -> 255" for the first few changes of an entry.
static const char *audit_detail(ccord_arena *a, const ccord_json *entry)
{
  const ccord_json *changes = ccord_json_get(entry, "changes");
  size_t n = changes && ccord_json_type_of(changes) == CCORD_JSON_ARRAY ? ccord_json_array_len(changes) : 0;
  if (n == 0) return "";
  ccord_buf b;
  ccord_buf_init(&b, LIVE_AUDIT_DETAIL_MAX * 2u);
  size_t shown = 0;
  for (size_t i = 0; i < n && shown < LIVE_AUDIT_CHANGES_SHOWN; i++) {
    const ccord_json *c = ccord_json_array_at(changes, i);
    const char *key = jstr(c, "key");
    if (!key || key[0] == '\0') continue;
    const ccord_json *nv = ccord_json_get(c, "new_value");
    const ccord_json *ov = ccord_json_get(c, "old_value");
    if (shown > 0) (void)ccord_buf_append_str(&b, "; ");
    (void)ccord_buf_appendf(&b, "%s: ", key);
    if (ov && nv) {
      audit_value(&b, ov);
      (void)ccord_buf_append_str(&b, " -> ");
      audit_value(&b, nv);
    } else {
      audit_value(&b, nv ? nv : ov);
    }
    shown++;
  }
  if (shown < n && shown > 0) (void)ccord_buf_appendf(&b, " (+%zu more)", n - shown);
  const char *s = ccord_buf_cstr(&b);
  size_t sl = strlen(s);
  size_t cut = utf8_trunc_len(s, sl, LIVE_AUDIT_DETAIL_MAX);
  const char *dup = ccord_arena_strndup(a, s, cut);
  ccord_buf_free(&b);
  return dup ? dup : "";
}

static const char *audit_change_name(ccord_arena *a, const ccord_json *entry)
{
  const ccord_json *changes = ccord_json_get(entry, "changes");
  size_t n = changes && ccord_json_type_of(changes) == CCORD_JSON_ARRAY ? ccord_json_array_len(changes) : 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *c = ccord_json_array_at(changes, i);
    const char *key = jstr(c, "key");
    if (!key || strcmp(key, "name") != 0) continue;
    const char *v = jstr(c, "new_value");
    if (!v || v[0] == '\0') v = jstr(c, "old_value");
    if (v && v[0] != '\0') return opt_text_dup(a, v, CCORD_NAME_MAX_BYTES * 4u);
  }
  return "";
}

static const char *audit_user_name(ccord_arena *a, const ccord_json *users, const id_index *user_idx, ccord_id id)
{
  if (!users || ccord_json_type_of(users) != CCORD_JSON_ARRAY) return "";
  size_t n = ccord_json_array_len(users);
  if (user_idx && user_idx->count > 0) {
    size_t idx = 0;
    if (ccord_idmap_find(user_idx, id, &idx) && idx < n) {
      return user_label(a, ccord_json_array_at(users, idx));
    }
    return "";
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_json *u = ccord_json_array_at(users, i);
    ccord_id uid = CCORD_ID_NONE;
    if (json_id_key(u, "id", &uid) && uid == id) return user_label(a, u);
  }
  return "";
}

static void audit_populate_changes(const ccord_json *entry, ccord_audit_entry *out)
{
  out->change_count = 0;
  const ccord_json *changes = ccord_json_get(entry, "changes");
  size_t n = changes && ccord_json_type_of(changes) == CCORD_JSON_ARRAY ? ccord_json_array_len(changes) : 0;
  if (n == 0) return;
  ccord_buf b;
  ccord_buf_init(&b, 128);
  for (size_t i = 0; i < n && out->change_count < CCORD_AUDIT_MAX_CHANGES; i++) {
    const ccord_json *c = ccord_json_array_at(changes, i);
    const char *key = jstr(c, "key");
    if (!key || key[0] == '\0') continue;
    ccord_audit_change *dst = &out->changes[out->change_count++];
    memset(dst, 0, sizeof *dst);
    copy_trunc_utf8(dst->key, sizeof dst->key, key);
    const ccord_json *ov = ccord_json_get(c, "old_value");
    const ccord_json *nv = ccord_json_get(c, "new_value");
    if (ov) {
      b.len = 0;
      audit_value(&b, ov);
      copy_trunc_utf8(dst->old_val, sizeof dst->old_val, ccord_buf_cstr(&b));
    }
    if (nv) {
      b.len = 0;
      audit_value(&b, nv);
      copy_trunc_utf8(dst->new_val, sizeof dst->new_val, ccord_buf_cstr(&b));
    }
  }
  ccord_buf_free(&b);
}

// GET /guilds/{g}/audit-logs: {audit_log_entries:[...], users:[...]}, newest first.
ccord_status ccord_live_json_audit_log(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                       ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad audit args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  id_index user_idx = {0};
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad audit log");
    goto done;
  }
  const ccord_json *entries = ccord_json_get(root, "audit_log_entries");
  if (!entries || ccord_json_type_of(entries) != CCORD_JSON_ARRAY) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad audit log");
    goto done;
  }
  const ccord_json *users = ccord_json_get(root, "users");
  size_t nu = users && ccord_json_type_of(users) == CCORD_JSON_ARRAY ? ccord_json_array_len(users) : 0;
  if (nu > 8u && ccord_idmap_reserve(&user_idx, nu)) {
    for (size_t i = 0; i < nu; i++) {
      ccord_id uid = CCORD_ID_NONE;
      if (json_id_key(ccord_json_array_at(users, i), "id", &uid) && uid != CCORD_ID_NONE &&
          !ccord_idmap_find(&user_idx, uid, nullptr)) {
        if (!ccord_idmap_put(&user_idx, uid, i)) {
          ccord_idmap_free(&user_idx);
          break;
        }
      }
    }
  }
  size_t n = ccord_json_array_len(entries);
  if (n > LIVE_ADMIN_LIST_MAX) n = LIVE_ADMIN_LIST_MAX;
  ccord_audit_entry *items = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_audit_entry)) : nullptr;
  if (n > 0 && !items) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *e = ccord_json_array_at(entries, i);
    ccord_id id = CCORD_ID_NONE, uid = CCORD_ID_NONE, tid = CCORD_ID_NONE;
    if (!json_id_key(e, "id", &id)) continue;
    (void)json_id_key(e, "user_id", &uid);
    (void)json_id_key(e, "target_id", &tid);
    int64_t action = json_i64(e, "action_type", 0);
    if (action < 0 || action > UINT16_MAX) action = 0;
    const char *tname = tid != CCORD_ID_NONE ? audit_user_name(&out->arena, users, &user_idx, tid) : "";
    if (tname[0] == '\0') tname = audit_change_name(&out->arena, e);
    items[kept] = (ccord_audit_entry){
      .id = id, .time_ms = (int64_t)(id >> 22) + 1420070400000LL,   // Discord's epoch is 2015-01-01
      .user_id = uid, .user_name = uid != CCORD_ID_NONE ? audit_user_name(&out->arena, users, &user_idx, uid) : "",
      .target_id = tid, .target_name = tname, .action = (uint16_t)action,
      .reason = opt_text_dup(&out->arena, jstr(e, "reason"), CCORD_NAME_MAX_BYTES * 8u), .detail = audit_detail(&out->arena, e),
    };
    audit_populate_changes(e, &items[kept]);
    kept++;
  }
  out->audit = items;
  out->audit_count = kept;
  out->ev.type = CCORD_NET_AUDIT_LOG;
  out->ev.guild_id = guild_id;
  out->ev.audit = items;
  out->ev.audit_count = kept;
done:
  ccord_idmap_free(&user_idx);
  ccord_arena_free(&scratch);
  return st;
}

// Discord session object: id_hash, approx_last_used_time, client_info{client,os,platform,location}.
// Flat os/browser/device/location keys are accepted as a fallback for older/synthetic payloads.
static void map_auth_session_obj(const ccord_json *j, ccord_auth_session *s)
{
  memset(s, 0, sizeof *s);
  copy_trunc_utf8(s->id_hash, sizeof s->id_hash, jstr(j, "id_hash"));
  const ccord_json *ci = ccord_json_get(j, "client_info");
  if (ci && ccord_json_type_of(ci) == CCORD_JSON_OBJECT) {
    copy_trunc_utf8(s->os, sizeof s->os, jstr(ci, "os"));
    copy_trunc_utf8(s->browser, sizeof s->browser, jstr(ci, "client"));
    copy_trunc_utf8(s->device, sizeof s->device, jstr(ci, "platform"));
    copy_trunc_utf8(s->location, sizeof s->location, jstr(ci, "location"));
  }
  if (s->os[0] == '\0') copy_trunc_utf8(s->os, sizeof s->os, jstr(j, "os"));
  if (s->browser[0] == '\0') copy_trunc_utf8(s->browser, sizeof s->browser, jstr(j, "browser"));
  if (s->device[0] == '\0') copy_trunc_utf8(s->device, sizeof s->device, jstr(j, "device"));
  if (s->location[0] == '\0') copy_trunc_utf8(s->location, sizeof s->location, jstr(j, "location"));
  s->approx_last_used_ms = json_ts_ms(j, "approx_last_used_time");
  s->current = json_bool_key(j, "current", false);
}

ccord_status ccord_live_json_auth_sessions(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad auth sessions args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad auth sessions");
    goto done;
  }
  size_t total = ccord_json_array_len(root);
  size_t cap = total < CCORD_LIVE_MAX_AUTH_SESSIONS ? total : CCORD_LIVE_MAX_AUTH_SESSIONS;
  ccord_auth_session *items =
    cap > 0 ? ccord_arena_alloc(&out->arena, cap * sizeof *items, _Alignof(ccord_auth_session)) : nullptr;
  if (cap > 0 && !items) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < total && kept < cap; i++) {
    const ccord_json *j = ccord_json_array_at(root, i);
    if (!j || ccord_json_type_of(j) != CCORD_JSON_OBJECT) continue;
    map_auth_session_obj(j, &items[kept]);
    if (items[kept].id_hash[0] == '\0') continue;   // cannot address (or log out) a session without its hash
    kept++;
  }
  out->auth_sessions = items;
  out->auth_session_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

// Connection object: id, name, type, revoked, verified, friend_sync, show_activity, two_way_link, visibility.
static void map_connection_obj(const ccord_json *j, ccord_connection *c)
{
  memset(c, 0, sizeof *c);
  copy_trunc_utf8(c->id, sizeof c->id, jstr(j, "id"));
  copy_trunc_utf8(c->name, sizeof c->name, jstr(j, "name"));
  copy_trunc_utf8(c->type, sizeof c->type, jstr(j, "type"));
  c->revoked = json_bool_key(j, "revoked", false);
  c->verified = json_bool_key(j, "verified", false);
  c->friend_sync = json_bool_key(j, "friend_sync", false);
  c->show_activity = json_bool_key(j, "show_activity", false);
  c->two_way_link = json_bool_key(j, "two_way_link", false);
  c->visibility = json_u8_clamped(j, "visibility", 1u, 0u);
}

ccord_status ccord_live_json_connections(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad connections args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad connections");
    goto done;
  }
  bool single = ccord_json_type_of(root) == CCORD_JSON_OBJECT;
  if (!single && ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad connections");
    goto done;
  }
  size_t total = single ? 1u : ccord_json_array_len(root);
  size_t cap = total < CCORD_LIVE_MAX_CONNECTIONS ? total : CCORD_LIVE_MAX_CONNECTIONS;
  ccord_connection *items =
    cap > 0 ? ccord_arena_alloc(&out->arena, cap * sizeof *items, _Alignof(ccord_connection)) : nullptr;
  if (cap > 0 && !items) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < total && kept < cap; i++) {
    const ccord_json *j = single ? root : ccord_json_array_at(root, i);
    if (!j || ccord_json_type_of(j) != CCORD_JSON_OBJECT) continue;
    map_connection_obj(j, &items[kept]);
    if (items[kept].id[0] == '\0' || items[kept].name[0] == '\0') continue;   // PATCH/DELETE need name + id
    kept++;
  }
  out->connections = items;
  out->connection_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_user_settings(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad user settings args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  const ccord_json *root = parse_json(&scratch, json, len, err);
  ccord_status st = CCORD_OK;
  if (!root || !map_user_settings_folders(out, root, false)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad user settings");
  }
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_custom_status(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad custom status args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  const ccord_json *root = parse_json(&scratch, json, len, err);
  ccord_status st = CCORD_OK;
  if (!root || !map_user_settings_custom_status(out, root)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad custom status");
  }
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_user_note(const char *json, size_t len, ccord_id fallback_uid,
                                       ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad user note args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  const ccord_json *root = parse_json(&scratch, json, len, err);
  ccord_status st = CCORD_OK;
  if (!root || !map_user_note_update(out, root, fallback_uid)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad user note");
  }
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_invite_preview(const char *json, size_t len, bool accepted, ccord_live_event *out,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad invite preview args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad invite preview");
    goto done;
  }
  const char *code = jstr(root, "code");
  const ccord_json *gj = ccord_json_get(root, "guild");
  const ccord_json *cj = ccord_json_get(root, "channel");
  ccord_id gid = CCORD_ID_NONE, cid = CCORD_ID_NONE;
  (void)json_id_key(gj, "id", &gid);
  if (gid == CCORD_ID_NONE) (void)json_id_key(root, "guild_id", &gid);
  (void)json_id_key(cj, "id", &cid);
  if (!code || code[0] == '\0' || gid == CCORD_ID_NONE) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: incomplete invite preview");
    goto done;
  }
  ccord_invite_preview *p = ccord_arena_alloc(&out->arena, sizeof *p, _Alignof(ccord_invite_preview));
  if (!p) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  memset(p, 0, sizeof *p);
  copy_trunc_utf8(p->code, sizeof p->code, code);
  p->guild_id = gid;
  copy_trunc_utf8(p->guild_name, sizeof p->guild_name, gj ? jstr(gj, "name") : nullptr);
  if (p->guild_name[0] == '\0') copy_trunc_utf8(p->guild_name, sizeof p->guild_name, "Server");
  const char *icon_url = gj ? build_cdn_url(&scratch, "icons", gid, jstr(gj, "icon")) : "";
  copy_trunc_utf8(p->guild_icon_url, sizeof p->guild_icon_url, icon_url);
  copy_trunc_utf8(p->guild_description, sizeof p->guild_description, gj ? jstr(gj, "description") : nullptr);
  p->channel_id = cid;
  copy_trunc_utf8(p->channel_name, sizeof p->channel_name, cj ? jstr(cj, "name") : nullptr);
  int64_t online = json_i64(root, "approximate_presence_count", 0);
  int64_t total = json_i64(root, "approximate_member_count", 0);
  if (online < 0) online = 0;
  if (online > UINT32_MAX) online = UINT32_MAX;
  if (total < 0) total = 0;
  if (total > UINT32_MAX) total = UINT32_MAX;
  p->online_count = (uint32_t)online;
  p->member_count = (uint32_t)total;

  out->ev.type = accepted ? CCORD_NET_INVITE_ACCEPTED : CCORD_NET_INVITE_PREVIEW;
  out->ev.guild_id = gid;
  out->ev.channel_id = cid;
  out->ev.invite_preview = p;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_discoverable_guilds(const char *json, size_t len, const char *query,
                                                 uint32_t category_id, uint32_t offset,
                                                 ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad discovery args");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad discovery json");
    goto done;
  }
  const ccord_json *arr = nullptr;
  int64_t total_raw = -1;
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    arr = ccord_json_get(root, "guilds");
    total_raw = json_i64(root, "total", -1);
  } else if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    arr = root;
  }
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: discovery guilds missing");
    goto done;
  }
  size_t n = ccord_json_array_len(arr);
  if (n > CCORD_DISCOVERY_PAGE_MAX) n = CCORD_DISCOVERY_PAGE_MAX;
  ccord_discover_guild *items =
    n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_discover_guild)) : nullptr;
  if (n > 0 && !items) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *gj = ccord_json_array_at(arr, i);
    ccord_id gid = CCORD_ID_NONE;
    if (!gj || !json_id_key(gj, "id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) continue;
    const char *gname = jstr(gj, "name");
    if (!gname || gname[0] == '\0') continue;
    ccord_discover_guild *it = &items[kept++];
    memset(it, 0, sizeof *it);
    it->guild_id = gid;
    copy_trunc_utf8(it->name, sizeof it->name, gname);
    copy_trunc_utf8(it->description, sizeof it->description, jstr(gj, "description"));
    const char *icon_url = build_cdn_url(&scratch, "icons", gid, jstr(gj, "icon"));
    copy_trunc_utf8(it->icon_url, sizeof it->icon_url, icon_url);
    copy_trunc_utf8(it->vanity_code, sizeof it->vanity_code, jstr(gj, "vanity_url_code"));
    int64_t cat = json_i64(gj, "primary_category_id", 0);
    if (cat < 0 || cat > UINT32_MAX) cat = 0;
    it->primary_category_id = (uint32_t)cat;
    int64_t online = json_i64(gj, "approximate_presence_count", 0);
    int64_t members = json_i64(gj, "approximate_member_count", 0);
    if (online < 0) online = 0;
    if (online > UINT32_MAX) online = UINT32_MAX;
    if (members < 0) members = 0;
    if (members > UINT32_MAX) members = UINT32_MAX;
    it->online_count = (uint32_t)online;
    it->member_count = (uint32_t)members;
  }
  uint32_t total = total_raw >= 0 ? (total_raw > UINT32_MAX ? UINT32_MAX : (uint32_t)total_raw) : (uint32_t)kept;
  out->ev.type = CCORD_NET_DISCOVERY_RESULTS;
  out->ev.discovery_guilds = items;
  out->ev.discovery_count = kept;
  out->ev.discovery_total = total;
  out->ev.discovery_offset = offset;
  out->ev.discovery_category_id = category_id;
  out->ev.discovery_query = query ? ccord_arena_strndup(&out->arena, query, strlen(query)) : "";
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_incident_actions(const char *json, size_t len, ccord_id guild_id,
                                              ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad incident args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || (ccord_json_type_of(root) != CCORD_JSON_OBJECT && ccord_json_type_of(root) != CCORD_JSON_NULL)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad incident actions");
    goto done;
  }
  int64_t inv_ms = 0, dms_ms = 0;
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) parse_incidents_obj(root, &inv_ms, &dms_ms);
  out->ev.type = CCORD_NET_GUILD_INCIDENTS;
  out->ev.guild_id = guild_id;
  out->ev.invites_disabled_until_ms = inv_ms;
  out->ev.dms_disabled_until_ms = dms_ms;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_vanity_url(const char *json, size_t len, ccord_id guild_id,
                                        ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad vanity url args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad vanity url");
    goto done;
  }
  const char *code = jstr(root, "code");
  if (!code) code = "";
  const char *dup = ccord_arena_strndup(&out->arena, code, strlen(code));
  if (!dup) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  out->ev.type = CCORD_NET_GUILD_VANITY;
  out->ev.guild_id = guild_id;
  out->ev.invite_code = dup;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_guild_emojis(const char *json, size_t len, ccord_id guild_id,
                                          ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad emoji args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad emoji json");
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    ccord_emoji *el = nullptr;
    size_t en = 0, ecap = 0;
    bool ok = expr_append_emojis(&out->arena, root, guild_id, &el, &en, &ecap);
    if (ok) ok = expr_publish(out, el, en, nullptr, 0, nullptr, 0);
    else ccord_free(el);
    if (!ok) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    out->ev.type = CCORD_NET_GUILD_EXPRESSIONS;
    out->ev.guild_id = guild_id;
    out->ev.replace_emojis = true;
    out->ev.expressions_admin = true;
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    ccord_emoji *e = ccord_arena_alloc(&out->arena, sizeof *e, _Alignof(ccord_emoji));
    bool oom = false;
    if (!e || !map_one_emoji(&out->arena, root, guild_id, e, &oom)) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, (!e || oom) ? CCORD_ERR_NOMEM : CCORD_ERR_PARSE, "rest: bad emoji object");
      goto done;
    }
    e->guild_id = guild_id;
    out->ev.type = CCORD_NET_EMOJI_UPSERT;
    out->ev.guild_id = guild_id;
    out->ev.emojis = e;
    out->ev.emoji_count = 1;
    goto done;
  }
  ccord_live_event_free(out);
  st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad emoji payload");
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_guild_stickers(const char *json, size_t len, ccord_id guild_id,
                                            ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad sticker args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st_res = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    ccord_live_event_free(out);
    st_res = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad sticker json");
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    ccord_sticker *sl = nullptr;
    size_t sn = 0, scap = 0;
    bool ok = expr_append_stickers(&out->arena, root, guild_id, &sl, &sn, &scap);
    if (ok) ok = expr_publish(out, nullptr, 0, sl, sn, nullptr, 0);
    else ccord_free(sl);
    if (!ok) {
      ccord_live_event_free(out);
      st_res = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    out->ev.type = CCORD_NET_GUILD_EXPRESSIONS;
    out->ev.guild_id = guild_id;
    out->ev.replace_stickers = true;
    out->ev.expressions_admin = true;
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    ccord_sticker *st = ccord_arena_alloc(&out->arena, sizeof *st, _Alignof(ccord_sticker));
    bool oom = false;
    if (!st || !map_one_sticker(&out->arena, root, guild_id, st, &oom)) {
      ccord_live_event_free(out);
      st_res = ccord_error_set(err, (!st || oom) ? CCORD_ERR_NOMEM : CCORD_ERR_PARSE, "rest: bad sticker object");
      goto done;
    }
    st->guild_id = guild_id;
    out->ev.type = CCORD_NET_STICKER_UPSERT;
    out->ev.guild_id = guild_id;
    out->ev.stickers = st;
    out->ev.sticker_count = 1;
    goto done;
  }
  ccord_live_event_free(out);
  st_res = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad sticker payload");
done:
  ccord_arena_free(&scratch);
  return st_res;
}

ccord_status ccord_live_json_guild_soundboard(const char *json, size_t len, ccord_id guild_id,
                                              ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad soundboard args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad soundboard json");
    goto done;
  }
  const ccord_json *arr = nullptr;
  if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    arr = root;
  } else if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    const ccord_json *items = ccord_json_get(root, "items");
    if (items && ccord_json_type_of(items) == CCORD_JSON_ARRAY) arr = items;
  }
  if (arr) {
    ccord_soundboard_sound *sbl = nullptr;
    size_t sbn = 0, sbcap = 0;
    bool ok = expr_append_soundboard(&out->arena, arr, guild_id, &sbl, &sbn, &sbcap);
    if (ok) ok = expr_publish(out, nullptr, 0, nullptr, 0, sbl, sbn);
    else ccord_free(sbl);
    if (!ok) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    out->ev.type = CCORD_NET_GUILD_EXPRESSIONS;
    out->ev.guild_id = guild_id;
    out->ev.replace_soundboard = true;
    out->ev.expressions_admin = true;
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    ccord_soundboard_sound *sb =
      ccord_arena_alloc(&out->arena, sizeof *sb, _Alignof(ccord_soundboard_sound));
    bool oom = false;
    if (!sb || !map_one_soundboard(&out->arena, root, guild_id, sb, &oom)) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, (!sb || oom) ? CCORD_ERR_NOMEM : CCORD_ERR_PARSE, "rest: bad soundboard object");
      goto done;
    }
    sb->guild_id = guild_id;
    out->ev.type = CCORD_NET_SOUNDBOARD_UPSERT;
    out->ev.guild_id = guild_id;
    out->ev.soundboard = sb;
    out->ev.soundboard_count = 1;
    goto done;
  }
  ccord_live_event_free(out);
  st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad soundboard payload");
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_prune(const char *json, size_t len, ccord_id guild_id, uint8_t days, bool executed,
                                   ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad prune args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad prune json");
    goto done;
  }
  const ccord_json *pj = ccord_json_get(root, "pruned");
  if (!pj) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: missing pruned field");
    goto done;
  }
  int32_t pruned = -1;
  if (ccord_json_type_of(pj) != CCORD_JSON_NULL) {
    int64_t pv = 0;
    if (!ccord_json_int64(pj, &pv) || pv < 0 || pv > INT32_MAX) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad pruned count");
      goto done;
    }
    pruned = (int32_t)pv;
  }
  out->ev.type = CCORD_NET_PRUNE_RESULT;
  out->ev.guild_id = guild_id;
  out->ev.prune_info = (ccord_prune_info){
    .pruned = pruned,
    .days = days,
    .executed = executed,
    .has_result = true,
  };
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_automod_rules(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                           ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad automod args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad automod json");
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(root);
    if (n > 64u) n = 64u;
    ccord_automod_rule *items = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_automod_rule))
                                      : nullptr;
    if (n > 0 && !items) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
      bool oom = false;
      if (map_one_automod_rule(&out->arena, ccord_json_array_at(root, i), guild_id, &items[k], &oom)) {
        items[k].guild_id = guild_id;
        k++;
      } else if (oom) {
        ccord_live_event_free(out);
        st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
        goto done;
      }
    }
    out->automod_rules = k > 0 ? items : nullptr;
    out->automod_rule_count = k;
    out->ev.type = CCORD_NET_AUTOMOD_LIST;
    out->ev.guild_id = guild_id;
    out->ev.automod_rules = out->automod_rules;
    out->ev.automod_rule_count = k;
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    ccord_automod_rule *r = ccord_arena_alloc(&out->arena, sizeof *r, _Alignof(ccord_automod_rule));
    bool oom = false;
    if (!r || !map_one_automod_rule(&out->arena, root, guild_id, r, &oom)) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, (!r || oom) ? CCORD_ERR_NOMEM : CCORD_ERR_PARSE, "rest: bad automod rule object");
      goto done;
    }
    r->guild_id = guild_id;
    out->automod_rules = r;
    out->automod_rule_count = 1;
    out->ev.type = CCORD_NET_AUTOMOD_UPSERT;
    out->ev.guild_id = guild_id;
    out->ev.automod_rules = r;
    out->ev.automod_rule_count = 1;
    goto done;
  }
  ccord_live_event_free(out);
  st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad automod payload");
done:
  ccord_arena_free(&scratch);
  return st;
}

static const char *arena_dup_trunc(ccord_arena *a, const char *s, size_t max_bytes, bool *oom)
{
  if (!s) s = "";
  size_t n = utf8_trunc_len(s, strlen(s), max_bytes);
  const char *d = ccord_arena_strndup(a, s, n);
  if (!d && oom) *oom = true;
  return d;
}

static bool map_one_webhook(ccord_arena *a, const ccord_json *obj, ccord_id guild_hint, ccord_webhook *out, bool *oom)
{
  if (!obj || ccord_json_type_of(obj) != CCORD_JSON_OBJECT) return false;
  *out = (ccord_webhook){0};
  if (!json_id_key(obj, "id", &out->id) || out->id == CCORD_ID_NONE) return false;
  if (!json_id_key(obj, "guild_id", &out->guild_id) || out->guild_id == CCORD_ID_NONE) out->guild_id = guild_hint;
  if (out->guild_id == CCORD_ID_NONE || out->guild_id == CCORD_DM_GUILD_ID) return false;
  (void)json_id_key(obj, "channel_id", &out->channel_id);
  (void)json_id_key(obj, "application_id", &out->application_id);
  int64_t tp = json_i64(obj, "type", CCORD_WEBHOOK_INCOMING);
  out->type = (tp >= 1 && tp <= 3) ? (uint8_t)tp : (uint8_t)CCORD_WEBHOOK_INCOMING;

  const ccord_json *uj = ccord_json_get(obj, "user");
  const char *uname = "";
  if (uj && ccord_json_type_of(uj) == CCORD_JSON_OBJECT) {
    (void)json_id_key(uj, "id", &out->creator_id);
    const char *gn = jstr(uj, "global_name");
    const char *un = jstr(uj, "username");
    uname = (gn && gn[0] != '\0') ? gn : (un ? un : "");
  }

  const char *sg_name = "";
  const ccord_json *sgj = ccord_json_get(obj, "source_guild");
  if (sgj && ccord_json_type_of(sgj) == CCORD_JSON_OBJECT) {
    (void)json_id_key(sgj, "id", &out->source_guild_id);
    const char *sgn = jstr(sgj, "name");
    if (sgn) sg_name = sgn;
  }

  const char *sc_name = "";
  const ccord_json *scj = ccord_json_get(obj, "source_channel");
  if (scj && ccord_json_type_of(scj) == CCORD_JSON_OBJECT) {
    (void)json_id_key(scj, "id", &out->source_channel_id);
    const char *scn = jstr(scj, "name");
    if (scn) sc_name = scn;
  }

  const char *nm = jstr(obj, "name");
  if (!nm || nm[0] == '\0') nm = "Webhook";
  out->name = arena_dup_trunc(a, nm, CCORD_WEBHOOK_NAME_MAX_BYTES, oom);
  out->avatar = arena_dup_trunc(a, jstr(obj, "avatar"), 128u, oom);
  out->token = arena_dup_trunc(a, jstr(obj, "token"), 256u, oom);
  out->creator_name = arena_dup_trunc(a, uname, CCORD_NAME_MAX_BYTES, oom);
  out->source_guild_name = arena_dup_trunc(a, sg_name, CCORD_NAME_MAX_BYTES, oom);
  out->source_channel_name = arena_dup_trunc(a, sc_name, CCORD_NAME_MAX_BYTES, oom);
  return out->name && out->avatar && out->token && out->creator_name && out->source_guild_name &&
         out->source_channel_name;
}

ccord_status ccord_live_json_webhooks(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                      ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad webhooks args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad webhooks json");
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(root);
    if (n > 256u) n = 256u;
    ccord_webhook *items = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_webhook)) : nullptr;
    if (n > 0 && !items) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
      bool oom = false;
      if (map_one_webhook(&out->arena, ccord_json_array_at(root, i), guild_id, &items[k], &oom)) {
        items[k].guild_id = guild_id;
        k++;
      } else if (oom) {
        ccord_live_event_free(out);
        st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
        goto done;
      }
    }
    out->webhooks = k > 0 ? items : nullptr;
    out->webhook_count = k;
    out->ev.type = CCORD_NET_WEBHOOK_LIST;
    out->ev.guild_id = guild_id;
    out->ev.webhooks = out->webhooks;
    out->ev.webhook_count = k;
    goto done;
  }
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    ccord_webhook *wh = ccord_arena_alloc(&out->arena, sizeof *wh, _Alignof(ccord_webhook));
    bool oom = false;
    if (!wh || !map_one_webhook(&out->arena, root, guild_id, wh, &oom)) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, (!wh || oom) ? CCORD_ERR_NOMEM : CCORD_ERR_PARSE, "rest: bad webhook object");
      goto done;
    }
    wh->guild_id = guild_id;
    out->webhooks = wh;
    out->webhook_count = 1;
    out->ev.type = CCORD_NET_WEBHOOK_UPSERT;
    out->ev.guild_id = guild_id;
    out->ev.webhooks = wh;
    out->ev.webhook_count = 1;
    goto done;
  }
  ccord_live_event_free(out);
  st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad webhooks payload");
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_followed_channel(const char *json, size_t len, ccord_id guild_id, ccord_id fallback_src_ch,
                                              ccord_id target_ch, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID || target_ch == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad followed channel args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad followed channel json");
    goto done;
  }
  ccord_id src_id = CCORD_ID_NONE, wh_id = CCORD_ID_NONE;
  if (!json_id_key(root, "channel_id", &src_id) || src_id == CCORD_ID_NONE) src_id = fallback_src_ch;
  if (!json_id_key(root, "webhook_id", &wh_id) || wh_id == CCORD_ID_NONE || src_id == CCORD_ID_NONE) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: missing followed channel ids");
    goto done;
  }
  out->ev.type = CCORD_NET_CHANNEL_FOLLOWER_ADDED;
  out->ev.guild_id = guild_id;
  out->ev.channel_id = src_id;
  out->ev.target_channel_id = target_ch;
  out->ev.expr_id = wh_id;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_app_cmd_perms(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                           ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad app cmd perms args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad app cmd perms json");
    goto done;
  }
  size_t n = ccord_json_array_len(root);
  if (n > 256u) n = 256u;
  ccord_app_cmd_perms *items =
    n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *items, _Alignof(ccord_app_cmd_perms)) : nullptr;
  if (n > 0 && !items) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *obj = ccord_json_array_at(root, i);
    if (!obj || ccord_json_type_of(obj) != CCORD_JSON_OBJECT) continue;
    ccord_app_cmd_perms entry = {.guild_id = guild_id};
    if (!json_id_key(obj, "id", &entry.id) || entry.id == CCORD_ID_NONE) continue;
    if (!json_id_key(obj, "application_id", &entry.application_id) || entry.application_id == CCORD_ID_NONE) continue;
    ccord_id egid = CCORD_ID_NONE;
    if (json_id_key(obj, "guild_id", &egid) && egid != CCORD_ID_NONE) entry.guild_id = egid;
    const ccord_json *parr = ccord_json_get(obj, "permissions");
    if (parr && ccord_json_type_of(parr) == CCORD_JSON_ARRAY) {
      size_t pn = ccord_json_array_len(parr);
      for (size_t j = 0; j < pn && entry.rule_count < CCORD_APP_CMD_MAX_PERMS; j++) {
        const ccord_json *pj = ccord_json_array_at(parr, j);
        if (!pj || ccord_json_type_of(pj) != CCORD_JSON_OBJECT) continue;
        ccord_id tid = CCORD_ID_NONE;
        if (!json_id_key(pj, "id", &tid) || tid == CCORD_ID_NONE) continue;
        int64_t rtype = json_i64(pj, "type", 0);
        if (rtype < 1 || rtype > 3) continue;
        entry.rules[entry.rule_count++] = (ccord_app_cmd_perm_rule){
          .id = tid,
          .type = (uint8_t)rtype,
          .permission = json_bool_key(pj, "permission", false),
        };
      }
    }
    items[k++] = entry;
  }
  out->app_cmd_perms = k > 0 ? items : nullptr;
  out->app_cmd_perms_count = k;
  out->ev.type = CCORD_NET_APP_CMD_PERMS_LIST;
  out->ev.guild_id = guild_id;
  out->ev.app_cmd_perms = out->app_cmd_perms;
  out->ev.app_cmd_perms_count = k;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_welcome_screen(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad welcome screen args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad welcome screen json");
    goto done;
  }
  ccord_welcome_screen *ws = ccord_arena_alloc(&out->arena, sizeof *ws, _Alignof(ccord_welcome_screen));
  if (!ws) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  *ws = (ccord_welcome_screen){0};
  bool oom = false;
  ws->description = arena_dup_trunc(&out->arena, jstr(root, "description"), CCORD_WELCOME_DESC_MAX_BYTES, &oom);
  if (oom) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  const ccord_json *carr = ccord_json_get(root, "welcome_channels");
  if (carr && ccord_json_type_of(carr) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(carr);
    if (n > CCORD_WELCOME_MAX_CHANNELS) n = CCORD_WELCOME_MAX_CHANNELS;
    ccord_welcome_channel *chs =
      n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *chs, _Alignof(ccord_welcome_channel)) : nullptr;
    if (n > 0 && !chs) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
      const ccord_json *cj = ccord_json_array_at(carr, i);
      if (!cj || ccord_json_type_of(cj) != CCORD_JSON_OBJECT) continue;
      ccord_welcome_channel ch = {0};
      if (!json_id_key(cj, "channel_id", &ch.channel_id) || ch.channel_id == CCORD_ID_NONE) continue;
      (void)json_id_key(cj, "emoji_id", &ch.emoji_id);
      ch.description = arena_dup_trunc(&out->arena, jstr(cj, "description"), CCORD_WELCOME_CH_DESC_MAX_BYTES, &oom);
      ch.emoji_name = arena_dup_trunc(&out->arena, jstr(cj, "emoji_name"), CCORD_EMOJI_MAX_BYTES, &oom);
      if (oom) {
        ccord_live_event_free(out);
        st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
        goto done;
      }
      chs[k++] = ch;
    }
    ws->channels = k > 0 ? chs : nullptr;
    ws->channel_count = k;
  }
  ws->enabled = json_bool_key(root, "enabled", ws->channel_count > 0 || (ws->description && ws->description[0] != '\0'));
  out->welcome_screen = ws;
  out->ev.type = CCORD_NET_WELCOME_SCREEN;
  out->ev.guild_id = guild_id;
  out->ev.welcome_screen = ws;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_onboarding(const char *json, size_t len, ccord_id guild_id, ccord_live_event *out,
                                        ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad onboarding args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad onboarding json");
    goto done;
  }
  ccord_onboarding *ob = ccord_arena_alloc(&out->arena, sizeof *ob, _Alignof(ccord_onboarding));
  if (!ob) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  *ob = (ccord_onboarding){.guild_id = guild_id};
  ccord_id ogid = CCORD_ID_NONE;
  if (json_id_key(root, "guild_id", &ogid) && ogid != CCORD_ID_NONE) ob->guild_id = ogid;
  ob->enabled = json_bool_key(root, "enabled", false);
  int64_t mode = json_i64(root, "mode", CCORD_ONBOARDING_DEFAULT);
  ob->mode = (mode == CCORD_ONBOARDING_ADVANCED) ? CCORD_ONBOARDING_ADVANCED : CCORD_ONBOARDING_DEFAULT;

  const ccord_json *dc_arr = ccord_json_get(root, "default_channel_ids");
  if (dc_arr && ccord_json_type_of(dc_arr) == CCORD_JSON_ARRAY) {
    size_t dn = ccord_json_array_len(dc_arr);
    for (size_t i = 0; i < dn && ob->default_channel_count < CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS; i++) {
      ccord_id cid = CCORD_ID_NONE;
      if (json_id(ccord_json_array_at(dc_arr, i), &cid) && cid != CCORD_ID_NONE) {
        ob->default_channel_ids[ob->default_channel_count++] = cid;
      }
    }
  }

  const ccord_json *parr = ccord_json_get(root, "prompts");
  if (parr && ccord_json_type_of(parr) == CCORD_JSON_ARRAY) {
    size_t pn = ccord_json_array_len(parr);
    if (pn > CCORD_ONBOARDING_MAX_PROMPTS) pn = CCORD_ONBOARDING_MAX_PROMPTS;
    ccord_onboarding_prompt *prompts =
      pn > 0 ? ccord_arena_alloc(&out->arena, pn * sizeof *prompts, _Alignof(ccord_onboarding_prompt)) : nullptr;
    if (pn > 0 && !prompts) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
    size_t pk = 0;
    bool oom = false;
    for (size_t i = 0; i < pn; i++) {
      const ccord_json *pj = ccord_json_array_at(parr, i);
      if (!pj || ccord_json_type_of(pj) != CCORD_JSON_OBJECT) continue;
      ccord_onboarding_prompt pr = {0};
      (void)json_id_key(pj, "id", &pr.id);
      int64_t ptype = json_i64(pj, "type", CCORD_ONBOARDING_MULTIPLE_CHOICE);
      pr.type = (ptype == CCORD_ONBOARDING_DROPDOWN) ? CCORD_ONBOARDING_DROPDOWN : CCORD_ONBOARDING_MULTIPLE_CHOICE;
      const char *ptitle = jstr(pj, "title");
      if (!ptitle || ptitle[0] == '\0') continue;
      pr.title = arena_dup_trunc(&out->arena, ptitle, CCORD_ONBOARDING_TITLE_MAX_BYTES, &oom);
      if (oom) {
        ccord_live_event_free(out);
        st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
        goto done;
      }
      pr.single_select = json_bool_key(pj, "single_select", false);
      pr.required = json_bool_key(pj, "required", false);
      pr.in_onboarding = json_bool_key(pj, "in_onboarding", true);

      const ccord_json *oarr = ccord_json_get(pj, "options");
      if (oarr && ccord_json_type_of(oarr) == CCORD_JSON_ARRAY) {
        size_t on = ccord_json_array_len(oarr);
        if (on > CCORD_ONBOARDING_MAX_OPTIONS) on = CCORD_ONBOARDING_MAX_OPTIONS;
        ccord_onboarding_option *opts =
          on > 0 ? ccord_arena_alloc(&out->arena, on * sizeof *opts, _Alignof(ccord_onboarding_option)) : nullptr;
        if (on > 0 && !opts) {
          ccord_live_event_free(out);
          st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
          goto done;
        }
        size_t ok = 0;
        for (size_t j = 0; j < on; j++) {
          const ccord_json *oj = ccord_json_array_at(oarr, j);
          if (!oj || ccord_json_type_of(oj) != CCORD_JSON_OBJECT) continue;
          ccord_onboarding_option opt = {0};
          (void)json_id_key(oj, "id", &opt.id);
          const char *otitle = jstr(oj, "title");
          if (!otitle || otitle[0] == '\0') continue;
          opt.title = arena_dup_trunc(&out->arena, otitle, CCORD_ONBOARDING_TITLE_MAX_BYTES, &oom);
          opt.description = arena_dup_trunc(&out->arena, jstr(oj, "description"), CCORD_ONBOARDING_DESC_MAX_BYTES, &oom);
          const ccord_json *emj = ccord_json_get(oj, "emoji");
          const char *ename = nullptr;
          if (emj && ccord_json_type_of(emj) == CCORD_JSON_OBJECT) {
            (void)json_id_key(emj, "id", &opt.emoji_id);
            ename = jstr(emj, "name");
          } else {
            (void)json_id_key(oj, "emoji_id", &opt.emoji_id);
            ename = jstr(oj, "emoji_name");
          }
          opt.emoji_name = arena_dup_trunc(&out->arena, ename, CCORD_EMOJI_MAX_BYTES, &oom);
          if (oom) {
            ccord_live_event_free(out);
            st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
            goto done;
          }
          const ccord_json *oc_arr = ccord_json_get(oj, "channel_ids");
          if (oc_arr && ccord_json_type_of(oc_arr) == CCORD_JSON_ARRAY) {
            size_t cn = ccord_json_array_len(oc_arr);
            for (size_t m = 0; m < cn && opt.channel_count < CCORD_ONBOARDING_MAX_OPT_CHANNELS; m++) {
              ccord_id cid = CCORD_ID_NONE;
              if (json_id(ccord_json_array_at(oc_arr, m), &cid) && cid != CCORD_ID_NONE) {
                opt.channel_ids[opt.channel_count++] = cid;
              }
            }
          }
          const ccord_json *or_arr = ccord_json_get(oj, "role_ids");
          if (or_arr && ccord_json_type_of(or_arr) == CCORD_JSON_ARRAY) {
            size_t rn = ccord_json_array_len(or_arr);
            for (size_t m = 0; m < rn && opt.role_count < CCORD_ONBOARDING_MAX_OPT_ROLES; m++) {
              ccord_id rid = CCORD_ID_NONE;
              if (json_id(ccord_json_array_at(or_arr, m), &rid) && rid != CCORD_ID_NONE) {
                opt.role_ids[opt.role_count++] = rid;
              }
            }
          }
          opts[ok++] = opt;
        }
        pr.options = ok > 0 ? opts : nullptr;
        pr.option_count = ok;
      }
      if (pr.option_count > 0) prompts[pk++] = pr;
    }
    ob->prompts = pk > 0 ? prompts : nullptr;
    ob->prompt_count = pk;
  }
  out->onboarding = ob;
  out->ev.type = CCORD_NET_ONBOARDING;
  out->ev.guild_id = guild_id;
  out->ev.onboarding = ob;
done:
  ccord_arena_free(&scratch);
  return st;
}

// GET /guilds/{g}/member-verification answer: {"version":"...","description":"...","form_fields":[{"field_type":"TERMS",
// "label":"Rules","values":["Be kind"],"required":true}],"guild":{...}} -> CCORD_NET_MEMBER_VERIFICATION. Missing
// members tolerate; fields, values and text are cut at the model.h caps. Field types the form model can't hold
// (multiple choice) are skipped; PARAGRAPH reads as TEXT_INPUT.
ccord_status ccord_live_json_member_verification(const char *json, size_t len, ccord_id guild_id,
                                                 ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad member verification args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad member verification json");
    goto done;
  }
  ccord_member_verification *mv = ccord_arena_alloc(&out->arena, sizeof *mv, _Alignof(ccord_member_verification));
  if (!mv) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  *mv = (ccord_member_verification){0};
  bool oom = false;
  mv->version = arena_dup_trunc(&out->arena, jstr(root, "version"), CCORD_MEMBER_VERIFICATION_VERSION_MAX_BYTES, &oom);
  mv->description = arena_dup_trunc(&out->arena, jstr(root, "description"), CCORD_MEMBER_VERIFICATION_DESC_MAX_BYTES, &oom);
  const ccord_json *farr = ccord_json_get(root, "form_fields");
  size_t fn = (farr && ccord_json_type_of(farr) == CCORD_JSON_ARRAY) ? ccord_json_array_len(farr) : 0;
  for (size_t i = 0; i < fn && !oom && mv->field_count < CCORD_MEMBER_VERIFICATION_MAX_FIELDS; i++) {
    const ccord_json *fj = ccord_json_array_at(farr, i);
    if (!fj || ccord_json_type_of(fj) != CCORD_JSON_OBJECT) continue;
    const char *ft = jstr(fj, "field_type");
    ccord_member_verification_field fld = {.field_type = CCORD_MEMBER_VERIFICATION_TERMS};
    if (ft && (strcmp(ft, "TEXT_INPUT") == 0 || strcmp(ft, "PARAGRAPH") == 0)) {
      fld.field_type = CCORD_MEMBER_VERIFICATION_TEXT_INPUT;
    } else if (ft && strcmp(ft, "TERMS") != 0) {
      continue;
    }
    fld.label = arena_dup_trunc(&out->arena, jstr(fj, "label"), CCORD_MEMBER_VERIFICATION_LABEL_MAX_BYTES, &oom);
    fld.required = json_bool_key(fj, "required", true);
    const ccord_json *varr = ccord_json_get(fj, "values");
    size_t vn = (varr && ccord_json_type_of(varr) == CCORD_JSON_ARRAY) ? ccord_json_array_len(varr) : 0;
    for (size_t j = 0; j < vn && !oom && fld.value_count < CCORD_MEMBER_VERIFICATION_MAX_VALUES; j++) {
      const ccord_json *vj = ccord_json_array_at(varr, j);
      size_t slen = 0;
      const char *v = vj ? ccord_json_string(vj, &slen) : nullptr;
      if (!v) continue;
      fld.values[fld.value_count++] = arena_dup_trunc(&out->arena, v, CCORD_MEMBER_VERIFICATION_VALUE_MAX_BYTES, &oom);
    }
    mv->fields[mv->field_count++] = fld;
  }
  if (oom) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  out->ev.type = CCORD_NET_MEMBER_VERIFICATION;
  out->ev.guild_id = guild_id;
  out->ev.member_verification = mv;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_stage_instance(const char *json, size_t len, ccord_id guild_id, ccord_id channel_id,
                                            ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null stage instance");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad stage instance json");
    goto done;
  }
  ccord_stage_instance *si = ccord_arena_alloc(&out->arena, sizeof *si, _Alignof(ccord_stage_instance));
  if (!si) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  memset(si, 0, sizeof *si);
  if (!map_stage_instance_obj(root, guild_id, channel_id, si)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: stage instance has no channel id");
    goto done;
  }
  out->ev.type = CCORD_NET_STAGE_INSTANCE_UPDATE;
  out->ev.guild_id = si->guild_id;
  out->ev.channel_id = si->channel_id;
  out->ev.stage_instance = si;
  out->ev.stage_deleted = false;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_scheduled_event(const char *json, size_t len, ccord_id guild_id,
                                             ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null scheduled event");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad scheduled event json");
    goto done;
  }
  ccord_scheduled_event *se = ccord_arena_alloc(&out->arena, sizeof *se, _Alignof(ccord_scheduled_event));
  if (!se) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  if (!map_scheduled_event_obj(root, guild_id, false, se)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad scheduled event object");
    goto done;
  }
  out->ev.type = CCORD_NET_SCHEDULED_EVENT_UPDATE;
  out->ev.guild_id = se->guild_id;
  out->ev.scheduled_events = se;
  out->ev.scheduled_event_count = 1;
  out->ev.scheduled_event_id = se->id;
  out->ev.scheduled_event_deleted = false;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_scheduled_event_list(const char *json, size_t len, ccord_id guild_id,
                                                  ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null scheduled events");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root) {
    st = CCORD_ERR_PARSE;
    goto done;
  }
  if (!is_json_array(root)) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: scheduled events not an array");
    goto done;
  }
  size_t raw_n = ccord_json_array_len(root);
  size_t cap = raw_n < CCORD_LIVE_MAX_SCHEDULED_EVENTS ? raw_n : CCORD_LIVE_MAX_SCHEDULED_EVENTS;
  ccord_scheduled_event *events = nullptr;
  if (cap > 0) {
    events = ccord_arena_alloc(&out->arena, cap * sizeof *events, _Alignof(ccord_scheduled_event));
    if (!events) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
  }
  size_t kept = 0;
  for (size_t i = 0; i < raw_n && kept < cap; i++) {
    const ccord_json *ej = ccord_json_array_at(root, i);
    if (!is_json_object(ej)) continue;
    if (map_scheduled_event_obj(ej, guild_id, false, &events[kept])) kept++;
  }
  out->ev.type = CCORD_NET_SCHEDULED_EVENT_LIST;
  out->ev.guild_id = guild_id;
  out->ev.scheduled_events = kept > 0 ? events : nullptr;
  out->ev.scheduled_event_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

// One shelf config passes the platform filter when it names "web": a "platform" string, a "platforms" or
// "supported_platforms" list, or a "client_platform_config" object keyed by platform. Absent platform info keeps
// the entry, since the shapes come from unofficial docs.
static bool activity_config_is_web(const ccord_json *cj)
{
  const char *platform = jstr(cj, "platform");
  if (platform) return strcmp(platform, "web") == 0;
  static const char *const lists[] = {"platforms", "supported_platforms"};
  bool seen = false;
  for (size_t k = 0; k < sizeof lists / sizeof lists[0]; k++) {
    const ccord_json *plats = ccord_json_get(cj, lists[k]);
    if (!is_json_array(plats)) continue;
    seen = true;
    size_t n = ccord_json_array_len(plats);
    for (size_t i = 0; i < n; i++) {
      size_t len = 0;
      const char *ps = ccord_json_string(ccord_json_array_at(plats, i), &len);
      if (ps && strcmp(ps, "web") == 0) return true;
    }
  }
  const ccord_json *cfg = ccord_json_get(cj, "client_platform_config");
  if (is_json_object(cfg)) return ccord_json_get(cfg, "web") != nullptr;
  return !seen;
}

// True when `a` must sort after `b`: ranked entries ascend, unranked (0) entries trail them.
static bool shelf_rank_after(const ccord_activity_app *a, const ccord_activity_app *b)
{
  if (a->shelf_rank == 0) return b->shelf_rank != 0;
  if (b->shelf_rank == 0) return false;
  return a->shelf_rank > b->shelf_rank;
}

// GET /activities/shelf?guild_id={g} answer: {"activities":[config],"applications":[partial app],"assets":...}
// -> CCORD_NET_ACTIVITY_SHELF. Each config is joined to its application by application_id (an unmatched config
// has nothing to show and is dropped), only web entries are kept, and the shelf is sorted by shelf_rank.
ccord_status ccord_live_json_activity_shelf(const char *json, size_t len, ccord_id guild_id,
                                            ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad activity shelf args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_live_event_free(out);
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad activity shelf json");
    goto done;
  }
  const ccord_json *carr = ccord_json_get(root, "activities");
  const ccord_json *aarr = ccord_json_get(root, "applications");
  size_t cn = is_json_array(carr) ? ccord_json_array_len(carr) : 0;
  size_t an = is_json_array(aarr) ? ccord_json_array_len(aarr) : 0;
  size_t cap = cn < CCORD_LIVE_MAX_ACTIVITY_APPS ? cn : CCORD_LIVE_MAX_ACTIVITY_APPS;
  ccord_activity_app *apps = nullptr;
  if (cap > 0) {
    apps = ccord_arena_alloc(&out->arena, cap * sizeof *apps, _Alignof(ccord_activity_app));
    if (!apps) {
      ccord_live_event_free(out);
      st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
      goto done;
    }
  }
  size_t kept = 0;
  for (size_t i = 0; i < cn && kept < cap; i++) {
    const ccord_json *cj = ccord_json_array_at(carr, i);
    if (!is_json_object(cj)) continue;
    ccord_id app_id = CCORD_ID_NONE;
    if (!json_id_key(cj, "application_id", &app_id) || app_id == CCORD_ID_NONE) continue;
    if (!activity_config_is_web(cj)) continue;
    const ccord_json *aj = nullptr;
    for (size_t j = 0; j < an; j++) {
      const ccord_json *cand = ccord_json_array_at(aarr, j);
      ccord_id cand_id = CCORD_ID_NONE;
      if (is_json_object(cand) && json_id_key(cand, "id", &cand_id) && cand_id == app_id) {
        aj = cand;
        break;
      }
    }
    if (!aj) continue;
    ccord_activity_app *app = &apps[kept];
    memset(app, 0, sizeof *app);
    app->application_id = app_id;
    copy_json_str(app->name, sizeof app->name, aj, "name");
    copy_json_str(app->description, sizeof app->description, aj, "description");
    copy_json_str(app->icon_hash, sizeof app->icon_hash, aj, "icon");
    if (app->icon_hash[0] == '\0') copy_json_str(app->icon_hash, sizeof app->icon_hash, aj, "icon_hash");
    copy_json_str(app->cover_hash, sizeof app->cover_hash, aj, "cover_image");
    if (app->cover_hash[0] == '\0') copy_json_str(app->cover_hash, sizeof app->cover_hash, aj, "cover_hash");
    int64_t mp = json_i64(aj, "max_participants", 0);
    if (mp <= 0) mp = json_i64(cj, "max_participants", 0);
    app->max_participants = clamp_u32(mp);
    app->shelf_rank = clamp_u32(json_i64(cj, "shelf_rank", 0));
    kept++;
  }
  // Stable sort by shelf_rank, ascending; an entry with no reported rank (0) goes after the ranked ones, in
  // arrival order. The shelf is small, so insertion sort is enough.
  for (size_t i = 1; i < kept; i++) {
    ccord_activity_app tmp = apps[i];
    size_t j = i;
    while (j > 0 && shelf_rank_after(&apps[j - 1], &tmp)) {
      apps[j] = apps[j - 1];
      j--;
    }
    apps[j] = tmp;
  }
  out->ev.type = CCORD_NET_ACTIVITY_SHELF;
  out->ev.guild_id = guild_id;
  out->ev.activity_apps = kept > 0 ? apps : nullptr;
  out->ev.activity_app_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_guild_profile(const char *json, size_t len, ccord_id guild_id,
                                           ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null guild profile");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad guild profile object");
    goto done;
  }
  const ccord_json *src = root;
  const ccord_json *sub = ccord_json_get(root, "profile");
  if (!sub || ccord_json_type_of(sub) != CCORD_JSON_OBJECT) sub = ccord_json_get(root, "guild");
  if (!jstr(src, "name") && sub && ccord_json_type_of(sub) == CCORD_JSON_OBJECT) src = sub;

  ccord_id gid = guild_id;
  (void)json_id_key(src, "id", &gid);
  if (gid == CCORD_ID_NONE) (void)json_id_key(root, "id", &gid);
  if (gid == CCORD_ID_NONE) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: guild profile missing id");
    goto done;
  }

  ccord_guild_profile gp = {.guild_id = gid};
  copy_trunc_utf8(gp.name, sizeof gp.name, jstr(src, "name"));
  copy_trunc_utf8(gp.description, sizeof gp.description, jstr(src, "description"));
  copy_trunc_utf8(gp.tag, sizeof gp.tag, jstr(src, "tag"));
  if (gp.tag[0] == '\0') copy_trunc_utf8(gp.tag, sizeof gp.tag, jstr(root, "tag"));

  const char *icon = jstr(src, "icon_hash");
  if (!icon || icon[0] == '\0') icon = jstr(src, "icon");
  const char *iurl = build_cdn_url_sized(&scratch, "icons", gid, icon, "128");
  copy_trunc_utf8(gp.icon_url, sizeof gp.icon_url, iurl);

  const char *banner = jstr(src, "custom_banner_hash");
  if (!banner || banner[0] == '\0') banner = jstr(src, "banner_hash");
  if (!banner || banner[0] == '\0') banner = jstr(src, "banner");
  const char *burl = build_cdn_url_sized(&scratch, "banners", gid, banner, "512");
  copy_trunc_utf8(gp.banner_url, sizeof gp.banner_url, burl);

  const char *badge = jstr(src, "badge_hash");
  if (!badge || badge[0] == '\0') badge = jstr(src, "badge");
  if (cdn_name_ok(badge)) {
    char idbuf[21];
    ccord_id_format(gid, idbuf);
    (void)snprintf(gp.badge_url, sizeof gp.badge_url,
                   "https://cdn.discordapp.com/guild-tag-badges/%s/%s.png?size=64", idbuf, badge);
  }
  const ccord_json *bcj = ccord_json_get(src, "brand_color_primary");
  int64_t bci = -1;
  if (parse_hex_color(jstr(src, "brand_color_primary"), &gp.brand_color)) {
    gp.has_brand_color = true;
  } else if (bcj && ccord_json_int64(bcj, &bci) && bci >= 0 && bci <= 0xFFFFFF) {
    gp.brand_color = (uint32_t)bci;
    gp.has_brand_color = true;
  }
  gp.online_count = clamp_u32(json_i64(src, "online_count", json_i64(root, "online_count", 0)));
  gp.member_count = clamp_u32(json_i64(src, "member_count", json_i64(root, "member_count", 0)));
  if (gp.name[0] == '\0') gp.unavailable = true;

  out->ev.type = CCORD_NET_GUILD_PROFILE;
  out->ev.guild_id = gid;
  out->ev.guild_profile = gp;
done:
  ccord_arena_free(&scratch);
  return st;
}
