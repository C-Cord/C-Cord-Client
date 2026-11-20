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

#include "ui_interaction_harness.h"

fixture F;

uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

void host_clip_set(void *ud, const char *s)
{
  (void)ud;
  snprintf(F.clip, sizeof F.clip, "%s", s);
  F.clip_calls++;
}

void host_text_input(void *ud, bool active, float x, float y, float w, float h)
{
  (void)ud, (void)x, (void)y, (void)w, (void)h;
  F.text_input_active = active;
}

void host_set_extensions(void *ud, bool on)
{
  (void)ud;
  F.ext_calls++;
  F.ext_last = on;
}

bool host_open_url(void *ud, const char *url)
{
  (void)ud;
  F.open_url_calls++;
  snprintf(F.last_url, sizeof F.last_url, "%s", url ? url : "");
  return true;
}

bool host_open_file(void *ud, char *out_path, size_t cap)
{
  (void)ud;
  if (!F.pick_path[0] || !out_path || cap == 0) return false;
  snprintf(out_path, cap, "%s", F.pick_path);
  return true;
}

void menu_activate(void *ud, const ccord_menu_contribution *item, const ccord_menu_context *ctx)
{
  (void)ud;
  F.menu_activations++;
  F.last_ctx = *ctx;
  snprintf(F.last_owner, sizeof F.last_owner, "%s", item->owner);
  snprintf(F.last_item, sizeof F.last_item, "%s", item->item_id);
}

void frame(int n)
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

void draw_only(void)
{
  F.now += 50;
  (void)ccord_ui_update(F.ui, F.now);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_render_end(F.renderer);
}

void ui_send(ccord_input_event ev)
{
  (void)ccord_ui_handle_input(F.ui, &ev);
}

void mouse(ccord_input_type t, uint8_t button, float x, float y)
{
  ui_send((ccord_input_event){.type = t, .button = button, .clicks = 1, .x = x, .y = y});
}

void click(float x, float y)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  frame(2);
}

void seed_user_settings(bool developer_mode, bool animate_emoji, uint8_t animate_stickers)
{
  ccord_error err = {0};
  ccord_user_settings settings = {0};
  settings.developer_mode = developer_mode;
  settings.animate_emoji = animate_emoji;
  settings.animate_stickers = animate_stickers;
  settings.gif_auto_play = true;
  settings.friend_source_all = true;
  settings.friend_source_mutual_friends = true;
  settings.friend_source_mutual_guilds = true;
  snprintf(settings.locale, sizeof settings.locale, "en-US");
  CT_CHECK(ccord_store_set_user_settings(F.store, &settings, &err) == CCORD_OK);
}

void key(ccord_key k, uint8_t mods)
{
  ui_send((ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = k, .mods = mods});
}

void type_text(const char *s)
{
  size_t left = strlen(s);
  while (left > 0) {
    ccord_input_event ev = {.type = CCORD_IN_TEXT};
    size_t take = left < sizeof ev.text - 1u ? left : sizeof ev.text - 1u;
    while (take < left && take > 1 && ((uint8_t)s[take] & 0xC0u) == 0x80u) take--;
    memcpy(ev.text, s, take);
    ui_send(ev);
    s += take;
    left -= take;
  }
}

ccord_ui_debug dbg(void)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(F.ui, &d);
  return d;
}

void shot(const char *name)
{
  const char *dir = getenv("CCORD_SHOT_DIR");
  if (!dir || !*dir) return;
  char path[512];
  snprintf(path, sizeof path, "%s/%s.png", dir, name);
  F.now += 50;
  (void)ccord_ui_update(F.ui, F.now);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_error serr;
  (void)ccord_render_screenshot(F.renderer, path, &serr);
  ccord_render_end(F.renderer);
}

float scan_hover_channel(float x, ccord_id want)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  for (float y = l->channel_scroll.y + 2; y < l->channel_scroll.y + l->channel_scroll.h; y += 3.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
    if (dbg().hover_channel == want) return y;
  }
  return -1.0f;
}

float scan_hover_message(float x)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  for (float y = l->message_area.y + 60; y < l->message_area.y + l->message_area.h; y += 3.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
    if (dbg().hover_message != CCORD_ID_NONE) return y;
  }
  return -1.0f;
}

