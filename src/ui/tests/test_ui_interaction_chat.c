// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include "ui_interaction_harness.h"

static void test_expression_picker_and_stickers(void)
{
  ccord_error err = {0};
  ccord_id g1 = ccord_store_guild_at(F.store, 0)->id;
  ccord_id ch = find_channel(g1, "off-topic", CCORD_CHANNEL_TEXT);
  CT_CHECK(ch != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, ch, &err) == CCORD_OK);
  frame(3);

  // 1. Verify custom guild emojis and stickers seeded from mock_transport READY snapshot
  CT_CHECK(ccord_store_guild_emoji_count(F.store, g1) >= 4);
  CT_CHECK(ccord_store_guild_sticker_count(F.store, g1) >= 3);
  const ccord_emoji *ce = ccord_store_find_emoji_by_name(F.store, "ccord");
  CT_CHECK(ce != nullptr && ce->id == 5001u);
  const ccord_sticker *st_wave = ccord_store_find_sticker(F.store, 7001u);
  CT_CHECK(st_wave != nullptr && strcmp(st_wave->name, "WumpusWave") == 0);

  // 2. Send messages containing inline custom emojis and a 160x160 inline sticker
  const char *em_msg =
    "Testing custom emojis <:ccord:5001> <a:partyblob:5004> <:shipit:5002> and Unicode \xF0\x9F\x94\xA5 \xF0\x9F\x8E\x89!";
  CT_CHECK(ccord_session_send_message(F.session, ch, em_msg, strlen(em_msg), &err) == CCORD_OK);
  CT_CHECK(ccord_session_send_sticker(F.session, ch, 7001u, &err) == CCORD_OK);
  frame(4);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(4);
  ui_pump_media(F.ui, F.renderer);
  frame(3);
  save_optional_screenshot("chat_expressions.png");

  // 3. Open Expression Picker on the Emoji tab via composer right-side [Emoji] button
  ccord_rect cbox = ui_composer_box(F.ui);
  click(cbox.x + cbox.w - 52.0f, cbox.y + cbox.h * 0.5f);
  frame(5);
  ui_pump_media(F.ui, F.renderer);
  frame(2);
  CT_CHECK(F.ui->emoji_picker_open);
  CT_CHECK(F.ui->emoji_picker_rect.w >= 400.0f && F.ui->emoji_picker_rect.h >= 400.0f);
  CT_CHECK(ccord_store_gif_category_count(F.store) >= 4);
  CT_CHECK(ccord_store_gif_trending_count(F.store) >= 4);
  CT_CHECK(ccord_store_sticker_pack_count(F.store) >= 2);
  CT_CHECK(ccord_store_favorite_gif_count(F.store) >= 2);
  save_optional_screenshot("picker_emoji.png");

  // Click first emoji cell in composer mode -> inserts emoji into composer and closes picker
  ccord_rect ecell0 = F.ui->emoji_picker_cells[0];
  CT_CHECK(ecell0.w > 0.0f && ecell0.h > 0.0f);
  click(ecell0.x + ecell0.w * 0.5f, ecell0.y + ecell0.h * 0.5f);
  frame(2);
  CT_CHECK(!F.ui->emoji_picker_open);
  CT_CHECK(strlen(ccord_ui_composer_text(F.ui)) > 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);

  // 4. Open Expression Picker on the GIFs tab via composer right-side [GIF] button
  click(cbox.x + cbox.w - 122.0f, cbox.y + cbox.h * 0.5f);
  frame(4);
  ui_pump_media(F.ui, F.renderer);
  frame(2);
  CT_CHECK(F.ui->emoji_picker_open);
  save_optional_screenshot("picker_gifs.png");

  // Click a Trending GIF card in the lower half of the GIFs tab -> sends GIF URL as a message.
  // The home view lists every GIF category (8 in the mock), so the trending grid starts lower down.
  size_t before_gif = ccord_store_message_count(F.store, ch);
  ccord_rect pbox = F.ui->emoji_picker_rect;
  // The tile is found from the picker's drawn hit records (tile heights follow each GIF's own shape, as in Discord).
  ui_expr_inspect gif_ins;
  CT_CHECK(ui_expr_get_inspect(F.ui, &gif_ins));
  ccord_rect gif_tile = {0, 0, 0, 0};   // the visible part of the first GIF tile
  for (size_t gi = 0; gi < gif_ins.nhits && gif_tile.w == 0.0f; gi++) {
    ui_expr_hit_info ghi;
    if (!ui_expr_hit_at(F.ui, gi, &ghi) || ghi.kind != 2) continue;
    float x0 = ghi.rect.x > gif_ins.grid_view.x ? ghi.rect.x : gif_ins.grid_view.x;
    float y0 = ghi.rect.y > gif_ins.grid_view.y ? ghi.rect.y : gif_ins.grid_view.y;
    float x1 = ghi.rect.x + ghi.rect.w < gif_ins.grid_view.x + gif_ins.grid_view.w ? ghi.rect.x + ghi.rect.w
                                                                                    : gif_ins.grid_view.x + gif_ins.grid_view.w;
    float y1 = ghi.rect.y + ghi.rect.h < gif_ins.grid_view.y + gif_ins.grid_view.h ? ghi.rect.y + ghi.rect.h
                                                                                    : gif_ins.grid_view.y + gif_ins.grid_view.h;
    if (x1 - x0 > 20.0f && y1 - y0 > 20.0f) gif_tile = (ccord_rect){x0, y0, x1 - x0, y1 - y0};
  }
  CT_CHECK(gif_tile.w > 0.0f);
  click(gif_tile.x + gif_tile.w * 0.5f, gif_tile.y + gif_tile.h * 0.5f);
  frame(4);
  CT_CHECK(!F.ui->emoji_picker_open);
  CT_CHECK(ccord_store_message_count(F.store, ch) == before_gif + 1);
  const ccord_message *sent_gif = ccord_store_message_at(F.store, ch, ccord_store_message_count(F.store, ch) - 1);
  CT_CHECK(sent_gif && strstr(sent_gif->content, "https://tenor.com/view/") != nullptr);

  // 5. Open Expression Picker on the Stickers tab via composer right-side [Sticker] button
  click(cbox.x + cbox.w - 84.0f, cbox.y + cbox.h * 0.5f);
  frame(4);
  ui_pump_media(F.ui, F.renderer);
  frame(2);
  CT_CHECK(F.ui->emoji_picker_open);
  save_optional_screenshot("picker_stickers.png");

  // Click a sticker cell in the Stickers tab -> sends sticker message via ccord_session_send_sticker
  size_t before_stk = ccord_store_message_count(F.store, ch);
  click(pbox.x + 95.0f, pbox.y + 165.0f);
  frame(4);
  CT_CHECK(!F.ui->emoji_picker_open);
  CT_CHECK(ccord_store_message_count(F.store, ch) == before_stk + 1);
  const ccord_message *sent_stk = ccord_store_message_at(F.store, ch, ccord_store_message_count(F.store, ch) - 1);
  CT_CHECK(sent_stk && sent_stk->content[0] == '\0' && sent_stk->rich && sent_stk->rich->sticker_count == 1);

  // 6. Verify send-time :shortcode: expansion for both Unicode and custom guild emojis
  size_t before_sc = ccord_store_message_count(F.store, ch);
  type_text("Test :thumbsup: :ccord:!");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  CT_CHECK(ccord_store_message_count(F.store, ch) == before_sc + 1);
  const ccord_message *sent_sc = ccord_store_message_at(F.store, ch, ccord_store_message_count(F.store, ch) - 1);
  CT_CHECK(sent_sc != nullptr);
  if (sent_sc) {
    CT_CHECK(strstr(sent_sc->content, "\xF0\x9F\x91\x8D") != nullptr);
    CT_CHECK(strstr(sent_sc->content, "<:ccord:5001>") != nullptr);
  }
}

static void test_forum_channel_ui_flow(void)
{
  ccord_error err = {0};
  ccord_id g1 = ccord_store_guild_at(F.store, 0)->id;
  ccord_id forum_ch = find_channel(g1, "design-forum", CCORD_CHANNEL_TEXT);
  CT_CHECK(forum_ch != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, forum_ch, &err) == CCORD_OK);
  frame(3);

  // 1. Verify Forum UI view is active and displays all 5 seeded post cards (pinned post first)
  CT_CHECK(ui_forum_is_active(F.ui));
  CT_CHECK(ui_forum_visible_post_count(F.ui) == 5);
  ccord_id first_tid = CCORD_ID_NONE;
  ccord_rect card0 = ui_forum_post_card_rect(F.ui, 0, &first_tid);
  CT_CHECK(card0.w > 100.0f && card0.h > 40.0f);
  CT_CHECK(first_tid == 3151); // Pinned "Welcome & Forum Guidelines" post

  // 2. Click a filter tag pill ("Bug", index 1) -> filters visible posts down to 1
  ccord_rect bug_tag = ui_forum_filter_tag_rect(F.ui, 1);
  CT_CHECK(bug_tag.w > 20.0f && bug_tag.h > 14.0f);
  click(bug_tag.x + bug_tag.w * 0.5f, bug_tag.y + bug_tag.h * 0.5f);
  frame(2);
  CT_CHECK(ui_forum_visible_post_count(F.ui) == 1);
  // Click the same tag pill again to clear the tag filter
  click(bug_tag.x + bug_tag.w * 0.5f, bug_tag.y + bug_tag.h * 0.5f);
  frame(2);
  CT_CHECK(ui_forum_visible_post_count(F.ui) == 5);

  // 3. Click "+ New Post", enter title & body, and submit -> creates post and opens its thread channel
  ccord_rect new_btn = ui_forum_new_post_btn_rect(F.ui);
  CT_CHECK(new_btn.w > 40.0f && new_btn.h > 20.0f);
  click(new_btn.x + new_btn.w * 0.5f, new_btn.y + new_btn.h * 0.5f);
  frame(2);
  type_text("UI Test Forum Post");
  key(CCORD_KEY_TAB, 0);
  frame(1);
  type_text("Body of the newly created forum post.");
  frame(2);
  ccord_rect submit_btn = ui_forum_composer_submit_rect(F.ui);
  CT_CHECK(submit_btn.w > 30.0f && submit_btn.h > 16.0f);
  click(submit_btn.x + submit_btn.w * 0.5f, submit_btn.y + submit_btn.h * 0.5f);
  frame(4);

  // Submitting the new forum post opens and selects the newly created thread channel!
  CT_CHECK(!ui_forum_is_active(F.ui));
  ccord_id opened_tid = ccord_store_selected_channel(F.store);
  CT_CHECK(opened_tid != forum_ch && opened_tid != CCORD_ID_NONE);
  const ccord_channel *opened_ch = ccord_store_channel(F.store, opened_tid);
  CT_CHECK(opened_ch != nullptr && opened_ch->parent_id == forum_ch);

  // 4. Click the forum breadcrumb in the chat header ("<forum icon> design-forum") to return to the forum post list
  ccord_rect bc = ui_forum_breadcrumb_rect(F.ui);
  CT_CHECK(bc.w > 30.0f && bc.h > 12.0f);
  click(bc.x + bc.w * 0.5f, bc.y + bc.h * 0.5f);
  frame(3);
  CT_CHECK(ccord_store_selected_channel(F.store) == forum_ch);
  CT_CHECK(ui_forum_is_active(F.ui));
  CT_CHECK(ui_forum_visible_post_count(F.ui) >= 5);
}

