// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

static ccord_id resolve_user_token(const ccord_ui *ui, const char *raw, size_t len)
{
  while (len > 0 && (*raw == ' ' || *raw == '\t')) { raw++; len--; }
  while (len > 0 && (raw[len - 1u] == ' ' || raw[len - 1u] == '\t')) len--;
  if (len == 0 || len >= 96u) return CCORD_ID_NONE;
  char tok[96];
  memcpy(tok, raw, len);
  tok[len] = '\0';
  ccord_id uid = CCORD_ID_NONE;
  if (ccord_id_parse(tok, len, &uid) && uid >= (1ULL << 22)) return uid;
  if (!ui->d.store) return CCORD_ID_NONE;
  size_t nrel = ccord_store_relationship_count(ui->d.store);
  for (size_t i = 0; i < nrel && uid == CCORD_ID_NONE; i++) {
    const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
    const ccord_user *u = rel ? ccord_store_user(ui->d.store, rel->user_id) : nullptr;
    if (u && ((u->name && strcasecmp(u->name, tok) == 0) ||
              (u->handle && strcasecmp(u->handle, tok) == 0) ||
              (u->display_name && strcasecmp(u->display_name, tok) == 0))) {
      uid = u->id;
    }
  }
  size_t ng = ccord_store_guild_count(ui->d.store);
  for (size_t g = 0; g < ng && uid == CCORD_ID_NONE; g++) {
    const ccord_guild *gd = ccord_store_guild_at(ui->d.store, g);
    if (!gd) continue;
    size_t nm = ccord_store_member_count(ui->d.store, gd->id);
    for (size_t i = 0; i < nm && uid == CCORD_ID_NONE; i++) {
      const ccord_member *m = ccord_store_member_at(ui->d.store, gd->id, i);
      const ccord_user *u = m ? ccord_store_user(ui->d.store, m->user_id) : nullptr;
      if (u && ((u->name && strcasecmp(u->name, tok) == 0) ||
                (u->handle && strcasecmp(u->handle, tok) == 0) ||
                (u->display_name && strcasecmp(u->display_name, tok) == 0))) {
        uid = u->id;
      }
    }
  }
  return uid;
}

