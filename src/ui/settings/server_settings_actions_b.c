// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "server_settings_internal.h"

static void ss_join_csv(const char *const *items, size_t count, char *buf, size_t cap)
{
  if (!buf || cap == 0) return;
  buf[0] = '\0';
  size_t pos = 0;
  for (size_t i = 0; items && i < count; i++) {
    if (!items[i] || items[i][0] == '\0') continue;
    if (pos > 0 && pos + 2u < cap) {
      buf[pos++] = ',';
      buf[pos++] = ' ';
      buf[pos] = '\0';
    }
    int n = snprintf(buf + pos, cap - pos, "%s", items[i]);
    if (n <= 0) break;
    size_t adv = (size_t)n;
    if (pos + adv >= cap) {
      pos = cap - 1u;
      break;
    }
    pos += adv;
  }
}

static size_t ss_split_csv(char *scratch, const char **out, size_t cap, bool *overflow)
{
  size_t count = 0;
  if (overflow) *overflow = false;
  if (!scratch || !out || cap == 0) return 0;
  char *p = scratch;
  int brace_depth = 0;
  int bracket_depth = 0;
  bool escaped = false;
  while (*p != '\0') {
    char *comma = NULL;
    for (char *scan = p; *scan != '\0'; scan++) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (*scan == '\\') {
        escaped = true;
        continue;
      }
      if (*scan == '{') brace_depth++;
      else if (*scan == '}' && brace_depth > 0) brace_depth--;
      else if (*scan == '[') bracket_depth++;
      else if (*scan == ']' && bracket_depth > 0) bracket_depth--;
      else if (*scan == ',' && brace_depth == 0 && bracket_depth == 0) {
        comma = scan;
        break;
      }
    }
    if (comma) *comma = '\0';
    while (*p == ' ' || *p == '\t') p++;
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1u] == ' ' || p[len - 1u] == '\t')) p[--len] = '\0';
    if (len > 0) {
      if (count >= cap) {
        if (overflow) *overflow = true;
        break;
      }
      out[count++] = p;
    }
    if (!comma) break;
    p = comma + 1;
  }
  return count;
}

// Discord caps each keyword/allow-list item and regex pattern by byte length; the builder would otherwise reject the
// whole rule after the editor has already shown the values as accepted.
static bool ss_automod_items_fit(const char *const *items, size_t count, size_t max_bytes)
{
  for (size_t i = 0; items && i < count; i++) {
    if (!items[i] || strlen(items[i]) > max_bytes) return false;
  }
  return true;
}

static void ss_automod_begin_new(ccord_ui *ui, uint8_t trig)
{
  struct ui_server_settings *ss = ui->ss;
  if (trig != CCORD_AUTOMOD_TRIGGER_KEYWORD && trig != CCORD_AUTOMOD_TRIGGER_SPAM &&
      trig != CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET && trig != CCORD_AUTOMOD_TRIGGER_MENTION_SPAM) {
    trig = CCORD_AUTOMOD_TRIGGER_KEYWORD;
  }
  ss->am_editing = true;
  ss->am_edit_id = CCORD_ID_NONE;
  ss->am_del_confirm_id = CCORD_ID_NONE;
  ss->am_error[0] = '\0';
  ss->am_event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND;
  ss->am_trigger = trig;
  ss->am_enabled = true;
  ss->am_presets = CCORD_AUTOMOD_PRESET_PROFANITY | CCORD_AUTOMOD_PRESET_SLURS;
  ss->am_mention_limit = 5u;
  ss->am_mention_raid = false;
  ss->am_act_block = true;
  ss->am_act_alert = false;
  ss->am_alert_chan = ss_am_default_alert_chan(ui);
  ss->am_act_timeout = false;
  ss->am_timeout_sec = 300u;
  ss->am_act_block_interaction = false;
  ss->am_exempt_role_count = 0u;
  ss->am_exempt_chan_count = 0u;
  ss_set_edit(&ss->am_name, ss_automod_trig_label(trig));
  ccord_text_edit_clear(&ss->am_keywords);
  ccord_text_edit_clear(&ss->am_regex);
  ccord_text_edit_clear(&ss->am_allow);
  ccord_text_edit_clear(&ss->am_custom_msg);
  ss->focus = SSF_AM_NAME;
  ccord_text_edit_move(&ss->am_name, 2, false);
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
}

static void ss_automod_begin_edit(ccord_ui *ui, ccord_id rule_id)
{
  struct ui_server_settings *ss = ui->ss;
  size_t n = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(ui->d.store, ss->guild_id, &n);
  const ccord_automod_rule *ru = nullptr;
  for (size_t i = 0; rules && i < n; i++) {
    if (rules[i].id == rule_id) { ru = &rules[i]; break; }
  }
  if (!ru) return;

  ss->am_editing = true;
  ss->am_edit_id = ru->id;
  ss->am_del_confirm_id = CCORD_ID_NONE;
  ss->am_error[0] = '\0';
  ss->am_event_type = ru->event_type ? ru->event_type : (uint8_t)CCORD_AUTOMOD_EVENT_MESSAGE_SEND;
  ss->am_trigger = ru->trigger_type;
  ss->am_enabled = ru->enabled;
  ss->am_presets = ru->presets ? ru->presets : CCORD_AUTOMOD_PRESET_PROFANITY;
  ss->am_mention_limit = ru->mention_total_limit ? ru->mention_total_limit : 5u;
  ss->am_mention_raid = ru->mention_raid_protection;
  ss->am_act_block = ru->action_block;
  ss->am_act_alert = ru->action_alert;
  ss->am_alert_chan = ru->alert_channel_id != CCORD_ID_NONE ? ru->alert_channel_id : ss_am_default_alert_chan(ui);
  ss->am_act_timeout = ru->action_timeout;
  ss->am_timeout_sec = ru->timeout_seconds ? ru->timeout_seconds : 300u;
  ss->am_act_block_interaction = ru->action_block_interaction;
  ss->am_exempt_role_count = ru->exempt_role_count <= CCORD_AUTOMOD_MAX_EXEMPT_ROLES
                               ? ru->exempt_role_count : CCORD_AUTOMOD_MAX_EXEMPT_ROLES;
  if (ss->am_exempt_role_count > 0) {
    memcpy(ss->am_exempt_roles, ru->exempt_role_ids, ss->am_exempt_role_count * sizeof(ccord_id));
  }
  ss->am_exempt_chan_count = ru->exempt_channel_count <= CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS
                               ? ru->exempt_channel_count : CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS;
  if (ss->am_exempt_chan_count > 0) {
    memcpy(ss->am_exempt_chans, ru->exempt_channel_ids, ss->am_exempt_chan_count * sizeof(ccord_id));
  }

  ss_set_edit(&ss->am_name, ru->name ? ru->name : "");
  // Full max-capacity lists can be many kilobytes: join into a heap buffer so opening a rule never truncates it.
  char *csv = ccord_malloc(SS_AM_CSV_MAX);
  if (csv) {
    ss_join_csv(ru->keywords, ru->keyword_count, csv, SS_AM_KEYWORD_CSV_MAX);
    ss_set_edit(&ss->am_keywords, csv);
    ss_join_csv(ru->regex_patterns, ru->regex_count, csv, SS_AM_REGEX_CSV_MAX);
    ss_set_edit(&ss->am_regex, csv);
    ss_join_csv(ru->allow_list, ru->allow_count, csv, SS_AM_ALLOW_CSV_MAX);
    ss_set_edit(&ss->am_allow, csv);
    ccord_free(csv);
  } else {
    ccord_text_edit_clear(&ss->am_keywords);
    ccord_text_edit_clear(&ss->am_regex);
    ccord_text_edit_clear(&ss->am_allow);
    (void)snprintf(ss->am_error, sizeof ss->am_error, "%s", "Not enough memory to load this rule's lists.");
  }
  ss_set_edit(&ss->am_custom_msg, ru->custom_message ? ru->custom_message : "");
  ss->focus = SSF_AM_NAME;
  ccord_text_edit_move(&ss->am_name, 2, false);
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
}

