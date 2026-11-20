// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

// ---------------------------------------------------------------- emoji picker & autocomplete

static const char *const k_picker_emojis[UI_EMOJI_PICKER_COUNT] = {
  "\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x98\x82", "\xF0\x9F\x98\xAE",
  "\xF0\x9F\x98\xA2", "\xF0\x9F\x94\xA5",         "\xF0\x9F\x8E\x89", "\xF0\x9F\x91\x80",
};

static const char *const k_emoji_shortcodes[UI_EMOJI_PICKER_COUNT] = {
  ":thumbsup:", ":heart:", ":joy:", ":open_mouth:",
  ":sob:",      ":fire:",  ":tada:", ":eyes:",
};

void ui_draw_emoji_picker(ccord_ui *ui, ccord_renderer *r)
{
  ui_expr_picker_draw(ui, r);
}

bool dc_ascii_prefix_ci(const char *str, const char *prefix, size_t plen)
{
  if (!str) return false;
  for (size_t i = 0; i < plen; i++) {
    char a = str[i], b = prefix[i];
    if (a == '\0') return false;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

static uint64_t slash_fnv1a_step(uint64_t h, const void *data, size_t len)
{
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static void format_slash_choice_insert(const char *opt_name, bool named_already, const char *val,
                                       char out[static 96])
{
  bool need_quote = false;
  for (const char *p = val; *p; p++) {
    if (*p == ' ' || *p == '\t' || *p == '"') {
      need_quote = true;
      break;
    }
  }
  if (named_already) {
    if (need_quote) snprintf(out, 96, "\"%.88s\" ", val);
    else snprintf(out, 96, "%.92s ", val);
  } else {
    if (need_quote) snprintf(out, 96, "%.32s:\"%.54s\" ", opt_name, val);
    else snprintf(out, 96, "%.32s:%.56s ", opt_name, val);
  }
}

static void ui_schedule_slash_autocomplete(ccord_ui *ui, const ccord_channel *ch, const ccord_slash_command *cmd,
                                           const slash_parsed *sp, size_t opt_idx)
{
  if (!ui || !ui->d.session || !ch || !cmd || opt_idx >= cmd->option_count) return;
  const char *focused_val = sp->opt_vals[opt_idx];
  uint64_t key = 14695981039346656037ULL;
  key = slash_fnv1a_step(key, &ch->id, sizeof ch->id);
  key = slash_fnv1a_step(key, &cmd->id, sizeof cmd->id);
  key = slash_fnv1a_step(key, &opt_idx, sizeof opt_idx);
  key = slash_fnv1a_step(key, focused_val, strlen(focused_val));

  if (ui->slash_auto_cmd_id != cmd->id || ui->slash_auto_opt_idx != opt_idx) {
    ccord_session_clear_slash_choices(ui->d.session);
  }
  if (key == ui->slash_auto_sent_key) return;

  ui->slash_auto_sent_key = key;
  ui->slash_auto_cmd_id = cmd->id;
  ui->slash_auto_opt_idx = opt_idx;
  snprintf(ui->slash_auto_cmd_name, sizeof ui->slash_auto_cmd_name, "%s", cmd->name);
  ui->slash_auto_sub_group[0] = '\0';
  ui->slash_auto_sub_cmd[0] = '\0';
  if (sp->sub_group_idx >= 0) {
    snprintf(ui->slash_auto_sub_group, sizeof ui->slash_auto_sub_group, "%s", cmd->options[sp->sub_group_idx].name);
  }
  if (sp->sub_cmd_idx >= 0) {
    snprintf(ui->slash_auto_sub_cmd, sizeof ui->slash_auto_sub_cmd, "%s", cmd->options[sp->sub_cmd_idx].name);
  }

  size_t nopts = 0;
  int32_t focused_out = 0;
  for (size_t i = 0; i < cmd->option_count && nopts < CCORD_SLASH_MAX_OPTIONS; i++) {
    if (!dc_slash_opt_in_scope(cmd, i, sp)) continue;
    if (i != opt_idx && !sp->opt_set[i]) continue;
    snprintf(ui->slash_auto_opt_names[nopts], sizeof ui->slash_auto_opt_names[nopts], "%s", cmd->options[i].name);
    ui->slash_auto_opt_kinds[nopts] = (uint8_t)cmd->options[i].kind;
    snprintf(ui->slash_auto_opt_vals[nopts], sizeof ui->slash_auto_opt_vals[nopts], "%s", sp->opt_vals[i]);
    ui->slash_auto_opt_name_ptrs[nopts] = ui->slash_auto_opt_names[nopts];
    ui->slash_auto_opt_val_ptrs[nopts] = ui->slash_auto_opt_vals[nopts];
    if (i == opt_idx) focused_out = (int32_t)nopts;
    nopts++;
  }
  ui->slash_auto_req = (ccord_interaction_request){
    .type = CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE,
    .application_id = cmd->application_id,
    .guild_id = ch->guild_id == CCORD_DM_GUILD_ID ? CCORD_ID_NONE : ch->guild_id,
    .channel_id = ch->id,
    .command_id = cmd->id,
    .command_version = cmd->version != CCORD_ID_NONE ? cmd->version : cmd->id,
    .command_name = ui->slash_auto_cmd_name,
    .option_names = ui->slash_auto_opt_name_ptrs,
    .option_kinds = ui->slash_auto_opt_kinds,
    .option_values = ui->slash_auto_opt_val_ptrs,
    .option_count = nopts,
    .focused_option = focused_out,
    .command_type = 1u,
    .subcommand_group = ui->slash_auto_sub_group[0] != '\0' ? ui->slash_auto_sub_group : nullptr,
    .subcommand = ui->slash_auto_sub_cmd[0] != '\0' ? ui->slash_auto_sub_cmd : nullptr,
  };
  uint64_t due = ui->now_ms + UI_SLASH_AUTO_DEBOUNCE_MS;
  ui->slash_auto_due_ms = due != 0 ? due : 1u;
}

static bool id_in_list(const ccord_id *ids, size_t n, ccord_id id)
{
  for (size_t i = 0; i < n; i++) {
    if (ids[i] == id) return true;
  }
  return false;
}

static bool ac_text_match(const char *s, const char *q, size_t qlen, bool prefix_only)
{
  if (!s || !s[0]) return false;
  if (qlen == 0) return true;
  return prefix_only ? dc_ascii_prefix_ci(s, q, qlen) : dc_substr_ci(s, q, qlen);
}

static void add_user_autocomplete(ccord_ui *ui, const ccord_member *m, const ccord_user *u,
                                  ccord_id seen_uids[static UI_AUTOCOMPLETE_MAX], size_t *nseen,
                                  const char *query, size_t qlen, bool prefix_only)
{
  if (!ui || !u || ui->nautocomplete >= UI_AUTOCOMPLETE_MAX) return;
  const char *ins_name = (u->name && u->name[0]) ? u->name : ((u->handle && u->handle[0]) ? u->handle : nullptr);
  if (!ins_name) return;
  if (id_in_list(seen_uids, *nseen, u->id)) return;
  bool match = (qlen == 0) || ac_text_match(u->name, query, qlen, prefix_only) ||
               ac_text_match(u->handle, query, qlen, prefix_only) ||
               (m && ac_text_match(m->nick, query, qlen, prefix_only));
  if (!match) return;
  seen_uids[(*nseen)++] = u->id;
  ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
  if (m && m->nick[0]) {
    snprintf(it->display, sizeof it->display, "@%.40s (%.40s)", m->nick, ins_name);
  } else if (u->name && u->name[0] && u->handle && u->handle[0] && strcasecmp(u->name, u->handle) != 0) {
    snprintf(it->display, sizeof it->display, "@%.40s (@%.38s)", u->name, u->handle);
  } else {
    snprintf(it->display, sizeof it->display, "@%.80s", ins_name);
  }
  snprintf(it->insert, sizeof it->insert, "@%s ", ins_name);
}

static void collect_at_autocomplete(ccord_ui *ui, const ccord_channel *ch, ccord_id gid,
                                    const char *query, size_t qlen)
{
  ccord_id seen_uids[UI_AUTOCOMPLETE_MAX] = {0};
  size_t nseen_u = 0;
  ccord_id seen_rids[UI_AUTOCOMPLETE_MAX] = {0};
  size_t nseen_r = 0;
  bool added_everyone = false, added_here = false;
  bool in_guild = gid != CCORD_ID_NONE && gid != CCORD_DM_GUILD_ID;
  bool can_everyone = in_guild && dc_can_mention_everyone_in(ui->d.store, gid, ch->id, ui->now_unix_ms);
  size_t passes = qlen > 0 ? 2u : 1u;

  for (size_t pass = 0; pass < passes && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; pass++) {
    bool prefix_only = (pass == 0);
    if (in_guild) {
      size_t mc = ccord_store_member_count(ui->d.store, gid);
      for (size_t i = 0; i < mc && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
        const ccord_member *m = ccord_store_member_at(ui->d.store, gid, i);
        const ccord_user *u = m ? ccord_store_user(ui->d.store, m->user_id) : nullptr;
        add_user_autocomplete(ui, m, u, seen_uids, &nseen_u, query, qlen, prefix_only);
      }
      size_t msg_c = ccord_store_message_count(ui->d.store, ch->id);
      for (size_t i = msg_c; i > 0 && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i--) {
        const ccord_message *msg = ccord_store_message_at(ui->d.store, ch->id, i - 1u);
        if (!msg || msg->author_id == CCORD_ID_NONE) continue;
        const ccord_member *m = ccord_store_find_member(ui->d.store, gid, msg->author_id);
        const ccord_user *u = ccord_store_user(ui->d.store, msg->author_id);
        add_user_autocomplete(ui, m, u, seen_uids, &nseen_u, query, qlen, prefix_only);
      }
    } else if (gid == CCORD_DM_GUILD_ID) {
      if (ch->user_id != CCORD_ID_NONE) {
        add_user_autocomplete(ui, nullptr, ccord_store_user(ui->d.store, ch->user_id),
                              seen_uids, &nseen_u, query, qlen, prefix_only);
      }
      for (uint16_t r = 0; r < ch->recipient_count && r < CCORD_MAX_DM_RECIPIENTS &&
                           ui->nautocomplete < UI_AUTOCOMPLETE_MAX; r++) {
        add_user_autocomplete(ui, nullptr, ccord_store_user(ui->d.store, ch->recipient_ids[r]),
                              seen_uids, &nseen_u, query, qlen, prefix_only);
      }
      size_t msg_c = ccord_store_message_count(ui->d.store, ch->id);
      for (size_t i = msg_c; i > 0 && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i--) {
        const ccord_message *msg = ccord_store_message_at(ui->d.store, ch->id, i - 1u);
        if (!msg || msg->author_id == CCORD_ID_NONE) continue;
        add_user_autocomplete(ui, nullptr, ccord_store_user(ui->d.store, msg->author_id),
                              seen_uids, &nseen_u, query, qlen, prefix_only);
      }
      ccord_id self_id = ccord_store_self_id(ui->d.store);
      if (self_id != CCORD_ID_NONE) {
        add_user_autocomplete(ui, nullptr, ccord_store_user(ui->d.store, self_id),
                              seen_uids, &nseen_u, query, qlen, prefix_only);
      }
    }
    if (in_guild) {
      size_t rc = 0;
      const ccord_role *roles = ccord_store_roles(ui->d.store, gid, &rc);
      for (size_t i = 0; i < rc && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
        const ccord_role *rl = &roles[i];
        if (rl->id == gid || !rl->name || !rl->name[0]) continue;
        if (!rl->mentionable && !can_everyone) continue;
        if (id_in_list(seen_rids, nseen_r, rl->id)) continue;
        if (!ac_text_match(rl->name, query, qlen, prefix_only)) continue;
        seen_rids[nseen_r++] = rl->id;
        ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
        snprintf(it->display, sizeof it->display, "@%.72s (role)", rl->name);
        snprintf(it->insert, sizeof it->insert, "@%s ", rl->name);
      }
      if (can_everyone) {
        if (!added_everyone && ui->nautocomplete < UI_AUTOCOMPLETE_MAX &&
            ac_text_match("everyone", query, qlen, prefix_only)) {
          added_everyone = true;
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          snprintf(it->display, sizeof it->display, "@everyone");
          snprintf(it->insert, sizeof it->insert, "@everyone ");
        }
        if (!added_here && ui->nautocomplete < UI_AUTOCOMPLETE_MAX &&
            ac_text_match("here", query, qlen, prefix_only)) {
          added_here = true;
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          snprintf(it->display, sizeof it->display, "@here");
          snprintf(it->insert, sizeof it->insert, "@here ");
        }
      }
    }
  }
}

void ui_update_autocomplete(ccord_ui *ui)
{
  ui->autocomplete_open = false;
  ui->nautocomplete = 0;
  if (!ui || !ui->d.store || ui->settings_open || ui->menu.open || ui->edit.len == 0 || ui->edit.caret == 0) {
    if (ui) ui->slash_auto_due_ms = 0;
    return;
  }

  const char *text = ccord_text_edit_cstr(&ui->edit);
  size_t caret = ui->edit.caret;
  if (caret > ui->edit.len) caret = ui->edit.len;
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch || !ui_channel_has_chat(ch)) {
    ui->slash_auto_due_ms = 0;
    return;
  }
  ccord_id gid = ch->guild_id != CCORD_ID_NONE ? ch->guild_id : ccord_store_selected_guild(ui->d.store);

  // Slash command & option picker when composer starts with '/'
  if (text[0] == '/' && ui->editing_message == CCORD_ID_NONE && ui->staged_file_count == 0 &&
      ui->reply_message == CCORD_ID_NONE && ch && ch->kind == CCORD_CHANNEL_TEXT &&
      memchr(text, '\n', caret) == nullptr) {
    if (ui->d.session) (void)ccord_session_ensure_slash_commands(ui->d.session, ch->guild_id, ch->id, ui->now_ms);
    ui->built_slash_store_rev = ui->d.store ? ccord_store_revision(ui->d.store) : 0;
    size_t nchoices_init = 0;
    uint64_t seq_init = 0;
    if (ui->d.session) (void)ccord_session_slash_choices(ui->d.session, &nchoices_init, &seq_init);
    ui->built_slash_choice_seq = seq_init;

    size_t sp_pos = 1u;
    while (sp_pos < caret && text[sp_pos] != ' ' && text[sp_pos] != '\t') sp_pos++;
    if (sp_pos == caret) {
      ui->slash_auto_due_ms = 0;
      const char *q = text + 1u;
      size_t qlen = caret - 1u;
      size_t ncmds = 0;
      const ccord_slash_command *cmds = ui->d.store ? ccord_store_slash_commands(ui->d.store, ch->guild_id, &ncmds) : nullptr;
      for (size_t i = 0; i < ncmds && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
        if (!dc_slash_cmd_allowed(ui, ch, &cmds[i])) continue;
        if (qlen > 0 && !dc_ascii_prefix_ci(cmds[i].name, q, qlen)) continue;
        ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
        if (cmds[i].description[0] != '\0') {
          snprintf(it->display, sizeof it->display, "/%.32s \xE2\x80\x94 %.54s", cmds[i].name, cmds[i].description);
        } else {
          snprintf(it->display, sizeof it->display, "/%.32s", cmds[i].name);
        }
        snprintf(it->insert, sizeof it->insert, "/%.88s ", cmds[i].name);
      }
      // /tts is a built-in composer command (not an application command): always offered.
      if ((qlen == 0 || dc_ascii_prefix_ci("tts", q, qlen)) && ui->nautocomplete < UI_AUTOCOMPLETE_MAX) {
        ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
        snprintf(it->display, sizeof it->display, "/tts \xE2\x80\x94 Send a text-to-speech message");
        snprintf(it->insert, sizeof it->insert, "/tts ");
      }
      // Plugin slash commands
      if (ui->d.contrib) {
        size_t npc = ccord_contrib_command_count(ui->d.contrib);
        for (size_t i = 0; i < npc && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
          const ccord_command_contribution *pc = ccord_contrib_command_at(ui->d.contrib, i);
          if (!pc || !pc->name || !pc->name[0]) continue;
          if (qlen > 0 && !dc_ascii_prefix_ci(pc->name, q, qlen)) continue;
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          if (pc->description && pc->description[0]) {
            snprintf(it->display, sizeof it->display, "/%.32s \xE2\x80\x94 %.54s", pc->name, pc->description);
          } else {
            snprintf(it->display, sizeof it->display, "/%.32s (plugin)", pc->name);
          }
          snprintf(it->insert, sizeof it->insert, "/%.88s ", pc->name);
        }
      }
      if (ui->nautocomplete > 0) {
        ui->autocomplete_open = true;
        ui->autocomplete_trigger = '/';
        ui->autocomplete_start = 0;
        if (ui->autocomplete_sel >= ui->nautocomplete) ui->autocomplete_sel = 0;
      }
      return;
    }

    const ccord_slash_command *cmd = dc_find_slash_cmd(ui, ch->guild_id, text + 1u, sp_pos - 1u);
    if (cmd && dc_slash_cmd_allowed(ui, ch, cmd)) {
      slash_parsed sp;
      dc_slash_parse_args(cmd, text, caret, &sp);
      size_t rep_start = sp.token_start;
      bool named_already = !sp.completing_opt_name;

      // 0. Subcommand tree: offer the group / subcommand names while one is being picked
      if (sp.selecting_sub) {
        size_t plen = strlen(sp.cur_prefix);
        for (size_t i = 0; i < cmd->option_count && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
          const ccord_slash_option *o = &cmd->options[i];
          bool want;
          if (sp.sub_group_idx < 0) {
            want = o->parent_plus_one == 0 &&
                   (o->kind == CCORD_SLASH_OPT_SUB_COMMAND || o->kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP);
          } else {
            want = o->parent_plus_one == (uint8_t)(sp.sub_group_idx + 1) && o->kind == CCORD_SLASH_OPT_SUB_COMMAND;
          }
          if (!want) continue;
          if (plen > 0 && !dc_ascii_prefix_ci(o->name, sp.cur_prefix, plen)) continue;
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          if (o->description[0] != '\0') {
            snprintf(it->display, sizeof it->display, "%.32s \xE2\x80\x94 %.54s", o->name, o->description);
          } else {
            snprintf(it->display, sizeof it->display, "%.32s", o->name);
          }
          snprintf(it->insert, sizeof it->insert, "%.88s ", o->name);
        }
      }

      // 1. Offer matching option names when typing an unquoted token or after space
      if (sp.completing_opt_name && !sp.selecting_sub) {
        size_t plen = strlen(sp.cur_prefix);
        for (size_t i = 0; i < cmd->option_count && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
          if (!dc_slash_opt_in_scope(cmd, i, &sp)) continue;
          if (sp.opt_set[i] && (int)i != sp.focused_opt) continue;
          if (plen > 0 && !dc_ascii_prefix_ci(cmd->options[i].name, sp.cur_prefix, plen)) continue;
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          if (cmd->options[i].description[0] != '\0') {
            snprintf(it->display, sizeof it->display, "%.32s:%s \xE2\x80\x94 %.52s",
                     cmd->options[i].name, cmd->options[i].required ? "*" : "", cmd->options[i].description);
          } else {
            snprintf(it->display, sizeof it->display, "%.32s:%s",
                     cmd->options[i].name, cmd->options[i].required ? "*" : "");
          }
          snprintf(it->insert, sizeof it->insert, "%.88s:", cmd->options[i].name);
        }
      }

      // 2. Offer option values / dynamic autocomplete for the focused option
      if (sp.focused_opt >= 0 && (size_t)sp.focused_opt < cmd->option_count) {
        size_t oidx = (size_t)sp.focused_opt;
        const ccord_slash_option *opt = &cmd->options[oidx];
        const char *vq = sp.opt_vals[oidx];
        size_t vqlen = strlen(vq);
        if (opt->autocomplete) {
          ui_schedule_slash_autocomplete(ui, ch, cmd, &sp, oidx);
          size_t nch = 0;
          uint64_t cseq = 0;
          const ccord_slash_choice *choices = ui->d.session ? ccord_session_slash_choices(ui->d.session, &nch, &cseq) : nullptr;
          ui->built_slash_choice_seq = cseq;
          if (choices && ui->slash_auto_cmd_id == cmd->id && ui->slash_auto_opt_idx == oidx) {
            for (size_t c = 0; c < nch && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; c++) {
              ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
              snprintf(it->display, sizeof it->display, "%.58s (%.32s)", choices[c].name, opt->name);
              format_slash_choice_insert(opt->name, named_already, choices[c].value, it->insert);
            }
          }
        } else {
          ui->slash_auto_due_ms = 0;
          if (opt->kind == CCORD_SLASH_OPT_BOOLEAN) {
            const char *const bvals[2] = {"true", "false"};
            for (size_t b = 0; b < 2u && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; b++) {
              if (vqlen > 0 && !dc_ascii_prefix_ci(bvals[b], vq, vqlen)) continue;
              ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
              snprintf(it->display, sizeof it->display, "%s (%.32s)", bvals[b], opt->name);
              format_slash_choice_insert(opt->name, named_already, bvals[b], it->insert);
            }
          } else if (opt->kind == CCORD_SLASH_OPT_CHANNEL && gid != CCORD_ID_NONE && gid != CCORD_DM_GUILD_ID) {
            const char *cq = (vq[0] == '#') ? vq + 1u : vq;
            size_t cqlen = strlen(cq);
            size_t cc = ccord_store_channel_count(ui->d.store, gid);
            for (size_t c = 0; c < cc && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; c++) {
              const ccord_channel * cand = ccord_store_channel_at(ui->d.store, gid, c);
              if (!cand || cand->kind == CCORD_CHANNEL_CATEGORY || !cand->name || !ui_channel_visible(ui, gid, cand)) continue;
              if (cqlen > 0 && !dc_ascii_prefix_ci(cand->name, cq, cqlen)) continue;
              ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
              snprintf(it->display, sizeof it->display, "#%.54s (%.32s)", cand->name, opt->name);
              char hname[96];
              snprintf(hname, sizeof hname, "#%.90s", cand->name);
              format_slash_choice_insert(opt->name, named_already, hname, it->insert);
            }
          } else if (opt->kind == CCORD_SLASH_OPT_ROLE && gid != CCORD_ID_NONE && gid != CCORD_DM_GUILD_ID) {
            const char *rq = (vq[0] == '@') ? vq + 1u : vq;
            size_t rqlen = strlen(rq);
            size_t rc = 0;
            const ccord_role *roles = ccord_store_roles(ui->d.store, gid, &rc);
            for (size_t r = 0; r < rc && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; r++) {
              if (roles[r].id == gid || !roles[r].name || !roles[r].name[0]) continue;
              if (rqlen > 0 && !dc_ascii_prefix_ci(roles[r].name, rq, rqlen)) continue;
              ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
              snprintf(it->display, sizeof it->display, "@%.54s (%.32s)", roles[r].name, opt->name);
              char rname[96];
              snprintf(rname, sizeof rname, "@%.90s", roles[r].name);
              format_slash_choice_insert(opt->name, named_already, rname, it->insert);
            }
          }
        }
      } else {
        ui->slash_auto_due_ms = 0;
      }

      if (ui->nautocomplete > 0) {
        ui->autocomplete_open = true;
        ui->autocomplete_trigger = '/';
        ui->autocomplete_start = rep_start;
        if (ui->autocomplete_sel >= ui->nautocomplete) ui->autocomplete_sel = 0;
      }
      return;
    }
    // Dynamic autocomplete for plugin slash commands
    if (ui->d.contrib && sp_pos > 1u && sp_pos < caret) {
      char pname[64];
      size_t pn = sp_pos - 1u;
      if (pn >= sizeof pname) pn = sizeof pname - 1u;
      memcpy(pname, text + 1u, pn);
      pname[pn] = '\0';
      const ccord_command_contribution *pc = ccord_contrib_find_command(ui->d.contrib, pname);
      if (pc && pc->has_autocomplete) {
        size_t arg_start = sp_pos;
        while (arg_start < caret && (text[arg_start] == ' ' || text[arg_start] == '\t')) arg_start++;
        char args_buf[256];
        size_t alen = caret > arg_start ? caret - arg_start : 0u;
        if (alen >= sizeof args_buf) alen = sizeof args_buf - 1u;
        memcpy(args_buf, text + arg_start, alen);
        args_buf[alen] = '\0';
        ccord_autocomplete_item items[UI_AUTOCOMPLETE_MAX];
        size_t got = ccord_contrib_complete_command(ui->d.contrib, pc->name, args_buf, ch->guild_id, ch->id,
                                                    items, UI_AUTOCOMPLETE_MAX);
        for (size_t i = 0; i < got && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          if (items[i].detail[0]) {
            snprintf(it->display, sizeof it->display, "%.44s \xE2\x80\x94 %.44s", items[i].label, items[i].detail);
          } else {
            snprintf(it->display, sizeof it->display, "%.88s", items[i].label);
          }
          snprintf(it->insert, sizeof it->insert, "%.95s",
                   items[i].insert_text[0] ? items[i].insert_text : items[i].label);
        }
        if (ui->nautocomplete > 0) {
          ui->autocomplete_open = true;
          ui->autocomplete_trigger = '/';
          ui->autocomplete_start = arg_start;
          if (ui->autocomplete_sel >= ui->nautocomplete) ui->autocomplete_sel = 0;
        }
      }
    }
    ui->slash_auto_due_ms = 0;
    return;
  }
  ui->slash_auto_due_ms = 0;

  // Custom plugin autocomplete providers (e.g. "::", "$", "!s")
  if (ui->d.contrib && ccord_contrib_autocomplete_count(ui->d.contrib) > 0) {
    size_t wstart = caret;
    while (wstart > 0 && text[wstart - 1] != ' ' && text[wstart - 1] != '\n' && text[wstart - 1] != '\t') wstart--;
    size_t wlen = caret - wstart;
    size_t nprov = ccord_contrib_autocomplete_count(ui->d.contrib);
    for (size_t pi = 0; pi < nprov; pi++) {
      const ccord_autocomplete_provider *prov = ccord_contrib_autocomplete_at(ui->d.contrib, pi);
      if (!prov || !prov->trigger || !prov->trigger[0]) continue;
      size_t tlen = strlen(prov->trigger);
      if (wlen < tlen || strncmp(text + wstart, prov->trigger, tlen) != 0) continue;
      char qbuf[128];
      size_t qn = wlen - tlen;
      if (qn >= sizeof qbuf) qn = sizeof qbuf - 1u;
      memcpy(qbuf, text + wstart + tlen, qn);
      qbuf[qn] = '\0';
      ccord_autocomplete_item items[UI_AUTOCOMPLETE_MAX];
      size_t got = ccord_contrib_query_autocomplete(ui->d.contrib, prov->owner, prov->provider_id, qbuf,
                                                    gid, ch->id, items, UI_AUTOCOMPLETE_MAX);
      for (size_t i = 0; i < got && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
        ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
        if (items[i].detail[0]) {
          snprintf(it->display, sizeof it->display, "%.44s \xE2\x80\x94 %.44s", items[i].label, items[i].detail);
        } else {
          snprintf(it->display, sizeof it->display, "%.88s", items[i].label);
        }
        snprintf(it->insert, sizeof it->insert, "%.95s",
                 items[i].insert_text[0] ? items[i].insert_text : items[i].label);
      }
      if (ui->nautocomplete > 0) {
        ui->autocomplete_open = true;
        ui->autocomplete_trigger = prov->trigger[0];
        ui->autocomplete_start = wstart;
        if (ui->autocomplete_sel >= ui->nautocomplete) ui->autocomplete_sel = 0;
        return;
      }
    }
  }

  size_t start = caret;
  while (start > 0) {
    char c = text[start - 1];
    if (c == ' ' || c == '\n' || c == '\t') break;
    start--;
    if (c == '@' || c == '#' || c == ':') break;
  }
  char trig = text[start];
  if (trig != '@' && trig != '#' && trig != ':') return;
  if (start > 0) {
    if (trig == '@') {
      if (!dc_has_at_mention_boundary_before(text, start)) return;
    } else if (text[start - 1] != ' ' && text[start - 1] != '\n' && text[start - 1] != '\t') {
      return;
    }
  }

  const char *query = text + start + 1;
  size_t qlen = caret - (start + 1);
  if (qlen > 32) return;
  for (size_t i = 0; i < qlen; i++) {
    if (query[i] == ' ' || query[i] == '\n') return;
  }

  if (trig == '@') {
    collect_at_autocomplete(ui, ch, gid, query, qlen);
  } else if (trig == '#') {
    size_t cc = ccord_store_channel_count(ui->d.store, gid);
    for (size_t i = 0; i < cc && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
      const ccord_channel *c = ccord_store_channel_at(ui->d.store, gid, i);
      if (!c || c->kind != CCORD_CHANNEL_TEXT || !c->name || !ui_channel_visible(ui, gid, c)) continue;
      if (qlen > 0 && !dc_ascii_prefix_ci(c->name, query, qlen)) continue;
      ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
      snprintf(it->display, sizeof it->display, "#%s", c->name);
      snprintf(it->insert, sizeof it->insert, "#%s ", c->name);
    }
  } else if (trig == ':') {
    if (qlen == 0) return;
    if (qlen == 4 && strncasecmp(query, "fire", 4) == 0) {
      ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
      snprintf(it->display, sizeof it->display, "\xF0\x9F\x94\xA5 :fire:");
      snprintf(it->insert, sizeof it->insert, "\xF0\x9F\x94\xA5 ");
    } else {
      // 1. Guild custom emojis (selected guild first, then all guilds)
      if (ui->d.store) {
        size_t ec = ccord_store_guild_emoji_count(ui->d.store, gid);
        for (size_t i = 0; i < ec && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
          const ccord_emoji *ce = ccord_store_guild_emoji_at(ui->d.store, gid, i);
          if (!ce || !ce->name || !ce->name[0] || !dc_ascii_prefix_ci(ce->name, query, qlen)) continue;
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          snprintf(it->display, sizeof it->display, ":%s:", ce->name);
          snprintf(it->insert, sizeof it->insert, "<%s:%s:%llu> ",
                   ce->animated ? "a" : "", ce->name, (unsigned long long)ce->id);
        }
        size_t ng = ccord_store_guild_count(ui->d.store);
        for (size_t g = 0; g < ng && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; g++) {
          const ccord_guild *gd = ccord_store_guild_at(ui->d.store, g);
          if (!gd || gd->id == gid) continue;
          size_t gec = ccord_store_guild_emoji_count(ui->d.store, gd->id);
          for (size_t i = 0; i < gec && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
            const ccord_emoji *ce = ccord_store_guild_emoji_at(ui->d.store, gd->id, i);
            if (!ce || !ce->name || !ce->name[0] || !dc_ascii_prefix_ci(ce->name, query, qlen)) continue;
            ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
            snprintf(it->display, sizeof it->display, ":%s: (%s)", ce->name, gd->name ? gd->name : "");
            snprintf(it->insert, sizeof it->insert, "<%s:%s:%llu> ",
                     ce->animated ? "a" : "", ce->name, (unsigned long long)ce->id);
          }
        }
      }
      // 2. Common shortcodes + full Unicode emoji dataset
      for (size_t i = 0; i < UI_EMOJI_PICKER_COUNT && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
        const char *sc = k_emoji_shortcodes[i] + 1;
        if (!dc_ascii_prefix_ci(sc, query, qlen)) continue;
        ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
        snprintf(it->display, sizeof it->display, "%s %s", k_picker_emojis[i], k_emoji_shortcodes[i]);
        snprintf(it->insert, sizeof it->insert, "%s ", k_picker_emojis[i]);
      }
      if (ui->nautocomplete < UI_AUTOCOMPLETE_MAX) {
        size_t match_idx[UI_AUTOCOMPLETE_MAX];
        size_t nm = ccord_emoji_complete(query, qlen, match_idx, UI_AUTOCOMPLETE_MAX);
        if (nm == 0) nm = ccord_emoji_search(query, qlen, match_idx, UI_AUTOCOMPLETE_MAX);
        for (size_t i = 0; i < nm && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
          const ccord_emoji_entry *e = ccord_emoji_at(match_idx[i]);
          if (!e || !e->utf8 || !e->name) continue;
          bool dup = false;
          for (size_t k = 0; k < ui->nautocomplete; k++) {
            if (strncmp(ui->autocomplete_items[k].insert, e->utf8, strlen(e->utf8)) == 0) {
              dup = true;
              break;
            }
          }
          if (dup) continue;
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          snprintf(it->display, sizeof it->display, "%s :%s:", e->utf8, e->name);
          snprintf(it->insert, sizeof it->insert, "%s ", e->utf8);
        }
      }
    }
  }

  if (ui->nautocomplete > 0) {
    ui->autocomplete_open = true;
    ui->autocomplete_trigger = trig;
    ui->autocomplete_start = start;
    if (ui->autocomplete_sel >= ui->nautocomplete) ui->autocomplete_sel = 0;
  }
}

bool ui_accept_autocomplete(ccord_ui *ui, size_t index)
{
  if (!ui || !ui->autocomplete_open || index >= ui->nautocomplete) return false;
  char trig = ui->autocomplete_trigger;
  const char *ins = ui->autocomplete_items[index].insert;
  size_t ilen = strlen(ins);
  size_t caret = ui->edit.caret;
  if (caret > ui->edit.len) caret = ui->edit.len;
  size_t start = ui->autocomplete_start;
  if (start > caret) start = caret;

  char buf[CCORD_SEND_MAX_BYTES + 1];
  size_t tail_len = ui->edit.len - caret;
  if (start + ilen + tail_len >= sizeof buf) return false;
  memcpy(buf, ui->edit.data, start);
  memcpy(buf + start, ins, ilen);
  memcpy(buf + start + ilen, ui->edit.data + caret, tail_len);
  buf[start + ilen + tail_len] = '\0';
  ccord_error ierr;
  ccord_text_edit_clear(&ui->edit);
  (void)ccord_text_edit_insert(&ui->edit, buf, start + ilen + tail_len, &ierr);
  ccord_text_edit_set_caret(&ui->edit, start + ilen, false);
  ui->autocomplete_open = false;
  ui->nautocomplete = 0;
  if (trig == '/') ui_update_autocomplete(ui);
  ui->dirty = true;
  return true;
}

void ui_draw_autocomplete(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui->autocomplete_open || ui->nautocomplete == 0) {
    ui->autocomplete_rect = (ccord_rect){0, 0, 0, 0};
    return;
  }
  ccord_rect cbox = ui_composer_box(ui);
  float row_h = 26.0f;
  float h = (float)ui->nautocomplete * row_h + 12.0f;
  float max_w = ui->autocomplete_trigger == '/' ? 380.0f : 280.0f;
  float w = clampf(cbox.w, 180.0f, max_w);
  ccord_rect pop = {cbox.x, fmaxf(cbox.y - h - 6.0f, 6.0f), w, h};
  ui->autocomplete_rect = pop;
  uis_popover_frame(ui, r, pop);
  ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_normal));
  for (size_t i = 0; i < ui->nautocomplete; i++) {
    ccord_rect rr = {pop.x + 6.0f, pop.y + 6.0f + (float)i * row_h, pop.w - 12.0f, row_h};
    bool hover = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, rr);
    uis_list_row_fill(ui, r, rr, (uis_state){.selected = i == ui->autocomplete_sel, .hover = hover},
                      "autocrow", (uint64_t)i);
    ccord_text_draw_ellipsis(r, ui->autocomplete_items[i].display, strlen(ui->autocomplete_items[i].display),
                             rr.x + 8.0f, rr.y + (rr.h - ui_line_h(ui, st.size)) * 0.5f, rr.w - 16.0f, &st);
  }
}

