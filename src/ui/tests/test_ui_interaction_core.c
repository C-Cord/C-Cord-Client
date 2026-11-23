// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include "ui_interaction_harness.h"

static void test_markdown_tokens(void)
{
  const char *src = "a **bold** *ital* `code` @you [Nitro](https://discord.com/nitro) https://discord.com/shop";
  ui_md_iter it;
  ui_md_iter_init(&it, src, strlen(src));
  ui_md_token tok;
  bool bold = false, ital = false, code = false, mention = false, md_link = false, bare_link = false;
  while (ui_md_next(&it, &tok)) {
    CT_CHECK(memchr(tok.text, '*', tok.len) == nullptr && memchr(tok.text, '`', tok.len) == nullptr);
    if (tok.len == 4 && memcmp(tok.text, "bold", 4) == 0) {
      bold = true;
      CT_CHECK(tok.style == UI_MD_BOLD);
    } else if (tok.len == 4 && memcmp(tok.text, "ital", 4) == 0) {
      ital = true;
      CT_CHECK(tok.style == UI_MD_ITALIC);
    } else if (tok.len == 4 && memcmp(tok.text, "code", 4) == 0) {
      code = true;
      CT_CHECK(tok.style == UI_MD_CODE);
    } else if (tok.len == 4 && memcmp(tok.text, "@you", 4) == 0) {
      mention = true;
      CT_CHECK(tok.style == UI_MD_MENTION);
    } else if (tok.len == 5 && memcmp(tok.text, "Nitro", 5) == 0) {
      md_link = true;
      CT_CHECK(tok.style == UI_MD_LINK);
      CT_CHECK(tok.url_len == strlen("https://discord.com/nitro") &&
               memcmp(tok.url, "https://discord.com/nitro", tok.url_len) == 0);
    } else if (tok.style == UI_MD_LINK && tok.len == strlen("https://discord.com/shop") &&
               memcmp(tok.text, "https://discord.com/shop", tok.len) == 0) {
      bare_link = true;
      CT_CHECK(tok.url_len == tok.len && memcmp(tok.url, tok.text, tok.len) == 0);
    }
  }
  CT_CHECK(bold && ital && code && mention && md_link && bare_link);

  // Discord inline tokens: <@id>, <@!id>, <#id>, <@&id>, <:name:id>, <a:name:id>, <t:ts:R>, <url>, ***bi***, __u__, ~~s~~, ||sp||
  const char *dsrc =
    "<@1002> <@!1003> <#3111> <@&777> <:blob:12345> <a:party:67890> <t:1700000000:R> <https://example.org> ***bi*** __und__ ~~str~~ ||spoil||";
  ui_md_iter_init(&it, dsrc, strlen(dsrc));
  bool u1 = false, u2 = false, ch = false, role = false, ce1 = false, ce2 = false, ts = false, alink = false;
  bool bi = false, und = false, str = false, spoil = false;
  while (ui_md_next(&it, &tok)) {
    if (tok.style == UI_MD_USER_MENTION && tok.id == 1002) u1 = true;
    else if (tok.style == UI_MD_USER_MENTION && tok.id == 1003) u2 = true;
    else if (tok.style == UI_MD_CHANNEL_MENTION && tok.id == 3111) ch = true;
    else if (tok.style == UI_MD_ROLE_MENTION && tok.id == 777) role = true;
    else if (tok.style == UI_MD_CUSTOM_EMOJI && tok.id == 12345 && !tok.animated && tok.len == 4 && memcmp(tok.text, "blob", 4) == 0) ce1 = true;
    else if (tok.style == UI_MD_CUSTOM_EMOJI && tok.id == 67890 && tok.animated && tok.len == 5 && memcmp(tok.text, "party", 5) == 0) ce2 = true;
    else if (tok.style == UI_MD_TIMESTAMP && tok.timestamp_sec == 1700000000 && tok.timestamp_style == 'R') ts = true;
    else if (tok.style == UI_MD_LINK && tok.url_len == strlen("https://example.org")) alink = true;
    else if (tok.style == UI_MD_BOLD_ITALIC && tok.len == 2 && memcmp(tok.text, "bi", 2) == 0) bi = true;
    else if (tok.underline && tok.len == 3 && memcmp(tok.text, "und", 3) == 0) und = true;
    else if (tok.strikethrough && tok.len == 3 && memcmp(tok.text, "str", 3) == 0) str = true;
    else if (tok.spoiler && tok.len == 5 && memcmp(tok.text, "spoil", 5) == 0) spoil = true;
  }
  CT_CHECK(u1 && u2 && ch && role && ce1 && ce2 && ts && alink && bi && und && str && spoil);
}

// Collects the drawn text of one line (all tokens joined) and whether any token carries a style.
typedef struct md_probe {
  char text[256];
  size_t n;
  bool bold, italic, underline, strike, spoiler, code, link;
} md_probe;

static md_probe md_probe_line(const char *src)
{
  md_probe pr = {0};
  ui_md_iter it;
  ui_md_iter_init(&it, src, strlen(src));
  ui_md_token tok;
  while (ui_md_next(&it, &tok)) {
    if (tok.style == UI_MD_LINK) pr.link = true;
    if (tok.style == UI_MD_BOLD || tok.style == UI_MD_BOLD_ITALIC) pr.bold = true;
    if (tok.style == UI_MD_ITALIC || tok.style == UI_MD_BOLD_ITALIC) pr.italic = true;
    if (tok.style == UI_MD_CODE) pr.code = true;
    if (tok.underline) pr.underline = true;
    if (tok.strikethrough) pr.strike = true;
    if (tok.spoiler) pr.spoiler = true;
    if (pr.n + tok.len < sizeof pr.text) {
      memcpy(pr.text + pr.n, tok.text, tok.len);
      pr.n += tok.len;
      pr.text[pr.n] = '\0';
    }
  }
  return pr;
}

// Posted messages pair markers like the composer: no closer means literal text, a backslash escapes, _x_ is italic.
static void test_markdown_pairing(void)
{
  md_probe a = md_probe_line("**abc");
  CT_CHECK(strcmp(a.text, "**abc") == 0 && !a.bold && !a.italic);
  a = md_probe_line("*abc");
  CT_CHECK(strcmp(a.text, "*abc") == 0 && !a.italic);
  a = md_probe_line("x __abc ~~d ||e `f");
  CT_CHECK(strcmp(a.text, "x __abc ~~d ||e `f") == 0 && !a.underline && !a.strike && !a.spoiler && !a.code);
  a = md_probe_line("\\*abc\\*");
  CT_CHECK(strcmp(a.text, "*abc*") == 0 && !a.italic);
  a = md_probe_line("\\\\ \\*a*");
  CT_CHECK(strcmp(a.text, "\\ *a*") == 0);
  CT_CHECK(!a.italic);
  a = md_probe_line("_abc_");
  CT_CHECK(strcmp(a.text, "abc") == 0 && a.italic);
  a = md_probe_line("snake_case_name");
  CT_CHECK(strcmp(a.text, "snake_case_name") == 0 && !a.italic);
  a = md_probe_line("see https://x.com/_a_/b__c__d and <:a_b:1> ok");
  CT_CHECK(!a.italic && !a.underline && a.link);
  a = md_probe_line("`_a_` __init__");
  CT_CHECK(a.code && !a.italic && a.underline);
  a = md_probe_line("**a** *b* __c__ ~~d~~ `e` ||f||");
  CT_CHECK(a.bold && a.italic && a.underline && a.strike && a.code && a.spoiler);
  CT_CHECK(strcmp(a.text, "a b c d e f") == 0);
  a = md_probe_line("**bold** and *open");
  CT_CHECK(a.bold && !a.italic && strcmp(a.text, "bold and *open") == 0);
}

static void test_load_and_virtualization(void)
{
  frame(1);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.message_count == CCORD_SESSION_HISTORY_LIMIT); // history limit of the session, out of 3000 mock messages
  CT_CHECK(d.stick_bottom);
  CT_CHECK(d.last_visible == d.message_count - 1);
  size_t visible = d.last_visible - d.first_visible + 1;
  CT_CHECK(visible >= 5 && visible < 40);
  CT_CHECK(d.rows_drawn == visible);
  CT_CHECK(F.text_input_active);                         // composer is focused at start
  frame(2);
  d = dbg();
  CT_CHECK(d.rows_measured == 0);                        // steady state: height cache is warm
}

static void test_wheel_scroll(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ccord_ui_debug before = dbg();
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 5.0f, .x = l->message_area.x + 100, .y = l->message_area.y + 100});
  frame(1);
  ccord_ui_debug after = dbg();
  CT_CHECK(after.first_visible < before.first_visible);   // positive wheel = older messages
  CT_CHECK(!after.stick_bottom);
  CT_CHECK(after.rows_measured <= (after.last_visible - after.first_visible + 1) + 8 + 20);
  // wheel far up then back down re-sticks to the bottom
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100, .y = l->message_area.y + 100});
  frame(1);
  CT_CHECK(dbg().stick_bottom);
  // wheel outside any scrollable area does nothing
  ccord_ui_debug same = dbg();
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 3.0f, .x = l->chat_header.x + 10, .y = l->chat_header.y + 10});
  frame(1);
  CT_CHECK(dbg().first_visible == same.first_visible);
  // Scrolling to the top pages older history in through before=<id>.
  ccord_id ch = ccord_store_selected_channel(F.store);
  size_t before_count = ccord_store_message_count(F.store, ch);
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 2000.0f, .x = l->message_area.x + 100, .y = l->message_area.y + 100});
  frame(4);
  CT_CHECK(ccord_store_message_count(F.store, ch) > before_count);
}

static void test_channel_click(void)
{
  ccord_id guild = ccord_store_selected_guild(F.store);
  ccord_id general = ccord_store_selected_channel(F.store);
  ccord_id ann = find_channel(guild, "announcements", CCORD_CHANNEL_TEXT);
  CT_CHECK(ann != CCORD_ID_NONE && ann != general);
  float y = scan_hover_channel(200.0f, ann);
  CT_CHECK(y > 0);
  click(200.0f, y);
  CT_CHECK(ccord_store_selected_channel(F.store) == ann);
  frame(2);   // history arrives
  ccord_ui_debug d = dbg();
  CT_CHECK(d.message_count == ccord_store_message_count(F.store, ann));
  CT_CHECK(d.message_count > 0 && d.message_count <= CCORD_SESSION_HISTORY_LIMIT);
  CT_CHECK(d.stick_bottom && d.last_visible == d.message_count - 1);

  // voice channel: clicking joins voice and opens the stage without selecting a text channel
  ccord_id voice = find_channel(guild, "General", CCORD_CHANNEL_VOICE);
  CT_CHECK(voice != CCORD_ID_NONE);
  y = scan_hover_channel(200.0f, voice);
  CT_CHECK(y > 0);
  click(200.0f, y);
  frame(3);
  ccord_ui_debug vd = dbg();
  CT_CHECK(vd.voice_connected && vd.voice_channel == voice);
  CT_CHECK(vd.voice_stage_open);
  CT_CHECK(ccord_store_voice_count(F.store, voice) >= 2);   // seeded participant plus self
  CT_CHECK(ccord_store_selected_channel(F.store) == ann);
  // switch back to text: the stage closes, voice stays connected
  y = scan_hover_channel(200.0f, general);
  click(200.0f, y);
  CT_CHECK(!dbg().voice_stage_open);
  CT_CHECK(dbg().voice_connected);
  CT_CHECK(ccord_store_selected_channel(F.store) == general);
  frame(1);
  CT_CHECK(dbg().message_count == ccord_store_message_count(F.store, general));
}

static void test_server_click(void)
{
  ccord_id g1 = ccord_store_selected_guild(F.store);
  const ccord_guild *second = ccord_store_guild_at(F.store, 1);
  CT_CHECK(second && second->id != g1);
  ccord_rect dm_r = ui_guild_icon_rect(F.ui, 0);
  mouse(CCORD_IN_MOUSE_MOVE, 0, dm_r.x + dm_r.w * 0.5f, dm_r.y + dm_r.h * 0.5f);
  CT_CHECK(dbg().hover_guild == CCORD_DM_GUILD_ID);

  ccord_rect r2 = ui_guild_icon_rect(F.ui, 2);
  float x = r2.x + r2.w * 0.5f;
  float y = r2.y + r2.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  CT_CHECK(dbg().hover_guild == second->id);
  click(x, y);
  CT_CHECK(ccord_store_selected_guild(F.store) == second->id);
  frame(2);
  CT_CHECK(ccord_store_selected_channel(F.store) != CCORD_ID_NONE);
  CT_CHECK(dbg().message_count == ccord_store_message_count(F.store, ccord_store_selected_channel(F.store)));
  // back to the first guild (slot 1)
  ccord_rect r1 = ui_guild_icon_rect(F.ui, 1);
  click(r1.x + r1.w * 0.5f, r1.y + r1.h * 0.5f);
  CT_CHECK(ccord_store_selected_guild(F.store) == g1);
  frame(1);
}

static void test_dm_pseudo_guild(void)
{
  ccord_error err;
  ccord_user ada = {.id = 424242u, .name = "Ada", .avatar_url = "fe3ffc01cc5d93689dbd3f09462374e7", .presence = CCORD_PRESENCE_ONLINE};
  CT_CHECK(ccord_store_upsert_user(F.store, &ada, &err) == CCORD_OK);
  ccord_channel dm = {.id = 987654321u, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ada", .topic = "", .user_id = ada.id};
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm, &err) == CCORD_OK);
  frame(1);
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ccord_rect dm_r = ui_guild_icon_rect(F.ui, 0);
  float x = dm_r.x + dm_r.w * 0.5f;
  float y = dm_r.y + dm_r.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  CT_CHECK(dbg().hover_guild == CCORD_DM_GUILD_ID);
  click(x, y);
  CT_CHECK(ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID);
  CT_CHECK(ccord_store_selected_channel(F.store) == dm.id);
  CT_CHECK(dbg().relationship_rows >= 5);

  // Hover and click a relationship row in the DM channel column to open a DM conversation.
  float rx = l->channel_scroll.x + 80.0f;
  float ry = -1.0f;
  ccord_id rel_uid = CCORD_ID_NONE;
  for (float sy = l->channel_scroll.y + 40.0f; sy < l->channel_scroll.y + l->channel_scroll.h; sy += 3.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, rx, sy);
    if (dbg().hover_user != CCORD_ID_NONE) {
      ry = sy;
      rel_uid = dbg().hover_user;
      break;
    }
  }
  CT_CHECK(ry > 0.0f && rel_uid != CCORD_ID_NONE);
  // Right-click opens the DM relationship context menu (Message + relationship action).
  mouse(CCORD_IN_MOUSE_DOWN, 3, rx, ry);
  frame(1);
  CT_CHECK(dbg().context_menu_open && dbg().context_menu_items >= 2);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  // Left-click opens/selects the DM channel for that friend.
  click(rx, ry);
  frame(3);
  ccord_id opened_dm = 900000u + rel_uid;
  CT_CHECK(ccord_store_channel(F.store, opened_dm) != nullptr);
  CT_CHECK(ccord_store_selected_channel(F.store) == opened_dm);

  ccord_id g1 = ccord_store_guild_at(F.store, 0)->id;
  CT_CHECK(ccord_store_select_guild(F.store, g1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, dm.id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, opened_dm, &err) == CCORD_OK);
  frame(1);
}

