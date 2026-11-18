// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Schedule Message verification (`ccord --headless --mock --verify-slice schedule-message`).
//
// Drives the real queue (scheduled_messages.c) over the offline mock transport with an injected clock, so nothing
// sleeps: a message scheduled ten minutes ahead is stored, written to disk (mode 0600, under a scratch data dir in
// $TMPDIR), reloaded by a fresh init, sent exactly once when the clock passes its due time, and removed. An entry more
// than five minutes overdue at launch is marked missed and never auto-sent; Cancel removes without sending; a failed
// send keeps the entry with its error; empty text and a time under a minute ahead are refused. Then the dialog itself
// is driven by keys: Enter schedules the composer text and clears the composer, and refuses empty text, a past date
// and staged attachments.
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ccord/core/paths.h>
#include <ccord/store/store.h>

#include "scheduled_messages.h"
#include "ui_internal.h"
#include "verify_ui_parity.h"

#define SM_UNKNOWN_CHANNEL 4999999999ULL

typedef struct sm_v {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
  ccord_id ch;
  int64_t now;
} sm_v;

static int64_t g_clock;
static int64_t sm_clock(void *ud)
{
  (void)ud;
  return g_clock;
}

static bool sm_check(sm_v *v, bool ok, const char *name)
{
  if (!ok) {
    if (!v->fail[0]) snprintf(v->fail, sizeof v->fail, "%s", name);
    return false;
  }
  v->checks++;
  return true;
}

static int sm_rm_cb(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
  (void)sb; (void)flag; (void)ftw;
  return remove(path);
}

static size_t sm_count(const sm_v *v) { return ccord_store_message_count(v->c->store, v->ch); }

static void sm_key(sm_v *v, ccord_key k, uint8_t mods)
{
  ccord_input_event ev = {.type = CCORD_IN_KEY_DOWN, .key = k, .mods = mods};
  v->c->input(v->c->ud, &ev);
}

static void sm_text(sm_v *v, const char *s)
{
  ccord_input_event ev = {.type = CCORD_IN_TEXT};
  snprintf(ev.text, sizeof ev.text, "%s", s);
  v->c->input(v->c->ud, &ev);
}

static bool sm_write(const char *path, const char *text)
{
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  fputs(text, f);
  return fclose(f) == 0;
}

static void sm_settle(sm_v *v) { v->c->frames(v->c->ud, 6); }

// Number of queue entries for the account (all of them are this account's).
static size_t sm_queue_len(void)
{
  ccord_ui_scheduled_item it[CCORD_UI_SCHEDULED_MAX];
  return ccord_scheduled_messages_list(it, CCORD_UI_SCHEDULED_MAX);
}

static bool sm_first(ccord_ui_scheduled_item *out)
{
  return ccord_scheduled_messages_list(out, 1) == 1;
}

static void sm_pick_channel(sm_v *v)
{
  ccord_error err;
  ccord_id g0 = ccord_store_selected_guild(v->c->store);
  for (size_t i = ccord_store_channel_count(v->c->store, g0); i > 0; i--) {
    const ccord_channel *c = ccord_store_channel_at(v->c->store, g0, i - 1);
    if (!c || c->kind != CCORD_CHANNEL_TEXT || c->discord_type != 0) continue;
    if (ccord_session_select_channel(v->c->session, c->id, &err) != CCORD_OK) continue;
    v->c->frames(v->c->ud, 20);
    v->ch = c->id;
    if (ccord_store_message_count(v->c->store, c->id) > 0) break;
  }
}

