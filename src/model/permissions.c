// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/model/permissions.h>

#include <string.h>

#define PERM_SEND_DEPENDENTS (CCORD_PERM_SEND_TTS_MESSAGES | CCORD_PERM_MENTION_EVERYONE | CCORD_PERM_EMBED_LINKS | CCORD_PERM_ATTACH_FILES)

static const ccord_role *find_role(const ccord_role *roles, size_t nroles, ccord_id id)
{
  for (size_t i = 0; i < nroles; i++) {
    if (roles[i].id == id) return &roles[i];
  }
  return nullptr;
}

static bool holds_role(const ccord_id *member_roles, size_t n, ccord_id id)
{
  for (size_t i = 0; i < n; i++) {
    if (member_roles[i] == id) return true;
  }
  return false;
}

uint64_t ccord_perm_guild(ccord_id guild_id, ccord_id owner_id, ccord_id user_id,
                          const ccord_role *roles, size_t nroles,
                          const ccord_id *member_roles, size_t nmember_roles)
{
  if (owner_id != CCORD_ID_NONE && owner_id == user_id) return CCORD_PERM_ALL;
  uint64_t perms = 0;
  const ccord_role *everyone = roles ? find_role(roles, nroles, guild_id) : nullptr;
  if (everyone) perms |= everyone->permissions;
  if (roles && member_roles) {
    for (size_t i = 0; i < nmember_roles; i++) {
      const ccord_role *r = find_role(roles, nroles, member_roles[i]);
      if (r) perms |= r->permissions;
    }
  }
  return (perms & CCORD_PERM_ADMINISTRATOR) != 0 ? CCORD_PERM_ALL : perms;
}

uint64_t ccord_perm_channel_ex(uint64_t guild_perms, ccord_id guild_id, ccord_id user_id,
                              const ccord_id *member_roles, size_t role_count,
                              const ccord_overwrite *overwrites, size_t overwrite_count,
                              bool in_thread)
{
  if ((guild_perms & CCORD_PERM_ADMINISTRATOR) != 0) return CCORD_PERM_ALL;
  uint64_t perms = guild_perms;
  if (overwrites) {
    // 1. @everyone
    for (size_t i = 0; i < overwrite_count; i++) {
      if (overwrites[i].kind == CCORD_OVERWRITE_ROLE && overwrites[i].id == guild_id) {
        perms &= ~overwrites[i].deny;
        perms |= overwrites[i].allow;
      }
    }
    // 2. the member's roles, combined: every deny first, then every allow
    uint64_t allow = 0, deny = 0;
    for (size_t i = 0; i < overwrite_count; i++) {
      if (overwrites[i].kind == CCORD_OVERWRITE_ROLE && overwrites[i].id != guild_id &&
          member_roles && holds_role(member_roles, role_count, overwrites[i].id)) {
        allow |= overwrites[i].allow;
        deny |= overwrites[i].deny;
      }
    }
    perms &= ~deny;
    perms |= allow;
    // 3. the member
    for (size_t i = 0; i < overwrite_count; i++) {
      if (overwrites[i].kind == CCORD_OVERWRITE_MEMBER && overwrites[i].id == user_id) {
        perms &= ~overwrites[i].deny;
        perms |= overwrites[i].allow;
      }
    }
  }
  if ((perms & CCORD_PERM_VIEW_CHANNEL) == 0) return 0;
  uint64_t send_check = in_thread ? CCORD_PERM_SEND_MESSAGES_IN_THREADS : CCORD_PERM_SEND_MESSAGES;
  if ((perms & send_check) == 0) perms &= ~(uint64_t)PERM_SEND_DEPENDENTS;
  return perms;
}

uint64_t ccord_perm_channel(uint64_t base, ccord_id guild_id, ccord_id user_id,
                            const ccord_overwrite *overwrites, size_t noverwrites,
                            const ccord_id *member_roles, size_t nmember_roles)
{
  return ccord_perm_channel_ex(base, guild_id, user_id, member_roles, nmember_roles, overwrites, noverwrites, false);
}

uint64_t ccord_perm_apply_timeout(uint64_t perms, bool timed_out)
{
  if (!timed_out || (perms & CCORD_PERM_ADMINISTRATOR) != 0) return perms;
  return perms & (CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_READ_MESSAGE_HISTORY);
}

// ---- role hierarchy ----

bool ccord_perm_outranks(const ccord_role *a, const ccord_role *b)
{
  if (!a) return false;
  if (!b) return true;
  if (a->id == b->id) return false;
  if (a->position != b->position) return a->position > b->position;
  return a->id < b->id;
}

