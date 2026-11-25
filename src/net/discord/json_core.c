// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "json_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ccord_live_event_init(ccord_live_event *ev)
{
  memset(ev, 0, sizeof *ev);
  ccord_arena_init(&ev->arena, 1024u);
  ev->ev.type = CCORD_NET_NONE;
  ev->seq = -1;
}

void ccord_live_event_free(ccord_live_event *ev)
{
  if (!ev) return;
  if (ev->has_account_private) ccord_live_wipe(&ev->account_private, sizeof ev->account_private);
  ccord_arena_free(&ev->arena);
  memset(ev, 0, sizeof *ev);
  ev->seq = -1;
}

size_t dj_scratch_chunk_size(size_t json_len)
{
  if (json_len > 65536u) return 256u * 1024u;
  if (json_len > 4096u) return 16u * 1024u;
  return 4096u;
}

const ccord_json *dj_parse_json(ccord_arena *a, const char *text, size_t len, ccord_error *err)
{
  ccord_json_limits lim = {LIVE_JSON_MAX_DEPTH, LIVE_JSON_MAX_NODES, LIVE_JSON_MAX_STRING};
  return ccord_json_parse(a, text, len, &lim, err);
}

const char *dj_arena_dup(ccord_arena *a, const char *s)
{
  if (!s || s[0] == '\0' || !a) return "";
  char *d = ccord_arena_strndup(a, s, strlen(s));
  return d ? d : "";
}

const char *dj_arena_dup_nullable(ccord_arena *a, const char *s)
{
  if (!s || s[0] == '\0' || !a) return nullptr;
  return ccord_arena_strndup(a, s, strlen(s));
}

const char *dj_sanitize_str_inplace(char *s)
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

size_t dj_utf8_trunc_len(const char *s, size_t len, size_t max)
{
  if (!s) return 0;
  if (len <= max) return len;
  if ((((unsigned char)s[max]) & 0xC0u) != 0x80u) return max;
  return ccord_utf8_prev(s, max);
}

const char *dj_jstr(const ccord_json *obj, const char *key)
{
  if (!obj) return nullptr;
  const ccord_json *j = ccord_json_get(obj, key);
  if (!j) return nullptr;
  size_t n = 0;
  union { const char *c; char *m; } u = {.c = ccord_json_string(j, &n)};
  return dj_sanitize_str_inplace(u.m);
}

size_t dj_jarray_len(const ccord_json *obj, const char *key)
{
  if (!obj) return 0;
  const ccord_json *j = ccord_json_get(obj, key);
  return j ? ccord_json_array_len(j) : 0;
}


bool dj_json_id(const ccord_json *j, ccord_id *out)
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

bool dj_json_id_key(const ccord_json *obj, const char *key, ccord_id *out)
{
  if (!obj) return false;
  return dj_json_id(ccord_json_get(obj, key), out);
}

int64_t dj_json_i64(const ccord_json *obj, const char *key, int64_t fallback)
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

int64_t dj_iso_ms(const char *s, size_t len)
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

bool dj_text_ok(const char *s, size_t max)
{
  if (!s || s[0] == '\0') return false;
  size_t n = strlen(s);
  return n <= max && ccord_utf8_valid_text(s, n);
}

const char *dj_opt_text(const char *s, size_t max)
{
  if (!s || s[0] == '\0') return "";
  size_t n = strlen(s);
  return (n <= max && ccord_utf8_valid_text(s, n)) ? s : "";
}

const char *dj_opt_text_dup(ccord_arena *a, const char *s, size_t max)
{
  const char *ok = dj_opt_text(s, max);
  if (ok[0] == '\0') return "";
  return dj_arena_dup(a, ok);
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
    (void)dj_json_id_key(ru, "id", &ruid);
    if (count > 1 && self_id != CCORD_ID_NONE && ruid == self_id) continue;
    const char *name = dj_jstr(ru, "global_name");
    if (!name || name[0] == '\0') name = dj_jstr(ru, "username");
    const char *av = (ruid != CCORD_ID_NONE) ? dj_build_cdn_url(a, "avatars", ruid, dj_jstr(ru, "avatar")) : "";
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
      if (!dj_json_id(ccord_json_array_at(rids, i), &ruid)) continue;
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
  size_t n = dj_utf8_trunc_len(b.data, b.len, CCORD_NAME_MAX_BYTES);
  char *s = ccord_arena_strndup(a, ccord_buf_cstr(&b), n);
  ccord_buf_free(&b);
  if (out_uid) *out_uid = primary_uid;
  if (out_icon) *out_icon = primary_icon ? primary_icon : "";
  return s;
}

