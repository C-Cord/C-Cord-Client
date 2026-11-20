// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "live_transport_harness.h"

static void test_forum_json_and_gateway_events(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. READY with a type-15 forum channel (available_tags, default_sort_order) and an active forum thread in guild.threads:
  //    Verify the forum thread is NOT emitted as a sidebar channel in snapshot.channels, and is emitted in snapshot.forum_posts!
  const char *ready_forum =
    "{\"op\":0,\"t\":\"READY\",\"d\":{"
    "\"user\":{\"id\":\"42\",\"username\":\"selfuser\"},"
    "\"guilds\":[{\"id\":\"100\",\"name\":\"ForumGuild\","
    "\"channels\":["
    "{\"id\":\"500\",\"type\":15,\"name\":\"design-forum\",\"flags\":16,\"default_sort_order\":1,"
    "\"available_tags\":["
    "{\"id\":\"601\",\"name\":\"RFC\",\"emoji_name\":\"\\ud83d\\udcd0\",\"moderated\":false},"
    "{\"id\":\"602\",\"name\":\"Bug\",\"emoji_id\":\"999111\",\"moderated\":true}"
    "]}"
    "],"
    "\"threads\":["
    "{\"id\":\"7001\",\"parent_id\":\"500\",\"guild_id\":\"100\",\"type\":11,\"owner_id\":\"42\","
    "\"name\":\"Pinned Forum Thread\",\"flags\":2,\"message_count\":5,"
    "\"applied_tags\":[\"601\",\"602\"],"
    "\"thread_metadata\":{\"archived\":false,\"locked\":false,\"create_timestamp\":\"2026-11-08T10:00:00Z\"}}"
    "],"
    "\"members\":[]}],\"private_channels\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_forum, strlen(ready_forum), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ev.snapshot.channel_count == 1);
  CT_CHECK(ev.snapshot.channels[0].id == 500 && ev.snapshot.channels[0].discord_type == CCORD_DISCORD_CHANNEL_GUILD_FORUM);
  CT_CHECK(ev.ev.forum_meta_count == 1 && ev.ev.forum_metas != nullptr);
  if (ev.ev.forum_meta_count == 1) {
    CT_CHECK(ev.ev.forum_metas[0].forum_id == 500);
    CT_CHECK(ev.ev.forum_metas[0].require_tag);
    CT_CHECK(ev.ev.forum_metas[0].default_sort == CCORD_FORUM_SORT_CREATION);
    CT_CHECK(ev.ev.forum_metas[0].tag_count == 2);
    CT_CHECK(ev.ev.forum_metas[0].tags[0].id == 601 && strcmp(ev.ev.forum_metas[0].tags[0].name, "RFC") == 0);
    CT_CHECK(ev.ev.forum_metas[0].tags[1].id == 602 && ev.ev.forum_metas[0].tags[1].emoji_id == 999111ULL);
  }
  CT_CHECK(ev.ev.forum_post_count == 1 && ev.ev.forum_posts != nullptr);
  if (ev.ev.forum_post_count == 1) {
    CT_CHECK(ev.ev.forum_posts[0].thread_id == 7001 && ev.ev.forum_posts[0].forum_id == 500);
    CT_CHECK(ev.ev.forum_posts[0].pinned);
    CT_CHECK(ev.ev.forum_posts[0].applied_tag_count == 2);
    CT_CHECK(strcmp(ev.ev.forum_posts[0].title, "Pinned Forum Thread") == 0);
  }
  ccord_live_event_free(&ev);

  // 2. REST GET /channels/{forum_id}/threads/search response (ccord_live_json_forum_posts)
  const char *search_resp =
    "{\"threads\":["
    "{\"id\":\"7002\",\"parent_id\":\"500\",\"guild_id\":\"100\",\"type\":11,\"owner_id\":\"88\","
    "\"name\":\"Search Result Post\",\"message_count\":3,\"applied_tags\":[\"601\"],"
    "\"thread_metadata\":{\"archived\":true,\"locked\":false,\"create_timestamp\":\"2026-11-08T12:00:00Z\"}}"
    "],"
    "\"first_messages\":["
    "{\"id\":\"7002\",\"channel_id\":\"7002\",\"author\":{\"id\":\"88\",\"username\":\"chen_u\",\"global_name\":\"Chen\"},"
    "\"content\":\"Starter excerpt with **markdown**\",\"timestamp\":\"2026-11-08T12:00:00Z\","
    "\"reactions\":[{\"count\":7,\"emoji\":{\"name\":\"\\ud83d\\udd25\"}}]}"
    "],"
    "\"has_more\":true,\"total_results\":42}";
  CT_CHECK(ccord_live_json_forum_posts(search_resp, strlen(search_resp), 500, 0, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_FORUM_POSTS && ev.ev.channel_id == 500);
  CT_CHECK(ev.ev.forum_has_more && ev.ev.forum_total == 42 && ev.ev.forum_offset == 0);
  CT_CHECK(ev.ev.forum_post_count == 1 && ev.ev.forum_posts != nullptr);
  if (ev.ev.forum_post_count == 1) {
    CT_CHECK(ev.ev.forum_posts[0].thread_id == 7002);
    CT_CHECK(ev.ev.forum_posts[0].first_author_id == 88);
    CT_CHECK(strcmp(ev.ev.forum_posts[0].first_content, "Starter excerpt with **markdown**") == 0);
    CT_CHECK(ev.ev.forum_posts[0].reaction_count == 7);
  }
  CT_CHECK(ev.user_count == 1 && ev.users[0].id == 88);
  ccord_live_event_free(&ev);

  // 3. REST POST /channels/{forum_id}/threads response (ccord_live_json_forum_post_created)
  const char *create_resp =
    "{\"id\":\"7003\",\"parent_id\":\"500\",\"guild_id\":\"100\",\"type\":11,\"owner_id\":\"42\","
    "\"name\":\"Created Forum Thread\",\"applied_tags\":[\"601\"],"
    "\"message\":{\"id\":\"7003\",\"channel_id\":\"7003\",\"author\":{\"id\":\"42\",\"username\":\"selfuser\"},"
    "\"content\":\"Initial post body\",\"timestamp\":\"2026-11-09T01:00:00Z\"}}";
  CT_CHECK(ccord_live_json_forum_post_created(create_resp, strlen(create_resp), 500, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_FORUM_POST_CREATED && ev.ev.channel_id == 500);
  CT_CHECK(ev.ev.forum_post_count == 1 && ev.ev.forum_posts[0].thread_id == 7003);
  CT_CHECK(strcmp(ev.ev.forum_posts[0].first_content, "Initial post body") == 0);
  CT_CHECK(ev.message_count == 1 && ev.messages[0].id == 7003);
  ccord_live_event_free(&ev);
}

// Member changes pushed by Discord (and the REST answer to our own admin calls, which has the same shape) must turn
// into the same GUILD_MEMBERS event, carrying the nickname and the timeout end as unix milliseconds.
// Roles, member role ids and channel overwrites from the gateway and REST, as the permission engine needs them.
static void test_gateway_permission_data(void)
{
  ccord_live_event ev;
  ccord_error err;

  // GUILD_CREATE: role table, the roles of the members it lists, and overwrites per channel (member kind, numeric type).
  const char *create =
    "{\"op\":0,\"t\":\"GUILD_CREATE\",\"d\":{\"id\":\"77\",\"name\":\"HQ\",\"owner_id\":\"5\","
    "\"roles\":[{\"id\":\"77\",\"name\":\"@everyone\",\"permissions\":\"1049600\",\"position\":0},"
    "{\"id\":\"300\",\"name\":\"Mods\",\"permissions\":\"1099511627782\",\"position\":2,\"color\":3447003,"
    "\"hoist\":true,\"mentionable\":true},"
    "{\"id\":\"301\",\"name\":\"Bot\",\"permissions\":\"8\",\"position\":1,\"managed\":true},"
    "{\"id\":\"bad\",\"name\":\"skipped\"}],"
    "\"members\":[{\"user\":{\"id\":\"43\",\"username\":\"Ada\"},\"roles\":[\"300\",\"301\"]},"
    "{\"user\":{\"id\":\"44\",\"username\":\"Bob\"},\"roles\":[]},"
    "{\"user\":{\"id\":\"45\",\"username\":\"Cy\"}}],"
    "\"channels\":[{\"id\":\"11\",\"type\":0,\"name\":\"general\","
    "\"permission_overwrites\":[{\"id\":\"43\",\"type\":1,\"allow\":\"2048\",\"deny\":\"1024\"},"
    "{\"id\":\"300\",\"type\":\"0\",\"allow\":\"1\",\"deny\":\"0\"},"
    "{\"id\":\"x\",\"type\":0,\"allow\":\"1\",\"deny\":\"0\"},"
    "{\"id\":\"301\",\"type\":7,\"allow\":\"1\",\"deny\":\"0\"},"
    "{\"id\":\"302\",\"type\":\"bogus\",\"allow\":\"1\",\"deny\":\"0\"},"
    "{\"id\":\"303\",\"type\":\"member\",\"allow\":\"4\",\"deny\":\"0\"},"
    "{\"id\":\"304\",\"type\":\"role\",\"allow\":\"8\",\"deny\":\"0\"}]},"
    "{\"id\":\"12\",\"type\":0,\"name\":\"open\",\"permission_overwrites\":[]},"
    "{\"id\":\"13\",\"type\":0,\"name\":\"unknown-overwrites\"}]}}";
  CT_CHECK(ccord_live_gateway_decode(create, strlen(create), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT);
  CT_CHECK(ev.ev.role_count == 3);                                   // the entry without a usable id is skipped
  CT_CHECK(ev.ev.roles[1].id == 300 && ev.ev.roles[1].guild_id == 77 && ev.ev.roles[1].hoist && ev.ev.roles[1].mentionable &&
           ev.ev.roles[1].permissions == 1099511627782ull && ev.ev.roles[1].color == 3447003u);
  CT_CHECK(ev.ev.roles[2].id == 301 && ev.ev.roles[2].managed);
  CT_CHECK(ev.ev.member_role_count == 2);                            // Cy has no roles key: unknown, not "none"
  CT_CHECK(ev.ev.member_roles[0].user_id == 43 && ev.ev.member_roles[0].count == 2 && ev.ev.member_roles[0].role_ids[1] == 301);
  CT_CHECK(ev.ev.member_roles[1].user_id == 44 && ev.ev.member_roles[1].count == 0);
  CT_CHECK(ev.ev.overwrite_count == 2);                              // #unknown-overwrites has no key: nothing said
  CT_CHECK(ev.ev.overwrites[0].channel_id == 11 && ev.ev.overwrites[0].count == 4);   // bad id and bad types dropped
  CT_CHECK(ev.ev.overwrites[0].items[0].kind == CCORD_OVERWRITE_MEMBER && ev.ev.overwrites[0].items[0].id == 43 &&
           ev.ev.overwrites[0].items[0].allow == 2048 && ev.ev.overwrites[0].items[0].deny == 1024);
  CT_CHECK(ev.ev.overwrites[0].items[1].kind == CCORD_OVERWRITE_ROLE && ev.ev.overwrites[0].items[1].id == 300);
  CT_CHECK(ev.ev.overwrites[0].items[2].kind == CCORD_OVERWRITE_MEMBER && ev.ev.overwrites[0].items[2].id == 303 &&
           ev.ev.overwrites[0].items[3].kind == CCORD_OVERWRITE_ROLE && ev.ev.overwrites[0].items[3].id == 304);
  CT_CHECK(ev.ev.overwrites[1].channel_id == 12 && ev.ev.overwrites[1].count == 0);    // an empty list is a statement
  ccord_live_event_free(&ev);

  // A guild without roles in the payload (e.g. GUILD_UPDATE without them) says nothing about roles.
  const char *update = "{\"op\":0,\"t\":\"GUILD_UPDATE\",\"d\":{\"id\":\"77\",\"name\":\"HQ2\"}}";
  CT_CHECK(ccord_live_gateway_decode(update, strlen(update), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.role_count == 0 && ev.ev.roles == nullptr && ev.ev.member_role_count == 0 && ev.ev.overwrite_count == 0);
  ccord_live_event_free(&ev);

  // GUILD_ROLE_CREATE / UPDATE / DELETE.
  const char *rc = "{\"op\":0,\"t\":\"GUILD_ROLE_CREATE\",\"d\":{\"guild_id\":\"77\",\"role\":{\"id\":\"310\","
                   "\"name\":\"Helpers\",\"permissions\":\"8192\",\"position\":4,\"color\":0}}}";
  CT_CHECK(ccord_live_gateway_decode(rc, strlen(rc), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_ROLE_UPSERT && ev.ev.guild_id == 77 && ev.ev.role_id == 310 && ev.ev.role_count == 1);
  CT_CHECK(ev.ev.roles[0].guild_id == 77 && strcmp(ev.ev.roles[0].name, "Helpers") == 0 && ev.ev.roles[0].permissions == 8192);
  ccord_live_event_free(&ev);
  const char *ru = "{\"op\":0,\"t\":\"GUILD_ROLE_UPDATE\",\"d\":{\"guild_id\":\"77\",\"role\":{\"id\":\"310\","
                   "\"name\":\"Helpers+\",\"permissions\":\"8200\",\"position\":5}}}";
  CT_CHECK(ccord_live_gateway_decode(ru, strlen(ru), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_ROLE_UPSERT && ev.ev.roles[0].position == 5 && ev.ev.roles[0].permissions == 8200);
  ccord_live_event_free(&ev);
  const char *rd = "{\"op\":0,\"t\":\"GUILD_ROLE_DELETE\",\"d\":{\"guild_id\":\"77\",\"role_id\":\"310\"}}";
  CT_CHECK(ccord_live_gateway_decode(rd, strlen(rd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_ROLE_DELETE && ev.ev.guild_id == 77 && ev.ev.role_id == 310);
  ccord_live_event_free(&ev);
  const char *bad1 = "{\"op\":0,\"t\":\"GUILD_ROLE_CREATE\",\"d\":{\"guild_id\":\"77\",\"role\":{\"name\":\"no id\"}}}";
  CT_CHECK(ccord_live_gateway_decode(bad1, strlen(bad1), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);
  const char *bad2 = "{\"op\":0,\"t\":\"GUILD_ROLE_DELETE\",\"d\":{\"guild_id\":\"77\"}}";
  CT_CHECK(ccord_live_gateway_decode(bad2, strlen(bad2), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);

  // A member's roles change (GUILD_MEMBER_UPDATE) and arrive as member role ids; a payload without them says nothing.
  const char *mu = "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"43\","
                   "\"username\":\"ada\"},\"roles\":[\"300\"]}}";
  CT_CHECK(ccord_live_gateway_decode(mu, strlen(mu), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_MEMBERS && ev.ev.member_role_count == 1 && ev.ev.member_roles[0].guild_id == 77 &&
           ev.ev.member_roles[0].user_id == 43 && ev.ev.member_roles[0].count == 1 && ev.ev.member_roles[0].role_ids[0] == 300);
  ccord_live_event_free(&ev);
  const char *mn = "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"43\","
                   "\"username\":\"ada\"},\"nick\":\"x\"}}";
  CT_CHECK(ccord_live_gateway_decode(mn, strlen(mn), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_role_count == 0);
  ccord_live_event_free(&ev);

  // Member chunks and the lazy member list carry roles too.
  const char *chunk = "{\"op\":0,\"t\":\"GUILD_MEMBERS_CHUNK\",\"d\":{\"guild_id\":\"77\",\"members\":["
                      "{\"user\":{\"id\":\"50\",\"username\":\"u50\"},\"roles\":[\"300\"]},"
                      "{\"user\":{\"id\":\"51\",\"username\":\"u51\"},\"roles\":[]}]}}";
  CT_CHECK(ccord_live_gateway_decode(chunk, strlen(chunk), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_role_count == 2 && ev.ev.member_roles[1].user_id == 51 && ev.ev.member_roles[1].count == 0);
  ccord_live_event_free(&ev);
  const char *list = "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"77\",\"ops\":[{\"op\":\"SYNC\","
                     "\"range\":[0,1],\"items\":[{\"member\":{\"user\":{\"id\":\"60\",\"username\":\"u60\"},"
                     "\"roles\":[\"300\",\"301\"]}},{\"group\":{\"id\":\"online\",\"count\":1}}]}]}}";
  CT_CHECK(ccord_live_gateway_decode(list, strlen(list), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_role_count == 1 && ev.ev.member_roles[0].user_id == 60 && ev.ev.member_roles[0].count == 2);
  ccord_live_event_free(&ev);

  // CHANNEL_UPDATE carries the new overwrites (here: the channel is hidden from @everyone); REST answers do too.
  const char *cu = "{\"op\":0,\"t\":\"CHANNEL_UPDATE\",\"d\":{\"id\":\"11\",\"guild_id\":\"77\",\"type\":0,\"name\":\"general\","
                   "\"permission_overwrites\":[{\"id\":\"77\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]}}";
  CT_CHECK(ccord_live_gateway_decode(cu, strlen(cu), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CHANNEL_UPSERT && ev.ev.overwrite_count == 1 && ev.ev.overwrites[0].channel_id == 11 &&
           ev.ev.overwrites[0].count == 1 && ev.ev.overwrites[0].items[0].deny == 1024);
  ccord_live_event_free(&ev);
  const char *rest = "{\"id\":\"14\",\"guild_id\":\"77\",\"type\":0,\"name\":\"new\","
                     "\"permission_overwrites\":[{\"id\":\"300\",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}]}";
  CT_CHECK(ccord_live_json_channel(rest, strlen(rest), 77, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.overwrite_count == 1 && ev.ev.overwrites[0].channel_id == 14 && ev.ev.overwrites[0].items[0].allow == 1024);
  ccord_live_event_free(&ev);
  const char *no_ow = "{\"op\":0,\"t\":\"CHANNEL_UPDATE\",\"d\":{\"id\":\"11\",\"guild_id\":\"77\",\"type\":0,\"name\":\"general\"}}";
  CT_CHECK(ccord_live_gateway_decode(no_ow, strlen(no_ow), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.overwrite_count == 0);
  ccord_live_event_free(&ev);

  // Limits: a role table is capped, and 600 overwrites on a channel are cut to Discord's 500.
  ccord_buf big;
  ccord_buf_init(&big, 65536);
  (void)ccord_buf_append_str(&big, "{\"op\":0,\"t\":\"CHANNEL_UPDATE\",\"d\":{\"id\":\"11\",\"guild_id\":\"77\",\"type\":0,\"name\":\"g\","
                                   "\"permission_overwrites\":[");
  for (int i = 1; i <= 600; i++) (void)ccord_buf_appendf(&big, "%s{\"id\":\"%d\",\"type\":1,\"allow\":\"1\",\"deny\":\"0\"}", i > 1 ? "," : "", 1000 + i);
  (void)ccord_buf_append_str(&big, "]}}");
  CT_CHECK(!big.failed && ccord_live_gateway_decode(ccord_buf_cstr(&big), big.len, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.overwrite_count == 1 && ev.ev.overwrites[0].count == CCORD_MAX_OVERWRITES_PER_CHANNEL);
  ccord_live_event_free(&ev);
  ccord_buf_free(&big);
}

static void test_gateway_member_events(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *upd =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"43\",\"username\":\"ada\"},"
    "\"nick\":\"Ace\",\"roles\":[],\"communication_disabled_until\":\"2026-11-10T12:10:00.000000+00:00\"}}";
  CT_CHECK(ccord_live_gateway_decode(upd, strlen(upd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_MEMBERS && ev.ev.guild_id == 77);
  CT_CHECK(ev.member_count == 1 && ev.members[0].user_id == 43);
  CT_CHECK(strcmp(ev.members[0].nick, "Ace") == 0);
  CT_CHECK(ev.members[0].timeout_until_ms == 1794312600000LL);
  ccord_live_event_free(&ev);

  // Cleared nickname and lifted timeout arrive as null, and must come through as "none", not be skipped.
  const char *cleared =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"43\",\"username\":\"ada\"},"
    "\"nick\":null,\"roles\":[],\"communication_disabled_until\":null}}";
  CT_CHECK(ccord_live_gateway_decode(cleared, strlen(cleared), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && ev.members[0].nick[0] == '\0' && ev.members[0].timeout_until_ms == 0);
  ccord_live_event_free(&ev);

  const char *add =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_ADD\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"44\",\"username\":\"bob\"},\"roles\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(add, strlen(add), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_MEMBERS && ev.members[0].user_id == 44);
  ccord_live_event_free(&ev);

  const char *rm = "{\"op\":0,\"t\":\"GUILD_MEMBER_REMOVE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"43\"}}}";
  CT_CHECK(ccord_live_gateway_decode(rm, strlen(rm), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MEMBER_REMOVE && ev.ev.guild_id == 77 && ev.ev.user_id == 43);
  ccord_live_event_free(&ev);

  // Malformed payloads are rejected, never turned into a removal of user 0.
  const char *bad = "{\"op\":0,\"t\":\"GUILD_MEMBER_REMOVE\",\"d\":{\"guild_id\":\"77\"}}";
  CT_CHECK(ccord_live_gateway_decode(bad, strlen(bad), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);   // a failed decode still owns what it parsed
  const char *bad2 = "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"user\":{\"id\":\"43\"}}}";
  CT_CHECK(ccord_live_gateway_decode(bad2, strlen(bad2), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);

  // The REST answer to PATCH /guilds/{g}/members/{u} is the same object without guild_id.
  const char *rest = "{\"user\":{\"id\":\"43\",\"username\":\"ada\"},\"nick\":\"Ace\",\"roles\":[],"
                     "\"communication_disabled_until\":\"2026-11-10T12:10:00.000000+00:00\"}";
  CT_CHECK(ccord_live_json_member(rest, strlen(rest), 77, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_MEMBERS && ev.ev.guild_id == 77 && ev.members[0].timeout_until_ms == 1794312600000LL);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_member(rest, strlen(rest), 0, &ev, &err) == CCORD_ERR_INVALID);   // refused before it parses anything
  CT_CHECK(ccord_live_json_member("[]", 2, 77, &ev, &err) == CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);

  // The REST answer to PATCH /guilds/{g}: a rename, with no channel list in it.
  const char *g = "{\"id\":\"77\",\"name\":\"New Name\",\"owner_id\":\"42\",\"icon\":null}";
  CT_CHECK(ccord_live_json_guild(g, strlen(g), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_UPSERT && ev.ev.guild_id == 77 && strcmp(ev.ev.guild->name, "New Name") == 0);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_guild("{}", 2, &ev, &err) == CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);
}

// What the user reads when Discord refuses: its own message, plus the nested detail for form errors.
static void test_discord_error_message(void)
{
  char out[200];
  const char *perm = "{\"message\":\"Missing Permissions\",\"code\":50013}";
  CT_CHECK(ccord_live_json_error_message(perm, strlen(perm), out, sizeof out));
  CT_CHECK(strcmp(out, "Missing Permissions") == 0);

  const char *form =
    "{\"message\":\"Invalid Form Body\",\"code\":50035,\"errors\":{\"name\":{\"_errors\":"
    "[{\"code\":\"BASE_TYPE_MAX_LENGTH\",\"message\":\"Must be 100 or fewer in length.\"}]}}}";
  CT_CHECK(ccord_live_json_error_message(form, strlen(form), out, sizeof out));
  CT_CHECK(strcmp(out, "Invalid Form Body: Must be 100 or fewer in length.") == 0);

  // Deeper nesting (channel permission overwrites, etc.) is still found.
  const char *deep = "{\"message\":\"Invalid Form Body\",\"errors\":{\"a\":{\"b\":{\"c\":{\"_errors\":[{\"message\":\"deep\"}]}}}}}";
  CT_CHECK(ccord_live_json_error_message(deep, strlen(deep), out, sizeof out));
  CT_CHECK(strstr(out, "deep") != nullptr);

  // Not Discord's shape: nothing to say, and the buffer is left a valid empty string.
  CT_CHECK(!ccord_live_json_error_message("<html>502</html>", 16, out, sizeof out));
  CT_CHECK(out[0] == '\0');
  CT_CHECK(!ccord_live_json_error_message("{\"code\":1}", 10, out, sizeof out));
  CT_CHECK(!ccord_live_json_error_message(nullptr, 0, out, sizeof out));

  // A long message is truncated safely, never overrun.
  char small[12];
  CT_CHECK(ccord_live_json_error_message(perm, strlen(perm), small, sizeof small));
  CT_CHECK(strlen(small) < sizeof small);
}

static const ccord_notify_scope *find_notify(const ccord_net_event *e, ccord_id g, ccord_id c)
{
  for (size_t i = 0; i < e->notify_count; i++) {
    if (e->notify[i].guild_id == g && e->notify[i].channel_id == c) return &e->notify[i];
  }
  return nullptr;
}

// READY's user_guild_settings and the gateway's USER_GUILD_SETTINGS_UPDATE: mute (timed or open ended), level,
// suppress flags and per-channel overrides, for servers and for direct messages.
static void test_gateway_notify_settings(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *ready_a =
      "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
      "\"guilds\":[{\"id\":\"77\",\"name\":\"HQ\",\"channels\":[{\"id\":\"11\",\"type\":0,\"name\":\"general\"}],\"members\":[]}],"
      "\"user_guild_settings\":{\"version\":3,\"partial\":false,\"entries\":["
      "{\"guild_id\":\"77\",\"message_notifications\":1,\"muted\":true,"
      "\"mute_config\":{\"selected_time_window\":3600,\"end_time\":\"2026-11-10T12:30:00.000Z\"},"
      "\"suppress_everyone\":true,\"suppress_roles\":false,\"channel_overrides\":["
      "{\"channel_id\":\"11\",\"message_notifications\":2,\"muted\":false,\"mute_config\":null},"
      "{\"channel_id\":\"12\",\"message_notifications\":3,\"muted\":true,\"mute_config\":{\"end_time\":null}},"
      "{\"channel_id\":\"oops\",\"muted\":true},"
      "{\"muted\":true}]},"
      "{\"guild_id\":null,\"message_notifications\":3,\"muted\":false,\"channel_overrides\":["
      "{\"channel_id\":\"900\",\"muted\":true,\"mute_config\":{\"end_time\":\"2026-11-10T12:30:00Z\"}}]},"
      "{\"guild_id\":\"not-an-id\",\"muted\":true},"
      "{\"message_notifications\":0,\"muted\":true}]}}}";
  CT_CHECK(ccord_live_gateway_decode(ready_a, strlen(ready_a), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ev.ev.notify_count == 5);                           // 77 + 2 good overrides, DMs + 1 override; the bad entries are gone
  const ccord_notify_scope *srv = find_notify(&ev.ev, 77, 0);
  CT_CHECK(srv && srv->muted && srv->level == CCORD_NOTIFY_MENTIONS && srv->suppress_everyone && !srv->suppress_roles);
  CT_CHECK(srv && srv->mute_until_ms == 1794313800000LL);    // 2026-11-10T12:30:00Z
  const ccord_notify_scope *c11 = find_notify(&ev.ev, 77, 11);
  CT_CHECK(c11 && !c11->muted && c11->level == CCORD_NOTIFY_NOTHING && c11->mute_until_ms == 0);
  const ccord_notify_scope *c12 = find_notify(&ev.ev, 77, 12);
  CT_CHECK(c12 && c12->muted && c12->mute_until_ms == 0 && c12->level == CCORD_NOTIFY_INHERIT);   // until turned back on
  const ccord_notify_scope *dm = find_notify(&ev.ev, CCORD_DM_GUILD_ID, 0);
  CT_CHECK(dm && !dm->muted && dm->level == CCORD_NOTIFY_INHERIT);                                 // guild_id null = direct messages
  const ccord_notify_scope *dm900 = find_notify(&ev.ev, CCORD_DM_GUILD_ID, 900);
  CT_CHECK(dm900 && dm900->muted && dm900->mute_until_ms == 1794313800000LL);
  CT_CHECK(find_notify(&ev.ev, 77, 0) < find_notify(&ev.ev, 77, 12));                              // rows stay grouped per server
  ccord_live_event_free(&ev);

  // The same settings as a bare array (older gateway shape).
  const char *ready_b =
      "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"guilds\":[],"
      "\"user_guild_settings\":[{\"guild_id\":\"77\",\"message_notifications\":0,\"muted\":false}]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_b, strlen(ready_b), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.notify_count == 1 && ev.ev.notify[0].guild_id == 77 && ev.ev.notify[0].level == CCORD_NOTIFY_ALL);
  ccord_live_event_free(&ev);

  // No settings in the payload: nothing is claimed.
  const char *ready_c = "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"guilds\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_c, strlen(ready_c), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.notify_count == 0 && ev.ev.notify == nullptr);
  ccord_live_event_free(&ev);

  // A muted row with an end time that cannot be read is open ended; an unmuted row ignores its end time.
  const char *ready_d =
      "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"guilds\":[],"
      "\"user_guild_settings\":[{\"guild_id\":\"5\",\"muted\":true,\"mute_config\":{\"end_time\":\"soon\"}},"
      "{\"guild_id\":\"6\",\"muted\":false,\"mute_config\":{\"end_time\":\"2026-11-10T12:30:00Z\"}}]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_d, strlen(ready_d), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.notify_count == 2 && ev.ev.notify[0].muted && ev.ev.notify[0].mute_until_ms == 0);
  CT_CHECK(!ev.ev.notify[1].muted && ev.ev.notify[1].mute_until_ms == 0);
  // No level in the entry means none was chosen: "use the default", never "All Messages" by accident.
  CT_CHECK(ev.ev.notify[0].level == CCORD_NOTIFY_INHERIT && ev.ev.notify[1].level == CCORD_NOTIFY_INHERIT);
  ccord_live_event_free(&ev);

  // USER_GUILD_SETTINGS_UPDATE carries one server whole.
  const char *upd =
      "{\"op\":0,\"t\":\"USER_GUILD_SETTINGS_UPDATE\",\"d\":{\"guild_id\":\"77\",\"message_notifications\":2,\"muted\":false,"
      "\"suppress_roles\":true,\"channel_overrides\":[{\"channel_id\":\"11\",\"muted\":true,\"mute_config\":null}]}}";
  CT_CHECK(ccord_live_gateway_decode(upd, strlen(upd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_NOTIFY_SETTINGS && ev.ev.guild_id == 77 && ev.ev.notify_count == 2);
  CT_CHECK(ev.ev.notify[0].channel_id == 0 && ev.ev.notify[0].level == CCORD_NOTIFY_NOTHING && ev.ev.notify[0].suppress_roles &&
           !ev.ev.notify[0].suppress_everyone);
  CT_CHECK(ev.ev.notify[1].channel_id == 11 && ev.ev.notify[1].muted && ev.ev.notify[1].mute_until_ms == 0);
  ccord_live_event_free(&ev);
  const char *upd_dm = "{\"op\":0,\"t\":\"USER_GUILD_SETTINGS_UPDATE\",\"d\":{\"guild_id\":null,\"muted\":true}}";
  CT_CHECK(ccord_live_gateway_decode(upd_dm, strlen(upd_dm), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_NOTIFY_SETTINGS && ev.ev.guild_id == CCORD_DM_GUILD_ID && ev.ev.notify_count == 1 &&
           ev.ev.notify[0].muted);
  ccord_live_event_free(&ev);
  const char *upd_bad = "{\"op\":0,\"t\":\"USER_GUILD_SETTINGS_UPDATE\",\"d\":{\"muted\":true}}";
  CT_CHECK(ccord_live_gateway_decode(upd_bad, strlen(upd_bad), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);
}

static void test_polls_components_and_modals_json(void)
{
  ccord_error err;
  ccord_live_event ev;

  // 1. Message with components: buttons, selects, emoji, disabled, default option, invalid items skipped, application_id.
  const char *msg_json =
      "{\"id\":\"900\",\"channel_id\":\"10\",\"application_id\":\"555001\","
      "\"author\":{\"id\":\"42\",\"username\":\"HelperBot\",\"bot\":true},"
      "\"content\":\"\",\"timestamp\":\"2026-11-10T12:00:00Z\","
      "\"components\":["
      "{\"type\":1,\"components\":["
      "{\"type\":2,\"style\":1,\"label\":\"Confirm\",\"custom_id\":\"act:yes\",\"emoji\":{\"name\":\"ok\",\"id\":\"701\"}},"
      "{\"type\":2,\"style\":5,\"label\":\"Docs\",\"url\":\"https://example.com\",\"disabled\":true},"
      "{\"type\":2,\"style\":6},"
      "{\"type\":2,\"style\":5,\"label\":\"BrokenLink\"},"
      "{\"type\":2,\"style\":1,\"label\":\"BrokenBtn\"},"
      "{\"type\":4,\"custom_id\":\"modal_only\"}"
      "]},"
      "{\"type\":1,\"components\":["
      "{\"type\":3,\"custom_id\":\"sel:lang\",\"placeholder\":\"Language\",\"min_values\":0,\"max_values\":2,"
      "\"options\":["
      "{\"label\":\"C23\",\"value\":\"c23\",\"description\":\"Systems\",\"default\":true,\"emoji\":{\"name\":\"gear\",\"id\":\"702\"}},"
      "{\"value\":\"zig\"},"
      "{\"label\":\"EmptyVal\",\"value\":\"\"}"
      "]}"
      "]},"
      "{\"type\":1,\"components\":["
      "{\"type\":6,\"custom_id\":\"sel:role\",\"min_values\":1,\"max_values\":3}"
      "]}"
      "]}";
  CT_CHECK(ccord_live_json_message(msg_json, strlen(msg_json), CCORD_NET_MESSAGE_CREATE, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.message_count == 1 && ev.messages[0].rich != nullptr);
  const ccord_message_rich *r = ev.messages[0].rich;
  CT_CHECK(r->application_id == 555001 && r->component_row_count == 3);
  CT_CHECK(r->components[0].component_count == 3);
  CT_CHECK(r->components[0].components[0].kind == CCORD_COMP_BUTTON &&
           r->components[0].components[0].style == CCORD_BTN_PRIMARY &&
           strcmp(r->components[0].components[0].custom_id, "act:yes") == 0 &&
           strcmp(r->components[0].components[0].emoji, "ok") == 0 &&
           r->components[0].components[0].emoji_id == 701 &&
           !r->components[0].components[0].disabled);
  CT_CHECK(r->components[0].components[1].style == CCORD_BTN_LINK &&
           strcmp(r->components[0].components[1].url, "https://example.com") == 0 &&
           r->components[0].components[1].disabled);
  CT_CHECK(r->components[0].components[2].style == CCORD_BTN_PREMIUM);
  const ccord_component *sel = &r->components[1].components[0];
  CT_CHECK(sel->kind == CCORD_COMP_STRING_SELECT && sel->min_values == 0 && sel->max_values == 2 && sel->option_count == 2);
  CT_CHECK(strcmp(sel->options[0].label, "C23") == 0 && strcmp(sel->options[0].value, "c23") == 0 &&
           strcmp(sel->options[0].description, "Systems") == 0 && sel->options[0].default_ &&
           sel->options[0].emoji_id == 702);
  CT_CHECK(strcmp(sel->options[1].label, "zig") == 0 && strcmp(sel->options[1].value, "zig") == 0 && !sel->options[1].default_);
  CT_CHECK(r->components[2].components[0].kind == CCORD_COMP_ROLE_SELECT &&
           r->components[2].components[0].min_values == 1 && r->components[2].components[0].max_values == 3);
  CT_CHECK(ccord_message_rich_validate(r, &err) == CCORD_OK);
  ccord_live_event_free(&ev);

  // Bot author fallback when application_id is absent vs non-bot author leaving application_id == 0.
  const char *bot_fallback =
      "{\"id\":\"901\",\"channel_id\":\"10\",\"author\":{\"id\":\"888\",\"username\":\"Bot\",\"bot\":true},"
      "\"content\":\"hi\",\"timestamp\":\"2026-11-10T12:00:00Z\","
      "\"components\":[{\"type\":1,\"components\":[{\"type\":2,\"style\":2,\"label\":\"Ping\",\"custom_id\":\"ping\"}]}]}";
  CT_CHECK(ccord_live_json_message(bot_fallback, strlen(bot_fallback), CCORD_NET_MESSAGE_CREATE, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->application_id == 888);
  ccord_live_event_free(&ev);

  // Partial MESSAGE_UPDATE clearing components sets CCORD_RICH_HAS_COMPONENTS and text_absent.
  const char *comp_clear_upd =
      "{\"op\":0,\"t\":\"MESSAGE_UPDATE\",\"d\":{\"id\":\"901\",\"channel_id\":\"10\",\"components\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(comp_clear_upd, strlen(comp_clear_upd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_UPDATE && ev.messages[0].rich != nullptr);
  CT_CHECK((ev.messages[0].rich->present & CCORD_RICH_HAS_COMPONENTS) != 0 &&
           ev.messages[0].rich->component_row_count == 0 && ev.messages[0].rich->text_absent);
  ccord_live_event_free(&ev);

  // 2. MESSAGE_POLL_VOTE_ADD / REMOVE gateway events.
  const char *pv_add =
      "{\"op\":0,\"t\":\"MESSAGE_POLL_VOTE_ADD\",\"d\":{\"user_id\":\"42\",\"channel_id\":\"10\","
      "\"message_id\":\"500\",\"guild_id\":\"7\",\"answer_id\":3}}";
  CT_CHECK(ccord_live_gateway_decode(pv_add, strlen(pv_add), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_POLL_VOTE && ev.ev.user_id == 42 && ev.ev.channel_id == 10 &&
           ev.ev.message_id == 500 && ev.ev.guild_id == 7 && ev.ev.poll_answer_id == 3 && ev.ev.poll_vote_added);
  ccord_live_event_free(&ev);

  const char *pv_rem =
      "{\"op\":0,\"t\":\"MESSAGE_POLL_VOTE_REMOVE\",\"d\":{\"user_id\":\"42\",\"channel_id\":\"10\","
      "\"message_id\":\"500\",\"answer_id\":3}}";
  CT_CHECK(ccord_live_gateway_decode(pv_rem, strlen(pv_rem), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_POLL_VOTE && ev.ev.guild_id == 0 && ev.ev.poll_answer_id == 3 && !ev.ev.poll_vote_added);
  ccord_live_event_free(&ev);

  const char *pv_bad = "{\"op\":0,\"t\":\"MESSAGE_POLL_VOTE_ADD\",\"d\":{\"user_id\":\"42\",\"channel_id\":\"10\",\"message_id\":\"500\",\"answer_id\":0}}";
  CT_CHECK(ccord_live_gateway_decode(pv_bad, strlen(pv_bad), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);

  // 3. INTERACTION_MODAL_CREATE gateway events (nested d.data and flat d).
  const char *modal_gw =
      "{\"op\":0,\"t\":\"INTERACTION_MODAL_CREATE\",\"d\":{\"id\":\"9901\",\"application_id\":\"555001\","
      "\"guild_id\":\"7\",\"channel_id\":\"10\",\"data\":{\"custom_id\":\" ticket:open \",\"title\":\"New Ticket\","
      "\"components\":["
      "{\"type\":1,\"components\":[{\"type\":4,\"custom_id\":\"subj\",\"label\":\"Subject\",\"style\":1,\"min_length\":3,\"max_length\":100}]},"
      "{\"type\":1,\"components\":[{\"type\":4,\"custom_id\":\"body\",\"label\":\"Details\",\"placeholder\":\"Optional\","
      "\"value\":\"init\",\"style\":2,\"required\":false}]}"
      "]}}}";
  CT_CHECK(ccord_live_gateway_decode(modal_gw, strlen(modal_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_INTERACTION_MODAL && ev.ev.modal != nullptr);
  if (ev.ev.modal) {
    CT_CHECK(ev.ev.modal->id == 9901 && ev.ev.modal->application_id == 555001 &&
             ev.ev.modal->guild_id == 7 && ev.ev.modal->channel_id == 10);
    CT_CHECK(strcmp(ev.ev.modal->title, "New Ticket") == 0 && ev.ev.modal->field_count == 2);
    CT_CHECK(strcmp(ev.ev.modal->fields[0].custom_id, "subj") == 0 && ev.ev.modal->fields[0].style == 1 &&
             ev.ev.modal->fields[0].min_length == 3 && ev.ev.modal->fields[0].max_length == 100 &&
             ev.ev.modal->fields[0].required);
    CT_CHECK(strcmp(ev.ev.modal->fields[1].custom_id, "body") == 0 && ev.ev.modal->fields[1].style == 2 &&
             strcmp(ev.ev.modal->fields[1].value, "init") == 0 && !ev.ev.modal->fields[1].required);
  }
  ccord_live_event_free(&ev);

  const char *modal_bad =
      "{\"op\":0,\"t\":\"INTERACTION_MODAL_CREATE\",\"d\":{\"application_id\":\"555\",\"custom_id\":\"m\",\"title\":\"Empty\",\"components\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(modal_bad, strlen(modal_bad), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);

  // 4. Wire JSON builders: poll votes & interactions.
  ccord_buf b;
  ccord_buf_init(&b, 0);
  CT_CHECK(ccord_live_build_poll_vote_body(nullptr, 0, &b, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b), "{\"answer_ids\":[]}") == 0);
  ccord_buf_clear(&b);
  uint32_t aids[2] = {1, 4};
  CT_CHECK(ccord_live_build_poll_vote_body(aids, 2, &b, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b), "{\"answer_ids\":[1,4]}") == 0);
  uint32_t zero_aid = 0;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_poll_vote_body(&zero_aid, 1, &b, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_build_poll_vote_body(aids, CCORD_RICH_MAX_POLL_ANSWERS + 1, &b, &err) == CCORD_ERR_RANGE);

  ccord_buf_clear(&b);
  ccord_interaction_request btn_req = {
    .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
    .application_id = 555, .guild_id = 7, .channel_id = 10, .message_id = 500, .message_flags = 64,
    .component_type = CCORD_COMP_BUTTON, .custom_id = "act:yes",
  };
  CT_CHECK(ccord_live_build_interaction_body(&btn_req, "sess1", "999", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"type\":3") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"guild_id\":\"7\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"component_type\":2") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"values\"") == nullptr);

  ccord_buf_clear(&b);
  const char *svals[2] = {"a\"b", "c"};
  ccord_interaction_request sel_req = {
    .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
    .application_id = 555, .guild_id = CCORD_DM_GUILD_ID, .channel_id = 10, .message_id = 500,
    .component_type = CCORD_COMP_STRING_SELECT, .custom_id = "sel:x", .values = svals, .value_count = 2,
  };
  CT_CHECK(ccord_live_build_interaction_body(&sel_req, "sess1", "1000", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"guild_id\"") == nullptr &&
           strstr(ccord_buf_cstr(&b), "\"values\":[\"a\\\"b\",\"c\"]") != nullptr);

  ccord_buf_clear(&b);
  const char *fids[2] = {"subj", "body"};
  const char *fvals[2] = {"Hello", "Line 1\nLine 2"};
  ccord_interaction_request mod_req = {
    .type = CCORD_INTERACTION_MODAL_SUBMIT,
    .application_id = 555, .guild_id = 7, .channel_id = 10,
    .custom_id = "ticket:open", .modal_id = 9901,
    .field_ids = fids, .field_values = fvals, .field_count = 2,
  };
  CT_CHECK(ccord_live_build_interaction_body(&mod_req, "sess1", "1001", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"type\":5") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"id\":\"9901\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"type\":4,\"custom_id\":\"subj\",\"value\":\"Hello\"") != nullptr);
  ccord_buf_free(&b);
}

static void test_slash_commands_ephemeral_reply_and_uploads_json(void)
{
  ccord_error err;
  ccord_buf b;
  ccord_buf_init(&b, 0);

  // 1. Reply builder: mention_author true vs false ("replied_user":false).
  CT_CHECK(ccord_live_build_reply_body("hello", 5, 77, true, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"message_id\":\"77\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"replied_user\"") == nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_reply_body("quiet", 5, 77, false, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"message_id\":\"77\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"allowed_mentions\":{\"replied_user\":false}") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_reply_body("bad", 3, CCORD_ID_NONE, true, &b, &err) == CCORD_ERR_INVALID);

  // 2. Multi-file upload slots + final message body builders (SPOILER_ prefix idempotent).
  static const uint8_t k_dummy_bytes[4] = {1, 2, 3, 4};
  ccord_upload_file ufiles[3] = {
    {.filename = "a.png", .data = k_dummy_bytes, .size = 120, .spoiler = false},
    {.filename = "b.png", .data = k_dummy_bytes, .size = 240, .spoiler = true},
    {.filename = "SPOILER_c.png", .data = k_dummy_bytes, .size = 360, .spoiler = true},
  };
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_slots_body(ufiles, 3, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"filename\":\"a.png\",\"file_size\":120,\"id\":\"0\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"filename\":\"SPOILER_b.png\",\"file_size\":240,\"id\":\"1\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"filename\":\"SPOILER_c.png\",\"file_size\":360,\"id\":\"2\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "SPOILER_SPOILER_") == nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_slots_body(ufiles, 0, &b, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_build_upload_slots_body(ufiles, CCORD_RICH_MAX_ATTACHMENTS + 1, &b, &err) == CCORD_ERR_RANGE);

  char up_names[3][256] = {"up/0/a.png", "up/1/SPOILER_b.png", "up/2/SPOILER_c.png"};
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_message_body("", 0, 88, false, ufiles, up_names, 3, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"content\":\"\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"message_id\":\"88\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"replied_user\":false") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"id\":\"0\",\"filename\":\"a.png\",\"uploaded_filename\":\"up/0/a.png\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"id\":\"1\",\"filename\":\"SPOILER_b.png\",\"uploaded_filename\":\"up/1/SPOILER_b.png\"") != nullptr);

  // 2b. Voice upload: duration_secs + waveform on the attachment and flags 8192 on the message; an ordinary upload
  //     body carries none of them (the checks above already pin its exact attachment text).
  CT_CHECK(strstr(ccord_buf_cstr(&b), "duration_secs") == nullptr && strstr(ccord_buf_cstr(&b), "waveform") == nullptr &&
           strstr(ccord_buf_cstr(&b), "\"flags\"") == nullptr);
  ccord_upload_file vfile = {.filename = "voice-message.ogg", .data = k_dummy_bytes, .size = 4,
                             .duration_ms = 2500, .waveform_b64 = "AAECAwQFBgcICQ=="};
  char vnames[1][256] = {"up/0/voice-message.ogg"};
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_message_body("", 0, CCORD_ID_NONE, true, &vfile, vnames, 1, &b, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b),
                  "{\"content\":\"\",\"attachments\":[{\"id\":\"0\",\"filename\":\"voice-message.ogg\","
                  "\"uploaded_filename\":\"up/0/voice-message.ogg\",\"duration_secs\":2.5,"
                  "\"waveform\":\"AAECAwQFBgcICQ==\"}],\"flags\":8192}") == 0);
  vfile.waveform_b64 = nullptr;   // no waveform: Discord draws none, but the message is still a voice message
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_message_body("", 0, CCORD_ID_NONE, true, &vfile, vnames, 1, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"duration_secs\":2.5") != nullptr && strstr(ccord_buf_cstr(&b), "waveform") == nullptr &&
           strstr(ccord_buf_cstr(&b), "\"flags\":8192") != nullptr);
  vfile.waveform_b64 = "not base64!";
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_message_body("", 0, CCORD_ID_NONE, true, &vfile, vnames, 1, &b, &err) == CCORD_ERR_INVALID);
  char long_wf[CCORD_RICH_WAVEFORM_MAX_BYTES + 2];
  memset(long_wf, 'A', sizeof long_wf - 1);
  long_wf[sizeof long_wf - 1] = '\0';
  vfile.waveform_b64 = long_wf;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_message_body("", 0, CCORD_ID_NONE, true, &vfile, vnames, 1, &b, &err) == CCORD_ERR_INVALID);
  // A waveform on an ordinary (duration 0) file is ignored: the body stays the plain upload body.
  vfile.duration_ms = 0;
  vfile.waveform_b64 = "AAEC";
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_message_body("", 0, CCORD_ID_NONE, true, &vfile, vnames, 1, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "waveform") == nullptr && strstr(ccord_buf_cstr(&b), "\"flags\"") == nullptr);

  // 3. Slash command & autocomplete interaction JSON builders (type 2 & type 4).
  const char *onames[5] = {"count", "ratio", "quiet", "text", "target"};
  uint8_t okinds[5] = {CCORD_SLASH_OPT_INTEGER, CCORD_SLASH_OPT_NUMBER, CCORD_SLASH_OPT_BOOLEAN,
                       CCORD_SLASH_OPT_STRING, CCORD_SLASH_OPT_USER};
  const char *ovals[5] = {"42", "3.5", "true", "hi \"there\"", "1002"};
  ccord_interaction_request sreq = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 777,
    .guild_id = 5,
    .channel_id = 10,
    .command_id = 9101,
    .command_version = 0, // falls back to command_id
    .command_name = "run",
    .option_names = onames,
    .option_kinds = okinds,
    .option_values = ovals,
    .option_count = 5,
    .focused_option = -1,
  };
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&sreq, "sess1", "2001", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"type\":2") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"guild_id\":\"5\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"version\":\"9101\",\"id\":\"9101\",\"name\":\"run\",\"type\":1") != nullptr &&
           strstr(ccord_buf_cstr(&b), "{\"type\":4,\"name\":\"count\",\"value\":42}") != nullptr &&
           strstr(ccord_buf_cstr(&b), "{\"type\":10,\"name\":\"ratio\",\"value\":3.5}") != nullptr &&
           strstr(ccord_buf_cstr(&b), "{\"type\":5,\"name\":\"quiet\",\"value\":true}") != nullptr &&
           strstr(ccord_buf_cstr(&b), "{\"type\":3,\"name\":\"text\",\"value\":\"hi \\\"there\\\"\"}") != nullptr &&
           strstr(ccord_buf_cstr(&b), "{\"type\":6,\"name\":\"target\",\"value\":\"1002\"}") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"focused\"") == nullptr);

  // Type 4 autocomplete marks only focused_option with "focused":true, omits guild_id in DMs.
  sreq.type = CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE;
  sreq.guild_id = CCORD_DM_GUILD_ID;
  sreq.focused_option = 3;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&sreq, "sess1", "2002", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"type\":4") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"guild_id\"") == nullptr &&
           strstr(ccord_buf_cstr(&b), "{\"type\":3,\"name\":\"text\",\"value\":\"hi \\\"there\\\"\",\"focused\":true}") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"name\":\"count\",\"value\":42,\"focused\"") == nullptr);
  sreq.focused_option = 5;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&sreq, "sess1", "2003", &b, &err) == CCORD_ERR_INVALID);
  ccord_buf_free(&b);

  // 4. Multi-slot upload response parser (including out-of-order numeric and string slot IDs).
  const char *slots_json =
      "{\"attachments\":["
      "{\"id\":\"2\",\"upload_url\":\"https://upload.discord.com/put/2\",\"upload_filename\":\"buck/2\"},"
      "{\"id\":1,\"upload_url\":\"https://upload.discord.com/put/1\",\"upload_filename\":\"buck/1\"},"
      "{\"id\":\"0\",\"upload_url\":\"https://upload.discord.com/put/0\",\"upload_filename\":\"buck/0\"}"
      "]}";
  char urls[4][2048], names[4][256];
  CT_CHECK(ccord_live_json_upload_slots(slots_json, strlen(slots_json), 3, urls, names, &err) == CCORD_OK);
  CT_CHECK(strcmp(urls[0], "https://upload.discord.com/put/0") == 0 && strcmp(names[0], "buck/0") == 0);
  CT_CHECK(strcmp(urls[1], "https://upload.discord.com/put/1") == 0 && strcmp(names[1], "buck/1") == 0);
  CT_CHECK(strcmp(urls[2], "https://upload.discord.com/put/2") == 0 && strcmp(names[2], "buck/2") == 0);
  CT_CHECK(ccord_live_json_upload_slots(slots_json, strlen(slots_json), 4, urls, names, &err) != CCORD_OK);

  // 5. Slash command index decoder (object & array, CHAT_INPUT + USER/MESSAGE context commands,
  // default_member_permissions, dm_permission, options).
  const char *idx_json =
      "{\"application_commands\":["
      "{\"id\":\"9101\",\"application_id\":\"777\",\"type\":1,\"name\":\"ping\",\"description\":\"Check latency\","
      "\"default_member_permissions\":null,\"dm_permission\":true},"
      "{\"id\":\"9102\",\"application_id\":\"777\",\"type\":2,\"name\":\"UserInfo\",\"description\":\"Context menu\"},"
      "{\"id\":\"9103\",\"application_id\":\"777\",\"guild_id\":\"5\",\"version\":\"9999\",\"type\":1,"
      "\"name\":\"purge\",\"description\":\"Mod purge\",\"default_member_permissions\":\"8192\",\"dm_permission\":false,"
      "\"options\":["
      "{\"type\":4,\"name\":\"count\",\"description\":\"How many\",\"required\":true},"
      "{\"type\":3,\"name\":\"topic\",\"description\":\"Filter\",\"required\":false,\"autocomplete\":true}"
      "]},"
      "{\"id\":\"9104\",\"application_id\":\"777\",\"type\":1,\"name\":\"rootonly\",\"description\":\"Admin only\","
      "\"default_member_permissions\":\"0\"}"
      "]}";
  ccord_live_event ev;
  CT_CHECK(ccord_live_json_slash_commands(idx_json, strlen(idx_json), 5, 10, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SLASH_COMMANDS && ev.ev.guild_id == 5 && ev.ev.channel_id == 10 &&
           ev.ev.slash_command_count == 4);
  if (ev.ev.slash_command_count == 4) {
    CT_CHECK(strcmp(ev.ev.slash_commands[0].name, "ping") == 0 &&
             ev.ev.slash_commands[0].command_type == 1 &&
             ev.ev.slash_commands[0].default_member_permissions == UINT64_MAX &&
             ev.ev.slash_commands[0].dm_permission &&
             ev.ev.slash_commands[0].version == 9101);
    CT_CHECK(strcmp(ev.ev.slash_commands[1].name, "UserInfo") == 0 &&
             ev.ev.slash_commands[1].command_type == 2 &&
             ev.ev.slash_commands[1].option_count == 0);
    CT_CHECK(strcmp(ev.ev.slash_commands[2].name, "purge") == 0 &&
             ev.ev.slash_commands[2].version == 9999 &&
             ev.ev.slash_commands[2].default_member_permissions == 8192u &&
             !ev.ev.slash_commands[2].dm_permission &&
             ev.ev.slash_commands[2].option_count == 2 &&
             ev.ev.slash_commands[2].options[0].kind == CCORD_SLASH_OPT_INTEGER &&
             ev.ev.slash_commands[2].options[0].required &&
             !ev.ev.slash_commands[2].options[0].autocomplete &&
             ev.ev.slash_commands[2].options[1].kind == CCORD_SLASH_OPT_STRING &&
             ev.ev.slash_commands[2].options[1].autocomplete);
    CT_CHECK(strcmp(ev.ev.slash_commands[3].name, "rootonly") == 0 &&
             ev.ev.slash_commands[3].default_member_permissions == 0u);
  }
  ccord_live_event_free(&ev);

  // 6. Gateway APPLICATION_COMMAND_AUTOCOMPLETE_RESULT and ephemeral MESSAGE_CREATE (flags: 64).
  const char *ac_gw =
      "{\"op\":0,\"t\":\"APPLICATION_COMMAND_AUTOCOMPLETE_RESULT\",\"d\":{\"nonce\":\"n-42\",\"choices\":["
      "{\"name\":\"SDL3 GPU\",\"value\":\"sdl3\"},"
      "{\"name\":\"Answer\",\"value\":42}"
      "]}}";
  CT_CHECK(ccord_live_gateway_decode(ac_gw, strlen(ac_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SLASH_AUTOCOMPLETE && ev.ev.slash_choice_count == 2 &&
           ev.ev.interaction_nonce && strcmp(ev.ev.interaction_nonce, "n-42") == 0);
  if (ev.ev.slash_choice_count == 2) {
    CT_CHECK(strcmp(ev.ev.slash_choices[0].name, "SDL3 GPU") == 0 &&
             strcmp(ev.ev.slash_choices[0].value, "sdl3") == 0);
    CT_CHECK(strcmp(ev.ev.slash_choices[1].name, "Answer") == 0 &&
             strcmp(ev.ev.slash_choices[1].value, "42") == 0);
  }
  ccord_live_event_free(&ev);

  const char *ephem_gw =
      "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"999\",\"channel_id\":\"10\",\"type\":20,\"flags\":64,"
      "\"author\":{\"id\":\"777\",\"username\":\"Bot\",\"bot\":true},\"content\":\"Only for you\","
      "\"timestamp\":\"2026-11-10T12:00:00Z\"}}";
  CT_CHECK(ccord_live_gateway_decode(ephem_gw, strlen(ephem_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_CREATE && ev.message_count == 1 &&
           ccord_message_is_ephemeral(&ev.messages[0]));
  ccord_live_event_free(&ev);
}

static void test_search_pins_threads_and_burst_reactions_json(void)
{
  ccord_error err;
  ccord_live_event ev;

  // 1. ccord_live_build_search_path: guild vs DM, all operators, offset, empty rejection.
  ccord_search_query sq;
  ccord_search_parse("has:image has:poll pinned:true hello world", 42, &sq);
  sq.author_id = 101;
  sq.mentions_id = 202;
  sq.channel_id = 303;
  sq.min_id = 404;
  sq.max_id = 505;
  sq.offset = 50;
  ccord_buf path;
  ccord_buf_init(&path, 512);
  CT_CHECK(ccord_live_build_search_path(9001, CCORD_ID_NONE, &sq, &path, &err) == CCORD_OK);
  const char *ps = ccord_buf_cstr(&path);
  CT_CHECK(strncmp(ps, "/guilds/9001/messages/search?", 29) == 0);
  CT_CHECK(strstr(ps, "content=hello%20world") != nullptr);
  CT_CHECK(strstr(ps, "channel_id=303") != nullptr);
  CT_CHECK(strstr(ps, "author_id=101") != nullptr);
  CT_CHECK(strstr(ps, "mentions=202") != nullptr);
  CT_CHECK(strstr(ps, "has=image") != nullptr && strstr(ps, "has=poll") != nullptr);
  CT_CHECK(strstr(ps, "min_id=404") != nullptr && strstr(ps, "max_id=505") != nullptr);
  CT_CHECK(strstr(ps, "pinned=true") != nullptr && strstr(ps, "offset=50") != nullptr);
  ccord_buf_free(&path);

  ccord_buf_init(&path, 256);
  ccord_search_parse("dm query", 8, &sq);
  CT_CHECK(ccord_live_build_search_path(CCORD_DM_GUILD_ID, 777, &sq, &path, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&path), "/channels/777/messages/search?content=dm%20query") == 0);
  ccord_buf_free(&path);

  ccord_buf_init(&path, 256);
  ccord_search_parse("", 0, &sq);
  CT_CHECK(ccord_live_build_search_path(9001, CCORD_ID_NONE, &sq, &path, &err) == CCORD_ERR_INVALID);
  ccord_buf_free(&path);

  // 2. ccord_live_json_search: total_results & flattened hits.
  const char *sjson =
      "{\"total_results\":137,\"messages\":[[{\"id\":\"1\",\"channel_id\":\"10\","
      "\"author\":{\"id\":\"2\",\"username\":\"ada\"},\"content\":\"hit 1\","
      "\"timestamp\":\"2026-11-10T00:00:00Z\"}]]}";
  CT_CHECK(ccord_live_json_search(sjson, strlen(sjson), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SEARCH_RESULTS && ev.ev.search_total_results == 137 &&
           ev.message_count == 1 && ev.messages[0].id == 1);
  ccord_live_event_free(&ev);

  // 3. ccord_live_json_pins: modern {items, has_more} + cursor AND legacy array.
  const char *pjson_new =
      "{\"items\":["
      "{\"pinned_at\":\"2026-11-10T05:00:00.000Z\",\"message\":{\"id\":\"12\",\"channel_id\":\"10\",\"pinned\":true,"
      "\"author\":{\"id\":\"2\",\"username\":\"ada\"},\"content\":\"p1\",\"timestamp\":\"2026-11-10T01:00:00Z\"}},"
      "{\"pinned_at\":\"2026-11-09T04:00:00.000Z\",\"message\":{\"id\":\"11\",\"channel_id\":\"10\",\"pinned\":true,"
      "\"author\":{\"id\":\"2\",\"username\":\"ada\"},\"content\":\"p2\",\"timestamp\":\"2026-11-09T01:00:00Z\"}}"
      "],\"has_more\":true}";
  CT_CHECK(ccord_live_json_pins(pjson_new, strlen(pjson_new), 10, false, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_PIN_LIST && ev.message_count == 2 && ev.ev.pins_has_more &&
           ev.ev.pins_next_cursor && strcmp(ev.ev.pins_next_cursor, "2026-11-09T04:00:00.000Z") == 0);
  ccord_live_event_free(&ev);

  const char *pjson_old =
      "[{\"id\":\"12\",\"channel_id\":\"10\",\"pinned\":true,"
      "\"author\":{\"id\":\"2\",\"username\":\"ada\"},\"content\":\"p1\",\"timestamp\":\"2026-11-10T01:00:00Z\"}]";
  CT_CHECK(ccord_live_json_pins(pjson_old, strlen(pjson_old), 10, true, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_PIN_LIST && ev.message_count == 1 && ev.ev.pins_append && !ev.ev.pins_has_more);
  ccord_live_event_free(&ev);

  // 4. ccord_live_json_reaction_users.
  const char *ru_json =
      "[{\"id\":\"2\",\"username\":\"ada\",\"global_name\":\"Ada Lovelace\"},"
      "{\"id\":\"3\",\"username\":\"bot\",\"bot\":true}]";
  CT_CHECK(ccord_live_json_reaction_users(ru_json, strlen(ru_json), 10, 500, "\xF0\x9F\x94\xA5", &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_REACTION_USERS && ev.ev.channel_id == 10 && ev.ev.message_id == 500 &&
           ev.user_count == 2 && strcmp(ev.users[0].display_name, "Ada Lovelace") == 0 && ev.users[1].is_bot);
  ccord_live_event_free(&ev);

  // 5. Burst reactions on messages & gateway MESSAGE_REACTION_ADD / REMOVE.
  const char *burst_msg =
      "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"700\",\"channel_id\":\"10\","
      "\"author\":{\"id\":\"2\",\"username\":\"ada\"},\"content\":\"wow\",\"timestamp\":\"2026-11-10T12:00:00Z\","
      "\"reactions\":[{\"count\":5,\"count_details\":{\"burst\":2,\"normal\":3},\"me\":false,\"me_burst\":true,"
      "\"emoji\":{\"id\":null,\"name\":\"\xF0\x9F\x94\xA5\"}}]}}";
  CT_CHECK(ccord_live_gateway_decode(burst_msg, strlen(burst_msg), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.message_count == 1 && ev.messages[0].reaction_kind_count == 1 &&
           ev.messages[0].reactions[0].count == 5 && ev.messages[0].reactions[0].burst_count == 2 &&
           ev.messages[0].reactions[0].me_burst && !ev.messages[0].reactions[0].self_reacted);
  ccord_live_event_free(&ev);

  const char *burst_add =
      "{\"op\":0,\"t\":\"MESSAGE_REACTION_ADD\",\"d\":{\"user_id\":\"42\",\"channel_id\":\"10\","
      "\"message_id\":\"700\",\"burst\":true,\"type\":1,\"emoji\":{\"id\":null,\"name\":\"\xF0\x9F\x94\xA5\"}}}";
  CT_CHECK(ccord_live_gateway_decode(burst_add, strlen(burst_add), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_REACTION_UPDATE && ev.ev.user_id == 42 && ev.ev.reaction_delta == 1 &&
           ev.ev.reaction_burst && ev.ev.self_reacted);
  ccord_live_event_free(&ev);

  // 6. Gateway THREAD_CREATE, THREAD_MEMBER_UPDATE, and THREAD_MEMBERS_UPDATE.
  const char *th_create =
      "{\"op\":0,\"t\":\"THREAD_CREATE\",\"d\":{\"id\":\"8801\",\"guild_id\":\"5\",\"parent_id\":\"10\",\"type\":11,"
      "\"name\":\"release-notes\",\"message_count\":7,"
      "\"thread_metadata\":{\"archived\":true,\"locked\":true,\"auto_archive_duration\":60},"
      "\"member\":{\"user_id\":\"42\"}}}";
  CT_CHECK(ccord_live_gateway_decode(th_create, strlen(th_create), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CHANNEL_UPSERT && ev.ev.channel && ev.ev.channel->id == 8801 &&
           ev.ev.channel->thread_archived && ev.ev.channel->thread_locked &&
           ev.ev.channel->thread_auto_archive_mins == 60 && ev.ev.channel->thread_message_count == 7 &&
           ev.ev.channel->thread_has_member && ev.ev.channel->thread_joined);
  ccord_live_event_free(&ev);

  const char *tm_upd = "{\"op\":0,\"t\":\"THREAD_MEMBER_UPDATE\",\"d\":{\"id\":\"8801\",\"guild_id\":\"5\",\"user_id\":\"42\"}}";
  CT_CHECK(ccord_live_gateway_decode(tm_upd, strlen(tm_upd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_THREAD_MEMBER_SELF && ev.ev.channel_id == 8801 && ev.ev.user_id == 42 && ev.ev.thread_joined);
  ccord_live_event_free(&ev);

  const char *tms_rem =
      "{\"op\":0,\"t\":\"THREAD_MEMBERS_UPDATE\",\"d\":{\"id\":\"8801\",\"guild_id\":\"5\","
      "\"removed_member_ids\":[\"42\"]}}";
  CT_CHECK(ccord_live_gateway_decode(tms_rem, strlen(tms_rem), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_THREAD_MEMBER_SELF && ev.ev.channel_id == 8801 && ev.ev.user_id == 42 && !ev.ev.thread_joined);
  ccord_live_event_free(&ev);
}

static void test_m3b_folders_invite_and_discovery_json(void)
{
  ccord_error err;
  ccord_live_event ev;

  // 1. READY with guild_folders (metadata: folder_id, folder_name, folder_color) + USER_SETTINGS_UPDATE + PROTO + REST.
  const char *ready_gf =
      "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
      "\"guilds\":[{\"id\":\"10\",\"name\":\"Ten\"},{\"id\":\"20\",\"name\":\"Twenty\"},{\"id\":\"30\",\"name\":\"Thirty\"}],"
      "\"user_settings\":{\"guild_folders\":["
      "{\"id\":77,\"name\":\"Core\",\"color\":5793266,\"guild_ids\":[\"30\",\"10\"]},"
      "{\"id\":null,\"name\":null,\"color\":null,\"guild_ids\":[\"20\"]}"
      "]}}}";
  CT_CHECK(ccord_live_gateway_decode(ready_gf, strlen(ready_gf), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT && ev.snapshot.guild_count == 3);
  if (ev.snapshot.guild_count == 3) {
    CT_CHECK(ev.snapshot.guilds[0].id == 30 && ev.snapshot.guilds[0].folder_id == 77 &&
             strcmp(ev.snapshot.guilds[0].folder_name, "Core") == 0 &&
             ev.snapshot.guilds[0].folder_color == 5793266u);
    CT_CHECK(ev.snapshot.guilds[1].id == 10 && ev.snapshot.guilds[1].folder_id == 77 &&
             strcmp(ev.snapshot.guilds[1].folder_name, "Core") == 0);
    CT_CHECK(ev.snapshot.guilds[2].id == 20 && ev.snapshot.guilds[2].folder_id == 0);
  }
  ccord_live_event_free(&ev);

  const char *us_gw =
      "{\"op\":0,\"t\":\"USER_SETTINGS_UPDATE\",\"d\":{\"guild_folders\":["
      "{\"id\":null,\"guild_ids\":[\"20\"]},"
      "{\"id\":88,\"name\":\"Gaming\",\"color\":5763719,\"guild_ids\":[\"10\",\"30\"]}"
      "]}}";
  CT_CHECK(ccord_live_gateway_decode(us_gw, strlen(us_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_LAYOUT && ev.ev.folder_item_count == 3);
  if (ev.ev.folder_item_count == 3) {
    CT_CHECK(ev.ev.folder_items[0].guild_id == 20 && ev.ev.folder_items[0].folder_id == 0);
    CT_CHECK(ev.ev.folder_items[1].guild_id == 10 && ev.ev.folder_items[1].folder_id == 88 &&
             strcmp(ev.ev.folder_items[1].folder_name, "Gaming") == 0 &&
             ev.ev.folder_items[1].folder_color == 5763719u);
    CT_CHECK(ev.ev.folder_items[2].guild_id == 30 && ev.ev.folder_items[2].folder_id == 88);
  }
  ccord_live_event_free(&ev);

  const char *proto_gw =
      "{\"op\":0,\"t\":\"USER_SETTINGS_PROTO_UPDATE\",\"d\":{\"settings\":{\"type\":1,"
      "\"proto\":\"choKFAgeCAoSAgg3GgUKA09wcyIDCP8BCgIIFA==\"}}}";
  CT_CHECK(ccord_live_gateway_decode(proto_gw, strlen(proto_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_LAYOUT && ev.ev.folder_item_count == 3);
  if (ev.ev.folder_item_count == 3) {
    CT_CHECK(ev.ev.folder_items[0].guild_id == 30 && ev.ev.folder_items[0].folder_id == 55 &&
             strcmp(ev.ev.folder_items[0].folder_name, "Ops") == 0 && ev.ev.folder_items[0].folder_color == 255u);
    CT_CHECK(ev.ev.folder_items[1].guild_id == 10 && ev.ev.folder_items[1].folder_id == 55);
    CT_CHECK(ev.ev.folder_items[2].guild_id == 20 && ev.ev.folder_items[2].folder_id == 0);
  }
  ccord_live_event_free(&ev);

  const char *us_rest = "{\"guild_folders\":[{\"id\":99,\"name\":\"Dev\",\"color\":255,\"guild_ids\":[\"10\"]}]}";
  CT_CHECK(ccord_live_json_user_settings(us_rest, strlen(us_rest), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_LAYOUT && ev.ev.folder_item_count == 1 &&
           ev.ev.folder_items[0].folder_id == 99 && ev.ev.folder_items[0].folder_color == 255u);
  ccord_live_event_free(&ev);

  // 2. Invite preview & accept JSON decoder.
  const char *inv_json =
      "{\"code\":\"c23sys\",\"guild\":{\"id\":\"5001\",\"name\":\"C23 Systems\",\"icon\":\"ab12\","
      "\"description\":\"Low-level C23\"},\"channel\":{\"id\":\"6001\",\"name\":\"welcome\"},"
      "\"approximate_presence_count\":42,\"approximate_member_count\":300}";
  CT_CHECK(ccord_live_json_invite_preview(inv_json, strlen(inv_json), false, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_INVITE_PREVIEW && ev.ev.invite_preview != nullptr);
  if (ev.ev.invite_preview) {
    CT_CHECK(strcmp(ev.ev.invite_preview->code, "c23sys") == 0 && ev.ev.invite_preview->guild_id == 5001 &&
             strcmp(ev.ev.invite_preview->guild_name, "C23 Systems") == 0 &&
             strcmp(ev.ev.invite_preview->channel_name, "welcome") == 0 &&
             ev.ev.invite_preview->online_count == 42 && ev.ev.invite_preview->member_count == 300 &&
             strstr(ev.ev.invite_preview->guild_icon_url, "/icons/5001/ab12.png") != nullptr);
  }
  ccord_live_event_free(&ev);

  CT_CHECK(ccord_live_json_invite_preview(inv_json, strlen(inv_json), true, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_INVITE_ACCEPTED && ev.ev.guild_id == 5001 && ev.ev.channel_id == 6001);
  ccord_live_event_free(&ev);

  // 3. Discoverable guilds JSON decoder.
  const char *disc_json =
      "{\"total\":99,\"guilds\":["
      "{\"id\":\"7001\",\"name\":\"Vulkan Hub\",\"description\":\"GPU programming\",\"icon\":\"a_anim\","
      "\"vanity_url_code\":\"vulkan\",\"primary_category_id\":5,"
      "\"approximate_presence_count\":1200,\"approximate_member_count\":8500}"
      "]}";
  CT_CHECK(ccord_live_json_discoverable_guilds(disc_json, strlen(disc_json), "vulkan", 5, 24, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_DISCOVERY_RESULTS && ev.ev.discovery_total == 99 &&
           ev.ev.discovery_category_id == 5 && ev.ev.discovery_offset == 24 &&
           ev.ev.discovery_query && strcmp(ev.ev.discovery_query, "vulkan") == 0 &&
           ev.ev.discovery_count == 1);
  if (ev.ev.discovery_count == 1) {
    CT_CHECK(ev.ev.discovery_guilds[0].guild_id == 7001 && strcmp(ev.ev.discovery_guilds[0].name, "Vulkan Hub") == 0 &&
             strcmp(ev.ev.discovery_guilds[0].vanity_code, "vulkan") == 0 &&
             ev.ev.discovery_guilds[0].primary_category_id == 5 &&
             ev.ev.discovery_guilds[0].online_count == 1200 && ev.ev.discovery_guilds[0].member_count == 8500 &&
             strstr(ev.ev.discovery_guilds[0].icon_url, ".gif") != nullptr);
  }
  ccord_live_event_free(&ev);
}

static void test_m3c_social_presence_requests_and_profile_json(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. READY with Group DM owner_id, message requests (is_message_request/is_spam/consent_status),
  //    personal notes, clan tags, Rich Presence, and Custom Status.
  const char *ready_m3c =
      "{\"op\":0,\"t\":\"READY\",\"d\":{"
      "\"user\":{\"id\":\"42\",\"username\":\"me\",\"primary_guild\":{\"tag\":\"CORE\"}},"
      "\"notes\":{\"501\":\"Met at conference\"},"
      "\"user_settings\":{\"custom_status\":{\"text\":\"Coding C23\",\"emoji_name\":\"\\ud83d\\udd25\","
      "\"expires_at\":\"2026-11-10T14:00:00.000Z\"}},"
      "\"guilds\":[{\"id\":\"100\",\"name\":\"HQ\",\"channels\":[],"
      "\"members\":[{\"user\":{\"id\":\"501\",\"username\":\"ada\",\"clan\":{\"tag\":\"ADA\"}}}],"
      "\"presences\":[{\"user\":{\"id\":\"501\"},\"status\":\"dnd\",\"activities\":["
      "{\"type\":4,\"state\":\"In flow\",\"emoji\":{\"name\":\"\\u26a1\"},\"timestamps\":{\"end\":1794320000000}},"
      "{\"type\":0,\"name\":\"Neovim\",\"details\":\"Editing session.c\",\"state\":\"Workspace C-Cord\","
      "\"timestamps\":{\"start\":1794310000000,\"end\":1794319000000}}"
      "]}]}],"
      "\"private_channels\":["
      "{\"id\":\"9101\",\"type\":3,\"owner_id\":\"501\",\"name\":\"Core Team\","
      "\"recipients\":[{\"id\":\"501\",\"username\":\"ada\"}]},"
      "{\"id\":\"9102\",\"type\":1,\"is_message_request\":true,\"is_spam\":false,"
      "\"recipients\":[{\"id\":\"502\",\"username\":\"stranger\"}]},"
      "{\"id\":\"9103\",\"type\":1,\"consent_status\":1,\"is_spam\":true,"
      "\"recipients\":[{\"id\":\"503\",\"username\":\"spammer\"}]},"
      "{\"id\":\"9104\",\"type\":1,\"is_message_request\":true,\"consent_status\":2,"
      "\"recipients\":[{\"id\":\"504\",\"username\":\"accepted_friend\"}]}"
      "]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_m3c, strlen(ready_m3c), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ev.snapshot.note_count == 1 && ev.snapshot.notes[0].user_id == 501 &&
           strcmp(ev.snapshot.notes[0].note, "Met at conference") == 0);

  const ccord_user *u_me = nullptr, *u_ada = nullptr;
  for (size_t i = 0; i < ev.snapshot.user_count; i++) {
    if (ev.snapshot.users[i].id == 42) u_me = &ev.snapshot.users[i];
    if (ev.snapshot.users[i].id == 501) u_ada = &ev.snapshot.users[i];
  }
  CT_CHECK(u_me && strcmp(u_me->clan_tag, "CORE") == 0 && strcmp(u_me->custom_status_text, "Coding C23") == 0 &&
           u_me->custom_status_expires_ms > 0);
  CT_CHECK(u_ada && strcmp(u_ada->clan_tag, "ADA") == 0 && u_ada->presence == CCORD_PRESENCE_DND &&
           strcmp(u_ada->custom_status_text, "In flow") == 0 && u_ada->custom_status_expires_ms == 1794320000000LL &&
           u_ada->has_activity && u_ada->activity_type == CCORD_ACTIVITY_PLAYING &&
           strcmp(u_ada->activity_name, "Neovim") == 0 &&
           strcmp(u_ada->activity_details, "Editing session.c") == 0 &&
           strcmp(u_ada->activity_state, "Workspace C-Cord") == 0 &&
           u_ada->activity_start_ms == 1794310000000LL && u_ada->activity_end_ms == 1794319000000LL);

  bool saw_9101 = false, saw_9102 = false, saw_9103 = false, saw_9104 = false;
  for (size_t i = 0; i < ev.snapshot.channel_count; i++) {
    const ccord_channel *c = &ev.snapshot.channels[i];
    if (c->id == 9101) { saw_9101 = true; CT_CHECK(c->owner_id == 501 && !c->is_message_request); }
    if (c->id == 9102) { saw_9102 = true; CT_CHECK(c->is_message_request && !c->is_spam); }
    if (c->id == 9103) { saw_9103 = true; CT_CHECK(c->is_message_request && c->is_spam); }
    if (c->id == 9104) { saw_9104 = true; CT_CHECK(!c->is_message_request && !c->is_spam); }
  }
  CT_CHECK(saw_9101 && saw_9102 && saw_9103 && saw_9104);
  ccord_live_event_free(&ev);

  // 2. Gateway CHANNEL_RECIPIENT_ADD / REMOVE, USER_NOTE_UPDATE, USER_SETTINGS_UPDATE custom_status, PRESENCE_UPDATE.
  const char *rec_add =
      "{\"op\":0,\"t\":\"CHANNEL_RECIPIENT_ADD\",\"d\":{\"channel_id\":\"9101\","
      "\"user\":{\"id\":\"505\",\"username\":\"grace\",\"global_name\":\"Grace\"}}}";
  CT_CHECK(ccord_live_gateway_decode(rec_add, strlen(rec_add), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_DM_RECIPIENT_CHANGE && ev.ev.channel_id == 9101 &&
           ev.ev.user_id == 505 && ev.ev.recipient_added && ev.ev.user_count == 1 &&
           strcmp(ev.ev.users[0].name, "Grace") == 0);
  ccord_live_event_free(&ev);

  const char *rec_rem =
      "{\"op\":0,\"t\":\"CHANNEL_RECIPIENT_REMOVE\",\"d\":{\"channel_id\":\"9101\","
      "\"user\":{\"id\":\"505\",\"username\":\"grace\"}}}";
  CT_CHECK(ccord_live_gateway_decode(rec_rem, strlen(rec_rem), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_DM_RECIPIENT_CHANGE && ev.ev.channel_id == 9101 &&
           ev.ev.user_id == 505 && !ev.ev.recipient_added);
  ccord_live_event_free(&ev);

  const char *note_gw = "{\"op\":0,\"t\":\"USER_NOTE_UPDATE\",\"d\":{\"id\":\"501\",\"note\":\"Updated note\"}}";
  CT_CHECK(ccord_live_gateway_decode(note_gw, strlen(note_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_USER_NOTE_UPDATE && ev.ev.user_id == 501 &&
           ev.ev.text && strcmp(ev.ev.text, "Updated note") == 0);
  ccord_live_event_free(&ev);

  const char *cs_gw =
      "{\"op\":0,\"t\":\"USER_SETTINGS_UPDATE\",\"d\":{\"custom_status\":{\"text\":\"AFK\",\"emoji_name\":\"\\u2615\"}}}";
  CT_CHECK(ccord_live_gateway_decode(cs_gw, strlen(cs_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CUSTOM_STATUS_UPDATE && !ev.ev.custom_status_clear &&
           strcmp(ev.ev.custom_status_text, "AFK") == 0);
  ccord_live_event_free(&ev);

  const char *cs_clr = "{\"op\":0,\"t\":\"USER_SETTINGS_UPDATE\",\"d\":{\"custom_status\":null}}";
  CT_CHECK(ccord_live_gateway_decode(cs_clr, strlen(cs_clr), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CUSTOM_STATUS_UPDATE && ev.ev.custom_status_clear);
  ccord_live_event_free(&ev);

  const char *pres_gw =
      "{\"op\":0,\"t\":\"PRESENCE_UPDATE\",\"d\":{\"user\":{\"id\":\"501\"},\"status\":\"online\",\"activities\":["
      "{\"type\":2,\"name\":\"Spotify\",\"details\":\"Synthwave\",\"state\":\"Midnight\","
      "\"timestamps\":{\"start\":1700000000000}},"
      "{\"type\":4,\"state\":\"Vibing\",\"emoji\":{\"name\":\"tune\",\"id\":\"888999\"}}"
      "]}}";
  CT_CHECK(ccord_live_gateway_decode(pres_gw, strlen(pres_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_PRESENCE_UPDATE && ev.ev.has_presence_detail &&
           ev.ev.has_activity && ev.ev.activity_type == CCORD_ACTIVITY_LISTENING &&
           strcmp(ev.ev.activity_name, "Spotify") == 0 && strcmp(ev.ev.activity_details, "Synthwave") == 0 &&
           strcmp(ev.ev.custom_status_text, "Vibing") == 0 &&
           strcmp(ev.ev.custom_status_emoji, "tune:888999") == 0);
  ccord_live_event_free(&ev);

  // 3. Gateway op 3 presence_ex builder & extended user profile JSON decoder.
  ccord_buf b;
  ccord_buf_init(&b, 0);
  CT_CHECK(ccord_gateway_build_presence_ex(CCORD_PRESENCE_IDLE, "tune", 888999, "Vibing", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"op\":3") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"status\":\"idle\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"type\":4") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"state\":\"Vibing\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"name\":\"tune\",\"id\":\"888999\"") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_presence_ex(CCORD_PRESENCE_OFFLINE, nullptr, 0, nullptr, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"status\":\"invisible\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"activities\":[]") != nullptr);
  ccord_buf_free(&b);

  const char *ext_prof =
      "{\"user\":{\"id\":\"501\",\"username\":\"ada\",\"global_name\":\"Ada\",\"clan\":{\"tag\":\"C23\"}},"
      "\"user_profile\":{\"bio\":\"Engine\",\"pronouns\":\"she/her\"},"
      "\"connected_accounts\":["
      "{\"type\":\"github\",\"name\":\"adalovelace\",\"verified\":true},"
      "{\"type\":\"twitch\",\"name\":\"ada_live\",\"verified\":false}"
      "],"
      "\"mutual_guilds\":[{\"id\":\"100\"},{\"id\":\"200\"}],"
      "\"mutual_friends\":[{\"id\":\"777\",\"username\":\"babbage\",\"global_name\":\"Charles\",\"avatar\":\"av1\"}],"
      "\"mutual_friends_count\":9}";
  CT_CHECK(ccord_live_json_user_profile(ext_prof, strlen(ext_prof), 501, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_USER_PROFILE && ev.ev.user_profile != nullptr);
  if (ev.ev.user_profile) {
    const ccord_user_profile *p = ev.ev.user_profile;
    CT_CHECK(strcmp(p->clan_tag, "C23") == 0 && p->connection_count == 2);
    CT_CHECK(strcmp(p->connections[0].type, "github") == 0 && strcmp(p->connections[0].name, "adalovelace") == 0 &&
             p->connections[0].verified);
    CT_CHECK(strcmp(p->connections[1].type, "twitch") == 0 && !p->connections[1].verified);
    CT_CHECK(p->mutual_guild_count == 2 && p->mutual_guild_ids[0] == 100 && p->mutual_guild_ids[1] == 200);
    CT_CHECK(p->mutual_friend_count == 1 && p->mutual_friend_total == 9 &&
             p->mutual_friends[0].user_id == 777 && strcmp(p->mutual_friends[0].name, "Charles") == 0 &&
             strcmp(p->mutual_friends[0].handle, "babbage") == 0);
  }
  ccord_live_event_free(&ev);
}

static void test_m4a_guild_settings_incidents_vanity_and_audit_changes_json(void)
{
  ccord_error err;
  ccord_live_event ev = {0};

  // 1. Guild JSON decoding: banner CDN URL (animated + static), system/AFK channels & flags, default notifications,
  // verification_level, explicit_content_filter, premium_tier/boosts, vanity_url_code, features, incidents_data.
  const char *g_json =
      "{\"id\":\"1111\",\"name\":\"C-Cord HQ\",\"owner_id\":\"42\","
      "\"icon\":\"ic_hash\",\"banner\":\"a_bn_hash\","
      "\"system_channel_id\":\"2222\",\"system_channel_flags\":11,"
      "\"default_message_notifications\":1,"
      "\"afk_channel_id\":\"2225\",\"afk_timeout\":900,"
      "\"verification_level\":3,\"explicit_content_filter\":2,"
      "\"premium_tier\":3,\"premium_subscription_count\":18,"
      "\"vanity_url_code\":\"ccord-hq\","
      "\"features\":[\"VANITY_URL\",\"BANNER\",\"COMMUNITY\",\"DISCOVERABLE\"],"
      "\"incidents_data\":{\"invites_disabled_until\":\"2026-11-12T00:00:00.000000+00:00\","
      "\"dms_disabled_until\":\"2026-11-11T06:00:00.000000+00:00\"}}";
  CT_CHECK(ccord_live_json_guild(g_json, strlen(g_json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_UPSERT && ev.ev.guild != nullptr);
  if (ev.ev.guild) {
    const ccord_guild *g = ev.ev.guild;
    CT_CHECK(g->id == 1111 && g->owner_id == 42 && g->has_guild_settings && g->has_incidents_data);
    CT_CHECK(strstr(g->banner_url, "/banners/1111/a_bn_hash.gif") != nullptr);
    CT_CHECK(g->system_channel_id == 2222 && g->system_channel_flags == 11);
    CT_CHECK(g->default_message_notifications == 1);
    CT_CHECK(g->afk_channel_id == 2225 && g->afk_timeout == 900);
    CT_CHECK(g->verification_level == 3 && g->explicit_content_filter == 2);
    CT_CHECK(g->premium_tier == 3 && g->premium_subscription_count == 18);
    CT_CHECK(strcmp(g->vanity_url_code, "ccord-hq") == 0);
    CT_CHECK(g->feature_vanity_url && g->feature_banner && g->feature_community);
    CT_CHECK(g->invites_disabled_until_ms > 0 && g->dms_disabled_until_ms > 0 &&
             g->invites_disabled_until_ms > g->dms_disabled_until_ms);
  }
  ccord_live_event_free(&ev);

  // Gateway GUILD_UPDATE with null banner/system/afk/incidents_data clears them cleanly.
  const char *gw_gupd =
      "{\"op\":0,\"t\":\"GUILD_UPDATE\",\"d\":{\"id\":\"1111\",\"name\":\"C-Cord HQ\",\"owner_id\":\"3333\","
      "\"banner\":\"bn_static\",\"system_channel_id\":null,\"system_channel_flags\":0,"
      "\"default_message_notifications\":0,\"afk_channel_id\":null,\"afk_timeout\":300,"
      "\"verification_level\":1,\"explicit_content_filter\":0,\"premium_tier\":2,"
      "\"premium_subscription_count\":7,\"vanity_url_code\":null,\"features\":[\"BANNER\"],"
      "\"incidents_data\":null}}";
  CT_CHECK(ccord_live_gateway_decode(gw_gupd, strlen(gw_gupd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT && ev.guild_count == 1 && ev.guilds != nullptr);
  if (ev.guilds && ev.guild_count == 1) {
    const ccord_guild *g = &ev.guilds[0];
    CT_CHECK(g->owner_id == 3333 && g->has_guild_settings && g->has_incidents_data);
    CT_CHECK(strstr(g->banner_url, "/banners/1111/bn_static.png") != nullptr);
    CT_CHECK(g->system_channel_id == CCORD_ID_NONE && g->afk_channel_id == CCORD_ID_NONE && g->afk_timeout == 300);
    CT_CHECK(g->vanity_url_code[0] == '\0' && !g->feature_vanity_url && g->feature_banner);
    CT_CHECK(g->invites_disabled_until_ms == 0 && g->dms_disabled_until_ms == 0);
  }
  ccord_live_event_free(&ev);

  // 2. Incident actions & vanity URL REST decoders (including null clears and malformed input).
  const char *inc_json =
      "{\"invites_disabled_until\":\"2026-11-12T12:00:00.000000+00:00\",\"dms_disabled_until\":null}";
  CT_CHECK(ccord_live_json_incident_actions(inc_json, strlen(inc_json), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_INCIDENTS && ev.ev.guild_id == 1111 &&
           ev.ev.invites_disabled_until_ms > 0 && ev.ev.dms_disabled_until_ms == 0);
  ccord_live_event_free(&ev);

  CT_CHECK(ccord_live_json_incident_actions("null", 4, 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_INCIDENTS &&
           ev.ev.invites_disabled_until_ms == 0 && ev.ev.dms_disabled_until_ms == 0);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_incident_actions("{bad", 4, 1111, &ev, &err) == CCORD_ERR_PARSE);
  CT_CHECK(ccord_live_json_incident_actions("{}", 2, CCORD_ID_NONE, &ev, &err) == CCORD_ERR_INVALID);

  const char *van_json = "{\"code\":\"systems-c23\",\"uses\":42}";
  CT_CHECK(ccord_live_json_vanity_url(van_json, strlen(van_json), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_VANITY && ev.ev.guild_id == 1111 &&
           ev.ev.invite_code && strcmp(ev.ev.invite_code, "systems-c23") == 0);
  ccord_live_event_free(&ev);

  const char *van_null = "{\"code\":null}";
  CT_CHECK(ccord_live_json_vanity_url(van_null, strlen(van_null), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_VANITY && (!ev.ev.invite_code || ev.ev.invite_code[0] == '\0'));
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_vanity_url("[]", 2, 1111, &ev, &err) == CCORD_ERR_PARSE);
  CT_CHECK(ccord_live_json_vanity_url("{}", 2, CCORD_ID_NONE, &ev, &err) == CCORD_ERR_INVALID);

  // 3. Structured audit log changes[] decoding (strings, ints, bools, nulls, arrays/objects).
  const char *aud_json =
      "{\"audit_log_entries\":[{\"id\":\"900001\",\"user_id\":\"42\",\"target_id\":\"1111\",\"action_type\":1,"
      "\"reason\":\"Lockdown\",\"changes\":["
      "{\"key\":\"name\",\"old_value\":\"Old\",\"new_value\":\"New\"},"
      "{\"key\":\"verification_level\",\"old_value\":1,\"new_value\":3},"
      "{\"key\":\"widget_enabled\",\"old_value\":false,\"new_value\":true},"
      "{\"key\":\"vanity_url_code\",\"old_value\":\"oldslug\",\"new_value\":null},"
      "{\"key\":\"$add\",\"new_value\":[{\"id\":\"1\",\"name\":\"Mod\"}]}"
      "]}],\"users\":[{\"id\":\"42\",\"username\":\"admin\"}]}";
  CT_CHECK(ccord_live_json_audit_log(aud_json, strlen(aud_json), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_AUDIT_LOG && ev.ev.audit_count == 1 && ev.ev.audit != nullptr);
  if (ev.ev.audit && ev.ev.audit_count == 1) {
    const ccord_audit_entry *a = &ev.ev.audit[0];
    CT_CHECK(a->change_count == 5);
    CT_CHECK(strcmp(a->changes[0].key, "name") == 0 && strcmp(a->changes[0].old_val, "Old") == 0 &&
             strcmp(a->changes[0].new_val, "New") == 0);
    CT_CHECK(strcmp(a->changes[1].key, "verification_level") == 0 && strcmp(a->changes[1].old_val, "1") == 0 &&
             strcmp(a->changes[1].new_val, "3") == 0);
    CT_CHECK(strcmp(a->changes[2].key, "widget_enabled") == 0 && strcmp(a->changes[2].old_val, "no") == 0 &&
             strcmp(a->changes[2].new_val, "yes") == 0);
    CT_CHECK(strcmp(a->changes[3].key, "vanity_url_code") == 0 && strcmp(a->changes[3].old_val, "oldslug") == 0 &&
             strcmp(a->changes[3].new_val, "none") == 0);
    CT_CHECK(strcmp(a->changes[4].key, "$add") == 0 && a->changes[4].old_val[0] == '\0' &&
             strcmp(a->changes[4].new_val, "1 items") == 0);
  }
  ccord_live_event_free(&ev);
}

static void test_m4b_expressions_json_and_gateway(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. REST decoders: guild emojis (array + single object + uploader info).
  const char *em_arr =
      "[{\"id\":\"501\",\"name\":\"wave\",\"animated\":false,\"available\":true,"
      "\"user\":{\"id\":\"42\",\"username\":\"you_h\",\"global_name\":\"You\"}},"
      "{\"id\":\"502\",\"name\":\"spin\",\"animated\":true,\"user\":{\"id\":\"43\",\"username\":\"ada\"}}]";
  CT_CHECK(ccord_live_json_guild_emojis(em_arr, strlen(em_arr), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_EXPRESSIONS && ev.ev.guild_id == 1111 &&
           ev.ev.expressions_admin && ev.ev.replace_emojis && ev.ev.emoji_count == 2);
  CT_CHECK(ev.ev.emojis[0].id == 501 && ev.ev.emojis[0].user_id == 42 &&
           strcmp(ev.ev.emojis[0].user_name, "You") == 0);
  CT_CHECK(ev.ev.emojis[1].id == 502 && ev.ev.emojis[1].animated && ev.ev.emojis[1].user_id == 43 &&
           strcmp(ev.ev.emojis[1].user_name, "ada") == 0);
  ccord_live_event_free(&ev);

  const char *em_one = "{\"id\":\"503\",\"name\":\"fresh\",\"animated\":true,\"user\":{\"id\":\"42\",\"username\":\"me\"}}";
  CT_CHECK(ccord_live_json_guild_emojis(em_one, strlen(em_one), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_EMOJI_UPSERT && ev.ev.emoji_count == 1 &&
           ev.ev.emojis[0].id == 503 && ev.ev.emojis[0].user_id == 42);
  ccord_live_event_free(&ev);

  // 2. REST decoders: guild stickers (array + single object).
  const char *st_arr =
      "[{\"id\":\"601\",\"name\":\"Salute\",\"description\":\"o7\",\"tags\":\"salute\",\"format_type\":2,"
      "\"available\":true,\"user\":{\"id\":\"42\",\"global_name\":\"You\"}}]";
  CT_CHECK(ccord_live_json_guild_stickers(st_arr, strlen(st_arr), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_EXPRESSIONS && ev.ev.expressions_admin && ev.ev.replace_stickers &&
           ev.ev.sticker_count == 1 && ev.ev.stickers[0].format == CCORD_STICKER_APNG &&
           ev.ev.stickers[0].user_id == 42 && strcmp(ev.ev.stickers[0].user_name, "You") == 0);
  ccord_live_event_free(&ev);

  const char *st_one =
      "{\"id\":\"602\",\"name\":\"Vec\",\"description\":null,\"tags\":\"vec\",\"format_type\":3,\"available\":true}";
  CT_CHECK(ccord_live_json_guild_stickers(st_one, strlen(st_one), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_STICKER_UPSERT && ev.ev.sticker_count == 1 &&
           ev.ev.stickers[0].format == CCORD_STICKER_LOTTIE);
  ccord_live_event_free(&ev);

  // 3. REST decoders: guild soundboard ({"items":[...]} + array + single object + volume rounding/clamping).
  const char *sb_items =
      "{\"items\":[{\"sound_id\":\"701\",\"name\":\"Airhorn\",\"volume\":0.75,\"emoji_name\":\"📯\","
      "\"available\":true,\"user\":{\"id\":\"42\",\"username\":\"me\"}},"
      "{\"sound_id\":\"702\",\"name\":\"Loud\",\"volume\":1.8,\"available\":true}]}";
  CT_CHECK(ccord_live_json_guild_soundboard(sb_items, strlen(sb_items), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_EXPRESSIONS && ev.ev.expressions_admin && ev.ev.replace_soundboard &&
           ev.ev.soundboard_count == 2);
  CT_CHECK(ev.ev.soundboard[0].id == 701 && ev.ev.soundboard[0].volume_pct == 75 &&
           ev.ev.soundboard[0].user_id == 42 && strcmp(ev.ev.soundboard[0].emoji_name, "📯") == 0);
  CT_CHECK(ev.ev.soundboard[1].id == 702 && ev.ev.soundboard[1].volume_pct == 100);
  ccord_live_event_free(&ev);

  const char *sb_one = "{\"sound_id\":\"703\",\"name\":\"Rimshot\",\"volume\":0.25,\"emoji_id\":\"501\",\"available\":true}";
  CT_CHECK(ccord_live_json_guild_soundboard(sb_one, strlen(sb_one), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SOUNDBOARD_UPSERT && ev.ev.soundboard_count == 1 &&
           ev.ev.soundboard[0].id == 703 && ev.ev.soundboard[0].volume_pct == 25 &&
           ev.ev.soundboard[0].emoji_id == 501);
  ccord_live_event_free(&ev);

  // 4. Gateway dispatches: GUILD_SOUNDBOARD_SOUNDS_UPDATE / SOUND_CREATE / SOUND_UPDATE / SOUND_DELETE.
  const char *gw_list =
      "{\"op\":0,\"t\":\"GUILD_SOUNDBOARD_SOUNDS_UPDATE\",\"d\":{\"guild_id\":\"1111\","
      "\"soundboard_sounds\":[{\"sound_id\":\"710\",\"name\":\"Bell\",\"volume\":0.5}]}}";
  CT_CHECK(ccord_live_gateway_decode(gw_list, strlen(gw_list), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_EXPRESSIONS && !ev.ev.expressions_admin && ev.ev.replace_soundboard &&
           ev.ev.soundboard_count == 1 && ev.ev.soundboard[0].id == 710 && ev.ev.soundboard[0].volume_pct == 50);
  ccord_live_event_free(&ev);

  const char *gw_create =
      "{\"op\":0,\"t\":\"GUILD_SOUNDBOARD_SOUND_CREATE\",\"d\":{\"guild_id\":\"1111\",\"sound_id\":\"711\",\"name\":\"Gong\",\"volume\":1.0}}";
  CT_CHECK(ccord_live_gateway_decode(gw_create, strlen(gw_create), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SOUNDBOARD_UPSERT && ev.ev.guild_id == 1111 &&
           ev.ev.soundboard_count == 1 && ev.ev.soundboard[0].id == 711);
  ccord_live_event_free(&ev);

  const char *gw_upd =
      "{\"op\":0,\"t\":\"GUILD_SOUNDBOARD_SOUND_UPDATE\",\"d\":{\"guild_id\":\"1111\",\"sound_id\":\"711\",\"name\":\"Gong2\",\"volume\":0.4}}";
  CT_CHECK(ccord_live_gateway_decode(gw_upd, strlen(gw_upd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SOUNDBOARD_UPSERT && ev.ev.soundboard[0].volume_pct == 40 &&
           strcmp(ev.ev.soundboard[0].name, "Gong2") == 0);
  ccord_live_event_free(&ev);

  const char *gw_del =
      "{\"op\":0,\"t\":\"GUILD_SOUNDBOARD_SOUND_DELETE\",\"d\":{\"guild_id\":\"1111\",\"sound_id\":\"711\"}}";
  CT_CHECK(ccord_live_gateway_decode(gw_del, strlen(gw_del), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SOUNDBOARD_DELETE && ev.ev.guild_id == 1111 && ev.ev.expr_id == 711);
  ccord_live_event_free(&ev);
}

static void test_m4c_members_prune_and_automod_json(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. Gateway GUILD_MEMBER_ADD decodes ISO-8601 joined_at -> joined_at_ms > 0:
  const char *mem_add =
      "{\"op\":0,\"t\":\"GUILD_MEMBER_ADD\",\"d\":{\"guild_id\":\"1111\","
      "\"user\":{\"id\":\"3333\",\"username\":\"ada\",\"global_name\":\"Ada\"},"
      "\"roles\":[\"7001\"],\"joined_at\":\"2026-01-15T08:30:00.000000+00:00\"}}";
  CT_CHECK(ccord_live_gateway_decode(mem_add, strlen(mem_add), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_MEMBERS && ev.ev.guild_member_count == 1 &&
           ev.ev.guild_members[0].user_id == 3333 && ev.ev.guild_members[0].joined_at_ms > 1700000000000LL);
  ccord_live_event_free(&ev);

  // 2. REST prune decoders (preview integer + execute null):
  const char *pr_prev = "{\"pruned\":5}";
  CT_CHECK(ccord_live_json_prune(pr_prev, strlen(pr_prev), 1111, 14, false, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_PRUNE_RESULT && ev.ev.guild_id == 1111 && ev.ev.prune_info.has_result &&
           !ev.ev.prune_info.executed && ev.ev.prune_info.days == 14 && ev.ev.prune_info.pruned == 5);
  ccord_live_event_free(&ev);

  const char *pr_null = "{\"pruned\":null}";
  CT_CHECK(ccord_live_json_prune(pr_null, strlen(pr_null), 1111, 7, true, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_PRUNE_RESULT && ev.ev.prune_info.executed && ev.ev.prune_info.pruned == -1);
  ccord_live_event_free(&ev);

  // 3. REST AutoMod rules decoder (array -> AUTOMOD_LIST, single object -> AUTOMOD_UPSERT):
  const char *am_arr =
      "[{\"id\":\"8801\",\"guild_id\":\"1111\",\"name\":\"Scam Links\",\"creator_id\":\"42\","
      "\"event_type\":1,\"trigger_type\":1,\"enabled\":true,"
      "\"trigger_metadata\":{\"keyword_filter\":[\"free nitro\",\"*scam*\"],"
      "\"regex_patterns\":[\"d[i1]scord\\\\.gift\"],\"allow_list\":[\"discord.com\"]},"
      "\"actions\":[{\"type\":1,\"metadata\":{\"custom_message\":\"Blocked scam\"}},"
      "{\"type\":2,\"metadata\":{\"channel_id\":\"2222\"}},{\"type\":3,\"metadata\":{\"duration_seconds\":300}}],"
      "\"exempt_roles\":[\"7001\"],\"exempt_channels\":[\"2223\"]},"
      "{\"id\":\"8802\",\"guild_id\":\"1111\",\"name\":\"Presets\",\"creator_id\":\"42\","
      "\"event_type\":1,\"trigger_type\":4,\"enabled\":false,"
      "\"trigger_metadata\":{\"presets\":[1,3],\"allow_list\":[\"ok\"]},"
      "\"actions\":[{\"type\":1,\"metadata\":{}}],\"exempt_roles\":[],\"exempt_channels\":[]},"
      "{\"id\":\"8803\",\"guild_id\":\"1111\",\"name\":\"Mention Cap\",\"creator_id\":\"42\","
      "\"event_type\":1,\"trigger_type\":5,\"enabled\":true,"
      "\"trigger_metadata\":{\"mention_total_limit\":8,\"mention_raid_protection_enabled\":true},"
      "\"actions\":[{\"type\":1,\"metadata\":{}}],\"exempt_roles\":[],\"exempt_channels\":[]}]";
  CT_CHECK(ccord_live_json_automod_rules(am_arr, strlen(am_arr), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_AUTOMOD_LIST && ev.ev.guild_id == 1111 && ev.ev.automod_rule_count == 3);
  const ccord_automod_rule *r0 = &ev.ev.automod_rules[0];
  CT_CHECK(r0->id == 8801 && r0->creator_id == 42 && r0->trigger_type == CCORD_AUTOMOD_TRIGGER_KEYWORD && r0->enabled);
  CT_CHECK(r0->keyword_count == 2 && strcmp(r0->keywords[0], "free nitro") == 0);
  CT_CHECK(r0->regex_count == 1 && strcmp(r0->regex_patterns[0], "d[i1]scord\\.gift") == 0);
  CT_CHECK(r0->allow_count == 1 && strcmp(r0->allow_list[0], "discord.com") == 0);
  CT_CHECK(r0->action_block && strcmp(r0->custom_message, "Blocked scam") == 0);
  CT_CHECK(r0->action_alert && r0->alert_channel_id == 2222);
  CT_CHECK(r0->action_timeout && r0->timeout_seconds == 300);
  CT_CHECK(r0->exempt_role_count == 1 && r0->exempt_role_ids[0] == 7001);
  CT_CHECK(r0->exempt_channel_count == 1 && r0->exempt_channel_ids[0] == 2223);
  CT_CHECK(ev.ev.automod_rules[1].presets == (CCORD_AUTOMOD_PRESET_PROFANITY | CCORD_AUTOMOD_PRESET_SLURS));
  CT_CHECK(ev.ev.automod_rules[2].mention_total_limit == 8 && ev.ev.automod_rules[2].mention_raid_protection);
  ccord_live_event_free(&ev);

  const char *am_one =
      "{\"id\":\"8804\",\"guild_id\":\"1111\",\"name\":\"Spam\",\"creator_id\":\"42\","
      "\"event_type\":1,\"trigger_type\":3,\"enabled\":true,\"trigger_metadata\":{},"
      "\"actions\":[{\"type\":1,\"metadata\":{}}],\"exempt_roles\":[],\"exempt_channels\":[]}";
  CT_CHECK(ccord_live_json_automod_rules(am_one, strlen(am_one), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_AUTOMOD_UPSERT && ev.ev.automod_rule_count == 1 && ev.ev.automod_rules[0].id == 8804);
  ccord_live_event_free(&ev);

  // 4. Gateway AUTO_MODERATION_RULE_CREATE / UPDATE / DELETE:
  const char *gw_am_c =
      "{\"op\":0,\"t\":\"AUTO_MODERATION_RULE_CREATE\",\"d\":{\"id\":\"8810\",\"guild_id\":\"1111\","
      "\"name\":\"GW Rule\",\"creator_id\":\"42\",\"event_type\":1,\"trigger_type\":3,\"enabled\":true,"
      "\"trigger_metadata\":{},\"actions\":[{\"type\":1,\"metadata\":{}}],\"exempt_roles\":[],\"exempt_channels\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(gw_am_c, strlen(gw_am_c), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_AUTOMOD_UPSERT && ev.ev.guild_id == 1111 && ev.ev.automod_rules[0].id == 8810);
  ccord_live_event_free(&ev);

  const char *gw_am_u =
      "{\"op\":0,\"t\":\"AUTO_MODERATION_RULE_UPDATE\",\"d\":{\"id\":\"8810\",\"guild_id\":\"1111\","
      "\"name\":\"GW Rule Off\",\"creator_id\":\"42\",\"event_type\":1,\"trigger_type\":3,\"enabled\":false,"
      "\"trigger_metadata\":{},\"actions\":[{\"type\":1,\"metadata\":{}}],\"exempt_roles\":[],\"exempt_channels\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(gw_am_u, strlen(gw_am_u), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_AUTOMOD_UPSERT && !ev.ev.automod_rules[0].enabled &&
           strcmp(ev.ev.automod_rules[0].name, "GW Rule Off") == 0);
  ccord_live_event_free(&ev);

  const char *gw_am_d =
      "{\"op\":0,\"t\":\"AUTO_MODERATION_RULE_DELETE\",\"d\":{\"id\":\"8810\",\"guild_id\":\"1111\"}}";
  CT_CHECK(ccord_live_gateway_decode(gw_am_d, strlen(gw_am_d), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_AUTOMOD_DELETE && ev.ev.guild_id == 1111 && ev.ev.expr_id == 8810);
  ccord_live_event_free(&ev);
}


int main(void)
{
  test_gateway_member_events();
  test_gateway_permission_data();
  test_gateway_notify_settings();
  test_polls_components_and_modals_json();
  test_slash_commands_ephemeral_reply_and_uploads_json();
  test_search_pins_threads_and_burst_reactions_json();
  test_m3b_folders_invite_and_discovery_json();
  test_m3c_social_presence_requests_and_profile_json();
  test_m4a_guild_settings_incidents_vanity_and_audit_changes_json();
  test_m4b_expressions_json_and_gateway();
  test_m4c_members_prune_and_automod_json();
  test_discord_error_message();
  test_forum_json_and_gateway_events();
  return CT_RESULT("test_live_rest");
}
