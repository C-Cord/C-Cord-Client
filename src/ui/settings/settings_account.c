// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "settings_internal.h"

#include <ccord/net/account_edit.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>

// ---- Signed-in My Account Sub-view & Hit-list State ----

enum acct_subview {
  ACCT_SUB_OVERVIEW = 0,
  ACCT_SUB_STANDING = 1,
  ACCT_SUB_MFA = 2,
};

typedef enum acct_hit_kind : uint8_t {
  AH_NONE = 0,
  AH_SUBTAB_OVERVIEW,
  AH_SUBTAB_STANDING,
  AH_BACK_OVERVIEW,
  AH_EDIT_PROFILE,
  AH_EDIT_USERNAME,
  AH_EDIT_EMAIL,
  AH_REVEAL_EMAIL,
  AH_RESEND_VERIFY,
  AH_EDIT_PHONE,
  AH_REVEAL_PHONE,
  AH_REMOVE_PHONE,
  AH_COPY_USER_ID,
  AH_REFRESH,
  AH_SIGN_OUT,
  AH_CHANGE_PASSWORD,
  AH_OPEN_MFA,
  AH_VIEW_BACKUP_CODES,
  AH_TOTP_ENABLE,
  AH_TOTP_DISABLE,
  AH_SMS_TOGGLE,
  AH_WEBAUTHN_DELETE,
  AH_WEBAUTHN_WEB,
  AH_OPEN_DEVICES,
  AH_OPEN_STANDING,
  AH_REFRESH_STANDING,
  AH_OPEN_PRIVACY,
  AH_DISABLE_ACCOUNT,
  AH_DELETE_ACCOUNT,
  AH_ADD_ACCOUNT,
  AH_SAVED_SWITCH,
  AH_SAVED_REMOVE,
  AH_TOKEN_TOGGLE,
  AH_TOKEN_BOX,
  AH_TOKEN_PASTE,
  AH_TOKEN_SAVE,
  AH_TOKEN_CLEAR,
  AH_WEB_BTN,
} acct_hit_kind;

#define ACCT_HITS_MAX 96u
#define ACCT_REVEAL_MS 30000u

typedef struct acct_hit {
  acct_hit_kind kind;
  int arg;
  char str_arg[64];
  ccord_rect rect;
} acct_hit;

static struct {
  int subview;
  bool token_expanded;
  uint64_t reveal_email_until_ms;
  uint64_t reveal_phone_until_ms;
  bool standing_fetched;
  bool webauthn_fetched;
  bool resend_pending;
  int webauthn_confirm_idx;
  float content_h;
  ccord_rect clip_rect;
  acct_hit hits[ACCT_HITS_MAX];
  size_t nhits;
  // Cached debug rects when signed_in == true
  bool has_cached;
  ccord_rect web_btn[5];
  ccord_rect token_box, token_paste, token_save, token_clear;
  ccord_rect add_btn;
  ccord_rect saved_row[SAVED_ACCOUNT_ROWS];
  ccord_rect saved_switch[SAVED_ACCOUNT_ROWS];
  ccord_rect saved_remove[SAVED_ACCOUNT_ROWS];
} A = {.webauthn_confirm_idx = -1};

static void acct_add_hit(acct_hit_kind kind, int arg, const char *str_arg, ccord_rect rc)
{
  if (A.nhits >= ACCT_HITS_MAX) return;
  acct_hit *h = &A.hits[A.nhits++];
  h->kind = kind;
  h->arg = arg;
  snprintf(h->str_arg, sizeof h->str_arg, "%s", str_arg ? str_arg : "");
  h->rect = rc;
}

static void acct_mask_email(const char *email, char *out, size_t cap)
{
  if (!email || !email[0]) {
    snprintf(out, cap, "None");
    return;
  }
  const char *at = strchr(email, '@');
  if (!at || at == email) {
    snprintf(out, cap, "••••••");
    return;
  }
  snprintf(out, cap, "%c••••••%s", email[0], at);
}

static void acct_mask_phone(const char *phone, char *out, size_t cap)
{
  if (!phone || !phone[0]) {
    snprintf(out, cap, "You haven't added a phone number yet.");
    return;
  }
  size_t len = strlen(phone);
  if (len <= 4) {
    snprintf(out, cap, "••••");
    return;
  }
  snprintf(out, cap, "••••••%s", phone + len - 4);
}

static void acct_draw_pill(ccord_ui *ui, ccord_renderer *r, float x, float y, const char *text,
                           ccord_color fg, ccord_color bg, float *out_w)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, fg);
  float pw = ccord_text_measure(r, text, strlen(text), &ps, 0.0f).x + 14.0f;
  ccord_rect rc = {x, y, pw, 18.0f};
  uis_plate(ui, r, rc, tk.r_control, bg);
  ccord_text_draw(r, text, strlen(text), rc.x + 7.0f,
                  rc.y + (rc.h - ui_line_h(ui, ps.size)) * 0.5f, &ps, 0.0f);
  if (out_w) *out_w = pw;
}

static ccord_color acct_rgb(uint32_t v)
{
  return (ccord_color){(uint8_t)((v >> 16) & 0xFFu), (uint8_t)((v >> 8) & 0xFFu), (uint8_t)(v & 0xFFu), 255};
}

// ---- Security surface, lifecycle & tick hooks ----

bool st_account_security_active(const ccord_ui *ui)
{
  if (!ui) return false;
  if (st_account_modal_open()) return true;
  if (ui->settings_open && ui->settings_page == SP_ACCOUNT) {
    if (A.subview == ACCT_SUB_MFA) return true;
    if (A.reveal_email_until_ms != 0 || A.reveal_phone_until_ms != 0) return true;
  }
  return false;
}

bool ui_settings_account_wants_text(const ccord_ui *ui)
{
  if (!ui || !ui->settings_open || ui->settings_page != SP_ACCOUNT) return false;
  if (st_account_modal_open()) return st_account_modal_wants_text();
  return ui->account_focus_field != 0;
}

bool ui_settings_account_has_overlay(const ccord_ui *ui)
{
  if (!ui || !ui->settings_open || ui->settings_page != SP_ACCOUNT) return false;
  return st_account_modal_open();
}

bool ui_settings_account_dismiss_overlay(ccord_ui *ui)
{
  if (!ui || ui->settings_page != SP_ACCOUNT) return false;
  if (st_account_modal_open()) {
    st_account_modal_close(ui);
    return true;
  }
  if (A.subview != ACCT_SUB_OVERVIEW) {
    A.subview = ACCT_SUB_OVERVIEW;
    A.webauthn_confirm_idx = -1;
    st_s_page_scroll = 0.0f;
    ui->dirty = true;
    return true;
  }
  return false;
}

bool st_account_handle_back(ccord_ui *ui)
{
  if (st_account_modal_open()) {
    st_account_modal_close(ui);
    return true;
  }
  if (A.subview != ACCT_SUB_OVERVIEW) {
    A.subview = ACCT_SUB_OVERVIEW;
    A.webauthn_confirm_idx = -1;
    st_s_page_scroll = 0.0f;
    ui->dirty = true;
    return true;
  }
  return false;
}

void st_account_release(ccord_ui *ui)
{
  st_account_modal_close(ui);
  A.subview = ACCT_SUB_OVERVIEW;
  A.reveal_email_until_ms = 0;
  A.reveal_phone_until_ms = 0;
  A.standing_fetched = false;
  A.webauthn_fetched = false;
  A.webauthn_confirm_idx = -1;
  A.has_cached = false;
}

void ui_settings_account_release(ccord_ui *ui) { st_account_release(ui); }

bool st_account_tick(ccord_ui *ui)
{
  if (!ui) return false;
  if (!ui->settings_open || ui->settings_page != SP_ACCOUNT) {
    if (st_account_modal_open() || A.reveal_email_until_ms != 0 || A.reveal_phone_until_ms != 0) {
      st_account_release(ui);
    }
    return false;
  }
  if (ui->settings_streamer_mode && (A.reveal_email_until_ms != 0 || A.reveal_phone_until_ms != 0)) {
    A.reveal_email_until_ms = 0;
    A.reveal_phone_until_ms = 0;
    ui->dirty = true;
  }
  if (A.reveal_email_until_ms != 0 && ui->now_ms >= A.reveal_email_until_ms) {
    A.reveal_email_until_ms = 0;
    ui->dirty = true;
  }
  if (A.reveal_phone_until_ms != 0 && ui->now_ms >= A.reveal_phone_until_ms) {
    A.reveal_phone_until_ms = 0;
    ui->dirty = true;
  }
  return A.reveal_email_until_ms != 0 || A.reveal_phone_until_ms != 0;
}

float st_account_content_height(const ccord_ui *ui, const sgeom *g)
{
  (void)ui;
  (void)g;
  return A.content_h > 0.0f ? A.content_h : 640.0f;
}

bool st_account_cached_web_btn(const ccord_ui *ui, int idx, ccord_rect *out)
{
  if (!ui || !ui->account_view.signed_in || !A.has_cached || idx < 0 || idx >= 5 || !out) return false;
  if (A.web_btn[idx].w <= 0.0f) return false;
  *out = A.web_btn[idx];
  return true;
}

