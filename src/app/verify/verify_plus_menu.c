// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice plus-menu: the composer '+' opens a three-row menu (Upload a File, Create Poll, Schedule Message).
// Drives the real UI over the mock transport: clicks the drawn '+' rect and the drawn rows. Upload a File uses the
// multi-select host callback (open_file_dialog_async); a fake picker fills the platform's drop-file queue the way the
// real SDL callback does (ccord_platform_inject_drop_file), and the drop events are delivered the way run_interactive
// does (pop one path, ccord_ui_attach_file). A pick of several files must stage each one, in pick order, once.
#include "verify_ui_parity.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ccord/core/error.h>
#include <ccord/platform/platform.h>

#include "ui_internal.h"

#define PM_MAX_PICK 4

typedef struct pm {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
} pm;

static int g_picker_calls;
static ccord_platform *g_platform;
static const char *g_pick[PM_MAX_PICK];   // paths the next pick hands back, in pick order
static size_t g_pick_n;

// Stands in for the native multi-select picker. The real one (ccord_platform_open_file_dialog) returns at once and
// its SDL callback injects each chosen path into the drop queue, which is what this does.
static void pm_fake_picker(void *ud)
{
  (void)ud;
  g_picker_calls++;
  ccord_error err;
  for (size_t i = 0; i < g_pick_n; i++) (void)ccord_platform_inject_drop_file(g_platform, g_pick[i], &err);
}

// Delivers the queued drop-file events exactly like run_interactive: each event pops one path and attaches it.
static void pm_deliver_drops(pm *s)
{
  ccord_input_event ev;
  for (int n = 0; n < 64 && ccord_platform_next_event(s->c->platform, &ev, 0); n++) {
    if (ev.type != CCORD_EV_DROP_FILE) continue;
    char dpath[1024] = {0};
    if (ccord_platform_pop_drop_file(s->c->platform, dpath, sizeof dpath) && dpath[0]) {
      (void)ccord_ui_attach_file(s->c->ui, dpath);
    } else if (ev.text[0]) {
      (void)ccord_ui_attach_file(s->c->ui, ev.text);
    }
  }
}

static bool pm_check(pm *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

static void pm_frames(pm *s, int n) { s->c->frames(s->c->ud, n); }
static void pm_send(pm *s, ccord_input_event ev) { s->c->input(s->c->ud, &ev); }

static void pm_click(pm *s, float x, float y)
{
  pm_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  pm_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  pm_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
  pm_frames(s, 3);
}

static void pm_click_rect(pm *s, ccord_rect r) { pm_click(s, r.x + r.w * 0.5f, r.y + r.h * 0.5f); }
static void pm_key(pm *s, ccord_key k)
{
  pm_send(s, (ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = k});
  pm_frames(s, 2);
}

static void pm_open(pm *s) { pm_click_rect(s, s->c->ui->composer_attach_rect); }

static void pm_reset(pm *s)
{
  ccord_ui *ui = s->c->ui;
  if (ui->plus_menu_open) ui_plus_menu_close(ui);
  if (ui->poll_open) ui_poll_composer_close(ui);
  if (ui->schedule_open) ui_schedule_dialog_close(ui);
  pm_frames(s, 2);
}

static bool pm_make_file(const char *path, off_t size, const char *text)
{
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) return false;
  bool ok = true;
  if (size > 0) {
    // Sparse: one byte at the last offset makes the file `size` long without writing 25 MB; the attach cap reads the size.
    static const char zero = 0;
    ok = lseek(fd, size - 1, SEEK_SET) == size - 1 && write(fd, &zero, 1) == 1;
  } else if (text) {
    ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
  }
  ok = close(fd) == 0 && ok;
  return ok;
}

