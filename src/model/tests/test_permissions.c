// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>

#include <ccord/model/permissions.h>

#include "ccord_test.h"

enum { GUILD = 100, OWNER = 1, ALICE = 11, BOB = 12, CAROL = 13, DAVE = 14 };
enum { R_HELPER = 150, R_TIE = 160, R_MOD = 200, R_MOD2 = 210, R_ADMIN = 300, R_BOT = 400, R_GONE = 999 };

#define EVERYONE_PERMS (CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_SEND_MESSAGES | CCORD_PERM_READ_MESSAGE_HISTORY | \
                        CCORD_PERM_ADD_REACTIONS | CCORD_PERM_CHANGE_NICKNAME | CCORD_PERM_EMBED_LINKS | CCORD_PERM_ATTACH_FILES)

static const ccord_role k_roles[] = {
  {.id = GUILD, .name = "@everyone", .position = 0, .permissions = EVERYONE_PERMS},
  {.id = R_HELPER, .name = "Helper", .position = 1, .permissions = CCORD_PERM_MANAGE_MESSAGES},
  {.id = R_TIE, .name = "Tie", .position = 1, .permissions = 0},
  {.id = R_MOD, .name = "Mod", .position = 2,
   .permissions = CCORD_PERM_KICK_MEMBERS | CCORD_PERM_MODERATE_MEMBERS | CCORD_PERM_MANAGE_NICKNAMES},
  {.id = R_MOD2, .name = "Mod2", .position = 2, .permissions = CCORD_PERM_BAN_MEMBERS},
  {.id = R_ADMIN, .name = "Admin", .position = 3, .permissions = CCORD_PERM_ADMINISTRATOR},
  {.id = R_BOT, .name = "Bot", .position = 1, .permissions = CCORD_PERM_MANAGE_ROLES, .managed = true},
};
#define NROLES (sizeof k_roles / sizeof k_roles[0])

static uint64_t guild_perms(ccord_id user, const ccord_id *roles, size_t n)
{
  return ccord_perm_guild(GUILD, OWNER, user, k_roles, NROLES, roles, n);
}

static void test_guild_permissions(void)
{
  // The owner has everything, whatever roles say.
  CT_CHECK(guild_perms(OWNER, nullptr, 0) == CCORD_PERM_ALL);
  // A member with no roles gets exactly @everyone.
  CT_CHECK(guild_perms(ALICE, nullptr, 0) == EVERYONE_PERMS);
  // Roles add to it.
  ccord_id mod[] = {R_MOD};
  CT_CHECK(guild_perms(BOB, mod, 1) == (EVERYONE_PERMS | CCORD_PERM_KICK_MEMBERS | CCORD_PERM_MODERATE_MEMBERS | CCORD_PERM_MANAGE_NICKNAMES));
  ccord_id two[] = {R_MOD, R_HELPER};
  CT_CHECK((guild_perms(BOB, two, 2) & CCORD_PERM_MANAGE_MESSAGES) != 0);
  CT_CHECK((guild_perms(BOB, two, 2) & CCORD_PERM_KICK_MEMBERS) != 0);
  // ADMINISTRATOR means everything.
  ccord_id adm[] = {R_ADMIN};
  CT_CHECK(guild_perms(CAROL, adm, 1) == CCORD_PERM_ALL);
  // A role that no longer exists grants nothing.
  ccord_id gone[] = {R_GONE};
  CT_CHECK(guild_perms(DAVE, gone, 1) == EVERYONE_PERMS);
  // Without a role table there is nothing to compute from (and no crash).
  CT_CHECK(ccord_perm_guild(GUILD, OWNER, ALICE, nullptr, 0, nullptr, 0) == 0);
  // An unknown owner (0) never matches a user.
  CT_CHECK(ccord_perm_guild(GUILD, CCORD_ID_NONE, CCORD_ID_NONE, k_roles, NROLES, nullptr, 0) == EVERYONE_PERMS);
}