static void ss_automod_toggle_enabled(ccord_ui *ui, ccord_id rule_id)
{
  struct ui_server_settings *ss = ui->ss;
  size_t n = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(ui->d.store, ss->guild_id, &n);
  const ccord_automod_rule *ru = nullptr;
  for (size_t i = 0; rules && i < n; i++) {
    if (rules[i].id == rule_id) { ru = &rules[i]; break; }
  }
  if (!ru) return;
  ccord_automod_rule upd = *ru;
  upd.enabled = !ru->enabled;
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_edit_automod_rule(&req, ss->guild_id, ru->id, &upd, nullptr, &err),
            &req, &err, "toggle AutoMod rule");
}

static void ss_automod_delete_submit(ccord_ui *ui, ccord_id rule_id)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_delete_automod_rule(&req, ss->guild_id, rule_id, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't delete AutoMod rule: %s", err.message);
    return;
  }
  if (ss_admin_send(ui, &req)) {
    ss->am_del_confirm_id = CCORD_ID_NONE;
    if (ss->am_edit_id == rule_id) {
      ss->am_editing = false;
      ss->am_edit_id = CCORD_ID_NONE;
    }
  }
}

static void ss_automod_cycle_alert_chan(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_id cur = ss_am_default_alert_chan(ui), first = CCORD_ID_NONE, next = CCORD_ID_NONE;
  bool seen = false;
  size_t n = ccord_store_channel_count(ui->d.store, ss->guild_id);
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(ui->d.store, ss->guild_id, i);
    if (!ch || ch->kind != CCORD_CHANNEL_TEXT || ccord_discord_type_is_thread(ch->discord_type)) continue;
    if (first == CCORD_ID_NONE) first = ch->id;
    if (seen && next == CCORD_ID_NONE) next = ch->id;
    if (ch->id == cur) seen = true;
  }
  ss->am_alert_chan = next != CCORD_ID_NONE ? next : first;
}

static void ss_automod_save_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ss->am_error[0] = '\0';

  // Heap copies: the splitter writes terminators into the text, and a max-capacity list is many kilobytes.
  char *kw_buf = ccord_strdup(ccord_text_edit_cstr(&ss->am_keywords));
  char *rx_buf = ccord_strdup(ccord_text_edit_cstr(&ss->am_regex));
  char *al_buf = ccord_strdup(ccord_text_edit_cstr(&ss->am_allow));
  if (!kw_buf || !rx_buf || !al_buf) {
    ccord_free(kw_buf);
    ccord_free(rx_buf);
    ccord_free(al_buf);
    (void)snprintf(ss->am_error, sizeof ss->am_error, "%s", "Not enough memory to save this rule.");
    ui_notice(ui, "Couldn't save AutoMod rule: %s", ss->am_error);
    return;
  }

  const char *kw_ptrs[CCORD_AUTOMOD_MAX_KEYWORDS];
  const char *rx_ptrs[CCORD_AUTOMOD_MAX_REGEX];
  const char *al_ptrs[CCORD_AUTOMOD_MAX_ALLOW_LIST];

  bool kw_ovf = false, rx_ovf = false, al_ovf = false;
  size_t nkw = ss_split_csv(kw_buf, kw_ptrs, CCORD_AUTOMOD_MAX_KEYWORDS, &kw_ovf);
  size_t nrx = ss_split_csv(rx_buf, rx_ptrs, CCORD_AUTOMOD_MAX_REGEX, &rx_ovf);
  size_t nal = ss_split_csv(al_buf, al_ptrs, CCORD_AUTOMOD_MAX_ALLOW_LIST, &al_ovf);

  bool uses_keywords = ss->am_trigger == CCORD_AUTOMOD_TRIGGER_KEYWORD ||
                       ss->am_trigger == CCORD_AUTOMOD_TRIGGER_MEMBER_PROFILE;
  bool uses_allow = uses_keywords || ss->am_trigger == CCORD_AUTOMOD_TRIGGER_KEYWORD_PRESET;

  const char *problem = nullptr;
  ss_field problem_field = SSF_NONE;
  if (uses_keywords && (kw_ovf || nkw > CCORD_AUTOMOD_MAX_KEYWORDS)) {
    problem = "Too many keywords: Discord allows at most 128 per rule.";
    problem_field = SSF_AM_KEYWORDS;
  } else if (uses_keywords && !ss_automod_items_fit(kw_ptrs, nkw, CCORD_AUTOMOD_KEYWORD_MAX_BYTES)) {
    problem = "Each keyword must be 60 characters or fewer.";
    problem_field = SSF_AM_KEYWORDS;
  } else if (uses_keywords && (rx_ovf || nrx > CCORD_AUTOMOD_MAX_REGEX)) {
    problem = "Too many regex patterns: Discord allows at most 10 per rule.";
    problem_field = SSF_AM_REGEX;
  } else if (uses_keywords && !ss_automod_items_fit(rx_ptrs, nrx, CCORD_AUTOMOD_REGEX_MAX_BYTES)) {
    problem = "Each regex pattern must be 260 characters or fewer.";
    problem_field = SSF_AM_REGEX;
  } else if (uses_allow && (al_ovf || nal > CCORD_AUTOMOD_MAX_ALLOW_LIST)) {
    problem = "Too many allow-list entries: Discord allows at most 128 per rule.";
    problem_field = SSF_AM_ALLOW;
  } else if (uses_allow && !ss_automod_items_fit(al_ptrs, nal, CCORD_AUTOMOD_KEYWORD_MAX_BYTES)) {
    problem = "Each allow-list entry must be 60 characters or fewer.";
    problem_field = SSF_AM_ALLOW;
  }
  if (problem) {
    (void)snprintf(ss->am_error, sizeof ss->am_error, "%s", problem);
    ss->focus = problem_field;
    ui_notice(ui, "Couldn't save AutoMod rule: %s", problem);
    ccord_free(kw_buf);
    ccord_free(rx_buf);
    ccord_free(al_buf);
    return;
  }

  bool timeout_ok = (ss->am_trigger == CCORD_AUTOMOD_TRIGGER_KEYWORD || ss->am_trigger == CCORD_AUTOMOD_TRIGGER_MENTION_SPAM);
  ccord_id achan = ss->am_act_alert ? ss_am_default_alert_chan(ui) : CCORD_ID_NONE;

  ccord_automod_rule rule = {
    .id = ss->am_edit_id,
    .guild_id = ss->guild_id,
    .name = ccord_text_edit_cstr(&ss->am_name),
    .event_type = ss->am_event_type ? ss->am_event_type : (uint8_t)CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = ss->am_trigger,
    .enabled = ss->am_enabled,
    .keywords = nkw > 0 ? kw_ptrs : nullptr,
    .keyword_count = nkw,
    .regex_patterns = nrx > 0 ? rx_ptrs : nullptr,
    .regex_count = nrx,
    .allow_list = nal > 0 ? al_ptrs : nullptr,
    .allow_count = nal,
    .presets = ss->am_presets,
    .mention_total_limit = ss->am_mention_limit ? ss->am_mention_limit : 5u,
    .mention_raid_protection = ss->am_mention_raid,
    .action_block = ss->am_act_block,
    .custom_message = ccord_text_edit_cstr(&ss->am_custom_msg),
    .action_alert = ss->am_act_alert && achan != CCORD_ID_NONE,
    .alert_channel_id = achan,
    .action_timeout = ss->am_act_timeout && timeout_ok,
    .timeout_seconds = ss->am_timeout_sec ? ss->am_timeout_sec : 60u,
    .action_block_interaction = ss->am_act_block_interaction,
    .exempt_role_count = ss->am_exempt_role_count,
    .exempt_channel_count = ss->am_exempt_chan_count,
  };
  if (rule.exempt_role_count > 0) {
    memcpy(rule.exempt_role_ids, ss->am_exempt_roles, rule.exempt_role_count * sizeof(ccord_id));
  }
  if (rule.exempt_channel_count > 0) {
    memcpy(rule.exempt_channel_ids, ss->am_exempt_chans, rule.exempt_channel_count * sizeof(ccord_id));
  }

  ccord_admin_request req;
  ccord_error err;
  ccord_status st = (ss->am_edit_id == CCORD_ID_NONE)
                      ? ccord_admin_create_automod_rule(&req, ss->guild_id, &rule, nullptr, &err)
                      : ccord_admin_edit_automod_rule(&req, ss->guild_id, ss->am_edit_id, &rule, nullptr, &err);
  ccord_free(kw_buf);
  ccord_free(rx_buf);
  ccord_free(al_buf);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't save AutoMod rule: %s", err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ss->am_editing = false;
  ss->am_edit_id = CCORD_ID_NONE;
  ss->focus = SSF_NONE;
}

