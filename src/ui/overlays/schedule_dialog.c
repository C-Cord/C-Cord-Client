// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _DEFAULT_SOURCE
// Schedule Message dialog. Takes the composer's text (editable here), the target channel, a local date and time (with
// presets) and queues the message through the host's scheduled_* callbacks (the app keeps the on-disk queue and sends
// the message when due). Attached files are scheduled with it by path. The same dialog lists this account's pending
// messages with Send now / Cancel (Delete for a missed one).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ccord/core/alloc.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>

#include "ui_internal.h"
#include "ui_style.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define SD_ROW_H 56.0f
#define SD_ROWS_MAX 3
#define SD_H 600.0f
#define SD_H_FIXED (SD_H - (float)SD_ROWS_MAX * SD_ROW_H)
#define SD_MSG_H 88.0f

enum { SD_FOCUS_MSG, SD_FOCUS_DATE, SD_FOCUS_TIME };
enum { SD_PRESET_15M, SD_PRESET_1H, SD_PRESET_TOMORROW, SD_PRESET_COUNT };

typedef struct sd_row_rects {
  ccord_rect send, cancel;
} sd_row_rects;

// One dialog per process (one window); lazily allocated on open and freed on close.
typedef struct sd_state {
  ccord_text_edit msg, date, time;
  int focus;
  char status[160];
  bool status_ok;                       // status is a confirmation, not an error
  ccord_id channel_id;
  ccord_ui_scheduled_item items[CCORD_UI_SCHEDULED_MAX];
  size_t nitems, first_row;
  ccord_rect box, body_view, msg_rect, date_rect, time_rect, preset[SD_PRESET_COUNT], schedule, close, x_btn;
  sd_row_rects rows[SD_ROWS_MAX];
  bool click_pending;                   // a click in the message box, resolved in draw (needs the renderer)
  float click_x, click_y;
  float msg_scroll;
  float body_scroll, body_max, body_content_h;
  float row_wheel_acc;
} sd_state;

static sd_state *sd;
static size_t g_sd_dbg_att_drawn;       // attachment count shown on the first drawn pending row

static bool sd_ensure(void)
{
  if (sd) return true;
  sd = ccord_calloc(1, sizeof *sd);
  if (!sd) return false;
  ccord_text_edit_init(&sd->msg, CCORD_SEND_MAX_BYTES);
  ccord_text_edit_init(&sd->date, 16);
  ccord_text_edit_init(&sd->time, 8);
  return true;
}

static void sd_free_state(void)
{
  if (!sd) return;
  ccord_text_edit_free(&sd->msg);
  ccord_text_edit_free(&sd->date);
  ccord_text_edit_free(&sd->time);
  ccord_free(sd);
  sd = nullptr;
}

// ---- time helpers ----------------------------------------------------------------------------------------------

static bool sd_local_tm(const ccord_ui *ui, int64_t ms, struct tm *out)
{
  time_t t = (time_t)(ms / 1000);
  return ui->use_utc ? ccord_gmtime(t, out) : ccord_localtime(t, out);
}

static void sd_set_edit(ccord_text_edit *e, const char *s)
{
  ccord_text_edit_clear(e);
  ccord_error err;
  (void)ccord_text_edit_insert(e, s, strlen(s), &err);
}

