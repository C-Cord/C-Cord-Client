// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Internal Discord JSON decoder helpers and structs shared across src/net/discord/json_*.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/idmap.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>
#include <ccord/model/model.h>
#include <ccord/model/rich.h>
#include <ccord/net/live_transport.h>

#define LIVE_JSON_MAX_DEPTH 32u
#define LIVE_JSON_MAX_NODES 2500000u
#define LIVE_JSON_MAX_STRING (1024u * 1024u)
#define LIVE_STATUS_MAX_BYTES 128u
#define LIVE_TOPIC_MAX_BYTES 1024u

typedef ccord_idmap id_index;

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

typedef struct raw_dm_channel {
  ccord_channel chan;
  ccord_id last_message_id;
} raw_dm_channel;

typedef struct perm_lists {
  ccord_role *roles; size_t nroles, caproles;
  ccord_member_roles *mr; size_t nmr, capmr;
  ccord_channel_overwrites *ow; size_t now, capow;
} perm_lists;

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

typedef struct role_table role_table;

// Cross-TU internal decoder helpers
size_t dj_scratch_chunk_size(size_t json_len);
const ccord_json *dj_parse_json(ccord_arena *a, const char *text, size_t len, ccord_error *err);
const char *dj_arena_dup(ccord_arena *a, const char *s);
const char *dj_arena_dup_nullable(ccord_arena *a, const char *s);
const char *dj_sanitize_str_inplace(char *s);
size_t dj_utf8_trunc_len(const char *s, size_t len, size_t max);
const char *dj_jstr(const ccord_json *obj, const char *key);
size_t dj_jarray_len(const ccord_json *obj, const char *key);
bool dj_json_id(const ccord_json *j, ccord_id *out);
bool dj_json_id_key(const ccord_json *obj, const char *key, ccord_id *out);
int64_t dj_json_i64(const ccord_json *obj, const char *key, int64_t fallback);
int64_t dj_iso_ms(const char *s, size_t len);
bool dj_text_ok(const char *s, size_t max);
const char *dj_opt_text(const char *s, size_t max);
const char *dj_opt_text_dup(ccord_arena *a, const char *s, size_t max);
bool dj_map_channel_ex(ccord_arena *a, const ccord_user *users, size_t user_count, const id_index *user_idx, ccord_id self_id, const ccord_json *j, ccord_id guild_hint, ccord_channel *out);
bool dj_map_channel(ccord_arena *a, const ccord_json *j, ccord_id guild_hint, ccord_channel *out);
const char *dj_format_reaction_emoji(ccord_arena *a, const ccord_json *emj, ccord_id *out_id, bool *out_animated);
uint32_t dj_clamp_u32(int64_t v);
bool dj_is_json_object(const ccord_json *j);
bool dj_is_json_array(const ccord_json *j);
bool dj_map_message_ex(ccord_arena *a, const ccord_json *j, ccord_id fallback_channel, bool allow_missing_author, ccord_message *out);
bool dj_map_message(ccord_arena *a, const ccord_json *j, ccord_id fallback_channel, ccord_message *out);
const char *dj_build_cdn_url_sized(ccord_arena *a, const char *kind, ccord_id id, const char *hash, const char *size_str);
const char *dj_build_cdn_url(ccord_arena *a, const char *kind, ccord_id id, const char *hash);
bool dj_sb_user(snapshot_builder *b, const ccord_user *u);
bool dj_sb_relationship(snapshot_builder *b, const ccord_relationship *rel);
bool dj_sb_guild(snapshot_builder *b, const ccord_guild *g);
bool dj_sb_channel(snapshot_builder *b, const ccord_channel *c);
bool dj_sb_voice(snapshot_builder *b, const ccord_voice_state *v);
bool dj_sb_forum_meta(snapshot_builder *b, const ccord_forum_meta *m);
bool dj_sb_forum_post(snapshot_builder *b, const ccord_forum_post *p);
void dj_sb_free(snapshot_builder *b);
bool dj_sb_publish(snapshot_builder *b, ccord_live_event *ev);
bool dj_sb_finish(snapshot_builder *b, ccord_live_event *ev);
const ccord_json *dj_guild_props(const ccord_json *gj);
const char *dj_guild_str_p(const ccord_json *gj, const ccord_json *props, const char *key);
bool dj_guild_id_key_p(const ccord_json *gj, const ccord_json *props, const char *key, ccord_id *out);
const ccord_json *dj_guild_arr_p(const ccord_json *gj, const ccord_json *props, const char *key);
int64_t dj_guild_i64_p(const ccord_json *gj, const ccord_json *props, const char *key, int64_t def);
const ccord_json *dj_guild_obj_p(const ccord_json *gj, const ccord_json *props, const char *key);
const ccord_json *dj_guild_arr(const ccord_json *gj, const char *key);
const ccord_json *dj_guild_roles(const ccord_json *gj);
bool dj_json_u64_key(const ccord_json *obj, const char *key, uint64_t *out);
void dj_init_guild_perm_ctx(guild_perm_ctx *ctx, const ccord_json *gj, const ccord_json *props, const ccord_json *ready_merged_entry, ccord_id gid, ccord_id self_id);
bool dj_channel_can_view(const guild_perm_ctx *ctx, const ccord_json *cj);
int dj_cmp_raw_guild_channel(const void *a, const void *b);
int dj_cmp_raw_dm_channel(const void *a, const void *b);
bool dj_perm_bool(const ccord_json *obj, const char *key);
bool dj_perm_add_role(ccord_arena *a, perm_lists *L, const ccord_json *rj, ccord_id gid);
bool dj_perm_add_roles(ccord_arena *a, perm_lists *L, const ccord_json *arr, ccord_id gid);
bool dj_perm_add_member(ccord_arena *a, perm_lists *L, ccord_id gid, const ccord_json *mj);
bool dj_perm_add_channel(ccord_arena *a, perm_lists *L, const ccord_json *cj);
bool dj_perm_publish(ccord_live_event *ev, perm_lists *L);
void dj_perm_collect_guilds(ccord_live_event *ev, const ccord_json *const *guilds, const ccord_json *const *merged, size_t ng, ccord_id self_id);
void dj_notes_collect_ready(ccord_live_event *ev, const ccord_json *d);
void dj_notify_collect_ready(ccord_live_event *ev, const ccord_json *d);
bool dj_map_notify_settings(ccord_live_event *ev, const ccord_json *d);
void dj_extract_clan_info(const ccord_json *uj, char out_tag[16], ccord_id *out_guild_id, char out_badge[64]);
void dj_copy_trunc_utf8(char *dst, size_t cap, const char *src);
bool dj_cdn_name_ok(const char *s);
const char *dj_cdn_asset_url(ccord_arena *a, const char *path, const char *name, const char *suffix);
const char *dj_extract_avatar_deco_url(ccord_arena *a, const ccord_json *obj);
void dj_extract_avatar_deco_buf(const ccord_json *obj, char out[256]);
void dj_extract_nameplate_info(const ccord_json *obj, char out_asset[128], char out_palette[24]);
int64_t dj_json_ts_ms(const ccord_json *obj, const char *key);
void dj_extract_presence_detail(const ccord_json *obj, parsed_presence_detail *out);
void dj_apply_presence_detail_to_user(ccord_user *u, const parsed_presence_detail *pd);
const char *dj_extract_activity_status(ccord_arena *a, const ccord_json *obj);
bool dj_map_member_with_pres(snapshot_builder *b, const role_table *rt, const id_index *pres_map, const ccord_json *mj, ccord_id gid);
void dj_role_table_free(role_table *t);
void dj_role_table_build(role_table *t, const ccord_json *roles);
const role_table *dj_cache_guild_roles(ccord_id gid, const ccord_json *roles);
void dj_resolve_member_role(ccord_arena *a, ccord_id gid, const role_table *rt, const ccord_json *mj, const char **out_name, uint32_t *out_color, uint8_t *out_rank);
bool dj_map_merged_member_entry(snapshot_builder *b, const role_table *rt, const id_index *pres_map, const ccord_json *entry, ccord_id gid);
bool dj_map_voice_state(const ccord_json *v, ccord_id guild_hint, ccord_voice_state *out);
bool dj_sb_ensure_user_stub(snapshot_builder *b, ccord_id uid);
bool dj_collect_voice_state_entry(snapshot_builder *b, const ccord_json *vj, ccord_id guild_hint, bool allow_disconnect, bool require_channel_known, bool stub_if_missing);
bool dj_collect_member_users_from_array(snapshot_builder *b, const ccord_json *arr, int depth);
void dj_map_forum_meta(const ccord_json *cj, ccord_id forum_id, uint8_t discord_type, ccord_forum_meta *out);
bool dj_map_forum_post(ccord_arena *a, const ccord_json *tj, const ccord_json *first_msg, ccord_id forum_fallback, ccord_id guild_fallback, ccord_forum_post *out);
void dj_parse_incidents_obj(const ccord_json *inc, int64_t *out_inv_ms, int64_t *out_dms_ms);
bool dj_map_guild_ex(snapshot_builder *b, const ccord_json *gj, const ccord_json *ready_merged_entry);
bool dj_map_guild(snapshot_builder *b, const ccord_json *gj);
ccord_relationship_kind dj_rel_kind_from(int64_t type);
bool dj_map_user_obj(ccord_arena *a, const ccord_json *uj, ccord_presence pres, ccord_user *out);
uint8_t dj_json_u8_clamped(const ccord_json *obj, const char *key, uint8_t max, uint8_t fallback);
bool dj_map_user_settings_custom_status(ccord_live_event *ev, const ccord_json *d);
bool dj_map_user_settings_folders(ccord_live_event *ev, const ccord_json *d, bool require_folders);
bool dj_map_user_settings_proto_update(ccord_live_event *ev, const ccord_json *d);
void dj_map_sessions_replace(ccord_live_event *ev, const ccord_json *d);
bool dj_parse_ready(ccord_live_event *ev, const ccord_json *d, ccord_error *err);
ccord_presence dj_presence_from(const char *status);
bool dj_map_user_note_update(ccord_live_event *ev, const ccord_json *d, ccord_id fallback_uid);
void dj_attach_channel_recipients(ccord_live_event *ev, const ccord_json *d);
bool dj_json_bool_key(const ccord_json *obj, const char *key, bool fallback);
bool dj_map_one_emoji(ccord_arena *a, const ccord_json *ej, ccord_id fallback_gid, ccord_emoji *out, bool *oom);
bool dj_map_one_sticker(ccord_arena *a, const ccord_json *sj, ccord_id fallback_gid, ccord_sticker *out, bool *oom);
bool dj_map_one_soundboard(ccord_arena *a, const ccord_json *sbj, ccord_id fallback_gid, ccord_soundboard_sound *out, bool *oom);
bool dj_expr_append_emojis(ccord_arena *a, const ccord_json *arr, ccord_id gid, ccord_emoji **list, size_t *n, size_t *cap);
bool dj_expr_append_stickers(ccord_arena *a, const ccord_json *arr, ccord_id gid, ccord_sticker **list, size_t *n, size_t *cap);
bool dj_expr_append_soundboard(ccord_arena *a, const ccord_json *arr, ccord_id gid, ccord_soundboard_sound **list, size_t *n, size_t *cap);
bool dj_expr_publish(ccord_live_event *ev, ccord_emoji *el, size_t en, ccord_sticker *sl, size_t sn, ccord_soundboard_sound *sbl, size_t sbn);
void dj_expr_collect_guild(ccord_live_event *ev, const ccord_json *const *guilds, size_t ng);
bool dj_map_guild_expressions_update(ccord_live_event *ev, const ccord_json *d, int kind);
bool dj_map_soundboard_upsert_dispatch(ccord_live_event *ev, const ccord_json *d);
bool dj_map_soundboard_delete_dispatch(ccord_live_event *ev, const ccord_json *d);
bool dj_map_one_automod_rule(ccord_arena *a, const ccord_json *obj, ccord_id guild_hint, ccord_automod_rule *out, bool *oom);
bool dj_map_guild_upsert(ccord_live_event *ev, const ccord_json *d);
bool dj_map_ready_supplemental(ccord_live_event *ev, const ccord_json *d);
bool dj_map_passive_update_v2(ccord_live_event *ev, const ccord_json *d);
bool dj_map_passive_update_v1(ccord_live_event *ev, const ccord_json *d);
void dj_map_message_author(ccord_arena *a, const ccord_json *j, ccord_id author_id, ccord_user *out);
void dj_collect_message_mentions(ccord_arena *a, const ccord_json *j, ccord_user *users, size_t *ukept, size_t max_users);
void dj_copy_json_str(char *dst, size_t cap, const ccord_json *obj, const char *key);
bool dj_map_stage_instance_obj(const ccord_json *d, ccord_id guild_hint, ccord_id channel_hint, ccord_stage_instance *out);
bool dj_map_scheduled_event_obj(const ccord_json *d, ccord_id guild_hint, bool deleted, ccord_scheduled_event *out);
bool dj_parse_hex_color(const char *s, uint32_t *out);