static const ccord_webhook *ss_find_webhook(const ccord_store *store, ccord_id gid, ccord_id wh_id)
{
  size_t n = 0;
  const ccord_webhook *whs = ccord_store_webhooks(store, gid, &n);
  for (size_t i = 0; whs && i < n; i++) {
    if (whs[i].id == wh_id) return &whs[i];
  }
  return nullptr;
}

static void ss_webhook_refresh_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_list_webhooks(&req, ss->guild_id, &err), &req, &err, "reload webhooks");
}

void ss_webhook_create_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_id ch_id = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->wh_new_chan, 0);
  const char *raw_name = ccord_text_edit_cstr(&ss->wh_new_name);
  const char *nm = (raw_name && raw_name[0] != '\0') ? raw_name : "Captain Hook";
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_create_webhook(&req, ss->guild_id, ch_id, nm, nullptr, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't create webhook: %s", err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ccord_text_edit_clear(&ss->wh_new_name);
  ss->focus = SSF_NONE;
}

static void ss_webhook_copy_url(ccord_ui *ui, ccord_id wh_id)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_webhook *wh = ss_find_webhook(ui->d.store, ss->guild_id, wh_id);
  if (!wh || !wh->token || wh->token[0] == '\0') {
    ui_notice(ui, "Webhook URL is not available");
    return;
  }
  char url[256];
  (void)snprintf(url, sizeof url, "https://discord.com/api/webhooks/%llu/%s",
                 (unsigned long long)wh->id, wh->token);
  ss_copy_text(ui, url, "Webhook URL copied");
}

static void ss_webhook_begin_edit(ccord_ui *ui, ccord_id wh_id)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_webhook *wh = ss_find_webhook(ui->d.store, ss->guild_id, wh_id);
  if (!wh) return;
  ss->wh_edit_id = wh->id;
  ss->wh_del_confirm_id = CCORD_ID_NONE;
  ss->wh_edit_chan = wh->channel_id;
  ss_set_edit(&ss->wh_edit_name, wh->name ? wh->name : "");
  ss->focus = SSF_WH_EDIT_NAME;
  ccord_text_edit_move(&ss->wh_edit_name, 2, false);
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
}

void ss_webhook_edit_save_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->wh_edit_id == CCORD_ID_NONE) return;
  const ccord_webhook *wh = ss_find_webhook(ui->d.store, ss->guild_id, ss->wh_edit_id);
  const char *nm = ccord_text_edit_cstr(&ss->wh_edit_name);
  ccord_id ech = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->wh_edit_chan, 0);
  ccord_admin_webhook_edit ed = {0};
  if (!wh || strcmp(nm, wh->name ? wh->name : "") != 0) ed.name = nm;
  bool can_move = !wh || wh->type == CCORD_WEBHOOK_INCOMING || wh->type == CCORD_WEBHOOK_CHANNEL_FOLLOWER;
  if (can_move && ech != CCORD_ID_NONE && (!wh || ech != wh->channel_id)) ed.channel_id = ech;
  if (!ed.name && ed.channel_id == CCORD_ID_NONE) {
    ss->wh_edit_id = CCORD_ID_NONE;
    ss->focus = SSF_NONE;
    return;
  }
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_edit_webhook(&req, ss->guild_id, ss->wh_edit_id, &ed, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't update webhook: %s", err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ss->wh_edit_id = CCORD_ID_NONE;
  ss->focus = SSF_NONE;
}

