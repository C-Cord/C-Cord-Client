// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Saved Messages (bookmarks) popover + Forward channel picker (Result 1 UI).
//
// Bookmarks are local: the UI list (ui->saved_msgs) is the display + persistence source of truth. It is
// written to settings.ini ([saved_messages]) and rehydrated into the store so the session/store bookmark
// APIs agree. The popover is a header panel like Inbox; the Forward picker borrows the generic modal
// routing (ui->modal_kind = UI_MODAL_FORWARD_PICKER) and intercepts ui_draw_overlays / ui_overlays_handle
// before the legacy modal code, like the scheduled-events modal. Every Discord change is a session intent.
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>

#include "ui_internal.h"
#include "ui_style.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

static float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

// Case-insensitive substring (needle length nlen); empty needle matches everything.
static bool ci_contains(const char *hay, const char *needle, size_t nlen)
{
  if (!hay) return false;
  if (nlen == 0) return true;
  for (const char *p = hay; *p; p++) {
    size_t i = 0;
    while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
    if (i == nlen) return true;
  }
  return false;
}

// Single-line, delimiter-safe copy of text into out (replaces control chars and '|' with spaces).
static void sanitize_line(const char *src, char *out, size_t cap)
{
  if (cap == 0) return;
  size_t w = 0;
  if (src) {
    for (size_t i = 0; src[i] && w + 1 < cap; i++) {
      unsigned char c = (unsigned char)src[i];
      if (c == '\n' || c == '\r' || c == '\t' || c == '|') c = ' ';
      out[w++] = (char)c;
    }
  }
  // Trim trailing spaces, then cut on a UTF-8 boundary so the INI line stays valid text.
  while (w > 0 && out[w - 1] == ' ') w--;
  while (w > 0 && ((unsigned char)out[w - 1] & 0xC0u) == 0x80u) w--;   // drop a split continuation tail
  if (w > 0 && ((unsigned char)out[w - 1] & 0x80u) != 0) {
    // If the last byte is a lead byte whose sequence was cut, drop it too.
    unsigned char last = (unsigned char)out[w - 1];
    size_t need = (last >= 0xF0u) ? 4u : (last >= 0xE0u) ? 3u : (last >= 0xC0u) ? 2u : 1u;
    size_t have = 1;
    while (have < need && w >= have + 1 && ((unsigned char)out[w - 1 - have] & 0xC0u) == 0x80u) have++;
    if (have < need) w -= have;
  }
  out[w] = '\0';
}

// ---------------------------------------------------------------- Saved Messages list

bool ui_saved_messages_contains(const ccord_ui *ui, ccord_id channel_id, ccord_id message_id)
{
  if (!ui) return false;
  for (size_t i = 0; i < ui->nsaved_msgs; i++) {
    if (ui->saved_msgs[i].channel_id == channel_id && ui->saved_msgs[i].message_id == message_id) return true;
  }
  return false;
}

static long saved_find(const ccord_ui *ui, ccord_id channel_id, ccord_id message_id)
{
  for (size_t i = 0; i < ui->nsaved_msgs; i++) {
    if (ui->saved_msgs[i].channel_id == channel_id && ui->saved_msgs[i].message_id == message_id) return (long)i;
  }
  return -1;
}

// Fill a UI bookmark from a stored message, capturing the author label and a sanitized preview.
static void saved_fill(ccord_ui *ui, const ccord_message *m, ui_saved_msg *out)
{
  memset(out, 0, sizeof *out);
  out->channel_id = m->channel_id;
  out->message_id = m->id;
  out->author_id = m->author_id;
  out->ts_ms = m->timestamp_ms;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, m->channel_id);
  out->guild_id = ch ? ch->guild_id : CCORD_ID_NONE;
  const ccord_user *u = ccord_store_user(ui->d.store, m->author_id);
  const char *name = (u && u->name && u->name[0]) ? u->name : "Unknown User";
  sanitize_line(name, out->label, sizeof out->label);
  sanitize_line(m->content, out->preview, sizeof out->preview);
}

// Insert / refresh at the front (newest first), matching the store's ordering.
static void saved_list_upsert(ccord_ui *ui, const ui_saved_msg *e)
{
  long at = saved_find(ui, e->channel_id, e->message_id);
  if (at == 0) {
    ui->saved_msgs[0] = *e;
    return;
  }
  if (at > 0) {
    memmove(&ui->saved_msgs[at], &ui->saved_msgs[at + 1], (ui->nsaved_msgs - (size_t)at - 1u) * sizeof(ui->saved_msgs[0]));
    ui->nsaved_msgs--;
  }
  if (ui->nsaved_msgs >= UI_SAVED_MSG_MAX) ui->nsaved_msgs = UI_SAVED_MSG_MAX - 1u;   // oldest makes room
  memmove(&ui->saved_msgs[1], &ui->saved_msgs[0], ui->nsaved_msgs * sizeof(ui->saved_msgs[0]));
  ui->saved_msgs[0] = *e;
  ui->nsaved_msgs++;
}

