// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

// ---------------------------------------------------------------- overlays: Quick Switcher, Lightbox & Modals

bool dc_substr_ci(const char *hay, const char *needle, size_t nlen)
{
  if (nlen == 0) return true;
  if (!hay) return false;
  size_t hlen = strlen(hay);
  if (hlen < nlen) return false;
  for (size_t i = 0; i + nlen <= hlen; i++) {
    if (dc_ascii_prefix_ci(hay + i, needle, nlen)) return true;
  }
  return false;
}

ccord_rect ui_quick_switcher_rect(const ccord_ui *ui)
{
  float mw = clampf(ui->win_w - 64.0f, 320.0f, 540.0f);
  float mh = fminf(360.0f, ui->win_h - 24.0f);   // a short window keeps the old height: the list scrolls inside it
  if (mh < 160.0f) mh = 160.0f;
  return uis_modal_box(ui, mw, mh);
}

// A new query or a fresh open starts at the first row at the top; a wheel offset must not survive it.
void ui_quick_switcher_reset_list(ccord_ui *ui)
{
  ui->quick_switcher_sel = 0;
  ui->quick_switcher_scroll = 0.0f;
  ccord_scroller_cancel(&ui->quick_switcher_glide);
}

void dc_populate_quick_switcher(ccord_ui *ui)
{
  ui->nquick_switcher_items = 0;
  if (!ui || !ui->d.store) return;
  const char *q = ccord_text_edit_cstr(&ui->quick_switcher_edit);
  size_t qlen = ui->quick_switcher_edit.len;
  char filter = '\0';
  if (qlen > 0 && (q[0] == '@' || q[0] == '#' || q[0] == '!' || q[0] == '*')) {
    filter = q[0];
    q++;
    qlen--;
  }
  const ccord_store *st = ui->d.store;

  // 1. DM channels (@)
  if (filter == '\0' || filter == '@') {
    size_t dmc = ccord_store_channel_count(st, CCORD_DM_GUILD_ID);
    for (size_t i = 0; i < dmc && ui->nquick_switcher_items < 10; i++) {
      const ccord_channel *c = ccord_store_channel_at(st, CCORD_DM_GUILD_ID, i);
      if (!c || c->is_message_request) continue;
      const char *dname = ui_dm_display_name(ui, c);
      if (!dc_substr_ci(dname, q, qlen) && !dc_substr_ci(c->name, q, qlen)) continue;
      auto it = &ui->quick_switcher_items[ui->nquick_switcher_items++];
      it->guild_id = CCORD_DM_GUILD_ID;
      it->channel_id = c->id;
      it->user_id = c->user_id;
      snprintf(it->label, sizeof it->label, "@ %s", dname ? dname : c->name);
      snprintf(it->sub, sizeof it->sub, "Direct Message");
    }
  }

  // 2. Guild channels (# text, ! voice) and Guilds (*)
  size_t gc = ccord_store_guild_count(st);
  for (size_t g = 0; g < gc && ui->nquick_switcher_items < 10; g++) {
    const ccord_guild *gd = ccord_store_guild_at(st, g);
    if (!gd) continue;
    if ((filter == '\0' || filter == '*') && dc_substr_ci(gd->name, q, qlen) && ui->nquick_switcher_items < 10) {
      auto it = &ui->quick_switcher_items[ui->nquick_switcher_items++];
      it->guild_id = gd->id;
      it->channel_id = CCORD_ID_NONE;
      it->user_id = CCORD_ID_NONE;
      snprintf(it->label, sizeof it->label, "* %s", gd->name);
      snprintf(it->sub, sizeof it->sub, "Server");
    }
    if (filter == '*' || filter == '@') continue;
    size_t cc = ccord_store_channel_count(st, gd->id);
    for (size_t c = 0; c < cc && ui->nquick_switcher_items < 10; c++) {
      const ccord_channel *ch = ccord_store_channel_at(st, gd->id, c);
      if (!ch || ch->kind == CCORD_CHANNEL_CATEGORY || !ui_channel_visible(ui, gd->id, ch)) continue;
      if (filter == '#' && ch->kind != CCORD_CHANNEL_TEXT) continue;
      if (filter == '!' && ch->kind != CCORD_CHANNEL_VOICE) continue;
      if (!dc_substr_ci(ch->name, q, qlen)) continue;
      auto it = &ui->quick_switcher_items[ui->nquick_switcher_items++];
      it->guild_id = gd->id;
      it->channel_id = ch->id;
      it->user_id = CCORD_ID_NONE;
      snprintf(it->label, sizeof it->label, "%s %s", ch->kind == CCORD_CHANNEL_VOICE ? "!" : "#", ch->name);
      snprintf(it->sub, sizeof it->sub, "%s", gd->name ? gd->name : "");
    }
  }
  if (ui->quick_switcher_sel >= ui->nquick_switcher_items) ui_quick_switcher_reset_list(ui);
}

void dc_activate_quick_switcher_item(ccord_ui *ui, size_t idx)
{
  if (!ui || idx >= ui->nquick_switcher_items) return;
  auto it = &ui->quick_switcher_items[idx];
  ui->quick_switcher_open = false;
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  ccord_error err;
  if (it->guild_id != CCORD_ID_NONE) {
    if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, it->guild_id, &err);
    else (void)ccord_store_select_guild(ui->d.store, it->guild_id, &err);
  }
  if (it->channel_id != CCORD_ID_NONE) {
    if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, it->channel_id, &err);
    else (void)ccord_store_select_channel(ui->d.store, it->channel_id, &err);
  }
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_save_current_location(ui);
  ui->dirty = true;
}

// ---- modal geometry (shared by drawing and hit-testing, so the two cannot drift apart) ----


static const confirm_choice k_timeout_choices[6] = {
  {"60 secs", 60LL * 1000},        {"5 mins", 5LL * 60 * 1000},    {"10 mins", 10LL * 60 * 1000},
  {"1 hour", 60LL * 60 * 1000},    {"1 day", 24LL * 60 * 60 * 1000}, {"1 week", 7LL * 24 * 60 * 60 * 1000},
};
static const confirm_choice k_ban_choices[5] = {
  {"None", 0}, {"1 hour", 3600}, {"24 hours", 86400}, {"3 days", 3 * 86400}, {"7 days", 7 * 86400},
};

const confirm_choice *dc_confirm_choices(ccord_admin_op op, size_t *n)
{
  if (op == CCORD_ADMIN_TIMEOUT) {
    *n = sizeof k_timeout_choices / sizeof k_timeout_choices[0];
    return k_timeout_choices;
  }
  if (op == CCORD_ADMIN_BAN) {
    *n = sizeof k_ban_choices / sizeof k_ban_choices[0];
    return k_ban_choices;
  }
  *n = 0;
  return nullptr;
}

const uint32_t dc_k_folder_color_presets[6] = {0u, 0x5865F2u, 0x57F287u, 0xFEE75Cu, 0xEB459Eu, 0xED4245u};

bool dc_parse_folder_hex_color(const char *s, uint32_t *out)
{
  *out = 0u;
  if (!s) return true;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '\0') return true;
  if (*s == '#') s++;
  uint32_t val = 0u;
  size_t digits = 0;
  while (s[digits] && s[digits] != ' ' && s[digits] != '\t') {
    unsigned char ch = (unsigned char)s[digits];
    uint32_t nib = 0;
    if (ch >= '0' && ch <= '9') nib = (uint32_t)(ch - '0');
    else if (ch >= 'a' && ch <= 'f') nib = (uint32_t)(ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') nib = (uint32_t)(ch - 'A' + 10);
    else return false;
    if (digits >= 6u) return false;
    val = (val << 4u) | nib;
    digits++;
  }
  const char *tail = s + digits;
  while (*tail == ' ' || *tail == '\t') tail++;
  if (*tail != '\0' || digits != 6u) return false;
  *out = val;
  return true;
}


