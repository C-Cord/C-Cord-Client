// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Search results-panel verification; see verify_ui_parity.h.
//
// Drives the real UI over the offline mock transport: the search button and the query come in as input events, the
// query goes out as a transport search request (the mock answers it), and the results panel is clicked at the card
// rectangles the draw pass recorded. Nothing here re-implements the panel; it reads what the UI drew and did.
#include "verify_ui_parity.h"

#include <stdio.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/model/search.h>
#include <ccord/net/mock_transport.h>

#include "ui_internal.h"

#define SV_WAIT_FRAMES 400

typedef struct sv {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
  float win_w, win_h;
} sv;

static bool sv_check(sv *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

static void sv_frames(sv *s, int n) { s->c->frames(s->c->ud, n); }

static void sv_send(sv *s, ccord_input_event ev) { s->c->input(s->c->ud, &ev); }

static void sv_move(sv *s, float x, float y)
{
  sv_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
}

static void sv_click(sv *s, float x, float y)
{
  sv_move(s, x, y);
  sv_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  sv_send(s, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
  sv_frames(s, 2);
}

static void sv_click_rect(sv *s, ccord_rect r) { sv_click(s, r.x + r.w * 0.5f, r.y + r.h * 0.5f); }

static void sv_key(sv *s, ccord_key k, uint8_t mods)
{
  sv_send(s, (ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = k, .mods = mods});
}

static void sv_type(sv *s, const char *text)
{
  ccord_input_event ev = {.type = CCORD_IN_TEXT};
  snprintf(ev.text, sizeof ev.text, "%s", text);
  sv_send(s, ev);
}

static ccord_ui_debug sv_dbg(const sv *s)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(s->c->ui, &d);
  return d;
}

// Real resize event; the layout follows on the next frame.
static void sv_resize(sv *s, float w, float h)
{
  sv_send(s, (ccord_input_event){.type = CCORD_IN_RESIZE, .x = w, .y = h});
  sv_frames(s, 3);
}

// Opens search (header button) and types `query`; returns once the session has answered the request for it.
static bool sv_search(sv *s, const char *query, bool already_open)
{
  ccord_ui_debug d = sv_dbg(s);
  if (!already_open) {
    sv_click_rect(s, d.search_btn);
    if (!sv_dbg(s).search_open) return false;
  } else {
    sv_key(s, CCORD_KEY_A, CCORD_MOD_CTRL);
    sv_key(s, CCORD_KEY_BACKSPACE, 0);
  }
  size_t before = ccord_mock_call_count(s->c->transport, CCORD_MOCK_CALL_SEARCH);
  sv_type(s, query);
  for (int i = 0; i < SV_WAIT_FRAMES; i++) {
    sv_frames(s, 1);
    if (ccord_mock_call_count(s->c->transport, CCORD_MOCK_CALL_SEARCH) > before &&
        !ccord_session_search_loading(s->c->session)) {
      sv_frames(s, 2);
      return true;
    }
  }
  return false;
}

static void sv_close_search(sv *s)
{
  if (sv_dbg(s).search_open) sv_key(s, CCORD_KEY_ESCAPE, 0);
  sv_frames(s, 3);
}

// Number of result cards the draw pass must have recorded: those that intersect the panel body.
static size_t sv_expected_cards(const sv *s, size_t results)
{
  const ccord_layout *l = ccord_ui_get_layout(s->c->ui);
  uint32_t total = ccord_session_search_total_results(s->c->session);
  uint32_t off = ccord_session_search_offset(s->c->session);
  bool pager = total > CCORD_SEARCH_PAGE_SIZE || off > 0u;
  float body_h = l->member_list.h - 37.0f - (pager ? 38.0f : 0.0f);
  size_t n = 0;
  for (size_t i = 0; i < results; i++) {
    float top = 8.0f + (float)i * (62.0f + 6.0f);
    if (top <= body_h && top + 62.0f >= 0.0f) n++;
  }
  return n;
}

// The panel shows one card per result (in order) and the header count equals the session's total.
static bool sv_panel_matches_results(sv *s, const char *name)
{
  ccord_ui_debug d = sv_dbg(s);
  size_t sc = 0;
  const ccord_message *sr = ccord_session_search_results(s->c->session, &sc);
  if (!sv_check(s, d.search_panel_open, name)) return false;
  if (!sv_check(s, d.search_total_results == ccord_session_search_total_results(s->c->session) &&
                     d.search_total_results >= sc, name)) return false;
  size_t want = sv_expected_cards(s, sc);
  if (!sv_check(s, s->c->ui->nsearch_card_hits == want, name)) return false;
  for (size_t i = 0; i < s->c->ui->nsearch_card_hits; i++) {
    if (!sv_check(s, s->c->ui->search_card_hits[i].message_id == sr[i].id, name)) return false;
  }
  return true;
}

static int sv_finish(sv *s)
{
  if (s->fail[0]) {
    printf("search-view: FAIL %s\n", s->fail);
    fflush(stdout);
    return 1;
  }
  printf("search-view: PASS checks=%d\n", s->checks);
  fflush(stdout);
  return 0;
}

// Picks a query that matches a message of the selected channel and also matches in another channel of the guild.
static bool sv_pick_cross_channel_query(sv *s, char *out, size_t cap)
{
  ccord_store *st = s->c->store;
  ccord_id ch = ccord_store_selected_channel(st);
  size_t n = ccord_store_message_count(st, ch);
  for (size_t i = n; i > 0; i--) {
    const ccord_message *m = ccord_store_message_at(st, ch, i - 1);
    if (!m || !m->content) continue;
    const char *p = m->content;
    while (*p) {
      while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) p++;
      const char *w = p;
      while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) p++;
      size_t wl = (size_t)(p - w);
      if (wl < 4 || wl >= cap) continue;
      memcpy(out, w, wl);
      out[wl] = '\0';
      bool okq = sv_search(s, out, sv_dbg(s).search_open);
      if (!okq) return false;
      size_t sc = 0;
      const ccord_message *sr = ccord_session_search_results(s->c->session, &sc);
      for (size_t k = 0; k < sc; k++) {
        if (sr[k].channel_id != CCORD_ID_NONE && sr[k].channel_id != ch) return true;
      }
    }
  }
  return false;
}