void ui_saved_messages_save(ccord_ui *ui, ccord_id channel_id, ccord_id message_id)
{
  if (!ui) return;
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_error err;
  if (ccord_session_save_message(ui->d.session, channel_id, message_id, &err) != CCORD_OK) {
    ui_notice(ui, "Cannot save message: %s", err.message);
    return;
  }
  const ccord_message *m = ccord_store_saved_message_at(ui->d.store, 0);   // just moved to the front
  if (m) {
    ui_saved_msg e;
    saved_fill(ui, m, &e);
    saved_list_upsert(ui, &e);
  }
  ui_settings_save(ui);
  ui_notice(ui, "Message saved");
  ui->dirty = true;
}

void ui_saved_messages_unsave(ccord_ui *ui, ccord_id channel_id, ccord_id message_id)
{
  if (!ui) return;
  if (ui->d.session) {
    ccord_error err;
    (void)ccord_session_unsave_message(ui->d.session, channel_id, message_id, &err);
  }
  long at = saved_find(ui, channel_id, message_id);
  if (at >= 0) {
    memmove(&ui->saved_msgs[at], &ui->saved_msgs[at + 1], (ui->nsaved_msgs - (size_t)at - 1u) * sizeof(ui->saved_msgs[0]));
    ui->nsaved_msgs--;
  }
  ui_settings_save(ui);
  ui_notice(ui, "Removed from Saved Messages");
  ui->dirty = true;
}

void ui_saved_messages_toggle_popover(ccord_ui *ui)
{
  if (!ui) return;
  ui->saved_popover_open = !ui->saved_popover_open;
  if (ui->saved_popover_open) {
    ui->saved_scroll = 0.0f;
    ccord_scroller_cancel(&ui->saved_glide);
    ui->inbox_open = false;
    ui->pins_open = false;
    ui->search_open = false;
  }
  ui->dirty = true;
}

// ---------------------------------------------------------------- INI persistence

void ui_saved_messages_save_ini(const ccord_ui *ui, FILE *f)
{
  if (!ui || !f || ui->nsaved_msgs == 0) return;
  fprintf(f, "[saved_messages]\n");
  for (size_t i = 0; i < ui->nsaved_msgs; i++) {
    const ui_saved_msg *e = &ui->saved_msgs[i];
    fprintf(f, "msg_%zu = %llu,%llu,%llu,%lld,%llu|%s|%s\n", i,
            (unsigned long long)e->channel_id, (unsigned long long)e->message_id,
            (unsigned long long)e->guild_id, (long long)e->ts_ms, (unsigned long long)e->author_id,
            e->label, e->preview);
  }
}

// Visit one [saved_messages] key: msg_N = ch,msg,guild,ts,author|label|preview. Appends in file order
// (index 0 = newest); apply_loaded pushes them into the store afterwards.
void ui_saved_messages_load_ini(ccord_ui *ui, const char *key, const char *value)
{
  if (!ui || !key || !value) return;
  if (strncmp(key, "msg_", 4) != 0) return;
  if (ui->nsaved_msgs >= UI_SAVED_MSG_MAX) return;
  // Split numeric part | label | preview.
  const char *p1 = strchr(value, '|');
  if (!p1) return;
  const char *p2 = strchr(p1 + 1, '|');
  if (!p2) return;
  char nums[128];
  size_t nn = (size_t)(p1 - value);
  if (nn >= sizeof nums) return;
  memcpy(nums, value, nn);
  nums[nn] = '\0';
  unsigned long long ch = 0, msg = 0, guild = 0, author = 0;
  long long ts = 0;
  if (sscanf(nums, "%llu,%llu,%llu,%lld,%llu", &ch, &msg, &guild, &ts, &author) != 5) return;
  if (ch == 0 || msg == 0) return;
  ui_saved_msg *e = &ui->saved_msgs[ui->nsaved_msgs++];
  memset(e, 0, sizeof *e);
  e->channel_id = (ccord_id)ch;
  e->message_id = (ccord_id)msg;
  e->guild_id = (ccord_id)guild;
  e->author_id = (ccord_id)author;
  e->ts_ms = (int64_t)ts;
  snprintf(e->label, sizeof e->label, "%.*s", (int)(p2 - (p1 + 1)), p1 + 1);
  snprintf(e->preview, sizeof e->preview, "%s", p2 + 1);
}

