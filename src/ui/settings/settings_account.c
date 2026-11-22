// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "settings_internal.h"

bool st_handle_account_field_edit_key(ccord_ui *ui, ccord_text_edit *active, bool masked,
                                          const ccord_input_event *ev)
{
  if (!active || ev->type != CCORD_IN_KEY_DOWN) return false;
  bool mod = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
  if (ev->key == CCORD_KEY_BACKSPACE) {
    if (mod) ccord_text_edit_backspace_word(active);
    else ccord_text_edit_backspace(active);
    ui->dirty = true;
    return true;
  }
  if (ev->key == CCORD_KEY_DELETE) {
    if (mod) ccord_text_edit_delete_word(active);
    else ccord_text_edit_delete(active);
    ui->dirty = true;
    return true;
  }
  if (ev->key == CCORD_KEY_LEFT) {
    ccord_text_edit_move(active, mod ? -3 : -1, (ev->mods & CCORD_MOD_SHIFT) != 0);
    ui->dirty = true;
    return true;
  }
  if (ev->key == CCORD_KEY_RIGHT) {
    ccord_text_edit_move(active, mod ? 3 : 1, (ev->mods & CCORD_MOD_SHIFT) != 0);
    ui->dirty = true;
    return true;
  }
  if (ev->key == CCORD_KEY_HOME) {
    ccord_text_edit_move(active, -2, (ev->mods & CCORD_MOD_SHIFT) != 0);
    ui->dirty = true;
    return true;
  }
  if (ev->key == CCORD_KEY_END) {
    ccord_text_edit_move(active, 2, (ev->mods & CCORD_MOD_SHIFT) != 0);
    ui->dirty = true;
    return true;
  }
  if (mod && ev->key == CCORD_KEY_A) {
    ccord_text_edit_select_all(active);
    ui->dirty = true;
    return true;
  }
  if (mod && !masked && (ev->key == CCORD_KEY_C || ev->key == CCORD_KEY_X) && ui->d.host.clipboard_set) {
    char *sel = ccord_text_edit_copy_selection(active);
    if (sel) {
      ui->d.host.clipboard_set(ui->d.host.ud, sel);
      ccord_free(sel);
      if (ev->key == CCORD_KEY_X) ccord_text_edit_backspace(active);
    } else if (active->len > 0) {
      ui->d.host.clipboard_set(ui->d.host.ud, ccord_text_edit_cstr(active));
      if (ev->key == CCORD_KEY_X) ccord_text_edit_clear(active);
    }
    ui->dirty = true;
    return true;
  }
  if (mod && ev->key == CCORD_KEY_V && ui->d.host.clipboard_get) {
    char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
    if (clip) {
      ccord_error ierr;
      (void)ccord_text_edit_insert(active, clip, strlen(clip), &ierr);
      ccord_free(clip);
    }
    ui->dirty = true;
    return true;
  }
  return false;
}

bool st_handle_mfa_popup(ccord_ui *ui, const ccord_input_event *ev)
{
  ccord_rect pop = st_mfa_popup_rect(ui);
  ccord_rect mbox = st_mfa_popup_input_rect(&pop);
  ccord_rect vbtn = st_mfa_popup_verify_rect(&pop);
  ccord_rect bbtn = st_mfa_popup_back_rect(&pop);
  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    int64_t hk = ui_in_rect(ev->x, ev->y, vbtn) ? 901
               : ui_in_rect(ev->x, ev->y, bbtn) ? 902
               : ui_in_rect(ev->x, ev->y, mbox) ? 903
                                                : 900;
    if (hk != st_s_hover_key) {
      st_s_hover_key = hk;
      ui->dirty = true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 3) {
    if (ui_in_rect(ev->x, ev->y, mbox)) {
      ui->account_focus_field = 4;
      ui->text_area = mbox;
      ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_mfa_edit, false, false, false, false);
      return true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (ui_in_rect(ev->x, ev->y, mbox)) {
      ui->account_focus_field = 4;
      ui->touch_text_focused = true;
      ui->text_area = mbox;
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, vbtn)) {
      ui->touch_text_focused = false;
      if (ui->d.host.account_action) {
        ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN);
      }
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, bbtn)) {
      ui_settings_dismiss_mfa(ui);
      return true;
    }
    ui->touch_text_focused = false;
    return true;
  }
  if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
    ccord_error ierr;
    (void)ccord_text_edit_insert(&ui->account_mfa_edit, ev->text, strlen(ev->text), &ierr);
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_ESCAPE) {
      ui_settings_dismiss_mfa(ui);
      return true;
    }
    if (ev->key == CCORD_KEY_RETURN) {
      ui->touch_text_focused = false;
      if (ui->d.host.account_action) {
        ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN);
      }
      ui->dirty = true;
      return true;
    }
    if (st_handle_account_field_edit_key(ui, &ui->account_mfa_edit, false, ev)) return true;
  }
  return true;
}