static void sd_set_fields(const ccord_ui *ui, int64_t ms)
{
  struct tm tm;
  if (!sd_local_tm(ui, ms, &tm)) return;
  char d[48], t[32];
  snprintf(d, sizeof d, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  snprintf(t, sizeof t, "%02d:%02d", tm.tm_hour, tm.tm_min);
  sd_set_edit(&sd->date, d);
  sd_set_edit(&sd->time, t);
}

static void sd_apply_preset(ccord_ui *ui, int preset)
{
  int64_t now = ui->now_unix_ms;
  if (preset == SD_PRESET_15M) {
    sd_set_fields(ui, now + 15 * 60 * 1000);
  } else if (preset == SD_PRESET_1H) {
    sd_set_fields(ui, now + 60 * 60 * 1000);
  } else {
    struct tm tm;
    if (!sd_local_tm(ui, now, &tm)) return;
    tm.tm_mday += 1;
    tm.tm_hour = 9;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t t = ui->use_utc ? ccord_timegm(&tm) : mktime(&tm);
    sd_set_fields(ui, (int64_t)t * 1000);
  }
  sd->status[0] = '\0';
  ui->dirty = true;
}

// Reads the date and time fields as a local time. False (with `why`) when they are not a real date and time.
static bool sd_parse_due(const ccord_ui *ui, int64_t *out_ms, char *why, size_t why_cap)
{
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, n1 = 0, n2 = 0;
  const char *ds = ccord_text_edit_cstr(&sd->date), *ts = ccord_text_edit_cstr(&sd->time);
  if (sscanf(ds, "%d-%d-%d%n", &y, &mo, &d, &n1) != 3 || ds[n1] != '\0' || y < 1970 || y > 2200) {
    snprintf(why, why_cap, "Enter the date as YYYY-MM-DD.");
    return false;
  }
  if (sscanf(ts, "%d:%d%n", &h, &mi, &n2) != 2 || ts[n2] != '\0') {
    snprintf(why, why_cap, "Enter the time as HH:MM (24-hour).");
    return false;
  }
  struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_isdst = -1};
  struct tm chk = tm;
  time_t t = ui->use_utc ? ccord_timegm(&chk) : mktime(&chk);
  if (t == (time_t)-1 || chk.tm_year != tm.tm_year || chk.tm_mon != tm.tm_mon || chk.tm_mday != tm.tm_mday ||
      chk.tm_hour != tm.tm_hour || chk.tm_min != tm.tm_min) {
    snprintf(why, why_cap, "That date or time does not exist.");
    return false;
  }
  *out_ms = (int64_t)t * 1000;
  return true;
}

static void sd_fmt_time(const ccord_ui *ui, int64_t ms, char *out, size_t cap)
{
  struct tm tm;
  if (!sd_local_tm(ui, ms, &tm) || strftime(out, cap, "%a %b %d, %H:%M", &tm) == 0) snprintf(out, cap, "?");
}

// Why Schedule is unavailable right now, or nullptr when it can go. Fills *due when the time fields parse.
static const char *sd_blocker(const ccord_ui *ui, int64_t *due, char *buf, size_t cap)
{
  if (!ui->d.host.scheduled_add) return "Scheduling is not available in this build.";
  if (sd->channel_id == CCORD_ID_NONE) return "Select a text channel first.";
  for (size_t k = 0; k < ui->staged_file_count; k++) {
    if (!ui->staged_files[k].path[0]) return "A pasted image cannot be scheduled: remove it or attach a file from disk.";
  }
  const char *t = ccord_text_edit_cstr(&sd->msg);
  size_t i = 0;
  while (t[i] == ' ' || t[i] == '\n' || t[i] == '\t' || t[i] == '\r') i++;
  if (t[i] == '\0' && ui->staged_file_count == 0) return "Write a message to schedule.";
  if (!sd_parse_due(ui, due, buf, cap)) return buf;
  if (*due < ui->now_unix_ms + 60 * 1000) return "Pick a time at least 1 minute from now.";
  return nullptr;
}

static void sd_refresh_items(const ccord_ui *ui)
{
  sd->nitems = ui->d.host.scheduled_list ? ui->d.host.scheduled_list(ui->d.host.ud, sd->items, CCORD_UI_SCHEDULED_MAX) : 0;
  if (sd->nitems > CCORD_UI_SCHEDULED_MAX) sd->nitems = CCORD_UI_SCHEDULED_MAX;
  size_t max_first = sd->nitems > SD_ROWS_MAX ? sd->nitems - SD_ROWS_MAX : 0;
  if (sd->first_row > max_first) sd->first_row = max_first;
}

// ---- open / close ----------------------------------------------------------------------------------------------

void ui_schedule_dialog_free(ccord_ui *ui)
{
  (void)ui;
  sd_free_state();
  g_sd_dbg_att_drawn = 0;
}

void ui_schedule_dialog_open(ccord_ui *ui)
{
  if (!ui) return;
  if (!sd_ensure()) return;
  ui->plus_menu_open = false;
  const ccord_channel *ch = ui_selected_channel(ui);
  sd->channel_id = ch && ui_channel_has_chat(ch) ? ch->id : CCORD_ID_NONE;
  sd_set_edit(&sd->msg, ccord_text_edit_cstr(&ui->edit));
  sd->focus = SD_FOCUS_MSG;
  sd->status[0] = '\0';
  sd->first_row = 0;
  sd->msg_scroll = 0.0f;
  sd->click_pending = false;
  sd_apply_preset(ui, SD_PRESET_1H);
  sd_refresh_items(ui);
  ui->schedule_open = true;
  ui->dirty = true;
}

