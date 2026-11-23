// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "json_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ccord/core/base64.h>

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
    if (dj_json_i64(e, "read_state_type", 0) != 0) continue;
    ccord_id cid = CCORD_ID_NONE;
    if (!dj_json_id_key(e, "id", &cid)) continue;
    size_t k = 0;
    if (!ccord_idmap_find(&b->chan_idx, cid, &k) || k >= b->channel_count) continue;
    if (b->channels[k].kind != CCORD_CHANNEL_TEXT) continue;
    int64_t mc = dj_json_i64(e, "mention_count", 0);
    int64_t uc = dj_json_i64(e, "unread_count", -1);
    if (mc < 0) mc = 0;
    if (mc > (int64_t)UINT32_MAX) mc = (int64_t)UINT32_MAX;
    ccord_id ack_id = CCORD_ID_NONE;
    (void)dj_json_id_key(e, "last_message_id", &ack_id);
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

static uint8_t *b64_decode_alloc(const char *in, size_t in_len, size_t *out_len)
{
  if (out_len) *out_len = 0;
  if (!in || in_len == 0) return nullptr;
  return ccord_base64_decode_alloc(in, in_len, out_len);
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
      size_t n = dj_utf8_trunc_len(folder_name, strlen(folder_name), sizeof g->folder_name - 1u);
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
  *ok = dj_map_guild_ex(b, gj, mj);
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
      size_t n = dj_utf8_trunc_len(s, (size_t)slen, 63u);
      if (ccord_utf8_valid_text(s, n)) {
        memcpy(out, s, n);
        out[n] = '\0';
      }
      return;
    }
  }
  const char *s = (const char *)sub;
  size_t n = dj_utf8_trunc_len(s, nsub, 63u);
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
  if (dj_json_id_key(fj, "id", &fid) && fid != CCORD_DM_GUILD_ID) *out_fid = fid;
  const char *fname = dj_jstr(fj, "name");
  if (fname && fname[0] != '\0') {
    size_t n = dj_utf8_trunc_len(fname, strlen(fname), 63u);
    if (ccord_utf8_valid_text(fname, n)) {
      memcpy(out_name, fname, n);
      out_name[n] = '\0';
    }
  }
  int64_t col = dj_json_i64(fj, "color", 0);
  if (col > 0 && col <= 0xFFFFFF) *out_color = (uint32_t)col;
  if (*out_fid == CCORD_ID_NONE && (ngids > 1 || out_name[0] != '\0' || *out_color != 0)) {
    *out_fid = (ccord_id)(0xF01DE000ULL + (uint64_t)folder_seq + 1ULL);
  }
}

// Bounded integer field: absent/invalid keeps the fallback, out-of-range values clamp to [0, max].
uint8_t dj_json_u8_clamped(const ccord_json *obj, const char *key, uint8_t max, uint8_t fallback)
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
  s.default_guilds_restricted = dj_json_bool_key(d, "default_guilds_restricted", s.default_guilds_restricted);
  s.explicit_content_filter = dj_json_u8_clamped(d, "explicit_content_filter", 2u, s.explicit_content_filter);
  s.friend_source_all = dj_json_bool_key(d, "friend_source_all", s.friend_source_all);
  s.friend_source_mutual_friends = dj_json_bool_key(d, "friend_source_mutual_friends", s.friend_source_mutual_friends);
  s.friend_source_mutual_guilds = dj_json_bool_key(d, "friend_source_mutual_guilds", s.friend_source_mutual_guilds);
  const ccord_json *fsf = ccord_json_get(d, "friend_source_flags");
  if (fsf && ccord_json_type_of(fsf) == CCORD_JSON_OBJECT) {
    s.friend_source_all = dj_json_bool_key(fsf, "all", s.friend_source_all);
    s.friend_source_mutual_friends = dj_json_bool_key(fsf, "mutual_friends", s.friend_source_mutual_friends);
    s.friend_source_mutual_guilds = dj_json_bool_key(fsf, "mutual_guilds", s.friend_source_mutual_guilds);
  }
  s.allow_access_to_age_restricted_servers =
    dj_json_bool_key(d, "allow_access_to_age_restricted_servers", s.allow_access_to_age_restricted_servers);
  s.developer_mode = dj_json_bool_key(d, "developer_mode", s.developer_mode);
  s.gif_auto_play = dj_json_bool_key(d, "gif_auto_play", s.gif_auto_play);
  s.animate_emoji = dj_json_bool_key(d, "animate_emoji", s.animate_emoji);
  s.enable_tts_command = dj_json_bool_key(d, "enable_tts_command", s.enable_tts_command);
  s.stream_notifications_enabled =
    dj_json_bool_key(d, "stream_notifications_enabled", s.stream_notifications_enabled);
  s.message_display_compact = dj_json_bool_key(d, "message_display_compact", s.message_display_compact);
  s.animate_stickers = dj_json_u8_clamped(d, "animate_stickers", 2u, s.animate_stickers);
  dj_copy_trunc_utf8(s.locale, sizeof s.locale, dj_jstr(d, "locale"));
  if (s.locale[0] == '\0') memcpy(s.locale, "en-US", sizeof "en-US");
  ev->user_settings = s;
  ev->has_user_settings = true;
}

