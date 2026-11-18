// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M6 account settings pages: Privacy & Safety, Devices and Connections (m6-ui-devices).
// See settings_m6_devices.h for the contract. Discord state is read from the store / session
// accessors and only ever changes through governed session intents: a toggle sends its PATCH
// and keeps showing the last confirmed value until the full answer lands (design D4). Lists
// are fetched lazily on the page visit, behind the session's 60 s TTL / 15 s in-flight guards.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"
#include "ui_style.h"

#include "settings_m6_devices.h"
#include "settings_m6_prefs.h"   // ui_settings_m6_mask (Streamer Mode personal-info masking)

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

// ---- geometry ---------------------------------------------------------------------------------
// Privacy uses the same 54 px row grid as the other settings pages (settings.c row_y). The two
// list pages scroll: cards hang below a 44 px title + 26 px intro, a footer button follows the
// last card, and a fixed scrollbar lives in the right margin (the Voice & Video pattern).
#define M6_ROW_H 54.0f
#define M6_TITLE_H 44.0f
#define M6_INTRO_H 26.0f
#define M6_FOOTER_H 46.0f
#define M6_DEV_CARD_H 92.0f
#define M6_CONN_CARD_H 132.0f
#define M6_CARD_GAP 10.0f
#define M6_SCROLL_STEP 108.0f

static float privacy_row_y(ccord_rect content, int row)
{
  return content.y + M6_TITLE_H + (float)row * M6_ROW_H;
}

// The row a Privacy toggle sits in: UIS_H_BTN_SM tall, the switch is centred in it and flush right.
static ccord_rect privacy_row_rect(ccord_rect content, int row)
{
  return (ccord_rect){content.x, privacy_row_y(content, row), content.w, UIS_H_BTN_SM};
}

static ccord_rect privacy_toggle_rect(ccord_rect content, int row)
{
  return uis_toggle_rect(privacy_row_rect(content, row));
}

static ccord_rect privacy_chip_rect(ccord_rect content, int row, int opt, int count)
{
  if (count < 1) return (ccord_rect){0, 0, 0, 0};
  float gap = 8.0f;
  float cw = (content.w - gap * (float)(count - 1)) / (float)count;
  if (cw > 132.0f) cw = 132.0f;
  if (cw < 44.0f) cw = 44.0f;
  return (ccord_rect){content.x + (float)opt * (cw + gap), privacy_row_y(content, row) + 20.0f, cw, UIS_H_CHIP};
}

static float m6_card_h(int page)
{
  return page == CCORD_M6_PAGE_DEVICES ? M6_DEV_CARD_H : M6_CONN_CARD_H;
}

static size_t m6_list_count(const ccord_ui *ui, int page)
{
  size_t n = 0;
  if (!ui->d.session) return 0;
  if (page == CCORD_M6_PAGE_DEVICES) (void)ccord_session_auth_sessions(ui->d.session, &n);
  else (void)ccord_session_connections(ui->d.session, &n);
  return n;
}

static float m6_list_top(ccord_rect content)
{
  return content.y + M6_TITLE_H + M6_INTRO_H;
}

static float m6_content_height(int page, size_t n)
{
  return M6_TITLE_H + M6_INTRO_H + (float)n * (m6_card_h(page) + M6_CARD_GAP) + M6_FOOTER_H;
}

static float m6_max_scroll(int page, ccord_rect content, size_t n)
{
  float m = m6_content_height(page, n) - content.h;
  return m > 0.0f ? m : 0.0f;
}

static ccord_rect m6_card_rect(ccord_rect content, int page, size_t i, float scroll)
{
  float h = m6_card_h(page);
  return (ccord_rect){content.x, m6_list_top(content) + (float)i * (h + M6_CARD_GAP) - scroll, content.w, h};
}

static ccord_rect m6_footer_rect(ccord_rect content, int page, size_t n, float scroll)
{
  float y = m6_list_top(content) + (float)n * (m6_card_h(page) + M6_CARD_GAP) + 6.0f - scroll;
  return (ccord_rect){content.x, y, 250.0f, UIS_H_BTN};
}

static ccord_rect m6_retry_rect(ccord_rect content)
{
  return (ccord_rect){content.x, m6_list_top(content), 140.0f, UIS_H_BTN_SM};
}

static ccord_rect m6_device_logout_rect(ccord_rect card)
{
  return (ccord_rect){card.x + card.w - 96.0f, card.y + 8.0f, 84.0f, UIS_H_BTN_SM};
}

static ccord_rect m6_conn_disconnect_rect(ccord_rect card)
{
  return (ccord_rect){card.x + card.w - 110.0f, card.y + 8.0f, 98.0f, UIS_H_BTN_SM};
}

