// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

// ---------------------------------------------------------------- Server Discovery view

static const char *const k_discovery_cat_names[6] = {
  "Home", "Gaming", "Music", "Education", "Science & Tech", "Entertainment",
};

void ui_draw_discovery(ccord_ui *ui, ccord_renderer *r)
{
  ui->ndiscovery_hits = 0;
  ui->discovery_search_rect = (ccord_rect){0};
  for (size_t i = 0; i < 6u; i++) ui->discovery_cat_rects[i] = (ccord_rect){0};
  ui->discovery_content_h = 0.0f;
  if (!ui->discovery_open) return;

  float top_in = ui->mobile_layout ? ui->safe_insets.top : 0.0f;
  float bot_in = ui->mobile_layout ? ui->safe_insets.bottom : 0.0f;
  ccord_rect pane = {ui->layout.chat_header.x, 0.0f, ui->layout.chat_header.w, ui->win_h};
  if (pane.w <= 0.0f) pane.w = maxf(ui->win_w - pane.x, 0.0f);
  ccord_rect bg = ui->mobile_layout ? (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h} : pane;
  ccord_draw_rect(r, bg, COL(bg_primary));
  ccord_push_clip(r, pane);

  float pad_x = ui->mobile_layout ? 14.0f : 24.0f;
  ccord_rect hero = {pane.x + pad_x, top_in + 16.0f, maxf(pane.w - 2.0f * pad_x, 220.0f), 116.0f};
  ccord_draw_rrect(r, hero, 10.0f, ui_mix(COL(bg_secondary), COL(brand), 0.16f));

  ccord_text_style hts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 2.0f, COL(text_header));
  ccord_text_draw_ellipsis(r, "Find your community on Discord", 30, hero.x + 20.0f, hero.y + 14.0f, hero.w - 40.0f, &hts);
  ccord_text_style hss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_secondary));
  ccord_text_draw_ellipsis(r, "From gaming, to music, to learning, there's a place for you.", 60,
                           hero.x + 20.0f, hero.y + 39.0f, hero.w - 40.0f, &hss);

  ccord_rect sbox = {hero.x + 20.0f, hero.y + 68.0f, clampf(hero.w - 40.0f, 160.0f, 480.0f), 34.0f};
  ui->discovery_search_rect = sbox;
  bool sfocused = !ui->settings_open && !ui->menu.open && ui->modal_kind == UI_MODAL_NONE &&
                  !ui->quick_switcher_open && !ui->shortcuts_open;
  if (sfocused) {
    ccord_draw_rrect(r, (ccord_rect){sbox.x - 1.0f, sbox.y - 1.0f, sbox.w + 2.0f, sbox.h + 2.0f}, 6.0f, COL(brand));
  }
  ccord_draw_rrect(r, sbox, 5.0f, COL(bg_input));
  ui_icon_search(r, sbox.x + 15.0f, sbox.y + sbox.h * 0.5f, 15.0f, COL(text_muted));
  const char *q = ccord_text_edit_cstr(&ui->discovery_edit);
  bool has_q = q[0] != '\0';
  ccord_text_style qst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base),
                                  has_q ? COL(text_normal) : COL(text_placeholder));
  float qlh = ui_line_h(ui, qst.size);
  const char *qdisp = has_q ? q : "Explore communities";
  ccord_text_draw_ellipsis(r, qdisp, strlen(qdisp), sbox.x + 30.0f, sbox.y + (sbox.h - qlh) * 0.5f,
                           maxf(sbox.w - 40.0f, 20.0f), &qst);
  if (sfocused && ui->caret_on) {
    ccord_vec2 csz = ccord_text_measure(r, q, ui->discovery_edit.caret, &qst, 0.0f);
    float caretx = fminf(sbox.x + 30.0f + csz.x, sbox.x + sbox.w - 10.0f);
    ccord_draw_rect(r, (ccord_rect){caretx, sbox.y + (sbox.h - qlh) * 0.5f, 1.5f, qlh}, COL(text_normal));
  }

  float chip_y = hero.y + hero.h + 12.0f;
  float chip_x = hero.x;
  for (uint32_t c = 0; c < 6u; c++) {
    bool active = (ui->discovery_category == c);
    ccord_text_style cs = ui_style(ui, active ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small),
                                   active ? COL(text_on_brand) : COL(text_secondary));
    ccord_vec2 ct = ccord_text_measure(r, k_discovery_cat_names[c], strlen(k_discovery_cat_names[c]), &cs, 0.0f);
    ccord_rect cr = {chip_x, chip_y, ct.x + 24.0f, 28.0f};
    ui->discovery_cat_rects[c] = cr;
    bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cr);
    ccord_draw_rrect(r, cr, 14.0f, active ? COL(brand) : (chv ? COL(bg_item_hover) : COL(bg_secondary)));
    ccord_text_draw(r, k_discovery_cat_names[c], strlen(k_discovery_cat_names[c]),
                    cr.x + 12.0f, cr.y + (cr.h - ct.y) * 0.5f, &cs, 0.0f);
    chip_x += cr.w + 8.0f;
  }

  size_t count = 0;
  uint32_t total = 0, offset = 0;
  bool loading = false;
  const ccord_discover_guild *items = ui->d.session
    ? ccord_session_discovery_results(ui->d.session, &count, &total, &offset, &loading)
    : nullptr;

  float hdr_y = chip_y + 38.0f;
  char hdr_buf[96];
  if (loading && count == 0) {
    snprintf(hdr_buf, sizeof hdr_buf, "Loading discoverable servers...");
  } else {
    snprintf(hdr_buf, sizeof hdr_buf, "%u Featured %s",
             (unsigned)total, total == 1u ? "Community" : "Communities");
  }
  ccord_text_style shs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
  ccord_text_draw_ellipsis(r, hdr_buf, strlen(hdr_buf), hero.x, hdr_y, hero.w, &shs);

  float cards_y = hdr_y + 26.0f;
  float view_h = maxf(pane.h - bot_in - cards_y - 12.0f, 48.0f);
  size_t cols = hero.w >= 540.0f ? 2u : 1u;
  float gap = 12.0f;
  float card_w = cols == 2u ? (hero.w - gap) * 0.5f : hero.w;
  float card_h = 122.0f;
  size_t rows = count > 0 ? (count + cols - 1u) / cols : 0u;
  ui->discovery_content_h = rows > 0 ? (float)rows * (card_h + gap) - gap : 0.0f;
  float max_s = ui->discovery_content_h > view_h ? ui->discovery_content_h - view_h : 0.0f;
  ui->discovery_max = max_s;
  if (ui->discovery_scroll > max_s) ui->discovery_scroll = max_s;
  if (ui->discovery_scroll < 0.0f) ui->discovery_scroll = 0.0f;

  ccord_rect list_clip = {hero.x, cards_y, hero.w, view_h};
  ccord_push_clip(r, list_clip);
  if (count == 0 && !loading) {
    const char *empty = "No discoverable servers matched your search.";
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
    ccord_vec2 et = ccord_text_measure(r, empty, strlen(empty), &es, 0.0f);
    ccord_text_draw(r, empty, strlen(empty), hero.x + (hero.w - et.x) * 0.5f,
                    cards_y + (view_h - et.y) * 0.42f, &es, 0.0f);
  } else {
    ccord_text_style nst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    ccord_text_style dst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_secondary));
    ccord_text_style mst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    for (size_t i = 0; i < count && i < CCORD_DISCOVERY_PAGE_MAX; i++) {
      const ccord_discover_guild *d = &items[i];
      size_t col = i % cols, row = i / cols;
      ccord_rect card = {
        hero.x + (float)col * (card_w + gap),
        cards_y + (float)row * (card_h + gap) - ui->discovery_scroll,
        card_w,
        card_h,
      };
      ccord_rect jbtn = {card.x + card.w - 86.0f, card.y + card.h - 36.0f, 74.0f, 26.0f};
      if (ui->ndiscovery_hits < CCORD_DISCOVERY_PAGE_MAX) {
        auto hit = &ui->discovery_hits[ui->ndiscovery_hits++];
        hit->join_rect = jbtn;
        hit->guild_id = d->guild_id;
        snprintf(hit->vanity_code, sizeof hit->vanity_code, "%s", d->vanity_code);
      }
      if (card.y + card.h < cards_y || card.y > cards_y + view_h) continue;

      bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, card);
      ccord_draw_rrect(r, (ccord_rect){card.x - 1.0f, card.y - 1.0f, card.w + 2.0f, card.h + 2.0f}, 9.0f, COL(border_subtle));
      ccord_draw_rrect(r, card, 8.0f, chv ? COL(bg_item_hover) : COL(bg_secondary));

      ccord_rect ic = {card.x + 12.0f, card.y + 12.0f, 44.0f, 44.0f};
      if (d->icon_url[0] && ui_ensure_media(ui, r, d->icon_url)) {
        ccord_draw_texture_rounded(r, d->icon_url, ic, 12.0f, 255);
      } else {
        ccord_draw_rrect(r, ic, 12.0f, COL(brand));
        char init[8];
        ui_initials(d->name, init);
        ccord_text_style ist = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_on_brand));
        ccord_vec2 it = ccord_text_measure(r, init, strlen(init), &ist, 0.0f);
        ccord_text_draw(r, init, strlen(init), ic.x + (ic.w - it.x) * 0.5f, ic.y + (ic.h - it.y) * 0.5f, &ist, 0.0f);
      }

      const char *gname = d->name[0] ? d->name : "Server";
      ccord_text_draw_ellipsis(r, gname, strlen(gname), ic.x + ic.w + 10.0f, card.y + 14.0f,
                               maxf(card.w - 78.0f, 40.0f), &nst);
      if (d->description[0]) {
        ccord_push_clip(r, (ccord_rect){ic.x + ic.w + 10.0f, card.y + 36.0f, maxf(card.w - 78.0f, 40.0f), 38.0f});
        ccord_text_draw(r, d->description, strlen(d->description), ic.x + ic.w + 10.0f, card.y + 36.0f,
                        &dst, maxf(card.w - 78.0f, 40.0f));
        ccord_pop_clip(r);
      }

      float mcy = card.y + card.h - 23.0f;
      ccord_draw_circle(r, card.x + 18.0f, mcy, 4.0f, COL(status_online));
      char stats[80];
      snprintf(stats, sizeof stats, "%u Online  •  %u Members", (unsigned)d->online_count, (unsigned)d->member_count);
      ccord_text_draw_ellipsis(r, stats, strlen(stats), card.x + 28.0f, mcy - ui_line_h(ui, mst.size) * 0.5f,
                               maxf(jbtn.x - (card.x + 34.0f), 30.0f), &mst);

      bool joined = ui->d.store && ccord_store_guild(ui->d.store, d->guild_id) != nullptr;
      bool jhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, jbtn);
      ccord_color jbg = joined ? (jhv ? COL(bg_item_selected) : COL(bg_tertiary))
                               : (jhv ? ui_mix(COL(status_online), COL(brand), 0.35f) : COL(status_online));
      ccord_draw_rrect(r, jbtn, 5.0f, jbg);
      const char *jlbl = joined ? "Joined" : "Join";
      ccord_text_style jst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f,
                                      joined ? COL(text_header) : COL(text_on_brand));
      ccord_vec2 jt = ccord_text_measure(r, jlbl, strlen(jlbl), &jst, 0.0f);
      ccord_text_draw(r, jlbl, strlen(jlbl), jbtn.x + (jbtn.w - jt.x) * 0.5f, jbtn.y + (jbtn.h - jt.y) * 0.5f, &jst, 0.0f);
    }
  }
  ccord_pop_clip(r);
  ccord_pop_clip(r);
}