float menu_item_center_y(float pointer_y, size_t count, int index, int seps_before)
{
  const float pad = F.theme.m.context_menu_pad;
  const float ih = F.theme.m.context_menu_item_height;
  const float sep = 9.0f;
  float h = 2.0f * pad + (float)count * ih + 2.0f * sep;
  float my = pointer_y;
  if (my + h > 800.0f - 4.0f) my = 800.0f - h - 4.0f;
  if (my < 4.0f) my = 4.0f;
  return my + pad + (float)index * ih + (float)seps_before * sep + ih * 0.5f;
}

bool menu_click_label(const char *label)
{
  for (size_t i = 0; i < F.ui->menu.count; i++) {
    const menu_item *it = &F.ui->menu.items[i];
    if (strcmp(it->label, label) != 0) continue;
    if (it->parent >= 0 && it->parent != F.ui->menu.open_sub) continue;
    ccord_rect r = ui_menu_item_rect(F.ui, i);
    mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    frame(1);
    mouse(CCORD_IN_MOUSE_DOWN, 1, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    frame(1);
    return true;
  }
  return false;
}

float scan_hover_self_message(float x)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ccord_id self = ccord_store_self_id(F.store);
  for (float y = l->message_area.y + 60; y < l->message_area.y + l->message_area.h; y += 3.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
    ccord_id id = dbg().hover_message;
    if (id == CCORD_ID_NONE) continue;
    const ccord_message *m = ccord_store_find_message(F.store, ccord_store_selected_channel(F.store), id);
    // M0-3: pending/failed local echoes are not editable/deletable yet; the action tests target confirmed rows.
    if (m && m->author_id == self && m->send_state == CCORD_MSG_SEND_CONFIRMED) return y;
  }
  return -1.0f;
}

float scan_hover_other_message(float x)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  ccord_id self = ccord_store_self_id(F.store);
  for (float y = l->message_area.y + 60; y < l->message_area.y + l->message_area.h; y += 3.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
    ccord_id id = dbg().hover_message;
    if (id == CCORD_ID_NONE) continue;
    const ccord_message *m = ccord_store_find_message(F.store, ccord_store_selected_channel(F.store), id);
    if (m && m->author_id != self) return y;
  }
  return -1.0f;
}

ccord_id find_channel(ccord_id guild, const char *name, ccord_channel_kind kind)
{
  for (size_t i = 0; i < ccord_store_channel_count(F.store, guild); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, guild, i);
    if (c->kind == kind && strcmp(c->name, name) == 0) return c->id;
  }
  return CCORD_ID_NONE;
}

bool pixel_is(SDL_Surface *s, int x, int y, ccord_color want, int tol)
{
  uint8_t r, g, b, a;
  if (!SDL_ReadSurfacePixel(s, x, y, &r, &g, &b, &a)) return false;
  return abs(r - want.r) <= tol && abs(g - want.g) <= tol && abs(b - want.b) <= tol;
}

const char *ui_test_file(const char *rel)
{
  static char bufs[4][1024];
  static unsigned idx = 0;
  char *b = bufs[idx++ & 3u];
  snprintf(b, 1024, "%s/%s", ccord_test_init_dir("ui_test_data"), rel);
  return b;
}

bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
  // Keep the settings INI and drafts JSON hermetic across test runs.
  ccord_paths_set_data_dir(ccord_test_init_dir("ui_test_data"));
  remove(ui_test_file("settings.ini"));
  remove(ui_test_file("drafts.json"));
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK) return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 3000, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  F.transport = tr;
  F.session = ccord_session_create(F.bus, F.store, tr, &err);
  if (!F.bus || !F.store || !F.contrib || !F.session) return false;
  CT_CHECK(ccord_contrib_add_menu(F.contrib, "test.plugin", "copy-id", "Copy Message ID", CCORD_MENU_MESSAGE, &err) == CCORD_OK);
  CT_CHECK(ccord_contrib_add_menu(F.contrib, "test.plugin", "chan-item", "Channel Thing", CCORD_MENU_CHANNEL, &err) == CCORD_OK);
  ccord_contrib_set_handlers(F.contrib, &(ccord_contrib_handlers){.menu_activate = menu_activate});
  ccord_platform_config pc = {.title = "ui-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .host = {.clipboard_set = host_clip_set, .text_input = host_text_input, .set_extensions_enabled = host_set_extensions, .open_url = host_open_url, .open_file_dialog = host_open_file},
                     .fixed_now_unix_ms = 1793534400000LL + 30 * 60 * 1000};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  CT_CHECK(ccord_session_start(F.session, &err) == CCORD_OK);
  return true;
}