static void save_profile_modal_note(ccord_ui *ui)
{
  if (!ui || ui->modal_target_id == CCORD_ID_NONE) return;
  const char *note = ccord_text_edit_cstr(&ui->modal_edit1);
  ccord_admin_request req;
  ccord_error err;
  if (ccord_admin_set_user_note(&req, ui->modal_target_id, note, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't save note: %s", err.message);
    return;
  }
  ui_admin_submit(ui, &req);
  ui_notice(ui, "Note saved");
}

static void confirm_run(ccord_ui *ui, const char *reason)
{
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = CCORD_ERR_INVALID;
  size_t n = 0;
  const confirm_choice *choices = dc_confirm_choices(ui->confirm_op, &n);
  int64_t pick = choices && ui->modal_type_sel >= 0 && (size_t)ui->modal_type_sel < n ? choices[ui->modal_type_sel].value : 0;
  switch (ui->confirm_op) {
    case CCORD_ADMIN_DELETE_CHANNEL:
      st = ccord_admin_delete_channel(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    case CCORD_ADMIN_DELETE_ROLE:
      st = ccord_admin_delete_role(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    case CCORD_ADMIN_REVOKE_INVITE:
      st = ccord_admin_revoke_invite(&req, ui->modal_guild_id, ui->confirm_name, reason, &err);
      break;
    case CCORD_ADMIN_KICK:
      st = ccord_admin_kick(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    case CCORD_ADMIN_BAN:
      st = ccord_admin_ban(&req, ui->modal_guild_id, ui->modal_target_id, (int32_t)pick, reason, &err);
      break;
    case CCORD_ADMIN_TIMEOUT:
      st = ccord_admin_timeout(&req, ui->modal_guild_id, ui->modal_target_id, ui->now_unix_ms, pick, reason, &err);
      break;
    case CCORD_ADMIN_LEAVE_GUILD:
      st = ccord_admin_leave_guild(&req, ui->modal_guild_id, &err);
      break;
    case CCORD_ADMIN_DELETE_GUILD:
      st = ccord_admin_delete_guild(&req, ui->modal_guild_id, &err);
      break;
    case CCORD_ADMIN_TRANSFER_OWNERSHIP:
      st = ccord_admin_transfer_ownership(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    case CCORD_ADMIN_DELETE_EMOJI:
      st = ccord_admin_delete_emoji(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    case CCORD_ADMIN_DELETE_STICKER:
      st = ccord_admin_delete_sticker(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    case CCORD_ADMIN_DELETE_SOUNDBOARD:
      st = ccord_admin_delete_soundboard(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    default:
      return;
  }
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't %s: %s", ccord_admin_op_verb(ui->confirm_op), err.message);
    return;
  }
  ui_admin_submit(ui, &req);
}

static void submit_modal(ccord_ui *ui)
{
  if (!ui || ui->modal_kind == UI_MODAL_NONE) return;
  const char *val = ccord_text_edit_cstr(&ui->modal_edit1);
  size_t vlen = ui->modal_edit1.len;
  char name_buf[128];
  snprintf(name_buf, sizeof name_buf, "%s", val ? val : "");
  ui_modal_kind kind = ui->modal_kind;
  ui->modal_kind = UI_MODAL_NONE;
  ui->dirty = true;
  if (kind == UI_MODAL_USER_PROFILE) {
    ui->modal_kind = UI_MODAL_USER_PROFILE;
    save_profile_modal_note(ui);
    return;
  }
  if (kind == UI_MODAL_EDIT_GROUP_DM) {
    ccord_admin_request req;
    ccord_error err;
    ccord_admin_group_dm_edit edit = {
      .name = name_buf,
      .icon_data_uri = (ui->modal_type_sel == 1) ? "" : nullptr,
    };
    if (ccord_admin_edit_group_dm(&req, ui->modal_target_id, &edit, &err) != CCORD_OK) {
      ui->modal_kind = UI_MODAL_EDIT_GROUP_DM;
      ui_notice(ui, "Couldn't edit Group DM: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (kind == UI_MODAL_NEW_DM || kind == UI_MODAL_ADD_DM_RECIPIENT) {
    ccord_id recips[CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX];
    size_t nrec = 0;
    ccord_id self_id = ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;
    for (size_t i = 0; i < ui->new_dm_recipient_count && nrec < CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX; i++) {
      ccord_id uid = ui->new_dm_recipients[i];
      if (uid != CCORD_ID_NONE && uid != self_id) recips[nrec++] = uid;
    }
    if (vlen > 0) {
      const char *p = val;
      while (*p) {
        const char *comma = strchr(p, ',');
        size_t tlen = comma ? (size_t)(comma - p) : strlen(p);
        ccord_id uid = resolve_user_token(ui, p, tlen);
        if (uid == CCORD_ID_NONE && tlen > 0) {
          ui_notice(ui, "Could not find user \"%.*s\"", (int)tlen, p);
          return;
        }
        if (uid != CCORD_ID_NONE && uid != self_id && nrec < CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX) {
          bool dup = false;
          for (size_t k = 0; k < nrec; k++) {
            if (recips[k] == uid) { dup = true; break; }
          }
          if (!dup) recips[nrec++] = uid;
        }
        if (!comma) break;
        p = comma + 1;
      }
    }
    ui->new_dm_recipient_count = 0;
    if (nrec == 0) return;
    if (kind == UI_MODAL_ADD_DM_RECIPIENT) {
      for (size_t i = 0; i < nrec; i++) {
        ccord_admin_request req;
        ccord_error err;
        if (ccord_admin_add_dm_recipient(&req, ui->modal_target_id, recips[i], &err) == CCORD_OK) {
          ui_admin_submit(ui, &req);
        } else {
          ui_notice(ui, "Couldn't add member: %s", err.message);
        }
      }
      return;
    }
    const char *gname = ccord_text_edit_cstr(&ui->modal_edit2);
    if (nrec == 1u && gname[0] == '\0') {
      ui_open_dm_with_user(ui, recips[0]);
      return;
    }
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_create_group_dm(&req, recips, nrec, gname[0] ? gname : nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't create Group DM: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (kind == UI_MODAL_CONFIRM) {   // the text field is an optional audit-log reason (or exact name for delete-guild)
    if (ui->confirm_op == CCORD_ADMIN_DELETE_GUILD && strcmp(name_buf, ui->confirm_name) != 0) {
      ui->modal_kind = UI_MODAL_CONFIRM;
      ui_notice(ui, "Server name did not match \"%s\".", ui->confirm_name);
      return;
    }
    confirm_run(ui, name_buf);
    return;
  }
  if (kind == UI_MODAL_FOLDER_SETTINGS) {
    ccord_id fid = ui->modal_target_id;
    if (fid == CCORD_ID_NONE || !ui->d.store) return;
    uint32_t color = 0u;
    if (!dc_parse_folder_hex_color(ccord_text_edit_cstr(&ui->modal_edit2), &color)) {
      ui->modal_kind = UI_MODAL_FOLDER_SETTINGS;
      ui->modal_focus_field = 1;
      ui_notice(ui, "Folder color must be #RRGGBB");
      return;
    }
    ccord_guild_folder_item items[CCORD_MAX_GUILDS];
    size_t ng = ccord_store_guild_count(ui->d.store);
    if (ng > CCORD_MAX_GUILDS) ng = CCORD_MAX_GUILDS;
    size_t count = 0;
    for (size_t gi = 0; gi < ng; gi++) {
      const ccord_guild *gd = ccord_store_guild_at(ui->d.store, gi);
      if (!gd) continue;
      ccord_guild_folder_item it_f = {.guild_id = gd->id};
      if (gd->folder_id == fid) {
        it_f.folder_id = fid;
        it_f.folder_color = color;
        snprintf(it_f.folder_name, sizeof it_f.folder_name, "%.63s", name_buf);
      } else {
        it_f.folder_id = gd->folder_id;
        it_f.folder_color = gd->folder_color;
        snprintf(it_f.folder_name, sizeof it_f.folder_name, "%s", gd->folder_name);
      }
      items[count++] = it_f;
    }
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_set_guild_folders(&req, items, count, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't save folder settings: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (kind == UI_MODAL_ADD_SERVER) {
    ccord_admin_request req;
    ccord_error err;
    if (ui->modal_type_sel == 0) {
      if (ccord_admin_accept_invite(&req, name_buf, &err) != CCORD_OK) {
        ui->modal_kind = UI_MODAL_ADD_SERVER;
        ui_notice(ui, "Couldn't join server: %s", err.message);
        return;
      }
      if (ui->d.session) ccord_session_clear_invite_preview(ui->d.session);
      ui->discovery_open = false;
      ui_compute_layout(ui);
      ui_admin_submit(ui, &req);
      return;
    }
    if (ccord_admin_create_guild(&req, name_buf, &err) != CCORD_OK) {
      ui->modal_kind = UI_MODAL_ADD_SERVER;
      ui_notice(ui, "Couldn't create server: %s", err.message);
      return;
    }
    ui->discovery_open = false;
    ui_compute_layout(ui);
    ui_admin_submit(ui, &req);
    return;
  }
  if (vlen == 0 && kind != UI_MODAL_CHANGE_NICK && kind != UI_MODAL_USER_NOTE && kind != UI_MODAL_FRIEND_NICK) return;

  if (kind == UI_MODAL_FRIEND_NICK) {
    ui_set_friend_nickname(ui, ui->modal_target_id, val ? val : "");
    return;
  }

  if (kind == UI_MODAL_ADD_FRIEND) {
    ccord_id uid = resolve_user_token(ui, val, vlen);
    if (uid == CCORD_ID_NONE) {
      ui_notice(ui, "Could not find user \"%s\"", val);
      return;
    }
    ui_set_user_relationship(ui, uid, CCORD_RELATIONSHIP_FRIEND);
    return;
  }

  // Everything below asks Discord. Nothing is applied locally: the change shows up when Discord confirms it (the
  // gateway event, or the REST answer), and a refusal becomes a notice that carries Discord's own reason.
  ccord_error err;
  ccord_admin_request req;
  ccord_id sel_g = ui->d.store ? ccord_store_selected_guild(ui->d.store) : CCORD_ID_NONE;
  ccord_id gid = ui->modal_guild_id != CCORD_ID_NONE ? ui->modal_guild_id : sel_g;

  if (kind == UI_MODAL_CREATE_CHANNEL) {
    uint8_t type = ui->modal_type_sel == 1 ? 2u : 0u;
    if (ccord_admin_create_channel(&req, gid, name_buf, type, ui->modal_target_id, nullptr, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't create the channel: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }

  if (kind == UI_MODAL_EDIT_CHANNEL) {
    const ccord_channel *existing = ccord_store_channel(ui->d.store, ui->modal_target_id);
    if (!existing) return;
    if (existing->name && strcmp(existing->name, name_buf) == 0) return;   // nothing changed: no request
    ccord_admin_channel_edit edit = CCORD_ADMIN_CHANNEL_EDIT_INIT;
    edit.name = name_buf;
    if (ccord_admin_edit_channel(&req, existing->guild_id, existing->id, &edit, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't edit the channel: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }

  if (kind == UI_MODAL_CHANGE_NICK) {
    ccord_id self = ccord_store_self_id(ui->d.store);
    ccord_id uid = ui->modal_target_id != CCORD_ID_NONE ? ui->modal_target_id : self;
    const ccord_member *existing = ccord_store_find_member(ui->d.store, gid, uid);
    if (existing && strcmp(existing->nick, name_buf) == 0) return;
    if (ccord_admin_set_nick(&req, gid, uid, self, name_buf, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't change the nickname: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }

  if (kind == UI_MODAL_USER_NOTE) {
    if (vlen > CCORD_USER_NOTE_MAX_BYTES) {
      ui_notice(ui, "Note is too long (max %d characters)", CCORD_USER_NOTE_MAX_BYTES);
      return;
    }
    if (ccord_admin_set_user_note(&req, ui->modal_target_id, val ? val : "", &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't save the note: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    ui_notice(ui, "Note saved");
    return;
  }

  if (kind == UI_MODAL_CREATE_THREAD) {
    ccord_id parent = ui->modal_target_id != CCORD_ID_NONE ? ui->modal_target_id : ccord_store_selected_channel(ui->d.store);
    ccord_id msg_id = ui->modal_msg_id;
    ui->modal_msg_id = CCORD_ID_NONE;
    ccord_status st = (msg_id != CCORD_ID_NONE)
                        ? ccord_admin_create_thread_from_message(&req, gid, parent, msg_id, name_buf, &err)
                        : ccord_admin_create_thread(&req, gid, parent, name_buf, &err);
    if (st != CCORD_OK) {
      ui_notice(ui, "Couldn't create the thread: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
}

static void submit_select_dropdown(ccord_ui *ui, const char *const *vals, size_t nvals)
{
  if (!ui || !ui->select_target) return;
  uint8_t min_v = ui->select_target->min_values;
  uint8_t max_v = ui->select_target->max_values > 0 ? ui->select_target->max_values : 1u;
  if (nvals < (size_t)min_v) {
    ui_notice(ui, "Select at least %u option%s", (unsigned)min_v, min_v == 1u ? "" : "s");
    return;
  }
  if (nvals > (size_t)max_v) {
    ui_notice(ui, "You can select at most %u option%s", (unsigned)max_v, max_v == 1u ? "" : "s");
    return;
  }
  ui_select_target target = *ui->select_target;
  ui->select_open = false;
  ui->dirty = true;
  if (!ui->d.session) {
    ui_close_select_dropdown(ui);
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_interaction_request req = {
    .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
    .application_id = target.application_id,
    .channel_id = target.channel_id,
    .message_id = target.message_id,
    .message_flags = target.message_flags,
    .component_type = (uint8_t)target.kind,
    .custom_id = target.custom_id,
    .values = vals,
    .value_count = nvals,
  };
  ccord_error err;
  if (ccord_session_send_interaction(ui->d.session, &req, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't submit selection: %s", err.message);
  }
  ui_close_select_dropdown(ui);
}

static void submit_interaction_modal(ccord_ui *ui)
{
  if (!ui || ui->modal_kind != UI_MODAL_INTERACTION || !ui->inter_modal || !ui->inter_modal_edits) return;
  for (size_t i = 0; i < ui->inter_modal->field_count; i++) {
    const ccord_modal_field *fld = &ui->inter_modal->fields[i];
    size_t len = ui->inter_modal_edits[i].len;
    const char *lbl = fld->label[0] ? fld->label : "Field";
    if (fld->required && len == 0) {
      ui->inter_modal_focus = i;
      ui_notice(ui, "%s is required", lbl);
      ui->dirty = true;
      return;
    }
    if (len > 0 && fld->min_length > 0 && len < (size_t)fld->min_length) {
      ui->inter_modal_focus = i;
      ui_notice(ui, "%s must be at least %u characters", lbl, (unsigned)fld->min_length);
      ui->dirty = true;
      return;
    }
    if (fld->max_length > 0 && len > (size_t)fld->max_length) {
      ui->inter_modal_focus = i;
      ui_notice(ui, "%s must be at most %u characters", lbl, (unsigned)fld->max_length);
      ui->dirty = true;
      return;
    }
  }
  const char *fids[CCORD_RICH_MAX_COMPONENT_ROWS];
  const char *fvals[CCORD_RICH_MAX_COMPONENT_ROWS];
  for (size_t i = 0; i < ui->inter_modal->field_count; i++) {
    fids[i] = ui->inter_modal->fields[i].custom_id;
    fvals[i] = ccord_text_edit_cstr(&ui->inter_modal_edits[i]);
  }
  ccord_interaction_request req = {
    .type = CCORD_INTERACTION_MODAL_SUBMIT,
    .application_id = ui->inter_modal->application_id,
    .guild_id = ui->inter_modal->guild_id,
    .channel_id = ui->inter_modal->channel_id != CCORD_ID_NONE
                    ? ui->inter_modal->channel_id
                    : ccord_store_selected_channel(ui->d.store),
    .custom_id = ui->inter_modal->custom_id,
    .modal_id = ui->inter_modal->id,
    .field_ids = fids,
    .field_values = fvals,
    .field_count = ui->inter_modal->field_count,
  };
  ui->modal_kind = UI_MODAL_NONE;
  ui->dirty = true;
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_error err;
  if (ccord_session_send_interaction(ui->d.session, &req, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't submit form: %s", err.message);
  }
}

bool ui_overlays_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev) return false;

  // M5 voice surfaces own every event while their borrowed modal is open.
  if (ui_call_prompt_active(ui)) return ui_call_prompt_handle(ui, ev);
  if (ui_dave_modal_active(ui)) return ui_dave_modal_handle(ui, ev);
  if (ui_scheduled_events_active(ui)) return ui_scheduled_events_handle(ui, ev);
  if (ui_forward_picker_active(ui)) return ui_forward_picker_handle(ui, ev);
  if (ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION) return ui_member_verification_handle(ui, ev);
  if (ui_activity_shelf_active(ui)) return ui_activity_shelf_handle(ui, ev);

  if (ui->lightbox_open) {
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_ESCAPE) {
        ui_close_lightbox(ui);
        return true;
      }
      if (ev->key == CCORD_KEY_LEFT && ui->lightbox_urls && ui->lightbox_count > 1u) {
        ui->lightbox_index = (ui->lightbox_index + ui->lightbox_count - 1u) % ui->lightbox_count;
        ui_url_copy(ui->lightbox_url, sizeof ui->lightbox_url, ui->lightbox_urls[ui->lightbox_index]);
        ui->lightbox_zoom = 1.0f;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_RIGHT && ui->lightbox_urls && ui->lightbox_count > 1u) {
        ui->lightbox_index = (ui->lightbox_index + 1u) % ui->lightbox_count;
        ui_url_copy(ui->lightbox_url, sizeof ui->lightbox_url, ui->lightbox_urls[ui->lightbox_index]);
        ui->lightbox_zoom = 1.0f;
        ui->dirty = true;
        return true;
      }
      return true;
    }
    if (ev->type == CCORD_IN_TEXT) {
      if (ev->text[0] == '+' || ev->text[0] == '=') {
        ui->lightbox_zoom = clampf(ui->lightbox_zoom * 1.25f, 0.5f, 4.0f);
        ui->dirty = true;
      } else if (ev->text[0] == '-') {
        ui->lightbox_zoom = clampf(ui->lightbox_zoom / 1.25f, 0.5f, 4.0f);
        ui->dirty = true;
      } else if (ev->text[0] == '0') {
        ui->lightbox_zoom = 1.0f;
        ui->dirty = true;
      }
      return true;
    }
    if (ev->type == CCORD_IN_WHEEL) {
      if (ev->wheel > 0.0f) {
        ui->lightbox_zoom = clampf(ui->lightbox_zoom * 1.25f, 0.5f, 4.0f);
        ui->dirty = true;
      } else if (ev->wheel < 0.0f) {
        ui->lightbox_zoom = clampf(ui->lightbox_zoom / 1.25f, 0.5f, 4.0f);
        ui->dirty = true;
      }
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
      if (ui->lightbox_open_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_open_btn)) {
        (void)ui_activate_link(ui, ui->lightbox_url);
        return true;
      }
      if (ui->lightbox_zoom_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_zoom_btn)) {
        ui->lightbox_zoom = 1.0f;
        ui->dirty = true;
        return true;
      }
      if (ui->lightbox_prev_btn.w > 0.0f && ui->lightbox_urls && ui->lightbox_count > 1u &&
          ui_in_rect(ev->x, ev->y, ui->lightbox_prev_btn)) {
        ui->lightbox_index = (ui->lightbox_index + ui->lightbox_count - 1u) % ui->lightbox_count;
        ui_url_copy(ui->lightbox_url, sizeof ui->lightbox_url, ui->lightbox_urls[ui->lightbox_index]);
        ui->lightbox_zoom = 1.0f;
        ui->dirty = true;
        return true;
      }
      if (ui->lightbox_next_btn.w > 0.0f && ui->lightbox_urls && ui->lightbox_count > 1u &&
          ui_in_rect(ev->x, ev->y, ui->lightbox_next_btn)) {
        ui->lightbox_index = (ui->lightbox_index + 1u) % ui->lightbox_count;
        ui_url_copy(ui->lightbox_url, sizeof ui->lightbox_url, ui->lightbox_urls[ui->lightbox_index]);
        ui->lightbox_zoom = 1.0f;
        ui->dirty = true;
        return true;
      }
      if (ui->lightbox_close_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_close_btn)) {
        ui_close_lightbox(ui);
        return true;
      }
      if (ui->lightbox_img_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_img_rect)) {
        if (ev->clicks >= 2) {
          ui->lightbox_zoom = fabsf(ui->lightbox_zoom - 1.0f) < 0.05f ? 2.0f : 1.0f;
          ui->dirty = true;
        }
        return true;
      }
      ui_close_lightbox(ui);
      return true;
    }
    return true;
  }

  if (ui->quick_switcher_open) {
    dc_populate_quick_switcher(ui);
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_ESCAPE) {
        ui->quick_switcher_open = false;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_DOWN && ui->nquick_switcher_items > 0) {
        ui->quick_switcher_sel = (ui->quick_switcher_sel + 1) % ui->nquick_switcher_items;
        ccord_scroller_cancel(&ui->quick_switcher_glide);   // the selection's follow-scroll must not fight a wheel glide
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_UP && ui->nquick_switcher_items > 0) {
        ui->quick_switcher_sel = (ui->quick_switcher_sel + ui->nquick_switcher_items - 1) % ui->nquick_switcher_items;
        ccord_scroller_cancel(&ui->quick_switcher_glide);   // the selection's follow-scroll must not fight a wheel glide
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_RETURN || ev->key == CCORD_KEY_TAB) {
        dc_activate_quick_switcher_item(ui, ui->quick_switcher_sel);
        return true;
      }
      if (ev->key == CCORD_KEY_BACKSPACE) {
        ccord_text_edit_backspace(&ui->quick_switcher_edit);
        ui_quick_switcher_reset_list(ui);
        ui->dirty = true;
        return true;
      }
      return true;
    }
    if (ev->type == CCORD_IN_TEXT && ev->text[0] != '\0') {
      ccord_error err;
      (void)ccord_text_edit_insert(&ui->quick_switcher_edit, ev->text, strlen(ev->text), &err);
      ui_quick_switcher_reset_list(ui);
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_WHEEL) {
      ccord_rect m = ui_quick_switcher_rect(ui);
      ccord_rect lview = {m.x + 12.0f, m.y + 66.0f, m.w - 24.0f, m.h - 76.0f};
      float content_h = (float)ui->nquick_switcher_items * 30.0f;
      float max_s = maxf(content_h - lview.h, 0.0f);
      ccord_scroller_scroll(&ui->quick_switcher_glide, &ui->quick_switcher_scroll, -ev->wheel * 30.0f, max_s,
                            ui_wheel_direct(ui, ev));
      ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
      ccord_rect m = ui_quick_switcher_rect(ui);
      if (!ui_in_rect(ev->x, ev->y, m)) {
        ui->quick_switcher_open = false;
        ui->dirty = true;
        return true;
      }
      for (size_t i = 0; i < ui->nquick_switcher_items; i++) {
        if (ui_in_rect(ev->x, ev->y, ui->quick_switcher_items[i].rect)) {
          dc_activate_quick_switcher_item(ui, i);
          return true;
        }
      }
      return true;
    }
    return true;
  }

  if (ui->select_open) {
    if (!ui->select_target || !ui->select_opts) {
      ui_close_select_dropdown(ui);
      return true;
    }
    bool is_multi = ui->select_target->max_values > 1u;
    if (ev->type == CCORD_IN_WHEEL) {
      ui->select_scroll = clampf(ui->select_scroll - ev->wheel * 36.0f, 0.0f, ui->select_max);
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_ESCAPE) {
        ui_close_select_dropdown(ui);
      } else if (ev->key == CCORD_KEY_RETURN && is_multi) {
        const char *vals[CCORD_RICH_MAX_SELECT_OPTIONS];
        size_t nvals = 0;
        for (size_t i = 0; i < ui->select_opt_count; i++) {
          if (ui->select_opts[i].selected) vals[nvals++] = ui->select_opts[i].value;
        }
        submit_select_dropdown(ui, vals, nvals);
      }
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
      if (ui->select_dropdown_rect.w <= 0.0f || !ui_in_rect(ev->x, ev->y, ui->select_dropdown_rect)) {
        ui_close_select_dropdown(ui);
        return true;
      }
      if (is_multi && ui->select_confirm_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->select_confirm_btn)) {
        const char *vals[CCORD_RICH_MAX_SELECT_OPTIONS];
        size_t nvals = 0;
        for (size_t i = 0; i < ui->select_opt_count; i++) {
          if (ui->select_opts[i].selected) vals[nvals++] = ui->select_opts[i].value;
        }
        submit_select_dropdown(ui, vals, nvals);
        return true;
      }
      for (size_t i = 0; i < ui->select_opt_count; i++) {
        if (ui_in_rect(ev->x, ev->y, ui->select_opt_rects[i])) {
          if (is_multi) {
            if (!ui->select_opts[i].selected) {
              size_t cur = 0;
              for (size_t k = 0; k < ui->select_opt_count; k++) {
                if (ui->select_opts[k].selected) cur++;
              }
              if (cur >= (size_t)ui->select_target->max_values) {
                ui_notice(ui, "You can select at most %u options", (unsigned)ui->select_target->max_values);
                return true;
              }
            }
            ui->select_opts[i].selected = !ui->select_opts[i].selected;
            ui->dirty = true;
          } else {
            if (ui->select_opts[i].selected && ui->select_target->min_values == 0u) {
              ui->select_opts[i].selected = false;
              submit_select_dropdown(ui, nullptr, 0);
            } else {
              for (size_t k = 0; k < ui->select_opt_count; k++) ui->select_opts[k].selected = (k == i);
              const char *v = ui->select_opts[i].value;
              submit_select_dropdown(ui, &v, 1u);
            }
          }
          return true;
        }
      }
      return true;
    }
    return true;
  }

  if (ui->modal_kind == UI_MODAL_INTERACTION) {
    if (!ui->inter_modal || !ui->inter_modal_edits) {
      ui->modal_kind = UI_MODAL_NONE;
      ui->dirty = true;
      return true;
    }
    bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
    bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
    size_t nf = ui->inter_modal->field_count;
    size_t focus = nf > 0 && ui->inter_modal_focus < nf ? ui->inter_modal_focus : 0;
    if (ev->type == CCORD_IN_WHEEL) {
      ui->inter_modal_scroll = clampf(ui->inter_modal_scroll - ev->wheel * 36.0f, 0.0f, ui->inter_modal_max);
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_ESCAPE) {
        ui->modal_kind = UI_MODAL_NONE;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_TAB && nf > 0) {
        ui->inter_modal_focus = shift ? (focus + nf - 1u) % nf : (focus + 1u) % nf;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_RETURN) {
        if (nf > 0 && ui->inter_modal->fields[focus].style == 2 && !ctrl) {
          ccord_error ierr;
          (void)ccord_text_edit_insert(&ui->inter_modal_edits[focus], "\n", 1u, &ierr);
          ui->dirty = true;
        } else {
          submit_interaction_modal(ui);
        }
        return true;
      }
      if (nf > 0) {
        ccord_text_edit *ed = &ui->inter_modal_edits[focus];
        switch (ev->key) {
          case CCORD_KEY_BACKSPACE:
            if (ctrl) ccord_text_edit_backspace_word(ed);
            else ccord_text_edit_backspace(ed);
            break;
          case CCORD_KEY_DELETE:
            if (ctrl) ccord_text_edit_delete_word(ed);
            else ccord_text_edit_delete(ed);
            break;
          case CCORD_KEY_LEFT: ccord_text_edit_move(ed, ctrl ? -3 : -1, shift); break;
          case CCORD_KEY_RIGHT: ccord_text_edit_move(ed, ctrl ? 3 : 1, shift); break;
          case CCORD_KEY_HOME: ccord_text_edit_move(ed, -2, shift); break;
          case CCORD_KEY_END: ccord_text_edit_move(ed, 2, shift); break;
          case CCORD_KEY_A: if (ctrl) ccord_text_edit_select_all(ed); break;
          case CCORD_KEY_V:
            if (ctrl && ui->d.host.clipboard_get) {
              char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
              if (clip) {
                ccord_error perr;
                (void)ccord_text_edit_insert(ed, clip, strlen(clip), &perr);
                ccord_free(clip);
              }
            }
            break;
          default: break;
        }
        ui->dirty = true;
      }
      return true;
    }
    if (ev->type == CCORD_IN_TEXT && ev->text[0] != '\0' && nf > 0) {
      ccord_error ierr;
      (void)ccord_text_edit_insert(&ui->inter_modal_edits[focus], ev->text, strlen(ev->text), &ierr);
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
      if (ui->inter_modal_box.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->inter_modal_box)) {
        ui->modal_kind = UI_MODAL_NONE;
        ui->dirty = true;
        return true;
      }
      if (ui->inter_modal_cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inter_modal_cancel)) {
        ui->modal_kind = UI_MODAL_NONE;
        ui->dirty = true;
        return true;
      }
      if (ui->inter_modal_ok.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inter_modal_ok)) {
        submit_interaction_modal(ui);
        return true;
      }
      for (size_t i = 0; i < nf; i++) {
        if (ui->inter_modal_field_rects[i].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inter_modal_field_rects[i])) {
          ui->inter_modal_focus = i;
          ui->dirty = true;
          return true;
        }
      }
      return true;
    }
    return true;
  }

  if (ui->modal_kind != UI_MODAL_NONE) {
    bool two_field = (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS ||
                      ui->modal_kind == UI_MODAL_NEW_DM);
    ccord_text_edit *ed = (two_field && ui->modal_focus_field == 1)
                            ? &ui->modal_edit2
                            : &ui->modal_edit1;
    bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
    bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
    if (ev->type == CCORD_IN_WHEEL && ui->modal_kind == UI_MODAL_USER_PROFILE) {
      float step = 42.0f;
      ccord_scroller_scroll(&ui->profile_modal_glide, &ui->profile_modal_scroll, -ev->wheel * step,
                            ui->profile_modal_max, ui_wheel_direct(ui, ev));
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_ESCAPE) {
        if (ui->modal_kind == UI_MODAL_ADD_SERVER && ui->d.session) {
          ccord_session_clear_invite_preview(ui->d.session);
        }
        ui->modal_kind = UI_MODAL_NONE;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_TAB && two_field) {
        ui->modal_focus_field = ui->modal_focus_field == 1 ? 0 : 1;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_RETURN) {
        submit_modal(ui);
        return true;
      }
      if (ui->modal_kind == UI_MODAL_CONFIRM && (ev->key == CCORD_KEY_LEFT || ev->key == CCORD_KEY_RIGHT)) {
        size_t n = 0;
        (void)dc_confirm_choices(ui->confirm_op, &n);
        if (n > 0) {
          int next = ui->modal_type_sel + (ev->key == CCORD_KEY_RIGHT ? 1 : -1);
          if (next >= 0 && (size_t)next < n) ui->modal_type_sel = next;
          ui->dirty = true;
        }
        return true;
      }
      switch (ev->key) {
        case CCORD_KEY_BACKSPACE:
          if (ctrl) ccord_text_edit_backspace_word(ed);
          else ccord_text_edit_backspace(ed);
          break;
        case CCORD_KEY_DELETE:
          if (ctrl) ccord_text_edit_delete_word(ed);
          else ccord_text_edit_delete(ed);
          break;
        case CCORD_KEY_LEFT: ccord_text_edit_move(ed, ctrl ? -3 : -1, shift); break;
        case CCORD_KEY_RIGHT: ccord_text_edit_move(ed, ctrl ? 3 : 1, shift); break;
        case CCORD_KEY_HOME: ccord_text_edit_move(ed, -2, shift); break;
        case CCORD_KEY_END: ccord_text_edit_move(ed, 2, shift); break;
        case CCORD_KEY_A: if (ctrl) ccord_text_edit_select_all(ed); break;
        case CCORD_KEY_V:
          if (ctrl && ui->d.host.clipboard_get) {
            char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
            if (clip) {
              ccord_error perr;
              (void)ccord_text_edit_insert(ed, clip, strlen(clip), &perr);
              ccord_free(clip);
            }
          }
          break;
        default: break;
      }
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_TEXT && ev->text[0] != '\0') {
      ccord_error err;
      (void)ccord_text_edit_insert(ed, ev->text, strlen(ev->text), &err);
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
      if (ui->modal_kind == UI_MODAL_USER_PROFILE) {
        if (ui->profile_modal_box.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->profile_modal_box)) {
          ui->modal_kind = UI_MODAL_NONE;
          ui->touch_text_focused = false;
          ui->dirty = true;
          return true;
        }
        if (ui->profile_modal_close_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->profile_modal_close_rect)) {
          ui->modal_kind = UI_MODAL_NONE;
          ui->touch_text_focused = false;
          ui->dirty = true;
          return true;
        }
        if (ui->profile_modal_msg_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->profile_modal_msg_rect)) {
          ccord_id uid = ui->modal_target_id;
          ui->modal_kind = UI_MODAL_NONE;
          ui->touch_text_focused = false;
          if (ui->mobile_layout) {
            ui->left_drawer_open = false;
            ui->right_drawer_open = false;
          }
          ui_open_dm_with_user(ui, uid);
          ui->dirty = true;
          return true;
        }
        for (int t = 0; t < 3; t++) {
          if (ui->profile_modal_tab_rects[t].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->profile_modal_tab_rects[t])) {
            if (ui->touch_text_focused) {
              ui->touch_text_focused = false;
              if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
            }
            if (ui->profile_modal_tab != t) {
              ui->profile_modal_tab = t;
              ui->profile_modal_scroll = 0.0f;
              ui->dirty = true;
            }
            return true;
          }
        }
        if (ui->profile_modal_tab == 0) {
          if (ui->profile_modal_note_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->profile_modal_note_rect)) {
            if (ui->touch_mode) {
              ui->touch_text_focused = true;
              ui->dirty = true;
            }
            return true;
          }
          if (ui->profile_modal_note_save_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->profile_modal_note_save_rect)) {
            if (ui->touch_text_focused) {
              ui->touch_text_focused = false;
              if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
            }
            save_profile_modal_note(ui);
            return true;
          }
          if (ui->touch_text_focused) {
            ui->touch_text_focused = false;
            if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
            ui->dirty = true;
          }
          for (size_t ci = 0; ci < ui->nprofile_modal_conn_hits; ci++) {
            if (ui_in_rect(ev->x, ev->y, ui->profile_modal_conn_hits[ci].rect) &&
                ui->profile_modal_conn_hits[ci].url[0] != '\0') {
              (void)ui_open_url(ui, ui->profile_modal_conn_hits[ci].url, "Connection");
              return true;
            }
          }
        } else if (ui->profile_modal_tab == 1) {
          for (size_t i = 0; i < ui->nprofile_modal_mutual_hits; i++) {
            if (ui_in_rect(ev->x, ev->y, ui->profile_modal_mutual_hits[i].rect)) {
              ccord_id mgid = ui->profile_modal_mutual_hits[i].id;
              ui->modal_kind = UI_MODAL_NONE;
              ui->touch_text_focused = false;
              ui->discovery_open = false;
              ui->dm_friends_view = false;
              ui->dm_requests_view = false;
              ui->stage_channel = CCORD_ID_NONE;
              if (ui->mobile_layout) {
                ui->left_drawer_open = false;
                ui->right_drawer_open = false;
              }
              ui_compute_layout(ui);
              ccord_error s_err;
              if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, mgid, &s_err);
              else (void)ccord_store_select_guild(ui->d.store, mgid, &s_err);
              ui_ensure_caches(ui);
              ui_sync_messages(ui);
              ui_save_current_location(ui);
              ui->dirty = true;
              return true;
            }
          }
        } else if (ui->profile_modal_tab == 2) {
          for (size_t i = 0; i < ui->nprofile_modal_mutual_hits; i++) {
            if (ui_in_rect(ev->x, ev->y, ui->profile_modal_mutual_hits[i].rect)) {
              ccord_id fuid = ui->profile_modal_mutual_hits[i].id;
              ui->modal_kind = UI_MODAL_NONE;
              ui->touch_text_focused = false;
              if (ui->mobile_layout) {
                ui->left_drawer_open = false;
                ui->right_drawer_open = false;
              }
              ui_open_dm_with_user(ui, fuid);
              ui->dirty = true;
              return true;
            }
          }
        }
        return true;
      }
      if (ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT) {
        if (ui->inter_modal_box.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->inter_modal_box)) {
          ui->modal_kind = UI_MODAL_NONE;
          ui->dirty = true;
          return true;
        }
        if (ui->inter_modal_cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inter_modal_cancel)) {
          ui->modal_kind = UI_MODAL_NONE;
          ui->dirty = true;
          return true;
        }
        if (ui->modal_ok_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->modal_ok_rect)) {
          submit_modal(ui);
          return true;
        }
        if (ui->modal_input_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->modal_input_rect)) {
          ui->modal_focus_field = 0;
          ui->dirty = true;
          return true;
        }
        if (ui->modal_kind == UI_MODAL_NEW_DM && ui->modal_input2_rect.w > 0.0f &&
            ui_in_rect(ev->x, ev->y, ui->modal_input2_rect)) {
          ui->modal_focus_field = 1;
          ui->dirty = true;
          return true;
        }
        const ccord_channel *ech = (ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT)
                                     ? ccord_store_channel(ui->d.store, ui->modal_target_id)
                                     : nullptr;
        size_t existing = (ech && ech->recipient_count <= CCORD_MAX_DM_RECIPIENTS) ? ech->recipient_count : 0u;
        size_t max_add = existing < CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX ? (CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX - existing) : 0u;
        for (size_t i = 0; i < ui->nnew_dm_friend_hits; i++) {
          if (ui_in_rect(ev->x, ev->y, ui->new_dm_friend_hits[i].rect)) {
            dc_new_dm_toggle_recipient(ui, ui->new_dm_friend_hits[i].user_id, max_add);
            return true;
          }
        }
        return true;
      }
      if (ui->modal_kind == UI_MODAL_EDIT_GROUP_DM) {
        if (ui->inter_modal_box.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->inter_modal_box)) {
          ui->modal_kind = UI_MODAL_NONE;
          ui->dirty = true;
          return true;
        }
        if (ui->inter_modal_cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inter_modal_cancel)) {
          ui->modal_kind = UI_MODAL_NONE;
          ui->dirty = true;
          return true;
        }
        if (ui->modal_ok_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->modal_ok_rect)) {
          submit_modal(ui);
          return true;
        }
        if (ui->modal_tab_rects[0].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->modal_tab_rects[0])) {
          ui->modal_type_sel = (ui->modal_type_sel == 1) ? 0 : 1;
          ui->dirty = true;
          return true;
        }
        return true;
      }
      modal_geom mg = dc_modal_layout(ui);
      if (!ui_in_rect(ev->x, ev->y, mg.box)) {
        if (ui->modal_kind == UI_MODAL_ADD_SERVER && ui->d.session) {
          ccord_session_clear_invite_preview(ui->d.session);
        }
        ui->modal_kind = UI_MODAL_NONE;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, mg.ok)) {
        submit_modal(ui);
        return true;
      }
      if (mg.cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, mg.cancel)) {
        if (ui->modal_kind == UI_MODAL_ADD_SERVER && ui->d.session) {
          ccord_session_clear_invite_preview(ui->d.session);
        }
        ui->modal_kind = UI_MODAL_NONE;
        ui->dirty = true;
        return true;
      }
      if (ui->modal_kind == UI_MODAL_ADD_SERVER) {
        for (int t = 0; t < 2; t++) {
          if (ui_in_rect(ev->x, ev->y, mg.tabs[t])) {
            if (ui->modal_type_sel != t) {
              ui->modal_type_sel = t;
              ccord_text_edit_clear(&ui->modal_edit1);
              if (ui->d.session) ccord_session_clear_invite_preview(ui->d.session);
              ui->dirty = true;
            }
            return true;
          }
        }
        if (ui->modal_type_sel == 0 && mg.preview_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, mg.preview_btn)) {
          ccord_admin_request req;
          ccord_error err;
          if (ccord_admin_preview_invite(&req, ccord_text_edit_cstr(&ui->modal_edit1), &err) != CCORD_OK) {
            ui_notice(ui, "Invalid invite: %s", err.message);
          } else {
            ui_admin_submit(ui, &req);
          }
          ui->dirty = true;
          return true;
        }
        return true;
      }
      if (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS) {
        if (ui_in_rect(ev->x, ev->y, mg.input)) {
          ui->modal_focus_field = 0;
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, mg.input2)) {
          ui->modal_focus_field = 1;
          ui->dirty = true;
          return true;
        }
        for (size_t i = 0; i < mg.nchips; i++) {
          if (ui_in_rect(ev->x, ev->y, mg.chip[i])) {
            ui->modal_type_sel = (int)i;
            ccord_text_edit_clear(&ui->modal_edit2);
            if (dc_k_folder_color_presets[i] != 0) {
              char hex[16];
              ccord_error herr;
              int hn = snprintf(hex, sizeof hex, "#%06X", (unsigned)dc_k_folder_color_presets[i]);
              if (hn > 0) (void)ccord_text_edit_insert(&ui->modal_edit2, hex, (size_t)hn, &herr);
            }
            ui->dirty = true;
            return true;
          }
        }
        return true;
      }
      if (ui->modal_kind == UI_MODAL_CONFIRM) {
        if (mg.input.w > 0.0f && ui_in_rect(ev->x, ev->y, mg.input)) {
          if (ui->touch_mode) {
            ui->touch_text_focused = true;
            ui->dirty = true;
          }
          return true;
        }
        if (ui->touch_text_focused) {
          ui->touch_text_focused = false;
          if (ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
          ui->dirty = true;
        }
        for (size_t i = 0; i < mg.nchips; i++) {
          if (ui_in_rect(ev->x, ev->y, mg.chip[i])) {
            ui->modal_type_sel = (int)i;
            ui->dirty = true;
            break;
          }
        }
      }
      return true;
    }
    return true;
  }

  return false;
}