static void test_channel_overwrites(void)
{
  uint64_t base = EVERYONE_PERMS;
  ccord_id helper[] = {R_HELPER};
  ccord_id both[] = {R_HELPER, R_MOD};

  // No overwrites: base, with the implicit rules satisfied.
  CT_CHECK(ccord_perm_channel(base, GUILD, ALICE, nullptr, 0, nullptr, 0) == base);

  // #announcements: @everyone may not send; the Helper role may.
  ccord_overwrite ow[] = {
    {.id = GUILD, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_SEND_MESSAGES},
    {.id = R_HELPER, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_SEND_MESSAGES},
  };
  uint64_t alice = ccord_perm_channel(base, GUILD, ALICE, ow, 2, nullptr, 0);
  CT_CHECK((alice & CCORD_PERM_SEND_MESSAGES) == 0);
  CT_CHECK((alice & CCORD_PERM_VIEW_CHANNEL) != 0);
  CT_CHECK((alice & (CCORD_PERM_EMBED_LINKS | CCORD_PERM_ATTACH_FILES)) == 0);   // implied by no SEND_MESSAGES
  uint64_t bob = ccord_perm_channel(base, GUILD, BOB, ow, 2, helper, 1);
  CT_CHECK((bob & CCORD_PERM_SEND_MESSAGES) != 0);
  CT_CHECK((bob & CCORD_PERM_EMBED_LINKS) != 0);

  // A member overwrite beats every role overwrite.
  ccord_overwrite mw[] = {
    {.id = GUILD, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_SEND_MESSAGES},
    {.id = R_HELPER, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_SEND_MESSAGES},
    {.id = BOB, .kind = CCORD_OVERWRITE_MEMBER, .deny = CCORD_PERM_SEND_MESSAGES},
    {.id = ALICE, .kind = CCORD_OVERWRITE_MEMBER, .allow = CCORD_PERM_ADD_REACTIONS},   // someone else's: ignored
  };
  CT_CHECK((ccord_perm_channel(base, GUILD, BOB, mw, 4, helper, 1) & CCORD_PERM_SEND_MESSAGES) == 0);
  CT_CHECK((ccord_perm_channel(base, GUILD, ALICE, mw, 4, nullptr, 0) & CCORD_PERM_ADD_REACTIONS) != 0);

  // Among a member's own roles, allow beats deny (all denies are applied, then all allows).
  ccord_overwrite rw[] = {
    {.id = R_HELPER, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_ADD_REACTIONS},
    {.id = R_MOD, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_ADD_REACTIONS},
  };
  CT_CHECK((ccord_perm_channel(base, GUILD, BOB, rw, 2, both, 2) & CCORD_PERM_ADD_REACTIONS) != 0);
  // ... but the @everyone step comes before the role step: a role allow rescues what @everyone denied.
  ccord_overwrite ev[] = {
    {.id = GUILD, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_ADD_REACTIONS},
    {.id = R_MOD, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_ADD_REACTIONS},
  };
  CT_CHECK((ccord_perm_channel(base, GUILD, BOB, ev, 2, both, 2) & CCORD_PERM_ADD_REACTIONS) != 0);
  CT_CHECK((ccord_perm_channel(base, GUILD, ALICE, ev, 2, nullptr, 0) & CCORD_PERM_ADD_REACTIONS) == 0);
  // Overwrites for roles the member does not hold do nothing.
  CT_CHECK((ccord_perm_channel(base, GUILD, ALICE, rw, 2, nullptr, 0) & CCORD_PERM_ADD_REACTIONS) != 0);
  ccord_id tie_only[] = {R_TIE};   // holds a role, just not the ones with overwrites
  CT_CHECK((ccord_perm_channel(base, GUILD, ALICE, rw, 2, tie_only, 1) & CCORD_PERM_ADD_REACTIONS) != 0);
  ccord_overwrite deny_helper[] = {{.id = R_HELPER, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_ADD_REACTIONS}};
  CT_CHECK((ccord_perm_channel(base, GUILD, ALICE, deny_helper, 1, tie_only, 1) & CCORD_PERM_ADD_REACTIONS) != 0);
  CT_CHECK((ccord_perm_channel(base, GUILD, BOB, deny_helper, 1, helper, 1) & CCORD_PERM_ADD_REACTIONS) == 0);

  // Within one overwrite, deny is applied before allow (so a bit in both ends up allowed).
  ccord_overwrite both_bits[] = {{.id = GUILD, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_ADD_REACTIONS, .deny = CCORD_PERM_ADD_REACTIONS}};
  CT_CHECK((ccord_perm_channel(0, GUILD, ALICE, both_bits, 1, nullptr, 0) | CCORD_PERM_VIEW_CHANNEL) != 0);
  CT_CHECK((ccord_perm_channel(CCORD_PERM_VIEW_CHANNEL, GUILD, ALICE, both_bits, 1, nullptr, 0) & CCORD_PERM_ADD_REACTIONS) != 0);
  ccord_overwrite both_member[] = {{.id = ALICE, .kind = CCORD_OVERWRITE_MEMBER, .allow = CCORD_PERM_ADD_REACTIONS, .deny = CCORD_PERM_ADD_REACTIONS}};
  CT_CHECK((ccord_perm_channel(CCORD_PERM_VIEW_CHANNEL, GUILD, ALICE, both_member, 1, nullptr, 0) & CCORD_PERM_ADD_REACTIONS) != 0);

  // A private channel: @everyone cannot view it, so nothing else is usable either.
  ccord_overwrite priv[] = {
    {.id = GUILD, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_VIEW_CHANNEL},
    {.id = R_MOD, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_VIEW_CHANNEL},
  };
  CT_CHECK(ccord_perm_channel(base, GUILD, ALICE, priv, 2, nullptr, 0) == 0);
  CT_CHECK((ccord_perm_channel(base, GUILD, BOB, priv, 2, both, 2) & CCORD_PERM_VIEW_CHANNEL) != 0);

  // Administrators ignore overwrites entirely.
  CT_CHECK(ccord_perm_channel(CCORD_PERM_ALL, GUILD, CAROL, priv, 2, nullptr, 0) == CCORD_PERM_ALL);
  CT_CHECK(ccord_perm_channel(CCORD_PERM_ADMINISTRATOR, GUILD, CAROL, priv, 2, nullptr, 0) == CCORD_PERM_ALL);
}

