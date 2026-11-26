// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Smooth scroll verification (--verify-slice smooth-scroll); see verify_ui_parity.h.
//
// Every scrollable surface is a row of the table below. Each row's `prepare` opens the surface through the real UI and
// fills a probe; the row is then measured: the wheel event goes in through the host's input path, frames of 4, 8, 16 and
// 33 ms of virtual time are stepped through ctx->frame_ms (pump + ui update + draw), and the surface's own offset is read
// after each one. A row without `prepare` prints FAIL. The slice prints PASS only when every row is ok.
//
// Per measured surface:
//   - one notch: the first frame moves at most 35% of the notch (and more than 0), the offset is monotonic with no
//     overshoot, the final offset is the notch distance, and the settle time differs by at most 10% across frame steps
//   - a burst of 5 notches 16 ms apart ends exactly 5 notches away, monotonic, without overshoot
//   - a precise (trackpad-sized) delta is applied at once, not eased
//   - with reduced motion the offset is at its target on the first frame after the notch
// Set CCORD_SCROLL_TRACE=1 to print every frame's offset.
#include "verify_ui_parity.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/net/mock_transport.h>

#include "ui_internal.h"

// Per-surface wheel notch distances (each the surface's own handler step).
#define SS_VOICE_NOTCH 108.0f        // settings.c VOICE_SCROLL_STEP
#define SS_SERVER_NOTCH 52.0f        // server_settings.c ss_wheel_by
#define SS_EVENT_NOTCH 48.0f         // ui_scheduled_events.c wheel
#define SS_PICKER_NOTCH 105.0f       // expression_picker.c wheel (42 * 2.5)
#define SS_FORUM_NOTCH 42.0f         // draw_forum.c wheel
#define SS_QUICK_NOTCH 30.0f         // draw_chat.c quick switcher wheel
#define SS_CHANNEL_NOTCH 52.0f       // channel_settings.c cs_wheel_by (ev->wheel * 52)
#define SS_KEY_STEP 48.0f            // server_settings.c and channel_settings.c Up/Down key step
#define SS_TAB_EMOJI 2               // expression_picker.c EXPR_TAB_EMOJI
#define SS_SEARCH_TRIES 12           // queries tried until the results panel overflows
#define SS_EVENTS 30                 // scheduled events seeded into the home guild
#define SS_EVENT_ID0 960000u
#define SS_SHORT_H 280.0f            // a window this tall makes the settings and forum panels overflow
#define SS_QUICK_H 200.0f            // the switcher keeps 10 items; this height leaves its list 100 px tall
#define SS_CHANNEL_SHORT_H 150.0f    // Channel Settings overview: its view keeps at least 80 px, so this overflows by 6 notches
#define SS_VOICE_SHORT_H 200.0f      // Voice & Video is 822 px tall; here its view is 152 px, so it overflows by 670 px
#define SS_MENU_NOTCH 32.0f          // context_menu.c wheel: context_menu_item_height (UIS_H_BTN)
#define SS_MENU_H 150.0f             // a short window: the rail's context menu is capped to this height and overflows

// settings.c: the Voice & Video page offset and its travel (declared in the .c file only).
void ui_settings_voice_m5_debug(const ccord_ui *ui, ccord_rect *out_camera, ccord_rect *out_ptt_key,
                                ccord_rect *out_sens_track, ccord_rect *out_scroll_up, ccord_rect *out_scroll_down,
                                ccord_rect *out_diag, float *out_scroll, float *out_max_scroll, bool *out_in_voice);

#define SS_MAX_FRAMES 400
#define SS_RUN_MS 1200u
#define SS_EPS 0.05f
#define SS_BACKSTEP 0.001f           // float dust only: any real step back fails
#define SS_IDLE_WAKE_MS 100u         // the app loop idles this long between frames
#define SS_FIRST_FRAME_MAX 0.35f
#define SS_SETTLE_SPREAD_MAX 1.10f
#define SS_BURST 5
#define SS_EXTRA_GUILDS 24
#define SS_GUILD_ID0 910000u
#define SS_BUSY_GUILD 919000u        // a guild with enough channels and members to overflow its sidebars
#define SS_BUSY_CHANNELS 30
#define SS_BUSY_MEMBERS 40
#define SS_BUSY_USER0 929000u

typedef struct ss ss;

// How to drive one surface. `read` returns the offset in the direction the notch moves it (it grows with the notch).
typedef struct ss_probe {
  float x, y;                // wheel position over the surface
  float wheel;               // wheel value of one notch that grows the offset
  float notch_px;            // distance one notch must travel
  float (*read)(ss *s);
} ss_probe;

typedef struct ss_surface {
  const char *name;
  bool (*prepare)(ss *s, ss_probe *p);   // nullptr: not probed yet (prints FAIL)
  void (*close)(ss *s);                  // shuts the surface again so the next row starts clean (may be nullptr)
  float (*room)(ss *s);                  // set for a surface whose Down key steps the list: its scroll room (else nullptr)
} ss_surface;

struct ss {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  int rows_failed;
  char why[200];
  bool trace;
  size_t ref_row;            // timeline: the row whose y is the offset
  float save_w, save_h;
  ccord_id extra_guilds[SS_EXTRA_GUILDS];
  size_t n_extra;
  ccord_id home_guild;       // the guild the mock started in
  bool busy;                 // the busy guild exists
};

typedef struct ss_run {
  float d[SS_MAX_FRAMES];    // offset change since the notch, after each frame
  uint32_t t[SS_MAX_FRAMES]; // virtual ms since the first wheel event, after each frame
  int n;
} ss_run;

static bool ss_check(ss *s, bool ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool ss_check(ss *s, bool ok, const char *fmt, ...)
{
  if (ok) {
    s->checks++;
    return true;
  }
  if (!s->why[0]) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(s->why, sizeof s->why, fmt, ap);
    va_end(ap);
  }
  return false;
}

static void ss_input(ss *s, ccord_input_event ev) { s->c->input(s->c->ud, &ev); }
static void ss_frames(ss *s, int n) { s->c->frames(s->c->ud, n); }