// An open picker over a forum gets the click on its tile; with no overlay the same post card still opens.
static void test_picker_over_forum_clicks(void)
{
  ccord_error err = {0};
  ccord_id g1 = ccord_store_guild_at(F.store, 0)->id;
  ccord_id forum_ch = find_channel(g1, "design-forum", CCORD_CHANNEL_TEXT);
  CT_CHECK(forum_ch != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, forum_ch, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(ui_forum_is_active(F.ui));

  // 1. The emoji picker (tab 2 = EXPR_TAB_EMOJI) opens over the forum post list. A click on its first cell goes
  // to the picker: the emoji is inserted and the picker closes, and no forum post opens.
  ui_expr_picker_open(F.ui, 2, forum_ch, CCORD_ID_NONE);
  frame(3);
  CT_CHECK(F.ui->emoji_picker_open);
  ccord_rect cell = F.ui->emoji_picker_cells[0];
  CT_CHECK(cell.w > 0.0f && cell.h > 0.0f);
  ccord_text_edit_clear(&F.ui->edit);
  click(cell.x + cell.w * 0.5f, cell.y + cell.h * 0.5f);
  frame(2);
  CT_CHECK(!F.ui->emoji_picker_open);
  CT_CHECK(strlen(ccord_ui_composer_text(F.ui)) > 0);
  CT_CHECK(ccord_store_selected_channel(F.store) == forum_ch);
  CT_CHECK(ui_forum_is_active(F.ui));
  ccord_text_edit_clear(&F.ui->edit);

  // 2. No overlay is open: a click on a post card (index 1, not the pinned one) opens that post.
  ccord_id tid = CCORD_ID_NONE;
  ccord_rect card = ui_forum_post_card_rect(F.ui, 1, &tid);
  CT_CHECK(card.w > 100.0f && card.h > 40.0f && tid != CCORD_ID_NONE);
  click(card.x + card.w * 0.5f, card.y + card.h * 0.5f);
  frame(3);
  CT_CHECK(ccord_store_selected_channel(F.store) == tid);
  CT_CHECK(!ui_forum_is_active(F.ui));
}

static void test_vanilla_discord_gap_features(void)
{
  ccord_error err = {0};
  ccord_id g1 = ccord_store_guild_at(F.store, 0)->id;
  ccord_id gen = find_channel(g1, "general", CCORD_CHANNEL_TEXT);
  ccord_id dev = find_channel(g1, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(gen != CCORD_ID_NONE && dev != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, gen, &err) == CCORD_OK);
  frame(3);

  // 1. Rich Composer UX: Ctrl+B bold wrap, Ctrl+Z undo, Ctrl+Y redo
  ccord_text_edit_clear(&F.ui->edit);
  type_text("hello");
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_B, CCORD_MOD_CTRL);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&F.ui->edit), "**hello**") == 0);
  key(CCORD_KEY_Z, CCORD_MOD_CTRL);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&F.ui->edit), "hello") == 0);
  key(CCORD_KEY_Y, CCORD_MOD_CTRL);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&F.ui->edit), "**hello**") == 0);
  ccord_text_edit_clear(&F.ui->edit);

  // 2. Per-channel draft persistence across channel switches
  type_text("draft in general");
  float dev_y = scan_hover_channel(200.0f, dev);
  CT_CHECK(dev_y > 0.0f);
  click(200.0f, dev_y);
  frame(2);
  CT_CHECK(ccord_store_selected_channel(F.store) == dev);
  CT_CHECK(F.ui->edit.len == 0);
  float gen_y = scan_hover_channel(200.0f, gen);
  CT_CHECK(gen_y > 0.0f);
  click(200.0f, gen_y);
  frame(2);
  CT_CHECK(ccord_store_selected_channel(F.store) == gen);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&F.ui->edit), "draft in general") == 0);
  ccord_text_edit_clear(&F.ui->edit);

  // 3. Up arrow on empty composer starts editing last own message
  key(CCORD_KEY_UP, 0);
  frame(1);
  CT_CHECK(F.ui->editing_message != CCORD_ID_NONE);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(F.ui->editing_message == CCORD_ID_NONE);

  // 4. Quick Switcher (Ctrl+K) modal open, filter, and Escape close
  key(CCORD_KEY_K, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(F.ui->quick_switcher_open);
  type_text("#dev");
  frame(2);
  CT_CHECK(F.ui->nquick_switcher_items >= 1);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!F.ui->quick_switcher_open);

  // 5. Chat message text highlighting (drag-select) & Ctrl+C copy
  const ccord_layout *l = &F.ui->layout;
  float mx = l->message_area.x + 120.0f;
  float my = scan_hover_self_message(mx);
  if (my > 0.0f) {
    F.clip[0] = '\0';
    mouse(CCORD_IN_MOUSE_DOWN, 1, l->message_area.x + 60.0f, my - 10.0f);
    mouse(CCORD_IN_MOUSE_MOVE, 0, l->message_area.x + 420.0f, my + 24.0f);
    frame(2);
    mouse(CCORD_IN_MOUSE_UP, 1, l->message_area.x + 420.0f, my + 24.0f);
    frame(1);
    CT_CHECK(F.ui->chat_sel_active && F.ui->chat_sel_len > 0);
    shot("selection");
    key(CCORD_KEY_C, CCORD_MOD_CTRL);
    CT_CHECK(strlen(F.clip) > 0);
  }

  // 6. Permission-gated server management & moderation context menus when user is guild owner
  const ccord_guild *orig_g = ccord_store_guild(F.store, g1);
  CT_CHECK(orig_g != nullptr);
  ccord_guild admin_g = *orig_g;
  admin_g.owner_id = ccord_store_self_id(F.store);
  admin_g.self_permissions = UINT64_MAX;
  CT_CHECK(ccord_store_upsert_guild(F.store, &admin_g, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  frame(2);

  // Right-clicking channel `dev` now includes Mute, Notification Settings, Edit Channel, Clone Channel,
  // Create Channel, Create Thread, Delete Channel + plugin item (8 items)
  dev_y = scan_hover_channel(200.0f, dev);
  CT_CHECK(dev_y > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dev_y);
  frame(1);
  CT_CHECK(dbg().context_menu_open && dbg().context_menu_items == 8);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
}



// What the context menus offer follows the permission engine, not a flag: the mock makes you an administrator of C-Cord
// HQ, a moderator of Lua Lounge (whose owner is Ada) and an ordinary member of Pixel Garden.
static void test_permission_gated_menus(void)
{
  const ccord_id self = 1001, ada = 1002, bjorn = 1003, chen = 1004;
  const ccord_id hq = 2001, lua = 2002, garden = 2003;
  CT_CHECK(ccord_store_self_id(F.store) == self);
  const ccord_id hq_chan = find_channel(hq, "dev", CCORD_CHANNEL_TEXT);
  const ccord_id lua_chan = find_channel(lua, "showcase", CCORD_CHANNEL_TEXT);
  const ccord_id garden_chan = find_channel(garden, "art", CCORD_CHANNEL_TEXT);
  const ccord_id garden_read_only = find_channel(garden, "critique", CCORD_CHANNEL_TEXT);   // overwrites: no threads
  CT_CHECK(hq_chan && lua_chan && garden_chan && garden_read_only);

  // The engine's own answers first, so a menu difference below can be traced to the right layer.
  CT_CHECK(ccord_store_can_act_on(F.store, CCORD_PERM_ACT_KICK, hq, self, ada) == CCORD_PERM_ALLOWED);
  CT_CHECK(ccord_store_can_act_on(F.store, CCORD_PERM_ACT_KICK, lua, self, chen) == CCORD_PERM_ALLOWED);
  CT_CHECK(ccord_store_can_act_on(F.store, CCORD_PERM_ACT_KICK, lua, self, bjorn) == CCORD_PERM_TARGET_HIGHER);
  CT_CHECK(ccord_store_can_act_on(F.store, CCORD_PERM_ACT_KICK, lua, self, ada) == CCORD_PERM_TARGET_OWNER);
  CT_CHECK(ccord_store_can_act_on(F.store, CCORD_PERM_ACT_KICK, garden, self, chen) == CCORD_PERM_MISSING);

  // ---- C-Cord HQ: administrator ----
  open_menu(CCORD_MENU_MEMBER, hq, CCORD_ID_NONE, ada, CCORD_ID_NONE);
  CT_CHECK(F.ui->menu.open && menu_has("Kick Member") && menu_has("Ban Member") && menu_has("Timeout Member") &&
           menu_has("Change Nickname"));
  open_menu(CCORD_MENU_MEMBER, hq, CCORD_ID_NONE, self, CCORD_ID_NONE);   // yourself: a nickname, never a kick
  CT_CHECK(F.ui->menu.open && menu_has("Change Nickname") && !menu_has("Kick Member") && !menu_has("Ban Member") &&
           !menu_has("Timeout Member"));
  open_menu(CCORD_MENU_CHANNEL, hq, hq_chan, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_has("Edit Channel") && menu_has("Create Channel") && menu_has("Create Thread") && menu_has("Delete Channel"));
  open_menu(CCORD_MENU_GUILD, hq, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_has("Server Settings") && menu_has("Create Channel"));
  open_menu(CCORD_MENU_MESSAGE, hq, hq_chan, ada, 5);
  CT_CHECK(menu_has("Delete Message"));

  // ---- Lua Lounge: moderator under an owner ----
  open_menu(CCORD_MENU_MEMBER, lua, CCORD_ID_NONE, chen, CCORD_ID_NONE);
  CT_CHECK(F.ui->menu.open && menu_has("Kick Member") && menu_has("Ban Member") && menu_has("Timeout Member") &&
           menu_has("Change Nickname"));
  open_menu(CCORD_MENU_MEMBER, lua, CCORD_ID_NONE, bjorn, CCORD_ID_NONE);   // a peer: same highest role
  CT_CHECK(!menu_has("Kick Member") && !menu_has("Ban Member") && !menu_has("Timeout Member") && !menu_has("Change Nickname"));
  open_menu(CCORD_MENU_MEMBER, lua, CCORD_ID_NONE, ada, CCORD_ID_NONE);     // the owner: untouchable
  CT_CHECK(!menu_has("Kick Member") && !menu_has("Ban Member") && !menu_has("Timeout Member") && !menu_has("Change Nickname"));
  open_menu(CCORD_MENU_CHANNEL, lua, lua_chan, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_has("Create Thread") && !menu_has("Edit Channel") && !menu_has("Delete Channel") && !menu_has("Create Channel"));
  open_menu(CCORD_MENU_GUILD, lua, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_has("Server Settings") && !menu_has("Create Channel"));   // may ban, so the Bans page is theirs
  open_menu(CCORD_MENU_MESSAGE, lua, lua_chan, ada, 5);   // Manage Messages comes from the Moderators role
  CT_CHECK(menu_has("Delete Message"));

  // ---- Pixel Garden: ordinary member ----
  open_menu(CCORD_MENU_MEMBER, garden, CCORD_ID_NONE, chen, CCORD_ID_NONE);
  CT_CHECK(F.ui->menu.open && menu_has("Profile") && menu_has("Message (Open DM)") && menu_has("Report User") &&
           !menu_has("Kick Member") && !menu_has("Ban Member") && !menu_has("Timeout Member") &&
           !menu_has("Roles") && !menu_has("Open in Mod View"));
  open_menu(CCORD_MENU_MEMBER, garden, CCORD_ID_NONE, self, CCORD_ID_NONE);   // yourself: Change Nickname is everyone's right
  CT_CHECK(F.ui->menu.open && menu_has("Change Nickname") && !menu_has("Kick Member"));
  open_menu(CCORD_MENU_CHANNEL, garden, garden_chan, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(menu_has("Create Thread") && !menu_has("Edit Channel") && !menu_has("Delete Channel") && !menu_has("Create Channel"));
  open_menu(CCORD_MENU_CHANNEL, garden, garden_read_only, CCORD_ID_NONE, CCORD_ID_NONE);   // the overwrite takes threads away
  CT_CHECK(!menu_has("Create Thread") && !menu_has("Edit Channel"));
  open_menu(CCORD_MENU_GUILD, garden, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE);
  CT_CHECK(!menu_has("Server Settings") && !menu_has("Create Channel"));
  open_menu(CCORD_MENU_MESSAGE, garden, garden_chan, ada, 5);
  CT_CHECK(!menu_has("Delete Message") && menu_has("Copy Text"));
  ui_menu_close(F.ui);
  frame(1);
}

// Where the permission engine knows you cannot send, the composer shuts: it says why, takes no text and sends nothing.
// Where it only has a guess (no role data), it never locks you out.
static void test_composer_locks_where_sending_is_not_allowed(void)
{
  ccord_error err;
  ccord_id back_guild = ccord_store_selected_guild(F.store);
  ccord_id back_chan = ccord_store_selected_channel(F.store);
  const ccord_id self = 1001;
  const ccord_id hq = 2001, lua = 2002, garden = 2003;
  ui_cancel_reply(F.ui);   // earlier tests may have left a reply or an edit pending; it would answer Enter instead
  ui_cancel_edit(F.ui);
  ccord_id announcements = find_channel(hq, "announcements", CCORD_CHANNEL_TEXT);
  ccord_id showcase = find_channel(lua, "showcase", CCORD_CHANNEL_TEXT);
  ccord_id critique = find_channel(garden, "critique", CCORD_CHANNEL_TEXT);
  ccord_id art = find_channel(garden, "art", CCORD_CHANNEL_TEXT);
  CT_CHECK(announcements && showcase && critique && art);

  // Read-only for @everyone, but you are an administrator / hold the Moderators role the overwrite lets through.
  CT_CHECK(ccord_store_select_guild(F.store, hq, &err) == CCORD_OK && ccord_store_select_channel(F.store, announcements, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr);
  CT_CHECK(ccord_store_select_guild(F.store, lua, &err) == CCORD_OK && ccord_store_select_channel(F.store, showcase, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr);
  CT_CHECK(ccord_store_select_guild(F.store, garden, &err) == CCORD_OK && ccord_store_select_channel(F.store, art, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr);

  // An ordinary member of Pixel Garden meets the read-only channel.
  CT_CHECK(ccord_store_select_channel(F.store, critique, &err) == CCORD_OK);
  frame(2);
  const char *why = ui_composer_lock_reason(F.ui);
  CT_CHECK(why != nullptr && strstr(why, "permission") != nullptr);
  type_text("hello");
  frame(1);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');   // nothing was typed
  ccord_error ierr;
  CT_CHECK(ccord_text_edit_insert(&F.ui->edit, "stuck draft", 11, &ierr) == CCORD_OK);   // e.g. a draft restored earlier
  size_t before = ccord_store_message_count(F.store, critique);
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(ccord_store_message_count(F.store, critique) == before);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "stuck draft") == 0);   // not sent, not cleared
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);

  // A timeout shuts the box everywhere, with its own explanation; lifting it opens it again.
  CT_CHECK(ccord_store_select_channel(F.store, art, &err) == CCORD_OK);
  frame(1);
  ccord_member timed_out = {.guild_id = garden, .user_id = self, .role_name = "Members", .role_rank = 2,
                            .timeout_until_ms = F.ui->now_unix_ms + 3600000};
  CT_CHECK(ccord_store_upsert_guild_members(F.store, garden, &timed_out, 1, nullptr, 0, &err) == CCORD_OK);
  frame(1);
  why = ui_composer_lock_reason(F.ui);
  CT_CHECK(why != nullptr && strstr(why, "timed out") != nullptr);
  timed_out.timeout_until_ms = 0;
  CT_CHECK(ccord_store_upsert_guild_members(F.store, garden, &timed_out, 1, nullptr, 0, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr);

  // A thread is judged by "Send Messages in Threads", not "Send Messages": the parent may be read-only for talking while
  // its threads stay open.
  ccord_overwrite threads_only[1] = {{.id = garden, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_SEND_MESSAGES}};
  CT_CHECK(ccord_store_set_channel_overwrites(F.store, art, garden, threads_only, 1, &err) == CCORD_OK);
  ccord_channel thread = {.id = 9001, .guild_id = garden, .parent_id = art, .kind = CCORD_CHANNEL_TEXT,
                          .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD, .name = "side talk", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(F.store, &thread, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, art, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(ui_composer_lock_reason(F.ui) != nullptr);   // the channel itself: shut
  CT_CHECK(ccord_store_select_channel(F.store, 9001, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr);   // its thread: open
  CT_CHECK(ccord_store_set_channel_overwrites(F.store, art, garden, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, 9001, &err) == CCORD_OK);

  // A server whose roles we never received: no lock, because the answer would be a guess.
  ccord_guild unknown = {.id = 777, .name = "Unknown Roles"};
  ccord_channel unknown_chan = {.id = 778, .guild_id = 777, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = ""};
  CT_CHECK(ccord_store_upsert_guild(F.store, &unknown, &unknown_chan, 1, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_guild(F.store, 777, &err) == CCORD_OK && ccord_store_select_channel(F.store, 778, &err) == CCORD_OK);
  frame(1);
  CT_CHECK(!ccord_store_permissions_exact(F.store, 777, self));
  CT_CHECK(ccord_store_permissions(F.store, 777, 778, self, 0) == 0);   // the guess is "nothing" ...
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr);                   // ... and it must not lock the box

  CT_CHECK(ccord_store_select_guild(F.store, back_guild, &err) == CCORD_OK);
  if (back_chan != CCORD_ID_NONE) CT_CHECK(ccord_store_select_channel(F.store, back_chan, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_guild(F.store, 777, &err) == CCORD_OK);
  frame(2);
}

static void test_smooth_scroll_hover_anim_loading_and_restore(void)
{
  ccord_error err = {0};

  // 1. Smooth scrolling (motion smoothing on wheel scrolls)
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  F.ui->msg_scroll_vel = 0.0f;
  frame(1);
  CT_CHECK(dbg().stick_bottom);

  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 6.0f, .x = l->message_area.x + 120.0f, .y = l->message_area.y + 120.0f});
  CT_CHECK(F.ui->msg_scroll_vel < 0.0f);
  float v0 = F.ui->msg_scroll_vel;
  frame(1);
  CT_CHECK(F.ui->msg_scroll_vel < 0.0f && F.ui->msg_scroll_vel > v0);   // velocity decays smoothly across frames
  frame(18);
  CT_CHECK(fabsf(F.ui->msg_scroll_vel) < 1.0f);
  F.ui->msg_scroll_vel = 0.0f;
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(1);

  // 2. Animated server icons (`a_` hash -> `.gif`) animate ONLY when hovered
  static const uint8_t k_rgb_gif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x08,
    0x05, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x15,
    0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1,
    0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08,
    0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08,
    0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00,
    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c,
    0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03,
    0x02, 0x00, 0x3b,
  };
  const ccord_guild *g1 = ccord_store_guild_at(F.store, 0);
  CT_CHECK(g1 != nullptr);
  ccord_guild anim_g = *g1;
  anim_g.icon_url = "a_serveranim99";
  CT_CHECK(ccord_store_upsert_guild(F.store, &anim_g, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);

  char icon_url[256] = {0};
  const ccord_guild *g1_after = ccord_store_guild(F.store, anim_g.id);
  CT_CHECK(ui_guild_icon_url(g1_after, icon_url, sizeof icon_url));
  CT_CHECK(strstr(icon_url, "/icons/") != nullptr && strstr(icon_url, "a_serveranim99.gif?size=64") != nullptr);

  CT_CHECK(ccord_render_upload_image(F.renderer, icon_url, k_rgb_gif, sizeof k_rgb_gif, nullptr, nullptr, &err) == CCORD_OK);
  size_t cur_frame = 99, frame_count = 0;
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, icon_url, &cur_frame, &frame_count));
  CT_CHECK(frame_count == 3);

  // Move mouse away from server list -> icon stays on frame 0 even across multiple frames
  mouse(CCORD_IN_MOUSE_MOVE, 0, 500.0f, 400.0f);
  frame(6);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, icon_url, &cur_frame, &frame_count));
  CT_CHECK(cur_frame == 0);

  // Hover over guild 0 (slot 1 in server list) -> icon animates past frame 0 as time advances
  ccord_rect g_ic = ui_guild_icon_rect(F.ui, 1);
  mouse(CCORD_IN_MOUSE_MOVE, 0, g_ic.x + g_ic.w * 0.5f, g_ic.y + g_ic.h * 0.5f);
  frame(6);   // 6 * 16ms = 96ms > 50ms delay of frame 0
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, icon_url, &cur_frame, &frame_count));
  CT_CHECK(cur_frame >= 1);

  // Unhover -> resets back to static frame 0
  mouse(CCORD_IN_MOUSE_MOVE, 0, 500.0f, 400.0f);
  frame(2);
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, icon_url, &cur_frame, &frame_count));
  CT_CHECK(cur_frame == 0);

  // 3. Centered "C-Cord" loading screen with progress bar
  ccord_ui_set_loading_screen(F.ui, true);
  frame(3);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.loading_screen_open);
  CT_CHECK(d.loading_bar_rect.w > 100.0f && d.loading_bar_rect.h > 0.0f);
  float bar_center_x = d.loading_bar_rect.x + d.loading_bar_rect.w * 0.5f;
  CT_CHECK(fabsf(bar_center_x - F.ui->win_w * 0.5f) < 2.0f);
  CT_CHECK(d.loading_progress > 0.15f && d.loading_progress <= 1.0f);
  ccord_ui_set_loading_screen(F.ui, false);
  frame(1);
  CT_CHECK(!dbg().loading_screen_open);

  // 4. Persist & restore previous guild and channel across restarts
  const ccord_guild *g2 = ccord_store_guild_at(F.store, 1);
  CT_CHECK(g2 != nullptr);
  ccord_rect g2_ic = ui_guild_icon_rect(F.ui, 2);
  click(g2_ic.x + g2_ic.w * 0.5f, g2_ic.y + g2_ic.h * 0.5f);
  frame(2);
  CT_CHECK(ccord_store_selected_guild(F.store) == g2->id);

  // Find a second text channel in g2 to select so it's not just the default first channel
  ccord_id target_ch = ccord_store_selected_channel(F.store);
  size_t nch2 = ccord_store_channel_count(F.store, g2->id);
  for (size_t i = 0; i < nch2; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, g2->id, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->id != target_ch) {
      float cy = scan_hover_channel(200.0f, c->id);
      if (cy > 0.0f) {
        click(200.0f, cy);
        frame(2);
        target_ch = c->id;
        break;
      }
    }
  }
  CT_CHECK(ccord_store_selected_channel(F.store) == target_ch);
  CT_CHECK(F.ui->saved_guild_id == g2->id && F.ui->saved_channel_id == target_ch);

  // Simulate a fresh restart by selecting guild 0 first, then creating a new UI instance reading `ui_test_data/settings.ini`
  (void)ccord_session_select_guild(F.session, anim_g.id, &err);
  CT_CHECK(ccord_store_selected_guild(F.store) == anim_g.id);

  ccord_ui_deps deps2 = F.ui->d;
  ccord_ui *ui2 = ccord_ui_create(&deps2, &err);
  CT_CHECK(ui2 != nullptr);
  CT_CHECK(ccord_store_selected_guild(F.store) == g2->id);
  CT_CHECK(ccord_store_selected_channel(F.store) == target_ch);
  ccord_ui_destroy(ui2);
}