ccord_id append_rich_message(ccord_id ch, const char *content, const ccord_message_rich *rich)
{
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  size_t n = ccord_store_message_count(F.store, ch);
  const ccord_message *last = n ? ccord_store_message_at(F.store, ch, n - 1) : nullptr;
  ccord_message m = {.id = last ? last->id + 1 : 1, .channel_id = ch, .author_id = ccord_store_self_id(F.store),
                     .timestamp_ms = last ? last->timestamp_ms + 1 : (int64_t)F.now, .content = content, .rich = rich};
  ccord_error err;
  CT_CHECK(ccord_store_append_message(F.store, &m, &err) == CCORD_OK);
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100.0f,
                           .y = l->message_area.y + 100.0f});
  frame(3);
  return m.id;
}

void remove_message(ccord_id ch, ccord_id id)
{
  ccord_error err;
  CT_CHECK(ccord_store_delete_message(F.store, ch, id, &err) == CCORD_OK);
  frame(2);
}

bool surf_pixel_near(SDL_Surface *s, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
  uint8_t pr = 0, pg = 0, pb = 0, pa = 0;
  return SDL_ReadSurfacePixel(s, x, y, &pr, &pg, &pb, &pa) &&
         abs((int)pr - (int)r) <= 3 && abs((int)pg - (int)g) <= 3 && abs((int)pb - (int)b) <= 3;
}

void save_optional_screenshot(const char *filename)
{
  const char *dir = getenv("CCORD_EXPR_SCREENSHOT_DIR");
  if (!dir || !dir[0]) return;
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dir, filename);
  ccord_error err = {0};
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  (void)ccord_render_screenshot(F.renderer, path, &err);
  ccord_render_end(F.renderer);
}

bool menu_has(const char *label)
{
  for (size_t i = 0; i < F.ui->menu.count; i++) {
    if (F.ui->menu.items[i].label && strcmp(F.ui->menu.items[i].label, label) == 0) return true;
  }
  return false;
}

void open_menu(ccord_menu_target target, ccord_id guild, ccord_id channel, ccord_id user, ccord_id message)
{
  ui_menu_close(F.ui);
  ccord_menu_context ctx = {.target = target, .guild_id = guild, .channel_id = channel, .message_id = message, .user_id = user};
  ui_menu_open(F.ui, 300.0f, 300.0f, &ctx);
}

ccord_id other_member(ccord_id guild, size_t skip)
{
  ccord_id self = ccord_store_self_id(F.store);
  for (size_t i = 0; i < ccord_store_member_count(F.store, guild); i++) {
    const ccord_member *m = ccord_store_member_at(F.store, guild, i);
    if (m && m->user_id != self && skip-- == 0) return m->user_id;
  }
  return CCORD_ID_NONE;
}

ccord_ui_server_settings_debug ssd(void)
{
  ccord_ui_server_settings_debug d;
  ccord_ui_get_server_settings_debug(F.ui, &d);
  return d;
}

bool ss_has(const char *name)
{
  return ccord_ui_server_settings_find(F.ui, name, nullptr);
}

bool ss_click(const char *name)
{
  ccord_rect r;
  if (!ccord_ui_server_settings_find(F.ui, name, &r)) return false;
  click(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  return true;
}

bool ss_scroll_to(const char *name)
{
  const float x = F.ui->win_w - 220.0f, y = F.ui->win_h * 0.5f;   // inside the editor, whatever is scrolled off
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = x, .y = y, .wheel = 1000.0f});   // back to the top
  frame(1);
  for (int i = 0; i < 120 && !ss_has(name); i++) {
    ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = x, .y = y, .wheel = -3.0f});
    frame(1);
  }
  return ss_has(name);
}

void frame_deaf(uint64_t jump_ms)
{
  F.now += jump_ms;
  (void)ccord_ui_update(F.ui, F.now);
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_render_end(F.renderer);
}

