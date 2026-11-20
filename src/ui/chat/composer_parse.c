// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

// ---------------------------------------------------------------- mention serialization & slash commands

static void ser_append(char *out, size_t cap, size_t *w, const char *src, size_t n)
{
  if (*w + 1u >= cap || n == 0) return;
  size_t avail = cap - 1u - *w;
  if (n > avail) n = avail;
  memcpy(out + *w, src, n);
  *w += n;
}

static bool is_mention_boundary(char c)
{
  return !isalnum((unsigned char)c) && c != '_';
}

static void check_user_cand(const char *src, size_t rem, const char *cand, ccord_id uid,
                            size_t *best_len, char best_tag[static 32])
{
  if (!cand || cand[0] == '\0' || uid == CCORD_ID_NONE) return;
  size_t clen = strlen(cand);
  if (clen == 0 || clen > rem || clen <= *best_len) return;
  if (strncasecmp(src, cand, clen) != 0) return;
  if (clen < rem && !is_mention_boundary(src[clen])) return;
  *best_len = clen;
  snprintf(best_tag, 32, "<@%llu>", (unsigned long long)uid);
}

bool dc_can_mention_everyone_in(const ccord_store *store, ccord_id guild_id, ccord_id channel_id,
                                    int64_t now_unix_ms)
{
  if (!store || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return false;
  ccord_id self = ccord_store_self_id(store);
  if (self == CCORD_ID_NONE || !ccord_store_permissions_exact(store, guild_id, self)) return true;
  uint64_t p = ccord_store_permissions(store, guild_id, channel_id, self, now_unix_ms);
  return ccord_perm_has(p, CCORD_PERM_MENTION_EVERYONE);
}

bool dc_has_at_mention_boundary_before(const char *in, size_t i)
{
  if (i == 0) return true;
  char prev = in[i - 1u];
  return !isalnum((unsigned char)prev) && prev != '_' && prev != '.' && prev != '/' && prev != ':' &&
         prev != '@' && prev != '<';
}

static void check_user_obj_cand(const char *src, size_t rem, const ccord_user *u,
                                size_t *best_len, char best_tag[static 32])
{
  if (!u) return;
  check_user_cand(src, rem, u->name, u->id, best_len, best_tag);
  check_user_cand(src, rem, u->handle, u->id, best_len, best_tag);
}

// Matches a `@user`, `@role`, `@everyone`, `@here`, or `#channel` mention at `in[i]`.
// Returns the matched raw byte length (including `@`/`#`) or 0 if none. Writes `<@id>` / `<@&id>` / `<#id>` /
// `@everyone` / `@here` into `tag_out` when non-null.
static size_t match_mention_at(const ccord_store *store, ccord_id guild_id, const ccord_channel *active_ch,
                               bool gate_perms, int64_t now_unix_ms,
                               const char *in, size_t in_len, size_t i, char tag_out[32])
{
  if (!store || !in || i >= in_len) return 0;
  if (in[i] == '@' && dc_has_at_mention_boundary_before(in, i)) {
    const char *src = in + i + 1u;
    size_t rem = in_len - (i + 1u);
    bool in_guild = guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID;
    bool can_everyone = !gate_perms || (in_guild && active_ch &&
                                        dc_can_mention_everyone_in(store, guild_id, active_ch->id, now_unix_ms));
    if (can_everyone && (!gate_perms || in_guild)) {
      if (rem >= 8u && strncmp(src, "everyone", 8u) == 0 && (rem == 8u || is_mention_boundary(src[8u]))) {
        if (tag_out) snprintf(tag_out, 32, "@everyone");
        return 9u;
      }
      if (rem >= 4u && strncmp(src, "here", 4u) == 0 && (rem == 4u || is_mention_boundary(src[4u]))) {
        if (tag_out) snprintf(tag_out, 32, "@here");
        return 5u;
      }
    }
    size_t best_len = 0;
    char best_tag[32] = {0};
    if (in_guild) {
      size_t mc = ccord_store_member_count(store, guild_id);
      for (size_t k = 0; k < mc; k++) {
        const ccord_member *m = ccord_store_member_at(store, guild_id, k);
        if (!m) continue;
        const ccord_user *u = ccord_store_user(store, m->user_id);
        if (m->nick[0] != '\0') check_user_cand(src, rem, m->nick, m->user_id, &best_len, best_tag);
        check_user_obj_cand(src, rem, u, &best_len, best_tag);
      }
    }
    if (guild_id == CCORD_DM_GUILD_ID) {
      ccord_id self_id = ccord_store_self_id(store);
      check_user_obj_cand(src, rem, ccord_store_user(store, self_id), &best_len, best_tag);
      if (active_ch) {
        if (active_ch->user_id != CCORD_ID_NONE) {
          check_user_obj_cand(src, rem, ccord_store_user(store, active_ch->user_id), &best_len, best_tag);
        }
        for (uint16_t r = 0; r < active_ch->recipient_count && r < CCORD_MAX_DM_RECIPIENTS; r++) {
          check_user_obj_cand(src, rem, ccord_store_user(store, active_ch->recipient_ids[r]), &best_len, best_tag);
        }
      } else {
        size_t dmc = ccord_store_channel_count(store, CCORD_DM_GUILD_ID);
        for (size_t d = 0; d < dmc; d++) {
          const ccord_channel *dc = ccord_store_channel_at(store, CCORD_DM_GUILD_ID, d);
          if (!dc) continue;
          if (dc->user_id != CCORD_ID_NONE) {
            check_user_obj_cand(src, rem, ccord_store_user(store, dc->user_id), &best_len, best_tag);
          }
          for (uint16_t r = 0; r < dc->recipient_count && r < CCORD_MAX_DM_RECIPIENTS; r++) {
            check_user_obj_cand(src, rem, ccord_store_user(store, dc->recipient_ids[r]), &best_len, best_tag);
          }
        }
      }
    }
    if (active_ch) {
      size_t msg_c = ccord_store_message_count(store, active_ch->id);
      for (size_t k = 0; k < msg_c; k++) {
        const ccord_message *msg = ccord_store_message_at(store, active_ch->id, k);
        if (msg && msg->author_id != CCORD_ID_NONE) {
          check_user_obj_cand(src, rem, ccord_store_user(store, msg->author_id), &best_len, best_tag);
        }
      }
    }
    if (in_guild) {
      size_t rc = 0;
      const ccord_role *roles = ccord_store_roles(store, guild_id, &rc);
      for (size_t k = 0; k < rc; k++) {
        const ccord_role *rl = &roles[k];
        if (rl->id == guild_id || !rl->name || rl->name[0] == '\0') continue;
        if (gate_perms && !rl->mentionable && !can_everyone) continue;
        size_t rlen = strlen(rl->name);
        if (rlen == 0 || rlen > rem || rlen <= best_len) continue;
        if (strncasecmp(src, rl->name, rlen) != 0) continue;
        if (rlen < rem && !is_mention_boundary(src[rlen])) continue;
        best_len = rlen;
        snprintf(best_tag, sizeof best_tag, "<@&%llu>", (unsigned long long)rl->id);
      }
    }
    if (best_len > 0) {
      if (tag_out) memcpy(tag_out, best_tag, sizeof best_tag);
      return 1u + best_len;
    }
    return 0;
  }
  if (in[i] == '#' && guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID &&
      (i == 0 || (!isalnum((unsigned char)in[i - 1u]) && in[i - 1u] != '_' &&
                  in[i - 1u] != '&' && in[i - 1u] != '<' && in[i - 1u] != '#'))) {
    const char *src = in + i + 1u;
    size_t rem = in_len - (i + 1u);
    size_t best_len = 0;
    char best_tag[32] = {0};
    size_t cc = ccord_store_channel_count(store, guild_id);
    for (size_t k = 0; k < cc; k++) {
      const ccord_channel *c = ccord_store_channel_at(store, guild_id, k);
      if (!c || c->kind == CCORD_CHANNEL_CATEGORY || !c->name || c->name[0] == '\0') continue;
      size_t clen = strlen(c->name);
      if (clen == 0 || clen > rem || clen <= best_len) continue;
      if (strncasecmp(src, c->name, clen) != 0) continue;
      if (clen < rem && (!is_mention_boundary(src[clen]) || src[clen] == '-')) continue;
      best_len = clen;
      snprintf(best_tag, sizeof best_tag, "<#%llu>", (unsigned long long)c->id);
    }
    if (best_len > 0) {
      if (tag_out) memcpy(tag_out, best_tag, sizeof best_tag);
      return 1u + best_len;
    }
  }
  return 0;
}

size_t ui_composer_match_mention(const ccord_ui *ui, const char *line, size_t line_len, size_t pos)
{
  if (!ui || !ui->d.store || !line || pos >= line_len) return 0;
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch || !ui_channel_has_chat(ch)) return 0;
  ccord_id gid = ch->guild_id != CCORD_ID_NONE ? ch->guild_id : ccord_store_selected_guild(ui->d.store);
  return match_mention_at(ui->d.store, gid, ch, true, ui->now_unix_ms, line, line_len, pos, nullptr);
}