void st_account_fill_debug_rects(const ccord_ui *ui, ccord_ui_settings_debug *out)
{
  if (!ui || !out || !ui->account_view.signed_in || !A.has_cached) return;
  if (A.web_btn[0].w > 0.0f) out->billing_btn = A.web_btn[0];
  if (A.web_btn[1].w > 0.0f) out->nitro_btn = A.web_btn[1];
  if (A.web_btn[2].w > 0.0f) out->shop_btn = A.web_btn[2];
  if (A.web_btn[3].w > 0.0f) out->boost_btn = A.web_btn[3];
  if (A.add_btn.w > 0.0f) out->account_add_btn = A.add_btn;
  for (size_t i = 0; i < SAVED_ACCOUNT_ROWS; i++) {
    if (A.saved_row[i].w > 0.0f) {
      out->saved_account_rows[i] = A.saved_row[i];
      out->saved_account_switch[i] = A.saved_switch[i];
      out->saved_account_remove[i] = A.saved_remove[i];
    }
  }
  if (A.token_box.w > 0.0f) {
    out->account_token_box = A.token_box;
    out->account_token_paste_btn = A.token_paste;
    out->account_token_save_btn = A.token_save;
    out->account_token_clear_btn = A.token_clear;
  }
}

// ---- Shared text editing & Login MFA popup (preserved verbatim) ----

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
  float bh = uis_btn_h_for(ui, UIS_H_BTN);
  float wy = st_account_web_y(ui, g);
  float extra_web = g->content.w < 380.0f ? (bh + 6.0f) : 0.0f;
  float min_by = wy + bh + extra_web + 6.0f;
  float bot_pad = (ui && ui->mobile_layout) ? 48.0f : 42.0f;
  float bot_by = g->content.y + g->content.h - bot_pad;
  float by = bot_by > min_by ? bot_by : min_by;
  float pw = fminf(156.0f, fmaxf((g->content.w - 12.0f) * 0.54f, 96.0f));
  float sw = fminf(132.0f, fmaxf(g->content.w - pw - 12.0f, 84.0f));
  *primary = (ccord_rect){g->content.x, by, pw, bh};
  *secondary = (ccord_rect){g->content.x + pw + 12.0f, by, sw, bh};
}

void st_draw_button(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, bool primary)
{
  if (rc.w <= 0.0f) return;
  uis_btn_size size = rc.h <= UIS_H_BTN_SM ? UIS_BTN_S_SM : rc.h >= UIS_H_BTN_LG ? UIS_BTN_S_LG : UIS_BTN_S_MD;
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, rc), .pressed = st_settings_pressed(ui, rc)};
  uint64_t id = (uint64_t)(rc.y * 4096.0f + rc.x);
  uis_button(ui, r, rc, label, primary ? UIS_BTN_PRIMARY : UIS_BTN_SECONDARY, size, st, "sbtn", id);
}

static void draw_danger_btn(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, bool filled, bool disabled)
{
  if (rc.w <= 0.0f) return;
  uis_btn_size size = rc.h <= UIS_H_BTN_SM ? UIS_BTN_S_SM : rc.h >= UIS_H_BTN_LG ? UIS_BTN_S_LG : UIS_BTN_S_MD;
  uis_state st = {.hover = !disabled && ui_in_rect(ui->mx, ui->my, rc), .disabled = disabled};
  uint64_t id = (uint64_t)(rc.y * 4096.0f + rc.x);
  uis_button(ui, r, rc, label, filled ? UIS_BTN_DANGER : UIS_BTN_SECONDARY, size, st, "sdbtn", id);
}

static void draw_input_field(ccord_ui *ui, ccord_renderer *r, ccord_rect box, const ccord_text_edit *ed,
                             bool focused, bool masked, const char *placeholder)
{
  uis_state st = {.focused = focused};
  uis_input(ui, r, box, ed, placeholder, st, masked, "sfield", (uint64_t)(box.y * 4096.0f + box.x));
}

// ---- Async quick callbacks for Resend Verify / Standing / WebAuthn ----

static void acct_on_resend_done(void *ud, const ccord_account_result *res)
{
  ccord_ui *ui = ud;
  A.resend_pending = false;
  if (!ui) return;
  if (res && res->status == CCORD_OK) {
    ui_notice(ui, "Verification email resent. Check your inbox.");
  } else {
    ui_notice(ui, "%s", (res && res->message[0]) ? res->message : "Could not resend verification email.");
  }
  ui->dirty = true;
}

static void acct_on_fetch_done(void *ud, const ccord_account_result *res)
{
  ccord_ui *ui = ud;
  if (!ui) return;
  if (res && res->status != CCORD_OK && res->message[0]) {
    ui_notice(ui, "%s", res->message);
  }
  ui->dirty = true;
}

static void acct_on_webauthn_del_done(void *ud, const ccord_account_result *res)
{
  ccord_ui *ui = ud;
  A.webauthn_confirm_idx = -1;
  if (!ui) return;
  if (res && res->status == CCORD_OK) {
    ui_notice(ui, "Security key removed.");
  } else {
    ui_notice(ui, "%s", (res && res->message[0]) ? res->message : "Could not remove security key.");
  }
  ui->dirty = true;
}

// ---- Signed-in My Account Drawing ----

static const char *standing_tier_label(ccord_account_standing_tier t)
{
  switch (t) {
    case CCORD_STANDING_ALL_GOOD: return "All Good";
    case CCORD_STANDING_LIMITED: return "Limited";
    case CCORD_STANDING_VERY_LIM: return "Very Limited";
    case CCORD_STANDING_AT_RISK: return "At Risk";
    case CCORD_STANDING_SUSPENDED: return "Suspended";
    default: return "All Good";
  }
}

static ccord_color standing_tier_color(const ccord_ui *ui, ccord_account_standing_tier t)
{
  switch (t) {
    case CCORD_STANDING_ALL_GOOD: return COL(status_online);
    case CCORD_STANDING_LIMITED: return COL(status_idle);
    case CCORD_STANDING_VERY_LIM: return (ccord_color){245, 158, 11, 255};
    case CCORD_STANDING_AT_RISK: return COL(danger);
    case CCORD_STANDING_SUSPENDED: return COL(danger);
    default: return COL(status_online);
  }
}

static float draw_detail_row(ccord_ui *ui, ccord_renderer *r, float x, float y, float w,
                             const char *label, const char *value,
                             const char *pill_txt, ccord_color pill_fg,
                             const char *b1_lbl, acct_hit_kind b1_kind, bool b1_disabled,
                             const char *b2_lbl, acct_hit_kind b2_kind, bool b2_disabled,
                             const char *b3_lbl, acct_hit_kind b3_kind, bool b3_disabled)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
  ccord_text_style vs = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_body, COL(text_normal));

  float bh = uis_btn_h_for(ui, UIS_H_BTN_SM);
  float row_h = ui->mobile_layout ? 64.0f : 52.0f;
  float btn_right = x + w;
  float by = y + (row_h - bh) * 0.5f;
  uis_btn_size bsz = ui->mobile_layout ? UIS_BTN_S_MD : UIS_BTN_S_SM;

  // Draw up to 3 right-aligned compact buttons (b3 rightmost, then b2, then b1)
  if (b3_lbl) {
    float bw = fmaxf(uis_label_width(ui, r, b3_lbl), 58.0f);
    btn_right -= bw;
    ccord_rect rc = {btn_right, by, bw, bh};
    uis_button(ui, r, rc, b3_lbl, UIS_BTN_SECONDARY, bsz,
               (uis_state){.hover = !b3_disabled && ui_in_rect(ui->mx, ui->my, rc), .disabled = b3_disabled},
               "adr", (uint64_t)b3_kind);
    if (!b3_disabled) acct_add_hit(b3_kind, 0, nullptr, rc);
    btn_right -= UIS_SP_2;
  }
  if (b2_lbl) {
    float bw = fmaxf(uis_label_width(ui, r, b2_lbl), 58.0f);
    btn_right -= bw;
    ccord_rect rc = {btn_right, by, bw, bh};
    uis_button(ui, r, rc, b2_lbl, UIS_BTN_SECONDARY, bsz,
               (uis_state){.hover = !b2_disabled && ui_in_rect(ui->mx, ui->my, rc), .disabled = b2_disabled},
               "adr", (uint64_t)b2_kind);
    if (!b2_disabled) acct_add_hit(b2_kind, 0, nullptr, rc);
    btn_right -= UIS_SP_2;
  }
  if (b1_lbl) {
    float bw = fmaxf(uis_label_width(ui, r, b1_lbl), 58.0f);
    btn_right -= bw;
    ccord_rect rc = {btn_right, by, bw, bh};
    uis_button(ui, r, rc, b1_lbl, UIS_BTN_SECONDARY, bsz,
               (uis_state){.hover = !b1_disabled && ui_in_rect(ui->mx, ui->my, rc), .disabled = b1_disabled},
               "adr", (uint64_t)b1_kind);
    if (!b1_disabled) acct_add_hit(b1_kind, 0, nullptr, rc);
    btn_right -= UIS_SP_2;
  }

  float avail = fmaxf(btn_right - x - UIS_SP_2, 80.0f);
  ccord_text_draw(r, label, strlen(label), x, y + (ui->mobile_layout ? 10.0f : 6.0f), &ls, 0.0f);
  if (pill_txt && pill_txt[0]) {
    float lw = ccord_text_measure(r, label, strlen(label), &ls, 0.0f).x;
    acct_draw_pill(ui, r, x + lw + 8.0f, y + (ui->mobile_layout ? 7.0f : 3.0f), pill_txt, pill_fg, ui_alpha(pill_fg, 0.16f), nullptr);
  }
  ccord_text_draw_ellipsis(r, value, strlen(value), x, y + (ui->mobile_layout ? 30.0f : 25.0f), avail, &vs);
  return row_h;
}