modal_geom dc_modal_layout(const ccord_ui *ui)
{
  modal_geom g = {0};
  float mw = clampf(ui->win_w - 64.0f, 300.0f, ui->modal_kind == UI_MODAL_ADD_SERVER ? 460.0f : 440.0f);
  float mh = 220.0f;
  if (ui->modal_kind == UI_MODAL_CONFIRM) {
    size_t n = 0;
    (void)dc_confirm_choices(ui->confirm_op, &n);
    bool no_input = (ui->confirm_op == CCORD_ADMIN_LEAVE_GUILD || ui->confirm_op == CCORD_ADMIN_TRANSFER_OWNERSHIP);
    mh = 56.0f + 64.0f + (n ? 56.0f : 0.0f) + (no_input ? 0.0f : 52.0f) + 62.0f;   // title, body, choices, input, buttons
  } else if (ui->modal_kind == UI_MODAL_ADD_SERVER) {
    mh = (ui->modal_type_sel == 0) ? 318.0f : 246.0f;
  } else if (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS) {
    mh = 296.0f;
  }
  g.box = uis_modal_box(ui, mw, mh);
  g.ok = (ccord_rect){g.box.x + g.box.w - 160.0f, g.box.y + g.box.h - 46.0f, 140.0f, UIS_H_BTN};
  if (ui->modal_kind == UI_MODAL_ADD_SERVER) {
    float tw = (mw - 48.0f) * 0.5f;
    g.tabs[0] = (ccord_rect){g.box.x + 20.0f, g.box.y + 55.0f, tw, UIS_H_BTN_SM};
    g.tabs[1] = (ccord_rect){g.tabs[0].x + tw + 8.0f, g.box.y + 55.0f, tw, UIS_H_BTN_SM};
    if (ui->modal_type_sel == 0) {
      g.input = (ccord_rect){g.box.x + 20.0f, g.box.y + 114.0f, mw - 40.0f - 92.0f, 36.0f};
      g.preview_btn = (ccord_rect){g.input.x + g.input.w + 8.0f, g.input.y + 2.0f, 84.0f, UIS_H_BTN};
    } else {
      g.input = (ccord_rect){g.box.x + 20.0f, g.box.y + 114.0f, mw - 40.0f, 36.0f};
    }
    g.cancel = (ccord_rect){g.ok.x - 100.0f, g.ok.y, 90.0f, 32.0f};
    return g;
  }
  if (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS) {
    g.input = (ccord_rect){g.box.x + 20.0f, g.box.y + 74.0f, mw - 40.0f, 36.0f};
    g.input2 = (ccord_rect){g.box.x + 20.0f, g.box.y + 142.0f, mw - 40.0f, 36.0f};
    float gap = 8.0f, inner = mw - 40.0f;
    float cw = (inner - gap * 5.0f) / 6.0f;
    for (size_t i = 0; i < 6u; i++) {
      g.chip[i] = (ccord_rect){g.box.x + 20.0f + (float)i * (cw + gap), g.box.y + 190.0f, cw, 28.0f};
    }
    g.nchips = 6u;
    g.cancel = (ccord_rect){g.ok.x - 100.0f, g.ok.y, 90.0f, 32.0f};
    return g;
  }
  if (ui->modal_kind != UI_MODAL_CONFIRM) {
    g.input = (ccord_rect){g.box.x + 20.0f, g.box.y + 64.0f, g.box.w - 40.0f, 36.0f};
    g.cancel = (ccord_rect){g.ok.x - 100.0f, g.ok.y, 90.0f, UIS_H_BTN};
    return g;
  }
  g.cancel = (ccord_rect){g.ok.x - 100.0f, g.ok.y, 90.0f, 32.0f};
  size_t n = 0;
  (void)dc_confirm_choices(ui->confirm_op, &n);
  g.body_y = g.box.y + 54.0f;
  float y = g.body_y + 64.0f;
  if (n > 0 && n <= UI_CONFIRM_MAX_CHIPS) {
    g.choices_label_y = y;
    float gap = 6.0f, inner = g.box.w - 40.0f;
    float cw = (inner - gap * (float)(n - 1)) / (float)n;
    for (size_t i = 0; i < n; i++) {
      g.chip[i] = (ccord_rect){g.box.x + 20.0f + (float)i * (cw + gap), y + 20.0f, cw, 28.0f};
    }
    g.nchips = n;
    y += 56.0f;
  }
  if (ui->confirm_op != CCORD_ADMIN_LEAVE_GUILD && ui->confirm_op != CCORD_ADMIN_TRANSFER_OWNERSHIP) {
    g.input = (ccord_rect){g.box.x + 20.0f, y, g.box.w - 40.0f, 36.0f};
  }
  return g;
}

void ui_confirm_open(ccord_ui *ui, ccord_admin_op op, ccord_id guild_id, ccord_id target_id, const char *name)
{
  ui->modal_kind = UI_MODAL_CONFIRM;
  ui->confirm_op = op;
  ui->modal_guild_id = guild_id;
  ui->modal_target_id = target_id;
  ui->modal_type_sel = op == CCORD_ADMIN_TIMEOUT ? UI_CONFIRM_DEFAULT_TIMEOUT : 0;
  (void)snprintf(ui->confirm_name, sizeof ui->confirm_name, "%s", name ? name : "");
  ccord_text_edit_clear(&ui->modal_edit1);
  ui->dirty = true;
}

bool dc_has_global_name_differs(const char *global, const char *shown)
{
  return global && global[0] && (!shown || strcmp(global, shown) != 0);
}

static bool new_dm_has_recipient(const ccord_ui *ui, ccord_id uid)
{
  for (size_t i = 0; i < ui->new_dm_recipient_count; i++) {
    if (ui->new_dm_recipients[i] == uid) return true;
  }
  return false;
}

void dc_new_dm_toggle_recipient(ccord_ui *ui, ccord_id uid, size_t max_add)
{
  for (size_t i = 0; i < ui->new_dm_recipient_count; i++) {
    if (ui->new_dm_recipients[i] == uid) {
      for (size_t k = i + 1u; k < ui->new_dm_recipient_count; k++) {
        ui->new_dm_recipients[k - 1u] = ui->new_dm_recipients[k];
      }
      ui->new_dm_recipient_count--;
      ui->dirty = true;
      return;
    }
  }
  if (ui->new_dm_recipient_count >= max_add || ui->new_dm_recipient_count >= CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX) {
    ui_notice(ui, "Group DMs can have at most 10 members");
    return;
  }
  ui->new_dm_recipients[ui->new_dm_recipient_count++] = uid;
  ui->dirty = true;
}

