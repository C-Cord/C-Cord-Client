// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>

#include <ccord/model/audit.h>

#include "ccord_test.h"

static void test_labels(void)
{
  CT_CHECK(strcmp(ccord_audit_action_label(22), "banned") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(23), "unbanned") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(20), "kicked") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(30), "created role") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(32), "deleted role") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(42), "deleted invite") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(1), "updated the server") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(25), "changed the roles of") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(60), "created emoji") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(61), "updated emoji") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(62), "deleted emoji") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(90), "created sticker") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(91), "updated sticker") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(92), "deleted sticker") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(130), "created soundboard sound") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(131), "updated soundboard sound") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(132), "deleted soundboard sound") == 0);
  // What this client does not know still reads as a sentence, never as null or "".
  CT_CHECK(strcmp(ccord_audit_action_label(0), "did something") == 0);
  CT_CHECK(strcmp(ccord_audit_action_label(9999), "did something") == 0);
}

static void test_every_label_is_unique_and_readable(void)
{
  const char *seen[256];
  size_t n = 0;
  for (uint16_t a = 1; a < 200; a++) {
    const char *l = ccord_audit_action_label(a);
    CT_CHECK(l && l[0] != '\0');
    if (strcmp(l, "did something") == 0) continue;
    for (size_t i = 0; i < n; i++) CT_CHECK(strcmp(seen[i], l) != 0);   // two actions must not read the same
    if (n < 256) seen[n++] = l;
  }
  CT_CHECK(n > 40);   // the table covers Discord's documented action types
}

static void test_categories(void)
{
  CT_CHECK(ccord_audit_action_category(1) == CCORD_AUDIT_CAT_SERVER);
  CT_CHECK(ccord_audit_action_category(12) == CCORD_AUDIT_CAT_CHANNEL);
  CT_CHECK(ccord_audit_action_category(111) == CCORD_AUDIT_CAT_CHANNEL);
  CT_CHECK(ccord_audit_action_category(22) == CCORD_AUDIT_CAT_MEMBER);
  CT_CHECK(ccord_audit_action_category(25) == CCORD_AUDIT_CAT_MEMBER);
  CT_CHECK(ccord_audit_action_category(31) == CCORD_AUDIT_CAT_ROLE);
  CT_CHECK(ccord_audit_action_category(40) == CCORD_AUDIT_CAT_INVITE);
  CT_CHECK(ccord_audit_action_category(60) == CCORD_AUDIT_CAT_OTHER);
  CT_CHECK(ccord_audit_action_category(91) == CCORD_AUDIT_CAT_OTHER);
  CT_CHECK(ccord_audit_action_category(130) == CCORD_AUDIT_CAT_OTHER);
  CT_CHECK(ccord_audit_action_category(131) == CCORD_AUDIT_CAT_OTHER);
  CT_CHECK(ccord_audit_action_category(132) == CCORD_AUDIT_CAT_OTHER);
  CT_CHECK(ccord_audit_matches(CCORD_AUDIT_CAT_OTHER, 130) && !ccord_audit_matches(CCORD_AUDIT_CAT_ROLE, 130));
  CT_CHECK(ccord_audit_action_category(72) == CCORD_AUDIT_CAT_OTHER);
  CT_CHECK(ccord_audit_action_category(9999) == CCORD_AUDIT_CAT_OTHER);
  for (uint16_t a = 0; a < 200; a++) CT_CHECK(ccord_audit_action_category(a) != CCORD_AUDIT_CAT_ALL);
}

static void test_filter(void)
{
  CT_CHECK(ccord_audit_matches(CCORD_AUDIT_CAT_ALL, 22) && ccord_audit_matches(CCORD_AUDIT_CAT_ALL, 9999));
  CT_CHECK(ccord_audit_matches(CCORD_AUDIT_CAT_MEMBER, 22));
  CT_CHECK(!ccord_audit_matches(CCORD_AUDIT_CAT_MEMBER, 30));
  CT_CHECK(ccord_audit_matches(CCORD_AUDIT_CAT_ROLE, 30) && !ccord_audit_matches(CCORD_AUDIT_CAT_ROLE, 22));
  CT_CHECK(ccord_audit_matches(CCORD_AUDIT_CAT_OTHER, 9999));
  for (ccord_audit_category c = CCORD_AUDIT_CAT_ALL; c < CCORD_AUDIT_CAT_COUNT; c++) {
    CT_CHECK(strlen(ccord_audit_category_label(c)) > 0);
  }
}

static void test_action_table(void)
{
  size_t n = 0;
  const ccord_audit_action_info *tbl = ccord_audit_action_table(&n);
  CT_CHECK(tbl != nullptr && n > 40);
  CT_CHECK(ccord_audit_action_table(nullptr) == tbl);
  for (size_t i = 0; i < n; i++) {
    CT_CHECK(tbl[i].action != 0);
    CT_CHECK(tbl[i].cat != CCORD_AUDIT_CAT_ALL && tbl[i].cat < CCORD_AUDIT_CAT_COUNT);
    CT_CHECK(tbl[i].label && tbl[i].label[0] != '\0');
    CT_CHECK(strcmp(ccord_audit_action_label(tbl[i].action), tbl[i].label) == 0);
    CT_CHECK(ccord_audit_action_category(tbl[i].action) == tbl[i].cat);
  }
}

int main(void)
{
  test_labels();
  test_every_label_is_unique_and_readable();
  test_categories();
  test_filter();
  test_action_table();
  return CT_RESULT("test_audit");
}