static void save_stream_screenshot(const char *filename)
{
  const char *dir = getenv("CCORD_STREAM_SCREENSHOT_DIR");
  if (!dir || !dir[0]) return;
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dir, filename);
  ccord_error err = {0};
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  (void)ccord_render_screenshot(F.renderer, path, &err);
  ccord_render_end(F.renderer);
}

static void test_voice_and_stream(void)
{
  ccord_id guild = ccord_store_selected_guild(F.store);
  ccord_id voice = find_channel(guild, "General", CCORD_CHANNEL_VOICE);
  CT_CHECK(voice != CCORD_ID_NONE);
  // Joining from the channel list happened in test_channel_click; rejoin if a reset happened.
  ccord_ui_debug d = dbg();
  if (!d.voice_connected) {
    float y = scan_hover_channel(200.0f, voice);
    CT_CHECK(y > 0);
    click(200.0f, y);
    frame(3);
    d = dbg();
  }
  CT_CHECK(d.voice_connected && d.voice_channel == voice);
  CT_CHECK(d.voice_bar.h > 0.0f);
  CT_CHECK(d.mute_btn.w > 0.0f && d.deafen_btn.w > 0.0f);
  CT_CHECK(d.camera_btn.w > 0.0f && d.go_live_btn.w > 0.0f && d.disconnect_btn.w > 0.0f);
  CT_CHECK(ccord_store_user_speaking(F.store, ccord_store_self_id(F.store)));
  CT_CHECK(ccord_store_user_speaking(F.store, 1002u));
  save_stream_screenshot("voice_stage_speaking.png");

  // Mute / deafen buttons next to the settings gear drive the session voice state.
  click(d.mute_btn.x + d.mute_btn.w * 0.5f, d.mute_btn.y + d.mute_btn.h * 0.5f);
  frame(2);
  const ccord_voice_state *self = ccord_store_voice_user(F.store, guild, ccord_store_self_id(F.store));
  CT_CHECK(self && self->self_mute);
  CT_CHECK(!ccord_store_user_speaking(F.store, ccord_store_self_id(F.store)));
  click(d.mute_btn.x + d.mute_btn.w * 0.5f, d.mute_btn.y + d.mute_btn.h * 0.5f);
  frame(2);
  self = ccord_store_voice_user(F.store, guild, ccord_store_self_id(F.store));
  CT_CHECK(self && !self->self_mute);
  CT_CHECK(ccord_store_user_speaking(F.store, ccord_store_self_id(F.store)));
  click(d.deafen_btn.x + d.deafen_btn.w * 0.5f, d.deafen_btn.y + d.deafen_btn.h * 0.5f);
  frame(2);
  self = ccord_store_voice_user(F.store, guild, ccord_store_self_id(F.store));
  CT_CHECK(self && self->self_deaf);
  click(d.deafen_btn.x + d.deafen_btn.w * 0.5f, d.deafen_btn.y + d.deafen_btn.h * 0.5f);
  frame(2);

  // Camera is a local stage flag; screen share opens the Go Live source & quality modal.
  click(d.camera_btn.x + d.camera_btn.w * 0.5f, d.camera_btn.y + d.camera_btn.h * 0.5f);
  CT_CHECK(dbg().camera_on);
  frame(2);
  self = ccord_store_voice_user(F.store, guild, ccord_store_self_id(F.store));
  CT_CHECK(self && self->self_video);   // camera toggles travel through the session/transport
  click(d.go_live_btn.x + d.go_live_btn.w * 0.5f, d.go_live_btn.y + d.go_live_btn.h * 0.5f);
  frame(2);
  d = dbg();
  CT_CHECK(d.go_live_open);
  CT_CHECK(F.ui->go_live_source_count > 0);
  save_stream_screenshot("go_live_modal.png");
  CT_CHECK(d.go_live_option_count[0] == 5 && d.go_live_option_count[1] == 2 &&
           d.go_live_option_count[2] == 5 && d.go_live_option_count[3] == 4 && d.go_live_option_count[4] == 3);
  // Pick AMF / AV1 / Vulkan / 1440p / 15 FPS.
  ccord_rect picks[5] = {d.go_live_option[0][2], d.go_live_option[1][1], d.go_live_option[2][3],
                         d.go_live_option[3][2], d.go_live_option[4][0]};
  for (int i = 0; i < 5; i++) {
    click(picks[i].x + picks[i].w * 0.5f, picks[i].y + picks[i].h * 0.5f);
  }
  d = dbg();
  click(d.go_live_start.x + d.go_live_start.w * 0.5f, d.go_live_start.y + d.go_live_start.h * 0.5f);
  frame(4);
  d = dbg();
  CT_CHECK(d.streaming && !d.go_live_open);
  ccord_capture_source active_src;
  CT_CHECK(ccord_ui_active_capture_source(F.ui, &active_src) && active_src.title[0] != '\0');
  {
    ccord_error verr = {0};
    uint8_t thumb[320 * 180 * 4];
    int tw = 0, th = 0;
    if (ccord_capture_thumbnail(&active_src, 320, 180, thumb, &tw, &th, &verr) && tw > 0 && th > 0) {
      (void)ccord_render_upload_rgba(F.renderer, "__video_stream__", thumb, tw, th, &verr);
    }
  }
  frame(2);
  save_stream_screenshot("stream_stage_local.png");
  const ccord_stream_state *st = ccord_store_stream(F.store);
  CT_CHECK(st && st->active && st->channel_id == voice);
  CT_CHECK(st->config.encoder == CCORD_STREAM_ENCODER_AMF);
  CT_CHECK(st->config.codec == CCORD_STREAM_CODEC_AV1);
  CT_CHECK(st->config.framework == CCORD_STREAM_FRAMEWORK_VULKAN);
  CT_CHECK(st->config.resolution == CCORD_STREAM_RES_1440P);
  CT_CHECK(st->config.fps == CCORD_STREAM_FPS_15);

  // Reopen and stop the stream.
  click(d.go_live_btn.x + d.go_live_btn.w * 0.5f, d.go_live_btn.y + d.go_live_btn.h * 0.5f);
  frame(1);
  d = dbg();
  CT_CHECK(d.go_live_open);
  click(d.go_live_start.x + d.go_live_start.w * 0.5f, d.go_live_start.y + d.go_live_start.h * 0.5f);
  frame(4);
  CT_CHECK(!dbg().streaming);

  // Watch Ada's remote stream from her stage participant tile.
  CT_CHECK(F.ui->stage_watch_count >= 1);
  if (F.ui->stage_watch_count >= 1) {
    ccord_rect wbtn = F.ui->stage_watch_rects[0];
    click(wbtn.x + wbtn.w * 0.5f, wbtn.y + wbtn.h * 0.5f);
    frame(4);
    const ccord_watched_stream *ws = ccord_session_watched_stream(F.session);
    CT_CHECK(ws && ws->active && ws->user_id == 1002u && !ws->paused);
    save_stream_screenshot("stream_stage_watching.png");
    // Stop watching via the stage Stop Watching button.
    ccord_rect stop_wbtn = F.ui->stream_stage_btn2_rect;
    CT_CHECK(stop_wbtn.w > 0.0f && stop_wbtn.h > 0.0f);
    click(stop_wbtn.x + stop_wbtn.w * 0.5f, stop_wbtn.y + stop_wbtn.h * 0.5f);
    frame(4);
    ws = ccord_session_watched_stream(F.session);
    CT_CHECK(!ws || !ws->active);
  }

  // Clicking the voice channel reopens its stage, then a text channel closes it.
  float y = scan_hover_channel(200.0f, voice);
  CT_CHECK(y > 0);
  click(200.0f, y);
  frame(2);
  CT_CHECK(dbg().voice_stage_open);
  self = ccord_store_voice_user(F.store, guild, ccord_store_self_id(F.store));
  if (self && self->self_mute) {
    d = dbg();
    click(d.mute_btn.x + d.mute_btn.w * 0.5f, d.mute_btn.y + d.mute_btn.h * 0.5f);
    frame(2);
  }
  CT_CHECK(ccord_store_user_speaking(F.store, ccord_store_self_id(F.store)));
  CT_CHECK(ccord_store_user_speaking(F.store, 1002u));
  save_stream_screenshot("voice_stage_speaking.png");
  ccord_id text = find_channel(guild, "general", CCORD_CHANNEL_TEXT);
  if (text == CCORD_ID_NONE) text = ccord_store_selected_channel(F.store);
  y = scan_hover_channel(200.0f, text);
  CT_CHECK(y > 0);
  click(200.0f, y);
  frame(2);
  CT_CHECK(!dbg().voice_stage_open);

  // Disconnect clears the bar and the stage.
  d = dbg();
  click(d.disconnect_btn.x + d.disconnect_btn.w * 0.5f, d.disconnect_btn.y + d.disconnect_btn.h * 0.5f);
  frame(4);
  d = dbg();
  CT_CHECK(!d.voice_connected && !d.voice_stage_open && !d.camera_on);
  CT_CHECK(ccord_store_voice_user(F.store, guild, ccord_store_self_id(F.store)) == nullptr);
}

static void test_send(void)
{
  ccord_id ch = ccord_store_selected_channel(F.store);
  size_t before = ccord_store_message_count(F.store, ch);
  type_text("hello ");
  type_text("w\xC3\xB6rld");
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "hello w\xC3\xB6rld") == 0);
  frame(1);
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  // M0-3: a pending local echo appears at once (nonce set, not confirmed); the mock's answer replaces it in place.
  CT_CHECK(ccord_store_message_count(F.store, ch) == before + 1);
  const ccord_message *pend = ccord_store_message_at(F.store, ch, before);
  CT_CHECK(pend && pend->send_state == CCORD_MSG_SEND_PENDING && pend->nonce[0] != '\0' &&
           strcmp(pend->content, "hello w\xC3\xB6rld") == 0);
  frame(3);
  CT_CHECK(ccord_store_message_count(F.store, ch) == before + 1);
  const ccord_message *m = ccord_store_message_at(F.store, ch, before);
  CT_CHECK(m && strcmp(m->content, "hello w\xC3\xB6rld") == 0 && m->author_id == ccord_store_self_id(F.store));
  CT_CHECK(m && m->send_state == CCORD_MSG_SEND_CONFIRMED);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.message_count == before + 1 && d.stick_bottom && d.last_visible == before);
  CT_CHECK(d.acked_message == m->id);                    // newest message acknowledged while pinned to the bottom

  // sending while scrolled up jumps to the new message
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 30.0f, .x = l->message_area.x + 100, .y = l->message_area.y + 100});
  frame(1);
  CT_CHECK(!dbg().stick_bottom);
  type_text("second");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  CT_CHECK(dbg().stick_bottom && dbg().last_visible == before + 1);

  // Enter on blank text sends nothing; shift+Enter inserts a newline
  size_t count = ccord_store_message_count(F.store, ch);
  type_text("   ");
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(ccord_store_message_count(F.store, ch) == count);
  key(CCORD_KEY_BACKSPACE, 0);
  key(CCORD_KEY_BACKSPACE, 0);
  key(CCORD_KEY_BACKSPACE, 0);
  type_text("a");
  key(CCORD_KEY_RETURN, CCORD_MOD_SHIFT);
  type_text("b");
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "a\nb") == 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_C, CCORD_MOD_CTRL);
  CT_CHECK(strcmp(F.clip, "a\nb") == 0);
  key(CCORD_KEY_BACKSPACE, 0);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  frame(1);
}

// Number of context-menu application-command rows of one type the store currently holds for a guild.
// The fixture runs as an administrator, so every indexed command of the type passes the permission gate.
static size_t count_app_cmds(ccord_id guild, uint8_t command_type)
{
  size_t n = 0, cnt = 0;
  const ccord_slash_command *cmds = ccord_store_slash_commands(F.store, guild, &cnt);
  for (size_t i = 0; i < cnt; i++) if (cmds[i].command_type == command_type) n++;
  return n;
}