static ccord_rect m6_conn_toggle_rect(ccord_rect card, int idx)
{
  return (ccord_rect){card.x + card.w - 56.0f, card.y + 44.0f + (float)idx * 28.0f, 44.0f, 24.0f};
}

static ccord_rect m6_scroll_track(ccord_rect content)
{
  return (ccord_rect){content.x + content.w + 12.0f, content.y + M6_TITLE_H, 8.0f, content.h - M6_TITLE_H};
}

static ccord_rect m6_scroll_up(ccord_rect content)
{
  ccord_rect t = m6_scroll_track(content);
  return (ccord_rect){t.x - 4.0f, t.y - 24.0f, 16.0f, 20.0f};
}

static ccord_rect m6_scroll_down(ccord_rect content)
{
  ccord_rect t = m6_scroll_track(content);
  return (ccord_rect){t.x - 4.0f, t.y + t.h + 4.0f, 16.0f, 20.0f};
}

static float m6_thumb_h(ccord_rect content, int page, size_t n)
{
  ccord_rect t = m6_scroll_track(content);
  float h = t.h * (content.h / m6_content_height(page, n));
  return h < 24.0f ? 24.0f : h;
}

// ---- transient page state ---------------------------------------------------------------------
// One settings modal at a time (process-wide, like the Voice page state in settings.c);
// ui_settings_m6_reset clears it from ui_settings_init.
static struct {
  int drawn_page;      // module-local id of the last drawn page, -1 before the first draw
  int visited_page;    // page the lazy fetch last ran for (fetch on visit, never per frame)
  ccord_rect content;  // content rect of the last draw/click (for the debug seam)
  float scroll[CCORD_M6_PAGE_COUNT];
  ccord_scroller glide[CCORD_M6_PAGE_COUNT];   // wheel glides of the list pages (scroll.h)
  int confirm_kind;    // 0 none, 1 log-out-all, 2 one device, 3 disconnect
  size_t confirm_index;
} s_m6 = {.drawn_page = -1, .visited_page = -1};

void ui_settings_m6_reset(void)
{
  s_m6.drawn_page = -1;
  s_m6.visited_page = -1;
  s_m6.content = (ccord_rect){0, 0, 0, 0};
  for (int i = 0; i < CCORD_M6_PAGE_COUNT; i++) {
    s_m6.scroll[i] = 0.0f;
    ccord_scroller_cancel(&s_m6.glide[i]);
  }
  s_m6.confirm_kind = 0;
  s_m6.confirm_index = 0;
}

// ---- governed intents -------------------------------------------------------------------------

static void m6_fetch_list(ccord_ui *ui, int page)
{
  if (!ui->d.session) return;
  ccord_error err;
  ccord_status st = page == CCORD_M6_PAGE_DEVICES ? ccord_session_fetch_auth_sessions(ui->d.session, &err)
                                                  : ccord_session_fetch_connections(ui->d.session, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't load your %s: %s", page == CCORD_M6_PAGE_DEVICES ? "devices" : "connections",
              err.message);
  }
  ui->dirty = true;
}

// Discord defaults until the first full settings object arrives (the store is nullptr before that).
static ccord_user_settings m6_privacy_current(const ccord_ui *ui)
{
  const ccord_user_settings *us = ui->d.store ? ccord_store_user_settings(ui->d.store) : nullptr;
  if (us) return *us;
  return (ccord_user_settings){.friend_source_all = true, .locale = "en-US"};
}

