// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>

#include <ccord/model/contrib.h>
#include <ccord/model/model.h>
#include <ccord/model/rich.h>
#include <ccord/model/search.h>

#include "ccord_test.h"

static int activated;
static int commanded;
static int panel_events;
static char last_panel_node[64];

static void on_menu(void *ud, const ccord_menu_contribution *item, const ccord_menu_context *ctx)
{
  (void)ud;
  if (ctx->target == CCORD_MENU_NAV) {
    CT_CHECK(strcmp(item->item_id, "nav-open") == 0);
    activated++;
    return;
  }
  CT_CHECK(strcmp(item->item_id, "copy-id") == 0);
  CT_CHECK(ctx->message_id == 77);
  activated++;
}

static void on_cmd(void *ud, const ccord_command_contribution *cmd, const char *args, ccord_id g, ccord_id c)
{
  (void)ud; (void)args; (void)g; (void)c;
  CT_CHECK(strcmp(cmd->name, "shrug") == 0);
  commanded++;
}

static void on_panel_ev(void *ud, const ccord_panel_contribution *panel, const ccord_panel_event *ev)
{
  (void)ud;
  CT_CHECK(panel != nullptr && ev != nullptr);
  snprintf(last_panel_node, sizeof last_panel_node, "%s", ev->node_id ? ev->node_id : "");
  panel_events++;
}

static bool mock_ui_nav(void *ud, ccord_id g, ccord_id c, ccord_id p, const char *name, const char *view)
{
  int *cnt = ud;
  (void)g; (void)c; (void)p; (void)name;
  if (view && strcmp(view, "friends") == 0) { (*cnt)++; return true; }
  return false;
}

static bool mock_ui_act(void *ud, const char *action)
{
  int *cnt = ud;
  if (action && strcmp(action, "quick_switcher") == 0) { (*cnt)++; return true; }
  return false;
}

static const char *mock_ui_view(void *ud)
{
  const int *cnt = ud;
  return *cnt > 0 ? "friends" : "channel";
}

static void test_ids(void)
{
  ccord_id id = 0;
  CT_CHECK(ccord_id_parse("12345", 5, &id) && id == 12345);
  CT_CHECK(!ccord_id_parse("0", 1, &id));
  CT_CHECK(!ccord_id_parse("+1", 2, &id));
  CT_CHECK(!ccord_id_parse("18446744073709551616", 20, &id));
  CT_CHECK(ccord_id_parse("18446744073709551615", 20, &id) && id == UINT64_MAX);
  char out[21];
  CT_CHECK(ccord_id_format(90210, out) == 5 && strcmp(out, "90210") == 0);
}

static void test_snapshot(void)
{
  ccord_user users[2] = {
    {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 2, .name = "Ann", .status_text = "", .presence = CCORD_PRESENCE_IDLE},
  };
  ccord_guild guilds[1] = {{.id = 10, .name = "HQ"}};
  ccord_channel chans[2] = {
    {.id = 20, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_CATEGORY, .name = "TEXT", .topic = ""},
    {.id = 21, .guild_id = 10, .parent_id = 20, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""},
  };
  ccord_member members[1] = {{.guild_id = 10, .user_id = 2, .role_name = "Members", .role_rank = 0}};
  ccord_snapshot s = {.self_id = 1, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
                      .channels = chans, .channel_count = 2, .members = members, .member_count = 1};
  ccord_error err;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_OK);

  users[1].id = 1; // duplicate user id
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_ERR_INVALID);
  users[1].id = 2;

  chans[1].parent_id = 21; // parent must be a category
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_ERR_INVALID);
  chans[1].parent_id = 20;

  users[0].bio = "Native C23 client";
  users[0].pronouns = "they/them";
  users[0].handle = "you_dev";
  users[0].banner_url = "https://cdn.discordapp.com/banners/1/b.png?size=600";
  users[0].accent_color = 0x5865F2u;
  ccord_relationship rels[1] = {{.user_id = 2, .kind = CCORD_RELATIONSHIP_FRIEND}};
  s.relationships = rels;
  s.relationship_count = 1;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_OK);

  ccord_user_profile prof = {
    .user_id = 2,
    .display_name = "Ann",
    .handle = "ann_handle",
    .avatar_url = "https://cdn.discordapp.com/avatars/2/a.png?size=128",
    .banner_url = "https://cdn.discordapp.com/banners/2/b.png?size=600",
    .accent_color = 0x57F287u,
    .bio = "Distributed systems",
    .pronouns = "she/her",
  };
  CT_CHECK(ccord_user_profile_validate(&prof, &err) == CCORD_OK);
  prof.accent_color = 0x1000000u;
  CT_CHECK(ccord_user_profile_validate(&prof, &err) == CCORD_ERR_INVALID);
  prof.accent_color = 0x57F287u;
  prof.user_id = 0;
  CT_CHECK(ccord_user_profile_validate(&prof, &err) == CCORD_ERR_INVALID);

  rels[0].kind = (ccord_relationship_kind)99;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_ERR_INVALID);
  rels[0].kind = CCORD_RELATIONSHIP_NONE;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_ERR_INVALID);
}