static void test_context_menu(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  // Load the application-command index up front so every menu open below includes the same rows.
  ccord_id guild0 = ccord_store_selected_guild(F.store);
  (void)ccord_session_ensure_slash_commands(F.session, guild0, ccord_store_selected_channel(F.store), F.now);
  frame(6);
  size_t msg_app_cmds = count_app_cmds(guild0, 3);
  float x = l->message_area.x + 300;
  float y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  ccord_id msg = dbg().hover_message;
  const ccord_message *mm_pre = ccord_store_find_message(F.store, ccord_store_selected_channel(F.store), msg);
  size_t own_react_item = (mm_pre && mm_pre->reaction_kind_count > 0) ? 1u : 0u;
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(2);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.context_menu_open && d.context_menu_items == 13u + own_react_item + msg_app_cmds);   // Copy, React, Edit, Delete, Reply, Forward, Copy Link, Pin, Mark Unread, Save, Create Reminder, Create Thread (+ View Reactions) (+ MESSAGE app commands) + plugin item
  // Esc closes
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!dbg().context_menu_open);
  // reopen and click the plugin item (last row, after the built-ins and one separator)
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(2);
  CT_CHECK(dbg().context_menu_open);
  CT_CHECK(menu_click_label("Copy Message ID"));
  frame(1);
  CT_CHECK(!dbg().context_menu_open);
  CT_CHECK(F.menu_activations == 1);
  CT_CHECK(strcmp(F.last_owner, "test.plugin") == 0 && strcmp(F.last_item, "copy-id") == 0);
  CT_CHECK(F.last_ctx.target == CCORD_MENU_MESSAGE && F.last_ctx.message_id == msg && F.last_ctx.channel_id == ccord_store_selected_channel(F.store));
  const ccord_message *mm = ccord_store_find_message(F.store, F.last_ctx.channel_id, msg);
  CT_CHECK(mm && F.last_ctx.user_id == mm->author_id);

  // built-in Copy Text puts the message text on the clipboard
  F.clip[0] = '\0';
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(menu_click_label("Copy Text"));
  frame(1);
  CT_CHECK(mm && strcmp(F.clip, mm->content) == 0);

  // clicking outside closes without activating
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  int acts = F.menu_activations;
  mouse(CCORD_IN_MOUSE_DOWN, 1, 5.0f, 5.0f);
  frame(1);
  CT_CHECK(!dbg().context_menu_open && F.menu_activations == acts);

  // Someone else's message has no Edit: Copy, React, Reply, Pin, the plugin item, Report Message, and Delete because you administer
  // this server (the mock gives you the Admins role here, so Manage Messages comes from the permission engine).
  float oy = scan_hover_other_message(x);
  if (oy > 0) {
    const ccord_message *om = ccord_store_find_message(F.store, ccord_store_selected_channel(F.store), dbg().hover_message);
    size_t oth_react_item = (om && om->reaction_kind_count > 0) ? 1u : 0u;
    mouse(CCORD_IN_MOUSE_DOWN, 3, x, oy);
    frame(1);
    CT_CHECK(dbg().context_menu_open && dbg().context_menu_items == 13u + oth_react_item + msg_app_cmds);
    key(CCORD_KEY_ESCAPE, 0);
  }

  // a channel menu: Mute and Notification Settings, the plugin item plus the four channel-management items an administrator gets; a member menu opens
  // too, because an administrator can moderate members (a plain member would get no menu: see the gating test)
  ccord_id guild = ccord_store_selected_guild(F.store);
  ccord_id ch = find_channel(guild, "dev", CCORD_CHANNEL_TEXT);
  float cy2 = scan_hover_channel(200.0f, ch);
  CT_CHECK(cy2 > 0);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, cy2);
  frame(1);
  CT_CHECK(dbg().context_menu_open && dbg().context_menu_items == 8);
  key(CCORD_KEY_ESCAPE, 0);
  if (l->member_list_visible) {
    mouse(CCORD_IN_MOUSE_DOWN, 3, l->member_list.x + 60, l->member_list.y + 60);
    frame(1);
    CT_CHECK(dbg().context_menu_open);
    key(CCORD_KEY_ESCAPE, 0);
    frame(1);
    CT_CHECK(!dbg().context_menu_open);
  }
}

// A menu taller than the window scrolls instead of losing its bottom rows, and clicks follow the scrolled rows.
static void test_context_menu_scroll(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float x = l->message_area.x + 300.0f;
  float y = scan_hover_self_message(x);
  CT_CHECK(y > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(F.ui->menu.open);
  ccord_menu_context ctx = F.ui->menu.ctx;
  ui_menu_close(F.ui);

  // Open at the bottom edge: the panel must flip up and stay inside [8, win_h - 8].
  ui_menu_open(F.ui, 300.0f, 780.0f, &ctx);
  CT_CHECK(F.ui->menu.open);
  // Fill it past the window height (each row is 32 px; the window is 800 px).
  while (F.ui->menu.count < 32 && F.ui->menu.count < UI_MENU_MAX_ITEMS) {
    menu_item *it = &F.ui->menu.items[F.ui->menu.count++];
    *it = (menu_item){.parent = -1};
    it->label = ccord_strdup("Scrollable row");
    it->owner = ccord_strdup("test.plugin");
    it->item_id = ccord_strdup("copy-id");
  }
  frame(1);
  ui_menu *m = &F.ui->menu;
  CT_CHECK(m->content_h > m->h);
  CT_CHECK(m->h <= 800.0f - 16.0f + 0.01f);
  CT_CHECK(m->y >= 8.0f - 0.01f && m->y + m->h <= 800.0f - 8.0f + 0.01f);
  CT_CHECK(m->scroll_y == 0.0f);
  CT_CHECK(m->scrollbar_rect.w == 4.0f && m->scrollbar_rect.h > 0.0f);

  // Wheel over the panel scrolls to the last row; the thumb tracks the visible window.
  float mx = m->x + m->w * 0.5f, my = m->y + m->h * 0.5f;
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -200.0f, .x = mx, .y = my});
  frame(1);
  CT_CHECK(m->scroll_y > 0.0f);
  CT_CHECK(fabsf(m->scroll_y - (m->content_h - m->h)) < 0.01f);
  CT_CHECK(m->scrollbar_rect.x >= m->x && m->scrollbar_rect.x + m->scrollbar_rect.w <= m->x + m->w + 0.01f);
  CT_CHECK(m->scrollbar_rect.y >= m->y && m->scrollbar_rect.y + m->scrollbar_rect.h <= m->y + m->h + 0.01f);

  // The scrolled bottom row owns its on-screen rect and dispatches its plugin action.
  size_t last = m->count - 1;
  ccord_rect r = ui_menu_item_rect(F.ui, last);
  CT_CHECK(r.y >= m->y - 0.01f && r.y + r.h <= m->y + m->h + 0.01f);
  int acts = F.menu_activations;
  mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  frame(1);
  mouse(CCORD_IN_MOUSE_DOWN, 1, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  frame(1);
  CT_CHECK(!m->open);
  CT_CHECK(F.menu_activations == acts + 1);
}

static int64_t self_typing_expiry(ccord_id ch)
{
  ccord_id self = ccord_store_self_id(F.store);
  size_t n = ccord_store_typing_count(F.store, ch);
  for (size_t i = 0; i < n; i++) {
    if (ccord_store_typing_user_at(F.store, ch, i) == self) return ccord_store_typing_expires_at(F.store, ch, i);
  }
  return -1;
}

static void test_typing_throttle(void)
{
  ccord_id ch = ccord_store_selected_channel(F.store);
  F.now += 8000;                                 // leave the window opened by test_send's keystrokes
  frame(1);
  type_text("q");
  frame(1);
  size_t n = ccord_store_typing_count(F.store, ch);
  int64_t e0 = self_typing_expiry(ch);
  CT_CHECK(n >= 1 && e0 > 0);                    // first keystroke sent a typing notice
  CT_CHECK(dbg().typing_count == n);
  type_text("t");
  frame(1);
  CT_CHECK(ccord_store_typing_count(F.store, ch) == n);
  CT_CHECK(self_typing_expiry(ch) == e0);        // second keystroke inside the throttle window is dropped
  F.now += 8000;
  frame(1);
  type_text("y");
  frame(1);
  CT_CHECK(self_typing_expiry(ch) > e0);         // a second notice goes out after the window
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
}

static void test_message_actions(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float x = l->message_area.x + 300.0f;
  float y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  ccord_id ch = ccord_store_selected_channel(F.store);
  ccord_id msg = dbg().hover_message;

  // Add Reaction opens the emoji picker for this message; Escape dismisses it and sends nothing
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(menu_click_label("Add Reaction"));
  frame(2);
  CT_CHECK(F.ui->emoji_picker_open && F.ui->emoji_picker_msg_id == msg);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!F.ui->emoji_picker_open);
  const ccord_message *m = ccord_store_find_message(F.store, ch, msg);
  CT_CHECK(m && m->reaction_count == 0 && !m->self_reacted);

  // Edit Message loads the body into the composer, Enter saves it
  y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  msg = dbg().hover_message;
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(menu_click_label("Edit Message"));
  frame(1);
  m = ccord_store_find_message(F.store, ch, msg);
  CT_CHECK(m && strcmp(ccord_ui_composer_text(F.ui), m->content) == 0);
  CT_CHECK(dbg().composer_editing);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("edited body");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  m = ccord_store_find_message(F.store, ch, msg);
  CT_CHECK(m && strcmp(m->content, "edited body") == 0 && m->edited);
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  CT_CHECK(!dbg().composer_editing);

  // Delete Message removes it through the transport echo
  y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  msg = dbg().hover_message;
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(menu_click_label("Delete Message"));
  frame(3);
  CT_CHECK(ccord_store_find_message(F.store, ch, msg) == nullptr);
}

// Appends a message with structure at the end of the open channel, scrolls to it and draws it.