bool dj_map_channel_ex(ccord_arena *a, const ccord_user *users, size_t user_count, const id_index *user_idx,
                           ccord_id self_id, const ccord_json *j, ccord_id guild_hint, ccord_channel *out)
{
  if (!j || !out) return false;
  *out = (ccord_channel){0};
  ccord_id id = CCORD_ID_NONE;
  if (!dj_json_id_key(j, "id", &id)) return false;
  int64_t type = dj_json_i64(j, "type", 0);
  if (!supported_channel_type(type)) return false;
  ccord_id gid = guild_hint;
  if (gid == CCORD_ID_NONE) (void)dj_json_id_key(j, "guild_id", &gid);
  if (gid == CCORD_ID_NONE && (type == 1 || type == 3)) gid = CCORD_DM_GUILD_ID;
  size_t kept = 0, total = 0;
  ccord_id owner = CCORD_ID_NONE;
  bool msg_req = false;
  bool spam = false;
  if (gid == CCORD_DM_GUILD_ID || type == 1 || type == 3) {
    (void)dj_json_id_key(j, "owner_id", &owner);
    const ccord_json *mr_j = ccord_json_get(j, "is_message_request");
    int64_t consent = dj_json_i64(j, "consent_status", 0);
    msg_req = (consent == 2) ? false : (mr_j ? ccord_json_bool(mr_j, false) : (consent == 1));
    spam = (consent == 2) ? false : dj_json_bool_key(j, "is_spam", false);
    if (spam && consent != 2) msg_req = true;
    const ccord_json *recs = ccord_json_get(j, "recipients");
    size_t nrec = recs ? ccord_json_array_len(recs) : 0;
    for (size_t i = 0; i < nrec; i++) {
      ccord_id ruid = CCORD_ID_NONE;
      if (!dj_json_id_key(ccord_json_array_at(recs, i), "id", &ruid) || ruid == CCORD_ID_NONE) continue;
      if (self_id != CCORD_ID_NONE && ruid == self_id) continue;
      total++;
      if (kept < CCORD_MAX_DM_RECIPIENTS) out->recipient_ids[kept++] = ruid;
    }
    if (total == 0) {
      const ccord_json *rids = ccord_json_get(j, "recipient_ids");
      size_t nids = rids ? ccord_json_array_len(rids) : 0;
      for (size_t i = 0; i < nids; i++) {
        ccord_id ruid = CCORD_ID_NONE;
        if (!dj_json_id(ccord_json_array_at(rids, i), &ruid) || ruid == CCORD_ID_NONE) continue;
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
    const char *gdm_icon = dj_jstr(j, "icon");
    if (gdm_icon && gdm_icon[0] != '\0') {
      const char *cicon = dj_build_cdn_url(a, "channel-icons", id, gdm_icon);
      if (cicon && cicon[0] != '\0') dm_icon = cicon;
    }
  }
  const char *name = dj_jstr(j, "name");
  bool name_owned = false;
  if ((!name || name[0] == '\0') && resolved_dm) {
    name = resolved_dm;
    name_owned = true;
  }
  // Discord caps names at 100 *characters*; emoji and styled Unicode can push that well past
  // CCORD_NAME_MAX_BYTES. Truncate on a code point boundary instead of dropping the channel
  // (dropping it hid named group DMs and made their messages spawn a fake 1:1 DM).
  if (name && strlen(name) > CCORD_NAME_MAX_BYTES) {
    size_t cut = dj_utf8_trunc_len(name, strlen(name), CCORD_NAME_MAX_BYTES);
    name = cut > 0 ? ccord_arena_strndup(a, name, cut) : nullptr;
    name_owned = true;
  }
  if (!dj_text_ok(name, CCORD_NAME_MAX_BYTES)) {
    if (resolved_dm && dj_text_ok(resolved_dm, CCORD_NAME_MAX_BYTES)) {
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
  const char *topic = dj_jstr(j, "topic");
  if (!topic || topic[0] == '\0' || !topic_ok(topic)) topic = "";
  else topic = dj_arena_dup(a, topic);
  ccord_id parent = CCORD_ID_NONE;
  if (type != 4) (void)dj_json_id_key(j, "parent_id", &parent);
  int64_t pos = dj_json_i64(j, "position", 0);
  if (pos < (int64_t)INT32_MIN) pos = (int64_t)INT32_MIN;
  if (pos > (int64_t)INT32_MAX) pos = (int64_t)INT32_MAX;
  ccord_id lmid = CCORD_ID_NONE;
  (void)dj_json_id_key(j, "last_message_id", &lmid);
  int64_t unread = dj_json_i64(j, "unread_count", 0);
  int64_t mentions = dj_json_i64(j, "mention_count", 0);
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
  int64_t rl = dj_json_i64(j, "rate_limit_per_user", 0);
  if (rl < 0) rl = 0;
  if (rl > 21600) rl = 21600;
  out->rate_limit_per_user = (uint16_t)rl;
  const ccord_json *nsfw_j = ccord_json_get(j, "nsfw");
  out->nsfw = nsfw_j && ccord_json_bool(nsfw_j, false);
  int64_t br = dj_json_i64(j, "bitrate", 0);
  if (br < 0) br = 0;
  if (br > (int64_t)UINT32_MAX) br = (int64_t)UINT32_MAX;
  out->bitrate = (uint32_t)br;
  int64_t ul = dj_json_i64(j, "user_limit", 0);
  if (ul < 0) ul = 0;
  if (ul > (int64_t)UINT32_MAX) ul = (int64_t)UINT32_MAX;
  out->user_limit = (uint32_t)ul;
  int64_t daad = dj_json_i64(j, "default_auto_archive_duration", 0);
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
    int64_t aad = dj_json_i64(tm, "auto_archive_duration", 0);
    if (aad < 0) aad = 0;
    if (aad > UINT16_MAX) aad = UINT16_MAX;
    out->thread_auto_archive_mins = (uint16_t)aad;
  }
  const ccord_json *mem_j = ccord_json_get(j, "member");
  if (mem_j && ccord_json_type_of(mem_j) == CCORD_JSON_OBJECT) {
    out->thread_has_member = true;
    out->thread_joined = true;
  }
  int64_t tmc = dj_json_i64(j, "message_count", 0);
  if (tmc < 0) tmc = 0;
  if (tmc > (int64_t)UINT32_MAX) tmc = (int64_t)UINT32_MAX;
  out->thread_message_count = (uint32_t)tmc;
  return true;
}

bool dj_map_channel(ccord_arena *a, const ccord_json *j, ccord_id guild_hint, ccord_channel *out)
{
  return dj_map_channel_ex(a, nullptr, 0, nullptr, CCORD_ID_NONE, j, guild_hint, out);
}


static void read_clan_obj(const ccord_json *clan, char out_tag[16], ccord_id *out_guild_id, char out_badge[64])
{
  if (!clan || ccord_json_type_of(clan) != CCORD_JSON_OBJECT) return;
  const ccord_json *ie = ccord_json_get(clan, "identity_enabled");
  if (ie && ccord_json_type_of(ie) == CCORD_JSON_BOOL && !ccord_json_bool(ie, true)) return;
  if (out_tag && out_tag[0] == '\0') {
    const char *tag = dj_jstr(clan, "tag");
    if (tag && tag[0] != '\0') {
      size_t len = dj_utf8_trunc_len(tag, strlen(tag), 15);
      if (len > 0 && ccord_utf8_valid_text(tag, len)) {
        memcpy(out_tag, tag, len);
        out_tag[len] = '\0';
      }
    }
  }
  if (out_guild_id && *out_guild_id == CCORD_ID_NONE) {
    ccord_id gid = CCORD_ID_NONE;
    if (dj_json_id_key(clan, "identity_guild_id", &gid)) *out_guild_id = gid;
  }
  if (out_badge && out_badge[0] == '\0') {
    const char *badge = dj_jstr(clan, "badge");
    if (!badge || badge[0] == '\0') badge = dj_jstr(clan, "badge_hash");
    if (badge && badge[0] != '\0') {
      size_t blen = dj_utf8_trunc_len(badge, strlen(badge), 63);
      if (blen > 0 && ccord_utf8_valid_text(badge, blen)) {
        memcpy(out_badge, badge, blen);
        out_badge[blen] = '\0';
      }
    }
  }
}

void dj_extract_clan_info(const ccord_json *uj, char out_tag[16], ccord_id *out_guild_id,
                              char out_badge[64])
{
  if (out_tag) out_tag[0] = '\0';
  if (out_guild_id) *out_guild_id = CCORD_ID_NONE;
  if (out_badge) out_badge[0] = '\0';
  if (!uj) return;
  read_clan_obj(ccord_json_get(uj, "primary_guild"), out_tag, out_guild_id, out_badge);
  read_clan_obj(ccord_json_get(uj, "clan"), out_tag, out_guild_id, out_badge);
}

void dj_copy_trunc_utf8(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  dst[0] = '\0';
  if (!src || src[0] == '\0') return;
  size_t n = dj_utf8_trunc_len(src, strlen(src), cap - 1u);
  if (n > 0 && ccord_utf8_valid_text(src, n)) {
    memcpy(dst, src, n);
    dst[n] = '\0';
  }
}

// A CDN name (hash, badge icon, decoration asset) is letters, digits, '_' and '-': it cannot steer the URL path.
bool dj_cdn_name_ok(const char *s)
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
const char *dj_cdn_asset_url(ccord_arena *a, const char *path, const char *name, const char *suffix)
{
  if (!a || !path || !dj_cdn_name_ok(name)) return "";
  size_t total = strlen("https://cdn.discordapp.com/") + strlen(path) + 1u + strlen(name) + strlen(suffix);
  if (total > 512u) return "";
  char *out = ccord_arena_alloc(a, total + 1u, 1);
  if (!out) return "";
  (void)snprintf(out, total + 1u, "https://cdn.discordapp.com/%s/%s%s", path, name, suffix);
  return out;
}

// user/member.avatar_decoration_data.asset -> avatar-decoration-presets CDN URL.
const char *dj_extract_avatar_deco_url(ccord_arena *a, const ccord_json *obj)
{
  const ccord_json *adj = obj ? ccord_json_get(obj, "avatar_decoration_data") : nullptr;
  if (!adj || ccord_json_type_of(adj) != CCORD_JSON_OBJECT) return "";
  return dj_cdn_asset_url(a, "avatar-decoration-presets", dj_jstr(adj, "asset"), ".png?size=128&passthrough=true");
}

ccord_id dj_extract_avatar_deco_sku(const ccord_json *obj)
{
  const ccord_json *adj = obj ? ccord_json_get(obj, "avatar_decoration_data") : nullptr;
  if (!adj || ccord_json_type_of(adj) != CCORD_JSON_OBJECT) return CCORD_ID_NONE;
  ccord_id sku = CCORD_ID_NONE;
  (void)dj_json_id_key(adj, "sku_id", &sku);
  return sku;
}

void dj_extract_avatar_deco_buf(const ccord_json *obj, char out[256])
{
  if (!out) return;
  out[0] = '\0';
  const ccord_json *adj = obj ? ccord_json_get(obj, "avatar_decoration_data") : nullptr;
  if (!adj || ccord_json_type_of(adj) != CCORD_JSON_OBJECT) return;
  const char *asset = dj_jstr(adj, "asset");
  if (!dj_cdn_name_ok(asset)) return;
  (void)snprintf(out, 256u, "https://cdn.discordapp.com/avatar-decoration-presets/%s.png?size=128&passthrough=true",
                 asset);
}

// Safe relative asset path for collectibles.nameplate.asset (e.g. "nameplates/nameplates/american_flag/").
bool dj_nameplate_asset_ok(const char *s)
{
  if (!s || s[0] == '\0' || s[0] == '/') return false;
  size_t n = strlen(s);
  if (n > 127u) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '/';
    if (!ok) return false;
    if (c == '/' && i > 0 && s[i - 1] == '/') return false;
  }
  return true;
}

// user/member.collectibles.nameplate.{asset, palette}.
void dj_extract_nameplate_info(const ccord_json *obj, char out_asset[128], char out_palette[24])
{
  if (out_asset) out_asset[0] = '\0';
  if (out_palette) out_palette[0] = '\0';
  if (!obj) return;
  const ccord_json *col = ccord_json_get(obj, "collectibles");
  if (!col || ccord_json_type_of(col) != CCORD_JSON_OBJECT) return;
  const ccord_json *np = ccord_json_get(col, "nameplate");
  if (!np || ccord_json_type_of(np) != CCORD_JSON_OBJECT) return;
  const char *asset = dj_jstr(np, "asset");
  if (out_asset && dj_nameplate_asset_ok(asset)) {
    dj_copy_trunc_utf8(out_asset, 128u, asset);
  }
  const char *pal = dj_jstr(np, "palette");
  if (out_palette && dj_cdn_name_ok(pal)) {
    dj_copy_trunc_utf8(out_palette, 24u, pal);
  }
}

ccord_id dj_extract_nameplate_sku(const ccord_json *obj)
{
  if (!obj) return CCORD_ID_NONE;
  const ccord_json *col = ccord_json_get(obj, "collectibles");
  if (!col || ccord_json_type_of(col) != CCORD_JSON_OBJECT) return CCORD_ID_NONE;
  const ccord_json *np = ccord_json_get(col, "nameplate");
  if (!np || ccord_json_type_of(np) != CCORD_JSON_OBJECT) return CCORD_ID_NONE;
  ccord_id sku = CCORD_ID_NONE;
  (void)dj_json_id_key(np, "sku_id", &sku);
  return sku;
}

int64_t dj_json_ts_ms(const ccord_json *obj, const char *key)
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
  int64_t t = dj_iso_ms(s, slen);
  return t > 0 ? t : 0;
}

void dj_extract_presence_detail(ccord_arena *a, const ccord_json *obj, parsed_presence_detail *out)
{
  memset(out, 0, sizeof *out);
  if (!obj) return;
  const ccord_json *cs = ccord_json_get(obj, "client_status");
  if (cs && ccord_json_type_of(cs) == CCORD_JSON_OBJECT) {
    ccord_presence dp = dj_presence_from(dj_jstr(cs, "desktop"));
    ccord_presence mp = dj_presence_from(dj_jstr(cs, "mobile"));
    ccord_presence wp = dj_presence_from(dj_jstr(cs, "web"));
    out->client_status_bits = ccord_client_status_pack(dp, mp, wp);
  }
  const ccord_json *acts = ccord_json_get(obj, "activities");
  size_t na = acts ? ccord_json_array_len(acts) : 0;
  bool saw_custom = false;
  ccord_user_activity collected[CCORD_USER_MAX_ACTIVITIES];
  size_t ncoll = 0;
  for (size_t i = 0; i < na; i++) {
    const ccord_json *act = ccord_json_array_at(acts, i);
    if (!act || ccord_json_type_of(act) != CCORD_JSON_OBJECT) continue;
    int64_t type = dj_json_i64(act, "type", -1);
    if (a && ncoll < CCORD_USER_MAX_ACTIVITIES && type >= 0 && type <= 5) {
      ccord_user_activity *ua = &collected[ncoll];
      memset(ua, 0, sizeof *ua);
      ua->type = (uint8_t)type;
      dj_copy_trunc_utf8(ua->name, sizeof ua->name, dj_jstr(act, "name"));
      dj_copy_trunc_utf8(ua->details, sizeof ua->details, dj_jstr(act, "details"));
      dj_copy_trunc_utf8(ua->state, sizeof ua->state, dj_jstr(act, "state"));
      dj_copy_trunc_utf8(ua->url, sizeof ua->url, dj_jstr(act, "url"));
      (void)dj_json_id_key(act, "application_id", &ua->application_id);
      dj_copy_trunc_utf8(ua->sync_id, sizeof ua->sync_id, dj_jstr(act, "sync_id"));
      dj_copy_trunc_utf8(ua->session_id, sizeof ua->session_id, dj_jstr(act, "session_id"));
      int64_t fl = dj_json_i64(act, "flags", 0);
      if (fl > 0) ua->flags = dj_clamp_u32(fl);
      ua->created_at_ms = dj_json_ts_ms(act, "created_at");
      const ccord_json *tsj = ccord_json_get(act, "timestamps");
      if (tsj && ccord_json_type_of(tsj) == CCORD_JSON_OBJECT) {
        ua->start_ms = dj_json_ts_ms(tsj, "start");
        ua->end_ms = dj_json_ts_ms(tsj, "end");
      }
      const ccord_json *pj = ccord_json_get(act, "party");
      if (pj && ccord_json_type_of(pj) == CCORD_JSON_OBJECT) {
        dj_copy_trunc_utf8(ua->party_id, sizeof ua->party_id, dj_jstr(pj, "id"));
        const ccord_json *sz = ccord_json_get(pj, "size");
        if (sz && ccord_json_type_of(sz) == CCORD_JSON_ARRAY) {
          size_t sl = ccord_json_array_len(sz);
          int64_t v0 = 0, v1 = 0;
          if (sl >= 1 && ccord_json_int64(ccord_json_array_at(sz, 0), &v0) && v0 > 0 && v0 <= INT32_MAX) {
            ua->party_size = (int32_t)v0;
          }
          if (sl >= 2 && ccord_json_int64(ccord_json_array_at(sz, 1), &v1) && v1 > 0 && v1 <= INT32_MAX) {
            ua->party_max = (int32_t)v1;
          }
        }
      }
      const ccord_json *aj = ccord_json_get(act, "assets");
      if (aj && ccord_json_type_of(aj) == CCORD_JSON_OBJECT) {
        dj_copy_trunc_utf8(ua->large_image, sizeof ua->large_image, dj_jstr(aj, "large_image"));
        dj_copy_trunc_utf8(ua->large_text, sizeof ua->large_text, dj_jstr(aj, "large_text"));
        dj_copy_trunc_utf8(ua->small_image, sizeof ua->small_image, dj_jstr(aj, "small_image"));
        dj_copy_trunc_utf8(ua->small_text, sizeof ua->small_text, dj_jstr(aj, "small_text"));
      }
      const ccord_json *ej = ccord_json_get(act, "emoji");
      if (ej && ccord_json_type_of(ej) == CCORD_JSON_OBJECT) {
        (void)dj_json_id_key(ej, "id", &ua->emoji_id);
        dj_copy_trunc_utf8(ua->emoji_name, sizeof ua->emoji_name, dj_jstr(ej, "name"));
        ua->emoji_animated = dj_json_bool_key(ej, "animated", false);
      }
      ncoll++;
    }
    if (type == 4 && !saw_custom) {
      saw_custom = true;
      dj_copy_trunc_utf8(out->custom_status_text, sizeof out->custom_status_text, dj_jstr(act, "state"));
      const ccord_json *ej = ccord_json_get(act, "emoji");
      if (ej && ccord_json_type_of(ej) == CCORD_JSON_OBJECT) {
        const char *ename = dj_jstr(ej, "name");
        ccord_id eid = CCORD_ID_NONE;
        (void)dj_json_id_key(ej, "id", &eid);
        if (eid != CCORD_ID_NONE && ename && ename[0] != '\0') {
          char nbuf[44];
          dj_copy_trunc_utf8(nbuf, sizeof nbuf, ename);
          if (nbuf[0] != '\0') {
            char ebuf[96];
            snprintf(ebuf, sizeof ebuf, "%s:%llu", nbuf, (unsigned long long)eid);
            dj_copy_trunc_utf8(out->custom_status_emoji, sizeof out->custom_status_emoji, ebuf);
          }
        } else {
          dj_copy_trunc_utf8(out->custom_status_emoji, sizeof out->custom_status_emoji, ename);
        }
      }
      const ccord_json *tsj = ccord_json_get(act, "timestamps");
      if (tsj && ccord_json_type_of(tsj) == CCORD_JSON_OBJECT) {
        out->custom_status_expires_ms = dj_json_ts_ms(tsj, "end");
      }
      continue;
    }
    if (!out->has_activity && (type == 0 || type == 1 || type == 2 || type == 3 || type == 5)) {
      const char *name = dj_jstr(act, "name");
      char nbuf[CCORD_ACTIVITY_TEXT_MAX_BYTES + 1];
      dj_copy_trunc_utf8(nbuf, sizeof nbuf, name);
      if (nbuf[0] == '\0') continue;
      out->has_activity = true;
      out->activity_type = (ccord_activity_type)type;
      memcpy(out->activity_name, nbuf, sizeof out->activity_name);
      dj_copy_trunc_utf8(out->activity_details, sizeof out->activity_details, dj_jstr(act, "details"));
      dj_copy_trunc_utf8(out->activity_state, sizeof out->activity_state, dj_jstr(act, "state"));
      const ccord_json *tsj = ccord_json_get(act, "timestamps");
      if (tsj && ccord_json_type_of(tsj) == CCORD_JSON_OBJECT) {
        out->activity_start_ms = dj_json_ts_ms(tsj, "start");
        out->activity_end_ms = dj_json_ts_ms(tsj, "end");
      }
    }
  }
  if (a && ncoll > 0) {
    size_t sz = sizeof(ccord_user_activities) + ncoll * sizeof(ccord_user_activity);
    ccord_user_activities *ext = ccord_arena_alloc(a, sz, _Alignof(ccord_user_activities));
    if (ext) {
      ext->count = ncoll;
      memcpy(ext->items, collected, ncoll * sizeof(ccord_user_activity));
      out->activities_ext = ext;
    }
  }
}

void dj_apply_presence_detail_to_user(ccord_user *u, const parsed_presence_detail *pd)
{
  if (!u || !pd) return;
  u->has_presence = true;
  u->client_status_bits = pd->client_status_bits;
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
  u->activities_ext = pd->activities_ext;
}

const char *dj_extract_activity_status(ccord_arena *a, const ccord_json *obj)
{
  if (!obj) return "";
  const ccord_json *acts = ccord_json_get(obj, "activities");
  size_t na = acts ? ccord_json_array_len(acts) : 0;
  for (size_t i = 0; i < na; i++) {
    const ccord_json *act = ccord_json_array_at(acts, i);
    if (dj_json_i64(act, "type", -1) != 4) continue;
    const char *state = dj_jstr(act, "state");
    if (dj_text_ok(state, LIVE_STATUS_MAX_BYTES)) return dj_arena_dup(a, state);
  }
  for (size_t i = 0; i < na; i++) {
    const ccord_json *act = ccord_json_array_at(acts, i);
    int64_t type = dj_json_i64(act, "type", -1);
    const char *name = dj_jstr(act, "name");
    if (!dj_text_ok(name, LIVE_STATUS_MAX_BYTES)) continue;
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
    size_t clen = dj_utf8_trunc_len(buf, plen + nlen, LIVE_STATUS_MAX_BYTES);
    const char *dup = ccord_arena_strndup(a, buf, clen);
    if (dup && ccord_utf8_valid_text(dup, clen)) return dup;
  }
  return "";
}


bool dj_map_voice_state(const ccord_json *v, ccord_id guild_hint, ccord_voice_state *out)
{
  if (!v) return false;
  ccord_id guild = guild_hint, channel = CCORD_ID_NONE, user = CCORD_ID_NONE;
  (void)dj_json_id_key(v, "guild_id", &guild);
  (void)dj_json_id_key(v, "channel_id", &channel);
  if (!dj_json_id_key(v, "user_id", &user)) return false;
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
      int64_t ms = dj_iso_ms(rs, rn);
      if (ms > 0) out->request_to_speak_timestamp = ms;
    }
  }
  return true;
}


