// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/ui/account_view.h>

#include <string.h>

#include "ccord_test.h"

static const char *const k_expected_non_goals[6] = {
  "Server channels and messages",
  "Message history",
  "Live messages",
  "Sending messages",
  "Direct messages",
  "Voice",
};

static void make_identity(ccord_identity *id, bool truncated)
{
  ccord_identity_clear(id);
  id->user_id = 123456789012345678ULL;
  ccord_identity_set_name(id->username, "alice", strlen("alice"));
  ccord_identity_set_name(id->display_name, "Alice", strlen("Alice"));
  id->scope_mask = CCORD_IDENTITY_SCOPE_IDENTIFY | CCORD_IDENTITY_SCOPE_GUILDS;
  CT_CHECK(ccord_identity_add_guild(id, 1, "one", 3, true));
  CT_CHECK(ccord_identity_add_guild(id, 2, "two", 3, false));
  id->guilds_truncated = truncated;
}

static void check_non_goals(const ccord_account_view *v)
{
  CT_CHECK(v->non_goals != nullptr);
  CT_CHECK(v->non_goal_count == 6);
  if (!v->non_goals || v->non_goal_count != 6) return;
  for (size_t i = 0; i < 6; i++) {
    CT_CHECK(strcmp(v->non_goals[i], k_expected_non_goals[i]) == 0);
    size_t seen = 0;
    for (size_t j = 0; j < v->non_goal_count; j++) {
      if (strcmp(v->non_goals[j], k_expected_non_goals[i]) == 0) seen++;
    }
    CT_CHECK(seen == 1);
  }
}