// What each kind of structured message draws, how a system message reads, and what the search filters see.
static void test_rich_message_rendering(void)
{
  ccord_id ch = ccord_store_selected_channel(F.store);
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ccord_error err;
  char text[256];

  // Reply cards already on screen (the reply sent earlier in this test) are the baseline for the reply checks.
  ccord_id plain = append_rich_message(ch, "plain", nullptr);
  size_t base_replies = dbg().reply_cards;
  remove_message(ch, plain);

  // A forward: a "Forwarded" card, then the forwarded files and embeds as ordinary cards.
  ccord_attachment fatt = {.id = 1, .filename = "orig.png", .content_type = "image/png", .url = "https://cdn.discordapp.com/attachments/1/2/orig.png"};
  ccord_embed femb = {.title = "Original embed"};
  ccord_message_rich fwd = {.present = CCORD_RICH_ALL, .reference = {.kind = CCORD_REF_FORWARD, .message_id = 5},
                            .forward = {.present = true, .content = "the original words", .attachments = &fatt, .attachment_count = 1,
                                        .embeds = &femb, .embed_count = 1}};
  ccord_id id = append_rich_message(ch, "", &fwd);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.forward_cards == 1 && d.attachment_cards == 1 && d.embed_cards == 1 && d.reply_cards == base_replies);
  remove_message(ch, id);

  // With attachment/embed previews off, only the forward header remains.
  F.ui->settings_attachments = false;
  F.ui->settings_embeds = false;
  id = append_rich_message(ch, "", &fwd);
  d = dbg();
  CT_CHECK(d.forward_cards == 1 && d.attachment_cards == 0 && d.embed_cards == 0 && d.reply_cards == base_replies);
  remove_message(ch, id);
  F.ui->settings_attachments = true;
  F.ui->settings_embeds = true;

  // A poll draws one card; a sticker draws one sticker.
  ccord_poll_answer poll_ans[2] = {
    {.id = 1, .text = "Yes", .votes = 3, .me_voted = true},
    {.id = 2, .text = "No", .votes = 1},
  };
  ccord_message_rich poll = {.present = CCORD_RICH_ALL, .poll = {.present = true, .question = "Pizza?", .answers = poll_ans, .answer_count = 2}};
  id = append_rich_message(ch, "", &poll);
  d = dbg();
  CT_CHECK(d.poll_cards == 1 && d.sticker_cards == 0);
  remove_message(ch, id);
  ccord_msg_sticker stk = {.id = 8001, .name = "WumpusHi", .format = 1};
  ccord_message_rich sticker = {.present = CCORD_RICH_ALL, .stickers = &stk, .sticker_count = 1};
  id = append_rich_message(ch, "", &sticker);
  d = dbg();
  CT_CHECK(d.sticker_cards == 1 && d.poll_cards == 0);
  remove_message(ch, id);

  // A reply whose original was deleted still draws its card.
  ccord_message_rich gone = {.present = CCORD_RICH_ALL, .type = 19, .reference = {.kind = CCORD_REF_REPLY, .message_id = 999999},
                             .ref_preview = {.deleted = true}};
  id = append_rich_message(ch, "answer", &gone);
  d = dbg();
  CT_CHECK(d.reply_cards == base_replies + 1);
  remove_message(ch, id);

  // An unknown reply target with no preview draws no card (nothing to show).
  ccord_message_rich unknown = {.present = CCORD_RICH_ALL, .type = 19, .reference = {.kind = CCORD_REF_REPLY, .message_id = 999999}};
  id = append_rich_message(ch, "answer", &unknown);
  CT_CHECK(dbg().reply_cards == base_replies);
  remove_message(ch, id);

  // System messages read "<name> <what happened>"; text-carrying ones add it; unknown ones say so.
  const ccord_user *self = ccord_store_user(F.store, ccord_store_self_id(F.store));
  CT_CHECK(self != nullptr);
  ccord_message_rich join = {.present = CCORD_RICH_ALL, .type = 7};
  id = append_rich_message(ch, "", &join);
  const ccord_message *jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0);
  char want[256];
  snprintf(want, sizeof want, "%s joined the server.", self ? self->name : "Someone");
  CT_CHECK(strcmp(text, want) == 0);
  remove_message(ch, id);
  ccord_message_rich thread = {.present = CCORD_RICH_ALL, .type = 18};
  id = append_rich_message(ch, "planning", &thread);
  jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0);
  snprintf(want, sizeof want, "%s started a thread: planning", self ? self->name : "Someone");
  CT_CHECK(strcmp(text, want) == 0);
  remove_message(ch, id);
  ccord_message_rich odd = {.present = CCORD_RICH_ALL, .type = 46};
  id = append_rich_message(ch, "", &odd);
  jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0);
  snprintf(want, sizeof want, "%s sent a system message.", self ? self->name : "Someone");
  CT_CHECK(strcmp(text, want) == 0);
  remove_message(ch, id);
  ccord_message_rich odd_text = {.present = CCORD_RICH_ALL, .type = 46};   // an unknown type that has text shows the text
  id = append_rich_message(ch, "poll results here", &odd_text);
  jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0 && strcmp(text, "poll results here") == 0);
  remove_message(ch, id);
  // Only the types that name something append it (a channel rename, a thread title); an empty name adds nothing at all,
  // and the rest never show their text next to the wording.
  ccord_message_rich renamed = {.present = CCORD_RICH_ALL, .type = 4};
  id = append_rich_message(ch, "ideas", &renamed);
  jm = ccord_store_find_message(F.store, ch, id);
  snprintf(want, sizeof want, "%s changed the channel name: ideas", self ? self->name : "Someone");
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0 && strcmp(text, want) == 0);
  remove_message(ch, id);
  id = append_rich_message(ch, "", &renamed);
  jm = ccord_store_find_message(F.store, ch, id);
  snprintf(want, sizeof want, "%s changed the channel name:", self ? self->name : "Someone");
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0 && strcmp(text, want) == 0);
  remove_message(ch, id);
  id = append_rich_message(ch, "", &thread);
  jm = ccord_store_find_message(F.store, ch, id);
  snprintf(want, sizeof want, "%s started a thread:", self ? self->name : "Someone");
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0 && strcmp(text, want) == 0);
  remove_message(ch, id);
  ccord_message_rich pinned = {.present = CCORD_RICH_ALL, .type = 6};
  id = append_rich_message(ch, "stray text", &pinned);
  jm = ccord_store_find_message(F.store, ch, id);
  snprintf(want, sizeof want, "%s pinned a message to this channel.", self ? self->name : "Someone");
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) > 0 && strcmp(text, want) == 0);
  remove_message(ch, id);
  // A message with nothing to say in words (a sticker, a file) is not a system message: it has no text.
  ccord_message_rich wordless = {.present = CCORD_RICH_ALL, .type = 0};
  id = append_rich_message(ch, "", &wordless);
  jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) == 0 && text[0] == '\0');
  remove_message(ch, id);
  ccord_message_rich silent_reply = {.present = CCORD_RICH_ALL, .type = 19};
  id = append_rich_message(ch, "", &silent_reply);
  jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) == 0 && text[0] == '\0');
  remove_message(ch, id);
  // An ordinary message is its text and nothing else; the copied text never has a system line or an edited tag.
  ccord_message_rich reply = {.present = CCORD_RICH_ALL, .type = 19};
  id = append_rich_message(ch, "just text", &reply);
  jm = ccord_store_find_message(F.store, ch, id);
  CT_CHECK(jm && ui_message_display_body(F.ui, jm, text, sizeof text) == 9 && strcmp(text, "just text") == 0);
  CT_CHECK(jm && ui_message_clean_body(F.ui, jm, text, sizeof text) == 9 && strcmp(text, "just text") == 0);
  char tiny[5];
  CT_CHECK(jm && ui_message_clean_body(F.ui, jm, tiny, sizeof tiny) == 4 && strcmp(tiny, "just") == 0);
  CT_CHECK(ui_message_display_body(F.ui, nullptr, text, sizeof text) == 0 && ui_message_clean_body(F.ui, nullptr, text, sizeof text) == 0);
  remove_message(ch, id);

  // Search filters read the structure: has:file, has:image, has:video, has:sound, has:sticker, has:embed, has:link.
  ccord_attachment a_img = {.id = 1, .filename = "p.png", .content_type = "image/png", .url = "https://cdn.discordapp.com/a/p.png"};
  ccord_attachment a_vid = {.id = 2, .filename = "v.mp4", .content_type = "video/mp4", .url = "https://cdn.discordapp.com/a/v.mp4"};
  ccord_attachment a_snd = {.id = 3, .filename = "s.ogg", .content_type = "audio/ogg", .url = "https://cdn.discordapp.com/a/s.ogg"};
  ccord_attachment a_doc = {.id = 4, .filename = "d.pdf", .content_type = "application/pdf", .url = "https://cdn.discordapp.com/a/d.pdf"};
  ccord_embed e_pic = {.title = "pic", .thumbnail = {.url = "https://e.example/t.png"}};
  ccord_embed e_link = {.title = "link", .url = "https://e.example/page"};
  ccord_embed e_vid = {.title = "vid", .video = {.url = "https://e.example/v.mp4"}};
  ccord_message_rich r_img = {.present = CCORD_RICH_ALL, .attachments = &a_img, .attachment_count = 1};
  ccord_message_rich r_vid = {.present = CCORD_RICH_ALL, .attachments = &a_vid, .attachment_count = 1};
  ccord_message_rich r_snd = {.present = CCORD_RICH_ALL, .attachments = &a_snd, .attachment_count = 1};
  ccord_message_rich r_doc = {.present = CCORD_RICH_ALL, .attachments = &a_doc, .attachment_count = 1};
  ccord_message_rich r_epic = {.present = CCORD_RICH_ALL, .embeds = &e_pic, .embed_count = 1};
  ccord_message_rich r_elink = {.present = CCORD_RICH_ALL, .embeds = &e_link, .embed_count = 1};
  ccord_message_rich r_evid = {.present = CCORD_RICH_ALL, .embeds = &e_vid, .embed_count = 1};
  ccord_id ids[9];
  ids[0] = append_rich_message(ch, "", &r_img);
  ids[1] = append_rich_message(ch, "", &r_vid);
  ids[2] = append_rich_message(ch, "", &r_snd);
  ids[3] = append_rich_message(ch, "", &r_doc);
  ids[4] = append_rich_message(ch, "", &r_epic);
  ids[5] = append_rich_message(ch, "", &r_elink);
  ids[6] = append_rich_message(ch, "", &r_evid);
  ids[7] = append_rich_message(ch, "", &sticker);
  ids[8] = append_rich_message(ch, "", nullptr);   // nothing at all
  d = dbg();
  click(d.search_btn.x + d.search_btn.w * 0.5f, d.search_btn.y + d.search_btn.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().search_open);
  struct { const char *query; size_t matches; } filters[] = {
    {"has:file", 4},      // the four attachments
    {"has:image", 2},     // an image file, and an embed with a picture
    {"has:video", 2},     // a video file, and an embed with a video
    {"has:sound", 1},
    {"has:sticker", 1},
    {"has:embed", 3},
    {"has:link", 1},      // only the embed with a link (the earlier channel text has plain links too: counted below)
  };
  size_t base_links = 0;
  for (size_t i = 0; i < ccord_store_message_count(F.store, ch); i++) {
    const ccord_message *cm = ccord_store_message_at(F.store, ch, i);
    bool has_http = cm && cm->content && (strstr(cm->content, "http://") || strstr(cm->content, "https://"));
    if (has_http) base_links++;
  }
  for (size_t i = 0; i < sizeof filters / sizeof filters[0]; i++) {
    key(CCORD_KEY_A, CCORD_MOD_CTRL);
    key(CCORD_KEY_BACKSPACE, 0);
    type_text(filters[i].query);
    frame(3);
    size_t want_n = filters[i].matches + (strcmp(filters[i].query, "has:link") == 0 ? base_links : 0u);
    if (dbg().search_matches != want_n) fprintf(stderr, "filter %s: got %zu want %zu\n", filters[i].query, dbg().search_matches, want_n);
    CT_CHECK(dbg().search_matches == want_n);
  }
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!dbg().search_open);
  for (size_t i = 0; i < 9; i++) CT_CHECK(ccord_store_delete_message(F.store, ch, ids[i], &err) == CCORD_OK);
  (void)l;
  frame(2);
}

static void test_reply_pin_search_cards(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float x = l->message_area.x + 300.0f;
  float y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  ccord_id ch = ccord_store_selected_channel(F.store);
  ccord_id target = dbg().hover_message;

  // Reply arms the composer banner.
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(menu_click_label("Reply"));
  frame(1);
  CT_CHECK(dbg().replying);
  type_text("reply body");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(!dbg().replying);
  size_t n = ccord_store_message_count(F.store, ch);
  const ccord_message *sent = ccord_store_message_at(F.store, ch, n - 1);
  CT_CHECK(sent && sent->reply_to_id == target && strcmp(sent->content, "reply body") == 0);

  // Pin toggles the message's pinned flag through the session.
  y = scan_hover_self_message(x);
  CT_CHECK(y > 0);
  target = dbg().hover_message;
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  frame(1);
  CT_CHECK(menu_click_label("Pin Message"));
  frame(3);
  const ccord_message *pinned = ccord_store_find_message(F.store, ch, target);
  CT_CHECK(pinned && pinned->pinned);
  // The header Pinned Messages popover lists it and closes on Esc.
  ccord_ui_debug d = dbg();
  click(d.pin_btn.x + d.pin_btn.w * 0.5f, d.pin_btn.y + d.pin_btn.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().pins_open);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!dbg().pins_open);

  // Search keeps the full transcript in the timeline; matches feed the results panel instead.
  size_t total = ccord_store_message_count(F.store, ch);
  d = dbg();
  click(d.search_btn.x + d.search_btn.w * 0.5f, d.search_btn.y + d.search_btn.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().search_open);
  type_text("supercalifragilistic");
  frame(3);
  d = dbg();
  CT_CHECK(d.search_matches > 0 && d.search_matches < total);
  CT_CHECK(d.message_count == total);   // the timeline is not filtered by search
  CT_CHECK(F.ui->view_count == total);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(3);
  CT_CHECK(dbg().message_count == total);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(!dbg().search_open);

  // Rich cards: reply bar, attachment card and accented embed card.
  const ccord_message *last = ccord_store_message_at(F.store, ch, ccord_store_message_count(F.store, ch) - 1);
  CT_CHECK(last != nullptr);
  ccord_attachment card_att = {.id = 1, .filename = "shot.png", .content_type = "image/png",
                               .url = "https://cdn.discordapp.com/attachments/1/2/shot.png", .size = 2048};
  ccord_embed card_emb = {.title = "Release", .description = "Notes for v1"};
  ccord_message_rich card_rich = {
    .present = CCORD_RICH_ALL, .type = 19,
    .reference = {.kind = CCORD_REF_REPLY, .message_id = 999999},   // not in the store: the card comes from the preview
    .ref_preview = {.present = true, .author_name = "Ada", .content = "original note"},
    .attachments = &card_att, .attachment_count = 1, .embeds = &card_emb, .embed_count = 1,
  };
  ccord_message card = {.id = last->id + 1, .channel_id = ch, .author_id = ccord_store_self_id(F.store),
                        .timestamp_ms = last->timestamp_ms + 1, .content = "Release ready", .rich = &card_rich};
  ccord_error err;
  CT_CHECK(ccord_store_append_message(F.store, &card, &err) == CCORD_OK);
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(3);
  d = dbg();
  CT_CHECK(d.reply_cards >= 1 && d.attachment_cards >= 1 && d.embed_cards >= 1);
  CT_CHECK(ccord_store_delete_message(F.store, ch, card.id, &err) == CCORD_OK);
  frame(2);
  test_rich_message_rendering();
}

// Request budget: what the UI asks Discord for while the user types a search or reads a busy channel.
static void type_slowly(const char *s)
{
  for (const char *p = s; *p; p++) {
    char one[2] = {*p, '\0'};
    type_text(one);
    frame(1);   // 50 ms between keystrokes
  }
}

static void test_request_budget_search_and_ack(void)
{
  ccord_id ch = ccord_store_selected_channel(F.store);
  CT_CHECK(ch != CCORD_ID_NONE);

  // --- search: one request when the user pauses, not one per keystroke ---
  ccord_ui_debug d = dbg();
  frame(10);
  size_t base = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH);
  click(d.search_btn.x + d.search_btn.w * 0.5f, d.search_btn.y + d.search_btn.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().search_open);
  type_slowly("hello world");                       // 11 keystrokes, 50 ms apart
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base);   // still typing: nothing sent
  frame(12);                                        // the user pauses (600 ms)
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base + 1);
  key(CCORD_KEY_RETURN, 0);                         // Enter right after it went out does not repeat it
  frame(2);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base + 1);

  // A changed query typed fast and confirmed with Enter goes out at once, and the pending debounce is cancelled.
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  type_slowly("hello");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base + 2);
  frame(12);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base + 2);

  // A single character is too short to search for, and closing the box with a query pending sends nothing.
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  type_slowly("a");
  frame(12);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base + 2);
  type_slowly("b");                                 // "ab" is pending ...
  key(CCORD_KEY_ESCAPE, 0);                         // ... but the user closes the box
  CT_CHECK(!dbg().search_open);
  frame(12);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_SEARCH) == base + 2);

  // --- read ack: a burst of incoming messages is acknowledged once per gap, newest message last ---
  frame(60);                                        // 3 s: the channel is settled and its ack gap has passed
  size_t acks = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK);
  ccord_error ack_err;
  ccord_message_rich r_burst = {.present = CCORD_RICH_ALL};
  for (int i = 0; i < 6; i++) {
    ccord_id bmid = 0;
    CT_CHECK(ccord_mock_post_rich_message(F.transport, ch, 1002, "burst", &r_burst, &bmid, &ack_err) == CCORD_OK);
    frame(1);
  }
  size_t n = ccord_store_message_count(F.store, ch);
  const ccord_message *newest = ccord_store_message_at(F.store, ch, n - 1);
  CT_CHECK(newest != nullptr);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK) - acks <= 1);   // the burst is not acked 6 times
  frame(60);                                        // 3 s later the newest message is acknowledged
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK) - acks == 2);
  CT_CHECK(dbg().acked_message == newest->id);
  frame(60);                                        // and then nothing more, however long the user stays
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK) - acks == 2);
}

static void test_request_budget_typing_and_channel_switch(void)
{
  ccord_error err;
  ccord_id g = ccord_store_selected_guild(F.store);
  ccord_id home = ccord_store_selected_channel(F.store);

  // --- typing: one notice per throttle window, however many keys are pressed ---
  frame(200);                                       // 10 s: any earlier typing notice has expired
  size_t t0 = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_TYPING);
  type_slowly("a long message that takes a while");   // 34 keys over ~1.7 s
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_TYPING) - t0 == 1);
  frame(160);                                       // 8 s later the notice is refreshed, once
  type_slowly("more");
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_TYPING) - t0 == 2);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);

  // --- channel switching: each channel's history is requested at most once, however often the user flips ---
  size_t h0 = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_HISTORY);
  ccord_id text[8];
  size_t ntext = 0;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, g) && ntext < 8; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, g, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT) text[ntext++] = c->id;
  }
  CT_CHECK(ntext >= 3);
  for (int round = 0; round < 4; round++) {
    for (size_t i = 0; i < ntext; i++) {
      CT_CHECK(ccord_session_select_channel(F.session, text[i], &err) == CCORD_OK);
      frame(1);
    }
  }
  frame(10);
  size_t asked = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_HISTORY) - h0;
  CT_CHECK(asked <= ntext);                         // 4 rounds over ntext channels: no more than one request each
  CT_CHECK(ccord_session_select_channel(F.session, home, &err) == CCORD_OK);
  frame(2);
}

