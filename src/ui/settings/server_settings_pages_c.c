// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "server_settings_internal.h"

// ---------------------------------------------------------------- AutoMod

const char *ss_automod_trig_label(uint8_t trig)
{
  switch (trig) {
    case CCORD_AUTOMOD_TRIGGER_KEYWORD: return "Custom Keywords";
    case CCORD_AUTOMOD_TRIGGER_SPAM: return "Suspected Spam";
    case CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET: return "Commonly Flagged Words";
    case CCORD_AUTOMOD_TRIGGER_MENTION_SPAM: return "Mention Spam";
    case CCORD_AUTOMOD_TRIGGER_MEMBER_PROFILE: return "Member Profile Keywords";
    default: return "AutoMod Rule";
  }
}

static bool ss_automod_singleton_taken(const ccord_ui *ui, uint8_t trig, ccord_id ignore_id)
{
  if (trig != CCORD_AUTOMOD_TRIGGER_SPAM && trig != CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET &&
      trig != CCORD_AUTOMOD_TRIGGER_MENTION_SPAM) {
    return false;
  }
  size_t n = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(ui->d.store, ui->ss->guild_id, &n);
  for (size_t i = 0; rules && i < n; i++) {
    if (rules[i].id != ignore_id && rules[i].trigger_type == trig) return true;
  }
  return false;
}

ccord_id ss_am_default_alert_chan(const ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->am_alert_chan != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ss->am_alert_chan);
    if (ch && ch->guild_id == ss->guild_id && ch->kind == CCORD_CHANNEL_TEXT &&
        !ccord_discord_type_is_thread(ch->discord_type)) {
      return ss->am_alert_chan;
    }
  }
  size_t n = ccord_store_channel_count(ui->d.store, ss->guild_id);
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(ui->d.store, ss->guild_id, i);
    if (ch && ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type)) return ch->id;
  }
  return CCORD_ID_NONE;
}

