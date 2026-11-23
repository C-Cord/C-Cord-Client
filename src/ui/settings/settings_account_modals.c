// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Protected Account Editing & Multi-Factor Modals (username, email, phone, password, TOTP QR setup,
// backup codes, SMS toggle, disable/delete account, and unified HTTP 401/403 code 60003 MFA flow).
//
// Security guarantees:
// - Drawn under C-Cord's immutable native protected_theme (plugins cannot spoof or skin these modals).
// - All passwords, verification codes, TOTP secrets, and backup codes are volatile-wiped on submit,
//   cancel, ESC, modal close, or settings teardown.
#include "settings_internal.h"

#include <ccord/net/account_edit.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>

// ---- Volatile memory wipe (layer-safe, no core/secret.h dependency) ----

static void am_wipe_bytes(void *ptr, size_t len)
{
  if (!ptr || len == 0) return;
  volatile unsigned char *p = (volatile unsigned char *)ptr;
  for (size_t i = 0; i < len; i++) p[i] = 0;
}

static void am_wipe_edit(ccord_text_edit *ed)
{
  if (!ed) return;
  if (ed->data && ed->cap > 0) am_wipe_bytes(ed->data, ed->cap);
  ccord_text_edit_clear(ed);
}

// ---- Minimal ISO/IEC 18004 QR Code Version 6-L (41x41, up to 134 UTF-8 bytes) ----

#define QR_V6_SIZE 41
#define QR_V6_TOTAL_CW 172
#define QR_V6_BLOCKS 2
#define QR_V6_DATA_PER_BLK 68
#define QR_V6_EC_PER_BLK 18
#define QR_V6_DATA_CW (QR_V6_BLOCKS * QR_V6_DATA_PER_BLK)   // 136

typedef struct qr_matrix {
  bool valid;
  uint8_t mod[QR_V6_SIZE][QR_V6_SIZE];   // 1 = dark, 0 = light
} qr_matrix;

static uint8_t qr_gf_mul(uint8_t a, uint8_t b)
{
  uint8_t res = 0;
  for (int i = 0; i < 8; i++) {
    if (b & 1u) res ^= a;
    uint8_t hi = (uint8_t)(a & 0x80u);
    a = (uint8_t)(a << 1);
    if (hi) a ^= 0x1Du;
    b >>= 1;
  }
  return res;
}

static void qr_rs_encode(const uint8_t *data, size_t dlen, uint8_t *ec, size_t eclen)
{
  uint8_t gen[QR_V6_EC_PER_BLK + 1] = {0};
  gen[0] = 1;
  uint8_t root = 1;
  for (size_t i = 0; i < eclen; i++) {
    for (size_t j = i + 1; j > 0; j--) {
      gen[j] = (uint8_t)(gen[j] ^ qr_gf_mul(gen[j - 1], root));
    }
    root = qr_gf_mul(root, 2);
  }
  memset(ec, 0, eclen);
  for (size_t i = 0; i < dlen; i++) {
    uint8_t factor = (uint8_t)(data[i] ^ ec[0]);
    memmove(ec, ec + 1, eclen - 1);
    ec[eclen - 1] = 0;
    for (size_t j = 0; j < eclen; j++) {
      ec[j] ^= qr_gf_mul(gen[j + 1], factor);
    }
  }
}

static void qr_set_func(uint8_t mod[QR_V6_SIZE][QR_V6_SIZE], uint8_t is_func[QR_V6_SIZE][QR_V6_SIZE],
                        int r, int c, uint8_t dark)
{
  if (r < 0 || r >= QR_V6_SIZE || c < 0 || c >= QR_V6_SIZE) return;
  mod[r][c] = dark ? 1u : 0u;
  is_func[r][c] = 1u;
}

static void qr_place_finder(uint8_t mod[QR_V6_SIZE][QR_V6_SIZE], uint8_t is_func[QR_V6_SIZE][QR_V6_SIZE],
                            int r0, int c0)
{
  for (int dr = -1; dr <= 7; dr++) {
    for (int dc = -1; dc <= 7; dc++) {
      int r = r0 + dr, c = c0 + dc;
      if (r < 0 || r >= QR_V6_SIZE || c < 0 || c >= QR_V6_SIZE) continue;
      bool dark = (dr >= 0 && dr <= 6 && dc >= 0 && dc <= 6) &&
                  (dr == 0 || dr == 6 || dc == 0 || dc == 6 || (dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4));
      qr_set_func(mod, is_func, r, c, dark ? 1u : 0u);
    }
  }
}