static void ss_wheel(ss *s, const ss_probe *p, float notches)
{
  ss_input(s, (ccord_input_event){.type = CCORD_IN_WHEEL, .x = p->x, .y = p->y, .wheel = notches});
}

static void ss_click(ss *s, float x, float y)
{
  ss_input(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  ss_input(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  ss_input(s, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y});
  ss_frames(s, 2);
}

static void ss_key(ss *s, ccord_key k, uint8_t mods)
{
  ss_input(s, (ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = k, .mods = mods});
  ss_frames(s, 2);
}

static void ss_type(ss *s, const char *text)
{
  ccord_input_event ev = {.type = CCORD_IN_TEXT};
  (void)snprintf(ev.text, sizeof ev.text, "%s", text);
  ss_input(s, ev);
}

static float ss_notch_px(ss *s)
{
  ccord_ui *ui = s->c->ui;
  return 3.0f * ui_line_h(ui, ui->theme->m.font_size_base);
}

static void ss_center(ccord_layout_rect r, ss_probe *p)
{
  p->x = r.x + r.w * 0.5f;
  p->y = r.y + r.h * 0.5f;
}

// ---- offset readers -----------------------------------------------------------------------------------------------

static float ss_read_timeline(ss *s)
{
  // y of one fixed row relative to the viewport, from the anchor and the measured row heights between them
  const ccord_vlist *v = &s->c->ui->vlist;
  float y = v->anchor_offset;
  size_t r = s->ref_row;
  if (r >= v->anchor_index) {
    for (size_t i = v->anchor_index; i < r; i++) y += ccord_vlist_row_height(v, i);
  } else {
    for (size_t i = r; i < v->anchor_index; i++) y -= ccord_vlist_row_height(v, i);
  }
  return y;   // an older-ward notch moves the row down
}
static float ss_read_channels(ss *s) { return s->c->ui->chan_scroll; }
static float ss_read_members(ss *s) { return s->c->ui->member_scroll; }
static float ss_read_servers(ss *s) { return s->c->ui->server_scroll; }
static float ss_read_voice(ss *s)
{
  float v = 0.0f;
  ui_settings_voice_m5_debug(s->c->ui, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &v, nullptr, nullptr);
  return v;
}
static float ss_read_server_settings(ss *s)
{
  float v = 0.0f;
  ui_server_settings_scroll_debug(s->c->ui, &v, nullptr);
  return v;
}
static float ss_room_server_settings(ss *s)
{
  float room = 0.0f;
  ui_server_settings_scroll_debug(s->c->ui, nullptr, &room);
  return room;
}
static float ss_read_channel_settings(ss *s)
{
  float v = 0.0f;
  ui_channel_settings_scroll_debug(s->c->ui, &v, nullptr);
  return v;
}
static float ss_room_channel_settings(ss *s)
{
  float room = 0.0f;
  ui_channel_settings_scroll_debug(s->c->ui, nullptr, &room);
  return room;
}
static float ss_read_events(ss *s)
{
  ui_events_debug d;
  ui_scheduled_events_get_debug(s->c->ui, &d);
  return d.list_scroll;
}
static float ss_read_picker(ss *s)
{
  float v = 0.0f;
  ui_expr_scroll_debug(s->c->ui, &v, nullptr);
  return v;
}
static float ss_read_forum(ss *s)
{
  float v = 0.0f;
  ui_forum_scroll_debug(s->c->ui, nullptr, &v, nullptr);
  return v;
}
static float ss_read_search(ss *s) { return s->c->ui->search_panel_scroll; }
static float ss_read_quick_switcher(ss *s) { return s->c->ui->quick_switcher_scroll; }
static float ss_read_composer(ss *s) { return s->c->ui->composer_scroll; }

// ---- surface setup ------------------------------------------------------------------------------------------------

// A short window so the sidebars overflow.
static void ss_short_window(ss *s, float h)
{
  ccord_ui_set_viewport(s->c->ui, s->save_w, h);
  ss_frames(s, 3);
}

// Selects the home guild's last plain text channel that has history and returns its message count.
static size_t ss_select_history(ss *s)
{
  const ccord_verify_ui_parity_ctx *c = s->c;
  ccord_error err;
  ccord_id g = ccord_store_selected_guild(c->store);
  size_t have = 0;
  for (size_t i = ccord_store_channel_count(c->store, g); i > 0 && have < 30; i--) {
    const ccord_channel *ch = ccord_store_channel_at(c->store, g, i - 1);
    if (!ch || ch->kind != CCORD_CHANNEL_TEXT || ch->discord_type != 0) continue;
    if (ccord_session_select_channel(c->session, ch->id, &err) != CCORD_OK) continue;
    ss_frames(s, 20);
    have = ccord_store_message_count(c->store, ch->id);
  }
  return have;
}

static bool ss_prepare_timeline(ss *s, ss_probe *p)
{
  ccord_ui *ui = s->c->ui;
  size_t have = ss_select_history(s);
  if (!ss_check(s, have >= 30, "timeline: a text channel with history (found %zu messages)", have)) return false;
  const ccord_layout *l = ccord_ui_get_layout(ui);
  ss_center(l->message_area, p);
  p->wheel = 1.0f;   // towards older messages, content moves down
  p->notch_px = ss_notch_px(s);
  p->read = ss_read_timeline;
  ss_wheel(s, p, -4000.0f);   // newest message at the bottom
  ss_frames(s, 3);
  s->ref_row = ui->vlist.count >= 4 ? ui->vlist.count - 3 : 0;
  return ss_check(s, ui->vlist.count >= 30 && ui->vlist.stick_bottom, "timeline: opens at the newest message");
}

static bool ss_prepare_sidebar(ss *s, ss_probe *p, ccord_layout_rect view, float content_h, float (*read)(ss *),
                               const char *name)
{
  p->notch_px = ss_notch_px(s);
  ss_center(view, p);
  p->wheel = -1.0f;   // towards the end of the list, offset grows
  p->read = read;
  float room = content_h - view.h;
  return ss_check(s, room >= (float)(SS_BURST + 3) * p->notch_px, "%s: list overflows its view by %.0f px, needs %.0f", name,
                  (double)room, (double)((float)(SS_BURST + 3) * p->notch_px));
}

static bool ss_select_busy(ss *s)
{
  ccord_error err;
  if (!ss_check(s, s->busy, "the busy guild could not be created")) return false;
  if (!ss_check(s, ccord_store_select_guild(s->c->store, SS_BUSY_GUILD, &err) == CCORD_OK, "cannot select the busy guild")) {
    return false;
  }
  ss_frames(s, 10);
  return true;
}

static bool ss_prepare_channels(ss *s, ss_probe *p)
{
  if (!ss_select_busy(s)) return false;
  ss_short_window(s, 330.0f);
  ccord_ui *ui = s->c->ui;
  return ss_prepare_sidebar(s, p, ccord_ui_get_layout(ui)->channel_scroll, ui->chan_content_h, ss_read_channels, "channels");
}

static bool ss_prepare_members(ss *s, ss_probe *p)
{
  if (!ss_select_busy(s)) return false;
  ss_short_window(s, 330.0f);
  ccord_ui *ui = s->c->ui;
  const ccord_layout *l = ccord_ui_get_layout(ui);
  if (!ss_check(s, l->member_list_visible, "members: the member list is shown")) return false;
  return ss_prepare_sidebar(s, p, l->member_list, ui->member_content_h, ss_read_members, "members");
}

static bool ss_prepare_servers(ss *s, ss_probe *p)
{
  ss_short_window(s, 330.0f);
  ccord_ui *ui = s->c->ui;
  float total = 0.0f;
  (void)ui_rail_build_slots(ui, nullptr, 0, &total);
  return ss_prepare_sidebar(s, p, ccord_ui_get_layout(ui)->server_list, total, ss_read_servers, "servers");
}

// Probe that wheels over the centre of rect r.
static void ss_probe_at(ss_probe *p, ccord_rect r, float notch, float (*read)(ss *))
{
  p->x = r.x + r.w * 0.5f;
  p->y = r.y + r.h * 0.5f;
  p->wheel = -1.0f;   // towards the end of the list, offset grows
  p->notch_px = notch;
  p->read = read;
}

// The probe's list must hold a burst of SS_BURST notches plus one more notch of travel.
static bool ss_check_room(ss *s, float room, float notch, const char *name)
{
  return ss_check(s, room >= (float)(SS_BURST + 1) * notch, "%s: list overflows by %.0f px, needs %.0f", name, (double)room,
                  (double)((float)(SS_BURST + 1) * notch));
}

// Opens Settings through the user-panel gear (or keeps it open) and switches to the sidebar tab `tab`.
static bool ss_open_settings_tab(ss *s, int tab)
{
  ccord_ui *ui = s->c->ui;
  ccord_ui_settings_debug d;
  ccord_ui_get_settings_debug(ui, &d);
  if (!d.open) {
    const ccord_layout *l = ccord_ui_get_layout(ui);
    ss_click(s, l->user_panel.x + l->user_panel.w - 24.0f, l->user_panel.y + l->user_panel.h * 0.5f);
    ccord_ui_get_settings_debug(ui, &d);
    if (!ss_check(s, d.open, "settings: the user-panel gear opens the modal")) return false;
  }
  ss_click(s, d.tabs[tab].x + 10.0f, d.tabs[tab].y + 10.0f);
  ccord_ui_get_settings_debug(ui, &d);
  return ss_check(s, d.page == tab, "settings: sidebar tab %d opens (page %d)", tab, d.page);
}

static void ss_close_settings(ss *s) { ui_open_settings(s->c->ui, false); ss_frames(s, 2); }

static bool ss_prepare_user_settings(ss *s, ss_probe *p)
{
  ccord_ui *ui = s->c->ui;
  if (!ss_open_settings_tab(s, 3)) return false;   // 3 = Voice & Video, the long page
  ss_short_window(s, SS_VOICE_SHORT_H);
  float scroll = 0.0f, max = 0.0f;
  ui_settings_voice_m5_debug(ui, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &scroll, &max, nullptr);
  ss_probe_at(p, (ccord_rect){0.0f, 0.0f, (float)ui->win_w, (float)ui->win_h}, SS_VOICE_NOTCH, ss_read_voice);
  return ss_check_room(s, max, SS_VOICE_NOTCH, "user settings");
}

static bool ss_prepare_server_settings(ss *s, ss_probe *p)
{
  ccord_ui *ui = s->c->ui;
  if (!ss_check(s, ccord_ui_open_server_settings(ui, s->home_guild), "server settings: the home guild opens")) return false;
  ss_frames(s, 3);
  ss_short_window(s, SS_SHORT_H);
  float room = 0.0f;
  ui_server_settings_scroll_debug(ui, nullptr, &room);
  ss_probe_at(p, (ccord_rect){0.0f, 0.0f, (float)ui->win_w, (float)ui->win_h}, SS_SERVER_NOTCH, ss_read_server_settings);
  return ss_check_room(s, room, SS_SERVER_NOTCH, "server settings");
}

static void ss_close_server_settings(ss *s)
{
  ss_key(s, CCORD_KEY_ESCAPE, 0);
}

static bool ss_prepare_channel_settings(ss *s, ss_probe *p)
{
  ccord_ui *ui = s->c->ui;
  ccord_id g = s->home_guild;   // the guild Server Settings opens on: the account can manage it
  ccord_id ch = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(s->c->store, g) && ch == CCORD_ID_NONE; i++) {
    const ccord_channel *c = ccord_store_channel_at(s->c->store, g, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->discord_type == 0) ch = c->id;
  }
  if (!ss_check(s, ccord_ui_open_channel_settings(ui, ch), "channel settings: a text channel of the selected guild opens")) return false;
  ss_frames(s, 3);
  ss_short_window(s, SS_CHANNEL_SHORT_H);   // the overview is about 410 px: this height leaves room for the burst
  float room = 0.0f;
  ui_channel_settings_scroll_debug(ui, nullptr, &room);
  ss_probe_at(p, (ccord_rect){0.0f, 0.0f, (float)ui->win_w, (float)ui->win_h}, SS_CHANNEL_NOTCH, ss_read_channel_settings);
  return ss_check_room(s, room, SS_CHANNEL_NOTCH, "channel settings");
}

