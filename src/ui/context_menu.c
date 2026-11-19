// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdio.h>
#include <stdlib.h>

#include "ui_internal.h"
#include "ui_style.h"

#include <ccord/core/alloc.h>
#include <ccord/model/forum.h>
#include <ccord/store/admin.h>

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define SEP_H 9.0f
// Smallest panel we keep when the window is shorter than the items; the window itself may still be smaller.
#define MENU_MIN_H 96.0f
// Gap between a panel and the window edge (the window clamp is [MENU_EDGE, win_h - MENU_EDGE]).
#define MENU_EDGE 8.0f
// Mobile bottom-sheet handle & sub-menu header heights.
#define SHEET_HANDLE_H 20.0f
#define SHEET_BACK_H   44.0f

static float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

// The tallest a panel may be: the window minus its top and bottom margins, but never below MENU_MIN_H.
static float menu_max_h(const ccord_ui *ui)
{
  if (ui->mobile_layout) {
    float avail = ui->win_h - ui->safe_insets.top - ui->safe_insets.bottom - 2.0f * MENU_EDGE;
    float max_h = avail * 0.72f;
    return max_h > MENU_MIN_H ? max_h : MENU_MIN_H;
  }
  float max_h = ui->win_h - 2.0f * MENU_EDGE;
  return max_h > MENU_MIN_H ? max_h : MENU_MIN_H;
}

static void item_free(menu_item *it)
{
  ccord_free(it->label);
  ccord_free(it->sublabel);
  ccord_free(it->owner);
  ccord_free(it->item_id);
  *it = (menu_item){0};
}

void ui_menu_release(ccord_ui *ui)
{
  for (size_t i = 0; i < ui->menu.count; i++) item_free(&ui->menu.items[i]);
  ui->menu.count = 0;
  ui->menu.open = false;
  ui->menu.hover = -1;
  ui->menu.open_sub = -1;
  ui->mobile_menu_in_sub = false;
  ui->menu.content_h = 0.0f;
  ui->menu.scroll_y = 0.0f;
  ui->menu.sub_content_h = 0.0f;
  ui->menu.sub_scroll_y = 0.0f;
  ccord_scroller_cancel(&ui->menu_glide);
  ccord_scroller_cancel(&ui->menu_sub_glide);
  ui->menu.scrollbar_rect = (ccord_rect){0};
  ui->menu.sub_scrollbar_rect = (ccord_rect){0};
  ui->menu_folder_id = CCORD_ID_NONE;
  ui->menu_text_edit = nullptr;
  ui->menu_text_masked = false;
  ui->menu_text_is_composer = false;
  ui->menu_text_is_quickcss = false;
  ui->menu_text_is_search = false;
  ui->dirty = true;
}

// The panel stays on screen while it fades out: the items, position and fly-out are kept and ui_menu_draw frees them
// when the fade is over. Nothing treats a menu that is not `open` as live, so it takes no input meanwhile.
void ui_menu_close(ccord_ui *ui)
{
  if (!ui->menu.open) return;   // already closed (or fading out)
  if (ui->anim.reduced_motion) {
    ui_menu_release(ui);
    return;
  }
  ui->menu.open = false;
  ui->menu.hover = -1;
  ui->mobile_menu_in_sub = false;
  ui->menu_folder_id = CCORD_ID_NONE;
  ui->dirty = true;
}

static bool add_item_ex(ccord_ui *ui, const char *label, const char *sublabel, menu_builtin builtin, const char *owner,
                        const char *item_id, bool sep, bool disabled, bool danger, int64_t arg)
{
  if (ui->menu.count >= UI_MENU_MAX_ITEMS) return false;
  menu_item it = {
    .builtin = builtin,
    .sep_before = sep,
    .disabled = disabled,
    .danger = danger,
    .parent = -1,
    .arg = arg,
  };
  it.label = ccord_strdup(label);
  if (sublabel) it.sublabel = ccord_strdup(sublabel);
  if (owner) it.owner = ccord_strdup(owner);
  if (item_id) it.item_id = ccord_strdup(item_id);
  if (!it.label || (sublabel && !it.sublabel) || (owner && !it.owner) || (item_id && !it.item_id)) {
    item_free(&it);
    return false;
  }
  ui->menu.items[ui->menu.count++] = it;
  return true;
}

static bool add_item(ccord_ui *ui, const char *label, menu_builtin builtin, const char *owner, const char *item_id, bool sep)
{
  return add_item_ex(ui, label, nullptr, builtin, owner, item_id, sep, false, false, 0);
}

static bool add_toggle_item(ccord_ui *ui, const char *label, menu_builtin builtin, int64_t arg, bool checked, bool sep)
{
  if (!add_item(ui, label, builtin, nullptr, nullptr, sep)) return false;
  menu_item *it = &ui->menu.items[ui->menu.count - 1];
  it->arg = arg;
  it->checked = checked;
  return true;
}

// A parent item: pointing at it (or clicking it) opens a fly-out holding the children added after it. Returns its
// index, or -1 when the menu is full.
static int add_parent_ex(ccord_ui *ui, const char *label, bool sep, bool disabled)
{
  if (!add_item_ex(ui, label, nullptr, MENU_BUILTIN_NONE, nullptr, nullptr, sep, disabled, false, 0)) return -1;
  int idx = (int)ui->menu.count - 1;
  ui->menu.items[idx].has_sub = true;
  return idx;
}

static int add_parent(ccord_ui *ui, const char *label, bool sep)
{
  return add_parent_ex(ui, label, sep, false);
}

static bool add_child(ccord_ui *ui, int parent, const char *label, menu_builtin builtin, int64_t arg, bool checked, bool sep)
{
  if (parent < 0 || !add_item(ui, label, builtin, nullptr, nullptr, sep)) return false;
  menu_item *it = &ui->menu.items[ui->menu.count - 1];
  it->parent = (int8_t)parent;
  it->arg = arg;
  it->checked = checked;
  return true;
}

static size_t child_count(const ccord_ui *ui, int parent)
{
  size_t n = 0;
  for (size_t i = 0; i < ui->menu.count; i++) n += ui->menu.items[i].parent == parent;
  return n;
}

static float row_height(const ccord_ui *ui, const menu_item *it)
{
  float base = ui->mobile_layout ? fmaxf(M(context_menu_item_height), 44.0f) : M(context_menu_item_height);
  return base + (it && it->sublabel ? 12.0f : 0.0f);
}

static float menu_height(const ccord_ui *ui)
{
  float h = 2.0f * M(context_menu_pad) + (ui->mobile_layout ? SHEET_HANDLE_H : 0.0f);
  for (size_t i = 0; i < ui->menu.count; i++) {
    if (ui->menu.items[i].parent >= 0) continue;   // fly-out items have their own panel
    h += row_height(ui, &ui->menu.items[i]);
    if (ui->menu.items[i].sep_before) h += SEP_H;
  }
  return h;
}

static ccord_rect top_item_rect(const ccord_ui *ui, size_t idx)
{
  float top_pad = M(context_menu_pad) + (ui->mobile_layout ? SHEET_HANDLE_H : 0.0f);
  float y = ui->menu.y + top_pad - ui->menu.scroll_y;
  for (size_t i = 0; i < idx; i++) {
    if (ui->menu.items[i].parent >= 0) continue;
    y += row_height(ui, &ui->menu.items[i]) + (ui->menu.items[i].sep_before ? SEP_H : 0.0f);
  }
  if (ui->menu.items[idx].sep_before) y += SEP_H;
  return (ccord_rect){ui->menu.x + M(context_menu_pad), y, ui->menu.w - 2.0f * M(context_menu_pad),
                      row_height(ui, &ui->menu.items[idx])};
}

// Full height of the open fly-out's children (its panel scrolls when this exceeds the window).
static float sub_content_height(const ccord_ui *ui)
{
  int p = ui->menu.open_sub;
  if (p < 0 || (size_t)p >= ui->menu.count) return 0.0f;
  float h = 2.0f * M(context_menu_pad) + (ui->mobile_layout ? (SHEET_HANDLE_H + SHEET_BACK_H) : 0.0f);
  for (size_t i = 0; i < ui->menu.count; i++) {
    if (ui->menu.items[i].parent != p) continue;
    h += row_height(ui, &ui->menu.items[i]) + (ui->menu.items[i].sep_before ? SEP_H : 0.0f);
  }
  return h;
}

static ccord_rect mobile_sheet_back_rect(const ccord_ui *ui)
{
  if (!ui->mobile_layout || !ui->mobile_menu_in_sub || ui->menu.open_sub < 0) return (ccord_rect){0};
  return (ccord_rect){ui->menu.x + M(context_menu_pad), ui->menu.y + SHEET_HANDLE_H + M(context_menu_pad),
                      ui->menu.w - 2.0f * M(context_menu_pad), SHEET_BACK_H};
}

// The panel of the open fly-out, beside its parent; a zero rectangle when none is open. It opens to the right and
// flips to the left when the window has no room. Like the top-level panel, its visible height is capped to the
// window and its rows scroll inside it. On mobile, sub-menus drill down in-place inside the same bottom sheet.
static ccord_rect sub_box(const ccord_ui *ui)
{
  int p = ui->menu.open_sub;
  if (p < 0 || (size_t)p >= ui->menu.count || child_count(ui, p) == 0) return (ccord_rect){0};
  if (ui->mobile_layout) {
    if (!ui->mobile_menu_in_sub) return (ccord_rect){0};
    return (ccord_rect){ui->menu.x, ui->menu.y, ui->menu.w, ui->menu.h};
  }
  float h = sub_content_height(ui);
  float max_h = menu_max_h(ui);
  if (h > max_h) h = max_h;
  ccord_rect pr = top_item_rect(ui, (size_t)p);
  float w = ui->menu.sub_w;
  float x = ui->menu.x + ui->menu.w + 2.0f;
  if (x + w > ui->win_w - MENU_EDGE) x = ui->menu.x - w - 2.0f;
  if (x < MENU_EDGE) x = MENU_EDGE;
  float y = pr.y - M(context_menu_pad);
  if (y + h > ui->win_h - MENU_EDGE) y = ui->win_h - MENU_EDGE - h;
  if (y < MENU_EDGE) y = MENU_EDGE;
  return (ccord_rect){x, y, w, h};
}

static ccord_rect item_rect(const ccord_ui *ui, size_t idx)
{
  const menu_item *me = &ui->menu.items[idx];
  if (me->parent < 0) return top_item_rect(ui, idx);
  ccord_rect box = sub_box(ui);
  float top_pad = M(context_menu_pad) + (ui->mobile_layout ? (SHEET_HANDLE_H + SHEET_BACK_H) : 0.0f);
  float y = box.y + top_pad - ui->menu.sub_scroll_y;
  for (size_t i = 0; i < idx; i++) {
    if (ui->menu.items[i].parent != me->parent) continue;
    y += row_height(ui, &ui->menu.items[i]) + (ui->menu.items[i].sep_before ? SEP_H : 0.0f);
  }
  if (me->sep_before) y += SEP_H;
  return (ccord_rect){box.x + M(context_menu_pad), y, box.w - 2.0f * M(context_menu_pad), row_height(ui, me)};
}

// "Mute <noun>" with its durations, or "Unmute <noun>" while the row mutes; then "Notification Settings" (levels and,
// for a server, the @everyone / role switches). `channel == 0` is the server row. Returns true when anything was added.
static bool add_notify_items_ex(ccord_ui *ui, ccord_id guild, ccord_id channel, const char *noun, bool with_levels,
                                const char *inherit_label, bool sep_first)
{
  const ccord_store *st = ui->d.store;
  ccord_notify_scope row = {.guild_id = guild, .channel_id = channel, .level = CCORD_NOTIFY_INHERIT};
  bool have = ccord_store_notify_scope(st, guild, channel, &row);
  bool added = false;
  char label[128];
  if (have && ccord_notify_scope_muted(&row, ui->now_unix_ms)) {
    char left[48];
    if (ccord_notify_mute_remaining_text(&row, ui->now_unix_ms, left, sizeof left)) {
      (void)snprintf(label, sizeof label, "Unmute %s (%s)", noun, left);
    } else {
      (void)snprintf(label, sizeof label, "Unmute %s", noun);
    }
    added = add_item(ui, label, MENU_BUILTIN_UNMUTE, nullptr, nullptr, sep_first);
  } else {
    (void)snprintf(label, sizeof label, "Mute %s", noun);
    int p = add_parent(ui, label, sep_first);
    size_t n = 0;
    const ccord_mute_option *opts = ccord_notify_mute_options(&n);
    for (size_t i = 0; p >= 0 && i < n; i++) {
      (void)add_child(ui, p, opts[i].label, MENU_BUILTIN_MUTE, opts[i].duration_ms, false, false);
    }
    added = p >= 0;
  }
  if (with_levels) {
    int p = add_parent(ui, "Notification Settings", false);
    if (p >= 0) {
      added = true;
      ccord_notify_level cur = have ? row.level : CCORD_NOTIFY_INHERIT;
      if (channel != CCORD_ID_NONE) {
        (void)add_child(ui, p, inherit_label, MENU_BUILTIN_NOTIFY_LEVEL, CCORD_NOTIFY_INHERIT, cur == CCORD_NOTIFY_INHERIT, false);
      }
      for (ccord_notify_level lv = CCORD_NOTIFY_ALL; lv <= CCORD_NOTIFY_NOTHING; lv++) {
        (void)add_child(ui, p, ccord_notify_level_label(lv), MENU_BUILTIN_NOTIFY_LEVEL, lv, cur == lv, false);
      }
      if (channel == CCORD_ID_NONE) {   // the broad mentions are set for the whole server
        bool se = have && row.suppress_everyone, sr = have && row.suppress_roles;
        (void)add_child(ui, p, "Suppress @everyone and @here", MENU_BUILTIN_SUPPRESS_EVERYONE, se ? 0 : 1, se, true);
        (void)add_child(ui, p, "Suppress All Role @mentions", MENU_BUILTIN_SUPPRESS_ROLES, sr ? 0 : 1, sr, false);
      }
    }
  }
  return added;
}

static bool add_notify_items(ccord_ui *ui, ccord_id guild, ccord_id channel, const char *noun, bool with_levels,
                             const char *inherit_label)
{
  return add_notify_items_ex(ui, guild, channel, noun, with_levels, inherit_label, false);
}

// "Mark As Read" leads the menu of a server, a category, a channel or a conversation that has something unread
// (`channel` == 0 is the whole server).
static bool add_mark_read_item(ccord_ui *ui, ccord_id guild, ccord_id channel)
{
  if (ccord_store_read_targets(ui->d.store, guild, channel, ui->now_unix_ms, nullptr, 0) == 0) return false;
  return add_item(ui, "Mark As Read", MENU_BUILTIN_MARK_READ, nullptr, nullptr, false);
}

// The notification entries of a channel, a category or a conversation.
static bool add_channel_notify_items(ccord_ui *ui, const ccord_channel *ch)
{
  bool dm = ch->guild_id == CCORD_DM_GUILD_ID;
  const char *noun = dm ? "Conversation" : (ch->kind == CCORD_CHANNEL_CATEGORY ? "Category" : "Channel");
  const ccord_channel *parent = ch->parent_id != CCORD_ID_NONE ? ccord_store_channel(ui->d.store, ch->parent_id) : nullptr;
  const char *inherit = (parent && parent->kind == CCORD_CHANNEL_CATEGORY) ? "Use Category Default" : "Use Server Default";
  return add_notify_items(ui, ch->guild_id, ch->id, noun, !dm, inherit);
}

static bool message_owned_by_self(const ccord_ui *ui, const ccord_menu_context *ctx)
{
  const ccord_message *m = ccord_store_find_message(ui->d.store, ctx->channel_id, ctx->message_id);
  ccord_id self = ccord_store_self_id(ui->d.store);
  return m && self != CCORD_ID_NONE && m->author_id == self;
}

// Developer Mode: the built-in Copy ID entries are Discord's developer-only menu rows.
// Unset settings mean off, so a fresh account sees the normal menu.
static bool developer_mode_enabled(const ccord_ui *ui)
{
  const ccord_user_settings *settings =
    ui->d.store ? ccord_store_user_settings(ui->d.store) : nullptr;
  return settings && settings->developer_mode;
}