static void qr_encode_v6l(const char *utf8, qr_matrix *out)
{
  memset(out, 0, sizeof *out);
  if (!utf8 || !utf8[0]) return;
  size_t len = strlen(utf8);
  if (len > QR_V6_DATA_CW - 2u) return;

  // 1. Build bit stream: 0100 (byte mode) + 8-bit length + payload + terminator + padding
  uint8_t data_cw[QR_V6_DATA_CW] = {0};
  size_t bit_pos = 0;
  uint32_t hdr = (0x4u << 8) | (uint32_t)(len & 0xFFu);
  for (int i = 11; i >= 0; i--) {
    if ((hdr >> i) & 1u) data_cw[bit_pos / 8u] |= (uint8_t)(1u << (7u - (bit_pos % 8u)));
    bit_pos++;
  }
  for (size_t i = 0; i < len; i++) {
    uint32_t ch = (uint8_t)utf8[i];
    for (int b = 7; b >= 0; b--) {
      if ((ch >> b) & 1u) data_cw[bit_pos / 8u] |= (uint8_t)(1u << (7u - (bit_pos % 8u)));
      bit_pos++;
    }
  }
  for (int i = 0; i < 4 && bit_pos < QR_V6_DATA_CW * 8u; i++) bit_pos++;
  while ((bit_pos % 8u) != 0 && bit_pos < QR_V6_DATA_CW * 8u) bit_pos++;
  size_t cw_idx = bit_pos / 8u;
  uint8_t pad_bytes[2] = {0xECu, 0x11u};
  for (size_t p = 0; cw_idx < QR_V6_DATA_CW; cw_idx++, p++) {
    data_cw[cw_idx] = pad_bytes[p & 1u];
  }

  // 2. Reed-Solomon EC for 2 blocks + interleaving
  uint8_t ec[QR_V6_BLOCKS][QR_V6_EC_PER_BLK];
  for (int b = 0; b < QR_V6_BLOCKS; b++) {
    qr_rs_encode(data_cw + (size_t)b * QR_V6_DATA_PER_BLK, QR_V6_DATA_PER_BLK, ec[b], QR_V6_EC_PER_BLK);
  }
  uint8_t final_cw[QR_V6_TOTAL_CW];
  size_t fi = 0;
  for (size_t i = 0; i < QR_V6_DATA_PER_BLK; i++) {
    for (int b = 0; b < QR_V6_BLOCKS; b++) {
      final_cw[fi++] = data_cw[(size_t)b * QR_V6_DATA_PER_BLK + i];
    }
  }
  for (size_t i = 0; i < QR_V6_EC_PER_BLK; i++) {
    for (int b = 0; b < QR_V6_BLOCKS; b++) {
      final_cw[fi++] = ec[b][i];
    }
  }

  // 3. Place function patterns
  uint8_t is_func[QR_V6_SIZE][QR_V6_SIZE] = {{0}};
  qr_place_finder(out->mod, is_func, 0, 0);
  qr_place_finder(out->mod, is_func, 0, QR_V6_SIZE - 7);
  qr_place_finder(out->mod, is_func, QR_V6_SIZE - 7, 0);

  // Timing patterns
  for (int i = 8; i < QR_V6_SIZE - 8; i++) {
    qr_set_func(out->mod, is_func, 6, i, (i % 2 == 0) ? 1u : 0u);
    qr_set_func(out->mod, is_func, i, 6, (i % 2 == 0) ? 1u : 0u);
  }

  // Alignment pattern at (34, 34)
  for (int dr = -2; dr <= 2; dr++) {
    for (int dc = -2; dc <= 2; dc++) {
      int ad = (dr < 0 ? -dr : dr);
      int ac = (dc < 0 ? -dc : dc);
      int d = ad > ac ? ad : ac;
      qr_set_func(out->mod, is_func, 34 + dr, 34 + dc, (d != 1) ? 1u : 0u);
    }
  }

  // Reserve format info & dark module
  for (int i = 0; i <= 8; i++) {
    if (i != 6) {
      qr_set_func(out->mod, is_func, 8, i, 0);
      qr_set_func(out->mod, is_func, i, 8, 0);
    }
  }
  for (int i = 0; i < 8; i++) {
    qr_set_func(out->mod, is_func, 8, QR_V6_SIZE - 1 - i, 0);
    if (i < 7) qr_set_func(out->mod, is_func, QR_V6_SIZE - 1 - i, 8, 0);
  }
  qr_set_func(out->mod, is_func, QR_V6_SIZE - 8, 8, 1);

  // 4. Fill data bits in zigzag + apply Mask 0 ((r + c) % 2 == 0)
  size_t total_bits = QR_V6_TOTAL_CW * 8u;
  size_t bi = 0;
  bool upward = true;
  for (int right = QR_V6_SIZE - 1; right >= 1; right -= 2) {
    if (right == 6) right = 5;
    for (int step = 0; step < QR_V6_SIZE; step++) {
      int r = upward ? (QR_V6_SIZE - 1 - step) : step;
      for (int j = 0; j < 2; j++) {
        int c = right - j;
        if (is_func[r][c]) continue;
        uint8_t bit = 0;
        if (bi < total_bits) {
          bit = (uint8_t)((((uint32_t)final_cw[bi / 8u]) >> (7u - (bi % 8u))) & 1u);
          bi++;
        }
        if (((r + c) & 1) == 0) bit ^= 1u;   // Mask 0
        out->mod[r][c] = bit;
      }
    }
    upward = !upward;
  }

  // 5. Write format info for EC Level L (01) + Mask 0 (000) -> BCH(15,5) ^ 0x5412
  uint32_t fmt_data = (1u << 3) | 0u;   // Level L = 01, Mask 0 = 000
  uint32_t rem = fmt_data << 10;
  for (int i = 14; i >= 10; i--) {
    if ((rem >> i) & 1u) rem ^= (0x537u << (i - 10));
  }
  uint32_t fmt = ((fmt_data << 10) | rem) ^ 0x5412u;

  for (int i = 0; i <= 5; i++) out->mod[8][i] = (uint8_t)((fmt >> (14 - i)) & 1u);
  out->mod[8][7] = (uint8_t)((fmt >> 8) & 1u);
  out->mod[8][8] = (uint8_t)((fmt >> 7) & 1u);
  out->mod[7][8] = (uint8_t)((fmt >> 6) & 1u);
  for (int i = 0; i <= 5; i++) out->mod[5 - i][8] = (uint8_t)((fmt >> (5 - i)) & 1u);

  for (int i = 0; i < 8; i++) out->mod[8][QR_V6_SIZE - 1 - i] = (uint8_t)((fmt >> i) & 1u);
  for (int i = 0; i < 7; i++) out->mod[QR_V6_SIZE - 1 - i][8] = (uint8_t)((fmt >> (14 - i)) & 1u);

  out->valid = true;
}

// ---- Modal State ----

static const char *const k_mfa_labels[4] = {"Authenticator", "Backup Code", "SMS", "Password"};
static const char *const k_mfa_types[4] = {"totp", "backup", "sms", "password"};
static const uint32_t k_mfa_bits[4] = {
  CCORD_MFA_METHOD_TOTP, CCORD_MFA_METHOD_BACKUP, CCORD_MFA_METHOD_SMS, CCORD_MFA_METHOD_PASSWORD
};

static struct {
  int kind;                         // st_acct_modal_kind (0 = none)
  bool inited;
  ccord_text_edit f[3];             // up to 3 inputs (primary value, secondary/new pw, password/confirm)
  int focus;                        // 0..2 or -1
  int field_count;
  bool masked[3];
  bool pending;
  bool captcha_fallback;
  int phone_step;                   // 1 = send SMS, 2 = verify SMS code + password
  char phone_pending_num[64];
  bool sms_enable_target;
  char totp_secret[33];
  qr_matrix qr;
  bool showing_codes;
  size_t backup_count;
  ccord_account_backup_code backup_codes[CCORD_ACCOUNT_MAX_BACKUP_CODES];
  uint32_t mfa_methods;
  int mfa_method_idx;
  char error[256];
  char info[200];
  uint64_t cb_gen;
  ccord_ui *owner;
  // Hit rects cached from last draw
  ccord_rect box;
  ccord_rect field_rect[3];
  ccord_rect submit_btn;
  ccord_rect cancel_btn;
  ccord_rect aux_btn;               // Copy Secret / Copy All / Open Browser
  ccord_rect aux2_btn;              // Regenerate backup codes
  ccord_rect mfa_chips[4];
} M;

