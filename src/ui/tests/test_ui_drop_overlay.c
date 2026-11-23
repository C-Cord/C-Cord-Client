// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
static const char *ccord_test_init_dir(const char *prefix) {
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

#include <SDL3/SDL.h>

#include <ccord/core/paths.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"
#include "../../session/session_internal.h"
#include "../../store/store_internal.h"
#include "ui_style.h"

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
} fixture;

static fixture F;

static uint64_t fake_clock(void *ud) { (void)ud; return F.now; }
static void host_clip_set(void *ud, const char *s) { (void)ud; (void)s; }
static void host_text_input(void *ud, bool a, float x, float y, float w, float h)
{ (void)ud; (void)a; (void)x; (void)y; (void)w; (void)h; }
static bool host_open_url(void *ud, const char *url) { (void)ud; (void)url; return true; }
static bool host_open_file(void *ud, char *out, size_t cap) { (void)ud; (void)out; (void)cap; return false; }

static void frame_step(int n, uint64_t ms)
{
  for (int i = 0; i < n; i++) {
    F.now += ms;
    (void)ccord_session_pump(F.session);
    (void)ccord_bus_drain(F.bus);
    (void)ccord_ui_update(F.ui, F.now);
    ccord_render_begin(F.renderer, F.theme.c.bg_primary);
    ccord_ui_draw(F.ui, F.renderer);
    ccord_render_end(F.renderer);
  }
}

static bool make_sparse_file(const char *path, uint64_t size)
{
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) return false;
  bool ok = true;
  if (size > 0) {
    static const char zero = 0;
    off_t last = (off_t)(size - 1u);
    ok = lseek(fd, last, SEEK_SET) == last && write(fd, &zero, 1) == 1;
  }
  ok = close(fd) == 0 && ok;
  return ok;
}

static bool setup(const char *argv0)
{
  ccord_error err;
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
  ccord_paths_set_data_dir(ccord_test_init_dir("drop_ovl_data"));
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK) return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 30, .latency_ms = 0, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.transport || !F.session) return false;
  if (ccord_session_start(F.session, &err) != CCORD_OK) return false;
  ccord_platform_config pc = {.title = "drop-ovl-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) return false;
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_ui_deps d = {
    .bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
    .fixed_now_unix_ms = 1793534400000LL,
    .host = {.clipboard_set = host_clip_set, .text_input = host_text_input,
             .open_url = host_open_url, .open_file_dialog = host_open_file},
  };
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  frame_step(5, 50);
  return true;
}

static void teardown(void)
{
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_contrib_destroy(F.contrib);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
}

static void set_premium_type(uint8_t pt)
{
  F.session->account_private.valid = true;
  F.session->account_private.premium_type = pt;
}

static void set_selected_guild_tier(uint8_t tier)
{
  ccord_id gid = ccord_store_selected_guild(F.store);
  for (size_t i = 0; i < F.store->d.nguilds; i++) {
    if (F.store->d.guilds[i].id == gid) {
      F.store->d.guilds[i].premium_tier = tier;
      break;
    }
  }
}

