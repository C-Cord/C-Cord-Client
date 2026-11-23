// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Desktop & Mobile integration tests for the Discord live-audit feature additions:
// search filters/sort, Quick Switcher $ actions, built-in slash commands, reminders & 5-tab Inbox,
// Hide Muted / channel pinning / voice Hide Names / Announcement Follow, quick Audio Device popover,
// Server Guide & Channels & Roles, Split-View Threads, System theme sync, 24h time format,
// accessibility controls (role colors, always underline links, spoiler policy), and Role Icons.
#include "ui_interaction_harness.h"
#include "../chat/chat_internal.h"
#include "../settings/settings_m6_prefs.h"
#include <ccord/model/search.h>
#include <ccord/ui/timefmt.h>

static void enter_mobile(void)
{
  ccord_ui_set_touch_mode(F.ui, true);
  ccord_ui_set_safe_insets(F.ui, 44.0f, 0.0f, 34.0f, 0.0f);
  ccord_ui_set_viewport(F.ui, 360.0f, 780.0f);
  frame(4);
  CT_CHECK(F.ui->mobile_layout);
}

static void enter_desktop(void)
{
  ccord_ui_set_touch_mode(F.ui, false);
  ccord_ui_set_safe_insets(F.ui, 0.0f, 0.0f, 0.0f, 0.0f);
  ccord_ui_set_viewport(F.ui, 1280.0f, 800.0f);
  frame(4);
  CT_CHECK(!F.ui->mobile_layout);
}

static void test_search_filters_and_quick_switcher(void)
{
  // 1. Model search parser: has:forward, author_type:bot, sort:old / sort:relevant.
  ccord_search_query q = {0};
  const char *s1 = "has:forward author_type:bot sort:old hello";
  ccord_search_parse(s1, strlen(s1), &q);
  CT_CHECK((q.has_mask & CCORD_SEARCH_HAS_FORWARD) != 0);
  CT_CHECK(q.author_type == CCORD_SEARCH_AUTHOR_BOT);
  CT_CHECK(q.sort == CCORD_SEARCH_SORT_OLD);
  CT_CHECK(strcmp(q.content, "hello") == 0);

  const char *s2 = "sort:relevant author_type:webhook";
  ccord_search_parse(s2, strlen(s2), &q);
  CT_CHECK(q.sort == CCORD_SEARCH_SORT_RELEVANT);
  CT_CHECK(q.author_type == CCORD_SEARCH_AUTHOR_WEBHOOK);

  // 2. Desktop Search Popover chips & results sort chips.
  enter_desktop();
  click_rect(ui_search_rect(F.ui));
  frame(2);
  CT_CHECK(F.ui->search_open);
  type_text("hello");
  key(CCORD_KEY_RETURN, 0);
  frame(6);
  CT_CHECK(F.ui->search_sort_rects[1].w > 0.0f && F.ui->search_sort_rects[2].w > 0.0f);
  click_rect(F.ui->search_sort_rects[1]);   // Old
  frame(4);
  CT_CHECK(F.ui->search_sort == CCORD_SEARCH_SORT_OLD);
  click_rect(F.ui->search_sort_rects[2]);   // Relevant
  frame(4);
  CT_CHECK(F.ui->search_sort == CCORD_SEARCH_SORT_RELEVANT);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!F.ui->search_open);

  // 3. Quick Switcher $ quick actions.
  key(CCORD_KEY_K, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(F.ui->quick_switcher_open);
  type_text("$inbox");
  frame(2);
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(!F.ui->quick_switcher_open && F.ui->inbox_open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!F.ui->inbox_open);
}

