// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Offscreen UI test: real store + mock transport + session + renderer, fake OS host, injected input events.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/alloc.h>
#include <ccord/core/paths.h>
#include <ccord/core/utf8.h>
#include <ccord/model/audit.h>
#include <ccord/model/permissions.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/store/admin.h>
#include <ccord/store/expressions.h>
#include <ccord/store/roles.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "ui_voice_surfaces.h"

#include "ccord_test.h"

typedef struct fixture {
  ccord_theme theme;
  ccord_bus *bus;
  ccord_store *store;
  ccord_session *session;
  ccord_transport *transport;    // the mock behind `session` (request counters)
  ccord_contrib *contrib;
  ccord_platform *platform;
  ccord_renderer *renderer;
  ccord_ui *ui;
  uint64_t now;
  char clip[4096];
  int clip_calls;
  bool text_input_active;
  int menu_activations;
  ccord_menu_context last_ctx;
  char last_owner[64], last_item[64];
  int ext_calls;
  bool ext_last;
  int open_url_calls;
  char last_url[512];
  char pick_path[512];
} fixture;


typedef struct login_test_transport {
  ccord_transport base;
  int step;
  ccord_connection_state next_state;
  bool emit_state;
  bool emit_snap;
  ccord_snapshot snap;
  ccord_user self_user;
  ccord_guild guild;
  ccord_channel chan;
} login_test_transport;

extern const ccord_transport_vtable k_ltt_vt;

extern fixture F;

void ui_test_teardown(void);
int ui_test_harness_failures(void);
uint64_t fake_clock(void *ud);
void host_clip_set(void *ud, const char *s);
void host_text_input(void *ud, bool active, float x, float y, float w, float h);
void host_set_extensions(void *ud, bool on);
bool host_open_url(void *ud, const char *url);
bool host_open_file(void *ud, char *out_path, size_t cap);
void menu_activate(void *ud, const ccord_menu_contribution *item, const ccord_menu_context *ctx);
void frame(int n);
void draw_only(void);
void ui_send(ccord_input_event ev);
void mouse(ccord_input_type t, uint8_t button, float x, float y);
void click(float x, float y);
void seed_user_settings(bool developer_mode, bool animate_emoji, uint8_t animate_stickers);
void key(ccord_key k, uint8_t mods);
void type_text(const char *s);
ccord_ui_debug dbg(void);
void shot(const char *name);
float scan_hover_channel(float x, ccord_id want);
float scan_hover_message(float x);
float menu_item_center_y(float pointer_y, size_t count, int index, int seps_before);
bool menu_click_label(const char *label);
float scan_hover_self_message(float x);
float scan_hover_other_message(float x);
ccord_id find_channel(ccord_id guild, const char *name, ccord_channel_kind kind);
bool pixel_is(SDL_Surface *s, int x, int y, ccord_color want, int tol);
const char * ui_test_file(const char *rel);
bool setup(const char *argv0);
ccord_id append_rich_message(ccord_id ch, const char *content, const ccord_message_rich *rich);
void remove_message(ccord_id ch, ccord_id id);
bool surf_pixel_near(SDL_Surface *s, int x, int y, uint8_t r, uint8_t g, uint8_t b);
void save_optional_screenshot(const char *filename);
bool menu_has(const char *label);
void open_menu(ccord_menu_target target, ccord_id guild, ccord_id channel, ccord_id user, ccord_id message);
ccord_id other_member(ccord_id guild, size_t skip);
ccord_ui_server_settings_debug ssd(void);
bool ss_has(const char *name);
bool ss_click(const char *name);
bool ss_scroll_to(const char *name);
void frame_deaf(uint64_t jump_ms);
size_t member_header(const char *prefix);
size_t member_header_count(const char *prefix);
int menu_index(const char *prefix);
void menu_point_at(int idx);
void menu_click_index(int idx);
void clear_counts(ccord_id guild);
bool autocomplete_has(const char *substr);
void clear_composer_input(void);
void click_rect(ccord_rect r);
bool media_url_requested(const char *url);