static void ss_webhook_delete_submit(ccord_ui *ui, ccord_id wh_id)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_delete_webhook(&req, ss->guild_id, wh_id, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't delete webhook: %s", err.message);
    return;
  }
  if (ss_admin_send(ui, &req)) {
    ss->wh_del_confirm_id = CCORD_ID_NONE;
    if (ss->wh_edit_id == wh_id) ss->wh_edit_id = CCORD_ID_NONE;
  }
}

static void ss_follow_channel_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_id src = ss_resolve_follow_src_chan(ui->d.store, ss->guild_id, ss->follow_src_chan);
  ccord_id dst = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->follow_dst_chan, 0);
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_follow_channel(&req, ss->guild_id, src, dst, nullptr, &err),
            &req, &err, "follow announcement channel");
}

static void ss_community_enable_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  ccord_id r_id = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_wiz_rules, 0);
  ccord_id u_id = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_wiz_updates, 1);
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_community = true;
  ge.feature_community = true;
  ge.preserve_vanity_feature = gd->feature_vanity_url;
  ge.preserve_banner_feature = gd->feature_banner;
  ge.has_rules_channel = true;
  ge.rules_channel_id = r_id;
  ge.has_public_updates_channel = true;
  ge.public_updates_channel_id = u_id;
  if (ss->comm_wiz_safety) {
    ge.explicit_content_filter = 2;
    ge.default_notifications = 1;
    if (gd->verification_level == 0) ge.verification_level = 1;
  }
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_edit_guild_ex(&req, ss->guild_id, &ge, nullptr, &err),
            &req, &err, "enable Community");
}

static void ss_community_channels_save_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_id r_id = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_work_rules, 0);
  ccord_id u_id = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_work_updates, 1);
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_rules_channel = true;
  ge.rules_channel_id = r_id;
  ge.has_public_updates_channel = true;
  ge.public_updates_channel_id = u_id;
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_edit_guild_ex(&req, ss->guild_id, &ge, nullptr, &err),
            &req, &err, "save Community channels");
}

static void ss_community_disable_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_community = true;
  ge.feature_community = false;
  ge.preserve_vanity_feature = gd->feature_vanity_url;
  ge.preserve_banner_feature = gd->feature_banner;
  ge.has_rules_channel = true;
  ge.rules_channel_id = CCORD_ID_NONE;
  ge.has_public_updates_channel = true;
  ge.public_updates_channel_id = CCORD_ID_NONE;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_edit_guild_ex(&req, ss->guild_id, &ge, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't disable Community: %s", err.message);
    return;
  }
  if (ss_admin_send(ui, &req)) ss->requested[SS_COMMUNITY] = false;
}

void ss_welcome_add_channel(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->ws_work.channel_count >= CCORD_WELCOME_MAX_CHANNELS) return;
  ccord_id cid = ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->ws_add_chan, 0);
  const char *desc = ccord_text_edit_cstr(&ss->ws_add_desc);
  if (cid == CCORD_ID_NONE) {
    ui_notice(ui, "Select a channel first.");
    return;
  }
  if (!desc || desc[0] == '\0') {
    ui_notice(ui, "Enter a short channel description first.");
    return;
  }
  ss_welcome_chan_copy *dst = &ss->ws_work.channels[ss->ws_work.channel_count++];
  *dst = (ss_welcome_chan_copy){.channel_id = cid, .emoji_id = CCORD_ID_NONE};
  (void)snprintf(dst->description, sizeof dst->description, "%s", desc);
  (void)snprintf(dst->emoji_name, sizeof dst->emoji_name, "%s", ccord_text_edit_cstr(&ss->ws_add_emoji));
  ccord_text_edit_clear(&ss->ws_add_desc);
  ccord_text_edit_clear(&ss->ws_add_emoji);
  ss->focus = SSF_NONE;
}

static void ss_welcome_save_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_welcome_channel chans[CCORD_WELCOME_MAX_CHANNELS];
  size_t n = ss->ws_work.channel_count < CCORD_WELCOME_MAX_CHANNELS
               ? ss->ws_work.channel_count
               : CCORD_WELCOME_MAX_CHANNELS;
  for (size_t i = 0; i < n; i++) {
    chans[i] = (ccord_welcome_channel){
      .channel_id = ss->ws_work.channels[i].channel_id,
      .description = ss->ws_work.channels[i].description,
      .emoji_id = ss->ws_work.channels[i].emoji_id,
      .emoji_name = ss->ws_work.channels[i].emoji_name,
    };
  }
  ccord_welcome_screen ws = {
    .enabled = ss->ws_work.enabled,
    .description = ccord_text_edit_cstr(&ss->ws_desc),
    .channels = n > 0 ? chans : nullptr,
    .channel_count = n,
  };
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_edit_welcome_screen(&req, ss->guild_id, &ws, nullptr, &err),
            &req, &err, "save Welcome Screen");
}

static void ss_ob_prompt_begin_new(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->ob_work.prompt_count >= CCORD_ONBOARDING_MAX_PROMPTS) return;
  ss->ob_editing_prompt = true;
  ss->ob_edit_prompt_idx = ss->ob_work.prompt_count;
  ss->ob_work_prompt = (ss_ob_prompt_copy){
    .type = CCORD_ONBOARDING_MULTIPLE_CHOICE,
    .in_onboarding = true,
  };
  ss->ob_new_opt_chan_count = 0;
  ss->ob_new_opt_role_count = 0;
  ccord_text_edit_clear(&ss->ob_prompt_title);
  ccord_text_edit_clear(&ss->ob_opt_title);
  ccord_text_edit_clear(&ss->ob_opt_desc);
  ss->focus = SSF_OB_PROMPT_TITLE;
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
}

static void ss_ob_prompt_begin_edit(ccord_ui *ui, size_t idx)
{
  struct ui_server_settings *ss = ui->ss;
  if (idx >= ss->ob_work.prompt_count) return;
  ss->ob_editing_prompt = true;
  ss->ob_edit_prompt_idx = idx;
  ss->ob_work_prompt = ss->ob_work.prompts[idx];
  ss->ob_new_opt_chan_count = 0;
  ss->ob_new_opt_role_count = 0;
  ss_set_edit(&ss->ob_prompt_title, ss->ob_work_prompt.title);
  ccord_text_edit_clear(&ss->ob_opt_title);
  ccord_text_edit_clear(&ss->ob_opt_desc);
  ss->focus = SSF_OB_PROMPT_TITLE;
  ccord_text_edit_move(&ss->ob_prompt_title, 2, false);
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
}