ccord_relationship_kind dj_rel_kind_from(int64_t type)
{
  switch (type) {
    case 1: return CCORD_REL_FRIEND;
    case 2: return CCORD_REL_BLOCKED;
    case 3: return CCORD_REL_INCOMING_REQUEST;
    case 4: return CCORD_REL_OUTGOING_REQUEST;
    default: return CCORD_REL_NONE;
  }
}

bool dj_map_user_obj(ccord_arena *a, const ccord_json *uj, ccord_presence pres, ccord_user *out)
{
  if (!uj || !out) return false;
  ccord_id uid = CCORD_ID_NONE;
  if (!dj_json_id_key(uj, "id", &uid)) return false;
  const char *name = dj_jstr(uj, "global_name");
  if (!name || name[0] == '\0') name = dj_jstr(uj, "username");
  if (!dj_text_ok(name, CCORD_NAME_MAX_BYTES)) name = "User";
  else name = dj_arena_dup(a, name);
  const ccord_json *botj = ccord_json_get(uj, "bot");
  const ccord_json *acj = ccord_json_get(uj, "accent_color");
  bool has_accent = acj && ccord_json_type_of(acj) == CCORD_JSON_NUMBER;
  int64_t accent = has_accent ? dj_json_i64(uj, "accent_color", 0) : 0;
  if (accent < 0 || accent > (int64_t)0xFFFFFF) {
    accent = 0;
    has_accent = false;
  }
  *out = (ccord_user){
    .id = uid,
    .name = name,
    .handle = dj_opt_text_dup(a, dj_jstr(uj, "username"), CCORD_NAME_MAX_BYTES),
    .status_text = "",
    .avatar_url = dj_build_cdn_url(a, "avatars", uid, dj_jstr(uj, "avatar")),
    .avatar_decoration_url = dj_extract_avatar_deco_url(a, uj),
    .avatar_decoration_sku_id = dj_extract_avatar_deco_sku(uj),
    .banner_url = dj_build_cdn_url(a, "banners", uid, dj_jstr(uj, "banner")),
    .accent_color = (uint32_t)accent,
    .has_accent_color = has_accent,
    .bio = dj_opt_text_dup(a, dj_jstr(uj, "bio"), CCORD_USER_BIO_MAX_BYTES),
    .pronouns = dj_opt_text_dup(a, dj_jstr(uj, "pronouns"), CCORD_USER_PRONOUNS_MAX_BYTES),
    .presence = pres,
    .is_bot = botj && ccord_json_bool(botj, false),
    .avatar_palette = (uint8_t)(uid % 6u),
  };
  dj_extract_clan_info(uj, out->clan_tag, &out->clan_guild_id, out->clan_badge);
  dj_extract_nameplate_info(uj, out->nameplate_asset, out->nameplate_palette);
  out->nameplate_sku_id = dj_extract_nameplate_sku(uj);
  return true;
}


