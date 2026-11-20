// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void invalidate_invite_cache(ccord_session *s, const char *code)
{
  session_invite_cache *entry = sess_find_invite_cache(s, code);
  if (entry) memset(entry, 0, sizeof *entry);
}

void sess_reset_invite_cache(ccord_session *s)
{
  if (s->invite_cache) memset(s->invite_cache, 0, SESSION_INVITE_CACHE_SLOTS * sizeof *s->invite_cache);
  s->invite_cache_rr = 0;
}


static ccord_perm_verdict session_role_verdict(const ccord_session *s, const ccord_admin_request *req, ccord_id self)
{
  const ccord_store *st = s->store;
  if (!ccord_store_permissions_exact(st, req->guild_id, self)) return CCORD_PERM_ALLOWED;
  bool assign = req->op == CCORD_ADMIN_ADD_MEMBER_ROLE || req->op == CCORD_ADMIN_REMOVE_MEMBER_ROLE;
  const ccord_guild *g = ccord_store_guild(st, req->guild_id);
  uint64_t mine = ccord_store_permissions(st, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
  ccord_id owner = g ? g->owner_id : CCORD_ID_NONE;
  switch (req->op) {
    case CCORD_ADMIN_CREATE_ROLE:
      if (req->role_permissions > 0) {
        return ccord_perm_can_grant(req->guild_id, owner, self, mine, 0, (uint64_t)req->role_permissions);
      }
      return CCORD_PERM_ALLOWED;
    case CCORD_ADMIN_EDIT_ROLE: {
      ccord_perm_verdict v = ccord_store_can_edit_role(st, req->guild_id, self, req->role_id, false);
      const ccord_role *r = ccord_store_role(st, req->guild_id, req->role_id);
      if (v == CCORD_PERM_ALLOWED && r && req->role_permissions >= 0) {
        v = ccord_perm_can_grant(req->guild_id, owner, self, mine, r->permissions, (uint64_t)req->role_permissions);
      }
      return v;
    }
    case CCORD_ADMIN_REORDER_ROLES:
      for (size_t i = 0; i < req->reorder_count; i++) {
        ccord_perm_verdict v = ccord_store_can_edit_role(st, req->guild_id, self, req->reorder_ids[i], false);
        if (v != CCORD_PERM_ALLOWED) return v;
      }
      return CCORD_PERM_ALLOWED;
    case CCORD_ADMIN_DELETE_ROLE:
    case CCORD_ADMIN_ADD_MEMBER_ROLE:
    case CCORD_ADMIN_REMOVE_MEMBER_ROLE:
      return ccord_store_can_edit_role(st, req->guild_id, self, req->role_id, assign);
    default: return CCORD_PERM_ALLOWED;
  }
}

ccord_status ccord_session_admin_ex(ccord_session *s, const ccord_admin_request *req, uint64_t now_ms, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s || !req) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (req->op == CCORD_ADMIN_NONE || req->op >= CCORD_ADMIN_OP_COUNT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad admin request");
  }
  ccord_id self = ccord_store_self_id(s->store);
  // The direct messages are not a server we hold a record of: closing one, and muting them, address them by the pseudo id.
  // Account-global operations (guild folders, creating a server, previewing/accepting an invite, fetching a public
  // server/clan profile) also need no store guild.
  bool dm = req->op == CCORD_ADMIN_CLOSE_DM || req->op == CCORD_ADMIN_ADD_DM_RECIPIENT ||
            req->op == CCORD_ADMIN_REMOVE_DM_RECIPIENT || req->op == CCORD_ADMIN_EDIT_GROUP_DM ||
            req->op == CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST || req->op == CCORD_ADMIN_REJECT_MESSAGE_REQUEST ||
            (req->op == CCORD_ADMIN_SET_NOTIFY && req->guild_id == CCORD_DM_GUILD_ID);
  bool global_op = req->op == CCORD_ADMIN_SET_GUILD_FOLDERS || req->op == CCORD_ADMIN_CREATE_GUILD ||
                   req->op == CCORD_ADMIN_PREVIEW_INVITE || req->op == CCORD_ADMIN_ACCEPT_INVITE ||
                   req->op == CCORD_ADMIN_GET_GUILD_PROFILE ||
                   req->op == CCORD_ADMIN_CREATE_GROUP_DM || req->op == CCORD_ADMIN_SET_USER_NOTE ||
                   req->op == CCORD_ADMIN_SET_CUSTOM_STATUS || req->op == CCORD_ADMIN_USER_SETTINGS_PATCH ||
                   req->op == CCORD_ADMIN_AUTH_SESSIONS_LIST || req->op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT ||
                   req->op == CCORD_ADMIN_CONNECTIONS_LIST || req->op == CCORD_ADMIN_CONNECTION_EDIT ||
                   req->op == CCORD_ADMIN_CONNECTION_DELETE;
  // Voice status, stage topic/end and activity launch/leave carry no guild id in their REST path. The first three
  // and LAUNCH look the channel row up in their cases below; LEAVE only checks the instance in the store.
  bool channel_op = req->op == CCORD_ADMIN_VOICE_STATUS || req->op == CCORD_ADMIN_STAGE_TOPIC ||
                    req->op == CCORD_ADMIN_STAGE_DELETE || req->op == CCORD_ADMIN_ACTIVITY_LAUNCH ||
                    req->op == CCORD_ADMIN_ACTIVITY_LEAVE;
  const ccord_guild *g = (dm || global_op || channel_op) ? nullptr : ccord_store_guild(s->store, req->guild_id);
  if (!dm && !global_op && !channel_op && !g) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That server isn't available.");
  // At -O3 the compiler cannot tie req->op back to the dm/global/channel classification above, so it sees the
  // guild-scoped cases below as reachable with a null row. Settle the two fields they read once, here, where the
  // impossible null case simply keeps the "unknown" defaults instead of dereferencing.
  ccord_id guild_owner = g ? g->owner_id : CCORD_ID_NONE;
  uint8_t guild_premium_tier = g ? g->premium_tier : 0u;
  switch (req->op) {
    case CCORD_ADMIN_EDIT_CHANNEL:
    case CCORD_ADMIN_DELETE_CHANNEL:
    case CCORD_ADMIN_CREATE_THREAD:
    case CCORD_ADMIN_CREATE_THREAD_MSG:
    case CCORD_ADMIN_ARCHIVE_THREAD:
    case CCORD_ADMIN_JOIN_THREAD:
    case CCORD_ADMIN_LEAVE_THREAD:
    case CCORD_ADMIN_CLOSE_DM:
    case CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST:
    case CCORD_ADMIN_REJECT_MESSAGE_REQUEST: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (dm ? ch->guild_id != CCORD_DM_GUILD_ID : ch->guild_id != req->guild_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't in this server.");
      }
      if (req->op == CCORD_ADMIN_CREATE_THREAD_MSG) {
        if (req->message_id == CCORD_ID_NONE ||
            !ccord_store_find_message(s->store, req->channel_id, req->message_id)) {
          return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That message isn't available.");
        }
      }
      if ((req->op == CCORD_ADMIN_EDIT_CHANNEL || req->op == CCORD_ADMIN_DELETE_CHANNEL) && !dm &&
          !ccord_discord_type_is_thread(ch->discord_type) &&
          ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, req->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Manage Channels permission for this channel.");
        }
      }
      break;
    }
    case CCORD_ADMIN_ADD_DM_RECIPIENT:
    case CCORD_ADMIN_REMOVE_DM_RECIPIENT:
    case CCORD_ADMIN_EDIT_GROUP_DM: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That conversation isn't available.");
      if (ch->guild_id != CCORD_DM_GUILD_ID || ch->discord_type != CCORD_DISCORD_CHANNEL_GROUP_DM) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That conversation isn't a Group DM.");
      }
      if (req->op == CCORD_ADMIN_ADD_DM_RECIPIENT) {
        if (req->user_id == CCORD_ID_NONE || req->user_id == self) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Choose someone else to add to the Group DM.");
        }
        if (ch->recipient_count >= CCORD_MAX_DM_RECIPIENTS) {
          return ccord_error_set(err, CCORD_ERR_RANGE, "Group DMs can have at most 10 members.");
        }
      } else if (req->op == CCORD_ADMIN_REMOVE_DM_RECIPIENT) {
        if (req->user_id == CCORD_ID_NONE) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Choose a member to remove.");
        }
        if (ch->owner_id != CCORD_ID_NONE && self != CCORD_ID_NONE && req->user_id != self && ch->owner_id != self) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "Only the group owner can remove members.");
        }
      }
      break;
    }
    case CCORD_ADMIN_CREATE_GROUP_DM: {
      if (req->dm_recipient_count == 0 || req->dm_recipient_count > CCORD_MAX_DM_RECIPIENTS) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Select between 1 and 9 recipients.");
      }
      for (size_t i = 0; i < req->dm_recipient_count; i++) {
        if (req->dm_recipient_ids[i] == CCORD_ID_NONE || req->dm_recipient_ids[i] == self) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Invalid Group DM recipient.");
        }
      }
      break;
    }
    case CCORD_ADMIN_SET_USER_NOTE:
      if (req->user_id == CCORD_ID_NONE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Invalid user for note.");
      }
      break;
    case CCORD_ADMIN_REORDER_CHANNELS: {
      if (req->reorder_count == 0 || req->reorder_count > CCORD_ADMIN_CHANNEL_REORDER_MAX) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid channel reorder count");
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_MANAGE_CHANNELS) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Manage Channels permission to reorder channels.");
        }
      }
      for (size_t i = 0; i < req->reorder_count; i++) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->reorder_ids[i]);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
        if (ch->guild_id != req->guild_id) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't in this server.");
        }
        if (req->reorder_has_parent[i] && req->reorder_parents[i] != CCORD_ID_NONE) {
          if (ch->kind == CCORD_CHANNEL_CATEGORY) {
            return ccord_error_set(err, CCORD_ERR_INVALID, "A category cannot sit inside another category.");
          }
          const ccord_channel *par = ccord_store_channel(s->store, req->reorder_parents[i]);
          if (!par || par->guild_id != req->guild_id || par->kind != CCORD_CHANNEL_CATEGORY) {
            return ccord_error_set(err, CCORD_ERR_INVALID, "Invalid parent category.");
          }
        }
      }
      break;
    }
    case CCORD_ADMIN_KICK:
    case CCORD_ADMIN_BAN:
    case CCORD_ADMIN_TIMEOUT:
    case CCORD_ADMIN_SET_NICK: {
      static const ccord_perm_action action_of[] = {
        [CCORD_ADMIN_KICK] = CCORD_PERM_ACT_KICK, [CCORD_ADMIN_BAN] = CCORD_PERM_ACT_BAN,
        [CCORD_ADMIN_TIMEOUT] = CCORD_PERM_ACT_TIMEOUT, [CCORD_ADMIN_SET_NICK] = CCORD_PERM_ACT_SET_NICKNAME,
      };
      ccord_perm_action action = action_of[req->op];
      ccord_perm_verdict v = ccord_store_can_act_on(s->store, action, req->guild_id, self, req->user_id);
      bool lifting = req->op == CCORD_ADMIN_TIMEOUT && req->until_ms == 0;
      // Only what is certain stops a request here: whom the action may not touch (yourself, the owner, someone at or
      // above your highest role). Whether you hold the permission is Discord's to say, since our picture of your
      // roles can lag; a refusal then comes back as a failure notice with Discord's own words.
      if (v == CCORD_PERM_TARGET_SELF) return ccord_error_set(err, CCORD_ERR_INVALID, "%s", ccord_perm_verdict_text(v, action));
      if (v == CCORD_PERM_TARGET_OWNER || v == CCORD_PERM_TARGET_HIGHER || (v == CCORD_PERM_TARGET_ADMIN && !lifting)) {
        return ccord_error_set(err, CCORD_ERR_PERMISSION, "%s", ccord_perm_verdict_text(v, action));
      }
      break;
    }
    case CCORD_ADMIN_CREATE_ROLE:
    case CCORD_ADMIN_EDIT_ROLE:
    case CCORD_ADMIN_DELETE_ROLE:
    case CCORD_ADMIN_REORDER_ROLES:
    case CCORD_ADMIN_ADD_MEMBER_ROLE:
    case CCORD_ADMIN_REMOVE_MEMBER_ROLE: {
      ccord_perm_verdict v = session_role_verdict(s, req, self);
      if (v == CCORD_PERM_TARGET_HIGHER || v == CCORD_PERM_ROLE_MANAGED || v == CCORD_PERM_ESCALATION) {
        return ccord_error_set(err, CCORD_ERR_PERMISSION, "%s", ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES));
      }
      break;
    }
    case CCORD_ADMIN_SET_NOTIFY:   // the user's own setting: no permission applies, but the channel must be one of this server's
      if (req->channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
        if (ch->guild_id != req->guild_id) return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't in this server.");
      }
      break;
    case CCORD_ADMIN_EDIT_GUILD:
      if (req->has_system_channel && req->system_channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->system_channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That system channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(ch->discord_type) || ccord_store_channel_is_forum(s->store, ch->id)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "System messages must point to a text channel in this server.");
        }
      }
      if (req->has_afk_channel && req->afk_channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->afk_channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That inactive channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_VOICE ||
            ch->discord_type == CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Inactive channel must be a voice channel in this server.");
        }
      }
      if (req->has_rules_channel && req->rules_channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->rules_channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That rules channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(ch->discord_type) || ccord_store_channel_is_forum(s->store, ch->id)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Rules channel must be a text channel in this server.");
        }
      }
      if (req->has_public_updates_channel && req->public_updates_channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->public_updates_channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That community updates channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(ch->discord_type) || ccord_store_channel_is_forum(s->store, ch->id)) {
          return ccord_error_set(err, CCORD_ERR_INVALID,
                                 "Community updates channel must be a text channel in this server.");
        }
      }
      if ((req->has_community || req->has_rules_channel || req->has_public_updates_channel) &&
          ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_MANAGE_GUILD) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Server permission to update Community settings.");
        }
      }
      break;
    case CCORD_ADMIN_LEAVE_GUILD:
      if (guild_owner != CCORD_ID_NONE && guild_owner == self) {
        return ccord_error_set(err, CCORD_ERR_PERMISSION,
                               "Server owners cannot leave their server. Transfer ownership or delete the server first.");
      }
      break;
    case CCORD_ADMIN_DELETE_GUILD:
      if (guild_owner != CCORD_ID_NONE && guild_owner != self) {
        return ccord_error_set(err, CCORD_ERR_PERMISSION, "Only the server owner can delete the server.");
      }
      break;
    case CCORD_ADMIN_TRANSFER_OWNERSHIP:
      if (req->user_id == CCORD_ID_NONE || req->user_id == self) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Choose another member to transfer ownership to.");
      }
      if (guild_owner != CCORD_ID_NONE && guild_owner != self) {
        return ccord_error_set(err, CCORD_ERR_PERMISSION, "Only the server owner can transfer ownership.");
      }
      break;
    case CCORD_ADMIN_LIST_EMOJIS:
    case CCORD_ADMIN_LIST_STICKERS:
    case CCORD_ADMIN_LIST_SOUNDBOARD:
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
      }
      break;
    case CCORD_ADMIN_CREATE_EMOJI: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
      }
      uint32_t used = 0;
      size_t n = ccord_store_guild_emoji_count(s->store, req->guild_id);
      for (size_t i = 0; i < n; i++) {
        const ccord_emoji *e = ccord_store_guild_emoji_at(s->store, req->guild_id, i);
        if (e && e->animated == req->emoji_animated) used++;
      }
      if (used >= ccord_guild_emoji_slot_limit(guild_premium_tier)) {
        return ccord_error_set(err, CCORD_ERR_RANGE,
                               req->emoji_animated ? "This server has reached its animated emoji slot limit."
                                                   : "This server has reached its static emoji slot limit.");
      }
      break;
    }
    case CCORD_ADMIN_CREATE_STICKER: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
      }
      if (ccord_store_guild_sticker_count(s->store, req->guild_id) >=
          ccord_guild_sticker_slot_limit(guild_premium_tier)) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "This server has reached its sticker slot limit.");
      }
      break;
    }
    case CCORD_ADMIN_CREATE_SOUNDBOARD: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
      }
      if (ccord_store_guild_soundboard_count(s->store, req->guild_id) >=
          ccord_guild_soundboard_slot_limit(guild_premium_tier)) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "This server has reached its soundboard slot limit.");
      }
      break;
    }
    case CCORD_ADMIN_EDIT_EMOJI:
    case CCORD_ADMIN_DELETE_EMOJI: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
        const ccord_emoji *e = ccord_store_find_emoji(s->store, req->expr_id);
        if (e && e->guild_id == req->guild_id && e->user_id != CCORD_ID_NONE &&
            !ccord_perm_can_modify_expression(perms, self, e->user_id)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Expressions permission to modify expressions created by others.");
        }
      }
      break;
    }
    case CCORD_ADMIN_EDIT_STICKER:
    case CCORD_ADMIN_DELETE_STICKER: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
        const ccord_sticker *st_it = ccord_store_find_sticker(s->store, req->expr_id);
        if (st_it && st_it->guild_id == req->guild_id && st_it->user_id != CCORD_ID_NONE &&
            !ccord_perm_can_modify_expression(perms, self, st_it->user_id)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Expressions permission to modify expressions created by others.");
        }
      }
      break;
    }
    case CCORD_ADMIN_EDIT_SOUNDBOARD:
    case CCORD_ADMIN_DELETE_SOUNDBOARD: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if (!ccord_perm_can_create_expression(perms)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Create Expressions or Manage Expressions permission.");
        }
        const ccord_soundboard_sound *sb = ccord_store_find_soundboard(s->store, req->expr_id);
        if (sb && sb->guild_id == req->guild_id && sb->user_id != CCORD_ID_NONE &&
            !ccord_perm_can_modify_expression(perms, self, sb->user_id)) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Expressions permission to modify expressions created by others.");
        }
      }
      break;
    }
    case CCORD_ADMIN_PREVIEW_PRUNE:
    case CCORD_ADMIN_EXECUTE_PRUNE:
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_KICK_MEMBERS) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Kick Members permission to prune members.");
        }
        if (g && guild_owner != self && req->prune_role_count > 0) {
          size_t nroles = 0, nactor = 0;
          const ccord_role *roles = ccord_store_roles(s->store, req->guild_id, &nroles);
          const ccord_id *actor_roles = nullptr;
          if (roles && nroles > 0 && ccord_store_member_roles(s->store, req->guild_id, self, &actor_roles, &nactor)) {
            const ccord_role *actor_top = ccord_perm_top_role(req->guild_id, roles, nroles, actor_roles, nactor);
            for (size_t i = 0; i < req->prune_role_count; i++) {
              const ccord_role *ro = ccord_store_role(s->store, req->guild_id, req->prune_role_ids[i]);
              if (!ro || ro->managed || !ccord_perm_outranks(actor_top, ro)) {
                return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                       "You can only prune roles lower than your highest role.");
              }
            }
          }
        }
      }
      break;
    case CCORD_ADMIN_LIST_AUTOMOD_RULES:
    case CCORD_ADMIN_CREATE_AUTOMOD_RULE:
    case CCORD_ADMIN_EDIT_AUTOMOD_RULE:
    case CCORD_ADMIN_DELETE_AUTOMOD_RULE:
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_MANAGE_GUILD) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Server permission to manage AutoMod rules.");
        }
      }
      break;
    case CCORD_ADMIN_LIST_WEBHOOKS:
    case CCORD_ADMIN_CREATE_WEBHOOK:
    case CCORD_ADMIN_EDIT_WEBHOOK:
    case CCORD_ADMIN_DELETE_WEBHOOK:
    case CCORD_ADMIN_FOLLOW_CHANNEL: {
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_MANAGE_WEBHOOKS) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Webhooks permission to manage integrations.");
        }
      }
      if (req->op == CCORD_ADMIN_CREATE_WEBHOOK) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That webhook channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(ch->discord_type)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Webhooks must post to a text channel in this server.");
        }
      } else if (req->op == CCORD_ADMIN_EDIT_WEBHOOK && req->target_channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->target_channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That webhook channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(ch->discord_type)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Webhooks must post to a text channel in this server.");
        }
      } else if (req->op == CCORD_ADMIN_FOLLOW_CHANNEL) {
        const ccord_channel *src = ccord_store_channel(s->store, req->channel_id);
        if (!src) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That announcement channel isn't available.");
        if (src->discord_type != CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Only announcement channels can be followed.");
        }
        const ccord_channel *dst = ccord_store_channel(s->store, req->target_channel_id);
        if (!dst) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That destination channel isn't available.");
        if (dst->guild_id != req->guild_id || dst->kind != CCORD_CHANNEL_TEXT ||
            ccord_discord_type_is_thread(dst->discord_type)) {
          return ccord_error_set(err, CCORD_ERR_INVALID,
                                 "Followed announcements must post to a text channel in this server.");
        }
      }
      break;
    }
    case CCORD_ADMIN_LIST_APP_CMD_PERMS:
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & (CCORD_PERM_MANAGE_GUILD | CCORD_PERM_MANAGE_WEBHOOKS)) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Server or Manage Webhooks permission to view integration permissions.");
        }
      }
      break;
    case CCORD_ADMIN_GET_WELCOME_SCREEN:
    case CCORD_ADMIN_EDIT_WELCOME_SCREEN:
    case CCORD_ADMIN_GET_ONBOARDING:
    case CCORD_ADMIN_PUT_ONBOARDING:
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_MANAGE_GUILD) == 0) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Server permission to manage Community settings.");
        }
      }
      break;
    case CCORD_ADMIN_PUT_CHANNEL_OVERWRITE:
    case CCORD_ADMIN_DELETE_CHANNEL_OVERWRITE: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (ch->guild_id != req->guild_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't in this server.");
      }
      if (ccord_discord_type_is_thread(ch->discord_type)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Threads inherit permissions from their parent channel.");
      }
      if (req->op == CCORD_ADMIN_PUT_CHANNEL_OVERWRITE && req->overwrite.kind == CCORD_OVERWRITE_ROLE &&
          ccord_store_roles_known(s->store, req->guild_id) &&
          !ccord_store_role(s->store, req->guild_id, req->overwrite.id)) {
        return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That role isn't available.");
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, req->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_MANAGE_ROLES) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Permissions in this channel to edit overwrites.");
        }
        if (req->op == CCORD_ADMIN_PUT_CHANNEL_OVERWRITE) {
          uint64_t old_allow = 0u, old_deny = 0u;
          size_t nows = 0u;
          const ccord_overwrite *ows = ccord_store_channel_overwrites(s->store, req->channel_id, &nows);
          for (size_t i = 0u; ows && i < nows; i++) {
            if (ows[i].id == req->overwrite.id) {
              old_allow = ows[i].allow;
              old_deny = ows[i].deny;
              break;
            }
          }
          uint64_t changed = (old_allow ^ req->overwrite.allow) | (old_deny ^ req->overwrite.deny);
          if ((changed & ~ch_perms) != 0u) {
            return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                   "You can't grant or deny permissions you don't hold in this channel.");
          }
        }
      }
      break;
    }
    case CCORD_ADMIN_LIST_CHANNEL_INVITES: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (ch->guild_id != req->guild_id || ch->kind == CCORD_CHANNEL_CATEGORY ||
          ccord_discord_type_is_thread(ch->discord_type)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Channel invites are not available for this channel.");
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, req->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Channels permission to view channel invites.");
        }
      }
      break;
    }
    case CCORD_ADMIN_CLONE_CHANNEL: {
      if (req->parent_id != CCORD_ID_NONE && req->channel_type != 4u) {
        const ccord_channel *par = ccord_store_channel(s->store, req->parent_id);
        if (!par || par->guild_id != req->guild_id || par->kind != CCORD_CHANNEL_CATEGORY) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Invalid parent category.");
        }
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, CCORD_ID_NONE, self, session_now_unix_ms(s));
        if ((perms & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Manage Channels permission to clone channels.");
        }
        for (size_t i = 0u; i < req->clone_overwrite_count; i++) {
          uint64_t bits = req->clone_overwrites[i].allow | req->clone_overwrites[i].deny;
          if ((bits & ~perms) != 0u) {
            return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                   "You can't clone permission overwrites you don't hold.");
          }
        }
      }
      break;
    }
    case CCORD_ADMIN_SET_VOICE_MUTE:
    case CCORD_ADMIN_SET_VOICE_DEAF:
    case CCORD_ADMIN_MOVE_VOICE_MEMBER: {
      if (req->user_id == CCORD_ID_NONE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Choose a voice participant.");
      }
      const ccord_voice_state *vs = ccord_store_voice_user(s->store, req->guild_id, req->user_id);
      if (!vs || vs->channel_id == CCORD_ID_NONE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That member is not connected to a voice channel.");
      }
      if (req->op == CCORD_ADMIN_MOVE_VOICE_MEMBER && req->target_channel_id != CCORD_ID_NONE) {
        const ccord_channel *dst = ccord_store_channel(s->store, req->target_channel_id);
        if (!dst) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That voice channel isn't available.");
        if (dst->guild_id != req->guild_id || dst->kind != CCORD_CHANNEL_VOICE) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Choose a voice channel in this server.");
        }
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t vperms = ccord_store_permissions(s->store, req->guild_id, vs->channel_id, self, session_now_unix_ms(s));
        if (req->op == CCORD_ADMIN_SET_VOICE_MUTE && (vperms & CCORD_PERM_MUTE_MEMBERS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Mute Members permission in this voice channel.");
        }
        if (req->op == CCORD_ADMIN_SET_VOICE_DEAF && (vperms & CCORD_PERM_DEAFEN_MEMBERS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Deafen Members permission in this voice channel.");
        }
        if (req->op == CCORD_ADMIN_MOVE_VOICE_MEMBER) {
          if ((vperms & CCORD_PERM_MOVE_MEMBERS) == 0u) {
            return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                   "You need Move Members permission in this voice channel.");
          }
          if (req->target_channel_id != CCORD_ID_NONE) {
            uint64_t dperms = ccord_store_permissions(s->store, req->guild_id, req->target_channel_id, self, session_now_unix_ms(s));
            if ((dperms & CCORD_PERM_CONNECT) == 0u) {
              return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                     "You need Connect permission in the destination voice channel.");
            }
          }
        }
      }
      break;
    }
    case CCORD_ADMIN_VOICE_STATUS: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (ch->guild_id == CCORD_DM_GUILD_ID ||
          (ch->kind != CCORD_CHANNEL_VOICE && ch->kind != CCORD_CHANNEL_STAGE)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Voice status needs a voice or stage channel.");
      }
      if (ccord_store_permissions_exact(s->store, ch->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, ch->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_SET_VOICE_CHANNEL_STATUS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Set Voice Channel Status permission in this channel.");
        }
      }
      break;
    }
    case CCORD_ADMIN_STAGE_REQUEST_SPEAK: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch || ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_STAGE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't a stage in this server.");
      }
      const ccord_voice_state *vs = sess_self_voice(s, req->guild_id);
      if (!vs || vs->channel_id != req->channel_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Join the stage before asking to speak.");
      }
      if (req->request_to_speak_ms != 0 && !vs->suppress) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "You're already a speaker on this stage.");
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, req->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_REQUEST_TO_SPEAK) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Request to Speak permission in this stage.");
        }
      }
      break;
    }
    case CCORD_ADMIN_STAGE_SET_SPEAKER: {
      if (req->user_id == CCORD_ID_NONE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Choose a stage participant.");
      }
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch || ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_STAGE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't a stage in this server.");
      }
      // Discord answers 403 before it looks at the target's state (same order as the server).
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, req->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_MUTE_MEMBERS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Mute Members permission to manage stage speakers.");
        }
      }
      const ccord_voice_state *vs = ccord_store_voice_user(s->store, req->guild_id, req->user_id);
      if (!vs || vs->channel_id != req->channel_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That member is not connected to this stage.");
      }
      break;
    }
    case CCORD_ADMIN_STAGE_TOPIC:
    case CCORD_ADMIN_STAGE_DELETE: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (ch->kind != CCORD_CHANNEL_STAGE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't a stage.");
      }
      if (ccord_store_permissions_exact(s->store, ch->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, ch->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_MUTE_MEMBERS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 req->op == CCORD_ADMIN_STAGE_DELETE
                                   ? "You need Mute Members permission to end the stage."
                                   : "You need Mute Members permission to change the stage topic.");
        }
      }
      break;
    }
    case CCORD_ADMIN_STAGE_CREATE: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_STAGE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "That channel isn't a stage in this server.");
      }
      if (ccord_store_stage_instance(s->store, req->channel_id) != nullptr) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "This stage already has an active stage instance.");
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, req->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_MUTE_MEMBERS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Mute Members permission to start a stage.");
        }
      }
      break;
    }
    case CCORD_ADMIN_ACTIVITY_SHELF:
      break;   // the guild row was validated above; the answer is cached per guild
    case CCORD_ADMIN_ACTIVITY_LAUNCH: {
      const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
      if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
      if (ch->kind != CCORD_CHANNEL_VOICE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Activities run in voice channels.");
      }
      const ccord_voice_state *vs = sess_self_voice(s, ch->guild_id);
      if (!vs || vs->channel_id != req->channel_id) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "Join the voice channel before starting an activity.");
      }
      if (ccord_store_permissions_exact(s->store, ch->guild_id, self)) {
        uint64_t ch_perms = ccord_store_permissions(s->store, ch->guild_id, req->channel_id, self, session_now_unix_ms(s));
        if ((ch_perms & CCORD_PERM_USE_EMBEDDED_ACTIVITIES) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Use Embedded Activities permission in this channel.");
        }
      }
      break;
    }
    case CCORD_ADMIN_ACTIVITY_LEAVE: {
      // Only a running instance we know can be left (no voice-connection gate: leaving after a disconnect is the
      // point). The REST location segment is the instance's own location, so it is not compared to a channel here.
      const ccord_activity_instance *inst = ccord_store_find_activity_instance(s->store, req->instance_id);
      if (!inst || inst->application_id != req->application_id) {
        return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That activity isn't running.");
      }
      break;
    }
    case CCORD_ADMIN_EVENT_CREATE: {
      if (req->event.entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->event.channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That stage channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_STAGE) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Stage events require a stage channel in this server.");
        }
      } else if (req->event.entity_type == CCORD_SCHEDULED_EVENT_VOICE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->event.channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That voice channel isn't available.");
        if (ch->guild_id != req->guild_id || ch->kind != CCORD_CHANNEL_VOICE) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "Voice events require a voice channel in this server.");
        }
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        ccord_id perm_ch = (req->event.entity_type == CCORD_SCHEDULED_EVENT_EXTERNAL)
                             ? CCORD_ID_NONE
                             : req->event.channel_id;
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, perm_ch, self, session_now_unix_ms(s));
        if ((perms & (CCORD_PERM_CREATE_EVENTS | CCORD_PERM_MANAGE_EVENTS)) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Create Events or Manage Events permission.");
        }
      }
      break;
    }
    case CCORD_ADMIN_EVENT_MODIFY:
    case CCORD_ADMIN_EVENT_DELETE: {
      if (req->op == CCORD_ADMIN_EVENT_MODIFY && (req->event_has & CCORD_ADMIN_EVENT_HAS_CHANNEL) != 0u &&
          req->event.channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, req->event.channel_id);
        if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That channel isn't available.");
        if (ch->guild_id != req->guild_id ||
            (ch->kind != CCORD_CHANNEL_VOICE && ch->kind != CCORD_CHANNEL_STAGE)) {
          return ccord_error_set(err, CCORD_ERR_INVALID,
                                 "Scheduled events require a voice or stage channel in this server.");
        }
      }
      if (ccord_store_permissions_exact(s->store, req->guild_id, self)) {
        const ccord_scheduled_event *ev = ccord_store_find_scheduled_event(s->store, req->event_id);
        ccord_id perm_ch = (ev && ev->guild_id == req->guild_id) ? ev->channel_id : CCORD_ID_NONE;
        uint64_t perms = ccord_store_permissions(s->store, req->guild_id, perm_ch, self, session_now_unix_ms(s));
        if ((perms & (CCORD_PERM_CREATE_EVENTS | CCORD_PERM_MANAGE_EVENTS)) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Create Events or Manage Events permission.");
        }
        if (ev && ev->guild_id == req->guild_id && ev->creator_id != CCORD_ID_NONE && ev->creator_id != self &&
            (perms & CCORD_PERM_MANAGE_EVENTS) == 0u) {
          return ccord_error_set(err, CCORD_ERR_PERMISSION,
                                 "You need Manage Events permission to modify events created by others.");
        }
      }
      break;
    }
    case CCORD_ADMIN_EVENT_LIST:
    case CCORD_ADMIN_EVENT_RSVP_ADD:
    case CCORD_ADMIN_EVENT_RSVP_REMOVE:
      break;
    case CCORD_ADMIN_CREATE_CHANNEL:
    case CCORD_ADMIN_SET_INCIDENT_ACTIONS:
    case CCORD_ADMIN_SET_VANITY_URL:
    case CCORD_ADMIN_UNBAN:
    case CCORD_ADMIN_CREATE_INVITE:
    case CCORD_ADMIN_REVOKE_INVITE:
    case CCORD_ADMIN_LIST_BANS:
    case CCORD_ADMIN_LIST_INVITES:
    case CCORD_ADMIN_LIST_AUDIT_LOG:
    case CCORD_ADMIN_SET_GUILD_FOLDERS:
    case CCORD_ADMIN_CREATE_GUILD:
    case CCORD_ADMIN_PREVIEW_INVITE:
    case CCORD_ADMIN_ACCEPT_INVITE:
    case CCORD_ADMIN_SET_CUSTOM_STATUS:
    case CCORD_ADMIN_GET_MEMBER_VERIFICATION:
    case CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION:
    case CCORD_ADMIN_NONE:
    case CCORD_ADMIN_OP_COUNT: break;
    case CCORD_ADMIN_GET_GUILD_PROFILE:
      if (req->guild_id == CCORD_ID_NONE || req->guild_id == CCORD_DM_GUILD_ID) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid guild id");
      }
      break;
    default: break;
  }
  if (req->op == CCORD_ADMIN_PREVIEW_INVITE) {
    if (req->code[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad invite preview request");
    }
    uint64_t t = now_ms ? now_ms : (ccord_clock_ms() ? ccord_clock_ms() : 1u);
    session_invite_cache *entry = sess_find_invite_cache(s, req->code);
    if (entry) {
      if (entry->fetched_ms != 0 && t >= entry->fetched_ms && t < entry->fetched_ms + SESSION_INVITE_TTL_MS) {
        s->invite_preview = entry->preview;
        s->has_invite_preview = true;
        s->invite_preview_loading = false;
        return CCORD_OK;
      }
      if (entry->sent_ms != 0 && t < entry->sent_ms + SESSION_INVITE_WAIT_MS) return CCORD_OK;
    }
    ccord_status preview_st = ccord_transport_admin_action(s->transport, req, err);
    if (preview_st == CCORD_OK) {
      entry = sess_invite_cache_for(s, req->code);
      if (entry) {
        entry->sent_ms = t;
        entry->failed = false;
      }
      s->has_invite_preview = false;
      s->invite_preview_loading = true;
    }
    return preview_st;
  }

  ccord_status st = ccord_transport_admin_action(s->transport, req, err);
  if (st == CCORD_OK && req->op == CCORD_ADMIN_REVOKE_INVITE) {
    invalidate_invite_cache(s, req->code);
  } else if (st == CCORD_OK && req->op == CCORD_ADMIN_ACCEPT_INVITE) {
    // Expire TTL so an explicit PREVIEW_INVITE refreshes counts, while keeping the preview struct for inline cards.
    session_invite_cache *entry = sess_find_invite_cache(s, req->code);
    if (entry) entry->fetched_ms = 0;
  }
  return st;
}

