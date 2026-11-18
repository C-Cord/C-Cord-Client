// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/model/report.h>

#include "ccord_test.h"

// The design's sample (a one-node user menu, from the community docs).
static const char SAMPLE[] =
  "{\"name\":\"user\",\"variant\":\"3\",\"version\":\"1.0\",\"postback_url\":\"/api/reporting/user\",\"root_node_id\":1,"
  "\"success_node_id\":1,\"fail_node_id\":1,\"nodes\":{\"1\":{\"id\":1,\"key\":\"GENERIC_SUBMIT\",\"header\":\"Report Summary\","
  "\"subheader\":\"Review your report before submitting.\",\"info\":null,\"button\":{\"type\":\"submit\",\"target\":null},"
  "\"elements\":[{\"name\":\"breadcrumbs\",\"type\":\"breadcrumbs\",\"data\":null,\"should_submit_data\":false,"
  "\"skip_if_unlocalized\":false,\"is_localized\":true}],\"report_type\":null,\"children\":[],"
  "\"is_multi_select_required\":false,\"is_auto_submit\":false}}}";

// A three-step menu: root -> (child) -> submit node, with a checkbox, a dropdown, free text and an unknown element.
static const char WALK[] =
  "{\"name\":\"message\",\"variant\":\"3\",\"version\":\"1.0\",\"language\":\"en\",\"root_node_id\":1,\"success_node_id\":9,"
  "\"fail_node_id\":8,\"nodes\":{"
  "\"1\":{\"id\":1,\"key\":\"START\",\"header\":\"What is wrong?\",\"children\":[[\"Spam\",2],{\"name\":\"Abuse\",\"target_node_id\":3}],"
  "\"elements\":[],\"button\":null,\"is_multi_select_required\":true,\"is_auto_submit\":false},"
  "\"2\":{\"id\":2,\"key\":\"SPAM\",\"header\":\"Spam\",\"children\":[[\"Continue\",3]],\"button\":{\"type\":\"next\",\"target\":3},"
  "\"elements\":[{\"name\":\"spam_kind\",\"type\":\"checkbox\",\"should_submit_data\":true,"
  "\"data\":[[\"phishing\",\"Phishing\",\"Scam links\"],[\"ads\",\"Ads\"]]},"
  "{\"name\":\"future\",\"type\":\"hologram\",\"data\":{\"x\":1}}]},"
  "\"3\":{\"id\":3,\"key\":\"SUBMIT\",\"header\":\"Review\",\"children\":[],\"button\":{\"type\":\"submit\",\"target\":null},"
  "\"elements\":[{\"name\":\"how\",\"type\":\"dropdown\",\"should_submit_data\":true,"
  "\"data\":{\"title\":\"How\",\"options\":[{\"value\":\"a\",\"label\":\"A\"}]}},"
  "{\"name\":\"note\",\"type\":\"free_text\",\"should_submit_data\":true,\"data\":{\"placeholder\":\"why\",\"rows\":3}}]},"
  "\"4\":{\"id\":5,\"key\":\"MISMATCH\"}}}";