static void test_contrib(void)
{
  ccord_error err;
  ccord_contrib *c = ccord_contrib_create(&err);
  CT_CHECK(c != nullptr);
  ccord_contrib_handlers h = {
    .ud = nullptr, .menu_activate = on_menu, .command_run = on_cmd, .panel_event = on_panel_ev
  };
  ccord_contrib_set_handlers(c, &h);
  uint64_t rev0 = ccord_contrib_revision(c);
  CT_CHECK(ccord_contrib_add_menu(c, "dev.ccord.copy-id", "copy-id", "Copy Message ID", CCORD_MENU_MESSAGE, &err) == CCORD_OK);
  CT_CHECK(ccord_contrib_revision(c) > rev0);
  CT_CHECK(ccord_contrib_add_menu(c, "dev.ccord.copy-id", "copy-id", "again", CCORD_MENU_MESSAGE, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_contrib_menu_count(c, CCORD_MENU_MESSAGE) == 1);
  CT_CHECK(ccord_contrib_add_menu(c, "dev.ccord.nav", "nav-open", "Open Aux", CCORD_MENU_NAV, &err) == CCORD_OK);
  CT_CHECK(ccord_contrib_menu_count(c, CCORD_MENU_NAV) == 1);
  CT_CHECK(ccord_contrib_add_command(c, "dev.ccord.shrug", "shrug", "Shrug", &err) == CCORD_OK);
  CT_CHECK(ccord_contrib_add_command(c, "x", "Bad Name", "", &err) == CCORD_ERR_INVALID);

  ccord_menu_context ctx = {.target = CCORD_MENU_MESSAGE, .message_id = 77};
  CT_CHECK(ccord_contrib_activate_menu(c, "dev.ccord.copy-id", "copy-id", &ctx, &err) == CCORD_OK);
  CT_CHECK(activated == 1);
  ccord_menu_context nav_ctx = {.target = CCORD_MENU_NAV, .message_id = 1};
  CT_CHECK(ccord_contrib_activate_menu(c, "dev.ccord.nav", "nav-open", &nav_ctx, &err) == CCORD_OK);
  CT_CHECK(activated == 2);
  CT_CHECK(ccord_contrib_remove_menu(c, "dev.ccord.nav", "nav-open"));
  CT_CHECK(ccord_contrib_menu_count(c, CCORD_MENU_NAV) == 0);
  CT_CHECK(!ccord_contrib_remove_menu(c, "dev.ccord.nav", "nav-open"));

  CT_CHECK(ccord_contrib_run_command(c, "shrug", "", 0, 0, &err) == CCORD_OK);
  CT_CHECK(commanded == 1);

  const char *spec1 = "{\"type\":\"row\",\"children\":[{\"type\":\"text\",\"text\":\"hello\"}]}";
  CT_CHECK(ccord_contrib_set_panel(c, "dev.ccord.bar", "main-bar", CCORD_PANEL_TOP, 34.0f, spec1, &err) == CCORD_OK);
  CT_CHECK(ccord_contrib_panel_count(c) == 1);
  const ccord_panel_contribution *p = ccord_contrib_find_panel(c, "dev.ccord.bar", "main-bar");
  CT_CHECK(p != nullptr && p->slot == CCORD_PANEL_TOP && p->height == 34.0f);
  uint64_t r1 = p->revision;
  const char *spec2 = "{\"type\":\"row\",\"children\":[{\"type\":\"text\",\"text\":\"updated\"}]}";
  CT_CHECK(ccord_contrib_set_panel(c, "dev.ccord.bar", "main-bar", CCORD_PANEL_TOP, 40.0f, spec2, &err) == CCORD_OK);
  p = ccord_contrib_find_panel(c, "dev.ccord.bar", "main-bar");
  CT_CHECK(p != nullptr && p->height == 40.0f && p->revision > r1 && strcmp(p->root_json, spec2) == 0);
  ccord_panel_event pev = {.kind = CCORD_PANEL_EV_CLICK, .node_id = "btn-1", .button = 1};
  CT_CHECK(ccord_contrib_dispatch_panel_event(c, "dev.ccord.bar", "main-bar", &pev, &err) == CCORD_OK);
  CT_CHECK(panel_events == 1 && strcmp(last_panel_node, "btn-1") == 0);

  CT_CHECK(ccord_contrib_set_panel(c, "dev.ccord.copy-id", "sub", CCORD_PANEL_SUBHEADER, 28.0f, spec1, &err) == CCORD_OK);
  CT_CHECK(ccord_contrib_panel_count(c) == 2);

  int ui_calls = 0;
  ccord_contrib_ui_hooks uh = {.ud = &ui_calls, .navigate = mock_ui_nav, .ui_action = mock_ui_act, .get_view = mock_ui_view};
  ccord_contrib_set_ui_hooks(c, &uh);
  CT_CHECK(strcmp(ccord_contrib_ui_view(c), "channel") == 0);
  CT_CHECK(ccord_contrib_ui_navigate(c, 0, 0, 0, nullptr, "friends"));
  CT_CHECK(strcmp(ccord_contrib_ui_view(c), "friends") == 0);
  CT_CHECK(ccord_contrib_ui_action(c, "quick_switcher"));
  CT_CHECK(ui_calls == 2);
  ccord_contrib_set_ui_hooks(c, nullptr);
  CT_CHECK(strcmp(ccord_contrib_ui_view(c), "channel") == 0);

  ccord_contrib_remove_owner(c, "dev.ccord.copy-id");
  CT_CHECK(ccord_contrib_menu_count(c, CCORD_MENU_MESSAGE) == 0);
  CT_CHECK(ccord_contrib_panel_count(c) == 1);
  CT_CHECK(ccord_contrib_remove_panel(c, "dev.ccord.bar", "main-bar"));
  CT_CHECK(ccord_contrib_panel_count(c) == 0);
  CT_CHECK(!ccord_contrib_remove_panel(c, "dev.ccord.bar", "main-bar"));
  CT_CHECK(ccord_contrib_activate_menu(c, "dev.ccord.copy-id", "copy-id", &ctx, &err) == CCORD_ERR_NOT_FOUND);

  ccord_contrib_set_handlers(c, nullptr);
  CT_CHECK(ccord_contrib_run_command(c, "shrug", "", 0, 0, &err) == CCORD_ERR_UNSUPPORTED);
  ccord_contrib_destroy(c);
}

static void test_message_reactions(void)
{
  ccord_error err;
  ccord_reaction rs[2] = {
    {.emoji = "\xf0\x9f\x91\x8d", .count = 2, .self_reacted = true},
    {.emoji = "party", .count = 1, .self_reacted = false},
  };
  ccord_message m = {.id = 1, .channel_id = 2, .author_id = 3, .content = "hi",
                     .reaction_count = 3, .self_reacted = true,
                     .reactions = rs, .reaction_kind_count = 2};
  CT_CHECK(ccord_message_validate(&m, &err) == CCORD_OK);
  rs[0].emoji = "";
  CT_CHECK(ccord_message_validate(&m, &err) == CCORD_ERR_INVALID);
  rs[0].emoji = "\xf0\x9f\x91\x8d";
  m.reactions = nullptr;
  CT_CHECK(ccord_message_validate(&m, &err) == CCORD_ERR_INVALID);
}

static bool dummy_user_matches(void *ud, ccord_id user_id, const char *needle)
{
  (void)ud;
  if (user_id == 42 && ccord_search_contains_ci("Alice", needle)) return true;
  if (user_id == 77 && ccord_search_contains_ci("Bob", needle)) return true;
  return false;
}

static void test_search_parse_and_match(void)
{
  int64_t day_ms = 0;
  CT_CHECK(ccord_search_date_to_unix_ms("2025-05-10", 10, &day_ms));
  CT_CHECK(day_ms == 1746835200000LL);
  CT_CHECK(!ccord_search_date_to_unix_ms("2025-02-29", 10, &day_ms));
  CT_CHECK(ccord_search_date_to_unix_ms("2024-02-29", 10, &day_ms));
  CT_CHECK(!ccord_search_date_to_unix_ms("2014-12-31", 10, &day_ms));

  ccord_id sf = 0;
  CT_CHECK(ccord_search_unix_ms_to_snowflake(1746835200000LL, &sf));
  CT_CHECK((sf >> 22) == (uint64_t)(1746835200000LL - CCORD_DISCORD_EPOCH_MS));

  ccord_search_query q;
  const char *raw = "from:@Alice mentions:<@!77> has:image has:link has:poll pinned:true in:#dev during:2025-05-10 crash report";
  ccord_search_parse(raw, strlen(raw), &q);
  CT_CHECK(q.has_operators);
  CT_CHECK(strcmp(q.from, "Alice") == 0 && q.author_id == CCORD_ID_NONE);
  CT_CHECK(q.mentions_id == 77);
  CT_CHECK(strcmp(q.in_channel, "dev") == 0 && q.channel_id == CCORD_ID_NONE);
  CT_CHECK(q.pinned == 1);
  CT_CHECK((q.has_mask & CCORD_SEARCH_HAS_IMAGE) != 0);
  CT_CHECK((q.has_mask & CCORD_SEARCH_HAS_LINK) != 0);
  CT_CHECK((q.has_mask & CCORD_SEARCH_HAS_POLL) != 0);
  CT_CHECK(q.min_id == sf && q.max_id == sf + (86400000ULL << 22) && !q.min_exclusive);
  CT_CHECK(strcmp(q.content, "crash report") == 0 && q.term_count == 2);

  ccord_id mention_ids[1] = {77};
  ccord_attachment att = {.id = 1, .filename = "screen.png", .content_type = "image/png", .url = "https://cdn/s.png"};
  ccord_message_rich rich = {
    .present = CCORD_RICH_ALL,
    .mention_users = mention_ids,
    .mention_user_count = 1,
    .attachments = &att,
    .attachment_count = 1,
    .poll = {.present = true, .question = "Ship?"},
  };
  ccord_message m = {
    .id = sf + (3600000ULL << 22),
    .channel_id = 500,
    .author_id = 42,
    .pinned = true,
    .content = "See https://example.com/bug for the crash report",
    .rich = &rich,
  };
  ccord_search_msg_ctx ctx = {
    .self_id = 1,
    .author_name = "Alice",
    .channel_name = "dev-ops",
    .user_matches = dummy_user_matches,
  };
  CT_CHECK(ccord_search_message_matches(&q, &m, &ctx));
  m.pinned = false;
  CT_CHECK(!ccord_search_message_matches(&q, &m, &ctx));
  m.pinned = true;

  // Plain phrase without operators preserves exact substring matching with internal spaces.
  ccord_search_parse("crash report", 12, &q);
  CT_CHECK(!q.has_operators && strcmp(q.content, "crash report") == 0);
  CT_CHECK(ccord_search_message_matches(&q, &m, &ctx));
  m.content = "report of a crash";
  CT_CHECK(!ccord_search_message_matches(&q, &m, &ctx));

  // Unknown colon token stays in content rather than being swallowed.
  ccord_search_parse("ERR:404", 7, &q);
  CT_CHECK(!q.has_operators && strcmp(q.content, "ERR:404") == 0);

  // Snowflake before/after bounds.
  ccord_search_parse("after:100 before:200", 20, &q);
  CT_CHECK(q.min_id == 100 && q.min_exclusive && q.max_id == 200);
  m.content = "anything";
  m.id = 100;
  CT_CHECK(!ccord_search_message_matches(&q, &m, &ctx));
  m.id = 150;
  CT_CHECK(ccord_search_message_matches(&q, &m, &ctx));
  m.id = 200;
  CT_CHECK(!ccord_search_message_matches(&q, &m, &ctx));
}

static void test_m3c_profiles_notes_and_activities(void)
{
  ccord_error err;
  ccord_user u = {
    .id = 1,
    .name = "Ada",
    .status_text = "Online",
    .presence = CCORD_PRESENCE_ONLINE,
    .clan_tag = "C23",
    .custom_status_emoji = "\xF0\x9F\x94\xA5",
    .custom_status_text = "Shipping M3c",
    .custom_status_expires_ms = 1794315600000LL,
    .has_activity = true,
    .activity_type = CCORD_ACTIVITY_COMPETING,
    .activity_name = "Systems Cup",
    .activity_details = "Finals",
    .activity_state = "Round 3",
    .activity_start_ms = 1794312000000LL,
  };
  ccord_user_note notes[2] = {
    {.user_id = 10, .note = "Compiler lead"},
    {.user_id = 20, .note = ""},
  };
  ccord_snapshot snap = {
    .self_id = 1,
    .users = &u,
    .user_count = 1,
    .notes = notes,
    .note_count = 2,
  };
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_OK);

  u.activity_type = 99;
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_ERR_INVALID);
  u.has_activity = false;
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_OK);
  u.has_activity = true;
  u.activity_type = CCORD_ACTIVITY_PLAYING;

  memset(u.clan_tag, 'X', sizeof u.clan_tag); // missing NUL
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_ERR_INVALID);
  snprintf(u.clan_tag, sizeof u.clan_tag, "C23");

  snap.notes = nullptr;
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_ERR_INVALID);
  snap.notes = notes;
  snap.note_count = CCORD_MAX_USER_NOTES + 1u;
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_ERR_RANGE);
  snap.note_count = 2;

  notes[1].user_id = 10; // duplicate user note
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_ERR_INVALID);
  notes[1].user_id = 0;  // zero user_id
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_ERR_INVALID);
  notes[1].user_id = 20;

  char long_note[CCORD_USER_NOTE_MAX_BYTES + 2];
  memset(long_note, 'n', CCORD_USER_NOTE_MAX_BYTES + 1);
  long_note[CCORD_USER_NOTE_MAX_BYTES + 1] = '\0';
  notes[1].note = long_note;
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_ERR_INVALID);
  long_note[CCORD_USER_NOTE_MAX_BYTES] = '\0';
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_OK);

  ccord_user_profile prof = {
    .user_id = 501,
    .display_name = "Ada Lovelace",
    .handle = "ada",
    .avatar_url = "https://cdn.discordapp.com/avatars/501/a.png",
    .banner_url = "https://cdn.discordapp.com/banners/501/b.png",
    .accent_color = 0x5865F2,
    .bio = "Analytical Engine",
    .pronouns = "she/her",
    .clan_tag = "MATH",
    .connection_count = 1,
    .connections = {{.type = "github", .name = "adalovelace", .verified = true}},
    .mutual_guild_count = 2,
    .mutual_guild_ids = {100, 200},
    .mutual_friend_count = 1,
    .mutual_friend_total = 4,
    .mutual_friends = {{.user_id = 777, .name = "Babbage", .handle = "charles", .avatar_url = ""}},
  };
  CT_CHECK(ccord_user_profile_validate(&prof, &err) == CCORD_OK);
  CT_CHECK(ccord_user_profile_validate(nullptr, &err) == CCORD_ERR_INVALID);

  prof.user_id = 0;
  CT_CHECK(ccord_user_profile_validate(&prof, &err) == CCORD_ERR_INVALID);
  prof.user_id = 501;

  prof.accent_color = 0x1000000u;
  CT_CHECK(ccord_user_profile_validate(&prof, &err) == CCORD_ERR_INVALID);
  prof.accent_color = 0x5865F2;

  prof.connection_count = CCORD_PROFILE_MAX_CONNECTIONS + 1u;
  CT_CHECK(ccord_user_profile_validate(&prof, &err) == CCORD_ERR_RANGE);
  prof.connection_count = 1;
  prof.connections[0].name[0] = '\0';
  CT_CHECK(ccord_user_profile_validate(&prof, &err) == CCORD_ERR_INVALID);
  snprintf(prof.connections[0].name, sizeof prof.connections[0].name, "adalovelace");

  prof.mutual_guild_count = CCORD_PROFILE_MAX_MUTUAL_GUILDS + 1u;
  CT_CHECK(ccord_user_profile_validate(&prof, &err) == CCORD_ERR_RANGE);
  prof.mutual_guild_count = 2;
  prof.mutual_guild_ids[1] = 0;
  CT_CHECK(ccord_user_profile_validate(&prof, &err) == CCORD_ERR_INVALID);
  prof.mutual_guild_ids[1] = 200;

  prof.mutual_friend_count = CCORD_PROFILE_MAX_MUTUAL_FRIENDS + 1u;
  CT_CHECK(ccord_user_profile_validate(&prof, &err) == CCORD_ERR_RANGE);
  prof.mutual_friend_count = 1;
  prof.mutual_friends[0].user_id = 0;
  CT_CHECK(ccord_user_profile_validate(&prof, &err) == CCORD_ERR_INVALID);
  prof.mutual_friends[0].user_id = 777;
  prof.mutual_friends[0].name[0] = '\0';
  CT_CHECK(ccord_user_profile_validate(&prof, &err) == CCORD_ERR_INVALID);
}

