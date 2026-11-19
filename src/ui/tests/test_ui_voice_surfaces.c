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

// M5 UI voice surfaces: DM calls, per-user volume / local mute, voice status, stage speakers /
// topic / request-to-speak, soundboard play gate, Go Live cards, DAVE badge + verification modal and
// the RTC debug popover. Real store + mock transport + session + renderer; every Discord change is a
// session intent that only lands in the store when the transport answers (m5-ui-surfaces).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/paths.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/session/session.h>
#include <ccord/store/roles.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "../ui_voice_surfaces.h"

#include "ccord_test.h"

#define HQ 2001u
#define ME 1001u
#define ADA 1002u
#define BJORN 1003u
#define CHEN 1004u
#define HQ_VOICE 3121u
#define HQ_STAGE 3123u

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
} fixture;

static fixture F;

static uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

static void host_clip_set(void *ud, const char *s) { (void)ud; (void)s; }
static void host_text_input(void *ud, bool a, float x, float y, float w, float h)
{
  (void)ud; (void)a; (void)x; (void)y; (void)w; (void)h;
}
static void host_set_ext(void *ud, bool on) { (void)ud; (void)on; }
// The host URL opener records what the UI asked it to open (Activities: Open in browser).
static char g_opened_url[512];
static int g_open_count;
static bool host_open_url(void *ud, const char *url)
{
  (void)ud;
  snprintf(g_opened_url, sizeof g_opened_url, "%s", url);
  g_open_count++;
  return true;
}
static bool host_open_file(void *ud, char *out, size_t cap) { (void)ud; (void)out; (void)cap; return false; }
static bool g_fullscreen_state;
static int g_fullscreen_calls;
static void host_set_fullscreen(void *ud, bool on)
{
  (void)ud;
  g_fullscreen_state = on;
  g_fullscreen_calls++;
}

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

static void click(float x, float y)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  frame(2);
}

static void click_rect(ccord_rect r)
{
  click(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
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

static ui_voice_surfaces_debug vsdbg(void)
{
  ui_voice_surfaces_debug d;
  ui_voice_surfaces_get_debug(F.ui, &d);
  return d;
}

// Opt-in screenshots for looking at a change (CCORD_SHOT_DIR=<dir>); a no-op otherwise.
static void shot(const char *name)
{
  const char *dir = getenv("CCORD_SHOT_DIR");
  if (!dir || !dir[0]) return;
  char path[512];
  snprintf(path, sizeof path, "%s/%s.png", dir, name);
  ccord_error err = {0};
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  (void)ccord_render_screenshot(F.renderer, path, &err);
  ccord_render_end(F.renderer);
}

static ui_activity_debug actdbg(void)
{
  ui_activity_debug d;
  ui_activity_get_debug(F.ui, &d);
  return d;
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_vs_data"));
remove("ui_vs_data/settings.ini");
  remove("ui_vs_data/drafts.json");
  remove("ui_vs_data");
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK)
    return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 10, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session) return false;
  ccord_platform_config pc = {.title = "ui-vs-test", .width = 1280, .height = 800, .headless = true};
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
                              .open_file_dialog = host_open_file, .set_fullscreen = host_set_fullscreen},
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

// Join a voice/stage channel through the session and open its stage view.
static void join_and_open(ccord_id channel)
{
  ccord_error err;
  CT_CHECK(ccord_session_join_voice(F.session, HQ, channel, &err) == CCORD_OK);
  frame(3);
  F.ui->stage_channel = channel;
  F.ui->dirty = true;
  frame(2);
}

static void leave_voice_and_close(void)
{
  ccord_error err;
  const ccord_voice_state *v = ui_self_voice(F.ui);
  if (v && v->channel_id != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(F.store, v->channel_id);
    (void)ccord_session_leave_voice(F.session, ch ? ch->guild_id : HQ, &err);
    frame(3);
  }
  F.ui->stage_channel = CCORD_ID_NONE;
  F.ui->dirty = true;
  frame(2);
}

// Centre y of the sidebar row for `channel` (or one of its participants); -1 when it is not listed.
static float channel_row_y(ccord_id channel, ccord_id participant)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ccord_id g = ccord_store_selected_guild(F.store);
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    const chan_row *row = &F.ui->crow[i];
    if (row->is_category || row->is_relationship || row->is_thread || row->dm_nav != DM_NAV_NONE) continue;
    const ccord_channel *ch = ccord_store_channel_at(F.store, g, row->index);
    if (!ch || ch->id != channel) continue;
    if (participant != CCORD_ID_NONE && (!row->is_voice_user || row->user_id != participant)) continue;
    return l->channel_scroll.y + row->y - F.ui->chan_scroll + row->h * 0.5f;
  }
  return -1.0f;
}

// ---- DM calls: ring banner, accept, call bar, leave, header button ------------------------------

static void test_dm_call(void)
{
  ccord_error err;
  CT_CHECK(ccord_store_selected_guild(F.store) == HQ);
  // Switch to the DM pseudo-guild so a DM channel can be selected.
  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  frame(2);

  CT_CHECK(!vsdbg().ring_visible && !vsdbg().call_bar_visible);
  ccord_id call_ch = 0;
  CT_CHECK(ccord_mock_push_incoming_call(F.transport, ADA, &call_ch, &err) == CCORD_OK);
  frame(3);
  ui_voice_surfaces_debug d = vsdbg();
  CT_CHECK(d.ring_visible && d.ring_accept.w > 0.0f && d.ring_decline.w > 0.0f);
  CT_CHECK(ccord_session_incoming_call(F.session) != nullptr);

  // Accept: the store only gains the voice state once the transport answers.
  click_rect(d.ring_accept);
  frame(3);
  const ccord_voice_state *me = ccord_store_voice_user(F.store, CCORD_DM_GUILD_ID, ME);
  CT_CHECK(me && me->channel_id == call_ch);
  d = vsdbg();
  CT_CHECK(!d.ring_visible);
  CT_CHECK(d.call_bar_visible && d.call_bar_channel == call_ch && d.call_bar_leave.w > 0.0f);

  // Leave from the call bar.
  click_rect(d.call_bar_leave);
  frame(3);
  CT_CHECK(ccord_store_voice_user(F.store, CCORD_DM_GUILD_ID, ME) == nullptr);
  CT_CHECK(!vsdbg().call_bar_visible);

  // Header button: select the DM channel, "Start Call" rings the recipient; then "Leave Call".
  CT_CHECK(ccord_session_select_channel(F.session, call_ch, &err) == CCORD_OK);
  frame(2);
  d = vsdbg();
  CT_CHECK(d.header_call_visible && d.header_call.w > 0.0f);
  click_rect(d.header_call);
  frame(3);
  me = ccord_store_voice_user(F.store, CCORD_DM_GUILD_ID, ME);
  CT_CHECK(me && me->channel_id == call_ch);
  d = vsdbg();
  CT_CHECK(d.header_call_visible);   // now a Leave Call button
  click_rect(d.header_call);
  frame(3);
  CT_CHECK(ccord_store_voice_user(F.store, CCORD_DM_GUILD_ID, ME) == nullptr);

  // Decline a fresh ring.
  CT_CHECK(ccord_mock_push_incoming_call(F.transport, ADA, &call_ch, &err) == CCORD_OK);
  frame(3);
  d = vsdbg();
  CT_CHECK(d.ring_visible);
  click_rect(d.ring_decline);
  frame(3);
  CT_CHECK(!vsdbg().ring_visible);
  CT_CHECK(ccord_store_voice_user(F.store, CCORD_DM_GUILD_ID, ME) == nullptr);

  CT_CHECK(ccord_session_select_guild(F.session, HQ, &err) == CCORD_OK);
  frame(2);
}

// ---- per-user volume slider (0..200) + local mute -----------------------------------------------

static void test_volume_and_mute(void)
{
  join_and_open(HQ_VOICE);
  ui_voice_surfaces_debug d = vsdbg();
  CT_CHECK(d.stage.ctl_count >= 1);
  // Find Ada's controls.
  int ai = -1;
  for (size_t i = 0; i < d.stage.ctl_count; i++) {
    if (d.stage.ctl[i].user_id == ADA) { ai = (int)i; break; }
  }
  CT_CHECK(ai >= 0);
  if (ai < 0) return;
  ccord_rect slider = d.stage.ctl[ai].slider;
  ccord_rect mute = d.stage.ctl[ai].mute;
  CT_CHECK(slider.w > 0.0f && mute.w > 0.0f);
  CT_CHECK(ccord_store_user_volume(F.store, ADA) == 100u);

  float cy = slider.y + slider.h * 0.5f;
  click(slider.x + 0.1f, cy);              // 0 %
  CT_CHECK(ccord_store_user_volume(F.store, ADA) == 0u);
  click(slider.x + slider.w * 0.5f, cy);   // 100 %
  CT_CHECK(ccord_store_user_volume(F.store, ADA) == 100u);
  click(slider.x + slider.w * 0.75f, cy);  // 150 %
  CT_CHECK(ccord_store_user_volume(F.store, ADA) == 150u);
  click(slider.x + slider.w - 0.1f, cy);   // 200 %
  CT_CHECK(ccord_store_user_volume(F.store, ADA) == 200u);
  CT_CHECK(ccord_mock_user_volume(F.transport, ADA) == 200u);

  // Local mute is a client-only preference.
  CT_CHECK(!ccord_store_user_local_mute(F.store, ADA));
  click_rect(mute);
  CT_CHECK(ccord_store_user_local_mute(F.store, ADA));
  click_rect(mute);
  CT_CHECK(!ccord_store_user_local_mute(F.store, ADA));
  // Mute does not reset the gain.
  CT_CHECK(ccord_store_user_volume(F.store, ADA) == 200u);
  leave_voice_and_close();
}

// ---- voice status prompt ------------------------------------------------------------------------

static void test_voice_status(void)
{
  join_and_open(HQ_VOICE);
  ui_voice_surfaces_debug d = vsdbg();
  CT_CHECK(d.stage.status_edit.w > 0.0f);
  CT_CHECK(ccord_store_voice_status(F.store, HQ_VOICE) == nullptr);
  click_rect(d.stage.status_edit);
  frame(1);
  d = vsdbg();
  CT_CHECK(d.prompt_open && d.prompt_kind == UI_VOICE_PROMPT_STATUS && d.prompt_channel == HQ_VOICE);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_EDIT_CHANNEL);
  type_text("Chilling in voiceX");
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
  d = vsdbg();
  CT_CHECK(strcmp(d.prompt_text, "Chilling in voice") == 0);
  key(CCORD_KEY_RETURN, 0);   // Return submits just like the Save button
  frame(3);
  const char *status = ccord_store_voice_status(F.store, HQ_VOICE);
  CT_CHECK(status && strcmp(status, "Chilling in voice") == 0);
  CT_CHECK(!vsdbg().prompt_open && F.ui->modal_kind == UI_MODAL_NONE);

  // Escape cancels without touching the store.
  d = vsdbg();
  click_rect(d.stage.status_edit);
  frame(1);
  type_text("throwaway");
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!vsdbg().prompt_open);
  status = ccord_store_voice_status(F.store, HQ_VOICE);
  CT_CHECK(status && strcmp(status, "Chilling in voice") == 0);

  // Clicking outside the prompt box closes it without submitting.
  d = vsdbg();
  click_rect(d.stage.status_edit);
  frame(1);
  CT_CHECK(vsdbg().prompt_open);
  click(5.0f, 5.0f);
  frame(1);
  CT_CHECK(!vsdbg().prompt_open && F.ui->modal_kind == UI_MODAL_NONE);
  status = ccord_store_voice_status(F.store, HQ_VOICE);
  CT_CHECK(status && strcmp(status, "Chilling in voice") == 0);
  leave_voice_and_close();
}

// ---- stage: speakers / audience, topic, request-to-speak, invite to speak -----------------------