ccord_presence dj_presence_from(const char *status)
{
  if (status) {
    if (strcmp(status, "online") == 0) return CCORD_PRESENCE_ONLINE;
    if (strcmp(status, "idle") == 0) return CCORD_PRESENCE_IDLE;
    if (strcmp(status, "dnd") == 0) return CCORD_PRESENCE_DND;
  }
  return CCORD_PRESENCE_OFFLINE;
}


bool dj_json_bool_key(const ccord_json *obj, const char *key, bool fallback)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  return j ? ccord_json_bool(j, fallback) : fallback;
}


void dj_copy_json_str(char *dst, size_t cap, const ccord_json *obj, const char *key)
{
  if (!dst || cap == 0) return;
  dst[0] = '\0';
  const char *s = dj_jstr(obj, key);
  if (!s || s[0] == '\0') return;
  size_t n = dj_utf8_trunc_len(s, strlen(s), cap - 1u);
  memcpy(dst, s, n);
  dst[n] = '\0';
}


static const char *first_error_detail(const ccord_json *node, int depth)
{
  if (!node || depth > 6 || ccord_json_type_of(node) != CCORD_JSON_OBJECT) return nullptr;
  const ccord_json *errs = ccord_json_get(node, "_errors");
  if (errs && ccord_json_array_len(errs) > 0) {
    const char *m = dj_jstr(ccord_json_array_at(errs, 0), "message");
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
  const ccord_json *root = dj_parse_json(&arena, json, len, &perr);
  bool ok = false;
  if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    const char *msg = dj_jstr(root, "message");
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


static bool dj_copy_exact_token(const ccord_json *root, const char *key, char *dst, size_t cap)
{
  if (!dst || cap == 0) return false;
  dst[0] = '\0';
  const ccord_json *j = ccord_json_get(root, key);
  if (!j || ccord_json_type_of(j) != CCORD_JSON_STRING) return false;
  size_t n = 0;
  const char *s = ccord_json_string(j, &n);
  if (!s || n == 0 || n >= cap) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20u || c == 0x7Fu) return false;
  }
  memcpy(dst, s, n);
  dst[n] = '\0';
  return true;
}