static void ss_close_channel_settings(ss *s)
{
  ui_channel_settings_close(s->c->ui);
  ss_frames(s, 2);
}

static void ss_seed_events(ss *s)
{
  ccord_error err;
  ccord_scheduled_event *ev = calloc(SS_EVENTS, sizeof *ev);
  if (!ev) return;
  for (size_t i = 0; i < SS_EVENTS; i++) {
    ev[i].id = SS_EVENT_ID0 + (ccord_id)i;
    ev[i].guild_id = s->home_guild;
    (void)snprintf(ev[i].name, sizeof ev[i].name, "Scroll event %zu", i);
    (void)snprintf(ev[i].location, sizeof ev[i].location, "Room %zu", i);
    ev[i].scheduled_start_ms = 1900000000000LL + (int64_t)i * 3600000LL;
    ev[i].privacy_level = 2;
    ev[i].status = CCORD_SCHEDULED_EVENT_SCHEDULED;
    ev[i].entity_type = CCORD_SCHEDULED_EVENT_EXTERNAL;
  }
  (void)ccord_store_set_scheduled_events(s->c->store, s->home_guild, ev, SS_EVENTS, &err);
  ccord_error_clear(&err);
  free(ev);
}

static bool ss_prepare_scheduled_events(ss *s, ss_probe *p)
{
  ccord_ui *ui = s->c->ui;
  ui_scheduled_events_open(ui, s->home_guild);
  ss_frames(s, 10);   // the list request answers through pump()
  ss_seed_events(s);
  ss_frames(s, 3);
  ui_events_debug d;
  ui_scheduled_events_get_debug(ui, &d);
  if (!ss_check(s, d.open && d.view == UI_EVENTS_LIST, "scheduled events: the list opens")) return false;
  ss_probe_at(p, d.list_view, SS_EVENT_NOTCH, ss_read_events);
  float room = d.list_content_h - d.list_view.h;
  return ss_check_room(s, room, SS_EVENT_NOTCH, "scheduled events");
}