static void test_stage(void)
{
  join_and_open(HQ_STAGE);
  ui_voice_surfaces_debug d = vsdbg();
  CT_CHECK(d.stage.speaker_count >= 1);    // Bjorn
  CT_CHECK(d.stage.audience_count >= 2);   // Chen + self
  CT_CHECK(d.stage.topic_edit.w > 0.0f);
  CT_CHECK(d.stage.request_speak.w > 0.0f && !d.stage.request_speak_cancel);

  // Topic edit through the prompt.
  const ccord_stage_instance *si = ccord_store_stage_instance(F.store, HQ_STAGE);
  CT_CHECK(si && strcmp(si->topic, "Community Town Hall") == 0);
  click_rect(d.stage.topic_edit);
  frame(1);
  d = vsdbg();
  CT_CHECK(d.prompt_open && d.prompt_kind == UI_VOICE_PROMPT_TOPIC);
  CT_CHECK(strcmp(d.prompt_text, "Community Town Hall") == 0);   // prefilled
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("New topic");
  click_rect(d.prompt_ok);
  frame(3);
  si = ccord_store_stage_instance(F.store, HQ_STAGE);
  CT_CHECK(si && strcmp(si->topic, "New topic") == 0);

  // Request to speak, then cancel.
  d = vsdbg();
  CT_CHECK(d.stage.request_speak.w > 0.0f && !d.stage.request_speak_cancel);
  click_rect(d.stage.request_speak);
  frame(3);
  const ccord_voice_state *me = ccord_store_voice_user(F.store, HQ, ME);
  CT_CHECK(me && me->request_to_speak_timestamp > 0 && me->suppress);
  d = vsdbg();
  CT_CHECK(d.stage.request_speak_cancel);
  click_rect(d.stage.request_speak);
  frame(3);
  me = ccord_store_voice_user(F.store, HQ, ME);
  CT_CHECK(me && me->request_to_speak_timestamp == 0);

  // Invite Chen (audience) to speak via the tile mod button.
  d = vsdbg();
  int ci = -1;
  for (size_t i = 0; i < d.stage.ctl_count; i++) {
    if (d.stage.ctl[i].user_id == CHEN && d.stage.ctl[i].mod.w > 0.0f) { ci = (int)i; break; }
  }
  CT_CHECK(ci >= 0);
  if (ci >= 0) {
    CT_CHECK(d.stage.ctl[ci].mod_is_invite);
    click_rect(d.stage.ctl[ci].mod);
    frame(3);
    const ccord_voice_state *chen = ccord_store_voice_user(F.store, HQ, CHEN);
    CT_CHECK(chen && !chen->suppress);
  }
  leave_voice_and_close();
}

// ---- stage channel sidebar row: participant children, click to join / reopen ---------------------

static void test_stage_sidebar(void)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float rx = l->channel_scroll.x + 80.0f;
  CT_CHECK(ccord_store_selected_guild(F.store) == HQ);
  const ccord_channel *st = ccord_store_channel(F.store, HQ_STAGE);
  CT_CHECK(st && st->kind == CCORD_CHANNEL_STAGE);
  CT_CHECK(ccord_store_voice_user(F.store, HQ, ME) == nullptr);
  CT_CHECK(F.ui->stage_channel == CCORD_ID_NONE);

  // The stage channel is listed, with its connected participants as child rows.
  float row_y = channel_row_y(HQ_STAGE, CCORD_ID_NONE);
  CT_CHECK(row_y > 0.0f);
  CT_CHECK(channel_row_y(HQ_STAGE, BJORN) > 0.0f);
  CT_CHECK(channel_row_y(HQ_STAGE, CHEN) > 0.0f);
  if (row_y <= 0.0f) return;

  // Clicking the stage row joins it through the session and opens its stage view on its own.
  click(rx, row_y);
  frame(3);
  const ccord_voice_state *me = ccord_store_voice_user(F.store, HQ, ME);
  CT_CHECK(me && me->channel_id == HQ_STAGE);
  CT_CHECK(F.ui->stage_channel == HQ_STAGE);
  CT_CHECK(F.ui->stage_armed);
  CT_CHECK(vsdbg().stage.speaker_count >= 1);
  CT_CHECK(vsdbg().stage.audience_count >= 1);

  // Closing the stage view and clicking the joined row again reopens it.
  ui_clear_voice_stage(F.ui);
  frame(2);
  CT_CHECK(F.ui->stage_channel == CCORD_ID_NONE);
  row_y = channel_row_y(HQ_STAGE, CCORD_ID_NONE);
  CT_CHECK(row_y > 0.0f);
  click(rx, row_y);
  frame(2);
  CT_CHECK(F.ui->stage_channel == HQ_STAGE);
  CT_CHECK(F.ui->stage_armed);

  // A participant row reopens the stage view too.
  ui_clear_voice_stage(F.ui);
  frame(2);
  float part_y = channel_row_y(HQ_STAGE, BJORN);
  CT_CHECK(part_y > 0.0f);
  if (part_y > 0.0f) {
    click(rx, part_y);
    frame(2);
    CT_CHECK(F.ui->stage_channel == HQ_STAGE);
  }

  // Left voice, a participant row joins the stage from scratch.
  leave_voice_and_close();
  CT_CHECK(ccord_store_voice_user(F.store, HQ, ME) == nullptr);
  part_y = channel_row_y(HQ_STAGE, BJORN);
  CT_CHECK(part_y > 0.0f);
  if (part_y > 0.0f) {
    click(rx, part_y);
    frame(3);
    me = ccord_store_voice_user(F.store, HQ, ME);
    CT_CHECK(me && me->channel_id == HQ_STAGE);
    CT_CHECK(F.ui->stage_channel == HQ_STAGE);
  }
  leave_voice_and_close();
}

// ---- soundboard: voice gate + governed playback -------------------------------------------------

static void test_soundboard(void)
{
  ccord_error err;
  // Not in voice: the picker lists sounds but playback is refused by the session.
  ui_expr_picker_open(F.ui, 3 /* Soundboard */, ccord_store_selected_channel(F.store), CCORD_ID_NONE);
  frame(2);
  CT_CHECK(F.ui->emoji_picker_open);
  ccord_rect airhorn = ui_expr_soundboard_hit(F.ui, 6501u);
  CT_CHECK(airhorn.w > 0.0f);
  if (airhorn.w > 0.0f) {
    click_rect(airhorn);
    frame(2);
    CT_CHECK(!ccord_store_user_speaking(F.store, ME));
  }
  ui_expr_picker_close(F.ui);
  frame(1);

  // In voice: playback goes through and the mock makes self speak.
  CT_CHECK(ccord_session_join_voice(F.session, HQ, HQ_VOICE, &err) == CCORD_OK);
  frame(3);
  ui_expr_picker_open(F.ui, 3 /* Soundboard */, ccord_store_selected_channel(F.store), CCORD_ID_NONE);
  frame(2);
  // Search narrows the sound list to matching names.
  type_text("air");
  frame(1);
  CT_CHECK(ui_expr_soundboard_hit(F.ui, 6501u).w > 0.0f);
  CT_CHECK(ui_expr_soundboard_hit(F.ui, 6502u).w == 0.0f);
  CT_CHECK(ui_expr_soundboard_hit(F.ui, 6503u).w == 0.0f);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_DELETE, 0);
  frame(1);
  airhorn = ui_expr_soundboard_hit(F.ui, 6501u);
  CT_CHECK(airhorn.w > 0.0f);
  if (airhorn.w > 0.0f) {
    click_rect(airhorn);
    // Playback is proven by the deterministic airhorn PCM (85 % of 3000 = 2550), not just a flag.
    int16_t pcm[960 * 2];
    size_t got = ccord_session_pull_voice_pcm(F.session, pcm, 960);
    CT_CHECK(got == 960 && pcm[0] == 2550 && pcm[1] == 2550);
    CT_CHECK(ccord_store_user_speaking(F.store, ME));   // speaking window is ~200 ms; check promptly
  }
  ui_expr_picker_close(F.ui);
  leave_voice_and_close();
}

// ---- Go Live broadcast cards --------------------------------------------------------------------

static void test_golive_cards(void)
{
  ccord_error err;
  join_and_open(HQ_VOICE);
  // Ada is seeded streaming in HQ voice; watching her creates a broadcast row + card.
  CT_CHECK(ccord_session_watch_stream(F.session, HQ, HQ_VOICE, ADA, &err) == CCORD_OK);
  frame(3);
  ui_voice_surfaces_debug d = vsdbg();
  CT_CHECK(d.stage.stream_cards >= 1);
  int ai = -1;
  for (size_t i = 0; i < d.stage.stream_cards; i++) {
    if (d.stage.card[i].user_id == ADA) { ai = (int)i; break; }
  }
  CT_CHECK(ai >= 0);
  if (ai >= 0) {
    CT_CHECK(d.stage.card[ai].watching);
    CT_CHECK(d.stage.card[ai].watch.w > 0.0f);
    click_rect(d.stage.card[ai].watch);   // stop watching
    frame(3);
    const ccord_watched_stream *ws = ccord_session_watched_stream(F.session);
    CT_CHECK(!ws || !ws->active);
  }
  leave_voice_and_close();
}

// ---- Go Live stage: honest STARTING / CONNECTING vs LIVE + real frame dimensions -----------------