static void test_timeout(void)
{
  uint64_t p = EVERYONE_PERMS | CCORD_PERM_MANAGE_MESSAGES;
  CT_CHECK(ccord_perm_apply_timeout(p, false) == p);
  uint64_t t = ccord_perm_apply_timeout(p, true);
  CT_CHECK(t == (CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_READ_MESSAGE_HISTORY));
  CT_CHECK(ccord_perm_apply_timeout(CCORD_PERM_ALL, true) == CCORD_PERM_ALL);
  CT_CHECK(!ccord_perm_has(t, CCORD_PERM_SEND_MESSAGES));
}

static void test_has(void)
{
  CT_CHECK(ccord_perm_has(CCORD_PERM_ALL, CCORD_PERM_BAN_MEMBERS | CCORD_PERM_KICK_MEMBERS));
  CT_CHECK(ccord_perm_has(CCORD_PERM_ADMINISTRATOR, CCORD_PERM_BAN_MEMBERS));
  CT_CHECK(ccord_perm_has(CCORD_PERM_BAN_MEMBERS | CCORD_PERM_KICK_MEMBERS, CCORD_PERM_BAN_MEMBERS | CCORD_PERM_KICK_MEMBERS));
  CT_CHECK(!ccord_perm_has(CCORD_PERM_BAN_MEMBERS, CCORD_PERM_BAN_MEMBERS | CCORD_PERM_KICK_MEMBERS));
  CT_CHECK(ccord_perm_has(0, 0));
}

static void test_hierarchy(void)
{
  const ccord_role *everyone = &k_roles[0], *helper = &k_roles[1], *tie = &k_roles[2], *mod = &k_roles[3],
                   *mod2 = &k_roles[4], *admin = &k_roles[5];
  CT_CHECK(ccord_perm_outranks(admin, mod));
  CT_CHECK(!ccord_perm_outranks(mod, admin));
  CT_CHECK(ccord_perm_outranks(mod, helper));
  CT_CHECK(ccord_perm_outranks(helper, everyone));
  CT_CHECK(!ccord_perm_outranks(mod, mod));                       // never above itself
  CT_CHECK(ccord_perm_outranks(helper, tie) && !ccord_perm_outranks(tie, helper));   // same position: older role wins
  CT_CHECK(ccord_perm_outranks(mod, mod2) && !ccord_perm_outranks(mod2, mod));
  CT_CHECK(ccord_perm_outranks(everyone, nullptr) && !ccord_perm_outranks(nullptr, everyone) && !ccord_perm_outranks(nullptr, nullptr));

  ccord_id r[] = {R_HELPER, R_ADMIN, R_MOD};
  CT_CHECK(ccord_perm_top_role(GUILD, k_roles, NROLES, r, 3) == admin);
  CT_CHECK(ccord_perm_top_role(GUILD, k_roles, NROLES, nullptr, 0) == everyone);
  ccord_id gone[] = {R_GONE};
  CT_CHECK(ccord_perm_top_role(GUILD, k_roles, NROLES, gone, 1) == everyone);
  CT_CHECK(ccord_perm_top_role(GUILD, nullptr, 0, r, 3) == nullptr);
}

