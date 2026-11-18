// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
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

static const char *const k_cs_page_names[CS_PAGE_COUNT] = {"overview", "permissions", "invites"};
static const char *const k_cs_page_labels[CS_PAGE_COUNT] = {"Overview", "Permissions", "Invites"};

static const uint32_t k_slowmodes[] = {0, 5, 10, 15, 30, 60, 120, 300, 600, 900, 3600, 21600};
static const char *const k_slowmode_labels[] = {
  "Off", "5s", "10s", "15s", "30s", "1m", "2m", "5m", "10m", "15m", "1h", "6h"
};
#define SLOWMODE_COUNT (sizeof k_slowmodes / sizeof k_slowmodes[0])

static const uint32_t k_bitrates[] = {32000, 64000, 96000, 128000, 256000, 384000};
static const char *const k_bitrate_labels[] = {"32 kbps", "64 kbps", "96 kbps", "128 kbps", "256 kbps", "384 kbps"};
#define BITRATE_COUNT (sizeof k_bitrates / sizeof k_bitrates[0])

static const uint32_t k_user_limits[] = {0, 2, 5, 10, 25, 50, 99};
static const char *const k_user_limit_labels[] = {"Unlimited", "2", "5", "10", "25", "50", "99"};
#define USER_LIMIT_COUNT (sizeof k_user_limits / sizeof k_user_limits[0])

static const uint32_t k_archives[] = {60, 1440, 4320, 10080};
static const char *const k_archive_labels[] = {"1 Hour", "24 Hours", "3 Days", "1 Week"};
#define ARCHIVE_COUNT (sizeof k_archives / sizeof k_archives[0])

static const uint32_t k_cs_inv_ages[] = {1800, 3600, 21600, 43200, 86400, 604800, 0};
static const char *const k_cs_inv_age_labels[] = {
  "30 minutes", "1 hour", "6 hours", "12 hours", "1 day", "7 days", "Never"
};
static const uint32_t k_cs_inv_uses[] = {0, 1, 5, 10, 25, 50, 100};
static const char *const k_cs_inv_use_labels[] = {
  "No limit", "1 use", "5 uses", "10 uses", "25 uses", "50 uses", "100 uses"
};
#define CS_INV_AGE_COUNT (sizeof k_cs_inv_ages / sizeof k_cs_inv_ages[0])
#define CS_INV_USES_COUNT (sizeof k_cs_inv_uses / sizeof k_cs_inv_uses[0])

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

static bool cs_init_edits(struct ui_channel_settings *cs)
{
  ccord_text_edit_init(&cs->name_edit, CCORD_ADMIN_CHANNEL_NAME_MAX);
  ccord_text_edit_init(&cs->topic_edit, CCORD_ADMIN_TOPIC_MAX);
  ccord_text_edit_init(&cs->tag_name_edit, CCORD_FORUM_TAG_NAME_BYTES - 1u);
  ccord_text_edit_init(&cs->tag_emoji_edit, 64);
  return true;
}

static struct ui_channel_settings *cs_get(ccord_ui *ui)
{
  if (ui->cs) return ui->cs;
  struct ui_channel_settings *cs = ccord_calloc(1, sizeof *cs);
  if (!cs) return nullptr;
  if (!cs_init_edits(cs)) {
    ccord_free(cs);
    return nullptr;
  }
  cs->hover = -1;
  cs->inv_age_idx = 4;   // 1 day
  cs->inv_uses_idx = 0;  // unlimited
  ui->cs = cs;
  return cs;
}

void ui_channel_settings_destroy(ccord_ui *ui)
{
  if (!ui || !ui->cs) return;
  ccord_text_edit_free(&ui->cs->name_edit);
  ccord_text_edit_free(&ui->cs->topic_edit);
  ccord_text_edit_free(&ui->cs->tag_name_edit);
  ccord_text_edit_free(&ui->cs->tag_emoji_edit);
  ccord_free(ui->cs);
  ui->cs = nullptr;
}

bool ui_channel_settings_is_open(const ccord_ui *ui)
{
  return ui && ui->cs && ui->cs->open;
}

bool ui_channel_settings_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  struct ui_channel_settings *cs = ui->cs;
  if (!cs || cs->glide.pending == 0.0f) return false;
  if (!cs->open) {   // shut: drop the glide so it does not carry on when reopened
    ccord_scroller_cancel(&cs->glide);
    return false;
  }
  ui->dirty = true;
  return ccord_scroller_tick(&cs->glide, &cs->scroll, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced);
}

static void cs_set_edit(ccord_text_edit *e, const char *s)
{
  ccord_error err;
  ccord_text_edit_clear(e);
  if (s && s[0] != '\0') (void)ccord_text_edit_insert(e, s, strlen(s), &err);
}

static ccord_text_edit *cs_edit_for(struct ui_channel_settings *cs, cs_field f)
{
  switch (f) {
    case CSF_NAME: return &cs->name_edit;
    case CSF_TOPIC: return &cs->topic_edit;
    case CSF_TAG_NAME: return &cs->tag_name_edit;
    case CSF_TAG_EMOJI: return &cs->tag_emoji_edit;
    case CSF_NONE: break;
  }
  return nullptr;
}

// ---------------------------------------------------------------- permissions

static uint64_t cs_self_chan_perms(const ccord_ui *ui, ccord_id guild_id, ccord_id channel_id)
{
  return ccord_store_permissions(ui->d.store, guild_id, channel_id, ccord_store_self_id(ui->d.store), ui->now_unix_ms);
}

static bool cs_page_allowed(const ccord_ui *ui, const ccord_channel *ch, cs_page p)
{
  if (!ui || !ch || ch->guild_id == CCORD_ID_NONE || ch->guild_id == CCORD_DM_GUILD_ID) return false;
  uint64_t perms = cs_self_chan_perms(ui, ch->guild_id, ch->id);
  switch (p) {
    case CS_OVERVIEW:
      return ccord_perm_has(perms, CCORD_PERM_MANAGE_CHANNELS);
    case CS_PERMISSIONS:
      return !ccord_discord_type_is_thread(ch->discord_type) && ccord_perm_has(perms, CCORD_PERM_MANAGE_ROLES);
    case CS_INVITES:
      return ch->kind != CCORD_CHANNEL_CATEGORY && !ccord_discord_type_is_thread(ch->discord_type) &&
             ccord_perm_has(perms, CCORD_PERM_MANAGE_CHANNELS);
    case CS_PAGE_COUNT:
      break;
  }
  return false;
}

bool ui_channel_settings_available(const ccord_ui *ui, ccord_id guild_id, ccord_id channel_id)
{
  if (!ui || !ui->d.store || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return false;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, channel_id);
  if (!ch || ch->guild_id != guild_id || ccord_discord_type_is_thread(ch->discord_type)) return false;
  for (cs_page p = CS_OVERVIEW; p < CS_PAGE_COUNT; p++) {
    if (cs_page_allowed(ui, ch, p)) return true;
  }
  return false;
}

// ---------------------------------------------------------------- working copies & sync

static cs_overview_copy cs_copy_channel(const ccord_store *st, const ccord_channel *ch)
{
  cs_overview_copy c = {0};
  if (!ch) return c;
  (void)snprintf(c.name, sizeof c.name, "%s", ch->name ? ch->name : "");
  (void)snprintf(c.topic, sizeof c.topic, "%s", ch->topic ? ch->topic : "");
  c.nsfw = ch->nsfw;
  c.slowmode_s = ch->rate_limit_per_user;
  c.bitrate = ch->bitrate > 0 ? ch->bitrate : (ch->kind == CCORD_CHANNEL_VOICE ? 64000u : 0u);
  c.user_limit = ch->user_limit;
  c.archive_min = ch->default_auto_archive_duration > 0 ? ch->default_auto_archive_duration : 1440u;
  if (ccord_discord_type_is_forum(ch->discord_type)) {
    const ccord_forum_meta *fm = ccord_store_forum_meta(st, ch->id);
    if (fm && fm->tag_count > 0) {
      c.tag_count = fm->tag_count <= CCORD_FORUM_MAX_TAGS ? fm->tag_count : CCORD_FORUM_MAX_TAGS;
      memcpy(c.tags, fm->tags, c.tag_count * sizeof(ccord_forum_tag));
    }
  }
  return c;
}

static bool cs_tags_eq(const cs_overview_copy *a, const cs_overview_copy *b)
{
  if (a->tag_count != b->tag_count) return false;
  for (size_t i = 0; i < a->tag_count; i++) {
    if (a->tags[i].id != b->tags[i].id) return false;
    if (a->tags[i].moderated != b->tags[i].moderated) return false;
    if (strcmp(a->tags[i].name, b->tags[i].name) != 0) return false;
    if (strcmp(a->tags[i].emoji_name, b->tags[i].emoji_name) != 0) return false;
  }
  return true;
}

static bool cs_ov_eq(const cs_overview_copy *a, const cs_overview_copy *b, const ccord_channel *ch)
{
  if (strcmp(a->name, b->name) != 0) return false;
  bool has_topic = ch && ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type);
  if (has_topic && strcmp(a->topic, b->topic) != 0) return false;
  if (ch && ch->kind != CCORD_CHANNEL_CATEGORY && !ccord_discord_type_is_thread(ch->discord_type)) {
    if (a->nsfw != b->nsfw) return false;
  }
  if (ch && ch->kind != CCORD_CHANNEL_CATEGORY) {
    if (a->slowmode_s != b->slowmode_s) return false;
  }
  if (ch && ch->kind == CCORD_CHANNEL_VOICE) {
    if (a->bitrate != b->bitrate || a->user_limit != b->user_limit) return false;
  }
  if (ch && ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type)) {
    if (a->archive_min != b->archive_min) return false;
  }
  if (ch && ccord_discord_type_is_forum(ch->discord_type)) {
    if (!cs_tags_eq(a, b)) return false;
  }
  return true;
}