static void draw_automod_editor(ss_cx *c, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float y = *py;

  ccord_rect acc = {x, y, w, 3.0f};
  ccord_draw_rrect(c->r, acc, acc.h * 0.5f, COL(brand));
  y += 12.0f;
  (void)ss_text(c, ss->am_edit_id == CCORD_ID_NONE ? "Create AutoMod Rule" : "Edit AutoMod Rule",
                x, y, SS_TITLE(c), true, COL(text_header), w);
  y += 32.0f;

  ss_label(c, "RULE NAME", x, y);
  y += 22.0f;
  ss_field_box(c, (ccord_rect){x, y, fminf(w, 460.0f), 36.0f}, &ss->am_name, SSF_AM_NAME,
               "e.g. Block Scam Links", nullptr, "automod-name");
  y += 36.0f + 16.0f;

  ss_label(c, "RULE TRIGGER TYPE", x, y);
  y += 22.0f;
  static const uint8_t k_trigs[4] = {
    CCORD_AUTOMOD_TRIGGER_KEYWORD,
    CCORD_AUTOMOD_TRIGGER_SPAM,
    CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET,
    CCORD_AUTOMOD_TRIGGER_MENTION_SPAM
  };
  float cx = x;
  for (size_t i = 0; i < 4u; i++) {
    uint8_t tr_t = k_trigs[i];
    const char *lbl = ss_automod_trig_label(tr_t);
    float cw = ss_text_width(c, lbl, SS_CAP(c), false) + 22.0f;
    if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
    const char *twhy = nullptr;
    if (ss->am_edit_id != CCORD_ID_NONE && ss->am_trigger != tr_t) {
      twhy = "Discord does not allow changing a rule's trigger type after creation.";
    } else if (ss->am_edit_id == CCORD_ID_NONE && ss_automod_singleton_taken(ui, tr_t, CCORD_ID_NONE)) {
      twhy = "This server already has a rule of this type.";
    }
    char nm[SS_NAME_LEN];
    ss_button(c, (ccord_rect){cx, y, cw, 26.0f}, lbl, ss->am_trigger == tr_t ? BTN_PRIMARY : BTN_SECONDARY,
              twhy, A_AM_TRIG, tr_t, NAME(nm, "automod-trig:%u", (unsigned)tr_t));
    cx += cw + 8.0f;
  }
  y += 26.0f + 18.0f;

  // Trigger-specific fields
  if (ss->am_trigger == CCORD_AUTOMOD_TRIGGER_KEYWORD || ss->am_trigger == CCORD_AUTOMOD_TRIGGER_MEMBER_PROFILE) {
    ss_label(c, "KEYWORDS (COMMA-SEPARATED, SUPPORTS * WILDCARDS)", x, y);
    y += 22.0f;
    ss_field_box(c, (ccord_rect){x, y, w, 36.0f}, &ss->am_keywords, SSF_AM_KEYWORDS,
                 "free nitro, *scam.gg*, steamcommunity-gift", nullptr, "automod-keywords");
    y += 36.0f + 12.0f;

    ss_label(c, "REGEX PATTERNS (COMMA-SEPARATED RUST REGEX, OPTIONAL)", x, y);
    y += 22.0f;
    ss_field_box(c, (ccord_rect){x, y, w, 36.0f}, &ss->am_regex, SSF_AM_REGEX,
                 "discord\\.gift/[A-Za-z0-9]+", nullptr, "automod-regex");
    y += 36.0f + 12.0f;

    ss_label(c, "ALLOW LIST (EXEMPT WORDS, COMMA-SEPARATED)", x, y);
    y += 22.0f;
    ss_field_box(c, (ccord_rect){x, y, w, 36.0f}, &ss->am_allow, SSF_AM_ALLOW,
                 "official-link.com", nullptr, "automod-allow");
    y += 36.0f + 16.0f;
  } else if (ss->am_trigger == CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET) {
    ss_label(c, "DISCORD PRESET WORD LISTS", x, y);
    y += 22.0f;
    static const uint32_t k_pmask[3] = {
      CCORD_AUTOMOD_PRESET_PROFANITY,
      CCORD_AUTOMOD_PRESET_SEXUAL_CONTENT,
      CCORD_AUTOMOD_PRESET_SLURS
    };
    static const char *const k_plbl[3] = {"Severe Profanity", "Insults & Slurs (Sexual Content)", "Hate Speech / Slurs"};
    cx = x;
    for (size_t i = 0; i < 3u; i++) {
      float cw = ss_text_width(c, k_plbl[i], SS_CAP(c), false) + 24.0f;
      if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){cx, y, cw, 26.0f}, k_plbl[i], (ss->am_presets & k_pmask[i]) != 0,
              A_AM_PRESET, k_pmask[i], NAME(nm, "automod-preset:%u", (unsigned)k_pmask[i]));
      cx += cw + 8.0f;
    }
    y += 26.0f + 12.0f;
    ss_label(c, "ALLOW LIST (EXEMPT WORDS, COMMA-SEPARATED)", x, y);
    y += 22.0f;
    ss_field_box(c, (ccord_rect){x, y, w, 36.0f}, &ss->am_allow, SSF_AM_ALLOW,
                 "exempt words", nullptr, "automod-allow");
    y += 36.0f + 16.0f;
  } else if (ss->am_trigger == CCORD_AUTOMOD_TRIGGER_MENTION_SPAM) {
    ss_label(c, "UNIQUE MENTION LIMIT PER MESSAGE (1-50)", x, y);
    y += 22.0f;
    static const uint8_t k_mlims[7] = {3u, 5u, 10u, 15u, 20u, 30u, 50u};
    cx = x;
    for (size_t i = 0; i < 7u; i++) {
      char mlbl[16], nm[SS_NAME_LEN];
      (void)snprintf(mlbl, sizeof mlbl, "%u", (unsigned)k_mlims[i]);
      ss_chip(c, (ccord_rect){cx, y, 48.0f, 26.0f}, mlbl, ss->am_mention_limit == k_mlims[i],
              A_AM_MENTION_LIM, k_mlims[i], NAME(nm, "automod-mention-limit:%u", (unsigned)k_mlims[i]));
      cx += 56.0f;
    }
    y += 26.0f + 12.0f;
    (void)ss_text(c, "Mention Raid Protection", x, y + 3.0f, M(font_size_base), false, COL(text_normal), w - 60.0f);
    ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->am_mention_raid, nullptr, A_AM_RAID, 0,
              "automod-raid");
    y += 34.0f;
  } else {
    (void)ss_text(c, "Automatically filters messages matching Discord's suspected spam heuristics.",
                  x, y, SS_CAP(c), false, COL(text_muted), w);
    y += 28.0f;
  }

  // Responses / Actions
  ss_label(c, "RESPONSES (CHOOSE AT LEAST ONE)", x, y);
  y += 24.0f;

  // 1. Block Message
  (void)ss_text(c, "Block Message", x, y + 3.0f, M(font_size_base), true, COL(text_header), w - 60.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->am_act_block, nullptr, A_AM_ACT_BLOCK, 0,
            "automod-act-block");
  y += 28.0f;
  if (ss->am_act_block) {
    ss_field_box(c, (ccord_rect){x, y, w, 34.0f}, &ss->am_custom_msg, SSF_AM_CUSTOM_MSG,
                 "Custom explanation shown to the member (optional, max 150 chars)", nullptr, "automod-custom-msg");
    y += 34.0f + 10.0f;
  } else {
    y += 6.0f;
  }

  // 2. Send Alert
  ccord_id achan = ss_am_default_alert_chan(ui);
  const char *alert_why = achan == CCORD_ID_NONE ? "Create a text channel first to receive alerts." : nullptr;
  (void)ss_text(c, "Send Alert to Channel", x, y + 3.0f, M(font_size_base), true, COL(text_header), w - 60.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->am_act_alert && achan != CCORD_ID_NONE, alert_why,
            A_AM_ACT_ALERT, 0, "automod-act-alert");
  y += 28.0f;
  if (ss->am_act_alert && achan != CCORD_ID_NONE) {
    const ccord_channel *ach = ccord_store_channel(ui->d.store, achan);
    char clbl[CCORD_NAME_MAX_BYTES + 40];
    (void)snprintf(clbl, sizeof clbl, "Alert Channel: #%s   (click to change)", ach && ach->name ? ach->name : "channel");
    ss_select(c, (ccord_rect){x, y, fminf(w, 360.0f), UIS_H_INPUT}, clbl, A_AM_ALERT_CHAN, 0, nullptr, "automod-alert-chan");
    y += UIS_H_INPUT + 10.0f;
  } else {
    y += 6.0f;
  }

  // 3. Timeout Member (KEYWORD and MENTION_SPAM only)
  bool timeout_ok = (ss->am_trigger == CCORD_AUTOMOD_TRIGGER_KEYWORD || ss->am_trigger == CCORD_AUTOMOD_TRIGGER_MENTION_SPAM);
  const char *to_why = timeout_ok ? nullptr : "Timeout action is only supported on Custom Keywords and Mention Spam rules.";
  (void)ss_text(c, "Timeout Member", x, y + 3.0f, M(font_size_base), true, COL(text_header), w - 60.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->am_act_timeout && timeout_ok, to_why,
            A_AM_ACT_TIMEOUT, 0, "automod-act-timeout");
  y += 28.0f;
  if (ss->am_act_timeout && timeout_ok) {
    static const uint32_t k_tdurs[6] = {60u, 300u, 600u, 3600u, 86400u, 604800u};
    static const char *const k_tlbls[6] = {"60 sec", "5 min", "10 min", "1 hour", "1 day", "1 week"};
    cx = x;
    for (size_t i = 0; i < 6u; i++) {
      float cw = ss_text_width(c, k_tlbls[i], SS_CAP(c), false) + 22.0f;
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){cx, y, cw, 25.0f}, k_tlbls[i], ss->am_timeout_sec == k_tdurs[i], A_AM_TIMEOUT_DUR,
              k_tdurs[i], NAME(nm, "automod-timeout-dur:%u", k_tdurs[i]));
      cx += cw + 8.0f;
    }
    y += 25.0f + 12.0f;
  } else {
    y += 6.0f;
  }

  // Exempt Roles & Channels
  size_t nroles = 0;
  const ccord_role *roles = ss_roles(ui, &nroles);
  if (roles && nroles > 1u) {
    ss_label(c, "EXEMPT ROLES", x, y);
    y += 22.0f;
    cx = x;
    for (size_t i = 0; i < nroles; i++) {
      if (roles[i].id == ss->guild_id) continue;
      const char *rn = roles[i].name && roles[i].name[0] ? roles[i].name : "role";
      float cw = fminf(160.0f, ss_text_width(c, rn, SS_CAP(c), false) + 22.0f);
      if (cx + cw > x + w && cx > x) { cx = x; y += 29.0f; }
      bool sel = ss_id_in_array(ss->am_exempt_roles, ss->am_exempt_role_count, roles[i].id);
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){cx, y, cw, 25.0f}, rn, sel, A_AM_EXEMPT_ROLE, roles[i].id,
              NAME(nm, "automod-exempt-role:%llu", (unsigned long long)roles[i].id));
      cx += cw + 6.0f;
    }
    y += 25.0f + 14.0f;
  }

  size_t nchans = ccord_store_channel_count(ui->d.store, ss->guild_id);
  if (nchans > 0u) {
    ss_label(c, "EXEMPT CHANNELS", x, y);
    y += 22.0f;
    cx = x;
    for (size_t i = 0; i < nchans; i++) {
      const ccord_channel *ch = ccord_store_channel_at(ui->d.store, ss->guild_id, i);
      if (!ch || ch->discord_type == CCORD_DISCORD_CHANNEL_GUILD_CATEGORY ||
          ccord_discord_type_is_thread(ch->discord_type)) {
        continue;
      }
      char cnm[CCORD_NAME_MAX_BYTES + 4];
      (void)snprintf(cnm, sizeof cnm, "%s%s", ch->kind == CCORD_CHANNEL_TEXT ? "#" : "", ch->name ? ch->name : "chan");
      float cw = fminf(160.0f, ss_text_width(c, cnm, SS_CAP(c), false) + 22.0f);
      if (cx + cw > x + w && cx > x) { cx = x; y += 29.0f; }
      bool sel = ss_id_in_array(ss->am_exempt_chans, ss->am_exempt_chan_count, ch->id);
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){cx, y, cw, 25.0f}, cnm, sel, A_AM_EXEMPT_CHAN, ch->id,
              NAME(nm, "automod-exempt-chan:%llu", (unsigned long long)ch->id));
      cx += cw + 6.0f;
    }
    y += 25.0f + 14.0f;
  }

  (void)ss_text(c, "Rule Enabled", x, y + 3.0f, M(font_size_base), true, COL(text_header), w - 60.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->am_enabled, nullptr, A_AM_ENABLED, 0,
            "automod-enabled");
  y += 34.0f;

  if (ss->am_error[0] != '\0') {   // validation stays with the form until the field is corrected or saved again
    (void)ss_hit_add(c, (ccord_rect){x, y, w, 20.0f}, A_NONE, 0, nullptr, "automod-error");
    (void)ss_text(c, ss->am_error, x, y, SS_CAP(c), false, COL(danger), w);
    y += 24.0f;
  }

  ss_button(c, (ccord_rect){x, y, 124.0f, 34.0f}, ss->am_edit_id == CCORD_ID_NONE ? "Create Rule" : "Save Rule",
            BTN_PRIMARY, nullptr, A_AM_SAVE, 0, "automod-save");
  ss_button(c, (ccord_rect){x + 134.0f, y, 96.0f, 34.0f}, "Cancel", BTN_SECONDARY, nullptr, A_AM_CANCEL, 0,
            "automod-cancel");
  y += 34.0f + 24.0f;
  uis_divider(ui, c->r, x, y, w);
  *py = y + 20.0f;
}

