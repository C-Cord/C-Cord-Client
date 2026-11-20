// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "mock_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

ccord_status mock_request_slash_commands(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                                ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_SLASH_INDEX]++;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;

  mock_item it = mk_make_item(CCORD_NET_SLASH_COMMANDS, mk_now_of(m) + m->cfg.latency_ms);
  it.slash_own = ccord_calloc(7, sizeof *it.slash_own);
  if (!it.slash_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");

  ccord_id app_id = m->users[10].id; // Lua Bot (1011)
  size_t count = 0;
  it.slash_own[count++] = (ccord_slash_command){
    .id = 9101, .application_id = app_id, .guild_id = 0, .version = 9101, .command_type = 1,
    .name = "ping", .description = "Check latency with an ephemeral response",
    .default_member_permissions = UINT64_MAX, .dm_permission = true, .option_count = 0,
  };
  it.slash_own[count++] = (ccord_slash_command){
    .id = 9102, .application_id = app_id, .guild_id = 0, .version = 9102, .command_type = 1,
    .name = "echo", .description = "Repeat text publicly or ephemerally",
    .default_member_permissions = UINT64_MAX, .dm_permission = true, .option_count = 2,
    .options = {
      {.kind = CCORD_SLASH_OPT_STRING, .name = "text", .description = "Message text to echo", .required = true},
      {.kind = CCORD_SLASH_OPT_BOOLEAN, .name = "ephemeral", .description = "Only show to you", .required = false},
    },
  };
  it.slash_own[count++] = (ccord_slash_command){
    .id = 9103, .application_id = app_id, .guild_id = 0, .version = 9103, .command_type = 1,
    .name = "topic", .description = "Look up an engineering topic",
    .default_member_permissions = UINT64_MAX, .dm_permission = true, .option_count = 1,
    .options = {
      {.kind = CCORD_SLASH_OPT_STRING, .name = "subject", .description = "Topic name", .required = true, .autocomplete = true},
    },
  };
  if (guild_id != CCORD_DM_GUILD_ID && guild_id != CCORD_ID_NONE) {
    it.slash_own[count++] = (ccord_slash_command){
      .id = 9104, .application_id = app_id, .guild_id = guild_id, .version = 9104, .command_type = 1,
      .name = "purge", .description = "Delete recent messages (moderators only)",
      .default_member_permissions = CCORD_PERM_MANAGE_MESSAGES, .dm_permission = false, .option_count = 1,
      .options = {
        {.kind = CCORD_SLASH_OPT_INTEGER, .name = "count", .description = "How many messages", .required = true},
      },
    };
  }
  it.slash_own[count++] = (ccord_slash_command){
    .id = 9105, .application_id = app_id, .guild_id = 0, .version = 9105, .command_type = 1,
    .name = "config", .description = "Inspect or change bot configuration",
    .default_member_permissions = UINT64_MAX, .dm_permission = true, .option_count = 5,
    .options = {
      {.kind = CCORD_SLASH_OPT_SUB_COMMAND, .name = "show", .description = "Show current settings", .parent_plus_one = 0},
      {.kind = CCORD_SLASH_OPT_SUB_COMMAND_GROUP, .name = "theme", .description = "Manage theme settings", .parent_plus_one = 0},
      {.kind = CCORD_SLASH_OPT_SUB_COMMAND, .name = "set", .description = "Set active theme preset", .parent_plus_one = 2},
      {.kind = CCORD_SLASH_OPT_STRING, .name = "preset", .description = "Theme preset name", .required = true, .parent_plus_one = 3},
      {.kind = CCORD_SLASH_OPT_SUB_COMMAND, .name = "reset", .description = "Reset theme to default", .parent_plus_one = 2},
    },
  };
  it.slash_own[count++] = (ccord_slash_command){
    .id = 9106, .application_id = app_id, .guild_id = 0, .version = 9106, .command_type = 2,
    .name = "UserInfo", .description = "",
    .default_member_permissions = UINT64_MAX, .dm_permission = true, .option_count = 0,
  };
  it.slash_own[count++] = (ccord_slash_command){
    .id = 9107, .application_id = app_id, .guild_id = 0, .version = 9107, .command_type = 3,
    .name = "QuoteMessage", .description = "",
    .default_member_permissions = UINT64_MAX, .dm_permission = true, .option_count = 0,
  };

  it.ev.guild_id = guild_id;
  it.ev.channel_id = channel_id;
  it.ev.slash_commands = it.slash_own;
  it.ev.slash_command_count = count;
  return mk_push_item(m, &it, err);
}


void mk_clear_last_interaction_values(mock *m)
{
  for (size_t i = 0; i < m->last_interaction_nvalues; i++) {
    ccord_free(m->last_interaction_values[i]);
    m->last_interaction_values[i] = nullptr;
  }
  m->last_interaction_nvalues = 0;
}