static void m6_submit_patch(ccord_ui *ui, const ccord_user_settings_patch *patch)
{
  ui->dirty = true;
  if (!ui->d.session) {
    ui_notice(ui, "Sign in to change your Discord account settings.");
    return;
  }
  ccord_error err;
  if (ccord_session_user_settings_patch(ui->d.session, patch, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't update your settings: %s", err.message);
  }
}

static void m6_submit_logout(ccord_ui *ui, const char hashes[][64], size_t count)
{
  ui->dirty = true;
  if (!ui->d.session) {
    ui_notice(ui, "Sign in to manage your devices.");
    return;
  }
  ccord_error err;
  if (ccord_session_logout_auth_sessions(ui->d.session, hashes, count, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't log out devices: %s", err.message);
  }
}

static void m6_submit_conn_edit(ccord_ui *ui, const ccord_connection *c, const ccord_connection_edit *edit)
{
  ui->dirty = true;
  if (!ui->d.session) {
    ui_notice(ui, "Sign in to manage your connections.");
    return;
  }
  ccord_error err;
  // The route key is the service type + account id (c->name is only the display name).
  if (ccord_session_edit_connection(ui->d.session, c->type, c->id, edit, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't update the connection: %s", err.message);
  }
}

static void m6_submit_conn_delete(ccord_ui *ui, const ccord_connection *c)
{
  ui->dirty = true;
  if (!ui->d.session) {
    ui_notice(ui, "Sign in to manage your connections.");
    return;
  }
  ccord_error err;
  if (ccord_session_delete_connection(ui->d.session, c->type, c->id, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't disconnect the account: %s", err.message);
  }
}

// ---- click handling ---------------------------------------------------------------------------

static bool m6_privacy_click(ccord_ui *ui, ccord_rect content, float x, float y)
{
  ccord_user_settings cur = m6_privacy_current(ui);
  ccord_user_settings_patch p = CCORD_USER_SETTINGS_PATCH_INIT;
  if (ui_in_rect(x, y, privacy_toggle_rect(content, 0))) {
    p.default_guilds_restricted = cur.default_guilds_restricted ? 0 : 1;   // the UI toggle is inverted
    m6_submit_patch(ui, &p);
    return true;
  }
  for (int i = 0; i < 3; i++) {
    if (ui_in_rect(x, y, privacy_chip_rect(content, 1, i, 3))) {
      if (cur.explicit_content_filter != (uint8_t)i) {
        p.explicit_content_filter = (int16_t)i;
        m6_submit_patch(ui, &p);
      }
      return true;
    }
  }
  if (ui_in_rect(x, y, privacy_toggle_rect(content, 2))) {
    p.friend_source_all = cur.friend_source_all ? 0 : 1;
    m6_submit_patch(ui, &p);
    return true;
  }
  if (ui_in_rect(x, y, privacy_toggle_rect(content, 3))) {
    p.friend_source_mutual_friends = cur.friend_source_mutual_friends ? 0 : 1;
    m6_submit_patch(ui, &p);
    return true;
  }
  if (ui_in_rect(x, y, privacy_toggle_rect(content, 4))) {
    p.friend_source_mutual_guilds = cur.friend_source_mutual_guilds ? 0 : 1;
    m6_submit_patch(ui, &p);
    return true;
  }
  if (ui_in_rect(x, y, privacy_toggle_rect(content, 5))) {
    p.allow_access_to_age_restricted_servers = cur.allow_access_to_age_restricted_servers ? 0 : 1;
    m6_submit_patch(ui, &p);
    return true;
  }
  return false;
}

// The fixed scrollbar lives outside the content width, so answer it before clipping rows.
static bool m6_scroll_click(ccord_ui *ui, int page, ccord_rect content, size_t n, float x, float y)
{
  float max = m6_max_scroll(page, content, n);
  if (max <= 0.0f) return false;
  ccord_rect up = m6_scroll_up(content), down = m6_scroll_down(content), tr = m6_scroll_track(content);
  float *scroll = &s_m6.scroll[page];
  ccord_scroller_cancel(&s_m6.glide[page]);   // a scrollbar press replaces any wheel glide
  if (ui_in_rect(x, y, up)) {
    *scroll -= M6_SCROLL_STEP;
  } else if (ui_in_rect(x, y, down)) {
    *scroll += M6_SCROLL_STEP;
  } else if (ui_in_rect(x, y, tr)) {
    float th = m6_thumb_h(content, page, n);
    float ty = tr.y + (tr.h - th) * (*scroll / max);
    if (y < ty) *scroll -= M6_SCROLL_STEP;
    else *scroll += M6_SCROLL_STEP;
  } else {
    return false;
  }
  if (*scroll < 0.0f) *scroll = 0.0f;
  if (*scroll > max) *scroll = max;
  ui->dirty = true;
  return true;
}

static bool m6_devices_click(ccord_ui *ui, ccord_rect content, float x, float y)
{
  size_t n = m6_list_count(ui, CCORD_M6_PAGE_DEVICES);
  ccord_fetch_state state = ui->d.session ? ccord_session_auth_sessions_state(ui->d.session) : CCORD_FETCH_IDLE;
  if (m6_scroll_click(ui, CCORD_M6_PAGE_DEVICES, content, n, x, y)) return true;
  if (!ui_in_rect(x, y, content)) return false;
  if (state == CCORD_FETCH_FAILED && ui_in_rect(x, y, m6_retry_rect(content))) {
    m6_fetch_list(ui, CCORD_M6_PAGE_DEVICES);
    return true;
  }
  const ccord_auth_session *list = ui->d.session ? ccord_session_auth_sessions(ui->d.session, &n) : nullptr;
  float scroll = s_m6.scroll[CCORD_M6_PAGE_DEVICES];
  bool armed_all = s_m6.confirm_kind == 1;
  bool armed_one = s_m6.confirm_kind == 2;
  size_t armed_idx = s_m6.confirm_index;
  s_m6.confirm_kind = 0;   // any click that is not the armed button cancels the confirmation
  if (list) {
    for (size_t i = 0; i < n; i++) {
      if (list[i].current) continue;   // the API refuses to log out the current session
      ccord_rect b = m6_device_logout_rect(m6_card_rect(content, CCORD_M6_PAGE_DEVICES, i, scroll));
      if (ui_in_rect(x, y, b)) {
        if (armed_one && armed_idx == i) {
          char hashes[1][64];
          snprintf(hashes[0], sizeof hashes[0], "%.63s", list[i].id_hash);
          m6_submit_logout(ui, hashes, 1);
        } else {
          s_m6.confirm_kind = 2;
          s_m6.confirm_index = i;
        }
        ui->dirty = true;
        return true;
      }
    }
  }
  ccord_rect all = m6_footer_rect(content, CCORD_M6_PAGE_DEVICES, n, scroll);
  if (ui_in_rect(x, y, all)) {
    size_t others = 0;
    if (list) {
      for (size_t i = 0; i < n; i++) {
        if (!list[i].current) others++;
      }
    }
    if (others == 0) {
      ui_notice(ui, "No other devices are signed in.");
      ui->dirty = true;
      return true;
    }
    if (armed_all) {
      char hashes[CCORD_SESSION_MAX_AUTH_SESSIONS][64];
      size_t k = 0;
      for (size_t i = 0; i < n && k < CCORD_SESSION_MAX_AUTH_SESSIONS; i++) {
        if (list[i].current) continue;
        snprintf(hashes[k], sizeof hashes[k], "%.63s", list[i].id_hash);
        k++;
      }
      m6_submit_logout(ui, hashes, k);
    } else {
      s_m6.confirm_kind = 1;
    }
    ui->dirty = true;
    return true;
  }
  return false;
}

static bool m6_connections_click(ccord_ui *ui, ccord_rect content, float x, float y)
{
  size_t n = m6_list_count(ui, CCORD_M6_PAGE_CONNECTIONS);
  ccord_fetch_state state = ui->d.session ? ccord_session_connections_state(ui->d.session) : CCORD_FETCH_IDLE;
  if (m6_scroll_click(ui, CCORD_M6_PAGE_CONNECTIONS, content, n, x, y)) return true;
  if (!ui_in_rect(x, y, content)) return false;
  if (state == CCORD_FETCH_FAILED && ui_in_rect(x, y, m6_retry_rect(content))) {
    m6_fetch_list(ui, CCORD_M6_PAGE_CONNECTIONS);
    return true;
  }
  const ccord_connection *list = ui->d.session ? ccord_session_connections(ui->d.session, &n) : nullptr;
  float scroll = s_m6.scroll[CCORD_M6_PAGE_CONNECTIONS];
  bool armed_disc = s_m6.confirm_kind == 3;
  size_t armed_idx = s_m6.confirm_index;
  s_m6.confirm_kind = 0;
  if (list) {
    for (size_t i = 0; i < n; i++) {
      ccord_rect card = m6_card_rect(content, CCORD_M6_PAGE_CONNECTIONS, i, scroll);
      ccord_connection_edit edit = {.visibility = -1, .friend_sync = -1, .show_activity = -1};
      if (ui_in_rect(x, y, m6_conn_toggle_rect(card, 0))) {
        edit.visibility = list[i].visibility ? 0 : 1;
        m6_submit_conn_edit(ui, &list[i], &edit);
        return true;
      }
      if (ui_in_rect(x, y, m6_conn_toggle_rect(card, 1))) {
        edit.show_activity = list[i].show_activity ? 0 : 1;
        m6_submit_conn_edit(ui, &list[i], &edit);
        return true;
      }
      if (ui_in_rect(x, y, m6_conn_toggle_rect(card, 2))) {
        edit.friend_sync = list[i].friend_sync ? 0 : 1;
        m6_submit_conn_edit(ui, &list[i], &edit);
        return true;
      }
      if (ui_in_rect(x, y, m6_conn_disconnect_rect(card))) {
        if (armed_disc && armed_idx == i) {
          m6_submit_conn_delete(ui, &list[i]);
        } else {
          s_m6.confirm_kind = 3;
          s_m6.confirm_index = i;
        }
        ui->dirty = true;
        return true;
      }
    }
  }
  if (ui_in_rect(x, y, m6_footer_rect(content, CCORD_M6_PAGE_CONNECTIONS, n, scroll))) {
    // Connecting a NEW account is an OAuth flow: browser redirect, never a local form (design D6).
    ui_open_url(ui, "https://discord.com/connections", "Connections");
    return true;
  }
  return false;
}

// Wheel over a list page: queues one notch (M6_SCROLL_STEP px) on the page's glide. Returns false for
// pages that do not scroll (Privacy) or lists that fit, so the wheel is left unused as before.
bool ui_settings_m6_wheel(ccord_ui *ui, int page, ccord_rect content, float wheel, bool direct)
{
  if (!ui || (page != CCORD_M6_PAGE_DEVICES && page != CCORD_M6_PAGE_CONNECTIONS)) return false;
  float max = m6_max_scroll(page, content, m6_list_count(ui, page));
  if (max <= 0.0f) return false;
  ccord_scroller_scroll(&s_m6.glide[page], &s_m6.scroll[page], -wheel * M6_SCROLL_STEP, max, direct);
  ui->dirty = true;
  return true;
}

// Ticks the list-page glides by fdt_ms (scroll.h). A glide survives only while its page is the drawn one.
bool ui_settings_m6_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  bool moving = false;
  for (int page = 0; page < CCORD_M6_PAGE_COUNT; page++) {
    ccord_scroller *g = &s_m6.glide[page];
    if (g->pending == 0.0f) continue;
    if (!ui->settings_open || s_m6.drawn_page != page) {
      ccord_scroller_cancel(g);
      continue;
    }
    float max = m6_max_scroll(page, s_m6.content, m6_list_count(ui, page));
    moving = ccord_scroller_tick(g, &s_m6.scroll[page], fdt_ms, max, reduced) || moving;
    ui->dirty = true;
  }
  return moving;
}

bool ui_settings_m6_click(ccord_ui *ui, int page, ccord_rect content, ccord_rect panel, float x, float y)
{
  (void)panel;
  if (!ui || page < 0 || page >= CCORD_M6_PAGE_COUNT) return false;
  s_m6.content = content;
  s_m6.drawn_page = page;
  if (page == CCORD_M6_PAGE_PRIVACY) return m6_privacy_click(ui, content, x, y);
  if (page == CCORD_M6_PAGE_DEVICES) return m6_devices_click(ui, content, x, y);
  return m6_connections_click(ui, content, x, y);
}

// ---- drawing helpers --------------------------------------------------------------------------

static void m6_row_label(ccord_ui *ui, ccord_renderer *r, ccord_rect content, int row, const char *label)
{
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_caption, COL(text_muted));
  ccord_text_draw(r, label, strlen(label), content.x, privacy_row_y(content, row), &ls, 0.0f);
}

static void m6_toggle_row(ccord_ui *ui, ccord_renderer *r, ccord_rect content, int row, const char *label,
                          const char *note, bool on, uint64_t id)
{
  m6_row_label(ui, r, content, row, label);
  if (note && note[0]) {
    ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
    ccord_text_draw_ellipsis(r, note, strlen(note), content.x, privacy_row_y(content, row) + 18.0f,
                             content.w - 60.0f, &ns);
  }
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, privacy_toggle_rect(content, row))};
  uis_toggle(ui, r, privacy_row_rect(content, row), on, st, "m6sw", id);
}