void ss_draw_automod(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "AutoMod", y);
  ss_button(c, (ccord_rect){x + w - 224.0f, y - 2.0f, 114.0f, 32.0f}, "Create Rule", BTN_PRIMARY, nullptr,
            A_AM_NEW, CCORD_AUTOMOD_TRIGGER_KEYWORD, "automod-new");
  ss_button(c, (ccord_rect){x + w - 100.0f, y - 2.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr,
            A_REFRESH, 0, "refresh");
  y += 52.0f;

  if (ss->am_editing) {
    draw_automod_editor(c, x, w, &y);
  }

  bool loaded = ccord_store_automod_loaded(ui->d.store, ss->guild_id);
  if (ss_draw_list_state(c, SS_AUTOMOD, loaded, x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }

  size_t n = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(ui->d.store, ss->guild_id, &n);
  ss->rows = n;

  // Quick-add preset strip when not editing
  if (!ss->am_editing) {
    ss_label(c, "QUICK SETUP", x, y);
    y += 22.0f;
    static const uint8_t k_qtrigs[4] = {
      CCORD_AUTOMOD_TRIGGER_KEYWORD,
      CCORD_AUTOMOD_TRIGGER_SPAM,
      CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET,
      CCORD_AUTOMOD_TRIGGER_MENTION_SPAM
    };
    float cx = x;
    for (size_t i = 0; i < 4u; i++) {
      uint8_t qt = k_qtrigs[i];
      char qlbl[64], nm[SS_NAME_LEN];
      (void)snprintf(qlbl, sizeof qlbl, "+ %s", ss_automod_trig_label(qt));
      float cw = ss_text_width(c, qlbl, SS_CAP(c), false) + 20.0f;
      if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
      const char *qwhy = ss_automod_singleton_taken(ui, qt, CCORD_ID_NONE)
                           ? "This server already has a rule of this type."
                           : nullptr;
      ss_button(c, (ccord_rect){cx, y, cw, 26.0f}, qlbl, BTN_SECONDARY, qwhy, A_AM_NEW, qt,
                NAME(nm, "automod-new:%u", (unsigned)qt));
      cx += cw + 8.0f;
    }
    y += 26.0f + 22.0f;
  }

  ss_label(c, "CONFIGURED RULES", x, y);
  y += 24.0f;
  if (n == 0) {
    ss_message(c, "No AutoMod rules configured yet. Use Create Rule above to filter keywords, spam, or mass mentions.",
               x, y, w);
    y += 40.0f;
    ss_end_content(ss, y);
    return;
  }

  for (size_t i = 0; i < n; i++) {
    const ccord_automod_rule *ru = &rules[i];
    bool confirming = (ss->am_del_confirm_id == ru->id);
    ccord_rect row = {x, y, w, 68.0f};
    y += row.h;
    if (!ss_visible(c, row)) continue;

    char title_buf[CCORD_NAME_MAX_BYTES + 48];
    (void)snprintf(title_buf, sizeof title_buf, "%s  [%s]",
                   ru->name && ru->name[0] ? ru->name : "Unnamed Rule",
                   ss_automod_trig_label(ru->trigger_type));
    (void)ss_text(c, title_buf, row.x + 6.0f, row.y + 8.0f, M(font_size_base), true,
                  ru->enabled ? COL(text_header) : COL(text_muted), w - 240.0f);

    char trig_sum[128] = "";
    if (ru->trigger_type == CCORD_AUTOMOD_TRIGGER_KEYWORD || ru->trigger_type == CCORD_AUTOMOD_TRIGGER_MEMBER_PROFILE) {
      (void)snprintf(trig_sum, sizeof trig_sum, "%zu keyword%s, %zu regex", ru->keyword_count,
                     ru->keyword_count == 1 ? "" : "s", ru->regex_count);
    } else if (ru->trigger_type == CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET) {
      (void)snprintf(trig_sum, sizeof trig_sum, "Presets:%s%s%s",
                     (ru->presets & CCORD_AUTOMOD_PRESET_PROFANITY) ? " Profanity" : "",
                     (ru->presets & CCORD_AUTOMOD_PRESET_SEXUAL_CONTENT) ? " Sexual" : "",
                     (ru->presets & CCORD_AUTOMOD_PRESET_SLURS) ? " Slurs" : "");
    } else if (ru->trigger_type == CCORD_AUTOMOD_TRIGGER_MENTION_SPAM) {
      (void)snprintf(trig_sum, sizeof trig_sum, "Max %u mentions%s", (unsigned)ru->mention_total_limit,
                     ru->mention_raid_protection ? " + Raid shield" : "");
    } else {
      (void)snprintf(trig_sum, sizeof trig_sum, "Heuristic spam filter");
    }

    char act_sum[160];
    (void)snprintf(act_sum, sizeof act_sum, "%s  -  Actions:%s%s%s  -  Exempt: %zuR/%zuC", trig_sum,
                   ru->action_block ? " Block" : "",
                   ru->action_alert ? " Alert" : "",
                   ru->action_timeout ? " Timeout" : "",
                   ru->exempt_role_count, ru->exempt_channel_count);
    (void)ss_text(c, act_sum, row.x + 6.0f, row.y + 32.0f, SS_CAP(c), false, COL(text_muted), w - 240.0f);

    char nm[SS_NAME_LEN];
    if (confirming) {
      ss_button(c, (ccord_rect){row.x + w - 168.0f, row.y + 19.0f, 84.0f, 30.0f}, "Confirm", BTN_DANGER, nullptr,
                A_AM_DELETE_CONFIRM, ru->id, NAME(nm, "automod-delete-confirm:%llu", (unsigned long long)ru->id));
      ss_button(c, (ccord_rect){row.x + w - 78.0f, row.y + 19.0f, 78.0f, 30.0f}, "Cancel", BTN_SECONDARY, nullptr,
                A_AM_DELETE_CANCEL, ru->id, NAME(nm, "automod-delete-cancel:%llu", (unsigned long long)ru->id));
    } else {
      ss_toggle(c, (ccord_rect){row.x + w - 212.0f, row.y + 22.0f, 44.0f, 24.0f}, ru->enabled, nullptr,
                A_AM_TOGGLE, ru->id, NAME(nm, "automod-toggle:%llu", (unsigned long long)ru->id));
      char nm2[SS_NAME_LEN], nm3[SS_NAME_LEN];
      ss_button(c, (ccord_rect){row.x + w - 156.0f, row.y + 19.0f, 72.0f, 30.0f}, "Edit", BTN_SECONDARY, nullptr,
                A_AM_EDIT, ru->id, NAME(nm2, "automod-edit:%llu", (unsigned long long)ru->id));
      ss_button(c, (ccord_rect){row.x + w - 76.0f, row.y + 19.0f, 76.0f, 30.0f}, "Delete", BTN_DANGER, nullptr,
                A_AM_DELETE, ru->id, NAME(nm3, "automod-delete:%llu", (unsigned long long)ru->id));
    }
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- Integrations & Community (M4c)

static bool ss_is_guild_text_chan(const ccord_channel *ch)
{
  return ch && ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type);
}

static bool ss_is_onboarding_chan(const ccord_channel *ch)
{
  return ch && ch->kind != CCORD_CHANNEL_CATEGORY && !ccord_discord_type_is_thread(ch->discord_type);
}

static ccord_id ss_guild_nth_text_chan(const ccord_store *st, ccord_id gid, size_t nth)
{
  size_t n = ccord_store_channel_count(st, gid), seen = 0;
  ccord_id first = CCORD_ID_NONE;
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(st, gid, i);
    if (!ss_is_guild_text_chan(ch)) continue;
    if (first == CCORD_ID_NONE) first = ch->id;
    if (seen == nth) return ch->id;
    seen++;
  }
  return first;
}

ccord_id ss_resolve_guild_text_chan(const ccord_store *st, ccord_id gid, ccord_id cur, size_t fallback_nth)
{
  if (cur != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(st, cur);
    if (ch && ch->guild_id == gid && ss_is_guild_text_chan(ch)) return cur;
  }
  return ss_guild_nth_text_chan(st, gid, fallback_nth);
}

ccord_id ss_cycle_guild_text_chan(const ccord_store *st, ccord_id gid, ccord_id cur)
{
  size_t n = ccord_store_channel_count(st, gid);
  ccord_id first = CCORD_ID_NONE;
  bool pick_next = false;
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(st, gid, i);
    if (!ss_is_guild_text_chan(ch)) continue;
    if (first == CCORD_ID_NONE) first = ch->id;
    if (pick_next) return ch->id;
    if (ch->id == cur) pick_next = true;
  }
  return first;
}

ccord_id ss_resolve_follow_src_chan(const ccord_store *st, ccord_id prefer_other_gid, ccord_id cur)
{
  if (cur != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(st, cur);
    if (ch && ch->discord_type == CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) return cur;
  }
  size_t ng = ccord_store_guild_count(st);
  ccord_id any_ann = CCORD_ID_NONE;
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *gd = ccord_store_guild_at(st, gi);
    if (!gd) continue;
    size_t nc = ccord_store_channel_count(st, gd->id);
    for (size_t ci = 0; ci < nc; ci++) {
      const ccord_channel *ch = ccord_store_channel_at(st, gd->id, ci);
      if (ch && ch->discord_type == CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) {
        if (any_ann == CCORD_ID_NONE) any_ann = ch->id;
        if (gd->id != prefer_other_gid) return ch->id;
      }
    }
  }
  return any_ann;
}