ccord_status ccord_mock_push_message_request(ccord_transport *t, ccord_id sender_id,
                                             const char *initial_message, bool is_spam,
                                             ccord_id *out_channel_id, ccord_error *err)
{
  if (out_channel_id) *out_channel_id = CCORD_ID_NONE;
  ccord_error_clear(err);
  if (!t) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (sender_id == CCORD_ID_NONE || sender_id == m->self_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid message request sender");
  }
  const ccord_user *sender = nullptr;
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == sender_id) {
      sender = &m->users[i];
      break;
    }
  }
  if (!sender) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown sender");

  ccord_id dm_cid = 900000u + sender_id;
  size_t ci = mk_admin_channel_index(m, dm_cid);
  if (ci == SIZE_MAX) {
    ccord_channel dmc = {
      .id = dm_cid,
      .guild_id = CCORD_DM_GUILD_ID,
      .parent_id = CCORD_ID_NONE,
      .kind = CCORD_CHANNEL_TEXT,
      .discord_type = CCORD_DISCORD_CHANNEL_DM,
      .name = sender->name,
      .topic = "Direct Message",
      .user_id = sender_id,
      .recipient_count = 1u,
      .recipient_ids = {sender_id},
      .is_message_request = true,
      .is_spam = is_spam,
      .icon_url = sender->avatar_url ? sender->avatar_url : "",
    };
    ci = mk_admin_add_channel(m, &dmc, err);
    if (ci == SIZE_MAX) return CCORD_ERR_NOMEM;
  } else {
    m->channels[ci].is_message_request = true;
    m->channels[ci].is_spam = is_spam;
  }
  if (out_channel_id) *out_channel_id = dm_cid;

  ccord_status st = mk_admin_emit_channel_op(m, ci, CCORD_ADMIN_NONE, err);
  if (st != CCORD_OK) return st;

  if (initial_message && initial_message[0] != '\0') {
    size_t len = strlen(initial_message);
    if (len > CCORD_SEND_MAX_BYTES || !ccord_utf8_valid_text(initial_message, len)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid initial message");
    }
    ccord_message msg = mk_store_new(m, ci, sender_id, initial_message, len, err, &st);
    if (st != CCORD_OK) return st;
    m->channels[ci].last_message_id = msg.id;
    st = mk_queue_message_create(m, msg, mk_now_of(m) + m->cfg.latency_ms, err);
    if (st != CCORD_OK) return st;
  }
  return CCORD_OK;
}


ccord_status mock_vote_poll(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                   const uint32_t *answer_ids, size_t answer_count, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (answer_count > CCORD_RICH_MAX_POLL_ANSWERS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many poll answers");
  }
  if (answer_count > 0 && !answer_ids) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null poll answers");
  }
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = mk_find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  ccord_message *msg = &m->chmsgs[ci][mi];
  if (!msg->rich || !msg->rich->poll.present) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message has no poll");
  }
  const ccord_poll *oldp = &msg->rich->poll;
  if (oldp->finalized || (oldp->expiry_ms > 0 && mk_message_time(m) >= oldp->expiry_ms)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: poll is closed");
  }
  if (!oldp->multiselect && answer_count > 1) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: poll allows one answer");
  }
  bool want[CCORD_RICH_MAX_POLL_ANSWERS] = {0};
  for (size_t i = 0; i < answer_count; i++) {
    bool found = false;
    for (size_t k = 0; k < oldp->answer_count; k++) {
      if (oldp->answers[k].id == answer_ids[i]) {
        if (want[k]) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: duplicate poll answer");
        want[k] = true;
        found = true;
        break;
      }
    }
    if (!found) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: unknown poll answer");
  }
  if (m->cfg.refuse_writes) {
    return mk_queue_action_refused(m, CCORD_ACTION_POLL_VOTE, channel_id, message_id, "", 0, nullptr, 0, err);
  }

  ccord_message_rich *nr = ccord_message_rich_clone(msg->rich, err);
  if (!nr) return CCORD_ERR_NOMEM;
  uint64_t due = mk_now_of(m) + m->cfg.latency_ms;
  ccord_id gid = m->channels[ci].guild_id;

  // Emit removals first, then additions, matching Discord's poll vote gateway sequence.
  for (size_t k = 0; k < nr->poll.answer_count; k++) {
    ccord_poll_answer *pa = &nr->poll.answers[k];
    if (pa->me_voted && !want[k]) {
      pa->me_voted = false;
      if (pa->votes > 0) pa->votes--;
      mock_item it = mk_make_item(CCORD_NET_POLL_VOTE, due);
      it.ev.guild_id = gid;
      it.ev.channel_id = channel_id;
      it.ev.message_id = message_id;
      it.ev.user_id = m->self_id;
      it.ev.poll_answer_id = pa->id;
      it.ev.poll_vote_added = false;
      st = mk_push_item(m, &it, err);
      if (st != CCORD_OK) {
        ccord_message_rich_free(nr);
        return st;
      }
    }
  }
  for (size_t k = 0; k < nr->poll.answer_count; k++) {
    ccord_poll_answer *pa = &nr->poll.answers[k];
    if (!pa->me_voted && want[k]) {
      pa->me_voted = true;
      pa->votes++;
      mock_item it = mk_make_item(CCORD_NET_POLL_VOTE, due);
      it.ev.guild_id = gid;
      it.ev.channel_id = channel_id;
      it.ev.message_id = message_id;
      it.ev.user_id = m->self_id;
      it.ev.poll_answer_id = pa->id;
      it.ev.poll_vote_added = true;
      st = mk_push_item(m, &it, err);
      if (st != CCORD_OK) {
        ccord_message_rich_free(nr);
        return st;
      }
    }
  }
  mk_retire_rich(m, msg->rich);
  msg->rich = nr;
  return CCORD_OK;
}