// Permission gate for USER (2) / MESSAGE (3) context-menu application commands. The slash picker's
// slash_cmd_allowed rejects these types; this is their own gate (same permission engine rules).
static bool menu_app_cmd_allowed(const ccord_ui *ui, const ccord_channel *ch, const ccord_slash_command *cmd)
{
  if (!ui || !ui->d.store || !ch || !cmd || cmd->name[0] == '\0') return false;
  if (cmd->command_type != 2u && cmd->command_type != 3u) return false;
  if (ch->guild_id == CCORD_DM_GUILD_ID) return cmd->dm_permission;
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, ch->guild_id, self)) return true;
  uint64_t ch_perms = ccord_store_permissions(ui->d.store, ch->guild_id, ch->id, self, ui->now_unix_ms);
  if (!ccord_perm_has(ch_perms, CCORD_PERM_USE_APPLICATION_COMMANDS)) return false;
  if (cmd->default_member_permissions != UINT64_MAX) {
    if (cmd->default_member_permissions == 0) {
      if ((ch_perms & CCORD_PERM_ADMINISTRATOR) == 0) return false;
    } else if (!ccord_perm_has(ch_perms, cmd->default_member_permissions)) {
      return false;
    }
  }
  return true;
}

// Appends the USER (2) / MESSAGE (3) application commands of one type as MENU_BUILTIN_APP_COMMAND rows
// (arg = command id). Returns true when anything was added; a separator leads the first row.
static bool add_app_commands(ccord_ui *ui, const ccord_channel *ch, uint8_t command_type, bool sep_first)
{
  if (!ui->d.store || !ch) return false;
  size_t count = 0;
  const ccord_slash_command *cmds = ccord_store_slash_commands(ui->d.store, ch->guild_id, &count);
  bool added = false;
  for (size_t i = 0; i < count; i++) {
    if (cmds[i].command_type != command_type) continue;
    if (!menu_app_cmd_allowed(ui, ch, &cmds[i])) continue;
    if (ui->menu.count >= UI_MENU_MAX_ITEMS) break;
    if (add_item(ui, cmds[i].name, MENU_BUILTIN_APP_COMMAND, nullptr, nullptr, sep_first && !added)) {
      ui->menu.items[ui->menu.count - 1].arg = (int64_t)cmds[i].id;
      added = true;
    }
  }
  return added;
}

static bool add_apps_submenu(ccord_ui *ui, const ccord_channel *ch, bool sep)
{
  int p = add_parent_ex(ui, "Apps", sep, false);
  if (p < 0) return false;
  size_t added = 0;
  if (ui->d.store && ch) {
    size_t count = 0;
    const ccord_slash_command *cmds = ccord_store_slash_commands(ui->d.store, ch->guild_id, &count);
    for (size_t i = 0; i < count; i++) {
      if (cmds[i].command_type != 2u || !menu_app_cmd_allowed(ui, ch, &cmds[i])) continue;
      if (add_child(ui, p, cmds[i].name, MENU_BUILTIN_APP_COMMAND, (int64_t)cmds[i].id, false, false)) {
        added++;
      }
    }
  }
  if (added == 0) ui->menu.items[p].disabled = true;
  return true;
}

static ccord_id guild_invite_channel(const ccord_ui *ui, ccord_id gid, ccord_id self_id)
{
  if (!ui || !ui->d.store || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) return CCORD_ID_NONE;
  const ccord_store *st = ui->d.store;
  const ccord_guild *gd = ccord_store_guild(st, gid);
  if (!gd) return CCORD_ID_NONE;
  uint64_t need = CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_CREATE_INSTANT_INVITE;
  if (gd->system_channel_id != CCORD_ID_NONE) {
    const ccord_channel *sc = ccord_store_channel(st, gd->system_channel_id);
    if (sc && sc->guild_id == gid && sc->kind == CCORD_CHANNEL_TEXT &&
        ccord_store_has_permission(st, gid, sc->id, self_id, need)) {
      return sc->id;
    }
  }
  size_t nc = ccord_store_channel_count(st, gid);
  for (size_t i = 0; i < nc; i++) {
    const ccord_channel *c = ccord_store_channel_at(st, gid, i);
    if (!c || c->kind != CCORD_CHANNEL_TEXT || ccord_discord_type_is_thread(c->discord_type)) continue;
    if (ccord_store_has_permission(st, gid, c->id, self_id, need)) return c->id;
  }
  return CCORD_ID_NONE;
}

static bool add_invite_to_server_submenu(ccord_ui *ui, ccord_id self_id, bool sep)
{
  int p = add_parent_ex(ui, "Invite to Server", sep, false);
  if (p < 0) return false;
  size_t added = 0;
  size_t ng = ui->d.store ? ccord_store_guild_count(ui->d.store) : 0;
  for (size_t i = 0; i < ng; i++) {
    const ccord_guild *gd = ccord_store_guild_at(ui->d.store, i);
    if (!gd || gd->id == CCORD_DM_GUILD_ID || !gd->name || !gd->name[0]) continue;
    if (guild_invite_channel(ui, gd->id, self_id) == CCORD_ID_NONE) continue;
    if (add_child(ui, p, gd->name, MENU_BUILTIN_INVITE_TO_GUILD, (int64_t)gd->id, false, false)) {
      added++;
    }
  }
  if (added == 0) ui->menu.items[p].disabled = true;
  return true;
}

static ccord_id find_1on1_dm_channel(const ccord_ui *ui, ccord_id user_id)
{
  if (!ui || !ui->d.store || user_id == CCORD_ID_NONE) return CCORD_ID_NONE;
  size_t n = ccord_store_channel_count(ui->d.store, CCORD_DM_GUILD_ID);
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(ui->d.store, CCORD_DM_GUILD_ID, i);
    if (!ch || ui_channel_is_group_dm(ch)) continue;
    if (ch->user_id == user_id || (ch->recipient_count == 1 && ch->recipient_ids[0] == user_id)) return ch->id;
  }
  return CCORD_ID_NONE;
}

static const char *call_action_label(const ccord_ui *ui, ccord_id dm_ch)
{
  if (ui && ui->d.store && dm_ch != CCORD_ID_NONE) {
    ccord_id self_id = ccord_store_self_id(ui->d.store);
    const ccord_voice_state *vs = self_id != CCORD_ID_NONE ? ccord_store_voice_user(ui->d.store, CCORD_DM_GUILD_ID, self_id)
                                                           : nullptr;
    if (vs && vs->channel_id == dm_ch) return "Leave Call";
    const ccord_call_state *ring = ui->d.session ? ccord_session_incoming_call(ui->d.session) : nullptr;
    if (ring && ring->channel_id == dm_ch) return "Join Call";
  }
  return "Start a Call";
}

static void add_dm_relationship_rows(ccord_ui *ui, ccord_id uid, ccord_id self_id, bool sep_first)
{
  if (uid == CCORD_ID_NONE || uid == self_id) return;
  ccord_relationship_kind rk = ccord_store_relationship(ui->d.store, uid);
  const char *friend_lbl = "Add Friend";
  menu_builtin friend_act = MENU_BUILTIN_ADD_FRIEND;
  if (rk == CCORD_RELATIONSHIP_FRIEND) {
    friend_lbl = "Remove Friend";
    friend_act = MENU_BUILTIN_REMOVE_FRIEND;
  } else if (rk == CCORD_RELATIONSHIP_PENDING_INCOMING) {
    friend_lbl = "Accept Friend Request";
    friend_act = MENU_BUILTIN_ADD_FRIEND;
  } else if (rk == CCORD_RELATIONSHIP_PENDING_OUTGOING) {
    friend_lbl = "Cancel Friend Request";
    friend_act = MENU_BUILTIN_REMOVE_FRIEND;
  }
  (void)add_item_ex(ui, friend_lbl, nullptr, friend_act, nullptr, nullptr, sep_first, false, false, (int64_t)uid);
  bool ignored = ui_user_is_ignored(ui, uid);
  (void)add_item_ex(ui, ignored ? "Unignore" : "Ignore", nullptr, MENU_BUILTIN_IGNORE_USER, nullptr, nullptr,
                    false, false, false, (int64_t)uid);
  bool blocked = rk == CCORD_RELATIONSHIP_BLOCKED;
  (void)add_item_ex(ui, blocked ? "Unblock" : "Block", nullptr, MENU_BUILTIN_BLOCK_USER, nullptr, nullptr,
                    false, false, !blocked, (int64_t)uid);
}

static bool can_mention_in_composer(const ccord_ui *ui)
{
  if (!ui || !ui->d.store || ui->discovery_open || ui_is_dm_friends_view(ui) || ui_is_dm_requests_view(ui) ||
      ui_forum_is_active(ui)) {
    return false;
  }
  const ccord_channel *sel = ui_selected_channel(ui);
  return ui_channel_has_chat(sel) && ui_composer_lock_reason(ui) == nullptr;
}

static bool member_holds_role(const ccord_id *ids, size_t n, ccord_id role_id)
{
  for (size_t i = 0; i < n; i++) {
    if (ids[i] == role_id) return true;
  }
  return false;
}

static bool add_member_roles_submenu(ccord_ui *ui, ccord_id gid, ccord_id self_id, ccord_id target_id, bool sep)
{
  if (!ui || !ui->d.store || gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID ||
      self_id == CCORD_ID_NONE || target_id == CCORD_ID_NONE) {
    return false;
  }
  const ccord_store *st = ui->d.store;
  const ccord_id *held = nullptr;
  size_t nheld = 0;
  if (!ccord_store_member_roles(st, gid, target_id, &held, &nheld)) return false;
  size_t nroles = 0;
  const ccord_role *roles = ccord_store_roles(st, gid, &nroles);
  size_t manageable = 0;
  for (size_t i = 0; i < nroles; i++) {
    const ccord_role *rl = &roles[i];
    if (!rl || rl->id == CCORD_ID_NONE || rl->id == gid || rl->managed || !rl->name || !rl->name[0]) continue;
    if (ccord_store_can_edit_role(st, gid, self_id, rl->id, true) == CCORD_PERM_ALLOWED) manageable++;
  }
  if (manageable == 0) return false;
  int p = add_parent(ui, "Roles", sep);
  if (p < 0) return false;
  for (size_t i = 0; i < nroles; i++) {
    if (ui->menu.count + 20u >= UI_MENU_MAX_ITEMS) break;   // leave room for moderation, report and devmode rows
    const ccord_role *rl = &roles[i];
    if (!rl || rl->id == CCORD_ID_NONE || rl->id == gid || rl->managed || !rl->name || !rl->name[0]) continue;
    if (ccord_store_can_edit_role(st, gid, self_id, rl->id, true) != CCORD_PERM_ALLOWED) continue;
    bool has_it = member_holds_role(held, nheld, rl->id);
    (void)add_child(ui, p, rl->name, MENU_BUILTIN_TOGGLE_MEMBER_ROLE, (int64_t)rl->id, has_it, false);
  }
  return true;
}