bool ui_discovery_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || !ui->discovery_open) return false;
  ccord_rect pane = {ui->layout.chat_header.x, 0.0f, ui->layout.chat_header.w, ui->win_h};
  if (pane.w <= 0.0f) pane.w = maxf(ui->win_w - pane.x, 0.0f);

  if (ev->type == CCORD_IN_WHEEL) {
    if (!ui_in_rect(ev->x, ev->y, pane)) return false;
    // The draw computed the bound (discovery_max) from the card area it lays out; reuse it so both clamp alike.
    ccord_scroller_scroll(&ui->discovery_glide, &ui->discovery_scroll, -ev->wheel * 48.0f, ui->discovery_max,
                          ui_wheel_direct(ui, ev));
    ui->dirty = true;
    return true;
  }

  if (ev->type == CCORD_IN_MOUSE_DOWN) {
    if (!ui_in_rect(ev->x, ev->y, pane)) return false;
    if (ev->button != 1) return true;
    for (uint32_t c = 0; c < 6u; c++) {
      if (ui->discovery_cat_rects[c].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->discovery_cat_rects[c])) {
        if (ui->discovery_category != c) {
          ui->discovery_category = c;
          ui->discovery_scroll = 0.0f;
          ui->discovery_due_ms = 0;
          ui_discovery_request(ui, false);
          ui->dirty = true;
        }
        return true;
      }
    }
    float top_in = ui->mobile_layout ? ui->safe_insets.top : 0.0f;
    float bot_in = ui->mobile_layout ? ui->safe_insets.bottom : 0.0f;
    float cards_top = top_in + 204.0f;
    for (size_t i = 0; i < ui->ndiscovery_hits; i++) {
      ccord_rect jr = ui->discovery_hits[i].join_rect;
      if (jr.w <= 0.0f || jr.y + jr.h < cards_top || jr.y > pane.h - bot_in - 12.0f) continue;
      if (ui_in_rect(ev->x, ev->y, jr)) {
        ccord_id gid = ui->discovery_hits[i].guild_id;
        if (ui->d.store && ccord_store_guild(ui->d.store, gid) != nullptr) {
          ccord_error err;
          if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, gid, &err);
          else (void)ccord_store_select_guild(ui->d.store, gid, &err);
          ui->discovery_open = false;
          ui_compute_layout(ui);
          ui_ensure_caches(ui);
          ui_sync_messages(ui);
          ui_record_nav_location(ui);
          ui->dirty = true;
          return true;
        }
        if (ui->discovery_hits[i].vanity_code[0] != '\0') {
          ccord_admin_request req;
          ccord_error err;
          if (ccord_admin_accept_invite(&req, ui->discovery_hits[i].vanity_code, &err) != CCORD_OK) {
            ui_notice(ui, "Couldn't join server: %s", err.message);
            return true;
          }
          ui->discovery_pending_guild_id = gid;
          ui_admin_submit(ui, &req);
          ui->dirty = true;
        }
        return true;
      }
    }
    ui->dirty = true;
    return true;
  }

  if (ev->type == CCORD_IN_MOUSE_MOVE || ev->type == CCORD_IN_MOUSE_UP) {
    return false;
  }

  if (ev->type == CCORD_IN_KEY_DOWN) {
    bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
    bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
    if ((ev->mods & CCORD_MOD_ALT) != 0) return false;
    if (ctrl && (ev->key == CCORD_KEY_K || ev->key == CCORD_KEY_SLASH || ev->key == CCORD_KEY_I)) return false;

    if (ev->key == CCORD_KEY_ESCAPE) {
      if (ui->discovery_edit.len > 0) {
        ccord_text_edit_clear(&ui->discovery_edit);
        ui->discovery_scroll = 0.0f;
        ui->discovery_due_ms = 0;
        ui_discovery_request(ui, false);
      } else {
        ui->discovery_open = false;
        ui_compute_layout(ui);
      }
      ui->dirty = true;
      return true;
    }
    if (ev->key == CCORD_KEY_RETURN) {
      ui->discovery_due_ms = 0;
      ui->discovery_scroll = 0.0f;
      ui_discovery_request(ui, false);
      ui->dirty = true;
      return true;
    }

    size_t prev_len = ui->discovery_edit.len;
    switch (ev->key) {
      case CCORD_KEY_BACKSPACE:
        if (ctrl) ccord_text_edit_backspace_word(&ui->discovery_edit);
        else ccord_text_edit_backspace(&ui->discovery_edit);
        break;
      case CCORD_KEY_DELETE:
        if (ctrl) ccord_text_edit_delete_word(&ui->discovery_edit);
        else ccord_text_edit_delete(&ui->discovery_edit);
        break;
      case CCORD_KEY_LEFT: ccord_text_edit_move(&ui->discovery_edit, ctrl ? -3 : -1, shift); break;
      case CCORD_KEY_RIGHT: ccord_text_edit_move(&ui->discovery_edit, ctrl ? 3 : 1, shift); break;
      case CCORD_KEY_HOME: ccord_text_edit_move(&ui->discovery_edit, -2, shift); break;
      case CCORD_KEY_END: ccord_text_edit_move(&ui->discovery_edit, 2, shift); break;
      case CCORD_KEY_A: if (ctrl) ccord_text_edit_select_all(&ui->discovery_edit); break;
      case CCORD_KEY_V:
        if (ctrl && ui->d.host.clipboard_get) {
          char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
          if (clip) {
            ccord_error perr;
            (void)ccord_text_edit_insert(&ui->discovery_edit, clip, strlen(clip), &perr);
            ccord_free(clip);
          }
        }
        break;
      default:
        return false;
    }
    if (ui->discovery_edit.len != prev_len || ev->key == CCORD_KEY_V) {
      uint64_t due = ui->now_ms + UI_SEARCH_DEBOUNCE_MS;
      ui->discovery_due_ms = due != 0 ? due : 1u;
      ui->discovery_scroll = 0.0f;
    }
    ui->dirty = true;
    return true;
  }

  if (ev->type == CCORD_IN_TEXT && ev->text[0] != '\0') {
    ccord_error ierr;
    (void)ccord_text_edit_insert(&ui->discovery_edit, ev->text, strlen(ev->text), &ierr);
    uint64_t due = ui->now_ms + UI_SEARCH_DEBOUNCE_MS;
    ui->discovery_due_ms = due != 0 ? due : 1u;
    ui->discovery_scroll = 0.0f;
    ui->dirty = true;
    return true;
  }

  return false;
}