ccord_status mock_send_interaction(ccord_transport *t, const ccord_interaction_request *req, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!req || req->application_id == CCORD_ID_NONE || req->channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid interaction");
  }
  ccord_status st;
  size_t ci = mk_channel_index(m, req->channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;

  const char *ident = nullptr;
  char ident_buf[CCORD_RICH_CUSTOM_ID_MAX_BYTES + 1];
  bool has_group = false;
  bool has_sub = false;
  const char *grp_name = "";
  const char *sub_name = "";
  long target_mi = -1;
  if (req->type == CCORD_INTERACTION_MESSAGE_COMPONENT) {
    if (!req->custom_id || req->custom_id[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid custom_id");
    }
    size_t idlen = strlen(req->custom_id);
    if (idlen > CCORD_RICH_CUSTOM_ID_MAX_BYTES || !ccord_utf8_valid_text(req->custom_id, idlen)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid custom_id");
    }
    if (req->message_id == CCORD_ID_NONE || mk_find_message(m, ci, req->message_id) < 0) {
      return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
    }
    if (req->component_type < CCORD_COMP_BUTTON || req->component_type > CCORD_COMP_CHANNEL_SELECT ||
        req->component_type == CCORD_COMP_TEXT_INPUT) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid component_type");
    }
    if (req->value_count > CCORD_RICH_MAX_SELECT_OPTIONS || (req->value_count > 0 && !req->values)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid select values");
    }
    ident = req->custom_id;
  } else if (req->type == CCORD_INTERACTION_MODAL_SUBMIT) {
    if (!req->custom_id || req->custom_id[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid custom_id");
    }
    size_t idlen = strlen(req->custom_id);
    if (idlen > CCORD_RICH_CUSTOM_ID_MAX_BYTES || !ccord_utf8_valid_text(req->custom_id, idlen)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid custom_id");
    }
    if (req->field_count == 0 || req->field_count > CCORD_RICH_MAX_COMPONENT_ROWS ||
        !req->field_ids || !req->field_values) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid modal fields");
    }
    ident = req->custom_id;
  } else if (req->type == CCORD_INTERACTION_APPLICATION_COMMAND ||
             req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE) {
    if (req->command_id == CCORD_ID_NONE || !req->command_name || req->command_name[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid slash command");
    }
    if (req->command_type > 3u) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid command_type");
    }
    size_t nlen = strlen(req->command_name);
    if (nlen > CCORD_SLASH_NAME_MAX_BYTES || !ccord_utf8_valid_text(req->command_name, nlen)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid command_name");
    }
    has_group = req->subcommand_group && req->subcommand_group[0] != '\0';
    has_sub = req->subcommand && req->subcommand[0] != '\0';
    if (has_group) grp_name = req->subcommand_group;
    if (has_sub) sub_name = req->subcommand;

    if (req->command_type == 2u || req->command_type == 3u) {
      if (req->type != CCORD_INTERACTION_APPLICATION_COMMAND ||
          req->target_id == CCORD_ID_NONE || req->option_count > 0 || has_group || has_sub) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid context-menu command");
      }
      if (req->command_type == 2u) {
        if (mock_user_name(m, req->target_id)[0] == '\0') {
          return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown target user");
        }
      } else {
        target_mi = mk_find_message(m, ci, req->target_id);
        if (target_mi < 0) {
          return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown target message");
        }
      }
      ident = req->command_name;
    } else {
      if (req->target_id != CCORD_ID_NONE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "mock: unexpected target_id on chat command");
      }
      for (size_t i = 0; i < nlen; i++) {
        char c = req->command_name[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
          return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid command_name");
        }
      }
      if (has_group && !has_sub) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "mock: subcommand_group without subcommand");
      }
      if (has_group) {
        size_t glen = strlen(grp_name);
        if (glen > CCORD_SLASH_NAME_MAX_BYTES || !ccord_utf8_valid_text(grp_name, glen)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid subcommand_group");
        }
        for (size_t i = 0; i < glen; i++) {
          char c = grp_name[i];
          if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
            return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid subcommand_group");
          }
        }
      }
      if (has_sub) {
        size_t slen = strlen(sub_name);
        if (slen > CCORD_SLASH_NAME_MAX_BYTES || !ccord_utf8_valid_text(sub_name, slen)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid subcommand");
        }
        for (size_t i = 0; i < slen; i++) {
          char c = sub_name[i];
          if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
            return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid subcommand");
          }
        }
      }
      if (req->option_count > CCORD_SLASH_MAX_OPTIONS ||
          (req->option_count > 0 && (!req->option_names || !req->option_kinds || !req->option_values))) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid slash options");
      }
      for (size_t i = 0; i < req->option_count; i++) {
        const char *on = req->option_names[i];
        if (!on || on[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid option name");
        size_t olen = strlen(on);
        if (olen > CCORD_SLASH_NAME_MAX_BYTES || !ccord_utf8_valid_text(on, olen)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid option name");
        }
        uint8_t ok = req->option_kinds[i];
        if (ok < CCORD_SLASH_OPT_STRING || ok > CCORD_SLASH_OPT_NUMBER) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid option kind");
        }
        const char *ov = req->option_values[i] ? req->option_values[i] : "";
        size_t ovlen = strlen(ov);
        if (ovlen > CCORD_SEND_MAX_BYTES || !ccord_utf8_valid_text(ov, ovlen)) {
          return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid option value");
        }
      }
      if (req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE &&
          (req->option_count == 0 || req->focused_option < 0 || (size_t)req->focused_option >= req->option_count)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid focused_option");
      }
      if (has_group) {
        (void)snprintf(ident_buf, sizeof ident_buf, "%s %s %s", req->command_name, grp_name, sub_name);
        ident = ident_buf;
      } else if (has_sub) {
        (void)snprintf(ident_buf, sizeof ident_buf, "%s %s", req->command_name, sub_name);
        ident = ident_buf;
      } else {
        ident = req->command_name;
      }
    }
  } else {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: unsupported interaction type");
  }

  if (m->cfg.refuse_writes && req->type != CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE) {
    return mk_queue_action_refused(m, CCORD_ACTION_INTERACTION, req->channel_id, req->message_id, "", 0, nullptr, 0, err);
  }

  mk_clear_last_interaction_values(m);
  m->interaction_count++;
  m->last_interaction_type = (uint8_t)req->type;
  m->last_interaction_comp_type = req->component_type;
  (void)snprintf(m->last_interaction_custom_id, sizeof m->last_interaction_custom_id, "%s", ident);
  if (req->type == CCORD_INTERACTION_MESSAGE_COMPONENT) {
    for (size_t i = 0; i < req->value_count; i++) {
      const char *v = req->values[i] ? req->values[i] : "";
      m->last_interaction_values[i] = ccord_strndup(v, strlen(v));
      m->last_interaction_nvalues = i + 1u;
    }
    if (strncmp(req->custom_id, "modal:", 6) == 0) {
      mock_item it = mk_make_item(CCORD_NET_INTERACTION_MODAL, mk_now_of(m) + m->cfg.latency_ms);
      it.modal_own = ccord_calloc(1, sizeof *it.modal_own);
      if (!it.modal_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      ccord_interaction_modal *md = it.modal_own;
      md->id = m->next_msg_id++;
      md->application_id = req->application_id;
      md->guild_id = req->guild_id;
      md->channel_id = req->channel_id;
      const char *mcid = req->custom_id[6] ? req->custom_id + 6 : "modal_submit";
      snprintf(md->custom_id, sizeof md->custom_id, "%s", mcid);
      snprintf(md->title, sizeof md->title, "Feedback Form");
      md->field_count = 2;
      snprintf(md->fields[0].custom_id, sizeof md->fields[0].custom_id, "subject");
      snprintf(md->fields[0].label, sizeof md->fields[0].label, "Subject");
      snprintf(md->fields[0].placeholder, sizeof md->fields[0].placeholder, "Short summary");
      md->fields[0].style = 1;
      md->fields[0].min_length = 2;
      md->fields[0].max_length = 80;
      md->fields[0].required = true;
      snprintf(md->fields[1].custom_id, sizeof md->fields[1].custom_id, "details");
      snprintf(md->fields[1].label, sizeof md->fields[1].label, "Details");
      snprintf(md->fields[1].placeholder, sizeof md->fields[1].placeholder, "Optional notes");
      md->fields[1].style = 2;
      md->fields[1].max_length = 500;
      md->fields[1].required = false;
      it.ev.modal = md;
      it.ev.guild_id = md->guild_id;
      it.ev.channel_id = md->channel_id;
      return mk_push_item(m, &it, err);
    }
  } else if (req->type == CCORD_INTERACTION_MODAL_SUBMIT) {
    for (size_t i = 0; i < req->field_count; i++) {
      const char *v = req->field_values[i] ? req->field_values[i] : "";
      m->last_interaction_values[i] = ccord_strndup(v, strlen(v));
      m->last_interaction_nvalues = i + 1u;
    }
    char reply[256];
    const char *v0 = (req->field_count > 0 && req->field_values[0]) ? req->field_values[0] : "";
    int rn = snprintf(reply, sizeof reply, "Modal submitted: %s", v0);
    if (rn > 0) {
      size_t rlen = (size_t)rn < sizeof reply ? (size_t)rn : sizeof reply - 1u;
      ccord_message msg = mk_store_new(m, ci, req->application_id, reply, rlen, err, &st);
      if (st != CCORD_OK) return st;
      return mk_queue_message_create(m, msg, mk_now_of(m) + m->cfg.latency_ms, err);
    }
  } else if (req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE) {
    for (size_t i = 0; i < req->option_count && i < CCORD_RICH_MAX_SELECT_OPTIONS; i++) {
      const char *v = req->option_values[i] ? req->option_values[i] : "";
      m->last_interaction_values[i] = ccord_strndup(v, strlen(v));
      m->last_interaction_nvalues = i + 1u;
    }
    const char *q = req->option_values[req->focused_option] ? req->option_values[req->focused_option] : "";
    size_t qlen = strlen(q);
    static const struct { const char *name; const char *value; } k_topics[4] = {
      {"C23 Language & Atomics", "c23"},
      {"SDL3 GPU & Windowing", "sdl3"},
      {"Vulkan Render Pipelines", "vulkan"},
      {"Gateway & REST Networking", "networking"},
    };
    mock_item it = mk_make_item(CCORD_NET_SLASH_AUTOCOMPLETE, mk_now_of(m) + m->cfg.latency_ms);
    it.choices_own = ccord_calloc(4, sizeof *it.choices_own);
    if (!it.choices_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    size_t out_n = 0;
    for (size_t i = 0; i < 4; i++) {
      if (qlen == 0 || mk_has_ci_substr(k_topics[i].value, q, qlen) || mk_has_ci_substr(k_topics[i].name, q, qlen)) {
        (void)snprintf(it.choices_own[out_n].name, sizeof it.choices_own[out_n].name, "%s", k_topics[i].name);
        (void)snprintf(it.choices_own[out_n].value, sizeof it.choices_own[out_n].value, "%s", k_topics[i].value);
        out_n++;
      }
    }
    it.ev.guild_id = req->guild_id;
    it.ev.channel_id = req->channel_id;
    it.ev.slash_choices = it.choices_own;
    it.ev.slash_choice_count = out_n;
    it.ev.interaction_nonce = "mock-nonce";
    return mk_push_item(m, &it, err);
  } else if (req->command_type == 2u || req->command_type == 3u) {
    char tbuf[32];
    int tn = snprintf(tbuf, sizeof tbuf, "%llu", (unsigned long long)req->target_id);
    if (tn > 0) {
      m->last_interaction_values[0] = ccord_strndup(tbuf, (size_t)tn);
      m->last_interaction_nvalues = 1u;
    }
    char reply[256];
    int rn = 0;
    bool ephemeral = false;
    if (req->command_type == 2u) {
      const char *uname = mock_user_name(m, req->target_id);
      ephemeral = true;
      rn = snprintf(reply, sizeof reply, "User Info: %s (%llu)", uname, (unsigned long long)req->target_id);
    } else {
      const ccord_message *tmsg = &m->chmsgs[ci][(size_t)target_mi];
      const char *author = mock_user_name(m, tmsg->author_id);
      const char *body = tmsg->content ? tmsg->content : "";
      rn = snprintf(reply, sizeof reply, "Quote (%s): %.160s", author[0] ? author : "user", body);
    }
    if (rn > 0) {
      size_t rlen = (size_t)rn < sizeof reply ? (size_t)rn : sizeof reply - 1u;
      ccord_message_rich r = {
        .present = CCORD_RICH_HAS_TYPE,
        .type = 23,
        .flags = ephemeral ? CCORD_MSGFLAG_EPHEMERAL : 0u,
      };
      ccord_message msg = mk_store_new_rich(m, ci, req->application_id, reply, rlen, &r, err, &st);
      if (st != CCORD_OK) return st;
      return mk_queue_message_create(m, msg, mk_now_of(m) + m->cfg.latency_ms, err);
    }
  } else {
    for (size_t i = 0; i < req->option_count && i < CCORD_RICH_MAX_SELECT_OPTIONS; i++) {
      const char *v = req->option_values[i] ? req->option_values[i] : "";
      m->last_interaction_values[i] = ccord_strndup(v, strlen(v));
      m->last_interaction_nvalues = i + 1u;
    }
    bool ephemeral = strcmp(req->command_name, "ping") == 0;
    const char *text_val = "";
    for (size_t i = 0; i < req->option_count; i++) {
      const char *on = req->option_names[i] ? req->option_names[i] : "";
      const char *ov = req->option_values[i] ? req->option_values[i] : "";
      if (strcmp(on, "ephemeral") == 0 && (strcmp(ov, "true") == 0 || strcmp(ov, "1") == 0)) ephemeral = true;
      if (i == 0 || strcmp(on, "text") == 0 || strcmp(on, "subject") == 0 || strcmp(on, "preset") == 0) text_val = ov;
    }
    char reply[256];
    int rn = 0;
    if (has_sub) {
      if (strcmp(req->command_name, "config") == 0 && !has_group && strcmp(sub_name, "show") == 0) {
        ephemeral = true;
        rn = snprintf(reply, sizeof reply, "Config: theme=dark, compact=false");
      } else if (strcmp(req->command_name, "config") == 0 && has_group &&
                 strcmp(grp_name, "theme") == 0 && strcmp(sub_name, "set") == 0) {
        rn = snprintf(reply, sizeof reply, "Config theme set: %s", text_val);
      } else if (strcmp(req->command_name, "config") == 0 && has_group &&
                 strcmp(grp_name, "theme") == 0 && strcmp(sub_name, "reset") == 0) {
        rn = snprintf(reply, sizeof reply, "Config theme reset to default");
      } else if (has_group) {
        rn = text_val[0]
                 ? snprintf(reply, sizeof reply, "Command /%s %s %s: %s", req->command_name, grp_name, sub_name, text_val)
                 : snprintf(reply, sizeof reply, "Command /%s %s %s", req->command_name, grp_name, sub_name);
      } else {
        rn = text_val[0]
                 ? snprintf(reply, sizeof reply, "Command /%s %s: %s", req->command_name, sub_name, text_val)
                 : snprintf(reply, sizeof reply, "Command /%s %s", req->command_name, sub_name);
      }
    } else if (strcmp(req->command_name, "ping") == 0) {
      rn = snprintf(reply, sizeof reply, "Pong! 12 ms");
    } else if (strcmp(req->command_name, "echo") == 0) {
      rn = snprintf(reply, sizeof reply, "Echo: %s", text_val);
    } else {
      rn = snprintf(reply, sizeof reply, "Command /%s: %s", req->command_name, text_val);
    }
    if (rn > 0) {
      size_t rlen = (size_t)rn < sizeof reply ? (size_t)rn : sizeof reply - 1u;
      ccord_message_rich r = {
        .present = CCORD_RICH_HAS_TYPE,
        .type = 20,
        .flags = ephemeral ? CCORD_MSGFLAG_EPHEMERAL : 0u,
      };
      ccord_message msg = mk_store_new_rich(m, ci, req->application_id, reply, rlen, &r, err, &st);
      if (st != CCORD_OK) return st;
      return mk_queue_message_create(m, msg, mk_now_of(m) + m->cfg.latency_ms, err);
    }
  }
  return CCORD_OK;
}

size_t ccord_mock_interaction_count(const ccord_transport *t)
{
  return t ? ((const mock *)t)->interaction_count : 0;
}

uint8_t ccord_mock_last_interaction_type(const ccord_transport *t)
{
  return t ? ((const mock *)t)->last_interaction_type : 0;
}

uint8_t ccord_mock_last_interaction_component_type(const ccord_transport *t)
{
  return t ? ((const mock *)t)->last_interaction_comp_type : 0;
}

const char *ccord_mock_last_interaction_custom_id(const ccord_transport *t)
{
  if (!t) return nullptr;
  const mock *m = (const mock *)t;
  return m->interaction_count ? m->last_interaction_custom_id : nullptr;
}

size_t ccord_mock_last_interaction_value_count(const ccord_transport *t)
{
  return t ? ((const mock *)t)->last_interaction_nvalues : 0;
}

const char *ccord_mock_last_interaction_value(const ccord_transport *t, size_t index)
{
  if (!t) return nullptr;
  const mock *m = (const mock *)t;
  return index < m->last_interaction_nvalues ? m->last_interaction_values[index] : nullptr;
}


// ---- Report flow: canned menus (shape of GET /reporting/menu/{type}), validated submit, recorded request -------------
#define MOCK_MENU_TAIL_SPAM \
  "\"2\":{\"id\":2,\"key\":\"SPAM\",\"header\":\"What kind of spam?\",\"children\":[],\"button\":{\"type\":\"next\",\"target\":4}," \
  "\"elements\":[{\"name\":\"spam_kind\",\"type\":\"checkbox\",\"should_submit_data\":true," \
  "\"data\":[[\"phishing\",\"Phishing or scams\"],[\"bot\",\"A spam bot\"]]}],\"is_multi_select_required\":true},"
#define MOCK_MENU_DONE_FAIL \
  "\"90\":{\"id\":90,\"key\":\"SUCCESS\",\"header\":\"Thanks for reporting\",\"children\":[],\"button\":{\"type\":\"done\",\"target\":null}," \
  "\"elements\":[{\"name\":\"success\",\"type\":\"success\",\"data\":null},{\"name\":\"block_users\",\"type\":\"block_users\",\"data\":null}]}," \
  "\"91\":{\"id\":91,\"key\":\"FAIL\",\"header\":\"Something went wrong\",\"children\":[],\"button\":{\"type\":\"cancel\",\"target\":null}," \
  "\"elements\":[{\"name\":\"fail\",\"type\":\"fail\",\"data\":null}]}}}"
#define MOCK_MENU_MESSAGE(EXTRA_CHILD) \
  "{\"name\":\"message\",\"variant\":\"3\",\"version\":\"1.0\",\"language\":\"en\",\"postback_url\":\"/api/reporting/message\"," \
  "\"root_node_id\":1,\"success_node_id\":90,\"fail_node_id\":91,\"nodes\":{" \
  "\"1\":{\"id\":1,\"key\":\"START\",\"header\":\"What's wrong with this message?\",\"children\":[[\"It's spam\",2],[\"It's abusive\",3]," \
  "[\"Something else\",5]" EXTRA_CHILD "],\"button\":null,\"elements\":[]}," \
  MOCK_MENU_TAIL_SPAM \
  "\"3\":{\"id\":3,\"key\":\"ABUSE\",\"header\":\"How is it abusive?\",\"children\":[[\"Harassment\",4]],\"button\":null,\"elements\":[]}," \
  "\"4\":{\"id\":4,\"key\":\"SUMMARY\",\"header\":\"Report summary\",\"children\":[],\"button\":{\"type\":\"submit\",\"target\":null}," \
  "\"report_type\":\"message\",\"elements\":[{\"name\":\"breadcrumbs\",\"type\":\"breadcrumbs\",\"data\":null}," \
  "{\"name\":\"message_preview\",\"type\":\"message_preview\",\"data\":null}," \
  "{\"name\":\"details\",\"type\":\"free_text\",\"should_submit_data\":true,\"data\":{\"placeholder\":\"Anything else?\",\"character_limit\":200}}]}," \
  "\"5\":{\"id\":5,\"key\":\"OTHER\",\"header\":\"Tell us more\",\"children\":[],\"button\":{\"type\":\"submit\",\"target\":null}," \
  "\"report_type\":\"message\",\"elements\":[{\"name\":\"details\",\"type\":\"free_text\",\"should_submit_data\":true,\"data\":{\"rows\":4}}]}," \
  MOCK_MENU_DONE_FAIL
static const char k_mock_report_menu_message[] = MOCK_MENU_MESSAGE("");
static const char k_mock_report_menu_message_broken[] = MOCK_MENU_MESSAGE(",[\"Broken link\",999]");
static const char k_mock_report_menu_user[] =
  "{\"name\":\"user\",\"variant\":\"3\",\"version\":\"1.0\",\"language\":\"en\",\"postback_url\":\"/api/reporting/user\"," \
  "\"root_node_id\":10,\"success_node_id\":90,\"fail_node_id\":91,\"nodes\":{" \
  "\"10\":{\"id\":10,\"key\":\"START\",\"header\":\"What's wrong with this user?\",\"children\":[[\"Impersonation\",11],[\"Harassment\",11]]," \
  "\"button\":null,\"elements\":[]}," \
  "\"11\":{\"id\":11,\"key\":\"SUMMARY\",\"header\":\"Report summary\",\"children\":[],\"button\":{\"type\":\"submit\",\"target\":null}," \
  "\"report_type\":\"user\",\"elements\":[{\"name\":\"breadcrumbs\",\"type\":\"breadcrumbs\",\"data\":null}," \
  "{\"name\":\"user_preview\",\"type\":\"user_preview\",\"data\":null}," \
  "{\"name\":\"details\",\"type\":\"free_text\",\"should_submit_data\":true,\"data\":{\"character_limit\":200}}]}," \
  MOCK_MENU_DONE_FAIL;

typedef struct mock_report {
  ccord_arena arena;           // owns every string and array of req
  ccord_report_request req;
} mock_report;

void mock_report_free(mock_report *r)
{
  if (!r) return;
  ccord_arena_free(&r->arena);
  ccord_free(r);
}

static const char *mock_report_menu_json(const mock *m, ccord_report_type type)
{
  if (type == CCORD_REPORT_USER) return k_mock_report_menu_user;
  return m->report_broken_link ? k_mock_report_menu_message_broken : k_mock_report_menu_message;
}

ccord_status mock_request_report_menu(ccord_transport *t, ccord_report_type type, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (type >= CCORD_REPORT_TYPE_COUNT) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad report type");
  m->report_menu_calls++;
  if (m->report_menu_fail[0]) {
    mock_item it = mk_make_item(CCORD_NET_ACTION_FAILED, mk_now_of(m) + m->cfg.latency_ms);
    it.ev.action = CCORD_ACTION_REPORT_MENU;
    it.ev.status = CCORD_ERR_NOT_FOUND;
    char *why = ccord_strndup(m->report_menu_fail, strlen(m->report_menu_fail));   // a copy: the hook may change before the poll
    if (!why) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    mk_retire_content(m, why);
    it.ev.text = why;
    return mk_push_item(m, &it, err);
  }
  const char *json = mock_report_menu_json(m, type);
  char *copy = ccord_strndup(json, strlen(json));
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  mk_retire_content(m, copy);
  mock_item it = mk_make_item(CCORD_NET_REPORT_MENU, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.status = CCORD_OK;
  it.ev.expr_arg = ccord_report_type_name(type);
  it.ev.expr_body = copy;
  it.ev.expr_body_len = strlen(copy);
  return mk_push_item(m, &it, err);
}

static bool report_is_edge(const ccord_report_node *n, uint32_t to)
{
  for (size_t i = 0; i < n->child_count; i++)
    if (n->children[i].target == to) return true;
  return n->has_button_target && n->button_target == to;
}

static bool report_value_ok(const ccord_report_element *el, const char *v)
{
  if (el->kind != CCORD_REPORT_EL_CHECKBOX && el->kind != CCORD_REPORT_EL_DROPDOWN) return v != nullptr;
  for (size_t i = 0; v && i < el->choice_count; i++)
    if (strcmp(el->choices[i].value, v) == 0) return true;
  return false;
}

// What the server would check: the path follows the menu edges to a submit node, and every value belongs to a submitting element on it.
static bool report_request_valid(const ccord_report_menu *menu, const ccord_report_request *r)
{
  if (!r->breadcrumbs || r->breadcrumb_count == 0 || r->breadcrumb_count > CCORD_REPORT_MAX_PATH) return false;
  if (r->breadcrumbs[0] != menu->root) return false;
  for (size_t i = 0; i < r->breadcrumb_count; i++) {
    const ccord_report_node *n = ccord_report_menu_node(menu, r->breadcrumbs[i]);
    if (!n) return false;
    if (i > 0 && !report_is_edge(ccord_report_menu_node(menu, r->breadcrumbs[i - 1]), r->breadcrumbs[i])) return false;
  }
  if (ccord_report_menu_node(menu, r->breadcrumbs[r->breadcrumb_count - 1])->button != CCORD_REPORT_BTN_SUBMIT) return false;
  if (r->element_count && !r->elements) return false;
  for (size_t e = 0; e < r->element_count; e++) {
    const ccord_report_value *v = &r->elements[e];
    if (!v->name || (v->value_count && !v->values)) return false;
    const ccord_report_element *found = nullptr;
    for (size_t i = 0; i < r->breadcrumb_count && !found; i++) {
      const ccord_report_node *n = ccord_report_menu_node(menu, r->breadcrumbs[i]);
      for (size_t k = 0; k < n->element_count; k++)
        if (n->elements[k].should_submit && n->elements[k].name && strcmp(n->elements[k].name, v->name) == 0) {
          found = &n->elements[k];
          break;
        }
    }
    if (!found) return false;
    for (size_t k = 0; k < v->value_count; k++)
      if (!report_value_ok(found, v->values[k])) return false;
  }
  return true;
}

static mock_report *mock_report_copy(const ccord_report_request *src)
{
  mock_report *r = ccord_calloc(1, sizeof *r);
  if (!r) return nullptr;
  ccord_arena_init(&r->arena, 0);
  ccord_arena *a = &r->arena;
  bool ok = true;
#define MR_STR(s) ((s) ? (ok = ok && (tmp = ccord_arena_strndup(a, (s), strlen(s))) != nullptr, tmp) : nullptr)
  char *tmp = nullptr;
  r->req = *src;
  r->req.version = MR_STR(src->version);
  r->req.variant = MR_STR(src->variant);
  r->req.language = MR_STR(src->language);
  uint32_t *bc = ccord_arena_alloc(a, src->breadcrumb_count * sizeof *bc + 1, alignof(uint32_t));
  ccord_report_value *els = ccord_arena_alloc(a, src->element_count * sizeof *els + 1, alignof(ccord_report_value));
  ok = ok && bc && els;
  if (ok) {
    memcpy(bc, src->breadcrumbs, src->breadcrumb_count * sizeof *bc);
    r->req.breadcrumbs = bc;
    for (size_t i = 0; ok && i < src->element_count; i++) {
      els[i] = src->elements[i];
      els[i].name = MR_STR(src->elements[i].name);
      const char **vals = ccord_arena_alloc(a, src->elements[i].value_count * sizeof *vals + 1, alignof(char *));
      ok = ok && vals;
      for (size_t k = 0; ok && k < src->elements[i].value_count; k++) vals[k] = MR_STR(src->elements[i].values[k]);
      els[i].values = vals;
    }
    r->req.elements = els;
  }
#undef MR_STR
  if (!ok) {
    mock_report_free(r);
    return nullptr;
  }
  return r;
}

ccord_status mock_submit_report(ccord_transport *t, const ccord_report_request *req, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!req || req->type >= CCORD_REPORT_TYPE_COUNT || !req->version || !req->variant)
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad report request");
  if (req->type == CCORD_REPORT_MESSAGE ? (!req->channel_id || !req->message_id) : !req->reported_user_id)
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: report target missing");
  const char *json = mock_report_menu_json(m, req->type);
  ccord_report_menu menu;
  ccord_status st = ccord_report_menu_parse(&menu, json, strlen(json), err);
  if (st != CCORD_OK) return st;
  bool valid = report_request_valid(&menu, req);
  ccord_report_menu_free(&menu);
  if (!valid) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: report path does not follow the menu");

  if (m->late_report_fail[0]) {   // accepted by the call, refused later like a Discord 4xx; nothing is recorded
    mock_item it = mk_make_item(CCORD_NET_ACTION_FAILED, mk_now_of(m) + m->cfg.latency_ms);
    it.ev.action = CCORD_ACTION_REPORT;
    it.ev.channel_id = req->channel_id;
    it.ev.message_id = req->message_id;
    it.ev.user_id = req->reported_user_id;
    it.ev.status = CCORD_ERR_PERMISSION;
    char *why = ccord_strndup(m->late_report_fail, strlen(m->late_report_fail));   // a copy: the hook may change before the poll
    if (!why) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    mk_retire_content(m, why);
    it.ev.text = why;
    return mk_push_item(m, &it, err);
  }
  mock_report *copy = mock_report_copy(req);
  if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  mock_item it = mk_make_item(CCORD_NET_REPORT_SUBMITTED, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.status = CCORD_OK;
  it.ev.expr_arg = ccord_report_type_name(req->type);
  it.ev.report_id = 700000000000000001ull + m->report_submits;
  it.ev.channel_id = req->channel_id;
  it.ev.message_id = req->message_id;
  it.ev.user_id = req->reported_user_id;
  st = mk_push_item(m, &it, err);
  if (st != CCORD_OK) {
    mock_report_free(copy);
    return st;
  }
  mock_report_free(m->last_report);
  m->last_report = copy;
  m->report_submits++;
  return CCORD_OK;
}

void ccord_mock_set_report_menu_failure(ccord_transport *t, const char *reason)
{
  if (!t) return;
  (void)snprintf(((mock *)t)->report_menu_fail, sizeof ((mock *)t)->report_menu_fail, "%s", reason ? reason : "");
}
void ccord_mock_set_late_report_failure(ccord_transport *t, const char *reason)
{
  if (!t) return;
  (void)snprintf(((mock *)t)->late_report_fail, sizeof ((mock *)t)->late_report_fail, "%s", reason ? reason : "");
}
void ccord_mock_set_report_broken_link(ccord_transport *t, bool on)
{
  if (t) ((mock *)t)->report_broken_link = on;
}
size_t ccord_mock_report_menu_count(const ccord_transport *t) { return t ? ((const mock *)t)->report_menu_calls : 0; }
size_t ccord_mock_report_submit_count(const ccord_transport *t) { return t ? ((const mock *)t)->report_submits : 0; }
const ccord_report_request *ccord_mock_last_report(const ccord_transport *t)
{
  const mock *m = (const mock *)t;
  return m && m->last_report ? &m->last_report->req : nullptr;
}