static void draw_signed_in_standing(ccord_ui *ui, ccord_renderer *r, float x, float *io_y, float w)
{
  uis_tokens tk = uis_tokens_of(ui);
  float y = *io_y;
  const ccord_account_standing *st = ui->d.session ? ccord_session_account_standing(ui->d.session) : nullptr;

  if (!A.standing_fetched && ui->d.session) {
    A.standing_fetched = true;
    ccord_error err = {0};
    (void)ccord_session_account_fetch_standing(ui->d.session, acct_on_fetch_done, ui, &err);
  }

  ccord_account_standing_tier tier = (st && st->loaded) ? st->tier : CCORD_STANDING_ALL_GOOD;
  ccord_color tcol = standing_tier_color(ui, tier);

  // Standing Hero Card
  float hero_h = 154.0f;
  ccord_rect card = {x, y, w, hero_h};
  uis_card(ui, r, card);

  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_title, COL(text_header));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_secondary));
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));

  float bh = uis_btn_h_for(ui, UIS_H_BTN_SM);
  char title[96];
  snprintf(title, sizeof title, "Your Account is %s", standing_tier_label(tier));
  float ref_w = 104.0f;
  ccord_rect ref_btn = {card.x + card.w - ref_w - 16.0f, card.y + 14.0f, ref_w, bh};
  ccord_text_draw_ellipsis(r, title, strlen(title), card.x + 16.0f, card.y + 16.0f, card.w - ref_w - 40.0f, &hs);
  st_draw_button(ui, r, ref_btn, "Refresh", false);
  acct_add_hit(AH_REFRESH_STANDING, 0, nullptr, ref_btn);

  const char *desc = tier == CCORD_STANDING_ALL_GOOD
                         ? "Thanks for upholding Discord's Terms of Service and Community Guidelines."
                         : "Your account has active safety restrictions. Review any violations below.";
  ccord_text_draw_ellipsis(r, desc, strlen(desc), card.x + 16.0f, card.y + 44.0f, card.w - 32.0f, &ss);

  // 5-segment standing meter
  static const char *const k_segs[5] = {"All Good", "Limited", "Very Limited", "At Risk", "Suspended"};
  float seg_gap = 6.0f;
  float seg_w = (card.w - 32.0f - seg_gap * 4.0f) / 5.0f;
  float seg_y = card.y + 78.0f;
  int active_seg = (int)tier;
  if (active_seg < 0) active_seg = 0;
  if (active_seg > 4) active_seg = 4;
  for (int i = 0; i < 5; i++) {
    ccord_rect sr = {card.x + 16.0f + (float)i * (seg_w + seg_gap), seg_y, seg_w, 10.0f};
    ccord_color sc = (i <= active_seg) ? tcol : COL(bg_tertiary);
    uis_plate(ui, r, sr, tk.r_control, sc);
    ccord_text_style sls = ui_style(ui, i == active_seg ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR,
                                    tk.fs_overline, i == active_seg ? tcol : COL(text_muted));
    ccord_text_draw_ellipsis(r, k_segs[i], strlen(k_segs[i]), sr.x, seg_y + 16.0f, seg_w, &sls);
  }

  y += hero_h + UIS_SP_4;

  // Violations List Card
  ccord_text_draw(r, "ACTIVE VIOLATIONS", 17, x, y, &ls, 0.0f);
  y += 20.0f;
  if (!st || !st->loaded || st->violation_count == 0) {
    ccord_rect vc = {x, y, w, 64.0f};
    uis_card(ui, r, vc);
    ccord_draw_rect(r, (ccord_rect){vc.x, vc.y + 8.0f, 3.0f, vc.h - 16.0f}, COL(status_online));
    ccord_text_style ok_st = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
    ccord_text_draw_ellipsis(r, "No active violations", 20, vc.x + 16.0f, vc.y + 12.0f, vc.w - 32.0f, &ok_st);
    ccord_text_draw_ellipsis(r, "Your account standing is clean with no active penalties.", 55,
                             vc.x + 16.0f, vc.y + 34.0f, vc.w - 32.0f, &ss);
    y += 64.0f + UIS_SP_4;
  } else {
    for (size_t i = 0; i < st->violation_count; i++) {
      const ccord_standing_violation *v = &st->violations[i];
      ccord_rect vc = {x, y, w, 78.0f};
      uis_card(ui, r, vc);
      ccord_draw_rect(r, (ccord_rect){vc.x, vc.y + 8.0f, 3.0f, vc.h - 16.0f}, COL(danger));
      ccord_text_style vt = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
      ccord_text_draw_ellipsis(r, v->title[0] ? v->title : "Policy Violation",
                               strlen(v->title[0] ? v->title : "Policy Violation"),
                               vc.x + 16.0f, vc.y + 12.0f, vc.w - 32.0f, &vt);
      if (v->detail[0]) {
        ccord_text_draw_ellipsis(r, v->detail, strlen(v->detail),
                                 vc.x + 16.0f, vc.y + 34.0f, vc.w - 32.0f, &ss);
      }
      if (v->expires_at_ms > 0) {
        char when[64] = {0};
        ccord_timefmt_message(when, sizeof when, v->expires_at_ms, ui->now_unix_ms, ui->use_utc);
        char exp[96];
        snprintf(exp, sizeof exp, "Expires: %s", when);
        ccord_text_draw_ellipsis(r, exp, strlen(exp), vc.x + 16.0f, vc.y + 54.0f, vc.w - 32.0f, &ls);
      }
      y += 78.0f + UIS_SP_2;
    }
    y += UIS_SP_2;
  }

  // Privacy & Data cross-link card
  float pc_h = ui->mobile_layout ? 76.0f : 66.0f;
  ccord_rect pc = {x, y, w, pc_h};
  uis_card(ui, r, pc);
  float pbw = 152.0f;
  ccord_rect pbtn = {pc.x + pc.w - pbw - 14.0f, pc.y + (pc.h - bh) * 0.5f, pbw, bh};
  ccord_text_style pt = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
  ccord_text_draw_ellipsis(r, "Privacy & Your Data", 19, pc.x + 16.0f, pc.y + (ui->mobile_layout ? 16.0f : 12.0f),
                           pbtn.x - pc.x - 24.0f, &pt);
  ccord_text_draw_ellipsis(r, "Manage DM filters, profile privacy, and Request My Data.", 55,
                           pc.x + 16.0f, pc.y + (ui->mobile_layout ? 38.0f : 34.0f), pbtn.x - pc.x - 24.0f, &ss);
  st_draw_button(ui, r, pbtn, "Privacy & Safety ->", false);
  acct_add_hit(AH_OPEN_PRIVACY, 0, nullptr, pbtn);
  y += pc_h + UIS_SP_4;

  *io_y = y;
}