static void ss_close_scheduled_events(ss *s)
{
  if (s->c->ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS) ss_key(s, CCORD_KEY_ESCAPE, 0);
}

static bool ss_prepare_picker(ss *s, ss_probe *p)
{
  ccord_ui *ui = s->c->ui;
  ccord_id ch = ccord_store_selected_channel(s->c->store);
  ui_expr_picker_open(ui, SS_TAB_EMOJI, ch, CCORD_ID_NONE);
  ss_frames(s, 8);
  if (!ss_check(s, ui->emoji_picker_open, "picker: the emoji picker opens")) return false;
  float room = 0.0f;
  ui_expr_scroll_debug(ui, nullptr, &room);
  ss_probe_at(p, ui->emoji_picker_rect, SS_PICKER_NOTCH, ss_read_picker);
  return ss_check_room(s, room, SS_PICKER_NOTCH, "picker");
}

static void ss_close_picker(ss *s) { ui_expr_picker_close(s->c->ui); }

static bool ss_prepare_forum(ss *s, ss_probe *p)
{
  ccord_ui *ui = s->c->ui;
  ccord_error err;
  ccord_id forum = CCORD_ID_NONE;
  for (size_t i = ccord_store_channel_count(s->c->store, s->home_guild); i > 0 && forum == CCORD_ID_NONE; i--) {
    const ccord_channel *ch = ccord_store_channel_at(s->c->store, s->home_guild, i - 1);
    if (ch && ch->discord_type == CCORD_DISCORD_CHANNEL_GUILD_FORUM) forum = ch->id;
  }
  if (!ss_check(s, forum != CCORD_ID_NONE, "forum: the home guild has a forum channel")) return false;
  if (!ss_check(s, ccord_session_select_channel(s->c->session, forum, &err) == CCORD_OK, "forum: the channel selects")) {
    ccord_error_clear(&err);
    return false;
  }
  ss_frames(s, 6);
  if (!ss_check(s, ui_forum_is_active(ui), "forum: the post list is shown")) return false;
  ss_short_window(s, SS_SHORT_H);
  ccord_rect area = {0.0f, 0.0f, 0.0f, 0.0f};
  float room = 0.0f;
  ui_forum_scroll_debug(ui, &area, nullptr, &room);
  ss_probe_at(p, area, SS_FORUM_NOTCH, ss_read_forum);
  return ss_check_room(s, room, SS_FORUM_NOTCH, "forum");
}

// Types `query` into the search box (opening it first) and waits for the answer. True when a search went out.
static bool ss_run_search(ss *s, const char *query)
{
  ccord_ui *ui = s->c->ui;
  if (!ui->search_open) {
    ccord_ui_debug d;
    ccord_ui_get_debug(ui, &d);
    ss_click(s, d.search_btn.x + d.search_btn.w * 0.5f, d.search_btn.y + d.search_btn.h * 0.5f);
    if (!ui->search_open) return false;
  } else {
    ss_key(s, CCORD_KEY_A, CCORD_MOD_CTRL);
    ss_key(s, CCORD_KEY_BACKSPACE, 0);
  }
  size_t before = ccord_mock_call_count(s->c->transport, CCORD_MOCK_CALL_SEARCH);
  ss_type(s, query);
  for (int i = 0; i < 60; i++) {
    ss_frames(s, 1);
    if (ccord_mock_call_count(s->c->transport, CCORD_MOCK_CALL_SEARCH) > before && !ccord_session_search_loading(s->c->session)) {
      ss_frames(s, 2);
      return true;
    }
  }
  return false;
}