// Discord refuses writes: the call that started the action already returned OK, so the failure arrives later.
// The user must be told, and the text they wrote must come back instead of vanishing.
static void test_refused_writes_return_the_text(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 3000, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;  // the session now owns tr
  frame(8);
  ccord_id ch = ccord_store_selected_channel(F.store);
  CT_CHECK(ch != CCORD_ID_NONE && ccord_store_message_count(F.store, ch) > 0);
  // One accepted message of ours (the Edit menu is only offered on own messages), then Discord starts refusing.
  type_text("a message to edit later");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  size_t before = ccord_store_message_count(F.store, ch);
  ccord_mock_set_refuse_writes(tr, true);

  // 1. M0-3: a refused send fails its pending echo in place. The composer stays empty (the failed bubble keeps
  // the text for Retry), no draft-restore notice fires, and nothing was posted to Discord.
  type_text("keep me");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  frame(2);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  CT_CHECK(ccord_store_message_count(F.store, ch) == before + 1);   // the failed echo, not a posted message
  const ccord_message *failed = ccord_store_message_at(F.store, ch, before);
  CT_CHECK(failed && failed->send_state == CCORD_MSG_SEND_FAILED && strcmp(failed->content, "keep me") == 0 &&
           failed->nonce[0] != '\0');

  // 2. The user already started the next message: the refusal does not touch the composer, so only the new
  // draft survives there; the refused text lives on in its failed bubble.
  type_text("first");
  key(CCORD_KEY_RETURN, 0);
  type_text("second");
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "second") == 0);
  CT_CHECK(ccord_store_message_count(F.store, ch) == before + 2);
  const ccord_message *failed2 = ccord_store_message_at(F.store, ch, before + 1);
  CT_CHECK(failed2 && failed2->send_state == CCORD_MSG_SEND_FAILED && strcmp(failed2->content, "first") == 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);

  // 3. A refused edit puts the user back in edit mode on the same message with what they typed.
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float x = l->message_area.x + 300.0f;
  float y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  ccord_id msg = dbg().hover_message;
  const ccord_message *m0 = ccord_store_find_message(F.store, ch, msg);
  CT_CHECK(m0 != nullptr);
  char original[CCORD_MESSAGE_MAX_BYTES + 1];
  snprintf(original, sizeof original, "%s", m0 ? m0->content : "");
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  float ey = menu_item_center_y(y, dbg().context_menu_items, 2, 0);
  mouse(CCORD_IN_MOUSE_MOVE, 0, x + 20.0f, ey);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x + 20.0f, ey);
  frame(1);
  CT_CHECK(dbg().composer_editing);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("my edit");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(!dbg().composer_editing && ccord_ui_composer_text(F.ui)[0] == '\0');
  frame(2);
  CT_CHECK(dbg().composer_editing);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "my edit") == 0);
  CT_CHECK(strstr(dbg().notice, "Couldn't edit") != nullptr);
  const ccord_message *m1 = ccord_store_find_message(F.store, ch, msg);
  CT_CHECK(m1 && strcmp(m1->content, original) == 0 && !m1->edited);  // the message is unchanged

  // 4. Same, but the composer is busy by the time the refusal arrives: it is not overwritten, and the user is told.
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(!dbg().composer_editing);
  type_text("something else");
  frame(2);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "something else") == 0);
  CT_CHECK(strstr(dbg().notice, "could not be restored") != nullptr);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
  // The session owns `tr` now (the fixture's old transport is gone): later tests must neither inherit its refusals
  // nor poke the stale pointer.
  ccord_mock_set_refuse_writes(tr, false);
  F.transport = tr;
}

// M0-3: pending rows are dimmed with a clock/status line, failed rows are red and expose a Retry button,
// and Retry reuses the failed echo's nonce so the server's nonce-matched MESSAGE_CREATE confirms it in place.
static const ccord_message *find_by_nonce(ccord_id ch, const char *nonce)
{
  size_t n = ccord_store_message_count(F.store, ch);
  for (size_t i = 0; i < n; i++) {
    const ccord_message *m = ccord_store_message_at(F.store, ch, i);
    if (m && m->nonce[0] != '\0' && strcmp(m->nonce, nonce) == 0) return m;
  }
  return nullptr;
}

static bool has_reaction_hit_for(ccord_id message_id)
{
  for (size_t i = 0; i < F.ui->nreaction_hits; i++) {
    if (F.ui->reaction_hits[i].message_id == message_id) return true;
  }
  return false;
}

static void test_m0_pending_failed_retry_ui(void)
{
  draw_only();
  ccord_id ch = ccord_store_selected_channel(F.store);
  ccord_mock_set_refuse_writes(F.transport, false);
  // Earlier refusal tests leave their failed echoes in this channel; start from confirmed history only so the
  // drawn-status counts below belong to this test.
  for (size_t i = ccord_store_message_count(F.store, ch); i > 0; i--) {
    const ccord_message *m = ccord_store_message_at(F.store, ch, i - 1);
    if (m && m->send_state != CCORD_MSG_SEND_CONFIRMED) {
      ccord_error derr;
      (void)ccord_store_delete_message(F.store, ch, m->id, &derr);
    }
  }
  ccord_vlist_reset_bottom(&F.ui->vlist);
  draw_only();

  // Pending: draw without pumping, so the mock's MESSAGE_CREATE cannot confirm the echo yet.
  size_t before = ccord_store_message_count(F.store, ch);
  type_text("pending bubble");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  size_t after_send = ccord_store_message_count(F.store, ch);
  CT_CHECK(after_send == before + 1);
  const ccord_message *p = find_by_nonce(ch, ccord_store_message_at(F.store, ch, after_send - 1)->nonce);
  CT_CHECK(p && p->send_state == CCORD_MSG_SEND_PENDING && p->nonce[0] != '\0' &&
           strcmp(p->content, "pending bubble") == 0);
  char nonce[32];
  snprintf(nonce, sizeof nonce, "%s", p ? p->nonce : "");
  ccord_vlist_reset_bottom(&F.ui->vlist);
  draw_only();
  ccord_ui_debug d = dbg();
  CT_CHECK(d.pending_echoes_drawn == 1 && d.failed_echoes_drawn == 0 && d.retry_echo_count == 0);

  frame(3);
  p = find_by_nonce(ch, nonce);
  CT_CHECK(p && p->send_state == CCORD_MSG_SEND_CONFIRMED && strcmp(p->content, "pending bubble") == 0);
  CT_CHECK(ccord_store_message_count(F.store, ch) == after_send);   // replaced in place
  draw_only();
  d = dbg();
  CT_CHECK(d.pending_echoes_drawn == 0 && d.failed_echoes_drawn == 0 && d.retry_echo_count == 0);

  // Failed: Discord refuses, and the refusal marks exactly this echo.
  ccord_mock_set_refuse_writes(F.transport, true);
  before = ccord_store_message_count(F.store, ch);
  type_text("retry bubble");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  after_send = ccord_store_message_count(F.store, ch);
  p = ccord_store_message_at(F.store, ch, after_send - 1);
  CT_CHECK(after_send == before + 1 && p && p->send_state == CCORD_MSG_SEND_FAILED &&
           strcmp(p->content, "retry bubble") == 0);
  char failed_nonce[32];
  snprintf(failed_nonce, sizeof failed_nonce, "%s", p ? p->nonce : "");
  ccord_id failed_id = p ? p->id : CCORD_ID_NONE;
  ccord_vlist_reset_bottom(&F.ui->vlist);
  draw_only();
  d = dbg();
  CT_CHECK(d.failed_echoes_drawn == 1 && d.pending_echoes_drawn == 0);
  CT_CHECK(d.retry_echo_count == 1 && d.retry_echo_ids[0] == failed_id && d.retry_echo_rects[0].w > 0.0f);
  ccord_rect rb = d.retry_echo_rects[0];

  // An unconfirmed echo offers no server actions: hovering it draws no reply/pin/reaction bar, and a
  // right-click does not open the message menu.
  CT_CHECK(!dbg().context_menu_open);
  mouse(CCORD_IN_MOUSE_MOVE, 0, rb.x + rb.w * 0.5f, rb.y + rb.h * 0.5f);
  frame(2);   // the hover bar (if it were drawn) fades in over two frames
  CT_CHECK(dbg().hover_message == failed_id);
  CT_CHECK(F.ui->hover_bar_msg_id == CCORD_ID_NONE && !has_reaction_hit_for(failed_id));
  mouse(CCORD_IN_MOUSE_DOWN, 3, rb.x + 10.0f, rb.y + rb.h * 0.5f);
  CT_CHECK(!dbg().context_menu_open);
  mouse(CCORD_IN_MOUSE_UP, 3, rb.x + 10.0f, rb.y + rb.h * 0.5f);

  // Retry: the pill re-pends the bubble with its ORIGINAL nonce, then the server confirms it in place.
  ccord_mock_set_refuse_writes(F.transport, false);
  rb = dbg().retry_echo_rects[0];
  mouse(CCORD_IN_MOUSE_MOVE, 0, rb.x + rb.w * 0.5f, rb.y + rb.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, rb.x + rb.w * 0.5f, rb.y + rb.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 1, rb.x + rb.w * 0.5f, rb.y + rb.h * 0.5f);
  p = find_by_nonce(ch, failed_nonce);
  CT_CHECK(p && p->send_state == CCORD_MSG_SEND_PENDING && p->id == failed_id);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  draw_only();
  d = dbg();
  CT_CHECK(d.pending_echoes_drawn == 1 && d.failed_echoes_drawn == 0 && d.retry_echo_count == 0);
  frame(3);
  p = find_by_nonce(ch, failed_nonce);
  CT_CHECK(p && p->send_state == CCORD_MSG_SEND_CONFIRMED && strcmp(p->content, "retry bubble") == 0);
  CT_CHECK(ccord_store_message_count(F.store, ch) == after_send);
  draw_only();
  d = dbg();
  CT_CHECK(d.pending_echoes_drawn == 0 && d.failed_echoes_drawn == 0 && d.retry_echo_count == 0);
  ccord_mock_set_refuse_writes(F.transport, false);
}

static const ccord_member *member_of(ccord_id guild, ccord_id user)
{
  for (size_t i = 0; i < ccord_store_member_count(F.store, guild); i++) {
    const ccord_member *m = ccord_store_member_at(F.store, guild, i);
    if (m && m->user_id == user) return m;
  }
  return nullptr;
}