void ui_menu_open(ccord_ui *ui, float x, float y, const ccord_menu_context *ctx)
{
  ccord_id folder_id =
    (ctx->target == CCORD_MENU_GUILD && ctx->guild_id == CCORD_ID_NONE) ? ui->menu_folder_id : CCORD_ID_NONE;
  ui->menu_replay = ui->menu.open;   // a menu opened over an open one plays its entrance again
  ui_menu_release(ui);
  ui->menu_folder_id = folder_id;
  ui->menu.seq++;
  ui->menu.ctx = *ctx;
  // Context-menu application commands (USER / MESSAGE) come from the same per-guild index as slash
  // commands; make sure it is requested (cached) so the rows appear once the answer lands.
  if (ui->d.session &&
      (ctx->target == CCORD_MENU_MESSAGE || ctx->target == CCORD_MENU_MEMBER ||
       (ctx->target == CCORD_MENU_CHANNEL && ctx->guild_id == CCORD_DM_GUILD_ID))) {
    ccord_id ensure_ch = ctx->channel_id != CCORD_ID_NONE ? ctx->channel_id : ccord_store_selected_channel(ui->d.store);
    const ccord_channel *ech = ensure_ch != CCORD_ID_NONE ? ccord_store_channel(ui->d.store, ensure_ch) : nullptr;
    if (ech) (void)ccord_session_ensure_slash_commands(ui->d.session, ech->guild_id, ensure_ch, ui->now_ms);
  }
  bool builtin = false;
  ccord_id self_id = ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;
  ccord_id gid = ctx->guild_id != CCORD_ID_NONE
                   ? ctx->guild_id
                   : (ui->d.store ? ccord_store_selected_guild(ui->d.store) : CCORD_ID_NONE);
  if (ctx->target == CCORD_MENU_MESSAGE) {
    const ccord_message *m = ccord_store_find_message(ui->d.store, ctx->channel_id, ctx->message_id);
    if (m && ccord_message_is_ephemeral(m)) {
      builtin = add_item(ui, "Copy Text", MENU_BUILTIN_COPY_TEXT, nullptr, nullptr, false);
      if (builtin) builtin = add_item(ui, "Dismiss Message", MENU_BUILTIN_DISMISS_EPHEMERAL, nullptr, nullptr, true);
    } else {
      bool owned = message_owned_by_self(ui, ctx);
      bool can_manage_msg = (gid != CCORD_DM_GUILD_ID &&
                             ccord_store_has_permission(ui->d.store, gid, ctx->channel_id, self_id,
                                                        CCORD_PERM_MANAGE_MESSAGES));
      const ccord_channel *mch = ccord_store_channel(ui->d.store, ctx->channel_id);
      bool confirmed = m && m->send_state == CCORD_MSG_SEND_CONFIRMED;
      bool is_announce = mch && mch->discord_type == CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT;
      bool already_crossposted = m && (ccord_message_flags(m) & CCORD_MSGFLAG_CROSSPOSTED) != 0u;
      bool can_crosspost = false;
      if (is_announce && confirmed && !already_crossposted && gid != CCORD_DM_GUILD_ID) {
        uint64_t need = owned ? CCORD_PERM_SEND_MESSAGES : CCORD_PERM_MANAGE_MESSAGES;
        can_crosspost = ccord_store_has_permission(ui->d.store, gid, ctx->channel_id, self_id, need);
      }
      bool saved = ui_saved_messages_contains(ui, ctx->channel_id, ctx->message_id);
      builtin = add_item(ui, "Copy Text", MENU_BUILTIN_COPY_TEXT, nullptr, nullptr, false);
      // The hover bar's react button shows for confirmed messages only, so the row follows the same rule.
      if (builtin && confirmed) builtin = add_item(ui, "Add Reaction", MENU_BUILTIN_REACT, nullptr, nullptr, false);
      if (builtin && owned) builtin = add_item(ui, "Edit Message", MENU_BUILTIN_EDIT, nullptr, nullptr, false);
      if (builtin && (owned || can_manage_msg)) {
        builtin = add_item(ui, "Delete Message", MENU_BUILTIN_DELETE, nullptr, nullptr, false);
      }
      if (builtin) builtin = add_item(ui, "Reply", MENU_BUILTIN_REPLY, nullptr, nullptr, true);
      if (builtin && confirmed) builtin = add_item(ui, "Forward", MENU_BUILTIN_FORWARD, nullptr, nullptr, false);
      if (builtin) builtin = add_item(ui, "Copy Message Link", MENU_BUILTIN_COPY_LINK, nullptr, nullptr, false);
      if (builtin) {
        builtin = add_item(ui, (m && m->pinned) ? "Unpin Message" : "Pin Message", MENU_BUILTIN_PIN, nullptr, nullptr, false);
      }
      if (builtin && confirmed) builtin = add_item(ui, "Mark Unread", MENU_BUILTIN_MARK_UNREAD, nullptr, nullptr, true);
      if (builtin && confirmed) {
        builtin = add_item(ui, saved ? "Unsave Message" : "Save Message", MENU_BUILTIN_SAVE_MESSAGE, nullptr, nullptr, false);
        if (builtin) ui->menu.items[ui->menu.count - 1].arg = saved ? 0 : 1;
      }
      if (builtin && can_crosspost) {
        builtin = add_item(ui, "Publish Message", MENU_BUILTIN_CROSSPOST, nullptr, nullptr, false);
      }
      if (builtin && gid != CCORD_DM_GUILD_ID) {
        const ccord_channel *ch = mch;
        bool has_thread = m && m->rich && m->rich->thread.id != CCORD_ID_NONE;
        if (ch && ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_forum(ch->discord_type) &&
            !ccord_discord_type_is_thread(ch->discord_type) && !has_thread &&
            ccord_store_has_permission(ui->d.store, gid, ctx->channel_id, self_id,
                                       CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_CREATE_PUBLIC_THREADS)) {
          builtin = add_item(ui, "Create Thread", MENU_BUILTIN_CREATE_THREAD, nullptr, nullptr, false);
        }
      }
      if (builtin && m && m->reactions && m->reaction_kind_count > 0) {
        bool any_react = false;
        for (size_t ri = 0; ri < m->reaction_kind_count; ri++) {
          if (m->reactions[ri].emoji && m->reactions[ri].count > 0) { any_react = true; break; }
        }
        if (any_react) {
          builtin = add_item(ui, "View Reactions", MENU_BUILTIN_VIEW_REACTIONS, nullptr, nullptr, false);
        }
      }
      if (builtin && confirmed && mch) {
        if (add_app_commands(ui, mch, 3u, true)) builtin = true;   // MESSAGE context-menu commands
      }
      // Reporting someone else's confirmed message; never your own, and never a local echo.
      if (builtin && confirmed && m && m->author_id != self_id) {
        builtin = add_item(ui, "Report Message", MENU_BUILTIN_REPORT_MESSAGE, nullptr, nullptr, true);
      }
      if (builtin && developer_mode_enabled(ui)) {
        builtin = add_item(ui, "Copy Message ID", MENU_BUILTIN_COPY_ID, nullptr, nullptr, false);
      }
    }
  } else if (ctx->target == CCORD_MENU_CHANNEL) {
    const ccord_channel *nch = ctx->channel_id != CCORD_ID_NONE ? ccord_store_channel(ui->d.store, ctx->channel_id) : nullptr;
    if (nch && nch->guild_id == CCORD_DM_GUILD_ID) {
      bool group = ui_channel_is_group_dm(nch);
      bool has_unread = ccord_store_read_targets(ui->d.store, nch->guild_id, nch->id, ui->now_unix_ms, nullptr, 0) > 0;
      (void)add_item_ex(ui, "Mark As Read", nullptr, MENU_BUILTIN_MARK_READ, nullptr, nullptr, false, !has_unread, false, 0);
      bool pinned = ui_dm_is_pinned(ui, nch->id);
      bool fav = ui_dm_is_favorite(ui, nch->id);
      (void)add_item_ex(ui, pinned ? "Unpin" : "Pin", nullptr, MENU_BUILTIN_PIN_DM, nullptr, nullptr, true, false, false,
                        (int64_t)nch->id);
      (void)add_item_ex(ui, fav ? "Remove from Favorites" : "Add to Favorites", nullptr, MENU_BUILTIN_FAVORITE_DM,
                        nullptr, nullptr, false, false, false, (int64_t)nch->id);
      if (!group) {
        const ccord_user *dmu = ui_dm_recipient(ui, nch);
        ccord_id uid = ctx->user_id != CCORD_ID_NONE ? ctx->user_id : (dmu ? dmu->id : CCORD_ID_NONE);
        if (uid != CCORD_ID_NONE) ui->menu.ctx.user_id = uid;
        bool sep = true;
        if (uid != CCORD_ID_NONE) {
          (void)add_item_ex(ui, "Profile", nullptr, MENU_BUILTIN_VIEW_PROFILE, nullptr, nullptr, sep, false, false, 0);
          sep = false;
        }
        (void)add_item_ex(ui, call_action_label(ui, nch->id), nullptr, MENU_BUILTIN_START_CALL, nullptr, nullptr,
                          sep, false, false, (int64_t)nch->id);
        if (uid != CCORD_ID_NONE) {
          const char *note = ccord_store_user_note(ui->d.store, uid);
          const char *note_lbl = (note && note[0] != '\0') ? "Edit Note" : "Add Note";
          (void)add_item_ex(ui, note_lbl, "Only visible to you", MENU_BUILTIN_USER_NOTE, nullptr, nullptr,
                            false, false, false, (int64_t)uid);
          if (ccord_store_relationship(ui->d.store, uid) == CCORD_RELATIONSHIP_FRIEND) {
            const char *fnick = ui_friend_nickname(ui, uid);
            (void)add_item_ex(ui, fnick ? "Edit Friend Nickname" : "Add Friend Nickname", nullptr,
                              MENU_BUILTIN_FRIEND_NICK, nullptr, nullptr, false, false, false, (int64_t)uid);
          }
        }
        (void)add_item_ex(ui, "Close DM", nullptr, MENU_BUILTIN_CLOSE_DM, nullptr, nullptr, false, false, false,
                          (int64_t)nch->id);
        (void)add_apps_submenu(ui, nch, true);
        if (uid != CCORD_ID_NONE && uid != self_id) {
          (void)add_invite_to_server_submenu(ui, self_id, false);
          add_dm_relationship_rows(ui, uid, self_id, false);
        }
        char mute_noun[104];
        const char *dname = ui_dm_display_name(ui, nch);
        if (dname && dname[0] && strcmp(dname, "Direct Message") != 0) {
          snprintf(mute_noun, sizeof mute_noun, "@%s", dname);
        } else {
          snprintf(mute_noun, sizeof mute_noun, "Conversation");
        }
        (void)add_notify_items_ex(ui, nch->guild_id, nch->id, mute_noun, false, "", true);
        if (developer_mode_enabled(ui)) {
          bool dev_sep = true;
          if (uid != CCORD_ID_NONE) {
            (void)add_item_ex(ui, "Copy User ID", nullptr, MENU_BUILTIN_COPY_USER_ID, nullptr, nullptr,
                              dev_sep, false, false, (int64_t)uid);
            dev_sep = false;
          }
          (void)add_item_ex(ui, "Copy Channel ID", nullptr, MENU_BUILTIN_COPY_CHANNEL_ID, nullptr, nullptr,
                            dev_sep, false, false, (int64_t)nch->id);
        }
      } else {
        (void)add_item_ex(ui, call_action_label(ui, nch->id), nullptr, MENU_BUILTIN_START_CALL, nullptr, nullptr,
                          true, false, false, (int64_t)nch->id);
        (void)add_item(ui, "Add Friends to DM", MENU_BUILTIN_ADD_DM_RECIPIENT, nullptr, nullptr, false);
        (void)add_item(ui, "Edit Group DM", MENU_BUILTIN_EDIT_GROUP_DM, nullptr, nullptr, false);
        (void)add_item_ex(ui, "Leave Group", nullptr, MENU_BUILTIN_CLOSE_DM, nullptr, nullptr,
                          false, false, true, (int64_t)nch->id);
        (void)add_notify_items_ex(ui, nch->guild_id, nch->id, "Conversation", false, "", true);
        if (developer_mode_enabled(ui)) {
          (void)add_item_ex(ui, "Copy Channel ID", nullptr, MENU_BUILTIN_COPY_CHANNEL_ID, nullptr, nullptr,
                            true, false, false, (int64_t)nch->id);
        }
      }
      builtin = true;
    } else {
      bool mark = nch && add_mark_read_item(ui, nch->guild_id, nch->id);
      size_t first_notify = ui->menu.count;
      bool lead = nch && add_channel_notify_items(ui, nch);   // mark as read, mute and notification choices come first, as in Discord
      if (mark && lead) ui->menu.items[first_notify].sep_before = true;
      lead = lead || mark;
      builtin = lead;
      if (gid != CCORD_DM_GUILD_ID) {
        // Each item asks for what Discord asks for: managing the channel (its overwrites count), creating one anywhere in
        // the server, and starting a thread in a text channel.
        const ccord_store *st = ui->d.store;
        const ccord_channel *ch = ccord_store_channel(st, ctx->channel_id);
        bool manage = ccord_store_has_permission(st, gid, ctx->channel_id, self_id, CCORD_PERM_MANAGE_CHANNELS);
        bool can_settings = ui_channel_settings_available(ui, gid, ctx->channel_id);
        bool create = ccord_store_has_permission(st, gid, CCORD_ID_NONE, self_id, CCORD_PERM_MANAGE_CHANNELS);
        bool can_clone = create && ch && ch->kind != CCORD_CHANNEL_CATEGORY &&
                         !ccord_discord_type_is_thread(ch->discord_type);
        if (can_clone && ccord_store_permissions_exact(st, gid, self_id)) {
          uint64_t gperms = ccord_store_permissions(st, gid, CCORD_ID_NONE, self_id, 0);
          size_t nows = 0;
          const ccord_overwrite *ows = ccord_store_channel_overwrites(st, ch->id, &nows);
          for (size_t oi = 0; ows && oi < nows; oi++) {
            if (((ows[oi].allow | ows[oi].deny) & ~gperms) != 0u) {
              can_clone = false;
              break;
            }
          }
        }
        bool thread = ch && ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_forum(ch->discord_type) &&
                      !ccord_discord_type_is_thread(ch->discord_type) &&
                      ccord_store_has_permission(st, gid, ctx->channel_id, self_id,
                                                 CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_CREATE_PUBLIC_THREADS);
        bool sep = lead;
        if (can_settings && add_item(ui, "Edit Channel", MENU_BUILTIN_EDIT_CHANNEL, nullptr, nullptr, sep)) { builtin = true; sep = false; }
        if (can_clone && add_item(ui, "Clone Channel", MENU_BUILTIN_CLONE_CHANNEL, nullptr, nullptr, sep)) { builtin = true; sep = false; }
        if (create && add_item(ui, "Create Channel", MENU_BUILTIN_CREATE_CHANNEL, nullptr, nullptr, sep)) { builtin = true; sep = false; }
        if (thread && add_item(ui, "Create Thread", MENU_BUILTIN_CREATE_THREAD, nullptr, nullptr, sep)) { builtin = true; sep = false; }
        if (manage && add_item(ui, "Delete Channel", MENU_BUILTIN_DELETE_CHANNEL, nullptr, nullptr, true)) builtin = true;
      }
      if (developer_mode_enabled(ui)) {
        if (add_item(ui, "Copy Channel ID", MENU_BUILTIN_COPY_ID, nullptr, nullptr, builtin)) builtin = true;
      }
    }
  } else if (ctx->target == CCORD_MENU_GUILD && ui->menu_folder_id != CCORD_ID_NONE) {
    bool any_unread = false;
    size_t ng = ccord_store_guild_count(ui->d.store);
    for (size_t gi = 0; gi < ng; gi++) {
      const ccord_guild *gd = ccord_store_guild_at(ui->d.store, gi);
      if (gd && gd->folder_id == ui->menu_folder_id &&
          ccord_store_read_targets(ui->d.store, gd->id, CCORD_ID_NONE, ui->now_unix_ms, nullptr, 0) > 0) {
        any_unread = true;
        break;
      }
    }
    bool mark = any_unread && add_item(ui, "Mark Folder As Read", MENU_BUILTIN_FOLDER_MARK_READ, nullptr, nullptr, false);
    builtin = add_item(ui, "Folder Settings", MENU_BUILTIN_FOLDER_SETTINGS, nullptr, nullptr, mark) || mark;
    if (add_item(ui, "Ungroup Folder", MENU_BUILTIN_FOLDER_UNGROUP, nullptr, nullptr, true)) builtin = true;
  } else if (ctx->target == CCORD_MENU_GUILD) {
    bool real = gid != CCORD_DM_GUILD_ID && gid != CCORD_ID_NONE;
    bool mark = real && add_mark_read_item(ui, gid, CCORD_ID_NONE);
    size_t first_notify = ui->menu.count;
    bool lead = real && add_notify_items(ui, gid, CCORD_ID_NONE, "Server", true, "");
    if (mark && lead) ui->menu.items[first_notify].sep_before = true;
    lead = lead || mark;
    builtin = add_item(ui, "Server Boost (Browser)", MENU_BUILTIN_SERVER_BOOST, nullptr, nullptr, lead);
    if (builtin) builtin = add_item(ui, "Discord Nitro (Browser)", MENU_BUILTIN_OPEN_NITRO, nullptr, nullptr, false);
    if (builtin) builtin = add_item(ui, "Discord Shop (Browser)", MENU_BUILTIN_OPEN_SHOP, nullptr, nullptr, false);
    if (builtin) builtin = add_item(ui, "Billing Settings (Browser)", MENU_BUILTIN_OPEN_BILLING, nullptr, nullptr, true);
    if (gid != CCORD_DM_GUILD_ID) {
      if (ui_server_settings_available(ui, gid)) {   // any of its pages: Overview, Roles, Safety, Invites, Bans, Audit Log, Danger Zone
        builtin = add_item(ui, "Server Settings", MENU_BUILTIN_SERVER_SETTINGS, nullptr, nullptr, true);
      }
      // Scheduled events are visible to every member; creating/editing is gated inside the modal.
      builtin = add_item(ui, "Events", MENU_BUILTIN_GUILD_EVENTS, nullptr, nullptr, true) || builtin;
      if (ccord_store_has_permission(ui->d.store, gid, CCORD_ID_NONE, self_id, CCORD_PERM_MANAGE_CHANNELS)) {
        builtin = add_item(ui, "Create Channel", MENU_BUILTIN_CREATE_CHANNEL, nullptr, nullptr, false);
      }
      const ccord_guild *gd = ccord_store_guild(ui->d.store, gid);
      if (gd && (gd->owner_id == CCORD_ID_NONE || gd->owner_id != self_id)) {
        if (add_item(ui, "Leave Server", MENU_BUILTIN_LEAVE_GUILD, nullptr, nullptr, true)) builtin = true;
      }
    }
  } else if (ctx->target == CCORD_MENU_MEMBER && ctx->guild_id == CCORD_DM_GUILD_ID && ctx->user_id != CCORD_ID_NONE) {
    ccord_id uid = ctx->user_id;
    ccord_id dm_ch = find_1on1_dm_channel(ui, uid);
    ccord_id call_ch = ctx->channel_id != CCORD_ID_NONE ? ctx->channel_id : dm_ch;
    builtin = add_item(ui, "Profile", MENU_BUILTIN_VIEW_PROFILE, nullptr, nullptr, false);
    if (builtin && can_mention_in_composer(ui)) {
      builtin = add_item(ui, "Mention", MENU_BUILTIN_MENTION_USER, nullptr, nullptr, false);
    }
    if (builtin && uid != self_id) builtin = add_item(ui, "Message", MENU_BUILTIN_OPEN_DM, nullptr, nullptr, false);
    if (builtin && call_ch != CCORD_ID_NONE && uid != self_id) {
      (void)add_item_ex(ui, call_action_label(ui, call_ch), nullptr, MENU_BUILTIN_START_CALL, nullptr, nullptr,
                        false, false, false, (int64_t)call_ch);
    }
    if (ctx->channel_id != CCORD_ID_NONE && uid != self_id) {
      const ccord_voice_state *vs = ccord_store_voice_user(ui->d.store, CCORD_DM_GUILD_ID, uid);
      if (vs && vs->channel_id == ctx->channel_id) {
        bool lm = ccord_store_user_local_mute(ui->d.store, uid);
        (void)add_toggle_item(ui, "Mute", MENU_BUILTIN_LOCAL_MUTE, lm ? 0 : 1, lm, false);
        if (vs->self_stream) {
          const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
          bool watching = ws && ws->active && ws->user_id == uid;
          (void)add_item_ex(ui, watching ? "Stop Watching" : "Watch Stream", nullptr, MENU_BUILTIN_WATCH_STREAM,
                            nullptr, nullptr, false, false, false, (int64_t)ctx->channel_id);
        }
      }
    }
    const char *note = ccord_store_user_note(ui->d.store, uid);
    (void)add_item_ex(ui, (note && note[0] != '\0') ? "Edit Note" : "Add Note", "Only visible to you",
                      MENU_BUILTIN_USER_NOTE, nullptr, nullptr, false, false, false, (int64_t)uid);
    if (ccord_store_relationship(ui->d.store, uid) == CCORD_RELATIONSHIP_FRIEND) {
      const char *fnick = ui_friend_nickname(ui, uid);
      (void)add_item_ex(ui, fnick ? "Edit Friend Nickname" : "Add Friend Nickname", nullptr,
                        MENU_BUILTIN_FRIEND_NICK, nullptr, nullptr, false, false, false, (int64_t)uid);
    }
    if (ctx->channel_id != CCORD_ID_NONE) {
      const ccord_channel *ach = ccord_store_channel(ui->d.store, ctx->channel_id);
      if (ach && add_apps_submenu(ui, ach, true)) builtin = true;
    }
    if (uid != self_id) {
      (void)add_invite_to_server_submenu(ui, self_id, ctx->channel_id == CCORD_ID_NONE);
      add_dm_relationship_rows(ui, uid, self_id, false);
      (void)add_item(ui, "Report User", MENU_BUILTIN_REPORT_USER, nullptr, nullptr, false);
    }
    if (ctx->channel_id != CCORD_ID_NONE) {
      const ccord_channel *gch = ccord_store_channel(ui->d.store, ctx->channel_id);
      if (gch && ui_channel_is_group_dm(gch) && gch->owner_id != CCORD_ID_NONE &&
          gch->owner_id == self_id && uid != self_id) {
        (void)add_item_ex(ui, "Remove from Group", nullptr, MENU_BUILTIN_REMOVE_DM_RECIPIENT, nullptr, nullptr,
                          true, false, true, 0);
      }
    }
    if (developer_mode_enabled(ui)) {
      add_item(ui, "Copy User ID", MENU_BUILTIN_COPY_ID, nullptr, nullptr, true);
    }
  } else if (ctx->target == CCORD_MENU_MEMBER && gid != CCORD_DM_GUILD_ID && ctx->user_id != CCORD_ID_NONE) {
    // Profile & social actions come first on every user; server admin and moderation tools follow below, gated by the
    // permission engine (permission bit, role hierarchy, and rules about yourself and the server owner).
    const ccord_store *st = ui->d.store;
    ccord_id uid = ctx->user_id;
    bool can_nick = ccord_store_can_act_on(st, CCORD_PERM_ACT_SET_NICKNAME, gid, self_id, uid) == CCORD_PERM_ALLOWED;
    bool can_timeout = ccord_store_can_act_on(st, CCORD_PERM_ACT_TIMEOUT, gid, self_id, uid) == CCORD_PERM_ALLOWED;
    bool can_kick = ccord_store_can_act_on(st, CCORD_PERM_ACT_KICK, gid, self_id, uid) == CCORD_PERM_ALLOWED;
    bool can_ban = ccord_store_can_act_on(st, CCORD_PERM_ACT_BAN, gid, self_id, uid) == CCORD_PERM_ALLOWED;
    bool can_modview = uid != self_id && ui_server_settings_modview_available(ui, gid) &&
                       ccord_store_find_member(st, gid, uid) != nullptr;
    const ccord_channel *vch = ctx->channel_id != CCORD_ID_NONE ? ccord_store_channel(st, ctx->channel_id) : nullptr;
    const ccord_voice_state *vs = ccord_store_voice_user(st, gid, uid);
    bool is_voice_channel = vch && (vch->kind == CCORD_CHANNEL_VOICE || vch->kind == CCORD_CHANNEL_STAGE);
    bool voice_ctx = is_voice_channel && vs && vs->channel_id != CCORD_ID_NONE;
    bool can_vmute = voice_ctx && ccord_store_has_permission(st, gid, vs->channel_id, self_id, CCORD_PERM_MUTE_MEMBERS);
    bool can_vdeaf = voice_ctx && ccord_store_has_permission(st, gid, vs->channel_id, self_id, CCORD_PERM_DEAFEN_MEMBERS);
    bool can_vmove = voice_ctx && ccord_store_has_permission(st, gid, vs->channel_id, self_id, CCORD_PERM_MOVE_MEMBERS);

    if (add_item(ui, "Profile", MENU_BUILTIN_VIEW_PROFILE, nullptr, nullptr, false)) builtin = true;
    if (can_mention_in_composer(ui)) {
      if (add_item(ui, "Mention", MENU_BUILTIN_MENTION_USER, nullptr, nullptr, false)) builtin = true;
    }
    if (uid != self_id) {
      if (add_item(ui, "Message (Open DM)", MENU_BUILTIN_OPEN_DM, nullptr, nullptr, false)) builtin = true;
    }
    if (is_voice_channel && uid != self_id) {
      bool lm = ccord_store_user_local_mute(st, uid);
      if (add_toggle_item(ui, "Mute", MENU_BUILTIN_LOCAL_MUTE, lm ? 0 : 1, lm, false)) builtin = true;
      if (vs && vs->self_stream) {
        const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
        bool watching = ws && ws->active && ws->user_id == uid;
        if (add_item_ex(ui, watching ? "Stop Watching" : "Watch Stream", nullptr, MENU_BUILTIN_WATCH_STREAM,
                        nullptr, nullptr, false, false, false, (int64_t)ctx->channel_id)) {
          builtin = true;
        }
      }
    }
    const char *note = ccord_store_user_note(st, uid);
    if (add_item_ex(ui, (note && note[0] != '\0') ? "Edit Note" : "Add Note", "Only visible to you",
                    MENU_BUILTIN_USER_NOTE, nullptr, nullptr, true, false, false, (int64_t)uid)) {
      builtin = true;
    }
    if (uid != self_id && ccord_store_relationship(st, uid) == CCORD_RELATIONSHIP_FRIEND) {
      const char *fnick = ui_friend_nickname(ui, uid);
      (void)add_item_ex(ui, fnick ? "Edit Friend Nickname" : "Add Friend Nickname", nullptr,
                        MENU_BUILTIN_FRIEND_NICK, nullptr, nullptr, false, false, false, (int64_t)uid);
    }
    if (uid != self_id) {
      add_dm_relationship_rows(ui, uid, self_id, true);
    }

    // Server Administration & Moderation section (permission-gated)
    bool admin_sep = ui->menu.count > 0;
    if (can_modview && add_item(ui, "Open in Mod View", MENU_BUILTIN_MOD_VIEW, nullptr, nullptr, admin_sep)) {
      builtin = true;
      admin_sep = false;
    }
    if (add_member_roles_submenu(ui, gid, self_id, uid, admin_sep)) {
      builtin = true;
      admin_sep = false;
    }
    if (can_nick && add_item(ui, "Change Nickname", MENU_BUILTIN_CHANGE_NICK, nullptr, nullptr, admin_sep)) {
      builtin = true;
      admin_sep = false;
    }
    bool punish_sep = ui->menu.count > 0;
    if (can_timeout) {
      const ccord_member *tm = ccord_store_find_member(st, gid, uid);
      bool timed_out = tm && tm->timeout_until_ms > ui->now_unix_ms;
      if (timed_out) {
        if (add_item(ui, "Remove Timeout", MENU_BUILTIN_REMOVE_TIMEOUT, nullptr, nullptr, punish_sep)) {
          builtin = true;
          punish_sep = false;
        }
      } else if (add_item_ex(ui, "Timeout Member", nullptr, MENU_BUILTIN_TIMEOUT_MEMBER, nullptr, nullptr,
                             punish_sep, false, true, 0)) {
        builtin = true;
        punish_sep = false;
      }
    }
    if (can_kick && add_item_ex(ui, "Kick Member", nullptr, MENU_BUILTIN_KICK_MEMBER, nullptr, nullptr,
                                punish_sep, false, true, 0)) {
      builtin = true;
      punish_sep = false;
    }
    if (can_ban && add_item_ex(ui, "Ban Member", nullptr, MENU_BUILTIN_BAN_MEMBER, nullptr, nullptr,
                               punish_sep, false, true, 0)) {
      builtin = true;
    }
    if (can_vmute || can_vdeaf || can_vmove) {
      const ccord_member *vm = ccord_store_find_member(st, gid, uid);
      bool srv_muted = (vs && vs->mute) || (vm && vm->mute);
      bool srv_deaf = (vs && vs->deaf) || (vm && vm->deaf);
      bool vsep = ui->menu.count > 0;
      if (can_vmute && add_toggle_item(ui, "Server Mute", MENU_BUILTIN_VOICE_MUTE, srv_muted ? 0 : 1, srv_muted, vsep)) {
        builtin = true;
        vsep = false;
      }
      if (can_vdeaf && add_toggle_item(ui, "Server Deafen", MENU_BUILTIN_VOICE_DEAF, srv_deaf ? 0 : 1, srv_deaf, vsep)) {
        builtin = true;
        vsep = false;
      }
      if (can_vmove) {
        size_t nch = ccord_store_channel_count(st, gid);
        size_t ndest = 0;
        for (size_t ci = 0; ci < nch; ci++) {
          const ccord_channel *dc = ccord_store_channel_at(st, gid, ci);
          if (!dc || (dc->kind != CCORD_CHANNEL_VOICE && dc->kind != CCORD_CHANNEL_STAGE) ||
              dc->id == vs->channel_id) {
            continue;
          }
          if (ccord_store_has_permission(st, gid, dc->id, self_id, CCORD_PERM_CONNECT)) ndest++;
        }
        if (ndest > 0) {
          int mp = add_parent(ui, "Move to", vsep);
          if (mp >= 0) {
            builtin = true;
            vsep = false;
            for (size_t ci = 0; ci < nch; ci++) {
              const ccord_channel *dc = ccord_store_channel_at(st, gid, ci);
              if (!dc || (dc->kind != CCORD_CHANNEL_VOICE && dc->kind != CCORD_CHANNEL_STAGE) ||
                  dc->id == vs->channel_id) {
                continue;
              }
              if (!ccord_store_has_permission(st, gid, dc->id, self_id, CCORD_PERM_CONNECT)) continue;
              (void)add_child(ui, mp, dc->name ? dc->name : "Voice", MENU_BUILTIN_VOICE_MOVE, (int64_t)dc->id, false, false);
            }
          }
        }
        if (add_item(ui, "Disconnect", MENU_BUILTIN_VOICE_DISCONNECT, nullptr, nullptr, vsep)) builtin = true;
      }
    }
    if (uid != self_id) {
      if (add_item(ui, "Report User", MENU_BUILTIN_REPORT_USER, nullptr, nullptr, ui->menu.count > 0)) builtin = true;
    }
    // USER context-menu application commands are not moderation: they appear even without any moderation
    // permission, gated only by menu_app_cmd_allowed (USE_APPLICATION_COMMANDS + command permissions).
    const ccord_channel *ach = ctx->channel_id != CCORD_ID_NONE ? ccord_store_channel(ui->d.store, ctx->channel_id) : nullptr;
    if (!ach || ach->guild_id != gid) {
      ach = nullptr;
      size_t nc = ccord_store_channel_count(ui->d.store, gid);
      for (size_t ci = 0; ci < nc; ci++) {
        const ccord_channel *c = ccord_store_channel_at(ui->d.store, gid, ci);
        if (c && c->kind == CCORD_CHANNEL_TEXT) { ach = c; break; }
      }
    }
    if (ach && add_app_commands(ui, ach, 2u, true)) builtin = true;
    if (developer_mode_enabled(ui)) {
      if (add_item(ui, "Copy User ID", MENU_BUILTIN_COPY_ID, nullptr, nullptr, ui->menu.count > 0)) builtin = true;
    }
  }
  if (ui->d.contrib && ui->menu_folder_id == CCORD_ID_NONE) {
    size_t n = ccord_contrib_menu_count(ui->d.contrib, ctx->target);
    for (size_t i = 0; i < n; i++) {
      const ccord_menu_contribution *c = ccord_contrib_menu_at(ui->d.contrib, ctx->target, i);
      if (!c) continue;
      if (!add_item(ui, c->label, MENU_BUILTIN_NONE, c->owner, c->item_id, builtin && i == 0)) break;
    }
  }
  if (ui->menu.count == 0) return;   // nothing to show: the menu does not open
  ui->menu.open = true;
  ui->menu.hover = -1;
  ui->menu.open_sub = -1;
  ui->menu.sub_w = M(context_menu_min_width);
  ui->menu.w = M(context_menu_min_width);
  ui->menu.content_h = menu_height(ui);
  // Cap the panel to the window; rows beyond the visible height scroll instead of being lost.
  ui->menu.h = ui->menu.content_h;
  float max_h = menu_max_h(ui);
  if (ui->menu.h > max_h) ui->menu.h = max_h;
  ui->menu.scroll_y = 0.0f;
  ui->menu.sub_scroll_y = 0.0f;
  ccord_scroller_cancel(&ui->menu_glide);
  ccord_scroller_cancel(&ui->menu_sub_glide);
  // clamp inside the window using the minimum width; the real width is applied on first draw. A panel that does
  // not fit below the pointer opens upward instead (its bottom edge stays on screen, then the top is clamped).
  ui->menu.x = x;
  ui->menu.y = y;
  if (ui->menu.x + ui->menu.w > ui->win_w - MENU_EDGE) ui->menu.x = ui->win_w - ui->menu.w - MENU_EDGE;
  if (ui->menu.y + ui->menu.h > ui->win_h - MENU_EDGE) ui->menu.y = ui->win_h - MENU_EDGE - ui->menu.h;
  if (ui->menu.x < MENU_EDGE) ui->menu.x = MENU_EDGE;
  if (ui->menu.y < MENU_EDGE) ui->menu.y = MENU_EDGE;
  ui->dirty = true;
}

