// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <ftw.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int ccord_test_unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(fpath);
}
static char *ccord_test_data_dir = NULL;
static void ccord_test_cleanup_dir(void) {
    if (ccord_test_data_dir) {
        nftw(ccord_test_data_dir, ccord_test_unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
        free(ccord_test_data_dir);
        ccord_test_data_dir = NULL;
    }
}
[[maybe_unused]] static const char *ccord_test_init_dir(const char *prefix) {
    if (ccord_test_data_dir) return ccord_test_data_dir;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    size_t len = strlen(tmp) + strlen(prefix) + 16;
    ccord_test_data_dir = malloc(len);
    if (!ccord_test_data_dir) abort();
    snprintf(ccord_test_data_dir, len, "%s/%s_XXXXXX", tmp, prefix);
    if (!mkdtemp(ccord_test_data_dir)) {
        free(ccord_test_data_dir);
        ccord_test_data_dir = NULL;
        abort();
    }
    atexit(ccord_test_cleanup_dir);
    return ccord_test_data_dir;
}

// M6 UI host seams + composer IME + chat GIF autoplay gate (m6-ui-ime).
// Real store + mock transport + session + renderer, fake OS host, injected input
// events. Covers the preedit buffer/clamps, commit/Esc/blur clears, the caret
// text-input rect, the settings debug seam and gif_auto_play vs hover gating.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/paths.h>
#include <ccord/core/utf8.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/session/session.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"

#include "ccord_test.h"

typedef struct fixture {
  ccord_theme theme;
  ccord_bus *bus;
  ccord_store *store;
  ccord_session *session;
  ccord_transport *transport;
  ccord_contrib *contrib;
  ccord_platform *platform;
  ccord_renderer *renderer;
  ccord_ui *ui;
  uint64_t now;
  bool ti_active;
  ccord_rect ti_rect;
  int ti_calls;
} fixture;

static fixture F;

static uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

static void host_clip_set(void *ud, const char *s) { (void)ud; (void)s; }
static void host_text_input(void *ud, bool active, float x, float y, float w, float h)
{
  (void)ud;
  F.ti_active = active;
  F.ti_rect = (ccord_rect){x, y, w, h};
  F.ti_calls++;
}
static void host_set_ext(void *ud, bool on) { (void)ud; (void)on; }
static bool host_open_url(void *ud, const char *url) { (void)ud; (void)url; return true; }
static bool host_open_file(void *ud, char *out, size_t cap) { (void)ud; (void)out; (void)cap; return false; }

static void frame(int n)
{
  for (int i = 0; i < n; i++) {
    F.now += 50;
    (void)ccord_session_pump(F.session);
    (void)ccord_bus_drain(F.bus);
    (void)ccord_ui_update(F.ui, F.now);
    ccord_render_begin(F.renderer, F.theme.c.bg_primary);
    ccord_ui_draw(F.ui, F.renderer);
    ccord_render_end(F.renderer);
  }
}

static void send(ccord_input_event ev) { (void)ccord_ui_handle_input(F.ui, &ev); }

static void mouse(ccord_input_type t, uint8_t button, float x, float y)
{
  send((ccord_input_event){.type = t, .button = button, .clicks = 1, .x = x, .y = y});
}

static void key(ccord_key k, uint8_t mods)
{
  send((ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = k, .mods = mods});
}

static void type_text(const char *s)
{
  size_t left = strlen(s);
  while (left > 0) {
    ccord_input_event ev = {.type = CCORD_IN_TEXT};
    size_t take = left < sizeof ev.text - 1u ? left : sizeof ev.text - 1u;
    while (take < left && take > 1 && ((uint8_t)s[take] & 0xC0u) == 0x80u) take--;
    memcpy(ev.text, s, take);
    send(ev);
    s += take;
    left -= take;
  }
}

static void preedit(const char *text, int32_t start, int32_t length)
{
  ccord_input_event ev = {.type = CCORD_EV_IME_PREEDIT, .ime_start = start, .ime_length = length};
  if (text) snprintf(ev.text, sizeof ev.text, "%s", text);
  send(ev);
}

static ccord_rect composer_rect(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  return ui_rect(l->composer);
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_ime_data"));
remove("ui_ime_data/settings.ini");
  remove("ui_ime_data/drafts.json");
  remove("ui_ime_data");
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK)
    return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 1, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session) return false;
  ccord_platform_config pc = {.title = "ui-ime-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .host = {.clipboard_set = host_clip_set, .text_input = host_text_input,
                              .set_extensions_enabled = host_set_ext, .open_url = host_open_url,
                              .open_file_dialog = host_open_file},
                     .fixed_now_unix_ms = 1793534400000LL + 30 * 60 * 1000};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  CT_CHECK(ccord_session_start(F.session, &err) == CCORD_OK);
  frame(3);
  return true;
}