size_t ccord_ui_serialize_mentions(const ccord_store *store, ccord_id guild_id,
                                   const char *in, size_t in_len, char *out, size_t out_cap)
{
  if (!out || out_cap == 0) return 0;
  if (!in || in_len == 0) {
    out[0] = '\0';
    return 0;
  }
  if (!store) {
    size_t n = in_len < out_cap - 1u ? in_len : out_cap - 1u;
    memcpy(out, in, n);
    out[n] = '\0';
    return n;
  }
  size_t w = 0;
  size_t i = 0;
  while (i < in_len && w + 1u < out_cap) {
    // 1. Fenced code block ```...```
    if (i + 2u < in_len && in[i] == '`' && in[i + 1u] == '`' && in[i + 2u] == '`') {
      size_t j = i + 3u;
      while (j + 2u < in_len && !(in[j] == '`' && in[j + 1u] == '`' && in[j + 2u] == '`')) j++;
      size_t end = (j + 2u < in_len) ? (j + 3u) : in_len;
      ser_append(out, out_cap, &w, in + i, end - i);
      i = end;
      continue;
    }
    // 2. Inline code `...`
    if (in[i] == '`') {
      size_t j = i + 1u;
      while (j < in_len && in[j] != '`') j++;
      if (j < in_len) {
        ser_append(out, out_cap, &w, in + i, j + 1u - i);
        i = j + 1u;
        continue;
      }
      ser_append(out, out_cap, &w, in + i, 1u);
      i++;
      continue;
    }
    // 3. Existing angle-bracket tag <...>
    if (in[i] == '<') {
      size_t j = i + 1u;
      while (j < in_len && in[j] != '>' && in[j] != ' ' && in[j] != '\n' && in[j] != '\t') j++;
      if (j < in_len && in[j] == '>') {
        ser_append(out, out_cap, &w, in + i, j + 1u - i);
        i = j + 1u;
        continue;
      }
    }
    // 4. URLs http:// or https://
    if ((i + 7u <= in_len && strncasecmp(in + i, "http://", 7u) == 0) ||
        (i + 8u <= in_len && strncasecmp(in + i, "https://", 8u) == 0)) {
      size_t j = i;
      while (j < in_len && in[j] != ' ' && in[j] != '\t' && in[j] != '\n' && in[j] != '\r') j++;
      ser_append(out, out_cap, &w, in + i, j - i);
      i = j;
      continue;
    }
    // 5. @user / @role / @everyone / @here & 6. #channel
    if (in[i] == '@' || in[i] == '#') {
      char tag[32] = {0};
      size_t mlen = match_mention_at(store, guild_id, nullptr, false, 0, in, in_len, i, tag);
      if (mlen > 0) {
        ser_append(out, out_cap, &w, tag, strlen(tag));
        i += mlen;
        continue;
      }
    }
    ser_append(out, out_cap, &w, in + i, 1u);
    i++;
  }
  out[w] = '\0';
  return w;
}