static void draw_signed_in_mfa(ccord_ui *ui, ccord_renderer *r, float x, float *io_y, float w)
{
  uis_tokens tk = uis_tokens_of(ui);
  float y = *io_y;
  float bh = uis_btn_h_for(ui, UIS_H_BTN_SM);
  const ccord_account_private *ap = ui->d.session ? ccord_session_account_private(ui->d.session) : nullptr;
  bool mfa_on = ap && ap->valid && ap->mfa_enabled;
  bool totp_on = ap && ap->valid && ((ap->authenticator_types & CCORD_AUTH_TYPE_TOTP) != 0 || ap->mfa_enabled);
  bool sms_on = ap && ap->valid && ((ap->authenticator_types & CCORD_AUTH_TYPE_SMS) != 0 || ap->sms_mfa_enabled);
  bool has_phone = ap && ap->valid && ap->phone[0] != '\0';

  if (!A.webauthn_fetched && ui->d.session) {
    A.webauthn_fetched = true;
    ccord_error err = {0};
    (void)ccord_session_account_fetch_webauthn(ui->d.session, acct_on_fetch_done, ui, &err);
  }

  // Back header
  ccord_rect back_btn = {x, y, 160.0f, bh};
  st_draw_button(ui, r, back_btn, "< Back to My Account", false);
  acct_add_hit(AH_BACK_OVERVIEW, 0, nullptr, back_btn);
  y += bh + UIS_SP_4;

  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_title, COL(text_header));
  ccord_text_style bt = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_secondary));
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));

  ccord_text_draw(r, "Multi-Factor Authentication", 27, x, y, &hs, 0.0f);
  y += ui_line_h(ui, hs.size) + UIS_SP_3;

  // 1. Authenticator App (TOTP) Card
  float totp_h = ui->mobile_layout ? 118.0f : 108.0f;
  ccord_rect tc = {x, y, w, totp_h};
  uis_card(ui, r, tc);
  ccord_text_draw(r, "Authenticator App (TOTP)", 24, tc.x + 16.0f, tc.y + 14.0f, &bt, 0.0f);
  float tw = ccord_text_measure(r, "Authenticator App (TOTP)", 24, &bt, 0.0f).x;
  acct_draw_pill(ui, r, tc.x + 24.0f + tw, tc.y + 14.0f,
                 totp_on ? "Active" : "Not Set Up",
                 totp_on ? COL(status_online) : COL(text_muted),
                 ui_alpha(totp_on ? COL(status_online) : COL(text_muted), 0.16f), nullptr);
  const char *tdesc = totp_on
                          ? "Your account is protected by a 6-digit TOTP authenticator app."
                          : "Use an authenticator app (Aegis, Authy, 1Password, Google Authenticator) for 2FA codes.";
  ccord_text_draw_ellipsis(r, tdesc, strlen(tdesc), tc.x + 16.0f, tc.y + 38.0f, tc.w - 32.0f, &ss);
  float bx = tc.x + 16.0f;
  float by = tc.y + 66.0f;
  if (!totp_on) {
    ccord_rect eb = {bx, by, 192.0f, bh};
    st_draw_button(ui, r, eb, "Set Up Authenticator App", true);
    acct_add_hit(AH_TOTP_ENABLE, 0, nullptr, eb);
  } else {
    ccord_rect bc = {bx, by, 152.0f, bh};
    ccord_rect db = {bx + 160.0f, by, 126.0f, bh};
    st_draw_button(ui, r, bc, "View Backup Codes", false);
    acct_add_hit(AH_VIEW_BACKUP_CODES, 0, nullptr, bc);
    draw_danger_btn(ui, r, db, "Disable 2FA", true, false);
    acct_add_hit(AH_TOTP_DISABLE, 0, nullptr, db);
  }
  y += totp_h + UIS_SP_3;

  // 2. SMS Backup Authentication Card
  float sms_h = ui->mobile_layout ? 94.0f : 84.0f;
  ccord_rect sc = {x, y, w, sms_h};
  uis_card(ui, r, sc);
  float sbw = 136.0f;
  ccord_rect sbtn = {sc.x + sc.w - sbw - 14.0f, sc.y + (sc.h - bh) * 0.5f, sbw, bh};
  ccord_text_draw_ellipsis(r, "SMS Backup Authentication", 25, sc.x + 16.0f, sc.y + 14.0f, sbtn.x - sc.x - 24.0f, &bt);
  const char *sdesc = (!mfa_on || !has_phone)
                          ? "Enable Authenticator 2FA and add a verified phone number to use SMS backup."
                          : (sms_on ? "SMS backup codes are enabled on your phone." : "Receive fallback codes via SMS.");
  ccord_text_draw(r, sdesc, strlen(sdesc), sc.x + 16.0f, sc.y + 38.0f, &ss, fmaxf(sbtn.x - sc.x - 24.0f, 80.0f));
  bool sms_disabled = (!mfa_on || !has_phone);
  uis_button(ui, r, sbtn, sms_on ? "Disable SMS" : "Enable SMS", UIS_BTN_SECONDARY,
             ui->mobile_layout ? UIS_BTN_S_MD : UIS_BTN_S_SM,
             (uis_state){.hover = !sms_disabled && ui_in_rect(ui->mx, ui->my, sbtn), .disabled = sms_disabled},
             "asms", 1);
  if (!sms_disabled) acct_add_hit(AH_SMS_TOGGLE, sms_on ? 0 : 1, nullptr, sbtn);
  y += sms_h + UIS_SP_3;

  // 3. Security Keys & Passkeys (WebAuthn) Card
  size_t nkeys = 0;
  bool keys_loaded = false;
  const ccord_webauthn_cred *keys = ui->d.session
                                        ? ccord_session_account_webauthn(ui->d.session, &nkeys, &keys_loaded)
                                        : nullptr;
  ccord_text_draw(r, "SECURITY KEYS & PASSKEYS (WEBAUTHN)", 36, x, y, &ls, 0.0f);
  y += 20.0f;
  float krow_h = ui->mobile_layout ? 48.0f : 40.0f;
  float krow_step = ui->mobile_layout ? 56.0f : 48.0f;
  float klist_h = (ui->mobile_layout ? 94.0f : 84.0f) + (float)(nkeys > 0 ? nkeys : 1u) * krow_step;
  ccord_rect kc = {x, y, w, klist_h};
  uis_card(ui, r, kc);
  float ky = kc.y + 14.0f;
  if (!keys_loaded) {
    ccord_text_draw(r, "Loading registered security keys...", 35, kc.x + 16.0f, ky + 6.0f, &ss, kc.w - 32.0f);
    ky += krow_step;
  } else if (nkeys == 0) {
    ccord_text_draw(r, "No hardware security keys or passkeys registered.", 49, kc.x + 16.0f, ky + 6.0f, &ss, kc.w - 32.0f);
    ky += krow_step;
  } else {
    for (size_t i = 0; i < nkeys; i++) {
      ccord_rect krow = {kc.x + 12.0f, ky, kc.w - 24.0f, krow_h};
      uis_plate(ui, r, krow, tk.r_control, COL(bg_primary));
      bool armed = A.webauthn_confirm_idx == (int)i;
      float rbw = armed ? 92.0f : 78.0f;
      ccord_rect rbtn = {krow.x + krow.w - rbw - 8.0f, krow.y + (krow.h - bh) * 0.5f, rbw, bh};
      const char *kn = keys[i].name[0] ? keys[i].name : "Security Key";
      ccord_text_draw_ellipsis(r, kn, strlen(kn), krow.x + 12.0f,
                               krow.y + (krow.h - ui_line_h(ui, bt.size)) * 0.5f,
                               fmaxf(rbtn.x - krow.x - 20.0f, 60.0f), &bt);
      draw_danger_btn(ui, r, rbtn, armed ? "Confirm" : "Remove", armed, false);
      acct_add_hit(AH_WEBAUTHN_DELETE, (int)i, keys[i].id, rbtn);
      ky += krow_step;
    }
  }
  uis_divider(ui, r, kc.x + 16.0f, ky, kc.w - 32.0f);
  ky += 12.0f;
  float wbw = 196.0f;
  ccord_rect wbtn = {kc.x + kc.w - wbw - 14.0f, ky + 4.0f, wbw, bh};
  const char *wnote = "Enrolling a new FIDO2 Passkey uses browser WebAuthn.";
  ccord_text_draw(r, wnote, strlen(wnote), kc.x + 16.0f, ky + 6.0f, &ss, fmaxf(wbtn.x - kc.x - 24.0f, 80.0f));
  st_draw_button(ui, r, wbtn, "Add Key on discord.com", false);
  acct_add_hit(AH_WEBAUTHN_WEB, 0, nullptr, wbtn);
  y += klist_h + UIS_SP_4;

  *io_y = y;
}

