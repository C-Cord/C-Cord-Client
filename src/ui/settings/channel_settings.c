// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "channel_settings_internal.h"

static const char *const k_cs_page_names[CS_PAGE_COUNT] = {"overview", "permissions", "invites"};
static const char *const k_cs_page_labels[CS_PAGE_COUNT] = {"Overview", "Permissions", "Invites"};

const uint32_t cs_k_slowmodes[12] = {0, 5, 10, 15, 30, 60, 120, 300, 600, 900, 3600, 21600};
const char *const cs_k_slowmode_labels[12] = {
  "Off", "5s", "10s", "15s", "30s", "1m", "2m", "5m", "10m", "15m", "1h", "6h"
};

const uint32_t cs_k_bitrates[6] = {32000, 64000, 96000, 128000, 256000, 384000};
const char *const cs_k_bitrate_labels[6] = {"32 kbps", "64 kbps", "96 kbps", "128 kbps", "256 kbps", "384 kbps"};

const uint32_t cs_k_user_limits[7] = {0, 2, 5, 10, 25, 50, 99};
const char *const cs_k_user_limit_labels[7] = {"Unlimited", "2", "5", "10", "25", "50", "99"};

const uint32_t cs_k_archives[4] = {60, 1440, 4320, 10080};
const char *const cs_k_archive_labels[4] = {"1 Hour", "24 Hours", "3 Days", "1 Week"};

static const uint32_t k_cs_inv_ages[7] = {1800, 3600, 21600, 43200, 86400, 604800, 0};
const char *const cs_k_cs_inv_age_labels[7] = {
  "30 minutes", "1 hour", "6 hours", "12 hours", "1 day", "7 days", "Never"
};
static const uint32_t k_cs_inv_uses[7] = {0, 1, 5, 10, 25, 50, 100};
const char *const cs_k_cs_inv_use_labels[7] = {
  "No limit", "1 use", "5 uses", "10 uses", "25 uses", "50 uses", "100 uses"
};

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
  ccord_free(ui->cs->ow_targets);
  ccord_free(ui->cs->ow_add_roles);
  ccord_free(ui->cs->ow_add_members);
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

ccord_text_edit *cs_edit_for(struct ui_channel_settings *cs, cs_field f)
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

uint64_t cs_cached_chan_perms(const ccord_ui *ui, struct ui_channel_settings *cs)
{
  uint64_t rev = ccord_store_revision(ui->d.store);
  if (cs->perm_cache_valid && cs->perm_cache_rev == rev && cs->perm_cache_now_ms == ui->now_unix_ms) {
    return cs->cached_perms;
  }
  ccord_id self = ccord_store_self_id(ui->d.store);
  cs->cached_perms = ccord_store_permissions(ui->d.store, cs->guild_id, cs->channel_id, self, ui->now_unix_ms);
  cs->cached_exact = ccord_store_permissions_exact(ui->d.store, cs->guild_id, self);
  cs->perm_cache_rev = rev;
  cs->perm_cache_now_ms = ui->now_unix_ms;
  cs->perm_cache_valid = true;
  return cs->cached_perms;
}