static bool dj_extract_captcha_obj(const ccord_json *root, ccord_captcha_challenge *out)
{
  if (out) memset(out, 0, sizeof *out);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) return false;
  bool has_cap = ccord_json_get(root, "captcha_key") != nullptr ||
                 ccord_json_get(root, "captcha_sitekey") != nullptr;
  if (!has_cap) return false;
  if (!out) return true;
  out->required = true;
  if (!dj_copy_exact_token(root, "captcha_service", out->service, sizeof out->service)) {
    snprintf(out->service, sizeof out->service, "hcaptcha");
  }
  (void)dj_copy_exact_token(root, "captcha_sitekey", out->sitekey, sizeof out->sitekey);
  (void)dj_copy_exact_token(root, "captcha_rqdata", out->rqdata, sizeof out->rqdata);
  (void)dj_copy_exact_token(root, "captcha_rqtoken", out->rqtoken, sizeof out->rqtoken);
  (void)dj_copy_exact_token(root, "captcha_session_id", out->session_id, sizeof out->session_id);
  return true;
}

bool ccord_live_json_parse_captcha(const char *json, size_t len, ccord_captcha_challenge *out)
{
  if (out) memset(out, 0, sizeof *out);
  if (!json || len == 0) return false;
  ccord_arena arena;
  ccord_arena_init(&arena, dj_scratch_chunk_size(len));
  ccord_error err = {0};
  const ccord_json *root = dj_parse_json(&arena, json, len, &err);
  bool ok = dj_extract_captcha_obj(root, out);
  ccord_arena_free(&arena);
  return ok;
}

