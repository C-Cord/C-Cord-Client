// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice report-flow: Report Message / Report User end to end over the offline mock transport (
// section 6). Drives the real UI: right-click menus, the report overlay's own rows and buttons (rects come from its last draw),
// keys and typed text. The mock serves fixed report menus, validates every submit against the menu it served and records the last
// accepted request, so the slice compares the body Discord would get with what the user chose. Prints
// `report-flow: PASS checks=N` (exit 0) or `report-flow: FAIL <first failed check>`.
#include "verify_ui_parity.h"

#include <stdio.h>
#include <string.h>

#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/error.h>
#include <ccord/core/json.h>
#include <ccord/net/mock_transport.h>

#include "ui_internal.h"

#define RS_MOCK_HQ 2001u       // the viewer administers it (moderation rows)
#define RS_MOCK_GARDEN 2003u   // the viewer is an ordinary member (no moderation permission)
#define RS_MOCK_ADA 1002u
#define RS_MOCK_CHEN 1004u

typedef struct rs {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
  ccord_id guild, ch, self;
  ccord_id msg_other, msg_own, msg_pending, msg_eph;   // messages of the selected channel
  ccord_id other;                                      // author of msg_other
  ccord_id dm_user;                                    // a user whose DM-style menu is opened
} rs;

static bool rs_check(rs *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

static void rs_frames(rs *s, int n) { s->c->frames(s->c->ud, n); }
static void rs_send(rs *s, ccord_input_event ev) { s->c->input(s->c->ud, &ev); }

// A left click on the middle of `r`; `settle` runs frames afterwards (0: nothing is pumped, so a request stays in flight).
static void rs_click(rs *s, ccord_rect r, bool settle)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  rs_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
  rs_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  rs_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
  if (settle) rs_frames(s, 3);
}

static void rs_key(rs *s, ccord_key key, uint8_t mods, bool settle)
{
  rs_send(s, (ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = key, .mods = mods});
  if (settle) rs_frames(s, 2);
}

// Types ASCII `text` as TEXT events of at most 24 bytes each.
static void rs_type(rs *s, const char *text)
{
  size_t n = strlen(text);
  for (size_t i = 0; i < n; i += 24) {
    ccord_input_event ev = {.type = CCORD_IN_TEXT};
    size_t k = n - i < 24 ? n - i : 24;
    memcpy(ev.text, text + i, k);
    rs_send(s, ev);
  }
  rs_frames(s, 2);
}

static ui_report_flow_debug rs_dbg(const rs *s)
{
  ui_report_flow_debug d;
  ui_report_flow_debug_get(s->c->ui, &d);
  return d;
}

// Runs frames (one at a time, so the mock's answers are applied between them) until the flow shows `phase` and, for phase 1 and 3,
// `node` (0 = any); then two more frames so its rects are drawn. False after a timeout.
static bool rs_wait(rs *s, int phase, uint32_t node)
{
  for (int i = 0; i < 80; i++) {
    ui_report_flow_debug d = rs_dbg(s);
    if (d.open && d.phase == phase && (node == 0 || d.node_id == node)) {
      rs_frames(s, 2);
      return true;
    }
    rs_frames(s, 1);
  }
  return false;
}

// ---- menus ---------------------------------------------------------------------------------------------------------

static int rs_row(const ccord_ui *ui, const char *label)
{
  for (size_t i = 0; i < ui->menu.count; i++) {
    const menu_item *it = &ui->menu.items[i];
    if (it->parent < 0 && it->label && strcmp(it->label, label) == 0) return (int)i;
  }
  return -1;
}

static void rs_menu(rs *s, ccord_menu_target target, ccord_id guild, ccord_id channel, ccord_id message, ccord_id user)
{
  ui_menu_close(s->c->ui);
  ccord_menu_context ctx = {.target = target, .guild_id = guild, .channel_id = channel, .message_id = message, .user_id = user};
  ui_menu_open(s->c->ui, 200.0f, 120.0f, &ctx);
  rs_frames(s, 2);
}

static void rs_message_menu(rs *s, ccord_id msg)
{
  const ccord_message *m = ccord_store_find_message(s->c->store, s->ch, msg);
  rs_menu(s, CCORD_MENU_MESSAGE, s->guild, s->ch, msg, m ? m->author_id : CCORD_ID_NONE);
}

// Clicks a row of the open menu; `settle` as in rs_click. False when the menu has no such row.
static bool rs_menu_click(rs *s, const char *label, bool settle)
{
  ccord_rect r;
  if (!ccord_ui_context_menu_find(s->c->ui, label, &r)) return false;
  rs_click(s, r, settle);
  return true;
}