static void m6_chip_row(ccord_ui *ui, ccord_renderer *r, ccord_rect content, int row, int count,
                        const char *const *labels, int selected)
{
  for (int i = 0; i < count; i++) {
    ccord_rect o = privacy_chip_rect(content, row, i, count);
    uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, o), .selected = i == selected};
    uint64_t cid = (uint64_t)row * 64u + (uint64_t)i;
    uis_chip(ui, r, o, labels[i], st, "m6chip", cid);
  }
}

static void m6_button(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, bool primary, bool dim)
{
  if (rc.w <= 0.0f) return;
  uis_btn_size size = rc.h <= UIS_H_BTN_SM ? UIS_BTN_S_SM : rc.h >= UIS_H_BTN_LG ? UIS_BTN_S_LG : UIS_BTN_S_MD;
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, rc), .disabled = dim};
  uint64_t id = (uint64_t)(rc.y * 4096.0f + rc.x);
  uis_button(ui, r, rc, label, primary ? UIS_BTN_PRIMARY : UIS_BTN_SECONDARY, size, st, "m6btn", id);
}

static void m6_note(ccord_ui *ui, ccord_renderer *r, float x, float y, float w, const char *text)
{
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  ccord_text_draw_ellipsis(r, text, strlen(text), x, y, w, &ns);
}