void st_account_buttons(const ccord_ui *ui, const sgeom *g, ccord_rect *primary, ccord_rect *secondary)
{
  float wy = st_account_web_y(ui, g);
  float extra_web = g->content.w < 380.0f ? (UIS_H_BTN + 6.0f) : 0.0f;
  float min_by = wy + UIS_H_BTN + extra_web + 6.0f;
  float bot_by = g->content.y + g->content.h - 42.0f;
  float by = bot_by > min_by ? bot_by : min_by;
  float pw = fminf(156.0f, fmaxf((g->content.w - 12.0f) * 0.54f, 96.0f));
  float sw = fminf(132.0f, fmaxf(g->content.w - pw - 12.0f, 84.0f));
  *primary = (ccord_rect){g->content.x, by, pw, UIS_H_BTN};
  *secondary = (ccord_rect){g->content.x + pw + 12.0f, by, sw, UIS_H_BTN};
}

void st_draw_button(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, bool primary)
{
  if (rc.w <= 0.0f) return;
  uis_btn_size size = rc.h <= UIS_H_BTN_SM ? UIS_BTN_S_SM : rc.h >= UIS_H_BTN_LG ? UIS_BTN_S_LG : UIS_BTN_S_MD;
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, rc), .pressed = st_settings_pressed(ui, rc)};
  uint64_t id = (uint64_t)(rc.y * 4096.0f + rc.x);
  uis_button(ui, r, rc, label, primary ? UIS_BTN_PRIMARY : UIS_BTN_SECONDARY, size, st, "sbtn", id);
}

static void draw_input_field(ccord_ui *ui, ccord_renderer *r, ccord_rect box, const ccord_text_edit *ed,
                             bool focused, bool masked, const char *placeholder)
{
  uis_state st = {.focused = focused};
  uis_input(ui, r, box, ed, placeholder, st, masked, "sfield", (uint64_t)(box.y * 4096.0f + box.x));
}