void ui_menu_open_text_field(ccord_ui *ui, float x, float y, ccord_text_edit *edit, bool masked,
                             bool is_composer, bool is_quickcss, bool is_search)
{
  if (!ui || !edit) return;
  ui_menu_release(ui);
  ui->menu.seq++;
  ui->menu.ctx = (ccord_menu_context){0};
  ui->menu_text_edit = edit;
  ui->menu_text_masked = masked;
  ui->menu_text_is_composer = is_composer;
  ui->menu_text_is_quickcss = is_quickcss;
  ui->menu_text_is_search = is_search;

  bool has_text = edit->len > 0;
  // Never probe clipboard_get here: on Android 12+, reading the clipboard shows a system toast,
  // so Paste stays enabled whenever the host provides clipboard_get and only reads on tap.
  bool can_paste = ui->d.host.clipboard_get != nullptr || (is_composer && ui->d.host.get_clipboard_image != nullptr);
  bool can_copy = !masked && has_text && ui->d.host.clipboard_set != nullptr;
  (void)add_item_ex(ui, "Paste", nullptr, MENU_BUILTIN_TEXT_PASTE, nullptr, nullptr, false, !can_paste, false, 0);
  if (!masked) {
    (void)add_item_ex(ui, "Copy", nullptr, MENU_BUILTIN_TEXT_COPY, nullptr, nullptr, false, !can_copy, false, 0);
    (void)add_item_ex(ui, "Cut", nullptr, MENU_BUILTIN_TEXT_CUT, nullptr, nullptr, false, !can_copy, false, 0);
  }
  (void)add_item_ex(ui, "Select All", nullptr, MENU_BUILTIN_TEXT_SELECT_ALL, nullptr, nullptr, false, !has_text, false, 0);
  (void)add_item_ex(ui, "Clear", nullptr, MENU_BUILTIN_TEXT_CLEAR, nullptr, nullptr, true, !has_text, true, 0);

  if (ui->menu.count == 0) return;
  ui->menu.open = true;
  ui->menu.hover = -1;
  ui->menu.open_sub = -1;
  ui->menu.sub_w = M(context_menu_min_width);
  ui->menu.w = M(context_menu_min_width);
  ui->menu.content_h = menu_height(ui);
  ui->menu.h = ui->menu.content_h;
  float max_h = menu_max_h(ui);
  if (ui->menu.h > max_h) ui->menu.h = max_h;
  ui->menu.scroll_y = 0.0f;
  ui->menu.sub_scroll_y = 0.0f;
  ccord_scroller_cancel(&ui->menu_glide);
  ccord_scroller_cancel(&ui->menu_sub_glide);
  ui->menu.x = x;
  ui->menu.y = y;
  if (ui->menu.x + ui->menu.w > ui->win_w - MENU_EDGE) ui->menu.x = ui->win_w - ui->menu.w - MENU_EDGE;
  if (ui->menu.y + ui->menu.h > ui->win_h - MENU_EDGE) ui->menu.y = ui->win_h - MENU_EDGE - ui->menu.h;
  if (ui->menu.x < MENU_EDGE) ui->menu.x = MENU_EDGE;
  if (ui->menu.y < MENU_EDGE) ui->menu.y = MENU_EDGE;
  ui->dirty = true;
}

