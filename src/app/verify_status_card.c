// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Status card verification (--verify-slice status-card); see verify_ui_parity.h.
//
// A real click on the user panel opens the account card. The slice reads only what the real layout and draw produced
// (ui->layout.user_panel, ui->sc.card and the hit-testable items the last draw recorded) and drives the card with real
// input events: it checks the card hangs above the user panel (not screen-centred), a presence row applies at once
// with no further click, a custom status is set with Set and confirmed with a checkmark, a refused request restores
// the marker, there is no Save or Cancel, and Escape / an outside click close it.
#include "verify_ui_parity.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ccord/net/mock_transport.h>

#include "ui_internal.h"

typedef struct sc_ctx {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
} sc_ctx;

static int sc_fail(const char *name, const char *detail)
{
  printf("status-card: FAIL %s%s%s\n", name, detail && detail[0] ? ": " : "", detail ? detail : "");
  fflush(stdout);
  return 1;
}

#define SC_CHECK(m, name, cond, detail)                \
  do {                                                 \
    if (!(cond)) return sc_fail((name), (detail));     \
    (m)->checks++;                                     \
  } while (0)

static void sc_click(sc_ctx *m, ccord_rect r)
{
  float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
  ccord_input_event mv = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  m->c->input(m->c->ud, &mv);
  ccord_input_event dn = {.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  m->c->input(m->c->ud, &dn);
  ccord_input_event up = {.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y};
  m->c->input(m->c->ud, &up);
  m->c->frames(m->c->ud, 3);
}

static void sc_key(sc_ctx *m, ccord_key key)
{
  ccord_input_event dn = {.type = CCORD_IN_KEY_DOWN, .key = key};
  m->c->input(m->c->ud, &dn);
  m->c->frames(m->c->ud, 2);
}

static void sc_type(sc_ctx *m, const char *s)
{
  ccord_input_event ev = {.type = CCORD_IN_TEXT};
  snprintf(ev.text, sizeof ev.text, "%s", s);
  m->c->input(m->c->ud, &ev);
  m->c->frames(m->c->ud, 2);
}

static const ui_sc_item *sc_find(const sc_ctx *m, ui_sc_kind kind, int arg)
{
  const ui_status_card *sc = &m->c->ui->sc;
  for (size_t i = 0; i < sc->nitems; i++) {
    if (sc->items[i].kind == kind && (arg < 0 || sc->items[i].arg == arg)) return &sc->items[i];
  }
  return nullptr;
}

static bool sc_has_word(const char *s, const char *word)
{
  size_t n = strlen(word);
  for (const char *p = s; *p; p++) {
    size_t i = 0;
    while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)word[i])) i++;
    if (i == n) return true;
  }
  return false;
}

static ccord_presence sc_self_presence(const sc_ctx *m)
{
  const ccord_user *u = ccord_store_user(m->c->store, ccord_store_self_id(m->c->store));
  return u ? u->presence : CCORD_PRESENCE_OFFLINE;
}