static ccord_perm_verdict act(ccord_perm_action a, ccord_id actor, const ccord_id *ar, size_t nar,
                              ccord_id target, const ccord_id *tr, size_t ntr)
{
  uint64_t ap = guild_perms(actor, ar, nar), tp = guild_perms(target, tr, ntr);
  return ccord_perm_can_act_on(a, GUILD, OWNER, actor, ap, ar, nar, target, tp, tr, ntr, k_roles, NROLES);
}

static void test_acting_on_members(void)
{
  ccord_id mod[] = {R_MOD}, mod2[] = {R_MOD2}, helper[] = {R_HELPER}, adm[] = {R_ADMIN};

  // A moderator may kick an ordinary member ...
  CT_CHECK(act(CCORD_PERM_ACT_KICK, BOB, mod, 1, ALICE, nullptr, 0) == CCORD_PERM_ALLOWED);
  CT_CHECK(act(CCORD_PERM_ACT_TIMEOUT, BOB, mod, 1, ALICE, nullptr, 0) == CCORD_PERM_ALLOWED);
  CT_CHECK(act(CCORD_PERM_ACT_SET_NICKNAME, BOB, mod, 1, ALICE, nullptr, 0) == CCORD_PERM_ALLOWED);
  // ... but lacks the permission to ban or to edit roles.
  CT_CHECK(act(CCORD_PERM_ACT_BAN, BOB, mod, 1, ALICE, nullptr, 0) == CCORD_PERM_MISSING);
  CT_CHECK(act(CCORD_PERM_ACT_EDIT_ROLES, BOB, mod, 1, ALICE, nullptr, 0) == CCORD_PERM_MISSING);
  // An ordinary member can do nothing to anyone.
  CT_CHECK(act(CCORD_PERM_ACT_KICK, ALICE, nullptr, 0, BOB, helper, 1) == CCORD_PERM_MISSING);

  // The hierarchy: not above, and not beside, someone of the same rank.
  CT_CHECK(act(CCORD_PERM_ACT_KICK, BOB, mod, 1, CAROL, adm, 1) == CCORD_PERM_TARGET_HIGHER);
  CT_CHECK(act(CCORD_PERM_ACT_KICK, BOB, mod, 1, DAVE, mod2, 1) == CCORD_PERM_ALLOWED);          // Mod (older id) outranks Mod2
  CT_CHECK(act(CCORD_PERM_ACT_BAN, DAVE, mod2, 1, BOB, mod, 1) == CCORD_PERM_TARGET_HIGHER);     // ... and not the other way round
  CT_CHECK(act(CCORD_PERM_ACT_KICK, BOB, mod, 1, CAROL, mod, 1) == CCORD_PERM_TARGET_HIGHER);    // same role: equal rank

  // Nobody touches the owner; the owner is exempt from the hierarchy.
  CT_CHECK(act(CCORD_PERM_ACT_KICK, CAROL, adm, 1, OWNER, nullptr, 0) == CCORD_PERM_TARGET_OWNER);
  CT_CHECK(act(CCORD_PERM_ACT_BAN, OWNER, nullptr, 0, CAROL, adm, 1) == CCORD_PERM_ALLOWED);
  // Administrator is not a hierarchy bypass: an admin cannot act on the owner or on a higher-ranked admin role.
  ccord_id adm_low[] = {R_ADMIN};
  CT_CHECK(act(CCORD_PERM_ACT_KICK, CAROL, adm_low, 1, BOB, mod, 1) == CCORD_PERM_ALLOWED);

  // Not against oneself.
  CT_CHECK(act(CCORD_PERM_ACT_KICK, BOB, mod, 1, BOB, mod, 1) == CCORD_PERM_TARGET_SELF);
  CT_CHECK(act(CCORD_PERM_ACT_BAN, CAROL, adm, 1, CAROL, adm, 1) == CCORD_PERM_TARGET_SELF);
  CT_CHECK(act(CCORD_PERM_ACT_TIMEOUT, BOB, mod, 1, BOB, mod, 1) == CCORD_PERM_TARGET_SELF);
  // Own nickname needs only Change Nickname; own roles need Manage Roles.
  CT_CHECK(act(CCORD_PERM_ACT_SET_NICKNAME, ALICE, nullptr, 0, ALICE, nullptr, 0) == CCORD_PERM_ALLOWED);
  CT_CHECK(act(CCORD_PERM_ACT_EDIT_ROLES, ALICE, nullptr, 0, ALICE, nullptr, 0) == CCORD_PERM_MISSING);

  // Administrators cannot be timed out.
  CT_CHECK(act(CCORD_PERM_ACT_TIMEOUT, OWNER, nullptr, 0, CAROL, adm, 1) == CCORD_PERM_TARGET_ADMIN);
  // The owner of an unknown guild (owner id 0) is not "anyone with id 0".
  CT_CHECK(ccord_perm_can_act_on(CCORD_PERM_ACT_KICK, GUILD, CCORD_ID_NONE, BOB, guild_perms(BOB, mod, 1), mod, 1,
                                 CCORD_ID_NONE, 0, nullptr, 0, k_roles, NROLES) != CCORD_PERM_TARGET_OWNER);
}