ccord_rect ui_menu_item_rect(const ccord_ui *ui, size_t idx)
{
  return idx < ui->menu.count ? item_rect(ui, idx) : (ccord_rect){0};
}

bool ccord_ui_context_menu_find(const ccord_ui *ui, const char *label, ccord_rect *out)
{
  if (!ui || !ui->menu.open || !label) return false;
  for (size_t i = 0; i < ui->menu.count; i++) {
    const menu_item *it = &ui->menu.items[i];
    if (it->parent >= 0 && it->parent != ui->menu.open_sub) continue;
    if (it->label && strcmp(it->label, label) == 0) {
      if (out) *out = item_rect(ui, i);
      return true;
    }
  }
  return false;
}

static bool in_sub_box(const ccord_ui *ui, float x, float y)
{
  ccord_rect sb = sub_box(ui);
  return sb.w > 0.0f && ui_in_rect(x, y, sb);
}

static bool in_menu_interior(const ccord_ui *ui, float x, float y)
{
  return x >= ui->menu.x && x <= ui->menu.x + ui->menu.w &&
         y >= ui->menu.y && y <= ui->menu.y + ui->menu.h;
}

// The item under the point: the open fly-out's items first (they sit over the right edge of the menu), then the
// top-level ones. -1 for padding, separators, scrolled-off rows and outside.
static int hit_item(const ccord_ui *ui, float x, float y)
{
  if (in_sub_box(ui, x, y)) {
    for (size_t i = 0; i < ui->menu.count; i++) {
      if (ui->menu.items[i].parent == ui->menu.open_sub && ui_in_rect(x, y, item_rect(ui, i))) return (int)i;
    }
    return -1;
  }
  if (!in_menu_interior(ui, x, y)) return -1;
  for (size_t i = 0; i < ui->menu.count; i++) {
    if (ui->menu.items[i].parent < 0 && ui_in_rect(x, y, item_rect(ui, i))) return (int)i;
  }
  return -1;
}