// Push the loaded list into the store (newest first). Insert oldest-first so the store's push-front
// ordering ends up newest-first. Prefer the real message when it is already in the channel history.
void ui_saved_messages_apply_loaded(ccord_ui *ui)
{
  if (!ui || !ui->d.store || ui->nsaved_msgs == 0) return;
  for (size_t k = ui->nsaved_msgs; k > 0; k--) {
    const ui_saved_msg *e = &ui->saved_msgs[k - 1];
    const ccord_message *real = ccord_store_find_message(ui->d.store, e->channel_id, e->message_id);
    ccord_error err;
    if (real) {
      (void)ccord_store_save_message(ui->d.store, real, &err);
      continue;
    }
    ccord_message recon = {0};
    recon.id = e->message_id;
    recon.channel_id = e->channel_id;
    recon.author_id = e->author_id != CCORD_ID_NONE ? e->author_id : 1u;
    recon.timestamp_ms = e->ts_ms >= 0 ? e->ts_ms : 0;
    recon.content = e->preview;
    (void)ccord_store_save_message(ui->d.store, &recon, &err);
  }
}

// ---------------------------------------------------------------- Saved Messages popover

void ui_draw_saved_messages_popover(ccord_ui *ui, ccord_renderer *r)
{
  ui->nsaved_hits = 0;
  ui->saved_popover_rect = (ccord_rect){0};
  ui->saved_close_rect = (ccord_rect){0};
  ui->saved_thumb = (ccord_rect){0};
  ui->saved_content_h = 0.0f;
  if (!ui->saved_popover_open) return;

  ccord_rect br = ui_saved_messages_rect(ui);
  const ccord_layout_rect *h = &ui->layout.chat_header;
  float w = 420.0f, hgt = 420.0f;
  if (w > ui->win_w - 16.0f && ui->win_w > 32.0f) w = ui->win_w - 16.0f;
  if (hgt > ui->win_h - (h->y + h->h + 14.0f) && ui->win_h > 120.0f) hgt = ui->win_h - (h->y + h->h + 14.0f);
  if (hgt < 160.0f) hgt = 160.0f;
  float x = br.x + br.w - w;
  if (x < h->x + 8.0f) x = h->x + 8.0f;
  if (x + w > ui->win_w - 8.0f) x = ui->win_w - w - 8.0f;
  ccord_rect box = {x, h->y + h->h + 6.0f, w, hgt};
  ui->saved_popover_rect = box;

  ui_popup_begin(ui, r, "savedpop", 0, UI_POPUP_MS, UIS_SLIDE_POPOVER);
  uis_popover_frame(ui, r, box);

  const float head_h = UIS_H_BTN + UIS_SP_3;   // header band: title, close, then a rule
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
  ccord_text_draw(r, "Saved Messages", 14, box.x + UIS_SP_3, box.y + UIS_SP_3, &hs, 0.0f);
  ccord_rect close_r = {box.x + box.w - UIS_SP_2 - UIS_ICON_BTN, box.y + UIS_SP_2, UIS_ICON_BTN, UIS_ICON_BTN};
  ui->saved_close_rect = close_r;
  bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, close_r);
  ccord_color cc = uis_icon_button(ui, r, close_r, (uis_state){.hover = chv}, "savedclose", 0);
  ui_icon_close(r, close_r.x + close_r.w * 0.5f, close_r.y + close_r.h * 0.5f, UIS_ICON_GLYPH, cc);
  uis_divider(ui, r, box.x, box.y + head_h, box.w);

  float list_x = box.x + UIS_SP_2;
  float list_y = box.y + head_h + UIS_SP_2;
  float list_w = box.w - 2.0f * UIS_SP_2;
  float list_h = box.h - head_h - 2.0f * UIS_SP_2;
  float card_h = 74.0f, gap = 6.0f;
  size_t n = ui->nsaved_msgs;
  ui->saved_content_h = n > 0 ? (float)n * (card_h + gap) - gap : 0.0f;
  float max_s = ui->saved_content_h > list_h ? ui->saved_content_h - list_h : 0.0f;
  ui->saved_max = max_s;
  ui->saved_scroll = clampf(ui->saved_scroll, 0.0f, max_s);

  ccord_push_clip(r, (ccord_rect){list_x, list_y, list_w, list_h});
  if (n == 0) {
    // Wrapped inside the popover padding, so a long line never runs off either edge.
    const char *empty = "No saved messages yet. Right-click a message and choose Save Message.";
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    float ew = list_w - 2.0f * UIS_SP_2;
    ccord_vec2 et = ccord_text_measure(r, empty, strlen(empty), &es, ew);
    // Centred as a block: the wrap width is the measured block width, so the lines share one left edge.
    ccord_text_draw(r, empty, strlen(empty), list_x + (list_w - et.x) * 0.5f, list_y + (list_h - et.y) * 0.45f, &es,
                    et.x);
  } else {
    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_normal));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    float jump_w = uis_label_width(ui, r, "Jump"), rem_w = uis_label_width(ui, r, "Remove");
    for (size_t i = 0; i < n; i++) {
      float cy = list_y + (float)i * (card_h + gap) - ui->saved_scroll;
      if (cy + card_h < list_y || cy > list_y + list_h) continue;
      const ui_saved_msg *e = &ui->saved_msgs[i];
      ccord_rect card = {list_x, cy, list_w, card_h};
      float btn_y = card.y + (card_h - UIS_H_BTN_SM) * 0.5f;
      ccord_rect rem_r = {card.x + card.w - UIS_SP_3 - rem_w, btn_y, rem_w, UIS_H_BTN_SM};
      ccord_rect jump_r = {rem_r.x - UIS_SP_2 - jump_w, btn_y, jump_w, UIS_H_BTN_SM};
      if (cy >= list_y && cy + card_h <= list_y + list_h && ui->nsaved_hits < UI_SAVED_HIT_MAX) {
        ui->saved_hits[ui->nsaved_hits++] = (ui_saved_hit){
          .row = card, .jump = jump_r, .remove = rem_r,
          .channel_id = e->channel_id, .message_id = e->message_id,
        };
      }
      bool rhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, card);
      bool jhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, jump_r);
      bool xhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rem_r);
      ccord_draw_rrect(r, card, uis_tokens_of(ui).r_control, COL(bg_secondary));
      uis_list_row_fill(ui, r, card, (uis_state){.hover = rhv && !jhv && !xhv}, "savedrow", i);

      // The label and preview stop short of the buttons; the subtitle sits below them and takes the full width.
      float text_w = jump_r.x - (card.x + UIS_SP_3) - UIS_SP_2;
      ccord_text_draw_ellipsis(r, e->label, strlen(e->label), card.x + UIS_SP_3, card.y + UIS_SP_2, text_w, &ls);
      ccord_text_draw_ellipsis(r, e->preview, strlen(e->preview), card.x + UIS_SP_3, card.y + 30.0f, text_w, &ps);
      // Subtitle: where it came from + when.
      char sub[160];
      const ccord_channel *c = ccord_store_channel(ui->d.store, e->channel_id);
      const char *where = c ? (c->name && c->name[0] ? c->name : "Direct Message") : "Unknown channel";
      char when[48];
      when[0] = '\0';
      if (e->ts_ms > 0) ccord_timefmt_message(when, sizeof when, e->ts_ms, ui->now_unix_ms, ui->use_utc);
      snprintf(sub, sizeof sub, "%.80s  %s", where, when);
      ccord_text_draw_ellipsis(r, sub, strlen(sub), card.x + UIS_SP_3, card.y + 52.0f, card.w - 2.0f * UIS_SP_3, &ss);

      uis_button(ui, r, jump_r, "Jump", UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){.hover = jhv}, "savedjump", i);
      uis_button(ui, r, rem_r, "Remove", UIS_BTN_DANGER_SOFT, UIS_BTN_S_SM, (uis_state){.hover = xhv}, "savedrem", i);
    }
  }
  ccord_pop_clip(r);
  ui->saved_thumb = ui_draw_list_scrollbar(ui, r, (ccord_rect){list_x, list_y, list_w, list_h},
                                           ui->saved_content_h, ui->saved_scroll, "savedscroll", 0);
  ui_popup_end(r);
}