// Searches words of the channel's recent messages until the results panel overflows by a burst.
static bool ss_search_until_overflow(ss *s)
{
  ccord_ui *ui = s->c->ui;
  ccord_id ch = ccord_store_selected_channel(s->c->store);
  size_t n = ccord_store_message_count(s->c->store, ch);
  int tries = 0;
  for (size_t i = n; i > 0 && tries < SS_SEARCH_TRIES; i--) {
    const ccord_message *m = ccord_store_message_at(s->c->store, ch, i - 1);
    if (!m || !m->content) continue;
    const char *p = m->content;
    while (*p && tries < SS_SEARCH_TRIES) {
      while (*p && !isalpha((unsigned char)*p)) p++;
      const char *w = p;
      while (isalpha((unsigned char)*p)) p++;
      size_t wl = (size_t)(p - w);
      if (wl < 4 || wl >= 32) continue;
      char q[32];
      memcpy(q, w, wl);
      q[wl] = '\0';
      tries++;
      if (!ss_run_search(s, q)) continue;
      if (ui->search_panel_max >= (float)(SS_BURST + 1) * ss_notch_px(s)) return true;
    }
  }
  return false;
}

static bool ss_prepare_search(ss *s, ss_probe *p)
{
  ccord_ui *ui = s->c->ui;
  size_t have = ss_select_history(s);
  if (!ss_check(s, have >= 30, "search results: a text channel with history")) return false;
  if (!ss_check(s, ss_search_until_overflow(s), "search results: a query returns enough results to overflow the panel")) {
    return false;
  }
  const ccord_layout *l = ccord_ui_get_layout(ui);
  ss_probe_at(p, (ccord_rect){l->member_list.x, l->member_list.y, l->member_list.w, l->member_list.h}, ss_notch_px(s),
              ss_read_search);
  return ss_check(s, ui->search_open, "search results: the results panel is open");
}

static void ss_close_search(ss *s)
{
  if (s->c->ui->search_open) ss_key(s, CCORD_KEY_ESCAPE, 0);
}

static bool ss_prepare_quick_switcher(ss *s, ss_probe *p)
{
  ccord_ui *ui = s->c->ui;
  // The busy guild lists its 30 channels, enough for the switcher to scroll.
  ccord_error err;
  if (s->busy && ccord_store_select_guild(s->c->store, SS_BUSY_GUILD, &err) != CCORD_OK) ccord_error_clear(&err);
  ss_frames(s, 4);
  ss_short_window(s, SS_QUICK_H);
  ss_key(s, CCORD_KEY_K, CCORD_MOD_CTRL);
  ss_frames(s, 3);
  if (!ss_check(s, ui->quick_switcher_open, "quick switcher: Ctrl+K opens the popover")) return false;
  ccord_rect m = ui_quick_switcher_rect(ui);
  ccord_rect list = {m.x + 12.0f, m.y + 66.0f, m.w - 24.0f, m.h - 76.0f};
  // A wheel offset must not survive a reopen or a typed filter: both start at the first row, at the top.
  ss_probe wq = {0};
  ss_probe_at(&wq, list, SS_QUICK_NOTCH, ss_read_quick_switcher);
  ss_wheel(s, &wq, -4000.0f);   // to the bottom
  ss_frames(s, 2);
  if (!ss_check(s, ui->quick_switcher_scroll > 0.0f, "quick switcher: the wheel scrolls the list before the reopen")) return false;
  ss_key(s, CCORD_KEY_ESCAPE, 0);
  ss_key(s, CCORD_KEY_K, CCORD_MOD_CTRL);
  ss_frames(s, 3);
  if (!ss_check(s, ui->quick_switcher_open && ui->quick_switcher_scroll == 0.0f && ui->quick_switcher_sel == 0,
                "quick switcher: a reopen starts at the top (scroll %.1f)", (double)ui->quick_switcher_scroll)) return false;
  size_t all = ui->nquick_switcher_items;
  ss_wheel(s, &wq, -4000.0f);
  ss_frames(s, 2);
  // "scroll-2" leaves the busy guild's 10 capped rows, still taller than the view: a kept offset would show.
  ss_input(s, (ccord_input_event){.type = CCORD_IN_TEXT, .text = "scroll-2"});
  ss_frames(s, 3);
  if (!ss_check(s, ui->nquick_switcher_items >= 4 && ui->quick_switcher_scroll == 0.0f,
                "quick switcher: typing a filter starts at the top (scroll %.1f, %zu rows)",
                (double)ui->quick_switcher_scroll, ui->nquick_switcher_items)) return false;
  for (int i = 0; i < 8; i++) ss_key(s, CCORD_KEY_BACKSPACE, 0);   // clear the 8-character filter for the measured list
  ss_frames(s, 3);
  if (!ss_check(s, ui->nquick_switcher_items == all, "quick switcher: clearing the filter restores all %zu rows", all)) return false;
  float room = (float)ui->nquick_switcher_items * SS_QUICK_NOTCH - list.h;
  ss_probe_at(p, list, SS_QUICK_NOTCH, ss_read_quick_switcher);
  return ss_check_room(s, room, SS_QUICK_NOTCH, "quick switcher");
}

static float ss_read_menu(ss *s)
{
  float scroll = 0.0f;
  ui_menu_scroll_debug(s->c->ui, &scroll, nullptr);
  return scroll;
}

// A popover: the server rail's context menu, opened by a right click on the rail. It is capped to the window height,
// so at SS_MENU_H its items overflow and the wheel scrolls them.
static bool ss_prepare_popovers(ss *s, ss_probe *p)
{
  ccord_ui *ui = s->c->ui;
  if (!ss_select_busy(s)) return false;
  ss_short_window(s, SS_MENU_H);
  ccord_layout_rect rail = ccord_ui_get_layout(ui)->server_list;
  ss_probe rp = {0};
  ss_center(rail, &rp);
  ss_input(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = rp.x, .y = rp.y});
  ss_input(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 3, .clicks = 1, .x = rp.x, .y = rp.y});
  ss_frames(s, 3);
  if (!ss_check(s, ui->menu.open, "popovers: a right click on the server rail opens its context menu")) return false;
  float room = 0.0f;
  ui_menu_scroll_debug(ui, nullptr, &room);
  ccord_rect m = {ui->menu.x, ui->menu.y, ui->menu.w, ui->menu.h};
  ss_probe_at(p, m, SS_MENU_NOTCH, ss_read_menu);
  return ss_check_room(s, room, SS_MENU_NOTCH, "popovers");
}