static void draw_group_dm_modal(ccord_ui *ui, ccord_renderer *r, bool is_add)
{
  ui_popup_begin(ui, r, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS, 10.0f);
  float mw = clampf(ui->win_w - 64.0f, 320.0f, 460.0f);
  float mh = is_add ? 380.0f : 436.0f;
  ccord_rect m = uis_modal_box(ui, mw, mh);
  uis_modal_frame(ui, r, m);

  const ccord_channel *ech = is_add ? ccord_store_channel(ui->d.store, ui->modal_target_id) : nullptr;
  size_t existing = (ech && ech->recipient_count <= CCORD_MAX_DM_RECIPIENTS) ? ech->recipient_count : 0u;
  size_t max_add = existing < CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX ? (CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX - existing) : 0u;
  size_t rem_slots = max_add > ui->new_dm_recipient_count ? (max_add - ui->new_dm_recipient_count) : 0u;

  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  const char *title = is_add ? "Add Friends to DM" : "Select Friends";
  ccord_text_draw(r, title, strlen(title), m.x + 20.0f, m.y + 16.0f, &ts, 0.0f);

  char sub_buf[96];
  snprintf(sub_buf, sizeof sub_buf, "You can add %zu more friend%s.", rem_slots, rem_slots == 1u ? "" : "s");
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, sub_buf, strlen(sub_buf), m.x + 20.0f, m.y + 42.0f, m.w - 40.0f, &ss);

  ccord_rect inp = {m.x + 20.0f, m.y + 66.0f, m.w - 40.0f, 36.0f};
  ui->modal_input_rect = inp;
  const char *v1 = ccord_text_edit_cstr(&ui->modal_edit1);
  (void)uis_input(ui, r, inp, &ui->modal_edit1, "Type the username or ID of a friend",
                  (uis_state){.focused = ui->modal_focus_field == 0}, false, "gdmin", 0);

  float fy = inp.y + inp.h + 10.0f;
  float list_bottom = is_add ? (m.y + m.h - 58.0f) : (m.y + m.h - 118.0f);
  ccord_id self_id = ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;
  size_t nrel = ui->d.store ? ccord_store_relationship_count(ui->d.store) : 0u;
  size_t qlen = strlen(v1);
  ccord_text_style fns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_style fhs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));

  for (size_t i = 0; i < nrel && ui->nnew_dm_friend_hits < 8u && fy + 30.0f <= list_bottom; i++) {
    const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
    if (!rel || rel->kind != CCORD_RELATIONSHIP_FRIEND || rel->user_id == self_id) continue;
    if (ech) {
      bool already_in = false;
      for (size_t ri = 0; ri < ech->recipient_count && ri < CCORD_MAX_DM_RECIPIENTS; ri++) {
        if (ech->recipient_ids[ri] == rel->user_id) {
          already_in = true;
          break;
        }
      }
      if (already_in) continue;
    }
    const ccord_user *fu = ccord_store_user(ui->d.store, rel->user_id);
    const char *fname = (fu && fu->display_name && fu->display_name[0]) ? fu->display_name
                        : ((fu && fu->name && fu->name[0]) ? fu->name : "Friend");
    const char *fhand = (fu && fu->handle && fu->handle[0]) ? fu->handle : fname;
    if (qlen > 0 && strchr(v1, ',') == nullptr && !dc_substr_ci(fname, v1, qlen) && !dc_substr_ci(fhand, v1, qlen)) continue;

    ccord_rect fr = {m.x + 16.0f, fy, m.w - 32.0f, 28.0f};
    bool fhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, fr);
    bool sel = new_dm_has_recipient(ui, rel->user_id);
    uis_plate(ui, r, fr, uis_tokens_of(ui).r_control, COL(bg_tertiary));
    uis_list_row_fill(ui, r, fr, (uis_state){.hover = fhv, .selected = sel}, "gdmrow", (uint64_t)rel->user_id);
    ui_draw_avatar(ui, r, fu, fr.x + 16.0f, fr.y + fr.h * 0.5f, 20.0f, COL(bg_tertiary), false);
    float nw = ccord_text_draw_ellipsis(r, fname, strlen(fname), fr.x + 32.0f, fr.y + 5.0f, fr.w - 120.0f, &fns);
    char htag[80];
    snprintf(htag, sizeof htag, "@%s", fhand);
    ccord_text_draw_ellipsis(r, htag, strlen(htag), fr.x + 38.0f + nw, fr.y + 7.0f,
                             maxf(fr.w - (64.0f + nw), 16.0f), &fhs);
    ccord_rect cb = {fr.x + fr.w - 22.0f, fr.y + 7.0f, 14.0f, 14.0f};
    if (sel) {
      ccord_draw_rrect(r, cb, 3.0f, COL(brand));
      ccord_draw_rrect(r, (ccord_rect){cb.x + 4.0f, cb.y + 4.0f, 6.0f, 6.0f}, 1.5f, COL(text_on_brand));
    } else {
      ccord_draw_rrect(r, cb, 3.0f, COL(text_muted));
      ccord_draw_rrect(r, (ccord_rect){cb.x + 1.5f, cb.y + 1.5f, 11.0f, 11.0f}, 2.0f, COL(bg_secondary));
    }
    ui->new_dm_friend_hits[ui->nnew_dm_friend_hits].rect = fr;
    ui->new_dm_friend_hits[ui->nnew_dm_friend_hits].user_id = rel->user_id;
    ui->nnew_dm_friend_hits++;
    fy += 31.0f;
  }

  if (!is_add) {
    ccord_text_style gls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
    ccord_text_draw(r, "GROUP NAME (OPTIONAL)", 21, m.x + 20.0f, m.y + m.h - 108.0f, &gls, 0.0f);
    ccord_rect inp2 = {m.x + 20.0f, m.y + m.h - 92.0f, m.w - 40.0f, UIS_H_INPUT};
    ui->modal_input2_rect = inp2;
    (void)uis_input(ui, r, inp2, &ui->modal_edit2, "Optional group name", (uis_state){.focused = ui->modal_focus_field == 1}, false,
                    "gdmin2", 0);
  }

  ccord_rect ok_r = {m.x + m.w - 160.0f, m.y + m.h - 44.0f, 140.0f, 32.0f};
  ccord_rect cn_r = {ok_r.x - 98.0f, ok_r.y, 88.0f, 32.0f};
  ui->modal_ok_rect = ok_r;
  ui->inter_modal_box = m;
  ui->inter_modal_cancel = cn_r;
  uis_button(ui, r, cn_r, "Cancel", UIS_BTN_LINK, UIS_BTN_S_MD,
             (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cn_r)}, "modcancel", 0);
  uis_button(ui, r, ok_r, is_add ? "Add" : (ui->new_dm_recipient_count >= 2u ? "Create Group DM" : "Create DM"), UIS_BTN_PRIMARY,
             UIS_BTN_S_MD, (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ok_r)}, "modok", 0);
  ui_popup_end(r);
}

static void draw_edit_group_dm_modal(ccord_ui *ui, ccord_renderer *r)
{
  ui_popup_begin(ui, r, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS, 10.0f);
  float mw = clampf(ui->win_w - 64.0f, 300.0f, 440.0f), mh = 248.0f;
  ccord_rect m = uis_modal_box(ui, mw, mh);
  ui->inter_modal_box = m;
  uis_modal_frame(ui, r, m);

  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_draw(r, "Edit Group DM", 13, m.x + 20.0f, m.y + 18.0f, &ts, 0.0f);
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
  ccord_text_draw(r, "GROUP NAME", 10, m.x + 20.0f, m.y + 56.0f, &ls, 0.0f);

  ccord_rect inp = {m.x + 20.0f, m.y + 74.0f, m.w - 40.0f, 36.0f};
  ui->modal_input_rect = inp;
  (void)uis_input(ui, r, inp, &ui->modal_edit1, "Group DM Name", (uis_state){.focused = true}, false, "egdmin", 0);

  ccord_rect ic_chip = {m.x + 20.0f, m.y + 124.0f, 180.0f, 28.0f};
  ui->modal_tab_rects[0] = ic_chip;
  bool clr_icon = (ui->modal_type_sel == 1);
  uis_tab(ui, r, ic_chip, clr_icon ? "Removing Group Icon" : "Remove Group Icon",
          (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ic_chip), .selected = clr_icon}, "egdmicon", 0);

  ccord_rect ok_r = {m.x + m.w - 150.0f, m.y + m.h - 44.0f, 130.0f, 32.0f};
  ccord_rect cn_r = {ok_r.x - 96.0f, ok_r.y, 86.0f, 32.0f};
  ui->modal_ok_rect = ok_r;
  ui->inter_modal_cancel = cn_r;
  uis_button(ui, r, cn_r, "Cancel", UIS_BTN_LINK, UIS_BTN_S_MD,
             (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cn_r)}, "modcancel", 0);
  uis_button(ui, r, ok_r, "Save Changes", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ok_r)}, "modok", 0);
  ui_popup_end(r);
}