// ---- page bodies ------------------------------------------------------------------------------

static void m6_draw_privacy(ccord_ui *ui, ccord_renderer *r, ccord_rect content)
{
  ccord_user_settings cur = m6_privacy_current(ui);
  m6_toggle_row(ui, r, content, 0, "ALLOW DIRECT MESSAGES FROM SERVER MEMBERS",
                "When off, new server members cannot DM you by default.", !cur.default_guilds_restricted, 0);
  m6_row_label(ui, r, content, 1, "DIRECT MESSAGE SPAM FILTER");
  static const char *const k_filter[3] = {"Don't scan", "Non-friends", "Everyone"};
  m6_chip_row(ui, r, content, 1, 3, k_filter, (int)cur.explicit_content_filter);
  m6_toggle_row(ui, r, content, 2, "FRIEND REQUESTS: EVERYONE", "Allow anyone to send you a friend request.",
                cur.friend_source_all, 2);
  m6_toggle_row(ui, r, content, 3, "FRIEND REQUESTS: FRIENDS OF FRIENDS",
                "Allow friends of your friends to send you a friend request.", cur.friend_source_mutual_friends, 3);
  m6_toggle_row(ui, r, content, 4, "FRIEND REQUESTS: SERVER MEMBERS",
                "Allow members of your servers to send you a friend request.", cur.friend_source_mutual_guilds, 4);
  m6_toggle_row(ui, r, content, 5, "AGE-RESTRICTED SERVERS", "Allow access to age-restricted servers.",
                cur.allow_access_to_age_restricted_servers, 5);
}