static void test_m4a_guild_settings_and_audit_changes(void)
{
  ccord_error err;
  ccord_user u = {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  ccord_guild g = {
    .id = 10,
    .name = "HQ",
    .owner_id = 1,
    .system_channel_id = 20,
    .system_channel_flags =
        CCORD_SYSCHAN_SUPPRESS_JOIN_NOTIFICATIONS | CCORD_SYSCHAN_SUPPRESS_GUILD_REMINDER_NOTIFICATIONS,
    .default_message_notifications = 1,
    .afk_channel_id = 21,
    .afk_timeout = 900,
    .verification_level = 3,
    .explicit_content_filter = 2,
    .premium_tier = 3,
    .premium_subscription_count = 14,
    .feature_vanity_url = true,
    .feature_banner = true,
    .feature_community = true,
    .invites_disabled_until_ms = 1800000000000LL,
    .dms_disabled_until_ms = 1800003600000LL,
  };
  snprintf(g.banner_url, sizeof g.banner_url, "https://cdn.discordapp.com/banners/10/bn.png?size=600");
  snprintf(g.vanity_url_code, sizeof g.vanity_url_code, "ccord-hq");
  ccord_snapshot snap = {.self_id = 1, .users = &u, .user_count = 1, .guilds = &g, .guild_count = 1};
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_OK);

  memset(g.banner_url, 'b', sizeof g.banner_url); // missing NUL
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_ERR_INVALID);
  snprintf(g.banner_url, sizeof g.banner_url, "https://cdn.discordapp.com/banners/10/bn.png");
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_OK);

  memset(g.vanity_url_code, 'v', sizeof g.vanity_url_code); // missing NUL
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_ERR_INVALID);
  g.vanity_url_code[0] = '\xff';
  g.vanity_url_code[1] = '\0';
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_ERR_INVALID);
  snprintf(g.vanity_url_code, sizeof g.vanity_url_code, "ccord");
  CT_CHECK(ccord_snapshot_validate(&snap, &err) == CCORD_OK);

  CT_CHECK(CCORD_SYSCHAN_KNOWN_MASK == 0x0Fu);
  ccord_audit_entry ae = {.id = 900, .action = 1, .change_count = 1};
  snprintf(ae.changes[0].key, sizeof ae.changes[0].key, "name");
  snprintf(ae.changes[0].old_val, sizeof ae.changes[0].old_val, "Old");
  snprintf(ae.changes[0].new_val, sizeof ae.changes[0].new_val, "New");
  CT_CHECK(ae.change_count == 1 && strcmp(ae.changes[0].key, "name") == 0);
}