static void saved_jump(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id, ccord_id message_id)
{
  ui->saved_popover_open = false;
  if (guild_id == CCORD_ID_NONE) {
    const ccord_channel *c = ccord_store_channel(ui->d.store, channel_id);
    if (c) guild_id = c->guild_id;
  }
  if (guild_id != CCORD_ID_NONE) ui_select_guild_and_channel(ui, guild_id, channel_id);
  if (!ui_jump_to_message(ui, message_id)) {
    ui->pending_jump_msg_id = message_id;
    ui->pending_jump_channel = channel_id;
  }
  ui->dirty = true;
}

bool ui_saved_messages_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ui->saved_popover_open) return false;
  if (ev->type == CCORD_IN_MOUSE_MOVE) return false;
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (ui_in_rect(ev->x, ev->y, ui_saved_messages_rect(ui))) {
      ui->saved_popover_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui_inbox_rect(ui))) {
      ui->saved_popover_open = false;
      ui->inbox_open = true;
      ui->inbox_scroll = 0.0f;
      ccord_scroller_cancel(&ui->inbox_glide);
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, ui_pin_rect(ui))) {
      ui->saved_popover_open = false;
      ui->pins_open = true;
      ui->dirty = true;
      return true;
    }
    if (ui->saved_close_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->saved_close_rect)) {
      ui->saved_popover_open = false;
      ui->dirty = true;
      return true;
    }
    if (ui->saved_popover_rect.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->saved_popover_rect)) {
      ui->saved_popover_open = false;
      ui->dirty = true;
      return true;
    }
    for (size_t i = 0; i < ui->nsaved_hits; i++) {
      const ui_saved_hit *hit = &ui->saved_hits[i];
      if (hit->remove.w > 0.0f && ui_in_rect(ev->x, ev->y, hit->remove)) {
        ui_saved_messages_unsave(ui, hit->channel_id, hit->message_id);
        return true;
      }
      if ((hit->jump.w > 0.0f && ui_in_rect(ev->x, ev->y, hit->jump)) || ui_in_rect(ev->x, ev->y, hit->row)) {
        ccord_id gid = CCORD_ID_NONE;
        long at = saved_find(ui, hit->channel_id, hit->message_id);
        if (at >= 0) gid = ui->saved_msgs[at].guild_id;
        saved_jump(ui, gid, hit->channel_id, hit->message_id);
        return true;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------- Forward picker

typedef struct ui_fwd_item {
  ccord_id guild_id, channel_id;
  char label[96];
  char sub[64];
} ui_fwd_item;

typedef struct ui_forward_state {
  const ccord_ui *owner;
  bool open;
  ccord_id source_channel, source_message;
  ccord_text_edit search;
  bool search_init;
  size_t sel;
  float scroll;
  ccord_scroller glide;   // wheel glide of the list (scroll.h)
  ui_fwd_item items[UI_FORWARD_PICKER_MAX];
  size_t nitems;
  ccord_rect box, search_rect, close_rect, list_view, thumb;
  ccord_rect item_rects[UI_FORWARD_PICKER_MAX];
  float content_h;
} ui_forward_state;

static ui_forward_state *g_fwd;

static void fwd_free(void)
{
  if (!g_fwd) return;
  if (g_fwd->search_init) ccord_text_edit_free(&g_fwd->search);
  ccord_free(g_fwd);
  g_fwd = nullptr;
}

void ui_saved_messages_free(ccord_ui *ui)
{
  (void)ui;
  fwd_free();
}

// Owner-tracked lazy heap state (the M5 pattern): heals when the ui address is reused or the modal kind taken over.
static ui_forward_state *fwd_state(const ccord_ui *ui, bool create)
{
  if (g_fwd && (g_fwd->owner != ui || (g_fwd->open && ui->modal_kind != UI_MODAL_FORWARD_PICKER))) {
    fwd_free();
  }
  if (!g_fwd && create) {
    g_fwd = ccord_calloc(1, sizeof *g_fwd);
    if (g_fwd) g_fwd->owner = ui;
  }
  return g_fwd;
}

static void fwd_add_item(ui_forward_state *st, ccord_id guild_id, ccord_id channel_id, const char *label, const char *sub)
{
  if (st->nitems >= UI_FORWARD_PICKER_MAX) return;
  ui_fwd_item *it = &st->items[st->nitems++];
  it->guild_id = guild_id;
  it->channel_id = channel_id;
  snprintf(it->label, sizeof it->label, "%s", label);
  snprintf(it->sub, sizeof it->sub, "%s", sub ? sub : "");
}

// Rebuild the candidate channel list, filtered by the search query (DMs first, then guild text channels).
static void fwd_rebuild(ccord_ui *ui, ui_forward_state *st)
{
  st->nitems = 0;
  const char *q = ccord_text_edit_cstr(&st->search);
  size_t qlen = strlen(q);
  const ccord_store *store = ui->d.store;
  if (!store) return;
  size_t ndm = ccord_store_channel_count(store, CCORD_DM_GUILD_ID);
  for (size_t ci = 0; ci < ndm; ci++) {
    const ccord_channel *c = ccord_store_channel_at(store, CCORD_DM_GUILD_ID, ci);
    if (!c || c->kind != CCORD_CHANNEL_TEXT || c->is_message_request) continue;
    const char *name = ui_dm_display_name(ui, c);
    if (!ci_contains(name, q, qlen)) continue;
    fwd_add_item(st, CCORD_DM_GUILD_ID, c->id, name, "Direct Message");
  }
  size_t ng = ccord_store_guild_count(store);
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *g = ccord_store_guild_at(store, gi);
    if (!g) continue;
    size_t nc = ccord_store_channel_count(store, g->id);
    for (size_t ci = 0; ci < nc; ci++) {
      const ccord_channel *c = ccord_store_channel_at(store, g->id, ci);
      if (!c || c->kind != CCORD_CHANNEL_TEXT || ccord_store_channel_is_forum(store, c->id)) continue;
      if (!ui_channel_visible(ui, g->id, c)) continue;
      char label[96];
      snprintf(label, sizeof label, "#%.88s", c->name ? c->name : "channel");
      const char *gname = g->name ? g->name : "";
      if (!ci_contains(label, q, qlen) && !ci_contains(gname, q, qlen)) continue;
      fwd_add_item(st, g->id, c->id, label, gname);
    }
  }
  if (st->sel >= st->nitems) st->sel = 0;
}

bool ui_forward_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  ui_forward_state *st = fwd_state(ui, false);
  if (!st || st->glide.pending == 0.0f) return false;
  if (!st->open) {
    ccord_scroller_cancel(&st->glide);
    return false;
  }
  ui->dirty = true;
  return ccord_scroller_tick(&st->glide, &st->scroll, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced);
}