// Scrolls the panel until a card satisfying want_other (a different channel) is on screen; returns its index.
static int sv_find_card(sv *s, ccord_id not_channel)
{
  ccord_ui *ui = s->c->ui;
  const ccord_layout *l = ccord_ui_get_layout(ui);
  for (int tries = 0; tries < 40; tries++) {
    for (size_t i = 0; i < ui->nsearch_card_hits; i++) {
      if (ui->search_card_hits[i].channel_id != not_channel) return (int)i;
    }
    sv_send(s, (ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -3.0f, .x = l->member_list.x + 40.0f,
                                   .y = l->member_list.y + 200.0f});
    sv_frames(s, 2);
  }
  return -1;
}

int ccord_verify_search_view_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  sv s = {.c = ctx};
  ccord_ui *ui = ctx->ui;
  ccord_store *st = ctx->store;
  ccord_error err;

  sv_frames(&s, 10);
  const ccord_layout *l = ccord_ui_get_layout(ui);
  s.win_w = l->chat_header.x + l->chat_header.w;
  s.win_h = l->user_panel.y + l->user_panel.h;
  ccord_id g0 = ccord_store_selected_guild(st);
  // Start in the guild's last plain text channel with history: the mock answers guild-wide searches in channel order,
  // so page 1 then holds hits from earlier channels too.
  for (size_t i = ccord_store_channel_count(st, g0); i > 0; i--) {
    const ccord_channel *c = ccord_store_channel_at(st, g0, i - 1);
    if (!c || c->kind != CCORD_CHANNEL_TEXT || c->discord_type != 0) continue;
    if (ccord_session_select_channel(ctx->session, c->id, &err) != CCORD_OK) continue;
    sv_frames(&s, 20);
    if (ccord_store_message_count(st, c->id) > 0) break;
  }
  ccord_id ch0 = ccord_store_selected_channel(st);
  if (!sv_check(&s, l->member_list_visible && g0 != CCORD_ID_NONE && g0 != CCORD_DM_GUILD_ID && ch0 != CCORD_ID_NONE,
                "mock starts in a guild channel with the member list shown")) return sv_finish(&s);

  // --- 1: results panel with one card per result, timeline untouched -------------------------------------------
  ccord_ui_debug pre = sv_dbg(&s);
  size_t store_count = ccord_store_message_count(st, ch0);
  if (!sv_check(&s, pre.message_count == store_count && store_count > 0, "timeline starts with the full channel")) {
    return sv_finish(&s);
  }
  char query[32];
  if (!sv_check(&s, sv_pick_cross_channel_query(&s, query, sizeof query),
                "a query typed into search is answered with results from another channel")) return sv_finish(&s);
  ccord_ui_debug d = sv_dbg(&s);
  if (!sv_check(&s, d.search_open && ccord_mock_call_count(ctx->transport, CCORD_MOCK_CALL_SEARCH) > 0,
                "search goes out as a transport request")) return sv_finish(&s);
  if (!sv_panel_matches_results(&s, "results panel: one card per result, header count = total")) return sv_finish(&s);
  if (!sv_check(&s, d.message_count == pre.message_count && ui->view_count == store_count &&
                      d.first_visible == pre.first_visible && d.last_visible == pre.last_visible &&
                      d.rows_drawn == pre.rows_drawn,
                "main timeline message count and visible rows are unchanged during search")) return sv_finish(&s);

  // --- 2: click a result in another channel -------------------------------------------------------------------
  int ci = sv_find_card(&s, ch0);
  if (!sv_check(&s, ci >= 0, "a card of another channel can be scrolled into view")) return sv_finish(&s);
  ccord_id target_ch = ui->search_card_hits[ci].channel_id;
  ccord_id target_msg = ui->search_card_hits[ci].message_id;
  const ccord_channel *tch = ccord_store_channel(st, target_ch);
  sv_click_rect(&s, ui->search_card_hits[ci].rect);   // the highlight flashes for ~1.4 s of virtual time: read it now
  d = sv_dbg(&s);
  if (!sv_check(&s, !d.search_open && !d.search_panel_open, "clicking a result closes search")) return sv_finish(&s);
  if (!sv_check(&s, ccord_store_selected_channel(st) == target_ch &&
                      (!tch || ccord_store_selected_guild(st) == tch->guild_id),
                "clicking a result in another channel selects that channel")) return sv_finish(&s);
  if (!sv_check(&s, d.jump_highlight_message == target_msg && ccord_store_find_message(st, target_ch, target_msg),
                "the clicked message is anchored and highlighted")) return sv_finish(&s);
  l = ccord_ui_get_layout(ui);
  if (!sv_check(&s, l->member_list_visible && !ui_search_panel_active(ui),
                "the member list returns when search closes")) return sv_finish(&s);

  // --- 3: click a result in the current channel ---------------------------------------------------------------
  if (!sv_check(&s, sv_search(&s, query, false), "search can be opened again")) return sv_finish(&s);
  ccord_id cur = ccord_store_selected_channel(st);
  int same = -1;
  for (int tries = 0; tries < 40 && same < 0; tries++) {
    for (size_t i = 0; i < ui->nsearch_card_hits; i++) {
      if (ui->search_card_hits[i].channel_id == cur) { same = (int)i; break; }
    }
    if (same < 0) {
      l = ccord_ui_get_layout(ui);
      sv_send(&s, (ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -3.0f, .x = l->member_list.x + 40.0f,
                                      .y = l->member_list.y + 200.0f});
      sv_frames(&s, 2);
    }
  }
  if (!sv_check(&s, same >= 0, "a card of the selected channel can be scrolled into view")) return sv_finish(&s);
  ccord_id same_msg = ui->search_card_hits[same].message_id;
  sv_click_rect(&s, ui->search_card_hits[same].rect);
  sv_frames(&s, 3);
  d = sv_dbg(&s);
  size_t anchor_raw = SIZE_MAX;
  for (size_t i = 0; i < ccord_store_message_count(st, cur); i++) {
    const ccord_message *m = ccord_store_message_at(st, cur, i);
    if (m && m->id == same_msg) { anchor_raw = i; break; }
  }
  if (!sv_check(&s, !d.search_open && ccord_store_selected_channel(st) == cur && d.jump_highlight_message == same_msg &&
                      anchor_raw != SIZE_MAX && anchor_raw >= d.first_visible && anchor_raw <= d.last_visible,
                "clicking a result in the current channel scrolls it into view and highlights it")) return sv_finish(&s);

  // --- 4: empty result set --------------------------------------------------------------------------------------
  if (!sv_check(&s, sv_search(&s, "zzqxjvw", false), "an unmatched query is answered")) return sv_finish(&s);
  d = sv_dbg(&s);
  if (!sv_check(&s, d.search_panel_open && d.search_total_results == 0 && d.search_panel_cards == 0 &&
                      !ccord_session_search_loading(ctx->session),
                "an empty result set shows the panel with no cards (empty state)")) return sv_finish(&s);
  sv_close_search(&s);

  // --- 5: pager ---------------------------------------------------------------------------------------------------
  static const char *const wide[] = {"the", "an", "er", "in", "e", "a "};
  bool paged = false;
  for (size_t i = 0; i < sizeof wide / sizeof *wide && !paged; i++) {
    if (!sv_search(&s, wide[i], sv_dbg(&s).search_open)) continue;
    paged = ccord_session_search_total_results(ctx->session) > CCORD_SEARCH_PAGE_SIZE;
  }
  if (!sv_check(&s, paged, "a query with more than one page of results exists in the mock")) return sv_finish(&s);
  d = sv_dbg(&s);
  if (!sv_check(&s, d.search_panel_open && d.search_offset == 0 && d.search_next_btn.w > 0.0f && d.search_prev_btn.w == 0.0f,
                "page 1 offers Next and no Prev")) return sv_finish(&s);
  size_t calls = ccord_mock_call_count(ctx->transport, CCORD_MOCK_CALL_SEARCH);
  ccord_id first_p1 = ui->search_card_hits[0].message_id;
  sv_click_rect(&s, d.search_next_btn);
  for (int i = 0; i < SV_WAIT_FRAMES && ccord_session_search_offset(ctx->session) == 0u; i++) sv_frames(&s, 1);
  sv_frames(&s, 3);
  d = sv_dbg(&s);
  if (!sv_check(&s, d.search_offset == CCORD_SEARCH_PAGE_SIZE && d.search_prev_btn.w > 0.0f &&
                      ccord_mock_call_count(ctx->transport, CCORD_MOCK_CALL_SEARCH) == calls + 1 &&
                      ui->nsearch_card_hits > 0 && ui->search_card_hits[0].message_id != first_p1,
                "Next moves to page 2 through a new transport request")) return sv_finish(&s);
  sv_click_rect(&s, d.search_prev_btn);
  for (int i = 0; i < SV_WAIT_FRAMES && ccord_session_search_offset(ctx->session) != 0u; i++) sv_frames(&s, 1);
  sv_frames(&s, 3);
  d = sv_dbg(&s);
  if (!sv_check(&s, d.search_offset == 0 && ui->nsearch_card_hits > 0 && ui->search_card_hits[0].message_id == first_p1,
                "Prev returns to page 1")) return sv_finish(&s);
  sv_close_search(&s);

  // --- 6: member list hidden by the user --------------------------------------------------------------------------
  d = sv_dbg(&s);
  sv_click_rect(&s, d.members_toggle);
  l = ccord_ui_get_layout(ui);
  if (!sv_check(&s, !l->member_list_visible, "the member list can be hidden with its header button")) return sv_finish(&s);
  size_t count_hidden_pre = sv_dbg(&s).message_count;
  if (!sv_check(&s, sv_search(&s, query, false), "search answers with the member list hidden")) return sv_finish(&s);
  d = sv_dbg(&s);
  if (!sv_check(&s, ui->nsearch_card_hits > 0 && sv_panel_matches_results(&s, "member list hidden: results panel shown"),
                "results panel shows with the member list hidden")) return sv_finish(&s);
  if (!sv_check(&s, d.message_count == count_hidden_pre, "member list hidden: timeline stays unfiltered")) {
    return sv_finish(&s);
  }
  sv_close_search(&s);
  l = ccord_ui_get_layout(ui);
  if (!sv_check(&s, !l->member_list_visible, "closing search restores the hidden member list state")) return sv_finish(&s);
  sv_click_rect(&s, sv_dbg(&s).members_toggle);
  l = ccord_ui_get_layout(ui);
  if (!sv_check(&s, l->member_list_visible, "the member list can be shown again")) return sv_finish(&s);

  // --- 7: narrow window (member list normally off) ----------------------------------------------------------------
  sv_resize(&s, 800.0f, s.win_h);
  l = ccord_ui_get_layout(ui);
  if (!sv_check(&s, !l->member_list_visible, "an 800px window has no member list")) return sv_finish(&s);
  if (!sv_check(&s, sv_search(&s, query, false), "search answers in a narrow window")) return sv_finish(&s);
  l = ccord_ui_get_layout(ui);
  if (!sv_check(&s, sv_dbg(&s).search_panel_open && l->member_list.w > 0.0f &&
                      l->message_area.x + l->message_area.w <= l->member_list.x + 0.5f &&
                      sv_panel_matches_results(&s, "narrow window: results panel shown"),
                "results panel shows in a narrow window beside the timeline")) return sv_finish(&s);
  sv_close_search(&s);
  l = ccord_ui_get_layout(ui);
  if (!sv_check(&s, !l->member_list_visible, "closing search in a narrow window drops the panel")) return sv_finish(&s);
  sv_resize(&s, s.win_w, s.win_h);

  // --- 8: in a DM -------------------------------------------------------------------------------------------------
  ccord_id peer = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_user_count(st) && peer == CCORD_ID_NONE; i++) {
    const ccord_user *u = ccord_store_user_at(st, i);
    if (u && u->id != ccord_store_self_id(st)) peer = u->id;
  }
  if (!sv_check(&s, peer != CCORD_ID_NONE && ccord_session_open_dm(ctx->session, peer, &err) == CCORD_OK,
                "a DM can be opened")) return sv_finish(&s);
  const ccord_channel *dmc = nullptr;
  for (int i = 0; i < SV_WAIT_FRAMES && !dmc; i++) {
    sv_frames(&s, 1);
    if (ccord_store_channel_count(st, CCORD_DM_GUILD_ID) > 0) dmc = ccord_store_channel_at(st, CCORD_DM_GUILD_ID, 0);
  }
  if (!sv_check(&s, dmc != nullptr, "the mock creates the DM channel")) return sv_finish(&s);
  ccord_id dm_id = dmc->id;
  if (!sv_check(&s, ccord_session_select_guild(ctx->session, CCORD_DM_GUILD_ID, &err) == CCORD_OK &&
                      ccord_session_select_channel(ctx->session, dm_id, &err) == CCORD_OK,
                "a DM can be selected")) return sv_finish(&s);
  sv_frames(&s, 10);
  if (!sv_check(&s, ccord_store_selected_guild(st) == CCORD_DM_GUILD_ID && ccord_store_selected_channel(st) == dm_id,
                "the DM is selected")) return sv_finish(&s);
  // Give the DM some history through the composer, like a user would.
  sv_type(&s, "rainbow unicorn lunch");
  sv_key(&s, CCORD_KEY_RETURN, 0);
  for (int i = 0; i < SV_WAIT_FRAMES && ccord_store_message_count(st, dm_id) == 0; i++) sv_frames(&s, 1);
  sv_frames(&s, 5);
  if (!sv_check(&s, ccord_store_message_count(st, dm_id) == 1, "a message sent in the DM lands in its timeline")) {
    return sv_finish(&s);
  }
  const char *dmq = "unicorn";
  size_t dm_count = sv_dbg(&s).message_count;
  if (!sv_check(&s, sv_search(&s, dmq, false), "search answers in a DM")) return sv_finish(&s);
  if (!sv_check(&s, ccord_session_search_total_results(ctx->session) == 1, "the DM search finds the DM message")) return sv_finish(&s);
  l = ccord_ui_get_layout(ui);
  if (!sv_check(&s, sv_dbg(&s).search_panel_open && l->member_list_visible &&
                      sv_panel_matches_results(&s, "DM: results panel shown") &&
                      sv_dbg(&s).message_count == dm_count,
                "results panel shows in a DM and the DM timeline stays unfiltered")) return sv_finish(&s);
  sv_close_search(&s);
  if (!sv_check(&s, !ui_search_panel_active(ui) && ccord_store_selected_guild(st) == CCORD_DM_GUILD_ID,
                "closing search in a DM leaves the DM open without the panel")) return sv_finish(&s);

  return sv_finish(&s);
}
