// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ccord_test.h"
#include <ccord/net/ccord_cloud.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_pkce_rfc7636_vector(void)
{
  // RFC 7636 Appendix B test vector:
  // verifier  = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"
  // challenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
  char ch[64] = "";
  CT_CHECK(ccord_cloud_pkce_s256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", ch));
  CT_CHECK(strcmp(ch, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM") == 0);
  // Too short verifier rejected
  CT_CHECK(!ccord_cloud_pkce_s256("short", ch));
}

static void test_query_param_parser(void)
{
  char val[128];
  CT_CHECK(ccord_cloud_parse_query_param("code=ccode_abc123&state=xyz%2099", "code", val, sizeof val));
  CT_CHECK(strcmp(val, "ccode_abc123") == 0);
  CT_CHECK(ccord_cloud_parse_query_param("code=ccode_abc123&state=xyz%2099", "state", val, sizeof val));
  CT_CHECK(strcmp(val, "xyz 99") == 0);
  CT_CHECK(!ccord_cloud_parse_query_param("code=abc", "missing", val, sizeof val));
}

static void test_social_and_me_parsers(void)
{
  const char *social_json =
    "{"
    "\"plugin_id\":\"dev.alice.demo\","
    "\"rating\":{\"score\":8.3,\"score_x10\":83,\"sentiment\":\"positive\",\"pos\":2,\"neu\":1,\"neg\":0,\"total\":3,\"my_vote\":1},"
    "\"claimed_developers\":[{\"github_login\":\"alicedev\",\"source\":\"github_user\"}],"
    "\"comments\":{"
      "\"total\":2,\"has_more\":false,\"next_cursor\":0,"
      "\"comments\":[{"
        "\"id\":11,\"author_username\":\"bob\",\"author_display_name\":\"Bob\",\"plugin_version\":\"1.2.0\","
        "\"created_at\":\"2026-11-24T12:00:00Z\",\"body\":\"Works great!\",\"is_developer\":false,\"is_mine\":true,\"is_deleted\":false,"
        "\"replies\":[{"
          "\"id\":12,\"author_username\":\"alice\",\"author_display_name\":\"Alice\",\"plugin_version\":\"1.2.0\","
          "\"created_at\":\"2026-11-24T13:00:00Z\",\"body\":\"Thank you!\",\"is_developer\":true,\"is_mine\":false,\"is_deleted\":false"
        "}]"
      "}]"
    "}"
    "}";
  ccord_cloud_social soc = {0};
  CT_CHECK(ccord_cloud_parse_social_json(social_json, strlen(social_json), &soc));
  CT_CHECK(strcmp(soc.plugin_id, "dev.alice.demo") == 0);
  CT_CHECK(soc.rating.score_x10 == 83);
  CT_CHECK(soc.rating.sentiment == CCORD_RATING_POSITIVE);
  CT_CHECK(soc.rating.total == 3);
  CT_CHECK(soc.rating.has_my_vote && soc.rating.my_vote == 1);
  CT_CHECK(strcmp(soc.claimed_dev, "alicedev") == 0);
  CT_CHECK(soc.comment_count == 1);
  CT_CHECK(soc.comments[0].id == 11 && soc.comments[0].is_mine && !soc.comments[0].is_developer);
  CT_CHECK(strcmp(soc.comments[0].created_date, "2026-11-24") == 0);
  CT_CHECK(soc.comments[0].reply_count == 1);
  CT_CHECK(soc.comments[0].replies[0].is_developer);

  const char *me_json =
    "{"
    "\"authenticated\":true,"
    "\"companion_online\":true,"
    "\"current_device_id\":\"dev_1\","
    "\"discord_oauth_ready\":true,"
    "\"github_link_ready\":true,"
    "\"user\":{\"id\":\"usr_1\",\"discord_id\":\"175928847299117063\",\"username\":\"gavin\",\"display_name\":\"Gavin\",\"github_login\":\"gavindev\"},"
    "\"claimed_plugins\":[{\"id\":\"org.ccord.demo\",\"name\":\"Demo\",\"version\":\"1.0.0\",\"downloads\":42,\"rating\":{\"score_x10\":100,\"sentiment\":\"positive\",\"total\":1}}],"
    "\"devices\":[{\"id\":\"dev_1\",\"name\":\"Arch PC\",\"platform\":\"desktop\",\"role\":\"companion\",\"wake_enabled\":true,\"online\":true}]"
    "}";
  ccord_cloud_state st = {0};
  CT_CHECK(ccord_cloud_parse_me_json(me_json, strlen(me_json), &st));
  CT_CHECK(st.authenticated && st.companion_online);
  CT_CHECK(strcmp(st.username, "gavin") == 0 && strcmp(st.github_login, "gavindev") == 0);
  CT_CHECK(st.claimed_count == 1 && strcmp(st.claimed[0].id, "org.ccord.demo") == 0);
  CT_CHECK(st.device_count == 1 && st.devices[0].is_current && st.devices[0].online);
}

static void test_offline_lifecycle(void)
{
  ccord_cloud_config cfg = {
    .is_mobile = false,
    .disable_network = true,
  };
  ccord_cloud *c = ccord_cloud_create(&cfg);
  CT_CHECK(c != nullptr);
  ccord_cloud_state st;
  ccord_cloud_get_state(c, &st);
  CT_CHECK(!st.authenticated);
  CT_CHECK(!ccord_cloud_companion_online(c));
  ccord_wake_kind wk = CCORD_WAKE_MSG;
  CT_CHECK(!ccord_cloud_pop_wake(c, &wk) && wk == CCORD_WAKE_NONE);
  ccord_cloud_destroy(c);
}

int main(void)
{
  test_pkce_rfc7636_vector();
  test_query_param_parser();
  test_social_and_me_parsers();
  test_offline_lifecycle();
  return CT_RESULT("ccord_cloud");
}