void ui_schedule_dialog_close(ccord_ui *ui)
{
  if (!ui) return;
  ui->schedule_open = false;
  sd_free_state();
  ui->dirty = true;
}

size_t ui_schedule_dialog_debug_first_row_attachments(void) { return g_sd_dbg_att_drawn; }
bool ui_schedule_dialog_is_open(const ccord_ui *ui) { return ui && ui->schedule_open; }

// ---- actions ---------------------------------------------------------------------------------------------------

static void sd_do_schedule(ccord_ui *ui)
{
  int64_t due = 0;
  char buf[96];
  const char *why = sd_blocker(ui, &due, buf, sizeof buf);
  if (why) {
    snprintf(sd->status, sizeof sd->status, "%s", why);
    sd->status_ok = false;
    ui->dirty = true;
    return;
  }
  char err[128] = "";
  const char *paths[CCORD_RICH_MAX_ATTACHMENTS];
  bool spoilers[CCORD_RICH_MAX_ATTACHMENTS];
  size_t np = ui->staged_file_count < CCORD_RICH_MAX_ATTACHMENTS ? ui->staged_file_count : CCORD_RICH_MAX_ATTACHMENTS;
  for (size_t k = 0; k < np; k++) {
    paths[k] = ui->staged_files[k].path;
    spoilers[k] = ui->staged_files[k].spoiler;
  }
  const ccord_channel *sch = ui->d.store ? ccord_store_channel(ui->d.store, sd->channel_id) : nullptr;
  ccord_id sgid = sch ? sch->guild_id : CCORD_ID_NONE;
  char serialized_msg[CCORD_SEND_MAX_BYTES + 1];
  size_t sm_len = ccord_ui_serialize_mentions(ui->d.store, sgid, ccord_text_edit_cstr(&sd->msg), sd->msg.len,
                                              serialized_msg, sizeof serialized_msg);
  if (!ui->d.host.scheduled_add(ui->d.host.ud, sd->channel_id, serialized_msg, sm_len, due,
                                ui->now_unix_ms, paths, spoilers, np, err, sizeof err)) {
    snprintf(sd->status, sizeof sd->status, "%s", err[0] ? err : "Could not schedule the message.");
    sd->status_ok = false;
    ui->dirty = true;
    return;
  }
  // The composer's text and files are now the scheduled message: clear them (and the saved draft) like a send does.
  ccord_ui_clear_attachment(ui);
  ccord_text_edit_clear(&ui->edit);
  ui_composer_reset_scroll(ui);
  ui_clear_channel_draft(ui, sd->channel_id);
  ccord_text_edit_clear(&sd->msg);
  sd->msg_scroll = 0.0f;
  char when[48];
  sd_fmt_time(ui, due, when, sizeof when);
  snprintf(sd->status, sizeof sd->status, "Scheduled for %s.", when);
  sd->status_ok = true;
  sd_refresh_items(ui);
  ui->dirty = true;
}

static void sd_row_action(ccord_ui *ui, size_t item, bool send_now)
{
  if (item >= sd->nitems) return;
  uint64_t id = sd->items[item].id;
  char err[128] = "";
  bool ok;
  if (send_now) {
    ok = ui->d.host.scheduled_send_now && ui->d.host.scheduled_send_now(ui->d.host.ud, id, err, sizeof err);
    if (ok) snprintf(sd->status, sizeof sd->status, "Sent.");
    else snprintf(sd->status, sizeof sd->status, "Could not send: %s", err[0] ? err : "not available");
  } else {
    ok = ui->d.host.scheduled_cancel && ui->d.host.scheduled_cancel(ui->d.host.ud, id);
    snprintf(sd->status, sizeof sd->status, ok ? "Removed." : "That message is no longer scheduled.");
  }
  sd->status_ok = ok;
  sd_refresh_items(ui);
  ui->dirty = true;
}

// ---- draw ------------------------------------------------------------------------------------------------------

static void sd_text(ccord_ui *ui, ccord_renderer *r, const char *s, float x, float y, float max_w, ccord_font_style font,
                    float size, ccord_color c)
{
  ccord_text_style st = ui_style(ui, font, size, c);
  ccord_text_draw_ellipsis(r, s, strlen(s), x, y, max_w, &st);
}

static const char *sd_channel_name(const ccord_ui *ui, ccord_id id, char *buf, size_t cap)
{
  const ccord_channel *ch = ui->d.store ? ccord_store_channel(ui->d.store, id) : nullptr;
  if (!ch || !ch->name[0]) snprintf(buf, cap, "unknown channel");
  else snprintf(buf, cap, "%s%s", ch->guild_id == CCORD_DM_GUILD_ID ? "@" : "#", ch->name);
  return buf;
}

