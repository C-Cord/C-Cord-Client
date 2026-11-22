// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "server_settings_internal.h"

// ---------------------------------------------------------------- Overview

static void ss_radio_row(ss_cx *c, ccord_rect row, const char *title, const char *desc, bool sel, const char *why,
                         ss_act act, uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  if (!ss_visible(c, row)) return;
  bool hov = ss_hit_add(c, row, act, arg, why, name) && why == nullptr;
  uint64_t id = ui_name_id(name);
  if (!desc || !desc[0]) {
    uis_radio(ui, c->r, row, title, sel, (uis_state){.hover = hov, .disabled = why != nullptr}, "ssrad", id);
    return;
  }
  // With a caption the row fill covers the whole row, and the radio and its title sit on the top band.
  uis_list_row_fill(ui, c->r, row, (uis_state){.hover = hov, .selected = sel, .disabled = why != nullptr}, "ssradrow", id);
  ccord_rect band = {row.x, row.y, row.w, UIS_H_BTN};
  uis_radio(ui, c->r, band, title, sel, (uis_state){.disabled = why != nullptr}, "ssrad", id);
  (void)ss_text(c, desc, row.x + UIS_SP_2 + UIS_RADIO_D + UIS_SP_3, row.y + UIS_H_BTN, SS_CAP(c), false, COL(text_muted),
                row.w - 52.0f);
}

void ss_draw_overview(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float x = g->col_x, w = g->col_w, y = g->top_y - ss->scroll;
  ss_title(c, g, "Server Overview", y);
  y += 58.0f;
  ss_label(c, "SERVER NAME", x, y);
  y += 22.0f;
  ss_field_box(c, (ccord_rect){x, y, w, 42.0f}, &ss->name_edit, SSF_NAME, "Server name", nullptr, "name");
  y += 42.0f + 30.0f;

  char idbuf[24];
  (void)ccord_id_format(gd->id, idbuf);
  ss_label(c, "SERVER ID", x, y);
  y += 22.0f;
  (void)ss_text(c, idbuf, x, y + 6.0f, M(font_size_base), false, COL(text_normal), w - 130.0f);
  ss_button(c, (ccord_rect){x + w - 110.0f, y, 110.0f, 30.0f}, "Copy ID", BTN_SECONDARY, nullptr, A_COPY_ID, 0, "copy-id");
  y += 30.0f + 30.0f;

  if (gd->owner_id != CCORD_ID_NONE) {   // not told who owns it: say nothing rather than "Unknown"
    ss_label(c, "OWNER", x, y);
    y += 22.0f;
    const ccord_user *owner = ccord_store_user(ui->d.store, gd->owner_id);
    (void)ss_text(c, owner && owner->name && owner->name[0] ? owner->name : "Unknown member", x, y, M(font_size_base), false,
                  COL(text_normal), w);
    y += 22.0f + 30.0f;
  }

  ss_label(c, "MEMBERS", x, y);
  y += 22.0f;
  char mb[96];
  if (gd->premium_subscription_count > 0 || gd->premium_tier > 0) {
    (void)snprintf(mb, sizeof mb, "%zu loaded in this client  -  Boost Level %u (%u boosts)",
                   ccord_store_member_count(ui->d.store, gd->id), (unsigned)gd->premium_tier,
                   (unsigned)gd->premium_subscription_count);
  } else {
    (void)snprintf(mb, sizeof mb, "%zu loaded in this client", ccord_store_member_count(ui->d.store, gd->id));
  }
  (void)ss_text(c, mb, x, y, M(font_size_base), false, COL(text_normal), w);
  y += 22.0f + 30.0f;

  // ---- Server Icon & Banner ----
  ss_label(c, "SERVER ICON & BANNER", x, y);
  y += 24.0f;
  bool store_has_icon = gd->icon_url && gd->icon_url[0] != '\0';
  const char *icon_state = ss->ov_icon_mode == SS_IMG_SET ? "Staged new icon (Save Changes to apply)"
                         : ss->ov_icon_mode == SS_IMG_CLEAR ? "Icon will be removed on Save"
                         : store_has_icon ? "Custom server icon active" : "Default monogram icon";
  (void)ss_text(c, icon_state, x, y + 7.0f, M(font_size_small), false, COL(text_secondary), w - 250.0f);
  ss_button(c, (ccord_rect){x + w - 236.0f, y, 122.0f, 32.0f}, "Upload Icon", BTN_SECONDARY, nullptr,
            A_ICON_UPLOAD, 0, "icon-upload");
  const char *icon_clr_why = (!store_has_icon && ss->ov_icon_mode != SS_IMG_SET) ? "This server has no custom icon." : nullptr;
  ss_button(c, (ccord_rect){x + w - 106.0f, y, 106.0f, 32.0f}, "Remove Icon", BTN_LINK, icon_clr_why,
            A_ICON_CLEAR, 0, "icon-clear");
  y += 32.0f + 14.0f;

  bool can_banner = gd->feature_banner || gd->premium_tier >= 2u;
  bool store_has_banner = gd->banner_url[0] != '\0';
  const char *banner_state = ss->ov_banner_mode == SS_IMG_SET ? "Staged new banner (Save Changes to apply)"
                           : ss->ov_banner_mode == SS_IMG_CLEAR ? "Banner will be removed on Save"
                           : store_has_banner ? "Custom server banner active"
                           : can_banner ? "No banner uploaded" : "Requires Server Boost Level 2";
  (void)ss_text(c, banner_state, x, y + 7.0f, M(font_size_small), false, COL(text_secondary), w - 250.0f);
  ss_button(c, (ccord_rect){x + w - 236.0f, y, 122.0f, 32.0f}, "Upload Banner", BTN_SECONDARY,
            can_banner ? nullptr : "Server Banner requires Server Boost Level 2.", A_BANNER_UPLOAD, 0, "banner-upload");
  const char *ban_clr_why = (!store_has_banner && ss->ov_banner_mode != SS_IMG_SET) ? "This server has no banner." : nullptr;
  ss_button(c, (ccord_rect){x + w - 106.0f, y, 106.0f, 32.0f}, "Remove Banner", BTN_LINK, ban_clr_why,
            A_BANNER_CLEAR, 0, "banner-clear");
  y += 32.0f + 30.0f;

  // ---- System Messages Channel & Flags ----
  ss_label(c, "SYSTEM MESSAGES CHANNEL", x, y);
  y += 24.0f;
  const ccord_channel *sys_ch = ss->ov_work_cfg.system_channel_id != CCORD_ID_NONE
                                  ? ccord_store_channel(ui->d.store, ss->ov_work_cfg.system_channel_id)
                                  : nullptr;
  char sys_lbl[CCORD_NAME_MAX_BYTES + 48];
  if (sys_ch && sys_ch->guild_id == ss->guild_id && sys_ch->name) {
    (void)snprintf(sys_lbl, sizeof sys_lbl, "#%s   (click to change)", sys_ch->name);
  } else {
    (void)snprintf(sys_lbl, sizeof sys_lbl, "No System Messages   (click to change)");
  }
  ss_select(c, (ccord_rect){x, y, fminf(w, 380.0f), UIS_H_INPUT}, sys_lbl, A_SYS_CHANNEL, 0, nullptr, "sys-channel");
  y += UIS_H_INPUT + 16.0f;

  static const char *const k_sysflag_titles[4] = {
    "Send a random welcome message when someone joins this server",
    "Send a message when someone boosts this server",
    "Send helpful tips for server setup",
    "Prompt members to reply to welcome messages with a sticker"
  };
  for (uint32_t fi = 0; fi < 4u; fi++) {
    uint32_t bit = 1u << fi;
    bool enabled = (ss->ov_work_cfg.system_channel_flags & bit) == 0u;   // Discord sets bit to SUPPRESS
    (void)ss_text(c, k_sysflag_titles[fi], x, y + 3.0f, M(font_size_base), false, COL(text_normal), w - 64.0f);
    char fnm[SS_NAME_LEN];
    ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, enabled, nullptr, A_SYS_FLAG, bit,
              NAME(fnm, "sysflag:%u", fi));
    y += 36.0f;
  }
  y += 18.0f;

  // ---- Inactive (AFK) Channel & Timeout ----
  ss_label(c, "INACTIVE CHANNEL (AFK)", x, y);
  y += 24.0f;
  const ccord_channel *afk_ch = ss->ov_work_cfg.afk_channel_id != CCORD_ID_NONE
                                  ? ccord_store_channel(ui->d.store, ss->ov_work_cfg.afk_channel_id)
                                  : nullptr;
  char afk_lbl[CCORD_NAME_MAX_BYTES + 48];
  if (afk_ch && afk_ch->guild_id == ss->guild_id && afk_ch->name) {
    (void)snprintf(afk_lbl, sizeof afk_lbl, "%s   (click to change)", afk_ch->name);
  } else {
    (void)snprintf(afk_lbl, sizeof afk_lbl, "No Inactive Channel   (click to change)");
  }
  ss_select(c, (ccord_rect){x, y, fminf(w, 380.0f), UIS_H_INPUT}, afk_lbl, A_AFK_CHANNEL, 0, nullptr, "afk-channel");
  y += UIS_H_INPUT + 14.0f;
  ss_label(c, "INACTIVE TIMEOUT", x, y);
  y += 22.0f;
  float tx = x;
  for (size_t i = 0; i < AFK_TIMEOUT_COUNT; i++) {
    float cw = ss_text_width(c, ss_k_afk_timeout_labels[i], SS_CAP(c), false) + 24.0f;
    if (tx + cw > x + w && tx > x) {
      tx = x;
      y += 32.0f;
    }
    char nm[SS_NAME_LEN];
    ss_chip(c, (ccord_rect){tx, y, cw, 26.0f}, ss_k_afk_timeout_labels[i],
            ss->ov_work_cfg.afk_timeout == ss_k_afk_timeouts[i], A_AFK_TIMEOUT, ss_k_afk_timeouts[i],
            NAME(nm, "afktimeout:%u", ss_k_afk_timeouts[i]));
    tx += cw + 8.0f;
  }
  y += 26.0f + 28.0f;

  // ---- Default Notification Settings ----
  ss_label(c, "DEFAULT NOTIFICATION SETTINGS", x, y);
  y += 24.0f;
  ss_radio_row(c, (ccord_rect){x, y, w, UIS_H_BTN}, "All Messages", nullptr,
               ss->ov_work_cfg.default_notifications == 0u, nullptr, A_DEF_NOTIFY, 0, "defnotify:0");
  y += UIS_H_BTN + 6.0f;
  ss_radio_row(c, (ccord_rect){x, y, w, UIS_H_BTN}, "Only @mentions", nullptr,
               ss->ov_work_cfg.default_notifications == 1u, nullptr, A_DEF_NOTIFY, 1, "defnotify:1");
  y += UIS_H_BTN + 6.0f;

  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- Roles