static void test_golive_status(void)
{
  ccord_error err;
  join_and_open(HQ_VOICE);
  ccord_render_evict_texture(F.renderer, "__video_stream__");
  frame(2);
  CT_CHECK(vsdbg().stage.stream_badge[0] == '\0');   // no stream player while nothing is active

  // Self stream: the store knows the stream exists, but no encoded preview frame was uploaded yet.
  ccord_stream_config cfg = {.encoder = CCORD_STREAM_ENCODER_AUTO,
                             .codec = CCORD_STREAM_CODEC_H264,
                             .framework = CCORD_STREAM_FRAMEWORK_AUTO,
                             .resolution = CCORD_STREAM_RES_1080P,
                             .fps = CCORD_STREAM_FPS_60};
  CT_CHECK(ccord_session_start_stream(F.session, HQ, HQ_VOICE, &cfg, &err) == CCORD_OK);
  frame(3);
  const ccord_stream_state *st = ccord_store_stream(F.store);
  CT_CHECK(st && st->active);
  ui_voice_surfaces_debug d = vsdbg();
  CT_CHECK(strcmp(d.stage.stream_badge, "STARTING") == 0);
  CT_CHECK(strcmp(d.stage.stream_placeholder, "Starting capture & encoder...") == 0);
  CT_CHECK(!d.stage.stream_frame_present);

  // A real encoded preview frame arrives: LIVE with the texture's real dimensions.
  uint8_t self_px[160 * 90 * 4];
  memset(self_px, 0x55, sizeof self_px);
  CT_CHECK(ccord_render_upload_rgba(F.renderer, "__video_stream__", self_px, 160, 90, &err) == CCORD_OK);
  frame(2);
  d = vsdbg();
  CT_CHECK(strcmp(d.stage.stream_badge, "LIVE") == 0);
  CT_CHECK(d.stage.stream_frame_present);
  CT_CHECK(d.stage.stream_frame_w == 160 && d.stage.stream_frame_h == 90);
  CT_CHECK(d.stage.stream_placeholder[0] == '\0');

  // Self-stream Pause / Resume button toggles st->paused and badge without stopping the stream.
  CT_CHECK(F.ui->stream_stage_pause_rect.w > 0.0f);
  click_rect(F.ui->stream_stage_pause_rect);   // Pause
  frame(3);
  st = ccord_store_stream(F.store);
  CT_CHECK(st && st->active && st->paused);
  CT_CHECK(strcmp(vsdbg().stage.stream_badge, "PAUSED") == 0);
  click_rect(F.ui->stream_stage_pause_rect);   // Resume
  frame(3);
  st = ccord_store_stream(F.store);
  CT_CHECK(st && st->active && !st->paused);
  CT_CHECK(strcmp(vsdbg().stage.stream_badge, "LIVE") == 0);

  // Hide Preview: still LIVE even with no preview texture, because the user chose not to watch it.
  ccord_render_evict_texture(F.renderer, "__video_stream__");
  frame(2);
  CT_CHECK(strcmp(vsdbg().stage.stream_badge, "STARTING") == 0);
  click_rect(F.ui->stream_stage_btn1_rect);   // Hide Preview
  frame(2);
  CT_CHECK(strcmp(vsdbg().stage.stream_badge, "LIVE") == 0);

  // Stop Streaming must stop only the stream; the voice call keeps running.
  CT_CHECK(ccord_store_voice_user(F.store, HQ, ME) != nullptr);
  click_rect(F.ui->stream_stage_btn2_rect);   // Stop Streaming
  frame(3);
  CT_CHECK(!ccord_store_stream_active(F.store));
  const ccord_voice_state *v = ccord_store_voice_user(F.store, HQ, ME);
  CT_CHECK(v && v->channel_id == HQ_VOICE);

  // Watching a remote stream: the store has no decoded frame yet, so CONNECTING, and the subtitle is
  // the honest placeholder instead of a fabricated "1080p 60 FPS".
  ccord_render_evict_texture(F.renderer, "__video_stream__");
  CT_CHECK(ccord_session_watch_stream(F.session, HQ, HQ_VOICE, ADA, &err) == CCORD_OK);
  frame(3);
  const ccord_watched_stream *ws = ccord_session_watched_stream(F.session);
  CT_CHECK(ws && ws->active && !ws->paused);
  d = vsdbg();
  CT_CHECK(strcmp(d.stage.stream_badge, "CONNECTING") == 0);
  CT_CHECK(strcmp(d.stage.stream_placeholder, "Connecting to stream video...") == 0);
  CT_CHECK(strstr(d.stage.stream_title, "Screen Share") != nullptr);
  CT_CHECK(strstr(d.stage.stream_title, "1080p 60 FPS") == nullptr);

  // Pause is surfaced before any frame arrives, and resuming goes back to CONNECTING.
  CT_CHECK(ccord_session_set_stream_paused(F.session, true, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(strcmp(vsdbg().stage.stream_badge, "PAUSED") == 0);
  CT_CHECK(ccord_session_set_stream_paused(F.session, false, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(strcmp(vsdbg().stage.stream_badge, "CONNECTING") == 0);

  // The first decoded remote frame turns it LIVE and shows its real dimensions.
  uint8_t remote_px[320 * 180 * 4];
  memset(remote_px, 0x33, sizeof remote_px);
  CT_CHECK(ccord_render_upload_rgba(F.renderer, "__video_stream__", remote_px, 320, 180, &err) == CCORD_OK);
  frame(2);
  d = vsdbg();
  CT_CHECK(strcmp(d.stage.stream_badge, "LIVE") == 0);
  CT_CHECK(d.stage.stream_frame_w == 320 && d.stage.stream_frame_h == 180);
  CT_CHECK(strstr(d.stage.stream_title, "320x180") != nullptr);
  CT_CHECK(strstr(d.stage.stream_title, "1080p 60 FPS") == nullptr);

  // Stop Watching also leaves the voice call untouched.
  click_rect(F.ui->stream_stage_btn2_rect);   // Stop Watching
  frame(3);
  ws = ccord_session_watched_stream(F.session);
  CT_CHECK(!ws || !ws->active);
  v = ccord_store_voice_user(F.store, HQ, ME);
  CT_CHECK(v && v->channel_id == HQ_VOICE);
  leave_voice_and_close();
}

// ---- side-by-side Stage Text Chat: split layout, read/send without closing the stage ------------

static void test_stage_text_chat(void)
{
  ccord_error err;
  join_and_open(HQ_STAGE);
  CT_CHECK(F.ui->stage_channel == HQ_STAGE && !F.ui->stage_chat_open);
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  CT_CHECK(!l->stage_split);
  ccord_ui_debug d;
  ccord_ui_get_debug(F.ui, &d);
  CT_CHECK(d.voice_stage_open && !d.stage_chat_open);
  const ccord_id text_before = ccord_store_selected_channel(F.store);
  const ccord_channel *tb = ccord_store_channel(F.store, text_before);
  CT_CHECK(tb && tb->kind == CCORD_CHANNEL_TEXT);

  // Text Chat opens the side-by-side pane; the stage and the voice call stay open.
  click_rect(ui_stage_chat_btn(F.ui));
  frame(3);
  CT_CHECK(F.ui->stage_channel == HQ_STAGE && F.ui->stage_armed && F.ui->stage_chat_open);
  const ccord_voice_state *me = ccord_store_voice_user(F.store, HQ, ME);
  CT_CHECK(me && me->channel_id == HQ_STAGE);
  l = ccord_ui_get_layout(F.ui);
  CT_CHECK(l->stage_split);
  CT_CHECK(l->stage_pane.w >= 280.0f && l->stage_pane.h == 800.0f);
  // The chat pane sits right of the stage pane; header/timeline/composer share its column.
  CT_CHECK(l->message_area.x == l->stage_pane.x + l->stage_pane.w);
  CT_CHECK(l->message_area.w >= 320.0f);
  CT_CHECK(l->chat_header.x == l->message_area.x);
  CT_CHECK(l->composer.x == l->message_area.x && l->composer.w == l->message_area.w);
  CT_CHECK(!l->member_list_visible);
  ccord_ui_get_debug(F.ui, &d);
  CT_CHECK(d.stage_chat_open && d.stage_pane.w > 0.0f);
  ccord_rect btn = ui_stage_chat_btn(F.ui);
  CT_CHECK(btn.x >= l->stage_pane.x && btn.x + btn.w <= l->stage_pane.x + l->stage_pane.w);
  // The pane binds the stage channel's own chat (a stage channel carries messages), and the guild's remembered text channel
  // is untouched by it.
  CT_CHECK(d.stage_chat_channel == HQ_STAGE);
  const ccord_channel *bound = ccord_store_channel(F.store, d.stage_chat_channel);
  CT_CHECK(bound && bound->kind == CCORD_CHANNEL_STAGE && bound->guild_id == HQ);
  CT_CHECK(ccord_store_selected_channel(F.store) == HQ_STAGE);

  // The mock seeds no messages in voice/stage channels: the chat starts empty, then fills through the session.
  CT_CHECK(ccord_store_message_count(F.store, HQ_STAGE) == 0);
  for (int i = 0; i < 40; i++) {
    char line[32];
    int n = snprintf(line, sizeof line, "stage chat seed %d", i);
    CT_CHECK(ccord_session_send_message(F.session, HQ_STAGE, line, (size_t)n, &err) == CCORD_OK);
    frame(1);
  }
  frame(3);
  // The stage keeps rendering its speakers on the left.
  CT_CHECK(vsdbg().stage.speaker_count >= 1);

  // Reading: the bound channel's timeline renders in the right pane.
  ccord_ui_get_debug(F.ui, &d);
  size_t total = ccord_store_message_count(F.store, d.stage_chat_channel);
  CT_CHECK(total == 40);
  CT_CHECK(d.message_count == total && d.rows_drawn > 0);
  shot("stage_text_chat_split");

  // Sending: a composer message lands in the bound channel without closing the stage.
  ccord_rect cbox = ui_composer_box(F.ui);
  CT_CHECK(cbox.x >= l->message_area.x && cbox.w > 0.0f);
  click(cbox.x + cbox.w * 0.5f, cbox.y + cbox.h * 0.5f);
  type_text("hello from the stage");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  CT_CHECK(ccord_store_message_count(F.store, d.stage_chat_channel) == total + 1);
  const ccord_message *sent = ccord_store_message_at(F.store, d.stage_chat_channel, total);
  CT_CHECK(sent && strcmp(sent->content, "hello from the stage") == 0 &&
           sent->author_id == ME && sent->send_state == CCORD_MSG_SEND_CONFIRMED);
  CT_CHECK(F.ui->stage_channel == HQ_STAGE && F.ui->stage_chat_open);
  CT_CHECK(ccord_store_voice_user(F.store, HQ, ME) != nullptr);

  // Wheel routing: the chat pane scrolls the timeline; the stage pane must not.
  l = ccord_ui_get_layout(F.ui);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 30.0f,
                           .x = l->message_area.x + 100.0f, .y = l->message_area.y + 100.0f});
  frame(1);
  ccord_ui_get_debug(F.ui, &d);
  CT_CHECK(!d.stick_bottom);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -30.0f,
                           .x = l->stage_pane.x + 100.0f, .y = l->stage_pane.y + 300.0f});
  frame(1);
  ccord_ui_get_debug(F.ui, &d);
  CT_CHECK(!d.stick_bottom);   // the stage pane did not scroll the timeline back down
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -30.0f,
                           .x = l->message_area.x + 100.0f, .y = l->message_area.y + 100.0f});
  frame(1);
  ccord_ui_get_debug(F.ui, &d);
  CT_CHECK(d.stick_bottom);

  // Toggling again closes only the chat pane; the stage stays.
  click_rect(ui_stage_chat_btn(F.ui));
  frame(2);
  CT_CHECK(!F.ui->stage_chat_open && F.ui->stage_channel == HQ_STAGE);
  CT_CHECK(ccord_store_selected_channel(F.store) == text_before);   // closing the chat returns to the text channel
  l = ccord_ui_get_layout(F.ui);
  CT_CHECK(!l->stage_split);
  CT_CHECK(ccord_store_voice_user(F.store, HQ, ME) != nullptr);

  // With another guild's channel selected, Text Chat still binds the connected stage channel itself (its guild is
  // selected with it), and closing it goes back to the channel that was open.
  const ccord_guild *other = ccord_store_guild_at(F.store, 1);
  CT_CHECK(other != nullptr);
  ccord_id other_ch = CCORD_ID_NONE;
  if (other) {
    size_t n = ccord_store_channel_count(F.store, other->id);
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *c = ccord_store_channel_at(F.store, other->id, i);
      if (c && c->kind == CCORD_CHANNEL_TEXT) { other_ch = c->id; break; }
    }
  }
  CT_CHECK(other_ch != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, other_ch, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(F.ui->stage_channel == HQ_STAGE);   // selecting a channel elsewhere keeps the stage
  click_rect(ui_stage_chat_btn(F.ui));
  frame(3);
  ccord_ui_get_debug(F.ui, &d);
  CT_CHECK(d.stage_chat_open && d.stage_chat_channel == HQ_STAGE);
  CT_CHECK(ccord_store_selected_guild(F.store) == HQ);

  // Narrow window: the split does not fit, the stage keeps the full column; widening restores it.
  ccord_ui_set_viewport(F.ui, 700, 800);
  frame(1);
  ccord_ui_get_debug(F.ui, &d);
  CT_CHECK(!d.stage_chat_open && F.ui->stage_channel == HQ_STAGE);
  ccord_ui_set_viewport(F.ui, 1280, 800);
  frame(1);
  ccord_ui_get_debug(F.ui, &d);
  CT_CHECK(d.stage_chat_open && F.ui->stage_channel == HQ_STAGE);

  // Leaving voice resets the split flag along with the stage and returns the selection to a text channel.
  leave_voice_and_close();
  CT_CHECK(!F.ui->stage_chat_open && F.ui->stage_channel == CCORD_ID_NONE);
  const ccord_channel *after = ui_selected_channel(F.ui);
  CT_CHECK(after && after->kind == CCORD_CHANNEL_TEXT);
}

// ---- stream cards: switching the watched broadcast cleanly --------------------------------------

