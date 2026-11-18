// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Message context-menu reactions (--verify-slice ctx-react); see verify_ui_parity.h.
//
// Drives the real UI over the offline mock transport. A confirmed message's menu has an "Add Reaction" row and no
// "React (👍)" row. Activating the row opens the emoji picker aimed at the right-clicked message; picking an emoji toggles
// that reaction on that message; Escape dismisses the picker and sends nothing. An unconfirmed message's menu has no Add
// Reaction row, as its hover bar has no react button. Rows and cells are clicked where the draw pass recorded them, and
// the reaction state is read back from the store after the mock's gateway echo.
#include "verify_ui_parity.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/error.h>

#include "ui_internal.h"

#define CR_OLD_REACT_LABEL "React (\xF0\x9F\x91\x8D)"
#define CR_MAX_REACTIONS 16

// The probe message's reactions, copied out of the store.
typedef struct cr_snap {
  size_t n;
  char emoji[CR_MAX_REACTIONS][96];
  bool self[CR_MAX_REACTIONS];
} cr_snap;

typedef struct cr {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
  ccord_id ch;
  ccord_id msg;   // a confirmed message of the selected channel, present in the mock's history
} cr;

// Records the first failed check; the run stops at the first false one.
static bool cr_check(cr *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

static void cr_frames(cr *s, int n) { s->c->frames(s->c->ud, n); }

static void cr_send(cr *s, ccord_input_event ev) { s->c->input(s->c->ud, &ev); }

// Moves the pointer onto (x, y), presses and releases the button there, then lets the frames settle.
static void cr_click(cr *s, float x, float y, uint8_t button)
{
  cr_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  cr_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = button, .clicks = 1, .x = x, .y = y});
  cr_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = button, .x = x, .y = y});
  cr_frames(s, 3);
}

// Scrolls the message timeline to its newest row so the probe message is on screen.
static void cr_to_bottom(cr *s)
{
  const ccord_layout *l = ccord_ui_get_layout(s->c->ui);
  cr_send(s, (ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = l->message_area.x + 100.0f,
                                 .y = l->message_area.y + 100.0f});
  cr_frames(s, 4);
}

// Right-clicks the drawn row of message `id`; true when the message menu is open on that message.
static bool cr_open_menu(cr *s, ccord_id id)
{
  const ccord_ui *ui = s->c->ui;
  const ccord_layout *l = ccord_ui_get_layout(ui);
  for (size_t i = 0; i < ui->nvis; i++) {
    if (ui->vis[i].msg_id != id) continue;
    float x = l->message_area.x + l->message_area.w * 0.5f, y = ui->vis[i].y + ui->vis[i].h * 0.5f;
    cr_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
    cr_frames(s, 2);
    cr_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 3, .clicks = 1, .x = x, .y = y});
    cr_frames(s, 3);
    return ui->menu.open && ui->menu.ctx.target == CCORD_MENU_MESSAGE && ui->menu.ctx.message_id == id;
  }
  return false;
}

// Index of the top-level menu row with this label, or -1.
static int cr_row(const ccord_ui *ui, const char *label)
{
  for (size_t i = 0; i < ui->menu.count; i++) {
    const menu_item *it = &ui->menu.items[i];
    if (it->parent < 0 && it->label && strcmp(it->label, label) == 0) return (int)i;
  }
  return -1;
}

// Clicks the top-level menu row with this label; false when the open menu has no such row.
static bool cr_click_row(cr *s, const char *label)
{
  int idx = cr_row(s->c->ui, label);
  if (idx < 0) return false;
  ccord_rect r = ui_menu_item_rect(s->c->ui, (size_t)idx);
  cr_click(s, r.x + r.w * 0.5f, r.y + r.h * 0.5f, 1);
  return true;
}

static void cr_snapshot(cr *s, cr_snap *out)
{
  memset(out, 0, sizeof *out);
  const ccord_message *m = ccord_store_find_message(s->c->store, s->ch, s->msg);
  if (!m || !m->reactions) return;
  for (size_t i = 0; i < m->reaction_kind_count && out->n < CR_MAX_REACTIONS; i++) {
    const ccord_reaction *r = &m->reactions[i];
    if (!r->emoji) continue;
    snprintf(out->emoji[out->n], sizeof out->emoji[0], "%s", r->emoji);
    out->self[out->n] = r->self_reacted;
    out->n++;
  }
}