static void test_builtin_commands_reminders_and_inbox(void)
{
  enter_desktop();
  ccord_id gid = ccord_store_selected_guild(F.store);
  ccord_id cid = find_channel(gid, "general", CCORD_CHANNEL_TEXT);
  CT_CHECK(cid != CCORD_ID_NONE);
  ccord_error err;
  (void)ccord_session_select_channel(F.session, cid, &err);
  frame(3);

  // /shrug expands to shrug emoticon when sent.
  size_t before = ccord_store_message_count(F.store, cid);
  clear_composer_input();
  type_text("/shrug hello");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(ccord_store_message_count(F.store, cid) == before + 1);
  const ccord_message *m = ccord_store_message_at(F.store, cid, before);
  CT_CHECK(m && m->content && strstr(m->content, "hello") != nullptr && strstr(m->content, "\\_(\xE3\x83\x84)_/") != nullptr);

  // /spoiler wraps message in ||...||.
  before = ccord_store_message_count(F.store, cid);
  clear_composer_input();
  type_text("/spoiler secret");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(ccord_store_message_count(F.store, cid) == before + 1);
  m = ccord_store_message_at(F.store, cid, before);
  CT_CHECK(m && m->content && strcmp(m->content, "||secret||") == 0);

  // /schedule opens schedule dialog cleanly (first Enter accepts slash autocomplete, second executes); Esc closes.
  clear_composer_input();
  type_text("/schedule");
  key(CCORD_KEY_RETURN, 0);
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(F.ui->schedule_open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!F.ui->schedule_open);

  // Reminders: add, view in 5-tab Inbox (tab 3 = Reminders), tick fires notice.
  ui_reminder_add(F.ui, cid, m ? m->id : 100u, 15 * 60 * 1000LL);
  CT_CHECK(F.ui->nreminders >= 1);
  click_rect(ui_inbox_rect(F.ui));
  frame(2);
  CT_CHECK(F.ui->inbox_open);
  for (int t = 0; t < UI_INBOX_TAB_COUNT; t++) {
    CT_CHECK(F.ui->inbox_tab_rects[t].w > 0.0f && F.ui->inbox_tab_rects[t].h > 0.0f);
  }
  click_rect(F.ui->inbox_tab_rects[3]);   // Reminders tab
  frame(2);
  CT_CHECK(F.ui->inbox_tab == 3);
  click_rect(F.ui->inbox_tab_rects[4]);   // Scheduled tab
  frame(2);
  CT_CHECK(F.ui->inbox_tab == 4);

  // Mobile 5-tab Inbox: touch targets >= 36px and Back closes cleanly.
  enter_mobile();
  CT_CHECK(F.ui->inbox_open);
  CT_CHECK(F.ui->inbox_tab_rects[0].h >= 36.0f && F.ui->inbox_tab_rects[3].h >= 36.0f);
  CT_CHECK(F.ui->inbox_popover_rect.y >= F.ui->safe_insets.top);
  CT_CHECK(ccord_ui_handle_back(F.ui));
  frame(2);
  CT_CHECK(!F.ui->inbox_open);

  // Force reminder due and verify tick marks fired and emits a notice.
  F.ui->reminders[0].due_unix_ms = F.ui->now_unix_ms - 1000LL;
  F.ui->reminders[0].fired = false;
  F.ui->reminder_last_check_ms = 0;
  F.ui->notice[0] = '\0';
  frame(2);
  CT_CHECK(F.ui->reminders[0].fired && strstr(F.ui->notice, "Reminder") != nullptr);
  ui_reminder_remove(F.ui, 0);
}