static void activate(ccord_ui *ui, size_t idx)
{
  menu_item it = ui->menu.items[idx];
  ccord_menu_context ctx = ui->menu.ctx;
  if (it.builtin >= MENU_BUILTIN_TEXT_PASTE && it.builtin <= MENU_BUILTIN_TEXT_CLEAR) {
    ccord_text_edit *te = ui->menu_text_edit;
    bool is_composer = ui->menu_text_is_composer;
    bool is_quickcss = ui->menu_text_is_quickcss;
    bool is_search = ui->menu_text_is_search;
    if (!te) return;
    if (it.builtin == MENU_BUILTIN_TEXT_PASTE) {
      bool pasted = false;
      if (ui->d.host.clipboard_get) {
        char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
        if (clip && clip[0]) {
          ccord_error ierr = {0};
          ccord_status st = is_composer ? ccord_text_edit_paste(te, clip, strlen(clip), &ierr)
                                        : ccord_text_edit_insert(te, clip, strlen(clip), &ierr);
          if (st == CCORD_OK) {
            pasted = true;
            if (is_quickcss) ui_quickcss_changed(ui);
          } else {
            ui_notice(ui, "%s", ierr.message[0] ? ierr.message : "Could not paste text");
          }
        }
        ccord_free(clip);
      }
      if (!pasted && is_composer && ui->d.host.get_clipboard_image) {
        uint8_t *png = nullptr;
        size_t png_len = 0;
        if (ui->d.host.get_clipboard_image(ui->d.host.ud, &png, &png_len) && png && png_len > 0) {
          (void)ccord_ui_attach_bytes(ui, "clipboard.png", png, png_len);
          ccord_free(png);
        }
      }
      (void)is_search;
      ui->dirty = true;
      return;
    }
    if (it.builtin == MENU_BUILTIN_TEXT_COPY || it.builtin == MENU_BUILTIN_TEXT_CUT) {
      if (ui->d.host.clipboard_set && te->len > 0) {
        char *sel = ccord_text_edit_copy_selection(te);
        if (sel) {
          ui->d.host.clipboard_set(ui->d.host.ud, sel);
          ccord_free(sel);
          if (it.builtin == MENU_BUILTIN_TEXT_CUT) ccord_text_edit_backspace(te);
        } else {
          ui->d.host.clipboard_set(ui->d.host.ud, ccord_text_edit_cstr(te));
          if (it.builtin == MENU_BUILTIN_TEXT_CUT) ccord_text_edit_clear(te);
        }
        if (it.builtin == MENU_BUILTIN_TEXT_CUT && is_quickcss) ui_quickcss_changed(ui);
        ui_notice(ui, it.builtin == MENU_BUILTIN_TEXT_CUT ? "Cut to clipboard" : "Copied to clipboard");
      }
      ui->dirty = true;
      return;
    }
    if (it.builtin == MENU_BUILTIN_TEXT_SELECT_ALL) {
      ccord_text_edit_select_all(te);
      ui->dirty = true;
      return;
    }
    if (it.builtin == MENU_BUILTIN_TEXT_CLEAR) {
      ccord_text_edit_clear(te);
      if (is_quickcss) ui_quickcss_changed(ui);
      ui->dirty = true;
      return;
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_MENTION_USER) {
    if (ctx.user_id == CCORD_ID_NONE || !can_mention_in_composer(ui)) return;
    const ccord_user *u = ccord_store_user(ui->d.store, ctx.user_id);
    bool need_lead = false;
    if (ui->edit.caret > 0 && ui->edit.caret <= ui->edit.len) {
      char prev = ui->edit.data[ui->edit.caret - 1u];
      need_lead = (prev != ' ' && prev != '\t' && prev != '\n');
    }
    char ins[CCORD_NAME_MAX_BYTES + 32];
    int n = 0;
    if (u && u->name && u->name[0] != '\0') {
      n = snprintf(ins, sizeof ins, "%s@%s ", need_lead ? " " : "", u->name);
    } else {
      n = snprintf(ins, sizeof ins, "%s<@%llu> ", need_lead ? " " : "", (unsigned long long)ctx.user_id);
    }
    if (n > 0 && (size_t)n < sizeof ins) {
      ccord_error err;
      if (ccord_text_edit_insert(&ui->edit, ins, (size_t)n, &err) != CCORD_OK) {
        ui_notice(ui, "%s", err.message);
        return;
      }
      ui->autocomplete_open = false;
      ui->nautocomplete = 0;
      ui->caret_epoch_ms = ui->now_ms;
      ui->caret_on = true;
      ui->dirty = true;
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_TOGGLE_MEMBER_ROLE) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ccord_id role_id = (ccord_id)it.arg;
    if (gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID || ctx.user_id == CCORD_ID_NONE || role_id == CCORD_ID_NONE) {
      return;
    }
    const ccord_id *held = nullptr;
    size_t nheld = 0;
    bool has_it = ccord_store_member_roles(ui->d.store, gid, ctx.user_id, &held, &nheld) &&
                  member_holds_role(held, nheld, role_id);
    ccord_admin_request req;
    ccord_error err;
    ccord_status st = has_it ? ccord_admin_remove_member_role(&req, gid, ctx.user_id, role_id, nullptr, &err)
                             : ccord_admin_add_member_role(&req, gid, ctx.user_id, role_id, nullptr, &err);
    if (st != CCORD_OK) {
      ui_notice(ui, "Couldn't %s: %s", has_it ? "remove the role" : "give the role", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_MOD_VIEW) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ui_server_settings_open_modview(ui, gid, ctx.user_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_DISMISS_EPHEMERAL) {
    ui_dismiss_ephemeral(ui, ctx.channel_id, ctx.message_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_COPY_TEXT) {
    if (ui->chat_sel_active && ui->chat_sel_len > 0 && ui->d.host.clipboard_set) {
      ui->d.host.clipboard_set(ui->d.host.ud, ui->chat_sel_buf);
      ui_notice(ui, "Copied selected text");
      return;
    }
    const ccord_message *m = ccord_store_find_message(ui->d.store, ctx.channel_id, ctx.message_id);
    if (m && ui->d.host.clipboard_set) {
      char raw[CCORD_MESSAGE_MAX_BYTES + 1];
      ui_message_clean_body(ui, m, raw, sizeof raw);
      size_t rlen = strlen(raw);
      char *clean = ccord_malloc(rlen + 1);
      if (clean) {
        size_t mlen = 0;
        size_t i = 0;
        while (i < rlen) {
          unsigned char c = (unsigned char)raw[i];
          if (c == 0x1B) {
            i++;
            if (i < rlen && raw[i] == '[') {
              i++;
              while (i < rlen) {
                unsigned char pc = (unsigned char)raw[i];
                if (pc >= 0x40 && pc <= 0x7E) { i++; break; }
                i++;
              }
            }
            continue;
          }
          if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            i++;
            continue;
          }
          clean[mlen++] = (char)c;
          i++;
        }
        clean[mlen] = '\0';
        ui->d.host.clipboard_set(ui->d.host.ud, clean);
        ccord_free(clean);
        ui_notice(ui, "Copied message text");
      }
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_EDIT) {
    if (!message_owned_by_self(ui, &ctx)) {
      ui_notice(ui, "You can only edit your own messages");
      return;
    }
    ui_begin_edit(ui, ctx.channel_id, ctx.message_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_REACT) {
    // The same emoji picker the hover bar's react button opens (tab 2 is the full emoji tab), aimed at this message.
    ui_expr_picker_open(ui, 2, ctx.channel_id, ctx.message_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_DELETE) {
    ccord_id self_id = ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE
                     ? ctx.guild_id
                     : (ui->d.store ? ccord_store_selected_guild(ui->d.store) : CCORD_ID_NONE);
    bool can_manage_msg = (gid != CCORD_DM_GUILD_ID &&
                           ccord_store_has_permission(ui->d.store, gid, ctx.channel_id, self_id,
                                                      CCORD_PERM_MANAGE_MESSAGES));
    if (!message_owned_by_self(ui, &ctx) && !can_manage_msg) {
      ui_notice(ui, "You can only delete your own messages");
      return;
    }
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_error err;
    if (ccord_session_delete_message(ui->d.session, ctx.channel_id, ctx.message_id, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot delete: %s", err.message);
      return;
    }
    if (ui->editing_message == ctx.message_id) ui_cancel_edit(ui);
    if (ui->reply_message == ctx.message_id) ui_cancel_reply(ui);
    return;
  }
  if (it.builtin == MENU_BUILTIN_REPLY) {
    ui_begin_reply(ui, ctx.channel_id, ctx.message_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_FORWARD) {
    ui_forward_picker_open(ui, ctx.channel_id, ctx.message_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_CROSSPOST) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_error err;
    if (ccord_session_crosspost_message(ui->d.session, ctx.channel_id, ctx.message_id, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot publish: %s", err.message);
      return;
    }
    ui_notice(ui, "Published to followers");
    return;
  }
  if (it.builtin == MENU_BUILTIN_COPY_LINK) {
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
    if (!ch || ctx.message_id == CCORD_ID_NONE || !ui->d.host.clipboard_set) return;
    char gbuf[24], cbuf[24], mbuf[24];
    ccord_id_format(ch->guild_id, gbuf);
    ccord_id_format(ctx.channel_id, cbuf);
    ccord_id_format(ctx.message_id, mbuf);
    const char *g = ch->guild_id == CCORD_DM_GUILD_ID ? "@me" : gbuf;
    char link[128];
    (void)snprintf(link, sizeof link, "https://discord.com/channels/%s/%s/%s", g, cbuf, mbuf);
    ui->d.host.clipboard_set(ui->d.host.ud, link);
    ui_notice(ui, "Copied message link");
    return;
  }
  if (it.builtin == MENU_BUILTIN_MARK_UNREAD) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_error err;
    if (ccord_session_mark_unread(ui->d.session, ctx.channel_id, ctx.message_id, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot mark unread: %s", err.message);
      return;
    }
    ui_notice(ui, "Marked unread from this message");
    return;
  }
  if (it.builtin == MENU_BUILTIN_SAVE_MESSAGE) {
    if (it.arg != 0) ui_saved_messages_save(ui, ctx.channel_id, ctx.message_id);
    else ui_saved_messages_unsave(ui, ctx.channel_id, ctx.message_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_APP_COMMAND) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
    if (!ch) return;
    size_t count = 0;
    const ccord_slash_command *cmds = ccord_store_slash_commands(ui->d.store, ch->guild_id, &count);
    const ccord_slash_command *cmd = nullptr;
    for (size_t i = 0; i < count; i++) {
      if (cmds[i].id == (ccord_id)it.arg) { cmd = &cmds[i]; break; }
    }
    if (!cmd || (cmd->command_type != 2u && cmd->command_type != 3u)) return;
    ccord_id target = cmd->command_type == 2u ? ctx.user_id : ctx.message_id;
    if (target == CCORD_ID_NONE) return;
    ccord_interaction_request ireq = {
      .type = CCORD_INTERACTION_APPLICATION_COMMAND,
      .application_id = cmd->application_id,
      .guild_id = ch->guild_id == CCORD_DM_GUILD_ID ? CCORD_DM_GUILD_ID : ch->guild_id,
      .channel_id = ctx.channel_id,
      .command_id = cmd->id,
      .command_version = cmd->version,
      .command_name = cmd->name,
      .command_type = cmd->command_type,
      .target_id = target,
      .focused_option = -1,   // not an autocomplete interaction; live validates focused_option < 0 for context commands
    };
    ccord_error err;
    if (ccord_session_send_interaction(ui->d.session, &ireq, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot run %s: %s", cmd->name, err.message);
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_PIN) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_error err;
    if (ccord_session_toggle_pin(ui->d.session, ctx.channel_id, ctx.message_id, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot pin: %s", err.message);
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_SERVER_BOOST) {
    ui_open_url(ui, "https://discord.com/nitro", "Server Boost");
    return;
  }
  if (it.builtin == MENU_BUILTIN_OPEN_NITRO) {
    ui_open_url(ui, "https://discord.com/nitro", "Nitro");
    return;
  }
  if (it.builtin == MENU_BUILTIN_OPEN_SHOP) {
    ui_open_url(ui, "https://discord.com/shop", "Shop");
    return;
  }
  if (it.builtin == MENU_BUILTIN_OPEN_BILLING) {
    ui_open_url(ui, "https://discord.com/settings/billing", "Billing");
    return;
  }
  if (it.builtin == MENU_BUILTIN_OPEN_DM) {
    ui_open_dm_with_user(ui, ctx.user_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_ADD_FRIEND) {
    ui_set_user_relationship(ui, ctx.user_id, CCORD_RELATIONSHIP_FRIEND);
    return;
  }
  if (it.builtin == MENU_BUILTIN_REMOVE_FRIEND) {
    ui_set_user_relationship(ui, ctx.user_id, CCORD_RELATIONSHIP_NONE);
    return;
  }
  if (it.builtin == MENU_BUILTIN_REPORT_MESSAGE || it.builtin == MENU_BUILTIN_REPORT_USER) {
    ui_report_target target = {.type = CCORD_REPORT_USER, .guild_id = ctx.guild_id, .user_id = ctx.user_id};
    if (it.builtin == MENU_BUILTIN_REPORT_MESSAGE) {
      const ccord_message *rm = ccord_store_find_message(ui->d.store, ctx.channel_id, ctx.message_id);
      target = (ui_report_target){.type = CCORD_REPORT_MESSAGE, .guild_id = ctx.guild_id, .channel_id = ctx.channel_id,
                                  .message_id = ctx.message_id, .user_id = rm ? rm->author_id : ctx.user_id};
    }
    ui_report_flow_open(ui, &target);
    return;
  }
  if (it.builtin == MENU_BUILTIN_BLOCK_USER) {
    bool blocked = ui->d.store && ccord_store_relationship(ui->d.store, ctx.user_id) == CCORD_RELATIONSHIP_BLOCKED;
    ui_set_user_relationship(ui, ctx.user_id, blocked ? CCORD_RELATIONSHIP_NONE : CCORD_RELATIONSHIP_BLOCKED);
    return;
  }
  if (it.builtin == MENU_BUILTIN_PIN_DM) {
    ui_dm_toggle_pin(ui, (ccord_id)it.arg);
    return;
  }
  if (it.builtin == MENU_BUILTIN_FAVORITE_DM) {
    ui_dm_toggle_favorite(ui, (ccord_id)it.arg);
    return;
  }
  if (it.builtin == MENU_BUILTIN_START_CALL) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_id call_ch = (ccord_id)it.arg != CCORD_ID_NONE ? (ccord_id)it.arg : ctx.channel_id;
    if (call_ch == CCORD_ID_NONE) return;
    ccord_id self_id = ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;
    const ccord_voice_state *vs =
      (ui->d.store && self_id != CCORD_ID_NONE) ? ccord_store_voice_user(ui->d.store, CCORD_DM_GUILD_ID, self_id) : nullptr;
    bool in_here = vs && vs->channel_id == call_ch;
    ccord_error err;
    ccord_status st = in_here ? ccord_session_call_leave(ui->d.session, call_ch, &err)
                              : ccord_session_call_join(ui->d.session, call_ch, &err);
    if (st != CCORD_OK) {
      ui_notice(ui, "%s", err.message);
      return;
    }
    if (!in_here) (void)ccord_session_select_channel(ui->d.session, call_ch, &err);
    return;
  }
  if (it.builtin == MENU_BUILTIN_USER_NOTE) {
    ccord_id uid = (ccord_id)it.arg != CCORD_ID_NONE ? (ccord_id)it.arg : ctx.user_id;
    if (uid == CCORD_ID_NONE) return;
    ui->modal_kind = UI_MODAL_USER_NOTE;
    ui->modal_guild_id = CCORD_DM_GUILD_ID;
    ui->modal_target_id = uid;
    ui->modal_focus_field = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    const char *note = ui->d.store ? ccord_store_user_note(ui->d.store, uid) : nullptr;
    if (note && note[0] != '\0') {
      ccord_error err;
      (void)ccord_text_edit_insert(&ui->modal_edit1, note, strlen(note), &err);
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_FRIEND_NICK) {
    ccord_id uid = (ccord_id)it.arg != CCORD_ID_NONE ? (ccord_id)it.arg : ctx.user_id;
    if (uid == CCORD_ID_NONE) return;
    ui->modal_kind = UI_MODAL_FRIEND_NICK;
    ui->modal_guild_id = CCORD_DM_GUILD_ID;
    ui->modal_target_id = uid;
    ui->modal_focus_field = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    const char *fnick = ui_friend_nickname(ui, uid);
    if (fnick && fnick[0] != '\0') {
      ccord_error err;
      (void)ccord_text_edit_insert(&ui->modal_edit1, fnick, strlen(fnick), &err);
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_CLOSE_DM) {
    ccord_id cid = (ccord_id)it.arg != CCORD_ID_NONE ? (ccord_id)it.arg : ctx.channel_id;
    if (cid != CCORD_ID_NONE) ui_close_dm(ui, cid);
    return;
  }
  if (it.builtin == MENU_BUILTIN_INVITE_TO_GUILD) {
    ccord_id inv_gid = (ccord_id)it.arg;
    ccord_id self_id = ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;
    ccord_id inv_ch = guild_invite_channel(ui, inv_gid, self_id);
    if (inv_gid == CCORD_ID_NONE || inv_ch == CCORD_ID_NONE) {
      ui_notice(ui, "No channel available to create an invite");
      return;
    }
    ccord_id dm_ch = ctx.channel_id != CCORD_ID_NONE ? ctx.channel_id : find_1on1_dm_channel(ui, ctx.user_id);
    uint64_t prev_seq = 0;
    char prev_code[36] = {0};
    (void)ccord_store_last_invite(ui->d.store, inv_gid, prev_code, &prev_seq);
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_create_invite(&req, inv_gid, inv_ch, 86400u, 0u, false, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't create invite: %s", err.message);
      return;
    }
    if (dm_ch == CCORD_ID_NONE && ctx.user_id != CCORD_ID_NONE && ui->d.session) {
      (void)ccord_session_open_dm(ui->d.session, ctx.user_id, &err);
    }
    ui->pending_dm_invite_guild = inv_gid;
    ui->pending_dm_invite_channel = dm_ch;
    ui->pending_dm_invite_user = ctx.user_id;
    ui->pending_dm_invite_prev_count = (size_t)prev_seq;
    ui->pending_dm_invite_until_ms = ui->now_ms + 10000u;
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_IGNORE_USER) {
    ccord_id uid = (ccord_id)it.arg != CCORD_ID_NONE ? (ccord_id)it.arg : ctx.user_id;
    if (uid != CCORD_ID_NONE) ui_user_toggle_ignore(ui, uid);
    return;
  }
  if (it.builtin == MENU_BUILTIN_VIEW_PROFILE) {
    ccord_id pg = (ctx.guild_id != CCORD_ID_NONE && ctx.guild_id != CCORD_DM_GUILD_ID) ? ctx.guild_id : CCORD_ID_NONE;
    ui_open_user_profile_modal(ui, ctx.user_id, pg);
    return;
  }
  if (it.builtin == MENU_BUILTIN_LOCAL_MUTE) {
    if (ctx.user_id != CCORD_ID_NONE && ui->d.store) {
      ccord_error err;
      (void)ccord_store_set_user_local_mute(ui->d.store, ctx.user_id, it.arg != 0, &err);
      ui->dirty = true;
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_WATCH_STREAM) {
    if (ui->d.session && ctx.user_id != CCORD_ID_NONE) {
      ccord_id ch_id = (ccord_id)it.arg != CCORD_ID_NONE ? (ccord_id)it.arg : ctx.channel_id;
      const ccord_watched_stream *ws = ccord_session_watched_stream(ui->d.session);
      ccord_error err;
      if (ws && ws->active && ws->user_id == ctx.user_id) {
        (void)ccord_session_stop_watching(ui->d.session, &err);
      } else if (ch_id != CCORD_ID_NONE) {
        (void)ccord_session_watch_stream(ui->d.session, ctx.guild_id, ch_id, ctx.user_id, &err);
      }
      ui->dirty = true;
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_ADD_DM_RECIPIENT) {
    ui->modal_kind = UI_MODAL_ADD_DM_RECIPIENT;
    ui->modal_guild_id = CCORD_DM_GUILD_ID;
    ui->modal_target_id = ctx.channel_id;
    ui->modal_focus_field = 0;
    ui->new_dm_recipient_count = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    ccord_text_edit_clear(&ui->modal_edit2);
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_EDIT_GROUP_DM) {
    ui->modal_kind = UI_MODAL_EDIT_GROUP_DM;
    ui->modal_guild_id = CCORD_DM_GUILD_ID;
    ui->modal_target_id = ctx.channel_id;
    ui->modal_type_sel = 0;
    ui->modal_focus_field = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
    if (ch && ch->name && ch->name[0]) {
      ccord_error err;
      (void)ccord_text_edit_insert(&ui->modal_edit1, ch->name, strlen(ch->name), &err);
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_REMOVE_DM_RECIPIENT) {
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_remove_dm_recipient(&req, ctx.channel_id, ctx.user_id, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot remove member: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_EDIT_CHANNEL) {
    if (ui_channel_settings_open(ui, ctx.channel_id)) return;
    ui->modal_kind = UI_MODAL_EDIT_CHANNEL;
    ui->modal_guild_id = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ui->modal_target_id = ctx.channel_id;
    ccord_text_edit_clear(&ui->modal_edit1);
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
    if (ch && ch->name) {
      ccord_error err;
      (void)ccord_text_edit_insert(&ui->modal_edit1, ch->name, strlen(ch->name), &err);
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_CLONE_CHANNEL) {
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
    if (!ch) return;
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ch->guild_id;
    uint8_t dtype = ch->discord_type != CCORD_DISCORD_CHANNEL_UNKNOWN
                      ? ch->discord_type
                      : (ch->kind == CCORD_CHANNEL_VOICE ? 2u : (ch->kind == CCORD_CHANNEL_CATEGORY ? 4u : 0u));
    size_t nows = 0;
    const ccord_overwrite *ows = ccord_store_channel_overwrites(ui->d.store, ch->id, &nows);
    const ccord_forum_meta *fm = ccord_discord_type_is_forum(dtype) ? ccord_store_forum_meta(ui->d.store, ch->id) : nullptr;
    ccord_admin_channel_clone cl = {
      .name = ch->name ? ch->name : "channel",
      .discord_type = dtype,
      .parent_id = ch->parent_id,
      .topic = ch->topic,
      .nsfw = ch->nsfw,
      .slowmode_s = ch->rate_limit_per_user,
      .bitrate = ch->bitrate,
      .user_limit = ch->user_limit,
      .default_auto_archive_duration = ch->default_auto_archive_duration,
      .overwrites = ows,
      .overwrite_count = nows,
      .available_tags = fm ? fm->tags : nullptr,
      .available_tag_count = fm ? fm->tag_count : 0,
    };
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_clone_channel(&req, gid, &cl, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't clone channel: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_VOICE_MUTE) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_set_voice_mute(&req, gid, ctx.user_id, it.arg != 0, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't change server mute: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_VOICE_DEAF) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_set_voice_deaf(&req, gid, ctx.user_id, it.arg != 0, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't change server deafen: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_VOICE_MOVE || it.builtin == MENU_BUILTIN_VOICE_DISCONNECT) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ccord_id dst = it.builtin == MENU_BUILTIN_VOICE_MOVE ? (ccord_id)it.arg : CCORD_ID_NONE;
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_move_voice_member(&req, gid, ctx.user_id, dst, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't move voice member: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_CREATE_CHANNEL) {
    ui->modal_kind = UI_MODAL_CREATE_CHANNEL;
    ui->modal_guild_id = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
    ui->modal_target_id = ch ? (ch->kind == CCORD_CHANNEL_CATEGORY ? ch->id : ch->parent_id) : CCORD_ID_NONE;
    ui->modal_type_sel = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_CREATE_THREAD) {
    ui->modal_kind = UI_MODAL_CREATE_THREAD;
    ui->modal_guild_id = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ui->modal_target_id = ctx.channel_id;
    ui->modal_msg_id = (ctx.target == CCORD_MENU_MESSAGE) ? ctx.message_id : CCORD_ID_NONE;
    ccord_text_edit_clear(&ui->modal_edit1);
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_VIEW_REACTIONS) {
    const ccord_message *m = ccord_store_find_message(ui->d.store, ctx.channel_id, ctx.message_id);
    if (!m) return;
    ui->reactions_open = true;
    ui->reactions_channel_id = ctx.channel_id;
    ui->reactions_msg_id = ctx.message_id;
    ui->reactions_tab = 0;
    ui->reactions_scroll = 0.0f;
    if (ui->d.session && m->reactions) {
      for (size_t ri = 0; ri < m->reaction_kind_count; ri++) {
        if (m->reactions[ri].emoji && m->reactions[ri].count > 0) {
          ccord_error rerr;
          (void)ccord_session_request_reaction_users(ui->d.session, ctx.channel_id, ctx.message_id,
                                                     m->reactions[ri].emoji, 50u, &rerr);
          break;
        }
      }
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_DELETE_CHANNEL) {
    const ccord_channel *ch = ctx.channel_id != CCORD_ID_NONE ? ccord_store_channel(ui->d.store, ctx.channel_id) : nullptr;
    if (!ch) return;
    char label[CCORD_NAME_MAX_BYTES + 2];
    (void)snprintf(label, sizeof label, "%s%s", ch->kind == CCORD_CHANNEL_TEXT ? "#" : "", ch->name ? ch->name : "");
    ui_confirm_open(ui, CCORD_ADMIN_DELETE_CHANNEL, ch->guild_id, ch->id, label);
    return;
  }
  if (it.builtin == MENU_BUILTIN_SERVER_SETTINGS) {
    ui_server_settings_open(ui, ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store));
    return;
  }
  if (it.builtin == MENU_BUILTIN_GUILD_EVENTS) {
    ui_scheduled_events_open(ui, ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store));
    return;
  }
  if (it.builtin == MENU_BUILTIN_LEAVE_GUILD) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    const ccord_guild *gd = ccord_store_guild(ui->d.store, gid);
    if (!gd) return;
    ui_confirm_open(ui, CCORD_ADMIN_LEAVE_GUILD, gid, CCORD_ID_NONE, gd->name && gd->name[0] ? gd->name : "this server");
    return;
  }
  if (it.builtin == MENU_BUILTIN_CHANGE_NICK) {
    ui->modal_kind = UI_MODAL_CHANGE_NICK;
    ui->modal_guild_id = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ui->modal_target_id = ctx.user_id;
    ccord_text_edit_clear(&ui->modal_edit1);
    const ccord_member *m = ccord_store_find_member(ui->d.store, ui->modal_guild_id, ctx.user_id);
    if (m && m->nick[0] != '\0') {
      ccord_error err;
      (void)ccord_text_edit_insert(&ui->modal_edit1, m->nick, strlen(m->nick), &err);
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_TIMEOUT_MEMBER || it.builtin == MENU_BUILTIN_KICK_MEMBER ||
      it.builtin == MENU_BUILTIN_BAN_MEMBER) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ccord_admin_op op = it.builtin == MENU_BUILTIN_TIMEOUT_MEMBER ? CCORD_ADMIN_TIMEOUT
                        : it.builtin == MENU_BUILTIN_KICK_MEMBER  ? CCORD_ADMIN_KICK
                                                                  : CCORD_ADMIN_BAN;
    ui_confirm_open(ui, op, gid, ctx.user_id, ui_member_display_name(ui, gid, ctx.user_id));
    return;
  }
  if (it.builtin == MENU_BUILTIN_REMOVE_TIMEOUT) {   // lifting a timeout is not destructive: no dialog
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_timeout(&req, gid, ctx.user_id, ui->now_unix_ms, 0, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't remove the timeout: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_MARK_READ) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_id guild = CCORD_ID_NONE, channel = CCORD_ID_NONE;
    if (ctx.target == CCORD_MENU_CHANNEL) {
      const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
      if (!ch) return;
      guild = ch->guild_id;
      channel = ch->id;
    } else {
      guild = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    }
    size_t acked = 0;
    ccord_error err;
    if (ccord_session_mark_read(ui->d.session, guild, channel, &acked, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't mark as read: %s", err.message);
      return;
    }
    if (ui->new_marker_active) {
      const ccord_channel *mc = ccord_store_channel(ui->d.store, ui->new_marker_channel);
      if (!mc || mc->unread_count == 0) ui_dismiss_new_marker(ui, false);
    }
    size_t left = ccord_store_read_targets(ui->d.store, guild, channel, ui->now_unix_ms, nullptr, 0);
    if (left > 0) {   // a very busy server is taken in steps so one click cannot flood Discord
      ui_notice(ui, "Marked %zu channels as read; %zu more are left. Choose Mark As Read again for them.", acked, left);
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_FOLDER_MARK_READ) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_id fid = ui->menu_folder_id;
    size_t ng = ccord_store_guild_count(ui->d.store);
    size_t total_acked = 0;
    for (size_t gi = 0; gi < ng && total_acked < CCORD_MARK_READ_MAX; gi++) {
      const ccord_guild *gd = ccord_store_guild_at(ui->d.store, gi);
      if (!gd || gd->folder_id != fid) continue;
      if (ccord_store_read_targets(ui->d.store, gd->id, CCORD_ID_NONE, ui->now_unix_ms, nullptr, 0) == 0) continue;
      size_t acked = 0;
      ccord_error err;
      if (ccord_session_mark_read(ui->d.session, gd->id, CCORD_ID_NONE, &acked, &err) == CCORD_OK) {
        total_acked += acked;
      }
    }
    if (ui->new_marker_active) {
      const ccord_channel *mc = ccord_store_channel(ui->d.store, ui->new_marker_channel);
      if (!mc || mc->unread_count == 0) ui_dismiss_new_marker(ui, false);
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_FOLDER_SETTINGS) {
    ccord_id fid = ui->menu_folder_id;
    if (fid == CCORD_ID_NONE) return;
    ui->modal_kind = UI_MODAL_FOLDER_SETTINGS;
    ui->modal_target_id = fid;
    ui->modal_focus_field = 0;
    ui->modal_type_sel = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    ccord_text_edit_clear(&ui->modal_edit2);
    size_t ng = ccord_store_guild_count(ui->d.store);
    for (size_t gi = 0; gi < ng; gi++) {
      const ccord_guild *gd = ccord_store_guild_at(ui->d.store, gi);
      if (!gd || gd->folder_id != fid) continue;
      ccord_error err;
      if (gd->folder_name[0] != '\0') {
        (void)ccord_text_edit_insert(&ui->modal_edit1, gd->folder_name, strlen(gd->folder_name), &err);
      }
      if (gd->folder_color != 0) {
        char hex[16];
        int hn = snprintf(hex, sizeof hex, "#%06X", (unsigned)(gd->folder_color & 0xFFFFFFu));
        if (hn > 0) (void)ccord_text_edit_insert(&ui->modal_edit2, hex, (size_t)hn, &err);
        static const uint32_t k_presets[6] = {0u, 0x5865F2u, 0x57F287u, 0xFEE75Cu, 0xEB459Eu, 0xED4245u};
        for (int pi = 1; pi < 6; pi++) {
          if ((gd->folder_color & 0xFFFFFFu) == k_presets[pi]) {
            ui->modal_type_sel = pi;
            break;
          }
        }
      }
      break;
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_FOLDER_UNGROUP) {
    ccord_id fid = ui->menu_folder_id;
    if (fid == CCORD_ID_NONE) return;
    ccord_guild_folder_item items[CCORD_MAX_GUILDS];
    size_t ng = ccord_store_guild_count(ui->d.store);
    if (ng > CCORD_MAX_GUILDS) ng = CCORD_MAX_GUILDS;
    size_t count = 0;
    for (size_t gi = 0; gi < ng; gi++) {
      const ccord_guild *gd = ccord_store_guild_at(ui->d.store, gi);
      if (!gd) continue;
      ccord_guild_folder_item it_f = {.guild_id = gd->id};
      if (gd->folder_id != fid) {
        it_f.folder_id = gd->folder_id;
        it_f.folder_color = gd->folder_color;
        (void)snprintf(it_f.folder_name, sizeof it_f.folder_name, "%s", gd->folder_name);
      }
      items[count++] = it_f;
    }
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_set_guild_folders(&req, items, count, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't ungroup folder: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_MUTE || it.builtin == MENU_BUILTIN_UNMUTE || it.builtin == MENU_BUILTIN_NOTIFY_LEVEL ||
      it.builtin == MENU_BUILTIN_SUPPRESS_EVERYONE || it.builtin == MENU_BUILTIN_SUPPRESS_ROLES) {
    ccord_id guild = CCORD_ID_NONE, channel = CCORD_ID_NONE;
    if (ctx.target == CCORD_MENU_CHANNEL) {
      const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
      if (!ch) return;
      guild = ch->guild_id;
      channel = ch->id;
    } else {
      guild = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    }
    ccord_admin_notify_edit e = CCORD_ADMIN_NOTIFY_EDIT_INIT;
    e.channel_id = channel;
    switch (it.builtin) {
      case MENU_BUILTIN_MUTE: e.muted = 1; e.mute_duration_ms = it.arg; break;
      case MENU_BUILTIN_UNMUTE: e.muted = 0; break;
      case MENU_BUILTIN_NOTIFY_LEVEL: e.level = (int8_t)it.arg; break;
      case MENU_BUILTIN_SUPPRESS_EVERYONE: e.suppress_everyone = (int8_t)it.arg; break;
      default: e.suppress_roles = (int8_t)it.arg; break;
    }
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_set_notify(&req, guild, &e, ui->now_unix_ms, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't change the notification settings: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_COPY_USER_ID || it.builtin == MENU_BUILTIN_COPY_CHANNEL_ID ||
      it.builtin == MENU_BUILTIN_COPY_ID) {
    ccord_id target_id = CCORD_ID_NONE;
    if (it.builtin == MENU_BUILTIN_COPY_USER_ID) {
      target_id = it.arg != 0 ? (ccord_id)it.arg : ctx.user_id;
    } else if (it.builtin == MENU_BUILTIN_COPY_CHANNEL_ID) {
      target_id = it.arg != 0 ? (ccord_id)it.arg : ctx.channel_id;
    } else {
      target_id = ctx.message_id != CCORD_ID_NONE ? ctx.message_id :
                  (ctx.user_id != CCORD_ID_NONE ? ctx.user_id :
                  (ctx.channel_id != CCORD_ID_NONE ? ctx.channel_id : ctx.guild_id));
    }
    if (target_id != CCORD_ID_NONE && ui->d.host.clipboard_set) {
      char idbuf[24];
      ccord_id_format(target_id, idbuf);
      ui->d.host.clipboard_set(ui->d.host.ud, idbuf);
      ui_notice(ui, it.builtin == MENU_BUILTIN_COPY_USER_ID ? "Copied User ID"
                  : (it.builtin == MENU_BUILTIN_COPY_CHANNEL_ID ? "Copied Channel ID" : "Copied ID"));
    }
    return;
  }
  if (!ui->d.contrib || !it.owner || !it.item_id) return;
  ccord_error err;
  if (ccord_contrib_activate_menu(ui->d.contrib, it.owner, it.item_id, &ctx, &err) != CCORD_OK) {
    ui_notice(ui, "%s: %s", it.label ? it.label : "Menu item", err.message);
  }
}

// Returns true when the event was consumed (always while the menu is open).
bool ui_menu_handle_mouse(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->menu.open) return false;
  if (ev->type == CCORD_IN_WHEEL) {
    // The panel that sits under the pointer scrolls; rows outside its visible interior stop being live.
    if (ui->menu.open_sub >= 0 && in_sub_box(ui, ev->x, ev->y)) {
      float full = sub_content_height(ui);
      float visible = sub_box(ui).h;
      float max_s = full > visible ? full - visible : 0.0f;
      ccord_scroller_scroll(&ui->menu_sub_glide, &ui->menu.sub_scroll_y, -ev->wheel * M(context_menu_item_height), max_s,
                            ui_wheel_direct(ui, ev));
    } else {
      float max_s = ui->menu.content_h > ui->menu.h ? ui->menu.content_h - ui->menu.h : 0.0f;
      ccord_scroller_scroll(&ui->menu_glide, &ui->menu.scroll_y, -ev->wheel * M(context_menu_item_height), max_s,
                            ui_wheel_direct(ui, ev));
    }
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    if (ui->mobile_layout) return true;   // mobile uses explicit tap drill-down, not hover fly-outs
    int h = hit_item(ui, ev->x, ev->y);
    if (h >= 0 && ui->menu.items[h].parent < 0) {
      // Pointing at a parent opens its fly-out; pointing at any other top-level item closes it. Over a gap or the
      // fly-out's own padding the current one stays, so the pointer can cross to it.
      int want = (ui->menu.items[h].has_sub && !ui->menu.items[h].disabled) ? h : -1;
      if (want != ui->menu.open_sub) {
        ui->menu.open_sub = want;
        ui->menu.sub_scroll_y = 0.0f;
        ccord_scroller_cancel(&ui->menu_sub_glide);   // a wheel glide of the old fly-out does not carry over
        ui->dirty = true;
      }
    }
    if (h != ui->menu.hover) {
      ui->menu.hover = h;
      ui->dirty = true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN) {
    if (ev->button == 1 && ui->mobile_layout && ui->mobile_menu_in_sub && ui->menu.open_sub >= 0) {
      ccord_rect back_r = mobile_sheet_back_rect(ui);
      if (ui_in_rect(ev->x, ev->y, back_r)) {
        ui->mobile_menu_in_sub = false;
        ui->menu.open_sub = -1;
        ui->menu.sub_scroll_y = 0.0f;
        ccord_scroller_cancel(&ui->menu_sub_glide);
        ui->dirty = true;
        return true;
      }
    }
    int h = ev->button == 1 ? hit_item(ui, ev->x, ev->y) : -1;
    if (ev->button == 1 && h >= 0 && ui->menu.items[h].disabled) return true;
    if (ev->button == 1 && h >= 0 && ui->menu.items[h].has_sub) {   // a parent only opens its fly-out / sub-sheet
      ui->menu.open_sub = h;
      ui->menu.sub_scroll_y = 0.0f;
      if (ui->mobile_layout) ui->mobile_menu_in_sub = true;
      ccord_scroller_cancel(&ui->menu_sub_glide);   // a wheel glide of the old fly-out does not carry over
      ui->dirty = true;
      return true;
    }
    if (ev->button == 1 && h < 0 && (in_sub_box(ui, ev->x, ev->y) || (ui->mobile_layout && in_menu_interior(ui, ev->x, ev->y)))) {
      return true;   // padding / handle inside the sheet
    }
    // Take copies: activation can change the registry, and closing frees the item strings.
    if (h >= 0) {
      size_t idx = (size_t)h;
      activate(ui, idx);
    }
    ui_menu_close(ui);
    return true;
  }
  return true;
}

// One panel (the menu, or a fly-out): shadow, border, body, then the items whose parent is `parent`.
static void draw_panel(ccord_ui *ui, ccord_renderer *r, ccord_rect box, int parent, const ccord_text_style *st)
{
  (void)st;   // row text comes from uis_menu_row
  uis_popover_frame(ui, r, box);
  // Rows scrolled past either edge must not paint over the panel border or the window.
  ccord_push_clip(r, box);
  for (size_t i = 0; i < ui->menu.count; i++) {
    const menu_item *it = &ui->menu.items[i];
    if (it->parent != parent) continue;
    ccord_rect ir = item_rect(ui, i);
    if (ir.y + ir.h <= box.y || ir.y >= box.y + box.h) continue;   // fully outside the visible interior
    if (it->sep_before) {
      uis_divider(ui, r, box.x + UIS_SP_2, ir.y - SEP_H * 0.5f - 0.5f, box.w - 2.0f * UIS_SP_2);
    }
    // A parent stays lit while its fly-out is open, so it is clear what the panel beside it belongs to.
    bool lit = ((int)i == ui->menu.hover || (int)i == ui->menu.open_sub) && !it->disabled;
    uis_state mst = {.hover = lit, .disabled = it->disabled};
    if (it->sublabel) {
      uis_menu_row(ui, r, ir, "", mst, it->has_sub, it->checked, it->danger, "mitem", i);
      ccord_color fg = it->danger ? COL(danger) : COL(text_normal);
      if (lit) fg = COL(text_on_brand);
      if (it->disabled) fg = ui_alpha(fg, UIS_DISABLED_FG);
      float fs = uis_tokens_of(ui).fs_control;
      ccord_text_style lts = ui_style(ui, CCORD_FONT_REGULAR, fs, fg);
      ccord_color sub_fg = lit ? ui_alpha(COL(text_on_brand), 0.85f) : COL(text_muted);
      ccord_text_style sts = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_caption, sub_fg);
      float lh = ui_line_h(ui, fs);
      float sh = ui_line_h(ui, sts.size);
      float ty = ir.y + (ir.h - (lh + sh)) * 0.5f;
      float tw = ir.w - 2.0f * UIS_SP_2 - ((it->has_sub || it->checked) ? 18.0f : 0.0f);
      ccord_text_draw_ellipsis(r, it->label, strlen(it->label), ir.x + UIS_SP_2, ty, tw, &lts);
      ccord_text_draw_ellipsis(r, it->sublabel, strlen(it->sublabel), ir.x + UIS_SP_2, ty + lh, tw, &sts);
    } else {
      uis_menu_row(ui, r, ir, it->label, mst, it->has_sub, it->checked, it->danger, "mitem", i);
      if (it->owner && it->owner[0] != '\0' && !it->has_sub && !it->checked) {
        ccord_color tag_fg = lit ? ui_alpha(COL(text_on_brand), 0.72f) : COL(text_muted);
        ccord_text_style pts = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_caption, tag_fg);
        ccord_vec2 ptm = ccord_text_measure(r, "lua", 3, &pts, 0.0f);
        ccord_text_draw(r, "lua", 3, ir.x + ir.w - UIS_SP_2 - ptm.x, ir.y + (ir.h - ptm.y) * 0.5f, &pts, 0.0f);
      }
    }
  }
  ccord_pop_clip(r);
}

// A subtle 4 px thumb on the panel's right edge while its contents overflow the visible height.
static void draw_menu_scrollbar(ccord_ui *ui, ccord_renderer *r, ccord_rect box, float content_h, float scroll,
                                ccord_rect *out)
{
  *out = (ccord_rect){0};
  if (content_h <= box.h + 0.5f) return;
  float max_s = content_h - box.h;
  float frac = clampf(scroll / max_s, 0.0f, 1.0f);
  float w = 4.0f;
  float thumb_h = box.h * box.h / content_h;
  if (thumb_h < 20.0f) thumb_h = 20.0f;
  if (thumb_h > box.h - 8.0f) thumb_h = box.h - 8.0f;
  float top = box.y + 4.0f + frac * (box.h - 8.0f - thumb_h);
  ccord_rect thumb = {box.x + box.w - w - 3.0f, top, w, thumb_h};
  *out = thumb;
  ccord_draw_rrect(r, thumb, w * 0.5f, ui_alpha(COL(scrollbar_thumb), 0.9f));
}

void ui_menu_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (ui->menu_replay && ui->menu.open) ui_overlay_restart(ui, UI_OVL_MENU, UI_POPUP_MS);
  ui->menu_replay = false;
  float pres = ui_overlay_presence(ui, UI_OVL_MENU, ui->menu.open, UI_POPUP_MS, UI_POPUP_CLOSE_MS);
  bool fading = !ui->menu.open && ui->menu.count > 0;
  if (!ui->menu.open && (!fading || pres <= UI_PRESENCE_EPS)) {
    if (fading) ui_menu_release(ui);   // the fade is over
    return;
  }
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) + 1.0f, COL(text_normal));
  float maxw = 0.0f, subw = 0.0f;
  for (size_t i = 0; i < ui->menu.count; i++) {
    const menu_item *it = &ui->menu.items[i];
    float tw = ccord_text_measure(r, it->label, strlen(it->label), &st, 0.0f).x;
    if (it->sublabel) {
      float sw0 = ccord_text_measure(r, it->sublabel, strlen(it->sublabel), &st, 0.0f).x;
      if (sw0 > tw) tw = sw0;
    }
    float w = tw + ((it->has_sub || it->checked) ? 18.0f : ((it->owner && it->owner[0] != '\0') ? 26.0f : 0.0f));
    if (it->parent < 0) maxw = w > maxw ? w : maxw;
    else if (it->parent == ui->menu.open_sub) subw = w > subw ? w : subw;
  }
  float w = maxw + 2.0f * M(context_menu_pad) + 24.0f;
  if (w < M(context_menu_min_width)) w = M(context_menu_min_width);
  if (w > 420.0f) w = 420.0f;
  ui->menu.w = w;
  ui->menu.content_h = menu_height(ui);
  ui->menu.h = ui->menu.content_h;
  float max_h = menu_max_h(ui);
  if (ui->menu.h > max_h) ui->menu.h = max_h;
  float max_s = ui->menu.content_h > ui->menu.h ? ui->menu.content_h - ui->menu.h : 0.0f;
  ui->menu.scroll_y = clampf(ui->menu.scroll_y, 0.0f, max_s);
  float sw = subw + 2.0f * M(context_menu_pad) + 24.0f;
  if (sw < M(context_menu_min_width)) sw = M(context_menu_min_width);
  if (sw > 420.0f) sw = 420.0f;
  ui->menu.sub_w = sw;
  ui->menu.sub_content_h = sub_content_height(ui);
  float max_sub_s = ui->menu.sub_content_h > max_h ? ui->menu.sub_content_h - max_h : 0.0f;
  ui->menu.sub_scroll_y = clampf(ui->menu.sub_scroll_y, 0.0f, max_sub_s);
  if (ui->menu.x + w > ui->win_w - MENU_EDGE) ui->menu.x = ui->win_w - w - MENU_EDGE;
  if (ui->menu.x < MENU_EDGE) ui->menu.x = MENU_EDGE;
  if (ui->menu.y + ui->menu.h > ui->win_h - MENU_EDGE) ui->menu.y = ui->win_h - MENU_EDGE - ui->menu.h;
  if (ui->menu.y < MENU_EDGE) ui->menu.y = MENU_EDGE;
  ccord_rect box = {ui->menu.x, ui->menu.y, ui->menu.w, ui->menu.h};
  // The panel follows the menu's presence (it enters and leaves with it). A fly-out opening inside an open menu
  // plays its own entrance; while the menu fades out it goes with the panel.
  // Entrance of this menu (a new menu starts over) or the presence, whichever is further along; a fading menu has
  // only the presence.
  float shown = pres;
  if (ui->menu.open) {
    float enter = ui_enter(ui, "menu", ui->menu.seq, UI_POPUP_MS);
    if (enter > shown) shown = enter;
  }
  if (ui->mobile_layout) {
    // Full-width bottom sheet anchored above safe_insets.bottom with dimmed backdrop scrim & handle bar.
    float sl = ui->safe_insets.left + MENU_EDGE;
    float sr = ui->safe_insets.right + MENU_EDGE;
    float sb_pad = ui->safe_insets.bottom + MENU_EDGE;
    ui->menu.w = fmaxf(ui->win_w - sl - sr, 200.0f);
    ui->menu.sub_w = ui->menu.w;
    bool in_sub = ui->mobile_menu_in_sub && ui->menu.open_sub >= 0 &&
                  (size_t)ui->menu.open_sub < ui->menu.count && child_count(ui, ui->menu.open_sub) > 0;
    float active_content_h = in_sub ? ui->menu.sub_content_h : ui->menu.content_h;
    ui->menu.h = active_content_h > max_h ? max_h : active_content_h;
    ui->menu.x = sl;
    ui->menu.y = ui->win_h - sb_pad - ui->menu.h;
    if (ui->menu.y < ui->safe_insets.top + MENU_EDGE) ui->menu.y = ui->safe_insets.top + MENU_EDGE;
    ccord_rect sheet = {ui->menu.x, ui->menu.y, ui->menu.w, ui->menu.h};
    ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h}, ui_alpha(COL(bg_tertiary), 0.50f * shown));
    ui_overlay_begin(ui, r, UI_OVL_MENU, shown, 24.0f);
    draw_panel(ui, r, sheet, in_sub ? ui->menu.open_sub : -1, &st);
    // Centered grab handle bar at top of sheet.
    ccord_draw_rrect(r, (ccord_rect){sheet.x + (sheet.w - 36.0f) * 0.5f, sheet.y + 8.0f, 36.0f, 4.0f},
                     2.0f, ui_alpha(COL(text_muted), 0.5f));
    if (in_sub) {
      ccord_rect back_r = mobile_sheet_back_rect(ui);
      const char *plbl = ui->menu.items[ui->menu.open_sub].label;
      char blbl[96];
      snprintf(blbl, sizeof blbl, "<  %.80s", plbl ? plbl : "Back");
      ccord_text_style bts = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, COL(text_header));
      float by = back_r.y + (back_r.h - ui_line_h(ui, bts.size)) * 0.5f;
      ccord_text_draw_ellipsis(r, blbl, strlen(blbl), back_r.x + UIS_SP_2, by, back_r.w - 2.0f * UIS_SP_2, &bts);
      uis_divider(ui, r, sheet.x + UIS_SP_2, back_r.y + back_r.h - 1.0f, sheet.w - 2.0f * UIS_SP_2);
      draw_menu_scrollbar(ui, r, sheet, ui->menu.sub_content_h, ui->menu.sub_scroll_y, &ui->menu.sub_scrollbar_rect);
      ui->menu.scrollbar_rect = (ccord_rect){0};
    } else {
      draw_menu_scrollbar(ui, r, sheet, ui->menu.content_h, ui->menu.scroll_y, &ui->menu.scrollbar_rect);
      ui->menu.sub_scrollbar_rect = (ccord_rect){0};
    }
    ui_overlay_end(ui, r);
    return;
  }
  ui_overlay_begin(ui, r, UI_OVL_MENU, shown, 6.0f);
  draw_panel(ui, r, box, -1, &st);
  draw_menu_scrollbar(ui, r, box, ui->menu.content_h, ui->menu.scroll_y, &ui->menu.scrollbar_rect);
  ui_overlay_end(ui, r);
  ccord_rect sb = sub_box(ui);
  if (sb.w > 0.0f) {
    if (fading) {
      ui_overlay_begin(ui, r, UI_OVL_MENU, pres, 4.0f);
    } else {
      ui_popup_begin(ui, r, "menusub", ui->menu.seq * 64u + (uint64_t)ui->menu.open_sub, UI_POPUP_MS, 4.0f);
    }
    draw_panel(ui, r, sb, ui->menu.open_sub, &st);
    draw_menu_scrollbar(ui, r, sb, ui->menu.sub_content_h, ui->menu.sub_scroll_y, &ui->menu.sub_scrollbar_rect);
    if (fading) ui_overlay_end(ui, r);
    else ui_popup_end(r);
  } else {
    ui->menu.sub_scrollbar_rect = (ccord_rect){0};
  }
}

// Test seam for the slice: the open menu's scroll offset and how far it can move (0 when it fits or is closed).
void ui_menu_scroll_debug(const ccord_ui *ui, float *scroll, float *room)
{
  if (scroll) *scroll = ui->menu.scroll_y;
  if (room) *room = ui->menu.open && ui->menu.content_h > ui->menu.h ? ui->menu.content_h - ui->menu.h : 0.0f;
}

static bool id_list_has(const ccord_id *arr, size_t n, ccord_id id)
{
  for (size_t i = 0; i < n; i++) {
    if (arr[i] == id) return true;
  }
  return false;
}

static bool id_list_toggle(ccord_id *arr, size_t *n, size_t cap, ccord_id id)
{
  for (size_t i = 0; i < *n; i++) {
    if (arr[i] == id) {
      memmove(&arr[i], &arr[i + 1], (*n - i - 1) * sizeof arr[0]);
      (*n)--;
      return false;
    }
  }
  if (*n < cap) arr[(*n)++] = id;
  return true;
}

bool ui_dm_is_pinned(const ccord_ui *ui, ccord_id channel_id)
{
  return ui && channel_id != CCORD_ID_NONE && id_list_has(ui->pinned_dms, ui->npinned_dms, channel_id);
}

void ui_dm_toggle_pin(ccord_ui *ui, ccord_id channel_id)
{
  if (!ui || channel_id == CCORD_ID_NONE) return;
  bool on = id_list_toggle(ui->pinned_dms, &ui->npinned_dms, sizeof ui->pinned_dms / sizeof ui->pinned_dms[0], channel_id);
  ui->built_rev = UINT64_MAX;
  ui->built_lists_rev = UINT64_MAX;   // trigger channel-row rebuild so pinned order updates at once
  ui_settings_save(ui);
  ui_notice(ui, on ? "Pinned conversation" : "Unpinned conversation");
  ui->dirty = true;
}

bool ui_dm_is_favorite(const ccord_ui *ui, ccord_id channel_id)
{
  return ui && channel_id != CCORD_ID_NONE && id_list_has(ui->favorite_dms, ui->nfavorite_dms, channel_id);
}

void ui_dm_toggle_favorite(ccord_ui *ui, ccord_id channel_id)
{
  if (!ui || channel_id == CCORD_ID_NONE) return;
  bool on = id_list_toggle(ui->favorite_dms, &ui->nfavorite_dms,
                           sizeof ui->favorite_dms / sizeof ui->favorite_dms[0], channel_id);
  ui->built_rev = UINT64_MAX;
  ui->built_lists_rev = UINT64_MAX;
  ui_settings_save(ui);
  ui_notice(ui, on ? "Added to Favorites" : "Removed from Favorites");
  ui->dirty = true;
}

const char *ui_friend_nickname(const ccord_ui *ui, ccord_id user_id)
{
  if (!ui || user_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < ui->nfriend_nicks; i++) {
    if (ui->friend_nicks[i].user_id == user_id && ui->friend_nicks[i].nick[0] != '\0') {
      return ui->friend_nicks[i].nick;
    }
  }
  return nullptr;
}

void ui_set_friend_nickname(ccord_ui *ui, ccord_id user_id, const char *nick)
{
  if (!ui || user_id == CCORD_ID_NONE) return;
  while (nick && (*nick == ' ' || *nick == '\t')) nick++;
  size_t len = nick ? strlen(nick) : 0;
  while (len > 0 && (nick[len - 1] == ' ' || nick[len - 1] == '\t')) len--;
  for (size_t i = 0; i < ui->nfriend_nicks; i++) {
    if (ui->friend_nicks[i].user_id != user_id) continue;
    if (len == 0) {
      memmove(&ui->friend_nicks[i], &ui->friend_nicks[i + 1],
              (ui->nfriend_nicks - i - 1) * sizeof ui->friend_nicks[0]);
      ui->nfriend_nicks--;
      ui_settings_save(ui);
      ui_notice(ui, "Removed friend nickname");
      ui->dirty = true;
      return;
    }
    if (len > CCORD_NAME_MAX_BYTES) len = CCORD_NAME_MAX_BYTES;
    memcpy(ui->friend_nicks[i].nick, nick, len);
    ui->friend_nicks[i].nick[len] = '\0';
    ui_settings_save(ui);
    ui_notice(ui, "Updated friend nickname");
    ui->dirty = true;
    return;
  }
  if (len == 0) return;
  if (ui->nfriend_nicks < sizeof ui->friend_nicks / sizeof ui->friend_nicks[0]) {
    if (len > CCORD_NAME_MAX_BYTES) len = CCORD_NAME_MAX_BYTES;
    ui->friend_nicks[ui->nfriend_nicks].user_id = user_id;
    memcpy(ui->friend_nicks[ui->nfriend_nicks].nick, nick, len);
    ui->friend_nicks[ui->nfriend_nicks].nick[len] = '\0';
    ui->nfriend_nicks++;
    ui_settings_save(ui);
    ui_notice(ui, "Added friend nickname");
    ui->dirty = true;
  }
}

bool ui_user_is_ignored(const ccord_ui *ui, ccord_id user_id)
{
  return ui && user_id != CCORD_ID_NONE && id_list_has(ui->ignored_users, ui->nignored_users, user_id);
}

void ui_user_toggle_ignore(ccord_ui *ui, ccord_id user_id)
{
  if (!ui || user_id == CCORD_ID_NONE) return;
  bool on = id_list_toggle(ui->ignored_users, &ui->nignored_users,
                           sizeof ui->ignored_users / sizeof ui->ignored_users[0], user_id);
  ui_settings_save(ui);
  ui_notice(ui, on ? "User ignored" : "User unignored");
  ui->dirty = true;
}

void ui_dm_prefs_load_ini(ccord_ui *ui, const char *section, const char *key, const char *value)
{
  if (!ui || !section || !key || !value) return;
  if (strcmp(section, "pinned_dms") == 0) {
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(value, strlen(value), &id) && id != CCORD_ID_NONE &&
        !id_list_has(ui->pinned_dms, ui->npinned_dms, id) &&
        ui->npinned_dms < sizeof ui->pinned_dms / sizeof ui->pinned_dms[0]) {
      ui->pinned_dms[ui->npinned_dms++] = id;
    }
  } else if (strcmp(section, "favorite_dms") == 0) {
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(value, strlen(value), &id) && id != CCORD_ID_NONE &&
        !id_list_has(ui->favorite_dms, ui->nfavorite_dms, id) &&
        ui->nfavorite_dms < sizeof ui->favorite_dms / sizeof ui->favorite_dms[0]) {
      ui->favorite_dms[ui->nfavorite_dms++] = id;
    }
  } else if (strcmp(section, "ignored_users") == 0) {
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(value, strlen(value), &id) && id != CCORD_ID_NONE &&
        !id_list_has(ui->ignored_users, ui->nignored_users, id) &&
        ui->nignored_users < sizeof ui->ignored_users / sizeof ui->ignored_users[0]) {
      ui->ignored_users[ui->nignored_users++] = id;
    }
  } else if (strcmp(section, "friend_nicknames") == 0) {
    ccord_id uid = CCORD_ID_NONE;
    if (ccord_id_parse(key, strlen(key), &uid) && uid != CCORD_ID_NONE && value[0] != '\0' &&
        ui->nfriend_nicks < sizeof ui->friend_nicks / sizeof ui->friend_nicks[0]) {
      for (size_t i = 0; i < ui->nfriend_nicks; i++) {
        if (ui->friend_nicks[i].user_id == uid) return;
      }
      ui->friend_nicks[ui->nfriend_nicks].user_id = uid;
      snprintf(ui->friend_nicks[ui->nfriend_nicks].nick, sizeof ui->friend_nicks[0].nick, "%s", value);
      ui->nfriend_nicks++;
    }
  }
}

void ui_dm_prefs_save_ini(const ccord_ui *ui, FILE *f)
{
  if (!ui || !f) return;
  if (ui->npinned_dms > 0) {
    fprintf(f, "[pinned_dms]\n");
    for (size_t i = 0; i < ui->npinned_dms; i++) {
      fprintf(f, "dm_%zu = %llu\n", i, (unsigned long long)ui->pinned_dms[i]);
    }
  }
  if (ui->nfavorite_dms > 0) {
    fprintf(f, "[favorite_dms]\n");
    for (size_t i = 0; i < ui->nfavorite_dms; i++) {
      fprintf(f, "dm_%zu = %llu\n", i, (unsigned long long)ui->favorite_dms[i]);
    }
  }
  if (ui->nignored_users > 0) {
    fprintf(f, "[ignored_users]\n");
    for (size_t i = 0; i < ui->nignored_users; i++) {
      fprintf(f, "u_%zu = %llu\n", i, (unsigned long long)ui->ignored_users[i]);
    }
  }
  if (ui->nfriend_nicks > 0) {
    fprintf(f, "[friend_nicknames]\n");
    for (size_t i = 0; i < ui->nfriend_nicks; i++) {
      if (ui->friend_nicks[i].nick[0] == '\0') continue;
      fprintf(f, "%llu = %s\n", (unsigned long long)ui->friend_nicks[i].user_id, ui->friend_nicks[i].nick);
    }
  }
}

void ui_dm_invite_tick(ccord_ui *ui)
{
  if (!ui || ui->pending_dm_invite_guild == CCORD_ID_NONE) return;
  if (ui->now_ms > ui->pending_dm_invite_until_ms) {
    ui->pending_dm_invite_guild = CCORD_ID_NONE;
    return;
  }
  char code[36] = {0};
  uint64_t seq = 0;
  if (!ccord_store_last_invite(ui->d.store, ui->pending_dm_invite_guild, code, &seq) ||
      seq == (uint64_t)ui->pending_dm_invite_prev_count) {
    return;
  }
  ccord_id target_ch = ui->pending_dm_invite_channel;
  if (target_ch == CCORD_ID_NONE && ui->pending_dm_invite_user != CCORD_ID_NONE) {
    target_ch = find_1on1_dm_channel(ui, ui->pending_dm_invite_user);
    if (target_ch == CCORD_ID_NONE) return;
  }
  if (target_ch != CCORD_ID_NONE && ui->d.session) {
    char msg[96];
    snprintf(msg, sizeof msg, "https://discord.gg/%s", code);
    ccord_error err;
    if (ccord_session_send_message(ui->d.session, target_ch, msg, strlen(msg), &err) == CCORD_OK) {
      ui_notice(ui, "Sent server invite");
    } else {
      ui_notice(ui, "Couldn't send invite: %s", err.message);
    }
  }
  ui->pending_dm_invite_guild = CCORD_ID_NONE;
  ui->pending_dm_invite_channel = CCORD_ID_NONE;
  ui->pending_dm_invite_user = CCORD_ID_NONE;
}