void ui_forward_picker_open(ccord_ui *ui, ccord_id source_channel_id, ccord_id message_id)
{
  if (!ui || source_channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) return;
  if (ui->modal_kind != UI_MODAL_NONE) return;
  fwd_free();
  ui_forward_state *st = fwd_state(ui, true);
  if (!st) return;
  st->owner = ui;
  st->open = true;
  st->source_channel = source_channel_id;
  st->source_message = message_id;
  ccord_text_edit_init(&st->search, 128);
  st->search_init = true;
  st->sel = 0;
  st->scroll = 0.0f;
  ccord_scroller_cancel(&st->glide);
  fwd_rebuild(ui, st);
  ui->modal_kind = UI_MODAL_FORWARD_PICKER;
  ui->dirty = true;
}

bool ui_forward_picker_active(const ccord_ui *ui)
{
  if (!ui || ui->modal_kind != UI_MODAL_FORWARD_PICKER) return false;
  ui_forward_state *st = fwd_state(ui, false);
  return st && st->open;
}

static void fwd_close(ccord_ui *ui)
{
  fwd_free();
  if (ui->modal_kind == UI_MODAL_FORWARD_PICKER) ui->modal_kind = UI_MODAL_NONE;
  ui->dirty = true;
}

static void fwd_do(ccord_ui *ui, ui_forward_state *st, size_t idx)
{
  if (idx >= st->nitems) return;
  ccord_id gid = st->items[idx].guild_id;
  ccord_id cid = st->items[idx].channel_id;
  ccord_id src_ch = st->source_channel;
  ccord_id src_msg = st->source_message;
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    fwd_close(ui);
    return;
  }
  ccord_error err;
  if (ccord_session_forward_message(ui->d.session, cid, src_ch, src_msg, &err) != CCORD_OK) {
    ui_notice(ui, "Cannot forward: %s", err.message);
    fwd_close(ui);
    return;
  }
  ui_notice(ui, "Message forwarded");
  fwd_close(ui);
  ui_select_guild_and_channel(ui, gid, cid);
}