static void sd_draw_message_box(ccord_ui *ui, ccord_renderer *r, ccord_rect box)
{
  float rad = M(channel_item_radius);
  bool focused = sd->focus == SD_FOCUS_MSG;
  if (focused) ccord_draw_rrect(r, (ccord_rect){box.x - 1.0f, box.y - 1.0f, box.w + 2.0f, box.h + 2.0f}, rad + 1.0f, COL(brand));
  ccord_draw_rrect(r, box, rad, COL(bg_input));
  float fs = M(font_size_base);
  float lh = ui_line_h(ui, fs);
  float wrap = box.w - 2.0f * UIS_SP_3;
  const char *text = ccord_text_edit_cstr(&sd->msg);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, fs, COL(text_normal));
  float ox = box.x + UIS_SP_3, oy = box.y + UIS_SP_2;
  if (sd->msg.len == 0) {
    sd_text(ui, r, "Message to send later", ox, oy, wrap, CCORD_FONT_REGULAR, fs, COL(text_placeholder));
    if (focused && ui->caret_on) ccord_draw_rect(r, (ccord_rect){ox, oy, 1.5f, lh}, COL(caret));
    if (focused) ui->text_area = box;
    return;
  }
  float view_h = box.h - 2.0f * UIS_SP_2;
  if (sd->click_pending) {
    size_t pos = ccord_text_hit_test(r, text, sd->msg.len, &ts, wrap, sd->click_x - ox, sd->click_y - oy + sd->msg_scroll);
    ccord_text_edit_set_caret(&sd->msg, pos, false);
    sd->click_pending = false;
  }
  ccord_rect cr = ccord_text_caret_rect(r, text, sd->msg.len, &ts, wrap, sd->msg.caret);
  if (cr.y < sd->msg_scroll) sd->msg_scroll = cr.y;
  if (cr.y + lh > sd->msg_scroll + view_h) sd->msg_scroll = cr.y + lh - view_h;
  if (sd->msg_scroll < 0.0f) sd->msg_scroll = 0.0f;
  ccord_push_clip(r, (ccord_rect){box.x, box.y + UIS_SP_1, box.w, box.h - 2.0f * UIS_SP_1});
  ccord_text_draw(r, text, sd->msg.len, ox, oy - sd->msg_scroll, &ts, wrap);
  if (focused && ui->caret_on) ccord_draw_rect(r, (ccord_rect){ox + cr.x, oy + cr.y - sd->msg_scroll, 1.5f, lh}, COL(caret));
  ccord_pop_clip(r);
  if (focused) ui->text_area = box;
}