bool ccord_live_json_parse_fingerprint(const char *json, size_t len, char *out, size_t cap)
{
  if (out && cap > 0) out[0] = '\0';
  if (!json || len == 0 || !out || cap < 2) return false;
  ccord_arena arena;
  ccord_arena_init(&arena, dj_scratch_chunk_size(len));
  ccord_error err = {0};
  const ccord_json *root = dj_parse_json(&arena, json, len, &err);
  bool ok = root && ccord_json_type_of(root) == CCORD_JSON_OBJECT &&
            dj_copy_exact_token(root, "fingerprint", out, cap);
  ccord_arena_free(&arena);
  return ok;
}

ccord_status ccord_live_json_auth_response_ex(const char *json, size_t len, long http_status,
                                              char *out_token, size_t token_cap,
                                              char *out_mfa_ticket, size_t ticket_cap,
                                              ccord_captcha_challenge *out_captcha, ccord_error *err)
{
  ccord_error_clear(err);
  if (out_captcha) memset(out_captcha, 0, sizeof *out_captcha);
  if (!out_token || token_cap < 2) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "auth: invalid token buffer");
  }
  out_token[0] = '\0';
  if (out_mfa_ticket && ticket_cap > 0) out_mfa_ticket[0] = '\0';
  if (!json || len == 0) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "auth: empty response (HTTP %ld)", http_status);
  }
  ccord_arena arena;
  ccord_arena_init(&arena, dj_scratch_chunk_size(len));
  const ccord_json *root = dj_parse_json(&arena, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return ccord_error_set(err, CCORD_ERR_PARSE, "auth: invalid response JSON");
  }
  if (http_status < 200 || http_status >= 300) {
    if (dj_extract_captcha_obj(root, out_captcha)) {
      ccord_error_set(err, CCORD_ERR_PERMISSION, "auth: CAPTCHA verification required");
      ccord_arena_free(&arena);
      return CCORD_ERR_PERMISSION;
    }
    const char *msg = dj_jstr(root, "message");
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
  const char *tok = dj_jstr(root, "token");
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
  const char *ticket = dj_jstr(root, "ticket");
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
  const char *msg = dj_jstr(root, "message");
  ccord_error_set(err, CCORD_ERR_PERMISSION, "auth: %s", (msg && msg[0]) ? msg : "missing token in response");
  ccord_arena_free(&arena);
  return CCORD_ERR_PERMISSION;
}