static void test_sample_and_walk(void)
{
  ccord_report_menu m;
  ccord_error err = {0};
  CT_CHECK(ccord_report_menu_parse(&m, SAMPLE, sizeof SAMPLE - 1, &err) == CCORD_OK);
  CT_CHECK(m.node_count == 1 && m.root == 1 && strcmp(m.variant, "3") == 0 && strcmp(m.version, "1.0") == 0);
  const ccord_report_node *n = ccord_report_menu_node(&m, m.root);
  CT_CHECK(n && n->button == CCORD_REPORT_BTN_SUBMIT && !n->has_button_target);
  CT_CHECK(n && n->element_count == 1 && n->elements[0].kind == CCORD_REPORT_EL_BREADCRUMBS);
  ccord_report_menu_free(&m);

  CT_CHECK(ccord_report_menu_parse(&m, WALK, sizeof WALK - 1, &err) == CCORD_OK);
  CT_CHECK(m.node_count == 3);                              // node "4" whose id is 5 is dropped
  CT_CHECK(ccord_report_menu_node(&m, 5) == nullptr && ccord_report_menu_node(&m, 4) == nullptr);
  const ccord_report_node *cur = ccord_report_menu_node(&m, m.root);
  CT_CHECK(cur && cur->multi_select_required && cur->child_count == 2);
  CT_CHECK(cur && strcmp(cur->children[0].name, "Spam") == 0 && cur->children[1].target == 3);
  uint32_t path[CCORD_REPORT_MAX_PATH];
  size_t pn = 0;
  path[pn++] = cur->id;
  cur = ccord_report_menu_node(&m, cur->children[0].target);
  CT_CHECK(cur && cur->element_count == 2 && cur->elements[1].kind == CCORD_REPORT_EL_UNKNOWN);
  CT_CHECK(cur && cur->elements[0].kind == CCORD_REPORT_EL_CHECKBOX && cur->elements[0].choice_count == 2);
  CT_CHECK(cur && strcmp(cur->elements[0].choices[0].value, "phishing") == 0 &&
           strcmp(cur->elements[0].choices[0].description, "Scam links") == 0);
  CT_CHECK(cur && cur->button == CCORD_REPORT_BTN_NEXT && cur->has_button_target && cur->button_target == 3);
  path[pn++] = cur->id;
  cur = ccord_report_menu_node(&m, cur->button_target);
  CT_CHECK(cur && cur->button == CCORD_REPORT_BTN_SUBMIT);
  CT_CHECK(cur && cur->elements[0].kind == CCORD_REPORT_EL_DROPDOWN && cur->elements[0].choice_count == 1);
  CT_CHECK(cur && cur->elements[1].kind == CCORD_REPORT_EL_FREE_TEXT && cur->elements[1].rows == 3 &&
           cur->elements[1].character_limit == 800);
  path[pn++] = cur->id;

  const char *kinds[] = {"phishing", "ads"};
  const char *text[] = {"he spams \"me\""};
  ccord_report_value vals[] = {{"spam_kind", kinds, 2}, {"note", text, 1}};
  ccord_report_request req = {.type = CCORD_REPORT_MESSAGE, .version = m.version, .variant = m.variant, .language = m.language,
                              .breadcrumbs = path, .breadcrumb_count = pn, .elements = vals, .element_count = 2,
                              .channel_id = 222, .message_id = 333};
  ccord_buf b;
  ccord_buf_init(&b, 0);
  CT_CHECK(ccord_report_build_body(&req, &b, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b),
                  "{\"version\":\"1.0\",\"variant\":\"3\",\"name\":\"message\",\"language\":\"en\",\"breadcrumbs\":[1,2,3],"
                  "\"elements\":{\"spam_kind\":[\"phishing\",\"ads\"],\"note\":[\"he spams \\\"me\\\"\"]},"
                  "\"channel_id\":\"222\",\"message_id\":\"333\"}") == 0);

  // User report: language defaults to "en", no elements, reported_user_id only.
  ccord_buf_clear(&b);
  uint32_t p1[] = {1};
  ccord_report_request ur = {.type = CCORD_REPORT_USER, .version = "1.0", .variant = "3", .breadcrumbs = p1, .breadcrumb_count = 1,
                             .reported_user_id = 444, .channel_id = 9, .message_id = 9};
  CT_CHECK(ccord_report_build_body(&ur, &b, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b),
                  "{\"version\":\"1.0\",\"variant\":\"3\",\"name\":\"user\",\"language\":\"en\",\"breadcrumbs\":[1],"
                  "\"elements\":{},\"reported_user_id\":\"444\"}") == 0);

  // Validation.
  ccord_report_request bad = ur;
  bad.reported_user_id = 0;
  CT_CHECK(ccord_report_build_body(&bad, &b, &err) != CCORD_OK);
  bad = req;
  bad.message_id = 0;
  CT_CHECK(ccord_report_build_body(&bad, &b, &err) != CCORD_OK);
  bad = ur;
  bad.breadcrumb_count = 0;
  CT_CHECK(ccord_report_build_body(&bad, &b, &err) != CCORD_OK);
  bad.breadcrumb_count = CCORD_REPORT_MAX_PATH + 1;
  CT_CHECK(ccord_report_build_body(&bad, &b, &err) != CCORD_OK);
  const char *badutf[] = {"\xff\xfe"};
  ccord_report_value bv[] = {{"note", badutf, 1}};
  bad = req;
  bad.elements = bv;
  bad.element_count = 1;
  CT_CHECK(ccord_report_build_body(&bad, &b, &err) != CCORD_OK);
  ccord_buf_free(&b);
  ccord_report_menu_free(&m);
}

