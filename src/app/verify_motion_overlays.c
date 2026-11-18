// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Motion area "overlays": every overlay animates on open AND on close; see verify_motion.c.
//
// Drives the real UI over the offline mock with ctx->frame_ms steps (virtual time, 16 ms a frame). For each overlay
// (modal, context menu, pins popover, inbox popover, emoji picker, tooltips) it reads the value the overlay was last
// drawn with (ui->dbg_ovl_t, stamped by ui_overlay_begin in the draw pass) and the frame it was drawn in, and checks:
//   - mid-open and mid-close the overlay is drawn with a value strictly between 0 and 1, which only moves one way;
//   - it is no longer drawn 300 ms after it closed, and the exit takes at most 150 ms;
//   - reopening during the exit continues from the current value (no jump down to 0 and no flash up to 1);
//   - a click on the rect of a closing overlay does not activate what was there (the handlers see it closed);
//   - under reduced motion it is fully drawn the frame after it opens and gone the frame after it closes.
// Nothing here re-implements a draw or a handler: the overlays are opened and closed through the UI's own input
// events and entry points, and the clicks go through ctx->input.
#include "verify_ui_parity.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/error.h>

#include "ui_internal.h"

#define MO_STEP_MS 16u
#define MO_MAX_EXIT_MS 150u   // the longest exit the spec allows
#define MO_GONE_MS 300u       // not drawn any more this long after the close

typedef struct mo {
  const ccord_verify_ui_parity_ctx *c;
  ccord_ui *ui;
  int *checks;
  char fail[200];
} mo;

static bool mo_check(mo *m, bool ok, const char *fmt, ...)
{
  if (ok) {
    (*m->checks)++;
    return true;
  }
  if (!m->fail[0]) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(m->fail, sizeof m->fail, fmt, ap);
    va_end(ap);
    printf("motion overlays: FAIL %s\n", m->fail);
    fflush(stdout);
  }
  return false;
}

static void mo_step(mo *m, int n)
{
  for (int i = 0; i < n; i++) m->c->frame_ms(m->c->ud, MO_STEP_MS);
}

static void mo_send(mo *m, ccord_input_event ev) { m->c->input(m->c->ud, &ev); }