const ccord_role *ss_roles(const ccord_ui *ui, size_t *n)
{
  return ccord_store_roles(ui->d.store, ui->ss->guild_id, n);
}

static void draw_up_down(ss_cx *c, ccord_rect row, bool can_up, bool can_down, const char *why_up, const char *why_down)
{
  ccord_ui *ui = c->ui;
  float s = UIS_H_BTN_SM;
  ccord_rect up = {row.x + row.w - 2.0f * s - 10.0f, row.y + (row.h - s) * 0.5f, s, s};
  ccord_rect dn = {row.x + row.w - s - 6.0f, up.y, s, s};
  for (int i = 0; i < 2; i++) {
    ccord_rect b = i == 0 ? up : dn;
    bool ok = i == 0 ? can_up : can_down;
    const char *why = ok ? nullptr : (i == 0 ? why_up : why_down);
    if (!ok && !why) continue;   // nothing to show: at the end of the list
    bool hov = ss_hit_add(c, b, i == 0 ? A_ROLE_UP : A_ROLE_DOWN, 0, why, i == 0 ? "role-up" : "role-down") && ok;
    uis_button(c->ui, c->r, b, "", UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){.hover = hov, .disabled = !ok}, "ssarrow", (uint64_t)i);
    ccord_color ic = ok ? COL(text_header) : ui_alpha(COL(text_muted), UIS_DISABLED_FG);
    float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f, d = 4.0f, e = 3.0f;
    float k = i == 0 ? -1.0f : 1.0f;   // up arrow points up, down arrow points down
    ccord_draw_line(c->r, (ccord_vec2){cx - d, cy - k * e * 0.5f}, (ccord_vec2){cx, cy + k * e * 0.5f}, 1.8f, ic);
    ccord_draw_line(c->r, (ccord_vec2){cx + d, cy - k * e * 0.5f}, (ccord_vec2){cx, cy + k * e * 0.5f}, 1.8f, ic);
  }
}

static void draw_role_list(ss_cx *c, const ss_geom *g, ccord_rect list)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  size_t n = 0;
  const ccord_role *roles = ss_roles(ui, &n);
  uis_surface(ui, c->r, list, COL(bg_secondary));
  (void)g;
  const char *create_why = ss_role_verdict(ui, ss->guild_id, ss->guild_id, false) == CCORD_PERM_MISSING
                             ? ccord_perm_verdict_text(CCORD_PERM_MISSING, CCORD_PERM_ACT_EDIT_ROLES)
                             : nullptr;
  ccord_rect create = {list.x + 10.0f, list.y + 10.0f, list.w - 20.0f, 32.0f};
  ccord_rect body = {list.x, list.y + 52.0f, list.w, list.h - 52.0f};
  ss->list_view = body;
  ss_button(c, create, "Create Role", BTN_PRIMARY, create_why, A_ROLE_CREATE, 0, "role-create");

  ss_cx lc = *c;
  lc.clip = body;
  ccord_push_clip(c->r, body);
  float y = body.y - ss->list_scroll + 4.0f;
  size_t sel_i = SIZE_MAX;
  for (size_t i = 0; i < n; i++) {
    if (roles[i].id == ss->role_sel) sel_i = i;
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_role *ro = &roles[i];
    ccord_rect row = {list.x + 6.0f, y, list.w - 12.0f, 36.0f};
    y += 38.0f;
    if (!ss_visible(&lc, row)) continue;
    bool sel = ro->id == ss->role_sel;
    char nm[SS_NAME_LEN];
    bool hov = ss_hit_add(&lc, row, A_ROLE_SELECT, ro->id, nullptr, NAME(nm, "role:%llu", (unsigned long long)ro->id));
    uis_list_row_fill(c->ui, c->r, row, (uis_state){.hover = hov, .selected = sel}, "ssrole", ro->id);
    ccord_color dot = ro->color ? (ccord_color){(uint8_t)(ro->color >> 16), (uint8_t)(ro->color >> 8), (uint8_t)ro->color, 255}
                                : COL(text_muted);
    ccord_draw_circle(c->r, row.x + 16.0f, row.y + row.h * 0.5f, 6.0f, dot);
    const char *label = ro->id == ss->guild_id ? "@everyone" : (ro->name ? ro->name : "");
    float reserve = sel ? 62.0f : 12.0f;
    (void)ss_text(&lc, label, row.x + 32.0f, row.y + (row.h - ui_line_h(ui, M(font_size_base))) * 0.5f, M(font_size_base),
                  sel, sel ? COL(text_interactive_active) : COL(text_secondary), row.w - 32.0f - reserve);
    if (sel && sel_i != SIZE_MAX) {
      bool up_ok = sel_i > 0 && ro->id != ss->guild_id;
      bool dn_ok = sel_i + 1 < n && roles[sel_i + 1].id != ss->guild_id && ro->id != ss->guild_id;
      const char *why_up = nullptr, *why_dn = nullptr;
      if (up_ok) {
        ccord_perm_verdict v = ss_role_verdict(ui, ss->guild_id, ro->id, false);
        if (v == CCORD_PERM_ALLOWED) v = ss_role_verdict(ui, ss->guild_id, roles[sel_i - 1].id, false);
        if (v != CCORD_PERM_ALLOWED) {
          why_up = ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES);
          up_ok = false;
        }
      }
      if (dn_ok) {
        ccord_perm_verdict v = ss_role_verdict(ui, ss->guild_id, ro->id, false);
        if (v == CCORD_PERM_ALLOWED) v = ss_role_verdict(ui, ss->guild_id, roles[sel_i + 1].id, false);
        if (v != CCORD_PERM_ALLOWED) {
          why_dn = ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES);
          dn_ok = false;
        }
      }
      bool show_up = ro->id != ss->guild_id && sel_i > 0, show_dn = ro->id != ss->guild_id && sel_i + 1 < n && roles[sel_i + 1].id != ss->guild_id;
      draw_up_down(&lc, row, up_ok, dn_ok, show_up ? why_up : nullptr, show_dn ? why_dn : nullptr);
    }
  }
  ccord_pop_clip(c->r);
  ss->list_content_h = y - body.y + ss->list_scroll + 8.0f;
}