// Relocated pure helpers (M0-8): name truncation, guild-list overflow and clear.
static void test_identity_helpers(void)
{
  char dst[CCORD_IDENTITY_NAME_BYTES + 1];

  // Code-point cap: 150 ASCII characters keep only the first 100, NUL-terminated.
  char ascii[150];
  memset(ascii, 'a', sizeof ascii);
  memset(dst, 'X', sizeof dst);
  ccord_identity_set_name(dst, ascii, sizeof ascii);
  CT_CHECK(strlen(dst) == CCORD_IDENTITY_NAME_MAX);
  CT_CHECK(dst[CCORD_IDENTITY_NAME_MAX] == '\0');

  // Exactly 100 characters are kept whole.
  ccord_identity_set_name(dst, ascii, CCORD_IDENTITY_NAME_MAX);
  CT_CHECK(strlen(dst) == CCORD_IDENTITY_NAME_MAX);

  // One character is copied (len > 0, not len > 1).
  ccord_identity_set_name(dst, "z", 1);
  CT_CHECK(strcmp(dst, "z") == 0);

  // 4-byte UTF-8: 120 emoji (480 bytes) cut to 100 complete code points (400 bytes).
  char emoji[120 * 4];
  for (size_t i = 0; i < 120; i++) {
    emoji[i * 4] = (char)0xF0;
    emoji[i * 4 + 1] = (char)0x9F;
    emoji[i * 4 + 2] = (char)0x98;
    emoji[i * 4 + 3] = (char)0x80;
  }
  ccord_identity_set_name(dst, emoji, sizeof emoji);
  CT_CHECK(strlen(dst) == CCORD_IDENTITY_NAME_BYTES);
  CT_CHECK((unsigned char)dst[0] == 0xF0 && (unsigned char)dst[396] == 0xF0);

  // Never end inside a UTF-8 sequence: two 3-byte chars with only 4 bytes offered keep just the first.
  ccord_identity_set_name(dst, "\xE2\x82\xAC\xE2\x82\xAC", 4);
  CT_CHECK(strlen(dst) == 3 && strcmp(dst, "\xE2\x82\xAC") == 0);
  // A trailing partial sequence is dropped, not copied byte-wise.
  ccord_identity_set_name(dst, "\xE2\x82\xAC\xE2\x82", 5);
  CT_CHECK(strlen(dst) == 3 && strcmp(dst, "\xE2\x82\xAC") == 0);
  // 2-byte sequences are kept whole and cut on their own boundary.
  ccord_identity_set_name(dst, "\xC3\xA9\xC3", 3);
  CT_CHECK(strlen(dst) == 2 && strcmp(dst, "\xC3\xA9") == 0);

  // Invalid lead bytes are copied alone rather than hidden.
  ccord_identity_set_name(dst, "\xFF" "a", 2);
  CT_CHECK(strlen(dst) == 2 && (unsigned char)dst[0] == 0xFF && dst[1] == 'a');

  // Empty / null inputs still NUL-terminate; a null dst is ignored.
  memset(dst, 'X', sizeof dst);
  ccord_identity_set_name(dst, "abc", 0);
  CT_CHECK(dst[0] == '\0');
  ccord_identity_set_name(dst, nullptr, 3);
  CT_CHECK(dst[0] == '\0');
  ccord_identity_set_name(nullptr, "abc", 3);

  // add_guild copies the name and owner flag, and counts entries.
  ccord_identity id = {0};
  ccord_identity_clear(&id);
  CT_CHECK(ccord_identity_add_guild(&id, 42, "guild", 5, true));
  CT_CHECK(id.guild_count == 1 && id.guilds[0].id == 42 && id.guilds[0].owner);
  CT_CHECK(strcmp(id.guilds[0].name, "guild") == 0);
  CT_CHECK(!id.guilds_truncated);
  for (size_t i = 1; i < CCORD_IDENTITY_MAX_GUILDS; i++) {
    CT_CHECK(ccord_identity_add_guild(&id, i, "g", 1, false));
  }
  CT_CHECK(id.guild_count == CCORD_IDENTITY_MAX_GUILDS && !id.guilds_truncated);
  // The 201st guild is refused, flags truncation and leaves the list untouched.
  CT_CHECK(!ccord_identity_add_guild(&id, 999, "overflow", 8, false));
  CT_CHECK(id.guilds_truncated);
  CT_CHECK(id.guild_count == CCORD_IDENTITY_MAX_GUILDS);
  CT_CHECK(id.guilds[CCORD_IDENTITY_MAX_GUILDS - 1].id == CCORD_IDENTITY_MAX_GUILDS - 1);
  CT_CHECK(!ccord_identity_add_guild(nullptr, 1, "x", 1, false));

  // clear zeroes every field.
  ccord_identity_clear(&id);
  CT_CHECK(id.user_id == 0 && id.guild_count == 0 && !id.guilds_truncated && id.scope_mask == 0);
  CT_CHECK(id.username[0] == '\0' && id.display_name[0] == '\0' && id.guilds == nullptr);
  ccord_identity_clear(nullptr);
}