// Server administration from the UI. Asking is not doing: a dialog changes nothing until the user confirms; the
// confirmed call goes to Discord exactly as chosen; and nothing in the store moves until Discord has answered.
static void test_admin_dialogs_call_discord(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  ccord_guild admin_g = *ccord_store_guild(F.store, g);
  admin_g.owner_id = ccord_store_self_id(F.store);
  admin_g.self_permissions = UINT64_MAX;
  CT_CHECK(ccord_store_upsert_guild(F.store, &admin_g, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  frame(2);
  ccord_id v1 = other_member(g, 0), v2 = other_member(g, 1), v3 = other_member(g, 2), v4 = other_member(g, 3);
  CT_CHECK(v1 && v2 && v3 && v4);
  size_t members = ccord_store_member_count(F.store, g);
  CT_CHECK(ccord_mock_admin_count(tr) == 0);
  // These menus count on the built-in Copy User ID row, so Developer Mode is on for the member sections below.
  seed_user_settings(true, true, 0);

  // 1. Kick through the member menu: choosing the item only opens the dialog.
  ui_menu_open(F.ui, 400.0f, 300.0f, &(ccord_menu_context){CCORD_MENU_MEMBER, g, 0, 0, v1, nullptr, nullptr});
  CT_CHECK(dbg().context_menu_open && menu_has("Profile") && menu_has("Kick Member") &&
           menu_has("Copy User ID") && menu_has("Report User"));
  ccord_rect kick_rc = {0};
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Kick Member", &kick_rc));
  mouse(CCORD_IN_MOUSE_MOVE, 0, kick_rc.x + kick_rc.w * 0.5f, kick_rc.y + kick_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, kick_rc.x + kick_rc.w * 0.5f, kick_rc.y + kick_rc.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_KICK);
  CT_CHECK(ccord_mock_admin_count(tr) == 0);
  key(CCORD_KEY_ESCAPE, 0);   // cancel
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE && ccord_mock_admin_count(tr) == 0 && member_of(g, v1) != nullptr);

  ui_menu_open(F.ui, 400.0f, 300.0f, &(ccord_menu_context){CCORD_MENU_MEMBER, g, 0, 0, v1, nullptr, nullptr});
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Kick Member", &kick_rc));
  mouse(CCORD_IN_MOUSE_MOVE, 0, kick_rc.x + kick_rc.w * 0.5f, kick_rc.y + kick_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, kick_rc.x + kick_rc.w * 0.5f, kick_rc.y + kick_rc.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM);
  type_text("spam");   // the optional audit-log reason
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);
  CT_CHECK(ccord_mock_admin_count(tr) == 1);
  const ccord_admin_request *last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_KICK && last->guild_id == g && last->user_id == v1);
  CT_CHECK(last && strcmp(last->reason, "spam") == 0);
  CT_CHECK(member_of(g, v1) != nullptr);   // not removed on our say-so
  frame(2);
  CT_CHECK(member_of(g, v1) == nullptr && ccord_store_member_count(F.store, g) == members - 1);  // removed when the server confirmed

  // 2. Timeout: the default is 10 minutes, the member shows as timed out once Discord says so, and the menu then
  //    offers "Remove Timeout" instead of "Timeout Member".
  ui_confirm_open(F.ui, CCORD_ADMIN_TIMEOUT, g, v2, "x");
  key(CCORD_KEY_RETURN, 0);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 2 && last && last->op == CCORD_ADMIN_TIMEOUT);
  CT_CHECK(last && last->until_ms == F.ui->now_unix_ms + 10LL * 60 * 1000);
  CT_CHECK(member_of(g, v2)->timeout_until_ms == 0);
  frame(2);
  CT_CHECK(member_of(g, v2)->timeout_until_ms == F.ui->now_unix_ms + 10LL * 60 * 1000);
  ui_menu_open(F.ui, 400.0f, 300.0f, &(ccord_menu_context){CCORD_MENU_MEMBER, g, 0, 0, v2, nullptr, nullptr});
  CT_CHECK(dbg().context_menu_open && menu_has("Remove Timeout") && !menu_has("Timeout Member"));
  ccord_rect rt_rc = {0};
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Remove Timeout", &rt_rc));
  mouse(CCORD_IN_MOUSE_MOVE, 0, rt_rc.x + rt_rc.w * 0.5f, rt_rc.y + rt_rc.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, rt_rc.x + rt_rc.w * 0.5f, rt_rc.y + rt_rc.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);   // lifting a timeout asks nothing
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 3 && last && last->op == CCORD_ADMIN_TIMEOUT && last->until_ms == 0);
  frame(2);
  CT_CHECK(member_of(g, v2)->timeout_until_ms == 0);

  // 3. The duration chips: Right/Left move through the choices and stop at the ends.
  ui_confirm_open(F.ui, CCORD_ADMIN_TIMEOUT, g, v3, "x");
  key(CCORD_KEY_RIGHT, 0);
  key(CCORD_KEY_RIGHT, 0);
  for (int i = 0; i < 4; i++) key(CCORD_KEY_RIGHT, 0);   // past the end: stays on "1 week"
  key(CCORD_KEY_RETURN, 0);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->until_ms == F.ui->now_unix_ms + 7LL * 24 * 60 * 60 * 1000);
  ui_confirm_open(F.ui, CCORD_ADMIN_TIMEOUT, g, v3, "x");
  for (int i = 0; i < 6; i++) key(CCORD_KEY_LEFT, 0);   // past the start: stays on "60 secs"
  key(CCORD_KEY_RETURN, 0);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->until_ms == F.ui->now_unix_ms + 60LL * 1000);
  frame(2);

  // 4. Ban with "delete 24 hours of messages".
  ui_confirm_open(F.ui, CCORD_ADMIN_BAN, g, v4, "x");
  key(CCORD_KEY_RIGHT, 0);
  key(CCORD_KEY_RIGHT, 0);
  key(CCORD_KEY_RETURN, 0);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_BAN && last->user_id == v4);
  CT_CHECK(last && strstr(last->body, "\"delete_message_seconds\":86400") != nullptr);
  frame(2);
  CT_CHECK(member_of(g, v4) == nullptr);

  // 5. Nobody gets kicked, banned or timed out by accident: yourself and the owner are refused before anything is sent.
  size_t sent = ccord_mock_admin_count(tr);
  ui_confirm_open(F.ui, CCORD_ADMIN_KICK, g, ccord_store_self_id(F.store), "me");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_mock_admin_count(tr) == sent);
  CT_CHECK(strstr(dbg().notice, "Couldn't kick") != nullptr && strstr(dbg().notice, "yourself") != nullptr);

  // 6. Delete channel: confirm dialog, then a real call; a refusal leaves the channel and says why.
  seed_user_settings(false, true, 0);   // channel menus below keep their normal rows
  ccord_id dev = find_channel(g, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(dev != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  frame(3);
  float dev_y = scan_hover_channel(200.0f, dev);
  CT_CHECK(dev_y > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dev_y);
  frame(1);
  CT_CHECK(dbg().context_menu_open && dbg().context_menu_items == 8);
  CT_CHECK(menu_click_label("Delete Channel"));
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_DELETE_CHANNEL);
  CT_CHECK(ccord_mock_admin_count(tr) == sent);
  ccord_mock_set_refuse_writes(tr, true);
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_mock_admin_count(tr) == sent + 1);
  frame(3);
  CT_CHECK(ccord_store_channel(F.store, dev) != nullptr);   // refused: still there
  CT_CHECK(strstr(dbg().notice, "Couldn't delete the channel") != nullptr);
  CT_CHECK(strstr(dbg().notice, "Missing Permissions") != nullptr);   // Discord's words, not ours
  ccord_mock_set_refuse_writes(tr, false);

  // 7. Closing a DM goes through Discord too: the conversation stays in the list until it confirms.
  ccord_id dm_guild = CCORD_DM_GUILD_ID;
  ccord_id friend_id = other_member(g, 0);
  CT_CHECK(ccord_session_open_dm(F.session, friend_id, &err) == CCORD_OK);
  frame(3);
  ccord_id dm_id = 900000u + friend_id;
  CT_CHECK(ccord_store_channel(F.store, dm_id) != nullptr);
  size_t dms = ccord_store_channel_count(F.store, dm_guild);
  size_t before_close = ccord_mock_admin_count(tr);
  ui_close_dm(F.ui, dm_id);
  CT_CHECK(ccord_mock_admin_count(tr) == before_close + 1);
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_CLOSE_DM && ccord_mock_last_admin(tr)->channel_id == dm_id);
  CT_CHECK(ccord_store_channel_count(F.store, dm_guild) == dms);
  frame(2);
  CT_CHECK(ccord_store_channel_count(F.store, dm_guild) == dms - 1 && ccord_store_channel(F.store, dm_id) == nullptr);
}

// ---------------------------------------------------------------- Server Settings overlay

// Opt-in (CCORD_SHOT_DIR=<dir>): saves what the overlay looks like at key moments, for eyeballing. Asserts nothing.


// Clicks the centre of a named control of the overlay; false when it is not on screen.

// Like ss_click, but without letting any time pass afterwards (the mock answers within a frame or two).
static bool ss_press(const char *name)
{
  ccord_rect r;
  if (!ccord_ui_server_settings_find(F.ui, name, &r)) return false;
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  return true;
}

// Scrolls the editor until the named control is on screen; false when it never appears.

static void clear_focused_text(void)
{
  for (int i = 0; i < 60; i++) key(CCORD_KEY_BACKSPACE, 0);
}

static const ccord_role *role_named(ccord_id guild, const char *name)
{
  size_t n = 0;
  const ccord_role *r = ccord_store_roles(F.store, guild, &n);
  for (size_t i = 0; r && i < n; i++) {
    if (r[i].name && strcmp(r[i].name, name) == 0) return &r[i];
  }
  return nullptr;
}

// Who may open Server Settings, and what the overlay offers, follows the permission engine; the title page of a
// moderator is Bans, not an empty Overview.
static void test_server_settings_gating_and_overview(ccord_transport *tr)
{
  const ccord_id hq = 2001, lua = 2002, garden = 2003;
  CT_CHECK(!ccord_ui_open_server_settings(F.ui, garden));   // an ordinary member has nothing to manage
  CT_CHECK(!ssd().open);
  CT_CHECK(!ccord_ui_open_server_settings(F.ui, CCORD_DM_GUILD_ID) && !ccord_ui_open_server_settings(F.ui, 99999));

  // Lua Lounge: a moderator may ban, nothing else here.
  size_t before = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, lua));
  frame(3);
  CT_CHECK(ssd().open && ssd().guild_id == lua && ssd().page == 3);
  shot("lua-bans");
  CT_CHECK(ss_has("page:bans") && !ss_has("page:overview") && !ss_has("page:roles") && !ss_has("page:invites") &&
           !ss_has("page:audit"));
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_BANS);
  frame(20);   // drawing and idling never ask again
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!ssd().open);

  // C-Cord HQ: administrator, every page.
  before = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(3);
  CT_CHECK(ssd().open && ssd().guild_id == hq && ssd().page == 0);
  CT_CHECK(ss_has("page:overview") && ss_has("page:roles") && ss_has("page:invites") && ss_has("page:bans") &&
           ss_has("page:audit") && ss_has("close"));
  CT_CHECK(ccord_mock_admin_count(tr) == before);   // Overview needs no list
  CT_CHECK(!ssd().unsaved && !ss_has("save"));

  // Renaming: typing alone changes nothing on Discord; Save sends exactly one request; the name changes when it answers.
  const char *old_name = ccord_store_guild(F.store, hq)->name;
  char saved_old[96];
  snprintf(saved_old, sizeof saved_old, "%s", old_name);
  CT_CHECK(ss_click("name") && ssd().focus == 1);
  type_text(" Reborn");
  CT_CHECK(ssd().unsaved);
  frame(1);
  shot("overview-unsaved");
  CT_CHECK(ss_has("save") && ss_has("reset"));
  CT_CHECK(ccord_mock_admin_count(tr) == before);
  CT_CHECK(strcmp(ccord_store_guild(F.store, hq)->name, saved_old) == 0);

  // Reset puts the field back without bothering Discord.
  CT_CHECK(ss_click("reset"));
  CT_CHECK(!ssd().unsaved && ccord_mock_admin_count(tr) == before);

  // While changes are pending, leaving is refused and says why; Escape leaves the field first.
  CT_CHECK(ss_click("name"));
  type_text("!");
  CT_CHECK(ssd().unsaved);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(ssd().open && ssd().focus == 0);
  CT_CHECK(ss_click("page:roles"));
  CT_CHECK(ssd().page == 0 && strstr(dbg().notice, "unsaved") != nullptr);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(ssd().open);
  CT_CHECK(ss_click("close"));
  CT_CHECK(ssd().open);

  // Save: one request however hard the button is pressed.
  ccord_rect save_rc;
  CT_CHECK(ccord_ui_server_settings_find(F.ui, "save", &save_rc));
  for (int i = 0; i < 3; i++) {   // three presses before any frame passes
    mouse(CCORD_IN_MOUSE_MOVE, 0, save_rc.x + 4.0f, save_rc.y + 4.0f);
    mouse(CCORD_IN_MOUSE_DOWN, 1, save_rc.x + 4.0f, save_rc.y + 4.0f);
    mouse(CCORD_IN_MOUSE_UP, 1, save_rc.x + 4.0f, save_rc.y + 4.0f);
  }
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1);
  const ccord_admin_request *last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_EDIT_GUILD && last->guild_id == hq);
  CT_CHECK(last && strstr(last->body, "\"name\":\"") != nullptr && strstr(last->body, "!\"") != nullptr);
  frame(3);
  CT_CHECK(!ssd().unsaved);
  char want[128];
  snprintf(want, sizeof want, "%s!", saved_old);
  CT_CHECK(strcmp(ccord_store_guild(F.store, hq)->name, want) == 0);   // Discord's answer, not ours

  // Put the name back (also checks the field follows the store when clean). A second save within the press guard
  // (0.8 s) of the first would be ignored, which no human edit-and-save can reach.
  frame(20);
  CT_CHECK(ss_click("name"));
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
  CT_CHECK(ssd().unsaved && ss_click("save"));
  frame(3);
  CT_CHECK(strcmp(ccord_store_guild(F.store, hq)->name, saved_old) == 0 && !ssd().unsaved);
}

static void test_server_settings_roles(ccord_transport *tr)
{
  const ccord_id hq = 2001, chen = 1004;
  CT_CHECK(ssd().open && ssd().guild_id == hq);
  size_t nperm = 0;
  const ccord_perm_info *ptbl = ccord_perm_table(&nperm);
  CT_CHECK(ptbl && nperm > 40);
  char last_name[32];
  snprintf(last_name, sizeof last_name, "perm:%llx", (unsigned long long)ptbl[nperm - 1].bit);
  CT_CHECK(ss_click("page:roles") && ssd().page == 1);
  CT_CHECK(ssd().role_id == hq);   // @everyone, which always exists
  CT_CHECK(ss_has("role:5003") && ss_has("role:5002") && ss_has("role:5001"));
  size_t before = ccord_mock_admin_count(tr);
  size_t roles_before = 0;
  (void)ccord_store_roles(F.store, hq, &roles_before);

  // @everyone has permissions only: no name, colour or delete.
  CT_CHECK(ss_click("role:2001") || true);
  CT_CHECK(!ss_has("role-delete"));

  // Create: asks Discord, then selects what Discord made.
  CT_CHECK(ss_click("role-create"));
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_CREATE_ROLE);
  frame(4);
  size_t roles_after = 0;
  (void)ccord_store_roles(F.store, hq, &roles_after);
  CT_CHECK(roles_after == roles_before + 1);
  ccord_id made = ssd().role_id;
  CT_CHECK(made >= 6000 && made != hq && ssd().role_tab == 0 && !ssd().unsaved);
  CT_CHECK(ss_has("role-name") && ss_has("role-delete") && ss_has("role-color"));
  shot("roles-display");

  // Rename + one colour + one permission: nothing sent until Save, and Save sends only what changed.
  CT_CHECK(ss_click("role-name") && ssd().focus == 2);
  clear_focused_text();
  type_text("Artists");
  CT_CHECK(ssd().unsaved);
  CT_CHECK(ss_click("color:E91E63") && ssd().work_color == 0xE91E63u);
  CT_CHECK(ss_click("tab:perms") && ssd().role_tab == 1);
  shot("roles-perms-unsaved");
  uint64_t perms0 = ssd().work_permissions;
  uint64_t first_bit = 0;   // the first switch on screen that is off (new roles start with @everyone's permissions)
  char first_name[32] = "";
  for (size_t i = 0; i < nperm && !first_bit; i++) {
    snprintf(first_name, sizeof first_name, "perm:%llx", (unsigned long long)ptbl[i].bit);
    if ((perms0 & ptbl[i].bit) == 0 && ss_has(first_name)) first_bit = ptbl[i].bit;
  }
  CT_CHECK(first_bit != 0);
  CT_CHECK(ss_click(first_name));
  CT_CHECK((ssd().work_permissions & first_bit) != 0);
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1);

  // Pending changes block leaving the page, and selecting another role.
  CT_CHECK(ss_click("role:5003"));
  CT_CHECK(ssd().role_id == made && strstr(dbg().notice, "unsaved") != nullptr);
  CT_CHECK(ss_click("page:bans") && ssd().page == 1);

  // Reset returns to Discord's copy...
  CT_CHECK(ss_click("reset"));
  CT_CHECK(!ssd().unsaved && ssd().work_permissions == perms0 && ssd().work_color == 0);
  // ...and Save sends the diff.
  CT_CHECK(ss_click(first_name) && ss_click("tab:display") && ss_click("role-name"));
  clear_focused_text();
  type_text("Artists");
  CT_CHECK(ss_click("color:E91E63"));
  CT_CHECK(ss_click("save"));
  CT_CHECK(ccord_mock_admin_count(tr) == before + 2);
  const ccord_admin_request *last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_EDIT_ROLE && last->role_id == made);
  CT_CHECK(last && strstr(last->body, "\"name\":\"Artists\"") != nullptr && strstr(last->body, "\"color\":") != nullptr &&
           strstr(last->body, "\"permissions\"") != nullptr && strstr(last->body, "hoist") == nullptr);
  frame(4);
  const ccord_role *art = ccord_store_role(F.store, hq, made);
  CT_CHECK(art && strcmp(art->name, "Artists") == 0 && art->color == 0xE91E63u && (art->permissions & first_bit));
  CT_CHECK(!ssd().unsaved);

  // The colour field takes #RRGGBB too.
  CT_CHECK(ss_click("role-color"));
  clear_focused_text();
  type_text("#3498db");
  frame(1);
  CT_CHECK(ssd().work_color == 0x3498DBu && ssd().unsaved);
  CT_CHECK(ss_click("reset") && ssd().work_color == 0xE91E63u);

  // Somebody else edits the role while you have changes pending: yours stay, the rest follows Discord, and Save sends
  // only yours (it must not put their colour back).
  CT_CHECK(ss_click("role-name"));
  clear_focused_text();
  type_text("Artists 2");
  frame(1);
  CT_CHECK(ssd().unsaved && ssd().work_color == 0xE91E63u);
  {
    ccord_role other = *ccord_store_role(F.store, hq, made);
    char other_name[64];
    snprintf(other_name, sizeof other_name, "%s", other.name);
    other.name = other_name;
    other.guild_id = hq;
    other.color = 0x2ECC71u;
    ccord_error uerr;
    CT_CHECK(ccord_store_upsert_role(F.store, &other, &uerr) == CCORD_OK);
  }
  frame(2);
  CT_CHECK(ssd().unsaved && ssd().work_color == 0x2ECC71u);   // their colour shows, our name edit is still ours
  CT_CHECK(ss_click("save"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_EDIT_ROLE && strstr(last->body, "\"name\":\"Artists 2\"") != nullptr &&
           strstr(last->body, "color") == nullptr && strstr(last->body, "permissions") == nullptr);
  frame(4);
  CT_CHECK(!ssd().unsaved && strcmp(ccord_store_role(F.store, hq, made)->name, "Artists 2") == 0);   // (the mock never knew their colour)

  // Their permission change and rename arrive while you have flipped a different switch: both are kept, and the name
  // you did not touch follows theirs.
  frame(20);
  CT_CHECK(ss_click("tab:perms"));
  CT_CHECK(ss_scroll_to(first_name));
  CT_CHECK(ss_click(first_name) && ssd().unsaved);
  {
    const ccord_role *cur = ccord_store_role(F.store, hq, made);
    ccord_role other = *cur;
    char other_name[64];
    snprintf(other_name, sizeof other_name, "Renamed By Them");
    other.name = other_name;
    other.guild_id = hq;
    other.permissions = cur->permissions ^ CCORD_PERM_MANAGE_MESSAGES;
    uint64_t want = other.permissions ^ first_bit;   // theirs, with our flip on top
    ccord_error uerr;
    CT_CHECK(ccord_store_upsert_role(F.store, &other, &uerr) == CCORD_OK);
    frame(2);
    CT_CHECK(ssd().unsaved && ssd().work_permissions == want);
    CT_CHECK(strcmp(ssd().role_name, "Renamed By Them") == 0);
    CT_CHECK(ss_click("reset") && !ssd().unsaved && ssd().work_permissions == other.permissions);
  }

  // Only what changed is sent: one permission flip sends the permissions and nothing else; the two display switches
  // are sent when, and only when, they were flipped.
  frame(20);   // past the press guard
  CT_CHECK(ss_click(first_name) && ssd().unsaved && ss_click("save"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_EDIT_ROLE && strstr(last->body, "permissions") != nullptr &&
           strstr(last->body, "name") == nullptr && strstr(last->body, "color") == nullptr &&
           strstr(last->body, "hoist") == nullptr && strstr(last->body, "mentionable") == nullptr);
  frame(4);
  CT_CHECK(!ssd().unsaved);
  frame(20);
  CT_CHECK(ss_click("tab:display") && ss_click("hoist") && ssd().unsaved && ss_click("save"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && strstr(last->body, "\"hoist\":true") != nullptr && strstr(last->body, "permissions") == nullptr &&
           strstr(last->body, "name") == nullptr && strstr(last->body, "mentionable") == nullptr);
  frame(4);
  CT_CHECK(ccord_store_role(F.store, hq, made)->hoist && !ssd().unsaved);
  frame(20);
  CT_CHECK(ss_click("mention") && ssd().unsaved && ss_click("save"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && strstr(last->body, "\"mentionable\":true") != nullptr && strstr(last->body, "hoist") == nullptr);
  frame(4);
  CT_CHECK(ccord_store_role(F.store, hq, made)->mentionable && !ssd().unsaved);

  // Reorder: up asks Discord to move it, and the list follows the answer.
  int32_t pos0 = ccord_store_role(F.store, hq, made)->position;
  CT_CHECK(ss_press("role-up"));
  CT_CHECK(ccord_mock_admin_count(tr) == before + 7 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_REORDER_ROLES);   // create, 5 saves, this
  CT_CHECK(ccord_store_role(F.store, hq, made)->position == pos0);   // not on our say-so
  frame(4);
  CT_CHECK(ccord_store_role(F.store, hq, made)->position > pos0);
  CT_CHECK(ss_click("role-down"));
  frame(4);
  CT_CHECK(ccord_store_role(F.store, hq, made)->position == pos0);

  // The long permission list scrolls with the wheel: the last switch is off screen until then.
  CT_CHECK(ss_click("tab:perms"));
  CT_CHECK(!ss_has(last_name));
  ccord_rect view = {0};
  CT_CHECK(ccord_ui_server_settings_find(F.ui, first_name, &view));
  for (int i = 0; i < 80 && !ss_has(last_name); i++) {
    ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = view.x - 20.0f, .y = view.y, .wheel = -3.0f});
    frame(1);
  }
  CT_CHECK(ss_has(last_name) && ssd().scroll > 0.0f);
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = view.x - 20.0f, .y = view.y, .wheel = 500.0f});
  frame(2);
  CT_CHECK(ssd().scroll == 0.0f);   // never above the top

  // Members tab: giving the role to somebody is one request each, and shows once Discord answers.
  CT_CHECK(ss_click("tab:members") && ssd().role_tab == 2);
  size_t n_before = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("member:1004"));
  CT_CHECK(ccord_mock_admin_count(tr) == n_before + 1);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_ADD_MEMBER_ROLE && last->user_id == chen && last->role_id == made);
  frame(4);
  const ccord_id *ids = nullptr;
  size_t idn = 0;
  bool has = false;
  CT_CHECK(ccord_store_member_roles(F.store, hq, chen, &ids, &idn));
  for (size_t i = 0; i < idn; i++) has = has || ids[i] == made;
  CT_CHECK(has);
  CT_CHECK(ss_click("member:1004"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_REMOVE_MEMBER_ROLE && last->user_id == chen);
  frame(4);

  // Delete: a confirm dialog first (Escape backs out), then the real call; selection falls back to @everyone.
  size_t d_before = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("tab:display") && ss_click("role-delete"));
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_DELETE_ROLE);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE && ccord_mock_admin_count(tr) == d_before && ccord_store_role(F.store, hq, made));
  CT_CHECK(ssd().open);
  CT_CHECK(ss_click("role-delete"));
  type_text("cleanup");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_mock_admin_count(tr) == d_before + 1);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_DELETE_ROLE && last->role_id == made && strcmp(last->reason, "cleanup") == 0);
  frame(4);
  CT_CHECK(ccord_store_role(F.store, hq, made) == nullptr && ssd().role_id == hq);
  CT_CHECK(role_named(hq, "Artists") == nullptr);
}