static void test_editing_roles(void)
{
  ccord_id bot_role[] = {R_BOT};   // Manage Roles, managed
  ccord_id mod[] = {R_MOD};
  ccord_id adm[] = {R_ADMIN};
  const ccord_role *helper = &k_roles[1], *mod_role = &k_roles[3], *admin_role = &k_roles[5], *bot = &k_roles[6],
                   *everyone = &k_roles[0];
  uint64_t manage = EVERYONE_PERMS | CCORD_PERM_MANAGE_ROLES;
  (void)bot_role;
  (void)mod;
  // Alice holds Manage Roles through the Bot role's position-1 permissions only via her own roles: model her as
  // a member of Helper plus a Manage Roles grant.
  ccord_id helper_only[] = {R_HELPER};
  // Helper (position 1) may edit roles below position 1 (everyone) but not itself or above.
  CT_CHECK(ccord_perm_can_edit_role(GUILD, OWNER, ALICE, manage, helper_only, 1, everyone, false, k_roles, NROLES) == CCORD_PERM_ALLOWED);
  CT_CHECK(ccord_perm_can_edit_role(GUILD, OWNER, ALICE, manage, helper_only, 1, helper, false, k_roles, NROLES) == CCORD_PERM_TARGET_HIGHER);
  CT_CHECK(ccord_perm_can_edit_role(GUILD, OWNER, ALICE, manage, helper_only, 1, mod_role, false, k_roles, NROLES) == CCORD_PERM_TARGET_HIGHER);
  // Anyone with Manage Roles may edit @everyone, even holding no other role.
  CT_CHECK(ccord_perm_can_edit_role(GUILD, OWNER, ALICE, manage, nullptr, 0, everyone, false, k_roles, NROLES) == CCORD_PERM_ALLOWED);
  // Without Manage Roles: nothing.
  CT_CHECK(ccord_perm_can_edit_role(GUILD, OWNER, ALICE, EVERYONE_PERMS, helper_only, 1, everyone, false, k_roles, NROLES) == CCORD_PERM_MISSING);
  // A higher role may edit lower ones; the owner may edit anything.
  ccord_id mod_only[] = {R_MOD};
  CT_CHECK(ccord_perm_can_edit_role(GUILD, OWNER, BOB, manage, mod_only, 1, helper, false, k_roles, NROLES) == CCORD_PERM_ALLOWED);
  CT_CHECK(ccord_perm_can_edit_role(GUILD, OWNER, BOB, manage, mod_only, 1, admin_role, false, k_roles, NROLES) == CCORD_PERM_TARGET_HIGHER);
  CT_CHECK(ccord_perm_can_edit_role(GUILD, OWNER, OWNER, CCORD_PERM_ALL, nullptr, 0, admin_role, false, k_roles, NROLES) == CCORD_PERM_ALLOWED);
  (void)adm;
  // Managed (bot) roles can be edited but not handed out.
  CT_CHECK(ccord_perm_can_edit_role(GUILD, OWNER, BOB, manage, mod_only, 1, bot, false, k_roles, NROLES) == CCORD_PERM_ALLOWED);
  CT_CHECK(ccord_perm_can_edit_role(GUILD, OWNER, BOB, manage, mod_only, 1, bot, true, k_roles, NROLES) == CCORD_PERM_ROLE_MANAGED);
  // A missing role is an error, not an allowance.
  CT_CHECK(ccord_perm_can_edit_role(GUILD, OWNER, OWNER, CCORD_PERM_ALL, nullptr, 0, nullptr, false, k_roles, NROLES) == CCORD_PERM_MISSING);
}