uint32_t ss_color_hex_parse(const char *s, bool *ok)
{
  *ok = false;
  if (*s == '#') s++;
  if (strlen(s) != 6) return 0;
  uint32_t v = 0;
  for (int i = 0; i < 6; i++) {
    char ch = s[i];
    uint32_t d;
    if (ch >= '0' && ch <= '9') d = (uint32_t)(ch - '0');
    else if (ch >= 'a' && ch <= 'f') d = (uint32_t)(ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') d = (uint32_t)(ch - 'A' + 10);
    else return 0;
    v = v * 16u + d;
  }
  *ok = true;
  return v;
}

ccord_color ss_rgb(uint32_t c)
{
  return (ccord_color){(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, 255};
}

static void draw_role_display(ss_cx *c, float x, float w, float *py, const char *lock_why)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float y = *py;
  bool everyone = ss->role_sel == ss->guild_id;
  ss_label(c, "ROLE NAME", x, y);
  y += 22.0f;
  if (everyone) {
    ccord_rect every = {x, y, w, UIS_H_INPUT};
    if (ss_visible(c, every)) {
      ccord_text_edit blank = {0};
      (void)uis_input(ui, c->r, every, &blank, "@everyone", (uis_state){.disabled = true}, false, "sseveryone", 0);
    }
  } else {
    ss_field_box(c, (ccord_rect){x, y, w, UIS_H_INPUT}, &ss->role_name_edit, SSF_ROLE_NAME, "Role name", lock_why, "role-name");
  }
  y += UIS_H_INPUT + 28.0f;
  ss_label(c, "ROLE COLOR", x, y);
  y += 22.0f;
  if (everyone) {
    ss_message(c, "@everyone has no colour or display options. Its permissions are the baseline for every member.", x, y, w);
    y += 44.0f;
  } else {
    float sw = UIS_SWATCH_D, gap = 8.0f;
    size_t per_row = (size_t)fmaxf(1.0f, floorf((w + gap) / (sw + gap)));
    if (per_row > 11) per_row = 11;
    ccord_rect none = {x, y, sw, sw};
    bool nh = ss_hit_add(c, none, A_COLOR, 0, lock_why, "color:none") && lock_why == nullptr;
    uis_swatch(c->ui, c->r, none, COL(bg_tertiary), (uis_state){.hover = nh, .selected = ss->work.color == 0,
               .disabled = lock_why != nullptr}, "ssswatch", 0u);
    ui_icon_close(c->r, none.x + sw * 0.5f, none.y + sw * 0.5f, 14.0f, ss->work.color == 0 ? COL(text_header) : COL(text_muted));
    size_t slot = 1;
    for (size_t i = 0; i < COLOR_COUNT; i++, slot++) {
      size_t cx = slot % per_row, cy = slot / per_row;
      ccord_rect sr = {x + (float)cx * (sw + gap), y + (float)cy * (sw + gap), sw, sw};
      char nm[SS_NAME_LEN];
      bool hov = ss_hit_add(c, sr, A_COLOR, ss_k_colors[i], lock_why, NAME(nm, "color:%06X", ss_k_colors[i])) && lock_why == nullptr;
      bool sel = ss->work.color == ss_k_colors[i];
      uis_swatch(c->ui, c->r, sr, ss_rgb(ss_k_colors[i]), (uis_state){.hover = hov, .selected = sel,
                 .disabled = lock_why != nullptr}, "ssswatch", (uint64_t)(i + 1));
    }
    size_t rows = (COLOR_COUNT + 1 + per_row - 1) / per_row;
    y += (float)rows * (sw + gap) + 6.0f;
    ss_label(c, "CUSTOM COLOR", x, y);
    y += 22.0f;
    ss_field_box(c, (ccord_rect){x, y, fminf(w, 180.0f), 38.0f}, &ss->color_edit, SSF_ROLE_COLOR, "#RRGGBB", lock_why, "role-color");
    y += 38.0f + 28.0f;
    ss_label(c, "DISPLAY", x, y);
    y += 26.0f;
    for (int i = 0; i < 2; i++) {
      const char *title = i == 0 ? "Display role members separately from online members" : "Allow anyone to @mention this role";
      (void)ss_text(c, title, x, y + 3.0f, M(font_size_base), false, COL(text_normal), w - 64.0f);
      ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, i == 0 ? ss->work.hoist : ss->work.mention, lock_why,
                i == 0 ? A_HOIST : A_MENTION, 0, i == 0 ? "hoist" : "mention");
      y += 40.0f;
    }
    y += 16.0f;
    ccord_rect del = {x, y, 140.0f, 34.0f};
    ss_button(c, del, "Delete Role", BTN_DANGER, lock_why, A_ROLE_DELETE, 0, "role-delete");
    y += 34.0f;
  }
  *py = y;
}

static void draw_role_perms(ss_cx *c, float x, float w, float *py, const char *lock_why)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float y = *py;
  size_t n = 0;
  const ccord_perm_info *tbl = ccord_perm_table(&n);
  ccord_perm_group last = CCORD_PERM_GROUP_COUNT;
  for (size_t i = 0; i < n; i++) {
    if (tbl[i].group != last) {
      last = tbl[i].group;
      y += 8.0f;
      char head[64];
      const char *gl = ccord_perm_group_label(last);
      size_t k = 0;
      for (; gl[k] && k + 1 < sizeof head; k++) head[k] = (char)((gl[k] >= 'a' && gl[k] <= 'z') ? gl[k] - 32 : gl[k]);
      head[k] = '\0';
      ss_label(c, head, x, y);
      y += 26.0f;
    }
    ccord_rect row = {x, y, w, SS_PERM_ROW_H};
    y += SS_PERM_ROW_H;
    if (!ss_visible(c, row)) continue;
    bool on = (ss->work.perms & tbl[i].bit) != 0;
    const char *why = lock_why;
    if (!why) {
      ccord_perm_verdict v = ss_grant_verdict(ui, ss->guild_id, ss->base.perms, ss->work.perms ^ tbl[i].bit);
      if (v != CCORD_PERM_ALLOWED) why = ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES);
    }
    (void)ss_text(c, tbl[i].label, row.x, row.y + 6.0f, M(font_size_base), true, COL(text_header), w - 70.0f);
    (void)ss_text(c, tbl[i].description, row.x, row.y + 6.0f + ui_line_h(ui, M(font_size_base)), M(font_size_small), false,
                  COL(text_muted), w - 70.0f);
    char nm[SS_NAME_LEN];
    ss_toggle(c, (ccord_rect){row.x + w - 44.0f, row.y + 8.0f, 44.0f, 24.0f}, on, why, A_PERM, tbl[i].bit,
              NAME(nm, "perm:%llx", (unsigned long long)tbl[i].bit));
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  *py = y;
}

bool ss_member_has_role(const ccord_ui *ui, ccord_id uid, ccord_id role, bool *known)
{
  const ccord_id *ids = nullptr;
  size_t cnt = 0;
  *known = ccord_store_member_roles(ui->d.store, ui->ss->guild_id, uid, &ids, &cnt);
  if (!*known) return false;
  for (size_t i = 0; i < cnt; i++) {
    if (ids[i] == role) return true;
  }
  return false;
}