static void test_big_menu(void)
{
  // 3000 nodes with 4 JSON values each: well over the default 8192-node limit.
  ccord_buf b;
  ccord_buf_init(&b, 0);
  CT_CHECK(ccord_buf_append_str(&b, "{\"name\":\"message\",\"version\":\"1.0\",\"variant\":\"3\",\"root_node_id\":1,"
                                    "\"success_node_id\":1,\"fail_node_id\":1,\"nodes\":{") == CCORD_OK);
  enum { N = 3000 };
  for (int i = 1; i <= N; i++) {
    CT_CHECK(ccord_buf_appendf(&b, "%s\"%d\":{\"id\":%d,\"key\":\"K%d\",\"header\":\"h\",\"children\":[[\"c\",1]],\"elements\":[]}",
                               i > 1 ? "," : "", i, i, i) == CCORD_OK);
  }
  CT_CHECK(ccord_buf_append_str(&b, "}}") == CCORD_OK);
  ccord_report_menu m;
  ccord_error err = {0};
  CT_CHECK(ccord_report_menu_parse(&m, ccord_buf_cstr(&b), b.len, &err) == CCORD_OK);
  CT_CHECK(m.node_count == N && ccord_report_menu_node(&m, N) != nullptr);
  ccord_report_menu_free(&m);
  ccord_buf_free(&b);
}

static void test_malformed(void)
{
  ccord_report_menu m;
  ccord_error err = {0};
  size_t full = sizeof WALK - 1;
  // Every truncation fails and leaves an empty menu (the sanitizer build reports any leak).
  for (size_t cut = 0; cut < full; cut += 7) {
    CT_CHECK(ccord_report_menu_parse(&m, WALK, cut, &err) != CCORD_OK);
    CT_CHECK(m.nodes == nullptr && m.node_count == 0);
    ccord_report_menu_free(&m);
  }
  const char *bad[] = {"", "[]", "{}", "{\"nodes\":{},\"root_node_id\":1}",
                       "{\"nodes\":{\"2\":{\"id\":2}},\"root_node_id\":1}", "{\"root_node_id\":1,\"nodes\":[1]}"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    CT_CHECK(ccord_report_menu_parse(&m, bad[i], strlen(bad[i]), &err) == CCORD_ERR_PARSE);
    ccord_report_menu_free(&m);
  }
  // Allocation failure at every step must fail cleanly or succeed, never leak.
  for (long k = 0; k < 60; k++) {
    ccord_alloc_test_fail_after(k);
    ccord_status s = ccord_report_menu_parse(&m, WALK, full, &err);
    ccord_alloc_test_fail_after(-1);
    if (s == CCORD_OK) ccord_report_menu_free(&m);
    else CT_CHECK(m.nodes == nullptr);
  }
  CT_CHECK(ccord_report_type_name(CCORD_REPORT_TYPE_COUNT) == nullptr);
}

int main(void)
{
  test_sample_and_walk();
  test_big_menu();
  test_malformed();
  return CT_RESULT("test_report");
}