// Right-click menu on the probe message, then Report Message. With settle false nothing runs afterwards (the menu is still loading).
static bool rs_open_message_flow(rs *s, bool settle)
{
  rs_message_menu(s, s->msg_other);
  return rs_menu_click(s, "Report Message", settle);
}

static bool rs_open_user_flow(rs *s, bool settle)
{
  rs_menu(s, CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, CCORD_ID_NONE, CCORD_ID_NONE, s->dm_user);
  return rs_menu_click(s, "Report User", settle);
}

static void rs_close_flow(rs *s)
{
  if (ui_report_flow_is_open(s->c->ui)) rs_key(s, CCORD_KEY_ESCAPE, 0, true);
}

// ---- setup ---------------------------------------------------------------------------------------------------------

static ccord_id rs_append(rs *s, ccord_id author, const char *text, uint8_t send_state, bool ephemeral)
{
  size_t n = ccord_store_message_count(s->c->store, s->ch);
  const ccord_message *last = n ? ccord_store_message_at(s->c->store, s->ch, n - 1) : nullptr;
  ccord_message_rich rich = {.flags = CCORD_MSGFLAG_EPHEMERAL};
  ccord_message m = {.id = last ? last->id + 1 : 1, .channel_id = s->ch, .author_id = author,
                     .timestamp_ms = last ? last->timestamp_ms + 1 : 1, .content = text, .send_state = send_state,
                     .rich = ephemeral ? &rich : nullptr};
  ccord_error err;
  return ccord_store_append_message(s->c->store, &m, &err) == CCORD_OK ? m.id : CCORD_ID_NONE;
}

// Selects a text channel with history; takes the newest confirmed message by someone else as the probe and adds an own, a
// pending and an ephemeral message after it.
static bool rs_setup(rs *s)
{
  const ccord_verify_ui_parity_ctx *c = s->c;
  s->self = ccord_store_self_id(c->store);
  s->guild = ccord_store_selected_guild(c->store);
  ccord_error err;
  for (size_t i = ccord_store_channel_count(c->store, s->guild); i > 0; i--) {
    const ccord_channel *ch = ccord_store_channel_at(c->store, s->guild, i - 1);
    if (!ch || ch->kind != CCORD_CHANNEL_TEXT || ch->discord_type != 0) continue;
    if (ccord_session_select_channel(c->session, ch->id, &err) != CCORD_OK) continue;
    rs_frames(s, 20);
    if (ccord_store_message_count(c->store, ch->id) > 0) break;
  }
  s->ch = ccord_store_selected_channel(c->store);
  for (size_t i = ccord_store_message_count(c->store, s->ch); i > 0 && s->msg_other == CCORD_ID_NONE; i--) {
    const ccord_message *m = ccord_store_message_at(c->store, s->ch, i - 1);
    if (m && m->send_state == CCORD_MSG_SEND_CONFIRMED && m->author_id != s->self && m->author_id != CCORD_ID_NONE) {
      s->msg_other = m->id;
      s->other = m->author_id;
    }
  }
  if (s->msg_other == CCORD_ID_NONE) return false;
  s->msg_own = rs_append(s, s->self, "report slice own message", CCORD_MSG_SEND_CONFIRMED, false);
  s->msg_pending = rs_append(s, s->self, "report slice pending message", CCORD_MSG_SEND_PENDING, false);
  s->msg_eph = rs_append(s, s->other, "report slice ephemeral message", CCORD_MSG_SEND_CONFIRMED, true);
  s->dm_user = s->other;   // the DM menu needs only the user: the mock's DM list is not loaded until the DM view is opened
  return s->msg_own && s->msg_pending && s->msg_eph;
}

// ---- helpers over the recorded request -----------------------------------------------------------------------------

static const ccord_report_value *rs_value(const ccord_report_request *r, const char *name)
{
  for (size_t i = 0; i < r->element_count; i++) {
    if (r->elements[i].name && strcmp(r->elements[i].name, name) == 0) return &r->elements[i];
  }
  return nullptr;
}

static bool rs_value_is(const ccord_report_request *r, const char *name, const char *want)
{
  const ccord_report_value *v = rs_value(r, name);
  return v && v->value_count == 1 && v->values[0] && strcmp(v->values[0], want) == 0;
}

static bool rs_path_is(const uint32_t *got, size_t n, const uint32_t *want, size_t m)
{
  if (n != m) return false;
  for (size_t i = 0; i < n; i++) {
    if (got[i] != want[i]) return false;
  }
  return true;
}