// A frame that lets time pass without reading the transport: nothing can answer in it.

static void test_server_settings_lists(ccord_transport *tr)
{
  const ccord_id hq = 2001;
  CT_CHECK(ssd().open && ssd().guild_id == hq);
  size_t base = ccord_mock_admin_count(tr);
  const ccord_admin_request *last = nullptr;

  // ---- Invites: one request when the page opens, then the list is reused.
  CT_CHECK(ss_click("page:invites") && ssd().page == 2);
  CT_CHECK(ccord_mock_admin_count(tr) == base + 1 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_INVITES);
  frame(4);
  CT_CHECK(ccord_store_invites_loaded(F.store, hq) && ssd().rows == 2);
  CT_CHECK(ss_has("invite-copy:cCordHQ") && ss_has("invite-revoke:weekend") && ss_has("invite-create"));
  shot("invites");
  frame(30);   // idling and drawing never ask again
  CT_CHECK(ccord_mock_admin_count(tr) == base + 1);

  F.clip[0] = '\0';
  CT_CHECK(ss_click("invite-copy:cCordHQ"));
  CT_CHECK(strcmp(F.clip, "https://discord.gg/cCordHQ") == 0 && ccord_mock_admin_count(tr) == base + 1);

  // Refresh is no hammer: presses inside 2 s of the last request are ignored, later ones go through.
  CT_CHECK(ss_press("refresh") && ss_press("refresh") && ss_press("refresh"));
  CT_CHECK(ccord_mock_admin_count(tr) == base + 1);
  frame(50);
  CT_CHECK(ss_press("refresh") && ss_press("refresh"));
  CT_CHECK(ccord_mock_admin_count(tr) == base + 2 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_INVITES);
  frame(4);

  // Another page costs one request of its own; coming back inside 30 s costs none.
  CT_CHECK(ss_click("page:bans") && ccord_mock_admin_count(tr) == base + 3);
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_BANS);
  frame(4);
  CT_CHECK(ss_click("page:invites") && ccord_mock_admin_count(tr) == base + 3 && ssd().rows == 2);
  size_t c = ccord_mock_admin_count(tr);

  // Create: the chips are local, the button sends one request with what they say.
  CT_CHECK(ss_click("invite-age:604800") && ss_click("invite-uses:10"));
  CT_CHECK(ccord_mock_admin_count(tr) == c);
  CT_CHECK(ss_click("invite-create"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == c + 1 && last && last->op == CCORD_ADMIN_CREATE_INVITE);
  CT_CHECK(last && last->invite_max_age_s == 604800u && last->invite_max_uses == 10u && last->guild_id == hq);
  const ccord_channel *target = last ? ccord_store_channel(F.store, last->channel_id) : nullptr;
  CT_CHECK(target && target->guild_id == hq && target->kind == CCORD_CHANNEL_TEXT);
  frame(4);
  CT_CHECK(ssd().rows == 3);
  size_t ni = 0;
  const ccord_invite *invs = ccord_store_invites(F.store, hq, &ni);
  char code[40] = "";
  for (size_t i = 0; invs && i < ni; i++) {
    if (invs[i].max_uses == 10 && strcmp(invs[i].code, "weekend") != 0) snprintf(code, sizeof code, "%s", invs[i].code);
  }
  CT_CHECK(code[0] != '\0');
  char copy_name[64], revoke_name[64], link[96];
  snprintf(copy_name, sizeof copy_name, "invite-copy:%s", code);
  snprintf(revoke_name, sizeof revoke_name, "invite-revoke:%s", code);
  snprintf(link, sizeof link, "https://discord.gg/%s", code);
  CT_CHECK(ss_click(copy_name) && strcmp(F.clip, link) == 0);

  // Revoke: a confirm dialog first (Escape backs out), then the real call; the row goes when Discord says so.
  c = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click(revoke_name));
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM && F.ui->confirm_op == CCORD_ADMIN_REVOKE_INVITE);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE && ccord_mock_admin_count(tr) == c && ssd().rows == 3 && ssd().open);
  CT_CHECK(ss_click(revoke_name));
  key(CCORD_KEY_RETURN, 0);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == c + 1 && last && last->op == CCORD_ADMIN_REVOKE_INVITE && strcmp(last->code, code) == 0);
  CT_CHECK(ssd().rows == 3);
  frame(4);
  CT_CHECK(ssd().rows == 2 && !ss_has(revoke_name));

  // ---- Bans: searching is local; Revoke Ban is one request and the row goes when Discord answers.
  CT_CHECK(ss_click("page:bans") && ssd().page == 3);
  frame(2);
  c = ccord_mock_admin_count(tr);
  CT_CHECK(ssd().rows == 2 && ss_has("unban:7001") && ss_has("unban:7002"));
  shot("bans");
  CT_CHECK(ss_click("ban-search") && ssd().focus == 4);
  type_text("troll");
  frame(2);
  CT_CHECK(ssd().rows == 1 && ss_has("unban:7002") && !ss_has("unban:7001"));
  clear_focused_text();
  type_text("7001");   // by id, too
  frame(2);
  CT_CHECK(ssd().rows == 1 && ss_has("unban:7001") && !ss_has("unban:7002"));
  clear_focused_text();
  type_text("nobody");
  frame(2);
  CT_CHECK(ssd().rows == 0);
  clear_focused_text();
  frame(2);
  CT_CHECK(ssd().rows == 2 && ccord_mock_admin_count(tr) == c);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(ssd().focus == 0 && ssd().open);
  CT_CHECK(ss_click("unban:7001"));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == c + 1 && last && last->op == CCORD_ADMIN_UNBAN && last->user_id == 7001u);
  frame(4);
  CT_CHECK(ssd().rows == 1 && !ss_has("unban:7001") && ss_has("unban:7002"));

  // ---- Audit log: the filter chips are local and agree with the model.
  CT_CHECK(ss_click("page:audit") && ssd().page == 4);
  CT_CHECK(ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_AUDIT_LOG);
  frame(4);
  c = ccord_mock_admin_count(tr);
  size_t na = 0;
  const ccord_audit_entry *log = ccord_store_audit(F.store, hq, &na);
  CT_CHECK(log && na >= 6 && ssd().rows == na);   // the seeded three, and what this test just did
  shot("audit");
  bool narrowed = false;
  for (int cat = CCORD_AUDIT_CAT_ALL + 1; cat < CCORD_AUDIT_CAT_COUNT; cat++) {
    char fname[40];
    snprintf(fname, sizeof fname, "audit-filter:%d", cat);
    CT_CHECK(ss_click(fname));
    size_t want = 0;
    for (size_t i = 0; i < na; i++) want += ccord_audit_matches((ccord_audit_category)cat, log[i].action) ? 1u : 0u;
    CT_CHECK(ssd().rows == want);
    narrowed = narrowed || (want > 0 && want < na);
  }
  CT_CHECK(narrowed);
  char all_name[40];
  snprintf(all_name, sizeof all_name, "audit-filter:%d", (int)CCORD_AUDIT_CAT_ALL);
  CT_CHECK(ss_click(all_name) && ssd().rows == na);
  CT_CHECK(ccord_mock_admin_count(tr) == c);

  // ---- Leaving drops the lists (they are never kept around); a stale list is asked for again, once.
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!ssd().open);
  CT_CHECK(!ccord_store_audit_loaded(F.store, hq) && !ccord_store_bans_loaded(F.store, hq) &&
           !ccord_store_invites_loaded(F.store, hq));
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_click("page:bans"));
  frame(4);
  F.now += 31000;   // past the 30 s a list may be reused for
  frame(2);
  c = ccord_mock_admin_count(tr);
  CT_CHECK(ss_click("page:invites") && ss_click("page:bans"));
  CT_CHECK(ccord_mock_admin_count(tr) == c + 2);   // each page re-entered once
  frame(4);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!ssd().open);
}

// What the page says when Discord refuses or never answers, and that it recovers.
static void test_server_settings_failures(ccord_transport *tr)
{
  const ccord_id hq = 2001;
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  size_t c = ccord_mock_admin_count(tr);

  // Refused: nothing to show, so the page offers Try Again (and the notice carries Discord's words).
  ccord_mock_set_refuse_writes(tr, true);
  CT_CHECK(ss_click("page:invites"));
  frame(3);
  CT_CHECK(ccord_mock_admin_count(tr) == c + 1 && !ccord_store_invites_loaded(F.store, hq));
  CT_CHECK(ss_has("retry") && ssd().rows == 0);
  CT_CHECK(strstr(dbg().notice, "Missing Permissions") != nullptr);
  frame(60);   // stays put, and does not retry by itself
  CT_CHECK(ccord_mock_admin_count(tr) == c + 1);
  ccord_mock_set_refuse_writes(tr, false);
  CT_CHECK(ss_click("retry"));   // at least 2 s since the request: allowed
  CT_CHECK(ccord_mock_admin_count(tr) == c + 2);
  frame(4);
  CT_CHECK(ccord_store_invites_loaded(F.store, hq) && ssd().rows == 2 && !ss_has("retry"));

  // A refusal belongs to the page that asked.
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!ssd().open);
  for (int pass = 0; pass < 2; pass++) {
    CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
    frame(2);
    ccord_mock_set_refuse_writes(tr, true);
    CT_CHECK(ss_click(pass == 0 ? "page:bans" : "page:audit"));
    frame(3);
    CT_CHECK(ss_has("retry"));
    ccord_mock_set_refuse_writes(tr, false);
    key(CCORD_KEY_ESCAPE, 0);
    CT_CHECK(!ssd().open);
  }
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);

  // No answer at all: after 10 s the page stops saying "Loading" and offers Try Again; a late answer still lands.
  CT_CHECK(ss_click("page:audit"));   // sent; the mock has not been read yet when time jumps
  CT_CHECK(ssd().page == 4);
  c = ccord_mock_admin_count(tr);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!ssd().open);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_press("page:bans"));
  CT_CHECK(ccord_mock_admin_count(tr) == c + 1);
  frame_deaf(100);
  CT_CHECK(!ss_has("retry"));   // still loading
  frame_deaf(11000);
  CT_CHECK(ss_has("retry"));    // given up waiting
  frame(4);                       // ...but the answer was only slow
  CT_CHECK(ccord_store_bans_loaded(F.store, hq) && ssd().rows == 1 && !ss_has("retry"));
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!ssd().open);
}

static void set_hq_moderator_perms(uint64_t perms)
{
  const ccord_id hq = 2001, mods = 5002;
  const ccord_role *old = ccord_store_role(F.store, hq, mods);
  CT_CHECK(old != nullptr);
  if (!old) return;
  char name[64];
  snprintf(name, sizeof name, "%s", old->name);
  ccord_role r = *old;
  r.name = name;
  r.guild_id = hq;
  r.permissions = perms;
  ccord_error err;
  CT_CHECK(ccord_store_upsert_role(F.store, &r, &err) == CCORD_OK);
}