static void teardown(void)
{
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
  ccord_contrib_destroy(F.contrib);
}

// ------------------------------------------------------------------ settings seam

static void test_settings_seam(void)
{
  ccord_ui_settings_debug d;
  ui_settings_m6_debug(F.ui, &d);
  CT_CHECK(strcmp(d.theme_id, "c-cord-dark") == 0);
  CT_CHECK(fabsf(d.zoom - 1.0f) < 0.001f);
  CT_CHECK(strcmp(d.locale, "en-US") == 0);
  CT_CHECK(d.gif_auto_play && d.animate_emoji);
  CT_CHECK(d.animate_stickers == 0);
  CT_CHECK(d.notify_sounds && d.hardware_accel);

  snprintf(F.ui->settings_theme_id, sizeof F.ui->settings_theme_id, "light");
  F.ui->settings_zoom = 1.25f;
  F.ui->settings_streamer_mode = true;
  F.ui->settings_developer_mode = true;
  snprintf(F.ui->settings_locale, sizeof F.ui->settings_locale, "de");
  F.ui->settings_explicit_content_filter = 1;
  F.ui->settings_animate_stickers = 2;
  F.ui->settings_gif_auto_play = false;
  F.ui->settings_animate_emoji = false;
  F.ui->settings_enable_tts_command = true;
  F.ui->settings_stream_notifications_enabled = true;
  F.ui->settings_default_guilds_restricted = true;
  F.ui->settings_allow_age_restricted = true;
  F.ui->settings_notify_sounds = false;
  F.ui->settings_hardware_accel = false;
  ui_settings_m6_debug(F.ui, &d);
  CT_CHECK(strcmp(d.theme_id, "light") == 0 && fabsf(d.zoom - 1.25f) < 0.001f);
  CT_CHECK(d.streamer_mode && d.developer_mode && !d.notify_sounds && !d.hardware_accel);
  CT_CHECK(strcmp(d.locale, "de") == 0 && d.explicit_content_filter == 1 && d.animate_stickers == 2);
  CT_CHECK(!d.gif_auto_play && !d.animate_emoji && d.enable_tts_command);
  CT_CHECK(d.stream_notifications_enabled && d.default_guilds_restricted && d.allow_age_restricted);

  // The public settings getter (settings.c) forwards these through the same seam.
  ccord_ui_settings_debug pub;
  ccord_ui_get_settings_debug(F.ui, &pub);
  CT_CHECK(strcmp(pub.theme_id, "light") == 0 && fabsf(pub.zoom - 1.25f) < 0.001f);
  CT_CHECK(pub.developer_mode && !pub.gif_auto_play && pub.animate_stickers == 2);
  CT_CHECK(strcmp(pub.locale, "de") == 0 && pub.stream_notifications_enabled);

  // Restore the defaults the other cases rely on.
  snprintf(F.ui->settings_theme_id, sizeof F.ui->settings_theme_id, "c-cord-dark");
  F.ui->settings_zoom = 1.0f;
  F.ui->settings_streamer_mode = false;
  F.ui->settings_developer_mode = false;
  snprintf(F.ui->settings_locale, sizeof F.ui->settings_locale, "en-US");
  F.ui->settings_explicit_content_filter = 0;
  F.ui->settings_animate_stickers = 0;
  F.ui->settings_gif_auto_play = true;
  F.ui->settings_animate_emoji = true;
  F.ui->settings_enable_tts_command = false;
  F.ui->settings_stream_notifications_enabled = false;
  F.ui->settings_default_guilds_restricted = false;
  F.ui->settings_allow_age_restricted = false;
  F.ui->settings_hardware_accel = true;
}

// ------------------------------------------------------------------ IME preedit

