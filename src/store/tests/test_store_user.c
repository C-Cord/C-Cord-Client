// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/store/store.h>

#include "ccord_test.h"

static ccord_user_settings full_settings(void)
{
  ccord_user_settings st = {
    .default_guilds_restricted = true,
    .explicit_content_filter = 1,
    .friend_source_all = false,
    .friend_source_mutual_friends = true,
    .friend_source_mutual_guilds = false,
    .allow_access_to_age_restricted_servers = true,
    .developer_mode = true,
    .gif_auto_play = false,
    .animate_emoji = false,
    .enable_tts_command = true,
    .stream_notifications_enabled = true,
    .message_display_compact = true,
    .animate_stickers = 2,
  };
  memcpy(st.locale, "de", sizeof "de");
  return st;
}

static ccord_store *make_store(ccord_error *err)
{
  return ccord_store_create(nullptr, err);
}

static void reset_with_snapshot(ccord_store *s, ccord_error *err)
{
  ccord_user user = {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  ccord_snapshot snap = {.self_id = 1, .users = &user, .user_count = 1};
  CT_CHECK(ccord_store_set_snapshot(s, &snap, err) == CCORD_OK);
}

static void test_types(void)
{
  // Compile-time field contract
  ccord_auth_session session = {
    .id_hash = "abc", .os = "Linux", .browser = "Firefox", .device = "desktop",
    .location = "Berlin", .approx_last_used_ms = 1234, .current = true,
  };
  ccord_connection conn = {
    .id = "42", .name = "alex", .type = "github", .revoked = false, .verified = true,
    .friend_sync = true, .show_activity = false, .two_way_link = true, .visibility = 1,
  };
  ccord_connection_edit edit = {.visibility = 1, .friend_sync = 0, .show_activity = -1};
  ccord_user_settings_patch patch = CCORD_USER_SETTINGS_PATCH_INIT;
  CT_CHECK(session.current && session.approx_last_used_ms == 1234 && strcmp(session.os, "Linux") == 0);
  CT_CHECK(strcmp(conn.type, "github") == 0 && conn.verified && !conn.revoked && conn.visibility == 1);
  CT_CHECK(edit.visibility == 1 && edit.friend_sync == 0 && edit.show_activity == -1);
  CT_CHECK(patch.default_guilds_restricted == -1 && patch.explicit_content_filter == -1 &&
           patch.animate_stickers == -1 && patch.locale[0] == '\0');
  CT_CHECK(CCORD_FETCH_IDLE == 0 && CCORD_FETCH_LOADING == 1 && CCORD_FETCH_READY == 2 &&
           CCORD_FETCH_FAILED == 3);
}

static void test_defaults_and_full_update(void)
{
  ccord_error err;
  ccord_store *s = make_store(&err);
  CT_CHECK(s != nullptr);
  if (!s) return;

  ccord_user_settings arg_settings = full_settings();
  CT_CHECK(ccord_store_set_user_settings(nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_user_settings(nullptr, &arg_settings, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_set_user_settings(s, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: user settings arguments") == 0);
  CT_CHECK(ccord_store_user_settings(nullptr) == nullptr);
  CT_CHECK(ccord_store_user_settings(s) == nullptr);   // no authoritative object yet

  // A PATCH before the first full object starts from Discord's documented defaults.
  ccord_user_settings_patch patch = CCORD_USER_SETTINGS_PATCH_INIT;
  patch.developer_mode = 1;
  patch.explicit_content_filter = 2;
  patch.animate_stickers = 2;
  memcpy(patch.locale, "fr", sizeof "fr");
  uint64_t rev0 = ccord_store_revision(s), lists0 = ccord_store_lists_revision(s);
  CT_CHECK(ccord_store_apply_user_settings_patch(s, &patch, &err) == CCORD_OK);
  const ccord_user_settings *got = ccord_store_user_settings(s);
  CT_CHECK(got != nullptr);
  if (got) {
    CT_CHECK(got->developer_mode && got->explicit_content_filter == 2 && got->animate_stickers == 2);
    CT_CHECK(got->gif_auto_play && got->animate_emoji);
    CT_CHECK(!got->default_guilds_restricted && !got->message_display_compact);
    CT_CHECK(strcmp(got->locale, "fr") == 0);
  }
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_lists_revision(s) == lists0);   // traffic only

  // A full object replaces every field (no leftover patch state).
  ccord_user_settings full = full_settings();
  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_user_settings(s, &full, &err) == CCORD_OK);
  got = ccord_store_user_settings(s);
  CT_CHECK(got != nullptr);
  if (got) {
    CT_CHECK(got->default_guilds_restricted && got->explicit_content_filter == 1);
    CT_CHECK(!got->friend_source_all && got->friend_source_mutual_friends && !got->friend_source_mutual_guilds);
    CT_CHECK(got->allow_access_to_age_restricted_servers && got->developer_mode);
    CT_CHECK(!got->gif_auto_play && !got->animate_emoji && got->enable_tts_command);
    CT_CHECK(got->stream_notifications_enabled && got->message_display_compact);
    CT_CHECK(got->animate_stickers == 2 && strcmp(got->locale, "de") == 0);
  }
  CT_CHECK(ccord_store_revision(s) > rev0 && ccord_store_lists_revision(s) == lists0);

  // Re-applying the identical object is idempotent (no revision move, no allocation).
  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_set_user_settings(s, &full, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) == rev0);
  CT_CHECK(ccord_store_apply_user_settings_patch(s, &(ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT,
                                                 &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) == rev0);   // all sentinels: nothing changed

  // A locale-only change is still a real change (same struct, different inline text).
  ccord_user_settings same_but_locale = full;
  memcpy(same_but_locale.locale, "dE", sizeof "dE");
  CT_CHECK(ccord_store_set_user_settings(s, &same_but_locale, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev0 && strcmp(ccord_store_user_settings(s)->locale, "dE") == 0);

  // A real change moves the revision again.
  rev0 = ccord_store_revision(s);
  full.developer_mode = false;
  CT_CHECK(ccord_store_set_user_settings(s, &full, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) > rev0 && !ccord_store_user_settings(s)->developer_mode);

  ccord_store_destroy(s);
}

static void test_validation(void)
{
  ccord_error err;
  ccord_store *s = make_store(&err);
  CT_CHECK(s != nullptr);
  if (!s) return;

  ccord_user_settings bad = full_settings();
  bad.explicit_content_filter = 3;
  CT_CHECK(ccord_store_set_user_settings(s, &bad, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: explicit content filter") == 0);
  bad = full_settings();
  bad.animate_stickers = 3;
  CT_CHECK(ccord_store_set_user_settings(s, &bad, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: animate stickers") == 0);
  bad = full_settings();
  memset(bad.locale, 'x', sizeof bad.locale);   // no NUL in the inline array
  CT_CHECK(ccord_store_set_user_settings(s, &bad, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: locale text") == 0);
  bad = full_settings();
  memcpy(bad.locale, "\xff", sizeof "\xff");   // invalid UTF-8
  CT_CHECK(ccord_store_set_user_settings(s, &bad, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: locale text") == 0);
  CT_CHECK(ccord_store_user_settings(s) == nullptr);   // refused writes never land

  // The locale holds at most 15 characters plus the terminator; 16 bytes are accepted.
  bad = full_settings();
  memcpy(bad.locale, "abcdefghijklmno", sizeof "abcdefghijklmno");
  CT_CHECK(ccord_store_set_user_settings(s, &bad, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_store_user_settings(s)->locale, "abcdefghijklmno") == 0);

  ccord_store_destroy(s);
}

static void test_patch_validation(void)
{
  ccord_error err;
  ccord_store *s = make_store(&err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  ccord_user_settings full = full_settings();
  CT_CHECK(ccord_store_set_user_settings(s, &full, &err) == CCORD_OK);

  CT_CHECK(ccord_store_apply_user_settings_patch(nullptr, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_store_apply_user_settings_patch(s, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: user settings patch arguments") == 0);

  ccord_user_settings_patch bad = CCORD_USER_SETTINGS_PATCH_INIT;
  bad.friend_source_all = 2;
  CT_CHECK(ccord_store_apply_user_settings_patch(s, &bad, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: user settings patch flags") == 0);
  bad = (ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT;
  bad.animate_stickers = 3;
  CT_CHECK(ccord_store_apply_user_settings_patch(s, &bad, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: user settings patch values") == 0);
  bad = (ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT;
  memset(bad.locale, 'x', sizeof bad.locale);
  CT_CHECK(ccord_store_apply_user_settings_patch(s, &bad, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: user settings patch locale") == 0);
  bad = (ccord_user_settings_patch)CCORD_USER_SETTINGS_PATCH_INIT;
  memcpy(bad.locale, "\xff", sizeof "\xff");
  CT_CHECK(ccord_store_apply_user_settings_patch(s, &bad, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(err.message, "store: user settings patch locale") == 0);

  // One field changes; every other value stays exactly as stored.
  ccord_user_settings before = *ccord_store_user_settings(s);
  ccord_user_settings_patch one = CCORD_USER_SETTINGS_PATCH_INIT;
  one.gif_auto_play = 1;
  one.developer_mode = 0;
  one.explicit_content_filter = 0;
  one.animate_stickers = 0;
  memcpy(one.locale, "en-US", sizeof "en-US");
  uint64_t rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_apply_user_settings_patch(s, &one, &err) == CCORD_OK);
  const ccord_user_settings *got = ccord_store_user_settings(s);
  CT_CHECK(got != nullptr);
  if (got) {
    CT_CHECK(got->gif_auto_play && !got->developer_mode);
    CT_CHECK(got->explicit_content_filter == 0 && got->animate_stickers == 0);
    CT_CHECK(got->animate_emoji == before.animate_emoji);
    CT_CHECK(strcmp(got->locale, "en-US") == 0);
  }
  CT_CHECK(ccord_store_revision(s) > rev0);

  // With the stored locale already "en-US" the same patch is a silent no-op.
  rev0 = ccord_store_revision(s);
  CT_CHECK(ccord_store_apply_user_settings_patch(s, &one, &err) == CCORD_OK);
  CT_CHECK(ccord_store_revision(s) == rev0);

  ccord_store_destroy(s);
}

static void test_snapshot_and_destroy(void)
{
  ccord_error err;
  ccord_store *s = make_store(&err);
  CT_CHECK(s != nullptr);
  if (!s) return;

  ccord_user_settings full = full_settings();
  CT_CHECK(ccord_store_set_user_settings(s, &full, &err) == CCORD_OK);
  CT_CHECK(ccord_store_user_settings(s) != nullptr);
  reset_with_snapshot(s, &err);
  CT_CHECK(ccord_store_user_settings(s) == nullptr);   // a new READY owns the account state
  CT_CHECK(ccord_store_set_user_settings(s, &full, &err) == CCORD_OK);
  CT_CHECK(ccord_store_user_settings(s) != nullptr);
  ccord_store_destroy(s);

  // A settings write can be the first cache allocation on a fresh store (then destroy frees it).
  ccord_store *s2 = make_store(&err);
  CT_CHECK(s2 != nullptr);
  if (!s2) return;
  CT_CHECK(ccord_store_set_user_settings(s2, &full, &err) == CCORD_OK);
  ccord_store_destroy(s2);
}

int main(void)
{
  test_types();
  test_defaults_and_full_update();
  test_validation();
  test_patch_validation();
  test_snapshot_and_destroy();
  return CT_RESULT("test_store_user");
}