int ccord_verify_status_card_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  sc_ctx mm = {.c = ctx};
  sc_ctx *m = &mm;
  ccord_ui *ui = ctx->ui;
  if (!ui || !ctx->store || !ctx->session) return sc_fail("setup", "no ui, store or session");
  ctx->frames(ctx->ud, 6);
  SC_CHECK(m, "setup", ccord_store_self_id(ctx->store) != CCORD_ID_NONE, "no self user");
  SC_CHECK(m, "closed at start", !ui_status_card_is_open(ui), "card open before any click");

  // 1. A click on the user panel opens the card anchored above it.
  sc_click(m, ui_user_status_area_rect(ui));
  SC_CHECK(m, "open", ui_status_card_is_open(ui), "click on the user panel did not open the card");
  SC_CHECK(m, "no modal", ui->modal_kind == UI_MODAL_NONE, "the old status modal opened");
  ccord_rect card = ui->sc.card, panel = ui_rect(ui->layout.user_panel);
  SC_CHECK(m, "card drawn", card.w > 0.0f && card.h > 0.0f, "card has no rect");
  SC_CHECK(m, "above panel", card.y + card.h <= panel.y + 0.5f, "card is not above the user panel");
  SC_CHECK(m, "overlaps panel x", card.x < panel.x + panel.w && card.x + card.w > panel.x,
           "card does not overlap the user panel's x-range");
  SC_CHECK(m, "left aligned", card.x >= panel.x && card.x < panel.x + panel.w * 0.5f, "card is not left-aligned with the panel");
  float ccx = card.x + card.w * 0.5f, ccy = card.y + card.h * 0.5f;
  SC_CHECK(m, "not centred", !(fabsf(ccx - ui->win_w * 0.5f) < 8.0f && fabsf(ccy - ui->win_h * 0.5f) < 8.0f),
           "card is screen-centred");
  SC_CHECK(m, "no overlay scrim", ui->modal_kind == UI_MODAL_NONE && !ui->settings_open, "a modal opened");
  printf("status-card: anchored above user panel\n");

  // 2. No Save / Cancel anywhere; Edit Profile and the four presence rows exist.
  for (size_t i = 0; i < ui->sc.nitems; i++) {
    const ui_sc_item *it = &ui->sc.items[i];
    SC_CHECK(m, "no Save/Cancel", !sc_has_word(it->label, "save") && !sc_has_word(it->label, "cancel"), it->label);
  }
  SC_CHECK(m, "edit profile row", sc_find(m, UI_SC_EDIT_PROFILE, -1) != nullptr, "no Edit Profile row");
  for (int i = 0; i < 4; i++) SC_CHECK(m, "presence rows", sc_find(m, UI_SC_PRESENCE, i) != nullptr, "missing presence row");
  SC_CHECK(m, "set button", sc_find(m, UI_SC_SET, -1) != nullptr, "no Set button");
  SC_CHECK(m, "emoji button", sc_find(m, UI_SC_EMOJI_BUTTON, -1) != nullptr, "no emoji button");
  SC_CHECK(m, "clear-after", sc_find(m, UI_SC_DURATION, -1) != nullptr, "no clear-after choice");

  // 3. Idle applies on the one click: the request goes out and the store shows it, with the card still open.
  SC_CHECK(m, "starts online", sc_self_presence(m) == CCORD_PRESENCE_ONLINE, "self is not online at the start");
  size_t admin_before = ccord_mock_admin_count(ctx->transport);
  const ui_sc_item *idle = sc_find(m, UI_SC_PRESENCE, 1);
  SC_CHECK(m, "idle row", idle != nullptr, "no Idle row");
  sc_click(m, idle->rect);
  SC_CHECK(m, "idle store", sc_self_presence(m) == CCORD_PRESENCE_IDLE, "store does not show idle after the click");
  SC_CHECK(m, "idle sent", ccord_mock_admin_count(ctx->transport) > admin_before, "no presence request was sent");
  const ccord_admin_request *last = ccord_mock_last_admin(ctx->transport);
  SC_CHECK(m, "idle request", last && last->op == CCORD_ADMIN_USER_SETTINGS_PATCH && strstr(last->body, "idle"),
           "the request is not a status patch for idle");
  SC_CHECK(m, "stays open", ui_status_card_is_open(ui), "card closed after choosing a status");
  printf("status-card: presence applied without save\n");

  // 4. Type a custom status and press Set: it is sent and the checkmark replaces the button.
  sc_type(m, "Heads down");
  SC_CHECK(m, "typed", strcmp(ccord_text_edit_cstr(&ui->sc.text), "Heads down") == 0, "text did not reach the field");
  const ui_sc_item *emo = sc_find(m, UI_SC_EMOJI_BUTTON, -1);
  SC_CHECK(m, "emoji btn", emo != nullptr, "no emoji button");
  sc_click(m, emo->rect);
  SC_CHECK(m, "picker opened", ui->emoji_picker_open && ui->emoji_picker_for_status && ui_status_card_is_open(ui),
           "the emoji button did not open the emoji picker with the card open");
  SC_CHECK(m, "no preset row", sc_find(m, UI_SC_EMOJI_PICK, -1) == nullptr, "preset emoji row still offered");
  // Dismissing the picker (Escape) changes nothing and leaves the card open.
  sc_key(m, CCORD_KEY_ESCAPE);
  SC_CHECK(m, "dismiss closes picker", !ui->emoji_picker_open, "Escape did not close the picker");
  SC_CHECK(m, "dismiss keeps card", ui_status_card_is_open(ui), "dismissing the picker closed the card");
  SC_CHECK(m, "dismiss no emoji", ui->sc.emoji[0] == '\0', "dismissing the picker set an emoji");
  SC_CHECK(m, "dismiss keeps text", strcmp(ccord_text_edit_cstr(&ui->sc.text), "Heads down") == 0, "text changed");
  emo = sc_find(m, UI_SC_EMOJI_BUTTON, -1);
  SC_CHECK(m, "emoji btn again", emo != nullptr, "emoji button gone");
  sc_click(m, emo->rect);
  ctx->frames(ctx->ud, 4);
  SC_CHECK(m, "picker reopened", ui->emoji_picker_open && ui->emoji_picker_for_status, "picker did not reopen");
  ccord_rect cell = {0};
  for (size_t i = 0; i < UI_EMOJI_PICKER_COUNT; i++) {
    if (ui->emoji_picker_cells[i].w > 0.0f && ui->emoji_picker_cells[i].h > 0.0f) {
      cell = ui->emoji_picker_cells[i];
      break;
    }
  }
  SC_CHECK(m, "picker cell", cell.w > 0.0f, "the picker drew no emoji cell");
  sc_click(m, cell);
  SC_CHECK(m, "picker closes on pick", !ui->emoji_picker_open, "picker stayed open after a pick");
  SC_CHECK(m, "card open after pick", ui_status_card_is_open(ui), "card closed after a pick");
  SC_CHECK(m, "emoji chosen", ui->sc.emoji[0] != '\0', "no emoji chosen");
  char chosen[sizeof ui->sc.emoji];
  snprintf(chosen, sizeof chosen, "%s", ui->sc.emoji);
  admin_before = ccord_mock_admin_count(ctx->transport);
  const ui_sc_item *set = sc_find(m, UI_SC_SET, -1);
  SC_CHECK(m, "set present", set != nullptr, "Set button missing");
  ccord_rect set_rect = set->rect;
  sc_click(m, set_rect);
  last = ccord_mock_last_admin(ctx->transport);
  SC_CHECK(m, "set sent", ccord_mock_admin_count(ctx->transport) > admin_before && last &&
                              last->op == CCORD_ADMIN_SET_CUSTOM_STATUS && strstr(last->body, "Heads down"),
           "Set did not send the custom status");
  SC_CHECK(m, "set carries emoji", strstr(last->body, chosen) != nullptr || strstr(last->custom_status_emoji, chosen),
           "the custom status request does not carry the picked emoji");
  SC_CHECK(m, "confirm active", ui_status_card_confirming(ui), "no confirmation after Set");
  SC_CHECK(m, "confirm replaces Set", sc_find(m, UI_SC_SET, -1) == nullptr, "Set button still shown while confirming");
  const ccord_user *me = ccord_store_user(ctx->store, ccord_store_self_id(ctx->store));
  SC_CHECK(m, "status in store", me && strcmp(me->custom_status_text, "Heads down") == 0, "store has no custom status");
  printf("status-card: custom status set confirmed\n");
  ctx->frames(ctx->ud, 20);   // 2 s of virtual time: the confirmation ends and Set returns
  SC_CHECK(m, "confirm ends", !ui_status_card_confirming(ui) && sc_find(m, UI_SC_SET, -1) != nullptr,
           "confirmation did not end");

  // 5. A refused presence request shows an inline error and restores the previous marker.
  ccord_mock_fail_next_admin(ctx->transport, true);
  const ui_sc_item *dnd = sc_find(m, UI_SC_PRESENCE, 2);
  SC_CHECK(m, "dnd row", dnd != nullptr, "no Do Not Disturb row");
  sc_click(m, dnd->rect);
  ctx->frames(ctx->ud, 6);
  SC_CHECK(m, "refusal restores", sc_self_presence(m) == CCORD_PRESENCE_IDLE, "marker was not restored after a refusal");
  SC_CHECK(m, "inline error", ui->sc.error[0] != '\0', "no inline error after a refusal");

  // 6. The clear (x) removes the custom status at once.
  const ui_sc_item *clr = sc_find(m, UI_SC_CLEAR, -1);
  SC_CHECK(m, "clear present", clr != nullptr, "no clear control while a custom status is set");
  sc_click(m, clr->rect);
  me = ccord_store_user(ctx->store, ccord_store_self_id(ctx->store));
  SC_CHECK(m, "cleared", me && me->custom_status_text[0] == '\0', "custom status still set after clear");
  SC_CHECK(m, "clear field", ccord_text_edit_cstr(&ui->sc.text)[0] == '\0', "field not emptied");

  // 6b. Invisible applies on one click: store and request both say invisible.
  const ui_sc_item *inv = sc_find(m, UI_SC_PRESENCE, 3);
  SC_CHECK(m, "invisible row", inv != nullptr, "no Invisible row");
  admin_before = ccord_mock_admin_count(ctx->transport);
  sc_click(m, inv->rect);
  SC_CHECK(m, "invisible store", sc_self_presence(m) == CCORD_PRESENCE_OFFLINE, "store is not invisible");
  last = ccord_mock_last_admin(ctx->transport);
  SC_CHECK(m, "invisible sent", ccord_mock_admin_count(ctx->transport) > admin_before && last &&
                                    last->op == CCORD_ADMIN_USER_SETTINGS_PATCH && strstr(last->body, "invisible"),
           "the request does not carry invisible");

  // 7. Escape closes; so does a click outside.
  sc_key(m, CCORD_KEY_ESCAPE);
  SC_CHECK(m, "escape closes", !ui_status_card_is_open(ui), "Escape did not close the card");
  sc_click(m, ui_user_status_area_rect(ui));
  SC_CHECK(m, "reopen", ui_status_card_is_open(ui), "card did not reopen");
  sc_click(m, (ccord_rect){ui->win_w * 0.6f, 40.0f, 2.0f, 2.0f});
  SC_CHECK(m, "outside click closes", !ui_status_card_is_open(ui), "a click outside did not close the card");

  printf("status-card: PASS checks=%d\n", m->checks);
  fflush(stdout);
  return 0;
}
