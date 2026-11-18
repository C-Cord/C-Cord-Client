// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Profile extras of GET /users/{id}/profile: ccord_live_json_user_profile parses banner colour, theme colours, the
// per-server profile (guild_member_profile + guild_member), badges and the avatar decoration. Absent or null keys leave
// the fields empty; a per-server theme wins over the user-level one. The mock transport answers the same fields for one
// seeded member (Ada, 1002) and none for another (Chen, 1004).
#include <stdio.h>
#include <string.h>

#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>
#include <ccord/store/store.h>

#include "ccord_test.h"

static const char kFull[] =
  "{\"user\":{\"id\":\"501\",\"username\":\"ada\",\"global_name\":\"Ada\",\"avatar\":\"av_hash\","
  "\"banner\":\"bn_hash\",\"accent_color\":3447003,\"banner_color\":\"#1a2b3c\","
  "\"avatar_decoration_data\":{\"asset\":\"deco_neon\",\"sku_id\":\"1\",\"expires_at\":null}},"
  "\"user_profile\":{\"bio\":\"hi\",\"pronouns\":\"she/her\",\"theme_colors\":[16711680,255]},"
  "\"badges\":[{\"id\":\"early_supporter\",\"description\":\"Early Supporter\",\"icon\":\"es_icon\"},"
  "{\"id\":\"bug_hunter\",\"description\":\"Bug Hunter\",\"icon\":\"bh_icon\"}],"
  "\"guild_member_profile\":{\"guild_id\":\"2001\",\"bio\":\"server bio\",\"pronouns\":\"they/them\","
  "\"banner\":\"gbn_hash\",\"theme_colors\":[65280,16776960]},"
  "\"guild_member\":{\"nick\":\"Ada The Builder\",\"avatar\":\"gav_hash\","
  "\"joined_at\":\"2024-03-05T10:00:00.000000+00:00\",\"premium_since\":\"2025-01-01T00:00:00+00:00\"}}";

// Every new key absent or null: the parse still succeeds and the new fields stay empty.
static const char kAbsent[] =
  "{\"user\":{\"id\":\"502\",\"username\":\"bob\",\"banner_color\":null,\"avatar_decoration_data\":null},"
  "\"user_profile\":{\"bio\":\"x\",\"theme_colors\":null},\"badges\":null,"
  "\"guild_member_profile\":null,\"guild_member\":null}";

// Malformed values: a bad colour, an unsafe icon name, an empty per-server theme (user-level theme used instead).
static const char kMixed[] =
  "{\"user\":{\"id\":\"504\",\"username\":\"d\",\"banner_color\":\"#zz0000\"},"
  "\"user_profile\":{\"theme_colors\":[10,20]},"
  "\"badges\":[{\"id\":\"b1\",\"description\":\"B\",\"icon\":\"../evil\"}],"
  "\"guild_member_profile\":{\"guild_id\":\"2001\",\"theme_colors\":[]},"
  "\"guild_member\":{\"avatar\":\"../x\"}}";

static bool is_empty(const char *s)
{
  return !s || s[0] == '\0';
}

static void test_full_profile(void)
{
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_json_user_profile(kFull, strlen(kFull), 501, &ev, &err) == CCORD_OK);
  const ccord_user_profile *p = ev.ev.user_profile;
  CT_CHECK(p != nullptr);
  if (p) {
    // Existing fields still parse.
    CT_CHECK(strcmp(p->display_name, "Ada") == 0 && strcmp(p->bio, "hi") == 0);
    CT_CHECK(p->accent_color == 3447003u);
    CT_CHECK(strstr(p->banner_url, "banners/501/bn_hash.png") != nullptr);
    // banner_color "#1a2b3c".
    CT_CHECK(p->has_banner_color && p->banner_color == 0x1A2B3Cu);
    // Per-server theme [65280, 16776960] wins over the user-level [16711680, 255].
    CT_CHECK(p->has_theme && p->theme_primary == 0x00FF00u && p->theme_accent == 0xFFFF00u);
    // Per-server profile.
    CT_CHECK(p->guild_id == 2001);
    CT_CHECK(strcmp(p->guild_nick, "Ada The Builder") == 0);
    CT_CHECK(strcmp(p->guild_bio, "server bio") == 0 && strcmp(p->guild_pronouns, "they/them") == 0);
    CT_CHECK(strcmp(p->guild_avatar_url,
                    "https://cdn.discordapp.com/guilds/2001/users/501/avatars/gav_hash.png?size=64") == 0);
    CT_CHECK(strcmp(p->guild_banner_url,
                    "https://cdn.discordapp.com/guilds/2001/users/501/banners/gbn_hash.png?size=512") == 0);
    CT_CHECK(p->guild_joined_at_ms == 1709632800000LL);   // 2024-03-05T10:00:00Z
    CT_CHECK(p->premium_since_ms == 1735689600000LL);     // 2025-01-01T00:00:00Z
    // Badges keep their order and get CDN icon URLs.
    CT_CHECK(p->badge_count == 2);
    CT_CHECK(strcmp(p->badges[0].id, "early_supporter") == 0);
    CT_CHECK(strcmp(p->badges[0].description, "Early Supporter") == 0);
    CT_CHECK(strcmp(p->badges[0].icon_url, "https://cdn.discordapp.com/badge-icons/es_icon.png") == 0);
    CT_CHECK(strcmp(p->badges[1].id, "bug_hunter") == 0);
    // Avatar decoration preset.
    CT_CHECK(strcmp(p->avatar_decoration_url,
                    "https://cdn.discordapp.com/avatar-decoration-presets/deco_neon.png?size=128&passthrough=true") == 0);
  }
  ccord_live_event_free(&ev);
}