bool dc_slash_cmd_allowed(const ccord_ui *ui, const ccord_channel *ch, const ccord_slash_command *cmd)
{
  if (!ui || !ui->d.store || !ch || !cmd || cmd->name[0] == '\0') return false;
  if (cmd->command_type > 1u) return false;   // USER / MESSAGE commands live in context menus, not the slash picker
  if (ch->guild_id == CCORD_DM_GUILD_ID) return cmd->dm_permission;
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, ch->guild_id, self)) return true;
  uint64_t ch_perms = ccord_store_permissions(ui->d.store, ch->guild_id, ch->id, self, ui->now_unix_ms);
  if (!ccord_perm_has(ch_perms, CCORD_PERM_USE_APPLICATION_COMMANDS)) return false;
  if (cmd->default_member_permissions != UINT64_MAX) {
    if (cmd->default_member_permissions == 0) {
      if ((ch_perms & CCORD_PERM_ADMINISTRATOR) == 0) return false;
    } else if (!ccord_perm_has(ch_perms, cmd->default_member_permissions)) {
      return false;
    }
  }
  return true;
}

const ccord_slash_command *dc_find_slash_cmd(const ccord_ui *ui, ccord_id guild_id,
                                                 const char *name, size_t nlen)
{
  if (!ui || !ui->d.store || !name || nlen == 0) return nullptr;
  size_t count = 0;
  const ccord_slash_command *cmds = ccord_store_slash_commands(ui->d.store, guild_id, &count);
  for (size_t i = 0; i < count; i++) {
    if (strncasecmp(cmds[i].name, name, nlen) == 0 && cmds[i].name[nlen] == '\0') return &cmds[i];
  }
  return nullptr;
}