void ui_schedule_dialog_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r || !ui->schedule_open) return;
  if (!sd_ensure()) return;
  sd_refresh_items(ui);
  ccord_rect box = uis_modal_box(ui, UIS_MODAL_W_MD, SD_H);
  sd->box = box;
  ui_popup_begin(ui, r, "schedlg", 0, UI_MODAL_MS, UIS_SLIDE_MODAL);
  uis_modal_frame(ui, r, box);
  float y = uis_modal_title(ui, r, box, "Schedule Message");
  float x0 = box.x + UIS_SP_5, w = box.w - 2.0f * UIS_SP_5;
  float fs_c = M(font_size_small);

  sd->x_btn = (ccord_rect){box.x + box.w - UIS_SP_3 - UIS_ICON_BTN, box.y + UIS_SP_5 + ui_line_h(ui, M(font_size_large)) * 0.5f - UIS_ICON_BTN * 0.5f,
                           UIS_ICON_BTN, UIS_ICON_BTN};
  bool xh = ui->mouse_in && ui_in_rect(ui->mx, ui->my, sd->x_btn);
  ccord_color xc = uis_icon_button(ui, r, sd->x_btn, (uis_state){.hover = xh}, "schedx", 0);
  ui_icon_close(r, sd->x_btn.x + sd->x_btn.w * 0.5f, sd->x_btn.y + sd->x_btn.h * 0.5f, UIS_ICON_GLYPH, xc);

  // footer computed early so the scrollable body clips cleanly above it
  sd->schedule = uis_modal_footer_button(box, 0, 120.0f);
  sd->close = uis_modal_footer_button(box, 1, 90.0f);
  float body_top = y;
  sd->body_view = (ccord_rect){box.x + UIS_SP_2, body_top, box.w - 2.0f * UIS_SP_2,
                               fmaxf(sd->schedule.y - UIS_SP_2 - body_top, 0.0f)};
  sd->body_scroll = fminf(fmaxf(sd->body_scroll, 0.0f), sd->body_max);
  ccord_push_clip(r, sd->body_view);
  y -= sd->body_scroll;

  // target channel
  char name[128], line[200];
  if (sd->channel_id != CCORD_ID_NONE) snprintf(line, sizeof line, "Sending to %s", sd_channel_name(ui, sd->channel_id, name, sizeof name));
  else snprintf(line, sizeof line, "No text channel selected");
  sd_text(ui, r, line, x0, y, w, CCORD_FONT_BOLD, fs_c, COL(text_header));
  y += 22.0f;

  sd->msg_rect = (ccord_rect){x0, y, w, SD_MSG_H};
  sd_draw_message_box(ui, r, sd->msg_rect);
  y += SD_MSG_H + UIS_SP_3;

  // date and time
  float gap = UIS_SP_2, dw = (w - gap) * 0.55f, tw = w - gap - dw;
  sd_text(ui, r, "DATE (YYYY-MM-DD)", x0, y, dw, CCORD_FONT_BOLD, fs_c - 3.0f, COL(text_muted));
  sd_text(ui, r, "TIME (HH:MM, LOCAL)", x0 + dw + gap, y, tw, CCORD_FONT_BOLD, fs_c - 3.0f, COL(text_muted));
  y += 16.0f;
  sd->date_rect = (ccord_rect){x0, y, dw, UIS_H_INPUT};
  sd->time_rect = (ccord_rect){x0 + dw + gap, y, tw, UIS_H_INPUT};
  uis_input(ui, r, sd->date_rect, &sd->date, "YYYY-MM-DD", (uis_state){.focused = sd->focus == SD_FOCUS_DATE}, false, "scheddate", 0);
  uis_input(ui, r, sd->time_rect, &sd->time, "HH:MM", (uis_state){.focused = sd->focus == SD_FOCUS_TIME}, false, "schedtime", 0);
  y += UIS_H_INPUT + UIS_SP_2;

  // presets (wrap to a second row on narrow phone viewports)
  static const char *k_presets[SD_PRESET_COUNT] = {"In 15 minutes", "In 1 hour", "Tomorrow 9:00"};
  float px = x0;
  for (int i = 0; i < SD_PRESET_COUNT; i++) {
    float bw = uis_label_width(ui, r, k_presets[i]);
    if (px > x0 && px + bw > x0 + w + 1.0f) {
      px = x0;
      y += UIS_H_BTN_SM + UIS_SP_1;
    }
    sd->preset[i] = (ccord_rect){px, y, fminf(bw, w), UIS_H_BTN_SM};
    bool hv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, sd->preset[i]);
    uis_button(ui, r, sd->preset[i], k_presets[i], UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){.hover = hv}, "schedpre", (uint64_t)i);
    px += sd->preset[i].w + UIS_SP_2;
  }
  y += UIS_H_BTN_SM + UIS_SP_2;

  // text-only note, then why Schedule is blocked (or a result)
  bool staged = ui->staged_file_count > 0;
  if (staged) snprintf(line, sizeof line, "%zu attached file%s will be scheduled with the message.", ui->staged_file_count, ui->staged_file_count == 1 ? "" : "s");
  else snprintf(line, sizeof line, "Files attached in the composer are scheduled with the message.");
  sd_text(ui, r, line, x0, y, w, CCORD_FONT_REGULAR, fs_c - 1.0f, COL(text_muted));
  y += 18.0f;
  int64_t due = 0;
  char buf[96];
  const char *why = sd_blocker(ui, &due, buf, sizeof buf);
  if (sd->status[0]) {
    sd_text(ui, r, sd->status, x0, y, w, CCORD_FONT_REGULAR, fs_c - 1.0f, sd->status_ok ? COL(text_normal) : COL(danger));
  } else if (why) {
    sd_text(ui, r, why, x0, y, w, CCORD_FONT_REGULAR, fs_c - 1.0f, COL(text_muted));
  } else {
    char when[48];
    sd_fmt_time(ui, due, when, sizeof when);
    snprintf(line, sizeof line, "Will be sent %s", when);
    sd_text(ui, r, line, x0, y, w, CCORD_FONT_REGULAR, fs_c - 1.0f, COL(text_normal));
  }
  y += 18.0f + UIS_SP_2;
  uis_divider(ui, r, x0, y, w);
  y += UIS_SP_2;

  sd_text(ui, r, "Sent locally while C-Cord is open", x0, y, w, CCORD_FONT_REGULAR, fs_c - 2.0f, COL(text_muted));
  y += 18.0f;
  // pending list
  snprintf(line, sizeof line, "SCHEDULED MESSAGES (%zu)", sd->nitems);
  sd_text(ui, r, line, x0, y, w, CCORD_FONT_BOLD, fs_c - 3.0f, COL(text_muted));
  y += 20.0f;
  int rows_fit = (int)((box.h - SD_H_FIXED) / SD_ROW_H);
  if (rows_fit < 1 && sd->nitems > 0) rows_fit = 1;
  if (rows_fit > SD_ROWS_MAX) rows_fit = SD_ROWS_MAX;
  memset(sd->rows, 0, sizeof sd->rows);
  if (sd->nitems == 0) {
    sd_text(ui, r, "Nothing scheduled.", x0, y + 4.0f, w, CCORD_FONT_REGULAR, fs_c, COL(text_muted));
    y += 28.0f;
  }
  for (int i = 0; i < rows_fit && sd->first_row + (size_t)i < sd->nitems; i++) {
    const ccord_ui_scheduled_item *it = &sd->items[sd->first_row + (size_t)i];
    ccord_rect row = {x0, y + (float)i * SD_ROW_H, w, SD_ROW_H - 4.0f};
    uis_card(ui, r, row);
    const char *cancel_label = it->missed ? "Delete" : "Cancel";
    float cw = uis_label_width(ui, r, cancel_label), sw = uis_label_width(ui, r, "Send now");
    sd->rows[i].cancel = (ccord_rect){row.x + row.w - UIS_SP_2 - cw, row.y + (row.h - UIS_H_BTN_SM) * 0.5f, cw, UIS_H_BTN_SM};
    sd->rows[i].send = (ccord_rect){sd->rows[i].cancel.x - UIS_SP_2 - sw, sd->rows[i].cancel.y, sw, UIS_H_BTN_SM};
    bool sh = ui->mouse_in && ui_in_rect(ui->mx, ui->my, sd->rows[i].send);
    bool ch = ui->mouse_in && ui_in_rect(ui->mx, ui->my, sd->rows[i].cancel);
    uis_button(ui, r, sd->rows[i].send, "Send now", UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){.hover = sh}, "schedsend", (uint64_t)i);
    uis_button(ui, r, sd->rows[i].cancel, cancel_label, UIS_BTN_DANGER_SOFT, UIS_BTN_S_SM, (uis_state){.hover = ch}, "schedcancel", (uint64_t)i);
    float tx = row.x + UIS_SP_3, tmax = fmaxf(sd->rows[i].send.x - UIS_SP_2 - tx, 40.0f);
    sd_text(ui, r, it->preview, tx, row.y + 5.0f, tmax, CCORD_FONT_BOLD, fs_c, COL(text_header));
    char when[48], cn[128];
    sd_fmt_time(ui, it->due_unix_ms, when, sizeof when);
    char att[48] = "";
    if (i == 0) g_sd_dbg_att_drawn = it->attachments;
    if (it->attachments > 0) snprintf(att, sizeof att, "  %zu file%s", it->attachments, it->attachments == 1 ? "" : "s");
    snprintf(line, sizeof line, "%s%s  %s%s", it->missed ? "Missed, was due " : "", when, sd_channel_name(ui, it->channel_id, cn, sizeof cn), att);
    sd_text(ui, r, line, tx, row.y + 22.0f, tmax, CCORD_FONT_REGULAR, fs_c - 2.0f, it->missed ? COL(danger) : COL(text_muted));
    if (it->error[0]) sd_text(ui, r, it->error, tx, row.y + 37.0f, tmax, CCORD_FONT_REGULAR, fs_c - 2.0f, COL(danger));
  }
  if (rows_fit > 0 && sd->nitems > 0) {
    size_t drawn = sd->nitems - sd->first_row;
    if (drawn > (size_t)rows_fit) drawn = (size_t)rows_fit;
    y += (float)drawn * SD_ROW_H;
  }
  if (rows_fit > 0 && sd->nitems > (size_t)rows_fit) {
    snprintf(line, sizeof line, "%zu-%zu of %zu (scroll)", sd->first_row + 1, sd->first_row + (size_t)rows_fit < sd->nitems ? sd->first_row + (size_t)rows_fit : sd->nitems, sd->nitems);
    sd_text(ui, r, line, x0 + w - 150.0f, y - (float)rows_fit * SD_ROW_H - 18.0f, 150.0f, CCORD_FONT_REGULAR, fs_c - 2.0f, COL(text_muted));
  }
  sd->body_content_h = fmaxf((y + sd->body_scroll) - body_top + 6.0f, 0.0f);
  sd->body_max = sd->body_content_h > sd->body_view.h ? (sd->body_content_h - sd->body_view.h) : 0.0f;
  ccord_pop_clip(r);
  if (sd->body_max > 0.0f) {
    (void)ui_draw_list_scrollbar(ui, r, sd->body_view, sd->body_content_h, sd->body_scroll, "schedscroll", 0);
  }

  // footer: Schedule (right), Close
  bool sch_h = ui->mouse_in && why == nullptr && ui_in_rect(ui->mx, ui->my, sd->schedule);
  bool cl_h = ui->mouse_in && ui_in_rect(ui->mx, ui->my, sd->close);
  uis_button(ui, r, sd->schedule, "Schedule", UIS_BTN_PRIMARY, UIS_BTN_S_MD, (uis_state){.hover = sch_h, .disabled = why != nullptr}, "schedgo", 0);
  uis_button(ui, r, sd->close, "Close", UIS_BTN_SECONDARY, UIS_BTN_S_MD, (uis_state){.hover = cl_h}, "schedclose", 0);
  ui_popup_end(r);
}