static void test_absent_profile(void)
{
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_json_user_profile(kAbsent, strlen(kAbsent), 502, &ev, &err) == CCORD_OK);
  const ccord_user_profile *p = ev.ev.user_profile;
  CT_CHECK(p != nullptr);
  if (p) {
    CT_CHECK(!p->has_banner_color && p->banner_color == 0);
    CT_CHECK(!p->has_theme && p->theme_primary == 0 && p->theme_accent == 0);
    CT_CHECK(p->guild_id == CCORD_ID_NONE);
    CT_CHECK(is_empty(p->guild_nick) && is_empty(p->guild_avatar_url) && is_empty(p->guild_banner_url));
    CT_CHECK(is_empty(p->guild_bio) && is_empty(p->guild_pronouns));
    CT_CHECK(p->guild_joined_at_ms == 0 && p->premium_since_ms == 0);
    CT_CHECK(p->badge_count == 0);
    CT_CHECK(is_empty(p->avatar_decoration_url));
    CT_CHECK(strcmp(p->bio, "x") == 0);   // the existing fields are unaffected
  }
  ccord_live_event_free(&ev);
}

static void test_mixed_profile(void)
{
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_json_user_profile(kMixed, strlen(kMixed), 504, &ev, &err) == CCORD_OK);
  const ccord_user_profile *p = ev.ev.user_profile;
  CT_CHECK(p != nullptr);
  if (p) {
    CT_CHECK(!p->has_banner_color);                                       // "#zz0000" is not a colour
    CT_CHECK(p->has_theme && p->theme_primary == 10u && p->theme_accent == 20u);   // empty per-server theme: user-level
    CT_CHECK(p->guild_id == 2001);
    CT_CHECK(is_empty(p->guild_avatar_url));                              // "../x" is not a CDN name
    CT_CHECK(p->badge_count == 1 && strcmp(p->badges[0].id, "b1") == 0);
    CT_CHECK(is_empty(p->badges[0].icon_url));                            // "../evil" is not a badge icon
  }
  ccord_live_event_free(&ev);
}

static uint64_t fake_now(void *ud)
{
  (void)ud;
  return 1000;
}

static bool poll_profile(ccord_transport *t, ccord_net_event *out)
{
  for (int i = 0; i < 16; i++) {
    if (!ccord_transport_poll(t, out)) return false;
    if (out->type == CCORD_NET_USER_PROFILE) return true;
  }
  return false;
}