ccord_status ccord_live_json_auth_response(const char *json, size_t len, long http_status,
                                           char *out_token, size_t token_cap,
                                           char *out_mfa_ticket, size_t ticket_cap, ccord_error *err)
{
  return ccord_live_json_auth_response_ex(json, len, http_status, out_token, token_cap,
                                          out_mfa_ticket, ticket_cap, nullptr, err);
}

void dj_extract_account_private(const ccord_json *u, ccord_account_private *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!u || ccord_json_type_of(u) != CCORD_JSON_OBJECT) return;
  bool any = ccord_json_get(u, "email") != nullptr ||
             ccord_json_get(u, "phone") != nullptr ||
             ccord_json_get(u, "verified") != nullptr ||
             ccord_json_get(u, "mfa_enabled") != nullptr ||
             ccord_json_get(u, "authenticator_types") != nullptr ||
             ccord_json_get(u, "nsfw_allowed") != nullptr ||
             ccord_json_get(u, "premium_type") != nullptr;
  if (!any) return;
  out->valid = true;
  dj_copy_trunc_utf8(out->email, sizeof out->email, dj_jstr(u, "email"));
  dj_copy_trunc_utf8(out->phone, sizeof out->phone, dj_jstr(u, "phone"));
  out->verified = dj_json_bool_key(u, "verified", false);
  out->mfa_enabled = dj_json_bool_key(u, "mfa_enabled", false);
  out->nsfw_allowed = dj_json_bool_key(u, "nsfw_allowed", false);
  out->premium_type = dj_json_u8_clamped(u, "premium_type", 3u, 0u);
  const ccord_json *at = ccord_json_get(u, "authenticator_types");
  size_t nat = at && ccord_json_type_of(at) == CCORD_JSON_ARRAY ? ccord_json_array_len(at) : 0;
  for (size_t i = 0; i < nat; i++) {
    int64_t v = 0;
    if (!ccord_json_int64(ccord_json_array_at(at, i), &v)) continue;
    if (v == 1) out->authenticator_types |= CCORD_AUTH_TYPE_WEBAUTHN;
    else if (v == 2) out->authenticator_types |= CCORD_AUTH_TYPE_TOTP;
    else if (v == 3) {
      out->authenticator_types |= CCORD_AUTH_TYPE_SMS;
      out->sms_mfa_enabled = true;
    }
  }
  if (out->mfa_enabled && out->authenticator_types == 0) {
    // Legacy payloads that omit authenticator_types while mfa_enabled=true default to TOTP.
    out->authenticator_types = CCORD_AUTH_TYPE_TOTP;
  }
}

