// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Profile extras of GET /users/{id}/profile: ccord_live_json_user_profile parses banner colour, theme colours, the
// per-server profile (guild_member_profile + guild_member), badges and the avatar decoration. Absent or null keys leave
// the fields empty; a per-server theme wins over the user-level one. The mock transport answers the same fields for one
// seeded member (Ada, 1002) and none for another (Chen, 1004).
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
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

static void test_profile_decor_parsing(void)
{
  // Nameplate, per-server avatar decoration, profile effect ID, and profile frame SKU ID.
  static const char kDecor[] =
    "{\"user\":{\"id\":\"601\",\"username\":\"gavin\",\"global_name\":\"Gavin\","
    "\"avatar_decoration_data\":{\"asset\":\"a_user_deco\",\"sku_id\":\"10\"},"
    "\"collectibles\":{\"nameplate\":{\"sku_id\":\"20\",\"asset\":\"nameplates/nameplates/american_flag/\",\"palette\":\"cobalt\"}}},"
    "\"user_profile\":{\"bio\":\"hi\",\"profile_effect\":{\"id\":\"1139323093991575696\"},"
    "\"profile_frame\":{\"sku_id\":\"1400000000000000001\"}},"
    "\"guild_member_profile\":{\"guild_id\":\"2001\",\"profile_effect\":{\"id\":\"1139323093991575999\"}},"
    "\"guild_member\":{\"nick\":\"Gavin\",\"avatar_decoration_data\":{\"asset\":\"a_guild_deco\"},"
    "\"collectibles\":{\"nameplate\":{\"asset\":\"nameplates/nameplates/twilight/\",\"palette\":\"berry\"}}}}";
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_json_user_profile(kDecor, strlen(kDecor), 601, &ev, &err) == CCORD_OK);
  const ccord_user_profile *p = ev.ev.user_profile;
  CT_CHECK(p != nullptr && ev.ev.user_count == 1 && ev.ev.users != nullptr);
  if (p) {
    CT_CHECK(strcmp(p->avatar_decoration_url,
                    "https://cdn.discordapp.com/avatar-decoration-presets/a_user_deco.png?size=128&passthrough=true") == 0);
    CT_CHECK(strcmp(p->guild_avatar_decoration_url,
                    "https://cdn.discordapp.com/avatar-decoration-presets/a_guild_deco.png?size=128&passthrough=true") == 0);
    CT_CHECK(strcmp(p->nameplate_asset, "nameplates/nameplates/american_flag/") == 0);
    CT_CHECK(strcmp(p->nameplate_palette, "cobalt") == 0);
    CT_CHECK(strcmp(p->guild_nameplate_asset, "nameplates/nameplates/twilight/") == 0);
    CT_CHECK(strcmp(p->guild_nameplate_palette, "berry") == 0);
    CT_CHECK(p->profile_effect_id == 1139323093991575696ULL);
    CT_CHECK(p->guild_profile_effect_id == 1139323093991575999ULL);
    CT_CHECK(p->profile_frame_sku_id == 1400000000000000001ULL);
    // Also propagated onto the embedded user struct.
    CT_CHECK(strcmp(ev.ev.users[0].avatar_decoration_url, p->avatar_decoration_url) == 0);
    CT_CHECK(strcmp(ev.ev.users[0].nameplate_asset, "nameplates/nameplates/american_flag/") == 0);
    CT_CHECK(strcmp(ev.ev.users[0].nameplate_palette, "cobalt") == 0);
  }
  ccord_live_event_free(&ev);

  // Path traversal in nameplate asset is rejected while parse still succeeds.
  static const char kBadNp[] =
    "{\"user\":{\"id\":\"602\",\"username\":\"bad\","
    "\"collectibles\":{\"nameplate\":{\"asset\":\"../secret/\",\"palette\":\"../bad\"}}}}";
  CT_CHECK(ccord_live_json_user_profile(kBadNp, strlen(kBadNp), 602, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.user_profile && ev.ev.user_profile->nameplate_asset[0] == '\0' &&
           ev.ev.user_profile->nameplate_palette[0] == '\0');
  ccord_live_event_free(&ev);

  // GET /user-profile-effects catalog parsing: zIndex ordering, randomizedSources, hostile URL rejection.
  static const char kEffects[] =
    "{\"profile_effect_configs\":["
    "{\"id\":\"1139323093991575696\",\"sku_id\":\"1139323093991575696\","
    "\"thumbnailPreviewSrc\":\"https://cdn.discordapp.com/assets/profile_effects/thumb.png\","
    "\"reducedMotionSrc\":\"https://cdn.discordapp.com/assets/profile_effects/rm.png\","
    "\"staticFrameSrc\":\"https://cdn.discordapp.com/assets/profile_effects/static.png\","
    "\"effects\":["
    "{\"src\":\"https://cdn.discordapp.com/assets/profile_effects/idle.png\",\"loop\":true,\"width\":450,\"height\":880,"
    "\"start\":2880,\"duration\":2880,\"loopDelay\":0,\"position\":{\"x\":0,\"y\":0},\"zIndex\":101},"
    "{\"src\":\"https://evil.example/pwn.png\",\"loop\":true,\"zIndex\":50},"
    "{\"src\":\"https://cdn.discordapp.com/assets/profile_effects/intro.png\",\"loop\":false,\"width\":450,\"height\":880,"
    "\"start\":0,\"duration\":2880,\"position\":{\"x\":0,\"y\":0},\"zIndex\":100}"
    "]}]}";
  ccord_profile_effect_config *items = nullptr;
  size_t count = 0;
  CT_CHECK(ccord_live_json_profile_effects(kEffects, strlen(kEffects), &items, &count, &err) == CCORD_OK);
  CT_CHECK(count == 1 && items != nullptr);
  if (items && count == 1) {
    CT_CHECK(items[0].id == 1139323093991575696ULL && items[0].sku_id == 1139323093991575696ULL);
    CT_CHECK(strcmp(items[0].reduced_motion_url, "https://cdn.discordapp.com/assets/profile_effects/rm.png") == 0);
    CT_CHECK(items[0].layer_count == 2);   // evil.example layer dropped
    // Sorted by zIndex: 100 (intro) before 101 (idle).
    CT_CHECK(items[0].layers[0].z_index == 100 && !items[0].layers[0].loop &&
             strstr(items[0].layers[0].src, "intro.png") != nullptr);
    CT_CHECK(items[0].layers[1].z_index == 101 && items[0].layers[1].loop && items[0].layers[1].start_ms == 2880);
  }
  ccord_free(items);

  // GET /collectibles-products/{sku_id} for PROFILE_FRAME (type 3).
  static const char kFrameProduct[] =
    "{\"sku_id\":\"1400000000000000001\",\"items\":[{"
    "\"type\":3,\"sku_id\":\"1400000000000000001\",\"inner_width\":300,"
    "\"overflow_top\":36,\"overflow_bottom\":18,\"overflow_horizontal\":14,"
    "\"assets\":{\"animated_image_url\":\"https://cdn.discordapp.com/assets/collectibles/cat/anim.png\","
    "\"static_image_url\":\"https://cdn.discordapp.com/assets/collectibles/cat/static.png\"},"
    "\"layers\":["
    "{\"id\":\"200\",\"type\":1,\"order\":2,\"anchor\":1,\"assets\":{\"animated_image_url\":\"https://cdn.discordapp.com/assets/collectibles/cat/bottom.png\"}},"
    "{\"id\":\"100\",\"type\":1,\"order\":1,\"anchor\":0,\"assets\":{\"animated_image_url\":\"https://cdn.discordapp.com/assets/collectibles/cat/top.png\"}}"
    "]}]}";
  ccord_profile_effect_config pe;
  ccord_profile_frame_config pf;
  bool has_pe = false, has_pf = false;
  CT_CHECK(ccord_live_json_collectible_product(kFrameProduct, strlen(kFrameProduct), 1400000000000000001ULL,
                                               &pe, &has_pe, &pf, &has_pf, &err) == CCORD_OK);
  CT_CHECK(!has_pe && has_pf);
  CT_CHECK(pf.sku_id == 1400000000000000001ULL && pf.inner_width == 300 && pf.overflow_top == 36 &&
           pf.overflow_bottom == 18 && pf.overflow_horizontal == 14);
  CT_CHECK(pf.layer_count == 2);
  // Sorted by order: 1 (anchor 0 = top) before 2 (anchor 1 = bottom).
  CT_CHECK(pf.layers[0].order == 1 && pf.layers[0].anchor == 0 && strstr(pf.layers[0].url, "top.png") != nullptr);
  CT_CHECK(pf.layers[1].order == 2 && pf.layers[1].anchor == 1 && strstr(pf.layers[1].url, "bottom.png") != nullptr);

  // Store propagation: profile upsert updates user avatar_decoration_url + nameplate and bumps revision.
  ccord_store *s = ccord_store_create(nullptr, &err);
  CT_CHECK(s != nullptr);
  if (s) {
    ccord_user_profile up = {
      .user_id = 601,
      .display_name = "Gavin",
      .avatar_decoration_url = "https://cdn.discordapp.com/avatar-decoration-presets/a_user_deco.png?size=128&passthrough=true",
      .profile_effect_id = 1139323093991575696ULL,
      .profile_frame_sku_id = 1400000000000000001ULL,
      .profile_frame = pf,
    };
    snprintf(up.nameplate_asset, sizeof up.nameplate_asset, "nameplates/nameplates/american_flag/");
    snprintf(up.nameplate_palette, sizeof up.nameplate_palette, "cobalt");
    CT_CHECK(ccord_store_upsert_user_profile(s, &up, &err) == CCORD_OK);
    const ccord_user *su = ccord_store_user(s, 601);
    CT_CHECK(su != nullptr && strstr(su->avatar_decoration_url, "a_user_deco") != nullptr);
    CT_CHECK(su != nullptr && strcmp(su->nameplate_asset, "nameplates/nameplates/american_flag/") == 0 &&
             strcmp(su->nameplate_palette, "cobalt") == 0);
    uint64_t rev1 = ccord_store_revision(s);
    // Re-upserting with same frame SKU but unresolved frame preserves the resolved frame AND bumps revision.
    ccord_user_profile up2 = up;
    up2.profile_frame = (ccord_profile_frame_config){0};
    CT_CHECK(ccord_store_upsert_user_profile(s, &up2, &err) == CCORD_OK);
    const ccord_user_profile *gp = ccord_store_user_profile(s, 601);
    CT_CHECK(gp != nullptr && gp->profile_frame.sku_id == 1400000000000000001ULL && gp->profile_frame.layer_count == 2);
    CT_CHECK(ccord_store_revision(s) > rev1);
    ccord_store_destroy(s);
  }
}