static void draw_role_members(ss_cx *c, float x, float w, float *py, const char *lock_why)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float y = *py;
  if (ss->role_sel == ss->guild_id) {
    ss_message(c, "Every member has @everyone, so there is nobody to add or remove.", x, y, w);
    *py = y + 40.0f;
    return;
  }
  const char *why = lock_why;
  if (!why) {
    ccord_perm_verdict v = ss_role_verdict(ui, ss->guild_id, ss->role_sel, true);
    if (v != CCORD_PERM_ALLOWED) why = ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES);
  }
  size_t count = ccord_store_member_count(ui->d.store, ss->guild_id);
  if (count == 0) {
    ss_message(c, "No members are loaded in this client yet.", x, y, w);
    *py = y + 40.0f;
    return;
  }
  ss_message(c, "Members are changed one at a time; Discord confirms each change.", x, y, w);
  y += 34.0f;
  float row_h = 44.0f;
  size_t first = 0;
  if (c->clip.y > y) first = (size_t)fmaxf(0.0f, floorf((c->clip.y - y) / row_h));
  for (size_t i = first; i < count; i++) {
    ccord_rect row = {x, y + (float)i * row_h, w, row_h};
    if (row.y > c->clip.y + c->clip.h) break;
    const ccord_member *m = ccord_store_member_at(ui->d.store, ss->guild_id, i);
    if (!m) continue;
    const ccord_user *u = ccord_store_user(ui->d.store, m->user_id);
    bool known = false;
    bool has = ss_member_has_role(ui, m->user_id, ss->role_sel, &known);
    ui_draw_avatar(ui, c->r, u, row.x + 18.0f, row.y + row_h * 0.5f, 28.0f, (ccord_color){0, 0, 0, 0}, false);
    (void)ss_text(c, ui_member_display_name(ui, ss->guild_id, m->user_id), row.x + 42.0f,
                  row.y + (row_h - ui_line_h(ui, M(font_size_base))) * 0.5f, M(font_size_base), false, COL(text_normal),
                  w - 120.0f);
    const char *row_why = why ? why : (known ? nullptr : "This member's roles haven't loaded yet.");
    char nm[SS_NAME_LEN];
    ss_toggle(c, (ccord_rect){row.x + w - 44.0f, row.y + (row_h - 24.0f) * 0.5f, 44.0f, 24.0f}, has, row_why, A_MEMBER,
              m->user_id, NAME(nm, "member:%llu", (unsigned long long)m->user_id));
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  *py = y + (float)count * row_h;
}

void ss_draw_roles(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float top = ui->mobile_layout ? g->top_y : 40.0f;
  float bot_in = ui->mobile_layout ? fmaxf(ui->safe_insets.bottom, ui->keyboard_inset) : 0.0f;
  ccord_rect all = {g->col_x, top, g->col_w, fmaxf(ui->win_h - bot_in - top - 20.0f, 120.0f)};
  bool narrow = all.w < 460.0f;
  float ex, ew;
  if (narrow) {
    if (!ss->mobile_role_editing) {
      draw_role_list(c, g, all);
      ss->editor_view = (ccord_rect){0};
      ss->view = all;
      return;
    }
    ss->list_view = (ccord_rect){0};
    ex = all.x;
    ew = all.w;
  } else {
    float list_w = fminf(240.0f, all.w * 0.38f);
    ccord_rect list = {all.x, all.y, list_w, all.h};
    draw_role_list(c, g, list);
    ex = list.x + list_w + 24.0f;
    ew = all.w - list_w - 24.0f;
  }
  if (ew < 100.0f) return;
  ccord_rect view = {ex - 4.0f, top, ew + 8.0f, fmaxf(ui->win_h - bot_in - top, 120.0f)};
  ss->editor_view = view;
  ss->view = view;
  const ccord_role *role = ccord_store_role(ui->d.store, ss->guild_id, ss->role_sel);
  if (!role) return;

  // The editor is locked when Discord would refuse the role to this user (exact data only).
  ccord_perm_verdict v = ss_role_verdict(ui, ss->guild_id, role->id, false);
  const char *lock_why = v == CCORD_PERM_ALLOWED ? nullptr : ccord_perm_verdict_text(v, CCORD_PERM_ACT_EDIT_ROLES);

  float y = top + (narrow ? 4.0f : 16.0f) - ss->scroll;
  char head[SS_TITLE_MAX + 24];
  (void)snprintf(head, sizeof head, "Edit Role - %s", role->id == ss->guild_id ? "@everyone" : (ss->base.name));
  float preview_w = 128.0f;
  ss_cx ec = *c;
  ec.clip = (ccord_rect){view.x, top, view.w, view.h};
  ccord_push_clip(c->r, ec.clip);
  if (ew < 340.0f) {
    (void)ss_text(&ec, head, ex, y, SS_TITLE(c), true, COL(text_header), ew);
    y += 30.0f;
    ss_button(&ec, (ccord_rect){ex, y, preview_w, 30.0f}, "View as Role", BTN_SECONDARY, nullptr,
              A_ROLE_PREVIEW, 0, "role-preview");
    y += 38.0f;
  } else {
    (void)ss_text(&ec, head, ex, y, SS_TITLE(c), true, COL(text_header), fmaxf(40.0f, ew - preview_w - 12.0f));
    ss_button(&ec, (ccord_rect){ex + ew - preview_w, y - 3.0f, preview_w, 30.0f}, "View as Role", BTN_SECONDARY, nullptr,
              A_ROLE_PREVIEW, 0, "role-preview");
    y += 38.0f;
  }
  if (lock_why) {
    ccord_rect note = {ex, y, ew, 34.0f};
    uis_surface(ui, c->r, note, ui_alpha(COL(danger), 0.18f));
    (void)ss_text(&ec, lock_why, note.x + 12.0f, note.y + (note.h - ui_line_h(ui, M(font_size_small))) * 0.5f,
                  M(font_size_small), false, COL(text_normal), note.w - 24.0f);
    y += 44.0f;
  }
  float tx = ex;
  for (int t = 0; t < TAB_COUNT; t++) {
    float tw = uis_label_width(ui, c->r, ss_k_tab_labels[t]);
    if (tx > ex && tx + tw > ex + ew) {
      tx = ex;
      y += UIS_H_BTN_SM + 4.0f;
    }
    ccord_rect tr = {tx, y, tw, UIS_H_BTN_SM};
    char nm[SS_NAME_LEN];
    bool hov = ss_hit_add(&ec, tr, A_TAB, (uint64_t)t, nullptr, NAME(nm, "tab:%s", ss_k_tab_names[t]));
    bool sel = ss->tab == (ss_tab)t;
    uis_tab(ui, c->r, tr, ss_k_tab_labels[t], (uis_state){.hover = hov, .selected = sel}, "sstab", (uint64_t)t);
    tx += tr.w + UIS_SP_1;
  }
  y += 32.0f;
  uis_divider(ui, c->r, ex, y, ew);
  y += 22.0f;
  if (ss->tab == TAB_DISPLAY) draw_role_display(&ec, ex, ew, &y, lock_why);
  else if (ss->tab == TAB_PERMS) draw_role_perms(&ec, ex, ew, &y, lock_why);
  else draw_role_members(&ec, ex, ew, &y, lock_why);
  ccord_pop_clip(c->r);
  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- Safety Setup

void ss_format_remaining(int64_t ms, char *out, size_t cap)
{
  if (ms <= 0) {
    (void)snprintf(out, cap, "expired");
    return;
  }
  int64_t s = ms / 1000;
  if (s < 3600) (void)snprintf(out, cap, "%lld min", (long long)((s + 59) / 60));
  else if (s < 86400) (void)snprintf(out, cap, "%lld hr", (long long)((s + 1799) / 3600));
  else (void)snprintf(out, cap, "%lld days", (long long)((s + 43199) / 86400));
}

static void ss_format_remaining_short(int64_t until_ms, int64_t now_ms, char *out, size_t cap)
{
  if (until_ms <= 0 || until_ms <= now_ms) {
    (void)snprintf(out, cap, "Active (not paused)");
    return;
  }
  char rem[32];
  ss_format_remaining(until_ms - now_ms, rem, sizeof rem);
  (void)snprintf(out, cap, "Paused (%s left)", rem);
}

void ss_draw_safety(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float x = g->col_x, w = g->col_w, y = g->top_y - ss->scroll;
  ss_title(c, g, "Safety Setup", y);
  y += 56.0f;

  // ---- Raid Protection: Pause Invites & DMs (immediate governed PUT /guilds/{g}/incident-actions) ----
  ss_label(c, "RAID PROTECTION", x, y);
  y += 24.0f;
  static const uint32_t k_pause_s[4] = {0, 3600, 21600, 86400};
  static const char *const k_pause_lbl[4] = {"Active", "Pause 1h", "Pause 6h", "Pause 24h"};
  for (int kind = 0; kind < 2; kind++) {
    int64_t until = kind == 0 ? gd->invites_disabled_until_ms : gd->dms_disabled_until_ms;
    bool paused = until > ui->now_unix_ms;
    char st_txt[96];
    ss_format_remaining_short(until, ui->now_unix_ms, st_txt, sizeof st_txt);
    char row_lbl[160];
    (void)snprintf(row_lbl, sizeof row_lbl, "%s: %s",
                   kind == 0 ? "Server Invites" : "Direct Messages between members", st_txt);
    (void)ss_text(c, row_lbl, x, y + 4.0f, M(font_size_base), true, paused ? COL(danger) : COL(text_header), w);
    y += 26.0f;
    float px = x;
    for (size_t pi = 0; pi < 4u; pi++) {
      float cw = ss_text_width(c, k_pause_lbl[pi], SS_CAP(c), false) + 24.0f;
      bool sel = (pi == 0) ? !paused : false;
      if (paused && pi > 0) {
        int64_t rem_s = (until - ui->now_unix_ms) / 1000;
        if (pi == 1 && rem_s <= 3600) sel = true;
        else if (pi == 2 && rem_s > 3600 && rem_s <= 21600) sel = true;
        else if (pi == 3 && rem_s > 21600) sel = true;
      }
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){px, y, cw, 26.0f}, k_pause_lbl[pi], sel,
              kind == 0 ? A_PAUSE_INVITES : A_PAUSE_DMS, k_pause_s[pi],
              NAME(nm, "pause-%s:%u", kind == 0 ? "invites" : "dms", k_pause_s[pi]));
      px += cw + 8.0f;
    }
    y += 26.0f + 16.0f;
  }
  y += 12.0f;

  // ---- Verification Level ----
  ss_label(c, "VERIFICATION LEVEL", x, y);
  y += 24.0f;
  static const struct { const char *title; const char *desc; } k_verif[5] = {
    {"None", "Unrestricted"},
    {"Low", "Must have a verified email on their Discord account"},
    {"Medium", "Must also be registered on Discord for longer than 5 minutes"},
    {"High", "Must also be a member of this server for longer than 10 minutes"},
    {"Highest", "Must have a verified phone on their Discord account"}
  };
  for (uint8_t vi = 0; vi < 5u; vi++) {
    char nm[SS_NAME_LEN];
    ss_radio_row(c, (ccord_rect){x, y, w, 56.0f}, k_verif[vi].title, k_verif[vi].desc,
                 ss->safety_work.verification_level == vi, nullptr, A_VERIF_LEVEL, vi, NAME(nm, "verif:%u", vi));
    y += 60.0f;
  }
  y += 20.0f;

  // ---- Explicit Media Content Filter ----
  ss_label(c, "EXPLICIT MEDIA CONTENT FILTER", x, y);
  y += 24.0f;
  static const struct { const char *title; const char *desc; } k_ecf[3] = {
    {"Do not scan any media content", "Media content will not be scanned for explicit content"},
    {"Scan media content from members without a role", "Recommended option for servers that use roles for trusted membership"},
    {"Scan media content from all members", "Recommended option when you want squeaky-clean chat"}
  };
  for (uint8_t ei = 0; ei < 3u; ei++) {
    char nm[SS_NAME_LEN];
    ss_radio_row(c, (ccord_rect){x, y, w, 56.0f}, k_ecf[ei].title, k_ecf[ei].desc,
                 ss->safety_work.explicit_filter == ei, nullptr, A_EXPLICIT_FILTER, ei, NAME(nm, "ecf:%u", ei));
    y += 60.0f;
  }

  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- list pages (Invites, Bans, Audit Log)