const ccord_role *ccord_perm_top_role(ccord_id guild_id, const ccord_role *roles, size_t nroles,
                                      const ccord_id *member_roles, size_t nmember_roles)
{
  if (!roles) return nullptr;
  const ccord_role *top = nullptr;
  if (member_roles) {
    for (size_t i = 0; i < nmember_roles; i++) {
      const ccord_role *r = find_role(roles, nroles, member_roles[i]);
      if (r && ccord_perm_outranks(r, top)) top = r;
    }
  }
  return top ? top : find_role(roles, nroles, guild_id);
}

ccord_perm_verdict ccord_perm_can_act_on(ccord_perm_action action, ccord_id guild_id, ccord_id owner_id,
                                          ccord_id actor_id, uint64_t actor_perms,
                                          const ccord_id *actor_roles, size_t nactor_roles,
                                          ccord_id target_id, uint64_t target_perms,
                                          const ccord_id *target_roles, size_t ntarget_roles,
                                          const ccord_role *roles, size_t nroles)
{
  uint64_t need = 0;
  switch (action) {
    case CCORD_PERM_ACT_KICK: need = CCORD_PERM_KICK_MEMBERS; break;
    case CCORD_PERM_ACT_BAN: need = CCORD_PERM_BAN_MEMBERS; break;
    case CCORD_PERM_ACT_TIMEOUT: need = CCORD_PERM_MODERATE_MEMBERS; break;
    case CCORD_PERM_ACT_SET_NICKNAME: need = CCORD_PERM_MANAGE_NICKNAMES; break;
    case CCORD_PERM_ACT_EDIT_ROLES: need = CCORD_PERM_MANAGE_ROLES; break;
  }
  if (actor_id != CCORD_ID_NONE && actor_id == target_id) {
    // Against oneself the hierarchy does not apply: you cannot remove or silence yourself, but you can rename
    // yourself (CHANGE_NICKNAME is enough) and drop roles you may manage.
    switch (action) {
      case CCORD_PERM_ACT_KICK:
      case CCORD_PERM_ACT_BAN:
      case CCORD_PERM_ACT_TIMEOUT:
        return CCORD_PERM_TARGET_SELF;
      case CCORD_PERM_ACT_SET_NICKNAME:
        return ccord_perm_has(actor_perms, CCORD_PERM_CHANGE_NICKNAME) || ccord_perm_has(actor_perms, need)
                   ? CCORD_PERM_ALLOWED : CCORD_PERM_MISSING;
      case CCORD_PERM_ACT_EDIT_ROLES:
        return ccord_perm_has(actor_perms, need) ? CCORD_PERM_ALLOWED : CCORD_PERM_MISSING;
    }
  }
  if (!ccord_perm_has(actor_perms, need)) return CCORD_PERM_MISSING;
  if (owner_id != CCORD_ID_NONE && target_id == owner_id) return CCORD_PERM_TARGET_OWNER;
  if (action == CCORD_PERM_ACT_TIMEOUT && (target_perms & CCORD_PERM_ADMINISTRATOR) != 0) return CCORD_PERM_TARGET_ADMIN;
  if (owner_id != CCORD_ID_NONE && actor_id == owner_id) return CCORD_PERM_ALLOWED;
  const ccord_role *atop = ccord_perm_top_role(guild_id, roles, nroles, actor_roles, nactor_roles);
  const ccord_role *ttop = ccord_perm_top_role(guild_id, roles, nroles, target_roles, ntarget_roles);
  return ccord_perm_outranks(atop, ttop) ? CCORD_PERM_ALLOWED : CCORD_PERM_TARGET_HIGHER;
}

ccord_perm_verdict ccord_perm_can_edit_role(ccord_id guild_id, ccord_id owner_id, ccord_id actor_id,
                                             uint64_t actor_perms,
                                             const ccord_id *actor_roles, size_t nactor_roles,
                                             const ccord_role *role, bool assign,
                                             const ccord_role *roles, size_t nroles)
{
  if (!ccord_perm_has(actor_perms, CCORD_PERM_MANAGE_ROLES)) return CCORD_PERM_MISSING;
  if (!role) return CCORD_PERM_MISSING;
  if (assign && role->managed) return CCORD_PERM_ROLE_MANAGED;
  if (role->id == guild_id) return CCORD_PERM_ALLOWED;
  if (owner_id != CCORD_ID_NONE && actor_id == owner_id) return CCORD_PERM_ALLOWED;
  const ccord_role *atop = ccord_perm_top_role(guild_id, roles, nroles, actor_roles, nactor_roles);
  return ccord_perm_outranks(atop, role) ? CCORD_PERM_ALLOWED : CCORD_PERM_TARGET_HIGHER;
}