static void draw_signed_in_account(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  const ccord_account_view *v = &ui->account_view;
  const ccord_user *self = ui->d.store ? ccord_store_user(ui->d.store, ccord_store_self_id(ui->d.store)) : nullptr;
  const ccord_user_profile *prof = (ui->d.store && self) ? ccord_store_user_profile(ui->d.store, self->id) : nullptr;
  const ccord_account_private *ap = ui->d.session ? ccord_session_account_private(ui->d.session) : nullptr;
  const ccord_account_standing *st = ui->d.session ? ccord_session_account_standing(ui->d.session) : nullptr;

  uis_tokens tk = uis_tokens_of(ui);
  float x = g->content.x;
  float w = g->content.w;
  float y = g->content.y + 42.0f;
  float start_y = y;
  bool compact = w < 440.0f;

  A.nhits = 0;
  A.has_cached = true;
  memset(A.web_btn, 0, sizeof A.web_btn);
  memset(A.saved_row, 0, sizeof A.saved_row);
  memset(A.saved_switch, 0, sizeof A.saved_switch);
  memset(A.saved_remove, 0, sizeof A.saved_remove);
  A.token_box = A.token_paste = A.token_save = A.token_clear = A.add_btn = (ccord_rect){0};

  float bh_sm = uis_btn_h_for(ui, UIS_H_BTN_SM);
  float bh = uis_btn_h_for(ui, UIS_H_BTN);

  // Top Sub-tab bar (Security | Standing) when not inside MFA sub-view
  if (A.subview != ACCT_SUB_MFA) {
    float t1w = fmaxf(uis_label_width(ui, r, "Security"), 92.0f);
    float t2w = fmaxf(uis_label_width(ui, r, "Standing"), 92.0f);
    float sub_h = ui->mobile_layout ? UIS_MOB_H_CHIP : (UIS_H_CHIP + 4.0f);
    ccord_rect r1 = {x, y, t1w, sub_h};
    ccord_rect r2 = {x + t1w + UIS_SP_2, y, t2w, sub_h};
    uis_chip(ui, r, r1, "Security",
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, r1), .selected = A.subview == ACCT_SUB_OVERVIEW},
             "asub", 0);
    uis_chip(ui, r, r2, "Standing",
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, r2), .selected = A.subview == ACCT_SUB_STANDING},
             "asub", 1);
    acct_add_hit(AH_SUBTAB_OVERVIEW, 0, nullptr, r1);
    acct_add_hit(AH_SUBTAB_STANDING, 0, nullptr, r2);
    y += sub_h + 12.0f;
  }

  if (A.subview == ACCT_SUB_STANDING) {
    draw_signed_in_standing(ui, r, x, &y, w);
    A.content_h = (y - start_y) + 24.0f;
    return;
  }
  if (A.subview == ACCT_SUB_MFA) {
    draw_signed_in_mfa(ui, r, x, &y, w);
    A.content_h = (y - start_y) + 24.0f;
    return;
  }

  // ---- 1. Hero Identity & Account Details Card ----
  char mname[128], muser[128], mid[32];
  const char *raw_dn = (self && self->display_name && self->display_name[0]) ? self->display_name
                                                                             : (v->display_name ? v->display_name : "");
  const char *raw_un = (self && self->handle && self->handle[0]) ? self->handle : (v->username ? v->username : "");
  const char *mdn = ui_settings_m6_mask(ui, raw_dn, mname, sizeof mname);
  const char *mun = ui_settings_m6_mask(ui, raw_un, muser, sizeof muser);
  const char *mid_s = ui_settings_m6_mask(ui, v->user_id_text, mid, sizeof mid);
  char handle[CCORD_IDENTITY_NAME_BYTES + 2];
  snprintf(handle, sizeof handle, "@%s", mun);

  float detail_row_h = ui->mobile_layout ? 64.0f : 52.0f;
  float meta_h = ui->mobile_layout ? 50.0f : 42.0f;
  float banner_h = 68.0f;
  float header_h = compact ? (ui->mobile_layout ? 116.0f : 108.0f) : 68.0f;
  float details_h = 4.0f * detail_row_h + meta_h;
  float hero_h = banner_h + header_h + details_h + 24.0f;
  ccord_rect hero = {x, y, w, hero_h};
  uis_card(ui, r, hero);

  // Banner strip
  ccord_rect banner = {hero.x, hero.y, hero.w, banner_h};
  ccord_push_clip(r, banner);
  ccord_color bcol = COL(brand);
  if (prof && prof->has_theme) bcol = acct_rgb(prof->theme_primary);
  else if (self && (self->has_accent_color || self->accent_color != 0)) bcol = acct_rgb(self->accent_color);
  uis_plate(ui, r, (ccord_rect){banner.x, banner.y, banner.w, banner.h + tk.r_surface * 2.0f}, tk.r_surface, bcol);
  char burl[256] = {0};
  if (self && ui_user_banner_url(self, burl, sizeof burl) && ui_ensure_media(ui, r, burl)) {
    int btw = 0, bth = 0;
    if (ccord_render_has_texture(r, burl, &btw, &bth) && btw > 2 && bth > 2) {
      ccord_draw_texture_cover(r, burl, banner, 0.0f, 255, false);
    }
  }
  ccord_pop_clip(r);

  // Avatar overlapping banner bottom
  float av_d = 68.0f;
  float av_cx = hero.x + 16.0f + av_d * 0.5f;
  float av_cy = hero.y + banner_h + 4.0f;
  ccord_draw_circle(r, av_cx, av_cy, av_d * 0.5f + 4.0f, COL(bg_secondary));
  if (self) {
    ui_draw_avatar(ui, r, self, av_cx, av_cy, av_d, COL(bg_secondary), true);
  } else {
    ccord_draw_circle(r, av_cx, av_cy, av_d * 0.5f, COL(brand));
  }

  // Name + Clan tag + Edit User Profile button
  float info_x = hero.x + 16.0f + av_d + 14.0f;
  float info_y = hero.y + banner_h + 12.0f;
  float ep_w = fmaxf(uis_label_width(ui, r, "Edit User Profile"), 138.0f);
  ccord_rect ep_btn;
  if (compact) {
    ep_btn = (ccord_rect){hero.x + 16.0f, hero.y + banner_h + 58.0f, fminf(ep_w, hero.w - 32.0f), bh_sm};
  } else {
    ep_btn = (ccord_rect){hero.x + hero.w - ep_w - 16.0f, info_y + 4.0f, ep_w, bh_sm};
  }
  float name_max_w = compact ? (hero.x + hero.w - info_x - 16.0f)
                             : fmaxf(ep_btn.x - info_x - 12.0f, 80.0f);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_title, COL(text_header));
  ccord_text_style us = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_secondary));
  float clan_w = (self && self->clan_tag[0]) ? (ui_clan_badge_width(ui, r, self->clan_tag, self->clan_badge[0] != '\0') + 8.0f) : 0.0f;
  float drawn_nw = ccord_text_draw_ellipsis(r, mdn[0] ? mdn : mun, strlen(mdn[0] ? mdn : mun),
                                            info_x, info_y, fmaxf(name_max_w - clan_w, 50.0f), &ns);
  if (self && self->clan_tag[0] && info_x + drawn_nw + clan_w <= info_x + name_max_w + 4.0f) {
    (void)ui_draw_clan_badge_ex(ui, r, self->clan_guild_id, self->clan_tag, self->clan_badge,
                                info_x + drawn_nw + 6.0f, info_y + 2.0f, ui_line_h(ui, ns.size),
                                UI_CLAN_LAYER_NONE);
  }
  ccord_text_draw_ellipsis(r, handle, strlen(handle), info_x, info_y + ui_line_h(ui, ns.size) + 2.0f, name_max_w, &us);

  st_draw_button(ui, r, ep_btn, "Edit User Profile", true);
  acct_add_hit(AH_EDIT_PROFILE, 0, nullptr, ep_btn);

  // Nested Account Details inset card
  ccord_rect inset = {hero.x + 14.0f, hero.y + banner_h + header_h, hero.w - 28.0f, details_h};
  uis_plate(ui, r, inset, tk.r_control, COL(bg_primary));
  float ix = inset.x + 14.0f;
  float iw = inset.w - 28.0f;
  float iy = inset.y + 4.0f;

  iy += draw_detail_row(ui, r, ix, iy, iw, "DISPLAY NAME", mdn[0] ? mdn : "Not set",
                        nullptr, COL(text_muted),
                        nullptr, AH_NONE, false, nullptr, AH_NONE, false,
                        "Edit", AH_EDIT_PROFILE, false);
  uis_divider(ui, r, ix, iy, iw);
  iy += draw_detail_row(ui, r, ix, iy, iw, "USERNAME", handle,
                        nullptr, COL(text_muted),
                        nullptr, AH_NONE, false, nullptr, AH_NONE, false,
                        "Edit", AH_EDIT_USERNAME, false);
  uis_divider(ui, r, ix, iy, iw);

  // Email row (masked by default; 30s reveal disabled in Streamer Mode)
  char email_disp[160] = "Not available";
  bool has_email = ap && ap->valid && ap->email[0] != '\0';
  bool email_revealed = has_email && !ui->settings_streamer_mode && A.reveal_email_until_ms > ui->now_ms;
  if (has_email) {
    if (ui->settings_streamer_mode) {
      snprintf(email_disp, sizeof email_disp, "%s", ui_settings_m6_mask(ui, ap->email, email_disp, sizeof email_disp));
    } else if (email_revealed) {
      snprintf(email_disp, sizeof email_disp, "%s", ap->email);
    } else {
      acct_mask_email(ap->email, email_disp, sizeof email_disp);
    }
  }
  const char *ver_pill = (ap && ap->valid && has_email) ? (ap->verified ? "Verified" : "Unverified") : nullptr;
  ccord_color ver_col = (ap && ap->verified) ? COL(status_online) : COL(status_idle);
  bool show_resend = has_email && ap && !ap->verified;
  iy += draw_detail_row(ui, r, ix, iy, iw, "EMAIL", email_disp, ver_pill, ver_col,
                        show_resend ? (A.resend_pending ? "Sending..." : "Resend") : nullptr, AH_RESEND_VERIFY, A.resend_pending,
                        has_email ? (email_revealed ? "Hide" : "Reveal") : nullptr, AH_REVEAL_EMAIL, ui->settings_streamer_mode,
                        "Edit", AH_EDIT_EMAIL, false);
  uis_divider(ui, r, ix, iy, iw);

  // Phone row
  char phone_disp[128] = "You haven't added a phone number yet.";
  bool has_phone = ap && ap->valid && ap->phone[0] != '\0';
  bool phone_revealed = has_phone && !ui->settings_streamer_mode && A.reveal_phone_until_ms > ui->now_ms;
  if (has_phone) {
    if (ui->settings_streamer_mode) {
      snprintf(phone_disp, sizeof phone_disp, "%s", ui_settings_m6_mask(ui, ap->phone, phone_disp, sizeof phone_disp));
    } else if (phone_revealed) {
      snprintf(phone_disp, sizeof phone_disp, "%s", ap->phone);
    } else {
      acct_mask_phone(ap->phone, phone_disp, sizeof phone_disp);
    }
  }
  iy += draw_detail_row(ui, r, ix, iy, iw, "PHONE NUMBER", phone_disp, nullptr, COL(text_muted),
                        has_phone ? (phone_revealed ? "Hide" : "Reveal") : nullptr, AH_REVEAL_PHONE, ui->settings_streamer_mode,
                        has_phone ? "Remove" : nullptr, AH_REMOVE_PHONE, false,
                        has_phone ? "Edit" : "Add", AH_EDIT_PHONE, false);
  uis_divider(ui, r, ix, iy, iw);

  // Compact metadata footer inside inset (User ID + Copy, Guild count, connection status)
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_muted));
  char meta[192];
  snprintf(meta, sizeof meta, "ID: %s  ·  %zu guild%s  ·  %s",
           mid_s[0] ? mid_s : "—", v->guild_count, v->guild_count == 1 ? "" : "s",
           v->status ? v->status : "Connected");
  ccord_rect cpy_btn = {ix + iw - 78.0f, iy + (ui->mobile_layout ? 6.0f : 4.0f), 78.0f, bh_sm};
  ccord_text_draw_ellipsis(r, meta, strlen(meta), ix, iy + (ui->mobile_layout ? 14.0f : 10.0f),
                           fmaxf(cpy_btn.x - ix - 8.0f, 60.0f), &ms);
  uis_button(ui, r, cpy_btn, "Copy ID", UIS_BTN_SECONDARY, ui->mobile_layout ? UIS_BTN_S_MD : UIS_BTN_S_SM,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, cpy_btn)}, "acid", 1);
  acct_add_hit(AH_COPY_USER_ID, 0, nullptr, cpy_btn);

  y += hero_h + UIS_SP_5;

  // ---- 2. Password & Authentication Card ----
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
  ccord_text_style bt = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_secondary));

  ccord_text_draw(r, "PASSWORD AND AUTHENTICATION", 27, x, y, &ls, 0.0f);
  y += 18.0f;
  bool mfa_on = ap && ap->valid && ap->mfa_enabled;
  float sec_h = compact ? (ui->mobile_layout ? 190.0f : 168.0f) : (ui->mobile_layout ? 160.0f : 138.0f);
  ccord_rect scard = {x, y, w, sec_h};
  uis_card(ui, r, scard);

  float sy = scard.y + 14.0f;
  ccord_rect pw_btn = {scard.x + 16.0f, sy, 152.0f, bh_sm};
  st_draw_button(ui, r, pw_btn, "Change Password", true);
  acct_add_hit(AH_CHANGE_PASSWORD, 0, nullptr, pw_btn);
  sy += bh_sm + 12.0f;
  uis_divider(ui, r, scard.x + 16.0f, sy, scard.w - 32.0f);
  sy += 12.0f;

  ccord_text_draw(r, "Multi-Factor Authentication", 27, scard.x + 16.0f, sy, &bt, 0.0f);
  float mtw = ccord_text_measure(r, "Multi-Factor Authentication", 27, &bt, 0.0f).x;
  acct_draw_pill(ui, r, scard.x + 24.0f + mtw, sy, mfa_on ? "Enabled" : "Disabled",
                 mfa_on ? COL(status_online) : COL(text_muted),
                 ui_alpha(mfa_on ? COL(status_online) : COL(text_muted), 0.16f), nullptr);
  const char *mdesc = mfa_on ? "Protect your account with Authenticator App (TOTP), Passkeys, SMS, and backup codes."
                             : "Add an extra layer of security with an Authenticator App (TOTP) or Passkeys.";
  ccord_text_draw_ellipsis(r, mdesc, strlen(mdesc), scard.x + 16.0f, sy + 22.0f, scard.w - 32.0f, &ss);
  float mby = sy + 46.0f;
  float mfa_w = (mfa_on && compact) ? fminf(154.0f, floorf((scard.w - 40.0f) * 0.5f)) : 154.0f;
  ccord_rect mfa_btn = {scard.x + 16.0f, mby, mfa_w, bh_sm};
  st_draw_button(ui, r, mfa_btn, mfa_on ? "Manage 2FA & Keys" : "Enable 2FA", !mfa_on);
  acct_add_hit(AH_OPEN_MFA, 0, nullptr, mfa_btn);
  if (mfa_on) {
    float bkc_w = compact ? fminf(146.0f, fmaxf(scard.w - 40.0f - mfa_w, 80.0f)) : 146.0f;
    ccord_rect bkc_btn = {mfa_btn.x + mfa_btn.w + 8.0f, mby, bkc_w, bh_sm};
    st_draw_button(ui, r, bkc_btn, "View Backup Codes", false);
    acct_add_hit(AH_VIEW_BACKUP_CODES, 0, nullptr, bkc_btn);
  }
  y += sec_h + UIS_SP_5;

  // ---- 3. Devices & Account Status Card ----
  ccord_text_draw(r, "DEVICES & ACCOUNT STATUS", 24, x, y, &ls, 0.0f);
  y += 18.0f;
  float st_h = 2.0f * detail_row_h + 12.0f;
  ccord_rect stcard = {x, y, w, st_h};
  uis_card(ui, r, stcard);
  float dy = stcard.y + 4.0f;
  dy += draw_detail_row(ui, r, stcard.x + 14.0f, dy, stcard.w - 28.0f,
                        "DEVICES & ACTIVE SESSIONS", "Review active logins and log out unrecognized sessions.",
                        nullptr, COL(text_muted),
                        nullptr, AH_NONE, false, nullptr, AH_NONE, false,
                        "Open Devices ->", AH_OPEN_DEVICES, false);
  uis_divider(ui, r, stcard.x + 14.0f, dy, stcard.w - 28.0f);
  ccord_account_standing_tier cur_tier = (st && st->loaded) ? st->tier : CCORD_STANDING_ALL_GOOD;
  char status_val[96];
  const char *age_str = (ap && ap->valid) ? (ap->nsfw_allowed ? "18+ Verified" : "Standard") : "Standard";
  snprintf(status_val, sizeof status_val, "Standing: %s  ·  Age Group: %s", standing_tier_label(cur_tier), age_str);
  (void)draw_detail_row(ui, r, stcard.x + 14.0f, dy, stcard.w - 28.0f,
                        "ACCOUNT STANDING & AGE GROUP", status_val,
                        standing_tier_label(cur_tier), standing_tier_color(ui, cur_tier),
                        nullptr, AH_NONE, false, nullptr, AH_NONE, false,
                        "View Standing ->", AH_OPEN_STANDING, false);
  y += st_h + UIS_SP_5;

  // ---- 4. Saved Accounts Card ----
  ccord_text_draw(r, "SAVED ACCOUNTS", 14, x, y + 6.0f, &ls, 0.0f);
  A.add_btn = (ccord_rect){x + w - 126.0f, y, 126.0f, bh_sm};
  st_draw_button(ui, r, A.add_btn, "Add Account", false);
  acct_add_hit(AH_ADD_ACCOUNT, 0, nullptr, A.add_btn);
  y += bh_sm + 10.0f;

  size_t shown = ui->saved_account_count < SAVED_ACCOUNT_ROWS ? ui->saved_account_count : SAVED_ACCOUNT_ROWS;
  if (shown == 0) {
    ccord_rect ec = {x, y, w, 46.0f};
    uis_card(ui, r, ec);
    ccord_text_draw_ellipsis(r, "No additional saved accounts in keyring switcher.", 49,
                             ec.x + 14.0f, ec.y + 14.0f, ec.w - 28.0f, &ss);
    y += 46.0f + UIS_SP_4;
  } else {
    ccord_text_style rns = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_normal));
    ccord_text_style ract = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, COL(brand));
    for (size_t i = 0; i < shown; i++) {
      const ccord_ui_saved_account *sa = &ui->saved_accounts[i];
      ccord_rect row = {x, y, w, bh + (ui->mobile_layout ? 10.0f : 6.0f)};
      A.saved_row[i] = row;
      uis_card(ui, r, row);
      if (sa->active) uis_list_row_fill(ui, r, row, (uis_state){.selected = true}, "sacc", (uint64_t)i);
      const char *name = sa->display_name[0] ? sa->display_name : (sa->username[0] ? sa->username : "Saved account");
      char line[160];
      snprintf(line, sizeof line, "%s%s", name, sa->active ? "  (active)" : "");
      ccord_rect rbtn = {row.x + row.w - 78.0f - 8.0f, row.y + (row.h - bh_sm) * 0.5f, 78.0f, bh_sm};
      ccord_rect sbtn = {rbtn.x - 72.0f - 6.0f, rbtn.y, 72.0f, bh_sm};
      A.saved_switch[i] = sbtn;
      A.saved_remove[i] = rbtn;
      ccord_text_draw_ellipsis(r, line, strlen(line), row.x + 12.0f,
                               row.y + (row.h - ui_line_h(ui, rns.size)) * 0.5f,
                               fmaxf(sbtn.x - row.x - 16.0f, 60.0f), sa->active ? &ract : &rns);
      if (!sa->active) {
        st_draw_button(ui, r, sbtn, "Switch", false);
        acct_add_hit(AH_SAVED_SWITCH, (int)i, nullptr, sbtn);
      }
      st_draw_button(ui, r, rbtn, "Remove", false);
      acct_add_hit(AH_SAVED_REMOVE, (int)i, nullptr, rbtn);
      y += row.h + 6.0f;
    }
    y += UIS_SP_3;
  }

  // ---- 5. Standalone Discord Token (Collapsible Advanced Disclosure) ----
  float tok_h = A.token_expanded ? (ui->mobile_layout ? 168.0f : 142.0f) : (ui->mobile_layout ? 58.0f : 48.0f);
  ccord_rect tcard = {x, y, w, tok_h};
  uis_card(ui, r, tcard);
  ccord_rect ttoggle = {tcard.x + tcard.w - 84.0f - 12.0f, tcard.y + 10.0f, 84.0f, bh_sm};
  ccord_text_draw_ellipsis(r, "STANDALONE DISCORD TOKEN (ADVANCED)", 35,
                           tcard.x + 14.0f, tcard.y + 16.0f, fmaxf(ttoggle.x - tcard.x - 20.0f, 80.0f), &ls);
  st_draw_button(ui, r, ttoggle, A.token_expanded ? "Hide" : "Show", false);
  acct_add_hit(AH_TOKEN_TOGGLE, 0, nullptr, ttoggle);
  if (A.token_expanded) {
    float tok_inp_h = ui->mobile_layout ? 42.0f : UIS_H_INPUT;
    A.token_box = (ccord_rect){tcard.x + 14.0f, tcard.y + (ui->mobile_layout ? 58.0f : 50.0f), tcard.w - 28.0f, tok_inp_h};
    draw_input_field(ui, r, A.token_box, &ui->account_token_edit, ui->account_focus_field == 1, true,
                     "Paste Discord user token...");
    acct_add_hit(AH_TOKEN_BOX, 0, nullptr, A.token_box);
    float bty = A.token_box.y + tok_inp_h + 10.0f;
    float pw = compact ? floorf((tcard.w - 40.0f) * 0.24f) : 74.0f;
    float sw = compact ? floorf((tcard.w - 40.0f) * 0.46f) : 142.0f;
    float cw = compact ? fmaxf(tcard.w - 40.0f - pw - sw, 64.0f) : 96.0f;
    A.token_paste = (ccord_rect){tcard.x + 14.0f, bty, pw, bh_sm};
    A.token_save = (ccord_rect){A.token_paste.x + pw + 6.0f, bty, sw, bh_sm};
    A.token_clear = (ccord_rect){A.token_save.x + sw + 6.0f, bty, cw, bh_sm};
    st_draw_button(ui, r, A.token_paste, "Paste", false);
    st_draw_button(ui, r, A.token_save, "Save & Connect", true);
    st_draw_button(ui, r, A.token_clear, "Sign Out", false);
    acct_add_hit(AH_TOKEN_PASTE, 0, nullptr, A.token_paste);
    acct_add_hit(AH_TOKEN_SAVE, 0, nullptr, A.token_save);
    acct_add_hit(AH_TOKEN_CLEAR, 0, nullptr, A.token_clear);
  }
  y += tok_h + UIS_SP_5;

  // ---- 6. Account Removal (Danger) Card ----
  ccord_text_draw(r, "ACCOUNT REMOVAL", 15, x, y, &ls, 0.0f);
  y += 18.0f;
  float dcard_h = ui->mobile_layout ? 108.0f : 96.0f;
  ccord_rect dcard = {x, y, w, dcard_h};
  uis_card(ui, r, dcard);
  ccord_draw_rect(r, (ccord_rect){dcard.x, dcard.y + 8.0f, 3.0f, dcard.h - 16.0f}, COL(danger));
  const char *rnote = "Disabling your account means you can recover it at any time. Deleting is permanent.";
  ccord_text_draw_ellipsis(r, rnote, strlen(rnote), dcard.x + 16.0f, dcard.y + 14.0f, dcard.w - 32.0f, &ss);
  float dis_w = compact ? fminf(138.0f, floorf((dcard.w - 42.0f) * 0.5f)) : 138.0f;
  float del_w = compact ? fminf(134.0f, fmaxf(dcard.w - 42.0f - dis_w, 80.0f)) : 134.0f;
  float dby = dcard.y + (ui->mobile_layout ? 54.0f : 48.0f);
  ccord_rect dis_btn = {dcard.x + 16.0f, dby, dis_w, bh_sm};
  ccord_rect del_btn = {dis_btn.x + dis_btn.w + 10.0f, dby, del_w, bh_sm};
  draw_danger_btn(ui, r, dis_btn, "Disable Account", false, false);
  draw_danger_btn(ui, r, del_btn, "Delete Account", true, false);
  acct_add_hit(AH_DISABLE_ACCOUNT, 0, nullptr, dis_btn);
  acct_add_hit(AH_DELETE_ACCOUNT, 0, nullptr, del_btn);
  y += dcard_h + UIS_SP_5;

  // ---- 7. Billing, Nitro, Shop & Web Settings Card ----
  ccord_text_draw(r, "BILLING, NITRO, SHOP & WEB SETTINGS", 35, x, y, &ls, 0.0f);
  y += 18.0f;
  float web_h = w < 380.0f ? (2.0f * bh + 6.0f) : bh;
  for (int i = 0; i < 5; i++) {
    ccord_rect brc;
    if (w < 380.0f) {
      float gap = 6.0f;
      int row = i < 3 ? 0 : 1;
      int col = i < 3 ? i : (i - 3);
      int cols = row == 0 ? 3 : 2;
      float bw = floorf((w - (float)(cols - 1) * gap) / (float)cols);
      brc = (ccord_rect){x + (float)col * (bw + gap), y + (float)row * (bh + gap), bw, bh};
    } else {
      float gap = 8.0f;
      float bw = (w - 4.0f * gap) / 5.0f;
      brc = (ccord_rect){x + (float)i * (bw + gap), y, bw, bh};
    }
    A.web_btn[i] = brc;
    st_draw_button(ui, r, brc, st_k_web_labels[i], false);
    acct_add_hit(AH_WEB_BTN, i, nullptr, brc);
  }
  y += web_h + UIS_SP_5;

  // ---- 8. Session Footer (Refresh & Sign Out) ----
  float fx = x;
  if (v->can_refresh) {
    ccord_rect rf_btn = {fx, y, 156.0f, bh};
    st_draw_button(ui, r, rf_btn, "Refresh", true);
    acct_add_hit(AH_REFRESH, 0, nullptr, rf_btn);
    fx += 156.0f + UIS_SP_3;
  }
  ccord_rect so_btn = {fx, y, 132.0f, bh};
  st_draw_button(ui, r, so_btn, "Sign out", false);
  acct_add_hit(AH_SIGN_OUT, 0, nullptr, so_btn);
  y += bh + UIS_SP_4;

  A.content_h = (y - start_y) + 20.0f;
}