// Manage Roles is not Administrator: the editor locks what is above your highest role, shows why, and sends nothing;
// you cannot hand out a permission you do not hold; and when Discord takes pages away from under an open window, it
// follows (or closes).
static void test_server_settings_hierarchy(ccord_transport *tr)
{
  const ccord_id hq = 2001, self = 1001, admins = 5001, mods = 5002, members = 5003;
  const ccord_role *mrole = ccord_store_role(F.store, hq, mods);
  CT_CHECK(mrole != nullptr);
  if (!mrole) return;
  const uint64_t mods_perms_before = mrole->permissions;
  const ccord_id *ids = nullptr;
  size_t nids = 0;
  CT_CHECK(ccord_store_member_roles(F.store, hq, self, &ids, &nids) && nids > 0 && nids < 8);
  ccord_id self_roles[8];
  for (size_t i = 0; i < nids; i++) self_roles[i] = ids[i];
  const size_t self_nroles = nids;

  ccord_error err;
  set_hq_moderator_perms(CCORD_PERM_MANAGE_ROLES | CCORD_PERM_VIEW_CHANNEL);
  CT_CHECK(ccord_store_set_member_roles(F.store, hq, self, &mods, 1, &err) == CCORD_OK);
  uint64_t perms = ccord_store_permissions(F.store, hq, CCORD_ID_NONE, self, 0);
  CT_CHECK(ccord_store_permissions_exact(F.store, hq, self) && (perms & CCORD_PERM_MANAGE_ROLES) &&
           !(perms & CCORD_PERM_ADMINISTRATOR) && !(perms & CCORD_PERM_MANAGE_GUILD) && !(perms & CCORD_PERM_BAN_MEMBERS));
  CT_CHECK(ccord_store_guild(F.store, hq)->owner_id != self);
  frame(2);

  size_t before = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(3);
  CT_CHECK(ssd().page == 1);   // the first page this user has
  CT_CHECK(ss_has("page:roles") && !ss_has("page:overview") && !ss_has("page:invites") && !ss_has("page:bans") &&
           !ss_has("page:audit"));

  // A role above yours: viewable, not editable. Nothing is sent, and the reason is given.
  CT_CHECK(ss_click("role:5001") && ssd().role_id == admins);
  shot("roles-locked");
  CT_CHECK(ss_click("role-name") && ssd().focus == 0 && strstr(dbg().notice, "below your highest role") != nullptr);
  CT_CHECK(ss_click("color:E91E63") && !ssd().unsaved);
  CT_CHECK(ss_click("role-delete") && F.ui->modal_kind == UI_MODAL_NONE);
  CT_CHECK(ss_click("tab:perms"));
  CT_CHECK(ss_scroll_to("perm:4"));
  uint64_t admins_perms = ssd().work_permissions;
  CT_CHECK(ss_click("perm:4") && ssd().work_permissions == admins_perms && !ssd().unsaved);
  CT_CHECK(ccord_mock_admin_count(tr) == before);

  // A role below yours: editable, but only with permissions you hold yourself.
  CT_CHECK(ss_click("role:5003") && ssd().role_id == members);
  CT_CHECK(ss_click("tab:display") && ss_click("role-name") && ssd().focus == 2);
  type_text("x");
  frame(1);
  CT_CHECK(ssd().unsaved);
  CT_CHECK(ss_click("reset") && !ssd().unsaved);
  CT_CHECK(ss_click("tab:perms"));
  CT_CHECK((ssd().work_permissions & CCORD_PERM_BAN_MEMBERS) == 0 && (ssd().work_permissions & CCORD_PERM_MANAGE_ROLES) == 0);
  CT_CHECK(ss_scroll_to("perm:4"));
  CT_CHECK(ss_click("perm:4") && (ssd().work_permissions & CCORD_PERM_BAN_MEMBERS) == 0 && !ssd().unsaved);   // not yours to give
  CT_CHECK(strstr(dbg().notice, "give out permissions you have") != nullptr);
  CT_CHECK(ss_scroll_to("perm:10000000"));   // Manage Roles, which you do hold
  CT_CHECK(ss_click("perm:10000000") && (ssd().work_permissions & CCORD_PERM_MANAGE_ROLES) && ssd().unsaved);
  CT_CHECK(ss_click("perm:10000000") && !ssd().unsaved);
  CT_CHECK(ccord_mock_admin_count(tr) == before);

  // Discord takes Manage Roles away and leaves Ban Members: the window moves to Bans (asking for the list once).
  set_hq_moderator_perms(CCORD_PERM_BAN_MEMBERS | CCORD_PERM_VIEW_CHANNEL);
  frame(3);
  CT_CHECK(ssd().open && ssd().page == 3 && !ss_has("page:roles"));
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1 && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_LIST_BANS);
  frame(30);
  CT_CHECK(ccord_mock_admin_count(tr) == before + 1);
  // ...and when nothing is left, it closes.
  set_hq_moderator_perms(CCORD_PERM_VIEW_CHANNEL);
  frame(3);
  CT_CHECK(!ssd().open);
  CT_CHECK(!ccord_ui_open_server_settings(F.ui, hq));

  // Put the world back.
  set_hq_moderator_perms(mods_perms_before);
  CT_CHECK(ccord_store_set_member_roles(F.store, hq, self, self_roles, self_nroles, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  ui_server_settings_close(F.ui);
}

static void test_server_settings_overlay(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  CT_CHECK(ccord_mock_admin_count(tr) == 0);
  test_server_settings_gating_and_overview(tr);
  test_server_settings_roles(tr);
  test_server_settings_lists(tr);
  test_server_settings_failures(tr);
  test_server_settings_hierarchy(tr);
  ui_server_settings_close(F.ui);
}

// The row of the member list whose header starts with `prefix` ("ARTISTS"); SIZE_MAX when there is none.


// The member list and the colour of a name follow the role table, the way Discord does it: grouped under the highest
// hoisted role ("Online" when there is none), coloured by the highest role that has a colour, and editing a role in
// Server Settings regroups and recolours at once.
static void test_member_list_follows_roles(void)
{
  const ccord_id hq = 2001, chen = 1004;
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  frame(6);
  size_t admins = member_header("ADMINS"), mods = member_header("MODERATORS"), online = member_header("ONLINE");
  CT_CHECK(admins != SIZE_MAX && mods != SIZE_MAX && online != SIZE_MAX && admins < mods && mods < online);
  CT_CHECK(member_header("MEMBERS") == SIZE_MAX && member_header("ARTISTS") == SIZE_MAX);   // Members is not hoisted
  size_t online_before = member_header_count("ONLINE");
  CT_CHECK(online_before >= 2);
  bool has = false;
  (void)ui_role_color(F.ui, hq, chen, &has);
  CT_CHECK(!has);   // a plain member has no colour

  // A new role, hoisted and pink, given to Chen through Server Settings.
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_click("page:roles") && ss_click("role-create"));
  frame(4);
  ccord_id made = ssd().role_id;
  CT_CHECK(made >= 6000);
  CT_CHECK(ss_click("role-name"));
  clear_focused_text();
  type_text("Artists");
  CT_CHECK(ss_click("color:E91E63") && ss_click("hoist") && ss_click("save"));
  frame(4);
  CT_CHECK(!ssd().unsaved);
  CT_CHECK(member_header("ARTISTS") == SIZE_MAX);   // nobody holds it yet
  CT_CHECK(ss_click("tab:members") && ss_click("member:1004"));
  frame(4);
  key(CCORD_KEY_ESCAPE, 0);
  frame(3);
  CT_CHECK(!ssd().open);
  size_t artists = member_header("ARTISTS");
  CT_CHECK(artists != SIZE_MAX && member_header_count("ARTISTS") == 1);
  mods = member_header("MODERATORS");
  online = member_header("ONLINE");
  CT_CHECK(mods < artists && artists < online);                      // by role position
  CT_CHECK(member_header_count("ONLINE") == online_before - 1);      // he left the plain group
  ccord_color pink = ui_role_color(F.ui, hq, chen, &has);
  CT_CHECK(has && pink.r == 0xE9 && pink.g == 0x1E && pink.b == 0x63);

  // Un-hoisting dissolves the group at once, but the colour stays (it is a different setting).
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_click("page:roles"));
  char rname[32];
  snprintf(rname, sizeof rname, "role:%llu", (unsigned long long)made);
  CT_CHECK(ss_click(rname));
  frame(20);
  CT_CHECK(ss_click("hoist") && ssd().unsaved && ss_click("save"));
  frame(4);
  key(CCORD_KEY_ESCAPE, 0);
  frame(3);
  CT_CHECK(member_header("ARTISTS") == SIZE_MAX && member_header_count("ONLINE") == online_before);
  (void)ui_role_color(F.ui, hq, chen, &has);
  CT_CHECK(has);

  // Deleting the role takes the colour with it.
  CT_CHECK(ccord_ui_open_server_settings(F.ui, hq));
  frame(2);
  CT_CHECK(ss_click("page:roles") && ss_click(rname) && ss_click("role-delete"));
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  key(CCORD_KEY_ESCAPE, 0);
  frame(3);
  CT_CHECK(ccord_store_role(F.store, hq, made) == nullptr);
  (void)ui_role_color(F.ui, hq, chen, &has);
  CT_CHECK(!has && member_header_count("ONLINE") == online_before);
}

// Whether a channel (or category, or thread) of the selected guild has a row in the channel list.
static bool channel_listed(ccord_id channel)
{
  ccord_id g = ccord_store_selected_guild(F.store);
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    const chan_row *r = &F.ui->crow[i];
    if (r->is_voice_user || r->is_relationship || r->dm_nav != DM_NAV_NONE) continue;
    const ccord_channel *c = ccord_store_channel_at(F.store, g, r->index);
    if (c && c->id == channel) return true;
  }
  return false;
}

static size_t mock_requests(const ccord_transport *t)
{
  size_t n = ccord_mock_admin_count(t);
  for (int c = 0; c < CCORD_MOCK_CALL_COUNT; c++) n += ccord_mock_call_count(t, (ccord_mock_call)c);
  return n;
}

static size_t switcher_hits(const char *query)
{
  key(CCORD_KEY_K, CCORD_MOD_CTRL);
  frame(2);
  type_text(query);
  frame(2);
  size_t n = F.ui->nquick_switcher_items;
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  return n;
}

static size_t hash_complete(const char *typed)
{
  type_text(typed);
  frame(1);
  size_t n = dbg().autocomplete_open ? dbg().autocomplete_count : 0;
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
  return n;
}