static bool raw_key_is_private_pii(const char *k, size_t kl)
{
  static const char *const k_pii[] = {
    "email", "phone", "mfa_enabled", "authenticator_types", "verified",
    "purchased_flags", "premium_usage_flags", "token", "session_id", "nsfw_allowed",
  };
  for (size_t i = 0; i < sizeof k_pii / sizeof k_pii[0]; i++) {
    size_t nl = strlen(k_pii[i]);
    if (kl == nl && memcmp(k, k_pii[i], nl) == 0) return true;
  }
  return false;
}

static bool raw_skip_json_value(const char *s, size_t len, size_t *io)
{
  size_t i = *io;
  while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
  if (i >= len) return false;
  if (s[i] == '{' || s[i] == '[') {
    int depth = 1;
    bool in_s = false, esc = false;
    i++;
    while (i < len && depth > 0) {
      char c = s[i++];
      if (in_s) {
        if (esc) esc = false;
        else if (c == '\\') esc = true;
        else if (c == '"') in_s = false;
      } else if (c == '"') {
        in_s = true;
      } else if (c == '{' || c == '[') {
        depth++;
      } else if (c == '}' || c == ']') {
        depth--;
      }
    }
    if (depth != 0) return false;
  } else if (s[i] == '"') {
    i++;
    while (i < len && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < len) i++;
      i++;
    }
    if (i >= len) return false;
    i++;
  } else {
    size_t start = i;
    while (i < len && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') {
      i++;
    }
    if (i == start) return false;
  }
  *io = i;
  return true;
}

bool ccord_live_gateway_redact_raw(const char *t_name, const char *d_json, size_t d_len, ccord_buf *out)
{
  if (!t_name || !t_name[0] || !d_json || d_len == 0 || !out) return false;
  if (strcmp(t_name, "READY") == 0 ||
      strcmp(t_name, "READY_SUPPLEMENTAL") == 0 ||
      strcmp(t_name, "AUTH_SESSION_CHANGE") == 0 ||
      strcmp(t_name, "SESSIONS_REPLACE") == 0 ||
      strcmp(t_name, "AUTHENTICATOR_UPDATE") == 0 ||
      strcmp(t_name, "USER_REQUIRED_ACTION_UPDATE") == 0 ||
      strcmp(t_name, "USER_SETTINGS_UPDATE") == 0 ||
      strcmp(t_name, "USER_SETTINGS_PROTO_UPDATE") == 0 ||
      strncmp(t_name, "SAFETY_HUB_", 11) == 0) {
    return false;
  }
  if (strcmp(t_name, "USER_UPDATE") != 0) {
    return ccord_buf_append(out, d_json, d_len) == CCORD_OK;
  }
  // USER_UPDATE carries email, phone, mfa_enabled, authenticator_types, etc.
  // Rebuild top-level JSON object omitting every sensitive field.
  size_t i = 0;
  while (i < d_len && (d_json[i] == ' ' || d_json[i] == '\t' || d_json[i] == '\r' || d_json[i] == '\n')) i++;
  if (i >= d_len || d_json[i] != '{') return false;
  i++;
  if (ccord_buf_append_str(out, "{") != CCORD_OK) return false;
  bool first = true;
  while (i < d_len) {
    while (i < d_len && (d_json[i] == ' ' || d_json[i] == '\t' || d_json[i] == '\r' || d_json[i] == '\n' || d_json[i] == ',')) i++;
    if (i >= d_len || d_json[i] == '}') break;
    if (d_json[i] != '"') return false;
    size_t kq_start = i++;
    size_t ks = i;
    while (i < d_len && d_json[i] != '"') {
      if (d_json[i] == '\\' && i + 1 < d_len) i++;
      i++;
    }
    if (i >= d_len) return false;
    size_t kl = i - ks;
    i++; // closing '"'
    size_t kq_end = i;
    while (i < d_len && (d_json[i] == ' ' || d_json[i] == '\t' || d_json[i] == '\r' || d_json[i] == '\n')) i++;
    if (i >= d_len || d_json[i] != ':') return false;
    i++;
    while (i < d_len && (d_json[i] == ' ' || d_json[i] == '\t' || d_json[i] == '\r' || d_json[i] == '\n')) i++;
    size_t vs = i;
    if (!raw_skip_json_value(d_json, d_len, &i)) return false;
    size_t ve = i;
    if (raw_key_is_private_pii(d_json + ks, kl)) continue;
    if (!first && ccord_buf_append_str(out, ",") != CCORD_OK) return false;
    first = false;
    if (ccord_buf_append(out, d_json + kq_start, kq_end - kq_start) != CCORD_OK ||
        ccord_buf_append_str(out, ":") != CCORD_OK ||
        ccord_buf_append(out, d_json + vs, ve - vs) != CCORD_OK) {
      return false;
    }
  }
  return ccord_buf_append_str(out, "}") == CCORD_OK;
}