// Options live in a tree: with a subcommand tree only the chosen subcommand's children are fillable;
// without one only root options are. SUB_COMMAND / SUB_COMMAND_GROUP entries never carry values.
bool dc_slash_opt_in_scope(const ccord_slash_command *cmd, size_t i, const slash_parsed *sp)
{
  if (!sp->has_sub_tree) return cmd->options[i].parent_plus_one == 0;
  if (sp->sub_cmd_idx < 0) return false;
  return cmd->options[i].parent_plus_one == (uint8_t)(sp->sub_cmd_idx + 1);
}

static int find_cmd_opt(const ccord_slash_command *cmd, const char *name, size_t nlen, const slash_parsed *sp)
{
  if (!cmd || !name || nlen == 0) return -1;
  for (size_t i = 0; i < cmd->option_count; i++) {
    if (!dc_slash_opt_in_scope(cmd, i, sp)) continue;
    if (strncasecmp(cmd->options[i].name, name, nlen) == 0 && cmd->options[i].name[nlen] == '\0') {
      return (int)i;
    }
  }
  return -1;
}

static size_t read_slash_val_token(const char *text, size_t limit, size_t pos, char *dst, size_t dst_cap, bool *unclosed)
{
  *unclosed = false;
  size_t w = 0;
  if (pos < limit && text[pos] == '"') {
    pos++;
    while (pos < limit) {
      if (text[pos] == '\\' && pos + 1u < limit) {
        pos++;
        if (w + 1u < dst_cap) dst[w++] = text[pos];
        pos++;
        continue;
      }
      if (text[pos] == '"') {
        pos++;
        dst[w] = '\0';
        return pos;
      }
      if (w + 1u < dst_cap) dst[w++] = text[pos];
      pos++;
    }
    *unclosed = true;
    dst[w] = '\0';
    return pos;
  }
  while (pos < limit && text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\n') {
    if (w + 1u < dst_cap) dst[w++] = text[pos];
    pos++;
  }
  dst[w] = '\0';
  return pos;
}

