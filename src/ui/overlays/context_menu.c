// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "context_menu_internal.h"


static menu_aux g_menu_aux;

menu_aux *cm_menu_aux_for(const ccord_ui *ui)
{
  if (!ui) return nullptr;
  if (g_menu_aux.arena_live && g_menu_aux.owner == ui && g_menu_aux.seq == ui->menu.seq) {
    return &g_menu_aux;
  }
  return nullptr;
}

static void menu_aux_begin(ccord_ui *ui)
{
  if (!ui) return;
  if (g_menu_aux.arena_live) {
    // Only reset if we own it (or the previous owner's menu is no longer using this seq).
    if (g_menu_aux.owner == ui) {
      ccord_arena_free(&g_menu_aux.arena);
      memset(&g_menu_aux, 0, sizeof g_menu_aux);
    } else {
      return;   // another ui owns the arena; this ui will fall back to ccord_strdup
    }
  }
  ccord_arena_init(&g_menu_aux.arena, 4096u);
  g_menu_aux.owner = ui;
  g_menu_aux.seq = ui->menu.seq;
  g_menu_aux.arena_live = true;
  g_menu_aux.invite_parent = -1;
  g_menu_aux.measured_open_sub = -2;
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
  if (!ui) return;
  menu_aux *aux = cm_menu_aux_for(ui);
  for (size_t i = 0; i < ui->menu.count; i++) {
    if (aux && aux->from_arena[i]) {
      ui->menu.items[i] = (menu_item){0};
    } else {
      item_free(&ui->menu.items[i]);
    }
  }
  if (aux) {
    ccord_arena_free(&aux->arena);
    memset(aux, 0, sizeof *aux);
  }
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

static char *menu_str_copy(menu_aux *aux, const char *s)
{
  if (!s) return nullptr;
  if (aux && aux->arena_live) {
    return ccord_arena_strndup(&aux->arena, s, strlen(s));
  }
  return ccord_strdup(s);
}

static bool add_item_ex(ccord_ui *ui, const char *label, const char *sublabel, menu_builtin builtin, const char *owner,
                        const char *item_id, bool sep, bool disabled, bool danger, int64_t arg)
{
  if (ui->menu.count >= UI_MENU_MAX_ITEMS) return false;
  menu_aux *aux = cm_menu_aux_for(ui);
  bool use_arena = aux != nullptr && aux->arena_live;
  menu_item it = {
    .builtin = builtin,
    .sep_before = sep,
    .disabled = disabled,
    .danger = danger,
    .parent = -1,
    .arg = arg,
  };
  it.label = menu_str_copy(use_arena ? aux : nullptr, label);
  if (sublabel) it.sublabel = menu_str_copy(use_arena ? aux : nullptr, sublabel);
  if (owner) it.owner = menu_str_copy(use_arena ? aux : nullptr, owner);
  if (item_id) it.item_id = menu_str_copy(use_arena ? aux : nullptr, item_id);
  if (!it.label || (sublabel && !it.sublabel) || (owner && !it.owner) || (item_id && !it.item_id)) {
    if (!use_arena) item_free(&it);
    return false;
  }
  size_t idx = ui->menu.count++;
  ui->menu.items[idx] = it;
  if (aux) aux->from_arena[idx] = use_arena;
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

size_t cm_child_count(const ccord_ui *ui, int parent)
{
  size_t n = 0;
  for (size_t i = 0; i < ui->menu.count; i++) n += ui->menu.items[i].parent == parent;
  return n;
}

float cm_row_height(const ccord_ui *ui, const menu_item *it)
{
  float base = ui->mobile_layout ? fmaxf(M(context_menu_item_height), 44.0f) : M(context_menu_item_height);
  return base + (it && it->sublabel ? 12.0f : 0.0f);
}

float cm_menu_height(const ccord_ui *ui)
{
  float h = 2.0f * M(context_menu_pad) + (ui->mobile_layout ? SHEET_HANDLE_H : 0.0f);
  for (size_t i = 0; i < ui->menu.count; i++) {
    if (ui->menu.items[i].parent >= 0) continue;   // fly-out items have their own panel
    h += cm_row_height(ui, &ui->menu.items[i]);
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
    y += cm_row_height(ui, &ui->menu.items[i]) + (ui->menu.items[i].sep_before ? SEP_H : 0.0f);
  }
  if (ui->menu.items[idx].sep_before) y += SEP_H;
  return (ccord_rect){ui->menu.x + M(context_menu_pad), y, ui->menu.w - 2.0f * M(context_menu_pad),
                      cm_row_height(ui, &ui->menu.items[idx])};
}

// Full height of the open fly-out's children (its panel scrolls when this exceeds the window).
float cm_sub_content_height(const ccord_ui *ui)
{
  int p = ui->menu.open_sub;
  if (p < 0 || (size_t)p >= ui->menu.count) return 0.0f;
  float h = 2.0f * M(context_menu_pad) + (ui->mobile_layout ? (SHEET_HANDLE_H + SHEET_BACK_H) : 0.0f);
  for (size_t i = 0; i < ui->menu.count; i++) {
    if (ui->menu.items[i].parent != p) continue;
    h += cm_row_height(ui, &ui->menu.items[i]) + (ui->menu.items[i].sep_before ? SEP_H : 0.0f);
  }
  return h;
}

ccord_rect cm_mobile_sheet_back_rect(const ccord_ui *ui)
{
  if (!ui->mobile_layout || !ui->mobile_menu_in_sub || ui->menu.open_sub < 0) return (ccord_rect){0};
  return (ccord_rect){ui->menu.x + M(context_menu_pad), ui->menu.y + SHEET_HANDLE_H + M(context_menu_pad),
                      ui->menu.w - 2.0f * M(context_menu_pad), SHEET_BACK_H};
}

// The panel of the open fly-out, beside its parent; a zero rectangle when none is open. It opens to the right and
// flips to the left when the window has no room. Like the top-level panel, its visible height is capped to the
// window and its rows scroll inside it. On mobile, sub-menus drill down in-place inside the same bottom sheet.
ccord_rect cm_sub_box(const ccord_ui *ui)
{
  int p = ui->menu.open_sub;
  if (p < 0 || (size_t)p >= ui->menu.count || cm_child_count(ui, p) == 0) return (ccord_rect){0};
  if (ui->mobile_layout) {
    if (!ui->mobile_menu_in_sub) return (ccord_rect){0};
    return (ccord_rect){ui->menu.x, ui->menu.y, ui->menu.w, ui->menu.h};
  }
  float h = cm_sub_content_height(ui);
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
  ccord_rect box = cm_sub_box(ui);
  float top_pad = M(context_menu_pad) + (ui->mobile_layout ? (SHEET_HANDLE_H + SHEET_BACK_H) : 0.0f);
  float y = box.y + top_pad - ui->menu.sub_scroll_y;
  for (size_t i = 0; i < idx; i++) {
    if (ui->menu.items[i].parent != me->parent) continue;
    y += cm_row_height(ui, &ui->menu.items[i]) + (ui->menu.items[i].sep_before ? SEP_H : 0.0f);
  }
  if (me->sep_before) y += SEP_H;
  return (ccord_rect){box.x + M(context_menu_pad), y, box.w - 2.0f * M(context_menu_pad), cm_row_height(ui, me)};
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

bool cm_message_owned_by_self(const ccord_ui *ui, const ccord_menu_context *ctx)
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

ccord_id cm_guild_invite_channel(const ccord_ui *ui, ccord_id gid, ccord_id self_id)
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
  size_t ng = ui->d.store ? ccord_store_guild_count(ui->d.store) : 0;
  int p = add_parent_ex(ui, "Invite to Server", sep, ng == 0);
  if (p < 0) return false;
  menu_aux *aux = cm_menu_aux_for(ui);
  if (aux && ng > 0) {
    // Populate invitable guilds lazily only when the user hovers/opens this submenu.
    aux->invite_parent = p;
    aux->invite_populated = false;
    aux->invite_self_id = self_id;
    return true;
  }
  size_t added = 0;
  for (size_t i = 0; i < ng; i++) {
    const ccord_guild *gd = ccord_store_guild_at(ui->d.store, i);
    if (!gd || gd->id == CCORD_DM_GUILD_ID || !gd->name || !gd->name[0]) continue;
    if (cm_guild_invite_channel(ui, gd->id, self_id) == CCORD_ID_NONE) continue;
    if (add_child(ui, p, gd->name, MENU_BUILTIN_INVITE_TO_GUILD, (int64_t)gd->id, false, false)) {
      added++;
    }
  }
  if (added == 0) ui->menu.items[p].disabled = true;
  return true;
}

void cm_ensure_invite_submenu_populated(ccord_ui *ui)
{
  menu_aux *aux = cm_menu_aux_for(ui);
  if (!aux || aux->invite_populated || aux->invite_parent < 0) return;
  if (ui->menu.open_sub != aux->invite_parent) return;
  aux->invite_populated = true;
  int p = aux->invite_parent;
  if ((size_t)p >= ui->menu.count) return;
  size_t added = 0;
  size_t ng = ui->d.store ? ccord_store_guild_count(ui->d.store) : 0;
  for (size_t i = 0; i < ng; i++) {
    const ccord_guild *gd = ccord_store_guild_at(ui->d.store, i);
    if (!gd || gd->id == CCORD_DM_GUILD_ID || !gd->name || !gd->name[0]) continue;
    if (cm_guild_invite_channel(ui, gd->id, aux->invite_self_id) == CCORD_ID_NONE) continue;
    if (add_child(ui, p, gd->name, MENU_BUILTIN_INVITE_TO_GUILD, (int64_t)gd->id, false, false)) {
      added++;
    }
  }
  aux->measured_sub = false;
  if (added == 0) {
    ui->menu.items[p].disabled = true;
    ui->menu.open_sub = -1;
    ui->mobile_menu_in_sub = false;
  }
}

ccord_id cm_find_1on1_dm_channel(const ccord_ui *ui, ccord_id user_id)
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

bool cm_can_mention_in_composer(const ccord_ui *ui)
{
  if (!ui || !ui->d.store || ui->discovery_open || ui_is_dm_friends_view(ui) || ui_is_dm_requests_view(ui) ||
      ui_forum_is_active(ui)) {
    return false;
  }
  const ccord_channel *sel = ui_selected_channel(ui);
  return ui_channel_has_chat(sel) && ui_composer_lock_reason(ui) == nullptr;
}

bool cm_member_holds_role(const ccord_id *ids, size_t n, ccord_id role_id)
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
    bool has_it = cm_member_holds_role(held, nheld, rl->id);
    (void)add_child(ui, p, rl->name, MENU_BUILTIN_TOGGLE_MEMBER_ROLE, (int64_t)rl->id, has_it, false);
  }
  return true;
}

