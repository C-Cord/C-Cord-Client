// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "context_menu_internal.h"

static void activate(ccord_ui *ui, size_t idx)
{
  menu_item it = ui->menu.items[idx];
  ccord_menu_context ctx = ui->menu.ctx;
  if (it.builtin >= MENU_BUILTIN_TEXT_PASTE && it.builtin <= MENU_BUILTIN_TEXT_CLEAR) {
    ccord_text_edit *te = ui->menu_text_edit;
    bool is_composer = ui->menu_text_is_composer;
    bool is_quickcss = ui->menu_text_is_quickcss;
    bool is_search = ui->menu_text_is_search;
    if (!te) return;
    if (it.builtin == MENU_BUILTIN_TEXT_PASTE) {
      bool pasted = false;
      if (ui->d.host.clipboard_get) {
        char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
        if (clip && clip[0]) {
          ccord_error ierr = {0};
          ccord_status st = is_composer ? ccord_text_edit_paste(te, clip, strlen(clip), &ierr)
                                        : ccord_text_edit_insert(te, clip, strlen(clip), &ierr);
          if (st == CCORD_OK) {
            pasted = true;
            if (is_quickcss) ui_quickcss_changed(ui);
          } else {
            ui_notice(ui, "%s", ierr.message[0] ? ierr.message : "Could not paste text");
          }
        }
        ccord_free(clip);
      }
      if (!pasted && is_composer && ui->d.host.get_clipboard_image) {
        uint8_t *png = nullptr;
        size_t png_len = 0;
        if (ui->d.host.get_clipboard_image(ui->d.host.ud, &png, &png_len) && png && png_len > 0) {
          (void)ccord_ui_attach_bytes(ui, "clipboard.png", png, png_len);
          ccord_free(png);
        }
      }
      (void)is_search;
      ui->dirty = true;
      return;
    }
    if (it.builtin == MENU_BUILTIN_TEXT_COPY || it.builtin == MENU_BUILTIN_TEXT_CUT) {
      if (ui->d.host.clipboard_set && te->len > 0) {
        char *sel = ccord_text_edit_copy_selection(te);
        if (sel) {
          ui->d.host.clipboard_set(ui->d.host.ud, sel);
          ccord_free(sel);
          if (it.builtin == MENU_BUILTIN_TEXT_CUT) ccord_text_edit_backspace(te);
        } else {
          ui->d.host.clipboard_set(ui->d.host.ud, ccord_text_edit_cstr(te));
          if (it.builtin == MENU_BUILTIN_TEXT_CUT) ccord_text_edit_clear(te);
        }
        if (it.builtin == MENU_BUILTIN_TEXT_CUT && is_quickcss) ui_quickcss_changed(ui);
        ui_notice(ui, it.builtin == MENU_BUILTIN_TEXT_CUT ? "Cut to clipboard" : "Copied to clipboard");
      }
      ui->dirty = true;
      return;
    }
    if (it.builtin == MENU_BUILTIN_TEXT_SELECT_ALL) {
      ccord_text_edit_select_all(te);
      ui->dirty = true;
      return;
    }
    if (it.builtin == MENU_BUILTIN_TEXT_CLEAR) {
      ccord_text_edit_clear(te);
      if (is_quickcss) ui_quickcss_changed(ui);
      ui->dirty = true;
      return;
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_MENTION_USER) {
    if (ctx.user_id == CCORD_ID_NONE || !cm_can_mention_in_composer(ui)) return;
    const ccord_user *u = ccord_store_user(ui->d.store, ctx.user_id);
    bool need_lead = false;
    if (ui->edit.caret > 0 && ui->edit.caret <= ui->edit.len) {
      char prev = ui->edit.data[ui->edit.caret - 1u];
      need_lead = (prev != ' ' && prev != '\t' && prev != '\n');
    }
    char ins[CCORD_NAME_MAX_BYTES + 32];
    int n = 0;
    if (u && u->name && u->name[0] != '\0') {
      n = snprintf(ins, sizeof ins, "%s@%s ", need_lead ? " " : "", u->name);
    } else {
      n = snprintf(ins, sizeof ins, "%s<@%llu> ", need_lead ? " " : "", (unsigned long long)ctx.user_id);
    }
    if (n > 0 && (size_t)n < sizeof ins) {
      ccord_error err;
      if (ccord_text_edit_insert(&ui->edit, ins, (size_t)n, &err) != CCORD_OK) {
        ui_notice(ui, "%s", err.message);
        return;
      }
      ui->autocomplete_open = false;
      ui->nautocomplete = 0;
      ui->caret_epoch_ms = ui->now_ms;
      ui->caret_on = true;
      ui->dirty = true;
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_TOGGLE_MEMBER_ROLE) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ccord_id role_id = (ccord_id)it.arg;
    if (gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID || ctx.user_id == CCORD_ID_NONE || role_id == CCORD_ID_NONE) {
      return;
    }
    const ccord_id *held = nullptr;
    size_t nheld = 0;
    bool has_it = ccord_store_member_roles(ui->d.store, gid, ctx.user_id, &held, &nheld) &&
                  cm_member_holds_role(held, nheld, role_id);
    ccord_admin_request req;
    ccord_error err;
    ccord_status st = has_it ? ccord_admin_remove_member_role(&req, gid, ctx.user_id, role_id, nullptr, &err)
                             : ccord_admin_add_member_role(&req, gid, ctx.user_id, role_id, nullptr, &err);
    if (st != CCORD_OK) {
      ui_notice(ui, "Couldn't %s: %s", has_it ? "remove the role" : "give the role", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_MOD_VIEW) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ui_server_settings_open_modview(ui, gid, ctx.user_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_DISMISS_EPHEMERAL) {
    ui_dismiss_ephemeral(ui, ctx.channel_id, ctx.message_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_COPY_TEXT) {
    if (ui->chat_sel_active && ui->chat_sel_len > 0 && ui->d.host.clipboard_set) {
      ui->d.host.clipboard_set(ui->d.host.ud, ui->chat_sel_buf);
      ui_notice(ui, "Copied selected text");
      return;
    }
    const ccord_message *m = ccord_store_find_message(ui->d.store, ctx.channel_id, ctx.message_id);
    if (m && ui->d.host.clipboard_set) {
      char raw[CCORD_MESSAGE_MAX_BYTES + 1];
      ui_message_clean_body(ui, m, raw, sizeof raw);
      size_t rlen = strlen(raw);
      char *clean = ccord_malloc(rlen + 1);
      if (clean) {
        size_t mlen = 0;
        size_t i = 0;
        while (i < rlen) {
          unsigned char c = (unsigned char)raw[i];
          if (c == 0x1B) {
            i++;
            if (i < rlen && raw[i] == '[') {
              i++;
              while (i < rlen) {
                unsigned char pc = (unsigned char)raw[i];
                if (pc >= 0x40 && pc <= 0x7E) { i++; break; }
                i++;
              }
            }
            continue;
          }
          if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            i++;
            continue;
          }
          clean[mlen++] = (char)c;
          i++;
        }
        clean[mlen] = '\0';
        ui->d.host.clipboard_set(ui->d.host.ud, clean);
        ccord_free(clean);
        ui_notice(ui, "Copied message text");
      }
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_EDIT) {
    if (!cm_message_owned_by_self(ui, &ctx)) {
      ui_notice(ui, "You can only edit your own messages");
      return;
    }
    ui_begin_edit(ui, ctx.channel_id, ctx.message_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_REACT) {
    // The same emoji picker the hover bar's react button opens (tab 2 is the full emoji tab), aimed at this message.
    ui_expr_picker_open(ui, 2, ctx.channel_id, ctx.message_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_DELETE) {
    ccord_id self_id = ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE
                     ? ctx.guild_id
                     : (ui->d.store ? ccord_store_selected_guild(ui->d.store) : CCORD_ID_NONE);
    bool can_manage_msg = (gid != CCORD_DM_GUILD_ID &&
                           ccord_store_has_permission(ui->d.store, gid, ctx.channel_id, self_id,
                                                      CCORD_PERM_MANAGE_MESSAGES));
    if (!cm_message_owned_by_self(ui, &ctx) && !can_manage_msg) {
      ui_notice(ui, "You can only delete your own messages");
      return;
    }
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_error err;
    if (ccord_session_delete_message(ui->d.session, ctx.channel_id, ctx.message_id, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot delete: %s", err.message);
      return;
    }
    if (ui->editing_message == ctx.message_id) ui_cancel_edit(ui);
    if (ui->reply_message == ctx.message_id) ui_cancel_reply(ui);
    return;
  }
  if (it.builtin == MENU_BUILTIN_REPLY) {
    ui_begin_reply(ui, ctx.channel_id, ctx.message_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_FORWARD) {
    ui_forward_picker_open(ui, ctx.channel_id, ctx.message_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_CROSSPOST) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_error err;
    if (ccord_session_crosspost_message(ui->d.session, ctx.channel_id, ctx.message_id, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot publish: %s", err.message);
      return;
    }
    ui_notice(ui, "Published to followers");
    return;
  }
  if (it.builtin == MENU_BUILTIN_COPY_LINK) {
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
    if (!ch || ctx.message_id == CCORD_ID_NONE || !ui->d.host.clipboard_set) return;
    char gbuf[24], cbuf[24], mbuf[24];
    ccord_id_format(ch->guild_id, gbuf);
    ccord_id_format(ctx.channel_id, cbuf);
    ccord_id_format(ctx.message_id, mbuf);
    const char *g = ch->guild_id == CCORD_DM_GUILD_ID ? "@me" : gbuf;
    char link[128];
    (void)snprintf(link, sizeof link, "https://discord.com/channels/%s/%s/%s", g, cbuf, mbuf);
    ui->d.host.clipboard_set(ui->d.host.ud, link);
    ui_notice(ui, "Copied message link");
    return;
  }
  if (it.builtin == MENU_BUILTIN_MARK_UNREAD) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_error err;
    if (ccord_session_mark_unread(ui->d.session, ctx.channel_id, ctx.message_id, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot mark unread: %s", err.message);
      return;
    }
    ui_notice(ui, "Marked unread from this message");
    return;
  }
  if (it.builtin == MENU_BUILTIN_SAVE_MESSAGE) {
    if (it.arg != 0) ui_saved_messages_save(ui, ctx.channel_id, ctx.message_id);
    else ui_saved_messages_unsave(ui, ctx.channel_id, ctx.message_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_REMIND_ME) {
    ui_reminder_add(ui, ctx.channel_id, ctx.message_id, it.arg);
    return;
  }
  if (it.builtin == MENU_BUILTIN_APP_COMMAND) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
    if (!ch) return;
    size_t count = 0;
    const ccord_slash_command *cmds = ccord_store_slash_commands(ui->d.store, ch->guild_id, &count);
    const ccord_slash_command *cmd = nullptr;
    for (size_t i = 0; i < count; i++) {
      if (cmds[i].id == (ccord_id)it.arg) { cmd = &cmds[i]; break; }
    }
    if (!cmd || (cmd->command_type != 2u && cmd->command_type != 3u)) return;
    ccord_id target = cmd->command_type == 2u ? ctx.user_id : ctx.message_id;
    if (target == CCORD_ID_NONE) return;
    ccord_interaction_request ireq = {
      .type = CCORD_INTERACTION_APPLICATION_COMMAND,
      .application_id = cmd->application_id,
      .guild_id = ch->guild_id == CCORD_DM_GUILD_ID ? CCORD_DM_GUILD_ID : ch->guild_id,
      .channel_id = ctx.channel_id,
      .command_id = cmd->id,
      .command_version = cmd->version,
      .command_name = cmd->name,
      .command_type = cmd->command_type,
      .target_id = target,
      .focused_option = -1,   // not an autocomplete interaction; live validates focused_option < 0 for context commands
    };
    ccord_error err;
    if (ccord_session_send_interaction(ui->d.session, &ireq, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot run %s: %s", cmd->name, err.message);
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_PIN) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_error err;
    if (ccord_session_toggle_pin(ui->d.session, ctx.channel_id, ctx.message_id, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot pin: %s", err.message);
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_SERVER_BOOST) {
    ui_open_url(ui, "https://discord.com/nitro", "Server Boost");
    return;
  }
  if (it.builtin == MENU_BUILTIN_OPEN_NITRO) {
    ui_open_url(ui, "https://discord.com/nitro", "Nitro");
    return;
  }
  if (it.builtin == MENU_BUILTIN_OPEN_SHOP) {
    ui_open_url(ui, "https://discord.com/shop", "Shop");
    return;
  }
  if (it.builtin == MENU_BUILTIN_OPEN_BILLING) {
    ui_open_url(ui, "https://discord.com/settings/billing", "Billing");
    return;
  }
  if (it.builtin == MENU_BUILTIN_OPEN_DM) {
    ui_open_dm_with_user(ui, ctx.user_id);
    return;
  }
  if (it.builtin == MENU_BUILTIN_ADD_FRIEND) {
    ui_set_user_relationship(ui, ctx.user_id, CCORD_RELATIONSHIP_FRIEND);
    return;
  }
  if (it.builtin == MENU_BUILTIN_REMOVE_FRIEND) {
    ui_set_user_relationship(ui, ctx.user_id, CCORD_RELATIONSHIP_NONE);
    return;
  }
  if (it.builtin == MENU_BUILTIN_REPORT_MESSAGE || it.builtin == MENU_BUILTIN_REPORT_USER) {
    ui_report_target target = {.type = CCORD_REPORT_USER, .guild_id = ctx.guild_id, .user_id = ctx.user_id};
    if (it.builtin == MENU_BUILTIN_REPORT_MESSAGE) {
      const ccord_message *rm = ccord_store_find_message(ui->d.store, ctx.channel_id, ctx.message_id);
      target = (ui_report_target){.type = CCORD_REPORT_MESSAGE, .guild_id = ctx.guild_id, .channel_id = ctx.channel_id,
                                  .message_id = ctx.message_id, .user_id = rm ? rm->author_id : ctx.user_id};
    }
    ui_report_flow_open(ui, &target);
    return;
  }
  if (it.builtin == MENU_BUILTIN_BLOCK_USER) {
    bool blocked = ui->d.store && ccord_store_relationship(ui->d.store, ctx.user_id) == CCORD_RELATIONSHIP_BLOCKED;
    ui_set_user_relationship(ui, ctx.user_id, blocked ? CCORD_RELATIONSHIP_NONE : CCORD_RELATIONSHIP_BLOCKED);
    return;
  }
  if (it.builtin == MENU_BUILTIN_PIN_DM) {
    ui_dm_toggle_pin(ui, (ccord_id)it.arg);
    return;
  }
  if (it.builtin == MENU_BUILTIN_FAVORITE_DM) {
    ui_dm_toggle_favorite(ui, (ccord_id)it.arg);
    return;
  }
  if (it.builtin == MENU_BUILTIN_START_CALL) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_id call_ch = (ccord_id)it.arg != CCORD_ID_NONE ? (ccord_id)it.arg : ctx.channel_id;
    if (call_ch == CCORD_ID_NONE) return;
    ccord_id self_id = ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;
    const ccord_voice_state *vs =
      (ui->d.store && self_id != CCORD_ID_NONE) ? ccord_store_voice_user(ui->d.store, CCORD_DM_GUILD_ID, self_id) : nullptr;
    bool in_here = vs && vs->channel_id == call_ch;
    ccord_error err;
    ccord_status st = in_here ? ccord_session_call_leave(ui->d.session, call_ch, &err)
                              : ccord_session_call_join(ui->d.session, call_ch, &err);
    if (st != CCORD_OK) {
      ui_notice(ui, "%s", err.message);
      return;
    }
    if (!in_here) (void)ccord_session_select_channel(ui->d.session, call_ch, &err);
    return;
  }
  if (it.builtin == MENU_BUILTIN_USER_NOTE) {
    ccord_id uid = (ccord_id)it.arg != CCORD_ID_NONE ? (ccord_id)it.arg : ctx.user_id;
    if (uid == CCORD_ID_NONE) return;
    ui->modal_kind = UI_MODAL_USER_NOTE;
    ui->modal_guild_id = CCORD_DM_GUILD_ID;
    ui->modal_target_id = uid;
    ui->modal_focus_field = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    const char *note = ui->d.store ? ccord_store_user_note(ui->d.store, uid) : nullptr;
    if (note && note[0] != '\0') {
      ccord_error err;
      (void)ccord_text_edit_insert(&ui->modal_edit1, note, strlen(note), &err);
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_FRIEND_NICK) {
    ccord_id uid = (ccord_id)it.arg != CCORD_ID_NONE ? (ccord_id)it.arg : ctx.user_id;
    if (uid == CCORD_ID_NONE) return;
    ui->modal_kind = UI_MODAL_FRIEND_NICK;
    ui->modal_guild_id = CCORD_DM_GUILD_ID;
    ui->modal_target_id = uid;
    ui->modal_focus_field = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    const char *fnick = ui_friend_nickname(ui, uid);
    if (fnick && fnick[0] != '\0') {
      ccord_error err;
      (void)ccord_text_edit_insert(&ui->modal_edit1, fnick, strlen(fnick), &err);
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_CLOSE_DM) {
    ccord_id cid = (ccord_id)it.arg != CCORD_ID_NONE ? (ccord_id)it.arg : ctx.channel_id;
    if (cid != CCORD_ID_NONE) ui_close_dm(ui, cid);
    return;
  }
  if (it.builtin == MENU_BUILTIN_INVITE_TO_GUILD) {
    ccord_id inv_gid = (ccord_id)it.arg;
    ccord_id self_id = ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;
    ccord_id inv_ch = cm_guild_invite_channel(ui, inv_gid, self_id);
    if (inv_gid == CCORD_ID_NONE || inv_ch == CCORD_ID_NONE) {
      ui_notice(ui, "No channel available to create an invite");
      return;
    }
    ccord_id dm_ch = ctx.channel_id != CCORD_ID_NONE ? ctx.channel_id : cm_find_1on1_dm_channel(ui, ctx.user_id);
    uint64_t prev_seq = 0;
    char prev_code[36] = {0};
    (void)ccord_store_last_invite(ui->d.store, inv_gid, prev_code, &prev_seq);
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_create_invite(&req, inv_gid, inv_ch, 86400u, 0u, false, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't create invite: %s", err.message);
      return;
    }
    if (dm_ch == CCORD_ID_NONE && ctx.user_id != CCORD_ID_NONE && ui->d.session) {
      (void)ccord_session_open_dm(ui->d.session, ctx.user_id, &err);
    }
    ui->pending_dm_invite_guild = inv_gid;
    ui->pending_dm_invite_channel = dm_ch;
    ui->pending_dm_invite_user = ctx.user_id;
    ui->pending_dm_invite_prev_count = (size_t)prev_seq;
    ui->pending_dm_invite_until_ms = ui->now_ms + 10000u;
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_IGNORE_USER) {
    ccord_id uid = (ccord_id)it.arg != CCORD_ID_NONE ? (ccord_id)it.arg : ctx.user_id;
    if (uid != CCORD_ID_NONE) ui_user_toggle_ignore(ui, uid);
    return;
  }
  if (it.builtin == MENU_BUILTIN_VIEW_PROFILE) {
    ccord_id pg = (ctx.guild_id != CCORD_ID_NONE && ctx.guild_id != CCORD_DM_GUILD_ID) ? ctx.guild_id : CCORD_ID_NONE;
    ui_open_user_profile_modal(ui, ctx.user_id, pg);
    return;
  }
  if (it.builtin == MENU_BUILTIN_LOCAL_MUTE) {
    if (ctx.user_id != CCORD_ID_NONE && ui->d.store) {
      ccord_error err;
      (void)ccord_store_set_user_local_mute(ui->d.store, ctx.user_id, it.arg != 0, &err);
      ui->dirty = true;
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_WATCH_STREAM) {
    if (ui->d.session && ctx.user_id != CCORD_ID_NONE) {
      ccord_id ch_id = (ccord_id)it.arg != CCORD_ID_NONE ? (ccord_id)it.arg : ctx.channel_id;
      const ccord_watched_stream *ws = ccord_session_watched_stream(ui->d.session);
      ccord_error err;
      if (ws && ws->active && ws->user_id == ctx.user_id) {
        (void)ccord_session_stop_watching(ui->d.session, &err);
      } else if (ch_id != CCORD_ID_NONE) {
        (void)ccord_session_watch_stream(ui->d.session, ctx.guild_id, ch_id, ctx.user_id, &err);
      }
      ui->dirty = true;
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_ADD_DM_RECIPIENT) {
    ui->modal_kind = UI_MODAL_ADD_DM_RECIPIENT;
    ui->modal_guild_id = CCORD_DM_GUILD_ID;
    ui->modal_target_id = ctx.channel_id;
    ui->modal_focus_field = 0;
    ui->new_dm_recipient_count = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    ccord_text_edit_clear(&ui->modal_edit2);
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_EDIT_GROUP_DM) {
    ui->modal_kind = UI_MODAL_EDIT_GROUP_DM;
    ui->modal_guild_id = CCORD_DM_GUILD_ID;
    ui->modal_target_id = ctx.channel_id;
    ui->modal_type_sel = 0;
    ui->modal_focus_field = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
    if (ch && ch->name && ch->name[0]) {
      ccord_error err;
      (void)ccord_text_edit_insert(&ui->modal_edit1, ch->name, strlen(ch->name), &err);
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_REMOVE_DM_RECIPIENT) {
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_remove_dm_recipient(&req, ctx.channel_id, ctx.user_id, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot remove member: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_EDIT_CHANNEL) {
    if (ui_channel_settings_open(ui, ctx.channel_id)) return;
    ui->modal_kind = UI_MODAL_EDIT_CHANNEL;
    ui->modal_guild_id = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ui->modal_target_id = ctx.channel_id;
    ccord_text_edit_clear(&ui->modal_edit1);
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
    if (ch && ch->name) {
      ccord_error err;
      (void)ccord_text_edit_insert(&ui->modal_edit1, ch->name, strlen(ch->name), &err);
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_CLONE_CHANNEL) {
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
    if (!ch) return;
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ch->guild_id;
    uint8_t dtype = ch->discord_type != CCORD_DISCORD_CHANNEL_UNKNOWN
                      ? ch->discord_type
                      : (ch->kind == CCORD_CHANNEL_VOICE ? 2u : (ch->kind == CCORD_CHANNEL_CATEGORY ? 4u : 0u));
    size_t nows = 0;
    const ccord_overwrite *ows = ccord_store_channel_overwrites(ui->d.store, ch->id, &nows);
    const ccord_forum_meta *fm = ccord_discord_type_is_forum(dtype) ? ccord_store_forum_meta(ui->d.store, ch->id) : nullptr;
    ccord_admin_channel_clone cl = {
      .name = ch->name ? ch->name : "channel",
      .discord_type = dtype,
      .parent_id = ch->parent_id,
      .topic = ch->topic,
      .nsfw = ch->nsfw,
      .slowmode_s = ch->rate_limit_per_user,
      .bitrate = ch->bitrate,
      .user_limit = ch->user_limit,
      .default_auto_archive_duration = ch->default_auto_archive_duration,
      .overwrites = ows,
      .overwrite_count = nows,
      .available_tags = fm ? fm->tags : nullptr,
      .available_tag_count = fm ? fm->tag_count : 0,
    };
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_clone_channel(&req, gid, &cl, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't clone channel: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_VOICE_MUTE) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_set_voice_mute(&req, gid, ctx.user_id, it.arg != 0, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't change server mute: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_VOICE_DEAF) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_set_voice_deaf(&req, gid, ctx.user_id, it.arg != 0, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't change server deafen: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_VOICE_MOVE || it.builtin == MENU_BUILTIN_VOICE_DISCONNECT) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ccord_id dst = it.builtin == MENU_BUILTIN_VOICE_MOVE ? (ccord_id)it.arg : CCORD_ID_NONE;
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_move_voice_member(&req, gid, ctx.user_id, dst, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't move voice member: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_CREATE_CHANNEL) {
    ui->modal_kind = UI_MODAL_CREATE_CHANNEL;
    ui->modal_guild_id = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
    ui->modal_target_id = ch ? (ch->kind == CCORD_CHANNEL_CATEGORY ? ch->id : ch->parent_id) : CCORD_ID_NONE;
    ui->modal_type_sel = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_CREATE_THREAD) {
    ui->modal_kind = UI_MODAL_CREATE_THREAD;
    ui->modal_guild_id = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ui->modal_target_id = ctx.channel_id;
    ui->modal_msg_id = (ctx.target == CCORD_MENU_MESSAGE) ? ctx.message_id : CCORD_ID_NONE;
    ccord_text_edit_clear(&ui->modal_edit1);
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_VIEW_REACTIONS) {
    const ccord_message *m = ccord_store_find_message(ui->d.store, ctx.channel_id, ctx.message_id);
    if (!m) return;
    ui->reactions_open = true;
    ui->reactions_channel_id = ctx.channel_id;
    ui->reactions_msg_id = ctx.message_id;
    ui->reactions_tab = 0;
    ui->reactions_scroll = 0.0f;
    if (ui->d.session && m->reactions) {
      for (size_t ri = 0; ri < m->reaction_kind_count; ri++) {
        if (m->reactions[ri].emoji && m->reactions[ri].count > 0) {
          ccord_error rerr;
          (void)ccord_session_request_reaction_users(ui->d.session, ctx.channel_id, ctx.message_id,
                                                     m->reactions[ri].emoji, 50u, &rerr);
          break;
        }
      }
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_DELETE_CHANNEL) {
    const ccord_channel *ch = ctx.channel_id != CCORD_ID_NONE ? ccord_store_channel(ui->d.store, ctx.channel_id) : nullptr;
    if (!ch) return;
    char label[CCORD_NAME_MAX_BYTES + 2];
    (void)snprintf(label, sizeof label, "%s%s", ch->kind == CCORD_CHANNEL_TEXT ? "#" : "", ch->name ? ch->name : "");
    ui_confirm_open(ui, CCORD_ADMIN_DELETE_CHANNEL, ch->guild_id, ch->id, label);
    return;
  }
  if (it.builtin == MENU_BUILTIN_SERVER_SETTINGS) {
    ui_server_settings_open(ui, ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store));
    return;
  }
  if (it.builtin == MENU_BUILTIN_GUILD_EVENTS) {
    ui_scheduled_events_open(ui, ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store));
    return;
  }
  if (it.builtin == MENU_BUILTIN_LEAVE_GUILD) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    const ccord_guild *gd = ccord_store_guild(ui->d.store, gid);
    if (!gd) return;
    ui_confirm_open(ui, CCORD_ADMIN_LEAVE_GUILD, gid, CCORD_ID_NONE, gd->name && gd->name[0] ? gd->name : "this server");
    return;
  }
  if (it.builtin == MENU_BUILTIN_CHANGE_NICK) {
    ui->modal_kind = UI_MODAL_CHANGE_NICK;
    ui->modal_guild_id = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ui->modal_target_id = ctx.user_id;
    ccord_text_edit_clear(&ui->modal_edit1);
    const ccord_member *m = ccord_store_find_member(ui->d.store, ui->modal_guild_id, ctx.user_id);
    if (m && m->nick[0] != '\0') {
      ccord_error err;
      (void)ccord_text_edit_insert(&ui->modal_edit1, m->nick, strlen(m->nick), &err);
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_TIMEOUT_MEMBER || it.builtin == MENU_BUILTIN_KICK_MEMBER ||
      it.builtin == MENU_BUILTIN_BAN_MEMBER) {
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ccord_admin_op op = it.builtin == MENU_BUILTIN_TIMEOUT_MEMBER ? CCORD_ADMIN_TIMEOUT
                        : it.builtin == MENU_BUILTIN_KICK_MEMBER  ? CCORD_ADMIN_KICK
                                                                  : CCORD_ADMIN_BAN;
    ui_confirm_open(ui, op, gid, ctx.user_id, ui_member_display_name(ui, gid, ctx.user_id));
    return;
  }
  if (it.builtin == MENU_BUILTIN_REMOVE_TIMEOUT) {   // lifting a timeout is not destructive: no dialog
    ccord_id gid = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_timeout(&req, gid, ctx.user_id, ui->now_unix_ms, 0, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't remove the timeout: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_MARK_READ) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_id guild = CCORD_ID_NONE, channel = CCORD_ID_NONE;
    if (ctx.target == CCORD_MENU_CHANNEL) {
      const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
      if (!ch) return;
      guild = ch->guild_id;
      channel = ch->id;
    } else {
      guild = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    }
    size_t acked = 0;
    ccord_error err;
    if (ccord_session_mark_read(ui->d.session, guild, channel, &acked, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't mark as read: %s", err.message);
      return;
    }
    if (ui->new_marker_active) {
      const ccord_channel *mc = ccord_store_channel(ui->d.store, ui->new_marker_channel);
      if (!mc || mc->unread_count == 0) ui_dismiss_new_marker(ui, false);
    }
    size_t left = ccord_store_read_targets(ui->d.store, guild, channel, ui->now_unix_ms, nullptr, 0);
    if (left > 0) {   // a very busy server is taken in steps so one click cannot flood Discord
      ui_notice(ui, "Marked %zu channels as read; %zu more are left. Choose Mark As Read again for them.", acked, left);
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_FOLDER_MARK_READ) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    ccord_id fid = ui->menu_folder_id;
    size_t ng = ccord_store_guild_count(ui->d.store);
    size_t total_acked = 0;
    for (size_t gi = 0; gi < ng && total_acked < CCORD_MARK_READ_MAX; gi++) {
      const ccord_guild *gd = ccord_store_guild_at(ui->d.store, gi);
      if (!gd || gd->folder_id != fid) continue;
      if (ccord_store_read_targets(ui->d.store, gd->id, CCORD_ID_NONE, ui->now_unix_ms, nullptr, 0) == 0) continue;
      size_t acked = 0;
      ccord_error err;
      if (ccord_session_mark_read(ui->d.session, gd->id, CCORD_ID_NONE, &acked, &err) == CCORD_OK) {
        total_acked += acked;
      }
    }
    if (ui->new_marker_active) {
      const ccord_channel *mc = ccord_store_channel(ui->d.store, ui->new_marker_channel);
      if (!mc || mc->unread_count == 0) ui_dismiss_new_marker(ui, false);
    }
    return;
  }
  if (it.builtin == MENU_BUILTIN_FOLDER_SETTINGS) {
    ccord_id fid = ui->menu_folder_id;
    if (fid == CCORD_ID_NONE) return;
    ui->modal_kind = UI_MODAL_FOLDER_SETTINGS;
    ui->modal_target_id = fid;
    ui->modal_focus_field = 0;
    ui->modal_type_sel = 0;
    ccord_text_edit_clear(&ui->modal_edit1);
    ccord_text_edit_clear(&ui->modal_edit2);
    size_t ng = ccord_store_guild_count(ui->d.store);
    for (size_t gi = 0; gi < ng; gi++) {
      const ccord_guild *gd = ccord_store_guild_at(ui->d.store, gi);
      if (!gd || gd->folder_id != fid) continue;
      ccord_error err;
      if (gd->folder_name[0] != '\0') {
        (void)ccord_text_edit_insert(&ui->modal_edit1, gd->folder_name, strlen(gd->folder_name), &err);
      }
      if (gd->folder_color != 0) {
        char hex[16];
        int hn = snprintf(hex, sizeof hex, "#%06X", (unsigned)(gd->folder_color & 0xFFFFFFu));
        if (hn > 0) (void)ccord_text_edit_insert(&ui->modal_edit2, hex, (size_t)hn, &err);
        static const uint32_t k_presets[6] = {0u, 0x5865F2u, 0x57F287u, 0xFEE75Cu, 0xEB459Eu, 0xED4245u};
        for (int pi = 1; pi < 6; pi++) {
          if ((gd->folder_color & 0xFFFFFFu) == k_presets[pi]) {
            ui->modal_type_sel = pi;
            break;
          }
        }
      }
      break;
    }
    ui->dirty = true;
    return;
  }
  if (it.builtin == MENU_BUILTIN_FOLDER_UNGROUP) {
    ccord_id fid = ui->menu_folder_id;
    if (fid == CCORD_ID_NONE) return;
    ccord_guild_folder_item items[CCORD_MAX_GUILDS];
    size_t ng = ccord_store_guild_count(ui->d.store);
    if (ng > CCORD_MAX_GUILDS) ng = CCORD_MAX_GUILDS;
    size_t count = 0;
    for (size_t gi = 0; gi < ng; gi++) {
      const ccord_guild *gd = ccord_store_guild_at(ui->d.store, gi);
      if (!gd) continue;
      ccord_guild_folder_item it_f = {.guild_id = gd->id};
      if (gd->folder_id != fid) {
        it_f.folder_id = gd->folder_id;
        it_f.folder_color = gd->folder_color;
        (void)snprintf(it_f.folder_name, sizeof it_f.folder_name, "%s", gd->folder_name);
      }
      items[count++] = it_f;
    }
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_set_guild_folders(&req, items, count, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't ungroup folder: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_MUTE || it.builtin == MENU_BUILTIN_UNMUTE || it.builtin == MENU_BUILTIN_NOTIFY_LEVEL ||
      it.builtin == MENU_BUILTIN_SUPPRESS_EVERYONE || it.builtin == MENU_BUILTIN_SUPPRESS_ROLES) {
    ccord_id guild = CCORD_ID_NONE, channel = CCORD_ID_NONE;
    if (ctx.target == CCORD_MENU_CHANNEL) {
      const ccord_channel *ch = ccord_store_channel(ui->d.store, ctx.channel_id);
      if (!ch) return;
      guild = ch->guild_id;
      channel = ch->id;
    } else {
      guild = ctx.guild_id != CCORD_ID_NONE ? ctx.guild_id : ccord_store_selected_guild(ui->d.store);
    }
    ccord_admin_notify_edit e = CCORD_ADMIN_NOTIFY_EDIT_INIT;
    e.channel_id = channel;
    switch (it.builtin) {
      case MENU_BUILTIN_MUTE: e.muted = 1; e.mute_duration_ms = it.arg; break;
      case MENU_BUILTIN_UNMUTE: e.muted = 0; break;
      case MENU_BUILTIN_NOTIFY_LEVEL: e.level = (int8_t)it.arg; break;
      case MENU_BUILTIN_SUPPRESS_EVERYONE: e.suppress_everyone = (int8_t)it.arg; break;
      default: e.suppress_roles = (int8_t)it.arg; break;
    }
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_set_notify(&req, guild, &e, ui->now_unix_ms, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't change the notification settings: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (it.builtin == MENU_BUILTIN_COPY_USER_ID || it.builtin == MENU_BUILTIN_COPY_CHANNEL_ID ||
      it.builtin == MENU_BUILTIN_COPY_ID) {
    ccord_id target_id = CCORD_ID_NONE;
    if (it.builtin == MENU_BUILTIN_COPY_USER_ID) {
      target_id = it.arg != 0 ? (ccord_id)it.arg : ctx.user_id;
    } else if (it.builtin == MENU_BUILTIN_COPY_CHANNEL_ID) {
      target_id = it.arg != 0 ? (ccord_id)it.arg : ctx.channel_id;
    } else {
      target_id = ctx.message_id != CCORD_ID_NONE ? ctx.message_id :
                  (ctx.user_id != CCORD_ID_NONE ? ctx.user_id :
                  (ctx.channel_id != CCORD_ID_NONE ? ctx.channel_id : ctx.guild_id));
    }
    if (target_id != CCORD_ID_NONE && ui->d.host.clipboard_set) {
      char idbuf[24];
      ccord_id_format(target_id, idbuf);
      ui->d.host.clipboard_set(ui->d.host.ud, idbuf);
      ui_notice(ui, it.builtin == MENU_BUILTIN_COPY_USER_ID ? "Copied User ID"
                  : (it.builtin == MENU_BUILTIN_COPY_CHANNEL_ID ? "Copied Channel ID" : "Copied ID"));
    }
    return;
  }
  if (!ui->d.contrib || !it.owner || !it.item_id) return;
  ccord_error err;
  if (ccord_contrib_activate_menu(ui->d.contrib, it.owner, it.item_id, &ctx, &err) != CCORD_OK) {
    ui_notice(ui, "%s: %s", it.label ? it.label : "Menu item", err.message);
  }
}

// Returns true when the event was consumed (always while the menu is open).
bool ui_menu_handle_mouse(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->menu.open) return false;
  if (ev->type == CCORD_IN_WHEEL) {
    // The panel that sits under the pointer scrolls; rows outside its visible interior stop being live.
    if (ui->menu.open_sub >= 0 && cm_in_sub_box(ui, ev->x, ev->y)) {
      float full = cm_sub_content_height(ui);
      float visible = cm_sub_box(ui).h;
      float max_s = full > visible ? full - visible : 0.0f;
      ccord_scroller_scroll(&ui->menu_sub_glide, &ui->menu.sub_scroll_y, -ev->wheel * M(context_menu_item_height), max_s,
                            ui_wheel_direct(ui, ev));
    } else {
      float max_s = ui->menu.content_h > ui->menu.h ? ui->menu.content_h - ui->menu.h : 0.0f;
      ccord_scroller_scroll(&ui->menu_glide, &ui->menu.scroll_y, -ev->wheel * M(context_menu_item_height), max_s,
                            ui_wheel_direct(ui, ev));
    }
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    if (ui->mobile_layout) return true;   // mobile uses explicit tap drill-down, not hover fly-outs
    int h = cm_hit_item(ui, ev->x, ev->y);
    if (h >= 0 && ui->menu.items[h].parent < 0) {
      // Pointing at a parent opens its fly-out; pointing at any other top-level item closes it. Over a gap or the
      // fly-out's own padding the current one stays, so the pointer can cross to it.
      int want = (ui->menu.items[h].has_sub && !ui->menu.items[h].disabled) ? h : -1;
      if (want != ui->menu.open_sub) {
        ui->menu.open_sub = want;
        ui->menu.sub_scroll_y = 0.0f;
        ccord_scroller_cancel(&ui->menu_sub_glide);   // a wheel glide of the old fly-out does not carry over
        cm_ensure_invite_submenu_populated(ui);
        ui->dirty = true;
      }
    }
    if (h != ui->menu.hover) {
      ui->menu.hover = h;
      ui->dirty = true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN) {
    if (ev->button == 1 && ui->mobile_layout && ui->mobile_menu_in_sub && ui->menu.open_sub >= 0) {
      ccord_rect back_r = cm_mobile_sheet_back_rect(ui);
      if (ui_in_rect(ev->x, ev->y, back_r)) {
        ui->mobile_menu_in_sub = false;
        ui->menu.open_sub = -1;
        ui->menu.sub_scroll_y = 0.0f;
        ccord_scroller_cancel(&ui->menu_sub_glide);
        ui->dirty = true;
        return true;
      }
    }
    int h = ev->button == 1 ? cm_hit_item(ui, ev->x, ev->y) : -1;
    if (ev->button == 1 && h >= 0 && ui->menu.items[h].disabled) return true;
    if (ev->button == 1 && h >= 0 && ui->menu.items[h].has_sub) {   // a parent only opens its fly-out / sub-sheet
      ui->menu.open_sub = h;
      ui->menu.sub_scroll_y = 0.0f;
      if (ui->mobile_layout) ui->mobile_menu_in_sub = true;
      ccord_scroller_cancel(&ui->menu_sub_glide);   // a wheel glide of the old fly-out does not carry over
      cm_ensure_invite_submenu_populated(ui);
      ui->dirty = true;
      return true;
    }
    if (ev->button == 1 && h < 0 && (cm_in_sub_box(ui, ev->x, ev->y) || (ui->mobile_layout && cm_in_menu_interior(ui, ev->x, ev->y)))) {
      return true;   // padding / handle inside the sheet
    }
    // Take copies: activation can change the registry, and closing frees the item strings.
    if (h >= 0) {
      size_t idx = (size_t)h;
      activate(ui, idx);
    }
    ui_menu_close(ui);
    return true;
  }
  return true;
}

// One panel (the menu, or a fly-out): shadow, border, body, then the items whose parent is `parent`.
// Computes incremental y offsets in a single O(N) pass.
static void draw_panel(ccord_ui *ui, ccord_renderer *r, ccord_rect box, int parent, const ccord_text_style *st)
{
  (void)st;   // row text comes from uis_menu_row
  uis_popover_frame(ui, r, box);
  // Rows scrolled past either edge must not paint over the panel border or the window.
  ccord_push_clip(r, box);
  float pad = M(context_menu_pad);
  float top_pad = pad;
  float scroll = ui->menu.scroll_y;
  if (parent < 0) {
    if (ui->mobile_layout) top_pad += SHEET_HANDLE_H;
  } else {
    if (ui->mobile_layout) top_pad += SHEET_HANDLE_H + SHEET_BACK_H;
    scroll = ui->menu.sub_scroll_y;
  }
  float ry = box.y + top_pad - scroll;
  float rx = box.x + pad;
  float rw = box.w - 2.0f * pad;
  for (size_t i = 0; i < ui->menu.count; i++) {
    const menu_item *it = &ui->menu.items[i];
    if (it->parent != parent) continue;
    if (it->sep_before) ry += SEP_H;
    float rh = cm_row_height(ui, it);
    ccord_rect ir = {rx, ry, rw, rh};
    ry += rh;
    if (ir.y + ir.h <= box.y || ir.y >= box.y + box.h) continue;   // fully outside the visible interior
    if (it->sep_before) {
      uis_divider(ui, r, box.x + UIS_SP_2, ir.y - SEP_H * 0.5f - 0.5f, box.w - 2.0f * UIS_SP_2);
    }
    // A parent stays lit while its fly-out is open, so it is clear what the panel beside it belongs to.
    bool lit = ((int)i == ui->menu.hover || (int)i == ui->menu.open_sub) && !it->disabled;
    uis_state mst = {.hover = lit, .disabled = it->disabled};
    if (it->sublabel) {
      uis_menu_row(ui, r, ir, "", mst, it->has_sub, it->checked, it->danger, "mitem", i);
      ccord_color fg = it->danger ? COL(danger) : COL(text_normal);
      if (lit) fg = COL(text_on_brand);
      if (it->disabled) fg = ui_alpha(fg, UIS_DISABLED_FG);
      float fs = uis_tokens_of(ui).fs_control;
      ccord_text_style lts = ui_style(ui, CCORD_FONT_REGULAR, fs, fg);
      ccord_color sub_fg = lit ? ui_alpha(COL(text_on_brand), 0.85f) : COL(text_muted);
      ccord_text_style sts = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_caption, sub_fg);
      float lh = ui_line_h(ui, fs);
      float sh = ui_line_h(ui, sts.size);
      float ty = ir.y + (ir.h - (lh + sh)) * 0.5f;
      float tw = ir.w - 2.0f * UIS_SP_2 - ((it->has_sub || it->checked) ? 18.0f : 0.0f);
      ccord_text_draw_ellipsis(r, it->label, strlen(it->label), ir.x + UIS_SP_2, ty, tw, &lts);
      ccord_text_draw_ellipsis(r, it->sublabel, strlen(it->sublabel), ir.x + UIS_SP_2, ty + lh, tw, &sts);
    } else {
      uis_menu_row(ui, r, ir, it->label, mst, it->has_sub, it->checked, it->danger, "mitem", i);
      if (it->owner && it->owner[0] != '\0' && !it->has_sub && !it->checked) {
        ccord_color tag_fg = lit ? ui_alpha(COL(text_on_brand), 0.72f) : COL(text_muted);
        ccord_text_style pts = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_caption, tag_fg);
        ccord_vec2 ptm = ccord_text_measure(r, "lua", 3, &pts, 0.0f);
        ccord_text_draw(r, "lua", 3, ir.x + ir.w - UIS_SP_2 - ptm.x, ir.y + (ir.h - ptm.y) * 0.5f, &pts, 0.0f);
      }
    }
  }
  ccord_pop_clip(r);
}

// A subtle 4 px thumb on the panel's right edge while its contents overflow the visible height.
static void draw_menu_scrollbar(ccord_ui *ui, ccord_renderer *r, ccord_rect box, float content_h, float scroll,
                                ccord_rect *out)
{
  *out = (ccord_rect){0};
  if (content_h <= box.h + 0.5f) return;
  float max_s = content_h - box.h;
  float frac = clampf(scroll / max_s, 0.0f, 1.0f);
  float w = 4.0f;
  float thumb_h = box.h * box.h / content_h;
  if (thumb_h < 20.0f) thumb_h = 20.0f;
  if (thumb_h > box.h - 8.0f) thumb_h = box.h - 8.0f;
  float top = box.y + 4.0f + frac * (box.h - 8.0f - thumb_h);
  ccord_rect thumb = {box.x + box.w - w - 3.0f, top, w, thumb_h};
  *out = thumb;
  ccord_draw_rrect(r, thumb, w * 0.5f, ui_alpha(COL(scrollbar_thumb), 0.9f));
}

void ui_menu_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (ui->menu_replay && ui->menu.open) ui_overlay_restart(ui, UI_OVL_MENU, UI_POPUP_MS);
  ui->menu_replay = false;
  float pres = ui_overlay_presence(ui, UI_OVL_MENU, ui->menu.open, UI_POPUP_MS, UI_POPUP_CLOSE_MS);
  bool fading = !ui->menu.open && ui->menu.count > 0;
  if (!ui->menu.open && (!fading || pres <= UI_PRESENCE_EPS)) {
    if (fading) ui_menu_release(ui);   // the fade is over
    return;
  }
  cm_ensure_invite_submenu_populated(ui);
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) + 1.0f, COL(text_normal));
  float pad = M(context_menu_pad);
  float min_w = M(context_menu_min_width);
  menu_aux *aux = cm_menu_aux_for(ui);
  bool style_changed = !aux || !aux->measured_top || aux->measured_font_size != st.size || aux->measured_pad != pad ||
                       aux->measured_min_w != min_w || aux->measured_mobile != ui->mobile_layout;
  bool need_top = style_changed || aux->measured_count != ui->menu.count;
  bool need_sub = style_changed || !aux->measured_sub || aux->measured_open_sub != ui->menu.open_sub ||
                  aux->measured_sub_count != ui->menu.count;
  float w = aux && !need_top ? aux->cached_w : min_w;
  float sw = aux && !need_sub ? aux->cached_sub_w : min_w;
  if (need_top || need_sub) {
    float maxw = 0.0f, subw = 0.0f;
    for (size_t i = 0; i < ui->menu.count; i++) {
      const menu_item *it = &ui->menu.items[i];
      bool is_top = it->parent < 0;
      bool is_sub = !is_top && it->parent == ui->menu.open_sub;
      if ((is_top && !need_top) || (is_sub && !need_sub) || (!is_top && !is_sub)) continue;
      float tw = ccord_text_measure(r, it->label, strlen(it->label), &st, 0.0f).x;
      if (it->sublabel) {
        float sw0 = ccord_text_measure(r, it->sublabel, strlen(it->sublabel), &st, 0.0f).x;
        if (sw0 > tw) tw = sw0;
      }
      float iw = tw + ((it->has_sub || it->checked) ? 18.0f : ((it->owner && it->owner[0] != '\0') ? 26.0f : 0.0f));
      if (is_top) maxw = iw > maxw ? iw : maxw;
      else subw = iw > subw ? iw : subw;
    }
    if (need_top) {
      w = maxw + 2.0f * pad + 24.0f;
      if (w < min_w) w = min_w;
      if (w > 420.0f) w = 420.0f;
    }
    if (need_sub) {
      sw = subw + 2.0f * pad + 24.0f;
      if (sw < min_w) sw = min_w;
      if (sw > 420.0f) sw = 420.0f;
    }
    if (aux) {
      aux->measured_font_size = st.size;
      aux->measured_pad = pad;
      aux->measured_min_w = min_w;
      aux->measured_mobile = ui->mobile_layout;
      if (need_top) {
        aux->measured_top = true;
        aux->measured_count = ui->menu.count;
        aux->cached_w = w;
      }
      if (need_sub) {
        aux->measured_sub = true;
        aux->measured_open_sub = ui->menu.open_sub;
        aux->measured_sub_count = ui->menu.count;
        aux->cached_sub_w = sw;
      }
    }
  }
  ui->menu.w = w;
  ui->menu.content_h = cm_menu_height(ui);
  ui->menu.h = ui->menu.content_h;
  float max_h = menu_max_h(ui);
  if (ui->menu.h > max_h) ui->menu.h = max_h;
  float max_s = ui->menu.content_h > ui->menu.h ? ui->menu.content_h - ui->menu.h : 0.0f;
  ui->menu.scroll_y = clampf(ui->menu.scroll_y, 0.0f, max_s);
  ui->menu.sub_w = sw;
  ui->menu.sub_content_h = cm_sub_content_height(ui);
  float max_sub_s = ui->menu.sub_content_h > max_h ? ui->menu.sub_content_h - max_h : 0.0f;
  ui->menu.sub_scroll_y = clampf(ui->menu.sub_scroll_y, 0.0f, max_sub_s);
  if (ui->menu.x + w > ui->win_w - MENU_EDGE) ui->menu.x = ui->win_w - w - MENU_EDGE;
  if (ui->menu.x < MENU_EDGE) ui->menu.x = MENU_EDGE;
  if (ui->menu.y + ui->menu.h > ui->win_h - MENU_EDGE) ui->menu.y = ui->win_h - MENU_EDGE - ui->menu.h;
  if (ui->menu.y < MENU_EDGE) ui->menu.y = MENU_EDGE;
  ccord_rect box = {ui->menu.x, ui->menu.y, ui->menu.w, ui->menu.h};
  // The panel follows the menu's presence (it enters and leaves with it). A fly-out opening inside an open menu
  // plays its own entrance; while the menu fades out it goes with the panel.
  // Entrance of this menu (a new menu starts over) or the presence, whichever is further along; a fading menu has
  // only the presence.
  float shown = pres;
  if (ui->menu.open) {
    float enter = ui_enter(ui, "menu", ui->menu.seq, UI_POPUP_MS);
    if (enter > shown) shown = enter;
  }
  if (ui->mobile_layout) {
    // Full-width bottom sheet anchored above safe_insets.bottom with dimmed backdrop scrim & handle bar.
    float sl = ui->safe_insets.left + MENU_EDGE;
    float sr = ui->safe_insets.right + MENU_EDGE;
    float sb_pad = ui->safe_insets.bottom + MENU_EDGE;
    ui->menu.w = fmaxf(ui->win_w - sl - sr, 200.0f);
    ui->menu.sub_w = ui->menu.w;
    bool in_sub = ui->mobile_menu_in_sub && ui->menu.open_sub >= 0 &&
                  (size_t)ui->menu.open_sub < ui->menu.count && cm_child_count(ui, ui->menu.open_sub) > 0;
    float active_content_h = in_sub ? ui->menu.sub_content_h : ui->menu.content_h;
    ui->menu.h = active_content_h > max_h ? max_h : active_content_h;
    ui->menu.x = sl;
    ui->menu.y = ui->win_h - sb_pad - ui->menu.h;
    if (ui->menu.y < ui->safe_insets.top + MENU_EDGE) ui->menu.y = ui->safe_insets.top + MENU_EDGE;
    ccord_rect sheet = {ui->menu.x, ui->menu.y, ui->menu.w, ui->menu.h};
    ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h}, ui_alpha(COL(bg_tertiary), 0.50f * shown));
    ui_overlay_begin(ui, r, UI_OVL_MENU, shown, 24.0f);
    draw_panel(ui, r, sheet, in_sub ? ui->menu.open_sub : -1, &st);
    // Centered grab handle bar at top of sheet.
    ccord_draw_rrect(r, (ccord_rect){sheet.x + (sheet.w - 36.0f) * 0.5f, sheet.y + 8.0f, 36.0f, 4.0f},
                     2.0f, ui_alpha(COL(text_muted), 0.5f));
    if (in_sub) {
      ccord_rect back_r = cm_mobile_sheet_back_rect(ui);
      const char *plbl = ui->menu.items[ui->menu.open_sub].label;
      char blbl[96];
      snprintf(blbl, sizeof blbl, "<  %.80s", plbl ? plbl : "Back");
      ccord_text_style bts = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, COL(text_header));
      float by = back_r.y + (back_r.h - ui_line_h(ui, bts.size)) * 0.5f;
      ccord_text_draw_ellipsis(r, blbl, strlen(blbl), back_r.x + UIS_SP_2, by, back_r.w - 2.0f * UIS_SP_2, &bts);
      uis_divider(ui, r, sheet.x + UIS_SP_2, back_r.y + back_r.h - 1.0f, sheet.w - 2.0f * UIS_SP_2);
      draw_menu_scrollbar(ui, r, sheet, ui->menu.sub_content_h, ui->menu.sub_scroll_y, &ui->menu.sub_scrollbar_rect);
      ui->menu.scrollbar_rect = (ccord_rect){0};
    } else {
      draw_menu_scrollbar(ui, r, sheet, ui->menu.content_h, ui->menu.scroll_y, &ui->menu.scrollbar_rect);
      ui->menu.sub_scrollbar_rect = (ccord_rect){0};
    }
    ui_overlay_end(ui, r);
    return;
  }
  ui_overlay_begin(ui, r, UI_OVL_MENU, shown, 6.0f);
  draw_panel(ui, r, box, -1, &st);
  draw_menu_scrollbar(ui, r, box, ui->menu.content_h, ui->menu.scroll_y, &ui->menu.scrollbar_rect);
  ui_overlay_end(ui, r);
  ccord_rect sb = cm_sub_box(ui);
  if (sb.w > 0.0f) {
    if (fading) {
      ui_overlay_begin(ui, r, UI_OVL_MENU, pres, 4.0f);
    } else {
      ui_popup_begin(ui, r, "menusub", ui->menu.seq * 64u + (uint64_t)ui->menu.open_sub, UI_POPUP_MS, 4.0f);
    }
    draw_panel(ui, r, sb, ui->menu.open_sub, &st);
    draw_menu_scrollbar(ui, r, sb, ui->menu.sub_content_h, ui->menu.sub_scroll_y, &ui->menu.sub_scrollbar_rect);
    if (fading) ui_overlay_end(ui, r);
    else ui_popup_end(r);
  } else {
    ui->menu.sub_scrollbar_rect = (ccord_rect){0};
  }
}

// Test seam for the slice: the open menu's scroll offset and how far it can move (0 when it fits or is closed).
void ui_menu_scroll_debug(const ccord_ui *ui, float *scroll, float *room)
{
  if (scroll) *scroll = ui->menu.scroll_y;
  if (room) *room = ui->menu.open && ui->menu.content_h > ui->menu.h ? ui->menu.content_h - ui->menu.h : 0.0f;
}