static void test_extensions_switch(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  click(l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
  CT_CHECK(dbg().settings_open);
  ccord_ui_set_extensions_enabled(F.ui, false);            // app-side sync: no host callback
  CT_CHECK(F.ext_calls == 0 && !dbg().extensions_on);
  click(dbg().settings_plugins_tab.x + 10.0f, dbg().settings_plugins_tab.y + 10.0f);
  ccord_rect t = dbg().extensions_toggle;
  click(t.x + t.w - 20.0f, t.y + t.h * 0.5f);             // switch on
  CT_CHECK(F.ext_calls == 1 && F.ext_last && dbg().extensions_on);
  click(t.x + t.w - 20.0f, t.y + t.h * 0.5f);             // switch off
  CT_CHECK(F.ext_calls == 2 && !F.ext_last && !dbg().extensions_on);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!dbg().settings_open);
}

static void test_settings_categories(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  click(l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
  ccord_ui_settings_debug s;
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.open && s.page_count == 9);
  // Slot 1 is the retired Profiles page: it keeps its index but has no tab.
  for (int i = 0; i < s.page_count; i++) {
    if (i == 1) CT_CHECK(s.tabs[i].w == 0.0f && s.tabs[i].h == 0.0f);
    else CT_CHECK(s.tabs[i].w > 0.0f && s.tabs[i].h > 0.0f);
  }

  // Appearance: compact/cozy selector.
  click(s.tabs[2].x + 10.0f, s.tabs[2].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 2);
  CT_CHECK(s.display_mode_value == 1);
  click(s.display_mode[0].x + 12.0f, s.display_mode[0].y + 12.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.display_mode_value == 0);
  click(s.display_mode[1].x + 12.0f, s.display_mode[1].y + 12.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.display_mode_value == 1);

  // Voice & Video: devices, volume sliders, mode and hardware acceleration.
  click(s.tabs[3].x + 10.0f, s.tabs[3].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 3);
  click(s.input_device.x + 20.0f, s.input_device.y + 12.0f);
  click(s.output_device.x + 20.0f, s.output_device.y + 12.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.mic_device_value == 1);
  CT_CHECK(s.speaker_device_value == 1);
  click(s.input_volume.x + s.input_volume.w * 0.25f, s.input_volume.y + 2.0f);
  click(s.output_volume.x + s.output_volume.w * 0.75f, s.output_volume.y + 2.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.input_volume_value >= 20 && s.input_volume_value <= 30);
  CT_CHECK(s.output_volume_value >= 70 && s.output_volume_value <= 80);
  click(s.voice_mode[1].x + 12.0f, s.voice_mode[1].y + 12.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.voice_mode_value == 1);
  bool hw = s.hardware_accel;
  click(s.hardware_toggle.x + s.hardware_toggle.w - 10.0f, s.hardware_toggle.y + s.hardware_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.hardware_accel == !hw);

  // Streaming & Encoders: default encoder, framework, resolution, FPS and codec.
  click(s.tabs[4].x + 10.0f, s.tabs[4].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 4);
  CT_CHECK(s.codec_count == 2);
  save_stream_screenshot("settings_streaming.png");
  click(s.encoder[2].x + 10.0f, s.encoder[2].y + 12.0f);
  click(s.framework[3].x + 10.0f, s.framework[3].y + 12.0f);
  click(s.resolution[1].x + 10.0f, s.resolution[1].y + 12.0f);
  click(s.fps[2].x + 10.0f, s.fps[2].y + 12.0f);
  click(s.codec[1].x + 10.0f, s.codec[1].y + 12.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.encoder_value == 2 && s.framework_value == 3 && s.resolution_value == 1 && s.fps_value == 2 &&
           s.codec_value == CCORD_STREAM_CODEC_AV1);

  // Text & Images: inline embed and attachment cards.
  click(s.tabs[5].x + 10.0f, s.tabs[5].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 5);
  CT_CHECK(s.embeds && s.attachments);
  click(s.embed_toggle.x + s.embed_toggle.w - 10.0f, s.embed_toggle.y + s.embed_toggle.h * 0.5f);
  click(s.attachment_toggle.x + s.attachment_toggle.w - 10.0f, s.attachment_toggle.y + s.attachment_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(!s.embeds && !s.attachments);

  // The same choices are persisted to the INI file (display mode back to cozy, cards off).
  FILE *sf = fopen(ui_test_file("settings.ini"), "r");
  CT_CHECK(sf != nullptr);
  if (sf) {
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, sf);
    buf[n] = '\0';
    fclose(sf);
    CT_CHECK(strstr(buf, "[ui]") != nullptr);
    CT_CHECK(strstr(buf, "display_mode = 1") != nullptr);
    CT_CHECK(strstr(buf, "embeds = 0") != nullptr);
    CT_CHECK(strstr(buf, "attachments = 0") != nullptr);
  }

  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!dbg().settings_open);

  // With both Text & Images switches off, attachment/embed markers stay plain (no cards).
  ccord_id ch = ccord_store_selected_channel(F.store);
  size_t n = ccord_store_message_count(F.store, ch);
  const ccord_message *last = n ? ccord_store_message_at(F.store, ch, n - 1) : nullptr;
  ccord_attachment off_att = {.id = 1, .filename = "shot.png", .content_type = "image/png",
                              .url = "https://cdn.discordapp.com/attachments/1/2/shot.png", .size = 2048};
  ccord_embed off_emb = {.title = "Release", .description = "Notes for v1"};
  ccord_message_rich off_rich = {.present = CCORD_RICH_ALL, .attachments = &off_att, .attachment_count = 1,
                                 .embeds = &off_emb, .embed_count = 1};
  ccord_message card = {.id = last ? last->id + 1 : 1, .channel_id = ch, .author_id = ccord_store_self_id(F.store),
                        .timestamp_ms = last ? last->timestamp_ms + 1 : (int64_t)F.now,
                        .content = "", .rich = &off_rich};
  ccord_error err;
  CT_CHECK(ccord_store_append_message(F.store, &card, &err) == CCORD_OK);
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(3);
  ccord_ui_debug cards = dbg();
  CT_CHECK(cards.attachment_cards == 0 && cards.embed_cards == 0);
  CT_CHECK(ccord_store_delete_message(F.store, ch, card.id, &err) == CCORD_OK);
  frame(2);
}

static void test_settings_and_resize(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float gx = l->user_panel.x + l->user_panel.w - 24.0f, gy = l->user_panel.y + l->user_panel.h * 0.5f;
  click(gx, gy);
  CT_CHECK(dbg().settings_open);
  CT_CHECK(!F.text_input_active);   // modal: the composer no longer gets text
  type_text("zzz");
  CT_CHECK(ccord_ui_composer_text(F.ui)[0] == '\0');
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!dbg().settings_open && F.text_input_active);
  click(gx, gy);
  CT_CHECK(dbg().settings_open);
  // the close button (top-right of the full-window Settings page: 32 px square, 16 px inset) closes it
  click(1280.0f - 56.0f + 16.0f, 16.0f + 16.0f);
  CT_CHECK(!dbg().settings_open);

  // resize: layout stays valid, bottom anchor kept
  frame(1);
  CT_CHECK(dbg().stick_bottom);
  ui_send((ccord_input_event){.type = CCORD_IN_RESIZE, .x = 700.0f, .y = 450.0f});
  frame(2);
  l = ccord_ui_get_layout(F.ui);
  CT_CHECK(!l->member_list_visible);
  CT_CHECK(l->message_area.w > 100 && l->message_area.h > 50);
  CT_CHECK(fabsf(l->composer.y + l->composer.h - 450.0f) < 0.01f);
  CT_CHECK(dbg().stick_bottom && dbg().last_visible == dbg().message_count - 1);
  ui_send((ccord_input_event){.type = CCORD_IN_RESIZE, .x = 1280.0f, .y = 800.0f});
  frame(2);
  CT_CHECK(ccord_ui_get_layout(F.ui)->member_list_visible);
}

static void test_members_toggle(void)
{
  frame(2);
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  CT_CHECK(l->member_list_visible);
  ccord_rect b = dbg().members_toggle;
  CT_CHECK(b.w > 0.0f && b.h > 0.0f);
  CT_CHECK(b.y >= l->chat_header.y && b.y + b.h <= l->chat_header.y + l->chat_header.h);
  CT_CHECK(b.x + b.w <= l->chat_header.x + l->chat_header.w);
  float chat_w = l->message_area.w;
  click(b.x + b.w * 0.5f, b.y + b.h * 0.5f);   // hide
  frame(2);
  l = ccord_ui_get_layout(F.ui);
  CT_CHECK(!l->member_list_visible);
  CT_CHECK(fabsf(l->message_area.w - (chat_w + F.theme.m.member_list_width)) < 0.01f);
  CT_CHECK(dbg().members_toggle.w > 0.0f);   // the button stays so the list can come back
  CT_CHECK(dbg().stick_bottom);
  b = dbg().members_toggle;
  click(b.x + b.w * 0.5f, b.y + b.h * 0.5f);   // show again
  frame(2);
  l = ccord_ui_get_layout(F.ui);
  CT_CHECK(l->member_list_visible && fabsf(l->message_area.w - chat_w) < 0.01f);
  // narrow window: no member list, no button; hidden state does not resurrect the list
  click(b.x + b.w * 0.5f, b.y + b.h * 0.5f);   // hide
  ui_send((ccord_input_event){.type = CCORD_IN_RESIZE, .x = 700.0f, .y = 450.0f});
  frame(2);
  CT_CHECK(!ccord_ui_get_layout(F.ui)->member_list_visible && dbg().members_toggle.w == 0.0f);
  ui_send((ccord_input_event){.type = CCORD_IN_RESIZE, .x = 1280.0f, .y = 800.0f});
  frame(2);
  CT_CHECK(!ccord_ui_get_layout(F.ui)->member_list_visible && dbg().members_toggle.w > 0.0f);
  b = dbg().members_toggle;
  click(b.x + b.w * 0.5f, b.y + b.h * 0.5f);   // restore the default for the following tests
  frame(2);
  CT_CHECK(ccord_ui_get_layout(F.ui)->member_list_visible);
}