void st_draw_account(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  const ccord_account_view *v = &ui->account_view;
  float x = g->content.x, w = g->content.w;
  float y = g->content.y + 40.0f;
  ccord_text_style ss = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_title, COL(text_header));
  ccord_text_draw_ellipsis(r, v->status, strlen(v->status), x, y, w, &ss);
  y += ui_line_h(ui, ss.size) + 4.0f;
  const char *detail_str = (ui->account_login_status[0] != '\0') ? ui->account_login_status : v->detail;
  if (detail_str && detail_str[0]) {
    ccord_color dcol = (ui->account_login_status[0] != '\0' && ui->account_login_is_error) ? COL(danger) : COL(text_secondary);
    ccord_text_style dstyle = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, dcol);
    ccord_vec2 d = ccord_text_measure(r, detail_str, strlen(detail_str), &dstyle, w);
    ccord_text_draw(r, detail_str, strlen(detail_str), x, y, &dstyle, w);
    y += d.y + 8.0f;
  }
  if (v->busy && !v->signed_in) {
    ccord_draw_rrect(r, (ccord_rect){x, y, w, 6.0f}, 3.0f, COL(bg_tertiary));
    ccord_draw_rrect(r, (ccord_rect){x, y, w * 0.4f, 6.0f}, 3.0f, COL(brand));
    y += 18.0f;
  }

  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
  if (v->signed_in) {
    // Streamer Mode masks the account identity strings (local preference, design §2 Streamer Mode).
    char mname[128], muser[128], mid[32];
    const char *mdn = ui_settings_m6_mask(ui, v->display_name ? v->display_name : "", mname, sizeof mname);
    const char *mun = ui_settings_m6_mask(ui, v->username ? v->username : "", muser, sizeof muser);
    const char *mid_s = ui_settings_m6_mask(ui, v->user_id_text, mid, sizeof mid);
    char handle[CCORD_IDENTITY_NAME_BYTES + 2];
    snprintf(handle, sizeof handle, "@%s", mun);
    float colw = (w - 16.0f) * 0.5f;
    float y_left = y, y_right = y;
    st_label_value(ui, r, x, &y_left, colw, "DISPLAY NAME", mdn);
    st_label_value(ui, r, x + colw + 16.0f, &y_right, colw, "USERNAME", handle);
    y = y_left > y_right ? y_left : y_right;
    y_left = y;
    y_right = y;
    st_label_value(ui, r, x, &y_left, colw, "USER ID", mid_s);
    char guilds[256];
    int gn = snprintf(guilds, sizeof guilds, "%zu guild%s", v->guild_count, v->guild_count == 1 ? "" : "s");
    if (v->identity && v->guild_count > 0 && v->identity->guilds[0].name[0] && gn > 0 && (size_t)gn + 3 < sizeof guilds) {
      gn += snprintf(guilds + gn, sizeof guilds - (size_t)gn, ": %.60s", v->identity->guilds[0].name);
      if (v->guild_count > 1 && v->identity->guilds[1].name[0] && gn > 0 && (size_t)gn + 3 < sizeof guilds) {
        gn += snprintf(guilds + gn, sizeof guilds - (size_t)gn, ", %.60s", v->identity->guilds[1].name);
      }
      if (v->guild_count > 2 && gn > 0 && (size_t)gn + 5 < sizeof guilds) {
        snprintf(guilds + gn, sizeof guilds - (size_t)gn, ", ...");
      }
    }
    st_label_value(ui, r, x + colw + 16.0f, &y_right, colw, "GUILDS", guilds);
    y = y_left > y_right ? y_left : y_right;
  } else {
    ccord_rect em_box = st_account_email_box_rect(g);
    ccord_rect pw_box = st_account_password_box_rect(g);
    ccord_rect login_btn = st_account_login_btn_rect(g);
    ccord_text_draw(r, "EMAIL OR PHONE", 14, em_box.x, em_box.y - 15.0f, &ls, 0.0f);
    ccord_text_draw(r, "PASSWORD", 8, pw_box.x, pw_box.y - 15.0f, &ls, 0.0f);
    draw_input_field(ui, r, em_box, &ui->account_email_edit, ui->account_focus_field == 2, false, "you@example.com");
    draw_input_field(ui, r, pw_box, &ui->account_password_edit, ui->account_focus_field == 3, true, "Password");
    st_draw_button(ui, r, login_btn, "Log In to Discord", true);
  }

  ccord_rect tbox = st_account_token_box_rect(g);
#if defined(_WIN32)
  const char *tok_hdr = "STANDALONE DISCORD TOKEN (%APPDATA%\\C-Cord\\token)";
  const char *tok_ph = "Paste Discord user token or press Ctrl+V...";
#elif defined(__APPLE__)
  const char *tok_hdr = "STANDALONE DISCORD TOKEN (~/Library/Application Support/C-Cord/token)";
  const char *tok_ph = "Paste Discord user token or press Cmd+V...";
#else
  const char *tok_hdr = "STANDALONE DISCORD TOKEN ($XDG_CONFIG_HOME/ccord/token)";
  const char *tok_ph = "Paste Discord user token or press Ctrl+V...";