static int cr_find(const cr_snap *a, const char *emoji)
{
  for (size_t i = 0; i < a->n; i++) {
    if (strcmp(a->emoji[i], emoji) == 0) return (int)i;
  }
  return -1;
}

// Number of reactions whose signed-in-user flag differs between the two snapshots (added, removed or toggled).
static int cr_flips(const cr_snap *before, const cr_snap *after)
{
  int flips = 0;
  for (size_t i = 0; i < after->n; i++) {
    int j = cr_find(before, after->emoji[i]);
    bool was = j >= 0 && before->self[j];
    if (was != after->self[i]) flips++;
  }
  for (size_t j = 0; j < before->n; j++) {
    if (before->self[j] && cr_find(after, before->emoji[j]) < 0) flips++;
  }
  return flips;
}

// Selects the last plain text channel with history and takes its newest confirmed message as the probe.
static void cr_pick_message(cr *s)
{
  ccord_id g0 = ccord_store_selected_guild(s->c->store);
  ccord_error err;
  for (size_t i = ccord_store_channel_count(s->c->store, g0); i > 0; i--) {
    const ccord_channel *c = ccord_store_channel_at(s->c->store, g0, i - 1);
    if (!c || c->kind != CCORD_CHANNEL_TEXT || c->discord_type != 0) continue;
    if (ccord_session_select_channel(s->c->session, c->id, &err) != CCORD_OK) continue;
    cr_frames(s, 20);
    if (ccord_store_message_count(s->c->store, c->id) > 0) break;
  }
  s->ch = ccord_store_selected_channel(s->c->store);
  size_t n = ccord_store_message_count(s->c->store, s->ch);
  for (size_t i = n; i > 0 && s->msg == CCORD_ID_NONE; i--) {
    const ccord_message *m = ccord_store_message_at(s->c->store, s->ch, i - 1);
    if (m && m->send_state == CCORD_MSG_SEND_CONFIRMED) s->msg = m->id;
  }
}

// ---- reaction layout: where every pill and its emoji are drawn ------------------------------------------------------
//
// The draw pass records each pill, the box its emoji was drawn in and its message (ui->dbg_reaction_draws). The checks
// below seed reactions on every loaded message (Unicode, custom, burst, enough kinds to wrap) and require each emoji box
// inside its pill, each pill inside its message's row and inside the message list. A glyph that is drawn bigger than its
// box or at the wrong place cannot be seen in those rects, so the frame itself is also captured and compared: adding a
// fallback font (what a first CJK, Thai or maths character in any text does) or opening and closing the picker must not
// change one pixel of the message list.

static const char *const cr_pool[] = {"\xF0\x9F\x94\x97", "\xE2\xAD\x90", "\xE2\x9C\xA8", "\xF0\x9F\x99\x8F",
                                      "\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x94\xA5",
                                      "\xF0\x9F\x98\x82", "\xF0\x9F\x8E\x89", "\xF0\x9F\x91\x80",
                                      "\xF0\x9F\x98\xAD", "\xF0\x9F\x92\xAF", "\xE2\x9C\x85", "\xF0\x9F\x9A\x80"};
#define CR_POOL_N (sizeof cr_pool / sizeof cr_pool[0])

typedef struct cr_frame {
  uint8_t *rgba;
  int w, h;
  float scale;
} cr_frame;

static void cr_frame_free(cr_frame *f)
{
  ccord_free(f->rgba);
  *f = (cr_frame){0};
}