// ---- input -----------------------------------------------------------------------------------------------------

static ccord_text_edit *sd_focus_edit(void)
{
  return sd->focus == SD_FOCUS_DATE ? &sd->date : sd->focus == SD_FOCUS_TIME ? &sd->time : &sd->msg;
}

// Date and time fields take digits, '-' and ':' only.
static void sd_insert(ccord_ui *ui, const char *s, size_t n)
{
  ccord_text_edit *e = sd_focus_edit();
  char clean[512];
  size_t m = 0;
  for (size_t i = 0; i < n && m < sizeof clean - 1; i++) {
    unsigned char c = (unsigned char)s[i];
    if (sd->focus != SD_FOCUS_MSG && !((c >= '0' && c <= '9') || c == '-' || c == ':')) continue;
    if (sd->focus == SD_FOCUS_MSG && (c < 0x20 && c != '\n' && c != '\t')) continue;
    clean[m++] = (char)c;
  }
  if (m == 0) return;
  size_t valid_len = m;
  while (valid_len > 0 && !ccord_utf8_valid(clean, valid_len)) valid_len--;
  if (valid_len == 0) return;
  ccord_error err;
  (void)ccord_text_edit_insert(e, clean, valid_len, &err);
  sd->status[0] = '\0';
  ui->caret_on = true;
  ui->dirty = true;
}