bool dj_map_user_settings_custom_status(ccord_live_event *ev, const ccord_json *d)
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
  dj_copy_trunc_utf8(ev->ev.custom_status_text, sizeof ev->ev.custom_status_text, dj_jstr(cs, "text"));
  dj_copy_trunc_utf8(ev->ev.custom_status_emoji, sizeof ev->ev.custom_status_emoji, dj_jstr(cs, "emoji_name"));
  ccord_id cs_eid = CCORD_ID_NONE;
  (void)dj_json_id_key(cs, "emoji_id", &cs_eid);
  if (cs_eid != CCORD_ID_NONE && ev->ev.custom_status_emoji[0] != '\0') {   // the store keeps a custom emoji as name:id
    char nbuf[44];
    dj_copy_trunc_utf8(nbuf, sizeof nbuf, ev->ev.custom_status_emoji);
    snprintf(ev->ev.custom_status_emoji, sizeof ev->ev.custom_status_emoji, "%s:%llu", nbuf, (unsigned long long)cs_eid);
  }
  ev->ev.custom_status_expires_ms = dj_json_ts_ms(cs, "expires_at");
  if (ev->ev.custom_status_text[0] == '\0' && ev->ev.custom_status_emoji[0] == '\0') {
    ev->ev.custom_status_clear = true;
  }
  return true;
}