#endif
  ccord_text_draw(r, tok_hdr, strlen(tok_hdr), tbox.x, tbox.y - 15.0f, &ls, 0.0f);
  draw_input_field(ui, r, tbox, &ui->account_token_edit, ui->account_focus_field == 1, true, tok_ph);
  st_draw_button(ui, r, st_account_token_paste_rect(g), "Paste", false);
  st_draw_button(ui, r, st_account_token_save_rect(g), "Save & Connect", true);
  st_draw_button(ui, r, st_account_token_clear_rect(g), "Sign Out", false);

  // Multi-account switcher: the app pushes the keyring roster (names only, never tokens).
  {
    ccord_text_style rls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
    ccord_text_draw(r, "SAVED ACCOUNTS", 14, x, g->content.y + 338.0f, &rls, 0.0f);
    st_draw_button(ui, r, st_account_add_btn_rect(g), "Add Account", false);
    ccord_text_style rns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_normal));
    ccord_text_style ract = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, COL(brand));
    size_t shown = ui->saved_account_count < SAVED_ACCOUNT_ROWS ? ui->saved_account_count : SAVED_ACCOUNT_ROWS;
    for (size_t i = 0; i < shown; i++) {
      const ccord_ui_saved_account *sa = &ui->saved_accounts[i];
      ccord_rect row = st_saved_account_row_rect(g, (int)i);
      uis_card(ui, r, row);
      if (sa->active) uis_list_row_fill(ui, r, row, (uis_state){.selected = true}, "sacc", (uint64_t)i);
      const char *name = sa->display_name[0] ? sa->display_name : (sa->username[0] ? sa->username : "Saved account");
      char line[160];
      snprintf(line, sizeof line, "%s%s", name, sa->active ? "  (active)" : "");
      ccord_text_draw_ellipsis(r, line, strlen(line), row.x + 8.0f,
                               row.y + (row.h - ui_line_h(ui, rns.size)) * 0.5f, row.w - 156.0f,
                               sa->active ? &ract : &rns);
      if (!sa->active) st_draw_button(ui, r, st_saved_account_switch_rect(g, (int)i), "Switch", false);
      st_draw_button(ui, r, st_saved_account_remove_rect(g, (int)i), "Remove", false);
    }
    if (ui->saved_account_count == 0) {
      ccord_text_style empty = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
      const char *msg = "No saved accounts yet. Sign in to add one to the switcher.";
      ccord_text_draw_ellipsis(r, msg, strlen(msg), x, g->content.y + 372.0f, w - 120.0f, &empty);
    } else if (ui->saved_account_count > SAVED_ACCOUNT_ROWS) {
      char more[48];
      snprintf(more, sizeof more, "+%zu more", ui->saved_account_count - SAVED_ACCOUNT_ROWS);
      ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_caption, COL(text_muted));
      ccord_text_draw(r, more, strlen(more), x, g->content.y + 372.0f + SAVED_ACCOUNT_ROWS * 40.0f + 8.0f, &ms, 0.0f);
    }
  }

  if (v->show_non_goals && v->non_goals && v->non_goal_count > 0 && v->signed_in && v->demo_chip_visible) {
    const size_t cols = 2, rows = 3;
    float ph = 26.0f + (float)rows * 17.0f + 12.0f;
    float py = g->content.y + g->content.h - 110.0f - ph;
    ccord_rect panel = {x, py, w, ph};
    uis_card(ui, r, panel);
    ccord_text_style pt = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, COL(text_header));
    ccord_text_draw_ellipsis(r, "Not available with a Discord personal sign-in",
                             strlen("Not available with a Discord personal sign-in"), panel.x + 12.0f, panel.y + 8.0f,
                             panel.w - 24.0f, &pt);
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
    float colw = (panel.w - 24.0f) / (float)cols;
    for (size_t i = 0; i < v->non_goal_count && i < cols * rows; i++) {
      float lx = panel.x + 12.0f + (float)(i / rows) * colw;
      float ly = panel.y + 26.0f + (float)(i % rows) * 17.0f;
      ccord_text_draw_ellipsis(r, v->non_goals[i], strlen(v->non_goals[i]), lx, ly, colw - 8.0f, &ps);
    }
  }

  ccord_text_style wls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
  const char *web_head = "BILLING, NITRO, SHOP & ACCOUNT (OPEN IN WEB BROWSER)";
  float web_y = st_account_web_y(ui, g);
  ccord_text_draw_ellipsis(r, web_head, strlen(web_head), x, web_y - 18.0f, w, &wls);
  for (int i = 0; i < 5; i++) {
    st_draw_button(ui, r, st_account_web_btn_rect(ui, g, i), st_k_web_labels[i], false);
  }

  ccord_rect primary = {0, 0, 0, 0}, secondary = {0, 0, 0, 0};
  st_account_buttons(ui, g, &primary, &secondary);
  if (v->signed_in) {
    if (v->can_refresh) st_draw_button(ui, r, primary, "Refresh", false);
    st_draw_button(ui, r, secondary, "Sign out", false);
  } else if (v->can_sign_in) {
    st_draw_button(ui, r, primary, "Sign in with Discord", true);
  }
}

