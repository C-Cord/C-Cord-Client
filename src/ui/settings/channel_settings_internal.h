// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Channel Settings: the full-window overlay for editing a channel's Overview, tri-state Permission Overwrites,
// Invites, and Delete Channel.
//
// Like Server Settings, every page and control is gated by the permission engine, every action goes through the
// ccord_admin_* builders and session queue, and nothing mutates the store until Discord confirms.
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_internal.h"
#include "ui_style.h"

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>
#include <ccord/model/forum.h>
#include <ccord/model/permissions.h>
#include <ccord/net/admin.h>
#include <ccord/store/admin.h>
#include <ccord/store/roles.h>

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)
#define CS_CAP(c) (uis_tokens_of((c)->ui).fs_caption)   // 12 px metadata, descriptions and hints
#define CS_TITLE(c) (uis_tokens_of((c)->ui).fs_title)
#define NAME(buf, ...) (snprintf((buf), sizeof(buf), __VA_ARGS__), (buf))

#define CS_HIT_MAX 1024u
#define CS_NAME_LEN 64u
#define CS_LIST_FRESH_MS 30000u
#define CS_LIST_TIMEOUT_MS 10000u
#define CS_REFRESH_GAP_MS 2000u
#define CS_FLASH_MS 700u
#define CS_PERM_ROW_H 58.0f

typedef enum cs_page : uint8_t {
  CS_OVERVIEW = 0,
  CS_PERMISSIONS = 1,
  CS_INVITES = 2,
  CS_PAGE_COUNT = 3
} cs_page;

typedef enum cs_field : uint8_t {
  CSF_NONE = 0,
  CSF_NAME = 1,
  CSF_TOPIC = 2,
  CSF_TAG_NAME = 3,
  CSF_TAG_EMOJI = 4
} cs_field;

typedef enum cs_act : uint8_t {
  CA_NONE = 0,
  CA_CLOSE,
  CA_PAGE,
  CA_DELETE_CHANNEL,
  CA_FIELD,
  CA_SAVE,
  CA_RESET,
  CA_NSFW,
  CA_SLOWMODE,
  CA_BITRATE,
  CA_USER_LIMIT,
  CA_USER_LIMIT_STEP,
  CA_ARCHIVE,
  CA_TAG_MOD,
  CA_TAG_DEL,
  CA_TAG_NEW_MOD,
  CA_TAG_ADD,
  CA_OW_TARGET,
  CA_OW_ADD_TOGGLE,
  CA_OW_ADD_ROLE,
  CA_OW_ADD_MEMBER,
  CA_OW_DELETE,
  CA_OW_DENY,
  CA_OW_INHERIT,
  CA_OW_ALLOW,
  CA_INV_AGE,
  CA_INV_USES,
  CA_INV_CREATE,
  CA_INV_COPY,
  CA_INV_REVOKE,
  CA_REFRESH
} cs_act;

typedef struct cs_hit {
  ccord_rect r;
  cs_act act;
  uint64_t arg;
  const char *why;
  char name[CS_NAME_LEN];
} cs_hit;

typedef struct cs_overview_copy {
  char name[CCORD_ADMIN_CHANNEL_NAME_MAX + 4];
  char topic[CCORD_ADMIN_TOPIC_MAX + 4];
  bool nsfw;
  uint32_t slowmode_s;
  uint32_t bitrate;
  uint32_t user_limit;
  uint32_t archive_min;
  ccord_forum_tag tags[CCORD_FORUM_MAX_TAGS];
  size_t tag_count;
} cs_overview_copy;

typedef struct cs_ow_target_item {
  ccord_id id;
  ccord_overwrite_kind kind;
  char label[128];
} cs_ow_target_item;

typedef struct cs_ow_add_item {
  ccord_id id;
  char label[96];
} cs_ow_add_item;