void dc_slash_parse_args(const ccord_slash_command *cmd, const char *text, size_t limit, slash_parsed *out)
{
  memset(out, 0, sizeof *out);
  out->focused_opt = -1;
  out->sub_group_idx = -1;
  out->sub_cmd_idx = -1;
  out->token_start = limit;
  if (!cmd || !text || limit == 0 || text[0] != '/') return;
  size_t nlen = strlen(cmd->name);
  if (limit < 1u + nlen) return;
  size_t pos = 1u + nlen;
  int last_pos_string_opt = -1;
  for (size_t i = 0; i < cmd->option_count; i++) {
    if (cmd->options[i].parent_plus_one == 0 &&
        (cmd->options[i].kind == CCORD_SLASH_OPT_SUB_COMMAND ||
         cmd->options[i].kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP)) {
      out->has_sub_tree = true;
      break;
    }
  }

  while (pos < limit) {
    while (pos < limit && (text[pos] == ' ' || text[pos] == '\t')) pos++;
    if (pos >= limit) {
      out->token_start = limit;
      out->completing_opt_name = true;
      out->focused_opt = -1;
      if (out->has_sub_tree && out->sub_cmd_idx < 0) out->selecting_sub = true;
      return;
    }
    size_t tok_start = pos;
    size_t ilen = 0;
    while (pos + ilen < limit &&
           (isalnum((unsigned char)text[pos + ilen]) || text[pos + ilen] == '_' || text[pos + ilen] == '-')) {
      ilen++;
    }
    int named_idx = -1;
    if (ilen > 0 && pos + ilen < limit && text[pos + ilen] == ':') {
      named_idx = find_cmd_opt(cmd, text + pos, ilen, out);
    }
    if (named_idx >= 0) {
      last_pos_string_opt = -1;
      pos += ilen + 1u;
      while (pos < limit && text[pos] == ' ') pos++;
      size_t val_start = pos;
      bool unclosed = false;
      pos = read_slash_val_token(text, limit, pos, out->opt_vals[named_idx],
                                 sizeof out->opt_vals[named_idx], &unclosed);
      out->opt_set[named_idx] = true;
      if (pos >= limit && (unclosed || val_start == limit || (limit > 0 && text[limit - 1u] != ' '))) {
        out->focused_opt = named_idx;
        out->token_start = val_start;
        out->completing_opt_name = false;
        return;
      }
      continue;
    }
    bool was_quoted = (text[pos] == '"');
    char tok_buf[256];
    bool unclosed = false;
    size_t next_pos = read_slash_val_token(text, limit, pos, tok_buf, sizeof tok_buf, &unclosed);
    bool at_caret_end = (next_pos >= limit && (unclosed || (limit > 0 && text[limit - 1u] != ' ')));

    // Subcommand tree: the first tokens pick the group (if any) and the subcommand before any option.
    if (out->has_sub_tree && out->sub_cmd_idx < 0) {
      int match = -1;
      if (out->sub_group_idx < 0) {
        for (size_t k = 0; k < cmd->option_count; k++) {
          if (cmd->options[k].parent_plus_one != 0) continue;
          if (cmd->options[k].kind != CCORD_SLASH_OPT_SUB_COMMAND &&
              cmd->options[k].kind != CCORD_SLASH_OPT_SUB_COMMAND_GROUP) continue;
          if (strcasecmp(cmd->options[k].name, tok_buf) == 0) { match = (int)k; break; }
        }
        if (match >= 0) {
          if (cmd->options[match].kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP) out->sub_group_idx = match;
          else out->sub_cmd_idx = match;
          pos = next_pos;
          continue;
        }
      } else {
        for (size_t k = 0; k < cmd->option_count; k++) {
          if (cmd->options[k].parent_plus_one != (uint8_t)(out->sub_group_idx + 1)) continue;
          if (cmd->options[k].kind != CCORD_SLASH_OPT_SUB_COMMAND) continue;
          if (strcasecmp(cmd->options[k].name, tok_buf) == 0) { match = (int)k; break; }
        }
        if (match >= 0) {
          out->sub_cmd_idx = match;
          pos = next_pos;
          continue;
        }
      }
      if (at_caret_end) {
        out->selecting_sub = true;
        out->token_start = tok_start;
        out->focused_opt = -1;
        snprintf(out->cur_prefix, sizeof out->cur_prefix, "%.127s", tok_buf);
        return;
      }
      pos = next_pos;   // an unknown subcommand token is skipped, not turned into an option value
      continue;
    }

    int pos_idx = -1;
    for (size_t k = 0; k < cmd->option_count; k++) {
      if (!out->opt_set[k] && dc_slash_opt_in_scope(cmd, k, out)) {
        pos_idx = (int)k;
        break;
      }
    }
    // Single-string-option ergonomics: `/echo hello world` appends unquoted words to the string option
    if (!was_quoted && last_pos_string_opt >= 0 &&
        (pos_idx < 0 || (!cmd->options[pos_idx].required &&
                         cmd->options[pos_idx].kind == CCORD_SLASH_OPT_BOOLEAN &&
                         strcasecmp(tok_buf, "true") != 0 && strcasecmp(tok_buf, "false") != 0 &&
                         strcmp(tok_buf, "1") != 0 && strcmp(tok_buf, "0") != 0))) {
      size_t cur_l = strlen(out->opt_vals[last_pos_string_opt]);
      size_t add_l = strlen(tok_buf);
      if (cur_l + 1u + add_l < sizeof out->opt_vals[last_pos_string_opt]) {
        out->opt_vals[last_pos_string_opt][cur_l] = ' ';
        memcpy(out->opt_vals[last_pos_string_opt] + cur_l + 1u, tok_buf, add_l + 1u);
      }
      if (at_caret_end) {
        out->focused_opt = last_pos_string_opt;
        out->token_start = tok_start;
        out->completing_opt_name = !was_quoted;
        snprintf(out->cur_prefix, sizeof out->cur_prefix, "%.127s", tok_buf);
        return;
      }
      pos = next_pos;
      continue;
    }
    if (pos_idx >= 0) {
      snprintf(out->opt_vals[pos_idx], sizeof out->opt_vals[pos_idx], "%s", tok_buf);
      out->opt_set[pos_idx] = true;
      last_pos_string_opt = (!was_quoted && cmd->options[pos_idx].kind == CCORD_SLASH_OPT_STRING) ? pos_idx : -1;
      if (at_caret_end) {
        out->focused_opt = pos_idx;
        out->token_start = tok_start;
        out->completing_opt_name = !was_quoted;
        snprintf(out->cur_prefix, sizeof out->cur_prefix, "%.127s", tok_buf);
        return;
      }
    } else if (at_caret_end) {
      out->focused_opt = -1;
      out->token_start = tok_start;
      out->completing_opt_name = !was_quoted;
      snprintf(out->cur_prefix, sizeof out->cur_prefix, "%.127s", tok_buf);
      return;
    }
    pos = next_pos;
  }
  out->token_start = limit;
  out->completing_opt_name = true;
  out->focused_opt = -1;
  if (out->has_sub_tree && out->sub_cmd_idx < 0) out->selecting_sub = true;
}