// ---- the 16 groups -------------------------------------------------------------------------------------------------

// 1: Report Message is on someone else's confirmed message, and nowhere else.
static bool rs_g1_message_menu(rs *s)
{
  ccord_ui *ui = s->c->ui;
  rs_message_menu(s, s->msg_other);
  if (!rs_check(s, ui->menu.open && rs_row(ui, "Report Message") >= 0, "someone else's message has Report Message")) return false;
  int rep = rs_row(ui, "Report Message");
  if (!rs_check(s, rs_row(ui, "Copy Text") >= 0 && rs_row(ui, "Reply") >= 0 && rs_row(ui, "Copy Text") < rs_row(ui, "Reply") &&
                     rs_row(ui, "Reply") < rep,
                "the other message rows keep their order and Report Message comes after them"))
    return false;
  rs_message_menu(s, s->msg_own);
  if (!rs_check(s, ui->menu.open && rs_row(ui, "Copy Text") >= 0 && rs_row(ui, "Report Message") < 0,
                "your own message has no Report Message"))
    return false;
  rs_message_menu(s, s->msg_pending);
  if (!rs_check(s, ui->menu.open && rs_row(ui, "Copy Text") >= 0 && rs_row(ui, "Report Message") < 0,
                "an unconfirmed message has no Report Message"))
    return false;
  rs_message_menu(s, s->msg_eph);
  if (!rs_check(s, ui->menu.open && rs_row(ui, "Dismiss Message") >= 0 && rs_row(ui, "Report Message") < 0,
                "an ephemeral message has no Report Message"))
    return false;
  ui_menu_close(ui);
  return true;
}

// 2: Report User in DM menus, and in guild menus with and without moderation permission.
static bool rs_g2_user_menus(rs *s)
{
  ccord_ui *ui = s->c->ui;
  ccord_store *st = s->c->store;
  rs_menu(s, CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, CCORD_ID_NONE, CCORD_ID_NONE, s->dm_user);
  if (!rs_check(s, ui->menu.open && rs_row(ui, "Block User") >= 0 && rs_row(ui, "Report User") > rs_row(ui, "Block User"),
                "a DM member's menu has Report User after Block User"))
    return false;
  rs_menu(s, CCORD_MENU_MEMBER, CCORD_DM_GUILD_ID, CCORD_ID_NONE, CCORD_ID_NONE, s->self);
  if (!rs_check(s, ui->menu.open && rs_row(ui, "Report User") < 0, "your own DM entry has no Report User")) return false;

  if (!rs_check(s, ccord_store_guild(st, RS_MOCK_GARDEN) && ccord_store_can_act_on(st, CCORD_PERM_ACT_KICK, RS_MOCK_GARDEN, s->self,
                                                                                RS_MOCK_CHEN) == CCORD_PERM_MISSING,
                "the viewer has no moderation permission in the ordinary server"))
    return false;
  rs_menu(s, CCORD_MENU_MEMBER, RS_MOCK_GARDEN, CCORD_ID_NONE, CCORD_ID_NONE, RS_MOCK_CHEN);
  if (!rs_check(s, ui->menu.open && rs_row(ui, "Report User") >= 0 && rs_row(ui, "Kick Member") < 0 && rs_row(ui, "Ban Member") < 0,
                "an ordinary member's menu opens with Report User"))
    return false;
  if (!rs_check(s, ui->menu.items[0].sep_before == false, "the lone Report User row has no separator above it")) return false;

  rs_menu(s, CCORD_MENU_MEMBER, RS_MOCK_HQ, CCORD_ID_NONE, CCORD_ID_NONE, RS_MOCK_ADA);
  if (!rs_check(s, ui->menu.open && rs_row(ui, "Kick Member") >= 0 && rs_row(ui, "Ban Member") >= 0 &&
                     rs_row(ui, "Report User") > rs_row(ui, "Ban Member"),
                "a moderator's menu has Report User after the moderation rows"))
    return false;
  rs_menu(s, CCORD_MENU_MEMBER, RS_MOCK_HQ, CCORD_ID_NONE, CCORD_ID_NONE, s->self);
  if (!rs_check(s, ui->menu.open && rs_row(ui, "Report User") < 0, "your own guild entry has no Report User")) return false;
  ui_menu_close(ui);
  return true;
}