// Forgets the hit rectangles of every action modal: only the one drawn this frame may be clicked.
static void modal_reset_hits(ccord_ui *ui)
{
  ui->inter_modal_box = (ccord_rect){0, 0, 0, 0};
  ui->inter_modal_ok = (ccord_rect){0, 0, 0, 0};
  ui->inter_modal_cancel = (ccord_rect){0, 0, 0, 0};
  ui->modal_tab_rects[0] = (ccord_rect){0, 0, 0, 0};
  ui->modal_tab_rects[1] = (ccord_rect){0, 0, 0, 0};
  ui->modal_preview_btn_rect = (ccord_rect){0, 0, 0, 0};
  ui->modal_ok_rect = (ccord_rect){0, 0, 0, 0};
  ui->modal_input_rect = (ccord_rect){0, 0, 0, 0};
  ui->modal_input2_rect = (ccord_rect){0, 0, 0, 0};
  ui->nnew_dm_friend_hits = 0;
  ui->profile_modal_box = (ccord_rect){0, 0, 0, 0};
  ui->profile_modal_close_rect = (ccord_rect){0, 0, 0, 0};
  ui->profile_modal_msg_rect = (ccord_rect){0, 0, 0, 0};
  memset(ui->profile_modal_tab_rects, 0, sizeof ui->profile_modal_tab_rects);
  ui->profile_modal_note_rect = (ccord_rect){0, 0, 0, 0};
  ui->profile_modal_note_save_rect = (ccord_rect){0, 0, 0, 0};
  ui->nprofile_modal_mutual_hits = 0;
  ui->nprofile_modal_conn_hits = 0;
  ui->mv.box = ui->mv.view = ui->mv.cancel = ui->mv.submit = (ccord_rect){0, 0, 0, 0};
  memset(ui->mv.checks, 0, sizeof ui->mv.checks);
  for (size_t fi = 0; fi < CCORD_RICH_MAX_COMPONENT_ROWS; fi++) {
    ui->inter_modal_field_rects[fi] = (ccord_rect){0, 0, 0, 0};
  }
}