static void test_ime_storage_and_clamps(void)
{
  ccord_text_edit_clear(&F.ui->edit);

  preedit("にほ", 1, 1);
  CT_CHECK(strcmp(F.ui->ime_preedit, "にほ") == 0);
  CT_CHECK(F.ui->ime_cursor == 1 && F.ui->ime_sel_len == 1);

  // Cursor past the end clamps to the composition length; selection has no room left.
  preedit("ab", 99, 99);
  CT_CHECK(strcmp(F.ui->ime_preedit, "ab") == 0);
  CT_CHECK(F.ui->ime_cursor == 2 && F.ui->ime_sel_len == 0);

  // Selection clamps to the remaining codepoints.
  preedit("abc", 1, 99);
  CT_CHECK(F.ui->ime_cursor == 1 && F.ui->ime_sel_len == 2);

  // Negative offsets mean the start of the composition / no selection.
  preedit("abc", -5, -5);
  CT_CHECK(F.ui->ime_cursor == 0 && F.ui->ime_sel_len == 0);

  // Long ASCII input is cut to the bounded buffer (still NUL-terminated).
  char long_text[100];
  memset(long_text, 'a', sizeof long_text - 1u);
  long_text[sizeof long_text - 1u] = '\0';
  ui_ime_preedit_set(F.ui, long_text, 100, 100);
  CT_CHECK(strlen(F.ui->ime_preedit) == sizeof F.ui->ime_preedit - 1u);
  CT_CHECK(F.ui->ime_cursor == (int32_t)(sizeof F.ui->ime_preedit - 1u));
  CT_CHECK(F.ui->ime_sel_len == 0);

  // A cut mid-codepoint backs off to a whole codepoint.
  char multi[80];
  size_t mp = 0;
  for (int i = 0; i < 25; i++) { multi[mp++] = (char)0xC3; multi[mp++] = (char)0xA9; }   // 50 bytes of é
  for (int i = 0; i < 5; i++) { multi[mp++] = (char)0xE3; multi[mp++] = (char)0x81; multi[mp++] = (char)0x82; }   // 15 bytes of あ
  multi[mp] = '\0';
  ui_ime_preedit_set(F.ui, multi, 0, 0);
  size_t mlen = strlen(F.ui->ime_preedit);
  CT_CHECK(mlen == 62);   // the 63rd byte would split the final 3-byte codepoint
  CT_CHECK(ccord_utf8_valid(F.ui->ime_preedit, mlen));

  // Invalid UTF-8 clears instead of storing garbage.
  preedit("ok", 0, 0);
  send((ccord_input_event){.type = CCORD_EV_IME_PREEDIT, .text = {(char)0xFF, (char)0xFE, '\0'}});
  CT_CHECK(F.ui->ime_preedit[0] == '\0' && F.ui->ime_cursor == 0 && F.ui->ime_sel_len == 0);

  // Empty composition clears (including a nonzero cursor left by the previous one).
  preedit("xy", 2, 0);
  preedit("", 0, 0);
  CT_CHECK(F.ui->ime_preedit[0] == '\0' && F.ui->ime_cursor == 0 && F.ui->ime_sel_len == 0);
}

static void test_ime_clears(void)
{
  ccord_text_edit_clear(&F.ui->edit);

  // Commit: a text event inserts it (unchanged path) and drops the composition.
  preedit("ka", 0, 0);
  send((ccord_input_event){.type = CCORD_IN_TEXT, .text = "k"});
  CT_CHECK(F.ui->ime_preedit[0] == '\0' && F.ui->ime_cursor == 0 && F.ui->ime_sel_len == 0);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&F.ui->edit), "k") == 0);

  // Esc cancels the composition first.
  preedit("ka", 0, 0);
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(F.ui->ime_preedit[0] == '\0' && F.ui->ime_cursor == 0 && F.ui->ime_sel_len == 0);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&F.ui->edit), "k") == 0);   // the draft survives

  // Window blur cancels it.
  preedit("ka", 0, 0);
  send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = false});
  CT_CHECK(!F.ui->window_focused);
  CT_CHECK(F.ui->ime_preedit[0] == '\0' && F.ui->ime_cursor == 0 && F.ui->ime_sel_len == 0);
  send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  CT_CHECK(F.ui->window_focused);
  ccord_text_edit_clear(&F.ui->edit);
}

// ------------------------------------------------------------------ composer caret