static void m6_draw_scrollbar(ccord_ui *ui, ccord_renderer *r, ccord_rect content, int page, size_t n, float scroll)
{
  float max = m6_max_scroll(page, content, n);
  if (max <= 0.0f) return;
  ccord_rect tr = m6_scroll_track(content), up = m6_scroll_up(content), down = m6_scroll_down(content);
  float th = m6_thumb_h(content, page, n);
  float ty = tr.y + (tr.h - th) * (scroll / max);
  uis_scrollbar(ui, r, tr, (ccord_rect){tr.x, ty, tr.w, th});
  uis_state us = {.hover = ui_in_rect(ui->mx, ui->my, up)}, ds = {.hover = ui_in_rect(ui->mx, ui->my, down)};
  uint64_t id = (uint64_t)page * 2u;
  uis_scroll_arrow(ui, r, up, true, us, "m6scroll", id);
  uis_scroll_arrow(ui, r, down, false, ds, "m6scroll", id + 1u);
}

static void m6_draw_device_card(ccord_ui *ui, ccord_renderer *r, ccord_rect card, const ccord_auth_session *s,
                                bool armed)
{
  uis_card(ui, r, card);
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_body, COL(text_header));
  const char *dev = s->device[0] ? s->device : "Unknown device";
  ccord_text_draw_ellipsis(r, dev, strlen(dev), card.x + 12.0f, card.y + 10.0f, card.w - 130.0f, &hs);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
  char line[160];
  snprintf(line, sizeof line, "%s · %s", s->os[0] ? s->os : "Unknown OS", s->browser[0] ? s->browser : "Unknown browser");
  ccord_text_draw_ellipsis(r, line, strlen(line), card.x + 12.0f, card.y + 32.0f, card.w - 24.0f, &ns);
  snprintf(line, sizeof line, "%s", s->location[0] ? s->location : "Unknown location");
  ccord_text_draw_ellipsis(r, line, strlen(line), card.x + 12.0f, card.y + 52.0f, card.w - 24.0f, &ns);
  char when[64];
  ccord_timefmt_message(when, sizeof when, s->approx_last_used_ms, ui->now_unix_ms, ui->use_utc);
  snprintf(line, sizeof line, "Last used: %s", when);
  ccord_text_draw_ellipsis(r, line, strlen(line), card.x + 12.0f, card.y + 70.0f, card.w - 24.0f, &ns);
  ccord_rect b = m6_device_logout_rect(card);
  if (s->current) {
    uis_plate(ui, r, b, uis_tokens_of(ui).r_control, ui_alpha(COL(brand), 0.25f));
    ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, COL(brand));
    const char *tag = "This device";
    ccord_vec2 bt = ccord_text_measure(r, tag, strlen(tag), &bs, 0.0f);
    ccord_text_draw(r, tag, strlen(tag), b.x + (b.w - bt.x) * 0.5f, b.y + (b.h - bt.y) * 0.5f, &bs, 0.0f);
  } else {
    m6_button(ui, r, b, armed ? "Confirm" : "Log Out", false, false);
  }
}