void ui_draw_overlays(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r) return;

  // M5 voice surfaces borrow the generic modal routing (UI_MODAL_EDIT_CHANNEL); intercept before the
  // legacy modal code so the prompt / DAVE / RTC dialogs own the screen while they are open.
  if (ui_call_prompt_active(ui)) {
    ui_call_prompt_draw(ui, r);
    return;
  }
  if (ui_dave_modal_active(ui)) {
    ui_dave_modal_draw(ui, r);
    return;
  }
  // Scheduled events / stage lifecycle prompts borrow their own modal kind; same interception.
  if (ui_scheduled_events_active(ui)) {
    ui_scheduled_events_draw(ui, r);
    return;
  }
  // The Forward channel picker owns the screen while its modal kind is set.
  if (ui_forward_picker_active(ui)) {
    ui_forward_picker_draw(ui, r);
    return;
  }
  // The Activities shelf owns the screen while its modal kind is set.
  if (ui_activity_shelf_active(ui)) {
    ui_activity_shelf_draw(ui, r);
    return;
  }

  // 1. Inline Image Lightbox
  if (!ui->lightbox_open) {
    if (ui->lightbox_urls) ui_close_lightbox(ui);
    ui->lightbox_img_rect = (ccord_rect){0, 0, 0, 0};
    ui->lightbox_open_btn = (ccord_rect){0, 0, 0, 0};
    ui->lightbox_prev_btn = (ccord_rect){0, 0, 0, 0};
    ui->lightbox_next_btn = (ccord_rect){0, 0, 0, 0};
    ui->lightbox_close_btn = (ccord_rect){0, 0, 0, 0};
    ui->lightbox_zoom_btn = (ccord_rect){0, 0, 0, 0};
  }
  if (ui->lightbox_open && (ui->lightbox_url[0] || ui->lightbox_count > 0)) {
    if (ui->lightbox_count == 0 && ui->lightbox_url[0]) {
      const char *one = ui->lightbox_url;
      ui_open_lightbox(ui, &one, 1u, 0u);
    }
    if (ui->lightbox_index >= ui->lightbox_count && ui->lightbox_count > 0) {
      ui->lightbox_index = ui->lightbox_count - 1u;
    }
    if (ui->lightbox_urls && ui->lightbox_count > 0) {
      ui_url_copy(ui->lightbox_url, sizeof ui->lightbox_url, ui->lightbox_urls[ui->lightbox_index]);
    }
    if (ui->lightbox_zoom < 0.5f || ui->lightbox_zoom > 4.0f) ui->lightbox_zoom = 1.0f;

    ui_popup_begin(ui, r, "lightbox", 0, UI_MODAL_MS, 8.0f);
    ccord_draw_rect(r, (ccord_rect){0, 0, ui->win_w, ui->win_h}, COL(overlay));
    bool lb_vid = ui_url_path_ends(ui->lightbox_url, ".mp4") || ui_url_path_ends(ui->lightbox_url, ".webm") ||
                  ui_url_path_ends(ui->lightbox_url, ".mov") || ui_url_path_ends(ui->lightbox_url, ".m4v");
    (void)ui_ensure_media_ex(ui, r, ui->lightbox_url, 0, lb_vid);
    int tw = 0, th = 0;
    bool has_tex = ccord_render_has_texture(r, ui->lightbox_url, &tw, &th);
    float max_w = clampf(ui->win_w * 0.74f, 220.0f, 920.0f);
    float max_h = clampf(ui->win_h * 0.68f, 160.0f, 680.0f);
    float base_w = fminf(max_w, 480.0f), base_h = fminf(max_h, 320.0f);
    if (has_tex && tw > 4 && th > 4) {
      float scale = fminf(max_w / (float)tw, max_h / (float)th);
      if (scale > 2.0f) scale = 2.0f;
      base_w = maxf((float)tw * scale, 120.0f);
      base_h = maxf((float)th * scale, 90.0f);
    }
    float iw = base_w * ui->lightbox_zoom;
    float ih = base_h * ui->lightbox_zoom;
    ccord_rect img_r = {(ui->win_w - iw) * 0.5f, (ui->win_h - ih) * 0.5f - 14.0f, iw, ih};
    ui->lightbox_img_rect = img_r;
    ccord_draw_rrect(r, (ccord_rect){img_r.x - 4.0f, img_r.y - 4.0f, img_r.w + 8.0f, img_r.h + 8.0f}, 8.0f,
                     COL(bg_secondary));
    if (has_tex) {
      ccord_draw_texture_rounded(r, ui->lightbox_url, img_r, 6.0f, 255);
    } else {
      ui_icon_attachment(r, img_r.x + img_r.w * 0.5f, img_r.y + img_r.h * 0.5f, 32.0f, COL(text_secondary));
    }

    float bar_y = clampf(img_r.y + img_r.h + 10.0f, 16.0f, maxf(ui->win_h - 42.0f, 16.0f));
    float bar_x = clampf(img_r.x, 24.0f, maxf(ui->win_w - 280.0f, 24.0f));
    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_link));
    const char *open_lbl = "Open in Browser";
    ccord_vec2 ot = ccord_text_measure(r, open_lbl, strlen(open_lbl), &ls, 0.0f);
    ui->lightbox_open_btn = (ccord_rect){bar_x, bar_y, ot.x + 20.0f, 28.0f};
    ccord_draw_rrect(r, ui->lightbox_open_btn, 5.0f, COL(bg_secondary));
    ccord_text_draw(r, open_lbl, strlen(open_lbl), bar_x + 10.0f, bar_y + (28.0f - ot.y) * 0.5f, &ls, 0.0f);

    char zbuf[24];
    snprintf(zbuf, sizeof zbuf, "%d%%", (int)lroundf(ui->lightbox_zoom * 100.0f));
    ccord_text_style zs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));
    ccord_vec2 zt = ccord_text_measure(r, zbuf, strlen(zbuf), &zs, 0.0f);
    ui->lightbox_zoom_btn = (ccord_rect){ui->lightbox_open_btn.x + ui->lightbox_open_btn.w + 8.0f, bar_y,
                                         zt.x + 18.0f, 28.0f};
    ccord_draw_rrect(r, ui->lightbox_zoom_btn, 5.0f, COL(bg_secondary));
    ccord_text_draw(r, zbuf, strlen(zbuf), ui->lightbox_zoom_btn.x + 9.0f, bar_y + (28.0f - zt.y) * 0.5f, &zs, 0.0f);

    ui->lightbox_close_btn = (ccord_rect){ui->win_w - 48.0f, 16.0f, 32.0f, 32.0f};
    ccord_draw_rrect(r, ui->lightbox_close_btn, 16.0f, COL(bg_secondary));
    ui_icon_close(r, ui->lightbox_close_btn.x + 16.0f, ui->lightbox_close_btn.y + 16.0f, 14.0f, COL(text_header));

    if (ui->lightbox_count > 1u) {
      char cbuf[32];
      snprintf(cbuf, sizeof cbuf, "%zu / %zu", ui->lightbox_index + 1u, ui->lightbox_count);
      ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_secondary));
      ccord_vec2 ct = ccord_text_measure(r, cbuf, strlen(cbuf), &cs, 0.0f);
      ccord_rect cpill = {ui->lightbox_zoom_btn.x + ui->lightbox_zoom_btn.w + 8.0f, bar_y, ct.x + 18.0f, 28.0f};
      ccord_draw_rrect(r, cpill, 5.0f, COL(bg_secondary));
      ccord_text_draw(r, cbuf, strlen(cbuf), cpill.x + 9.0f, bar_y + (28.0f - ct.y) * 0.5f, &cs, 0.0f);

      ui->lightbox_prev_btn = (ccord_rect){20.0f, (ui->win_h - 44.0f) * 0.5f, 40.0f, 44.0f};
      ui->lightbox_next_btn = (ccord_rect){ui->win_w - 60.0f, (ui->win_h - 44.0f) * 0.5f, 40.0f, 44.0f};
      ccord_draw_rrect(r, ui->lightbox_prev_btn, 8.0f, COL(bg_secondary));
      ccord_draw_rrect(r, ui->lightbox_next_btn, 8.0f, COL(bg_secondary));
      ccord_text_style as = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 2.0f, COL(text_header));
      ccord_vec2 lt = ccord_text_measure(r, "<", 1, &as, 0.0f);
      ccord_vec2 rt = ccord_text_measure(r, ">", 1, &as, 0.0f);
      ccord_text_draw(r, "<", 1, ui->lightbox_prev_btn.x + (40.0f - lt.x) * 0.5f,
                      ui->lightbox_prev_btn.y + (44.0f - lt.y) * 0.5f, &as, 0.0f);
      ccord_text_draw(r, ">", 1, ui->lightbox_next_btn.x + (40.0f - rt.x) * 0.5f,
                      ui->lightbox_next_btn.y + (44.0f - rt.y) * 0.5f, &as, 0.0f);
    } else {
      ui->lightbox_prev_btn = (ccord_rect){0, 0, 0, 0};
      ui->lightbox_next_btn = (ccord_rect){0, 0, 0, 0};
    }
    ui_popup_end(r);
  }

  // 2. Quick Switcher (Ctrl+K)
  if (ui->quick_switcher_open) {
    dc_populate_quick_switcher(ui);
    ui->quick_switcher_thumb = (ccord_rect){0, 0, 0, 0};
    ui_popup_begin(ui, r, "qswitch", 0, UI_MODAL_MS, 8.0f);
    ccord_rect m = ui_quick_switcher_rect(ui);
    uis_modal_frame(ui, r, m);
    ccord_rect inp = {m.x + UIS_SP_4, m.y + UIS_SP_4, m.w - 2.0f * UIS_SP_4, UIS_H_INPUT};
    (void)uis_input(ui, r, inp, &ui->quick_switcher_edit, "Where would you like to go? (@user, #channel, *server)",
                    (uis_state){.focused = true}, false, "qswin", 0);
    // The result list scrolls inside the dialog so every match stays reachable on short windows.
    ccord_rect lview = {m.x + 12.0f, inp.y + inp.h + 12.0f, m.w - 24.0f, m.y + m.h - 10.0f - (inp.y + inp.h + 12.0f)};
    if (lview.h < 0.0f) lview.h = 0.0f;
    float content_h = (float)ui->nquick_switcher_items * 30.0f;
    float max_s = maxf(content_h - lview.h, 0.0f);
    ui->quick_switcher_scroll = clampf(ui->quick_switcher_scroll, 0.0f, max_s);
    // Follow the selection only when it moved (keys, reopen), so a wheel offset is not snapped back each frame.
    if (ui->quick_switcher_sel != ui->quick_switcher_sel_seen && ui->quick_switcher_sel < ui->nquick_switcher_items) {
      float sel_top = (float)ui->quick_switcher_sel * 30.0f;
      float sel_bot = sel_top + 28.0f;
      ccord_scroller_cancel(&ui->quick_switcher_glide);   // the follow-scroll replaces a wheel glide (scroll.h)
      if (sel_top < ui->quick_switcher_scroll) ui->quick_switcher_scroll = sel_top;
      else if (sel_bot > ui->quick_switcher_scroll + lview.h)
        ui->quick_switcher_scroll = clampf(sel_bot - lview.h, 0.0f, max_s);
    }
    ui->quick_switcher_sel_seen = ui->quick_switcher_sel;
    ccord_push_clip(r, lview);
    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    for (size_t i = 0; i < ui->nquick_switcher_items; i++) {
      auto it = &ui->quick_switcher_items[i];
      float ry = lview.y + (float)i * 30.0f - ui->quick_switcher_scroll;
      if (ry + 28.0f < lview.y || ry > lview.y + lview.h) {
        it->rect = (ccord_rect){0, 0, 0, 0};
        continue;
      }
      ccord_rect rr = {lview.x, ry, lview.w, 28.0f};
      it->rect = rr;
      uis_list_row_fill(ui, r, rr, (uis_state){.selected = i == ui->quick_switcher_sel}, "qsw", i);
      ccord_text_draw_ellipsis(r, it->label, strlen(it->label), rr.x + 10.0f, rr.y + 4.0f, rr.w - 140.0f, &ls);
      ccord_vec2 st = ccord_text_measure(r, it->sub, strlen(it->sub), &ss, 0.0f);
      ccord_text_draw_ellipsis(r, it->sub, strlen(it->sub), rr.x + rr.w - fminf(st.x, 120.0f) - 10.0f,
                               rr.y + 6.0f, 120.0f, &ss);
    }
    ccord_pop_clip(r);
    ui->quick_switcher_thumb = ui_draw_list_scrollbar(ui, r, lview, content_h, ui->quick_switcher_scroll,
                                                      "qswitchlist", 0);
    ui_popup_end(r);
  }

  // 2b. Message Component Select Menu Dropdown
  ui->select_dropdown_rect = (ccord_rect){0, 0, 0, 0};
  ui->select_confirm_btn = (ccord_rect){0, 0, 0, 0};
  if (ui->select_open && ui->select_target && ui->select_opts) {
    bool is_multi = ui->select_target->max_values > 1u;
    size_t nopt = ui->select_opt_count;
    float row_h = 34.0f;
    float list_h = (nopt > 0 ? (float)nopt : 1.0f) * row_h + 8.0f;
    float foot_h = is_multi ? 38.0f : 0.0f;
    float dw = clampf(maxf(ui->select_anchor_rect.w, 240.0f), 180.0f, maxf(ui->win_w - 24.0f, 180.0f));
    float dh = list_h + foot_h;
    float dx = clampf(ui->select_anchor_rect.x, 8.0f, maxf(ui->win_w - dw - 8.0f, 8.0f));
    float dy = ui->select_anchor_rect.y + ui->select_anchor_rect.h + 4.0f;
    if (dy + dh > ui->win_h - 8.0f && ui->select_anchor_rect.y - dh - 4.0f >= 8.0f) {
      dy = ui->select_anchor_rect.y - dh - 4.0f;
    }
    dy = clampf(dy, 8.0f, maxf(ui->win_h - dh - 8.0f, 8.0f));
    ccord_rect dbox = {dx, dy, dw, dh};
    ui->select_dropdown_rect = dbox;
    ui_popup_begin(ui, r, "seldd", ui->select_target->message_id ^ (uint64_t)ui->select_target->row_idx, UI_POPUP_MS, 4.0f);
    uis_popover_frame(ui, r, dbox);
    ccord_text_style lst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style dst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
    float oy = dbox.y + 4.0f;
    if (nopt == 0) {
      ccord_text_draw(r, "No options available", 20, dbox.x + 12.0f, oy + 8.0f, &dst, 0.0f);
    }
    for (size_t i = 0; i < nopt; i++) {
      ccord_rect or_ = {dbox.x + 4.0f, oy, dbox.w - 8.0f, row_h - 2.0f};
      ui->select_opt_rects[i] = or_;
      bool ohov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, or_);
      uis_list_row_fill(ui, r, or_, (uis_state){.hover = ohov, .selected = ui->select_opts[i].selected}, "selopt", i);
      float tx = or_.x + 10.0f;
      if (is_multi) {
        ccord_rect cb = {or_.x + 8.0f, or_.y + (or_.h - 14.0f) * 0.5f, 14.0f, 14.0f};
        if (ui->select_opts[i].selected) {
          ccord_draw_rrect(r, cb, 3.0f, COL(brand));
          ccord_draw_rrect(r, (ccord_rect){cb.x + 4.0f, cb.y + 4.0f, 6.0f, 6.0f}, 1.5f, COL(text_on_brand));
        } else {
          ccord_draw_rrect(r, cb, 3.0f, COL(text_muted));
          ccord_draw_rrect(r, (ccord_rect){cb.x + 1.5f, cb.y + 1.5f, 11.0f, 11.0f}, 2.0f, COL(bg_secondary));
        }
        tx = or_.x + 28.0f;
      } else if (ui->select_opts[i].selected) {
        ccord_draw_rrect(r, (ccord_rect){or_.x + or_.w - 16.0f, or_.y + (or_.h - 8.0f) * 0.5f, 8.0f, 8.0f}, 4.0f, COL(brand));
      }
      if (ui->select_opts[i].desc[0]) {
        ccord_text_draw_ellipsis(r, ui->select_opts[i].label, strlen(ui->select_opts[i].label), tx, or_.y + 2.0f,
                                 maxf(or_.w - (tx - or_.x) - 24.0f, 16.0f), &lst);
        ccord_text_draw_ellipsis(r, ui->select_opts[i].desc, strlen(ui->select_opts[i].desc), tx, or_.y + 17.0f,
                                 maxf(or_.w - (tx - or_.x) - 24.0f, 16.0f), &dst);
      } else {
        float llh = ui_line_h(ui, lst.size);
        ccord_text_draw_ellipsis(r, ui->select_opts[i].label, strlen(ui->select_opts[i].label), tx,
                                 or_.y + (or_.h - llh) * 0.5f, maxf(or_.w - (tx - or_.x) - 24.0f, 16.0f), &lst);
      }
      oy += row_h;
    }
    if (is_multi) {
      float fy = dbox.y + list_h;
      uis_divider(ui, r, dbox.x + 6.0f, fy, dbox.w - 12.0f);
      ccord_rect abtn = {dbox.x + dbox.w - 76.0f, fy + 5.0f, 68.0f, UIS_H_BTN_SM};
      ui->select_confirm_btn = abtn;
      uis_button(ui, r, abtn, "Apply", UIS_BTN_PRIMARY, UIS_BTN_S_SM,
                 (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, abtn)}, "selapply", 0);
    }
    ui_popup_end(r);
  }

  // 3. Action Modals (Add Friend, New DM, Create/Edit Channel, Change Nickname, Create Thread, Confirm, Interaction, Add Server, Folder Settings)
  modal_reset_hits(ui);
  // The modal fades in and out as a whole (ui_overlay_begin); a closed one is drawn while it fades out with its kind
  // put back for that call only, so the handlers still see no modal and a click goes to what is underneath.
  float modal_t = ui_overlay_presence(ui, UI_OVL_MODAL, ui->modal_kind != UI_MODAL_NONE, UI_MODAL_MS,
                                      UI_MODAL_CLOSE_MS);
  if (ui->modal_kind != UI_MODAL_NONE) {
    // The dialog's own entrance (per kind, starts over after a frame it was not open) or the presence, whichever is
    // further along: reopened mid-exit it continues from where it is.
    ui->modal_ghost_kind = ui->modal_kind;
    float enter = ui_enter(ui, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS);
    if (enter > modal_t) modal_t = enter;
  }
  bool modal_fading = ui->modal_kind == UI_MODAL_NONE && ui->modal_ghost_kind != UI_MODAL_NONE &&
                      modal_t > UI_PRESENCE_EPS;
  if (modal_fading) ui->modal_kind = ui->modal_ghost_kind;
  bool modal_drawn = ui->modal_kind != UI_MODAL_NONE;
  if (modal_drawn) ui_overlay_begin(ui, r, UI_OVL_MODAL, modal_t, 10.0f);
  if (ui->modal_kind == UI_MODAL_USER_PROFILE) {
    dc_draw_user_profile_modal(ui, r);
  } else if (ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT) {
    draw_group_dm_modal(ui, r, ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT);
  } else if (ui->modal_kind == UI_MODAL_EDIT_GROUP_DM) {
    draw_edit_group_dm_modal(ui, r);
  } else if (ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION) {
    ui_member_verification_draw(ui, r);
  } else if (ui->modal_kind == UI_MODAL_INTERACTION && ui->inter_modal && ui->inter_modal_edits) {
    ui_popup_begin(ui, r, "imodal", 1u, UI_MODAL_MS, 10.0f);
    uis_tokens mtk = uis_tokens_of(ui);
    float mw = clampf(ui->win_w - 64.0f, 320.0f, 460.0f);
    float mh = 56.0f + 56.0f;
    for (size_t fi = 0; fi < ui->inter_modal->field_count; fi++) {
      mh += 22.0f + (ui->inter_modal->fields[fi].style == 2 ? 72.0f : 36.0f) + 14.0f;
    }
    ccord_rect m = uis_modal_box(ui, mw, mh);
    ui->inter_modal_box = m;
    uis_modal_frame(ui, r, m);
    const char *mtitle = ui->inter_modal->title[0] ? ui->inter_modal->title : "Form";
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
    ccord_text_draw_ellipsis(r, mtitle, strlen(mtitle), m.x + 20.0f, m.y + 18.0f, m.w - 40.0f, &ts);
    float fy = m.y + 56.0f;
    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_secondary));
    ccord_text_style rs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(danger));
    for (size_t fi = 0; fi < ui->inter_modal->field_count; fi++) {
      const ccord_modal_field *fld = &ui->inter_modal->fields[fi];
      const char *flbl = fld->label[0] ? fld->label : "Input";
      float lw = ccord_text_draw_ellipsis(r, flbl, strlen(flbl), m.x + 20.0f, fy, m.w - 52.0f, &ls);
      if (fld->required) {
        ccord_text_draw(r, " *", 2, m.x + 20.0f + lw, fy, &rs, 0.0f);
      }
      fy += 22.0f;
      float fh = fld->style == 2 ? 72.0f : 36.0f;
      ccord_rect fbox = {m.x + 20.0f, fy, m.w - 40.0f, fh};
      ui->inter_modal_field_rects[fi] = fbox;
      bool focused = (fi == ui->inter_modal_focus);
      if (fld->style != 2) {
        (void)uis_input(ui, r, fbox, &ui->inter_modal_edits[fi], fld->placeholder, (uis_state){.focused = focused}, false,
                        "imodin", (uint64_t)fi);
      } else {
        if (focused) {
          ccord_draw_rrect(r, (ccord_rect){fbox.x - 1.0f, fbox.y - 1.0f, fbox.w + 2.0f, fbox.h + 2.0f}, mtk.r_control + 1.0f,
                           COL(brand));
        }
        ccord_draw_rrect(r, fbox, mtk.r_control, COL(bg_input));   // multi-line field: no shared helper yet
      }
      const char *fval = ccord_text_edit_cstr(&ui->inter_modal_edits[fi]);
      bool has_val = fval[0] != '\0';
      const char *fdisp = has_val ? fval : fld->placeholder;
      ccord_text_style vst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base),
                                      has_val ? COL(text_normal) : COL(text_placeholder));
      if (fld->style == 2) {
        ccord_push_clip(r, fbox);
        if (fdisp[0]) ccord_text_draw(r, fdisp, strlen(fdisp), fbox.x + 10.0f, fbox.y + 8.0f, &vst, maxf(fbox.w - 20.0f, 20.0f));
        ccord_pop_clip(r);
      }
      fy += fh + 14.0f;
    }
    ccord_rect ok_r = uis_modal_footer_button(m, 0, 84.0f);
    ccord_rect cn_r = {ok_r.x - UIS_SP_2 - 76.0f, ok_r.y, 76.0f, ok_r.h};
    ui->inter_modal_ok = ok_r;
    ui->inter_modal_cancel = cn_r;
    uis_button(ui, r, cn_r, "Cancel", UIS_BTN_LINK, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cn_r)}, "imodcn", 0);
    uis_button(ui, r, ok_r, "Submit", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ok_r)}, "imodok", 0);
    ui_popup_end(r);
  } else if (ui->modal_kind == UI_MODAL_ADD_SERVER) {
    ui_popup_begin(ui, r, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS, 10.0f);
    modal_geom mg = dc_modal_layout(ui);
    ccord_rect m = mg.box;
    ui->modal_tab_rects[0] = mg.tabs[0];
    ui->modal_tab_rects[1] = mg.tabs[1];
    ui->modal_preview_btn_rect = mg.preview_btn;
    ui->modal_ok_rect = mg.ok;
    ui->modal_input_rect = mg.input;
    uis_modal_frame(ui, r, m);

    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
    const char *mtitle = (ui->modal_type_sel == 0) ? "Join a Server" : "Create Your Server";
    ccord_text_draw(r, mtitle, strlen(mtitle), m.x + 20.0f, m.y + 18.0f, &ts, 0.0f);

    static const char *const k_add_tabs[2] = {"Join a Server", "Create a Server"};
    for (int t = 0; t < 2; t++) {
      uis_tab(ui, r, mg.tabs[t], k_add_tabs[t],
              (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.tabs[t]), .selected = ui->modal_type_sel == t},
              "addsrvtab", (uint64_t)t);
    }

    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
    const char *flbl = (ui->modal_type_sel == 0) ? "INVITE LINK OR CODE" : "SERVER NAME";
    ccord_text_draw(r, flbl, strlen(flbl), m.x + 20.0f, m.y + 94.0f, &ls, 0.0f);

    ccord_rect inp = mg.input;
    (void)uis_input(ui, r, inp, &ui->modal_edit1,
                    (ui->modal_type_sel == 0) ? "https://discord.gg/linux-gaming" : "My New Server",
                    (uis_state){.focused = true}, false, "addsrvin", 0);

    if (ui->modal_type_sel == 0) {
      uis_button(ui, r, mg.preview_btn, "Preview", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
                 (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.preview_btn)}, "addsrvprev", 0);

      bool prev_loading = false;
      const ccord_invite_preview *prev = ui->d.session
        ? ccord_session_invite_preview(ui->d.session, &prev_loading)
        : nullptr;
      ccord_rect pcard = {m.x + 20.0f, m.y + 162.0f, m.w - 40.0f, 92.0f};
      uis_plate(ui, r, pcard, uis_tokens_of(ui).r_surface, COL(bg_tertiary));
      if (prev && prev->guild_id != CCORD_ID_NONE) {
        ccord_rect pic = {pcard.x + 12.0f, pcard.y + 12.0f, 40.0f, 40.0f};
        if (prev->guild_icon_url[0] && ui_ensure_media(ui, r, prev->guild_icon_url)) {
          ccord_draw_texture_rounded(r, prev->guild_icon_url, pic, 10.0f, 255);
        } else {
          ccord_draw_rrect(r, pic, 10.0f, COL(brand));
          char init[8];
          ui_initials(prev->guild_name, init);
          ccord_text_style ist = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_on_brand));
          ccord_vec2 it = ccord_text_measure(r, init, strlen(init), &ist, 0.0f);
          ccord_text_draw(r, init, strlen(init), pic.x + (pic.w - it.x) * 0.5f, pic.y + (pic.h - it.y) * 0.5f, &ist, 0.0f);
        }
        ccord_text_style pns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
        ccord_text_draw_ellipsis(r, prev->guild_name, strlen(prev->guild_name), pic.x + 50.0f, pcard.y + 13.0f,
                                 pcard.w - 72.0f, &pns);
        char pstats[96];
        snprintf(pstats, sizeof pstats, "%u Online  •  %u Members",
                 (unsigned)prev->online_count, (unsigned)prev->member_count);
        ccord_text_style pms = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
        ccord_text_draw_ellipsis(r, pstats, strlen(pstats), pic.x + 50.0f, pcard.y + 34.0f, pcard.w - 72.0f, &pms);
        const char *pdesc = prev->guild_description[0] ? prev->guild_description : prev->channel_name;
        if (pdesc[0]) {
          ccord_text_style pds = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_secondary));
          ccord_text_draw_ellipsis(r, pdesc, strlen(pdesc), pcard.x + 12.0f, pcard.y + 62.0f, pcard.w - 24.0f, &pds);
        }
      } else {
        const char *hint = prev_loading
          ? "Fetching invite preview..."
          : "Enter an invite code or discord.gg link and click Preview, or Join Server directly.";
        ccord_text_style hst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
        ccord_text_draw(r, hint, strlen(hint), pcard.x + 12.0f, pcard.y + 14.0f, &hst, pcard.w - 24.0f);
      }
    } else {
      const char *chint = "By creating a server, you get text and voice channels ready to invite friends.";
      ccord_text_style hst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
      ccord_text_draw(r, chint, strlen(chint), m.x + 20.0f, m.y + 160.0f, &hst, m.w - 40.0f);
    }

    uis_button(ui, r, mg.cancel, "Cancel", UIS_BTN_LINK, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.cancel)}, "modcancel", 0);
    uis_button(ui, r, mg.ok, (ui->modal_type_sel == 0) ? "Join Server" : "Create Server", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.ok)}, "modok", 0);
    ui_popup_end(r);
  } else if (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS) {
    ui_popup_begin(ui, r, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS, 10.0f);
    modal_geom mg = dc_modal_layout(ui);
    ccord_rect m = mg.box;
    ui->modal_ok_rect = mg.ok;
    ui->modal_input_rect = mg.input;
    ui->modal_input2_rect = mg.input2;
    uis_modal_frame(ui, r, m);

    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
    ccord_text_draw(r, "Folder Settings", 15, m.x + 20.0f, m.y + 18.0f, &ts, 0.0f);

    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
    ccord_text_draw(r, "FOLDER NAME", 11, m.x + 20.0f, m.y + 54.0f, &ls, 0.0f);
    ccord_text_draw(r, "FOLDER COLOR (#RRGGBB)", 22, m.x + 20.0f, m.y + 122.0f, &ls, 0.0f);

    ccord_rect boxes[2] = {mg.input, mg.input2};
    ccord_text_edit *edits[2] = {&ui->modal_edit1, &ui->modal_edit2};
    const char *phs[2] = {"Server Folder", "#5865F2"};
    for (int fi = 0; fi < 2; fi++) {
      (void)uis_input(ui, r, boxes[fi], edits[fi], phs[fi], (uis_state){.focused = ui->modal_focus_field == fi}, false,
                      "foldin", (uint64_t)fi);
    }

    for (size_t i = 0; i < mg.nchips; i++) {
      uis_state cst = {.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.chip[i]), .selected = (int)i == ui->modal_type_sel};
      if (i == 0) {
        uis_tab(ui, r, mg.chip[i], "Auto", cst, "foldcol", i);
      } else {
        ccord_rect sw = {mg.chip[i].x + (mg.chip[i].w - UIS_SWATCH_D) * 0.5f, mg.chip[i].y + (mg.chip[i].h - UIS_SWATCH_D) * 0.5f,
                         UIS_SWATCH_D, UIS_SWATCH_D};
        uis_swatch(ui, r, sw, ui_rgb_color(dc_k_folder_color_presets[i]), cst, "foldcol", i);
      }
    }

    uis_button(ui, r, mg.cancel, "Cancel", UIS_BTN_LINK, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.cancel)}, "modcancel", 0);
    uis_button(ui, r, mg.ok, "Save", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.ok)}, "modok", 0);
    ui_popup_end(r);
  } else if (ui->modal_kind != UI_MODAL_NONE) {
    ui_popup_begin(ui, r, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS, 10.0f);
    modal_geom mg = dc_modal_layout(ui);
    ui->modal_ok_rect = mg.ok;
    ui->modal_input_rect = mg.input;
    ccord_rect m = mg.box;
    uis_modal_frame(ui, r, m);
    const char *title = "Modal";
    const char *placeholder = "Enter value...";
    const char *btn_lbl = "Save";
    char title_buf[CCORD_NAME_MAX_BYTES + 24];
    char body_buf[CCORD_NAME_MAX_BYTES + 160];
    const char *body = nullptr;
    const char *choices_label = nullptr;
    switch (ui->modal_kind) {
      case UI_MODAL_ADD_FRIEND:
        title = "Add Friend";
        placeholder = "You can add friends with their Discord username or ID";
        btn_lbl = "Send Friend Request";
        break;
      case UI_MODAL_NEW_DM:
        title = "New Direct Message";
        placeholder = "Enter username or User ID to message";
        btn_lbl = "Create DM";
        break;
      case UI_MODAL_CREATE_CHANNEL:
        title = "Create Channel";
        placeholder = "new-channel";
        btn_lbl = "Create Channel";
        break;
      case UI_MODAL_EDIT_CHANNEL:
        title = "Edit Channel";
        placeholder = "channel-name";
        btn_lbl = "Save Changes";
        break;
      case UI_MODAL_CHANGE_NICK:
        title = "Change Server Nickname";
        placeholder = "Enter a new nickname";
        btn_lbl = "Save";
        break;
      case UI_MODAL_USER_NOTE:
        title = "Add Note";
        placeholder = "Click to add a note (only visible to you)";
        btn_lbl = "Save";
        break;
      case UI_MODAL_FRIEND_NICK:
        title = "Friend Nickname";
        placeholder = "Enter a nickname for this friend";
        btn_lbl = "Save";
        break;
      case UI_MODAL_CREATE_THREAD:
        title = "Create Thread";
        placeholder = "Thread Name";
        btn_lbl = "Create Thread";
        break;
      case UI_MODAL_CONFIRM: {
        const char *who = ui->confirm_name[0] ? ui->confirm_name : "this";
        placeholder = "Reason (shows in the audit log, optional)";
        switch (ui->confirm_op) {
          case CCORD_ADMIN_DELETE_CHANNEL:
            title = "Delete Channel";
            btn_lbl = "Delete Channel";
            (void)snprintf(body_buf, sizeof body_buf, "Are you sure you want to delete %s? This cannot be undone.", who);
            break;
          case CCORD_ADMIN_DELETE_ROLE:
            title = "Delete Role";
            btn_lbl = "Delete Role";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to delete the role %s? Members lose it and the permissions it gave them.", who);
            break;
          case CCORD_ADMIN_REVOKE_INVITE:
            title = "Revoke Invite";
            btn_lbl = "Revoke Invite";
            {
              // Streamer Mode: `who` is the invite code; mask it with Hide Instant Invite Links.
              char who_mask[sizeof ui->confirm_name];
              const char *shown_who = ui->confirm_name[0] ? ui_streamer_mask_code(ui, who, who_mask, sizeof who_mask, true) : who;
              (void)snprintf(body_buf, sizeof body_buf, "Are you sure you want to revoke the invite %s? Its link will stop working.",
                             shown_who);
            }
            break;
          case CCORD_ADMIN_KICK:
            (void)snprintf(title_buf, sizeof title_buf, "Kick %s", who);
            title = title_buf;
            btn_lbl = "Kick";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to kick %s from the server? They can rejoin with a new invite.", who);
            break;
          case CCORD_ADMIN_BAN:
            (void)snprintf(title_buf, sizeof title_buf, "Ban %s", who);
            title = title_buf;
            btn_lbl = "Ban";
            choices_label = "DELETE MESSAGE HISTORY";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to ban %s? They can't rejoin until you unban them.", who);
            break;
          case CCORD_ADMIN_TIMEOUT:
            (void)snprintf(title_buf, sizeof title_buf, "Timeout %s", who);
            title = title_buf;
            btn_lbl = "Timeout";
            choices_label = "DURATION";
            (void)snprintf(body_buf, sizeof body_buf,
                           "%s won't be able to send messages, react, or speak in voice channels until it ends.", who);
            break;
          case CCORD_ADMIN_LEAVE_GUILD:
            (void)snprintf(title_buf, sizeof title_buf, "Leave '%s'", who);
            title = title_buf;
            btn_lbl = "Leave Server";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to leave %s? You won't be able to rejoin unless you are re-invited.", who);
            break;
          case CCORD_ADMIN_DELETE_GUILD:
            (void)snprintf(title_buf, sizeof title_buf, "Delete '%s'", who);
            title = title_buf;
            btn_lbl = "Delete Server";
            placeholder = "Enter server name to confirm";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to delete %s? This cannot be undone. Type the server name below to confirm.",
                           who);
            break;
          case CCORD_ADMIN_TRANSFER_OWNERSHIP: {
            const ccord_guild *tgd = ui->d.store ? ccord_store_guild(ui->d.store, ui->modal_guild_id) : nullptr;
            const char *gname = (tgd && tgd->name && tgd->name[0]) ? tgd->name : "this server";
            title = "Transfer Server Ownership";
            btn_lbl = "Transfer Ownership";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Transfer ownership of %s to %s? You will no longer be the server owner.", gname, who);
            break;
          }
          case CCORD_ADMIN_DELETE_EMOJI:
            title = "Delete Emoji";
            btn_lbl = "Delete Emoji";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to delete :%s:? This cannot be undone.", who);
            break;
          case CCORD_ADMIN_DELETE_STICKER:
            title = "Delete Sticker";
            btn_lbl = "Delete Sticker";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to delete the sticker %s? This cannot be undone.", who);
            break;
          case CCORD_ADMIN_DELETE_SOUNDBOARD:
            title = "Delete Sound";
            btn_lbl = "Delete Sound";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to delete the soundboard sound %s? This cannot be undone.", who);
            break;
          default:
            body_buf[0] = '\0';
            break;
        }
        body = body_buf;
        break;
      }
      default:
        break;
    }
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
    ccord_text_draw(r, title, strlen(title), m.x + 20.0f, m.y + 18.0f, &ts, 0.0f);
    if (body) {
      ccord_text_style bodys = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
      ccord_text_draw(r, body, strlen(body), m.x + 20.0f, mg.body_y, &bodys, m.w - 40.0f);
    }
    if (choices_label && mg.nchips > 0) {
      ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
      ccord_text_draw(r, choices_label, strlen(choices_label), m.x + 20.0f, mg.choices_label_y, &ls, 0.0f);
      size_t nch = 0;
      const confirm_choice *choices = dc_confirm_choices(ui->confirm_op, &nch);
      for (size_t i = 0; i < mg.nchips && i < nch; i++) {
        uis_tab(ui, r, mg.chip[i], choices[i].label,
                (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.chip[i]), .selected = (int)i == ui->modal_type_sel},
                "confchoice", (uint64_t)i);
      }
    }
    ccord_rect inp = mg.input;
    if (inp.w > 0.0f) {
      (void)uis_input(ui, r, inp, &ui->modal_edit1, placeholder, (uis_state){.focused = ui->modal_focus_field == 0}, false,
                      "modin", 0);
    }
    bool danger = ui->modal_kind == UI_MODAL_CONFIRM;
    uis_button(ui, r, mg.cancel, "Cancel", UIS_BTN_LINK, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.cancel)}, "modcancel", 0);
    uis_button(ui, r, mg.ok, btn_lbl, danger ? UIS_BTN_DANGER : UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.ok)}, "modok", 0);
    ui_popup_end(r);
  }
  if (modal_drawn) ui_overlay_end(ui, r);
  if (modal_fading) {
    ui->modal_kind = UI_MODAL_NONE;
    modal_reset_hits(ui);
  } else if (ui->modal_kind != UI_MODAL_INTERACTION && ui->inter_modal) {
    ui_free_inter_modal(ui);
  }
  if (!ui->select_open && (ui->select_opts || ui->select_target)) {
    ui_close_select_dropdown(ui);
  }

  // M5: DAVE badge / RTC button by the voice bar, then the ring banner & DM call bar on top.
  ui_dave_draw_badge(ui, r);
  ui_call_draw(ui, r);
}

// Builds and sends the call the dialog was asking about. `reason` is the optional audit-log note.