static void ss_close_popovers(ss *s)
{
  if (s->c->ui->menu.open) ss_key(s, CCORD_KEY_ESCAPE, 0);
}

static void ss_close_quick_switcher(ss *s)
{
  if (s->c->ui->quick_switcher_open) ss_key(s, CCORD_KEY_ESCAPE, 0);
}

static bool ss_prepare_composer(ss *s, ss_probe *p)
{
  ccord_ui *ui = s->c->ui;
  ccord_error err;
  if (s->home_guild != CCORD_ID_NONE && ccord_store_select_guild(s->c->store, s->home_guild, &err) != CCORD_OK) {
    ccord_error_clear(&err);
  }
  ss_frames(s, 3);
  size_t have = ss_select_history(s);
  if (!ss_check(s, have > 0, "composer: a text channel is selected")) return false;
  ccord_text_edit_clear(&ui->edit);
  char buf[1024];
  size_t pos = 0;
  for (int i = 0; i < 40 && pos + 20 < sizeof buf; i++) {
    int n = snprintf(buf + pos, sizeof buf - pos, "Line %02d\n", i + 1);
    if (n > 0) pos += (size_t)n;
  }
  if (!ss_check(s, ccord_text_edit_insert(&ui->edit, buf, pos, &err) == CCORD_OK, "composer: insert multiline text")) {
    ccord_error_clear(&err);
    return false;
  }
  ss_frames(s, 4);
  if (!ss_check(s, ui->composer_scroll_rect.w > 0.0f && ui->composer_scroll_max > 0.0f,
                "composer: multiline text overflows (max %.1f)", (double)ui->composer_scroll_max)) {
    return false;
  }
  ss_probe_at(p, ui->composer_scroll_rect, ss_notch_px(s), ss_read_composer);
  return ss_check_room(s, ui->composer_scroll_max, p->notch_px, "composer");
}

static void ss_close_composer(ss *s)
{
  ccord_text_edit_clear(&s->c->ui->edit);
  ui_composer_reset_scroll(s->c->ui);
  ss_frames(s, 2);
}

static const ss_surface ss_surfaces[] = {
    {"timeline", ss_prepare_timeline, nullptr, nullptr},
    {"composer", ss_prepare_composer, ss_close_composer, nullptr},
    {"channels", ss_prepare_channels, nullptr, nullptr},
    {"members", ss_prepare_members, nullptr, nullptr},
    {"servers", ss_prepare_servers, nullptr, nullptr},
    {"settings", ss_prepare_user_settings, ss_close_settings, nullptr},
    {"server-settings", ss_prepare_server_settings, ss_close_server_settings, ss_room_server_settings},
    {"channel-settings", ss_prepare_channel_settings, ss_close_channel_settings, ss_room_channel_settings},
    {"events", ss_prepare_scheduled_events, ss_close_scheduled_events, nullptr},
    {"pickers", ss_prepare_picker, ss_close_picker, nullptr},
    {"forum", ss_prepare_forum, nullptr, nullptr},
    {"search-results", ss_prepare_search, ss_close_search, nullptr},
    {"quick-switcher", ss_prepare_quick_switcher, ss_close_quick_switcher, nullptr},
    {"popovers", ss_prepare_popovers, ss_close_popovers, nullptr},
};

// ---- measurement --------------------------------------------------------------------------------------------------

// Moves the surface back to its start (a direct jump) and lets it settle.
static void ss_reset(ss *s, const ss_probe *p)
{
  ss_wheel(s, p, -p->wheel * 4000.0f);
  ss_frames(s, 4);
}

static void ss_record(ss *s, const ss_probe *p, ss_run *r, float base, uint32_t ms, uint32_t *clock)
{
  s->c->frame_ms(s->c->ud, ms);
  *clock += ms;
  if (r->n < SS_MAX_FRAMES) {
    r->d[r->n] = p->read(s) - base;
    r->t[r->n] = *clock;
    r->n++;
  }
}

// `notches` wheel events spaced by `gap_ms` (the event lands between frames), then frames of step_ms until SS_RUN_MS.
static void ss_run_notches(ss *s, const ss_probe *p, int notches, uint32_t gap_ms, uint32_t step_ms, ss_run *r, const char *tag)
{
  memset(r, 0, sizeof *r);
  ss_reset(s, p);
  float base = p->read(s);
  uint32_t clock = 0;
  for (int i = 0; i < notches; i++) {
    ss_wheel(s, p, p->wheel);
    if (i + 1 < notches) {
      uint32_t spent = 0;
      while (spent < gap_ms) {
        uint32_t ms = gap_ms - spent < step_ms ? gap_ms - spent : step_ms;
        ss_record(s, p, r, base, ms, &clock);
        spent += ms;
      }
    }
  }
  while (clock < SS_RUN_MS && r->n < SS_MAX_FRAMES) ss_record(s, p, r, base, step_ms, &clock);
  if (s->trace) {
    for (int i = 0; i < r->n; i++) printf("trace %s %s step=%u t=%u d=%.3f\n", tag, "", step_ms, r->t[i], (double)r->d[i]);
  }
}

// Monotonic towards `total`, never past it, and ends on it.
static bool ss_check_curve(ss *s, const ss_run *r, float total, const char *name, const char *what)
{
  float prev = 0.0f;
  for (int i = 0; i < r->n; i++) {
    if (!ss_check(s, r->d[i] >= prev - SS_BACKSTEP, "%s: %s offset moves back at frame %d (%.2f after %.2f)", name, what, i,
                  (double)r->d[i], (double)prev)) return false;
    if (!ss_check(s, r->d[i] <= total + SS_EPS, "%s: %s overshoots (%.2f of %.2f)", name, what, (double)r->d[i],
                  (double)total)) return false;
    prev = r->d[i];
  }
  return ss_check(s, fabsf(r->d[r->n - 1] - total) <= SS_EPS, "%s: %s ends at %.2f, not the distance %.2f", name, what,
                  (double)r->d[r->n - 1], (double)total);
}