static void modal_ensure_init(void)
{
  if (M.inited) return;
  for (int i = 0; i < 3; i++) ccord_text_edit_init(&M.f[i], 256);
  M.inited = true;
}

static void modal_wipe_all(void)
{
  if (M.inited) {
    for (int i = 0; i < 3; i++) am_wipe_edit(&M.f[i]);
  }
  am_wipe_bytes(M.totp_secret, sizeof M.totp_secret);
  am_wipe_bytes(M.backup_codes, sizeof M.backup_codes);
  am_wipe_bytes(M.phone_pending_num, sizeof M.phone_pending_num);
  memset(&M.qr, 0, sizeof M.qr);
  M.backup_count = 0;
  M.showing_codes = false;
  M.captcha_fallback = false;
  M.error[0] = '\0';
  M.info[0] = '\0';
}

static void modal_set_text(int idx, const char *utf8)
{
  if (!M.inited || idx < 0 || idx >= 3) return;
  am_wipe_edit(&M.f[idx]);
  if (utf8 && utf8[0]) {
    ccord_error err = {0};
    (void)ccord_text_edit_insert(&M.f[idx], utf8, strlen(utf8), &err);
  }
}

static size_t count_owned_guilds(const ccord_ui *ui, char *names_out, size_t cap)
{
  if (names_out && cap > 0) names_out[0] = '\0';
  if (!ui || !ui->d.store) return 0;
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (self == CCORD_ID_NONE) return 0;
  size_t ng = ccord_store_guild_count(ui->d.store);
  size_t owned = 0;
  size_t pos = 0;
  for (size_t i = 0; i < ng; i++) {
    const ccord_guild *g = ccord_store_guild_at(ui->d.store, i);
    if (!g || g->owner_id != self) continue;
    owned++;
    if (names_out && cap > 4 && g->name && g->name[0]) {
      if (owned <= 3) {
        int n = snprintf(names_out + pos, cap - pos, "%s%s", owned > 1 ? ", " : "", g->name);
        if (n > 0 && (size_t)n < cap - pos) pos += (size_t)n;
      } else if (owned == 4) {
        int n = snprintf(names_out + pos, cap - pos, ", ...");
        if (n > 0 && (size_t)n < cap - pos) pos += (size_t)n;
      }
    }
  }
  return owned;
}

static int first_mfa_method(uint32_t mask)
{
  for (int i = 0; i < 4; i++) {
    if (mask & k_mfa_bits[i]) return i;
  }
  return 0;
}

bool st_account_modal_open(void) { return M.kind != ST_AMOD_NONE; }

bool st_account_modal_wants_text(void)
{
  return M.kind != ST_AMOD_NONE && M.focus >= 0 && M.focus < M.field_count && !M.showing_codes;
}

void st_account_modal_close(ccord_ui *ui)
{
  if (M.kind == ST_AMOD_MFA_CHALLENGE && ui && ui->d.session) {
    ccord_session_account_cancel_mfa(ui->d.session);
  }
  modal_wipe_all();
  M.kind = ST_AMOD_NONE;
  M.focus = -1;
  M.field_count = 0;
  M.pending = false;
  M.cb_gen++;
  if (ui) {
    if (ui->touch_mode) ui->touch_text_focused = false;
    ui->dirty = true;
  }
}

void st_account_open_modal(ccord_ui *ui, int modal_kind, const char *ctx_str)
{
  if (!ui) return;
  modal_ensure_init();
  modal_wipe_all();
  M.kind = modal_kind;
  M.owner = ui;
  M.pending = false;
  M.cb_gen++;
  M.focus = 0;
  M.masked[0] = M.masked[1] = M.masked[2] = false;
  M.phone_step = 1;
  M.sms_enable_target = (ctx_str && strcmp(ctx_str, "1") == 0);

  switch (modal_kind) {
    case ST_AMOD_USERNAME:
      M.field_count = 2;
      M.masked[0] = false;
      M.masked[1] = true;
      if (ctx_str && ctx_str[0]) modal_set_text(0, ctx_str);
      break;
    case ST_AMOD_EMAIL:
      M.field_count = 2;
      M.masked[0] = false;
      M.masked[1] = true;
      break;
    case ST_AMOD_PHONE_ADD:
      M.field_count = 1;
      M.masked[0] = false;
      break;
    case ST_AMOD_PHONE_REMOVE:
    case ST_AMOD_BACKUP_CODES:
    case ST_AMOD_SMS_TOGGLE:
    case ST_AMOD_DISABLE_ACCT:
      M.field_count = 1;
      M.masked[0] = true;
      break;
    case ST_AMOD_DELETE_ACCT: {
      char owned_names[160] = {0};
      size_t owned = count_owned_guilds(ui, owned_names, sizeof owned_names);
      if (owned > 0) {
        M.field_count = 0;
        M.focus = -1;
        snprintf(M.error, sizeof M.error,
                 "Transfer ownership of %zu server%s (%s) before deleting your account.",
                 owned, owned == 1 ? "" : "s", owned_names[0] ? owned_names : "owned servers");
      } else {
        M.field_count = 1;
        M.masked[0] = true;
      }
      break;
    }
    case ST_AMOD_PASSWORD:
      M.field_count = 3;
      M.masked[0] = true;
      M.masked[1] = true;
      M.masked[2] = true;
      break;
    case ST_AMOD_TOTP_SETUP: {
      M.field_count = 2;
      M.masked[0] = false;   // 6-digit code
      M.masked[1] = true;    // current password
      ccord_error gerr = {0};
      if (ccord_account_generate_totp_secret(M.totp_secret, &gerr) == CCORD_OK) {
        const char *lbl = (ui->account_view.username && ui->account_view.username[0])
                            ? ui->account_view.username
                            : "user";
        char uri[256] = {0};
        if (ccord_account_totp_uri(lbl, M.totp_secret, uri, sizeof uri)) {
          qr_encode_v6l(uri, &M.qr);
        }
        am_wipe_bytes(uri, sizeof uri);
      } else {
        snprintf(M.error, sizeof M.error, "%s", gerr.message[0] ? gerr.message : "Could not generate TOTP secret.");
      }
      break;
    }
    case ST_AMOD_TOTP_DISABLE:
      M.field_count = 1;
      M.masked[0] = false;
      break;
    case ST_AMOD_MFA_CHALLENGE:
      M.field_count = 1;
      M.mfa_methods = CCORD_MFA_METHOD_TOTP | CCORD_MFA_METHOD_BACKUP | CCORD_MFA_METHOD_PASSWORD;
      M.mfa_method_idx = first_mfa_method(M.mfa_methods);
      M.masked[0] = (M.mfa_method_idx == 3);
      break;
    default:
      M.kind = ST_AMOD_NONE;
      M.field_count = 0;
      M.focus = -1;
      break;
  }
  ui->account_focus_field = 0;
  if (ui->touch_mode && M.focus >= 0) ui->touch_text_focused = true;
  ui->dirty = true;
}