void st_draw_account(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  const ccord_account_view *v = &ui->account_view;
  A.clip_rect = (ccord_rect){g->content.x - 4.0f, g->content.y + 34.0f, g->content.w + 8.0f,
                             fmaxf(g->content.h - 34.0f, 40.0f)};

  if (v->signed_in) {
    draw_signed_in_account(ui, r, g);
    return;
  }

  // Signed-out fallback inside Settings (preserves exact layout for tests & pre-login flows).
  A.has_cached = false;
  A.nhits = 0;
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
  ccord_rect em_box = st_account_email_box_rect(g);
  ccord_rect pw_box = st_account_password_box_rect(g);
  ccord_rect login_btn = st_account_login_btn_rect(g);
  ccord_text_draw(r, "EMAIL OR PHONE", 14, em_box.x, em_box.y - 15.0f, &ls, 0.0f);
  ccord_text_draw(r, "PASSWORD", 8, pw_box.x, pw_box.y - 15.0f, &ls, 0.0f);
  draw_input_field(ui, r, em_box, &ui->account_email_edit, ui->account_focus_field == 2, false, "you@example.com");
  draw_input_field(ui, r, pw_box, &ui->account_password_edit, ui->account_focus_field == 3, true, "Password");
  st_draw_button(ui, r, login_btn, "Log In to Discord", true);

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

  {
    ccord_text_style rls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
    ccord_rect add_r = st_account_add_btn_rect(g);
    ccord_rect row0 = st_saved_account_row_rect(g, 0);
    float row_step = g->mobile ? 48.0f : 40.0f;
    ccord_text_draw(r, "SAVED ACCOUNTS", 14, x, add_r.y + (g->mobile ? 12.0f : 8.0f), &rls, 0.0f);
    st_draw_button(ui, r, add_r, "Add Account", false);
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
      ccord_text_draw_ellipsis(r, msg, strlen(msg), x, row0.y, w - 120.0f, &empty);
    } else if (ui->saved_account_count > SAVED_ACCOUNT_ROWS) {
      char more[48];
      snprintf(more, sizeof more, "+%zu more", ui->saved_account_count - SAVED_ACCOUNT_ROWS);
      ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_caption, COL(text_muted));
      ccord_text_draw(r, more, strlen(more), x, row0.y + (float)SAVED_ACCOUNT_ROWS * row_step + 8.0f, &ms, 0.0f);
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
  if (v->can_sign_in) {
    st_draw_button(ui, r, primary, "Sign in with Discord", true);
  }
}