static void m6_draw_devices(ccord_ui *ui, ccord_renderer *r, ccord_rect content)
{
  ccord_fetch_state state = ui->d.session ? ccord_session_auth_sessions_state(ui->d.session) : CCORD_FETCH_IDLE;
  size_t n = 0;
  const ccord_auth_session *list = ui->d.session ? ccord_session_auth_sessions(ui->d.session, &n) : nullptr;
  float max = m6_max_scroll(CCORD_M6_PAGE_DEVICES, content, n);
  if (s_m6.scroll[CCORD_M6_PAGE_DEVICES] > max) s_m6.scroll[CCORD_M6_PAGE_DEVICES] = max;
  float scroll = s_m6.scroll[CCORD_M6_PAGE_DEVICES];
  m6_draw_scrollbar(ui, r, content, CCORD_M6_PAGE_DEVICES, n, scroll);   // fixed, outside the clip
  ccord_push_clip(r, content);
  m6_note(ui, r, content.x, content.y + M6_TITLE_H, content.w,
          "Devices signed in to your Discord account. Log out any you don't recognize.");
  if (!ui->d.session) {
    m6_note(ui, r, content.x, m6_list_top(content), content.w, "Sign in to see your devices.");
  } else if (state == CCORD_FETCH_FAILED) {
    m6_note(ui, r, content.x, m6_list_top(content), content.w, "Couldn't load your devices.");
    m6_button(ui, r, m6_retry_rect(content), "Retry", false, false);
  } else if (state != CCORD_FETCH_READY) {
    m6_note(ui, r, content.x, m6_list_top(content), content.w, "Loading your devices...");
  } else {
    for (size_t i = 0; i < n; i++) {
      bool armed = s_m6.confirm_kind == 2 && s_m6.confirm_index == i;
      m6_draw_device_card(ui, r, m6_card_rect(content, CCORD_M6_PAGE_DEVICES, i, scroll), &list[i], armed);
    }
  }
  size_t others = 0;
  if (list) {
    for (size_t i = 0; i < n; i++) {
      if (!list[i].current) others++;
    }
  }
  bool armed_all = s_m6.confirm_kind == 1;
  m6_button(ui, r, m6_footer_rect(content, CCORD_M6_PAGE_DEVICES, n, scroll),
            armed_all ? "Confirm Log Out" : "Log Out All Known Devices", false, others == 0);
  ccord_pop_clip(r);
}

static void m6_draw_conn_card(ccord_ui *ui, ccord_renderer *r, ccord_rect card, const ccord_connection *c, bool armed)
{
  uis_card(ui, r, card);
  // Streamer Mode: the connected account's username is personal information (Hide Personal
  // Information); the account type stays visible.
  char masked_name[80];
  const char *conn_name = ui_settings_m6_mask(ui, c->name, masked_name, sizeof masked_name);
  char title[160];
  snprintf(title, sizeof title, "%s — %s", c->type, conn_name);
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_body, COL(text_header));
  float tw = ccord_text_draw_ellipsis(r, title, strlen(title), card.x + 12.0f, card.y + 10.0f, card.w - 250.0f, &hs);
  float bx = card.x + 12.0f + tw + 10.0f;
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, COL(status_online));
  if (c->verified) {
    ccord_text_draw(r, "Verified", 8, bx, card.y + 13.0f, &bs, 0.0f);
    bx += ccord_text_measure(r, "Verified", 8, &bs, 0.0f).x + 10.0f;
  }
  if (c->revoked) {
    ccord_text_style rs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, COL(danger));
    ccord_text_draw(r, "Revoked", 7, bx, card.y + 13.0f, &rs, 0.0f);
  }
  m6_button(ui, r, m6_conn_disconnect_rect(card), armed ? "Confirm" : "Disconnect", false, false);
  static const char *const k_labels[3] = {"Display on profile", "Display on status", "Sync friends"};
  const bool on[3] = {c->visibility != 0, c->show_activity, c->friend_sync};
  ccord_text_style ls = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
  for (int k = 0; k < 3; k++) {
    ccord_rect t = m6_conn_toggle_rect(card, k);
    ccord_text_draw(r, k_labels[k], strlen(k_labels[k]), card.x + 12.0f,
                    t.y + (t.h - ui_line_h(ui, ls.size)) * 0.5f, &ls, 0.0f);
    uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, t)};
    uis_toggle(ui, r, (ccord_rect){card.x, t.y, card.w - 12.0f, t.h}, on[k], st, "m6conn",
               (uint64_t)ui_name_id(c->type) * 4u + (uint64_t)k);
  }
}

static void m6_draw_connections(ccord_ui *ui, ccord_renderer *r, ccord_rect content)
{
  ccord_fetch_state state = ui->d.session ? ccord_session_connections_state(ui->d.session) : CCORD_FETCH_IDLE;
  size_t n = 0;
  const ccord_connection *list = ui->d.session ? ccord_session_connections(ui->d.session, &n) : nullptr;
  float max = m6_max_scroll(CCORD_M6_PAGE_CONNECTIONS, content, n);
  if (s_m6.scroll[CCORD_M6_PAGE_CONNECTIONS] > max) s_m6.scroll[CCORD_M6_PAGE_CONNECTIONS] = max;
  float scroll = s_m6.scroll[CCORD_M6_PAGE_CONNECTIONS];
  m6_draw_scrollbar(ui, r, content, CCORD_M6_PAGE_CONNECTIONS, n, scroll);
  ccord_push_clip(r, content);
  m6_note(ui, r, content.x, content.y + M6_TITLE_H, content.w,
          "Accounts linked to your Discord. Adding a new one opens discord.com in your browser.");
  if (!ui->d.session) {
    m6_note(ui, r, content.x, m6_list_top(content), content.w, "Sign in to see your connections.");
  } else if (state == CCORD_FETCH_FAILED) {
    m6_note(ui, r, content.x, m6_list_top(content), content.w, "Couldn't load your connections.");
    m6_button(ui, r, m6_retry_rect(content), "Retry", false, false);
  } else if (state != CCORD_FETCH_READY) {
    m6_note(ui, r, content.x, m6_list_top(content), content.w, "Loading your connections...");
  } else {
    for (size_t i = 0; i < n; i++) {
      bool armed = s_m6.confirm_kind == 3 && s_m6.confirm_index == i;
      m6_draw_conn_card(ui, r, m6_card_rect(content, CCORD_M6_PAGE_CONNECTIONS, i, scroll), &list[i], armed);
    }
  }
  m6_button(ui, r, m6_footer_rect(content, CCORD_M6_PAGE_CONNECTIONS, n, scroll), "Connect More Accounts", false,
            false);
  ccord_pop_clip(r);
}