static bool sd_key(ccord_ui *ui, const ccord_input_event *ev)
{
  ccord_text_edit *e = sd_focus_edit();
  bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0, shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
  switch (ev->key) {
    case CCORD_KEY_ESCAPE: ui_schedule_dialog_close(ui); return true;
    case CCORD_KEY_TAB: sd->focus = (sd->focus + (shift ? 2 : 1)) % 3; break;
    case CCORD_KEY_RETURN:
      if (sd->focus == SD_FOCUS_MSG && shift) sd_insert(ui, "\n", 1);
      else sd_do_schedule(ui);
      break;
    case CCORD_KEY_BACKSPACE: if (ctrl) ccord_text_edit_backspace_word(e); else ccord_text_edit_backspace(e); sd->status[0] = '\0'; break;
    case CCORD_KEY_DELETE: if (ctrl) ccord_text_edit_delete_word(e); else ccord_text_edit_delete(e); sd->status[0] = '\0'; break;
    case CCORD_KEY_LEFT: ccord_text_edit_move(e, ctrl ? -3 : -1, shift); break;
    case CCORD_KEY_RIGHT: ccord_text_edit_move(e, ctrl ? 3 : 1, shift); break;
    case CCORD_KEY_HOME: ccord_text_edit_move(e, ctrl ? -2 : -4, shift); break;
    case CCORD_KEY_END: ccord_text_edit_move(e, ctrl ? 2 : 4, shift); break;
    case CCORD_KEY_A:
      if (ctrl && sd->focus != SD_FOCUS_MSG) ccord_text_edit_select_all(e);
      break;
    case CCORD_KEY_V:
      if (ctrl && ui->d.host.clipboard_get) {
        char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
        if (clip) {
          sd_insert(ui, clip, strlen(clip));
          ccord_free(clip);
        }
      }
      break;
    case CCORD_KEY_Z: if (ctrl) (void)ccord_text_edit_undo(e); break;
    default: break;
  }
  ui->caret_on = true;
  ui->dirty = true;
  return true;
}