// One real frame (begin, draw, capture, end) saved to `path` and decoded.
static bool cr_capture(cr *s, const char *path, cr_frame *out)
{
  *out = (cr_frame){0};
  ccord_error err;
  char tmp[512];
  if (!path) {   // no file asked for: a scratch one under $TMPDIR
    const char *dir = getenv("TMPDIR");
    snprintf(tmp, sizeof tmp, "%s/ctx-react-frame.png", dir && dir[0] ? dir : ".");
    path = tmp;
  }
  ccord_render_begin(s->c->renderer, (ccord_color){0x31, 0x33, 0x38, 255});
  ccord_ui_draw(s->c->ui, s->c->renderer);
  ccord_status st = ccord_render_screenshot(s->c->renderer, path, &err);
  ccord_render_end(s->c->renderer);
  if (st != CCORD_OK) return false;
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = len > 0 ? (uint8_t *)malloc((size_t)len) : nullptr;
  size_t got = buf ? fread(buf, 1, (size_t)len, f) : 0;
  fclose(f);
  bool ok = buf && got == (size_t)len &&
            ccord_image_decode_rgba(buf, got, &out->rgba, &out->w, &out->h, &err) == CCORD_OK;
  free(buf);
  if (!ok) return false;
  out->scale = s->c->ui->win_w > 0 ? (float)out->w / (float)s->c->ui->win_w : 1.0f;
  return true;
}

// Pixels of the message list that differ between two frames (any channel off by more than 2).
static long cr_diff(const cr_frame *a, const cr_frame *b, ccord_rect area)
{
  if (!a->rgba || !b->rgba || a->w != b->w || a->h != b->h) return -1;
  int x0 = (int)(area.x * a->scale), y0 = (int)(area.y * a->scale);
  int x1 = (int)((area.x + area.w) * a->scale), y1 = (int)((area.y + area.h) * a->scale);
  if (x1 > a->w) x1 = a->w;
  if (y1 > a->h) y1 = a->h;
  long n = 0;
  for (int y = y0 < 0 ? 0 : y0; y < y1; y++) {
    for (int x = x0 < 0 ? 0 : x0; x < x1; x++) {
      const uint8_t *p = a->rgba + ((size_t)y * (size_t)a->w + (size_t)x) * 4u;
      const uint8_t *q = b->rgba + ((size_t)y * (size_t)b->w + (size_t)x) * 4u;
      if (abs(p[0] - q[0]) > 2 || abs(p[1] - q[1]) > 2 || abs(p[2] - q[2]) > 2) n++;
    }
  }
  return n;
}

static bool cr_inside(ccord_rect in, ccord_rect out)
{
  const float e = 0.5f;
  return in.x >= out.x - e && in.y >= out.y - e && in.x + in.w <= out.x + out.w + e && in.y + in.h <= out.y + out.h + e;
}

// Seeds every loaded message of the channel with a different kind of reaction row.
static void cr_seed_reactions(cr *s)
{
  ccord_error err;
  size_t n = ccord_store_message_count(s->c->store, s->ch);
  for (size_t i = 0; i < n; i++) {
    const ccord_message *m = ccord_store_message_at(s->c->store, s->ch, i);
    if (!m) continue;
    ccord_id id = m->id;
    switch (i % 3) {
    case 0:   // the four Unicode emoji the report names
      for (unsigned k = 0; k < 4; k++) {
        (void)ccord_store_set_reaction_ex(s->c->store, s->ch, id, cr_pool[k], 2 + k, k == 1, 0, false, &err);
      }
      break;
    case 1:   // a custom emoji, and burst (super) reactions
      (void)ccord_store_set_reaction_ex(s->c->store, s->ch, id, "party:123456789", 3, false, 0, false, &err);
      (void)ccord_store_set_reaction_ex(s->c->store, s->ch, id, cr_pool[0], 5, true, 2, true, &err);
      (void)ccord_store_set_reaction_ex(s->c->store, s->ch, id, cr_pool[1], 5, false, 1, false, &err);
      break;
    default:  // enough kinds to wrap onto a second row
      for (unsigned k = 0; k < CR_POOL_N; k++) {
        (void)ccord_store_set_reaction_ex(s->c->store, s->ch, id, cr_pool[k], 10 + k, false, 0, false, &err);
      }
      break;
    }
  }
  cr_frames(s, 6);
  cr_to_bottom(s);
  cr_frames(s, 6);
}