// Secondary modal popup for 6-digit TOTP / 2FA verification (shown only when Discord requires 2FA).
void st_draw_mfa_popup(ccord_ui *ui, ccord_renderer *r)
{
  ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h}, ui_alpha(COL(bg_tertiary), 0.72f));
  ccord_rect pop = st_mfa_popup_rect(ui);
  ccord_draw_rect(r, (ccord_rect){pop.x - 1.0f, pop.y - 1.0f, pop.w + 2.0f, pop.h + 2.0f}, COL(border_subtle));
  uis_card(ui, r, pop);
  ccord_push_clip(r, pop);

  float msc = st_mfa_scroll_offset(ui, &pop);
  ccord_draw_rect(r, (ccord_rect){pop.x, pop.y, pop.w, 4.0f}, COL(brand));

  const char *title = "Two-Factor Authentication";
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_title, COL(text_header));
  ccord_vec2 tt = ccord_text_measure(r, title, strlen(title), &ts, 0.0f);
  ccord_text_draw(r, title, strlen(title), pop.x + (pop.w - tt.x) * 0.5f, pop.y + 22.0f - msc, &ts, 0.0f);

  const char *sub = "Enter the 6-digit code from your authenticator app.";
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
  ccord_vec2 st = ccord_text_measure(r, sub, strlen(sub), &ss, 0.0f);
  float sub_x = st.x < pop.w - 32.0f ? pop.x + (pop.w - st.x) * 0.5f : pop.x + 16.0f;
  ccord_text_draw_ellipsis(r, sub, strlen(sub), sub_x, pop.y + 50.0f - msc, pop.w - 32.0f, &ss);

  ccord_rect banner = {pop.x + 28.0f, pop.y + 80.0f - msc, pop.w - 56.0f, 34.0f};
  if (ui->account_login_status[0] != '\0') {
    uis_card(ui, r, banner);
    ccord_color accent = ui->account_login_is_error ? COL(danger) : COL(brand);
    ccord_draw_rect(r, (ccord_rect){banner.x, banner.y, 4.0f, banner.h}, accent);
    ccord_text_style es = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control,
                                   ui->account_login_is_error ? COL(danger) : COL(text_header));
    ccord_text_draw_ellipsis(r, ui->account_login_status, strlen(ui->account_login_status), banner.x + 12.0f,
                             banner.y + (banner.h - ui_line_h(ui, es.size)) * 0.5f, banner.w - 18.0f, &es);
  }

  ccord_rect mbox = st_mfa_popup_input_rect(&pop);
  ccord_rect vbtn = st_mfa_popup_verify_rect(&pop);
  ccord_rect bbtn = st_mfa_popup_back_rect(&pop);
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
  ccord_text_draw(r, "6-DIGIT AUTHENTICATOR CODE", 26, mbox.x, mbox.y - 16.0f, &ls, 0.0f);
  draw_input_field(ui, r, mbox, &ui->account_mfa_edit, ui->account_focus_field == 4, false, "000000");
  st_draw_button(ui, r, vbtn, "Verify & Log In", true);
  st_draw_button(ui, r, bbtn, "Back to Login", false);

  ccord_pop_clip(r);
}