static void test_m4b_expression_slot_limits(void)
{
  CT_CHECK(ccord_guild_emoji_slot_limit(0) == 50u);
  CT_CHECK(ccord_guild_emoji_slot_limit(1) == 100u);
  CT_CHECK(ccord_guild_emoji_slot_limit(2) == 150u);
  CT_CHECK(ccord_guild_emoji_slot_limit(3) == 250u);
  CT_CHECK(ccord_guild_emoji_slot_limit(4) == 250u);
  CT_CHECK(ccord_guild_emoji_slot_limit(255) == 250u);

  CT_CHECK(ccord_guild_sticker_slot_limit(0) == 5u);
  CT_CHECK(ccord_guild_sticker_slot_limit(1) == 15u);
  CT_CHECK(ccord_guild_sticker_slot_limit(2) == 30u);
  CT_CHECK(ccord_guild_sticker_slot_limit(3) == 60u);
  CT_CHECK(ccord_guild_sticker_slot_limit(4) == 60u);
  CT_CHECK(ccord_guild_sticker_slot_limit(255) == 60u);

  CT_CHECK(ccord_guild_soundboard_slot_limit(0) == 8u);
  CT_CHECK(ccord_guild_soundboard_slot_limit(1) == 24u);
  CT_CHECK(ccord_guild_soundboard_slot_limit(2) == 36u);
  CT_CHECK(ccord_guild_soundboard_slot_limit(3) == 48u);
  CT_CHECK(ccord_guild_soundboard_slot_limit(4) == 48u);
  CT_CHECK(ccord_guild_soundboard_slot_limit(255) == 48u);
}