static void acct_modal_on_done(void *ud, const ccord_account_result *res)
{
  uint64_t gen = (uint64_t)(uintptr_t)ud;
  if (gen != M.cb_gen || !M.owner || M.kind == ST_AMOD_NONE) return;
  ccord_ui *ui = M.owner;
  M.pending = false;

  if (!res) {
    snprintf(M.error, sizeof M.error, "Account request failed.");
    ui->dirty = true;
    return;
  }

  if (res->mfa_challenge_required) {
    // Seamless transition into unified code 60003 MFA verification modal.
    for (int i = 0; i < 3; i++) am_wipe_edit(&M.f[i]);
    M.kind = ST_AMOD_MFA_CHALLENGE;
    M.mfa_methods = res->mfa_methods ? res->mfa_methods : CCORD_MFA_METHOD_TOTP;
    M.mfa_method_idx = first_mfa_method(M.mfa_methods);
    M.field_count = 1;
    M.masked[0] = (M.mfa_method_idx == 3);
    M.focus = 0;
    M.error[0] = '\0';
    M.captcha_fallback = false;
    snprintf(M.info, sizeof M.info, "Discord requires additional verification to complete this action.");
    ui->dirty = true;
    return;
  }

  if (res->status != CCORD_OK) {
    M.captcha_fallback = res->captcha_required;
    if (res->captcha_required) {
      snprintf(M.error, sizeof M.error, "Discord requested a CAPTCHA. Complete this change in your web browser.");
    } else if (res->rate_limited) {
      snprintf(M.error, sizeof M.error, "Too many requests. Try again in %d seconds.", (int)(res->retry_after_s + 0.5));
    } else if (res->message[0]) {
      snprintf(M.error, sizeof M.error, "%s", res->message);
    } else {
      snprintf(M.error, sizeof M.error, "Discord rejected this change (HTTP %ld).", res->http_status);
    }
    ui->dirty = true;
    return;
  }

  // Success paths!
  if (res->op == CCORD_ACCT_OP_PHONE_SEND) {
    M.phone_step = 2;
    M.field_count = 2;
    M.masked[0] = false;
    M.masked[1] = true;
    am_wipe_edit(&M.f[0]);
    am_wipe_edit(&M.f[1]);
    M.focus = 0;
    M.error[0] = '\0';
    snprintf(M.info, sizeof M.info, "SMS code sent to %s. Enter the 6-digit code and your password.", M.phone_pending_num);
    ui->dirty = true;
    return;
  }

  if (res->op == CCORD_ACCT_OP_MFA_BACKUP_CODES) {
    M.showing_codes = true;
    M.backup_count = res->backup_code_count;
    if (M.backup_count > CCORD_ACCOUNT_MAX_BACKUP_CODES) M.backup_count = CCORD_ACCOUNT_MAX_BACKUP_CODES;
    memcpy(M.backup_codes, res->backup_codes, M.backup_count * sizeof M.backup_codes[0]);
    M.focus = -1;
    M.error[0] = '\0';
    snprintf(M.info, sizeof M.info, "Store these one-time recovery codes in a safe place.");
    ui->dirty = true;
    return;
  }

  const char *ok_msg = "Account updated.";
  switch (res->op) {
    case CCORD_ACCT_OP_USERNAME: ok_msg = "Username updated."; break;
    case CCORD_ACCT_OP_EMAIL: ok_msg = "Email updated. Check your inbox to verify."; break;
    case CCORD_ACCT_OP_PHONE_VERIFY: ok_msg = "Phone number verified and linked."; break;
    case CCORD_ACCT_OP_PHONE_REMOVE: ok_msg = "Phone number removed."; break;
    case CCORD_ACCT_OP_PASSWORD:
      ok_msg = res->token_rotated ? "Password changed and session refreshed." : "Password changed.";
      break;
    case CCORD_ACCT_OP_MFA_TOTP_ENABLE: ok_msg = "Authenticator app (2FA) enabled."; break;
    case CCORD_ACCT_OP_MFA_TOTP_DISABLE: ok_msg = "Authenticator app (2FA) disabled."; break;
    case CCORD_ACCT_OP_MFA_SMS_TOGGLE:
      ok_msg = M.sms_enable_target ? "SMS backup authentication enabled." : "SMS backup authentication disabled.";
      break;
    case CCORD_ACCT_OP_DISABLE_ACCOUNT:
      ok_msg = "Account disabled. Signing out.";
      if (ui->d.host.account_action) ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_SIGN_OUT);
      break;
    case CCORD_ACCT_OP_DELETE_ACCOUNT:
      ok_msg = "Account scheduled for deletion. Signing out.";
      if (ui->d.host.account_action) ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_SIGN_OUT);
      break;
    default: break;
  }
  ui_notice(ui, "%s", ok_msg);
  st_account_modal_close(ui);
}