#define FWD_ROW_H 40.0f

void ui_forward_picker_draw(ccord_ui *ui, ccord_renderer *r)
{
  ui_forward_state *st = fwd_state(ui, false);
  if (!st || !st->open) return;
  st->box = (ccord_rect){0};
  st->search_rect = (ccord_rect){0};
  st->close_rect = (ccord_rect){0};
  st->thumb = (ccord_rect){0};
  st->content_h = 0.0f;
  for (size_t i = 0; i < UI_FORWARD_PICKER_MAX; i++) st->item_rects[i] = (ccord_rect){0};

  ccord_rect box = uis_modal_box(ui, UIS_MODAL_W_SM, 480.0f);
  st->box = box;

  ui_popup_begin(ui, r, "fwdpick", 0, UI_MODAL_MS, UIS_SLIDE_MODAL);
  uis_modal_frame(ui, r, box);   // scrim and panel share one popup fade

  float body_y = uis_modal_title(ui, r, box, "Forward Message");
  float title_mid = box.y + UIS_SP_5 + ui_line_h(ui, M(font_size_large)) * 0.5f;
  ccord_rect close_r = {box.x + box.w - UIS_SP_3 - UIS_ICON_BTN, title_mid - UIS_ICON_BTN * 0.5f, UIS_ICON_BTN, UIS_ICON_BTN};
  st->close_rect = close_r;
  bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, close_r);
  ccord_color cc = uis_icon_button(ui, r, close_r, (uis_state){.hover = chv}, "fwdclose", 0);
  ui_icon_close(r, close_r.x + close_r.w * 0.5f, close_r.y + close_r.h * 0.5f, UIS_ICON_GLYPH, cc);

  // Search input (the picker search is always focused while open).
  ccord_rect sr = {box.x + UIS_SP_5, body_y, box.w - 2.0f * UIS_SP_5, UIS_H_INPUT};
  st->search_rect = sr;
  uis_input(ui, r, sr, &st->search, "Search channels", (uis_state){.focused = true}, false, "fwdsearch", 0);

  // Channel list.
  float list_x = box.x + UIS_SP_3;
  float list_y = sr.y + sr.h + UIS_SP_3;
  float list_w = box.w - 2.0f * UIS_SP_3;
  float list_h = box.y + box.h - UIS_SP_3 - list_y;
  st->list_view = (ccord_rect){list_x, list_y, list_w, list_h};
  st->content_h = st->nitems > 0 ? (float)st->nitems * FWD_ROW_H : 0.0f;
  float max_s = st->content_h > list_h ? st->content_h - list_h : 0.0f;
  st->scroll = clampf(st->scroll, 0.0f, max_s);

  ccord_push_clip(r, (ccord_rect){list_x, list_y, list_w, list_h});
  if (st->nitems == 0) {
    const char *empty = "No channels match your search.";
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    ccord_vec2 et = ccord_text_measure(r, empty, strlen(empty), &es, 0.0f);
    ccord_text_draw(r, empty, strlen(empty), list_x + (list_w - et.x) * 0.5f, list_y + (list_h - et.y) * 0.45f, &es, 0.0f);
  } else {
    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    for (size_t i = 0; i < st->nitems; i++) {
      float cy = list_y + (float)i * FWD_ROW_H - st->scroll;
      if (cy + FWD_ROW_H < list_y || cy > list_y + list_h) continue;
      ccord_rect row = {list_x, cy, list_w, FWD_ROW_H - 2.0f};
      st->item_rects[i] = row;
      bool selected = (i == st->sel);
      bool rhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, row);
      uis_list_row_fill(ui, r, row, (uis_state){.hover = rhv, .selected = selected}, "fwdrow", i);
      ccord_text_draw_ellipsis(r, st->items[i].label, strlen(st->items[i].label), row.x + UIS_SP_3, row.y + 4.0f,
                               row.w - 2.0f * UIS_SP_3, &ls);
      ccord_text_draw_ellipsis(r, st->items[i].sub, strlen(st->items[i].sub), row.x + UIS_SP_3, row.y + 21.0f,
                               row.w - 2.0f * UIS_SP_3, &ss);
    }
  }
  ccord_pop_clip(r);
  st->thumb = ui_draw_list_scrollbar(ui, r, (ccord_rect){list_x, list_y, list_w, list_h},
                                     st->content_h, st->scroll, "fwdscroll", 0);
  ui_popup_end(r);
}