void st_draw_login_screen(ccord_ui *ui, ccord_renderer *r)
{
  ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h}, COL(bg_tertiary));

  ccord_rect card = st_login_card_rect(ui);
  ccord_draw_rrect(r, (ccord_rect){card.x - 1.0f, card.y - 1.0f, card.w + 2.0f, card.h + 2.0f},
                   uis_tokens_of(ui).r_surface + 1.0f, COL(border_subtle));
  uis_card(ui, r, card);
  ccord_push_clip(r, card);

  float lsc = st_login_scroll_offset(ui, &card);
  // Top Discord brand accent bar + badge
  ccord_draw_rrect(r, (ccord_rect){card.x, card.y, card.w, 8.0f}, 4.0f, COL(brand));
  ccord_rect badge = {card.x + (card.w - 96.0f) * 0.5f, card.y + 14.0f - lsc, 96.0f, 22.0f};
  ccord_draw_rrect(r, badge, 11.0f, COL(brand));
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_caption, COL(text_on_brand));
  ccord_vec2 bt = ccord_text_measure(r, "DISCORD", 7, &bs, 0.0f);
  ccord_text_draw(r, "DISCORD", 7, badge.x + (badge.w - bt.x) * 0.5f, badge.y + (badge.h - bt.y) * 0.5f, &bs, 0.0f);

  // Header: "Welcome back!" + "We're so excited to see you again!"
  const char *title = "Welcome back!";
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_title, COL(text_header));
  ccord_vec2 tt = ccord_text_measure(r, title, strlen(title), &ts, 0.0f);
  ccord_text_draw(r, title, strlen(title), card.x + (card.w - tt.x) * 0.5f, card.y + 42.0f - lsc, &ts, 0.0f);

  const char *sub = "We're so excited to see you again!";
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
  ccord_vec2 st = ccord_text_measure(r, sub, strlen(sub), &ss, 0.0f);
  ccord_text_draw(r, sub, strlen(sub), card.x + (card.w - st.x) * 0.5f, card.y + 68.0f - lsc, &ss, 0.0f);

  // Two-tab bar: Tab 0 = Token Sign-In, Tab 1 = Email & Password
  static const char *const k_tab_labels[2] = {"Token Sign-In", "Email & Password"};
  for (int i = 0; i < 2; i++) {
    ccord_rect tr = st_login_tab_rect(&card, i);
    uis_state tst = {.hover = ui_in_rect(ui->mx, ui->my, tr)};
    uis_button(ui, r, tr, k_tab_labels[i], ui->login_tab == i ? UIS_BTN_PRIMARY : UIS_BTN_SECONDARY, UIS_BTN_S_MD,
               tst, "ltab", (uint64_t)i);
  }

  // Connecting state / Auth error banner / Info banner
  float cx = card.x + 32.0f, cw = card.w - 64.0f;
  ccord_rect banner = {cx, card.y + 136.0f - lsc, cw, 32.0f};
  bool connecting = (ui->d.session && ccord_session_state(ui->d.session) == CCORD_CONN_CONNECTING) ||
                    (ui->account_view.busy && !ui->account_view.signed_in);
  if (connecting) {
    uis_card(ui, r, banner);
    ccord_draw_rrect(r, (ccord_rect){banner.x, banner.y, 4.0f, banner.h}, 2.0f, COL(brand));
    ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, COL(text_header));
    const char *cmsg = "Connecting to Discord Gateway...";
    ccord_text_draw_ellipsis(r, cmsg, strlen(cmsg), banner.x + 12.0f,
                             banner.y + (banner.h - ui_line_h(ui, cs.size)) * 0.5f, banner.w - 20.0f, &cs);
  } else if (ui->account_login_status[0] != '\0') {
    ccord_color accent = ui->account_login_is_error ? COL(danger) : COL(brand);
    uis_card(ui, r, banner);
    ccord_draw_rrect(r, (ccord_rect){banner.x, banner.y, 4.0f, banner.h}, 2.0f, accent);
    ccord_text_style es = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control,
                                   ui->account_login_is_error ? COL(danger) : COL(text_header));
    ccord_text_draw_ellipsis(r, ui->account_login_status, strlen(ui->account_login_status), banner.x + 12.0f,
                             banner.y + (banner.h - ui_line_h(ui, es.size)) * 0.5f, banner.w - 20.0f, &es);
  } else {
    uis_card(ui, r, banner);
    ccord_text_style is = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
    const char *imsg = ui->mobile_layout ? "Sign in to connect to Discord."
                                         : "Sign in with your Discord account to connect to the live Gateway.";
    ccord_text_draw_ellipsis(r, imsg, strlen(imsg), banner.x + 10.0f,
                             banner.y + (banner.h - ui_line_h(ui, is.size)) * 0.5f, banner.w - 20.0f, &is);
  }

  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  if (ui->login_tab == 0) {
    ccord_rect tbox = st_login_token_box_rect(&card);
    ccord_text_draw(r, "DISCORD USER TOKEN", 18, tbox.x, tbox.y - 16.0f, &ls, 0.0f);
    draw_input_field(ui, r, tbox, &ui->account_token_edit, ui->account_focus_field == 1, true,
                     ui->mobile_layout ? "Paste Discord user token..." : "Paste Discord user token or press Ctrl+V...");
    st_draw_button(ui, r, st_login_token_paste_rect(&card),
                ui->mobile_layout ? "Paste from Clipboard" : "Paste from Clipboard (Ctrl+V)", false);
    st_draw_button(ui, r, st_login_token_save_rect(&card), connecting ? "Connecting to Discord Gateway..." : "Connect to Discord", true);
#if defined(_WIN32)
    const char *h1 = "Token is saved locally in your Windows user profile at:";
    const char *h2 = "%APPDATA%\\C-Cord\\token";
#elif defined(__APPLE__)
    const char *h1 = "Token is saved locally with 0600 permissions at:";
    const char *h2 = "~/Library/Application Support/C-Cord/token";
#elif defined(__ANDROID__)
    const char *h1 = "Token is saved privately in app storage.";
    const char *h2 = "Stored locally with 0600 permissions.";
#else
    const char *h1 = "Token is saved locally with 0600 permissions at:";
    const char *h2 = "$XDG_CONFIG_HOME/ccord/token";
#endif
    ccord_text_draw_ellipsis(r, h1, strlen(h1), cx, card.y + 344.0f - lsc, cw, &hs);
    ccord_text_draw_ellipsis(r, h2, strlen(h2), cx, card.y + 362.0f - lsc, cw, &ls);
  } else {
    ccord_rect em_box = st_login_email_box_rect(&card);
    ccord_rect pw_box = st_login_password_box_rect(&card);
    ccord_rect lbtn = st_login_credentials_btn_rect(&card);
    ccord_text_draw(r, "EMAIL OR PHONE NUMBER", 21, em_box.x, em_box.y - 16.0f, &ls, 0.0f);
    draw_input_field(ui, r, em_box, &ui->account_email_edit, ui->account_focus_field == 2, false, "you@example.com");
    ccord_text_draw(r, "PASSWORD", 8, pw_box.x, pw_box.y - 16.0f, &ls, 0.0f);
    draw_input_field(ui, r, pw_box, &ui->account_password_edit, ui->account_focus_field == 3, true, "Password");
    st_draw_button(ui, r, lbtn, connecting ? "Connecting to Discord Gateway..." : "Connect to Discord", true);
    const char *h3 = ui->mobile_layout ? "CAPTCHA required? Switch to Token Sign-In."
                                       : "If Discord requires a browser CAPTCHA, switch to Token Sign-In.";
    ccord_text_draw_ellipsis(r, h3, strlen(h3), cx, card.y + 372.0f - lsc, cw, &hs);
  }

  ccord_pop_clip(r);
  if (ui->account_mfa_required) {
    st_draw_mfa_popup(ui, r);
  }
}
void ccord_ui_open_account_settings(ccord_ui *ui)
{
  if (!ui) return;
  st_settings_init_defaults(ui);
  ui_open_settings(ui, true);
  ui->settings_page = SP_ACCOUNT;
  ui->settings_category = st_k_pages[SP_ACCOUNT].category;
  if (!ui->account_view.signed_in && ccord_store_self_id(ui->d.store) == CCORD_ID_NONE) {
    ui->login_screen = true;
    ui->account_focus_field = (ui->login_tab == 0) ? 1 : (ui->account_mfa_required ? 4 : 2);
  } else if (!ui->account_view.signed_in && ui->account_focus_field == 0) {
    ui->account_focus_field = 2;   // focus Email field by default
  }
}