static void acct_modal_submit(ccord_ui *ui, bool regen_codes)
{
  if (!ui || M.pending || M.kind == ST_AMOD_NONE) return;
  M.error[0] = '\0';
  M.captcha_fallback = false;
  if (!ui->d.session) {
    snprintf(M.error, sizeof M.error, "Not connected to Discord.");
    ui->dirty = true;
    return;
  }

  if (M.kind == ST_AMOD_MFA_CHALLENGE) {
    const char *code = ccord_text_edit_cstr(&M.f[0]);
    if (!code || !code[0]) {
      snprintf(M.error, sizeof M.error, "Enter your verification code.");
      ui->dirty = true;
      return;
    }
    int midx = (M.mfa_method_idx >= 0 && M.mfa_method_idx < 4) ? M.mfa_method_idx : 0;
    ccord_error err = {0};
    ccord_status st = ccord_session_account_finish_mfa(ui->d.session, k_mfa_types[midx], code,
                                                       acct_modal_on_done, (void *)(uintptr_t)M.cb_gen, &err);
    am_wipe_edit(&M.f[0]);
    if (st != CCORD_OK) {
      snprintf(M.error, sizeof M.error, "%s", err.message[0] ? err.message : "MFA verification failed.");
    } else {
      M.pending = true;
    }
    ui->dirty = true;
    return;
  }

  if (M.kind == ST_AMOD_DELETE_ACCT && M.field_count == 0) {
    return;   // blocked by owned guilds
  }

  ccord_account_req req = {0};
  const char *v0 = ccord_text_edit_cstr(&M.f[0]);
  const char *v1 = ccord_text_edit_cstr(&M.f[1]);
  const char *v2 = ccord_text_edit_cstr(&M.f[2]);

  switch (M.kind) {
    case ST_AMOD_USERNAME:
      if (!v0[0] || !v1[0]) { snprintf(M.error, sizeof M.error, "Enter a username and current password."); ui->dirty = true; return; }
      req.op = CCORD_ACCT_OP_USERNAME;
      req.value = v0;
      req.password = v1;
      break;
    case ST_AMOD_EMAIL:
      if (!v0[0] || !strchr(v0, '@') || !v1[0]) {
        snprintf(M.error, sizeof M.error, "Enter a valid email address and your current password.");
        ui->dirty = true;
        return;
      }
      req.op = CCORD_ACCT_OP_EMAIL;
      req.value = v0;
      req.password = v1;
      break;
    case ST_AMOD_PHONE_ADD:
      if (M.phone_step == 1) {
        if (!v0[0]) { snprintf(M.error, sizeof M.error, "Enter your phone number (e.g. +15550100199)."); ui->dirty = true; return; }
        snprintf(M.phone_pending_num, sizeof M.phone_pending_num, "%s", v0);
        req.op = CCORD_ACCT_OP_PHONE_SEND;
        req.value = M.phone_pending_num;
      } else {
        if (!v0[0] || !v1[0]) { snprintf(M.error, sizeof M.error, "Enter the 6-digit SMS code and your password."); ui->dirty = true; return; }
        req.op = CCORD_ACCT_OP_PHONE_VERIFY;
        req.value = M.phone_pending_num;
        req.code = v0;
        req.password = v1;
      }
      break;
    case ST_AMOD_PHONE_REMOVE:
      if (!v0[0]) { snprintf(M.error, sizeof M.error, "Enter your current password."); ui->dirty = true; return; }
      req.op = CCORD_ACCT_OP_PHONE_REMOVE;
      req.password = v0;
      break;
    case ST_AMOD_PASSWORD:
      if (!v0[0] || strlen(v1) < 8) {
        snprintf(M.error, sizeof M.error, "Current password required; new password must be at least 8 characters.");
        ui->dirty = true;
        return;
      }
      if (strcmp(v1, v2) != 0) {
        snprintf(M.error, sizeof M.error, "New password and confirmation do not match.");
        ui->dirty = true;
        return;
      }
      req.op = CCORD_ACCT_OP_PASSWORD;
      req.password = v0;
      req.new_password = v1;
      break;
    case ST_AMOD_TOTP_SETUP:
      if (!M.totp_secret[0] || strlen(v0) < 6 || !v1[0]) {
        snprintf(M.error, sizeof M.error, "Enter the 6-digit code from your authenticator app and your password.");
        ui->dirty = true;
        return;
      }
      req.op = CCORD_ACCT_OP_MFA_TOTP_ENABLE;
      req.value = M.totp_secret;
      req.code = v0;
      req.password = v1;
      break;
    case ST_AMOD_TOTP_DISABLE:
      if (strlen(v0) < 6) {
        snprintf(M.error, sizeof M.error, "Enter a 6-digit TOTP code or 8-character backup code.");
        ui->dirty = true;
        return;
      }
      req.op = CCORD_ACCT_OP_MFA_TOTP_DISABLE;
      req.code = v0;
      break;
    case ST_AMOD_BACKUP_CODES:
      if (!v0[0]) { snprintf(M.error, sizeof M.error, "Enter your current password."); ui->dirty = true; return; }
      req.op = CCORD_ACCT_OP_MFA_BACKUP_CODES;
      req.password = v0;
      req.flag = regen_codes;
      break;
    case ST_AMOD_SMS_TOGGLE:
      if (!v0[0]) { snprintf(M.error, sizeof M.error, "Enter your current password."); ui->dirty = true; return; }
      req.op = CCORD_ACCT_OP_MFA_SMS_TOGGLE;
      req.password = v0;
      req.flag = M.sms_enable_target;
      break;
    case ST_AMOD_DISABLE_ACCT:
      if (!v0[0]) { snprintf(M.error, sizeof M.error, "Enter your current password to confirm."); ui->dirty = true; return; }
      req.op = CCORD_ACCT_OP_DISABLE_ACCOUNT;
      req.password = v0;
      break;
    case ST_AMOD_DELETE_ACCT:
      if (!v0[0]) { snprintf(M.error, sizeof M.error, "Enter your current password to confirm deletion."); ui->dirty = true; return; }
      req.op = CCORD_ACCT_OP_DELETE_ACCOUNT;
      req.password = v0;
      break;
    default:
      return;
  }

  ccord_error err = {0};
  ccord_status st = ccord_session_account_submit(ui->d.session, &req, acct_modal_on_done,
                                                 (void *)(uintptr_t)M.cb_gen, &err);
  // Immediately volatile-wipe password fields (except backup codes password kept only until regeneration or close)
  if (M.kind != ST_AMOD_BACKUP_CODES) {
    for (int i = 0; i < M.field_count; i++) {
      if (M.masked[i]) am_wipe_edit(&M.f[i]);
    }
  }
  if (st != CCORD_OK) {
    snprintf(M.error, sizeof M.error, "%s", err.message[0] ? err.message : "Could not submit account update.");
  } else {
    M.pending = true;
  }
  ui->dirty = true;
}

// ---- Drawing ----