// Keep the selected row inside the visible list window.
// Only the arrow keys call this: a key step replaces a wheel glide in flight (scroll.h).
static void fwd_scroll_to_sel(ui_forward_state *st)
{
  if (st->nitems == 0 || st->list_view.h <= 0.0f) return;
  ccord_scroller_cancel(&st->glide);
  float top = (float)st->sel * FWD_ROW_H;
  float bot = top + FWD_ROW_H;
  if (top < st->scroll) st->scroll = top;
  else if (bot > st->scroll + st->list_view.h) st->scroll = bot - st->list_view.h;
}

bool ui_forward_picker_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  ui_forward_state *st = fwd_state(ui, false);
  if (!st || !st->open) return false;
  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_ESCAPE) {
      fwd_close(ui);
      return true;
    }
    if (ev->key == CCORD_KEY_DOWN && st->nitems > 0) {
      st->sel = (st->sel + 1) % st->nitems;
      fwd_scroll_to_sel(st);
      ui->dirty = true;
      return true;
    }
    if (ev->key == CCORD_KEY_UP && st->nitems > 0) {
      st->sel = (st->sel + st->nitems - 1) % st->nitems;
      fwd_scroll_to_sel(st);
      ui->dirty = true;
      return true;
    }
    if (ev->key == CCORD_KEY_RETURN || ev->key == CCORD_KEY_TAB) {
      fwd_do(ui, st, st->sel);
      return true;
    }
    if (ev->key == CCORD_KEY_BACKSPACE) {
      ccord_text_edit_backspace(&st->search);
      fwd_rebuild(ui, st);
      st->sel = 0;
      ui->dirty = true;
      return true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_TEXT) {
    size_t n = strnlen(ev->text, sizeof ev->text);
    ccord_error err;
    if (ccord_text_edit_insert(&st->search, ev->text, n, &err) == CCORD_OK) {
      fwd_rebuild(ui, st);
      st->sel = 0;
      st->scroll = 0.0f;
      ccord_scroller_cancel(&st->glide);
    }
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_WHEEL) {
    float notch = 3.0f * ui_line_h(ui, M(font_size_base));
    float max_s = st->content_h > st->list_view.h ? st->content_h - st->list_view.h : 0.0f;
    ccord_scroller_scroll(&st->glide, &st->scroll, -ev->wheel * notch, max_s, ui_wheel_direct(ui, ev));
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    for (size_t i = 0; i < st->nitems; i++) {
      if (st->item_rects[i].w > 0.0f && ui_in_rect(ev->x, ev->y, st->item_rects[i])) {
        if (st->sel != i) {
          st->sel = i;
          ui->dirty = true;
        }
        break;
      }
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (st->close_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, st->close_rect)) {
      fwd_close(ui);
      return true;
    }
    for (size_t i = 0; i < st->nitems; i++) {
      if (st->item_rects[i].w > 0.0f && ui_in_rect(ev->x, ev->y, st->item_rects[i])) {
        fwd_do(ui, st, i);
        return true;
      }
    }
    if (st->box.w > 0.0f && !ui_in_rect(ev->x, ev->y, st->box)) {
      fwd_close(ui);
      return true;
    }
    return true;
  }
  return true;
}