static void pm_run(pm *s)
{
  ccord_ui *ui = s->c->ui;
  const char *tmp = getenv("TMPDIR");
  const char *dir = tmp && tmp[0] ? tmp : "/tmp";
  char names[3][160], big[160];
  for (int i = 0; i < 3; i++) {
    snprintf(names[i], sizeof names[i], "ccord_plus_menu_%d_%d.txt", (int)getpid(), i + 1);
  }
  snprintf(big, sizeof big, "%s/ccord_plus_menu_%d_big.bin", dir, (int)getpid());
  char paths[3][512];
  for (int i = 0; i < 3; i++) snprintf(paths[i], sizeof paths[i], "%s/%s", dir, names[i]);
  bool made = true;
  for (int i = 0; i < 3; i++) made = made && pm_make_file(paths[i], 0, "plus menu upload");
  if (!pm_check(s, made, "temp upload files created")) {
    for (int i = 0; i < 3; i++) remove(paths[i]);
    return;
  }
  g_platform = s->c->platform;
  if (!pm_check(s, g_platform != nullptr, "the platform is available to the slice")) {
    for (int i = 0; i < 3; i++) remove(paths[i]);
    remove(big);
    return;
  }

  void (*saved_async)(void *) = ui->d.host.open_file_dialog_async;
  ui->d.host.open_file_dialog_async = pm_fake_picker;
  g_picker_calls = 0;

  pm_frames(s, 10);
  ccord_id g0 = ccord_store_selected_guild(s->c->store);
  ccord_error err;
  for (size_t i = ccord_store_channel_count(s->c->store, g0); i > 0; i--) {
    const ccord_channel *c = ccord_store_channel_at(s->c->store, g0, i - 1);
    if (!c || c->kind != CCORD_CHANNEL_TEXT || c->discord_type != 0) continue;
    if (ccord_session_select_channel(s->c->session, c->id, &err) == CCORD_OK) break;
  }
  pm_frames(s, 20);

  do {
    if (!pm_check(s, ui->composer_attach_rect.w > 0.0f, "composer + button is drawn")) break;
    if (!pm_check(s, !ui->plus_menu_open, "menu starts closed")) break;

    // Click '+': menu opens, exactly three rows, no file dialog.
    pm_open(s);
    if (!pm_check(s, ui->plus_menu_open && ui_plus_menu_is_open(ui), "click on + opens the menu")) break;
    if (!pm_check(s, g_picker_calls == 0, "click on + does not open the file picker")) break;
    ccord_rect r0 = ui_plus_menu_row_rect(ui, 0), r1 = ui_plus_menu_row_rect(ui, 1), r2 = ui_plus_menu_row_rect(ui, 2);
    if (!pm_check(s, r0.w > 0 && r0.y < r1.y && r1.y < r2.y && r2.y < ui->composer_attach_rect.y,
                  "three rows in order, anchored above the + button"))
      break;

    // + again closes.
    pm_open(s);
    if (!pm_check(s, !ui->plus_menu_open && g_picker_calls == 0, "clicking + again closes the menu")) break;

    // Escape closes with no action.
    pm_open(s);
    pm_key(s, CCORD_KEY_ESCAPE);
    if (!pm_check(s, !ui->plus_menu_open && g_picker_calls == 0 && !ui->poll_open && !ui->schedule_open,
                  "Escape closes the menu without action"))
      break;

    // Click outside closes with no action.
    pm_open(s);
    pm_click(s, 4.0f, 4.0f);
    if (!pm_check(s, !ui->plus_menu_open && g_picker_calls == 0 && !ui->poll_open && !ui->schedule_open,
                  "click outside closes the menu without action"))
      break;

    // Keyboard: Down, Down, Enter reaches Create Poll.
    pm_open(s);
    pm_key(s, CCORD_KEY_DOWN);
    pm_key(s, CCORD_KEY_DOWN);
    pm_key(s, CCORD_KEY_RETURN);
    if (!pm_check(s, !ui->plus_menu_open && ui->poll_open, "Down, Down, Enter opens the poll composer")) break;
    pm_reset(s);

    // Create Poll by click.
    pm_open(s);
    pm_click_rect(s, ui_plus_menu_row_rect(ui, 1));
    if (!pm_check(s, ui->poll_open && !ui->plus_menu_open && !ui->schedule_open, "Create Poll opens the poll overlay, menu closed")) break;
    pm_reset(s);

    // Schedule Message.
    pm_open(s);
    pm_click_rect(s, ui_plus_menu_row_rect(ui, 2));
    if (!pm_check(s, ui->schedule_open && !ui->plus_menu_open && !ui->poll_open,
                  "Schedule Message opens the schedule overlay, menu closed"))
      break;
    pm_reset(s);

    // Upload a File: the picker starts once, the menu closes, and nothing is staged until the picks arrive.
    ccord_ui_clear_attachment(ui);
    ui->notice[0] = '\0';
    g_pick_n = 0;
    pm_open(s);
    pm_click_rect(s, ui_plus_menu_row_rect(ui, 0));
    pm_deliver_drops(s);
    if (!pm_check(s, g_picker_calls == 1 && !ui->plus_menu_open, "Upload a File starts the picker once, menu closed")) break;
    if (!pm_check(s, ccord_ui_staged_attachment_count(ui) == 0 && ui->notice[0] == '\0',
                  "a cancelled picker attaches nothing and shows no error"))
      break;

    // One pick of three files: three attachments, in pick order, none duplicated.
    for (size_t i = 0; i < 3; i++) g_pick[i] = paths[i];
    g_pick_n = 3;
    pm_open(s);
    pm_click_rect(s, ui_plus_menu_row_rect(ui, 0));
    pm_deliver_drops(s);
    if (!pm_check(s, ccord_ui_staged_attachment_count(ui) == 3, "one pick of 3 files stages 3 attachments")) break;
    bool order = true;
    for (size_t i = 0; i < 3; i++) order = order && strcmp(ui->staged_files[i].filename, names[i]) == 0;
    if (!pm_check(s, order, "the picked files are staged in pick order, none duplicated")) break;

    // A pick with one file over the active channel upload cap: that file is refused with the size notice + modal,
    // while the other valid files still attach in pick order.
    uint64_t active_cap = ui_current_upload_limit(ui, nullptr);
    char lim_s[32];
    ccord_format_upload_limit(active_cap, lim_s, sizeof lim_s);
    if (!pm_check(s, pm_make_file(big, (off_t)active_cap + 1, nullptr), "oversized sparse temp file created")) break;
    ccord_ui_clear_attachment(ui);
    ui->notice[0] = '\0';
    g_pick[0] = paths[0];
    g_pick[1] = big;
    g_pick[2] = paths[1];
    g_pick_n = 3;
    pm_open(s);
    pm_click_rect(s, ui_plus_menu_row_rect(ui, 0));
    pm_deliver_drops(s);
    if (!pm_check(s, ccord_ui_staged_attachment_count(ui) == 2, "the other files of a pick with an oversized one still attach")) break;
    if (!pm_check(s, strstr(ui->notice, lim_s) != nullptr && ui_upload_error_is_open(ui),
                  "the oversized file shows the active size notice and error modal"))
      break;
    ui_upload_error_close(ui);
    if (!pm_check(s, strcmp(ui->staged_files[0].filename, names[0]) == 0 && strcmp(ui->staged_files[1].filename, names[1]) == 0,
                  "the attached files keep their pick order around the refused one"))
      break;
    ccord_ui_clear_attachment(ui);
  } while (0);

  ui->d.host.open_file_dialog_async = saved_async;
  g_pick_n = 0;
  for (int i = 0; i < 3; i++) remove(paths[i]);
  remove(big);
}

int ccord_verify_plus_menu_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  pm s = {.c = ctx};
  pm_run(&s);
  if (s.fail[0]) {
    printf("plus-menu: FAIL %s\n", s.fail);
    fflush(stdout);
    return 1;
  }
  printf("plus-menu: PASS checks=%d\n", s.checks);
  fflush(stdout);
  return 0;
}