void st_account_draw_modal(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r || M.kind == ST_AMOD_NONE) return;

  // Protected Security Surface: always draw under immutable native theme.
  const ccord_theme *saved_theme = ui->theme;
  if (ui->d.protected_theme) ui->theme = ui->d.protected_theme;

  uis_tokens tk = uis_tokens_of(ui);
  bool is_danger = (M.kind == ST_AMOD_DISABLE_ACCT || M.kind == ST_AMOD_DELETE_ACCT || M.kind == ST_AMOD_TOTP_DISABLE ||
                    M.kind == ST_AMOD_PHONE_REMOVE);
  bool is_totp = (M.kind == ST_AMOD_TOTP_SETUP);

  const char *title = "Update Account";
  const char *subtitle = nullptr;
  const char *labels[3] = {nullptr, nullptr, nullptr};
  const char *ph[3] = {"", "", ""};
  const char *submit_lbl = "Save";

  switch (M.kind) {
    case ST_AMOD_USERNAME:
      title = "Change Your Username";
      subtitle = "Enter a new username and your existing password.";
      labels[0] = "USERNAME"; ph[0] = "new_username";
      labels[1] = "CURRENT PASSWORD"; ph[1] = "Password";
      submit_lbl = "Done";
      break;
    case ST_AMOD_EMAIL:
      title = "Change Your Email";
      subtitle = "Discord will send a verification email to your new address.";
      labels[0] = "NEW EMAIL"; ph[0] = "you@example.com";
      labels[1] = "CURRENT PASSWORD"; ph[1] = "Password";
      submit_lbl = "Done";
      break;
    case ST_AMOD_PHONE_ADD:
      title = M.phone_step == 1 ? "Add Phone Number" : "Verify Phone Number";
      subtitle = M.phone_step == 1 ? "Enter your phone number in international format (e.g. +15550100199)."
                                   : "Enter the 6-digit SMS code and confirm with your password.";
      if (M.phone_step == 1) {
        labels[0] = "PHONE NUMBER"; ph[0] = "+15550100199";
        submit_lbl = "Send Code";
      } else {
        labels[0] = "SMS CODE"; ph[0] = "000000";
        labels[1] = "CURRENT PASSWORD"; ph[1] = "Password";
        submit_lbl = "Verify Phone";
      }
      break;
    case ST_AMOD_PHONE_REMOVE:
      title = "Remove Phone Number";
      subtitle = "Confirm your password to unlink your phone number.";
      labels[0] = "CURRENT PASSWORD"; ph[0] = "Password";
      submit_lbl = "Remove";
      break;
    case ST_AMOD_PASSWORD:
      title = "Change Your Password";
      subtitle = "Changing your password rotates your session token automatically.";
      labels[0] = "CURRENT PASSWORD"; ph[0] = "Current password";
      labels[1] = "NEW PASSWORD"; ph[1] = "At least 8 characters";
      labels[2] = "CONFIRM NEW PASSWORD"; ph[2] = "Repeat new password";
      submit_lbl = "Update Password";
      break;
    case ST_AMOD_TOTP_SETUP:
      title = "Set Up Authenticator App";
      subtitle = "Scan the QR code (or copy the key) in your 2FA app, then enter the 6-digit code.";
      labels[0] = "6-DIGIT AUTHENTICATOR CODE"; ph[0] = "000000";
      labels[1] = "CURRENT PASSWORD"; ph[1] = "Password";
      submit_lbl = "Enable 2FA";
      break;
    case ST_AMOD_TOTP_DISABLE:
      title = "Disable Two-Factor Auth";
      subtitle = "Enter a 6-digit authenticator code or 8-character backup code.";
      labels[0] = "2FA OR BACKUP CODE"; ph[0] = "000000";
      submit_lbl = "Disable 2FA";
      break;
    case ST_AMOD_BACKUP_CODES:
      title = "Backup Recovery Codes";
      subtitle = M.showing_codes ? "Each code can only be used once if you lose access to your authenticator."
                                 : "Enter your password to view your Discord backup codes.";
      labels[0] = "CURRENT PASSWORD"; ph[0] = "Password";
      submit_lbl = "View Codes";
      break;
    case ST_AMOD_SMS_TOGGLE:
      title = M.sms_enable_target ? "Enable SMS Backup" : "Disable SMS Backup";
      subtitle = M.sms_enable_target ? "Allow SMS codes as a backup 2FA method on your verified phone."
                                     : "Disable SMS fallback codes for two-factor authentication.";
      labels[0] = "CURRENT PASSWORD"; ph[0] = "Password";
      submit_lbl = M.sms_enable_target ? "Enable SMS" : "Disable SMS";
      break;
    case ST_AMOD_DISABLE_ACCT:
      title = "Disable Account";
      subtitle = "Your account will be disabled until you log back in to restore it.";
      labels[0] = "CURRENT PASSWORD"; ph[0] = "Password";
      submit_lbl = "Disable Account";
      break;
    case ST_AMOD_DELETE_ACCT:
      title = "Delete Account";
      subtitle = "This schedules permanent deletion of your Discord account and cannot be undone.";
      labels[0] = "CURRENT PASSWORD"; ph[0] = "Password";
      submit_lbl = "Delete Account";
      break;
    case ST_AMOD_MFA_CHALLENGE:
      title = "Verify Your Identity";
      subtitle = "Confirm this account change using one of your security methods.";
      labels[0] = M.mfa_method_idx == 3 ? "CURRENT PASSWORD" : "VERIFICATION CODE";
      ph[0] = M.mfa_method_idx == 3 ? "Password" : "000000";
      submit_lbl = "Verify";
      break;
    default: break;
  }

  float mw = is_totp ? 496.0f : UIS_MODAL_W_MD;
  ccord_rect probe = uis_modal_box(ui, mw, 260.0f);
  float inner_w = fmaxf(probe.w - UIS_SP_5 * 2.0f, 160.0f);

  ccord_text_style sub_st = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_secondary));
  ccord_text_style lbl_st = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
  ccord_text_style err_st = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, COL(danger));
  ccord_text_style inf_st = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(brand));

  float sub_h = subtitle ? (ccord_text_measure(r, subtitle, strlen(subtitle), &sub_st, inner_w).y + UIS_SP_3) : 0.0f;
  float qr_h = is_totp ? 168.0f : 0.0f;
  float chips_h = (M.kind == ST_AMOD_MFA_CHALLENGE) ? (UIS_H_CHIP + UIS_SP_3) : 0.0f;
  float codes_h = M.showing_codes ? (fmaxf(1.0f, (float)((M.backup_count + 1u) / 2u)) * 28.0f + 44.0f) : 0.0f;
  float fields_h = M.showing_codes ? 0.0f : ((float)M.field_count * (18.0f + UIS_H_INPUT + UIS_SP_3));
  float info_h = M.info[0] ? (ccord_text_measure(r, M.info, strlen(M.info), &inf_st, inner_w).y + UIS_SP_2) : 0.0f;
  float err_h = M.error[0] ? (ccord_text_measure(r, M.error, strlen(M.error), &err_st, inner_w - 16.0f).y + 20.0f) : 0.0f;
  float cap_h = M.captcha_fallback ? (UIS_H_BTN_SM + UIS_SP_2) : 0.0f;

  float mh = 76.0f + sub_h + info_h + qr_h + chips_h + codes_h + fields_h + err_h + cap_h + UIS_H_BTN + UIS_SP_5;
  if (mh < 210.0f) mh = 210.0f;
  ccord_rect box = uis_modal_box(ui, mw, mh);
  M.box = box;
  inner_w = fmaxf(box.w - UIS_SP_5 * 2.0f, 160.0f);

  uis_modal_frame(ui, r, box);
  ccord_draw_rrect(r, (ccord_rect){box.x, box.y, box.w, 4.0f}, 2.0f, is_danger ? COL(danger) : COL(brand));

  float y = uis_modal_title(ui, r, box, title);
  float x = box.x + UIS_SP_5;

  M.aux_btn = (ccord_rect){0};
  M.aux2_btn = (ccord_rect){0};
  for (int i = 0; i < 3; i++) M.field_rect[i] = (ccord_rect){0};
  for (int i = 0; i < 4; i++) M.mfa_chips[i] = (ccord_rect){0};

  if (subtitle) {
    ccord_text_draw(r, subtitle, strlen(subtitle), x, y, &sub_st, inner_w);
    y += sub_h;
  }
  if (M.info[0]) {
    ccord_text_draw(r, M.info, strlen(M.info), x, y, &inf_st, inner_w);
    y += info_h;
  }

  // TOTP QR Code + Manual Base32 Secret Key block
  if (is_totp) {
    float qr_box_sz = 148.0f;
    ccord_rect qbg = {x, y, qr_box_sz, qr_box_sz};
    ccord_draw_rrect(r, qbg, 6.0f, (ccord_color){255, 255, 255, 255});
    if (M.qr.valid) {
      float quiet = 12.0f;
      float mod_sz = floorf((qr_box_sz - 2.0f * quiet) / (float)QR_V6_SIZE);
      if (mod_sz < 2.0f) mod_sz = 2.0f;
      float ox = qbg.x + floorf((qr_box_sz - mod_sz * (float)QR_V6_SIZE) * 0.5f);
      float oy = qbg.y + floorf((qr_box_sz - mod_sz * (float)QR_V6_SIZE) * 0.5f);
      ccord_color black = {12, 14, 18, 255};
      for (int row = 0; row < QR_V6_SIZE; row++) {
        for (int col = 0; col < QR_V6_SIZE; col++) {
          if (M.qr.mod[row][col]) {
            ccord_draw_rect(r, (ccord_rect){ox + (float)col * mod_sz, oy + (float)row * mod_sz, mod_sz, mod_sz}, black);
          }
        }
      }
    }
    float rx = x + qr_box_sz + UIS_SP_4;
    float rw = fmaxf(inner_w - qr_box_sz - UIS_SP_4, 110.0f);
    ccord_text_draw(r, "MANUAL ENTRY KEY", 16, rx, y + 6.0f, &lbl_st, 0.0f);
    ccord_rect kcard = {rx, y + 24.0f, rw, 54.0f};
    uis_card(ui, r, kcard);
    ccord_text_style ks = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, COL(text_header));
    ccord_text_draw(r, M.totp_secret, strlen(M.totp_secret), kcard.x + 8.0f, kcard.y + 8.0f, &ks, kcard.w - 16.0f);
    M.aux_btn = (ccord_rect){rx, y + 88.0f, fminf(130.0f, rw), UIS_H_BTN_SM};
    uis_button(ui, r, M.aux_btn, "Copy Key", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, M.aux_btn)}, "am_copy", 1);
    y += qr_h;
  }

  // Unified 60003 MFA method selector chips
  if (M.kind == ST_AMOD_MFA_CHALLENGE) {
    float cx = x;
    for (int i = 0; i < 4; i++) {
      if (!(M.mfa_methods & k_mfa_bits[i])) continue;
      float cw = uis_label_width(ui, r, k_mfa_labels[i]);
      if (cx + cw > x + inner_w) break;
      ccord_rect crc = {cx, y, cw, UIS_H_CHIP};
      M.mfa_chips[i] = crc;
      uis_chip(ui, r, crc, k_mfa_labels[i],
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, crc), .selected = M.mfa_method_idx == i},
               "am_mfa", (uint64_t)i);
      cx += cw + UIS_SP_2;
    }
    y += chips_h;
  }

  // Backup codes grid
  if (M.showing_codes) {
    float col_w = (inner_w - UIS_SP_3) * 0.5f;
    ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, COL(text_header));
    ccord_text_style used_st = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_muted));
    for (size_t i = 0; i < M.backup_count; i++) {
      float bx = x + (float)(i & 1u) * (col_w + UIS_SP_3);
      float by = y + (float)(i / 2u) * 28.0f;
      ccord_rect cr = {bx, by, col_w, 24.0f};
      uis_plate(ui, r, cr, tk.r_control, COL(bg_tertiary));
      char line[32];
      snprintf(line, sizeof line, "%s%s", M.backup_codes[i].code, M.backup_codes[i].consumed ? " (used)" : "");
      ccord_text_draw_ellipsis(r, line, strlen(line), cr.x + 8.0f,
                               cr.y + (cr.h - ui_line_h(ui, cs.size)) * 0.5f, cr.w - 16.0f,
                               M.backup_codes[i].consumed ? &used_st : &cs);
    }
    y += fmaxf(1.0f, (float)((M.backup_count + 1u) / 2u)) * 28.0f + UIS_SP_2;
    M.aux_btn = (ccord_rect){x, y, 118.0f, UIS_H_BTN_SM};
    M.aux2_btn = (ccord_rect){x + 126.0f, y, 134.0f, UIS_H_BTN_SM};
    uis_button(ui, r, M.aux_btn, "Copy All", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, M.aux_btn)}, "am_copy", 2);
    uis_button(ui, r, M.aux2_btn, M.pending ? "Working..." : "Regenerate", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, M.aux2_btn), .disabled = M.pending}, "am_regen", 3);
    y += UIS_H_BTN_SM + UIS_SP_3;
  } else {
    for (int i = 0; i < M.field_count; i++) {
      if (labels[i]) ccord_text_draw(r, labels[i], strlen(labels[i]), x, y, &lbl_st, 0.0f);
      y += 18.0f;
      ccord_rect fr = {x, y, inner_w, UIS_H_INPUT};
      M.field_rect[i] = fr;
      (void)uis_input(ui, r, fr, &M.f[i], ph[i],
                      (uis_state){.hover = ui_in_rect(ui->mx, ui->my, fr), .focused = M.focus == i},
                      M.masked[i], "am_fld", (uint64_t)(M.kind * 16 + i));
      if (M.focus == i) ui->text_area = fr;
      y += UIS_H_INPUT + UIS_SP_3;
    }
  }

  if (M.error[0]) {
    ccord_rect erc = {x, y, inner_w, err_h - 6.0f};
    uis_plate(ui, r, erc, tk.r_control, ui_alpha(COL(danger), 0.14f));
    ccord_text_draw(r, M.error, strlen(M.error), erc.x + 8.0f, erc.y + 6.0f, &err_st, inner_w - 16.0f);
    y += err_h;
  }
  if (M.captcha_fallback) {
    M.aux_btn = (ccord_rect){x, y, 186.0f, UIS_H_BTN_SM};
    uis_button(ui, r, M.aux_btn, "Open in Web Browser", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, M.aux_btn)}, "am_cap", 4);
  }

  // Footer
  if (M.showing_codes || (M.kind == ST_AMOD_DELETE_ACCT && M.field_count == 0)) {
    float cw = fmaxf(uis_label_width(ui, r, "Close"), 88.0f);
    M.cancel_btn = uis_modal_footer_button(box, 0, cw);
    M.submit_btn = (ccord_rect){0};
    uis_button(ui, r, M.cancel_btn, "Close", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, M.cancel_btn)}, "am_btn", 1);
  } else {
    const char *sl = M.pending ? "Working..." : submit_lbl;
    float sw = fmaxf(uis_label_width(ui, r, sl), 96.0f);
    float cw = fmaxf(uis_label_width(ui, r, "Cancel"), 82.0f);
    M.submit_btn = uis_modal_footer_button(box, 0, sw);
    M.cancel_btn = (ccord_rect){M.submit_btn.x - UIS_SP_2 - cw, M.submit_btn.y, cw, UIS_H_BTN};
    uis_button(ui, r, M.cancel_btn, "Cancel", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, M.cancel_btn), .disabled = M.pending}, "am_btn", 1);
    uis_button(ui, r, M.submit_btn, sl, is_danger ? UIS_BTN_DANGER : UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){.hover = ui_in_rect(ui->mx, ui->my, M.submit_btn), .disabled = M.pending}, "am_btn", 2);
  }

  ui->theme = saved_theme;
}