static void test_composer_caret_rect(void)
{
  ccord_text_edit_clear(&F.ui->edit);
  frame(2);
  ccord_rect start = F.ui->text_area;
  ccord_rect comp = composer_rect();
  CT_CHECK(start.w <= 2.0f && start.h > 0.0f);
  CT_CHECK(start.x >= comp.x && start.x < comp.x + comp.w);
  CT_CHECK(start.y >= comp.y && start.y < comp.y + comp.h);

  type_text("hello");
  frame(2);
  ccord_rect caret = F.ui->text_area;
  CT_CHECK(caret.w <= 2.0f && caret.h > 0.0f);
  CT_CHECK(caret.x > start.x + 5.0f);   // the rect now follows the caret past the typed text
  CT_CHECK(fabsf(caret.y - start.y) < 2.0f);
  CT_CHECK(F.ti_active);
  CT_CHECK(fabsf(F.ti_rect.x - caret.x) < 0.5f && fabsf(F.ti_rect.y - caret.y) < 0.5f);
  CT_CHECK(fabsf(F.ti_rect.w - caret.w) < 0.5f && fabsf(F.ti_rect.h - caret.h) < 0.5f);

  // The preedit itself does not crash the draw and leaves the caret rect in place.
  preedit("にほ", 1, 1);
  frame(2);
  CT_CHECK(fabsf(F.ui->text_area.x - caret.x) < 0.5f);
  preedit("", 0, 0);
  ccord_text_edit_clear(&F.ui->edit);
}

// ------------------------------------------------------------------ GIF autoplay gate

// Same 3-frame animated GIF the server-icon animation test uses.
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

static const char *k_att_url = "https://cdn.discordapp.com/attachments/1/2/ime_pic.png";
static const char *k_thumb_url = "https://media.discordapp.net/attachments/1/2/ime_thumb.png";
static const char *k_img_url = "https://media.discordapp.net/attachments/1/2/ime_img.png";
static const char *k_gifv_url = "https://media.discordapp.net/attachments/1/2/ime_gifv.gif";

static ccord_rect hit_rect(const char *url, bool *found)
{
  *found = false;
  for (size_t i = 0; i < F.ui->nimg_hits; i++) {
    if (strcmp(F.ui->img_hits[i].urls[0], url) != 0) continue;
    *found = true;
    return F.ui->img_hits[i].rect;
  }
  return (ccord_rect){0, 0, 0, 0};
}

static size_t anim_frame(const char *url)
{
  size_t cur = 0, count = 0;
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, url, &cur, &count));
  CT_CHECK(count == 3);
  return cur;
}

// Renders `n` frames and returns the highest frame index seen, so a check cannot
// pass just because the 3-frame GIF wrapped back to frame 0 on the last frame.
static size_t max_anim_frame(const char *url, int n)
{
  size_t top = 0;
  for (int i = 0; i < n; i++) {
    frame(1);
    size_t cur = anim_frame(url);
    if (cur > top) top = cur;
  }
  return top;
}