ccord_id ss_cycle_follow_src_chan(const ccord_store *st, ccord_id cur)
{
  size_t ng = ccord_store_guild_count(st);
  ccord_id first = CCORD_ID_NONE;
  bool pick_next = false;
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *gd = ccord_store_guild_at(st, gi);
    if (!gd) continue;
    size_t nc = ccord_store_channel_count(st, gd->id);
    for (size_t ci = 0; ci < nc; ci++) {
      const ccord_channel *ch = ccord_store_channel_at(st, gd->id, ci);
      if (!ch || ch->discord_type != CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) continue;
      if (first == CCORD_ID_NONE) first = ch->id;
      if (pick_next) return ch->id;
      if (ch->id == cur) pick_next = true;
    }
  }
  return first;
}

static const char *ss_webhook_type_label(uint8_t type)
{
  switch (type) {
    case CCORD_WEBHOOK_INCOMING: return "Incoming";
    case CCORD_WEBHOOK_CHANNEL_FOLLOWER: return "Channel Follower";
    case CCORD_WEBHOOK_APPLICATION: return "Application";
    default: return "Webhook";
  }
}

void ss_draw_integrations(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_store *st = ui->d.store;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Integrations", y);
  ss_button(c, (ccord_rect){x + w - 100.0f, y - 2.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr,
            A_REFRESH, 0, "refresh");
  y += 52.0f;

  uint64_t perms = ss_self_perms(ui, ss->guild_id);
  bool can_wh = ccord_perm_has(perms, CCORD_PERM_MANAGE_WEBHOOKS);

  // ---- Webhooks CRUD ----
  ss_label(c, "WEBHOOKS", x, y);
  y += 24.0f;
  if (!can_wh) {
    ss_message(c, "Manage Webhooks permission is required to view, create, or edit server webhooks.", x, y, w);
    y += 42.0f;
  } else {
    ccord_id stale_ch = CCORD_ID_NONE;
    if (ccord_store_webhooks_stale(st, ss->guild_id, &stale_ch)) {
      ccord_rect ban_rc = {x, y, w, 40.0f};
      if (ss_visible(c, ban_rc)) {
        uis_surface(ui, c->r, ban_rc, COL(bg_secondary));
        const ccord_channel *sch = stale_ch != CCORD_ID_NONE ? ccord_store_channel(st, stale_ch) : nullptr;
        char sbuf[160];
        if (sch && sch->name) {
          (void)snprintf(sbuf, sizeof sbuf, "Webhooks in #%s changed outside this view.", sch->name);
        } else {
          (void)snprintf(sbuf, sizeof sbuf, "Webhooks in this server changed outside this view.");
        }
        (void)ss_text(c, sbuf, ban_rc.x + 12.0f, ban_rc.y + 11.0f, M(font_size_small), false, COL(text_normal),
                      ban_rc.w - 160.0f);
        ss_button(c, (ccord_rect){ban_rc.x + ban_rc.w - 138.0f, ban_rc.y + 5.0f, 128.0f, 30.0f}, "Reload Webhooks",
                  BTN_PRIMARY, nullptr, A_WH_REFRESH, 0, "webhook-stale-refresh");
      }
      y += 50.0f;
    }

    ccord_id new_ch_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->wh_new_chan, 0);
    const ccord_channel *new_ch = new_ch_id != CCORD_ID_NONE ? ccord_store_channel(st, new_ch_id) : nullptr;
    char ch_lbl[CCORD_NAME_MAX_BYTES + 24];
    (void)snprintf(ch_lbl, sizeof ch_lbl, "Channel: #%s", new_ch && new_ch->name ? new_ch->name : "none");
    float ch_w = fminf(220.0f, w * 0.34f);
    float btn_w = 122.0f;
    float name_w = fmaxf(120.0f, w - ch_w - btn_w - 16.0f);
    ss_button(c, (ccord_rect){x, y, ch_w, 34.0f}, ch_lbl, BTN_SECONDARY,
              new_ch ? nullptr : "No text channels in this server.", A_WH_NEW_CHAN, 0, "webhook-new-channel");
    ss_field_box(c, (ccord_rect){x + ch_w + 8.0f, y, name_w, 34.0f}, &ss->wh_new_name, SSF_WH_NEW_NAME,
                 "Webhook name (default: Captain Hook)", nullptr, "webhook-new-name");
    ss_button(c, (ccord_rect){x + ch_w + name_w + 16.0f, y, btn_w, 34.0f}, "New Webhook", BTN_PRIMARY,
              new_ch ? nullptr : "Create a text channel before adding a webhook.", A_WH_CREATE, 0, "webhook-create");
    y += 46.0f;

    bool wh_loaded = ccord_store_webhooks_loaded(st, ss->guild_id);
    if (!ss_draw_list_state(c, SS_INTEGRATIONS, wh_loaded, x, &y, w)) {
      size_t nwh = 0;
      const ccord_webhook *whs = ccord_store_webhooks(st, ss->guild_id, &nwh);
      ss->rows = nwh;
      if (nwh == 0) {
        ss_message(c, "No webhooks configured yet. Create one above to post automated updates into a channel.", x, y, w);
        y += 38.0f;
      } else {
        for (size_t i = 0; i < nwh; i++) {
          const ccord_webhook *wh = &whs[i];
          bool editing = (ss->wh_edit_id == wh->id);
          bool confirming = (ss->wh_del_confirm_id == wh->id);
          float row_h = editing ? 84.0f : 62.0f;
          ccord_rect row = {x, y, w, row_h};
          y += row_h;
          if (!ss_visible(c, row)) continue;

          if (editing) {
            uis_surface(ui, c->r, (ccord_rect){row.x, row.y + 2.0f, row.w, row.h - 4.0f}, COL(bg_secondary));
            float ew = fmaxf(140.0f, w - 210.0f);
            ss_field_box(c, (ccord_rect){row.x + 10.0f, row.y + 8.0f, ew, 32.0f}, &ss->wh_edit_name, SSF_WH_EDIT_NAME,
                         "Webhook name", nullptr, "webhook-edit-name");
            ss_button(c, (ccord_rect){row.x + w - 186.0f, row.y + 9.0f, 84.0f, 30.0f}, "Save", BTN_PRIMARY, nullptr,
                      A_WH_EDIT_SAVE, wh->id, "webhook-edit-save");
            ss_button(c, (ccord_rect){row.x + w - 94.0f, row.y + 9.0f, 84.0f, 30.0f}, "Cancel", BTN_SECONDARY, nullptr,
                      A_WH_EDIT_CANCEL, wh->id, "webhook-edit-cancel");

            ccord_id ech_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->wh_edit_chan, 0);
            const ccord_channel *ech = ech_id != CCORD_ID_NONE ? ccord_store_channel(st, ech_id) : nullptr;
            char eclbl[CCORD_NAME_MAX_BYTES + 28];
            (void)snprintf(eclbl, sizeof eclbl, "Post to: #%s (click to change)",
                           ech && ech->name ? ech->name : "none");
            const char *ec_why = wh->type == CCORD_WEBHOOK_INCOMING || wh->type == CCORD_WEBHOOK_CHANNEL_FOLLOWER
                                   ? nullptr
                                   : "Application webhooks manage their own channel.";
            ss_button(c, (ccord_rect){row.x + 10.0f, row.y + 46.0f, fminf(320.0f, w - 24.0f), 28.0f}, eclbl,
                      BTN_SECONDARY, ec_why, A_WH_EDIT_CHAN, wh->id, "webhook-edit-channel");
          } else {
            const ccord_channel *wch = wh->channel_id != CCORD_ID_NONE ? ccord_store_channel(st, wh->channel_id) : nullptr;
            char title_buf[180];
            (void)snprintf(title_buf, sizeof title_buf, "%s  [%s]",
                           wh->name && wh->name[0] ? wh->name : "Unnamed Webhook", ss_webhook_type_label(wh->type));
            (void)ss_text(c, title_buf, row.x + 6.0f, row.y + 8.0f, M(font_size_base), true, COL(text_header), w - 260.0f);

            char sub_buf[220];
            if (wh->type == CCORD_WEBHOOK_CHANNEL_FOLLOWER && wh->source_guild_name && wh->source_guild_name[0]) {
              (void)snprintf(sub_buf, sizeof sub_buf, "#%s  <-  %s #%s",
                             wch && wch->name ? wch->name : "unknown", wh->source_guild_name,
                             wh->source_channel_name && wh->source_channel_name[0] ? wh->source_channel_name : "news");
            } else if (wh->creator_name && wh->creator_name[0]) {
              (void)snprintf(sub_buf, sizeof sub_buf, "#%s  -  Created by %s",
                             wch && wch->name ? wch->name : "unknown", wh->creator_name);
            } else {
              (void)snprintf(sub_buf, sizeof sub_buf, "#%s", wch && wch->name ? wch->name : "unknown");
            }
            (void)ss_text(c, sub_buf, row.x + 6.0f, row.y + 31.0f, SS_CAP(c), false, COL(text_muted),
                          w - 260.0f);

            char nm1[SS_NAME_LEN], nm2[SS_NAME_LEN], nm3[SS_NAME_LEN];
            if (confirming) {
              ss_button(c, (ccord_rect){row.x + w - 168.0f, row.y + 16.0f, 84.0f, 30.0f}, "Confirm", BTN_DANGER, nullptr,
                        A_WH_DELETE_CONFIRM, wh->id,
                        NAME(nm1, "webhook-delete-confirm:%llu", (unsigned long long)wh->id));
              ss_button(c, (ccord_rect){row.x + w - 78.0f, row.y + 16.0f, 78.0f, 30.0f}, "Cancel", BTN_SECONDARY, nullptr,
                        A_WH_DELETE_CANCEL, wh->id,
                        NAME(nm2, "webhook-delete-cancel:%llu", (unsigned long long)wh->id));
            } else {
              if (wh->type == CCORD_WEBHOOK_INCOMING && wh->token && wh->token[0]) {
                ss_button(c, (ccord_rect){row.x + w - 246.0f, row.y + 16.0f, 84.0f, 30.0f}, "Copy URL", BTN_SECONDARY,
                          nullptr, A_WH_COPY_URL, wh->id,
                          NAME(nm1, "webhook-copy-url:%llu", (unsigned long long)wh->id));
              }
              ss_button(c, (ccord_rect){row.x + w - 156.0f, row.y + 16.0f, 72.0f, 30.0f}, "Edit", BTN_SECONDARY, nullptr,
                        A_WH_EDIT, wh->id, NAME(nm2, "webhook-edit:%llu", (unsigned long long)wh->id));
              ss_button(c, (ccord_rect){row.x + w - 76.0f, row.y + 16.0f, 76.0f, 30.0f}, "Delete", BTN_DANGER, nullptr,
                        A_WH_DELETE, wh->id, NAME(nm3, "webhook-delete:%llu", (unsigned long long)wh->id));
            }
            uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
          }
        }
      }
    }
  }

  y += 20.0f;
  uis_divider(ui, c->r, x, y, w);
  y += 22.0f;

  // ---- Followed Announcement Channels ----
  ss_label(c, "CHANNELS FOLLOWED", x, y);
  y += 24.0f;
  ccord_id src_id = ss_resolve_follow_src_chan(st, ss->guild_id, ss->follow_src_chan);
  ccord_id dst_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->follow_dst_chan, 0);
  const ccord_channel *src_ch = src_id != CCORD_ID_NONE ? ccord_store_channel(st, src_id) : nullptr;
  const ccord_guild *src_gd = src_ch ? ccord_store_guild(st, src_ch->guild_id) : nullptr;
  const ccord_channel *dst_ch = dst_id != CCORD_ID_NONE ? ccord_store_channel(st, dst_id) : nullptr;

  char src_lbl[160], dst_lbl[120];
  if (src_ch && src_ch->name) {
    (void)snprintf(src_lbl, sizeof src_lbl, "Announcement: %s #%s",
                   src_gd && src_gd->name ? src_gd->name : "Server", src_ch->name);
  } else {
    (void)snprintf(src_lbl, sizeof src_lbl, "Announcement: none available");
  }
  (void)snprintf(dst_lbl, sizeof dst_lbl, "Into: #%s", dst_ch && dst_ch->name ? dst_ch->name : "none");

  const char *fol_why = !can_wh ? "Manage Webhooks permission is required to follow channels."
                      : !src_ch ? "No announcement channels are visible to follow."
                      : !dst_ch ? "No destination text channel in this server."
                                : nullptr;
  float btn_fw = 132.0f;
  float half_w = fmaxf(110.0f, (w - btn_fw - 16.0f) * 0.5f);
  ss_button(c, (ccord_rect){x, y, half_w, 34.0f}, src_lbl, BTN_SECONDARY,
            src_ch ? nullptr : "No announcement channels available.", A_FOLLOW_SRC_CHAN, 0, "follow-source-channel");
  ss_button(c, (ccord_rect){x + half_w + 8.0f, y, half_w, 34.0f}, dst_lbl, BTN_SECONDARY,
            dst_ch ? nullptr : "No text channels in this server.", A_FOLLOW_DST_CHAN, 0, "follow-target-channel");
  ss_button(c, (ccord_rect){x + half_w * 2.0f + 16.0f, y, btn_fw, 34.0f}, "Follow Channel", BTN_PRIMARY, fol_why,
            A_FOLLOW_SUBMIT, 0, "follow-submit");
  y += 44.0f;

  size_t nfc = 0;
  const ccord_followed_channel *fcs = ccord_store_followed_channels(st, ss->guild_id, &nfc);
  for (size_t i = 0; fcs && i < nfc; i++) {
    ccord_rect frow = {x, y, w, 30.0f};
    y += 30.0f;
    if (!ss_visible(c, frow)) continue;
    const ccord_channel *fsrc = ccord_store_channel(st, fcs[i].source_channel_id);
    const ccord_channel *fdst = ccord_store_channel(st, fcs[i].target_channel_id);
    char fdesc[180];
    if (fsrc && fsrc->name) {
      (void)snprintf(fdesc, sizeof fdesc, "Following #%s  ->  #%s (webhook %llu)", fsrc->name,
                     fdst && fdst->name ? fdst->name : "channel", (unsigned long long)fcs[i].webhook_id);
    } else {
      (void)snprintf(fdesc, sizeof fdesc, "Following channel %llu  ->  #%s (webhook %llu)",
                     (unsigned long long)fcs[i].source_channel_id,
                     fdst && fdst->name ? fdst->name : "channel", (unsigned long long)fcs[i].webhook_id);
    }
    (void)ss_text(c, fdesc, frow.x + 6.0f, frow.y + 6.0f, M(font_size_small), false, COL(text_secondary), w - 12.0f);
  }

  y += 18.0f;
  uis_divider(ui, c->r, x, y, w);
  y += 22.0f;

  // ---- Application / Bot Command Permissions Overview (read-only) ----
  ss_label(c, "BOTS AND APPS - COMMAND PERMISSIONS (READ-ONLY)", x, y);
  y += 24.0f;
  bool ap_loaded = ccord_store_app_cmd_perms_loaded(st, ss->guild_id);
  if (!ap_loaded) {
    if (!can_wh) (void)ss_draw_list_state(c, SS_INTEGRATIONS, false, x, &y, w);
    else {
      ss_message(c, "Loading application command permissions...", x, y, w);
      y += 34.0f;
    }
  } else {
    size_t napp = 0;
    const ccord_app_cmd_perms *apps = ccord_store_app_cmd_perms(st, ss->guild_id, &napp);
    if (!can_wh) ss->rows = napp;
    if (napp == 0) {
      ss_message(c, "No custom application or slash-command permission overrides are set in this server.", x, y, w);
      y += 36.0f;
    } else {
      for (size_t i = 0; i < napp; i++) {
        const ccord_app_cmd_perms *ap = &apps[i];
        float card_h = ss_grid(34.0f + (float)(ap->rule_count > 0 ? ap->rule_count : 1u) * 22.0f + 10.0f);
        ccord_rect card = {x, y, w, card_h};
        y += card_h + 8.0f;
        if (!ss_visible(c, card)) continue;
        uis_surface(ui, c->r, card, COL(bg_secondary));
        const ccord_user *app_u = ccord_store_user(st, ap->application_id);
        char hdr[160];
        if (ap->id == ap->application_id) {
          (void)snprintf(hdr, sizeof hdr, "%s (App %llu) - Application-wide defaults",
                         app_u && app_u->name ? app_u->name : "Application",
                         (unsigned long long)ap->application_id);
        } else {
          (void)snprintf(hdr, sizeof hdr, "%s (App %llu) - Command %llu",
                         app_u && app_u->name ? app_u->name : "Application",
                         (unsigned long long)ap->application_id, (unsigned long long)ap->id);
        }
        (void)ss_text(c, hdr, card.x + 12.0f, card.y + 8.0f, M(font_size_small), true, COL(text_header), card.w - 24.0f);
        float ry = card.y + 32.0f;
        if (ap->rule_count == 0) {
          (void)ss_text(c, "No explicit overrides", card.x + 16.0f, ry, SS_CAP(c), false,
                        COL(text_muted), card.w - 32.0f);
        }
        for (size_t ri = 0; ri < ap->rule_count; ri++) {
          const ccord_app_cmd_perm_rule *rl = &ap->rules[ri];
          char target[120];
          if (rl->type == CCORD_APP_PERM_ROLE) {
            if (rl->id == ss->guild_id) (void)snprintf(target, sizeof target, "Role: @everyone");
            else {
              const ccord_role *ro = ccord_store_role(st, ss->guild_id, rl->id);
              if (ro && ro->name) (void)snprintf(target, sizeof target, "Role: @%s", ro->name);
              else (void)snprintf(target, sizeof target, "Role: %llu", (unsigned long long)rl->id);
            }
          } else if (rl->type == CCORD_APP_PERM_USER) {
            const ccord_user *u = ccord_store_user(st, rl->id);
            if (u && u->name) (void)snprintf(target, sizeof target, "Member: %s", u->name);
            else (void)snprintf(target, sizeof target, "Member: %llu", (unsigned long long)rl->id);
          } else {
            if (ss->guild_id > 0 && rl->id == ss->guild_id - 1u) {
              (void)snprintf(target, sizeof target, "Channel: All Channels");
            } else {
              const ccord_channel *ch = ccord_store_channel(st, rl->id);
              if (ch && ch->name) (void)snprintf(target, sizeof target, "Channel: #%s", ch->name);
              else (void)snprintf(target, sizeof target, "Channel: %llu", (unsigned long long)rl->id);
            }
          }
          char rline[160];
          (void)snprintf(rline, sizeof rline, "%s  ->  %s", target, rl->permission ? "Allowed" : "Denied");
          (void)ss_text(c, rline, card.x + 16.0f, ry, SS_CAP(c), false,
                        rl->permission ? COL(text_normal) : COL(danger), card.w - 32.0f);
          ry += 22.0f;
        }
      }
    }
  }
  ss_end_content(ss, y);
}