bool st_account_modal_handle_click(ccord_ui *ui, float x, float y)
{
  if (M.kind == ST_AMOD_NONE) return false;
  if (!M.pending && (!ui_in_rect(x, y, M.box) || ui_in_rect(x, y, M.cancel_btn))) {
    st_account_modal_close(ui);
    return true;
  }
  if (!M.pending && M.submit_btn.w > 0.0f && ui_in_rect(x, y, M.submit_btn)) {
    acct_modal_submit(ui, false);
    return true;
  }
  if (M.aux_btn.w > 0.0f && ui_in_rect(x, y, M.aux_btn)) {
    if (M.captcha_fallback) {
      ui_open_url(ui, "https://discord.com/settings/account", "Discord Account");
    } else if (M.kind == ST_AMOD_TOTP_SETUP && M.totp_secret[0] && ui->d.host.clipboard_set) {
      ui->d.host.clipboard_set(ui->d.host.ud, M.totp_secret);
      ui_notice(ui, "Authenticator key copied to clipboard.");
    } else if (M.showing_codes && ui->d.host.clipboard_set) {
      char all[256] = {0};
      size_t pos = 0;
      for (size_t i = 0; i < M.backup_count; i++) {
        int n = snprintf(all + pos, sizeof all - pos, "%s%s", i ? "\n" : "", M.backup_codes[i].code);
        if (n > 0 && (size_t)n < sizeof all - pos) pos += (size_t)n;
      }
      ui->d.host.clipboard_set(ui->d.host.ud, all);
      am_wipe_bytes(all, sizeof all);
      ui_notice(ui, "Backup codes copied to clipboard.");
    }
    ui->dirty = true;
    return true;
  }
  if (!M.pending && M.aux2_btn.w > 0.0f && ui_in_rect(x, y, M.aux2_btn)) {
    acct_modal_submit(ui, true);
    return true;
  }
  if (M.kind == ST_AMOD_MFA_CHALLENGE) {
    for (int i = 0; i < 4; i++) {
      if (M.mfa_chips[i].w > 0.0f && ui_in_rect(x, y, M.mfa_chips[i])) {
        M.mfa_method_idx = i;
        M.masked[0] = (i == 3);
        M.focus = 0;
        ui->dirty = true;
        return true;
      }
    }
  }
  M.focus = -1;
  for (int i = 0; i < M.field_count; i++) {
    if (ui_in_rect(x, y, M.field_rect[i])) {
      M.focus = i;
      ui->text_area = M.field_rect[i];
      if (ui->touch_mode) ui->touch_text_focused = true;
      ui->dirty = true;
      return true;
    }
  }
  if (ui->touch_mode) ui->touch_text_focused = false;
  ui->dirty = true;
  return true;
}