static bool sd_click(ccord_ui *ui, float x, float y)
{
  if (!ui_in_rect(x, y, sd->box)) {   // a click outside the dialog dismisses it
    ui_schedule_dialog_close(ui);
    return true;
  }
  if (ui_in_rect(x, y, sd->x_btn) || ui_in_rect(x, y, sd->close)) {
    ui_schedule_dialog_close(ui);
    return true;
  }
  if (ui_in_rect(x, y, sd->schedule)) {
    ui->touch_text_focused = false;
    sd_do_schedule(ui);
    return true;
  }
  if (sd->body_view.h > 0.0f && !ui_in_rect(x, y, sd->body_view)) return true;
  bool focused_input = false;
  if (ui_in_rect(x, y, sd->msg_rect)) {
    sd->focus = SD_FOCUS_MSG;
    sd->click_pending = true;
    sd->click_x = x;
    sd->click_y = y;
    focused_input = true;
  } else if (ui_in_rect(x, y, sd->date_rect)) {
    sd->focus = SD_FOCUS_DATE;
    ccord_text_edit_set_caret(&sd->date, sd->date.len, false);
    focused_input = true;
  } else if (ui_in_rect(x, y, sd->time_rect)) {
    sd->focus = SD_FOCUS_TIME;
    ccord_text_edit_set_caret(&sd->time, sd->time.len, false);
    focused_input = true;
  }
  if (ui->touch_mode) {
    ui->touch_text_focused = focused_input;
    if (!focused_input && ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
  }
  for (int i = 0; i < SD_PRESET_COUNT; i++) {
    if (ui_in_rect(x, y, sd->preset[i])) sd_apply_preset(ui, i);
  }
  for (int i = 0; i < SD_ROWS_MAX; i++) {
    if (sd->rows[i].send.w > 0.0f && ui_in_rect(x, y, sd->rows[i].send)) sd_row_action(ui, sd->first_row + (size_t)i, true);
    else if (sd->rows[i].cancel.w > 0.0f && ui_in_rect(x, y, sd->rows[i].cancel)) sd_row_action(ui, sd->first_row + (size_t)i, false);
  }
  ui->dirty = true;
  return true;
}

bool ui_schedule_dialog_input(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || !ui->schedule_open || !sd) return false;
  switch (ev->type) {
    case CCORD_IN_KEY_DOWN:
      return sd_key(ui, ev);
    case CCORD_IN_TEXT:
      sd_insert(ui, ev->text, strnlen(ev->text, sizeof ev->text));
      return true;
    case CCORD_IN_MOUSE_DOWN:
      return ev->button == 1 ? sd_click(ui, ev->x, ev->y) : true;
    case CCORD_IN_MOUSE_UP:
      return true;
    case CCORD_IN_WHEEL:
      if (sd->body_max > 0.0f) {
        ui_scroll_offset_by(ui, &sd->body_scroll, ui_wheel_delta_px(ui, ev, 36.0f), sd->body_max);
        ui->dirty = true;
      } else if (sd->nitems > SD_ROWS_MAX) {
        size_t max_first = sd->nitems - SD_ROWS_MAX;
        if (ui->touch_scroll_direct_override != 0) {
          sd->row_wheel_acc += ui->touch_scroll_px;
          while (sd->row_wheel_acc >= SD_ROW_H) {
            if (sd->first_row < max_first) sd->first_row++;
            sd->row_wheel_acc -= SD_ROW_H;
          }
          while (sd->row_wheel_acc <= -SD_ROW_H) {
            if (sd->first_row > 0) sd->first_row--;
            sd->row_wheel_acc += SD_ROW_H;
          }
        } else {
          if (ev->wheel > 0.0f && sd->first_row > 0) sd->first_row--;
          else if (ev->wheel < 0.0f && sd->first_row < max_first) sd->first_row++;
        }
        ui->dirty = true;
      }
      return true;
    default:
      return false;   // pointer moves still reach the ui so hover tracking keeps working
  }
}