static void test_stream_card_switch(void)
{
  ccord_error err;
  join_and_open(HQ_VOICE);
  // Switch streams with the side-by-side Text Chat pane open: the cards live in the stage pane.
  click_rect(ui_stage_chat_btn(F.ui));
  frame(2);
  CT_CHECK(F.ui->stage_chat_open);
  // Two broadcasters in the channel: Ada (seeded) and Bjorn (watching creates his broadcast row).
  CT_CHECK(ccord_session_watch_stream(F.session, HQ, HQ_VOICE, ADA, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(ccord_session_watch_stream(F.session, HQ, HQ_VOICE, BJORN, &err) == CCORD_OK);
  frame(3);
  const ccord_watched_stream *ws = ccord_session_watched_stream(F.session);
  CT_CHECK(ws && ws->active && ws->user_id == BJORN);
  ui_voice_surfaces_debug d = vsdbg();
  CT_CHECK(d.stage.stream_cards >= 2);
  int ai = -1, bi = -1;
  for (size_t i = 0; i < d.stage.stream_cards; i++) {
    if (d.stage.card[i].user_id == ADA) ai = (int)i;
    if (d.stage.card[i].user_id == BJORN) bi = (int)i;
  }
  CT_CHECK(ai >= 0 && bi >= 0);
  if (ai < 0 || bi < 0) { leave_voice_and_close(); return; }
  CT_CHECK(d.stage.card[bi].watching && !d.stage.card[ai].watching);
  CT_CHECK(d.stage.card[ai].watch.w > 0.0f && d.stage.card[bi].watch.w > 0.0f);
  shot("stage_stream_cards");

  // Click Ada's Watch: the session switches the watched stream; the voice call keeps running.
  click_rect(d.stage.card[ai].watch);
  frame(3);
  ws = ccord_session_watched_stream(F.session);
  CT_CHECK(ws && ws->active && ws->user_id == ADA && ws->channel_id == HQ_VOICE);
  d = vsdbg();
  ai = bi = -1;
  for (size_t i = 0; i < d.stage.stream_cards; i++) {
    if (d.stage.card[i].user_id == ADA) ai = (int)i;
    if (d.stage.card[i].user_id == BJORN) bi = (int)i;
  }
  CT_CHECK(ai >= 0 && bi >= 0);
  if (ai < 0 || bi < 0) { leave_voice_and_close(); return; }
  CT_CHECK(d.stage.card[ai].watching && !d.stage.card[bi].watching);

  // And back to Bjorn.
  click_rect(d.stage.card[bi].watch);
  frame(3);
  ws = ccord_session_watched_stream(F.session);
  CT_CHECK(ws && ws->active && ws->user_id == BJORN);
  const ccord_voice_state *me = ccord_store_voice_user(F.store, HQ, ME);
  CT_CHECK(me && me->channel_id == HQ_VOICE);

  // Clicking the watched card stops watching; the other broadcast stays listed.
  d = vsdbg();
  bi = -1;
  for (size_t i = 0; i < d.stage.stream_cards; i++) {
    if (d.stage.card[i].user_id == BJORN) bi = (int)i;
  }
  CT_CHECK(bi >= 0 && d.stage.card[bi].watching);
  if (bi >= 0) {
    click_rect(d.stage.card[bi].watch);
    frame(3);
    ws = ccord_session_watched_stream(F.session);
    CT_CHECK(!ws || !ws->active);
    d = vsdbg();
    bool ada_listed = false;
    for (size_t i = 0; i < d.stage.stream_cards; i++) {
      if (d.stage.card[i].user_id == ADA) ada_listed = true;
    }
    CT_CHECK(ada_listed);
  }
  leave_voice_and_close();
}

// ---- Go Live "Share System Audio" checkbox -------------------------------------------------------

static void test_golive_share_audio(void)
{
  join_and_open(HQ_VOICE);
  ccord_ui_debug d;
  ccord_ui_get_debug(F.ui, &d);
  CT_CHECK(d.go_live_btn.w > 0.0f);
  click_rect(d.go_live_btn);
  frame(2);
  ccord_ui_get_debug(F.ui, &d);
  CT_CHECK(d.go_live_open);
  CT_CHECK(d.go_live_share_audio);                    // enabled by default
  CT_CHECK(ccord_ui_go_live_share_audio(F.ui));       // the read seam app_media polls
  CT_CHECK(d.go_live_audio_toggle.w > 0.0f && d.go_live_audio_toggle.h > 0.0f);
  // The row sits inside the modal, above the Go Live button.
  CT_CHECK(d.go_live_audio_toggle.x >= d.go_live_modal.x);
  CT_CHECK(d.go_live_audio_toggle.x + d.go_live_audio_toggle.w <= d.go_live_modal.x + d.go_live_modal.w);
  CT_CHECK(d.go_live_audio_toggle.y + d.go_live_audio_toggle.h <= d.go_live_start.y);
  shot("go_live_share_audio");

  click_rect(d.go_live_audio_toggle);
  frame(1);
  CT_CHECK(!ccord_ui_go_live_share_audio(F.ui));
  ccord_ui_get_debug(F.ui, &d);
  CT_CHECK(!d.go_live_share_audio);
  click_rect(d.go_live_audio_toggle);
  frame(1);
  CT_CHECK(ccord_ui_go_live_share_audio(F.ui));

  // Go Live with system audio shared: the stream starts and the toggle state survives.
  CT_CHECK(F.ui->go_live_source_count > 0);
  ccord_ui_get_debug(F.ui, &d);
  click_rect(d.go_live_start);
  frame(3);
  CT_CHECK(ccord_store_stream_active(F.store));
  CT_CHECK(!F.ui->go_live_open);
  CT_CHECK(ccord_ui_go_live_share_audio(F.ui));

  // Stop the stream again; the voice call keeps running.
  CT_CHECK(F.ui->stream_stage_btn2_rect.w > 0.0f);
  click_rect(F.ui->stream_stage_btn2_rect);
  frame(3);
  CT_CHECK(!ccord_store_stream_active(F.store));
  CT_CHECK(ccord_store_voice_user(F.store, HQ, ME) != nullptr);
  leave_voice_and_close();
}

// ---- guild scheduled events: guild menu -> modal, RSVP, create, edit, delete -------------------

static ui_events_debug evdbg(void)
{
  ui_events_debug d;
  ui_scheduled_events_get_debug(F.ui, &d);
  return d;
}

static int events_row_of(const ui_events_debug *d, ccord_id event_id)
{
  for (size_t i = 0; i < d->row_count; i++) {
    if (d->rows[i].event_id == event_id) return (int)i;
  }
  return -1;
}

// Moves to and clicks a top-level item of the open context menu by label.
static bool menu_click_label(const char *label)
{
  for (size_t i = 0; i < F.ui->menu.count; i++) {
    const menu_item *it = &F.ui->menu.items[i];
    if (it->parent >= 0 || strcmp(it->label, label) != 0) continue;
    click_rect(ui_menu_item_rect(F.ui, i));
    return true;
  }
  return false;
}

// Right-click the HQ guild icon and choose "Events"; pumps the EVENT_LIST round-trip.
static void open_guild_events(void)
{
  ccord_rect gr = ui_guild_icon_rect(F.ui, 1);   // C-Cord HQ
  mouse(CCORD_IN_MOUSE_MOVE, 0, gr.x + gr.w * 0.5f, gr.y + gr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, gr.x + gr.w * 0.5f, gr.y + gr.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->menu.open);
  CT_CHECK(menu_click_label("Events"));
  frame(3);
}

static ccord_id find_event_by_name(ccord_id guild, const char *name)
{
  size_t n = ccord_store_scheduled_event_count(F.store, guild);
  for (size_t i = 0; i < n; i++) {
    const ccord_scheduled_event *e = ccord_store_scheduled_event_at(F.store, guild, i);
    if (e && strcmp(e->name, name) == 0) return e->id;
  }
  return CCORD_ID_NONE;
}

static void test_scheduled_events(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, HQ, &err) == CCORD_OK);
  frame(2);

  // Opening the modal asks for the current list (the snapshot already seeded the cache).
  size_t lists_before = ccord_mock_admin_count(F.transport);
  open_guild_events();
  CT_CHECK(ccord_mock_admin_count(F.transport) == lists_before + 1);
  const ccord_admin_request *list_req = ccord_mock_last_admin(F.transport);
  CT_CHECK(list_req && list_req->op == CCORD_ADMIN_EVENT_LIST && list_req->guild_id == HQ);
  ui_events_debug d = evdbg();
  CT_CHECK(d.open && d.view == UI_EVENTS_LIST && d.guild_id == HQ);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS);
  CT_CHECK(d.row_count == 2);   // Community Town Hall (stage) + Pixel Art Meetup (external)
  CT_CHECK(d.create_btn.w > 0.0f && d.close_btn.w > 0.0f);
  const ccord_scheduled_event *town = ccord_store_find_scheduled_event(F.store, 97000u);
  CT_CHECK(town && town->user_count == 12 && !town->me_rsvped);

  // RSVP toggle: the store only moves when the transport answers.
  int ti = events_row_of(&d, 97000u);
  CT_CHECK(ti >= 0 && d.rows[ti].rsvp.w > 0.0f && !d.rows[ti].rsvp_on);
  click_rect(d.rows[ti].rsvp);
  frame(3);
  town = ccord_store_find_scheduled_event(F.store, 97000u);
  CT_CHECK(town && town->me_rsvped && town->user_count == 13);
  d = evdbg();
  ti = events_row_of(&d, 97000u);
  CT_CHECK(ti >= 0 && d.rows[ti].rsvp_on);
  click_rect(d.rows[ti].rsvp);
  frame(3);
  town = ccord_store_find_scheduled_event(F.store, 97000u);
  CT_CHECK(town && !town->me_rsvped && town->user_count == 12);

  // Create: an empty form is refused locally before anything is sent.
  size_t admins = ccord_mock_admin_count(F.transport);
  d = evdbg();
  click_rect(d.create_btn);
  frame(1);
  d = evdbg();
  CT_CHECK(d.view == UI_EVENTS_FORM && d.edit_event_id == CCORD_ID_NONE);
  CT_CHECK(d.entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE && d.channel_id == HQ_STAGE);
  click_rect(d.save_btn);
  frame(1);
  d = evdbg();
  CT_CHECK(d.view == UI_EVENTS_FORM && d.form_error[0] != '\0');
  CT_CHECK(ccord_mock_admin_count(F.transport) == admins);

  // Name + relative start time; the default Stage type targets the HQ stage channel.
  type_text("Test Driven Event");
  click_rect(d.start_input);
  type_text("+2h");
  int64_t t0 = F.ui->now_unix_ms;
  d = evdbg();
  click_rect(d.save_btn);
  frame(3);
  d = evdbg();
  CT_CHECK(d.view == UI_EVENTS_LIST && d.form_error[0] == '\0');
  CT_CHECK(ccord_mock_admin_count(F.transport) == admins + 1);
  const ccord_admin_request *last = ccord_mock_last_admin(F.transport);
  CT_CHECK(last && last->op == CCORD_ADMIN_EVENT_CREATE);
  CT_CHECK(ccord_store_scheduled_event_count(F.store, HQ) == 3);
  ccord_id new_id = find_event_by_name(HQ, "Test Driven Event");
  CT_CHECK(new_id != CCORD_ID_NONE);
  const ccord_scheduled_event *created = ccord_store_find_scheduled_event(F.store, new_id);
  CT_CHECK(created && created->entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE &&
           created->channel_id == HQ_STAGE && created->creator_id == ME &&
           created->status == CCORD_SCHEDULED_EVENT_SCHEDULED && created->scheduled_end_ms == 0);
  CT_CHECK(created->scheduled_start_ms >= t0 + 2 * 3600000 &&
           created->scheduled_start_ms <= t0 + 2 * 3600000 + 1000);

  // Edit: the form is prefilled (an unchanged save round-trips), then a rename lands.
  d = evdbg();
  int ni = events_row_of(&d, new_id);
  CT_CHECK(ni >= 0 && d.rows[ni].edit.w > 0.0f && d.rows[ni].del.w > 0.0f);
  click_rect(d.rows[ni].edit);
  frame(1);
  d = evdbg();
  CT_CHECK(d.view == UI_EVENTS_FORM && d.edit_event_id == new_id);
  CT_CHECK(d.entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE && d.channel_id == HQ_STAGE);
  click_rect(d.save_btn);
  frame(3);
  created = ccord_store_find_scheduled_event(F.store, new_id);
  CT_CHECK(created && strcmp(created->name, "Test Driven Event") == 0 &&
           created->scheduled_start_ms >= t0 + 2 * 3600000);
  d = evdbg();
  ni = events_row_of(&d, new_id);
  CT_CHECK(ni >= 0);
  click_rect(d.rows[ni].edit);
  frame(1);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);   // the name field is focused when the form opens
  type_text("Renamed Event");
  d = evdbg();
  click_rect(d.save_btn);
  frame(3);
  created = ccord_store_find_scheduled_event(F.store, new_id);
  CT_CHECK(created && strcmp(created->name, "Renamed Event") == 0);
  last = ccord_mock_last_admin(F.transport);
  CT_CHECK(last && last->op == CCORD_ADMIN_EVENT_MODIFY);

  // Delete is two-step: the first click arms, "Keep" disarms, the second click deletes.
  d = evdbg();
  ni = events_row_of(&d, new_id);
  CT_CHECK(ni >= 0);
  click_rect(d.rows[ni].del);
  frame(1);
  d = evdbg();
  ni = events_row_of(&d, new_id);
  CT_CHECK(ni >= 0 && d.rows[ni].delete_armed);
  CT_CHECK(ccord_store_find_scheduled_event(F.store, new_id) != nullptr);
  click_rect(d.rows[ni].edit);   // "Keep"
  frame(1);
  d = evdbg();
  ni = events_row_of(&d, new_id);
  CT_CHECK(ni >= 0 && !d.rows[ni].delete_armed);
  CT_CHECK(ccord_store_find_scheduled_event(F.store, new_id) != nullptr);
  click_rect(d.rows[ni].del);
  frame(1);
  d = evdbg();
  ni = events_row_of(&d, new_id);
  CT_CHECK(ni >= 0 && d.rows[ni].delete_armed);
  click_rect(d.rows[ni].del);   // "Confirm"
  frame(3);
  CT_CHECK(ccord_store_find_scheduled_event(F.store, new_id) == nullptr);
  CT_CHECK(ccord_store_scheduled_event_count(F.store, HQ) == 2);
  last = ccord_mock_last_admin(F.transport);
  CT_CHECK(last && last->op == CCORD_ADMIN_EVENT_DELETE);

  // External event: cycling the type chip exposes the location field and drops the channel chip;
  // an end time is required.
  d = evdbg();
  click_rect(d.create_btn);
  frame(1);
  type_text("Picnic");
  d = evdbg();
  click_rect(d.type_chip);   // Stage -> Voice
  frame(1);
  d = evdbg();
  CT_CHECK(d.entity_type == CCORD_SCHEDULED_EVENT_VOICE && d.channel_chip.w > 0.0f);
  click_rect(d.type_chip);   // Voice -> External
  frame(1);
  d = evdbg();
  CT_CHECK(d.entity_type == CCORD_SCHEDULED_EVENT_EXTERNAL && d.location_input.w > 0.0f &&
           d.channel_chip.w == 0.0f);
  click_rect(d.location_input);
  type_text("The Park");
  click_rect(d.start_input);
  type_text("+1d");
  d = evdbg();
  click_rect(d.save_btn);   // external without an end time is refused
  frame(1);
  d = evdbg();
  CT_CHECK(d.view == UI_EVENTS_FORM && d.form_error[0] != '\0');
  click_rect(d.end_input);
  type_text("+1d2h");
  d = evdbg();
  click_rect(d.save_btn);
  frame(3);
  CT_CHECK(evdbg().view == UI_EVENTS_LIST);
  ccord_id picnic_id = find_event_by_name(HQ, "Picnic");
  CT_CHECK(picnic_id != CCORD_ID_NONE);
  const ccord_scheduled_event *picnic = ccord_store_find_scheduled_event(F.store, picnic_id);
  CT_CHECK(picnic && picnic->entity_type == CCORD_SCHEDULED_EVENT_EXTERNAL &&
           picnic->channel_id == CCORD_ID_NONE && strcmp(picnic->location, "The Park") == 0 &&
           picnic->scheduled_end_ms > picnic->scheduled_start_ms);

  // A long list scrolls inside the modal: wheel moves the offset, a thumb is drawn, and only
  // fully visible rows keep live hits.
  ccord_scheduled_event many[12];
  for (size_t i = 0; i < 12; i++) {
    many[i] = (ccord_scheduled_event){
        .id = 98000u + i, .guild_id = HQ, .channel_id = HQ_STAGE, .creator_id = ME,
        .name = "Scroll Probe", .scheduled_start_ms = F.ui->now_unix_ms + 86400000ll,
        .privacy_level = 2, .status = CCORD_SCHEDULED_EVENT_SCHEDULED,
        .entity_type = CCORD_SCHEDULED_EVENT_STAGE_INSTANCE, .user_count = 1,
    };
    snprintf(many[i].name, sizeof many[i].name, "Scroll Probe %zu", i);
  }
  CT_CHECK(ccord_store_set_scheduled_events(F.store, HQ, many, 12, &err) == CCORD_OK);
  frame(1);
  d = evdbg();
  CT_CHECK(d.list_content_h > d.list_view.h);
  CT_CHECK(d.row_count < 12);   // off-fold rows are culled, not recorded
  for (size_t i = 0; i < d.row_count; i++) {
    CT_CHECK(d.rows[i].row.y >= d.list_view.y &&
             d.rows[i].row.y + d.rows[i].row.h <= d.list_view.y + d.list_view.h);
  }
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -3.0f,
                           .x = d.list_view.x + 10.0f, .y = d.list_view.y + 10.0f});
  frame(1);
  d = evdbg();
  CT_CHECK(d.list_scroll > 0.0f && d.list_thumb.w > 0.0f);
  CT_CHECK(events_row_of(&d, 98000u) < 0);   // the first row scrolled off is no longer live
  CT_CHECK(ccord_session_request_scheduled_events(F.session, HQ, &err) == CCORD_OK);   // restore the real list
  frame(3);
  CT_CHECK(ccord_store_scheduled_event_count(F.store, HQ) == 3);

  // Close button, then Escape on a fresh open.
  d = evdbg();
  click_rect(d.close_btn);
  frame(1);
  CT_CHECK(!evdbg().open && F.ui->modal_kind == UI_MODAL_NONE);
  open_guild_events();
  CT_CHECK(evdbg().open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!evdbg().open && F.ui->modal_kind == UI_MODAL_NONE);
}