static bool cs_page_allowed_with(const ccord_channel *ch, uint64_t perms, cs_page p)
{
  if (!ch || ch->guild_id == CCORD_ID_NONE || ch->guild_id == CCORD_DM_GUILD_ID) return false;
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
  uint64_t perms = cs_self_chan_perms(ui, guild_id, channel_id);
  for (cs_page p = CS_OVERVIEW; p < CS_PAGE_COUNT; p++) {
    if (cs_page_allowed_with(ch, perms, p)) return true;
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
  if (!ch || ch->guild_id != cs->guild_id || ccord_discord_type_is_thread(ch->discord_type)) {
    ui_channel_settings_close(ui);
    return;
  }
  uint64_t perms = cs_cached_chan_perms(ui, cs);
  bool any_page = false;
  for (cs_page p = CS_OVERVIEW; p < CS_PAGE_COUNT; p++) {
    if (cs_page_allowed_with(ch, perms, p)) {
      any_page = true;
      break;
    }
  }
  if (!any_page) {
    ui_channel_settings_close(ui);
    return;
  }
  if (!cs_page_allowed_with(ch, perms, cs->page)) {
    for (cs_page p = CS_OVERVIEW; p < CS_PAGE_COUNT; p++) {
      if (cs_page_allowed_with(ch, perms, p)) {
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
  cs->perm_cache_valid = false;
  cs->ow_targets_valid = false;
  cs->ow_add_valid = false;
  ui->mobile_channel_settings_detail = false;
  ui->left_drawer_open = false;
  ui->right_drawer_open = false;
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

  uint64_t perms = cs_cached_chan_perms(ui, cs);
  cs_page first = CS_OVERVIEW;
  for (cs_page p = CS_OVERVIEW; p < CS_PAGE_COUNT; p++) {
    if (cs_page_allowed_with(ch, perms, p)) {
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
  if (ui->d.store && cid != CCORD_ID_NONE) ccord_store_forget_channel_invites(ui->d.store, gid, cid);
  ui_channel_settings_destroy(ui);
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

// ---------------------------------------------------------------- full draw

static ccord_rect cs_mobile_back_rect(const ccord_ui *ui)
{
  if (!ui->mobile_layout || !ui->mobile_channel_settings_detail) return (ccord_rect){0};
  return (ccord_rect){ui->safe_insets.left + UIS_SP_3, ui->safe_insets.top + UIS_SP_3, 76.0f, UIS_ICON_BTN};
}

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

  bool show_side = !ui->mobile_layout || !ui->mobile_channel_settings_detail;
  bool show_body = !ui->mobile_layout || ui->mobile_channel_settings_detail;
  float side_w = ui->mobile_layout ? (show_side ? ww : 0.0f) : fmaxf(210.0f, floorf(ww * 0.25f));
  if (!ui->mobile_layout && side_w > 280.0f) side_w = 280.0f;

  if (show_side) {
    ccord_draw_rect(r, (ccord_rect){0, 0, side_w, wh}, COL(bg_secondary));
    cs_cx side_cx = {.ui = ui, .cs = cs, .r = r, .clip = {0, 0, side_w, wh}};
    float sx = ui->mobile_layout ? (ui->safe_insets.left + UIS_SP_4) : (side_w - 192.0f);
    if (!ui->mobile_layout && sx < 12.0f) sx = 12.0f;
    float sw = ui->mobile_layout ? fmaxf(ww - ui->safe_insets.left - ui->safe_insets.right - 2.0f * UIS_SP_4, 160.0f)
                                 : (side_w - sx - 12.0f);
    float sy = ui->mobile_layout ? (ui->safe_insets.top + UIS_SP_4) : 44.0f;

    char title[160];
    const char *prefix = ch->kind == CCORD_CHANNEL_VOICE ? "" : (ch->kind == CCORD_CHANNEL_CATEGORY ? "" : "#");
    (void)snprintf(title, sizeof title, "%s%s", prefix, ch->name ? ch->name : "channel");
    (void)cs_text(&side_cx, title, sx + 8.0f, sy, uis_tokens_of(ui).fs_control, true, COL(text_header), sw - 48.0f);
    sy += 26.0f;

    uint64_t side_perms = cs_cached_chan_perms(ui, cs);
    for (cs_page p = CS_OVERVIEW; p < CS_PAGE_COUNT; p++) {
      if (!cs_page_allowed_with(ch, side_perms, p)) continue;
      float row_h = ui->mobile_layout ? 42.0f : 32.0f;
      ccord_rect pr = {sx, sy, sw, row_h};
      char nm[CS_NAME_LEN];
      bool hov = cs_add_hit(&side_cx, pr, CA_PAGE, (uint64_t)p, nullptr, NAME(nm, "page:%s", k_cs_page_names[p]));
      bool sel = !ui->mobile_layout && cs->page == p;
      ui_row_state rs = ui_row_state_of(ui, "cspage", (uint64_t)p, hov, sel, COL(bg_secondary));
      uis_list_row_fill(ui, r, pr, (uis_state){.hover = hov, .selected = sel}, "cspage", (uint64_t)p);
      ccord_color tc = ui_row_state_text(&rs, ui->mobile_layout ? COL(text_normal) : COL(text_secondary),
                                         COL(text_interactive_hover), COL(text_interactive_active));
      (void)cs_text(&side_cx, k_cs_page_labels[p], pr.x + 10.0f,
                    pr.y + (pr.h - ui_line_h(ui, uis_tokens_of(ui).fs_body)) * 0.5f,
                    uis_tokens_of(ui).fs_body, sel, tc, sw - 20.0f);
      sy += row_h + 2.0f;
    }

    if (ccord_perm_has(side_perms, CCORD_PERM_MANAGE_CHANNELS)) {
      sy += 8.0f;
      uis_divider(ui, r, sx + 8.0f, sy, sw - 16.0f);
      sy += 10.0f;
      ccord_rect dr = {sx, sy, sw, ui->mobile_layout ? 42.0f : 32.0f};
      bool hov = cs_add_hit(&side_cx, dr, CA_DELETE_CHANNEL, 0, nullptr, "delete_channel");
      // A destructive menu row: red label, red fill on hover (the shared menu-row style).
      uis_menu_row(ui, r, dr, "Delete Channel", (uis_state){.hover = hov}, false, false, true, "csdelete", 0);
    }
  }

  // Content area + ESC close button
  float content_x = ui->mobile_layout ? (ui->safe_insets.left + UIS_SP_4) : (side_w + 40.0f);
  float content_w = ui->mobile_layout
                      ? fmaxf(ww - ui->safe_insets.left - ui->safe_insets.right - 2.0f * UIS_SP_4, 160.0f)
                      : fminf(660.0f, ww - content_x - 96.0f);
  if (!ui->mobile_layout && content_w < 260.0f) content_w = fmaxf(180.0f, ww - content_x - 24.0f);
  bool dirty = cs_unsaved(ui);
  if (show_body) {
    float view_top = ui->mobile_layout ? (ui->safe_insets.top + UIS_SP_3 + UIS_ICON_BTN + UIS_SP_3) : 44.0f;
    float bottom_in = ui->mobile_layout ? fmaxf(ui->safe_insets.bottom, ui->keyboard_inset) : 0.0f;
    float view_h = wh - bottom_in - view_top - (dirty ? 76.0f : 24.0f);
    if (view_h < 80.0f) view_h = 80.0f;
    cs->view_h = view_h;

    ccord_rect clip = ui->mobile_layout ? (ccord_rect){ui->safe_insets.left, view_top,
                                                       ww - ui->safe_insets.left - ui->safe_insets.right, view_h}
                                        : (ccord_rect){side_w, view_top, ww - side_w - 72.0f, view_h};
    ccord_push_clip(r, clip);
    cs_cx cx = {.ui = ui, .cs = cs, .r = r, .clip = clip};
    float y = view_top - cs->scroll;
    if (cs->page == CS_OVERVIEW) cs_draw_cs_overview(&cx, ch, content_x, content_w, &y);
    else if (cs->page == CS_PERMISSIONS) cs_draw_cs_permissions(&cx, content_x, content_w, &y);
    else if (cs->page == CS_INVITES) cs_draw_cs_invites(&cx, content_x, content_w, &y);
    cs->content_h = (y + cs->scroll) - view_top + 24.0f;
    float max_scroll = fmaxf(0.0f, cs->content_h - cs->view_h);
    if (cs->scroll > max_scroll) cs->scroll = max_scroll;
    ccord_pop_clip(r);
  }

  // Close (ESC) & optional mobile Back button
  cs_cx top_cx = {.ui = ui, .cs = cs, .r = r, .clip = {0, 0, ww, wh}};
  ccord_rect back_r = cs_mobile_back_rect(ui);
  if (back_r.w > 0.0f) {
    uis_button(ui, r, back_r, "< Back", UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){0}, "csback", 0);
  }
  ccord_rect close_r = ui->mobile_layout
                         ? (ccord_rect){ww - ui->safe_insets.right - UIS_ICON_BTN - UIS_SP_3,
                                        ui->safe_insets.top + UIS_SP_3, UIS_ICON_BTN, UIS_ICON_BTN}
                         : (ccord_rect){ww - 60.0f, 40.0f, UIS_ICON_BTN, UIS_ICON_BTN};
  bool close_hov = cs_add_hit(&top_cx, close_r, CA_CLOSE, 0, nullptr, "close");
  ccord_color close_ic = uis_icon_button(ui, r, close_r, (uis_state){.hover = close_hov}, "csclose", 0);
  ui_icon_close(r, close_r.x + UIS_ICON_BTN * 0.5f, close_r.y + UIS_ICON_BTN * 0.5f, 14.0f, close_ic);
  if (!ui->mobile_layout) {
    (void)cs_text(&top_cx, "ESC", close_r.x + 4.0f, close_r.y + UIS_ICON_BTN + 4.0f, uis_tokens_of(ui).fs_caption, true, COL(text_muted), 36.0f);
  }

  // Unsaved Changes bar: a floating strip (popover frame, token radius), red while it flashes
  if (dirty) {
    bool flash = cs->flash_until_ms > ui->now_ms;
    float bot_in = ui->mobile_layout ? fmaxf(ui->safe_insets.bottom, ui->keyboard_inset) : 0.0f;
    ccord_rect bar = {content_x, wh - bot_in - 62.0f, content_w, 48.0f};
    uis_frame(ui, r, bar, uis_tokens_of(ui).r_surface, flash ? COL(danger) : COL(bg_floating), UIS_ELEV_POPOVER);
    bool compact_bar = bar.w < 400.0f;
    float save_w = compact_bar ? 76.0f : 112.0f;
    float reset_w = compact_bar ? 64.0f : 72.0f;
    float btns_w = save_w + reset_w + 20.0f;
    (void)cs_text(&top_cx, compact_bar ? "Unsaved changes" : "Careful — you have unsaved changes!",
                  bar.x + 12.0f, bar.y + 14.0f, uis_tokens_of(ui).fs_body,
                  false, flash ? COL(text_on_brand) : COL(text_header), fmaxf(bar.w - btns_w - 16.0f, 60.0f));
    cs_button(&top_cx, (ccord_rect){bar.x + bar.w - btns_w, bar.y + 9.0f, reset_w, 30.0f}, "Reset", UIS_BTN_SECONDARY, nullptr, CA_RESET, 0, "reset");
    cs_button(&top_cx, (ccord_rect){bar.x + bar.w - save_w - 12.0f, bar.y + 9.0f, save_w, 30.0f},
              compact_bar ? "Save" : "Save Changes", UIS_BTN_SUCCESS, nullptr, CA_SAVE, 0, "save");
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
        if (ui->mobile_layout) ui->mobile_channel_settings_detail = true;
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
      if (ui->touch_mode) ui->touch_text_focused = true;
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
  ui_scroller_scroll_ui(ui, &cs->glide, &cs->scroll, dy, fmaxf(0.0f, cs->content_h - cs->view_h), ui_wheel_direct(ui, ev));
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
      ccord_rect br = cs_mobile_back_rect(ui);
      if (br.w > 0.0f && ui_in_rect(ev->x, ev->y, br)) {
        if (cs_unsaved(ui)) {
          cs->flash_until_ms = ui->now_ms + CS_FLASH_MS;
        } else {
          ui->mobile_channel_settings_detail = false;
          cs->focus = CSF_NONE;
        }
        ui->dirty = true;
        return true;
      }
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
      cs_wheel_by(ui, ev, ui_wheel_delta_px(ui, ev, 52.0f));
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