// First frame (ms) from which the offset stays on the target.
static uint32_t ss_settle_ms(const ss_run *r, float total)
{
  int first = r->n;
  for (int i = r->n - 1; i >= 0 && fabsf(r->d[i] - total) <= SS_EPS; i--) first = i;
  return first < r->n ? r->t[first] : 0u;
}

// A Down key pressed part way through a wheel glide leaves the list at the offset at the press plus one key step,
// clamped. A glide that carries on after the key ends further on, so the settled offset catches it.
static bool ss_check_key_mid_glide(ss *s, const ss_surface *sf, const ss_probe *p)
{
  ss_reset(s, p);
  ss_wheel(s, p, p->wheel);
  s->c->frame_ms(s->c->ud, 16);
  float at_press = p->read(s);
  if (!ss_check(s, at_press > SS_EPS && at_press < p->notch_px - SS_EPS,
                "%s: the wheel glide is not part way at the key press (%.2f of %.2f px)", sf->name, (double)at_press,
                (double)p->notch_px)) return false;
  ss_key(s, CCORD_KEY_DOWN, 0);
  ss_frames(s, 20);
  float want = fminf(at_press + SS_KEY_STEP, sf->room(s));
  float settled = p->read(s);
  return ss_check(s, fabsf(settled - want) <= SS_EPS,
                  "%s: a Down key pressed mid glide settled at %.2f, expected %.2f (the press offset %.2f plus %.0f px, clamped)",
                  sf->name, (double)settled, (double)want, (double)at_press, (double)SS_KEY_STEP);
}

static bool ss_measure(ss *s, const ss_surface *sf, const ss_probe *p)
{
  static const uint32_t steps[] = {4, 8, 16, 33};
  uint32_t settle[4] = {0};
  ss_run *r = malloc(sizeof *r);
  if (!r) return ss_check(s, false, "%s: out of memory", sf->name);
  bool ok = true;

  // one notch at every frame step
  for (size_t k = 0; k < 4 && (ok || s->trace); k++) {
    char tag[48];
    (void)snprintf(tag, sizeof tag, "%s one-notch", sf->name);
    ss_run_notches(s, p, 1, 0, steps[k], r, tag);
    bool step_ok = ss_check(s, r->d[0] > SS_EPS * 0.01f, "%s: nothing moves on the first %u ms frame", sf->name, steps[k]) &&
         ss_check(s, r->d[0] <= SS_FIRST_FRAME_MAX * p->notch_px,
                  "%s: first %u ms frame moves %.1f px = %.0f%% of the notch (limit 35%%)", sf->name, steps[k],
                  (double)r->d[0], (double)(100.0f * r->d[0] / p->notch_px)) &&
         ss_check_curve(s, r, p->notch_px, sf->name, "one notch");
    ok = ok && step_ok;
    settle[k] = ss_settle_ms(r, p->notch_px);
  }
  if (ok) {
    uint32_t lo = settle[0], hi = settle[0];
    for (size_t k = 1; k < 4; k++) {
      if (settle[k] < lo) lo = settle[k];
      if (settle[k] > hi) hi = settle[k];
    }
    ok = ss_check(s, lo > 0 && (float)hi <= (float)lo * SS_SETTLE_SPREAD_MAX,
                  "%s: settle time differs across frame steps (%u/%u/%u/%u ms)", sf->name, settle[0], settle[1], settle[2],
                  settle[3]);
    if (s->trace) printf("trace %s settle ms: 4=%u 8=%u 16=%u 33=%u\n", sf->name, settle[0], settle[1], settle[2], settle[3]);
  }

  // the first frame after a notch is an idle wake-up (the app loop sleeps up to 100 ms), then frames run at 16 ms
  if (ok) {
    memset(r, 0, sizeof *r);
    ss_reset(s, p);
    float base = p->read(s);
    uint32_t clock = 0;
    ss_wheel(s, p, p->wheel);
    ss_record(s, p, r, base, SS_IDLE_WAKE_MS, &clock);
    while (clock < SS_RUN_MS + SS_IDLE_WAKE_MS && r->n < SS_MAX_FRAMES) ss_record(s, p, r, base, 16, &clock);
    ok = ss_check(s, r->d[0] <= SS_FIRST_FRAME_MAX * p->notch_px,
                  "%s: the first frame after an idle wait moves %.1f px = %.0f%% of the notch (limit 35%%)", sf->name,
                  (double)r->d[0], (double)(100.0f * r->d[0] / p->notch_px)) &&
         ss_check_curve(s, r, p->notch_px, sf->name, "one notch after an idle wait");
  }

  // a burst, at two frame steps
  for (size_t k = 1; k < 3 && (ok || s->trace); k++) {
    char tag[48];
    (void)snprintf(tag, sizeof tag, "%s burst", sf->name);
    ss_run_notches(s, p, SS_BURST, 16, steps[k], r, tag);
    ok = ss_check_curve(s, r, (float)SS_BURST * p->notch_px, sf->name, "burst of 5 notches") && ok;
  }

  // a precise delta is applied at once
  if (ok) {
    ss_reset(s, p);
    float base = p->read(s);
    ss_wheel(s, p, p->wheel * 0.25f);
    s->c->frame_ms(s->c->ud, 16);
    float got = p->read(s) - base;
    ok = ss_check(s, fabsf(got - 0.25f * p->notch_px) <= SS_EPS, "%s: a 0.25 notch (trackpad) delta moved %.2f px, expected %.2f at once",
                  sf->name, (double)got, (double)(0.25f * p->notch_px));
  }

  // reduced motion: at the target on the first frame
  if (ok) {
    ccord_ui *ui = s->c->ui;
    bool saved = ui->settings_reduced_motion;
    ui->settings_reduced_motion = true;
    ui->anim.reduced_motion = true;
    ss_reset(s, p);
    float base = p->read(s);
    ss_wheel(s, p, p->wheel);
    s->c->frame_ms(s->c->ud, 16);
    float got = p->read(s) - base;
    ui->settings_reduced_motion = saved;
    ui->anim.reduced_motion = saved;
    ok = ss_check(s, fabsf(got - p->notch_px) <= SS_EPS, "%s: reduced motion moved %.2f px on the first frame, expected %.2f",
                  sf->name, (double)got, (double)p->notch_px);
  }

  // a Down key during a glide replaces it (keyboard scrolling surfaces only)
  if (ok && sf->room) ok = ss_check_key_mid_glide(s, sf, p);
  free(r);
  return ok;
}