static bool resolve_slash_snowflake(const ccord_store *store, ccord_id guild_id, ccord_slash_opt_kind kind,
                                    const char *raw, char out[static 32])
{
  if (!raw || raw[0] == '\0') return false;
  size_t len = strlen(raw);
  // Strip <@123>, <@!123>, <@&123>, <#123>
  if (len >= 4u && raw[0] == '<' && raw[len - 1u] == '>') {
    size_t off = 1u;
    if (raw[off] == '@' || raw[off] == '#') off++;
    if (raw[off] == '!' || raw[off] == '&') off++;
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(raw + off, len - 1u - off, &id)) {
      ccord_id_format(id, out);
      return true;
    }
  }
  ccord_id parsed = CCORD_ID_NONE;
  if (ccord_id_parse(raw, len, &parsed)) {
    ccord_id_format(parsed, out);
    return true;
  }
  const char *q = (raw[0] == '@' || raw[0] == '#') ? raw + 1u : raw;
  if (q[0] == '\0' || !store) return false;
  if (kind == CCORD_SLASH_OPT_USER || kind == CCORD_SLASH_OPT_MENTIONABLE) {
    size_t mc = ccord_store_member_count(store, guild_id);
    for (size_t i = 0; i < mc; i++) {
      const ccord_member *m = ccord_store_member_at(store, guild_id, i);
      const ccord_user *u = m ? ccord_store_user(store, m->user_id) : nullptr;
      if ((m && m->nick[0] && strcasecmp(m->nick, q) == 0) ||
          (u && u->name && strcasecmp(u->name, q) == 0) ||
          (u && u->handle && u->handle[0] && strcasecmp(u->handle, q) == 0)) {
        ccord_id_format(m->user_id, out);
        return true;
      }
    }
  }
  if ((kind == CCORD_SLASH_OPT_ROLE || kind == CCORD_SLASH_OPT_MENTIONABLE) &&
      guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID) {
    size_t rc = 0;
    const ccord_role *roles = ccord_store_roles(store, guild_id, &rc);
    for (size_t i = 0; i < rc; i++) {
      if (roles[i].name && strcasecmp(roles[i].name, q) == 0) {
        ccord_id_format(roles[i].id, out);
        return true;
      }
    }
  }
  if (kind == CCORD_SLASH_OPT_CHANNEL && guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID) {
    size_t cc = ccord_store_channel_count(store, guild_id);
    for (size_t i = 0; i < cc; i++) {
      const ccord_channel *c = ccord_store_channel_at(store, guild_id, i);
      if (c && c->kind != CCORD_CHANNEL_CATEGORY && c->name && strcasecmp(c->name, q) == 0) {
        ccord_id_format(c->id, out);
        return true;
      }
    }
  }
  return false;
}