static bool cs_find_stored_overwrite(const ccord_store *st, ccord_id channel_id, ccord_id target_id,
                                     ccord_overwrite *out)
{
  size_t n = 0;
  const ccord_overwrite *ows = ccord_store_channel_overwrites(st, channel_id, &n);
  for (size_t i = 0; ows && i < n; i++) {
    if (ows[i].id == target_id) {
      if (out) *out = ows[i];
      return true;
    }
  }
  return false;
}

static void cs_load_channel(ccord_ui *ui, const ccord_channel *ch)
{
  struct ui_channel_settings *cs = ui->cs;
  cs->ov_base = cs_copy_channel(ui->d.store, ch);
  cs->ov_work = cs->ov_base;
  cs_set_edit(&cs->name_edit, cs->ov_work.name);
  cs_set_edit(&cs->topic_edit, cs->ov_work.topic);
  ccord_text_edit_clear(&cs->tag_name_edit);
  ccord_text_edit_clear(&cs->tag_emoji_edit);
  cs->tag_new_mod = false;
}

static void cs_select_overwrite_target(ccord_ui *ui, ccord_id id, ccord_overwrite_kind kind, bool staged)
{
  struct ui_channel_settings *cs = ui->cs;
  cs->ow_sel_id = id;
  cs->ow_sel_kind = kind;
  cs->ow_sel_staged = staged;
  ccord_overwrite stored = {.id = id, .kind = kind, .allow = 0, .deny = 0};
  if (cs_find_stored_overwrite(ui->d.store, cs->channel_id, id, &stored)) {
    cs->ow_sel_kind = stored.kind;
    cs->ow_sel_staged = false;
  }
  cs->ow_base = stored;
  cs->ow_work = stored;
}

static bool cs_unsaved(const ccord_ui *ui)
{
  if (!ui_channel_settings_is_open(ui)) return false;
  const struct ui_channel_settings *cs = ui->cs;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, cs->channel_id);
  if (cs->page == CS_OVERVIEW) return !cs_ov_eq(&cs->ov_work, &cs->ov_base, ch);
  if (cs->page == CS_PERMISSIONS) {
    return cs->ow_work.allow != cs->ow_base.allow || cs->ow_work.deny != cs->ow_base.deny;
  }
  return false;
}

static void cs_enter_page(ccord_ui *ui, cs_page p);

static void cs_sync(ccord_ui *ui)
{
  if (ui_server_settings_is_open(ui) || ui->settings_open) {
    ui_channel_settings_close(ui);
    return;
  }
  struct ui_channel_settings *cs = ui->cs;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, cs->channel_id);
  if (!ch || !ui_channel_settings_available(ui, cs->guild_id, cs->channel_id)) {
    ui_channel_settings_close(ui);
    return;
  }
  if (!cs_page_allowed(ui, ch, cs->page)) {
    for (cs_page p = CS_OVERVIEW; p < CS_PAGE_COUNT; p++) {
      if (cs_page_allowed(ui, ch, p)) {
        cs_enter_page(ui, p);
        break;
      }
    }
  }
  uint64_t rev = ccord_store_revision(ui->d.store);
  if (rev == cs->synced_rev) return;
  cs->synced_rev = rev;

  cs_overview_copy fresh = cs_copy_channel(ui->d.store, ch);
  bool ov_clean = cs_ov_eq(&cs->ov_work, &cs->ov_base, ch);
  cs->ov_base = fresh;
  if (ov_clean || cs_ov_eq(&cs->ov_work, &fresh, ch)) {
    cs->ov_work = fresh;
    if (cs->focus != CSF_NAME) cs_set_edit(&cs->name_edit, cs->ov_work.name);
    if (cs->focus != CSF_TOPIC) cs_set_edit(&cs->topic_edit, cs->ov_work.topic);
  }

  if (cs->ow_sel_id == CCORD_ID_NONE) {
    cs_select_overwrite_target(ui, cs->guild_id, CCORD_OVERWRITE_ROLE, false);
  } else {
    ccord_overwrite stored = {.id = cs->ow_sel_id, .kind = cs->ow_sel_kind, .allow = 0, .deny = 0};
    bool has_stored = cs_find_stored_overwrite(ui->d.store, cs->channel_id, cs->ow_sel_id, &stored);
    bool ow_clean = (cs->ow_work.allow == cs->ow_base.allow && cs->ow_work.deny == cs->ow_base.deny);
    cs->ow_base = stored;
    if (has_stored) cs->ow_sel_staged = false;
    else if (cs->ow_sel_id != cs->guild_id && ow_clean && !cs->ow_sel_staged) {
      // Selected overwrite was removed on Discord: return to @everyone.
      cs_select_overwrite_target(ui, cs->guild_id, CCORD_OVERWRITE_ROLE, false);
      return;
    }
    if (ow_clean || (cs->ow_work.allow == stored.allow && cs->ow_work.deny == stored.deny)) {
      cs->ow_work = stored;
    }
  }
}

static void cs_request_invites(ccord_ui *ui, bool force)
{
  struct ui_channel_settings *cs = ui->cs;
  if (!ui->d.session) return;
  uint64_t now = ui->now_ms ? ui->now_ms : 1u;
  if (force) {
    if (cs->invites_req_ms && now < cs->invites_req_ms + CS_REFRESH_GAP_MS) return;
  } else {
    if (ccord_store_channel_invites_loaded(ui->d.store, cs->guild_id, cs->channel_id) &&
        cs->invites_req_ms && now < cs->invites_req_ms + CS_LIST_FRESH_MS) {
      return;
    }
    if (cs->invites_req_ms && !ccord_store_channel_invites_loaded(ui->d.store, cs->guild_id, cs->channel_id) &&
        now < cs->invites_req_ms + CS_LIST_TIMEOUT_MS) {
      return;
    }
  }
  ccord_admin_request req;
  ccord_error err;
  if (ccord_admin_list_channel_invites(&req, cs->guild_id, cs->channel_id, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't load channel invites: %s", err.message);
    return;
  }
  cs->invites_req_ms = now;
  cs->invites_fail_ms = 0;
  ui_admin_submit(ui, &req);
}

static void cs_enter_page(ccord_ui *ui, cs_page p)
{
  struct ui_channel_settings *cs = ui->cs;
  cs->page = p;
  cs->scroll = 0.0f;
  ccord_scroller_cancel(&cs->glide);
  cs->focus = CSF_NONE;
  if (p == CS_INVITES) cs_request_invites(ui, false);
}

bool ui_channel_settings_open(ccord_ui *ui, ccord_id channel_id)
{
  if (!ui || !ui->d.store || channel_id == CCORD_ID_NONE) return false;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, channel_id);
  if (!ch || !ui_channel_settings_available(ui, ch->guild_id, channel_id)) return false;
  struct ui_channel_settings *cs = cs_get(ui);
  if (!cs) return false;

  ui_menu_close(ui);
  ui->settings_open = false;
  ui->go_live_open = false;
  ui->search_open = false;
  ui->pins_open = false;
  ui->inbox_open = false;
  ui->shortcuts_open = false;
  ui->member_popover_open = false;
  ui->modal_kind = UI_MODAL_NONE;
  if (ui_server_settings_is_open(ui)) ui_server_settings_close(ui);

  bool same = cs->guild_id == ch->guild_id && cs->channel_id == channel_id;
  if (cs->open && !same && ui->d.store) {
    ccord_store_forget_channel_invites(ui->d.store, cs->guild_id, cs->channel_id);
  }
  cs->open = true;
  cs->guild_id = ch->guild_id;
  cs->channel_id = channel_id;
  cs->focus = CSF_NONE;
  cs->scroll = 0.0f;
  ccord_scroller_cancel(&cs->glide);
  cs->list_scroll = 0.0f;
  cs->hover = -1;
  cs->nhits = 0;
  cs->flash_until_ms = 0;
  cs->ow_add_open = false;
  if (!same) {
    cs->invites_req_ms = 0;
    cs->invites_fail_ms = 0;
  }
  cs->synced_rev = ccord_store_revision(ui->d.store);
  cs_load_channel(ui, ch);
  cs_select_overwrite_target(ui, ch->guild_id, CCORD_OVERWRITE_ROLE, false);

  cs_page first = CS_OVERVIEW;
  for (cs_page p = CS_OVERVIEW; p < CS_PAGE_COUNT; p++) {
    if (cs_page_allowed(ui, ch, p)) {
      first = p;
      break;
    }
  }
  cs_enter_page(ui, first);
  ui->dirty = true;
  return true;
}

bool ccord_ui_open_channel_settings(ccord_ui *ui, ccord_id channel_id)
{
  return ui_channel_settings_open(ui, channel_id);
}

void ui_channel_settings_scroll_debug(const ccord_ui *ui, float *scroll, float *room)
{
  const struct ui_channel_settings *cs = ui ? ui->cs : nullptr;
  if (scroll) *scroll = cs ? cs->scroll : 0.0f;
  if (room) *room = cs ? fmaxf(0.0f, cs->content_h - cs->view_h) : 0.0f;
}