// Discord does not list what you may not see: a channel whose overwrites take View Channel away is missing from the
// channel list, the "#" completion, the quick switcher and the server's unread badge. It is decided from the exact
// roles only (a guess hides nothing), the channel you are looking at stays, and a role change shows or hides it live.
static void test_hidden_channels(void)
{
  const ccord_id garden = 2003, private_ch = 3316, critique = 3313, general = 3311;
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  CT_CHECK(ccord_session_select_guild(F.session, garden, &err) == CCORD_OK);
  frame(6);
  ccord_id self = ccord_store_self_id(F.store);

  // The channel exists, the user is a plain member of the guild, and the roles are exact: so it is really hidden.
  bool known = false;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, garden); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, garden, i);
    if (c && c->id == private_ch) known = true;
  }
  CT_CHECK(known);
  CT_CHECK(ccord_store_permissions_exact(F.store, garden, self));
  CT_CHECK((ccord_store_permissions(F.store, garden, private_ch, self, 0) & CCORD_PERM_VIEW_CHANNEL) == 0);
  CT_CHECK(channel_listed(general) && channel_listed(critique));   // read-only is not hidden
  CT_CHECK(!channel_listed(private_ch));
  size_t listed_before = F.ui->ncrow;

  CT_CHECK(hash_complete("#mod") == 0);
  CT_CHECK(hash_complete("#cri") == 1);                  // a channel you can see still completes
  CT_CHECK(switcher_hits("#mod-lounge") == 0);
  CT_CHECK(switcher_hits("#critique") == 1);

  // Unread counts of a channel you cannot see are not yours to see either.
  CT_CHECK(ccord_store_set_channel_counts(F.store, private_ch, 7, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, critique, 2, 1, &err) == CCORD_OK);
  uint32_t mentions = 99;
  CT_CHECK(ui_guild_unread(F.ui, garden, &mentions) == 2 && mentions == 1);

  // Becoming a moderator shows it, at once and without a request of any kind.
  const ccord_role *mod_role = role_named(garden, "Moderators");
  CT_CHECK(mod_role != nullptr);
  ccord_id moderators = mod_role ? mod_role->id : CCORD_ID_NONE;
  size_t calls_before = mock_requests(tr);
  CT_CHECK(ccord_store_apply_member_role(F.store, garden, self, moderators, true, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(channel_listed(private_ch) && F.ui->ncrow == listed_before + 1);
  CT_CHECK(mock_requests(tr) == calls_before);
  CT_CHECK(hash_complete("#mod") == 1);
  CT_CHECK(switcher_hits("#mod-lounge") == 1);
  CT_CHECK(ui_guild_unread(F.ui, garden, &mentions) == 9 && mentions == 4);

  // Looking at it keeps it listed when the role goes (the view must not pull the floor out from under you); leaving
  // it for another channel lets it go.
  CT_CHECK(ccord_session_select_channel(F.session, private_ch, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_store_apply_member_role(F.store, garden, self, moderators, false, &err) == CCORD_OK);
  frame(4);
  CT_CHECK((ccord_store_permissions(F.store, garden, private_ch, self, 0) & CCORD_PERM_VIEW_CHANNEL) == 0);
  CT_CHECK(channel_listed(private_ch));
  CT_CHECK(switcher_hits("#mod-lounge") == 1);   // the quick switcher agrees with the list (the composer is locked here)
  CT_CHECK(ccord_session_select_channel(F.session, general, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(!channel_listed(private_ch) && F.ui->ncrow == listed_before);
  CT_CHECK(hash_complete("#mod") == 0);

  // A category you cannot view stays while something inside it can be viewed (Discord lists it then).
  const ccord_id text_category = 3310;
  const ccord_overwrite deny_view = {.id = garden, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_VIEW_CHANNEL};
  CT_CHECK(ccord_store_set_channel_overwrites(F.store, text_category, garden, &deny_view, 1, &err) == CCORD_OK);
  frame(4);
  CT_CHECK((ccord_store_permissions(F.store, garden, text_category, self, 0) & CCORD_PERM_VIEW_CHANNEL) == 0);
  CT_CHECK(channel_listed(text_category) && channel_listed(general));

  // A whole category closed to the user disappears with everything in it, and so does a loose channel (no category).
  const ccord_id voice_category = 3320, voice_a = 3321, voice_b = 3322, stage_ch = 3323, loose = 3399;
  CT_CHECK(channel_listed(voice_category) && channel_listed(voice_a) && channel_listed(voice_b));
  const ccord_channel loose_channel = {.id = loose, .guild_id = garden, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT,
                                       .name = "loose", .topic = ""};
  CT_CHECK(ccord_store_upsert_channel(F.store, &loose_channel, &err) == CCORD_OK);
  const ccord_id closed[5] = {voice_category, voice_a, voice_b, stage_ch, loose};
  for (size_t i = 0; i < 5; i++) {
    CT_CHECK(ccord_store_set_channel_overwrites(F.store, closed[i], garden, &deny_view, 1, &err) == CCORD_OK);
  }
  frame(4);
  CT_CHECK(!channel_listed(voice_category) && !channel_listed(voice_a) && !channel_listed(voice_b));
  CT_CHECK(!channel_listed(loose));
  CT_CHECK(channel_listed(text_category) && channel_listed(general));

  // Without exact roles nothing is hidden: Discord decides, and we do not guess someone out of a channel.
  ccord_store_forget_member_roles(F.store, garden, self);
  frame(4);
  CT_CHECK(!ccord_store_permissions_exact(F.store, garden, self));
  CT_CHECK(channel_listed(private_ch) && channel_listed(voice_category) && channel_listed(voice_a));
  CT_CHECK(channel_listed(loose));
  CT_CHECK(hash_complete("#mod") == 1);

  // Administrators see everything, owners too: the other guilds list all their channels.
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(6);
  CT_CHECK(channel_listed(3112) && channel_listed(3111));

  // A guild whose roles we have never heard of: every channel counts as viewable (Discord will tell us otherwise).
  const ccord_guild unknown_guild = {.id = 778000u, .name = "Unknown Roles"};
  const ccord_channel unknown_chan[1] = {
    {.id = 778001u, .guild_id = 778000u, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT, .name = "somewhere", .topic = ""},
  };
  CT_CHECK(ccord_store_upsert_guild(F.store, &unknown_guild, unknown_chan, 1, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_permissions_exact(F.store, 778000u, self));
  CT_CHECK(ui_channel_visible(F.ui, 778000u, &unknown_chan[0]));
}

// Selecting text highlights it the way a browser does (and so Discord): every line is one unbroken band - the blanks
// between words included, not word-sized islands - the line break at the end of a line the selection runs past is
// highlighted as a short stub, and copying gives a space across a soft wrap but a line break across a real one.
static void test_selection_is_one_band(void)
{
  const ccord_id g = 778200u, chn = 778201u, ada = 1002u;
  ccord_error err;
  const ccord_guild guild = {.id = g, .name = "Band"};
  const ccord_channel chans[1] = {
    {.id = chn, .guild_id = g, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT, .name = "band", .topic = ""},
  };
  CT_CHECK(ccord_store_upsert_guild(F.store, &guild, chans, 1, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  const char *para = "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen "
                     "seventeen eighteen nineteen twenty twentyone twentytwo twentythree twentyfour twentyfive twentysix";
  char first[512];
  snprintf(first, sizeof first, "%s\ntail line", para);
  const ccord_message msgs[2] = {
    {.id = 9000001u, .channel_id = chn, .author_id = ccord_store_self_id(F.store), .timestamp_ms = 1700000000000,
     .content = first},
    {.id = 9000002u, .channel_id = chn, .author_id = ada, .timestamp_ms = 1700000060000,
     .content = "second message here"},
  };
  CT_CHECK(ccord_store_set_history(F.store, chn, msgs, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_guild(F.store, g, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, chn, &err) == CCORD_OK);
  frame(4);

  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  F.clip[0] = '\0';
  mouse(CCORD_IN_MOUSE_DOWN, 1, l->message_area.x + 5.0f, l->message_area.y + 2.0f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, l->message_area.x + l->message_area.w - 20.0f, l->message_area.y + l->message_area.h - 2.0f);
  frame(2);
  mouse(CCORD_IN_MOUSE_UP, 1, l->message_area.x + l->message_area.w - 20.0f, l->message_area.y + l->message_area.h - 2.0f);
  frame(2);
  CT_CHECK(F.ui->chat_sel_active && F.ui->dbg_nsel > 10);

  // Per line (rectangles sharing a top), sorted left to right, each starts where the previous one ended.
  size_t lines = 0, widest = 0;
  bool used[128] = {false};
  for (size_t i = 0; i < F.ui->dbg_nsel; i++) {
    if (used[i]) continue;
    float y = F.ui->dbg_sel[i].y;
    ccord_rect row[128];
    size_t n = 0;
    for (size_t k = i; k < F.ui->dbg_nsel; k++) {
      if (fabsf(F.ui->dbg_sel[k].y - y) < 0.5f) { row[n++] = F.ui->dbg_sel[k]; used[k] = true; }
    }
    for (size_t a = 1; a < n; a++) {   // insertion sort by x
      ccord_rect t = row[a];
      size_t b = a;
      while (b > 0 && row[b - 1].x > t.x) { row[b] = row[b - 1]; b--; }
      row[b] = t;
    }
    for (size_t a = 1; a < n; a++) CT_CHECK(fabsf(row[a].x - (row[a - 1].x + row[a - 1].w)) < 0.6f);   // no holes
    CT_CHECK(row[n - 1].w < 14.0f);   // the selection runs on past every line, so each ends in the line-break stub
    if (n > widest) widest = n;
    lines++;
  }
  CT_CHECK(lines >= 4);      // two or more lines of the paragraph, "tail line", the second message
  CT_CHECK(widest >= 12);    // words and the blanks between them, as separate pieces of one band

  shot("selection-band");

  // What gets copied: soft wraps are spaces, real line ends (also between messages) are line breaks.
  key(CCORD_KEY_C, CCORD_MOD_CTRL);
  const char *from = strstr(F.clip, "one two three");
  const char *to = strstr(F.clip, "twentysix");
  CT_CHECK(from && to && to > from && memchr(from, '\n', (size_t)(to - from)) == nullptr);
  CT_CHECK(strncmp(F.clip, "You ", 4) == 0 && strstr(F.clip, "\none two three") != nullptr);   // the header is selected too
  CT_CHECK(strstr(F.clip, "twentysix\ntail line\nAda ") != nullptr && strstr(F.clip, "\nsecond message here") != nullptr);
  F.ui->chat_sel_active = false;
}

// ---- transitions: state changes ease over a few frames instead of switching ----

// Where the transition (ns, id) is right now; -1 when nothing is tracking it.
static float tval(const char *ns, uint64_t id)
{
  uint32_t k = ccord_anim_key(ns, id);
  for (size_t i = 0; i < CCORD_ANIM_SLOTS; i++) {
    if (F.ui->anim.slots[i].key == k) return F.ui->anim.slots[i].value;
  }
  return -1.0f;
}

static bool tmid(float v)
{
  return v > 0.05f && v < 0.99f;
}

// Draws one more frame (50 ms on) and reads one pixel of it.
static int grab_luma(int x, int y)
{
  F.now += 50;
  (void)ccord_ui_update(F.ui, F.now);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_error err;
  bool ok = ccord_render_screenshot(F.renderer, "ui_trans_shot.png", &err) == CCORD_OK;
  ccord_render_end(F.renderer);
  SDL_Surface *surf = ok ? SDL_LoadPNG("ui_trans_shot.png") : nullptr;
  uint8_t r = 0, g = 0, b = 0, a = 0;
  if (surf) SDL_ReadSurfacePixel(surf, x, y, &r, &g, &b, &a);
  CT_CHECK(surf != nullptr);
  if (surf) SDL_DestroySurface(surf);
  remove("ui_trans_shot.png");
  return (int)r + (int)g + (int)b;
}

static float l0_msg_x(void)
{
  return ccord_ui_get_layout(F.ui)->message_area.x + 300.0f;
}

static void test_transitions(void)
{
  // Make sure nothing is left open by an earlier test, and let everything settle.
  ui_server_settings_close(F.ui);
  frame(30);
  CT_CHECK(!F.ui->settings_open && !F.ui->menu.open && F.ui->modal_kind == UI_MODAL_NONE && !F.ui->quick_switcher_open);
  ccord_rect first_guild = ui_guild_icon_rect(F.ui, 1);   // an earlier test left a one-channel guild selected
  click(first_guild.x + first_guild.w * 0.5f, first_guild.y + first_guild.h * 0.5f);
  frame(30);

  // 1. Hovering a channel row fades its background in over ~110 ms and out again when the pointer leaves.
  ccord_id guild = ccord_store_selected_guild(F.store);
  ccord_id sel = ccord_store_selected_channel(F.store);
  ccord_id other = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, guild) && other == CCORD_ID_NONE; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, guild, i);
    if (c->kind == CCORD_CHANNEL_TEXT && c->id != sel && ui_channel_visible(F.ui, guild, c)) other = c->id;
  }
  CT_CHECK(other != CCORD_ID_NONE);
  float y = scan_hover_channel(200.0f, other);
  CT_CHECK(y > 0);
  frame(1);
  CT_CHECK(tval("chan", other * 2u) < 0.05f);                 // the first frame under the pointer has only begun
  frame(1);
  CT_CHECK(tmid(tval("chan", other * 2u)));                   // 50 ms in: on its way
  CT_CHECK(ccord_anim_active(&F.ui->anim));                   // and the client knows it needs more frames
  frame(6);
  CT_CHECK(tval("chan", other * 2u) == 1.0f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, 640.0f, 400.0f);
  frame(2);
  CT_CHECK(tmid(tval("chan", other * 2u)));                   // fading back out, not snapping
  frame(6);
  CT_CHECK(tval("chan", other * 2u) == 0.0f);
  frame(1);
  CT_CHECK(!ccord_anim_active(&F.ui->anim) && !F.ui->animating);   // idle again: no frames are being asked for

  // The same for a message row: its wash (and the action bar that rides on it) follows the pointer.
  float msg_y = scan_hover_message(l0_msg_x());
  CT_CHECK(msg_y > 0);
  ccord_id hov_msg = dbg().hover_message;
  frame(2);
  CT_CHECK(tmid(tval("msg", hov_msg)));
  frame(6);
  CT_CHECK(tval("msg", hov_msg) == 1.0f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, 640.0f, 20.0f);
  frame(8);
  CT_CHECK(tval("msg", hov_msg) == 0.0f);

  // 2. A context menu eases in, stays put while you use it, and starts over when opened again.
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float x = l->message_area.x + 300.0f;
  float my = scan_hover_self_message(x);
  CT_CHECK(my > 0);
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, my);
  frame(1);
  CT_CHECK(F.ui->menu.open);
  uint64_t seq1 = F.ui->menu.seq;
  CT_CHECK(tval("menu", seq1) < 0.05f);
  frame(1);
  CT_CHECK(tmid(tval("menu", seq1)));
  frame(6);
  CT_CHECK(tval("menu", seq1) == 1.0f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, x + 20.0f, my + 30.0f);       // using the menu does not replay the entrance
  frame(3);
  CT_CHECK(tval("menu", seq1) == 1.0f);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, my);
  frame(1);
  CT_CHECK(F.ui->menu.open && F.ui->menu.seq != seq1);
  CT_CHECK(tval("menu", F.ui->menu.seq) < 0.05f);             // a new menu, a new entrance
  key(CCORD_KEY_ESCAPE, 0);
  frame(8);

  // 3. The quick switcher (a full-window overlay) fades in as well.
  key(CCORD_KEY_K, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(F.ui->quick_switcher_open);
  CT_CHECK(tmid(tval("qswitch", 0)));
  frame(6);
  CT_CHECK(tval("qswitch", 0) == 1.0f);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  key(CCORD_KEY_K, CCORD_MOD_CTRL);
  frame(2);
  CT_CHECK(tmid(tval("qswitch", 0)));                         // closing and reopening replays it
  key(CCORD_KEY_ESCAPE, 0);
  frame(8);

  // A popover plays its entrance each time it opens, not only the first time.
  F.ui->search_open = true;
  F.ui->dirty = true;
  frame(2);
  CT_CHECK(tmid(tval("search", 0)));
  frame(6);
  CT_CHECK(tval("search", 0) == 1.0f);
  F.ui->search_open = false;
  F.ui->dirty = true;
  frame(2);
  F.ui->search_open = true;
  F.ui->dirty = true;
  frame(2);
  CT_CHECK(tmid(tval("search", 0)));
  F.ui->search_open = false;
  F.ui->dirty = true;
  frame(4);

  // What the eye sees: the scrim over the channel list darkens a little more on every frame, from nothing to full.
  frame(8);
  int before = grab_luma(150, 600);
  key(CCORD_KEY_K, CCORD_MOD_CTRL);
  int f1 = grab_luma(150, 600), f2 = grab_luma(150, 600), f3 = grab_luma(150, 600);
  frame(8);
  int fin = grab_luma(150, 600);
  CT_CHECK(abs(f1 - before) <= 3);                            // the first frame has not started to show it
  CT_CHECK(f1 > f2 + 3 && f2 > f3 + 3 && f3 > fin + 3);      // then it darkens step by step
  CT_CHECK(fin < before - 30);
  key(CCORD_KEY_ESCAPE, 0);
  frame(8);

  // 4. Settings: the overlay fades in, a page eases in when chosen, switches slide instead of jumping.
  click(l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
  ccord_ui_settings_debug sd;
  ccord_ui_get_settings_debug(F.ui, &sd);
  CT_CHECK(sd.open);
  CT_CHECK(tmid(tval("settings", 0)));
  frame(8);
  CT_CHECK(tval("settings", 0) == 1.0f);
  click(sd.tabs[2].x + 10.0f, sd.tabs[2].y + 10.0f);           // Appearance
  ccord_ui_get_settings_debug(F.ui, &sd);
  CT_CHECK(sd.page == 2 && !sd.reduced_motion);
  CT_CHECK(tmid(tval("spagebody", 2)));                       // the page eases in
  frame(8);
  CT_CHECK(tval("spagebody", 2) == 1.0f);
  uint64_t knob = (2u * 64u + 7u) * 2u;                       // the Reduced motion switch of this page
  CT_CHECK(tval("swset", knob) == 0.0f);
  click(sd.reduced_motion_toggle.x + sd.reduced_motion_toggle.w - 10.0f,
        sd.reduced_motion_toggle.y + sd.reduced_motion_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &sd);
  CT_CHECK(sd.reduced_motion && F.ui->anim.reduced_motion);   // the setting reaches the animation clock
  CT_CHECK(tval("swset", knob) == 1.0f);                      // and with it on, even this switch moved at once
  click(sd.tabs[3].x + 10.0f, sd.tabs[3].y + 10.0f);           // Voice & Video: no easing under reduced motion
  CT_CHECK(tval("spagebody", 3) == 1.0f);
  click(sd.tabs[2].x + 10.0f, sd.tabs[2].y + 10.0f);
  click(sd.reduced_motion_toggle.x + sd.reduced_motion_toggle.w - 10.0f,
        sd.reduced_motion_toggle.y + sd.reduced_motion_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &sd);
  CT_CHECK(!sd.reduced_motion && !F.ui->anim.reduced_motion);
  CT_CHECK(tmid(tval("swset", knob)) || tval("swset", knob) == 0.0f);   // back to easing: it is travelling home
  key(CCORD_KEY_ESCAPE, 0);
  frame(12);
  CT_CHECK(!F.ui->settings_open);

  // 5. Dialogs and toasts.
  ui_confirm_open(F.ui, CCORD_ADMIN_DELETE_ROLE, guild, 1u, "Test");
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_CONFIRM);
  CT_CHECK(tmid(tval("modal", (uint64_t)UI_MODAL_CONFIRM)));
  frame(8);
  CT_CHECK(tval("modal", (uint64_t)UI_MODAL_CONFIRM) == 1.0f);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);
  ui_notice(F.ui, "Saved");
  float toast_a, toast_slide;
  ui_toast_look(F.ui, &toast_a, &toast_slide);
  CT_CHECK(toast_a < 0.05f && toast_slide > 9.0f);            // a toast starts transparent and 10 px low
  frame(2);
  CT_CHECK(tmid(tval("toast", F.ui->notice_seq)));
  frame(6);
  ui_toast_look(F.ui, &toast_a, &toast_slide);
  CT_CHECK(toast_a == 1.0f && toast_slide == 0.0f);           // then sits still
  while (F.ui->notice[0] && F.ui->notice_until_ms > F.now + 150) frame(1);
  ui_toast_look(F.ui, &toast_a, &toast_slide);
  CT_CHECK(tmid(toast_a) && toast_slide == 0.0f);             // and fades out over its last 250 ms
  uint64_t toast1 = F.ui->notice_seq;
  F.now += 5000;
  frame(4);
  ui_notice(F.ui, "Saved again");
  frame(1);
  CT_CHECK(F.ui->notice_seq != toast1 && tval("toast", F.ui->notice_seq) < 0.05f);   // a second toast slides in afresh
  F.now += 5000;
  frame(8);

  // 6. Someone typing: a pill with moving dots, which keeps the frames coming until they stop.
  CT_CHECK(ccord_store_set_typing(F.store, ccord_store_selected_channel(F.store), 1002u, (int64_t)F.now + 3000, &(ccord_error){0}) == CCORD_OK);
  frame(2);
  CT_CHECK(tmid(tval("typing", 0)));
  frame(8);
  CT_CHECK(tval("typing", 0) == 1.0f);
  CT_CHECK(ccord_anim_active(&F.ui->anim) && F.ui->animating);   // the dots are still going
  frame(70);                                                      // the notice expires
  CT_CHECK(!ccord_anim_active(&F.ui->anim) && !F.ui->animating);

  // 7. The scrollbar fades in with scrolling and fades out when it has been still for a while.
  mouse(CCORD_IN_MOUSE_MOVE, 0, l->chat_header.x + 10.0f, l->chat_header.y + 10.0f);
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 3.0f, .x = l->chat_header.x + 10.0f, .y = l->chat_header.y + 10.0f});
  frame(1);
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 3.0f, .x = l->message_area.x + 100, .y = l->message_area.y + 100});
  mouse(CCORD_IN_MOUSE_MOVE, 0, l->chat_header.x + 10.0f, l->chat_header.y + 10.0f);
  frame(3);
  CT_CHECK(tmid(tval("vscroll", 0)) || tval("vscroll", 0) == 1.0f);
  frame(4);
  CT_CHECK(tval("vscroll", 0) == 1.0f && F.ui->thumb_h > 0.0f);
  while (F.now < F.ui->last_scroll_ms + 1200) frame(1);          // the moment it is no longer wanted...
  CT_CHECK(tval("vscroll", 0) > 0.5f && F.ui->thumb_h == 0.0f);   // ...it is still fading out, but can no longer be grabbed
  frame(30);                                                      // 1.5 s of stillness
  CT_CHECK(tval("vscroll", 0) < 0.01f && F.ui->thumb_h == 0.0f);
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100, .y = l->message_area.y + 100});
  frame(40);

  // 8. Server Settings: the overlay fades in, pages ease in, and the unsaved-changes bar rises when something is edited.
  ui_server_settings_open(F.ui, guild);
  frame(2);
  CT_CHECK(ui_server_settings_is_open(F.ui));
  CT_CHECK(tmid(tval("ssettings", 0)));
  frame(8);
  CT_CHECK(tval("ssettings", 0) == 1.0f);
  CT_CHECK(tval("sspagebody", 0) == 1.0f);                   // the Overview page
  CT_CHECK(ss_click("name") && ssd().focus == 1);
  type_text("x");
  frame(2);
  CT_CHECK(ssd().unsaved && tmid(tval("ssbar", 0)));              // the unsaved-changes bar rises into place
  frame(8);
  CT_CHECK(tval("ssbar", 0) == 1.0f);
  CT_CHECK(ss_click("reset"));
  frame(2);
  CT_CHECK(!ssd().unsaved);
  ui_server_settings_close(F.ui);
  frame(8);

  // 9. When nothing moves the client asks for no frames.
  frame(40);
  CT_CHECK(!ccord_anim_active(&F.ui->anim) && !F.ui->animating);
}