void ccord_ui_set_login_tab(ccord_ui *ui, int tab)
{
  if (!ui) return;
  ui->login_tab = (tab == 1) ? 1 : 0;
  if (ui_is_login_screen(ui)) {
    ui->account_focus_field = (ui->login_tab == 0) ? 1 : (ui->account_mfa_required ? 4 : 2);
  }
  ui->dirty = true;
}

const char *ccord_ui_account_token_text(const ccord_ui *ui)
{
  return ui ? ccord_text_edit_cstr(&ui->account_token_edit) : "";
}

void ccord_ui_clear_account_token_text(ccord_ui *ui)
{
  if (!ui) return;
  if (ui->account_token_edit.len > 0) {
    memset(ui->account_token_edit.data, 0, ui->account_token_edit.len);
  }
  ccord_text_edit_clear(&ui->account_token_edit);
  if (ui->account_password_edit.len > 0) {
    memset(ui->account_password_edit.data, 0, ui->account_password_edit.len);
  }
  ccord_text_edit_clear(&ui->account_password_edit);
  ccord_text_edit_clear(&ui->account_mfa_edit);
  ui->account_focus_field = 0;
  ui->account_mfa_required = false;
  ui->account_login_is_error = false;
  ui->account_login_status[0] = '\0';
  ui->dirty = true;
}