// ---- stage lifecycle: Start Stage / Edit Topic / End Stage chips --------------------------------

static void test_stage_lifecycle(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, HQ, &err) == CCORD_OK);
  frame(2);
  join_and_open(HQ_STAGE);
  ui_stage_lifecycle_debug lc;
  ui_stage_lifecycle_get_debug(F.ui, &lc);
  CT_CHECK(lc.stage_active && lc.end_stage.w > 0.0f && lc.start_stage.w == 0.0f);
  CT_CHECK(vsdbg().stage.topic_edit.w > 0.0f);   // Edit Topic sits alongside End Stage
  CT_CHECK(ccord_store_stage_instance(F.store, HQ_STAGE) != nullptr);

  // End Stage asks for confirmation; Cancel keeps the instance.
  click_rect(lc.end_stage);
  frame(1);
  ui_events_debug d = evdbg();
  CT_CHECK(d.open && d.view == UI_EVENTS_STAGE_END && d.stage_channel == HQ_STAGE);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS);
  click_rect(d.stage_cancel);
  frame(1);
  CT_CHECK(!evdbg().open && F.ui->modal_kind == UI_MODAL_NONE);
  CT_CHECK(ccord_store_stage_instance(F.store, HQ_STAGE) != nullptr);

  // Confirm: the instance goes away only when the transport answers.
  ui_stage_lifecycle_get_debug(F.ui, &lc);
  click_rect(lc.end_stage);
  frame(1);
  d = evdbg();
  click_rect(d.stage_ok);
  frame(3);
  CT_CHECK(ccord_store_stage_instance(F.store, HQ_STAGE) == nullptr);
  CT_CHECK(!evdbg().open && F.ui->modal_kind == UI_MODAL_NONE);
  ui_stage_lifecycle_get_debug(F.ui, &lc);
  CT_CHECK(!lc.stage_active && lc.start_stage.w > 0.0f && lc.end_stage.w == 0.0f);
  CT_CHECK(vsdbg().stage.topic_edit.w == 0.0f);   // no instance, no topic to edit

  // Start Stage: the topic prompt creates the instance through the session.
  click_rect(lc.start_stage);
  frame(1);
  d = evdbg();
  CT_CHECK(d.open && d.view == UI_EVENTS_STAGE_START && d.stage_channel == HQ_STAGE);
  type_text("Launch Party");
  CT_CHECK(strcmp(evdbg().stage_topic, "Launch Party") == 0);
  click_rect(d.stage_ok);
  frame(3);
  const ccord_stage_instance *si = ccord_store_stage_instance(F.store, HQ_STAGE);
  CT_CHECK(si && strcmp(si->topic, "Launch Party") == 0);
  CT_CHECK(!evdbg().open && F.ui->modal_kind == UI_MODAL_NONE);
  ui_stage_lifecycle_get_debug(F.ui, &lc);
  CT_CHECK(lc.stage_active && lc.end_stage.w > 0.0f && lc.start_stage.w == 0.0f);

  // Escape cancels the start prompt; an empty topic starts without one.
  click_rect(lc.end_stage);
  frame(1);
  d = evdbg();
  click_rect(d.stage_ok);
  frame(3);
  CT_CHECK(ccord_store_stage_instance(F.store, HQ_STAGE) == nullptr);
  ui_stage_lifecycle_get_debug(F.ui, &lc);
  click_rect(lc.start_stage);
  frame(1);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!evdbg().open && ccord_store_stage_instance(F.store, HQ_STAGE) == nullptr);
  ui_stage_lifecycle_get_debug(F.ui, &lc);
  click_rect(lc.start_stage);
  frame(1);
  d = evdbg();
  click_rect(d.stage_ok);   // empty topic
  frame(3);
  si = ccord_store_stage_instance(F.store, HQ_STAGE);
  CT_CHECK(si && si->topic[0] == '\0');

  // Edit Topic still works on the running instance (the shared voice prompt path).
  ui_voice_surfaces_debug vd = vsdbg();
  CT_CHECK(vd.stage.topic_edit.w > 0.0f);
  click_rect(vd.stage.topic_edit);
  frame(1);
  CT_CHECK(vsdbg().prompt_open && vsdbg().prompt_kind == UI_VOICE_PROMPT_TOPIC);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  type_text("Edited Topic");
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  si = ccord_store_stage_instance(F.store, HQ_STAGE);
  CT_CHECK(si && strcmp(si->topic, "Edited Topic") == 0);
  leave_voice_and_close();

  // Permission gate: a plain member in Pixel Garden gets no lifecycle or topic chips.
  CT_CHECK(ccord_session_select_guild(F.session, 2003, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_session_join_voice(F.session, 2003, 3323, &err) == CCORD_OK);
  frame(3);
  F.ui->stage_channel = 3323;
  F.ui->dirty = true;
  frame(2);
  ui_stage_lifecycle_get_debug(F.ui, &lc);
  CT_CHECK(lc.start_stage.w == 0.0f && lc.end_stage.w == 0.0f);
  CT_CHECK(vsdbg().stage.topic_edit.w == 0.0f);
  leave_voice_and_close();
  CT_CHECK(ccord_session_select_guild(F.session, HQ, &err) == CCORD_OK);
  frame(2);
}

// ---- DAVE badge + verification modal + RTC popover ----------------------------------------------