// ---- notification settings: Mute / Notification Settings in the menus, and what a mute does to the lists ----





static void test_notification_settings_ui(void)
{
  ccord_error err;
  ccord_mock_config mc = {.big_channel_messages = 200, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  frame(3);
  ccord_id dev = find_channel(g, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(dev != CCORD_ID_NONE);
  size_t nopts = 0;
  const ccord_mute_option *opts = ccord_notify_mute_options(&nopts);
  CT_CHECK(nopts == 6);
  CT_CHECK(ccord_mock_admin_count(tr) == 0);
  clear_counts(g);
  frame(2);

  // 1. Right-click a channel: Mute Channel is a parent; pointing at it opens a fly-out of the durations.
  float dev_y = scan_hover_channel(200.0f, dev);
  CT_CHECK(dev_y > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dev_y);
  frame(1);
  int mute = menu_index("Mute Channel");
  int notif = menu_index("Notification Settings");
  CT_CHECK(dbg().context_menu_open && mute == 0 && notif == 7);   // first: Mute Channel (+6 children), then Notification Settings
  CT_CHECK(F.ui->menu.items[mute].has_sub && F.ui->menu.open_sub == -1 && dbg().context_menu_sub_items == 0);
  menu_point_at(mute);
  CT_CHECK(F.ui->menu.open_sub == mute && dbg().context_menu_sub_items == 6);
  float t = tval("menusub", F.ui->menu.seq * 64u + (uint64_t)mute);
  CT_CHECK(t >= 0.0f && t < 0.99f);                      // the fly-out eases in
  frame(10);
  CT_CHECK(tval("menusub", F.ui->menu.seq * 64u + (uint64_t)mute) > 0.99f);
  for (size_t i = 0; i < nopts; i++) CT_CHECK(menu_index(opts[i].label) >= 0);

  // The pointer can cross to the fly-out; pointing at another top-level item closes it; clicking a parent keeps the menu.
  int child = menu_index(opts[1].label);
  menu_point_at(child);
  CT_CHECK(F.ui->menu.open_sub == mute && F.ui->menu.hover == child && dbg().context_menu_open);
  menu_point_at(notif);
  CT_CHECK(F.ui->menu.open_sub == notif && dbg().context_menu_sub_items == 4);   // server default, all, mentions, nothing
  menu_point_at(menu_index("Edit Channel"));
  CT_CHECK(F.ui->menu.open_sub == -1 && dbg().context_menu_sub_items == 0);
  ccord_rect pr = ui_menu_item_rect(F.ui, (size_t)mute);
  mouse(CCORD_IN_MOUSE_DOWN, 1, pr.x + 20.0f, pr.y + pr.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().context_menu_open && F.ui->menu.open_sub == mute);   // a click on a parent opens it, nothing is sent
  CT_CHECK(ccord_mock_admin_count(tr) == 0);

  // 2. Choosing a duration asks Discord; the store changes only when Discord answers.
  {
    ccord_rect cr = ui_menu_item_rect(F.ui, (size_t)menu_index(opts[1].label));
    mouse(CCORD_IN_MOUSE_MOVE, 0, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f);
    frame(1);
    mouse(CCORD_IN_MOUSE_DOWN, 1, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f);   // no frame yet: the request is out, nothing answered
  }
  CT_CHECK(!dbg().context_menu_open);
  CT_CHECK(ccord_mock_admin_count(tr) == 1);
  const ccord_admin_request *last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_SET_NOTIFY && last->guild_id == g && last->channel_id == dev && last->notify_muted == 1);
  CT_CHECK(last && last->until_ms == F.ui->now_unix_ms + opts[1].duration_ms && strstr(last->body, "\"muted\":true") != nullptr);
  CT_CHECK(!ccord_store_channel_muted(F.store, dev, F.ui->now_unix_ms));
  frame(2);
  CT_CHECK(ccord_store_channel_muted(F.store, dev, F.ui->now_unix_ms));

  // 3. A muted channel has no unread marker (its counts keep running), and a mention still counts on the server.
  CT_CHECK(ccord_store_set_channel_counts(F.store, dev, 4, 0, &err) == CCORD_OK);
  frame(8);
  uint32_t mentions = 99;
  CT_CHECK(ui_guild_unread(F.ui, g, &mentions) == 0 && mentions == 0);
  CT_CHECK(tval("unread", dev) < 0.01f);
  CT_CHECK(ccord_store_set_channel_counts(F.store, dev, 4, 2, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ui_guild_unread(F.ui, g, &mentions) == 0 && mentions == 2);
  CT_CHECK(ui_channel_muted(F.ui, ccord_store_channel(F.store, dev)));

  // 4. The menu now offers Unmute, with the time left; choosing it lifts the mute once Discord says so.
  dev_y = scan_hover_channel(200.0f, dev);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dev_y);
  frame(1);
  CT_CHECK(menu_index("Mute Channel") < 0);
  int unmute = menu_index("Unmute Channel (");
  CT_CHECK(unmute == 0 && strstr(F.ui->menu.items[unmute].label, "left") != nullptr);
  {
    ccord_rect cr = ui_menu_item_rect(F.ui, (size_t)unmute);
    mouse(CCORD_IN_MOUSE_MOVE, 0, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f);
    frame(1);
    mouse(CCORD_IN_MOUSE_DOWN, 1, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f);
  }
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 2 && last && last->notify_muted == 0 && strstr(last->body, "\"mute_config\":null") != nullptr);
  CT_CHECK(ccord_store_channel_muted(F.store, dev, F.ui->now_unix_ms));   // not yet: Discord has not answered
  frame(10);
  CT_CHECK(!ccord_store_channel_muted(F.store, dev, F.ui->now_unix_ms));
  CT_CHECK(ui_guild_unread(F.ui, g, &mentions) == 4 && mentions == 2);
  CT_CHECK(tval("unread", dev) > 0.9f);                                     // the marker grows back

  // 5. Notification Settings: the choices, the current one marked; picking one asks Discord.
  dev_y = scan_hover_channel(200.0f, dev);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dev_y);
  frame(1);
  notif = menu_index("Notification Settings");
  menu_point_at(notif);
  const ccord_channel *dev_ch = ccord_store_channel(F.store, dev);
  const ccord_channel *dev_parent = dev_ch && dev_ch->parent_id ? ccord_store_channel(F.store, dev_ch->parent_id) : nullptr;
  CT_CHECK(dev_parent && dev_parent->kind == CCORD_CHANNEL_CATEGORY);
  int use_default = menu_index("Use Category Default"), only = menu_index("Only @mentions");   // a channel in a category defers to it
  CT_CHECK(menu_index("Use Server Default") < 0);
  CT_CHECK(use_default >= 0 && F.ui->menu.items[use_default].checked && only >= 0 && !F.ui->menu.items[only].checked);
  CT_CHECK(menu_index("Suppress @everyone") < 0);   // those belong to the server menu
  menu_click_index(only);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 3 && last && last->notify_level == (int8_t)CCORD_NOTIFY_MENTIONS && last->channel_id == dev);
  frame(4);
  ccord_notify_scope row;
  CT_CHECK(ccord_store_notify_scope(F.store, g, dev, &row) && row.level == CCORD_NOTIFY_MENTIONS && !row.muted);
  dev_y = scan_hover_channel(200.0f, dev);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dev_y);
  frame(1);
  menu_point_at(menu_index("Notification Settings"));
  CT_CHECK(F.ui->menu.items[menu_index("Only @mentions")].checked && !F.ui->menu.items[menu_index("Use Category Default")].checked);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);

  // 6. The server menu: Mute Server mutes everything in it; its levels and the two @ switches live under Notification Settings.
  ccord_rect gr = ui_guild_icon_rect(F.ui, 1);
  float gx = gr.x + gr.w * 0.5f, gy = gr.y + gr.h * 0.5f;
  mouse(CCORD_IN_MOUSE_DOWN, 3, gx, gy);
  frame(1);
  CT_CHECK(dbg().context_menu_open && menu_index("Mute Server") == 0);
  menu_point_at(0);
  CT_CHECK(dbg().context_menu_sub_items == 6);
  menu_click_index(menu_index(opts[5].label));       // until I turn it back on
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 4 && last && last->op == CCORD_ADMIN_SET_NOTIFY && last->channel_id == 0 && last->until_ms == 0);
  CT_CHECK(strstr(last->body, "\"end_time\":null") != nullptr);
  frame(4);
  CT_CHECK(ccord_store_guild_muted(F.store, g, F.ui->now_unix_ms));
  CT_CHECK(ui_guild_unread(F.ui, g, &mentions) == 0 && mentions == 2);   // nothing unread in a muted server; the mention stays
  CT_CHECK(ccord_store_channel_muted(F.store, dev, F.ui->now_unix_ms));
  mouse(CCORD_IN_MOUSE_DOWN, 3, gx, gy);
  frame(1);
  CT_CHECK(menu_index("Mute Server") < 0 && menu_index("Unmute Server") == 0);
  CT_CHECK(strstr(F.ui->menu.items[0].label, "turn it back on") != nullptr);
  menu_point_at(menu_index("Notification Settings"));
  CT_CHECK(dbg().context_menu_sub_items == 5);   // all, mentions, nothing + the two switches (no "use default": nothing is above a server)
  int se = menu_index("Suppress @everyone");
  CT_CHECK(se >= 0 && !F.ui->menu.items[se].checked);
  menu_click_index(se);
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 5 && last && strstr(last->body, "\"suppress_everyone\":true") != nullptr);
  frame(4);
  CT_CHECK(ccord_store_notify_scope(F.store, g, 0, &row) && row.suppress_everyone && row.muted);
  mouse(CCORD_IN_MOUSE_DOWN, 3, gx, gy);
  frame(1);
  menu_point_at(menu_index("Notification Settings"));
  se = menu_index("Suppress @everyone");
  CT_CHECK(F.ui->menu.items[se].checked && F.ui->menu.items[se].arg == 0);   // clicking it again would switch it off
  menu_click_index(menu_index("Unmute Server"));
  frame(4);
  CT_CHECK(!ccord_store_guild_muted(F.store, g, F.ui->now_unix_ms));
  CT_CHECK(ui_guild_unread(F.ui, g, &mentions) == 4);

  // 6b. A menu opened on a server that is not the one on screen acts on that server, not on the selected one.
  CT_CHECK(ccord_store_guild_count(F.store) >= 2);
  ccord_id g2 = ccord_store_guild_at(F.store, 1)->id;
  CT_CHECK(g2 != g && ccord_store_selected_guild(F.store) == g);
  ccord_rect gr2 = ui_guild_icon_rect(F.ui, 2);
  mouse(CCORD_IN_MOUSE_DOWN, 3, gr2.x + gr2.w * 0.5f, gr2.y + gr2.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().context_menu_open && menu_index("Mute Server") == 0);
  menu_point_at(0);
  menu_click_index(menu_index(opts[1].label));
  last = ccord_mock_last_admin(tr);
  CT_CHECK(ccord_mock_admin_count(tr) == 7 && last && last->guild_id == g2 && last->channel_id == 0);
  frame(4);
  CT_CHECK(ccord_store_guild_muted(F.store, g2, F.ui->now_unix_ms) && !ccord_store_guild_muted(F.store, g, F.ui->now_unix_ms));
  CT_CHECK(ccord_store_selected_guild(F.store) == g);

  // 7. A fly-out near the right edge opens on the left of the menu instead of off the window.
  ui_menu_open(F.ui, F.ui->win_w - 90.0f, 300.0f, &(ccord_menu_context){CCORD_MENU_GUILD, g, 0, 0, 0, nullptr, nullptr});
  frame(1);
  menu_point_at(menu_index("Mute Server"));
  ccord_rect first_child = ui_menu_item_rect(F.ui, (size_t)menu_index(opts[0].label));
  CT_CHECK(first_child.x + first_child.w <= F.ui->menu.x + 1.0f && first_child.x >= 0.0f);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);

  // 8. Direct messages can be muted but have no levels to pick; a failure from Discord is reported in plain words.
  ccord_id friend_id = other_member(g, 0);
  CT_CHECK(ccord_session_open_dm(F.session, friend_id, &err) == CCORD_OK);
  frame(4);
  const ccord_channel *dm = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, 0);
  CT_CHECK(dm != nullptr);
  if (dm) {
    ui_menu_open(F.ui, 400.0f, 300.0f, &(ccord_menu_context){CCORD_MENU_CHANNEL, CCORD_DM_GUILD_ID, dm->id, 0, 0, nullptr, nullptr});
    frame(1);
    int mute_idx = menu_index("Mute @");
    if (mute_idx < 0) mute_idx = menu_index("Mute Conversation");
    CT_CHECK(dbg().context_menu_open && mute_idx >= 0 && menu_index("Notification Settings") < 0);
    menu_click_index(mute_idx);   // clicking the parent only opens its fly-out
    CT_CHECK(dbg().context_menu_open && dbg().context_menu_sub_items == 6);
    menu_click_index(menu_index(opts[0].label));
    frame(4);
    CT_CHECK(ccord_store_channel_muted(F.store, dm->id, F.ui->now_unix_ms));
    CT_CHECK(ccord_store_notify_scope(F.store, CCORD_DM_GUILD_ID, dm->id, &row) && row.muted);
  }
  ccord_admin_notify_edit bad = CCORD_ADMIN_NOTIFY_EDIT_INIT;
  bad.channel_id = 987654321;
  bad.muted = 1;
  ccord_admin_request req;
  CT_CHECK(ccord_admin_set_notify(&req, g, &bad, F.ui->now_unix_ms, &err) == CCORD_OK);
  ui_admin_submit(F.ui, &req);
  CT_CHECK(strstr(dbg().notice, "Couldn't change the notification settings") != nullptr);

  // 9. Idle again: the fly-out's animations have settled, so the client asks for no more frames.
  frame(20);
  CT_CHECK(!ccord_anim_active(&F.ui->anim));
}

// "Mark As Read" leads the menu of a channel or a server that has something unread, sends one acknowledgement per
// channel, and clears the counts; with nothing unread it is not offered.

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_interaction_chat")) {
    fprintf(stderr, "test_ui_interaction_chat: setup failed or no display driver; skipping\n");
    return 77;
  }
  frame(4);
  test_expression_picker_and_stickers();
  test_forum_channel_ui_flow();
  test_picker_over_forum_clicks();
  test_vanilla_discord_gap_features();
  test_smooth_scroll_hover_anim_loading_and_restore();
  test_refused_writes_return_the_text();
  test_m0_pending_failed_retry_ui();
  test_permission_gated_menus();
  test_composer_locks_where_sending_is_not_allowed();
  test_admin_dialogs_call_discord();
  test_server_settings_overlay();
  test_member_list_follows_roles();
  test_hidden_channels();
  test_selection_is_one_band();
  test_transitions();
  test_notification_settings_ui();
  ui_test_teardown();
  ct_failures += ui_test_harness_failures();
  return CT_RESULT("test_ui_interaction_chat");
}