static void test_channel_pins_hide_muted_and_audio_popover(void)
{
  enter_desktop();
  ccord_id gid = ccord_store_selected_guild(F.store);
  ccord_id dev_id = find_channel(gid, "dev", CCORD_CHANNEL_TEXT);
  ccord_id gen_id = find_channel(gid, "general", CCORD_CHANNEL_TEXT);
  CT_CHECK(dev_id != CCORD_ID_NONE && gen_id != CCORD_ID_NONE);

  // Pin #dev to top of guild channel list.
  ui_channel_toggle_pin(F.ui, dev_id);
  frame(2);
  CT_CHECK(ui_channel_is_pinned(F.ui, dev_id));
  size_t dev_row = SIZE_MAX, gen_row = SIZE_MAX;
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    if (F.ui->crow[i].dm_nav != DM_NAV_NONE || F.ui->crow[i].is_category || F.ui->crow[i].is_voice_user) continue;
    const ccord_channel *c = ccord_store_channel_at(F.store, gid, F.ui->crow[i].index);
    if (c && c->id == dev_id) dev_row = i;
    if (c && c->id == gen_id) gen_row = i;
  }
  CT_CHECK(dev_row != SIZE_MAX && gen_row != SIZE_MAX && dev_row < gen_row);
  ui_channel_toggle_pin(F.ui, dev_id);
  frame(2);
  CT_CHECK(!ui_channel_is_pinned(F.ui, dev_id));

  // Hide Muted Channels toggle.
  CT_CHECK(!ui_guild_hide_muted(F.ui, gid));
  ui_guild_toggle_hide_muted(F.ui, gid);
  frame(2);
  CT_CHECK(ui_guild_hide_muted(F.ui, gid));
  ui_guild_toggle_hide_muted(F.ui, gid);
  frame(2);
  CT_CHECK(!ui_guild_hide_muted(F.ui, gid));

  // Voice Hide Names toggle.
  ccord_id vc_id = find_channel(gid, "Lounge", CCORD_CHANNEL_VOICE);
  if (vc_id != CCORD_ID_NONE) {
    ui_voice_toggle_names_hidden(F.ui, vc_id);
    frame(2);
    CT_CHECK(ui_voice_names_hidden(F.ui, vc_id));
    ui_voice_toggle_names_hidden(F.ui, vc_id);
    frame(2);
    CT_CHECK(!ui_voice_names_hidden(F.ui, vc_id));
  }

  // Announcement channel Follow header button + destination picker modal.
  ccord_channel ann = {
    .id = 993101u, .guild_id = gid, .kind = CCORD_CHANNEL_TEXT,
    .discord_type = CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT, .name = "releases", .topic = "Release notes",
  };
  ccord_error err;
  CT_CHECK(ccord_store_upsert_channel(F.store, &ann, &err) == CCORD_OK);
  (void)ccord_session_select_channel(F.session, ann.id, &err);
  frame(3);
  CT_CHECK(F.ui->announce_follow_rect.w > 0.0f && F.ui->announce_follow_rect.h > 0.0f);
  click_rect(F.ui->announce_follow_rect);
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_FOLLOW_CHANNEL);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);
  (void)ccord_session_select_channel(F.session, gen_id, &err);
  frame(2);

  // Quick Audio Device carets & popover (Desktop + Mobile).
  ccord_rect mc = ui_audio_caret_rect(F.ui, 0);
  CT_CHECK(mc.w > 0.0f && mc.h > 0.0f);
  click_rect(mc);
  frame(2);
  CT_CHECK(ui_audio_popover_is_open(F.ui));
  // Esc closes.
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!ui_audio_popover_is_open(F.ui));

  // Output caret opens output popover; mobile Back closes it cleanly.
  ccord_rect dc = ui_audio_caret_rect(F.ui, 1);
  click_rect(dc);
  frame(2);
  CT_CHECK(ui_audio_popover_is_open(F.ui));
  enter_mobile();
  CT_CHECK(ui_audio_popover_is_open(F.ui));
  CT_CHECK(ccord_ui_handle_back(F.ui));
  frame(2);
  CT_CHECK(!ui_audio_popover_is_open(F.ui));
}