size_t member_header(const char *prefix)
{
  for (size_t i = 0; i < F.ui->nmrow; i++) {
    if (F.ui->mrow[i].is_header && strncmp(F.ui->mrow[i].label, prefix, strlen(prefix)) == 0) return i;
  }
  return SIZE_MAX;
}

size_t member_header_count(const char *prefix)
{
  size_t i = member_header(prefix);
  if (i == SIZE_MAX) return 0;
  const char *dash = strstr(F.ui->mrow[i].label, "\xE2\x80\x94");   // "ARTISTS — 3"
  return dash ? (size_t)strtoul(dash + 4, nullptr, 10) : 0;
}

int menu_index(const char *prefix)
{
  for (size_t i = 0; i < F.ui->menu.count; i++) {
    if (strncmp(F.ui->menu.items[i].label, prefix, strlen(prefix)) == 0) return (int)i;
  }
  return -1;
}

void menu_point_at(int idx)
{
  ccord_rect r = ui_menu_item_rect(F.ui, (size_t)idx);
  mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  frame(1);
}

void menu_click_index(int idx)
{
  ccord_rect r = ui_menu_item_rect(F.ui, (size_t)idx);
  mouse(CCORD_IN_MOUSE_MOVE, 0, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  frame(1);
  mouse(CCORD_IN_MOUSE_DOWN, 1, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
  frame(1);
}

void clear_counts(ccord_id guild)
{
  ccord_error err;
  size_t n = ccord_store_channel_count(F.store, guild);
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, guild, i);
    if (c) (void)ccord_store_set_channel_counts(F.store, c->id, 0, 0, &err);
  }
}

bool autocomplete_has(const char *substr)
{
  if (!F.ui || !F.ui->autocomplete_open) return false;
  for (size_t i = 0; i < F.ui->nautocomplete; i++) {
    if (strstr(F.ui->autocomplete_items[i].display, substr) != nullptr) return true;
  }
  return false;
}

void clear_composer_input(void)
{
  if (F.ui && F.ui->autocomplete_open) key(CCORD_KEY_ESCAPE, 0);
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
}

void click_rect(ccord_rect r)
{
  click(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}

bool media_url_requested(const char *url)
{
  return ui_media_requested(F.ui, url);
}

void ui_test_teardown(void)
{
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_contrib_destroy(F.contrib);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
  remove(ui_test_file("settings.ini"));
  remove(ui_test_file("drafts.json"));
}

static ccord_status ltt_connect(ccord_transport *t, ccord_error *err)
{
  (void)t;
  ccord_error_clear(err);
  return CCORD_OK;
}
static void ltt_disconnect(ccord_transport *t) { (void)t; }
static ccord_status ltt_req_hist(ccord_transport *t, ccord_id c, uint32_t l, ccord_error *err)
{
  (void)t, (void)c, (void)l;
  ccord_error_clear(err);
  return CCORD_OK;
}
static ccord_status ltt_send_msg(ccord_transport *t, ccord_id c, const char *s, size_t n, ccord_error *err)
{
  (void)t, (void)c, (void)s, (void)n;
  ccord_error_clear(err);
  return CCORD_OK;
}
static bool ltt_poll(ccord_transport *t, ccord_net_event *out)
{
  login_test_transport *lt = (login_test_transport *)t;
  *out = (ccord_net_event){0};
  if (lt->emit_state) {
    lt->emit_state = false;
    out->type = CCORD_NET_STATE;
    out->state = lt->next_state;
    return true;
  }
  if (lt->emit_snap) {
    lt->emit_snap = false;
    out->type = CCORD_NET_SNAPSHOT;
    out->snapshot = &lt->snap;
    return true;
  }
  return false;
}
static void ltt_destroy(ccord_transport *t) { free(t); }

const ccord_transport_vtable k_ltt_vt = {
  .name = "login_test",
  .connect = ltt_connect,
  .disconnect = ltt_disconnect,
  .request_history = ltt_req_hist,
  .send_message = ltt_send_msg,
  .poll = ltt_poll,
  .destroy = ltt_destroy,
};

int ui_test_harness_failures(void)
{
  return ct_failures;
}