static void test_drag_lifecycle_and_lazy_alloc(void)
{
  ui_drop_overlay_debug d;
  ui_drop_overlay_debug_get(F.ui, &d);
  // Zero heap allocation before the first drag or error.
  CT_CHECK(!d.alloc_live && !d.hover_open && !d.error_open);

  set_premium_type(0);
  set_selected_guild_tier(0);

  // BEGIN opens the overlay and draws through shared style tokens (uis_modal_frame + uis_plate).
  ccord_input_event ev = {.type = CCORD_EV_DROP_BEGIN};
  CT_CHECK(ccord_ui_handle_input(F.ui, &ev));
  uis_audit_reset();
  frame_step(2, 100);
  ui_drop_overlay_debug_get(F.ui, &d);
  CT_CHECK(d.alloc_live && d.hover_open && d.mode == UI_DROP_MODE_UPLOAD);
  CT_CHECK(d.card_rect.w > 200.0f && d.card_rect.h > 150.0f);
  CT_CHECK(strstr(d.title, "Upload to #") != nullptr);
  CT_CHECK(strstr(d.limit_pill, "20 MB") != nullptr && strstr(d.limit_pill, "Free") != nullptr);
  CT_CHECK(uis_audit_get()->kind[UIS_K_MODAL].count >= 1u);
  CT_CHECK(uis_audit_get()->kind[UIS_K_PLATE].count >= 1u);

  // POSITION outside vs inside card tracks pointer_over_card; Shift switches to UI_DROP_MODE_SPOILER.
  ev = (ccord_input_event){.type = CCORD_EV_DROP_POSITION, .x = 8.0f, .y = 8.0f, .mods = 0};
  CT_CHECK(ccord_ui_handle_input(F.ui, &ev));
  frame_step(1, 50);
  ui_drop_overlay_debug_get(F.ui, &d);
  CT_CHECK(!d.pointer_over_card && d.mode == UI_DROP_MODE_UPLOAD);

  ev = (ccord_input_event){.type = CCORD_EV_DROP_POSITION,
                           .x = d.card_rect.x + d.card_rect.w * 0.5f,
                           .y = d.card_rect.y + d.card_rect.h * 0.5f,
                           .mods = CCORD_MOD_SHIFT};
  CT_CHECK(ccord_ui_handle_input(F.ui, &ev));
  frame_step(1, 50);
  ui_drop_overlay_debug_get(F.ui, &d);
  CT_CHECK(d.pointer_over_card && d.shift_held && d.mode == UI_DROP_MODE_SPOILER);
  CT_CHECK(strstr(d.title, "Upload Spoiler to #") != nullptr);

  // COMPLETE closes and auto-frees the lazy heap block after the 140 ms close fade settles.
  ev = (ccord_input_event){.type = CCORD_EV_DROP_COMPLETE};
  CT_CHECK(ccord_ui_handle_input(F.ui, &ev));
  frame_step(5, 50);
  ui_drop_overlay_debug_get(F.ui, &d);
  CT_CHECK(!d.hover_open && !d.alloc_live);

  // Defensive clear: a missed DROP_COMPLETE clears on normal MOUSE_MOVE, Escape, or APP_BACKGROUND.
  ev = (ccord_input_event){.type = CCORD_EV_DROP_BEGIN};
  CT_CHECK(ccord_ui_handle_input(F.ui, &ev));
  frame_step(1, 16);
  CT_CHECK(ui_drop_overlay_active(F.ui));
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = 100.0f, .y = 100.0f};
  (void)ccord_ui_handle_input(F.ui, &ev);
  CT_CHECK(!ui_drop_overlay_active(F.ui));

  ev = (ccord_input_event){.type = CCORD_EV_DROP_BEGIN};
  CT_CHECK(ccord_ui_handle_input(F.ui, &ev));
  ev = (ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_ESCAPE};
  CT_CHECK(ccord_ui_handle_input(F.ui, &ev));
  CT_CHECK(!ui_drop_overlay_active(F.ui));
  frame_step(5, 50);
  ui_drop_overlay_debug_get(F.ui, &d);
  CT_CHECK(!d.alloc_live);
}