static void test_server_guide_channels_roles_and_split_threads(void)
{
  enter_desktop();
  ccord_id gid = ccord_store_selected_guild(F.store);
  ccord_id gen_id = find_channel(gid, "general", CCORD_CHANNEL_TEXT);
  ccord_id dev_id = find_channel(gid, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(gen_id != CCORD_ID_NONE && dev_id != CCORD_ID_NONE);

  // Community guild displays Server Guide & Channels & Roles rows at the top of crow.
  bool saw_guide = false, saw_cr = false;
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    if (F.ui->crow[i].dm_nav == DM_NAV_SERVER_GUIDE) saw_guide = true;
    if (F.ui->crow[i].dm_nav == DM_NAV_CHANNELS_ROLES) saw_cr = true;
  }
  CT_CHECK(saw_guide && saw_cr);

  // Open Server Guide and Channels & Roles.
  ui_guild_view_open(F.ui, UI_GUILD_VIEW_SERVER_GUIDE);
  frame(3);
  CT_CHECK(ui_guild_view_active(F.ui) && F.ui->guild_view == UI_GUILD_VIEW_SERVER_GUIDE);

  ui_guild_view_open(F.ui, UI_GUILD_VIEW_CHANNELS_ROLES);
  frame(3);
  CT_CHECK(ui_guild_view_active(F.ui) && F.ui->guild_view == UI_GUILD_VIEW_CHANNELS_ROLES);

  // Hide #dev via Browse Channels toggle -> disappears from sidebar until Show All Channels is enabled.
  ui_channel_toggle_hidden(F.ui, dev_id);
  frame(2);
  CT_CHECK(ui_channel_is_hidden(F.ui, dev_id));
  bool dev_visible = false;
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    if (F.ui->crow[i].dm_nav != DM_NAV_NONE || F.ui->crow[i].is_category || F.ui->crow[i].is_voice_user) continue;
    const ccord_channel *c = ccord_store_channel_at(F.store, gid, F.ui->crow[i].index);
    if (c && c->id == dev_id) dev_visible = true;
  }
  CT_CHECK(!dev_visible);
  ui_guild_toggle_show_all_channels(F.ui, gid);
  frame(2);
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    if (F.ui->crow[i].dm_nav != DM_NAV_NONE || F.ui->crow[i].is_category || F.ui->crow[i].is_voice_user) continue;
    const ccord_channel *c = ccord_store_channel_at(F.store, gid, F.ui->crow[i].index);
    if (c && c->id == dev_id) dev_visible = true;
  }
  CT_CHECK(dev_visible);
  // Restore defaults.
  ui_guild_toggle_show_all_channels(F.ui, gid);
  ui_channel_toggle_hidden(F.ui, dev_id);

  // Mobile Back closes Server Guide / Channels & Roles.
  enter_mobile();
  CT_CHECK(ui_guild_view_active(F.ui));
  CT_CHECK(ccord_ui_handle_back(F.ui));
  frame(2);
  CT_CHECK(!ui_guild_view_active(F.ui));

  // Split-View Threads (Desktop side-by-side + Full View / Split toggle + Mobile Back to parent).
  enter_desktop();
  ccord_channel th = {
    .id = 993201u, .guild_id = gid, .parent_id = gen_id, .kind = CCORD_CHANNEL_TEXT,
    .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD, .name = "Split Audit Thread", .topic = "",
    .thread_joined = true,
  };
  ccord_error err;
  CT_CHECK(ccord_store_upsert_channel(F.store, &th, &err) == CCORD_OK);
  F.ui->settings_threads_split_view = true;
  (void)ccord_session_select_channel(F.session, th.id, &err);
  frame(4);
  CT_CHECK(ui_thread_split_active(F.ui));
  CT_CHECK(F.ui->layout.stage_split && F.ui->layout.stage_pane.w >= 280.0f);
  CT_CHECK(F.ui->thread_parent_close_rect.w > 0.0f && F.ui->thread_parent_focus_rect.w > 0.0f);

  // Click Full View -> exits split, shows "Split" button in thread header.
  click_rect(F.ui->thread_parent_close_rect);
  frame(3);
  CT_CHECK(!ui_thread_split_active(F.ui) && !F.ui->settings_threads_split_view);
  CT_CHECK(F.ui->thread_split_toggle_rect.w > 0.0f);

  // Click "Split" -> restores split view.
  click_rect(F.ui->thread_split_toggle_rect);
  frame(3);
  CT_CHECK(ui_thread_split_active(F.ui) && F.ui->settings_threads_split_view);

  // Mobile thread view: full screen (!split), Back returns straight to parent channel #general!
  enter_mobile();
  CT_CHECK(!ui_thread_split_active(F.ui));
  F.ui->left_drawer_open = false;
  frame(2);
  CT_CHECK(ccord_ui_handle_back(F.ui));
  frame(3);
  CT_CHECK(ccord_store_selected_channel(F.store) == gen_id);
}