void ui_channel_settings_close(ccord_ui *ui)
{
  if (!ui_channel_settings_is_open(ui)) return;
  ccord_id gid = ui->cs->guild_id;
  ccord_id cid = ui->cs->channel_id;
  ui->cs->open = false;
  ui->cs->focus = CSF_NONE;
  ui->cs->nhits = 0;
  ui->cs->hover = -1;
  if (ui->d.store && cid != CCORD_ID_NONE) ccord_store_forget_channel_invites(ui->d.store, gid, cid);
  ui->dirty = true;
}

void ui_channel_settings_on_failure(ccord_ui *ui, ccord_admin_op op)
{
  if (!ui_channel_settings_is_open(ui)) return;
  if (op == CCORD_ADMIN_LIST_CHANNEL_INVITES) {
    ui->cs->invites_fail_ms = ui->now_ms ? ui->now_ms : 1u;
    ui->dirty = true;
  }
}

ccord_cursor_kind ui_channel_settings_cursor(const ccord_ui *ui)
{
  if (!ui_channel_settings_is_open(ui)) return CCORD_CURSOR_DEFAULT;
  int h = ui->cs->hover;
  if (h < 0 || (size_t)h >= ui->cs->nhits) return CCORD_CURSOR_DEFAULT;
  const cs_hit *hit = &ui->cs->hits[h];
  if (hit->why) return CCORD_CURSOR_DEFAULT;
  return hit->act == CA_FIELD ? CCORD_CURSOR_TEXT : CCORD_CURSOR_POINTER;
}

bool ui_channel_settings_wants_caret(const ccord_ui *ui)
{
  return ui_channel_settings_is_open(ui) && ui->cs->focus != CSF_NONE;
}

bool ccord_ui_channel_settings_find(const ccord_ui *ui, const char *name, ccord_rect *out)
{
  if (!ui_channel_settings_is_open(ui) || !name) return false;
  for (size_t i = 0; i < ui->cs->nhits; i++) {
    if (strcmp(ui->cs->hits[i].name, name) == 0) {
      if (out) *out = ui->cs->hits[i].r;
      return true;
    }
  }
  return false;
}

void ccord_ui_get_channel_settings_debug(const ccord_ui *ui, ccord_ui_channel_settings_debug *out)
{
  if (!out) return;
  *out = (ccord_ui_channel_settings_debug){0};
  if (!ui_channel_settings_is_open(ui)) return;
  const struct ui_channel_settings *cs = ui->cs;
  out->open = true;
  out->unsaved = cs_unsaved(ui);
  out->guild_id = cs->guild_id;
  out->channel_id = cs->channel_id;
  out->overwrite_id = cs->ow_sel_id;
  out->page = (int)cs->page;
  out->focus = (int)cs->focus;
  out->hit_count = cs->nhits;
  out->rows = cs->dbg_rows;
  out->scroll = cs->scroll;
  out->content_height = cs->content_h;
  out->work_allow = cs->ow_work.allow;
  out->work_deny = cs->ow_work.deny;
  out->work_slowmode_s = cs->ov_work.slowmode_s;
  out->work_bitrate = cs->ov_work.bitrate;
  out->work_user_limit = cs->ov_work.user_limit;
  out->work_archive_min = cs->ov_work.archive_min;
  out->work_nsfw = cs->ov_work.nsfw;
  out->work_tag_count = cs->ov_work.tag_count;
  (void)snprintf(out->channel_name, sizeof out->channel_name, "%s", cs->ov_work.name);
}