static void draw_community_prompt_editor(ss_cx *c, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_store *st = ui->d.store;
  float y = *py;

  ss_label(c, ss->ob_edit_prompt_idx < ss->ob_work.prompt_count ? "EDIT QUESTION" : "NEW ONBOARDING QUESTION", x, y);
  y += 22.0f;
  ss_field_box(c, (ccord_rect){x, y, w, 36.0f}, &ss->ob_prompt_title, SSF_OB_PROMPT_TITLE,
               "Question prompt (e.g. What topics interest you?)", nullptr, "onboarding-prompt-title");
  y += 46.0f;

  // Type + flags row
  float cx = x;
  ss_chip(c, (ccord_rect){cx, y, 122.0f, 26.0f}, "Multiple Choice",
          ss->ob_work_prompt.type == CCORD_ONBOARDING_MULTIPLE_CHOICE, A_OB_PROMPT_TYPE,
          CCORD_ONBOARDING_MULTIPLE_CHOICE, "onboarding-prompt-type:0");
  cx += 130.0f;
  ss_chip(c, (ccord_rect){cx, y, 96.0f, 26.0f}, "Dropdown",
          ss->ob_work_prompt.type == CCORD_ONBOARDING_DROPDOWN, A_OB_PROMPT_TYPE,
          CCORD_ONBOARDING_DROPDOWN, "onboarding-prompt-type:1");
  cx += 108.0f;
  ss_chip(c, (ccord_rect){cx, y, 86.0f, 26.0f}, "Required", ss->ob_work_prompt.required,
          A_OB_PROMPT_REQ, 0, "onboarding-prompt-req");
  cx += 94.0f;
  ss_chip(c, (ccord_rect){cx, y, 108.0f, 26.0f}, "Single-Select", ss->ob_work_prompt.single_select,
          A_OB_PROMPT_SINGLE, 0, "onboarding-prompt-single");
  cx += 116.0f;
  if (cx + 118.0f > x + w) { cx = x; y += 32.0f; }
  ss_chip(c, (ccord_rect){cx, y, 118.0f, 26.0f}, "In Onboarding", ss->ob_work_prompt.in_onboarding,
          A_OB_PROMPT_INOB, 0, "onboarding-prompt-inob");
  y += 38.0f;

  // Existing options on this prompt
  ss_label(c, "ANSWER OPTIONS", x, y);
  y += 22.0f;
  if (ss->ob_work_prompt.option_count == 0) {
    ss_message(c, "Add at least one option that assigns a channel or role.", x, y, w);
    y += 30.0f;
  } else {
    for (size_t oi = 0; oi < ss->ob_work_prompt.option_count; oi++) {
      const ss_ob_opt_copy *op = &ss->ob_work_prompt.options[oi];
      ccord_rect orow = {x, y, w, 36.0f};
      y += 38.0f;
      if (!ss_visible(c, orow)) continue;
      uis_surface(ui, c->r, orow, COL(bg_secondary));
      char osum[180];
      (void)snprintf(osum, sizeof osum, "%s  (%zu channel%s, %zu role%s)",
                     op->title[0] ? op->title : "Option",
                     op->channel_count, op->channel_count == 1 ? "" : "s",
                     op->role_count, op->role_count == 1 ? "" : "s");
      (void)ss_text(c, osum, orow.x + 10.0f, orow.y + 9.0f, M(font_size_small), false, COL(text_normal), w - 104.0f);
      char nm[SS_NAME_LEN];
      ss_button(c, (ccord_rect){orow.x + w - 84.0f, orow.y + 4.0f, 78.0f, 28.0f}, "Remove", BTN_DANGER, nullptr,
                A_OB_OPT_DEL, (uint64_t)oi, NAME(nm, "onboarding-opt-del:%zu", oi));
    }
  }

  // Add new option sub-form
  if (ss->ob_work_prompt.option_count < CCORD_ONBOARDING_MAX_OPTIONS) {
    float half = (w - 10.0f) * 0.5f;
    ss_field_box(c, (ccord_rect){x, y, half, 34.0f}, &ss->ob_opt_title, SSF_OB_OPT_TITLE,
                 "New option label (e.g. Systems C)", nullptr, "onboarding-opt-title");
    ss_field_box(c, (ccord_rect){x + half + 10.0f, y, half, 34.0f}, &ss->ob_opt_desc, SSF_OB_OPT_DESC,
                 "Description (optional)", nullptr, "onboarding-opt-desc");
    y += 42.0f;

    ss_label(c, "OPTION CHANNELS", x, y);
    y += 20.0f;
    cx = x;
    size_t nch = ccord_store_channel_count(st, ss->guild_id);
    for (size_t ci = 0; ci < nch; ci++) {
      const ccord_channel *ch = ccord_store_channel_at(st, ss->guild_id, ci);
      if (!ss_is_onboarding_chan(ch)) continue;
      char clbl[CCORD_NAME_MAX_BYTES + 8], nm[SS_NAME_LEN];
      (void)snprintf(clbl, sizeof clbl, "#%s", ch->name ? ch->name : "channel");
      float cw = fminf(ss_text_width(c, clbl, SS_CAP(c), false) + 20.0f, w);
      if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
      bool sel = ss_id_in_array(ss->ob_new_opt_chans, ss->ob_new_opt_chan_count, ch->id);
      ss_chip(c, (ccord_rect){cx, y, cw, 24.0f}, clbl, sel, A_OB_OPT_CHAN, ch->id,
              NAME(nm, "onboarding-opt-chan:%llu", (unsigned long long)ch->id));
      cx += cw + 6.0f;
    }
    y += 30.0f;

    size_t nro = 0;
    const ccord_role *roles = ccord_store_roles(st, ss->guild_id, &nro);
    bool has_assignable_role = false;
    for (size_t ri = 0; roles && ri < nro; ri++) {
      if (roles[ri].id != ss->guild_id) { has_assignable_role = true; break; }
    }
    if (has_assignable_role) {
      ss_label(c, "OPTION ROLES", x, y);
      y += 20.0f;
      cx = x;
      for (size_t ri = 0; roles && ri < nro; ri++) {
        if (roles[ri].id == ss->guild_id) continue;
        char rlbl[CCORD_NAME_MAX_BYTES + 8], nm[SS_NAME_LEN];
        (void)snprintf(rlbl, sizeof rlbl, "@%s", roles[ri].name ? roles[ri].name : "role");
        float cw = fminf(ss_text_width(c, rlbl, SS_CAP(c), false) + 20.0f, w);
        if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
        bool sel = ss_id_in_array(ss->ob_new_opt_roles, ss->ob_new_opt_role_count, roles[ri].id);
        ss_chip(c, (ccord_rect){cx, y, cw, 24.0f}, rlbl, sel, A_OB_OPT_ROLE, roles[ri].id,
                NAME(nm, "onboarding-opt-role:%llu", (unsigned long long)roles[ri].id));
        cx += cw + 6.0f;
      }
      y += 30.0f;
    }

    ss_button(c, (ccord_rect){x, y, 114.0f, 30.0f}, "Add Option", BTN_SECONDARY, nullptr, A_OB_OPT_ADD, 0,
              "onboarding-opt-add");
    y += 40.0f;
  }

  ss_button(c, (ccord_rect){x, y, 132.0f, 34.0f}, "Save Question", BTN_PRIMARY, nullptr, A_OB_PROMPT_SAVE, 0,
            "onboarding-prompt-save");
  ss_button(c, (ccord_rect){x + 142.0f, y, 96.0f, 34.0f}, "Cancel", BTN_SECONDARY, nullptr, A_OB_PROMPT_CANCEL, 0,
            "onboarding-prompt-cancel");
  y += 46.0f;
  *py = y;
}