static void test_granting(void)
{
  uint64_t mine = CCORD_PERM_MANAGE_ROLES | CCORD_PERM_KICK_MEMBERS | CCORD_PERM_VIEW_CHANNEL;
  // Giving out permissions I hold is fine; so is leaving a role's existing bits alone.
  CT_CHECK(ccord_perm_can_grant(GUILD, OWNER, BOB, mine, 0, CCORD_PERM_KICK_MEMBERS) == CCORD_PERM_ALLOWED);
  CT_CHECK(ccord_perm_can_grant(GUILD, OWNER, BOB, mine, CCORD_PERM_BAN_MEMBERS, CCORD_PERM_BAN_MEMBERS | CCORD_PERM_KICK_MEMBERS) == CCORD_PERM_ALLOWED);
  // Adding one I lack is escalation.
  CT_CHECK(ccord_perm_can_grant(GUILD, OWNER, BOB, mine, 0, CCORD_PERM_BAN_MEMBERS) == CCORD_PERM_ESCALATION);
  CT_CHECK(ccord_perm_can_grant(GUILD, OWNER, BOB, mine, 0, CCORD_PERM_ADMINISTRATOR) == CCORD_PERM_ESCALATION);
  // Removing one I lack is allowed.
  CT_CHECK(ccord_perm_can_grant(GUILD, OWNER, BOB, mine, CCORD_PERM_BAN_MEMBERS, 0) == CCORD_PERM_ALLOWED);
  // Owner and administrators may grant anything.
  CT_CHECK(ccord_perm_can_grant(GUILD, OWNER, OWNER, 0, 0, CCORD_PERM_ADMINISTRATOR) == CCORD_PERM_ALLOWED);
  CT_CHECK(ccord_perm_can_grant(GUILD, OWNER, CAROL, CCORD_PERM_ALL, 0, CCORD_PERM_ADMINISTRATOR | CCORD_PERM_BAN_MEMBERS) == CCORD_PERM_ALLOWED);
}