void st_draw_account_overlays(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  (void)g;
  if (st_account_modal_open()) {
    st_account_draw_modal(ui, r);
  }
}

// ---- Signed-in Account Click, Key & Hover Dispatch ----

int64_t st_account_hover_key(const ccord_ui *ui, const sgeom *g, float x, float y)
{
  (void)g;
  if (!ui || !ui->account_view.signed_in) return 0;
  if (st_account_modal_open()) return 150;
  for (size_t i = A.nhits; i-- > 0;) {
    if (ui_in_rect(x, y, A.hits[i].rect)) return 160 + (int64_t)i;
  }
  return 0;
}

bool st_account_handle_click(ccord_ui *ui, const sgeom *g, float x, float y)
{
  (void)g;
  if (!ui) return false;
  if (st_account_modal_open()) {
    return st_account_modal_handle_click(ui, x, y);
  }
  if (!ui->account_view.signed_in) return false;

  for (size_t i = A.nhits; i-- > 0;) {
    const acct_hit *h = &A.hits[i];
    if (!ui_in_rect(x, y, h->rect)) continue;
    if (h->kind != AH_WEBAUTHN_DELETE) A.webauthn_confirm_idx = -1;
    if (h->kind != AH_TOKEN_BOX) {
      ui->account_focus_field = 0;
      ui->touch_text_focused = false;
    }
    switch (h->kind) {
      case AH_SUBTAB_OVERVIEW:
        A.subview = ACCT_SUB_OVERVIEW;
        st_s_page_scroll = 0.0f;
        ui->dirty = true;
        return true;
      case AH_SUBTAB_STANDING:
        A.subview = ACCT_SUB_STANDING;
        st_s_page_scroll = 0.0f;
        ui->dirty = true;
        return true;
      case AH_BACK_OVERVIEW:
        A.subview = ACCT_SUB_OVERVIEW;
        st_s_page_scroll = 0.0f;
        ui->dirty = true;
        return true;
      case AH_EDIT_PROFILE:
        ui_settings_open_profile(ui);
        return true;
      case AH_EDIT_USERNAME: {
        const ccord_user *self = ui->d.store ? ccord_store_user(ui->d.store, ccord_store_self_id(ui->d.store)) : nullptr;
        const char *cur_un = (self && self->handle && self->handle[0]) ? self->handle : ui->account_view.username;
        st_account_open_modal(ui, ST_AMOD_USERNAME, cur_un);
        return true;
      }
      case AH_EDIT_EMAIL:
        st_account_open_modal(ui, ST_AMOD_EMAIL, nullptr);
        return true;
      case AH_REVEAL_EMAIL:
        if (!ui->settings_streamer_mode) {
          A.reveal_email_until_ms = (A.reveal_email_until_ms > ui->now_ms) ? 0 : (ui->now_ms + ACCT_REVEAL_MS);
          ui->dirty = true;
        }
        return true;
      case AH_RESEND_VERIFY:
        if (ui->d.session && !A.resend_pending) {
          ccord_account_req req = {.op = CCORD_ACCT_OP_RESEND_VERIFY};
          ccord_error err = {0};
          if (ccord_session_account_submit(ui->d.session, &req, acct_on_resend_done, ui, &err) == CCORD_OK) {
            A.resend_pending = true;
          } else {
            ui_notice(ui, "%s", err.message[0] ? err.message : "Could not resend verification email.");
          }
          ui->dirty = true;
        }
        return true;
      case AH_EDIT_PHONE:
        st_account_open_modal(ui, ST_AMOD_PHONE_ADD, nullptr);
        return true;
      case AH_REVEAL_PHONE:
        if (!ui->settings_streamer_mode) {
          A.reveal_phone_until_ms = (A.reveal_phone_until_ms > ui->now_ms) ? 0 : (ui->now_ms + ACCT_REVEAL_MS);
          ui->dirty = true;
        }
        return true;
      case AH_REMOVE_PHONE:
        st_account_open_modal(ui, ST_AMOD_PHONE_REMOVE, nullptr);
        return true;
      case AH_COPY_USER_ID:
        if (ui->d.host.clipboard_set && ui->account_view.user_id_text[0]) {
          ui->d.host.clipboard_set(ui->d.host.ud, ui->account_view.user_id_text);
          ui_notice(ui, "User ID copied to clipboard.");
        }
        ui->dirty = true;
        return true;
      case AH_REFRESH:
        if (ui->d.host.account_action) ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_REFRESH);
        ui->dirty = true;
        return true;
      case AH_SIGN_OUT:
        if (ui->d.host.account_action) ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_SIGN_OUT);
        ui->dirty = true;
        return true;
      case AH_CHANGE_PASSWORD:
        st_account_open_modal(ui, ST_AMOD_PASSWORD, nullptr);
        return true;
      case AH_OPEN_MFA:
        A.subview = ACCT_SUB_MFA;
        A.webauthn_fetched = false;
        st_s_page_scroll = 0.0f;
        ui->dirty = true;
        return true;
      case AH_VIEW_BACKUP_CODES:
        st_account_open_modal(ui, ST_AMOD_BACKUP_CODES, nullptr);
        return true;
      case AH_TOTP_ENABLE:
        st_account_open_modal(ui, ST_AMOD_TOTP_SETUP, nullptr);
        return true;
      case AH_TOTP_DISABLE:
        st_account_open_modal(ui, ST_AMOD_TOTP_DISABLE, nullptr);
        return true;
      case AH_SMS_TOGGLE:
        st_account_open_modal(ui, ST_AMOD_SMS_TOGGLE, h->arg ? "1" : "0");
        return true;
      case AH_WEBAUTHN_DELETE:
        if (A.webauthn_confirm_idx == h->arg) {
          if (ui->d.session && h->str_arg[0]) {
            ccord_account_req req = {.op = CCORD_ACCT_OP_WEBAUTHN_DELETE, .value = h->str_arg};
            ccord_error err = {0};
            if (ccord_session_account_submit(ui->d.session, &req, acct_on_webauthn_del_done, ui, &err) != CCORD_OK) {
              ui_notice(ui, "%s", err.message[0] ? err.message : "Could not remove security key.");
            }
          }
          A.webauthn_confirm_idx = -1;
        } else {
          A.webauthn_confirm_idx = h->arg;
        }
        ui->dirty = true;
        return true;
      case AH_WEBAUTHN_WEB:
        ui_open_url(ui, "https://discord.com/settings/account", "Security Keys");
        ui->dirty = true;
        return true;
      case AH_OPEN_DEVICES:
        ui->settings_page = SP_DEVICES;
        ui->settings_category = st_k_pages[SP_DEVICES].category;
        st_s_page_scroll = 0.0f;
        ui->dirty = true;
        return true;
      case AH_OPEN_STANDING:
        A.subview = ACCT_SUB_STANDING;
        A.standing_fetched = false;
        st_s_page_scroll = 0.0f;
        ui->dirty = true;
        return true;
      case AH_REFRESH_STANDING:
        if (ui->d.session) {
          ccord_error err = {0};
          (void)ccord_session_account_fetch_standing(ui->d.session, acct_on_fetch_done, ui, &err);
        }
        ui->dirty = true;
        return true;
      case AH_OPEN_PRIVACY:
        ui->settings_page = SP_PRIVACY;
        ui->settings_category = st_k_pages[SP_PRIVACY].category;
        st_s_page_scroll = 0.0f;
        ui->dirty = true;
        return true;
      case AH_DISABLE_ACCOUNT:
        st_account_open_modal(ui, ST_AMOD_DISABLE_ACCT, nullptr);
        return true;
      case AH_DELETE_ACCOUNT:
        st_account_open_modal(ui, ST_AMOD_DELETE_ACCT, nullptr);
        return true;
      case AH_ADD_ACCOUNT:
        A.token_expanded = true;
        ccord_text_edit_clear(&ui->account_token_edit);
        ui->account_focus_field = 1;
        if (ui->touch_mode) ui->touch_text_focused = true;
        ui->dirty = true;
        return true;
      case AH_SAVED_SWITCH:
        if (h->arg >= 0 && (size_t)h->arg < ui->saved_account_count && !ui->saved_accounts[h->arg].active &&
            ui->d.host.account_switch) {
          ui->d.host.account_switch(ui->d.host.ud, ui->saved_accounts[h->arg].user_id);
        }
        ui->dirty = true;
        return true;
      case AH_SAVED_REMOVE:
        if (h->arg >= 0 && (size_t)h->arg < ui->saved_account_count && ui->d.host.account_remove) {
          ui->d.host.account_remove(ui->d.host.ud, ui->saved_accounts[h->arg].user_id);
        }
        ui->dirty = true;
        return true;
      case AH_TOKEN_TOGGLE:
        A.token_expanded = !A.token_expanded;
        if (!A.token_expanded && ui->account_focus_field == 1) ui->account_focus_field = 0;
        ui->dirty = true;
        return true;
      case AH_TOKEN_BOX:
        ui->account_focus_field = 1;
        ui->touch_text_focused = true;
        ui->text_area = h->rect;
        ui->dirty = true;
        return true;
      case AH_TOKEN_PASTE:
        if (ui->d.host.clipboard_get) {
          char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
          if (clip) {
            ccord_error ierr;
            ccord_text_edit_clear(&ui->account_token_edit);
            (void)ccord_text_edit_insert(&ui->account_token_edit, clip, strlen(clip), &ierr);
            ccord_free(clip);
          }
        }
        ui->dirty = true;
        return true;
      case AH_TOKEN_SAVE:
        if (ui->d.host.account_action) ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN);
        ui->dirty = true;
        return true;
      case AH_TOKEN_CLEAR:
        ccord_text_edit_clear(&ui->account_token_edit);
        if (ui->d.host.account_action) ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_TOKEN_SIGN_OUT);
        ui->dirty = true;
        return true;
      case AH_WEB_BTN:
        if (h->arg >= 0 && h->arg < 5) {
          ui_open_url(ui, st_k_web_urls[h->arg], st_k_web_labels[h->arg]);
        }
        ui->dirty = true;
        return true;
      default:
        break;
    }
  }
  ui->account_focus_field = 0;
  ui->touch_text_focused = false;
  A.webauthn_confirm_idx = -1;
  ui->dirty = true;
  return true;
}