static void test_theme_timefmt_accessibility_and_role_icons(void)
{
  enter_desktop();
  // 1. Settings -> Appearance "System" theme chip.
  ui_open_settings(F.ui, true);
  F.ui->settings_page = 2;   // SP_APPEARANCE
  frame(3);
  ccord_settings_m6_prefs_debug pd;
  ui_settings_m6_prefs_get_debug(F.ui, &pd);
  CT_CHECK(pd.theme_chip_system.w > 0.0f);
  click_rect(pd.theme_chip_system);
  frame(2);
  CT_CHECK(strcmp(F.ui->settings_theme_id, "c-cord-system") == 0);
  click_rect(pd.theme_chips[0]);   // restore c-cord-dark
  frame(2);
  CT_CHECK(strcmp(F.ui->settings_theme_id, "c-cord-dark") == 0);

  // 2. Settings -> Language "24-hour" time format chip.
  F.ui->settings_page = 14;   // SP_LANGUAGE
  frame(3);
  ui_settings_m6_prefs_get_debug(F.ui, &pd);
  CT_CHECK(pd.time_format_chips[2].w > 0.0f);
  click_rect(pd.time_format_chips[2]);
  frame(2);
  CT_CHECK(F.ui->settings_time_format == 2 && ccord_timefmt_get_mode() == CCORD_TIMEFMT_24H);
  click_rect(pd.time_format_chips[0]);   // restore Auto
  frame(2);
  CT_CHECK(ccord_timefmt_get_mode() == CCORD_TIMEFMT_AUTO);

  // 3. Settings -> Accessibility Role Colors & Always Underline Links.
  F.ui->settings_page = 12;   // SP_ACCESSIBILITY
  frame(3);
  ui_settings_m6_prefs_get_debug(F.ui, &pd);
  CT_CHECK(pd.role_color_chips[1].w > 0.0f && pd.underline_links_toggle.w > 0.0f);
  click_rect(pd.role_color_chips[1]);
  frame(2);
  CT_CHECK(F.ui->settings_role_color_mode == 1);
  click_rect(pd.role_color_chips[2]);
  frame(2);
  CT_CHECK(F.ui->settings_role_color_mode == 2);
  CT_CHECK(F.ui->settings_underline_links);
  click_rect(pd.underline_links_toggle);
  frame(2);
  CT_CHECK(!F.ui->settings_underline_links);
  // Restore defaults.
  click_rect(pd.role_color_chips[0]);
  click_rect(pd.underline_links_toggle);
  ui_open_settings(F.ui, false);
  frame(2);

  // 4. Spoiler display policy (Always reveals unclicked spoilers).
  F.ui->settings_spoiler_mode = 0;
  CT_CHECK(!dc_is_spoiler_revealed(F.ui, 777777u));
  F.ui->settings_spoiler_mode = 2;
  CT_CHECK(dc_is_spoiler_revealed(F.ui, 777777u));
  F.ui->settings_spoiler_mode = 0;

  // 5. Role Icon (unicode_emoji) flows through store -> ui_member_role_icon and renders in chat & member list.
  ccord_id gid = ccord_store_selected_guild(F.store);
  ccord_id self_id = ccord_store_self_id(F.store);
  ccord_role star = {
    .id = 994001u, .guild_id = gid, .name = "VIP", .color = 0xF1C40F, .position = 99,
    .permissions = 0, .hoist = false, .unicode_emoji = "\xE2\xAD\x90",
  };
  ccord_error err;
  CT_CHECK(ccord_store_upsert_role(F.store, &star, &err) == CCORD_OK);
  CT_CHECK(ccord_store_apply_member_role(F.store, gid, self_id, star.id, true, &err) == CCORD_OK);
  const char *icon = ui_member_role_icon(F.ui, gid, self_id);
  CT_CHECK(icon != nullptr && strcmp(icon, "\xE2\xAD\x90") == 0);
  // Render a frame with Role Icon + Next-to-name dot enabled to verify zero crashes/layout issues.
  F.ui->settings_role_color_mode = 1;
  frame(3);
  F.ui->settings_role_color_mode = 0;
  frame(2);
}

int main(int argc, char **argv)
{
  (void)argc;
  if (!setup(argv[0])) return 1;
  test_search_filters_and_quick_switcher();
  test_builtin_commands_reminders_and_inbox();
  test_channel_pins_hide_muted_and_audio_popover();
  test_server_guide_channels_roles_and_split_threads();
  test_theme_timefmt_accessibility_and_role_icons();
  ui_test_teardown();
  return CT_RESULT("test_ui_audit_features") || (ui_test_harness_failures() != 0);
}