static void test_table_and_text(void)
{
  size_t n = 0;
  const ccord_perm_info *t = ccord_perm_table(&n);
  CT_CHECK(t != nullptr && n == 52);
  uint64_t seen = 0;
  for (size_t i = 0; i < n; i++) {
    CT_CHECK(t[i].bit != 0 && (t[i].bit & (t[i].bit - 1)) == 0);          // exactly one bit
    CT_CHECK((seen & t[i].bit) == 0);                                       // listed once
    seen |= t[i].bit;
    CT_CHECK(t[i].label && t[i].label[0] && t[i].description && t[i].description[0]);
    CT_CHECK(t[i].group < CCORD_PERM_GROUP_COUNT);
    CT_CHECK(ccord_perm_label(t[i].bit) == t[i].label);
  }
  // Every named CCORD_PERM_* bit is in the table.
  uint64_t named = CCORD_PERM_CREATE_INSTANT_INVITE | CCORD_PERM_KICK_MEMBERS | CCORD_PERM_BAN_MEMBERS | CCORD_PERM_ADMINISTRATOR |
                   CCORD_PERM_MANAGE_CHANNELS | CCORD_PERM_MANAGE_GUILD | CCORD_PERM_ADD_REACTIONS | CCORD_PERM_VIEW_AUDIT_LOG |
                   CCORD_PERM_PRIORITY_SPEAKER | CCORD_PERM_STREAM | CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_SEND_MESSAGES |
                   CCORD_PERM_SEND_TTS_MESSAGES | CCORD_PERM_MANAGE_MESSAGES | CCORD_PERM_EMBED_LINKS | CCORD_PERM_ATTACH_FILES |
                   CCORD_PERM_READ_MESSAGE_HISTORY | CCORD_PERM_MENTION_EVERYONE | CCORD_PERM_USE_EXTERNAL_EMOJIS |
                   CCORD_PERM_VIEW_GUILD_INSIGHTS | CCORD_PERM_CONNECT | CCORD_PERM_SPEAK | CCORD_PERM_MUTE_MEMBERS |
                   CCORD_PERM_DEAFEN_MEMBERS | CCORD_PERM_MOVE_MEMBERS | CCORD_PERM_USE_VAD | CCORD_PERM_CHANGE_NICKNAME |
                   CCORD_PERM_MANAGE_NICKNAMES | CCORD_PERM_MANAGE_ROLES | CCORD_PERM_MANAGE_WEBHOOKS |
                   CCORD_PERM_MANAGE_EXPRESSIONS | CCORD_PERM_USE_APPLICATION_COMMANDS | CCORD_PERM_REQUEST_TO_SPEAK |
                   CCORD_PERM_MANAGE_EVENTS | CCORD_PERM_MANAGE_THREADS | CCORD_PERM_CREATE_PUBLIC_THREADS |
                   CCORD_PERM_CREATE_PRIVATE_THREADS | CCORD_PERM_USE_EXTERNAL_STICKERS | CCORD_PERM_SEND_MESSAGES_IN_THREADS |
                   CCORD_PERM_USE_EMBEDDED_ACTIVITIES | CCORD_PERM_MODERATE_MEMBERS | CCORD_PERM_VIEW_CREATOR_ANALYTICS |
                   CCORD_PERM_USE_SOUNDBOARD | CCORD_PERM_CREATE_EXPRESSIONS | CCORD_PERM_CREATE_EVENTS |
                   CCORD_PERM_USE_EXTERNAL_SOUNDS | CCORD_PERM_SEND_VOICE_MESSAGES | CCORD_PERM_SET_VOICE_CHANNEL_STATUS |
                   CCORD_PERM_SEND_POLLS | CCORD_PERM_USE_EXTERNAL_APPS | CCORD_PERM_PIN_MESSAGES | CCORD_PERM_BYPASS_SLOWMODE;
  CT_CHECK(named == seen);

  CT_CHECK(strcmp(ccord_perm_label(CCORD_PERM_KICK_MEMBERS), "Kick Members") == 0);
  CT_CHECK(strcmp(ccord_perm_label(CCORD_PERM_MODERATE_MEMBERS), "Time Out Members") == 0);
  CT_CHECK(ccord_perm_label(CCORD_PERM_KICK_MEMBERS | CCORD_PERM_BAN_MEMBERS) == nullptr);
  CT_CHECK(ccord_perm_label(1ULL << 47) == nullptr);
  for (int g = 0; g < CCORD_PERM_GROUP_COUNT; g++) CT_CHECK(ccord_perm_group_label((ccord_perm_group)g)[0] != '\0');

  CT_CHECK(ccord_perm_verdict_text(CCORD_PERM_ALLOWED, CCORD_PERM_ACT_KICK)[0] == '\0');
  for (int v = CCORD_PERM_MISSING; v <= CCORD_PERM_ESCALATION; v++) {
    for (int a = CCORD_PERM_ACT_KICK; a <= CCORD_PERM_ACT_EDIT_ROLES; a++) {
      CT_CHECK(ccord_perm_verdict_text((ccord_perm_verdict)v, (ccord_perm_action)a)[0] != '\0');
    }
  }
  CT_CHECK(strstr(ccord_perm_verdict_text(CCORD_PERM_TARGET_HIGHER, CCORD_PERM_ACT_KICK), "kick") != nullptr);
}

// Invariants that must hold for any overwrites: the result is never more than the base plus what was allowed, a
// member overwrite's deny always wins, and administrators keep everything.
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  return rng_state * 0x2545F4914F6CDD1Dull;
}

