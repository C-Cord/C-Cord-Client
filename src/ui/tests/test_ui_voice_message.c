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

// Voice messages, composer side: the recorder drives the real Ogg/Opus encoder (no fake) past the 10 minute cap and
// must send exactly one message of at most 600 s. The echoed message must carry the voice flag's waveform and duration
// and draw as the waveform player. Real store + mock transport + session + renderer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/paths.h>
#include <ccord/model/rich.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
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

static void click_rect(ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  frame(2);
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_vm_data"));
remove("ui_vm_data/settings.ini");
  remove("ui_vm_data/drafts.json");
  remove("ui_vm_data");
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
  ccord_platform_config pc = {.title = "ui-vm-test", .width = 1280, .height = 800, .headless = true};
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

// Opens #general of the first server, the channel every voice message in these tests goes to.
static void open_general(void)
{
  ccord_error err = {0};
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  size_t n = ccord_store_channel_count(F.store, g);
  ccord_id general = CCORD_ID_NONE;
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, g, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && strcmp(c->name, "general") == 0) general = c->id;
  }
  CT_CHECK(general != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, general, &err) == CCORD_OK);
  frame(3);
}

// Counts the voice messages in the selected channel and returns the duration of the last one.
static size_t count_voice_messages(uint32_t *last_ms)
{
  ccord_id ch = ccord_store_selected_channel(F.store);
  size_t voice = 0;
  size_t cnt = ccord_store_message_count(F.store, ch);
  for (size_t i = 0; i < cnt; i++) {
    const ccord_message *m = ccord_store_message_at(F.store, ch, i);
    if (!m || !m->rich) continue;
    for (size_t k = 0; k < m->rich->attachment_count; k++) {
      const ccord_attachment *a = &m->rich->attachments[k];
      if (a->waveform_b64 && a->waveform_b64[0] != '\0' && a->duration_ms > 0) {
        voice++;
        if (last_ms) *last_ms = a->duration_ms;
      }
    }
  }
  return voice;
}

// 100 ms of PCM per call: a non-constant pattern so the encoder has real content to code.
static void fill_pcm(int16_t *pcm, size_t frames)
{
  for (size_t i = 0; i < frames; i++) pcm[i] = (int16_t)((int)(i % 96u) * 300 - 14000);
}

// The cap: 660 s of PCM is offered. The real encoder takes exactly 600 s, the recorder sends it once and stops, and
// the rest is not taken. A cap above 600 s makes the encoder refuse a chunk, so `taken` falls short and this fails.
static void test_cap_sends_one_message(void)
{
  enum { CHUNK = 4800 };
  static int16_t pcm[CHUNK];
  fill_pcm(pcm, CHUNK);
  CT_CHECK(ui_vm_test_begin(F.ui));
  uint32_t sent_before = F.ui->vm.sent_count;
  size_t taken = 0;
  for (int chunk = 0; chunk < 6600; chunk++) {
    size_t n = ui_vm_feed_test_pcm(F.ui, pcm, CHUNK);
    if (n == 0) break;
    taken += n;
  }
  CT_CHECK(taken == (size_t)UI_VM_MAX_FRAMES);
  CT_CHECK(!F.ui->vm.recording);
  CT_CHECK(!ui_vm_is_active(F.ui));
  CT_CHECK(F.ui->vm.sent_count == sent_before + 1);
  CT_CHECK(F.ui->vm.last_sent_ms > 0 && F.ui->vm.last_sent_ms <= 600000u);
  frame(3);
  uint32_t ms = 0;
  CT_CHECK(count_voice_messages(&ms) == 1);
  CT_CHECK(ms > 0 && ms <= 600000u);
  frame(2);
  CT_CHECK(F.ui->vm.dbg_cards > 0);   // the echoed message draws as the waveform player
}

// A short take sent from the bar: the duration is exact (2 s of PCM is 2000 ms) and Send clears the bar.
static void test_short_take_send_from_bar(void)
{
  enum { CHUNK = 4800 };
  static int16_t pcm[CHUNK];
  fill_pcm(pcm, CHUNK);
  CT_CHECK(ui_vm_test_begin(F.ui));
  for (int i = 0; i < 20; i++) CT_CHECK(ui_vm_feed_test_pcm(F.ui, pcm, CHUNK) == CHUNK);
  frame(2);
  // While recording, UI redraws are paced (~66 ms / 15 Hz) rather than VSync-spinning every frame.
  uint32_t rec_wait = ccord_ui_next_wait_ms(F.ui, F.now);
  CT_CHECK(rec_wait > 0u && rec_wait <= 66u);
  CT_CHECK(F.ui->vm.bar_rect.w > 0.0f && F.ui->vm.send_rect.w > 0.0f);
  uint32_t sent_before = F.ui->vm.sent_count;
  click_rect(F.ui->vm.send_rect);
  CT_CHECK(F.ui->vm.sent_count == sent_before + 1);
  CT_CHECK(F.ui->vm.last_sent_ms == 2000u);
  CT_CHECK(!ui_vm_is_active(F.ui));
}

// Cancel drops the take: nothing is sent and the recorder is released.
static void test_cancel_sends_nothing(void)
{
  enum { CHUNK = 4800 };
  static int16_t pcm[CHUNK];
  fill_pcm(pcm, CHUNK);
  CT_CHECK(ui_vm_test_begin(F.ui));
  CT_CHECK(ui_vm_feed_test_pcm(F.ui, pcm, CHUNK) == CHUNK);
  frame(2);
  uint32_t sent_before = F.ui->vm.sent_count;
  CT_CHECK(F.ui->vm.cancel_rect.w > 0.0f);
  click_rect(F.ui->vm.cancel_rect);
  CT_CHECK(!ui_vm_is_active(F.ui));
  CT_CHECK(F.ui->vm.sent_count == sent_before);
  CT_CHECK(F.ui->vm.peaks == nullptr && F.ui->vm.enc == nullptr);
}

// Discord's waveform is standard base64 with one byte per bar; a capped decode keeps the first bars.
static void test_waveform_decode(void)
{
  uint8_t out[8] = {0};
  CT_CHECK(ui_vm_waveform_bars("AQIDBAUG", out, sizeof out) == 6);
  CT_CHECK(out[0] == 1 && out[2] == 3 && out[5] == 6);
  CT_CHECK(ui_vm_waveform_bars("AQ!D", out, sizeof out) == 0);
  CT_CHECK(ui_vm_waveform_bars("AQIDBAUG", out, 3) == 3);
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_voice_message")) return 77;
  open_general();
  test_waveform_decode();
  test_cap_sends_one_message();
  test_short_take_send_from_bar();
  test_cancel_sends_nothing();
  teardown();
  return CT_RESULT("test_ui_voice_message");
}