static void ss_ob_option_add(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->ob_work_prompt.option_count >= CCORD_ONBOARDING_MAX_OPTIONS) return;
  const char *otitle = ccord_text_edit_cstr(&ss->ob_opt_title);
  if (!otitle || otitle[0] == '\0') {
    ui_notice(ui, "Option label cannot be empty.");
    return;
  }
  if (ss->ob_new_opt_chan_count == 0 && ss->ob_new_opt_role_count == 0) {
    ui_notice(ui, "Select at least one channel or role for this option.");
    return;
  }
  ss_ob_opt_copy *dst = &ss->ob_work_prompt.options[ss->ob_work_prompt.option_count++];
  *dst = (ss_ob_opt_copy){0};
  (void)snprintf(dst->title, sizeof dst->title, "%s", otitle);
  (void)snprintf(dst->description, sizeof dst->description, "%s", ccord_text_edit_cstr(&ss->ob_opt_desc));
  dst->channel_count = ss->ob_new_opt_chan_count;
  if (dst->channel_count > 0) memcpy(dst->channel_ids, ss->ob_new_opt_chans, dst->channel_count * sizeof(ccord_id));
  dst->role_count = ss->ob_new_opt_role_count;
  if (dst->role_count > 0) memcpy(dst->role_ids, ss->ob_new_opt_roles, dst->role_count * sizeof(ccord_id));
  ccord_text_edit_clear(&ss->ob_opt_title);
  ccord_text_edit_clear(&ss->ob_opt_desc);
  ss->ob_new_opt_chan_count = 0;
  ss->ob_new_opt_role_count = 0;
  ss->focus = SSF_NONE;
}

static void ss_ob_prompt_save_local(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const char *ptitle = ccord_text_edit_cstr(&ss->ob_prompt_title);
  if (!ptitle || ptitle[0] == '\0') {
    ui_notice(ui, "Question prompt cannot be empty.");
    return;
  }
  if (ss->ob_work_prompt.option_count == 0) {
    ui_notice(ui, "Add at least one answer option before saving the question.");
    return;
  }
  (void)snprintf(ss->ob_work_prompt.title, sizeof ss->ob_work_prompt.title, "%s", ptitle);
  if (ss->ob_edit_prompt_idx < ss->ob_work.prompt_count) {
    ss->ob_work.prompts[ss->ob_edit_prompt_idx] = ss->ob_work_prompt;
  } else if (ss->ob_work.prompt_count < CCORD_ONBOARDING_MAX_PROMPTS) {
    ss->ob_work.prompts[ss->ob_work.prompt_count++] = ss->ob_work_prompt;
  }
  ss->ob_editing_prompt = false;
  ss->focus = SSF_NONE;
}

static void ss_onboarding_save_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  size_t np = ss->ob_work.prompt_count < CCORD_ONBOARDING_MAX_PROMPTS
                ? ss->ob_work.prompt_count
                : CCORD_ONBOARDING_MAX_PROMPTS;
  ccord_onboarding_option *opt_pool = ccord_calloc(CCORD_ONBOARDING_MAX_PROMPTS * CCORD_ONBOARDING_MAX_OPTIONS,
                                                   sizeof *opt_pool);
  if (!opt_pool) {
    ui_notice(ui, "Out of memory");
    return;
  }
  ccord_onboarding_prompt prompts[CCORD_ONBOARDING_MAX_PROMPTS] = {0};
  for (size_t pi = 0; pi < np; pi++) {
    const ss_ob_prompt_copy *sp = &ss->ob_work.prompts[pi];
    ccord_onboarding_option *popts = &opt_pool[pi * CCORD_ONBOARDING_MAX_OPTIONS];
    size_t no = sp->option_count < CCORD_ONBOARDING_MAX_OPTIONS ? sp->option_count : CCORD_ONBOARDING_MAX_OPTIONS;
    for (size_t oi = 0; oi < no; oi++) {
      const ss_ob_opt_copy *so = &sp->options[oi];
      popts[oi] = (ccord_onboarding_option){
        .id = so->id,
        .title = so->title,
        .description = so->description,
        .emoji_id = so->emoji_id,
        .emoji_name = so->emoji_name,
        .channel_count = so->channel_count,
        .role_count = so->role_count,
      };
      if (so->channel_count > 0) memcpy(popts[oi].channel_ids, so->channel_ids, so->channel_count * sizeof(ccord_id));
      if (so->role_count > 0) memcpy(popts[oi].role_ids, so->role_ids, so->role_count * sizeof(ccord_id));
    }
    prompts[pi] = (ccord_onboarding_prompt){
      .id = sp->id,
      .type = sp->type,
      .title = sp->title,
      .single_select = sp->single_select,
      .required = sp->required,
      .in_onboarding = sp->in_onboarding,
      .options = no > 0 ? popts : nullptr,
      .option_count = no,
    };
  }
  ccord_onboarding ob = {
    .guild_id = ss->guild_id,
    .enabled = ss->ob_work.enabled,
    .mode = ss->ob_work.mode,
    .default_channel_count = ss->ob_work.default_channel_count,
    .prompts = np > 0 ? prompts : nullptr,
    .prompt_count = np,
  };
  if (ob.default_channel_count > 0) {
    memcpy(ob.default_channel_ids, ss->ob_work.default_channel_ids, ob.default_channel_count * sizeof(ccord_id));
  }
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_put_onboarding(&req, ss->guild_id, &ob, nullptr, &err);
  ccord_free(opt_pool);
  ss_submit(ui, st, &req, &err, "save Server Onboarding");
}