bool st_account_modal_handle_key(ccord_ui *ui, const ccord_input_event *ev)
{
  if (M.kind == ST_AMOD_NONE || !ev) return false;
  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_ESCAPE) {
      if (!M.pending) st_account_modal_close(ui);
      return true;
    }
    if (ev->key == CCORD_KEY_TAB && M.field_count > 1 && !M.showing_codes) {
      bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
      if (M.focus < 0) M.focus = 0;
      else M.focus = shift ? (M.focus + M.field_count - 1) % M.field_count : (M.focus + 1) % M.field_count;
      ui->dirty = true;
      return true;
    }
    if (ev->key == CCORD_KEY_RETURN && !M.pending && !M.showing_codes) {
      if (M.focus >= 0 && M.focus + 1 < M.field_count) {
        M.focus++;
        ui->dirty = true;
      } else {
        acct_modal_submit(ui, false);
      }
      return true;
    }
  }
  if (M.focus >= 0 && M.focus < M.field_count && !M.showing_codes) {
    if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
      ccord_error ierr = {0};
      (void)ccord_text_edit_insert(&M.f[M.focus], ev->text, strlen(ev->text), &ierr);
      ui->dirty = true;
      return true;
    }
    if (st_handle_account_field_edit_key(ui, &M.f[M.focus], M.masked[M.focus], ev)) return true;
  }
  return true;
}