// 3: a stale session does not open the flow and sends nothing.
static bool rs_g3_stale(rs *s)
{
  ccord_ui *ui = s->c->ui;
  size_t before = ccord_mock_report_menu_count(s->c->transport);
  ccord_session_set_stale(s->c->session, true);
  bool clicked = rs_open_message_flow(s, true);
  bool open = ui_report_flow_is_open(ui);
  bool notice = strstr(ui->notice, "Reconnecting") != nullptr;
  size_t after = ccord_mock_report_menu_count(s->c->transport);
  ccord_session_set_stale(s->c->session, false);
  rs_frames(s, 2);
  return rs_check(s, clicked, "Report Message can be clicked while stale") && rs_check(s, !open, "a stale session does not open the flow") &&
         rs_check(s, notice, "the stale refusal says Reconnecting") && rs_check(s, after == before, "no menu request reached the transport while stale");
}

// 4..10: the full message report.
static bool rs_g4_open(rs *s)
{
  size_t before = ccord_mock_report_menu_count(s->c->transport);
  if (!rs_check(s, rs_open_message_flow(s, false), "Report Message can be clicked")) return false;
  ui_report_flow_debug d = rs_dbg(s);
  if (!rs_check(s, d.open && d.phase == 0, "the flow opens loading")) return false;
  if (!rs_check(s, !s->c->ui->menu.open, "the menu closes once Report Message is activated")) return false;
  if (!rs_check(s, rs_wait(s, 1, 1), "the menu arrives and the root node shows")) return false;
  d = rs_dbg(s);
  return rs_check(s, strcmp(d.header, "What's wrong with this message?") == 0 && d.child_count == 3, "the root node is the server's first question") &&
         rs_check(s, d.children[0].w > 0.0f && d.path_len == 1 && d.path[0] == 1, "the root node's rows are drawn and the path starts at the root") &&
         rs_check(s, ccord_mock_report_menu_count(s->c->transport) == before + 1, "one menu request was made");
}

static bool rs_g5_checkboxes(rs *s)
{
  ui_report_flow_debug d = rs_dbg(s);
  rs_click(s, d.children[0], true);   // It's spam
  if (!rs_check(s, rs_wait(s, 1, 2), "It's spam leads to node 2")) return false;
  d = rs_dbg(s);
  if (!rs_check(s, d.checkbox_count == 2 && d.checked_count == 0 && !d.button_enabled && strcmp(d.button_label, "Next") == 0,
                "Next is disabled until a box is ticked"))
    return false;
  rs_click(s, d.checks[0], true);
  d = rs_dbg(s);
  if (!rs_check(s, d.checked_count == 1 && d.button_enabled, "ticking Phishing enables Next")) return false;
  rs_click(s, d.checks[1], true);
  d = rs_dbg(s);
  if (!rs_check(s, d.checked_count == 2, "a second box can be ticked")) return false;
  rs_click(s, d.checks[1], true);
  d = rs_dbg(s);
  return rs_check(s, d.checked_count == 1 && d.button_enabled, "unticking it leaves one ticked");
}

static bool rs_g6_path(rs *s)
{
  ui_report_flow_debug d = rs_dbg(s);
  rs_click(s, d.button, true);
  if (!rs_check(s, rs_wait(s, 1, 4), "Next leads to node 4")) return false;
  d = rs_dbg(s);
  static const uint32_t want[] = {1, 2, 4};
  if (!rs_check(s, rs_path_is(d.path, d.path_len, want, 3), "the path is [1, 2, 4]")) return false;
  if (!rs_check(s, strstr(d.crumb, "It's spam") != nullptr, "the breadcrumbs element shows the row clicked")) return false;
  if (!rs_check(s, d.preview[0] != '\0', "the message preview card is shown")) return false;
  if (!rs_check(s, strcmp(d.button_label, "Submit") == 0 && d.button_enabled && d.back.w > 0.0f, "node 4 has Back and an enabled Submit")) return false;
  rs_click(s, d.back, true);
  d = rs_dbg(s);
  if (!rs_check(s, d.node_id == 2 && d.path_len == 2 && d.checked_count == 1, "Back returns to node 2 with the box still ticked")) return false;
  rs_click(s, d.button, true);
  if (!rs_check(s, rs_wait(s, 1, 4), "Next again reaches node 4")) return false;
  d = rs_dbg(s);
  return rs_check(s, rs_path_is(d.path, d.path_len, want, 3), "the path is [1, 2, 4] again");
}