static bool plugin_menu_when_matches(const ccord_ui *ui, const ccord_menu_context *ctx, const char *when_json)
{
  if (!when_json || !when_json[0]) return true;
  if (!ui || !ui->d.store || !ctx) return true;
  ccord_arena a = {0};
  ccord_arena_init(&a, 1024);
  ccord_error err = {0};
  const ccord_json *obj = ccord_json_parse(&a, when_json, strlen(when_json), nullptr, &err);
  if (!obj || ccord_json_type_of(obj) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&a);
    return true;
  }
  bool ok = true;
  const ccord_message *m = (ctx->channel_id != CCORD_ID_NONE && ctx->message_id != CCORD_ID_NONE)
                             ? ccord_store_find_message(ui->d.store, ctx->channel_id, ctx->message_id)
                             : nullptr;
  ccord_id uid = ctx->user_id != CCORD_ID_NONE ? ctx->user_id : (m ? m->author_id : CCORD_ID_NONE);
  const ccord_user *u = uid != CCORD_ID_NONE ? ccord_store_user(ui->d.store, uid) : nullptr;
  const ccord_channel *ch = ctx->channel_id != CCORD_ID_NONE ? ccord_store_channel(ui->d.store, ctx->channel_id) : nullptr;

  const ccord_json *jv = ccord_json_get(obj, "is_self");
  if (ok && jv && ccord_json_type_of(jv) == CCORD_JSON_BOOL) {
    bool is_self = uid != CCORD_ID_NONE && uid == ccord_store_self_id(ui->d.store);
    if (is_self != ccord_json_bool(jv, false)) ok = false;
  }
  jv = ccord_json_get(obj, "is_bot");
  if (ok && jv && ccord_json_type_of(jv) == CCORD_JSON_BOOL) {
    bool is_bot = u && u->is_bot;
    if (is_bot != ccord_json_bool(jv, false)) ok = false;
  }
  jv = ccord_json_get(obj, "has_attachments");
  if (ok && jv && ccord_json_type_of(jv) == CCORD_JSON_BOOL) {
    bool has_att = m && m->rich && m->rich->attachment_count > 0;
    if (has_att != ccord_json_bool(jv, false)) ok = false;
  }
  jv = ccord_json_get(obj, "is_dm");
  if (ok && jv && ccord_json_type_of(jv) == CCORD_JSON_BOOL) {
    bool is_dm = (ctx->guild_id == CCORD_DM_GUILD_ID) || (ch && ch->guild_id == CCORD_DM_GUILD_ID);
    if (is_dm != ccord_json_bool(jv, false)) ok = false;
  }
  jv = ccord_json_get(obj, "channel_kind");
  if (ok && jv && ccord_json_type_of(jv) == CCORD_JSON_STRING) {
    const char *want = ccord_json_string(jv, nullptr);
    const char *got = "text";
    if (ch) {
      if (ch->kind == CCORD_CHANNEL_VOICE) got = "voice";
      else if (ch->kind == CCORD_CHANNEL_STAGE) got = "stage";
      else if (ch->kind == CCORD_CHANNEL_CATEGORY) got = "category";
    }
    if (want && strcmp(want, got) != 0) ok = false;
  }
  ccord_arena_free(&a);
  return ok;
}