bool dj_map_user_settings_folders(ccord_live_event *ev, const ccord_json *d, bool require_folders)
{
  if (!d || ccord_json_type_of(d) != CCORD_JSON_OBJECT) return false;
  map_user_settings_model(ev, d);
  const ccord_json *gf = ccord_json_get(d, "guild_folders");
  if (!gf || ccord_json_type_of(gf) != CCORD_JSON_ARRAY) {
    if (require_folders) return false;
    (void)dj_map_user_settings_custom_status(ev, d);
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
      if (!dj_json_id(ccord_json_array_at(gids, k), &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) continue;
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

bool dj_map_user_settings_proto_update(ccord_live_event *ev, const ccord_json *d)
{
  if (!d || ccord_json_type_of(d) != CCORD_JSON_OBJECT) return false;
  const ccord_json *settings = ccord_json_get(d, "settings");
  if (!settings || ccord_json_type_of(settings) != CCORD_JSON_OBJECT) return true;
  int64_t proto_type = dj_json_i64(settings, "type", 0);
  if (proto_type != 1 && proto_type != 2) return true;
  const char *proto_b64 = dj_jstr(settings, "proto");
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
    ev->ev.settings_proto1_partial = dj_json_bool_key(d, "partial", false);
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
  ev->ev.settings_proto1_partial = dj_json_bool_key(d, "partial", false);
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

void dj_map_sessions_replace(ccord_live_event *ev, const ccord_json *d)
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
    const char *sid = sj ? dj_jstr(sj, "session_id") : nullptr;
    if (sid && strcmp(sid, "all") == 0) pick = sj;
  }
  if (pick) {
    const char *status = dj_jstr(pick, "status");
    if (session_status_rank(status) < 0) return;
    ev->ev.type = CCORD_NET_SESSIONS_REPLACE;
    ev->ev.presence = dj_presence_from(status);
    const ccord_json *acts = ccord_json_get(pick, "activities");
    if (acts && ccord_json_type_of(acts) == CCORD_JSON_ARRAY) {
      parsed_presence_detail pd;
      dj_extract_presence_detail(pick, &pd);
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
    const char *st = dj_jstr(sj, "status");
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
        dj_extract_presence_detail(sj, &cand);
        if (!act_pick || (!best_pd.has_activity && cand.has_activity)) {
          act_pick = sj;
          best_pd = cand;
        }
      }
    }
  }
  if (!best_status_sj) return;
  ev->ev.type = CCORD_NET_SESSIONS_REPLACE;
  ev->ev.presence = dj_presence_from(dj_jstr(best_status_sj, "status"));
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

bool dj_parse_ready(ccord_live_event *ev, const ccord_json *d, ccord_error *err)
{
  if (!d || ccord_json_type_of(d) != CCORD_JSON_OBJECT) return false;
  const ccord_json *me = ccord_json_get(d, "user");
  ccord_id self = CCORD_ID_NONE;
  if (!me || !dj_json_id_key(me, "id", &self)) {
    ccord_error_set(err, CCORD_ERR_PARSE, "gateway: READY without user");
    return false;
  }
  const char *uname = dj_jstr(me, "global_name");
  if (!uname || uname[0] == '\0') uname = dj_jstr(me, "username");
  if (!dj_text_ok(uname, CCORD_NAME_MAX_BYTES)) uname = "You";
  else {
    uname = dj_arena_dup(&ev->arena, uname);
    if (uname[0] == '\0') uname = "You";
  }
  ev->session_id = dj_arena_dup_nullable(&ev->arena, dj_jstr(d, "session_id"));
  ev->resume_gateway_url = dj_arena_dup_nullable(&ev->arena, dj_jstr(d, "resume_gateway_url"));
  // M6 account settings ride READY; the snapshot event keeps ev.type == CCORD_NET_SNAPSHOT.
  const ccord_json *uset = ccord_json_get(d, "user_settings");
  map_user_settings_model(ev, uset);
  {   // READY's type-1 proto is the base a later status write patches (the snapshot itself ignores it here)
    const char *p1 = dj_jstr(d, "user_settings_proto");
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
  const char *self_bio = dj_jstr(me, "bio");
  if ((!self_bio || self_bio[0] == '\0') && prof) self_bio = dj_jstr(prof, "bio");
  const char *self_pronouns = dj_jstr(me, "pronouns");
  if ((!self_pronouns || self_pronouns[0] == '\0') && prof) self_pronouns = dj_jstr(prof, "pronouns");
  const char *self_banner = dj_jstr(me, "banner");
  if ((!self_banner || self_banner[0] == '\0') && prof) self_banner = dj_jstr(prof, "banner");
  const ccord_json *acj = ccord_json_get(me, "accent_color");
  bool has_self_accent = acj && ccord_json_type_of(acj) == CCORD_JSON_NUMBER;
  int64_t self_accent = has_self_accent ? dj_json_i64(me, "accent_color", 0) : -1;
  if (!has_self_accent && prof) {
    const ccord_json *pac = ccord_json_get(prof, "accent_color");
    if (pac && ccord_json_type_of(pac) == CCORD_JSON_NUMBER) {
      has_self_accent = true;
      self_accent = dj_json_i64(prof, "accent_color", 0);
    }
  }
  if (self_accent < 0 || self_accent > (int64_t)0xFFFFFF) {
    self_accent = 0;
    if (!has_self_accent) {
      uint32_t bcol = 0;
      if (dj_parse_hex_color(dj_jstr(me, "banner_color"), &bcol) ||
          (prof && dj_parse_hex_color(dj_jstr(prof, "banner_color"), &bcol))) {
        self_accent = (int64_t)bcol;
        has_self_accent = true;
      }
    }
  }
  ccord_user self_user = {
    .id = self,
    .name = uname,
    .handle = dj_opt_text_dup(&ev->arena, dj_jstr(me, "username"), CCORD_NAME_MAX_BYTES),
    .status_text = "",
    .avatar_url = dj_build_cdn_url(&ev->arena, "avatars", self, dj_jstr(me, "avatar")),
    .avatar_decoration_url = dj_extract_avatar_deco_url(&ev->arena, me),
    .avatar_decoration_sku_id = dj_extract_avatar_deco_sku(me),
    .banner_url = dj_build_cdn_url(&ev->arena, "banners", self, self_banner),
    .accent_color = (uint32_t)self_accent,
    .has_accent_color = has_self_accent,
    .bio = dj_opt_text_dup(&ev->arena, self_bio, CCORD_USER_BIO_MAX_BYTES),
    .pronouns = dj_opt_text_dup(&ev->arena, self_pronouns, CCORD_USER_PRONOUNS_MAX_BYTES),
    .presence = CCORD_PRESENCE_ONLINE,
    .is_bot = is_bot,
    .avatar_palette = (uint8_t)(self % 6u),
  };
  dj_extract_clan_info(me, self_user.clan_tag, &self_user.clan_guild_id, self_user.clan_badge);
  dj_extract_nameplate_info(me, self_user.nameplate_asset, self_user.nameplate_palette);
  self_user.nameplate_sku_id = dj_extract_nameplate_sku(me);
  {
    const ccord_json *cs_init = uset ? ccord_json_get(uset, "custom_status") : nullptr;
    if (cs_init && ccord_json_type_of(cs_init) == CCORD_JSON_OBJECT) {
      dj_copy_trunc_utf8(self_user.custom_status_text, sizeof self_user.custom_status_text, dj_jstr(cs_init, "text"));
      dj_copy_trunc_utf8(self_user.custom_status_emoji, sizeof self_user.custom_status_emoji, dj_jstr(cs_init, "emoji_name"));
      self_user.custom_status_expires_ms = dj_json_ts_ms(cs_init, "expires_at");
      const char *cs_st = dj_opt_text_dup(&ev->arena, dj_jstr(cs_init, "text"), LIVE_STATUS_MAX_BYTES);
      if (cs_st[0] != '\0') self_user.status_text = cs_st;
    }
  }
  bool ok = dj_sb_user(&b, &self_user);
  const ccord_json *users_arr = ccord_json_get(d, "users");
  size_t nu = users_arr ? ccord_json_array_len(users_arr) : 0;
  for (size_t i = 0; ok && i < nu; i++) {
    ccord_user top_u;
    if (dj_map_user_obj(&ev->arena, ccord_json_array_at(users_arr, i), CCORD_PRESENCE_OFFLINE, &top_u)) {
      ok = dj_sb_user(&b, &top_u);
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
      if (dj_map_user_obj(&ev->arena, ccord_json_array_at(recs, r), CCORD_PRESENCE_OFFLINE, &dm_u)) {
        ok = dj_sb_user(&b, &dm_u);
      }
    }
  }
  size_t nrel = rels_arr ? ccord_json_array_len(rels_arr) : 0;
  for (size_t i = 0; ok && i < nrel; i++) {
    const ccord_json *rj = ccord_json_array_at(rels_arr, i);
    const ccord_json *ru = rj ? ccord_json_get(rj, "user") : nullptr;
    ccord_id ruid = CCORD_ID_NONE;
    if (!dj_json_id_key(rj, "user_id", &ruid) && (!ru || !dj_json_id_key(ru, "id", &ruid))) {
      (void)dj_json_id_key(rj, "id", &ruid);
    }
    if (ruid == CCORD_ID_NONE || ruid == self) continue;
    ccord_relationship_kind rkind = dj_rel_kind_from(dj_json_i64(rj, "type", 0));
    if (rkind == CCORD_REL_NONE) continue;
    ccord_user rel_u;
    if (ru && dj_map_user_obj(&ev->arena, ru, CCORD_PRESENCE_OFFLINE, &rel_u)) {
      rel_u.id = ruid;
      ok = dj_sb_user(&b, &rel_u);
    } else {
      ok = dj_sb_ensure_user_stub(&b, ruid);
      if (!ok) break;
    }
    if (ok) {
      ccord_relationship rel = {.user_id = ruid, .kind = rkind};
      ok = dj_sb_relationship(&b, &rel);
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
        const ccord_json *gprops = dj_guild_props(gj);
        ccord_id gid = CCORD_ID_NONE;
        if (dj_guild_id_key_p(gj, gprops, "id", &gid) && gid != CCORD_ID_NONE &&
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
          if (!dj_json_id(ccord_json_array_at(gids, k), &want_gid)) continue;
          emit_want_guild_folder(&b, guilds_arr, merged_arr, &ready_guild_idx, ng, guild_cap, visited,
                                 want_gid, fid, fname, fcolor, &ok);
        }
      }
      const ccord_json *gpos = uset ? ccord_json_get(uset, "guild_positions") : nullptr;
      size_t npos = gpos ? ccord_json_array_len(gpos) : 0;
      for (size_t p = 0; ok && p < npos && b.guild_count < guild_cap; p++) {
        ccord_id want_gid = CCORD_ID_NONE;
        if (!dj_json_id(ccord_json_array_at(gpos, p), &want_gid)) continue;
        emit_want_guild(&b, guilds_arr, merged_arr, &ready_guild_idx, ng, guild_cap, visited, want_gid, &ok);
      }
      const char *proto_b64 = dj_jstr(d, "user_settings_proto");
      if (proto_b64 && proto_b64[0] != '\0') {
        apply_proto_guild_order(proto_b64, &b, guilds_arr, merged_arr, &ready_guild_idx, ng, guild_cap, visited, &ok);
      }
      for (size_t i = 0; ok && i < ng && b.guild_count < guild_cap; i++) {
        if (visited[i]) continue;
        visited[i] = true;
        ok = dj_map_guild_ex(&b, ccord_json_array_at(guilds_arr, i),
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
      if (dj_json_id(ccord_json_array_at(rids, r), &ruid) && ruid != CCORD_ID_NONE) {
        ok = dj_sb_ensure_user_stub(&b, ruid);
      }
    }
  }
  const ccord_json *pres_arr = ccord_json_get(d, "presences");
  size_t npres = pres_arr ? ccord_json_array_len(pres_arr) : 0;
  for (size_t i = 0; ok && i < npres; i++) {
    const ccord_json *pj = ccord_json_array_at(pres_arr, i);
    const ccord_json *pu = pj ? ccord_json_get(pj, "user") : nullptr;
    ccord_id pid = CCORD_ID_NONE;
    if (!dj_json_id_key(pu, "id", &pid) && !dj_json_id_key(pj, "user_id", &pid)) continue;
    ccord_presence pres = dj_presence_from(dj_jstr(pj, "status"));
    const char *stext = dj_extract_activity_status(&ev->arena, pj);
    size_t uidx = 0;
    if (ccord_idmap_find(&b.user_idx, pid, &uidx) && uidx < b.user_count) {
      b.users[uidx].presence = pres;
      b.users[uidx].status_text = stext;
      parsed_presence_detail pd;
      dj_extract_presence_detail(pj, &pd);
      dj_apply_presence_detail_to_user(&b.users[uidx], &pd);
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
        if (!dj_map_channel_ex(&ev->arena, b.users, b.user_count, &b.user_idx, self, pc, CCORD_DM_GUILD_ID, &c)) {
          continue;
        }
        dms[ndms++] = (raw_dm_channel){.chan = c, .last_message_id = c.last_message_id};
      }
      if (ndms > 1) {
        qsort(dms, ndms, sizeof *dms, dj_cmp_raw_dm_channel);
      }
      for (size_t i = 0; ok && i < ndms; i++) {
        ok = dj_sb_channel(&b, &dms[i].chan);
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
    ok = dj_collect_voice_state_entry(&b, ccord_json_array_at(vs_arr, i), CCORD_ID_NONE, false, true, true);
  }
  if (ok) ok = dj_sb_finish(&b, ev);
  if (ok && ng > 0) {
    const ccord_json **gl = ccord_calloc(ng, sizeof *gl);
    if (gl) {
      const ccord_json **ml = ccord_calloc(ng, sizeof *ml);
      for (size_t i = 0; i < ng; i++) {
        gl[i] = ccord_json_array_at(guilds_arr, i);
        if (ml) ml[i] = merged_arr ? ccord_json_array_at(merged_arr, i) : nullptr;
      }
      dj_expr_collect_guild(ev, gl, ng);
      dj_perm_collect_guilds(ev, gl, ml, ng, ev->snapshot.self_id);
      ccord_free(ml);
      ccord_free(gl);
    }
  }
  if (ok) dj_notify_collect_ready(ev, d);
  if (ok) dj_notes_collect_ready(ev, d);
  if (!ok) ccord_error_set(err, CCORD_ERR_NOMEM, "gateway: READY too large");
  dj_sb_free(&b);
  return ok;
}


static bool publish_voice_states_event(snapshot_builder *b, ccord_live_event *ev, ccord_id guild_id,
                                       bool replace_voice_states)
{
  bool ok = dj_sb_publish(b, ev);
  dj_sb_free(b);
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

bool dj_map_ready_supplemental(ccord_live_event *ev, const ccord_json *d)
{
  if (!dj_is_json_object(d)) return false;
  snapshot_builder b = {.arena = &ev->arena};
  const ccord_json *users_arr = ccord_json_get(d, "users");
  size_t nu = users_arr ? ccord_json_array_len(users_arr) : 0;
  for (size_t i = 0; i < nu; i++) {
    ccord_user u;
    if (dj_map_user_obj(&ev->arena, ccord_json_array_at(users_arr, i), CCORD_PRESENCE_OFFLINE, &u)) {
      if (!dj_sb_user(&b, &u)) {
        dj_sb_free(&b);
        return false;
      }
    }
  }
  if (!dj_collect_member_users_from_array(&b, ccord_json_get(d, "merged_members"), 0)) {
    dj_sb_free(&b);
    return false;
  }
  const ccord_json *guilds_arr = ccord_json_get(d, "guilds");
  size_t ng = guilds_arr ? ccord_json_array_len(guilds_arr) : 0;
  for (size_t gi = 0; gi < ng && gi < CCORD_MAX_GUILDS; gi++) {
    const ccord_json *gj = ccord_json_array_at(guilds_arr, gi);
    ccord_id gid = CCORD_ID_NONE;
    if (!gj || !dj_json_id_key(gj, "id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) continue;
    const ccord_json *vs_arr = ccord_json_get(gj, "voice_states");
    size_t nv = vs_arr && dj_is_json_array(vs_arr) ? ccord_json_array_len(vs_arr) : 0;
    for (size_t k = 0; k < nv && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
      if (!dj_collect_voice_state_entry(&b, ccord_json_array_at(vs_arr, k), gid, false, false, false)) {
        dj_sb_free(&b);
        return false;
      }
    }
  }
  const ccord_json *top_vs_arr = ccord_json_get(d, "voice_states");
  size_t top_nv = top_vs_arr ? ccord_json_array_len(top_vs_arr) : 0;
  for (size_t k = 0; k < top_nv && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
    if (!dj_collect_voice_state_entry(&b, ccord_json_array_at(top_vs_arr, k), CCORD_ID_NONE, false, false, false)) {
      dj_sb_free(&b);
      return false;
    }
  }
  return publish_voice_states_event(&b, ev, CCORD_ID_NONE, false);
}

bool dj_map_passive_update_v2(ccord_live_event *ev, const ccord_json *d)
{
  if (!dj_is_json_object(d)) return false;
  ccord_id gid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "guild_id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) return false;
  snapshot_builder b = {.arena = &ev->arena};
  if (!dj_collect_member_users_from_array(&b, ccord_json_get(d, "updated_members"), 0)) {
    dj_sb_free(&b);
    return false;
  }
  const ccord_json *uvs_arr = ccord_json_get(d, "updated_voice_states");
  size_t nv = uvs_arr ? ccord_json_array_len(uvs_arr) : 0;
  for (size_t k = 0; k < nv && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
    if (!dj_collect_voice_state_entry(&b, ccord_json_array_at(uvs_arr, k), gid, true, false, false)) {
      dj_sb_free(&b);
      return false;
    }
  }
  const ccord_json *rvs_arr = ccord_json_get(d, "removed_voice_states");
  size_t nr = rvs_arr ? ccord_json_array_len(rvs_arr) : 0;
  for (size_t k = 0; k < nr && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
    const ccord_json *rj = ccord_json_array_at(rvs_arr, k);
    ccord_id uid = CCORD_ID_NONE;
    if (!dj_json_id(rj, &uid)) (void)dj_json_id_key(rj, "user_id", &uid);
    if (uid == CCORD_ID_NONE) continue;
    ccord_voice_state removed = {.guild_id = gid, .channel_id = CCORD_ID_NONE, .user_id = uid};
    if (!dj_sb_voice(&b, &removed)) {
      dj_sb_free(&b);
      return false;
    }
  }
  return publish_voice_states_event(&b, ev, gid, false);
}

bool dj_map_passive_update_v1(ccord_live_event *ev, const ccord_json *d)
{
  if (!dj_is_json_object(d)) return false;
  ccord_id gid = CCORD_ID_NONE;
  if (!dj_json_id_key(d, "guild_id", &gid) || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) return false;
  snapshot_builder b = {.arena = &ev->arena};
  if (!dj_collect_member_users_from_array(&b, ccord_json_get(d, "members"), 0)) {
    dj_sb_free(&b);
    return false;
  }
  const ccord_json *vs_arr = ccord_json_get(d, "voice_states");
  bool has_vs = vs_arr && dj_is_json_array(vs_arr);
  size_t nv = has_vs ? ccord_json_array_len(vs_arr) : 0;
  for (size_t k = 0; k < nv && k < CCORD_MAX_MEMBERS_PER_GUILD; k++) {
    if (!dj_collect_voice_state_entry(&b, ccord_json_array_at(vs_arr, k), gid, false, false, false)) {
      dj_sb_free(&b);
      return false;
    }
  }
  return publish_voice_states_event(&b, ev, gid, has_vs);
}