static void append_media_message(void)
{
  ccord_error err;
  ccord_id ch = ccord_store_selected_channel(F.store);
  CT_CHECK(ch != CCORD_ID_NONE);
  size_t mcount = ccord_store_message_count(F.store, ch);
  const ccord_message *last = mcount ? ccord_store_message_at(F.store, ch, mcount - 1) : nullptr;
  ccord_attachment att = {.id = 1, .filename = "ime_pic.png", .content_type = "image/png", .size = 512,
                          .url = k_att_url, .width = 120, .height = 90};
  ccord_embed embs[2] = {
    {.title = "Shot", .description = "Preview", .url = "https://example.test/post",
     .thumbnail = {.url = k_thumb_url, .width = 60, .height = 60},
     .image = {.url = k_img_url, .width = 140, .height = 90}},
    {.kind = CCORD_EMBED_GIFV, .url = "https://example.test/gifv",
     .image = {.url = k_gifv_url, .width = 120, .height = 90}},
  };
  ccord_message_rich rich = {.present = CCORD_RICH_ALL, .attachments = &att, .attachment_count = 1,
                             .embeds = embs, .embed_count = 2};
  ccord_message msg = {
    .id = last ? last->id + 1u : 1u,
    .channel_id = ch,
    .author_id = ccord_store_self_id(F.store),
    .timestamp_ms = last ? last->timestamp_ms + 1 : (int64_t)F.now,
    .content = "media",
    .rich = &rich,
  };
  CT_CHECK(ccord_store_append_message(F.store, &msg, &err) == CCORD_OK);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  CT_CHECK(ccord_render_upload_image(F.renderer, k_att_url, k_rgb_gif, sizeof k_rgb_gif, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_render_upload_image(F.renderer, k_thumb_url, k_rgb_gif, sizeof k_rgb_gif, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_render_upload_image(F.renderer, k_img_url, k_rgb_gif, sizeof k_rgb_gif, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_render_upload_image(F.renderer, k_gifv_url, k_rgb_gif, sizeof k_rgb_gif, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(anim_frame(k_att_url) == 0);
}

static void test_gif_autoplay_gate(void)
{
  append_media_message();
  // No settings object yet: autoplay defaults ON.
  CT_CHECK(ccord_store_user_settings(F.store) == nullptr);
  mouse(CCORD_IN_MOUSE_MOVE, 0, 1270.0f, 40.0f);
  frame(6);
  bool found = false;
  ccord_rect att = hit_rect(k_att_url, &found);
  CT_CHECK(found && att.w > 0.0f);
  ccord_rect thumb = hit_rect(k_thumb_url, &found);
  CT_CHECK(found && thumb.w > 0.0f);
  ccord_rect img = hit_rect(k_img_url, &found);
  CT_CHECK(found && img.w > 0.0f);
  ccord_rect gifv = hit_rect(k_gifv_url, &found);
  CT_CHECK(found && gifv.w > 0.0f);
  CT_CHECK(max_anim_frame(k_att_url, 6) >= 1);
  CT_CHECK(max_anim_frame(k_thumb_url, 6) >= 1);
  CT_CHECK(max_anim_frame(k_img_url, 6) >= 1);
  CT_CHECK(max_anim_frame(k_gifv_url, 6) >= 1);

  // Autoplay off, pointer away: every chat GIF pins frame 0.
  ccord_user_settings off = {0};
  off.gif_auto_play = false;
  snprintf(off.locale, sizeof off.locale, "en-US");
  ccord_error err;
  CT_CHECK(ccord_store_set_user_settings(F.store, &off, &err) == CCORD_OK);
  mouse(CCORD_IN_MOUSE_MOVE, 0, 1270.0f, 40.0f);
  CT_CHECK(max_anim_frame(k_att_url, 6) == 0);
  CT_CHECK(max_anim_frame(k_thumb_url, 6) == 0);
  CT_CHECK(max_anim_frame(k_img_url, 6) == 0);
  CT_CHECK(max_anim_frame(k_gifv_url, 6) == 0);

  // Hovering one tile plays only that tile (Discord behavior).
  mouse(CCORD_IN_MOUSE_MOVE, 0, img.x + img.w * 0.5f, img.y + img.h * 0.5f);
  CT_CHECK(max_anim_frame(k_img_url, 6) >= 1);
  CT_CHECK(max_anim_frame(k_att_url, 6) == 0);
  CT_CHECK(max_anim_frame(k_thumb_url, 6) == 0);
  CT_CHECK(max_anim_frame(k_gifv_url, 6) == 0);

  // The pointer must actually be in the window: a leave resets playback.
  send((ccord_input_event){.type = CCORD_IN_MOUSE_LEAVE});
  CT_CHECK(max_anim_frame(k_img_url, 6) == 0);

  mouse(CCORD_IN_MOUSE_MOVE, 0, att.x + att.w * 0.5f, att.y + att.h * 0.5f);
  CT_CHECK(max_anim_frame(k_att_url, 6) >= 1);
  mouse(CCORD_IN_MOUSE_MOVE, 0, thumb.x + thumb.w * 0.5f, thumb.y + thumb.h * 0.5f);
  CT_CHECK(max_anim_frame(k_thumb_url, 6) >= 1);
  mouse(CCORD_IN_MOUSE_MOVE, 0, gifv.x + gifv.w * 0.5f, gifv.y + gifv.h * 0.5f);
  CT_CHECK(max_anim_frame(k_gifv_url, 6) >= 1);

  // Turning autoplay back on plays everything without hover.
  ccord_user_settings on = off;
  on.gif_auto_play = true;
  CT_CHECK(ccord_store_set_user_settings(F.store, &on, &err) == CCORD_OK);
  mouse(CCORD_IN_MOUSE_MOVE, 0, 1270.0f, 40.0f);
  CT_CHECK(max_anim_frame(k_att_url, 6) >= 1);
  CT_CHECK(max_anim_frame(k_thumb_url, 6) >= 1);
  CT_CHECK(max_anim_frame(k_img_url, 6) >= 1);
  CT_CHECK(max_anim_frame(k_gifv_url, 6) >= 1);
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_ime")) return 77;
  test_settings_seam();
  test_ime_storage_and_clamps();
  test_ime_clears();
  test_composer_caret_rect();
  test_gif_autoplay_gate();
  teardown();
  return CT_RESULT("test_ui_ime");
}