static void test_dynamic_discord_limits_and_error_modal(void)
{
  const char *dir = ccord_test_init_dir("drop_ovl_data");
  char f25[512], f60[512], f120[512];
  snprintf(f25, sizeof f25, "%s/clip_25mb.mp4", dir);
  snprintf(f60, sizeof f60, "%s/clip_60mb.mp4", dir);
  snprintf(f120, sizeof f120, "%s/clip_120mb.mp4", dir);
  CT_CHECK(make_sparse_file(f25, 25ULL * 1024ULL * 1024ULL));
  CT_CHECK(make_sparse_file(f60, 60ULL * 1024ULL * 1024ULL));
  CT_CHECK(make_sparse_file(f120, 120ULL * 1024ULL * 1024ULL));

  ui_drop_overlay_debug d;

  // 1. Free + unboosted server (20 MB limit): 25 MB file rejected with zero allocation + error modal.
  set_premium_type(0);
  set_selected_guild_tier(0);
  CT_CHECK(ui_current_upload_limit(F.ui, nullptr) == CCORD_UPLOAD_LIMIT_FREE);
  CT_CHECK(!ccord_ui_attach_file(F.ui, f25));
  CT_CHECK(ccord_ui_staged_attachment_count(F.ui) == 0);
  CT_CHECK(strstr(F.ui->notice, "20 MB") != nullptr);
  CT_CHECK(ui_upload_error_is_open(F.ui));
  frame_step(3, 80);
  ui_drop_overlay_debug_get(F.ui, &d);
  CT_CHECK(d.error_open && d.error_box.w > 200.0f && d.error_ok_btn.w > 40.0f);
  CT_CHECK(strstr(d.error_body, "clip_25mb.mp4") != nullptr && strstr(d.error_body, "20 MB") != nullptr);

  // Cursor over OK button is POINTER, click OK dismisses cleanly.
  ccord_input_event mv = {.type = CCORD_IN_MOUSE_MOVE,
                          .x = d.error_ok_btn.x + d.error_ok_btn.w * 0.5f,
                          .y = d.error_ok_btn.y + d.error_ok_btn.h * 0.5f};
  CT_CHECK(ccord_ui_handle_input(F.ui, &mv));
  frame_step(1, 16);
  CT_CHECK(F.ui->cursor == CCORD_CURSOR_POINTER);
  ccord_input_event md = {.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = mv.x, .y = mv.y};
  ccord_input_event mu = {.type = CCORD_IN_MOUSE_UP, .button = 1, .x = mv.x, .y = mv.y};
  CT_CHECK(ccord_ui_handle_input(F.ui, &md));
  CT_CHECK(ccord_ui_handle_input(F.ui, &mu));
  CT_CHECK(!ui_upload_error_is_open(F.ui));
  frame_step(5, 50);
  ui_drop_overlay_debug_get(F.ui, &d);
  CT_CHECK(!d.alloc_live);

  // 2. Free + Boost Lv. 2 server (50 MB limit): overlay advertises 50 MB (Server Boost Lv. 2); 60 MB file rejected.
  set_selected_guild_tier(2);
  ccord_upload_limit_source src = CCORD_UPLOAD_SRC_FREE;
  CT_CHECK(ui_current_upload_limit(F.ui, &src) == CCORD_UPLOAD_LIMIT_TIER2 && src == CCORD_UPLOAD_SRC_GUILD_TIER2);
  ccord_input_event db = {.type = CCORD_EV_DROP_BEGIN};
  (void)ccord_ui_handle_input(F.ui, &db);
  frame_step(2, 60);
  ui_drop_overlay_debug_get(F.ui, &d);
  CT_CHECK(strstr(d.limit_pill, "50 MB") != nullptr && strstr(d.limit_pill, "Server Boost Lv. 2") != nullptr);
  CT_CHECK(!ccord_ui_attach_file(F.ui, f60));
  CT_CHECK(ui_upload_error_is_open(F.ui) && strstr(F.ui->notice, "50 MB") != nullptr);
  // Dismiss via Enter key.
  ccord_input_event ret = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_RETURN};
  CT_CHECK(ccord_ui_handle_input(F.ui, &ret));
  CT_CHECK(!ui_upload_error_is_open(F.ui));

  // 3. Free + Boost Lv. 3 server (100 MB limit): 120 MB file rejected with 100 MB; Nitro (1 GB) upgrades to 1 GB.
  set_selected_guild_tier(3);
  CT_CHECK(ui_current_upload_limit(F.ui, &src) == CCORD_UPLOAD_LIMIT_TIER3 && src == CCORD_UPLOAD_SRC_GUILD_TIER3);
  CT_CHECK(!ccord_ui_attach_file(F.ui, f120));
  CT_CHECK(ui_upload_error_is_open(F.ui) && strstr(F.ui->notice, "100 MB") != nullptr);
  CT_CHECK(ccord_ui_handle_back(F.ui));
  CT_CHECK(!ui_upload_error_is_open(F.ui));

  set_premium_type(2);
  CT_CHECK(ui_current_upload_limit(F.ui, &src) == CCORD_UPLOAD_LIMIT_NITRO && src == CCORD_UPLOAD_SRC_NITRO);
  (void)ccord_ui_handle_input(F.ui, &db);
  frame_step(2, 60);
  ui_drop_overlay_debug_get(F.ui, &d);
  CT_CHECK(strstr(d.limit_pill, "1 GB") != nullptr && strstr(d.limit_pill, "Nitro") != nullptr);
  ccord_input_event dc = {.type = CCORD_EV_DROP_COMPLETE};
  (void)ccord_ui_handle_input(F.ui, &dc);

  // 4. Context modes: Settings > Plugins (.cce) and 10/10 attachment cap.
  F.ui->settings_open = true;
  F.ui->settings_page = UI_SETTINGS_PAGE_PLUGINS;
  F.ui->extensions_on = true;
  (void)ccord_ui_handle_input(F.ui, &db);
  frame_step(2, 60);
  ui_drop_overlay_debug_get(F.ui, &d);
  CT_CHECK(d.mode == UI_DROP_MODE_PLUGINS && strstr(d.title, "Plugin Package") != nullptr);
  F.ui->extensions_on = false;
  frame_step(1, 30);
  ui_drop_overlay_debug_get(F.ui, &d);
  CT_CHECK(d.mode == UI_DROP_MODE_LOCKED);
  F.ui->settings_open = false;

  static const uint8_t k_tiny[4] = {'a', 'b', 'c', 'd'};
  for (size_t i = 0; i < CCORD_RICH_MAX_ATTACHMENTS; i++) {
    char nm[32];
    snprintf(nm, sizeof nm, "f%zu.txt", i);
    CT_CHECK(ccord_ui_attach_bytes(F.ui, nm, k_tiny, sizeof k_tiny));
  }
  frame_step(1, 30);
  ui_drop_overlay_debug_get(F.ui, &d);
  CT_CHECK(d.mode == UI_DROP_MODE_LIMIT_FULL);
  ccord_ui_clear_attachment(F.ui);
  (void)ccord_ui_handle_input(F.ui, &dc);
  frame_step(5, 50);
  ui_drop_overlay_debug_get(F.ui, &d);
  CT_CHECK(!d.alloc_live);

  // 5. Session send guard rejects oversized file if channel limit dropped after staging.
  set_premium_type(0);
  set_selected_guild_tier(0);
  ccord_id ch = ccord_store_selected_channel(F.store);
  ccord_upload_file huge = {.filename = "too_big.bin", .data = k_tiny, .size = (size_t)(CCORD_UPLOAD_LIMIT_FREE + 1u)};
  ccord_error serr;
  CT_CHECK(ccord_session_send_message_with_files(F.session, ch, CCORD_ID_NONE, true, "", 0, &huge, 1, &serr) == CCORD_ERR_RANGE);
  CT_CHECK(strstr(serr.message, "20 MB") != nullptr);

  remove(f25);
  remove(f60);
  remove(f120);
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_drop_overlay")) {
    fprintf(stderr, "test_ui_drop_overlay: setup failed or no display driver; skipping\n");
    return 77;
  }
  test_drag_lifecycle_and_lazy_alloc();
  test_dynamic_discord_limits_and_error_modal();
  teardown();
  return CT_RESULT("test_ui_drop_overlay");
}