// ---- driver -------------------------------------------------------------------------------------------------------

static void ss_add_guilds(ss *s)
{
  // enough servers for the rail to overflow a short window
  ccord_error err;
  char name[32];
  for (size_t i = 0; i < SS_EXTRA_GUILDS; i++) {
    (void)snprintf(name, sizeof name, "Scroll %zu", i);
    ccord_guild g = {.id = SS_GUILD_ID0 + (ccord_id)i, .name = name, .icon_url = ""};
    if (ccord_store_upsert_guild(s->c->store, &g, nullptr, 0, nullptr, 0, nullptr, 0, &err) != CCORD_OK) {
      ccord_error_clear(&err);
      continue;
    }
    s->extra_guilds[s->n_extra++] = g.id;
  }
  ss_frames(s, 3);
}

static void ss_add_busy_guild(ss *s)
{
  ccord_error err;
  ccord_channel *chans = calloc(SS_BUSY_CHANNELS, sizeof *chans);
  ccord_member *mems = calloc(SS_BUSY_MEMBERS, sizeof *mems);
  ccord_user *users = calloc(SS_BUSY_MEMBERS, sizeof *users);
  char (*names)[32] = calloc(SS_BUSY_CHANNELS + SS_BUSY_MEMBERS, sizeof *names);
  if (chans && mems && users && names) {
    for (size_t i = 0; i < SS_BUSY_CHANNELS; i++) {
      (void)snprintf(names[i], sizeof names[i], "scroll-%zu", i);
      chans[i] = (ccord_channel){.id = SS_BUSY_GUILD + 1 + (ccord_id)i, .guild_id = SS_BUSY_GUILD, .kind = CCORD_CHANNEL_TEXT,
                                 .name = names[i], .topic = "", .position = (int32_t)i};
    }
    for (size_t i = 0; i < SS_BUSY_MEMBERS; i++) {
      char *nm = names[SS_BUSY_CHANNELS + i];
      (void)snprintf(nm, sizeof names[0], "Scroller %zu", i);
      users[i] = (ccord_user){.id = SS_BUSY_USER0 + (ccord_id)i, .name = nm, .handle = nm, .status_text = "", .avatar_url = "",
                              .presence = CCORD_PRESENCE_ONLINE};
      mems[i] = (ccord_member){.guild_id = SS_BUSY_GUILD, .user_id = users[i].id, .role_name = "Scrollers"};
    }
    ccord_guild g = {.id = SS_BUSY_GUILD, .name = "Scroll busy", .icon_url = ""};
    if (ccord_store_upsert_guild(s->c->store, &g, chans, SS_BUSY_CHANNELS, mems, SS_BUSY_MEMBERS, users, SS_BUSY_MEMBERS,
                                 &err) == CCORD_OK) {
      s->busy = true;
    } else {
      ccord_error_clear(&err);
    }
  }
  free(chans);
  free(mems);
  free(users);
  free(names);
}

int ccord_verify_smooth_scroll_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  ss s = {.c = ctx, .trace = getenv("CCORD_SCROLL_TRACE") != nullptr};
  ccord_ui *ui = ctx->ui;
  if (!ctx->frame_ms || !ctx->input) {
    printf("smooth-scroll: FAIL the host gives no frame stepping\n");
    fflush(stdout);
    return 1;
  }
  ss_frames(&s, 10);
  s.save_w = ui->win_w;
  s.save_h = ui->win_h;
  s.home_guild = ccord_store_selected_guild(ctx->store);
  ss_add_guilds(&s);
  ss_add_busy_guild(&s);

  size_t nrows = sizeof ss_surfaces / sizeof ss_surfaces[0];
  for (size_t i = 0; i < nrows; i++) {
    const ss_surface *sf = &ss_surfaces[i];
    s.why[0] = '\0';
    ss_probe p = {0};
    bool ok = false;
    if (!sf->prepare) {
      (void)snprintf(s.why, sizeof s.why, "not on the shared scroller yet");
    } else {
      ok = sf->prepare(&s, &p) && ss_measure(&s, sf, &p);
    }
    if (sf->close) sf->close(&s);
    ccord_ui_set_viewport(ui, s.save_w, s.save_h);
    ss_frames(&s, 3);
    if (ok) {
      s.checks++;
      printf("smooth-scroll %s: ok\n", sf->name);
    } else {
      s.rows_failed++;
      printf("smooth-scroll %s: FAIL %s\n", sf->name, s.why[0] ? s.why : "check failed");
    }
  }

  ccord_error err;
  if (s.home_guild != CCORD_ID_NONE && ccord_store_select_guild(ctx->store, s.home_guild, &err) != CCORD_OK) {
    ccord_error_clear(&err);
  }
  for (size_t i = 0; i < s.n_extra; i++) {
    if (ccord_store_delete_guild(ctx->store, s.extra_guilds[i], &err) != CCORD_OK) ccord_error_clear(&err);
  }
  if (s.busy && ccord_store_delete_guild(ctx->store, SS_BUSY_GUILD, &err) != CCORD_OK) ccord_error_clear(&err);
  ccord_ui_set_viewport(ui, s.save_w, s.save_h);

  if (s.rows_failed) {
    printf("smooth-scroll surfaces: FAIL %d of %zu surfaces\n", s.rows_failed, nrows);
    printf("smooth-scroll: FAIL %d surface(s) not smooth\n", s.rows_failed);
    fflush(stdout);
    return 1;
  }
  printf("smooth-scroll surfaces: ok\n");
  printf("smooth-scroll: PASS checks=%d\n", s.checks);
  fflush(stdout);
  return 0;
}