static void test_m5_stage_and_dm_call_snapshot(void)
{
  ccord_error err;
  ccord_user users[2] = {
    {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 2, .name = "Ann", .status_text = "", .presence = CCORD_PRESENCE_IDLE},
  };
  ccord_guild guilds[2] = {{.id = 10, .name = "HQ"}, {.id = 11, .name = "Branch"}};
  ccord_channel chans[5] = {
    {.id = 20, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_CATEGORY, .name = "VOICE", .topic = ""},
    {.id = 21, .guild_id = 10, .parent_id = 20, .kind = CCORD_CHANNEL_VOICE, .name = "General", .topic = ""},
    {.id = 22, .guild_id = 10, .parent_id = 20, .kind = CCORD_CHANNEL_STAGE, .name = "Stage", .topic = ""},
    {.id = 23, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""},
    {.id = 90, .guild_id = CCORD_DM_GUILD_ID, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT, .name = "Ann", .topic = ""},
  };
  ccord_voice_state voices[2] = {
    {.guild_id = 10, .channel_id = 22, .user_id = 1, .suppress = true},
    {.guild_id = CCORD_DM_GUILD_ID, .channel_id = 90, .user_id = 2},
  };
  ccord_snapshot s = {.self_id = 1, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 2,
                      .channels = chans, .channel_count = 5,
                      .voice_states = voices, .voice_state_count = 2};
  // Stage channels are valid guild channels; stage audiences and DM/group call participants are
  // valid voice states (the store accepts the same rows through voice_channel_ok).
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_OK);

  // A guild voice channel still works alongside the stage.
  voices[0].channel_id = 21;
  voices[0].suppress = false;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_OK);
  voices[0].channel_id = 22;

  // Guild text channels never carry voice states.
  voices[0].channel_id = 23;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_ERR_INVALID);
  voices[0].channel_id = 22;

  // A stage channel of another guild is not a voice channel of this one.
  voices[0].guild_id = 11;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_ERR_INVALID);
  voices[0].guild_id = 10;

  // DM call states must ride a DM TEXT channel: a guild id mismatch or a voice-kind DM row fails.
  voices[1].guild_id = 10;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_ERR_INVALID);
  voices[1].guild_id = CCORD_DM_GUILD_ID;
  chans[4].kind = CCORD_CHANNEL_VOICE;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_ERR_INVALID);
  chans[4].kind = CCORD_CHANNEL_TEXT;

  // Unknown channel and unknown user are still rejected.
  voices[1].channel_id = 999;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_ERR_INVALID);
  voices[1].channel_id = 90;
  voices[1].user_id = 42;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_ERR_INVALID);
  voices[1].user_id = 2;

  // One voice state per (guild, user): a duplicate is rejected even across channel kinds.
  ccord_voice_state dup[3] = {voices[0], voices[1],
                              {.guild_id = 10, .channel_id = 21, .user_id = 1}};
  s.voice_states = dup;
  s.voice_state_count = 3;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_ERR_INVALID);
  s.voice_states = voices;
  s.voice_state_count = 2;

  // Channel kinds beyond STAGE stay invalid.
  chans[2].kind = (ccord_channel_kind)4;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_ERR_INVALID);
  chans[2].kind = (ccord_channel_kind)255;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_ERR_INVALID);
  chans[2].kind = CCORD_CHANNEL_STAGE;
  CT_CHECK(ccord_snapshot_validate(&s, &err) == CCORD_OK);
}

int main(void)
{
  test_ids();
  test_snapshot();
  test_message_reactions();
  test_contrib();
  test_search_parse_and_match();
  test_m3c_profiles_notes_and_activities();
  test_m4a_guild_settings_and_audit_changes();
  test_m4b_expression_slot_limits();
  test_m5_stage_and_dm_call_snapshot();
  return CT_RESULT("test_model");
}