static void test_dave_rtc(void)
{
  ccord_error err;
  join_and_open(HQ_VOICE);
  // No media report yet: no badge.
  CT_CHECK(!vsdbg().dave_badge_visible);
  CT_CHECK(vsdbg().rtc_btn.w > 0.0f);   // RTC button is offered whenever connected

  ccord_rtc_stats rtc = {.state = 4, .ssrc = 4242, .ping_ms = 31, .jitter_depth = 3,
                         .packets_lost = 2, .bitrate_kbps = 64, .dave_epoch = 7};
  ccord_dave_status dave = {.active = true, .verified = false, .protocol_version = 1};
  CT_CHECK(ccord_store_set_rtc_stats(F.store, &rtc, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_dave_status(F.store, &dave, &err) == CCORD_OK);
  frame(2);
  ui_voice_surfaces_debug d = vsdbg();
  CT_CHECK(d.dave_badge_visible && d.dave_badge.w > 0.0f);
  CT_CHECK(d.dave_active && !d.dave_verified && d.dave_epoch == 7);
  // Deterministic local fingerprint of ssrc 4242 / epoch 7.
  CT_CHECK(strcmp(d.dave_privacy_code, "5384-4345-5391") == 0);

  // Verification modal (read-only).
  click_rect(d.dave_badge);
  frame(1);
  d = vsdbg();
  CT_CHECK(d.dave_modal_open && !d.rtc_open);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_EDIT_CHANNEL);
  CT_CHECK(d.dave_close.w > 0.0f);
  click_rect(d.dave_close);
  frame(1);
  CT_CHECK(!vsdbg().dave_modal_open && F.ui->modal_kind == UI_MODAL_NONE);

  // RTC popover.
  d = vsdbg();
  click_rect(d.rtc_btn);
  frame(1);
  d = vsdbg();
  CT_CHECK(d.rtc_open && !d.dave_modal_open);
  CT_CHECK(d.rtc_close.w > 0.0f);
  click(5.0f, 5.0f);   // outside the dialog closes it
  frame(1);
  CT_CHECK(!vsdbg().rtc_open && F.ui->modal_kind == UI_MODAL_NONE);
  click_rect(d.rtc_btn);
  frame(1);
  CT_CHECK(vsdbg().rtc_open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!vsdbg().rtc_open && F.ui->modal_kind == UI_MODAL_NONE);
  leave_voice_and_close();
}

// ---- Embedded Activities: chip -> shelf -> launch, instance card, Join / Leave / Open in browser --

static bool instance_has_user(const ccord_activity_instance *in, ccord_id user)
{
  for (size_t i = 0; i < in->participant_count; i++) {
    if (in->participants[i] == user) return true;
  }
  return false;
}

static void test_activities(void)
{
  ccord_error err;
  // Stage view of a voice channel the user is connected to: the chip is in the control row and no
  // activity is running yet (so no instance cards and no banner).
  join_and_open(HQ_VOICE);
  ui_activity_debug d = actdbg();
  CT_CHECK(d.chip.w > 0.0f && d.chip.h > 0.0f);
  CT_CHECK(!d.open && d.instance_count == 0 && !d.banner_visible);
  // The chip lives in the stage control row (inside the stage area): it takes none of the three
  // fixed voice-bar slots.
  ccord_rect sa = ui_stage_area(F.ui);
  CT_CHECK(d.chip.x >= sa.x && d.chip.x + d.chip.w <= sa.x + sa.w && d.chip.y >= sa.y && d.chip.y < sa.y + 100.0f);

  // Clicking the chip opens the shelf; the mock answers GET /activities/shelf through pump().
  click_rect(d.chip);
  frame(3);
  d = actdbg();
  CT_CHECK(d.open && d.guild_id == HQ && d.channel_id == HQ_VOICE);
  CT_CHECK(ui_activity_shelf_active(F.ui));
  CT_CHECK(d.box.w > 0.0f && d.close_btn.w > 0.0f && d.list_view.w > 0.0f);
  CT_CHECK(d.row_count >= 1u);
  CT_CHECK(d.rows[0].row.w > 0.0f && d.rows[0].name[0] != '\0' && d.rows[0].application_id != CCORD_ID_NONE);
  size_t shelf_n = 0;
  const ccord_activity_app *shelf = ccord_session_activity_shelf(F.session, HQ, &shelf_n);
  CT_CHECK(shelf && shelf_n == d.row_count);
  CT_CHECK(strcmp(d.rows[0].name, shelf[0].name) == 0);
  shot("activities_shelf");

  // Escape closes the shelf, the close button too; the chip reopens it.
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!actdbg().open);
  click_rect(actdbg().chip);
  d = actdbg();
  CT_CHECK(d.open);
  click_rect(d.close_btn);
  CT_CHECK(!actdbg().open);
  click_rect(actdbg().chip);
  d = actdbg();
  CT_CHECK(d.open && d.rows[0].row.w > 0.0f);

  // Clicking an entry launches it: the session sends the request, the instance (with the user as a
  // participant) only appears once the transport answers, and the shelf closes.
  ccord_id app = d.rows[0].application_id;
  CT_CHECK(ccord_store_activity_instance_count(F.store, HQ_VOICE) == 0u);
  click_rect(d.rows[0].row);
  frame(3);
  d = actdbg();
  CT_CHECK(!d.open);
  size_t n = 0;
  const ccord_activity_instance *inst = ccord_session_activity_instances(F.session, HQ_VOICE, &n);
  CT_CHECK(inst && n == 1u && inst[0].application_id == app && instance_has_user(&inst[0], ME));
  ccord_id instance_id = inst[0].instance_id;

  // The stage view now lists the running instance with the browser banner and the own-instance
  // buttons: Open in browser and Leave, no Join.
  CT_CHECK(d.instance_count == 1u && d.banner_visible);
  CT_CHECK(d.instances[0].instance_id == instance_id && d.instances[0].application_id == app);
  CT_CHECK(d.instances[0].self_in && d.instances[0].participant_count == inst[0].participant_count);
  CT_CHECK(d.instances[0].card.w > 0.0f && d.instances[0].leave.w > 0.0f && d.instances[0].open_browser.w > 0.0f);
  CT_CHECK(d.instances[0].join.w == 0.0f);
  shot("activities_instance");

  // Open in browser hands the channel URL to the host opener; nothing is embedded or changed.
  g_open_count = 0;
  g_opened_url[0] = '\0';
  click_rect(d.instances[0].open_browser);
  CT_CHECK(g_open_count == 1);
  CT_CHECK(strcmp(g_opened_url, "https://discord.com/channels/2001/3121") == 0);
  CT_CHECK(ccord_store_activity_instance_count(F.store, HQ_VOICE) == 1u);

  // Leave calls the session; the row only disappears when the transport confirms.
  d = actdbg();
  click_rect(d.instances[0].leave);
  frame(3);
  CT_CHECK(ccord_store_activity_instance_count(F.store, HQ_VOICE) == 0u);
  d = actdbg();
  CT_CHECK(d.instance_count == 0u && !d.banner_visible);

  // An instance the user is not in offers Join (and nothing else). Join launches the app, which
  // joins that same running instance: the user shows up as a participant and gets Leave instead.
  ccord_id other_participants[] = {ADA, BJORN};
  ccord_id other_id = 0;
  CT_CHECK(ccord_mock_push_activity_instance(F.transport, HQ_VOICE, app, other_participants, 2u, &other_id, &err) ==
           CCORD_OK);
  frame(3);
  d = actdbg();
  CT_CHECK(d.instance_count == 1u && d.banner_visible && !d.instances[0].self_in);
  CT_CHECK(d.instances[0].instance_id == other_id && d.instances[0].participant_count == 2u);
  CT_CHECK(d.instances[0].join.w > 0.0f && d.instances[0].leave.w == 0.0f && d.instances[0].open_browser.w == 0.0f);
  click_rect(d.instances[0].join);
  frame(3);
  inst = ccord_session_activity_instances(F.session, HQ_VOICE, &n);
  CT_CHECK(inst && n == 1u && inst[0].instance_id == other_id && inst[0].participant_count == 3u);
  CT_CHECK(instance_has_user(&inst[0], ME) && instance_has_user(&inst[0], ADA));
  d = actdbg();
  CT_CHECK(d.instance_count == 1u && d.instances[0].self_in && d.instances[0].participant_count == 3u);
  CT_CHECK(d.instances[0].leave.w > 0.0f && d.instances[0].open_browser.w > 0.0f && d.instances[0].join.w == 0.0f);

  // Leaving takes only the user out; the others keep the instance and Join is offered again.
  click_rect(d.instances[0].leave);
  frame(3);
  inst = ccord_session_activity_instances(F.session, HQ_VOICE, &n);
  CT_CHECK(inst && n == 1u && inst[0].participant_count == 2u && !instance_has_user(&inst[0], ME));
  d = actdbg();
  CT_CHECK(!d.instances[0].self_in && d.instances[0].join.w > 0.0f);

  // When the others leave the instance ends and the block (banner included) disappears.
  CT_CHECK(ccord_mock_push_activity_instance(F.transport, HQ_VOICE, app, nullptr, 0, nullptr, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(ccord_store_activity_instance_count(F.store, HQ_VOICE) == 0u);
  d = actdbg();
  CT_CHECK(d.instance_count == 0u && !d.banner_visible);

  // Keyboard: Down moves the selection, Enter launches the selected entry; the wheel over a list that fits
  // changes nothing.
  click_rect(d.chip);
  d = actdbg();
  CT_CHECK(d.open && d.row_count >= 2u && d.sel == 0u && d.gutter == 0.0f);
  send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -30.0f, .x = d.box.x + 40.0f, .y = d.box.y + 120.0f});
  frame(2);
  d = actdbg();
  CT_CHECK(d.open && d.list_scroll == 0.0f);
  key(CCORD_KEY_DOWN, 0);
  frame(2);
  d = actdbg();
  CT_CHECK(d.open && d.sel == 1u);
  ccord_id second_app = d.rows[1].application_id;
  CT_CHECK(second_app != app);
  key(CCORD_KEY_RETURN, 0);
  frame(3);
  d = actdbg();
  CT_CHECK(!d.open);
  inst = ccord_session_activity_instances(F.session, HQ_VOICE, &n);
  CT_CHECK(inst && n == 1u && inst[0].application_id == second_app && instance_has_user(&inst[0], ME));
  click_rect(d.instances[0].leave);
  frame(3);
  CT_CHECK(ccord_store_activity_instance_count(F.store, HQ_VOICE) == 0u);

  // Short window: the list overflows, so the scrollbar owns a gutter on the right. Clicking it scrolls and
  // never launches; a row still launches.
  ccord_ui_set_viewport(F.ui, 1280, 280);
  ui_activity_shelf_open(F.ui, HQ, HQ_VOICE);
  frame(3);
  d = actdbg();
  CT_CHECK(d.open && d.gutter > 0.0f && d.list_content_h > d.list_view.h && d.list_scroll == 0.0f);
  CT_CHECK(d.rows[0].row.w > 0.0f && d.rows[0].row.x + d.rows[0].row.w <= d.list_view.x + d.list_view.w - d.gutter + 0.5f);
  click(d.list_view.x + d.list_view.w - 3.0f, d.list_view.y + d.list_view.h - 2.0f);
  d = actdbg();
  CT_CHECK(d.open && d.list_scroll > 0.0f);
  CT_CHECK(ccord_store_activity_instance_count(F.store, HQ_VOICE) == 0u);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!actdbg().open);
  ccord_ui_set_viewport(F.ui, 1280, 800);
  frame(2);

  // A card that is only partly scrolled into view keeps the visible part of its buttons clickable, and
  // never outside the stage viewport.
  CT_CHECK(ccord_mock_push_activity_instance(F.transport, HQ_VOICE, app, other_participants, 2u, &other_id, &err) ==
           CCORD_OK);
  frame(3);
  d = actdbg();
  CT_CHECK(d.instance_count == 1u && d.instances[0].join.w > 0.0f);
  click_rect(d.instances[0].join);
  frame(3);
  ccord_ui_set_viewport(F.ui, 1280, 280);
  F.ui->stage_scroll = 60.0f;
  F.ui->dirty = true;
  frame(2);
  d = actdbg();
  float view_top = ui_stage_area(F.ui).y + 96.0f;
  CT_CHECK(F.ui->stage_scroll > 0.0f);
  CT_CHECK(d.instance_count == 1u && d.instances[0].self_in);
  CT_CHECK(d.instances[0].leave.w > 0.0f && d.instances[0].open_browser.w > 0.0f);
  CT_CHECK(d.instances[0].leave.y >= view_top - 0.01f && d.instances[0].leave.h <= 24.0f);
  click_rect(d.instances[0].leave);
  frame(3);
  inst = ccord_session_activity_instances(F.session, HQ_VOICE, &n);
  CT_CHECK(inst && n == 1u && !instance_has_user(&inst[0], ME));
  CT_CHECK(ccord_mock_push_activity_instance(F.transport, HQ_VOICE, app, nullptr, 0, nullptr, &err) == CCORD_OK);
  frame(3);
  F.ui->stage_scroll = 0.0f;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  frame(2);

  // Not connected: once the user is out of the voice channel the session refuses the launch; the UI says so
  // and nothing starts.
  leave_voice_and_close();
  ui_activity_shelf_open(F.ui, HQ, HQ_VOICE);
  frame(3);
  d = actdbg();
  CT_CHECK(d.open && d.row_count >= 1u && d.rows[0].row.w > 0.0f);
  F.ui->notice[0] = '\0';
  click_rect(d.rows[0].row);
  frame(2);
  CT_CHECK(!actdbg().open);
  CT_CHECK(strstr(F.ui->notice, "Cannot start activity") != nullptr);
  CT_CHECK(ccord_store_activity_instance_count(F.store, HQ_VOICE) == 0u);
}