// The drawn pills: each emoji box inside its pill, each pill inside its message's row (and inside the list when the row
// is). `kinds` collects what was seen: bit 0 Unicode, 1 custom, 2 burst, 3 a message with pills on two rows.
static bool cr_pills_in_place(cr *s, unsigned *kinds, const char **why)
{
  const ccord_ui *ui = s->c->ui;
  ccord_rect list = ui_rect(ccord_ui_get_layout(ui)->message_area);
  *kinds = 0;
  if (ui->ndbg_reaction_draws == 0) {
    *why = "reaction pills are drawn";
    return false;
  }
  for (size_t d = 0; d < ui->ndbg_reaction_draws; d++) {
    const ui_reaction_draw *rd = &ui->dbg_reaction_draws[d];
    const vis_row *row = nullptr;
    for (size_t i = 0; i < ui->nvis; i++) {
      if (ui->vis[i].msg_id == rd->message_id) row = &ui->vis[i];
    }
    if (!row) {
      *why = "every drawn pill belongs to a drawn message row";
      return false;
    }
    ccord_rect rr = {list.x, row->y, list.w, row->h};
    if (!cr_inside(rd->emoji, rd->pill)) {
      *why = "each reaction emoji is inside its pill";
      return false;
    }
    if (!cr_inside(rd->pill, rr)) {
      *why = "each pill is inside its message's row";
      return false;
    }
    if (cr_inside(rr, list) && !cr_inside(rd->pill, list)) {
      *why = "each pill of a visible row is inside the message list";
      return false;
    }
    *kinds |= rd->custom ? 2u : 1u;
    if (rd->burst) *kinds |= 4u;
    for (size_t e = 0; e < ui->ndbg_reaction_draws; e++) {
      const ui_reaction_draw *o = &ui->dbg_reaction_draws[e];
      if (o->message_id == rd->message_id && o->pill.y > rd->pill.y + 4.0f) *kinds |= 8u;
    }
  }
  return true;
}