ccord_status ccord_session_admin(ccord_session *s, const ccord_admin_request *req, ccord_error *err)
{
  return ccord_session_admin_ex(s, req, 0, err);
}


ccord_status ccord_session_open_thread(ccord_session *s, ccord_id guild_id, ccord_id parent_id,
                                       ccord_id thread_id, const char *name, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (thread_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: thread id");

  if (!ccord_store_channel(s->store, thread_id)) {
    const ccord_channel *parent = parent_id != CCORD_ID_NONE ? ccord_store_channel(s->store, parent_id) : nullptr;
    if (guild_id == CCORD_ID_NONE && parent) guild_id = parent->guild_id;
    if (guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown thread guild");
    ccord_channel synth = {
      .id = thread_id,
      .guild_id = guild_id,
      .parent_id = parent ? parent->id : CCORD_ID_NONE,
      .kind = CCORD_CHANNEL_TEXT,
      .name = (name && name[0] != '\0') ? name : "Thread",
      .topic = "",
      .icon_url = "",
      .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD,
    };
    ccord_status ust = ccord_store_upsert_channel(s->store, &synth, err);
    if (ust != CCORD_OK) return ust;
    ccord_error ignored;
    (void)ccord_transport_request_channel(s->transport, thread_id, &ignored);
  }
  return ccord_session_select_channel(s, thread_id, err);
}


const ccord_invite_preview *ccord_session_invite_preview(const ccord_session *s, bool *out_loading)
{
  if (out_loading) *out_loading = s ? s->invite_preview_loading : false;
  return (s && s->has_invite_preview) ? &s->invite_preview : nullptr;
}

void ccord_session_clear_invite_preview(ccord_session *s)
{
  if (!s) return;
  s->has_invite_preview = false;
  s->invite_preview_loading = false;
}

ccord_status ccord_session_invite_fetch(ccord_session *s, const char *code_or_url, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s || !code_or_url || !code_or_url[0]) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad invite fetch args");
  }
  char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
  ccord_status st = ccord_admin_parse_invite_code(code_or_url, code, err);
  if (st != CCORD_OK) return st;
  uint64_t t = ccord_clock_ms();
  if (t == 0) t = 1u;
  session_invite_cache *entry = sess_find_invite_cache(s, code);
  if (entry) {
    if (entry->fetched_ms != 0 && t >= entry->fetched_ms && t < entry->fetched_ms + SESSION_INVITE_TTL_MS) {
      return CCORD_OK;
    }
    if (entry->failed || (entry->sent_ms != 0 && t < entry->sent_ms + SESSION_INVITE_WAIT_MS)) {
      return CCORD_OK;
    }
  }
  ccord_admin_request req;
  if ((st = ccord_admin_preview_invite(&req, code, err)) != CCORD_OK) return st;
  st = ccord_transport_admin_action(s->transport, &req, err);
  if (st == CCORD_OK) {
    entry = sess_invite_cache_for(s, code);
    if (entry) {
      entry->sent_ms = t;
      entry->failed = false;
    }
  }
  return st;
}

const ccord_invite_preview *ccord_session_invite_lookup(const ccord_session *s, const char *code_or_url,
                                                        bool *out_loading, bool *out_failed)
{
  if (out_loading) *out_loading = false;
  if (out_failed) *out_failed = false;
  if (!s || !s->invite_cache || !code_or_url || !code_or_url[0]) return nullptr;
  char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
  ccord_error err;
  if (ccord_admin_parse_invite_code(code_or_url, code, &err) != CCORD_OK) return nullptr;
  for (size_t i = 0; i < SESSION_INVITE_CACHE_SLOTS; i++) {
    const session_invite_cache *e = &s->invite_cache[i];
    if (!e->valid || strcmp(e->code, code) != 0) continue;
    if (e->preview.guild_id != CCORD_ID_NONE) return &e->preview;
    if (out_loading) *out_loading = e->sent_ms != 0;
    if (out_failed) *out_failed = e->failed;
    return nullptr;
  }
  return nullptr;
}