void ss_activate(ccord_ui *ui, const ss_hit *h)
{
  struct ui_server_settings *ss = ui->ss;
  if (h->why) {
    ui_notice(ui, "%s", h->why);
    ui->dirty = true;
    return;
  }
  char idbuf[24];
  switch (h->act) {
    case A_CLOSE: ss_try_close(ui); break;
    case A_PAGE:
      ss_go_page(ui, (ss_page)h->arg);
      if (ui->mobile_layout) ui->mobile_server_settings_detail = true;
      break;
    case A_FIELD: {
      ss->focus = (ss_field)h->arg;
      ccord_text_edit *ed = ss_edit_for(ss, ss->focus);
      if (ed) ccord_text_edit_move(ed, 2, false);
      ui->caret_on = true;
      ui->caret_epoch_ms = ui->now_ms;
      break;
    }
    case A_SAVE: ss_save(ui); break;
    case A_RESET: ss_reset(ui); break;
    case A_COPY_ID:
      (void)ccord_id_format(ss->guild_id, idbuf);
      ss_copy_text(ui, idbuf, "Copied ID");
      break;
    case A_ICON_UPLOAD: ss_pick_overview_image(ui, false); break;
    case A_ICON_CLEAR: ss_clear_overview_image(ui, false); break;
    case A_BANNER_UPLOAD: ss_pick_overview_image(ui, true); break;
    case A_BANNER_CLEAR: ss_clear_overview_image(ui, true); break;
    case A_SYS_CHANNEL: ss_cycle_overview_channel(ui, false); break;
    case A_SYS_FLAG: ss->ov_work_cfg.system_channel_flags ^= ((uint32_t)h->arg & CCORD_SYSCHAN_KNOWN_MASK); break;
    case A_DEF_NOTIFY: ss->ov_work_cfg.default_notifications = (uint8_t)(h->arg ? 1u : 0u); break;
    case A_AFK_CHANNEL: ss_cycle_overview_channel(ui, true); break;
    case A_AFK_TIMEOUT: ss->ov_work_cfg.afk_timeout = ss_norm_afk_timeout((uint32_t)h->arg); break;
    case A_ROLE_SELECT:
      if (h->arg == ss->role_sel) break;
      if (ss_role_dirty(ss)) {
        ss_flash(ui, "You have unsaved changes. Save or reset them first.");
        break;
      }
      ss->role_sel = h->arg;
      ss->scroll = 0.0f;
      ccord_scroller_cancel(&ss->scroll_glide);
      ss->focus = SSF_NONE;
      break;
    case A_ROLE_CREATE:
      if (ss_role_dirty(ss)) ss_flash(ui, "You have unsaved changes. Save or reset them first.");
      else ss_create_role(ui);
      break;
    case A_ROLE_UP: ss_move_role(ui, true); break;
    case A_ROLE_DOWN: ss_move_role(ui, false); break;
    case A_ROLE_DELETE: ui_confirm_open(ui, CCORD_ADMIN_DELETE_ROLE, ss->guild_id, ss->role_sel, ss->base.name); break;
    case A_ROLE_PREVIEW:
      if (ss_dirty(ss)) {
        ss_flash(ui, "You have unsaved changes. Save or reset them first.");
      } else {
        ccord_id gid = ss->guild_id, rid = ss->role_sel;
        ui_server_settings_close(ui);
        if (ui->d.store && ccord_store_selected_guild(ui->d.store) != gid) {
          ccord_error serr;
          if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, gid, &serr);
          else (void)ccord_store_select_guild(ui->d.store, gid, &serr);
        }
        ui_start_role_preview(ui, gid, rid);
      }
      break;
    case A_TAB:
      ss->tab = (ss_tab)h->arg;
      ss->scroll = 0.0f;
      ccord_scroller_cancel(&ss->scroll_glide);
      ss->focus = SSF_NONE;
      break;
    case A_COLOR: ss->work.color = (uint32_t)h->arg; break;
    case A_HOIST: ss->work.hoist = !ss->work.hoist; break;
    case A_MENTION: ss->work.mention = !ss->work.mention; break;
    case A_PERM: ss->work.perms ^= h->arg; break;
    case A_MEMBER: ss_toggle_member(ui, h->arg); break;
    case A_VERIF_LEVEL: ss->safety_work.verification_level = (uint8_t)h->arg; break;
    case A_EXPLICIT_FILTER: ss->safety_work.explicit_filter = (uint8_t)h->arg; break;
    case A_PAUSE_INVITES: ss_set_incident_pause(ui, true, h->arg); break;
    case A_PAUSE_DMS: ss_set_incident_pause(ui, false, h->arg); break;
    case A_UNBAN: {
      ccord_admin_request req;
      ccord_error err;
      ss_submit(ui, ccord_admin_unban(&req, ss->guild_id, h->arg, nullptr, &err), &req, &err, "revoke the ban");
      break;
    }
    case A_INV_AGE: ss->inv_age_i = (size_t)h->arg; break;
    case A_INV_USES: ss->inv_uses_i = (size_t)h->arg; break;
    case A_INV_CHANNEL: ss_next_invite_channel(ui); break;
    case A_INV_CREATE: ss_create_invite(ui); break;
    case A_INV_COPY: {
      char link[96];
      (void)snprintf(link, sizeof link, "https://discord.gg/%s", ss_hit_code(h));
      ss_copy_text(ui, link, "Invite link copied");
      break;
    }
    case A_INV_REVOKE: ui_confirm_open(ui, CCORD_ADMIN_REVOKE_INVITE, ss->guild_id, CCORD_ID_NONE, ss_hit_code(h)); break;
    case A_INV_PAUSE_TOGGLE: {
      const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
      bool paused = gd && gd->invites_disabled_until_ms > ui->now_unix_ms;
      ss_set_incident_pause(ui, true, paused ? 0u : 86400u);
      break;
    }
    case A_VANITY_SAVE: ss_save_vanity_code(ui, ccord_text_edit_cstr(&ss->vanity_edit)); break;
    case A_VANITY_CLEAR:
      ss_set_edit(&ss->vanity_edit, "");
      ss_save_vanity_code(ui, "");
      break;
    case A_REFRESH: ss_request(ui, ss->page, true); break;
    case A_AUDIT_FILTER: ss->audit_filter = (ccord_audit_category)h->arg; break;
    case A_AUDIT_USER: ss_cycle_audit_user(ui); break;
    case A_AUDIT_ACTION: ss_cycle_audit_action(ui); break;
    case A_AUDIT_EXPAND: ss->audit_expanded_id = (ss->audit_expanded_id == h->arg) ? CCORD_ID_NONE : h->arg; break;
    case A_TRANSFER_MEMBER: ss_cycle_transfer_member(ui); break;
    case A_TRANSFER_OWNER: {
      ccord_id target = ss_transfer_target(ui);
      if (target != CCORD_ID_NONE) {
        ui_confirm_open(ui, CCORD_ADMIN_TRANSFER_OWNERSHIP, ss->guild_id, target,
                        ui_member_display_name(ui, ss->guild_id, target));
      }
      break;
    }
    case A_DELETE_GUILD: {
      const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
      ui_confirm_open(ui, CCORD_ADMIN_DELETE_GUILD, ss->guild_id, CCORD_ID_NONE, gd && gd->name ? gd->name : "");
      break;
    }
    case A_EMOJI_PICK: ss_pick_emoji_file(ui); break;
    case A_EMOJI_CREATE: ss_create_emoji_submit(ui); break;
    case A_EMOJI_EDIT: ss_begin_expr_edit(ui, SS_EMOJI, h->arg); break;
    case A_EMOJI_DELETE: ss_delete_expr_open(ui, SS_EMOJI, h->arg); break;
    case A_STICKER_PICK: ss_pick_sticker_file(ui); break;
    case A_STICKER_CREATE: ss_create_sticker_submit(ui); break;
    case A_STICKER_EDIT: ss_begin_expr_edit(ui, SS_STICKERS, h->arg); break;
    case A_STICKER_DELETE: ss_delete_expr_open(ui, SS_STICKERS, h->arg); break;
    case A_SOUND_PICK: ss_pick_sound_file(ui); break;
    case A_SOUND_NEW_VOL: ss->sound_new_vol_pct = (uint8_t)h->arg; break;
    case A_SOUND_CREATE: ss_create_sound_submit(ui); break;
    case A_SOUND_EDIT: ss_begin_expr_edit(ui, SS_SOUNDBOARD, h->arg); break;
    case A_SOUND_DELETE: ss_delete_expr_open(ui, SS_SOUNDBOARD, h->arg); break;
    case A_EXPR_EDIT_VOL: ss->edit_sound_vol_pct = (uint8_t)h->arg; break;
    case A_EXPR_EDIT_SAVE: ss_save_expr_edit(ui); break;
    case A_EXPR_EDIT_CANCEL:
      ss->edit_expr_id = CCORD_ID_NONE;
      ss->focus = SSF_NONE;
      break;
    case A_MEM_ROLE_FILTER: ss->member_role_filter = h->arg; ss->scroll = 0.0f; ccord_scroller_cancel(&ss->scroll_glide); break;
    case A_MEM_ROLE_PICK:
      ss->member_role_picker_uid = (ss->member_role_picker_uid == h->arg) ? CCORD_ID_NONE : h->arg;
      break;
    case A_MEM_ROLE_ADD: ss_member_role_change(ui, h->arg, ss_hit_second_id(h), true); break;
    case A_MEM_ROLE_DEL: ss_member_role_change(ui, h->arg, ss_hit_second_id(h), false); break;
    case A_MEM_TIMEOUT: ss_member_timeout_act(ui, h->arg); break;
    case A_MEM_KICK:
      ui_confirm_open(ui, CCORD_ADMIN_KICK, ss->guild_id, h->arg, ui_member_display_name(ui, ss->guild_id, h->arg));
      break;
    case A_MEM_BAN:
      ui_confirm_open(ui, CCORD_ADMIN_BAN, ss->guild_id, h->arg, ui_member_display_name(ui, ss->guild_id, h->arg));
      break;
    case A_MEM_TRANSFER:
      ui_confirm_open(ui, CCORD_ADMIN_TRANSFER_OWNERSHIP, ss->guild_id, h->arg,
                      ui_member_display_name(ui, ss->guild_id, h->arg));
      break;
    case A_MEM_MODVIEW:
      if (ss->modview_uid == h->arg) {
        ss->modview_uid = CCORD_ID_NONE;
      } else {
        ss->modview_uid = h->arg;
        ss->modview_scroll = 0.0f;
        if (ccord_perm_has(ss_self_perms(ui, ss->guild_id), CCORD_PERM_VIEW_AUDIT_LOG)) {
          ss_request(ui, SS_AUDIT, false);
        }
      }
      break;
    case A_MODVIEW_CLOSE: ss->modview_uid = CCORD_ID_NONE; break;
    case A_PRUNE_TOGGLE:
      ss->prune_open = !ss->prune_open;
      ss->prune_confirm = false;
      if (ss->prune_open) {
        ccord_prune_info pr = {0};
        bool has_pr = ccord_store_prune_info(ui->d.store, ss->guild_id, &pr);
        if (!ss_prune_matches_state(ss, has_pr, &pr) || pr.executed) ss_prune_preview_submit(ui);
      }
      break;
    case A_PRUNE_DAYS:
      ss->prune_days = (uint8_t)h->arg;
      ss->prune_confirm = false;
      ss->prune_dirty = true;
      break;
    case A_PRUNE_ROLE:
      ss_toggle_id_array(ss->prune_roles, &ss->prune_role_count, CCORD_PRUNE_MAX_ROLES, h->arg);
      ss->prune_confirm = false;
      ss->prune_dirty = true;
      break;
    case A_PRUNE_PREVIEW: ss_prune_preview_submit(ui); break;
    case A_PRUNE_EXEC: ss->prune_confirm = true; break;
    case A_PRUNE_CONFIRM: ss_prune_execute_submit(ui); break;
    case A_PRUNE_CANCEL: ss->prune_confirm = false; break;
    case A_AM_NEW: ss_automod_begin_new(ui, (uint8_t)h->arg); break;
    case A_AM_EDIT: ss_automod_begin_edit(ui, h->arg); break;
    case A_AM_TOGGLE: ss_automod_toggle_enabled(ui, h->arg); break;
    case A_AM_DELETE: ss->am_del_confirm_id = h->arg; break;
    case A_AM_DELETE_CONFIRM: ss_automod_delete_submit(ui, h->arg); break;
    case A_AM_DELETE_CANCEL: ss->am_del_confirm_id = CCORD_ID_NONE; break;
    case A_AM_TRIG:
      ss->am_trigger = (uint8_t)h->arg;
      if (ss->am_trigger != CCORD_AUTOMOD_TRIGGER_KEYWORD && ss->am_trigger != CCORD_AUTOMOD_TRIGGER_MENTION_SPAM) {
        ss->am_act_timeout = false;
      }
      break;
    case A_AM_PRESET: ss->am_presets ^= (uint32_t)h->arg; break;
    case A_AM_MENTION_LIM: ss->am_mention_limit = (uint8_t)h->arg; break;
    case A_AM_RAID: ss->am_mention_raid = !ss->am_mention_raid; break;
    case A_AM_ACT_BLOCK: ss->am_act_block = !ss->am_act_block; break;
    case A_AM_ACT_ALERT:
      ss->am_act_alert = !ss->am_act_alert;
      if (ss->am_act_alert) ss->am_alert_chan = ss_am_default_alert_chan(ui);
      break;
    case A_AM_ALERT_CHAN: ss_automod_cycle_alert_chan(ui); break;
    case A_AM_ACT_TIMEOUT: ss->am_act_timeout = !ss->am_act_timeout; break;
    case A_AM_TIMEOUT_DUR: ss->am_timeout_sec = (uint32_t)h->arg; break;
    case A_AM_EXEMPT_ROLE:
      ss_toggle_id_array(ss->am_exempt_roles, &ss->am_exempt_role_count, CCORD_AUTOMOD_MAX_EXEMPT_ROLES, h->arg);
      break;
    case A_AM_EXEMPT_CHAN:
      ss_toggle_id_array(ss->am_exempt_chans, &ss->am_exempt_chan_count, CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS, h->arg);
      break;
    case A_AM_ENABLED: ss->am_enabled = !ss->am_enabled; break;
    case A_AM_SAVE: ss_automod_save_submit(ui); break;
    case A_AM_CANCEL:
      ss->am_editing = false;
      ss->am_edit_id = CCORD_ID_NONE;
      ss->am_error[0] = '\0';
      ss->focus = SSF_NONE;
      break;
    case A_WH_REFRESH: ss_webhook_refresh_submit(ui); break;
    case A_WH_NEW_CHAN:
      ss->wh_new_chan = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                 ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->wh_new_chan, 0));
      break;
    case A_WH_CREATE: ss_webhook_create_submit(ui); break;
    case A_WH_COPY_URL: ss_webhook_copy_url(ui, h->arg); break;
    case A_WH_EDIT: ss_webhook_begin_edit(ui, h->arg); break;
    case A_WH_EDIT_CHAN:
      ss->wh_edit_chan = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                  ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->wh_edit_chan, 0));
      break;
    case A_WH_EDIT_SAVE: ss_webhook_edit_save_submit(ui); break;
    case A_WH_EDIT_CANCEL:
      ss->wh_edit_id = CCORD_ID_NONE;
      ss->focus = SSF_NONE;
      break;
    case A_WH_DELETE:
      ss->wh_del_confirm_id = h->arg;
      ss->wh_edit_id = CCORD_ID_NONE;
      break;
    case A_WH_DELETE_CONFIRM: ss_webhook_delete_submit(ui, h->arg); break;
    case A_WH_DELETE_CANCEL: ss->wh_del_confirm_id = CCORD_ID_NONE; break;
    case A_FOLLOW_SRC_CHAN:
      ss->follow_src_chan = ss_cycle_follow_src_chan(ui->d.store,
                                                     ss_resolve_follow_src_chan(ui->d.store, ss->guild_id, ss->follow_src_chan));
      break;
    case A_FOLLOW_DST_CHAN:
      ss->follow_dst_chan = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                     ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->follow_dst_chan, 0));
      break;
    case A_FOLLOW_SUBMIT: ss_follow_channel_submit(ui); break;
    case A_COMM_WIZ_RULES:
      ss->comm_wiz_rules = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                    ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_wiz_rules, 0));
      break;
    case A_COMM_WIZ_UPDATES:
      ss->comm_wiz_updates = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                      ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_wiz_updates, 1));
      break;
    case A_COMM_WIZ_SAFETY: ss->comm_wiz_safety = !ss->comm_wiz_safety; break;
    case A_COMM_ENABLE_SUBMIT: ss_community_enable_submit(ui); break;
    case A_COMM_RULES_CHAN:
      ss->comm_work_rules = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                     ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_work_rules, 0));
      break;
    case A_COMM_UPDATES_CHAN:
      ss->comm_work_updates = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                       ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->comm_work_updates, 1));
      break;
    case A_COMM_CHAN_SAVE: ss_community_channels_save_submit(ui); break;
    case A_COMM_DISABLE: ss_community_disable_submit(ui); break;
    case A_WS_ENABLED: ss->ws_work.enabled = !ss->ws_work.enabled; break;
    case A_WS_ADD_CHAN:
      ss->ws_add_chan = ss_cycle_guild_text_chan(ui->d.store, ss->guild_id,
                                                 ss_resolve_guild_text_chan(ui->d.store, ss->guild_id, ss->ws_add_chan, 0));
      break;
    case A_WS_CHAN_ADD: ss_welcome_add_channel(ui); break;
    case A_WS_CHAN_DEL: {
      size_t idx = (size_t)h->arg;
      if (idx < ss->ws_work.channel_count) {
        memmove(&ss->ws_work.channels[idx], &ss->ws_work.channels[idx + 1u],
                (ss->ws_work.channel_count - idx - 1u) * sizeof(ss_welcome_chan_copy));
        ss->ws_work.channel_count--;
      }
      break;
    }
    case A_WS_SAVE: ss_welcome_save_submit(ui); break;
    case A_WS_RESET:
      ss->ws_work = ss->ws_base;
      ss_set_edit(&ss->ws_desc, ss->ws_base.description);
      ccord_text_edit_clear(&ss->ws_add_desc);
      ccord_text_edit_clear(&ss->ws_add_emoji);
      ss->focus = SSF_NONE;
      break;
    case A_OB_ENABLED: ss->ob_work.enabled = !ss->ob_work.enabled; break;
    case A_OB_MODE: ss->ob_work.mode = (uint8_t)h->arg; break;
    case A_OB_DEF_CHAN:
      ss_toggle_id_array(ss->ob_work.default_channel_ids, &ss->ob_work.default_channel_count,
                         CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS, h->arg);
      break;
    case A_OB_PROMPT_NEW: ss_ob_prompt_begin_new(ui); break;
    case A_OB_PROMPT_EDIT: ss_ob_prompt_begin_edit(ui, (size_t)h->arg); break;
    case A_OB_PROMPT_DEL: {
      size_t idx = (size_t)h->arg;
      if (idx < ss->ob_work.prompt_count) {
        memmove(&ss->ob_work.prompts[idx], &ss->ob_work.prompts[idx + 1u],
                (ss->ob_work.prompt_count - idx - 1u) * sizeof(ss_ob_prompt_copy));
        ss->ob_work.prompt_count--;
        ss->ob_editing_prompt = false;
        ss->focus = SSF_NONE;
      }
      break;
    }
    case A_OB_PROMPT_TYPE: ss->ob_work_prompt.type = (uint8_t)h->arg; break;
    case A_OB_PROMPT_REQ: ss->ob_work_prompt.required = !ss->ob_work_prompt.required; break;
    case A_OB_PROMPT_SINGLE: ss->ob_work_prompt.single_select = !ss->ob_work_prompt.single_select; break;
    case A_OB_PROMPT_INOB: ss->ob_work_prompt.in_onboarding = !ss->ob_work_prompt.in_onboarding; break;
    case A_OB_OPT_CHAN:
      ss_toggle_id_array(ss->ob_new_opt_chans, &ss->ob_new_opt_chan_count, CCORD_ONBOARDING_MAX_OPT_CHANNELS, h->arg);
      break;
    case A_OB_OPT_ROLE:
      ss_toggle_id_array(ss->ob_new_opt_roles, &ss->ob_new_opt_role_count, CCORD_ONBOARDING_MAX_OPT_ROLES, h->arg);
      break;
    case A_OB_OPT_ADD: ss_ob_option_add(ui); break;
    case A_OB_OPT_DEL: {
      size_t idx = (size_t)h->arg;
      if (idx < ss->ob_work_prompt.option_count) {
        memmove(&ss->ob_work_prompt.options[idx], &ss->ob_work_prompt.options[idx + 1u],
                (ss->ob_work_prompt.option_count - idx - 1u) * sizeof(ss_ob_opt_copy));
        ss->ob_work_prompt.option_count--;
      }
      break;
    }
    case A_OB_PROMPT_SAVE: ss_ob_prompt_save_local(ui); break;
    case A_OB_PROMPT_CANCEL:
      ss->ob_editing_prompt = false;
      ss->focus = SSF_NONE;
      break;
    case A_OB_SAVE: ss_onboarding_save_submit(ui); break;
    case A_OB_RESET:
      ss->ob_work = ss->ob_base;
      ss->ob_editing_prompt = false;
      ss->focus = SSF_NONE;
      break;
    case A_NONE: break;
  }
  ui->dirty = true;
}