static void test_media_and_redirects(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  // Open Settings -> My Account: test Billing, Nitro, Shop, Server Boost browser redirects.
  click(l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
  ccord_ui_settings_debug s;
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.open);
  click(s.tabs[0].x + 10.0f, s.tabs[0].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 0);
  CT_CHECK(s.billing_btn.w > 0.0f && s.nitro_btn.w > 0.0f && s.shop_btn.w > 0.0f && s.boost_btn.w > 0.0f);

  int before_urls = F.open_url_calls;
  click(s.billing_btn.x + 20.0f, s.billing_btn.y + 12.0f);
  CT_CHECK(F.open_url_calls == before_urls + 1 && strcmp(F.last_url, "https://discord.com/settings/billing") == 0);
  click(s.nitro_btn.x + 20.0f, s.nitro_btn.y + 12.0f);
  CT_CHECK(F.open_url_calls == before_urls + 2 && strcmp(F.last_url, "https://discord.com/nitro") == 0);
  click(s.shop_btn.x + 20.0f, s.shop_btn.y + 12.0f);
  CT_CHECK(F.open_url_calls == before_urls + 3 && strcmp(F.last_url, "https://discord.com/shop") == 0);
  click(s.boost_btn.x + 20.0f, s.boost_btn.y + 12.0f);
  CT_CHECK(F.open_url_calls == before_urls + 4 && strcmp(F.last_url, "https://discord.com/nitro") == 0);

  // Appearance: Custom theme overrides toggle.
  click(s.tabs[2].x + 10.0f, s.tabs[2].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 2 && !s.quickcss_enabled);
  click(s.quickcss_toggle.x + s.quickcss_toggle.w - 10.0f, s.quickcss_toggle.y + s.quickcss_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.quickcss_enabled);
  click(s.quickcss_toggle.x + s.quickcss_toggle.w - 10.0f, s.quickcss_toggle.y + s.quickcss_toggle.h * 0.5f);

  // Text & Images: re-enable inline embeds and attachments for media card testing.
  click(s.tabs[5].x + 10.0f, s.tabs[5].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  if (!s.embeds) click(s.embed_toggle.x + s.embed_toggle.w - 10.0f, s.embed_toggle.y + s.embed_toggle.h * 0.5f);
  if (!s.attachments) click(s.attachment_toggle.x + s.attachment_toggle.w - 10.0f, s.attachment_toggle.y + s.attachment_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.embeds && s.attachments);

  // Notifications: Desktop notification toggle and timeout selector.
  click(s.tabs[6].x + 10.0f, s.tabs[6].y + 10.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.page == 6 && s.notify_desktop);
  click(s.notify_timeout[2].x + 12.0f, s.notify_timeout[2].y + 12.0f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.notify_timeout_ms == 10000);

  ccord_id ch = ccord_store_selected_channel(F.store);
  ccord_notification raw_notif = {.guild_id = ccord_store_selected_guild(F.store), .channel_id = ch,
                                  .message_id = 1, .author_id = ccord_store_self_id(F.store),
                                  .is_dm = false, .is_mention = true};
  char ntitle[128] = {0}, nbody[256] = {0};
  CT_CHECK(ccord_ui_prepare_notification(F.ui, &raw_notif, ntitle, sizeof ntitle, nbody, sizeof nbody));
  CT_CHECK(ntitle[0] != '\0' && nbody[0] != '\0');

  // Disabling desktop notifications suppresses ccord_ui_prepare_notification.
  click(s.notify_desktop_toggle.x + s.notify_desktop_toggle.w - 10.0f,
        s.notify_desktop_toggle.y + s.notify_desktop_toggle.h * 0.5f);
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(!s.notify_desktop);
  CT_CHECK(!ccord_ui_prepare_notification(F.ui, &raw_notif, ntitle, sizeof ntitle, nbody, sizeof nbody));
  click(s.notify_desktop_toggle.x + s.notify_desktop_toggle.w - 10.0f,
        s.notify_desktop_toggle.y + s.notify_desktop_toggle.h * 0.5f);

  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!dbg().settings_open);

  int saved_w = 0, saved_h = 0;
  CT_CHECK(ccord_ui_get_saved_window_size(F.ui, &saved_w, &saved_h));
  CT_CHECK(saved_w == 1280 && saved_h == 800);

  // Guild right-click menu: Server Boost, Nitro, Shop, Billing browser redirects, plus Server Settings and Create
  // Channel because you administer this server.
  ccord_rect gr1 = ui_guild_icon_rect(F.ui, 1);
  float gx = gr1.x + gr1.w * 0.5f;
  float gy = gr1.y + gr1.h * 0.5f;
  mouse(CCORD_IN_MOUSE_DOWN, 3, gx, gy);
  frame(1);
  CT_CHECK(dbg().context_menu_open && dbg().context_menu_items == 10);   // + Mute Server, Notification Settings, Events, Leave Server
  CT_CHECK(menu_click_label("Discord Nitro (Browser)"));
  CT_CHECK(!dbg().context_menu_open);
  CT_CHECK(strcmp(F.last_url, "https://discord.com/nitro") == 0);

  // Media texture fetching/rendering (guild icon, user avatar, image attachment/embed) and clickable chat links.
  ccord_error err;
  const ccord_guild *g0 = ccord_store_guild_at(F.store, 0);
  CT_CHECK(g0 != nullptr);
  char g_name_buf[128];
  snprintf(g_name_buf, sizeof g_name_buf, "%s", g0->name ? g0->name : "");
  ccord_guild g_icon = {.id = g0->id, .name = g_name_buf, .icon_url = "a_1234567890abcdef"};
  CT_CHECK(ccord_store_upsert_guild(F.store, &g_icon, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);

  const ccord_user *self = ccord_store_user(F.store, ccord_store_self_id(F.store));
  CT_CHECK(self != nullptr);
  char u_name_buf[128], u_status_buf[128];
  snprintf(u_name_buf, sizeof u_name_buf, "%s", self->name ? self->name : "");
  snprintf(u_status_buf, sizeof u_status_buf, "%s", self->status_text ? self->status_text : "");
  ccord_user u_orig = {.id = self->id, .name = u_name_buf, .status_text = u_status_buf, .avatar_url = "",
                       .bio = "", .pronouns = "", .presence = self->presence, .is_bot = self->is_bot,
                       .avatar_palette = self->avatar_palette};
  ccord_user u_av = u_orig;
  u_av.avatar_url = "fe3ffc01cc5d93689dbd3f09462374e7";
  CT_CHECK(ccord_store_upsert_user(F.store, &u_av, &err) == CCORD_OK);

  size_t mcount = ccord_store_message_count(F.store, ch);
  const ccord_message *last = mcount ? ccord_store_message_at(F.store, ch, mcount - 1) : nullptr;
  ccord_attachment media_att = {.id = 1, .filename = "pic.png", .content_type = "image/png", .size = 512,
                                .url = "https://cdn.discordapp.com/attachments/1/2/pic.png"};
  ccord_embed media_emb = {.title = "Shot", .description = "Preview", .url = "https://media.discordapp.net/attachments/1/2/shot.png",
                           .thumbnail = {.url = "https://media.discordapp.net/attachments/1/2/shot.png"}};
  ccord_message_rich media_rich = {.present = CCORD_RICH_ALL, .attachments = &media_att, .attachment_count = 1,
                                   .embeds = &media_emb, .embed_count = 1};
  ccord_message media_msg = {
    .id = last ? last->id + 1 : 1,
    .channel_id = ch,
    .author_id = u_av.id,
    .timestamp_ms = last ? last->timestamp_ms + 1 : (int64_t)F.now,
    .content = "Visit [Shop](https://discord.com/store/skus/99) or https://discord.com/billing/payments",
    .rich = &media_rich,
  };
  CT_CHECK(ccord_store_append_message(F.store, &media_msg, &err) == CCORD_OK);
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(6);
  ccord_ui_debug md = dbg();
  CT_CHECK(md.media_textures >= 3);
  CT_CHECK(md.guild_icon_textures_drawn >= 1);
  CT_CHECK(md.avatar_textures_drawn >= 1);
  CT_CHECK(md.attachment_textures_drawn >= 1);
  CT_CHECK(md.clickable_links >= 2);

  // Verify opening the emoji picker and evicting textures from the renderer cache
  // immediately restores profile pictures and guild icons from the session cache.
  click(l->composer.x + l->composer.w - 58.0f, l->composer.y + 22.0f);   // emoji button: 30px left of the mic
  frame(3);
  CT_CHECK(dbg().emoji_picker_open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!dbg().emoji_picker_open);
  uint8_t dummy_px[4] = {255, 0, 255, 255};
  for (int ti = 0; ti < 1040; ti++) {
    char dkey[64];
    snprintf(dkey, sizeof dkey, "https://cdn.discordapp.com/emojis/%d.png", 900000 + ti);
    CT_CHECK(ccord_render_upload_rgba(F.renderer, dkey, dummy_px, 1, 1, &err) == CCORD_OK);
  }
  frame(2);
  md = dbg();
  CT_CHECK(md.guild_icon_textures_drawn >= 1);
  CT_CHECK(md.avatar_textures_drawn >= 1);
  CT_CHECK(md.attachment_textures_drawn >= 1);

  // Scan and click a rendered link in the message area; payment/store routes normalize to discord.com browser URLs.
  bool clicked_link = false;
  int urls_before_click = F.open_url_calls;
  for (float ly = l->message_area.y + l->message_area.h - 260.0f;
       ly < l->message_area.y + l->message_area.h && !clicked_link; ly += 4.0f) {
    for (float lx = l->message_area.x + 60.0f; lx < l->message_area.x + 420.0f; lx += 12.0f) {
      mouse(CCORD_IN_MOUSE_MOVE, 0, lx, ly);
      if (dbg().hover_link >= 0) {
        click(lx, ly);
        clicked_link = true;
        break;
      }
    }
  }
  CT_CHECK(clicked_link && F.open_url_calls == urls_before_click + 1);
  CT_CHECK(strcmp(F.last_url, "https://discord.com/shop") == 0 ||
           strcmp(F.last_url, "https://discord.com/settings/billing") == 0);

  // Restore original message, user, and guild state before screenshot pixel checks.
  CT_CHECK(ccord_store_delete_message(F.store, ch, media_msg.id, &err) == CCORD_OK);
  frame(2);
}

static void test_screenshot_pixels(void)
{
  frame(3);
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float gx = l->message_area.x + 300;
  float y = scan_hover_message(gx);
  mouse(CCORD_IN_MOUSE_DOWN, 3, gx, y);
  frame(3);
  CT_CHECK(dbg().context_menu_open);
  F.now += 50;
  (void)ccord_ui_update(F.ui, F.now);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  const char *path = "ui_test_shot.png";
  ccord_error err;
  CT_CHECK(ccord_render_screenshot(F.renderer, path, &err) == CCORD_OK);
  ccord_render_end(F.renderer);
  SDL_Surface *s = SDL_LoadPNG(path);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(s->w == 1280 && s->h == 800);
  const ccord_theme_colors *c = &F.theme.c;
  CT_CHECK(pixel_is(s, 5, 700, c->bg_tertiary, 1));                    // server list
  CT_CHECK(pixel_is(s, 150, 600, c->bg_secondary, 1));                  // channel list
  CT_CHECK(pixel_is(s, 1100, 760, c->bg_secondary, 1));                 // member list
  CT_CHECK(pixel_is(s, 120, 780, c->bg_user_panel, 1));                 // user panel
  ccord_rect box = {l->composer.x + F.theme.m.composer_margin_x, l->composer.y, l->composer.w - 2 * F.theme.m.composer_margin_x, F.theme.m.composer_min_height};
  CT_CHECK(pixel_is(s, (int)(box.x + box.w - 40), (int)(box.y + 6), c->bg_input, 1));   // composer
  CT_CHECK(pixel_is(s, 275, 98, c->bg_item_selected, 1) || pixel_is(s, 275, 99, c->bg_item_selected, 1));   // selected channel row
  ccord_rect sel_g = ui_guild_icon_rect(F.ui, 1);
  CT_CHECK(pixel_is(s, 2, (int)(sel_g.y + sel_g.h * 0.5f), c->pill, 2));                              // selected server pill
  // The context menu body is drawn at the pointer position.
  CT_CHECK(pixel_is(s, (int)gx + 150, (int)y + 20, c->bg_floating, 2));
  // not blank: many distinct colours on a coarse grid
  uint32_t seen[64] = {0};
  size_t nseen = 0;
  for (int py = 0; py < 800; py += 5) {
    for (int px = 0; px < 1280; px += 5) {
      uint8_t r, g, b, a;
      SDL_ReadSurfacePixel(s, px, py, &r, &g, &b, &a);
      uint32_t v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
      bool found = false;
      for (size_t i = 0; i < nseen; i++) found = found || seen[i] == v;
      if (!found && nseen < 64) seen[nseen++] = v;
    }
  }
  CT_CHECK(nseen >= 8);
  SDL_DestroySurface(s);
  remove(path);
  key(CCORD_KEY_ESCAPE, 0);
}

static void test_reactions_autocomplete_attachments_and_login(void)
{
  frame(2);
  // 1. Composer autocomplete: @member, #channel, :emoji:
  type_text("@");
  frame(1);
  CT_CHECK(dbg().autocomplete_open && dbg().autocomplete_count > 0);
  key(CCORD_KEY_TAB, 0);
  frame(1);
  CT_CHECK(!dbg().autocomplete_open && ccord_ui_composer_text(F.ui)[0] == '@');
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);

  type_text(":fire");
  frame(1);
  CT_CHECK(dbg().autocomplete_open && dbg().autocomplete_count == 1);
  key(CCORD_KEY_RETURN, 0);
  frame(1);
  CT_CHECK(!dbg().autocomplete_open && strlen(ccord_ui_composer_text(F.ui)) > 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);

  // 2. Staged file attachment send via ccord_session_send_message_with_file
  static const uint8_t k_png[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  CT_CHECK(ccord_ui_attach_bytes(F.ui, "test_image.png", k_png, sizeof k_png));
  frame(1);
  CT_CHECK(dbg().has_staged_attachment && strcmp(dbg().staged_filename, "test_image.png") == 0);
  ccord_id ch = ccord_store_selected_channel(F.store);
  size_t before = ccord_store_message_count(F.store, ch);
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  CT_CHECK(!dbg().has_staged_attachment);
  CT_CHECK(ccord_store_message_count(F.store, ch) == before + 1);

  // 3. Settings > My Account: Email/Password/2FA + standalone token entry
  ccord_ui_open_account_settings(F.ui);
  frame(1);
  ccord_ui_settings_debug s;
  ccord_ui_get_settings_debug(F.ui, &s);
  CT_CHECK(s.open && s.page == 0);
  CT_CHECK(s.account_email_box.w > 0.0f && s.account_password_box.w > 0.0f && s.account_token_box.w > 0.0f);
  type_text("user@example.com");
  key(CCORD_KEY_TAB, 0);
  type_text("secret_pw");
  const char *em = nullptr, *pw = nullptr, *mfa = nullptr;
  ccord_ui_account_login_fields(F.ui, &em, &pw, &mfa);
  CT_CHECK(em && strcmp(em, "user@example.com") == 0);
  CT_CHECK(pw && strcmp(pw, "secret_pw") == 0);
  click(s.account_token_box.x + 12.0f, s.account_token_box.y + 12.0f);
  type_text("TEST_TOKEN_VALUE");
  CT_CHECK(strcmp(ccord_ui_account_token_text(F.ui), "TEST_TOKEN_VALUE") == 0);
  ccord_ui_clear_account_token_text(F.ui);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!dbg().settings_open);
}

typedef struct login_host_state {
  char clip[256];
  int token_sign_ins;
  int cred_logins;
  int mfa_cancels;
} login_host_state;

static char *login_clip_get(void *ud)
{
  login_host_state *hs = ud;
  size_t n = strlen(hs->clip);
  char *out = malloc(n + 1);
  if (out) memcpy(out, hs->clip, n + 1);
  return out;
}

static void login_clip_set(void *ud, const char *utf8)
{
  login_host_state *hs = ud;
  snprintf(hs->clip, sizeof hs->clip, "%s", utf8 ? utf8 : "");
}

static void login_account_action(void *ud, ccord_account_action action)
{
  login_host_state *hs = ud;
  if (action == CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN) hs->token_sign_ins++;
  if (action == CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN) hs->cred_logins++;
  if (action == CCORD_ACCOUNT_ACTION_MFA_CANCEL) hs->mfa_cancels++;
}