// "Loading ...", the failure with a Refresh button, or false when the list is there to be drawn.
bool ss_draw_list_state(ss_cx *c, ss_page p, bool loaded, float x, float *py, float w)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  if (loaded) return false;
  float y = *py;
  bool timed_out = ss->requested[p] && ui->now_ms >= ss->requested_ms[p] + SS_LIST_TIMEOUT_MS;
  if (ss->failed[p] || timed_out || !ss->requested[p]) {
    ss_message(c, "Couldn't load this list. Discord may have refused or the connection dropped.", x, y, w);
    ss_button(c, (ccord_rect){x, y + 40.0f, 110.0f, 32.0f}, "Try Again", BTN_PRIMARY, nullptr, A_REFRESH, 0, "retry");
    *py = y + 80.0f;
  } else {
    ss_message(c, "Loading...", x, y, w);
    *py = y + 36.0f;
  }
  return true;
}

// The text channel an invite will point at: the one the user is looking at if it belongs here, else the first one.
ccord_id ss_invite_channel(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_store *st = ui->d.store;
  if (ss->inv_channel != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(st, ss->inv_channel);
    if (ch && ch->guild_id == ss->guild_id) return ss->inv_channel;
  }
  ccord_id sel = ccord_store_selected_channel(st);
  const ccord_channel *sc = sel != CCORD_ID_NONE ? ccord_store_channel(st, sel) : nullptr;
  if (sc && sc->guild_id == ss->guild_id && sc->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(sc->discord_type)) {
    return sel;
  }
  size_t n = ccord_store_channel_count(st, ss->guild_id);
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(st, ss->guild_id, i);
    if (ch && ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type)) return ch->id;
  }
  return CCORD_ID_NONE;
}

static void draw_invites_extras(ss_cx *c, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float y = *py + 24.0f;

  // Pause Invites toggle
  ss_label(c, "PAUSE INVITES", x, y);
  y += 24.0f;
  bool paused = gd->invites_disabled_until_ms > ui->now_unix_ms;
  char pdesc[128];
  if (paused) {
    char rem[32];
    ss_format_remaining(gd->invites_disabled_until_ms - ui->now_unix_ms, rem, sizeof rem);
    (void)snprintf(pdesc, sizeof pdesc, "Invites are paused (%s remaining). Existing links will not admit new members.", rem);
  } else {
    (void)snprintf(pdesc, sizeof pdesc, "Temporarily disable all invite links to this server for 24 hours.");
  }
  (void)ss_text(c, pdesc, x, y + 3.0f, M(font_size_base), false, paused ? COL(danger) : COL(text_normal), w - 64.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, paused, nullptr, A_INV_PAUSE_TOGGLE, 0,
            "invites-pause-toggle");
  y += 38.0f + 16.0f;

  // Vanity URL
  ss_label(c, "VANITY INVITE LINK", x, y);
  y += 24.0f;
  bool can_vanity = gd->feature_vanity_url || gd->premium_tier >= 3u;
  const char *v_why = can_vanity ? nullptr : "Vanity URL requires Server Boost Level 3.";
  const char *clr_why = v_why ? v_why : (gd->vanity_url_code[0] == '\0' ? "No vanity URL is set." : nullptr);
  if (w < 440.0f) {
    ss_field_box(c, (ccord_rect){x, y, w, 36.0f}, &ss->vanity_edit, SSF_VANITY, "discord.gg/custom-slug", v_why,
                 "vanity-code");
    y += 42.0f;
    ss_button(c, (ccord_rect){x, y, 92.0f, 34.0f}, "Save URL", BTN_PRIMARY, v_why, A_VANITY_SAVE, 0, "vanity-save");
    ss_button(c, (ccord_rect){x + 100.0f, y, 84.0f, 34.0f}, "Clear", BTN_SECONDARY, clr_why, A_VANITY_CLEAR, 0,
              "vanity-clear");
    y += 42.0f;
  } else {
    float box_w = fminf(fmaxf(140.0f, w - 210.0f), 320.0f);
    ss_field_box(c, (ccord_rect){x, y, box_w, 36.0f}, &ss->vanity_edit, SSF_VANITY, "discord.gg/custom-slug", v_why,
                 "vanity-code");
    ss_button(c, (ccord_rect){x + box_w + 10.0f, y + 1.0f, 92.0f, 34.0f}, "Save URL", BTN_PRIMARY, v_why,
              A_VANITY_SAVE, 0, "vanity-save");
    ss_button(c, (ccord_rect){x + box_w + 110.0f, y + 1.0f, 84.0f, 34.0f}, "Clear", BTN_SECONDARY, clr_why,
              A_VANITY_CLEAR, 0, "vanity-clear");
    y += 42.0f;
  }
  *py = y;
}