// A dropped voice link keeps the bar up, relabels it, and leaves the disconnect button working.
static void test_voice_bar_reconnecting(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_join_voice(F.session, HQ, HQ_VOICE, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(strcmp(ui_voice_bar_title(F.ui), "Voice Connected") == 0);
  ccord_rtc_stats st = {0};
  st.state = CCORD_VOICE_RECONNECTING;
  CT_CHECK(ccord_store_set_rtc_stats(F.store, &st, &err) == CCORD_OK);
  F.ui->dirty = true;
  frame(2);
  CT_CHECK(strcmp(ui_voice_bar_title(F.ui), "Reconnecting...") == 0);
  CT_CHECK(ui_self_voice(F.ui) != nullptr);
  CT_CHECK(ui_voice_bar_rect(F.ui).h > 0.0f);
  click_rect(ui_voice_btn_rect(F.ui, 2));
  frame(3);
  CT_CHECK(ui_self_voice(F.ui) == nullptr);
  F.ui->dirty = true;
  frame(2);
}

// ---- camera tile grid ----
static void test_cam_layout_compute(void)
{
  const size_t ns[] = {1, 2, 3, 4, 5, 9};
  const int want_cols[] = {1, 2, 2, 2, 3, 3};
  for (size_t i = 0; i < 6; i++) {
    ui_cam_layout l;
    ui_cam_layout_compute(1100.0f, ns[i], &l);
    CT_CHECK(l.cols == want_cols[i]);
    CT_CHECK(l.tile_w >= 220.0f && l.tile_w <= 520.0f);
    float vw = l.tile_w - 12.0f;
    CT_CHECK(fabsf(l.video_h - vw * 9.0f / 16.0f) <= 1.0f);
    // The last (partial) row is centred under the block.
    size_t rows = (ns[i] + (size_t)l.cols - 1) / (size_t)l.cols;
    size_t in_last = ns[i] - (rows - 1) * (size_t)l.cols;
    ccord_rect first = ui_cam_layout_tile(&l, 100.0f, 1100.0f, 0.0f, ns[i], (rows - 1) * (size_t)l.cols);
    ccord_rect last = ui_cam_layout_tile(&l, 100.0f, 1100.0f, 0.0f, ns[i], ns[i] - 1);
    float left = first.x - 100.0f, right = 1200.0f - (last.x + last.w);
    CT_CHECK(fabsf(left - right) < 0.01f);
    (void)in_last;
  }
  // Narrow areas: columns drop so a tile is never under 220 wide, or the tile fills a narrower area.
  ui_cam_layout l;
  ui_cam_layout_compute(320.0f, 9, &l);
  CT_CHECK(l.cols == 1 && l.tile_w == 320.0f);
  ui_cam_layout_compute(480.0f, 9, &l);
  CT_CHECK(l.cols == 2 && l.tile_w >= 220.0f);
  ui_cam_layout_compute(200.0f, 4, &l);
  CT_CHECK(l.cols == 1 && l.tile_w == 200.0f);
  // 4 tiles of 220 px fit in 452 x 422.
  ui_cam_layout_compute(452.0f, 4, &l);
  CT_CHECK(l.cols == 2 && l.tile_w == 220.0f);
  CT_CHECK(ui_cam_layout_height(&l, 4) <= 422.0f);
}

static void test_cam_layout_bounds(void)
{
  const float widths[] = {180.0f, 320.0f, 452.0f, 700.0f, 1100.0f, 2000.0f};
  for (size_t wi = 0; wi < 6; wi++) {
    for (size_t n = 1; n <= 16; n++) {
      ui_cam_layout l;
      ui_cam_layout_compute(widths[wi], n, &l);
      float hh = ui_cam_layout_height(&l, n);
      for (size_t i = 0; i < n; i++) {
        ccord_rect t = ui_cam_layout_tile(&l, 50.0f, widths[wi], 30.0f, n, i);
        CT_CHECK(t.x >= 50.0f - 0.01f && t.x + t.w <= 50.0f + widths[wi] + 0.01f);
        CT_CHECK(t.y >= 30.0f - 0.01f && t.y + t.h <= 30.0f + hh + 0.01f);
      }
    }
  }
}

static void test_cam_tiles_state(void)
{
  join_and_open(HQ_VOICE);
  ccord_error err;
  CT_CHECK(ccord_mock_set_remote_camera(F.transport, HQ, HQ_VOICE, 1005, true, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_set_remote_camera(F.transport, HQ, HQ_VOICE, 1006, true, &err) == CCORD_OK);
  frame(4);
  ui_stage_debug d;
  ui_stage_get_debug(F.ui, &d);
  CT_CHECK(d.cam_tile_count == 2);
  CT_CHECK(d.cam_cols == 2);
  for (size_t i = 0; i < d.cam_tile_count; i++) {
    CT_CHECK(strncmp(d.cam_tile[i].key, "__cam__:", 8) == 0);
    CT_CHECK(!d.cam_tile[i].limit);
    CT_CHECK(d.cam_tile[i].connecting || d.cam_tile[i].textured);
  }
  // Waiting senders (beyond 4 decoding) carry the limit label.
  CT_CHECK(ccord_mock_set_remote_camera(F.transport, HQ, HQ_VOICE, 1007, true, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_set_remote_camera(F.transport, HQ, HQ_VOICE, 1008, true, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_set_remote_camera(F.transport, HQ, HQ_VOICE, 1009, true, &err) == CCORD_OK);
  frame(4);
  ui_stage_get_debug(F.ui, &d);
  CT_CHECK(d.cam_tile_count == 5);
  CT_CHECK(d.cam_cols == 3);
  size_t limited = 0;
  for (size_t i = 0; i < d.cam_tile_count; i++) {
    if (d.cam_tile[i].limit) {
      limited++;
      CT_CHECK(d.cam_tile[i].user_id == 1009);
    }
  }
  CT_CHECK(limited == 1);
  leave_voice_and_close();
}

static void test_prejoin_mute_deafen_and_voice_bar_layout(void)
{
  ccord_error err;
  CT_CHECK(ui_self_voice(F.ui) == nullptr);
  F.ui->notice[0] = '\0';

  click_rect(ui_user_btn_rect(F.ui, 0));   // pre-join Mute
  frame(2);
  CT_CHECK(ccord_session_voice_muted(F.session) && !ccord_session_voice_deafened(F.session));
  CT_CHECK(strstr(F.ui->notice, "Join a voice channel first") == nullptr);

  click_rect(ui_user_btn_rect(F.ui, 1));   // pre-join Deafen
  frame(2);
  CT_CHECK(ccord_session_voice_muted(F.session) && ccord_session_voice_deafened(F.session));

  click_rect(ui_user_btn_rect(F.ui, 0));   // pre-join Unmute clears deafen too
  frame(2);
  CT_CHECK(!ccord_session_voice_muted(F.session) && !ccord_session_voice_deafened(F.session));

  // Pre-join mute carries into joining voice via session, and voice bar shrinks channel_scroll cleanly.
  click_rect(ui_user_btn_rect(F.ui, 0));
  frame(2);
  CT_CHECK(ccord_session_join_voice(F.session, HQ, HQ_VOICE, &err) == CCORD_OK);
  frame(4);
  const ccord_voice_state *v = ui_self_voice(F.ui);
  CT_CHECK(v != nullptr && v->channel_id == HQ_VOICE && v->self_mute && !v->self_deaf);
  ccord_rect vbar = ui_voice_bar_rect(F.ui);
  CT_CHECK(vbar.h > 0.0f);
  CT_CHECK(F.ui->layout.channel_scroll.y + F.ui->layout.channel_scroll.h <= vbar.y + 0.01f);

  // Restore unmuted state cleanly before leaving.
  click_rect(ui_user_btn_rect(F.ui, 0));
  frame(4);
  v = ui_self_voice(F.ui);
  CT_CHECK(v != nullptr && !v->self_mute);
  leave_voice_and_close();
}

static void right_click(float x, float y)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 3, x, y);
  mouse(CCORD_IN_MOUSE_UP, 3, x, y);
  frame(2);
}

static void test_vc_profile_click_and_right_click(void)
{
  join_and_open(HQ_VOICE);
  ccord_error err;
  ui_stage_debug d = vsdbg().stage;
  CT_CHECK(d.ctl_count >= 1);
  // Find CHEN's tile in the VC stage (or any non-self tile; CHEN is in HQ_VOICE in fixture).
  ccord_id target = CCORD_ID_NONE;
  float tx = 0.0f, ty = 0.0f;
  for (size_t i = 0; i < d.ctl_count; i++) {
    if (d.ctl[i].user_id != ME && d.ctl[i].mute.w > 0.0f) {
      target = d.ctl[i].user_id;
      // Top-center of the tile (above Row 3/4 controls) hits the VC card / avatar area.
      tx = d.ctl[i].mute.x + 40.0f;
      ty = d.ctl[i].mute.y - 90.0f;
      break;
    }
  }
  CT_CHECK(target != CCORD_ID_NONE);
  if (target == CCORD_ID_NONE) {
    leave_voice_and_close();
    return;
  }

  // Left-clicking a VC participant card opens their profile popover; clicking the header opens the modal.
  click(tx, ty);
  CT_CHECK(F.ui->member_popover_open && F.ui->member_popover_user_id == target);
  click(F.ui->member_popover_rect.x + 40.0f, F.ui->member_popover_rect.y + 40.0f);
  CT_CHECK(!F.ui->member_popover_open && F.ui->modal_kind == UI_MODAL_USER_PROFILE &&
           F.ui->modal_target_id == target);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);

  // Right-clicking the VC card opens CCORD_MENU_MEMBER with Profile, Message (Open DM), and Mute.
  right_click(tx, ty);
  CT_CHECK(F.ui->menu.open);
  ccord_rect prof_rc = {0}, mute_rc = {0};
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Profile", &prof_rc));
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Message (Open DM)", &mute_rc));
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Mute", &mute_rc));
  click_rect(mute_rc);
  CT_CHECK(ccord_store_user_local_mute(F.store, target));
  (void)ccord_store_set_user_local_mute(F.store, target, false, &err);

  // Clicking "Profile" in the VC right-click menu opens the full user profile modal.
  right_click(tx, ty);
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Profile", &prof_rc));
  click_rect(prof_rc);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_USER_PROFILE && F.ui->modal_target_id == target);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);

  // Right-clicking a sidebar voice user row also exposes Profile and Mute; left-clicking opens popover.
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float part_y = channel_row_y(HQ_VOICE, target);
  CT_CHECK(part_y > 0.0f);
  if (part_y > 0.0f) {
    float rx = l->channel_scroll.x + 80.0f;
    right_click(rx, part_y);
    CT_CHECK(F.ui->menu.open && ccord_ui_context_menu_find(F.ui, "Profile", &prof_rc));
    ui_menu_close(F.ui);
    frame(1);
    click(rx, part_y);
    CT_CHECK(F.ui->member_popover_open && F.ui->member_popover_user_id == target);
    F.ui->member_popover_open = false;
    frame(1);
  }
  leave_voice_and_close();
}