static bool cr_reaction_layout(cr *s)
{
  ccord_ui *ui = s->c->ui;
  ccord_error err;
  const char *why = "";
  unsigned kinds = 0;
  const char *shot = getenv("CR_SHOT");   // the slice's own capture; docs/screenshots/reaction-draw.png comes from it
  cr_frame a = {0}, b = {0};

  cr_seed_reactions(s);
  // Park the pointer on the server rail: no hover bar or wash over the list while frames are compared.
  cr_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = 30.0f, .y = 400.0f});
  cr_frames(s, 12);
  if (!cr_check(s, cr_pills_in_place(s, &kinds, &why), why[0] ? why : "pills in place")) return false;
  if (!cr_check(s, (kinds & 1u) && (kinds & 2u), "Unicode and custom reactions are drawn")) return false;
  if (!cr_check(s, (kinds & 4u) != 0, "burst reactions are drawn")) return false;
  if (!cr_check(s, (kinds & 8u) != 0, "a message with many reactions wraps onto a second row")) return false;
  if (!cr_check(s, cr_capture(s, shot, &a), "frame captured")) return false;
  ccord_rect list = ui_rect(ccord_ui_get_layout(ui)->message_area);

  // A first CJK, Thai, Arabic, Hebrew or maths character anywhere in the app attaches system fonts to the open fonts. The
  // pills already drawn must not change: no emoji grows, moves or vanishes.
  {
    ccord_text_style ts = {.font = CCORD_FONT_REGULAR, .size = 13.0f, .color = {255, 255, 255, 255}};
    const char *odd = "\xE1\x84\x80\xE0\xB8\x81\xE0\xA4\x95\xD8\xA7\xD7\x90\xF0\x9D\x94\x84\xE2\xA0\x81\xE4\xB8\x82";
    int before = ccord_render_fallback_font_count(s->c->renderer);
    (void)ccord_text_measure(s->c->renderer, odd, strlen(odd), &ts, 0.0f);
    ts.size = 14.0f;
    ts.font = CCORD_FONT_BOLD;
    (void)ccord_text_measure(s->c->renderer, odd, strlen(odd), &ts, 0.0f);
    printf("ctx-react: fallback fonts %d -> %d\n", before, ccord_render_fallback_font_count(s->c->renderer));
  }
  cr_frames(s, 6);
  cr_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = 30.0f, .y = 400.0f});
  cr_frames(s, 2);
  const char *shot_b = getenv("CR_SHOT_B");
  if (!cr_check(s, cr_capture(s, shot_b, &b), "frame captured after a font fallback")) {
    cr_frame_free(&a);
    return false;
  }
  long d1 = cr_diff(&a, &b, list);
  printf("ctx-react: message list pixels changed by a font fallback: %ld\n", d1);
  bool ok = cr_check(s, d1 == 0, "adding a fallback font leaves every drawn reaction emoji as it was");
  cr_frame_free(&b);
  if (!ok) {
    cr_frame_free(&a);
    return false;
  }
  if (!cr_check(s, cr_pills_in_place(s, &kinds, &why), why[0] ? why : "pills in place after a fallback")) return false;

  // A reaction picked for a message and the picker then dismissed leaves nothing of the picker or the pick in the list.
  ui_expr_picker_open(ui, 2, s->ch, s->msg);
  cr_frames(s, 4);
  if (!cr_check(s, ui->emoji_picker_open, "reaction picker opens")) {
    cr_frame_free(&a);
    return false;
  }
  cr_send(s, (ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_ESCAPE});
  cr_frames(s, 20);
  cr_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = 30.0f, .y = 400.0f});
  cr_frames(s, 2);
  if (!cr_check(s, cr_capture(s, nullptr, &b), "frame captured after the picker closed")) {
    cr_frame_free(&a);
    return false;
  }
  long d2 = cr_diff(&a, &b, list);
  printf("ctx-react: message list pixels changed by the picker: %ld\n", d2);
  ok = cr_check(s, d2 == 0, "a closed picker leaves the message list as it was");
  cr_frame_free(&a);
  cr_frame_free(&b);
  if (!ok) return false;

  // The picker left open across a channel switch is drawn with its own panel or not at all.
  ui_expr_picker_open(ui, 2, s->ch, s->msg);
  cr_frames(s, 4);
  ccord_id g0 = ccord_store_selected_guild(s->c->store);
  for (size_t i = ccord_store_channel_count(s->c->store, g0); i > 0; i--) {
    const ccord_channel *c = ccord_store_channel_at(s->c->store, g0, i - 1);
    if (!c || c->kind != CCORD_CHANNEL_TEXT || c->discord_type != 0 || c->id == s->ch) continue;
    (void)ccord_session_select_channel(s->c->session, c->id, &err);
    break;
  }
  cr_frames(s, 20);
  printf("ctx-react: picker after a channel switch: open=%d rect=%.0fx%.0f\n", (int)ui->emoji_picker_open,
         (double)ui->emoji_picker_rect.w, (double)ui->emoji_picker_rect.h);
  if (!cr_check(s, !ui->emoji_picker_open || (ui->emoji_picker_rect.w > 0.0f && ui->emoji_picker_rect.h > 0.0f),
                "a picker left open across a channel switch still has its panel")) {
    return false;
  }
  ui_expr_picker_close(ui);
  cr_frames(s, 20);
  return true;
}