void ui_settings_m6_draw(ccord_ui *ui, ccord_renderer *r, int page, ccord_rect content, ccord_rect panel)
{
  (void)panel;
  if (!ui || page < 0 || page >= CCORD_M6_PAGE_COUNT) return;
  s_m6.content = content;
  s_m6.drawn_page = page;
  if (page != s_m6.visited_page) {
    s_m6.visited_page = page;
    s_m6.confirm_kind = 0;   // a pending confirmation never survives a page change
    if (page == CCORD_M6_PAGE_DEVICES || page == CCORD_M6_PAGE_CONNECTIONS) m6_fetch_list(ui, page);
  }
  if (page == CCORD_M6_PAGE_PRIVACY) m6_draw_privacy(ui, r, content);
  else if (page == CCORD_M6_PAGE_DEVICES) m6_draw_devices(ui, r, content);
  else m6_draw_connections(ui, r, content);
}

// ---- debug seam -------------------------------------------------------------------------------

void ui_settings_m6_get_debug(const ccord_ui *ui, ccord_settings_m6_debug *out)
{
  *out = (ccord_settings_m6_debug){0};
  out->drawn_page = -1;
  if (!ui) return;
  out->devices_state = (int)(ui->d.session ? ccord_session_auth_sessions_state(ui->d.session) : CCORD_FETCH_IDLE);
  out->connections_state =
    (int)(ui->d.session ? ccord_session_connections_state(ui->d.session) : CCORD_FETCH_IDLE);
  out->confirm_kind = s_m6.confirm_kind;
  out->confirm_index = s_m6.confirm_index;
  if (s_m6.drawn_page < 0) return;
  int page = s_m6.drawn_page;
  ccord_rect content = s_m6.content;
  out->drawn_page = page;
  out->content = content;
  out->scroll = s_m6.scroll[page];
  if (page == CCORD_M6_PAGE_PRIVACY) {
    out->dm_toggle = privacy_toggle_rect(content, 0);
    for (int i = 0; i < 3; i++) out->filter_chips[i] = privacy_chip_rect(content, 1, i, 3);
    out->friend_all_toggle = privacy_toggle_rect(content, 2);
    out->friend_mutual_toggle = privacy_toggle_rect(content, 3);
    out->friend_guild_toggle = privacy_toggle_rect(content, 4);
    out->age_toggle = privacy_toggle_rect(content, 5);
    return;
  }
  size_t n = m6_list_count(ui, page);
  float scroll = s_m6.scroll[page];
  float max = m6_max_scroll(page, content, n);
  if (scroll > max) scroll = max;
  if (page == CCORD_M6_PAGE_DEVICES) {
    out->device_count = n;
    size_t an = 0;
    const ccord_auth_session *list = ui->d.session ? ccord_session_auth_sessions(ui->d.session, &an) : nullptr;
    for (size_t i = 0; i < n && list; i++) {
      if (!list[i].current) out->device_logout[i] = m6_device_logout_rect(m6_card_rect(content, page, i, scroll));
    }
    out->logout_all_btn = m6_footer_rect(content, page, n, scroll);
    if (out->devices_state == CCORD_FETCH_FAILED) out->devices_retry_btn = m6_retry_rect(content);
    return;
  }
  out->connection_count = n;
  size_t cn = 0;
  const ccord_connection *list = ui->d.session ? ccord_session_connections(ui->d.session, &cn) : nullptr;
  for (size_t i = 0; i < n && list; i++) {
    ccord_rect card = m6_card_rect(content, page, i, scroll);
    out->conn_visibility[i] = m6_conn_toggle_rect(card, 0);
    out->conn_activity[i] = m6_conn_toggle_rect(card, 1);
    out->conn_sync[i] = m6_conn_toggle_rect(card, 2);
    out->conn_disconnect[i] = m6_conn_disconnect_rect(card);
  }
  out->conn_add_btn = m6_footer_rect(content, page, n, scroll);
  if (out->connections_state == CCORD_FETCH_FAILED) out->connections_retry_btn = m6_retry_rect(content);
}