ccord_perm_verdict ccord_perm_can_grant(ccord_id guild_id, ccord_id owner_id, ccord_id actor_id,
                                         uint64_t actor_perms, uint64_t old_perms, uint64_t new_perms)
{
  (void)guild_id;
  if (owner_id != CCORD_ID_NONE && actor_id == owner_id) return CCORD_PERM_ALLOWED;
  if ((actor_perms & CCORD_PERM_ADMINISTRATOR) != 0) return CCORD_PERM_ALLOWED;
  uint64_t added = new_perms & ~old_perms;
  return (added & ~actor_perms) != 0 ? CCORD_PERM_ESCALATION : CCORD_PERM_ALLOWED;
}

// ---- display ----

#define PI(bit, label, desc, group) {bit, label, desc, group}
static const ccord_perm_info k_table[] = {
  // General server permissions
  PI(CCORD_PERM_VIEW_CHANNEL, "View Channels", "Members can view channels by default (except private channels).", CCORD_PERM_GROUP_GENERAL),
  PI(CCORD_PERM_MANAGE_CHANNELS, "Manage Channels", "Create, edit and delete channels and categories.", CCORD_PERM_GROUP_GENERAL),
  PI(CCORD_PERM_MANAGE_ROLES, "Manage Roles", "Create new roles and edit or delete roles lower than this one.", CCORD_PERM_GROUP_GENERAL),
  PI(CCORD_PERM_CREATE_EXPRESSIONS, "Create Expressions", "Add custom emoji, stickers and sounds in this server.", CCORD_PERM_GROUP_GENERAL),
  PI(CCORD_PERM_MANAGE_EXPRESSIONS, "Manage Expressions", "Edit or remove custom emoji, stickers and sounds.", CCORD_PERM_GROUP_GENERAL),
  PI(CCORD_PERM_VIEW_AUDIT_LOG, "View Audit Log", "See who changed what in this server.", CCORD_PERM_GROUP_GENERAL),
  PI(CCORD_PERM_VIEW_GUILD_INSIGHTS, "View Server Insights", "See Server Insights, which show growth and engagement.", CCORD_PERM_GROUP_GENERAL),
  PI(CCORD_PERM_VIEW_CREATOR_ANALYTICS, "View Creator Analytics", "See the server's creator monetization analytics.", CCORD_PERM_GROUP_GENERAL),
  PI(CCORD_PERM_MANAGE_WEBHOOKS, "Manage Webhooks", "Create, edit and delete webhooks.", CCORD_PERM_GROUP_GENERAL),
  PI(CCORD_PERM_MANAGE_GUILD, "Manage Server", "Change the server's name and settings, and add bots.", CCORD_PERM_GROUP_GENERAL),
  // Membership
  PI(CCORD_PERM_CREATE_INSTANT_INVITE, "Create Invite", "Invite new people to this server.", CCORD_PERM_GROUP_MEMBERSHIP),
  PI(CCORD_PERM_CHANGE_NICKNAME, "Change Nickname", "Change their own nickname.", CCORD_PERM_GROUP_MEMBERSHIP),
  PI(CCORD_PERM_MANAGE_NICKNAMES, "Manage Nicknames", "Change other members' nicknames.", CCORD_PERM_GROUP_MEMBERSHIP),
  PI(CCORD_PERM_KICK_MEMBERS, "Kick Members", "Remove members from the server. They can rejoin with an invite.", CCORD_PERM_GROUP_MEMBERSHIP),
  PI(CCORD_PERM_BAN_MEMBERS, "Ban Members", "Permanently ban members from the server.", CCORD_PERM_GROUP_MEMBERSHIP),
  PI(CCORD_PERM_MODERATE_MEMBERS, "Time Out Members", "Stop members from sending messages, reacting or speaking for a while.", CCORD_PERM_GROUP_MEMBERSHIP),
  // Text channels
  PI(CCORD_PERM_SEND_MESSAGES, "Send Messages", "Send messages in text channels.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_SEND_MESSAGES_IN_THREADS, "Send Messages in Threads", "Send messages in threads.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_CREATE_PUBLIC_THREADS, "Create Public Threads", "Start threads that everyone in the channel can see.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_CREATE_PRIVATE_THREADS, "Create Private Threads", "Start invite-only threads.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_EMBED_LINKS, "Embed Links", "Links shared by members show a preview.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_ATTACH_FILES, "Attach Files", "Upload files and media.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_ADD_REACTIONS, "Add Reactions", "Add new emoji reactions to messages.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_USE_EXTERNAL_EMOJIS, "Use External Emoji", "Use emoji from other servers.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_USE_EXTERNAL_STICKERS, "Use External Stickers", "Use stickers from other servers.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_MENTION_EVERYONE, "Mention Everyone", "Use @everyone and @here, and mention all roles.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_MANAGE_MESSAGES, "Manage Messages", "Delete other people's messages and remove embeds.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_PIN_MESSAGES, "Pin Messages", "Pin and unpin messages.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_BYPASS_SLOWMODE, "Bypass Slowmode", "Send messages without waiting out slowmode.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_MANAGE_THREADS, "Manage Threads", "Rename, delete, close and slow down threads.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_READ_MESSAGE_HISTORY, "Read Message History", "Read messages that were sent before they joined.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_SEND_TTS_MESSAGES, "Send Text-to-Speech Messages", "Send /tts messages that are read aloud.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_SEND_VOICE_MESSAGES, "Send Voice Messages", "Send voice messages.", CCORD_PERM_GROUP_TEXT),
  PI(CCORD_PERM_SEND_POLLS, "Create Polls", "Start polls.", CCORD_PERM_GROUP_TEXT),
  // Voice channels
  PI(CCORD_PERM_CONNECT, "Connect", "Join voice channels.", CCORD_PERM_GROUP_VOICE),
  PI(CCORD_PERM_SPEAK, "Speak", "Talk in voice channels.", CCORD_PERM_GROUP_VOICE),
  PI(CCORD_PERM_STREAM, "Video", "Share their screen or turn on their camera.", CCORD_PERM_GROUP_VOICE),
  PI(CCORD_PERM_USE_SOUNDBOARD, "Use Soundboard", "Play sounds from the soundboard.", CCORD_PERM_GROUP_VOICE),
  PI(CCORD_PERM_USE_EXTERNAL_SOUNDS, "Use External Sounds", "Play sounds from other servers.", CCORD_PERM_GROUP_VOICE),
  PI(CCORD_PERM_USE_VAD, "Use Voice Activity", "Talk without push-to-talk.", CCORD_PERM_GROUP_VOICE),
  PI(CCORD_PERM_PRIORITY_SPEAKER, "Priority Speaker", "Be heard more clearly; others are lowered while they talk.", CCORD_PERM_GROUP_VOICE),
  PI(CCORD_PERM_MUTE_MEMBERS, "Mute Members", "Mute other members in voice channels.", CCORD_PERM_GROUP_VOICE),
  PI(CCORD_PERM_DEAFEN_MEMBERS, "Deafen Members", "Deafen other members in voice channels.", CCORD_PERM_GROUP_VOICE),
  PI(CCORD_PERM_MOVE_MEMBERS, "Move Members", "Move members between voice channels.", CCORD_PERM_GROUP_VOICE),
  PI(CCORD_PERM_SET_VOICE_CHANNEL_STATUS, "Set Voice Channel Status", "Create and edit a voice channel's status.", CCORD_PERM_GROUP_VOICE),
  PI(CCORD_PERM_REQUEST_TO_SPEAK, "Request to Speak", "Ask to speak in stage channels.", CCORD_PERM_GROUP_VOICE),
  // Apps
  PI(CCORD_PERM_USE_APPLICATION_COMMANDS, "Use Application Commands", "Use slash commands and other app commands.", CCORD_PERM_GROUP_APPS),
  PI(CCORD_PERM_USE_EMBEDDED_ACTIVITIES, "Use Activities", "Start and join Activities in voice channels.", CCORD_PERM_GROUP_APPS),
  PI(CCORD_PERM_USE_EXTERNAL_APPS, "Use External Apps", "Use apps the member added to their own account.", CCORD_PERM_GROUP_APPS),
  // Events
  PI(CCORD_PERM_CREATE_EVENTS, "Create Events", "Schedule events.", CCORD_PERM_GROUP_EVENTS),
  PI(CCORD_PERM_MANAGE_EVENTS, "Manage Events", "Edit and cancel events.", CCORD_PERM_GROUP_EVENTS),
  // Advanced
  PI(CCORD_PERM_ADMINISTRATOR, "Administrator", "Every permission, and channel permissions no longer apply. Dangerous: grant with care.", CCORD_PERM_GROUP_ADVANCED),
};
#undef PI