bool st_account_handle_key(ccord_ui *ui, const ccord_input_event *ev)
{
  if (st_account_modal_open()) {
    return st_account_modal_handle_key(ui, ev);
  }
  return false;
}

// ---- Login Screen & Login MFA Popup (preserved) ----

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
  ccord_draw_rrect(r, (ccord_rect){card.x, card.y, card.w, 8.0f}, 4.0f, COL(brand));
  ccord_rect badge = {card.x + (card.w - 96.0f) * 0.5f, card.y + 14.0f - lsc, 96.0f, 22.0f};
  ccord_draw_rrect(r, badge, 11.0f, COL(brand));
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_caption, COL(text_on_brand));
  ccord_vec2 bt = ccord_text_measure(r, "DISCORD", 7, &bs, 0.0f);
  ccord_text_draw(r, "DISCORD", 7, badge.x + (badge.w - bt.x) * 0.5f, badge.y + (badge.h - bt.y) * 0.5f, &bs, 0.0f);

  const char *title = "Welcome back!";
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_title, COL(text_header));
  ccord_vec2 tt = ccord_text_measure(r, title, strlen(title), &ts, 0.0f);
  ccord_text_draw(r, title, strlen(title), card.x + (card.w - tt.x) * 0.5f, card.y + 42.0f - lsc, &ts, 0.0f);

  const char *sub = "We're so excited to see you again!";
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
  ccord_vec2 st = ccord_text_measure(r, sub, strlen(sub), &ss, 0.0f);
  ccord_text_draw(r, sub, strlen(sub), card.x + (card.w - st.x) * 0.5f, card.y + 68.0f - lsc, &ss, 0.0f);

  static const char *const k_tab_labels[2] = {"Token Sign-In", "Email & Password"};
  for (int i = 0; i < 2; i++) {
    ccord_rect tr = st_login_tab_rect(&card, i);
    uis_state tst = {.hover = ui_in_rect(ui->mx, ui->my, tr)};
    uis_button(ui, r, tr, k_tab_labels[i], ui->login_tab == i ? UIS_BTN_PRIMARY : UIS_BTN_SECONDARY, UIS_BTN_S_MD,
               tst, "ltab", (uint64_t)i);
  }

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
                             banner.y + (banner.h - ui_line_h(ui, es.size)) * 0.5f, banner.w - 18.0f, &es);
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
    st_draw_button(ui, r, st_login_token_save_rect(&card),
                   connecting ? "Connecting to Discord Gateway..." : "Connect to Discord", true);
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
    ui->account_focus_field = 2;
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
    volatile unsigned char *p = (volatile unsigned char *)ui->account_token_edit.data;
    for (size_t i = 0; i < ui->account_token_edit.len; i++) p[i] = 0;
  }
  ccord_text_edit_clear(&ui->account_token_edit);
  if (ui->account_password_edit.len > 0) {
    volatile unsigned char *p = (volatile unsigned char *)ui->account_password_edit.data;
    for (size_t i = 0; i < ui->account_password_edit.len; i++) p[i] = 0;
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