static void append_plugin_menu_items(ccord_ui *ui, const ccord_menu_context *ctx, ccord_menu_target target, bool had_builtin)
{
  if (!ui || !ui->d.contrib) return;
  size_t n = ccord_contrib_menu_count(ui->d.contrib, target);
  bool added_any = false;
  for (size_t i = 0; i < n; i++) {
    const ccord_menu_contribution *c = ccord_contrib_menu_at(ui->d.contrib, target, i);
    if (!c || !plugin_menu_when_matches(ui, ctx, c->when_json)) continue;
    bool sep = c->divider_before || (had_builtin && !added_any);
    if (c->submenu && c->submenu[0]) {
      int parent = -1;
      for (size_t k = 0; k < ui->menu.count; k++) {
        if (ui->menu.items[k].parent < 0 && ui->menu.items[k].has_sub && ui->menu.items[k].label &&
            strcmp(ui->menu.items[k].label, c->submenu) == 0) {
          parent = (int)k;
          break;
        }
      }
      if (parent < 0) parent = add_parent(ui, c->submenu, sep);
      if (parent < 0) break;
      if (!add_item_ex(ui, c->label, nullptr, MENU_BUILTIN_NONE, c->owner, c->item_id,
                       c->divider_before, false, c->danger, 0)) {
        break;
      }
      menu_item *it = &ui->menu.items[ui->menu.count - 1u];
      it->parent = (int8_t)parent;
      it->checked = c->checked;
      added_any = true;
    } else {
      if (!add_item_ex(ui, c->label, nullptr, MENU_BUILTIN_NONE, c->owner, c->item_id, sep, false, c->danger, 0)) {
        break;
      }
      ui->menu.items[ui->menu.count - 1u].checked = c->checked;
      added_any = true;
    }
  }
}