void ss_draw_invites(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float x = g->col_x, w = g->col_w, y = g->top_y - ss->scroll;
  ss_title(c, g, "Invites", y);
  y += 56.0f;

  // Create one: channel, expiry, uses.
  ccord_id chan = ss_invite_channel(ui);
  const ccord_channel *ch = chan != CCORD_ID_NONE ? ccord_store_channel(ui->d.store, chan) : nullptr;
  ss_label(c, "CREATE AN INVITE LINK", x, y);
  y += 24.0f;
  char chlabel[CCORD_NAME_MAX_BYTES + 16];
  (void)snprintf(chlabel, sizeof chlabel, "Channel: #%s   (click to change)", ch && ch->name ? ch->name : "none");
  ss_select(c, (ccord_rect){x, y, fminf(w, 360.0f), UIS_H_INPUT}, chlabel, A_INV_CHANNEL, 0, nullptr, "invite-channel");
  y += UIS_H_INPUT + 14.0f;
  ss_label(c, "EXPIRE AFTER", x, y);
  y += 22.0f;
  float cx = x;
  for (size_t i = 0; i < INV_AGE_COUNT; i++) {
    float cw = ss_text_width(c, ss_k_inv_age_labels[i], SS_CAP(c), false) + 24.0f;
    if (cx + cw > x + w) {
      cx = x;
      y += 32.0f;
    }
    char nm[SS_NAME_LEN];
    ss_chip(c, (ccord_rect){cx, y, cw, 26.0f}, ss_k_inv_age_labels[i], ss->inv_age_i == i, A_INV_AGE, i,
            NAME(nm, "invite-age:%u", ss_k_inv_ages[i]));
    cx += cw + 8.0f;
  }
  y += 26.0f + 14.0f;
  ss_label(c, "MAX NUMBER OF USES", x, y);
  y += 22.0f;
  cx = x;
  for (size_t i = 0; i < INV_USES_COUNT; i++) {
    float cw = ss_text_width(c, ss_k_inv_use_labels[i], SS_CAP(c), false) + 24.0f;
    if (cx + cw > x + w) {
      cx = x;
      y += 32.0f;
    }
    char nm[SS_NAME_LEN];
    ss_chip(c, (ccord_rect){cx, y, cw, 26.0f}, ss_k_inv_use_labels[i], ss->inv_uses_i == i, A_INV_USES, i,
            NAME(nm, "invite-uses:%u", ss_k_inv_uses[i]));
    cx += cw + 8.0f;
  }
  y += 26.0f + 16.0f;
  ss_button(c, (ccord_rect){x, y, 150.0f, 34.0f}, "Generate Invite", BTN_PRIMARY,
            chan == CCORD_ID_NONE ? "This server has no text channel to invite to." : nullptr, A_INV_CREATE, 0,
            "invite-create");
  ss_button(c, (ccord_rect){x + 162.0f, y, 100.0f, 34.0f}, "Refresh", BTN_SECONDARY, nullptr, A_REFRESH, 0, "refresh");
  y += 34.0f + 30.0f;

  ss_label(c, "ACTIVE INVITE LINKS", x, y);
  y += 26.0f;
  size_t n = 0;
  const ccord_invite *inv = ccord_store_invites(ui->d.store, ss->guild_id, &n);
  if (ss_draw_list_state(c, SS_INVITES, ccord_store_invites_loaded(ui->d.store, ss->guild_id), x, &y, w)) {
    draw_invites_extras(c, x, w, &y);
    ss_end_content(ss, y);
    return;
  }
  ss->rows = n;
  if (n == 0) {
    ss_message(c, "No invites yet. Generate one above.", x, y, w);
    y += 40.0f;
  }
  // Columns: code / inviter / uses / expires, then the actions (2-row compact cards on narrow widths).
  bool narrow_inv = w < 500.0f;
  float act_w = 170.0f;
  float cw_code = fmaxf(90.0f, (w - act_w) * 0.36f), cw_inv = fmaxf(70.0f, (w - act_w) * 0.28f);
  float cw_uses = 70.0f;
  if (n > 0 && !narrow_inv) {
    float hx = x;
    ss_label(c, "INVITE CODE", hx, y);
    ss_label(c, "INVITER", hx + cw_code, y);
    ss_label(c, "USES", hx + cw_code + cw_inv, y);
    ss_label(c, "EXPIRES", hx + cw_code + cw_inv + cw_uses, y);
    y += 24.0f;
  }
  for (size_t i = 0; i < n; i++) {
    float rh = narrow_inv ? 74.0f : SS_ROW_H;
    ccord_rect row = {x, y, w, rh};
    y += rh;
    if (!ss_visible(c, row)) continue;
    char code_mask[128];
    const char *shown_code = ui_streamer_mask_code(ui, inv[i].code, code_mask, sizeof code_mask, true);
    char uses[32];
    if (inv[i].max_uses) (void)snprintf(uses, sizeof uses, "%u / %u", inv[i].uses, inv[i].max_uses);
    else (void)snprintf(uses, sizeof uses, "%u", inv[i].uses);
    char exp[32];
    if (inv[i].expires_ms == 0) (void)snprintf(exp, sizeof exp, "Never");
    else ss_format_remaining(inv[i].expires_ms - ui->now_unix_ms, exp, sizeof exp);
    char nm[SS_NAME_LEN], nm2[SS_NAME_LEN];
    if (narrow_inv) {
      char meta[192];
      (void)snprintf(meta, sizeof meta, "%s  ·  %s uses  ·  %s",
                     inv[i].inviter_name && inv[i].inviter_name[0] ? inv[i].inviter_name : "-", uses, exp);
      (void)ss_text(c, shown_code, row.x, row.y + 6.0f, M(font_size_base), true, COL(text_header), w);
      (void)ss_text(c, meta, row.x, row.y + 24.0f, SS_CAP(c), false, COL(text_muted), w);
      ss_button(c, (ccord_rect){row.x, row.y + 42.0f, 72.0f, 26.0f}, "Copy", BTN_SECONDARY, nullptr, A_INV_COPY, i,
                NAME(nm, "invite-copy:%s", inv[i].code));
      ss_button(c, (ccord_rect){row.x + 80.0f, row.y + 42.0f, 76.0f, 26.0f}, "Revoke", BTN_DANGER, nullptr, A_INV_REVOKE, i,
                NAME(nm2, "invite-revoke:%s", inv[i].code));
    } else {
      float ty = row.y + (row.h - ui_line_h(ui, M(font_size_base))) * 0.5f;
      (void)ss_text(c, shown_code, row.x, ty, M(font_size_base), true, COL(text_header), cw_code - 10.0f);
      (void)ss_text(c, inv[i].inviter_name && inv[i].inviter_name[0] ? inv[i].inviter_name : "-", row.x + cw_code, ty,
                    M(font_size_base), false, COL(text_normal), cw_inv - 10.0f);
      (void)ss_text(c, uses, row.x + cw_code + cw_inv, ty, M(font_size_base), false, COL(text_normal), cw_uses - 6.0f);
      (void)ss_text(c, exp, row.x + cw_code + cw_inv + cw_uses, ty, M(font_size_base), false, COL(text_normal), 90.0f);
      ss_button(c, (ccord_rect){row.x + w - 160.0f, row.y + 8.0f, 74.0f, 30.0f}, "Copy", BTN_SECONDARY, nullptr, A_INV_COPY, i,
                NAME(nm, "invite-copy:%s", inv[i].code));
      ss_button(c, (ccord_rect){row.x + w - 80.0f, row.y + 8.0f, 80.0f, 30.0f}, "Revoke", BTN_DANGER, nullptr, A_INV_REVOKE, i,
                NAME(nm2, "invite-revoke:%s", inv[i].code));
    }
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  draw_invites_extras(c, x, w, &y);
  ss_end_content(ss, y);
}

void ss_draw_bans(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float x = g->col_x, w = g->col_w, y = g->top_y - ss->scroll;
  ss_title(c, g, "Bans", y);
  y += 56.0f;
  size_t n = 0;
  const ccord_ban *bans = ccord_store_bans(ui->d.store, ss->guild_id, &n);
  bool loaded = ccord_store_bans_loaded(ui->d.store, ss->guild_id);
  ss_field_box(c, (ccord_rect){x, y, fminf(w - 120.0f, 420.0f), 38.0f}, &ss->ban_search, SSF_BAN_SEARCH, "Search by username or ID",
               nullptr, "ban-search");
  ss_button(c, (ccord_rect){x + fminf(w - 120.0f, 420.0f) + 12.0f, y + 3.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr,
            A_REFRESH, 0, "refresh");
  y += 38.0f + 26.0f;
  if (ss_draw_list_state(c, SS_BANS, loaded, x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }
  const char *needle = ccord_text_edit_cstr(&ss->ban_search);
  size_t shown = 0;
  char idbuf[24];
  for (size_t i = 0; i < n; i++) {
    (void)ccord_id_format(bans[i].user_id, idbuf);
    const char *nm = bans[i].name && bans[i].name[0] ? bans[i].name : "Unknown user";
    if (!ss_contains_ci(nm, needle) && !ss_contains_ci(idbuf, needle)) continue;
    shown++;
    ccord_rect row = {x, y, w, SS_ROW_H + 8.0f};
    y += row.h;
    if (!ss_visible(c, row)) continue;
    ccord_draw_circle(c->r, row.x + 18.0f, row.y + row.h * 0.5f, 16.0f, ui_user_color(ui, (uint8_t)(bans[i].user_id % 6u)));
    char ini[8];
    ui_initials(nm, ini);
    ss_text_centered(c, ini, (ccord_rect){row.x + 2.0f, row.y + row.h * 0.5f - 16.0f, 32.0f, 32.0f}, M(font_size_small), true,
                     COL(text_on_brand));
    (void)ss_text(c, nm, row.x + 46.0f, row.y + 6.0f, M(font_size_base), true, COL(text_header), w - 190.0f);
    char sub[CCORD_NAME_MAX_BYTES + 160];
    if (bans[i].reason && bans[i].reason[0]) (void)snprintf(sub, sizeof sub, "Reason: %s", bans[i].reason);
    else (void)snprintf(sub, sizeof sub, "No reason given - %s", idbuf);
    (void)ss_text(c, sub, row.x + 46.0f, row.y + 8.0f + ui_line_h(ui, M(font_size_base)), SS_CAP(c), false,
                  COL(text_muted), w - 190.0f);
    char nm2[SS_NAME_LEN];
    ss_button(c, (ccord_rect){row.x + w - 120.0f, row.y + (row.h - 30.0f) * 0.5f, 120.0f, 30.0f}, "Revoke Ban", BTN_SECONDARY,
              nullptr, A_UNBAN, bans[i].user_id, NAME(nm2, "unban:%llu", (unsigned long long)bans[i].user_id));
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  ss->rows = shown;
  if (shown == 0) {
    ss_message(c, n == 0 ? "Nobody is banned from this server." : "No bans match your search.", x, y, w);
    y += 40.0f;
  }
  ss_end_content(ss, y);
}

static void ss_remember_audit_actor(struct ui_server_settings *ss, ccord_id uid, const char *name)
{
  if (uid == CCORD_ID_NONE) return;
  for (size_t i = 0; i < ss->audit_actor_count; i++) {
    if (ss->audit_actor_ids[i] == uid) {
      if (name && name[0] && ss->audit_actor_names[i][0] == '\0') {
        (void)snprintf(ss->audit_actor_names[i], sizeof ss->audit_actor_names[i], "%s", name);
      }
      return;
    }
  }
  if (ss->audit_actor_count >= SS_AUDIT_ACTORS_MAX) return;
  size_t idx = ss->audit_actor_count++;
  ss->audit_actor_ids[idx] = uid;
  (void)snprintf(ss->audit_actor_names[idx], sizeof ss->audit_actor_names[idx], "%s", name ? name : "");
}

static bool ss_audit_actors_all_named(const struct ui_server_settings *ss)
{
  if (ss->audit_actor_count < SS_AUDIT_ACTORS_MAX) return false;
  for (size_t i = 0; i < ss->audit_actor_count; i++) {
    if (ss->audit_actor_names[i][0] == '\0') return false;
  }
  return true;
}

void ss_populate_audit_actors(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  uint64_t rev = ccord_store_revision(ui->d.store);
  if (ss->audit_actors_valid && ss->audit_actors_rev == rev && ss->audit_actors_guild == ss->guild_id) {
    return;
  }
  size_t nm = ccord_store_member_count(ui->d.store, ss->guild_id);
  for (size_t i = 0; i < nm; i++) {
    if (ss_audit_actors_all_named(ss)) break;
    const ccord_member *m = ccord_store_member_at(ui->d.store, ss->guild_id, i);
    if (!m) continue;
    ss_remember_audit_actor(ss, m->user_id, ui_member_display_name(ui, ss->guild_id, m->user_id));
  }
  size_t na = 0;
  const ccord_audit_entry *log = ccord_store_audit(ui->d.store, ss->guild_id, &na);
  for (size_t i = 0; log && i < na; i++) {
    if (ss_audit_actors_all_named(ss)) break;
    ss_remember_audit_actor(ss, log[i].user_id, log[i].user_name);
  }
  ss->audit_actors_rev = rev;
  ss->audit_actors_guild = ss->guild_id;
  ss->audit_actors_valid = true;
}

static const char *ss_audit_actor_label(const ccord_ui *ui, ccord_id uid)
{
  if (uid == CCORD_ID_NONE) return "All Users";
  const struct ui_server_settings *ss = ui->ss;
  for (size_t i = 0; i < ss->audit_actor_count; i++) {
    if (ss->audit_actor_ids[i] == uid && ss->audit_actor_names[i][0] != '\0') return ss->audit_actor_names[i];
  }
  const ccord_user *u = ccord_store_user(ui->d.store, uid);
  return (u && u->name && u->name[0]) ? u->name : "Selected User";
}

void ss_draw_audit(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  ss_populate_audit_actors(ui);
  float x = g->col_x, w = g->col_w, y = g->top_y - ss->scroll;
  ss_title(c, g, "Audit Log", y);
  y += 56.0f;
  float cx = x;
  for (int cat = CCORD_AUDIT_CAT_ALL; cat < CCORD_AUDIT_CAT_COUNT; cat++) {
    const char *label = ccord_audit_category_label((ccord_audit_category)cat);
    float cw = uis_label_width(ui, c->r, label);
    if (cx + cw > x + w - 100.0f && cx > x) {
      cx = x;
      y += 32.0f;
    }
    char nm[SS_NAME_LEN];
    ss_filter_tab(c, (ccord_rect){cx, y, cw, UIS_H_BTN_SM}, label, ss->audit_filter == (ccord_audit_category)cat,
                  A_AUDIT_FILTER, (uint64_t)cat, NAME(nm, "audit-filter:%d", cat));
    cx += cw + UIS_SP_2;
  }
  ss_button(c, (ccord_rect){x + w - 100.0f, y - 3.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr, A_REFRESH, 0, "refresh");
  y += 26.0f + 12.0f;

  // Server-side query filters: Filter by User + Filter by Action
  float sel_w = fminf((w - 12.0f) * 0.5f, 280.0f);
  char ulbl[CCORD_NAME_MAX_BYTES + 32];
  (void)snprintf(ulbl, sizeof ulbl, "Filter by User: %s", ss_audit_actor_label(ui, ss->audit_user_id));
  ss_select(c, (ccord_rect){x, y, sel_w, UIS_H_INPUT}, ulbl, A_AUDIT_USER, 0, nullptr, "audit-user");

  char albl[96];
  (void)snprintf(albl, sizeof albl, "Filter by Action: %s",
                 ss->audit_action == 0 ? "All Actions" : ccord_audit_action_label(ss->audit_action));
  ss_select(c, (ccord_rect){x + sel_w + 12.0f, y, sel_w, UIS_H_INPUT}, albl, A_AUDIT_ACTION, 0, nullptr, "audit-action");
  y += UIS_H_INPUT + 18.0f;

  size_t n = 0;
  const ccord_audit_entry *log = ccord_store_audit(ui->d.store, ss->guild_id, &n);
  if (ss_draw_list_state(c, SS_AUDIT, ccord_store_audit_loaded(ui->d.store, ss->guild_id), x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }
  size_t shown = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_audit_entry *e = &log[i];
    if (!ccord_audit_matches(ss->audit_filter, e->action)) continue;
    shown++;
    bool has_detail = e->detail && e->detail[0], has_reason = e->reason && e->reason[0];
    bool expanded = e->change_count > 0 && ss->audit_expanded_id == e->id;
    float lh = ui_line_h(ui, M(font_size_small));
    float h = 44.0f + (has_detail ? lh + 2.0f : 0.0f) + (has_reason ? lh + 2.0f : 0.0f) +
              (expanded ? (float)e->change_count * (lh + 4.0f) + 8.0f : 0.0f);
    h = ss_grid(h);
    ccord_rect row = {x, y, w, h};
    y += h;
    if (!ss_visible(c, row)) continue;
    if (e->change_count > 0) {
      char rnm[SS_NAME_LEN];
      bool rhov = ss_hit_add(c, row, A_AUDIT_EXPAND, e->id, nullptr, NAME(rnm, "audit-row:%llu", (unsigned long long)e->id));
      uis_list_row_fill(ui, c->r, (ccord_rect){row.x - 6.0f, row.y + 2.0f, row.w + 12.0f, row.h - 4.0f},
                        (uis_state){.hover = rhov, .selected = expanded}, "ssauditrow", e->id);
    }
    const char *who = e->user_name && e->user_name[0] ? e->user_name : "Someone";
    float ty = row.y + 8.0f;
    float lx = row.x;
    float bw = ss_text_width(c, who, M(font_size_base), true);
    (void)ss_text(c, who, lx, ty, M(font_size_base), true, COL(text_header), w * 0.4f);
    lx += fminf(bw, w * 0.4f) + 6.0f;
    char what[CCORD_NAME_MAX_BYTES + 128];
    if (e->target_name && e->target_name[0]) {
      if (e->change_count > 0) {
        (void)snprintf(what, sizeof what, "%s %s  [%s%zu]", ccord_audit_action_label(e->action), e->target_name,
                       expanded ? "-" : "+", e->change_count);
      } else {
        (void)snprintf(what, sizeof what, "%s %s", ccord_audit_action_label(e->action), e->target_name);
      }
    } else if (e->change_count > 0) {
      (void)snprintf(what, sizeof what, "%s  [%s%zu]", ccord_audit_action_label(e->action),
                     expanded ? "-" : "+", e->change_count);
    } else {
      (void)snprintf(what, sizeof what, "%s", ccord_audit_action_label(e->action));
    }
    (void)ss_text(c, what, lx, ty, M(font_size_base), false, COL(text_normal), row.x + w - lx - 160.0f);
    char when[48];
    (void)ccord_timefmt_message(when, sizeof when, e->time_ms, ui->now_unix_ms, ui->use_utc);
    float ww = ss_text_width(c, when, SS_CAP(c), false);
    (void)ss_text(c, when, row.x + w - ww, ty + 2.0f, SS_CAP(c), false, COL(text_muted), 0.0f);
    float ly = ty + ui_line_h(ui, M(font_size_base)) + 4.0f;
    if (has_detail) {
      (void)ss_text(c, e->detail, row.x, ly, M(font_size_small), false, COL(text_secondary), w - 10.0f);
      ly += lh + 2.0f;
    }
    if (has_reason) {
      char rs[CCORD_NAME_MAX_BYTES + 160];
      (void)snprintf(rs, sizeof rs, "Reason: %s", e->reason);
      (void)ss_text(c, rs, row.x, ly, SS_CAP(c), false, COL(text_muted), w - 10.0f);
      ly += lh + 2.0f;
    }
    if (expanded) {
      ly += 4.0f;
      for (size_t k = 0; k < e->change_count; k++) {
        const ccord_audit_change *ch = &e->changes[k];
        char diff[256];
        if (ch->old_val[0] && ch->new_val[0]) {
          (void)snprintf(diff, sizeof diff, "%s: %s -> %s", ch->key, ch->old_val, ch->new_val);
        } else if (ch->new_val[0]) {
          (void)snprintf(diff, sizeof diff, "%s: set to %s", ch->key, ch->new_val);
        } else if (ch->old_val[0]) {
          (void)snprintf(diff, sizeof diff, "%s: cleared (was %s)", ch->key, ch->old_val);
        } else {
          (void)snprintf(diff, sizeof diff, "%s: updated", ch->key);
        }
        ccord_rect drc = {row.x + 12.0f, ly, w - 24.0f, lh + 2.0f};
        char cnm[SS_NAME_LEN];
        (void)ss_hit_add(c, drc, A_AUDIT_EXPAND, e->id, nullptr,
                         NAME(cnm, "audit-change:%llu:%zu", (unsigned long long)e->id, k));
        ccord_draw_rect(c->r, (ccord_rect){row.x + 4.0f, ly + 2.0f, 3.0f, lh - 2.0f}, COL(brand));
        (void)ss_text(c, diff, drc.x, drc.y, M(font_size_small), false, COL(text_normal), drc.w);
        ly += lh + 4.0f;
      }
    }
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  ss->rows = shown;
  if (shown == 0) {
    ss_message(c, n == 0 ? "Nothing has been logged yet." : "No entries match this filter.", x, y, w);
    y += 40.0f;
  }
  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- Danger Zone (Owner Only)

ccord_id ss_transfer_target(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_store *st = ui->d.store;
  ccord_id self = ccord_store_self_id(st);
  size_t n = ccord_store_member_count(st, ss->guild_id);
  ccord_id first = CCORD_ID_NONE;
  for (size_t i = 0; i < n; i++) {
    const ccord_member *m = ccord_store_member_at(st, ss->guild_id, i);
    if (!m || m->user_id == self) continue;
    const ccord_user *u = ccord_store_user(st, m->user_id);
    if (u && u->is_bot) continue;
    if (first == CCORD_ID_NONE) first = m->user_id;
    if (m->user_id == ss->transfer_user_id) return m->user_id;
  }
  return first;
}

void ss_draw_danger(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float x = g->col_x, w = g->col_w, y = g->top_y - ss->scroll;
  ss_title(c, g, "Danger Zone", y);
  y += 58.0f;

  ss_label(c, "TRANSFER OWNERSHIP", x, y);
  y += 24.0f;
  ss_message(c, "Hand ownership of this server to another non-bot member. You will remain a member, but can no longer delete the server.",
             x, y, w);
  y += 44.0f;
  ccord_id target = ss_transfer_target(ui);
  char mlbl[CCORD_NAME_MAX_BYTES + 48];
  if (target != CCORD_ID_NONE) {
    (void)snprintf(mlbl, sizeof mlbl, "New Owner: %s   (click to change)", ui_member_display_name(ui, ss->guild_id, target));
  } else {
    (void)snprintf(mlbl, sizeof mlbl, "No eligible non-bot members loaded");
  }
  ss_select(c, (ccord_rect){x, y, fminf(w, 360.0f), UIS_H_INPUT}, mlbl, A_TRANSFER_MEMBER, 0,
            target == CCORD_ID_NONE ? "No eligible member loaded to transfer ownership to." : nullptr, "transfer-member");
  y += UIS_H_INPUT + 14.0f;
  ss_button(c, (ccord_rect){x, y, 168.0f, 34.0f}, "Transfer Ownership", BTN_DANGER,
            target == CCORD_ID_NONE ? "No eligible member loaded to transfer ownership to." : nullptr,
            A_TRANSFER_OWNER, 0, "transfer-owner");
  y += 34.0f + 38.0f;

  uis_divider(ui, c->r, x, y, w);
  y += 26.0f;

  ss_label(c, "DELETE SERVER", x, y);
  y += 24.0f;
  ss_message(c, "Permanently delete this server, its channels, and its roles for everyone. You will be asked to type the server's exact name to confirm.",
             x, y, w);
  y += 44.0f;
  ss_button(c, (ccord_rect){x, y, 148.0f, 34.0f}, "Delete Server", BTN_DANGER, nullptr, A_DELETE_GUILD, 0, "delete-guild");
  y += 34.0f;
  ss_end_content(ss, y);
}