static void sm_run(sm_v *v)
{
  ccord_session *ses = v->c->session;
  ccord_ui *ui = v->c->ui;
  ccord_error err;
  sm_pick_channel(v);
  if (!sm_check(v, v->ch != CCORD_ID_NONE && ccord_store_self_id(v->c->store) != CCORD_ID_NONE, "mock has a text channel and a self user")) return;
  v->now = ui->now_unix_ms ? ui->now_unix_ms : 1793536200000LL;
  g_clock = v->now;
  ccord_scheduled_messages_set_clock(sm_clock, nullptr);
  ccord_scheduled_messages_init(ses, v->c->store);
  if (!sm_check(v, ccord_scheduled_messages_path()[0] != '\0' && sm_queue_len() == 0, "queue starts empty for the account")) return;

  // ---- refusals ----
  uint64_t id = 0;
  if (!sm_check(v, ccord_scheduled_messages_add(v->ch, "", 0, v->now + 10 * 60000, v->now, &id, &err) != CCORD_OK, "empty text refused")) return;
  if (!sm_check(v, ccord_scheduled_messages_add(v->ch, " \n ", 3, v->now + 10 * 60000, v->now, &id, &err) != CCORD_OK, "blank text refused")) return;
  if (!sm_check(v, ccord_scheduled_messages_add(v->ch, "x", 1, v->now + 30000, v->now, &id, &err) != CCORD_OK, "30 seconds ahead refused")) return;
  if (!sm_check(v, ccord_scheduled_messages_add(v->ch, "x", 1, v->now + 59999, v->now, &id, &err) != CCORD_OK, "59.999 seconds ahead refused")) return;
  if (!sm_check(v, sm_queue_len() == 0, "refused adds leave the queue empty")) return;

  // ---- store, persist, reload ----
  size_t before = sm_count(v);
  if (!sm_check(v, ccord_scheduled_messages_add(v->ch, "hello", 5, v->now + 10 * 60000, v->now, &id, &err) == CCORD_OK && sm_queue_len() == 1, "scheduling hello 10 minutes ahead stores one entry")) return;
  const char *path = ccord_scheduled_messages_path();
  const char *tmp = getenv("TMPDIR");
  struct stat st;
  if (!sm_check(v, stat(path, &st) == 0 && (st.st_mode & 0777) == 0600, "queue file exists with mode 0600")) return;
  if (!sm_check(v, tmp && tmp[0] && strncmp(path, tmp, strlen(tmp)) == 0 && strstr(path, "/accounts/") != nullptr, "queue file is under $TMPDIR in a per-account directory")) return;
  char dir[512];
  snprintf(dir, sizeof dir, "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash) *slash = '\0';
  if (!sm_check(v, stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700, "account directory is mode 0700")) return;
  ccord_scheduled_messages_shutdown();
  ccord_scheduled_messages_init(ses, v->c->store);
  ccord_ui_scheduled_item it;
  if (!sm_check(v, sm_queue_len() == 1 && sm_first(&it) && strcmp(it.preview, "hello") == 0 && it.channel_id == v->ch &&
                       it.due_unix_ms == v->now + 10 * 60000 && !it.missed,
                "a fresh init reloads the entry")) return;

  // ---- not due yet, then due ----
  g_clock = v->now + 5 * 60000;
  ccord_scheduled_messages_tick(ses, g_clock);
  sm_settle(v);
  if (!sm_check(v, sm_count(v) == before && sm_queue_len() == 1, "tick before the due time sends nothing")) return;
  g_clock = v->now + 10 * 60000 + 1000;
  ccord_scheduled_messages_tick(ses, g_clock);
  sm_settle(v);
  const ccord_message *last = sm_count(v) ? ccord_store_message_at(v->c->store, v->ch, sm_count(v) - 1) : nullptr;
  if (!sm_check(v, sm_count(v) == before + 1 && last && last->content && strcmp(last->content, "hello") == 0 &&
                       last->author_id == ccord_store_self_id(v->c->store),
                "tick past due sends exactly one message to the channel")) return;
  if (!sm_check(v, sm_queue_len() == 0, "the queue is empty after the send")) return;
  ccord_scheduled_messages_tick(ses, g_clock + 1000);
  sm_settle(v);
  if (!sm_check(v, sm_count(v) == before + 1, "a second tick sends nothing more")) return;
  ccord_scheduled_messages_shutdown();
  ccord_scheduled_messages_init(ses, v->c->store);
  if (!sm_check(v, sm_queue_len() == 0, "the emptied queue stays empty after a reload")) return;

  // ---- cancel ----
  int64_t t1 = g_clock;
  before = sm_count(v);
  if (!sm_check(v, ccord_scheduled_messages_add(v->ch, "never sent", 10, t1 + 2 * 60000, t1, &id, &err) == CCORD_OK, "schedule an entry to cancel")) return;
  if (!sm_check(v, ccord_scheduled_messages_cancel(id) && sm_queue_len() == 0 && !ccord_scheduled_messages_cancel(id), "Cancel removes the entry")) return;
  g_clock = t1 + 10 * 60000;
  ccord_scheduled_messages_tick(ses, g_clock);
  sm_settle(v);
  if (!sm_check(v, sm_count(v) == before, "a cancelled entry is never sent")) return;

  // ---- missed: more than 5 minutes overdue at init ----
  int64_t t2 = g_clock;
  if (!sm_check(v, ccord_scheduled_messages_add(v->ch, "late one", 8, t2 + 2 * 60000, t2, &id, &err) == CCORD_OK &&
                       ccord_scheduled_messages_add(v->ch, "late two", 8, t2 + 3 * 60000, t2, &id, &err) == CCORD_OK,
                "schedule two entries to miss")) return;
  ccord_scheduled_messages_shutdown();
  g_clock = t2 + 3 * 60000 + CCORD_SCHEDULED_MISSED_MS + 1000;   // the later one is 5 min 1 s overdue
  ccord_scheduled_messages_init(ses, v->c->store);
  ccord_ui_scheduled_item list[4];
  size_t n = ccord_scheduled_messages_list(list, 4);
  if (!sm_check(v, n == 2 && list[0].missed && list[1].missed, "entries more than 5 minutes overdue at init are marked missed")) return;
  ccord_scheduled_messages_tick(ses, g_clock);
  ccord_scheduled_messages_tick(ses, g_clock + 120000);
  sm_settle(v);
  if (!sm_check(v, sm_count(v) == before && sm_queue_len() == 2, "tick never auto-sends a missed entry")) return;
  if (!sm_check(v, ccord_scheduled_messages_send_now(list[0].id, &err) == CCORD_OK, "Send now sends a missed entry")) return;
  sm_settle(v);
  last = ccord_store_message_at(v->c->store, v->ch, sm_count(v) - 1);
  if (!sm_check(v, sm_count(v) == before + 1 && last && strcmp(last->content, "late one") == 0 && sm_queue_len() == 1, "the missed entry went out once and left the queue")) return;
  if (!sm_check(v, ccord_scheduled_messages_cancel(list[1].id) && sm_queue_len() == 0 && sm_count(v) == before + 1, "Delete drops a missed entry without sending")) return;

  // ---- a failed send keeps the entry with an error ----
  int64_t t3 = g_clock;
  if (!sm_check(v, ccord_scheduled_messages_add(SM_UNKNOWN_CHANNEL, "doomed", 6, t3 + 2 * 60000, t3, &id, &err) == CCORD_OK, "schedule to a channel the session refuses")) return;
  g_clock = t3 + 2 * 60000 + 1000;
  ccord_scheduled_messages_tick(ses, g_clock);
  if (!sm_check(v, sm_queue_len() == 1 && sm_first(&it) && it.error[0] != '\0' && !it.missed, "a failed send keeps the entry with an error")) return;
  ccord_scheduled_messages_shutdown();
  ccord_scheduled_messages_init(ses, v->c->store);
  if (!sm_check(v, sm_queue_len() == 1 && sm_first(&it) && it.error[0] != '\0', "the error survives a reload")) return;
  if (!sm_check(v, ccord_scheduled_messages_send_now(it.id, &err) != CCORD_OK && sm_queue_len() == 1, "Send now on a failing entry keeps it")) return;
  if (!sm_check(v, ccord_scheduled_messages_cancel(it.id) && sm_queue_len() == 0, "the failing entry can be cancelled")) return;

  // ---- the dialog: Enter schedules the composer text and clears the composer ----
  g_clock = v->now;   // the ui reads its own (fixed) clock; the queue is validated against ui->now_unix_ms
  ccord_text_edit_clear(&ui->edit);
  (void)ccord_text_edit_insert(&ui->edit, "from the composer", 17, &err);
  ui_schedule_dialog_open(ui);
  sm_settle(v);
  if (!sm_check(v, ui_schedule_dialog_is_open(ui), "the schedule dialog opens")) return;
  sm_key(v, CCORD_KEY_RETURN, 0);
  sm_settle(v);
  if (!sm_check(v, sm_queue_len() == 1 && sm_first(&it) && strcmp(it.preview, "from the composer") == 0 && it.channel_id == v->ch &&
                       it.due_unix_ms >= ui->now_unix_ms + 59 * 60000 && it.due_unix_ms <= ui->now_unix_ms + 61 * 60000,
                "Schedule in the dialog queues the text for about an hour ahead")) return;
  if (!sm_check(v, ui->edit.len == 0, "scheduling clears the composer text")) return;
  sm_key(v, CCORD_KEY_RETURN, 0);   // the dialog's own text is empty now
  sm_settle(v);
  if (!sm_check(v, sm_queue_len() == 1, "Schedule is refused for empty text")) return;
  sm_text(v, "second");
  sm_key(v, CCORD_KEY_TAB, 0);      // date field
  sm_key(v, CCORD_KEY_A, CCORD_MOD_CTRL);
  sm_text(v, "2020-01-01");
  sm_key(v, CCORD_KEY_RETURN, 0);
  sm_settle(v);
  if (!sm_check(v, sm_queue_len() == 1, "Schedule is refused for a time in the past")) return;
  sm_key(v, CCORD_KEY_A, CCORD_MOD_CTRL);
  sm_text(v, "2999-13-45");
  sm_key(v, CCORD_KEY_RETURN, 0);
  if (!sm_check(v, sm_queue_len() == 1, "Schedule is refused for a date that does not exist")) return;
  ui->staged_file_count = 1;   // no frame runs while a fake attachment is staged
  sm_key(v, CCORD_KEY_RETURN, 0);
  bool staged_ok = sm_queue_len() == 1;
  ui->staged_file_count = 0;
  if (!sm_check(v, staged_ok, "Schedule is refused while attachments are staged")) return;
  sm_key(v, CCORD_KEY_ESCAPE, 0);
  sm_settle(v);
  if (!sm_check(v, !ui_schedule_dialog_is_open(ui), "Escape closes the dialog")) return;
  ui_schedule_dialog_open(ui);   // reopening lists the pending entry
  sm_settle(v);
  if (!sm_check(v, ui_schedule_dialog_is_open(ui) && sm_queue_len() == 1, "the reopened dialog still lists the pending entry")) return;
  ui_schedule_dialog_close(ui);
  (void)ccord_scheduled_messages_cancel(it.id);

  // ---- attachments ----
  const char *tmpd = getenv("TMPDIR");
  char fa[600], fb[600];
  snprintf(fa, sizeof fa, "%s/sched-a.txt", tmpd && tmpd[0] ? tmpd : "/tmp");
  snprintf(fb, sizeof fb, "%s/sched-b.txt", tmpd && tmpd[0] ? tmpd : "/tmp");
  if (!sm_check(v, sm_write(fa, "alpha") && sm_write(fb, "bravo!"), "write two attachment files")) return;
  // through the dialog: stage both files in the composer, schedule, and the composer is cleared like a send
  g_clock = v->now;
  ccord_text_edit_clear(&ui->edit);
  (void)ccord_text_edit_insert(&ui->edit, "with files", 10, &err);
  if (!sm_check(v, ccord_ui_attach_file(ui, fa) && ccord_ui_attach_file(ui, fb) && ui->staged_file_count == 2, "two files staged in the composer")) return;
  ui_schedule_dialog_open(ui);
  sm_settle(v);
  sm_key(v, CCORD_KEY_RETURN, 0);
  sm_settle(v);
  if (!sm_check(v, sm_queue_len() == 1 && sm_first(&it) && it.attachments == 2 && strcmp(it.preview, "with files") == 0,
                "an item scheduled with two attachments lists both")) return;
  if (!sm_check(v, ui->staged_file_count == 0, "scheduling clears the staged attachments from the composer")) return;
  ui_schedule_dialog_close(ui);
  if (!sm_check(v, stat(path, &st) == 0 && (st.st_mode & 0777) == 0600, "queue file is mode 0600 after a write with attachments")) return;
  ccord_scheduled_messages_shutdown();
  ccord_scheduled_messages_init(ses, v->c->store);
  if (!sm_check(v, sm_queue_len() == 1 && sm_first(&it) && it.attachments == 2, "attachments survive a reload")) return;
  ui_schedule_dialog_open(ui);
  sm_settle(v);
  if (!sm_check(v, ui_schedule_dialog_debug_first_row_attachments() == 2, "the pending list shows the attachment count")) return;
  ui_schedule_dialog_close(ui);
  before = sm_count(v);
  g_clock = it.due_unix_ms + 1000;
  ccord_scheduled_messages_tick(ses, g_clock);
  sm_settle(v);
  last = sm_count(v) ? ccord_store_message_at(v->c->store, v->ch, sm_count(v) - 1) : nullptr;
  if (!sm_check(v, sm_count(v) == before + 1 && last && last->content && strcmp(last->content, "with files") == 0 && last->rich && last->rich->attachment_count == 2 && sm_queue_len() == 0,
                "when due the item is sent as one message carrying both files")) return;

  // a file deleted before the due time: failed with the file named, nothing sent
  int64_t t4 = g_clock;
  before = sm_count(v);
  const char *pp[2] = {fa, fb};
  if (!sm_check(v, ccord_scheduled_messages_add_files(v->ch, "needs both", 10, t4 + 2 * 60000, t4, pp, nullptr, 2, &id, &err) == CCORD_OK, "schedule two files again")) return;
  (void)unlink(fb);
  g_clock = t4 + 2 * 60000 + 1000;
  ccord_scheduled_messages_tick(ses, g_clock);
  sm_settle(v);
  if (!sm_check(v, sm_count(v) == before && sm_queue_len() == 1 && sm_first(&it) && strstr(it.error, "sched-b.txt") != nullptr,
                "a deleted attachment marks the item failed naming the file, and nothing is sent")) return;
  if (!sm_check(v, ccord_scheduled_messages_send_now(it.id, &err) != CCORD_OK && sm_count(v) == before, "Send now with a missing file sends nothing")) return;
  (void)ccord_scheduled_messages_cancel(it.id);
  (void)unlink(fa);

  // a queue file written before attachments existed still loads and sends
  int64_t t5 = g_clock;
  before = sm_count(v);
  char qpath[512];
  snprintf(qpath, sizeof qpath, "%s", path);
  ccord_scheduled_messages_shutdown();
  {
    FILE *f = fopen(qpath, "wb");
    char body[512];
    snprintf(body, sizeof body, "{\"version\":1,\"next_id\":7,\"entries\":[{\"id\":6,\"channel\":\"%llu\",\"due\":%lld,\"text\":\"old format\",\"error\":\"\",\"missed\":false}]}",
             (unsigned long long)v->ch, (long long)(t5 + 60000));
    if (!sm_check(v, f != nullptr, "write an old-format queue")) return;
    fputs(body, f);
    fclose(f);
  }
  ccord_scheduled_messages_init(ses, v->c->store);
  if (!sm_check(v, sm_queue_len() == 1 && sm_first(&it) && it.attachments == 0 && strcmp(it.preview, "old format") == 0, "an old-format queue file loads")) return;
  g_clock = t5 + 61000;
  ccord_scheduled_messages_tick(ses, g_clock);
  sm_settle(v);
  last = sm_count(v) ? ccord_store_message_at(v->c->store, v->ch, sm_count(v) - 1) : nullptr;
  if (!sm_check(v, sm_count(v) == before + 1 && last && strcmp(last->content, "old format") == 0, "the old-format item sends")) return;
}

int ccord_verify_schedule_message_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->input || !ctx->frames) {
    puts("schedule-message: FAIL invalid context");
    return 1;
  }
  const char *base = getenv("TMPDIR");
  char scratch[512];
  snprintf(scratch, sizeof scratch, "%s/ccord-sched-XXXXXX", base && base[0] ? base : "/tmp");
  if (!mkdtemp(scratch)) {
    puts("schedule-message: FAIL scratch dir");
    return 1;
  }
  char orig[512];
  snprintf(orig, sizeof orig, "%s", ccord_paths_data_dir());
  ccord_paths_set_data_dir(scratch);

  sm_v v = {.c = ctx};
  ctx->frames(ctx->ud, 10);   // let the mock session finish its handshake first
  sm_run(&v);

  ccord_scheduled_messages_shutdown();
  ccord_scheduled_messages_set_clock(nullptr, nullptr);
  ccord_paths_set_data_dir(orig);
  (void)nftw(scratch, sm_rm_cb, 8, FTW_DEPTH | FTW_PHYS);
  if (v.fail[0]) {
    printf("schedule-message: FAIL %s\n", v.fail);
    return 1;
  }
  printf("schedule-message: PASS checks=%d\n", v.checks);
  fflush(stdout);
  return 0;
}