static bool rs_g7_text(rs *s)
{
  ui_report_flow_debug d = rs_dbg(s);
  if (!rs_check(s, d.text_rect.w > 0.0f, "node 4 draws a free-text field")) return false;
  rs_click(s, d.text_rect, true);
  rs_type(s, "buy now");
  d = rs_dbg(s);
  if (!rs_check(s, strcmp(d.text, "buy now") == 0, "typed text lands in the field")) return false;
  char junk[260];
  memset(junk, 'x', 250);
  junk[250] = '\0';
  rs_type(s, junk);
  d = rs_dbg(s);
  if (!rs_check(s, strlen(d.text) == 200, "typing past the 200 character limit is cut at 200")) return false;
  rs_key(s, CCORD_KEY_A, CCORD_MOD_CTRL, true);
  rs_key(s, CCORD_KEY_BACKSPACE, 0, true);
  rs_type(s, "buy now");
  d = rs_dbg(s);
  return rs_check(s, strcmp(d.text, "buy now") == 0, "the field can be cleared and retyped");
}

static bool rs_g8_refusal(rs *s)
{
  ccord_ui *ui = s->c->ui;
  const char *reason = "You need to verify your email";
  size_t submits = ccord_mock_report_submit_count(s->c->transport);
  ccord_mock_set_late_report_failure(s->c->transport, reason);
  ui_report_flow_debug d = rs_dbg(s);
  rs_click(s, d.button, false);
  if (!rs_check(s, rs_dbg(s).phase == 2 && !rs_dbg(s).button_enabled, "Submit puts the flow in the submitting phase")) return false;
  if (!rs_check(s, rs_wait(s, 1, 4), "a refused report returns to the node it was sent from")) return false;
  d = rs_dbg(s);
  static const uint32_t want[] = {1, 2, 4};
  return rs_check(s, strstr(d.error, reason) != nullptr, "the overlay shows the mock's reason") &&
         rs_check(s, strstr(ui->notice, "Couldn't send report: ") != nullptr && strstr(ui->notice, reason) != nullptr,
                  "a notice says Couldn't send report with the reason") &&
         rs_check(s, rs_path_is(d.path, d.path_len, want, 3) && strcmp(d.text, "buy now") == 0 && d.button_enabled,
                  "the path and the typed text are still there and Submit is enabled again") &&
         rs_check(s, ccord_mock_report_submit_count(s->c->transport) == submits, "a refused report recorded no submit");
}

static bool rs_g9_submit(rs *s)
{
  ccord_ui *ui = s->c->ui;
  ccord_transport *t = s->c->transport;
  ccord_mock_set_late_report_failure(t, nullptr);
  size_t submits = ccord_mock_report_submit_count(t);
  ui_report_flow_debug d = rs_dbg(s);
  rs_click(s, d.button, true);
  if (!rs_check(s, rs_wait(s, 3, 90), "the accepted report shows the success node")) return false;
  if (!rs_check(s, ccord_mock_report_submit_count(t) == submits + 1, "exactly one submit was recorded")) return false;
  if (!rs_check(s, strstr(ui->notice, "Report sent") != nullptr, "a notice says the report was sent")) return false;
  const ccord_report_request *r = ccord_mock_last_report(t);
  if (!rs_check(s, r != nullptr, "the mock recorded the request")) return false;
  static const uint32_t want[] = {1, 2, 4};
  return rs_check(s, r->type == CCORD_REPORT_MESSAGE && r->channel_id == s->ch && r->message_id == s->msg_other && r->reported_user_id == 0,
                  "the body names the message report's channel and message only") &&
         rs_check(s, rs_path_is(r->breadcrumbs, r->breadcrumb_count, want, 3), "the breadcrumbs are exactly [1, 2, 4]") &&
         rs_check(s, r->element_count == 2 && rs_value_is(r, "spam_kind", "phishing") && rs_value_is(r, "details", "buy now"),
                  "the elements are exactly spam_kind=[phishing] and details=[buy now]") &&
         rs_check(s, r->version && strcmp(r->version, "1.0") == 0 && r->variant && strcmp(r->variant, "3") == 0 &&
                       r->language && strcmp(r->language, "en") == 0,
                  "version, variant and language are echoed from the menu");
}

static bool rs_g10_block_done(rs *s)
{
  ccord_store *st = s->c->store;
  ui_report_flow_debug d = rs_dbg(s);
  if (!rs_check(s, strcmp(d.header, "Thanks for reporting") == 0 && strncmp(d.button_label, "Done", 4) == 0, "the success node says Thanks for reporting with Done")) return false;
  if (!rs_check(s, d.block.w > 0.0f && strncmp(d.block_label, "Block ", 6) == 0, "a Block <name> row is offered")) return false;
  const ccord_user *u = ccord_store_user(st, s->other);
  if (!rs_check(s, u && u->name && strstr(d.block_label, u->name) != nullptr, "the Block row names the message's author")) return false;
  rs_click(s, d.block, true);
  rs_frames(s, 20);
  if (!rs_check(s, ccord_store_relationship(st, s->other) == CCORD_RELATIONSHIP_BLOCKED, "clicking Block blocks the author")) return false;
  // Put the mock's relationship back so the later groups see an ordinary author.
  ccord_error err;
  (void)ccord_store_set_relationship(st, s->other, CCORD_RELATIONSHIP_NONE, &err);
  rs_frames(s, 3);
  d = rs_dbg(s);
  rs_click(s, d.button, true);
  return rs_check(s, !ui_report_flow_is_open(s->c->ui) && ui_report_flow_live_states() == 0, "Done closes the flow and frees its state");
}