bool ccord_ui_channel_gear_rect(const ccord_ui *ui, ccord_id channel_id, ccord_rect *out)
{
  if (!ui || channel_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < ui->nchan_gear_rects; i++) {
    if (ui->chan_gear_rects[i].channel_id == channel_id) {
      if (out) *out = ui->chan_gear_rects[i].rect;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------- drawing primitives

static bool cs_visible(const cs_cx *c, ccord_rect r)
{
  return r.y + r.h >= c->clip.y && r.y <= c->clip.y + c->clip.h;
}

static bool cs_add_hit(cs_cx *c, ccord_rect r, cs_act act, uint64_t arg, const char *why, const char *name)
{
  if (c->cs->nhits >= CS_HIT_MAX) return false;
  float x0 = fmaxf(r.x, c->clip.x), y0 = fmaxf(r.y, c->clip.y);
  float x1 = fminf(r.x + r.w, c->clip.x + c->clip.w), y1 = fminf(r.y + r.h, c->clip.y + c->clip.h);
  if (x1 <= x0 || y1 <= y0) return false;
  size_t idx = c->cs->nhits++;
  cs_hit *h = &c->cs->hits[idx];
  h->r = (ccord_rect){x0, y0, x1 - x0, y1 - y0};
  h->act = act;
  h->arg = arg;
  h->why = why;
  (void)snprintf(h->name, sizeof h->name, "%s", name ? name : "");
  return c->cs->hover == (int)idx;
}

static float cs_text(cs_cx *c, const char *s, float x, float y, float size, bool bold, ccord_color col, float max_w)
{
  ccord_text_style st = ui_style(c->ui, bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, size, col);
  if (s && s[0] != '\0') {
    if (max_w > 0.0f) ccord_text_draw_ellipsis(c->r, s, strlen(s), x, y, max_w, &st);
    else ccord_text_draw(c->r, s, strlen(s), x, y, &st, 0.0f);
  }
  return ui_line_h(c->ui, size);
}

static void cs_label(cs_cx *c, const char *s, float x, float y)
{
  (void)cs_text(c, s, x, y, uis_tokens_of(c->ui).fs_overline, true, c->ui->theme->c.text_muted, 0.0f);
}

// Buttons snap to the nearest token height (28 / 32 / 38): below 30 is small, below 35 medium, else large.
static uis_btn_size cs_btn_size(float h)
{
  return h < 30.0f ? UIS_BTN_S_SM : (h < 35.0f ? UIS_BTN_S_MD : UIS_BTN_S_LG);
}

static void cs_button(cs_cx *c, ccord_rect r, const char *label, uis_btn_kind kind, const char *why, cs_act act,
                      uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  uis_btn_size size = cs_btn_size(r.h);
  r.h = uis_button_height(size);
  if (!cs_visible(c, r)) return;
  bool hov = cs_add_hit(c, r, act, arg, why, name);
  uis_button(ui, c->r, r, label, kind, size, (uis_state){.hover = hov, .disabled = why != nullptr}, "csbtn",
             ui_name_id(name));
}

static void cs_chip(cs_cx *c, ccord_rect r, const char *label, bool on, const char *why, cs_act act, uint64_t arg,
                    const char *name)
{
  ccord_ui *ui = c->ui;
  r.h = UIS_H_BTN_SM;
  if (!cs_visible(c, r)) return;
  bool hov = cs_add_hit(c, r, act, arg, why, name);
  uis_chip(ui, c->r, r, label, (uis_state){.hover = hov, .selected = on, .disabled = why != nullptr}, "cschip", ui_name_id(name));
}

static void cs_toggle(cs_cx *c, ccord_rect r, bool on, const char *why, cs_act act, uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  r = uis_toggle_rect(r);
  if (!cs_visible(c, r)) return;
  bool hov = cs_add_hit(c, r, act, arg, why, name);
  uis_toggle(ui, c->r, r, on, (uis_state){.hover = hov, .disabled = why != nullptr}, "cssw", ui_name_id(name));
}

// A text input (uis_input). A field whose edit does not exist yet is drawn empty.
static void cs_field_box(cs_cx *c, ccord_rect r, cs_field f, const char *placeholder, const char *why, const char *name)
{
  ccord_ui *ui = c->ui;
  r.h = UIS_H_INPUT;
  if (!cs_visible(c, r)) return;
  bool focused = c->cs->focus == f && !why;
  (void)cs_add_hit(c, r, CA_FIELD, (uint64_t)f, why, name);
  ccord_text_edit blank = {0};
  ccord_text_edit *ed = cs_edit_for(c->cs, f);
  (void)uis_input(ui, c->r, r, ed ? ed : &blank, placeholder ? placeholder : "",
                  (uis_state){.focused = focused, .disabled = why != nullptr}, false, "csfocus", (uint64_t)f);
}

// ---------------------------------------------------------------- Overview page

static void draw_cs_overview(cs_cx *c, const ccord_channel *ch, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_channel_settings *cs = c->cs;
  float y = *py;

  (void)cs_text(c, "Overview", x, y, CS_TITLE(c), true, COL(text_header), w);
  y += 34.0f;

  cs_label(c, "CHANNEL NAME", x, y);
  y += 20.0f;
  cs_field_box(c, (ccord_rect){x, y, w, 38.0f}, CSF_NAME, "new-channel", nullptr, "field:name");
  y += 52.0f;

  bool has_topic = ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type);
  if (has_topic) {
    cs_label(c, ccord_discord_type_is_forum(ch->discord_type) ? "POST GUIDELINES" : "CHANNEL TOPIC", x, y);
    y += 20.0f;
    cs_field_box(c, (ccord_rect){x, y, w, 38.0f}, CSF_TOPIC, "Let everyone know how to use this channel!", nullptr,
                 "field:topic");
    y += 52.0f;
  }

  if (ch->kind != CCORD_CHANNEL_CATEGORY) {
    cs_label(c, "SLOWMODE", x, y);
    y += 20.0f;
    float cx = x;
    for (size_t i = 0; i < SLOWMODE_COUNT; i++) {
      float cw = 54.0f;
      if (cx + cw > x + w) {
        cx = x;
        y += 34.0f;
      }
      char nm[CS_NAME_LEN];
      cs_chip(c, (ccord_rect){cx, y, cw, 28.0f}, k_slowmode_labels[i], cs->ov_work.slowmode_s == k_slowmodes[i],
              nullptr, CA_SLOWMODE, k_slowmodes[i], NAME(nm, "slowmode:%u", (unsigned)k_slowmodes[i]));
      cx += cw + 6.0f;
    }
    y += 44.0f;
  }

  if (ch->kind != CCORD_CHANNEL_CATEGORY && !ccord_discord_type_is_thread(ch->discord_type)) {
    ccord_rect nr = {x, y, w, 48.0f};
    (void)cs_text(c, "Age-Restricted Channel", x, y + 4.0f, uis_tokens_of(ui).fs_body, true, COL(text_header), w - 60.0f);
    (void)cs_text(c, "Users will need to confirm they are of over legal age to view the content in this channel.",
                  x, y + 24.0f, CS_CAP(c), false, COL(text_muted), w - 60.0f);
    cs_toggle(c, (ccord_rect){x + w - 44.0f, y + 10.0f, 44.0f, 24.0f}, cs->ov_work.nsfw, nullptr, CA_NSFW, 0, "nsfw");
    (void)nr;
    y += 60.0f;
  }

  if (ch->kind == CCORD_CHANNEL_VOICE) {
    cs_label(c, "BITRATE", x, y);
    y += 20.0f;
    float cx = x;
    for (size_t i = 0; i < BITRATE_COUNT; i++) {
      float cw = 78.0f;
      if (cx + cw > x + w) {
        cx = x;
        y += 34.0f;
      }
      char nm[CS_NAME_LEN];
      cs_chip(c, (ccord_rect){cx, y, cw, 28.0f}, k_bitrate_labels[i], cs->ov_work.bitrate == k_bitrates[i],
              nullptr, CA_BITRATE, k_bitrates[i], NAME(nm, "bitrate:%u", (unsigned)k_bitrates[i]));
      cx += cw + 6.0f;
    }
    y += 44.0f;

    cs_label(c, "USER LIMIT", x, y);
    y += 20.0f;
    cx = x;
    for (size_t i = 0; i < USER_LIMIT_COUNT; i++) {
      float cw = i == 0 ? 84.0f : 44.0f;
      if (cx + cw > x + w) {
        cx = x;
        y += 34.0f;
      }
      char nm[CS_NAME_LEN];
      cs_chip(c, (ccord_rect){cx, y, cw, 28.0f}, k_user_limit_labels[i], cs->ov_work.user_limit == k_user_limits[i],
              nullptr, CA_USER_LIMIT, k_user_limits[i], NAME(nm, "user_limit:%u", (unsigned)k_user_limits[i]));
      cx += cw + 6.0f;
    }
    cs_button(c, (ccord_rect){cx + 6.0f, y, 28.0f, 28.0f}, "-", UIS_BTN_SECONDARY,
              cs->ov_work.user_limit == 0 ? "Already unlimited." : nullptr, CA_USER_LIMIT_STEP, 0, "user_limit:dec");
    cs_button(c, (ccord_rect){cx + 38.0f, y, 28.0f, 28.0f}, "+", UIS_BTN_SECONDARY,
              cs->ov_work.user_limit >= CCORD_ADMIN_USER_LIMIT_MAX ? "Maximum user limit reached." : nullptr,
              CA_USER_LIMIT_STEP, 1, "user_limit:inc");
    y += 44.0f;
  }

  if (ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type)) {
    cs_label(c, "HIDE AFTER INACTIVITY", x, y);
    y += 20.0f;
    float cx = x;
    for (size_t i = 0; i < ARCHIVE_COUNT; i++) {
      float cw = 88.0f;
      char nm[CS_NAME_LEN];
      cs_chip(c, (ccord_rect){cx, y, cw, 28.0f}, k_archive_labels[i], cs->ov_work.archive_min == k_archives[i],
              nullptr, CA_ARCHIVE, k_archives[i], NAME(nm, "archive:%u", (unsigned)k_archives[i]));
      cx += cw + 8.0f;
    }
    y += 44.0f;
  }

  if (ccord_discord_type_is_forum(ch->discord_type)) {
    cs_label(c, "TAGS", x, y);
    y += 22.0f;
    cs->dbg_rows = cs->ov_work.tag_count;
    for (size_t i = 0; i < cs->ov_work.tag_count; i++) {
      const ccord_forum_tag *tg = &cs->ov_work.tags[i];
      ccord_rect rr = {x, y, w, 38.0f};
      if (cs_visible(c, rr)) {
        ccord_draw_rrect(c->r, rr, uis_tokens_of(ui).r_control, COL(bg_secondary));
        char line[128];
        if (tg->emoji_name[0] != '\0') (void)snprintf(line, sizeof line, "%s %s", tg->emoji_name, tg->name);
        else (void)snprintf(line, sizeof line, "%s", tg->name);
        (void)cs_text(c, line, x + 12.0f, y + 10.0f, uis_tokens_of(ui).fs_body, false, COL(text_header), w - 210.0f);
        char nm[CS_NAME_LEN];
        cs_chip(c, (ccord_rect){x + w - 186.0f, y + 5.0f, 96.0f, 28.0f},
                tg->moderated ? "Moderated" : "Anyone", tg->moderated, nullptr, CA_TAG_MOD, i,
                NAME(nm, "tag_mod:%zu", i));
        cs_button(c, (ccord_rect){x + w - 82.0f, y + 5.0f, 74.0f, 28.0f}, "Remove", UIS_BTN_DANGER,
                  nullptr, CA_TAG_DEL, i, NAME(nm, "tag_del:%zu", i));
      }
      y += 44.0f;
    }
    if (cs->ov_work.tag_count < CCORD_FORUM_MAX_TAGS) {
      cs_field_box(c, (ccord_rect){x, y, w * 0.45f, 34.0f}, CSF_TAG_NAME, "New tag name", nullptr, "field:tag_name");
      cs_field_box(c, (ccord_rect){x + w * 0.47f, y, w * 0.20f, 34.0f}, CSF_TAG_EMOJI, "Emoji", nullptr,
                   "field:tag_emoji");
      cs_chip(c, (ccord_rect){x + w * 0.69f, y + 3.0f, 92.0f, 28.0f}, "Moderated", cs->tag_new_mod, nullptr,
              CA_TAG_NEW_MOD, 0, "tag_new_mod");
      cs_button(c, (ccord_rect){x + w - 84.0f, y + 3.0f, 84.0f, 28.0f}, "Add Tag", UIS_BTN_PRIMARY,
                nullptr, CA_TAG_ADD, 0, "tag_add");
      y += 46.0f;
    }
  }

  *py = y;
}

// ---------------------------------------------------------------- Permissions page (tri-state overwrites)

typedef struct cs_ow_target_item {
  ccord_id id;
  ccord_overwrite_kind kind;
  char label[128];
} cs_ow_target_item;

static size_t cs_build_overwrite_targets(const ccord_ui *ui, cs_ow_target_item *out, size_t cap)
{
  const struct ui_channel_settings *cs = ui->cs;
  const ccord_store *st = ui->d.store;
  size_t count = 0;
  if (cap == 0) return 0;

  out[count++] = (cs_ow_target_item){.id = cs->guild_id, .kind = CCORD_OVERWRITE_ROLE, .label = "@everyone"};

  size_t nows = 0;
  const ccord_overwrite *ows = ccord_store_channel_overwrites(st, cs->channel_id, &nows);
  for (size_t i = 0; ows && i < nows && count < cap; i++) {
    if (ows[i].id == cs->guild_id) continue;
    cs_ow_target_item it = {.id = ows[i].id, .kind = ows[i].kind};
    if (ows[i].kind == CCORD_OVERWRITE_ROLE) {
      const ccord_role *ro = ccord_store_role(st, cs->guild_id, ows[i].id);
      (void)snprintf(it.label, sizeof it.label, "@%s", (ro && ro->name && ro->name[0]) ? ro->name : "role");
    } else {
      const char *mn = ui_member_display_name(ui, cs->guild_id, ows[i].id);
      (void)snprintf(it.label, sizeof it.label, "%s", mn && mn[0] ? mn : "Member");
    }
    out[count++] = it;
  }

  if (cs->ow_sel_id != CCORD_ID_NONE && cs->ow_sel_id != cs->guild_id && count < cap) {
    bool present = false;
    for (size_t i = 0; i < count; i++) {
      if (out[i].id == cs->ow_sel_id) {
        present = true;
        break;
      }
    }
    if (!present) {
      cs_ow_target_item it = {.id = cs->ow_sel_id, .kind = cs->ow_sel_kind};
      if (cs->ow_sel_kind == CCORD_OVERWRITE_ROLE) {
        const ccord_role *ro = ccord_store_role(st, cs->guild_id, cs->ow_sel_id);
        (void)snprintf(it.label, sizeof it.label, "@%s", (ro && ro->name && ro->name[0]) ? ro->name : "role");
      } else {
        const char *mn = ui_member_display_name(ui, cs->guild_id, cs->ow_sel_id);
        (void)snprintf(it.label, sizeof it.label, "%s", mn && mn[0] ? mn : "Member");
      }
      out[count++] = it;
    }
  }
  return count;
}

// A tri-state permission button is a button: danger for deny, success for allow, secondary for inherit / unset.
static void draw_tristate_btn(cs_cx *c, ccord_rect r, const char *glyph, int kind, bool active, const char *why,
                              cs_act act, uint64_t bit, const char *name)
{
  // The active choice: red for deny, green for allow, the selected fill for inherit.
  uis_btn_kind bk = UIS_BTN_SECONDARY;
  if (active && kind < 0) bk = UIS_BTN_DANGER;
  else if (active && kind > 0) bk = UIS_BTN_SUCCESS;
  uis_btn_size size = cs_btn_size(r.h);
  r.h = uis_button_height(size);
  if (!cs_visible(c, r)) return;
  bool hov = cs_add_hit(c, r, act, bit, why, name);
  uis_button(c->ui, c->r, r, glyph, bk, size,
             (uis_state){.hover = hov, .selected = active && kind == 0, .disabled = why != nullptr}, "csbtn",
             ui_name_id(name));
}

static void draw_cs_permissions(cs_cx *c, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_channel_settings *cs = c->cs;
  float y = *py;

  (void)cs_text(c, "Channel Permissions", x, y, CS_TITLE(c), true, COL(text_header), w);
  y += 28.0f;
  (void)cs_text(c, "Use permissions to customise who can do what in this channel.", x, y, CS_CAP(c), false,
                COL(text_muted), w);
  y += 28.0f;

  cs_label(c, "ROLES / MEMBERS", x, y);
  cs_button(c, (ccord_rect){x + w - 154.0f, y - 4.0f, 154.0f, 26.0f},
            cs->ow_add_open ? "Close Add Menu" : "+ Add Role / Member",
            UIS_BTN_PRIMARY, nullptr, CA_OW_ADD_TOGGLE, 0, "ow_add_toggle");
  y += 28.0f;

  if (cs->ow_add_open) {
    size_t nroles = 0;
    const ccord_role *roles = ccord_store_roles(ui->d.store, cs->guild_id, &nroles);
    float cx = x;
    for (size_t i = 0; roles && i < nroles; i++) {
      if (roles[i].id == cs->guild_id) continue;
      if (cs_find_stored_overwrite(ui->d.store, cs->channel_id, roles[i].id, nullptr)) continue;
      char lbl[96], nm[CS_NAME_LEN];
      (void)snprintf(lbl, sizeof lbl, "+ @%s", roles[i].name ? roles[i].name : "role");
      float cw = 128.0f;
      if (cx + cw > x + w) {
        cx = x;
        y += 32.0f;
      }
      cs_chip(c, (ccord_rect){cx, y, cw, 26.0f}, lbl, false, nullptr, CA_OW_ADD_ROLE, roles[i].id,
              NAME(nm, "ow_add_role:%llu", (unsigned long long)roles[i].id));
      cx += cw + 6.0f;
    }
    if (cx > x) {
      cx = x;
      y += 32.0f;
    }
    size_t nmemb = ccord_store_member_count(ui->d.store, cs->guild_id);
    for (size_t i = 0; i < nmemb && i < 24u; i++) {
      const ccord_member *mb = ccord_store_member_at(ui->d.store, cs->guild_id, i);
      if (!mb || cs_find_stored_overwrite(ui->d.store, cs->channel_id, mb->user_id, nullptr)) continue;
      char lbl[96], nm[CS_NAME_LEN];
      (void)snprintf(lbl, sizeof lbl, "+ %s", ui_member_display_name(ui, cs->guild_id, mb->user_id));
      float cw = 128.0f;
      if (cx + cw > x + w) {
        cx = x;
        y += 32.0f;
      }
      cs_chip(c, (ccord_rect){cx, y, cw, 26.0f}, lbl, false, nullptr, CA_OW_ADD_MEMBER, mb->user_id,
              NAME(nm, "ow_add_member:%llu", (unsigned long long)mb->user_id));
      cx += cw + 6.0f;
    }
    if (cx > x) y += 32.0f;
    y += 8.0f;
  }

  cs_ow_target_item targets[CCORD_ADMIN_CLONE_OVERWRITES_MAX];
  size_t ntargets = cs_build_overwrite_targets(ui, targets, CCORD_ADMIN_CLONE_OVERWRITES_MAX);
  cs->dbg_rows = ntargets;
  float cx = x;
  for (size_t i = 0; i < ntargets; i++) {
    float cw = 132.0f;
    if (cx + cw > x + w) {
      cx = x;
      y += 34.0f;
    }
    char nm[CS_NAME_LEN];
    cs_chip(c, (ccord_rect){cx, y, cw, 28.0f}, targets[i].label, targets[i].id == cs->ow_sel_id, nullptr, CA_OW_TARGET,
            targets[i].id | ((uint64_t)targets[i].kind << 63u),
            NAME(nm, "ow_target:%llu", (unsigned long long)targets[i].id));
    cx += cw + 6.0f;
  }
  y += 40.0f;

  bool has_stored = cs_find_stored_overwrite(ui->d.store, cs->channel_id, cs->ow_sel_id, nullptr);
  if (has_stored) {
    cs_button(c, (ccord_rect){x + w - 154.0f, y - 4.0f, 154.0f, 28.0f}, "Remove Overwrite", UIS_BTN_DANGER, nullptr, CA_OW_DELETE, cs->ow_sel_id, "ow_delete");
    y += 32.0f;
  }

  ccord_id self = ccord_store_self_id(ui->d.store);
  bool exact = ccord_store_permissions_exact(ui->d.store, cs->guild_id, self);
  uint64_t ch_perms = cs_self_chan_perms(ui, cs->guild_id, cs->channel_id);

  size_t nperms = 0;
  const ccord_perm_info *tbl = ccord_perm_table(&nperms);
  ccord_perm_group last_group = CCORD_PERM_GROUP_COUNT;
  for (size_t i = 0; i < nperms; i++) {
    if (tbl[i].group != last_group) {
      last_group = tbl[i].group;
      y += 8.0f;
      char head[64];
      const char *gl = ccord_perm_group_label(last_group);
      size_t k = 0;
      for (; gl[k] && k + 1 < sizeof head; k++) head[k] = (char)((gl[k] >= 'a' && gl[k] <= 'z') ? gl[k] - 32 : gl[k]);
      head[k] = '\0';
      cs_label(c, head, x, y);
      y += 24.0f;
    }
    ccord_rect row = {x, y, w, CS_PERM_ROW_H};
    y += CS_PERM_ROW_H;
    if (!cs_visible(c, row)) continue;

    uint64_t bit = tbl[i].bit;
    bool is_allow = (cs->ow_work.allow & bit) != 0;
    bool is_deny = (cs->ow_work.deny & bit) != 0;
    bool is_inherit = !is_allow && !is_deny;
    const char *why = (exact && !ccord_perm_has(ch_perms, bit))
                        ? "You can't grant or deny permissions you don't hold in this channel."
                        : nullptr;

    (void)cs_text(c, tbl[i].label, row.x, row.y + 6.0f, uis_tokens_of(ui).fs_body, true, COL(text_header), w - 120.0f);
    (void)cs_text(c, tbl[i].description, row.x, row.y + 6.0f + ui_line_h(ui, uis_tokens_of(ui).fs_body), CS_CAP(c),
                  false, COL(text_muted), w - 120.0f);

    float bx = row.x + w - 102.0f;
    float by = row.y + 12.0f;
    char nm[CS_NAME_LEN];
    draw_tristate_btn(c, (ccord_rect){bx, by, 30.0f, 28.0f}, "X", -1, is_deny, why, CA_OW_DENY, bit,
                      NAME(nm, "ow_deny:%llx", (unsigned long long)bit));
    draw_tristate_btn(c, (ccord_rect){bx + 34.0f, by, 30.0f, 28.0f}, "/", 0, is_inherit, why, CA_OW_INHERIT, bit,
                      NAME(nm, "ow_inherit:%llx", (unsigned long long)bit));
    draw_tristate_btn(c, (ccord_rect){bx + 68.0f, by, 30.0f, 28.0f}, "V", 1, is_allow, why, CA_OW_ALLOW, bit,
                      NAME(nm, "ow_allow:%llx", (unsigned long long)bit));
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }

  *py = y;
}

// ---------------------------------------------------------------- Invites page

static void draw_cs_invites(cs_cx *c, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_channel_settings *cs = c->cs;
  float y = *py;

  (void)cs_text(c, "Channel Invites", x, y, CS_TITLE(c), true, COL(text_header), w - 100.0f);
  cs_button(c, (ccord_rect){x + w - 88.0f, y, 88.0f, 30.0f}, "Refresh", UIS_BTN_SECONDARY,
            nullptr, CA_REFRESH, 0, "refresh");
  y += 40.0f;

  bool can_create = ccord_store_has_permission(ui->d.store, cs->guild_id, cs->channel_id,
                                               ccord_store_self_id(ui->d.store), CCORD_PERM_CREATE_INSTANT_INVITE);
  if (can_create) {
    char age_lbl[64], use_lbl[64];
    (void)snprintf(age_lbl, sizeof age_lbl, "Expire: %s", k_cs_inv_age_labels[cs->inv_age_idx]);
    (void)snprintf(use_lbl, sizeof use_lbl, "Max: %s", k_cs_inv_use_labels[cs->inv_uses_idx]);
    cs_chip(c, (ccord_rect){x, y, 160.0f, 30.0f}, age_lbl, false, nullptr, CA_INV_AGE, 0, "inv_age");
    cs_chip(c, (ccord_rect){x + 168.0f, y, 150.0f, 30.0f}, use_lbl, false, nullptr, CA_INV_USES, 0, "inv_uses");
    cs_button(c, (ccord_rect){x + 326.0f, y, 128.0f, 30.0f}, "Create Invite", UIS_BTN_PRIMARY,
              nullptr, CA_INV_CREATE, 0, "inv_create");
    y += 46.0f;
  }

  size_t count = 0;
  const ccord_invite *invs = ccord_store_channel_invites(ui->d.store, cs->guild_id, cs->channel_id, &count);
  cs->dbg_rows = count;
  if (!ccord_store_channel_invites_loaded(ui->d.store, cs->guild_id, cs->channel_id)) {
    const char *msg = cs->invites_fail_ms ? "Couldn't load channel invites. Try Refresh." : "Loading channel invites...";
    (void)cs_text(c, msg, x, y, uis_tokens_of(ui).fs_body, false, COL(text_muted), w);
    *py = y + 32.0f;
    return;
  }
  if (count == 0) {
    (void)cs_text(c, "No active invites for this channel.", x, y, uis_tokens_of(ui).fs_body, false, COL(text_muted), w);
    *py = y + 32.0f;
    return;
  }

  for (size_t i = 0; i < count; i++) {
    const ccord_invite *inv = &invs[i];
    ccord_rect row = {x, y, w, 46.0f};
    y += 50.0f;
    if (!cs_visible(c, row)) continue;
    ccord_draw_rrect(c->r, row, uis_tokens_of(ui).r_control, COL(bg_secondary));
    char code_line[128], meta_line[128];
    (void)snprintf(code_line, sizeof code_line, "discord.gg/%s", inv->code);
    // Streamer Mode: mask the invite code while Hide Instant Invite Links is on.
    char code_masked[160];
    size_t code_ml = ui_streamer_mask_invites(ui, code_line, strlen(code_line), code_masked, sizeof code_masked, true);
    const char *shown_code = code_ml > 0 ? code_masked : code_line;
    const ccord_user *u = inv->inviter_id ? ccord_store_user(ui->d.store, inv->inviter_id) : nullptr;
    if (inv->max_uses > 0) {
      (void)snprintf(meta_line, sizeof meta_line, "by %s  •  %u / %u uses",
                     (u && u->name) ? u->name : "Unknown", inv->uses, inv->max_uses);
    } else {
      (void)snprintf(meta_line, sizeof meta_line, "by %s  •  %u uses",
                     (u && u->name) ? u->name : "Unknown", inv->uses);
    }
    (void)cs_text(c, shown_code, row.x + 12.0f, row.y + 5.0f, uis_tokens_of(ui).fs_body, true, COL(text_header), w - 180.0f);
    (void)cs_text(c, meta_line, row.x + 12.0f, row.y + 24.0f, CS_CAP(c), false, COL(text_muted), w - 180.0f);
    char nm[CS_NAME_LEN];
    cs_button(c, (ccord_rect){row.x + w - 156.0f, row.y + 9.0f, 68.0f, 28.0f}, "Copy", UIS_BTN_SECONDARY, nullptr, CA_INV_COPY, i, NAME(nm, "inv_copy:%s", inv->code));
    cs_button(c, (ccord_rect){row.x + w - 80.0f, row.y + 9.0f, 70.0f, 28.0f}, "Revoke", UIS_BTN_DANGER, nullptr, CA_INV_REVOKE, i, NAME(nm, "inv_revoke:%s", inv->code));
  }

  *py = y;
}

// ---------------------------------------------------------------- full draw

void ui_channel_settings_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui_channel_settings_is_open(ui)) return;
  cs_sync(ui);
  if (!ui_channel_settings_is_open(ui)) return;

  struct ui_channel_settings *cs = ui->cs;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, cs->channel_id);
  if (!ch) return;

  cs->nhits = 0;
  cs->dbg_rows = 0;
  float ww = ui->win_w, wh = ui->win_h;
  ccord_draw_rect(r, (ccord_rect){0, 0, ww, wh}, COL(bg_primary));

  float side_w = fmaxf(210.0f, floorf(ww * 0.25f));
  if (side_w > 280.0f) side_w = 280.0f;
  ccord_draw_rect(r, (ccord_rect){0, 0, side_w, wh}, COL(bg_secondary));

  cs_cx side_cx = {.ui = ui, .cs = cs, .r = r, .clip = {0, 0, side_w, wh}};
  float sx = side_w - 192.0f;
  if (sx < 12.0f) sx = 12.0f;
  float sw = side_w - sx - 12.0f;
  float sy = 44.0f;

  char title[160];
  const char *prefix = ch->kind == CCORD_CHANNEL_VOICE ? "" : (ch->kind == CCORD_CHANNEL_CATEGORY ? "" : "#");
  (void)snprintf(title, sizeof title, "%s%s", prefix, ch->name ? ch->name : "channel");
  (void)cs_text(&side_cx, title, sx + 8.0f, sy, uis_tokens_of(ui).fs_control, true, COL(text_header), sw - 16.0f);
  sy += 26.0f;

  for (cs_page p = CS_OVERVIEW; p < CS_PAGE_COUNT; p++) {
    if (!cs_page_allowed(ui, ch, p)) continue;
    ccord_rect pr = {sx, sy, sw, 32.0f};
    char nm[CS_NAME_LEN];
    bool hov = cs_add_hit(&side_cx, pr, CA_PAGE, (uint64_t)p, nullptr, NAME(nm, "page:%s", k_cs_page_names[p]));
    ui_row_state rs = ui_row_state_of(ui, "cspage", (uint64_t)p, hov, cs->page == p, COL(bg_secondary));
    uis_list_row_fill(ui, r, pr, (uis_state){.hover = hov, .selected = cs->page == p}, "cspage", (uint64_t)p);
    ccord_color tc = ui_row_state_text(&rs, COL(text_secondary), COL(text_interactive_hover), COL(text_interactive_active));
    (void)cs_text(&side_cx, k_cs_page_labels[p], pr.x + 10.0f, pr.y + 6.0f, uis_tokens_of(ui).fs_body, cs->page == p, tc, sw - 20.0f);
    sy += 34.0f;
  }

  if (ccord_perm_has(cs_self_chan_perms(ui, cs->guild_id, cs->channel_id), CCORD_PERM_MANAGE_CHANNELS)) {
    sy += 8.0f;
    uis_divider(ui, r, sx + 8.0f, sy, sw - 16.0f);
    sy += 10.0f;
    ccord_rect dr = {sx, sy, sw, 32.0f};
    bool hov = cs_add_hit(&side_cx, dr, CA_DELETE_CHANNEL, 0, nullptr, "delete_channel");
    // A destructive menu row: red label, red fill on hover (the shared menu-row style).
    uis_menu_row(ui, r, dr, "Delete Channel", (uis_state){.hover = hov}, false, false, true, "csdelete", 0);
  }

  // Content area + ESC close button
  float content_x = side_w + 40.0f;
  float content_w = fminf(660.0f, ww - content_x - 96.0f);
  if (content_w < 260.0f) content_w = fmaxf(180.0f, ww - content_x - 24.0f);
  bool dirty = cs_unsaved(ui);
  float view_top = 44.0f;
  float view_h = wh - view_top - (dirty ? 76.0f : 24.0f);
  if (view_h < 80.0f) view_h = 80.0f;
  cs->view_h = view_h;

  ccord_rect clip = {side_w, view_top, ww - side_w - 72.0f, view_h};
  ccord_push_clip(r, clip);
  cs_cx cx = {.ui = ui, .cs = cs, .r = r, .clip = clip};
  float y = view_top - cs->scroll;
  if (cs->page == CS_OVERVIEW) draw_cs_overview(&cx, ch, content_x, content_w, &y);
  else if (cs->page == CS_PERMISSIONS) draw_cs_permissions(&cx, content_x, content_w, &y);
  else if (cs->page == CS_INVITES) draw_cs_invites(&cx, content_x, content_w, &y);
  cs->content_h = (y + cs->scroll) - view_top + 24.0f;
  float max_scroll = fmaxf(0.0f, cs->content_h - cs->view_h);
  if (cs->scroll > max_scroll) cs->scroll = max_scroll;
  ccord_pop_clip(r);

  // Close (ESC) button
  cs_cx top_cx = {.ui = ui, .cs = cs, .r = r, .clip = {0, 0, ww, wh}};
  ccord_rect close_r = {ww - 60.0f, 40.0f, UIS_ICON_BTN, UIS_ICON_BTN};
  bool close_hov = cs_add_hit(&top_cx, close_r, CA_CLOSE, 0, nullptr, "close");
  ccord_color close_ic = uis_icon_button(ui, r, close_r, (uis_state){.hover = close_hov}, "csclose", 0);
  ui_icon_close(r, close_r.x + UIS_ICON_BTN * 0.5f, close_r.y + UIS_ICON_BTN * 0.5f, 14.0f, close_ic);
  (void)cs_text(&top_cx, "ESC", close_r.x + 4.0f, close_r.y + UIS_ICON_BTN + 4.0f, uis_tokens_of(ui).fs_caption, true, COL(text_muted), 36.0f);

  // Unsaved Changes bar: a floating strip (popover frame, token radius), red while it flashes
  if (dirty) {
    bool flash = cs->flash_until_ms > ui->now_ms;
    ccord_rect bar = {content_x, wh - 62.0f, content_w, 48.0f};
    uis_frame(ui, r, bar, uis_tokens_of(ui).r_surface, flash ? COL(danger) : COL(bg_floating), UIS_ELEV_POPOVER);
    (void)cs_text(&top_cx, "Careful — you have unsaved changes!", bar.x + 16.0f, bar.y + 14.0f, uis_tokens_of(ui).fs_body,
                  false, flash ? COL(text_on_brand) : COL(text_header), bar.w - 220.0f);
    cs_button(&top_cx, (ccord_rect){bar.x + bar.w - 204.0f, bar.y + 9.0f, 72.0f, 30.0f}, "Reset", UIS_BTN_SECONDARY, nullptr, CA_RESET, 0, "reset");
    cs_button(&top_cx, (ccord_rect){bar.x + bar.w - 124.0f, bar.y + 9.0f, 112.0f, 30.0f}, "Save Changes",
              UIS_BTN_SUCCESS, nullptr, CA_SAVE, 0, "save");
  }
}