const ccord_perm_info *ccord_perm_table(size_t *count)
{
  if (count) *count = sizeof k_table / sizeof k_table[0];
  return k_table;
}

const char *ccord_perm_group_label(ccord_perm_group g)
{
  switch (g) {
    case CCORD_PERM_GROUP_GENERAL: return "General Server Permissions";
    case CCORD_PERM_GROUP_MEMBERSHIP: return "Membership Permissions";
    case CCORD_PERM_GROUP_TEXT: return "Text Channel Permissions";
    case CCORD_PERM_GROUP_VOICE: return "Voice Channel Permissions";
    case CCORD_PERM_GROUP_APPS: return "App Permissions";
    case CCORD_PERM_GROUP_EVENTS: return "Events Permissions";
    case CCORD_PERM_GROUP_ADVANCED: return "Advanced Permissions";
    case CCORD_PERM_GROUP_COUNT: break;
  }
  return "";
}

const char *ccord_perm_label(uint64_t bit)
{
  for (size_t i = 0; i < sizeof k_table / sizeof k_table[0]; i++) {
    if (k_table[i].bit == bit) return k_table[i].label;
  }
  return nullptr;
}

const char *ccord_perm_verdict_text(ccord_perm_verdict v, ccord_perm_action action)
{
  // Static strings only: one per (verdict, action) that needs the verb.
  switch (v) {
    case CCORD_PERM_ALLOWED: return "";
    case CCORD_PERM_MISSING:
      switch (action) {
        case CCORD_PERM_ACT_KICK: return "You need the Kick Members permission.";
        case CCORD_PERM_ACT_BAN: return "You need the Ban Members permission.";
        case CCORD_PERM_ACT_TIMEOUT: return "You need the Time Out Members permission.";
        case CCORD_PERM_ACT_SET_NICKNAME: return "You need the Manage Nicknames permission.";
        case CCORD_PERM_ACT_EDIT_ROLES: return "You need the Manage Roles permission.";
      }
      return "You don't have permission to do that.";
    case CCORD_PERM_TARGET_OWNER: return "Nobody can act on the server owner.";
    case CCORD_PERM_TARGET_HIGHER:
      switch (action) {
        case CCORD_PERM_ACT_KICK: return "You can't kick someone whose highest role is not below yours.";
        case CCORD_PERM_ACT_BAN: return "You can't ban someone whose highest role is not below yours.";
        case CCORD_PERM_ACT_TIMEOUT: return "You can't time out someone whose highest role is not below yours.";
        case CCORD_PERM_ACT_SET_NICKNAME: return "You can't change the nickname of someone whose highest role is not below yours.";
        case CCORD_PERM_ACT_EDIT_ROLES: return "You can only manage roles that are below your highest role.";
      }
      return "That member's highest role is not below yours.";
    case CCORD_PERM_TARGET_SELF:
      switch (action) {
        case CCORD_PERM_ACT_KICK: return "You can't kick yourself.";
        case CCORD_PERM_ACT_BAN: return "You can't ban yourself.";
        case CCORD_PERM_ACT_TIMEOUT: return "You can't time yourself out.";
        default: break;
      }
      return "You can't do that to yourself.";
    case CCORD_PERM_TARGET_ADMIN: return "Administrators can't be timed out.";
    case CCORD_PERM_ROLE_MANAGED: return "This role belongs to a bot or integration and can't be assigned by hand.";
    case CCORD_PERM_ESCALATION: return "You can only give out permissions you have yourself.";
  }
  return "You can't do that.";
}

uint64_t ccord_perm_known_mask(void)
{
  size_t n = 0;
  const ccord_perm_info *t = ccord_perm_table(&n);
  uint64_t m = 0;
  for (size_t i = 0; i < n; i++) m |= t[i].bit;
  return m;
}

bool ccord_perm_can_create_expression(uint64_t actor_perms)
{
  return ccord_perm_has(actor_perms, CCORD_PERM_MANAGE_EXPRESSIONS) ||
         ccord_perm_has(actor_perms, CCORD_PERM_CREATE_EXPRESSIONS);
}

bool ccord_perm_can_modify_expression(uint64_t actor_perms, ccord_id actor_id, ccord_id creator_id)
{
  if (ccord_perm_has(actor_perms, CCORD_PERM_MANAGE_EXPRESSIONS)) return true;
  if (ccord_perm_has(actor_perms, CCORD_PERM_CREATE_EXPRESSIONS)) {
    return actor_id != CCORD_ID_NONE && creator_id == actor_id;
  }
  return false;
}