// 11: the exact JSON that goes to POST /reporting/message.
static bool rs_g11_body(rs *s)
{
  const ccord_report_request *r = ccord_mock_last_report(s->c->transport);
  if (!rs_check(s, r != nullptr, "there is a recorded request to build")) return false;
  ccord_buf body;
  ccord_buf_init(&body, 0);
  ccord_error err;
  bool built = ccord_report_build_body(r, &body, &err) == CCORD_OK;
  ccord_arena a;
  ccord_arena_init(&a, 0);
  const char *text = built ? ccord_buf_cstr(&body) : "";
  const ccord_json *j = built ? ccord_json_parse(&a, text, strlen(text), nullptr, &err) : nullptr;
  char want_ch[32], want_msg[32];
  snprintf(want_ch, sizeof want_ch, "%llu", (unsigned long long)s->ch);
  snprintf(want_msg, sizeof want_msg, "%llu", (unsigned long long)s->msg_other);
  size_t l1 = 0, l2 = 0, l3 = 0;
  const char *ch = j ? ccord_json_string(ccord_json_get(j, "channel_id"), &l1) : nullptr;
  const char *msg = j ? ccord_json_string(ccord_json_get(j, "message_id"), &l2) : nullptr;
  const char *name = j ? ccord_json_string(ccord_json_get(j, "name"), &l3) : nullptr;
  const ccord_json *crumbs = j ? ccord_json_get(j, "breadcrumbs") : nullptr;
  const ccord_json *els = j ? ccord_json_get(j, "elements") : nullptr;
  size_t ls = 0;
  const char *spam = els ? ccord_json_string(ccord_json_array_at(ccord_json_get(els, "spam_kind"), 0), &ls) : nullptr;
  bool ok = j && ch && msg && name && strcmp(ch, want_ch) == 0 && strcmp(msg, want_msg) == 0 && strcmp(name, "message") == 0 &&
            crumbs && ccord_json_array_len(crumbs) == 3 && spam && strcmp(spam, "phishing") == 0 && !ccord_json_get(j, "guild_id");
  ccord_arena_free(&a);
  ccord_buf_free(&body);
  return rs_check(s, ok, "the body parses; ids are decimal strings; breadcrumbs, elements and name are as chosen");
}

// 12: the menu cannot be loaded: Discord's reason, Retry, then the menu.
static bool rs_g12_menu_failure(rs *s)
{
  ccord_ui *ui = s->c->ui;
  ccord_transport *t = s->c->transport;
  ccord_mock_set_report_menu_failure(t, "Unknown Report");
  if (!rs_check(s, rs_open_message_flow(s, true), "Report Message can be clicked")) return false;
  if (!rs_check(s, rs_wait(s, 4, 0), "a menu refusal shows the load-failed phase")) return false;
  ui_report_flow_debug d = rs_dbg(s);
  if (!rs_check(s, strstr(d.error, "Unknown Report") != nullptr && strstr(ui->notice, "Unknown Report") != nullptr,
                "Discord's reason is in the overlay and in a notice"))
    return false;
  if (!rs_check(s, d.retry.w > 0.0f && d.close.w > 0.0f, "Retry and Close are offered")) return false;
  ccord_mock_set_report_menu_failure(t, nullptr);
  rs_click(s, d.retry, true);
  if (!rs_check(s, rs_wait(s, 1, 1), "Retry loads the menu")) return false;
  rs_close_flow(s);
  return rs_check(s, !ui_report_flow_is_open(ui), "the flow closes");
}