bool dc_validate_slash_opt_val(const ccord_store *store, ccord_id guild_id, const ccord_slash_option *opt,
                                   char val[static 256], char err_msg[static 128])
{
  switch (opt->kind) {
    case CCORD_SLASH_OPT_STRING:
      return true;
    case CCORD_SLASH_OPT_INTEGER: {
      if (val[0] == '\0') {
        snprintf(err_msg, 128, "Option '%s' requires an integer", opt->name);
        return false;
      }
      errno = 0;
      char *endp = nullptr;
      (void)strtoll(val, &endp, 10);
      if (errno == ERANGE || !endp || *endp != '\0') {
        snprintf(err_msg, 128, "Option '%s' must be a valid integer", opt->name);
        return false;
      }
      return true;
    }
    case CCORD_SLASH_OPT_NUMBER: {
      if (val[0] == '\0') {
        snprintf(err_msg, 128, "Option '%s' requires a number", opt->name);
        return false;
      }
      errno = 0;
      char *endp = nullptr;
      double d = strtod(val, &endp);
      if (errno == ERANGE || !endp || *endp != '\0' || !isfinite(d)) {
        snprintf(err_msg, 128, "Option '%s' must be a valid number", opt->name);
        return false;
      }
      return true;
    }
    case CCORD_SLASH_OPT_BOOLEAN:
      if (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0 || strcasecmp(val, "yes") == 0) {
        snprintf(val, 256, "true");
        return true;
      }
      if (strcasecmp(val, "false") == 0 || strcmp(val, "0") == 0 || strcasecmp(val, "no") == 0) {
        snprintf(val, 256, "false");
        return true;
      }
      snprintf(err_msg, 128, "Option '%s' must be true or false", opt->name);
      return false;
    case CCORD_SLASH_OPT_USER:
    case CCORD_SLASH_OPT_CHANNEL:
    case CCORD_SLASH_OPT_ROLE:
    case CCORD_SLASH_OPT_MENTIONABLE: {
      char resolved[32];
      if (!resolve_slash_snowflake(store, guild_id, opt->kind, val, resolved)) {
        snprintf(err_msg, 128, "Option '%s' could not be resolved", opt->name);
        return false;
      }
      snprintf(val, 256, "%s", resolved);
      return true;
    }
    default: break;  // SUB_COMMAND / SUB_COMMAND_GROUP carry no value of their own
  }
  return true;
}