struct ui_channel_settings {
  bool open;
  ccord_id guild_id;
  ccord_id channel_id;
  cs_page page;
  cs_field focus;
  float scroll, content_h, view_h;
  ccord_scroller glide;   // wheel glide of the page (scroll.h)
  float list_scroll, list_content_h;
  ccord_rect list_view;
  uint64_t synced_rev;
  uint64_t flash_until_ms;
  int hover;
  size_t dbg_rows;

  // Cached channel permission summary (avoids 5-9x ccord_store_permissions per frame).
  bool perm_cache_valid;
  uint64_t perm_cache_rev;
  int64_t perm_cache_now_ms;
  uint64_t cached_perms;
  bool cached_exact;

  // Cached overwrite targets and add-menu candidates (avoids per-frame O(Roles x Overwrites)).
  bool ow_targets_valid;
  uint64_t ow_targets_rev;
  ccord_id ow_targets_sel_id;
  ccord_overwrite_kind ow_targets_sel_kind;
  bool ow_sel_has_stored;
  cs_ow_target_item *ow_targets;
  size_t ow_target_count;
  size_t ow_target_cap;

  bool ow_add_valid;
  uint64_t ow_add_rev;
  cs_ow_add_item *ow_add_roles;
  size_t ow_add_role_count;
  size_t ow_add_role_cap;
  cs_ow_add_item *ow_add_members;
  size_t ow_add_member_count;
  size_t ow_add_member_cap;

  cs_overview_copy ov_base, ov_work;
  ccord_text_edit name_edit;
  ccord_text_edit topic_edit;
  ccord_text_edit tag_name_edit;
  ccord_text_edit tag_emoji_edit;
  bool tag_new_mod;

  ccord_id ow_sel_id;
  ccord_overwrite_kind ow_sel_kind;
  bool ow_sel_staged;
  bool ow_add_open;
  ccord_overwrite ow_base, ow_work;

  uint64_t invites_req_ms, invites_fail_ms;
  size_t inv_age_idx, inv_uses_idx;

  cs_hit hits[CS_HIT_MAX];
  size_t nhits;
};

typedef struct cs_cx {
  ccord_ui *ui;
  struct ui_channel_settings *cs;
  ccord_renderer *r;
  ccord_rect clip;
} cs_cx;


#define SLOWMODE_COUNT 12u
#define BITRATE_COUNT 6u
#define USER_LIMIT_COUNT 7u
#define ARCHIVE_COUNT 4u
#define CS_INV_AGE_COUNT 7u
#define CS_INV_USES_COUNT 7u

// Cross-TU internal declarations
uint64_t cs_cached_chan_perms(const ccord_ui *ui, struct ui_channel_settings *cs);
ccord_text_edit * cs_edit_for(struct ui_channel_settings *cs, cs_field f);
extern const char *const cs_k_archive_labels[4];
extern const uint32_t cs_k_archives[4];
extern const char *const cs_k_bitrate_labels[6];
extern const uint32_t cs_k_bitrates[6];
extern const char *const cs_k_cs_inv_age_labels[7];
extern const char *const cs_k_cs_inv_use_labels[7];
extern const char *const cs_k_slowmode_labels[12];
extern const uint32_t cs_k_slowmodes[12];
extern const char *const cs_k_user_limit_labels[7];
extern const uint32_t cs_k_user_limits[7];
bool cs_add_hit(cs_cx *c, ccord_rect r, cs_act act, uint64_t arg, const char *why, const char *name);
void cs_button(cs_cx *c, ccord_rect r, const char *label, uis_btn_kind kind, const char *why, cs_act act, uint64_t arg, const char *name);
float cs_text(cs_cx *c, const char *s, float x, float y, float size, bool bold, ccord_color col, float max_w);
void cs_draw_cs_invites(cs_cx *c, float x, float w, float *py);
void cs_draw_cs_overview(cs_cx *c, const ccord_channel *ch, float x, float w, float *py);
void cs_draw_cs_permissions(cs_cx *c, float x, float w, float *py);