void ccord_ui_account_login_fields(const ccord_ui *ui, const char **out_email,
                                   const char **out_password, const char **out_mfa_code)
{
  if (out_email) *out_email = ui ? ccord_text_edit_cstr(&ui->account_email_edit) : "";
  if (out_password) *out_password = ui ? ccord_text_edit_cstr(&ui->account_password_edit) : "";
  if (out_mfa_code) *out_mfa_code = ui ? ccord_text_edit_cstr(&ui->account_mfa_edit) : "";
}

void ccord_ui_set_account_login_field(ccord_ui *ui, int field, const char *utf8)
{
  if (!ui || !utf8) return;
  ccord_text_edit *dst = nullptr;
  if (field == 1) {
    dst = &ui->account_email_edit;
    if (ui_is_login_screen(ui)) ui->login_tab = 1;
  } else if (field == 2) {
    dst = &ui->account_password_edit;
    if (ui_is_login_screen(ui)) ui->login_tab = 1;
  } else if (field == 3) {
    dst = &ui->account_mfa_edit;
  }
  if (!dst) return;
  ccord_text_edit_clear(dst);
  if (utf8[0]) {
    ccord_error ierr = {0};
    (void)ccord_text_edit_insert(dst, utf8, strlen(utf8), &ierr);
  }
  ui->dirty = true;
}

void ccord_ui_set_login_status(ccord_ui *ui, const char *status_msg, bool mfa_required)
{
  if (!ui) return;
  bool was_mfa = ui->account_mfa_required;
  ui->account_mfa_required = mfa_required;
  ui->account_login_is_error = (!mfa_required && status_msg && status_msg[0] != '\0');
  if (status_msg && status_msg[0]) {
    snprintf(ui->account_login_status, sizeof ui->account_login_status, "%s", status_msg);
  } else {
    ui->account_login_status[0] = '\0';
  }
  if (mfa_required) {
    ui->login_tab = 1;
    ui->account_focus_field = 4;
    if (!was_mfa) {
      ccord_text_edit_clear(&ui->account_mfa_edit);
      ccord_rect pop = st_mfa_popup_rect(ui);
      ui->text_area = st_mfa_popup_input_rect(&pop);
      if (ui->touch_mode) ui->touch_text_focused = true;
    }
  }
  ui->dirty = true;
}

void ccord_ui_set_login_busy(ccord_ui *ui, const char *status_msg)
{
  if (!ui) return;
  ui->account_login_is_error = false;
  if (status_msg && status_msg[0]) {
    snprintf(ui->account_login_status, sizeof ui->account_login_status, "%s", status_msg);
  } else {
    ui->account_login_status[0] = '\0';
  }
  ui->dirty = true;
}

void ccord_ui_set_login_error(ccord_ui *ui, const char *status_msg, bool keep_mfa)
{
  if (!ui) return;
  ui->account_mfa_required = keep_mfa;
  ui->account_login_is_error = true;
  if (status_msg && status_msg[0]) {
    snprintf(ui->account_login_status, sizeof ui->account_login_status, "%s", status_msg);
  } else {
    ui->account_login_status[0] = '\0';
  }
  if (keep_mfa) ui->account_focus_field = 4;
  ui->dirty = true;
}