static void test_mock_profile_extras(void)
{
  ccord_error err;
  ccord_mock_config cfg = {.seed = 42, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;

  // Ada (1002) in HQ (2001): the rich profile, with the per-server fields and the per-server theme.
  CT_CHECK(ccord_transport_request_profile_ex(t, 1002, 2001, &err) == CCORD_OK);
  CT_CHECK(poll_profile(t, &ev) && ev.user_profile != nullptr);
  if (ev.user_profile) {
    const ccord_user_profile p = *ev.user_profile;
    CT_CHECK(p.has_banner_color && p.banner_color == 0x2B6CB0u);
    CT_CHECK(p.has_theme && p.theme_primary == 0xFAA61Au && p.theme_accent == 0x3BA55Cu);
    CT_CHECK(p.guild_id == 2001 && strcmp(p.guild_nick, "Ada the Builder") == 0);
    CT_CHECK(strstr(p.guild_avatar_url, "guilds/2001/users/1002/avatars/") != nullptr);
    CT_CHECK(strstr(p.guild_banner_url, "guilds/2001/users/1002/banners/") != nullptr);
    CT_CHECK(!is_empty(p.guild_bio) && !is_empty(p.guild_pronouns));
    CT_CHECK(p.guild_joined_at_ms > 0 && p.premium_since_ms > p.guild_joined_at_ms);
    CT_CHECK(p.badge_count == 2 && strcmp(p.badges[0].id, "early_supporter") == 0);
    CT_CHECK(strstr(p.badges[1].icon_url, "https://cdn.discordapp.com/badge-icons/") != nullptr);
    CT_CHECK(strstr(p.avatar_decoration_url, "avatar-decoration-presets/") != nullptr);
  }

  // Ada without a server: the user-level theme, and no per-server fields.
  CT_CHECK(ccord_transport_request_profile(t, 1002, &err) == CCORD_OK);
  CT_CHECK(poll_profile(t, &ev) && ev.user_profile != nullptr);
  if (ev.user_profile) {
    const ccord_user_profile p = *ev.user_profile;
    CT_CHECK(p.has_theme && p.theme_primary == 0x5865F2u && p.guild_id == CCORD_ID_NONE);
    CT_CHECK(is_empty(p.guild_nick) && is_empty(p.guild_avatar_url));
  }

  // Chen (1004): none of the extras.
  CT_CHECK(ccord_transport_request_profile_ex(t, 1004, 2001, &err) == CCORD_OK);
  CT_CHECK(poll_profile(t, &ev) && ev.user_profile != nullptr);
  if (ev.user_profile) {
    const ccord_user_profile p = *ev.user_profile;
    CT_CHECK(!p.has_banner_color && !p.has_theme && p.badge_count == 0);
    CT_CHECK(p.guild_id == CCORD_ID_NONE && is_empty(p.guild_nick) && is_empty(p.guild_avatar_url));
    CT_CHECK(is_empty(p.avatar_decoration_url) && p.guild_joined_at_ms == 0 && p.premium_since_ms == 0);
  }
  ccord_transport_destroy(t);
}

// The store keeps its own copies of the extra strings: a parsed event's arena is freed after the event, and a caller
// may reuse its buffers. Overwrite the caller's buffers after the upsert and the stored strings must still read right.
static void test_store_owns_profile_extras(void)
{
  ccord_error err;
  ccord_store *s = ccord_store_create(nullptr, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  char nick[] = "Ada The Builder";
  char avatar[] = "https://cdn.discordapp.com/guilds/2001/users/501/avatars/gav_hash.png?size=64";
  ccord_user_profile up = {.user_id = 501, .display_name = "Ada", .guild_id = 2001, .guild_nick = nick,
                           .guild_avatar_url = avatar, .guild_bio = "server bio"};
  CT_CHECK(ccord_store_upsert_user_profile(s, &up, &err) == CCORD_OK);
  memset(nick, 'x', sizeof nick - 1);
  memset(avatar, 'x', sizeof avatar - 1);
  const ccord_user_profile *got = ccord_store_user_profile(s, 501);
  CT_CHECK(got != nullptr);
  if (got) {
    CT_CHECK(strcmp(got->guild_nick, "Ada The Builder") == 0);
    CT_CHECK(strstr(got->guild_avatar_url, "guilds/2001/users/501/avatars/gav_hash.png") != nullptr);
    CT_CHECK(strcmp(got->guild_bio, "server bio") == 0);
    // Upserting the stored profile back (its strings alias the store's own) keeps them valid.
    ccord_user_profile again = *got;
    CT_CHECK(ccord_store_upsert_user_profile(s, &again, &err) == CCORD_OK);
    got = ccord_store_user_profile(s, 501);
    CT_CHECK(got != nullptr && strcmp(got->guild_nick, "Ada The Builder") == 0);
    // A replacement frees the old copies and keeps the new ones.
    ccord_user_profile next = {.user_id = 501, .display_name = "Ada", .guild_id = 2001, .guild_nick = "Ada 2"};
    CT_CHECK(ccord_store_upsert_user_profile(s, &next, &err) == CCORD_OK);
    got = ccord_store_user_profile(s, 501);
    CT_CHECK(got != nullptr && strcmp(got->guild_nick, "Ada 2") == 0 && is_empty(got->guild_bio));
  }
  ccord_store_destroy(s);
}

int main(void)
{
  test_full_profile();
  test_absent_profile();
  test_mixed_profile();
  test_mock_profile_extras();
  test_store_owns_profile_extras();
  return CT_RESULT("test_profile_json");
}