static void cr_run(cr *s)
{
  ccord_ui *ui = s->c->ui;
  cr_pick_message(s);
  if (!cr_check(s, s->ch != CCORD_ID_NONE && s->msg != CCORD_ID_NONE, "mock channel has a confirmed message")) return;
  cr_to_bottom(s);

  // --- the menu rows ------------------------------------------------------------------------------------------------
  if (!cr_check(s, cr_open_menu(s, s->msg), "right-click on the message opens its menu")) return;
  if (!cr_check(s, cr_row(ui, "Add Reaction") >= 0, "message menu has an Add Reaction row")) return;
  if (!cr_check(s, cr_row(ui, CR_OLD_REACT_LABEL) < 0, "message menu has no React (👍) row")) return;

  // --- Add Reaction opens the picker and sends nothing by itself ----------------------------------------------------
  cr_snap before, after;
  cr_snapshot(s, &before);
  if (!cr_check(s, cr_click_row(s, "Add Reaction"), "Add Reaction row can be clicked")) return;
  cr_frames(s, 3);
  if (!cr_check(s, ui->emoji_picker_open && ui->emoji_picker_msg_id == s->msg && ui->emoji_picker_channel == s->ch,
                "Add Reaction opens the emoji picker for the right-clicked message")) {
    return;
  }
  if (!cr_check(s, !ui->menu.open, "the menu closes once its row is activated")) return;
  cr_snapshot(s, &after);
  if (!cr_check(s, cr_flips(&before, &after) == 0, "opening the picker sends no reaction")) return;
  printf("ctx-react: Add Reaction opens the picker\n");

  // --- Escape dismisses the picker and sends nothing ----------------------------------------------------------------
  cr_send(s, (ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_ESCAPE});
  cr_frames(s, 20);
  cr_snapshot(s, &after);
  if (!cr_check(s, !ui->emoji_picker_open, "Escape closes the picker")) return;
  if (!cr_check(s, cr_flips(&before, &after) == 0, "dismissing the picker sends no reaction")) return;
  printf("ctx-react: Escape dismisses the picker without a reaction\n");

  // --- picking an emoji toggles that reaction on the right-clicked message -------------------------------------------
  if (!cr_check(s, cr_open_menu(s, s->msg) && cr_click_row(s, "Add Reaction"), "menu reopens on the message")) return;
  cr_frames(s, 3);
  if (!cr_check(s, ui->emoji_picker_open, "picker reopens from the menu")) return;
  const ccord_rect cell = ui->emoji_picker_cells[0];
  if (!cr_check(s, cell.w > 0.0f && cell.h > 0.0f, "picker draws an emoji cell")) return;
  cr_snapshot(s, &before);
  cr_click(s, cell.x + cell.w * 0.5f, cell.y + cell.h * 0.5f, 1);
  cr_frames(s, 20);
  if (!cr_check(s, !ui->emoji_picker_open, "picking an emoji closes the picker")) return;
  cr_snapshot(s, &after);
  if (!cr_check(s, cr_flips(&before, &after) == 1, "picking an emoji toggles exactly one reaction on the message")) {
    return;
  }
  printf("ctx-react: picking an emoji toggles its reaction\n");

  // --- an unconfirmed message has no Add Reaction row -------------------------------------------------------------
  ccord_id guild = ccord_store_selected_guild(s->c->store);
  size_t n = ccord_store_message_count(s->c->store, s->ch);
  const ccord_message *last = n ? ccord_store_message_at(s->c->store, s->ch, n - 1) : nullptr;
  ccord_message pending = {.id = last ? last->id + 1 : 1, .channel_id = s->ch,
                           .author_id = ccord_store_self_id(s->c->store), .timestamp_ms = last ? last->timestamp_ms + 1 : 1,
                           .content = "ctx react pending", .send_state = CCORD_MSG_SEND_PENDING};
  ccord_error err;
  if (!cr_check(s, ccord_store_append_message(s->c->store, &pending, &err) == CCORD_OK, "pending echo appended")) return;
  ccord_menu_context pctx = {.target = CCORD_MENU_MESSAGE, .guild_id = guild, .channel_id = s->ch,
                             .message_id = pending.id, .user_id = pending.author_id};
  ui_menu_open(ui, 40.0f, 60.0f, &pctx);
  if (!cr_check(s, cr_row(ui, "Copy Text") >= 0, "pending message menu opens")) return;
  if (!cr_check(s, cr_row(ui, "Add Reaction") < 0, "unconfirmed message menu has no Add Reaction row")) return;
  ui_menu_close(ui);
  cr_frames(s, 3);
  (void)cr_reaction_layout(s);
}

int ccord_verify_ctx_react_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  cr s = {.c = ctx, .ch = CCORD_ID_NONE, .msg = CCORD_ID_NONE};
  cr_frames(&s, 10);
  cr_run(&s);
  if (s.fail[0]) {
    printf("ctx-react: FAIL %s\n", s.fail);
    fflush(stdout);
    return 1;
  }
  printf("ctx-react: PASS checks=%d\n", s.checks);
  fflush(stdout);
  return 0;
}