int main(void)
{
  test_identity_helpers();

  ccord_identity id = {0}, truncated = {0};
  make_identity(&id, false);
  make_identity(&truncated, true);
  ccord_account_view v;

  ccord_account_view_build(&v, CCORD_ACCOUNT_SIGNED_OUT, CCORD_ACCOUNT_ERROR_NONE, true, nullptr);
  CT_CHECK(v.mode == CCORD_ACCOUNT_SIGNED_OUT);
  CT_CHECK(strcmp(v.status, "Not signed in") == 0);
  CT_CHECK(v.configured && !v.signed_in && !v.busy);
  CT_CHECK(v.can_sign_in && !v.can_sign_out && !v.can_refresh);
  CT_CHECK(!v.demo_chip_visible);
  check_non_goals(&v);

  ccord_account_view_build(&v, CCORD_ACCOUNT_UNAVAILABLE, CCORD_ACCOUNT_ERROR_NONE, false, nullptr);
  CT_CHECK(v.mode == CCORD_ACCOUNT_UNAVAILABLE);
  CT_CHECK(strcmp(v.status, "Not configured in this build") == 0);
  CT_CHECK(!v.configured && !v.can_sign_in && !v.signed_in && !v.demo_chip_visible);
  check_non_goals(&v);

  ccord_account_view_build(&v, CCORD_ACCOUNT_UNAVAILABLE, CCORD_ACCOUNT_ERROR_NONE, true, nullptr);
  CT_CHECK(strcmp(v.status, "Live sign-in is not available in this build") == 0);

  ccord_account_view_build(&v, CCORD_ACCOUNT_WAITING, CCORD_ACCOUNT_ERROR_NONE, true, nullptr);
  CT_CHECK(strcmp(v.status, "Waiting for your browser") == 0);
  CT_CHECK(v.busy && !v.can_sign_in && !v.signed_in && !v.demo_chip_visible);
  CT_CHECK(v.non_goal_count == 0);

  ccord_account_view_build(&v, CCORD_ACCOUNT_FINISHING, CCORD_ACCOUNT_ERROR_NONE, true, nullptr);
  CT_CHECK(strcmp(v.status, "Finishing sign-in") == 0);
  CT_CHECK(v.busy && !v.signed_in && !v.demo_chip_visible);

  ccord_account_view_build(&v, CCORD_ACCOUNT_SIGNED_IN, CCORD_ACCOUNT_ERROR_NONE, true, &id);
  CT_CHECK(strcmp(v.status, "Signed in") == 0);
  CT_CHECK(v.signed_in && v.demo_chip_visible && !v.can_sign_in);
  CT_CHECK(v.can_sign_out && v.can_refresh && !v.busy);
  CT_CHECK(strcmp(v.display_name, "Alice") == 0);
  CT_CHECK(strcmp(v.username, "alice") == 0);
  CT_CHECK(strcmp(v.user_id_text, "123456789012345678") == 0);
  CT_CHECK(v.guild_count == 2 && !v.guilds_truncated);
  check_non_goals(&v);

  ccord_account_view_build(&v, CCORD_ACCOUNT_SIGNED_IN_TRUNCATED, CCORD_ACCOUNT_ERROR_NONE, true, &truncated);
  CT_CHECK(strcmp(v.status, "Signed in (guild list truncated)") == 0);
  CT_CHECK(v.signed_in && v.guilds_truncated && v.demo_chip_visible && v.guild_count == 2);
  check_non_goals(&v);

  ccord_account_view_build(&v, CCORD_ACCOUNT_ERROR, CCORD_ACCOUNT_ERROR_DENIED, true, nullptr);
  CT_CHECK(strcmp(v.status, "Sign-in failed") == 0);
  CT_CHECK(v.error == CCORD_ACCOUNT_ERROR_DENIED);
  CT_CHECK(strcmp(ccord_account_error_name(v.error), "DENIED") == 0);
  CT_CHECK(strcmp(v.detail, "Discord did not grant access. Try again when you are ready.") == 0);
  CT_CHECK(v.can_sign_in && !v.signed_in && !v.demo_chip_visible);
  check_non_goals(&v);

  ccord_account_view_build(&v, CCORD_ACCOUNT_ERROR, CCORD_ACCOUNT_ERROR_NETWORK, true, nullptr);
  CT_CHECK(v.error == CCORD_ACCOUNT_ERROR_NETWORK);
  CT_CHECK(strcmp(ccord_account_error_name(v.error), "NETWORK") == 0);
  CT_CHECK(strcmp(v.detail, "Could not reach Discord. Check the connection and try again.") == 0);

  CT_CHECK(strcmp(ccord_account_mode_name(CCORD_ACCOUNT_SIGNED_IN), "signed_in") == 0);
  CT_CHECK(strcmp(ccord_account_mode_name(CCORD_ACCOUNT_SIGNED_IN_TRUNCATED), "signed_in_truncated") == 0);

  // A signed-in mode without identity degrades to a readable error instead of dereferencing null.
  ccord_account_view_build(&v, CCORD_ACCOUNT_SIGNED_IN, CCORD_ACCOUNT_ERROR_NONE, true, nullptr);
  CT_CHECK(v.mode == CCORD_ACCOUNT_ERROR && v.error == CCORD_ACCOUNT_ERROR_BAD_RESPONSE);

  ccord_identity_clear(&id);
  ccord_identity_clear(&truncated);
  return CT_RESULT("test_ui_account_view");
}