// ---------------------------------------------------------------- actions & submits

static void cs_after_edit(ccord_ui *ui)
{
  struct ui_channel_settings *cs = ui->cs;
  if (cs->focus == CSF_NAME) {
    (void)snprintf(cs->ov_work.name, sizeof cs->ov_work.name, "%s", ccord_text_edit_cstr(&cs->name_edit));
  } else if (cs->focus == CSF_TOPIC) {
    (void)snprintf(cs->ov_work.topic, sizeof cs->ov_work.topic, "%s", ccord_text_edit_cstr(&cs->topic_edit));
  }
  ui->dirty = true;
}

static void cs_reset(ccord_ui *ui)
{
  struct ui_channel_settings *cs = ui->cs;
  if (cs->page == CS_OVERVIEW) {
    cs->ov_work = cs->ov_base;
    cs_set_edit(&cs->name_edit, cs->ov_work.name);
    cs_set_edit(&cs->topic_edit, cs->ov_work.topic);
    cs->focus = CSF_NONE;
  } else if (cs->page == CS_PERMISSIONS) {
    cs->ow_work = cs->ow_base;
  }
}

static void cs_save(ccord_ui *ui)
{
  struct ui_channel_settings *cs = ui->cs;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, cs->channel_id);
  if (!ch || !cs_unsaved(ui)) return;

  ccord_admin_request req;
  ccord_error err;
  if (cs->page == CS_OVERVIEW) {
    ccord_admin_channel_edit ed = CCORD_ADMIN_CHANNEL_EDIT_INIT;
    if (strcmp(cs->ov_work.name, cs->ov_base.name) != 0) ed.name = cs->ov_work.name;
    bool has_topic = ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type);
    if (has_topic && strcmp(cs->ov_work.topic, cs->ov_base.topic) != 0) ed.topic = cs->ov_work.topic;
    if (ch->kind != CCORD_CHANNEL_CATEGORY && !ccord_discord_type_is_thread(ch->discord_type) &&
        cs->ov_work.nsfw != cs->ov_base.nsfw) {
      ed.nsfw = cs->ov_work.nsfw ? 1 : 0;
    }
    if (ch->kind != CCORD_CHANNEL_CATEGORY && cs->ov_work.slowmode_s != cs->ov_base.slowmode_s) {
      ed.slowmode_s = (int32_t)cs->ov_work.slowmode_s;
    }
    if (ch->kind == CCORD_CHANNEL_VOICE) {
      if (cs->ov_work.bitrate != cs->ov_base.bitrate) ed.bitrate = (int32_t)cs->ov_work.bitrate;
      if (cs->ov_work.user_limit != cs->ov_base.user_limit) ed.user_limit = (int32_t)cs->ov_work.user_limit;
    }
    if (ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type) &&
        cs->ov_work.archive_min != cs->ov_base.archive_min) {
      ed.default_auto_archive_duration = (int32_t)cs->ov_work.archive_min;
    }
    if (ccord_discord_type_is_forum(ch->discord_type) && !cs_tags_eq(&cs->ov_work, &cs->ov_base)) {
      ed.has_available_tags = true;
      ed.available_tags = cs->ov_work.tags;
      ed.available_tag_count = cs->ov_work.tag_count;
    }
    if (ccord_admin_edit_channel(&req, cs->guild_id, cs->channel_id, &ed, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't save channel: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }

  if (cs->page == CS_PERMISSIONS) {
    ccord_overwrite ow = {
      .id = cs->ow_sel_id,
      .kind = cs->ow_sel_kind,
      .allow = cs->ow_work.allow,
      .deny = cs->ow_work.deny,
    };
    if (ccord_admin_put_channel_overwrite(&req, cs->guild_id, cs->channel_id, &ow, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't save channel permissions: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
  }
}

static void cs_try_close(ccord_ui *ui)
{
  if (cs_unsaved(ui)) {
    ui->cs->flash_until_ms = ui->now_ms + CS_FLASH_MS;
    ui->dirty = true;
    return;
  }
  ui_channel_settings_close(ui);
}

static void cs_add_forum_tag(ccord_ui *ui)
{
  struct ui_channel_settings *cs = ui->cs;
  if (cs->ov_work.tag_count >= CCORD_FORUM_MAX_TAGS) return;
  const char *tname = ccord_text_edit_cstr(&cs->tag_name_edit);
  if (!tname || tname[0] == '\0') {
    ui_notice(ui, "Enter a tag name first.");
    return;
  }
  ccord_forum_tag tg = {0};
  (void)snprintf(tg.name, sizeof tg.name, "%s", tname);
  (void)snprintf(tg.emoji_name, sizeof tg.emoji_name, "%s", ccord_text_edit_cstr(&cs->tag_emoji_edit));
  tg.moderated = cs->tag_new_mod;
  cs->ov_work.tags[cs->ov_work.tag_count++] = tg;
  ccord_text_edit_clear(&cs->tag_name_edit);
  ccord_text_edit_clear(&cs->tag_emoji_edit);
  cs->tag_new_mod = false;
  cs->focus = CSF_NONE;
}

static void cs_activate(ccord_ui *ui, const cs_hit *h)
{
  struct ui_channel_settings *cs = ui->cs;
  if (h->why) {
    ui_notice(ui, "%s", h->why);
    return;
  }
  switch (h->act) {
    case CA_CLOSE: cs_try_close(ui); break;
    case CA_PAGE:
      if (cs_unsaved(ui)) {
        cs->flash_until_ms = ui->now_ms + CS_FLASH_MS;
      } else {
        cs_enter_page(ui, (cs_page)h->arg);
      }
      break;
    case CA_DELETE_CHANNEL: {
      const ccord_channel *ch = ccord_store_channel(ui->d.store, cs->channel_id);
      if (ch) {
        char lbl[CCORD_NAME_MAX_BYTES + 2];
        (void)snprintf(lbl, sizeof lbl, "%s%s", ch->kind == CCORD_CHANNEL_TEXT ? "#" : "", ch->name ? ch->name : "");
        ui_confirm_open(ui, CCORD_ADMIN_DELETE_CHANNEL, cs->guild_id, cs->channel_id, lbl);
      }
      break;
    }
    case CA_FIELD: {
      cs->focus = (cs_field)h->arg;
      ccord_text_edit *ed = cs_edit_for(cs, cs->focus);
      if (ed) ccord_text_edit_set_caret(ed, ed->len, false);
      break;
    }
    case CA_SAVE: cs_save(ui); break;
    case CA_RESET: cs_reset(ui); break;
    case CA_NSFW: cs->ov_work.nsfw = !cs->ov_work.nsfw; break;
    case CA_SLOWMODE: cs->ov_work.slowmode_s = (uint32_t)h->arg; break;
    case CA_BITRATE: cs->ov_work.bitrate = (uint32_t)h->arg; break;
    case CA_USER_LIMIT: cs->ov_work.user_limit = (uint32_t)h->arg; break;
    case CA_USER_LIMIT_STEP:
      if (h->arg == 0 && cs->ov_work.user_limit > 0) cs->ov_work.user_limit--;
      else if (h->arg == 1 && cs->ov_work.user_limit < CCORD_ADMIN_USER_LIMIT_MAX) cs->ov_work.user_limit++;
      break;
    case CA_ARCHIVE: cs->ov_work.archive_min = (uint32_t)h->arg; break;
    case CA_TAG_MOD:
      if (h->arg < cs->ov_work.tag_count) cs->ov_work.tags[h->arg].moderated = !cs->ov_work.tags[h->arg].moderated;
      break;
    case CA_TAG_DEL:
      if (h->arg < cs->ov_work.tag_count) {
        size_t idx = (size_t)h->arg;
        memmove(&cs->ov_work.tags[idx], &cs->ov_work.tags[idx + 1u],
                (cs->ov_work.tag_count - idx - 1u) * sizeof(ccord_forum_tag));
        cs->ov_work.tag_count--;
      }
      break;
    case CA_TAG_NEW_MOD: cs->tag_new_mod = !cs->tag_new_mod; break;
    case CA_TAG_ADD: cs_add_forum_tag(ui); break;
    case CA_OW_TARGET:
      if (cs_unsaved(ui)) {
        cs->flash_until_ms = ui->now_ms + CS_FLASH_MS;
      } else {
        ccord_overwrite_kind kind = (h->arg >> 63u) ? CCORD_OVERWRITE_MEMBER : CCORD_OVERWRITE_ROLE;
        ccord_id tid = h->arg & ~(1ULL << 63u);
        cs_select_overwrite_target(ui, tid, kind, false);
      }
      break;
    case CA_OW_ADD_TOGGLE: cs->ow_add_open = !cs->ow_add_open; break;
    case CA_OW_ADD_ROLE:
      if (cs_unsaved(ui)) cs->flash_until_ms = ui->now_ms + CS_FLASH_MS;
      else {
        cs->ow_add_open = false;
        cs_select_overwrite_target(ui, h->arg, CCORD_OVERWRITE_ROLE, true);
      }
      break;
    case CA_OW_ADD_MEMBER:
      if (cs_unsaved(ui)) cs->flash_until_ms = ui->now_ms + CS_FLASH_MS;
      else {
        cs->ow_add_open = false;
        cs_select_overwrite_target(ui, h->arg, CCORD_OVERWRITE_MEMBER, true);
      }
      break;
    case CA_OW_DELETE: {
      ccord_admin_request req;
      ccord_error err;
      if (ccord_admin_delete_channel_overwrite(&req, cs->guild_id, cs->channel_id, h->arg, nullptr, &err) != CCORD_OK) {
        ui_notice(ui, "Couldn't remove overwrite: %s", err.message);
        break;
      }
      ui_admin_submit(ui, &req);
      break;
    }
    case CA_OW_DENY:
      cs->ow_work.deny |= h->arg;
      cs->ow_work.allow &= ~h->arg;
      break;
    case CA_OW_INHERIT:
      cs->ow_work.allow &= ~h->arg;
      cs->ow_work.deny &= ~h->arg;
      break;
    case CA_OW_ALLOW:
      cs->ow_work.allow |= h->arg;
      cs->ow_work.deny &= ~h->arg;
      break;
    case CA_INV_AGE: cs->inv_age_idx = (cs->inv_age_idx + 1u) % CS_INV_AGE_COUNT; break;
    case CA_INV_USES: cs->inv_uses_idx = (cs->inv_uses_idx + 1u) % CS_INV_USES_COUNT; break;
    case CA_INV_CREATE: {
      ccord_admin_request req;
      ccord_error err;
      if (ccord_admin_create_invite(&req, cs->guild_id, cs->channel_id, k_cs_inv_ages[cs->inv_age_idx],
                                    k_cs_inv_uses[cs->inv_uses_idx], false, &err) != CCORD_OK) {
        ui_notice(ui, "Couldn't create invite: %s", err.message);
        break;
      }
      ui_admin_submit(ui, &req);
      break;
    }
    case CA_INV_COPY: {
      size_t n = 0;
      const ccord_invite *invs = ccord_store_channel_invites(ui->d.store, cs->guild_id, cs->channel_id, &n);
      if (invs && h->arg < n && ui->d.host.clipboard_set) {
        char url[128];
        (void)snprintf(url, sizeof url, "https://discord.gg/%s", invs[h->arg].code);
        ui->d.host.clipboard_set(ui->d.host.ud, url);
        ui_notice(ui, "Copied invite link");
      }
      break;
    }
    case CA_INV_REVOKE: {
      size_t n = 0;
      const ccord_invite *invs = ccord_store_channel_invites(ui->d.store, cs->guild_id, cs->channel_id, &n);
      if (invs && h->arg < n) {
        ccord_admin_request req;
        ccord_error err;
        if (ccord_admin_revoke_invite(&req, cs->guild_id, invs[h->arg].code, nullptr, &err) != CCORD_OK) {
          ui_notice(ui, "Couldn't revoke invite: %s", err.message);
          break;
        }
        ui_admin_submit(ui, &req);
      }
      break;
    }
    case CA_REFRESH: cs_request_invites(ui, true); break;
    case CA_NONE: break;
  }
  ui->dirty = true;
}

// ---------------------------------------------------------------- input

static int cs_hit_at(const struct ui_channel_settings *cs, float x, float y)
{
  for (size_t i = cs->nhits; i > 0; i--) {
    if (ui_in_rect(x, y, cs->hits[i - 1].r)) return (int)(i - 1);
  }
  return -1;
}

// Wheel notch over the page: the same bound as cs_scroll_by, but the distance glides (scroll.h).
static void cs_wheel_by(ccord_ui *ui, const ccord_input_event *ev, float dy)
{
  struct ui_channel_settings *cs = ui->cs;
  ccord_scroller_scroll(&cs->glide, &cs->scroll, dy, fmaxf(0.0f, cs->content_h - cs->view_h), ui_wheel_direct(ui, ev));
  ui->dirty = true;
}

// A key step cancels the wheel glide, then moves from the offset at the press (scroll.h).
static void cs_scroll_by(ccord_ui *ui, float dy)
{
  struct ui_channel_settings *cs = ui->cs;
  ccord_scroller_cancel(&cs->glide);
  float max = fmaxf(0.0f, cs->content_h - cs->view_h);
  cs->scroll = fminf(fmaxf(cs->scroll + dy, 0.0f), max);
  ui->dirty = true;
}

static bool cs_handle_field_key(ccord_ui *ui, const ccord_input_event *ev)
{
  struct ui_channel_settings *cs = ui->cs;
  ccord_text_edit *ed = cs_edit_for(cs, cs->focus);
  if (!ed) return false;
  bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
  bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
  switch (ev->key) {
    case CCORD_KEY_BACKSPACE:
      if (ctrl) ccord_text_edit_backspace_word(ed);
      else ccord_text_edit_backspace(ed);
      break;
    case CCORD_KEY_DELETE:
      if (ctrl) ccord_text_edit_delete_word(ed);
      else ccord_text_edit_delete(ed);
      break;
    case CCORD_KEY_LEFT: ccord_text_edit_move(ed, ctrl ? -3 : -1, shift); break;
    case CCORD_KEY_RIGHT: ccord_text_edit_move(ed, ctrl ? 3 : 1, shift); break;
    case CCORD_KEY_HOME: ccord_text_edit_move(ed, -2, shift); break;
    case CCORD_KEY_END: ccord_text_edit_move(ed, 2, shift); break;
    case CCORD_KEY_A:
      if (!ctrl) return true;
      ccord_text_edit_select_all(ed);
      break;
    case CCORD_KEY_C:
    case CCORD_KEY_X: {
      if (!ctrl) return true;
      char *sel = ccord_text_edit_copy_selection(ed);
      if (sel) {
        if (ui->d.host.clipboard_set) ui->d.host.clipboard_set(ui->d.host.ud, sel);
        ccord_free(sel);
        if (ev->key == CCORD_KEY_X) ccord_text_edit_delete(ed);
      }
      break;
    }
    case CCORD_KEY_V: {
      if (!ctrl) return true;
      char *clip = ui->d.host.clipboard_get ? ui->d.host.clipboard_get(ui->d.host.ud) : nullptr;
      if (clip) {
        for (char *p = clip; *p; p++) {
          if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
        }
        ccord_error err;
        (void)ccord_text_edit_insert(ed, clip, strlen(clip), &err);
        ccord_free(clip);
      }
      break;
    }
    case CCORD_KEY_RETURN:
      if (cs->focus == CSF_TAG_NAME || cs->focus == CSF_TAG_EMOJI) {
        cs_add_forum_tag(ui);
      } else {
        cs->focus = CSF_NONE;
      }
      ui->dirty = true;
      return true;
    case CCORD_KEY_ESCAPE:
    case CCORD_KEY_TAB:
      cs->focus = CSF_NONE;
      ui->dirty = true;
      return true;
    default: return true;
  }
  cs_after_edit(ui);
  return true;
}

bool ui_channel_settings_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui_channel_settings_is_open(ui) || !ev) return false;
  struct ui_channel_settings *cs = ui->cs;
  switch (ev->type) {
    case CCORD_IN_MOUSE_MOVE: {
      int h = cs_hit_at(cs, ev->x, ev->y);
      bool changed = h != cs->hover;
      cs->hover = h;
      return changed;
    }
    case CCORD_IN_MOUSE_DOWN: {
      if (ev->button != 1) return true;
      int h = cs_hit_at(cs, ev->x, ev->y);
      if (h < 0 || cs->hits[h].act != CA_FIELD) cs->focus = CSF_NONE;
      if (h >= 0) {
        cs_hit hit = cs->hits[h];
        cs_activate(ui, &hit);
      }
      ui->dirty = true;
      return true;
    }
    case CCORD_IN_WHEEL:
      cs_wheel_by(ui, ev, -ev->wheel * 52.0f);
      return true;
    case CCORD_IN_KEY_DOWN: {
      if (cs->focus != CSF_NONE) return cs_handle_field_key(ui, ev);
      bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
      switch (ev->key) {
        case CCORD_KEY_ESCAPE: cs_try_close(ui); break;
        case CCORD_KEY_S:
          if (ctrl) cs_save(ui);
          break;
        case CCORD_KEY_DOWN: cs_scroll_by(ui, 48.0f); break;
        case CCORD_KEY_UP: cs_scroll_by(ui, -48.0f); break;
        case CCORD_KEY_PAGEDOWN: cs_scroll_by(ui, cs->view_h * 0.85f); break;
        case CCORD_KEY_PAGEUP: cs_scroll_by(ui, -cs->view_h * 0.85f); break;
        default: break;
      }
      ui->dirty = true;
      return true;
    }
    case CCORD_IN_TEXT: {
      ccord_text_edit *ed = cs_edit_for(cs, cs->focus);
      if (!ed || ev->text[0] == '\0') return true;
      ccord_error err;
      (void)ccord_text_edit_insert(ed, ev->text, strlen(ev->text), &err);
      cs_after_edit(ui);
      return true;
    }
    default: return false;
  }
}