static void test_dedicated_login_screen(void)
{
  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  login_test_transport *lt = calloc(1, sizeof *lt);
  CT_CHECK(bus && store && lt);
  lt->base.vt = &k_ltt_vt;
  ccord_session *sess = ccord_session_create(bus, store, &lt->base, &err);
  CT_CHECK(sess != nullptr);
  CT_CHECK(ccord_store_self_id(store) == CCORD_ID_NONE);
  CT_CHECK(ccord_store_guild_count(store) == 0);

  login_host_state hs = {.clip = "CLIPBOARD_DISCORD_TOKEN_999"};
  ccord_ui_deps deps = {
    .bus = bus,
    .store = store,
    .session = sess,
    .contrib = F.contrib,
    .theme = &F.theme,
    .host = {.ud = &hs, .clipboard_set = login_clip_set, .clipboard_get = login_clip_get, .account_action = login_account_action},
    .fixed_now_unix_ms = 1793536200000LL,
  };
  ccord_ui *ui = ccord_ui_create(&deps, &err);
  CT_CHECK(ui != nullptr);
  ccord_ui_set_viewport(ui, 1280.0f, 800.0f);
  ccord_ui_open_account_settings(ui);

  // Pump and render the full-window Login Screen
  (void)ccord_ui_update(ui, 100);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(ui, F.renderer);
  const char *lshot = "ui_login_screen_test.png";
  remove(lshot);
  CT_CHECK(ccord_render_screenshot(F.renderer, lshot, &err) == CCORD_OK);
  ccord_render_end(F.renderer);

  ccord_ui_debug d = {0};
  ccord_ui_get_debug(ui, &d);
  ccord_ui_settings_debug sd = {0};
  ccord_ui_get_settings_debug(ui, &sd);
  CT_CHECK(d.settings_open && d.login_screen_open && sd.login_screen);
  CT_CHECK(sd.login_card.w == 480.0f && sd.login_card.h == 420.0f);
  CT_CHECK(sd.login_tab == 0 && sd.account_focus_field == 1);

  SDL_Surface *surf = SDL_LoadPNG(lshot);
  CT_CHECK(surf != nullptr);
  if (surf) {
    // Full-window bg_tertiary backdrop outside the 480x420 card
    CT_CHECK(surf_pixel_near(surf, 20, 20, F.theme.c.bg_tertiary.r, F.theme.c.bg_tertiary.g, F.theme.c.bg_tertiary.b));
    // bg_secondary card interior (the shared uis_card token)
    CT_CHECK(surf_pixel_near(surf, (int)(sd.login_card.x + 12.0f), (int)(sd.login_card.y + 200.0f),
                             F.theme.c.bg_secondary.r, F.theme.c.bg_secondary.g, F.theme.c.bg_secondary.b));
    // brand Connect to Discord primary CTA button
    CT_CHECK(surf_pixel_near(surf, (int)(sd.account_token_save_btn.x + 16.0f),
                             (int)(sd.account_token_save_btn.y + 18.0f),
                             F.theme.c.brand.r, F.theme.c.brand.g, F.theme.c.brand.b));
    SDL_DestroySurface(surf);
  }
  remove(lshot);

  // Esc and outside-card clicks must NOT dismiss the unauthenticated Login Screen
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_ESCAPE});
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = 20.0f, .y = 20.0f});
  ccord_ui_get_debug(ui, &d);
  CT_CHECK(d.settings_open && d.login_screen_open);

  // Tab 1 (Token Sign-In): Paste from Clipboard button + Connect to Discord button
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                                                       .x = sd.account_token_paste_btn.x + 12.0f,
                                                       .y = sd.account_token_paste_btn.y + 12.0f});
  CT_CHECK(strcmp(ccord_ui_account_token_text(ui), "CLIPBOARD_DISCORD_TOKEN_999") == 0);
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                                                       .x = sd.account_token_save_btn.x + 12.0f,
                                                       .y = sd.account_token_save_btn.y + 12.0f});
  CT_CHECK(hs.token_sign_ins == 1);

  // Tab 2 (Email & Password): 2FA input is NOT shown inline until Discord requests MFA
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                                                       .x = sd.login_tab_credentials.x + 12.0f,
                                                       .y = sd.login_tab_credentials.y + 12.0f});
  ccord_ui_get_settings_debug(ui, &sd);
  CT_CHECK(sd.login_tab == 1 && sd.account_focus_field == 2);
  CT_CHECK(!sd.account_mfa_required && sd.account_mfa_box.w == 0.0f && sd.mfa_popup_rect.w == 0.0f);
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_TEXT, .text = "dev@discord.test"});
  // Right-click on Email box opens Copy/Cut/Paste/Select All/Clear menu and Copy works.
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 3, .clicks = 1,
                                                       .x = sd.account_email_box.x + 12.0f,
                                                       .y = sd.account_email_box.y + 12.0f});
  ccord_rect copy_r = {0};
  CT_CHECK(ccord_ui_context_menu_find(ui, "Copy", &copy_r));
  CT_CHECK(ccord_ui_context_menu_find(ui, "Paste", nullptr));
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                                                       .x = copy_r.x + 8.0f, .y = copy_r.y + 8.0f});
  CT_CHECK(strcmp(hs.clip, "dev@discord.test") == 0);
  // Tab switches cleanly between Email (2) and Password (3)
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_TAB});
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_TEXT, .text = "correct_horse"});
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                                                       .x = sd.account_login_btn.x + 12.0f,
                                                       .y = sd.account_login_btn.y + 12.0f});
  CT_CHECK(hs.cred_logins == 1);

  // Signing-in busy state must NOT prematurely open the 2FA popup
  ccord_ui_set_login_busy(ui, "Signing in...");
  ccord_ui_get_settings_debug(ui, &sd);
  CT_CHECK(!sd.account_mfa_required && sd.mfa_popup_rect.w == 0.0f);

  // Simulate MFA required response -> transitions to secondary 2FA popup and auto-focuses field 4
  ccord_ui_set_login_status(ui, "2FA required: enter 6-digit code", true);
  ccord_ui_get_settings_debug(ui, &sd);
  CT_CHECK(sd.account_mfa_required && sd.account_focus_field == 4);
  CT_CHECK(sd.mfa_popup_rect.w > 200.0f && sd.account_mfa_box.w > 100.0f && sd.mfa_back_btn.w > 100.0f);
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_TEXT, .text = "654321"});
  const char *em = nullptr, *pw = nullptr, *mfa = nullptr;
  ccord_ui_account_login_fields(ui, &em, &pw, &mfa);
  CT_CHECK(em && strcmp(em, "dev@discord.test") == 0);
  CT_CHECK(pw && strcmp(pw, "correct_horse") == 0);
  CT_CHECK(mfa && strcmp(mfa, "654321") == 0);

  // Wrong 2FA code error keeps the secondary 2FA popup open for retry
  ccord_ui_set_login_error(ui, "Invalid two-factor code", true);
  ccord_ui_get_settings_debug(ui, &sd);
  CT_CHECK(sd.account_mfa_required && sd.account_focus_field == 4);

  // Clicking "Back to Login" dismisses the 2FA popup, clears MFA code, and notifies host
  (void)ccord_ui_handle_input(ui, &(ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                                                       .x = sd.mfa_back_btn.x + 12.0f,
                                                       .y = sd.mfa_back_btn.y + 12.0f});
  ccord_ui_get_settings_debug(ui, &sd);
  CT_CHECK(!sd.account_mfa_required && hs.mfa_cancels == 1);
  ccord_ui_account_login_fields(ui, &em, &pw, &mfa);
  CT_CHECK(mfa && mfa[0] == '\0');

  // Connecting state -> Auth failure (4004/401) keeps Login Screen open with error banner
  ccord_ui_clear_account_token_text(ui);
  lt->emit_state = true;
  lt->next_state = CCORD_CONN_CONNECTING;
  (void)ccord_session_pump(sess);
  (void)ccord_bus_drain(bus);
  (void)ccord_ui_update(ui, 200);
  ccord_ui_get_debug(ui, &d);
  CT_CHECK(d.login_screen_open);

  lt->emit_state = true;
  lt->next_state = CCORD_CONN_FAILED;
  (void)ccord_session_pump(sess);
  (void)ccord_bus_drain(bus);
  (void)ccord_ui_update(ui, 250);
  ccord_ui_get_settings_debug(ui, &sd);
  CT_CHECK(sd.login_screen && sd.account_login_status[0] != '\0');

  // Gateway READY (SNAPSHOT + CCORD_CONN_CONNECTED) auto-closes the Login Screen into the live workspace
  lt->self_user = (ccord_user){.id = 42u, .name = "LiveUser", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  lt->guild = (ccord_guild){.id = 100u, .name = "Live Guild"};
  lt->chan = (ccord_channel){.id = 200u, .guild_id = 100u, .name = "general", .topic = "", .kind = CCORD_CHANNEL_TEXT};
  lt->snap = (ccord_snapshot){
    .self_id = 42u,
    .users = &lt->self_user,
    .user_count = 1,
    .guilds = &lt->guild,
    .guild_count = 1,
    .channels = &lt->chan,
    .channel_count = 1,
  };
  lt->emit_snap = true;
  (void)ccord_session_pump(sess);
  (void)ccord_bus_drain(bus);
  (void)ccord_ui_update(ui, 300);
  ccord_ui_get_debug(ui, &d);
  CT_CHECK(!d.settings_open && !d.login_screen_open);

  ccord_ui_destroy(ui);
  ccord_session_destroy(sess);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_category_grouping_and_media_throttling(void)
{
  ccord_error err = {0};
  ccord_id g_orig = ccord_store_selected_guild(F.store);
  ccord_guild g_cat = {.id = 777000u, .name = "Grouping Guild"};
  ccord_channel chans[] = {
    {.id = 777010u, .guild_id = 777000u, .parent_id = 0, .kind = CCORD_CHANNEL_CATEGORY, .name = "Cat A", .topic = ""},
    {.id = 777020u, .guild_id = 777000u, .parent_id = 0, .kind = CCORD_CHANNEL_CATEGORY, .name = "Cat B", .topic = ""},
    {.id = 777011u, .guild_id = 777000u, .parent_id = 777010u, .kind = CCORD_CHANNEL_TEXT, .name = "a-one", .topic = ""},
    {.id = 777001u, .guild_id = 777000u, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT, .name = "uncat-text", .topic = ""},
    {.id = 777021u, .guild_id = 777000u, .parent_id = 777020u, .kind = CCORD_CHANNEL_TEXT, .name = "b-one", .topic = ""},
    {.id = 777012u, .guild_id = 777000u, .parent_id = 777010u, .kind = CCORD_CHANNEL_TEXT, .name = "a-two", .topic = ""},
    {.id = 777002u, .guild_id = 777000u, .parent_id = 0, .kind = CCORD_CHANNEL_VOICE, .name = "uncat-voice", .topic = ""},
  };
  CT_CHECK(ccord_store_upsert_guild(F.store, &g_cat, chans, sizeof chans / sizeof chans[0], nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_guild(F.store, g_cat.id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, 777021u, &err) == CCORD_OK);
  frame(1);

  CT_CHECK(F.ui->ncrow == 7);
  const ccord_id want_order[7] = {777001u, 777002u, 777010u, 777011u, 777012u, 777020u, 777021u};
  for (size_t i = 0; i < 7; i++) {
    const ccord_channel *rc = ccord_store_channel_at(F.store, g_cat.id, F.ui->crow[i].index);
    CT_CHECK(rc && rc->id == want_order[i]);
  }

  // Collapse Cat A (row 2): its unselected children (a-one, a-two) hide, while uncategorized channels remain visible.
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float cat_a_y = l->channel_scroll.y + F.ui->crow[2].y + F.ui->crow[2].h * 0.5f;
  click(l->channel_scroll.x + 40.0f, cat_a_y);
  CT_CHECK(ui_is_collapsed(F.ui, 777010u));
  CT_CHECK(F.ui->ncrow == 5);
  const ccord_id want_collapsed[5] = {777001u, 777002u, 777010u, 777020u, 777021u};
  for (size_t i = 0; i < 5; i++) {
    const ccord_channel *rc = ccord_store_channel_at(F.store, g_cat.id, F.ui->crow[i].index);
    CT_CHECK(rc && rc->id == want_collapsed[i]);
  }

  // Media throttling: queue 6 fixed-size avatar URLs; one ui_pump_media call uploads at most 4 and does not bump vlist epoch.
  for (int i = 0; i < 6; i++) {
    char aurl[128];
    snprintf(aurl, sizeof aurl, "https://cdn.discordapp.com/avatars/88800%d/av%d.png", i, i);
    CT_CHECK(ccord_session_fetch_media(F.session, aurl, &err) == CCORD_OK);
  }
  (void)ccord_session_pump(F.session);
  size_t before_tex = F.ui->dbg_media_textures;
  uint32_t before_epoch = F.ui->vlist.epoch;
  ui_pump_media(F.ui, F.renderer);
  size_t after_first = F.ui->dbg_media_textures;
  CT_CHECK(after_first > before_tex && after_first - before_tex <= 4);
  CT_CHECK(F.ui->vlist.epoch == before_epoch);
  ui_pump_media(F.ui, F.renderer);
  CT_CHECK(F.ui->dbg_media_textures == before_tex + 6);
  CT_CHECK(F.ui->vlist.epoch == before_epoch);

  CT_CHECK(ccord_store_select_guild(F.store, g_orig, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_guild(F.store, g_cat.id, &err) == CCORD_OK);
  frame(1);
}


static void test_dm_layout_ordering_and_pagination(void)
{
  ccord_error err = {0};
  const ccord_layout *l = ccord_ui_get_layout(F.ui);

  // 1. Chunked scroll pagination: scrolling to the top pages 50-message chunks without duplicate requests.
  ccord_id g1 = ccord_store_guild_at(F.store, 0)->id;
  ccord_id gen = find_channel(g1, "general", CCORD_CHANNEL_TEXT);
  CT_CHECK(gen != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, gen, &err) == CCORD_OK);
  frame(3);
  size_t gen_before = ccord_store_message_count(F.store, gen);
  CT_CHECK(ccord_session_history_has_more(F.session, gen));
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 2000.0f,
                           .x = l->message_area.x + 100.0f, .y = l->message_area.y + 100.0f});
  frame(4);
  CT_CHECK(ccord_store_message_count(F.store, gen) == gen_before + CCORD_SESSION_HISTORY_LIMIT);

  // 2. DM channel ordering (descending effective activity: last_message_id ? last_message_id : id),
  //    Friends view tabs, and right-hand User Profile panel.
  ccord_user u1 = {.id = 1007u, .name = "Fatima", .handle = "fatima", .bio = "", .pronouns = "", .presence = CCORD_PRESENCE_ONLINE};
  ccord_user u2 = {.id = 1008u, .name = "Gus", .handle = "gus", .bio = "", .pronouns = "", .presence = CCORD_PRESENCE_IDLE};
  ccord_user u3 = {.id = 1009u, .name = "Hana", .handle = "hana", .bio = "", .pronouns = "", .presence = CCORD_PRESENCE_ONLINE};
  CT_CHECK(ccord_store_upsert_user(F.store, &u1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_user(F.store, &u2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_user(F.store, &u3, &err) == CCORD_OK);
  ccord_channel dm_old = {.id = 950001u, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                          .name = "Fatima", .topic = "", .user_id = u1.id, .last_message_id = 1000u};
  ccord_channel dm_new = {.id = 950002u, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                          .name = "Gus", .topic = "", .user_id = u2.id, .last_message_id = 5000u};
  // Newly created DM channel with last_message_id == 0 and a newer channel snowflake (980000u > 5000u):
  // UI cmp_dm_sort must fall back to channel.id so dm_created sorts above dm_new and dm_old!
  ccord_channel dm_created = {.id = 980000u, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
                              .name = "Hana", .topic = "", .user_id = u3.id, .last_message_id = 0u};
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm_old, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm_new, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm_created, &err) == CCORD_OK);

  ccord_rect dm_icon = ui_guild_icon_rect(F.ui, 0);
  click(dm_icon.x + dm_icon.w * 0.5f, dm_icon.y + dm_icon.h * 0.5f);
  frame(3);
  CT_CHECK(ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID);
  CT_CHECK(F.ui->ncrow >= 7);
  CT_CHECK(F.ui->crow[0].dm_nav == DM_NAV_FRIENDS);
  CT_CHECK(F.ui->crow[1].dm_nav == DM_NAV_NITRO);
  CT_CHECK(F.ui->crow[2].dm_nav == DM_NAV_SHOP);
  CT_CHECK(F.ui->crow[3].dm_nav == DM_NAV_DM_HEADER);
  const ccord_channel *first_dm = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[4].index);
  const ccord_channel *second_dm = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[5].index);
  const ccord_channel *third_dm = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[6].index);
  CT_CHECK(first_dm && second_dm && third_dm);
  CT_CHECK(first_dm && first_dm->id == dm_created.id);
  CT_CHECK(second_dm && second_dm->id == dm_new.id);
  CT_CHECK(third_dm && third_dm->id == dm_old.id);

  // Select dm_new and verify on-demand profile fetch populated u2's profile without reordering DM rows.
  float dm_new_y = l->channel_scroll.y + F.ui->crow[5].y + F.ui->crow[5].h * 0.5f;
  click(l->channel_scroll.x + 60.0f, dm_new_y);
  frame(3);
  const ccord_user *u2_after = ccord_store_user(F.store, u2.id);
  CT_CHECK(u2_after && u2_after->bio && u2_after->bio[0] != '\0' && u2_after->pronouns && u2_after->pronouns[0] != '\0');
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[4].index)->id == dm_created.id);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[5].index)->id == dm_new.id);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[6].index)->id == dm_old.id);

  // Click "Friends" in the DM sidebar -> opens Friends view with Online/All/Pending/Blocked tabs.
  float friends_y = l->channel_scroll.y + F.ui->crow[0].y + F.ui->crow[0].h * 0.5f;
  click(l->channel_scroll.x + 60.0f, friends_y);
  frame(2);
  CT_CHECK(ui_is_dm_friends_view(F.ui));
  for (int t = 0; t < 4; t++) {
    ccord_rect tr = F.ui->dm_friends_tab_rects[t];
    CT_CHECK(tr.w > 0.0f && tr.h > 0.0f);
    click(tr.x + tr.w * 0.5f, tr.y + tr.h * 0.5f);
    frame(1);
    CT_CHECK(F.ui->dm_friends_tab == t);
  }

  // Click dm_old row in DM sidebar -> exits Friends view and selects dm_old without reordering the DM list.
  float dm_old_y = l->channel_scroll.y + F.ui->crow[6].y + F.ui->crow[6].h * 0.5f;
  click(l->channel_scroll.x + 60.0f, dm_old_y);
  frame(3);
  CT_CHECK(!ui_is_dm_friends_view(F.ui));
  CT_CHECK(ccord_store_selected_channel(F.store) == dm_old.id);
  const ccord_user *u1_after = ccord_store_user(F.store, u1.id);
  CT_CHECK(u1_after && u1_after->bio && u1_after->bio[0] != '\0');
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[4].index)->id == dm_created.id);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[5].index)->id == dm_new.id);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[6].index)->id == dm_old.id);

  // Live message arriving in dm_old with id=990000 (> 980000) promotes dm_old to the top of the DM sidebar!
  ccord_message live_dm_msg = {.id = 990000u, .channel_id = dm_old.id, .author_id = u1.id,
                               .timestamp_ms = 1711901000000ULL, .content = "New DM activity"};
  CT_CHECK(ccord_store_append_message(F.store, &live_dm_msg, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[4].index)->id == dm_old.id);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[5].index)->id == dm_created.id);
  CT_CHECK(ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, F.ui->crow[6].index)->id == dm_new.id);

  // Verify DM User Profile panel toggle (dm_profile_hidden) is completely independent from Server Member List toggle (members_hidden):
  CT_CHECK(!F.ui->dm_profile_hidden && !F.ui->members_hidden);
  ccord_rect mb = ui_members_btn_rect(F.ui);
  CT_CHECK(mb.w > 0.0f);
  click(mb.x + mb.w * 0.5f, mb.y + mb.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->dm_profile_hidden && !F.ui->members_hidden);
  CT_CHECK(!ccord_ui_get_layout(F.ui)->member_list_visible);
  // Restore DM User Profile panel
  click(mb.x + mb.w * 0.5f, mb.y + mb.h * 0.5f);
  frame(1);
  CT_CHECK(!F.ui->dm_profile_hidden && ccord_ui_get_layout(F.ui)->member_list_visible);
  save_optional_screenshot("dm_1to1_user_profile.png");

  // Verify Group DM rendering in DM sidebar, header, and right-hand participants panel:
  ccord_channel dm_group = {
    .id = 995000u,
    .guild_id = CCORD_DM_GUILD_ID,
    .kind = CCORD_CHANNEL_TEXT,
    .discord_type = CCORD_DISCORD_CHANNEL_GROUP_DM,
    .name = "Direct Message",
    .topic = "",
    .user_id = CCORD_ID_NONE,
    .last_message_id = 999000u,
    .recipient_ids = {u1.id, u2.id, u3.id},
    .recipient_count = 3,
  };
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm_group, &err) == CCORD_OK);
  const ccord_channel *stored_gdm = ccord_store_channel(F.store, dm_group.id);
  CT_CHECK(stored_gdm != nullptr && stored_gdm->user_id == CCORD_ID_NONE);
  CT_CHECK(stored_gdm != nullptr && strcmp(stored_gdm->name, "Fatima, Gus, Hana") == 0);
  CT_CHECK(ccord_store_select_channel(F.store, dm_group.id, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ui_channel_is_group_dm(ui_selected_channel(F.ui)));
  CT_CHECK(ui_dm_recipient(F.ui, ui_selected_channel(F.ui)) == nullptr);
  save_optional_screenshot("dm_group_members.png");

  CT_CHECK(ccord_store_select_guild(F.store, g1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, dm_group.id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, dm_old.id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, dm_new.id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, dm_created.id, &err) == CCORD_OK);
  frame(2);

  // 3. Clicking plain message body text does NOT open the user profile popover;
  //    clicking the author display name, author avatar, or inline @mention pill opens it.
  if (F.ui->nvis > 0) {
    const vis_row *hdr_row = nullptr;
    for (size_t vi = F.ui->nvis; vi > 0; vi--) {
      if (F.ui->vis[vi - 1].author_rect.w > 0.0f && F.ui->vis[vi - 1].avatar_rect.w > 0.0f) {
        hdr_row = &F.ui->vis[vi - 1];
        break;
      }
    }
    CT_CHECK(hdr_row != nullptr);
    if (hdr_row) {
      // Plain message body text click -> must NOT open popover.
      float body_x = hdr_row->author_rect.x + 12.0f;
      float body_y = hdr_row->author_rect.y + hdr_row->author_rect.h + 8.0f;
      click(body_x, body_y);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);

      // Blank space to the right of the author header -> must NOT open popover.
      float blank_x = l->message_area.x + l->message_area.w - 40.0f;
      float blank_y = hdr_row->author_rect.y + hdr_row->author_rect.h * 0.5f;
      click(blank_x, blank_y);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);

      // Direct click on author display name -> opens popover and fetches profile.
      click(hdr_row->author_rect.x + hdr_row->author_rect.w * 0.5f,
            hdr_row->author_rect.y + hdr_row->author_rect.h * 0.5f);
      frame(2);
      CT_CHECK(F.ui->member_popover_open);
      CT_CHECK(F.ui->member_popover_user_id != CCORD_ID_NONE);
      CT_CHECK(F.ui->member_popover_rect.h >= 260.0f);
      const ccord_user_profile *mprof = ccord_store_user_profile(F.store, F.ui->member_popover_user_id);
      CT_CHECK(mprof != nullptr);

      // Click outside popover to close it.
      click(l->server_list.x + 10.0f, l->server_list.y + l->server_list.h - 10.0f);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);

      // Direct click on author avatar -> opens popover.
      click(hdr_row->avatar_rect.x + hdr_row->avatar_rect.w * 0.5f,
            hdr_row->avatar_rect.y + hdr_row->avatar_rect.h * 0.5f);
      frame(2);
      CT_CHECK(F.ui->member_popover_open);
      CT_CHECK(F.ui->member_popover_user_id != CCORD_ID_NONE);

      // Click outside popover to close it.
      click(l->server_list.x + 10.0f, l->server_list.y + l->server_list.h - 10.0f);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);
    }

    // Append a message with an inline <@id> mention and verify clicking the mention pill opens the popover,
    // while clicking the surrounding plain message text does not.
    ccord_id sel_ch = ccord_store_selected_channel(F.store);
    char mention_txt[128];
    snprintf(mention_txt, sizeof mention_txt, "Hello there <@%llu> welcome to the channel",
             (unsigned long long)u1.id);
    ccord_message m_ment = {
      .id = 999888777666ULL,
      .channel_id = sel_ch,
      .author_id = u2.id,
      .timestamp_ms = 1711900999000LL,
      .content = mention_txt,
    };
    CT_CHECK(ccord_store_append_message(F.store, &m_ment, &err) == CCORD_OK);
    frame(2);
    ccord_rect ment_rect = {0};
    for (size_t hi = 0; hi < F.ui->nuser_hits; hi++) {
      if (F.ui->user_hits[hi].user_id == u1.id) {
        ment_rect = F.ui->user_hits[hi].rect;
      }
    }
    CT_CHECK(ment_rect.w > 0.0f && ment_rect.h > 0.0f);
    if (ment_rect.w > 0.0f) {
      // Plain text after the mention pill -> does NOT open popover.
      click(ment_rect.x + ment_rect.w + 25.0f, ment_rect.y + ment_rect.h * 0.5f);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);

      // Clicking the inline @mention pill -> opens popover for u1.
      click(ment_rect.x + ment_rect.w * 0.5f, ment_rect.y + ment_rect.h * 0.5f);
      frame(2);
      CT_CHECK(F.ui->member_popover_open);
      CT_CHECK(F.ui->member_popover_user_id == u1.id);

      click(l->server_list.x + 10.0f, l->server_list.y + l->server_list.h - 10.0f);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);
    }

    // Verify right-side guild member list population, role grouping, and click-to-popover:
    ccord_id sel_gid = ccord_store_selected_guild(F.store);
    CT_CHECK(sel_gid != CCORD_ID_NONE && sel_gid != CCORD_DM_GUILD_ID);
    ccord_user extra_users[2] = {
      {.id = 77001, .name = "Zelda Admin", .handle = "zelda", .status_text = "Ruling Hyrule", .presence = CCORD_PRESENCE_ONLINE},
      {.id = 77002, .name = "Link Hero", .handle = "link", .status_text = "", .presence = CCORD_PRESENCE_OFFLINE},
    };
    ccord_member extra_members[2] = {
      {.guild_id = sel_gid, .user_id = 77001, .role_name = "Triforce", .role_color = 0xF1C40Fu, .role_rank = 1},
      {.guild_id = sel_gid, .user_id = 77002, .role_name = "", .role_color = 0, .role_rank = 255},
    };
    CT_CHECK(ccord_store_upsert_guild_members(F.store, sel_gid, extra_members, 2, extra_users, 2, &err) == CCORD_OK);
    frame(2);
    l = ccord_ui_get_layout(F.ui);
    CT_CHECK(l->member_list_visible);
    CT_CHECK(F.ui->nmrow >= 4);
    bool saw_triforce_group = false;
    bool saw_offline_group = false;
    const member_row *zelda_row = nullptr;
    for (size_t mi = 0; mi < F.ui->nmrow; mi++) {
      const member_row *mr = &F.ui->mrow[mi];
      if (mr->is_header) {
        if (strstr(mr->label, "TRIFORCE") != nullptr) saw_triforce_group = true;
        if (strstr(mr->label, "OFFLINE") != nullptr) saw_offline_group = true;
      } else {
        const ccord_member *m = ccord_store_member_at(F.store, sel_gid, mr->index);
        if (m && m->user_id == 77001) zelda_row = mr;
      }
    }
    CT_CHECK(saw_triforce_group);
    CT_CHECK(saw_offline_group);
    CT_CHECK(zelda_row != nullptr);
    if (zelda_row) {
      float click_x = l->member_list.x + l->member_list.w * 0.5f;
      float click_y = l->member_list.y + zelda_row->y - F.ui->member_scroll + zelda_row->h * 0.5f;
      click(click_x, click_y);
      frame(2);
      CT_CHECK(F.ui->member_popover_open);
      CT_CHECK(F.ui->member_popover_user_id == 77001);
      // In a Server guild, member popover exposes both User Profile (0) and Server Profile (1) tabs:
      CT_CHECK(F.ui->member_popover_tab == 0);
      CT_CHECK(F.ui->member_popover_tab_rects[0].w > 0.0f && F.ui->member_popover_tab_rects[1].w > 0.0f);
      click(F.ui->member_popover_tab_rects[1].x + F.ui->member_popover_tab_rects[1].w * 0.5f,
            F.ui->member_popover_tab_rects[1].y + F.ui->member_popover_tab_rects[1].h * 0.5f);
      frame(2);
      CT_CHECK(F.ui->member_popover_open && F.ui->member_popover_tab == 1);
      save_optional_screenshot("server_member_popover_tabs.png");
      click(l->server_list.x + 10.0f, l->server_list.y + l->server_list.h - 10.0f);
      frame(1);
      CT_CHECK(!F.ui->member_popover_open);
    }
  }
}


int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_interaction_core")) {
    fprintf(stderr, "test_ui_interaction_core: setup failed or no display driver; skipping\n");
    return 77;
  }
  test_markdown_tokens();
  test_markdown_pairing();
  test_load_and_virtualization();
  test_wheel_scroll();
  test_channel_click();
  test_server_click();
  test_dm_pseudo_guild();
  test_voice_and_stream();
  test_send();
  test_typing_throttle();
  test_context_menu();
  test_context_menu_scroll();
  test_message_actions();
  test_reply_pin_search_cards();
  test_request_budget_search_and_ack();
  test_request_budget_typing_and_channel_switch();
  test_settings_and_resize();
  test_extensions_switch();
  test_settings_categories();
  test_members_toggle();
  test_media_and_redirects();
  test_screenshot_pixels();
  test_reactions_autocomplete_attachments_and_login();
  test_dedicated_login_screen();
  test_category_grouping_and_media_throttling();
  test_dm_layout_ordering_and_pagination();
  ui_test_teardown();
  ct_failures += ui_test_harness_failures();
  return CT_RESULT("test_ui_interaction_core");
}