static void mo_move(mo *m, float x, float y)
{
  mo_send(m, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
}

static void mo_click(mo *m, float x, float y)
{
  mo_move(m, x, y);
  mo_send(m, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  mo_send(m, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
}

static void mo_click_rect(mo *m, ccord_rect r) { mo_click(m, r.x + r.w * 0.5f, r.y + r.h * 0.5f); }

static void mo_key(mo *m, ccord_key k)
{
  mo_send(m, (ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = k});
}

static void mo_text(mo *m, const char *text)
{
  ccord_input_event ev = {.type = CCORD_IN_TEXT};
  snprintf(ev.text, sizeof ev.text, "%s", text);
  mo_send(m, ev);
}

// Was the overlay drawn in the most recent frame, and with what value.
static bool mo_drawn(const mo *m, ui_overlay o) { return m->ui->dbg_ovl_frame[o] == m->ui->anim.frame; }
static float mo_t(const mo *m, ui_overlay o) { return m->ui->dbg_ovl_t[o]; }

typedef struct ovl_case {
  const char *name;
  ui_overlay id;
  bool (*open)(mo *m);    // the overlay's own trigger; true when it is open afterwards
  void (*close)(mo *m);   // the overlay's own close
  bool (*is_open)(const mo *m);
  void (*prepare)(mo *m);
} ovl_case;

// ---------------------------------------------------------------- the overlays

static ccord_id mo_guild(const mo *m) { return ccord_store_selected_guild(m->c->store); }

// -- modal: the Add Friend dialog, closed with Escape.
static bool modal_open(mo *m)
{
  m->ui->modal_kind = UI_MODAL_ADD_FRIEND;
  m->ui->modal_focus_field = 0;
  ccord_text_edit_clear(&m->ui->modal_edit1);
  m->ui->dirty = true;
  return true;
}
static void modal_close(mo *m) { mo_key(m, CCORD_KEY_ESCAPE); }
static bool modal_is_open(const mo *m) { return m->ui->modal_kind != UI_MODAL_NONE; }

// -- context menu on the guild header (its "Create Channel" row opens a modal, which a click can be told by).
static bool menu_open(mo *m)
{
  ccord_menu_context ctx = {.target = CCORD_MENU_GUILD, .guild_id = mo_guild(m)};
  ui_menu_open(m->ui, 420.0f, 260.0f, &ctx);
  return m->ui->menu.open;
}
static void menu_close(mo *m) { mo_key(m, CCORD_KEY_ESCAPE); }
static bool menu_is_open(const mo *m) { return m->ui->menu.open; }

// -- pins popover: the header pin button opens it, Escape closes it.
static bool pins_open(mo *m)
{
  mo_click_rect(m, ui_pin_rect(m->ui));
  return m->ui->pins_open;
}
static void pins_close(mo *m) { mo_key(m, CCORD_KEY_ESCAPE); }
static bool pins_is_open(const mo *m) { return m->ui->pins_open; }

// -- inbox popover.
static bool inbox_open(mo *m)
{
  mo_click_rect(m, ui_inbox_rect(m->ui));
  return m->ui->inbox_open;
}
static void inbox_close(mo *m) { mo_key(m, CCORD_KEY_ESCAPE); }
static bool inbox_is_open(const mo *m) { return m->ui->inbox_open; }

// -- expression picker on its emoji tab.
static bool picker_open(mo *m)
{
  ui_expr_picker_open(m->ui, 2, ccord_store_selected_channel(m->c->store), CCORD_ID_NONE);
  return m->ui->emoji_picker_open;
}
static void picker_close(mo *m) { mo_key(m, CCORD_KEY_ESCAPE); }
static bool picker_is_open(const mo *m) { return m->ui->emoji_picker_open; }

// -- tooltip of the header member-list button: the pointer rests on it, then leaves.
static ccord_rect mo_members_btn(const mo *m)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(m->ui, &d);
  return d.members_toggle;
}
static bool tip_open(mo *m)
{
  ccord_rect b = mo_members_btn(m);
  mo_move(m, b.x + b.w * 0.5f, b.y + b.h * 0.5f);
  return b.w > 0.0f;
}
static void tip_close(mo *m) { mo_move(m, 6.0f, m->ui->win_h * 0.5f); }
static bool tip_is_open(const mo *m) { return m->ui->hover_members_btn; }

// -- tooltip of a message timestamp.
static bool tstip_open(mo *m)
{
  if (m->ui->nts_hits == 0) return false;
  ccord_rect b = m->ui->ts_hits[0].rect;
  mo_move(m, b.x + b.w * 0.5f, b.y + b.h * 0.5f);
  return b.w > 0.0f;
}
static bool tstip_is_open(const mo *m) { return m->ui->hover_ts_hit >= 0; }

static const ovl_case cases[] = {
    {"modal", UI_OVL_MODAL, modal_open, modal_close, modal_is_open, nullptr},
    {"context menu", UI_OVL_MENU, menu_open, menu_close, menu_is_open, nullptr},
    {"pins popover", UI_OVL_PINS, pins_open, pins_close, pins_is_open, nullptr},
    {"inbox popover", UI_OVL_INBOX, inbox_open, inbox_close, inbox_is_open, nullptr},
    {"expression picker", UI_OVL_PICKER, picker_open, picker_close, picker_is_open, nullptr},
    {"header tooltip", UI_OVL_TIP, tip_open, tip_close, tip_is_open, nullptr},
    {"timestamp tooltip", UI_OVL_TSTIP, tstip_open, tip_close, tstip_is_open, nullptr},
};

// ---------------------------------------------------------------- the timeline checks

static void mo_settle(mo *m, const ovl_case *oc)
{
  // Long enough for any exit to be over and the overlay to be gone; then nothing of it may be drawn.
  mo_step(m, 30);
  (void)oc;
}

// Open, watch the entrance, close, watch the exit, reopen mid-exit. Normal motion.
static bool mo_timeline(mo *m, const ovl_case *oc)
{
  const char *n = oc->name;
  ui_overlay o = oc->id;
  mo_settle(m, oc);
  if (!mo_check(m, !mo_drawn(m, o), "%s: drawn before it was opened", n)) return false;

  // -- open: strictly between 0 and 1 part-way, rising, and fully there in the end.
  if (!mo_check(m, oc->open(m), "%s: could not be opened", n)) return false;
  mo_step(m, 1);
  if (!mo_check(m, oc->is_open(m), "%s: not open after its trigger", n)) return false;
  mo_step(m, 1);
  float t = mo_t(m, o);
  if (!mo_check(m, mo_drawn(m, o) && t > 0.0f && t < 1.0f, "%s: mid-open it is drawn with %.3f, not strictly between 0 and 1",
                n, (double)t)) {
    return false;
  }
  float prev = t;
  bool full = false;
  for (int i = 0; i < 40 && !full; i++) {
    mo_step(m, 1);
    t = mo_t(m, o);
    if (!mo_check(m, mo_drawn(m, o) && t >= prev - 0.0001f && t <= 1.0f, "%s: the entrance went backwards or was not drawn (%.3f after %.3f)",
                  n, (double)t, (double)prev)) {
      return false;
    }
    prev = t;
    full = t >= 1.0f;
  }
  if (!mo_check(m, full, "%s: never fully open after 640 ms", n)) return false;

  // -- close: drawn, falling, strictly between 0 and 1 part-way, gone within the limit and 300 ms on.
  oc->close(m);
  mo_step(m, 1);
  if (!mo_check(m, !oc->is_open(m), "%s: still open after its close", n)) return false;
  mo_step(m, 1);
  t = mo_t(m, o);
  if (!mo_check(m, mo_drawn(m, o) && t > 0.0f && t < 1.0f, "%s: mid-close it is drawn with %.3f, not strictly between 0 and 1",
                n, (double)t)) {
    return false;
  }
  prev = t;
  uint32_t elapsed = 2u * MO_STEP_MS;
  bool gone = false;
  for (int i = 0; i < 40 && !gone; i++) {
    mo_step(m, 1);
    elapsed += MO_STEP_MS;
    if (mo_drawn(m, o)) {
      t = mo_t(m, o);
      if (!mo_check(m, t <= prev + 0.0001f && t >= 0.0f, "%s: the exit went forwards (%.3f after %.3f)", n, (double)t,
                    (double)prev)) {
        return false;
      }
      prev = t;
    } else {
      gone = true;
    }
  }
  if (!mo_check(m, gone && elapsed <= MO_MAX_EXIT_MS + 3u * MO_STEP_MS, "%s: the exit took %u ms (limit %u ms)", n,
                (unsigned)elapsed, (unsigned)MO_MAX_EXIT_MS)) {
    return false;
  }
  mo_step(m, (int)(MO_GONE_MS / MO_STEP_MS));
  if (!mo_check(m, !mo_drawn(m, o), "%s: still drawn 300 ms after it closed", n)) return false;

  // -- reopen during the exit: it continues from where it is.
  if (!mo_check(m, oc->open(m), "%s: could not be reopened", n)) return false;
  mo_step(m, 12);   // fully in
  oc->close(m);
  mo_step(m, 4);   // 64 ms into the exit
  float before = mo_t(m, o);
  if (!mo_check(m, mo_drawn(m, o) && before > 0.0f && before < 1.0f, "%s: not mid-exit to reopen (%.3f)", n,
                (double)before)) {
    return false;
  }
  if (!mo_check(m, oc->open(m), "%s: could not be reopened mid-exit", n)) return false;
  mo_step(m, 1);
  float resumed = mo_t(m, o);
  if (!mo_check(m, mo_drawn(m, o) && resumed >= before - 0.0001f && resumed <= 1.0f,
                "%s: reopening mid-exit jumped from %.3f to %.3f", n, (double)before, (double)resumed)) {
    return false;
  }
  mo_step(m, 12);
  if (!mo_check(m, mo_drawn(m, o) && mo_t(m, o) >= 1.0f, "%s: not fully open after reopening", n)) return false;
  oc->close(m);
  mo_settle(m, oc);
  return mo_check(m, !mo_drawn(m, o), "%s: still drawn after the reopen test", n);
}

// Reduced motion: fully drawn on the frame after open, gone on the frame after close.
static bool mo_reduced(mo *m, const ovl_case *oc)
{
  const char *n = oc->name;
  ui_overlay o = oc->id;
  mo_settle(m, oc);
  if (!mo_check(m, oc->open(m), "%s (reduced motion): could not be opened", n)) return false;
  mo_step(m, 1);
  if (!mo_check(m, mo_drawn(m, o) && mo_t(m, o) >= 1.0f, "%s (reduced motion): not fully drawn on the frame after open (%.3f)", n,
                (double)mo_t(m, o))) {
    return false;
  }
  oc->close(m);
  mo_step(m, 1);
  if (!mo_check(m, !oc->is_open(m) && !mo_drawn(m, o), "%s (reduced motion): still drawn on the frame after close", n)) return false;
  mo_settle(m, oc);
  return true;
}

// ---------------------------------------------------------------- clicks on a closing overlay

static const char *mo_notice(const mo *m)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(m->ui, &d);
  return d.notice ? d.notice : "";
}

static void mo_reset(mo *m)
{
  // Whatever a previous case left open: closed and drawn out.
  m->ui->modal_kind = UI_MODAL_NONE;
  ui_menu_release(m->ui);
  m->ui->pins_open = m->ui->inbox_open = m->ui->emoji_picker_open = false;
  mo_step(m, 30);
}

// Modal: "Add Friend" with a name nobody has submits on its OK button and says "Could not find user". Open, the click
// does that (control); closing, the same click on the same rect must do nothing.
static bool mo_click_modal(mo *m)
{
  mo_reset(m);
  modal_open(m);
  mo_step(m, 3);
  mo_text(m, "zzz-nobody");
  mo_step(m, 1);
  ccord_rect ok = m->ui->modal_ok_rect;
  if (!mo_check(m, ok.w > 0.0f, "modal: OK button not drawn")) return false;
  uint64_t seq = m->ui->notice_seq;
  mo_click_rect(m, ok);
  mo_step(m, 2);
  if (!mo_check(m, m->ui->notice_seq != seq && strstr(mo_notice(m), "Could not find") != nullptr,
                "modal: clicking OK while open did not submit (control)")) {
    return false;
  }
  mo_reset(m);

  modal_open(m);
  mo_step(m, 3);
  mo_text(m, "zzz-nobody");
  mo_step(m, 1);
  ok = m->ui->modal_ok_rect;
  seq = m->ui->notice_seq;
  modal_close(m);
  mo_step(m, 3);   // mid-exit
  if (!mo_check(m, mo_drawn(m, UI_OVL_MODAL) && mo_t(m, UI_OVL_MODAL) < 1.0f, "modal: not mid-exit for the click")) return false;
  mo_click_rect(m, ok);
  mo_text(m, "q");
  mo_step(m, 1);
  bool typed = strchr(ccord_text_edit_cstr(&m->ui->modal_edit1), 'q') != nullptr;
  if (!mo_check(m, m->ui->notice_seq == seq && m->ui->modal_kind == UI_MODAL_NONE && !typed,
                "modal: a click or key during the exit reached the closing dialog")) {
    return false;
  }
  mo_reset(m);
  return true;
}

// Context menu: "Create Channel" opens a modal. Clicked while open it does (control); clicked at the same spot while
// the menu fades out it must not.
static bool mo_click_menu(mo *m)
{
  mo_reset(m);
  menu_open(m);
  mo_step(m, 3);
  ccord_rect row;
  if (!mo_check(m, ccord_ui_context_menu_find(m->ui, "Create Channel", &row), "menu: no Create Channel row on the guild menu")) return false;
  mo_click_rect(m, row);
  mo_step(m, 2);
  if (!mo_check(m, m->ui->modal_kind == UI_MODAL_CREATE_CHANNEL, "menu: clicking the row while open did not activate it (control)")) return false;
  mo_reset(m);

  menu_open(m);
  mo_step(m, 3);
  if (!mo_check(m, ccord_ui_context_menu_find(m->ui, "Create Channel", &row), "menu: row vanished")) return false;
  menu_close(m);
  mo_step(m, 3);
  if (!mo_check(m, mo_drawn(m, UI_OVL_MENU) && mo_t(m, UI_OVL_MENU) < 1.0f, "menu: not mid-exit for the click")) return false;
  mo_click_rect(m, row);
  mo_step(m, 1);
  if (!mo_check(m, m->ui->modal_kind == UI_MODAL_NONE && !m->ui->menu.open, "menu: a click during the exit activated a row")) return false;
  mo_reset(m);
  return true;
}

// Inbox: the Mentions tab switches the list. Same control/closing pair.
static bool mo_click_inbox(mo *m)
{
  mo_reset(m);
  m->ui->inbox_tab = 0;
  if (!mo_check(m, inbox_open(m), "inbox: could not open")) return false;
  mo_step(m, 4);
  ccord_rect tab = m->ui->inbox_tab_rects[1];
  if (!mo_check(m, tab.w > 0.0f, "inbox: Mentions tab not drawn")) return false;
  mo_click_rect(m, tab);
  mo_step(m, 2);
  if (!mo_check(m, m->ui->inbox_tab == 1, "inbox: clicking the Mentions tab while open did not switch (control)")) return false;
  mo_reset(m);

  m->ui->inbox_tab = 0;
  inbox_open(m);
  mo_step(m, 4);
  tab = m->ui->inbox_tab_rects[1];
  inbox_close(m);
  mo_step(m, 3);
  if (!mo_check(m, mo_drawn(m, UI_OVL_INBOX) && mo_t(m, UI_OVL_INBOX) < 1.0f, "inbox: not mid-exit for the click")) return false;
  mo_click_rect(m, tab);
  mo_step(m, 1);
  if (!mo_check(m, m->ui->inbox_tab == 0 && !m->ui->inbox_open, "inbox: a click during the exit reached the closing popover")) return false;
  mo_reset(m);
  return true;
}

// Opens the pins popover and gives the channel one pin (the mock answers the pin request with none), drawn in.
static bool mo_pins_open_with_pin(mo *m, ccord_id ch, ccord_rect *row)
{
  if (!mo_check(m, pins_open(m), "pins: could not open")) return false;
  mo_step(m, 4);
  ccord_error err;
  // Read the message only now: stepping applies the pins page, which reloads the channel's history.
  size_t n = ccord_store_message_count(m->c->store, ch);
  const ccord_message *pin = n ? ccord_store_message_at(m->c->store, ch, n - 1) : nullptr;
  if (!mo_check(m, pin != nullptr, "pins: the selected channel has no message to pin")) return false;
  ccord_message one = *pin;
  one.pinned = true;
  if (!mo_check(m, ccord_store_set_pins(m->c->store, ch, &one, 1, &err) == CCORD_OK, "pins: could not give the channel a pin")) return false;
  mo_step(m, 4);
  if (!mo_check(m, m->ui->npins_vis > 0, "pins: no pin row drawn")) return false;
  *row = m->ui->pins_vis_rects[0];
  return true;
}

// Pins: a pin row jumps to its message.
static bool mo_click_pins(mo *m)
{
  mo_reset(m);
  ccord_id ch = ccord_store_selected_channel(m->c->store);
  ccord_rect row;
  if (!mo_pins_open_with_pin(m, ch, &row)) return false;
  m->ui->jump_highlight_msg_id = CCORD_ID_NONE;
  mo_click_rect(m, row);
  mo_step(m, 1);
  if (!mo_check(m, m->ui->jump_highlight_msg_id != CCORD_ID_NONE && !m->ui->pins_open,
                "pins: clicking a pin row while open did not jump to it (control)")) {
    return false;
  }
  mo_reset(m);

  if (!mo_pins_open_with_pin(m, ch, &row)) return false;
  m->ui->jump_highlight_msg_id = CCORD_ID_NONE;
  pins_close(m);
  mo_step(m, 3);
  if (!mo_check(m, mo_drawn(m, UI_OVL_PINS) && mo_t(m, UI_OVL_PINS) < 1.0f, "pins: not mid-exit for the click")) return false;
  mo_click_rect(m, row);
  mo_step(m, 1);
  if (!mo_check(m, m->ui->jump_highlight_msg_id == CCORD_ID_NONE && !m->ui->pins_open,
                "pins: a click during the exit reached the closing popover")) {
    return false;
  }
  mo_reset(m);
  return true;
}

// Picker: a click on an emoji cell inserts it into the composer.
static bool mo_click_picker(mo *m)
{
  mo_reset(m);
  ccord_text_edit_clear(&m->ui->edit);
  picker_open(m);
  mo_step(m, 8);
  ui_expr_inspect ins;
  ui_expr_hit_info cell = {0};
  bool found = false;
  if (ui_expr_get_inspect(m->ui, &ins)) {
    for (size_t i = 0; i < ins.nhits && !found; i++) {
      ui_expr_hit_info h;
      if (!ui_expr_hit_at(m->ui, i, &h) || (h.kind != 0 && h.kind != 1)) continue;
      float cx = h.rect.x + h.rect.w * 0.5f, cy = h.rect.y + h.rect.h * 0.5f;
      if (cx < ins.grid_view.x || cx > ins.grid_view.x + ins.grid_view.w || cy < ins.grid_view.y ||
          cy > ins.grid_view.y + ins.grid_view.h) {
        continue;
      }
      cell = h;
      found = true;
    }
  }
  if (!mo_check(m, found, "picker: no emoji cell drawn")) return false;
  mo_click_rect(m, cell.rect);
  mo_step(m, 2);
  if (!mo_check(m, ccord_ui_composer_text(m->ui)[0] != '\0' && !m->ui->emoji_picker_open,
                "picker: clicking a cell while open inserted nothing (control)")) {
    return false;
  }
  mo_reset(m);

  ccord_text_edit_clear(&m->ui->edit);
  picker_open(m);
  mo_step(m, 8);
  found = false;
  if (ui_expr_get_inspect(m->ui, &ins)) {
    for (size_t i = 0; i < ins.nhits && !found; i++) {
      ui_expr_hit_info h;
      if (!ui_expr_hit_at(m->ui, i, &h) || (h.kind != 0 && h.kind != 1)) continue;
      float cx = h.rect.x + h.rect.w * 0.5f, cy = h.rect.y + h.rect.h * 0.5f;
      if (cx < ins.grid_view.x || cx > ins.grid_view.x + ins.grid_view.w || cy < ins.grid_view.y ||
          cy > ins.grid_view.y + ins.grid_view.h) {
        continue;
      }
      cell = h;
      found = true;
    }
  }
  if (!mo_check(m, found, "picker: no emoji cell drawn (second open)")) return false;
  picker_close(m);
  mo_step(m, 3);
  if (!mo_check(m, mo_drawn(m, UI_OVL_PICKER) && mo_t(m, UI_OVL_PICKER) < 1.0f, "picker: not mid-exit for the click")) return false;
  mo_click_rect(m, cell.rect);
  mo_step(m, 1);
  if (!mo_check(m, ccord_ui_composer_text(m->ui)[0] == '\0' && !m->ui->emoji_picker_open,
                "picker: a click during the exit reached the closing popover")) {
    return false;
  }
  mo_reset(m);
  return true;
}

// ---------------------------------------------------------------- setup and entry point

static bool mo_setup(mo *m)
{
  const ccord_verify_ui_parity_ctx *c = m->c;
  ccord_error err;
  mo_step(m, 10);
  ccord_id g0 = ccord_store_selected_guild(c->store);
  // A text channel with history, so the timeline has messages (timestamps to hover, a message to pin).
  for (size_t i = ccord_store_channel_count(c->store, g0); i > 0; i--) {
    const ccord_channel *ch = ccord_store_channel_at(c->store, g0, i - 1);
    if (!ch || ch->kind != CCORD_CHANNEL_TEXT || ch->discord_type != 0) continue;
    if (ccord_session_select_channel(c->session, ch->id, &err) != CCORD_OK) continue;
    mo_step(m, 12);
    if (ccord_store_message_count(c->store, ch->id) > 0) break;
  }
  mo_step(m, 12);
  return mo_check(m, g0 != CCORD_ID_NONE && g0 != CCORD_DM_GUILD_ID &&
                       ccord_store_message_count(c->store, ccord_store_selected_channel(c->store)) > 0,
                  "setup: the mock did not start in a guild text channel with messages");
}

int ccord_verify_motion_overlays(const ccord_verify_ui_parity_ctx *ctx, int *checks)
{
  if (!ctx || !ctx->ui || !ctx->frame_ms || !ctx->input || !ctx->session || !ctx->store || !checks) return 1;
  mo m = {.c = ctx, .ui = ctx->ui, .checks = checks};
  bool saved_reduced = m.ui->settings_reduced_motion;
  m.ui->settings_reduced_motion = false;
  m.ui->anim.reduced_motion = false;
  bool ok = mo_setup(&m);

  for (size_t i = 0; ok && i < sizeof cases / sizeof cases[0]; i++) {
    ok = mo_timeline(&m, &cases[i]);
    if (ok) {
      printf("motion overlays: %s open/close ok\n", cases[i].name);
      fflush(stdout);
    }
  }
  ok = ok && mo_click_modal(&m) && mo_click_menu(&m) && mo_click_pins(&m) && mo_click_inbox(&m) && mo_click_picker(&m);

  m.ui->settings_reduced_motion = true;
  m.ui->anim.reduced_motion = true;
  for (size_t i = 0; ok && i < sizeof cases / sizeof cases[0]; i++) ok = mo_reduced(&m, &cases[i]);

  m.ui->settings_reduced_motion = saved_reduced;
  m.ui->anim.reduced_motion = saved_reduced;
  mo_reset(&m);
  return ok ? 0 : 1;
}