static void test_gateway_decor_parsing(void)
{
  // GUILD_MEMBER_LIST_UPDATE populates both user & member avatar decoration + nameplate without profile REST.
  static const char kMemberList[] =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"s\":1,\"d\":{"
    "\"guild_id\":\"3001\",\"id\":\"everyone\",\"member_count\":1,\"online_count\":1,"
    "\"ops\":[{\"op\":\"SYNC\",\"range\":[0,99],\"items\":[{\"member\":{"
    "\"nick\":\"Gavin\",\"roles\":[],"
    "\"avatar_decoration_data\":{\"asset\":\"a_srv_deco\"},"
    "\"collectibles\":{\"nameplate\":{\"asset\":\"nameplates/nameplates/american_flag/\",\"palette\":\"cobalt\"}},"
    "\"user\":{\"id\":\"701\",\"username\":\"gavin\",\"global_name\":\"Gavin\","
    "\"avatar_decoration_data\":{\"asset\":\"a_usr_deco\"},"
    "\"collectibles\":{\"nameplate\":{\"asset\":\"nameplates/nameplates/twilight/\",\"palette\":\"violet\"}}}"
    "}}]}]}}";
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_gateway_decode(kMemberList, strlen(kMemberList), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.user_count == 1 && ev.member_count == 1);
  if (ev.user_count == 1 && ev.member_count == 1) {
    CT_CHECK(strstr(ev.users[0].avatar_decoration_url, "a_usr_deco.png") != nullptr);
    CT_CHECK(strcmp(ev.users[0].nameplate_asset, "nameplates/nameplates/twilight/") == 0);
    CT_CHECK(strcmp(ev.users[0].nameplate_palette, "violet") == 0);
    CT_CHECK(strstr(ev.members[0].avatar_decoration_url, "a_srv_deco.png") != nullptr);
    CT_CHECK(strcmp(ev.members[0].nameplate_asset, "nameplates/nameplates/american_flag/") == 0);
    CT_CHECK(strcmp(ev.members[0].nameplate_palette, "cobalt") == 0);
  }
  ccord_live_event_free(&ev);

  // MESSAGE_CREATE populates author + partial member avatar decoration and nameplate.
  static const char kMsgCreate[] =
    "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"s\":2,\"d\":{"
    "\"id\":\"9001\",\"channel_id\":\"8001\",\"guild_id\":\"3001\",\"content\":\"hello\","
    "\"timestamp\":\"2026-10-25T12:00:00.000Z\","
    "\"author\":{\"id\":\"702\",\"username\":\"peppermint\",\"global_name\":\"Peppermint\","
    "\"avatar_decoration_data\":{\"asset\":\"a_pep_deco\"},"
    "\"collectibles\":{\"nameplate\":{\"asset\":\"nameplates/nameplates/sakura/\",\"palette\":\"berry\"}}},"
    "\"member\":{\"nick\":\"Pep\",\"roles\":[],\"avatar_decoration_data\":{\"asset\":\"a_pep_srv\"},"
    "\"collectibles\":{\"nameplate\":{\"asset\":\"nameplates/nameplates/cat/\",\"palette\":\"rose\"}}}}}";
  CT_CHECK(ccord_live_gateway_decode(kMsgCreate, strlen(kMsgCreate), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.user_count >= 1 && ev.member_count == 1);
  if (ev.user_count >= 1) {
    CT_CHECK(strstr(ev.users[0].avatar_decoration_url, "a_pep_deco.png") != nullptr);
    CT_CHECK(strcmp(ev.users[0].nameplate_asset, "nameplates/nameplates/sakura/") == 0);
    CT_CHECK(strcmp(ev.users[0].nameplate_palette, "berry") == 0);
  }
  if (ev.member_count == 1) {
    CT_CHECK(strstr(ev.members[0].avatar_decoration_url, "a_pep_srv.png") != nullptr);
    CT_CHECK(strcmp(ev.members[0].nameplate_asset, "nameplates/nameplates/cat/") == 0);
    CT_CHECK(strcmp(ev.members[0].nameplate_palette, "rose") == 0);
  }
  ccord_live_event_free(&ev);

  // GET /collectibles-products/{sku_id} for PROFILE_EFFECT (type 1) as well.
  static const char kEffectProduct[] =
    "{\"sku_id\":\"1500000000000000001\",\"items\":[{"
    "\"type\":1,\"id\":\"1500000000000000001\",\"sku_id\":\"1500000000000000001\","
    "\"reducedMotionSrc\":\"https://cdn.discordapp.com/assets/profile_effects/rm2.png\","
    "\"effects\":[{\"randomizedSources\":[{\"src\":\"https://cdn.discordapp.com/assets/profile_effects/rand.png\"}],"
    "\"loop\":true,\"width\":450,\"height\":880,\"start\":0,\"duration\":1500,\"zIndex\":10}]}]}";
  ccord_profile_effect_config pe;
  ccord_profile_frame_config pf;
  bool has_pe = false, has_pf = false;
  CT_CHECK(ccord_live_json_collectible_product(kEffectProduct, strlen(kEffectProduct), 1500000000000000001ULL,
                                               &pe, &has_pe, &pf, &has_pf, &err) == CCORD_OK);
  CT_CHECK(has_pe && !has_pf);
  CT_CHECK(pe.id == 1500000000000000001ULL && pe.layer_count == 1 &&
           strstr(pe.layers[0].src, "rand.png") != nullptr &&
           strcmp(pe.reduced_motion_url, "https://cdn.discordapp.com/assets/profile_effects/rm2.png") == 0);

  // Real Discord PROFILE_FRAME layer format: layers carry only asset snowflake `id` (no inline `assets`).
  static const char kRealFrameProduct[] =
    "{\"sku_id\":\"1411111111111111111\",\"items\":[{"
    "\"type\":3,\"sku_id\":\"1411111111111111111\",\"inner_width\":300,"
    "\"overflow_top\":36,\"overflow_bottom\":18,\"overflow_horizontal\":14,"
    "\"layers\":["
    "{\"id\":\"2222222222222222222\",\"type\":0,\"order\":1,\"anchor\":1,\"responsive\":false},"
    "{\"id\":\"1111111111111111111\",\"type\":0,\"order\":0,\"anchor\":0,\"responsive\":false}"
    "]}]}";
  CT_CHECK(ccord_live_json_collectible_product(kRealFrameProduct, strlen(kRealFrameProduct), 1411111111111111111ULL,
                                               &pe, &has_pe, &pf, &has_pf, &err) == CCORD_OK);
  CT_CHECK(!has_pe && has_pf && pf.layer_count == 2);
  CT_CHECK(strcmp(pf.layers[0].url,
                  "https://cdn.discordapp.com/media/v1/collectibles-shop/1411111111111111111/1111111111111111111/animated") == 0);
  CT_CHECK(strcmp(pf.layers[0].static_url,
                  "https://cdn.discordapp.com/media/v1/collectibles-shop/1411111111111111111/1111111111111111111/static") == 0);
  CT_CHECK(strcmp(pf.layers[1].url,
                  "https://cdn.discordapp.com/media/v1/collectibles-shop/1411111111111111111/2222222222222222222/animated") == 0);
}

int main(void)
{
  test_full_profile();
  test_absent_profile();
  test_mixed_profile();
  test_mock_profile_extras();
  test_store_owns_profile_extras();
  test_profile_decor_parsing();
  test_gateway_decor_parsing();
  return CT_RESULT("test_profile_json");
}