static void dblclick_at(float x, float y)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 2, .x = x, .y = y});
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  frame(2);
}

static void drag_from_to(float x0, float y0, float x1, float y1)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, x0, y0);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x0, y0);
  mouse(CCORD_IN_MOUSE_MOVE, 0, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, x1, y1);
  mouse(CCORD_IN_MOUSE_UP, 1, x1, y1);
  frame(2);
}

static void test_stream_resize_fullscreen_pip(void)
{
  ccord_error err;
  join_and_open(HQ_VOICE);

  // Watch Ada's remote stream and upload a 16:9 gradient test frame so aspect & visuals are verifiable.
  CT_CHECK(ccord_session_watch_stream(F.session, HQ, HQ_VOICE, ADA, &err) == CCORD_OK);
  uint8_t px[320 * 180 * 4];
  for (int y = 0; y < 180; y++) {
    for (int x = 0; x < 320; x++) {
      uint8_t *p = &px[(size_t)(y * 320 + x) * 4u];
      p[0] = (uint8_t)(40 + (x * 180) / 320);
      p[1] = (uint8_t)(70 + (y * 140) / 180);
      p[2] = (uint8_t)(190 - (x * 110) / 320);
      p[3] = 255u;
    }
  }
  CT_CHECK(ccord_render_upload_rgba(F.renderer, "__video_stream__", px, 320, 180, &err) == CCORD_OK);
  frame(3);

  ui_voice_surfaces_debug d = vsdbg();
  CT_CHECK(d.stage.stream_player.h >= 280.0f);
  CT_CHECK(d.stage.stream_resize_handle.w > 0.0f && d.stage.stream_resize_handle.h > 0.0f);
  CT_CHECK(d.stage.stream_fs_btn.w > 0.0f && d.stage.stream_fs_btn.h > 0.0f);
  CT_CHECK(!d.stage.stream_fullscreen && !d.stage.stream_pip_visible);
  float def_h = d.stage.stream_player.h;

  // 1. Default height is no longer artificially capped at 300 px on taller viewports.
  ccord_ui_set_viewport(F.ui, 1920, 1080);
  frame(2);
  ui_voice_surfaces_debug d_big = vsdbg();
  CT_CHECK(d_big.stage.stream_player.h > 450.0f);
  ccord_ui_set_viewport(F.ui, 1280, 800);
  frame(2);
  d = vsdbg();

  // 2. Vertical resize handle on the stage stream player: hover cursor, drag larger, drag smaller, double-click reset.
  float hx = d.stage.stream_resize_handle.x + d.stage.stream_resize_handle.w * 0.5f;
  float hy = d.stage.stream_resize_handle.y + d.stage.stream_resize_handle.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, hx, hy);
  CT_CHECK(F.ui->cursor == CCORD_CURSOR_NS_RESIZE);
  drag_from_to(hx, hy, hx, hy + 200.0f);
  d = vsdbg();
  CT_CHECK(d.stage.stream_player.h > def_h + 120.0f);
  shot("stream_stage_resized");

  hx = d.stage.stream_resize_handle.x + d.stage.stream_resize_handle.w * 0.5f;
  hy = d.stage.stream_resize_handle.y + d.stage.stream_resize_handle.h * 0.5f;
  drag_from_to(hx, hy, hx, d.stage.stream_player.y + 150.0f);
  d = vsdbg();
  CT_CHECK(d.stage.stream_player.h <= 180.0f && d.stage.stream_player.h >= 159.0f);

  hx = d.stage.stream_resize_handle.x + d.stage.stream_resize_handle.w * 0.5f;
  hy = d.stage.stream_resize_handle.y + d.stage.stream_resize_handle.h * 0.5f;
  dblclick_at(hx, hy);
  d = vsdbg();
  CT_CHECK(fabsf(d.stage.stream_player.h - def_h) < 2.0f);

  // 3. Horizontal resize divider between Voice Stage and Text Chat split pane.
  click_rect(ui_stage_chat_btn(F.ui));
  frame(2);
  CT_CHECK(F.ui->stage_chat_open);
  d = vsdbg();
  CT_CHECK(d.stage.stage_split_handle.w > 0.0f);
  float def_stage_w = ccord_ui_get_layout(F.ui)->stage_pane.w;
  float sx = d.stage.stage_split_handle.x + d.stage.stage_split_handle.w * 0.5f;
  float sy = d.stage.stage_split_handle.y + 200.0f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, sx, sy);
  CT_CHECK(F.ui->cursor == CCORD_CURSOR_EW_RESIZE);
  drag_from_to(sx, sy, sx + 120.0f, sy);
  float wider_stage_w = ccord_ui_get_layout(F.ui)->stage_pane.w;
  CT_CHECK(wider_stage_w > def_stage_w + 80.0f);
  shot("stream_stage_split_resized");
  d = vsdbg();
  sx = d.stage.stage_split_handle.x + d.stage.stage_split_handle.w * 0.5f;
  dblclick_at(sx, sy);
  CT_CHECK(fabsf(ccord_ui_get_layout(F.ui)->stage_pane.w - def_stage_w) < 2.0f);
  click_rect(ui_stage_chat_btn(F.ui));   // close split chat
  frame(2);

  // 4. Fullscreen via stage button, resize-during-fullscreen guard, exit via Exit button, double-click, F & Esc.
  g_fullscreen_calls = 0;
  g_fullscreen_state = false;
  int saved_w_before = F.ui->settings_win_w;
  int saved_h_before = F.ui->settings_win_h;
  d = vsdbg();
  click_rect(d.stage.stream_fs_btn);
  frame(2);
  d = vsdbg();
  CT_CHECK(d.stage.stream_fullscreen && g_fullscreen_state && g_fullscreen_calls == 1);
  CT_CHECK(d.stage.stream_fs_video.w >= 1279.0f && d.stage.stream_fs_video.h >= 719.0f);
  CT_CHECK(d.stage.stream_fs_exit_btn.w > 0.0f);
  shot("stream_fullscreen");

  // Fullscreen OS resize must not overwrite the saved windowed dimensions.
  send((ccord_input_event){.type = CCORD_IN_RESIZE, .x = 2560.0f, .y = 1440.0f});
  frame(1);
  CT_CHECK(F.ui->settings_win_w == saved_w_before && F.ui->settings_win_h == saved_h_before);
  ccord_ui_set_viewport(F.ui, 1280, 800);
  frame(1);

  d = vsdbg();
  click_rect(d.stage.stream_fs_exit_btn);
  frame(2);
  d = vsdbg();
  CT_CHECK(!d.stage.stream_fullscreen && !g_fullscreen_state);

  // Double-click on video viewport enters fullscreen; double-click in fullscreen exits.
  dblclick_at(d.stage.stream_vport.x + d.stage.stream_vport.w * 0.5f,
              d.stage.stream_vport.y + d.stage.stream_vport.h * 0.5f);
  CT_CHECK(vsdbg().stage.stream_fullscreen && g_fullscreen_state);
  dblclick_at(640.0f, 400.0f);
  CT_CHECK(!vsdbg().stage.stream_fullscreen && !g_fullscreen_state);

  // Keyboard shortcuts: 'F' enters fullscreen on stage; Escape exits; 'F' again and 'F' exits.
  key(CCORD_KEY_F, 0);
  frame(2);
  CT_CHECK(vsdbg().stage.stream_fullscreen && g_fullscreen_state);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!vsdbg().stage.stream_fullscreen && !g_fullscreen_state);
  key(CCORD_KEY_F, 0);
  frame(2);
  CT_CHECK(vsdbg().stage.stream_fullscreen);
  key(CCORD_KEY_F, 0);
  frame(2);
  CT_CHECK(!vsdbg().stage.stream_fullscreen && !g_fullscreen_state);

  // 5. Floating Picture-in-Picture (PiP) when switching to a text channel: resize, drag-move, fullscreen, return-to-stage.
  F.ui->stage_channel = CCORD_ID_NONE;
  ui_compute_layout(F.ui);
  frame(2);
  d = vsdbg();
  CT_CHECK(d.stage.stream_pip_visible);
  CT_CHECK(d.stage.stream_pip_rect.w >= 319.0f && d.stage.stream_pip_rect.w <= 321.0f);
  ccord_rect orig_pip = d.stage.stream_pip_rect;

  // Hover and drag top-left corner outward to resize PiP larger (bottom-right stays anchored).
  float tlx = orig_pip.x + 3.0f, tly = orig_pip.y + 3.0f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, tlx, tly);
  CT_CHECK(F.ui->cursor == CCORD_CURSOR_NWSE_RESIZE);
  drag_from_to(tlx, tly, tlx - 140.0f, tly - 80.0f);
  d = vsdbg();
  CT_CHECK(d.stage.stream_pip_visible);
  CT_CHECK(d.stage.stream_pip_rect.w > orig_pip.w + 100.0f);
  CT_CHECK(fabsf((d.stage.stream_pip_rect.x + d.stage.stream_pip_rect.w) - (orig_pip.x + orig_pip.w)) < 2.0f);
  shot("stream_pip_resized");

  // Drag body to move PiP left/up.
  ccord_rect before_move = d.stage.stream_pip_rect;
  float bx = before_move.x + before_move.w * 0.5f;
  float by = before_move.y + before_move.h * 0.65f;
  drag_from_to(bx, by, bx - 100.0f, by - 60.0f);
  d = vsdbg();
  CT_CHECK(d.stage.stream_pip_visible);
  CT_CHECK(d.stage.stream_pip_rect.x < before_move.x - 80.0f);
  CT_CHECK(d.stage.stream_pip_rect.y < before_move.y - 40.0f);

  // Fullscreen directly from PiP, then auto-exit fullscreen when the watched stream stops!
  click_rect(d.stage.stream_pip_fs_btn);
  frame(2);
  CT_CHECK(vsdbg().stage.stream_fullscreen && g_fullscreen_state);
  CT_CHECK(ccord_session_stop_watching(F.session, &err) == CCORD_OK);
  frame(3);
  d = vsdbg();
  CT_CHECK(!d.stage.stream_fullscreen && !g_fullscreen_state);
  CT_CHECK(!d.stage.stream_pip_visible);

  // Re-watch and verify PiP Return-to-Stage button reopens the voice stage.
  CT_CHECK(ccord_session_watch_stream(F.session, HQ, HQ_VOICE, ADA, &err) == CCORD_OK);
  frame(3);
  d = vsdbg();
  CT_CHECK(d.stage.stream_pip_visible && d.stage.stream_pip_stage_btn.w > 0.0f);
  click_rect(d.stage.stream_pip_stage_btn);
  frame(2);
  CT_CHECK(F.ui->stage_channel == HQ_VOICE && !vsdbg().stage.stream_pip_visible);

  // Clean up: stop watching and reset user-resized prefs so subsequent tests start cleanly.
  CT_CHECK(ccord_session_stop_watching(F.session, &err) == CCORD_OK);
  F.ui->stream_stage_height_frac = 0.0f;
  F.ui->stage_split_ratio = 0.55f;
  F.ui->stream_pip_w = 320.0f;
  F.ui->stream_pip_positioned = false;
  frame(2);
  leave_voice_and_close();
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_voice_surfaces")) return 77;
  test_dm_call();
  test_volume_and_mute();
  test_voice_status();
  test_stage();
  test_stage_sidebar();
  test_soundboard();
  test_golive_cards();
  test_golive_status();
  test_stage_text_chat();
  test_stream_card_switch();
  test_golive_share_audio();
  test_scheduled_events();
  test_stage_lifecycle();
  test_activities();
  test_dave_rtc();
  test_voice_bar_reconnecting();
  test_cam_layout_compute();
  test_cam_layout_bounds();
  test_cam_tiles_state();
  test_prejoin_mute_deafen_and_voice_bar_layout();
  test_vc_profile_click_and_right_click();
  test_stream_resize_fullscreen_pip();
  teardown();
  return CT_RESULT("test_ui_voice_surfaces");
}