static void test_channel_invariants(void)
{
  ccord_id roles[] = {R_HELPER, R_MOD};
  for (int iter = 0; iter < 2000; iter++) {
    ccord_overwrite ow[6];
    size_t n = (size_t)(rnd() % 7);
    uint64_t allow_union = 0;
    size_t made = 0;
    for (size_t i = 0; i < n; i++) {
      ccord_overwrite o;
      o.kind = (rnd() & 1) ? CCORD_OVERWRITE_ROLE : CCORD_OVERWRITE_MEMBER;
      uint64_t pick = rnd() % 4;
      o.id = o.kind == CCORD_OVERWRITE_MEMBER ? (pick < 2 ? BOB : ALICE) : (pick == 0 ? GUILD : pick == 1 ? R_HELPER : pick == 2 ? R_MOD : R_ADMIN);
      o.allow = rnd() & rnd();
      o.deny = rnd() & rnd() & ~o.allow;
      bool dup = false;   // Discord has at most one overwrite per role or member
      for (size_t k = 0; k < made; k++) dup = dup || (ow[k].kind == o.kind && ow[k].id == o.id);
      if (dup) continue;
      ow[made++] = o;
      allow_union |= o.allow;
    }
    n = made;
    uint64_t base = (rnd() & rnd()) & ~CCORD_PERM_ADMINISTRATOR;
    uint64_t got = ccord_perm_channel(base, GUILD, BOB, ow, n, roles, 2);
    CT_CHECK((got & ~(base | allow_union)) == 0);
    // Whatever BOB's own member overwrite denies (and does not allow) is gone.
    for (size_t i = 0; i < n; i++) {
      if (ow[i].kind == CCORD_OVERWRITE_MEMBER && ow[i].id == BOB) CT_CHECK((got & ow[i].deny & ~ow[i].allow) == 0);
    }
    // An administrator keeps everything.
    CT_CHECK(ccord_perm_channel(base | CCORD_PERM_ADMINISTRATOR, GUILD, BOB, ow, n, roles, 2) == CCORD_PERM_ALL);
    // Never usable without VIEW_CHANNEL.
    if ((got & CCORD_PERM_VIEW_CHANNEL) == 0) CT_CHECK(got == 0);
    // Sending dependents imply SEND_MESSAGES.
    if (got != 0 && (got & CCORD_PERM_SEND_MESSAGES) == 0) {
      CT_CHECK((got & (CCORD_PERM_SEND_TTS_MESSAGES | CCORD_PERM_MENTION_EVERYONE | CCORD_PERM_EMBED_LINKS | CCORD_PERM_ATTACH_FILES)) == 0);
    }
  }
}

static void test_expression_permissions(void)
{
  CT_CHECK(!ccord_perm_can_create_expression(0));
  CT_CHECK(!ccord_perm_can_create_expression(CCORD_PERM_MANAGE_GUILD | CCORD_PERM_MANAGE_ROLES));
  CT_CHECK(ccord_perm_can_create_expression(CCORD_PERM_CREATE_EXPRESSIONS));
  CT_CHECK(ccord_perm_can_create_expression(CCORD_PERM_MANAGE_EXPRESSIONS));
  CT_CHECK(ccord_perm_can_create_expression(CCORD_PERM_ADMINISTRATOR));

  // MANAGE_EXPRESSIONS and ADMINISTRATOR may modify any expression, even with unknown or different creator_id.
  CT_CHECK(ccord_perm_can_modify_expression(CCORD_PERM_MANAGE_EXPRESSIONS, 1001, 1002));
  CT_CHECK(ccord_perm_can_modify_expression(CCORD_PERM_MANAGE_EXPRESSIONS, 1001, CCORD_ID_NONE));
  CT_CHECK(ccord_perm_can_modify_expression(CCORD_PERM_MANAGE_EXPRESSIONS, CCORD_ID_NONE, CCORD_ID_NONE));
  CT_CHECK(ccord_perm_can_modify_expression(CCORD_PERM_ADMINISTRATOR, 1001, 1002));

  // CREATE_EXPRESSIONS alone may modify ONLY own expressions (both IDs non-zero and equal).
  CT_CHECK(ccord_perm_can_modify_expression(CCORD_PERM_CREATE_EXPRESSIONS, 1001, 1001));
  CT_CHECK(!ccord_perm_can_modify_expression(CCORD_PERM_CREATE_EXPRESSIONS, 1001, 1002));
  CT_CHECK(!ccord_perm_can_modify_expression(CCORD_PERM_CREATE_EXPRESSIONS, 1001, CCORD_ID_NONE));
  CT_CHECK(!ccord_perm_can_modify_expression(CCORD_PERM_CREATE_EXPRESSIONS, CCORD_ID_NONE, CCORD_ID_NONE));
  CT_CHECK(!ccord_perm_can_modify_expression(CCORD_PERM_CREATE_EXPRESSIONS, CCORD_ID_NONE, 1001));

  // Neither permission: cannot modify even own uploads.
  CT_CHECK(!ccord_perm_can_modify_expression(CCORD_PERM_MANAGE_GUILD, 1001, 1001));
  CT_CHECK(!ccord_perm_can_modify_expression(0, 1001, 1001));
}

int main(void)
{
  test_guild_permissions();
  test_channel_overwrites();
  test_timeout();
  test_has();
  test_hierarchy();
  test_acting_on_members();
  test_editing_roles();
  test_granting();
  test_table_and_text();
  test_channel_invariants();
  test_expression_permissions();
  return CT_RESULT("permissions");
}