// 13: a link to a node the menu does not have.
static bool rs_g13_broken_link(rs *s)
{
  ccord_ui *ui = s->c->ui;
  ccord_transport *t = s->c->transport;
  ccord_mock_set_report_broken_link(t, true);
  if (!rs_check(s, rs_open_message_flow(s, true) && rs_wait(s, 1, 1), "the broken menu loads")) return false;
  ui_report_flow_debug d = rs_dbg(s);
  if (!rs_check(s, d.child_count == 4, "the root node has the extra Broken link row")) return false;
  rs_click(s, d.children[3], true);
  d = rs_dbg(s);
  bool ok = rs_check(s, d.open && d.node_id == 1 && d.path_len == 1, "following the broken link stays on the root node") &&
            rs_check(s, strstr(d.error, "out of date") != nullptr, "the overlay says the form is out of date");
  ccord_mock_set_report_broken_link(t, false);
  rs_close_flow(s);
  return ok && rs_check(s, !ui_report_flow_is_open(ui), "the flow closes");
}

// 14: Report User from the DM menu.
static bool rs_g14_user(rs *s)
{
  ccord_transport *t = s->c->transport;
  size_t submits = ccord_mock_report_submit_count(t);
  if (!rs_check(s, rs_open_user_flow(s, true), "Report User can be clicked in the DM menu")) return false;
  if (!rs_check(s, rs_wait(s, 1, 10), "the user menu's root node shows")) return false;
  ui_report_flow_debug d = rs_dbg(s);
  if (!rs_check(s, d.child_count == 2 && strstr(d.header, "user") != nullptr, "the root node is the user question")) return false;
  rs_click(s, d.children[0], true);
  if (!rs_check(s, rs_wait(s, 1, 11), "Impersonation leads to the summary")) return false;
  d = rs_dbg(s);
  const ccord_user *u = ccord_store_user(s->c->store, s->dm_user);
  if (!rs_check(s, u && u->name && strcmp(d.preview, u->name) == 0, "the user preview names the reported user")) return false;
  rs_click(s, d.button, true);
  if (!rs_check(s, rs_wait(s, 3, 90), "the accepted user report shows the success node")) return false;
  const ccord_report_request *r = ccord_mock_last_report(t);
  static const uint32_t want[] = {10, 11};
  if (!rs_check(s, ccord_mock_report_submit_count(t) == submits + 1 && r != nullptr, "one more submit was recorded")) return false;
  if (!rs_check(s, r->type == CCORD_REPORT_USER && r->reported_user_id == s->dm_user && r->channel_id == 0 && r->message_id == 0,
                "the user report names the user and no channel or message"))
    return false;
  if (!rs_check(s, rs_path_is(r->breadcrumbs, r->breadcrumb_count, want, 2) && r->element_count == 0,
                "the breadcrumbs are [10, 11] and an empty free text is not sent"))
    return false;
  d = rs_dbg(s);
  if (!rs_check(s, d.block.w > 0.0f, "the success node offers Block for the reported user")) return false;
  rs_click(s, d.button, true);
  return rs_check(s, !ui_report_flow_is_open(s->c->ui), "Done closes the flow");
}

// 15: cancelling at any step sends nothing and frees the state.
static bool rs_g15_cancel(rs *s)
{
  ccord_ui *ui = s->c->ui;
  ccord_transport *t = s->c->transport;
  size_t submits = ccord_mock_report_submit_count(t);
  // loading
  if (!rs_check(s, rs_open_message_flow(s, false) && rs_dbg(s).phase == 0, "the flow is loading")) return false;
  rs_key(s, CCORD_KEY_ESCAPE, 0, false);
  if (!rs_check(s, !ui_report_flow_is_open(ui) && ui_report_flow_live_states() == 0, "Escape while loading closes and frees")) return false;
  rs_frames(s, 10);   // the menu arrives for a flow that is gone
  if (!rs_check(s, !ui_report_flow_is_open(ui) && ui_report_flow_live_states() == 0, "a late menu does not reopen the flow")) return false;
  // a node, via the Close button
  if (!rs_check(s, rs_open_message_flow(s, true) && rs_wait(s, 1, 1), "a reopened flow starts at the root")) return false;
  ui_report_flow_debug d = rs_dbg(s);
  if (!rs_check(s, d.path_len == 1 && d.path[0] == 1 && d.checked_count == 0, "a reopened flow has an empty path and no answers")) return false;
  rs_click(s, d.children[0], true);
  if (!rs_check(s, rs_wait(s, 1, 2), "node 2 shows")) return false;
  d = rs_dbg(s);
  rs_click(s, d.checks[0], true);
  d = rs_dbg(s);
  rs_click(s, d.close, true);
  if (!rs_check(s, !ui_report_flow_is_open(ui) && ui_report_flow_live_states() == 0, "Close on a node closes and frees")) return false;
  // a node, via Escape
  if (!rs_check(s, rs_open_message_flow(s, true) && rs_wait(s, 1, 1), "the flow opens again at the root")) return false;
  d = rs_dbg(s);
  if (!rs_check(s, d.path_len == 1 && d.checked_count == 0, "earlier answers did not leak into the new flow")) return false;
  rs_key(s, CCORD_KEY_ESCAPE, 0, true);
  if (!rs_check(s, !ui_report_flow_is_open(ui) && ui_report_flow_live_states() == 0, "Escape on a node closes and frees")) return false;
  // submitting: the report is on its way and fails later, with nothing recorded
  ccord_mock_set_late_report_failure(t, "Too many reports");
  if (!rs_check(s, rs_open_message_flow(s, true) && rs_wait(s, 1, 1), "the flow opens for the submitting check")) return false;
  d = rs_dbg(s);
  rs_click(s, d.children[2], true);   // Something else -> node 5
  if (!rs_check(s, rs_wait(s, 1, 5), "node 5 shows")) return false;
  d = rs_dbg(s);
  rs_click(s, d.button, false);
  if (!rs_check(s, rs_dbg(s).phase == 2, "Submit is in flight")) return false;
  rs_key(s, CCORD_KEY_ESCAPE, 0, false);
  if (!rs_check(s, !ui_report_flow_is_open(ui) && ui_report_flow_live_states() == 0, "Escape while submitting closes and frees")) return false;
  rs_frames(s, 10);
  ccord_mock_set_late_report_failure(t, nullptr);
  return rs_check(s, strstr(ui->notice, "Too many reports") != nullptr && !ui_report_flow_is_open(ui),
                  "the late refusal of a closed flow is only a notice") &&
         rs_check(s, ccord_mock_report_submit_count(t) == submits, "cancelling at every step recorded no submit");
}