void ss_draw_community(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_store *st = ui->d.store;
  const ccord_guild *gd = ccord_store_guild(st, ss->guild_id);
  if (!gd) return;

  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Community", y);
  if (gd->feature_community) {
    ss_button(c, (ccord_rect){x + w - 100.0f, y - 2.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr,
              A_REFRESH, 0, "refresh");
  }
  y += 52.0f;

  // ---- State A: Enable Community Wizard ----
  if (!gd->feature_community) {
    ss_message(c, "Upgrade this server into a Community Server to unlock Welcome Screen, Server Onboarding, "
                  "Announcement Channels, and Community Updates.", x, y, w);
    y += 48.0f;

    ss_label(c, "STEP 1 - SAFETY CHECKS", x, y);
    y += 24.0f;
    (void)ss_text(c, "Scan media from all members and set default notifications to @mentions only",
                  x, y + 3.0f, M(font_size_base), false, COL(text_normal), w - 60.0f);
    ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->comm_wiz_safety, nullptr,
              A_COMM_WIZ_SAFETY, 0, "community-wiz-safety");
    y += 42.0f;

    ss_label(c, "STEP 2 - RULES & UPDATES CHANNELS", x, y);
    y += 24.0f;
    ccord_id r_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->comm_wiz_rules, 0);
    ccord_id u_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->comm_wiz_updates, 1);
    const ccord_channel *r_ch = r_id != CCORD_ID_NONE ? ccord_store_channel(st, r_id) : nullptr;
    const ccord_channel *u_ch = u_id != CCORD_ID_NONE ? ccord_store_channel(st, u_id) : nullptr;

    char r_lbl[CCORD_NAME_MAX_BYTES + 40], u_lbl[CCORD_NAME_MAX_BYTES + 48];
    (void)snprintf(r_lbl, sizeof r_lbl, "Rules or Guidelines: #%s (click to change)",
                   r_ch && r_ch->name ? r_ch->name : "none");
    (void)snprintf(u_lbl, sizeof u_lbl, "Community Updates: #%s (click to change)",
                   u_ch && u_ch->name ? u_ch->name : "none");
    const char *ch_why = (r_ch && u_ch) ? nullptr : "Create at least one text channel before enabling Community.";
    ss_button(c, (ccord_rect){x, y, fminf(w, 420.0f), 34.0f}, r_lbl, BTN_SECONDARY, ch_why,
              A_COMM_WIZ_RULES, 0, "community-wiz-rules");
    y += 42.0f;
    ss_button(c, (ccord_rect){x, y, fminf(w, 420.0f), 34.0f}, u_lbl, BTN_SECONDARY, ch_why,
              A_COMM_WIZ_UPDATES, 0, "community-wiz-updates");
    y += 50.0f;

    ss_label(c, "STEP 3 - FINISH SETUP", x, y);
    y += 24.0f;
    ss_button(c, (ccord_rect){x, y, 168.0f, 36.0f}, "Enable Community", BTN_PRIMARY, ch_why,
              A_COMM_ENABLE_SUBMIT, 0, "community-enable-submit");
    y += 48.0f;
    ss_end_content(ss, y);
    return;
  }

  // ---- State B: Community Overview + Welcome Screen + Onboarding ----
  ss_label(c, "COMMUNITY CHANNELS", x, y);
  y += 24.0f;
  ccord_id cr_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->comm_work_rules, 0);
  ccord_id cu_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->comm_work_updates, 1);
  const ccord_channel *cr_ch = cr_id != CCORD_ID_NONE ? ccord_store_channel(st, cr_id) : nullptr;
  const ccord_channel *cu_ch = cu_id != CCORD_ID_NONE ? ccord_store_channel(st, cu_id) : nullptr;
  char cr_lbl[CCORD_NAME_MAX_BYTES + 32], cu_lbl[CCORD_NAME_MAX_BYTES + 36];
  (void)snprintf(cr_lbl, sizeof cr_lbl, "Rules: #%s", cr_ch && cr_ch->name ? cr_ch->name : "none");
  (void)snprintf(cu_lbl, sizeof cu_lbl, "Updates: #%s", cu_ch && cu_ch->name ? cu_ch->name : "none");
  float half_c = (w - 10.0f) * 0.5f;
  ss_button(c, (ccord_rect){x, y, half_c, 34.0f}, cr_lbl, BTN_SECONDARY, nullptr,
            A_COMM_RULES_CHAN, 0, "community-rules-channel");
  ss_button(c, (ccord_rect){x + half_c + 10.0f, y, half_c, 34.0f}, cu_lbl, BTN_SECONDARY, nullptr,
            A_COMM_UPDATES_CHAN, 0, "community-updates-channel");
  y += 42.0f;
  ss_button(c, (ccord_rect){x, y, 186.0f, 32.0f}, "Save Community Channels", BTN_PRIMARY, nullptr,
            A_COMM_CHAN_SAVE, 0, "community-channels-save");
  ss_button(c, (ccord_rect){x + 196.0f, y, 150.0f, 32.0f}, "Disable Community", BTN_DANGER, nullptr,
            A_COMM_DISABLE, 0, "community-disable");
  y += 48.0f;
  uis_divider(ui, c->r, x, y, w);
  y += 22.0f;

  // ---- Welcome Screen ----
  ss_label(c, "WELCOME SCREEN", x, y);
  y += 24.0f;
  bool ws_loaded = ccord_store_welcome_screen_loaded(st, ss->guild_id);
  if (ss_draw_list_state(c, SS_COMMUNITY, ws_loaded, x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }

  (void)ss_text(c, "Show Welcome Screen to new members", x, y + 3.0f, M(font_size_base), false,
                COL(text_normal), w - 60.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->ws_work.enabled, nullptr,
            A_WS_ENABLED, 0, "welcome-enabled");
  y += 38.0f;

  ss_label(c, "SERVER DESCRIPTION", x, y);
  y += 20.0f;
  ss_field_box(c, (ccord_rect){x, y, w, 36.0f}, &ss->ws_desc, SSF_WS_DESC,
               "Describe your server for new members (max 140 chars)", nullptr, "welcome-desc");
  y += 46.0f;

  ss_label(c, "RECOMMENDED CHANNELS (UP TO 5)", x, y);
  y += 22.0f;
  for (size_t i = 0; i < ss->ws_work.channel_count; i++) {
    const ss_welcome_chan_copy *wc = &ss->ws_work.channels[i];
    ccord_rect wrow = {x, y, w, 38.0f};
    y += 40.0f;
    if (!ss_visible(c, wrow)) continue;
    uis_surface(ui, c->r, (ccord_rect){wrow.x, wrow.y + 1.0f, wrow.w, 36.0f}, COL(bg_secondary));
    const ccord_channel *wch = ccord_store_channel(st, wc->channel_id);
    char wline[180];
    (void)snprintf(wline, sizeof wline, "%s%s#%s  -  %s",
                   wc->emoji_name[0] ? wc->emoji_name : "",
                   wc->emoji_name[0] ? " " : "",
                   wch && wch->name ? wch->name : "channel", wc->description);
    (void)ss_text(c, wline, wrow.x + 10.0f, wrow.y + 10.0f, M(font_size_small), false, COL(text_normal), w - 104.0f);
    char nm[SS_NAME_LEN];
    ss_button(c, (ccord_rect){wrow.x + w - 84.0f, wrow.y + 5.0f, 78.0f, 28.0f}, "Remove", BTN_DANGER, nullptr,
              A_WS_CHAN_DEL, (uint64_t)i, NAME(nm, "welcome-chan-del:%zu", i));
  }

  if (ss->ws_work.channel_count < CCORD_WELCOME_MAX_CHANNELS) {
    ccord_id wadd_id = ss_resolve_guild_text_chan(st, ss->guild_id, ss->ws_add_chan, 0);
    const ccord_channel *wadd_ch = wadd_id != CCORD_ID_NONE ? ccord_store_channel(st, wadd_id) : nullptr;
    char walbl[CCORD_NAME_MAX_BYTES + 16];
    (void)snprintf(walbl, sizeof walbl, "#%s", wadd_ch && wadd_ch->name ? wadd_ch->name : "none");
    float ch_w = fminf(160.0f, w * 0.25f);
    float em_w = 96.0f;
    float add_w = 108.0f;
    float d_w = fmaxf(110.0f, w - ch_w - em_w - add_w - 24.0f);
    ss_button(c, (ccord_rect){x, y, ch_w, 34.0f}, walbl, BTN_SECONDARY,
              wadd_ch ? nullptr : "No text channels.", A_WS_ADD_CHAN, 0, "welcome-add-channel");
    ss_field_box(c, (ccord_rect){x + ch_w + 8.0f, y, d_w, 34.0f}, &ss->ws_add_desc, SSF_WS_ADD_DESC,
                 "What happens here?", nullptr, "welcome-add-desc");
    ss_field_box(c, (ccord_rect){x + ch_w + d_w + 16.0f, y, em_w, 34.0f}, &ss->ws_add_emoji, SSF_WS_ADD_EMOJI,
                 "Emoji", nullptr, "welcome-add-emoji");
    ss_button(c, (ccord_rect){x + ch_w + d_w + em_w + 24.0f, y, add_w, 34.0f}, "Add Channel", BTN_SECONDARY,
              wadd_ch ? nullptr : "No text channels.", A_WS_CHAN_ADD, 0, "welcome-chan-add");
    y += 44.0f;
  }

  ss_button(c, (ccord_rect){x, y, 168.0f, 34.0f}, "Save Welcome Screen", BTN_PRIMARY, nullptr,
            A_WS_SAVE, 0, "welcome-save");
  ss_button(c, (ccord_rect){x + 178.0f, y, 84.0f, 34.0f}, "Reset", BTN_SECONDARY, nullptr,
            A_WS_RESET, 0, "welcome-reset");
  y += 50.0f;
  uis_divider(ui, c->r, x, y, w);
  y += 22.0f;

  // ---- Server Onboarding ----
  ss_label(c, "SERVER ONBOARDING", x, y);
  y += 24.0f;
  if (!ccord_store_onboarding_loaded(st, ss->guild_id)) {
    ss_message(c, "Loading Server Onboarding...", x, y, w);
    y += 36.0f;
    ss_end_content(ss, y);
    return;
  }

  ss->rows = ss->ob_work.prompt_count;

  (void)ss_text(c, "Enable Server Onboarding", x, y + 3.0f, M(font_size_base), false, COL(text_normal), w - 60.0f);
  ss_toggle(c, (ccord_rect){x + w - 44.0f, y, 44.0f, 24.0f}, ss->ob_work.enabled, nullptr,
            A_OB_ENABLED, 0, "onboarding-enabled");
  y += 38.0f;

  ss_label(c, "ONBOARDING MODE", x, y);
  y += 22.0f;
  ss_chip(c, (ccord_rect){x, y, 164.0f, 26.0f}, "Default (Channels)",
          ss->ob_work.mode == CCORD_ONBOARDING_DEFAULT, A_OB_MODE, CCORD_ONBOARDING_DEFAULT, "onboarding-mode:0");
  ss_chip(c, (ccord_rect){x + 172.0f, y, 206.0f, 26.0f}, "Advanced (Channels + Prompts)",
          ss->ob_work.mode == CCORD_ONBOARDING_ADVANCED, A_OB_MODE, CCORD_ONBOARDING_ADVANCED, "onboarding-mode:1");
  y += 38.0f;

  ss_label(c, "DEFAULT CHANNELS", x, y);
  y += 22.0f;
  float cx = x;
  size_t nch = ccord_store_channel_count(st, ss->guild_id);
  for (size_t ci = 0; ci < nch; ci++) {
    const ccord_channel *ch = ccord_store_channel_at(st, ss->guild_id, ci);
    if (!ss_is_onboarding_chan(ch)) continue;
    char clbl[CCORD_NAME_MAX_BYTES + 8], nm[SS_NAME_LEN];
    (void)snprintf(clbl, sizeof clbl, "#%s", ch->name ? ch->name : "channel");
    float cw = fminf(ss_text_width(c, clbl, SS_CAP(c), false) + 22.0f, w);
    if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
    bool sel = ss_id_in_array(ss->ob_work.default_channel_ids, ss->ob_work.default_channel_count, ch->id);
    ss_chip(c, (ccord_rect){cx, y, cw, 26.0f}, clbl, sel, A_OB_DEF_CHAN, ch->id,
            NAME(nm, "onboarding-defchan:%llu", (unsigned long long)ch->id));
    cx += cw + 8.0f;
  }
  y += 38.0f;

  ss_label(c, "CUSTOMIZATION QUESTIONS", x, y);
  if (!ss->ob_editing_prompt && ss->ob_work.prompt_count < CCORD_ONBOARDING_MAX_PROMPTS) {
    ss_button(c, (ccord_rect){x + w - 122.0f, y - 4.0f, 122.0f, 28.0f}, "Add Question", BTN_SECONDARY, nullptr,
              A_OB_PROMPT_NEW, 0, "onboarding-prompt-new");
  }
  y += 30.0f;

  if (ss->ob_editing_prompt) {
    draw_community_prompt_editor(c, x, w, &y);
  }

  if (ss->ob_work.prompt_count == 0 && !ss->ob_editing_prompt) {
    ss_message(c, "No onboarding questions configured yet.", x, y, w);
    y += 34.0f;
  } else {
    for (size_t pi = 0; pi < ss->ob_work.prompt_count; pi++) {
      const ss_ob_prompt_copy *pr = &ss->ob_work.prompts[pi];
      ccord_rect prow = {x, y, w, 54.0f};
      y += 56.0f;
      if (!ss_visible(c, prow)) continue;
      uis_surface(ui, c->r, (ccord_rect){prow.x, prow.y + 1.0f, prow.w, 52.0f}, COL(bg_secondary));
      (void)ss_text(c, pr->title[0] ? pr->title : "Untitled Question", prow.x + 12.0f, prow.y + 7.0f,
                    M(font_size_base), true, COL(text_header), w - 180.0f);
      char pmeta[160];
      (void)snprintf(pmeta, sizeof pmeta, "%zu option%s  -  %s%s%s", pr->option_count,
                     pr->option_count == 1 ? "" : "s",
                     pr->type == CCORD_ONBOARDING_DROPDOWN ? "Dropdown" : "Multiple Choice",
                     pr->required ? ", Required" : "",
                     pr->single_select ? ", Single-select" : "");
      (void)ss_text(c, pmeta, prow.x + 12.0f, prow.y + 29.0f, SS_CAP(c), false,
                    COL(text_muted), w - 180.0f);
      char nm1[SS_NAME_LEN], nm2[SS_NAME_LEN];
      ss_button(c, (ccord_rect){prow.x + w - 160.0f, prow.y + 12.0f, 70.0f, 30.0f}, "Edit", BTN_SECONDARY, nullptr,
                A_OB_PROMPT_EDIT, (uint64_t)pi, NAME(nm1, "onboarding-prompt-edit:%zu", pi));
      ss_button(c, (ccord_rect){prow.x + w - 82.0f, prow.y + 12.0f, 74.0f, 30.0f}, "Remove", BTN_DANGER, nullptr,
                A_OB_PROMPT_DEL, (uint64_t)pi, NAME(nm2, "onboarding-prompt-del:%zu", pi));
    }
  }

  y += 8.0f;
  ss_button(c, (ccord_rect){x, y, 156.0f, 34.0f}, "Save Onboarding", BTN_PRIMARY, nullptr,
            A_OB_SAVE, 0, "onboarding-save");
  ss_button(c, (ccord_rect){x + 166.0f, y, 84.0f, 34.0f}, "Reset", BTN_SECONDARY, nullptr,
            A_OB_RESET, 0, "onboarding-reset");
  y += 46.0f;
  ss_end_content(ss, y);
}