ccord_cursor_kind ui_forward_picker_cursor(const ccord_ui *ui)
{
  if (!ui_forward_picker_active(ui) || !ui->mouse_in) return CCORD_CURSOR_DEFAULT;
  ui_forward_state *st = fwd_state(ui, false);
  if (!st) return CCORD_CURSOR_DEFAULT;
  if (st->close_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, st->close_rect)) return CCORD_CURSOR_POINTER;
  if (st->search_rect.w > 0.0f && ui_in_rect(ui->mx, ui->my, st->search_rect)) return CCORD_CURSOR_TEXT;
  for (size_t i = 0; i < st->nitems; i++) {
    if (st->item_rects[i].w > 0.0f && ui_in_rect(ui->mx, ui->my, st->item_rects[i])) return CCORD_CURSOR_POINTER;
  }
  return CCORD_CURSOR_DEFAULT;
}

// ---------------------------------------------------------------- debug seams

void ui_saved_messages_get_debug(const ccord_ui *ui, ui_saved_msgs_debug *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!ui) return;
  out->open = ui->saved_popover_open;
  out->count = ui->nsaved_msgs;
  out->header_btn = ui_saved_messages_rect(ui);
  out->popover_rect = ui->saved_popover_rect;
  out->close_rect = ui->saved_close_rect;
  out->scroll = ui->saved_scroll;
  out->content_h = ui->saved_content_h;
  out->thumb = ui->saved_thumb;
  out->hit_count = ui->nsaved_hits;
  for (size_t i = 0; i < ui->nsaved_hits && i < UI_SAVED_HIT_MAX; i++) {
    out->hits[i].channel_id = ui->saved_hits[i].channel_id;
    out->hits[i].message_id = ui->saved_hits[i].message_id;
    out->hits[i].row = ui->saved_hits[i].row;
    out->hits[i].jump = ui->saved_hits[i].jump;
    out->hits[i].remove = ui->saved_hits[i].remove;
  }
}

void ui_forward_picker_get_debug(const ccord_ui *ui, ui_forward_picker_debug *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!ui) return;
  ui_forward_state *st = fwd_state(ui, false);
  if (!st) return;
  out->open = st->open && ui->modal_kind == UI_MODAL_FORWARD_PICKER;
  out->source_channel = st->source_channel;
  out->source_message = st->source_message;
  out->box = st->box;
  out->search_rect = st->search_rect;
  out->close_rect = st->close_rect;
  out->item_count = st->nitems;
  out->sel = st->sel;
  for (size_t i = 0; i < st->nitems && i < UI_FORWARD_PICKER_MAX; i++) {
    out->items[i].guild_id = st->items[i].guild_id;
    out->items[i].channel_id = st->items[i].channel_id;
    out->items[i].rect = st->item_rects[i];
    snprintf(out->items[i].label, sizeof out->items[i].label, "%s", st->items[i].label);
  }
}