// 16: the session itself refuses your own message and yourself.
static bool rs_g16_session_guard(rs *s)
{
  ccord_transport *t = s->c->transport;
  size_t submits = ccord_mock_report_submit_count(t);
  static const uint32_t path[] = {1, 5};
  ccord_report_request own = {.type = CCORD_REPORT_MESSAGE, .version = "1.0", .variant = "3", .language = "en", .breadcrumbs = path,
                              .breadcrumb_count = 2, .channel_id = s->ch, .message_id = s->msg_own};
  ccord_error err;
  bool own_refused = ccord_session_report_submit(s->c->session, &own, &err) == CCORD_ERR_INVALID;
  static const uint32_t upath[] = {10, 11};
  ccord_report_request me = {.type = CCORD_REPORT_USER, .version = "1.0", .variant = "3", .language = "en", .breadcrumbs = upath,
                             .breadcrumb_count = 2, .reported_user_id = s->self};
  bool self_refused = ccord_session_report_submit(s->c->session, &me, &err) == CCORD_ERR_INVALID;
  ccord_report_request none = me;
  none.reported_user_id = CCORD_ID_NONE;
  bool none_refused = ccord_session_report_submit(s->c->session, &none, &err) == CCORD_ERR_INVALID;
  rs_frames(s, 5);
  return rs_check(s, own_refused, "the session refuses a report of your own message") &&
         rs_check(s, self_refused && none_refused, "the session refuses a report of yourself or of no one") &&
         rs_check(s, ccord_mock_report_submit_count(t) == submits, "the transport saw none of them");
}

static bool rs_run(rs *s)
{
  rs_frames(s, 10);
  if (!rs_check(s, rs_setup(s), "the mock has a channel with someone else's message")) return false;
  rs_frames(s, 4);
  return rs_g1_message_menu(s) && rs_g2_user_menus(s) && rs_g3_stale(s) && rs_g4_open(s) && rs_g5_checkboxes(s) && rs_g6_path(s) &&
         rs_g7_text(s) && rs_g8_refusal(s) && rs_g9_submit(s) && rs_g10_block_done(s) && rs_g11_body(s) && rs_g12_menu_failure(s) &&
         rs_g13_broken_link(s) && rs_g14_user(s) && rs_g15_cancel(s) && rs_g16_session_guard(s);
}

int ccord_verify_report_flow_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  rs s = {.c = ctx};
  bool ok = rs_run(&s);
  if (ui_report_flow_is_open(ctx->ui)) ui_report_flow_close(ctx->ui);
  if (!ok || s.fail[0]) {
    printf("report-flow: FAIL %s\n", s.fail[0] ? s.fail : "unknown");
    fflush(stdout);
    return 1;
  }
  printf("report-flow: PASS checks=%d\n", s.checks);
  fflush(stdout);
  return 0;
}