void ui_menu_open(ccord_ui *ui, float x, float y, const ccord_menu_context *ctx)
{
  ccord_id folder_id =
    (ctx->target == CCORD_MENU_GUILD && ctx->guild_id == CCORD_ID_NONE) ? ui->menu_folder_id : CCORD_ID_NONE;
  ui->menu_replay = ui->menu.open;   // a menu opened over an open one plays its entrance again
  ui_menu_release(ui);
  ui->menu_folder_id = folder_id;
  ui->menu.seq++;
  menu_aux_begin(ui);
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
      bool owned = cm_message_owned_by_self(ui, ctx);
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
      if (builtin && confirmed) {
        int rp = add_parent(ui, "Create Reminder", false);
        if (rp >= 0) {
          (void)add_child(ui, rp, "In 15 minutes", MENU_BUILTIN_REMIND_ME, 15LL * 60 * 1000, false, false);
          (void)add_child(ui, rp, "In 1 hour", MENU_BUILTIN_REMIND_ME, 60LL * 60 * 1000, false, false);
          (void)add_child(ui, rp, "In 3 hours", MENU_BUILTIN_REMIND_ME, 3LL * 60 * 60 * 1000, false, false);
          (void)add_child(ui, rp, "Tomorrow (24 hours)", MENU_BUILTIN_REMIND_ME, 24LL * 60 * 60 * 1000, false, false);
        }
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
        if (!ui_dm_channel_is_bot(ui, nch)) {
          (void)add_item_ex(ui, call_action_label(ui, nch->id), nullptr, MENU_BUILTIN_START_CALL, nullptr, nullptr,
                            sep, false, false, (int64_t)nch->id);
          sep = false;
        }
        if (uid != CCORD_ID_NONE) {
          const char *note = ccord_store_user_note(ui->d.store, uid);
          const char *note_lbl = (note && note[0] != '\0') ? "Edit Note" : "Add Note";
          (void)add_item_ex(ui, note_lbl, "Only visible to you", MENU_BUILTIN_USER_NOTE, nullptr, nullptr,
                            sep, false, false, (int64_t)uid);
          sep = false;
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
    ccord_id dm_ch = cm_find_1on1_dm_channel(ui, uid);
    ccord_id call_ch = ctx->channel_id != CCORD_ID_NONE ? ctx->channel_id : dm_ch;
    builtin = add_item(ui, "Profile", MENU_BUILTIN_VIEW_PROFILE, nullptr, nullptr, false);
    if (builtin && cm_can_mention_in_composer(ui)) {
      builtin = add_item(ui, "Mention", MENU_BUILTIN_MENTION_USER, nullptr, nullptr, false);
    }
    if (builtin && uid != self_id) builtin = add_item(ui, "Message", MENU_BUILTIN_OPEN_DM, nullptr, nullptr, false);
    if (builtin && call_ch != CCORD_ID_NONE && uid != self_id) {
      const ccord_channel *cch = ccord_store_channel(ui->d.store, call_ch);
      const ccord_user *mu = ccord_store_user(ui->d.store, uid);
      bool bot_dm = (cch && ui_dm_channel_is_bot(ui, cch)) || (!cch && mu && mu->is_bot);
      if (!bot_dm) {
        (void)add_item_ex(ui, call_action_label(ui, call_ch), nullptr, MENU_BUILTIN_START_CALL, nullptr, nullptr,
                          false, false, false, (int64_t)call_ch);
      }
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
    if (cm_can_mention_in_composer(ui)) {
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
    append_plugin_menu_items(ui, ctx, ctx->target, builtin);
    if (ctx->target == CCORD_MENU_CHANNEL && ctx->channel_id != CCORD_ID_NONE) {
      const ccord_channel *vch = ccord_store_channel(ui->d.store, ctx->channel_id);
      if (vch && (vch->kind == CCORD_CHANNEL_VOICE || vch->kind == CCORD_CHANNEL_STAGE)) {
        append_plugin_menu_items(ui, ctx, CCORD_MENU_VOICE_CHANNEL, ui->menu.count > 0);
      }
    } else if (ctx->target == CCORD_MENU_MEMBER && ctx->channel_id != CCORD_ID_NONE) {
      const ccord_channel *vch = ccord_store_channel(ui->d.store, ctx->channel_id);
      if (vch && (vch->kind == CCORD_CHANNEL_VOICE || vch->kind == CCORD_CHANNEL_STAGE)) {
        append_plugin_menu_items(ui, ctx, CCORD_MENU_VOICE_USER, ui->menu.count > 0);
      }
    }
  }
  if (ui->menu.count == 0) {
    ui_menu_release(ui);
    return;   // nothing to show: the menu does not open
  }
  ui->menu.open = true;
  ui->menu.hover = -1;
  ui->menu.open_sub = -1;
  ui->menu.sub_w = M(context_menu_min_width);
  ui->menu.w = M(context_menu_min_width);
  ui->menu.content_h = cm_menu_height(ui);
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
  menu_aux_begin(ui);
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
  if (is_composer && ui->d.contrib) {
    ui->menu.ctx = (ccord_menu_context){
      .target = CCORD_MENU_COMPOSER,
      .guild_id = ui->d.store ? ccord_store_selected_guild(ui->d.store) : CCORD_ID_NONE,
      .channel_id = ui->d.store ? ccord_store_selected_channel(ui->d.store) : CCORD_ID_NONE,
    };
    append_plugin_menu_items(ui, &ui->menu.ctx, CCORD_MENU_COMPOSER, true);
  }

  if (ui->menu.count == 0) {
    ui_menu_release(ui);
    return;
  }
  ui->menu.open = true;
  ui->menu.hover = -1;
  ui->menu.open_sub = -1;
  ui->menu.sub_w = M(context_menu_min_width);
  ui->menu.w = M(context_menu_min_width);
  ui->menu.content_h = cm_menu_height(ui);
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

bool cm_in_sub_box(const ccord_ui *ui, float x, float y)
{
  ccord_rect sb = cm_sub_box(ui);
  return sb.w > 0.0f && ui_in_rect(x, y, sb);
}

bool cm_in_menu_interior(const ccord_ui *ui, float x, float y)
{
  return x >= ui->menu.x && x <= ui->menu.x + ui->menu.w &&
         y >= ui->menu.y && y <= ui->menu.y + ui->menu.h;
}

// The item under the point: the open fly-out's items first (they sit over the right edge of the menu), then the
// top-level ones. -1 for padding, separators, scrolled-off rows and outside. Single O(N) incremental y pass.
int cm_hit_item(const ccord_ui *ui, float x, float y)
{
  float pad = M(context_menu_pad);
  ccord_rect sb = cm_sub_box(ui);
  if (sb.w > 0.0f && ui_in_rect(x, y, sb)) {
    float top_pad = pad + (ui->mobile_layout ? (SHEET_HANDLE_H + SHEET_BACK_H) : 0.0f);
    float ry = sb.y + top_pad - ui->menu.sub_scroll_y;
    float rx = sb.x + pad, rw = sb.w - 2.0f * pad;
    if (x < rx || x > rx + rw) return -1;
    for (size_t i = 0; i < ui->menu.count; i++) {
      const menu_item *it = &ui->menu.items[i];
      if (it->parent != ui->menu.open_sub) continue;
      if (it->sep_before) ry += SEP_H;
      float rh = cm_row_height(ui, it);
      if (y >= ry && y < ry + rh) return (int)i;
      ry += rh;
    }
    return -1;
  }
  if (!cm_in_menu_interior(ui, x, y)) return -1;
  float top_pad = pad + (ui->mobile_layout ? SHEET_HANDLE_H : 0.0f);
  float ry = ui->menu.y + top_pad - ui->menu.scroll_y;
  float rx = ui->menu.x + pad, rw = ui->menu.w - 2.0f * pad;
  if (x < rx || x > rx + rw) return -1;
  for (size_t i = 0; i < ui->menu.count; i++) {
    const menu_item *it = &ui->menu.items[i];
    if (it->parent >= 0) continue;
    if (it->sep_before) ry += SEP_H;
    float rh = cm_row_height(ui, it);
    if (y >= ry && y < ry + rh) return (int)i;
    ry += rh;
  }
  return -1;
}

